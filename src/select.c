/* select.c -- Selection and the clipboard, including the OSC 52 push to the system one. */

#include "molly.h"

/* --- selection: an anchor (sx,sy) plus the live cursor (cx,cy). Movement with
 * Shift held extends it (setting the anchor on the first shifted move); any
 * other key drops it. The covered text is highlighted in reverse video by
 * editor_draw_text_row(), and the byte count is surfaced in the status bar. --- */

/* Every cursor's selection, not only the primary's. "Drop the selection" with
 * several cursors means all of them -- after Ctrl-D each one is holding an
 * occurrence, and clearing one of them would leave the rest highlighted with
 * nothing about to replace them. */
void selection_clear(void) {
  int i;
  for (i = 0; i < V.ncursors; i++) V.cursors[i].sel_active = 0;
}

/* Order the anchor and cursor into a top-left / bottom-right pair, clamped to
 * valid rows so the phantom line past the last row never indexes out of
 * bounds. Returns 0 (leaving the outputs undefined) when no selection. */
static int selection_range_of(const struct cursor *c,
                              int *ax, int *ay, int *bx, int *by) {
  if (!c->sel_active) return 0;
  int y0 = c->sy, y1 = c->cy, x0 = c->sx, x1 = c->cx;
  if (y0 > y1 || (y0 == y1 && x0 > x1)) {
    int ty = y0, tx = x0;
    y0 = y1; x0 = x1; y1 = ty; x1 = tx;
  }
  if (y0 < 0) y0 = 0;
  if (y1 > row_count()) y1 = row_count();
  *ay = y0; *ax = x0; *by = y1; *bx = x1;
  /* The phantom line past the last row has no characters, so a selection that
   * runs off the end of the buffer always ends at column 0 of it. */
  if (*by == row_count()) *bx = 0;
  return 1;
}

static int selection_range(int *ax, int *ay, int *bx, int *by) {
  return selection_range_of(&CUR, ax, ay, bx, by);
}

/* The same span for any cursor, which is what cursors.c needs in order to read
 * the text one of the far cursors has selected. */
int selection_span_of(const struct cursor *c,
                      int *ax, int *ay, int *bx, int *by) {
  return selection_range_of(c, ax, ay, bx, by);
}

/* The current cursor's selection in document order, for the one caller outside
 * this file that needs it: cursors.c, which has to know where the text an edit
 * is about to replace *ends* in order to move the cursors after it. Public
 * rather than a second copy of the ordering above, because getting that
 * ordering wrong in two places is exactly the bug it exists to prevent. */
int selection_span(int *ax, int *ay, int *bx, int *by) {
  return selection_range(ax, ay, bx, by);
}

/* Bytes spanned by the selection, counting one per internal line break. Used
 * only for the status-bar count, so whether a line break is "one byte" is a
 * display choice rather than a correctness one. */
int selection_bytes(void) {
  int ax, ay, bx, by;
  if (!selection_range(&ax, &ay, &bx, &by)) return 0;
  if (ay == by) {
    int sz = (ay < row_count()) ? (*row_at(ay)).size : 0;
    if (ax < 0) ax = 0;
    if (bx > sz) bx = sz;
    return (bx > ax) ? bx - ax : 0;
  }
  int total = 0;
  if (ay < row_count()) total += (*row_at(ay)).size - ax;
  int i;
  for (i = ay + 1; i < by && i < row_count(); i++) total += (*row_at(i)).size;
  total += bx;
  total += (by - ay);
  return total;
}

/* The selected display-column range on row 'filerow' as absolute columns
 * [out0, out1); out1 <= out0 when no text on this row is selected. Selection
 * endpoints are byte offsets into row->chars, mapped through
 * editor_row_cx_to_rx() so the highlight lands on the rendered glyphs (tab
 * stops and wide glyphs already accounted for). The whole interior span of a
 * multi-line selection is covered; the top row starts at the anchor and the
 * bottom row ends at the far edge. */
