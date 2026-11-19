#include <CLI/CLI.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>

#include "optum/export.hpp"
#include "optum/frontend.hpp"
#include "optum/frontends/binary.hpp"
#include "optum/ir.hpp"
#include "optum/score.hpp"
#include "optum/syntax_tree.hpp"
#include "optum/version.hpp"
#include "optum/view.hpp"

namespace {

int write_json(const nlohmann::json& j, const std::string& out_path, int indent) {
  if (out_path.empty() || out_path == "-") {
    std::cout << optum::dump_json(j, indent) << "\n";
    return 0;
  }
  std::ofstream out(out_path);
  if (!out) {
    std::cerr << "optum: cannot write " << out_path << "\n";
    return 2;
  }
  out << optum::dump_json(j, indent) << "\n";
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

std::vector<std::unique_ptr<optum::Frontend>> make_frontends(const std::vector<std::string>& adapters,
                                                            unsigned threads, int timeout_s) {
  std::vector<std::unique_ptr<optum::Frontend>> out;
  out.push_back(optum::make_python_frontend(threads));
  out.push_back(optum::make_js_frontend(threads));
  out.push_back(optum::make_c_frontend(threads));
  out.push_back(optum::make_binary_frontend());
  for (const auto& cmd : adapters) {
    out.push_back(optum::make_external_frontend(cmd, std::chrono::seconds(timeout_s)));
  }
  return out;
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
  std::vector<std::string> adapters;
  int adapter_timeout = 300;
  auto add_adapter_options = [&](CLI::App* cmd) {
    cmd->add_option("--adapter", adapters, "External frontend command (repeatable); see docs/protocol.md");
    cmd->add_option("--adapter-timeout", adapter_timeout, "Seconds an adapter may take per request")
        ->capture_default_str();
  };
  add_adapter_options(graph);

  std::string list_path;
  auto* frontends_cmd = app.add_subcommand("frontends", "List frontends and which files each would read");
  frontends_cmd->add_option("path", list_path, "Optional: show how many files each would read here");
  add_adapter_options(frontends_cmd);

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

  std::string disasm_path;
  auto* disasm_cmd = app.add_subcommand("disasm", "Research aid: an ELF binary's functions and instructions, as JSON");
  disasm_cmd->add_option("binary", disasm_path, "x86-64 ELF executable or shared library")->required();
  disasm_cmd->add_option("-o,--out", out_path, "Output file (default: stdout)");

  std::string tree_path;
  auto* tree_cmd = app.add_subcommand("syntax-tree", "Developer aid: print a file's tree-sitter syntax tree");
  tree_cmd->add_option("file", tree_path, ".py, .js, .ts, .tsx, .c or .h file")->required();

  CLI11_PARSE(app, argc, argv);

  try {
    if (*graph) {
      if (!std::filesystem::exists(path)) {
        std::cerr << "optum: no such file or directory: " << path << "\n";
        return 2;
      }
      const auto frontends = make_frontends(adapters, threads, adapter_timeout);
      const optum::Discovery d = optum::discover(path);
      optum::Graph g;
      g.source = path;
      for (const auto& fe : frontends) {
        std::vector<std::filesystem::path> mine;
        for (const auto& f : d.files) {
          if (optum::matches_any(fe->info().file_patterns, f)) mine.push_back(f);
        }
        if (mine.empty()) continue;
        const auto start = std::chrono::steady_clock::now();
        optum::Extraction x = fe->extract(d.root, mine);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start).count();
        if (!quiet) {
          for (std::size_t pos = 0; pos < x.diagnostics.size();) {
            const auto nl = x.diagnostics.find('\n', pos);
            const auto line = x.diagnostics.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            if (!line.empty()) std::cerr << "[" << fe->info().name << "] " << line << "\n";
            if (nl == std::string::npos) break;
            pos = nl + 1;
          }
          std::cerr << fe->info().name << ": " << x.summary << " -> " << x.graph.nodes().size()
                    << " nodes, " << x.graph.edges().size() << " edges in " << ms << " ms\n";
        }
        g.merge(x.graph);
      }
      if (!quiet && frontends.size() > 1) {
        std::cerr << "merged: " << g.nodes().size() << " nodes, " << g.edges().size() << " edges\n";
      }
      return emit(g, view, out_path, indent);
    }
    if (*disasm_cmd) return write_json(optum::disassemble_binary(disasm_path), out_path, -1);
    if (*tree_cmd) {
      std::cout << optum::syntax_tree(tree_path);
      return 0;
    }
    if (*frontends_cmd) {
      const auto frontends = make_frontends(adapters, 0, adapter_timeout);
      std::optional<optum::Discovery> d;
      if (!list_path.empty()) d = optum::discover(list_path);
      for (const auto& fe : frontends) {
        const auto& i = fe->info();
        std::cout << i.name << (i.version.empty() ? "" : " " + i.version) << "  ";
        for (std::size_t k = 0; k < i.file_patterns.size(); ++k) std::cout << (k ? "," : "") << i.file_patterns[k];
        if (d) {
          std::size_t n = 0;
          for (const auto& f : d->files) n += optum::matches_any(i.file_patterns, f) ? 1 : 0;
          std::cout << "  (" << n << " files)";
        }
        if (!i.command.empty()) std::cout << "  [" << i.command << "]";
        std::cout << "\n";
      }
      return 0;
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
      if (score_json) std::cout << optum::dump_json(optum::to_json(report), indent) << "\n";
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
