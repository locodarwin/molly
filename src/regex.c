/* regex.c -- a regular-expression engine: a PCRE subset, hand-rolled.
 *
 * olly left regular expressions out on the grounds that a matching engine is a
 * dependency. The answer to that was that POSIX <regex.h> is already linked, and
 * the answer to *that* is that POSIX ERE is not what anybody means when they type
 * a regular expression: no \d, no lazy quantifiers, no shorthands at all. So this
 * is the third option -- an engine of our own that speaks the subset people
 * actually type.
 *
 * In: literals, `.`, classes with ranges and the \d \w \s shorthands and their
 * negations, greedy and lazy `* + ? {m,n}`, groups both capturing and `(?:...)`,
 * alternation, the anchors `^` and `$`, the word boundaries \b and \B, and
 * case-insensitive matching. Matching is over raw bytes but advances a whole
 * UTF-8 character at a time, so `.` and a class consume a character rather than a
 * byte and a match can never end in the middle of one.
 *
 * Out: lookaround, backreferences inside the pattern, named groups, possessive
 * quantifiers, \x and \u escapes, POSIX [:alpha:] classes, and anything
 * multiline -- molly matches one row at a time, so `^` and `$` are the ends of
 * the row and `.` never has a newline to not match.
 *
 * Case folding is ASCII only, the same stance and for the same reason as the
 * literal search in search.c: folding by locale would make the same pattern
 * match differently under a Turkish locale, and molly compares raw bytes
 * everywhere else.
 *
 * The matcher backtracks. That was the decision rather than a Thompson
 * simulation because backtracking is the shape that can grow lookaround and
 * backreferences later, and the price is that a pattern like (a+)+$ takes
 * exponential time on a line it does not match. Two counters bound it: a step
 * budget, and a recursion depth -- every step of a backtracking match is a C
 * stack frame, so depth is what would overflow the stack rather than merely take
 * a while. Blowing either one gives REGEX_TOO_HARD, and the caller says "pattern
 * too complex" instead of the editor hanging. Two things keep those limits from
 * being reached by anything ordinary: a quantified single character (`.*`,
 * `[0-9]+`) is matched by walking the run rather than recursing down it, and a
 * pattern that cannot match at all is usually ruled out by a memchr before the
 * search starts. What is left for the limits to catch is genuinely nested
 * quantifiers.
 *
 * Two corners have no single right answer, because Perl and Python disagree with
 * each other about them: which iteration of a repetition a capture ends up
 * holding when the body is lazy, and whether a capture inside a repeated group
 * is cleared at every iteration or keeps its last value. Molly's answers fall
 * out of the design below rather than imitating either engine -- it follows Perl
 * on the first and Python on the second -- and tests/test_units.c pins both so
 * that neither can change by accident.
 */

#include "molly.h"

#define RX_MAX_NODES  8192     /* nodes in one compiled pattern */
#define RX_MAX_REPEAT 65535    /* the largest count in {m,n}. A repetition is a
                                * counted loop rather than copies of the
                                * subtree, so a large count costs matching time
                                * -- which the step budget bounds -- and not
                                * pattern size */
#define RX_MAX_NEST   64       /* ( nesting, to bound the parser's recursion */
#define RX_BUDGET     2000000L /* node visits per regex_exec call */
#define RX_MAX_DEPTH  10000    /* nested rx_m_node frames; see the note above */

#define RX_MAX_REQ    4        /* required characters remembered per pattern */

#define RX_CP_MAX 0x10FFFFu

enum rx_op {
  OP_EMPTY = 0, /* matches here and consumes nothing: what `a|` and `a{0}` are */
  OP_CHAR,      /* one literal character */
  OP_ANY,       /* . */
  OP_CLASS,     /* [...] and the \d \w \s shorthands */
  OP_BOL,       /* ^ */
  OP_EOL,       /* $ */
  OP_WORDB,     /* \b */
  OP_NWORDB,    /* \B */
  OP_CAT,       /* a then b */
  OP_ALT,       /* a, or b if a fails */
  OP_REP,       /* a, between rmin and rmax times, greedy or lazy */
  OP_CAPS,      /* a capturing group opens */
  OP_CAPE       /* and closes */
};

struct rx_range {
  uint32_t lo, hi;
};

/* Children are indices rather than pointers because the node array is grown with
 * realloc while the tree is being built, and an index survives that. */
struct rx_node {
  unsigned char op;
  unsigned char lazy;    /* OP_REP */
  unsigned char negate;  /* OP_CLASS */
  unsigned char lead;    /* OP_CHAR: its first byte, kept rather than re-derived
                          * from cp, so that a pattern holding malformed UTF-8
                          * still agrees with itself in the first-byte filter */
  int a, b;              /* OP_CAT and OP_ALT: the halves. OP_REP: a is the sub */
  uint32_t cp;           /* OP_CHAR */
  int first, count;      /* OP_CLASS: a slice of re->range */
  int cap;               /* OP_CAPS and OP_CAPE */
  int rmin, rmax;        /* OP_REP; rmax < 0 is unbounded */
};

struct regex {
  struct rx_node *node;
  int nnode, nodecap;
  struct rx_range *range;
  int nrange, rangecap;
  int root;
  int ngroups;    /* the whole match plus the capturing groups */
  int flags;

  /* A cheap prefilter. `anchored` means every path through the pattern starts
   * with ^, so only the start of the row is worth trying. `use_first` means the
   * pattern cannot match nothing, and so a match can only begin on a byte in
   * `first` -- which turns the leftmost scan of a long row into a memchr-shaped
   * walk for the patterns that have a definite first character. */
  int anchored;
  int use_first;
  unsigned char first[256];