void selection_row_rx_of(const struct cursor *c, int filerow,
                         int *out0, int *out1) {
  *out0 = 0;
  *out1 = 0;
  int ax, ay, bx, by;
  if (!selection_range_of(c, &ax, &ay, &bx, &by)) return;
  if (filerow < ay || filerow > by || filerow >= row_count()) return;
  erow *r = row_at(filerow);
  if (ay == by) {
    if (bx > ax) {
      *out0 = editor_row_cx_to_rx(r, ax);
      *out1 = editor_row_cx_to_rx(r, bx);
    }
  } else if (filerow == ay) {
    *out0 = editor_row_cx_to_rx(r, ax);
    *out1 = editor_row_cx_to_rx(r, r->size);
  } else if (filerow == by) {
    *out1 = editor_row_cx_to_rx(r, bx);
  } else {
    *out1 = editor_row_cx_to_rx(r, r->size);
  }
  if (*out1 < *out0) *out1 = *out0;
}

void selection_row_rx(int filerow, int *out0, int *out1) {
  selection_row_rx_of(&CUR, filerow, out0, out1);
}

/* Copy the selected bytes into a freshly allocated, NUL-terminated buffer
 * (internal line breaks become '\n'), matching selection_bytes()'s count. The
 * caller frees it. Returns NULL with *outlen 0 when the selection is empty. */
char *selection_dup(int *outlen) {
  int ax, ay, bx, by;
  *outlen = 0;
  if (!selection_range(&ax, &ay, &bx, &by)) return NULL;
  if (ay == by && ax == bx) return NULL;

  int size;
  if (ay == by) {
    size = bx - ax;
  } else {
    size = ((*row_at(ay)).size - ax) + 1;   /* tail of the top row + its newline */
    int i;
    for (i = ay + 1; i < by && i < row_count(); i++) size += (*row_at(i)).size + 1;
    size += bx;                          /* head of the bottom row */
  }
  char *buf = xmalloc((size_t)size + 1);
  int pos = 0;
  if (ay == by) {
    memcpy(buf + pos, (*row_at(ay)).chars + ax, (size_t)(bx - ax));
    pos += bx - ax;
  } else {
    memcpy(buf + pos, (*row_at(ay)).chars + ax, (size_t)((*row_at(ay)).size - ax));
    pos += (*row_at(ay)).size - ax;
    buf[pos++] = '\n';
    int i;
    for (i = ay + 1; i < by && i < row_count(); i++) {
      memcpy(buf + pos, (*row_at(i)).chars, (size_t)(*row_at(i)).size);
      pos += (*row_at(i)).size;
      buf[pos++] = '\n';
    }
    if (by < row_count()) {
      memcpy(buf + pos, (*row_at(by)).chars, (size_t)bx);
      pos += bx;
    }
  }
  buf[pos] = '\0';
  *outlen = pos;
  return buf;
}

/* Replace the clipboard contents. Allocate before releasing the old buffer:
 * if the allocation failed after the free, die()'s atexit cleanup would free
 * the stale E.clipboard a second time. Allocating first means a failure dies
 * with the clipboard still intact. */
void clipboard_set(const char *s, int len) {
  char *buf = (len > 0) ? xmalloc((size_t)len) : NULL;
  if (len > 0) memcpy(buf, s, (size_t)len);
  free(E.clipboard);
  E.clipboard = buf;
  E.clipsize = (len > 0) ? len : 0;
}

/* Delete the selected range, leaving the cursor at its start. Walks the far
 * end back to the near end with the tested single-character editor_delete_char
 * (which handles multibyte characters and line joins itself), so the selection
 * is always consumed on character boundaries. The whole run is one undo step.
 * No-op (just clears the selection) when it is empty. */
