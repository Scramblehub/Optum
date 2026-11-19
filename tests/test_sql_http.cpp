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
