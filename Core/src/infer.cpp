#include "optum/infer.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <optional>

#include "optum/ir.hpp"

namespace optum {
namespace {

using json = nlohmann::json;
using ojson = nlohmann::ordered_json;
using FieldMap = std::map<std::string, std::set<std::string>>;  // field path -> kinds seen

bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
bool is_alnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }

// An id-like path segment: digits, a uuid, a long hex id, or a Stripe-style prefix_id.
bool id_segment(std::string_view s) {
  if (s.empty()) return false;
  if (std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; })) return true;
  if (s.size() >= 36 && s[8] == '-' && std::all_of(s.begin(), s.begin() + 8, is_hex) &&
      std::all_of(s.begin() + 9, s.end(), [](char c) { return is_hex(c) || c == '-'; })) {
    return true;
  }
  if (s.size() >= 24 && std::all_of(s.begin(), s.end(), is_hex)) return true;
  const auto us = s.find('_');
  if (us >= 2 && us <= 4 && us != std::string_view::npos && s.size() - us - 1 >= 8 &&
      std::all_of(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(us), [](char c) { return c >= 'a' && c <= 'z'; }) &&
      std::all_of(s.begin() + static_cast<std::ptrdiff_t>(us) + 1, s.end(), is_alnum)) {
    return true;
  }
  return false;
}

// How a JSON value prints in the spec's text: strings as they are, null as None (the format the
// specs have always used).
std::string text_of(const json& v) {
  if (v.is_string()) return v.get<std::string>();
  if (v.is_null()) return "None";
  if (v.is_boolean()) return v.get<bool>() ? "True" : "False";
  return v.dump();
}

bool truthy(const json& v) {
  if (v.is_null()) return false;
  if (v.is_boolean()) return v.get<bool>();
  if (v.is_number()) return v != 0;
  if (v.is_string() || v.is_array() || v.is_object()) return !v.empty();
  return true;
}

const char* kind_of(const json& v) {
  if (v.is_null()) return "null";
  if (v.is_boolean()) return "bool";
  if (v.is_number()) return "number";
  if (v.is_string()) return "string";
  if (v.is_array()) return "list";
  return "object";
}

// Field paths and the kinds seen: {"billing.email": {"string"}, "line_items[].quantity": {"number"}}.
void fields_of(const json& value, const std::string& prefix, FieldMap& out, int depth = 0) {
  if (depth > 3) return;
  if (value.is_object()) {
    for (const auto& [k, v] : value.items()) {
      const std::string path = prefix + k;
      out[path].insert(kind_of(v));
      if (v.is_object()) fields_of(v, path + ".", out, depth + 1);
      else if (v.is_array() && !v.empty() && v[0].is_object()) fields_of(v[0], path + "[].", out, depth + 1);
    }
  } else if (value.is_array() && !value.empty() && value[0].is_object()) {
    fields_of(value[0], prefix, out, depth + 1);
  }
}

const json& member(const json& obj, const char* key) {
  static const json null_value;
  if (!obj.is_object()) return null_value;
  const auto it = obj.find(key);
  return it == obj.end() ? null_value : *it;
}

struct RpcCall {
  std::string op;
  std::optional<std::string> model;
  json values;  // null: none
};

// An Odoo-style JSON-RPC call: the operation, its model, and the values or domain it carries.
std::optional<RpcCall> rpc_call(const json& rec) {
  const json& body = member(member(rec, "request"), "body");
  const json& p = member(body, "params");
  if (!p.is_object()) return std::nullopt;
  static const json empty = json::array();
  const json& args = member(p, "args").is_array() ? member(p, "args") : empty;
  const json& service = member(p, "service");
  const json& method = member(p, "method");
  if (service == "object" && (method == "execute_kw" || method == "execute") && args.size() >= 5) {
    RpcCall c{"rpc " + text_of(args[3]) + "." + text_of(args[4]), text_of(args[3]), json()};
    const json rest = args.size() > 5 ? args[5] : json::array();
    if (args[4] == "create" && truthy(rest) && rest.is_array()) {
      if (rest[0].is_object() || rest[0].is_array()) c.values = rest[0];
    } else if (args[4] == "write" && rest.is_array() && rest.size() > 1) {
      c.values = rest[1];
    }
    return c;
  }
  if (service == "common" || service == "db") return RpcCall{"rpc " + text_of(service) + "." + text_of(method), std::nullopt, json()};
  return std::nullopt;
}

