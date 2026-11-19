/* Several files: calls across files, a static name used in two files, a header's inline
   function, and a dispatch table of function pointers. */
#include <stdio.h>

#include "util.h"

static int helper(int x) { return twice(x) + 1; }

typedef int (*op_fn)(int);

static int inc(int x) { return x + 1; }
static int dec(int x) { return x - 1; }

struct op {
  const char *name;
  op_fn fn;
};

static const struct op OPS[] = {{"inc", inc}, {"dec", dec}};

static int run(const char *name, int x) {
  for (unsigned i = 0; i < sizeof OPS / sizeof OPS[0]; i++) {
    if (OPS[i].name[0] == name[0]) return OPS[i].fn(x);
  }
  return x;
}

int main(void) {
  printf("%d %d %d\n", checksum("abc"), helper(3), run("inc", 4));
  return 0;
}
