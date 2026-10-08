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
static int quit_times = KILO_QUIT_TIMES;

static int cmd_newline(void) {
  if (CUR.sel_active) selection_delete();
  editor_insert_newline();
  return 0;
}

static int cmd_quit(void) {
  if (B.dirty && quit_times > 0) {
    editor_set_status_message(
        "WARNING!!! File has unsaved changes. Press Ctrl-Q %d more times to quit.",
        quit_times);
    quit_times--;
    /* The countdown must survive, and so must any selection: this keypress is a
     * warning rather than an action. */
    return CMD_KEEP_SELECTION | CMD_KEEP_QUIT_COUNT;
  }
  write_all(STDOUT_FILENO, "\x1b[2J", 4);
  write_all(STDOUT_FILENO, "\x1b[H", 3);
  if (B.recovery_path[0] != '\0') unlink(B.recovery_path);
  exit(0);
}

static int cmd_save(void) {
  editor_save();
  return 0;
}

static int cmd_open(void) {
  /* One buffer, one file: the open dialog replaces the buffer, so it is refused
   * while there is unsaved work rather than silently discarding it. */
  if (B.dirty) {
    editor_set_status_message(
        "Open aborted: file has unsaved changes (Ctrl-S first)");
    return 0;
  }
  /* No repaint needed on the way out: the dialog is a layer, so popping it is
   * enough and the grid works out what to redraw. */
  char *path = editor_open_dialog();
  if (path != NULL) editor_switch_file(path);
  return 0;
}

static int cmd_undo(void) { editor_undo(); return 0; }
static int cmd_redo(void) { editor_redo(); return 0; }

