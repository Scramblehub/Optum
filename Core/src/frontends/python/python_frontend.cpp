// Python frontend. Rules: docs/frontends.md ("Python").
//
// Pass 1 (parallel, one file per task): parse with tree-sitter and reduce each file to a compact
// ModuleInfo: definitions, imports, local names, simple assigned values and call sites. The
// syntax tree is dropped right after, so memory stays proportional to the summary.
// Between passes: index modules, resolve class bases, and index methods by name.
// Pass 2 (parallel, one module per task): resolve every call site and import to graph edges.

#include "optum/frontends/python.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "../../ts_util.hpp"
#include "../common.hpp"
#include "optum/frontend.hpp"
#include "optum/http.hpp"
#include "optum/sql.hpp"

extern "C" const TSLanguage* tree_sitter_python();

namespace optum {
namespace {

namespace fs = std::filesystem;
using frontends::parallel_for;
using frontends::count_lines;
using frontends::http_external_node;
using frontends::read_file;
using frontends::sql_store_node;
using frontends::to_upper;

// ===================================================================== pass 1 data

// A small, tree-free copy of the expressions we need to resolve later.
struct Expr {
  enum class Kind : std::uint8_t { Other, Name, Attr, Call, Str };
  Kind kind = Kind::Other;
  std::string text;       // Name: the identifier. Attr: the attribute.
  std::vector<Expr> sub;  // Attr: {object}. Call: {function}.
  // Str: literal pieces and holes; a hole holds the identifier inside {...}, or "" if complex.
  std::vector<std::pair<bool, std::string>> parts;
};

enum class DefKind : std::uint8_t { Module, Class, Function };

struct Binding {
  bool from = false;   // false: `import module [as x]`; true: `from module import name`
  std::string module;  // absolute dotted module
  std::string name;    // for `from`: the imported name
};

struct Def {
  std::string name;  // short name; "" for the module itself
  std::string qual;  // dotted path inside the module; "" for the module itself
  DefKind kind = DefKind::Module;
  int parent = -1;
  int attr_scope = 0;  // nearest function or module around the def statement
  std::uint32_t line = 1, end_line = 1;
  std::string self_name;    // methods: name of the first parameter
  std::vector<Expr> bases;  // classes
  // Classes: values assigned to self.<attr> in methods, with the method's scope (first few only).
  std::unordered_map<std::string, std::vector<std::pair<int, Expr>>> attr_values;
  std::unordered_map<std::string, int> children;
  std::unordered_map<std::string, Binding> bindings;
  std::unordered_set<std::string> locals;
  std::unordered_map<std::string, Expr> values;  // last simple assignment per local name
  std::vector<std::string> star_imports;
};

struct ImportRef {
  std::string module;  // absolute dotted
  std::string name;    // `from module import name`; "" for `import module` and `*`
  std::uint32_t line = 0;
};

struct CallSite {
  int scope = 0;  // attributed to this function (or the module)
  Expr callee;
  std::optional<Expr> first_arg;
  std::optional<Expr> url_kw;
  std::uint32_t line = 0;
};

struct ModuleInfo {
  std::string name;  // dotted
  std::string file;  // path relative to the root, for locations
  bool is_package = false;
  std::size_t lines = 0;
  bool syntax_error = false;
  std::vector<Def> defs;  // defs[0] is the module
  std::vector<ImportRef> imports;
  std::vector<CallSite> calls;
};

// ===================================================================== pass 1

class Summarizer {
 public:
  Summarizer(ModuleInfo& m, std::string_view src) : m_(m), src_(src) {}

  void run(TSNode root) {
    Def mod;
    mod.end_line = ts::end_line(root);
    m_.defs.push_back(std::move(mod));
    walk_children(root, 0, 0);
  }

 private:
  ModuleInfo& m_;
  std::string_view src_;

  std::string text(TSNode n) const { return std::string(ts::text(n, src_)); }

  void walk_children(TSNode n, int ds, int as) {
    ts::for_named(n, [&](TSNode c) { walk(c, ds, as); });
  }

  // ds: the def that new definitions belong to (module, class or function).
  // as: the scope calls are attributed to and names are resolved in (function or module).
  void walk(TSNode n, int ds, int as) {
    const char* type = ts_node_type(n);
    const std::string_view t(type);
    if (t == "function_definition") return on_function(n, ds, as);
    if (t == "class_definition") return on_class(n, ds, as);
    if (t == "decorated_definition") return on_decorated(n, ds, as);
    if (t == "call") on_call(n, as);
    else if (t == "assignment") return on_assignment(n, ds, as);
    else if (t == "augmented_assignment") bind_names(ts::field(n, "left"), ds, nullptr);
    else if (t == "for_statement" || t == "for_in_clause") bind_names(ts::field(n, "left"), ds, nullptr);
    else if (t == "as_pattern_target") bind_names(n, ds, nullptr);
    else if (t == "named_expression") bind_names(ts::field(n, "name"), ds, nullptr);
    else if (t == "import_statement" || t == "import_from_statement") return on_import(n, ds, as);
    walk_children(n, ds, as);
  }

