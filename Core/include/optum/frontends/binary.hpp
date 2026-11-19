#pragma once
// The binary frontend: x86-64 ELF executables and shared libraries, disassembled with capstone.
// Finds functions (from symbols, unwind tables, exports and the code itself), direct calls between
// them, and calls to imported library functions. Plan: docs/plans/binary.md (stage 3); ids and
// confidence: docs/decisions/0005-binaries.md; rules: docs/frontends.md ("Binaries").

#include <cstddef>
#include <filesystem>
#include <vector>

#include "optum/ir.hpp"

namespace optum {

struct BinaryStats {
  std::size_t files = 0;        // ELF files read
  std::size_t skipped = 0;      // ELF files left out (object files, other architectures, malformed)
  std::size_t functions = 0;
  std::size_t instructions = 0;
};

// `files` are relative to `root`; non-ELF files are ignored. Messages about skipped files go to
// `diagnostics`, one per line.
Graph extract_binary_files(const std::filesystem::path& root, const std::vector<std::filesystem::path>& files,
                           BinaryStats* stats = nullptr, std::string* diagnostics = nullptr);

}  // namespace optum
