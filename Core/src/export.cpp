#include "optum/export.hpp"

#include <map>
#include <set>

#include "optum/view.hpp"

namespace optum {
namespace {

// Validated with the dataviz palette checks (light surface, all pairs): blue, orange, aqua.
constexpr const char* kBlue = "#2a78d6";
constexpr const char* kOrange = "#eb6834";
constexpr const char* kAqua = "#1baf7a";
constexpr const char* kGray = "#8a8984";
constexpr const char* kInk = "#0b0b0b";
constexpr const char* kInk2 = "#52514e";
constexpr const char* kSurface = "#fcfcfb";
constexpr const char* kClusterLine = "#c3c2b7";

struct EdgeStyle {
  const char* color;
  bool bold;
  bool dashed;
  const char* label;  // shown on the edge; "" for calls and imports
  const char* legend;
};

EdgeStyle style_of(EdgeKind k) {
  switch (k) {
    case EdgeKind::Calls: return {kBlue, false, false, "", "calls"};
    case EdgeKind::Triggers: return {kBlue, true, false, "triggers", "triggers"};
    case EdgeKind::Reads: return {kAqua, false, false, "reads", "reads data from"};
    case EdgeKind::Writes: return {kOrange, false, false, "writes", "writes data to"};
    case EdgeKind::SendsTo: return {kOrange, true, false, "sends", "sends data out to"};
    case EdgeKind::Imports: return {kGray, false, true, "", "imports"};
    case EdgeKind::Defines: return {kGray, false, false, "", "contains"};
  }
  return {kGray, false, false, "", ""};
}

const char* dot_shape(NodeKind k) {
  switch (k) {
    case NodeKind::Program: return "box3d";
    case NodeKind::Module: return "folder";
    case NodeKind::Function: return "box";
    case NodeKind::DataType: return "box";
    case NodeKind::Store: return "cylinder";
    case NodeKind::External: return "hexagon";
    case NodeKind::Step: return "cds";
  }
  return "box";
}

const char* shape_name(NodeKind k) {
  switch (k) {
    case NodeKind::Program: return "3-D box: program";
    case NodeKind::Module: return "folder: module";
    case NodeKind::Function: return "rounded box: function";
    case NodeKind::DataType: return "double box: class / data type";
    case NodeKind::Store: return "cylinder: data store";
    case NodeKind::External: return "hexagon: external API";
    case NodeKind::Step: return "arrow: workflow step";
  }
  return "";
}

// Escapes a string for use inside a double-quoted DOT string.
std::string escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

// What to print on a node: the part of its id after its module, e.g. "Shape.area".
std::string label_of(const Graph& g, NodeIndex i, NodeIndex mod) {
  const Node& n = g.node(i);
  if (mod != i) {
    const std::string& mid = g.node(mod).id;
    if (n.id.size() > mid.size() + 1 && n.id.starts_with(mid) &&
        (n.id[mid.size()] == '.' || n.id[mid.size()] == '#')) {
      return n.id.substr(mid.size() + 1);
    }
  }
  if (n.kind == NodeKind::Module) {
    const auto colon = n.id.find(':');
    std::string name = colon == std::string::npos ? n.id : n.id.substr(colon + 1);
    if (n.attrs.value("third_party", false)) name += " (3rd party)";
    return name;
  }
  return n.name.empty() ? n.id : n.name;
}

std::string tooltip_of(const Node& n) {
  std::string t = n.id;
  if (n.loc) {
    t += "  " + n.loc->file;
    if (n.loc->line) t += ":" + std::to_string(n.loc->line);
  }
  return t;
}

struct Layout {
  std::vector<NodeIndex> module;                   // per node
  std::map<NodeIndex, std::vector<NodeIndex>> clusters;  // module -> members (incl. itself)
  std::vector<NodeIndex> loose;                    // not in any cluster
};

Layout layout(const Graph& g) {
  Layout l;
  l.module.resize(g.nodes().size());
  std::set<NodeIndex> has_members;
  for (NodeIndex i = 0; i < g.nodes().size(); ++i) {
    l.module[i] = module_of(g, i);
    if (l.module[i] != i) has_members.insert(l.module[i]);
  }
  for (NodeIndex i = 0; i < g.nodes().size(); ++i) {
    const NodeIndex m = l.module[i];
    if (has_members.count(m)) l.clusters[m].push_back(i);
    else l.loose.push_back(i);
  }
  return l;
}

std::string legend_dot(const Graph& g) {
  std::set<EdgeKind> kinds;
  std::set<NodeKind> shapes;
  bool inferred = false;
  for (const Edge& e : g.edges()) {
    if (e.kind == EdgeKind::Defines) continue;
    kinds.insert(e.kind);
    inferred = inferred || e.confidence == Confidence::Inferred;
  }
  for (const Node& n : g.nodes()) shapes.insert(n.kind);
  std::string rows;
  for (EdgeKind k : kinds) {
    const EdgeStyle s = style_of(k);
    rows += std::string("<tr><td align=\"left\"><font color=\"") + s.color + "\">" +
            (s.dashed ? "- - -" : s.bold ? "&#9473;&#9473;&#9473;" : "&#9472;&#9472;&#9472;") +
            "</font></td><td align=\"left\">" + s.legend + "</td></tr>";
  }
  if (inferred) {
    rows += "<tr><td align=\"left\">&#183;&#183;&#183;&#183;</td><td align=\"left\">dotted: inferred (a likely edge, not certain)</td></tr>";
  }
  for (NodeKind k : shapes) {
    rows += std::string("<tr><td></td><td align=\"left\">") + shape_name(k) + "</td></tr>";
  }
  return "  \"optum_legend\" [shape=plaintext, fillcolor=\"" + std::string(kSurface) +
         "\", label=<<table border=\"0\" cellspacing=\"2\" cellpadding=\"1\"><tr><td colspan=\"2\" "
         "align=\"left\"><b>Legend</b></td></tr>" + rows + "</table>>];\n";
}

}  // namespace

std::string to_dot(const Graph& g) {
  const Layout l = layout(g);
  std::string out = "digraph optum {\n";
  out += "  graph [rankdir=LR, fontname=\"Helvetica\", fontsize=11, bgcolor=\"" + std::string(kSurface) +
         "\", nodesep=0.25, ranksep=0.7, labelloc=t, labeljust=l, fontcolor=\"" + kInk2 +
         "\", label=\"" + escape(g.source) + "\"];\n";
  out += "  node [fontname=\"Helvetica\", fontsize=10, color=\"" + std::string(kInk2) +
         "\", fontcolor=\"" + kInk + "\", style=\"filled\", fillcolor=\"#ffffff\", penwidth=1];\n";
  out += "  edge [fontname=\"Helvetica\", fontsize=9, fontcolor=\"" + std::string(kInk2) +
         "\", arrowsize=0.6, penwidth=1.2];\n";

  auto node_line = [&](NodeIndex i, const std::string& indent) {
    const Node& n = g.node(i);
    std::string style = "filled";
    if (n.kind == NodeKind::Function) style += ",rounded";
    if (n.confidence == Confidence::Inferred) style += ",dashed";
    std::string line = indent + "\"" + escape(n.id) + "\" [label=\"" +
                       escape(label_of(g, i, l.module[i])) + "\", shape=" + dot_shape(n.kind) +
                       ", style=\"" + style + "\", tooltip=\"" + escape(tooltip_of(n)) + "\"";
    if (n.kind == NodeKind::DataType) line += ", peripheries=2";
    return line + "];\n";
  };

  int c = 0;
  for (const auto& [mod, members] : l.clusters) {
    out += "  subgraph \"cluster_" + std::to_string(c++) + "\" {\n";
    out += "    label=\"" + escape(label_of(g, mod, mod)) + "\"; style=\"rounded\"; color=\"" +
           kClusterLine + "\"; fontcolor=\"" + kInk2 + "\";\n";
    for (NodeIndex i : members) out += node_line(i, "    ");
    out += "  }\n";
  }
  for (NodeIndex i : l.loose) out += node_line(i, "  ");

  for (const Edge& e : g.edges()) {
    if (e.kind == EdgeKind::Defines) continue;
    const EdgeStyle s = style_of(e.kind);
    std::string attrs = std::string("color=\"") + s.color + "\"";
    std::string label = s.label;
    if (e.attrs.contains("count") && e.attrs["count"].is_number() && e.attrs["count"].get<int>() > 1) {
      label += (label.empty() ? "" : " ") + std::string("×") + std::to_string(e.attrs["count"].get<int>());
    }
    if (!label.empty()) attrs += ", label=\"" + escape(label) + "\"";
    if (e.confidence == Confidence::Inferred) attrs += ", style=dotted";
    else if (s.dashed) attrs += ", style=dashed";
    if (s.bold) attrs += ", penwidth=2.4";
    out += "  \"" + escape(g.node(e.from).id) + "\" -> \"" + escape(g.node(e.to).id) +
           "\" [" + attrs + "];\n";
  }
  if (!g.nodes().empty()) out += legend_dot(g);
  out += "}\n";
  return out;
}

std::string to_mermaid(const Graph& g) {
  const Layout l = layout(g);
  auto q = [](const std::string& s) {
    std::string out;
    for (char ch : s) out += ch == '"' ? std::string("#quot;") : std::string(1, ch);
    return "\"" + out + "\"";
  };
  auto shape = [&](NodeIndex i) {
    const std::string t = q(label_of(g, i, l.module[i]));
    switch (g.node(i).kind) {
      case NodeKind::Function: return "(" + t + ")";
      case NodeKind::DataType: return "[[" + t + "]]";
      case NodeKind::Store: return "[(" + t + ")]";
      case NodeKind::External: return "{{" + t + "}}";
      case NodeKind::Step: return ">" + t + "]";
      case NodeKind::Program: return "[/" + t + "/]";
      case NodeKind::Module: return "[" + t + "]";
    }
    return "[" + t + "]";
  };
  auto nid = [](NodeIndex i) { return "n" + std::to_string(i); };

  std::string out = "flowchart LR\n";
  int c = 0;
  for (const auto& [mod, members] : l.clusters) {
    out += "  subgraph c" + std::to_string(c++) + "[" + q(label_of(g, mod, mod)) + "]\n";
    for (NodeIndex i : members) out += "    " + nid(i) + shape(i) + "\n";
    out += "  end\n";
  }
  for (NodeIndex i : l.loose) out += "  " + nid(i) + shape(i) + "\n";

  std::string styles;
  int link = 0;
  std::set<EdgeKind> kinds;
  for (const Edge& e : g.edges()) {
    if (e.kind == EdgeKind::Defines) continue;
    kinds.insert(e.kind);
    const EdgeStyle s = style_of(e.kind);
    const char* arrow = e.confidence == Confidence::Inferred ? "-.->" : s.bold ? "==>" : "-->";
    out += "  " + nid(e.from) + " " + arrow + (s.label[0] ? std::string("|") + s.label + "|" : "") +
           " " + nid(e.to) + "\n";
    styles += "  linkStyle " + std::to_string(link++) + " stroke:" + s.color +
              (s.dashed ? ",stroke-dasharray:4 3" : "") + "\n";
  }
  // Legend: one sample edge per kind present.
  if (!kinds.empty()) {
    out += "  subgraph legend[\"Legend\"]\n    direction LR\n";
    int k = 0;
    for (EdgeKind kind : kinds) {
      const EdgeStyle s = style_of(kind);
      const std::string a = "lg" + std::to_string(k) + "a", b = "lg" + std::to_string(k) + "b";
      out += "    " + a + "[\" \"] " + (s.bold ? "==>" : "-->") + "|" + s.legend + "| " + b + "[\" \"]\n";
      styles += "  linkStyle " + std::to_string(link++) + " stroke:" + s.color +
                (s.dashed ? ",stroke-dasharray:4 3" : "") + "\n";
      ++k;
    }
    out += "  end\n";
  }
  return out + styles;
}

}  // namespace optum
