add_executable(optum_tests
  test_version.cpp
  test_ir.cpp
  test_ir_json.cpp
  test_score.cpp
  test_sql_http.cpp
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
add_test(NAME cli_graph_emits_json COMMAND optum graph -q ${CMAKE_SOURCE_DIR}/README.md)
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

# Sample programs (docs/frontends.md): expected graphs are valid and score 100% against
# themselves; the programs themselves compile.
file(GLOB_RECURSE PROGRAM_EXPECTED ${FIXTURES}/programs/expected.json)
foreach(f ${PROGRAM_EXPECTED})
  get_filename_component(dir ${f} DIRECTORY)
  file(RELATIVE_PATH rel ${FIXTURES}/programs ${dir})
  string(REPLACE "/" "_" name ${rel})
  add_test(NAME fixture_${name}_self_score
           COMMAND optum score ${f} ${f} --min-precision 1 --min-recall 1)
endforeach()
# The Python frontend against each sample program. These minimums only go up: a change that
# makes a sample worse fails here. `dynamic` misses its 5 hard edges by design (16/21).
set(PY_MIN_RECALL_dynamic 0.76)
file(GLOB PY_PROGRAMS LIST_DIRECTORIES true ${FIXTURES}/programs/python/*)
foreach(dir ${PY_PROGRAMS})
  if(NOT IS_DIRECTORY ${dir})
    continue()
  endif()
  get_filename_component(name ${dir} NAME)
  set(min_recall 1)
  if(DEFINED PY_MIN_RECALL_${name})
    set(min_recall ${PY_MIN_RECALL_${name}})
  endif()
  set(out ${CMAKE_CURRENT_BINARY_DIR}/produced_python_${name}.json)
  add_test(NAME frontend_python_${name}
           COMMAND sh -c "$<TARGET_FILE:optum> graph ${dir} -q -o ${out} && $<TARGET_FILE:optum> score ${dir}/expected.json ${out} --min-precision 1 --min-recall ${min_recall}")
endforeach()

add_test(NAME cli_score_detects_gaps
         COMMAND optum score ${FIXTURES}/programs/python/classes/expected.json
                 ${FIXTURES}/ir/valid/order_sync.json --min-recall 0.5)
set_tests_properties(cli_score_detects_gaps PROPERTIES WILL_FAIL TRUE)

find_program(GCC gcc)
if(GCC)
  file(GLOB_RECURSE C_FIXTURES ${FIXTURES}/programs/c/*.c)
  add_test(NAME fixture_c_compiles COMMAND ${GCC} -Wall -Wextra -Werror -fsyntax-only ${C_FIXTURES})
endif()

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
  file(GLOB_RECURSE PY_FIXTURES ${FIXTURES}/programs/python/*.py)
  add_test(NAME fixture_python_compiles
           COMMAND ${Python3_EXECUTABLE} -c "import ast, sys; [ast.parse(open(f).read(), f) for f in sys.argv[1:]]" ${PY_FIXTURES})
  add_test(NAME schema_accepts_expected COMMAND ${VALIDATE} ${PROGRAM_EXPECTED})
else()
  message(STATUS "Python jsonschema not found: skipping schema tests (pip install jsonschema)")
endif()
