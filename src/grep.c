/* grep.c -- find in files, and a buffer of what was found.
 *
 * The search is not molly's. `git grep` inside a work tree and `grep -r`
 * outside one already know how to walk a tree, skip binaries and respect a
 * .gitignore, and filter_run() already knows how to run a command without
 * letting it hang the editor -- the timeout, the output cap and Esc all come
 * with it. What is left is the part only an editor has: a place to put the
 * answer and a way to get from a line of it to the file it names.
 *
 * The place is a buffer, like any other, with one row per match exactly as
 * grep printed it: `path:line:text`. It can be scrolled, searched, split and
 * even saved, because it is text, and nothing about it needs a widget of its
 * own. Enter on a row opens that file at that line; F4 and Shift-F4 step
 * through the rows from wherever the keyboard is.
 *
 * It is filled the way a file is loaded rather than the way text is typed --
 * rows in, no undo, a saved snapshot taken afterwards -- so it is not
 * "modified" and quitting does not ask about it. Edit it and it is, like any
 * buffer: that is honest, and it is the user's to throw away.
 *
 * The pattern is read the way Find reads one: plain text unless pattern search
 * is on, and folded unless Match case is. Pattern search hands grep -E, whose
 * dialect is close to molly's but not the same, so where they disagree the
 * file list is grep's and the column the cursor lands on is molly's. */

#include "molly.h"

/* Bytes of a matched line kept in the results. A minified file is one line of
 * a megabyte, and a results row that long is a row nobody reads and every
 * redraw measures. */
#define GREP_LINE_MAX 400

/* ---- the command ----------------------------------------------------------- */

/* `s` inside single quotes for sh, where nothing is special but the quote
 * itself, which is closed, escaped and reopened. Returns 0 if it would not
 * fit. */
static int sh_quote(char *dst, size_t dsz, const char *s) {
  size_t n = 0;
  if (dsz < 3) return 0;
  dst[n++] = '\'';
  for (; *s != '\0'; s++) {
    if (*s == '\'') {
      if (n + 4 >= dsz) return 0;
      memcpy(dst + n, "'\\''", 4);
      n += 4;
    } else {
      if (n + 1 >= dsz) return 0;
      dst[n++] = *s;
    }
  }
  if (n + 2 > dsz) return 0;
  dst[n++] = '\'';
  dst[n] = '\0';
  return 1;
}

/* The command line for a search, or NULL if the pattern is too long to quote.
 * The caller frees it.
 *
 * One line for sh rather than a choice made here, because whether the
 * directory is in a work tree is the shell's question to answer in the
 * directory it runs in. -I skips binaries in both; -n is the line number the
 * jump needs; --no-color because a pager setting must not put escape codes in
 * the buffer. grep's own tree walk leaves .git out by hand, which git grep
 * never looks in. Pure over its arguments, so it is unit-tested. */
char *grep_command(const char *pat, int icase, int regex) {
  char q[1024];
  char *cmd;
  const char *flags;
  size_t n;

  if (!sh_quote(q, sizeof(q), pat)) return NULL;
  flags = regex ? (icase ? "-i -E" : "-E") : (icase ? "-i -F" : "-F");
  n = strlen(q) * 2 + 256;
  cmd = xmalloc(n);
  snprintf(cmd, n,
           "if git rev-parse --is-inside-work-tree >/dev/null 2>&1; "
           "then git grep -n -I --no-color %s -e %s; "
           "else grep -rnI --exclude-dir=.git %s -e %s .; fi",
           flags, q, flags, q);
  return cmd;
}

/* Split a results row into its path and line number. Returns 1 and fills the
 * three outputs, or 0 for a row that is not a match.
 *
 * `path:line:text`, where the path may itself hold a colon. The first colon
 * followed by digits and another colon ends it -- which a path could still
 * contain, as `a:1:b/c.txt`, and then the row is misread; grep's -Z would
 * settle it, but git grep and grep put the NUL in different places, and a
 * path like that is rarer than the second dialect is worth. A leading "./",
 * which grep -r puts on every path, is not part of it. */
int grep_parse_line(const char *s, int len, int *path0, int *pathlen, int *line) {
  int i, j, n;

  for (i = 0; i < len; i++) {
    if (s[i] != ':' || i == 0) continue;
    n = 0;
    for (j = i + 1; j < len && s[j] >= '0' && s[j] <= '9'; j++) {
      if (n < 100000000) n = n * 10 + (s[j] - '0');
    }
    if (j == i + 1 || j >= len || s[j] != ':' || n < 1) continue;
    *path0 = (i >= 2 && s[0] == '.' && s[1] == '/') ? 2 : 0;
    *pathlen = i - *path0;
    *line = n;
    return *pathlen > 0;
  }
  return 0;
}

/* ---- the results ----------------------------------------------------------- */

/* The pattern the results were made from, for the column a jump lands on, and
 * the row of them that was last jumped to, for F4 to go on from. */
static char *grep_query;
static int grep_cur = -1;

