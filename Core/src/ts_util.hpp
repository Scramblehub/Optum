#pragma once
// Small helpers over the tree-sitter C API. Internal to Core.

#include <tree_sitter/api.h>

#include <cstring>
#include <memory>
#include <string_view>

namespace optum::ts {

struct ParserDeleter {
  void operator()(TSParser* p) const { ts_parser_delete(p); }
};
struct TreeDeleter {
  void operator()(TSTree* t) const { ts_tree_delete(t); }
};
using ParserPtr = std::unique_ptr<TSParser, ParserDeleter>;
using TreePtr = std::unique_ptr<TSTree, TreeDeleter>;

inline bool is(TSNode n, const char* type) {
  return !ts_node_is_null(n) && std::strcmp(ts_node_type(n), type) == 0;
}

inline TSNode field(TSNode n, const char* name) {
  return ts_node_child_by_field_name(n, name, static_cast<uint32_t>(std::strlen(name)));
}

inline std::string_view text(TSNode n, std::string_view src) {
  if (ts_node_is_null(n)) return {};
  const uint32_t b = ts_node_start_byte(n), e = ts_node_end_byte(n);
  return src.substr(b, e - b);
}

inline uint32_t line(TSNode n) { return ts_node_start_point(n).row + 1; }
inline uint32_t end_line(TSNode n) { return ts_node_end_point(n).row + 1; }

// Calls f(child) for each named child; f(child, field_name) when the field name is wanted.
template <typename F>
void for_named(TSNode n, F&& f) {
  const uint32_t count = ts_node_named_child_count(n);
  for (uint32_t i = 0; i < count; ++i) f(ts_node_named_child(n, i));
}

// Calls f(child) for each child (named or not) stored under field `name`.
template <typename F>
void for_field(TSNode n, const char* name, F&& f) {
  const uint32_t count = ts_node_child_count(n);
  for (uint32_t i = 0; i < count; ++i) {
    const char* fname = ts_node_field_name_for_child(n, i);
    if (fname && std::strcmp(fname, name) == 0) f(ts_node_child(n, i));
  }
}

}  // namespace optum::ts
