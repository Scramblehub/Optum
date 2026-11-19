#include "optum/clones.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "test_code.hpp"
#include "ts_util.hpp"

extern "C" const TSLanguage* tree_sitter_python();
extern "C" const TSLanguage* tree_sitter_javascript();
extern "C" const TSLanguage* tree_sitter_typescript();
extern "C" const TSLanguage* tree_sitter_tsx();
extern "C" const TSLanguage* tree_sitter_c();

namespace optum {
namespace {

namespace fs = std::filesystem;

// ----------------------------------------------------------------------------------- hashing

std::uint64_t mix(std::uint64_t h, std::uint64_t v) {  // order-sensitive combine (splitmix-style)
  h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  h *= 0xbf58476d1ce4e5b9ULL;
  return h ^ (h >> 31);
}

std::uint64_t hash_str(std::string_view s) {
  std::uint64_t h = 1469598103934665603ULL;  // FNV-1a
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  return h;
}

const std::unordered_set<std::string_view>& name_types() {
  static const std::unordered_set<std::string_view> t = {
      "identifier", "property_identifier", "field_identifier", "type_identifier", "shorthand_property_identifier",
      "private_property_identifier", "statement_identifier", "shorthand_property_identifier_pattern"};
  return t;
}

const std::unordered_set<std::string_view>& literal_types() {
  static const std::unordered_set<std::string_view> t = {
      "string", "template_string", "string_literal", "concatenated_string", "char_literal", "raw_string_literal",
      "number", "integer", "float", "number_literal", "true", "false", "none", "null", "regex"};
  return t;
}

struct Shape {
  std::uint64_t exact = 0, norm = 0;
  std::uint32_t size = 0;    // named nodes
  std::uint32_t tokens = 0;  // leaves
};

// One function body: its exact and normalised hashes, and its structure memory: for every
// subtree of 4 to 40 named nodes, with typed holes: names become ID, literals LIT (CGL's
// skeletons, on syntax instead of instructions).
class Shaper {
 public:
  Shaper(std::string_view src, bool python) : src_(src), python_(python) {}
  std::vector<std::uint64_t> features;

  Shape run(TSNode body) { return walk(body, 0, true); }

 private:
  std::string_view src_;
  bool python_;

