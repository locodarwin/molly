/* cmd.c -- named commands, the keymap, and the dispatcher.
 *
 * Olly handled keys in one switch in editor_process_keypress. That works while
 * keys are the only way to reach an action, and molly's are not going to be: a
 * menu entry needs a label, an underlined mnemonic, a shortcut printed down the
 * right-hand side and the ability to grey itself out; a command palette needs a
 * name to search; a macro needs something to record; a configurable keymap needs
 * something to bind *to*. A switch case offers none of these, so every action
 * becomes a table entry instead, and the dispatcher never names an action
 * directly.
 *
 * The keymap is written as chord text -- "ctrl-s" -- and parsed at startup. That
 * is the same path a config file will take, so the parser is exercised every run
 * rather than waiting for the feature that needs it.
 *
 * Shift is half in and half out, and deliberately so. It still arrives out of
 * band from the input layer (see input_last_key_had_shift), because a shifted
 * *movement* extends a selection rather than running a different command, and
 * that is true of every movement at once rather than being a binding. But a
 * chord may now name it -- "shift-f3" -- and the dispatcher composes the two by
 * looking up `key | KEY_SHIFT` first and the bare key second. So Shift-Right
 * still extends a selection (nothing binds "shift-right", so it falls through to
 * move.right with the flag set) while Shift-F3 is a binding of its own.
 *
 * Shift is accepted only on the keys where a terminal actually reports it: the
 * function keys and the navigation keys. "shift-a" and "ctrl-shift-l" are
 * refused, because a terminal sends plain 'A' for the first and the same single
 * byte for the second as it does for Ctrl-L -- a binding that could never fire is
 * worse than an error message.
 *
 * What is still not here: multi-key sequences ("ctrl-k ctrl-f"). Nothing needs
 * one yet, and the chord type would have to become a list for it. */

#include "molly.h"

/* ---- the commands -------------------------------------------------------- */

/* Ctrl-Q's countdown. It belongs to the quit command, not to the dispatcher,
 * which only decides whether to reset it. */

static int cmd_newline(void) {
  /* In the find-in-files results Enter opens the match under the cursor, which
   * is what a list of matches is for -- the way it is in every other editor's
   * results. A row that names no file falls back to an ordinary Enter. */
  if (B.grep_results && V.ncursors <= 1 && grep_jump(0)) return 0;
  cursors_edit(MC_NEWLINE, 0);
  return 0;
}

/* The multi-cursor gestures. Each returns 0 because none of them changes the
 * text -- what they change is how many places the next keystroke lands in. */
/* CMD_KEEP_SELECTION, because the selection *is* the state this command
 * builds: the dispatcher drops the selection after every other command, and
 * dropping it here would throw away the text the next press searches for. */
static int cmd_cursor_add_next(void) {
  cursors_add_next();
  return CMD_KEEP_SELECTION;
}
static int cmd_cursor_add_below(void) {
  cursors_add_vertical(1);
  return 0;
}
static int cmd_cursor_add_above(void) {
  cursors_add_vertical(0);
  return 0;
}
/* Esc runs this. It used to reach no command at all and fall through to the
 * dispatcher's selection_clear(), which still happens -- so one key drops the
 * selection and the extra cursors together, which is the one thing a user
 * reaches for when a gesture has gone wrong. */
static int cmd_cursor_skip(void) {
  cursors_skip_next();
  return CMD_KEEP_SELECTION;
}
static int cmd_cursor_drop(void) {
  cursors_drop_last();
  return CMD_KEEP_SELECTION;
}
static int cmd_cursor_add_all(void) {
  cursors_add_all();
  return CMD_KEEP_SELECTION;
}
static int cmd_cursor_one(void) {
  if (V.ncursors > 1) {
    cursors_collapse();
    editor_set_status_message("One cursor");
  }
  return 0;
}
static int cursors_several(void) { return V.ncursors > 1; }

static int cmd_quit(void) {
  /* Every buffer, not the current one. The roadmap's warning about this feature
   * was that every "are you sure?" multiplies, and this is the first one: quit
   * with three files open and one of them edited and the edited one may not be
   * the one on screen. */
  if (buf_dirty_count() > 0) {
    int ans = editor_quit_dialog();
    if (ans == QUIT_CANCEL) return CMD_KEEP_SELECTION;
    if (ans == QUIT_SAVE) {
      /* Save All goes through the ordinary save, a buffer at a time, so a
       * nameless one gets its prompt and a file changed on disk gets its
       * question. Any buffer still dirty afterwards was aborted, and quitting
       * then would be the data loss the dialog exists to prevent: stop on it. */
      int i;
      for (i = 0; i < buf_count(); i++) {
        if (!buf_at(i)->dirty) continue;
        buf_show(i);
        editor_save();
        if (buf_at(i)->dirty) return CMD_KEEP_SELECTION;
      }
    }
  }
  write_all(STDOUT_FILENO, "\x1b[2J", 4);
  write_all(STDOUT_FILENO, "\x1b[H", 3);
  /* Every buffer's recovery file, for the same reason: leaving one behind means
   * the next start offers to recover work the user has just chosen to discard. */
  {
    int i;
    for (i = 0; i < buf_count(); i++)
      if (buf_at(i)->recovery_path[0] != '\0') unlink(buf_at(i)->recovery_path);
  }
  exit(0);
}

static int cmd_save(void) {
  editor_save();
  return 0;
}

static int cmd_open(void) {
  /* No repaint needed on the way out: the dialog is a layer, so popping it is
   * enough and the grid works out what to redraw.
   *
   * This used to refuse outright while the buffer was dirty, because opening
   * replaced the one buffer there was and there was nowhere for the unsaved work
   * to go. Opening into a buffer of its own is the answer to that, so the
   * refusal is gone. */
  char *path = editor_open_dialog();
  if (path != NULL) editor_open_in_new_buffer(path);
  return 0;
}

/* An empty buffer to type into. */
static int cmd_new(void) {
  buf_show(buf_index_of(buf_new()));
  build_recovery_path();
  saved_snapshot_take();
  editor_set_status_message("New buffer (%d of %d)", buf_current() + 1,
                            buf_count());
  return 0;
}

/* Close the current buffer. Refuses once while it has unsaved changes, on its
 * own confirmation rather than the quit countdown: pressing it twice by accident
 * should not be able to lose a file, and sharing Ctrl-Q's counter would mean a
 * refused close armed the quit.
 *
 * What is remembered is *which* buffer was warned about, by id, not a count. A
 * counter would let a refusal on one dirty buffer be spent on another: warn on
 * A, switch to B, press once, and B would close unwarned. Ids are never reused,
 * so this cannot be confused by a buffer that has since been closed. Any
 * unrelated keystroke clears it as well, in the dispatcher, exactly as one
 * cancels Ctrl-Q's countdown. */
static int close_warned_id = -1;

static int cmd_close(void) {
  if (B.dirty && close_warned_id != B.id) {
    editor_set_status_message(
        "%s has unsaved changes. Press again to close it anyway.",
        buf_name(V.buf));
    close_warned_id = B.id;
    return CMD_KEEP_SELECTION | CMD_KEEP_QUIT_COUNT;
  }
  close_warned_id = -1;
  /* The recovery file goes with the buffer: keeping it would offer to recover
   * work on the next start that the user has just discarded. */
  if (B.recovery_path[0] != '\0') unlink(B.recovery_path);
  {
    int n = buf_count();
    buf_close(buf_current());
    if (n == 1) editor_set_status_message("Closed - empty buffer");
    else editor_set_status_message("Closed - now %s (%d of %d)",
        buf_name(V.buf), buf_current() + 1, buf_count());
  }
  return 0;
}

static int cmd_buffer_list(void) {
  int i = editor_buffer_dialog();
  if (i >= 0 && i != buf_current()) buf_show(i);
  return 0;
}

static int cmd_buffer_next(void) {
  if (buf_count() < 2) return 0;
  buf_show((buf_current() + 1) % buf_count());
  return 0;
}

static int cmd_buffer_prev(void) {
  if (buf_count() < 2) return 0;
  buf_show((buf_current() + buf_count() - 1) % buf_count());
  return 0;
}

static int cmd_undo(void) { editor_undo(); return 0; }
static int cmd_redo(void) { editor_redo(); return 0; }

/* A block's rows, a line each, onto the clipboard -- marked as a block, so
 * that pasting it can put it back down as one. */
static int copy_block(void) {
  int n = 0;
  char *s = block_text(&n);
  if (s == NULL) return -1;
  clipboard_set(s, n);
  E.clip_block = 1;
  free(s);
  clipboard_sync_system();
  return n;
}

static int cmd_copy(void) {
  int n;
  /* Copy keeps the selection; everything else drops it. */
  if ((n = copy_block()) >= 0) {
    editor_set_status_message("Copied a block of %d rows", V.ncursors);
  } else if (CUR.sel_active) {
    int n = 0;
    char *s = selection_dup(&n);
    clipboard_set(s, n);
    free(s);
    clipboard_sync_system();
    editor_set_status_message("Copied %d bytes", n);
  }
  return CMD_KEEP_SELECTION;
}

static int cmd_cut(void) {
  int rows = V.ncursors;
  /* Copy then delete the selection, as one undo step. A block's slices go
   * through the multi-cursor delete, which removes each row's and stops. */
  if (copy_block() >= 0) {
    cursors_edit(MC_DELETE_BACK, 0);
    editor_set_status_message("Cut a block of %d rows", rows);
  } else if (CUR.sel_active) {
    int n = 0;
    char *s = selection_dup(&n);
    clipboard_set(s, n);
    free(s);
    clipboard_sync_system();
    selection_delete();
    editor_set_status_message("Cut %d bytes", n);
  }
  return 0;
}

/* The clipboard at every cursor, whole, rather than a line of it per cursor the
 * way some GUI editors do when the counts happen to match. One rule that holds
 * however many lines were copied beats two that depend on arithmetic the user
 * has to do in their head. */
/* The one exception is a block: see block_paste(). */
static int cmd_paste(void) {
  if (!block_paste()) cursors_edit(MC_PASTE, 0);
  return 0;
}

