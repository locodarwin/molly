/* json.c -- reading and writing JSON, for the language server.
 *
 * The roadmap said hand-rolled JSON was "a few hundred lines", and that is the
 * whole argument for not taking a library: molly has no runtime dependencies,
 * and the protocol uses a small, well-behaved corner of JSON. What it does need
 * is to be right about the corners that a server actually produces -- escaped
 * quotes in a diagnostic message, \u escapes and surrogate pairs in a hover,
 * deeply nested completion items -- and to refuse rather than misread anything
 * malformed, because a misread message is a wrong answer shown as a right one.
 *
 * Reading builds a tree. A message is small and is looked at once, by code that
 * wants "the `range` of the third `diagnostics` entry", and a tree is what makes
 * that a few lookups rather than a state machine per message. The tree lives in
 * one document: nodes come out of chunks the document owns and strings out of
 * one block the size of the input -- a decoded string is never longer than its
 * encoding, since every escape is at least as long as what it decodes to -- so a
 * message is two or three allocations however many values it holds, and
 * json_free() is the only free.
 *
 * Writing is the other direction and much smaller: a string has to be escaped,
 * and everything else is printf. It goes into the same `struct abuf` the screen
 * code builds its output in. */

#include "molly.h"

/* Nodes per chunk. A diagnostics message for a file with a few problems is tens
 * of nodes; a completion list from clangd is a few thousand. Chunks rather than
 * one array that grows, because a node's address is what a parent keeps. */
#define JSON_CHUNK 256

/* How deep the reader will go. The protocol's deepest values are a dozen levels;
 * the limit is there so that a hostile or broken server sending ten thousand
 * opening brackets runs out of patience here rather than out of stack. */
#define JSON_MAX_DEPTH 128

struct json_chunk {
  struct json_chunk *next;
  int used;
  struct json nodes[JSON_CHUNK];
};

struct json_doc {
  struct json *root;
  struct json_chunk *chunks;
  char *strings;
  size_t strused;
};

struct reader {
  const char *s;
  size_t len, i;
  struct json_doc *doc;
};

static struct json *node_new(struct json_doc *doc, int type) {
  struct json *n;
  if (doc->chunks == NULL || doc->chunks->used == JSON_CHUNK) {
    struct json_chunk *c = xmalloc(sizeof(*c));
    c->next = doc->chunks;
    c->used = 0;
    doc->chunks = c;
  }
  n = &doc->chunks->nodes[doc->chunks->used++];
  memset(n, 0, sizeof(*n));
  n->type = type;
  return n;
}

static void skip_ws(struct reader *r) {
  while (r->i < r->len) {
    char c = r->s[r->i];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
    r->i++;
  }
}

static int hex4(const char *s, unsigned *out) {
  unsigned v = 0;
  int k;
  for (k = 0; k < 4; k++) {
    char c = s[k];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
    else return 0;
  }
  *out = v;
  return 1;
}

