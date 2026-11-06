/* buffer.c -- An open file: its rows, loading and saving it, the saved-content snapshot
   that decides whether it is really modified, and crash recovery. */

#include "molly.h"

#include <sys/xattr.h>

/* --- where a buffer's text lives -----------------------------------------
 *
 * A file is read in one piece, into one block (B.text), and each row is a span
 * of that block: its line, with the byte after it overwritten by a NUL so the row
 * is terminated where it lies. Nothing is copied per line and nothing is
 * allocated per line, which is what opening a large file costs in time and in
 * memory -- the block is the file's size, and a row is 24 bytes beside it.
 *
 * A row gets text of its own the first time it is changed (row_own). Before
 * that it is *borrowed*, and a borrowed row is never written: the block is also
 * the saved snapshot, below, and a byte changed in it would be a change to what
 * the buffer is compared against. Whether a row is borrowed is a question about
 * its address and nothing else, so no row carries a flag for it.
 *
 * The rows themselves are a gap array: the array has its free slots in one run
 * (the gap) at B.gap, and inserting or deleting a row moves the gap there first.
 * Edits cluster, so the gap is nearly always where it is wanted already, and a
 * new line in a big file costs the rows between the last edit and this one
 * rather than every row below it. row_at() is the only translation between a
 * row's index and its slot.
 *
 * --- saved-content snapshot: the buffer is only "modified" when it really
 * differs from what is on disk, so undoing back to the saved state clears the
 * dirty flag and the quit warning. The snapshot is the block: line i of what was
 * saved is the span at B.saved_off[i], one NUL-terminated line after another, so
 * a line's length is the distance to the next one less its NUL. A row that still
 * borrows its own line is equal to it without a comparison. The mtime seen when
 * it was taken (B.on_disk_mtime) is per-buffer too, since each open file has its
 * own saved state; B.have_disk_mtime is 0 for a buffer not backed by an existing
 * file yet -- a brand-new buffer, or one just named via Save As. --- */

static int mtime_eq(struct timespec a, struct timespec b) {
  return a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec;
}

/* Whether a row's text is a span of the buffer's block rather than its own. */
static int row_borrowed(const erow *row) {
  uintptr_t p = (uintptr_t)row->chars, t = (uintptr_t)B.text;
  return B.text != NULL && p >= t && p < t + B.textlen;
}

/* Give a row text of its own, before anything writes to it. */
static void row_own(erow *row) {
  char *c;
  if (!row_borrowed(row)) return;
  c = xmalloc((size_t)row->size + 1);
  memcpy(c, row->chars, (size_t)row->size + 1);
  row->chars = c;
}

/* Forget the snapshot, so nothing matches it. The block stays: rows may still
 * be borrowing from it. */
void saved_snapshot_free(void) {
  free(B.saved_off);
  B.saved_off = NULL;
  free(B.saved_crlf);
  B.saved_crlf = NULL;
  B.saved_numrows = -1;
}

/* Where a borrowed row's text starts in the block, as an offset. */
static size_t row_off(const erow *row) {
  return (size_t)(row->chars - B.text);
}

/* Point every borrowed row at the same offset in a block that has moved. */
static void rows_rebase(uintptr_t was, size_t waslen) {
  int i;
  for (i = 0; i < row_count(); i++) {
    erow *row = row_at(i);
    uintptr_t p = (uintptr_t)row->chars;
    if (p >= was && p < was + waslen) row->chars = B.text + (p - was);
  }
}

/* Make what the buffer holds now the saved state.
 *
 * Which means the rows, one NUL-terminated line after another, in the block --
 * and that is done *in* the block rather than in a new one beside it, because a
 * new one would be a second copy of the file at the moment of saving it: 2.6
 * times the file's size at the peak, where every other moment is 1.4. The block
 * grows first if the rows have, and every row still borrowing is moved to where
 * it now belongs. Then the rows with text of their own are copied in and give it
 * up. So this is also where an editing session's scattered rows come back to one
 * allocation the size of the file, at every save.
 *
 * Moving rows within one block is safe because borrowed rows are always in file
 * order: a row is only ever made borrowed by a load or by this, in row order,
 * and every inserted row has text of its own. So the rows that move right are
 * moved last row first, and each lands past the end of every row before it that
 * has not moved yet; then the rows that move left are moved first row first, and
 * each lands before the start of every row after it. Rows that own their text
 * are copied in after both, when nothing borrowed is left to overwrite.
 *
 * An allocation failure leaves the rows where they were and no snapshot at all,
 * so the buffer reads as modified until the next save: wrong in the safe
 * direction, since the worst it does is ask before quitting. */
