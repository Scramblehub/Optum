#include <catch2/catch_test_macros.hpp>

#include "optum/infer.hpp"

using namespace optum;
using nlohmann::json;

TEST_CASE("infer: ids in paths fold, names don't", "[infer]") {
  CHECK(template_path("/wp-json/wc/v3/orders/2026") == "/wp-json/wc/v3/orders/{}");
  CHECK(template_path("/v1/charges/ch_Rk0oMioJB0GZRVq/refund") == "/v1/charges/{}/refund");
  CHECK(template_path("/jobs/3fa85f64-5717-4562-b3fc-2c963f66afa6") == "/jobs/{}");
  CHECK(template_path("/items/507f1f77bcf86cd799439011") == "/items/{}");  // 24 hex digits
  CHECK(template_path("/webhook/abandoned-cart") == "/webhook/abandoned-cart");
  CHECK(template_path("/api/v2/users") == "/api/v2/users");
  CHECK(template_path("/x_short/deadbeef") == "/x_short/deadbeef");  // a word, not an id
  CHECK(template_path("") == "");
}

TEST_CASE("infer: a JSON-RPC create is an operation on its model, with the record's fields", "[infer]") {
  const json rec = {{"tool", "accounting"}, {"started", "2026-11-20T10:00:00Z"}, {"method", "POST"}, {"path", "/jsonrpc"},
                    {"client", "owned"},
                    {"request", {{"body", {{"params", {{"service", "object"}, {"method", "execute_kw"},
                                 {"args", {"db", 2, "x", "account.move", "create", {{{"ref", "order 1"}, {"partner_id", 7}}}}}}}}}}},
                    {"response", {{"status", 200}, {"body", {{"result", 5}}}}}};
  const auto spec = learn_spec({rec});
  REQUIRE(spec["operations"].contains("rpc account.move.create"));
  const auto& op = spec["operations"]["rpc account.move.create"];
  CHECK(op["clients"]["owned"] == 1);
  CHECK(op["request_fields"].contains("ref"));
  CHECK(op["resource"] == "account.move");
  CHECK(spec["resources"]["account.move"].contains("partner_id"));
}

TEST_CASE("infer: a status sent or returned over time is a status change", "[infer]") {
  auto put = [](const char* t, const char* status) {
    return json{{"tool", "shop"}, {"started", t}, {"method", "PUT"}, {"path", "/orders/9"}, {"client", "hooks"},
                {"request", {{"body", {{"status", status}}}}}, {"response", {{"status", 200}, {"body", {{"id", 9}, {"status", status}}}}}};
  };
  // Out of order on purpose: changes follow time, not file order.
  const auto spec = learn_spec({put("2026-11-20T10:02:00Z", "completed"), put("2026-11-20T10:01:00Z", "processing"),
                                put("2026-11-20T10:01:30Z", "processing")});
  CHECK(spec["status_changes"]["orders"] == nlohmann::ordered_json{{"processing -> completed", 1}});
  CHECK(spec["operations"]["PUT /orders/{}"]["calls"] == 3);
}
