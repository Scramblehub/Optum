#include <catch2/catch_test_macros.hpp>

#include "optum/version.hpp"

TEST_CASE("version is set from CMake") { REQUIRE(optum::version() == "0.0.1"); }
