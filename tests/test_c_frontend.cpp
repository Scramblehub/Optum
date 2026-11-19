#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <string>

#include "../Core/src/frontends/c/pp_expr.hpp"
#include "optum/frontends/c.hpp"

namespace fs = std::filesystem;
using optum::c_pp::Tri;

namespace {

// Names as in the default configuration of the tests: ON is defined to 2, OFF isn't, MAYBE is unknown.
optum::c_pp::Env test_env() {
  return {[](const std::string& n) { return n == "ON" ? Tri::True : n == "MAYBE" ? Tri::Unknown : Tri::False; },
          [](const std::string& n) -> std::optional<long long> {
            if (n == "ON") return 2;
            if (n == "MAYBE") return std::nullopt;
            return 0;
          }};
}

const optum::Edge* find_call(const optum::Graph& g, const std::string& from, const std::string& to) {
  for (const optum::Edge& e : g.edges()) {
    if (e.kind == optum::EdgeKind::Calls && g.node(e.from).id == from && g.node(e.to).id == to) return &e;
  }
  return nullptr;
}

struct TempDir {
  fs::path path = fs::temp_directory_path() / ("optum_c_config_" + std::to_string(getpid()));
  TempDir() {
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  void write(const std::string& rel, const std::string& text) const {
    fs::create_directories((path / rel).parent_path());
    std::ofstream(path / rel) << text;
  }
};

}  // namespace

TEST_CASE("c #if: expressions evaluate with unknowns where they can't be decided") {
  const auto env = test_env();
  CHECK(optum::c_pp::holds("defined(ON) && !defined(OFF)", env) == Tri::True);
  CHECK(optum::c_pp::holds("defined OFF", env) == Tri::False);
  CHECK(optum::c_pp::holds("ON == 2 && (OFF + 1) > 0", env) == Tri::True);
  CHECK(optum::c_pp::holds("0x10 >> 4 == 1L", env) == Tri::True);
  CHECK(optum::c_pp::holds("defined(MAYBE)", env) == Tri::Unknown);
  CHECK(optum::c_pp::holds("defined(OFF) && defined(MAYBE)", env) == Tri::False);  // 0 && x
  CHECK(optum::c_pp::holds("defined(ON) || MAYBE", env) == Tri::True);             // 1 || x
  CHECK(optum::c_pp::holds("MAYBE > 3", env) == Tri::Unknown);
  CHECK(optum::c_pp::holds("ON ? 1 : MAYBE", env) == Tri::True);
  CHECK(optum::c_pp::holds("FN(1) > 0", env) == Tri::Unknown);  // function-like macros aren't expanded
  CHECK(optum::c_pp::holds("1 / OFF", env) == Tri::Unknown);    // division by zero
  CHECK(optum::c_pp::holds("(1", env) == Tri::Unknown);         // doesn't parse
  CHECK(optum::c_pp::holds("ON /* comment */ == 2 // more", env) == Tri::True);
}

TEST_CASE("c #if: the line scan follows directives, not the syntax tree") {
  const auto pp = optum::c_pp::scan(
      "#ifndef GUARD_H\n"     // 1
      "#define GUARD_H\n"     // 2
      "#ifdef __cplusplus\n"  // 3
      "extern \"C\" {\n"      // 4
      "#endif\n"              // 5
      "int a;\n"              // 6
      "#if X > 1 \\\n"        // 7
      "    && Y\n"            // 8
      "int b;\n"              // 9
      "#elif defined(Z)\n"    // 10
      "int c;\n"              // 11
      "#else\n"               // 12
      "int d;\n"              // 13
      "#endif\n"              // 14
      "/*\n#if NOT_A_DIRECTIVE\n*/\n"  // 15-17
      "int e;\n"              // 18
      "#endif\n");            // 19
  // Line 6 is inside the include guard only: shown without a condition.
  REQUIRE(pp.at(6) >= 0);
  CHECK(pp.conds[std::size_t(pp.at(6))].text.empty());
  CHECK(pp.conds[std::size_t(pp.at(4))].text == "__cplusplus");
  CHECK(pp.conds[std::size_t(pp.at(9))].text == "X > 1 && Y");
  CHECK(pp.conds[std::size_t(pp.at(11))].text == "not (X > 1 && Y) && defined(Z)");
  CHECK(pp.conds[std::size_t(pp.at(13))].text == "not (not (X > 1 && Y) && defined(Z))");
  CHECK(pp.at(18) == pp.at(6));  // the commented-out #if opened nothing
  const auto& b13 = pp.branches[std::size_t(pp.conds[std::size_t(pp.at(13))].branches.back())];
  CHECK(b13.prior.size() == 2);
  CHECK(b13.expr.empty());
  const auto& guard = pp.branches[std::size_t(pp.conds[std::size_t(pp.at(6))].branches.front())];
  CHECK(guard.self == std::vector<std::string>{"GUARD_H"});
}

TEST_CASE("c: calls compiled out in the default configuration are inferred, with their condition") {
  const fs::path root = fs::path(OPTUM_FIXTURES) / "programs/c/config";
  const optum::Graph g = optum::extract_c_files(root, {"config.h", "main.c", "util.c"});

  auto exact = [&](const char* from, const char* to) {
    const optum::Edge* e = find_call(g, from, to);
    REQUIRE(e);
    CHECK(e->confidence == optum::Confidence::Exact);
    CHECK_FALSE(e->attrs.contains("inactive"));
    return e;
  };
  auto inactive = [&](const char* from, const char* to, const std::string& condition) {
    const optum::Edge* e = find_call(g, from, to);
    REQUIRE(e);
    CHECK(e->confidence == optum::Confidence::Inferred);
    CHECK(e->attrs.value("inactive", false));
    CHECK(e->attrs.value("condition", "") == condition);
  };

  // `#define buf_copy fast_copy` is in force: the call is fast_copy's; buf_copy() isn't compiled.
  exact("c:main.c#main", "c:util.c#fast_copy");
  inactive("c:main.c#main", "c:util.c#buf_copy", "!defined(buf_copy) && !HAVE_FAST_COPY");
  // TRACE() is the #else macro: log_line, not trace. An unconditional call wins over a
  // compiled-out one to the same function (no condition shown).
  CHECK_FALSE(exact("c:main.c#main", "c:util.c#log_line")->attrs.contains("condition"));
  inactive("c:main.c#main", "c:util.c#trace", "TRACE_ON");
  // check() is a function only with SELF_TEST, a macro otherwise.
  inactive("c:main.c#main", "c:main.c#check", "SELF_TEST");
  // An #if around `else if`: a call parsed under a function named `if` still counts, and the
  // branch is compiled (NO_SUM2 is never defined).
  CHECK(exact("c:util.c#checksum", "c:util.c#sum2")->attrs.value("condition", "") == "!NO_SUM2");
}

TEST_CASE("c: compile_commands.json gives the -D / -U flags of each file") {
  TempDir dir;
  dir.write("src/a.c",
            "#ifdef FEATURE\n"
            "static void f(void) {}\n"
            "#endif\n"
            "#if LEVEL > 1\n"
            "static void g(void) {}\n"
            "#endif\n"
            "#ifndef OPT_OUT\n"
            "static void h(void) {}\n"
            "#endif\n"
            "void run(void) {\n"
            "#ifdef FEATURE\n"
            "  f();\n"
            "#endif\n"
            "#if LEVEL > 1\n"
            "  g();\n"
            "#endif\n"
            "#ifndef OPT_OUT\n"
            "  h();\n"
            "#endif\n"
            "}\n");
  auto confidence = [&](const char* to) {
    const optum::Graph g = optum::extract_c_files(dir.path, {"src/a.c"});
    const optum::Edge* e = find_call(g, "c:src/a.c#run", to);
    REQUIRE(e);
    return e->confidence;
  };
  using optum::Confidence;
  // No compile_commands.json: none of the three is defined.
  CHECK(confidence("c:src/a.c#f") == Confidence::Inferred);
  CHECK(confidence("c:src/a.c#g") == Confidence::Inferred);
  CHECK(confidence("c:src/a.c#h") == Confidence::Exact);

  // "command", with the directory the file name is relative to.
  dir.write("build/compile_commands.json",
            "[{\"directory\": \"" + (dir.path / "src").generic_string() +
                "\", \"file\": \"a.c\", \"command\": \"cc -DFEATURE -D LEVEL=2 -DOPT_OUT -c a.c\"}]");
  CHECK(confidence("c:src/a.c#f") == Confidence::Exact);
  CHECK(confidence("c:src/a.c#g") == Confidence::Exact);
  CHECK(confidence("c:src/a.c#h") == Confidence::Inferred);

  // "arguments" at the root wins over build/; -U undoes an earlier -D.
  dir.write("compile_commands.json",
            "[{\"directory\": \"" + dir.path.generic_string() +
                "\", \"file\": \"src/a.c\", \"arguments\": [\"cc\", \"-DFEATURE\", \"-UFEATURE\", \"-DLEVEL=1\"]}]");
  CHECK(confidence("c:src/a.c#f") == Confidence::Inferred);
  CHECK(confidence("c:src/a.c#g") == Confidence::Inferred);
  CHECK(confidence("c:src/a.c#h") == Confidence::Exact);
}
