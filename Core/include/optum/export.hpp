#pragma once
// Diagram exporters. Styling (see docs/diagrams.md): node kind -> shape; edge kind -> one of
// three validated colors plus line style and a text label, so meaning never relies on color.

#include <string>

#include "optum/ir.hpp"

namespace optum {

// Graphviz DOT. Nodes are grouped into one box per module. Render with `dot -Tsvg`.
std::string to_dot(const Graph& g);

// Mermaid flowchart, for markdown (GitHub renders it). Best under a few hundred nodes.
std::string to_mermaid(const Graph& g);

}  // namespace optum
