/* indent.c -- What one indent step is, and where its shape comes from.

   Three questions, each answerable per filetype and otherwise from `[editor]`:
   how wide a tab is, whether Tab inserts spaces instead of one, and whether a
   new line starts where the line above it did.

   The resolution lives here rather than in config.c because a per-filetype
   override is not a setting -- the settings table holds one value per name, and
   `[syntax python] tab width = 4` is a second value for a name the table has
   already answered. So the table keeps the default, `struct syntax` keeps the
   override, and the two are reconciled by the three functions below.

   The editing operations are here too, and not in edit.c, because what they do
   depends on these answers. edit.c stays the layer that knows how to change a
   row and what record reverses it; this is the layer that knows an indent. */

#include "molly.h"

/* The two `[editor]` flags. Tab width is not here: it is older than this file,
 * it is read once per row by the renderer, and it already had a home. */
static int insert_spaces;
static int auto_indent = 1;

/* Each flag reads two ways: what is in force for the current buffer, which is
 * what an edit asks for, and the `[editor]` default on its own, which is what
 * write-back saves. Writing the resolved answer would turn one filetype's
 * override into everybody's the first time settings were saved with that kind
 * of file open. */
void indent_set_insert_spaces(int v) { insert_spaces = v; }
int indent_insert_spaces_default(void) { return insert_spaces; }
int indent_insert_spaces(void) {
  int v = (V.buf != NULL) ? syntax_insert_spaces(B.syn) : -1;
  return (v >= 0) ? v : insert_spaces;
}

void indent_set_auto(int v) { auto_indent = v; }
int indent_auto_default(void) { return auto_indent; }
int indent_auto(void) {
  int v = (V.buf != NULL) ? syntax_auto_indent(B.syn) : -1;
  return (v >= 0) ? v : auto_indent;
}

/* Bring the current buffer's render strings up to date with the tab width now
 * in force.
 *
 * A buffer carries the width its rows were rendered at, because the width is a
 * function of the *filetype* and two windows can show two filetypes in one
 * frame. A single global "current width" re-rendered whole files twice per
 * frame in that arrangement, each window undoing the other's work.
 *
 * Called from three places, and a no-op in all of them unless something moved:
 * syntax_retype() (the name, so the filetype, changed), tab_width_set() (the
 * default changed), and window_scroll_all() per window per frame, which is the
 * backstop that catches a buffer becoming current without either. */
void indent_sync(void) {
  int w;
  if (V.buf == NULL) return;
  w = tab_width();
  if (w == B.tabw) return;
  B.tabw = w;
  editor_rerender_all();
}

/* ---- the editing operations ------------------------------------------------ */

/* How many spaces reach the next tab stop from where the cursor is.
 *
 * Measured in render columns rather than bytes, because a row indented with
 * literal tabs puts the cursor at a column its byte offset does not give. */
static int to_next_stop(void) {
  int w = tab_width(), rx = 0;
  if (CUR.cy < row_count()) rx = editor_row_cx_to_rx(row_at(CUR.cy), CUR.cx);
  return w - (rx % w);
}

/* One indent step at the cursor: a tab, or spaces as far as the next stop.
 *
 * The single way Tab becomes text, so that the choice is made in one place
 * rather than at every keystroke path that might carry a tab. The spaces
 * coalesce into one undo step on their own -- editor_insert_char() joins a run
 * of single-byte inserts -- so there is no group here. */
void indent_insert_step(void) {
  int n, i;
  if (!indent_insert_spaces()) {
    editor_insert_char('\t');
    return;
  }
  n = to_next_stop();
  for (i = 0; i < n; i++) editor_insert_char(' ');
}