/* The block grows from its moving corner. KEEP_SELECTION because the block is
 * its cursors' selections, and the dispatcher would otherwise drop them the
 * moment they were made. */
static int cmd_block_up(void) { block_extend(-1, 0); return CMD_KEEP_SELECTION; }
static int cmd_block_down(void) { block_extend(1, 0); return CMD_KEEP_SELECTION; }
static int cmd_block_left(void) { block_extend(0, -1); return CMD_KEEP_SELECTION; }
static int cmd_block_right(void) { block_extend(0, 1); return CMD_KEEP_SELECTION; }

static int cmd_find(void) { editor_find(); return 0; }
static int cmd_find_next(void) { editor_find_next(); return 0; }
static int cmd_find_prev(void) { editor_find_prev(); return 0; }
static int cmd_case_toggle(void) { editor_toggle_case_sensitive(); return 0; }
static int cmd_regex_toggle(void) { editor_toggle_regex(); return 0; }
static int cmd_replace(void) { editor_replace(); return 0; }
static int cmd_grep(void) { editor_grep(); return 0; }
static int cmd_grep_next(void) { grep_jump(1); return 0; }
static int cmd_grep_prev(void) { grep_jump(-1); return 0; }
static int have_grep_results(void) { return grep_result_count() > 0; }
static int cmd_goto_line(void) { editor_goto_line(); return 0; }
static int cmd_problem_next(void) { lspui_problem_jump(1); return 0; }
static int cmd_problem_prev(void) { lspui_problem_jump(-1); return 0; }
static int have_problems(void) { return B.ndiag > 0; }
static int cmd_lsp_status(void) { lspui_status(); return 0; }
static int cmd_lsp_restart(void) { lspui_restart(); return 0; }
static int cmd_definition(void) { return lspui_definition(); }
static int cmd_back(void) { lspui_back(); return 0; }
static int cmd_hover(void) { return lspui_hover(); }
static int can_define(void) { return lspui_can(LSP_REQ_DEFINITION); }
static int can_hover(void) { return lspui_can(LSP_REQ_HOVER); }
static int cmd_complete(void) { return lspui_complete(); }
static int can_complete(void) { return lspui_can(LSP_REQ_COMPLETION); }
static int cmd_help(void) { editor_help(); return 0; }

/* Each cursor to its own row's edge, which is the point of a column of them:
 * End with cursors down five ragged lines should reach five line ends. */
static int cmd_line_start(void) {
  cursors_line_edge(0);
  return 0;
}

static int cmd_line_end(void) {
  cursors_line_edge(1);
  return 0;
}

/* Select the whole buffer. Every selection in molly is an anchor plus the live
 * cursor, so this is two assignments and not a mode. */
static int cmd_select_all(void) {
  if (row_count() == 0) return 0;
  CUR.sy = 0;
  CUR.sx = 0;
  CUR.cy = row_count() - 1;
  CUR.cx = row_at(CUR.cy)->size;
  CUR.sel_active = 1;
  /* Without this the dispatcher clears the selection the moment this returns,
   * which is what it does for every command that is not a movement. */
  return CMD_KEEP_SELECTION;
}

/* Select the line the cursor is on, end to end. It does not take the newline
 * with it, so Ctrl-L then Ctrl-X leaves an empty line rather than closing the
 * gap -- which is what makes it useful for replacing a line's contents, and is
 * the behaviour to revisit if "select line, extend by line" arrives. */
static int cmd_select_line(void) {
  if (CUR.cy >= row_count()) return 0;
  CUR.sy = CUR.cy;
  CUR.sx = 0;
  CUR.cx = row_at(CUR.cy)->size;
  CUR.sel_active = 1;
  return CMD_KEEP_SELECTION;
}

/* Swap the character before the cursor with the one at it, and leave the cursor
 * after both -- the transpose that Ctrl-T has meant for decades. At the end of a
 * line there is nothing at the cursor, so it swaps the two characters before it
 * instead, which is what makes it useful for the typo you have just finished
 * typing.
 *
 * Built from the ordinary edit operations rather than from row surgery, so it
 * gets their UTF-8 handling and their undo records for free; the group is what
 * makes one Ctrl-Z undo the whole swap. A multibyte character is re-inserted a
 * byte at a time, which the insert path coalesces back into one record. */
static int cmd_transpose(void) {
  erow *row;
  int a, b, e, alen, slen, i;
  char first[8];

  if (CUR.cy >= row_count()) return 0;
  row = row_at(CUR.cy);

  if (CUR.cx >= row->size) {
    if (row->size == 0) return 0;
    b = utf8_prev(row->chars, row->size);
    if (b == 0) return 0;            /* one character on the line: nothing to swap */
    a = utf8_prev(row->chars, b);
  } else {
    if (CUR.cx == 0) return 0;       /* nothing before the cursor */
    a = utf8_prev(row->chars, CUR.cx);
    b = CUR.cx;
  }
  alen = b - a;
  slen = utf8_char_bytes(row->chars, row->size, b);
  e = b + slen;
  if (alen <= 0 || alen > (int)sizeof(first)) return 0;
  memcpy(first, row->chars + a, (size_t)alen);

  undo_group_begin();
  /* Locked so the delete cannot coalesce into whatever the last edit was and
   * drag an unrelated keystroke into this group. */
  B.undo.locked = 1;
  CUR.cx = b;
  editor_delete_char();              /* removes [a, b), leaving the cursor at a */
  /* The second character now sits at a, so the first goes back in after it. */
  CUR.cx = a + slen;
  for (i = 0; i < alen; i++) editor_insert_char((unsigned char)first[i]);
  undo_group_end();
  (void)e;
  return 0;
}

/* Both of these took the selection themselves before there was more than one
 * cursor. cursors_edit() does it per cursor now, which is the same thing when
 * there is one of them. */
/* Tab. A selection that covers more than one row is a block and gets indented;
 * anything else is the keystroke it has always been, which replaces a selection
 * on one row and otherwise types one indent step.
 *
 * Only with a single cursor: several cursors each typing a step is what Tab
 * does there, and it is not obvious that N blocks is what anyone means by it.
 * CMD_KEEP_SELECTION so the block stays selected and a second press indents it
 * again -- without it the second press would type over the text. */
static int cmd_indent(void) {
  if (V.ncursors == 1 && CUR.sel_active) {
    int ax, ay, bx, by;
    if (selection_span(&ax, &ay, &bx, &by) && by > ay) {
      indent_shift(1);
      return CMD_KEEP_SELECTION;
    }
  }
  cursors_edit(MC_INDENT, '\t');
  return 0;
}

/* Shift-Tab, which has nothing to replace and so is always the block operation:
 * the selected rows, or one row per cursor when nothing is selected. */
static int cmd_dedent(void) {
  indent_shift(-1);
  return CMD_KEEP_SELECTION;
}

/* Convert the indentation that is already there. The selection is kept, as
 * Indent keeps it, so converting a block and then converting it back is two
 * presses rather than a reselection. */
static int convert_indent(int to_spaces) {
  int n = indent_convert(to_spaces);
  if (n == 0)
    editor_set_status_message("No indentation to convert %s",
                              CUR.sel_active ? "in the selection" : "in this file");
  else
    editor_set_status_message("%d line%s now indented with %s", n,
                              n == 1 ? "" : "s", to_spaces ? "spaces" : "tabs");
  return CMD_KEEP_SELECTION;
}
static int cmd_to_spaces(void) { return convert_indent(1); }
static int cmd_to_tabs(void) { return convert_indent(0); }

static int cmd_delete_back(void) {
  cursors_edit(MC_DELETE_BACK, 0);
  return 0;
}

static int cmd_delete_forward(void) {
  cursors_edit(MC_DELETE_FWD, 0);
  return 0;
}

/* A page is the text area's height, moved one display row at a time so the
 * movement follows whatever the view model says a row is. */
static int cmd_page_up(void) {
  int times = V.screenrows;
  while (times--) cursors_move_all(ARROW_UP);
  return 0;
}

static int cmd_page_down(void) {
  int times = V.screenrows;
  while (times--) cursors_move_all(ARROW_DOWN);
  return 0;
}

/* Every cursor, not just the primary: a column of cursors that an arrow key
 * collapsed to one would make the gesture useless. cursors_move_all() is the
 * single-cursor call itself when there is one cursor. */
static int cmd_left(void)  { cursors_move_all(ARROW_LEFT);  return 0; }
static int cmd_right(void) { cursors_move_all(ARROW_RIGHT); return 0; }
static int cmd_up(void)    { cursors_move_all(ARROW_UP);    return 0; }
static int cmd_down(void)  { cursors_move_all(ARROW_DOWN);  return 0; }

static int cmd_repaint(void) {
  screen_force_repaint();
  return 0;
}

/* The menu bar costs a row, so whether it is worth that row is the user's call. It
 * is a command before it is a setting: configuration arrives in Phase 2, and this
 * reads its default from there when it does. */
static int cmd_menu_bar(void) {
  E.menubar = !E.menubar;
  /* The text area moves by a row, so every column of every row shifts. */
  screen_force_repaint();
  editor_set_status_message("Menu bar: %s", E.menubar ? "on" : "off");
  return 0;
}

/* Write every setting, as it is right now, to the configuration file.
 *
 * Which is what makes the existing toggles persistent without any of them
 * knowing that configuration exists: Ctrl-U still just flips V.linenum, and the
 * settings table finds out by asking when this runs. The Options dialogs, when
 * they arrive, call exactly this. */
static int cmd_options(void) {
  editor_options_dialog();
  return 0;
}

static int cmd_save_settings(void) {
  const char *err = NULL;
  const char *path = config_path();
  if (config_write(path, &err) == 0)
    editor_set_status_message("Settings saved to %s", path);
  else
    editor_set_status_message("Could not save settings: %s",
                              err != NULL ? err : "unknown error");
  return 0;
}

static int cmd_tab_bar(void) {
  E.tabbar = !E.tabbar;
  /* The row appears or goes, so every row below it moves. */
  screen_force_repaint();
  editor_set_status_message("Tab bar: %s%s", E.tabbar ? "on" : "off",
      (E.tabbar && buf_count() < 2) ? " (shown once a second file is open)" : "");
  return 0;
}

