// Generated code can nest expressions thousands deep (`a + b + c + ...`, `x.m().m().m()...`).
// The frontends must survive that instead of overflowing the stack.
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

#include "optum/frontend.hpp"

using namespace optum;
namespace fs = std::filesystem;

namespace {
std::string repeat(const std::string& s, int n) {
  std::string out;
  out.reserve(s.size() * static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) out += s;
  return out;
}

// Extracts a one-file project and returns the ids of its function nodes.
std::vector<std::string> functions(std::unique_ptr<Frontend> fe, const std::string& name,
                                   const std::string& source) {
  const fs::path dir = fs::temp_directory_path() / ("optum_deep_" + std::to_string(getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  std::ofstream(dir / name) << source;
  const Extraction x = fe->extract(dir, {name});
  fs::remove_all(dir);
  std::vector<std::string> out;
  for (const Node& n : x.graph.nodes()) {
    if (n.kind == NodeKind::Function) out.push_back(n.id);
  }
  return out;
}

bool has(const std::vector<std::string>& v, const std::string& id) {
  return std::find(v.begin(), v.end(), id) != v.end();
}
}  // namespace

TEST_CASE("python: very deep expressions don't overflow the stack") {
  const auto f = functions(make_python_frontend(1), "a.py",
                           "x = 1" + repeat(" + 1", 30000) + "\ny = a" + repeat(".m()", 20000) +
                               "\ndef after():\n    pass\n");
  REQUIRE(has(f, "py:a.after"));
}

TEST_CASE("js: very deep expressions don't overflow the stack") {
  const auto f = functions(make_js_frontend(1), "a.js",
                           "var x = 1" + repeat(" + 1", 30000) + ";\nvar y = a" + repeat(".m()", 20000) +
                               ";\nfunction after() {}\n");
  REQUIRE(has(f, "js:a.js#after"));
}

TEST_CASE("c: very deep expressions don't overflow the stack") {
  const auto f = functions(make_c_frontend(1), "a.c",
                           "int x = 1" + repeat(" + 1", 30000) + ";\nvoid after(void) {}\n");
  REQUIRE(has(f, "c:a.c#after"));
}
