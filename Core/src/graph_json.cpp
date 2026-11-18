#include "optum/graph_json.hpp"

#include "optum/version.hpp"

namespace optum {

nlohmann::json empty_graph(const std::string& source) {
  return {
      {"ir_version", std::string(kIrVersion)},
      {"source", source},
      {"nodes", nlohmann::json::array()},
      {"edges", nlohmann::json::array()},
  };
}

}  // namespace optum
