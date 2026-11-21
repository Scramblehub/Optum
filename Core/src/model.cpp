#include "optum/model.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

#include "optum/infer.hpp"

namespace optum {
namespace {

using json = nlohmann::json;
using ojson = nlohmann::ordered_json;
namespace fs = std::filesystem;

// --------------------------------------------------------------------------------- provenance

// Content hash for staleness (FNV-1a, 64 bits): detects change, not tampering.
std::string content_hash(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return "missing";
  std::uint64_t h = 1469598103934665603ULL;
  char buf[65536];
  while (in.read(buf, sizeof buf) || in.gcount() > 0) {
    for (std::streamsize i = 0; i < in.gcount(); ++i) {
      h ^= static_cast<unsigned char>(buf[i]);
      h *= 1099511628211ULL;
    }
  }
  std::ostringstream s;
  s << std::hex << std::setw(16) << std::setfill('0') << h;
  return s.str();
}

ojson inputs_json(const std::set<std::string>& paths) {
  ojson out = ojson::array();
  for (const auto& p : paths) out.push_back({{"path", p}, {"hash", content_hash(p)}});
  return out;
}

// --------------------------------------------------------------------------------- deployment

struct Deployment {
  std::map<std::string, std::vector<std::string>> hosts;     // system -> host[:port]
  std::map<std::string, std::set<std::string>> regulated;    // system -> resources
  std::map<std::string, std::vector<std::string>> runs;      // client -> unit patterns
  std::map<std::string, std::string> actors;                 // client -> who (outside the company's code)
};

std::vector<std::string> strings(const json& v, const std::string& where) {
  if (!v.is_array()) throw IrError("deployment: " + where + " must be a list of strings");
  std::vector<std::string> out;
  for (const auto& x : v) {
    if (!x.is_string()) throw IrError("deployment: " + where + " must be a list of strings");
    out.push_back(x.get<std::string>());
  }
  return out;
}

Deployment load_deployment(const fs::path& p) {
  std::ifstream in(p);
  if (!in) throw IrError(p.string() + ": cannot open");
  json j;
  try {
    j = json::parse(in);
  } catch (const json::exception& e) {
    throw IrError(p.string() + ": invalid JSON: " + e.what());
  }
  Deployment d;
  if (j.contains("systems")) {
    for (const auto& [name, s] : j["systems"].items()) {
      d.hosts[name] = strings(s.value("hosts", json::array()), "systems." + name + ".hosts");
      for (const auto& r : strings(s.value("regulated", json::array()), "systems." + name + ".regulated")) {
        d.regulated[name].insert(r);
      }
    }
  }
  if (j.contains("runs")) {
    for (const auto& [client, units] : j["runs"].items()) d.runs[client] = strings(units, "runs." + client);
  }
  if (j.contains("actors")) {
    for (const auto& [client, who] : j["actors"].items()) d.actors[client] = who.is_string() ? who.get<std::string>() : "";
  }
  return d;
}

bool matches(const std::string& pattern, const std::string& unit) {
  if (!pattern.empty() && pattern.back() == '*') return unit.starts_with(pattern.substr(0, pattern.size() - 1));
  return pattern == unit;
}

// --------------------------------------------------------------------------------- operations

struct OpUse {
  std::string system, op, endpoint, access, resource;
  std::vector<std::string> status_sent;
  long long calls = 0;
  std::set<std::string> evidence;  // static, recorded
  std::set<std::string> functions, clients, request_fields;
  bool static_exact = true;
  bool shared_client = false;  // its traffic came from a client that runs several components

