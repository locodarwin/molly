/* menu.c -- the menu bar and its drop-downs.
 *
 * Built from the command table, so a menu entry is a command *name* and nothing is
 * written down twice: the label, the shortcut shown down the right-hand side, whether
 * the item is available and whether it is ticked all come from the command. Rebind a
 * key and the menu follows; add a command and it has a menu entry the moment it is
 * listed here.
 *
 * Only commands that exist are listed, which is why there are six menus rather than
 * eight: Tools and Options have nothing in them until Phase 2, and a menu that opens
 * onto nothing is worse than a menu that is not there. Their Alt letters stay
 * reserved regardless, so adding them later costs nothing.
 *
 * Turbo Vision's structure in modern colors, as decided: a band a shade darker than
 * a dialog, drop-downs with a single-line border -- against the dialogs' double line,
 * which is the distinction Borland's IDE made -- and a drop shadow that restyles the
 * text underneath rather than painting over it.
 *
 * Everything is measured in display columns rather than bytes. "Find…" is seven
 * bytes and five columns; measuring it with strlen makes the box two columns too wide
 * and misaligns a mnemonic that sits after a multibyte character. */

#include "molly.h"

/* An item is a command name, or NULL for a separator. `mnemonic` is the display
 * column of the underlined letter within the label, -1 for none. */
struct menu_item {
  const char *cmd;
  int mnemonic;
};

struct menu {
  const char *title;
  int mnemonic;        /* column within the title */
  const struct menu_item *items;
  int nitems;
};

#define SEP {NULL, -1}

/* Mnemonics are the obvious first letter wherever that is unambiguous within the
 * menu, and the conventional alternative where it is not: Cut takes its 't' because
 * Copy has the 'C', exactly as every CUA menu since Windows 3 has done. */
static const struct menu_item file_items[] = {
  {"file.new", 0}, {"file.open", 0}, {"file.save", 0},
  SEP, {"file.close", 0}, SEP, {"file.quit", 0},
};
static const struct menu_item edit_items[] = {
  {"edit.undo", 0}, {"edit.redo", 0}, SEP,
  {"edit.cut", 2}, {"edit.copy", 0}, {"edit.paste", 0}, SEP,
  /* S and L, which are the two letters this menu has not already spent: Undo
   * has U, Redo R, Cut its 't', Copy C and Paste P. Transpose is not here on
   * purpose -- it is a keyboard operation, and it is in the palette. */
  {"edit.select-all", 0}, {"edit.select-line", 7},
};
static const struct menu_item search_items[] = {
  {"search.find", 0}, {"search.next", 5}, {"search.prev", 5},
  {"search.case", 6}, {"search.regex", 2}, SEP, {"search.replace", 0},
};
static const struct menu_item view_items[] = {
  {"app.palette", 0}, SEP,
  {"view.line-numbers", 0}, {"view.menu-bar", 0}, SEP, {"view.repaint", 0},
  SEP, {"app.save-settings", 0},
};
static const struct menu_item go_items[] = {
  {"goto.line", 0}, SEP,
  /* N and P, since Go has nothing else competing for either. */
  {"buffer.next", 0}, {"buffer.prev", 0},
};
static const struct menu_item help_items[] = {
  {"view.help", 0},
};

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const struct menu menus[] = {
  {"File",   0, file_items,   COUNT(file_items)},
  {"Edit",   0, edit_items,   COUNT(edit_items)},
  {"Search", 0, search_items, COUNT(search_items)},
  {"View",   0, view_items,   COUNT(view_items)},
  {"Go",     0, go_items,     COUNT(go_items)},
  {"Help",   0, help_items,   COUNT(help_items)},
};

#define NMENUS COUNT(menus)

/* Columns before the first title, and between titles. */
#define BAR_PAD 1
#define TITLE_GAP 1

/* Two columns at the left of every item for a tick, so a menu's labels line up
 * whether or not anything in it is checkable. */
#define CHECK_COLS 2
/* Columns between the longest label and the shortcut column. */
#define ACCEL_GAP 3

/* Which menu is open, -1 for none, and which item is highlighted in it. Read by the
 * bar's draw function, which has to know which title to light up. */
static int open_menu = -1;
static int open_item = 0;

int menu_is_open(void) {
  return open_menu >= 0;
}

/* Where each title starts, filled in as the bar draws and read by its mouse
 * handler, so the two cannot disagree about what was clicked. */
static int title_col[NMENUS];
static int title_cols[NMENUS];

