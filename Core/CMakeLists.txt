set(OPTUM_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion)

configure_file(src/version.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/version.cpp @ONLY)

# The core library: everything except the command-line front door.
add_library(optum_core
  ${CMAKE_CURRENT_BINARY_DIR}/version.cpp
  src/ir.cpp
  src/ir_json.cpp
  src/score.cpp
  src/sql.cpp
  src/http.cpp
  src/view.cpp
  src/export.cpp
  src/process.cpp
  src/frontend.cpp
  src/syntax_tree.cpp
  src/frontends/python/python_frontend.cpp
  src/frontends/js/js_frontend.cpp
  src/frontends/c/c_frontend.cpp
  src/frontends/binary/binary_frontend.cpp
  src/elf.cpp
)
target_include_directories(optum_core PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(optum_core PUBLIC nlohmann_json::nlohmann_json
                      PRIVATE tree_sitter tree_sitter_python tree_sitter_javascript
                              tree_sitter_typescript tree_sitter_c capstone Threads::Threads)
target_compile_options(optum_core PRIVATE ${OPTUM_WARNINGS})

add_executable(optum cli/main.cpp)
target_link_libraries(optum PRIVATE optum_core CLI11::CLI11)
target_compile_options(optum PRIVATE ${OPTUM_WARNINGS})