static int put_utf8(char *dst, unsigned cp) {
  if (cp < 0x80) {
    dst[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    dst[0] = (char)(0xC0 | (cp >> 6));
    dst[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    dst[0] = (char)(0xE0 | (cp >> 12));
    dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    dst[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  dst[0] = (char)(0xF0 | (cp >> 18));
  dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  dst[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

/* A string, with the reader on its opening quote. Decoded into the document's
 * string block; returns its start there, or NULL if it is malformed. */
static char *read_string(struct reader *r, int *outlen) {
  struct json_doc *doc = r->doc;
  char *start = doc->strings + doc->strused;
  char *d = start;

  r->i++;  /* the quote */
  while (r->i < r->len) {
    unsigned char c = (unsigned char)r->s[r->i++];
    if (c == '"') {
      *d = '\0';
      *outlen = (int)(d - start);
      doc->strused += (size_t)(d - start) + 1;
      return start;
    }
    if (c < 0x20) return NULL;  /* a raw control character is not allowed */
    if (c != '\\') {
      *d++ = (char)c;
      continue;
    }
    if (r->i >= r->len) return NULL;
    c = (unsigned char)r->s[r->i++];
    switch (c) {
    case '"': *d++ = '"'; break;
    case '\\': *d++ = '\\'; break;
    case '/': *d++ = '/'; break;
    case 'b': *d++ = '\b'; break;
    case 'f': *d++ = '\f'; break;
    case 'n': *d++ = '\n'; break;
    case 'r': *d++ = '\r'; break;
    case 't': *d++ = '\t'; break;
    case 'u': {
      unsigned cp, lo;
      if (r->i + 4 > r->len || !hex4(r->s + r->i, &cp)) return NULL;
      r->i += 4;
      /* A surrogate pair is two escapes for one character. A lone half -- which
       * a server that slices UTF-16 carelessly can send -- becomes U+FFFD
       * rather than a refusal: one wrong glyph in a hover beats no hover. */
      if (cp >= 0xD800 && cp <= 0xDBFF) {
        if (r->i + 6 <= r->len && r->s[r->i] == '\\' && r->s[r->i + 1] == 'u' &&
            hex4(r->s + r->i + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
          r->i += 6;
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        } else {
          cp = 0xFFFD;
        }
      } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
        cp = 0xFFFD;
      }
      d += put_utf8(d, cp);
      break;
    }
    default:
      return NULL;
    }
  }
  return NULL;  /* ran out before the closing quote */
}

/* A number, by hand rather than through strtod: strtod reads the decimal point
 * the locale says, and molly does not get to assume which locale it runs in.
 * Everything the protocol puts in a number is an integer, and a double holds
 * every integer it can send exactly. */
static int read_number(struct reader *r, double *out) {
  double v = 0, frac = 0, scale = 1;
  int neg = 0, digits = 0, exp = 0, eneg = 0;

  if (r->s[r->i] == '-') {
    neg = 1;
    r->i++;
  }
  while (r->i < r->len && isdigit((unsigned char)r->s[r->i])) {
    v = v * 10 + (r->s[r->i++] - '0');
    digits++;
  }
  if (digits == 0) return 0;
  if (r->i < r->len && r->s[r->i] == '.') {
    int fd = 0;
    r->i++;
    while (r->i < r->len && isdigit((unsigned char)r->s[r->i])) {
      scale /= 10;
      frac += (r->s[r->i++] - '0') * scale;
      fd++;
    }
    if (fd == 0) return 0;
    v += frac;
  }
  if (r->i < r->len && (r->s[r->i] == 'e' || r->s[r->i] == 'E')) {
    int ed = 0;
    r->i++;
    if (r->i < r->len && (r->s[r->i] == '+' || r->s[r->i] == '-'))
      eneg = (r->s[r->i++] == '-');
    while (r->i < r->len && isdigit((unsigned char)r->s[r->i])) {
      if (exp < 400) exp = exp * 10 + (r->s[r->i] - '0');
      r->i++;
      ed++;
    }
    if (ed == 0) return 0;
    while (exp-- > 0) v = eneg ? v / 10 : v * 10;
  }
  *out = neg ? -v : v;
  return 1;
}

static int match_word(struct reader *r, const char *w) {
  size_t n = strlen(w);
  if (r->len - r->i < n || memcmp(r->s + r->i, w, n) != 0) return 0;
  r->i += n;
  return 1;
}

static struct json *read_value(struct reader *r, int depth) {
  struct json *v, *last = NULL;
  char c;

  if (depth > JSON_MAX_DEPTH) return NULL;
  skip_ws(r);
  if (r->i >= r->len) return NULL;
  c = r->s[r->i];

  if (c == '"') {
    int n;
    char *s = read_string(r, &n);
    if (s == NULL) return NULL;
    v = node_new(r->doc, JSON_STRING);
    v->str = s;
    v->n = n;
    return v;
  }
  if (c == '-' || isdigit((unsigned char)c)) {
    v = node_new(r->doc, JSON_NUMBER);
    return read_number(r, &v->num) ? v : NULL;
  }
  if (match_word(r, "true")) return node_new(r->doc, JSON_TRUE);
  if (match_word(r, "false")) return node_new(r->doc, JSON_FALSE);
  if (match_word(r, "null")) return node_new(r->doc, JSON_NULL);

  if (c != '[' && c != '{') return NULL;
  v = node_new(r->doc, c == '[' ? JSON_ARRAY : JSON_OBJECT);
  r->i++;
  skip_ws(r);
  if (r->i < r->len && r->s[r->i] == (c == '[' ? ']' : '}')) {
    r->i++;
    return v;
  }
  for (;;) {
    struct json *kid;
    const char *key = NULL;
    if (c == '{') {
      int n;
      skip_ws(r);
      if (r->i >= r->len || r->s[r->i] != '"') return NULL;
      key = read_string(r, &n);
      if (key == NULL) return NULL;
      skip_ws(r);
      if (r->i >= r->len || r->s[r->i] != ':') return NULL;
      r->i++;
    }
    kid = read_value(r, depth + 1);
    if (kid == NULL) return NULL;
    kid->key = key;
    if (last == NULL) v->kid = kid;
    else last->next = kid;
    last = kid;
    v->n++;
    skip_ws(r);
    if (r->i >= r->len) return NULL;
    if (r->s[r->i] == ',') {
      r->i++;
      continue;
    }
    if (r->s[r->i] == (c == '[' ? ']' : '}')) {
      r->i++;
      return v;
    }
    return NULL;
  }
}

void json_free(struct json_doc *doc) {
  if (doc == NULL) return;
  while (doc->chunks != NULL) {
    struct json_chunk *c = doc->chunks;
    doc->chunks = c->next;
    free(c);
  }
  free(doc->strings);
  free(doc);
}

/* Parse `len` bytes, which need not be NUL-terminated. NULL if they are not one
 * JSON value with nothing but whitespace after it. */
struct json_doc *json_parse(const char *s, size_t len) {
  struct reader r;
  struct json_doc *doc = xmalloc(sizeof(*doc));
  memset(doc, 0, sizeof(*doc));
  doc->strings = xmalloc(len + 1);
  r.s = s;
  r.len = len;
  r.i = 0;
  r.doc = doc;
  doc->root = read_value(&r, 0);
  skip_ws(&r);
  if (doc->root == NULL || r.i != r.len) {
    json_free(doc);
    return NULL;
  }
  return doc;
}

const struct json *json_root(const struct json_doc *doc) {
  return doc != NULL ? doc->root : NULL;
}

/* ---- looking things up --------------------------------------------------- */

/* Every accessor takes NULL and a value of the wrong type and answers "not
 * there", so a chain like json_get(json_get(msg, "range"), "start") needs one
 * check at the end rather than one per step. A server that leaves out a field
 * the protocol says is optional is the common case, not an error. */

const struct json *json_get(const struct json *obj, const char *key) {
  const struct json *k;
  if (obj == NULL || obj->type != JSON_OBJECT) return NULL;
  for (k = obj->kid; k != NULL; k = k->next)
    if (strcmp(k->key, key) == 0) return k;
  return NULL;
}

const struct json *json_at(const struct json *arr, int i) {
  const struct json *k;
  if (arr == NULL || arr->type != JSON_ARRAY || i < 0) return NULL;
  for (k = arr->kid; k != NULL && i > 0; k = k->next) i--;
  return k;
}

const char *json_str(const struct json *v) {
  return (v != NULL && v->type == JSON_STRING) ? v->str : NULL;
}

/* An integer, or `dflt` if this is not a number. Clamped to int, which is what
 * every caller stores it in: a line number past two billion is not a line. */
int json_int(const struct json *v, int dflt) {
  if (v == NULL || v->type != JSON_NUMBER) return dflt;
  if (v->num >= (double)INT_MAX) return INT_MAX;
  if (v->num <= (double)INT_MIN) return INT_MIN;
  return (int)v->num;
}

int json_bool(const struct json *v, int dflt) {
  if (v == NULL) return dflt;
  if (v->type == JSON_TRUE) return 1;
  if (v->type == JSON_FALSE) return 0;
  return dflt;
}

/* ---- writing ------------------------------------------------------------- */

/* A JSON string of `len` bytes, quotes included.
 *
 * The text is a buffer's, which is bytes and not necessarily UTF-8, and JSON
 * has to be. A byte that does not start a valid sequence goes out as U+FFFD:
 * a server that is sent invalid UTF-8 is entitled to drop the whole message,
 * and losing one character's identity is better than losing the document. */
void json_put_str(struct abuf *ab, const char *s, int len) {
  char esc[8];
  int i = 0, run = 0;

  ab_append(ab, "\"", 1);
  while (i < len) {
    unsigned char c = (unsigned char)s[i];
    const char *rep = NULL;
    int replen = 0;

    if (c == '"') rep = "\\\"";
    else if (c == '\\') rep = "\\\\";
    else if (c == '\n') rep = "\\n";
    else if (c == '\r') rep = "\\r";
    else if (c == '\t') rep = "\\t";
    else if (c < 0x20 || c == 0x7f) {
      snprintf(esc, sizeof(esc), "\\u%04x", c);
      rep = esc;
    } else if (c >= 0x80) {
      int n = utf8_char_bytes(s, len, i);
      if (n == 1 || (c == 0xC0 || c == 0xC1 || c > 0xF4)) {
        rep = "\xEF\xBF\xBD";
        replen = 3;
      } else {
        i += n;
        run += n;
        continue;
      }
    } else {
      i++;
      run++;
      continue;
    }
    if (run > 0) ab_append(ab, s + i - run, run);
    run = 0;
    ab_append(ab, rep, replen ? replen : (int)strlen(rep));
    i++;
  }
  if (run > 0) ab_append(ab, s + i - run, run);
  ab_append(ab, "\"", 1);
}

/* printf into the buffer, for the parts of a message that are not text: keys,
 * numbers, punctuation. Any length: it was a fixed 512 bytes, and the client's
 * capabilities outgrew that the day signature help joined them -- a message
 * cut short is invalid JSON, and the server that receives it rightly dies. */
void json_putf(struct abuf *ab, const char *fmt, ...) {
  char buf[512], *big;
  va_list ap;
  int n;
  va_start(ap, fmt);
  n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n < (int)sizeof(buf)) {
    ab_append(ab, buf, n);
    return;
  }
  big = xmalloc((size_t)n + 1);
  va_start(ap, fmt);
  vsnprintf(big, (size_t)n + 1, fmt, ap);
  va_end(ap);
  ab_append(ab, big, n);
  free(big);
}
