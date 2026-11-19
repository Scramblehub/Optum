#include "pp_expr.hpp"

#include <cctype>
#include <cstring>
#include <algorithm>
#include <limits>
#include <map>
#include <vector>

namespace optum::c_pp {
namespace {

using V = std::optional<long long>;

struct Tok {
  enum Kind { Ident, Num, Punct, End } kind = End;
  std::string text;
  V num;  // Num: its value (nullopt for character literals and the like)
};

bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::vector<Tok> lex(std::string_view s) {
  std::vector<Tok> out;
  std::size_t i = 0;
  while (i < s.size()) {
    const char c = s[i];
    if (c == '\\' && i + 1 < s.size() && (s[i + 1] == '\n' || s[i + 1] == '\r')) {
      ++i;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
      const std::size_t e = s.find("*/", i + 2);
      i = e == std::string_view::npos ? s.size() : e + 2;
      continue;
    }
    if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') break;
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      std::size_t j = i;
      while (j < s.size() && ident_char(s[j])) ++j;
      out.push_back({Tok::Ident, std::string(s.substr(i, j - i)), std::nullopt});
      i = j;
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c))) {
      std::size_t j = i;
      while (j < s.size() && (ident_char(s[j]) || s[j] == '.')) ++j;
      std::string t(s.substr(i, j - i));
      std::string digits = t;
      while (!digits.empty() && std::strchr("uUlL", digits.back())) digits.pop_back();
      V v;
      try {
        std::size_t used = 0;
        const unsigned long long u = std::stoull(digits, &used, 0);
        if (used == digits.size()) v = static_cast<long long>(u);
      } catch (...) {
      }
      out.push_back({Tok::Num, std::move(t), v});
      i = j;
      continue;
    }
    if (c == '\'') {  // a character literal: its value isn't worth computing
      std::size_t j = i + 1;
      while (j < s.size() && s[j] != '\'') j += s[j] == '\\' ? 2 : 1;
      out.push_back({Tok::Num, std::string(s.substr(i, j + 1 - i)), std::nullopt});
      i = j + 1;
      continue;
    }
    static const char* kTwo[] = {"&&", "||", "==", "!=", "<=", ">=", "<<", ">>"};
    bool two = false;
    for (const char* p : kTwo) {
      if (s.substr(i, 2) == p) {
        out.push_back({Tok::Punct, p, std::nullopt});
        i += 2;
        two = true;
        break;
      }
    }
    if (two) continue;
    out.push_back({Tok::Punct, std::string(1, c), std::nullopt});
    ++i;
  }
  out.push_back({});
  return out;
}

class Parser {
 public:
  Parser(std::vector<Tok> toks, const Env& env) : t_(std::move(toks)), env_(env) {}

  // nullopt with ok() false: the expression didn't parse.
  V parse() {
    V v = ternary();
    if (t_[p_].kind != Tok::End) ok_ = false;
    return v;
  }
  bool ok() const { return ok_; }

 private:
  std::vector<Tok> t_;
  const Env& env_;
  std::size_t p_ = 0;
  bool ok_ = true;
  int depth_ = 0;

  bool at(const char* p) const { return t_[p_].kind == Tok::Punct && t_[p_].text == p; }
  bool eat(const char* p) {
    if (!at(p)) return false;
    ++p_;
    return true;
  }

  static V both(V a, V b, long long (*f)(unsigned long long, unsigned long long)) {
    if (!a || !b) return std::nullopt;
    return f(static_cast<unsigned long long>(*a), static_cast<unsigned long long>(*b));
  }

  V ternary() {
    V c = logical_or();
    if (!eat("?")) return c;
    V a = ternary();
    if (!eat(":")) ok_ = false;
    V b = ternary();
    if (c) return *c ? a : b;
    return a && b && *a == *b ? a : std::nullopt;
  }

  V logical_or() {
    V v = logical_and();
    while (eat("||")) {
      V r = logical_and();
      if ((v && *v) || (r && *r)) v = 1;
      else if (v && r) v = 0;
      else v = std::nullopt;
    }
    return v;
  }