static int cmd_syntax(void) {
  V.syntax = !V.syntax;
  screen_force_repaint();
  editor_set_status_message("Syntax highlighting: %s%s", V.syntax ? "on" : "off",
      (V.syntax && B.syn == NULL) ? " (no definition for this file)" : "");
  return 0;
}

/* Soft wrap, which is a per-window toggle like the gutter and the colours.
 *
 * The top line is kept where it is. The scroll offset is a display row, and the
 * same number means a different place in the file on each side of this toggle --
 * so it is read as a file row before the flip and asked for again after it,
 * which is the one thing that makes the screen look like it wrapped rather than
 * like it jumped. */
static int cmd_wrap(void) {
  struct drow d;
  struct cursor top;

  drow_at(V.drowoff, &d);
  V.wrap = !V.wrap;
  memset(&top, 0, sizeof top);
  top.cy = (d.filerow < row_count()) ? d.filerow : row_count();
  V.drowoff = drow_of_cursor(&top);
  /* Every row on screen is laid out differently, and the horizontal scroll goes
   * with it: wrap and sideways scrolling are alternatives. */
  screen_force_repaint();
  editor_set_status_message("Soft wrap: %s", V.wrap ? "on" : "off");
  return 0;
}

static int cmd_line_numbers(void) {
  V.linenum = !V.linenum;
  /* The gutter widens or shrinks the text area, shifting every column, so
   * repaint the whole screen rather than trust per-row diffing. */
  screen_force_repaint();
  editor_set_status_message("Line numbers: %s", V.linenum ? "on" : "off");
  return 0;
}

/* Filter the selection, or the whole buffer, through a shell command.
 *
 * CMD_KEEP_SELECTION because the dispatcher would otherwise clear the
 * selection on the way out -- which matters on the failure path, where the
 * whole point is that the screen is left as it was found. */
static int cmd_filter(void) {
  editor_filter();
  return CMD_KEEP_SELECTION;
}

/* Insert what a command prints. No CMD_KEEP_SELECTION: a selection is replaced
 * on success, and on failure it is cleared the way a failed paste would leave
 * it -- there is nothing on the screen this command promised to keep. */
static int cmd_insert_output(void) {
  editor_insert_output();
  return 0;
}

/* ---- windows -------------------------------------------------------------
 *
 * Named by where the new window lands rather than by the axis of the cut. "Split
 * horizontally" is the one piece of editor vocabulary nobody agrees on -- vi
 * splits horizontally and gets a window above another, every drawing program
 * means the opposite -- and a user who has to guess which one molly means will
 * guess wrong half the time. "Split right" and "Split below" cannot be read two
 * ways. */
static int cmd_window_split_right(void) {
  window_split(WIN_COLS);
  return 0;
}

static int cmd_window_split_below(void) {
  window_split(WIN_ROWS);
  return 0;
}

static int cmd_window_close(void) {
  window_close_current();
  return 0;
}

static int cmd_window_only(void) {
  window_only();
  return 0;
}

static int cmd_window_next(void) {
  window_move_focus(1);
  return 0;
}

static int cmd_window_prev(void) {
  window_move_focus(-1);
  return 0;
}

/* Move the border beside or above the current window. CMD_KEEP_SELECTION
 * because the size of the window the text is in has nothing to do with what
 * is selected in it, and losing a selection to a resize would be a surprise. */
static int cmd_border_left(void)  { window_move_border(-1, 0); return CMD_KEEP_SELECTION; }
static int cmd_border_right(void) { window_move_border(1, 0);  return CMD_KEEP_SELECTION; }
static int cmd_border_up(void)    { window_move_border(0, -1); return CMD_KEEP_SELECTION; }
static int cmd_border_down(void)  { window_move_border(0, 1);  return CMD_KEEP_SELECTION; }

/* Opening a menu is a command like any other, one per menu, so Alt-F and F10 are
 * ordinary bindings rather than a special case in the dispatcher -- and the command
 * palette will be able to offer "File menu" alongside everything else.
 *
 * Each returns what the command chosen inside the menu returned, so choosing Quit
 * from the File menu behaves exactly as Ctrl-Q does, countdown included. */
static int cmd_palette(void) { return palette_open(); }

static int cmd_menu_file(void)   { return menu_open(0); }
static int cmd_menu_edit(void)   { return menu_open(1); }
static int cmd_menu_search(void) { return menu_open(2); }
static int cmd_menu_view(void)   { return menu_open(3); }
static int cmd_menu_go(void)     { return menu_open(4); }
static int cmd_menu_window(void) { return menu_open(5); }
static int cmd_menu_help(void)   { return menu_open(6); }

/* F10 opens the bar at the first menu, which is where every keyboard-driven menu bar
 * since the 1980s has started. */
static int cmd_menu_bar_open(void) { return menu_open(0); }

/* Not a keystroke: a SIGWINCH woke the read loop. Nothing to do but let the next
 * refresh pick up the size -- without resetting the quit countdown, which a
 * terminal resize has no business cancelling. */
static int cmd_resize(void) {
  return CMD_KEEP_SELECTION | CMD_KEEP_QUIT_COUNT;
}

/* ---- what is available, and what is on -------------------------------------
 *
 * A menu greys out an item whose command is unavailable and ticks one that is on.
 * Each predicate lives beside the command it describes rather than in the menu, so
 * the palette and anything else that lists commands gets the same answers. */

static int have_selection(void) { return CUR.sel_active; }
static int have_clipboard(void) { return E.clipboard != NULL && E.clipsize > 0; }
static int have_rows(void) { return row_count() > 0; }
static int several_buffers(void) { return buf_count() > 1; }
/* Close is always available: with one buffer open it empties it, which is a
 * thing a user may well want and is what every editor with tabs does. */
static int on_a_row(void) { return CUR.cy < row_count(); }
static int linenumbers_on(void) { return V.linenum; }
static int menubar_on(void) { return E.menubar; }
static int tabbar_on(void) { return E.tabbar; }
static int syntax_on(void) { return V.syntax; }
static int wrap_on(void) { return V.wrap; }
/* With one window there is nothing to move to, nothing to close and nothing to
 * be the only one of, so all four grey out together. Splitting stays available:
 * whether there is room for it is a question about the window's size, and
 * window_split() answers it with a message that says which way it would not
 * fit -- which is more use than a greyed item that cannot say why. */
static int several_windows(void) { return window_count() > 1; }

/* The table. Order matters only for the help screen, which walks it in order
 * within each group. `label` is for the menu bar and the palette; `help` and
 * `group` are what the help screen shows, and a command with no group is not
 * listed there. */