void saved_snapshot_take(void) {
  int i, n = row_count();
  size_t need, w = 0;
  size_t *off = malloc(sizeof(size_t) * ((size_t)n + 1));
  unsigned char *crlf = malloc((size_t)n + 1);

  if (off == NULL || crlf == NULL) {
    free(off);
    free(crlf);
    saved_snapshot_free();
    return;
  }
  for (i = 0; i < n; i++) crlf[i] = row_at(i)->crlf;
  for (i = 0; i < n; i++) {
    off[i] = w;
    w += (size_t)row_at(i)->size + 1;
  }
  off[n] = w;
  need = w + 1;   /* a NUL after the last line's, so an empty block is one */

  if (B.text == NULL || need > B.textlen) {
    uintptr_t was = (uintptr_t)B.text;
    size_t waslen = B.textlen;
    char *t = realloc(B.text, need);
    if (t == NULL) {
      free(off);
      free(crlf);
      saved_snapshot_free();
      return;
    }
    B.text = t;
    B.textlen = need;
    if (was != 0) rows_rebase(was, waslen);
  }

  for (i = n - 1; i >= 0; i--) {
    erow *row = row_at(i);
    if (row_borrowed(row) && row_off(row) < off[i]) {
      memmove(B.text + off[i], row->chars, (size_t)row->size + 1);
      row->chars = B.text + off[i];
    }
  }
  for (i = 0; i < n; i++) {
    erow *row = row_at(i);
    if (row_borrowed(row) && row_off(row) > off[i]) {
      memmove(B.text + off[i], row->chars, (size_t)row->size + 1);
      row->chars = B.text + off[i];
    }
  }
  for (i = 0; i < n; i++) {
    erow *row = row_at(i);
    if (!row_borrowed(row)) {
      memcpy(B.text + off[i], row->chars, (size_t)row->size);
      B.text[off[i] + (size_t)row->size] = '\0';
      free(row->chars);
      row->chars = B.text + off[i];
    }
  }
  B.text[w] = '\0';

  /* Give back what deleting rows left unused. Only worth a call when it is a
   * real amount, and a shrink that fails or moves is still correct. */
  if (B.textlen - need > need / 8 + 4096) {
    uintptr_t was = (uintptr_t)B.text;
    size_t waslen = B.textlen;
    char *t = realloc(B.text, need);
    if (t != NULL) {
      B.text = t;
      B.textlen = need;
      rows_rebase(was, waslen);
    }
  }

  free(B.saved_off);
  B.saved_off = off;
  free(B.saved_crlf);
  B.saved_crlf = crlf;
  B.saved_numrows = n;
}

int editor_matches_saved(void) {
  int i;
  if (row_count() != B.saved_numrows) return 0;
  for (i = 0; i < row_count(); i++) {
    const erow *row = row_at(i);
    const char *was = B.text + B.saved_off[i];
    size_t len = B.saved_off[i + 1] - B.saved_off[i] - 1;
    if ((size_t)row->size != len || row->crlf != B.saved_crlf[i]) return 0;
    if (row->chars != was && memcmp(row->chars, was, len) != 0) return 0;
  }
  return 1;
}

void editor_recompute_dirty(void) {
  B.dirty = editor_matches_saved() ? 0 : 1;
}


/* --- crash recovery: when the editor is about to die with unsaved changes --
 * a fatal signal, or an out-of-memory exit through die() -- the buffer is
 * written to a side file so the work is not simply lost. The path is computed
 * ahead of time, in normal context, because the signal path may run only
 * async-signal-safe code. --- */


void build_recovery_path(void) {
  if (B.filename != NULL) {
    snprintf(B.recovery_path, sizeof(B.recovery_path), "%s.molly-recover",
        B.filename);
  } else if (B.id == 0) {
    snprintf(B.recovery_path, sizeof(B.recovery_path), "molly-recover.%ld",
        (long)getpid());
  } else {
    /* An unnamed buffer has nothing to name its recovery file after but the
     * process, and with more than one open that is no longer unique -- two
     * scratch buffers would have written over each other's recovery data. The
     * first keeps the plain name, which is what MANUAL.md documents and what a
     * user who has only ever had one will see; the rest take their buffer id. */
    snprintf(B.recovery_path, sizeof(B.recovery_path), "molly-recover.%ld-%d",
        (long)getpid(), B.id);
  }
  snprintf(B.recovery_alt, sizeof(B.recovery_alt), "%s.%ld", B.recovery_path,
           (long)geteuid());
}

/* Open a recovery file to write, at `path`, only if it is ours: a new file, or
 * one this user left before, emptied first. Never another user's: in a shared
 * directory anyone can leave a file of that name, and writing into it would
 * give them the text, or truncating it lose their own recovery data. -1 when
 * it cannot be. Safe in a signal handler: open, fstat, ftruncate and close. */
static int open_recovery(const char *path) {
  struct stat st;
  int fd;
  if (path[0] == '\0') return -1;
  fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd != -1 || errno != EEXIST) return fd;
  fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd == -1) return -1;
  if (fstat(fd, &st) == -1 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
      ftruncate(fd, 0) == -1) {
    close(fd);
    return -1;
  }
  return fd;
}

/* Best effort, and safe to call from a signal handler: only open/write/fsync/
 * close and plain memory reads. Reading B.row while it is mid-mutation can at
 * worst drop or duplicate a few bytes, which beats losing the whole buffer. */
/* Every dirty buffer's recovery file.
 *
 * This is what die() and the signal handlers call, and it has to cover all of
 * them: a crash with three files open and two of them edited used to save the
 * work in whichever one happened to be on screen and lose the other. The view
 * is borrowed around each buffer in turn because write_recovery_file() is
 * written against B, which is the view's buffer -- the same borrow buf_free()
 * makes, and acceptable for the same reason, except that here the process is
 * already on its way out. */
void write_recovery_all(void) {
  struct buffer *was = V.buf;
  int i;
  if (was == NULL) return;
  for (i = 0; i < buf_count(); i++) {
    V.buf = buf_at(i);
    write_recovery_file();
  }
  V.buf = was;
}

