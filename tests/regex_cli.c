/* regex_cli.c -- a driver for tests/fuzz_regex.py, which checks molly's engine
 * against Python's re on the subset where the two are supposed to agree.
 *
 * Reads one case per line from standard input, as three space-separated fields:
 *
 *     <flags> <start> <pattern-in-hex> <text-in-hex>
 *
 * Hex because a pattern is made of exactly the punctuation a shell or a
 * line-based protocol would otherwise eat, and because the text has to be able
 * to hold any byte. flags is 1 for REGEX_ICASE, 0 otherwise, and start is the
 * byte offset the search begins at -- which is how find-next reaches the second
 * match on a row, so it is worth fuzzing rather than only ever passing 0.
 *
 * Writes one line per case:
 *
 *     match <so>,<eo> <so>,<eo> ...     the whole match, then each group
 *     nomatch
 *     toohard                           the step budget or the depth ran out
 *     error <message>                   the pattern did not compile
 *
 * One process for the whole run: a fuzz pass is tens of thousands of cases and a
 * fork each would make the run slow enough not to be run. */

#include "molly.h"

static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Decodes one hex field, allocating what it decodes. Returns the number of
 * bytes consumed from s, or 0 if what is there is not a hex field. */
static int unhex(const char *s, char **out, int *outlen) {
  int n = 0, i;
  char *buf;

  while (s[n] && s[n] != ' ' && s[n] != '\n') n++;
  if (n % 2 != 0) return 0;
  buf = malloc((size_t)(n / 2) + 1);
  if (buf == NULL) return 0;
  for (i = 0; i < n / 2; i++) {
    int hi = hexval((unsigned char)s[i * 2]);
    int lo = hexval((unsigned char)s[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      free(buf);
      return 0;
    }
    buf[i] = (char)((hi << 4) | lo);
  }
  buf[n / 2] = '\0';
  *out = buf;
  *outlen = n / 2;
  return n;
}

int main(void) {
  char line[65536];

  while (fgets(line, sizeof line, stdin) != NULL) {
    char *pat = NULL, *text = NULL;
    int patlen = 0, textlen = 0, flags = 0, start = 0, used;
    const char *err = NULL;
    struct regex *re;
    struct regex_match m;
    char *p = line;

    flags = (*p == '1');
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    start = atoi(p);
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    used = unhex(p, &pat, &patlen);
    if (used == 0 && (*p != ' ' && *p != '\n' && *p != '\0')) {
      printf("error bad-hex\n");
      free(pat);
      continue;
    }
    p += used;
    while (*p == ' ') p++;
    unhex(p, &text, &textlen);
    if (pat == NULL || text == NULL) {
      printf("error bad-line\n");
      free(pat);
      free(text);
      continue;
    }
    (void)patlen;

    re = regex_compile(pat, flags ? REGEX_ICASE : 0, &err);
    if (re == NULL) {
      printf("error %s\n", err ? err : "?");
    } else {
      int r = regex_exec(re, text, textlen, start, &m);
      if (r == REGEX_TOO_HARD) {
        printf("toohard\n");
      } else if (r == REGEX_NOMATCH) {
        printf("nomatch\n");
      } else {
        int i;
        printf("match");
        for (i = 0; i < m.ngroups; i++) printf(" %d,%d", m.so[i], m.eo[i]);
        printf("\n");
      }
      regex_free(re);
    }
    fflush(stdout);
    free(pat);
    free(text);
  }
  return 0;
}
