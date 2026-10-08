/* buflist.c -- the list of open buffers.
 *
 * Olly held one file, and molly held one until now because Phase 0 only had to
 * separate buffer from view rather than make either plural. This is the part
 * that makes it plural, and it is mostly bookkeeping: `struct buffer` already
 * carried everything that belongs to a file (its text, its dirty flag, its undo
 * history, its saved snapshot, its recovery path), and `struct view` already
 * pointed at one rather than owning it. What was missing was somewhere to keep
 * more than one and a way to say which is current.
 *
 * Two decisions worth knowing, both in the comments on struct editorConfig and
 * struct buffer, repeated here because this is the file that depends on them:
 *
 *   - The list holds *pointers*. An array of structs would move every buffer
 *     when it grew, and `struct view` holds a `struct buffer *` -- so a view
 *     would end up looking at freed memory, or at the wrong file. With pointers
 *     a buffer's address is fixed for as long as it is open.
 *
 *   - `B` is a macro for `(*V.buf)`. That is why nearly two hundred existing
 *     references to `B.dirty`, `B.undo` and the rest did not have to change:
 *     "the current buffer" means "the current view's buffer", which is also the
 *     only definition that still works once there are splits.
 *
 * What is deliberately *not* here: closing the last buffer. molly always has at
 * least one, because an editor with no buffer has nothing to draw and every
 * caller would need a null check for a state the user cannot usefully be in.
 * Closing the only buffer empties it instead, which is what every editor with
 * tabs does.
 */

#include "molly.h"

/* ---- the list ------------------------------------------------------------ */

int buf_count(void) { return E.nbufs; }

struct buffer *buf_at(int i) {
  if (i < 0 || i >= E.nbufs) return NULL;
  return E.bufs[i];
}

int buf_index_of(const struct buffer *b) {
  int i;
  for (i = 0; i < E.nbufs; i++)
    if (E.bufs[i] == b) return i;
  return -1;
}

/* Which buffer is current: not stored, derived. Storing an index as well as the
 * view's pointer would be two sources of truth for one fact, and they would
 * disagree the first time a buffer was closed from under the index. */
int buf_current(void) { return buf_index_of(V.buf); }

int buf_dirty_count(void) {
  int i, n = 0;
  for (i = 0; i < E.nbufs; i++)
    if (E.bufs[i]->dirty) n++;
  return n;
}

/* The buffer already showing this file, or -1.
 *
 * Compared as written rather than resolved to a real path: two names for one
 * file would be two buffers, which is wrong, but resolving means a stat per open
 * buffer per open, and the names a user actually types are the ones they just
 * picked out of molly's own file browser. Worth revisiting when the file
 * explorer can reach the same file by two routes. */
int buf_find_path(const char *path) {
  int i;
  if (path == NULL) return -1;
  for (i = 0; i < E.nbufs; i++) {
    const char *n = E.bufs[i]->filename;
    if (n != NULL && strcmp(n, path) == 0) return i;
  }
  return -1;
}

/* ---- making and freeing ---------------------------------------------------- */

/* A buffer's starting state. Not a memset alone: the two fields whose zero is
 * wrong are the line-ending ones, because a brand-new file gets Unix endings and
 * a trailing newline rather than neither. */
static void buf_reset(struct buffer *b, int id) {
  memset(b, 0, sizeof(*b));
  b->id = id;
  b->eol_crlf = 0;
  b->final_newline = 1;
}

/* Append an empty, unnamed buffer and return it. Does not make it current; the
 * caller decides, because the open path wants to load the file before anything
 * is drawn and a buffer made for a background purpose should not steal focus. */
struct buffer *buf_new(void) {
  struct buffer *b;
  if (E.nbufs == E.bufcap) {
    int cap = E.bufcap ? E.bufcap * 2 : 4;
    E.bufs = xrealloc(E.bufs, (size_t)cap * sizeof(*E.bufs));
    E.bufcap = cap;
  }
  b = xmalloc(sizeof(*b));
  buf_reset(b, E.next_buf_id++);
  E.bufs[E.nbufs++] = b;
  return b;
}

/* Everything a buffer owns. The row array, the saved snapshot and the undo
 * history each have their own owner elsewhere, so this calls them rather than
 * reaching into them -- which is what keeps the piece-table rewrite from having
 * to touch this file.
 *
 * The undo and snapshot frees work on the *current* buffer, because that is what
 * their B-relative code says, so freeing a buffer that is not current means
 * pointing the view at it for the duration. That is ugly and it is honest: the
 * alternative is giving every one of those functions a buffer argument, which is
 * a change to code the buffer rewrite is going to replace. The view is restored
 * before returning unless the view was looking at this very buffer, in which
 * case the caller is mid-close and sets it afterwards. */
