#include "optum/sql.hpp"

#include <algorithm>
#include <cctype>

namespace optum {
namespace {

bool ident_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '"' ||
         c == '`' || c == '[' || c == ']';
}

std::vector<std::string> tokenize(std::string_view s) {
  std::vector<std::string> out;
  std::size_t i = 0;
  while (i < s.size()) {
    if (ident_char(s[i])) {
      std::size_t j = i;
      while (j < s.size() && ident_char(s[j])) {
        // A quoted identifier ("My Table", `t`, [t]) runs to its closing quote, spaces included.
        const char close = s[j] == '"' ? '"' : s[j] == '`' ? '`' : s[j] == '[' ? ']' : '\0';
        if (close) {
          const std::size_t end = s.find(close, j + 1);
          j = end == std::string_view::npos ? s.size() : end + 1;
        } else {
          ++j;
        }
      }
      out.emplace_back(s.substr(i, j - i));
      i = j;
    } else if (!std::isspace(static_cast<unsigned char>(s[i]))) {
      out.emplace_back(1, s[i++]);  // punctuation as its own token
    } else {
      ++i;
    }
  }
  return out;
}

std::string upper(const std::string& s) {
  std::string u = s;
  for (char& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return u;
}

// A table name token: starts with a letter or quote, no SQL keyword.
bool is_table(const std::string& t) {
  if (t.empty()) return false;
  const char c = t[0];
  if (!(std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '"' || c == '`' || c == '[')) {
    return false;
  }
  static const char* kKeywords[] = {"SELECT", "WHERE", "SET", "VALUES", "AS", "ON", "IF", "NOT",
                                    "EXISTS", "TABLE", "INTO", "FROM", "JOIN", "LATERAL"};
  const std::string u = upper(t);
  return std::none_of(std::begin(kKeywords), std::end(kKeywords),
                      [&u](const char* k) { return u == k; });
}

std::string clean(std::string t) {
  std::erase_if(t, [](char c) { return c == '"' || c == '`' || c == '[' || c == ']'; });
  return t;
}

void add(std::vector<std::string>& list, const std::string& table) {
  if (std::find(list.begin(), list.end(), table) == list.end()) list.push_back(table);
}

bool is_column(const std::string& t) {
  if (t.empty() || !(std::isalpha(static_cast<unsigned char>(t[0])) || t[0] == '_' || t[0] == '"' || t[0] == '`' || t[0] == '[')) {
    return false;
  }
  static const char* kNot[] = {"PRIMARY", "FOREIGN", "UNIQUE", "CONSTRAINT", "CHECK", "KEY", "INDEX", "DISTINCT",
                               "ALL", "AS", "NULL", "NOT", "DEFAULT", "REFERENCES", "CASE", "WHEN", "THEN", "ELSE",
                               "END", "AND", "OR", "TRUE", "FALSE", "EXCLUDED"};
  const std::string u = upper(t);
  return std::none_of(std::begin(kNot), std::end(kNot), [&u](const char* k) { return u == k; });
}

std::string column_name(std::string t) {  // drop quotes and a table prefix (o.total -> total)
  t = clean(std::move(t));
  if (const auto dot = t.rfind('.'); dot != std::string::npos) t = t.substr(dot + 1);
  return t;
}

// The columns each statement names for its table (see SqlTables::columns).
std::vector<std::pair<std::string, std::vector<std::string>>> sql_columns(
    const std::vector<std::string>& toks, const SqlTables& t,
    std::vector<std::pair<std::string, std::vector<std::string>>>& out_keys) {
  std::vector<std::pair<std::string, std::vector<std::string>>> out;
  auto add_cols = [&](const std::string& table, std::vector<std::string> cols) {
    std::vector<std::string> uniq;
    for (auto& c : cols) {
      if (!c.empty() && c != "*" && std::find(uniq.begin(), uniq.end(), c) == uniq.end()) uniq.push_back(std::move(c));
    }
    if (uniq.empty()) return;
    for (auto& [name, list] : out) {
      if (name == table) {
        for (auto& c : uniq) {
          if (std::find(list.begin(), list.end(), c) == list.end()) list.push_back(c);
        }
        return;
      }
    }
    out.emplace_back(table, std::move(uniq));
  };
  auto paren_end = [&](std::size_t open) {  // index of the ')' closing toks[open]
    int depth = 0;
    for (std::size_t i = open; i < toks.size(); ++i) {
      if (toks[i] == "(") ++depth;
      else if (toks[i] == ")" && --depth == 0) return i;
    }
    return toks.size();
  };
  for (std::size_t i = 0; i < toks.size(); ++i) {
    const std::string u = upper(toks[i]);
    if ((u == "INSERT" || u == "REPLACE") && !t.writes.empty()) {
      std::size_t j = i + 1;
      while (j < toks.size() && toks[j] != "(" && upper(toks[j]) != "VALUES" && upper(toks[j]) != "SELECT") ++j;
      if (j < toks.size() && toks[j] == "(") {
        const std::size_t end = paren_end(j);
        std::vector<std::string> cols;
        for (std::size_t k = j + 1; k < end; ++k) {
          if (toks[k] != "," && is_column(toks[k])) cols.push_back(column_name(toks[k]));
        }
        add_cols(t.writes.front(), std::move(cols));
      }
    } else if (u == "CREATE" && !t.writes.empty()) {
      std::size_t j = i + 1;
      while (j < toks.size() && toks[j] != "(") ++j;
      if (j >= toks.size()) continue;
      const std::size_t end = paren_end(j);
      std::vector<std::string> cols, keys;
      bool start = true;  // a column name starts each comma-separated definition
      int depth = 0;
      std::string current;     // the column being defined
      std::string constraint;  // a table-level PRIMARY KEY (...) / UNIQUE (...) being read
      std::vector<std::string> constraint_cols;
      auto end_definition = [&] {
        if (!constraint.empty() && constraint_cols.size() == 1) keys.push_back(constraint_cols[0]);  // one-column keys only
        constraint.clear();
        constraint_cols.clear();
        current.clear();
      };
      for (std::size_t k = j + 1; k < end; ++k) {
        const std::string uk = upper(toks[k]);
        if (toks[k] == "(") ++depth;
        else if (toks[k] == ")") --depth;
        else if (toks[k] == "," && depth == 0) {
          end_definition();
          start = true;
          continue;
        }
        if (start && depth == 0) {
          if (uk == "PRIMARY" || uk == "UNIQUE" || uk == "CONSTRAINT") constraint = uk;
          else if (is_column(toks[k])) cols.push_back(current = column_name(toks[k]));
          start = false;
          continue;
        }
        if (!constraint.empty() && depth == 1 && toks[k] != "(" && toks[k] != "," && is_column(toks[k])) {
          constraint_cols.push_back(column_name(toks[k]));
        } else if (!current.empty() && depth == 0 && (uk == "PRIMARY" || uk == "UNIQUE")) {
          keys.push_back(current);  // email TEXT UNIQUE, id INTEGER PRIMARY KEY
          current.clear();
        }
      }
      end_definition();
      add_cols(t.writes.front(), std::move(cols));
      if (!keys.empty()) out_keys.emplace_back(t.writes.front(), std::move(keys));
      i = end;
    } else if (u == "UPDATE" && !t.writes.empty()) {
      std::vector<std::string> cols;
      for (std::size_t k = i + 1; k + 1 < toks.size(); ++k) {
        const std::string uk = upper(toks[k]);
        if (uk == "WHERE" || uk == "FROM" || uk == "RETURNING") break;
        if (toks[k + 1] == "=" && is_column(toks[k]) && upper(toks[k]) != "SET") cols.push_back(column_name(toks[k]));
      }
      add_cols(t.writes.front(), std::move(cols));
    } else if (u == "SELECT" && t.reads.size() == 1 && t.writes.empty()) {
      std::vector<std::string> cols;
      int depth = 0;
      std::string last;
      bool aliased = false;
      for (std::size_t k = i + 1; k < toks.size(); ++k) {
        const std::string uk = upper(toks[k]);
        if (toks[k] == "(") ++depth;
        else if (toks[k] == ")") --depth;
        if (depth == 0 && (uk == "FROM" || toks[k] == ",")) {
          if (!last.empty()) cols.push_back(last);
          last.clear();
          aliased = false;
          if (uk == "FROM") break;
          continue;
        }
        if (depth > 0) {
          last.clear();  // an expression: not a plain column
          aliased = true;
          continue;
        }
        if (uk == "AS") {
          aliased = true;
          continue;
        }
        if (!aliased && last.empty() && is_column(toks[k])) last = column_name(toks[k]);
        else if (!aliased) last.clear();
      }
      add_cols(t.reads.front(), std::move(cols));
      break;
    }
  }
  return out;
}

}  // namespace

SqlTables sql_tables(std::string_view sql) {
  SqlTables out;
  const auto toks = tokenize(sql);
  if (toks.empty()) return out;
  const std::string first = upper(toks[0]);
  static const char* kStarts[] = {"SELECT", "INSERT", "UPDATE", "DELETE", "CREATE", "ALTER",
                                  "DROP",   "WITH",   "REPLACE", "MERGE"};
  if (std::none_of(std::begin(kStarts), std::end(kStarts),
                   [&first](const char* k) { return first == k; })) {
    return out;
  }

  // Index of the next token that names a table, skipping IF [NOT] EXISTS and ONLY.
  auto table_after = [&toks](std::size_t i) -> std::size_t {
    while (i < toks.size()) {
      const std::string u = upper(toks[i]);
      // SQLite's INSERT OR REPLACE / OR IGNORE ... INTO, too.
      if (u == "IF" || u == "NOT" || u == "EXISTS" || u == "ONLY" || u == "TABLE" || u == "INTO" || u == "OR" ||
          u == "IGNORE" || u == "ABORT" || u == "FAIL" || u == "ROLLBACK" ||
          (u == "REPLACE" && i > 0 && upper(toks[i - 1]) == "OR")) {
        ++i;
        continue;
      }
      return i;
    }
    return toks.size();
  };

  for (std::size_t i = 0; i < toks.size(); ++i) {
    const std::string u = upper(toks[i]);
    std::vector<std::string>* list = nullptr;
    std::size_t at = toks.size();
    if (u == "INSERT" || u == "REPLACE" || u == "MERGE") {
      list = &out.writes;
      at = table_after(i + 1);
    } else if (u == "UPDATE") {
      list = &out.writes;
      at = table_after(i + 1);
    } else if (u == "DELETE") {
      list = &out.writes;
      at = i + 1 < toks.size() && upper(toks[i + 1]) == "FROM" ? i + 2 : i + 1;
      i = at;  // so the FROM isn't read as a read
    } else if ((u == "CREATE" || u == "ALTER" || u == "DROP") && i + 1 < toks.size()) {
      std::size_t j = i + 1;
      while (j < toks.size() && (upper(toks[j]) == "TEMP" || upper(toks[j]) == "TEMPORARY" ||
                                 upper(toks[j]) == "UNLOGGED")) {
        ++j;
      }
      if (j < toks.size() && upper(toks[j]) == "TABLE") {
        list = &out.writes;
        at = table_after(j + 1);
      }
    } else if (u == "FROM" || u == "JOIN") {
      list = &out.reads;
      at = i + 1;
    }
    if (list && at < toks.size() && is_table(toks[at])) add(*list, clean(toks[at]));
  }
  out.columns = sql_columns(toks, out, out.keys);
  return out;
}

std::string sql_store_id(std::string_view table) {
  for (std::string_view schema : {"public.", "dbo.", "main."}) {
    if (table.size() > schema.size()) {
      bool same = true;
      for (std::size_t i = 0; i < schema.size() && same; ++i) {
        same = std::tolower(static_cast<unsigned char>(table[i])) == schema[i];
      }
      if (same) {
        table.remove_prefix(schema.size());
        break;
      }
    }
  }
  return "sql:" + std::string(table);
}

}  // namespace optum
