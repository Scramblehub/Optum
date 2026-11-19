#pragma once
// The JavaScript / TypeScript frontend: reads .js/.jsx/.mjs/.cjs/.ts/.tsx with tree-sitter and
// produces a graph following the rules in docs/frontends.md ("JavaScript and TypeScript").

#include <cstddef>
#include <filesystem>
#include <vector>

#include "optum/ir.hpp"

namespace optum {

struct JsStats {
  std::size_t files = 0;
  std::size_t lines = 0;
  std::size_t files_with_syntax_errors = 0;
};

// `files` are relative to `root`. Declaration files (.d.ts) and minified files (.min.js) are
// skipped. `threads` = 0 uses every core.
Graph extract_js_files(const std::filesystem::path& root, const std::vector<std::filesystem::path>& files,
                       JsStats* stats = nullptr, unsigned threads = 0);

// Is this a file the JS frontend reads?
bool is_js_source(const std::filesystem::path& rel);

}  // namespace optum
