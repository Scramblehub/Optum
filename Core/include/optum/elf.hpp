#pragma once
// A small reader for 64-bit little-endian ELF files (executables and shared libraries): what the
// binary frontend needs, nothing more. Every read is bounds-checked; malformed files throw IrError.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace optum::elf {

struct Section {
  std::string name;
  std::uint32_t type = 0;
  std::uint64_t flags = 0, addr = 0, offset = 0, size = 0, entsize = 0;
  std::uint32_t link = 0, info = 0;
  bool executable() const { return flags & 0x4; }  // SHF_EXECINSTR
};

struct Symbol {
  std::string name;
  std::uint64_t value = 0, size = 0;
  std::uint8_t type = 0;  // 2 = STT_FUNC
  std::uint8_t bind = 0;  // 0 = local, 1 = global, 2 = weak
  std::uint16_t shndx = 0;  // 0 = undefined (an import)
  std::string library;      // imports: the library its version names (GLIBC_2.2.5 -> libc.so.6)
  bool is_function() const { return type == 2; }
  bool defined() const { return shndx != 0; }
};

struct Fde {  // an .eh_frame unwind entry: one function (or function part)
  std::uint64_t start = 0, size = 0;
};

struct File {
  std::filesystem::path path;
  std::vector<std::uint8_t> data;
  std::uint16_t type = 0;  // 2 = ET_EXEC, 3 = ET_DYN
  std::uint16_t machine = 0;  // 62 = x86-64
  std::uint64_t entry = 0;
  bool has_interpreter = false;  // a program (also PIE), not just a shared library
  std::vector<Section> sections;
  std::vector<Symbol> symtab, dynsym;
  std::vector<std::string> needed;  // DT_NEEDED
  std::string soname;               // DT_SONAME
  std::unordered_map<std::uint64_t, std::string> got_imports;  // GOT slot -> imported function
  std::vector<std::uint64_t> init_array, fini_array;
  std::vector<std::uint64_t> relative_targets;  // R_X86_64_RELATIVE: addresses stored in data (pointers)
  std::unordered_map<std::uint64_t, std::uint64_t> relative_at;  // data slot -> the address it holds
  std::vector<std::string> runpath;  // DT_RUNPATH / DT_RPATH entries, as written ($ORIGIN unexpanded)
  std::vector<Fde> fdes;

  const Section* section(std::string_view name) const;
  // The pointer stored at a data address: from its relocation in position-independent code, else
  // the 8 bytes in the file. Nothing for addresses outside allocated, non-executable data.
  std::optional<std::uint64_t> pointer_at(std::uint64_t addr) const;
  // A NUL-terminated string at an address in read-only or initialised data, if it looks like text.
  std::optional<std::string> string_at(std::uint64_t addr, std::size_t max = 4096) const;
  const Section* section_at(std::uint64_t addr) const;
  std::span<const std::uint8_t> bytes(const Section& s) const;  // empty for SHT_NOBITS
};

// True if the file starts with the ELF magic. Cheap: reads 4 bytes.
bool is_elf(const std::filesystem::path& p);

// Reads an ELF64 little-endian file. Throws IrError for anything malformed or unsupported.
File read(const std::filesystem::path& p);

}  // namespace optum::elf
