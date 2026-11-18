#include <catch2/catch_test_macros.hpp>

#include "optum/graph_json.hpp"
#include "optum/version.hpp"

TEST_CASE("empty graph has the versioned shape") {
  auto g = optum::empty_graph("some/path");
  REQUIRE(g["ir_version"] == std::string(optum::kIrVersion));
  REQUIRE(g["source"] == "some/path");
  REQUIRE(g["nodes"].is_array());
  REQUIRE(g["nodes"].empty());
  REQUIRE(g["edges"].is_array());
}
