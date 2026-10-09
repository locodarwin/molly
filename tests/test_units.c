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
  check_fit("a\xe6\x9c\xa8b", 3, 4, 3); /* and the 'b' does not */

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
  V.menubar = 1;

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

  buflist_init();
  cursors_init();

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
  buflist_init();
  check("closing a buffer that does not exist is harmless",
        buf_close(7), 0);
  check("as is showing one", (buf_show(7), buf_current()), 0);
  check("and asking for one", buf_at(-1) == NULL, 1);
  buflist_cleanup();

  printf("\nbuffers: finding and counting\n");

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
  buflist_init();
  cursors_init();
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
  buflist_init();
  cursors_init();
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
  syntax_init();
  test_fit_cols();
  test_help_entries();
  test_syntax_c();
  test_syntax_python();
  test_syntax_autoit();
  test_syntax_markdown();
  test_syntax_misc();
  test_buflist();
  test_chords();
  test_config_parse();
  test_config_keys();
  test_config_write();
  test_mkdir_p();

  printf("\n%s\n", failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
