// Regression tests for the Python frontend that need a tiny project built on the fly.
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <set>

#include "optum/frontends/python.hpp"

using namespace optum;
namespace fs = std::filesystem;

namespace {
struct Project {
  fs::path root = fs::temp_directory_path() / ("optum_py_" + std::to_string(getpid()));
  Project() {
    fs::remove_all(root);
    fs::create_directories(root);
  }
  ~Project() { fs::remove_all(root); }
  void write(const std::string& rel, const std::string& text) const {
    fs::create_directories((root / rel).parent_path());
    std::ofstream(root / rel) << text;
  }
  // "kind from -> to" for every edge.
  std::set<std::string> edges() const {
    const Graph g = extract_python(root);
    std::set<std::string> out;
    for (const Edge& e : g.edges()) {
      out.insert(std::string(to_string(e.kind)) + " " + g.node(e.from).id + " -> " + g.node(e.to).id);
    }
    return out;
  }
};
}  // namespace

TEST_CASE("python: the first parameter of a @staticmethod is not the instance") {
  Project p;
  p.write("a.py",
          "class K:\n"
          "    @staticmethod\n"
          "    def make(x):\n"
          "        x.run()\n"
          "    def run(self):\n"
          "        self.go()\n"
          "    def go(self):\n"
          "        pass\n"
          "class L:\n"
          "    def run(self):\n"  // two classes define run(), so an unknown `x.run()` stays unresolved
          "        pass\n");
  const auto e = p.edges();
  REQUIRE(e.count("calls py:a.K.run -> py:a.K.go") == 1);  // self still works
  REQUIRE(e.count("calls py:a.K.make -> py:a.K.run") == 0);
}

TEST_CASE("python: `from .. import x` that climbs to the analysed root") {
  Project p;
  p.write("top.py", "def main():\n    pass\n");
  p.write("pkg/__init__.py", "");
  p.write("pkg/core.py", "from .. import top\n\ndef run():\n    top.main()\n");
  const auto e = p.edges();
  REQUIRE(e.count("imports py:pkg.core -> py:top") == 1);
  REQUIRE(e.count("calls py:pkg.core.run -> py:top.main") == 1);
}

TEST_CASE("python: `from . import x` in a top-level file") {
  Project p;
  p.write("helpers.py", "def work():\n    pass\n");
  p.write("main.py", "from . import helpers\n\ndef go():\n    helpers.work()\n");
  const auto e = p.edges();
  REQUIRE(e.count("imports py:main -> py:helpers") == 1);
  REQUIRE(e.count("calls py:main.go -> py:helpers.work") == 1);
}

TEST_CASE("python: escaped quotes in SQL strings are decoded; raw strings are not") {
  Project p;
  p.write("q.py",
          "def a(db):\n"
          "    db.execute(\"select * from \\\"Users\\\"\")\n"
          "def b(db):\n"
          "    db.execute(r'select * from \"raw\\tbl\"')\n");
  const auto e = p.edges();
  REQUIRE(e.count("reads py:q.a -> sql:Users") == 1);
  REQUIRE(e.count("reads py:q.b -> sql:raw\\tbl") == 1);
}

TEST_CASE("python: URLs from configuration defaults and string concatenation") {
  Project p;
  p.write("settings.py", "import os\nSHOP = os.environ.get(\"SHOP_URL\", \"http://shop.example.com/api\")\n"
                          "CRM = os.getenv(\"CRM_URL\", default=\"https://crm.example.com\")\nNOPE = os.environ.get(\"X\")\n");
  p.write("sync.py",
          "import requests\nfrom settings import SHOP, CRM, NOPE\n"
          "def a():\n    requests.get(SHOP + \"/orders\")\n"
          "def b(order_id):\n    requests.put(f\"{SHOP}/orders/{order_id}\")\n"
          "def c(x):\n    requests.post(CRM + \"/deals/\" + x)\n"
          "def d():\n    requests.get(NOPE + \"/x\")\n");
  const auto e = p.edges();
  REQUIRE(e.count("reads py:sync.a -> http:shop.example.com/api/orders") == 1);
  REQUIRE(e.count("sends_to py:sync.b -> http:shop.example.com/api/orders/{}") == 1);
  REQUIRE(e.count("sends_to py:sync.c -> http:crm.example.com/deals/{}") == 1);  // unknown right side: a hole
  for (const auto& edge : e) REQUIRE(edge.find("py:sync.d ->") == std::string::npos);  // no default: unknown
}
