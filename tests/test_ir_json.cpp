#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "optum/ir.hpp"
#include "optum/version.hpp"

using namespace optum;
using Catch::Matchers::ContainsSubstring;
using nlohmann::json;

namespace {
json doc(json nodes, json edges = json::array()) {
  return {{"ir_version", "0.1.0"}, {"nodes", std::move(nodes)}, {"edges", std::move(edges)}};
}
}  // namespace

TEST_CASE("a graph survives a JSON round trip unchanged") {
  Graph g;
  g.source = "repo";
  Node n;
  n.id = "bin:x!0x401136";
  n.kind = NodeKind::Function;
  n.name = "checksum";
  n.loc = SourceLoc{"x", 0, 0, 0xffffffffffffffffULL};  // above 2^53: kept exact as hex
  n.frontend = "elf";
  n.confidence = Confidence::Inferred;
  n.attrs = {{"size", 42}};
  g.add_node(n);
  Node m;
  m.id = "m";
  m.kind = NodeKind::Module;
  g.add_node(m);
  g.add_edge("m", "bin:x!0x401136", EdgeKind::Defines);

  const json once = to_json(g);
  REQUIRE(once["ir_version"] == std::string(kIrVersion));
  REQUIRE(once["nodes"][0]["loc"]["address"] == "0xffffffffffffffff");
  const json twice = to_json(graph_from_json(once));
  REQUIRE(once == twice);
}

TEST_CASE("errors name the exact field") {
  auto err = [](const json& j) {
    try {
      graph_from_json(j);
    } catch (const IrError& e) {
      return std::string(e.what());
    }
    return std::string("no error");
  };
  REQUIRE_THAT(err(doc({{{"id", "a"}, {"kind", "fn"}}})),
               ContainsSubstring("nodes[0].kind: unknown node kind 'fn'"));
  REQUIRE_THAT(err(doc({{{"id", "a"}, {"kind", "module"}, {"colour", 1}}})),
               ContainsSubstring("nodes[0]: unknown field 'colour'"));
  REQUIRE_THAT(err(doc({{{"kind", "module"}}})),
               ContainsSubstring("nodes[0]: missing required field 'id'"));
  REQUIRE_THAT(err(doc({{{"id", "a"}, {"kind", "module"}}, {{"id", "a"}, {"kind", "module"}}})),
               ContainsSubstring("nodes[1].id: duplicate id 'a'"));
  REQUIRE_THAT(err(doc({{{"id", "a"}, {"kind", "module"}}},
                       {{{"from", "a"}, {"to", "zz"}, {"kind", "calls"}}})),
               ContainsSubstring("edges[0].to: unknown node id 'zz'"));
  REQUIRE_THAT(
      err(doc({{{"id", "a"}, {"kind", "function"}, {"loc", {{"file", "x"}, {"line", -1}}}}})),
      ContainsSubstring("nodes[0].loc.line"));
  REQUIRE_THAT(
      err(doc({{{"id", "a"}, {"kind", "function"}, {"loc", {{"file", "x"}, {"address", "12"}}}}})),
      ContainsSubstring("nodes[0].loc.address"));
  REQUIRE_THAT(err(doc({{{"id", "a"}, {"kind", "module"}, {"confidence", "sure"}}})),
               ContainsSubstring("unknown confidence 'sure'"));
}

TEST_CASE("versions: same major.minor accepted, others rejected") {
  json j = doc(json::array());
  j["ir_version"] = "0.1.99";
  REQUIRE_NOTHROW(graph_from_json(j));
  for (const char* v : {"0.2.0", "1.1.0", "0.1", "banana", "0.1.", "0.1.x", "0.1.0-rc1", "0.1.0.0"}) {
    j["ir_version"] = v;
    REQUIRE_THROWS_WITH(graph_from_json(j), ContainsSubstring("ir_version"));
  }
}

TEST_CASE("load_graph reports missing files") {
  REQUIRE_THROWS_WITH(load_graph("/no/such/file.json"), ContainsSubstring("cannot open"));
}

TEST_CASE("an empty or one-character address is an error, not undefined behaviour") {
  for (const char* a : {"", "0", "x", "0x"}) {
    REQUIRE_THROWS_WITH(
        graph_from_json(doc({{{"id", "a"}, {"kind", "function"},
                              {"loc", {{"file", "x"}, {"address", a}}}}})),
        ContainsSubstring("nodes[0].loc.address"));
  }
}
