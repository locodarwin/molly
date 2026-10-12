/* main.c -- Startup, teardown and the main loop. Key dispatch lives in cmd.c,
   and the wait the loop is built on in loop.c. */


#include "molly.h"

void editor_cleanup(void) {
  /* The language servers first: they are other processes, and the one thing
   * worse than an editor that takes a moment to exit is one that leaves a
   * clangd running behind it. Before the buffers, whose frees would otherwise
   * be telling servers about each one on the way out. */
  lsp_cleanup();
  /* And every terminal's shell, for the same reason. */
  tty_cleanup();
  recent_cleanup();
  free(E.clipboard);
  E.clipboard = NULL;
  E.clipsize = 0;
  screen_cleanup();
  search_cleanup();
  grep_cleanup();
  keymap_cleanup();
  config_cleanup();
  syntax_cleanup();
  /* Every open buffer, with its rows, its saved snapshot and its undo history --
   * which is what this function used to do by hand for the one buffer there was.
   * After the frees above rather than before, because several of them still read
   * through B, and B is the current view's buffer. */
  buflist_cleanup();
  /* The windows last, because their views are what B resolves through and the
   * frees above still read it. This takes the cursor lists with them -- which
   * is the one thing this function used to do by hand, for the one view there
   * was. */
  window_cleanup();
}

void init_editor(void) {
  /* The window tree first, because V is whichever view it has focused and
   * buflist_init() below points that view at the first buffer. One window,
   * around the view state.c defines statically.
   *
   * Then the buffer list, because it is what gives the view something to point
   * at -- and B *is* the view's buffer, so until that runs there is no B to
   * initialise. buf_new() sets a buffer's starting state, which is why the
   * per-buffer assignments that used to be here are gone. */
  window_init();
  buflist_init();
  theme_init();
  screen_init_layers();
  keymap_init();
  syntax_init();
  /* No cursors_init() here any more: window_init() gives the first view its
   * cursor list, because "a view has at least one cursor" is a property of a
   * view and not a step in starting the editor. */
  V.drowoff = 0;
  V.coloff = 0;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;
  E.clipboard = NULL;
  E.clipsize = 0;
  build_recovery_path();

  screen_init_size();
}

/* The greeting, built from the keymap rather than written out.
 *
 * It used to be a string literal naming five chords, which was fine until a
 * chord could move: a rebinding in mollyrc, or molly's own defaults changing,
 * would have left the first thing molly says about itself quietly wrong. So the
 * keys come from the same place the menus and the help screen get theirs, and a
 * command with nothing bound to it is skipped rather than shown as blank. */
static void startup_message(void) {
  static const struct { const char *cmd; const char *what; } hints[] = {
    {"file.save", "save"},
    {"file.quit", "quit"},
    {"search.find", "find"},
    {"search.next", "next"},
    {"view.help", "help"},
  };
  char msg[256];
  size_t o = 0;
  int i, first = 1;

  o += (size_t)snprintf(msg, sizeof(msg), "HELP:");
  for (i = 0; i < (int)(sizeof(hints) / sizeof(hints[0])); i++) {
    char chord[32];
    int n;
    if (!cmd_shortcut(hints[i].cmd, chord, sizeof(chord))) continue;
    n = snprintf(msg + o, sizeof(msg) - o, "%s %s = %s", first ? "" : " |",
                 chord, hints[i].what);
    if (n < 0 || (size_t)n >= sizeof(msg) - o) break;
    o += (size_t)n;
    first = 0;
  }
  editor_set_status_message("%s", msg);
}

/* Is this argument a "+N" start-line request? Digits only after the plus:
 * an argument like "+foo" is treated as a (peculiar) file name instead, so
 * files whose names begin with a plus stay reachable. */
static int arg_is_line(const char *s, long *out) {
  if (s[0] != '+') return 0;
  const char *p;
  for (p = s + 1; *p != '\0'; p++)
    if (*p < '0' || *p > '9') return 0;
  if (p == s + 1) return 0;
  long v = strtol(s + 1, NULL, 10);
  *out = (v > 1000000000L) ? 1000000000L : v;
  return 1;
}

/* molly [--version | -v] [file] [+line] ...
 * The file is the first argument that is not a flag or a line request;
 * anything past that is reported on the status bar and ignored. */
