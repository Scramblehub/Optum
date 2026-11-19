#pragma once
#include <string>
#include <string_view>

namespace optum {

// Turns a URL (or a URL template with "{}" for unknown parts) into an external node id:
// "https://API.Example.com:443/v1/deals/{}?x=1" -> "http:api.example.com/v1/deals/{}".
// The scheme, default ports, query and fragment are dropped; the host is lower-cased; a trailing
// slash is removed. Returns "" if there's no scheme or the host itself is unknown.
std::string http_node_id(std::string_view url);

}  // namespace optum
