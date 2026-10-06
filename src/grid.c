/* grid.c -- the screen as a grid of cells, diffed once per frame.
 *
 * Olly cached one *byte string* per screen line and re-emitted the lines whose
 * bytes changed. That is a good fit for one full-width text area and a poor one
 * for everything molly is going to grow. A drop-down menu or a dialog paints over
 * the text and then has to put back what was underneath; with a line of bytes
 * there is nothing to put back, because the line was never addressable by column.
 * Splits need two independent regions of the same line. Syntax highlighting needs
 * somewhere to record that a given character is a keyword, and a byte string can
 * only record it by carrying the escape sequence inline, where it is impossible to
 * tell a style from the text.
 *
 * So the screen is a grid of cells, every element draws into it, and one diff per
 * frame turns the difference into escape sequences. This is how both Turbo Vision
 * and ncurses work, and it is what makes menus, dialogs and splits ordinary rather
 * than special.
 *
 * Two planes: `cur`, what this frame should look like, and `prev`, what the
 * terminal is believed to be showing. Everything draws into cur; flush compares
 * the two, emits the difference, and swaps.
 *
 * The emitted sequences are deliberately the same ones olly used -- reverse video
 * on with ESC[7m and off with ESC[27m, gray with ESC[90m and off with ESC[m --
 * because they are the minimal form and tests assert on them.
 *
 * Not yet on the grid: the help screen and the open-file dialog, which still write
 * escape sequences straight to the terminal and then ask for a full repaint. The
 * widget toolkit in Phase 1 brings them in; until then the full repaint is what
 * keeps the grid honest about a screen it did not draw. */

#include "molly.h"

/* Bytes stored per cell: one character plus a few combining marks. A cluster
 * longer than this keeps its base character and leading marks and drops the rest,
 * which is visually indistinguishable in every case this editor can produce and
 * avoids a variable-length allocation per cell. */
#define CELL_BYTES 12

struct cell {
  char ch[CELL_BYTES];
  unsigned char len;    /* bytes used; 0 means a blank */
  unsigned char width;  /* columns: 1, 2, or 0 for the tail of a wide glyph */
  unsigned char attr;   /* ATTR_* */
};

static struct cell *cur;
static struct cell *prev;
static int g_rows, g_cols;
static int g_alloc;        /* cells allocated per plane */
static int prev_valid;     /* 0 = the terminal's contents are unknown */
static unsigned char pen;  /* attributes new cells are written with */
static int want_cy, want_cx;

static struct cell *at(struct cell *plane, int y, int x) {
  return &plane[(size_t)y * (size_t)g_cols + (size_t)x];
}

static int cell_eq(const struct cell *a, const struct cell *b) {
  if (a->attr != b->attr || a->width != b->width || a->len != b->len) return 0;
  return a->len == 0 || memcmp(a->ch, b->ch, a->len) == 0;
}

/* A cell that would draw as an unstyled space. */
static int cell_blank(const struct cell *c) {
  return c->len == 0 && c->attr == 0 && c->width == 1;
}

/* Match the grid to the terminal size. The contents are dropped and the next
 * flush repaints in full, which is what we want after a resize anyway. */
void grid_resize(int rows, int cols) {
  if (rows < 1) rows = 1;
  if (cols < 1) cols = 1;
  if (rows == g_rows && cols == g_cols) return;
  int need = rows * cols;
  if (need > g_alloc) {
    cur = xrealloc(cur, sizeof(struct cell) * (size_t)need);
    prev = xrealloc(prev, sizeof(struct cell) * (size_t)need);
    g_alloc = need;
  }
  g_rows = rows;
  g_cols = cols;
  prev_valid = 0;
}

/* Forget what the terminal is showing, so the next flush emits every cell.
 * Needed after something else has drawn on the screen. */
void grid_invalidate(void) {
  prev_valid = 0;
}

/* Start a frame: every cell an unstyled blank, the pen back to default. */
void grid_begin(void) {
  int i, n = g_rows * g_cols;
  for (i = 0; i < n; i++) {
    cur[i].len = 0;
    cur[i].width = 1;
    cur[i].attr = 0;
  }
  pen = 0;
  want_cy = 0;
  want_cx = 0;
}

void grid_pen(unsigned char attr) {
  pen = attr;
}

void grid_cursor_to(int y, int x) {
  want_cy = y;
  want_cx = x;
}

int grid_rows(void) { return g_rows; }
int grid_cols(void) { return g_cols; }

/* Place one character. `w` is its display width: 0 for a combining mark, which
 * attaches to the cell on the left rather than occupying one of its own.
 * Returns the columns consumed. A glyph that will not fit is dropped. */
int grid_put_char(int y, int x, const char *s, int nbytes, int w) {
  if (y < 0 || y >= g_rows || x < 0 || x >= g_cols) return 0;
  if (nbytes <= 0) return 0;

  if (w == 0) {
    /* Attach to the character to the left, stepping over the tail of a wide
     * glyph. With nothing to the left there is nothing to combine with. */
    int bx = x - 1;
    while (bx >= 0 && at(cur, y, bx)->width == 0) bx--;
    if (bx < 0) return 0;
    struct cell *c = at(cur, y, bx);
    if (c->len + nbytes <= CELL_BYTES) {
      memcpy(c->ch + c->len, s, (size_t)nbytes);
      c->len = (unsigned char)(c->len + nbytes);
    }
    return 0;
  }

  if (w == 2 && x + 1 >= g_cols) {
    /* A wide glyph with one column left: show a blank so the columns after it
     * stay where they belong, exactly as the byte renderer did. */
    struct cell *c = at(cur, y, x);
    c->len = 0;
    c->width = 1;
    c->attr = pen;
    return 1;
  }

  struct cell *c = at(cur, y, x);
  int n = nbytes > CELL_BYTES ? CELL_BYTES : nbytes;
  memcpy(c->ch, s, (size_t)n);
  c->len = (unsigned char)n;
  c->width = (unsigned char)w;
  c->attr = pen;
  if (w == 2) {
    struct cell *t = at(cur, y, x + 1);
    t->len = 0;
    t->width = 0;
    t->attr = pen;
  }
  return w;
}