  int add_def(TSNode n, std::string name, DefKind kind, int parent, int as) {
    if (auto it = m_.defs[static_cast<std::size_t>(parent)].children.find(name);
        it != m_.defs[static_cast<std::size_t>(parent)].children.end()) {
      return it->second;  // redefinition (e.g. in if/else branches): same node
    }
    Def d;
    const std::string& pq = m_.defs[static_cast<std::size_t>(parent)].qual;
    d.qual = pq.empty() ? name : pq + "." + name;
    d.name = std::move(name);
    d.kind = kind;
    d.parent = parent;
    d.attr_scope = as;
    d.line = ts::line(n);
    d.end_line = ts::end_line(n);
    const int idx = static_cast<int>(m_.defs.size());
    m_.defs[static_cast<std::size_t>(parent)].children.emplace(d.name, idx);
    m_.defs.push_back(std::move(d));
    return idx;
  }

  Def& def(int i) { return m_.defs[static_cast<std::size_t>(i)]; }

  void on_function(TSNode n, int ds, int as) {
    const int idx = add_def(n, text(ts::field(n, "name")), DefKind::Function, ds, as);
    const TSNode params = ts::field(n, "parameters");
    bool first = true;
    ts::for_named(params, [&](TSNode p) {
      std::string pname;
      const std::string_view pt = ts_node_type(p);
      if (pt == "identifier") pname = text(p);
      else if (pt == "default_parameter" || pt == "typed_default_parameter") pname = text(ts::field(p, "name"));
      else if (pt == "typed_parameter" || pt == "list_splat_pattern" || pt == "dictionary_splat_pattern") {
        ts::for_named(p, [&](TSNode c) {
          if (pname.empty() && ts::is(c, "identifier")) pname = text(c);
        });
      }
      if (pname.empty()) return;
      if (first && def(ds).kind == DefKind::Class) def(idx).self_name = pname;
      first = false;
      def(idx).locals.insert(std::move(pname));
    });
    walk(params, ds, as);  // default values run in the enclosing scope
    const TSNode body = ts::field(n, "body");
    if (!ts_node_is_null(body)) walk_children(body, idx, idx);
  }

  void on_class(TSNode n, int ds, int as) {
    const int idx = add_def(n, text(ts::field(n, "name")), DefKind::Class, ds, as);
    const TSNode supers = ts::field(n, "superclasses");
    if (!ts_node_is_null(supers)) {
      ts::for_named(supers, [&](TSNode b) {
        if (ts::is(b, "identifier") || ts::is(b, "attribute")) def(idx).bases.push_back(to_expr(b));
      });
      walk(supers, ds, as);
    }
    const TSNode body = ts::field(n, "body");
    // The class body's own statements run in the enclosing scope (Python skips class scopes
    // when resolving names inside methods), so calls there are attributed to `as`.
    if (!ts_node_is_null(body)) walk_children(body, idx, as);
  }

  void on_decorated(TSNode n, int ds, int as) {
    ts::for_named(n, [&](TSNode c) {
      if (!ts::is(c, "decorator")) return;
      const TSNode e = ts_node_named_child(c, 0);
      // `@name` calls name(fn); `@name(args)` is a call node, recorded by walking it.
      if (!ts::is(e, "call")) {
        CallSite cs;
        cs.scope = as;
        cs.callee = to_expr(e);
        cs.line = ts::line(e);
        m_.calls.push_back(std::move(cs));
      }
      walk(c, ds, as);
    });
    walk(ts::field(n, "definition"), ds, as);
  }

  void on_call(TSNode n, int as) {
    CallSite cs;
    cs.scope = as;
    cs.callee = to_expr(ts::field(n, "function"));
    cs.line = ts::line(n);
    const TSNode args = ts::field(n, "arguments");
    if (ts::is(args, "argument_list")) {
      ts::for_named(args, [&](TSNode a) {
        const std::string_view at = ts_node_type(a);
        if (at == "comment" || at == "list_splat" || at == "dictionary_splat") return;
        if (at == "keyword_argument") {
          if (ts::text(ts::field(a, "name"), src_) == "url") cs.url_kw = to_expr(ts::field(a, "value"));
          return;
        }
        if (!cs.first_arg) cs.first_arg = to_expr(a);
      });
    }
    m_.calls.push_back(std::move(cs));
  }

  void on_assignment(TSNode n, int ds, int as) {
    const TSNode left = ts::field(n, "left");
    const TSNode right = ts::field(n, "right");
    std::optional<Expr> value;
    if (ts::is(left, "identifier") && !ts_node_is_null(right)) value = to_expr(right);
    // self.attr = value inside a method: remember it on the class for typing later calls.
    if (ts::is(left, "attribute") && !ts_node_is_null(right) && def(ds).kind == DefKind::Function &&
        !def(ds).self_name.empty() && ts::text(ts::field(left, "object"), src_) == def(ds).self_name) {
      auto& vals = def(def(ds).parent).attr_values[text(ts::field(left, "attribute"))];
      if (vals.size() < 4) vals.emplace_back(ds, to_expr(right));
    }
    bind_names(left, ds, value ? &*value : nullptr);
    walk(left, ds, as);  // e.g. calls inside subscripts on the left
    if (!ts_node_is_null(right)) walk(right, ds, as);
  }

