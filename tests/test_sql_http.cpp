#include <catch2/catch_test_macros.hpp>

#include "optum/http.hpp"
#include "optum/sql.hpp"

using optum::http_node_id;
using optum::sql_tables;
using V = std::vector<std::string>;

TEST_CASE("sql: reads and writes by statement type") {
  auto t = sql_tables("SELECT c.email, SUM(o.total) FROM orders o JOIN customers c ON c.id = o.customer_id");
  REQUIRE(t.reads == V{"orders", "customers"});
  REQUIRE(t.writes.empty());

  REQUIRE(sql_tables("INSERT INTO customers (email) VALUES (?)").writes == V{"customers"});
  REQUIRE(sql_tables("update public.orders set total = 1").writes == V{"public.orders"});
  t = sql_tables("DELETE FROM orders WHERE id IN (SELECT id FROM stale)");
  REQUIRE(t.writes == V{"orders"});
  REQUIRE(t.reads == V{"stale"});
  REQUIRE(sql_tables("CREATE TABLE IF NOT EXISTS \"Users\" (id int)").writes == V{"Users"});
  REQUIRE(sql_tables("CREATE TEMP TABLE scratch (x int)").writes == V{"scratch"});
  REQUIRE(sql_tables("SELECT * FROM (SELECT 1) sub").reads.empty());
}

TEST_CASE("sql: store ids drop default schemas only") {
  REQUIRE(optum::sql_store_id("orders") == "sql:orders");
  REQUIRE(optum::sql_store_id("public.orders") == "sql:orders");
  REQUIRE(optum::sql_store_id("DBO.Orders") == "sql:Orders");
  REQUIRE(optum::sql_store_id("sales.orders") == "sql:sales.orders");
}

TEST_CASE("sql: non-SQL text gives nothing") {
  const auto t = sql_tables("from here to there");
  REQUIRE(t.reads.empty());
  REQUIRE(t.writes.empty());
  REQUIRE(sql_tables("").reads.empty());
}

TEST_CASE("http: URLs become stable node ids") {
  REQUIRE(http_node_id("https://shop.example.com/api/orders") == "http:shop.example.com/api/orders");
  REQUIRE(http_node_id("https://API.Example.com:443/v1/deals/{}?x=1#f") == "http:api.example.com/v1/deals/{}");
  REQUIRE(http_node_id("http://localhost:8080/") == "http:localhost:8080");
  REQUIRE(http_node_id("https://crm.example.com/api/deals/") == "http:crm.example.com/api/deals");
}

TEST_CASE("http: unknown host or no scheme gives nothing") {
  REQUIRE(http_node_id("{}/orders").empty());
  REQUIRE(http_node_id("https://{}/orders").empty());
  REQUIRE(http_node_id("ftp://example.com/x").empty());
  REQUIRE(http_node_id("/api/orders").empty());
}

#include <fstream>
#include <nlohmann/json.hpp>

TEST_CASE("shared id cases (also checked against every adapter)") {
  std::ifstream in(OPTUM_FIXTURES "/ids/url_cases.json");
  REQUIRE(in);
  const auto cases = nlohmann::json::parse(in);
  for (const auto& c : cases["cases"]) {
    INFO(c[0].get<std::string>());
    REQUIRE(http_node_id(c[0].get<std::string>()) == c[1].get<std::string>());
  }
  for (const auto& c : cases["sql_cases"]) {
    INFO(c[0].get<std::string>());
    REQUIRE(optum::sql_store_id(c[0].get<std::string>()) == c[1].get<std::string>());
  }
}

TEST_CASE("sql: quoted identifiers are one name, spaces and schemas included") {
  auto t = sql_tables(R"(INSERT INTO "My Table" (x) VALUES (1))");
  REQUIRE(t.writes == std::vector<std::string>{"My Table"});
  t = sql_tables(R"(select * from public."Orders" o join "sales"."Items" i on 1)");
  REQUIRE(t.reads == std::vector<std::string>{"public.Orders", "sales.Items"});
  REQUIRE(optum::sql_store_id("public.Orders") == "sql:Orders");
  t = sql_tables("SELECT * FROM `t3` JOIN [order details] ON 1");
  REQUIRE(t.reads == std::vector<std::string>{"t3", "order details"});
}

TEST_CASE("sql: columns per table, for finding entities") {
  using Cols = std::vector<std::pair<std::string, std::vector<std::string>>>;
  REQUIRE(sql_tables("CREATE TABLE IF NOT EXISTS customers (id INTEGER PRIMARY KEY, email TEXT NOT NULL, "
                     "first_name VARCHAR(80), PRIMARY KEY (id), UNIQUE (email))").columns ==
          Cols{{"customers", {"id", "email", "first_name"}}});
  REQUIRE(sql_tables("INSERT INTO orders (customer_id, total, \"Status\") VALUES (?, ?, ?)").columns ==
          Cols{{"orders", {"customer_id", "total", "Status"}}});
  REQUIRE(sql_tables("UPDATE customers SET email = ?, phone = ? WHERE id = ?").columns ==
          Cols{{"customers", {"email", "phone"}}});
  REQUIRE(sql_tables("SELECT c.id, c.email, count(*) AS n, plan FROM customers c").columns ==
          Cols{{"customers", {"id", "email", "plan"}}});
  REQUIRE(sql_tables("SELECT o.id FROM orders o JOIN customers c ON 1").columns.empty());  // two tables
  REQUIRE(sql_tables("SELECT * FROM orders").columns.empty());
}

TEST_CASE("sql: SQLite's INSERT OR REPLACE names the table after INTO") {
  const auto t = sql_tables("INSERT OR REPLACE INTO bans(jail, ip, timeofban) VALUES(?, ?, ?)");
  REQUIRE(t.writes == V{"bans"});
  REQUIRE(t.columns == std::vector<std::pair<std::string, std::vector<std::string>>>{{"bans", {"jail", "ip", "timeofban"}}});
  REQUIRE(sql_tables("INSERT OR IGNORE INTO tags (name) VALUES (?)").writes == V{"tags"});
}
