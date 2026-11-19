#pragma once
// optum diagnose: what a graph says about waste. Plan: docs/plans/phase2.md; report: docs/diagnose.md.
//
// A report has an inventory (what the code reaches outside itself) and findings. Each finding has
// a kind, a confidence (certain: exact edges only; likely: some inferred edge involved) and its
// evidence. Analyses add finding kinds; the first is overlapping glue.

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "optum/ir.hpp"

namespace optum {

// Who reaches something outside. The unit is a component: a top-level package or directory, a
// root-level file, a program, a workflow or a binary: what a person would call one tool.
struct Use {
  std::string unit;      // the component (py:pkg, js:src, n8n:wf-1, bin:app)
  std::string function;  // the node that reaches the target (may equal the unit)
  std::string kind;      // reads, writes, sends_to
  std::string method;    // HTTP method, if any
  bool exact = true;
};

struct Target {  // something outside the code: an API, a SaaS service, a table, a library
  std::string id, kind, name;
  std::vector<Use> uses;
};

struct Finding {
  std::string kind;        // "overlapping_glue", "duplicated_logic"
  std::string title;
  std::string confidence;  // certain, likely
  std::vector<std::string> units;    // the units involved, sorted
  std::vector<std::string> targets;  // the shared targets, sorted
  nlohmann::json evidence = nlohmann::json::object();
};

struct UnitInfo {
  std::string id, name, frontend;
  std::uint32_t lines = 0;  // 0 if unknown
};

struct Diagnosis {
  std::string source;
  std::vector<UnitInfo> units;
  std::vector<Target> reached;              // APIs, services and tables, with who uses them
  std::vector<std::string> packages;        // third-party packages and libraries imported
  std::vector<Finding> findings;
  std::vector<std::string> notes;  // what analyses left out on purpose, and why
};

// `source_root`: the analysed directory, for analyses that read source (duplicated logic). Empty
// or missing: those analyses are skipped.
Diagnosis diagnose(const Graph& g, const std::filesystem::path& source_root = {});
nlohmann::json to_json(const Diagnosis& d);
std::string to_markdown(const Diagnosis& d);

}  // namespace optum
