// C frontend. Rules: docs/frontends.md ("C").
//
// Pass 1 (parallel, per file): functions with their parameters and locals, includes, string
// macros and constants, call sites, and every function name used other than as a callee (its
// address is taken, so it may be called through a pointer). Pass 2 (parallel, per file): resolve
// names the way the linker would, and turn libcurl / database calls into external and store nodes.

#include "optum/frontends/c.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "../../ts_util.hpp"
#include "../common.hpp"
#include "optum/http.hpp"
#include "optum/sql.hpp"

extern "C" const TSLanguage* tree_sitter_c();

namespace optum {
namespace {

namespace fs = std::filesystem;
using frontends::parallel_for;
using frontends::count_lines;
using frontends::http_external_node;
using frontends::read_file;
using frontends::sql_store_node;
using frontends::to_upper;

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
  std::string callee;  // direct call by name ("" for a call through a pointer)
  std::size_t nargs = 0;
  std::vector<Arg> args;
  std::uint32_t line = 0;
  std::string condition;  // inside #if / #ifdef blocks: their conditions, joined with " && "
};

struct Func {
  std::string name;
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
  std::vector<Func> funcs;
  std::vector<Include> includes;
  std::vector<CallSite> calls;
  std::unordered_map<std::string, Str> consts;  // file-scope string constants and string macros
  std::unordered_set<std::string> address_taken;
  std::unordered_set<std::string> prototypes;  // functions declared (not defined) here
  // Function-like macros: the names called in their bodies (FLUSH_BLOCK(s, x) -> flush_pending).
  std::unordered_map<std::string, std::vector<std::string>> macros;
};

// ===================================================================== pass 1

class Summarizer {
 public:
  Summarizer(ModuleInfo& m, std::string_view src) : m_(m), src_(src) {}
  void run(TSNode root) { walk(root, -1); }

 private:
  ModuleInfo& m_;
  std::string_view src_;
  std::string text(TSNode n) const { return std::string(ts::text(n, src_)); }
  std::vector<std::string> conditions_;  // enclosing #if / #ifdef conditions