  /* Characters every match has to contain, however it matches. If one of them is
   * nowhere in the row then neither is a match, and memchr settles that in a
   * pass. Without this, a pattern like .*zzz spends its whole step budget on a
   * long line working out what one memchr can say at once -- and then reports
   * "pattern too complex" for a line that simply does not contain a z. */
  unsigned char req[RX_MAX_REQ];
  int nreq;
};

/* --- characters -------------------------------------------------------------
 *
 * One "character", defined so that it reads the same forwards and backwards: a
 * byte, plus the run of UTF-8 continuation bytes after it. The one-character
 * matchers all advance by rx_char_bytes, and the greedy fast path walks a run
 * back one character at a time with rx_char_prev, so the two have to be exact
 * mirrors or the walk lands between characters. For well-formed UTF-8 this is
 * the character; for a stray continuation byte it groups with whatever precedes
 * it, which is not meaningful but is consistent, and consistency is the property
 * the walk actually needs. */

static int rx_char_bytes(const char *s, int len, int i) {
  int n = 1;
  while (i + n < len && utf8_is_cont((unsigned char)s[i + n])) n++;
  return n;
}

static int rx_char_prev(const char *s, int i) {
  int j = i - 1;
  while (j > 0 && utf8_is_cont((unsigned char)s[j])) j--;
  return j;
}

/* The pattern is NUL-terminated and NUL is not a continuation byte, so a
 * four-byte window is always enough to measure one character in it. */
#define RX_PAT_CHAR(p) rx_char_bytes((p), 4, 0)

static uint32_t rx_decode(const char *s, int i, int nb) {
  const unsigned char *u = (const unsigned char *)s + i;
  if (nb == 2)
    return ((uint32_t)(u[0] & 0x1F) << 6) | (uint32_t)(u[1] & 0x3F);
  if (nb == 3)
    return ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) |
           (uint32_t)(u[2] & 0x3F);
  if (nb == 4)
    return ((uint32_t)(u[0] & 0x07) << 18) | ((uint32_t)(u[1] & 0x3F) << 12) |
           ((uint32_t)(u[2] & 0x3F) << 6) | (uint32_t)(u[3] & 0x3F);
  return u[0];
}

static uint32_t rx_fold(uint32_t cp) {
  return (cp >= 'a' && cp <= 'z') ? cp - ('a' - 'A') : cp;
}

static uint32_t rx_swapcase(uint32_t cp) {
  if (cp >= 'a' && cp <= 'z') return cp - ('a' - 'A');
  if (cp >= 'A' && cp <= 'Z') return cp + ('a' - 'A');
  return cp;
}

/* What \w matches, and so also what \b counts as a word character. Everything
 * above ASCII is a word character: it makes \bcafé\b work, and the alternative
 * -- a Unicode letter table -- is a lot of table for a question molly only asks
 * at the edges of a match. */
static int rx_is_word(uint32_t cp) {
  return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') ||
         (cp >= 'a' && cp <= 'z') || cp == '_' || cp >= 0x80;
}

static const struct rx_range rx_tbl_digit[] = {{0x30, 0x39}};
static const struct rx_range rx_tbl_word[] = {
  {0x30, 0x39}, {0x41, 0x5A}, {0x5F, 0x5F}, {0x61, 0x7A}, {0x80, RX_CP_MAX}
};
static const struct rx_range rx_tbl_space[] = {{0x09, 0x0D}, {0x20, 0x20}};

#define RX_NELEM(a) ((int)(sizeof(a) / sizeof(*(a))))

/* \d \D \w \W \s \S -- the table the letter stands for, and whether it was the
 * negated form. Zero if ch is not one of the six. */
static int rx_shorthand(int ch, const struct rx_range **t, int *n, int *neg) {
  int lower = (ch >= 'A' && ch <= 'Z') ? ch + ('a' - 'A') : ch;
  *neg = (lower != ch);
  switch (lower) {
    case 'd': *t = rx_tbl_digit; *n = RX_NELEM(rx_tbl_digit); return 1;
    case 'w': *t = rx_tbl_word;  *n = RX_NELEM(rx_tbl_word);  return 1;
    case 's': *t = rx_tbl_space; *n = RX_NELEM(rx_tbl_space); return 1;
    default:  return 0;
  }
}

/* --- the parser ------------------------------------------------------------ */

struct rx_parse {
  const char *p;      /* the next pattern byte to look at */
  struct regex *re;
  const char *err;    /* the first thing that went wrong, which is the one to
                       * report: everything after it is a consequence */
  int ncap;
  int nest;
};

static int rx_fail(struct rx_parse *ps, const char *msg) {
  if (ps->err == NULL) ps->err = msg;
  return -1;
}

static int rx_node_new(struct rx_parse *ps, int op) {
  struct regex *re = ps->re;
  struct rx_node *n;

  if (re->nnode >= RX_MAX_NODES) return rx_fail(ps, "pattern is too large");
  if (re->nnode == re->nodecap) {
    re->nodecap = re->nodecap ? re->nodecap * 2 : 32;
    re->node = xrealloc(re->node, sizeof(*re->node) * (size_t)re->nodecap);
  }
  n = &re->node[re->nnode];
  memset(n, 0, sizeof(*n));
  n->op = (unsigned char)op;
  n->a = n->b = -1;
  n->cap = -1;
  return re->nnode++;
}

/* A two-child node, propagating a failure in either child rather than making
 * every call site test for one. */
