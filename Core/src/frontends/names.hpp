#pragma once
// Name uses, for attrs.referenced (docs/frontends.md): how often each name appears in a file
// other than as the name of a definition. A use can be a call, a value (a callback, a dispatch
// table entry), an attribute, or a string that is exactly the name (getattr(x, "name"),
// __all__). Counting names, not resolved references, is deliberately generous: a definition
// whose name appears nowhere else is very likely unused; one whose name appears may still be.

#include <algorithm>
#include <cctype>
#include <vector>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "../ts_util.hpp"

namespace optum::frontends {

using NameUses = std::unordered_map<std::string, std::uint32_t>;

// Keys of NameUses starting with this mark a name prefix that code builds names from:
// getattr(self, "_handle_" + kind), "_get_%s" % name, f"_visit_{node}". Any definition whose name
// starts with one is counted as used.
inline constexpr char kPrefixMark = '\x02';
// A string that is just a name may be a prefix too (METHOD_PREFIX = "_generate", then getattr
// over dir(self)): a candidate, kept by finalize_name_uses() only if 3+ definitions start with it.
inline constexpr char kCandidateMark = '\x03';
// The literal piece after an interpolation (f"self._{name}Presentation"): a name suffix.
inline constexpr char kSuffixMark = '\x04';

inline bool identifier_like(std::string_view s) {
  if (s.empty() || s.size() > 128) return false;
  if (!(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_' || s[0] == '$' || s[0] == '#')) return false;
  for (char c : s) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '#')) return false;
  }
  return true;
}

// Counts every identifier-like node of `name_types` and every string node of `string_types`
// whose whole text is a name (or a dotted path: its last part), skipping the definitions
// themselves: the given field of each node type in `def_fields` (function_definition -> "name").
using DefFields = std::unordered_map<std::string_view, const char*>;
inline void count_name_uses(TSNode root, std::string_view src, const std::unordered_set<std::string_view>& name_types,
                            const std::unordered_set<std::string_view>& string_types, const DefFields& def_fields,
                            NameUses& out, const std::unordered_set<std::string_view>& text_types = {}) {
  // The walk keeps its own stack of ancestors: ts_node_parent() walks down from the root each
  // time, which made deeply nested code (a 20,000-call chain) quadratic.
  TSTreeCursor cur = ts_tree_cursor_new(root);
  std::vector<TSNode> ancestors;
  for (;;) {
    const TSNode n = ts_tree_cursor_current_node(&cur);
    const std::string_view type = ts_node_type(n);
    if (name_types.count(type)) {
      const TSNode parent = ancestors.empty() ? TSNode{} : ancestors.back();
      bool is_def = false;
      if (!ts_node_is_null(parent)) {
        if (auto it = def_fields.find(ts_node_type(parent)); it != def_fields.end()) {
          is_def = ts_node_eq(ts::field(parent, it->second), n);
        }
      }
      if (!is_def) {
        const std::string_view t = ts::text(n, src);
        if (identifier_like(t)) ++out[std::string(t)];
      }
    } else if (text_types.count(type)) {  // unparsed text (C macro bodies): every word in it
      const std::string_view t = ts::text(n, src);
      std::size_t i = 0;
      while (i < t.size()) {
        std::size_t j = i;
        while (j < t.size() && (std::isalnum(static_cast<unsigned char>(t[j])) || t[j] == '_')) ++j;
        if (j > i && identifier_like(t.substr(i, j - i))) ++out[std::string(t.substr(i, j - i))];
        i = j == i ? i + 1 : j;
      }
    } else if (string_types.count(type)) {
      std::string_view t = ts::text(n, src);
      // A prefix: the leading name part, when the string ends in '_' or goes on with a format
      // marker, or when the literal goes on with an interpolation (f"_get_{x}", `_get_${x}`).
      std::size_t k = 0;
      while (k < t.size() && (std::isalnum(static_cast<unsigned char>(t[k])) || t[k] == '_')) ++k;
      const TSNode next = ts_node_next_sibling(n);
      const bool interpolated = !ts_node_is_null(next) && (ts::is(next, "interpolation") || ts::is(next, "template_substitution"));
      if (k >= 3 && identifier_like(t.substr(0, k)) &&
          ((k == t.size() && (t[k - 1] == '_' || interpolated)) || (k < t.size() && (t[k] == '%' || t[k] == '{')))) {
        ++out[kPrefixMark + std::string(t.substr(0, k))];
      }
      const TSNode prev = ts_node_prev_sibling(n);
      if (!ts_node_is_null(prev) && (ts::is(prev, "interpolation") || ts::is(prev, "template_substitution")) && k >= 4) {
        ++out[kSuffixMark + std::string(t.substr(0, k))];
      }
      if (k == t.size() && k >= 4 && identifier_like(t)) ++out[kCandidateMark + std::string(t)];
      // Code inside a string (templates, generated code): a name followed by '(' or after '.'.
      for (std::size_t i = 0; i < t.size();) {
        std::size_t j = i;
        while (j < t.size() && (std::isalnum(static_cast<unsigned char>(t[j])) || t[j] == '_')) ++j;
        if (j > i && ((j < t.size() && t[j] == '(') || (i > 0 && t[i - 1] == '.')) && identifier_like(t.substr(i, j - i))) {
          ++out[std::string(t.substr(i, j - i))];
        }
        i = j == i ? i + 1 : j;
      }
      if (const auto dot = t.rfind('.'); dot != std::string_view::npos) t = t.substr(dot + 1);
      if (identifier_like(t)) ++out[std::string(t)];
    }
    if (ts_tree_cursor_goto_first_child(&cur)) {
      ancestors.push_back(n);
      continue;
    }
    while (!ts_tree_cursor_goto_next_sibling(&cur)) {
      if (!ts_tree_cursor_goto_parent(&cur)) {
        ts_tree_cursor_delete(&cur);
        return;
      }
      ancestors.pop_back();
    }
  }
}

