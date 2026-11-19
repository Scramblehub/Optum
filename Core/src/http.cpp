#include "optum/http.hpp"

#include <cctype>

namespace optum {

std::string http_node_id(std::string_view url) {
  const auto scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos) return "";
  const std::string_view scheme = url.substr(0, scheme_end);
  if (scheme != "http" && scheme != "https") return "";
  std::string_view rest = url.substr(scheme_end + 3);
  rest = rest.substr(0, rest.find_first_of("?#"));

  const auto slash = rest.find('/');
  std::string_view host = rest.substr(0, slash);
  std::string_view path = slash == std::string_view::npos ? "" : rest.substr(slash);
  if (host.empty() || host.find("{}") != std::string_view::npos) return "";
  if ((scheme == "https" && host.ends_with(":443")) || (scheme == "http" && host.ends_with(":80"))) {
    host = host.substr(0, host.rfind(':'));
  }
  while (path.size() > 1 && path.back() == '/') path.remove_suffix(1);
  if (path == "/") path = "";

  std::string id = "http:";
  for (char c : host) id += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  id += path;
  return id;
}

}  // namespace optum
