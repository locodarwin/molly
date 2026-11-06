/* wrap.c -- soft wrap: the map between a screen line and a slice of a file row.
 *
 * A display row is one line of the text area. With wrap off there is exactly one
 * per file row and every function here is the identity mapping -- which is how
 * the rest of the editor came to be written against display rows long before
 * this file existed. The scroll offset, the draw loop, vertical movement and the
 * click-to-position arithmetic all ask these questions already and none of them
 * knows what the answer depends on. See struct drow in molly.h.
 *
 * With wrap on a file row becomes one or more slices, each a range of *display*
 * columns -- not byte offsets, because wrapping happens where the glyphs land on
 * the screen and not where the bytes fall. The slices of a row partition its
 * columns with nothing left out and nothing counted twice, which is what lets a
 * screen column and a cursor column stay the same question asked in two
 * directions.
 *
 * What it costs. Every answer here but one is a walk over file rows: the display
 * row a cursor is on sums the rows above it, and the row at a display index
 * counts up to it. A row's own slice count is cached on the row and recomputed
 * only when editor_update_row() sees the row change, or the window changes width,
 * so a walk is one addition per file row rather than a re-measure of the file.
 * A frame is therefore O(rows) with wrap on where it was O(1) with wrap off;
 * ROADMAP.md section 7 has what that measures at.
 *
 * The exception is wrap_step(), which is vertical movement and is local to the
 * row the cursor is on and the one it is going to. It is here rather than in
 * view.c for exactly that reason: an arrow key must not get slower because the
 * file is long, and the way to be sure of that is to keep it next to the walker
 * it would otherwise have been written in terms of.
 */

#include "molly.h"

/* The text columns a row is laid out in. Never less than one: a window narrow
 * enough to leave no text columns at all would otherwise ask for slices of
 * width zero, which no number of them would reach the end of a row. */
static int wrap_cols(void) {
  int cols = TEXT_COLS();
  return (cols < 1) ? 1 : cols;
}

/* Walk one row's wrap points.
 *
 * Returns the number of display rows the row occupies, always at least one. Then
 * `want` selects a slice by index and `rx` selects the slice holding that display
 * column (pass -1 for the one not being asked); when either names a slice the
 * walk stops there and fills *out, whose `rcols` is DROW_ALL for the last slice
 * of a row -- the one that runs to the end of the text.
 *
 * The break rule is a word wrap: a line breaks after the last space that fits,
 * and a run with no space in it breaks at the edge instead, because the
 * alternative is a column of nothing beside an unbroken word. A tab is a break
 * opportunity at each of its columns, which is what it was when rows kept a
 * tab-expanded copy of themselves and a tab was a run of spaces.
 *
 * The space a line breaks after stays on the line it ends, and a second space
 * after it opens the next one. Trimming it would read better and cost the thing
 * this file is built on: the slices would no longer cover every column of the
 * row, so a cursor in the trimmed run would have no screen cell to sit in.
 */
static int wrap_walk(erow *row, int cols, int want, int rx, struct drow *out) {
  const char *r = row->chars;
  int rs = row->size;
  int tabw = tab_width();
  int i = 0;        /* byte index into the row */
  int col = 0;      /* the display column byte i sits at */
  int start = 0;    /* first display column of the slice being measured */
  int bi = -1;      /* byte index just past the last space seen in this slice */
  int bcol = 0;     /* and its display column */
  int n = 0;        /* slices finished */

  while (i < rs) {
    int nb, w;
    /* An ASCII byte other than a tab is one column, printable or not (a
     * control character draws as a one-column '?'), so the decoding and the
     * width tables are only for the rest. Most text is all this case. */
    if ((unsigned char)r[i] < 0x80 && r[i] != '\t') {
      w = 1;
      nb = 1;
    } else {
      w = text_cell(r, rs, i, col, tabw, &nb);
    }
    /* `col > start` is what keeps a single glyph wider than the whole window
     * from stopping the walk: at the start of a slice the glyph is taken
     * whatever its width, and the draw loop drops what does not fit. */
    if (col > start && col + w > start + cols) {
      int brk = (bi >= 0) ? bcol : col;
      if (n == want || (rx >= 0 && rx < brk)) {
        out->rcol0 = start;
        out->rcols = brk - start;
        out->frag = n;
        return n + 1;
      }
      n++;
      if (bi >= 0) { i = bi; col = bcol; }   /* back up to after the space */
      start = brk;
      bi = -1;
      continue;
    }
    if (r[i] == ' ' || r[i] == '\t') { bi = i + nb; bcol = col + w; }
    col += w;
    i += nb;
  }

  /* The tail, which also answers for a row with nothing in it. It claims every
   * column past the end of the text, which is where a cursor resting after the
   * last character sits. */
  if (n == want || rx >= 0) {
    out->rcol0 = start;
    out->rcols = DROW_ALL;
    out->frag = n;
  }
  return n + 1;
}

