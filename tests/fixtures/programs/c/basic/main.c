/* Plain C: static and external functions, nested calls, a call through a function pointer. */
#include <stdio.h>

static int square(int x) { return x * x; }

int add(int a, int b) { return a + b; }

int sum_squares(int a, int b) { return add(square(a), square(b)); }

static int apply(int (*fn)(int), int x) { return fn(x); }

int main(void) {
  printf("%d\n", sum_squares(3, 4));
  printf("%d\n", apply(square, 5));
  return 0;
}
