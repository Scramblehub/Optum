#pragma once
// What counts as test code, for every analysis that leaves it out (docs/diagnose.md). Internal.

#include <sstream>
#include <string>
#include <string_view>

namespace optum {

// A node id or a file path is test code if a segment (split at ':', '.', '/', '#') is test,
// tests, testing, spec or __tests__, or starts with test_ or ends with _test / _tests. That covers
// test_*.py, *_test.go, tests/..., *.test.js and *.spec.ts.
inline bool is_test_code(std::string_view id) {
  const auto colon = id.find(':');
  std::string rest(colon == std::string_view::npos ? id : id.substr(colon + 1));
  for (char& c : rest) {
    if (c == '.' || c == '/' || c == '#') c = ' ';
  }
  std::istringstream words(rest);
  for (std::string w; words >> w;) {
    if (w == "test" || w == "tests" || w == "testing" || w == "spec" || w == "__tests__" ||
        w.starts_with("test_") || w.ends_with("_test") || w.ends_with("_tests")) {
      return true;
    }
  }
  return false;
}

}  // namespace optum