/* Whether a row fits on one display row without being laid out: true only
 * when it certainly does. No character is wider than its bytes -- one column
 * for every ASCII byte, a stray byte or a control character, at most one for
 * the two-byte characters and at most two for the longer ones -- except a tab,
 * which can be as wide as a tab stop. So bytes plus what the tabs could add
 * is the most the row can be, and when that fits so does the row: one slice,
 * which is what wrap_walk() would have said after decoding every character.
 * Measuring a file of ten million short lines was that walk ten million
 * times, 2.6 seconds whenever wrap was turned on or the window changed width;
 * this is a count of the tabs in each. */
static int fits(const erow *row, int cols) {
  const char *p = row->chars, *end = row->chars + row->size;
  int extra = 0, per = tab_width() - 1;
  if (row->size > cols) return 0;
  while ((p = memchr(p, '\t', (size_t)(end - p))) != NULL) {
    extra += per;
    if (row->size + extra > cols) return 0;
    p++;
  }
  return 1;
}

/* wrap_walk()'s count for a row of plain ASCII with no tab in it, where
 * every byte is a column: a slice ends after the last space within its
 * width, or at the width when there is none, and the next starts there. One
 * search back per slice rather than a step per byte. -1 when the row is not
 * that kind, for the full walk. */
static int plain_rows(const erow *row, int cols) {
  const unsigned char *p = (const unsigned char *)row->chars;
  int i, start = 0, n = 1;
  for (i = 0; i < row->size; i++)
    if (p[i] >= 0x80 || p[i] == '\t') return -1;
  while (row->size - start > cols) {
    const unsigned char *sp = memrchr(p + start, ' ', (size_t)cols);
    start = sp != NULL ? (int)(sp - p) + 1 : start + cols;
    n++;
  }
  return n;
}

/* How many display rows a row occupies at this width.
 *
 * Cached on the row, keyed by the width it was measured at, so a resize
 * invalidates every row by disagreeing with all of them. editor_update_row()
 * clears it, which is where every change to a row's text passes, and
 * indent_sync() clears every row's when the tab width moves -- the only two
 * ways the answer can change without the window's width changing. */
static int row_rows(erow *row, int cols) {
  if (row->wcols != cols) {
    int n = fits(row, cols) ? 1 : plain_rows(row, cols);
    row->wrows = n > 0 ? n : wrap_walk(row, cols, -1, -1, NULL);
    row->wcols = cols;
  }
  return row->wrows;
}

/* Display rows file row `i` occupies: none when a fold hides it, one when
 * nothing wraps, and its slices when something does. */
static int disp_rows(int i, int cols) {
  if (B.nfolds > 0 && fold_hidden(V.buf, i)) return 0;
  return V.wrap ? row_rows(row_at(i), cols) : 1;
}

/* The map is the identity only with wrap off and nothing folded. */
static int identity(void) { return !V.wrap && B.nfolds == 0; }

void wrap_row_invalidate(erow *row) {
  row->wcols = -1;
  row->wrows = 1;
  if (V.buf != NULL) B.wrapgen++;
}

/* Where the last drow_at() got to: the file row it answered with, and the
 * display index that row starts at.
 *
 * Without this the draw loop costs a walk of the file per screen line, because
 * it asks for drowoff, drowoff+1, ... in turn and each answer counts up from
 * row zero. With it a frame is one walk: the first ask walks, the rest step on
 * from here. A wrapped frame is then O(rows in the buffer) where an unwrapped
 * one is O(1) -- see ROADMAP.md section 7 for what that measures at, and why
 * the next step belongs with the piece table rather than here.
 *
 * Five things make it stale and all five are in the key: another buffer, a
 * different width, wrap turned on or off, a row count that has moved, and the
 * generation the buffer bumps whenever any row's measurement is dropped. Wrap
 * matters even off, because folded rows keep the map from being the identity:
 * a frame after Alt-Z, or a window beside one wrapped on the same buffer, would
 * otherwise go on from where the other setting left the walk. The last two
 * overlap on purpose -- deleting a row changes the count without re-measuring
 * anything, and an edit re-measures without changing the count. */
static struct {
  const struct buffer *buf;
  unsigned gen;
  int cols;
  int wrap;     /* the view's, which a fold can make matter with it off */
  int numrows;
  int index;    /* display index this row begins at */
  int filerow;
} memo;

/* Display rows the buffer occupies. */
int drow_total(void) {
  int i, cols, n = 0;
  if (identity()) return row_count();
  cols = wrap_cols();
  for (i = 0; i < row_count(); i++) n += disp_rows(i, cols);
  return n;
}

/* The display row holding a cursor. */
int drow_of_cursor(const struct cursor *c) {
  int i, cols, n = 0;
  struct drow d;

  if (identity()) return c->cy;
  cols = wrap_cols();
  for (i = 0; i < c->cy && i < row_count(); i++) n += disp_rows(i, cols);
  /* Past the last row the cursor is on a phantom row, one display row each. */
  if (c->cy >= row_count()) return n + (c->cy - row_count());
  if (!V.wrap) return n;
  wrap_walk(row_at(c->cy), cols, -1,
            editor_row_cx_to_rx(row_at(c->cy), c->cx), &d);
  return n + d.frag;
}

