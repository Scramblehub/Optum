#pragma once
// The system graph (IR): the one format every frontend writes. See docs/ir.md.

#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace optum {

enum class NodeKind : std::uint8_t { Program, Module, Function, DataType, Store, External, Step };
enum class EdgeKind : std::uint8_t { Calls, Imports, Defines, Reads, Writes, SendsTo, Triggers };
// Ordered weakest to strongest; merging keeps the stronger one.
enum class Confidence : std::uint8_t { Inferred, Observed, Exact };

std::string_view to_string(NodeKind k);
std::string_view to_string(EdgeKind k);
std::string_view to_string(Confidence c);
std::optional<NodeKind> node_kind_from_string(std::string_view s);
std::optional<EdgeKind> edge_kind_from_string(std::string_view s);
std::optional<Confidence> confidence_from_string(std::string_view s);

// Where something came from. line 0 means unknown; address is set for binaries.
struct SourceLoc {
  std::string file;
  std::uint32_t line = 0;
  std::uint32_t end_line = 0;
  std::optional<std::uint64_t> address;

  bool operator==(const SourceLoc&) const = default;
};

struct Node {
  std::string id;  // stable and unique within a graph, e.g. "py:pkg.mod.func"
  NodeKind kind = NodeKind::Function;
  std::string name;  // short display name
  std::optional<SourceLoc> loc;
  std::string frontend;  // which frontend produced it, e.g. "python"
  Confidence confidence = Confidence::Exact;
  nlohmann::json attrs = nlohmann::json::object();  // frontend-specific extras
};

using NodeIndex = std::uint32_t;

struct Edge {
  NodeIndex from = 0;
  NodeIndex to = 0;
  EdgeKind kind = EdgeKind::Calls;
  std::optional<SourceLoc> loc;
  std::string frontend;
  Confidence confidence = Confidence::Exact;
  nlohmann::json attrs = nlohmann::json::object();
};

class IrError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class Graph {
 public:
  std::string source;  // what this graph describes, e.g. a path or repo URL

  // Adds a node. If a node with the same id exists, merges into it (see docs/ir.md, "Merging")
  // and returns the existing index. Throws IrError if the kinds differ or the id is empty.
  NodeIndex add_node(Node node);

  // Adds an edge between existing nodes. An edge with the same (from, to, kind) is merged, not
  // duplicated. Throws IrError if either index is out of range.
  void add_edge(Edge edge);

  // Convenience: adds an edge between nodes given by id. Throws IrError if either id is unknown.
  void add_edge(std::string_view from_id, std::string_view to_id, EdgeKind kind);

  std::optional<NodeIndex> find(std::string_view id) const;
  const Node& node(NodeIndex i) const { return nodes_.at(i); }
  const std::vector<Node>& nodes() const { return nodes_; }
  const std::vector<Edge>& edges() const { return edges_; }

  // Adds every node and edge of `other` into this graph, merging by node id.
  void merge(const Graph& other);

 private:
  struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
  };
  struct EdgeKey {
    NodeIndex from;
    NodeIndex to;
    EdgeKind kind;
    bool operator==(const EdgeKey&) const = default;
  };
  struct EdgeKeyHash {
    std::size_t operator()(const EdgeKey& k) const {
      const std::uint64_t packed = (std::uint64_t{k.from} << 32) | k.to;
      return std::hash<std::uint64_t>{}(packed) ^
             (std::size_t{static_cast<std::uint8_t>(k.kind)} * 0x9e3779b97f4a7c15ULL);
    }
  };

  std::vector<Node> nodes_;
  std::vector<Edge> edges_;
  std::unordered_map<std::string, NodeIndex, StringHash, std::equal_to<>> node_index_;
  std::unordered_map<EdgeKey, std::size_t, EdgeKeyHash> edge_index_;
};

// JSON exchange format. graph_from_json validates strictly and throws IrError with the path of
// the first problem, e.g. "nodes[3].kind: unknown node kind 'fn'".
nlohmann::json to_json(const Graph& g);
Graph graph_from_json(const nlohmann::json& j);

// Reads and parses a JSON graph file. Throws IrError on I/O, parse or validation errors.
Graph load_graph(const std::string& path);

}  // namespace optum
