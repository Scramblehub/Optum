#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "optum/ir.hpp"

using namespace optum;
using Catch::Matchers::ContainsSubstring;

namespace {
Node make(std::string id, NodeKind kind = NodeKind::Function) {
  Node n;
  n.id = std::move(id);
  n.kind = kind;
  return n;
}
}  // namespace

TEST_CASE("enum names round-trip") {
  for (auto k : {NodeKind::Program, NodeKind::Module, NodeKind::Function, NodeKind::DataType,
                 NodeKind::Store, NodeKind::External, NodeKind::Step}) {
    REQUIRE(node_kind_from_string(to_string(k)) == k);
  }
  for (auto k : {EdgeKind::Calls, EdgeKind::Imports, EdgeKind::Defines, EdgeKind::Reads,
                 EdgeKind::Writes, EdgeKind::SendsTo, EdgeKind::Triggers}) {
    REQUIRE(edge_kind_from_string(to_string(k)) == k);
  }
  REQUIRE(to_string(NodeKind::DataType) == "data_type");
  REQUIRE(to_string(EdgeKind::SendsTo) == "sends_to");
  REQUIRE_FALSE(node_kind_from_string("fn"));
}

TEST_CASE("nodes are found by id and indexed in insertion order") {
  Graph g;
  REQUIRE(g.add_node(make("a")) == 0);
  REQUIRE(g.add_node(make("b")) == 1);
  REQUIRE(g.find("b") == 1u);
  REQUIRE_FALSE(g.find("c"));
  REQUIRE_THROWS_AS(g.add_node(make("")), IrError);
}

TEST_CASE("adding a node with an existing id merges into it") {
  Graph g;
  Node first = make("f");
  first.confidence = Confidence::Inferred;
  first.attrs = {{"a", 1}, {"b", 1}};
  g.add_node(first);

  Node second = make("f");
  second.name = "f";
  second.loc = SourceLoc{"f.py", 3, 5, std::nullopt};
  second.frontend = "python";
  second.confidence = Confidence::Exact;
  second.attrs = {{"b", 2}};
  REQUIRE(g.add_node(second) == 0);

  REQUIRE(g.nodes().size() == 1);
  const Node& n = g.node(0);
  REQUIRE(n.name == "f");
  REQUIRE(n.loc->line == 3);
  REQUIRE(n.frontend == "python");
  REQUIRE(n.confidence == Confidence::Exact);  // the stronger one wins
  REQUIRE(n.attrs == nlohmann::json{{"a", 1}, {"b", 2}});

  // Weaker confidence never downgrades.
  Node third = make("f");
  third.confidence = Confidence::Inferred;
  g.add_node(third);
  REQUIRE(g.node(0).confidence == Confidence::Exact);
}

TEST_CASE("merging nodes of different kinds is an error") {
  Graph g;
  g.add_node(make("x", NodeKind::Module));
  REQUIRE_THROWS_WITH(g.add_node(make("x", NodeKind::Function)),
                      ContainsSubstring("is a module, cannot merge a function"));
}

TEST_CASE("edges are deduplicated on (from, to, kind)") {
  Graph g;
  g.add_node(make("a"));
  g.add_node(make("b"));
  g.add_edge("a", "b", EdgeKind::Calls);
  g.add_edge("a", "b", EdgeKind::Calls);
  g.add_edge("a", "b", EdgeKind::Reads);
  g.add_edge("b", "a", EdgeKind::Calls);
  REQUIRE(g.edges().size() == 3);
  REQUIRE_THROWS_WITH(g.add_edge("a", "nope", EdgeKind::Calls), ContainsSubstring("nope"));
  REQUIRE_THROWS_AS(g.add_edge(Edge{0, 9, EdgeKind::Calls, {}, {}, {}, {}}), IrError);
}

TEST_CASE("merging graphs remaps indices and unions by id") {
  Graph a;
  a.add_node(make("shared"));
  a.add_node(make("only_a"));
  a.add_edge("only_a", "shared", EdgeKind::Calls);

  Graph b;
  b.add_node(make("only_b"));
  b.add_node(make("shared"));
  b.add_edge("only_b", "shared", EdgeKind::Calls);
  b.add_edge("only_b", "only_b", EdgeKind::Calls);

  a.merge(b);
  REQUIRE(a.nodes().size() == 3);
  REQUIRE(a.edges().size() == 3);
  const auto& e = a.edges()[1];
  REQUIRE(a.node(e.from).id == "only_b");
  REQUIRE(a.node(e.to).id == "shared");

  a.merge(b);  // merging again changes nothing
  REQUIRE(a.nodes().size() == 3);
  REQUIRE(a.edges().size() == 3);
}