static int rx_node2(struct rx_parse *ps, int op, int a, int b) {
  int k;

  if (a < 0 || b < 0) return -1;
  k = rx_node_new(ps, op);
  if (k < 0) return -1;
  ps->re->node[k].a = a;
  ps->re->node[k].b = b;
  return k;
}

static int rx_range_add(struct rx_parse *ps, uint32_t lo, uint32_t hi) {
  struct regex *re = ps->re;

  if (lo > hi) return rx_fail(ps, "character class range is out of order");
  if (re->nrange == re->rangecap) {
    re->rangecap = re->rangecap ? re->rangecap * 2 : 16;
    re->range = xrealloc(re->range, sizeof(*re->range) * (size_t)re->rangecap);
  }
  re->range[re->nrange].lo = lo;
  re->range[re->nrange].hi = hi;
  re->nrange++;
  return 0;
}

static int rx_range_add_table(struct rx_parse *ps, const struct rx_range *t,
                              int n) {
  int i;

  for (i = 0; i < n; i++)
    if (rx_range_add(ps, t[i].lo, t[i].hi) < 0) return -1;
  return 0;
}

/* The gaps in a sorted, non-overlapping table: what \D, \W and \S contribute
 * when they turn up *inside* a class, where there is no negate flag to borrow
 * and the items are unioned together. [^\D] then negates the complement and
 * comes out as the digits again, which is what it should be. */
static int rx_range_add_gaps(struct rx_parse *ps, const struct rx_range *t,
                             int n) {
  uint32_t next = 0;
  int i;

  for (i = 0; i < n; i++) {
    if (t[i].lo > next && rx_range_add(ps, next, t[i].lo - 1) < 0) return -1;
    next = t[i].hi + 1;
  }
  if (next <= RX_CP_MAX && rx_range_add(ps, next, RX_CP_MAX) < 0) return -1;
  return 0;
}

/* The escapes that stand for one character, shared by the class parser and the
 * atom parser. Returns the code point, or -1 having set the error. */
static int rx_escape_char(struct rx_parse *ps, int in_class) {
  int ch = (unsigned char)ps->p[1];

  if (ch == '\0') return rx_fail(ps, "pattern ends with a backslash");
  ps->p += 2;
  switch (ch) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    case 'a': return '\a';
    case 'e': return 0x1B;
    default: break;
  }
  /* Punctuation escaped to itself: \. \* \\ \] \- and the rest. A letter or
   * digit that got this far is a typo or a PCRE feature molly does not have,
   * and either way saying so beats matching the bare letter silently. */
  if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
      (ch >= '0' && ch <= '9'))
    return rx_fail(ps, in_class ? "unknown escape in character class"
                                : "unknown escape sequence");
  return ch;
}

/* One endpoint inside a class: an escape, or one UTF-8 character. */
static int rx_class_char(struct rx_parse *ps) {
  int nb;
  uint32_t cp;

  if (*ps->p == '\\') return rx_escape_char(ps, 1);
  if (*ps->p == '\0') return rx_fail(ps, "unterminated [ in pattern");
  nb = RX_PAT_CHAR(ps->p);
  cp = rx_decode(ps->p, 0, nb);
  ps->p += nb;
  return (int)cp;
}

/* Called with ps->p just past the '['. */
static int p_class(struct rx_parse *ps) {
  struct regex *re = ps->re;
  int k = rx_node_new(ps, OP_CLASS);
  int first, at_start = 1;

  if (k < 0) return -1;
  if (*ps->p == '^') {
    ps->p++;
    re->node[k].negate = 1;
  }
  first = re->nrange;
  for (;;) {
    const struct rx_range *t;
    int tn, neg, lo, hi;

    if (*ps->p == '\0') return rx_fail(ps, "unterminated [ in pattern");
    /* A ] first is the literal character; anywhere else it closes the class. */
    if (*ps->p == ']' && !at_start) break;
    at_start = 0;

    if (*ps->p == '\\' &&
        rx_shorthand((unsigned char)ps->p[1], &t, &tn, &neg)) {
      ps->p += 2;
      if ((neg ? rx_range_add_gaps(ps, t, tn)
               : rx_range_add_table(ps, t, tn)) < 0)
        return -1;
      continue;
    }

    lo = rx_class_char(ps);
    if (lo < 0) return -1;
    hi = lo;
    /* A - before the closing bracket is itself, which is how [a-] is read. */
    if (ps->p[0] == '-' && ps->p[1] != ']' && ps->p[1] != '\0') {
      ps->p++;
      hi = rx_class_char(ps);
      if (hi < 0) return -1;
    }
    if (rx_range_add(ps, (uint32_t)lo, (uint32_t)hi) < 0) return -1;
  }
  ps->p++;  /* the ] */
  re->node[k].first = first;
  re->node[k].count = re->nrange - first;
  if (re->node[k].count == 0) return rx_fail(ps, "empty character class");
  return k;
}

static int p_alt(struct rx_parse *ps);

static int p_escape_atom(struct rx_parse *ps) {
  const struct rx_range *t;
  int tn, neg, ch, k;

  ch = (unsigned char)ps->p[1];
  if (rx_shorthand(ch, &t, &tn, &neg)) {
    ps->p += 2;
    k = rx_node_new(ps, OP_CLASS);
    if (k < 0) return -1;
    ps->re->node[k].negate = (unsigned char)neg;
    ps->re->node[k].first = ps->re->nrange;
    if (rx_range_add_table(ps, t, tn) < 0) return -1;
    ps->re->node[k].count = ps->re->nrange - ps->re->node[k].first;
    return k;
  }
  if (ch == 'b' || ch == 'B') {
    ps->p += 2;
    return rx_node_new(ps, ch == 'b' ? OP_WORDB : OP_NWORDB);
  }
  ch = rx_escape_char(ps, 0);
  if (ch < 0) return -1;
  k = rx_node_new(ps, OP_CHAR);
  if (k < 0) return -1;
  ps->re->node[k].cp = (uint32_t)ch;
  ps->re->node[k].lead = (unsigned char)ch;
  return k;
}

