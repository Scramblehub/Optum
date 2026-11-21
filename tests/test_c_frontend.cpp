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

TEST_CASE("c: a prototype or definition with a parenthesised name is that function") {
  TempDir dir;
  dir.write("api.h",
            "LUA_API int (lua_settop) (lua_State *L, int idx);\n"
            "LUA_API const char *(lua_tolstring) (lua_State *L, int idx);\n"
            "#define lua_pop(L,n) lua_settop(L, -(n)-1)\n");
  dir.write("api.c",
            "#include \"api.h\"\n"
            "LUALIB_API lua_Integer (luaL_len) (lua_State *L, int idx) { return 0; }\n"
            "LUALIB_API lua_Unsigned lua_rawlen (lua_State *L, int idx) { return 0; }\n"
            "LUA_API int (lua_settop) (lua_State *L, int idx) { return 0; }\n"
            "void use(lua_State *L) { lua_settop(L, 0); luaL_len(L, 1); lua_rawlen(L, 1); lua_pop(L, 1); }\n");
  const optum::Graph g = optum::extract_c_files(dir.path, {"api.h", "api.c"});
  const optum::Edge* settop = find_call(g, "c:api.c#use", "c:api.c#lua_settop");
  REQUIRE(settop);
  CHECK(settop->confidence == optum::Confidence::Exact);
  const optum::Edge* len = find_call(g, "c:api.c#use", "c:api.c#luaL_len");  // not a function named lua_Integer
  REQUIRE(len);
  CHECK(len->confidence == optum::Confidence::Exact);
  CHECK(find_call(g, "c:api.c#use", "c:api.c#lua_rawlen"));  // `API Type name (...)`, not a function named lua_Unsigned
}

TEST_CASE("c: NAME(args) { } inside a function body is not a function definition") {
  TempDir dir;
  dir.write("vm.c",
            "void op_a(void) {}\n"
            "void op_b(void) {}\n"
            "int run(int i) {\n"
            "  vmdispatch(GET_OPCODE(i)) {\n"
            "    vmcase(OP_A) { op_a(); vmbreak; }\n"
            "    vmcase(OP_B) { op_b(); vmbreak; }\n"
            "  }\n"
            "  return 0;\n"
            "}\n");
  const optum::Graph g = optum::extract_c_files(dir.path, {"vm.c"});
  CHECK(find_call(g, "c:vm.c#run", "c:vm.c#op_a"));
  CHECK(find_call(g, "c:vm.c#run", "c:vm.c#op_b"));
  for (const optum::Node& n : g.nodes()) {
    CHECK(n.name != "OP_A");
    CHECK(n.name != "vmdispatch");
  }
}

TEST_CASE("c: a prototype followed by an attribute macro with double parentheses is still a prototype") {
  TempDir dir;
  dir.write("h.h",
            "int table_init(table_t *t) TABLE_ATTRS((warn_unused_result));\n"
            "void table_close(table_t *t) TABLE_ATTRS((nonnull));\n");
  dir.write("t.c", "#include \"h.h\"\nint table_init(table_t *t) { return 0; }\nvoid table_close(table_t *t) {}\n");
  dir.write("u.c", "#include \"h.h\"\nvoid use(table_t *t) { table_init(t); table_close(t); }\n");
  const optum::Graph g = optum::extract_c_files(dir.path, {"h.h", "t.c", "u.c"});
  for (const char* to : {"c:t.c#table_init", "c:t.c#table_close"}) {
    const optum::Edge* e = find_call(g, "c:u.c#use", to);
    REQUIRE(e);
    CHECK(e->confidence == optum::Confidence::Exact);
  }
}

