/* Family E (C): checksum_v1 and checksum_v2 are near copies; adler_like is a decoy. */
unsigned checksum_v1(const unsigned char *buf, int len) {
  unsigned a = 1, b = 0;
  int i;
  for (i = 0; i < len; i++) {
    a = (a + buf[i]) % 65521;
    b = (b + a) % 65521;
  }
  if (len > 1024) {
    a ^= 0x5a5a;
  }
  return (b << 16) | a;
}

unsigned checksum_v2(const unsigned char *buf, int len) {
  unsigned a = 1, b = 0;
  int i;
  for (i = 0; i < len; i++) {
    a = (a + buf[i]) % 65521;
    b = (b + a) % 65521;
  }
  if (len > 4096) {
    a ^= 0x5a5a;
    b ^= 0x1;
  }
  return (b << 16) | a;
}

int count_words(const char *s) { /* decoy: a loop over a buffer, different logic */
  int n = 0, in_word = 0;
  for (; *s; s++) {
    if (*s == ' ' || *s == '\n' || *s == '\t') {
      in_word = 0;
    } else if (!in_word) {
      in_word = 1;
      n++;
    }
  }
  return n;
}
