/* buffer.c -- An open file: its rows, loading and saving it, the saved-content snapshot
   that decides whether it is really modified, and crash recovery. */

#include "molly.h"

/* --- saved-content snapshot: the buffer is only "modified" when it really
 * differs from what is on disk, so undoing back to the saved state clears the
 * dirty flag and the quit warning. The snapshot (B.saved_rows) and the mtime
 * seen when it was taken (B.on_disk_mtime) are per-buffer, since each open file
 * has its own saved state; B.have_disk_mtime is 0 for a buffer not backed by an
 * existing file yet -- a brand-new buffer, or one just named via Save As. --- */

static int mtime_eq(struct timespec a, struct timespec b) {
  return a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec;
}

void saved_snapshot_free(void) {
  int i;
  for (i = 0; i < B.saved_numrows; i++) {
    free(B.saved_rows[i].render);
    free(B.saved_rows[i].chars);
  }
  free(B.saved_rows);
  B.saved_rows = NULL;
  B.saved_numrows = 0;
}

void saved_snapshot_take(void) {
  int i;
  saved_snapshot_free();
  if (row_count() == 0) return;
  B.saved_rows = malloc(sizeof(erow) * (size_t)row_count());
  if (B.saved_rows == NULL) return;
  B.saved_numrows = row_count();
  for (i = 0; i < B.saved_numrows; i++) {
    B.saved_rows[i].size = (*row_at(i)).size;
    B.saved_rows[i].chars = malloc((size_t)(*row_at(i)).size + 1);
    if (B.saved_rows[i].chars != NULL) {
      memcpy(B.saved_rows[i].chars, (*row_at(i)).chars, (size_t)(*row_at(i)).size);
      B.saved_rows[i].chars[(*row_at(i)).size] = '\0';
    } else {
      B.saved_rows[i].size = 0;
    }
    B.saved_rows[i].rsize = 0;
    B.saved_rows[i].render = NULL;
    B.saved_rows[i].rcap = 0;
  }
}

int editor_matches_saved(void) {
  int i;
  if (row_count() != B.saved_numrows) return 0;
  for (i = 0; i < row_count(); i++) {
    if (B.saved_rows[i].chars == NULL) return 0;
    if ((*row_at(i)).size != B.saved_rows[i].size) return 0;
    if (memcmp((*row_at(i)).chars, B.saved_rows[i].chars, (size_t)(*row_at(i)).size) != 0)
      return 0;
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
}

/* Best effort, and safe to call from a signal handler: only open/write/fsync/
 * close and plain memory reads. Reading B.row while it is mid-mutation can at
 * worst drop or duplicate a few bytes, which beats losing the whole buffer. */
void write_recovery_file(void) {
  /* There may be no buffer at all yet. die() calls this on its way out, and an
   * allocation failure while the buffer list itself is being built is exactly a
   * case where it is reached before there is a buffer to recover -- B is the
   * view's buffer, so reading it then dereferences NULL. The allocation-failure
   * sweep caught this as a SIGSEGV on the first two allocations molly makes. */
  if (V.buf == NULL) return;
  if (!B.dirty || B.recovery_path[0] == '\0') return;
  int fd = open(B.recovery_path,
      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd == -1) return;
  int j;
  for (j = 0; j < row_count(); j++) {
    if (write_all(fd, (*row_at(j)).chars, (size_t)(*row_at(j)).size) == -1) break;
    if (write_all(fd, "\n", 1) == -1) break;
  }
  fsync(fd);
  close(fd);
}


/* How many columns a tab occupies. A variable rather than KILO_TAB_STOP because
 * `[editor] tab width` is configurable, and it lives here because this is where
 * the render string that depends on it is built.
 *
 * Changing it re-renders every row, so the Options dialog can change it on a live
 * buffer and see the result, rather than it taking effect at the next start. That
 * is O(bytes in the buffer) for a setting nobody changes twice -- cheaper than
 * the alternative, which is a stale `render` that disagrees with the cursor
 * arithmetic measured against the new width. */
static int tabw = KILO_TAB_STOP;

int tab_width(void) { return tabw; }

void tab_width_set(int n) {
  int i;
  if (n < 1 || n > 16 || n == tabw) return;
  tabw = n;
  for (i = 0; i < B.numrows; i++) editor_update_row(&B.row[i]);
}

void editor_update_row(erow *row) {
  int tabs = 0;
  int j;

  for (j = 0; j < row->size; j++)
    if (row->chars[j] == '\t') tabs++;

  /* With no tabs, render is byte-for-byte chars, so alias it rather than keep
   * a second copy of every line. rcap > 0 marks an owned render buffer (always
   * a distinct allocation); rcap == 0 marks an alias to chars -- or a stale
   * pointer to a chars block a mutator just reallocated, which is why the
   * alias case must never free it. editor_free_row keys off rcap the same way. */
  if (tabs == 0) {
    if (row->rcap > 0) free(row->render);
    row->render = row->chars;
    row->rsize = row->size;
    row->rcap = 0;
    return;
  }

  if (row->rcap == 0) row->render = NULL;
  {
    int need = row->size + tabs * (tabw - 1) + 1;
    if (row->rcap < need) {
      row->render = xrealloc(row->render, (size_t)need);
      row->rcap = need;
    }
  }

  int idx = 0;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % tabw != 0) row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;
}

