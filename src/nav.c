/* nav.c -- getting back to where you were: the position history, bookmarks,
 * and the bracket that matches this one.
 *
 * The history is a browser's: a stack behind and a stack ahead. A jump --
 * F12, Ctrl-G, a search, a result, a problem, a bookmark -- pushes where it
 * left from onto the one behind and empties the one ahead; Alt-Left goes
 * back, and Alt-Right forward again until the next jump. Places are kept by
 * buffer id, not pointer, so a buffer closed in between is stepped over
 * rather than followed into freed memory -- the rule F12's own back stack had,
 * which this replaces.
 *
 * What is recorded is a jump and not a movement: arrows, PgDn and F3's next
 * match move the cursor somewhere you can see from where you were, and a
 * history of every one would be a history nobody could step back through.
 *
 * Bookmarks are ten per buffer, kept on the buffer and moved with their text
 * when rows are inserted or deleted above them, as diagnostics are. */

#include "molly.h"

#define NAV_MAX 64

struct place { int buf_id, cy, cx; };

static struct place back[NAV_MAX], ahead[NAV_MAX];
static int nback, nahead;

static void push(struct place *st, int *n, struct place p) {
  if (*n == NAV_MAX) {
    memmove(&st[0], &st[1], sizeof(st[0]) * (NAV_MAX - 1));
    (*n)--;
  }
  st[(*n)++] = p;
}

static struct place here(void) {
  struct place p;
  p.buf_id = B.id;
  p.cy = CUR.cy;
  p.cx = CUR.cx;
  return p;
}

/* Record a place a jump is leaving. The same place twice in a row is once. */
void nav_record(int buf_id, int cy, int cx) {
  struct place p;
  p.buf_id = buf_id;
  p.cy = cy;
  p.cx = cx;
  if (nback > 0 && back[nback - 1].buf_id == buf_id && back[nback - 1].cy == cy &&
      back[nback - 1].cx == cx)
    return;
  push(back, &nback, p);
  nahead = 0;
}

/* About to jump from where the cursor is. */
void nav_push(void) { nav_record(B.id, CUR.cy, CUR.cx); }

static int go(struct place p) {
  int i;
  for (i = 0; i < buf_count(); i++) {
    if (buf_at(i)->id != p.buf_id) continue;
    if (V.buf != buf_at(i)) buf_show(i);
    cursor_place(p.cy, p.cx);
    return 1;
  }
  return 0;
}

int nav_can_back(void) { return nback > 0; }
int nav_can_forward(void) { return nahead > 0; }

/* Alt-Left: to the last place a jump left, putting this one ahead. */
int nav_back(void) {
  while (nback > 0) {
    struct place p = back[--nback], now = here();
    if (go(p)) {
      push(ahead, &nahead, now);
      return 1;
    }
  }
  editor_set_status_message("Nowhere to go back to.");
  return 0;
}

/* Alt-Right: undo an Alt-Left. */
int nav_forward(void) {
  while (nahead > 0) {
    struct place p = ahead[--nahead], now = here();
    if (go(p)) {
      push(back, &nback, now);
      return 1;
    }
  }
  editor_set_status_message("Nowhere to go forward to.");
  return 0;
}

/* ---- bookmarks ------------------------------------------------------------ */

/* Alt-J and a digit: this place, as bookmark n of this buffer. */
void nav_mark_set(int n) {
  if (n < 0 || n > 9) return;
  B.marks[n].y = CUR.cy;
  B.marks[n].x = CUR.cx;
  B.marks[n].set = 1;
  editor_set_status_message("Bookmark %d set at line %d. Alt-%d comes back.", n,
                            CUR.cy + 1, n);
}

/* Alt-n: to bookmark n, as a jump. */
void nav_mark_go(int n) {
  struct place p;
  if (n < 0 || n > 9 || !B.marks[n].set) {
    editor_set_status_message("No bookmark %d in this file. Alt-J %d sets one.", n, n);
    return;
  }
  nav_push();
  p.buf_id = B.id;
  p.cy = B.marks[n].y;
  p.cx = B.marks[n].x;
  go(p);
  editor_set_status_message("Bookmark %d, line %d.", n, CUR.cy + 1);
}

/* Rows were inserted (delta > 0) or one deleted at `at`: the bookmarks below
 * move with their text. One on a deleted row stays at that row number, which
 * is now the line after -- a bookmark that vanished because its line was
 * edited away would be harder to understand than one that moved a line. */
void nav_rows_moved(struct buffer *b, int at, int delta) {
  int i;
  if (b == NULL) return;
  for (i = 0; i < 10; i++) {
    if (!b->marks[i].set) continue;
    if (delta > 0 && b->marks[i].y >= at) b->marks[i].y += delta;
    else if (delta < 0 && b->marks[i].y > at) b->marks[i].y--;
  }
}

/* ---- the matching bracket -------------------------------------------------- */

static int partner(char c, char *other, int *dir) {
  switch (c) {
  case '(': *other = ')'; *dir = 1; return 1;
  case '[': *other = ']'; *dir = 1; return 1;
  case '{': *other = '}'; *dir = 1; return 1;
  case ')': *other = '('; *dir = -1; return 1;
  case ']': *other = '['; *dir = -1; return 1;
  case '}': *other = '{'; *dir = -1; return 1;
  }
  return 0;
}

/* The bracket matching the one at (cy, cx) -- or, failing that, the one just
 * before it, which is where the cursor is after typing one. 1 with the place
 * in *my, *mx; 0 when the cursor is not at a bracket or it has no match.
 * Brackets are counted, not parsed: one inside a string or a comment counts
 * the same as one in code. */
int nav_match(int cy, int cx, int *my, int *mx) {
  erow *r;
  char c = 0, other;
  int dir, depth = 0, y, x;
  if (cy >= row_count()) return 0;
  r = row_at(cy);
  if (cx < r->size && partner(r->chars[cx], &other, &dir)) {
    c = r->chars[cx];
  } else if (cx > 0 && cx <= r->size && partner(r->chars[cx - 1], &other, &dir)) {
    c = r->chars[--cx];
  } else {
    return 0;
  }
  y = cy;
  x = cx;
  for (;;) {
    char ch;
    if (dir > 0) {
      x++;
      while (x >= row_at(y)->size) {
        if (++y >= row_count()) return 0;
        x = 0;
        if (row_at(y)->size > 0) break;
      }
    } else {
      x--;
      while (x < 0) {
        if (--y < 0) return 0;
        x = row_at(y)->size - 1;
      }
    }
    ch = row_at(y)->chars[x];
    if (ch == c) depth++;
    else if (ch == other) {
      if (depth == 0) {
        *my = y;
        *mx = x;
        return 1;
      }
      depth--;
    }
  }
}

/* Ctrl-]: to the matching bracket. */
void nav_match_jump(void) {
  int y, x;
  if (!nav_match(CUR.cy, CUR.cx, &y, &x)) {
    editor_set_status_message("No bracket here to match.");
    return;
  }
  cursors_collapse();
  CUR.sel_active = 0;
  CUR.cy = y;
  CUR.cx = x;
}