void write_recovery_file(void) {
  /* There may be no buffer at all yet. die() calls this on its way out, and an
   * allocation failure while the buffer list itself is being built is exactly a
   * case where it is reached before there is a buffer to recover -- B is the
   * view's buffer, so reading it then dereferences NULL. The allocation-failure
   * sweep caught this as a SIGSEGV on the first two allocations molly makes. */
  if (V.buf == NULL) return;
  if (!B.dirty || B.recovery_path[0] == '\0') return;
  /* The usual name, or when someone else has a file there, this user's own. */
  int fd = open_recovery(B.recovery_path);
  if (fd == -1) fd = open_recovery(B.recovery_alt);
  if (fd == -1) return;
  int j;
  /* As a save would write it: each line with its own ending, and the last with
   * one only if the file has one. A restore reads it back as it reads a file. */
  for (j = 0; j < row_count(); j++) {
    const erow *row = row_at(j);
    if (write_all(fd, row->chars, (size_t)row->size) == -1) break;
    if (j + 1 == row_count() && !B.final_newline) break;
    if (write_all(fd, row->crlf ? "\r\n" : "\n", row->crlf ? 2 : 1) == -1) break;
  }
  fsync(fd);
  close(fd);
}

/* Is the recovery file at `path` one molly wrote for this user: a regular file,
 * not a link, owned by us? In a shared directory anyone can leave a file of that
 * name, and offering it as "your unsaved changes" would put their text in the
 * buffer at one keypress -- or, a link to a file of our own, put that file's
 * contents into this one, to be saved where they can read it. Deleting one that
 * is not ours would lose another user's real recovery data. */
int recovery_file_ours(const char *path, struct stat *st) {
  struct stat own;
  if (st == NULL) st = &own;
  return path[0] != '\0' && lstat(path, st) == 0 && S_ISREG(st->st_mode) &&
         st->st_uid == geteuid();
}

/* Remove the buffer's recovery files, under either name, where they are ours
 * to remove. */
void recovery_files_remove(const struct buffer *b) {
  if (recovery_file_ours(b->recovery_path, NULL)) unlink(b->recovery_path);
  if (recovery_file_ours(b->recovery_alt, NULL)) unlink(b->recovery_alt);
}


/* How many columns a tab occupies: the `[editor] tab width` default, and the
 * width in force for the current buffer once a filetype has had its say.
 *
 * The resolution lives in indent.c, which is also what knows when a buffer's
 * wrap measurements have to be redone to match -- see indent_sync().
 *
 * Changing the default re-measures the current buffer, so the Options dialog
 * can change it on a live buffer and see the result rather than it taking
 * effect at the next start. */
static int tabw_default = KILO_TAB_STOP;

int tab_width(void) {
  int w = (V.buf != NULL) ? syntax_tab_width(B.syn) : 0;
  return (w > 0) ? w : tabw_default;
}

/* The default on its own, for write-back: tab_width() answers for the current
 * buffer, so saving settings with a file of a filetype that names its own width
 * open would write that filetype's width as everything's. */
int tab_width_default(void) { return tabw_default; }

void tab_width_set(int n) {
  if (n < 1 || n > 16 || n == tabw_default) return;
  tabw_default = n;
  /* Not a loop over the rows: a buffer whose filetype names its own width does
   * not move when the default does, and indent_sync() is what knows that. */
  indent_sync();
}

/* Drop every row's wrap measurement in the current buffer. Only indent_sync()
 * wants this, and only when the tab width those rows were measured at has
 * stopped being the right one. */
void editor_remeasure_all(void) {
  int i;
  for (i = 0; i < row_count(); i++) wrap_row_invalidate(row_at(i));
}

/* The index of a row of the current buffer from where it sits, or -1. The
 * inverse of row_at(): a slot below the gap is its own index, one above it is
 * the gap's length further on. */
static int row_index(const erow *row) {
  int gaplen = B.rowcap - B.numrows;
  if (B.row == NULL || row < B.row || row >= B.row + B.rowcap) return -1;
  if (row < B.row + B.gap) return (int)(row - B.row);
  if (row < B.row + B.gap + gaplen) return -1;   /* in the gap: not a row */
  return (int)(row - B.row) - gaplen;
}

void editor_update_row(erow *row) {
  /* The row's index, for the syntax watermark. Worked out from the pointer,
   * because this is called from a dozen places that hold the erow and not its
   * index. */
  if (V.buf != NULL) {
    int at = row_index(row);
    if (at >= 0) syntax_invalidate_from(at);
  }

  /* Whatever this row wrapped to, it was measured from the text that just
   * changed. */
  wrap_row_invalidate(row);
  if (V.buf != NULL) B.edits++;
}

/* Move the gap to just before row `at`, shifting the rows between where it
 * was and where it is going. */
static void gap_move(int at) {
  int gaplen = B.rowcap - B.numrows;
  if (gaplen > 0 && at < B.gap)
    memmove(&B.row[at + gaplen], &B.row[at], sizeof(erow) * (size_t)(B.gap - at));
  else if (gaplen > 0 && at > B.gap)
    memmove(&B.row[B.gap], &B.row[B.gap + gaplen],
            sizeof(erow) * (size_t)(at - B.gap));
  B.gap = at;
}

/* Room for one more row. Half as much again when full rather than double: the
 * array is the per-line cost of a big file, and doubling it for the sake of one
 * new line would add half of that again for nothing. Still a constant factor, so
 * filling it a row at a time stays O(rows) overall. */
static void rows_reserve(int more) {
  int cap;
  if (B.numrows + more <= B.rowcap) return;
  cap = B.rowcap < 32 ? 32 : B.rowcap + B.rowcap / 2;
  if (cap < B.numrows + more) cap = B.numrows + more;
  B.row = xrealloc(B.row, sizeof(erow) * (size_t)cap);
  /* The rows above the gap were at the top of the old array; they belong at
   * the top of the new one, with the new slots joining the gap. */
  memmove(&B.row[B.gap + (cap - B.numrows)], &B.row[B.gap + (B.rowcap - B.numrows)],
          sizeof(erow) * (size_t)(B.numrows - B.gap));
  B.rowcap = cap;
}

