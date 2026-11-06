/* snippet.c -- a completion that is a template: `printf(${1:fmt}, $0)`.
 *
 * A language server can offer a completion as a snippet, text with fields in
 * it. Molly puts the text in with the first field selected, so typing replaces
 * it; Tab goes on to the next field and Shift-Tab back, and after the last one
 * Tab goes to where the snippet says the cursor ends ($0), and the fields are
 * done with. Esc is done with them at once, and so is moving out of the field
 * being typed in.
 *
 * Two halves. snippet_expand() reads the syntax into plain text and a list of
 * fields, and knows nothing about buffers, which is what lets the unit tests
 * take every corner of the syntax on directly. The rest is the session: where
 * the fields are while the text around them changes.
 *
 * Where they are is kept as offsets into the snippet's text, which is a run of
 * the buffer from a fixed start to an end counted back from the end of the
 * buffer. Nothing is typed before the start, and nothing after the end, while
 * the fields are in use -- typing goes into the current field, and a cursor
 * that leaves it ends the session -- so both stay right without being told
 * about each edit. After every key the run's new length says how much the
 * current field grew or shrank, which moves the fields after it; and the
 * cursors have to be inside the field, or what happened was not typing in it
 * and the session ends. That check is what makes it safe to know so little:
 * an undo, a paste, a server's edit either keeps it true or ends the session,
 * and never leaves a field pointing at the wrong text. */

#include "molly.h"

/* ---- the syntax ---------------------------------------------------------- */

/* The grammar is the language server protocol's:
 *
 *   $1  ${1}  ${1:placeholder, which may hold $2 and ${3:more}}
 *   ${1|one,two,three|}     a choice: molly takes the first
 *   $name  ${name}  ${name:default}   a variable: its default, or nothing
 *   ${1/regex/format/opts}  a transform: the field, untransformed
 *
 * A backslash makes `$`, `}` and `\` themselves, and in a choice `,` and `|`
 * too. Anything that does not parse is text: a lone `$`, or `${1:` that
 * never closes, goes in as it was written. */

struct expander {
  const char *s;
  int i, n;
  struct abuf out;
  struct snip_stop *stops;
  int nstops, cap;
  const char *indent, *tab;   /* for a newline, and for a tab */
  int parent;                 /* the placeholder being read, or -1 */
  /* Where a `${` has already failed to parse. Without it every unclosed one
   * is read twice, as a field and then as text, and so is everything inside
   * it: `${1:${1:${1:` nested a few dozen deep took longer than anyone would
   * wait. With it, no `${` is tried more than once. */
  unsigned char *failed;
  /* The text each number's placeholder got, for a bare $n that repeats it. */
  struct { int num; char *text; } *seen;
  int nseen;
};

static void emit(struct expander *e, char c) {
  if (c == '\n') {
    ab_append(&e->out, "\n", 1);
    ab_append(&e->out, e->indent, (int)strlen(e->indent));
  } else if (c == '\t') {
    ab_append(&e->out, e->tab, (int)strlen(e->tab));
  } else {
    ab_append(&e->out, &c, 1);
  }
}

static int add_stop(struct expander *e, int num) {
  if (e->nstops == e->cap) {
    e->cap = e->cap ? e->cap * 2 : 8;
    e->stops = xrealloc(e->stops, sizeof(*e->stops) * (size_t)e->cap);
  }
  e->stops[e->nstops].num = num;
  e->stops[e->nstops].off = e->out.len;
  e->stops[e->nstops].len = 0;
  e->stops[e->nstops].parent = e->parent;
  return e->nstops++;
}

static const char *seen_text(const struct expander *e, int num) {
  int i;
  for (i = 0; i < e->nseen; i++)
    if (e->seen[i].num == num) return e->seen[i].text;
  return NULL;
}