// The resource an endpoint is about: its last named segment (orders in /wc/v3/orders/{}).
std::string resource_of(const std::string& path) {
  std::string last;
  std::size_t start = 0;
  while (start <= path.size()) {
    const auto end = std::min(path.find('/', start), path.size());
    const std::string seg = path.substr(start, end - start);
    if (!seg.empty() && seg != "{}") last = seg;
    start = end + 1;
  }
  return last.empty() ? path : last;
}

// A counter that remembers first-seen order, so ties keep it (as most_common does).
struct Counter {
  std::vector<std::pair<std::string, long long>> items;
  void add(const std::string& k, long long n = 1) {
    for (auto& [key, v] : items) {
      if (key == k) { v += n; return; }
    }
    items.emplace_back(k, n);
  }
  std::vector<std::pair<std::string, long long>> most_common() const {
    auto out = items;
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    return out;
  }
};

struct Op {
  long long calls = 0;
  Counter clients;
  std::map<long long, long long> statuses;
  json first, last;
  FieldMap request, response;
  std::set<std::string> query;
  json resource;
  std::string endpoint;               // method and templated path, also for RPC operations
  std::set<std::string> status_sent;  // the status values it sets: what a write does to a record
};

ojson fmap(const FieldMap& m) {
  ojson out = ojson::object();
  for (const auto& [k, v] : m) out[k] = std::vector<std::string>(v.begin(), v.end());
  return out;
}

std::string started_of(const json& r) {
  const json& s = member(r, "started");
  return s.is_string() ? s.get<std::string>() : "";
}

}  // namespace

std::string template_path(std::string_view path) {
  std::string out;
  std::size_t start = 0;
  while (true) {
    const auto end = path.find('/', start);
    const std::string_view seg = path.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    out += id_segment(seg) ? "{}" : std::string(seg);
    if (end == std::string_view::npos) break;
    out += '/';
    start = end + 1;
  }
  return out;
}