void editor_insert_row(int at, char *s, size_t len) {
  if (at < 0 || at > row_count()) return;
  /* Allocate the new row's text before touching the array. The memmove below
   * duplicates each shifted row's pointers into the next slot for an instant;
   * if an allocation failed after that and died, editor_cleanup would free
   * the duplicated pointer twice. Allocating first means a failure here dies
   * with the array still consistent. */
  char *chars = xmalloc(len + 1);
  memcpy(chars, s, len);
  chars[len] = '\0';

  /* Double the capacity when full, so loading an n-line file costs O(log n)
   * reallocations rather than one per line. */
  if (row_count() == B.rowcap) {
    B.rowcap = B.rowcap ? B.rowcap * 2 : 32;
    B.row = xrealloc(B.row, sizeof(erow) * (size_t)B.rowcap);
  }
  memmove(row_at(at + 1), row_at(at), sizeof(erow) * (row_count() - at));

  (*row_at(at)).size = len;
  (*row_at(at)).chars = chars;
  (*row_at(at)).rsize = 0;
  (*row_at(at)).render = NULL;
  (*row_at(at)).rcap = 0;
  editor_update_row(row_at(at));

  B.numrows++;
  B.dirty++;
}

void editor_free_row(erow *row) {
  if (row->rcap > 0) free(row->render);  /* rcap > 0 => render is owned */
  free(row->chars);
}

void editor_delete_row(int at) {
  if (at < 0 || at >= row_count()) return;
  editor_free_row(row_at(at));
  memmove(row_at(at), row_at(at + 1), sizeof(erow) * (row_count() - at - 1));
  B.numrows--;
  B.dirty++;
}

