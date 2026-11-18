#include "optum/ir.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace optum {
namespace {

constexpr std::array<std::string_view, 7> kNodeKinds = {
    "program", "module", "function", "data_type", "store", "external", "step"};
constexpr std::array<std::string_view, 7> kEdgeKinds = {
    "calls", "imports", "defines", "reads", "writes", "sends_to", "triggers"};
constexpr std::array<std::string_view, 3> kConfidences = {"inferred", "observed", "exact"};

template <typename Enum, std::size_t N>
std::optional<Enum> lookup(const std::array<std::string_view, N>& names, std::string_view s) {
  for (std::size_t i = 0; i < N; ++i) {
    if (names[i] == s) return static_cast<Enum>(i);
  }
  return std::nullopt;
}

// Shared merge rules for nodes and edges (docs/ir.md, "Merging").
template <typename T>
void merge_common(T& into, T&& from) {
  if (!into.loc && from.loc) into.loc = std::move(from.loc);
  if (into.frontend.empty()) into.frontend = std::move(from.frontend);
  into.confidence = std::max(into.confidence, from.confidence);
  into.attrs.update(from.attrs);  // later values win, key by key
}

}  // namespace

std::string_view to_string(NodeKind k) { return kNodeKinds.at(static_cast<std::size_t>(k)); }
std::string_view to_string(EdgeKind k) { return kEdgeKinds.at(static_cast<std::size_t>(k)); }
std::string_view to_string(Confidence c) { return kConfidences.at(static_cast<std::size_t>(c)); }
std::optional<NodeKind> node_kind_from_string(std::string_view s) {
  return lookup<NodeKind>(kNodeKinds, s);
}
std::optional<EdgeKind> edge_kind_from_string(std::string_view s) {
  return lookup<EdgeKind>(kEdgeKinds, s);
}
std::optional<Confidence> confidence_from_string(std::string_view s) {
  return lookup<Confidence>(kConfidences, s);
}

NodeIndex Graph::add_node(Node node) {
  if (node.id.empty()) throw IrError("node id must not be empty");
  if (auto it = node_index_.find(node.id); it != node_index_.end()) {
    Node& existing = nodes_[it->second];
    if (existing.kind != node.kind) {
      throw IrError("node '" + node.id + "' is a " + std::string(to_string(existing.kind)) +
                    ", cannot merge a " + std::string(to_string(node.kind)));
    }
    if (existing.name.empty()) existing.name = std::move(node.name);
    merge_common(existing, std::move(node));
    return it->second;
  }
  if (nodes_.size() >= std::numeric_limits<NodeIndex>::max()) throw IrError("graph too large");
  const auto idx = static_cast<NodeIndex>(nodes_.size());
  node_index_.emplace(node.id, idx);
  nodes_.push_back(std::move(node));
  return idx;
}

void Graph::add_edge(Edge edge) {
  if (edge.from >= nodes_.size() || edge.to >= nodes_.size()) {
    throw IrError("edge refers to a node index out of range");
  }
  const EdgeKey key{edge.from, edge.to, edge.kind};
  if (auto it = edge_index_.find(key); it != edge_index_.end()) {
    merge_common(edges_[it->second], std::move(edge));
    return;
  }
  edge_index_.emplace(key, edges_.size());
  edges_.push_back(std::move(edge));
}

void Graph::add_edge(std::string_view from_id, std::string_view to_id, EdgeKind kind) {
  const auto from = find(from_id);
  const auto to = find(to_id);
  if (!from) throw IrError("unknown node id '" + std::string(from_id) + "'");
  if (!to) throw IrError("unknown node id '" + std::string(to_id) + "'");
  Edge e;
  e.from = *from;
  e.to = *to;
  e.kind = kind;
  add_edge(std::move(e));
}

std::optional<NodeIndex> Graph::find(std::string_view id) const {
  if (auto it = node_index_.find(id); it != node_index_.end()) return it->second;
  return std::nullopt;
}

void Graph::merge(const Graph& other) {
  // Indices differ between graphs: map other's indices to ours.
  std::vector<NodeIndex> remap;
  remap.reserve(other.nodes_.size());
  for (const Node& n : other.nodes_) remap.push_back(add_node(n));
  for (Edge e : other.edges_) {
    e.from = remap[e.from];
    e.to = remap[e.to];
    add_edge(std::move(e));
  }
}

}  // namespace optum