ojson learn_spec(const std::vector<json>& records) {
  std::vector<const json*> recs;
  for (const auto& r : records) recs.push_back(&r);
  std::stable_sort(recs.begin(), recs.end(), [](const json* a, const json* b) { return started_of(*a) < started_of(*b); });

  std::vector<std::pair<std::string, Op>> ops;  // first-seen order
  std::map<std::string, FieldMap> resources;
  // resource -> record id -> statuses in order (first-seen order at both levels)
  std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::vector<std::string>>>>> states;

  for (const json* rp : recs) {
    const json& r = *rp;
    const auto call = rpc_call(r);
    std::string op;
    json values;
    json resource;
    const std::string endpoint = r.at("method").get<std::string>() + " " + template_path(r.at("path").get<std::string>());
    if (call) {
      op = call->op;
      if (call->model) resource = *call->model;
      values = call->values;
    } else {
      const std::string path = template_path(r.at("path").get<std::string>());
      const std::string method = r.at("method").get<std::string>();
      op = method + " " + path;
      resource = resource_of(path);
      if (method == "POST" || method == "PUT" || method == "PATCH") values = member(r.at("request"), "body");
    }
    auto it = std::find_if(ops.begin(), ops.end(), [&](const auto& p) { return p.first == op; });
    if (it == ops.end()) {
      Op o;
      o.first = member(r, "started");
      o.resource = resource;
      o.endpoint = endpoint;
      ops.emplace_back(op, std::move(o));
      it = std::prev(ops.end());
    }
    Op& o = it->second;
    ++o.calls;
    const json& client = member(r, "client");
    o.clients.add(truthy(client) ? text_of(client) : "unknown");
    o.statuses[r.at("response").at("status").get<long long>()]++;
    o.last = member(r, "started");
    const json& query = member(r, "query");
    if (query.is_object()) {
      for (const auto& [k, v] : query.items()) {
        (void)v;
        o.query.insert(k);
      }
    }
    const json& sent_status = member(member(r.at("request"), "body"), "status");
    if (!call && sent_status.is_string()) o.status_sent.insert(sent_status.get<std::string>());
    const bool has_resource = resource.is_string() && !resource.get<std::string>().empty();
    const std::string res = has_resource ? resource.get<std::string>() : "";
    if (!values.is_null()) {
      fields_of(values, "", o.request);
      if (has_resource) {
        FieldMap f;
        fields_of(values, "", f);
        for (const auto& [k, kinds] : f) resources[res][k].insert(kinds.begin(), kinds.end());
      }
    }
    json resp = member(r.at("response"), "body");
    if (call && resp.is_object() && resp.contains("result")) resp = resp["result"];
    if (resp.is_object() || resp.is_array()) {
      fields_of(resp, "", o.response);
      if (has_resource && !call) {
        FieldMap f;
        fields_of(resp, "", f);
        for (const auto& [k, kinds] : f) resources[res][k].insert(kinds.begin(), kinds.end());
      }
    }
    // Status changes: a record's status as it was sent or returned, over time.
    if (!call) {
      const json rid = resp.is_object() && resp.contains("id") ? resp["id"] : json();
      const json& body = member(r.at("request"), "body");
      const json& sent = member(body, "status");
      const json status = truthy(sent) ? sent : (resp.is_object() ? member(resp, "status") : json());
      if (!rid.is_null() && status.is_string()) {
        auto sit = std::find_if(states.begin(), states.end(), [&](const auto& p) { return p.first == res; });
        if (sit == states.end()) {
          states.emplace_back(res, std::vector<std::pair<std::string, std::vector<std::string>>>{});
          sit = std::prev(states.end());
        }
        const std::string key = text_of(rid);
        auto iit = std::find_if(sit->second.begin(), sit->second.end(), [&](const auto& p) { return p.first == key; });
        if (iit == sit->second.end()) {
          sit->second.emplace_back(key, std::vector<std::string>{});
          iit = std::prev(sit->second.end());
        }
        const std::string st = status.get<std::string>();
        if (iit->second.empty() || iit->second.back() != st) iit->second.push_back(st);
      }
    }
  }

  ojson spec = ojson::object();
  spec["spec_version"] = "0.1";
  std::vector<const std::pair<std::string, Op>*> order;
  for (const auto& p : ops) order.push_back(&p);
  std::stable_sort(order.begin(), order.end(), [](const auto* a, const auto* b) { return a->second.calls > b->second.calls; });
  ojson operations = ojson::object();
  for (const auto* p : order) {
    const Op& o = p->second;
    ojson clients = ojson::object(), statuses = ojson::object();
    for (const auto& [c, n] : o.clients.most_common()) clients[c] = n;
    for (const auto& [s, n] : o.statuses) statuses[std::to_string(s)] = n;
    ojson e = ojson::object();
    e["calls"] = o.calls;
    e["clients"] = clients;
    e["statuses"] = statuses;
    e["first_seen"] = ojson::parse(o.first.dump());
    e["last_seen"] = ojson::parse(o.last.dump());
    e["resource"] = ojson::parse(o.resource.dump());
    e["query"] = std::vector<std::string>(o.query.begin(), o.query.end());
    e["endpoint"] = o.endpoint;
    e["status_sent"] = std::vector<std::string>(o.status_sent.begin(), o.status_sent.end());
    e["request_fields"] = fmap(o.request);
    e["response_fields"] = fmap(o.response);
    operations[p->first] = e;
  }
  spec["operations"] = operations;
  ojson res = ojson::object();
  for (const auto& [name, f] : resources) res[name] = fmap(f);
  spec["resources"] = res;
  ojson transitions = ojson::object();
  for (const auto& [name, by_id] : states) {
    Counter t;
    for (const auto& [id, seq] : by_id) {
      (void)id;
      for (std::size_t i = 1; i < seq.size(); ++i) t.add(seq[i - 1] + " -> " + seq[i]);
    }
    if (t.items.empty()) continue;
    ojson m = ojson::object();
    for (const auto& [k, n] : t.most_common()) m[k] = n;
    transitions[name] = m;
  }
  spec["status_changes"] = transitions;
  return spec;
}