  Shape walk(TSNode n, int depth, bool is_body) {
    Shape s;
    const std::string_view type = ts_node_type(n);
    if (type == "comment" || depth > 400) return s;
    if (literal_types().count(type) || (ts_node_child_count(n) == 0)) {
      const std::string_view text = ts::text(n, src_);
      s.exact = hash_str(text);
      s.norm = name_types().count(type) ? hash_str("\x01ID") : literal_types().count(type) ? hash_str("\x01LIT") : hash_str(type);
      s.size = ts_node_is_named(n) ? 1 : 0;
      s.tokens = 1;
      return s;
    }
    s.exact = s.norm = hash_str(type);
    s.size = 1;
    const std::uint32_t count = ts_node_child_count(n);
    for (std::uint32_t i = 0; i < count; ++i) {
      const TSNode c = ts_node_child(n, i);
      // A Python docstring (the body's first statement, a lone string) isn't logic.
      if (is_body && python_ && i == 0 && ts::is(c, "expression_statement") && ts_node_named_child_count(c) == 1 &&
          ts::is(ts_node_named_child(c, 0), "string")) {
        continue;
      }
      const Shape cs = walk(c, depth + 1, false);
      if (!cs.tokens) continue;
      s.exact = mix(s.exact, cs.exact);
      s.norm = mix(s.norm, cs.norm);
      s.size += cs.size;
      s.tokens += cs.tokens;
    }
    // Local pieces only (4 to 40 named nodes): an edit then changes the few pieces around it, not
    // every enclosing subtree up to the function.
    if (s.size >= 4 && s.size <= 40 && !is_body) features.push_back(s.norm);
    return s;
  }
};

// ----------------------------------------------------------------------------------- functions

struct Fn {
  std::string id, file, area;
  bool generated = false;
  std::uint32_t line = 0, lines = 0;
  Shape shape;
  std::unordered_map<std::uint64_t, std::uint32_t> features;  // structure memory: subtree -> count
  std::optional<std::size_t> parent;  // the enclosing function, if it's a candidate too
};

const TSLanguage* language_of(const fs::path& p) {
  const std::string e = p.extension().string();
  if (e == ".py") return tree_sitter_python();
  if (e == ".js" || e == ".jsx" || e == ".mjs" || e == ".cjs") return tree_sitter_javascript();
  if (e == ".ts" || e == ".mts" || e == ".cts") return tree_sitter_typescript();
  if (e == ".tsx") return tree_sitter_tsx();
  if (e == ".c" || e == ".h") return tree_sitter_c();
  return nullptr;
}

bool is_function_node(std::string_view t) {
  return t == "function_definition" || t == "function_declaration" || t == "generator_function_declaration" ||
         t == "method_definition" || t == "function_expression" || t == "arrow_function" || t == "function" ||
         t == "generator_function";
}

// The file a node's location names, under the analysed root or one of its parents (Python names
// files from the outermost package around the root).
std::optional<fs::path> locate(const fs::path& root, const std::string& file) {
  fs::path base = root;
  for (int up = 0; up < 4 && !base.empty(); ++up) {
    std::error_code ec;
    if (fs::is_regular_file(base / file, ec)) return base / file;
    if (fs::is_regular_file(base, ec) && base.filename() == fs::path(file).filename()) return base;
    const fs::path parent = base.parent_path();
    if (parent == base) break;
    base = parent;
  }
  return std::nullopt;
}

// Every function-like syntax node, by start line: the outermost one starting there.
void index_functions(TSNode n, std::map<std::uint32_t, TSNode>& out, int depth = 0) {
  if (depth > 400) return;
  if (is_function_node(ts_node_type(n))) out.try_emplace(ts::line(n), n);
  const std::uint32_t count = ts_node_named_child_count(n);
  for (std::uint32_t i = 0; i < count; ++i) index_functions(ts_node_named_child(n, i), out, depth + 1);
}

double weighted_overlap(const Fn& a, const Fn& b, const std::unordered_map<std::uint64_t, double>& weight) {
  double inter = 0, uni = 0;
  for (const auto& [f, ca] : a.features) {
    const double w = weight.at(f);
    auto it = b.features.find(f);
    const std::uint32_t cb = it == b.features.end() ? 0 : it->second;
    inter += w * std::min(ca, cb);
    uni += w * std::max(ca, cb);
  }
  for (const auto& [f, cb] : b.features) {
    if (!a.features.count(f)) uni += weight.at(f) * cb;
  }
  return uni > 0 ? inter / uni : 0.0;
}

}  // namespace

std::vector<CloneGroup> find_clones(const Graph& g, const fs::path& root, const CloneOptions& opts) {
  // Candidates: Python, JS and C functions outside test code, grouped by file.
  std::unordered_map<NodeIndex, NodeIndex> parent;
  for (const Edge& e : g.edges()) {
    if (e.kind == EdgeKind::Defines) parent.try_emplace(e.to, e.from);
  }
  std::map<std::string, std::vector<NodeIndex>> by_file;
  for (NodeIndex i = 0; i < g.nodes().size(); ++i) {
    const Node& n = g.node(i);
    if (n.kind != NodeKind::Function || !n.loc || n.loc->file.empty()) continue;
    if (n.frontend != "python" && n.frontend != "js" && n.frontend != "c") continue;
    if (is_test_code(n.id) || is_test_code(n.loc->file)) continue;
    if (n.loc->end_line < n.loc->line || n.loc->end_line - n.loc->line + 1 < opts.min_lines) continue;
    by_file[n.loc->file].push_back(i);
  }

  std::vector<Fn> fns;
  std::unordered_map<NodeIndex, std::size_t> fn_of;
  ts::ParserPtr parser(ts_parser_new());
  for (const auto& [file, nodes] : by_file) {
    const auto path = locate(root, file);
    const TSLanguage* lang = path ? language_of(*path) : nullptr;
    if (!lang) continue;
    std::ifstream in(*path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string src = ss.str();
    const bool generated = generated_header(src);
    ts_parser_set_language(parser.get(), lang);
    ts::TreePtr tree(ts_parser_parse_string(parser.get(), nullptr, src.data(), static_cast<uint32_t>(src.size())));
    std::map<std::uint32_t, TSNode> starts;
    index_functions(ts_tree_root_node(tree.get()), starts);
    for (NodeIndex i : nodes) {
      const Node& n = g.node(i);
      auto it = starts.find(n.loc->line);
      if (it == starts.end()) continue;
      const TSNode body = ts::field(it->second, "body");
      if (ts_node_is_null(body)) continue;
      Shaper shaper(src, lang == tree_sitter_python());
      Fn fn;
      fn.shape = shaper.run(body);
      if (fn.shape.tokens < opts.min_tokens) continue;
      fn.id = n.id;
      fn.file = file;
      fn.area = code_area(file);
      fn.generated = generated;
      fn.line = n.loc->line;
      fn.lines = n.loc->end_line - n.loc->line + 1;
      for (std::uint64_t f : shaper.features) ++fn.features[f];
      // Enough distinct structure to be logic: a run of `self.x = x` is one piece, repeated.
      if (fn.features.size() < opts.min_distinct) continue;
      fn_of[i] = fns.size();
      fns.push_back(std::move(fn));
    }
  }
  for (const auto& [node, k] : fn_of) {  // nearest enclosing candidate function
    for (auto p = parent.find(node); p != parent.end(); p = parent.find(p->second)) {
      if (auto f = fn_of.find(p->second); f != fn_of.end()) {
        fns[k].parent = f->second;
        break;
      }
    }
  }
  if (fns.size() < 2) return {};
  // A function and one nested in it overlap by construction: never clones of each other.
  auto related = [&](std::size_t a, std::size_t b) {
    for (auto p = fns[a].parent; p; p = fns[*p].parent) if (*p == b) return true;
    for (auto p = fns[b].parent; p; p = fns[*p].parent) if (*p == a) return true;
    return false;
  };

  // The structure memory: how many functions each subtree appears in. Rare pieces weigh more.
  std::unordered_map<std::uint64_t, std::uint32_t> df;
  for (const Fn& f : fns) {
    for (const auto& [h, _] : f.features) ++df[h];
  }
  std::unordered_map<std::uint64_t, double> weight;
  const double n_fns = static_cast<double>(fns.size());
  for (const auto& [h, d] : df) weight[h] = std::log(1.0 + n_fns / d);

  // Groups start as functions with the same normalised shape (exact and renamed copies).
  std::vector<int> group_of(fns.size(), -1);
  std::vector<std::vector<std::size_t>> groups;
  {
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> by_norm;
    for (std::size_t k = 0; k < fns.size(); ++k) by_norm[fns[k].shape.norm].push_back(k);
    for (auto& [_, ks] : by_norm) {
      if (ks.size() < 2) continue;
      for (std::size_t k : ks) group_of[k] = static_cast<int>(groups.size());
      groups.push_back(ks);
    }
  }

  // Edited copies: candidate pairs share 3+ rare subtrees; then the weighted overlap decides.
  std::map<std::pair<std::size_t, std::size_t>, double> sims;
  auto similarity = [&](std::size_t a, std::size_t b) {
    if (a > b) std::swap(a, b);
    if (fns[a].shape.norm == fns[b].shape.norm) return 1.0;
    auto [it, fresh] = sims.try_emplace({a, b}, 0.0);
    if (fresh) it->second = weighted_overlap(fns[a], fns[b], weight);
    return it->second;
  };
  {
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> postings;
    for (std::size_t k = 0; k < fns.size(); ++k) {
      for (const auto& [h, _] : fns[k].features) {
        if (df[h] >= 2 && df[h] <= 20) postings[h].push_back(k);
      }
    }
    std::map<std::pair<std::size_t, std::size_t>, int> shared;
    for (const auto& [_, ks] : postings) {
      for (std::size_t i = 0; i < ks.size(); ++i) {
        for (std::size_t j = i + 1; j < ks.size(); ++j) ++shared[{ks[i], ks[j]}];
      }
    }
    std::vector<std::tuple<double, std::size_t, std::size_t>> pairs;
    for (const auto& [ab, count] : shared) {
      const auto [a, b] = ab;
      if (count < 3) continue;
      const double ta = fns[a].shape.tokens, tb = fns[b].shape.tokens;
      if (std::min(ta, tb) / std::max(ta, tb) < 0.6) continue;
      if (group_of[a] >= 0 && group_of[a] == group_of[b]) continue;
      if (related(a, b)) continue;
      const double s = similarity(a, b);
      if (s >= opts.min_similarity) pairs.emplace_back(s, a, b);
    }
    std::sort(pairs.begin(), pairs.end(), [](const auto& x, const auto& y) { return std::get<0>(x) > std::get<0>(y); });
    // Merge only when every member of one group is similar to every member of the other, so
    // loosely similar functions can't chain into one group.
    for (const auto& [s, a, b] : pairs) {
      std::vector<std::size_t> ga = group_of[a] >= 0 ? groups[std::size_t(group_of[a])] : std::vector<std::size_t>{a};
      std::vector<std::size_t> gb = group_of[b] >= 0 ? groups[std::size_t(group_of[b])] : std::vector<std::size_t>{b};
      if (group_of[a] >= 0 && group_of[a] == group_of[b]) continue;
      if (ga.size() + gb.size() > opts.max_group) continue;
      bool all = true;
      for (std::size_t x : ga) {
        for (std::size_t y : gb) all = all && !related(x, y) && similarity(x, y) >= opts.min_similarity;
      }
      if (!all) continue;
      std::vector<std::size_t> merged = ga;
      merged.insert(merged.end(), gb.begin(), gb.end());
      int gi;
      if (group_of[a] >= 0) gi = group_of[a];
      else if (group_of[b] >= 0) gi = group_of[b];
      else {
        gi = static_cast<int>(groups.size());
        groups.emplace_back();
      }
      if (group_of[a] >= 0 && group_of[b] >= 0 && group_of[a] != group_of[b]) groups[std::size_t(group_of[b])].clear();
      groups[std::size_t(gi)] = merged;
      for (std::size_t k : merged) group_of[k] = gi;
    }
  }

  // A group whose every member sits inside members of another group repeats that group.
  std::vector<CloneGroup> out;
  for (std::size_t gi = 0; gi < groups.size(); ++gi) {
    const auto& ks = groups[gi];
    if (ks.size() < 2 || ks.size() > opts.max_group) continue;
    const bool nested = std::all_of(ks.begin(), ks.end(), [&](std::size_t k) {
      for (auto p = fns[k].parent; p; p = fns[*p].parent) {
        if (group_of[*p] >= 0 && group_of[*p] != static_cast<int>(gi) && groups[std::size_t(group_of[*p])].size() >= 2) return true;
      }
      return false;
    });
    if (nested) continue;
    CloneGroup cg;
    bool same_norm = true, same_exact = true;
    double low = 1.0;
    for (std::size_t i = 0; i < ks.size(); ++i) {
      for (std::size_t j = i + 1; j < ks.size(); ++j) {
        same_norm = same_norm && fns[ks[i]].shape.norm == fns[ks[j]].shape.norm;
        same_exact = same_exact && fns[ks[i]].shape.exact == fns[ks[j]].shape.exact;
        low = std::min(low, similarity(ks[i], ks[j]));
      }
    }
    cg.type = !same_norm ? "similar" : same_exact ? "exact" : "renamed";
    cg.similarity = low;
    for (std::size_t k : ks) {
      cg.members.push_back({fns[k].id, fns[k].file, fns[k].line, fns[k].lines, fns[k].shape.tokens, fns[k].area,
                            fns[k].generated});
    }
    std::sort(cg.members.begin(), cg.members.end(), [](const auto& x, const auto& y) { return x.id < y.id; });
    out.push_back(std::move(cg));
  }
  auto saved = [](const CloneGroup& cg) {
    std::uint32_t total = 0, most = 0;
    for (const auto& m : cg.members) {
      total += m.lines;
      most = std::max(most, m.lines);
    }
    return total - most;
  };
  std::stable_sort(out.begin(), out.end(), [&](const auto& a, const auto& b) { return saved(a) > saved(b); });
  return out;
}

}  // namespace optum
