// JavaScript / TypeScript frontend. Rules: docs/frontends.md ("JavaScript and TypeScript").
//
// Same shape as the Python frontend: pass 1 (parallel, per file) reduces each syntax tree to a
// compact ModuleInfo; pass 2 (parallel, per module) resolves calls and imports. JS specifics:
// modules are files; exports are explicit (ES `export`, CommonJS `module.exports`); `this` is
// lexical in arrow functions only; anonymous callbacks are scopes of their own whose calls count
// for the enclosing named function; TypeScript annotations give types to parameters and fields.

#include "optum/frontends/js.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "../../ts_util.hpp"
#include "../common.hpp"
#include "optum/http.hpp"
#include "optum/sql.hpp"

extern "C" const TSLanguage* tree_sitter_javascript();
extern "C" const TSLanguage* tree_sitter_typescript();
extern "C" const TSLanguage* tree_sitter_tsx();

namespace optum {
namespace {

namespace fs = std::filesystem;
using frontends::parallel_for;
using frontends::read_file;

// ===================================================================== pass 1 data

struct Expr {
  enum class Kind : std::uint8_t { Other, Name, Member, Call, New, Str, This, Super, TypeRef, Require };
  Kind kind = Kind::Other;
  std::string text;       // Name: identifier. Member: property. Require: the module specifier.
  std::vector<Expr> sub;  // Member: {object}. Call: {function}. New: {constructor}. TypeRef: {type}.
  std::vector<std::pair<bool, std::string>> parts;  // Str: literal pieces and holes (identifier or "")
};

enum class DefKind : std::uint8_t { Module, Class, Function };

struct Binding {
  bool ns = false;    // true: the whole module (import * as x, const x = require(...))
  std::string spec;   // module specifier as written
  std::string name;   // otherwise: the exported name ("default" for default imports)
};

struct Def {
  std::string name, qual;
  DefKind kind = DefKind::Module;
  int parent = -1;
  bool arrow = false;      // arrow functions see the outer `this`
  bool anonymous = false;  // callbacks: a scope, but not a node
  std::string accessor;    // "get" / "set" for getters and setters
  std::uint32_t line = 1, end_line = 1;
  std::optional<Expr> base;  // classes: extends
  std::unordered_map<std::string, int> children;
  std::unordered_map<std::string, Binding> bindings;
  std::unordered_set<std::string> locals;
  std::unordered_map<std::string, Expr> values;  // last simple value, or a TypeRef from an annotation
  // Classes: values (or TypeRefs) of this.<attr>, with the scope to resolve them in.
  std::unordered_map<std::string, std::vector<std::pair<int, Expr>>> attr_values;
};

struct Export {
  enum class Kind : std::uint8_t { Local, Reexport, Namespace };
  Kind kind = Kind::Local;
  std::string name;  // Local: name in module scope. Reexport: name in the other module.
  std::string spec;  // Reexport / Namespace
};

struct ImportRef {
  std::string spec;
  std::uint32_t line = 0;
};

struct CallSite {
  int scope = 0;  // where names resolve (may be an anonymous callback)
  int attr = 0;   // who the call counts for: the nearest named function, or the module
  Expr callee;
  bool is_new = false;
  std::optional<Expr> first_arg;
  std::string method_option;  // fetch(url, { method: "POST" })
  std::uint32_t line = 0;
};

struct ModuleInfo {
  std::string path;  // relative to the root, '/' separated
  std::size_t lines = 0;
  bool syntax_error = false;
  std::vector<Def> defs;  // defs[0] is the module
  std::vector<ImportRef> imports;
  std::vector<CallSite> calls;
  std::unordered_map<std::string, Export> exports;
  std::vector<std::string> star_exports;
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
  Def& def(int i) { return m_.defs[static_cast<std::size_t>(i)]; }

  void walk_children(TSNode n, int s, int a) {
    ts::for_named(n, [&](TSNode c) { walk(c, s, a); });
  }

  // s: the scope new definitions and locals belong to. a: who calls count for.
  void walk(TSNode n, int s, int a) {
    const std::string_view t = ts_node_type(n);
    if (t == "function_declaration" || t == "generator_function_declaration") {
      return on_function(n, text(ts::field(n, "name")), s, false);
    }
    if (t == "class_declaration" || t == "abstract_class_declaration") return on_class(n, text(ts::field(n, "name")), s, a);
    if (t == "method_definition") return on_method(n, s);
    if (t == "public_field_definition" || t == "field_definition") return on_field(n, s, a);
    if (t == "variable_declarator") return on_declarator(n, s, a);
    if (t == "arrow_function" || t == "function_expression" || t == "function" || t == "generator_function") {
      return on_anonymous(n, s, a);
    }
    if (t == "import_statement") return on_import(n);
    if (t == "export_statement") return on_export(n, s, a);
    if (t == "assignment_expression") return on_assignment(n, s, a);
    if (t == "call_expression") on_call(n, s, a, false);
    else if (t == "new_expression") on_call(n, s, a, true);
    else if (t == "for_in_statement") bind_names(ts::field(n, "left"), s, nullptr);
    else if (t == "catch_clause") bind_names(ts::field(n, "parameter"), s, nullptr);
    walk_children(n, s, a);
  }

  int add_def(TSNode n, std::string name, DefKind kind, int parent, bool arrow, bool anonymous = false) {
    if (!anonymous) {
      if (auto it = def(parent).children.find(name); it != def(parent).children.end()) return it->second;
    }
    Def d;
    // Names are qualified by the nearest named ancestor: callbacks don't appear in ids.
    int named = parent;
    while (named > 0 && def(named).anonymous) named = def(named).parent;
    const std::string& pq = def(named).qual;
    d.qual = anonymous ? pq : pq.empty() ? name : pq + "." + name;
    d.name = std::move(name);
    d.kind = kind;
    d.parent = parent;
    d.arrow = arrow;
    d.anonymous = anonymous;
    d.line = ts::line(n);
    d.end_line = ts::end_line(n);
    const int idx = static_cast<int>(m_.defs.size());
    if (!anonymous) def(parent).children.emplace(d.name, idx);
    m_.defs.push_back(std::move(d));
    return idx;
  }

  // The scope a new named def or local should be visible from: callbacks are scopes too.
  int named_attr(int s) {
    while (s > 0 && def(s).anonymous) s = def(s).parent;
    return s;
  }

