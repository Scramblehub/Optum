#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>

#include "optum/ir.hpp"
#include "optum/version.hpp"

namespace optum {
namespace {

using nlohmann::json;

// ---- writing ----

json loc_to_json(const SourceLoc& loc) {
  json j = {{"file", loc.file}};
  if (loc.line) j["line"] = loc.line;
  if (loc.end_line) j["end_line"] = loc.end_line;
  if (loc.address) {
    char buf[2 + 16 + 1];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(*loc.address));
    j["address"] = buf;  // hex string: JSON numbers lose precision above 2^53 in many tools
  }
  return j;
}

template <typename T>
void write_common(json& j, const T& item) {
  if (item.loc) j["loc"] = loc_to_json(*item.loc);
  if (!item.frontend.empty()) j["frontend"] = item.frontend;
  j["confidence"] = to_string(item.confidence);
  if (!item.attrs.empty()) j["attrs"] = item.attrs;
}

// ---- reading ----

[[noreturn]] void fail(const std::string& path, const std::string& msg) {
  throw IrError(path + ": " + msg);
}

void check_keys(const json& obj, const std::string& path, std::initializer_list<const char*> allowed,
                std::initializer_list<const char*> required) {
  if (!obj.is_object()) fail(path, "expected an object");
  for (const char* key : required) {
    if (!obj.contains(key)) fail(path, std::string("missing required field '") + key + "'");
  }
  for (const auto& [key, _] : obj.items()) {
    bool ok = false;
    for (const char* a : allowed) ok = ok || key == a;
    if (!ok) fail(path, "unknown field '" + key + "' (put extra data in 'attrs')");
  }
}

std::string get_string(const json& obj, const char* key, const std::string& path) {
  const json& v = obj.at(key);
  if (!v.is_string()) fail(path + "." + key, "expected a string");
  return v.get<std::string>();
}

std::uint32_t get_line(const json& obj, const char* key, const std::string& path) {
  if (!obj.contains(key)) return 0;
  const json& v = obj.at(key);
  if (!v.is_number_unsigned() || v.get<std::uint64_t>() > UINT32_MAX) {
    fail(path + "." + key, "expected a non-negative integer");
  }
  return static_cast<std::uint32_t>(v.get<std::uint64_t>());
}

SourceLoc loc_from_json(const json& j, const std::string& path) {
  check_keys(j, path, {"file", "line", "end_line", "address"}, {"file"});
  SourceLoc loc;
  loc.file = get_string(j, "file", path);
  loc.line = get_line(j, "line", path);
  loc.end_line = get_line(j, "end_line", path);
  if (j.contains("address")) {
    const std::string s = get_string(j, "address", path);
    std::uint64_t addr = 0;
    const bool hex_prefix = s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    if (!hex_prefix) fail(path + ".address", "expected a hex string like \"0x401000\"");
    const char* begin = s.data() + 2;
    const char* end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(begin, end, addr, 16);
    if (ec != std::errc() || ptr != end) {
      fail(path + ".address", "expected a hex string like \"0x401000\"");
    }
    loc.address = addr;
  }
  return loc;
}

template <typename T>
void read_common(T& item, const json& j, const std::string& path) {
  if (j.contains("loc")) item.loc = loc_from_json(j.at("loc"), path + ".loc");
  if (j.contains("frontend")) item.frontend = get_string(j, "frontend", path);
  if (j.contains("confidence")) {
    const std::string s = get_string(j, "confidence", path);
    const auto c = confidence_from_string(s);
    if (!c) fail(path + ".confidence", "unknown confidence '" + s + "'");
    item.confidence = *c;
  }
  if (j.contains("attrs")) {
    if (!j.at("attrs").is_object()) fail(path + ".attrs", "expected an object");
    item.attrs = j.at("attrs");
  }
}

// True for exactly <digits>.<digits>.<digits>, the shape ir.schema.json requires.
bool is_dotted_triple(std::string_view v) {
  int dots = 0;
  bool digit_seen = false;  // since the last dot
  for (const char c : v) {
    if (c == '.') {
      if (!digit_seen) return false;
      ++dots;
      digit_seen = false;
    } else if (c >= '0' && c <= '9') {
      digit_seen = true;
    } else {
      return false;
    }
  }
  return dots == 2 && digit_seen;
}

// Accepts the same major.minor as this build; patch may differ.
void check_version(const json& j) {
  if (!j.is_string()) fail("ir_version", "expected a string like \"0.1.0\"");
  const std::string theirs = j.get<std::string>();
  const std::string_view ours = kIrVersion;
  auto major_minor = [](std::string_view v) { return v.substr(0, v.rfind('.')); };
  if (!is_dotted_triple(theirs) || major_minor(theirs) != major_minor(ours)) {
    fail("ir_version", "unsupported version '" + theirs + "', this build reads " +
                           std::string(major_minor(ours)) + ".x");
  }
}

}  // namespace