/* The column each row's match is at, when whoever filled the list knew it --
 * the language server's references do, and two on one line would otherwise
 * both land on the first. NULL for find in files, which searches the line for
 * the query instead. Only trusted while the list has as many rows as it was
 * given, because a list the user has edited no longer lines up. */
static int *result_cols;
static int nresult_cols;

static struct buffer *results_buffer(void) {
  int i;
  for (i = 0; i < buf_count(); i++)
    if (buf_at(i)->grep_results) return buf_at(i);
  return NULL;
}

/* Rows in the results buffer, which is the count of matches; 0 with none. */
int grep_result_count(void) {
  struct buffer *b = results_buffer();
  return b != NULL ? b->numrows : 0;
}

/* Replace the current buffer's rows with grep's output, a row per line, less
 * the "./" grep -r puts in front of every path -- it is noise on every row, and
 * it makes the two greps' results read differently for no reason. */
static void fill(const char *out, int len) {
  int i = 0;
  while (row_count() > 0) editor_delete_row(row_count() - 1);
  while (i < len) {
    int e = i, keep;
    while (e < len && out[e] != '\n') e++;
    if (e - i >= 2 && out[i] == '.' && out[i + 1] == '/') i += 2;
    keep = e - i;
    if (keep > GREP_LINE_MAX) keep = GREP_LINE_MAX;
    editor_insert_row(row_count(), (char *)out + i, (size_t)keep);
    i = e + 1;
  }
  editor_free_undo_redo();
  saved_snapshot_free();
  saved_snapshot_take();
  B.dirty = 0;
  memset(&CUR, 0, sizeof CUR);
  V.drowoff = 0;
  V.coloff = 0;
}

/* Files named in the results, counted as runs: grep prints a file's matches
 * together, so a change of path is a new file. */
static int count_files(void) {
  int i, files = 0, p0, pl, ln, lp0 = 0, lpl = -1;
  const char *last = NULL;
  for (i = 0; i < row_count(); i++) {
    erow *r = row_at(i);
    if (!grep_parse_line(r->chars, r->size, &p0, &pl, &ln)) continue;
    if (last == NULL || pl != lpl || memcmp(last + lp0, r->chars + p0, (size_t)pl)) {
      files++;
      last = r->chars;
      lp0 = p0;
      lpl = pl;
    }
  }
  return files;
}

/* Put the results on screen: the window already showing them, or a new one
 * below the current window, or -- with no room to split -- the current one.
 * The keyboard ends up in it, on the first match, because the next thing
 * anyone does with a list of matches is choose one. */
static void show_results(struct buffer *b) {
  int i;
  for (i = 0; i < window_count(); i++) {
    if (window_view_at(i)->buf == b) {
      window_focus(i);
      return;
    }
  }
  window_split(WIN_ROWS);
  buf_show(buf_index_of(b));
}

/* Ctrl-E: ask for a pattern, run the search, and show what it found. */
void editor_grep(void) {
  static char last[256];
  struct filter_io io;
  char *input, *cmd;
  char pat[256];
  int result, files, n;

  input = editor_prompt("Find in files: %s (ESC to cancel)", NULL);
  if (input == NULL) return;
  if (input[0] == '\0') {
    free(input);
    if (last[0] == '\0') return;
    snprintf(pat, sizeof(pat), "%s", last);
  } else {
    snprintf(pat, sizeof(pat), "%s", input);
    free(input);
  }

  cmd = grep_command(pat, !search_case_is_on(), search_regex_is_on());
  if (cmd == NULL) {
    editor_set_status_message("That pattern is too long to search for.");
    return;
  }
  editor_set_status_message("Searching for %.40s ... (ESC cancels)", pat);
  editor_refresh_screen();

  memset(&io, 0, sizeof(io));
  io.cmd = cmd;
  io.in = "";
  io.inlen = 0;
  io.timeout_ms = shell_timeout_ms();
  io.max_out = shell_output_max();
  result = filter_run(&io, input_escape_pressed);
  free(cmd);

  /* Both greps exit 1 for "nothing matched" and 2 for an error, which is the
   * one non-zero status that is an answer rather than a failure. filter_run()
   * puts "exited 1" where the first line of standard error would go when there
   * was none, so that is what a quiet exit 1 looks like; a grep that wrote
   * something to standard error and exited 1 is still a failure. */
  if (result == FILTER_FAILED && io.status == 1 && io.signaled == 0 &&
      io.outlen == 0 && strcmp(io.err, "exited 1") == 0) {
    snprintf(last, sizeof(last), "%s", pat);
    editor_set_status_message("No matches for %.60s", pat);
    filter_free(&io);
    return;
  }
  if (result != FILTER_OK) {
    editor_set_status_message("Find in files: %s",
                              io.err[0] != '\0' ? io.err : "did not run");
    filter_free(&io);
    return;
  }
  snprintf(last, sizeof(last), "%s", pat);

  n = (int)strlen(pat) + 16;
  {
    char *title = xmalloc((size_t)n);
    snprintf(title, (size_t)n, "grep: %s", pat);
    files = grep_show_list(title, io.out, io.outlen, pat, NULL, 0);
    free(title);
  }
  filter_free(&io);
  editor_set_status_message("%d match%s in %d file%s for %.40s -- Enter opens one, F4 the next",
                            row_count(), row_count() == 1 ? "" : "es",
                            files, files == 1 ? "" : "s", pat);
}

