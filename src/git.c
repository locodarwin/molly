/* git.c -- what git knows about the file being edited.
 *
 * Three things, all by asking git rather than reading .git: which lines
 * differ from the last commit (marks in a sign column beside the text), who
 * last changed this line (blame), and the file's history (a buffer of its
 * commits, each of which opens as its diff). Every one goes through
 * filter_run(), so a slow repository is bounded by the shell timeout and Esc,
 * as find in files is, and git missing is git saying nothing.
 *
 * The marks compare the *buffer* with HEAD, not the file on disk: the buffer
 * is sent to `diff` as standard input against `git show HEAD:file`, so an
 * unsaved edit shows as changed the moment the keyboard has been quiet for a
 * second and a half. A file git does not track has no sign column at all,
 * and one that it does always has one -- a column that appeared with the
 * first edit would move the text sideways under the cursor. */

#include "molly.h"

#define GIT_IDLE_MS 1500

static int git_gutter = 1;

int git_gutter_on(void) { return git_gutter; }
void git_set_gutter(int on) {
  git_gutter = on ? 1 : 0;
  screen_force_repaint();
}

/* Does this view draw a sign column? */
int git_signs(void) {
  return git_gutter && V.buf->git_tracked && V.buf->tty == NULL;
}

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* `s` single-quoted for sh, into dst. 0 if it does not fit. */
static int quote(char *dst, size_t n, const char *s) {
  size_t o = 0;
  if (n < 3) return 0;
  dst[o++] = '\'';
  for (; *s; s++) {
    if (*s == '\'') {
      if (o + 5 >= n) return 0;
      memcpy(dst + o, "'\\''", 4);
      o += 4;
    } else {
      if (o + 2 >= n) return 0;
      dst[o++] = *s;
    }
  }
  dst[o++] = '\'';
  dst[o] = '\0';
  return 1;
}

/* The file's directory and its name in it, quoted for sh; 0 for a buffer
 * with no file. */
static int where(const struct buffer *b, char *qdir, char *qbase, size_t n) {
  char dir[4096];
  const char *slash;
  if (b->filename == NULL) return 0;
  slash = strrchr(b->filename, '/');
  if (slash == NULL) snprintf(dir, sizeof(dir), ".");
  else if (slash == b->filename) snprintf(dir, sizeof(dir), "/");
  else snprintf(dir, sizeof(dir), "%.*s", (int)(slash - b->filename), b->filename);
  return quote(qdir, n, dir) &&
         quote(qbase, n, slash != NULL ? slash + 1 : b->filename);
}

/* Run a command for git, waiting for it; NULL with nothing to say on any
 * failure. The caller frees the output. */
static char *run_git(const char *cmd, const char *in, int inlen, int *status) {
  struct filter_io io;
  memset(&io, 0, sizeof(io));
  io.cmd = cmd;
  io.in = in;
  io.inlen = inlen;
  io.timeout_ms = shell_timeout_ms();
  io.max_out = shell_output_max();
  filter_run(&io, input_escape_pressed);
  if (status) *status = io.status;
  if (io.signalled || io.out == NULL) {
    filter_free(&io);
    return NULL;
  }
  return io.out;
}

/* ---- the marks ------------------------------------------------------------ */

/* Which rows differ from HEAD, worked out again. Called when the file is
 * opened and saved, and when the keyboard has paused after an edit. */
