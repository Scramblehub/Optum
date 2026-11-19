#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>

#include "optum/dwarf.hpp"
#include "optum/elf.hpp"

namespace {
// A function defined in this file, so the test can look for it by name.
int dwarf_test_marker(int x) { return x * 3 + 1; }
}  // namespace

TEST_CASE("dwarf: the test binary's own functions, with names and files") {
  REQUIRE(dwarf_test_marker(1) == 4);
  const auto f = optum::elf::read(std::filesystem::read_symlink("/proc/self/exe"));
  if (!f.section(".debug_info")) SKIP("built without debug info");
  const auto fns = optum::dwarf::functions(f);
  INFO("functions read: " << fns.size());
  REQUIRE(fns.size() > 100);
  const auto it = std::find_if(fns.begin(), fns.end(), [](const auto& fn) { return fn.name == "dwarf_test_marker"; });
  REQUIRE(it != fns.end());
  INFO("file: " << it->file);
  REQUIRE(it->file.ends_with("tests/test_dwarf.cpp"));
  REQUIRE(it->line == 11);
  REQUIRE(it->start != 0);
}