namespace {

struct Webhook {
  std::string flow, name, method, path;
};

// Every webhook-triggered flow in a folder of exported automation flows.
std::vector<Webhook> n8n_webhooks(const std::filesystem::path& folder) {
  std::vector<std::filesystem::path> files;
  for (const auto& e : std::filesystem::directory_iterator(folder)) {
    if (e.is_regular_file() && e.path().extension() == ".json") files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());
  std::vector<Webhook> out;
  for (const auto& f : files) {
    std::ifstream in(f);
    json wf;
    try {
      wf = json::parse(in);
    } catch (const json::exception&) {
      continue;
    }
    if (!wf.is_object()) continue;
    for (const auto& n : member(wf, "nodes").is_array() ? member(wf, "nodes") : json::array()) {
      const json& type = member(n, "type");
      if (!type.is_string() || !type.get<std::string>().ends_with(".webhook")) continue;
      const json& p = member(n, "parameters");
      const json& id = member(wf, "id");
      const json& name = member(wf, "name");
      const json& method = member(p, "httpMethod");
      const json& path = member(p, "path");
      out.push_back({truthy(id) ? text_of(id) : f.filename().string(), name.is_null() ? "" : text_of(name),
                     method.is_null() ? "GET" : text_of(method), path.is_null() ? "" : text_of(path)});
    }
  }
  return out;
}

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) out += (i ? sep : "") + parts[i];
  return out;
}

}  // namespace

std::map<std::string, std::vector<json>> load_recordings(const InferOptions& opts, std::size_t* harness) {
  if (!std::filesystem::is_directory(opts.recordings)) {
    throw IrError(opts.recordings.string() + ": not a folder of recordings");
  }
  std::vector<std::filesystem::path> files;
  for (const auto& e : std::filesystem::directory_iterator(opts.recordings)) {
    if (e.is_regular_file() && e.path().extension() == ".jsonl") files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());

  std::map<std::string, std::vector<json>> by_tool;
  for (const auto& f : files) {
    std::ifstream in(f);
    if (!in) throw IrError(f.string() + ": cannot open");
    std::string line;
    std::size_t lineno = 0;
    while (std::getline(in, line)) {
      ++lineno;
      if (line.find_first_not_of(" \t\r\n") == std::string::npos) continue;
      json r;
      try {
        r = json::parse(line);
      } catch (const json::exception& e) {
        throw IrError(f.string() + ":" + std::to_string(lineno) + ": invalid JSON: " + e.what());
      }
      if (!r.is_object() || !member(r, "tool").is_string()) {
        throw IrError(f.string() + ":" + std::to_string(lineno) + ": a recording needs a \"tool\"");
      }
      if (truthy(member(r, "harness"))) {  // Optum's own loading and checking tools: not the company's behaviour
        if (harness) ++*harness;
        continue;
      }
      const std::string tool = r["tool"].get<std::string>();
      if (!opts.tools.empty() && !opts.tools.count(tool)) continue;
      if (!opts.clients.empty() && !opts.clients.count(text_of(member(r, "client")))) continue;
      by_tool[tool].push_back(std::move(r));
    }
  }
  return by_tool;
}

