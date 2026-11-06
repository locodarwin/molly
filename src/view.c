/* view.c -- A window onto a buffer: the cursor list, the display-row mapping that makes
   soft wrap possible, cursor movement and scrolling. */

#include "molly.h"

/* --- cursors: one list, normally holding exactly one entry ---------------- */

/* Allocate the cursor list with a single cursor at the top of the buffer. */
void cursors_init(void) {
  V.cursorcap = 4;
  V.cursors = xmalloc(sizeof(struct cursor) * (size_t)V.cursorcap);
  V.ncursors = 1;
  V.primary = 0;
  memset(V.cursors, 0, sizeof(struct cursor) * (size_t)V.cursorcap);
}

/* Discard every cursor but the primary, which becomes cursor 0. Anything that
 * invalidates positions wholesale -- loading another file, or undoing past the
 * point where the extra cursors were made -- collapses rather than trying to
 * fix them up. */
void cursors_collapse(void) {
  if (V.ncursors > 1) {
    if (V.primary != 0) V.cursors[0] = V.cursors[V.primary];
    V.ncursors = 1;
  }
  V.primary = 0;
  /* Back to one cursor is back to the start of the numbering: the survivor is
   * the oldest there is, and nothing is left for `cursors_newest()` to pick
   * out. Letting the counter run on would work too and would eventually
   * overflow. */
  V.cursors[0].seq = 0;
  V.cursorseq = 0;
}

/* Clamp every cursor into the buffer. A deletion can leave a cursor past the
 * last row or past the end of its line; with one cursor this is the same clamp
 * the movement code already does, and with several it is the only thing keeping
 * the far ones valid. */
static void cursors_clamp(void) {
  int i;
  for (i = 0; i < V.ncursors; i++) {
    struct cursor *c = &V.cursors[i];
    if (c->cy > row_count()) c->cy = row_count();
    if (c->cy < 0) c->cy = 0;
    if (c->cy < row_count() && c->cx > (*row_at(c->cy)).size)
      c->cx = (*row_at(c->cy)).size;
    if (c->cy >= row_count()) c->cx = 0;
    if (c->cx < 0) c->cx = 0;
  }
}

/* One cursor at (cy, cx), for a jump to a place remembered from earlier: a
 * bookmark, a step back, a problem or a search result. The text may have
 * changed since, so the row is clamped to the buffer, the column to the row,
 * and a column that now falls inside a character moves to its start, or the
 * next thing typed would split the character in two. */
void cursor_place(int cy, int cx) {
  erow *r;
  cursors_collapse();
  CUR.sel_active = 0;
  if (cy >= row_count()) cy = row_count() > 0 ? row_count() - 1 : 0;
  if (cy < 0) cy = 0;
  CUR.cy = cy;
  if (cy >= row_count()) {
    CUR.cx = 0;
    return;
  }
  r = row_at(cy);
  if (cx > r->size) cx = r->size;
  if (cx < 0) cx = 0;
  CUR.cx = cx < r->size ? utf8_boundary(r->chars, r->size, cx) : cx;
}


/* One display column's step along a row's stored bytes, for the walks that lay a
 * row out on the screen: the draw loop and the wrapper.
 *
 * Returns the columns the character at byte `i` occupies when it starts at
 * column `col`, and sets *nb to the bytes to advance past it. A tab is the
 * exception, and it is taken one column at a time: each call returns 1, and *nb
 * stays 0 until the column that reaches the tab stop. That is what a row's tab
 * used to be when rows kept a second, tab-expanded copy of themselves -- a run
 * of spaces -- so a slice edge or the horizontal scroll can fall inside a tab
 * and the columns either side of it still draw, with the tab's own role, as
 * they always did. Without the copy, a row costs its bytes and nothing more.
 *
 * `col` is the column the walk has reached, which inside a tab is part-way
 * along it; the tab's remaining width is a function of that alone. */
int text_cell(const char *s, int len, int i, int col, int tabw, int *nb) {
  if (s[i] == '\t') {
    *nb = ((col + 1) % tabw == 0) ? 1 : 0;
    return 1;
  }
  return utf8_char_cols(s, len, i, nb);
}

/* Display column of the cursor at byte offset cx: tabs advance to the next
 * tab stop, other characters advance by their display width (0 for combining
 * marks, 2 for wide glyphs). This is what positions the on-screen cursor and
 * drives horizontal scrolling. */
int editor_row_cx_to_rx(erow *row, int cx) {
  int rx = 0, j = 0;
  while (j < cx) {
    if (row->chars[j] == '\t') {
      rx += tab_width() - (rx % tab_width());
      j++;
    } else {
      int nb;
      rx += utf8_char_cols(row->chars, row->size, j, &nb);
      j += nb;
    }
  }
  return rx;
}

/* Character index of the cursor at byte offset cx: the count of whole
 * characters before it, regardless of how wide any of them render (a tab or
 * a double-width glyph is one character, not eight or two). This is what
 * the status bar reports as "Col" -- a tab-stop- or wide-glyph-aware display
 * column would jump around in a way that doesn't match "the Nth character
 * in the line," which is what most editors and tools mean by column. */
/* The byte offset whose glyph covers display column `rx`: the inverse of
 * editor_row_cx_to_rx, for turning a mouse click into a cursor position.
 *
 * A click lands on a column, which may be in the middle of a tab's run of blanks or
 * on the second half of a wide glyph; either way the whole character is what the
 * cursor goes to, so the walk stops at the character whose span contains the column.
 * A click past the last glyph gives the end of the line, which is where someone
 * clicking in the empty space to the right of a line expects the cursor. */
