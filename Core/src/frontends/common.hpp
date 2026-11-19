#pragma once
// Helpers shared by the built-in frontends. Internal to Core.

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace optum::frontends {

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

inline std::string read_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

}  // namespace optum::frontends
