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

int main(void) {
  test_loop_sources();
  test_wait_key_services_sources();
  test_grid_emission();
  test_theme_depth();
  test_theme_styles();
  test_theme_color_bytes();
  test_palette_matcher();
  test_input_field();
  test_regex_syntax();
  test_regex_utf8();
  test_regex_corners();
  test_regex_limits();
  test_regex_expand();

  printf("\n%s\n", failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