void editor_insert_row(int at, char *s, size_t len) {
  erow *row;
  if (at < 0 || at > row_count()) return;
  /* Allocate the new row's text before touching the array, so a failure here
   * dies with the array still consistent and editor_cleanup frees each row
   * once. */
  char *chars = xmalloc(len + 1);
  memcpy(chars, s, len);
  chars[len] = '\0';

  rows_reserve(1);
  gap_move(at);
  row = &B.row[B.gap];
  B.gap++;
  B.numrows++;

  row->size = len;
  row->chars = chars;
  row->syn_in = 0;
  row->syn_out = 0;
  row->crlf = (unsigned char)B.eol_crlf;
  editor_update_row(row);
  /* Inserting shifts every row below, so their stored entry states now belong
   * to the wrong rows. */
  syntax_invalidate_from(at);
  lsp_rows_moved(V.buf, at, 1);
  nav_rows_moved(V.buf, at, 1);
  fold_rows_moved(V.buf, at, 1);

  B.dirty++;
}

void editor_free_row(erow *row) {
  if (!row_borrowed(row)) free(row->chars);
}

void editor_delete_row(int at) {
  if (at < 0 || at >= row_count()) return;
  gap_move(at);
  /* Row `at` is now the first one past the gap, and one less row makes the gap
   * one longer -- so it is already where it has gone. */
  editor_free_row(&B.row[B.gap + (B.rowcap - B.numrows)]);
  B.numrows--;
  B.dirty++;
  B.edits++;
  lsp_rows_moved(V.buf, at, -1);
  nav_rows_moved(V.buf, at, -1);
  fold_rows_moved(V.buf, at, -1);
  /* Deleting shifts every row below up one, so their stored entry states belong
   * to the wrong rows now -- and the deleted row may have opened a comment. */
  syntax_invalidate_from(at);
}

/* Every row of the current buffer gone, the array kept for what comes next. */
void editor_clear_rows(void) {
  int i;
  for (i = 0; i < row_count(); i++) editor_free_row(row_at(i));
  B.numrows = 0;
  B.gap = 0;
  B.edits++;
  syntax_invalidate_from(0);
  lsp_diag_clear(V.buf);
}

/* Everything the current buffer's text owns: the rows, their array, the block
 * and the snapshot. For freeing the buffer. */
void editor_free_rows(void) {
  editor_clear_rows();
  free(B.row);
  B.row = NULL;
  B.rowcap = 0;
  free(B.saved_off);
  B.saved_off = NULL;
  free(B.saved_crlf);
  B.saved_crlf = NULL;
  B.saved_numrows = 0;
  free(B.text);
  B.text = NULL;
  B.textlen = 0;
}

/* Cut a row short at byte `at`. */
void editor_row_truncate(erow *row, int at) {
  if (at < 0 || at >= row->size) return;
  row_own(row);
  row->size = at;
  row->chars[at] = '\0';
  editor_update_row(row);
}

