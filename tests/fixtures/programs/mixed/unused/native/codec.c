/* Planted unused code in C, and decoys. */
static int legacy_checksum(const char *s) { /* PLANTED: static, never used */
  int n = 0;
  while (*s) n += *s++;
  return n;
}

static int scale(int x) { return x * 4; } /* decoy: used only through a macro */
#define SCALED(x) scale(x)

static int op_add(int a, int b) { return a + b; } /* decoy: in a function-pointer table */
static int (*const OPS[])(int, int) = {op_add};

static int decode_one(int x) { return x - 1; } /* decoy: used only in the generated ops.inc */
#include "ops.inc"

#if 0
static int old_decoder(int x) { return x; } /* PLANTED: inside #if 0, never compiled */
#endif

int codec_run(int x) { return SCALED(x) + OPS[0](x, 1) + TABLE[0](x); }
