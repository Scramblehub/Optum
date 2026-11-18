cmake_minimum_required(VERSION 3.25)
project(optum VERSION 0.0.1 LANGUAGES CXX)

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

add_subdirectory(Core)

if(OPTUM_BUILD_TESTS)
  add_subdirectory(External_Libraries/Catch2 EXCLUDE_FROM_ALL)
  enable_testing()
  add_subdirectory(tests)
endif()