  V logical_and() {
    V v = bit_or();
    while (eat("&&")) {
      V r = bit_or();
      if ((v && !*v) || (r && !*r)) v = 0;
      else if (v && r) v = 1;
      else v = std::nullopt;
    }
    return v;
  }

  V bit_or() {
    V v = bit_xor();
    while (!at("||") && eat("|")) v = both(v, bit_xor(), [](unsigned long long a, unsigned long long b) { return static_cast<long long>(a | b); });
    return v;
  }
  V bit_xor() {
    V v = bit_and();
    while (eat("^")) v = both(v, bit_and(), [](unsigned long long a, unsigned long long b) { return static_cast<long long>(a ^ b); });
    return v;
  }
  V bit_and() {
    V v = equality();
    while (!at("&&") && eat("&")) v = both(v, equality(), [](unsigned long long a, unsigned long long b) { return static_cast<long long>(a & b); });
    return v;
  }

  V equality() {
    V v = relational();
    for (;;) {
      if (eat("==")) {
        V r = relational();
        v = v && r ? V(*v == *r) : std::nullopt;
      } else if (eat("!=")) {
        V r = relational();
        v = v && r ? V(*v != *r) : std::nullopt;
      } else {
        return v;
      }
    }
  }

  V relational() {
    V v = shift();
    for (;;) {
      if (eat("<=")) {
        V r = shift();
        v = v && r ? V(*v <= *r) : std::nullopt;
      } else if (eat(">=")) {
        V r = shift();
        v = v && r ? V(*v >= *r) : std::nullopt;
      } else if (!at("<<") && eat("<")) {
        V r = shift();
        v = v && r ? V(*v < *r) : std::nullopt;
      } else if (!at(">>") && eat(">")) {
        V r = shift();
        v = v && r ? V(*v > *r) : std::nullopt;
      } else {
        return v;
      }
    }
  }

  V shift() {
    V v = additive();
    for (;;) {
      const bool left = at("<<");
      if (!left && !at(">>")) return v;
      ++p_;
      V r = additive();
      if (!v || !r || *r < 0 || *r > 63) v = std::nullopt;
      else if (left) v = static_cast<long long>(static_cast<unsigned long long>(*v) << *r);
      else v = *v >> *r;
    }
  }

  V additive() {
    V v = multiplicative();
    for (;;) {
      if (eat("+")) v = both(v, multiplicative(), [](unsigned long long a, unsigned long long b) { return static_cast<long long>(a + b); });
      else if (eat("-")) v = both(v, multiplicative(), [](unsigned long long a, unsigned long long b) { return static_cast<long long>(a - b); });
      else return v;
    }
  }

  V multiplicative() {
    V v = unary();
    for (;;) {
      if (eat("*")) {
        v = both(v, unary(), [](unsigned long long a, unsigned long long b) { return static_cast<long long>(a * b); });
      } else if (at("/") || at("%")) {
        const bool div = at("/");
        ++p_;
        V r = unary();
        if (!v || !r || *r == 0 || (*r == -1 && *v == std::numeric_limits<long long>::min())) v = std::nullopt;
        else v = div ? *v / *r : *v % *r;
      } else {
        return v;
      }
    }
  }

  V unary() {
    if (++depth_ > 200) {  // absurd nesting: give up rather than recurse
      ok_ = false;
      return std::nullopt;
    }
    struct Leave {
      int& d;
      ~Leave() { --d; }
    } leave{depth_};
    if (eat("!")) {
      V v = unary();
      return v ? V(!*v) : std::nullopt;
    }
    if (eat("~")) {
      V v = unary();
      return v ? V(~*v) : std::nullopt;
    }
    if (eat("-")) {
      V v = unary();
      return v ? V(static_cast<long long>(0ULL - static_cast<unsigned long long>(*v))) : std::nullopt;
    }
    if (eat("+")) return unary();
    return primary();
  }

