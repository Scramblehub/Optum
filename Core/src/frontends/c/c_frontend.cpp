// C frontend. Rules: docs/frontends.md ("C").
//
// Pass 1 (parallel, per file): functions with their parameters and locals, includes, #defines,
// string constants, call sites, the #if around each (pp_expr.hpp), and every function name used
// other than as a callee (its address is taken, so it may be called through a pointer). Pass 2
// (parallel, per file): resolve names the way the linker would, decide which #ifs are compiled
// (Config), and turn libcurl / database calls into external and store nodes.

#include "optum/frontends/c.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "../../ts_util.hpp"
#include "../common.hpp"
#include "../names.hpp"
#include "../../test_code.hpp"
#include "pp_expr.hpp"
#include "optum/http.hpp"
#include "optum/sql.hpp"

extern "C" const TSLanguage* tree_sitter_c();

namespace optum {
namespace {

namespace fs = std::filesystem;
using frontends::parallel_for;
using frontends::http_external_node;
using frontends::read_file;
using frontends::sql_store_node;
using frontends::to_upper;

// A #define: function-like (the names its body calls) or object-like (its replacement text).
struct MacroDef {
  bool function_like = false;
  std::vector<std::string> calls;
  std::string value;
  int cond = -1;
};

// A string-ish value: literal pieces and holes (a name to resolve, or "" for unknown).
struct Str {
  std::vector<std::pair<bool, std::string>> parts;
};

struct Arg {
  std::string name;        // the argument is a plain identifier
  std::optional<Str> str;  // the argument is a string literal / concatenation
};

struct CallSite {
  int func = -1;
  int cond = -1;  // index in ModuleInfo::pp.conds, -1 outside any #if
  std::string callee;  // direct call by name ("" for a call through a pointer)
  std::size_t nargs = 0;
  std::vector<Arg> args;
  std::uint32_t line = 0;
  std::string condition;  // inside #if / #ifdef blocks: their conditions, joined with " && "
};

struct Func {
  std::string name;
  int cond = -1;
  bool is_static = false;
  int nparams = 0;  // -1: variadic
  std::uint32_t line = 1, end_line = 1;
  std::unordered_set<std::string> locals;
  std::unordered_map<std::string, Str> values;  // locals holding a known string (literals, snprintf)
};

struct Include {
  std::string spec;
  bool system = false;  // <...>
  std::uint32_t line = 0;
};

struct ModuleInfo {
  std::string path;
  std::size_t lines = 0;
  bool syntax_error = false;
  bool unreadable = false;
  std::vector<Func> funcs;
  frontends::NameUses names;  // name uses outside definitions
  bool test_file = false;
  std::vector<Include> includes;
  std::vector<CallSite> calls;
  std::unordered_map<std::string, Str> consts;  // file-scope string constants and string macros
  std::unordered_set<std::string> address_taken;
  std::unordered_set<std::string> prototypes;  // functions declared (not defined) here
  // Every #define, in file order (FLUSH_BLOCK(s, x) -> flush_pending; zmemcpy -> memcpy).
  std::unordered_map<std::string, std::vector<MacroDef>> defs;
  std::unordered_map<std::string, std::vector<int>> undefs;  // #undef NAME: where
  c_pp::Conditions pp;  // the #if structure; the `cond` fields above index pp.conds
  // -DNAME[=value] (nullopt: -UNAME) from compile_commands.json, if it compiles this file.
  std::unordered_map<std::string, std::optional<std::string>> flags;
  // The build's include directories for this file, in search order (absolute, normalised):
  // `quote_only` ones (-iquote) serve "x.h" includes only.
  struct IncDir {
    std::string path;
    bool quote_only = false;
  };
  std::vector<IncDir> inc_dirs;
  std::vector<std::string> forced_includes;  // -include FILE
  // `#define f_c f` where `f_c` is a function defined here: f -> (cond of f_c's definition, cond of the #define).
  std::unordered_map<std::string, std::vector<std::pair<int, int>>> renames;
  bool in_build = false;    // the file is in the compilation database
  bool external = false;    // read from outside the root (a generated header): defines, no nodes
};

// A /* comment */ in the middle of a #define (in a continued body, or with more text after it)
// makes the parser lose the macro body and the lines after it. Blank those comments out, keeping
// every byte offset and line: a comment that spans lines becomes a backslash-continued stretch of
// spaces. Comments after the whole directive, and in other directives, parse fine and stay.
void blank_directive_comments(std::string& s) {
  bool define = false, continued = false, line_start = true;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '\n') {
      const bool cont = i > 0 && (s[i - 1] == '\\' || (s[i - 1] == '\r' && i > 1 && s[i - 2] == '\\'));
      if (!cont) define = continued = false;
      else continued = true;
      line_start = true;
      continue;
    }
    if (line_start && (c == ' ' || c == '\t')) continue;
    if (line_start && c == '#') {
      std::size_t k = i + 1;
      while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) ++k;
      define = s.compare(k, 6, "define") == 0;
    }
    line_start = false;
    if (c == '"' || c == '\'') {  // string / char literal: no comments in it
      for (++i; i < s.size() && s[i] != c && s[i] != '\n'; ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] != '\n') ++i;
      }
      if (i < s.size() && s[i] == '\n') --i;
    } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
      while (i < s.size() && (s[i] != '\n' || s[i - 1] == '\\')) ++i;
      if (i < s.size()) --i;
    } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
      const std::size_t end = s.find("*/", i + 2);
      const std::size_t stop = end == std::string::npos ? s.size() : end + 2;
      if (define) {
        // What follows the comment on its line: nothing, a backslash, or more of the macro?
        std::size_t k = stop;
        while (k < s.size() && (s[k] == ' ' || s[k] == '\t' || s[k] == '\r')) ++k;
        const bool spans = s.find('\n', i) < stop;
        const bool more = k < s.size() && s[k] != '\n';  // includes a continuation backslash
        if (continued || more) {
          for (std::size_t j = i; j < stop; ++j) {
            if (s[j] == '\n') s[j - 1] = '\\';
            else if (s[j] != '\r') s[j] = ' ';
          }
          if (spans) continued = true;
        }
      }
      i = stop - 1;
    }
  }
}

// ===================================================================== pass 1

class Summarizer {
 public:
  Summarizer(ModuleInfo& m, std::string_view src) : m_(m), src_(src) {}
  void run(TSNode root) {
    m_.pp = c_pp::scan(src_);
    walk(root, -1);
  }

 private:
  ModuleInfo& m_;
  std::string_view src_;
  int depth_ = 0;  // recursion depth of walk()
  std::string text(TSNode n) const { return std::string(ts::text(n, src_)); }
  int cond_at(TSNode n) const { return m_.pp.at(ts::line(n)); }

  // The identifier a declarator declares, through pointers, arrays, parentheses and functions.
  TSNode declared(TSNode d) const {
    while (!ts_node_is_null(d) && !ts::is(d, "identifier")) {
      if (const TSNode name = parenthesised_name(d); !ts_node_is_null(name)) return name;
      if (ts::is(d, "parenthesized_declarator")) d = ts_node_named_child(d, 0);
      else {
        const TSNode inner = ts::field(d, "declarator");
        if (ts_node_is_null(inner)) return inner;
        d = inner;
      }
    }
    return d;
  }

  // `LUA_API int (lua_settop) (lua_State *L, int idx)` after an unknown macro reads as a function
  // `int` taking one parameter of type `lua_settop`, called with the real parameters. The name is
  // that "type": `(NAME)` with nothing else in it, behind a function declarator.
  static TSNode parenthesised_name(TSNode d) {
    if (!ts::is(d, "function_declarator")) return TSNode{};
    // `LUALIB_API lua_Integer luaL_len (...)`: two macros/types before the name; the parser takes the
    // first as the declarator and the name as a stray identifier before the parameters.
    TSNode stray{};
    ts::for_named(d, [&](TSNode c) {
      if (ts::is(c, "ERROR") && ts_node_named_child_count(c) == 1 && ts::is(ts_node_named_child(c, 0), "identifier")) {
        stray = ts_node_named_child(c, 0);
      }
    });
    if (!ts_node_is_null(stray) && ts::is(ts::field(d, "declarator"), "identifier")) return stray;
    const TSNode inner = ts::field(d, "declarator");
    if (!ts::is(inner, "function_declarator") || !ts::is(ts::field(inner, "declarator"), "identifier")) return TSNode{};
    const TSNode params = ts::field(inner, "parameters");
    if (ts_node_is_null(params) || ts_node_named_child_count(params) != 1) return TSNode{};
    const TSNode p = ts_node_named_child(params, 0);
    if (!ts::is(p, "parameter_declaration") || !ts_node_is_null(ts::field(p, "declarator"))) return TSNode{};
    const TSNode type = ts::field(p, "type");
    return ts::is(type, "type_identifier") ? type : TSNode{};
  }

  TSNode function_declarator(TSNode d) const {
    while (!ts_node_is_null(d) && !ts::is(d, "function_declarator")) {
      if (ts::is(d, "parenthesized_declarator")) d = ts_node_named_child(d, 0);
      else d = ts::field(d, "declarator");
    }
    return d;
  }

  bool has_static(TSNode n) const {
    bool s = false;
    ts::for_named(n, [&](TSNode c) { s = s || (ts::is(c, "storage_class_specifier") && ts::text(c, src_) == "static"); });
    return s;
  }