static int p_atom(struct rx_parse *ps) {
  int k, nb;

  /* p_cat stops at the end of the pattern, so this is unreachable rather than
   * routine -- but a parser that walks off the end of a string on a pattern
   * nobody thought of is worse than one that says it is confused. */
  if (*ps->p == '\0') return rx_fail(ps, "pattern ends early");

  switch (*ps->p) {
    case '(': {
      int sub, caps, cape, cap = -1;
      ps->p++;
      if (ps->p[0] == '?') {
        if (ps->p[1] != ':')
          return rx_fail(ps, "only (?:...) is supported after (?");
        ps->p += 2;
      } else {
        if (ps->ncap + 1 >= REGEX_MAX_GROUPS)
          return rx_fail(ps, "too many capture groups");
        cap = ++ps->ncap;
      }
      if (++ps->nest > RX_MAX_NEST) return rx_fail(ps, "groups nest too deeply");
      sub = p_alt(ps);
      ps->nest--;
      if (sub < 0) return -1;
      if (*ps->p != ')') return rx_fail(ps, "missing ) in pattern");
      ps->p++;
      if (cap < 0) return sub;
      caps = rx_node_new(ps, OP_CAPS);
      if (caps < 0) return -1;
      ps->re->node[caps].cap = cap;
      cape = rx_node_new(ps, OP_CAPE);
      if (cape < 0) return -1;
      ps->re->node[cape].cap = cap;
      return rx_node2(ps, OP_CAT, rx_node2(ps, OP_CAT, caps, sub), cape);
    }
    case '[':  ps->p++; return p_class(ps);
    case '.':  ps->p++; return rx_node_new(ps, OP_ANY);
    case '^':  ps->p++; return rx_node_new(ps, OP_BOL);
    case '$':  ps->p++; return rx_node_new(ps, OP_EOL);
    case '\\': return p_escape_atom(ps);
    case '*': case '+': case '?':
      return rx_fail(ps, "nothing to repeat");
    default: break;
  }

  nb = RX_PAT_CHAR(ps->p);
  k = rx_node_new(ps, OP_CHAR);
  if (k < 0) return -1;
  ps->re->node[k].cp = rx_decode(ps->p, 0, nb);
  ps->re->node[k].lead = (unsigned char)ps->p[0];
  ps->p += nb;
  return k;
}

/* One repetition node. A bounded {m,n} is this too, counted at match time:
 * building it as m copies of the subtree plus n-m optional ones was the obvious
 * way and it was wrong, because a repetition has a rule that a sequence of
 * optional copies cannot express -- an iteration that matches nothing ends the
 * repetition rather than going round again. With copies, ((|0)){,2}: against
 * "0:" put the capture on the first copy where PCRE and Python put it on the
 * second. Counting also means a{1000} is one node rather than a thousand. */
static int rx_rep(struct rx_parse *ps, int sub, int mn, int mx, int lazy) {
  int k;

  if (sub < 0) return -1;
  k = rx_node_new(ps, OP_REP);
  if (k < 0) return -1;
  ps->re->node[k].a = sub;
  ps->re->node[k].rmin = mn;
  ps->re->node[k].rmax = mx;
  ps->re->node[k].lazy = (unsigned char)lazy;
  return k;
}

/* {m}, {m,} or {m,n}. Returns 1 having consumed it, 0 if this { is not a
 * quantifier at all -- which is a literal brace, and people do type those -- or
 * -1 having set an error for a quantifier that is one but is out of range.
 *
 * A comma with nothing before it is a zero, so {,3} is {0,3} and {,} is * --
 * which is what both Python and PCRE2 make of them, and a brace with neither a
 * comma nor a digit in it is just a brace. */
static int rx_brace(struct rx_parse *ps, int *mn, int *mx) {
  const char *q = ps->p + 1;
  long lo = 0, hi;
  int digits = 0;

  while (*q >= '0' && *q <= '9') {
    if (lo < 100000) lo = lo * 10 + (*q - '0');
    q++;
    digits++;
  }
  hi = lo;
  if (*q == ',') {
    q++;
    if (*q == '}') {
      hi = -1;
    } else {
      int d2 = 0;
      hi = 0;
      while (*q >= '0' && *q <= '9') {
        if (hi < 100000) hi = hi * 10 + (*q - '0');
        q++;
        d2++;
      }
      if (d2 == 0) return 0;
    }
  } else if (digits == 0) {
    return 0;  /* {} and {oops} are the literal characters */
  }
  if (*q != '}') return 0;
  ps->p = q + 1;
  if (lo > RX_MAX_REPEAT || hi > RX_MAX_REPEAT)
    return rx_fail(ps, "repeat count is too large");
  if (hi >= 0 && hi < lo) return rx_fail(ps, "repeat counts are out of order");
  *mn = (int)lo;
  *mx = (int)hi;
  return 1;
}

