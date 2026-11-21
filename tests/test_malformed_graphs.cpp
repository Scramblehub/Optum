// Graphs that a frontend never writes but a saved file or an adapter can hold: `defines` cycles,
// attrs of the wrong type, a directory where a file was expected. Each of these once hung or
// aborted the process; they must give a report or an IrError instead.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fstream>

#include "optum/clones.hpp"
#include "optum/diagnose.hpp"
#include "optum/entities.hpp"
#include "optum/ir.hpp"
#include "optum/verify.hpp"
#include "optum/view.hpp"

using namespace optum;
using Catch::Matchers::ContainsSubstring;

namespace {

Node make(const char* id, NodeKind k, const char* frontend = "python") {
  Node n;
  n.id = id;
  n.kind = k;
  n.name = id;
  n.frontend = frontend;
  return n;
}

// A module whose two functions "define" each other: py:m.a -> py:m.b -> py:m.a.
Graph defines_cycle() {
  Graph g;
  Node m = make("py:m", NodeKind::Module);
  Node a = make("py:m.a", NodeKind::Function);
  Node b = make("py:m.b", NodeKind::Function);
  a.attrs = {{"local", true}};
  b.attrs = {{"local", true}};
  g.add_node(m);
  g.add_node(a);
  g.add_node(b);
  g.add_edge("py:m", "py:m.a", EdgeKind::Defines);
  g.add_edge("py:m.a", "py:m.b", EdgeKind::Defines);
  g.add_edge("py:m.b", "py:m.a", EdgeKind::Defines);
  return g;
}

}  // namespace

TEST_CASE("diagnose finishes on a graph whose defines edges form a cycle") {
  // The walk up "what defines this" looped forever when two unused functions defined each other.
  const Diagnosis d = diagnose(defines_cycle(), {});
  REQUIRE(d.findings.size() == 1);
  CHECK(d.findings[0].kind == "unused");
}

TEST_CASE("clone detection finishes when a function defines itself or its parent") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "optum_malformed_clones";
  fs::create_directories(dir);
  {
    std::ofstream out(dir / "m.py");
    for (const char* name : {"first", "second"}) {
      out << "def " << name << "(items, limit):\n"
          << "    total = 0\n"
          << "    seen = []\n"
          << "    for item in items:\n"
          << "        if item in seen:\n"
          << "            continue\n"
          << "        seen.append(item)\n"
          << "        if len(seen) > limit:\n"
          << "            break\n"
          << "        total += item * 2\n"
          << "    return total, seen\n\n";
    }
  }
  Graph g;
  g.add_node(make("py:m", NodeKind::Module));
  for (const auto& [id, line] : {std::pair{"py:m.first", 1u}, std::pair{"py:m.second", 13u}}) {
    Node n = make(id, NodeKind::Function);
    n.loc = SourceLoc{"m.py", line, line + 10, std::nullopt};
    g.add_node(n);
  }
  g.add_edge("py:m.first", "py:m.second", EdgeKind::Defines);
  g.add_edge("py:m.second", "py:m.first", EdgeKind::Defines);
  g.add_edge("py:m.first", "py:m.first", EdgeKind::Defines);  // and itself
  CloneOptions small;  // the sample is short: lower the size limits, not the similarity
  small.min_tokens = 20;
  small.min_distinct = 3;
  const auto groups = find_clones(g, dir, small);
  fs::remove_all(dir);
  REQUIRE(groups.size() == 1);
  CHECK(groups[0].members.size() == 2);
}

TEST_CASE("table keys of the wrong type are skipped, not fatal") {
  Graph g;
  g.add_node(make("py:a.load", NodeKind::Function));
  g.add_node(make("sql:customers", NodeKind::Store));
  Edge e;
  e.from = *g.find("py:a.load");
  e.to = *g.find("sql:customers");
  e.kind = EdgeKind::Reads;
  e.attrs = {{"columns", {"id", "email", "name"}}, {"keys", {7, "id"}}};
  g.add_edge(e);
  CHECK_NOTHROW(find_entities(g));
}

TEST_CASE("entities: a class and a table with the same fields are one entity") {
  Graph g;
  Node c = make("py:shop.Customer", NodeKind::DataType);
  c.attrs = {{"fields", {"id", "email", "first_name", "last_name", "phone"}}};
  g.add_node(c);
  Node other = make("py:shop.Invoice", NodeKind::DataType);
  other.attrs = {{"fields", {"id", "amount", "currency", "due_date", "paid"}}};
  g.add_node(other);
  g.add_node(make("py:shop.sync", NodeKind::Function));
  g.add_node(make("sql:customers", NodeKind::Store));
  Edge e;
  e.from = *g.find("py:shop.sync");
  e.to = *g.find("sql:customers");
  e.kind = EdgeKind::Reads;
  e.attrs = {{"columns", {"id", "email", "firstName", "lastName", "phone"}}};
  g.add_edge(e);
  const EntityResult r = find_entities(g);
  REQUIRE(r.groups.size() == 1);
  CHECK(r.groups[0].members.size() == 2);
  CHECK(r.groups[0].name == "customer");
}

TEST_CASE("verify rejects a trace_version of the wrong type or length") {
  const Graph g;
  using nlohmann::json;
  CHECK_THROWS_WITH(verify(g, json{{"trace_version", 1}, {"calls", json::array()}}),
                    ContainsSubstring("missing \"trace_version\""));
  CHECK_THROWS_WITH(verify(g, json{{"calls", json::array()}}), ContainsSubstring("missing \"trace_version\""));
  CHECK_THROWS_WITH(verify(g, json{{"trace_version", "0.10"}, {"calls", json::array()}}),
                    ContainsSubstring("unsupported trace_version '0.10'"));
  CHECK_NOTHROW(verify(g, json{{"trace_version", "0.1"}, {"calls", json::array()}}));
  CHECK_NOTHROW(verify(g, json{{"trace_version", "0.1.3"}, {"calls", json::array()}}));
}

TEST_CASE("loading a directory as a graph is an IrError") {
  CHECK_THROWS_AS(load_graph(std::filesystem::temp_directory_path().string()), IrError);
}

TEST_CASE("modules_of agrees with module_of for every node") {
  Graph g = defines_cycle();
  g.add_node(make("sql:t", NodeKind::Store));
  const auto all = modules_of(g);
  REQUIRE(all.size() == g.nodes().size());
  for (NodeIndex i = 0; i < all.size(); ++i) CHECK(all[i] == module_of(g, i));
}