  // An identifier on a directive line (`#ifdef NAME` inside an initializer list, which the parser
  // reads as an expression: `{ #ifdef NAME "x", ...`) is a condition's name, not a value.
  bool on_directive_line(TSNode n) const {
    std::size_t i = ts_node_start_byte(n);
    while (i > 0 && src_[i - 1] != '\n') --i;
    // A backslash-continued directive: its first line.
    while (i >= 2 && (src_[i - 2] == '\\' || (i >= 3 && src_[i - 2] == '\r' && src_[i - 3] == '\\'))) {
      i -= src_[i - 2] == '\r' ? 3 : 2;
      while (i > 0 && src_[i - 1] != '\n') --i;
    }
    while (i < src_.size() && (src_[i] == ' ' || src_[i] == '\t')) ++i;
    return i < src_.size() && src_[i] == '#';
  }

  void walk(TSNode n, int f) {
    const frontends::DepthGuard guard(depth_);
    if (guard.too_deep()) return;
    const std::string_view t = ts_node_type(n);
    if (t == "function_definition") return on_function(n, f);
    if (t == "declaration") return on_declaration(n, f);
    if (t == "call_expression") return on_call(n, f);
    if (t == "ERROR" && f < 0) on_misparsed_prototype(n);
    if (t == "preproc_include") return on_include(n);
    if (t == "preproc_def") return on_define(n);
    if (t == "preproc_function_def") return on_macro(n);
    if (t == "preproc_call") return on_directive(n);
    if (t == "preproc_ifdef" || t == "preproc_if" || t == "preproc_elif" || t == "preproc_else" ||
        t == "preproc_elifdef") {
      return on_conditional(n, f);
    }
    if (t == "identifier") {
      if (!on_directive_line(n)) m_.address_taken.insert(text(n));  // a name used as a value (filtered to functions later)
      return;
    }
    if (t == "parameter_list" || t == "type_definition" ||
        t == "struct_specifier" || t == "union_specifier" || t == "enum_specifier") {
      return;  // declarations of types and macros: no values in them
    }
    ts::for_named(n, [&](TSNode c) { walk(c, f); });
  }

  void on_function(TSNode n, int f) {
    const TSNode decl = ts::field(n, "declarator");
    const TSNode fd = function_declarator(decl);
    const TSNode id = declared(decl);
    // `vmcase(OP_X) { ... }` in a function body is a statement macro with a block, read as a
    // definition: `NAME` is its "type" and `(args)` its whole declarator. Its body is the enclosing one's.
    TSNode top = decl;
    while (ts::is(top, "pointer_declarator")) top = ts::field(top, "declarator");
    if (f >= 0 && ts::is(top, "parenthesized_declarator")) {
      walk(ts::field(n, "body"), f);
      return;
    }
    if (ts_node_is_null(id)) return;
    static const std::unordered_set<std::string> kKeywords = {
        "if", "else", "while", "for", "do", "switch", "case", "return", "sizeof", "goto", "break", "continue",
        "default", "typedef", "struct", "union", "enum", "static", "extern", "const", "volatile"};
    if (kKeywords.count(text(id))) {
      // Error recovery, not a function: `#ifdef GZIP / else if (x) { crc32(...); } / #endif` in
      // a function body reads as a function named `if`. Its body is still the enclosing function's.
      if (f >= 0) walk(ts::field(n, "body"), f);
      return;
    }
    Func fn;
    fn.name = text(id);
    fn.cond = cond_at(n);
    fn.is_static = has_static(n);
    fn.line = ts::line(n);
    fn.end_line = ts::end_line(n);
    if (!ts_node_is_null(fd)) {
      int count = 0;
      bool variadic = false, only_void = false;
      ts::for_named(ts::field(fd, "parameters"), [&](TSNode p) {
        if (ts::is(p, "variadic_parameter")) variadic = true;
        if (!ts::is(p, "parameter_declaration")) return;
        ++count;
        const TSNode pid = declared(ts::field(p, "declarator"));
        if (!ts_node_is_null(pid)) fn.locals.insert(text(pid));
        else if (ts::text(ts::field(p, "type"), src_) == "void") only_void = true;
      });
      fn.nparams = variadic ? -1 : (only_void && count == 1) ? 0 : count;
    }
    m_.funcs.push_back(std::move(fn));
    const int idx = static_cast<int>(m_.funcs.size()) - 1;
    walk(ts::field(n, "body"), idx);
  }

  // `int f(x) ATTR((warn_unused_result));` at file scope: the trailing attribute macro makes the
  // parser give up on the declaration, but the function declarator in it is intact.
  void on_misparsed_prototype(TSNode n) {
    ts::for_named(n, [&](TSNode c) {
      if (!ts::is(c, "function_declarator")) return;
      const TSNode id = declared(c);
      if (!ts_node_is_null(id)) m_.prototypes.insert(text(id));
    });
  }

  void on_declaration(TSNode n, int f) {
    ts::for_field(n, "declarator", [&](TSNode d) {
      if (!ts_node_is_null(function_declarator(d)) && !ts::is(d, "init_declarator")) {  // a prototype
        const TSNode pid = declared(d);
        if (!ts_node_is_null(pid)) m_.prototypes.insert(text(pid));
        // Old-style wrappers: `void f OF((int x));` parses as a prototype named OF of type f.
        const TSNode type = ts::field(n, "type");
        if (ts::is(type, "type_identifier")) m_.prototypes.insert(text(type));
        return;
      }
      const TSNode id = declared(ts::is(d, "init_declarator") ? ts::field(d, "declarator") : d);
      const TSNode value = ts::is(d, "init_declarator") ? ts::field(d, "value") : TSNode{};
      if (!ts_node_is_null(id)) {
        const std::string name = text(id);
        std::optional<Str> s = ts_node_is_null(value) ? std::nullopt : to_str(value);
        if (f >= 0) {
          m_.funcs[std::size_t(f)].locals.insert(name);
          if (s) m_.funcs[std::size_t(f)].values[name] = *s;
        } else if (s) {
          m_.consts[name] = *s;
        }
      }
      if (!ts_node_is_null(value)) walk(value, f);
    });
  }

  void on_call(TSNode n, int f) {
    const TSNode fn = ts::field(n, "function");
    // A "call" at file scope is a prototype the parser misread after an unknown macro
    // (`void EXPORT f(int x);`): there are no calls outside functions in C.
    if (f < 0) {
      if (ts::is(fn, "identifier")) m_.prototypes.insert(text(fn));
      return;
    }
    const TSNode args = ts::field(n, "arguments");
    CallSite cs;
    cs.func = f;
    cs.line = ts::line(n);
    cs.cond = cond_at(n);
    if (cs.cond >= 0) cs.condition = m_.pp.conds[std::size_t(cs.cond)].text;
    if (ts::is(fn, "identifier")) {
      const std::string name = text(fn);
      const bool local = f >= 0 && m_.funcs[std::size_t(f)].locals.count(name);
      if (!local) cs.callee = name;  // a call through a local function pointer stays indirect
    } else {
      walk(fn, f);  // s->ops[i].fn(x): the parts may hold calls and values
    }
    ts::for_named(args, [&](TSNode a) {
      if (ts::is(a, "comment")) return;
      ++cs.nargs;
      Arg arg;
      if (ts::is(a, "identifier")) arg.name = text(a);
      arg.str = to_str(a);
      cs.args.push_back(std::move(arg));
      walk(a, f);
    });
    // snprintf(buf, size, "fmt %s", ...) / sprintf(buf, "fmt", ...): buf now holds the format.
    if (f >= 0 && (cs.callee == "snprintf" || cs.callee == "sprintf" || cs.callee == "strcpy")) {
      const std::size_t fmt = cs.callee == "snprintf" ? 2 : 1;
      if (cs.args.size() > fmt && !cs.args[0].name.empty() && cs.args[fmt].str) {
        Str s = *cs.args[fmt].str;
        if (cs.callee != "strcpy") s = printf_template(s);
        m_.funcs[std::size_t(f)].values[cs.args[0].name] = s;
      }
    }
    if (f >= 0) m_.calls.push_back(std::move(cs));
  }

  // #if / #ifdef / #elif / #else: their conditions come from the line scan (c_pp::scan), so
  // only the bodies are walked here.
  void on_conditional(TSNode n, int f) {
    ts::for_named(n, [&](TSNode c) {
      if (c.id == ts::field(n, "condition").id || c.id == ts::field(n, "name").id) return;
      const std::string_view ct = ts_node_type(c);
      if (ct == "preproc_elif" || ct == "preproc_else" || ct == "preproc_elifdef") on_conditional(c, f);
      else walk(c, f);
    });
  }

  void on_directive(TSNode n) {
    if (ts::text(ts::field(n, "directive"), src_) != "#undef") return;
    std::string name = text(ts::field(n, "argument"));
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
    if (!name.empty()) m_.undefs[name].push_back(cond_at(n));
  }

  void on_macro(TSNode n) {
    const std::string body = text(ts::field(n, "value"));
    std::vector<std::string> called;
    for (std::size_t i = 0; i < body.size();) {
      if (std::isalpha(static_cast<unsigned char>(body[i])) || body[i] == '_') {
        std::size_t j = i;
        while (j < body.size() && (std::isalnum(static_cast<unsigned char>(body[j])) || body[j] == '_')) ++j;
        std::size_t k = j;
        while (k < body.size() && (body[k] == ' ' || body[k] == '\t')) ++k;
        if (k < body.size() && body[k] == '(') called.push_back(body.substr(i, j - i));
        i = j;
      } else if (body[i] == '"') {  // skip string literals
        for (++i; i < body.size() && body[i] != '"'; ++i) {
          if (body[i] == '\\') ++i;
        }
        ++i;
      } else {
        ++i;
      }
    }
    MacroDef d;
    d.function_like = true;
    d.calls = std::move(called);
    d.cond = cond_at(n);
    m_.defs[text(ts::field(n, "name"))].push_back(std::move(d));
  }