static int p_rep(struct rx_parse *ps) {
  int a = p_atom(ps);
  int quantified = 0;

  if (a < 0) return -1;
  for (;;) {
    int ch = (unsigned char)*ps->p;
    int brace = 0, lazy, mn = 0, mx = 0;

    if (ch == '{') {
      brace = rx_brace(ps, &mn, &mx);
      if (brace < 0) return -1;
      if (brace == 0) break;
    } else if (ch == '*' || ch == '+' || ch == '?') {
      ps->p++;
    } else {
      break;
    }
    /* a** and a*+ land here. PCRE reads the second one as possessive; molly
     * does not have possessive quantifiers, and reading it as "repeat a repeat"
     * would quietly mean something else, so it is an error. */
    if (quantified) return rx_fail(ps, "a quantifier cannot follow a quantifier");
    quantified = 1;
    lazy = (*ps->p == '?');
    if (lazy) ps->p++;

    if (brace)            a = rx_rep(ps, a, mn, mx, lazy);
    else if (ch == '*')   a = rx_rep(ps, a, 0, -1, lazy);
    else if (ch == '+')   a = rx_rep(ps, a, 1, -1, lazy);
    else                  a = rx_rep(ps, a, 0, 1, lazy);
    if (a < 0) return -1;
  }
  return a;
}

static int p_cat(struct rx_parse *ps) {
  int seq = -1;

  while (*ps->p != '\0' && *ps->p != '|' && *ps->p != ')') {
    int item = p_rep(ps);
    seq = (seq < 0) ? item : rx_node2(ps, OP_CAT, seq, item);
    if (seq < 0) return -1;
  }
  if (seq < 0) seq = rx_node_new(ps, OP_EMPTY);
  return seq;
}

static int p_alt(struct rx_parse *ps) {
  int left = p_cat(ps);

  while (left >= 0 && *ps->p == '|') {
    ps->p++;
    left = rx_node2(ps, OP_ALT, left, p_cat(ps));
  }
  return left;
}

/* --- what the prefilter works out ----------------------------------------- */

static int rx_class_in(const struct regex *re, const struct rx_node *n,
                       uint32_t cp) {
  int i;

  for (i = 0; i < n->count; i++) {
    const struct rx_range *r = &re->range[n->first + i];
    if (cp >= r->lo && cp <= r->hi) return 1;
  }
  return 0;
}

/* Membership, with the case fold applied before the negation rather than after:
 * [^a] under REGEX_ICASE must reject 'A' too, and testing the swapped case
 * against the negated answer would have accepted it. */
static int rx_class_match(const struct regex *re, const struct rx_node *n,
                          int icase, uint32_t cp) {
  int in = rx_class_in(re, n, cp);

  if (!in && icase) in = rx_class_in(re, n, rx_swapcase(cp));
  return n->negate ? !in : in;
}

static int rx_can_be_empty(const struct regex *re, int i) {
  const struct rx_node *n = &re->node[i];

  switch (n->op) {
    case OP_CHAR: case OP_ANY: case OP_CLASS:
      return 0;
    case OP_CAT:
      return rx_can_be_empty(re, n->a) && rx_can_be_empty(re, n->b);
    case OP_ALT:
      return rx_can_be_empty(re, n->a) || rx_can_be_empty(re, n->b);
    case OP_REP:
      return n->rmin == 0 || rx_can_be_empty(re, n->a);
    default:
      return 1;
  }
}

/* True when every path through the pattern has to start at the beginning of the
 * row, so the leftmost scan has exactly one position to try. */
static int rx_always_bol(const struct regex *re, int i) {
  const struct rx_node *n = &re->node[i];

  switch (n->op) {
    case OP_BOL:
      return 1;
    case OP_CAT:
      return rx_always_bol(re, n->a) ||
             (rx_can_be_empty(re, n->a) && rx_always_bol(re, n->b));
    case OP_ALT:
      return rx_always_bol(re, n->a) && rx_always_bol(re, n->b);
    case OP_REP:
      /* A repetition that may run zero times has a path through it that never
       * reaches the ^ inside it. */
      return n->rmin > 0 && rx_always_bol(re, n->a);
    default:
      return 0;
  }
}

/* The bytes a match can begin with. Returns whether this subtree can match
 * nothing, because a subtree that can is transparent -- the first byte then
 * comes from whatever follows it. */
static int rx_first(const struct regex *re, int i, unsigned char *set) {
  const struct rx_node *n = &re->node[i];
  int b, ea, eb, j, high;
  uint32_t cp;

  switch (n->op) {
    case OP_CHAR:
      set[n->lead] = 1;
      if (re->flags & REGEX_ICASE)
        set[(unsigned char)rx_swapcase(n->lead)] = 1;
      return 0;
    case OP_ANY:
      for (b = 0; b < 256; b++)
        if (b != '\n') set[b] = 1;
      return 0;
    case OP_CLASS:
      for (cp = 0; cp < 0x80; cp++)
        if (rx_class_match(re, n, re->flags & REGEX_ICASE, cp))
          set[cp] = 1;
      /* Above ASCII a character arrives as a lead byte, and working out which
       * lead bytes a code-point range can produce is more arithmetic than a
       * filter is worth: if the class admits any non-ASCII character at all,
       * admit every non-ASCII byte. Being too generous here only costs a
       * position that will fail to match; being too mean would lose a match. */
      high = n->negate;
      for (j = 0; !high && j < n->count; j++)
        high = re->range[n->first + j].hi >= 0x80;
      if (high)
        for (b = 0x80; b < 256; b++) set[b] = 1;
      return 0;
    case OP_CAT:
      return rx_first(re, n->a, set) ? rx_first(re, n->b, set) : 0;
    case OP_ALT:
      ea = rx_first(re, n->a, set);
      eb = rx_first(re, n->b, set);
      return ea || eb;
    case OP_REP:
      /* A repetition that has to run at least once is as opaque as its sub. */
      return rx_first(re, n->a, set) || n->rmin == 0;
    default:
      return 1;  /* the anchors, the captures and OP_EMPTY consume nothing */
  }
}

