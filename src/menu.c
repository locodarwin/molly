/* menu.c -- the menu bar and its drop-downs.
 *
 * Built from the command table, so a menu entry is a command *name* and nothing is
 * written down twice: the label, the shortcut shown down the right-hand side, whether
 * the item is available and whether it is ticked all come from the command. Rebind a
 * key and the menu follows; add a command and it has a menu entry the moment it is
 * listed here.
 *
 * Only commands that exist are listed, which is why there are seven menus rather
 * than nine: Tools and Options have nothing in them yet, and a menu that opens
 * onto nothing is worse than a menu that is not there. Their Alt letters stay
 * held regardless, so adding them later costs nothing.
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
  {"edit.select-all", 0}, {"edit.select-line", 7}, SEP,
  /* I and D, both free, in a group of their own between the commands that work
   * on a selection and the one that needs a prompt. This menu is the tallest
   * molly has -- sixteen rows with these two and their separator -- and that is
   * now a question of how much of it a short terminal can show at once rather
   * than of what may be in it: see menu_dropdown_height(). */
  {"edit.indent", 0}, {"edit.dedent", 0},
  /* A of "Tabs" and E of "Spaces", the first letters of each that this menu
   * has not spent. Here rather than in a group of their own because they are
   * the same subject as the two above, and a separator would be a row. */
  {"edit.to-spaces", 1}, {"edit.to-tabs", 4}, SEP,
  /* F, which is the only letter in "Filter" this menu has not spent, and the O
   * of "output" for the same reason: I is Indent's. */
  {"edit.filter", 0}, {"edit.insert-output", 7},
};
static const struct menu_item search_items[] = {
  {"search.find", 0}, {"search.next", 5}, {"search.prev", 5},
  {"search.case", 6}, {"search.regex", 2}, SEP, {"search.replace", 0},
  /* I of "in", then X and V, because N, P and R are spent above. */
  SEP, {"search.in-files", 5}, {"search.next-result", 2},
  {"search.prev-result", 3},
};
static const struct menu_item view_items[] = {
  {"app.palette", 0}, SEP,
  {"view.line-numbers", 0}, {"view.menu-bar", 0}, {"view.tab-bar", 0},
  /* Y of "Syntax", since S is Save settings further down this menu. */
  {"view.syntax", 1},
  /* W of "Soft wrap", for the same reason, and beside the other three toggles
   * that change how the text is drawn rather than what is in it. */
  {"view.soft-wrap", 5},
  SEP, {"view.repaint", 0},
  SEP, {"app.options", 0}, {"app.save-settings", 0},
};
static const struct menu_item go_items[] = {
  {"goto.line", 0}, SEP,
  /* D of "definition", K of "Back" because B is Buffers', R of "references",
   * I of "Info". X and
   * V for the problems, as Search's pair of results have them: N and P are
   * the buffers'. */
  {"goto.definition", 6}, {"goto.back", 3}, {"code.references", 5},
  {"code.hover", 0}, SEP,
  {"goto.next-problem", 2}, {"goto.prev-problem", 3}, SEP,
  /* N and P, since Go has nothing else competing for either. */
  {"buffer.next", 0}, {"buffer.prev", 0}, {"buffer.list", 0},
};
/* S and B, the first letters of the words that differ -- "Split right" and
 * "Split below" both begin with S, and it is the second word that says which is
 * which. N and P for the pair that walks them, as in the Go menu. C for Close,
 * and O for Only, which is the whole of what is left. */
static const struct menu_item window_items[] = {
  {"window.split-right", 6}, {"window.split-below", 6},
  /* T, which nothing else in this menu starts with. */
  {"terminal.toggle", 0}, SEP,
  {"window.next", 0}, {"window.prev", 0}, SEP,
  {"window.close", 0}, {"window.only", 0}, SEP,
  /* L, I, U and D: R is Split right's, so Right takes the I after it. */
  {"window.border-left", 12}, {"window.border-right", 13},
  {"window.border-up", 12}, {"window.border-down", 12},
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
  {"Window", 0, window_items, COUNT(window_items)},
  {"Help",   0, help_items,   COUNT(help_items)},
};

#define NMENUS COUNT(menus)

/* Columns before the first title. The gap between titles is the band's to
 * choose; see menu_band_layout(). */
#define BAR_PAD 1

/* Two columns at the left of every item for a tick, so a menu's labels line up
 * whether or not anything in it is checkable. */
#define CHECK_COLS 2
/* Columns between the longest label and the shortcut column. */
#define ACCEL_GAP 3

/* Which menu is open, -1 for none, and which item is highlighted in it. Read by the
 * bar's draw function, which has to know which title to light up. */
