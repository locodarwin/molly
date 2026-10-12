/* block.c -- a rectangular selection, made of the cursors molly already has.

   Every selection in molly is a pair of positions, and a rectangle is not one --
   which is why block selection waited. What it turned out not to need is a
   second selection model. A block is one cursor per row, each selecting the
   same display columns, and a column of cursors with a selection each is
   something every edit path already handles: typing replaces each row's slice,
   Backspace and Delete remove them, Enter, Tab and paste land at each. None of
   that is in this file.

   What is here is the rectangle itself -- two corners, in display columns
   rather than bytes, because a block is a shape on the screen and a tab or a
   wide character makes the same column a different byte on every row -- and
   the two things a column of cursors cannot do on its own: grow as a shape, and
   come back off the clipboard as one.

   ---- When a block is a block

   Nothing records that a block is in progress. The corners are kept, and the
   block counts as live while the cursors are exactly the ones those corners
   build. Any other command moves a cursor, drops a selection or edits a row,
   and the block is over without anything having to end it -- the same way a
   Shift-selection ends when a key that is not a shifted movement arrives.

   ---- Edges

   A row shorter than the block's left edge still gets a cursor, at its end,
   with an empty selection: typing into a block then writes on every row, which
   is what a column of cursors does, and an empty selection is what keeps
   Backspace from deleting the character before it. A tab or a wide character
   that straddles an edge is taken whole on either side -- half a tab is not
   something a byte offset can say. */

#include "molly.h"

static struct {
  const struct view *view;  /* whose cursors these corners built */
  int ay, arx;              /* the corner that stays put */
  int cy, crx;              /* the corner that moves, where the primary is */
} blk;

/* The byte offset of display column rx in a row, rounding a character that
 * straddles rx down (it starts before rx) or up (it ends after it). */
static int rx_to_cx_edge(erow *row, int rx, int up) {
  int cx = editor_row_rx_to_cx(row, rx);
  if (up && cx < row->size && editor_row_cx_to_rx(row, cx) < rx)
    cx += (row->chars[cx] == '\t') ? 1 : utf8_char_bytes(row->chars, row->size, cx);
  return cx;
}

/* Where row y's cursor and anchor go, for the block's current corners. */
static void row_span(int y, int *sx, int *cx) {
  erow *row = row_at(y);
  int right = blk.crx >= blk.arx;
  *sx = rx_to_cx_edge(row, blk.arx, !right);
  *cx = rx_to_cx_edge(row, blk.crx, right);
}

static int top_row(void) { return blk.ay < blk.cy ? blk.ay : blk.cy; }
static int bottom_row(void) { return blk.ay > blk.cy ? blk.ay : blk.cy; }

/* Lay the cursors out from the corners: one per row, primary on the moving
 * corner's row. Returns 0 if there were more rows than cursors allowed, in
 * which case the block is cut short at the list's ceiling. */
static int build(void) {
  int y, top = top_row(), bot = bottom_row(), full = 1;

  cursors_collapse();
  selection_clear();
  for (y = top; y <= bot; y++) {
    int sx, cx;
    struct cursor *c;
    row_span(y, &sx, &cx);
    if (y == top) {
      c = &CUR;
      c->cy = y;
      c->cx = cx;
    } else {
      if (!cursors_add(y, cx)) { full = 0; break; }
      c = &V.cursors[V.ncursors - 1];
    }
    c->sx = sx;
    c->sy = y;
    c->sel_active = 1;
    c->rx = blk.crx;
    if (y == blk.cy) V.primary = V.ncursors - 1;
  }
  blk.view = &V;
  return full;
}

/* Whether the cursors are still the ones the corners built. */
int block_is_live(void) {
  int y, i, top, bot;
  if (blk.view != &V || row_count() == 0) return 0;
  top = top_row();
  bot = bottom_row();
  if (bot >= row_count() || V.ncursors != bot - top + 1) return 0;
  for (i = 0, y = top; y <= bot; y++, i++) {
    const struct cursor *c = &V.cursors[i];
    int sx, cx;
    row_span(y, &sx, &cx);
    if (!c->sel_active || c->cy != y || c->sy != y || c->cx != cx || c->sx != sx)
      return 0;
  }
  return 1;
}

static void say_size(void) {
  int w = blk.crx - blk.arx;
  if (w < 0) w = -w;
  editor_set_status_message("Block: %d row%s, %d column%s",
                            bottom_row() - top_row() + 1,
                            bottom_row() == top_row() ? "" : "s",
                            w, w == 1 ? "" : "s");
}

/* Select the block between two corners, given as (row, display column). */
void block_set(int ay, int arx, int cy, int crx) {
  if (row_count() == 0) return;
  if (ay < 0) ay = 0;
  if (cy < 0) cy = 0;
  if (ay >= row_count()) ay = row_count() - 1;
  if (cy >= row_count()) cy = row_count() - 1;
  if (arx < 0) arx = 0;
  if (crx < 0) crx = 0;
  blk.ay = ay;
  blk.arx = arx;
  blk.cy = cy;
  blk.crx = crx;
  if (!build())
    editor_set_status_message("A block is at most %d rows; this one stops there",
                              V.ncursors);
  else
    say_size();
}

/* Grow or shrink the block by one row or one column from its moving corner,
 * starting one first if there is none: from the primary's selection anchor if
 * it has one -- so a Shift-selection turns into the block it spans -- and from
 * the cursor if not. */
