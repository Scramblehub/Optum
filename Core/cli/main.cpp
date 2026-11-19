#include <CLI/CLI.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "optum/export.hpp"
#include "optum/frontends/python.hpp"
#include "optum/ir.hpp"
#include "optum/score.hpp"
#include "optum/version.hpp"
#include "optum/view.hpp"

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

int write_text(const std::string& text, const std::string& out_path) {
  if (out_path.empty() || out_path == "-") {
    std::cout << text;
    return 0;
  }
  std::ofstream out(out_path);
  if (!out) {
    std::cerr << "optum: cannot write " << out_path << "\n";
    return 2;
  }
  out << text;
  return 0;
}

std::string shell_quote(const std::string& s) {
  std::string q = "'";
  for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return q + "'";
}

// SVG through Graphviz's `dot`, which must be installed.
int write_svg(const std::string& dot, const std::string& out_path) {
  if (std::system("command -v dot > /dev/null 2>&1") != 0) {
    std::cerr << "optum: --format svg needs Graphviz ('dot' not found). Install graphviz, or use "
                 "--format dot and render it elsewhere.\n";
    return 2;
  }
  std::string cmd = "dot -Tsvg";
  if (!out_path.empty() && out_path != "-") cmd += " -o " + shell_quote(out_path);
  FILE* pipe = popen(cmd.c_str(), "w");
  if (!pipe) {
    std::cerr << "optum: cannot run dot\n";
    return 2;
  }
  std::fwrite(dot.data(), 1, dot.size(), pipe);
  const int status = pclose(pipe);
  if (status != 0) {
    std::cerr << "optum: dot failed (status " << status << ")\n";
    return 2;
  }
  return 0;
}

// Options shared by `graph` and `export`: which part of the graph, and in what format.
struct ViewCli {
  std::string format = "json";
  std::string level = "function";
  std::string focus;
  int depth = 1;
  std::vector<std::string> edges;
  bool no_inferred = false;
  std::size_t max_nodes = 0;
};

void add_view_options(CLI::App* cmd, ViewCli& v) {
  cmd->add_option("-f,--format", v.format, "json, dot, svg or mermaid")
      ->check(CLI::IsMember({"json", "dot", "svg", "mermaid"}))
      ->capture_default_str();
  cmd->add_option("--level", v.level, "function, or module to collapse into modules")
      ->check(CLI::IsMember({"function", "module"}))
      ->capture_default_str();
  cmd->add_option("--focus", v.focus, "Only this node (id or unique id ending) and its neighbours");
  cmd->add_option("--depth", v.depth, "With --focus: how many edges away to include")->capture_default_str();
  cmd->add_option("--edges", v.edges, "Only these edge kinds, e.g. --edges calls,reads")->delimiter(',');
  cmd->add_flag("--no-inferred", v.no_inferred, "Leave out inferred (guessed) edges");
  cmd->add_option("--max-nodes", v.max_nodes,
                  "Refuse diagrams larger than this (default 1500 for dot/svg, 300 for mermaid)");
}

