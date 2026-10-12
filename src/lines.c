/* lines.c -- operations on whole lines.
 *
 * Duplicate, move up and down, join, sort, trim, change case, comment and
 * uncomment: the line-shaped half of the roadmap's "cheap and clearly worth
 * it" list.
 *
 * Every one of them is the same shape, and is built that way on purpose: work
 * out the rows it acts on, build what those rows should say afterwards, and
 * replace the span with it through the selection path -- one undo step, the
 * row splitting and joining that every paste already relies on, and nothing
 * here that touches a row directly. An operation whose result is what was
 * already there changes nothing, so a file sorted twice is not modified twice.
 *
 * The rows are the primary cursor's: its selection's rows, where a selection
 * that ends at the start of a row does not take that row in (the rule indent
 * uses, for the same reason -- selecting three lines by Shift-Down leaves the
 * cursor at the start of a fourth); without a selection, its own row. Other
 * cursors are let go: a line operation at five cursors on overlapping rows has
 * no one right answer. */

#include "molly.h"

/* The rows to act on, [*r0, *r1]. 0 when there are none. */
static int target_rows(int *r0, int *r1) {
  if (row_count() == 0) return 0;
  if (CUR.sel_active) {
    int ax, ay, bx, by;
    if (!selection_span(&ax, &ay, &bx, &by)) return 0;
    *r0 = ay;
    *r1 = (bx == 0 && by > ay) ? by - 1 : by;
  } else {
    *r0 = *r1 = CUR.cy < row_count() ? CUR.cy : row_count() - 1;
  }
  return 1;
}

/* Rows [r0, r1] as one string, joined by newlines, with no newline at the end. */
static void rows_text(int r0, int r1, struct abuf *ab) {
  int i;
  for (i = r0; i <= r1; i++) {
    erow *r = row_at(i);
    if (i > r0) ab_append(ab, "\n", 1);
    ab_append(ab, r->chars, r->size);
  }
}

/* Replace rows [r0, r1] with `text`, as one undo step. Nothing at all if it
 * is what is there already. The cursor is left wherever the caller puts it. */
static void replace_rows(int r0, int r1, const char *text, int len) {
  struct abuf now = ABUF_INIT;
  rows_text(r0, r1, &now);
  if (now.len == len && (len == 0 || memcmp(now.b, text, (size_t)len) == 0)) {
    free(now.b);
    return;
  }
  free(now.b);
  cursors_collapse();
  B.undo.locked = 1;
  undo_group_begin();
  CUR.sy = r0;
  CUR.sx = 0;
  CUR.cy = r1;
  CUR.cx = row_at(r1)->size;
  CUR.sel_active = 1;
  editor_insert_text(text, len);
  undo_group_end();
  CUR.sel_active = 0;
}

/* Put the cursor back, and the selection if there was one, `dy` rows from
 * where they were -- the rows having moved by that much. */
static void place(const struct cursor *was, int dy) {
  CUR = *was;
  CUR.cy += dy;
  if (was->sel_active) CUR.sy += dy;
  if (CUR.cy >= row_count()) CUR.cy = row_count() - 1;
  if (CUR.cy < 0) CUR.cy = 0;
  if (CUR.cx > row_at(CUR.cy)->size) CUR.cx = row_at(CUR.cy)->size;
  if (CUR.sel_active) {
    if (CUR.sy >= row_count()) CUR.sy = row_count() - 1;
    if (CUR.sy < 0) CUR.sy = 0;
    if (CUR.sx > row_at(CUR.sy)->size) CUR.sx = row_at(CUR.sy)->size;
  }
}

/* ---- duplicate and move ---------------------------------------------------- */

/* The rows again, below themselves, with the cursor on the copy -- so a second
 * press makes a third, and typing edits the new one. */
void lines_duplicate(void) {
  struct cursor was = CUR;
  struct abuf ab = ABUF_INIT, both = ABUF_INIT;
  int r0, r1;
  if (!target_rows(&r0, &r1)) return;
  rows_text(r0, r1, &ab);
  ab_append(&both, ab.b, ab.len);
  ab_append(&both, "\n", 1);
  ab_append(&both, ab.b, ab.len);
  replace_rows(r0, r1, both.b, both.len);
  place(&was, r1 - r0 + 1);
  free(ab.b);
  free(both.b);
}

/* The rows one up (dir < 0) or down, past the row on that side, which goes to
 * the other side of them. At the top or bottom of the file, nothing. */
