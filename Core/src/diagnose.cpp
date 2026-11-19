#include "optum/diagnose.hpp"

#include <algorithm>
#include <tuple>
#include <map>
#include <set>
#include <sstream>

namespace optum {
namespace {

// The unit each node belongs to: the nearest module or program up the `defines` edges.
std::vector<NodeIndex> units_of(const Graph& g) {
  std::vector<NodeIndex> parent(g.nodes().size());
  for (NodeIndex i = 0; i < parent.size(); ++i) parent[i] = i;
  for (const Edge& e : g.edges()) {
    if (e.kind == EdgeKind::Defines && parent[e.to] == e.to && e.from != e.to) parent[e.to] = e.from;
  }
  std::vector<NodeIndex> unit(g.nodes().size());
  for (NodeIndex i = 0; i < unit.size(); ++i) {
    NodeIndex cur = i;
    for (int steps = 0; steps < 64; ++steps) {
      const NodeKind k = g.node(cur).kind;
      if (k == NodeKind::Module || k == NodeKind::Program) break;
      if (parent[cur] == cur) break;
      cur = parent[cur];
    }
    unit[i] = cur;
  }
  return unit;
}

// The component a unit belongs to: what a person would call one tool or project. Modules of one
// top-level package (py:pkg.a, py:pkg.b) or one top-level directory (js:src/a.js, js:src/b.js) are
// one component; a root-level file, a program, a workflow and a binary are their own.
std::string component_of(const Node& unit) {
  if (unit.kind == NodeKind::Program || unit.frontend == "binary") return unit.id;
  const auto colon = unit.id.find(':');
  if (colon == std::string::npos) return unit.id;
  const std::string ns = unit.id.substr(0, colon + 1), rest = unit.id.substr(colon + 1);
  const char sep = ns == "py:" ? '.' : '/';
  const auto cut = rest.find(sep);
  return cut == std::string::npos ? unit.id : ns + rest.substr(0, cut);
}

// Test code (a test/tests/spec segment, test_*.py, *_test.*, *.test.js, *.spec.ts): it reaches
// what the code under test reaches, which isn't waste.
bool is_test(const std::string& id) {
  const auto colon = id.find(':');
  std::string rest = colon == std::string::npos ? id : id.substr(colon + 1);
  for (char& c : rest) {
    if (c == '.' || c == '/' || c == '#') c = ' ';
  }
  std::istringstream words(rest);
  for (std::string w; words >> w;) {
    if (w == "test" || w == "tests" || w == "testing" || w == "spec" || w == "__tests__" ||
        w.starts_with("test_") || w.ends_with("_test") || w.ends_with("_tests")) {
      return true;
    }
  }
  return false;
}

bool outside(const Node& n) {
  return n.kind == NodeKind::External || n.kind == NodeKind::Store;
}

}  // namespace

Diagnosis diagnose(const Graph& g) {
  Diagnosis d;
  d.source = g.source;
  const auto unit = units_of(g);

  // Units: modules and programs that aren't third-party packages or libraries.
  std::map<std::string, UnitInfo> units;
  for (NodeIndex i = 0; i < g.nodes().size(); ++i) {
    const Node& n = g.node(i);
    if (n.kind != NodeKind::Module && n.kind != NodeKind::Program) continue;
    if (n.attrs.value("third_party", false)) {
      d.packages.push_back(n.id);
      continue;
    }
    UnitInfo u{n.id, n.name.empty() ? n.id : n.name, n.frontend, 0};
    if (n.loc && n.loc->end_line) u.lines = n.loc->end_line;
    units.emplace(n.id, std::move(u));
  }
  std::sort(d.packages.begin(), d.packages.end());

  // What the code reaches outside itself, and who reaches it.
  std::map<std::string, Target> reached;
  for (const Edge& e : g.edges()) {
    if (e.kind != EdgeKind::Reads && e.kind != EdgeKind::Writes && e.kind != EdgeKind::SendsTo) continue;
    const Node& to = g.node(e.to);
    if (!outside(to)) continue;
    Target& t = reached[to.id];
    t.id = to.id;
    t.kind = to.kind == NodeKind::Store ? "store" : "external";
    t.name = to.name.empty() ? to.id : to.name;
    Use use;
    use.unit = component_of(g.node(unit[e.from]));
    use.function = g.node(e.from).id;
    use.kind = std::string(to_string(e.kind));
    use.method = e.attrs.value("method", "");
    use.exact = e.confidence == Confidence::Exact;
    t.uses.push_back(std::move(use));
  }
  for (auto& [id, t] : reached) {
    std::sort(t.uses.begin(), t.uses.end(), [](const Use& a, const Use& b) {
      return std::tie(a.unit, a.function, a.kind) < std::tie(b.unit, b.function, b.kind);
    });
    d.reached.push_back(t);
  }

  // Overlapping glue. Units that reach mostly the same targets do the same job: group them
  // (shared targets >= 2, and >= half of each unit's targets shared). Then single targets
  // reached by several units that aren't part of such a group.
  std::map<std::string, std::set<std::string>> targets_of;  // component -> targets (test code left out)
  for (const Target& t : d.reached) {
    for (const Use& u : t.uses) {
      if (!is_test(u.function)) targets_of[u.unit].insert(t.id);
    }
  }
  std::vector<std::string> unit_ids;
  for (const auto& [u, _] : targets_of) unit_ids.push_back(u);
  std::map<std::string, std::string> group;  // union-find over units
  for (const auto& u : unit_ids) group[u] = u;
  auto find = [&](std::string x) {
    while (group[x] != x) x = group[x] = group[group[x]];
    return x;
  };
  for (std::size_t a = 0; a < unit_ids.size(); ++a) {
    for (std::size_t b = a + 1; b < unit_ids.size(); ++b) {
      const auto& ta = targets_of[unit_ids[a]];
      const auto& tb = targets_of[unit_ids[b]];
      std::size_t shared = 0;
      for (const auto& t : ta) shared += tb.count(t);
      if (shared >= 2 && 2 * shared >= ta.size() && 2 * shared >= tb.size()) {
        group[find(unit_ids[a])] = find(unit_ids[b]);
      }
    }
  }
  std::map<std::string, std::vector<std::string>> groups;
  for (const auto& u : unit_ids) groups[find(u)].push_back(u);
  std::set<std::string> covered;  // targets already reported inside a group finding
  for (auto& [_, members] : groups) {
    if (members.size() < 2) continue;
    std::sort(members.begin(), members.end());
    std::map<std::string, std::size_t> count;
    for (const auto& u : members) {
      for (const auto& t : targets_of[u]) ++count[t];
    }
    Finding f;
    f.kind = "overlapping_glue";
    f.units = members;
    bool exact = true;
    nlohmann::json ev = nlohmann::json::array();
    for (const Target& t : d.reached) {
      if (count[t.id] < 2) continue;
      f.targets.push_back(t.id);
      covered.insert(t.id);
      nlohmann::json who = nlohmann::json::array();
      for (const Use& use : t.uses) {
        if (!std::binary_search(members.begin(), members.end(), use.unit) || is_test(use.function)) continue;
        exact = exact && use.exact;
        who.push_back({{"unit", use.unit}, {"function", use.function}, {"kind", use.kind}, {"method", use.method}});
      }
      ev.push_back({{"target", t.id}, {"uses", who}});
    }
    f.confidence = exact ? "certain" : "likely";
    f.title = std::to_string(members.size()) + " tools do the same job: they reach the same " +
              std::to_string(f.targets.size()) + " outside systems";
    f.evidence = {{"targets", ev}};
    d.findings.push_back(std::move(f));
  }
  for (const Target& t : d.reached) {
    if (covered.count(t.id)) continue;
    std::set<std::string> who;
    bool exact = true;
    for (const Use& u : t.uses) {
      if (is_test(u.function)) continue;
      who.insert(u.unit);
      exact = exact && u.exact;
    }
    if (who.size() < 2) continue;
    Finding f;
    f.kind = "overlapping_glue";
    f.units.assign(who.begin(), who.end());
    f.targets = {t.id};
    f.confidence = exact ? "certain" : "likely";
    f.title = std::to_string(who.size()) + " tools reach " + t.id;
    nlohmann::json uses = nlohmann::json::array();
    for (const Use& u : t.uses) {
      if (is_test(u.function)) continue;
      uses.push_back({{"unit", u.unit}, {"function", u.function}, {"kind", u.kind}, {"method", u.method}});
    }
    f.evidence = {{"targets", nlohmann::json::array({{{"target", t.id}, {"uses", uses}}})}};
    d.findings.push_back(std::move(f));
  }
  // Surest first (docs/plans/phase2.md), then strongest: more units, then more targets.
  auto rank = [](const std::string& c) { return c == "certain" ? 2 : c == "likely" ? 1 : 0; };
  std::stable_sort(d.findings.begin(), d.findings.end(), [&](const Finding& a, const Finding& b) {
    return std::make_tuple(rank(a.confidence), a.units.size(), a.targets.size()) >
           std::make_tuple(rank(b.confidence), b.units.size(), b.targets.size());
  });

  for (auto& [id, u] : units) d.units.push_back(std::move(u));
  return d;
}

nlohmann::json to_json(const Diagnosis& d) {
  nlohmann::json units = nlohmann::json::array();
  for (const auto& u : d.units) units.push_back({{"id", u.id}, {"name", u.name}, {"frontend", u.frontend}, {"lines", u.lines}});
  nlohmann::json reached = nlohmann::json::array();
  for (const auto& t : d.reached) {
    nlohmann::json uses = nlohmann::json::array();
    for (const auto& u : t.uses) {
      uses.push_back({{"unit", u.unit}, {"function", u.function}, {"kind", u.kind}, {"method", u.method}, {"exact", u.exact}});
    }
    reached.push_back({{"id", t.id}, {"kind", t.kind}, {"name", t.name}, {"uses", uses}});
  }
  nlohmann::json findings = nlohmann::json::array();
  for (const auto& f : d.findings) {
    findings.push_back({{"kind", f.kind}, {"title", f.title}, {"confidence", f.confidence}, {"units", f.units},
                        {"targets", f.targets}, {"evidence", f.evidence}});
  }
  return {{"report_version", "0.1"}, {"source", d.source}, {"units", units},
          {"reached", reached}, {"packages", d.packages}, {"findings", findings}};
}

std::string to_markdown(const Diagnosis& d) {
  std::string o = "# Optum diagnosis\n\nSource: `" + d.source + "`\n\n";
  std::map<std::string, std::size_t> by_frontend;
  for (const auto& u : d.units) ++by_frontend[u.frontend.empty() ? "?" : u.frontend];
  std::size_t apis = 0, stores = 0;
  for (const auto& t : d.reached) (t.kind == "store" ? stores : apis) += 1;
  o += "## Summary\n\n| | |\n|---|---|\n";
  o += "| Units (modules, programs, workflows) | " + std::to_string(d.units.size());
  std::string parts;
  for (const auto& [fe, n] : by_frontend) parts += (parts.empty() ? "" : ", ") + std::to_string(n) + " " + fe;
  o += parts.empty() ? " |\n" : " (" + parts + ") |\n";
  o += "| Outside APIs and services reached | " + std::to_string(apis) + " |\n";
  o += "| Tables reached | " + std::to_string(stores) + " |\n";
  o += "| Third-party packages and libraries | " + std::to_string(d.packages.size()) + " |\n";
  o += "| Findings | " + std::to_string(d.findings.size()) + " |\n\n";

  o += "## Findings\n\n";
  if (d.findings.empty()) o += "None.\n\n";
  int k = 0;
  for (const auto& f : d.findings) {
    o += "### " + std::to_string(++k) + ". " + f.title + "\n\n";
    o += "Kind: overlapping glue. Confidence: **" + f.confidence + "**.\n\n";
    o += "| Outside system |";
    for (const auto& u : f.units) o += " `" + u + "` |";
    o += "\n|---|";
    for (std::size_t i = 0; i < f.units.size(); ++i) o += "---|";
    o += "\n";
    for (const auto& tj : f.evidence["targets"]) {
      o += "| `" + tj["target"].get<std::string>() + "` |";
      for (const auto& u : f.units) {
        std::string cell;
        for (const auto& use : tj["uses"]) {
          if (use["unit"] != u) continue;
          std::string how = use["kind"].get<std::string>();
          if (!use["method"].get<std::string>().empty()) how = use["method"].get<std::string>();
          if (cell.find(how) == std::string::npos) cell += (cell.empty() ? "" : ", ") + how;
        }
        o += " " + (cell.empty() ? std::string("–") : cell) + " |";
      }
      o += "\n";
    }
    o += "\n";
  }

  o += "## What the code reaches\n\n";
  if (d.reached.empty()) o += "Nothing outside itself that Optum recognises.\n\n";
  else o += "| Outside system | Kind | Used by |\n|---|---|---|\n";
  for (const auto& t : d.reached) {
    std::set<std::string> who;
    for (const auto& u : t.uses) who.insert(u.unit);
    std::string list;
    for (const auto& w : who) list += (list.empty() ? "" : ", ") + std::string("`") + w + "`";
    o += "| `" + t.id + "` | " + (t.kind == "store" ? "table" : "API / service") + " | " + list + " |\n";
  }
  if (!d.packages.empty()) {
    o += "\nThird-party packages and libraries: ";
    for (std::size_t i = 0; i < d.packages.size(); ++i) o += (i ? ", " : "") + std::string("`") + d.packages[i] + "`";
    o += "\n";
  }
  return o;
}

}  // namespace optum
