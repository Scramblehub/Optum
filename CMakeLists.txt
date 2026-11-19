cmake_minimum_required(VERSION 3.25)
project(optum VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(OPTUM_BUILD_TESTS "Build the test suite" ON)
option(OPTUM_SANITIZE "Build with AddressSanitizer and UBSan" OFF)

if(OPTUM_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Third-party code, pinned as git submodules (see External_Libraries/README.md).
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/External_Libraries/json/CMakeLists.txt")
  message(FATAL_ERROR "Submodules missing. Run: git submodule update --init --depth 1")
endif()
set(JSON_BuildTests OFF CACHE INTERNAL "")
add_subdirectory(External_Libraries/json EXCLUDE_FROM_ALL)
add_subdirectory(External_Libraries/CLI11 EXCLUDE_FROM_ALL)

# tree-sitter runtime and grammars: plain C, built here rather than through their own build files.
set(TS_DIR ${CMAKE_SOURCE_DIR}/External_Libraries/tree-sitter/lib)
add_library(tree_sitter STATIC ${TS_DIR}/src/lib.c)
target_include_directories(tree_sitter PUBLIC ${TS_DIR}/include PRIVATE ${TS_DIR}/src)
target_compile_definitions(tree_sitter PRIVATE _POSIX_C_SOURCE=200112L _DEFAULT_SOURCE)
set_target_properties(tree_sitter PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)

set(TS_PY_DIR ${CMAKE_SOURCE_DIR}/External_Libraries/tree-sitter-python/src)
add_library(tree_sitter_python STATIC ${TS_PY_DIR}/parser.c ${TS_PY_DIR}/scanner.c)
target_include_directories(tree_sitter_python PRIVATE ${TS_PY_DIR})
set_target_properties(tree_sitter_python PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)

set(TS_JS_DIR ${CMAKE_SOURCE_DIR}/External_Libraries/tree-sitter-javascript/src)
add_library(tree_sitter_javascript STATIC ${TS_JS_DIR}/parser.c ${TS_JS_DIR}/scanner.c)
target_include_directories(tree_sitter_javascript PRIVATE ${TS_JS_DIR})
set(TS_TS_DIR ${CMAKE_SOURCE_DIR}/External_Libraries/tree-sitter-typescript)
add_library(tree_sitter_typescript STATIC
  ${TS_TS_DIR}/typescript/src/parser.c ${TS_TS_DIR}/typescript/src/scanner.c
  ${TS_TS_DIR}/tsx/src/parser.c ${TS_TS_DIR}/tsx/src/scanner.c)
target_include_directories(tree_sitter_typescript PRIVATE ${TS_TS_DIR}/typescript/src)
set(TS_C_DIR ${CMAKE_SOURCE_DIR}/External_Libraries/tree-sitter-c/src)
add_library(tree_sitter_c STATIC ${TS_C_DIR}/parser.c)
target_include_directories(tree_sitter_c PRIVATE ${TS_C_DIR})
set_target_properties(tree_sitter_javascript tree_sitter_typescript tree_sitter_c PROPERTIES
  C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)

add_subdirectory(Core)

if(OPTUM_BUILD_TESTS)
  add_subdirectory(External_Libraries/Catch2 EXCLUDE_FROM_ALL)
  enable_testing()
  add_subdirectory(tests)
endif()