  void on_include(TSNode n) {
    const TSNode p = ts::field(n, "path");
    std::string spec = text(p);
    const bool system = ts::is(p, "system_lib_string");
    if (spec.size() >= 2 && (spec.front() == '"' || spec.front() == '<')) spec = spec.substr(1, spec.size() - 2);
    m_.includes.push_back({spec, system, ts::line(n)});
  }

  // #define NAME "literal" ... : kept as a string constant.
  void on_define(TSNode n) {
    const std::string name = text(ts::field(n, "name"));
    std::string v = text(ts::field(n, "value"));
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
    MacroDef d;
    d.value = v;
    d.cond = cond_at(n);
    m_.defs[name].push_back(std::move(d));
    Str s;
    std::size_t i = 0;
    while (i < v.size()) {
      while (i < v.size() && std::isspace(static_cast<unsigned char>(v[i]))) ++i;
      if (i >= v.size()) break;
      if (v[i] != '"') return;  // not a plain string macro
      std::string piece;
      for (++i; i < v.size() && v[i] != '"'; ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) ++i;
        piece += v[i];
      }
      ++i;
      s.parts.emplace_back(false, piece);
    }
    if (!s.parts.empty()) m_.consts[name] = std::move(s);
  }

  std::optional<Str> to_str(TSNode n) const {
    if (ts::is(n, "string_literal")) {
      Str s;
      std::string piece;
      ts::for_named(n, [&](TSNode c) {
        if (ts::is(c, "string_content") || ts::is(c, "escape_sequence")) piece += text(c);
      });
      s.parts.emplace_back(false, piece);
      return s;
    }
    if (ts::is(n, "concatenated_string")) {  // "a" "b", or MACRO "b"
      Str s;
      ts::for_named(n, [&](TSNode c) {
        if (ts::is(c, "identifier")) s.parts.emplace_back(true, text(c));
        else if (auto inner = to_str(c)) s.parts.insert(s.parts.end(), inner->parts.begin(), inner->parts.end());
      });
      return s;
    }
    if (ts::is(n, "identifier")) {
      Str s;
      s.parts.emplace_back(true, text(n));
      return s;
    }
    if (ts::is(n, "parenthesized_expression") || ts::is(n, "cast_expression")) {
      const uint32_t k = ts_node_named_child_count(n);
      if (k) return to_str(ts_node_named_child(n, k - 1));
    }
    return std::nullopt;
  }

  // printf conversions (%s, %d, %08lx, %%...) become holes.
  static Str printf_template(const Str& in) {
    Str out;
    for (const auto& [hole, piece] : in.parts) {
      if (hole) {
        out.parts.emplace_back(true, piece);
        continue;
      }
      std::string lit;
      for (std::size_t i = 0; i < piece.size(); ++i) {
        if (piece[i] != '%') {
          lit += piece[i];
          continue;
        }
        if (i + 1 < piece.size() && piece[i + 1] == '%') {
          lit += '%';
          ++i;
          continue;
        }
        std::size_t j = i + 1;
        while (j < piece.size() && std::strchr("-+ #0123456789.*hlLqjzt", piece[j])) ++j;
        if (j < piece.size()) {
          out.parts.emplace_back(false, lit);
          lit.clear();
          out.parts.emplace_back(true, "");
          i = j;
        }
      }
      out.parts.emplace_back(false, lit);
    }
    return out;
  }
};

// ===================================================================== pass 2

bool is_system_header(const std::string& h) {
  static const std::unordered_set<std::string> kStd = {
      "assert.h", "complex.h", "ctype.h", "errno.h", "fenv.h", "float.h", "inttypes.h", "iso646.h",
      "limits.h", "locale.h", "math.h", "setjmp.h", "signal.h", "stdalign.h", "stdarg.h", "stdatomic.h",
      "stdbool.h", "stddef.h", "stdint.h", "stdio.h", "stdlib.h", "stdnoreturn.h", "string.h", "tgmath.h",
      "threads.h", "time.h", "uchar.h", "wchar.h", "wctype.h", "aio.h", "cpio.h", "dirent.h", "dlfcn.h",
      "fcntl.h", "fmtmsg.h", "fnmatch.h", "ftw.h", "glob.h", "grp.h", "iconv.h", "langinfo.h", "libgen.h",
      "monetary.h", "mqueue.h", "ndbm.h", "netdb.h", "nl_types.h", "poll.h", "pthread.h", "pwd.h", "regex.h",
      "sched.h", "search.h", "semaphore.h", "spawn.h", "strings.h", "syslog.h", "tar.h", "termios.h",
      "unistd.h", "utime.h", "utmpx.h", "wordexp.h", "endian.h", "malloc.h", "alloca.h", "getopt.h",
      "execinfo.h", "err.h", "error.h", "features.h", "ifaddrs.h", "link.h", "elf.h", "shadow.h"};
  for (const char* dir : {"sys/", "arpa/", "net/", "netinet/", "linux/", "asm/", "bits/", "gnu/", "asm-generic/"}) {
    if (h.starts_with(dir)) return true;
  }
  return kStd.count(h) > 0;
}

struct FnRef {
  int mod = -1;
  int fn = -1;
  bool declared = true;  // found the way the linker would (same file, included header, or a visible prototype)
};

class Project {
 public:
  std::vector<ModuleInfo> mods;
  std::string root_abs_;  // the root, absolute, to turn build include paths into project paths
  std::vector<ModuleInfo::IncDir> global_dirs_;  // the build's include dirs, for files it doesn't list
  bool has_db = false;

  void index_paths() {
    for (int i = 0; i < static_cast<int>(mods.size()); ++i) add_path(i);
  }
  void add_module(ModuleInfo m) {
    mods.push_back(std::move(m));
    add_path(static_cast<int>(mods.size()) - 1);
  }

  void index() {
    for (ModuleInfo& m : mods) {
      for (const Func& fn : m.funcs) {
        const auto it = m.defs.find(fn.name);
        if (it == m.defs.end()) continue;
        for (const MacroDef& d : it->second) {
          if (!d.function_like && !d.value.empty()) m.renames[d.value].emplace_back(fn.cond, d.cond);
        }
      }
    }
    for (int m = 0; m < static_cast<int>(mods.size()); ++m) {
      for (int f = 0; f < static_cast<int>(mods[std::size_t(m)].funcs.size()); ++f) {
        const Func& fn = mods[std::size_t(m)].funcs[std::size_t(f)];
        if (!fn.is_static && !mods[std::size_t(m)].external) globals_[fn.name].push_back({m, f});
      }
    }
    for (const ModuleInfo& mod : mods) {
      // A .c file's #defines reach only that file (unless it's included, and then it's visible).
      if (!mod.path.ends_with(".c")) {
        for (const auto& [name, d] : mod.defs) defined_in_header_.insert(name);
      }
    }
    // Resolved includes, then each file's headers seen through includes of includes.
    includes_.resize(mods.size());
    for (int m = 0; m < static_cast<int>(mods.size()); ++m) {
      for (const Include& inc : mods[std::size_t(m)].includes) {
        if (const int h = resolve_include(m, inc); h >= 0 && h != m) includes_[std::size_t(m)].push_back(h);
      }
    }
    visible_.resize(mods.size());
    for (int m = 0; m < static_cast<int>(mods.size()); ++m) {
      std::vector<int> todo = includes_[std::size_t(m)];
      std::unordered_set<int> seen{m};
      while (!todo.empty()) {
        const int h = todo.back();
        todo.pop_back();
        if (!seen.insert(h).second) continue;
        visible_[std::size_t(m)].push_back(h);
        for (int k : includes_[std::size_t(h)]) todo.push_back(k);
      }
    }
    // Functions whose address is taken: statics only where named, others anywhere.
    for (int m = 0; m < static_cast<int>(mods.size()); ++m) {
      const ModuleInfo& mod = mods[std::size_t(m)];
      for (const std::string& name : mod.address_taken) {
        const FnRef r = resolve_call(m, name);
        if (r.mod >= 0) address_taken_.push_back(r);
      }
    }
    std::sort(address_taken_.begin(), address_taken_.end(),
              [](const FnRef& a, const FnRef& b) { return std::tie(a.mod, a.fn) < std::tie(b.mod, b.fn); });
    address_taken_.erase(std::unique(address_taken_.begin(), address_taken_.end(),
                                     [](const FnRef& a, const FnRef& b) { return a.mod == b.mod && a.fn == b.fn; }),
                         address_taken_.end());
  }

  const Func& fn(FnRef r) const { return mods[std::size_t(r.mod)].funcs[std::size_t(r.fn)]; }
  std::string id(int mod) const { return "c:" + mods[std::size_t(mod)].path; }
  std::string id(FnRef r) const { return id(r.mod) + "#" + fn(r).name; }

  // Where `#include` finds a file, in the compiler's order: the includer's directory ("quoted" only),
  // the build's -iquote / -I / -isystem directories, and without any of those the project's
  // conventional include roots. <system> headers never match by file name alone: a vendored
  // deps/x/assert.h must not stand in for libc's. As a last resort a "quoted" include matches a
  // unique file by its trailing path, but only inside the includer's own tree (not a vendored one).
  int resolve_include(int from, const Include& inc) const {
    const ModuleInfo& m = mods[std::size_t(from)];
    if (fs::path(inc.spec).is_absolute()) return lookup(fs::path(inc.spec));
    if (!inc.system) {
      if (const int h = lookup(fs::path(m.path).parent_path() / inc.spec); h >= 0) return h;
    }
    const auto& dirs = m.inc_dirs.empty() ? global_dirs_ : m.inc_dirs;
    for (const ModuleInfo::IncDir& d : dirs) {
      if (inc.system && d.quote_only) continue;
      if (const int h = lookup(fs::path(d.path) / inc.spec); h >= 0) return h;
    }
    if (!inc.system) {
      if (const int h = lookup(fs::path(inc.spec)); h >= 0) return h;
    }
    if (dirs.empty()) {  // no build information: the usual places
      for (const char* dir : {"", "include", "inc", "src", "lib", "source"}) {
        if (const int h = lookup(fs::path(dir) / inc.spec); h >= 0) return h;
      }
    }
    if (inc.system) return -1;
    int hit = -1;
    const auto it = by_name_.find(fs::path(inc.spec).filename().generic_string());
    if (it == by_name_.end()) return -1;
    const std::string suffix = "/" + fs::path(inc.spec).lexically_normal().generic_string();
    for (int i : it->second) {
      const std::string& path = mods[std::size_t(i)].path;
      if (path.size() > suffix.size() && path.ends_with(suffix) && tree_of(path) == tree_of(m.path)) {
        if (hit >= 0) return -1;
        hit = i;
      }
    }
    return hit;
  }