int emit(const optum::Graph& g, const ViewCli& v, const std::string& out_path, int indent) {
  optum::ViewOptions o;
  o.level = v.level == "module" ? optum::Level::Module : optum::Level::Function;
  o.focus = v.focus;
  o.depth = v.depth;
  o.hide_inferred = v.no_inferred;
  for (const auto& k : v.edges) {
    const auto kind = optum::edge_kind_from_string(k);
    if (!kind) throw optum::IrError("unknown edge kind '" + k + "' in --edges");
    o.edge_kinds.push_back(*kind);
  }
  const optum::Graph view = o.is_identity() ? optum::Graph{} : optum::make_view(g, o);
  const optum::Graph& shown = o.is_identity() ? g : view;

  if (v.format == "json") return write_json(optum::to_json(shown), out_path, indent);
  const std::size_t limit = v.max_nodes ? v.max_nodes : v.format == "mermaid" ? 300 : 1500;
  if (shown.nodes().size() > limit) {
    std::cerr << "optum: " << shown.nodes().size() << " nodes is too many for a readable diagram "
              << "(limit " << limit << "). Try --level module, --focus <node>, --edges, "
              << "or raise --max-nodes.\n";
    return 2;
  }
  if (v.format == "mermaid") return write_text(optum::to_mermaid(shown), out_path);
  const std::string dot = optum::to_dot(shown);
  return v.format == "dot" ? write_text(dot, out_path) : write_svg(dot, out_path);
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"Optum: break software down into one graph"};
  app.set_version_flag("--version", std::string(optum::version()));
  app.require_subcommand(1);

  int indent = 2;
  std::string out_path;

  std::string path;
  ViewCli view;
  auto* graph = app.add_subcommand("graph", "Break a file or directory down into a graph");
  graph->add_option("path", path, "File or directory to read")->required();
  graph->add_option("-o,--out", out_path, "Output file (default: stdout)");
  graph->add_option("--indent", indent, "JSON indent; -1 for compact")->capture_default_str();
  unsigned threads = 0;
  bool quiet = false;
  graph->add_option("-j,--threads", threads, "Worker threads; 0 = all cores")->capture_default_str();
  graph->add_flag("-q,--quiet", quiet, "Don't print statistics to stderr");
  add_view_options(graph, view);

  std::string export_path;
  auto* exp = app.add_subcommand("export", "Turn a graph JSON file into a diagram or a smaller graph");
  exp->add_option("graph", export_path, "Graph JSON file")->required();
  exp->add_option("-o,--out", out_path, "Output file (default: stdout)");
  exp->add_option("--indent", indent, "JSON indent; -1 for compact")->capture_default_str();
  ViewCli export_view;
  export_view.format = "svg";
  add_view_options(exp, export_view);

  std::vector<std::string> check_files;
  auto* check = app.add_subcommand("check", "Validate graph JSON files against the IR format");
  check->add_option("files", check_files, "Graph JSON files")->required();

  std::vector<std::string> merge_files;
  auto* merge = app.add_subcommand("merge", "Merge graph JSON files into one graph");
  merge->add_option("files", merge_files, "Graph JSON files")->required();
  merge->add_option("-o,--out", out_path, "Output file (default: stdout)");
  merge->add_option("--indent", indent, "JSON indent; -1 for compact")->capture_default_str();

  std::string expected_path, produced_path;
  bool score_json = false;
  double min_precision = 0.0, min_recall = 0.0;
  auto* score = app.add_subcommand("score", "Compare a produced graph against an expected graph");
  score->add_option("expected", expected_path, "Hand-checked expected graph")->required();
  score->add_option("produced", produced_path, "Graph produced by a frontend")->required();
  score->add_flag("--json", score_json, "Print the report as JSON");
  score->add_option("--min-precision", min_precision, "Fail (exit 1) below this edge precision, 0-1");
  score->add_option("--min-recall", min_recall, "Fail (exit 1) below this edge recall, 0-1");

  CLI11_PARSE(app, argc, argv);

  try {
    if (*graph) {
      if (!std::filesystem::exists(path)) {
        std::cerr << "optum: no such file or directory: " << path << "\n";
        return 2;
      }
      const auto start = std::chrono::steady_clock::now();
      optum::PythonStats stats;
      const optum::Graph g = optum::extract_python(path, &stats, threads);
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - start).count();
      if (!quiet) {
        std::cerr << "python: " << stats.files << " files, " << stats.lines << " lines";
        if (stats.files_with_syntax_errors) {
          std::cerr << " (" << stats.files_with_syntax_errors << " with syntax errors)";
        }
        std::cerr << " -> " << g.nodes().size() << " nodes, " << g.edges().size() << " edges in "
                  << ms << " ms\n";
      }
      return emit(g, view, out_path, indent);
    }
    if (*exp) return emit(optum::load_graph(export_path), export_view, out_path, indent);
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
    if (*score) {
      const auto report = optum::score(optum::load_graph(expected_path),
                                       optum::load_graph(produced_path));
      if (score_json) std::cout << optum::to_json(report).dump(indent) << "\n";
      else std::cout << optum::to_text(report);
      const bool pass = report.edges.precision() >= min_precision &&
                        report.edges.recall() >= min_recall;
      return pass ? 0 : 1;
    }
  } catch (const optum::IrError& e) {
    std::cerr << "optum: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
