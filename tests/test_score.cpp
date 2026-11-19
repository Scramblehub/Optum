#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "optum/score.hpp"

using namespace optum;
using Catch::Approx;

namespace {
Graph make(std::initializer_list<const char*> fns,
           std::initializer_list<std::pair<const char*, const char*>> calls,
           const char* hard_to = nullptr) {
  Graph g;
  for (const char* f : fns) {
    Node n;
    n.id = f;
    n.kind = NodeKind::Function;
    g.add_node(n);
  }
  for (auto [a, b] : calls) {
    Edge e;
    e.from = *g.find(a);
    e.to = *g.find(b);
    if (hard_to && std::string(b) == hard_to) e.attrs = {{"difficulty", "hard"}};
    g.add_edge(e);
  }
  return g;
}
}  // namespace

TEST_CASE("identical graphs score 100%") {
  const Graph g = make({"a", "b"}, {{"a", "b"}});
  const auto r = score(g, g);
  REQUIRE(r.edges.precision() == 1.0);
  REQUIRE(r.edges.recall() == 1.0);
  REQUIRE(r.missing_edges.empty());
  REQUIRE(r.extra_edges.empty());
}

TEST_CASE("missing and extra edges are counted and listed") {
  const Graph expected = make({"a", "b", "c", "d"}, {{"a", "b"}, {"a", "c"}, {"b", "d"}, {"c", "d"}},
                              "d");
  const Graph produced = make({"a", "b", "c", "x"}, {{"a", "b"}, {"a", "c"}, {"a", "x"}});
  const auto r = score(expected, produced);

  REQUIRE(r.nodes.matched == 3);
  REQUIRE(r.missing_nodes == std::vector<std::string>{"d (function)"});
  REQUIRE(r.extra_nodes == std::vector<std::string>{"x (function)"});

  REQUIRE(r.edges.expected == 4);
  REQUIRE(r.edges.found == 3);
  REQUIRE(r.edges.matched == 2);
  REQUIRE(r.edges.precision() == Approx(2.0 / 3.0));
  REQUIRE(r.edges.recall() == Approx(0.5));
  REQUIRE(r.edges_by_kind.at("calls").matched == 2);
  REQUIRE(r.missing_edges == std::vector<std::string>{"b -calls-> d", "c -calls-> d"});
  REQUIRE(r.extra_edges == std::vector<std::string>{"a -calls-> x"});

  REQUIRE(r.hard_edges.expected == 2);
  REQUIRE(r.hard_edges.matched == 0);
}

TEST_CASE("node kind must match too") {
  Graph expected = make({"a"}, {});
  Graph produced;
  Node n;
  n.id = "a";
  n.kind = NodeKind::Module;
  produced.add_node(n);
  const auto r = score(expected, produced);
  REQUIRE(r.nodes.matched == 0);
}

TEST_CASE("empty produced graph: precision 1, recall 0") {
  const auto r = score(make({"a", "b"}, {{"a", "b"}}), Graph{});
  REQUIRE(r.edges.precision() == 1.0);
  REQUIRE(r.edges.recall() == 0.0);
  REQUIRE(to_json(r)["edges"]["recall"] == 0.0);
}
