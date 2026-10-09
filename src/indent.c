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