/* Enter, with the new line starting under the old one's first character.
 *
 * Separate from editor_insert_newline() and not a change to it: paste and the
 * shell filter both split rows through that function, and text that arrives
 * already indented must not be indented again. So the mechanism stays blind to
 * indentation and the command that a keystroke reaches is the one that carries
 * it down -- which is the same split this repo keeps everywhere else.
 *
 * The rule is that the text which moves down keeps the column it was at: only
 * the whitespace *behind the cursor* is copied, and the tail keeps its own. So
 * splitting "    alpha" after two spaces leaves "alpha" at column four, where
 * it started, and splitting at column zero copies nothing -- which is what
 * makes Enter there insert a blank line above rather than an indented one. The
 * references disagree about the middle case, so it is pinned in both suites. */
void editor_newline_and_indent(void) {
  char *lead = NULL;
  int n = 0, i;

  if (indent_auto() && CUR.cy < row_count()) {
    erow *row = row_at(CUR.cy);
    while (n < CUR.cx && n < row->size &&
           (row->chars[n] == ' ' || row->chars[n] == '\t'))
      n++;
    if (n > 0) {
      lead = xmalloc((size_t)n);
      memcpy(lead, row->chars, (size_t)n);
    }
  }

  /* One undo step for the whole gesture: Backspace-ing a line you did not mean
   * to start should not leave its indentation behind for a second press. */
  undo_group_begin();
  editor_insert_newline();
  for (i = 0; i < n; i++) editor_insert_char(lead[i]);
  undo_group_end();
  free(lead);
}

/* ---- a block, and the whitespace in front of one row ----------------------- */

/* How many bytes of leading space and tab this row starts with. */
static int lead_bytes(erow *row) {
  int n = 0;
  while (n < row->size && (row->chars[n] == ' ' || row->chars[n] == '\t')) n++;
  return n;
}

/* Where a position on this row ends up when `n` bytes are inserted at the front
 * of it, or removed from [keep, keep+n).
 *
 * Column zero stays at column zero on an indent: a selection anchored there is
 * a selection of whole rows, and moving the anchor would uncover the
 * indentation that was just put in front of it. */
static int shift_in(int cx, int n) { return (cx == 0) ? 0 : cx + n; }

static int shift_out(int cx, int keep, int n) {
  if (cx <= keep) return cx;
  return (cx >= keep + n) ? cx - n : keep;
}

/* Record one row edit as part of the group already open.
 *
 * `cx0/cy0` is where undo puts the cursor and `cx1/cy1` where redo does; a
 * group's undo is applied in reverse push order, so every record here carries
 * the position from before the whole operation and the first-pushed one wins. */
static void record(int kind, int y, int at, const char *text, int len,
                   int pre_cx, int pre_cy, int post_cx) {
  struct uchange *u = uc_new(kind, y, at, text, (size_t)len);
  u->back = (kind == UC_DELETE);
  u->cx0 = pre_cx;
  u->cy0 = pre_cy;
  u->cx1 = (y == pre_cy) ? post_cx : pre_cx;
  u->cy1 = pre_cy;
  undo_push(u);
  clear_redo();
}

/* One indent level in front of row y. A blank row is left alone: an indent on
 * nothing is trailing whitespace, and every editor that does this skips it. */
static void row_indent(int y, int pre_cx, int pre_cy) {
  erow *row = row_at(y);
  char step[64];
  int n, i;

  if (row->size == 0) return;
  if (!indent_insert_spaces()) {
    step[0] = '\t';
    n = 1;
  } else {
    n = tab_width();
    if (n > (int)sizeof(step)) n = (int)sizeof(step);
    memset(step, ' ', (size_t)n);
  }
  record(UC_INSERT, y, 0, step, n, pre_cx, pre_cy, shift_in(pre_cx, n));
  editor_row_insert_n(row, 0, step, (size_t)n);
  for (i = 0; i < V.ncursors; i++) {
    struct cursor *c = &V.cursors[i];
    if (c->cy == y) c->cx = shift_in(c->cx, n);
    if (c->sel_active && c->sy == y) c->sx = shift_in(c->sx, n);
  }
}

