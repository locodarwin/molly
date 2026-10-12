/* fold.c -- hiding a block of lines behind the one that opens it.
 *
 * Folding is indentation, not syntax: a fold is a row and the rows after it
 * that are indented further, which is the same answer for C, Python, YAML and
 * a Markdown list without a grammar for any of them -- the roadmap's
 * "indent-based folding only". Blank rows inside the block belong to it.
 *
 * It is done where soft wrap is done, in the map between display rows and
 * file rows (wrap.c): a folded row occupies no display rows, so the scroll,
 * the drawing, the mouse and PgDn all step over it without being told. What
 * that map cannot do on its own is keep the cursor out of a fold, and that is
 * fold_fix_cursor(): an arrow that steps into one steps past it, and anything
 * else that lands inside one -- a search, Go to line, a definition -- opens
 * it, because that is where the user was sent.
 *
 * Folds are the buffer's, so two windows on one file fold together, and move
 * with their text when rows are inserted or deleted above them. */

#include "molly.h"

static int indent_of(int y) {
  erow *r = row_at(y);
  int k = 0, col = 0, tw = tab_width();
  while (k < r->size && (r->chars[k] == ' ' || r->chars[k] == '\t')) {
    col = r->chars[k] == '\t' ? (col / tw + 1) * tw : col + 1;
    k++;
  }
  return k == r->size ? -1 : col;   /* -1: a blank row */
}

/* The last row of the block row `y` opens: the rows after it indented
 * further, through any blank rows inside them. `y` itself when there are
 * none. */
static int block_end(int y) {
  int base = indent_of(y), i, end = y;
  if (base < 0) return y;
  for (i = y + 1; i < row_count(); i++) {
    int ind = indent_of(i);
    if (ind < 0) continue;
    if (ind <= base) break;
    end = i;
  }
  return end;
}

int fold_hidden(const struct buffer *b, int y) {
  int i;
  for (i = 0; i < b->nfolds; i++)
    if (y > b->folds[i].start && y <= b->folds[i].end) return 1;
  return 0;
}

/* The fold row `y` heads, or -1. */
int fold_at(const struct buffer *b, int y) {
  int i;
  for (i = 0; i < b->nfolds; i++)
    if (b->folds[i].start == y) return i;
  return -1;
}

int fold_size(const struct buffer *b, int i) {
  return b->folds[i].end - b->folds[i].start;
}

static void fold_remove(int i) {
  memmove(&B.folds[i], &B.folds[i + 1], sizeof(B.folds[0]) * (size_t)(B.nfolds - i - 1));
  B.nfolds--;
  B.wrapgen++;
}

static void fold_add(int start, int end) {
  if (end <= start) return;
  B.folds = xrealloc(B.folds, sizeof(*B.folds) * (size_t)(B.nfolds + 1));
  B.folds[B.nfolds].start = start;
  B.folds[B.nfolds].end = end;
  B.nfolds++;
  B.wrapgen++;
}

/* Alt--: fold the block the cursor is in, or open the fold it is on. The
 * block is the one its row opens, or -- when the row opens none -- the one it
 * is inside, whose first row the cursor then moves to. */
void fold_toggle(void) {
  int y = CUR.cy, f, end;
  if (y >= row_count()) return;
  if ((f = fold_at(V.buf, y)) >= 0) {
    fold_remove(f);
    return;
  }
  end = block_end(y);
  if (end == y) {
    int ind = indent_of(y), i;
    if (ind <= 0 && ind != -1) {
      editor_set_status_message("Nothing to fold here.");
      return;
    }
    for (i = y - 1; i >= 0; i--) {
      int k = indent_of(i);
      if (k >= 0 && (ind < 0 || k < ind)) break;
    }
    if (i < 0 || (end = block_end(i)) <= i || end < y) {
      editor_set_status_message("Nothing to fold here.");
      return;
    }
    y = i;
  }
  fold_add(y, end);
  cursors_collapse();
  CUR.sel_active = 0;
  CUR.cy = y;
  if (CUR.cx > row_at(y)->size) CUR.cx = row_at(y)->size;
}

/* Every outermost block in the file. */
void fold_all(void) {
  int y = 0;
  while (y < row_count()) {
    int end = block_end(y);
    if (end > y && fold_at(V.buf, y) < 0) fold_add(y, end);
    y = end + 1;
  }
  if (fold_hidden(V.buf, CUR.cy)) {
    int i;
    for (i = 0; i < B.nfolds; i++)
      if (CUR.cy > B.folds[i].start && CUR.cy <= B.folds[i].end) CUR.cy = B.folds[i].start;
    CUR.cx = 0;
  }
}

void fold_none(void) {
  free(B.folds);
  B.folds = NULL;
  if (B.nfolds) B.wrapgen++;
  B.nfolds = 0;
}

/* Rows were inserted (delta > 0) or one deleted at `at`. A fold below moves;
 * one with the change inside it grows or shrinks; one whose header row went is
 * gone, since there is nothing left to show it by. */
void fold_rows_moved(struct buffer *b, int at, int delta) {
  int i, o = 0;
  if (b == NULL || b->nfolds == 0) return;
  for (i = 0; i < b->nfolds; i++) {
    struct fold f = b->folds[i];
    if (delta > 0) {
      if (f.start >= at) f.start += delta;
      if (f.end >= at) f.end += delta;
    } else {
      if (f.start == at) continue;
      if (f.start > at) f.start--;
      if (f.end >= at) f.end--;
      if (f.end <= f.start) continue;
    }
    b->folds[o++] = f;
  }
  if (o != b->nfolds) b->wrapgen++;
  b->nfolds = o;
}

/* After a key: no cursor may be inside a fold. One that stepped one row into
 * it steps the rest of the way -- past it going down, onto its header going
 * up -- and one that got there any other way opens it. */
void fold_fix_cursor(int prev_cy) {
  int i, guard = 0;
  if (B.nfolds == 0) return;
  while (fold_hidden(V.buf, CUR.cy) && guard++ < 1000) {
    for (i = 0; i < B.nfolds; i++) {
      struct fold f = B.folds[i];
      if (!(CUR.cy > f.start && CUR.cy <= f.end)) continue;
      if (CUR.cy == prev_cy + 1 && f.end + 1 < row_count()) {
        CUR.cy = f.end + 1;
      } else if (CUR.cy == prev_cy + 1 || CUR.cy == prev_cy - 1) {
        CUR.cy = f.start;
      } else {
        fold_remove(i);
      }
      if (CUR.cy < row_count() && CUR.cx > row_at(CUR.cy)->size)
        CUR.cx = row_at(CUR.cy)->size;
      break;
    }
  }
}
