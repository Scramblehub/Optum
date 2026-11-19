#include "optum/score.hpp"

#include <algorithm>
#include <cstdio>
#include <set>

namespace optum {
namespace {

std::string node_key(const Node& n) { return n.id + " (" + std::string(to_string(n.kind)) + ")"; }

std::string edge_key(const Graph& g, const Edge& e) {
  return g.node(e.from).id + " -" + std::string(to_string(e.kind)) + "-> " + g.node(e.to).id;
}

bool is_hard(const Edge& e) {
  const auto it = e.attrs.find("difficulty");
  return it != e.attrs.end() && *it == "hard";
}

nlohmann::json tally_json(const Tally& t) {
  return {{"expected", t.expected}, {"found", t.found},          {"matched", t.matched},
          {"precision", t.precision()}, {"recall", t.recall()}};
}

std::string row(const std::string& label, const Tally& t, bool recall_only = false) {
  char buf[160];
  if (recall_only) {
    std::snprintf(buf, sizeof buf, "%-16s %9zu %7s %8zu %10s %7.1f%%\n", label.c_str(), t.expected,
                  "-", t.matched, "-", 100.0 * t.recall());
  } else {
    std::snprintf(buf, sizeof buf, "%-16s %9zu %7zu %8zu %9.1f%% %7.1f%%\n", label.c_str(),
                  t.expected, t.found, t.matched, 100.0 * t.precision(), 100.0 * t.recall());
  }
  return buf;
}

}  // namespace

ScoreReport score(const Graph& expected, const Graph& produced) {
  ScoreReport r;

  std::set<std::string> exp_nodes, got_nodes;
  for (const Node& n : expected.nodes()) exp_nodes.insert(node_key(n));
  for (const Node& n : produced.nodes()) got_nodes.insert(node_key(n));
  r.nodes.expected = exp_nodes.size();
  r.nodes.found = got_nodes.size();
  for (const auto& k : exp_nodes) {
    if (got_nodes.count(k)) ++r.nodes.matched;
    else r.missing_nodes.push_back(k);
  }
  for (const auto& k : got_nodes) {
    if (!exp_nodes.count(k)) r.extra_nodes.push_back(k);
  }

  // Edges are compared by node id, so the two graphs' indices don't matter.
  std::set<std::string> got_edges;
  for (const Edge& e : produced.edges()) {
    got_edges.insert(edge_key(produced, e));
    ++r.edges_by_kind[std::string(to_string(e.kind))].found;
  }
  std::set<std::string> exp_edges;
  for (const Edge& e : expected.edges()) {
    const std::string k = edge_key(expected, e);
    exp_edges.insert(k);
    Tally& by_kind = r.edges_by_kind[std::string(to_string(e.kind))];
    ++by_kind.expected;
    const bool hit = got_edges.count(k) > 0;
    if (hit) ++by_kind.matched;
    else r.missing_edges.push_back(k);
    if (is_hard(e)) {
      ++r.hard_edges.expected;
      if (hit) ++r.hard_edges.matched;
    }
  }
  for (const auto& k : got_edges) {
    if (!exp_edges.count(k)) r.extra_edges.push_back(k);
  }
  r.edges.expected = exp_edges.size();
  r.edges.found = got_edges.size();
  r.edges.matched = r.edges.expected - r.missing_edges.size();
  std::sort(r.missing_edges.begin(), r.missing_edges.end());
  return r;
}

nlohmann::json to_json(const ScoreReport& r) {
  nlohmann::json by_kind = nlohmann::json::object();
  for (const auto& [kind, t] : r.edges_by_kind) by_kind[kind] = tally_json(t);
  return {{"nodes", tally_json(r.nodes)},
          {"edges", tally_json(r.edges)},
          {"edges_by_kind", by_kind},
          {"hard_edges", {{"expected", r.hard_edges.expected},
                          {"matched", r.hard_edges.matched},
                          {"recall", r.hard_edges.recall()}}},
          {"missing_nodes", r.missing_nodes},
          {"extra_nodes", r.extra_nodes},
          {"missing_edges", r.missing_edges},
          {"extra_edges", r.extra_edges}};
}

std::string to_text(const ScoreReport& r) {
  std::string out = "                  expected   found  matched  precision  recall\n";
  out += row("nodes", r.nodes);
  out += row("edges", r.edges);
  for (const auto& [kind, t] : r.edges_by_kind) out += row("  " + kind, t);
  if (r.hard_edges.expected) out += row("  hard (recall)", r.hard_edges, true);
  auto list = [&out](const char* title, const std::vector<std::string>& items) {
    if (items.empty()) return;
    out += std::string("\n") + title + " (" + std::to_string(items.size()) + "):\n";
    for (const auto& i : items) out += "  " + i + "\n";
  };
  list("missing nodes", r.missing_nodes);
  list("extra nodes", r.extra_nodes);
  list("missing edges", r.missing_edges);
  list("extra edges", r.extra_edges);
  return out;
}

}  // namespace optum