json to_json(const Graph& g) {
  json nodes = json::array();
  for (const Node& n : g.nodes()) {
    json j = {{"id", n.id}, {"kind", to_string(n.kind)}, {"name", n.name}};
    write_common(j, n);
    nodes.push_back(std::move(j));
  }
  json edges = json::array();
  for (const Edge& e : g.edges()) {
    json j = {{"from", g.node(e.from).id}, {"to", g.node(e.to).id}, {"kind", to_string(e.kind)}};
    write_common(j, e);
    edges.push_back(std::move(j));
  }
  return {{"ir_version", std::string(kIrVersion)},
          {"source", g.source},
          {"nodes", std::move(nodes)},
          {"edges", std::move(edges)}};
}

Graph graph_from_json(const json& j) {
  check_keys(j, "graph", {"ir_version", "source", "nodes", "edges"},
             {"ir_version", "nodes", "edges"});
  check_version(j.at("ir_version"));

  Graph g;
  if (j.contains("source")) g.source = get_string(j, "source", "graph");
  if (!j.at("nodes").is_array()) fail("nodes", "expected an array");
  if (!j.at("edges").is_array()) fail("edges", "expected an array");

  const json& nodes = j.at("nodes");
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const std::string path = "nodes[" + std::to_string(i) + "]";
    const json& nj = nodes[i];
    check_keys(nj, path, {"id", "kind", "name", "loc", "frontend", "confidence", "attrs"},
               {"id", "kind"});
    Node n;
    n.id = get_string(nj, "id", path);
    if (n.id.empty()) fail(path + ".id", "must not be empty");
    if (g.find(n.id)) fail(path + ".id", "duplicate id '" + n.id + "'");
    const std::string kind = get_string(nj, "kind", path);
    const auto k = node_kind_from_string(kind);
    if (!k) fail(path + ".kind", "unknown node kind '" + kind + "'");
    n.kind = *k;
    if (nj.contains("name")) n.name = get_string(nj, "name", path);
    read_common(n, nj, path);
    g.add_node(std::move(n));
  }

  const json& edges = j.at("edges");
  for (std::size_t i = 0; i < edges.size(); ++i) {
    const std::string path = "edges[" + std::to_string(i) + "]";
    const json& ej = edges[i];
    check_keys(ej, path, {"from", "to", "kind", "loc", "frontend", "confidence", "attrs"},
               {"from", "to", "kind"});
    Edge e;
    for (const char* end : {"from", "to"}) {
      const std::string id = get_string(ej, end, path);
      const auto idx = g.find(id);
      if (!idx) fail(path + "." + end, "unknown node id '" + id + "'");
      (std::string_view(end) == "from" ? e.from : e.to) = *idx;
    }
    const std::string kind = get_string(ej, "kind", path);
    const auto k = edge_kind_from_string(kind);
    if (!k) fail(path + ".kind", "unknown edge kind '" + kind + "'");
    e.kind = *k;
    read_common(e, ej, path);
    g.add_edge(std::move(e));
  }
  return g;
}

Graph load_graph(const std::string& path) {
  std::error_code ec;
  if (std::filesystem::is_directory(path, ec)) throw IrError(path + ": is a directory, not a graph JSON file");
  std::ifstream in(path);
  if (!in) throw IrError(path + ": cannot open file");
  json j;
  try {
    j = json::parse(in);
  } catch (const json::parse_error& e) {
    throw IrError(path + ": invalid JSON: " + e.what());
  } catch (const std::ios_base::failure& e) {
    throw IrError(path + ": cannot read file (" + e.what() + ")");
  }
  try {
    return graph_from_json(j);
  } catch (const IrError& e) {
    throw IrError(path + ": " + e.what());
  }
}

}  // namespace optum
