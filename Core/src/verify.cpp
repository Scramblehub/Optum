#include "optum/verify.hpp"

#include <set>

namespace optum {

VerifyReport verify(const Graph& g, const nlohmann::json& trace) {
  if (!trace.is_object() || !trace.contains("calls") || !trace["calls"].is_array()) {
    throw IrError("trace: expected an object with a \"calls\" array");
  }
  // "0.1" or "0.1.x"; not "0.10" or "0.1abc". A missing or non-string version is said as such.
  const auto vit = trace.find("trace_version");
  if (vit == trace.end() || !vit->is_string()) {
    throw IrError("trace: missing \"trace_version\" (a string; this build reads 0.1)");
  }
  const std::string version = vit->get<std::string>();
  if (version != "0.1" && !version.starts_with("0.1.")) {
    throw IrError("trace: unsupported trace_version '" + version + "' (this build reads 0.1)");
  }

  std::set<std::pair<std::string, std::string>> exact, inferred;
  for (const Edge& e : g.edges()) {
    if (e.kind != EdgeKind::Calls) continue;
    (e.confidence == Confidence::Inferred ? inferred : exact).emplace(g.node(e.from).id, g.node(e.to).id);
  }
  VerifyReport r;
  std::set<std::pair<std::string, std::string>> seen;
  for (std::size_t i = 0; i < trace["calls"].size(); ++i) {
    const auto& c = trace["calls"][i];
    if (!c.is_object() || !c.contains("from") || !c.contains("to") || !c["from"].is_string() || !c["to"].is_string()) {
      throw IrError("trace: calls[" + std::to_string(i) + "] needs string fields \"from\" and \"to\"");
    }
    const std::pair<std::string, std::string> call{c["from"], c["to"]};
    if (call.first == call.second || !seen.insert(call).second) continue;
    ++r.observed;
    const std::string text = call.first + " -> " + call.second;
    if (exact.count(call)) ++r.found_exact;
    else if (inferred.count(call)) ++r.found_inferred;
    else if (!g.find(call.first)) r.caller_missing.push_back(text);
    else if (!g.find(call.second)) r.callee_missing.push_back(text);
    else r.edge_missing.push_back(text);
  }
  return r;
}

nlohmann::json to_json(const VerifyReport& r) {
  return {{"observed", r.observed},
          {"found_exact", r.found_exact},
          {"found_inferred", r.found_inferred},
          {"recall", r.recall()},
          {"caller_missing", r.caller_missing},
          {"callee_missing", r.callee_missing},
          {"edge_missing", r.edge_missing}};
}

std::string to_text(const VerifyReport& r, std::size_t max_listed) {
  char head[256];
  std::snprintf(head, sizeof head,
                "observed calls: %zu\nin the graph:   %zu (%zu exact, %zu inferred), recall %.1f%%\n", r.observed,
                r.found_exact + r.found_inferred, r.found_exact, r.found_inferred, 100.0 * r.recall());
  std::string out = head;
  auto list = [&](const char* title, const std::vector<std::string>& items) {
    if (items.empty()) return;
    out += std::string("\n") + title + " (" + std::to_string(items.size()) + "):\n";
    for (std::size_t i = 0; i < items.size() && i < max_listed; ++i) out += "  " + items[i] + "\n";
    if (items.size() > max_listed) out += "  ...\n";
  };
  list("missed: no edge between the two nodes", r.edge_missing);
  list("missed: the callee isn't a node", r.callee_missing);
  list("missed: the caller isn't a node", r.caller_missing);
  return out;
}

}  // namespace optum