/* Put `path:line:text` rows in the results buffer, titled `title`, and show
 * it -- whoever found them. Find in files is one finder and the language
 * server's references are another; Enter and F4 do not care which. `query`
 * is what the cursor looks for on the line it lands on. Returns how many
 * files the rows name. */
int grep_show_list(const char *title, const char *text, int len, const char *query,
                   const int *cols, int ncols) {
  struct buffer *b = results_buffer();
  int files;
  if (b == NULL) {
    b = buf_new();
    b->grep_results = 1;
  }
  free(b->title);
  b->title = xstrdup(title);
  free(grep_query);
  grep_query = xstrdup(query);
  grep_cur = -1;
  free(result_cols);
  result_cols = NULL;
  nresult_cols = 0;
  if (cols != NULL && ncols > 0) {
    result_cols = xmalloc(sizeof(*result_cols) * (size_t)ncols);
    memcpy(result_cols, cols, sizeof(*result_cols) * (size_t)ncols);
    nresult_cols = ncols;
  }

  show_results(b);
  fill(text, len);
  files = count_files();
  screen_force_repaint();
  return files;
}

/* Go to a match. `delta` 0 is the row the cursor is on in the results (Enter);
 * 1 and -1 are the next and previous from the last one gone to (F4,
 * Shift-F4), from any window. Returns 1 if it went somewhere.
 *
 * The file opens in a window other than the results' when there is one -- the
 * previous window, which is the one the search was run from when the results
 * opened below it -- so the list stays up to go on from. With only one window
 * the file takes it, and F4 still works from there. */
int grep_jump(int delta) {
  struct buffer *b = results_buffer();
  int i, at, p0, pl, ln, col, total;
  char *path;
  erow *r;

  if (b == NULL || b->numrows == 0) {
    editor_set_status_message("No find-in-files results. Ctrl-E searches.");
    return 0;
  }
  total = b->numrows;

  if (delta == 0) {
    if (V.buf != b) return 0;
    at = CUR.cy;
    if (at >= total) at = total - 1;
  } else {
    at = grep_cur + delta;
    if (grep_cur < 0 && delta < 0) at = total - 1;
    if (at < 0 || at >= total) {
      editor_set_status_message(delta > 0 ? "That was the last match."
                                          : "That was the first match.");
      return 0;
    }
  }

  /* A row that does not parse -- the user edited the list -- is skipped in the
   * direction of travel, and is a refusal for Enter. */
  for (;;) {
    r = buf_row(b, at);
    if (grep_parse_line(r->chars, r->size, &p0, &pl, &ln)) break;
    if (delta == 0) {
      editor_set_status_message("Not a match: this row names no file and line.");
      return 0;
    }
    at += delta;
    if (at < 0 || at >= total) {
      editor_set_status_message(delta > 0 ? "That was the last match."
                                          : "That was the first match.");
      return 0;
    }
  }
  grep_cur = at;
  path = xmalloc((size_t)pl + 1);
  memcpy(path, r->chars + p0, (size_t)pl);
  path[pl] = '\0';

  /* Every window on the results follows, so the list shows where F4 is. */
  for (i = 0; i < window_count(); i++) {
    struct view *v = window_view_at(i);
    if (v->buf != b) continue;
    if (v == E.curview) {
      CUR.cy = at;
      CUR.cx = 0;
      CUR.sel_active = 0;
    } else {
      struct view *was = view_select(v);
      CUR.cy = at;
      CUR.cx = 0;
      CUR.sel_active = 0;
      view_select(was);
    }
  }

  if (V.buf == b && window_count() > 1) window_move_focus(-1);
  nav_push();
  if (!editor_open_in_new_buffer(path)) return 0;

  CUR.cy = (ln - 1 < row_count()) ? ln - 1 : (row_count() > 0 ? row_count() - 1 : 0);
  if (result_cols != NULL && nresult_cols == total && CUR.cy < row_count())
    col = result_cols[at] <= row_at(CUR.cy)->size ? result_cols[at] : -1;
  else
    col = search_find_in_row(CUR.cy, grep_query);
  /* The column was found when the list was made; the file may have been
   * edited since, which cursor_place() allows for. */
  cursor_place(CUR.cy, col >= 0 ? col : 0);
  editor_set_status_message("Match %d of %d: %.*s:%d", at + 1, total,
                            pl > 50 ? 50 : pl, r->chars + p0, ln);
  return 1;
}

void grep_cleanup(void) {
  free(grep_query);
  grep_query = NULL;
  free(result_cols);
  result_cols = NULL;
  nresult_cols = 0;
  grep_cur = -1;
}
