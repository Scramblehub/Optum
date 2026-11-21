#pragma once
// Frontends turn files into graphs. Built-in ones run in-process; external adapters are any
// program speaking the protocol in docs/protocol.md. Both look the same to the rest of Optum.

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "optum/ir.hpp"

namespace optum {

inline constexpr std::string_view kProtocolVersion = "optum-frontend/0.1";

struct FrontendInfo {
  std::string name;
  std::string version;
  std::vector<std::string> file_patterns;  // globs on the file name ("*.py") or relative path ("flows/*.json")
  std::string command;                     // external adapters only
};

struct Extraction {
  Graph graph;
  std::string summary;  // one line for people, e.g. "4 files, 120 lines"
  std::string diagnostics;  // anything the frontend reported (an adapter's stderr)
};

class Frontend {
 public:
  virtual ~Frontend() = default;
  virtual const FrontendInfo& info() const = 0;
  // `files` are relative to `root`, already filtered by the frontend's patterns.
  virtual Extraction extract(const std::filesystem::path& root, const std::vector<std::filesystem::path>& files) = 0;
};

std::unique_ptr<Frontend> make_python_frontend(unsigned threads = 0);
std::unique_ptr<Frontend> make_js_frontend(unsigned threads = 0);
// `compile_commands`: see extract_c_files ("" finds one at the root, "none" ignores any).
std::unique_ptr<Frontend> make_c_frontend(unsigned threads = 0, const std::string& compile_commands = "");
std::unique_ptr<Frontend> make_binary_frontend();

// Starts the adapter once with `describe` to learn its name and patterns. Throws IrError with
// the adapter named if it can't be run or answers badly.
std::unique_ptr<Frontend> make_external_frontend(const std::string& command,
                                                 std::chrono::milliseconds timeout);

struct Discovery {
  std::filesystem::path root;
  std::vector<std::filesystem::path> files;  // relative to root, sorted
};

// Every regular file under `path` (or `path` itself), relative to the returned root, sorted.
// Skips hidden directories, __pycache__, node_modules, venv and site-packages.
Discovery discover(const std::filesystem::path& path);

bool matches_any(const std::vector<std::string>& patterns, const std::filesystem::path& rel);

// For the unused-code analysis: names the project's binaries carry as strings (a C extension
// looking up a Python helper by name, as numpy and pyuno do) mark the local, unreferenced
// definitions with that name as referenced (attrs.referenced_from: "binary"). `files` are relative
// to `root`; only ELF files are read.
void mark_names_in_binaries(Graph& g, const std::filesystem::path& root,
                            const std::vector<std::filesystem::path>& files);

}  // namespace optum
