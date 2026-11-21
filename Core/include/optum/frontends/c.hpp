#pragma once
// The C frontend: reads .c and .h files with tree-sitter and produces a graph following the C
// rules in docs/frontends.md. No preprocessing: macros aren't expanded, but string macros are
// read for URLs and SQL, calls in function-like macros are followed, and #ifs are evaluated for
// one build configuration (compile_commands.json's flags if `root` has one, else no -D flags).

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
  bool build_database = false;           // a compilation database was read
  std::size_t build_files = 0;           // files it lists inside the root
  std::size_t external_headers = 0;      // generated headers read from outside the root
  // Without build information: names that no visible file defines, taken as off, and the
  // conditional (inferred, inactive) call edges whose conditions name them.
  std::size_t assumed_off_names = 0;
  std::size_t assumed_off_edges = 0;
};

// `files` are relative to `root`; only .c and .h are read. `threads` = 0 uses every core.
// `compile_commands`: a compilation database for the build ("" looks for compile_commands.json at the
// root or in build/; "none" ignores any). From it, per file: -D / -U, include directories (they may
// lie outside the root: generated headers are read from there), -include headers, and which files
// are in the build (a call links only to definitions in it).
Graph extract_c_files(const std::filesystem::path& root, const std::vector<std::filesystem::path>& files,
                      CStats* stats = nullptr, unsigned threads = 0, const std::string& compile_commands = "");

}  // namespace optum
