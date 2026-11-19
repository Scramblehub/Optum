#pragma once
// The C preprocessor's conditionals, without running it. Internal to the C frontend.
//
// scan() reads a file's #if structure line by line. eval() / holds() evaluate a `#if`
// expression with three-valued logic: a name the caller can't decide makes the parts that depend
// on it unknown, while `0 && x` and `1 || x` still decide.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace optum::c_pp {

enum class Tri : std::uint8_t { False, True, Unknown };

struct Env {
  // `defined(name)`.
  std::function<Tri(const std::string& name)> defined;
  // The value of `name` where it appears in the expression: nullopt if unknown. A name that isn't
  // a macro is 0, as in C; the callback decides that.
  std::function<std::optional<long long>(const std::string& name)> value;
};

// The value of the expression, nullopt if it can't be decided (or doesn't parse).
std::optional<long long> eval(std::string_view expr, const Env& env);

// Whether the `#if` holds.
Tri holds(std::string_view expr, const Env& env);

// One #if / #ifdef / #elif / #else branch: taken when `expr` holds and no earlier branch did.
struct Branch {
  std::vector<std::string> prior;  // the earlier branches of the same #if chain
  std::string expr;                // "" for #else; #ifdef X is "defined(X)"
  // `#ifndef X` whose body defines X (include guards, defaults): X counts as not yet defined here.
  std::vector<std::string> self;
};

// The branches around a line, outermost first.
struct Cond {
  std::vector<int> branches;  // indices in Conditions::branches
  std::string text;           // as shown in attrs.condition: "ZLIB_DEBUG", "not (HAVE_MEMCPY)"
};

// A file's #if structure, read line by line from the directives (not from the syntax tree, whose
// error recovery on macro-heavy code can stretch a branch over the rest of the file).
struct Conditions {
  std::vector<Branch> branches;
  std::vector<Cond> conds;
  std::vector<int> line_cond;  // by 1-based line: index in conds, -1 outside any #if

  int at(std::uint32_t line) const { return line < line_cond.size() ? line_cond[line] : -1; }
};

Conditions scan(std::string_view src);

// `defined(name)` / `defined name` in expr, negated with `!` (or not).
bool tests_defined(std::string_view expr, std::string_view name, bool negated);

}  // namespace optum::c_pp
