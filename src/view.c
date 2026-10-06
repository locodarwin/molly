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


/* Display column of the cursor at byte offset cx: tabs advance to the next
 * tab stop, other characters advance by their display width (0 for combining
 * marks, 2 for wide glyphs). This is what positions the on-screen cursor and
 * drives horizontal scrolling. */
int editor_row_cx_to_rx(erow *row, int cx) {
  int rx = 0, j = 0;
  while (j < cx) {
    if (row->chars[j] == '\t') {
      rx += KILO_TAB_STOP - (rx % KILO_TAB_STOP);
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
int editor_row_cx_to_charcol(erow *row, int cx) {
  int col = 0, j = 0;
  while (j < cx) {
    j += utf8_char_bytes(row->chars, row->size, j);
    col++;
  }
  return col;
}

/* Byte offset of the cursor within render (tabs expand to spaces there). The
 * search matches on render bytes, so it needs the cursor as a render byte
 * offset, not as a display column. */
int editor_row_cx_to_rbyte(erow *row, int cx) {
  int rb = 0, j = 0;
  while (j < cx) {
    if (row->chars[j] == '\t') {
      rb += KILO_TAB_STOP - (rb % KILO_TAB_STOP);
      j++;
    } else {
      int nb = utf8_char_bytes(row->chars, row->size, j);
      rb += nb;
      j += nb;
    }
  }
  return rb;
}

/* Inverse of the above: map a render byte offset (where a search matched) back
 * to a byte offset in chars. */
int editor_render_byte_to_cx(erow *row, int rbyte) {
  int rb = 0, cx = 0;
  while (cx < row->size) {
    int nb;
    if (row->chars[cx] == '\t') {
      rb += KILO_TAB_STOP - (rb % KILO_TAB_STOP);
      nb = 1;
    } else {
      nb = utf8_char_bytes(row->chars, row->size, cx);
      rb += nb;
    }
    if (rb > rbyte) return cx;
    cx += nb;
  }
  return row->size;
}


/* Display rows the buffer occupies. */
static int drow_total(void) {
  return row_count();
}

/* The display row holding a cursor. */
int drow_of_cursor(const struct cursor *c) {
  return c->cy;
}

/* Resolve a display-row index. An index past the end of the buffer yields a
 * slice whose filerow is >= numrows, which the draw loop paints as a tilde. */
void drow_at(int index, struct drow *out) {
  out->filerow = index;
  out->rcol0 = 0;
  out->rcols = DROW_ALL;
  out->frag = 0;
}

/* Step a cursor one display row up (dir < 0) or down (dir > 0). Unwrapped that
 * is one file row. Wrapped, it will move between slices of the same long line
 * before reaching the next line, which is why vertical movement goes through
 * here rather than touching cy directly. cy == numrows is the phantom row just
 * past the last line, which the editor allows the cursor to occupy. */
static void drow_step(struct cursor *c, int dir) {
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
    while (CUR.cx > 0 && CUR.cx < row->size &&
           utf8_is_cont((unsigned char)row->chars[CUR.cx]))
      CUR.cx = utf8_prev(row->chars, CUR.cx);
  } else {
    CUR.cx = 0;
  }
}

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
    if (cur_drow < V.drowoff) V.drowoff = cur_drow;
    if (cur_drow >= V.drowoff + V.screenrows)
      V.drowoff = cur_drow - V.screenrows + 1;
    if (V.drowoff > total) V.drowoff = total;
    if (V.drowoff < 0) V.drowoff = 0;
  }
  if (CUR.rx < V.coloff) V.coloff = CUR.rx;
  if (CUR.rx >= V.coloff + TEXT_COLS()) V.coloff = CUR.rx - TEXT_COLS() + 1;
}
