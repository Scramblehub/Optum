#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

#include "optum/frontend.hpp"

using namespace optum;
namespace fs = std::filesystem;

namespace {
struct TempDir {
  fs::path path = fs::temp_directory_path() / ("optum_robust_" + std::to_string(getpid()));
  TempDir() {
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(path, ec))
      fs::permissions(e.path(), fs::perms::owner_all, fs::perm_options::add, ec);
    fs::remove_all(path, ec);
  }
  void write(const std::string& rel, const std::string& text) const {
    fs::create_directories((path / rel).parent_path());
    std::ofstream(path / rel) << text;
  }
};
}  // namespace

TEST_CASE("python: stats.files counts files read, not merged modules") {
  TempDir d;
  d.write("a.py", "def f():\n    pass\n");
  d.write("a/__init__.py", "x = 1\n");
  const auto x = make_python_frontend(1)->extract(d.path, {"a.py", "a/__init__.py"});
  REQUIRE(x.summary.starts_with("2 files, "));
}

TEST_CASE("frontends: an unreadable file is reported, not silently empty") {
  if (geteuid() == 0) SKIP("root can read any file");
  TempDir d;
  d.write("ok.py", "def f():\n    pass\n");
  d.write("secret.py", "def g():\n    pass\n");
  d.write("ok.js", "function f() {}\n");
  d.write("secret.js", "function g() {}\n");
  d.write("ok.c", "int f(void) { return 0; }\n");
  d.write("secret.c", "int g(void) { return 0; }\n");
  for (const char* f : {"secret.py", "secret.js", "secret.c"})
    fs::permissions(d.path / f, fs::perms::none);
  struct Case {
    std::unique_ptr<Frontend> fe;
    std::vector<fs::path> files;
  };
  std::vector<Case> cases;
  cases.push_back({make_python_frontend(1), {"ok.py", "secret.py"}});
  cases.push_back({make_js_frontend(1), {"ok.js", "secret.js"}});
  cases.push_back({make_c_frontend(1), {"ok.c", "secret.c"}});
  for (const auto& [fe, files] : cases) {
    CAPTURE(fe->info().name);
    const auto x = fe->extract(d.path, files);
    REQUIRE(x.summary.find("1 unreadable") != std::string::npos);
    REQUIRE(x.diagnostics.find("secret.") != std::string::npos);
  }
}
