#include "optum/frontend.hpp"

#include <fnmatch.h>
#include <string.h>

#include <algorithm>
#include <unordered_map>

#include "optum/elf.hpp"
#include "optum/frontends/binary.hpp"
#include "optum/frontends/c.hpp"
#include "optum/frontends/js.hpp"
#include "optum/frontends/python.hpp"
#include "optum/process.hpp"

namespace optum {
namespace fs = std::filesystem;

namespace {

bool skip_dir(const fs::path& p) {
  const std::string name = p.filename().string();
  return (name.starts_with(".") && name != "." && name != "..") || name == "__pycache__" ||
         name == "node_modules" || name == "venv" || name == "site-packages";
}

// Unreadable files are analysed as empty modules; say so in the summary and name each one.
void report_unreadable(const std::vector<std::string>& files, Extraction& x) {
  if (files.empty()) return;
  x.summary += " (" + std::to_string(files.size()) + " unreadable)";
  for (const auto& f : files) x.diagnostics += "cannot read " + f + " (permissions?); treated as empty\n";
}

// ---------------------------------------------------------------- built-in Python

class PythonFrontend : public Frontend {
 public:
  explicit PythonFrontend(unsigned threads) : threads_(threads) {
    info_.name = "python";
    info_.version = "builtin";
    info_.file_patterns = {"*.py"};
  }
  const FrontendInfo& info() const override { return info_; }
  Extraction extract(const fs::path& root, const std::vector<fs::path>& files) override {
    PythonStats stats;
    Extraction x;
    x.graph = extract_python_files(root, files, &stats, threads_);
    x.summary = std::to_string(stats.files) + " files, " + std::to_string(stats.lines) + " lines";
    if (stats.files_with_syntax_errors) {
      x.summary += " (" + std::to_string(stats.files_with_syntax_errors) + " with syntax errors)";
    }
    report_unreadable(stats.unreadable, x);
    return x;
  }

 private:
  FrontendInfo info_;
  unsigned threads_;
};

class JsFrontend : public Frontend {
 public:
  explicit JsFrontend(unsigned threads) : threads_(threads) {
    info_.name = "js";
    info_.version = "builtin";
    info_.file_patterns = {"*.js", "*.jsx", "*.mjs", "*.cjs", "*.ts", "*.tsx", "*.mts", "*.cts"};
  }
  const FrontendInfo& info() const override { return info_; }
  Extraction extract(const fs::path& root, const std::vector<fs::path>& files) override {
    JsStats stats;
    Extraction x;
    x.graph = extract_js_files(root, files, &stats, threads_);
    x.summary = std::to_string(stats.files) + " files, " + std::to_string(stats.lines) + " lines";
    if (stats.files_with_syntax_errors) {
      x.summary += " (" + std::to_string(stats.files_with_syntax_errors) + " with syntax errors)";
    }
    report_unreadable(stats.unreadable, x);
    return x;
  }

 private:
  FrontendInfo info_;
  unsigned threads_;
};

class CFrontend : public Frontend {
 public:
  CFrontend(unsigned threads, std::string compile_commands)
      : threads_(threads), compile_commands_(std::move(compile_commands)) {
    info_.name = "c";
    info_.version = "builtin";
    info_.file_patterns = {"*.c", "*.h"};
  }
  const FrontendInfo& info() const override { return info_; }
  Extraction extract(const fs::path& root, const std::vector<fs::path>& files) override {
    CStats stats;
    Extraction x;
    x.graph = extract_c_files(root, files, &stats, threads_, compile_commands_);
    x.summary = std::to_string(stats.files) + " files, " + std::to_string(stats.lines) + " lines";
    if (stats.files_with_syntax_errors) {
      x.summary += " (" + std::to_string(stats.files_with_syntax_errors) + " with syntax errors)";
    }
    if (stats.build_database) {
      x.summary += "; build configuration from compile_commands.json (" + std::to_string(stats.build_files) +
                   " files, " + std::to_string(stats.external_headers) + " generated headers)";
    } else if (stats.assumed_off_names) {
      x.diagnostics += "conditions on " + std::to_string(stats.assumed_off_names) +
                       " names defined by no visible file were assumed off: " + std::to_string(stats.assumed_off_edges) +
                       " edges conditional; pass --compile-commands for the build's own configuration\n";
    }
    report_unreadable(stats.unreadable, x);
    return x;
  }

