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
  unsigned char role;   /* enum cell_role; the theme turns it into a style */
  unsigned char attrs;  /* ATTR_BOLD, ATTR_UNDERLINE: modifiers on top of role */
  int fg, bg;           /* with ATTR_RAW: colors, -1 for default; with
                         * ATTR_TINT, bg is the tint's role */
};

static struct cell *cur;
static struct cell *prev;
static int g_rows, g_cols;
static int g_alloc;        /* cells allocated per plane */
static int prev_valid;     /* 0 = the terminal's contents are unknown */
static int cursor_shown;         /* a dialog hides it; see grid_cursor_hide */
static int cursor_claimed;       /* an input field took it; see grid_cursor_claim */
static unsigned char pen;        /* the role new cells are written with */
static unsigned char pen_attrs;  /* and the modifiers they carry */
static int pen_fg = -1, pen_bg = -1;    /* with ATTR_RAW in pen_attrs */
static int pen_tint = -1;               /* a role whose background to lay under */
static int want_cy, want_cx;

static struct cell *at(struct cell *plane, int y, int x) {
  return &plane[(size_t)y * (size_t)g_cols + (size_t)x];
}

static int cell_eq(const struct cell *a, const struct cell *b) {
  if (a->role != b->role || a->attrs != b->attrs ||
      a->width != b->width || a->len != b->len) return 0;
  if ((a->attrs & ATTR_RAW) && (a->fg != b->fg || a->bg != b->bg)) return 0;
  if ((a->attrs & ATTR_TINT) && a->bg != b->bg) return 0;
  return a->len == 0 || memcmp(a->ch, b->ch, a->len) == 0;
}

/* A cell that would draw as an unstyled space, and so can be produced by
 * erasing rather than by writing. */
static int cell_blank(const struct cell *c) {
  if (c->len != 0 || c->width != 1) return 0;
  if (c->attrs == ATTR_RAW) return c->fg < 0 && c->bg < 0;
  return c->role == ROLE_TEXT && c->attrs == 0;
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
    cur[i].role = ROLE_TEXT;
    cur[i].attrs = 0;
    cur[i].fg = cur[i].bg = -1;
  }
  pen = ROLE_TEXT;
  pen_attrs = 0;
  pen_fg = pen_bg = -1;
  want_cy = 0;
  want_cx = 0;
  cursor_shown = 1;
  cursor_claimed = 0;
}

/* The role that subsequent writes are tagged with. Setting a role clears the
 * modifiers, because a modifier belongs to a run of text -- a mnemonic, a bold
 * title -- rather than to whatever is drawn next. */
void grid_role(unsigned char role) {
  pen = role;
  pen_attrs = 0;
  pen_fg = pen_bg = -1;
  pen_tint = -1;
}

/* Lay a role's background under what is written next, keeping the cells'
 * own role for everything else: the current line, the word under the cursor
 * and the ruler are tints over syntax colors, not replacements for them.
 * -1 for none. Cleared by grid_role(). */
void grid_tint(int role) { pen_tint = role; }

/* Write in colors the caller chose: palette indices 0-255, or -1 for the
 * terminal's default, and any of the ATTR_ modifiers. For the integrated
 * terminal and nothing else -- everything molly draws for itself names a role.
 * A color the terminal cannot show is brought down to one it can as the frame
 * is written, so the cell keeps what was asked for. */
void grid_color(int fg, int bg, unsigned char attrs) {
  pen = ROLE_TEXT;
  pen_attrs = (unsigned char)(attrs | ATTR_RAW);
  pen_fg = fg;
  pen_bg = bg;
}

/* Modifiers for subsequent writes, on top of the current role. */
void grid_attrs(unsigned char attrs) {
  pen_attrs = attrs;
}

void grid_cursor_to(int y, int x) {
  /* Clamped into the grid: with the view scrolled away from the cursor, the cursor's
   * row can be off-screen, and a position outside the terminal is a sequence whose
   * handling is the terminal's business rather than something to rely on. */
  if (y < 0) y = 0;
  if (y >= g_rows) y = g_rows - 1;
  if (x < 0) x = 0;
  if (x >= g_cols) x = g_cols - 1;
  want_cy = y;
  want_cx = x;
}

