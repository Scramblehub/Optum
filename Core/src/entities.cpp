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

// Natural keys: fields that identify a record by themselves.
bool natural_key(const std::string& f) {
  static const std::unordered_set<std::string> k = {"email", "email_address", "sku", "username", "user_name", "login",
                                                    "slug", "ean", "upc", "gtin", "isbn", "iban", "external_id",
                                                    "account_number", "customer_number", "order_number", "handle"};
  return k.count(f) > 0;
}

// Words of an id, a path or a name: camelCase and separators split, lower case.
std::vector<std::string> words(const std::string& s) {
  std::vector<std::string> out;
  std::string w = normalize_field(s);
  std::string cur;
  for (char c : w + "_") {
    if (std::isalnum(static_cast<unsigned char>(c))) cur += c;
    else if (!cur.empty()) {
      out.push_back(cur);
      cur.clear();
    }
  }
  return out;
}

// Regulated forms: tax, legal and medical records are kept as they are, separately (they're
// required in their own form, often immutable once issued), so they're never proposed for merging.
std::string regulated_kind(const EntityShape& s) {
  static const std::unordered_map<std::string, std::string> by_word = {
      {"tax", "tax"}, {"taxes", "tax"}, {"vat", "tax"}, {"gst", "tax"}, {"hst", "tax"}, {"w2", "tax"}, {"w4", "tax"},
      {"w8", "tax"}, {"w9", "tax"}, {"1099", "tax"}, {"1040", "tax"}, {"irs", "tax"}, {"hmrc", "tax"},
      {"legal", "legal"}, {"contract", "legal"}, {"contracts", "legal"}, {"agreement", "legal"}, {"consent", "legal"},
      {"waiver", "legal"}, {"nda", "legal"}, {"affidavit", "legal"}, {"litigation", "legal"}, {"gdpr", "legal"},
      {"kyc", "legal"}, {"aml", "legal"},
      {"medical", "medical"}, {"patient", "medical"}, {"patients", "medical"}, {"diagnosis", "medical"},
      {"prescription", "medical"}, {"clinical", "medical"}, {"hipaa", "medical"}, {"phi", "medical"},
      {"ehr", "medical"}, {"emr", "medical"}, {"healthcare", "medical"}, {"icd10", "medical"}};
  static const std::unordered_map<std::string, std::string> by_field = {
      {"ssn", "tax"}, {"tin", "tax"}, {"itin", "tax"}, {"ein", "tax"}, {"tax_id", "tax"}, {"vat_number", "tax"},
      {"vat_id", "tax"}, {"mrn", "medical"}, {"npi", "medical"}, {"icd10", "medical"}, {"icd_code", "medical"},
      {"diagnosis_code", "medical"}, {"nhs_number", "medical"}};
  // The shape's own name and where it lives (module path, table name, API path).
  for (const auto& w : words(s.id.substr(s.id.find(':') + 1))) {
    if (auto it = by_word.find(w); it != by_word.end()) return it->second;
  }
  for (const auto& f : s.normalized) {
    if (auto it = by_field.find(f); it != by_field.end()) return it->second;
  }
  return "";
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

EntityResult find_entities(const Graph& g) {
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
    if (n.loc && !code_area(n.loc->file).empty()) continue;  // vendored, build output, examples
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
    if (table && e.attrs.contains("keys") && e.attrs["keys"].is_array()) {
      for (const auto& k : e.attrs["keys"]) {
        if (!k.is_string()) continue;  // adapters write these: a stray number must not abort the report
        const std::string nk = normalize_field(k.get<std::string>());
        if (std::find(s.declared_keys.begin(), s.declared_keys.end(), nk) == s.declared_keys.end()) s.declared_keys.push_back(nk);
      }
    }
  }
  // Identity keys, then regulated forms out.
  EntityResult result;
  std::vector<EntityShape> list;
  for (auto& [_, s] : shapes) {
    if (s.normalized.size() < 3) continue;
    for (const auto& f : s.normalized) {
      std::string key;
      if ((f == "id" || f == "uuid" || f == "guid" || f == "pk") && !s.name.empty()) key = s.name + "_" + (f == "pk" ? "id" : f);
      else if (!s.name.empty() && (f == s.name + "_id" || f == s.name + "_uuid" || f == s.name + "_key")) key = f;
      else if (natural_key(f)) key = f;
      if (!key.empty() && std::find(s.keys.begin(), s.keys.end(), key) == s.keys.end()) s.keys.push_back(key);
    }
    for (const auto& k : s.declared_keys) {
      const std::string key = (k == "id" && !s.name.empty()) ? s.name + "_id" : k;
      if (std::find(s.keys.begin(), s.keys.end(), key) == s.keys.end()) s.keys.push_back(key);
    }
    s.regulated = regulated_kind(s);
    if (!s.regulated.empty()) {
      result.regulated.push_back(std::move(s));
      continue;
    }
    list.push_back(std::move(s));
  }
  if (list.size() < 2) return result;

  // Field weights: rare fields say more; generic ones (id, name, created_at) little. Fields are
  // numbered once, so comparing two shapes is integer lookups, not string sets rebuilt per pair.
  std::unordered_map<std::string, int> field_id;
  std::vector<double> weight_of;       // by field number
  std::vector<bool> generic_of;        // by field number
  std::vector<std::vector<int>> ids(list.size());      // each shape's field numbers, in field order
  std::vector<std::vector<int>> sorted_ids(list.size());  // the same, sorted: for membership tests
  std::vector<double> total_weight(list.size(), 0.0);
  std::vector<int> df;
  for (std::size_t k = 0; k < list.size(); ++k) {
    for (const auto& f : list[k].normalized) {
      const auto [it, fresh] = field_id.try_emplace(f, static_cast<int>(df.size()));
      if (fresh) {
        df.push_back(0);
        generic_of.push_back(generic_field(f));
      }
      ++df[static_cast<std::size_t>(it->second)];
      ids[k].push_back(it->second);
    }
  }
  const double n_shapes = static_cast<double>(list.size());
  for (std::size_t f = 0; f < df.size(); ++f) weight_of.push_back(generic_of[f] ? 0.1 : std::log(1.0 + n_shapes / df[f]));
  for (std::size_t k = 0; k < list.size(); ++k) {
    for (int f : ids[k]) total_weight[k] += weight_of[static_cast<std::size_t>(f)];  // in field order, as before
    sorted_ids[k] = ids[k];
    std::sort(sorted_ids[k].begin(), sorted_ids[k].end());
  }
  auto similarity = [&](std::size_t a, std::size_t b) {
    Similarity s;
    const auto& fb = sorted_ids[b];
    double shared = 0;
    for (int f : ids[a]) {
      if (!std::binary_search(fb.begin(), fb.end(), f)) continue;
      shared += weight_of[static_cast<std::size_t>(f)];
      if (!generic_of[static_cast<std::size_t>(f)]) ++s.shared_specific;
    }
    const double smaller = std::min(total_weight[a], total_weight[b]);
    s.containment = smaller > 0 ? shared / smaller : 0;
    return s;
  };
  // Same entity only when something identifies it, never by coincidence of names or a few
  // fields: a shared identity key (an id, a natural or declared-unique key), or else a temporary
  // composite key: 3+ specific fields in common, covering most of the smaller shape.
  auto shared_key = [&](std::size_t a, std::size_t b) {
    for (const auto& k : list[a].keys) {
      if (std::find(list[b].keys.begin(), list[b].keys.end(), k) != list[b].keys.end()) return true;
    }
    return false;
  };
  auto same = [&](std::size_t a, std::size_t b) {
    const Similarity s = similarity(a, b);
    if (shared_key(a, b)) return s.shared_specific >= 2 && s.containment >= 0.4;
    return s.shared_specific >= 3 && s.containment >= 0.75;
  };

  // Either test needs 2+ specific fields in common, so only shapes sharing that many are compared:
  // found through the shapes that have each specific field, not by trying every pair.
  std::vector<std::vector<std::size_t>> having(df.size());
  for (std::size_t k = 0; k < list.size(); ++k) {
    for (int f : ids[k]) {
      if (!generic_of[static_cast<std::size_t>(f)]) having[static_cast<std::size_t>(f)].push_back(k);
    }
  }
  std::vector<std::tuple<double, std::size_t, std::size_t>> pairs;
  std::vector<int> common(list.size(), 0);
  for (std::size_t a = 0; a < list.size(); ++a) {
    std::vector<std::size_t> touched;
    for (int f : ids[a]) {
      if (generic_of[static_cast<std::size_t>(f)]) continue;
      for (std::size_t b : having[static_cast<std::size_t>(f)]) {
        if (b <= a) continue;
        if (common[b]++ == 0) touched.push_back(b);
      }
    }
    std::sort(touched.begin(), touched.end());  // the order a plain a<b loop would visit them
    for (std::size_t b : touched) {
      const bool enough = common[b] >= 2;
      common[b] = 0;
      if (!enough || list[a].id == list[b].id) continue;
      const Similarity s = similarity(a, b);
      if (same(a, b)) pairs.emplace_back(s.containment, a, b);
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
      for (std::size_t j = i + 1; j < ks.size(); ++j) eg.score = std::min(eg.score, similarity(ks[i], ks[j]).containment);
      eg.members.push_back(s);
    }
    eg.name = std::max_element(names.begin(), names.end(), [](const auto& x, const auto& y) { return x.second < y.second; })->first;
    const int half = static_cast<int>((ks.size() + 1) / 2);
    std::stable_sort(order.begin(), order.end(), [&](const auto& x, const auto& y) { return counts[x] > counts[y]; });
    for (const auto& f : order) {
      if (counts[f] >= half) eg.proposed.push_back(f);
    }
    // The basis: identity keys every member has, else the specific fields they all share.
    std::map<std::string, std::size_t> key_count, field_count;
    for (const auto& m : eg.members) {
      for (const auto& k : m.keys) ++key_count[k];
      for (const auto& f : m.normalized) {
        if (!generic_field(f)) ++field_count[f];
      }
    }
    for (const auto& [k, n] : key_count) {
      if (n == eg.members.size()) eg.basis_fields.push_back(k);
    }
    eg.basis = eg.basis_fields.empty() ? "composite" : "identity";
    if (eg.basis_fields.empty()) {
      for (const auto& [f, n] : field_count) {
        if (n == eg.members.size()) eg.basis_fields.push_back(f);
      }
    }
    std::sort(eg.members.begin(), eg.members.end(), [](const auto& x, const auto& y) { return x.id < y.id; });
    result.groups.push_back(std::move(eg));
  }
  std::sort(result.groups.begin(), result.groups.end(), [](const auto& a, const auto& b) { return a.members.size() > b.members.size(); });
  return result;
}

}  // namespace optum
