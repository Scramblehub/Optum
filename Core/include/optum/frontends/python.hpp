#pragma once
// The Python frontend: reads .py source with tree-sitter and produces a graph following the
// Python rules in docs/frontends.md.

#include <cstddef>
#include <filesystem>
#include <vector>

#include "optum/ir.hpp"

namespace optum {

struct PythonStats {
  std::size_t files = 0;
  std::size_t lines = 0;
  std::size_t files_with_syntax_errors = 0;  // still analysed: tree-sitter recovers
};

// `path` is a .py file or a directory, searched recursively. Module names are dotted paths
// relative to the directory (or to the file's directory). `threads` = 0 uses every core.
Graph extract_python(const std::filesystem::path& path, PythonStats* stats = nullptr,
                     unsigned threads = 0);

// The same, for a given list of .py files relative to `root` (as optum::discover returns).
Graph extract_python_files(const std::filesystem::path& root,
                           const std::vector<std::filesystem::path>& files,
                           PythonStats* stats = nullptr, unsigned threads = 0);

}  // namespace optum