static const struct command *item_cmd(const struct menu *m, int i) {
  if (m->items[i].cmd == NULL) return NULL;
  return cmd_find_by_name(m->items[i].cmd);
}

static int item_is_separator(const struct menu *m, int i) {
  return m->items[i].cmd == NULL;
}

static int item_is_enabled(const struct menu *m, int i) {
  const struct command *c = item_cmd(m, i);
  if (c == NULL) return 0;
  return c->enabled == NULL || c->enabled();
}

/* --- the band ------------------------------------------------------------- */

void menu_bar_draw(const struct layer *self) {
  int i, x = self->x + BAR_PAD;

  grid_role(ROLE_MENUBAR);
  grid_fill(self->y, self->x, self->w, ' ');

  for (i = 0; i < NMENUS; i++) {
    int cols = utf8_str_cols(menus[i].title);
    int selected = (i == open_menu);
    title_col[i] = x;
    title_cols[i] = cols;
    grid_role(selected ? ROLE_MENUBAR_SELECTED : ROLE_MENUBAR);
    /* A column of padding each side, so the highlight is a block around the word
     * rather than a tight box against the letters. */
    grid_fill(self->y, x - 1, cols + 2, ' ');
    grid_puts(self->y, x, menus[i].title, (int)strlen(menus[i].title));
    if (menus[i].mnemonic >= 0) {
      int off = utf8_col_to_byte(menus[i].title, menus[i].mnemonic);
      int nb;
      (void)utf8_char_cols(menus[i].title, (int)strlen(menus[i].title), off, &nb);
      grid_attrs(ATTR_UNDERLINE);
      grid_puts(self->y, x + menus[i].mnemonic, menus[i].title + off, nb);
      grid_attrs(0);
    }
    x += cols + 1 + TITLE_GAP;
  }
  grid_role(ROLE_TEXT);
}

/* A click on a title opens that menu. While a menu is open its own loop owns the
 * mouse, so this only ever runs from the editor's dispatcher -- which is why it can
 * call menu_open and block until the menu closes. */
int menu_bar_mouse(const struct layer *self, const struct mouse_event *ev) {
  int i;
  (void)self;
  if (ev->type != MOUSE_PRESS || ev->button != MOUSE_BUTTON_LEFT) return 1;
  for (i = 0; i < NMENUS; i++) {
    if (ev->x >= title_col[i] - 1 && ev->x <= title_col[i] + title_cols[i]) {
      menu_open(i);
      return 1;
    }
  }
  return 1;  /* a click on the band itself does nothing, but is not the text's */
}

/* --- a drop-down ---------------------------------------------------------- */

/* Widest label and widest shortcut in a menu, in columns, which together give the
 * box its width. */
static void menu_measure(const struct menu *m, int *label_w, int *accel_w) {
  int i;
  *label_w = 0;
  *accel_w = 0;
  for (i = 0; i < m->nitems; i++) {
    const struct command *c = item_cmd(m, i);
    char accel[32];
    int w;
    if (c == NULL) continue;
    w = utf8_str_cols(c->label != NULL ? c->label : c->name);
    if (w > *label_w) *label_w = w;
    if (cmd_shortcut(c->name, accel, sizeof(accel))) {
      w = utf8_str_cols(accel);
      if (w > *accel_w) *accel_w = w;
    }
  }
}

static int menu_width(const struct menu *m) {
  int label_w, accel_w;
  menu_measure(m, &label_w, &accel_w);
  /* border, check column, label, gap, shortcut, border */
  return 1 + CHECK_COLS + label_w + (accel_w ? ACCEL_GAP + accel_w : 0) + 1 + 1;
}

/* How a drop-down's interior divides up at a given box width.
 *
 * At the natural width from menu_width() everything fits and this is the
 * identity. A terminal narrower than that is where it used to go wrong: the box
 * was clamped to the screen, the label was still drawn in full, and the shortcut
 * was then painted over its tail -- at 24 columns the View menu's first item read
 * "Command pCtrl-Space". So the squeeze is a decision rather than an accident,
 * and it comes off the *shortcut* first: the label says what the item is, while
 * the shortcut is a reminder of a key that is also on the help screen and in the
 * palette. Below MENU_MIN_LABEL columns of label there is nothing worth reading
 * left, so the shortcut column goes entirely and the label takes the whole
 * interior.
 *
 * The decision is made once per menu rather than per item, so that what
 * shortcuts are shown still line up down the right-hand side.
 *
 * A truncated label is cut without a marker. An ellipsis would be the obvious
 * thing and is the wrong thing here: molly already spends "…" on "this item
 * opens a dialog" ("Find…", "Open…"), so "Line num…" would promise a dialog
 * that is not coming. */
