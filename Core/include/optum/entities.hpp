#pragma once
// The same entity in several places: a class, a SQL table and an API payload describing the same
// thing (a customer, an order) with matching fields (docs/diagnose.md, "The same data in several
// places"). Read from the fields the frontends record: attrs.fields on classes, attrs.columns on
// SQL edges, attrs.fields on API request edges.

#include <string>
#include <vector>

#include "optum/ir.hpp"

namespace optum {

struct EntityShape {
  std::string id;    // the class, table or API node
  std::string kind;  // "class", "table", "payload"
  std::string form;  // the kind, with the language for classes ("class:python"): a group spans 2+
  std::string name;  // singular, lower case: Customer, customers and /contacts -> customer, customer, contact
  std::vector<std::string> fields;      // as written
  std::vector<std::string> normalized;  // same order: firstName, first_name, FirstName -> first_name
  std::vector<std::string> users;       // the nodes that define or use it (for components)
  // Identity keys (normalised): its own id, canonical ("id" in customers -> "customer_id"),
  // <name>_id / _uuid / _key fields, natural keys (email, sku...) and columns declared unique.
  std::vector<std::string> keys;
  std::string regulated;  // "tax", "legal" or "medical" if it's a regulated form: never merged
  std::vector<std::string> declared_keys;  // tables: columns declared PRIMARY KEY / UNIQUE
};

struct EntityGroup {
  std::string name;  // the members' most common name
  double score = 0;  // the lowest similarity between two members
  std::vector<EntityShape> members;   // sorted by id
  std::vector<std::string> proposed;  // normalised fields at least half the members have
  // What ties the members together: shared identity keys, or (no shared key) the specific fields
  // they all have, standing in as a temporary composite key.
  std::string basis;                   // "identity" or "composite"
  std::vector<std::string> basis_fields;
};

struct EntityResult {
  std::vector<EntityGroup> groups;
  std::vector<EntityShape> regulated;  // shapes left out: regulated forms (tax, legal, medical)
};

EntityResult find_entities(const Graph& g);

// For tests and the report: "firstName" -> "first_name"; "_Email" -> "email".
std::string normalize_field(const std::string& field);

}  // namespace optum