int main(int argc, char *argv[]) {
  long startline = 0;
  int have_line = 0;
  /* Every non-flag argument is a file now, where all but the first used to be
   * warned about and thrown away. `molly *.c` is the reason the feature exists,
   * so argv is the first place it has to work. Held as indices into argv rather
   * than copied: argv outlives main. */
  int files[256];
  int nfiles = 0;
  int too_many = 0;
  int i;
  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--version") == 0 || strcmp(a, "-v") == 0 ||
        strcmp(a, "-V") == 0) {
      printf("molly %s\n", MOLLY_VERSION);
      return 0;
    }
    long n;
    if (arg_is_line(a, &n)) {
      startline = n;
      have_line = 1;
      continue;
    }
    if (nfiles < (int)(sizeof(files) / sizeof(files[0]))) files[nfiles++] = i;
    else too_many++;
  }

  /* The configuration file, read before anything is applied and before the
   * terminal is touched -- which is what lets both dump hooks below report
   * exactly what the editor is about to do without needing a terminal to do it
   * on. Nothing here can fail fatally: a file molly cannot read or cannot parse
   * leaves the defaults in place and a warning to show once the screen is up. */
  config_load();

  /* Test hook (documented in MANUAL.md): print the parsed configuration and
   * exit. */
  if (getenv("MOLLY_DUMP_CONFIG") != NULL) {
    config_dump(stdout);
    return 0;
  }

  /* Test hook (documented in MANUAL.md): print the parsed keymap and exit,
   * before the terminal is touched. The file's [keys] are applied first, so this
   * reports what is actually bound rather than only what molly ships with. */
  if (getenv("MOLLY_DUMP_KEYMAP") != NULL) {
    keymap_init();
    config_apply_keys();
    keymap_dump(stdout);
    return 0;
  }

  enable_raw_mode();
  atexit(editor_cleanup);
  init_editor();
  /* Now that there is an editor to apply them to. This is also where the menu
   * bar and the tab width get their defaults, since the settings table is the
   * one place either is written down. */
  config_apply();

  /* Test hook (documented in MANUAL.md): shrink the undo cap so history eviction
   * is reachable without editing for an hour.
   *
   * After config_apply() on purpose, which settles the precedence: the built-in
   * default, then the configuration file, then the environment. It used to sit
   * in init_editor(), and the moment `undo steps` became a setting that made the
   * hook useless -- config_apply() ran afterwards and put the cap back. */
  {
    const char *cap = getenv("MOLLY_UNDO_STEPS");
    if (cap != NULL && *cap != '\0') {
      long v = strtol(cap, NULL, 10);
      if (v > 0) undo_set_max_steps((v > 1000000000L) ? 1000000000 : (int)v);
    }
  }

  /* One buffer per file, in the order given, with the first left showing. The
   * empty buffer molly starts with is reused for the first file rather than
   * left behind, which is what editor_open_in_new_buffer does. */
  for (i = 0; i < nfiles; i++)
    editor_open_in_new_buffer(xstrdup(argv[files[i]]));
  if (nfiles > 0) buf_show(0);

  if (have_line) {
    /* Same clamping as Ctrl-G: land on a real row, never on the phantom
     * row past the last one; the refresh scrolls it into view.
     *
     * Applied to the buffer on show, which with several files is the first one.
     * A +N per file would want a +N between each pair of names on the command
     * line, which is a command-line design question rather than this one. */
    int max_cy = (row_count() > 0) ? row_count() - 1 : 0;
    int target = (int)startline - 1;
    if (target < 0) target = 0;
    if (target > max_cy) target = max_cy;
    CUR.cy = target;
    CUR.rx = 0;
    CUR.cx = 0;
  }

  startup_message();

  /* May replace the HELP message and paint its own frame while it waits for an
   * answer -- once per buffer that has recovery data, since a crash with three
   * files open can have left three of them. Each prompt names its own file
   * because it is shown while that buffer is the one on screen. */
  {
    int cur = buf_current();
    int b;
    for (b = 0; b < buf_count(); b++) {
      buf_show(b);
      editor_check_recovery();
    }
    buf_show(cur);
  }

  if (too_many)
    editor_set_status_message("Warning: ignored %d file%s past the first %d",
        too_many, too_many > 1 ? "s" : "", nfiles);
  else if (buf_count() > 1)
    editor_set_status_message("%d files open - Ctrl-PgUp/PgDn to switch",
        buf_count());

  /* A problem in mollyrc is worth more of the message bar than the greeting is.
   * The first one is shown in full, with a count when there were more, because a
   * file with six mistakes in it usually has one mistake repeated six times and
   * the first line is enough to find it. */
  if (config_error_count() > 0) {
    const char *first = config_error_at(0);
    if (first == NULL) first = "configuration could not be read";
    if (config_error_count() == 1) editor_set_status_message("%s", first);
    else editor_set_status_message("%s (%d problems)", first,
                                   config_error_count());
  }

  /* Paint the first frame up front. The loop below reads a key before it
   * paints, so without this the screen stayed blank until the first
   * keystroke. */
  editor_refresh_screen();

  /* Wait for a key, act on it, repaint. The wait services the event loop, so a
   * language server or an integrated terminal is a registered source rather than a
   * reason to restructure this, and nothing is drawn until something has happened.
   *
   * A resize arrives as WINCH_KEY and is dispatched like any other key -- the
   * app.resize command does nothing except decline to reset the quit countdown,
   * and the repaint below picks up the new size. */
  while (1) {
    editor_process_key(ui_wait_key());
    /* A language server's answer, if one has come and is still about where
     * the cursor is. Here and not where it arrives: see lspui_deliver. */
    lspui_deliver();
    /* And the problems list, if a server has changed its mind since. */
    lspui_problems_refresh();
    /* Terminals whose shells have exited close here, for the same reason. */
    tty_tidy();
    editor_refresh_screen();
  }

  return 0;
}
