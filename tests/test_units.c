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

static void check_str(const char *label, const char *got, const char *want) {
  if (strcmp(got, want) == 0) {
    printf("  pass  %s\n", label);
  } else {
    printf("  FAIL  %s\n          want: %s\n          got:  %s\n",
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

  ida = loop_add(a[0], LOOP_READ, drain_handler, &idx[0]);
  idb = loop_add(b[0], LOOP_READ, drain_handler, &idx[1]);
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
      int id = loop_add(b[0], LOOP_READ, drain_handler, &idx[1]);
      if (id < 0) break;
      last++;
    }
    extra = loop_add(b[0], LOOP_READ, drain_handler, &idx[1]);
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
  if (loop_add(src[0], LOOP_READ, drain_handler, &idx) < 0) {
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

static void paint_comment(void) {
  grid_begin();
  grid_role(ROLE_SYN_COMMENT);
  grid_puts(0, 0, "abc", 3);
  grid_flush();
}

/* A comment straight into a string: both have a colour of their own, so the
 * change is not a reset, and italic has to be turned off by name. */
static void paint_comment_then_string(void) {
  grid_begin();
  grid_role(ROLE_SYN_COMMENT);
  grid_puts(0, 0, "ab", 2);
  grid_role(ROLE_SYN_STRING);
  grid_puts(0, 2, "cd", 2);
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

  /* The sixteen-colour table, which is the one loaded when theme_init has not
   * run: a comment is bright black and italic. */
  theme_set_italic(1);
  grid_capture(out, sizeof(out), paint_comment);
  contains("a comment is italic", out, "\x1b[1;1H\x1b[90m\x1b[3mabc\x1b[m\x1b[K");
  grid_capture(out, sizeof(out), paint_comment_then_string);
  contains("italic is turned off by name when no reset does it", out,
           "\x1b[3mab\x1b[33m\x1b[23mcd");
  theme_set_italic(0);
  grid_capture(out, sizeof(out), paint_comment);
  contains("with the setting off, a comment is only its colour", out,
           "\x1b[1;1H\x1b[90mabc\x1b[m\x1b[K");
  theme_set_italic(1);

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

  /* Italic belongs to comments and to nothing else. */
  theme_set_italic(1);
  theme_style(ROLE_SYN_COMMENT, &st);
  check("a comment is italic", st.italic, 1);
  check("an italic style is not the default", style_is_default(&st), 0);
  theme_style(ROLE_SYN_STRING, &st);
  check("a string is not italic", st.italic, 0);
  theme_style(ROLE_TEXT, &st);
  check("text is not italic", st.italic, 0);
  theme_set_italic(0);
  theme_style(ROLE_SYN_COMMENT, &st);
  check("the setting turns it off", st.italic, 0);
  check("and the setting reads back", theme_italic(), 0);
  theme_set_italic(1);
}

static void test_theme_italic(void) {
  printf("\ntheme: which terminals get italic\n");
  check("xterm draws italic", theme_detect_italic("xterm-256color"), 1);
  check("tmux's own TERM draws italic", theme_detect_italic("tmux-256color"), 1);
  check("kitty draws italic", theme_detect_italic("xterm-kitty"), 1);
  check("the Linux console does not", theme_detect_italic("linux"), 0);
  check("nor screen, which tmux shows as reverse", theme_detect_italic("screen"), 0);
  check("nor screen-256color", theme_detect_italic("screen-256color"), 0);
  check("nor screen.xterm-256color", theme_detect_italic("screen.xterm-256color"), 0);
  check("nor a vt100", theme_detect_italic("vt100"), 0);
  check("nor dumb", theme_detect_italic("dumb"), 0);
  check("nor no TERM at all", theme_detect_italic(NULL), 0);
  check("nor an empty one", theme_detect_italic(""), 0);
  check("a name that only starts like linux is fine",
        theme_detect_italic("linuxish"), 1);
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


/* --- the regex engine -------------------------------------------------------
 *
 * tests/fuzz_regex.py compares the engine against Python's re on hundreds of
 * thousands of random patterns, which is what actually establishes that matching
 * is right. What is here is the part that comparison cannot reach: the error
 * messages, the UTF-8 behavior (Python's re in bytes mode counts bytes where
 * molly counts characters), the two corners where the major engines disagree
 * with each other, and the limits that stop a backtracker from hanging. */

/* One match, rendered as a string the expectation is written as: the whole match
 * and then each group as "so,eo", or "-" for no match, "!" for the step budget
 * or the depth running out, and "E:message" for a pattern that did not
 * compile. */
static void rx_expect(const char *pat, const char *text, int start, int flags,
                      const char *want) {
  const char *err = NULL;
  struct regex *re = regex_compile(pat, flags, &err);
  struct regex_match m;
  char got[512], label[768];
  int i, r, n = 0;

  snprintf(label, sizeof label, "/%s/%s on \"%s\"%s", pat,
           (flags & REGEX_ICASE) ? "i" : "", text, start ? " resumed" : "");
  if (re == NULL) {
    snprintf(got, sizeof got, "E:%s", err ? err : "(no message)");
    check_str(label, got, want);
    return;
  }
  r = regex_exec(re, text, (int)strlen(text), start, &m);
  if (r == REGEX_TOO_HARD) {
    snprintf(got, sizeof got, "!");
  } else if (r == REGEX_NOMATCH) {
    snprintf(got, sizeof got, "-");
  } else {
    for (i = 0; i < m.ngroups && n < (int)sizeof got - 24; i++)
      n += snprintf(got + n, sizeof got - (size_t)n, "%s%d,%d",
                    i ? " " : "", m.so[i], m.eo[i]);
  }
  check_str(label, got, want);
  regex_free(re);
}

static void rx(const char *pat, const char *text, const char *want) {
  rx_expect(pat, text, 0, 0, want);
}

static void rx_i(const char *pat, const char *text, const char *want) {
  rx_expect(pat, text, 0, REGEX_ICASE, want);
}

static void rx_from(const char *pat, const char *text, int start,
                    const char *want) {
  rx_expect(pat, text, start, 0, want);
}

static void test_regex_syntax(void) {
  printf("\nregex: what a pattern means, and what it says when it does not\n");

  rx("abc", "xxabcxx", "2,5");
  rx("a.c", "abc", "0,3");
  rx("a.c", "a\nc", "-");            /* . is every character but a newline */
  rx("", "abc", "0,0");
  rx("x", "abc", "-");

  rx("^abc", "abcdef", "0,3");
  rx("^abc", "xabc", "-");
  rx("abc$", "xxabc", "2,5");
  rx("abc$", "abcx", "-");
  rx("^$", "", "0,0");

  rx("[abc]+", "xxabcaxx", "2,6");
  rx("[^abc]+", "abcxyz", "3,6");
  rx("[a-c]", "d", "-");
  rx("[]]", "]", "0,1");             /* a ] first is the character */
  rx("[a-]", "-", "0,1");            /* a - last is the character */
  rx("[\\d]+", "ab123", "2,5");
  rx("\\d+", "ab123", "2,5");
  rx("\\D+", "12ab", "2,4");
  rx("\\w+", " a_1 ", "1,4");
  rx("\\W", "ab ", "2,3");
  rx("\\s+", "a \t b", "1,4");
  rx("\\S+", "  ab  ", "2,4");
  rx("[^\\D]+", "ab12", "2,4");      /* not a non-digit is a digit */
  rx("[\\d\\s]+", "a1 2b", "1,4");

  rx("a*", "bbb", "0,0");
  rx("a+", "bbb", "-");
  rx("ab?c", "ac", "0,2");
  rx("ab?c", "abc", "0,3");
  rx("a*", "aaa", "0,3");
  rx("a*?b", "aaab", "0,4");         /* lazy, but it still has to reach the b */
  rx("a+?", "aaa", "0,1");
  rx(".*", "abc", "0,3");
  rx(".*?", "abc", "0,0");
  rx("a{2}", "aaa", "0,2");
  rx("a{2,}", "aaaa", "0,4");
  rx("a{2,3}", "aaaa", "0,3");
  rx("a{,2}", "aaa", "0,2");         /* a comma with nothing before it is a 0 */
  rx("a{0}b", "b", "0,1");
  rx("a{}", "a{}", "0,3");           /* no comma and no digit: just braces */

  rx("(ab)+", "ababab", "0,6 4,6");  /* the group holds the last iteration */
  rx("(a)(b)", "ab", "0,2 0,1 1,2");
  rx("(?:ab)+", "abab", "0,4");
  rx("(a|b)c", "bc", "0,2 0,1");
  rx("a|ab", "ab", "0,1");           /* leftmost-first: the first alternative */
  rx("(a)?b", "b", "0,1 -1,-1");
  rx("((a)(b))", "ab", "0,2 0,2 0,1 1,2");
  rx("(a)(b)(c)(d)(e)(f)(g)(h)(i)", "abcdefghi",
     "0,9 0,1 1,2 2,3 3,4 4,5 5,6 6,7 7,8 8,9");

  rx("\\bcat\\b", "a cat here", "2,5");
  rx("\\bcat\\b", "concat", "-");
  rx("\\Bcat", "concat", "3,6");
  rx("\\b", "", "-");                /* nothing either side is no boundary */
  rx("x\\b", "x y", "0,1");

  rx_i("abc", "ABC", "0,3");
  rx_i("[a-c]+", "ABC", "0,3");
  rx_i("[^a]", "A", "-");            /* the fold applies before the negation */
  rx_i("\u00e9", "\u00c9", "-");     /* and never above ASCII */

  rx_from("^a", "aa", 1, "-");       /* ^ is the row, not where we resumed */
  rx_from("a", "aa", 1, "1,2");
  rx_from("\\bb", "ab b", 1, "3,4"); /* the character before the start counts */

  rx("[b-a]", "x", "E:character class range is out of order");
  rx("[abc", "x", "E:unterminated [ in pattern");
  rx("[]", "x", "E:unterminated [ in pattern");
  rx("[\\q]", "x", "E:unknown escape in character class");
  rx("a{3,2}", "x", "E:repeat counts are out of order");
  rx("a{70000}", "x", "E:repeat count is too large");
  rx("*a", "x", "E:nothing to repeat");
  rx("a**", "x", "E:a quantifier cannot follow a quantifier");
  rx("a\\", "x", "E:pattern ends with a backslash");
  rx("\\q", "x", "E:unknown escape sequence");
  rx("(?=a)", "x", "E:only (?:...) is supported after (?");
  rx("(a", "x", "E:missing ) in pattern");
  rx("a)", "x", "E:unmatched ) in pattern");
  rx("(1)(2)(3)(4)(5)(6)(7)(8)(9)(a)", "x", "E:too many capture groups");
}

static void test_regex_utf8(void) {
  printf("\nregex: a character, not a byte\n");

  /* The whole point of the UTF-8 half: . and a class consume a character, so a
   * match can neither end in the middle of one nor leave the cursor inside one.
   * Python's re in bytes mode counts bytes here, which is why the differential
   * fuzz is ASCII and this is not. */
  rx("^.$", "\u00e9", "0,2");
  rx(".", "\u65e5\u672c", "0,3");
  rx("^..$", "\u65e5\u672c", "0,6");
  rx("[^x]", "\u00e9", "0,2");
  rx("a.c", "a\u00e9c", "0,4");
  rx(".{2}", "\u00e91", "0,3");
  rx("^.*$", "\u65e5\u672c\u8a9e", "0,9");

  /* Everything above ASCII is a word character, so \w and \b work on words
   * molly cannot case-fold. */
  rx("\\w+", "caf\u00e9", "0,5");
  rx("\\bcaf\u00e9\\b", "a caf\u00e9.", "2,7");
  rx("[\u00e9]", "x\u00e9", "1,3");
  rx("[\u00e0-\u00ff]", "\u00e9", "0,2");
  rx("[^\u00e9]", "\u00e9x", "2,3");
}

static void test_regex_corners(void) {
  printf("\nregex: the two corners where the big engines disagree\n");

  /* Neither of these has one right answer -- Perl and Python differ from each
   * other on both -- so they are pinned here to say what molly does rather than
   * to claim it is the only possibility. Molly's answers fall out of a matcher
   * that is one repetition node and a save/restore around a capture; it is not
   * imitating either engine.
   *
   * A lazy quantifier as the body of a repetition: molly and Perl reach (1,1)
   * for the group, Python reaches (0,1). Same text matched either way. */
  rx("(a*?){1,2}b", "ab", "0,2 1,1");
  rx("(\\w*?){1,2}$", "ab", "0,2 1,2");

  /* A capture inside a repeated group, where an iteration that does not reach it
   * follows one that did: molly and Python keep the older value, Perl and
   * JavaScript clear it at every iteration. Keeping it costs nothing; clearing
   * it would cost a save and a restore per iteration in the matcher's hot
   * path. */
  rx("(b(\\s)?){3}", "b\tbb", "0,4 3,4 1,2");
  rx("(a(b)?){2}", "aba", "0,3 2,3 1,2");
  rx("(?:(a)|(b)){2}", "ab", "0,2 0,1 1,2");

  /* An iteration that matches nothing ends the repetition rather than going
   * round again forever -- but not while the minimum is unmet, where iterating
   * is finite anyway and stopping early would be wrong. */
  rx("(a*)*$", "aa", "0,2 2,2");
  rx("(a?){3}$", "", "0,0 0,0");
  rx("(a?){2}b", "ab", "0,2 1,1");
  rx("()*", "x", "0,0 0,0");
  rx("(){0}", "x", "0,0 -1,-1");
}

static void test_regex_limits(void) {
  static char buf[60001];
  const char *err = NULL;
  struct regex *re;
  struct regex_match m;
  int i;

  printf("\nregex: giving up instead of hanging\n");

  /* The pattern every article about catastrophic backtracking opens with, in the
   * form that reaches the backtracking: written as (a+)+b it never gets that
   * far, because b is a character every match must contain and a line of a's
   * does not contain one. Anchoring on $ instead leaves nothing to rule out in
   * advance, and then the thirty a's can be partitioned in 2^29 ways, every one
   * of which fails on the b at the end. The step budget turns that from a hung
   * editor into a message. */
  for (i = 0; i < 30; i++) buf[i] = 'a';
  buf[30] = 'b';
  buf[31] = '\0';
  rx("(a+)+$", buf, "!");
  rx("(a|a)*$", buf, "!");
  rx("(a+)+b", buf, "0,31 0,30");  /* and with the b where it helps, at once */

  /* A quantified single character is walked rather than recursed down, so this
   * stays linear and shallow however long the line is -- which is the case that
   * would otherwise spend one C stack frame per character. */
  for (i = 0; i < 60000; i++) buf[i] = 'a';
  buf[60000] = '\0';
  /* And no match at all is settled by memchr before the scan runs: a b that is
   * nowhere in the line means there is nothing to find, which is the answer a
   * leftmost scan would otherwise spend the whole budget arriving at. */
  re = regex_compile("a*b", 0, &err);
  check("a*b over 60000 characters answers without a search",
        re ? regex_exec(re, buf, 60000, 0, &m) : -99, REGEX_NOMATCH);
  regex_free(re);
  re = regex_compile("^a*$", 0, &err);
  check("^a*$ over 60000 characters matches all of it",
        re ? regex_exec(re, buf, 60000, 0, &m) : -99, REGEX_MATCH);
  check("and the match is the whole line", re ? m.eo[0] : -1, 60000);
  regex_free(re);

  /* A repeated *group* does recurse, one frame per iteration, so it has a
   * ceiling: measured at 160 bytes of stack per unit of depth, RX_MAX_DEPTH is a
   * 1.6 MB budget. These two pin where the ceiling is -- if the second one ever
   * starts matching, the matcher stopped using the C stack for backtracking and
   * this test should say so instead. */
  for (i = 0; i < 1000; i++) { buf[2 * i] = 'a'; buf[2 * i + 1] = 'b'; }
  buf[2000] = '\0';
  re = regex_compile("^(ab)*$", 0, &err);
  check("a repeated group over 1000 iterations matches",
        re ? regex_exec(re, buf, 2000, 0, &m) : -99, REGEX_MATCH);
  regex_free(re);
  for (i = 0; i < 20000; i++) { buf[2 * i] = 'a'; buf[2 * i + 1] = 'b'; }
  buf[40000] = '\0';
  re = regex_compile("^(ab)*$", 0, &err);
  check("over 20000 it gives up rather than overflowing the stack",
        re ? regex_exec(re, buf, 40000, 0, &m) : -99, REGEX_TOO_HARD);
  regex_free(re);
}

static void rx_sub(const char *pat, const char *text, const char *tmpl,
                   const char *want) {
  const char *err = NULL;
  struct regex *re = regex_compile(pat, 0, &err);
  struct regex_match m;
  char label[512];
  char *out;
  int len = 0;

  snprintf(label, sizeof label, "/%s/ on \"%s\" replaced with \"%s\"",
           pat, text, tmpl);
  if (re == NULL || regex_exec(re, text, (int)strlen(text), 0, &m) != REGEX_MATCH) {
    check_str(label, "no match", want);
    regex_free(re);
    return;
  }
  out = regex_expand(tmpl, text, &m, &len, &err);
  if (out == NULL) {
    char got[512];
    snprintf(got, sizeof got, "E:%s", err ? err : "(no message)");
    check_str(label, got, want);
  } else {
    check_str(label, out, want);
    check("and the length it reports is the length it wrote",
          len, (long)strlen(out));
    free(out);
  }
  regex_free(re);
}

static void test_regex_expand(void) {
  printf("\nregex: building the replacement\n");

  rx_sub("(\\w+)=(\\w+)", "k=v", "\\2=\\1", "v=k");
  rx_sub("(\\w+)=(\\w+)", "k=v", "[\\0]", "[k=v]");
  rx_sub("a", "a", "x\\\\y", "x\\y");
  rx_sub("a", "a", "\\ty", "\ty");
  rx_sub("a", "a", "\\$1", "$1");          /* an escape before punctuation */
  rx_sub("a", "a", "plain", "plain");
  rx_sub("(a)|(b)", "b", "[\\1][\\2]", "[][b]");  /* the branch not taken */
  rx_sub("a", "a", "\\n", "E:a replacement cannot contain a newline");
  rx_sub("(a)", "a", "\\2", "E:the pattern has no group with that number");
  rx_sub("a", "a", "trailing\\", "E:the replacement ends with a backslash");
}

/* ---- fitting text to a column budget ------------------------------------ */

/* utf8_fit_cols exists because utf8_col_to_byte is the wrong tool for
 * truncating: it stops at or just *past* a column, so a double-width glyph
 * straddling the limit comes back included. Every case below with a wide
 * character in it is one where the two functions differ, which is the whole
 * reason for the second one. */
static void check_fit(const char *s, int cols, int want_bytes, int want_cols) {
  char label[128];
  int got_cols = -1;
  int got = utf8_fit_cols(s, cols, &got_cols);
  snprintf(label, sizeof(label), "utf8_fit_cols(\"%s\", %d) bytes", s, cols);
  check(label, got, want_bytes);
  snprintf(label, sizeof(label), "utf8_fit_cols(\"%s\", %d) cols", s, cols);
  check(label, got_cols, want_cols);
}

static void test_fit_cols(void) {
  printf("\nutf8: fitting a string to a column budget\n");

  check_fit("abc", 10, 3, 3);       /* room to spare */
  check_fit("abc", 3, 3, 3);        /* exactly */
  check_fit("abc", 2, 2, 2);        /* cut */
  check_fit("abc", 0, 0, 0);        /* no room at all */
  check_fit("", 5, 0, 0);

  /* A double-width character that does not fit is left out rather than drawn
   * half outside the budget, and the columns reported are the ones actually
   * used -- one short of the budget, which a caller measuring a mnemonic
   * against the truncated text needs to know. */
  check_fit("\xe6\x9c\xa8", 2, 3, 2);   /* the whole wide char fits */
  check_fit("\xe6\x9c\xa8", 1, 0, 0);   /* it does not, so nothing is taken */
  check_fit("a\xe6\x9c\xa8", 2, 1, 1);  /* 'a' fits, the wide char does not */
  check_fit("a\xe6\x9c\xa8", 3, 4, 3);  /* both fit */
  /* Written as two literals on purpose: "\xa8b" reads as one hex escape, so
   * the 'b' would vanish into it and this check would test the line above. */
  check_fit("a\xe6\x9c\xa8" "b", 3, 4, 3); /* and the 'b' does not */

  /* A negative budget is treated as no room, rather than read as a huge one. */
  check_fit("abc", -1, 0, 0);

  /* A combining mark takes no columns, so it travels with its base character
   * instead of being cut off it. */
  check_fit("e\xcc\x81", 1, 3, 1);
}

/* ---- chords -------------------------------------------------------------- */

/* chord_parse and chord_name are a pair, and the config file depends on both: it
 * writes what chord_name prints and reads it back through chord_parse. The
 * default keymap already proves the round trip for everything it binds (that is
 * what MOLLY_DUMP_KEYMAP is for); what is tested here is the rest -- the Shift
 * forms, and the chords that must be *refused* rather than bound to something
 * that could never fire. */
static void check_chord(const char *text, int want) {
  char label[96];
  snprintf(label, sizeof(label), "chord_parse(\"%s\")", text);
  check(label, chord_parse(text), want);
}

static void check_chord_name(int key, const char *want) {
  char label[96];
  char got[32];
  chord_name(key, got, sizeof(got));
  snprintf(label, sizeof(label), "chord_name(%#x)", (unsigned)key);
  check_str(label, got, want);
}

static void test_chords(void) {
  printf("\nchords: parsing, printing and what is refused\n");

  check_chord("ctrl-s", CTRL_KEY('S'));
  check_chord("Ctrl-S", CTRL_KEY('S'));
  check_chord("ctrl-?", CTRL_KEY('?'));
  check_chord("ctrl-space", CTRL_KEY(' '));
  check_chord("alt-x", ALT_KEY('x'));
  check_chord("f10", F1_KEY + 9);
  check_chord("F12", F1_KEY + 11);
  check_chord("pageup", PAGE_UP);
  check_chord("PgUp", PAGE_UP);
  check_chord("shift-tab", BACKTAB_KEY);
  check_chord("a", 'a');

  /* Shift, on the keys that report it. */
  check_chord("shift-f3", (F1_KEY + 2) | KEY_SHIFT);
  check_chord("Shift-F3", (F1_KEY + 2) | KEY_SHIFT);
  check_chord("shift-right", ARROW_RIGHT | KEY_SHIFT);
  check_chord("shift-pagedown", PAGE_DOWN | KEY_SHIFT);
  /* Modifiers in either order, since a hand-written file may use either. */
  check_chord("shift-f5", (F1_KEY + 4) | KEY_SHIFT);

  /* And the refusals. Each of these is a chord a terminal cannot deliver
   * distinguishably, so binding it would be a line in a config file that
   * silently never fires. */
  check_chord("shift-a", -1);
  check_chord("shift-ctrl-l", -1);
  check_chord("ctrl-shift-l", -1);
  check_chord("ctrl-alt-x", -1);
  check_chord("shift-enter", -1);
  check_chord("ctrl-", -1);
  check_chord("shift-", -1);
  check_chord("", -1);
  check_chord("f13", -1);
  check_chord("f0", -1);
  check_chord("f1x", -1);
  check_chord("ctrl-ss", -1);
  check_chord("nonsense", -1);

  /* Printing. Shift is a prefix on whatever the key is otherwise called. */
  check_chord_name(CTRL_KEY('S'), "Ctrl-S");
  check_chord_name(CTRL_KEY('?'), "Ctrl-?");
  check_chord_name(CTRL_KEY(' '), "Ctrl-Space");
  check_chord_name(ALT_KEY('x'), "Alt-X");
  check_chord_name(F1_KEY + 9, "F10");
  check_chord_name(PAGE_UP, "PgUp");
  check_chord_name(BACKTAB_KEY, "Shift-Tab");
  check_chord_name((F1_KEY + 2) | KEY_SHIFT, "Shift-F3");
  check_chord_name(ARROW_RIGHT | KEY_SHIFT, "Shift-Right");

  /* The round trip, for every chord the printer can produce from a shifted key:
   * what chord_name prints is what chord_parse has to accept, because that is
   * exactly what config_write puts in the file. */
  {
    int keys[] = {ARROW_LEFT, ARROW_RIGHT, ARROW_UP, ARROW_DOWN, HOME_KEY,
                  END_KEY, PAGE_UP, PAGE_DOWN, DEL_KEY, F1_KEY, F1_KEY + 11};
    int i, bad = 0;
    for (i = 0; i < (int)(sizeof(keys) / sizeof(keys[0])); i++) {
      char text[32];
      int shifted = keys[i] | KEY_SHIFT;
      chord_name(shifted, text, sizeof(text));
      if (chord_parse(text) != shifted) bad++;
    }
    check("every shifted key round-trips through its printed name", bad, 0);
  }
}

/* ---- the configuration file ---------------------------------------------- */

/* A clean slate: config_parse_text adds to whatever is already there, which is
 * what a caller parsing one file wants and not what a second test wants. */
static void config_fresh(void) {
  config_cleanup();
  config_defaults();
}

static void test_config_parse(void) {
  printf("\nconfig: parsing settings\n");

  config_fresh();
  check("a setting with no file is its default", setting_get("editor", "tab width"), 8);
  check("and so is the menu bar", setting_get("view", "menu bar"), 1);
  check("and line numbers are off", setting_get("view", "line numbers"), 0);

  config_fresh();
  config_parse_text(
      "# a comment\n"
      "; and another\n"
      "\n"
      "[editor]\n"
      "tab width = 4\n"
      "\n"
      "[view]\n"
      "line numbers = on\n"
      "menu bar = off\n"
      "[input]\n"
      "mouse = no\n",
      "<test>");
  check("no warnings from a good file", config_error_count(), 0);
  check("tab width = 4", setting_get("editor", "tab width"), 4);
  check("line numbers = on", setting_get("view", "line numbers"), 1);
  check("menu bar = off", setting_get("view", "menu bar"), 0);
  check("mouse = no", setting_get("input", "mouse"), 0);

  /* Spellings. A key is matched on its words, so dashes, underscores, extra
   * spaces and capitals all reach the same setting; the canonical form is what
   * config_write emits. */
  config_fresh();
  config_parse_text("[view]\nLine-Numbers = TRUE\n", "<test>");
  check("Line-Numbers reaches line numbers", setting_get("view", "line numbers"), 1);
  check("and warns about nothing", config_error_count(), 0);

  config_fresh();
  config_parse_text("[ View ]\nline_numbers = yes\n", "<test>");
  check("line_numbers too", setting_get("view", "line numbers"), 1);
  check("and the section tolerates spaces", config_error_count(), 0);

  config_fresh();
  config_parse_text("[view]\nline   numbers = 1\n", "<test>");
  check("a run of spaces collapses", setting_get("view", "line numbers"), 1);

  /* A file with no trailing newline is still a file. */
  config_fresh();
  config_parse_text("[editor]\ntab width = 2", "<test>");
  check("the last line counts without a newline", setting_get("editor", "tab width"), 2);
  check("and warns about nothing", config_error_count(), 0);

  printf("\nconfig: what a bad line does\n");

  config_fresh();
  config_parse_text("[view]\nline numbers = maybe\n", "<test>");
  check("a flag that is not on or off warns", config_error_count(), 1);
  check("and leaves the setting alone", setting_get("view", "line numbers"), 0);
  check_str("with a message naming the line", config_error_at(0),
            "<test>:2: line numbers wants on or off, not \"maybe\"");

  config_fresh();
  config_parse_text("[editor]\ntab width = 99\n", "<test>");
  check("an out-of-range number warns", config_error_count(), 1);
  check("and leaves the setting alone", setting_get("editor", "tab width"), 8);
  check_str("saying what the range is", config_error_at(0),
            "<test>:2: tab width must be 1 to 16, not 99");

  config_fresh();
  config_parse_text("[editor]\ntab width = wide\n", "<test>");
  check_str("a number that is not one", config_error_at(0),
            "<test>:2: tab width wants a number, not \"wide\"");

  config_fresh();
  config_parse_text("[nowhere]\nthing = 1\n", "<test>");
  check("an unknown section warns once", config_error_count(), 1);
  check_str("naming the section", config_error_at(0),
            "<test>:1: unknown section: [nowhere]");

  config_fresh();
  config_parse_text("[view]\nnot a setting = on\n", "<test>");
  check_str("an unknown key names itself", config_error_at(0),
            "<test>:2: unknown setting: [view] not a setting");

  config_fresh();
  config_parse_text("line numbers = on\n", "<test>");
  check_str("a key before any section", config_error_at(0),
            "<test>:1: line numbers is outside any section");

  config_fresh();
  config_parse_text("[view]\nline numbers\n", "<test>");
  check_str("a line with no =", config_error_at(0),
            "<test>:2: expected key = value");

  config_fresh();
  config_parse_text("[view\nline numbers = on\n", "<test>");
  check_str("a section with no ]", config_error_at(0),
            "<test>:1: section is missing its ]");

  /* Carrying on is the point: one bad line must not cost the good ones. */
  config_fresh();
  config_parse_text(
      "[editor]\n"
      "tab width = nonsense\n"
      "undo steps = 10\n"
      "[view]\n"
      "line numbers = on\n",
      "<test>");
  check("one bad line, one warning", config_error_count(), 1);
  check("the line after it still took", setting_get("editor", "undo steps"), 10);
  check("and so did the next section", setting_get("view", "line numbers"), 1);

  /* The warning list is capped; the count is not. */
  config_fresh();
  {
    char big[4096];
    int i;
    size_t o = (size_t)snprintf(big, sizeof(big), "[view]\n");
    for (i = 0; i < 40; i++)
      o += (size_t)snprintf(big + o, sizeof(big) - o, "bogus%d = on\n", i);
    config_parse_text(big, "<test>");
    check("every bad line is counted", config_error_count(), 40);
    check("but only the first few are kept", config_error_at(16) == NULL, 1);
    check("and the kept ones are there", config_error_at(15) != NULL, 1);
  }

  config_fresh();
}

static void test_config_keys(void) {
  printf("\nconfig: the [keys] section\n");

  config_fresh();
  config_parse_text("[keys]\nf9 = file.save\n", "<test>");
  check("a good binding warns about nothing", config_error_count(), 0);

  keymap_init();
  config_apply_keys();
  check("F9 is bound to what the file said",
        keymap_lookup(F1_KEY + 8) == cmd_find_by_name("file.save"), 1);
  check("and Ctrl-S still is too",
        keymap_lookup(CTRL_KEY('S')) == cmd_find_by_name("file.save"), 1);
  {
    char chord[32];
    cmd_shortcut("file.save", chord, sizeof(chord));
    check_str("the menu shows the binding from the file", chord, "F9");
  }

  /* Replacing a chord that is already bound, rather than adding a second
   * meaning to it. */
  config_fresh();
  config_parse_text("[keys]\nctrl-s = file.quit\n", "<test>");
  keymap_init();
  config_apply_keys();
  check("rebinding a default chord replaces it",
        keymap_lookup(CTRL_KEY('S')) == cmd_find_by_name("file.quit"), 1);

  /* Unbinding. */
  config_fresh();
  config_parse_text("[keys]\nctrl-s = none\n", "<test>");
  keymap_init();
  config_apply_keys();
  check("= none leaves the chord unbound", keymap_lookup(CTRL_KEY('S')) == NULL, 1);
  check("and the rest of the keymap survives",
        keymap_lookup(CTRL_KEY('Q')) == cmd_find_by_name("file.quit"), 1);

  /* A shifted binding, which is the whole reason Shift became nameable. */
  config_fresh();
  config_parse_text("[keys]\nshift-f3 = search.prev\n", "<test>");
  keymap_init();
  config_apply_keys();
  check("Shift-F3 binds without touching F3",
        keymap_lookup((F1_KEY + 2) | KEY_SHIFT) == cmd_find_by_name("search.prev"), 1);

  /* What is refused. */
  config_fresh();
  config_parse_text("[keys]\nctrl-s = no.such.command\n", "<test>");
  check_str("an unknown command names itself", config_error_at(0),
            "<test>:2: no such command: no.such.command");

  config_fresh();
  config_parse_text("[keys]\nnonsense = file.save\n", "<test>");
  check_str("so does a chord that is not one", config_error_at(0),
            "<test>:2: not a key: nonsense");

  config_fresh();
  config_parse_text("[keys]\nalt-f = file.save\n", "<test>");
  keymap_init();
  config_apply_keys();
  check("the menu bar's Alt letters are refused", config_error_count(), 1);
  check("and Alt-F still opens the File menu",
        keymap_lookup(ALT_KEY('f')) == cmd_find_by_name("menu.file"), 1);

  config_fresh();
  keymap_init();
}

/* The table as the Options dialog sees it. */
static void test_setting_access(void) {
  const char *sec, *name, *comment;
  int is_bool, lo, hi, i, tabw = -1, spaces = -1;

  printf("\nsettings: the table, row by row\n");
  check("there is a row for every setting", setting_count() > 10, 1);
  check("and none past the end",
        setting_info(setting_count(), &sec, &name, &is_bool, &lo, &hi, &comment), 0);
  for (i = 0; i < setting_count(); i++) {
    setting_info(i, &sec, &name, &is_bool, &lo, &hi, &comment);
    if (strcmp(name, "tab width") == 0) tabw = i;
    if (strcmp(name, "insert spaces") == 0) spaces = i;
  }
  check("tab width is one of them", tabw >= 0, 1);
  setting_info(tabw, &sec, &name, &is_bool, &lo, &hi, &comment);
  check_str("in [editor]", sec, "editor");
  check("a number", is_bool, 0);
  check("from 1", lo, 1);
  check("to 16", hi, 16);
  check("with a comment to show", comment != NULL && comment[0] != '\0', 1);
  setting_info(spaces, &sec, &name, &is_bool, &lo, &hi, &comment);
  check("insert spaces is on or off", is_bool * 100 + lo * 10 + hi, 101);

  setting_set(tabw, 4);
  check("setting it puts it into effect", tab_width_default(), 4);
  check("and reads back live", setting_live(tabw), 4);
  setting_set(tabw, 99);
  check("past its range is clamped to the top", setting_live(tabw), 16);
  setting_set(tabw, 0);
  check("and below it to the bottom", setting_live(tabw), 1);
  setting_set(tabw, KILO_TAB_STOP);
  setting_set(spaces, 7);
  check("on is any non-zero, and stored as 1", setting_live(spaces), 1);
  setting_set(spaces, 0);
  check("and off is off", indent_insert_spaces_default(), 0);
}

static void test_config_write(void) {
  char path[256];
  printf("\nconfig: writing it back\n");

  snprintf(path, sizeof(path), "/tmp/molly_cfg_%d/sub/mollyrc", (int)getpid());

  config_fresh();
  keymap_init();
  config_parse_text(
      "[editor]\ntab width = 3\n"
      "[view]\nline numbers = on\n"
      "[keys]\nf9 = file.save\nctrl-s = none\n",
      "<test>");
  config_apply_keys();

  /* What gets written is what the editor is *doing*, not what the file said --
   * that is the whole point of a settings file an Options dialog can save, and
   * it is why each setting may name a function to read its live value back. So
   * the live state is what this sets up, and the file below is checked against
   * it rather than against the text parsed above. Ctrl-U changes V.linenum
   * without going anywhere near the settings table; the table finds out by
   * asking. */
  tab_width_set(3);
  V.linenum = 1;
  /* The editor normally has the bar on; screen_init_layers is what turns it on
   * and no unit test runs it, so say so here rather than let this read as a
   * default. */
  E.menubar = 1;

  {
    const char *err = NULL;
    check("writing creates the directories it needs",
          config_write(path, &err), 0);
    if (err != NULL) printf("          (%s)\n", err);
  }

  /* Read it back through the parser. A written file that the parser rejects is
   * the failure this whole test exists to catch: the two halves have to agree on
   * the canonical spelling of every name and value. */
  {
    FILE *fp = fopen(path, "r");
    char text[8192];
    size_t n = 0;
    check("and the file is there", fp != NULL, 1);
    if (fp != NULL) {
      n = fread(text, 1, sizeof(text) - 1, fp);
      fclose(fp);
    }
    text[n] = '\0';

    config_fresh();
    keymap_init();
    config_parse_text(text, path);
    check("what molly writes, molly reads without a warning",
          config_error_count(), 0);
    check("the values survive the round trip",
          setting_get("editor", "tab width"), 3);
    check("including the flags", setting_get("view", "line numbers"), 1);
    check("and a setting the file never mentioned is written from the editor",
          setting_get("view", "menu bar"), 1);

    config_apply_keys();
    check("and so does a changed binding",
          keymap_lookup(F1_KEY + 8) == cmd_find_by_name("file.save"), 1);
    check("and an unbound chord stays unbound",
          keymap_lookup(CTRL_KEY('S')) == NULL, 1);
  }

  /* The surprising half, stated on its own: a setting changed since the file was
   * read is written as it is now. Without this, "turn the gutter off, then save
   * settings" would write the gutter back on. */
  {
    const char *err = NULL;
    FILE *fp;
    char text[8192];
    size_t n = 0;
    V.linenum = 0;
    tab_width_set(8);
    check("writing again succeeds", config_write(path, &err), 0);
    fp = fopen(path, "r");
    if (fp != NULL) {
      n = fread(text, 1, sizeof(text) - 1, fp);
      fclose(fp);
    }
    text[n] = '\0';
    config_fresh();
    config_parse_text(text, path);
    check("a setting changed since the file was read is written as it is now",
          setting_get("view", "line numbers"), 0);
    check("and so is a number", setting_get("editor", "tab width"), 8);
  }

  /* Tidy up: this is a test, so the directory is ours to remove. */
  {
    char dir[256];
    unlink(path);
    snprintf(dir, sizeof(dir), "/tmp/molly_cfg_%d/sub", (int)getpid());
    rmdir(dir);
    snprintf(dir, sizeof(dir), "/tmp/molly_cfg_%d", (int)getpid());
    rmdir(dir);
  }

  config_fresh();
  keymap_init();
  V.linenum = 0;
  tab_width_set(KILO_TAB_STOP);
}

static void test_mkdir_p(void) {
  char base[256], deep[320];
  printf("\nsys: mkdir -p\n");

  snprintf(base, sizeof(base), "/tmp/molly_mkdir_%d", (int)getpid());
  snprintf(deep, sizeof(deep), "%s/a/b/c", base);

  check("a path three levels deep is created", mkdir_p(deep), 0);
  check("and again is still success", mkdir_p(deep), 0);
  {
    struct stat st;
    check("and it really is a directory",
          stat(deep, &st) == 0 && S_ISDIR(st.st_mode), 1);
  }

  /* A file where a directory should go is an error rather than a surprise. */
  {
    char blocked[384];
    char file[320];
    FILE *fp;
    snprintf(file, sizeof(file), "%s/afile", base);
    fp = fopen(file, "w");
    if (fp != NULL) fclose(fp);
    snprintf(blocked, sizeof(blocked), "%s/under", file);
    check("a file in the way is refused", mkdir_p(blocked) == -1, 1);
    unlink(file);
  }

  check("and an empty path is refused", mkdir_p("") == -1, 1);

  {
    char p[320];
    snprintf(p, sizeof(p), "%s/a/b/c", base);
    rmdir(p);
    snprintf(p, sizeof(p), "%s/a/b", base);
    rmdir(p);
    snprintf(p, sizeof(p), "%s/a", base);
    rmdir(p);
    rmdir(base);
  }
}

/* ---- the help screen's data ----------------------------------------------- */

/* Every line of the keyboard reference has to name a key.
 *
 * This is a rule rather than a preference: the left column of the help screen is
 * the key, so an entry with nothing bound to it renders as a description beside
 * an empty column, which reads as a bug rather than as "reachable another way".
 * It has happened twice -- Match case when transpose took Ctrl-T, and the menu
 * bar toggle, which never had a chord and sat below a row cap where nobody could
 * see it. Both were fixed in different ways (one left the screen, one gained a
 * binding), so what is checked here is the rule and not either fix. */
static void test_help_entries(void) {
  int i, blank = 0;
  printf("\nhelp: every entry names a key\n");

  keymap_init();
  for (i = 0; i < help_entry_count(); i++) {
    const char *group, *desc;
    char key[64];
    help_entry_at(i, &group, key, sizeof(key), &desc);
    if (key[0] == '\0') {
      printf("          entry %d has no key: \"%s\"\n", i, desc);
      blank++;
    }
  }
  check("no help entry has an empty key column", blank, 0);
}

/* A cancel callback that asks to stop the second time it is consulted, so the
 * command is interrupted while it is running rather than before it starts. */
static int cancel_calls;
static int cancel_second_time(void) {
  return ++cancel_calls >= 2;
}

/* ---- the shell filter ---------------------------------------------------- */

/* filter_run() takes a string and a command and gives back a string, so every
 * one of its failure modes is reachable here rather than through a pty: a
 * command that fails, one that floods, one that hangs, one that does not
 * exist, and one that stops reading halfway through its input.
 *
 * The limits are arguments and not settings for exactly this reason. The editor
 * passes seconds and megabytes; these pass milliseconds and kilobytes, so the
 * timeout test costs a quarter of a second instead of ten. */
static int filt(const char *cmd, const char *in, struct filter_io *io) {
  memset(io, 0, sizeof(*io));
  io->cmd = cmd;
  io->in = in;
  io->inlen = (in != NULL) ? (int)strlen(in) : 0;
  io->timeout_ms = 2000;
  io->max_out = 64 * 1024;
  return filter_run(io, NULL);
}

static void test_filter(void) {
  struct filter_io io;
  printf("\nshell filter: text in, a command, text out\n");

  loop_cleanup();

  check("cat gives the text back", filt("cat", "one\ntwo\n", &io), FILTER_OK);
  check_str("unchanged", io.out, "one\ntwo\n");
  check("and the length agrees", io.outlen, 8);
  filter_free(&io);

  check("sort sorts it", filt("sort", "beta\nalpha\ngamma\n", &io), FILTER_OK);
  check_str("into order", io.out, "alpha\nbeta\ngamma\n");
  filter_free(&io);

  /* A pipeline, which is why the command goes through a shell rather than
   * being split on spaces and exec'd. */
  check("a pipeline is one command",
        filt("tr a-z A-Z | rev", "abc\n", &io), FILTER_OK);
  check_str("and both halves ran", io.out, "CBA\n");
  filter_free(&io);

  /* No input at all: the command's standard input must reach end of file
   * rather than staying open, or `cat` never returns. */
  check("a command with no input still finishes",
        filt("cat; echo done", "", &io), FILTER_OK);
  check_str("and sees end of file immediately", io.out, "done\n");
  filter_free(&io);

  /* More than a pipe will hold, in both directions at once. This is the
   * deadlock the writable event source exists to prevent: molly is still
   * feeding the command when the command's output fills its own pipe. */
  {
    char *big = xmalloc(200001);
    memset(big, 'x', 200000);
    big[200000] = '\0';
    memset(&io, 0, sizeof(io));
    io.cmd = "cat";
    io.in = big;
    io.inlen = 200000;
    io.timeout_ms = 5000;
    io.max_out = 1024 * 1024;
    check("200k through cat does not deadlock", filter_run(&io, NULL), FILTER_OK);
    check("and arrives whole", io.outlen, 200000);
    filter_free(&io);
    free(big);
  }

  printf("\nshell filter: when the command goes wrong\n");

  /* A non-zero exit is reported rather than applied. The editor does not
   * replace the text on a failure, which is why the status has to come back. */
  check("a failing command says so", filt("exit 3", "x\n", &io), FILTER_FAILED);
  check("with its status", io.status, 3);
  filter_free(&io);

  /* The command's own words, not molly's. "sh: 1: jqq: not found" tells the
   * user what to fix; "exited 127" does not. */
  check("a command that does not exist fails",
        filt("jqq-does-not-exist", "x\n", &io), FILTER_FAILED);
  check("and its own error message comes back", io.err[0] != '\0', 1);
  check("with the name of the thing that was missing",
        strstr(io.err, "jqq-does-not-exist") != NULL, 1);
  filter_free(&io);

  /* One line of it, with control bytes defanged: the message goes on a one-row
   * bar, and an escape sequence in it would repaint the screen. */
  check("several lines of error keep the first",
        filt("printf 'first\\nsecond\\n' >&2; exit 1", "x\n", &io), FILTER_FAILED);
  check_str("only", io.err, "first");
  filter_free(&io);

  check("an escape in the error text is defanged",
        filt("printf '\\033[31mred\\n' >&2; exit 1", "x\n", &io), FILTER_FAILED);
  check_str("to spaces", io.err, "[31mred");
  filter_free(&io);

  /* Standard error must be a pipe even when the command succeeds, or a warning
   * lands in the middle of the text area. Here it is collected and the output
   * is still clean. */
  check("a command that warns and succeeds still succeeds",
        filt("echo oops >&2; cat", "x\n", &io), FILTER_OK);
  check_str("with its output unpolluted", io.out, "x\n");
  filter_free(&io);

  printf("\nshell filter: the three limits\n");

  /* A command that never finishes. 250ms rather than the editor's ten seconds,
   * which is the whole reason the timeout is an argument. */
  memset(&io, 0, sizeof(io));
  io.cmd = "sleep 30";
  io.in = "";
  io.inlen = 0;
  io.timeout_ms = 250;
  io.max_out = 64 * 1024;
  check("a command that hangs is stopped", filter_run(&io, NULL), FILTER_TIMEOUT);
  check_str("and says why", io.err, "took too long");
  filter_free(&io);

  /* And the whole pipeline goes, not just the shell: `sleep` is the child of
   * the `sh` that molly started, so killing one process would leave it
   * running. The group is what makes this finish at all.
   *
   * Checked by the clock: if only the shell had been killed, reaping it would
   * have returned at once and this would be fast, but the sleep would still be
   * running. A command substitution waits for the whole pipeline, so the shell
   * cannot exit before its children do. */
  memset(&io, 0, sizeof(io));
  io.cmd = "sleep 30 | cat";
  io.in = "";
  io.inlen = 0;
  io.timeout_ms = 250;
  io.max_out = 64 * 1024;
  check("a hanging pipeline is stopped too", filter_run(&io, NULL), FILTER_TIMEOUT);
  filter_free(&io);

  /* A command that floods. The cap is checked as the output arrives, so a
   * runaway is never held in memory in order to be rejected. */
  memset(&io, 0, sizeof(io));
  io.cmd = "yes molly";
  io.in = "";
  io.inlen = 0;
  io.timeout_ms = 5000;
  io.max_out = 16 * 1024;
  check("a command that floods is stopped", filter_run(&io, NULL), FILTER_TOOBIG);
  check_str("and says why", io.err, "too much output");
  check("having kept no more than the cap plus the read that crossed it",
        io.outlen <= 16 * 1024 + 8192, 1);
  filter_free(&io);

  /* The cancel callback, which in the editor reads the keyboard. Here it says
   * stop on its second call, so the command is interrupted rather than waited
   * for. */
  memset(&io, 0, sizeof(io));
  io.cmd = "sleep 30";
  io.in = "";
  io.inlen = 0;
  io.timeout_ms = 10000;
  io.max_out = 64 * 1024;
  cancel_calls = 0;
  check("a cancelled command is stopped", filter_run(&io, cancel_second_time),
        FILTER_CANCELLED);
  check_str("and says so", io.err, "cancelled");
  filter_free(&io);

  printf("\nshell filter: and the awkward ones\n");

  /* `head -1` reads one line and closes its input. Molly is still writing, so
   * the write fails with EPIPE -- which is correct behaviour and must not be
   * fatal. Without SIGPIPE ignored this check kills the test binary. */
  {
    char *big = xmalloc(100001);
    memset(big, 'y', 100000);
    big[0] = 'z';
    big[1] = '\n';
    big[100000] = '\0';
    memset(&io, 0, sizeof(io));
    io.cmd = "head -1";
    io.in = big;
    io.inlen = 100000;
    io.timeout_ms = 5000;
    io.max_out = 1024 * 1024;
    check("a command that stops reading is not an error",
          filter_run(&io, NULL), FILTER_OK);
    check_str("and its output is what it did read", io.out, "z\n");
    filter_free(&io);
    free(big);
  }

  /* An empty command is refused before anything is forked. */
  check("an empty command does not run", filt("", "x\n", &io), FILTER_NOSTART);
  check_str("and says what was wrong", io.err, "no command");
  filter_free(&io);

  /* Nothing may be left registered. A source left behind would be polled for
   * the rest of the session on a descriptor that has been closed and may since
   * have been handed to something else. */
  check("every event source was given back", loop_source_count(), 0);

  /* Binary is text as far as this is concerned: the output is length-counted,
   * not NUL-terminated, because a filter may legitimately produce a zero byte
   * and truncating there would lose the rest. */
  check("a NUL in the output does not truncate it",
        filt("printf 'a\\000b'", "", &io), FILTER_OK);
  check("the length counts all of it", io.outlen, 3);
  check("including the zero byte", io.out[1] == '\0' && io.out[2] == 'b', 1);
  filter_free(&io);
}

/* ---- the window tree ----------------------------------------------------- */

/* Splits are testable without a terminal for the same reason the buffer list is:
 * what the tree does is hold windows, say which has the keyboard, and share a
 * rect out between them. Nothing in that draws.
 *
 * The rect is supplied by the test. In the editor the layout is recomputed from
 * the terminal size every frame -- which is what makes a resize, a split and a
 * close all end in the same walk -- and a split or a close therefore lays out
 * again before it returns. There is no terminal here, so each of those calls
 * has to be followed by handing the tree the screen this test is reasoning
 * about. 80x22 is the text area of an 80x24 terminal with a menu bar. */
static void win_screen(void) {
  window_layout_rect(0, 1, 80, 22);
}

static void test_windows(void) {
  printf("\nwindows: the tree, and the screen shared out between it\n");

  window_init();
  buflist_init();
  win_screen();

  check("a fresh editor has one window", window_count(), 1);
  check("and it has the keyboard", window_index_of(E.curview), 0);
  check("it starts where the chrome ends", V.screeny, 1);
  check("and takes the whole width", V.screencols, 80);
  /* One row of the window is its own status bar, which is why a single window
   * puts that bar exactly where the screen-wide one used to be. */
  check("its text area is a row shorter than the window", V.screenrows, 21);

  /* ---- side by side ---- */
  check("splitting right succeeds", window_split(WIN_COLS), 1);
  win_screen();
  check("there are two windows", window_count(), 2);
  check("and the new one has the keyboard", window_index_of(E.curview), 1);
  check("both show the same buffer",
        window_view_at(0)->buf == window_view_at(1)->buf, 1);
  check("the left one starts at column zero", window_view_at(0)->screenx, 0);
  check("and gets half the width", window_view_at(0)->screencols, 40);
  /* 40 columns, a separator column, 39 columns: the separator belongs to
   * neither window, which is why the two widths do not add up to 80. */
  check("the right one starts past the separator",
        window_view_at(1)->screenx, 41);
  check("and gets what is left", window_view_at(1)->screencols, 39);
  check("both are as tall as the area", window_view_at(1)->screenrows, 21);
  check("and both start on the same row", window_view_at(1)->screeny, 1);

  /* ---- the keyboard moves between them ---- */
  window_move_focus(1);
  check("next window wraps round to the first", window_index_of(E.curview), 0);
  window_move_focus(1);
  check("and back to the second", window_index_of(E.curview), 1);
  window_move_focus(-1);
  check("previous window goes the other way", window_index_of(E.curview), 0);
  window_focus(1);
  check("or the window can be named outright", window_index_of(E.curview), 1);

  /* ---- each window keeps its own place in the file ---- */
  window_view_at(0)->drowoff = 7;
  check("scrolling one does not scroll the other",
        window_view_at(1)->drowoff, 0);

  /* ---- closing ---- */
  check("closing it succeeds", window_close_current(), 1);
  win_screen();
  check("one window again", window_count(), 1);
  check("the survivor has the keyboard", window_index_of(E.curview), 0);
  /* The parent split node went with the window, so the survivor is back to the
   * full width rather than to half of it with nothing beside it. */
  check("and the full width back", V.screencols, 80);
  check("closing the only window is refused", window_close_current(), 0);

  /* ---- stacked ---- */
  check("splitting below succeeds", window_split(WIN_ROWS), 1);
  win_screen();
  check("two windows again", window_count(), 2);
  check("the top one starts at the top", window_view_at(0)->screeny, 1);
  /* 11 rows each: 10 of text and the status bar. No separator row, because the
   * upper window's status bar already is one. */
  check("and gets half the rows", window_view_at(0)->screenrows, 10);
  check("the bottom one starts below it", window_view_at(1)->screeny, 12);
  check("and gets the rest", window_view_at(1)->screenrows, 10);
  check("both keep the full width", window_view_at(1)->screencols, 80);

  /* ---- nesting ---- */
  check("the bottom one splits again", window_split(WIN_COLS), 1);
  win_screen();
  check("three windows", window_count(), 3);
  check("the untouched one is unchanged", window_view_at(0)->screencols, 80);
  check("the split one is halved", window_view_at(1)->screencols, 40);
  check("and its new neighbour takes the rest",
        window_view_at(2)->screencols, 39);
  check("which starts past the separator", window_view_at(2)->screenx, 41);
  check("and shares the bottom row band", window_view_at(2)->screeny, 12);

  /* Order is screen order: left before right, top before bottom. */
  check("only this window leaves one", (window_only(), window_count()), 1);
  win_screen();
  check("and gives it everything", V.screencols, 80);
  check("including the rows", V.screenrows, 21);

  buflist_cleanup();
}

/* A split that would make a window too small to use is refused rather than
 * made. The alternative is a window the user has to close again, which is worse
 * than being told no -- and the numbers are the only thing standing between a
 * 24-row terminal and a stack of status bars. */
static void test_windows_too_small(void) {
  printf("\nwindows: a split that will not fit\n");

  window_init();
  buflist_init();

  /* 16 columns: two eight-column windows would fit, but not the separator. */
  window_layout_rect(0, 0, 16, 10);
  check("16 columns is one column short", window_split(WIN_COLS), 0);
  check("so there is still one window", window_count(), 1);
  window_layout_rect(0, 0, 17, 10);
  check("17 is enough", window_split(WIN_COLS), 1);
  check("and there are two", window_count(), 2);

  window_init();
  buflist_init();
  /* Three rows: two windows need two each, a row of text and a status bar. */
  window_layout_rect(0, 0, 80, 3);
  check("three rows will not stack", window_split(WIN_ROWS), 0);
  window_layout_rect(0, 0, 80, 4);
  check("four will", window_split(WIN_ROWS), 1);
  win_screen();

  buflist_cleanup();
}

/* Moving a border with the keyboard. As above, the tree is laid out by the
 * test after every call, since window_move_border() lays out against a terminal
 * there is none of. */
static void test_windows_resize(void) {
  int w, ok, cut, prev;
  int widths[] = {17, 18, 25, 80, 81, 159, 333, 999, 1000, 1001, 1500, 2047};
  int i;

  printf("\nwindows: moving the border between two\n");

  window_init();
  buflist_init();
  win_screen();
  check("one window has no border beside it", window_move_border(-1, 0), 0);
  check("or above it", window_move_border(0, -1), 0);

  window_split(WIN_COLS);
  win_screen();
  /* 79 columns between them: 40 and 39, the keyboard in the right one. */
  check("the arrow moves the border, here left", window_move_border(-1, 0), 1);
  win_screen();
  check("so the left window is a column narrower",
        window_view_at(0)->screencols, 39);
  check("and the right one, which has the keyboard, a column wider",
        window_view_at(1)->screencols, 40);
  check("and starts a column earlier", window_view_at(1)->screenx, 40);
  window_focus(0);
  check("from the left window, Left moves the same border the same way",
        window_move_border(-1, 0), 1);
  win_screen();
  check("so now the window with the keyboard is the one that shrinks",
        window_view_at(0)->screencols, 38);
  check("and Right takes it back", window_move_border(1, 0), 1);
  win_screen();
  check("a column at a time", window_view_at(0)->screencols, 39);
  check("a side-by-side split has no border above or below",
        window_move_border(0, 1), 0);

  for (ok = 0; window_move_border(-1, 0); ok++) win_screen();
  win_screen();
  check("Left stops at the narrowest a window may be",
        window_view_at(0)->screencols, 8);
  check("after one press per column", ok, 31);
  for (ok = 0; window_move_border(1, 0); ok++) win_screen();
  win_screen();
  check("and Right stops where the other window is that narrow",
        window_view_at(1)->screencols, 8);

  /* A terminal resize scales the cut rather than keeping its column, because
   * what was stored is a share. */
  while (window_move_border(-1, 0)) win_screen();
  for (i = 0; i < 12; i++) { window_move_border(1, 0); win_screen(); }
  check("twenty columns of seventy-nine", window_view_at(0)->screencols, 20);
  window_layout_rect(0, 1, 159, 22);
  check("are forty of a hundred and fifty-eight",
        window_view_at(0)->screencols, 40);
  win_screen();

  /* A half that is itself split needs room for both its windows and the
   * separator between them, not just one window's minimum. */
  window_focus(1);
  window_split(WIN_COLS);
  win_screen();
  window_focus(0);
  while (window_move_border(1, 0)) win_screen();
  win_screen();
  check("a border stops short of squeezing a split half's own windows",
        window_view_at(1)->screencols, 8);
  check("both of them", window_view_at(2)->screencols, 8);
  check("so the left window takes what is left of the 79",
        window_view_at(0)->screencols, 79 - 17);

  /* ---- stacked ---- */
  window_init();
  buflist_init();
  win_screen();
  window_split(WIN_ROWS);
  win_screen();
  check("a stacked split has no border to either side",
        window_move_border(1, 0), 0);
  check("Up moves the border between them up", window_move_border(0, -1), 1);
  win_screen();
  check("so the upper window is a row shorter",
        window_view_at(0)->screenrows, 9);
  check("and the lower one starts a row earlier", window_view_at(1)->screeny, 11);
  while (window_move_border(0, -1)) win_screen();
  win_screen();
  check("Up stops at one row of text and a status bar",
        window_view_at(0)->screenrows, 1);
  while (window_move_border(0, 1)) win_screen();
  win_screen();
  check("Down stops at the same for the lower window",
        window_view_at(1)->screenrows, 1);

  /* Every press is one cell, at every width: the cut is stored in thousandths,
   * in thousandths stopped being exact past two thousand columns, and the
   * border stuck there for ever -- which is what 2047 is here for. A width it
   * skips a column at, or sticks at, is a border that cannot be put where the
   * user wants it. */
  ok = 1;
  for (i = 0; i < (int)(sizeof(widths) / sizeof(widths[0])); i++) {
    for (w = widths[i]; w < widths[i] + 3; w++) {
      window_init();
      buflist_init();
      window_layout_rect(0, 0, w, 10);
      window_split(WIN_COLS);
      window_focus(0);
      window_layout_rect(0, 0, w, 10);
      while (window_move_border(-1, 0)) window_layout_rect(0, 0, w, 10);
      window_layout_rect(0, 0, w, 10);
      prev = window_view_at(0)->screencols;
      if (prev != 8) ok = 0;
      while (window_move_border(1, 0)) {
        window_layout_rect(0, 0, w, 10);
        cut = window_view_at(0)->screencols;
        if (cut != prev + 1) ok = 0;
        prev = cut;
      }
      window_layout_rect(0, 0, w, 10);
      if (window_view_at(1)->screencols != 8) ok = 0;
    }
  }
  check("one press is one column, from minimum to minimum, at any width", ok, 1);
  win_screen();

  buflist_cleanup();
}

/* Find in files: the row parser, and the command line run for real.
 *
 * The command is what can go wrong quietly -- a quote that ends the pattern
 * early runs a different search, not a failing one -- so it is checked by
 * running it in a directory made for the purpose and reading what grep says,
 * rather than by comparing it with a string. */
static void grep_run(const char *pat, int icase, int regex, char *got, size_t n) {
  struct filter_io io;
  char *cmd = grep_command(pat, icase, regex);
  memset(&io, 0, sizeof(io));
  io.cmd = cmd;
  io.in = "";
  io.timeout_ms = 10000;
  filter_run(&io, NULL);
  snprintf(got, n, "%.*s", io.outlen, io.out != NULL ? io.out : "");
  filter_free(&io);
  free(cmd);
}

static void test_grep(void) {
  char tmpl[] = "/tmp/molly-grep-XXXXXX";
  char here[4096], got[1024];
  int p0, pl, ln;
  FILE *fp;

  printf("\nfind in files: reading a row of results\n");
  check("path:line:text parses", grep_parse_line("src/a.c:12:int x;", 17, &p0, &pl, &ln), 1);
  check("the path starts at the start", p0, 0);
  check("and ends at the colon", pl, 7);
  check("the line is the number", ln, 12);
  check("grep -r's ./ is not part of the path",
        (grep_parse_line("./a.c:3:x", 9, &p0, &pl, &ln), p0 * 100 + pl), 203);
  check("a colon in the text is the text's",
        (grep_parse_line("a.c:3:b:4:c", 11, &p0, &pl, &ln), pl * 100 + ln), 303);
  check("a colon in the path without digits after it is the path's",
        (grep_parse_line("c:x/a.c:7:y", 11, &p0, &pl, &ln), pl * 100 + ln), 707);
  check("a row with no line number is not a match",
        grep_parse_line("Binary file a matches", 21, &p0, &pl, &ln), 0);
  check("nor is line zero", grep_parse_line("a:0:x", 5, &p0, &pl, &ln), 0);
  check("nor a row that is only a path and a number",
        grep_parse_line("a:12", 4, &p0, &pl, &ln), 0);

  printf("\nfind in files: the command, run\n");
  if (getcwd(here, sizeof(here)) == NULL || mkdtemp(tmpl) == NULL ||
      chdir(tmpl) != 0) {
    check("a scratch directory to search", 0, 1);
    return;
  }
  fp = fopen("a.txt", "w");
  fputs("it's here\nabc\nA.C\n", fp);
  fclose(fp);

  grep_run("it's", 1, 0, got, sizeof(got));
  check_str("a quote in the pattern is searched for, not obeyed", got,
            "./a.txt:1:it's here\n");
  grep_run("a.c", 1, 0, got, sizeof(got));
  check_str("plain text: a dot is a dot, and case is folded", got,
            "./a.txt:3:A.C\n");
  grep_run("a.c", 0, 0, got, sizeof(got));
  check_str("with Match case it is not", got, "");
  grep_run("a.c", 0, 1, got, sizeof(got));
  check_str("pattern search: a dot is any character", got, "./a.txt:2:abc\n");
  grep_run("'; echo pwned; '", 1, 0, got, sizeof(got));
  check_str("a pattern cannot end the quoting and run something", got, "");

  /* The work-tree branch, where paths come back without the ./ and only
   * tracked files are searched. Skipped, and said so, without git. */
  if (system("git init -q . >/dev/null 2>&1 && git add a.txt >/dev/null 2>&1") == 0) {
    fp = fopen("untracked.txt", "w");
    fputs("it's not tracked\n", fp);
    fclose(fp);
    grep_run("it's", 1, 0, got, sizeof(got));
    check_str("in a work tree it is git grep, over tracked files", got,
              "a.txt:1:it's here\n");
  } else {
    printf("  skip  no git here, so the work-tree branch is not run\n");
  }

  if (chdir(here) != 0) check("back to where the tests started", 0, 1);
  snprintf(got, sizeof(got), "rm -rf %s", tmpl);
  if (system(got) != 0) printf("  note  could not remove %s\n", tmpl);
}

/* Closing a buffer that another window is showing.
 *
 * This is the one thing splits add to the buffer list, and it is the kind of
 * bug that does not announce itself: a window left pointing at a freed buffer
 * draws whatever is in that memory now, rather than failing anywhere near the
 * close. */
static void test_windows_buffer_closed(void) {
  printf("\nwindows: a buffer closing under another window\n");

  window_init();
  buflist_init();
  win_screen();
  buf_new();
  check("two buffers", buf_count(), 2);

  window_split(WIN_COLS);
  win_screen();
  buf_show(1);
  check("the focused window moved to the second buffer",
        V.buf == buf_at(1), 1);
  check("and the other window stayed on the first",
        window_view_at(0)->buf == buf_at(0), 1);

  {
    struct buffer *survivor = buf_at(1);
    buf_close(0);
    check("closing the first leaves one buffer", buf_count(), 1);
    check("and the window that was showing it moved",
          window_view_at(0)->buf == survivor, 1);
    check("as did the other one", window_view_at(1)->buf == survivor, 1);
  }

  /* Emptying the last buffer replaces it, so the same repointing has to happen
   * on that path too -- and it is the path where the replacement has to exist
   * before the old one is freed. */
  {
    struct buffer *before = buf_at(0);
    buf_close(0);
    check("closing the last one empties it instead", buf_count(), 1);
    check("into a different buffer", buf_at(0) != before, 1);
    check("that both windows are looking at",
          window_view_at(0)->buf == buf_at(0) &&
          window_view_at(1)->buf == buf_at(0), 1);
  }

  buflist_cleanup();
  check("cleanup leaves no window pointing at a buffer",
        window_view_at(0)->buf == NULL && window_view_at(1)->buf == NULL, 1);

  /* Stand the editor back up. These tests share one process, and the ones after
   * this assume a live current buffer -- tab_width_set() walks its rows, and
   * with the list torn down B.numrows is read through a null view buffer. */
  window_init();
  buflist_init();
}

/* ---- the buffer list ----------------------------------------------------- */

/* The list is testable without a terminal because nothing in it draws: what it
 * does is hold buffers, say which is current, and keep `V.buf` valid across
 * every rearrangement. That last part is the whole risk in the design, and it
 * is what most of these check -- a view pointing at freed or moved memory would
 * not show up as a failed assertion in the editor, it would show up as a crash
 * or as the wrong file's text, days later.
 */
static void test_buflist(void) {
  printf("\nbuffers: the list\n");

  window_init();
  buflist_init();

  check("a fresh editor has exactly one buffer", buf_count(), 1);
  check("and the view is looking at it", V.buf == buf_at(0), 1);
  check("which is the current one", buf_current(), 0);
  check("it is unnamed", buf_at(0)->filename == NULL, 1);
  check("and clean", buf_at(0)->dirty, 0);
  /* A new file gets Unix endings and a trailing newline, so neither is the
   * zero a memset would leave. */
  check("a new buffer writes LF endings", buf_at(0)->eol_crlf, 0);
  check("and ends with a newline", buf_at(0)->final_newline, 1);

  /* Growing past the initial capacity is where an array of structs would have
   * moved every buffer and left V.buf dangling. With pointers the address the
   * view holds has to survive it, so the list is grown well past its first
   * allocation and the pointer checked afterwards. */
  {
    struct buffer *first = buf_at(0);
    int i;
    for (i = 0; i < 40; i++) buf_new();
    check("the list grows", buf_count(), 41);
    check("and the first buffer has not moved", buf_at(0) == first, 1);
    check("so the view still points at it", V.buf == first, 1);
    check("and it is still the current one", buf_current(), 0);
  }

  /* Ids are unique and are not list indices. */
  {
    int i, clash = 0;
    int j;
    for (i = 0; i < buf_count(); i++)
      for (j = i + 1; j < buf_count(); j++)
        if (buf_at(i)->id == buf_at(j)->id) clash++;
    check("every buffer has its own id", clash, 0);
  }

  buflist_cleanup();
  check("cleanup empties the list", buf_count(), 0);
  check("and leaves no view buffer", V.buf == NULL, 1);

  printf("\nbuffers: closing\n");

  /* Closing one that is not current must not disturb which buffer is current,
   * even though the list shifts under it. */
  window_init();
  buflist_init();
  buf_new();
  buf_new();
  buf_show(1);
  check("three buffers, the middle one current", buf_current(), 1);
  {
    struct buffer *mid = buf_at(1);
    buf_close(0);
    check("closing an earlier one leaves two", buf_count(), 2);
    check("the current buffer is the same buffer", V.buf == mid, 1);
    check("at its new index", buf_current(), 0);
  }
  buflist_cleanup();

  /* Closing the current one moves to the next along. */
  window_init();
  buflist_init();
  buf_new();
  buf_new();
  buf_show(1);
  {
    struct buffer *third = buf_at(2);
    buf_close(1);
    check("closing the current one leaves two", buf_count(), 2);
    check("and moves to the next along", V.buf == third, 1);
    check("which is now the second", buf_current(), 1);
  }
  buflist_cleanup();

  /* Closing the last one in the list has no next, so it moves back. */
  window_init();
  buflist_init();
  buf_new();
  buf_show(1);
  {
    struct buffer *first = buf_at(0);
    buf_close(1);
    check("closing the last one moves back", V.buf == first, 1);
    check("leaving one buffer", buf_count(), 1);
  }
  buflist_cleanup();

  /* The only buffer is emptied rather than closed, because an editor with no
   * buffer has nothing to draw and every caller would need a null check for a
   * state the user cannot usefully be in. */
  window_init();
  buflist_init();
  {
    char *name = xstrdup("/tmp/whatever.txt");
    buf_at(0)->filename = name;
    buf_at(0)->dirty = 1;
    editor_insert_row(0, "some text", 9);
    check("the only buffer has content", row_count(), 1);
    buf_close(0);
    check("closing it leaves one buffer, not none", buf_count(), 1);
    check("the view still has somewhere to look", V.buf != NULL, 1);
    check("it is empty", row_count(), 0);
    check("unnamed", buf_at(0)->filename == NULL, 1);
    check("and clean", buf_at(0)->dirty, 0);
  }
  buflist_cleanup();

  /* An index nobody has is a no-op rather than a crash: the tab bar and the
   * mouse both compute one, and a stale click should do nothing. */
  window_init();
  buflist_init();
  check("closing a buffer that does not exist is harmless",
        buf_close(7), 0);
  check("as is showing one", (buf_show(7), buf_current()), 0);
  check("and asking for one", buf_at(-1) == NULL, 1);
  buflist_cleanup();

  printf("\nbuffers: finding and counting\n");

  window_init();
  buflist_init();
  buf_new();
  buf_at(0)->filename = xstrdup("/tmp/one.txt");
  buf_at(1)->filename = xstrdup("/tmp/two.txt");
  check("a path that is open is found", buf_find_path("/tmp/two.txt"), 1);
  check("one that is not is not", buf_find_path("/tmp/three.txt"), -1);
  check("and neither is nothing", buf_find_path(NULL), -1);
  check("no buffer is dirty yet", buf_dirty_count(), 0);
  buf_at(1)->dirty = 1;
  check("one is now", buf_dirty_count(), 1);
  buf_at(0)->dirty = 1;
  check("and then both", buf_dirty_count(), 2);
  buflist_cleanup();

  printf("\nbuffers: the remembered position\n");

  /* Switching away and back returns to where the cursor was, which is the whole
   * reason the position is kept on the buffer at all. */
  window_init();
  buflist_init();
  buf_new();
  buf_show(0);
  editor_insert_row(0, "one", 3);
  editor_insert_row(1, "two", 3);
  editor_insert_row(2, "three", 5);
  CUR.cy = 2;
  CUR.cx = 4;
  buf_show(1);
  check("a buffer switched to starts at the top", CUR.cy, 0);
  buf_show(0);
  check("and switching back returns to the row", CUR.cy, 2);
  check("and to the column", CUR.cx, 4);

  /* Clamped on arrival, because the buffer can be edited or reloaded shorter
   * while it is not the one being shown. */
  buf_show(1);
  while (row_count() > 0) editor_delete_row(0);
  buf_show(0);
  editor_delete_row(2);
  editor_delete_row(1);
  buf_show(1);
  buf_show(0);
  check("a remembered row past the end clamps", CUR.cy <= row_count(), 1);
  check("and so does the column",
        CUR.cy < row_count() ? CUR.cx <= row_at(CUR.cy)->size : CUR.cx == 0, 1);

  buflist_cleanup();

  /* Leave one buffer behind, because every test after this one expects an
   * editor with somewhere to put text. */
  window_init();
  buflist_init();
}

/* ---- syntax highlighting -------------------------------------------------- */

/* The lexer, against a role-per-character sketch of the expected answer.
 *
 * A row is written out with one letter per character of the input, which makes
 * the expectation readable as a line of its own and makes a failure point at the
 * column that is wrong rather than at a number:
 *
 *     "int x = 1;"
 *     "ttt.......t"   <- would be wrong; the trailing t has no character
 *
 * The letters are the first letter of the role, with 'w' for keyword (k is
 * taken by nothing but reads worse) and '.' for plain text. Anything the lexer
 * produces that has no letter here shows up as '?', so a new role cannot pass a
 * test by being invisible.
 */
static char syn_letter(unsigned char role) {
  switch (role) {
    case ROLE_TEXT:           return '.';
    case ROLE_SYN_KEYWORD:    return 'w';
    case ROLE_SYN_TYPE:       return 't';
    case ROLE_SYN_STRING:     return 's';
    case ROLE_SYN_NUMBER:     return 'n';
    case ROLE_SYN_COMMENT:    return 'c';
    case ROLE_SYN_PREPROC:    return 'p';
    case ROLE_SYN_HEADING:    return 'h';
    case ROLE_SYN_EMPHASIS:   return 'e';
    default:                  return '?';
  }
}

/* Lex one row and compare the sketch. `state` is carried in and out so a
 * multi-row case is a sequence of calls. */
static int syn_check(const struct syntax *lang, const char *text,
                     const char *want, int *state) {
  unsigned char roles[512];
  char got[513];
  int n = (int)strlen(text);
  int i, out = 0;
  char label[256];

  if (n > (int)sizeof(roles)) n = (int)sizeof(roles);
  syntax_row(lang, text, n, state ? *state : 0, roles, &out);
  for (i = 0; i < n; i++) got[i] = syn_letter(roles[i]);
  got[n] = '\0';
  if (state != NULL) *state = out;

  snprintf(label, sizeof(label), "%.60s", text);
  /* A sketch of the wrong length is a miscount in the test, not a disagreement
   * about roles -- and left unchecked it reads as the lexer being wrong from the
   * first column onwards. */
  if ((int)strlen(want) != n) {
    printf("  FAIL  %s\n          sketch is %d chars, row is %d\n",
           label, (int)strlen(want), n);
    failures++;
    return out;
  }
  check_str(label, got, want);
  return out;
}

static const struct syntax *syn_lang(const char *path) {
  return syntax_for_filename(path);
}

static void test_syntax_c(void) {
  const struct syntax *c = syn_lang("x.c");
  int st = 0;
  printf("\nsyntax: C\n");

  check("a .c file gets a definition", c != NULL, 1);
  check_str("and it is the C one", syntax_name(c), "c");

  syn_check(c, "int x = 1;",        "ttt.....n.", NULL);
  syn_check(c, "return 0;",         "wwwwww.n.", NULL);
  syn_check(c, "if (a) b();",       "ww.........", NULL);
  /* A keyword has to be a whole word: "iffy" is not "if". */
  syn_check(c, "iffy = 1;",         ".......n.", NULL);
  syn_check(c, "x_if = 1;",         ".......n.", NULL);
  /* Nor is a word that merely contains one. */
  syn_check(c, "printf(\"%d\", x);", ".......ssss.....", NULL);
  /* A number has to start a word, so the 1 in x1 is an identifier. */
  syn_check(c, "x1 = 1;",           ".....n.", NULL);
  syn_check(c, "1.5e-3;",           "nnnnnn.", NULL);
  /* A line comment outranks a string, so this is not an open string. */
  syn_check(c, "// \"unclosed",     "cccccccccccc", NULL);
  /* And a string outranks a keyword. */
  syn_check(c, "\"if\"",            "ssss", NULL);
  /* An escape keeps the string open past the quote it escapes. */
  syn_check(c, "\"a\\\"b\" x",      "ssssss..", NULL);
  /* A preprocessor line, which has to be line-leading. */
  syn_check(c, "#include <stdio.h>", "pppppppppppppppppp", NULL);
  /* The indentation before a directive is not part of it. */
  syn_check(c, "  #define X 1",     "..ppppppppppp", NULL);
  syn_check(c, "x = a#b;",          "........", NULL);

  printf("\nsyntax: a comment that spans rows\n");

  /* The whole reason rows carry a lexer state. */
  st = 0;
  syn_check(c, "/* open",           "ccccccc", &st);
  check("the row leaves inside a comment", st != 0, 1);
  syn_check(c, "int still comment", "ccccccccccccccccc", &st);
  check("and so does the next", st != 0, 1);
  syn_check(c, "closed */ int x;",  "ccccccccc.ttt...", &st);
  check("the closer ends it", st, 0);
  /* Code after the closer on the same row is code again. */
  syn_check(c, "/* a */ return 1;", "ccccccc.wwwwww.n.", NULL);
  /* An unterminated string does NOT span rows: a stray quote should not paint
   * the rest of the file. */
  st = 0;
  syn_check(c, "\"oops",            "sssss", &st);
  check("an unterminated string ends at the row", st, 0);
}

static void test_syntax_python(void) {
  const struct syntax *py = syn_lang("x.py");
  int st = 0;
  printf("\nsyntax: Python\n");

  check_str("a .py file gets Python", syntax_name(py), "python");
  syn_check(py, "def f(x):",        "www......", NULL);
  syn_check(py, "# a comment",      "ccccccccccc", NULL);
  syn_check(py, "s = 'text'",       "....ssssss", NULL);
  syn_check(py, "n = 42",           "....nn", NULL);

  /* Triple quotes are a region, which is what the `multiline strings` rule is
   * for -- and the single-quote rule must not claim the first two of the three. */
  st = 0;
  syn_check(py, "\"\"\"docstring",  "ssssssssssss", &st);
  check("a triple quote opens a region", st != 0, 1);
  syn_check(py, "still inside",     "ssssssssssss", &st);
  syn_check(py, "done\"\"\" x",     "sssssss..", &st);
  check("and the closing triple ends it", st, 0);
}

static void test_syntax_autoit(void) {
  const struct syntax *au = syn_lang("x.au3");
  int st = 0;
  printf("\nsyntax: AutoIt\n");

  check_str("a .au3 file gets AutoIt", syntax_name(au), "autoit");
  /* Case-insensitive keywords, which is why `case sensitive = off` exists. */
  syn_check(au, "If x Then",        "ww...wwww", NULL);
  syn_check(au, "IF x THEN",        "ww...wwww", NULL);
  syn_check(au, "if x then",        "ww...wwww", NULL);
  syn_check(au, "; a comment",      "ccccccccccc", NULL);

  /* #cs / #ce, one of the two things that make a cached per-row state
   * unavoidable rather than merely correct. */
  st = 0;
  syn_check(au, "#cs",              "ccc", &st);
  check("#cs opens a region", st != 0, 1);
  syn_check(au, "If this were code", "ccccccccccccccccc", &st);
  syn_check(au, "#ce",              "ccc", &st);
  check("#ce closes it", st, 0);
  /* The long spelling is the second pair. */
  st = 0;
  syn_check(au, "#comments-start",  "ccccccccccccccc", &st);
  check("#comments-start opens one too", st != 0, 1);
  syn_check(au, "#comments-end",    "ccccccccccccc", &st);
  check("and #comments-end closes it", st, 0);
  /* A directive that is not a comment opener is still a directive. */
  syn_check(au, "#include <x.au3>", "pppppppppppppppp", NULL);
}

static void test_syntax_markdown(void) {
  const struct syntax *md = syn_lang("x.md");
  int st = 0;
  printf("\nsyntax: Markdown\n");

  check_str("a .md file gets Markdown", syntax_name(md), "markdown");
  syn_check(md, "# Heading",        "hhhhhhhhh", NULL);
  syn_check(md, "### Deeper",       "hhhhhhhhhh", NULL);
  syn_check(md, "not a # heading",  "...............", NULL);
  syn_check(md, "a *word* here",    "..eeeeee.....", NULL);
  syn_check(md, "a `span` here",    "..ssssss.....", NULL);
  /* A lone delimiter is prose, not an opener that never closes. */
  syn_check(md, "2 * 3 is six",     "............", NULL);
  /* Markdown has `numbers = off`: a number in prose is prose. */
  syn_check(md, "there were 42",    ".............", NULL);

  /* A fenced block, the other thing that forces the cached state. */
  st = 0;
  syn_check(md, "```c",             "ssss", &st);
  check("a fence opens a region", st != 0, 1);
  syn_check(md, "# not a heading",  "sssssssssssssss", &st);
  check("and a heading inside it is not one", st != 0, 1);
  syn_check(md, "```",              "sss", &st);
  check("the closing fence ends it", st, 0);
  syn_check(md, "# heading again",  "hhhhhhhhhhhhhhh", NULL);
}

static void test_syntax_misc(void) {
  int st = 0;
  printf("\nsyntax: the rest of the seven, and no filetype\n");

  check_str("a .rs file gets Rust", syntax_name(syn_lang("x.rs")), "rust");
  check_str("a .cpp file gets C++", syntax_name(syn_lang("x.cpp")), "cpp");
  check_str("a .hpp file gets C++", syntax_name(syn_lang("x.hpp")), "cpp");
  check_str("a .cs file gets C#", syntax_name(syn_lang("x.cs")), "cs");
  /* A .h is C, not C++, which is a judgement rather than a fact -- it is the
   * one extension both claim, and C is the one molly itself is written in. */
  check_str("a .h file gets C", syntax_name(syn_lang("x.h")), "c");
  check("all seven definitions are loaded", syntax_lang_count(), 7);

  /* Extensions are matched without regard to case, and a path's directories
   * must not be mistaken for one. */
  check_str("an extension matches whatever its case",
            syntax_name(syn_lang("X.PY")), "python");
  check_str("and a full path still finds it",
            syntax_name(syn_lang("/a/b.c/thing.rs")), "rust");

  check("a file with no extension gets nothing",
        syn_lang("Makefile") == NULL, 1);
  check("nor does an unknown one", syn_lang("x.zzz") == NULL, 1);
  check("nor does a trailing dot", syn_lang("x.") == NULL, 1);
  check("and neither does nothing at all", syn_lang(NULL) == NULL, 1);

  /* No filetype means every byte is plain, which is what lets the draw loop
   * have no special case for highlighting being off. */
  syn_check(NULL, "int x = 1;", "..........", &st);
  check("and lexing with no definition leaves no state", st, 0);

  printf("\nsyntax: a rule molly does not know\n");
  {
    const char *err = NULL;
    check("an unknown rule is refused",
          syntax_set_line("c", "sparkles", "on", &err), 0);
    check_str("and says so", err, "unknown syntax rule");
    err = NULL;
    check("a block comment with an unpaired marker is refused",
          syntax_set_line("c", "block comment", "/*", &err), 0);
    err = NULL;
    check("and a flag that is not a flag",
          syntax_set_line("c", "numbers", "maybe", &err), 0);
    check_str("with its own message", err, "numbers wants on or off");
  }

  /* Leave the definitions as the rest of the suite expects to find them: the
   * refusals above are harmless, but a test that adds a rule should not leak it.
   */
  syntax_init();
}

/* The indentation model: where one indent step gets its shape, and what the two
 * editing operations do with it.
 *
 * A unit test rather than a pty session for the same reason the shell filter's
 * limits are: what has to be proved is the resolution -- filetype over default,
 * unset falling through -- and that is a question about three functions, not
 * about what a terminal shows. The keystrokes get a pty test of their own. */
static void test_indent(void) {
  const char *err = NULL;

  printf("\nindentation: where a step gets its shape\n");

  window_init();
  buflist_init();
  syntax_init();

  /* No filetype: the [editor] defaults answer everything. */
  free(B.filename);
  B.filename = NULL;
  syntax_retype();
  check("with no filetype the default width answers", tab_width(), 8);
  check("and the default flags", indent_insert_spaces(), 0);
  check("auto-indent is on out of the box", indent_auto(), 1);

  /* A filetype that says nothing about indentation still defers. */
  free(B.filename);
  B.filename = xstrdup("x.c");
  syntax_retype();
  check("a definition with no indent rule leaves the default", tab_width(), 8);
  check("and does not invent a flag", indent_insert_spaces(), 0);

  /* ...and one that does, wins. */
  check("a per-filetype width is accepted",
        syntax_set_line("c", "tab width", "4", &err), 1);
  check("and a per-filetype flag",
        syntax_set_line("c", "insert spaces", "on", &err), 1);
  check("and the other one",
        syntax_set_line("c", "auto indent", "off", &err), 1);
  syntax_retype();
  check("the filetype's width is what is in force", tab_width(), 4);
  check("and the filetype's flags", indent_insert_spaces(), 1);
  check("including the one that turns a default off", indent_auto(), 0);

  /* The default is still the default, which is what write-back saves: the
   * override must not become everybody's the first time settings are saved
   * with a .c file open. */
  check("the global default is untouched", tab_width_default(), 8);
  check("and so are the global flags", indent_insert_spaces_default(), 0);
  check("both of them", indent_auto_default(), 1);

  /* A file of another type is unaffected by C's override. */
  free(B.filename);
  B.filename = xstrdup("x.py");
  syntax_retype();
  check("another filetype keeps the default width", tab_width(), 8);
  check("and the default flags", indent_insert_spaces(), 0);

  printf("\nindentation: a rule that is not one\n");
  err = NULL;
  check("a width of zero is refused",
        syntax_set_line("c", "tab width", "0", &err), 0);
  check_str("and says so", err, "tab width wants 1 to 16");
  err = NULL;
  check("so is one past the renderer's limit",
        syntax_set_line("c", "tab width", "17", &err), 0);
  err = NULL;
  check("and a flag that is not a flag",
        syntax_set_line("c", "insert spaces", "sometimes", &err), 0);
  check_str("with its own message", err, "insert spaces wants on or off");
  err = NULL;
  check("and the other flag the same",
        syntax_set_line("c", "auto indent", "maybe", &err), 0);
  check_str("also with its own", err, "auto indent wants on or off");

  printf("\nindentation: the render follows the width\n");
  syntax_init();
  free(B.filename);
  B.filename = xstrdup("x.c");
  syntax_retype();
  editor_insert_row(0, "\tx", 2);
  check("a tab renders at the default width", B.row[0].rsize, 9);

  /* The width a filetype names reaches rows that were already rendered. This
   * is what the per-buffer `tabw` exists for: without the re-render the cursor
   * arithmetic would measure against a render string built at the old width. */
  check("giving the filetype a width is accepted",
        syntax_set_line("c", "tab width", "4", &err), 1);
  syntax_retype();
  check("and the row is re-rendered at it", B.row[0].rsize, 5);
  check("which the buffer records", B.tabw, 4);

  /* Moving the default does not move a buffer whose filetype named its own. */
  tab_width_set(2);
  check("the default moved", tab_width_default(), 2);
  check("but this buffer did not", B.row[0].rsize, 5);
  check("because the filetype still answers", tab_width(), 4);
  tab_width_set(8);

  printf("\nindentation: one step at the cursor\n");
  syntax_init();
  free(B.filename);
  B.filename = NULL;
  syntax_retype();
  while (B.numrows > 0) editor_delete_row(0);
  editor_insert_row(0, "ab", 2);
  CUR.cy = 0;
  CUR.cx = 2;

  /* With the flag off a step is the tab character it has always been. */
  indent_set_insert_spaces(0);
  indent_insert_step();
  check("a step with the flag off is one tab", B.row[0].size, 3);
  check("and it is a tab", B.row[0].chars[2] == '\t', 1);
  editor_undo();
  check("which undoes in one step", B.row[0].size, 2);

  /* With it on, a step reaches the next stop rather than inserting a fixed
   * number of spaces -- so from column 2 of an 8-column stop it is six. */
  indent_set_insert_spaces(1);
  CUR.cx = 2;
  indent_insert_step();
  check("a step with the flag on reaches the next stop", B.row[0].size, 8);
  check_str("in spaces", B.row[0].chars, "ab      ");
  editor_undo();
  check("and the run undoes as one step", B.row[0].size, 2);

  /* From a stop, a step is a whole width. */
  CUR.cx = 0;
  while (B.numrows > 0) editor_delete_row(0);
  editor_insert_row(0, "", 0);
  CUR.cy = 0;
  CUR.cx = 0;
  indent_insert_step();
  check("from column zero it is a whole width", B.row[0].size, 8);
  indent_set_insert_spaces(0);

  printf("\nindentation: a new line under the old one\n");
  while (B.numrows > 0) editor_delete_row(0);
  editor_insert_row(0, "    ab", 6);
  CUR.cy = 0;
  CUR.cx = 6;
  indent_set_auto(1);
  editor_newline_and_indent();
  check("the new row carries the indentation down", B.numrows, 2);
  check_str("as the same whitespace", B.row[1].chars, "    ");
  check("and the cursor sits after it", CUR.cx, 4);

  /* One undo step for the whole gesture: the split and the indent together.
   * Two steps would mean Backspace leaving the indentation of a line you had
   * just decided not to start. */
  editor_undo();
  check("and the whole gesture undoes at once", B.numrows, 1);
  check_str("leaving the row as it was", B.row[0].chars, "    ab");

  /* Enter inside the leading whitespace. The rule is that the text that moves
   * down keeps the column it was at: only the whitespace behind the cursor is
   * copied, and the tail keeps its own -- so "    ab" split after two spaces
   * leaves "ab" at column four, where it started. The references disagree
   * about this one, so it is pinned here and in the pty suite. */
  CUR.cy = 0;
  CUR.cx = 2;
  editor_newline_and_indent();
  check_str("a split inside the indent leaves the text in its column",
            B.row[1].chars, "    ab");
  check("and the cursor is at the indent it copied", CUR.cx, 2);
  editor_undo();

  /* The flag off is the old behaviour exactly. */
  indent_set_auto(0);
  CUR.cy = 0;
  CUR.cx = 6;
  editor_newline_and_indent();
  check_str("with auto-indent off the new row is empty", B.row[1].chars, "");
  editor_undo();

  /* And the mechanism underneath never indents, whoever calls it. This is what
   * keeps paste and the shell filter from indenting text that arrives indented
   * already -- both split rows through editor_insert_newline(). */
  indent_set_auto(1);
  CUR.cy = 0;
  CUR.cx = 6;
  editor_insert_newline();
  check_str("the plain split does not indent, even with the flag on",
            B.row[1].chars, "");
  editor_undo();

  /* Leave the editor as the rest of the suite expects to find it. */
  indent_set_auto(1);
  indent_set_insert_spaces(0);
  tab_width_set(8);
  syntax_init();
  window_init();
  buflist_init();
}

/* N cursors: the list staying ordered and distinct, and one edit landing at
 * every one of them.
 *
 * A unit test and not a pty session because what has to be proved is the
 * position arithmetic -- that a cursor still to come survives an edit earlier
 * in the document -- and that is a question about the buffer afterwards, not
 * about what a terminal drew. The keystrokes get their own pty tests. */

/* Put the editor in a known state with `body` in the buffer and one cursor. */
static void mc_load(const char *body) {
  window_init();
  buflist_init();
  syntax_init();
  free(B.filename);
  B.filename = NULL;
  syntax_retype();
  while (B.numrows > 0) editor_delete_row(0);
  {
    const char *p = body;
    while (*p != '\0') {
      const char *nl = strchr(p, '\n');
      int n = nl ? (int)(nl - p) : (int)strlen(p);
      editor_insert_row(row_count(), (char *)p, (size_t)n);
      if (!nl) break;
      p = nl + 1;
    }
  }
  V.ncursors = 1;
  V.primary = 0;
  memset(&V.cursors[0], 0, sizeof(struct cursor));
  V.cursorseq = 0;
  B.undo.locked = 0;
}

/* The buffer as one string, which is what an edit at N cursors is judged by. */
static char *mc_text(void) {
  static char out[4096];
  int i, n = 0;
  out[0] = '\0';
  for (i = 0; i < B.numrows; i++) {
    int len = B.row[i].size;
    if (n + len + 2 > (int)sizeof(out)) break;
    memcpy(out + n, B.row[i].chars, (size_t)len);
    n += len;
    if (i + 1 < B.numrows) out[n++] = '\n';
  }
  out[n] = '\0';
  return out;
}

/* Select [sx, cx) of row y on the primary, which is what Ctrl-D's first press
 * leaves behind and what the three commands below all start from. */
static void mc_select(int y, int sx, int cx) {
  CUR.cy = y;
  CUR.sy = y;
  CUR.sx = sx;
  CUR.cx = cx;
  CUR.sel_active = 1;
}

static void test_cursors_more(void) {
  printf("\nmulti-cursor: skipping a match, and taking one back\n");

  mc_load("red fish\nblue fish\nred bird\n");

  /* The order a cursor was added in is not its place in the list: this match
   * wraps to the top of the file, so the newer cursor sorts first. */
  mc_select(2, 0, 3);
  check("a cursor added later is the newest", cursors_add_sel(0, 0, 3), 1);
  cursors_sort();
  check("even though it sorts first", V.cursors[0].cy, 0);
  check("which is the one the newest is", cursors_newest(), 0);

  /* Skip: the newest cursor moves rather than another arriving. */
  mc_load("red fish\nblue fish\nred bird\n");
  mc_select(0, 0, 3);
  check("skip finds the next occurrence", cursors_skip_next(), 1);
  check("and does not add a cursor", V.ncursors, 1);
  check("the cursor is on the next match", CUR.cy, 2);
  check("with it selected", CUR.sx, 0);
  check("to its end", CUR.cx, 3);
  check("skipping again wraps to the first", cursors_skip_next(), 1);
  check("which is where it started", CUR.cy, 0);
  check("still one cursor", V.ncursors, 1);

  /* Skip with two cursors moves only the newest, which is what makes it a way
   * out of a match you did not want rather than a way to lose the ones you do. */
  mc_load("red fish\nred bird\nred fox\n");
  mc_select(0, 0, 3);
  check("a second cursor arrives", cursors_add_next(), 1);
  check("two of them", V.ncursors, 2);
  check("skip moves the newest one", cursors_skip_next(), 1);
  check("and leaves the count alone", V.ncursors, 2);
  cursors_sort();
  check("the first cursor has not moved", V.cursors[0].cy, 0);
  check("the newest is on the third match", V.cursors[1].cy, 2);

  /* And when the next match is one somebody already has, the two merge rather
   * than both editing the same place -- the same rule the rest of the list
   * lives by, reached from a new direction. */
  mc_load("red fish\nred bird\n");
  mc_select(0, 0, 3);
  check("two cursors on the two matches", cursors_add_next(), 1);
  check("skipping the newest comes back round", cursors_skip_next(), 1);
  check("onto the other one, and they merge", V.ncursors, 1);

  /* Drop: the newest goes, whatever its place in the list. */
  mc_load("red fish\nblue fish\nred bird\n");
  mc_select(2, 0, 3);
  check("a cursor is added by wrapping", cursors_add_next(), 1);
  cursors_sort();
  check("and sorts first", cursors_newest(), 0);
  check("dropping it says so", cursors_drop_last(), 1);
  check("one cursor is left", V.ncursors, 1);
  check("and it is the one that was there first", CUR.cy, 2);
  check("dropping the last cursor does nothing", cursors_drop_last(), 0);
  check("because there is only one", V.ncursors, 1);
  check("and the numbering is back to the start", V.cursorseq, 0);

  /* Every match at once, from no selection at all. */
  mc_load("red fish\nblue fish\nred bird\n");
  CUR.cy = 0;
  CUR.cx = 1;
  check("all matches takes the word under the cursor", cursors_add_all(), 1);
  check("and puts a cursor on every one", V.ncursors, 2);
  check("running it again adds nothing", cursors_add_all(), 1);
  check("because they are all taken", V.ncursors, 2);
  cursors_edit(MC_INSERT, 'X');
  check_str("one keystroke reaches every match", mc_text(),
            "X fish\nblue fish\nX bird");

  /* A word that occurs once is not an error, it is one cursor. */
  mc_load("alpha\nbeta\n");
  CUR.cy = 1;
  CUR.cx = 0;
  check("all matches on a lone word", cursors_add_all(), 1);
  check("leaves one cursor", V.ncursors, 1);
}

/* Converting the indentation a file already has. */
static void test_indent_convert(void) {
  printf("\nindentation: converting what is there\n");

  mc_load("\talpha\n  \tbeta\ngamma\n");
  check("to spaces changes the two indented rows", indent_convert(1), 2);
  check_str("a tab is the spaces it showed as, and a tab after spaces is "
            "the rest of the stop", mc_text(),
            "        alpha\n        beta\ngamma");
  check("converting again changes nothing", indent_convert(1), 0);
  editor_undo();
  check_str("and one undo puts both rows back", mc_text(),
            "\talpha\n  \tbeta\ngamma");

  mc_load("        x\n          y\n    z\n");
  check("to tabs changes the rows a tab stop fits in", indent_convert(0), 2);
  check_str("whole stops become tabs and the rest stay spaces", mc_text(),
            "\tx\n\t  y\n    z");

  mc_load("\ta\tb\n");
  indent_convert(1);
  check_str("a tab after the text is the text's, and stays", mc_text(),
            "        a\tb");

  mc_load("\t\n  \t  \nx\n");
  check("a row of nothing but whitespace is not indentation",
        indent_convert(1), 0);

  mc_load("\ta\n\tb\n\tc\n");
  mc_select(1, 0, 1);
  CUR.cy = 2;
  CUR.cx = 0;
  indent_convert(1);
  check_str("with a selection, only its rows", mc_text(),
            "\ta\n        b\n\tc");

  /* The cursor keeps its character when it is in the text. */
  mc_load("\tabc\n");
  CUR.cy = 0;
  CUR.cx = 2;
  indent_convert(1);
  check("a cursor in the text keeps its character", CUR.cx, 9);

  tab_width_set(4);
  mc_load("\t\tx\n      y\n");
  indent_convert(1);
  check_str("at the width in force for the file", mc_text(),
            "        x\n      y");
  indent_convert(0);
  check_str("and back at the same width", mc_text(), "\t\tx\n\t  y");
  tab_width_set(8);
}

static void test_indent_block(void) {
  printf("\nindentation: a block, and one level at a time\n");

  /* Width 8 and tabs, which are the defaults. */
  mc_load("alpha\nbeta\ngamma\n");
  mc_select(0, 0, 0);
  CUR.cy = 1;
  CUR.cx = 1;
  indent_shift(1);
  check_str("a selection over two rows indents both", mc_text(),
            "\talpha\n\tbeta\ngamma");
  indent_shift(-1);
  check_str("and dedenting puts them back", mc_text(),
            "alpha\nbeta\ngamma");

  /* The rule those two fixtures tripped on, written down: a selection that
   * stops at column zero of a row has not reached into that row. */
  mc_load("alpha\nbeta\n");
  mc_select(0, 0, 0);
  CUR.cy = 1;
  CUR.cx = 0;
  indent_shift(1);
  check_str("a selection stopping at column zero leaves that row", mc_text(),
            "\talpha\nbeta");

  /* The row the cursor is on, when nothing is selected. */
  mc_load("\talpha\n\tbeta\n");
  CUR.sel_active = 0;
  CUR.cy = 1;
  CUR.cx = 0;
  indent_shift(-1);
  check_str("with no selection it is the cursor's row", mc_text(),
            "\talpha\nbeta");

  /* One row per cursor, which is the multi-cursor shape of the same gesture. */
  mc_load("\talpha\n\tbeta\n\tgamma\n");
  CUR.cy = 0;
  check("a cursor on the third row", cursors_add(2, 0), 1);
  indent_shift(-1);
  check_str("every cursor's row is dedented, and only those", mc_text(),
            "alpha\n\tbeta\ngamma");

  /* A blank row inside a block is left alone: an indent on nothing is
   * trailing whitespace. */
  mc_load("alpha\n\nbeta\n");
  mc_select(0, 0, 0);
  CUR.cy = 2;
  CUR.cx = 1;
  indent_shift(1);
  check_str("a blank row in a block is not indented", mc_text(),
            "\talpha\n\n\tbeta");

  /* The dedent rule: back to the previous stop, whole characters off the end
   * of the run nearest the text. */
  tab_width_set(4);
  mc_load("      alpha\n");
  CUR.sel_active = 0;
  indent_shift(-1);
  check_str("six spaces at a width of four go back to four", mc_text(),
            "    alpha");
  indent_shift(-1);
  check_str("and then to none", mc_text(), "alpha");
  indent_shift(-1);
  check_str("dedenting nothing is not an error", mc_text(), "alpha");

  mc_load("\t  alpha\n");
  CUR.sel_active = 0;
  indent_shift(-1);
  check_str("a tab and two spaces lose the spaces", mc_text(), "\talpha");
  indent_shift(-1);
  check_str("and then the tab, whole", mc_text(), "alpha");

  /* The whole block is one undo step, including the rows that did not move. */
  mc_load("alpha\nbeta\n");
  mc_select(0, 0, 0);
  CUR.cy = 1;
  indent_shift(1);
  editor_undo();
  check_str("a block undoes in one step", mc_text(), "alpha\nbeta");

  /* Smart Backspace: one level, and only in the whitespace. */
  tab_width_set(4);
  mc_load("      alpha\n");
  CUR.sel_active = 0;
  CUR.cx = 6;
  check("backspace in the indent handles it", indent_backspace(), 1);
  check_str("and eats back to the stop", mc_text(), "    alpha");
  check("the cursor follows", CUR.cx, 4);
  check("again from the stop", indent_backspace(), 1);
  check_str("takes a whole level", mc_text(), "alpha");
  check("and with nothing in front of it", indent_backspace(), 0);

  mc_load("    alpha\n");
  CUR.sel_active = 0;
  CUR.cx = 6;
  check("backspace in the text is not this", indent_backspace(), 0);
  check_str("and the row is untouched", mc_text(), "    alpha");

  mc_load(" alpha\n");
  CUR.sel_active = 0;
  CUR.cx = 1;
  check("one space is left to the ordinary backspace", indent_backspace(), 0);

  mc_load("\t\talpha\n");
  CUR.sel_active = 0;
  CUR.cx = 2;
  check("and so is one tab, which is already one level",
        indent_backspace(), 0);

  tab_width_set(8);
}

/* Soft wrap: the display-row map, and vertical movement over it.
 *
 * A unit test because what has to be proved is arithmetic -- how many screen
 * lines a row takes, which slice of it a line shows, and which of them a cursor
 * is on -- and because a pty session can only show the answer at whatever width
 * the terminal happens to be. The screens get goldens and the keystrokes get
 * pty checks; the rules are here. */

/* Put `body` in the buffer with wrap on and a text area `cols` wide. The view
 * is laid out by hand: window_layout() asks the terminal, and these tests are
 * about a width they choose. */
static void wrap_load(const char *body, int cols) {
  mc_load(body);
  V.wrap = 1;
  V.gutter_w = 0;
  V.screencols = cols;
  V.screenrows = 10;
  V.coloff = 0;
  V.drowoff = 0;
}

/* The slice a display row shows, as the three numbers it is made of. */
static int slice_rcol0(int index) {
  struct drow d;
  drow_at(index, &d);
  return d.rcol0;
}
static int slice_rcols(int index) {
  struct drow d;
  drow_at(index, &d);
  return d.rcols;
}
static int slice_row(int index) {
  struct drow d;
  drow_at(index, &d);
  return d.filerow;
}
static int slice_frag(int index) {
  struct drow d;
  drow_at(index, &d);
  return d.frag;
}

/* Which display row the one cursor is on, and where in it. */
static int cur_drow(void) { return drow_of_cursor(&CUR); }

static void test_wrap(void) {
  printf("\nsoft wrap: how many screen lines a row takes, and which is which\n");

  /* Off, which is the mapping every other test in this file is written
   * against: one display row per file row, each the whole of it. */
  wrap_load("alpha\nbeta\ngamma\n", 10);
  V.wrap = 0;
  check("wrap off: a display row per file row", drow_total(), 3);
  check("wrap off: the mapping is the identity", slice_row(2), 2);
  check("wrap off: and a slice is the whole row", slice_rcols(0), DROW_ALL);

  /* On, with nothing long enough to wrap: the same answers. */
  wrap_load("alpha\nbeta\ngamma\n", 10);
  check("short rows take one line each", drow_total(), 3);
  check("and the slice is still the whole row", slice_rcols(1), DROW_ALL);
  check("an empty buffer has no display rows", (wrap_load("", 10), drow_total()), 0);

  /* A run with no space in it has to break at the edge: 200 columns at 80 is
   * three lines, the first two exactly a window wide. */
  {
    char body[256];
    memset(body, 'x', 200);
    body[200] = '\n';
    body[201] = '\0';
    wrap_load(body, 80);
    check("an unbroken run breaks at the edge", drow_total(), 3);
    check("the first line starts at column zero", slice_rcol0(0), 0);
    check("and is a window wide", slice_rcols(0), 80);
    check("the second starts where it ended", slice_rcol0(1), 80);
    check("the last runs to the end of the row", slice_rcols(2), DROW_ALL);
    check("every line of it is the same file row", slice_row(2), 0);
  }

  /* Words: the break goes after the last space that fits, and a word too long
   * for the width breaks at the edge anyway. "aa bbbbbbbb cc" at eight columns
   * is "aa ", then the eight b's, then " cc". */
  wrap_load("aa bbbbbbbb cc\n", 8);
  check("a line breaks after the last space that fits", drow_total(), 3);
  check("the first slice ends after the space", slice_rcols(0), 3);
  check("the word too long to fit takes the edge", slice_rcol0(1), 3);
  check("and all eight columns of it", slice_rcols(1), 8);
  check("what is left is the last slice", slice_rcol0(2), 11);

  /* A tab is a break opportunity without being named one: it is spaces in the
   * render string by the time this code sees it. */
  tab_width_set(8);
  indent_sync();
  wrap_load("\tfoo bar\n", 10);
  check("an indent is a break opportunity", drow_total(), 2);
  check("so the indent is a line of its own", slice_rcols(0), 8);
  check("and the text starts the next one", slice_rcol0(1), 8);

  /* A glyph wider than the whole window still has to make progress, or the
   * walk would never reach the end of the row. */
  wrap_load("\xe6\x97\xa5\xe6\x9c\xac\n", 1);
  check("a glyph wider than the window takes a line", drow_total(), 2);
  check("one for each", slice_rcol0(1), 2);

  /* Several rows: the total is the sum, and the index walks them in order. */
  wrap_load("aa bbbbbbbb cc\nshort\n", 8);
  check("the total is the sum over the rows", drow_total(), 4);
  check("the fourth display row is the second file row", slice_row(3), 1);
  check("past the end comes a phantom row", slice_row(4), 2);
  check("and another after that", slice_row(5), 3);

  /* Where a cursor is. The row is "aa bbbbbbbb cc" in three slices, and the
   * column a cursor rests at decides which of them it is on. */
  wrap_load("aa bbbbbbbb cc\n", 8);
  CUR.cy = 0; CUR.cx = 0;
  check("a cursor at the start is on the first line", cur_drow(), 0);
  CUR.cx = 2;
  check("the space it broke after is still on it", cur_drow(), 0);
  CUR.cx = 3;
  check("the column it broke at belongs to the next line", cur_drow(), 1);
  CUR.cx = 10;
  check("the last column of a slice is on it", cur_drow(), 1);
  CUR.cx = 11;
  check("and the next column is not", cur_drow(), 2);
  CUR.cx = 14;
  check("the end of the row is on its last line", cur_drow(), 2);
  check("the column within that line is measured from it",
        drow_cursor_col(&CUR), 3);
  CUR.cy = 1; CUR.cx = 0;
  check("the phantom row past the end is a display row too", cur_drow(), 3);

  /* Vertical movement. Down from the first slice keeps the screen column
   * rather than the byte offset, which is the only reading of "down" that
   * stays on screen. */
  wrap_load("aa bbbbbbbb cc\n", 8);
  CUR.cy = 0; CUR.cx = 1;
  editor_move_cursor(ARROW_DOWN);
  check("down a wrapped line keeps the screen column", CUR.cx, 4);
  check("and stays on the same file row", CUR.cy, 0);
  check("one display row further down", cur_drow(), 1);
  editor_move_cursor(ARROW_UP);
  check("up comes back to where it started", CUR.cx, 1);

  /* Into a slice narrower than the column asked for, the cursor stops on that
   * slice's last column -- the column after it is the next line's first. */
  CUR.cy = 0; CUR.cx = 10;
  editor_move_cursor(ARROW_UP);
  check("up into a shorter line stops on its last column", CUR.cx, 2);
  check("which is that line and not the one below", cur_drow(), 0);

  /* Down off the last slice of a row reaches the next row, not the one after. */
  wrap_load("aa bbbbbbbb cc\nsecond row\n", 8);
  CUR.cy = 0; CUR.cx = 12;
  check("starting on the last line of the first row", cur_drow(), 2);
  editor_move_cursor(ARROW_DOWN);
  check("down reaches the next file row", CUR.cy, 1);
  check("at the column it was at on screen", CUR.cx, 1);

  /* Down from the last row reaches the phantom row and stops there. */
  wrap_load("one\ntwo\n", 8);
  CUR.cy = 1; CUR.cx = 2;
  editor_move_cursor(ARROW_DOWN);
  check("down past the last row reaches the phantom row", CUR.cy, 2);
  editor_move_cursor(ARROW_DOWN);
  check("and stops there", CUR.cy, 2);
  editor_move_cursor(ARROW_UP);
  check("up from it lands on the last row", CUR.cy, 1);

  /* The walk down the display rows keeps one step of itself, so an edit to a
   * row above where it stopped has to throw that away. */
  wrap_load("aa bbbbbbbb cc\nshort\ntail\n", 8);
  check("five display rows over three file rows", drow_total(), 5);
  check("the fifth of them is the third row", slice_row(4), 2);
  editor_row_insert_n(row_at(1), 0, "xxxxxxxxxxxx", 12);
  check("an edit above where the walk stopped moves it", slice_row(4), 1);
  check("to a continuation of the row that grew", slice_frag(4), 1);

  /* The count is cached on the row, so both things that can change it have to
   * clear it: the width it was measured at, and the text itself. */
  {
    char body[256];
    char fill[128];
    memset(body, 'x', 100);
    body[100] = '\n';
    body[101] = '\0';
    memset(fill, 'x', sizeof(fill));
    wrap_load(body, 80);
    check("a hundred columns at eighty is two lines", drow_total(), 2);
    V.screencols = 120;
    check("the same row at a wider window is one", drow_total(), 1);
    V.screencols = 80;
    check("and narrowing it again is two", drow_total(), 2);
    editor_row_delete_n(row_at(0), 0, 30);
    check("an edit that shortens the row re-measures it", drow_total(), 1);
    editor_row_insert_n(row_at(0), 0, fill, 90);
    check("a row ending exactly on an edge opens no line past it",
          drow_total(), 2);
    editor_row_insert_n(row_at(0), 0, fill, 1);
    check("and one column more than that is a third line", drow_total(), 3);
  }

  V.wrap = 0;
}

/* The drop-down's own arithmetic, which is where the menu's height used to go
 * wrong: it was clamped to the grid rather than to the text area, so a menu
 * taller than the screen lost its tail off the bottom and a menu that merely
 * reached the bottom laid its drop shadow over the status bar. Both only happen
 * on a terminal short enough that nobody runs one, which is exactly why they
 * are tested here rather than left to a snapshot. */
static void test_menu_dropdown(void) {
  printf("\nmenu drop-downs: how tall a box may be, and which items it shows\n");

  /* Room to spare: the box is the list plus its two borders. The Edit menu's
   * eleven items on an 80x24 terminal, where the last row a box may touch is
   * 21 -- the text area's last, with the status row and the message bar under
   * it. */
  check("a menu with room is the list plus two borders",
        menu_dropdown_height(11, 1, 21), 13);
  /* The row after the box belongs to the drop shadow, which restyles what is
   * under it: that is the one row of slack the height leaves, and the reason
   * a menu ending exactly on the last text row is still one too tall. */
  check("a box ending one row short of the limit fits",
        menu_dropdown_height(11, 1, 14), 13);
  check("one ending on the limit itself does not",
        menu_dropdown_height(11, 1, 13), 12);
  check("the shadow's row is the slack", menu_dropdown_height(5, 1, 5), 4);
  /* Shorter than any menu, where the floor wins: a border, one item, a border.
   * The shadow does land on the chrome there, and one item with an arrow under
   * it still says more than an empty box. */
  check("a screen with no room floors at three rows",
        menu_dropdown_height(11, 1, 2), 3);
  check("and so does one with none at all",
        menu_dropdown_height(11, 1, 0), 3);

  /* Nothing to scroll: the window starts at the top whatever it is handed. */
  check("a list that fits starts at the top",
        menu_dropdown_scroll(11, 11, 7, 0), 0);
  check("and is pulled back there from any offset",
        menu_dropdown_scroll(11, 11, 7, 5), 0);
  /* The window moves only as far as it must, which is what makes walking the
   * list with the arrow keys scroll a row at a time rather than a boxful. */
  check("an item below the window pushes it down one",
        menu_dropdown_scroll(11, 6, 6, 0), 1);
  check("an item inside it moves nothing",
        menu_dropdown_scroll(11, 6, 4, 3), 3);
  check("an item above it pulls it up to that item",
        menu_dropdown_scroll(11, 6, 2, 4), 2);
  check("the last item puts the window at the end",
        menu_dropdown_scroll(11, 6, 10, 0), 5);
  /* A terminal can get taller as well as shorter, and an offset left from the
   * shorter box would leave the bottom rows of the taller one empty. */
  check("a taller box gives the rows back",
        menu_dropdown_scroll(11, 10, 10, 5), 1);
  check("a list shorter than the box cannot scroll",
        menu_dropdown_scroll(3, 6, 1, 2), 0);

  /* Over every shape a terminal and a menu can make between them, rather than
   * the handful above: the selected item is always drawn, and the window never
   * hangs off the end of a list long enough to fill it. */
  {
    int n, vis, sel, off, unseen = 0, hanging = 0;
    for (n = 1; n <= 16; n++)
      for (vis = 1; vis <= 16; vis++)
        for (sel = 0; sel < n; sel++)
          for (off = -2; off <= 18; off++) {
            int got = menu_dropdown_scroll(n, vis, sel, off);
            if (sel < got || sel >= got + vis) unseen++;
            if (got < 0 || (got > 0 && got + vis > n)) hanging++;
          }
    check("the selection is inside the window at every size", unseen, 0);
    check("and the window never hangs off the end", hanging, 0);
  }
}

static void test_cursors_list(void) {
  printf("\ncursors: the list stays ordered and distinct\n");

  mc_load("alpha\nbeta\ngamma\n");

  check("a fresh view has one cursor", V.ncursors, 1);
  check("adding one says so", cursors_add(2, 1), 1);
  check("and the list grows", V.ncursors, 2);
  check("adding one where a cursor already is does not",
        cursors_add(2, 1), 0);
  check("and the list does not grow", V.ncursors, 2);

  /* Document order, with the primary following its own cursor through the
   * sort rather than staying on an index. */
  CUR.cy = 0;
  CUR.cx = 3;
  check("adding a third", cursors_add(1, 2), 1);
  cursors_sort();
  check("the list is in document order", V.cursors[0].cy, 0);
  check("second", V.cursors[1].cy, 1);
  check("third", V.cursors[2].cy, 2);
  check("and the primary followed its cursor", V.primary, 0);
  check("which is still where it was", CUR.cx, 3);

  /* Two cursors at one position would insert twice there, so they merge. */
  V.cursors[2].cy = 1;
  V.cursors[2].cx = 2;
  cursors_dedup();
  check("duplicates are dropped", V.ncursors, 2);
  check("and the primary survives", V.primary, 0);
  check("still where it was", CUR.cx, 3);

  /* A cursor carrying a selection wins over a bare duplicate: the selection is
   * what the next keystroke is meant to replace. */
  mc_load("alpha\n");
  check("a cursor with a selection is added", cursors_add_sel(0, 1, 4), 1);
  check("there are two", V.ncursors, 2);
  V.cursors[0].cy = 0;
  V.cursors[0].cx = 4;
  cursors_dedup();
  check("the bare duplicate loses", V.ncursors, 1);
  check("and the selection is what is left", V.cursors[0].sel_active, 1);
  check("with its anchor", V.cursors[0].sx, 1);

  /* The ceiling is a guard, not a limit anyone reaches by hand. */
  mc_load("alpha\n");
  {
    int i;
    for (i = 0; i < 600; i++) cursors_add(0, i);
    check("the list stops growing at its ceiling", V.ncursors, 512);
    check("and says no once it is full", cursors_add(0, 9999), 0);
  }
}

static void test_cursors_edit(void) {
  printf("\ncursors: one edit at every cursor\n");

  /* Typing. Three cursors on one row: the two later ones have to survive the
   * byte the first one inserted, which is the whole point of the rule. */
  mc_load("abcdef\n");
  CUR.cy = 0;
  CUR.cx = 1;
  cursors_add(0, 3);
  cursors_add(0, 5);
  cursors_edit(MC_INSERT, '-');
  check_str("typing lands at all three", mc_text(), "a-bc-de-f");
  check("and each cursor moved past what it typed", V.cursors[0].cx, 2);
  check("the second", V.cursors[1].cx, 5);
  check("the third", V.cursors[2].cx, 8);

  /* One undo step for the lot: the group the driver opens swallows every
   * record, and editor_undo pops a group at a time. */
  editor_undo();
  check_str("and all three undo together", mc_text(), "abcdef");

  /* Cursors on different rows: the first one's edit must not move the others,
   * which it does not, because nothing an insert does reaches another row. */
  mc_load("one\ntwo\nthree\n");
  CUR.cy = 0;
  CUR.cx = 0;
  cursors_add(1, 0);
  cursors_add(2, 0);
  cursors_edit(MC_INSERT, '>');
  check_str("typing reaches every row", mc_text(), ">one\n>two\n>three");

  /* Enter at N cursors. Every split pushes the rows below it down, so the
   * cursors still to come are a row further on each time -- the case a single
   * global position would get wrong after the first one. */
  mc_load("ab\ncd\n");
  CUR.cy = 0;
  CUR.cx = 1;
  cursors_add(1, 1);
  cursors_edit(MC_NEWLINE, 0);
  check_str("Enter splits at both", mc_text(), "a\nb\nc\nd");
  check("the first cursor is on its new row", V.cursors[0].cy, 1);
  check("and the second followed the rows that moved", V.cursors[1].cy, 3);

  editor_undo();
  check_str("and both splits undo at once", mc_text(), "ab\ncd");

  /* Backspace mid-row: the removed byte is behind the cursor, so everything
   * after it comes back one column. */
  mc_load("abcdef\n");
  CUR.cy = 0;
  CUR.cx = 2;
  cursors_add(0, 4);
  cursors_add(0, 6);
  cursors_edit(MC_DELETE_BACK, 0);
  check_str("Backspace takes one from each", mc_text(), "ace");
  check("and the cursors are where the text is", V.cursors[0].cx, 1);
  check("the second", V.cursors[1].cx, 2);
  check("the third", V.cursors[2].cx, 3);

  /* Backspace at column zero joins rows, which moves every row below up and
   * re-anchors the joined row's text onto the end of the one above. */
  mc_load("ab\ncd\nef\n");
  V.cursors[0].cy = 1;
  V.cursors[0].cx = 0;
  cursors_add(2, 1);
  cursors_edit(MC_DELETE_BACK, 0);
  check_str("a join and a plain delete together", mc_text(), "abcd\nf");
  check("the joining cursor sits where the rows met", V.cursors[0].cy, 0);
  check("at the right column", V.cursors[0].cx, 2);
  check("and the one below followed the row that moved", V.cursors[1].cy, 1);

  /* Delete forward is the one operation whose removed range ends somewhere
   * other than the cursor it started at. */
  mc_load("abcdef\n");
  CUR.cy = 0;
  CUR.cx = 0;
  cursors_add(0, 2);
  cursors_add(0, 4);
  cursors_edit(MC_DELETE_FWD, 0);
  check_str("Delete takes the character in front of each", mc_text(), "bdf");
  check("and the cursors do not move past what is left", V.cursors[0].cx, 0);
  check("the second", V.cursors[1].cx, 1);
  check("the third", V.cursors[2].cx, 2);

  /* Delete at the end of a row joins the next one on. */
  mc_load("ab\ncd\nef\n");
  V.cursors[0].cy = 0;
  V.cursors[0].cx = 2;
  cursors_add(2, 0);
  cursors_edit(MC_DELETE_FWD, 0);
  check_str("a forward join and a plain delete", mc_text(), "abcd\nf");
  check("the joining cursor stays put", V.cursors[0].cy, 0);
  check("and the other followed the row that moved up", V.cursors[1].cy, 1);

  /* An operation that does nothing must not move anything. Backspace at the
   * very top of the buffer is the no-op worth checking, because the cursor it
   * runs at is the earliest one and every other cursor is after it. */
  mc_load("abc\ndef\n");
  V.cursors[0].cy = 0;
  V.cursors[0].cx = 0;
  cursors_add(1, 1);
  cursors_edit(MC_DELETE_BACK, 0);
  check_str("a no-op at the first cursor leaves the rest alone",
            mc_text(), "abc\nef");
  check("and the cursor that did work is where it should be",
        V.cursors[1].cx, 0);

  printf("\ncursors: a selection at every cursor\n");

  /* The Ctrl-D case: each cursor has selected an occurrence, and typing
   * replaces all of them. The replaced runs are different lengths from what
   * replaces them, which is what the cursors after have to survive. */
  mc_load("red fish red fish\n");
  V.cursors[0].cy = 0;
  V.cursors[0].cx = 3;
  V.cursors[0].sel_active = 1;
  V.cursors[0].sy = 0;
  V.cursors[0].sx = 0;
  cursors_add_sel(0, 9, 12);
  cursors_edit(MC_INSERT, 'X');
  check_str("typing replaces every selection", mc_text(), "X fish X fish");
  check("and the selections are gone", V.cursors[0].sel_active, 0);
  check("both of them", V.cursors[1].sel_active, 0);
  check("the second cursor is after what it typed", V.cursors[1].cx, 8);

  editor_undo();
  check_str("and the whole replacement undoes at once",
            mc_text(), "red fish red fish");

  /* Backspace with selections deletes the selections and stops -- one cursor
   * does that already, and N of them must not also eat a character each. */
  mc_load("red fish red fish\n");
  V.cursors[0].cy = 0;
  V.cursors[0].cx = 3;
  V.cursors[0].sel_active = 1;
  V.cursors[0].sy = 0;
  V.cursors[0].sx = 0;
  cursors_add_sel(0, 9, 12);
  cursors_edit(MC_DELETE_BACK, 0);
  check_str("Backspace over selections deletes just those",
            mc_text(), " fish  fish");

  /* A selection the edit does not touch keeps covering the same characters:
   * the anchor travels with the cursor across an edit earlier in the file. */
  mc_load("abcdef\n");
  V.cursors[0].cy = 0;
  V.cursors[0].cx = 1;
  cursors_add_sel(0, 3, 5);
  cursors_edit(MC_INSERT, '-');
  /* "abcdef" becomes "a-bcdef" at the first cursor, which pushes the second
   * cursor's selection from [3,5) to [4,6) -- still the same two characters,
   * "de" -- and typing then replaces them. A selection left on byte offsets
   * would have replaced "cd" instead. */
  check_str("an earlier insert moves a later selection along",
            mc_text(), "a-bc-f");
}

int main(void) {
  test_loop_sources();
  test_wait_key_services_sources();
  test_grid_emission();
  test_theme_depth();
  test_theme_styles();
  test_theme_italic();
  test_theme_color_bytes();
  test_palette_matcher();
  test_input_field();
  test_regex_syntax();
  test_regex_utf8();
  test_regex_corners();
  test_regex_limits();
  test_regex_expand();
  syntax_init();
  test_fit_cols();
  test_help_entries();
  test_syntax_c();
  test_syntax_python();
  test_syntax_autoit();
  test_syntax_markdown();
  test_syntax_misc();
  test_indent();
  test_cursors_list();
  test_cursors_edit();
  test_cursors_more();
  test_indent_block();
  test_indent_convert();
  test_wrap();
  test_menu_dropdown();
  test_buflist();
  test_filter();
  test_windows();
  test_windows_too_small();
  test_windows_resize();
  test_grep();
  test_windows_buffer_closed();
  test_chords();
  test_config_parse();
  test_config_keys();
  test_config_write();
  test_setting_access();
  test_mkdir_p();

  printf("\n%s\n", failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