/* One indent level off the front of row y.
 *
 * The rule, because the references do not all agree and a row can be indented
 * with both characters: the row's indentation goes back to the previous tab
 * stop, and whole characters come off the end of the whitespace run -- the end
 * nearest the text -- until it gets there. So four spaces at a width of four
 * leave nothing, six leave four, and a tab followed by two spaces loses the two
 * spaces. A tab that straddles the stop is removed whole, because half a tab is
 * not a thing to leave behind; that undershoots the stop, which is the one case
 * where this takes off more than it put on. */
static void row_dedent(int y, int pre_cx, int pre_cy) {
  erow *row = row_at(y);
  int n = lead_bytes(row), w = tab_width(), width, target, keep, i;

  if (n == 0) return;
  width = editor_row_cx_to_rx(row, n);
  target = width - ((width % w) ? (width % w) : w);
  if (target < 0) target = 0;
  keep = n;
  while (keep > 0 && editor_row_cx_to_rx(row, keep) > target) keep--;
  if (keep == n) return;

  record(UC_DELETE, y, keep, row->chars + keep, n - keep, pre_cx, pre_cy,
         shift_out(pre_cx, keep, n - keep));
  editor_row_delete_n(row, keep, n - keep);
  for (i = 0; i < V.ncursors; i++) {
    struct cursor *c = &V.cursors[i];
    if (c->cy == y) c->cx = shift_out(c->cx, keep, n - keep);
    if (c->sel_active && c->sy == y) c->sx = shift_out(c->sx, keep, n - keep);
  }
}

/* Tab and Shift-Tab over rows: `dir` > 0 indents them, `dir` < 0 dedents.
 *
 * Which rows: the ones a selection touches, or one row per cursor when nothing
 * is selected. A selection that ends at column zero of a row does not include
 * that row -- it was dragged to the start of it and stopped, and indenting the
 * row the user is about to reach is not what the drag said.
 *
 * The selection is kept, and kept over the same rows: Tab on a block is a
 * gesture you repeat, and an editor that collapsed the selection after the
 * first press would make the second one type a tab over the text. */
void indent_shift(int dir) {
  int pre_cx = CUR.cx, pre_cy = CUR.cy;
  int y0, y1, y, i;

  if (V.buf == NULL || row_count() == 0 || dir == 0) return;

  if (CUR.sel_active) {
    int ax, ay, bx, by;
    if (!selection_span(&ax, &ay, &bx, &by)) return;
    y0 = ay;
    y1 = (bx == 0 && by > ay) ? by - 1 : by;
  } else {
    y0 = y1 = CUR.cy;
    for (i = 0; i < V.ncursors; i++) {
      if (V.cursors[i].cy < y0) y0 = V.cursors[i].cy;
      if (V.cursors[i].cy > y1) y1 = V.cursors[i].cy;
    }
  }
  if (y0 < 0) y0 = 0;
  if (y1 >= row_count()) y1 = row_count() - 1;

  undo_group_begin();
  for (y = y0; y <= y1; y++) {
    /* With cursors rather than a selection, only the rows that have one. */
    if (!CUR.sel_active) {
      int has = 0;
      for (i = 0; i < V.ncursors; i++)
        if (V.cursors[i].cy == y) has = 1;
      if (!has) continue;
    }
    if (dir > 0) row_indent(y, pre_cx, pre_cy);
    else row_dedent(y, pre_cx, pre_cy);
  }
  undo_group_end();
}

/* Backspace in a row's leading whitespace, eating one indent level rather than
 * one space. Returns 0 when this is not that keystroke, and the ordinary
 * Backspace runs instead.
 *
 * Only in the whitespace and only when there is more than one character to
 * take: a single character is what plain Backspace already does, and leaving it
 * there keeps a run of them coalescing into one undo step. That is also what
 * keeps this invisible in a tab-indented file, where one indent level is one
 * character. */
