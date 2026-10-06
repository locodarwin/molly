/* main.c -- Startup, the key dispatch table in all but name, and the main loop. */

#include "molly.h"

void editor_process_keypress(void) {
  static int quit_times = KILO_QUIT_TIMES;
  int c = editor_read_key();
  int shift = input_last_key_had_shift();
  int pre_cx = CUR.cx, pre_cy = CUR.cy;
  int sel_move = (c == ARROW_UP || c == ARROW_DOWN || c == ARROW_LEFT ||
      c == ARROW_RIGHT || c == HOME_KEY || c == END_KEY ||
      c == PAGE_UP || c == PAGE_DOWN);

  switch (c) {
    case '\r':
      if (CUR.sel_active) selection_delete();
      editor_insert_newline();
      break;

    case CTRL_KEY('q'):
      if (B.dirty && quit_times > 0) {
        editor_set_status_message(
            "WARNING!!! File has unsaved changes. Press Ctrl-Q %d more times to quit.",
            quit_times);
        quit_times--;
        return;
      }
      write_all(STDOUT_FILENO, "\x1b[2J", 4);
      write_all(STDOUT_FILENO, "\x1b[H", 3);
      if (B.recovery_path[0] != '\0') unlink(B.recovery_path);
      exit(0);
      break;

    case CTRL_KEY('s'):
      editor_save();
      break;

    case CTRL_KEY('o'): {
      /* One buffer, one file: the open dialog replaces the buffer, so it
       * is refused while there is unsaved work rather than silently
       * discarding it. */
      if (B.dirty) {
        editor_set_status_message(
            "Open aborted: file has unsaved changes (Ctrl-S first)");
        break;
      }
      char *path = editor_open_dialog();
      screen_force_repaint();
      if (path != NULL) editor_switch_file(path);
      break;
    }

    case CTRL_KEY('z'):
      editor_undo();
      break;

    case CTRL_KEY('y'):
      editor_redo();
      break;

    case CTRL_KEY('c'): {
      /* Copy: selection to clipboard, selection kept (see the clear guard
       * below). Ctrl-C only reaches here because raw mode clears ISIG. */
      if (CUR.sel_active) {
        int n = 0;
        char *s = selection_dup(&n);
        clipboard_set(s, n);
        free(s);
        clipboard_sync_system();
        editor_set_status_message("Copied %d bytes", n);
      }
      break;
    }

    case CTRL_KEY('x'):
      /* Cut: copy then delete the selection (one undo step). */
      if (CUR.sel_active) {
        int n = 0;
        char *s = selection_dup(&n);
        clipboard_set(s, n);
        free(s);
        clipboard_sync_system();
        selection_delete();
        editor_set_status_message("Cut %d bytes", n);
      }
      break;

    case CTRL_KEY('v'):
      editor_paste();
      break;

    case CTRL_KEY('f'):
      editor_find();
      break;

    case CTRL_KEY('n'):
      editor_find_next();
      break;

    case CTRL_KEY('p'):
      editor_find_prev();
      break;

    case CTRL_KEY('t'):
      editor_toggle_case_sensitive();
      break;

    case CTRL_KEY('r'):
      editor_replace();
      break;

    case CTRL_KEY('g'):
      editor_goto_line();
      break;

    case CTRL_KEY('?'):
      editor_help();
      break;

    case HOME_KEY:
      CUR.cx = 0;
      break;

    case END_KEY:
      if (CUR.cy < row_count()) CUR.cx = (*row_at(CUR.cy)).size;
      break;

    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
      if (CUR.sel_active) {
        selection_delete();
      } else if (c == DEL_KEY) {
        editor_del_char();
      } else {
        editor_delete_char();
      }
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      {
        int times = V.screenrows;
        while (times--)
          editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
      break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editor_move_cursor(c);
      break;

    case CTRL_KEY('l'):
      screen_force_repaint();
      break;

    case CTRL_KEY('u'):
      V.linenum = !V.linenum;
      /* The gutter widens or shrinks the text area, shifting every column, so
       * repaint the whole screen rather than trust per-row diffing. */
      screen_force_repaint();
      editor_set_status_message("Line numbers: %s", V.linenum ? "on" : "off");
      break;

    case WINCH_KEY:
      /* Not a keystroke -- just loop back so the next editor_refresh_screen
       * picks up the new terminal size. Return rather than break, so this
       * doesn't reset the Ctrl-Q quit-confirmation countdown. */
      return;

    case '\x1b':
      break;

    default:
      /* Tab is a control character, so testing iscntrl() alone threw it
       * away and a tab could not be typed at all. Key codes of 1000 and up
       * are outside iscntrl()'s domain and must not reach it. */
      if (c == '\t' || (c < 256 && !iscntrl(c))) {
        if (CUR.sel_active) selection_delete();
        editor_insert_char(c);
      }
      break;
  }

  /* A shifted movement extends the selection (seeding the anchor on the first
   * one from a resting cursor). Copying keeps the selection; every other
   * plain move, edit, or command drops it. Edits that need the selected text
   * first consume it via selection_delete() above. */
  if (sel_move && shift) {
    if (!CUR.sel_active) {
      CUR.sx = pre_cx;
      CUR.sy = pre_cy;
      CUR.sel_active = 1;
    }
  } else if (c != CTRL_KEY('c')) {
    selection_clear();
  }

  quit_times = KILO_QUIT_TIMES;
}

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

  while (1) {
    editor_process_keypress();
    editor_refresh_screen();
  }

  return 0;
}
