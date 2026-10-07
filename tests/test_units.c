/* test_units.c -- unit tests for the parts that are testable without a terminal.
 *
 * The editor registers exactly one source today, the keyboard, so nothing in the
 * pty tests would notice if waiting on a second one were broken. The language
 * server and the integrated terminal will both depend on it, and a bug found then
 * would look like "diagnostics sometimes do not arrive" rather than like a loop
 * bug. So the loop is tested directly, against pipes.
 *
 * Linked against molly's objects (all but main.o) because loop.c calls die() on a
 * failed wait, and die() belongs to the terminal code. */

#include "molly.h"
#include <poll.h>

static int failures;

static void check(const char *label, long got, long want) {
  if (got == want) {
    printf("  pass  %s\n", label);
  } else {
    printf("  FAIL  %s\n          want: %ld\n          got:  %ld\n",
           label, want, got);
    failures++;
  }
}

/* What the handlers record, kept in parallel arrays indexed by the small integer
 * each source is registered with -- this is a test, not an interface. */
static int fired[4];
static int bytes_read[4];
static int handler_fd[4];

static void drain_handler(void *user) {
  int which = *(int *)user;
  char buf[64];
  ssize_t n;
  fired[which]++;
  n = read(handler_fd[which], buf, sizeof(buf));
  if (n > 0) bytes_read[which] += (int)n;
}

static void test_loop_sources(void) {
  int idx[4] = {0, 1, 2, 3};
  int a[2], b[2], kb[2];
  int ida, idb, r;

  printf("\nevent loop: waiting on several sources\n");

  /* Take over standard input first. Inherited stdin may be a terminal, a pipe,
   * /dev/null or closed, and the last two poll ready immediately -- which made
   * every "nothing is waiting" assertion depend on how the test was launched.
   * A pipe nobody writes to is quiet until this test writes to it. */
  if (pipe(kb) != 0) {
    fprintf(stderr, "pipe failed\n");
    { perror("test setup"); failures++; return; }
  }
  if (dup2(kb[0], STDIN_FILENO) < 0) {
    fprintf(stderr, "dup2 failed\n");
    { perror("test setup"); failures++; return; }
  }

  check("no sources to begin with", loop_source_count(), 0);

  if (pipe(a) != 0 || pipe(b) != 0) {
    fprintf(stderr, "pipe failed\n");
    { perror("test setup"); failures++; return; }
  }
  handler_fd[0] = a[0];
  handler_fd[1] = b[0];

  ida = loop_add(a[0], drain_handler, &idx[0]);
  idb = loop_add(b[0], drain_handler, &idx[1]);
  check("two sources registered", loop_source_count(), 2);
  check("ids are distinct", ida != idb, 1);

  /* Nothing written: the wait times out, reports nothing, and calls no handler. */
  r = loop_wait(10);
  check("an idle wait reports nothing", r, 0);
  check("an idle wait calls no handler", fired[0] + fired[1], 0);

  /* One source ready. */
  if (write(a[1], "xyz", 3) != 3) { perror("test setup"); failures++; return; }
  r = loop_wait(500);
  check("one ready source is dispatched", fired[0], 1);
  check("the other source is left alone", fired[1], 0);
  check("the handler got its bytes", bytes_read[0], 3);
  check("a non-keyboard source does not report stdin", r & LOOP_STDIN, 0);

  /* Both ready in the same wait: both handlers run before loop_wait returns,
   * which is the property an editor with a language server and a terminal
   * depends on. */
  fired[0] = fired[1] = 0;
  if (write(a[1], "1", 1) != 1 || write(b[1], "22", 2) != 2) { perror("test setup"); failures++; return; }
  r = loop_wait(500);
  check("both ready sources are dispatched in one wait",
        fired[0] + fired[1], 2);
  check("each got its own bytes", bytes_read[1], 2);

  /* A removed source is no longer waited on, even with data pending. */
  fired[0] = fired[1] = 0;
  loop_remove(idb);
  check("removing drops the count", loop_source_count(), 1);
  if (write(b[1], "ignored", 7) != 7) { perror("test setup"); failures++; return; }
  r = loop_wait(50);
  check("a removed source is not dispatched", fired[1], 0);

  /* A writer that goes away must be reported, not silently waited on forever:
   * a closed pipe polls ready at EOF, and a handler never told would spin. */
  fired[0] = 0;
  close(a[1]);
  r = loop_wait(500);
  check("a closed writer wakes the handler", fired[0] > 0, 1);

  /* The table is finite and says so rather than overflowing. */
  loop_cleanup();
  check("cleanup clears the table", loop_source_count(), 0);
  {
    int i, last = 0, extra;
    for (i = 0; i < 64; i++) {
      int id = loop_add(b[0], drain_handler, &idx[1]);
      if (id < 0) break;
      last++;
    }
    extra = loop_add(b[0], drain_handler, &idx[1]);
    check("a full table refuses rather than overflows", extra, -1);
    check("the table filled to its stated size", last, loop_source_count());
  }
  loop_cleanup();

  /* Keyboard input is reported rather than dispatched. Standard input is a pipe
   * here, so writing to it is the same thing as a keystroke as far as the loop
   * is concerned. */
  {
    fired[0] = fired[1] = 0;  /* earlier cases left these set */
    r = loop_wait(10);
    check("no keyboard input reports nothing", r & LOOP_STDIN, 0);
    if (write(kb[1], "k", 1) != 1) { perror("test setup"); failures++; return; }
    r = loop_wait(500);
    check("keyboard input is reported", (r & LOOP_STDIN) != 0, 1);
    check("keyboard input is not dispatched to a handler", fired[0], 0);
  }

  close(b[0]);
  close(b[1]);
  close(a[0]);
}

