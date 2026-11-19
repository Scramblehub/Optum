#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "optum/elf.hpp"
#include "optum/ir.hpp"

using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {
fs::path write_tmp(const std::string& name, const std::string& bytes) {
  const fs::path p = fs::temp_directory_path() / ("optum_test_" + name);
  std::ofstream(p, std::ios::binary) << bytes;
  return p;
}
}  // namespace

TEST_CASE("elf: non-ELF and broken files are rejected cleanly") {
  const fs::path text = write_tmp("text", "hello world, not a binary");
  REQUIRE_FALSE(optum::elf::is_elf(text));
  REQUIRE_THROWS_WITH(optum::elf::read(text), ContainsSubstring("not an ELF file"));

  const fs::path tiny = write_tmp("tiny", std::string("\x7f" "ELF\x02\x01", 6));
  REQUIRE(optum::elf::is_elf(tiny));
  REQUIRE_THROWS_AS(optum::elf::read(tiny), optum::IrError);

  std::string elf32(64, '\0');
  elf32.replace(0, 6, std::string("\x7f" "ELF\x01\x01", 6));
  REQUIRE_THROWS_WITH(optum::elf::read(write_tmp("elf32", elf32)), ContainsSubstring("not a 64-bit"));
}

TEST_CASE("elf: reads the test binary itself") {
  // The test executable is an x86-64 ELF file built by this very build.
  const fs::path self = fs::read_symlink("/proc/self/exe");
  const auto f = optum::elf::read(self);
  REQUIRE(f.machine == 62);
  REQUIRE(f.has_interpreter);
  REQUIRE(f.section(".text"));
  REQUIRE_FALSE(f.symtab.empty());
  REQUIRE_FALSE(f.fdes.empty());
  REQUIRE_FALSE(f.needed.empty());
}