  void add_path(int i) {
    by_path_.emplace(mods[std::size_t(i)].path, i);
    by_name_[fs::path(mods[std::size_t(i)].path).filename().generic_string()].push_back(i);
  }

  // The module at a path (absolute, or relative to the root), -1 if it isn't one.
  int lookup(const fs::path& p) const {
    std::string key = p.lexically_normal().generic_string();
    if (p.is_absolute() && key.size() > root_abs_.size() && key.starts_with(root_abs_) && key[root_abs_.size()] == '/') {
      key = key.substr(root_abs_.size() + 1);
    }
    const auto it = by_path_.find(key);
    return it == by_path_.end() ? -1 : it->second;
  }

  // A vendored tree: the directory after deps/, vendor/, third_party/ ... ("" for the project's own).
  static std::string tree_of(const std::string& path) {
    static const std::unordered_set<std::string> kVendor = {"deps", "vendor", "vendored", "third_party", "thirdparty",
                                                            "3rdparty", "external", "extern", "node_modules"};
    std::string prefix;
    std::size_t start = 0;
    bool next = false;
    while (start < path.size()) {
      const std::size_t slash = path.find('/', start);
      if (slash == std::string::npos) break;  // the file name
      const std::string part = path.substr(start, slash - start);
      prefix += part + "/";
      if (next) return prefix;
      if (kVendor.count(part)) next = true;
      start = slash + 1;
    }
    return next ? prefix : std::string();
  }

  // Like the linker: this file, then headers it includes, then a unique external definition
  // (preferring one in the same directory when several programs share a repo).
  FnRef resolve_call(int m, const std::string& name) const {
    auto in = [&](int mod) -> int {
      const auto& fs_ = mods[std::size_t(mod)].funcs;
      for (int f = 0; f < static_cast<int>(fs_.size()); ++f) {
        if (fs_[std::size_t(f)].name == name) return f;
      }
      return -1;
    };
    if (int f = in(m); f >= 0) return {m, f};
    for (int h : visible_[std::size_t(m)]) {
      if (int f = in(h); f >= 0) return {h, f};
    }
    auto it = globals_.find(name);
    if (it == globals_.end()) return {};
    // With a compilation database, a file in the build links only against definitions that are in it
    // (a libc name defined by os390-syscalls.c, a function in the win/ tree: not in this build).
    std::vector<FnRef> cands = it->second;
    if (has_db && mods[std::size_t(m)].in_build) {
      cands.clear();
      for (const FnRef& r : it->second) {
        if (mods[std::size_t(r.mod)].in_build || !mods[std::size_t(r.mod)].path.ends_with(".c")) cands.push_back(r);
      }
      if (cands.empty()) return {};
    }
    // A definition elsewhere counts as found only if this file can see a prototype for it;
    // otherwise it may belong to another program in the same repo (examples/, contrib/).
    bool proto = mods[std::size_t(m)].prototypes.count(name) > 0;
    for (int h : visible_[std::size_t(m)]) proto = proto || mods[std::size_t(h)].prototypes.count(name) > 0;
    auto with = [&](FnRef r) {
      r.declared = proto;
      return r;
    };
    if (cands.size() == 1) return with(cands[0]);
    const fs::path dir = fs::path(mods[std::size_t(m)].path).parent_path();
    FnRef same_dir;
    int count = 0;
    for (const FnRef& r : cands) {
      if (fs::path(mods[std::size_t(r.mod)].path).parent_path() == dir) {
        same_dir = r;
        ++count;
      }
    }
    return count == 1 ? with(same_dir) : FnRef{};
  }

  // The #defines of `name` visible from module m: its own file, then included headers.
  struct DefRef {
    int mod;
    const MacroDef* def;
  };
  std::vector<DefRef> defs(int m, const std::string& name) const {
    std::vector<DefRef> out;
    auto add = [&](int mod) {
      const auto& d = mods[std::size_t(mod)].defs;
      if (auto it = d.find(name); it != d.end()) {
        for (const MacroDef& def : it->second) out.push_back({mod, &def});
      }
    };
    add(m);
    for (int h : visible_[std::size_t(m)]) add(h);
    return out;
  }
  // Where `name` is #undef'd, visible from module m: (module, cond) pairs.
  std::vector<std::pair<int, int>> undefs(int m, const std::string& name) const {
    std::vector<std::pair<int, int>> out;
    auto add = [&](int mod) {
      const auto& u = mods[std::size_t(mod)].undefs;
      if (auto it = u.find(name); it != u.end()) {
        for (int c : it->second) out.emplace_back(mod, c);
      }
    };
    add(m);
    for (int h : visible_[std::size_t(m)]) add(h);
    return out;
  }
  bool defined_in_header(const std::string& name) const { return defined_in_header_.count(name) > 0; }
  // Module m, or a header it sees, uses `name` as a value (a file outside this translation unit, like
  // a test program that prints config names, says nothing about where the name comes from).
  bool used_in_code(int m, const std::string& name) const {
    if (mods[std::size_t(m)].address_taken.count(name)) return true;
    for (int h : visible_[std::size_t(m)]) {
      if (mods[std::size_t(h)].address_taken.count(name)) return true;
    }
    return false;
  }
  // Module m, or a header it sees, includes <header>.
  bool sees_system_header(int m, const std::string& header) const {
    auto has = [&](int mod) {
      for (const Include& inc : mods[std::size_t(mod)].includes) {
        if (inc.system && inc.spec == header) return true;
      }
      return false;
    };
    if (has(m)) return true;
    for (int h : visible_[std::size_t(m)]) {
      if (has(h)) return true;
    }
    return false;
  }

  const std::vector<FnRef>& address_taken() const { return address_taken_; }
  const std::vector<int>& includes(int m) const { return includes_[std::size_t(m)]; }

  std::optional<std::string> eval(int m, int func, const Str& s, int depth = 0) const {
    if (depth > 8) return std::nullopt;
    std::string out;
    for (const auto& [hole, piece] : s.parts) {
      if (!hole) {
        out += piece;
        continue;
      }
      std::optional<std::string> v;
      if (!piece.empty()) v = eval_name(m, func, piece, depth + 1);
      out += v ? *v : "{}";
    }
    return out;
  }

  std::optional<std::string> eval_name(int m, int func, const std::string& name, int depth) const {
    const ModuleInfo& mod = mods[std::size_t(m)];
    if (func >= 0) {
      const Func& f = mod.funcs[std::size_t(func)];
      if (auto it = f.values.find(name); it != f.values.end()) return eval(m, func, it->second, depth);
      if (f.locals.count(name)) return std::nullopt;
    }
    if (auto it = mod.consts.find(name); it != mod.consts.end()) return eval(m, -1, it->second, depth);
    for (int h : visible_[std::size_t(m)]) {
      const auto& c = mods[std::size_t(h)].consts;
      if (auto it = c.find(name); it != c.end()) return eval(h, -1, it->second, depth);
    }
    return std::nullopt;
  }

 private:
  std::unordered_map<std::string, int> by_path_;
  std::unordered_map<std::string, std::vector<int>> by_name_;
  std::unordered_map<std::string, std::vector<FnRef>> globals_;
  std::unordered_set<std::string> defined_in_header_;
  std::vector<std::vector<int>> includes_, visible_;
  std::vector<FnRef> address_taken_;
};

// The build configuration: the file's -D / -U flags from compile_commands.json (none without
// one), GCC or Clang on Linux. Decides whether an #if branch is compiled, from those flags and the
// project's own #defines that a translation unit can see:
//   - a name the project #defines is defined when one of its visible #defines is in a compiled
//     branch (while that is decided, the name itself counts as not yet defined, so
//     `#ifndef X / #define X` defaults and include guards hold);
//   - a name nobody in the project #defines, and that its code never uses as a value, is a
//     build option left off (ZLIB_DEBUG, Z_SOLO). One the code uses (O_CLOEXEC) comes from a
//     system header: unknown;
//   - the compiler's own names: __STDC__, __GNUC__, __linux__... are defined; other platforms'
//     and compilers' (_WIN32, _MSC_VER, __APPLE__...) are not; other reserved names (`_X`,
//     `__x`) are unknown. limits.h / stdint.h / stdio.h constants are defined (value unknown)
//     where that header is included.
// Anything else it can't decide is unknown, and an unknown branch is treated as compiled.
using c_pp::Tri;

class Config {
 public:
  explicit Config(const Project& p) : p_(p) {}

  // Names that no visible file defines and no flag mentions, taken as off without the build's say.
  const std::unordered_set<std::string>& assumed_off() const { return assumed_off_; }

