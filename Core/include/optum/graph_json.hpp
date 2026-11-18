#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace optum {

// Placeholder until feat/ir-core: an empty graph document in the versioned format.
nlohmann::json empty_graph(const std::string& source);

}  // namespace optum
