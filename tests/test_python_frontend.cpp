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

TEST_CASE("python: a subscripted base is followed; Generic[...] and Protocol[...] are not bases") {
  Project p;
  p.write("m.py",
          "from typing import Generic, TypeVar, Protocol\n"
          "T = TypeVar('T')\n"
          "class Base(Generic[T]):\n"
          "    def fail(self):\n"
          "        pass\n"
          "    def __init__(self):\n"
          "        pass\n"
          "class Child(Base[int]):\n"
          "    def __init__(self):\n"
          "        super().__init__()\n"
          "    def go(self):\n"
          "        self.fail()\n"
          "class P(Protocol[T]):\n"
          "    def run(self):\n"
          "        pass\n");
  const auto e = p.edges();
  REQUIRE(e.count("calls py:m.Child.go -> py:m.Base.fail") == 1);
  REQUIRE(e.count("calls py:m.Child.__init__ -> py:m.Base.__init__") == 1);
}

TEST_CASE("python: cls(...), type(self)(...) and self.__class__(...) build the enclosing class") {
  Project p;
  p.write("m.py",
          "class Bits:\n"
          "    def __init__(self, v):\n"
          "        self.v = v\n"
          "    @classmethod\n"
          "    def from_bin(cls, s):\n"
          "        return cls(int(s, 2))\n"
          "    def copy(self):\n"
          "        return type(self)(self.v)\n"
          "    def clone(self):\n"
          "        return self.__class__(self.v)\n"
          "    def __call__(self):\n"
          "        pass\n"
          "    def again(self):\n"
          "        self()\n");
  const auto e = p.edges();
  REQUIRE(e.count("calls py:m.Bits.from_bin -> py:m.Bits") == 1);
  REQUIRE(e.count("calls py:m.Bits.copy -> py:m.Bits") == 1);
  REQUIRE(e.count("calls py:m.Bits.clone -> py:m.Bits") == 1);
  REQUIRE(e.count("calls py:m.Bits.again -> py:m.Bits.__call__") == 1);  // self(...) is not a constructor call
  REQUIRE(e.count("calls py:m.Bits.again -> py:m.Bits") == 0);
}

TEST_CASE("python: obj(...) calls __call__ when obj's class is known") {
  Project p;
  p.write("v.py",
          "class Check:\n"
          "    def __call__(self, x):\n"
          "        pass\n"
          "def make():\n"
          "    return Check()\n"
          "class Field:\n"
          "    def __init__(self):\n"
          "        self.check = Check()\n"
          "    def run(self, x):\n"
          "        self.check(x)\n"
          "def local(x):\n"
          "    c = Check()\n"
          "    c(x)\n"
          "def returned(x):\n"
          "    c = make()\n"
          "    c(x)\n"
          "def unknown(c, x):\n"
          "    c(x)\n");
  const auto e = p.edges();
  REQUIRE(e.count("calls py:v.Field.run -> py:v.Check.__call__") == 1);
  REQUIRE(e.count("calls py:v.local -> py:v.Check.__call__") == 1);
  REQUIRE(e.count("calls py:v.returned -> py:v.Check.__call__") == 1);
  REQUIRE(e.count("calls py:v.unknown -> py:v.Check.__call__") == 0);
}

TEST_CASE("python: a parameter's default is added to what callers pass, not replaced by it") {
  Project p;
  p.write("m.py",
          "def d1(x):\n"
          "    pass\n"
          "def inner(x):\n"
          "    pass\n"
          "def run(enter=d1):\n"
          "    enter(1)\n"
          "run(enter=inner)\n"
          "def solo(f=d1):\n"
          "    f(1)\n");
  const auto e = p.edges();
  REQUIRE(e.count("calls py:m.run -> py:m.inner") == 1);
  REQUIRE(e.count("calls py:m.run -> py:m.d1") == 1);
  REQUIRE(e.count("calls py:m.solo -> py:m.d1") == 1);  // no caller passes one: the default alone
}

TEST_CASE("python: a method name is not guessed for a call on an imported standard-library module") {
  Project p;
  p.write("m.py",
          "import os\n"
          "import re as regex\n"
          "from os import path\n"
          "class Lazy:\n"
          "    def open(self):\n"
          "        pass\n"
          "    def sub(self):\n"
          "        pass\n"
          "    def join(self):\n"
          "        pass\n"
          "def go(x):\n"
          "    os.open('f', 0)\n"
          "    regex.sub('a', 'b', x)\n"
          "    path.join('a')\n"
          "    x.open()\n");  // an unknown receiver still gets the unique-name guess
  const auto e = p.edges();
  REQUIRE(e.count("calls py:m.go -> py:m.Lazy.open") == 1);  // from x.open() only
  REQUIRE(e.count("calls py:m.go -> py:m.Lazy.sub") == 0);
  REQUIRE(e.count("calls py:m.go -> py:m.Lazy.join") == 0);
}