void lines_move(int dir) {
  struct cursor was = CUR;
  struct abuf ab = ABUF_INIT;
  int r0, r1;
  if (!target_rows(&r0, &r1)) return;
  if (dir < 0 ? r0 == 0 : r1 >= row_count() - 1) return;
  if (dir < 0) {
    rows_text(r0, r1, &ab);
    ab_append(&ab, "\n", 1);
    ab_append(&ab, row_at(r0 - 1)->chars, row_at(r0 - 1)->size);
    replace_rows(r0 - 1, r1, ab.b, ab.len);
  } else {
    ab_append(&ab, row_at(r1 + 1)->chars, row_at(r1 + 1)->size);
    ab_append(&ab, "\n", 1);
    rows_text(r0, r1, &ab);
    replace_rows(r0, r1 + 1, ab.b, ab.len);
  }
  place(&was, dir < 0 ? -1 : 1);
  free(ab.b);
}

/* ---- join ------------------------------------------------------------------ */

/* The row and the next as one, or every selected row as one: the leading
 * blanks of each joined row become a single space, and there is no space
 * before a row that was blank. The cursor goes to the last join, on the
 * space that made it, as vi's J leaves it. */
void lines_join(void) {
  struct abuf ab = ABUF_INIT;
  int r0, r1, i, at = 0;
  if (!target_rows(&r0, &r1)) return;
  if (r1 == r0) r1 = r0 + 1;
  if (r1 >= row_count()) return;
  ab_append(&ab, row_at(r0)->chars, row_at(r0)->size);
  for (i = r0 + 1; i <= r1; i++) {
    erow *r = row_at(i);
    int k = 0;
    while (k < r->size && (r->chars[k] == ' ' || r->chars[k] == '\t')) k++;
    while (ab.len > 0 && (ab.b[ab.len - 1] == ' ' || ab.b[ab.len - 1] == '\t')) ab.len--;
    at = ab.len;
    if (k < r->size && ab.len > 0) ab_append(&ab, " ", 1);
    ab_append(&ab, r->chars + k, r->size - k);
  }
  replace_rows(r0, r1, ab.b, ab.len);
  cursors_collapse();
  CUR.sel_active = 0;
  CUR.cy = r0;
  CUR.cx = at;
  free(ab.b);
}

/* ---- sort, trim, case ------------------------------------------------------ */

static int row_cmp(const void *a, const void *b) {
  const erow *p = *(erow *const *)a, *q = *(erow *const *)b;
  int n = p->size < q->size ? p->size : q->size;
  int c = memcmp(p->chars, q->chars, (size_t)n);
  if (c != 0) return c;
  return p->size - q->size;
}

/* The selected rows in byte order: which is the order `sort` gives with
 * LC_ALL=C, and the only one that does not depend on the machine. Without a
 * selection of two rows or more there is nothing to sort, and it says so. */
void lines_sort(void) {
  struct cursor was = CUR;
  struct abuf ab = ABUF_INIT;
  erow **order;
  int r0, r1, i;
  if (!target_rows(&r0, &r1) || r1 == r0) {
    editor_set_status_message("Select the lines to sort.");
    return;
  }
  order = xmalloc(sizeof(*order) * (size_t)(r1 - r0 + 1));
  for (i = r0; i <= r1; i++) order[i - r0] = row_at(i);
  qsort(order, (size_t)(r1 - r0 + 1), sizeof(*order), row_cmp);
  for (i = 0; i <= r1 - r0; i++) {
    if (i > 0) ab_append(&ab, "\n", 1);
    ab_append(&ab, order[i]->chars, order[i]->size);
  }
  free(order);
  replace_rows(r0, r1, ab.b, ab.len);
  place(&was, 0);
  free(ab.b);
  editor_set_status_message("Sorted %d lines.", r1 - r0 + 1);
}

/* Blanks at the ends of rows: the selected ones, or every one in the file. */
void lines_trim(void) {
  struct cursor was = CUR;
  struct abuf ab = ABUF_INIT;
  int r0, r1, i, n = 0;
  if (row_count() == 0) return;
  if (CUR.sel_active) {
    if (!target_rows(&r0, &r1)) return;
  } else {
    r0 = 0;
    r1 = row_count() - 1;
  }
  for (i = r0; i <= r1; i++) {
    erow *r = row_at(i);
    int len = r->size;
    while (len > 0 && (r->chars[len - 1] == ' ' || r->chars[len - 1] == '\t')) len--;
    if (len != r->size) n++;
    if (i > r0) ab_append(&ab, "\n", 1);
    ab_append(&ab, r->chars, len);
  }
  replace_rows(r0, r1, ab.b, ab.len);
  place(&was, 0);
  free(ab.b);
  editor_set_status_message(n ? "Trimmed %d line%s." : "Nothing to trim.", n,
                            n == 1 ? "" : "s");
}