/* Place a UTF-8 run, decoding as it goes. Returns the columns consumed. */
int grid_puts(int y, int x, const char *s, int len) {
  int i = 0, used = 0;
  while (i < len) {
    int nb, w = utf8_char_cols(s, len, i, &nb);
    if (x + used >= g_cols) break;
    used += grid_put_char(y, x + used, s + i, nb, w);
    i += nb;
  }
  return used;
}

/* Fill n columns with one ASCII character in the current pen. */
void grid_fill(int y, int x, int n, char c) {
  int i;
  for (i = 0; i < n; i++) grid_put_char(y, x + i, &c, 1, 1);
}

/* --- emitting the difference ---------------------------------------------- */

/* Move the terminal's attributes from `have` to `want` in as few bytes as the
 * old byte-building code used, so output stays minimal and recognizable:
 * reverse video has a dedicated off code, gray does not and needs a full reset,
 * after which anything still wanted is re-applied. */
static unsigned char emit_attr(struct abuf *ab, unsigned char have,
                               unsigned char want) {
  if (have == want) return have;
  if ((have & ATTR_GRAY) && !(want & ATTR_GRAY)) {
    ab_append(ab, "\x1b[m", 3);
    have = 0;
  }
  if ((want & ATTR_GRAY) && !(have & ATTR_GRAY)) {
    ab_append(ab, "\x1b[90m", 5);
    have |= ATTR_GRAY;
  }
  if ((want & ATTR_REVERSE) && !(have & ATTR_REVERSE)) {
    ab_append(ab, "\x1b[7m", 4);
    have |= ATTR_REVERSE;
  } else if (!(want & ATTR_REVERSE) && (have & ATTR_REVERSE)) {
    ab_append(ab, "\x1b[27m", 5);
    have &= (unsigned char)~ATTR_REVERSE;
  }
  return have;
}

/* Emit one row, if anything in it changed.
 *
 * A changed row is redrawn from column zero rather than from the first differing
 * column. Emitting only the changed span would be fewer bytes, but it would also
 * mean a row's text never appears contiguously in the output -- type one character
 * into a selection and the stream carries that character alone. Olly redrew whole
 * lines, every test that reads text out of the output stream depends on it, and at
 * 80 columns the saving is tens of bytes a frame. Row granularity it is; the point
 * of the grid is addressable cells and attributes, not a tighter diff.
 *
 * Attribute changes inside the row emit the minimal transition. At the end, a row
 * with a tail to clear turns attributes off and clears it -- EL fills with whatever
 * is currently set, so it has to be the default first -- and a row that reaches the
 * last column resets instead, to leave the terminal clean. Both are what olly
 * emitted, which is why ESC[27m still appears only where text is highlighted. */
static void flush_row(struct abuf *ab, int y) {
  int x, last = -1, changed = 0;

  for (x = 0; x < g_cols; x++) {
    if (!prev_valid || !cell_eq(at(cur, y, x), at(prev, y, x))) changed = 1;
    if (!cell_blank(at(cur, y, x))) last = x;
  }
  if (!changed) return;

  char pos[32];
  int plen = snprintf(pos, sizeof(pos), "\x1b[%d;1H", y + 1);
  ab_append(ab, pos, plen);

  /* Every row leaves the terminal with default attributes, so a row may assume
   * them on entry; nothing needs to be reset here. */
  unsigned char have = 0;
  for (x = 0; x <= last; x++) {
    struct cell *c = at(cur, y, x);
    if (c->width == 0) continue;  /* tail of a wide glyph, emitted with its head */
    have = emit_attr(ab, have, c->attr);
    if (c->len == 0) ab_append(ab, " ", 1);
    else ab_append(ab, c->ch, c->len);
  }

  if (last + 1 < g_cols) {
    if (have != 0) have = emit_attr(ab, have, 0);
    ab_append(ab, "\x1b[K", 3);
  } else if (have != 0) {
    ab_append(ab, "\x1b[m", 3);
  }
}

/* Write the frame out: hide the cursor, emit each row's changes, put the cursor
 * where the editor asked for it, show it again. One write per frame. */
void grid_flush(void) {
  struct abuf ab = ABUF_INIT;
  int y;

  ab_append(&ab, "\x1b[?25l", 6);
  if (!prev_valid) {
    ab_append(&ab, "\x1b[2J", 4);
    ab_append(&ab, "\x1b[H", 3);
  }
  for (y = 0; y < g_rows; y++) flush_row(&ab, y);

  char pos[32];
  int plen = snprintf(pos, sizeof(pos), "\x1b[%d;%dH", want_cy + 1, want_cx + 1);
  ab_append(&ab, pos, plen);
  ab_append(&ab, "\x1b[?25h", 6);

  write_all(STDOUT_FILENO, ab.b, (size_t)ab.len);
  free(ab.b);

  memcpy(prev, cur, sizeof(struct cell) * (size_t)(g_rows * g_cols));
  prev_valid = 1;
}

void grid_cleanup(void) {
  free(cur);
  free(prev);
  cur = NULL;
  prev = NULL;
  g_alloc = 0;
  g_rows = 0;
  g_cols = 0;
  prev_valid = 0;
}
