#pragma once
#include <string_view>

namespace optum {

// Version of the optum tool itself.
std::string_view version();

// Version of the JSON graph format (the IR) this build reads and writes.
// See docs/decisions/0003-json-graph-format.md.
inline constexpr std::string_view kIrVersion = "0.1.0";

}  // namespace optum
