#pragma once
// Helpers shared by the built-in frontends. Internal to Core.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "optum/ir.hpp"

namespace optum::frontends {

// The syntax walkers recurse once per nesting level; `a + b + c + ...` with thousands of terms
// nests that deep and would overflow the stack. Below this depth nothing is looked at.
inline constexpr int kMaxWalkDepth = 1000;

// Counts the current recursion depth for as long as it lives; `too_deep()` says to stop.
class DepthGuard {
 public:
  explicit DepthGuard(int& depth) : depth_(depth) { ++depth_; }
  ~DepthGuard() { --depth_; }
  DepthGuard(const DepthGuard&) = delete;
  DepthGuard& operator=(const DepthGuard&) = delete;
  bool too_deep() const { return depth_ > kMaxWalkDepth; }

 private:
  int& depth_;
};

// Runs f(0..n-1) on `threads` workers (0 = all cores).
template <typename F>
void parallel_for(std::size_t n, unsigned threads, F&& f) {
  if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
  threads = static_cast<unsigned>(std::min<std::size_t>(threads, std::max<std::size_t>(n, 1)));
  std::atomic<std::size_t> next{0};
  auto worker = [&] {
    for (std::size_t i = next++; i < n; i = next++) f(i);
  };
  std::vector<std::thread> pool;
  for (unsigned t = 1; t < threads; ++t) pool.emplace_back(worker);
  worker();
  for (auto& th : pool) th.join();
}

// The file's bytes. If it can't be read, returns "" and sets *ok = false, so callers can report it
// instead of silently analysing an empty file.
inline std::string read_file(const std::filesystem::path& p, bool* ok = nullptr) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream buf;
  if (in) buf << in.rdbuf();
  if (ok) *ok = in.is_open() && !in.bad();
  return buf.str();
}

// ASCII upper-casing, for HTTP method names.
inline std::string to_upper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// The `external` node for an HTTP endpoint whose id came from http_node_id. A "{}" in the id
// (a variable part of the URL) makes it `inferred`, otherwise `exact`. Same rule in every frontend.
inline optum::Node http_external_node(std::string node_id, std::string frontend) {
  optum::Node n;
  n.kind = optum::NodeKind::External;
  n.name = node_id.substr(5);  // without "http:"
  n.confidence =
      node_id.find("{}") == std::string::npos ? optum::Confidence::Exact : optum::Confidence::Inferred;
  n.id = std::move(node_id);
  n.frontend = std::move(frontend);
  return n;
}

// The `store` node for a SQL table whose id came from sql_store_id.
inline optum::Node sql_store_node(std::string node_id, std::string name, std::string frontend) {
  optum::Node n;
  n.id = std::move(node_id);
  n.kind = optum::NodeKind::Store;
  n.name = std::move(name);
  n.frontend = std::move(frontend);
  return n;
}

}  // namespace optum::frontends
