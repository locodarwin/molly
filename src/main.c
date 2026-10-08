/* main.c -- Startup, teardown and the main loop. Key dispatch lives in cmd.c,
   and the wait the loop is built on in loop.c. */


#include "molly.h"

void editor_cleanup(void) {
  int i;
  for (i = 0; i < row_count(); i++) editor_free_row(row_at(i));
  free(B.row);
  B.row = NULL;
  B.numrows = 0;
  B.rowcap = 0;
  free(E.clipboard);
  E.clipboard = NULL;
  E.clipsize = 0;
  saved_snapshot_free();
  screen_cleanup();
  search_cleanup();
  editor_free_undo_redo();
  free(B.filename);
  B.filename = NULL;
  keymap_cleanup();
  config_cleanup();
  free(V.cursors);
  V.cursors = NULL;
  V.ncursors = 0;
  V.cursorcap = 0;
  V.primary = 0;
}

void init_editor(void) {
  /* One view onto one buffer. Everything that reads rows goes through the view
   * from here on, so a second buffer is a matter of pointing V.buf elsewhere. */
  V.buf = &B;
  theme_init();
  screen_init_layers();
  keymap_init();
  cursors_init();
  V.drowoff = 0;
  V.coloff = 0;
  B.numrows = 0;
  B.rowcap = 0;
  B.row = NULL;
  B.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;
  B.dirty = 0;
  E.clipboard = NULL;
  E.clipsize = 0;
  B.eol_crlf = 0;
  B.final_newline = 1;
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
  char *filename = NULL;
  long startline = 0;
  int have_line = 0;
  char *first_extra = NULL;
  int num_extra = 0;
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
    if (filename == NULL) {
      filename = (char *)a;
      continue;
    }
    if (first_extra == NULL) first_extra = (char *)a;
    num_extra++;
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

  if (filename != NULL) editor_open(filename);

  if (have_line) {
    /* Same clamping as Ctrl-G: land on a real row, never on the phantom
     * row past the last one; the refresh scrolls it into view. */
    int max_cy = (row_count() > 0) ? row_count() - 1 : 0;
    int target = (int)startline - 1;
    if (target < 0) target = 0;
    if (target > max_cy) target = max_cy;
    CUR.cy = target;
    CUR.rx = 0;
    CUR.cx = 0;
  }

  startup_message();

  /* May replace the HELP message and paint its own frame while it waits for
   * an answer. */
  editor_check_recovery();

  if (num_extra > 0)
    editor_set_status_message("Warning: ignored %d extra argument%s (e.g. %s)",
        num_extra, num_extra > 1 ? "s" : "", first_extra);

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
    editor_refresh_screen();
  }

  return 0;
}
