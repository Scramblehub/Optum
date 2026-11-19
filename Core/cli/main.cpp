#include <CLI/CLI.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "optum/ir.hpp"
#include "optum/version.hpp"

namespace {

int write_json(const nlohmann::json& j, const std::string& out_path, int indent) {
  if (out_path.empty() || out_path == "-") {
    std::cout << j.dump(indent) << "\n";
    return 0;
  }
  std::ofstream out(out_path);
  if (!out) {
    std::cerr << "optum: cannot write " << out_path << "\n";
    return 2;
  }
  out << j.dump(indent) << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"Optum: break software down into one graph"};
  app.set_version_flag("--version", std::string(optum::version()));
  app.require_subcommand(1);

  int indent = 2;
  std::string out_path;

  std::string path;
  auto* graph = app.add_subcommand("graph", "Break a file or directory down into a graph (JSON)");
  graph->add_option("path", path, "File or directory to read")->required();
  graph->add_option("-o,--out", out_path, "Output file (default: stdout)");
  graph->add_option("--indent", indent, "JSON indent; -1 for compact")->capture_default_str();

  std::vector<std::string> check_files;
  auto* check = app.add_subcommand("check", "Validate graph JSON files against the IR format");
  check->add_option("files", check_files, "Graph JSON files")->required();

  std::vector<std::string> merge_files;
  auto* merge = app.add_subcommand("merge", "Merge graph JSON files into one graph");
  merge->add_option("files", merge_files, "Graph JSON files")->required();
  merge->add_option("-o,--out", out_path, "Output file (default: stdout)");
  merge->add_option("--indent", indent, "JSON indent; -1 for compact")->capture_default_str();

  CLI11_PARSE(app, argc, argv);

  try {
    if (*graph) {
      if (!std::filesystem::exists(path)) {
        std::cerr << "optum: no such file or directory: " << path << "\n";
        return 2;
      }
      // No frontends yet: an empty graph, so the pipeline exists end to end.
      optum::Graph g;
      g.source = path;
      return write_json(optum::to_json(g), out_path, indent);
    }
    if (*check) {
      int failures = 0;
      for (const auto& f : check_files) {
        try {
          const optum::Graph g = optum::load_graph(f);
          std::cout << f << ": ok, " << g.nodes().size() << " nodes, " << g.edges().size()
                    << " edges\n";
        } catch (const optum::IrError& e) {
          std::cerr << e.what() << "\n";
          ++failures;
        }
      }
      return failures ? 1 : 0;
    }
    if (*merge) {
      optum::Graph merged;
      for (const auto& f : merge_files) merged.merge(optum::load_graph(f));
      merged.source = merge_files.size() == 1 ? merge_files[0] : "merged";
      return write_json(optum::to_json(merged), out_path, indent);
    }
  } catch (const optum::IrError& e) {
    std::cerr << "optum: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
