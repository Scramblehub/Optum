#include "optum/diagnose.hpp"

#include "optum/clones.hpp"
#include "optum/entities.hpp"
#include "test_code.hpp"

#include <algorithm>
#include <cmath>
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

// Test code reaches what the code under test reaches, which isn't waste.
bool is_test(const std::string& id) { return is_test_code(id); }

// Unused code (docs/diagnose.md). Names a framework or dispatch mechanism calls by name, unseen
// by the graph: they're never reported (the list follows Magellan's).
bool called_by_name(const std::string& name, bool method) {
  if (name.starts_with("__") && name.ends_with("__")) return true;  // Python dunders
  if (name == "constructor" || name == "main") return true;
  if (name.starts_with("_repr_") && name.ends_with("_")) return true;  // IPython display hooks
  if (!method) return false;
  static const char* kHooks[] = {"visit_", "leave_", "depart_", "do_", "handle_", "on_", "test_",
                                 "setUp", "tearDown", "setUpClass", "tearDownClass", "run"};
  return std::any_of(std::begin(kHooks), std::end(kHooks), [&](const char* h) { return name.starts_with(h); });
}

bool outside(const Node& n) {
  return n.kind == NodeKind::External || n.kind == NodeKind::Store;
}

// Definitions nothing in the project uses: local (unreachable by name from outside the project),
// never called by other code, and whose name appears nowhere else (attrs.referenced). Then
// definitions only test code calls. One finding of each kind per component.
void unused_code(const Graph& g, const std::vector<NodeIndex>& unit, std::vector<Finding>& out) {
  std::vector<NodeIndex> parent(g.nodes().size());
  for (NodeIndex i = 0; i < parent.size(); ++i) parent[i] = i;
  for (const Edge& e : g.edges()) {
    if (e.kind == EdgeKind::Defines && parent[e.to] == e.to && e.from != e.to) parent[e.to] = e.from;
  }
  std::vector<int> callers(g.nodes().size(), 0), product_callers(g.nodes().size(), 0);
  for (const Edge& e : g.edges()) {
    if (e.kind != EdgeKind::Calls || e.from == e.to) continue;
    ++callers[e.to];
    if (!is_test(g.node(e.from).id)) ++product_callers[e.to];
  }
  struct Hit {
    NodeIndex node;
    bool test_only;
  };
  std::map<std::string, std::vector<Hit>> by_component;
  std::vector<bool> reported(g.nodes().size(), false);
  for (NodeIndex i = 0; i < g.nodes().size(); ++i) {
    const Node& n = g.node(i);
    if (n.kind != NodeKind::Function && n.kind != NodeKind::DataType) continue;
    if (n.frontend != "python" && n.frontend != "js" && n.frontend != "c") continue;
    if (!n.attrs.value("local", false) || n.attrs.value("referenced", false) || n.attrs.contains("accessor")) continue;
    if (is_test(n.id)) continue;
    const bool method = g.node(parent[i]).kind == NodeKind::DataType;
    if (called_by_name(n.name, method)) continue;
    if (product_callers[i] > 0) continue;
    // Inside something already reported: the outer definition says it.
    bool inside = false;
    for (NodeIndex p = parent[i]; p != parent[p] && !inside; p = parent[p]) inside = reported[p];
    if (inside) continue;
    reported[i] = true;
    by_component[component_of(g.node(unit[i]))].push_back({i, callers[i] > 0 || n.attrs.value("referenced_by_tests", false)});
  }
  for (const bool test_only : {false, true}) {
    for (const auto& [component, hits] : by_component) {
      Finding f;
      f.kind = test_only ? "test_only" : "unused";
      f.units = {component};
      f.confidence = "likely";
      nlohmann::json defs = nlohmann::json::array();
      std::uint32_t lines = 0;
      for (const Hit& h : hits) {
        if (h.test_only != test_only) continue;
        const Node& n = g.node(h.node);
        f.targets.push_back(n.id);
        const std::uint32_t len = n.loc && n.loc->end_line >= n.loc->line ? n.loc->end_line - n.loc->line + 1 : 0;
        lines += len;
        defs.push_back({{"id", n.id}, {"kind", std::string(to_string(n.kind))},
                        {"file", n.loc ? n.loc->file : ""}, {"line", n.loc ? n.loc->line : 0}, {"lines", len}});
      }
      if (f.targets.empty()) continue;
      std::sort(f.targets.begin(), f.targets.end());
      const std::string count = std::to_string(f.targets.size()) + (f.targets.size() == 1 ? " definition" : " definitions");
      f.title = test_only ? count + " in " + component + " only tests use (" + std::to_string(lines) + " lines)"
                          : count + " in " + component + " with no static reference found (" + std::to_string(lines) + " lines)";
      f.evidence = {{"definitions", defs}, {"lines", lines}};
      out.push_back(std::move(f));
    }
  }
}

}  // namespace

