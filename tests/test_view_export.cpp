#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "optum/export.hpp"
#include "optum/view.hpp"

using namespace optum;
using Catch::Matchers::ContainsSubstring;

namespace {
// Two modules: a.f -> b.g (twice, from two functions), a.f reads a table, b.g calls b.h (inferred).
Graph sample() {
  Graph g;
  g.source = "sample";
  auto node = [&](const char* id, NodeKind k) {
    Node n;
    n.id = id;
    n.kind = k;
    n.name = id;
    g.add_node(n);
  };
  node("py:a", NodeKind::Module);
  node("py:a.f", NodeKind::Function);
  node("py:a.f2", NodeKind::Function);
  node("py:b", NodeKind::Module);
  node("py:b.g", NodeKind::Function);
  node("py:b.h", NodeKind::Function);
  node("sql:orders", NodeKind::Store);
  g.add_edge("py:a", "py:a.f", EdgeKind::Defines);
  g.add_edge("py:a", "py:a.f2", EdgeKind::Defines);
  g.add_edge("py:b", "py:b.g", EdgeKind::Defines);
  g.add_edge("py:b", "py:b.h", EdgeKind::Defines);
  g.add_edge("py:a.f", "py:b.g", EdgeKind::Calls);
  g.add_edge("py:a.f2", "py:b.g", EdgeKind::Calls);
  g.add_edge("py:a.f", "sql:orders", EdgeKind::Reads);
  Edge e;
  e.from = *g.find("py:b.g");
  e.to = *g.find("py:b.h");
  e.confidence = Confidence::Inferred;
  g.add_edge(e);
  return g;
}
}  // namespace

TEST_CASE("resolve_node: exact, unique ending, ambiguous") {
  const Graph g = sample();
  REQUIRE(g.node(resolve_node(g, "py:b.g")).id == "py:b.g");
  REQUIRE(g.node(resolve_node(g, "g")).id == "py:b.g");
  REQUIRE(g.node(resolve_node(g, "orders")).id == "sql:orders");
  REQUIRE_THROWS_WITH(resolve_node(g, "nope"), ContainsSubstring("no node matches"));
  Graph g2 = sample();
  Node n;
  n.id = "py:c.g";
  g2.add_node(n);
  REQUIRE_THROWS_WITH(resolve_node(g2, "g"), ContainsSubstring("matches 2 nodes"));
}

TEST_CASE("module level: edges collapse with counts, self-loops vanish") {
  ViewOptions o;
  o.level = Level::Module;
  const Graph v = make_view(sample(), o);
  REQUIRE(v.nodes().size() == 3);  // a, b, orders
  REQUIRE(v.edges().size() == 2);  // a -calls-> b (x2), a -reads-> orders; b.g->b.h is internal
  const Edge& calls = v.edges()[0].kind == EdgeKind::Calls ? v.edges()[0] : v.edges()[1];
  REQUIRE(v.node(calls.from).id == "py:a");
  REQUIRE(v.node(calls.to).id == "py:b");
  REQUIRE(calls.attrs["count"] == 2);
}

TEST_CASE("edge filters keep structure") {
  ViewOptions o;
  o.edge_kinds = {EdgeKind::Reads};
  Graph v = make_view(sample(), o);
  std::size_t reads = 0, calls = 0, defines = 0;
  for (const Edge& e : v.edges()) {
    reads += e.kind == EdgeKind::Reads;
    calls += e.kind == EdgeKind::Calls;
    defines += e.kind == EdgeKind::Defines;
  }
  REQUIRE(reads == 1);
  REQUIRE(calls == 0);
  REQUIRE(defines == 4);

  ViewOptions no_guess;
  no_guess.hide_inferred = true;
  v = make_view(sample(), no_guess);
  for (const Edge& e : v.edges()) REQUIRE(e.confidence != Confidence::Inferred);
}

TEST_CASE("focus keeps neighbours within depth plus their containers") {
  ViewOptions o;
  o.focus = "sql:orders";
  o.depth = 1;
  Graph v = make_view(sample(), o);
  REQUIRE(v.find("py:a.f"));
  REQUIRE(v.find("py:a"));  // container of a.f
  REQUIRE_FALSE(v.find("py:b.g"));
  o.depth = 2;
  v = make_view(sample(), o);
  REQUIRE(v.find("py:b.g"));
  REQUIRE(v.find("py:b"));
}

TEST_CASE("dot: clusters, styles, legend, escaping") {
  Graph g = sample();
  Node odd;
  odd.id = "http:x.example.com/say\"hi\"";
  odd.kind = NodeKind::External;
  g.add_node(odd);
  const std::string dot = to_dot(g);
  REQUIRE_THAT(dot, ContainsSubstring("digraph optum {"));
  REQUIRE_THAT(dot, ContainsSubstring("subgraph \"cluster_"));
  REQUIRE_THAT(dot, ContainsSubstring("label=\"f\""));  // name relative to its module
  REQUIRE_THAT(dot, ContainsSubstring("shape=cylinder"));
  REQUIRE_THAT(dot, ContainsSubstring("\"py:b.g\" -> \"py:b.h\" [color=\"#2a78d6\", style=dotted]"));
  REQUIRE_THAT(dot, ContainsSubstring("label=\"reads\""));
  REQUIRE_THAT(dot, ContainsSubstring("say\\\"hi\\\""));
  REQUIRE_THAT(dot, ContainsSubstring("Legend"));
}

TEST_CASE("mermaid: shapes, link styles, quotes") {
  Graph g = sample();
  Node odd;
  odd.id = "py:q";
  odd.kind = NodeKind::Function;
  odd.name = "say \"hi\"";
  g.add_node(odd);
  const std::string m = to_mermaid(g);
  REQUIRE_THAT(m, ContainsSubstring("flowchart LR"));
  REQUIRE_THAT(m, ContainsSubstring("[(\"sql:orders\")]"));
  REQUIRE_THAT(m, ContainsSubstring("-->|reads|"));
  REQUIRE_THAT(m, ContainsSubstring("-.->"));
  REQUIRE_THAT(m, ContainsSubstring("linkStyle 0 stroke:"));
  REQUIRE_THAT(m, ContainsSubstring("#quot;hi#quot;"));
}
