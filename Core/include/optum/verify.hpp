#pragma once
// Checking a static graph against what actually ran: a trace of observed caller -> callee pairs
// (written by the tracers in Core/tracers/) is compared with the graph's call edges.
//
// Trace format (JSON): {"trace_version": "0.1", "source": "...",
//                       "calls": [{"from": "<node id>", "to": "<node id>", "count": 3}, ...]}

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "optum/ir.hpp"

namespace optum {

struct VerifyReport {
  std::size_t observed = 0;          // distinct observed calls
  std::size_t found_exact = 0;       // in the graph as an exact edge
  std::size_t found_inferred = 0;    // in the graph as an inferred edge
  // Missed calls, by why: the caller or the callee isn't a node, or both are and the edge is missing.
  std::vector<std::string> caller_missing, callee_missing, edge_missing;
  double recall() const {
    return observed ? double(found_exact + found_inferred) / double(observed) : 1.0;
  }
};

// Throws IrError if the trace is malformed or of another version.
VerifyReport verify(const Graph& g, const nlohmann::json& trace);
nlohmann::json to_json(const VerifyReport& r);
std::string to_text(const VerifyReport& r, std::size_t max_listed = 20);

}  // namespace optum
