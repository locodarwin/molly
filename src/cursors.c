/* cursors.c -- N cursors: keeping the list ordered and distinct, and applying one
   edit at every one of them as a single undo step.

   The list itself is older than this file -- `struct cursor` and `V.cursors`
   landed in Phase 0 so that every edit path was written against a list from the
   start. What was missing is the two hard parts, and both are here.

   ---- Why an edit at one cursor breaks the others

   Every editing operation in molly works on `CUR`, the primary cursor, and
   mutates the buffer under it. Run one at each cursor in turn and the positions
   of the rest stop meaning anything: insert a character at row 5 column 3 and a
   cursor further along row 5 is one byte out, while a split moves every row
   below it down.

   Two things make that tractable, and neither of them needs a new edit path.

   The first is the *order*. Cursors are sorted and run top to bottom, so each
   edit happens strictly earlier in the document than every cursor still to come.
   Nothing an edit does reaches text before it, so a cursor that has already been
   processed is final the moment it is done -- which is the whole reason the loop
   runs forwards rather than backwards.

   The second is that one rule fixes up the cursors still to come, whatever the
   edit was. Every operation here is a *replace*: it removes the text in some
   range and leaves the cursor where the text that followed that range now
   begins. Call the end of the removed range (r1, c1) and the cursor's position
   afterwards (nr, nc), and for any position after (r1, c1):

       on row r1   ->  (nr, nc + (col - c1))
       below r1    ->  (row + (nr - r1), col)

   That is one rule for typing, Tab, Enter, Backspace, Delete and paste alike,
   derived from nothing but where the cursor ended up. Deleting forward is the
   only operation whose removed range ends somewhere other than the cursor it
   started at, which is why `range_end()` below has a case for it and nothing
   else does.

   ---- One undo step

   The grouping was already there: `undo_group_begin()` nests, so the outer group
   this file opens swallows the ones that `selection_delete()` and
   `editor_newline_and_indent()` open for themselves, and every record from every
   cursor carries the same group id. `editor_undo()` pops a whole group, so N
   simultaneous edits come back in one press without undo learning anything new.

   What undo cannot do is put the cursors back -- a record restores one position,
   not a list -- so undoing collapses to a single cursor. That is the same answer
   `cursors_collapse()` already gave for loading another file: when positions
   stop meaning anything, stop pretending they do. */

#include "molly.h"

/* A ceiling on the list. Nothing reaches it by hand; it is here so that a
 * command which adds a cursor per match cannot be handed a file that turns one
 * keystroke into a million allocations. */
#define CURSORS_MAX 512

/* ---- the list ------------------------------------------------------------- */

/* Document order, which is the order the edit loop depends on. */
static int before(const struct cursor *a, const struct cursor *b) {
  if (a->cy != b->cy) return a->cy < b->cy;
  return a->cx < b->cx;
}

/* Sort into document order, keeping `V.primary` pointing at the same cursor it
 * pointed at before.
 *
 * An insertion sort over the real array rather than qsort over indices: the
 * list is tens of entries at the most, the primary has to be followed through
 * the swaps anyway, and qsort would need a comparator that cannot see which
 * entry the view cares about. */
void cursors_sort(void) {
  int i, j;
  for (i = 1; i < V.ncursors; i++) {
    struct cursor hold = V.cursors[i];
    int from = i;
    for (j = i - 1; j >= 0 && before(&hold, &V.cursors[j]); j--) {
      V.cursors[j + 1] = V.cursors[j];
      if (V.primary == j) V.primary = j + 1;
      from = j;
    }
    V.cursors[from] = hold;
    if (V.primary == i) V.primary = from;
  }
}

/* Drop cursors that share a position with another.
 *
 * Two cursors at one place would insert twice at that place and delete twice
 * from it, so they are not merely redundant but wrong. The list is sorted here,
 * so duplicates are adjacent. The primary survives: if it is one of a pair, the
 * one that stays becomes the primary. */
void cursors_dedup(void) {
  int r = 1, w = 1;
  if (V.ncursors < 2) return;
  cursors_sort();
  for (; r < V.ncursors; r++) {
    if (V.cursors[r].cy == V.cursors[w - 1].cy &&
        V.cursors[r].cx == V.cursors[w - 1].cx) {
      /* Keep whichever of the two the view is following, and keep a selection
       * over none -- a cursor Ctrl-D made has one and a bare duplicate does
       * not, and losing it would lose what the next edit is meant to replace. */
      if (V.primary == r) V.primary = w - 1;
      if (V.cursors[r].sel_active && !V.cursors[w - 1].sel_active)
        V.cursors[w - 1] = V.cursors[r];
      continue;
    }
    if (V.primary == r) V.primary = w;
    V.cursors[w++] = V.cursors[r];
  }
  V.ncursors = w;
  if (V.primary >= V.ncursors) V.primary = V.ncursors - 1;
}

