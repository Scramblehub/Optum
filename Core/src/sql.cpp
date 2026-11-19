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
      while (j < s.size() && ident_char(s[j])) ++j;
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
      if (u == "IF" || u == "NOT" || u == "EXISTS" || u == "ONLY" || u == "TABLE" || u == "INTO") {
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