  // The identifier a declarator declares, through pointers, arrays, parentheses and functions.
  TSNode declared(TSNode d) const {
    while (!ts_node_is_null(d) && !ts::is(d, "identifier")) {
      if (ts::is(d, "parenthesized_declarator")) d = ts_node_named_child(d, 0);
      else {
        const TSNode inner = ts::field(d, "declarator");
        if (ts_node_is_null(inner)) return inner;
        d = inner;
      }
    }
    return d;
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

  void walk(TSNode n, int f) {
    const std::string_view t = ts_node_type(n);
    if (t == "function_definition") return on_function(n);
    if (t == "declaration") return on_declaration(n, f);
    if (t == "call_expression") return on_call(n, f);
    if (t == "preproc_include") return on_include(n);
    if (t == "preproc_def") return on_define(n);
    if (t == "preproc_function_def") return on_macro(n);
    if (t == "preproc_ifdef" || t == "preproc_if" || t == "preproc_elif" || t == "preproc_else" ||
        t == "preproc_elifdef") {
      return on_conditional(n, f);
    }
    if (t == "identifier") {
      m_.address_taken.insert(text(n));  // a name used as a value (filtered to functions later)
      return;
    }
    if (t == "parameter_list" || t == "type_definition" ||
        t == "struct_specifier" || t == "union_specifier" || t == "enum_specifier") {
      return;  // declarations of types and macros: no values in them
    }
    ts::for_named(n, [&](TSNode c) { walk(c, f); });
  }

  void on_function(TSNode n) {
    const TSNode decl = ts::field(n, "declarator");
    const TSNode fd = function_declarator(decl);
    const TSNode id = declared(decl);
    if (ts_node_is_null(id)) return;
    static const std::unordered_set<std::string> kKeywords = {
        "if", "else", "while", "for", "do", "switch", "case", "return", "sizeof", "goto", "break", "continue",
        "default", "typedef", "struct", "union", "enum", "static", "extern", "const", "volatile"};
    if (kKeywords.count(text(id))) return;  // error recovery on macro-heavy code, not a function
    Func fn;
    fn.name = text(id);
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
    for (const auto& c : conditions_) cs.condition += (cs.condition.empty() ? "" : " && ") + c;
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

  // #ifdef X / #ifndef X / #if expr: the body runs only under that condition. The #else / #elif
  // that follows is a sibling inside it, and gets "not (...)" of the condition before it.
  void on_conditional(TSNode n, int f) {
    std::string cond;
    const std::string_view t = ts_node_type(n);
    if (t == "preproc_ifdef" || t == "preproc_elifdef") {
      const std::string head = std::string(ts::text(ts_node_child(n, 0), src_));
      cond = (head.find("ndef") != std::string::npos ? "!" : "") + text(ts::field(n, "name"));
    } else if (t == "preproc_if" || t == "preproc_elif") {
      cond = text(ts::field(n, "condition"));
    } else {
      cond = "else";
    }
    std::erase_if(cond, [](char c) { return c == '\n' || c == '\r'; });
    if (cond.size() > 80) cond = cond.substr(0, 77) + "...";
    const bool branch = t == "preproc_elif" || t == "preproc_else" || t == "preproc_elifdef";
    if (branch && !conditions_.empty()) conditions_.back() = "not (" + conditions_.back() + ")";
    if (!branch) conditions_.push_back(cond);
    else if (t != "preproc_else") conditions_.back() += " && " + cond;
    ts::for_named(n, [&](TSNode c) {
      const std::string_view ct = ts_node_type(c);
      if (c.id == ts::field(n, "condition").id || c.id == ts::field(n, "name").id) return;
      if (ct == "preproc_elif" || ct == "preproc_else" || ct == "preproc_elifdef") on_conditional(c, f);
      else walk(c, f);
    });
    if (!branch) conditions_.pop_back();
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
    m_.macros[text(ts::field(n, "name"))] = std::move(called);
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

  void index() {
    for (int i = 0; i < static_cast<int>(mods.size()); ++i) by_path_.emplace(mods[std::size_t(i)].path, i);
    for (int m = 0; m < static_cast<int>(mods.size()); ++m) {
      for (int f = 0; f < static_cast<int>(mods[std::size_t(m)].funcs.size()); ++f) {
        const Func& fn = mods[std::size_t(m)].funcs[std::size_t(f)];
        if (!fn.is_static) globals_[fn.name].push_back({m, f});
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

  int resolve_include(int from, const Include& inc) const {
    const fs::path dir = fs::path(mods[std::size_t(from)].path).parent_path();
    if (!inc.system) {
      if (auto it = by_path_.find((dir / inc.spec).lexically_normal().generic_string()); it != by_path_.end()) return it->second;
    }
    if (auto it = by_path_.find(fs::path(inc.spec).lexically_normal().generic_string()); it != by_path_.end()) return it->second;
    // Found through an include directory we don't know: a unique project file ending in it.
    int hit = -1;
    for (const auto& [path, i] : by_path_) {
      if (path.size() > inc.spec.size() && path.ends_with("/" + inc.spec)) {
        if (hit >= 0) return -1;
        hit = i;
      }
    }
    return hit;
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
    // A definition elsewhere counts as found only if this file can see a prototype for it;
    // otherwise it may belong to another program in the same repo (examples/, contrib/).
    bool proto = mods[std::size_t(m)].prototypes.count(name) > 0;
    for (int h : visible_[std::size_t(m)]) proto = proto || mods[std::size_t(h)].prototypes.count(name) > 0;
    auto with = [&](FnRef r) {
      r.declared = proto;
      return r;
    };
    if (it->second.size() == 1) return with(it->second[0]);
    const fs::path dir = fs::path(mods[std::size_t(m)].path).parent_path();
    FnRef same_dir;
    int count = 0;
    for (const FnRef& r : it->second) {
      if (fs::path(mods[std::size_t(r.mod)].path).parent_path() == dir) {
        same_dir = r;
        ++count;
      }
    }
    return count == 1 ? with(same_dir) : FnRef{};
  }

  // A function-like macro visible from module m (its own file, then included headers).
  const std::vector<std::string>* macro(int m, const std::string& name) const {
    if (auto it = mods[std::size_t(m)].macros.find(name); it != mods[std::size_t(m)].macros.end()) return &it->second;
    for (int h : visible_[std::size_t(m)]) {
      if (auto it = mods[std::size_t(h)].macros.find(name); it != mods[std::size_t(h)].macros.end()) return &it->second;
    }
    return nullptr;
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
  std::unordered_map<std::string, std::vector<FnRef>> globals_;
  std::vector<std::vector<int>> includes_, visible_;
  std::vector<FnRef> address_taken_;
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

  std::vector<EdgeOut> run() {
    for (int f = 0; f < static_cast<int>(mod_.funcs.size()); ++f) {
      emit(p_.id(m_), p_.id(FnRef{m_, f}), EdgeKind::Defines, Confidence::Exact, mod_.funcs[std::size_t(f)].line);
    }
    for (const Include& inc : mod_.includes) on_include(inc);
    for (const CallSite& cs : mod_.calls) on_call(cs);
    for (int f = 0; f < static_cast<int>(mod_.funcs.size()); ++f) curl_edges(f);
    return std::move(out_);
  }

 private:
  const Project& p_;
  int m_;
  const ModuleInfo& mod_;
  std::vector<EdgeOut> out_;

  void emit(std::string from, std::string to, EdgeKind kind, Confidence c, std::uint32_t line,
            nlohmann::json attrs = nlohmann::json::object(), std::optional<Node> node = std::nullopt) {
    out_.push_back({std::move(from), std::move(to), kind, c, line, std::move(attrs), std::move(node)});
  }

  std::string caller(const CallSite& cs) const { return p_.id(FnRef{m_, cs.func}); }

  void on_include(const Include& inc) {
    if (const int h = p_.resolve_include(m_, inc); h >= 0) {
      if (h != m_) emit(p_.id(m_), p_.id(h), EdgeKind::Imports, Confidence::Exact, inc.line);
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

  // Calls hidden in a function-like macro: FLUSH_BLOCK(s, 0) -> flush_pending(...).
  void macro_calls(const CallSite& cs, const std::string& name, int depth, std::unordered_set<std::string>& seen) {
    const auto* body = p_.macro(m_, name);
    if (!body || depth > 4 || !seen.insert(name).second) return;
    for (const std::string& callee : *body) {
      const FnRef r = p_.resolve_call(m_, callee);
      if (r.mod >= 0) {
        emit(caller(cs), p_.id(r), EdgeKind::Calls, r.declared ? Confidence::Exact : Confidence::Inferred, cs.line,
             attrs_for(cs, r.declared ? "macro" : "macro_name_only"));
      }
      else macro_calls(cs, callee, depth + 1, seen);
    }
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
    const FnRef r = p_.resolve_call(m_, cs.callee);
    if (r.mod >= 0) {
      if (r.declared) emit(caller(cs), p_.id(r), EdgeKind::Calls, Confidence::Exact, cs.line, attrs_for(cs));
      else emit(caller(cs), p_.id(r), EdgeKind::Calls, Confidence::Inferred, cs.line, attrs_for(cs, "name_only"));
      return;
    }
    std::unordered_set<std::string> seen;
    macro_calls(cs, cs.callee, 0, seen);
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

Graph extract_c_files(const fs::path& root, const std::vector<fs::path>& rel_files, CStats* stats, unsigned threads) {
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
    const std::string src = read_file(root / files[i]);
    m.lines = count_lines(src);
    ts::TreePtr tree(ts_parser_parse_string(parser.get(), nullptr, src.data(), static_cast<uint32_t>(src.size())));
    const TSNode rootn = ts_tree_root_node(tree.get());
    m.syntax_error = ts_node_has_error(rootn);
    Summarizer(m, src).run(rootn);
  });
  project.index();

  std::vector<std::vector<EdgeOut>> edges(project.mods.size());
  parallel_for(project.mods.size(), threads,
               [&](std::size_t i) { edges[i] = Linker(project, static_cast<int>(i)).run(); });

  Graph g;
  g.source = root.string();
  for (int mi = 0; mi < static_cast<int>(project.mods.size()); ++mi) {
    const ModuleInfo& m = project.mods[std::size_t(mi)];
    Node mod;
    mod.id = project.id(mi);
    mod.kind = NodeKind::Module;
    mod.name = fs::path(m.path).filename().string();
    mod.loc = SourceLoc{m.path, 1, static_cast<std::uint32_t>(m.lines), std::nullopt};
    mod.frontend = "c";
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
    stats->files = project.mods.size();
    for (const auto& m : project.mods) {
      stats->lines += m.lines;
      stats->files_with_syntax_errors += m.syntax_error ? 1 : 0;
    }
  }
  return g;
}

}  // namespace optum