static const struct command commands[] = {
  {.name = "file.save", .label = "Save",
   .help = "Save file", .group = "FILES", .run = cmd_save},
  {.name = "file.open", .label = "Open…",
   .help = "Open a file", .group = "FILES", .run = cmd_open},
  {.name = "file.new", .label = "New",
   .help = "New buffer", .group = "FILES", .run = cmd_new},
  {.name = "file.close", .label = "Close",
   .help = "Close this buffer", .group = "FILES", .run = cmd_close},
  {.name = "file.quit", .label = "Quit",
   .help = "Quit", .group = "FILES", .run = cmd_quit},

  {.name = "buffer.list", .label = "Buffers…",
   .help = "List buffers", .group = "BUFFERS", .run = cmd_buffer_list},
  {.name = "buffer.next", .label = "Next buffer",
   .help = "Next buffer", .group = "BUFFERS", .run = cmd_buffer_next,
   .enabled = several_buffers},
  {.name = "buffer.prev", .label = "Previous buffer",
   .help = "Previous buffer", .group = "BUFFERS", .run = cmd_buffer_prev,
   .enabled = several_buffers},
  {.name = "view.help", .label = "Keyboard help",
   .help = "This help screen", .group = "FILES", .run = cmd_help},

  {.name = "move.page-up", .label = "Page up",
   .flags = CMD_MOVEMENT, .run = cmd_page_up},
  {.name = "move.page-down", .label = "Page down",
   .flags = CMD_MOVEMENT, .run = cmd_page_down},
  {.name = "move.left", .label = "Left", .flags = CMD_MOVEMENT, .run = cmd_left},
  {.name = "move.right", .label = "Right", .flags = CMD_MOVEMENT, .run = cmd_right},
  {.name = "move.up", .label = "Up", .flags = CMD_MOVEMENT, .run = cmd_up},
  {.name = "move.down", .label = "Down", .flags = CMD_MOVEMENT, .run = cmd_down},
  {.name = "move.line-start", .label = "Line start",
   .flags = CMD_MOVEMENT, .run = cmd_line_start},
  {.name = "move.line-end", .label = "Line end",
   .flags = CMD_MOVEMENT, .run = cmd_line_end},
  {.name = "goto.line", .label = "Go to line…",
   .help = "Go to line", .group = "MOVEMENT", .run = cmd_goto_line},

  /* What a language server knows. The two problem commands are greyed in a
   * file with none rather than hidden, so the menu says where they would be;
   * F2 itself still answers, with why there is nothing to go to. */
  {.name = "goto.next-problem", .label = "Next problem",
   .help = "Next problem", .group = "CODE", .run = cmd_problem_next,
   .enabled = have_problems},
  {.name = "goto.prev-problem", .label = "Previous problem",
   .help = "Previous problem", .group = "CODE", .run = cmd_problem_prev,
   .enabled = have_problems},
  {.name = "goto.definition", .label = "Go to definition",
   .help = "Go to definition", .group = "CODE", .run = cmd_definition,
   .enabled = can_define},
  {.name = "goto.back", .label = "Back",
   .help = "Back from there", .group = "CODE", .run = cmd_back,
   .enabled = lspui_can_back},
  {.name = "code.hover", .label = "Info at cursor",
   .help = "Info at cursor", .group = "CODE", .run = cmd_hover,
   .enabled = can_hover},
  {.name = "code.complete", .label = "Complete word",
   .help = "Complete word", .group = "CODE", .run = cmd_complete,
   .enabled = can_complete},
  {.name = "lsp.status", .label = "Language server status",
   .run = cmd_lsp_status},
  {.name = "lsp.restart", .label = "Restart language servers",
   .run = cmd_lsp_restart},

  {.name = "edit.newline", .label = "Insert line",
   .help = "Insert new line", .group = "EDITING", .run = cmd_newline},
  {.name = "edit.delete-forward", .label = "Delete",
   .help = "Delete at cursor", .group = "EDITING", .run = cmd_delete_forward},
  {.name = "edit.delete-back", .label = "Backspace", .run = cmd_delete_back},
  {.name = "edit.indent", .label = "Indent",
   .help = "Indent the lines", .group = "EDITING", .run = cmd_indent},
  {.name = "edit.dedent", .label = "Dedent",
   .help = "Dedent the lines", .group = "EDITING", .run = cmd_dedent,
   .enabled = have_rows},
  /* Unbound, like Save settings: converting a file is something done once, from
   * the menu or the palette, not a chord the hand needs to know. The labels
   * say tabs and spaces rather than "indentation" because that is the word
   * people search the palette for, and the help line says it is only the
   * indentation. */
  {.name = "edit.to-spaces", .label = "Tabs to spaces",
   .help = "Indent to spaces", .group = "EDITING", .run = cmd_to_spaces,
   .enabled = have_rows},
  {.name = "edit.to-tabs", .label = "Spaces to tabs",
   .help = "Indent to tabs", .group = "EDITING", .run = cmd_to_tabs,
   .enabled = have_rows},
  {.name = "edit.transpose", .label = "Transpose characters",
   /* The help column is 17 columns and truncates in silence, so this is written
    * to fit it. "Swap the two characters at the cursor" came out as "Swap the
    * two char". */
   .help = "Swap characters", .group = "EDITING",
   .run = cmd_transpose, .enabled = on_a_row},

  /* Not in the Edit menu, for the reason transpose is not: these are keyboard
   * operations, and the palette and the help screen are generated from this
   * table so both find them without the menu growing four rows -- which that
   * menu can least afford, being the tallest one and so the first to scroll on
   * a short terminal. */
  {.name = "cursor.add-next", .label = "Add cursor at next match",
   .help = "Cursor: next match", .group = "CURSORS",
   .run = cmd_cursor_add_next, .enabled = have_rows},
  {.name = "cursor.add-below", .label = "Add cursor below",
   .help = "Cursor below", .group = "CURSORS", .run = cmd_cursor_add_below},
  {.name = "cursor.add-above", .label = "Add cursor above",
   .help = "Cursor above", .group = "CURSORS", .run = cmd_cursor_add_above},
  {.name = "cursor.skip-next", .label = "Skip this match",
   .help = "Cursor: skip match", .group = "CURSORS",
   .run = cmd_cursor_skip, .enabled = have_rows},
  {.name = "cursor.add-all", .label = "Add cursor at every match",
   .help = "Cursor: all matches", .group = "CURSORS",
   .run = cmd_cursor_add_all, .enabled = have_rows},
  {.name = "cursor.drop-last", .label = "Drop the last cursor",
   .help = "Drop last cursor", .group = "CURSORS",
   .run = cmd_cursor_drop, .enabled = cursors_several},
  {.name = "cursor.one", .label = "One cursor",
   .help = "Back to one cursor", .group = "CURSORS",
   .run = cmd_cursor_one, .enabled = cursors_several},
  /* A block selection, from the moving corner. In the palette and on the
   * help screen as one row, not in a menu, for the reason the cursor commands
   * above are not. */
  {.name = "block.up", .label = "Extend block up",
   .help = "Block up", .group = "CURSORS", .run = cmd_block_up,
   .enabled = have_rows},
  {.name = "block.down", .label = "Extend block down",
   .help = "Block down", .group = "CURSORS", .run = cmd_block_down,
   .enabled = have_rows},
  {.name = "block.left", .label = "Extend block left",
   .help = "Block left", .group = "CURSORS", .run = cmd_block_left,
   .enabled = have_rows},
  {.name = "block.right", .label = "Extend block right",
   .help = "Block right", .group = "CURSORS", .run = cmd_block_right,
   .enabled = have_rows},

  {.name = "edit.select-all", .label = "Select all",
   .help = "Select everything", .group = "SELECTION",
   .run = cmd_select_all, .enabled = have_rows},
  {.name = "edit.select-line", .label = "Select line",
   .help = "Select this line", .group = "SELECTION",
   .run = cmd_select_line, .enabled = on_a_row},

  {.name = "search.find", .label = "Find…",
   .help = "Find forward", .group = "FIND & REPLACE", .run = cmd_find},
  {.name = "search.next", .label = "Find next",
   .help = "Find next", .group = "FIND & REPLACE", .run = cmd_find_next, .enabled = search_has_query},
  {.name = "search.prev", .label = "Find previous",
   .help = "Find previous", .group = "FIND & REPLACE", .run = cmd_find_prev, .enabled = search_has_query},
  {.name = "search.case", .label = "Match case",
   .help = "Case sensitivity", .group = "FIND & REPLACE", .run = cmd_case_toggle, .checked = search_case_is_on},
  {.name = "search.regex", .label = "Regular expression",
   .help = "Pattern search", .group = "FIND & REPLACE",
   .run = cmd_regex_toggle, .checked = search_regex_is_on},
  {.name = "search.replace", .label = "Replace…",
   .help = "Find & replace", .group = "FIND & REPLACE", .run = cmd_replace},
  {.name = "search.in-files", .label = "Find in files…",
   .help = "Search all files", .group = "FIND & REPLACE", .run = cmd_grep},
  {.name = "search.next-result", .label = "Next result",
   .help = "Next file match", .group = "FIND & REPLACE", .run = cmd_grep_next,
   .enabled = have_grep_results},
  {.name = "search.prev-result", .label = "Previous result",
   .help = "Prev file match", .group = "FIND & REPLACE", .run = cmd_grep_prev,
   .enabled = have_grep_results},

  {.name = "edit.copy", .label = "Copy",
   .help = "Copy selection", .group = "CLIPBOARD", .run = cmd_copy, .enabled = have_selection},
  {.name = "edit.cut", .label = "Cut",
   .help = "Cut selection", .group = "CLIPBOARD", .run = cmd_cut, .enabled = have_selection},
  {.name = "edit.paste", .label = "Paste",
   .help = "Paste at cursor", .group = "CLIPBOARD", .run = cmd_paste, .enabled = have_clipboard},

  {.name = "edit.undo", .label = "Undo",
   .help = "Undo", .group = "HISTORY", .run = cmd_undo, .enabled = undo_can_undo},
  {.name = "edit.redo", .label = "Redo",
   .help = "Redo", .group = "HISTORY", .run = cmd_redo, .enabled = undo_can_redo},

  {.name = "view.repaint", .label = "Redraw",
   .help = "Redraw screen", .group = "VIEW", .run = cmd_repaint},
  {.name = "view.line-numbers", .label = "Line numbers",
   .help = "Line numbers", .group = "VIEW", .run = cmd_line_numbers, .checked = linenumbers_on},
  {.name = "view.menu-bar", .label = "Menu bar",
   .help = "Show the menu bar", .group = "VIEW", .run = cmd_menu_bar, .checked = menubar_on},
  /* Unbound by default: there is no CUA chord for it, and the two places it
   * belongs -- the View menu and the palette -- are enough. It lives under View
   * rather than File because what it saves is the View, Search and Input
   * toggles; it moves to an Options menu when there is one. */
  {.name = "view.syntax", .label = "Syntax highlighting",
   .help = "Colour by filetype", .group = "VIEW", .run = cmd_syntax,
   .checked = syntax_on},
  {.name = "view.tab-bar", .label = "Tab bar",
   .help = "Show a tab per file", .group = "VIEW", .run = cmd_tab_bar,
   .checked = tabbar_on},
  {.name = "view.soft-wrap", .label = "Soft wrap",
   .help = "Wrap long lines", .group = "VIEW", .run = cmd_wrap,
   .checked = wrap_on},
  /* Unbound, beside Save settings: both are things done now and then from the
   * menu, and every setting in it already has a toggle or a chord of its own
   * for the times it is wanted mid-keystroke. */
  {.name = "app.options", .label = "Options…", .run = cmd_options},
  {.name = "app.save-settings", .label = "Save settings",
   .help = "Save settings to mollyrc", .group = "VIEW", .run = cmd_save_settings},

  /* The help screen's description column is seventeen columns wide, and
   * "Split, new window right" and "Split, new window below" both cut to "Split,
   * new window" -- two entries that read identically, one above the other. The
   * word that distinguishes them has to be inside the budget. */
  /* "Filter through…" rather than "Filter through command…": the longer label
   * widened the whole Edit menu by ten columns to say what the prompt says
   * next anyway. The help description is seventeen columns and says `sh`
   * because that is the useful part -- it is what makes a pipeline work. */
  {.name = "edit.filter", .label = "Filter through…",
   .help = "Filter through sh", .group = "EDITING", .run = cmd_filter,
   .enabled = have_rows},
  /* Not "Insert command output…", for the filter's reason: seven more columns
   * on the widest menu molly has, to say what the prompt then says. No
   * `enabled`: unlike the filter it needs no text, and an empty buffer is a
   * fine place to start from `ls`. */
  {.name = "edit.insert-output", .label = "Insert output…",
   .help = "Insert sh output", .group = "EDITING", .run = cmd_insert_output},

  {.name = "window.split-right", .label = "Split right",
   .help = "New window right", .group = "WINDOWS",
   .run = cmd_window_split_right},
  {.name = "window.split-below", .label = "Split below",
   .help = "New window below", .group = "WINDOWS",
   .run = cmd_window_split_below},
  {.name = "window.next", .label = "Next window",
   .help = "Next window", .group = "WINDOWS", .run = cmd_window_next,
   .enabled = several_windows},
  {.name = "window.prev", .label = "Previous window",
   .help = "Previous window", .group = "WINDOWS", .run = cmd_window_prev,
   .enabled = several_windows},
  {.name = "window.close", .label = "Close window",
   .help = "Close this window", .group = "WINDOWS", .run = cmd_window_close,
   .enabled = several_windows},
  /* Unbound, like Save settings: the menu and the palette are enough for
   * something nobody needs mid-keystroke, and a chord spent here is a chord not
   * spent on the four above. */
  {.name = "window.only", .label = "Only this window",
   .run = cmd_window_only, .enabled = several_windows},
  /* Named for the border and the way it goes, not for the window growing: the
   * same arrow moves the same line whichever side of it the keyboard is on, so
   * "Wider" would be true in one window and false in the other. */
  {.name = "window.border-left", .label = "Move border left",
   .help = "Border left", .group = "WINDOWS", .run = cmd_border_left,
   .enabled = several_windows},
  {.name = "window.border-right", .label = "Move border right",
   .help = "Border right", .group = "WINDOWS", .run = cmd_border_right,
   .enabled = several_windows},
  {.name = "window.border-up", .label = "Move border up",
   .help = "Border up", .group = "WINDOWS", .run = cmd_border_up,
   .enabled = several_windows},
  {.name = "window.border-down", .label = "Move border down",
   .help = "Border down", .group = "WINDOWS", .run = cmd_border_down,
   .enabled = several_windows},

  /* Hidden from the palette, all of them: the palette has no business offering to
   * open itself, and the menu openers are how you reach commands rather than commands
   * anyone means to run. */
  {.name = "app.palette", .label = "Command palette…", .flags = CMD_HIDDEN,
   .help = "Command palette", .group = "MENUS", .run = cmd_palette},

  {.name = "menu.open", .label = "Menu bar", .flags = CMD_HIDDEN, .run = cmd_menu_bar_open},
  {.name = "menu.file", .label = "File menu", .flags = CMD_HIDDEN, .run = cmd_menu_file},
  {.name = "menu.edit", .label = "Edit menu", .flags = CMD_HIDDEN, .run = cmd_menu_edit},
  {.name = "menu.search", .label = "Search menu", .flags = CMD_HIDDEN, .run = cmd_menu_search},
  {.name = "menu.view", .label = "View menu", .flags = CMD_HIDDEN, .run = cmd_menu_view},
  {.name = "menu.go", .label = "Go menu", .flags = CMD_HIDDEN, .run = cmd_menu_go},
  {.name = "menu.window", .label = "Window menu", .flags = CMD_HIDDEN, .run = cmd_menu_window},
  {.name = "menu.help", .label = "Help menu", .flags = CMD_HIDDEN, .run = cmd_menu_help},

  {.name = "app.resize", .run = cmd_resize},
};

