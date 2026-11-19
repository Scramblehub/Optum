#pragma once
// Views: smaller graphs cut from a big one, for people (diagrams) and agents (JSON) alike.
// A view is itself a valid Graph, so every exporter and command works on it.

#include <string>
#include <string_view>
#include <vector>

#include "optum/ir.hpp"

namespace optum {

enum class Level { Function, Module };

struct ViewOptions {
  Level level = Level::Function;  // Module: collapse everything into its module
  std::string focus;              // node id (or unique id suffix); empty = whole graph
  int depth = 1;                  // with focus: how many edges away to include
  std::vector<EdgeKind> edge_kinds;  // empty = all kinds; `defines` is always kept
  bool hide_inferred = false;        // drop edges with confidence "inferred"

  bool is_identity() const {
    return level == Level::Function && focus.empty() && edge_kinds.empty() && !hide_inferred;
  }
};

// Finds a node from what a person typed: the exact id, else the one id ending in it (after a
// '.', '#', ':' or '/'). Throws IrError naming candidates if none or several match.
NodeIndex resolve_node(const Graph& g, std::string_view ref);

// The module a node belongs to (following `defines` edges up), or the node itself if it isn't
// inside one (externals, stores, third-party packages, modules).
NodeIndex module_of(const Graph& g, NodeIndex n);

Graph make_view(const Graph& g, const ViewOptions& o);

}  // namespace optum