  void bind_params(TSNode params, int idx, bool constructor, int cls) {
    if (ts_node_is_null(params)) return;
    if (ts::is(params, "identifier")) {  // x => ...
      def(idx).locals.insert(text(params));
      return;
    }
    ts::for_named(params, [&](TSNode p) {
      const std::string_view pt = ts_node_type(p);
      if (pt == "required_parameter" || pt == "optional_parameter") {
        TSNode pat = ts::field(p, "pattern");
        if (ts_node_is_null(pat)) pat = ts::field(p, "name");
        std::optional<Expr> type = type_ref(ts::field(p, "type"));
        bind_names(pat, idx, type ? &*type : nullptr);
        // TypeScript parameter properties: constructor(private repo: Repo) sets this.repo.
        bool property = false;
        const uint32_t count = ts_node_child_count(p);
        for (uint32_t i = 0; i < count; ++i) {
          const std::string_view ct = ts_node_type(ts_node_child(p, i));
          property = property || ct == "accessibility_modifier" || ct == "readonly";
        }
        if (constructor && property && cls >= 0 && ts::is(pat, "identifier") && type) {
          def(cls).attr_values[text(pat)].emplace_back(idx, *type);
        }
      } else if (pt == "assignment_pattern") {
        bind_names(ts::field(p, "left"), idx, nullptr);
      } else if (pt != "comment") {
        bind_names(p, idx, nullptr);
      }
    });
  }

  void on_function(TSNode n, std::string name, int s, bool arrow, int cls = -1) {
    if (name.empty()) return on_anonymous(n, s, named_attr(s));
    const int idx = add_def(n, std::move(name), DefKind::Function, s, arrow);
    TSNode params = ts::field(n, "parameters");
    if (ts_node_is_null(params)) params = ts::field(n, "parameter");
    bind_params(params, idx, def(idx).name == "constructor", cls);
    if (!ts_node_is_null(params)) walk(params, s, named_attr(s));  // default values
    const TSNode body = ts::field(n, "body");
    if (!ts_node_is_null(body)) walk(body, idx, idx);
  }

  void on_anonymous(TSNode n, int s, int a) {
    const bool arrow = ts::is(n, "arrow_function");
    const int idx = add_def(n, "", DefKind::Function, s, arrow, true);
    TSNode params = ts::field(n, "parameters");
    if (ts_node_is_null(params)) params = ts::field(n, "parameter");
    bind_params(params, idx, false, -1);
    const TSNode body = ts::field(n, "body");
    if (!ts_node_is_null(body)) walk(body, idx, a);
  }

  void on_class(TSNode n, std::string name, int s, int a) {
    if (name.empty()) name = "default";
    const int idx = add_def(n, std::move(name), DefKind::Class, s, false);
    ts::for_named(n, [&](TSNode c) {
      if (!ts::is(c, "class_heritage")) return;
      ts::for_named(c, [&](TSNode h) {
        if (ts::is(h, "extends_clause")) def(idx).base = to_expr(ts::field(h, "value"));  // TypeScript
        else if (!ts::is(h, "implements_clause") && !def(idx).base) def(idx).base = to_expr(h);  // JavaScript
      });
      walk(c, s, a);
    });
    const TSNode body = ts::field(n, "body");
    // Field initializers run at construction but read like class-body code: they count for `a`.
    if (!ts_node_is_null(body)) walk_children(body, idx, a);
  }

  void on_method(TSNode n, int cls) {
    const TSNode name = ts::field(n, "name");
    if (ts::is(name, "computed_property_name")) return;
    std::string nm = text(name);
    if (ts::is(name, "string") && nm.size() >= 2) nm = nm.substr(1, nm.size() - 2);
    // Object literal methods ({ get() {...} }) aren't nodes yet: a scope, like a callback.
    if (def(cls).kind != DefKind::Class) return on_anonymous(n, cls, named_attr(cls));
    on_function(n, nm, cls, false, cls);
    const uint32_t count = ts_node_child_count(n);
    for (uint32_t i = 0; i < count; ++i) {
      const std::string_view ct = ts_node_type(ts_node_child(n, i));
      if (ct == "get" || ct == "set") def(def(cls).children.at(nm)).accessor = std::string(ct);
    }
  }

  void on_field(TSNode n, int cls, int a) {
    TSNode name = ts::field(n, "name");
    if (ts_node_is_null(name)) name = ts::field(n, "property");
    const TSNode value = ts::field(n, "value");
    if (ts_node_is_null(name) || ts::is(name, "computed_property_name")) return;
    const std::string nm = text(name);
    if (ts::is(value, "arrow_function") || ts::is(value, "function_expression")) {
      return on_function(value, nm, cls, ts::is(value, "arrow_function"));
    }
    if (def(cls).kind == DefKind::Class) {
      std::optional<Expr> type = type_ref(ts::field(n, "type"));
      if (type) def(cls).attr_values[nm].emplace_back(a, *type);
      else if (!ts_node_is_null(value)) def(cls).attr_values[nm].emplace_back(a, to_expr(value));
    }
    if (!ts_node_is_null(value)) walk(value, cls, a);
  }

  // require("x") or require("x").name, as written in a declarator.
  static bool is_require(TSNode call, std::string_view src) {
    return ts::is(call, "call_expression") && ts::text(ts::field(call, "function"), src) == "require";
  }
  std::optional<std::string> string_arg(TSNode call) {
    const TSNode args = ts::field(call, "arguments");
    if (ts_node_is_null(args) || ts_node_named_child_count(args) == 0) return std::nullopt;
    const Expr e = to_expr(ts_node_named_child(args, 0));
    if (e.kind != Expr::Kind::Str) return std::nullopt;
    std::string out;
    for (const auto& [hole, piece] : e.parts) {
      if (hole) return std::nullopt;
      out += piece;
    }
    return out;
  }