/* The column of its display row that a cursor sits at, which is the display
 * column less wherever that row's slice began. Unwrapped that is the display
 * column itself, and the caller still subtracts the horizontal scroll -- the two
 * are alternatives, so only one of them is ever nonzero. */
int drow_cursor_col(const struct cursor *c) {
  struct drow d;
  int rx;

  if (c->cy >= row_count()) return 0;
  rx = editor_row_cx_to_rx(row_at(c->cy), c->cx);
  if (!V.wrap) return rx;
  wrap_walk(row_at(c->cy), wrap_cols(), -1, rx, &d);
  return rx - d.rcol0;
}

/* Step a cursor one display row up (dir < 0) or down (dir > 0), wrapped.
 *
 * Local to the row and its neighbor, and deliberately so: everything else in
 * this file walks the buffer, and an arrow key must not get slower because the
 * file is long. Which slice the cursor is on comes from its own row, and the
 * slice it is going to comes from that row or the next one -- no display index
 * is ever computed.
 *
 * The screen column is what is kept, not the byte offset; view.c's drow_step
 * says why, and what the unwrapped half does instead. Into a slice narrower
 * than the column asked for, the cursor stops on that slice's last column: a
 * position past the end of a slice is the first position of the next one, so
 * landing there would make one step look like two. The end of a *row* is past
 * the last column of its final slice, which is where a cursor may rest.
 */
void wrap_step(struct cursor *c, int dir) {
  int cols = wrap_cols();
  struct drow here, there;
  erow *row;
  int nrows, frag, vcol, rx, to;

  /* The phantom row past the last line holds one display row and no columns, so
   * there is nothing below it and the row above it is the last slice there is. */
  if (c->cy >= row_count()) {
    if (dir >= 0 || row_count() == 0) return;
    row = row_at(row_count() - 1);
    wrap_walk(row, cols, row_rows(row, cols) - 1, -1, &there);
    c->cy = row_count() - 1;
    c->cx = editor_row_rx_to_cx(row, there.rcol0);
    return;
  }

  row = row_at(c->cy);
  nrows = row_rows(row, cols);
  rx = editor_row_cx_to_rx(row, c->cx);
  wrap_walk(row, cols, -1, rx, &here);
  frag = here.frag;
  vcol = rx - here.rcol0;

  to = c->cy;
  if (dir < 0) {
    if (frag > 0) {
      wrap_walk(row, cols, frag - 1, -1, &there);
    } else if (c->cy > 0) {
      to = c->cy - 1;
      wrap_walk(row_at(to), cols, row_rows(row_at(to), cols) - 1, -1, &there);
    } else {
      return;
    }
  } else {
    if (frag + 1 < nrows) {
      wrap_walk(row, cols, frag + 1, -1, &there);
    } else if (c->cy + 1 < row_count()) {
      to = c->cy + 1;
      wrap_walk(row_at(to), cols, 0, -1, &there);
    } else {
      c->cy = row_count();   /* the phantom row */
      c->cx = 0;
      return;
    }
  }

  rx = there.rcol0 + vcol;
  if (there.rcols != DROW_ALL && rx > there.rcol0 + there.rcols - 1)
    rx = there.rcol0 + there.rcols - 1;
  c->cy = to;
  c->cx = editor_row_rx_to_cx(row_at(to), rx);
}

/* Resolve a display-row index. An index past the end of the buffer yields a
 * slice whose filerow is >= numrows, which the draw loop paints as a tilde. */
void drow_at(int index, struct drow *out) {
  int i, cols, at;

  if (identity()) {
    out->filerow = index;
    out->rcol0 = 0;
    out->rcols = DROW_ALL;
    out->frag = 0;
    return;
  }

  cols = wrap_cols();
  /* A floor for the walk below, which would otherwise be asked for a slice
   * that cannot exist and leave *out unwritten. Nothing asks for one: the draw
   * loop counts up from the scroll offset and the two callers that can go
   * backwards clamp first. */
  if (index < 0) index = 0;
  i = 0;
  at = 0;
  if (memo.buf == V.buf && memo.gen == B.wrapgen && memo.cols == cols &&
      memo.wrap == V.wrap && memo.numrows == row_count() && index >= memo.index) {
    i = memo.filerow;
    at = memo.index;
  }
  for (; i < row_count(); i++) {
    int k = disp_rows(i, cols);
    if (index < at + k) {
      memo.buf = V.buf;
      memo.gen = B.wrapgen;
      memo.cols = cols;
      memo.wrap = V.wrap;
      memo.numrows = row_count();
      memo.index = at;
      memo.filerow = i;
      if (V.wrap) {
        wrap_walk(row_at(i), cols, index - at, -1, out);
      } else {
        out->rcol0 = 0;
        out->rcols = DROW_ALL;
        out->frag = 0;
      }
      out->filerow = i;
      return;
    }
    at += k;
  }
  out->filerow = row_count() + (index - at);
  out->rcol0 = 0;
  out->rcols = DROW_ALL;
  out->frag = 0;
}