/* Add a cursor at (cy, cx). Returns 0 if one is already there, or if the list is
 * full; the caller is what decides whether either is worth a message. */
int cursors_add(int cy, int cx) {
  int i;
  for (i = 0; i < V.ncursors; i++)
    if (V.cursors[i].cy == cy && V.cursors[i].cx == cx) return 0;
  if (V.ncursors >= CURSORS_MAX) return 0;
  if (V.ncursors == V.cursorcap) {
    V.cursorcap = V.cursorcap ? V.cursorcap * 2 : 4;
    V.cursors = xrealloc(V.cursors,
                         sizeof(struct cursor) * (size_t)V.cursorcap);
  }
  memset(&V.cursors[V.ncursors], 0, sizeof(struct cursor));
  V.cursors[V.ncursors].cy = cy;
  V.cursors[V.ncursors].cx = cx;
  V.ncursors++;
  return 1;
}

/* Add a cursor with a selection over [cx, ex) of row cy -- what a command that
 * finds an occurrence wants, since the point of selecting it is that the next
 * keystroke replaces it. */
int cursors_add_sel(int cy, int cx, int ex) {
  if (!cursors_add(cy, ex)) return 0;
  {
    struct cursor *c = &V.cursors[V.ncursors - 1];
    c->sel_active = 1;
    c->sx = cx;
    c->sy = cy;
  }
  return 1;
}

/* ---- applying one edit at every cursor ------------------------------------ */

/* Where the text that this operation is about to remove ends.
 *
 * For everything but a forward delete that is the cursor itself: typing, Tab,
 * Enter and paste remove nothing, and Backspace removes what is behind. A
 * selection moves it to the far end of the selection, because that is the text
 * the operation replaces. */
static void range_end(int what, int *r1, int *c1) {
  *r1 = CUR.cy;
  *c1 = CUR.cx;

  if (CUR.sel_active) {
    int ax, ay, bx, by;
    if (selection_span(&ax, &ay, &bx, &by)) {
      *r1 = by;
      *c1 = bx;
      return;
    }
  }

  if (what == MC_DELETE_FWD && CUR.cy < row_count()) {
    erow *row = row_at(CUR.cy);
    if (CUR.cx < row->size) {
      *c1 = CUR.cx + utf8_char_bytes(row->chars, row->size, CUR.cx);
    } else if (CUR.cy < row_count() - 1) {
      /* The row's terminator is what gets removed, so the range ends at the
       * start of the row that is about to be joined on. */
      *r1 = CUR.cy + 1;
      *c1 = 0;
    }
  }
}

/* Move one cursor across an edit that replaced the text ending at (r1, c1) with
 * text ending at (nr, nc). See the rule at the top of this file. */
static void cursor_shift(struct cursor *c, int r1, int c1, int nr, int nc) {
  int drows = nr - r1;

  /* The anchor travels with the cursor: a selection that survives an edit
   * earlier in the document has to keep covering the same characters. */
  if (c->sel_active) {
    if (c->sy == r1 && c->sx >= c1) {
      c->sy = nr;
      c->sx = nc + (c->sx - c1);
    } else if (c->sy > r1) {
      c->sy += drows;
    }
  }

  if (c->cy == r1 && c->cx >= c1) {
    c->cy = nr;
    c->cx = nc + (c->cx - c1);
  } else if (c->cy > r1) {
    c->cy += drows;
  }
}

/* Run one editing operation at the current cursor. The single place that maps a
 * multi-cursor edit onto the ordinary editing functions, so that nothing here
 * is a second implementation of an edit. */
