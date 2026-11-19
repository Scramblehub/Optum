#pragma once
// Duplicated logic: functions that are copies of each other, found by CGL's structure memory on
// source code (docs/diagnose.md, "Duplicated logic"). Each function's syntax subtrees, with typed
// holes for names and literals, are counted across the codebase; two functions are clones when
// they share most of their structure, weighted by how rare each piece is.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "optum/ir.hpp"

namespace optum {

struct CloneOptions {
  std::uint32_t min_tokens = 50;   // smaller functions are ignored (getters, small wrappers)
  std::uint32_t min_lines = 5;
  std::size_t min_distinct = 10;   // distinct structure pieces: boilerplate (self.x = x ...) has few
  // Weighted overlap of structure for edited copies. 0.70 is the usual line for near-miss clones
  // (NiCad's 30% difference); the planted samples' look-alikes score under 0.10.
  double min_similarity = 0.70;
  std::size_t max_group = 50;      // larger groups are cut (a pattern, not a copy)
};

struct CloneMember {
  std::string id;    // the function node
  std::string file;  // as in the node's location
  std::uint32_t line = 0, lines = 0, tokens = 0;
  std::string area;        // "" for the project's own code, else vendored / build / example (code_area)
  bool generated = false;  // its file says it's generated
};

struct CloneGroup {
  // "exact": the same code (comments and names of the functions aside); "renamed": the same code
  // with different identifiers or literals; "similar": the same structure with small edits.
  std::string type;
  double similarity = 1.0;  // the lowest between two members
  std::vector<CloneMember> members;  // sorted by id
};

// Clone groups among the graph's Python, JS and C functions, read from their source files under
// `root` (the analysed directory). Test code is left out. Functions whose file can't be read are
// skipped. Groups are sorted by lines that merging would save, most first.
std::vector<CloneGroup> find_clones(const Graph& g, const std::filesystem::path& root,
                                    const CloneOptions& opts = {});

}  // namespace optum