/* The characters every match must contain. Conservative: a class requires no
 * particular byte, and for an alternation only what both branches require counts,
 * which is more bookkeeping than a filter is worth -- so neither contributes. */
static void rx_required(const struct regex *re, int i, unsigned char *out,
                        int *n) {
  const struct rx_node *nd = &re->node[i];

  if (*n >= RX_MAX_REQ) return;
  switch (nd->op) {
    case OP_CHAR: {
      int j;
      for (j = 0; j < *n; j++)
        if (out[j] == nd->lead) return;  /* zzz needs one z looked for, not three */
      if (*n < RX_MAX_REQ) out[(*n)++] = nd->lead;
      return;
    }
    case OP_CAT:
      rx_required(re, nd->a, out, n);
      rx_required(re, nd->b, out, n);
      return;
    case OP_REP:
      if (nd->rmin > 0) rx_required(re, nd->a, out, n);
      return;
    default:
      return;
  }
}

/* --- the matcher ----------------------------------------------------------- */

struct rx_ctx {
  const struct regex *re;
  const char *text;
  int len;
  int icase;
  long steps;
  int depth;
  int overrun;
  int so[REGEX_MAX_GROUPS];
  int eo[REGEX_MAX_GROUPS];
};

/* What is still to be matched, as a list built on the C stack. A continuation is
 * either a node to match next, or the marker that one iteration of a star has
 * finished and the star should decide whether to go round again. */
struct rx_cont {
  int node;             /* or -1 for the marker */
  int rep;              /* which OP_REP, for the marker */
  int count;            /* iterations finished, including the one just now */
  int from;             /* where the iteration that just finished began */
  struct rx_cont *next;
};

static int rx_m_node(struct rx_ctx *c, int ni, int pos, struct rx_cont *k);
static int rx_m_cont(struct rx_ctx *c, struct rx_cont *k, int pos);
static int rx_m_rep(struct rx_ctx *c, int ni, int count, int pos,
                    struct rx_cont *k);

static int rx_is_one(const struct regex *re, int i) {
  int op = re->node[i].op;

  return op == OP_CHAR || op == OP_ANY || op == OP_CLASS;
}

/* The one-character matchers: how many bytes this node consumes at pos, or 0 if
 * it does not match there. */
static int rx_one(struct rx_ctx *c, int ni, int pos) {
  const struct rx_node *n = &c->re->node[ni];
  uint32_t cp;
  int nb;

  if (pos >= c->len) return 0;
  nb = rx_char_bytes(c->text, c->len, pos);
  cp = rx_decode(c->text, pos, nb);
  switch (n->op) {
    case OP_CHAR:
      if (cp == n->cp) return nb;
      if (c->icase && rx_fold(cp) == rx_fold(n->cp)) return nb;
      return 0;
    case OP_ANY:
      return cp == '\n' ? 0 : nb;
    case OP_CLASS:
      return rx_class_match(c->re, n, c->icase, cp) ? nb : 0;
    default:
      return 0;
  }
}

static int rx_spend(struct rx_ctx *c) {
  if (++c->steps > RX_BUDGET) {
    c->overrun = 1;
    return 0;
  }
  return 1;
}

/* A quantified single character, matched by walking the run rather than
 * recursing once per repetition. This is the case that would otherwise put one C
 * stack frame per matched character on the stack -- `.*` against a long line --
 * and it is also the overwhelmingly common one. Walking back over the run is
 * legal because every unit consumed was one whole character by rx_char_bytes,
 * and rx_char_prev is its mirror. A one-character sub can never match nothing,
 * so the empty-iteration rule has nothing to say here. */
static int rx_rep_simple(struct rx_ctx *c, int ni, int pos, struct rx_cont *k) {
  const struct rx_node *n = &c->re->node[ni];
  int sub = n->a, p = pos, adv, got = 0, floor;

  while (got < n->rmin) {  /* the minimum is not optional */
    adv = rx_one(c, sub, p);
    if (adv == 0) return 0;
    p += adv;
    got++;
    if (!rx_spend(c)) return 0;
  }
  floor = p;  /* the greedy walk may not back up past the minimum */
  if (n->lazy) {
    for (;;) {
      if (rx_m_cont(c, k, p)) return 1;
      if (c->overrun) return 0;
      if (n->rmax >= 0 && got >= n->rmax) return 0;
      adv = rx_one(c, sub, p);
      if (adv == 0) return 0;
      p += adv;
      got++;
      if (!rx_spend(c)) return 0;
    }
  }
  while ((n->rmax < 0 || got < n->rmax) && (adv = rx_one(c, sub, p)) > 0) {
    p += adv;
    got++;
    if (!rx_spend(c)) return 0;
  }
  for (;;) {
    if (rx_m_cont(c, k, p)) return 1;
    if (c->overrun) return 0;
    if (p <= floor) return 0;
    p = rx_char_prev(c->text, p);
    if (p < floor) p = floor;  /* only reachable if pos was mid-character */
    if (!rx_spend(c)) return 0;
  }
}

/* One decision point of a repetition: `count` iterations are behind us, and the
 * question is whether to try another or to carry on with what follows. Greedy
 * tries another first and lazy tries carrying on first, which is the whole of
 * what the two words mean. */