void git_refresh(struct buffer *b) {
  char qd[4200], qb[4200], *cmd, *out;
  struct abuf text = ABUF_INIT;
  int i, status = 0;
  size_t n;

  b->git_edits = b->edits;
  free(b->gitmark);
  b->gitmark = NULL;
  b->ngitmark = 0;
  if (!git_gutter || b->filename == NULL || b->tty != NULL || !where(b, qd, qb, sizeof(qd))) {
    b->git_tracked = 0;
    return;
  }
  for (i = 0; i < b->numrows; i++) {
    erow *r = buf_row(b, i);
    ab_append(&text, r->chars, r->size);
    if (i + 1 < b->numrows || b->final_newline) ab_append(&text, "\n", 1);
  }
  /* Untracked is exit 4 and no marks at all; tracked but not yet in HEAD
   * (added, not committed) is exit 3, every row new. -U0, so a hunk's
   * header says exactly which rows. */
  n = strlen(qd) + strlen(qb) * 3 + 512;
  cmd = xmalloc(n);
  snprintf(cmd, n,
           "cd %s 2>/dev/null || exit 4; "
           "git ls-files --error-unmatch -- %s >/dev/null 2>&1 || exit 4; "
           "t=$(mktemp) || exit 5; "
           "git show HEAD:./%s >\"$t\" 2>/dev/null || { rm -f \"$t\"; exit 3; }; "
           "diff -U0 \"$t\" -; s=$?; rm -f \"$t\"; exit $s",
           qd, qb, qb);
  out = run_git(cmd, text.b != NULL ? text.b : "", text.len, &status);
  free(cmd);
  free(text.b);

  b->git_tracked = (status == 0 || status == 1 || status == 3);
  if (!b->git_tracked) {
    free(out);
    return;
  }
  b->ngitmark = b->numrows;
  b->gitmark = xmalloc((size_t)(b->numrows > 0 ? b->numrows : 1));
  memset(b->gitmark, 0, (size_t)(b->numrows > 0 ? b->numrows : 1));
  if (status == 3) {
    memset(b->gitmark, GIT_ADDED, (size_t)b->numrows);
  } else if (status == 1 && out != NULL) {
    const char *p = out;
    while ((p = strstr(p, "\n@@ -")) != NULL) {
      long a, bl = 1, c, d = 1;
      char *e;
      p += 5;
      a = strtol(p, &e, 10);
      if (*e == ',') bl = strtol(e + 1, &e, 10);
      (void)a;
      if (*e != ' ' || e[1] != '+') continue;
      c = strtol(e + 2, &e, 10);
      if (*e == ',') d = strtol(e + 1, &e, 10);
      if (d == 0) {
        /* Lines went after row c (1-based): the row after the gap says so. */
        long y = c < b->numrows ? c : b->numrows - 1;
        if (y >= 0 && b->gitmark[y] == 0) b->gitmark[y] = GIT_DELETED;
      } else {
        long y;
        for (y = c - 1; y < c - 1 + d && y < b->numrows; y++)
          if (y >= 0) b->gitmark[y] = bl == 0 ? GIT_ADDED : GIT_CHANGED;
      }
    }
  }
  free(out);
}

/* From the idle tick: once an edit has been left alone long enough, the
 * marks are worked out again for the buffer on screen. */
void git_idle(void) {
  static unsigned seen;
  static long since;
  struct buffer *b = V.buf;
  if (!git_gutter || !b->git_tracked) return;
  if (b->edits == b->git_edits) return;
  if (b->edits != seen) {
    seen = b->edits;
    since = now_ms();
    return;
  }
  if (now_ms() - since < GIT_IDLE_MS) return;
  git_refresh(b);
  ui_request_redraw();
}

/* The sign for row y: GIT_ADDED, GIT_CHANGED, GIT_DELETED or 0. */
int git_mark(const struct buffer *b, int y) {
  if (b->gitmark == NULL || y < 0 || y >= b->ngitmark) return 0;
  return b->gitmark[y];
}

/* ---- blame and history ------------------------------------------------------ */

/* Who last changed the cursor's line, when, and why: on the message bar. */
void git_blame_line(void) {
  char qd[4200], qb[4200], *cmd, *out, hash[16] = "", author[80] = "", summary[120] = "";
  const char *p;
  long when = 0;
  size_t n;
  if (V.buf->dirty) {
    editor_set_status_message("Save first: blame is of the file as it is on disk.");
    return;
  }
  if (!where(V.buf, qd, qb, sizeof(qd))) {
    editor_set_status_message("Not a file git knows about.");
    return;
  }
  n = strlen(qd) + strlen(qb) + 128;
  cmd = xmalloc(n);
  snprintf(cmd, n, "cd %s && git blame --porcelain -L %d,%d -- %s 2>/dev/null",
           qd, CUR.cy + 1, CUR.cy + 1, qb);
  out = run_git(cmd, "", 0, NULL);
  free(cmd);
  if (out == NULL || out[0] == '\0') {
    editor_set_status_message("git has nothing to say about this line.");
    free(out);
    return;
  }
  snprintf(hash, sizeof(hash), "%.8s", out);
  if ((p = strstr(out, "\nauthor ")) != NULL)
    snprintf(author, sizeof(author), "%.*s", (int)strcspn(p + 8, "\n"), p + 8);
  if ((p = strstr(out, "\nauthor-time ")) != NULL) when = strtol(p + 13, NULL, 10);
  if ((p = strstr(out, "\nsummary ")) != NULL)
    snprintf(summary, sizeof(summary), "%.*s", (int)strcspn(p + 9, "\n"), p + 9);
  free(out);
  if (strncmp(hash, "00000000", 8) == 0) {
    editor_set_status_message("Line %d: not committed yet.", CUR.cy + 1);
  } else {
    char date[16] = "";
    time_t t = (time_t)when;
    struct tm *tm = gmtime(&t);
    if (tm != NULL) strftime(date, sizeof(date), "%Y-%m-%d", tm);
    editor_set_status_message("%s %s, %s: %s", hash, author, date, summary);
  }
}