  void on_declarator(TSNode n, int s, int a) {
    const TSNode name = ts::field(n, "name");
    const TSNode value = ts::field(n, "value");
    const std::string_view vt = ts_node_is_null(value) ? "" : ts_node_type(value);
    if (ts::is(name, "identifier") && (vt == "arrow_function" || vt == "function_expression" || vt == "function" ||
                                       vt == "generator_function")) {
      return on_function(value, text(name), s, vt == "arrow_function");
    }
    if (ts::is(name, "identifier") && vt == "class") return on_class(value, text(name), s, a);

    // CommonJS imports.
    TSNode req = value;
    std::string member;
    if (vt == "member_expression" && is_require(ts::field(value, "object"), src_)) {
      req = ts::field(value, "object");
      member = text(ts::field(value, "property"));
    }
    if (is_require(req, src_)) {
      if (auto spec = string_arg(req)) {
        m_.imports.push_back({*spec, ts::line(n)});
        auto bind = [&](const std::string& local, Binding b) {
          def(s).bindings[local] = std::move(b);
          def(s).locals.erase(local);
        };
        if (ts::is(name, "identifier")) {
          bind(text(name), member.empty() ? Binding{true, *spec, ""} : Binding{false, *spec, member});
        } else if (ts::is(name, "object_pattern") && member.empty()) {
          ts::for_named(name, [&](TSNode p) {
            if (ts::is(p, "shorthand_property_identifier_pattern")) bind(text(p), Binding{false, *spec, text(p)});
            else if (ts::is(p, "pair_pattern") && ts::is(ts::field(p, "value"), "identifier")) {
              bind(text(ts::field(p, "value")), Binding{false, *spec, text(ts::field(p, "key"))});
            }
          });
        }
        return;
      }
    }
    std::optional<Expr> v;
    if (std::optional<Expr> type = type_ref(ts::field(n, "type"))) v = std::move(type);
    else if (!ts_node_is_null(value)) v = to_expr(value);
    bind_names(name, s, ts::is(name, "identifier") && v ? &*v : nullptr);
    if (!ts_node_is_null(value)) walk(value, s, a);
  }

  void bind_names(TSNode target, int s, const Expr* value) {
    if (ts_node_is_null(target) || def(s).kind == DefKind::Class) return;
    const std::string_view t = ts_node_type(target);
    if (t == "identifier" || t == "shorthand_property_identifier_pattern") {
      const std::string name = text(target);
      def(s).locals.insert(name);
      if (value) def(s).values[name] = *value;
      else def(s).values.erase(name);
      return;
    }
    if (t == "member_expression" || t == "subscript_expression") return;
    if (t == "pair_pattern") return bind_names(ts::field(target, "value"), s, nullptr);
    if (t == "assignment_pattern" || t == "object_assignment_pattern") {
      return bind_names(ts::field(target, "left"), s, nullptr);
    }
    ts::for_named(target, [&](TSNode c) { bind_names(c, s, nullptr); });
  }

  void on_import(TSNode n) {
    const auto spec = string_from(ts::field(n, "source"));
    if (!spec) return;
    m_.imports.push_back({*spec, ts::line(n)});
    ts::for_named(n, [&](TSNode c) {
      if (ts::is(c, "import_require_clause")) {  // TypeScript: import x = require("y")
        ts::for_named(c, [&](TSNode r) {
          if (ts::is(r, "identifier")) def(0).bindings[text(r)] = Binding{true, *spec, ""};
        });
      }
      if (!ts::is(c, "import_clause")) return;
      ts::for_named(c, [&](TSNode k) {
        if (ts::is(k, "identifier")) {
          def(0).bindings[text(k)] = Binding{false, *spec, "default"};
        } else if (ts::is(k, "namespace_import")) {
          ts::for_named(k, [&](TSNode id) {
            if (ts::is(id, "identifier")) def(0).bindings[text(id)] = Binding{true, *spec, ""};
          });
        } else if (ts::is(k, "named_imports")) {
          ts::for_named(k, [&](TSNode sp) {
            if (!ts::is(sp, "import_specifier")) return;
            const std::string name = unquote(text(ts::field(sp, "name")));
            const TSNode alias = ts::field(sp, "alias");
            def(0).bindings[ts_node_is_null(alias) ? name : text(alias)] = Binding{false, *spec, name};
          });
        }
      });
    });
  }

  static std::string unquote(std::string s) {
    if (s.size() >= 2 && (s[0] == '"' || s[0] == '\'')) return s.substr(1, s.size() - 2);
    return s;
  }

  std::optional<std::string> string_from(TSNode n) {
    if (ts_node_is_null(n)) return std::nullopt;
    const Expr e = to_expr(n);
    if (e.kind != Expr::Kind::Str) return std::nullopt;
    std::string out;
    for (const auto& [hole, piece] : e.parts) out += hole ? "{}" : piece;
    return out;
  }

  void on_export(TSNode n, int s, int a) {
    bool is_default = false;
    const uint32_t count = ts_node_child_count(n);
    for (uint32_t i = 0; i < count; ++i) is_default = is_default || ts::is(ts_node_child(n, i), "default");
    const auto spec = string_from(ts::field(n, "source"));
    if (spec) m_.imports.push_back({*spec, ts::line(n)});

    const TSNode decl = ts::field(n, "declaration");
    if (!ts_node_is_null(decl)) {
      walk(decl, s, a);
      const std::string_view dt = ts_node_type(decl);
      if (dt == "function_declaration" || dt == "generator_function_declaration" || dt == "class_declaration" ||
          dt == "abstract_class_declaration") {
        const std::string name = text(ts::field(decl, "name"));
        m_.exports[is_default ? "default" : name] = {Export::Kind::Local, name, ""};
      } else if (dt == "lexical_declaration" || dt == "variable_declaration") {
        ts::for_named(decl, [&](TSNode d) {
          if (ts::is(d, "variable_declarator") && ts::is(ts::field(d, "name"), "identifier")) {
            const std::string name = text(ts::field(d, "name"));
            m_.exports[name] = {Export::Kind::Local, name, ""};
          }
        });
      }
      return;
    }
    const TSNode value = ts::field(n, "value");
    if (is_default && !ts_node_is_null(value)) {
      const std::string_view vt = ts_node_type(value);
      if (vt == "identifier") {
        m_.exports["default"] = {Export::Kind::Local, text(value), ""};
      } else if (vt == "arrow_function" || vt == "function_expression" || vt == "function") {
        on_function(value, "default", s, vt == "arrow_function");
        m_.exports["default"] = {Export::Kind::Local, "default", ""};
      } else if (vt == "class") {
        on_class(value, "default", s, a);
        m_.exports["default"] = {Export::Kind::Local, "default", ""};
      } else {
        walk(value, s, a);
      }
      return;
    }
    bool any = false;
    ts::for_named(n, [&](TSNode c) {
      if (ts::is(c, "export_clause")) {
        any = true;
        ts::for_named(c, [&](TSNode sp) {
          if (!ts::is(sp, "export_specifier")) return;
          const std::string name = unquote(text(ts::field(sp, "name")));
          const TSNode alias = ts::field(sp, "alias");
          const std::string as = ts_node_is_null(alias) ? name : unquote(text(alias));
          m_.exports[as] = spec ? Export{Export::Kind::Reexport, name, *spec} : Export{Export::Kind::Local, name, ""};
        });
      } else if (ts::is(c, "namespace_export") && spec) {
        any = true;
        ts::for_named(c, [&](TSNode id) { m_.exports[text(id)] = {Export::Kind::Namespace, "", *spec}; });
      }
    });
    if (!any && spec) m_.star_exports.push_back(*spec);  // export * from "x"
  }

