#include "optum/view.hpp"

#include <algorithm>
#include <map>
#include <tuple>

namespace optum {
namespace {

// parent[n] = the node that `defines` n, or n itself.
std::vector<NodeIndex> parents(const Graph& g) {
  std::vector<NodeIndex> p(g.nodes().size());
  for (NodeIndex i = 0; i < p.size(); ++i) p[i] = i;
  for (const Edge& e : g.edges()) {
    if (e.kind == EdgeKind::Defines && p[e.to] == e.to && e.from != e.to) p[e.to] = e.from;
  }
  return p;
}

NodeIndex module_with(const Graph& g, const std::vector<NodeIndex>& parent, NodeIndex n) {
  for (NodeIndex cur = n, steps = 0; steps < 64; ++steps) {
    if (g.node(cur).kind == NodeKind::Module) return cur;
    if (parent[cur] == cur) return n;
    cur = parent[cur];
  }
  return n;
}

// Copies the nodes marked in `keep` and the edges between them.
Graph subgraph(const Graph& g, const std::vector<bool>& keep) {
  Graph out;
  out.source = g.source;
  std::vector<NodeIndex> remap(g.nodes().size());
  for (NodeIndex i = 0; i < g.nodes().size(); ++i) {
    if (keep[i]) remap[i] = out.add_node(g.node(i));
  }
  for (const Edge& e : g.edges()) {
    if (!keep[e.from] || !keep[e.to]) continue;
    Edge c = e;
    c.from = remap[e.from];
    c.to = remap[e.to];
    out.add_edge(std::move(c));
  }
  return out;
}

Graph filter_edges(const Graph& g, const ViewOptions& o) {
  Graph out;
  out.source = g.source;
  for (const Node& n : g.nodes()) out.add_node(n);  // same order: indices match
  for (const Edge& e : g.edges()) {
    if (e.kind != EdgeKind::Defines) {
      if (o.hide_inferred && e.confidence == Confidence::Inferred) continue;
      if (!o.edge_kinds.empty() &&
          std::find(o.edge_kinds.begin(), o.edge_kinds.end(), e.kind) == o.edge_kinds.end()) {
        continue;
      }
    }
    out.add_edge(e);
  }
  return out;
}

Graph collapse_to_modules(const Graph& g) {
  const auto parent = parents(g);
  Graph out;
  out.source = g.source;
  std::vector<NodeIndex> rep(g.nodes().size());
  for (NodeIndex i = 0; i < g.nodes().size(); ++i) {
    rep[i] = out.add_node(g.node(module_with(g, parent, i)));
  }
  struct Agg {
    std::size_t count = 0;
    Confidence confidence = Confidence::Inferred;
  };
  std::map<std::tuple<NodeIndex, NodeIndex, EdgeKind>, Agg> agg;
  for (const Edge& e : g.edges()) {
    const NodeIndex a = rep[e.from], b = rep[e.to];
    if (a == b) continue;
    if (e.kind == EdgeKind::Defines &&
        !(out.node(a).kind == NodeKind::Module && out.node(b).kind == NodeKind::Module)) {
      continue;
    }
    Agg& x = agg[{a, b, e.kind}];
    ++x.count;
    x.confidence = std::max(x.confidence, e.confidence);
  }
  for (const auto& [key, x] : agg) {
    Edge e;
    std::tie(e.from, e.to, e.kind) = key;
    e.confidence = x.confidence;
    e.attrs = {{"count", x.count}};
    out.add_edge(std::move(e));
  }
  return out;
}

Graph neighbourhood(const Graph& g, NodeIndex focus, int depth) {
  std::vector<std::vector<NodeIndex>> adj(g.nodes().size());
  for (const Edge& e : g.edges()) {
    adj[e.from].push_back(e.to);
    adj[e.to].push_back(e.from);
  }
  std::vector<bool> keep(g.nodes().size(), false);
  std::vector<NodeIndex> frontier{focus};
  keep[focus] = true;
  for (int d = 0; d < depth && !frontier.empty(); ++d) {
    std::vector<NodeIndex> next;
    for (NodeIndex n : frontier) {
      for (NodeIndex m : adj[n]) {
        if (!keep[m]) {
          keep[m] = true;
          next.push_back(m);
        }
      }
    }
    frontier = std::move(next);
  }
  // Keep the containers of everything shown, so diagrams can still group by module.
  const auto parent = parents(g);
  for (NodeIndex i = 0; i < keep.size(); ++i) {
    if (!keep[i]) continue;
    for (NodeIndex cur = i, steps = 0; parent[cur] != cur && steps < 64; ++steps) {
      cur = parent[cur];
      keep[cur] = true;
    }
  }
  return subgraph(g, keep);
}

}  // namespace

NodeIndex resolve_node(const Graph& g, std::string_view ref) {
  if (auto exact = g.find(ref)) return *exact;
  std::vector<NodeIndex> hits;
  for (NodeIndex i = 0; i < g.nodes().size(); ++i) {
    const std::string& id = g.node(i).id;
    if (id.size() > ref.size() && id.ends_with(ref)) {
      const char before = id[id.size() - ref.size() - 1];
      if (before == '.' || before == '#' || before == ':' || before == '/') hits.push_back(i);
    }
  }
  if (hits.size() == 1) return hits[0];
  std::string msg = "no node matches '" + std::string(ref) + "'";
  if (hits.empty()) {
    for (NodeIndex i = 0; i < g.nodes().size() && hits.size() < 8; ++i) {
      if (g.node(i).id.find(ref) != std::string::npos) hits.push_back(i);
    }
    if (!hits.empty()) msg += "; ids containing it:";
  } else {
    msg = "'" + std::string(ref) + "' matches " + std::to_string(hits.size()) + " nodes:";
    if (hits.size() > 8) hits.resize(8);
  }
  for (NodeIndex i : hits) msg += "\n  " + g.node(i).id;
  throw IrError(msg);
}

NodeIndex module_of(const Graph& g, NodeIndex n) { return module_with(g, parents(g), n); }

Graph make_view(const Graph& g, const ViewOptions& o) {
  Graph v = filter_edges(g, o);
  if (o.level == Level::Module) v = collapse_to_modules(v);
  if (!o.focus.empty()) v = neighbourhood(v, resolve_node(v, o.focus), std::max(o.depth, 0));
  return v;
}

}  // namespace optum