  // Whether `cond` of module `mod` (an index in its conds) is compiled in translation unit `ctx`.
  Tri active(int ctx, int mod, int cond) {
    if (cond < 0) return Tri::True;
    const bool top = busy_.empty() && assumed_.empty();
    const auto key = std::make_tuple(ctx, mod, cond);
    if (top) {
      if (auto it = memo_active_.find(key); it != memo_active_.end()) return it->second;
      steps_ = 0;
    }
    Tri r = Tri::True;
    const c_pp::Conditions& pp = p_.mods[std::size_t(mod)].pp;
    for (int bi : pp.conds[std::size_t(cond)].branches) {
      const Tri t = branch(ctx, pp.branches[std::size_t(bi)]);
      if (t == Tri::False) {
        r = Tri::False;
        break;
      }
      if (t == Tri::Unknown) r = Tri::Unknown;
    }
    if (top) memo_active_[key] = r;
    return r;
  }

 private:
  const Project& p_;
  std::unordered_set<std::string> assumed_off_;
  std::vector<std::string> busy_;     // names whose #defines are being decided
  std::vector<std::string> assumed_;  // names that count as undefined for now
  std::map<std::tuple<int, int, int>, Tri> memo_active_;
  std::map<std::pair<int, std::string>, Tri> memo_defined_;
  std::map<std::pair<int, const c_pp::Branch*>, Tri> memo_branch_;
  int steps_ = 0;
  static constexpr int kMaxSteps = 20000;  // per top-level question; past it, unknown

  static bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
  }

  c_pp::Env env(int ctx) {
    return {[this, ctx](const std::string& n) { return defined(ctx, n); },
            [this, ctx](const std::string& n) { return value(ctx, n); }};
  }

  Tri branch(int ctx, const c_pp::Branch& b) {
    // At the top level (nothing assumed, nothing being decided) a branch's value is the same every
    // time: the outer #if of many inner ones is asked for once.
    const bool top = busy_.empty() && assumed_.empty();
    if (top) {
      if (auto it = memo_branch_.find({ctx, &b}); it != memo_branch_.end()) return it->second;
    }
    const Tri r = branch_value(ctx, b);
    if (top && steps_ <= kMaxSteps) memo_branch_[{ctx, &b}] = r;
    return r;
  }

  Tri branch_value(int ctx, const c_pp::Branch& b) {
    const std::size_t mark = assumed_.size();
    assumed_.insert(assumed_.end(), b.self.begin(), b.self.end());
    Tri r = Tri::True;
    auto take = [&](Tri t) {
      if (t == Tri::False) r = Tri::False;
      else if (t == Tri::Unknown && r == Tri::True) r = Tri::Unknown;
    };
    for (const std::string& prior : b.prior) {
      const Tri t = c_pp::holds(prior, env(ctx));
      take(t == Tri::True ? Tri::False : t == Tri::False ? Tri::True : Tri::Unknown);
    }
    if (!b.expr.empty()) take(c_pp::holds(b.expr, env(ctx)));
    assumed_.resize(mark);
    return r;
  }

  // The compiler's and the platform's names: True, False, or nullopt if not one of them.
  static std::optional<Tri> host(const std::string& n) {
    static const std::unordered_set<std::string> kDefined = {
        "__STDC__", "__STDC_VERSION__", "__STDC_HOSTED__", "__GNUC__", "__linux__", "__linux", "__gnu_linux__",
        "__unix__", "__unix", "__ELF__", "__CHAR_BIT__", "__SIZEOF_INT__", "__SIZEOF_LONG__", "__SIZEOF_POINTER__"};
    static const std::unordered_set<std::string> kUndefined = {
        "__cplusplus", "_WIN32", "_WIN64", "__WIN32__", "_WIN32_WCE", "_WINDOWS", "__CYGWIN__", "__MINGW32__",
        "__MINGW64__", "_MSC_VER", "__BORLANDC__", "__TURBOC__", "__WATCOMC__", "__SC__", "__SASC", "__MSDOS__",
        "__DOS__", "__OS2__", "__EMX__", "__APPLE__", "__MACH__", "__MACTYPES__", "__OS400__", "__TOS_OS400__",
        "__HOS_AIX__", "_AIX", "__MVS__", "__370__", "__VMS", "__BEOS__", "_BEOS_", "__HAIKU__", "__FreeBSD__",
        "__NetBSD__", "__OpenBSD__", "__DragonFly__", "__sun", "__hpux", "__QNX__", "__SMALL__", "__MEDIUM__",
        "__COMPACT__", "__LARGE__", "__HUGE__", "__FLAT__", "__386__", "__acorn", "__TARGET_LIB__", "__EMSCRIPTEN__",
        "__STDC_NO_ATOMICS__", "__STDC_NO_THREADS__", "__STDC_NO_VLA__"};
    if (kDefined.count(n)) return Tri::True;
    if (kUndefined.count(n)) return Tri::False;
    static const std::unordered_set<std::string> kPlatform = {"unix", "linux", "i386", "sun", "sparc", "mips"};
    if ((n.size() >= 2 && n[0] == '_' && (n[1] == '_' || std::isupper(static_cast<unsigned char>(n[1])))) ||
        kPlatform.count(n)) {
      return Tri::Unknown;
    }
    return std::nullopt;
  }

  // Constants of the standard headers: defined wherever their header is included.
  static const char* std_header(const std::string& n) {
    static const std::unordered_map<std::string, const char*> k = {
        {"CHAR_BIT", "limits.h"},  {"SCHAR_MIN", "limits.h"}, {"SCHAR_MAX", "limits.h"}, {"UCHAR_MAX", "limits.h"},
        {"CHAR_MIN", "limits.h"},  {"CHAR_MAX", "limits.h"},  {"SHRT_MIN", "limits.h"},  {"SHRT_MAX", "limits.h"},
        {"USHRT_MAX", "limits.h"}, {"INT_MIN", "limits.h"},   {"INT_MAX", "limits.h"},   {"UINT_MAX", "limits.h"},
        {"LONG_MIN", "limits.h"},  {"LONG_MAX", "limits.h"},  {"ULONG_MAX", "limits.h"}, {"LLONG_MIN", "limits.h"},
        {"LLONG_MAX", "limits.h"}, {"ULLONG_MAX", "limits.h"}, {"MB_LEN_MAX", "limits.h"},
        {"SIZE_MAX", "stdint.h"},  {"INTPTR_MAX", "stdint.h"}, {"UINTPTR_MAX", "stdint.h"},
        {"PTRDIFF_MAX", "stdint.h"}, {"INT32_MAX", "stdint.h"}, {"UINT32_MAX", "stdint.h"},
        {"INT64_MAX", "stdint.h"}, {"UINT64_MAX", "stdint.h"}, {"EOF", "stdio.h"}, {"BUFSIZ", "stdio.h"},
        {"SEEK_SET", "stdio.h"},   {"SEEK_CUR", "stdio.h"},   {"SEEK_END", "stdio.h"}, {"EXIT_SUCCESS", "stdlib.h"},
        {"EXIT_FAILURE", "stdlib.h"}, {"RAND_MAX", "stdlib.h"}};
    auto it = k.find(n);
    return it == k.end() ? nullptr : it->second;
  }

  // -D / -U for this translation unit from compile_commands.json: nullptr if none.
  const std::optional<std::string>* flag(int ctx, const std::string& n) const {
    const auto& f = p_.mods[std::size_t(ctx)].flags;
    auto it = f.find(n);
    return it == f.end() ? nullptr : &it->second;
  }

  Tri defined(int ctx, const std::string& n) {
    if (const auto* f = flag(ctx, n)) return *f ? Tri::True : Tri::False;
    if (has(assumed_, n)) return Tri::False;
    if (has(busy_, n) || ++steps_ > kMaxSteps) return Tri::Unknown;
    const bool top = busy_.empty() && assumed_.empty();
    if (top) {
      if (auto it = memo_defined_.find({ctx, n}); it != memo_defined_.end()) return it->second;
    }
    const auto defs = p_.defs(ctx, n);
    Tri r = Tri::False;
    if (defs.empty()) {
      if (const auto known = host(n)) r = *known;
      else if (const char* h = std_header(n)) r = p_.sees_system_header(ctx, h) ? Tri::True : Tri::Unknown;
      else if (p_.defined_in_header(n) || p_.used_in_code(ctx, n)) r = Tri::Unknown;  // in a header we don't see, or a system one
      else if (!p_.mods[std::size_t(ctx)].in_build) assumed_off_.insert(n);  // no build information: a guess
    } else {
      busy_.push_back(n);
      assumed_.push_back(n);
      bool undefined_later = false;
      for (const auto& [mod, c] : p_.undefs(ctx, n)) undefined_later = undefined_later || active(ctx, mod, c) != Tri::False;
      if (undefined_later) {
        r = Tri::Unknown;  // the order of #define and #undef would matter
      } else {
        for (const auto& d : defs) {
          const Tri t = active(ctx, d.mod, d.def->cond);
          if (t == Tri::True) {
            r = Tri::True;
            break;
          }
          if (t == Tri::Unknown) r = Tri::Unknown;
        }
      }
      busy_.pop_back();
      assumed_.pop_back();
    }
    if (top && steps_ <= kMaxSteps) memo_defined_[{ctx, n}] = r;
    return r;
  }

  std::optional<long long> value(int ctx, const std::string& n) {
    if (n == "__STDC__" || n == "__STDC_HOSTED__") return 1;
    if (n == "__STDC_VERSION__") return 201710L;
    if (const auto* f = flag(ctx, n)) {
      if (!*f) return 0;
      if (has(busy_, n)) return std::nullopt;
      busy_.push_back(n);
      const auto v = c_pp::eval(**f, env(ctx));
      busy_.pop_back();
      return v;
    }
    const Tri d = defined(ctx, n);
    if (d == Tri::False) return 0;
    if (d == Tri::Unknown || has(busy_, n)) return std::nullopt;
    // Defined: by the one #define in a compiled branch, if its value is a constant expression.
    const MacroDef* only = nullptr;
    busy_.push_back(n);
    assumed_.push_back(n);
    bool clear = true;
    for (const auto& ref : p_.defs(ctx, n)) {
      const Tri t = active(ctx, ref.mod, ref.def->cond);
      if (t == Tri::False) continue;
      if (t == Tri::Unknown || only || ref.def->function_like) clear = false;
      only = ref.def;
    }
    assumed_.pop_back();
    std::optional<long long> v;
    if (clear && only) v = c_pp::eval(only->value, env(ctx));
    busy_.pop_back();
    return v;
  }
};