  // The enclosing class for `this`: through arrow functions, stopping at plain functions.
  int this_class(int s) {
    while (s > 0 && def(s).kind == DefKind::Function) {
      const int p = def(s).parent;
      if (def(p).kind == DefKind::Class) return p;
      if (!def(s).arrow) return -1;
      s = p;
    }
    return -1;
  }

  void on_assignment(TSNode n, int s, int a) {
    const TSNode left = ts::field(n, "left");
    const TSNode right = ts::field(n, "right");
    if (ts::is(left, "member_expression")) {
      const std::string obj = text(ts::field(left, "object"));
      const std::string prop = text(ts::field(left, "property"));
      const std::string_view rt = ts_node_is_null(right) ? "" : ts_node_type(right);
      const bool fn = rt == "arrow_function" || rt == "function_expression" || rt == "function";
      if (obj == "module" && prop == "exports") {  // module.exports = ...
        if (rt == "object") {
          ts::for_named(right, [&](TSNode p) {
            if (ts::is(p, "shorthand_property_identifier")) m_.exports[text(p)] = {Export::Kind::Local, text(p), ""};
            else if (ts::is(p, "pair") && ts::is(ts::field(p, "value"), "identifier")) {
              m_.exports[unquote(text(ts::field(p, "key")))] = {Export::Kind::Local, text(ts::field(p, "value")), ""};
            }
          });
        } else if (rt == "identifier") {
          m_.exports["default"] = {Export::Kind::Local, text(right), ""};
        } else if (fn) {
          // module.exports = function audit() {}: keep the function's own name if it has one.
          std::string name = text(ts::field(right, "name"));
          if (name.empty()) name = "default";
          on_function(right, name, s, rt == "arrow_function");
          m_.exports["default"] = {Export::Kind::Local, name, ""};
          return;
        }
      } else if (obj == "exports" || obj == "module.exports") {  // exports.name = ...
        if (fn) {
          on_function(right, prop, s, rt == "arrow_function");
          m_.exports[prop] = {Export::Kind::Local, prop, ""};
          return;
        }
        if (rt == "identifier") m_.exports[prop] = {Export::Kind::Local, text(right), ""};
      } else if (obj == "this" && !ts_node_is_null(right)) {  // this.x = ...
        const int cls = this_class(s);
        if (cls >= 0) {
          auto& vals = def(cls).attr_values[prop];
          if (vals.size() < 4) vals.emplace_back(s, to_expr(right));
        }
      }
    } else if (ts::is(left, "identifier") && !ts_node_is_null(right)) {
      // Reassignment: update the value where the name is declared.
      const std::string name = text(left);
      for (int d = s; d >= 0; d = def(d).parent) {
        if (def(d).locals.count(name)) {
          def(d).values[name] = to_expr(right);
          break;
        }
      }
    }
    walk(left, s, a);
    if (!ts_node_is_null(right)) walk(right, s, a);
  }

  void on_call(TSNode n, int s, int a, bool is_new) {
    CallSite cs;
    cs.scope = s;
    cs.attr = a;
    cs.is_new = is_new;
    cs.line = ts::line(n);
    const TSNode f = ts::field(n, is_new ? "constructor" : "function");
    cs.callee = to_expr(f);
    if (!is_new && (ts::is(f, "import") || ts::text(f, src_) == "require")) {
      if (auto spec = string_arg(n)) m_.imports.push_back({*spec, cs.line});
      return;
    }
    const TSNode args = ts::field(n, "arguments");
    if (ts::is(args, "arguments")) {
      int i = 0;
      ts::for_named(args, [&](TSNode arg) {
        if (ts::is(arg, "comment")) return;
        if (i == 0) cs.first_arg = to_expr(arg);
        if (i == 1 && ts::is(arg, "object")) {
          ts::for_named(arg, [&](TSNode p) {
            if (ts::is(p, "pair") && unquote(text(ts::field(p, "key"))) == "method") {
              if (auto m = string_from(ts::field(p, "value"))) cs.method_option = *m;
            }
          });
        }
        ++i;
      });
    } else if (ts::is(args, "template_string")) {  // tagged template: sql`SELECT ...`
      cs.first_arg = to_expr(args);
    }
    m_.calls.push_back(std::move(cs));
  }

  std::optional<Expr> type_ref(TSNode annotation) {
    if (ts_node_is_null(annotation)) return std::nullopt;
    TSNode t = ts::is(annotation, "type_annotation") ? ts_node_named_child(annotation, 0) : annotation;
    if (ts::is(t, "generic_type")) t = ts::field(t, "name");
    Expr name;
    if (ts::is(t, "type_identifier")) {
      name.kind = Expr::Kind::Name;
      name.text = text(t);
    } else if (ts::is(t, "nested_type_identifier")) {  // ns.Type
      name.kind = Expr::Kind::Member;
      name.text = text(ts::field(t, "name"));
      Expr obj;
      obj.kind = Expr::Kind::Name;
      obj.text = text(ts::field(t, "module"));
      name.sub.push_back(std::move(obj));
    } else {
      return std::nullopt;
    }
    Expr e;
    e.kind = Expr::Kind::TypeRef;
    e.sub.push_back(std::move(name));
    return e;
  }

