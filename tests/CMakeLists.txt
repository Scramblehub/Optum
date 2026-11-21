add_executable(optum_tests
  test_version.cpp
  test_ir.cpp
  test_ir_json.cpp
  test_score.cpp
  test_infer.cpp
  test_python_frontend.cpp
  test_sql_http.cpp
  test_deep_nesting.cpp
  test_view_export.cpp
  test_process.cpp
  test_frontend_robustness.cpp
  test_c_frontend.cpp
  test_elf.cpp
  test_dwarf.cpp
  test_malformed_graphs.cpp
)
target_link_libraries(optum_tests PRIVATE optum_core Catch2::Catch2WithMain)
target_compile_definitions(optum_tests PRIVATE OPTUM_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

include(${CMAKE_SOURCE_DIR}/External_Libraries/Catch2/extras/Catch.cmake)
catch_discover_tests(optum_tests)

set(FIXTURES ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
file(GLOB IR_VALID ${FIXTURES}/ir/valid/*.json)
file(GLOB IR_INVALID ${FIXTURES}/ir/invalid/*.json)
file(GLOB IR_SCHEMA_INVALID ${FIXTURES}/ir/invalid/schema_*.json)

# The CLI as a whole: runs the real binary.
add_test(NAME cli_version COMMAND optum --version)
set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "^0\\.0\\.1")
include(${CMAKE_CURRENT_SOURCE_DIR}/cli_tests.cmake)
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
# makes a sample worse fails here. `dynamic` misses 3 of its 5 hard edges by design (18/21).
set(PY_MIN_RECALL_dynamic 0.85)
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

# Analysing a package directory directly names modules from the outermost package.
# The JS/TS frontend against each sample program, same rules as Python. `dynamic` misses its
# 3 hard edges by design (13/16).
set(JS_MIN_RECALL_dynamic 0.81)
file(GLOB JS_PROGRAMS LIST_DIRECTORIES true ${FIXTURES}/programs/js/*)
foreach(dir ${JS_PROGRAMS})
  if(NOT IS_DIRECTORY ${dir})
    continue()
  endif()
  get_filename_component(name ${dir} NAME)
  set(min_recall 1)
  if(DEFINED JS_MIN_RECALL_${name})
    set(min_recall ${JS_MIN_RECALL_${name}})
  endif()
  set(out ${CMAKE_CURRENT_BINARY_DIR}/produced_js_${name}.json)
  add_test(NAME frontend_js_${name}
           COMMAND sh -c "$<TARGET_FILE:optum> graph ${dir} -q -o ${out} && $<TARGET_FILE:optum> score ${dir}/expected.json ${out} --min-precision 1 --min-recall ${min_recall}")
endforeach()

add_test(NAME cli_syntax_tree COMMAND optum syntax-tree ${FIXTURES}/programs/js/classes/shapes.ts)
set_tests_properties(cli_syntax_tree PROPERTIES PASS_REGULAR_EXPRESSION "\\(program .*class_declaration name: \\(type_identifier\\)")

add_test(NAME frontend_python_package_root_naming
         COMMAND optum graph -q --indent -1 ${FIXTURES}/programs/python/imports/shop)
set_tests_properties(frontend_python_package_root_naming PROPERTIES
  PASS_REGULAR_EXPRESSION "\"id\":\"py:shop.pricing\""
  FAIL_REGULAR_EXPRESSION "\"py:pricing\"")

# Diagrams through the CLI.
set(SAMPLE_HTTP ${FIXTURES}/programs/python/http_glue)
add_test(NAME cli_graph_dot COMMAND optum graph -q -f dot ${SAMPLE_HTTP})
set_tests_properties(cli_graph_dot PROPERTIES PASS_REGULAR_EXPRESSION "digraph optum")
add_test(NAME cli_graph_mermaid COMMAND optum graph -q -f mermaid ${SAMPLE_HTTP})
set_tests_properties(cli_graph_mermaid PROPERTIES PASS_REGULAR_EXPRESSION "flowchart LR")
add_test(NAME cli_graph_module_level COMMAND optum graph -q --level module --indent -1 ${FIXTURES}/programs/python/imports)
set_tests_properties(cli_graph_module_level PROPERTIES PASS_REGULAR_EXPRESSION "\"count\":")
add_test(NAME cli_focus_ambiguous COMMAND optum graph -q --focus area -f dot ${FIXTURES}/programs/python/classes)
set_tests_properties(cli_focus_ambiguous PROPERTIES PASS_REGULAR_EXPRESSION "matches 3 nodes")
add_test(NAME cli_max_nodes_refuses COMMAND optum graph -q -f dot --max-nodes 2 ${SAMPLE_HTTP})
set_tests_properties(cli_max_nodes_refuses PROPERTIES PASS_REGULAR_EXPRESSION "too many for a readable diagram")
find_program(GRAPHVIZ_DOT dot)
if(GRAPHVIZ_DOT)
  add_test(NAME cli_export_svg COMMAND optum export ${FIXTURES}/ir/valid/order_sync.json)
  set_tests_properties(cli_export_svg PROPERTIES PASS_REGULAR_EXPRESSION "<svg")
endif()

add_test(NAME cli_score_detects_gaps
         COMMAND optum score ${FIXTURES}/programs/python/classes/expected.json
                 ${FIXTURES}/ir/valid/order_sync.json --min-recall 0.5)
set_tests_properties(cli_score_detects_gaps PROPERTIES WILL_FAIL TRUE)

# Each C sample compiles cleanly; http_glue needs the libcurl and sqlite headers (CI installs them).
find_program(GCC gcc)
include(CheckIncludeFile)
check_include_file(curl/curl.h HAVE_CURL_H)
check_include_file(sqlite3.h HAVE_SQLITE3_H)
if(GCC)
  file(GLOB C_PROGRAMS LIST_DIRECTORIES true ${FIXTURES}/programs/c/*)
  foreach(dir ${C_PROGRAMS})
    if(NOT IS_DIRECTORY ${dir})
      continue()
    endif()
    get_filename_component(name ${dir} NAME)
    if(name STREQUAL "http_glue" AND NOT (HAVE_CURL_H AND HAVE_SQLITE3_H))
      message(STATUS "libcurl/sqlite headers not found: skipping fixture_c_compiles_http_glue")
      continue()
    endif()
    file(GLOB_RECURSE srcs ${dir}/*.c)
    add_test(NAME fixture_c_compiles_${name} COMMAND ${GCC} -Wall -Wextra -Werror -fsyntax-only ${srcs})
  endforeach()
endif()

# The C frontend against each sample program (all edges, hard ones included, at 100%).
foreach(dir ${C_PROGRAMS})
  if(NOT IS_DIRECTORY ${dir})
    continue()
  endif()
  get_filename_component(name ${dir} NAME)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/produced_c_${name}.json)
  add_test(NAME frontend_c_${name}
           COMMAND sh -c "$<TARGET_FILE:optum> graph ${dir} -q -o ${out} && $<TARGET_FILE:optum> score ${dir}/expected.json ${out} --min-precision 1 --min-recall 1")
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
  file(GLOB_RECURSE PY_FIXTURES ${FIXTURES}/programs/python/*.py)
  add_test(NAME fixture_python_compiles
           COMMAND ${Python3_EXECUTABLE} -c "import ast, sys; [ast.parse(open(f).read(), f) for f in sys.argv[1:]]" ${PY_FIXTURES})
  add_test(NAME schema_accepts_expected COMMAND ${VALIDATE} ${PROGRAM_EXPECTED})
endif()

# Runtime traces (docs/verify.md): the Python tracer runs each runnable sample, and every call it
# observes must be in the graph, except the hard edges `dynamic` misses by design, and in `flows`
# the call a metaclass's super().__call__() makes to __init__.
if(Python3_FOUND)
  set(PY_VERIFY_MIN_dynamic 0.7)
  set(PY_VERIFY_MIN_flows 0.96)
  foreach(case basic_calls:app.py classes:shapes.py imports:main.py sql_store:store.py dynamic:plugins.py shadowing:main.py flows:flows.py)
    string(REPLACE ":" ";" parts ${case})
    list(GET parts 0 name)
    list(GET parts 1 script)
    set(min 1)
    if(DEFINED PY_VERIFY_MIN_${name})
      set(min ${PY_VERIFY_MIN_${name}})
    endif()
    set(dir ${FIXTURES}/programs/python/${name})
    set(g ${CMAKE_CURRENT_BINARY_DIR}/verify_${name}_graph.json)
    set(tr ${CMAKE_CURRENT_BINARY_DIR}/verify_${name}_trace.json)
    add_test(NAME verify_python_${name}
             COMMAND sh -c "${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/Core/tracers/python_trace.py --root ${dir} -o ${tr} -- ${dir}/${script} > /dev/null && $<TARGET_FILE:optum> graph ${dir} -q -o ${g} && $<TARGET_FILE:optum> verify ${g} ${tr} --min-recall ${min}")
  endforeach()
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/bad_trace.json "{\"trace_version\": \"9.0\", \"calls\": []}")
  add_test(NAME cli_verify_rejects_bad_trace
           COMMAND optum verify ${FIXTURES}/ir/valid/minimal.json ${CMAKE_CURRENT_BINARY_DIR}/bad_trace.json)
  set_tests_properties(cli_verify_rejects_bad_trace PROPERTIES PASS_REGULAR_EXPRESSION "unsupported trace_version '9.0'")
endif()

# optum diagnose against its answer keys (docs/diagnose.md): planted findings, matched exactly.
if(Python3_FOUND)
  foreach(name crm_glue shared_table one_package unused dupes entities)
    set(dir ${FIXTURES}/programs/mixed/${name})
    set(out ${CMAKE_CURRENT_BINARY_DIR}/diagnosis_${name}.json)
    add_test(NAME diagnose_${name}
             COMMAND sh -c "$<TARGET_FILE:optum> diagnose ${dir} -q -f json -o ${out} --adapter '${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Core/adapters/n8n/optum_n8n.py' && ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_diagnosis.py ${out} ${dir}/expected_diagnosis.json")
  endforeach()
endif()
add_test(NAME cli_diagnose_needs_input COMMAND optum diagnose)
set_tests_properties(cli_diagnose_needs_input PROPERTIES PASS_REGULAR_EXPRESSION "give a directory to analyse, or --graph")
add_test(NAME cli_diagnose_markdown COMMAND optum diagnose ${FIXTURES}/programs/mixed/shared_table -q)
set_tests_properties(cli_diagnose_markdown PROPERTIES PASS_REGULAR_EXPRESSION "# Optum diagnosis.*2 tools reach sql:customers")

# The test company's data generator (testco/README.md): deterministic, traps in place.
if(Python3_FOUND)
  add_test(NAME testco_generator COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_testco_data.py)
  add_test(NAME testco_observe_records COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_observe.py)
  add_test(NAME infer_spec COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_infer_spec.py $<TARGET_FILE:optum>)
  # optum model on the test company against its answer key (docs/plans/capabilities.md).
  add_test(NAME testco_model COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_model.py $<TARGET_FILE:optum> ${Python3_EXECUTABLE})
  # The plateau loop's gate: before/after per relationship type; a lost call fails (docs/plans/plateau.md).
  add_test(NAME plateau_compare COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_plateau_compare.py)
  # The test company's glue: optum diagnose finds its planted waste (static: no Docker needed).
  add_test(NAME testco_diagnose_glue
           COMMAND sh -c "$<TARGET_FILE:optum> diagnose ${CMAKE_SOURCE_DIR}/testco/company -q -f json -o ${CMAKE_CURRENT_BINARY_DIR}/testco_glue.json --adapter '${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Core/adapters/n8n/optum_n8n.py' && ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_diagnosis.py ${CMAKE_CURRENT_BINARY_DIR}/testco_glue.json ${CMAKE_SOURCE_DIR}/testco/expected_diagnosis.json")
  # ...and the webhook's URL (process.env.X || default, a template with a hole) reaches the shop.
  add_test(NAME testco_js_url_defaults
           COMMAND sh -c "$<TARGET_FILE:optum> graph ${CMAKE_SOURCE_DIR}/testco/company -q -o ${CMAKE_CURRENT_BINARY_DIR}/testco_glue_graph.json && ${Python3_EXECUTABLE} -c \"import json, sys; g = json.load(open(sys.argv[1])); assert any(e['from'].startswith('js:payments_webhook.js') and e['to'] == 'http:shop.testco.internal:8080/wp-json/wc/v3/orders/{}' for e in g['edges']), 'the JS webhook does not reach the shop'\" ${CMAKE_CURRENT_BINARY_DIR}/testco_glue_graph.json")
endif()

# The first automatic fix (docs/fixes.md): merge exact duplicates, gated by the sample's tests.
if(Python3_FOUND)
  foreach(mode apply dry revert)
    add_test(NAME fix_merge_duplicates_${mode}
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_fix.py ${mode}
                     --optum $<TARGET_FILE:optum> --sample ${FIXTURES}/fixes/merge_duplicates
                     --work ${CMAKE_CURRENT_BINARY_DIR}/fix_${mode})
    set_tests_properties(fix_merge_duplicates_${mode} PROPERTIES TIMEOUT 120)
  endforeach()
  add_test(NAME fix_merge_duplicates_edges
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_fix_edges.py
                   --optum $<TARGET_FILE:optum> --sample ${FIXTURES}/fixes/merge_duplicates
                   --work ${CMAKE_CURRENT_BINARY_DIR}/fix_edges)
  set_tests_properties(fix_merge_duplicates_edges PROPERTIES TIMEOUT 120)
  add_test(NAME tracers_edge_cases
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_tracers.py
                   --work ${CMAKE_CURRENT_BINARY_DIR}/tracer_edges)
  set_tests_properties(tracers_edge_cases PROPERTIES TIMEOUT 120)
endif()

# External adapters (docs/protocol.md). Need Python, but not jsonschema.
if(Python3_FOUND)
  set(N8N "${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Core/adapters/n8n/optum_n8n.py")
  add_test(NAME adapter_n8n_shared_ids
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_shared_ids.py
                   ${FIXTURES}/ids/url_cases.json ${CMAKE_SOURCE_DIR}/Core/adapters/n8n optum_n8n)
  foreach(dir n8n/order_to_crm mixed/crm_glue)
    string(REPLACE "/" "_" name ${dir})
    set(out ${CMAKE_CURRENT_BINARY_DIR}/produced_${name}.json)
    add_test(NAME frontend_${name}
             COMMAND sh -c "$<TARGET_FILE:optum> graph ${FIXTURES}/programs/${dir} -q --adapter '${N8N}' -o ${out} && $<TARGET_FILE:optum> score ${FIXTURES}/programs/${dir}/expected.json ${out} --min-precision 1 --min-recall 1")
  endforeach()
  add_test(NAME cli_frontends_lists_adapter
           COMMAND optum frontends ${FIXTURES}/programs/mixed --adapter "${N8N}")
  set_tests_properties(cli_frontends_lists_adapter PROPERTIES PASS_REGULAR_EXPRESSION "n8n 0.1.0  \\*.json  \\([0-9]+ files\\)")

  # A file shaped like a workflow but with odd entries must be read, not crash the adapter.
  add_test(NAME adapter_n8n_odd_workflow
           COMMAND optum graph ${FIXTURES}/adapters --adapter "${N8N}")
  set_tests_properties(adapter_n8n_odd_workflow PROPERTIES
                       PASS_REGULAR_EXPRESSION "n8n:odd_workflow/Fetch"
                       FAIL_REGULAR_EXPRESSION "Traceback|exited with status")

  # Misbehaving adapters: each must fail with a clear message naming the adapter.
  set(FAKE "${Python3_EXECUTABLE} ${FIXTURES}/adapters/fake_adapter.py")
  set(ADAPTER_DIR ${FIXTURES}/adapters)
  foreach(case "ok|fake adapter ran"
               "patch_protocol|fake adapter ran"
               "newer_minor|speaks protocol 'optum-frontend/0.2'"
               "bad_protocol|speaks protocol 'optum-frontend/9.0'"
               "crash|exited with status 3; last output:.something broke"
               "killed|killed by signal 9 .Killed.; last output:.about to die"
               "not_json|stdout is not JSON"
               "bad_graph|not a valid graph: nodes.0..kind: unknown node kind 'gizmo'"
               "slow|no answer within 1 s")
    string(REPLACE "|" ";" parts "${case}")
    list(GET parts 0 mode)
    list(GET parts 1 expect)
    add_test(NAME adapter_${mode}
             COMMAND optum graph ${ADAPTER_DIR} --adapter "${FAKE} ${mode}" --adapter-timeout 1)
    set_tests_properties(adapter_${mode} PROPERTIES PASS_REGULAR_EXPRESSION "${expect}"
                         FAIL_REGULAR_EXPRESSION "Sanitizer|runtime error|terminate called")
  endforeach()
  add_test(NAME adapter_missing_command COMMAND optum graph ${ADAPTER_DIR} --adapter /no/such/adapter)
  set_tests_properties(adapter_missing_command PROPERTIES PASS_REGULAR_EXPRESSION "adapter '/no/such/adapter': cannot run")
endif()

# The binary reference corpus (docs/plans/binary.md, stage 2; tests/corpus/README.md): built once
# by corpus_build, then checked. Building it needs gcc and binutils and takes a few seconds.
if(Python3_FOUND AND GCC)
  set(CORPUS_DIR ${CMAKE_BINARY_DIR}/corpus)
  set(TOOLS ${CMAKE_CURRENT_SOURCE_DIR}/tools)
  add_test(NAME corpus_build COMMAND ${Python3_EXECUTABLE} ${TOOLS}/build_corpus.py --out ${CORPUS_DIR})
  set_tests_properties(corpus_build PROPERTIES FIXTURES_SETUP corpus TIMEOUT 600)
  add_test(NAME corpus_answer_keys COMMAND ${Python3_EXECUTABLE} ${TOOLS}/build_corpus.py --out ${CORPUS_DIR} --check)
  # Symbols in the symbols-only form must match the answer keys exactly (checks the keys).
  add_test(NAME corpus_symbols_baseline
           COMMAND ${Python3_EXECUTABLE} ${TOOLS}/score_functions.py --corpus ${CORPUS_DIR} --baseline symbols
                   --min-precision 1 --min-recall 1)
  # gcc's unwind tables give nearly every function start, even stripped (all but the C runtime's
  # size-0 functions): the bar a binary frontend must clear.
  add_test(NAME corpus_eh_frame_baseline
           COMMAND ${Python3_EXECUTABLE} ${TOOLS}/score_functions.py --corpus ${CORPUS_DIR} --baseline eh_frame
                   --form stripped --min-recall 0.95)
  # The binary frontend against the answer keys (docs/plans/binary.md, stage 3 gates).
  set(SCORE_BIN ${Python3_EXECUTABLE} ${TOOLS}/score_binary.py --optum $<TARGET_FILE:optum> --corpus ${CORPUS_DIR})
  add_test(NAME binary_symbols_and_stripped
           COMMAND ${SCORE_BIN} --forms symbols,stripped --min-func-precision 1 --min-func-recall 1
                   --min-call-precision 0.99 --min-call-recall 0.99 --require-reach)
  add_test(NAME binary_bare
           COMMAND ${SCORE_BIN} --forms bare --min-func-precision 0.95 --min-func-recall 0.90
                   --min-call-precision 0.95 --min-call-recall 0.85 --require-reach)
  # With debug info, every project function gets its source id, and an -O0 build's graph merges
  # node for node with the source graph (docs/decisions/0005-binaries.md).
  add_test(NAME binary_debug
           COMMAND ${SCORE_BIN} --forms debug --min-func-precision 1 --min-func-recall 1 --min-source-ids 1 --require-reach)
  add_test(NAME binary_merges_with_source
           COMMAND ${Python3_EXECUTABLE} ${TOOLS}/check_merge.py --optum $<TARGET_FILE:optum>
                   --source ${FIXTURES}/programs/c --binary ${CORPUS_DIR}/samples/O0/debug/multi --min-functions 9)
  add_test(NAME cli_disasm COMMAND optum disasm ${CORPUS_DIR}/samples/O2/stripped/multi)
  set_tests_properties(cli_disasm PROPERTIES PASS_REGULAR_EXPRESSION "\"to_function\"")
  # Binaries in a repo are untrusted input: mutated ones must never crash the frontend.
  add_test(NAME binary_fuzz
           COMMAND ${Python3_EXECUTABLE} ${TOOLS}/fuzz_elf.py --optum $<TARGET_FILE:optum> --cases 150
                   --seed-binary ${CORPUS_DIR}/samples/O2/symbols/multi
                   --seed-binary ${CORPUS_DIR}/cJSON/O2/symbols/libcjson.so
                   --seed-binary ${CORPUS_DIR}/zlib/O0/bare/minigzip
                   --seed-binary ${CORPUS_DIR}/glue/O0/symbols/http_glue
                   --seed-binary ${CORPUS_DIR}/cJSON/O2/debug/libcjson.so)
  set_tests_properties(corpus_answer_keys corpus_symbols_baseline corpus_eh_frame_baseline
                       binary_symbols_and_stripped binary_bare binary_debug binary_merges_with_source binary_fuzz cli_disasm PROPERTIES
                       FIXTURES_REQUIRED corpus)
  set_tests_properties(binary_fuzz PROPERTIES TIMEOUT 900)
endif()

if(NOT Python3_FOUND OR NO_JSONSCHEMA)
  message(STATUS "Python jsonschema not found: skipping schema tests (pip install jsonschema)")
endif()