TEST_CASE("c: a comment in a backslash-continued macro body doesn't hide its calls") {
  TempDir dir;
  dir.write("m.c",
            "void grow(void) {}\n"
            "void push(void) {}\n"
            "#define check(L, n) \\\n"
            "  if ((n) > 1) { grow(); /* may move the stack */ } \\\n"
            "  else { (void)0; }\n"
            "#define PUSH(L) \\\n"
            "  /* multi\n"
            "     line */ push()\n"
            "void run(int *L) { check(L, 2); PUSH(L); }\n");
  const optum::Graph g = optum::extract_c_files(dir.path, {"m.c"});
  CHECK(find_call(g, "c:m.c#run", "c:m.c#grow"));
  CHECK(find_call(g, "c:m.c#run", "c:m.c#push"));
}

TEST_CASE("c: #ifdef lines inside an initializer list don't make their names values") {
  TempDir dir;
  dir.write("o.c",
            "static const char *const opts[] = {\n"
            "#ifdef FEATURE_ZED\n"
            "  \"ZED\",\n"
            "#endif\n"
            "  \"end\"\n"
            "};\n"
            "void zed(void) {}\n"
            "void run(void) {\n"
            "#ifdef FEATURE_ZED\n"
            "  zed();\n"
            "#endif\n"
            "}\n");
  const optum::Graph g = optum::extract_c_files(dir.path, {"o.c"});
  const optum::Edge* e = find_call(g, "c:o.c#run", "c:o.c#zed");
  REQUIRE(e);
  CHECK(e->confidence == optum::Confidence::Inferred);  // nobody defines FEATURE_ZED: off in this build
  CHECK(e->attrs.value("inactive", false));
}

TEST_CASE("c: a name another translation unit uses as a value doesn't make this file's #if unknown") {
  TempDir dir;
  // query.c prints config names (a value use of CFG_ZED); it isn't part of lib.c's build.
  dir.write("query.c", "int show(int x);\nint query(void) { return show(CFG_ZED); }\n");
  dir.write("lib.c",
            "void zed(void) {}\n"
            "void run(void) {\n"
            "#if defined(CFG_ZED)\n"
            "  zed();\n"
            "#endif\n"
            "}\n");
  dir.write("own.c",  // a value use in the same file: CFG_ZED may come from a system header
            "void zed2(void) {}\n"
            "int v(void) { return CFG_ZED; }\n"
            "void run2(void) {\n"
            "#if defined(CFG_ZED)\n"
            "  zed2();\n"
            "#endif\n"
            "}\n");
  const optum::Graph g = optum::extract_c_files(dir.path, {"query.c", "lib.c", "own.c"});
  const optum::Edge* off = find_call(g, "c:lib.c#run", "c:lib.c#zed");
  REQUIRE(off);
  CHECK(off->confidence == optum::Confidence::Inferred);
  CHECK(off->attrs.value("inactive", false));
  const optum::Edge* unknown = find_call(g, "c:own.c#run2", "c:own.c#zed2");
  REQUIRE(unknown);
  CHECK(unknown->confidence == optum::Confidence::Exact);
}

TEST_CASE("c: names on the continuation line of a #if are conditions, not values") {
  TempDir dir;
  // Cut from the sqlite amalgamation: the parser reads the second line of the #if as code.
  dir.write("o.c",
            "#if !defined(SQLITE_CORE) \\\n"
            "  || (defined(SQLITE_ENABLE_RTREE) && !defined(SQLITE_OMIT_VIRTUALTABLE))\n"
            "\n"
            "#ifndef SQLITE_CORE\n"
            "/*   #include \"sqlite3ext.h\" */\n"
            "  SQLITE_EXTENSION_INIT1\n"
            "#else\n"
            "/*   #include \"sqlite3.h\" */\n"
            "void zed(void) {}\n"
            "void run(void) {\n"
            "#if defined(SQLITE_OMIT_VIRTUALTABLE)\n"
            "  zed();\n"
            "#endif\n"
            "}\n");
  const optum::Graph g = optum::extract_c_files(dir.path, {"o.c"});
  const optum::Edge* e = find_call(g, "c:o.c#run", "c:o.c#zed");
  REQUIRE(e);
  CHECK(e->confidence == optum::Confidence::Inferred);
  CHECK(e->attrs.value("inactive", false));
}

