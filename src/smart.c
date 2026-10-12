/* smart.c -- the small kindnesses: smart Home, closing brackets and quotes,
 * and wrapping a selection in them.
 *
 * Each is a setting, because each annoys somebody (the roadmap's words), and
 * they default differently for a reason. Smart Home is on: it changes where a
 * key takes the cursor and nothing else, and Home pressed twice still reaches
 * column zero. Auto-close is off: it changes what typing a bracket puts in the
 * file, which is the line between help and surprise for the people it
 * annoys, and surround comes with it -- a selection with auto-close off is
 * replaced by what is typed, as it always was.
 *
 * Nothing here edits a row directly. A pair is two ordinary inserts, a
 * surround two more, a step over a closer a cursor move, so undo and the
 * multi-cursor fixups are the ones every keystroke already has. */

#include "molly.h"

static int smart_home = 1;
static int auto_close = 0;

int smart_home_on(void) { return smart_home; }
void smart_set_home(int on) { smart_home = on ? 1 : 0; }
int smart_auto_close(void) { return auto_close; }
void smart_set_auto_close(int on) { auto_close = on ? 1 : 0; }

/* Home: to the first character that is not a blank, or -- from there -- to
 * column zero. VS Code's, and Visual Studio's before it: the first press
 * lands where the code starts, the second where the line does. */
int smart_home_target(const erow *r, int cx) {
  int k = 0;
  if (!smart_home || r == NULL) return 0;
  while (k < r->size && (r->chars[k] == ' ' || r->chars[k] == '\t')) k++;
  return cx == k ? 0 : k;
}

static char closer_of(int c) {
  switch (c) {
  case '(': return ')';
  case '[': return ']';
  case '{': return '}';
  case '"': return '"';
  case '\'': return '\'';
  case '`': return '`';
  }
  return 0;
}

static int is_quote(int c) { return c == '"' || c == '\'' || c == '`'; }

static int word_byte(int c) {
  return c > 0 && (isalnum(c) || c == '_' || c >= 0x80);
}

/* Typing `c` at the cursor, with auto-close on and nothing selected.
 * Returns 1 when it has been dealt with, with *adjust the columns the cursor
 * should move once the edit has been accounted for at every cursor: -1 to
 * sit inside a pair just inserted, +1 to step over a closer that was
 * already there (which inserts nothing at all). 0 means type it as usual. */
int smart_insert(int c, int *adjust) {
  erow *r;
  int next, prev;
  char close;
  *adjust = 0;
  if (!auto_close || c <= 0 || c > 127 || CUR.cy >= row_count()) return 0;
  r = row_at(CUR.cy);
  next = CUR.cx < r->size ? (unsigned char)r->chars[CUR.cx] : 0;
  prev = CUR.cx > 0 ? (unsigned char)r->chars[CUR.cx - 1] : 0;

  /* A closer typed in front of its twin steps over it, so typing `foo()` with
   * auto-close on gives `foo()` and not `foo())`. */
  if ((c == ')' || c == ']' || c == '}' || is_quote(c)) && next == c) {
    *adjust = 1;
    return 1;
  }
  close = closer_of(c);
  if (close == 0) return 0;
  /* Only where a pair makes sense: before a blank, the end of the line or a
   * closer -- not in front of a word, where a lone bracket is what is meant.
   * A quote also not right after a word, which is an apostrophe: it's. */
  if (next != 0 && next != ' ' && next != '\t' && next != ')' && next != ']' &&
      next != '}' && next != ',' && next != ';')
    return 0;
  if (is_quote(c) && word_byte(prev)) return 0;
  editor_insert_char(c);
  editor_insert_char(close);
  *adjust = -1;
  return 1;
}

/* Backspace between an empty pair takes both halves. 1 if it did. */
int smart_backspace(void) {
  erow *r;
  if (!auto_close || CUR.cy >= row_count() || CUR.cx == 0) return 0;
  r = row_at(CUR.cy);
  if (CUR.cx >= r->size) return 0;
  if (closer_of((unsigned char)r->chars[CUR.cx - 1]) == 0 ||
      closer_of((unsigned char)r->chars[CUR.cx - 1]) != r->chars[CUR.cx])
    return 0;
  editor_del_char();
  editor_delete_char();
  return 1;
}

/* An opener typed over a selection wraps it, and the selection stays on what
 * it wrapped -- so `(` then `"` gives ("text"). 1 if it did. */
int smart_surround(int c) {
  int ax, ay, bx, by;
  char close = closer_of(c);
  if (!auto_close || close == 0 || !CUR.sel_active) return 0;
  if (!selection_span(&ax, &ay, &bx, &by)) return 0;
  CUR.sel_active = 0;
  undo_group_begin();
  CUR.cy = by;
  CUR.cx = bx;
  editor_insert_char(close);
  CUR.cy = ay;
  CUR.cx = ax;
  editor_insert_char(c);
  undo_group_end();
  CUR.sy = ay;
  CUR.sx = ax + 1;
  CUR.cy = by;
  CUR.cx = bx + (by == ay ? 1 : 0);
  CUR.sel_active = 1;
  return 1;
}
