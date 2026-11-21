# CLI behaviour that isn't about a graph's content: help text and error messages.
# Included from tests/CMakeLists.txt.
set(CLI_SAMPLE ${CMAKE_SOURCE_DIR}/tests/fixtures/programs/python/basic_calls)

add_test(NAME cli_help_has_examples COMMAND optum graph --help)
set_tests_properties(cli_help_has_examples PROPERTIES PASS_REGULAR_EXPRESSION "Examples:.*Edge kinds:")

# A bad --edges fails fast, names the valid kinds, and exits non-zero without a graph.
add_test(NAME cli_unknown_edge_kind
         COMMAND sh -c "$<TARGET_FILE:optum> graph ${CLI_SAMPLE} -q --edges bogus 2>&1; test $? -ne 0")
set_tests_properties(cli_unknown_edge_kind PROPERTIES
  PASS_REGULAR_EXPRESSION "unknown edge kind 'bogus'.*sends_to")

add_test(NAME cli_missing_graph_file COMMAND sh -c "$<TARGET_FILE:optum> export /no/such.json 2>&1; test $? -ne 0")
set_tests_properties(cli_missing_graph_file PROPERTIES PASS_REGULAR_EXPRESSION "no such file \\(make one with")

# A write that fails after the file opened (full disk) must be reported, with a non-zero exit.
if(EXISTS /dev/full)
  add_test(NAME cli_write_failure_reported
           COMMAND sh -c "$<TARGET_FILE:optum> graph ${CLI_SAMPLE} -q -o /dev/full 2>&1; test $? -eq 2")
  set_tests_properties(cli_write_failure_reported PROPERTIES PASS_REGULAR_EXPRESSION "error writing /dev/full")
endif()

# A directory where a graph (or a binary) is expected is an error message, not an abort.
add_test(NAME cli_check_directory_is_an_error
         COMMAND sh -c "$<TARGET_FILE:optum> check ${CMAKE_SOURCE_DIR}/tests 2>&1; test $? -eq 1")
set_tests_properties(cli_check_directory_is_an_error PROPERTIES PASS_REGULAR_EXPRESSION "is a directory, not a graph JSON file")
add_test(NAME cli_disasm_directory_is_an_error
         COMMAND sh -c "$<TARGET_FILE:optum> disasm ${CMAKE_SOURCE_DIR}/tests 2>&1; test $? -eq 1")
set_tests_properties(cli_disasm_directory_is_an_error PROPERTIES PASS_REGULAR_EXPRESSION "is a directory, not an ELF file")