  V primary() {
    const Tok& t = t_[p_];
    if (eat("(")) {
      V v = ternary();
      if (!eat(")")) ok_ = false;
      return v;
    }
    if (t.kind == Tok::Num) {
      ++p_;
      return t.num;
    }
    if (t.kind != Tok::Ident) {
      ok_ = false;
      return std::nullopt;
    }
    const std::string name = t.text;
    ++p_;
    if (name == "defined") {
      const bool paren = eat("(");
      if (t_[p_].kind != Tok::Ident) {
        ok_ = false;
        return std::nullopt;
      }
      const std::string m = t_[p_++].text;
      if (paren && !eat(")")) ok_ = false;
      const Tri d = env_.defined(m);
      return d == Tri::Unknown ? std::nullopt : V(d == Tri::True);
    }
    if (at("(")) {  // a function-like macro (or __has_include(...)): not evaluated
      int open = 0;
      do {
        if (at("(")) ++open;
        else if (at(")")) --open;
        ++p_;
      } while (open > 0 && t_[p_].kind != Tok::End);
      return std::nullopt;
    }
    return env_.value(name);
  }
};

}  // namespace

std::optional<long long> eval(std::string_view expr, const Env& env) {
  Parser p(lex(expr), env);
  V v = p.parse();
  return p.ok() ? v : std::nullopt;
}

Tri holds(std::string_view expr, const Env& env) {
  const V v = eval(expr, env);
  return !v ? Tri::Unknown : *v ? Tri::True : Tri::False;
}

bool tests_defined(std::string_view expr, std::string_view name, bool negated) {
  std::string e;
  for (char c : expr) {
    if (!std::isspace(static_cast<unsigned char>(c)) && c != '(' && c != ')') e += c;
  }
  const std::string needle = std::string(negated ? "!" : "") + "defined" + std::string(name);
  for (std::size_t at = e.find(needle); at != std::string::npos; at = e.find(needle, at + 1)) {
    const std::size_t end = at + needle.size();
    const bool whole = end == e.size() || !ident_char(e[end]);
    if (whole && (negated || at == 0 || e[at - 1] != '!')) return true;
  }
  return false;
}

namespace {

// Comments out, line continuations joined, whitespace trimmed. `in_comment` carries a block
// comment left open at the end over to the next line.
std::string clean(std::string_view s, bool& in_comment) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (in_comment) {
      if (s.substr(i, 2) == "*/") {
        in_comment = false;
        ++i;
        out += ' ';
      }
      continue;
    }
    if (s.substr(i, 2) == "/*") {
      in_comment = true;
      ++i;
    } else if (s.substr(i, 2) == "//") {
      break;
    } else if (s[i] == '"' || s[i] == '\'') {  // a literal: copied, comment markers in it don't count
      const char q = s[i];
      out += s[i];
      for (++i; i < s.size() && s[i] != q; ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) out += s[i++];
        out += s[i];
      }
      if (i < s.size()) out += s[i];
    } else if (s[i] == '\\' && (i + 1 == s.size() || s[i + 1] == '\n' || s[i + 1] == '\r')) {
      out += ' ';
    } else {
      out += (s[i] == '\n' || s[i] == '\r' || s[i] == '\t') ? ' ' : s[i];
    }
  }
  out.erase(std::unique(out.begin(), out.end(), [](char a, char b) { return a == ' ' && b == ' '; }), out.end());
  const std::size_t a = out.find_first_not_of(' ');
  if (a == std::string::npos) return {};
  return out.substr(a, out.find_last_not_of(' ') - a + 1);
}

std::string shown(std::string s) {
  if (s.size() > 80) s = s.substr(0, 77) + "...";
  return s;
}

}  // namespace