/* The property step 3 of Phase 1 exists for: a dialog waiting on a keystroke must
 * not stop everything else. ui_wait_key is what every loop in the editor waits on,
 * so proving it dispatches a ready source before handing back the key proves it for
 * the prompt, the help screen and the open dialog alike. */
static void test_wait_key_services_sources(void) {
  int idx = 0;
  int src[2], kb[2];
  int key;

  printf("\nui_wait_key: sources are serviced while waiting for a key\n");

  loop_cleanup();
  fired[0] = 0;
  bytes_read[0] = 0;

  if (pipe(src) != 0 || pipe(kb) != 0) {
    perror("pipe");
    failures++;
    return;
  }
  if (dup2(kb[0], STDIN_FILENO) < 0) {
    perror("dup2");
    failures++;
    return;
  }
  handler_fd[0] = src[0];
  if (loop_add(src[0], drain_handler, &idx) < 0) {
    failures++;
    return;
  }

  /* Something arrives on the source, and a key is pressed. */
  if (write(src[1], "diag", 4) != 4 || write(kb[1], "k", 1) != 1) {
    perror("write");
    failures++;
    return;
  }

  key = ui_wait_key();
  check("the key comes back", key, 'k');
  check("the source was serviced on the way", fired[0], 1);
  check("and its bytes were delivered", bytes_read[0], 4);

  loop_cleanup();
  close(src[0]);
  close(src[1]);
  close(kb[0]);
  close(kb[1]);
}

/* ---- the grid ------------------------------------------------------------ */

/* Drive the grid and capture what it wrote.
 *
 * grid_flush writes to standard output, so pointing that at a pipe makes the
 * renderer's actual bytes testable directly, rather than only end to end through a
 * pty. The byte form matters more than it looks: a stray reset or a missing
 * attribute-off leaves the terminal in the wrong state for everything after it. */
static void grid_capture(char *out, size_t outsz, void (*paint)(void)) {
  int pipefd[2], saved;
  ssize_t n;

  out[0] = '\0';
  /* Flush before redirecting, not after: anything still sitting in stdio's buffer
   * would otherwise drain into the capture pipe and vanish from the test output.
   * Which is exactly what happened the first time this was written. */
  fflush(stdout);
  if (pipe(pipefd) != 0) { perror("pipe"); failures++; return; }
  saved = dup(STDOUT_FILENO);
  if (saved < 0 || dup2(pipefd[1], STDOUT_FILENO) < 0) {
    perror("dup2"); failures++; return;
  }

  paint();

  fflush(stdout);
  dup2(saved, STDOUT_FILENO);
  close(saved);
  close(pipefd[1]);
  n = read(pipefd[0], out, outsz - 1);
  close(pipefd[0]);
  if (n < 0) n = 0;
  out[n] = '\0';
}