 private:
  FrontendInfo info_;
  unsigned threads_;
  std::string compile_commands_;
};

// Binaries are found by their content (the ELF magic), not their name: every file is offered.
class BinaryFrontend : public Frontend {
 public:
  BinaryFrontend() {
    info_.name = "binary";
    info_.version = "builtin";
    info_.file_patterns = {"*"};
  }
  const FrontendInfo& info() const override { return info_; }
  Extraction extract(const fs::path& root, const std::vector<fs::path>& files) override {
    BinaryStats stats;
    Extraction x;
    x.graph = extract_binary_files(root, files, &stats, &x.diagnostics);
    x.summary = std::to_string(stats.files) + " ELF files, " + std::to_string(stats.functions) + " functions, " +
                std::to_string(stats.instructions) + " instructions";
    if (stats.skipped) x.summary += " (" + std::to_string(stats.skipped) + " skipped)";
    return x;
  }

 private:
  FrontendInfo info_;
};

// ---------------------------------------------------------------- external adapters

std::string tail(const std::string& s, std::size_t max_lines) {
  std::size_t pos = s.size(), lines = 0;
  while (pos > 0 && lines <= max_lines) {
    pos = s.rfind('\n', pos - 1);
    if (pos == std::string::npos) return s;
    ++lines;
  }
  return s.substr(pos + 1);
}

// "optum-frontend/0.1.3" -> "optum-frontend/0.1": adapters may differ in patch, nothing else.
// A version with fewer than two dot-separated parts is returned whole.
std::string_view protocol_major_minor(std::string_view v) {
  const auto first = v.find('.');
  if (first == std::string_view::npos) return v;
  const auto second = v.find('.', first + 1);
  return second == std::string_view::npos ? v : v.substr(0, second);
}

class ExternalFrontend : public Frontend {
 public:
  ExternalFrontend(const std::string& command, std::chrono::milliseconds timeout)
      : argv_(split_command(command)), timeout_(timeout) {
    info_.command = command;
    if (argv_.empty()) throw IrError("adapter command is empty");
    const nlohmann::json d = call({"describe"}, "", "describe");
    auto field = [&](const char* key) -> const nlohmann::json& {
      if (!d.contains(key)) fail("describe: missing field '" + std::string(key) + "'");
      return d.at(key);
    };
    const nlohmann::json& proto = field("protocol");
    if (!proto.is_string()) fail("describe: 'protocol' must be a string");
    const std::string p = proto.get<std::string>();
    if (protocol_major_minor(p) != protocol_major_minor(kProtocolVersion)) {
      fail("speaks protocol '" + p + "', this build speaks " + std::string(kProtocolVersion));
    }
    const nlohmann::json& name = field("name");
    if (!name.is_string() || name.get<std::string>().empty()) fail("describe: 'name' must be a non-empty string");
    info_.name = name.get<std::string>();
    if (d.contains("version") && d["version"].is_string()) info_.version = d["version"];
    const nlohmann::json& pats = field("file_patterns");
    if (!pats.is_array() || pats.empty()) fail("describe: 'file_patterns' must be a non-empty array");
    for (const auto& pat : pats) {
      if (!pat.is_string()) fail("describe: 'file_patterns' must hold strings");
      info_.file_patterns.push_back(pat.get<std::string>());
    }
  }

  const FrontendInfo& info() const override { return info_; }

  Extraction extract(const fs::path& root, const std::vector<fs::path>& files) override {
    nlohmann::json req = {{"protocol", std::string(kProtocolVersion)},
                          {"root", fs::absolute(root).lexically_normal().string()},
                          {"files", nlohmann::json::array()}};
    for (const auto& f : files) req["files"].push_back(f.generic_string());
    Extraction x;
    const nlohmann::json out = call({"extract"}, dump_json(req), "extract", &x.diagnostics);
    try {
      x.graph = graph_from_json(out);
    } catch (const IrError& e) {
      fail(std::string("extract: output is not a valid graph: ") + e.what());
    }
    x.summary = std::to_string(files.size()) + " files";
    return x;
  }

 private:
  std::vector<std::string> argv_;
  std::chrono::milliseconds timeout_;
  FrontendInfo info_;

  [[noreturn]] void fail(const std::string& msg) const {
    const std::string who = info_.name.empty() ? info_.command : info_.name;
    throw IrError("adapter '" + who + "': " + msg);
  }