Conditions scan(std::string_view src) {
  Conditions out;
  struct Open {
    int branch;         // the branch we're in
    std::string shown;  // its condition as shown
  };
  std::vector<Open> stack;
  std::vector<std::string> shown_of;  // by branch
  std::map<std::vector<int>, int> interned;
  int cur = -1;
  auto update = [&] {
    if (stack.empty()) {
      cur = -1;
      return;
    }
    std::vector<int> ids;
    for (const Open& o : stack) ids.push_back(o.branch);
    auto [it, fresh] = interned.emplace(ids, static_cast<int>(out.conds.size()));
    if (fresh) {
      Cond c;
      c.branches = ids;
      out.conds.push_back(std::move(c));  // its text once the file is read (below)
    }
    cur = it->second;
  };

  out.line_cond.push_back(-1);  // line 0 doesn't exist
  bool in_comment = false;
  std::size_t pos = 0;
  while (pos < src.size()) {
    // A logical line: physical lines joined by a trailing backslash.
    std::size_t end = pos;
    std::size_t physical = 0;
    for (;;) {
      std::size_t nl = src.find('\n', end);
      if (nl == std::string_view::npos) nl = src.size();
      ++physical;
      std::size_t last = nl;
      while (last > end && (src[last - 1] == '\r' || src[last - 1] == ' ' || src[last - 1] == '\t')) --last;
      end = nl;
      if (nl < src.size() && last > pos && src[last - 1] == '\\') {
        end = nl + 1;
        continue;
      }
      break;
    }
    const std::string_view line = src.substr(pos, end - pos);
    pos = end < src.size() ? end + 1 : src.size();
    for (std::size_t k = 0; k < physical; ++k) out.line_cond.push_back(cur);

    const bool was_comment = in_comment;
    const std::string text = clean(line, in_comment);
    if (was_comment || text.empty() || text[0] != '#') continue;
    std::size_t i = 1;
    while (i < text.size() && text[i] == ' ') ++i;
    std::size_t j = i;
    while (j < text.size() && std::isalpha(static_cast<unsigned char>(text[j]))) ++j;
    const std::string word = text.substr(i, j - i);
    const std::size_t a0 = text.find_first_not_of(' ', j);
    const std::string arg = a0 == std::string::npos ? std::string() : text.substr(a0);
    const std::string name = arg.substr(0, std::min(arg.size(), arg.find_first_of(" ()")));

    if (word == "if" || word == "ifdef" || word == "ifndef") {
      Branch b;
      std::string s;
      if (word == "if") {
        b.expr = arg;
        s = arg;
      } else {
        const bool neg = word == "ifndef";
        b.expr = std::string(neg ? "!defined(" : "defined(") + name + ")";
        s = (neg ? "!" : "") + name;
      }
      out.branches.push_back(std::move(b));
      stack.push_back({static_cast<int>(out.branches.size()) - 1, shown(s)});
      shown_of.push_back(stack.back().shown);
      update();
    } else if (word == "elif" || word == "elifdef" || word == "elifndef" || word == "else") {
      if (stack.empty()) continue;  // unbalanced: ignore
      Open& o = stack.back();
      Branch b;
      b.prior = out.branches[std::size_t(o.branch)].prior;
      b.prior.push_back(out.branches[std::size_t(o.branch)].expr);
      std::string s = "not (" + o.shown + ")";
      if (word == "elif") {
        b.expr = arg;
        s += " && " + shown(arg);
      } else if (word != "else") {
        const bool neg = word == "elifndef";
        b.expr = std::string(neg ? "!defined(" : "defined(") + name + ")";
        s += " && " + shown((neg ? "!" : "") + name);
      }
      out.branches.push_back(std::move(b));
      o.branch = static_cast<int>(out.branches.size()) - 1;
      o.shown = s;
      shown_of.push_back(s);
      update();
    } else if (word == "endif") {
      if (stack.empty()) continue;
      stack.pop_back();
      update();
    } else if (word == "define" && !name.empty()) {
      // `#ifndef X ... #define X`: X counts as undefined while that branch is decided.
      for (const Open& o : stack) {
        Branch& b = out.branches[std::size_t(o.branch)];
        bool self = !b.expr.empty() && tests_defined(b.expr, name, true);
        for (const std::string& p : b.prior) self = self || tests_defined(p, name, false);
        if (self && std::find(b.self.begin(), b.self.end(), name) == b.self.end()) b.self.push_back(name);
      }
    }
  }
  out.line_cond.push_back(cur);  // a last line without a newline
  // Include guards and `#ifndef X / #define X` defaults always hold: not worth showing.
  for (Cond& c : out.conds) {
    for (int id : c.branches) {
      const Branch& b = out.branches[std::size_t(id)];
      const bool guard = b.prior.empty() && b.expr.starts_with("!defined(") &&
                         std::find(b.self.begin(), b.self.end(), b.expr.substr(9, b.expr.size() - 10)) != b.self.end();
      if (!guard) c.text += (c.text.empty() ? "" : " && ") + shown_of[std::size_t(id)];
    }
  }
  return out;
}

}  // namespace optum::c_pp