/* Leave the cursor hidden for this frame. A dialog with nothing to type into has
 * no business parking a blinking cursor in the text underneath it. Reset by
 * grid_begin, so it lasts one frame and has to be asked for again. */
void grid_cursor_hide(void) {
  cursor_shown = 0;
}

/* Put the cursor here, and say so.
 *
 * The refresh positions the text cursor after every layer has drawn, which would
 * otherwise overwrite anything a layer asked for; a claim says "this one is mine",
 * and the refresh leaves it alone. It is what puts the cursor in the prompt while
 * typing into it rather than back in the buffer. Claiming also unhides, because a
 * dialog hides the cursor on the way past and an input field inside one still wants
 * it. Cleared by grid_begin, so a claim lasts exactly one frame. */
void grid_cursor_claim(int y, int x) {
  grid_cursor_to(y, x);
  cursor_shown = 1;
  cursor_claimed = 1;
}

int grid_cursor_claimed(void) {
  return cursor_claimed;
}

int grid_rows(void) { return g_rows; }
int grid_cols(void) { return g_cols; }

/* Place one character. `w` is its display width: 0 for a combining mark, which
 * attaches to the cell on the left rather than occupying one of its own.
 * Returns the columns consumed. A glyph that will not fit is dropped. */
int grid_put_char(int y, int x, const char *s, int nbytes, int w) {
  if (y < 0 || y >= g_rows || x < 0 || x >= g_cols) return 0;
  if (nbytes <= 0) return 0;

  /* The one place everything drawn passes through, so the one place a file's
   * contents or a file's name are stopped from being a command to the terminal.
   * A caller that wants the real bytes marked for the reader, as the text area
   * does, sets ATTR_REVERSE; the buffer keeps them either way. */
  if (!utf8_safe(s, nbytes)) {
    s = "?";
    nbytes = 1;
    w = 1;
  }

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
    c->role = pen;
    c->attrs = pen_attrs;
    c->fg = pen_fg;
    c->bg = pen_bg;
    if (pen_tint >= 0 && !(pen_attrs & ATTR_RAW)) {
      c->attrs |= ATTR_TINT;
      c->bg = pen_tint;
    }
    return 1;
  }

  struct cell *c = at(cur, y, x);
  int n = nbytes > CELL_BYTES ? CELL_BYTES : nbytes;
  memcpy(c->ch, s, (size_t)n);
  c->len = (unsigned char)n;
  c->width = (unsigned char)w;
  c->role = pen;
  c->attrs = pen_attrs;
  c->fg = pen_fg;
  c->bg = pen_bg;
  if (pen_tint >= 0 && !(pen_attrs & ATTR_RAW)) {
    c->attrs |= ATTR_TINT;
    c->bg = pen_tint;
  }
  if (w == 2) {
    struct cell *t = at(cur, y, x + 1);
    t->len = 0;
    t->width = 0;
    t->role = pen;
    t->attrs = pen_attrs;
    t->fg = pen_fg;
    t->bg = c->bg;
    t->attrs = c->attrs;
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

/* Restyle a rectangle without touching what it says.
 *
 * This is how a drop shadow works, and why it costs almost nothing: the dialog
 * draws after the text, so the characters it is about to overshadow are already in
 * the grid. Changing their role to a dim one leaves the text legible underneath,
 * which is exactly what a Turbo Vision shadow looked like -- and considerably
 * better than painting a blank rectangle over it. */
void grid_shade(int x, int y, int w, int h, unsigned char role) {
  int yy, xx;
  for (yy = y; yy < y + h; yy++) {
    if (yy < 0 || yy >= g_rows) continue;
    for (xx = x; xx < x + w; xx++) {
      if (xx < 0 || xx >= g_cols) continue;
      at(cur, yy, xx)->role = role;
      /* A terminal's cell keeps its characters and loses its own colors,
       * or the shadow would not show on it. */
      at(cur, yy, xx)->attrs &= (unsigned char)~(ATTR_RAW | ATTR_REVERSE | ATTR_ITALIC |
                                                 ATTR_TINT);
    }
  }
}

/* --- emitting the difference ---------------------------------------------- */

/* Move the terminal from the style it is in to the one the next cell wants, in as
 * few bytes as will do it.
 *
 * Reverse video has a dedicated off code. A color does not -- "no color" means the
 * terminal's own default, and the portable way back to it is a full reset, after
 * which anything still wanted is re-applied. That is what olly did for the gray
 * gutter, and keeping it means the bytes for everything molly already drew are
 * unchanged: reverse on and off are still ESC[7m and ESC[27m, and the gutter is
 * still ESC[90m followed by ESC[m. */
static void emit_style(struct abuf *ab, struct style *have,
                       const struct style *want) {
  if (style_eq(have, want)) return;

  /* Returning to a default color needs the reset first; there is no "unset the
   * foreground" that is safe everywhere. */
  if ((have->fg != want->fg && want->fg < 0) ||
      (have->bg != want->bg && want->bg < 0)) {
    ab_append(ab, "\x1b[m", 3);
    style_default(have);
  }
  if (have->fg != want->fg) {
    style_emit_color(ab, want->fg, 0);
    have->fg = want->fg;
  }
  if (have->bg != want->bg) {
    style_emit_color(ab, want->bg, 1);
    have->bg = want->bg;
  }
  if (want->reverse && !have->reverse) {
    ab_append(ab, "\x1b[7m", 4);
    have->reverse = 1;
  } else if (!want->reverse && have->reverse) {
    ab_append(ab, "\x1b[27m", 5);
    have->reverse = 0;
  }
  if (want->bold != have->bold) {
    /* 22 rather than 21: 21 is double-underline on a good many terminals. */
    ab_append(ab, want->bold ? "\x1b[1m" : "\x1b[22m", want->bold ? 4 : 5);
    have->bold = want->bold;
  }
  if (want->underline != have->underline) {
    ab_append(ab, want->underline ? "\x1b[4m" : "\x1b[24m",
              want->underline ? 4 : 5);
    have->underline = want->underline;
  }
  if (want->italic != have->italic) {
    ab_append(ab, want->italic ? "\x1b[3m" : "\x1b[23m", want->italic ? 4 : 5);
    have->italic = want->italic;
  }
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

  /* Every row leaves the terminal in the default style, so a row may assume it on
   * entry; nothing needs to be reset here. */
  struct style have, want;
  style_default(&have);
  for (x = 0; x <= last; x++) {
    struct cell *c = at(cur, y, x);
    if (c->width == 0) continue;  /* tail of a wide glyph, emitted with its head */
    if (c->attrs & ATTR_RAW) {
      style_default(&want);
      want.fg = theme_fit_color(c->fg);
      want.bg = theme_fit_color(c->bg);
      want.reverse = (c->attrs & ATTR_REVERSE) != 0;
      want.italic = (c->attrs & ATTR_ITALIC) != 0;
    } else {
      theme_style(c->role, &want);
      if (c->attrs & ATTR_TINT) {
        struct style t;
        theme_style((unsigned char)c->bg, &t);
        if (t.bg >= 0) want.bg = t.bg;
      }
      /* The opposite of what is around it, so it shows inside a selection too. */
      if (c->attrs & ATTR_REVERSE) want.reverse = !want.reverse;
    }
    if (c->attrs & ATTR_BOLD) want.bold = 1;
    if (c->attrs & ATTR_UNDERLINE) want.underline = 1;
    emit_style(ab, &have, &want);
    if (c->len == 0) ab_append(ab, " ", 1);
    else ab_append(ab, c->ch, c->len);
  }

  if (last + 1 < g_cols) {
    if (!style_is_default(&have)) {
      style_default(&want);
      emit_style(ab, &have, &want);
    }
    ab_append(ab, "\x1b[K", 3);
  } else if (!style_is_default(&have)) {
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
  if (cursor_shown) ab_append(&ab, "\x1b[?25h", 6);

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