static void edit_one(int what, int arg) {
  /* A selection goes first, whatever the operation: typing over a selection
   * replaces it, and so does Enter and so does Tab. Backspace and Delete with a
   * selection delete the selection and stop, which is what one cursor already
   * does. */
  if (CUR.sel_active) {
    selection_delete();
    if (what == MC_DELETE_BACK || what == MC_DELETE_FWD) return;
  }
  switch (what) {
    case MC_INSERT:      editor_insert_char(arg); break;
    case MC_INDENT:      indent_insert_step(); break;
    case MC_NEWLINE:     editor_newline_and_indent(); break;
    case MC_DELETE_BACK: editor_delete_char(); break;
    case MC_DELETE_FWD:  editor_del_char(); break;
    /* editor_paste() takes the selection itself, so the call above has already
     * happened by the time this runs -- harmless, because the second one finds
     * nothing selected. */
    case MC_PASTE:       editor_paste(); break;
    default: break;
  }
}

/* Apply one editing operation at every cursor, as one undo step.
 *
 * With a single cursor this is the operation itself and nothing else, so the
 * ordinary editing path is not made to pay for a feature it is not using. */
void cursors_edit(int what, int arg) {
  int i, keep;

  if (V.ncursors <= 1) {
    edit_one(what, arg);
    return;
  }

  cursors_dedup();
  keep = V.primary;
  undo_group_begin();

  for (i = 0; i < V.ncursors; i++) {
    int r1, c1, rows0, len0, pcy, pcx;

    V.primary = i;
    range_end(what, &r1, &c1);

    /* Enough of the before picture to tell an edit from a no-op: Backspace at
     * the top of the buffer and Delete at the end of it change nothing, and
     * shifting the other cursors for them would move text that never moved. */
    rows0 = row_count();
    len0 = (CUR.cy < row_count()) ? (*row_at(CUR.cy)).size : -1;
    pcy = CUR.cy;
    pcx = CUR.cx;

    /* Keep each cursor's insertion its own undo record. Two cursors never land
     * close enough for editor_insert_char() to coalesce across them -- the only
     * positions that would are the ones cursors_dedup() has already removed --
     * but the invariant is worth stating rather than depending on. */
    B.undo.locked = 1;
    edit_one(what, arg);

    if (row_count() != rows0 || CUR.cy != pcy || CUR.cx != pcx ||
        (pcy < row_count() && (*row_at(pcy)).size != len0)) {
      int j;
      for (j = i + 1; j < V.ncursors; j++)
        cursor_shift(&V.cursors[j], r1, c1, CUR.cy, CUR.cx);
    }
  }

  undo_group_end();
  V.primary = (keep < V.ncursors) ? keep : 0;
  cursors_dedup();
}

/* ---- drawing the cursors the terminal cannot ------------------------------ */

/* Mark every cursor but the primary onto a row that has just been drawn.
 *
 * A second pass over the cells rather than a branch inside the draw loop. That
 * loop runs per character, on every row of every window, every frame; multi-
 * cursor is both rare and purely additive, so it re-roles the handful of cells
 * it owns afterwards and leaves the hot path exactly as it was. With one cursor
 * this returns before doing anything, which is why no golden screen of
 * single-cursor molly moved.
 *
 * The primary is skipped because it has the real thing: refresh_screen() puts
 * the terminal's own cursor on it, and drawing a block there as well would show
 * two cursors where there is one.
 *
 * Selections are marked here for every cursor including the primary, whose own
 * span the draw loop has already coloured -- the two agree, so re-roling those
 * cells changes nothing, and special-casing it would be a second copy of the
 * same arithmetic to keep in step. */
void cursors_mark_row(int y, int filerow, const struct drow *d) {
  int i;
  int gw = V.gutter_w;
  int skip = d->rcol0 + V.coloff;
  int text_cols = TEXT_COLS();
  erow *row;

  if (V.ncursors < 2 || filerow >= row_count()) return;
  if (d->rcols < text_cols) text_cols = d->rcols;
  row = row_at(filerow);

  for (i = 0; i < V.ncursors; i++) {
    const struct cursor *c = &V.cursors[i];
    int s0, s1;

    selection_row_rx_of(c, filerow, &s0, &s1);
    if (s1 > s0) {
      int a = s0 - skip, b = s1 - skip;
      if (a < 0) a = 0;
      if (b > text_cols) b = text_cols;
      if (b > a) grid_shade(V.screenx + gw + a, y, b - a, 1, ROLE_SELECTION);
    }

    if (i == V.primary || c->cy != filerow) continue;
    {
      int rx = editor_row_cx_to_rx(row, c->cx) - skip;
      /* A cursor resting one column past the last character still gets a cell,
       * which is what makes a cursor at the end of a line visible at all. */
      if (rx >= 0 && rx < text_cols)
        grid_shade(V.screenx + gw + rx, y, 1, 1, ROLE_CURSOR);
    }
  }
}