void selection_delete(void) {
  int ax, ay, bx, by;
  if (!selection_range(&ax, &ay, &bx, &by)) return;
  /* This cursor's selection, not every cursor's: with several cursors each
   * holding an occurrence, cursors.c calls this once per cursor, and
   * selection_clear() would throw away the selections of the cursors whose
   * turn has not come -- so the first one replaced its match and the rest
   * merely typed. */
  CUR.sel_active = 0;
  if (ay == by && ax == bx) return;

  undo_group_begin();
  if (by == row_count()) {
    if (row_count() == 0) { undo_group_end(); return; }
    CUR.cy = row_count() - 1;
    CUR.cx = (*row_at(row_count() - 1)).size;
  } else {
    CUR.cy = by;
    CUR.cx = bx;
  }
  while (!(CUR.cy == ay && CUR.cx == ax)) editor_delete_char();
  undo_group_end();
}

/* Insert a run of text at the cursor as one undo step; embedded newlines split
 * lines. An active selection is replaced. The newlines are the plain kind, not
 * Enter's: text that arrives from somewhere else brings its own indentation,
 * and copying the cursor row's leading whitespace onto every line of it would
 * indent it twice. */
void editor_insert_text(const char *s, int len) {
  int i;
  selection_delete();
  undo_group_begin();
  for (i = 0; i < len; i++) {
    if (s[i] == '\n') editor_insert_newline();
    else editor_insert_char((unsigned char)s[i]);
  }
  undo_group_end();
}

/* Insert the clipboard at the cursor. */
void editor_paste(void) {
  if (E.clipsize == 0) {
    editor_set_status_message("Clipboard is empty");
    return;
  }
  editor_insert_text(E.clipboard, E.clipsize);
  editor_set_status_message("Pasted %d bytes", E.clipsize);
}

/* Push the clipboard to the terminal's *system* clipboard with OSC 52, so a
 * terminal-native paste (or a Ctrl-V that the terminal intercepts and serves
 * from the system clipboard itself) yields exactly what Molly copied, and what
 * Molly copied can be pasted into other applications too. Without this the
 * in-editor clipboard is invisible to the terminal, so a paste shows whatever
 * an unrelated application last left on the system clipboard.
 *
 * Written straight to the terminal in fixed chunks: it allocates nothing, so a
 * failed copy can never die here after the in-editor clipboard already landed.
 * Strictly best-effort -- terminals that ignore OSC 52 (or have it disabled)
 * just keep their own clipboard, and Molly's own Ctrl-V still pastes the
 * internal buffer. */
void clipboard_sync_system(void) {
  static const char tbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  static const char hdr[] = "\x1b]52;c;";
  const unsigned char *in = (const unsigned char *)E.clipboard;
  int len = E.clipsize;
  if (len <= 0) return;
  write_all(STDOUT_FILENO, hdr, sizeof(hdr) - 1);

  char chunk[512];
  int c = 0, i = 0;
  while (i + 3 <= len) {
    unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) | in[i + 2];
    chunk[c++] = tbl[(v >> 18) & 63];
    chunk[c++] = tbl[(v >> 12) & 63];
    chunk[c++] = tbl[(v >> 6) & 63];
    chunk[c++] = tbl[v & 63];
    i += 3;
    if (c + 4 > (int)sizeof(chunk)) {  /* room for one more group, else flush */
      write_all(STDOUT_FILENO, chunk, (size_t)c);
      c = 0;
    }
  }
  if (i < len) {
    int rem = len - i;
    unsigned v = (unsigned)in[i] << 16;
    if (rem > 1) v |= (unsigned)in[i + 1] << 8;
    chunk[c++] = tbl[(v >> 18) & 63];
    chunk[c++] = tbl[(v >> 12) & 63];
    chunk[c++] = rem > 1 ? tbl[(v >> 6) & 63] : '=';
    chunk[c++] = '=';
  }
  if (c) write_all(STDOUT_FILENO, chunk, (size_t)c);
  write_all(STDOUT_FILENO, "\x07", 1);  /* BEL terminator (widely accepted) */
}