struct EdgeOut {
  std::string from, to;
  EdgeKind kind = EdgeKind::Calls;
  Confidence confidence = Confidence::Exact;
  std::uint32_t line = 0;
  nlohmann::json attrs = nlohmann::json::object();
  std::optional<Node> new_node;
};

constexpr std::size_t kMaxPointerTargets = 12;

class Linker {
 public:
  Linker(const Project& p, int m) : p_(p), m_(m), mod_(p.mods[std::size_t(m)]) {}

  const std::unordered_set<std::string>& assumed_off() const { return cfg_.assumed_off(); }

  std::vector<EdgeOut> run() {
    if (mod_.external) return {};
    for (int f = 0; f < static_cast<int>(mod_.funcs.size()); ++f) {
      emit(p_.id(m_), p_.id(FnRef{m_, f}), EdgeKind::Defines, Confidence::Exact, mod_.funcs[std::size_t(f)].line);
    }
    for (const Include& inc : mod_.includes) on_include(inc);
    for (const CallSite& cs : mod_.calls) on_call(cs);
    for (int f = 0; f < static_cast<int>(mod_.funcs.size()); ++f) curl_edges(f);
    return most_certain_calls(std::move(out_));
  }

  // One call edge per caller and callee, from its most certain call site: exact before inferred,
  // compiled before not, outside any #if before inside. Otherwise the graph's merge would mix
  // their attrs (an unconditional call shown with another call site's condition).
  static std::vector<EdgeOut> most_certain_calls(std::vector<EdgeOut> in) {
    auto rank = [](const EdgeOut& e) {
      return std::make_tuple(e.confidence == Confidence::Exact ? 0 : 1, e.attrs.contains("inactive") ? 1 : 0,
                             e.attrs.contains("condition") ? 1 : 0);
    };
    std::vector<EdgeOut> out;
    std::unordered_map<std::string, std::size_t> at;
    for (EdgeOut& e : in) {
      if (e.kind != EdgeKind::Calls) {
        out.push_back(std::move(e));
        continue;
      }
      auto [it, fresh] = at.emplace(e.from + '\n' + e.to, out.size());
      if (fresh) out.push_back(std::move(e));
      else if (rank(e) < rank(out[it->second])) out[it->second] = std::move(e);
    }
    return out;
  }

 private:
  const Project& p_;
  int m_;
  const ModuleInfo& mod_;
  std::vector<EdgeOut> out_;
  Config cfg_{p_};

  void emit(std::string from, std::string to, EdgeKind kind, Confidence c, std::uint32_t line,
            nlohmann::json attrs = nlohmann::json::object(), std::optional<Node> node = std::nullopt) {
    out_.push_back({std::move(from), std::move(to), kind, c, line, std::move(attrs), std::move(node)});
  }

  std::string caller(const CallSite& cs) const { return p_.id(FnRef{m_, cs.func}); }

  void on_include(const Include& inc) {
    if (const int h = p_.resolve_include(m_, inc); h >= 0) {
      if (h != m_ && !p_.mods[std::size_t(h)].external) emit(p_.id(m_), p_.id(h), EdgeKind::Imports, Confidence::Exact, inc.line);
      return;
    }
    if (is_system_header(inc.spec)) return;
    Node n;
    n.id = "c:<" + inc.spec + ">";
    n.kind = NodeKind::Module;
    n.name = inc.spec;
    n.frontend = "c";
    n.attrs = {{"third_party", true}};
    std::string to = n.id;
    emit(p_.id(m_), std::move(to), EdgeKind::Imports, Confidence::Exact, inc.line, nlohmann::json::object(), std::move(n));
  }

  nlohmann::json attrs_for(const CallSite& cs, const char* via = nullptr) const {
    nlohmann::json a = nlohmann::json::object();
    if (via) a["via"] = via;
    if (!cs.condition.empty()) a["condition"] = cs.condition;
    return a;
  }

  // Why a call may not happen in the default configuration: an #if that isn't compiled around the
  // call, around the macro it came through, or around the called function's definition.
  struct Off {
    bool inactive = false;
    std::string condition;  // the conditions beyond the call site's own
  };

  void exclude(Off& off, const std::string& cond) const {
    off.inactive = true;
    if (!cond.empty() && off.condition.find(cond) == std::string::npos) {
      off.condition += (off.condition.empty() ? "" : " && ") + cond;
    }
  }
  void exclude(Off& off, int mod, int cond) const { exclude(off, p_.mods[std::size_t(mod)].pp.conds[std::size_t(cond)].text); }

  // A function of module `mod` is renamed `name` by an object-like #define in a compiled branch.
  bool renamed_to(int mod, int ctx, const std::string& name) {
    const auto& by_value = p_.mods[std::size_t(mod)].renames;
    const auto it = by_value.find(name);
    if (it == by_value.end()) return false;
    for (const auto& [fn_cond, def_cond] : it->second) {
      if (cfg_.active(ctx, mod, fn_cond) != Tri::False && cfg_.active(ctx, mod, def_cond) == Tri::True) return true;
    }
    return false;
  }

  // Returns false if the function's definition isn't compiled in the default configuration.
  bool emit_call(const CallSite& cs, FnRef r, bool via_macro, Off off) {
    const Func& f = p_.fn(r);
    // A header's definition is compiled in the caller's translation unit, a .c file's in its own.
    const int ctx = p_.mods[std::size_t(r.mod)].path.ends_with(".h") ? m_ : r.mod;
    // `#if X / f() {...} / #else / f() {...}`: the same function twice, compiled if either is.
    bool compiled = false;
    for (const Func& other : p_.mods[std::size_t(r.mod)].funcs) {
      if (!compiled && other.name == f.name) compiled = cfg_.active(ctx, r.mod, other.cond) != Tri::False;
    }
    // `#if !defined(X) / #define f_c f / #endif` with `int f_c(...) {...}` below: in this configuration
    // the compiled function f_c is named f, whatever a `#if X` branch defines as f.
    if (!compiled) compiled = renamed_to(r.mod, ctx, f.name);
    if (!compiled) exclude(off, r.mod, f.cond);
    const char* via = via_macro ? (r.declared ? "macro" : "macro_name_only") : (r.declared ? nullptr : "name_only");
    nlohmann::json a = attrs_for(cs, via);
    if (off.inactive) {
      std::string c = cs.condition;
      if (!off.condition.empty() && c.find(off.condition) == std::string::npos) {
        c += (c.empty() ? "" : " && ") + off.condition;
      }
      if (!c.empty()) a["condition"] = c;
      a["inactive"] = true;
    }
    emit(caller(cs), p_.id(r), EdgeKind::Calls, r.declared && !off.inactive ? Confidence::Exact : Confidence::Inferred,
         cs.line, std::move(a));
    return compiled;
  }

