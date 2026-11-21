#pragma once
// optum model: what each component does to which data, and whether it could go
// (docs/plans/capabilities.md, items 1-3).
//
// Derived, never mixed with the facts it rests on. The graph (static facts) and the recordings
// (observed facts) are inputs; a deployment file says what runs where (declared facts). Out
// comes, per component: its operations (system, action, resource, read or write, the status it
// sets), the data each system holds and who reads or writes it, and a replaceability class with
// the evidence behind it, the evidence's weakest kind, and the inputs it rests on (by content
// hash), so a conclusion can be found stale when an input changes.

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "optum/diagnose.hpp"
#include "optum/ir.hpp"

namespace optum {

struct ModelInputs {
  const Graph* graph = nullptr;                // optum graph of the company's own code
  const Diagnosis* diagnosis = nullptr;        // diagnose(graph): units and what they reach
  std::filesystem::path source_root;           // where the graph's files are (for hashing)
  std::filesystem::path recordings;            // folder of <tool>.jsonl (optional)
  std::filesystem::path n8n;                   // exported automation flows (optional)
  std::filesystem::path deployment;            // what runs where (required)
};

// Throws IrError on a malformed deployment file or unreadable recordings.
nlohmann::ordered_json build_model(const ModelInputs& in);
std::string model_markdown(const nlohmann::ordered_json& model);

// Re-hashes every input a saved model rests on and lists the conclusions whose inputs changed:
// {"stale": [{"subject", "changed": [paths]}], "fresh": n}.
nlohmann::ordered_json check_model(const nlohmann::ordered_json& model, const std::filesystem::path& base);

}  // namespace optum
