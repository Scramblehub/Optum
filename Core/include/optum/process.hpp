#pragma once
// Runs a child process with input on stdin, collecting stdout and stderr, with a timeout.
// POSIX only for now.

#include <chrono>
#include <string>
#include <vector>

namespace optum {

struct ProcessResult {
  int exit_code = -1;      // -1 if killed or not started
  bool timed_out = false;
  std::string out;
  std::string err;
};

// argv[0] is looked up on PATH. Throws IrError if the process can't be started.
ProcessResult run_process(const std::vector<std::string>& argv, const std::string& input,
                          std::chrono::milliseconds timeout);

// Splits a command line on spaces, honouring '...' and "..." quoting. No other shell syntax.
std::vector<std::string> split_command(const std::string& command);

}  // namespace optum
