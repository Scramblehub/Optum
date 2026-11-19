#pragma once
// Compares a graph a frontend produced against a hand-checked expected graph. See docs/frontends.md.

#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "optum/ir.hpp"

namespace optum {

struct Tally {
  std::size_t expected = 0;
  std::size_t found = 0;    // in the produced graph
  std::size_t matched = 0;  // in both

  // Nothing claimed is nothing wrong: precision is 1 when found is 0; same for recall.
  double precision() const { return found ? double(matched) / double(found) : 1.0; }
  double recall() const { return expected ? double(matched) / double(expected) : 1.0; }
};

struct ScoreReport {
  Tally nodes;  // matched on (id, kind)
  Tally edges;  // matched on (from, to, kind)
  std::map<std::string, Tally> edges_by_kind;
  Tally hard_edges;  // expected edges marked attrs.difficulty = "hard"; only recall is meaningful
  std::vector<std::string> missing_nodes, extra_nodes, missing_edges, extra_edges;
};

ScoreReport score(const Graph& expected, const Graph& produced);
nlohmann::json to_json(const ScoreReport& r);
std::string to_text(const ScoreReport& r);

}  // namespace optum