struct dd_cols {
  int label;   /* columns the label may occupy */
  int accel;   /* widest shortcut to show, or 0 to show none */
};

#define MENU_MIN_LABEL 6

static void dropdown_cols(const struct menu *m, int w, struct dd_cols *out) {
  int label_w, accel_w;
  /* Border, check column, [this], padding, border -- which is the same division
   * menu_width() makes in the other direction, so at the natural width `room`
   * below comes out as exactly label_w. The padding column matters: without it
   * a squeezed label ran up against the right-hand border, which looks like the
   * text has broken out of the box. */
  int interior = w - 3 - CHECK_COLS;
  menu_measure(m, &label_w, &accel_w);
  if (interior < 1) interior = 1;
  (void)label_w;

  if (accel_w > 0) {
    int room = interior - ACCEL_GAP - accel_w;
    if (room >= MENU_MIN_LABEL) {
      out->label = room;
      out->accel = accel_w;
      return;
    }
  }
  out->label = interior;
  out->accel = 0;
}

#define BOX_TL "\xe2\x94\x8c"
#define BOX_TR "\xe2\x94\x90"
#define BOX_BL "\xe2\x94\x94"
#define BOX_BR "\xe2\x94\x98"
#define BOX_H  "\xe2\x94\x80"
#define BOX_V  "\xe2\x94\x82"
#define BOX_LT "\xe2\x94\x9c"
#define BOX_RT "\xe2\x94\xa4"
#define TICK   "\xe2\x9c\x93"   /* U+2713 */

static void dropdown_draw(const struct layer *self) {
  const struct menu *m = &menus[open_menu];
  int x = self->x, y = self->y, w = self->w, h = self->h;
  struct dd_cols cols;
  int i;

  dropdown_cols(m, w, &cols);

  /* The shadow first, so the frame covers the part that overlaps it. */
  grid_shade(x + w, y + 1, 2, h, ROLE_DIALOG_SHADOW);
  grid_shade(x + 2, y + h, w, 1, ROLE_DIALOG_SHADOW);

  grid_role(ROLE_MENU);
  for (i = 0; i < h; i++) grid_fill(y + i, x, w, ' ');

  grid_role(ROLE_MENU_FRAME);
  grid_puts(y, x, BOX_TL, 3);
  grid_puts(y, x + w - 1, BOX_TR, 3);
  grid_puts(y + h - 1, x, BOX_BL, 3);
  grid_puts(y + h - 1, x + w - 1, BOX_BR, 3);
  for (i = 1; i < w - 1; i++) {
    grid_puts(y, x + i, BOX_H, 3);
    grid_puts(y + h - 1, x + i, BOX_H, 3);
  }
  for (i = 1; i < h - 1; i++) {
    grid_puts(y + i, x, BOX_V, 3);
    grid_puts(y + i, x + w - 1, BOX_V, 3);
  }

  for (i = 0; i < m->nitems; i++) {
    int row = y + 1 + i;
    const struct command *c;
    unsigned char role;
    int enabled, checked;
    char accel[32];

    if (item_is_separator(m, i)) {
      int k;
      grid_role(ROLE_MENU_FRAME);
      grid_puts(row, x, BOX_LT, 3);
      for (k = 1; k < w - 1; k++) grid_puts(row, x + k, BOX_H, 3);
      grid_puts(row, x + w - 1, BOX_RT, 3);
      continue;
    }

    c = item_cmd(m, i);
    if (c == NULL) continue;
    enabled = item_is_enabled(m, i);
    checked = (c->checked != NULL && c->checked());
    role = (i == open_item) ? ROLE_MENU_SELECTED
                            : (enabled ? ROLE_MENU : ROLE_MENU_DISABLED);

    grid_role(role);
    grid_fill(row, x + 1, w - 2, ' ');
    if (checked) grid_puts(row, x + 1, TICK, 3);
    {
      const char *label = c->label != NULL ? c->label : c->name;
      int shown_cols;
      int shown = utf8_fit_cols(label, cols.label, &shown_cols);
      grid_puts(row, x + 1 + CHECK_COLS, label, shown);
      /* The mnemonic is the item's own colors plus an underline, which is what the
       * cell modifier byte is for -- a role per container-and-state would otherwise
       * multiply out. A disabled item gets no underline: there is nothing to press.
       * Nor does a letter the truncation above cut off: underlining a column the
       * label no longer reaches would mark whatever the shortcut put there. */
      if (m->items[i].mnemonic >= 0 && enabled &&
          m->items[i].mnemonic < shown_cols) {
        int off = utf8_col_to_byte(label, m->items[i].mnemonic);
        int nb;
        (void)utf8_char_cols(label, (int)strlen(label), off, &nb);
        grid_attrs(ATTR_UNDERLINE);
        grid_puts(row, x + 1 + CHECK_COLS + m->items[i].mnemonic, label + off, nb);
        grid_attrs(0);
      }
    }
    if (cols.accel && cmd_shortcut(c->name, accel, sizeof(accel))) {
      int aw = utf8_str_cols(accel);
      grid_role(role);
      /* One column of padding before the border, so the shortcut does not look
       * wedged against it. */
      grid_puts(row, x + w - 2 - aw, accel, (int)strlen(accel));
    }
  }
  grid_role(ROLE_TEXT);
  grid_cursor_hide();
}