static int cmd_copy(void) {
  /* Copy keeps the selection; everything else drops it. */
  if (CUR.sel_active) {
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
  /* Copy then delete the selection, as one undo step. */
  if (CUR.sel_active) {
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

static int cmd_paste(void) { editor_paste(); return 0; }

static int cmd_find(void) { editor_find(); return 0; }
static int cmd_find_next(void) { editor_find_next(); return 0; }
static int cmd_find_prev(void) { editor_find_prev(); return 0; }
static int cmd_case_toggle(void) { editor_toggle_case_sensitive(); return 0; }
static int cmd_regex_toggle(void) { editor_toggle_regex(); return 0; }
static int cmd_replace(void) { editor_replace(); return 0; }
static int cmd_goto_line(void) { editor_goto_line(); return 0; }
static int cmd_help(void) { editor_help(); return 0; }

static int cmd_line_start(void) {
  CUR.cx = 0;
  return 0;
}

static int cmd_line_end(void) {
  if (CUR.cy < row_count()) CUR.cx = row_at(CUR.cy)->size;
  return 0;
}

static int cmd_delete_back(void) {
  if (CUR.sel_active) selection_delete();
  else editor_delete_char();
  return 0;
}

static int cmd_delete_forward(void) {
  if (CUR.sel_active) selection_delete();
  else editor_del_char();
  return 0;
}

/* A page is the text area's height, moved one display row at a time so the
 * movement follows whatever the view model says a row is. */
static int cmd_page_up(void) {
  int times = V.screenrows;
  while (times--) editor_move_cursor(ARROW_UP);
  return 0;
}

static int cmd_page_down(void) {
  int times = V.screenrows;
  while (times--) editor_move_cursor(ARROW_DOWN);
  return 0;
}

static int cmd_left(void)  { editor_move_cursor(ARROW_LEFT);  return 0; }
static int cmd_right(void) { editor_move_cursor(ARROW_RIGHT); return 0; }
static int cmd_up(void)    { editor_move_cursor(ARROW_UP);    return 0; }
static int cmd_down(void)  { editor_move_cursor(ARROW_DOWN);  return 0; }

static int cmd_repaint(void) {
  screen_force_repaint();
  return 0;
}

/* The menu bar costs a row, so whether it is worth that row is the user's call. It
 * is a command before it is a setting: configuration arrives in Phase 2, and this
 * reads its default from there when it does. */
static int cmd_menu_bar(void) {
  V.menubar = !V.menubar;
  /* The text area moves by a row, so every column of every row shifts. */
  screen_force_repaint();
  editor_set_status_message("Menu bar: %s", V.menubar ? "on" : "off");
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
static int cmd_menu_help(void)   { return menu_open(5); }

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
static int linenumbers_on(void) { return V.linenum; }
static int menubar_on(void) { return V.menubar; }

/* The table. Order matters only for the help screen, which walks it in order
 * within each group. `label` is for the menu bar and the palette; `help` and
 * `group` are what the help screen shows, and a command with no group is not
 * listed there. */
static const struct command commands[] = {
  {.name = "file.save", .label = "Save",
   .help = "Save file", .group = "FILES", .run = cmd_save},
  {.name = "file.open", .label = "Open…",
   .help = "Open a file", .group = "FILES", .run = cmd_open},
  {.name = "file.quit", .label = "Quit",
   .help = "Quit", .group = "FILES", .run = cmd_quit},
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

  {.name = "edit.newline", .label = "Insert line",
   .help = "Insert new line", .group = "EDITING", .run = cmd_newline},
  {.name = "edit.delete-forward", .label = "Delete",
   .help = "Delete at cursor", .group = "EDITING", .run = cmd_delete_forward},
  {.name = "edit.delete-back", .label = "Backspace", .run = cmd_delete_back},

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

/* Whether Shift on this key reaches molly as something of its own.
 *
 * The CSI form carries a modifier parameter for the function keys and the
 * navigation keys ("\x1b[1;2C" is Shift-Right), so those can be bound shifted. A
 * letter cannot: Shift-A *is* 'A'. Nor can Ctrl plus Shift, because Ctrl-L and
 * Ctrl-Shift-L are the same byte on the wire -- which is why the roadmap's
 * suggestion of moving repaint to Ctrl-Shift-L could not be taken. */
static int shift_is_distinguishable(int key) {
  switch (key) {
    case ARROW_LEFT: case ARROW_RIGHT: case ARROW_UP: case ARROW_DOWN:
    case HOME_KEY: case END_KEY: case PAGE_UP: case PAGE_DOWN: case DEL_KEY:
      return 1;
    default:
      return key >= F1_KEY && key <= F12_KEY;
  }
}

/* Parse a chord: "ctrl-s", "alt-x", "shift-f3", "f10", "left", "enter", or a
 * bare character. Returns the key code -- with KEY_SHIFT set if the chord named
 * Shift -- or -1 if it means nothing.
 *
 * Case-insensitive, and the modifiers may be written in any order, so a config
 * file may say "Ctrl-S", "ctrl-s" or "shift-ctrl-f3". */
int chord_parse(const char *s) {
  int i;
  int ctrl = 0, alt = 0, shift = 0;
  int key;

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
    unsigned char c = (unsigned char)toupper((unsigned char)s[0]);
    if (s[1] != '\0' || c < 'A' || c > 'Z') return -1;
    key = CTRL_KEY(c);
  }
  if (alt) {
    if (s[1] != '\0' || (unsigned char)s[0] < 0x20 || (unsigned char)s[0] > 0x7e)
      return -1;
    key = ALT_KEY(s[0]);
  }
  if (shift) {
    if (!shift_is_distinguishable(key)) return -1;
    key |= KEY_SHIFT;
  }
  return key;
}

/* The display form of a chord: "Ctrl-S", "Alt-X", "F10", "PgUp". This is what the
 * help screen prints and what the menu bar will print down its right-hand side. */
void chord_name(int key, char *dst, size_t n) {
  int i;

  /* Shift is a prefix on whatever the rest of the key is called, so it is peeled
   * off and the bare key named by the code below -- which is also what keeps
   * "Shift-" out of every branch of it. */
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
 * command: Backspace and Ctrl-H both delete backwards.
 *
 * These are olly's bindings unchanged. The CUA moves the roadmap settles on --
 * Ctrl-L for select-line, Ctrl-T for transpose, match-case as a checkbox in the
 * Find dialog -- are feature work, not Phase 0. */
struct binding { const char *chord; const char *cmd; };

static const struct binding default_bindings[] = {
  {"ctrl-s", "file.save"},
  {"ctrl-o", "file.open"},
  {"ctrl-q", "file.quit"},
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

  {"ctrl-f", "search.find"},
  {"ctrl-n", "search.next"},
  {"ctrl-p", "search.prev"},
  {"ctrl-t", "search.case"},
  {"alt-r",  "search.regex"},
  {"ctrl-r", "search.replace"},
  {"ctrl-g", "goto.line"},

  {"left",   "move.left"},
  {"right",  "move.right"},
  {"up",     "move.up"},
  {"down",   "move.down"},
  {"home",   "move.line-start"},
  {"end",    "move.line-end"},
  {"pageup", "move.page-up"},
  {"pagedown", "move.page-down"},

  {"ctrl-l", "view.repaint"},
  {"ctrl-u", "view.line-numbers"},

  /* The palette, on two chords. Ctrl-Space is the one the roadmap settles on, and
   * Alt-X is there because not every terminal sends NUL for Ctrl-Space -- and because
   * Alt-X has meant "run a command by name" for forty years. X is not one of the
   * eight letters the menu bar reserves. */
  {"ctrl-space", "app.palette"},
  {"alt-x",      "app.palette"},

  /* The menu bar: F10 as every keyboard-driven menu bar has had it, plus the eight
   * reserved Alt letters -- six of which have a menu today. */
  {"f10",    "menu.open"},
  {"alt-f",  "menu.file"},
  {"alt-e",  "menu.edit"},
  {"alt-s",  "menu.search"},
  {"alt-v",  "menu.view"},
  {"alt-g",  "menu.go"},
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

  /* A shifted key gets one chance at a binding of its own before falling back to
   * the unshifted one. Nothing binds "shift-right", so Shift-Right lands on
   * move.right with `shift` still set and extends the selection below; "shift-f3"
   * is bound, so Shift-F3 never reaches search.next. */
  const struct command *cmd = NULL;
  if (shift) cmd = keymap_lookup(c | KEY_SHIFT);
  if (cmd == NULL) cmd = keymap_lookup(c);
  int sel_move = (cmd != NULL && (cmd->flags & CMD_MOVEMENT));
  int res = 0;

  if (cmd != NULL) {
    res = cmd->run();
  } else if (c == '\t' || (c < 256 && !iscntrl(c))) {
    /* Unbound and printable: it is text. Tab is a control character, so testing
     * iscntrl() alone threw it away and a tab could not be typed at all; key
     * codes of 1000 and up are outside iscntrl()'s domain and must not reach it. */
    if (CUR.sel_active) selection_delete();
    editor_insert_char(c);
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

  if (!(res & CMD_KEEP_QUIT_COUNT)) quit_times = KILO_QUIT_TIMES;
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
  {NULL,    NULL, NULL, "file.quit"},
  {NULL,    NULL, NULL, "view.help"},

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

  {"EDITING",    NULL,          NULL,           "edit.newline"},
  {NULL,         "Tab",         "Insert tab",   NULL},
  {NULL,         "Bksp/Ctrl-H", "Delete before", NULL},
  {NULL,         NULL,          NULL,           "edit.delete-forward"},

  {"FIND & REPLACE", NULL, NULL, "search.find"},
  {NULL,             NULL, NULL, "search.next"},
  {NULL,             NULL, NULL, "search.prev"},
  {NULL,             NULL, NULL, "search.case"},
  {NULL,             NULL, NULL, "search.regex"},
  {NULL,             NULL, NULL, "search.replace"},

  {"CLIPBOARD", NULL, NULL, "edit.copy"},
  {NULL,        NULL, NULL, "edit.cut"},
  {NULL,        NULL, NULL, "edit.paste"},

  {"HISTORY",   NULL, NULL, "edit.undo"},
  {NULL,        NULL, NULL, "edit.redo"},

  {"VIEW",      NULL, NULL, "view.repaint"},
  {NULL,        NULL, NULL, "view.line-numbers"},
  {NULL,        NULL, NULL, "view.menu-bar"},
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
