#pragma once
// The C frontend: reads .c and .h files with tree-sitter and produces a graph following the C
// rules in docs/frontends.md. No preprocessing: macros aren't expanded, but string macros are
// read for URLs and SQL.

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "optum/ir.hpp"

namespace optum {

struct CStats {
  std::size_t files = 0;
  std::size_t lines = 0;
  std::size_t files_with_syntax_errors = 0;
  std::vector<std::string> unreadable;  // files that could not be read (analysed as empty)
};

// `files` are relative to `root`; only .c and .h are read. `threads` = 0 uses every core.
Graph extract_c_files(const std::filesystem::path& root, const std::vector<std::filesystem::path>& files,
                      CStats* stats = nullptr, unsigned threads = 0);

}  // namespace optum