/* The selection, or the word under the cursor, in upper or lower case. ASCII
 * letters only: changing the case of anything else is a table molly does not
 * carry, and a byte-length change would be one too. */
void lines_case(int upper) {
  struct cursor was = CUR;
  int ay, ax, by, bx, y;
  if (row_count() == 0) return;
  if (CUR.sel_active) {
    if (!selection_span(&ax, &ay, &bx, &by)) return;
  } else {
    erow *r = row_at(CUR.cy < row_count() ? CUR.cy : row_count() - 1);
    ay = by = CUR.cy;
    ax = bx = CUR.cx;
    while (ax > 0 && (isalnum((unsigned char)r->chars[ax - 1]) || r->chars[ax - 1] == '_')) ax--;
    while (bx < r->size && (isalnum((unsigned char)r->chars[bx]) || r->chars[bx] == '_')) bx++;
    if (ax == bx) return;
  }
  {
    struct abuf ab = ABUF_INIT;
    int changed = 0;
    for (y = ay; y <= by; y++) {
      erow *r = row_at(y);
      int i;
      if (y > ay) ab_append(&ab, "\n", 1);
      for (i = 0; i < r->size; i++) {
        char c = r->chars[i];
        int in = (y > ay || i >= ax) && (y < by || i < bx);
        if (in && (unsigned char)c < 0x80) {
          char d = (char)(upper ? toupper((unsigned char)c) : tolower((unsigned char)c));
          if (d != c) changed = 1;
          c = d;
        }
        ab_append(&ab, &c, 1);
      }
    }
    if (changed) replace_rows(ay, by, ab.b, ab.len);
    free(ab.b);
  }
  place(&was, 0);
}

/* ---- comments -------------------------------------------------------------- */

/* Comment the rows out with the filetype's line comment, or back in. In if
 * every row that is not blank already starts with it; out otherwise, with the
 * marker at the shallowest indentation among them so the block stays lined
 * up, and a space after it. Blank rows are left alone either way. */
void lines_comment(void) {
  struct cursor was = CUR;
  const char *mark = syntax_line_comment(B.syn);
  struct abuf ab = ABUF_INIT;
  int r0, r1, i, ml, all = 1, any = 0, depth = INT_MAX, dx = 0;

  if (mark == NULL || mark[0] == '\0') {
    editor_set_status_message("No line comment is known for this file.");
    return;
  }
  if (!target_rows(&r0, &r1)) return;
  ml = (int)strlen(mark);
  for (i = r0; i <= r1; i++) {
    erow *r = row_at(i);
    int k = 0;
    while (k < r->size && (r->chars[k] == ' ' || r->chars[k] == '\t')) k++;
    if (k == r->size) continue;
    any = 1;
    if (k < depth) depth = k;
    if (r->size - k < ml || memcmp(r->chars + k, mark, (size_t)ml) != 0) all = 0;
  }
  if (!any) return;
  for (i = r0; i <= r1; i++) {
    erow *r = row_at(i);
    int k = 0;
    if (i > r0) ab_append(&ab, "\n", 1);
    while (k < r->size && (r->chars[k] == ' ' || r->chars[k] == '\t')) k++;
    if (k == r->size) {
      ab_append(&ab, r->chars, r->size);
      continue;
    }
    if (all) {
      int skip = ml + (k + ml < r->size && r->chars[k + ml] == ' ');
      ab_append(&ab, r->chars, k);
      ab_append(&ab, r->chars + k + skip, r->size - k - skip);
      if (i == was.cy) dx = -skip;
    } else {
      ab_append(&ab, r->chars, depth);
      ab_append(&ab, mark, ml);
      ab_append(&ab, " ", 1);
      ab_append(&ab, r->chars + depth, r->size - depth);
      if (i == was.cy) dx = ml + 1;
    }
  }
  replace_rows(r0, r1, ab.b, ab.len);
  free(ab.b);
  place(&was, 0);
  if (!was.sel_active) {
    CUR.cx += dx;
    if (CUR.cx < 0) CUR.cx = 0;
    if (CUR.cx > row_at(CUR.cy)->size) CUR.cx = row_at(CUR.cy)->size;
  }
}
