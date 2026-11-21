#pragma once
// optum infer: a first behaviour spec for each tool, learned from its recorded traffic
// (docs/plans/phase3.md, "Learning a tool we can't read"). Everything is evidence from traffic: a
// call counted, a field seen, a status change watched. Nothing is read from a tool's source.
//
// Input: the recording proxy's JSON lines (testco/observe/record.py), one file per tool.

#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

namespace optum {

// /wp-json/wc/v3/orders/2026 -> /wp-json/wc/v3/orders/{}: numbers, uuids, long hex ids and
// Stripe-style ids (ch_Rk0oMioJB0GZRVq) fold to {}.
std::string template_path(std::string_view path);

// A spec for one tool from its recordings (harness records must already be left out): operations
// (method and endpoint, or a JSON-RPC model and method) with calls, clients, statuses, the
// endpoint, the status values it sets, fields sent and received; resources and their fields; and
// status changes per resource. Key order is kept (operations by calls, most first), so the output
// reads the same every run.
nlohmann::ordered_json learn_spec(const std::vector<nlohmann::json>& records);

struct InferOptions {
  std::filesystem::path recordings;   // folder of <tool>.jsonl
  std::filesystem::path n8n;          // optional: the automation tool's exported flows
  std::set<std::string> tools;        // only these tools (empty: all)
  std::set<std::string> clients;      // only calls from these clients (empty: all)
};

struct InferResult {
  std::vector<std::pair<std::string, nlohmann::ordered_json>> specs;  // tool -> spec, by tool name
  nlohmann::ordered_json observed;  // {"window": [...], "findings": [...]}: flows that never ran
  std::string markdown;             // the specs, for people
  std::size_t calls = 0, harness = 0;
};

// The recordings by tool, harness calls left out (counted in *harness) and the filters applied.
std::map<std::string, std::vector<nlohmann::json>> load_recordings(const InferOptions& opts,
                                                                   std::size_t* harness = nullptr);

// Reads every recording, leaves out Optum's own harness calls, and learns a spec per tool.
// Throws IrError on unreadable files or malformed lines (with the file and line number).
InferResult infer(const InferOptions& opts);

}  // namespace optum