  // `#define NAME other_name` in a compiled branch: the name the compiler sees instead.
  std::string renamed(const std::string& name) {
    for (const auto& d : p_.defs(m_, name)) {
      if (d.def->function_like) continue;
      std::string v;
      for (char c : d.def->value) {
        if (c == '/') break;  // a comment after the name
        if (!std::isspace(static_cast<unsigned char>(c))) v += c;
      }
      const bool ident = !v.empty() && !std::isdigit(static_cast<unsigned char>(v[0])) &&
                         std::all_of(v.begin(), v.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
      if (ident && v != name && cfg_.active(m_, d.mod, d.def->cond) == Tri::True) return v;
    }
    return {};
  }

  // A call by name: to the project function, through object-like macros that rename it, and
  // through function-like macros (FLUSH_BLOCK(s, 0) -> flush_pending(...)).
  void call_name(const CallSite& cs, const std::string& name, bool via_macro, const Off& off, int depth,
                 std::vector<std::string>& stack) {
    if (depth > 4 || std::find(stack.begin(), stack.end(), name) != stack.end()) return;
    stack.push_back(name);
    const FnRef r = p_.resolve_call(m_, name);
    if (const std::string to = renamed(name); !to.empty()) {
      if (r.mod >= 0) {  // only reached when the #define isn't there
        Off o = off;
        exclude(o, "!defined(" + name + ")");
        emit_call(cs, r, via_macro, o);
      }
      call_name(cs, to, via_macro, off, depth + 1, stack);
    } else if (r.mod < 0 || !emit_call(cs, r, via_macro, off)) {
      // No function, or one that isn't compiled: a function-like macro of that name is what's called.
      for (const auto& d : p_.defs(m_, name)) {
        if (!d.def->function_like) continue;
        Off o = off;
        if (cfg_.active(m_, d.mod, d.def->cond) == Tri::False) exclude(o, d.mod, d.def->cond);
        for (const std::string& callee : d.def->calls) call_name(cs, callee, true, o, depth + 1, stack);
      }
    }
    stack.pop_back();
  }

  void on_call(const CallSite& cs) {
    if (cs.callee.empty()) {  // through a pointer: functions whose address is taken, by arity
      std::vector<FnRef> targets;
      for (const FnRef& r : p_.address_taken()) {
        const int np = p_.fn(r).nparams;
        if (np < 0 || static_cast<std::size_t>(np) == cs.nargs) targets.push_back(r);
      }
      if (targets.empty() || targets.size() > kMaxPointerTargets) return;
      for (const FnRef& r : targets) {
        emit(caller(cs), p_.id(r), EdgeKind::Calls, Confidence::Inferred, cs.line, attrs_for(cs, "function_pointer"));
      }
      return;
    }
    if (try_sql(cs)) return;
    Off off;
    if (cfg_.active(m_, m_, cs.cond) == Tri::False) off.inactive = true;
    std::vector<std::string> stack;
    call_name(cs, cs.callee, false, off, 0, stack);
  }

  bool try_sql(const CallSite& cs) {
    static const std::unordered_map<std::string, std::size_t> kSql = {
        {"sqlite3_exec", 1}, {"sqlite3_prepare", 1}, {"sqlite3_prepare_v2", 1}, {"sqlite3_prepare_v3", 1},
        {"PQexec", 1},       {"PQexecParams", 1},    {"PQprepare", 2},          {"PQsendQuery", 1},
        {"mysql_query", 1},  {"mysql_real_query", 1}};
    auto it = kSql.find(cs.callee);
    if (it == kSql.end() || cs.args.size() <= it->second || !cs.args[it->second].str) return false;
    const auto sql = p_.eval(m_, cs.func, *cs.args[it->second].str);
    if (!sql) return true;
    const SqlTables t = sql_tables(*sql);
    auto store = [&](const std::string& table, EdgeKind kind) {
      std::string store_id = sql_store_id(table);
      std::string name = store_id.substr(4);  // without "sql:"
      Node n = sql_store_node(std::move(store_id), std::move(name), "c");
      std::string to = n.id;
      emit(caller(cs), std::move(to), kind, Confidence::Exact, cs.line, nlohmann::json::object(), std::move(n));
    };
    for (const auto& r : t.reads) store(r, EdgeKind::Reads);
    for (const auto& w : t.writes) store(w, EdgeKind::Writes);
    return true;
  }

  // libcurl: the URLs a function sets (CURLOPT_URL), with the method its other options imply.
  void curl_edges(int f) {
    std::vector<std::pair<std::string, std::uint32_t>> urls;
    std::string method = "GET";
    for (const CallSite& cs : mod_.calls) {
      if (cs.func != f || cs.callee != "curl_easy_setopt" || cs.args.size() < 3) continue;
      const std::string& opt = cs.args[1].name;
      if (opt == "CURLOPT_URL" && cs.args[2].str) {
        if (auto u = p_.eval(m_, f, *cs.args[2].str)) urls.emplace_back(*u, cs.line);
      } else if (opt == "CURLOPT_POST" || opt == "CURLOPT_POSTFIELDS" || opt == "CURLOPT_COPYPOSTFIELDS" ||
                 opt == "CURLOPT_MIMEPOST") {
        if (method == "GET") method = "POST";
      } else if (opt == "CURLOPT_UPLOAD" || opt == "CURLOPT_PUT") {
        method = "PUT";
      } else if (opt == "CURLOPT_NOBODY") {
        method = "HEAD";
      } else if (opt == "CURLOPT_CUSTOMREQUEST" && cs.args[2].str) {
        if (auto m = p_.eval(m_, f, *cs.args[2].str); m && m->find("{}") == std::string::npos) {
          method = to_upper(*m);
        }
      }
    }
    for (const auto& [url, line] : urls) {
      const std::string node_id = http_node_id(url);
      if (node_id.empty()) continue;
      Node n = http_external_node(node_id, "c");
      const Confidence c = n.confidence;
      const bool reads = method == "GET" || method == "HEAD" || method == "OPTIONS";
      emit(p_.id(FnRef{m_, f}), node_id, reads ? EdgeKind::Reads : EdgeKind::SendsTo, c, line, {{"method", method}},
           std::move(n));
    }
  }
};

}  // namespace

namespace {

using Flags = std::unordered_map<std::string, std::optional<std::string>>;

// A shell command line split into words: quotes and backslashes as sh reads them.
std::vector<std::string> split_command(const std::string& cmd) {
  std::vector<std::string> out;
  std::string word;
  bool in_word = false;
  char quote = 0;
  for (std::size_t i = 0; i < cmd.size(); ++i) {
    const char c = cmd[i];
    if (quote) {
      if (c == quote) quote = 0;
      else if (c == '\\' && quote == '"' && i + 1 < cmd.size()) word += cmd[++i];
      else word += c;
    } else if (c == '"' || c == '\'') {
      quote = c;
      in_word = true;
    } else if (c == '\\' && i + 1 < cmd.size()) {
      word += cmd[++i];
      in_word = true;
    } else if (std::isspace(static_cast<unsigned char>(c))) {
      if (in_word) out.push_back(std::move(word));
      word.clear();
      in_word = false;
    } else {
      word += c;
      in_word = true;
    }
  }
  if (in_word) out.push_back(std::move(word));
  return out;
}

// What a compilation database says about one file.
struct BuildEntry {
  Flags flags;                              // -D / -U
  std::vector<ModuleInfo::IncDir> dirs;     // -iquote, -I, -isystem, -idirafter, in search order
  std::vector<std::string> forced;          // -include
};

struct Build {
  bool found = false;
  std::unordered_map<std::string, BuildEntry> files;  // by path relative to the root
  std::vector<ModuleInfo::IncDir> all_dirs;           // every directory, first seen first
};

// The compilation database: `db` if given ("none": ignore any), else compile_commands.json at the root
// or in build/. Per file: its -D / -U flags, include directories (relative ones against the entry's
// "directory"; they may lie outside the root, where a build puts generated headers) and forced
// headers. Files outside the root are left out.
Build read_build(const fs::path& root, const std::string& db_arg) {
  Build out;
  if (db_arg == "none") return out;
  std::vector<fs::path> candidates;
  if (!db_arg.empty()) candidates = {fs::path(db_arg)};
  else candidates = {root / "compile_commands.json", root / "build/compile_commands.json"};
  for (const fs::path& db : candidates) {
    std::error_code ec;
    if (!fs::is_regular_file(db, ec)) continue;
    const nlohmann::json j = nlohmann::json::parse(read_file(db), nullptr, false);
    if (!j.is_array()) return out;
    out.found = true;
    const fs::path base = fs::weakly_canonical(root, ec);
    auto str = [](const nlohmann::json& e, const char* key) {
      auto it = e.find(key);
      return it != e.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    std::unordered_set<std::string> seen_dirs;
    for (const nlohmann::json& e : j) {
      if (!e.is_object()) continue;
      fs::path file = str(e, "file");
      if (file.empty()) continue;
      fs::path directory = str(e, "directory");
      if (directory.is_relative()) directory = db.parent_path() / directory;
      if (file.is_relative()) file = directory / file;
      const fs::path rel = fs::weakly_canonical(file, ec).lexically_relative(base);
      if (ec || rel.empty() || *rel.begin() == "..") continue;
      std::vector<std::string> args;
      if (auto it = e.find("arguments"); it != e.end() && it->is_array()) {
        for (const auto& a : *it) {
          if (a.is_string()) args.push_back(a.get<std::string>());
        }
      } else {
        args = split_command(str(e, "command"));
      }
      BuildEntry& entry = out.files[rel.generic_string()];
      std::vector<std::string> quote, plain, system, after;
      auto absolute = [&](const std::string& d) {
        fs::path p = d;
        if (p.is_relative()) p = directory / p;
        std::error_code ec2;
        return fs::weakly_canonical(p, ec2).generic_string();
      };
      for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        // `-Xvalue` or `-X value`
        auto value = [&](const char* flag) -> std::optional<std::string> {
          const std::size_t n = std::strlen(flag);
          if (a.compare(0, n, flag) != 0) return std::nullopt;
          if (a.size() > n) return a.substr(n);
          if (i + 1 < args.size()) return args[++i];
          return std::nullopt;
        };
        if (a.starts_with("-iquote")) {
          if (auto v = value("-iquote")) quote.push_back(absolute(*v));
        } else if (a.starts_with("-isystem")) {
          if (auto v = value("-isystem")) system.push_back(absolute(*v));
        } else if (a.starts_with("-idirafter")) {
          if (auto v = value("-idirafter")) after.push_back(absolute(*v));
        } else if (a == "-include" || a == "--include") {
          if (i + 1 < args.size()) entry.forced.push_back(absolute(args[++i]));
        } else if (a.starts_with("-I")) {
          if (auto v = value("-I")) plain.push_back(absolute(*v));
        } else if (a.starts_with("-D") || a.starts_with("-U")) {
          const bool undef = a[1] == 'U';
          std::string v = a.substr(2);
          if (v.empty() && i + 1 < args.size()) v = args[++i];
          const std::size_t eq = v.find('=');
          const std::string macro = v.substr(0, eq);
          if (macro.empty()) continue;
          if (undef) entry.flags[macro] = std::nullopt;
          else entry.flags[macro] = eq == std::string::npos ? "1" : v.substr(eq + 1);
        }
      }
      entry.dirs.clear();
      for (const auto& d : quote) entry.dirs.push_back({d, true});
      for (const auto* list : {&plain, &system, &after}) {
        for (const auto& d : *list) entry.dirs.push_back({d, false});
      }
      for (const auto& d : entry.dirs) {
        if (seen_dirs.insert(d.path + (d.quote_only ? "?q" : "")).second) out.all_dirs.push_back(d);
      }
    }
    return out;
  }
  return out;
}

// Reads and summarises one file.
void parse_module(ModuleInfo& m, TSParser* parser, const fs::path& file) {
  bool readable = true;
  std::string src = read_file(file, &readable);
  blank_directive_comments(src);
  m.unreadable = !readable;
  m.lines = static_cast<std::size_t>(std::count(src.begin(), src.end(), '\n'));
  ts::TreePtr tree(ts_parser_parse_string(parser, nullptr, src.data(), static_cast<uint32_t>(src.size())));
  const TSNode rootn = ts_tree_root_node(tree.get());
  m.syntax_error = ts_node_has_error(rootn);
  Summarizer(m, src).run(rootn);
  {  // test files count separately (attrs.referenced_by_tests)
    static const std::unordered_set<std::string_view> names = {"identifier"}, strings = {"string_content"},
                                                       text = {"preproc_arg"};
    static const frontends::DefFields defs = {{"function_declarator", "declarator"}};
    m.test_file = is_test_code(m.path);
    frontends::count_name_uses(rootn, src, names, strings, defs, m.names, text);
  }
}

}  // namespace