  // Records every identifier in an assignment target as a local name of `ds`. Assignments in
  // class bodies are class attributes, not locals, and are skipped.
  void bind_names(TSNode target, int ds, const Expr* value) {
    if (ts_node_is_null(target) || def(ds).kind == DefKind::Class) return;
    if (ts::is(target, "identifier")) {
      const std::string name = text(target);
      def(ds).locals.insert(name);
      if (value) def(ds).values[name] = *value;
      else def(ds).values.erase(name);
      return;
    }
    if (ts::is(target, "attribute") || ts::is(target, "subscript")) return;
    ts::for_named(target, [&](TSNode c) { bind_names(c, ds, nullptr); });
  }

  static std::string strip_spaces(std::string s) {
    std::erase_if(s, [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\\'; });
    return s;
  }

  // Absolute module for a `from` clause, handling relative imports.
  std::string from_module(TSNode mod) {
    if (!ts::is(mod, "relative_import")) return strip_spaces(text(mod));
    std::size_t level = 0;
    std::string rest;
    ts::for_named(mod, [&](TSNode c) {
      if (ts::is(c, "import_prefix")) level = ts::text(c, src_).size();
      else if (ts::is(c, "dotted_name")) rest = strip_spaces(text(c));
    });
    std::string base = m_.name;
    if (!m_.is_package) base = base.substr(0, base.find_last_of('.') == std::string::npos ? 0 : base.rfind('.'));
    for (std::size_t i = 1; i < level && !base.empty(); ++i) {
      const auto dot = base.rfind('.');
      base = dot == std::string::npos ? "" : base.substr(0, dot);
    }
    if (rest.empty()) return base;
    return base.empty() ? rest : base + "." + rest;
  }

  void on_import(TSNode n, int ds, int as) {
    const int bs = def(ds).kind == DefKind::Class ? as : ds;
    const std::uint32_t ln = ts::line(n);
    if (ts::is(n, "import_statement")) {
      ts::for_field(n, "name", [&](TSNode c) {
        if (ts::is(c, "aliased_import")) {
          const std::string mod = strip_spaces(text(ts::field(c, "name")));
          def(bs).bindings[text(ts::field(c, "alias"))] = Binding{false, mod, ""};
          m_.imports.push_back({mod, "", ln});
        } else {
          const std::string mod = strip_spaces(text(c));
          const std::string top = mod.substr(0, mod.find('.'));
          def(bs).bindings[top] = Binding{false, top, ""};
          def(bs).locals.erase(top);
          m_.imports.push_back({mod, "", ln});
        }
      });
      return;
    }
    const std::string mod = from_module(ts::field(n, "module_name"));
    bool any = false;
    ts::for_field(n, "name", [&](TSNode c) {
      any = true;
      std::string name, alias;
      if (ts::is(c, "aliased_import")) {
        name = strip_spaces(text(ts::field(c, "name")));
        alias = text(ts::field(c, "alias"));
      } else {
        name = alias = strip_spaces(text(c));
      }
      def(bs).bindings[alias] = Binding{true, mod, name};
      m_.imports.push_back({mod, name, ln});
    });
    if (!any) {  // `from m import *`
      def(bs).star_imports.push_back(mod);
      m_.imports.push_back({mod, "", ln});
    }
  }

  Expr to_expr(TSNode n) {
    Expr e;
    if (ts_node_is_null(n)) return e;
    const std::string_view t = ts_node_type(n);
    if (t == "identifier") {
      e.kind = Expr::Kind::Name;
      e.text = text(n);
    } else if (t == "attribute") {
      e.kind = Expr::Kind::Attr;
      e.text = text(ts::field(n, "attribute"));
      e.sub.push_back(to_expr(ts::field(n, "object")));
    } else if (t == "call") {
      e.kind = Expr::Kind::Call;
      e.sub.push_back(to_expr(ts::field(n, "function")));
    } else if (t == "string") {
      e.kind = Expr::Kind::Str;
      add_string_parts(n, e);
    } else if (t == "concatenated_string") {
      e.kind = Expr::Kind::Str;
      ts::for_named(n, [&](TSNode s) {
        if (ts::is(s, "string")) add_string_parts(s, e);
      });
    } else if (t == "parenthesized_expression" && ts_node_named_child_count(n) == 1) {
      return to_expr(ts_node_named_child(n, 0));
    }
    return e;
  }

  void add_string_parts(TSNode s, Expr& e) {
    ts::for_named(s, [&](TSNode c) {
      if (ts::is(c, "string_content")) {
        e.parts.emplace_back(false, text(c));
      } else if (ts::is(c, "interpolation")) {
        const TSNode inner = ts::field(c, "expression");
        e.parts.emplace_back(true, ts::is(inner, "identifier") ? text(inner) : "");
      }
    });
  }
};

// ===================================================================== between passes

struct DefRef {
  int mod = -1;
  int def = -1;
  bool operator==(const DefRef&) const = default;
};
struct DefRefHash {
  std::size_t operator()(const DefRef& r) const {
    return std::hash<std::uint64_t>{}((std::uint64_t(std::uint32_t(r.mod)) << 32) | std::uint32_t(r.def));
  }
};

const std::unordered_set<std::string_view>& stdlib_modules() {
  static const std::unordered_set<std::string_view> names = {
#include "stdlib_modules.inc"
  };
  return names;
}

bool is_builtin_method(const std::string& name) {
  static const std::unordered_set<std::string_view> names = {
#include "builtin_methods.inc"
  };
  return names.count(name) > 0;
}

bool is_stdlib(const std::string& dotted) {
  return stdlib_modules().count(std::string_view(dotted).substr(0, dotted.find('.'))) > 0;
}

struct Value {
  enum class Kind : std::uint8_t { Unknown, Module, Def, Instance, SelfInstance, Super, ThirdParty };
  Kind kind = Kind::Unknown;
  DefRef ref;        // Module: ref.mod. Others: the def (for instances: their class)
  std::string path;  // ThirdParty: dotted path, "()" marks a call result
};

struct NameHit {
  enum class Kind : std::uint8_t { None, Value, Local } kind = Kind::None;
  Value value;
  int scope = 0;  // Local: the scope that binds it
};

class Project {
 public:
  std::vector<ModuleInfo> mods;  // sorted by name

  void index() {
    for (int i = 0; i < static_cast<int>(mods.size()); ++i) {
      by_name_.emplace(mods[std::size_t(i)].name, i);
      const std::string& n = mods[std::size_t(i)].name;
      for (std::size_t dot = n.find('.'); dot != std::string::npos; dot = n.find('.', dot + 1)) {
        auto [it, inserted] = by_suffix_.emplace(n.substr(dot + 1), i);
        if (!inserted) it->second = -1;  // ambiguous
      }
    }
    for (int m = 0; m < static_cast<int>(mods.size()); ++m) {
      const auto& defs = mods[std::size_t(m)].defs;
      for (int d = 1; d < static_cast<int>(defs.size()); ++d) {
        const Def& df = defs[std::size_t(d)];
        if (df.kind == DefKind::Function && defs[std::size_t(df.parent)].kind == DefKind::Class) {
          methods_by_name_[df.name].push_back({m, d});
        }
      }
    }
    // Class bases need name resolution, which needs the module index above.
    for (int m = 0; m < static_cast<int>(mods.size()); ++m) {
      const auto& defs = mods[std::size_t(m)].defs;
      for (int d = 1; d < static_cast<int>(defs.size()); ++d) {
        const Def& df = defs[std::size_t(d)];
        if (df.kind != DefKind::Class) continue;
        for (const Expr& b : df.bases) {
          const Value v = resolve_expr(m, df.attr_scope, b, 0);
          if (v.kind == Value::Kind::Def && def(v.ref).kind == DefKind::Class && !(v.ref == DefRef{m, d})) {
            bases_[{m, d}].push_back(v.ref);
            subclasses_[v.ref].push_back({m, d});
          }
        }
      }
    }
  }

  const Def& def(DefRef r) const { return mods[std::size_t(r.mod)].defs[std::size_t(r.def)]; }

  std::string id(DefRef r) const {
    const ModuleInfo& m = mods[std::size_t(r.mod)];
    const Def& d = def(r);
    return "py:" + m.name + (d.qual.empty() ? "" : "." + d.qual);
  }

  int find_module(const std::string& dotted) const {
    if (auto it = by_name_.find(dotted); it != by_name_.end()) return it->second;
    if (is_stdlib(dotted)) return -1;
    if (auto it = by_suffix_.find(dotted); it != by_suffix_.end()) return it->second;
    return -1;
  }

  // ---- name resolution ----

  Value resolve_binding(const Binding& b, int depth) const {
    if (depth > 12) return {};
    if (!b.from) {
      if (const int i = find_module(b.module); i >= 0) return module_value(i);
      return is_stdlib(b.module) ? Value{} : third_party(b.module);
    }
    const std::string full = b.module + "." + b.name;
    if (const int i = find_module(full); i >= 0) return module_value(i);
    if (const int i = find_module(b.module); i >= 0) return resolve_member(i, b.name, depth + 1);
    return is_stdlib(b.module) ? Value{} : third_party(full);
  }

  Value resolve_member(int m, const std::string& name, int depth) const {
    if (depth > 12 || m < 0) return {};
    const ModuleInfo& mod = mods[std::size_t(m)];
    if (auto it = by_name_.find(mod.name + "." + name); it != by_name_.end()) return module_value(it->second);
    const Def& top = mod.defs[0];
    if (auto it = top.children.find(name); it != top.children.end()) return def_value({m, it->second});
    if (auto it = top.bindings.find(name); it != top.bindings.end()) {
      return resolve_binding(it->second, depth + 1);
    }
    for (const std::string& star : top.star_imports) {
      const int sm = find_module(star);
      if (sm >= 0 && sm != m) {
        Value v = resolve_member(sm, name, depth + 1);
        if (v.kind != Value::Kind::Unknown) return v;
      }
    }
    return {};
  }

  NameHit resolve_name(int m, int scope, const std::string& name, int depth) const {
    const auto& defs = mods[std::size_t(m)].defs;
    for (int s = scope; s >= 0; s = defs[std::size_t(s)].parent) {
      const Def& d = defs[std::size_t(s)];
      if (d.kind == DefKind::Class) continue;  // class scopes aren't visible from methods
      if (auto it = d.children.find(name); it != d.children.end()) {
        return {NameHit::Kind::Value, def_value({m, it->second}), s};
      }
      if (auto it = d.bindings.find(name); it != d.bindings.end()) {
        return {NameHit::Kind::Value, resolve_binding(it->second, depth + 1), s};
      }
      if (d.locals.count(name)) return {NameHit::Kind::Local, {}, s};
      if (s == 0) {
        for (const std::string& star : d.star_imports) {
          const int sm = find_module(star);
          if (sm < 0 || sm == m) continue;
          Value v = resolve_member(sm, name, depth + 1);
          if (v.kind != Value::Kind::Unknown) return {NameHit::Kind::Value, std::move(v), s};
        }
      }
    }
    return {};
  }

  // The class around a method scope, if any.
  std::optional<DefRef> enclosing_class(int m, int scope) const {
    const auto& defs = mods[std::size_t(m)].defs;
    const Def& d = defs[std::size_t(scope)];
    if (d.kind == DefKind::Function && d.parent >= 0 && defs[std::size_t(d.parent)].kind == DefKind::Class) {
      return DefRef{m, d.parent};
    }
    return std::nullopt;
  }

  Value resolve_expr(int m, int scope, const Expr& e, int depth) const {
    if (depth > 10) return {};
    switch (e.kind) {
      case Expr::Kind::Name: {
        const NameHit hit = resolve_name(m, scope, e.text, depth);
        if (hit.kind == NameHit::Kind::Value) return hit.value;
        if (hit.kind != NameHit::Kind::Local) return {};
        const Def& d = mods[std::size_t(m)].defs[std::size_t(hit.scope)];
        if (!d.self_name.empty() && d.self_name == e.text) {
          if (auto cls = enclosing_class(m, hit.scope)) return {Value::Kind::SelfInstance, *cls, {}};
        }
        if (auto it = d.values.find(e.text); it != d.values.end()) {
          return resolve_expr(m, hit.scope, it->second, depth + 1);
        }
        return {};
      }
      case Expr::Kind::Attr: {
        const Value obj = resolve_expr(m, scope, e.sub[0], depth + 1);
        return member_of(obj, e.text, depth);
      }
      case Expr::Kind::Call: {
        const Expr& f = e.sub[0];
        if (f.kind == Expr::Kind::Name && f.text == "super" &&
            resolve_name(m, scope, "super", depth).kind == NameHit::Kind::None) {
          if (auto cls = enclosing_class(m, scope)) return {Value::Kind::Super, *cls, {}};
          return {};
        }
        const Value fv = resolve_expr(m, scope, f, depth + 1);
        if (fv.kind == Value::Kind::Def && def(fv.ref).kind == DefKind::Class) {
          return {Value::Kind::Instance, fv.ref, {}};
        }
        if (fv.kind == Value::Kind::ThirdParty) return third_party(fv.path + "()");
        return {};
      }
      default:
        return {};
    }
  }

  Value member_of(const Value& obj, const std::string& name, int depth) const {
    switch (obj.kind) {
      case Value::Kind::Module:
        return resolve_member(obj.ref.mod, name, depth + 1);
      case Value::Kind::Def:
        if (def(obj.ref).kind != DefKind::Class) return {};
        [[fallthrough]];
      case Value::Kind::Instance:
      case Value::Kind::SelfInstance:
        if (auto m = lookup_method(obj.ref, name)) return def_value(*m);
        return lookup_attr(obj.ref, name, depth + 1);
      case Value::Kind::Super:
        for (const DefRef& b : bases_of(obj.ref)) {
          if (auto m = lookup_method(b, name)) return def_value(*m);
        }
        return {};
      case Value::Kind::ThirdParty:
        return third_party(obj.path + "." + name);
      default:
        return {};
    }
  }

  // The definition `name` on class `cls`: the class itself first, then its bases depth-first.
  std::optional<DefRef> lookup_method(DefRef cls, const std::string& name, int depth = 0) const {
    if (depth > 32) return std::nullopt;
    const Def& c = def(cls);
    if (auto it = c.children.find(name); it != c.children.end()) return DefRef{cls.mod, it->second};
    for (const DefRef& b : bases_of(cls)) {
      if (auto r = lookup_method(b, name, depth + 1)) return r;
    }
    return std::nullopt;
  }

  // The value of instance attribute `name` (self.name = ...) on `cls` or its bases: the first
  // assignment that resolves to something known.
  Value lookup_attr(DefRef cls, const std::string& name, int depth) const {
    if (depth > 10) return {};
    const Def& c = def(cls);
    if (auto it = c.attr_values.find(name); it != c.attr_values.end()) {
      for (const auto& [scope, expr] : it->second) {
        Value v = resolve_expr(cls.mod, scope, expr, depth + 1);
        if (v.kind != Value::Kind::Unknown) return v;
      }
    }
    for (const DefRef& b : bases_of(cls)) {
      Value v = lookup_attr(b, name, depth + 1);
      if (v.kind != Value::Kind::Unknown) return v;
    }
    return {};
  }

  // Every class that inherits from `cls`, directly or not.
  std::vector<DefRef> descendants(DefRef cls) const {
    std::vector<DefRef> out;
    std::unordered_set<DefRef, DefRefHash> seen{cls};
    std::vector<DefRef> todo{cls};
    while (!todo.empty()) {
      const DefRef c = todo.back();
      todo.pop_back();
      if (auto it = subclasses_.find(c); it != subclasses_.end()) {
        for (const DefRef& s : it->second) {
          if (seen.insert(s).second) {
            out.push_back(s);
            todo.push_back(s);
          }
        }
      }
    }
    return out;
  }

  const std::vector<DefRef>* methods_named(const std::string& name) const {
    auto it = methods_by_name_.find(name);
    return it == methods_by_name_.end() ? nullptr : &it->second;
  }

  // A string value: literals, f-strings (unknown holes become "{}") and names bound to them,
  // including module constants imported from other project modules.
  std::optional<std::string> eval_str(int m, int scope, const Expr& e, int depth) const {
    if (depth > 8) return std::nullopt;
    if (e.kind == Expr::Kind::Str) {
      std::string out;
      for (const auto& [hole, piece] : e.parts) {
        if (!hole) {
          out += piece;
          continue;
        }
        std::optional<std::string> v;
        if (!piece.empty()) v = eval_str(m, scope, Expr{Expr::Kind::Name, piece, {}, {}}, depth + 1);
        out += v ? *v : "{}";
      }
      return out;
    }
    if (e.kind != Expr::Kind::Name) return std::nullopt;
    const auto& defs = mods[std::size_t(m)].defs;
    for (int s = scope; s >= 0; s = defs[std::size_t(s)].parent) {
      const Def& d = defs[std::size_t(s)];
      if (d.kind == DefKind::Class) continue;
      if (auto it = d.values.find(e.text); it != d.values.end()) return eval_str(m, s, it->second, depth + 1);
      if (d.locals.count(e.text)) return std::nullopt;
      if (auto it = d.bindings.find(e.text); it != d.bindings.end() && it->second.from) {
        const int other = find_module(it->second.module);
        if (other >= 0) return eval_str(other, 0, Expr{Expr::Kind::Name, it->second.name, {}, {}}, depth + 1);
        return std::nullopt;
      }
    }
    return std::nullopt;
  }

 private:
  std::unordered_map<std::string, int> by_name_;
  std::unordered_map<std::string, int> by_suffix_;
  std::unordered_map<DefRef, std::vector<DefRef>, DefRefHash> bases_, subclasses_;
  std::unordered_map<std::string, std::vector<DefRef>> methods_by_name_;

  static Value module_value(int i) { return {Value::Kind::Module, {i, 0}, {}}; }
  static Value def_value(DefRef r) { return {Value::Kind::Def, r, {}}; }
  static Value third_party(std::string path) { return {Value::Kind::ThirdParty, {}, std::move(path)}; }

  const std::vector<DefRef>& bases_of(DefRef cls) const {
    static const std::vector<DefRef> none;
    auto it = bases_.find(cls);
    return it == bases_.end() ? none : it->second;
  }
};

// ===================================================================== pass 2

struct EdgeOut {
  std::string from, to;
  EdgeKind kind = EdgeKind::Calls;
  Confidence confidence = Confidence::Exact;
  std::uint32_t line = 0;
  nlohmann::json attrs = nlohmann::json::object();
  std::optional<Node> new_node;  // externals, stores and third-party packages
};

class Linker {
 public:
  Linker(const Project& p, int m) : p_(p), m_(m), mod_(p.mods[std::size_t(m)]) {}

  std::vector<EdgeOut> run() {
    for (int d = 1; d < static_cast<int>(mod_.defs.size()); ++d) {
      emit(id(mod_.defs[std::size_t(d)].parent), id(d), EdgeKind::Defines, Confidence::Exact, mod_.defs[std::size_t(d)].line);
    }
    for (const ImportRef& imp : mod_.imports) on_import(imp);
    for (const CallSite& cs : mod_.calls) on_call(cs);
    return std::move(out_);
  }

 private:
  const Project& p_;
  int m_;
  const ModuleInfo& mod_;
  std::vector<EdgeOut> out_;

  std::string id(int def) const { return p_.id({m_, def}); }

  void emit(std::string from, std::string to, EdgeKind kind, Confidence c, std::uint32_t line,
            nlohmann::json attrs = nlohmann::json::object(), std::optional<Node> node = std::nullopt) {
    out_.push_back({std::move(from), std::move(to), kind, c, line, std::move(attrs), std::move(node)});
  }

  void on_import(const ImportRef& imp) {
    std::string target;
    std::optional<Node> node;
    if (!imp.name.empty()) {
      if (const int i = p_.find_module(imp.module + "." + imp.name); i >= 0) target = p_.id({i, 0});
    }
    if (target.empty()) {
      // The most specific project module named, or else the third-party package.
      std::string mod = imp.module;
      while (!mod.empty() && target.empty()) {
        if (const int i = p_.find_module(mod); i >= 0) target = p_.id({i, 0});
        const auto dot = mod.rfind('.');
        mod = dot == std::string::npos ? "" : mod.substr(0, dot);
      }
    }
    if (target.empty() && !imp.module.empty() && !is_stdlib(imp.module)) {
      const std::string top = imp.module.substr(0, imp.module.find('.'));
      target = "py:" + top;
      Node n;
      n.id = target;
      n.kind = NodeKind::Module;
      n.name = top;
      n.frontend = "python";
      n.attrs = {{"third_party", true}};
      node = std::move(n);
    }
    if (target.empty() || target == id(0)) return;
    emit(id(0), target, EdgeKind::Imports, Confidence::Exact, imp.line, nlohmann::json::object(), std::move(node));
  }

  // `via` says how an inferred edge was found: "override", "local_type" or "unique_method_name".
  void call_edge(const CallSite& cs, DefRef target, Confidence c, const char* via = nullptr) {
    const DefKind k = p_.def(target).kind;
    if (k != DefKind::Function && k != DefKind::Class) return;
    nlohmann::json attrs = nlohmann::json::object();
    if (via) attrs["via"] = via;
    emit(id(cs.scope), p_.id(target), EdgeKind::Calls, c, cs.line, std::move(attrs));
  }

  void on_call(const CallSite& cs) {
    const Expr& callee = cs.callee;
    if (callee.kind == Expr::Kind::Attr && try_sql(cs)) return;

    if (callee.kind == Expr::Kind::Attr) {
      const Value obj = p_.resolve_expr(m_, cs.scope, callee.sub[0], 0);
      const std::string& name = callee.text;
      switch (obj.kind) {
        case Value::Kind::SelfInstance: {
          if (auto mth = p_.lookup_method(obj.ref, name)) call_edge(cs, *mth, Confidence::Exact);
          for (const DefRef& sub : p_.descendants(obj.ref)) {  // overrides may run instead
            const Def& sd = p_.def(sub);
            if (auto it = sd.children.find(name); it != sd.children.end()) {
              call_edge(cs, {sub.mod, it->second}, Confidence::Inferred, "override");
            }
          }
          return;
        }
        case Value::Kind::Instance:
          if (auto mth = p_.lookup_method(obj.ref, name)) {
            if (callee.sub[0].kind == Expr::Kind::Call) call_edge(cs, *mth, Confidence::Exact);
            else call_edge(cs, *mth, Confidence::Inferred, "local_type");
          }
          return;
        case Value::Kind::Unknown:
          // Receiver unknown: if exactly one project class defines a method by this name, it's the
          // likely target. Not for dunders or names builtin types use (x.startswith(), x.get()):
          // those are far more likely to be the builtin.
          if (!name.starts_with("__") && !is_builtin_method(name)) {
            if (const auto* ms = p_.methods_named(name); ms && ms->size() == 1) {
              call_edge(cs, ms->front(), Confidence::Inferred, "unique_method_name");
            }
          }
          return;
        default: {
          const Value v = p_.member_of(obj, name, 0);
          if (v.kind == Value::Kind::Def) call_edge(cs, v.ref, Confidence::Exact);
          else if (v.kind == Value::Kind::ThirdParty) try_http(cs, v.path);
          return;
        }
      }
    }

    const Value v = p_.resolve_expr(m_, cs.scope, callee, 0);
    if (v.kind == Value::Kind::Def) call_edge(cs, v.ref, Confidence::Exact);
    else if (v.kind == Value::Kind::ThirdParty) try_http(cs, v.path);
  }

  bool try_sql(const CallSite& cs) {
    const std::string& name = cs.callee.text;
    if ((name != "execute" && name != "executemany" && name != "executescript") || !cs.first_arg) return false;
    const auto sql = p_.eval_str(m_, cs.scope, *cs.first_arg, 0);
    if (!sql) return false;
    const SqlTables t = sql_tables(*sql);
    if (t.reads.empty() && t.writes.empty()) return false;
    auto store = [&](const std::string& table, EdgeKind kind) {
      Node n = sql_store_node(sql_store_id(table), table, "python");
      std::string to = n.id;  // copied first: argument evaluation order is unspecified
      emit(id(cs.scope), std::move(to), kind, Confidence::Exact, cs.line, nlohmann::json::object(), std::move(n));
    };
    for (const auto& r : t.reads) store(r, EdgeKind::Reads);
    for (const auto& w : t.writes) store(w, EdgeKind::Writes);
    return true;
  }

  void try_http(const CallSite& cs, const std::string& path) {
    const auto dot = path.find('.');
    if (dot == std::string::npos) return;
    const std::string lib = path.substr(0, dot), method = path.substr(dot + 1);
    if (lib != "requests" && lib != "httpx") return;
    EdgeKind kind;
    if (method == "get" || method == "head" || method == "options") kind = EdgeKind::Reads;
    else if (method == "post" || method == "put" || method == "patch" || method == "delete") kind = EdgeKind::SendsTo;
    else return;
    const Expr* url = cs.first_arg ? &*cs.first_arg : cs.url_kw ? &*cs.url_kw : nullptr;
    if (!url) return;
    const auto s = p_.eval_str(m_, cs.scope, *url, 0);
    if (!s) return;
    const std::string node_id = http_node_id(*s);
    if (node_id.empty()) return;
    Node n = http_external_node(node_id, "python");
    const Confidence c = n.confidence;
    const std::string upper = to_upper(method);
    emit(id(cs.scope), node_id, kind, c, cs.line, {{"method", upper}}, std::move(n));
  }
};

// ===================================================================== driver

std::string module_name(const fs::path& rel, bool& is_package) {
  fs::path no_ext = rel;
  no_ext.replace_extension();
  std::string name;
  for (const auto& part : no_ext) {
    if (!name.empty()) name += '.';
    name += part.string();
  }
  is_package = rel.filename() == "__init__.py";
  if (is_package) {
    const auto dot = name.rfind('.');
    name = dot == std::string::npos ? "" : name.substr(0, dot);
  }
  return name;
}

}  // namespace

Graph extract_python(const fs::path& path, PythonStats* stats, unsigned threads) {
  Discovery d = discover(path);
  std::vector<fs::path> py;
  for (auto& f : d.files) {
    if (f.extension() == ".py") py.push_back(std::move(f));
  }
  Graph g = extract_python_files(d.root, py, stats, threads);
  g.source = path.string();
  return g;
}

Graph extract_python_files(const fs::path& root, const std::vector<fs::path>& rel_files,
                           PythonStats* stats, unsigned threads) {
  std::vector<fs::path> files;
  for (const auto& f : rel_files) files.push_back(root / f);
  std::sort(files.begin(), files.end());
  const fs::path& path = root;
  // Name modules from the outermost enclosing package: analysing .../email (which has an
  // __init__.py) must give email.message, not message, or imports won't resolve.
  fs::path base = fs::absolute(root).lexically_normal();
  if (!base.has_filename()) base = base.parent_path();
  while (fs::exists(base / "__init__.py") && base.has_parent_path() && base.parent_path() != base) {
    base = base.parent_path();
  }

  Project project;
  project.mods.resize(files.size());
  parallel_for(files.size(), threads, [&](std::size_t i) {
    thread_local ts::ParserPtr parser = [] {
      ts::ParserPtr p(ts_parser_new());
      ts_parser_set_language(p.get(), tree_sitter_python());
      return p;
    }();
    ModuleInfo& m = project.mods[i];
    const fs::path rel = fs::relative(fs::absolute(files[i]), base);
    m.file = rel.generic_string();
    m.name = module_name(rel, m.is_package);
    if (m.name.empty()) m.name = base.filename().string();

    const std::string src = read_file(files[i]);
    m.lines = count_lines(src);
    ts::TreePtr tree(ts_parser_parse_string(parser.get(), nullptr, src.data(), static_cast<uint32_t>(src.size())));
    const TSNode rootn = ts_tree_root_node(tree.get());
    m.syntax_error = ts_node_has_error(rootn);
    Summarizer(m, src).run(rootn);
  });

  // Two files can map to one module name (e.g. a.py and a/__init__.py); keep the first.
  std::stable_sort(project.mods.begin(), project.mods.end(),
                   [](const ModuleInfo& a, const ModuleInfo& b) { return a.name < b.name; });
  project.mods.erase(std::unique(project.mods.begin(), project.mods.end(),
                                 [](const ModuleInfo& a, const ModuleInfo& b) { return a.name == b.name; }),
                     project.mods.end());
  project.index();

  std::vector<std::vector<EdgeOut>> edges(project.mods.size());
  parallel_for(project.mods.size(), threads,
               [&](std::size_t i) { edges[i] = Linker(project, static_cast<int>(i)).run(); });

  Graph g;
  g.source = path.string();
  for (std::size_t mi = 0; mi < project.mods.size(); ++mi) {
    const ModuleInfo& m = project.mods[mi];
    for (std::size_t d = 0; d < m.defs.size(); ++d) {
      const Def& df = m.defs[d];
      Node n;
      n.id = project.id({static_cast<int>(mi), static_cast<int>(d)});
      n.kind = d == 0 ? NodeKind::Module : df.kind == DefKind::Class ? NodeKind::DataType : NodeKind::Function;
      n.name = d == 0 ? m.name.substr(m.name.rfind('.') == std::string::npos ? 0 : m.name.rfind('.') + 1) : df.name;
      n.loc = SourceLoc{m.file, df.line, df.end_line, std::nullopt};
      n.frontend = "python";
      g.add_node(std::move(n));
    }
    // Packages define their submodules.
    const auto dot = m.name.rfind('.');
    if (dot != std::string::npos) {
      if (auto parent = g.find("py:" + m.name.substr(0, dot))) {
        const int pi = project.find_module(m.name.substr(0, dot));
        if (pi >= 0 && project.mods[std::size_t(pi)].is_package) {
          Edge e;
          e.from = *parent;
          e.to = *g.find("py:" + m.name);
          e.kind = EdgeKind::Defines;
          e.frontend = "python";
          g.add_edge(std::move(e));
        }
      }
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
      e.frontend = "python";
      if (eo.line) e.loc = SourceLoc{project.mods[mi].file, eo.line, 0, std::nullopt};
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