int indent_backspace(void) {
  erow *row;
  int w, rx, target, keep;

  if (V.buf == NULL || CUR.sel_active || CUR.cy >= row_count()) return 0;
  if (CUR.cx <= 0) return 0;
  row = row_at(CUR.cy);
  if (CUR.cx > lead_bytes(row)) return 0;

  w = tab_width();
  rx = editor_row_cx_to_rx(row, CUR.cx);
  if (rx <= 0) return 0;
  target = rx - ((rx % w) ? (rx % w) : w);
  if (target < 0) target = 0;
  keep = CUR.cx;
  while (keep > 0 && editor_row_cx_to_rx(row, keep) > target) keep--;
  if (CUR.cx - keep < 2) return 0;

  record(UC_DELETE, CUR.cy, keep, row->chars + keep, CUR.cx - keep,
         CUR.cx, CUR.cy, keep);
  editor_row_delete_n(row, keep, CUR.cx - keep);
  CUR.cx = keep;
  return 1;
}

/* ---- converting what is already there ------------------------------------- */

/* Rewrite the leading whitespace of the rows a selection touches -- or of every
 * row, with nothing selected -- as spaces (`to_spaces`) or as tabs. Returns the
 * number of rows that changed; one undo step for all of them.
 *
 * `insert spaces` only decides what the next Tab writes, so a file of tabs stays
 * one until something converts it, and this is that something. Only the
 * indentation: a tab after the first non-blank character may be data -- a TSV
 * column, a Makefile's recipe after a target -- and an editor that rewrote it
 * as spaces would be changing what the file means rather than how it looks.
 *
 * Measured at the tab width in force for this file, so the indentation looks
 * exactly as it did before. To tabs, a width that is not a whole number of tab
 * stops keeps the remainder as spaces: four tabs and two spaces is still the
 * same column, where rounding either way would move the text. */
int indent_convert(int to_spaces) {
  int pre_cx = CUR.cx, pre_cy = CUR.cy;
  int w = tab_width(), y0 = 0, y1, y, changed = 0;
  char *buf = NULL;
  int bufcap = 0;

  if (V.buf == NULL || row_count() == 0) return 0;
  y1 = row_count() - 1;
  if (CUR.sel_active) {
    int ax, ay, bx, by;
    if (!selection_span(&ax, &ay, &bx, &by)) return 0;
    y0 = ay;
    y1 = (bx == 0 && by > ay) ? by - 1 : by;
    if (y1 >= row_count()) y1 = row_count() - 1;
  }

  undo_group_begin();
  for (y = y0; y <= y1; y++) {
    erow *row = row_at(y);
    int n = lead_bytes(row), width, len, k, post;

    /* A row that is nothing but whitespace is left as it is: converting it
     * would be an edit to trailing whitespace nobody asked about. */
    if (n == 0 || n == row->size) continue;
    width = editor_row_cx_to_rx(row, n);
    len = to_spaces ? width : width / w + width % w;
    if (len > bufcap) {
      bufcap = len + 16;
      buf = xrealloc(buf, (size_t)bufcap);
    }
    if (to_spaces) {
      memset(buf, ' ', (size_t)len);
    } else {
      memset(buf, '\t', (size_t)(width / w));
      memset(buf + width / w, ' ', (size_t)(width % w));
    }
    if (len == n && memcmp(buf, row->chars, (size_t)n) == 0) continue;

    post = (pre_cx >= n) ? pre_cx + len - n : (pre_cx > 0 ? len : 0);
    record(UC_DELETE, y, 0, row->chars, n, pre_cx, pre_cy, post);
    editor_row_delete_n(row, 0, n);
    record(UC_INSERT, y, 0, buf, len, pre_cx, pre_cy, post);
    editor_row_insert_n(row, 0, buf, (size_t)len);

    /* A cursor in the text keeps its character; one in the indentation goes to
     * the end of it, since the column it was on may no longer be a boundary. */
    for (k = 0; k < V.ncursors; k++) {
      struct cursor *c = &V.cursors[k];
      if (c->cy == y && c->cx > 0) c->cx = (c->cx >= n) ? c->cx + len - n : len;
      if (c->sel_active && c->sy == y && c->sx > 0)
        c->sx = (c->sx >= n) ? c->sx + len - n : len;
    }
    changed++;
  }
  undo_group_end();
  free(buf);
  return changed;
}
