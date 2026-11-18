add_executable(optum_tests
  test_version.cpp
  test_ir.cpp
  test_ir_json.cpp
)
target_link_libraries(optum_tests PRIVATE optum_core Catch2::Catch2WithMain)

include(${CMAKE_SOURCE_DIR}/External_Libraries/Catch2/extras/Catch.cmake)
catch_discover_tests(optum_tests)

set(FIXTURES ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
file(GLOB IR_VALID ${FIXTURES}/ir/valid/*.json)
file(GLOB IR_INVALID ${FIXTURES}/ir/invalid/*.json)
file(GLOB IR_SCHEMA_INVALID ${FIXTURES}/ir/invalid/schema_*.json)

# The CLI as a whole: runs the real binary.
add_test(NAME cli_version COMMAND optum --version)
set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "^0\\.0\\.1")
add_test(NAME cli_graph_missing_path COMMAND optum graph /no/such/path)
set_tests_properties(cli_graph_missing_path PROPERTIES WILL_FAIL TRUE)
add_test(NAME cli_graph_emits_json COMMAND optum graph ${CMAKE_SOURCE_DIR}/README.md)
set_tests_properties(cli_graph_emits_json PROPERTIES PASS_REGULAR_EXPRESSION "\"ir_version\": \"0\\.1\\.0\"")
add_test(NAME cli_check_valid COMMAND optum check ${IR_VALID})
add_test(NAME cli_merge COMMAND optum merge ${IR_VALID} --indent -1)
set_tests_properties(cli_merge PROPERTIES PASS_REGULAR_EXPRESSION "\"source\":\"merged\"")
foreach(f ${IR_INVALID})
  get_filename_component(name ${f} NAME_WE)
  add_test(NAME cli_check_rejects_${name} COMMAND optum check ${f})
  # Must fail with a proper error message, not a crash.
  set_tests_properties(cli_check_rejects_${name} PROPERTIES
    PASS_REGULAR_EXPRESSION "\\.json: (graph|ir_version|nodes|edges|invalid JSON)"
    FAIL_REGULAR_EXPRESSION "Sanitizer|runtime error|terminate called")
endforeach()

# The published JSON Schema must agree with the C++ reader on the fixtures.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  execute_process(COMMAND ${Python3_EXECUTABLE} -c "import jsonschema"
                  RESULT_VARIABLE NO_JSONSCHEMA OUTPUT_QUIET ERROR_QUIET)
endif()
if(Python3_FOUND AND NOT NO_JSONSCHEMA)
  set(VALIDATE ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_schema.py
      ${CMAKE_SOURCE_DIR}/docs/ir.schema.json)
  add_test(NAME schema_accepts_valid COMMAND ${VALIDATE} ${IR_VALID})
  add_test(NAME schema_rejects_invalid COMMAND ${VALIDATE} --expect-fail ${IR_SCHEMA_INVALID})
else()
  message(STATUS "Python jsonschema not found: skipping schema tests (pip install jsonschema)")
endif()