static int rx_m_rep(struct rx_ctx *c, int ni, int count, int pos,
                    struct rx_cont *k) {
  const struct rx_node *n = &c->re->node[ni];
  struct rx_cont back;
  int can_more = (n->rmax < 0 || count < n->rmax);

  if (rx_is_one(c->re, n->a)) return rx_rep_simple(c, ni, pos, k);

  back.node = -1;
  back.rep = ni;
  back.count = count + 1;
  back.from = pos;
  back.next = k;

  if (count < n->rmin)
    return can_more ? rx_m_node(c, n->a, pos, &back) : 0;
  if (n->lazy) {
    if (rx_m_cont(c, k, pos)) return 1;
    if (c->overrun) return 0;
    return can_more ? rx_m_node(c, n->a, pos, &back) : 0;
  }
  if (can_more && rx_m_node(c, n->a, pos, &back)) return 1;
  if (c->overrun) return 0;
  return rx_m_cont(c, k, pos);
}

static int rx_m_cont(struct rx_ctx *c, struct rx_cont *k, int pos) {
  if (k == NULL) {
    c->eo[0] = pos;
    return 1;
  }
  if (k->node >= 0) return rx_m_node(c, k->node, pos, k->next);
  /* One iteration of k->rep has just matched. If it matched nothing, going round
   * again would match nothing again, forever: that is the (a*)* hang, and the
   * guard is to stop iterating the moment an iteration fails to advance.
   *
   * Except while the minimum is unmet, where iterating is finite anyway because
   * the count is climbing toward it -- and where stopping early would be wrong
   * rather than merely different: (a?){3} has to be allowed its three
   * iterations, and which one of them a capture ends up holding is visible. */
  if (pos == k->from && k->count >= c->re->node[k->rep].rmin)
    return rx_m_cont(c, k->next, pos);
  return rx_m_rep(c, k->rep, k->count, pos, k->next);
}

static int rx_m_body(struct rx_ctx *c, int ni, int pos, struct rx_cont *k) {
  const struct rx_node *n = &c->re->node[ni];
  int adv, save, r;

  switch (n->op) {
    case OP_EMPTY:
      return rx_m_cont(c, k, pos);
    case OP_CHAR: case OP_ANY: case OP_CLASS:
      adv = rx_one(c, ni, pos);
      return adv > 0 ? rx_m_cont(c, k, pos + adv) : 0;
    case OP_BOL:
      return pos == 0 ? rx_m_cont(c, k, pos) : 0;
    case OP_EOL:
      return pos == c->len ? rx_m_cont(c, k, pos) : 0;
    case OP_WORDB: case OP_NWORDB: {
      int before = 0, after = 0, at;
      if (pos > 0) {
        at = rx_char_prev(c->text, pos);
        before = rx_is_word(rx_decode(c->text, at,
                                      rx_char_bytes(c->text, c->len, at)));
      }
      if (pos < c->len)
        after = rx_is_word(rx_decode(c->text, pos,
                                     rx_char_bytes(c->text, c->len, pos)));
      r = (before != after);
      if (n->op == OP_NWORDB) r = !r;
      return r ? rx_m_cont(c, k, pos) : 0;
    }
    case OP_CAT: {
      struct rx_cont k2;
      k2.node = n->b;
      k2.rep = -1;
      k2.count = 0;
      k2.from = 0;
      k2.next = k;
      return rx_m_node(c, n->a, pos, &k2);
    }
    case OP_ALT:
      if (rx_m_node(c, n->a, pos, k)) return 1;
      if (c->overrun) return 0;
      return rx_m_node(c, n->b, pos, k);
    case OP_REP:
      return rx_m_rep(c, ni, 0, pos, k);
    /* A capture is set on the way through and put back if what follows fails,
     * so a group that matched inside an alternative that was abandoned does not
     * leave its offsets behind. */
    case OP_CAPS:
      save = c->so[n->cap];
      c->so[n->cap] = pos;
      if (rx_m_cont(c, k, pos)) return 1;
      c->so[n->cap] = save;
      return 0;
    case OP_CAPE:
      save = c->eo[n->cap];
      c->eo[n->cap] = pos;
      if (rx_m_cont(c, k, pos)) return 1;
      c->eo[n->cap] = save;
      return 0;
    default:
      return 0;
  }
}

/* The accounting is here, around one call, rather than spread through the body:
 * the body has a dozen returns and a missed decrement would make the depth limit
 * drift down until an honest pattern was refused. */
static int rx_m_node(struct rx_ctx *c, int ni, int pos, struct rx_cont *k) {
  int r;

  if (c->overrun) return 0;
  if (!rx_spend(c)) return 0;
  if (++c->depth > RX_MAX_DEPTH) {
    c->overrun = 1;
    c->depth--;
    return 0;
  }
  r = rx_m_body(c, ni, pos, k);
  c->depth--;
  return r;
}

/* --- the interface -------------------------------------------------------- */

struct regex *regex_compile(const char *pat, int flags, const char **err) {
  struct rx_parse ps;
  struct regex *re;
  unsigned char set[256];

  if (err) *err = NULL;
  if (pat == NULL) {
    if (err) *err = "no pattern";
    return NULL;
  }

  re = xmalloc(sizeof *re);
  memset(re, 0, sizeof *re);
  re->flags = flags;

  ps.p = pat;
  ps.re = re;
  ps.err = NULL;
  ps.ncap = 0;
  ps.nest = 0;