void block_extend(int dy, int drx) {
  int ay, arx, cy, crx, y, widest = 0;

  if (row_count() == 0) return;
  if (block_is_live()) {
    ay = blk.ay; arx = blk.arx; cy = blk.cy; crx = blk.crx;
  } else {
    int py = CUR.cy < row_count() ? CUR.cy : row_count() - 1;
    cy = py;
    crx = (CUR.cy < row_count()) ? editor_row_cx_to_rx(row_at(py), CUR.cx) : 0;
    ay = cy;
    arx = crx;
    if (CUR.sel_active && CUR.sy < row_count()) {
      ay = CUR.sy;
      arx = editor_row_cx_to_rx(row_at(ay), CUR.sx);
    }
  }

  cy += dy;
  if (cy < 0) cy = 0;
  if (cy >= row_count()) cy = row_count() - 1;
  crx += drx;
  if (crx < 0) crx = 0;
  /* No further right than the longest row the block covers: past that every
   * row's cursor sits at its end and a keypress would change nothing on
   * screen but the number in the status bar. */
  for (y = (ay < cy ? ay : cy); y <= (ay > cy ? ay : cy); y++)
    if (editor_row_cx_to_rx(row_at(y), row_at(y)->size) > widest)
      widest = editor_row_cx_to_rx(row_at(y), row_at(y)->size);
  if (drx > 0 && crx > widest) crx = widest > arx ? widest : arx;
  block_set(ay, arx, cy, crx);
}

/* The block's text, a line per row with no newline after the last. NULL when
 * no block is live. */
char *block_text(int *outlen) {
  int i, pos = 0, cap = 1;
  char *buf;
  if (!block_is_live()) return NULL;
  for (i = 0; i < V.ncursors; i++) {
    const struct cursor *c = &V.cursors[i];
    cap += (c->cx > c->sx ? c->cx - c->sx : c->sx - c->cx) + 1;
  }
  buf = xmalloc((size_t)cap);
  for (i = 0; i < V.ncursors; i++) {
    const struct cursor *c = &V.cursors[i];
    int a = c->sx < c->cx ? c->sx : c->cx;
    int b = c->sx < c->cx ? c->cx : c->sx;
    if (i > 0) buf[pos++] = '\n';
    memcpy(buf + pos, row_at(c->cy)->chars + a, (size_t)(b - a));
    pos += b - a;
  }
  buf[pos] = '\0';
  *outlen = pos;
  return buf;
}

/* How many lines the clipboard holds, counting a last one with no newline. */
static int clip_lines(void) {
  int i, n = 1;
  for (i = 0; i < E.clipsize; i++)
    if (E.clipboard[i] == '\n') n++;
  return n;
}

/* Paste a clipboard that came from a block.
 *
 * Onto as many cursors as it has lines, one line each -- a block copied and
 * pasted onto a block of the same height is the one case where handing out the
 * lines is not arithmetic the user has to do, because both came out of the
 * same shape. With one cursor it goes down as a rectangle: each line at the
 * cursor's display column on successive rows, padding a short row with spaces
 * out to that column and adding rows at the end of the file if it runs out.
 * Anything else is the ordinary paste. Returns 0 when it was none of these and
 * the caller should paste the ordinary way. */
int block_paste(void) {
  int n, i, rx, y0, start;
  const char **lines;
  int *lens;

  if (!E.clip_block || E.clipsize == 0) return 0;
  n = clip_lines();
  if (V.ncursors > 1 && V.ncursors != n) return 0;

  lines = xmalloc(sizeof(*lines) * (size_t)n);
  lens = xmalloc(sizeof(*lens) * (size_t)n);
  for (i = 0, start = 0; i < n; i++) {
    int e = start;
    while (e < E.clipsize && E.clipboard[e] != '\n') e++;
    lines[i] = E.clipboard + start;
    lens[i] = e - start;
    start = e + 1;
  }

  if (V.ncursors > 1) {
    cursors_insert_each(lines, lens);
    editor_set_status_message("Pasted %d lines, one per cursor", n);
    free(lines);
    free(lens);
    return 1;
  }

  /* One cursor: the rectangle, as one undo step. */
  selection_delete();
  undo_group_begin();
  y0 = CUR.cy;
  rx = (y0 < row_count()) ? editor_row_cx_to_rx(row_at(y0), CUR.cx) : 0;
  for (i = 0; i < n; i++) {
    int y = y0 + i, j;
    if (y >= row_count()) {
      /* Off the end of the file: a newline at column zero of the row past the
       * last one appends an empty row to paste into. */
      CUR.cy = row_count();
      CUR.cx = 0;
      editor_insert_newline();
    }
    CUR.cy = y;
    if (editor_row_cx_to_rx(row_at(y), row_at(y)->size) < rx) {
      CUR.cx = row_at(y)->size;
      for (j = editor_row_cx_to_rx(row_at(y), row_at(y)->size); j < rx; j++)
        editor_insert_char(' ');
    }
    CUR.cx = editor_row_rx_to_cx(row_at(y), rx);
    for (j = 0; j < lens[i]; j++) editor_insert_char((unsigned char)lines[i][j]);
  }
  undo_group_end();
  CUR.cy = y0 + n - 1;
  CUR.cx = editor_row_rx_to_cx(row_at(CUR.cy), rx) + lens[n - 1];
  editor_set_status_message("Pasted a block of %d lines", n);
  free(lines);
  free(lens);
  return 1;
}