static void contains(const char *label, const char *haystack, const char *needle) {
  if (strstr(haystack, needle) != NULL) {
    printf("  pass  %s\n", label);
  } else {
    printf("  FAIL  %s\n          wanted to find: %s\n          in: %s\n",
           label, needle, haystack);
    failures++;
  }
}

static void paint_plain(void) {
  grid_begin();
  grid_role(ROLE_TEXT);
  grid_puts(0, 0, "abc", 3);
  grid_flush();
}

static void paint_gutter_role(void) {
  grid_begin();
  grid_role(ROLE_GUTTER);
  grid_puts(0, 0, "abc", 3);
  grid_flush();
}

static void paint_bold(void) {
  grid_begin();
  grid_role(ROLE_TEXT);
  grid_attrs(ATTR_BOLD);
  grid_puts(0, 0, "abc", 3);
  grid_flush();
}

static void paint_underline(void) {
  grid_begin();
  grid_role(ROLE_TEXT);
  grid_attrs(ATTR_UNDERLINE);
  grid_puts(0, 0, "abc", 3);
  grid_flush();
}

static void paint_shaded(void) {
  grid_begin();
  grid_role(ROLE_TEXT);
  grid_puts(0, 0, "abc", 3);
  /* What a drop shadow does: restyle without disturbing the text. */
  grid_shade(0, 0, 3, 1, ROLE_GUTTER);
  grid_flush();
}

static void test_grid_emission(void) {
  char out[4096];

  printf("\ngrid: what reaches the terminal\n");

  grid_resize(4, 20);

  grid_capture(out, sizeof(out), paint_plain);
  contains("plain text is positioned and cleared to EOL", out, "\x1b[1;1Habc\x1b[K");

  grid_capture(out, sizeof(out), paint_gutter_role);
  contains("a colored role wraps the run and resets after it", out,
           "\x1b[1;1H\x1b[90mabc\x1b[m\x1b[K");

  grid_capture(out, sizeof(out), paint_bold);
  contains("bold turns on and off again", out, "\x1b[1mabc\x1b[22m");

  grid_capture(out, sizeof(out), paint_underline);
  contains("underline turns on and off again", out, "\x1b[4mabc\x1b[24m");

  /* The shadow's point: the text is still there, in a different role. */
  grid_capture(out, sizeof(out), paint_shaded);
  contains("shading restyles without disturbing the text", out,
           "\x1b[90mabc\x1b[m");

  grid_cleanup();
}

/* ---- the theme ----------------------------------------------------------- */

static void test_theme_depth(void) {
  printf("\ntheme: how many colors the terminal admits to\n");

  check("a 256 in the name is enough",
        theme_detect_depth("xterm-256color", NULL), 256);
  check("so is screen-256color",
        theme_detect_depth("screen-256color", NULL), 256);
  check("and a direct-color name",
        theme_detect_depth("xterm-direct", NULL), 256);
  check("plain xterm is sixteen", theme_detect_depth("xterm", NULL), 16);
  check("vt100 is sixteen", theme_detect_depth("vt100", NULL), 16);
  check("so is a terminal that will not say",
        theme_detect_depth(NULL, NULL), 16);
  check("an empty name is sixteen", theme_detect_depth("", ""), 16);

  /* COLORTERM is the stronger signal: a terminal may support direct color while
   * its TERM says otherwise, which is the usual state of affairs inside tmux. */
  check("COLORTERM=truecolor overrides the name",
        theme_detect_depth("xterm", "truecolor"), 256);
  check("COLORTERM=24bit does too",
        theme_detect_depth("xterm", "24bit"), 256);

  /* A few terminals do 256 colors without advertising it in TERM. */
  check("kitty is known", theme_detect_depth("xterm-kitty", NULL), 256);
  check("alacritty is known", theme_detect_depth("alacritty", NULL), 256);
  check("foot is known", theme_detect_depth("foot", NULL), 256);
}