TEST_CASE("c: a multi-line comment after a whole #define leaves the declarations below it alone") {
  TempDir dir;
  dir.write("z.h",
            "#define LIMIT_DEFAULT 27   /* by default, the decoder will refuse any frame\n"
            "                           * requiring more memory than this window\n"
            "                           */\n"
            "ZLIB_API size_t z_free(void* s);\n");
  dir.write("z.c", "#include \"z.h\"\nsize_t z_free(void* s) { return 0; }\n");
  dir.write("u.c", "#include \"z.h\"\nvoid use(void* s) { z_free(s); }\n");
  const optum::Graph g = optum::extract_c_files(dir.path, {"z.h", "z.c", "u.c"});
  const optum::Edge* e = find_call(g, "c:u.c#use", "c:z.c#z_free");
  REQUIRE(e);
  CHECK(e->confidence == optum::Confidence::Exact);  // declared: not name_only
}

TEST_CASE("c: a function renamed by a #define in a compiled branch is compiled, whatever an off branch defines") {
  TempDir dir;
  // sha256.c: the C version is `process_c`, renamed `process` unless the accelerated version is on.
  dir.write("sha.c",
            "#if defined(USE_ACCEL)\n"
            "#  if !defined(HAVE_NEON)\n"
            "#    undef USE_ACCEL\n"
            "#  endif\n"
            "#endif\n"
            "#if !defined(USE_ACCEL)\n"
            "#define process_c process\n"
            "#endif\n"
            "int process_c(int *ctx) { return 0; }\n"
            "#if defined(USE_ACCEL)\n"
            "int process(int *ctx) { return process_c(ctx); }\n"
            "#endif\n"
            "int update(int *ctx) { return process(ctx); }\n");
  const optum::Graph g = optum::extract_c_files(dir.path, {"sha.c"});
  const optum::Edge* e = find_call(g, "c:sha.c#update", "c:sha.c#process");
  REQUIRE(e);
  CHECK(e->confidence == optum::Confidence::Exact);
  CHECK_FALSE(e->attrs.value("inactive", false));
}

TEST_CASE("c: <assert.h> is never a vendored header that happens to share its name") {
  TempDir dir;
  dir.write("deps/lib/internal/assert.h", "#define assert(x) vendored_check(x)\nvoid vendored_check(int x);\n");
  dir.write("deps/lib/impl.c", "void vendored_check(int x) {}\n");
  dir.write("deps/lib/sub/d.c",  // inside the same vendored tree a quoted include may find it
            "#include \"internal/assert.h\"\nvoid d(int x) { assert(x); }\n");
  dir.write("src/a.c", "#include <assert.h>\nvoid f(int x) { assert(x); }\n");
  dir.write("src/b.c", "#include \"internal/assert.h\"\nvoid g(int x) { assert(x); }\n");  // not our tree
  const optum::Graph g = optum::extract_c_files(dir.path, {"deps/lib/internal/assert.h", "deps/lib/impl.c",
                                                            "deps/lib/sub/d.c", "src/a.c", "src/b.c"});
  CHECK_FALSE(find_call(g, "c:src/a.c#f", "c:deps/lib/impl.c#vendored_check"));
  CHECK_FALSE(find_call(g, "c:src/b.c#g", "c:deps/lib/impl.c#vendored_check"));
  CHECK(find_call(g, "c:deps/lib/sub/d.c#d", "c:deps/lib/impl.c#vendored_check"));
  // The project's own include/ directory still serves <angle> includes without a build database.
  TempDir own;
  own.write("include/lib/api.h", "void api_call(void);\n");
  own.write("src/api.c", "#include <lib/api.h>\nvoid api_call(void) {}\n");
  own.write("src/use.c", "#include <lib/api.h>\nvoid use(void) { api_call(); }\n");
  const optum::Graph h = optum::extract_c_files(own.path, {"include/lib/api.h", "src/api.c", "src/use.c"});
  const optum::Edge* e = find_call(h, "c:src/use.c#use", "c:src/api.c#api_call");
  REQUIRE(e);
  CHECK(e->confidence == optum::Confidence::Exact);
}