void editor_row_insert_char(erow *row, int at, int c) {
  if (at < 0 || at > row->size) at = row->size;
  row_own(row);
  row->chars = xrealloc(row->chars, (size_t)row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  editor_update_row(row);
  B.dirty++;
}

void editor_row_append_string(erow *row, char *s, size_t len) {
  row_own(row);
  row->chars = xrealloc(row->chars, (size_t)row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  editor_update_row(row);
}

void editor_row_delete_char(erow *row, int at) {
  if (at < 0 || at >= row->size) return;
  row_own(row);
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  editor_update_row(row);
  B.dirty++;
}

void editor_row_insert_n(erow *row, int at, const char *s, size_t n) {
  if (at < 0 || at > row->size) at = row->size;
  row_own(row);
  row->chars = xrealloc(row->chars, (size_t)row->size + n + 1);
  memmove(&row->chars[at + n], &row->chars[at], row->size - at + 1);
  memcpy(&row->chars[at], s, n);
  row->size += n;
  editor_update_row(row);
  B.dirty++;
}

void editor_row_delete_n(erow *row, int at, int n) {
  if (at < 0 || at + n > row->size) return;
  row_own(row);
  memmove(&row->chars[at], &row->chars[at + n], row->size - at - n);
  row->size -= n;
  row->chars[row->size] = '\0';
  editor_update_row(row);
  B.dirty++;
}


/* Write every row to an already-open descriptor, ending each with its own line
 * ending (LF, or CRLF), and omitting the final one
 * if the file had no trailing newline. Rows are gathered into a 64 KB buffer
 * and written a chunk at a time: writing each row and then its newline cost
 * two syscalls per line, 100,000 of them for a 50,000-line file. */
static int editor_write_rows(int fd, size_t *total) {
  static char buf[65536];
  size_t used = 0, n = 0;
  int j;
  for (j = 0; j < row_count(); j++) {
    size_t len = (size_t)(*row_at(j)).size;
    const char *eol = (*row_at(j)).crlf ? "\r\n" : "\n";
    size_t elen = (j == row_count() - 1 && !B.final_newline) ? 0 : strlen(eol);
    if (used + len + elen > sizeof(buf)) {
      if (write_all(fd, buf, used) == -1) return -1;
      used = 0;
    }
    if (len + elen > sizeof(buf)) {
      /* Longer than the whole buffer: it was flushed above, so write the
       * row straight through. */
      if (write_all(fd, (*row_at(j)).chars, len) == -1) return -1;
      if (elen && write_all(fd, eol, elen) == -1) return -1;
    } else {
      memcpy(buf + used, (*row_at(j)).chars, len);
      if (elen) memcpy(buf + used + len, eol, elen);
      used += len + elen;
    }
    n += len + elen;
  }
  if (used > 0 && write_all(fd, buf, used) == -1) return -1;
  *total = n;
  return 0;
}

/* What save_to() came to. */
enum { SAVE_DONE, SAVE_STOPPED, SAVE_REFUSED };

/* Whether every extended attribute on `a` is on `b` with the same value. A value
 * too long for the buffers counts as different: the answer only ever decides
 * between a rename and a slower write that keeps everything. */
static int xattrs_within(const char *a, const char *b) {
  char names[8192], va[4096], vb[4096];
  ssize_t n = listxattr(a, names, sizeof(names)), i = 0;
  if (n == -1) return errno == ENOTSUP || errno == ENOSYS;
  while (i < n) {
    const char *name = names + i;
    ssize_t la = getxattr(a, name, va, sizeof(va));
    ssize_t lb = getxattr(b, name, vb, sizeof(vb));
    if (la == -1 || la != lb || memcmp(va, vb, (size_t)la) != 0) return 0;
    i += (ssize_t)strlen(name) + 1;
  }
  return 1;
}

/* Ask a yes/no question. */
static int save_confirm(char *question) {
  char *ans = editor_prompt(question, NULL);
  int yes = ans != NULL && (ans[0] == 'y' || ans[0] == 'Y');
  free(ans);
  return yes;
}

/* Write the buffer to `name`.
 *
 * Writing a temp file and renaming it over the target is what makes a save atomic,
 * and it is also a way round the file's permissions: a rename asks only for a
 * writable directory. So it would replace a file the user may not write, hand a
 * teammate's file to the saver and their group, drop ACLs and other extended
 * attributes, split hard links, and swap a FIFO for a regular file. The rename is
 * used only when the temp file can stand in for the target completely. Otherwise
 * the file is rewritten where it is, which keeps all of that and says it was not
 * atomic, and a file the user has no right to write is refused.
 *
 * `typed` is a name just given, rather than the buffer's own. SAVE_REFUSED sets
 * *why, and the caller offers another name. The others have set the message. */
static int save_to(const char *name, int typed, const char *fmsg, const char **why,
                   struct stat *after, int *have_after) {
  /* If the name is a symlink, write past it to the real target so atomic
   * rename replaces the file contents rather than the link itself. */
  const char *target = name;
  char *resolved = NULL;
  struct stat st;
  if (lstat(name, &st) == 0 && S_ISLNK(st.st_mode)) {
    resolved = realpath(name, NULL);
    if (resolved == NULL) {
      editor_set_status_message("Save failed: %s", strerror(errno));
      return SAVE_STOPPED;
    }
    target = resolved;
  }

  int target_exists = (stat(target, &st) == 0);

  if (target_exists && !S_ISREG(st.st_mode)) {
    *why = S_ISDIR(st.st_mode) ? strerror(EISDIR) : "not a regular file";
    free(resolved);
    return SAVE_REFUSED;
  }

  if (target_exists && typed &&
      !save_confirm("That file already exists. Overwrite it? (y/N): %s")) {
    editor_set_status_message("Save aborted: the file already exists");
    free(resolved);
    return SAVE_STOPPED;
  }

  /* Someone else may have written the file since it was opened (or last
   * saved). Ask before overwriting their change -- only meaningful when
   * there's a prior on-disk version to compare against and to lose. */
  if (!typed && B.have_disk_mtime && target_exists &&
      !mtime_eq(st.st_mtim, B.on_disk_mtime) &&
      !save_confirm("File changed on disk since opened. Save anyway? (y/N): %s")) {
    editor_set_status_message("Save aborted: file changed on disk");
    free(resolved);
    return SAVE_STOPPED;
  }

  /* Ours to replace if we say so, since the permission is ours to change. Not
   * someone else's: that a rename would work is not permission. */
  int writable = !target_exists || access(target, W_OK) == 0;
  if (!writable && st.st_uid != geteuid()) {
    *why = strerror(EACCES);
    free(resolved);
    return SAVE_REFUSED;
  }
  if (!writable && !save_confirm("This file is read-only. Save anyway? (y/N): %s")) {
    editor_set_status_message("Save aborted: the file is read-only");
    free(resolved);
    return SAVE_STOPPED;
  }

  mode_t mode = target_exists ? (st.st_mode & 07777) : default_file_mode();

  /* Write to a unique temp file next to the target, then rename over it, so
   * an interrupted save never leaves the real file truncated or empty.
   * mkstemp creates the file exclusively under a name nobody can predict:
   * a fixed "<target>.tmp" both clobbered any real file of that name and
   * let a local attacker win the race between unlink and open, redirecting
   * the write through a symlink of their choosing. */
  size_t tlen = strlen(target);
  char *tmp = malloc(tlen + sizeof(".XXXXXX"));
  if (tmp == NULL) {
    editor_set_status_message("Save failed: out of memory");
    free(resolved);
    return SAVE_STOPPED;
  }
  memcpy(tmp, target, tlen);
  memcpy(tmp + tlen, ".XXXXXX", sizeof(".XXXXXX"));

  size_t total = 0;
  int fd = mkstemp(tmp);

  /* A full disk is no reason to overwrite the only good copy. */
  if (fd == -1 && (errno == ENOSPC || errno == EDQUOT || errno == ENOMEM)) {
    editor_set_status_message("Save failed: %s", strerror(errno));
    free(tmp);
    free(resolved);
    return SAVE_STOPPED;
  }

  if (fd != -1) {
    /* In this order, because a change of owner clears setuid and setgid.
     * mkstemp creates at 0600, so the content is never briefly world-readable. */
    int stands_in = 1;
    if (target_exists) {
      if (st.st_nlink > 1) stands_in = 0;
      if (fchown(fd, st.st_uid, st.st_gid) == -1) stands_in = 0;
    }
    if (fchmod(fd, mode) == -1 && target_exists) stands_in = 0;
    if (stands_in && target_exists &&
        !(xattrs_within(target, tmp) && xattrs_within(tmp, target)))
      stands_in = 0;
    if (!stands_in) {
      close(fd);
      unlink(tmp);
      fd = -1;
    }
  }

  if (fd == -1) {
    /* Rewrite the file where it is: no room for a sibling temp file -- typically
     * a writable file in a read-only directory -- or one that could not stand in.
     * Over the old bytes and cut to length afterwards, so running out of disk
     * part way loses as little as it can. Non-blocking and checked once open,
     * because the name could have become a FIFO since it was looked at. */
    int direct = open(target, O_WRONLY | O_CREAT | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW, mode);
    struct stat dst;
    free(tmp);
    if (direct == -1) {
      *why = strerror(errno);
      free(resolved);
      return SAVE_REFUSED;
    }
    if (fstat(direct, &dst) == -1 || !S_ISREG(dst.st_mode)) {
      close(direct);
      *why = "not a regular file";
      free(resolved);
      return SAVE_REFUSED;
    }
    free(resolved);
    int dok = (editor_write_rows(direct, &total) == 0);
    if (dok && ftruncate(direct, (off_t)total) == -1) dok = 0;
    /* Writing clears setuid and setgid. Best effort: the owner can put them back,
     * and a teammate saving a group-writable file cannot. */
    if (dok && (dst.st_mode & 06000)) (void)fchmod(direct, dst.st_mode & 07777);
    if (dok && fsync(direct) == -1) dok = 0;
    *have_after = dok && (fstat(direct, after) == 0);
    if (close(direct) == -1) dok = 0;
    if (!dok) {
      editor_set_status_message("Save failed: %s", strerror(errno));
      return SAVE_STOPPED;
    }
    editor_set_status_message("%zu bytes written (in place, not atomic)%s",
        total, fmsg);
    return SAVE_DONE;
  }

  int ok = 1;

  if (editor_write_rows(fd, &total) == -1) {
    ok = 0;
    editor_set_status_message("Save failed: %s", strerror(errno));
  }

  /* Writing clears setuid and setgid, so they go back on after it. */
  if (ok && (mode & 06000) && fchmod(fd, mode) == -1) {
    ok = 0;
    editor_set_status_message("Save failed: %s", strerror(errno));
  }

  if (ok && fsync(fd) == -1) {
    ok = 0;
    editor_set_status_message("Save failed: fsync");
  }
  /* Captured from the still-open fd, before close/rename: rename() doesn't
   * change the file's mtime (same inode), but re-stating the target path
   * afterward would be both an extra syscall and a needless race window. */
  *have_after = ok && (fstat(fd, after) == 0);
  if (close(fd) == -1 && ok) {
    ok = 0;
    editor_set_status_message("Save failed: %s", strerror(errno));
  }

  if (ok && rename(tmp, target) == -1) {
    ok = 0;
    editor_set_status_message("Save failed: %s", strerror(errno));
  }

  if (ok) fsync_parent_dir(target);
  else unlink(tmp);

  free(tmp);
  free(resolved);

  if (!ok) return SAVE_STOPPED;

  editor_set_status_message("%zu bytes written%s", total, fmsg);
  return SAVE_DONE;
}

/* A file name from the user; NULL, having said so, for none. An empty answer must
 * not become the file name: it can never be saved to, and because it is non-NULL it
 * would suppress the prompt forever, stranding the buffer with no way to write it
 * out. */
static char *save_ask_name(char *question) {
  char *name = editor_prompt(question, NULL);
  if (name == NULL) {
    editor_set_status_message("Save aborted");
    return NULL;
  }
  if (name[0] == '\0') {
    free(name);
    editor_set_status_message("Save aborted: no file name given");
    return NULL;
  }
  return name;
}

void editor_save(void) {
  if (B.tty != NULL) {
    editor_set_status_message("A terminal is not a file to save.");
    return;
  }
  if (B.readonly) {
    editor_set_status_message("This file is read-only. View > Read only turns that off.");
    return;
  }
  /* A typed name becomes the buffer's only once a save to it has worked, so a bad
   * one cannot strand the buffer on it. */
  char *typed = NULL;
  if (B.filename == NULL) {
    typed = save_ask_name("Save as: %s (ESC to cancel)");
    if (typed == NULL) return;
  }

  /* Format on save: the server's edits made first, so what is written is
   * what it formats to. Here, once the name is settled and before anything
   * is looked at on disk; a server that does not answer in time is not
   * waited for, and the file is saved as it is. */
  int formatted = lspui_format_before_save();
  char fmsg[48] = "";
  if (formatted > 0)
    snprintf(fmsg, sizeof(fmsg), ", formatted (%d change%s)", formatted,
             formatted == 1 ? "" : "s");
  else if (formatted < 0)
    snprintf(fmsg, sizeof(fmsg), ", not formatted: no answer in time");

  struct stat after;
  int have_after = 0, result;
  for (;;) {
    const char *why = NULL;
    char question[160];
    char *other;
    result = save_to(typed != NULL ? typed : B.filename, typed != NULL, fmsg, &why,
                     &after, &have_after);
    if (result != SAVE_REFUSED) break;
    snprintf(question, sizeof(question),
             "Can't save there: %s. Save as: %%s (ESC to cancel)", why);
    other = save_ask_name(question);
    free(typed);
    typed = other;
    if (typed == NULL) return;
  }
  if (result != SAVE_DONE) {
    free(typed);
    return;
  }

  B.dirty = 0;
  saved_snapshot_take();
  B.have_disk_mtime = have_after;
  if (have_after) B.on_disk_mtime = after.st_mtim;
  recovery_files_remove(V.buf);
  if (typed != NULL) {
    free(B.filename);
    B.filename = typed;
    build_recovery_path();
  }
  lsp_buffer_saved(V.buf);
  git_refresh(V.buf);
}

/* All of fp, in one allocation with a byte to spare past the end. NULL if the
 * stream ended in a read error rather than at end of file -- which includes a
 * directory, so that refusal comes from here as well.
 *
 * Sized from fstat where there is a size to be had, so a file is read into a
 * block exactly big enough with no reallocation; a pipe or anything else that
 * grows is read in increasing steps. The byte past the end is what lets the last
 * line be terminated where it lies when the file has no final newline. */
static char *read_all(FILE *fp, size_t *len) {
  struct stat st;
  size_t cap = 4096, n = 0;
  char *b;

  if (fstat(fileno(fp), &st) == 0 && S_ISREG(st.st_mode) &&
      (size_t)st.st_size + 1 > cap)
    cap = (size_t)st.st_size + 1;
  b = xmalloc(cap);
  for (;;) {
    int c;
    n += fread(b + n, 1, cap - 1 - n, fp);
    if (n < cap - 1) break;              /* short: the end, or an error */
    /* Exactly full, which is the ordinary case for a regular file. Only grow
     * if there turns out to be more. */
    if ((c = getc(fp)) == EOF) break;
    cap += cap / 2;
    b = xrealloc(b, cap);
    b[n++] = (char)c;
  }
  if (ferror(fp)) {
    free(b);
    return NULL;
  }
  *len = n;
  return b;
}

/* Make rows of `text`, which is `n` bytes of a file with room for one more,
 * and set the end-of-line style flags from what was seen.
 *
 * With `adopt` the block becomes the buffer's: each line is moved down to sit
 * straight after the previous one with a NUL after it, the rows point at the
 * lines where they now lie, and the block is the saved snapshot as well. For a
 * file with LF endings nothing moves -- the newline is the byte that becomes the
 * NUL -- and a CRLF file moves each line back by the carriage returns before it.
 * Without `adopt` the lines are copied into rows of their own and the block is
 * freed, which is what restoring a recovery file wants: the snapshot there is
 * still the file on disk.
 *
 * A line ends at a newline, and gives up every carriage return and newline it
 * ends with -- the rule this had when lines were read one at a time, kept so that
 * no file loads differently. */
static void rows_from_text(char *text, size_t n, int adopt) {
  size_t i = 0, w = 0, lines = 0;
  int first_cr = 0, had_final_nl = 1;
  const char *p;

  for (p = text; p < text + n && (p = memchr(p, '\n', (size_t)(text + n - p)));
       p++)
    lines++;
  if (n > 0 && text[n - 1] != '\n') lines++;
  if (lines > (size_t)INT_MAX - 1) die("too many lines");

  if (adopt) {
    rows_reserve((int)lines);
    gap_move(row_count());
    free(B.saved_off);
    B.saved_off = xmalloc(sizeof(size_t) * (lines + 1));
  }

  while (i < n) {
    const char *nl = memchr(text + i, '\n', n - i);
    size_t e = nl ? (size_t)(nl - text) : n, len = e - i;
    int had_cr = 0;
    had_final_nl = (nl != NULL);
    /* Exactly one line ending comes off, and only a real one. Any other carriage
     * return is text: taking it would change bytes nobody edited. */
    if (nl != NULL && len > 0 && text[i + len - 1] == '\r') {
      had_cr = 1;
      len--;
    }
    if (i == 0) first_cr = had_cr;
    if (len > INT_MAX - 1) die("line too long");
    if (adopt) {
      erow *row = &B.row[B.gap];
      if (w != i) memmove(text + w, text + i, len);
      text[w + len] = '\0';
      B.saved_off[row_count()] = w;
      row->chars = text + w;
      row->size = (int)len;
      row->wcols = -1;
      row->wrows = 1;
      row->syn_in = 0;
      row->syn_out = 0;
      row->crlf = (unsigned char)had_cr;
      B.gap++;
      B.numrows++;
      w += len + 1;
    } else {
      editor_insert_row(row_count(), text + i, len);
      row_at(row_count() - 1)->crlf = (unsigned char)had_cr;
    }
    i = e + 1;
  }

  if (adopt) {
    B.saved_off[row_count()] = w;
    B.saved_numrows = row_count();
    free(B.text);
    B.text = text;
    B.textlen = n + 1;
    B.wrapgen++;
    syntax_invalidate_from(0);
  } else {
    free(text);
  }
  if (lines > 0) {
    B.eol_crlf = first_cr;
    B.final_newline = had_final_nl;
    /* A last line with no ending has none to keep, and takes the file's if it is
     * ever given one. */
    if (!had_final_nl) row_at(row_count() - 1)->crlf = (unsigned char)first_cr;
  }
  if (adopt) {
    /* The breaks are part of what was loaded, now that the last row's is set. */
    int k;
    free(B.saved_crlf);
    B.saved_crlf = xmalloc((size_t)row_count() + 1);
    for (k = 0; k < row_count(); k++) B.saved_crlf[k] = row_at(k)->crlf;
  }
}

/* Read every line of fp into the (already empty) buffer. With `adopt` the text
 * becomes the buffer's block and its saved snapshot; see rows_from_text().
 * Returns -1 if the stream ended in a read error rather than at end of file. */
int editor_fill_rows(FILE *fp, int adopt) {
  size_t n;
  char *text = read_all(fp, &n);
  /* An error taken for the end would present a partial buffer as the whole
   * file, and the next save would truncate the file on disk. */
  if (text == NULL) return -1;
  rows_from_text(text, n, adopt);
  return 0;
}

/* A descriptor to read `path` from, if it is a regular file. -1 otherwise, with
 * *why saying what is wrong, or NULL if the file simply does not exist yet.
 *
 * Non-blocking, because opening a FIFO with no writer never returns, with Ctrl-C
 * already off and no screen drawn. Nothing but a regular file is let through: a
 * device can have no end to read. The caller reads this descriptor rather than
 * opening the name again, so what was checked is what is read. */
int file_open_read(const char *path, const char **why) {
  struct stat st;
  int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  *why = NULL;
  if (fd == -1) {
    if (errno != ENOENT) *why = strerror(errno);
    return -1;
  }
  if (fstat(fd, &st) == -1) {
    *why = strerror(errno);
  } else if (S_ISDIR(st.st_mode)) {
    *why = strerror(EISDIR);
  } else if (!S_ISREG(st.st_mode)) {
    *why = "not a regular file";
  }
  if (*why != NULL) {
    close(fd);
    return -1;
  }
  return fd;
}

/* Into the current buffer. `fd` is from file_open_read(), and -1 for a file that
 * does not exist yet. */
void editor_open(char *filename, int fd) {
  free(B.filename);
  B.filename = xstrdup(filename);
  build_recovery_path();
  /* Which language, from the new name. Before the rows are read, so the first
   * lex of the first row already has a definition to work with. */
  syntax_retype();
  B.have_disk_mtime = 0;

  /* A missing file opens as an empty clean buffer. The dirty/snapshot
   * reset matters from mid-session too: after switching files the stale
   * snapshot of the previous buffer must not leak into this one. */
  if (fd == -1) {
    B.dirty = 0;
    saved_snapshot_take();
    return;
  }
  FILE *fp = fdopen(fd, "r");
  if (!fp) die("fdopen");
  if (editor_fill_rows(fp, 1) == -1) die(filename);
  /* fstat on the still-open descriptor, not a path-based stat after close:
   * the file could otherwise be replaced between the two calls, baselining
   * the wrong content's mtime. */
  struct stat ost;
  if (fstat(fileno(fp), &ost) == 0) {
    B.on_disk_mtime = ost.st_mtim;
    B.have_disk_mtime = 1;
  }
  fclose(fp);
  /* The rows are the snapshot already: they were read into it. */
  B.dirty = 0;
  /* A file molly could not write back is opened read-only, so the edits
   * that a save would then throw away are refused as they are made, rather
   * than at the end. */
  B.readonly = access(filename, W_OK) != 0;
}

/* Startup recovery UX: when the file being opened has a recovery file left by
 * a crashed session, offer its contents before this session's own save/quit
 * cleanup removes it. R replaces the buffer with the recovered contents and
 * marks them dirty, so an accidental Ctrl-S cannot silently drop them; the
 * file on disk, its mtime baseline and the saved snapshot are deliberately
 * left alone, because what is saved is still what is on disk. Any other key
 * continues with the on-disk file, and the stale recovery file is cleaned up
 * by the usual save/quit unlink exactly as it was before this prompt existed.
 * Only files named on the command line are offered: an anonymous buffer's
 * recovery file (molly-recover.PID) has no obvious owner to prompt for. */
void editor_check_recovery(void) {
  if (B.filename == NULL || B.recovery_path[0] == '\0') return;
  struct stat st;
  const char *rp;
  /* The usual name, or this user's own beside it, which is where the work went
   * if the usual name was someone else's at the time. */
  if (recovery_file_ours(B.recovery_path, &st)) {
    rp = B.recovery_path;
  } else if (recovery_file_ours(B.recovery_alt, &st)) {
    rp = B.recovery_alt;
  } else {
    const char *base = strrchr(B.recovery_path, '/');
    if (lstat(B.recovery_path, &st) == 0)
      editor_set_status_message("Not offered: %s is not your recovery file.",
                                base != NULL ? base + 1 : B.recovery_path);
    return;
  }

  editor_set_status_message(
      "Unsaved changes from a crashed session. R = restore | any key = drop");
  editor_refresh_screen();
  int c;
  do {
    c = ui_wait_key();
  } while (c == WINCH_KEY);
  if (c != 'r' && c != 'R') {
    editor_set_status_message("Recovery data dropped");
    return;
  }

  /* The file that was looked at, and not one put in its place since. */
  int fd = open(rp, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  struct stat now;
  FILE *fp = NULL;
  if (fd != -1 && fstat(fd, &now) == 0 && now.st_dev == st.st_dev &&
      now.st_ino == st.st_ino && now.st_uid == geteuid())
    fp = fdopen(fd, "r");
  if (fp == NULL) {
    int err = errno;
    if (fd != -1) close(fd);
    editor_set_status_message("Restore failed: %s",
                              fd == -1 ? strerror(err) : "the recovery file changed");
    return;
  }
  editor_clear_rows();
  CUR.cx = 0;
  CUR.rx = 0;
  CUR.cy = 0;
  V.drowoff = 0;
  V.coloff = 0;
  CUR.sel_active = 0;
  if (editor_fill_rows(fp, 0) == -1) {
    /* Only reachable if the file turns unreadable between the stat and the
     * read; the buffer is left holding whatever was salvaged. */
    fclose(fp);
    editor_set_status_message("Restore failed: recovery file unreadable");
    return;
  }
  fclose(fp);
  B.dirty = 1;
  editor_set_status_message("Restored unsaved changes (Ctrl-S to keep them)");
}
