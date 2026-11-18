#include <CLI/CLI.hpp>
#include <filesystem>
#include <iostream>

#include "optum/graph_json.hpp"
#include "optum/version.hpp"

int main(int argc, char** argv) {
  CLI::App app{"Optum: break software down into one graph"};
  app.set_version_flag("--version", std::string(optum::version()));
  app.require_subcommand(1);

  std::string path;
  int indent = 2;
  auto* graph = app.add_subcommand("graph", "Break a file or directory down into a graph (JSON)");
  graph->add_option("path", path, "File or directory to read")->required();
  graph->add_option("--indent", indent, "JSON indent; -1 for compact")->capture_default_str();

  CLI11_PARSE(app, argc, argv);

  if (*graph) {
    if (!std::filesystem::exists(path)) {
      std::cerr << "optum: no such file or directory: " << path << "\n";
      return 2;
    }
    // No frontends yet: emits an empty graph so the pipeline exists end to end.
    std::cout << optum::empty_graph(path).dump(indent) << "\n";
  }
  return 0;
}