/* --- navigating ----------------------------------------------------------- */

/* The next item that can actually be chosen, stepping by `dir` and skipping
 * separators and anything unavailable. Stops at the ends rather than wrapping: a
 * menu that jumps from its last item back to its first loses the user's place more
 * often than it helps. Returns the item index, or the one it started from. */
static int next_selectable(const struct menu *m, int from, int dir) {
  int i = from;
  for (;;) {
    i += dir;
    if (i < 0 || i >= m->nitems) return from;
    if (!item_is_separator(m, i) && item_is_enabled(m, i)) return i;
  }
}

static int first_selectable(const struct menu *m) {
  int i;
  for (i = 0; i < m->nitems; i++)
    if (!item_is_separator(m, i) && item_is_enabled(m, i)) return i;
  return 0;
}

static int last_selectable(const struct menu *m) {
  int i;
  for (i = m->nitems - 1; i >= 0; i--)
    if (!item_is_separator(m, i) && item_is_enabled(m, i)) return i;
  return 0;
}

/* The item whose mnemonic is `ch`, or -1. Case-insensitive, and only among items
 * that can be chosen. */
static int item_for_mnemonic(const struct menu *m, int ch) {
  int i;
  for (i = 0; i < m->nitems; i++) {
    const struct command *c;
    const char *label;
    int off;
    if (item_is_separator(m, i) || !item_is_enabled(m, i)) continue;
    if (m->items[i].mnemonic < 0) continue;
    c = item_cmd(m, i);
    label = (c->label != NULL) ? c->label : c->name;
    off = utf8_col_to_byte(label, m->items[i].mnemonic);
    if (tolower((unsigned char)label[off]) == tolower(ch)) return i;
  }
  return -1;
}

/* How many menus the bar has, and the character each one's Alt shortcut uses.
 * Read by cmd.c, which refuses to let the configuration file rebind them: the bar
 * underlines the letter whether or not it still opens the menu, so a rebinding
 * would make the menu bar lie. Returns 0 for a menu with no mnemonic. */
int menu_count(void) {
  return COUNT(menus);
}

int menu_mnemonic_char(int i) {
  int off;
  if (i < 0 || i >= COUNT(menus) || menus[i].mnemonic < 0) return 0;
  off = utf8_col_to_byte(menus[i].title, menus[i].mnemonic);
  return (unsigned char)menus[i].title[off];
}

/* The menu whose title mnemonic is `ch`, or -1. */
int menu_for_mnemonic(int ch) {
  int i;
  for (i = 0; i < NMENUS; i++) {
    int off;
    if (menus[i].mnemonic < 0) continue;
    off = utf8_col_to_byte(menus[i].title, menus[i].mnemonic);
    if (tolower((unsigned char)menus[i].title[off]) == tolower(ch)) return i;
  }
  return -1;
}

/* Lay the drop-down out under its title, kept on screen at the right-hand edge. */
static void dropdown_layout(struct layer *l) {
  const struct menu *m = &menus[open_menu];
  int w = menu_width(m);
  int h = m->nitems + 2;
  int bar_y = V.menubar ? 0 : 0;   /* the band's row either way; see menu_open */

  if (w > grid_cols()) w = grid_cols();
  l->x = title_col[open_menu] - 1;
  if (l->x + w > grid_cols()) l->x = grid_cols() - w;
  if (l->x < 0) l->x = 0;
  l->y = bar_y + 1;
  if (l->y + h > grid_rows()) h = grid_rows() - l->y;
  if (h < 3) h = 3;
  l->w = w;
  l->h = h;
  l->draw = dropdown_draw;
  l->mouse = NULL;   /* the menu's own loop reads the mouse */
}