  Expr to_expr(TSNode n) {
    Expr e;
    if (ts_node_is_null(n)) return e;
    const std::string_view t = ts_node_type(n);
    if (t == "identifier" || t == "shorthand_property_identifier") {
      e.kind = Expr::Kind::Name;
      e.text = text(n);
    } else if (t == "this") {
      e.kind = Expr::Kind::This;
    } else if (t == "super") {
      e.kind = Expr::Kind::Super;
    } else if (t == "member_expression") {
      e.kind = Expr::Kind::Member;
      e.text = text(ts::field(n, "property"));
      e.sub.push_back(to_expr(ts::field(n, "object")));
    } else if (t == "call_expression") {
      if (ts::text(ts::field(n, "function"), src_) == "require") {
        if (auto spec = string_arg(n)) {
          e.kind = Expr::Kind::Require;
          e.text = *spec;
          return e;
        }
      }
      e.kind = Expr::Kind::Call;
      e.sub.push_back(to_expr(ts::field(n, "function")));
    } else if (t == "new_expression") {
      e.kind = Expr::Kind::New;
      e.sub.push_back(to_expr(ts::field(n, "constructor")));
    } else if (t == "string" || t == "template_string") {
      e.kind = Expr::Kind::Str;
      ts::for_named(n, [&](TSNode c) {
        if (ts::is(c, "string_fragment") || ts::is(c, "escape_sequence")) e.parts.emplace_back(false, text(c));
        else if (ts::is(c, "template_substitution")) {
          const TSNode inner = ts_node_named_child(c, 0);
          e.parts.emplace_back(true, ts::is(inner, "identifier") ? text(inner) : "");
        }
      });
    } else if (t == "parenthesized_expression" || t == "await_expression" || t == "non_null_expression" ||
               t == "as_expression" || t == "satisfies_expression") {
      if (ts_node_named_child_count(n) >= 1) return to_expr(ts_node_named_child(n, 0));
    }
    return e;
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

const std::unordered_set<std::string_view>& node_builtins() {
  static const std::unordered_set<std::string_view> names = {
#include "node_builtins.inc"
  };
  return names;
}

bool is_builtin_method(const std::string& name) {
  static const std::unordered_set<std::string_view> names = {
#include "builtin_methods.inc"
  };
  return names.count(name) > 0;
}

struct Value {
  // Declared: the type from an annotation, so subclasses may be what actually runs.
  enum class Kind : std::uint8_t { Unknown, Module, Def, Instance, This, Declared, Super, ThirdParty };
  Kind kind = Kind::Unknown;
  DefRef ref;
  std::string path;  // ThirdParty: package and member path; "()" marks a call result
};

struct NameHit {
  enum class Kind : std::uint8_t { None, Value, Local } kind = Kind::None;
  Value value;
  int scope = 0;
};

struct SpecTarget {
  int mod = -1;
  std::string third_party;  // package name
  bool builtin = false;
};

class Project {
 public:
  std::vector<ModuleInfo> mods;

  void index() {
    for (int i = 0; i < static_cast<int>(mods.size()); ++i) by_path_.emplace(mods[std::size_t(i)].path, i);
    for (int m = 0; m < static_cast<int>(mods.size()); ++m) {
      const auto& defs = mods[std::size_t(m)].defs;
      for (int d = 1; d < static_cast<int>(defs.size()); ++d) {
        const Def& df = defs[std::size_t(d)];
        if (df.kind == DefKind::Function && !df.anonymous && defs[std::size_t(df.parent)].kind == DefKind::Class) {
          methods_by_name_[df.name].push_back({m, d});
        }
      }
    }
    for (int m = 0; m < static_cast<int>(mods.size()); ++m) {
      const auto& defs = mods[std::size_t(m)].defs;
      for (int d = 1; d < static_cast<int>(defs.size()); ++d) {
        const Def& df = defs[std::size_t(d)];
        if (df.kind != DefKind::Class || !df.base) continue;
        const Value v = callable(resolve_expr(m, named_scope(m, df.parent), *df.base, 0));
        if (v.kind == Value::Kind::Def && def(v.ref).kind == DefKind::Class && !(v.ref == DefRef{m, d})) {
          bases_[{m, d}] = v.ref;
          subclasses_[v.ref].push_back({m, d});
        }
      }
    }
  }

  const Def& def(DefRef r) const { return mods[std::size_t(r.mod)].defs[std::size_t(r.def)]; }

  int named_scope(int m, int s) const {
    while (s > 0 && mods[std::size_t(m)].defs[std::size_t(s)].anonymous) s = mods[std::size_t(m)].defs[std::size_t(s)].parent;
    return s;
  }

  std::string id(DefRef r) const {
    const ModuleInfo& m = mods[std::size_t(r.mod)];
    const Def& d = def(r);
    return "js:" + m.path + (d.qual.empty() ? "" : "#" + d.qual);
  }

  SpecTarget resolve_spec(int from, const std::string& spec_in) const {
    SpecTarget t;
    std::string spec = spec_in;
    if (spec.starts_with("node:")) {
      t.builtin = true;
      return t;
    }
    if (spec.starts_with("./") || spec.starts_with("../") || spec == "." || spec == "..") {
      const fs::path dir = fs::path(mods[std::size_t(from)].path).parent_path();
      t.mod = find_file((dir / spec).lexically_normal().generic_string());
      return t;
    }
    if (spec.starts_with("/")) return t;
    const std::string top = spec.substr(0, spec.find('/'));
    if (node_builtins().count(top)) {
      t.builtin = true;
      return t;
    }
    // Bare specifiers that name a project path (baseUrl-style imports) resolve to it.
    t.mod = find_file(fs::path(spec).lexically_normal().generic_string());
    if (t.mod >= 0) return t;
    if (spec.starts_with("@")) {
      const auto second = spec.find('/', spec.find('/') + 1);
      t.third_party = spec.substr(0, second);
    } else {
      t.third_party = top;
    }
    return t;
  }

  // ---- name resolution ----

  Value resolve_export(int m, const std::string& name, int depth) const {
    if (depth > 12 || m < 0) return {};
    const ModuleInfo& mod = mods[std::size_t(m)];
    if (auto it = mod.exports.find(name); it != mod.exports.end()) {
      const Export& ex = it->second;
      if (ex.kind == Export::Kind::Local) {
        const NameHit hit = resolve_name(m, 0, ex.name, depth + 1);
        return hit.kind == NameHit::Kind::Value ? hit.value : Value{};
      }
      const SpecTarget t = resolve_spec(m, ex.spec);
      if (ex.kind == Export::Kind::Namespace) {
        return t.mod >= 0 ? module_value(t.mod) : t.third_party.empty() ? Value{} : third_party(t.third_party);
      }
      if (t.mod >= 0) return resolve_export(t.mod, ex.name, depth + 1);
      return t.third_party.empty() ? Value{} : third_party(t.third_party + "." + ex.name);
    }
    for (const std::string& star : mod.star_exports) {
      const SpecTarget t = resolve_spec(m, star);
      if (t.mod < 0 || t.mod == m) continue;
      Value v = resolve_export(t.mod, name, depth + 1);
      if (v.kind != Value::Kind::Unknown) return v;
    }
    // Lenient: a top-level definition by that name, even if the export wasn't recognised.
    if (auto it = mod.defs[0].children.find(name); it != mod.defs[0].children.end()) return def_value({m, it->second});
    // CommonJS interop: the default import of a module without one is the whole module.
    if (name == "default" && depth > 0) return module_value(m);
    return {};
  }

  Value resolve_binding(int m, const Binding& b, int depth) const {
    const SpecTarget t = resolve_spec(m, b.spec);
    if (t.builtin) return {};
    if (t.mod < 0) {
      if (t.third_party.empty()) return {};
      return third_party(b.ns || b.name == "default" ? t.third_party : t.third_party + "." + b.name);
    }
    if (b.ns) return module_value(t.mod);
    return resolve_export(t.mod, b.name, depth + 1);
  }

  NameHit resolve_name(int m, int scope, const std::string& name, int depth) const {
    const auto& defs = mods[std::size_t(m)].defs;
    for (int s = scope; s >= 0; s = defs[std::size_t(s)].parent) {
      const Def& d = defs[std::size_t(s)];
      if (d.kind == DefKind::Class) continue;
      if (auto it = d.children.find(name); it != d.children.end()) {
        return {NameHit::Kind::Value, def_value({m, it->second}), s};
      }
      if (auto it = d.bindings.find(name); it != d.bindings.end()) {
        return {NameHit::Kind::Value, resolve_binding(m, it->second, depth + 1), s};
      }
      if (d.locals.count(name)) return {NameHit::Kind::Local, {}, s};
    }
    return {};
  }

  // `this` in scope s: the class of the nearest method, through arrow functions.
  std::optional<DefRef> this_class(int m, int s) const {
    const auto& defs = mods[std::size_t(m)].defs;
    while (s > 0 && defs[std::size_t(s)].kind == DefKind::Function) {
      const int p = defs[std::size_t(s)].parent;
      if (defs[std::size_t(p)].kind == DefKind::Class) return DefRef{m, p};
      if (!defs[std::size_t(s)].arrow) return std::nullopt;
      s = p;
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
        if (auto it = d.values.find(e.text); it != d.values.end()) return resolve_expr(m, hit.scope, it->second, depth + 1);
        return {};
      }
      case Expr::Kind::This:
        if (auto cls = this_class(m, scope)) return {Value::Kind::This, *cls, {}};
        return {};
      case Expr::Kind::Super:
        if (auto cls = this_class(m, scope)) return {Value::Kind::Super, *cls, {}};
        return {};
      case Expr::Kind::Member:
        return member_of(resolve_expr(m, scope, e.sub[0], depth + 1), e.text, depth);
      case Expr::Kind::Require: {
        const SpecTarget t = resolve_spec(m, e.text);
        if (t.mod >= 0) return module_value(t.mod);
        return t.third_party.empty() ? Value{} : third_party(t.third_party);
      }
      case Expr::Kind::New: {
        const Value c = callable(resolve_expr(m, scope, e.sub[0], depth + 1), depth);
        if (c.kind == Value::Kind::Def && def(c.ref).kind == DefKind::Class) return {Value::Kind::Instance, c.ref, {}};
        if (c.kind == Value::Kind::ThirdParty) return third_party(c.path + "()");
        return {};
      }
      case Expr::Kind::Call: {
        const Value f = callable(resolve_expr(m, scope, e.sub[0], depth + 1), depth);
        if (f.kind == Value::Kind::ThirdParty) return third_party(f.path + "()");
        return {};
      }
      case Expr::Kind::TypeRef: {
        const Value t = resolve_expr(m, scope, e.sub[0], depth + 1);
        if (t.kind == Value::Kind::Def && def(t.ref).kind == DefKind::Class) return {Value::Kind::Declared, t.ref, {}};
        return {};
      }
      default:
        return {};
    }
  }

  // Calling or constructing a module means its default export (module.exports = X).
  Value callable(Value v, int depth = 0) const {
    if (v.kind == Value::Kind::Module) return resolve_export(v.ref.mod, "default", depth + 1);
    return v;
  }

  Value member_of(const Value& obj, const std::string& name, int depth) const {
    switch (obj.kind) {
      case Value::Kind::Module:
        return resolve_export(obj.ref.mod, name, depth + 1);
      case Value::Kind::Def:
        if (def(obj.ref).kind != DefKind::Class) return {};
        [[fallthrough]];
      case Value::Kind::Instance:
      case Value::Kind::This:
      case Value::Kind::Declared:
        if (name == "constructor" && obj.kind != Value::Kind::Def) return def_value(obj.ref);  // this.constructor.m()
        if (auto mth = lookup_method(obj.ref, name)) return def_value(*mth);
        return lookup_attr(obj.ref, name, depth + 1);
      case Value::Kind::Super:
        if (auto b = base_of(obj.ref)) {
          if (auto mth = lookup_method(*b, name)) return def_value(*mth);
        }
        return {};
      case Value::Kind::ThirdParty:
        return third_party(obj.path + "." + name);
      default:
        return {};
    }
  }

  std::optional<DefRef> lookup_method(DefRef cls, const std::string& name, int depth = 0) const {
    if (depth > 32) return std::nullopt;
    const Def& c = def(cls);
    if (auto it = c.children.find(name); it != c.children.end()) return DefRef{cls.mod, it->second};
    if (auto b = base_of(cls)) return lookup_method(*b, name, depth + 1);
    return std::nullopt;
  }

  Value lookup_attr(DefRef cls, const std::string& name, int depth) const {
    if (depth > 10) return {};
    const Def& c = def(cls);
    if (auto it = c.attr_values.find(name); it != c.attr_values.end()) {
      for (const auto& [scope, expr] : it->second) {
        Value v = resolve_expr(cls.mod, scope, expr, depth + 1);
        if (v.kind != Value::Kind::Unknown) return v;
      }
    }
    if (auto b = base_of(cls)) return lookup_attr(*b, name, depth + 1);
    return {};
  }

  std::optional<DefRef> base_of(DefRef cls) const {
    auto it = bases_.find(cls);
    if (it == bases_.end()) return std::nullopt;
    return it->second;
  }

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
      if (auto it = d.bindings.find(e.text); it != d.bindings.end() && !it->second.ns) {
        const SpecTarget t = resolve_spec(m, it->second.spec);
        if (t.mod < 0) return std::nullopt;
        const auto ex = mods[std::size_t(t.mod)].exports.find(it->second.name);
        const std::string local = ex != mods[std::size_t(t.mod)].exports.end() &&
                                          ex->second.kind == Export::Kind::Local
                                      ? ex->second.name
                                      : it->second.name;
        return eval_str(t.mod, 0, Expr{Expr::Kind::Name, local, {}, {}}, depth + 1);
      }
    }
    return std::nullopt;
  }