static void note_seen(struct expander *e, int num, int off) {
  char *t;
  if (seen_text(e, num) != NULL) return;
  t = xmalloc((size_t)(e->out.len - off) + 1);
  if (e->out.len > off) memcpy(t, e->out.b + off, (size_t)(e->out.len - off));
  t[e->out.len - off] = '\0';
  e->seen = xrealloc(e->seen, sizeof(*e->seen) * (size_t)(e->nseen + 1));
  e->seen[e->nseen].num = num;
  e->seen[e->nseen].text = t;
  e->nseen++;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int var_start(char c) { return c == '_' || isalpha((unsigned char)c); }
static int var_char(char c) { return c == '_' || isalnum((unsigned char)c); }

static int parse_any(struct expander *e, int depth);

/* A number, capped so that a server's 99999999999 is a big number and not an
 * overflow. -1 when there is none. */
static int parse_int(struct expander *e) {
  int v = 0;
  if (e->i >= e->n || !is_digit(e->s[e->i])) return -1;
  while (e->i < e->n && is_digit(e->s[e->i])) {
    if (v < 100000) v = v * 10 + (e->s[e->i] - '0');
    e->i++;
  }
  return v;
}

/* Past a transform's three parts, `/regex/format/options`, to the `}` that
 * closes it. Returns 0 if the input ends first. */
static int skip_transform(struct expander *e) {
  int slashes = 0;
  e->i++;   /* the first / */
  while (e->i < e->n) {
    char c = e->s[e->i];
    if (c == '\\' && e->i + 1 < e->n) {
      e->i += 2;
      continue;
    }
    if (c == '/' && slashes < 2) slashes++;
    else if (c == '}' && slashes == 2) return 1;
    e->i++;
  }
  return 0;
}

/* `${` has been read; what follows up to and including the `}`. Returns 0
 * with nothing consumed that matters if it does not parse: the caller puts
 * the `$` in as text and goes on from the character after it. */
static int parse_braced(struct expander *e, int depth) {
  int num = parse_int(e), at, k;
  if (num >= 0) {
    if (e->i >= e->n) return 0;
    if (e->s[e->i] == '}') {          /* ${1} */
      const char *t = seen_text(e, num);
      at = add_stop(e, num);
      if (t != NULL) ab_append(&e->out, t, (int)strlen(t));
      e->stops[at].len = e->out.len - e->stops[at].off;
      e->i++;
      return 1;
    }
    if (e->s[e->i] == ':') {          /* ${1:placeholder} */
      int outer = e->parent;
      e->i++;
      at = add_stop(e, num);
      e->parent = at;
      k = parse_any(e, depth + 1);
      e->parent = outer;
      if (!k || e->i >= e->n || e->s[e->i] != '}') return 0;
      e->i++;
      e->stops[at].len = e->out.len - e->stops[at].off;
      note_seen(e, num, e->stops[at].off);
      return 1;
    }
    if (e->s[e->i] == '|') {          /* ${1|one,two|} */
      int first = 1;
      e->i++;
      at = add_stop(e, num);
      while (e->i < e->n) {
        char c = e->s[e->i];
        if (c == '|' && e->i + 1 < e->n && e->s[e->i + 1] == '}') {
          e->i += 2;
          e->stops[at].len = e->out.len - e->stops[at].off;
          note_seen(e, num, e->stops[at].off);
          return 1;
        }
        if (c == ',') {
          first = 0;
          e->i++;
          continue;
        }
        if (c == '\\' && e->i + 1 < e->n && strchr("$}\\,|", e->s[e->i + 1])) {
          c = e->s[++e->i];
        }
        if (first) emit(e, c);
        e->i++;
      }
      return 0;
    }
    if (e->s[e->i] == '/') {          /* ${1/regex/format/} */
      if (!skip_transform(e)) return 0;
      e->i++;
      at = add_stop(e, num);
      {
        const char *t = seen_text(e, num);
        if (t != NULL) ab_append(&e->out, t, (int)strlen(t));
      }
      e->stops[at].len = e->out.len - e->stops[at].off;
      return 1;
    }
    return 0;
  }
  if (e->i < e->n && var_start(e->s[e->i])) {
    while (e->i < e->n && var_char(e->s[e->i])) e->i++;
    if (e->i >= e->n) return 0;
    if (e->s[e->i] == '}') {
      e->i++;
      return 1;
    }
    if (e->s[e->i] == ':') {
      e->i++;
      if (!parse_any(e, depth + 1)) return 0;
      if (e->i >= e->n || e->s[e->i] != '}') return 0;
      e->i++;
      return 1;
    }
    if (e->s[e->i] == '/') {
      if (!skip_transform(e)) return 0;
      e->i++;
      return 1;
    }
  }
  return 0;
}

/* Text and fields up to a `}` that closes the placeholder this is inside
 * (depth > 0), or to the end. A placeholder nested past any sane depth is
 * refused, so a hostile snippet cannot run the stack out. */
static int parse_any(struct expander *e, int depth) {
  if (depth > 32) return 0;
  while (e->i < e->n) {
    char c = e->s[e->i];
    if (c == '\\' && e->i + 1 < e->n && strchr("$}\\", e->s[e->i + 1])) {
      emit(e, e->s[e->i + 1]);
      e->i += 2;
      continue;
    }
    if (c == '}' && depth > 0) return 1;
    if (c == '$') {
      int save_i = e->i, save_len = e->out.len, save_stops = e->nstops;
      e->i++;
      if (e->i < e->n && is_digit(e->s[e->i])) {          /* $1 */
        int num = parse_int(e), at = add_stop(e, num);
        const char *t = seen_text(e, num);
        if (t != NULL) ab_append(&e->out, t, (int)strlen(t));
        e->stops[at].len = e->out.len - e->stops[at].off;
        continue;
      }
      if (e->i < e->n && var_start(e->s[e->i])) {         /* $name */
        while (e->i < e->n && var_char(e->s[e->i])) e->i++;
        continue;
      }
      if (e->i < e->n && e->s[e->i] == '{' && !e->failed[save_i]) {
        e->i++;
        if (parse_braced(e, depth)) continue;
        e->failed[save_i] = 1;
      }
      /* Not a field after all: the `$` is text, and so is what follows it,
       * read again from just after the `$`. */
      e->i = save_i + 1;
      e->out.len = save_len;
      e->nstops = save_stops;
      emit(e, '$');
      continue;
    }
    emit(e, c);
    e->i++;
  }
  return depth == 0;
}

/* Read `src` into plain text with its fields. A newline in it is followed by
 * `indent`, the line's own indentation, so a snippet of several lines lines up
 * under the one it was typed on; a tab becomes `tab`, the file's own indent
 * step. Never fails: what is not a field is text. */
void snippet_expand(const char *src, const char *indent, const char *tab,
                    struct snippet *out) {
  struct expander e;
  int i;
  memset(&e, 0, sizeof(e));
  e.s = src;
  e.n = (int)strlen(src);
  e.indent = indent != NULL ? indent : "";
  e.tab = tab != NULL ? tab : "\t";
  e.parent = -1;
  e.failed = xmalloc((size_t)e.n + 1);
  memset(e.failed, 0, (size_t)e.n + 1);
  while (e.i < e.n) {
    parse_any(&e, 0);
    /* A `}` at the top level closes nothing, and is text. */
    if (e.i < e.n) {
      emit(&e, e.s[e.i]);
      e.i++;
    }
  }
  ab_append(&e.out, "", 1);
  out->text = e.out.b;
  out->len = e.out.len - 1;
  out->stops = e.stops;
  out->nstops = e.nstops;
  for (i = 0; i < e.nseen; i++) free(e.seen[i].text);
  free(e.seen);
  free(e.failed);
}

void snippet_free(struct snippet *s) {
  free(s->text);
  free(s->stops);
  memset(s, 0, sizeof(*s));
}

/* ---- the session --------------------------------------------------------- */

#define SNIP_ACTIVE_MAX 16

static struct {
  struct buffer *buf;          /* NULL when there is no session */
  int sy, sx;                  /* where the snippet's text starts */
  int end_rows, end_cols;      /* where it ends, counted from the end */
  int len;                     /* its length, a row break counting one */
  struct snip_stop *stops;
  int nstops;
  int order[64], norder;       /* the field numbers, in the order Tab takes */
  int cur;                     /* index into order[] */
  int active[SNIP_ACTIVE_MAX]; /* the stops the cursors are in */
  int nactive;
  unsigned edits;              /* the buffer's edit count when last looked at */
} S;

int snippet_active(void) { return S.buf != NULL; }

void snippet_end(void) {
  free(S.stops);
  memset(&S, 0, sizeof(S));
}

void snippet_forget(struct buffer *b) {
  if (S.buf == b) snippet_end();
}

/* The position `off` bytes into the snippet's text. */
static void pos_of(int off, int *y, int *x) {
  int cy = S.sy, cx = S.sx;
  while (cy < row_count()) {
    int room = row_at(cy)->size - cx;
    if (off <= room) break;
    off -= room + 1;
    cy++;
    cx = 0;
  }
  *y = cy;
  *x = cx + off;
}

/* How far into the snippet's text (y, x) is; -1 for before it, and a number
 * past its length for after. */
static int off_of(int y, int x) {
  int off = 0, cy = S.sy, cx = S.sx;
  if (y < S.sy || (y == S.sy && x < S.sx)) return -1;
  while (cy < y && cy < row_count()) {
    off += row_at(cy)->size - cx + 1;
    cy++;
    cx = 0;
  }
  return off + (x - cx);
}

/* Where the snippet's text ends now, from the end of the buffer. 0 if that is
 * not a place after its start any more. */
static int region_end(int *y, int *x) {
  *y = row_count() - 1 - S.end_rows;
  if (*y < S.sy || *y >= row_count()) return 0;
  *x = row_at(*y)->size - S.end_cols;
  if (*x < 0 || (*y == S.sy && *x < S.sx)) return 0;
  return 1;
}

/* Put the cursors on field order[i]: each copy of it selected, so typing
 * replaces them all. Copies on more than one row, or more of them than there
 * is room for, get the first only. */
static void select_field(int i) {
  int k, num = S.order[i], one_row = 1, n = 0;
  S.cur = i;
  S.nactive = 0;
  for (k = 0; k < S.nstops; k++) {
    int y0, x0, y1, x1;
    if (S.stops[k].num != num) continue;
    pos_of(S.stops[k].off, &y0, &x0);
    pos_of(S.stops[k].off + S.stops[k].len, &y1, &x1);
    if (y0 != y1) one_row = 0;
    n++;
  }
  cursors_collapse();
  for (k = 0; k < S.nstops; k++) {
    int y0, x0, y1, x1;
    if (S.stops[k].num != num) continue;
    pos_of(S.stops[k].off, &y0, &x0);
    pos_of(S.stops[k].off + S.stops[k].len, &y1, &x1);
    if (S.nactive == 0) {
      CUR.cy = y1;
      CUR.cx = x1;
      CUR.sy = y0;
      CUR.sx = x0;
      CUR.sel_active = S.stops[k].len > 0;
    } else if (!one_row || n > SNIP_ACTIVE_MAX || S.nactive >= SNIP_ACTIVE_MAX) {
      continue;
    } else if (S.stops[k].len > 0 ? !cursors_add_sel(y0, x0, x1) : !cursors_add(y0, x0)) {
      continue;
    }
    S.active[S.nactive++] = k;
  }
  V.primary = 0;
  S.edits = V.buf->edits;
}

/* Tab past the last field: to $0, or the end of the snippet, and done. */
static void finish(void) {
  int k, y, x, off = S.len;
  for (k = 0; k < S.nstops; k++)
    if (S.stops[k].num == 0) {
      off = S.stops[k].off;
      break;
    }
  pos_of(off, &y, &x);
  cursor_place(y, x);
  snippet_end();
}

/* The text of `sn` has just been put in at (y, x) of the current buffer.
 * Takes the snippet over. With no field but $0 there is nothing to Tab
 * through: the cursor goes to $0, and that is all. */
void snippet_begin(struct snippet *sn, int y, int x) {
  int k, ey, ex;
  snippet_end();
  S.buf = V.buf;
  S.sy = y;
  S.sx = x;
  S.len = sn->len;
  S.stops = sn->stops;
  S.nstops = sn->nstops;
  sn->stops = NULL;
  sn->nstops = 0;
  pos_of(S.len, &ey, &ex);
  if (ey >= row_count()) {
    snippet_end();
    return;
  }
  S.end_rows = row_count() - 1 - ey;
  S.end_cols = row_at(ey)->size - ex;

  /* Tab's order: 1, 2, 3 ... whatever numbers there are, smallest first. */
  for (k = 0; k < S.nstops && S.norder < (int)(sizeof(S.order) / sizeof(S.order[0])); k++) {
    int num = S.stops[k].num, j, at;
    if (num == 0) continue;
    for (j = 0; j < S.norder && S.order[j] != num; j++) {}
    if (j < S.norder) continue;
    for (at = S.norder; at > 0 && S.order[at - 1] > num; at--) S.order[at] = S.order[at - 1];
    S.order[at] = num;
    S.norder++;
  }
  if (S.norder == 0) {
    finish();
    return;
  }
  select_field(0);
  editor_set_status_message("Tab goes to the next field, Shift-Tab back, Esc leaves them.");
}

/* Is stop `outer` a placeholder that stop `inner` is somewhere inside? */
static int holds(int outer, int inner) {
  int p = S.stops[inner].parent;
  while (p >= 0) {
    if (p == outer) return 1;
    p = S.stops[p].parent;
  }
  return 0;
}

/* After any key: bring the fields up to date with what it did, or end the
 * session if it did anything but type in the current field. */
void snippet_after_key(void) {
  int ey, ex, nlen, delta, d, k, j, edited;
  int old_off[SNIP_ACTIVE_MAX], old_end[SNIP_ACTIVE_MAX];
  if (S.buf == NULL) return;
  if (V.buf != S.buf || S.nactive == 0 || !region_end(&ey, &ex)) {
    snippet_end();
    return;
  }
  nlen = off_of(ey, ex);
  delta = nlen - S.len;
  edited = delta != 0 || V.buf->edits != S.edits;
  if (delta % S.nactive != 0) {
    snippet_end();
    return;
  }
  d = delta / S.nactive;
  for (j = 0; j < S.nactive; j++) {
    old_off[j] = S.stops[S.active[j]].off;
    old_end[j] = old_off[j] + S.stops[S.active[j]].len;
    if (S.stops[S.active[j]].len + d < 0) {
      snippet_end();
      return;
    }
  }

  /* Every field after a copy of the current one moves by what that copy grew
   * by; a placeholder holding a copy grows with it; a field inside a copy is
   * gone once the copy has been typed over. All of it from where things were
   * before the key. */
  {
    int o = 0, nact = 0, newact[SNIP_ACTIVE_MAX];
    int *map = xmalloc(sizeof(*map) * (size_t)(S.nstops > 0 ? S.nstops : 1));
    for (k = 0; k < S.nstops; k++) {
      struct snip_stop st = S.stops[k];
      int shift = 0, grow = 0, inside = 0, mine = -1;
      for (j = 0; j < S.nactive; j++) {
        int a = S.active[j];
        if (k == a) {
          mine = j;
        } else if (holds(k, a)) {
          grow += d;
        } else if (holds(a, k)) {
          inside = 1;
        } else if (st.off > old_end[j] || (st.off == old_end[j] && k > a)) {
          shift += d;
        }
      }
      map[k] = -1;
      if (inside && edited && mine < 0) continue;
      if (mine >= 0) {
        st.len += d;
        newact[nact++] = o;
      }
      st.off += shift;
      st.len += grow;
      st.parent = st.parent >= 0 ? map[st.parent] : -1;
      map[k] = o;
      S.stops[o++] = st;
    }
    free(map);
    S.nstops = o;
    for (j = 0; j < nact; j++) S.active[j] = newact[j];
    S.nactive = nact;
  }
  S.len = nlen;
  S.edits = V.buf->edits;

  /* The cursors must be in the copies of the current field, one in each. */
  if (V.ncursors != S.nactive) {
    snippet_end();
    return;
  }
  for (k = 0; k < V.ncursors; k++) {
    int off = off_of(V.cursors[k].cy, V.cursors[k].cx), in = 0;
    for (j = 0; j < S.nactive; j++) {
      const struct snip_stop *st = &S.stops[S.active[j]];
      if (off >= st->off && off <= st->off + st->len) in = 1;
    }
    if (!in) {
      snippet_end();
      return;
    }
  }
}

/* Is field `num` still there? One inside a placeholder that has been typed
 * over is not, and Tab goes past it. */
static int field_left(int num) {
  int k;
  for (k = 0; k < S.nstops; k++)
    if (S.stops[k].num == num) return 1;
  return 0;
}

/* Tab and Shift-Tab, while there are fields; Esc leaves them and goes on to
 * do what Esc does. Returns 1 when the key has been used up. */
int snippet_key(int c) {
  if (S.buf == NULL) return 0;
  /* A key used up before the after-key update (a completion taken, say) may
   * have changed the text since the fields were last brought up to date. */
  snippet_after_key();
  if (S.buf == NULL) return 0;
  if (c == '\x1b') {
    snippet_end();
    return 0;
  }
  if (input_last_key_had_ctrl() || input_last_key_had_alt()) return 0;
  if (c == '\t' && !input_last_key_had_shift()) {
    int i = S.cur + 1;
    while (i < S.norder && !field_left(S.order[i])) i++;
    if (i < S.norder) select_field(i);
    else finish();
    return 1;
  }
  if (c == BACKTAB_KEY || (c == '\t' && input_last_key_had_shift())) {
    int i = S.cur - 1;
    while (i >= 0 && !field_left(S.order[i])) i--;
    if (i >= 0) select_field(i);
    return 1;
  }
  return 0;
}