  std::string key() const {
    std::string k = system + " | " + op;
    for (const auto& s : status_sent) k += " | status " + s;
    return k;
  }
};

std::string access_of(const std::string& op, const std::string& method) {
  if (op.starts_with("rpc ")) {
    static const std::set<std::string> reads = {"search", "search_read", "search_count", "read", "fields_get",
                                                "name_search", "read_group", "check_access_rights"};
    if (op.starts_with("rpc common.") || op.starts_with("rpc db.")) return "read";
    const auto dot = op.rfind('.');
    return reads.count(op.substr(dot + 1)) ? "read" : "write";
  }
  if (method.empty()) return "unknown";
  return method == "GET" || method == "HEAD" || method == "OPTIONS" ? "read" : "write";
}

struct Component {
  std::string id, frontend, trigger;
  std::map<std::string, OpUse> ops;                 // by key
  std::set<std::string> static_endpoints;           // "system|METHOD|path"
  std::set<std::string> observed_endpoints;
  std::set<std::string> observed_paths;             // "system|path": for static uses whose method isn't known
  std::set<std::string> files, dependents, recordings;
  long long calls = 0;
  bool never_ran = false;
  bool uses_declared = false;  // traffic reached it through the deployment's runs (declared)
};

std::string ep_key(const std::string& system, const std::string& method, const std::string& path) {
  return system + "|" + method + "|" + path;
}

// The unit a node belongs to: the longest unit id that prefixes it at a separator.
std::string unit_of(const std::string& node, const std::vector<std::string>& units) {
  std::string best;
  for (const auto& u : units) {
    if (u.size() > best.size() && node.starts_with(u) &&
        (node.size() == u.size() || std::string("./#:").find(node[u.size()]) != std::string::npos)) {
      best = u;
    }
  }
  return best;
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
  std::string out;
  for (std::size_t i = 0; i < v.size(); ++i) out += (i ? sep : "") + v[i];
  return out;
}

std::vector<std::string> sorted(const std::set<std::string>& s) { return {s.begin(), s.end()}; }

// Weakest evidence wins: a conclusion is never more certain than what it needs.
std::string weakest(const std::set<std::string>& kinds) {
  for (const char* k : {"inferred", "declared", "recorded", "exact"}) {
    if (kinds.count(k)) return k;
  }
  return "exact";
}

}  // namespace

ojson build_model(const ModelInputs& in) {
  if (!in.graph || !in.diagnosis) throw IrError("model: needs a graph and its diagnosis");
  const Deployment dep = load_deployment(in.deployment);
  const Graph& g = *in.graph;
  const Diagnosis& d = *in.diagnosis;

  std::map<std::string, Component> comps;
  std::vector<std::string> unit_ids;
  for (const auto& u : d.units) {
    comps[u.id].id = u.id;
    comps[u.id].frontend = u.frontend;
    unit_ids.push_back(u.id);
  }

  // Files, triggers and dependents, from the graph.
  for (const auto& n : g.nodes()) {
    const std::string u = unit_of(n.id, unit_ids);
    if (u.empty()) continue;
    if (n.loc && !n.loc->file.empty()) comps[u].files.insert((in.source_root / n.loc->file).string());
    if (n.attrs.is_object() && n.attrs.contains("trigger") && n.attrs["trigger"].is_string()) {
      comps[u].trigger = n.attrs["trigger"].get<std::string>();
    }
  }
  for (const auto& e : g.edges()) {
    const std::string from = unit_of(g.node(e.from).id, unit_ids), to = unit_of(g.node(e.to).id, unit_ids);
    if (!from.empty() && !to.empty() && from != to) comps[to].dependents.insert(from);
  }
  for (auto& [id, c] : comps) {
    if (c.trigger.empty()) c.trigger = "not in the code (started by a person or a scheduler outside it)";
  }

  // Static operations: each endpoint a component reaches, from diagnose's inventory.
  std::map<std::string, std::string> host_system;
  for (const auto& [sys, hosts] : dep.hosts) {
    for (const auto& h : hosts) host_system[h] = sys;
  }
  for (const auto& t : d.reached) {
    if (!t.id.starts_with("http:")) continue;  // tables and libraries: not operations on a system (yet)
    const std::string rest = t.id.substr(5);
    const auto slash = rest.find('/');
    const std::string host = rest.substr(0, slash);
    const std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    const auto sit = host_system.find(host);
    const std::string system = sit == host_system.end() ? host : sit->second;
    for (const auto& u : t.uses) {
      auto& c = comps[u.unit];
      c.id = u.unit;
      const std::string endpoint = (u.method.empty() ? "?" : u.method) + " " + path;
      OpUse op;
      op.system = system;
      op.op = endpoint;
      op.endpoint = endpoint;
      op.access = access_of(endpoint, u.method);
      op.evidence.insert("static");
      op.functions.insert(u.function);
      op.static_exact = u.exact;
      auto [it, fresh] = c.ops.emplace(op.key(), op);
      if (!fresh) {
        it->second.functions.insert(u.function);
        it->second.static_exact = it->second.static_exact && u.exact;
      }
      c.static_endpoints.insert(ep_key(system, u.method, path));
    }
  }

  // Observed operations: traffic per (system, client), attributed through the deployment's runs
  // and narrowed to the components whose code reaches that endpoint.
  struct Outside {
    std::string client, who, system, op, access;
    std::vector<std::string> status_sent;
    long long calls;
    std::string key;
  };
  std::vector<Outside> outside;
  ojson window = ojson::array({nullptr, nullptr});
  std::set<std::string> never_ran_flows;
  if (!in.recordings.empty()) {
    InferOptions io;
    io.recordings = in.recordings;
    io.n8n = in.n8n;
    const auto by_tool = load_recordings(io);
    const InferResult inferred = infer(io);
    window = inferred.observed["window"];
    for (const auto& f : inferred.observed["findings"]) never_ran_flows.insert(f["flow"].get<std::string>());
    for (const auto& [tool, recs] : by_tool) {
      std::map<std::string, std::vector<json>> by_client;
      for (const auto& r : recs) {
        const json& cl = r.contains("client") ? r["client"] : json();
        by_client[cl.is_string() ? cl.get<std::string>() : "unknown"].push_back(r);
      }
      const std::string rec_file = (in.recordings / (tool + ".jsonl")).string();
      for (const auto& [client, crecs] : by_client) {
        std::vector<std::string> candidates;
        if (const auto rit = dep.runs.find(client); rit != dep.runs.end()) {
          for (const auto& u : unit_ids) {
            for (const auto& p : rit->second) {
              if (matches(p, u)) {
                candidates.push_back(u);
                break;
              }
            }
          }
        }
        const ojson spec = learn_spec(crecs);
        for (const auto& [opname, o] : spec["operations"].items()) {
          const std::string endpoint = o["endpoint"].get<std::string>();
          const std::string method = endpoint.substr(0, endpoint.find(' '));
          const std::string path = endpoint.substr(endpoint.find(' ') + 1);
          OpUse op;
          op.system = tool;
          op.op = opname;
          op.endpoint = endpoint;
          op.access = access_of(opname, method);
          op.resource = o["resource"].is_string() ? o["resource"].get<std::string>() : "";
          for (const auto& s : o["status_sent"]) op.status_sent.push_back(s.get<std::string>());
          op.calls = o["calls"].get<long long>();
          op.clients.insert(client);
          for (const auto& [f, kinds] : o["request_fields"].items()) {
            (void)kinds;
            op.request_fields.insert(f);
          }
          op.evidence.insert("recorded");
          std::vector<std::string> reach;
          for (const auto& u : candidates) {
            const auto& se = comps[u].static_endpoints;
            if (se.count(ep_key(tool, method, path)) || se.count(ep_key(tool, "", path))) reach.push_back(u);
          }
          if (reach.empty()) {
            const auto ait = dep.actors.find(client);
            outside.push_back({client, ait == dep.actors.end() ? "" : ait->second, tool, opname, op.access,
                               op.status_sent, op.calls, op.key()});
            continue;
          }
          op.shared_client = reach.size() > 1;
          for (const auto& u : reach) {
            auto& c = comps[u];
            c.uses_declared = true;
            c.recordings.insert(rec_file);
            c.observed_endpoints.insert(ep_key(tool, method, path));
            c.observed_paths.insert(tool + "|" + path);
            c.calls += op.calls;
            auto [it, fresh] = c.ops.emplace(op.key(), op);
            if (!fresh) {  // the same operation, known from the code or from another client
              OpUse& known = it->second;
              known.calls += op.calls;
              known.clients.insert(client);
              known.evidence.insert("recorded");
              known.request_fields.insert(op.request_fields.begin(), op.request_fields.end());
              if (known.resource.empty()) known.resource = op.resource;
              known.shared_client = known.shared_client || op.shared_client;
            }
          }
        }
      }
    }
  }
  // An endpoint seen in traffic is described by what was observed on it; the static placeholder
  // (POST /jsonrpc) hands its functions to the observed operations and goes.
  for (auto& [id, c] : comps) {
    for (auto it = c.ops.begin(); it != c.ops.end();) {
      const OpUse& s = it->second;
      if (s.evidence.count("recorded") || s.evidence.size() != 1) {
        ++it;
        continue;
      }
      const std::string method = s.endpoint.substr(0, s.endpoint.find(' '));
      const std::string path = s.endpoint.substr(s.endpoint.find(' ') + 1);
      const bool seen = method == "?" ? c.observed_paths.count(s.system + "|" + path) > 0
                                      : c.observed_endpoints.count(ep_key(s.system, method, path)) > 0;
      if (!seen) {
        ++it;
        continue;
      }
      for (auto& [k, o] : c.ops) {
        if (o.evidence.count("recorded") && o.system == s.system && o.endpoint.substr(o.endpoint.find(' ') + 1) == path) {
          o.evidence.insert("static");
          o.functions.insert(s.functions.begin(), s.functions.end());
          o.static_exact = o.static_exact && s.static_exact;
        }
      }
      it = c.ops.erase(it);
    }
    c.never_ran = never_ran_flows.count(id) > 0;
  }

  // ------------------------------------------------------------------------- replaceability
  auto performers = [&](const std::string& key, const std::string& self) {
    std::vector<std::string> out;
    for (const auto& [id, c] : comps) {
      if (id != self && c.ops.count(key)) out.push_back(id);
    }
    return out;
  };
  auto describe = [](const OpUse& o) {
    std::string s = o.system + ": " + o.op;
    if (!o.status_sent.empty()) s += " (sets status " + join(o.status_sent, ", ") + ")";
    return s;
  };

  ojson components = ojson::array();
  for (auto& [id, c] : comps) {
    std::vector<const OpUse*> writes, unknown;
    for (const auto& [k, o] : c.ops) {
      if (o.access == "write") writes.push_back(&o);
      else if (o.access == "unknown") unknown.push_back(&o);
    }
    std::set<std::string> kinds;
    for (const auto& [k, o] : c.ops) {
      if (o.evidence.count("static")) kinds.insert(o.static_exact ? "exact" : "inferred");
      if (o.evidence.count("recorded")) kinds.insert("recorded");
    }
    if (c.uses_declared) kinds.insert("declared");
    std::size_t seen = 0;
    for (const auto& e : c.static_endpoints) {
      const auto bar = e.find('|');
      const auto bar2 = e.find('|', bar + 1);
      const bool no_method = bar2 == bar + 1;
      seen += c.observed_endpoints.count(e) || (no_method && c.observed_paths.count(e.substr(0, bar) + e.substr(bar2)))
                  ? 1 : 0;
    }
    const double coverage = c.static_endpoints.empty() ? 0.0 : static_cast<double>(seen) / static_cast<double>(c.static_endpoints.size());

    std::string cls, reason, condition;
    ojson properties = ojson::object();
    std::vector<std::string> evidence;
    std::set<std::string> covered_by_all;
    if (c.ops.empty()) {
      if (!c.dependents.empty()) {
        cls = "not_a_component";
        reason = "reaches no system itself; it's code used by " + join(sorted(c.dependents), ", ");
      } else {
        cls = "unknown";
        reason = "reaches no system the model can see";
      }
    } else if (c.never_ran && c.dependents.empty()) {
      cls = "independently_removable";
      reason = "never ran in the recorded window, and nothing in the code depends on it";
      evidence.push_back("no call to its trigger in the recordings, " + window[0].dump() + " to " + window[1].dump());
      kinds.erase("declared");
      kinds.insert("recorded");
    } else if (!unknown.empty() && writes.empty()) {
      cls = "unknown";
      reason = std::to_string(unknown.size()) + " operation(s) whose effect the recordings don't show";
    } else if (writes.empty()) {
      cls = "unknown";
      reason = "it only reads; what it produces goes outside the recorded systems (to people, files), so whether "
               "anyone needs it can't be seen";
    } else if (coverage < 0.5) {
      cls = "unknown";
      reason = "the recordings show " + std::to_string(seen) + " of its " + std::to_string(c.static_endpoints.size()) +
               " endpoints in use: too little to judge";
    } else {
      std::vector<std::string> unique;
      std::map<std::string, int> cover_count;
      for (const OpUse* w : writes) {
        const auto who = performers(w->key(), id);
        if (who.empty()) unique.push_back(describe(*w));
        for (const auto& p : who) cover_count[p]++;
      }
      std::vector<std::string> full;
      for (const auto& [p, n] : cover_count) {
        if (static_cast<std::size_t>(n) == writes.size()) full.push_back(p);
      }
      ojson functional = ojson::object();
      functional["unique"] = unique;
      functional["covered_by"] = full;
      properties["functional"] = functional;
      if (!unique.empty()) {
        cls = "essential";
        reason = "it does what no other component in the analysed code does: " + join(unique, "; ");
      } else {
        std::vector<std::string> by = full;
        if (by.empty()) {
          for (const auto& [p, n] : cover_count) by.push_back(p);
          reason = "every write it makes is also made by " + join(by, " and ") + " (together)";
        } else {
          reason = "every write it makes is also made by " + join(by, " or ");
        }
        covered_by_all.insert(by.begin(), by.end());
        // Two components that only cover each other: either can go, not both.
        for (const auto& o : by) {
          bool mutual = true;
          for (const auto& [k, w] : comps[o].ops) {
            if (w.access == "write" && !c.ops.count(k)) mutual = false;
          }
          if (mutual) evidence.push_back("mutual with " + o + ": each covers the other, so keep one of the two");
        }
        condition = "while " + join(by, full.empty() ? " and " : " or ") + " keep" + (by.size() == 1 ? "s" : "") + " running";
        cls = c.dependents.empty() ? "independently_removable" : "replaceable_after_dependents_change";
        if (!c.dependents.empty()) reason += "; but " + join(sorted(c.dependents), ", ") + " depend on it";
      }
      // Data equivalence: the same fields on the writes they share.
      ojson data = ojson::array();
      for (const auto& other : covered_by_all) {
        for (const OpUse* w : writes) {
          const auto oit = comps[other].ops.find(w->key());
          if (oit == comps[other].ops.end() || (w->request_fields.empty() && oit->second.request_fields.empty())) continue;
          std::vector<std::string> only_here, only_there;
          std::set_difference(w->request_fields.begin(), w->request_fields.end(), oit->second.request_fields.begin(),
                              oit->second.request_fields.end(), std::back_inserter(only_here));
          std::set_difference(oit->second.request_fields.begin(), oit->second.request_fields.end(),
                              w->request_fields.begin(), w->request_fields.end(), std::back_inserter(only_there));
          data.push_back({{"with", other}, {"operation", describe(*w)}, {"same_fields", only_here.empty() && only_there.empty()},
                          {"only_here", only_here}, {"only_there", only_there}});
        }
      }
      properties["data"] = data;
      properties["behavioural"] = "not shown: needs the same inputs replayed through both (feat/what-if)";
      ojson operational = ojson::object();
      operational["trigger"] = c.trigger;
      ojson others = ojson::object();
      for (const auto& o : covered_by_all) others[o] = comps[o].trigger;
      operational["covering_triggers"] = others;
      properties["operational"] = operational;
      for (const auto& o : outside) {
        for (const OpUse* w : writes) {
          if (o.key == w->key()) {
            evidence.push_back("also done by client '" + o.client + "', outside the analysed code" +
                               (o.who.empty() ? "" : " (" + o.who + ")") + ": " + describe(*w));
          }
        }
      }
    }
    for (const auto& [k, o] : c.ops) {
      if (o.shared_client) {
        evidence.push_back("traffic for " + describe(o) + " comes from a client that runs several components: "
                           "attributed to each whose code reaches the endpoint");
      }
    }

    std::set<std::string> inputs = c.files;
    inputs.insert(c.recordings.begin(), c.recordings.end());
    if (cls == "not_a_component") {  // the conclusion is about who uses it: their code is an input too
      for (const auto& dpt : c.dependents) inputs.insert(comps[dpt].files.begin(), comps[dpt].files.end());
    }
    if (c.uses_declared) inputs.insert(in.deployment.string());

    ojson ops = ojson::array();
    for (const auto& [k, o] : c.ops) {
      ojson j = ojson::object();
      j["system"] = o.system;
      j["operation"] = o.op;
      j["access"] = o.access;
      if (!o.resource.empty()) j["resource"] = o.resource;
      if (!o.status_sent.empty()) j["sets_status"] = o.status_sent;
      j["calls_seen"] = o.calls;
      j["evidence"] = sorted(o.evidence);
      j["functions"] = sorted(o.functions);
      ops.push_back(j);
    }
    ojson repl = ojson::object();
    repl["class"] = cls;
    repl["reason"] = reason;
    if (!condition.empty()) repl["condition"] = condition;
    if (!properties.empty()) repl["properties"] = properties;
    repl["confidence"] = weakest(kinds);
    repl["evidence_kinds"] = sorted(kinds);
    if (!evidence.empty()) repl["evidence"] = evidence;
    ojson comp = ojson::object();
    comp["id"] = id;
    comp["frontend"] = c.frontend;
    comp["trigger"] = c.trigger;
    comp["calls_seen"] = c.calls;
    comp["endpoint_coverage"] = static_cast<double>(static_cast<long long>(coverage * 1000.0 + 0.5)) / 1000.0;
    comp["dependents"] = sorted(c.dependents);
    comp["operations"] = ops;
    comp["replaceability"] = repl;
    comp["inputs"] = inputs_json(inputs);
    components.push_back(comp);
  }

  // Systems: what each holds and who reads or writes it.
  ojson systems = ojson::array();
  for (const auto& [sys, hosts] : dep.hosts) {
    std::map<std::string, std::pair<std::set<std::string>, std::set<std::string>>> res;  // resource -> writers, readers
    for (const auto& [id, c] : comps) {
      for (const auto& [k, o] : c.ops) {
        if (o.system != sys || o.resource.empty()) continue;
        (o.access == "write" ? res[o.resource].first : res[o.resource].second).insert(id);
      }
    }
    const auto rit = dep.regulated.find(sys);
    ojson resources = ojson::object();
    for (const auto& [r, wr] : res) {
      resources[r] = {{"writers", sorted(wr.first)}, {"readers", sorted(wr.second)},
                      {"regulated", rit != dep.regulated.end() && rit->second.count(r) > 0}};
    }
    ojson repl = ojson::object();
    if (rit != dep.regulated.end() && !rit->second.empty()) {
      repl["class"] = "essential";
      repl["reason"] = "holds regulated records (" + join(sorted(rit->second), ", ") + "), declared in the deployment";
      repl["confidence"] = "declared";
    } else {
      repl["class"] = "unknown";
      repl["reason"] = "a tool's replaceability needs its full behaviour spec (optum infer) set against what would "
                       "replace it; not decided from the glue alone";
      repl["confidence"] = "none";
    }
    std::set<std::string> inputs = {in.deployment.string()};
    if (!in.recordings.empty() && fs::exists(in.recordings / (sys + ".jsonl"))) inputs.insert((in.recordings / (sys + ".jsonl")).string());
    ojson s = ojson::object();
    s["name"] = sys;
    s["hosts"] = hosts;
    s["resources"] = resources;
    s["replaceability"] = repl;
    s["inputs"] = inputs_json(inputs);
    systems.push_back(s);
  }

  ojson unattributed = ojson::array();
  for (const auto& o : outside) {
    ojson u = ojson::object();
    u["client"] = o.client;
    if (!o.who.empty()) u["who"] = o.who;
    u["system"] = o.system;
    u["operation"] = o.op;
    u["access"] = o.access;
    if (!o.status_sent.empty()) u["sets_status"] = o.status_sent;
    u["calls"] = o.calls;
    unattributed.push_back(u);
  }

  ojson m = ojson::object();
  m["model_version"] = "0.1";
  m["source"] = g.source;
  m["window"] = window;
  m["components"] = components;
  m["systems"] = systems;
  m["outside_the_code"] = unattributed;
  m["classes"] = {
      {"independently_removable", "nothing depends on it, and it never ran or everything it writes is also written by another component (the condition says which must stay)"},
      {"replaceable_after_dependents_change", "covered like the above, but other components depend on it and must change first"},
      {"essential", "it does something no other analysed component does, or holds regulated records"},
      {"not_a_component", "code used by components, reaching no system itself"},
      {"unknown", "the evidence doesn't cover enough of what it does to say; the reason says what's missing"}};
  m["confidence_rule"] = "the weakest evidence kind a conclusion needs: inferred < declared < recorded, exact";
  return m;
}

std::string model_markdown(const ojson& m) {
  std::ostringstream s;
  s << "# What each component does, and whether it could go\n\n";
  s << "Code: " << m["source"].get<std::string>() << ". Traffic: " << m["window"][0].dump() << " to "
    << m["window"][1].dump() << ".\n\n";
  s << "| Component | Class | Why | Confidence |\n|---|---|---|---|\n";
  for (const auto& c : m["components"]) {
    const auto& r = c["replaceability"];
    std::string why = r["reason"].get<std::string>();
    if (r.contains("condition")) why += " (" + r["condition"].get<std::string>() + ")";
    s << "| `" << c["id"].get<std::string>() << "` | " << r["class"].get<std::string>() << " | " << why << " | "
      << r["confidence"].get<std::string>() << " |\n";
  }
  s << "\n| System | Class | Why |\n|---|---|---|\n";
  for (const auto& sys : m["systems"]) {
    const auto& r = sys["replaceability"];
    s << "| " << sys["name"].get<std::string>() << " | " << r["class"].get<std::string>() << " | "
      << r["reason"].get<std::string>() << " |\n";
  }
  if (!m["outside_the_code"].empty()) {
    s << "\nTraffic from outside the analysed code: ";
    std::map<std::string, long long> by;
    for (const auto& o : m["outside_the_code"]) by[o["client"].get<std::string>()] += o["calls"].get<long long>();
    bool first = true;
    for (const auto& [c, n] : by) {
      s << (first ? "" : ", ") << c << " (" << n << " calls)";
      first = false;
    }
    s << ".\n";
  }
  return s.str();
}

ojson check_model(const ojson& model, const fs::path& base) {
  ojson stale = ojson::array();
  long long fresh = 0;
  auto check = [&](const ojson& subject, const std::string& name) {
    std::vector<std::string> changed;
    for (const auto& i : subject["inputs"]) {
      fs::path p = i["path"].get<std::string>();
      if (p.is_relative()) p = base / p;
      if (content_hash(p) != i["hash"].get<std::string>()) changed.push_back(i["path"].get<std::string>());
    }
    if (changed.empty()) ++fresh;
    else stale.push_back({{"subject", name}, {"changed", changed}});
  };
  for (const auto& c : model["components"]) check(c, c["id"].get<std::string>());
  for (const auto& s : model["systems"]) check(s, "system " + s["name"].get<std::string>());
  ojson out = ojson::object();
  out["stale"] = stale;
  out["fresh"] = fresh;
  return out;
}

}  // namespace optum