int editor_row_rx_to_cx(erow *row, int rx) {
  int cur = 0, j = 0;
  while (j < row->size) {
    int w, nb;
    if (row->chars[j] == '\t') {
      w = tab_width() - (cur % tab_width());
      nb = 1;
    } else {
      w = utf8_char_cols(row->chars, row->size, j, &nb);
    }
    if (cur + w > rx) return j;
    cur += w;
    j += nb;
  }
  return row->size;
}

int editor_row_cx_to_charcol(erow *row, int cx) {
  int col = 0, j = 0;
  while (j < cx) {
    j += utf8_char_bytes(row->chars, row->size, j);
    col++;
  }
  return col;
}

/* Step a cursor one display row up (dir < 0) or down (dir > 0). Unwrapped that
 * is one file row. Wrapped, it moves between slices of the same long line before
 * reaching the next line, which is why vertical movement goes through here
 * rather than touching cy directly. cy == numrows is the phantom row just past
 * the last line, which the editor allows the cursor to occupy.
 *
 * Unwrapped it keeps the byte offset, which is what molly has always done and
 * what every test of an arrow key is written against. Wrapped it keeps the
 * *screen column*, because there is no other sane reading of "up" between two
 * slices of one line: the byte offset would move the cursor a windowful along
 * the same row. The two differ only on a row with a tab or a wide glyph before
 * the cursor, which is the asymmetry ROADMAP.md section 7 writes down.
 *
 * The wrapped half is wrap_step(), which is local to the row the cursor is on
 * and the one it is moving to -- an arrow key does not get slower because the
 * file is long. */
static void drow_step(struct cursor *c, int dir) {
  if (V.wrap) {
    wrap_step(c, dir);
    return;
  }
  if (dir < 0) {
    if (c->cy != 0) c->cy--;
  } else {
    if (c->cy < row_count()) c->cy++;
  }
}

void editor_move_cursor(int key) {
  erow *row = (CUR.cy >= row_count()) ? NULL : row_at(CUR.cy);

  switch (key) {
    case ARROW_LEFT:
      if (CUR.cx != 0) {
        CUR.cx = utf8_prev(row->chars, CUR.cx);
      } else if (CUR.cy > 0) {
        CUR.cy--;
        CUR.cx = (*row_at(CUR.cy)).size;
      }
      break;
    case ARROW_RIGHT:
      if (row && CUR.cx < row->size) {
        CUR.cx += utf8_char_bytes(row->chars, row->size, CUR.cx);
      } else if (row && CUR.cx == row->size) {
        CUR.cy++;
        CUR.cx = 0;
      }
      break;
    case ARROW_UP:
      drow_step(&CUR, -1);
      break;
    case ARROW_DOWN:
      drow_step(&CUR, 1);
      break;
  }

  row = (CUR.cy >= row_count()) ? NULL : row_at(CUR.cy);
  if (row) {
    if (CUR.cx > row->size) CUR.cx = row->size;
    /* A vertical move can land the byte offset inside a character on the new
     * line; step back to the character boundary. */
    if (CUR.cx < row->size) CUR.cx = utf8_boundary(row->chars, row->size, CUR.cx);
  } else {
    CUR.cx = 0;
  }
}

/* Where the cursor was when the view was last scrolled, so editor_scroll can tell a
 * cursor that moved from a view the user scrolled. Starts at a position no cursor
 * can hold, so the first frame counts as movement and scrolls the cursor into view. */
static int last_scroll_cy = -1;
static int last_scroll_cx = -1;

void editor_scroll(void) {
  /* Every edit path funnels through a refresh, so clamping here keeps the whole
   * list valid without each one having to remember to. With a single cursor
   * this is the clamp the movement code already applies. */
  cursors_clamp();

  CUR.rx = 0;
  if (CUR.cy < row_count()) {
    CUR.rx = editor_row_cx_to_rx(row_at(CUR.cy), CUR.cx);
  }

  {
    int cur_drow = drow_of_cursor(&CUR);
    int total = drow_total();
    /* The view follows the cursor only when the cursor has moved.
     *
     * Olly's view was welded to the cursor: every frame pulled the scroll offset
     * back until the cursor was visible. That is right for keyboard movement and
     * wrong for a wheel, which scrolls the view and deliberately leaves the cursor
     * where it is -- welded, a wheel event was undone by the very next frame. So the
     * view is allowed to detach, and any cursor movement re-attaches it, which is
     * what every editor does and what makes scrolling up to look at something and
     * then typing behave the way people expect. */
    int cursor_moved = (CUR.cy != last_scroll_cy || CUR.cx != last_scroll_cx);
    last_scroll_cy = CUR.cy;
    last_scroll_cx = CUR.cx;
    if (cursor_moved) {
      if (cur_drow < V.drowoff) V.drowoff = cur_drow;
      if (cur_drow >= V.drowoff + V.screenrows)
        V.drowoff = cur_drow - V.screenrows + 1;
    }
    if (V.drowoff > total) V.drowoff = total;
    if (V.drowoff < 0) V.drowoff = 0;
  }
  /* Wrap and horizontal scrolling are alternatives: a wrapped view has nothing
   * to scroll sideways to, and a nonzero offset would shift every slice of
   * every row by it. So one of the two is always zero, and every column
   * calculation downstream can go on subtracting coloff without asking. */
  if (V.wrap) {
    V.coloff = 0;
    return;
  }
  if (CUR.rx < V.coloff) V.coloff = CUR.rx;
  if (CUR.rx >= V.coloff + TEXT_COLS()) V.coloff = CUR.rx - TEXT_COLS() + 1;
}