TEST_CASE("c: --compile-commands gives -D, -I (even outside the root) and the files in the build") {
  TempDir dir;
  const fs::path gen = dir.path.parent_path() / ("optum_c_gen_" + std::to_string(getpid()));
  fs::remove_all(gen);
  fs::create_directories(gen);
  std::ofstream(gen / "gen_config.h") << "#define HAVE_FEATURE 1\n";
  dir.write("src/a.c",
            "#include \"gen_config.h\"\n"
            "void dup_fn(void);\n"
            "void feat(void) {}\n"
            "void jem(void) {}\n"
            "void nope(void) {}\n"
            "void run(void) {\n"
            "#if defined(HAVE_FEATURE)\n"
            "  feat();\n"
            "#endif\n"
            "#ifdef USE_JEMALLOC\n"
            "  jem();\n"
            "#endif\n"
            "#ifdef NOT_SET\n"
            "  nope();\n"
            "#endif\n"
            "  dup_fn();\n"
            "}\n");
  dir.write("lib/other.c", "void dup_fn(void) {}\n");
  dir.write("alt/dup.c", "void dup_fn(void) {}\n");
  const std::string db = (dir.path / "cc.json").string();
  std::ofstream(db) << "[{\"directory\": \"" << dir.path.generic_string()
                    << "\", \"file\": \"src/a.c\", \"command\": \"cc -DUSE_JEMALLOC -I" << gen.generic_string()
                    << " -c src/a.c\"}, {\"directory\": \"" << dir.path.generic_string()
                    << "\", \"file\": \"lib/other.c\", \"command\": \"cc -c lib/other.c\"}]";
  auto edge = [&](const optum::Graph& g, const char* to) { return find_call(g, "c:src/a.c#run", to); };
  const std::vector<fs::path> files = {"src/a.c", "lib/other.c", "alt/dup.c"};

  optum::CStats with;
  const optum::Graph g = optum::extract_c_files(dir.path, files, &with, 0, db);
  REQUIRE(edge(g, "c:src/a.c#feat"));
  CHECK(edge(g, "c:src/a.c#feat")->confidence == optum::Confidence::Exact);  // from the generated header
  REQUIRE(edge(g, "c:src/a.c#jem"));
  CHECK(edge(g, "c:src/a.c#jem")->confidence == optum::Confidence::Exact);  // from -D
  REQUIRE(edge(g, "c:src/a.c#nope"));
  CHECK(edge(g, "c:src/a.c#nope")->attrs.value("inactive", false));  // off, now for certain
  REQUIRE(edge(g, "c:lib/other.c#dup_fn"));  // the definition that is in the build
  CHECK_FALSE(edge(g, "c:alt/dup.c#dup_fn"));
  CHECK(with.build_database);
  CHECK(with.external_headers == 1);
  CHECK(with.assumed_off_names == 0);

  optum::CStats without;
  const optum::Graph h = optum::extract_c_files(dir.path, files, &without, 0, "none");
  CHECK(edge(h, "c:src/a.c#feat")->attrs.value("inactive", false));  // a guess
  CHECK(edge(h, "c:src/a.c#jem")->attrs.value("inactive", false));
  CHECK_FALSE(without.build_database);
  CHECK(without.assumed_off_names >= 2);
  CHECK(without.assumed_off_edges >= 2);
  CHECK_FALSE(edge(h, "c:lib/other.c#dup_fn"));  // two definitions: ambiguous without the file list
  fs::remove_all(gen);
}
