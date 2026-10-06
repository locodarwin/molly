/* main.c -- Startup, teardown and the main loop. Key dispatch lives in cmd.c,
   and the wait the loop is built on in loop.c. */

/* How long the editor waits before looking around again. A resize interrupts the
   wait, so this is not how quickly one is noticed; it is the ceiling on how long
   molly can be unaware that anything has happened. */
#define LOOP_TICK_MS 100

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

  /* Test hook (documented in MANUAL.md): shrink the undo cap so history
   * eviction is reachable without editing for an hour. */
  {
    const char *cap = getenv("MOLLY_UNDO_STEPS");
    if (cap && *cap) {
      long v = strtol(cap, NULL, 10);
      if (v > 0) undo_set_max_steps((v > 1000000000L) ? 1000000000 : (int)v);
    }
  }

  screen_init_size();
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

  /* Test hook (documented in MANUAL.md): print the parsed keymap and exit,
   * before the terminal is touched. */
  if (getenv("MOLLY_DUMP_KEYMAP") != NULL) {
    keymap_init();
    keymap_dump(stdout);
    return 0;
  }

  enable_raw_mode();
  atexit(editor_cleanup);
  init_editor();
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

  editor_set_status_message(
      "HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find | Ctrl-N = next | Ctrl-? = help");

  /* May replace the HELP message and paint its own frame while it waits for
   * an answer. */
  editor_check_recovery();

  if (num_extra > 0)
    editor_set_status_message("Warning: ignored %d extra argument%s (e.g. %s)",
        num_extra, num_extra > 1 ? "s" : "", first_extra);

  /* Paint the first frame up front. The loop below reads a key before it
   * paints, so without this the screen stayed blank until the first
   * keystroke. */
  editor_refresh_screen();

  /* Wait for something to happen, deal with it, repaint. The wait is the event
   * loop's rather than a blocking read's, so a language server or an integrated
   * terminal becomes a registered source instead of a reason to restructure this.
   *
   * Only repaint once there is something to repaint: an idle tick leaves the
   * screen alone, which matters over a slow link, where a frame emitted every
   * hundred milliseconds for no reason is a frame too many. */
  while (1) {
    int ready = loop_wait(LOOP_TICK_MS);

    if (ready & LOOP_STDIN) {
      editor_process_keypress();
    } else if (term_take_winch()) {
      /* A resize is not a keystroke; the repaint below picks up the new size. */
    } else {
      continue;  /* nothing happened, and nothing to draw */
    }

    editor_refresh_screen();
  }

  return 0;
}