#define NCOMMANDS ((int)(sizeof(commands) / sizeof(commands[0])))

const struct command *cmd_find_by_name(const char *name) {
  int i;
  for (i = 0; i < NCOMMANDS; i++)
    if (strcmp(commands[i].name, name) == 0) return &commands[i];
  return NULL;
}

/* The table by position, for anything that lists every command rather than looking one
 * up: the command palette today, a keybinding editor and a macro recorder later. */
int cmd_count(void) {
  return NCOMMANDS;
}

const struct command *cmd_at(int i) {
  if (i < 0 || i >= NCOMMANDS) return NULL;
  return &commands[i];
}

/* ---- chords -------------------------------------------------------------- */

/* Keys with a name rather than a character. The first spelling is what
 * chord_name prints; the rest are accepted by chord_parse. */
struct named_key { int key; const char *name; const char *alias; };

static const struct named_key named_keys[] = {
  {ARROW_LEFT,  "Left",      "left"},
  {ARROW_RIGHT, "Right",     "right"},
  {ARROW_UP,    "Up",        "up"},
  {ARROW_DOWN,  "Down",      "down"},
  {HOME_KEY,    "Home",      "home"},
  {END_KEY,     "End",       "end"},
  {PAGE_UP,     "PgUp",      "pageup"},
  {PAGE_DOWN,   "PgDn",      "pagedown"},
  {DEL_KEY,     "Del",       "delete"},
  {BACKSPACE,   "Backspace", "bksp"},
  {'\r',        "Enter",     "return"},
  {'\t',        "Tab",       "tab"},
  {'\x1b',      "Esc",       "escape"},
  /* Shift-Tab has its own key code rather than Tab plus the Shift bit, because
   * that is how it arrives -- as its own escape sequence. So it is a named key
   * here, and "shift-tab" is parsed by this table rather than by the modifier
   * loop below. */
  {BACKTAB_KEY, "Shift-Tab", "shift-tab"},
  {WINCH_KEY,   "Resize",    "resize"},
  /* Ctrl-Space arrives as NUL, which is what CTRL_KEY(' ') produces. It is named here
   * rather than handled in the "ctrl-" branch below so that one entry covers both
   * parsing it and printing it. */
  {CTRL_KEY(' '), "Ctrl-Space", "ctrl-space"},
};

#define NNAMED ((int)(sizeof(named_keys) / sizeof(named_keys[0])))