  re->root = p_alt(&ps);
  if (re->root >= 0 && *ps.p != '\0')
    re->root = rx_fail(&ps, *ps.p == ')' ? "unmatched ) in pattern"
                                         : "pattern is not understood");
  if (re->root < 0) {
    if (err) *err = ps.err ? ps.err : "pattern is not understood";
    regex_free(re);
    return NULL;
  }

  re->ngroups = ps.ncap + 1;
  re->anchored = rx_always_bol(re, re->root);
  rx_required(re, re->root, re->req, &re->nreq);
  memset(set, 0, sizeof set);
  re->use_first = !rx_first(re, re->root, set);
  memcpy(re->first, set, sizeof re->first);
  return re;
}

void regex_free(struct regex *re) {
  if (re == NULL) return;
  free(re->node);
  free(re->range);
  free(re);
}

int regex_ngroups(const struct regex *re) {
  return re ? re->ngroups : 0;
}

int regex_exec(const struct regex *re, const char *text, int len, int start,
               struct regex_match *m) {
  struct rx_ctx c;
  int pos, i;

  if (re == NULL || text == NULL || len < 0) return REGEX_NOMATCH;
  if (start < 0) start = 0;
  if (start > len) return REGEX_NOMATCH;

  c.re = re;
  c.text = text;
  c.len = len;
  c.icase = (re->flags & REGEX_ICASE) != 0;
  /* One budget for the whole call, not one per starting position: what a caller
   * needs bounded is the time to search a row, and a pattern that is slow at
   * every position is exactly the case a per-position budget would miss. */
  c.steps = 0;
  c.overrun = 0;

  /* The cheap no first: a required character that is not there at all. */
  for (i = 0; i < re->nreq; i++) {
    unsigned char b = re->req[i];
    if (memchr(text + start, b, (size_t)(len - start)) != NULL) continue;
    if (c.icase) {
      unsigned char other = (unsigned char)rx_swapcase(b);
      if (other != b &&
          memchr(text + start, other, (size_t)(len - start)) != NULL)
        continue;
    }
    return REGEX_NOMATCH;
  }

  for (pos = start; pos <= len; ) {
    if (re->anchored && pos > 0) break;
    if (re->use_first && pos < len && !re->first[(unsigned char)text[pos]]) {
      pos += rx_char_bytes(text, len, pos);
      continue;
    }
    for (i = 0; i < REGEX_MAX_GROUPS; i++) c.so[i] = c.eo[i] = -1;
    c.so[0] = pos;
    c.depth = 0;
    if (rx_m_node(&c, re->root, pos, NULL)) {
      if (m) {
        m->ngroups = re->ngroups;
        for (i = 0; i < REGEX_MAX_GROUPS; i++) {
          m->so[i] = c.so[i];
          m->eo[i] = c.eo[i];
        }
      }
      return REGEX_MATCH;
    }
    if (c.overrun) return REGEX_TOO_HARD;
    if (pos == len) break;
    pos += rx_char_bytes(text, len, pos);
  }
  return REGEX_NOMATCH;
}

/* --- building the replacement --------------------------------------------- */

static void rx_out(char **buf, int *len, int *cap, const char *s, int n) {
  if (*len + n + 1 > *cap) {
    while (*len + n + 1 > *cap) *cap = *cap ? *cap * 2 : 64;
    *buf = xrealloc(*buf, (size_t)*cap);
  }
  memcpy(*buf + *len, s, (size_t)n);
  *len += n;
}

char *regex_expand(const char *tmpl, const char *text,
                   const struct regex_match *m, int *outlen,
                   const char **err) {
  char *out = NULL;
  int len = 0, cap = 0;
  const char *t;

  if (err) *err = NULL;
  if (tmpl == NULL || m == NULL) {
    if (err) *err = "nothing to expand";
    return NULL;
  }
  rx_out(&out, &len, &cap, "", 0);

  for (t = tmpl; *t != '\0'; ) {
    int ch = (unsigned char)t[1];

    if (*t != '\\') {
      rx_out(&out, &len, &cap, t, 1);
      t++;
      continue;
    }
    if (ch == '\0') {
      free(out);
      if (err) *err = "the replacement ends with a backslash";
      return NULL;
    }
    if (ch >= '0' && ch <= '9') {
      int g = ch - '0';
      if (g >= m->ngroups) {
        free(out);
        if (err) *err = "the pattern has no group with that number";
        return NULL;
      }
      /* A group inside an alternative that was not taken never matched, and
       * substitutes as nothing rather than as an error: (a)|(b) is written
       * precisely so that one of the two is absent. */
      if (m->so[g] >= 0 && m->eo[g] >= m->so[g])
        rx_out(&out, &len, &cap, text + m->so[g], m->eo[g] - m->so[g]);
      t += 2;
      continue;
    }
    /* A row holds no newline -- that is the buffer's invariant, not a limit of
     * this function -- so a replacement that asks for one is refused here
     * rather than splicing a 0x0A into the middle of a line. */
    if (ch == 'n' || ch == 'r') {
      free(out);
      if (err) *err = "a replacement cannot contain a newline";
      return NULL;
    }
    if (ch == 't') {
      rx_out(&out, &len, &cap, "\t", 1);
      t += 2;
      continue;
    }
    /* \\ gives a backslash, and \ in front of anything else gives that
     * character -- so a literal $ or & needs no escaping but may have one. */
    rx_out(&out, &len, &cap, t + 1, 1);
    t += 2;
  }

  out[len] = '\0';
  if (outlen) *outlen = len;
  return out;
}
