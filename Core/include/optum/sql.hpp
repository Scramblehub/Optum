#pragma once
// Finds which tables a SQL statement reads and writes. Language-neutral: any frontend that sees
// a literal SQL string can use it. A token scan, not a full SQL parser: good for the common
// SELECT / JOIN / INSERT / UPDATE / DELETE / CREATE shapes.

#include <string>
#include <string_view>
#include <vector>

namespace optum {

struct SqlTables {
  std::vector<std::string> reads;   // FROM and JOIN sources
  std::vector<std::string> writes;  // INSERT INTO, UPDATE, DELETE FROM, CREATE/ALTER/DROP TABLE
  // Columns named for a table (unquoted, as written): CREATE TABLE definitions, INSERT's column
  // list, UPDATE's SET, and SELECT's list when the statement reads one table. For finding the
  // same entity in several places (docs/diagnose.md).
  std::vector<std::pair<std::string, std::vector<std::string>>> columns;
};

// Returns empty lists when the text doesn't look like SQL. Table names keep their schema prefix
// ("public.orders") and lose quotes. Each table appears at most once per list.
SqlTables sql_tables(std::string_view sql);

// The store node id for a table: "sql:" + the table, without a default schema ("public.",
// "dbo.", "main."), so every frontend names the same table the same way.
std::string sql_store_id(std::string_view table);

}  // namespace optum
