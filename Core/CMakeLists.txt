set(OPTUM_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion)

configure_file(src/version.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/version.cpp @ONLY)

# The core library: everything except the command-line front door.
add_library(optum_core
  ${CMAKE_CURRENT_BINARY_DIR}/version.cpp
  src/graph_json.cpp
)
target_include_directories(optum_core PUBLIC include)
target_link_libraries(optum_core PUBLIC nlohmann_json::nlohmann_json)
target_compile_options(optum_core PRIVATE ${OPTUM_WARNINGS})

add_executable(optum cli/main.cpp)
target_link_libraries(optum PRIVATE optum_core CLI11::CLI11)
target_compile_options(optum PRIVATE ${OPTUM_WARNINGS})