Diagnosis diagnose(const Graph& g, const std::filesystem::path& source_root) {
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
  unused_code(g, unit, d.findings);

  // The same data in several places: classes, tables and API payloads with matching fields.
  const EntityResult entities = find_entities(g);
  if (!entities.regulated.empty()) {
    std::map<std::string, std::vector<std::string>> by_kind;
    for (const auto& r : entities.regulated) by_kind[r.regulated].push_back(r.id);
    for (const auto& [kind, ids] : by_kind) {
      std::string list;
      for (std::size_t i = 0; i < ids.size() && i < 8; ++i) list += (i ? ", " : "") + std::string("`") + ids[i] + "`";
      if (ids.size() > 8) list += ", ...";
      d.notes.push_back(std::to_string(ids.size()) + " " + kind +
                        " record shape(s) left out of \"the same data in several places\": regulated forms stay as "
                        "they are, separately (" + list + ")");
    }
  }
  for (const EntityGroup& eg : entities.groups) {
    Finding f;
    f.kind = "same_entity";
    std::set<std::string> comps;
    std::map<std::string, int> kinds;
    nlohmann::json members = nlohmann::json::array();
    for (const EntityShape& m : eg.members) {
      f.targets.push_back(m.id);
      ++kinds[m.kind];
      for (const auto& u : m.users) {
        if (auto i = g.find(u)) comps.insert(component_of(g.node(unit[*i])));
      }
      members.push_back({{"id", m.id}, {"kind", m.kind}, {"name", m.name}, {"fields", m.fields},
                         {"normalized", m.normalized}, {"keys", m.keys}, {"used_by", m.users}});
    }
    f.units.assign(comps.begin(), comps.end());
    f.confidence = "likely";
    std::string where;
    for (const auto& [k, n] : kinds) {
      where += (where.empty() ? "" : ", ") + std::to_string(n) + " " +
               (k == "class" ? (n == 1 ? "class" : "classes") : k == "table" ? (n == 1 ? "table" : "tables") : (n == 1 ? "API payload" : "API payloads"));
    }
    f.title = "`" + eg.name + "` is defined in " + std::to_string(eg.members.size()) + " places (" + where + ")";
    f.evidence = {{"entity", eg.name}, {"similarity", std::round(eg.score * 100) / 100}, {"members", members},
                  {"proposed_fields", eg.proposed}, {"basis", eg.basis}, {"basis_fields", eg.basis_fields}};
    d.findings.push_back(std::move(f));
  }

  // Duplicated logic: clone groups found from the source (docs/diagnose.md).
  std::error_code ec;
  if (!source_root.empty() && std::filesystem::exists(source_root, ec)) {
    for (const CloneGroup& cg : find_clones(g, source_root)) {
      Finding f;
      f.kind = "duplicated_logic";
      std::set<std::string> comps;
      nlohmann::json members = nlohmann::json::array();
      std::uint32_t total = 0, most = 0;
      for (const CloneMember& m : cg.members) {
        f.targets.push_back(m.id);
        if (auto i = g.find(m.id)) comps.insert(component_of(g.node(unit[*i])));
        members.push_back({{"id", m.id}, {"file", m.file}, {"line", m.line}, {"lines", m.lines}, {"tokens", m.tokens}});
        total += m.lines;
        most = std::max(most, m.lines);
      }
      f.units.assign(comps.begin(), comps.end());
      f.confidence = cg.type == "similar" ? "likely" : "certain";
      const std::string how = cg.type == "exact"     ? "exact copies"
                              : cg.type == "renamed" ? "copies with renamed names or values"
                                                     : "near copies";
      f.title = std::to_string(cg.members.size()) + " functions are " + how + " (" + std::to_string(total - most) +
                " lines to remove by keeping one)";
      f.evidence = {{"clone_type", cg.type}, {"similarity", std::round(cg.similarity * 100) / 100},
                    {"members", members}, {"lines_saved", total - most}};
      d.findings.push_back(std::move(f));
    }
  }

  // Surest first (docs/plans/phase2.md), then strongest: more units, then more targets.
  auto rank = [](const std::string& c) { return c == "certain" ? 2 : c == "likely" ? 1 : 0; };
  // Duplicated logic weighs by the lines merging would save.
  auto weight = [](const Finding& f) {
    return f.kind == "duplicated_logic" ? f.evidence.value("lines_saved", 0u) : static_cast<unsigned>(f.targets.size());
  };
  std::stable_sort(d.findings.begin(), d.findings.end(), [&](const Finding& a, const Finding& b) {
    return std::make_tuple(rank(a.confidence), a.units.size(), weight(a)) >
           std::make_tuple(rank(b.confidence), b.units.size(), weight(b));
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
          {"reached", reached}, {"packages", d.packages}, {"findings", findings}, {"notes", d.notes}};
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
    if (f.kind == "unused" || f.kind == "test_only") {
      o += std::string("Kind: ") + (f.kind == "unused" ? "unused code" : "used only by tests") +
           ". Confidence: **" + f.confidence + "**. " +
           (f.kind == "unused"
                ? "Nothing in the project calls these, and their names appear nowhere else. Code that looks "
                  "names up at runtime (plugins, reflection) isn't visible: confirm before deleting.\n\n"
                : "Only test code calls these; the product never runs them.\n\n");
      o += "| Definition | Kind | Where | Lines |\n|---|---|---|---|\n";
      for (const auto& dj : f.evidence["definitions"]) {
        o += "| `" + dj["id"].get<std::string>() + "` | " + dj["kind"].get<std::string>() + " | " +
             dj["file"].get<std::string>() + ":" + std::to_string(dj["line"].get<std::uint32_t>()) + " | " +
             std::to_string(dj["lines"].get<std::uint32_t>()) + " |\n";
      }
      o += "\n";
      continue;
    }
    if (f.kind == "same_entity") {
      o += "Kind: the same data in several places. Confidence: **" + f.confidence +
           "**. One definition could replace them: `" + f.evidence["entity"].get<std::string>() + "` with ";
      std::string proposed;
      for (const auto& p : f.evidence["proposed_fields"]) proposed += (proposed.empty() ? "" : ", ") + p.get<std::string>();
      std::string basis;
      for (const auto& b : f.evidence["basis_fields"]) basis += (basis.empty() ? "" : ", ") + b.get<std::string>();
      o += proposed + ". Matched by " +
           (f.evidence["basis"] == "identity" ? "a shared identity key: " + basis
                                              : "a temporary composite key (no shared id): " + basis) +
           ".\n\n| Field |";
      for (const auto& mj : f.evidence["members"]) o += " `" + mj["id"].get<std::string>() + "` (" + mj["kind"].get<std::string>() + ") |";
      o += "\n|---|";
      for (std::size_t i = 0; i < f.evidence["members"].size(); ++i) o += "---|";
      o += "\n";
      std::vector<std::string> rows;
      for (const auto& mj : f.evidence["members"]) {
        for (const auto& nf : mj["normalized"]) {
          if (std::find(rows.begin(), rows.end(), nf.get<std::string>()) == rows.end()) rows.push_back(nf.get<std::string>());
        }
      }
      for (const auto& r : rows) {
        o += "| " + r + " |";
        for (const auto& mj : f.evidence["members"]) {
          std::string cell = "–";
          for (std::size_t fi = 0; fi < mj["normalized"].size(); ++fi) {
            if (mj["normalized"][fi] == r) cell = "`" + mj["fields"][fi].get<std::string>() + "`";
          }
          o += " " + cell + " |";
        }
        o += "\n";
      }
      o += "\n";
      continue;
    }
    if (f.kind == "duplicated_logic") {
      o += "Kind: duplicated logic (" + f.evidence["clone_type"].get<std::string>() + ", similarity " +
           std::to_string(static_cast<int>(std::lround(f.evidence["similarity"].get<double>() * 100))) +
           "%). Confidence: **" + f.confidence + "**.\n\n| Function | Where | Lines |\n|---|---|---|\n";
      for (const auto& mj : f.evidence["members"]) {
        o += "| `" + mj["id"].get<std::string>() + "` | " + mj["file"].get<std::string>() + ":" +
             std::to_string(mj["line"].get<std::uint32_t>()) + " | " + std::to_string(mj["lines"].get<std::uint32_t>()) + " |\n";
      }
      o += "\n";
      continue;
    }
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

  if (!d.notes.empty()) {
    o += "## Notes\n\n";
    for (const auto& n : d.notes) o += "- " + n + "\n";
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