/* A buffer of git's output, read-only, titled `title`, shown in a window
 * below. Loaded like a file, so it is never unsaved work. */
static void show_output(const char *title, const char *out, int kind,
                        const char *dir) {
  struct buffer *b = buf_new();
  const char *p = out;
  b->title = xstrdup(title);
  b->git_dir = xstrdup(dir);
  b->readonly = 1;
  b->git_view = kind;
  if (window_split(WIN_ROWS)) buf_show(buf_index_of(b));
  else buf_show(buf_index_of(b));
  while (*p != '\0') {
    size_t len = strcspn(p, "\n");
    editor_insert_row(row_count(), (char *)p, len);
    p += len;
    if (*p == '\n') p++;
  }
  editor_free_undo_redo();
  saved_snapshot_free();
  saved_snapshot_take();
  B.dirty = 0;
  memset(&CUR, 0, sizeof(CUR));
  V.drowoff = V.coloff = 0;
  screen_force_repaint();
}

/* The file's history: a commit a row, newest first. Enter opens one. */
void git_log_file(void) {
  char qd[4200], qb[4200], *cmd, *out, title[160];
  size_t n;
  if (!where(V.buf, qd, qb, sizeof(qd))) {
    editor_set_status_message("Not a file git knows about.");
    return;
  }
  n = strlen(qd) + strlen(qb) + 256;
  cmd = xmalloc(n);
  snprintf(cmd, n, "cd %s && git log --no-color --date=short "
                   "--format='%%h %%ad %%an  %%s' -- %s 2>/dev/null", qd, qb);
  out = run_git(cmd, "", 0, NULL);
  free(cmd);
  if (out == NULL || out[0] == '\0') {
    editor_set_status_message("git has no history for this file.");
    free(out);
    return;
  }
  snprintf(title, sizeof(title), "git log: %.120s", buf_name(V.buf));
  {
    /* Enter asks for a commit from the same directory, which is kept on the
     * history's buffer rather than worked out again from a name. */
    char dir[4096];
    const char *slash = strrchr(V.buf->filename, '/');
    if (slash == NULL) snprintf(dir, sizeof(dir), ".");
    else snprintf(dir, sizeof(dir), "%.*s", (int)(slash - V.buf->filename), V.buf->filename);
    show_output(title, out, 1, dir[0] ? dir : "/");
  }
  free(out);
  editor_set_status_message("Enter shows a commit.");
}

/* Enter in a history buffer: that commit, as its diff, in a buffer of its
 * own, asked for from the directory the history was taken in. */
int git_log_enter(void) {
  char qd[4200], hash[48], *cmd, *out, title[64];
  erow *r;
  size_t n, k = 0;
  if (V.buf->git_view != 1 || CUR.cy >= row_count() || V.buf->git_dir == NULL) return 0;
  if (!quote(qd, sizeof(qd), V.buf->git_dir)) return 1;
  r = row_at(CUR.cy);
  while (k < (size_t)r->size && k + 1 < sizeof(hash) && isxdigit((unsigned char)r->chars[k])) {
    hash[k] = r->chars[k];
    k++;
  }
  hash[k] = '\0';
  if (k < 4) return 1;
  n = strlen(qd) + 128;
  cmd = xmalloc(n);
  snprintf(cmd, n, "cd %s && git show --no-color --stat -p %s 2>/dev/null", qd, hash);
  out = run_git(cmd, "", 0, NULL);
  free(cmd);
  if (out == NULL || out[0] == '\0') {
    editor_set_status_message("git could not show %s.", hash);
    free(out);
    return 1;
  }
  snprintf(title, sizeof(title), "git show %s", hash);
  show_output(title, out, 2, V.buf->git_dir);
  free(out);
  return 1;
}
