#pragma once
// A minimal DWARF reader (versions 4 and 5): the functions a binary's debug info describes, with
// their names, source files and every address range they occupy. Enough to give binary functions
// their source ids (docs/decisions/0005-binaries.md); nothing else of DWARF is read.

#include <cstdint>
#include <string>
#include <vector>

#include "optum/elf.hpp"

namespace optum::dwarf {

struct Function {
  std::uint64_t start = 0;  // one per range: a .cold piece split off a function is its own entry
  std::string name;         // the source name, through abstract_origin / specification for clones
  std::string file;         // the declaring file, as recorded (comp_dir and directories applied)
  std::uint32_t line = 0;   // the declaring line
  bool is_main_range = true;  // false for extra ranges (split-off parts)
  bool has_code = true;       // false: a declaration only (start is 0); gcc sometimes emits a
                              // function's code without linking its entry to it
};

// Every function the debug info describes: those with code (one entry per address range), then
// declarations without an address (has_code false). Empty if there's no debug info. Throws IrError on
// malformed debug info (the caller decides whether to go on without it).
std::vector<Function> functions(const elf::File& f);

}  // namespace optum::dwarf