// Every word of plain text that looks like a name (files C code #includes but doesn't parse:
// generated .inc tables).
inline void count_words(std::string_view t, NameUses& out) {
  std::size_t i = 0;
  while (i < t.size()) {
    std::size_t j = i;
    while (j < t.size() && (std::isalnum(static_cast<unsigned char>(t[j])) || t[j] == '_')) ++j;
    if (j > i && identifier_like(t.substr(i, j - i))) ++out[std::string(t.substr(i, j - i))];
    i = j == i ? i + 1 : j;
  }
}

// Keeps the candidate prefixes that at least 3 of the project's definitions start with, as
// prefixes. Call once, with every definition's name, before name_used().
inline void finalize_name_uses(NameUses& uses, std::vector<std::string> def_names) {
  std::sort(def_names.begin(), def_names.end());
  std::vector<std::string> keep;
  for (auto it = uses.begin(); it != uses.end();) {
    if (it->first.empty() || it->first[0] != kCandidateMark) {
      ++it;
      continue;
    }
    const std::string p = it->first.substr(1);
    auto lo = std::lower_bound(def_names.begin(), def_names.end(), p);
    int n = 0;
    for (; lo != def_names.end() && lo->starts_with(p) && n < 3; ++lo) n += *lo != p;
    if (n >= 3) keep.push_back(p);
    it = uses.erase(it);
  }
  for (const auto& p : keep) ++uses[kPrefixMark + p];
}

// Is `name` used: by name, by a prefix code builds names from, or by a suffix after one?
inline bool name_used(const NameUses& uses, const std::string& name) {
  if (uses.count(name)) return true;
  std::string key(1, kPrefixMark);
  for (std::size_t i = 0; i + 1 < name.size(); ++i) {  // proper prefixes only
    key += name[i];
    if (key.size() > 3 && uses.count(key)) return true;
  }
  for (std::size_t i = 1; i + 4 <= name.size(); ++i) {  // proper suffixes of 4+ characters
    if (uses.count(kSuffixMark + name.substr(i))) return true;
  }
  return false;
}

}  // namespace optum::frontends