/* ---- the commands ---------------------------------------------------------- */

/* Is this byte part of a word, for the purpose of "the word under the cursor"?
 * The same answer syntax.c gives its keyword matcher, and for the same reason:
 * a word is what an identifier is. */
static int cursor_word_char(unsigned char c) {
  return isalnum(c) || c == '_';
}

/* The word under (or immediately behind) the cursor, as [*s, *e) of its row.
 * Returns 0 when there is no word there, which is what makes Ctrl-D on a blank
 * line say so rather than select nothing. */
static int word_under(int cy, int cx, int *s, int *e) {
  erow *row;
  int a, b;
  if (cy >= row_count()) return 0;
  row = row_at(cy);
  if (row->size == 0) return 0;

  /* A cursor resting just past a word counts as being on it, which is what
   * happens after typing one. */
  a = cx;
  if (a >= row->size) a = row->size - 1;
  if (!cursor_word_char((unsigned char)row->chars[a])) {
    if (a > 0 && cursor_word_char((unsigned char)row->chars[a - 1])) a--;
    else return 0;
  }
  b = a;
  while (a > 0 && cursor_word_char((unsigned char)row->chars[a - 1])) a--;
  while (b < row->size && cursor_word_char((unsigned char)row->chars[b])) b++;
  *s = a;
  *e = b;
  return 1;
}

/* The next literal occurrence of `needle` after the one at (oy, ox), wrapping
 * to the top of the buffer. Returns 0 when there is no other one.
 *
 * Literal and case-sensitive, and deliberately not routed through search.c:
 * Ctrl-D would otherwise change meaning with the Find dialog's checkboxes, so
 * turning on regular expressions would make a word with a dot in it stop
 * matching itself. What Ctrl-D looks for is the text that is already selected,
 * which is never a pattern.
 *
 * The sweep stops by recognising the match it came from rather than by counting
 * rows: a word that occurs once would otherwise be found again on the wrap and
 * handed back as its own next occurrence. Scanning left to right means any
 * other match on the starting row is seen on the first pass, so arriving back
 * at (oy, ox) really does mean there is nothing else. */
static int next_occurrence(const char *needle, int nlen, int oy, int ox,
                           int *ry, int *rx) {
  int rows = row_count();
  int k, y = oy, x = ox + 1;
  if (rows == 0 || nlen <= 0) return 0;

  for (k = 0; k <= rows; k++) {
    erow *row;
    int i;
    if (y >= rows) { y = 0; x = 0; }
    row = row_at(y);
    for (i = x; i + nlen <= row->size; i++) {
      if (memcmp(row->chars + i, needle, (size_t)nlen) != 0) continue;
      if (y == oy && i == ox) return 0;   /* back where it started */
      *ry = y;
      *rx = i;
      return 1;
    }
    y++;
    x = 0;
  }
  return 0;
}

/* Ctrl-D. The first press selects the word under the cursor; each one after that
 * puts another cursor on the next occurrence of what is already selected.
 *
 * Two presses to get two cursors is how every editor that has this behaves, and
 * it is what makes the gesture safe: the first press shows you what is about to
 * be matched before anything is multiplied. */
int cursors_add_next(void) {
  struct cursor *last;
  int ax, ay, bx, by, y, x, nlen;
  char needle[512];

  if (row_count() == 0) return 0;

  /* Nothing selected: select the word and stop. */
  if (!CUR.sel_active) {
    int s, e;
    if (!word_under(CUR.cy, CUR.cx, &s, &e)) {
      editor_set_status_message("No word at the cursor");
      return 0;
    }
    CUR.sy = CUR.cy;
    CUR.sx = s;
    CUR.cx = e;
    CUR.sel_active = 1;
    return 1;
  }

  /* Search from the last cursor in document order, so repeated presses walk
   * down the file rather than all finding the same next match. */
  cursors_sort();
  last = &V.cursors[V.ncursors - 1];
  if (!selection_span_of(last, &ax, &ay, &bx, &by)) return 0;
  if (ay != by) {
    editor_set_status_message("Add cursor needs a selection on one line");
    return 0;
  }
  nlen = bx - ax;
  if (nlen <= 0) return 0;
  if (nlen >= (int)sizeof(needle)) {
    editor_set_status_message("Selection is too long to match");
    return 0;
  }
  memcpy(needle, (*row_at(ay)).chars + ax, (size_t)nlen);

  if (!next_occurrence(needle, nlen, ay, ax, &y, &x)) {
    editor_set_status_message("No more occurrences");
    return 0;
  }
  if (!cursors_add_sel(y, x, x + nlen)) {
    editor_set_status_message("No room for another cursor");
    return 0;
  }
  editor_set_status_message("%d cursors", V.ncursors);
  return 1;
}