 private:
  std::unordered_map<std::string, int> by_path_;
  std::unordered_map<DefRef, DefRef, DefRefHash> bases_;
  std::unordered_map<DefRef, std::vector<DefRef>, DefRefHash> subclasses_;
  std::unordered_map<std::string, std::vector<DefRef>> methods_by_name_;

  static Value module_value(int i) { return {Value::Kind::Module, {i, 0}, {}}; }
  static Value def_value(DefRef r) { return {Value::Kind::Def, r, {}}; }
  static Value third_party(std::string path) { return {Value::Kind::ThirdParty, {}, std::move(path)}; }

  // Node / bundler resolution: the path itself, with an extension, or its index file. A ".js"
  // import may name a ".ts" source (TypeScript's ESM convention).
  int find_file(const std::string& p) const {
    static const char* kExts[] = {".ts", ".tsx", ".js", ".jsx", ".mjs", ".cjs", ".mts", ".cts"};
    auto at = [&](const std::string& q) -> int {
      auto it = by_path_.find(q);
      return it == by_path_.end() ? -1 : it->second;
    };
    if (int i = at(p); i >= 0) return i;
    for (const char* ext : kExts) {
      if (int i = at(p + ext); i >= 0) return i;
    }
    for (const char* js : {".js", ".jsx", ".mjs", ".cjs"}) {
      if (p.ends_with(js)) {
        const std::string stem = p.substr(0, p.size() - std::string_view(js).size());
        for (const char* ext : {".ts", ".tsx", ".mts", ".cts"}) {
          if (int i = at(stem + ext); i >= 0) return i;
        }
      }
    }
    for (const char* ext : kExts) {
      if (int i = at(p + "/index" + ext); i >= 0) return i;
    }
    return -1;
  }
};

// ===================================================================== pass 2

struct EdgeOut {
  std::string from, to;
  EdgeKind kind = EdgeKind::Calls;
  Confidence confidence = Confidence::Exact;
  std::uint32_t line = 0;
  nlohmann::json attrs = nlohmann::json::object();
  std::optional<Node> new_node;
};

class Linker {
 public:
  Linker(const Project& p, int m) : p_(p), m_(m), mod_(p.mods[std::size_t(m)]) {}