static void test_theme_styles(void) {
  struct style st;

  printf("\ntheme: roles resolve to styles\n");

  /* Both tables resolve to what olly drew, so these hold at either depth. */
  theme_style(ROLE_TEXT, &st);
  check("text is unstyled", style_is_default(&st), 1);

  theme_style(ROLE_GUTTER, &st);
  check("the gutter is palette 8", st.fg, 8);
  check("the gutter leaves the background alone", st.bg, -1);
  check("the gutter is not reverse", st.reverse, 0);

  theme_style(ROLE_SELECTION, &st);
  check("selection is reverse video", st.reverse, 1);
  check("selection takes no color", st.fg, -1);

  theme_style(ROLE_STATUS, &st);
  check("the status bar is reverse video", st.reverse, 1);

  theme_style(ROLE_COUNT + 7, &st);
  check("a role out of range falls back to text", style_is_default(&st), 1);
}

/* The byte forms matter: keeping 0-15 on the short SGR codes is what makes a gray
 * gutter "ESC[90m" whichever theme is loaded, and so what kept every byte molly
 * emitted before it had colors. */
static void emitted(const char *label, int color, int is_bg, const char *want) {
  struct abuf ab = ABUF_INIT;
  char got[64];
  style_emit_color(&ab, color, is_bg);
  snprintf(got, sizeof(got), "%.*s", ab.len, ab.b ? ab.b : "");
  if (strcmp(got, want) == 0) {
    printf("  pass  %s\n", label);
  } else {
    printf("  FAIL  %s\n          want: %s\n          got:  %s\n",
           label, want, got);
    failures++;
  }
  free(ab.b);
}

static void test_theme_color_bytes(void) {
  printf("\ntheme: colors emit the shortest form a terminal understands\n");

  emitted("palette 1 is a plain foreground", 1, 0, "\x1b[31m");
  emitted("palette 8 is a bright foreground", 8, 0, "\x1b[90m");
  emitted("palette 15 is the last bright one", 15, 0, "\x1b[97m");
  emitted("palette 16 needs the cube", 16, 0, "\x1b[38;5;16m");
  emitted("palette 244 needs the cube", 244, 0, "\x1b[38;5;244m");
  emitted("palette 2 as a background", 2, 1, "\x1b[42m");
  emitted("palette 9 as a bright background", 9, 1, "\x1b[101m");
  emitted("palette 200 as a cube background", 200, 1, "\x1b[48;5;200m");
  emitted("the default color emits nothing", -1, 0, "");
}

/* --- the command palette's matcher ---------------------------------------- *
 *
 * Checked here rather than through the pty because what is being tested is an
 * ordering, and an ordering is far easier to be sure of as a pair of scores than as
 * a screenful of list rows. The pty tests check that the ordering reaches the
 * screen; these check that it is the right one. */

static void better(const char *label, const char *pat, const char *win,
                   const char *lose) {
  int a = palette_score(win, pat), b = palette_score(lose, pat);
  if (a > b) {
    printf("  pass  %s\n", label);
  } else {
    printf("  FAIL  %s\n          \"%s\" scored %d, \"%s\" scored %d\n",
           label, win, a, lose, b);
    failures++;
  }
}

static void test_palette_matcher(void) {
  printf("\npalette: the fuzzy matcher's ranking\n");

  /* A subsequence matches; anything else does not. */
  check("an empty pattern matches everything", palette_score("Save", "") > 0, 1);
  check("a prefix matches", palette_score("Save", "sav") > 0, 1);
  check("a subsequence matches", palette_score("Line numbers", "lnm") > 0, 1);
  check("case is ignored", palette_score("Save", "SAV") > 0, 1);
  check("out of order does not match", palette_score("Save", "vas"), 0);
  check("a letter that is not there does not match",
        palette_score("Save", "sax"), 0);
  check("a pattern longer than the text does not match",
        palette_score("Cut", "cutting"), 0);

  /* The ranking, which is the part that decides how the palette feels. */
  better("a prefix beats a match halfway along", "sav", "Save", "Also saved");
  better("a word start beats a letter stranded mid-word",
         "ln", "Line numbers", "Line end");
  better("contiguous beats scattered", "red", "Redraw", "Reorder dates");
  better("the whole pattern at a word start wins", "mc", "Match case", "Mimic");

  /* The second pass must not lose a match the first one found: preferring a
   * word-start 'b' walks past the only 'a' that could follow it. */
  check("preferring a word start cannot lose a match",
        palette_score("ab a b", "aba") > 0, 1);
}