InferResult infer(const InferOptions& opts) {
  InferResult result;
  const auto by_tool = load_recordings(opts, &result.harness);

  std::vector<std::string> times;
  for (const auto& [tool, recs] : by_tool) {
    (void)tool;
    result.calls += recs.size();
    for (const auto& r : recs) {
      if (truthy(member(r, "started"))) times.push_back(text_of(r["started"]));
    }
  }
  std::sort(times.begin(), times.end());
  const ojson window = times.empty() ? ojson::array({nullptr, nullptr}) : ojson::array({times.front(), times.back()});
  const std::string from = times.empty() ? "None" : times.front(), to = times.empty() ? "None" : times.back();

  std::vector<std::string> md = {"# Behaviour specs, learned from recorded traffic", ""};
  std::vector<std::string> counts;
  for (const auto& [tool, recs] : by_tool) counts.push_back(tool + " " + std::to_string(recs.size()) + " calls");
  md.push_back("Recorded from " + from + " to " + to + ": " + join(counts, ", ") + ". Left out: " +
               std::to_string(result.harness) + " calls from Optum's own harness (loading, checks).");
  md.push_back("");
  for (const auto& [tool, recs] : by_tool) {
    try {
      ojson spec = learn_spec(recs);
      spec["tool"] = tool;
      md.insert(md.end(), {"## " + tool, "", "| Operation | Calls | By | Statuses |", "|---|---|---|---|"});
      for (const auto& [op, o] : spec["operations"].items()) {
        std::vector<std::string> by, st;
        for (const auto& [c, n] : o["clients"].items()) by.push_back(c + " (" + n.dump() + ")");
        for (const auto& [s, n] : o["statuses"].items()) st.push_back(s + ": " + n.dump());
        md.push_back("| `" + op + "` | " + o["calls"].dump() + " | " + join(by, ", ") + " | " + join(st, ", ") + " |");
      }
      if (!spec["status_changes"].empty()) {
        std::vector<std::string> parts;
        for (const auto& [res, ts] : spec["status_changes"].items()) {
          std::vector<std::string> t;
          for (const auto& [k, n] : ts.items()) t.push_back(k + " (" + n.dump() + ")");
          parts.push_back(res + ": " + join(t, ", "));
        }
        md.push_back("");
        md.push_back("Status changes seen: " + join(parts, "; "));
      }
      md.push_back("");
      result.specs.emplace_back(tool, std::move(spec));
    } catch (const json::exception& e) {
      throw IrError("recordings of " + tool + ": a recording is missing a field or has one of the wrong type (" +
                    e.what() + ")");
    }
  }

  ojson findings = ojson::array();
  if (!opts.n8n.empty()) {
    const auto it = by_tool.find("automation");
    const std::size_t automation_calls = it == by_tool.end() ? 0 : it->second.size();
    std::map<std::string, std::size_t> hits;
    if (it != by_tool.end()) {
      for (const auto& r : it->second) hits[template_path(text_of(member(r, "path")))]++;
    }
    for (const auto& w : n8n_webhooks(opts.n8n)) {
      const std::size_t seen = (hits.count("/webhook/" + w.path) ? hits["/webhook/" + w.path] : 0) +
                               (hits.count("/webhook-test/" + w.path) ? hits["/webhook-test/" + w.path] : 0);
      if (seen) continue;
      ojson f = ojson::object();
      f["kind"] = "never_triggered";
      f["flow"] = "n8n:" + w.flow;
      f["name"] = w.name;
      f["trigger"] = "webhook " + w.method + " /webhook/" + w.path;
      f["calls_seen"] = 0;
      f["window"] = window;
      f["evidence"] = "no call to /webhook/" + w.path + " in " + std::to_string(automation_calls) +
                      " recorded calls to the automation tool";
      findings.push_back(f);
    }
  }
  result.observed = ojson::object();
  result.observed["window"] = window;
  result.observed["findings"] = findings;
  if (!findings.empty()) {
    md.insert(md.end(), {"## Observed findings", ""});
    for (const auto& f : findings) {
      md.push_back("- **" + f["name"].get<std::string>() + "** (`" + f["flow"].get<std::string>() + "`) never ran: " +
                   f["evidence"].get<std::string>() + ", " + from + " to " + to + ".");
    }
  }
  result.markdown = join(md, "\n") + "\n";
  return result;
}

}  // namespace optum