  std::vector<EdgeOut> run() {
    for (int d = 1; d < static_cast<int>(mod_.defs.size()); ++d) {
      const Def& df = mod_.defs[std::size_t(d)];
      if (df.anonymous) continue;
      emit(id(p_.named_scope(m_, df.parent)), id(d), EdgeKind::Defines, Confidence::Exact, df.line);
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
    const SpecTarget t = p_.resolve_spec(m_, imp.spec);
    if (t.mod >= 0) {
      if (t.mod != m_) emit(id(0), p_.id({t.mod, 0}), EdgeKind::Imports, Confidence::Exact, imp.line);
      return;
    }
    if (t.third_party.empty()) return;
    Node n;
    n.id = "js:" + t.third_party;
    n.kind = NodeKind::Module;
    n.name = t.third_party;
    n.frontend = "js";
    n.attrs = {{"third_party", true}};
    std::string to = n.id;
    emit(id(0), std::move(to), EdgeKind::Imports, Confidence::Exact, imp.line, nlohmann::json::object(), std::move(n));
  }

  void call_edge(const CallSite& cs, DefRef target, Confidence c, const char* via = nullptr) {
    const Def& d = p_.def(target);
    if (d.anonymous || (d.kind != DefKind::Function && d.kind != DefKind::Class)) return;
    nlohmann::json attrs = nlohmann::json::object();
    if (via) attrs["via"] = via;
    emit(id(cs.attr), p_.id(target), EdgeKind::Calls, c, cs.line, std::move(attrs));
  }

  void method_calls(const CallSite& cs, const Value& obj, const std::string& name) {
    switch (obj.kind) {
      case Value::Kind::This:
      case Value::Kind::Declared: {
        const bool declared = obj.kind == Value::Kind::Declared;
        if (auto mth = p_.lookup_method(obj.ref, name)) {
          call_edge(cs, *mth, declared ? Confidence::Inferred : Confidence::Exact, declared ? "type_annotation" : nullptr);
        } else if (const Value attr = p_.lookup_attr(obj.ref, name, 0); attr.kind == Value::Kind::Def) {
          call_edge(cs, attr.ref, Confidence::Inferred, "local_type");
        }
        for (const DefRef& sub : p_.descendants(obj.ref)) {
          const Def& sd = p_.def(sub);
          if (auto it = sd.children.find(name); it != sd.children.end()) {
            call_edge(cs, {sub.mod, it->second}, Confidence::Inferred, "override");
          }
        }
        return;
      }
      case Value::Kind::Instance:
        if (auto mth = p_.lookup_method(obj.ref, name)) {
          if (cs.callee.sub[0].kind == Expr::Kind::New) call_edge(cs, *mth, Confidence::Exact);
          else call_edge(cs, *mth, Confidence::Inferred, "local_type");
        }
        return;
      case Value::Kind::Unknown:
        if (!name.starts_with("#") && !is_builtin_method(name)) {
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

  void on_call(const CallSite& cs) {
    const Expr& callee = cs.callee;
    if (cs.is_new) {
      const Value v = p_.callable(p_.resolve_expr(m_, cs.scope, callee, 0));
      if (v.kind == Value::Kind::Def) call_edge(cs, v.ref, Confidence::Exact);
      return;
    }
    if (callee.kind == Expr::Kind::Member && try_sql(cs)) return;
    if (callee.kind == Expr::Kind::Super) {  // super(...) runs the parent's constructor
      if (auto cls = p_.this_class(m_, cs.scope)) {
        if (auto base = p_.base_of(*cls)) {
          if (auto ctor = p_.lookup_method(*base, "constructor")) call_edge(cs, *ctor, Confidence::Exact);
        }
      }
      return;
    }
    if (callee.kind == Expr::Kind::Member) {
      return method_calls(cs, p_.resolve_expr(m_, cs.scope, callee.sub[0], 0), callee.text);
    }
    if (callee.kind == Expr::Kind::Name) {
      const NameHit hit = p_.resolve_name(m_, cs.scope, callee.text, 0);
      if (hit.kind == NameHit::Kind::None && callee.text == "fetch") return http_edge(cs, "GET", true);
    }
    const Value v = p_.callable(p_.resolve_expr(m_, cs.scope, callee, 0));
    if (v.kind == Value::Kind::Def) call_edge(cs, v.ref, Confidence::Exact);
    else if (v.kind == Value::Kind::ThirdParty) try_http(cs, v.path);
  }

  bool try_sql(const CallSite& cs) {
    static const std::unordered_set<std::string> kSql = {"query", "execute", "raw", "prepare", "run", "all", "exec", "unsafe"};
    if (!kSql.count(cs.callee.text) || !cs.first_arg) return false;
    const auto sql = p_.eval_str(m_, cs.scope, *cs.first_arg, 0);
    if (!sql) return false;
    const SqlTables t = sql_tables(*sql);
    if (t.reads.empty() && t.writes.empty()) return false;
    auto store = [&](const std::string& table, EdgeKind kind) {
      Node n;
      n.id = sql_store_id(table);
      n.kind = NodeKind::Store;
      n.name = n.id.substr(4);
      n.frontend = "js";
      std::string to = n.id;
      emit(id(cs.attr), std::move(to), kind, Confidence::Exact, cs.line, nlohmann::json::object(), std::move(n));
    };
    for (const auto& r : t.reads) store(r, EdgeKind::Reads);
    for (const auto& w : t.writes) store(w, EdgeKind::Writes);
    return true;
  }

  void try_http(const CallSite& cs, const std::string& path) {
    if (path == "node-fetch" || path == "cross-fetch" || path == "undici.fetch" || path == "node-fetch.default") {
      return http_edge(cs, "GET", true);
    }
    const auto dot = path.find('.');
    if (dot == std::string::npos || path.substr(0, dot) != "axios") return;
    const std::string method = path.substr(dot + 1);
    static const std::unordered_set<std::string> kMethods = {"get", "head", "options", "post", "put", "patch", "delete"};
    if (!kMethods.count(method)) return;
    std::string upper = method;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    http_edge(cs, upper, false);
  }

  void http_edge(const CallSite& cs, std::string method, bool fetch_style) {
    if (fetch_style && !cs.method_option.empty()) {
      method = cs.method_option;
      for (char& c : method) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (!cs.first_arg) return;
    const auto url = p_.eval_str(m_, cs.scope, *cs.first_arg, 0);
    if (!url) return;
    const std::string node_id = http_node_id(*url);
    if (node_id.empty()) return;
    const Confidence c = node_id.find("{}") == std::string::npos ? Confidence::Exact : Confidence::Inferred;
    Node n;
    n.id = node_id;
    n.kind = NodeKind::External;
    n.name = node_id.substr(5);
    n.frontend = "js";
    n.confidence = c;
    const bool reads = method == "GET" || method == "HEAD" || method == "OPTIONS";
    emit(id(cs.attr), node_id, reads ? EdgeKind::Reads : EdgeKind::SendsTo, c, cs.line, {{"method", method}}, std::move(n));
  }
};

const TSLanguage* language_for(const fs::path& p) {
  const std::string ext = p.extension().string();
  if (ext == ".ts" || ext == ".mts" || ext == ".cts") return tree_sitter_typescript();
  if (ext == ".tsx") return tree_sitter_tsx();
  return tree_sitter_javascript();
}

}  // namespace

bool is_js_source(const fs::path& rel) {
  const std::string name = rel.filename().string();
  if (name.ends_with(".d.ts") || name.ends_with(".min.js")) return false;
  const std::string ext = rel.extension().string();
  return ext == ".js" || ext == ".jsx" || ext == ".mjs" || ext == ".cjs" || ext == ".ts" || ext == ".tsx" ||
         ext == ".mts" || ext == ".cts";
}

Graph extract_js_files(const fs::path& root, const std::vector<fs::path>& rel_files, JsStats* stats, unsigned threads) {
  std::vector<fs::path> files;
  for (const auto& f : rel_files) {
    if (is_js_source(f)) files.push_back(f);
  }
  std::sort(files.begin(), files.end());

  Project project;
  project.mods.resize(files.size());
  parallel_for(files.size(), threads, [&](std::size_t i) {
    // One parser per thread and grammar.
    thread_local std::unordered_map<const TSLanguage*, ts::ParserPtr> parsers;
    const TSLanguage* lang = language_for(files[i]);
    ts::ParserPtr& parser = parsers[lang];
    if (!parser) {
      parser.reset(ts_parser_new());
      ts_parser_set_language(parser.get(), lang);
    }
    ModuleInfo& m = project.mods[i];
    m.path = files[i].generic_string();
    const std::string src = read_file(root / files[i]);
    m.lines = static_cast<std::size_t>(std::count(src.begin(), src.end(), '\n'));
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
  for (std::size_t mi = 0; mi < project.mods.size(); ++mi) {
    const ModuleInfo& m = project.mods[mi];
    for (std::size_t d = 0; d < m.defs.size(); ++d) {
      const Def& df = m.defs[d];
      if (df.anonymous) continue;
      Node n;
      n.id = project.id({static_cast<int>(mi), static_cast<int>(d)});
      n.kind = d == 0 ? NodeKind::Module : df.kind == DefKind::Class ? NodeKind::DataType : NodeKind::Function;
      n.name = d == 0 ? fs::path(m.path).filename().string() : df.name;
      n.loc = SourceLoc{m.path, df.line, df.end_line, std::nullopt};
      n.frontend = "js";
      if (!df.accessor.empty()) n.attrs = {{"accessor", df.accessor}};
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
      e.frontend = "js";
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