/* --- the input field ------------------------------------------------------ *
 *
 * The prompt and the palette both drive this, and the pty tests cover what that
 * looks like. What they cannot easily reach is the state after a sequence of edits
 * in the middle of a multi-byte string, which is exactly where an off-by-one would
 * leave invalid bytes behind. */

static void typed(struct widget *w, const char *s) {
  const unsigned char *p = (const unsigned char *)s;
  for (; *p != '\0'; p++) widget_input_key(w, *p);
}

static void test_input_field(void) {
  struct widget w;
  printf("\ninput field: editing stays on character boundaries\n");

  widget_input(&w, 0, 0, 20);
  typed(&w, "hello");
  check("typing accumulates", strcmp(widget_input_text(&w), "hello"), 0);
  check("the cursor is at the end", w.icur, 5);

  widget_input_key(&w, BACKSPACE);
  check("backspace deletes one character", strcmp(widget_input_text(&w), "hell"), 0);

  widget_input_key(&w, HOME_KEY);
  check("home goes to the start", w.icur, 0);
  widget_input_key(&w, DEL_KEY);
  check("delete takes the character at the cursor",
        strcmp(widget_input_text(&w), "ell"), 0);
  widget_input_key(&w, END_KEY);
  check("end goes to the finish", w.icur, 3);

  /* A key the field has no use for is handed back, which is how the palette can
   * give the arrows to its list and the text to the field. */
  check("an unknown key is not consumed", widget_input_key(&w, F5_KEY), 0);
  check("and the text is untouched", strcmp(widget_input_text(&w), "ell"), 0);
  check("a control byte is not consumed", widget_input_key(&w, 1), 0);
  widget_input_free(&w);

  /* Multi-byte characters. Each arrives as its bytes, one key at a time, and the
   * cursor must step over the whole character afterwards. */
  widget_input(&w, 0, 0, 20);
  typed(&w, "caf\xc3\xa9x");
  check("a multi-byte character reassembles",
        strcmp(widget_input_text(&w), "caf\xc3\xa9x"), 0);
  widget_input_key(&w, BACKSPACE);
  widget_input_key(&w, BACKSPACE);
  check("backspace eats the whole character, not one byte",
        strcmp(widget_input_text(&w), "caf"), 0);

  typed(&w, "\xc3\xa9\xc3\xa8");
  widget_input_key(&w, ARROW_LEFT);
  check("left steps back over a whole character", w.icur, 5);
  widget_input_key(&w, ARROW_LEFT);
  check("and over the one before it", w.icur, 3);
  widget_input_key(&w, ARROW_RIGHT);
  check("right steps forward over a whole character", w.icur, 5);
  widget_input_key(&w, DEL_KEY);
  check("delete removes the whole character",
        strcmp(widget_input_text(&w), "caf\xc3\xa9"), 0);
  widget_input_free(&w);

  /* The buffer grows: nothing caps what can be pasted into a prompt. */
  widget_input(&w, 0, 0, 20);
  {
    int i;
    for (i = 0; i < 5000; i++) widget_input_key(&w, 'x');
  }
  check("the buffer grows past its initial size", w.ilen, 5000);
  check("and stays terminated", (int)strlen(widget_input_text(&w)), 5000);
  widget_input_free(&w);
  check("freeing drops the buffer", w.ibuf == NULL, 1);
}


int main(void) {
  test_loop_sources();
  test_wait_key_services_sources();
  test_grid_emission();
  test_theme_depth();
  test_theme_styles();
  test_theme_color_bytes();
  test_palette_matcher();
  test_input_field();

  printf("\n%s\n", failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