static void buf_free(struct buffer *b) {
  struct buffer *was = V.buf;
  int i;

  V.buf = b;
  for (i = 0; i < b->numrows; i++) editor_free_row(&b->row[i]);
  free(b->row);
  b->row = NULL;
  b->numrows = 0;
  b->rowcap = 0;
  saved_snapshot_free();
  editor_free_undo_redo();
  free(b->filename);
  b->filename = NULL;
  V.buf = (was == b) ? NULL : was;
  free(b);
}

/* ---- switching ------------------------------------------------------------- */

/* Remember where the current view is, so coming back to this buffer comes back
 * to the same place. Clamped on the way out rather than on the way in, because
 * the buffer may be edited (or reloaded shorter) while it is not being shown. */
static void buf_remember(void) {
  if (V.buf == NULL) return;
  B.last_cx = CUR.cx;
  B.last_cy = CUR.cy;
  B.last_drowoff = V.drowoff;
  B.last_coloff = V.coloff;
}

void buf_show(int i) {
  struct buffer *b = buf_at(i);
  if (b == NULL || b == V.buf) return;

  buf_remember();
  V.buf = b;

  /* One cursor on arrival. A buffer switch is not the place to carry several
   * cursors over: they were placed against the other file's rows. */
  cursors_collapse();
  CUR.sel_active = 0;
  CUR.cy = (b->last_cy > b->numrows) ? b->numrows : b->last_cy;
  if (CUR.cy < 0) CUR.cy = 0;
  CUR.cx = b->last_cx;
  if (CUR.cy >= b->numrows) {
    /* The phantom row past the last line, which is where a remembered row lands
     * when the file has got shorter since. It holds no characters, so there is
     * no column but zero to be in -- anything else leaves the cursor drawn out
     * in the blank and an insert addressing an offset the row does not have. */
    CUR.cx = 0;
  } else if (CUR.cx > b->row[CUR.cy].size) {
    CUR.cx = b->row[CUR.cy].size;
  }
  if (CUR.cx < 0) CUR.cx = 0;
  V.drowoff = (b->last_drowoff < 0) ? 0 : b->last_drowoff;
  V.coloff = (b->last_coloff < 0) ? 0 : b->last_coloff;

  /* The gutter's width depends on the line count, so the whole text area shifts
   * when a 9-line file is replaced by a 1000-line one. */
  screen_force_repaint();
}

/* Close buffer `i`. Returns the index now current.
 *
 * The last buffer is not closable: it is emptied instead, which keeps the
 * invariant that there is always something to draw. Closing the current buffer
 * moves to the next one along, or to the previous one when the closed buffer was
 * last -- which is what leaves the eye where it was rather than at the far end
 * of the tab bar. */
int buf_close(int i) {
  struct buffer *b = buf_at(i);
  int was_current;

  if (b == NULL) return buf_current();
  was_current = (b == V.buf);

  if (E.nbufs == 1) {
    /* Emptied rather than closed. A fresh unnamed buffer in the same slot, so
     * the view keeps a valid pointer and nothing else has to know. */
    int id = b->id;
    buf_free(b);
    E.bufs[0] = xmalloc(sizeof(**E.bufs));
    buf_reset(E.bufs[0], id);
    V.buf = E.bufs[0];
    cursors_collapse();
    memset(&CUR, 0, sizeof CUR);
    V.drowoff = 0;
    V.coloff = 0;
    build_recovery_path();
    saved_snapshot_take();
    screen_force_repaint();
    return 0;
  }

  if (was_current) {
    /* Point the view somewhere valid *before* the buffer goes, so that the free
     * below -- which borrows V.buf -- has somewhere to put it back. */
    int to = (i + 1 < E.nbufs) ? i + 1 : i - 1;
    buf_show(to);
  }

  buf_free(b);
  memmove(&E.bufs[i], &E.bufs[i + 1],
          (size_t)(E.nbufs - i - 1) * sizeof(*E.bufs));
  E.nbufs--;
  screen_force_repaint();
  return buf_current();
}

/* ---- lifetime -------------------------------------------------------------- */

/* One empty buffer, which the view is pointed at. Called before anything reads
 * B, because B is V.buf and a null view buffer is not a state the editor can
 * draw. */
void buflist_init(void) {
  E.bufs = NULL;
  E.nbufs = 0;
  E.bufcap = 0;
  E.next_buf_id = 0;
  V.buf = buf_new();
}

void buflist_cleanup(void) {
  /* Backwards, so that each buf_free() borrows a V.buf that is still alive --
   * except for the last one, which is the view's own and leaves it NULL. */
  while (E.nbufs > 0) {
    struct buffer *b = E.bufs[--E.nbufs];
    buf_free(b);
  }
  free(E.bufs);
  E.bufs = NULL;
  E.bufcap = 0;
  V.buf = NULL;
}