/* Open the menu bar at `index` and run whatever is chosen.
 *
 * Returns the dispatcher flags of the command that ran, so a menu choice behaves
 * exactly as the same command does from its key -- including Ctrl-Q's countdown and
 * copy keeping the selection. The chosen command runs *after* the menu's layers are
 * popped, so a command that opens a dialog of its own does not stack it on top of a
 * menu that is about to vanish. */
int menu_open(int index) {
  int depth = layer_count();
  struct layer dd;
  const struct command *chosen = NULL;
  int li;

  if (index < 0 || index >= NMENUS) return 0;
  memset(&dd, 0, sizeof(dd));
  li = layer_push(&dd);
  if (li < 0) return 0;

  open_menu = index;
  open_item = first_selectable(&menus[open_menu]);

  for (;;) {
    const struct menu *m = &menus[open_menu];
    int c;

    screen_sync_size();
    dropdown_layout(layer_get(li));
    editor_refresh_screen();

    c = ui_wait_key();
    if (c == WINCH_KEY) continue;

    if (c == MOUSE_KEY) {
      struct mouse_event ev;
      struct layer *lp = layer_get(li);
      input_last_mouse(&ev);
      if (ev.type == MOUSE_WHEEL_UP) {
        open_item = next_selectable(m, open_item, -1);
      } else if (ev.type == MOUSE_WHEEL_DOWN) {
        open_item = next_selectable(m, open_item, 1);
      } else if (ev.type == MOUSE_PRESS || ev.type == MOUSE_DRAG) {
        int k = ev.y - (lp->y + 1);
        int i;
        /* On the band: switch to that menu rather than closing and reopening. */
        for (i = 0; i < NMENUS; i++) {
          if (ev.y == lp->y - 1 && ev.x >= title_col[i] - 1 &&
              ev.x <= title_col[i] + title_cols[i]) {
            open_menu = i;
            open_item = first_selectable(&menus[open_menu]);
            break;
          }
        }
        if (i < NMENUS) continue;
        if (ev.x < lp->x || ev.x >= lp->x + lp->w ||
            ev.y < lp->y || ev.y >= lp->y + lp->h) {
          break;   /* a click outside closes, as it does everywhere else */
        }
        if (k >= 0 && k < m->nitems && !item_is_separator(m, k) &&
            item_is_enabled(m, k)) {
          open_item = k;
          if (ev.type == MOUSE_PRESS) {
            chosen = item_cmd(m, k);
            break;
          }
        }
      }
      continue;
    }

    if (c == '\x1b' || c == CTRL_KEY('c')) break;
    if (c == ARROW_LEFT) {
      open_menu = (open_menu > 0) ? open_menu - 1 : NMENUS - 1;
      open_item = first_selectable(&menus[open_menu]);
    } else if (c == ARROW_RIGHT) {
      open_menu = (open_menu + 1) % NMENUS;
      open_item = first_selectable(&menus[open_menu]);
    } else if (c == ARROW_UP) {
      open_item = next_selectable(m, open_item, -1);
    } else if (c == ARROW_DOWN) {
      open_item = next_selectable(m, open_item, 1);
    } else if (c == HOME_KEY) {
      open_item = first_selectable(m);
    } else if (c == END_KEY) {
      open_item = last_selectable(m);
    } else if (c == '\r') {
      if (!item_is_separator(m, open_item) && item_is_enabled(m, open_item)) {
        chosen = item_cmd(m, open_item);
        break;
      }
    } else if (IS_ALT_KEY(c)) {
      int mi = menu_for_mnemonic(ALT_KEY_CHAR(c));
      if (mi >= 0) {
        open_menu = mi;
        open_item = first_selectable(&menus[open_menu]);
      }
    } else if (c >= 0x20 && c < 0x7f) {
      int k = item_for_mnemonic(m, c);
      if (k >= 0) {
        open_item = k;
        chosen = item_cmd(m, k);
        break;
      }
    }
  }

  open_menu = -1;
  layer_pop_to(depth);
  /* The band and the text area move back if the bar is hidden, and the row the
   * drop-down covered has to come back either way. */
  screen_force_repaint();

  if (chosen != NULL && chosen->run != NULL) return chosen->run();
  return 0;
}