Graph extract_c_files(const fs::path& root, const std::vector<fs::path>& rel_files, CStats* stats, unsigned threads,
                       const std::string& compile_commands) {
  std::vector<fs::path> files;
  for (const auto& f : rel_files) {
    if (f.extension() == ".c" || f.extension() == ".h") files.push_back(f);
  }
  std::sort(files.begin(), files.end());

  Project project;
  project.mods.resize(files.size());
  parallel_for(files.size(), threads, [&](std::size_t i) {
    thread_local ts::ParserPtr parser = [] {
      ts::ParserPtr p(ts_parser_new());
      ts_parser_set_language(p.get(), tree_sitter_c());
      return p;
    }();
    ModuleInfo& m = project.mods[i];
    m.path = files[i].generic_string();
    parse_module(m, parser.get(), root / files[i]);
  });
  // The build: its flags, include directories and file list; headers it generates outside the root.
  const Build build = read_build(root, compile_commands);
  std::error_code root_ec;
  project.root_abs_ = fs::weakly_canonical(root, root_ec).generic_string();
  project.has_db = build.found;
  project.global_dirs_ = build.all_dirs;
  for (ModuleInfo& m : project.mods) {
    if (auto it = build.files.find(m.path); it != build.files.end()) {
      m.flags = it->second.flags;
      m.inc_dirs = it->second.dirs;
      m.in_build = true;
      for (const std::string& f : it->second.forced) m.includes.insert(m.includes.begin(), Include{f, false, 0});
    }
  }
  project.index_paths();
  if (build.found) {
    ts::ParserPtr parser(ts_parser_new());
    ts_parser_set_language(parser.get(), tree_sitter_c());
    for (std::size_t i = 0; i < project.mods.size(); ++i) {  // the list grows as headers are found
      const std::vector<Include> includes = project.mods[i].includes;
      const std::vector<ModuleInfo::IncDir> dirs =
          project.mods[i].inc_dirs.empty() ? project.global_dirs_ : project.mods[i].inc_dirs;
      for (const Include& inc : includes) {
        if (project.resolve_include(static_cast<int>(i), inc) >= 0) continue;
        std::vector<fs::path> cands;
        if (fs::path(inc.spec).is_absolute()) cands.push_back(inc.spec);
        else {
          if (!inc.system) cands.push_back(fs::path(project.mods[i].path).parent_path() / inc.spec);
          for (const ModuleInfo::IncDir& d : dirs) {
            if (!(inc.system && d.quote_only)) cands.push_back(fs::path(d.path) / inc.spec);
          }
        }
        for (const fs::path& c : cands) {
          const std::string key = c.lexically_normal().generic_string();
          std::error_code ec;
          if (!c.is_absolute() || !fs::is_regular_file(c, ec)) continue;
          if (project.lookup(c) >= 0) break;  // inside the root: already a module
          ModuleInfo ext;
          ext.path = key;
          ext.external = true;
          ext.inc_dirs = project.mods[i].inc_dirs;
          parse_module(ext, parser.get(), c);
          ext.names = {};
          ext.funcs.clear();
          ext.calls.clear();
          project.add_module(std::move(ext));
          break;
        }
      }
    }
  }
  frontends::NameUses uses, test_uses;
  {
    // Included files the frontend doesn't parse (generated .inc tables, .def lists): their names
    // count as uses, as text.
    std::unordered_set<std::string> read;
    for (const auto& m : project.mods) {
      for (const Include& inc : m.includes) {
        if (inc.system || inc.spec.ends_with(".h") || inc.spec.ends_with(".c")) continue;
        for (const fs::path& cand : {root / fs::path(m.path).parent_path() / inc.spec, root / inc.spec}) {
          const std::string key = cand.lexically_normal().string();
          std::error_code ec;
          if (!fs::is_regular_file(cand, ec)) continue;
          if (read.insert(key).second) frontends::count_words(read_file(cand), m.test_file ? test_uses : uses);
          break;
        }
      }
    }
  }
  for (auto& m : project.mods) {
    for (const auto& [name, n] : m.names) (m.test_file ? test_uses : uses)[name] += n;
    m.names = {};
  }
  {
    std::vector<std::string> names;
    for (const auto& m : project.mods) for (const auto& f : m.funcs) names.push_back(f.name);
    frontends::finalize_name_uses(uses, names);
    frontends::finalize_name_uses(test_uses, std::move(names));
  }
  project.index();

  std::vector<std::vector<EdgeOut>> edges(project.mods.size());
  std::vector<std::size_t> assumed_names(project.mods.size(), 0), assumed_edges(project.mods.size(), 0);
  std::vector<std::vector<std::string>> assumed_list(project.mods.size());
  parallel_for(project.mods.size(), threads, [&](std::size_t i) {
    Linker linker(project, static_cast<int>(i));
    edges[i] = linker.run();
    const auto& names = linker.assumed_off();
    if (names.empty()) return;
    assumed_names[i] = names.size();
    assumed_list[i].assign(names.begin(), names.end());
    // Conditional edges whose condition names one of them.
    for (const EdgeOut& e : edges[i]) {
      if (e.kind != EdgeKind::Calls || !e.attrs.value("inactive", false)) continue;
      const std::string cond = e.attrs.value("condition", "");
      bool hit = false;
      for (std::size_t k = 0; k < cond.size() && !hit;) {
        if (std::isalpha(static_cast<unsigned char>(cond[k])) || cond[k] == '_') {
          std::size_t j = k;
          while (j < cond.size() && (std::isalnum(static_cast<unsigned char>(cond[j])) || cond[j] == '_')) ++j;
          hit = names.count(cond.substr(k, j - k)) > 0;
          k = j;
        } else {
          ++k;
        }
      }
      if (hit) ++assumed_edges[i];
    }
  });

  Graph g;
  g.source = root.string();
  for (int mi = 0; mi < static_cast<int>(project.mods.size()); ++mi) {
    const ModuleInfo& m = project.mods[std::size_t(mi)];
    if (m.external) continue;
    Node mod;
    mod.id = project.id(mi);
    mod.kind = NodeKind::Module;
    mod.name = fs::path(m.path).filename().string();
    mod.loc = SourceLoc{m.path, 1, static_cast<std::uint32_t>(m.lines), std::nullopt};
    mod.frontend = "c";
    if (assumed_names[std::size_t(mi)]) mod.attrs["assumed_off_names"] = assumed_names[std::size_t(mi)];
    g.add_node(std::move(mod));
    for (int f = 0; f < static_cast<int>(m.funcs.size()); ++f) {
      const Func& fn = m.funcs[std::size_t(f)];
      Node n;
      n.id = project.id(FnRef{mi, f});
      n.kind = NodeKind::Function;
      n.name = fn.name;
      n.loc = SourceLoc{m.path, fn.line, fn.end_line, std::nullopt};
      n.frontend = "c";
      if (fn.is_static) n.attrs = {{"static", true}};
      // Local (docs/frontends.md): static in a .c file. A static (inline) function in a header is
      // there for every file that includes it, possibly outside the project.
      if (fn.is_static && !m.path.ends_with(".h")) n.attrs["local"] = true;
      if (frontends::name_used(uses, fn.name)) n.attrs["referenced"] = true;
      else if (frontends::name_used(test_uses, fn.name)) n.attrs["referenced_by_tests"] = true;
      g.add_node(std::move(n));
    }
  }
  for (std::size_t mi = 0; mi < project.mods.size(); ++mi) {
    for (EdgeOut& eo : edges[mi]) {
      if (eo.new_node) g.add_node(std::move(*eo.new_node));
      const auto from = g.find(eo.from), to = g.find(eo.to);
      if (!from || !to) continue;
      Edge e;
      e.from = *from;
      e.to = *to;
      e.kind = eo.kind;
      e.confidence = eo.confidence;
      e.frontend = "c";
      if (eo.line) e.loc = SourceLoc{project.mods[mi].path, eo.line, 0, std::nullopt};
      e.attrs = std::move(eo.attrs);
      g.add_edge(std::move(e));
    }
  }
  if (stats) {
    stats->files = 0;
    stats->build_database = build.found;
    stats->build_files = build.files.size();
    std::unordered_set<std::string> all_names;
    for (std::size_t i = 0; i < project.mods.size(); ++i) {
      all_names.insert(assumed_list[i].begin(), assumed_list[i].end());
      stats->assumed_off_edges += assumed_edges[i];
      if (project.mods[i].external) ++stats->external_headers;
      else ++stats->files;
    }
    stats->assumed_off_names = all_names.size();
    for (const auto& m : project.mods) {
      if (m.external) continue;
      stats->lines += m.lines;
      stats->files_with_syntax_errors += m.syntax_error ? 1 : 0;
      if (m.unreadable) stats->unreadable.push_back(m.path);
    }
  }
  return g;
}

}  // namespace optum