static int open_menu = -1;
static int open_item = 0;
/* First item the open drop-down shows, and how many it has room for. Both are
 * worked out by the layout pass and read by the draw and the mouse, the way
 * title_col is -- a click has to be resolved against the rows that were drawn,
 * not against the list. */
static int open_off = 0;
static int open_vis = 0;

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

/* Where each title goes in a band `width` columns wide, with menu `open` open
 * (-1 for none): col[i] is its first column and cols[i] how many it shows.
 *
 * Seven titles need 43 columns at the band's two-space gaps, and narrower than
 * that the band used to lose its tail -- Help went first, still on Alt-H but
 * nowhere on screen. Dropping titles is the wrong answer because each one is
 * also the letter that opens it, so instead the band gives up space in the
 * order it matters least: the gaps go to one column, then every title becomes
 * its mnemonic letter, which is still the key that opens it. The open menu keeps
 * its whole name when that fits, because a column of items under a lone "W" is
 * the one place the letter does not say enough.
 *
 * Returns which of those four it chose, 0 to 3, for the tests. Below about
 * fourteen columns even the letters clip, which is a terminal nobody edits in. */
static int menu_title_cols(int i, int letters, int open) {
  if (letters && i != open) return 1;
  return utf8_str_cols(menus[i].title);
}

int menu_band_layout(int width, int open, int *col, int *cols) {
  int mode, i;
  for (mode = 0; mode < 4; mode++) {
    int letters = mode >= 2;
    int gap = mode == 0 ? 2 : 1;
    int keep = mode == 2 ? open : -1;
    int need = BAR_PAD - gap;
    if (mode == 2 && open < 0) continue;
    for (i = 0; i < NMENUS; i++) need += gap + menu_title_cols(i, letters, keep);
    if (need <= width || mode == 3) {
      int x = BAR_PAD;
      for (i = 0; i < NMENUS; i++) {
        col[i] = x;
        cols[i] = menu_title_cols(i, letters, keep);
        x += cols[i] + gap;
      }
      return mode;
    }
  }
  return 3;  /* not reached */
}

/* Draw title i in `cols` columns: the whole word, or only its mnemonic letter. */
static void band_title(int y, int i, int x, int cols) {
  const char *t = menus[i].title;
  int mn = menus[i].mnemonic >= 0 ? menus[i].mnemonic : 0;
  int off = utf8_col_to_byte(t, mn);
  int nb;
  int whole = cols == utf8_str_cols(t);

  grid_role(i == open_menu ? ROLE_MENUBAR_SELECTED : ROLE_MENUBAR);
  /* A column of padding each side, so the highlight is a block around the word
   * rather than a tight box against the letters. */
  grid_fill(y, x - 1, cols + 2, ' ');
  (void)utf8_char_cols(t, (int)strlen(t), off, &nb);
  if (whole) grid_puts(y, x, t, (int)strlen(t));
  if (menus[i].mnemonic >= 0) {
    grid_attrs(ATTR_UNDERLINE);
    grid_puts(y, whole ? x + mn : x, t + off, nb);
    grid_attrs(0);
  } else if (!whole) {
    grid_puts(y, x, t + off, nb);
  }
}