  nlohmann::json call(const std::vector<std::string>& args, const std::string& input, const char* what,
                      std::string* diagnostics = nullptr) const {
    std::vector<std::string> argv = argv_;
    argv.insert(argv.end(), args.begin(), args.end());
    ProcessResult r;
    try {
      r = run_process(argv, input, timeout_);
    } catch (const IrError& e) {
      fail(e.what());
    }
    if (diagnostics) *diagnostics = r.err;
    if (r.timed_out) fail(std::string(what) + ": no answer within " + std::to_string(timeout_.count() / 1000) + " s");
    if (r.exit_code != 0) {
      // A crash has no exit status (it reads -1): name the signal instead.
      const std::string how = r.term_signal ? "killed by signal " + std::to_string(r.term_signal) + " (" +
                                                  std::string(strsignal(r.term_signal)) + ")"
                                            : "exited with status " + std::to_string(r.exit_code);
      fail(std::string(what) + ": " + how + (r.err.empty() ? "" : "; last output:\n" + tail(r.err, 5)));
    }
    try {
      return nlohmann::json::parse(r.out);
    } catch (const nlohmann::json::parse_error& e) {
      fail(std::string(what) + ": stdout is not JSON (" + e.what() + ")");
    }
  }
};

}  // namespace

std::unique_ptr<Frontend> make_python_frontend(unsigned threads) {
  return std::make_unique<PythonFrontend>(threads);
}

std::unique_ptr<Frontend> make_js_frontend(unsigned threads) { return std::make_unique<JsFrontend>(threads); }
std::unique_ptr<Frontend> make_c_frontend(unsigned threads, const std::string& compile_commands) {
  return std::make_unique<CFrontend>(threads, compile_commands);
}
std::unique_ptr<Frontend> make_binary_frontend() { return std::make_unique<BinaryFrontend>(); }

std::unique_ptr<Frontend> make_external_frontend(const std::string& command,
                                                 std::chrono::milliseconds timeout) {
  return std::make_unique<ExternalFrontend>(command, timeout);
}

Discovery discover(const fs::path& path) {
  Discovery d;
  if (fs::is_directory(path)) {
    d.root = path;
    for (auto it = fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied);
         it != fs::recursive_directory_iterator(); ++it) {
      if (it->is_directory() && skip_dir(it->path())) it.disable_recursion_pending();
      else if (it->is_regular_file()) d.files.push_back(fs::relative(it->path(), path));
    }
  } else {
    d.root = path.parent_path().empty() ? fs::path(".") : path.parent_path();
    d.files.push_back(path.filename());
  }
  std::sort(d.files.begin(), d.files.end());
  return d;
}

bool matches_any(const std::vector<std::string>& patterns, const fs::path& rel) {
  const std::string name = rel.filename().string(), full = rel.generic_string();
  return std::any_of(patterns.begin(), patterns.end(), [&](const std::string& p) {
    const std::string& subject = p.find('/') == std::string::npos ? name : full;
    return fnmatch(p.c_str(), subject.c_str(), 0) == 0;
  });
}

void mark_names_in_binaries(Graph& g, const std::filesystem::path& root,
                            const std::vector<std::filesystem::path>& files) {
  std::unordered_map<std::string, std::vector<NodeIndex>> candidates;
  for (NodeIndex i = 0; i < g.nodes().size(); ++i) {
    const Node& n = g.node(i);
    if (n.frontend == "binary" || !n.attrs.value("local", false) || n.attrs.value("referenced", false)) continue;
    if (n.name.size() >= 3) candidates[n.name].push_back(i);
  }
  if (candidates.empty()) return;
  std::vector<NodeIndex> found;
  for (const auto& rel : files) {
    const std::filesystem::path p = root / rel;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec) || !elf::is_elf(p)) continue;
    elf::File f;
    try {
      f = elf::read(p);
    } catch (const IrError&) {
      continue;
    }
    for (const elf::Section& s : f.sections) {
      if (s.type != 1 || s.executable() || !(s.flags & 0x2)) continue;  // allocated PROGBITS data
      const auto bytes = f.bytes(s);
      std::size_t start = 0;
      for (std::size_t i = 0; i <= bytes.size(); ++i) {
        if (i < bytes.size() && bytes[i] != 0) continue;
        if (i - start >= 3 && i - start <= 128) {
          const std::string str(reinterpret_cast<const char*>(bytes.data()) + start, i - start);
          if (auto it = candidates.find(str); it != candidates.end()) {
            found.insert(found.end(), it->second.begin(), it->second.end());
            candidates.erase(it);
          }
        }
        start = i + 1;
      }
    }
    if (candidates.empty()) break;
  }
  for (NodeIndex i : found) {
    Node n = g.node(i);
    n.attrs = {{"referenced", true}, {"referenced_from", "binary"}};
    g.add_node(std::move(n));
  }
}

}  // namespace optum