/* Ctrl-Up and Ctrl-Down: another cursor on the row above or below, in the same
 * column. The column is the *display* column, so a cursor added below a tab
 * lands under where the cursor looks rather than under its byte offset. */
int cursors_add_vertical(int down) {
  const struct cursor *from;
  int cy, cx, rx;

  if (V.ncursors == 0) return 0;
  cursors_sort();
  /* Grow away from the block: downwards from its last cursor, upwards from its
   * first, so holding the chord walks instead of toggling one row. */
  from = down ? &V.cursors[V.ncursors - 1] : &V.cursors[0];
  cy = from->cy + (down ? 1 : -1);
  if (cy < 0 || cy > row_count()) return 0;

  rx = (from->cy < row_count())
      ? editor_row_cx_to_rx(row_at(from->cy), from->cx) : 0;
  cx = (cy < row_count()) ? editor_row_rx_to_cx(row_at(cy), rx) : 0;

  if (!cursors_add(cy, cx)) return 0;
  editor_set_status_message("%d cursors", V.ncursors);
  return 1;
}

/* ---- moving all of them --------------------------------------------------- */

/* Where each cursor was before a movement, so Shift can seed each one's own
 * selection anchor. File scope rather than a local array because there can be
 * CURSORS_MAX of them and movement is not reentrant. */
static struct { int cx, cy; } pre[CURSORS_MAX];
static int npre;

static void pre_record(void) {
  int i;
  npre = V.ncursors;
  for (i = 0; i < npre; i++) {
    pre[i].cx = V.cursors[i].cx;
    pre[i].cy = V.cursors[i].cy;
  }
}

/* Start a selection at each cursor's own starting point, for a movement made
 * with Shift held.
 *
 * The dispatcher does this for the primary, and can only do it for the primary:
 * it remembers one position, because until now there was one cursor whose
 * position could matter. With several, each needs an anchor where *it* was --
 * otherwise Shift-Right with five cursors highlights one character in one
 * place. Having seeded the primary here as well, the dispatcher's own attempt
 * finds a selection already active and leaves it alone. */
static void pre_seed_anchors(void) {
  int i;
  if (!input_last_key_had_shift()) return;
  for (i = 0; i < npre && i < V.ncursors; i++) {
    if (V.cursors[i].sel_active) continue;
    V.cursors[i].sel_active = 1;
    V.cursors[i].sx = pre[i].cx;
    V.cursors[i].sy = pre[i].cy;
  }
}

/* Move every cursor, then merge the ones that landed together.
 *
 * All of them rather than only the primary, because the block is the point: an
 * arrow key with five cursors down a column should keep five cursors down a
 * column. Movement changes no text, so none of the position fixing above is
 * needed -- but two cursors can walk into each other at the start of a line or
 * the end of the buffer, and two cursors in one place would then type twice
 * there, so the merge is not cosmetic.
 *
 * With one cursor this is the plain movement and nothing else, which is what
 * keeps every existing selection and scrolling test saying what it said. */
void cursors_move_all(int key) {
  int i, keep;
  if (V.ncursors <= 1) {
    editor_move_cursor(key);
    return;
  }
  keep = V.primary;
  pre_record();
  for (i = 0; i < V.ncursors; i++) {
    V.primary = i;
    editor_move_cursor(key);
  }
  V.primary = (keep < V.ncursors) ? keep : 0;
  pre_seed_anchors();
  cursors_dedup();
}

/* Every cursor to the start or the end of the row it is on. */
void cursors_line_edge(int to_end) {
  int i, keep;
  if (V.ncursors <= 1) {
    CUR.cx = (to_end && CUR.cy < row_count()) ? (*row_at(CUR.cy)).size : 0;
    return;
  }
  keep = V.primary;
  pre_record();
  for (i = 0; i < V.ncursors; i++) {
    struct cursor *c = &V.cursors[i];
    c->cx = (to_end && c->cy < row_count()) ? (*row_at(c->cy)).size : 0;
  }
  V.primary = (keep < V.ncursors) ? keep : 0;
  pre_seed_anchors();
  cursors_dedup();
}