void editor_row_insert_char(erow *row, int at, int c) {
  if (at < 0 || at > row->size) at = row->size;
  row->chars = xrealloc(row->chars, (size_t)row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  editor_update_row(row);
  B.dirty++;
}

void editor_row_append_string(erow *row, char *s, size_t len) {
  row->chars = xrealloc(row->chars, (size_t)row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  editor_update_row(row);
}

void editor_row_delete_char(erow *row, int at) {
  if (at < 0 || at >= row->size) return;
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  editor_update_row(row);
  B.dirty++;
}

void editor_row_insert_n(erow *row, int at, const char *s, size_t n) {
  if (at < 0 || at > row->size) at = row->size;
  row->chars = xrealloc(row->chars, (size_t)row->size + n + 1);
  memmove(&row->chars[at + n], &row->chars[at], row->size - at + 1);
  memcpy(&row->chars[at], s, n);
  row->size += n;
  editor_update_row(row);
  B.dirty++;
}

void editor_row_delete_n(erow *row, int at, int n) {
  if (at < 0 || at + n > row->size) return;
  memmove(&row->chars[at], &row->chars[at + n], row->size - at - n);
  row->size -= n;
  row->chars[row->size] = '\0';
  editor_update_row(row);
  B.dirty++;
}


/* Write every row to an already-open descriptor, ending each with the line
 * ending the file was loaded with (LF, or CRLF), and omitting the final one
 * if the file had no trailing newline. Rows are gathered into a 64 KB buffer
 * and written a chunk at a time: writing each row and then its newline cost
 * two syscalls per line, 100,000 of them for a 50,000-line file. */
static int editor_write_rows(int fd, size_t *total) {
  static char buf[65536];
  const char *eol = B.eol_crlf ? "\r\n" : "\n";
  size_t eollen = B.eol_crlf ? 2 : 1;
  size_t used = 0, n = 0;
  int j;
  for (j = 0; j < row_count(); j++) {
    size_t len = (size_t)(*row_at(j)).size;
    size_t elen = (j == row_count() - 1 && !B.final_newline) ? 0 : eollen;
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

/* A rename is only durable once the directory entry itself is on disk. */

void editor_save(void) {
  if (B.filename == NULL) {
    char *name = editor_prompt("Save as: %s (ESC to cancel)", NULL);
    if (name == NULL) {
      editor_set_status_message("Save aborted");
      return;
    }
    /* An empty answer must not become the file name: it can never be saved
     * to, and because it is non-NULL it would suppress this prompt forever,
     * stranding the buffer with no way to write it out. */
    if (name[0] == '\0') {
      free(name);
      editor_set_status_message("Save aborted: no file name given");
      return;
    }
    B.filename = name;
    build_recovery_path();
    B.have_disk_mtime = 0;
  }

  /* If the name is a symlink, write past it to the real target so atomic
   * rename replaces the file contents rather than the link itself. */
  const char *target = B.filename;
  char *resolved = NULL;
  struct stat st;
  if (lstat(B.filename, &st) == 0 && S_ISLNK(st.st_mode)) {
    resolved = realpath(B.filename, NULL);
    if (resolved == NULL) {
      editor_set_status_message("Save failed: %s", strerror(errno));
      return;
    }
    target = resolved;
  }

  int target_exists = (stat(target, &st) == 0);

  /* Someone else may have written the file since it was opened (or last
   * saved). Ask before overwriting their change -- only meaningful when
   * there's a prior on-disk version to compare against and to lose. */
  if (B.have_disk_mtime && target_exists && !mtime_eq(st.st_mtim, B.on_disk_mtime)) {
    char *ans = editor_prompt(
        "File changed on disk since opened. Save anyway? (y/N): %s", NULL);
    int proceed = ans != NULL && (ans[0] == 'y' || ans[0] == 'Y');
    free(ans);
    if (!proceed) {
      editor_set_status_message("Save aborted: file changed on disk");
      free(resolved);
      return;
    }
  }

  mode_t mode = target_exists ? (st.st_mode & 07777) : default_file_mode();
  uid_t owner_uid = target_exists ? st.st_uid : (uid_t)-1;
  gid_t owner_gid = target_exists ? st.st_gid : (gid_t)-1;

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
    return;
  }
  memcpy(tmp, target, tlen);
  memcpy(tmp + tlen, ".XXXXXX", sizeof(".XXXXXX"));

  size_t total = 0;
  int fd = mkstemp(tmp);

  if (fd == -1) {
    /* No room to create a sibling temp file -- typically a writable file in
     * a read-only directory. Fall back to rewriting the file in place, which
     * still saves the user's work, but say so: this write is not atomic. */
    int direct = open(target, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC |
                              O_NOFOLLOW, mode);
    if (direct == -1) {
      editor_set_status_message("Save failed: %s", strerror(errno));
      free(tmp);
      free(resolved);
      return;
    }
    int dok = (editor_write_rows(direct, &total) == 0);
    if (dok && fsync(direct) == -1) dok = 0;
    struct stat st_after;
    int have_after = dok && (fstat(direct, &st_after) == 0);
    if (close(direct) == -1) dok = 0;
    free(tmp);
    free(resolved);
    if (!dok) {
      editor_set_status_message("Save failed: %s", strerror(errno));
      return;
    }
    editor_set_status_message("%zu bytes written (in place, not atomic)",
        total);
    B.dirty = 0;
    saved_snapshot_take();
    B.have_disk_mtime = have_after;
    if (have_after) B.on_disk_mtime = st_after.st_mtim;
    if (B.recovery_path[0] != '\0') unlink(B.recovery_path);
    return;
  }

  int ok = 1;
  /* Not fatal if it fails: the file is saved either way, only its mode is off.
   * mkstemp creates at 0600, so the content is never briefly world-readable. */
  (void)fchmod(fd, mode);
  /* Keep the file's owner across the replace when we have the privilege to;
   * an unprivileged process simply cannot, and the save still succeeds. */
  if (target_exists) (void)fchown(fd, owner_uid, owner_gid);

  if (editor_write_rows(fd, &total) == -1) {
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
  struct stat st_after;
  int have_after = ok && (fstat(fd, &st_after) == 0);
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

  if (!ok) return;

  editor_set_status_message("%zu bytes written", total);
  B.dirty = 0;
  saved_snapshot_take();
  B.have_disk_mtime = have_after;
  if (have_after) B.on_disk_mtime = st_after.st_mtim;
  if (B.recovery_path[0] != '\0') unlink(B.recovery_path);
}

/* Read every line of fp into the (already empty) buffer and set the end-of-
 * line style flags from what was seen. Returns -1 if the stream ended in a
 * read error rather than at end of file. */
int editor_fill_rows(FILE *fp) {
  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  int any = 0, saw_cr = 0, had_final_nl = 1;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    any = 1;
    /* Whether this line -- so, once the loop ends, the last line -- carried a
     * trailing newline, and whether the endings were CRLF. Both are preserved
     * on save instead of being silently rewritten. */
    had_final_nl = (linelen > 0 && line[linelen - 1] == '\n');
    int had_cr = 0;
    while (linelen > 0 && (line[linelen - 1] == '\n' ||
        line[linelen - 1] == '\r')) {
      if (line[linelen - 1] == '\r') had_cr = 1;
      linelen--;
    }
    if (had_cr) saw_cr = 1;
    if (linelen > INT_MAX - 1) die("line too long");
    editor_insert_row(row_count(), line, (size_t)linelen);
  }
  free(line);
  /* getline returns -1 for a read error as well as at end of file. Taking an
   * error for the end would present a partial buffer as the whole file, and
   * the next save would truncate the file on disk. */
  if (!feof(fp)) return -1;
  if (any) {
    B.eol_crlf = saw_cr;
    B.final_newline = had_final_nl;
  }
  return 0;
}

void editor_open(char *filename) {
  free(B.filename);
  B.filename = xstrdup(filename);
  build_recovery_path();
  B.have_disk_mtime = 0;

  FILE *fp = fopen(filename, "r");
  if (!fp) {
    /* A missing file opens as an empty clean buffer. The dirty/snapshot
     * reset matters from mid-session too: after switching files the stale
     * snapshot of the previous buffer must not leak into this one. */
    if (errno == ENOENT) {
      B.dirty = 0;
      saved_snapshot_take();
      return;
    }
    die("fopen");
  }
  /* The error refusal also covers a directory, which used to open as an
   * empty buffer that could never be saved. */
  if (editor_fill_rows(fp) == -1) die(filename);
  /* fstat on the still-open descriptor, not a path-based stat after close:
   * the file could otherwise be replaced between the two calls, baselining
   * the wrong content's mtime. */
  struct stat ost;
  if (fstat(fileno(fp), &ost) == 0) {
    B.on_disk_mtime = ost.st_mtim;
    B.have_disk_mtime = 1;
  }
  fclose(fp);
  B.dirty = 0;
  saved_snapshot_take();
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
  if (stat(B.recovery_path, &st) == -1 || !S_ISREG(st.st_mode)) return;

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

  FILE *fp = fopen(B.recovery_path, "r");
  if (fp == NULL) {
    editor_set_status_message("Restore failed: %s", strerror(errno));
    return;
  }
  int i;
  for (i = 0; i < row_count(); i++) editor_free_row(row_at(i));
  B.numrows = 0;
  CUR.cx = 0;
  CUR.rx = 0;
  CUR.cy = 0;
  V.drowoff = 0;
  V.coloff = 0;
  CUR.sel_active = 0;
  if (editor_fill_rows(fp) == -1) {
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
