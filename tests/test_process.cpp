#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "optum/ir.hpp"
#include "optum/process.hpp"

using namespace optum;
using V = std::vector<std::string>;

TEST_CASE("split_command handles quotes") {
  REQUIRE(split_command("python3 a.py") == V{"python3", "a.py"});
  REQUIRE(split_command("  run  'my file.py' \"x y\" z ") == V{"run", "my file.py", "x y", "z"});
  REQUIRE(split_command("a ''") == V{"a", ""});
  REQUIRE_THROWS_AS(split_command("a 'b"), IrError);
}

TEST_CASE("run_process: stdin to stdout, exit codes, stderr") {
  auto r = run_process({"cat"}, "hello", std::chrono::seconds(5));
  REQUIRE(r.exit_code == 0);
  REQUIRE(r.out == "hello");
  r = run_process({"sh", "-c", "echo oops >&2; exit 4"}, "", std::chrono::seconds(5));
  REQUIRE(r.exit_code == 4);
  REQUIRE(r.err == "oops\n");
}

TEST_CASE("run_process: large input and output don't deadlock") {
  const std::string big(4 * 1024 * 1024, 'x');
  const auto r = run_process({"cat"}, big, std::chrono::seconds(20));
  REQUIRE(r.out.size() == big.size());
}

TEST_CASE("run_process: timeout and missing programs") {
  const auto r = run_process({"sleep", "5"}, "", std::chrono::milliseconds(200));
  REQUIRE(r.timed_out);
  REQUIRE_THROWS_WITH(run_process({"/no/such/program"}, "", std::chrono::seconds(1)),
                      Catch::Matchers::ContainsSubstring("cannot run"));
}