static int ieq(const char *a, const char *b) {
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

/* Whether a modifier on this key reaches molly as something of its own.
 *
 * The CSI forms carry a modifier parameter for the function keys and the
 * navigation keys -- "\x1b[1;2C" is Shift-Right, "\x1b[5;5~" is Ctrl-PgUp -- so
 * those can be bound with either. A letter cannot: Shift-A *is* 'A', and
 * Ctrl-Shift-L is the same single byte as Ctrl-L, which is why the roadmap's
 * suggestion of moving repaint to Ctrl-Shift-L could not be taken. Ctrl on a
 * letter is still the control byte it has always been. */
static int mods_are_reported(int key) {
  switch (key) {
    case ARROW_LEFT: case ARROW_RIGHT: case ARROW_UP: case ARROW_DOWN:
    case HOME_KEY: case END_KEY: case PAGE_UP: case PAGE_DOWN: case DEL_KEY:
      return 1;
    default:
      return key >= F1_KEY && key <= F12_KEY;
  }
}

/* Parse a chord: "ctrl-s", "alt-x", "shift-f3", "ctrl-pagedown", "f10", "left",
 * "enter", or a bare character. Returns the key code -- with KEY_SHIFT and
 * KEY_CTRL set as the chord named them -- or -1 if it means nothing.
 *
 * Case-insensitive, and the modifiers may be written in any order, so a config
 * file may say "Ctrl-S", "ctrl-s" or "shift-ctrl-f3". */
int chord_parse(const char *s) {
  int i;
  int ctrl = 0, alt = 0, shift = 0;
  int key, mods = 0;

  if (s == NULL || *s == '\0') return -1;

  /* Named keys first, because two of them are spelled with a modifier prefix --
   * "ctrl-space" and "shift-tab" -- and the loop below would otherwise claim them
   * and then reject what was left. No other name begins with one. */
  for (i = 0; i < NNAMED; i++)
    if (ieq(s, named_keys[i].name) || ieq(s, named_keys[i].alias))
      return named_keys[i].key;

  /* Ctrl-? is spelled out because '?' is not a letter and the Ctrl branch below
   * only accepts letters. It is 0x1f, which is also Ctrl-_. */
  if (ieq(s, "ctrl-?")) return CTRL_KEY('?');

  for (;;) {
    if (strncasecmp(s, "ctrl-", 5) == 0) { ctrl = 1; s += 5; continue; }
    if (strncasecmp(s, "alt-", 4) == 0) { alt = 1; s += 4; continue; }
    if (strncasecmp(s, "shift-", 6) == 0) { shift = 1; s += 6; continue; }
    break;
  }
  if (*s == '\0') return -1;          /* modifiers with nothing to modify */
  if (ctrl && alt) return -1;         /* not reported distinguishably either */

  /* The base key. A named key may appear here too, so "shift-pageup" works. */
  key = -1;
  for (i = 0; i < NNAMED; i++) {
    if (ieq(s, named_keys[i].name) || ieq(s, named_keys[i].alias)) {
      key = named_keys[i].key;
      break;
    }
  }
  if (key < 0 && (s[0] == 'f' || s[0] == 'F') && isdigit((unsigned char)s[1])) {
    int n = atoi(s + 1);
    char tail[8];
    snprintf(tail, sizeof(tail), "%d", n);
    /* "f1x" is not F1: the digits have to be the whole of it. */
    if (n >= 1 && n <= 12 && strcasecmp(s + 1, tail) == 0) key = F1_KEY + (n - 1);
  }
  if (key < 0 && s[1] == '\0' && (unsigned char)s[0] >= 0x20 &&
      (unsigned char)s[0] <= 0x7e)
    key = (unsigned char)s[0];
  if (key < 0) return -1;

  if (ctrl) {
    if (mods_are_reported(key)) {
      /* A navigation or function key, where Ctrl arrives as a parameter: a flag
       * on the key rather than a different key. */
      mods |= KEY_CTRL;
    } else {
      /* A letter, where Ctrl is a byte. `key` is rewritten rather than flagged,
       * which also means mods_are_reported() below now says no -- so
       * "ctrl-shift-l" is refused, correctly, because the terminal cannot tell
       * it from "ctrl-l". */
      unsigned char c = (unsigned char)toupper((unsigned char)s[0]);
      if (s[1] != '\0' || c < 'A' || c > 'Z') return -1;
      key = CTRL_KEY(c);
    }
  }
  if (alt) {
    if (mods_are_reported(key)) {
      /* An arrow or the like, where Alt is a parameter like the other two --
       * which is what lets "alt-shift-up" draw a block selection. */
      mods |= KEY_ALT;
    } else {
      if (s[1] != '\0' || (unsigned char)s[0] < 0x20 ||
          (unsigned char)s[0] > 0x7e)
        return -1;
      key = ALT_KEY(s[0]);
    }
  }
  if (shift) {
    if (!mods_are_reported(key)) return -1;
    mods |= KEY_SHIFT;
  }
  return key | mods;
}

/* The display form of a chord: "Ctrl-S", "Alt-X", "F10", "PgUp". This is what the
 * help screen prints and what the menu bar will print down its right-hand side. */
void chord_name(int key, char *dst, size_t n) {
  int i;

  /* The modifier flags are prefixes on whatever the rest of the key is called,
   * so they are peeled off one at a time and the bare key named by the code
   * below -- which is what keeps "Ctrl-" and "Shift-" out of every branch of it.
   * Ctrl, then Alt, then Shift, which is the order every editor prints them in. */
  if (key & KEY_CTRL) {
    char base[32];
    chord_name(key & ~KEY_CTRL, base, sizeof(base));
    snprintf(dst, n, "Ctrl-%s", base);
    return;
  }
  if (key & KEY_ALT) {
    char base[32];
    chord_name(key & ~KEY_ALT, base, sizeof(base));
    snprintf(dst, n, "Alt-%s", base);
    return;
  }
  if (key & KEY_SHIFT) {
    char base[32];
    chord_name(KEY_BASE(key), base, sizeof(base));
    snprintf(dst, n, "Shift-%s", base);
    return;
  }

  for (i = 0; i < NNAMED; i++) {
    if (named_keys[i].key == key) {
      snprintf(dst, n, "%s", named_keys[i].name);
      return;
    }
  }
  if (key >= F1_KEY && key <= F12_KEY) {
    snprintf(dst, n, "F%d", key - F1_KEY + 1);
    return;
  }
  if (IS_ALT_KEY(key)) {
    snprintf(dst, n, "Alt-%c", toupper(ALT_KEY_CHAR(key)));
    return;
  }
  /* Control characters print as Ctrl-<letter>. 0x1f is Ctrl-? in molly, which is
   * also Ctrl-_; the editor binds it as help, so name it the way it is bound. */
  if (key == CTRL_KEY('?')) {
    snprintf(dst, n, "Ctrl-?");
    return;
  }
  if (key >= 1 && key <= 26) {
    snprintf(dst, n, "Ctrl-%c", 'A' + key - 1);
    return;
  }
  if (key >= 0x20 && key <= 0x7e) {
    snprintf(dst, n, "%c", key);
    return;
  }
  snprintf(dst, n, "?");
}

/* ---- the keymap ---------------------------------------------------------- */

/* The default bindings, as text, parsed at startup. Several chords may share one
 * command: Backspace and Ctrl-H both delete backwards, and where that happens the
 * first one listed is the one the menus and the help screen print.
 *
 * CUA, which resolves the four places olly disagreed with it:
 *
 *   Ctrl-L  was repaint, is select-line. Repaint keeps the View menu and gains
 *           F5, which is what every browser and file manager has used for
 *           "refresh" since the nineties.
 *   Ctrl-T  was toggle-case-sensitive, is transpose. Case is a checkbox in the
 *           Find dialog now, which is better UI than a hidden toggle with a
 *           status-bar message, so the command keeps its menu entry and loses
 *           its chord.
 *   Ctrl-N  was find-next, still is; F3 and Shift-F3 become the primary pair and
 *           Ctrl-N / Ctrl-P stay as aliases.
 *   Ctrl-?  was help, still is; F1 becomes primary.
 *
 * The roadmap suggested moving repaint to Ctrl-Shift-L. That is not available:
 * a terminal sends the same single byte for Ctrl-Shift-L as for Ctrl-L, so the
 * binding could never have fired. F5 instead -- see ROADMAP.md section 6.
 *
 * Ctrl-A for select-all is the one CUA binding whose absence would have been
 * conspicuous. Ctrl-W (select word), Ctrl-K (delete to end of line) and Ctrl-/
 * (toggle comment) are free and still unbound: they need commands that do not
 * exist yet, and binding a chord before there is something to bind it to is how
 * a keymap acquires entries nobody can explain. */
struct binding { const char *chord; const char *cmd; };

static const struct binding default_bindings[] = {
  {"ctrl-s", "file.save"},
  {"ctrl-o", "file.open"},
  /* Ctrl-N for New and Ctrl-W for Close, which is what CUA says and what every
   * editor and browser with tabs does. Both were spoken for on paper: Ctrl-N was
   * olly's find-next, which keeps working as Ctrl-P's opposite number only
   * because F3 is the primary now -- so it is free; and ROADMAP.md section 6
   * earmarked Ctrl-W for select-word, which does not exist, and close-tab is the
   * far more expected meaning. Both are one line of mollyrc away for anyone who
   * disagrees. */
  {"ctrl-n", "file.new"},
  {"ctrl-w", "file.close"},
  /* The universal pair for walking tabs, and the reason Ctrl became nameable in
   * a chord at all. */
  {"ctrl-pagedown", "buffer.next"},
  {"ctrl-pageup",   "buffer.prev"},
  {"ctrl-q", "file.quit"},
  {"ctrl-b", "buffer.list"},
  /* F1 first, so it is the one the menus and the help screen print. */
  {"f1",     "view.help"},
  {"ctrl-?", "view.help"},

  {"enter",  "edit.newline"},
  {"bksp",   "edit.delete-back"},
  {"ctrl-h", "edit.delete-back"},
  {"delete", "edit.delete-forward"},

  {"ctrl-z", "edit.undo"},
  {"ctrl-y", "edit.redo"},
  {"ctrl-c", "edit.copy"},
  {"ctrl-x", "edit.cut"},
  {"ctrl-v", "edit.paste"},
  {"ctrl-a", "edit.select-all"},
  {"ctrl-l", "edit.select-line"},
  {"ctrl-t", "edit.transpose"},
  /* Ctrl-D for the next match, which is where every editor that has this puts
   * it. Above and below take Ctrl with the arrows rather than the Ctrl-Alt the
   * GUI editors use, because a chord here can name Shift or Ctrl and a terminal
   * does not report both at once on an arrow. */
  {"ctrl-d",    "cursor.add-next"},
  {"esc",       "cursor.one"},
  /* Alt-Shift and an arrow, which is Sublime's, Notepad++'s and JetBrains'
   * block selection -- VS Code adds Ctrl to it, which a terminal cannot carry
   * alongside Alt. Shift because it selects, Alt because it is the other
   * kind of selecting, as Alt-click is the other kind of click. */
  {"alt-shift-up",    "block.up"},
  {"alt-shift-down",  "block.down"},
  {"alt-shift-left",  "block.left"},
  {"alt-shift-right", "block.right"},
  {"ctrl-down", "cursor.add-below"},
  {"ctrl-up",   "cursor.add-above"},
  /* Ctrl-D takes the match and Alt-D steps over it, which is the only pair of
   * chords for these two that a terminal can carry: the editors that have them
   * use Ctrl-K Ctrl-D, and molly has no two-chord sequences. Alt-U takes the
   * last cursor back -- VS Code's Ctrl-U, which molly spent on the line-number
   * gutter long before this -- and Alt-A is every match at once. */
  {"alt-d",     "cursor.skip-next"},
  {"alt-u",     "cursor.drop-last"},
  {"alt-a",     "cursor.add-all"},
  /* Tab is a binding rather than a special case in the typed-character path, so
   * that it reads in the help screen and the menu like every other command and
   * can be rebound. Unbinding it (`tab = none`) leaves the typed path, which
   * still types one indent step. */
  {"tab",       "edit.indent"},
  {"shift-tab", "edit.dedent"},
  /* Alt-| for the filter, which is where Fresh puts it and the only mnemonic
   * there could be: the character is a pipe, and the command is a pipe. */
  {"alt-|",  "edit.filter"},
  /* Alt-! beside it: `!` is the shell in vi's `:r !date` and in Emacs's M-!,
   * which is this command's nearest relative in either. */
  {"alt-!",  "edit.insert-output"},

  {"ctrl-f", "search.find"},
  /* F3 first for the same reason F1 is: it is what gets printed. Shift-F3 is the
   * binding that the Shift-in-a-chord work was for.
   *
   * Ctrl-N is no longer here: New took it, as CUA has it. Ctrl-P stays, because
   * it costs nothing and olly's fingers know it. */
  {"f3",       "search.next"},
  {"shift-f3", "search.prev"},
  {"ctrl-p",   "search.prev"},
  {"alt-r",  "search.regex"},
  {"ctrl-r", "search.replace"},
  /* Ctrl-E, because Ctrl-Shift-F -- what most editors use -- reaches a
   * terminal program as plain Ctrl-F, and E was one of two Ctrl letters left.
   * F4 and Shift-F4 step through the results, which is VS Code's pair, and
   * sits beside F3's find-next where the hand already is. */
  {"ctrl-e",   "search.in-files"},
  {"f4",       "search.next-result"},
  {"shift-f4", "search.prev-result"},
  {"ctrl-g", "goto.line"},
  /* F2 and Shift-F2, a pair like F3's and F4's. VS Code's F8 is a split here,
   * and F2 is free until molly renames anything. */
  {"f2",       "goto.next-problem"},
  {"shift-f2", "goto.prev-problem"},
  /* F12 is VS Code's and Visual Studio's. Back is Alt-Left, as in a browser
   * and in VS Code on Windows; Alt-K for what is under the cursor, after vi's
   * K, because Ctrl-K is a chord a terminal cannot follow with a second. */
  {"f12",      "goto.definition"},
  {"alt-left", "goto.back"},
  {"alt-k",    "code.hover"},
  /* Alt-/ for completion, as in Emacs and the editors that took it from
   * there: Ctrl-Space is the palette's, and a terminal cannot tell it from
   * Ctrl-@ to give it a second meaning. */
  {"alt-/",    "code.complete"},

  {"left",   "move.left"},
  {"right",  "move.right"},
  {"up",     "move.up"},
  {"down",   "move.down"},
  {"home",   "move.line-start"},
  {"end",    "move.line-end"},
  {"pageup", "move.page-up"},
  {"pagedown", "move.page-down"},

  {"f5",     "view.repaint"},
  {"ctrl-u", "view.line-numbers"},
  /* Alt-M for the menu bar. It had no chord at all, which the help screen showed
   * as a blank key column beside a description -- invisible until the row cap
   * there was raised and the VIEW group became reachable. M is free: the eight
   * Alt letters the menu bar reserves are F, E, S, V, G, H and the T and O that
   * Tools and Options will take. */
  {"alt-m",  "view.menu-bar"},
  /* Alt-B for the tab bar. B is not one of the eight the menu bar reserves. */
  {"alt-b",  "view.tab-bar"},
  /* Alt-Y for syntax. Not S, which the Search menu owns; not C or H, which read
   * as "colour" and "highlight" but belong to Help and to nothing. */
  {"alt-y",  "view.syntax"},
  /* Alt-Z for soft wrap, which is where VS Code has it -- the one chord in this
   * table that a user is likely to arrive already knowing. W would have been
   * the mnemonic and the Window menu has it. */
  {"alt-z",  "view.soft-wrap"},

  /* Windows, on the function keys from F6 up. F6 is where Turbo Vision put
   * "next window" and where molly's model for all of this came from, and the
   * four that follow it group with it: a user who finds one finds the rest by
   * pressing the next key along, which is not true of any set of Alt letters
   * still free. Nothing mnemonic is left anyway -- W is the Window menu's own
   * Alt letter, S and V belong to Search and View, and C would sit one key away
   * from Copy while meaning Close. */
  {"f6",       "window.next"},
  {"shift-f6", "window.prev"},
  {"f7",       "window.split-right"},
  {"f8",       "window.split-below"},
  {"f9",       "window.close"},
  /* Ctrl-Shift and an arrow. Until now those fell through to Shift and the
   * arrow -- a second spelling of extending the selection, which nobody types
   * on purpose -- and they are the arrows, which is what a border move wants.
   * What this costs is the CUA word selection, Ctrl-Shift-Left and -Right, if
   * molly ever gets word movement; a binding is a line in mollyrc, so that is
   * a choice deferred rather than made. */
  {"ctrl-shift-left",  "window.border-left"},
  {"ctrl-shift-right", "window.border-right"},
  {"ctrl-shift-up",    "window.border-up"},
  {"ctrl-shift-down",  "window.border-down"},

  /* The palette, on two chords. Ctrl-Space is the one the roadmap settles on, and
   * Alt-X is there because not every terminal sends NUL for Ctrl-Space -- and because
   * Alt-X has meant "run a command by name" for forty years. X is not one of the
   * letters the menu bar reserves. */
  {"ctrl-space", "app.palette"},
  {"alt-x",      "app.palette"},

  /* The menu bar: F10 as every keyboard-driven menu bar has had it, plus the eight
   * reserved Alt letters -- seven of which have a menu today. */
  {"f10",    "menu.open"},
  {"alt-f",  "menu.file"},
  {"alt-e",  "menu.edit"},
  {"alt-s",  "menu.search"},
  {"alt-v",  "menu.view"},
  {"alt-g",  "menu.go"},
  /* W, which was free: the eight letters the bar reserves were F, E, S, V, G, H
   * and the T and O that Tools and Options will take, and a Window menu is the
   * seventh of them to exist. */
  {"alt-w",  "menu.window"},
  {"alt-h",  "menu.help"},

  {"resize", "app.resize"},
};

#define NBINDINGS ((int)(sizeof(default_bindings) / sizeof(default_bindings[0])))

/* The live keymap: a growable array, because the configuration file adds to it.
 * A linear scan over forty-odd entries, once per keystroke, at human typing
 * speed -- it still does not need to be a hash table, and saying so here is
 * cheaper than someone later wondering whether it does.
 *
 * One entry per key, never two: binding a chord that is already bound replaces
 * it, which is what makes "user config beats the built-in keymap" a property of
 * the structure rather than of the lookup order. `user` records which bindings
 * came from the file, so that write-back can put back exactly what the user
 * asked for and nothing else. */
static struct keybind {
  int key;
  const struct command *cmd;
  int user;
} *keymap;

static int keymap_n, keymap_cap;

static void keymap_grow(void) {
  if (keymap_n < keymap_cap) return;
  keymap_cap = keymap_cap ? keymap_cap * 2 : NBINDINGS + 16;
  keymap = xrealloc(keymap, (size_t)keymap_cap * sizeof(*keymap));
}

static int keymap_index_of(int key) {
  int i;
  for (i = 0; i < keymap_n; i++)
    if (keymap[i].key == key) return i;
  return -1;
}

/* Parse the default bindings into the keymap. A bad chord or an unknown command
 * is a programming error in the table above, so it exits rather than silently
 * dropping the binding; a binding that comes from the config file warns and
 * carries on instead, which is keymap_bind's job below. */
void keymap_init(void) {
  int i;
  keymap_n = 0;
  for (i = 0; i < NBINDINGS; i++) {
    int key = chord_parse(default_bindings[i].chord);
    const struct command *cmd = cmd_find_by_name(default_bindings[i].cmd);
    if (key < 0 || cmd == NULL) die("keymap_init: bad default binding");
    keymap_grow();
    keymap[keymap_n].key = key;
    keymap[keymap_n].cmd = cmd;
    keymap[keymap_n].user = 0;
    keymap_n++;
  }
}

void keymap_cleanup(void) {
  free(keymap);
  keymap = NULL;
  keymap_n = 0;
  keymap_cap = 0;
}

/* Is this chord one of the Alt letters the menu bar owns?
 *
 * Those eight are the one thing the roadmap says is not remappable, because they
 * are structural: the bar draws F as the mnemonic of "File" whether or not Alt-F
 * still opens it, so a rebinding would make the menu bar lie about itself. The
 * set is read off the menu table rather than written out here, so adding a menu
 * reserves its letter without anyone remembering to. */
static int chord_is_reserved(int key) {
  int i;
  if (!IS_ALT_KEY(key)) return 0;
  for (i = 0; i < menu_count(); i++) {
    int ch = menu_mnemonic_char(i);
    if (ch > 0 && tolower(ch) == tolower(ALT_KEY_CHAR(key))) return 1;
  }
  return 0;
}

/* Bind a chord to a command, or to "none" to leave it unbound. Returns 1 on
 * success, or 0 with *err set to something worth printing.
 *
 * This is the configuration file's way in, so every refusal here is a line in a
 * file somebody wrote by hand. The messages say what is wrong with the line
 * rather than what the code expected. */
int keymap_bind(const char *chord, const char *cmdname, const char **err) {
  int key = chord_parse(chord);
  int at;

  if (key < 0) {
    if (err) *err = "not a key";
    return 0;
  }
  if (chord_is_reserved(key)) {
    if (err) *err = "reserved by the menu bar";
    return 0;
  }

  at = keymap_index_of(key);

  if (strcasecmp(cmdname, "none") == 0) {
    if (at >= 0) {
      /* Order is not meaningful -- lookup is a scan -- so closing the gap with
       * the last entry beats moving everything after it. */
      keymap[at] = keymap[keymap_n - 1];
      keymap_n--;
    }
    return 1;
  }

  const struct command *cmd = cmd_find_by_name(cmdname);
  if (cmd == NULL) {
    if (err) *err = "no such command";
    return 0;
  }

  if (at >= 0) {
    keymap[at].cmd = cmd;
    keymap[at].user = 1;
  } else {
    keymap_grow();
    keymap[keymap_n].key = key;
    keymap[keymap_n].cmd = cmd;
    keymap[keymap_n].user = 1;
    keymap_n++;
  }
  return 1;
}

/* The `[keys]` section of a written configuration file: the bindings that differ
 * from the defaults, and nothing else.
 *
 * Writing all forty would pin the user to this version's defaults -- a later
 * molly that moves a key could never move it for anyone who had ever saved their
 * settings. So a changed chord is written as itself, and a default chord that is
 * now bound to nothing is written as "none". */
void keymap_write_changes(FILE *out) {
  int i;
  char chord[32];
  int wrote = 0;

  for (i = 0; i < keymap_n; i++) {
    if (!keymap[i].user) continue;
    chord_name(keymap[i].key, chord, sizeof(chord));
    fprintf(out, "%s = %s\n", chord, keymap[i].cmd->name);
    wrote++;
  }
  for (i = 0; i < NBINDINGS; i++) {
    int key = chord_parse(default_bindings[i].chord);
    if (key < 0 || keymap_index_of(key) >= 0) continue;
    chord_name(key, chord, sizeof(chord));
    fprintf(out, "%s = none\n", chord);
    wrote++;
  }
  if (!wrote) fprintf(out, "\n# (none; molly's defaults are all in use)\n");
}

/* Print the parsed keymap as "chord<TAB>command" lines.
 *
 * A test hook (documented in MANUAL.md) and a check on the chord code: every
 * binding here has been through chord_parse on the way in and chord_name on the
 * way out, so the text that comes back proves the round trip for the whole
 * default keymap. It is also the shape the config file will read and write. */
void keymap_dump(FILE *out) {
  int i;
  char chord[32];
  for (i = 0; i < keymap_n; i++) {
    chord_name(keymap[i].key, chord, sizeof(chord));
    fprintf(out, "%s\t%s\n", chord, keymap[i].cmd->name);
  }
}

const struct command *keymap_lookup(int key) {
  int i;
  for (i = 0; i < keymap_n; i++)
    if (keymap[i].key == key) return keymap[i].cmd;
  return NULL;
}

/* The chord bound to a command, for the help screen and the menu bar's shortcut
 * column. The first binding wins, which is why Backspace is listed before
 * Ctrl-H above. Returns 0 and leaves dst empty when nothing is bound. */
int cmd_shortcut(const char *name, char *dst, size_t n) {
  int pass, i;
  /* A binding from the configuration file is shown in preference to a default
   * one, so a user who moved Save to F9 reads "F9" in the File menu even while
   * Ctrl-S still works. Within a pass the first match wins, which is why
   * Backspace is listed before Ctrl-H in the defaults. */
  for (pass = 1; pass >= 0; pass--) {
    for (i = 0; i < keymap_n; i++) {
      if (keymap[i].user != pass) continue;
      if (strcmp(keymap[i].cmd->name, name) == 0) {
        chord_name(keymap[i].key, dst, n);
        return 1;
      }
    }
  }
  if (n) dst[0] = '\0';
  return 0;
}

/* ---- the dispatcher ------------------------------------------------------ */

/* Act on one key. The key is passed in rather than read here, so that exactly one
 * place in molly waits for input -- see ui_wait_key. */
void editor_process_key(int c) {
  int shift = input_last_key_had_shift();
  int pre_cx = CUR.cx, pre_cy = CUR.cy;

  /* A popup over the text closes on the next key, which then goes on to do
   * what it does -- unless it was Esc, which was only for the popup. */
  if (lspui_key(c)) return;

  /* A mouse event carries coordinates, so it cannot be a chord the keymap binds to
   * a command: it goes to whichever layer is on top where it landed. It also must
   * not fall through to the selection handling below, which would clear the
   * selection a drag has just been building. */
  if (c == MOUSE_KEY) {
    struct mouse_event ev;
    input_last_mouse(&ev);
    layer_dispatch_mouse(&ev);
    return;
  }

  /* A key that arrived with modifiers gets one chance at a binding of its own
   * before falling back to the bare key. Nothing binds "shift-right", so
   * Shift-Right lands on move.right with `shift` still set and extends the
   * selection below; "shift-f3" and "ctrl-pagedown" are bound, so neither
   * reaches the unmodified key's command. */
  int mods = (shift ? KEY_SHIFT : 0) |
             (input_last_key_had_ctrl() ? KEY_CTRL : 0) |
             (input_last_key_had_alt() ? KEY_ALT : 0);
  const struct command *cmd = NULL;
  if (mods) cmd = keymap_lookup(c | mods);
  if (cmd == NULL) cmd = keymap_lookup(c);
  int sel_move = (cmd != NULL && (cmd->flags & CMD_MOVEMENT));
  int res = 0;

  if (cmd != NULL) {
    res = cmd->run();
  } else if (c == '\t' || (c < 256 && !iscntrl(c))) {
    /* Unbound and printable: it is text. Tab is a control character, so testing
     * iscntrl() alone threw it away and a tab could not be typed at all; key
     * codes of 1000 and up are outside iscntrl()'s domain and must not reach it. */
    /* Through cursors_edit(), which takes each cursor's selection first and
     * lands the character at every one of them as a single undo step. With one
     * cursor it is the ordinary insert and nothing else.
     *
     * Tab goes to indent.c rather than being inserted as a character: whether a
     * step is a tab or spaces to the next stop is one decision. Tab is bound to
     * edit.indent, so this is the path only when that binding has been taken
     * away -- and then a typed tab still indents rather than becoming a
     * character nobody chose. */
    cursors_edit(c == '\t' ? MC_INDENT : MC_INSERT, c);
  }
  /* Anything else -- Esc, an unbound function or Alt key -- does nothing, but
   * still falls through to the selection and countdown handling below, which is
   * what Esc clearing a selection depends on. */

  /* A shifted movement extends the selection (seeding the anchor on the first
   * one from a resting cursor). Copying keeps the selection; every other plain
   * move, edit, or command drops it. Edits that need the selected text first
   * consume it via selection_delete(). */
  if (sel_move && shift) {
    if (!CUR.sel_active) {
      CUR.sx = pre_cx;
      CUR.sy = pre_cy;
      CUR.sel_active = 1;
    }
  } else if (!(res & CMD_KEEP_SELECTION)) {
    selection_clear();
  }

  if (!(res & CMD_KEEP_QUIT_COUNT)) {
    close_warned_id = -1;
  }

  /* A completion list follows the word it is completing; a typed trigger
   * character asks for one. After the key, because both are about what the
   * key did to the text. */
  lspui_after_key(c);
}

/* ---- the help screen's data ---------------------------------------------- */

/* The help screen's layout. Grouping and wording are editorial, so they stay
 * written down; the key column and the description of anything that *is* a
 * command come from the table above, so a rebinding or a new command cannot
 * leave the help screen stale. Entries with a literal key cover several keys at
 * once, or behavior that is not a command at all. */
static const struct help_entry help_entries[] = {
  {"FILES", NULL, NULL, "file.save"},
  {NULL,    NULL, NULL, "file.open"},
  {NULL,    NULL, NULL, "file.new"},
  {NULL,    NULL, NULL, "file.close"},
  {NULL,    NULL, NULL, "file.quit"},
  {NULL,    NULL, NULL, "view.help"},

  {"BUFFERS", NULL, NULL, "buffer.next"},
  {NULL,      NULL, NULL, "buffer.prev"},

  {"MENUS",      "F10",       "Open the menu bar", NULL},
  {NULL,         "Alt-letter", "Open that menu",   NULL},
  {NULL,         NULL,        NULL,               "app.palette"},
  {NULL,         "Shift+drag", "Select with the terminal, not molly", NULL},

  {"MOVEMENT",   "Arrows",    "Move cursor",     NULL},
  {NULL,         "PgUp/PgDn", "Page up / down",  NULL},
  {NULL,         "Home/End",  "Line start / end", NULL},
  {NULL,         NULL,        NULL,              "goto.line"},

  {"SELECTION",  "Shift+Arrows",   "Extend selection", NULL},
  {NULL,         "Shift+Home/End", "To line edge",     NULL},
  {NULL,         "Shift+PgUp/Dn",  "Extend by page",   NULL},
  {NULL,         NULL,             NULL,               "edit.select-all"},
  {NULL,         NULL,             NULL,               "edit.select-line"},

  /* The cursor commands, which MANUAL.md has claimed were here since they
   * landed and were not: they went into the table and the palette and this
   * list was not touched. The screen scrolls and HELP_MAXROWS has room, so the
   * fix is the group rather than the sentence. */
  {"CURSORS",    NULL, NULL, "cursor.add-next"},
  {NULL,         NULL, NULL, "cursor.skip-next"},
  {NULL,         NULL, NULL, "cursor.add-all"},
  {NULL,         NULL, NULL, "cursor.drop-last"},
  {NULL,         NULL, NULL, "cursor.add-below"},
  {NULL,         NULL, NULL, "cursor.add-above"},
  {NULL,         NULL, NULL, "cursor.one"},
  /* Fourteen columns, which is the key column's width exactly. */
  {NULL,         "Alt-Shift-\xe2\x86\x90\xe2\x86\x91\xe2\x86\x92\xe2\x86\x93",
   "Block selection", NULL},
  {NULL,         "Alt+drag",         "Block selection", NULL},

  {"EDITING",    NULL,          NULL,           "edit.newline"},
  {NULL,         NULL,          NULL,           "edit.indent"},
  {NULL,         NULL,          NULL,           "edit.dedent"},
  {NULL,         "Bksp/Ctrl-H", "Delete before", NULL},
  {NULL,         NULL,          NULL,           "edit.delete-forward"},
  {NULL,         NULL,          NULL,           "edit.transpose"},
  {NULL,         NULL,          NULL,           "edit.filter"},
  {NULL,         NULL,          NULL,           "edit.insert-output"},

  {"FIND & REPLACE", NULL, NULL, "search.find"},
  {NULL,             NULL, NULL, "search.next"},
  {NULL,             NULL, NULL, "search.prev"},
  /* Match case is not here any more. It lost its chord to transpose, and it is a
   * checkbox in the Find dialog now -- so on a screen whose left column is a key,
   * it rendered as a blank key beside a description, which reads as a bug. It is
   * still in the Search menu and in the palette. */
  {NULL,             NULL, NULL, "search.regex"},
  {NULL,             NULL, NULL, "search.replace"},
  {NULL,             NULL, NULL, "search.in-files"},
  {NULL,             NULL, NULL, "search.next-result"},
  {NULL,             NULL, NULL, "search.prev-result"},

  {"CLIPBOARD", NULL, NULL, "edit.copy"},
  {NULL,        NULL, NULL, "edit.cut"},
  {NULL,        NULL, NULL, "edit.paste"},

  {"HISTORY",   NULL, NULL, "edit.undo"},
  {NULL,        NULL, NULL, "edit.redo"},

  {"WINDOWS",   NULL, NULL, "window.split-right"},
  {NULL,        NULL, NULL, "window.split-below"},
  {NULL,        NULL, NULL, "window.next"},
  {NULL,        NULL, NULL, "window.prev"},
  {NULL,        NULL, NULL, "window.close"},
  /* Two literal rows rather than the four commands: "Ctrl-Shift-Right" is
   * sixteen columns and the key column is fourteen, so the four came out as
   * "Ctrl-Shift-Rig" and "Ctrl-Shift-Dow" -- the one part of the row that is
   * the information, cut. Literal like Shift+Arrows above, and with the same
   * cost: a rebinding in mollyrc does not reach these two lines. */
  {NULL,        "Ctrl-Shift-\xe2\x86\x90\xe2\x86\x92", "Border left/right", NULL},
  {NULL,        "Ctrl-Shift-\xe2\x86\x91\xe2\x86\x93", "Border up/down",    NULL},
  /* Only this window is not here: it has no chord, and the left column of this
   * screen is a key. It is in the Window menu and in the palette. */

  {"VIEW",      NULL, NULL, "view.repaint"},
  {NULL,        NULL, NULL, "view.line-numbers"},
  {NULL,        NULL, NULL, "view.menu-bar"},
  {NULL,        NULL, NULL, "view.tab-bar"},
  {NULL,        NULL, NULL, "view.syntax"},
  {NULL,        NULL, NULL, "view.soft-wrap"},

  /* Last, so that adding it moved nothing above it: the soft-wrap check finds
   * VIEW by paging down to it. */
  {"CODE",      NULL, NULL, "goto.next-problem"},
  {NULL,        NULL, NULL, "goto.prev-problem"},
  {NULL,        NULL, NULL, "goto.definition"},
  {NULL,        NULL, NULL, "goto.back"},
  {NULL,        NULL, NULL, "code.hover"},
  {NULL,        NULL, NULL, "code.complete"},
};

int help_entry_count(void) {
  return (int)(sizeof(help_entries) / sizeof(help_entries[0]));
}

/* Resolve one help line: its group heading (NULL to continue the current group),
 * and the key and description to print. */
void help_entry_at(int i, const char **group, char *key, size_t keysz,
                   const char **desc) {
  const struct help_entry *e = &help_entries[i];
  *group = e->group;
  if (e->cmd != NULL) {
    const struct command *c = cmd_find_by_name(e->cmd);
    cmd_shortcut(e->cmd, key, keysz);
    *desc = (c != NULL && c->help != NULL) ? c->help : "";
  } else {
    snprintf(key, keysz, "%s", e->key ? e->key : "");
    *desc = e->desc ? e->desc : "";
  }
}
