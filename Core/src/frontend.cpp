#include "optum/frontend.hpp"

#include <fnmatch.h>

#include <algorithm>

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
    return x;
  }

 private:
  FrontendInfo info_;
  unsigned threads_;
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
    if (p.substr(0, p.rfind('.')) != kProtocolVersion.substr(0, kProtocolVersion.rfind('.'))) {
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
    const nlohmann::json out = call({"extract"}, req.dump(), "extract", &x.diagnostics);
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
      fail(std::string(what) + ": exited with status " + std::to_string(r.exit_code) +
           (r.err.empty() ? "" : "; last output:\n" + tail(r.err, 5)));
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

}  // namespace optum
