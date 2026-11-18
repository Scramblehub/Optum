add_executable(optum_tests
  test_version.cpp
  test_graph_json.cpp
)
target_link_libraries(optum_tests PRIVATE optum_core Catch2::Catch2WithMain)

include(${CMAKE_SOURCE_DIR}/External_Libraries/Catch2/extras/Catch.cmake)
catch_discover_tests(optum_tests)

# The CLI as a whole: runs the real binary.
add_test(NAME cli_version COMMAND optum --version)
set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "^0\\.0\\.1")
add_test(NAME cli_graph_missing_path COMMAND optum graph /no/such/path)
set_tests_properties(cli_graph_missing_path PROPERTIES WILL_FAIL TRUE)
add_test(NAME cli_graph_emits_json COMMAND optum graph ${CMAKE_SOURCE_DIR}/README.md)
set_tests_properties(cli_graph_emits_json PROPERTIES PASS_REGULAR_EXPRESSION "\"ir_version\": \"0\\.0\\.0\"")
