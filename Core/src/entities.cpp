#include "optum/entities.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "test_code.hpp"

namespace optum {
namespace {

// Fields nearly every record has: they say little about which entity it is.
bool generic_field(const std::string& f) {
  static const std::unordered_set<std::string> g = {
      "id", "uuid", "pk", "key", "name", "title", "type", "kind", "status", "state", "value", "data", "metadata",
      "meta", "description", "notes", "version", "created", "updated", "deleted", "created_at", "updated_at",
      "deleted_at", "created_by", "updated_by", "timestamp", "date", "active", "enabled", "count", "total"};
  return g.count(f) > 0;
}

std::string singular(std::string s) {
  if (s.size() > 4 && s.ends_with("ies")) return s.substr(0, s.size() - 3) + "y";
  if (s.size() > 4 && (s.ends_with("ses") || s.ends_with("xes"))) return s.substr(0, s.size() - 2);
  if (s.size() > 3 && s.ends_with("s") && !s.ends_with("ss")) return s.substr(0, s.size() - 1);
  return s;
}

std::string entity_name(const std::string& raw) {
  std::string n = normalize_field(raw);
  std::erase(n, '_');
  return singular(n);
}

struct Similarity {
  double containment = 0;
  int shared_specific = 0;  // shared fields that aren't generic
};

}  // namespace

std::string normalize_field(const std::string& field) {
  std::string out;
  for (std::size_t i = 0; i < field.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(field[i]);
    if (std::isupper(c)) {
      const bool after_lower = i > 0 && std::islower(static_cast<unsigned char>(field[i - 1]));
      const bool before_lower = i + 1 < field.size() && std::islower(static_cast<unsigned char>(field[i + 1])) && i > 0 &&
                                std::isupper(static_cast<unsigned char>(field[i - 1]));
      if ((after_lower || before_lower) && !out.empty() && out.back() != '_') out += '_';
      out += static_cast<char>(std::tolower(c));
    } else if (std::isalnum(c)) {
      out += static_cast<char>(c);
    } else if (!out.empty() && out.back() != '_') {
      out += '_';
    }
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  return out;
}

std::vector<EntityGroup> find_entities(const Graph& g) {
  // Shapes: classes with fields; tables with the columns code names; APIs with the fields code sends.
  std::map<std::string, EntityShape> shapes;
  auto add_fields = [](EntityShape& s, const nlohmann::json& list) {
    if (!list.is_array()) return;
    for (const auto& f : list) {
      if (!f.is_string()) continue;
      const std::string raw = f.get<std::string>();
      const std::string norm = normalize_field(raw);
      if (norm.empty() || std::find(s.normalized.begin(), s.normalized.end(), norm) != s.normalized.end()) continue;
      s.fields.push_back(raw);
      s.normalized.push_back(norm);
    }
  };
  for (NodeIndex i = 0; i < g.nodes().size(); ++i) {
    const Node& n = g.node(i);
    if (n.kind != NodeKind::DataType || !n.attrs.contains("fields") || is_test_code(n.id)) continue;
    EntityShape& s = shapes[n.id];
    s.id = n.id;
    s.kind = "class";
    s.form = "class:" + n.frontend;
    s.name = entity_name(n.name);
    s.users = {n.id};
    add_fields(s, n.attrs["fields"]);
  }
  for (const Edge& e : g.edges()) {
    const Node& from = g.node(e.from);
    const Node& to = g.node(e.to);
    if (is_test_code(from.id)) continue;
    const bool table = to.kind == NodeKind::Store && to.id.starts_with("sql:") && e.attrs.contains("columns");
    const bool payload = e.kind == EdgeKind::SendsTo && to.kind == NodeKind::External && e.attrs.contains("fields");
    if (!table && !payload) continue;
    EntityShape& s = shapes[to.id];
    s.id = to.id;
    s.kind = table ? "table" : "payload";
    s.form = s.kind;
    if (table) {
      s.name = entity_name(to.id.substr(4));
    } else {  // the API's last named path segment: http:crm.example.com/api/contacts -> contact
      std::string path = to.id;
      std::string last;
      for (std::size_t p = 0; p < path.size();) {
        const auto slash = path.find('/', p);
        const std::string seg = path.substr(p, slash == std::string::npos ? std::string::npos : slash - p);
        if (!seg.empty() && seg != "{}" && seg.find('.') == std::string::npos && seg.find(':') == std::string::npos) last = seg;
        if (slash == std::string::npos) break;
        p = slash + 1;
      }
      s.name = entity_name(last);
    }
    if (std::find(s.users.begin(), s.users.end(), from.id) == s.users.end()) s.users.push_back(from.id);
    add_fields(s, table ? e.attrs["columns"] : e.attrs["fields"]);
  }
  std::vector<EntityShape> list;
  for (auto& [_, s] : shapes) {
    if (s.normalized.size() >= 3) list.push_back(std::move(s));
  }
  if (list.size() < 2) return {};

  // Field weights: rare fields say more; generic ones (id, name, created_at) little.
  std::unordered_map<std::string, int> df;
  for (const auto& s : list) {
    for (const auto& f : s.normalized) ++df[f];
  }
  const double n_shapes = static_cast<double>(list.size());
  auto weight = [&](const std::string& f) { return generic_field(f) ? 0.1 : std::log(1.0 + n_shapes / df[f]); };
  auto similarity = [&](const EntityShape& a, const EntityShape& b) {
    Similarity s;
    const std::set<std::string> fb(b.normalized.begin(), b.normalized.end());
    double shared = 0, wa = 0, wb = 0;
    for (const auto& f : a.normalized) {
      wa += weight(f);
      if (fb.count(f)) {
        shared += weight(f);
        if (!generic_field(f)) ++s.shared_specific;
      }
    }
    for (const auto& f : b.normalized) wb += weight(f);
    s.containment = std::min(wa, wb) > 0 ? shared / std::min(wa, wb) : 0;
    return s;
  };
  // Same entity: most of the smaller one's (weighted) fields are shared, two of them specific;
  // with the same name (Customer, customers, /customers), less is enough.
  auto same = [&](std::size_t a, std::size_t b) {
    const Similarity s = similarity(list[a], list[b]);
    if (list[a].name == list[b].name && !list[a].name.empty()) return s.shared_specific >= 1 && s.containment >= 0.4;
    return s.shared_specific >= 2 && s.containment >= 0.6;
  };

  std::vector<std::tuple<double, std::size_t, std::size_t>> pairs;
  for (std::size_t a = 0; a < list.size(); ++a) {
    for (std::size_t b = a + 1; b < list.size(); ++b) {
      if (list[a].id == list[b].id) continue;
      if (same(a, b)) pairs.emplace_back(similarity(list[a], list[b]).containment, a, b);
    }
  }
  std::sort(pairs.begin(), pairs.end(), [](const auto& x, const auto& y) { return std::get<0>(x) > std::get<0>(y); });
  std::vector<int> group_of(list.size(), -1);
  std::vector<std::vector<std::size_t>> groups;
  for (const auto& [_, a, b] : pairs) {  // complete link, as for clones
    if (group_of[a] >= 0 && group_of[a] == group_of[b]) continue;
    const auto ga = group_of[a] >= 0 ? groups[std::size_t(group_of[a])] : std::vector<std::size_t>{a};
    const auto gb = group_of[b] >= 0 ? groups[std::size_t(group_of[b])] : std::vector<std::size_t>{b};
    bool all = true;
    for (std::size_t x : ga) {
      for (std::size_t y : gb) all = all && same(x, y);
    }
    if (!all) continue;
    std::vector<std::size_t> merged = ga;
    merged.insert(merged.end(), gb.begin(), gb.end());
    const int gi = group_of[a] >= 0 ? group_of[a] : group_of[b] >= 0 ? group_of[b] : static_cast<int>(groups.size());
    if (gi == static_cast<int>(groups.size())) groups.emplace_back();
    if (group_of[a] >= 0 && group_of[b] >= 0 && group_of[a] != group_of[b]) groups[std::size_t(group_of[b])].clear();
    groups[std::size_t(gi)] = merged;
    for (std::size_t k : merged) group_of[k] = gi;
  }

  std::vector<EntityGroup> out;
  for (const auto& ks : groups) {
    if (ks.size() < 2) continue;
    // Data in several forms (a table, a payload, classes in different languages): classes of one
    // language alone are code structure (subclasses, vendored copies), which other findings cover.
    std::set<std::string> forms;
    for (std::size_t k : ks) forms.insert(list[k].form);
    if (forms.size() < 2) continue;
    EntityGroup eg;
    std::map<std::string, int> names, counts;
    std::vector<std::string> order;
    eg.score = 1.0;
    for (std::size_t i = 0; i < ks.size(); ++i) {
      const EntityShape& s = list[ks[i]];
      ++names[s.name];
      for (const auto& f : s.normalized) {
        if (!counts[f]++) order.push_back(f);
      }
      for (std::size_t j = i + 1; j < ks.size(); ++j) eg.score = std::min(eg.score, similarity(s, list[ks[j]]).containment);
      eg.members.push_back(s);
    }
    eg.name = std::max_element(names.begin(), names.end(), [](const auto& x, const auto& y) { return x.second < y.second; })->first;
    const int half = static_cast<int>((ks.size() + 1) / 2);
    std::stable_sort(order.begin(), order.end(), [&](const auto& x, const auto& y) { return counts[x] > counts[y]; });
    for (const auto& f : order) {
      if (counts[f] >= half) eg.proposed.push_back(f);
    }
    std::sort(eg.members.begin(), eg.members.end(), [](const auto& x, const auto& y) { return x.id < y.id; });
    out.push_back(std::move(eg));
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.members.size() > b.members.size(); });
  return out;
}

}  // namespace optum