void menu_bar_draw(const struct layer *self) {
  int i;

  grid_role(ROLE_MENUBAR);
  grid_fill(self->y, self->x, self->w, ' ');

  menu_band_layout(self->w, open_menu, title_col, title_cols);
  for (i = 0; i < NMENUS; i++) {
    title_col[i] += self->x;
    if (i != open_menu) band_title(self->y, i, title_col[i], title_cols[i]);
  }
  /* The open one last: at one-column gaps its highlight's padding is the
   * neighbours' gap, and drawn first it would be painted over. */
  if (open_menu >= 0 && open_menu < NMENUS)
    band_title(self->y, open_menu, title_col[open_menu], title_cols[open_menu]);
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

/* How tall a drop-down may be, in rows, for `nitems` items opening at row
 * `top` with `last_row` the last row it may touch.
 *
 * Three things have to fit inside that and did not before: the two border rows,
 * an item row each, and the drop shadow -- which lands on the row *after* the
 * box and restyles what is under it rather than painting over it, so a shadow
 * that reaches the status bar dims the half of it that says which line the
 * cursor is on. Hence `last_row - top` rather than `last_row - top + 1`: the
 * row the shadow needs is the one row of slack this leaves.
 *
 * The floor is three rows -- a border, one item, a border -- because a terminal
 * can always be made shorter than any menu, and a box with nothing in it says
 * less than a box with one item in it and an arrow saying there are more. On a
 * screen that short the floor wins and the shadow does land on the chrome;
 * everything above about eight rows is the case this is written for. */
int menu_dropdown_height(int nitems, int top, int last_row) {
  int h = nitems + 2;
  int room = last_row - top;
  if (h > room) h = room;
  if (h < 3) h = 3;
  return h;
}

/* Which item a drop-down showing `vis` of `nitems` starts at, given that `sel`
 * is selected and that it was showing from `off`.
 *
 * The same shape as the tab bar's scroll and the text area's: a window that
 * moves only as far as it must to keep the selected item inside it, so walking
 * down a menu with the arrow keys scrolls a row at a time at the bottom rather
 * than jumping by a boxful, and walking back up retraces it. The clamp comes
 * first because the terminal can get taller as well as shorter: `vis` grows,
 * and an offset that was right for the old height would leave the last rows of
 * the box empty with items above them scrolled away. */
int menu_dropdown_scroll(int nitems, int vis, int sel, int off) {
  int max = nitems - vis;
  if (max < 0) max = 0;
  if (off > max) off = max;
  if (off < 0) off = 0;
  if (sel < off) off = sel;
  if (sel >= off + vis) off = sel - vis + 1;
  return off;
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
/* The arrows that say there are items off the top or the bottom, on the border
 * row at that end. The small triangles rather than the big ones, to sit in a
 * line of box drawing the way the tab bar's ‹ and › sit in a row of tabs. */
#define MENU_UP_ARROW   "\xe2\x96\xb4"   /* U+25B4 */
#define MENU_DOWN_ARROW "\xe2\x96\xbe"   /* U+25BE */
/* Columns in from the right-hand border that an arrow sits at, so the pair
 * lines up vertically like the thumb of a scroll bar. */
#define MENU_ARROW_INSET 2

static void dropdown_draw(const struct layer *self) {
  const struct menu *m = &menus[open_menu];
  int x = self->x, y = self->y, w = self->w, h = self->h;
  struct dd_cols cols;
  int i, k;

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

  for (k = 0; k < open_vis; k++) {
    int item = open_off + k;
    int row = y + 1 + k;
    const struct command *c;
    unsigned char role;
    int enabled, checked;
    char accel[32];

    if (item >= m->nitems) break;

    if (item_is_separator(m, item)) {
      int bx;
      grid_role(ROLE_MENU_FRAME);
      grid_puts(row, x, BOX_LT, 3);
      for (bx = 1; bx < w - 1; bx++) grid_puts(row, x + bx, BOX_H, 3);
      grid_puts(row, x + w - 1, BOX_RT, 3);
      continue;
    }

    c = item_cmd(m, item);
    if (c == NULL) continue;
    enabled = item_is_enabled(m, item);
    checked = (c->checked != NULL && c->checked());
    role = (item == open_item) ? ROLE_MENU_SELECTED
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
      if (m->items[item].mnemonic >= 0 && enabled &&
          m->items[item].mnemonic < shown_cols) {
        int off = utf8_col_to_byte(label, m->items[item].mnemonic);
        int nb;
        (void)utf8_char_cols(label, (int)strlen(label), off, &nb);
        grid_attrs(ATTR_UNDERLINE);
        grid_puts(row, x + 1 + CHECK_COLS + m->items[item].mnemonic, label + off, nb);
        grid_attrs(0);
      }
    }
    if (cols.accel && cmd_shortcut(c->name, accel, sizeof(accel))) {
      int aw = utf8_str_cols(accel);
      grid_role(role);
      /* One column of padding before the border, so the shortcut does not look
       * wedged against item. */
      grid_puts(row, x + w - 2 - aw, accel, (int)strlen(accel));
    }
  }

  /* The arrows sit *in* the border, so they go on after the frame has drawn its
   * run of BOX_H over the row. Nothing says how far the list is scrolled beyond
   * their presence: a menu is short enough that "there is more below" is the
   * whole of what a reader needs, and a proportional thumb in a column one cell
   * wide would be inventing precision it cannot show. */
  if (w > MENU_ARROW_INSET) {
    grid_role(ROLE_MENU_FRAME);
    if (open_off > 0)
      grid_puts(y, x + w - MENU_ARROW_INSET, MENU_UP_ARROW, 3);
    if (open_off + open_vis < m->nitems)
      grid_puts(y + h - 1, x + w - MENU_ARROW_INSET, MENU_DOWN_ARROW, 3);
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

/* Open a different menu from the one that is open: its first usable item, and
 * its list scrolled back to the top. All four ways of moving along the bar go
 * through here -- the arrow keys, an Alt letter, a click on another title and
 * the first open -- because a menu that remembered the last one's scroll offset
 * would open showing its middle. */
static void menu_switch_to(int index) {
  open_menu = index;
  open_item = first_selectable(&menus[index]);
  open_off = 0;
}

static int last_selectable(const struct menu *m) {
  int i;
  for (i = m->nitems - 1; i >= 0; i--)
    if (!item_is_separator(m, i) && item_is_enabled(m, i)) return i;
  return 0;
}

/* Show the row past the edge the clicked arrow points at.
 *
 * The window follows the selection and nothing else, so moving the selection is
 * the only thing that can move the window: this takes it to the first item past
 * the edge, which scrolls by exactly the one row the arrow is offering. A
 * selection sitting in the middle of the box therefore jumps to the edge, which
 * is the right answer for a click that means "show me what is down there". If
 * there is nothing selectable past the edge -- a tail of separators or of
 * unavailable items -- nothing moves, and the arrow stays lit because the items
 * are still there to be scrolled to. */
static void menu_step_past_edge(const struct menu *m, int dir) {
  int from = (dir > 0) ? open_off + open_vis - 1 : open_off;
  int to;
  if (from < 0) from = 0;
  if (from >= m->nitems) from = m->nitems - 1;
  to = next_selectable(m, from, dir);
  if (to != from) open_item = to;
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

/* Lay the drop-down out under its title, kept on screen at the right-hand edge
 * and inside the text area at the bottom. */
static void dropdown_layout(struct layer *l) {
  const struct menu *m = &menus[open_menu];
  int w = menu_width(m);
  int h;
  int bar_y = E.menubar ? 0 : 0;   /* the band's row either way; see menu_open */
  int wx, wy, ww, wh, last_row;

  if (w > grid_cols()) w = grid_cols();
  /* Worked out again rather than read from the last draw: in the narrowest
   * band the open title is the one written out in full, so switching menus
   * moves the titles, and the band has not been drawn with the new one yet. */
  menu_band_layout(grid_cols(), open_menu, title_col, title_cols);
  l->x = title_col[open_menu] - 1;
  if (l->x + w > grid_cols()) l->x = grid_cols() - w;
  if (l->x < 0) l->x = 0;
  l->y = bar_y + 1;

  /* The bottom is the text area's, not the grid's. The last row of the window
   * area is a window's status row -- every window carries its own -- and under
   * that is the message bar, so the last row a drop-down may touch is two above
   * the end of the area. The window area is itself floored at a minimum height,
   * which on a terminal shorter than that is taller than the grid, so the grid
   * still has the last word. */
  screen_window_area(&wx, &wy, &ww, &wh);
  (void)wx; (void)ww;
  last_row = wy + wh - 2;
  if (last_row > grid_rows() - 2) last_row = grid_rows() - 2;

  h = menu_dropdown_height(m->nitems, l->y, last_row);
  open_vis = h - 2;
  open_off = menu_dropdown_scroll(m->nitems, open_vis, open_item, open_off);
  /* A separator as the first visible row reads as a second border rather than
   * as a divider between two groups, since the group above it is scrolled away.
   * So the window starts one later when it can: only while scrolled, only if
   * the row it gives up is not the last the box can show, and only if the
   * selection stays inside -- a tidier top is not worth hiding the highlight.
   * The scroll above is the thing that keeps the selection visible; this is
   * allowed to prefer a row and never to overrule it. */
  if (open_off > 0 && open_off < m->nitems - open_vis &&
      item_is_separator(m, open_off) && open_item > open_off)
    open_off++;

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

  menu_switch_to(index);

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
            menu_switch_to(i);
            break;
          }
        }
        if (i < NMENUS) continue;
        if (ev.x < lp->x || ev.x >= lp->x + lp->w ||
            ev.y < lp->y || ev.y >= lp->y + lp->h) {
          break;   /* a click outside closes, as it does everywhere else */
        }
        /* On a border row with an arrow on it: one step that way, which is what
         * makes the next item appear. The tab bar's arrows work the same, and
         * for the same reason -- the arrow means "there are more", and a
         * boxful would lose the user's place. */
        if (ev.type == MOUSE_PRESS && k == -1 && open_off > 0) {
          menu_step_past_edge(m, -1);
          continue;
        }
        if (ev.type == MOUSE_PRESS && k == open_vis &&
            open_off + open_vis < m->nitems) {
          menu_step_past_edge(m, 1);
          continue;
        }
        if (k >= 0 && k < open_vis && open_off + k < m->nitems &&
            !item_is_separator(m, open_off + k) &&
            item_is_enabled(m, open_off + k)) {
          open_item = open_off + k;
          if (ev.type == MOUSE_PRESS) {
            chosen = item_cmd(m, open_item);
            break;
          }
        }
      }
      continue;
    }

    if (c == '\x1b' || c == CTRL_KEY('c')) break;
    if (c == ARROW_LEFT) {
      menu_switch_to((open_menu > 0) ? open_menu - 1 : NMENUS - 1);
    } else if (c == ARROW_RIGHT) {
      menu_switch_to((open_menu + 1) % NMENUS);
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
      if (mi >= 0) menu_switch_to(mi);
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

  if (chosen != NULL && chosen->run != NULL) return cmd_run(chosen);
  return 0;
}
