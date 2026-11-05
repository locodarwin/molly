/* widget.c -- dialogs and the things inside them.
 *
 * A dialog is a layer: it says where it is and how to draw itself, and the ordinary
 * screen refresh paints it along with everything else. That is the whole reason
 * Phase 1 built a layer stack first, and what it buys is visible in the diff -- the
 * old open-file dialog cleared the screen, positioned every row by hand, emitted
 * its own escape sequences, and asked for a full repaint on the way out. None of
 * that is needed. It draws cells; the grid works out what changed.
 *
 * Turbo Vision's chrome, in modern colors: a double-line frame, the title inset in
 * the top border, and a drop shadow two columns right and one row down. The shadow
 * is the cells underneath restyled rather than painted over, so the text stays
 * legible through it, which is what it looked like on a VGA console and costs one
 * call.
 *
 * Widgets arrive with the thing that needs them: the label and the list box came
 * with the open-file dialog, the text pane with the help screen, the input field with
 * the prompt and the command palette. Buttons wait for the first dialog with two
 * things to focus, which is Phase 2's Find dialog -- building the full set first
 * would mean building most of it blind.
 *
 * The caller keeps the loop. Nothing here waits for a key: a dialog is laid out and
 * drawn, the caller reads with ui_wait_key and decides what a key means. Modal
 * loops that nest are the decision recorded in the roadmap, and keeping the loop in
 * the caller is what lets the open dialog stay the straight-line function it was. */

#include "molly.h"

/* Double-line box drawing, which is the Turbo Vision look. Each is three bytes of
 * UTF-8 and one column wide. */
#define BOX_TL "\xe2\x95\x94"  /* U+2554 */
#define BOX_TR "\xe2\x95\x97"  /* U+2557 */
#define BOX_BL "\xe2\x95\x9a"  /* U+255A */
#define BOX_BR "\xe2\x95\x9d"  /* U+255D */
#define BOX_H  "\xe2\x95\x90"  /* U+2550 */
#define BOX_V  "\xe2\x95\x91"  /* U+2551 */

/* Rows and columns the frame takes from the dialog's outer size. */
#define FRAME_PAD 1

/* A column of breathing room inside the frame, left and right. Vertical padding is
 * deliberately not added: rows are scarcer than columns, and a dialog that wastes
 * two of them on air shows two fewer files. */
#define CONTENT_PAD 1

void widget_label(struct widget *w, int x, int y, int cols, const char *text) {
  memset(w, 0, sizeof(*w));
  w->kind = W_LABEL;
  w->role = ROLE_DIALOG;
  w->x = x;
  w->y = y;
  w->w = cols;
  w->h = 1;
  w->text = text;
}

void widget_list(struct widget *w, int x, int y, int cols, int rows,
                 int count, widget_row_fn row, void *user) {
  memset(w, 0, sizeof(*w));
  w->kind = W_LIST;
  w->role = ROLE_DIALOG;
  w->x = x;
  w->y = y;
  w->w = cols;
  w->h = rows;
  w->count = count;
  w->row = row;
  w->user = user;
}

/* Keep the selection inside the list and the window around the selection. Called
 * before drawing, so a resize that shortens the list cannot leave either adrift. */
void widget_list_clamp(struct widget *w) {
  if (w->count <= 0) {
    w->sel = 0;
    w->top = 0;
    return;
  }
  if (w->sel >= w->count) w->sel = w->count - 1;
  if (w->sel < 0) w->sel = 0;
  if (w->h < 1) return;
  if (w->sel < w->top) w->top = w->sel;
  if (w->sel >= w->top + w->h) w->top = w->sel - w->h + 1;
  if (w->top > w->count - 1) w->top = w->count - 1;
  if (w->top < 0) w->top = 0;
}

/* Move the selection by `delta` rows, clamping at the ends rather than wrapping --
 * a file list that jumps from the bottom back to the top on one more keypress
 * loses the user's place more often than it helps. */
void widget_list_move(struct widget *w, int delta) {
  w->sel += delta;
  widget_list_clamp(w);
}

/* A text pane: a list with nothing selected. Scrolled rather than navigated, so it
 * keeps `top` and ignores `sel`. */
void widget_text(struct widget *w, int x, int y, int cols, int rows,
                 int count, widget_row_fn row, void *user) {
  memset(w, 0, sizeof(*w));
  w->kind = W_TEXT;
  w->role = ROLE_DIALOG;
  w->x = x;
  w->y = y;
  w->w = cols;
  w->h = rows;
  w->count = count;
  w->row = row;
  w->user = user;
}

/* How far a pane can scroll before its last line is at the bottom. */
int widget_text_maxtop(const struct widget *w) {
  int m = w->count - w->h;
  return m > 0 ? m : 0;
}

void widget_text_scroll(struct widget *w, int delta) {
  int max = widget_text_maxtop(w);
  w->top += delta;
  if (w->top > max) w->top = max;
  if (w->top < 0) w->top = 0;
}

/* --- the input field ------------------------------------------------------ *
 *
 * An editable line: what editor_prompt used to be, with the loop taken out and a
 * cursor put in. The widget owns its buffer and grows it, so neither the prompt nor
 * the palette has to decide in advance how much anyone might type.
 *
 * It holds bytes rather than characters, as everything else in molly does, but every
 * move and every delete lands on a character boundary -- so an edit can no more
 * split a UTF-8 sequence here than it can in the buffer. */

#define INPUT_INITIAL 128

void widget_input(struct widget *w, int x, int y, int cols) {
  memset(w, 0, sizeof(*w));
  w->kind = W_INPUT;
  w->role = ROLE_INPUT;
  w->x = x;
  w->y = y;
  w->w = cols;
  w->h = 1;
  w->icap = INPUT_INITIAL;
  w->ibuf = xmalloc((size_t)w->icap);
  w->ibuf[0] = '\0';
}

void widget_input_free(struct widget *w) {
  free(w->ibuf);
  w->ibuf = NULL;
  w->ilen = 0;
  w->icap = 0;
  w->icur = 0;
  w->iscroll = 0;
}

const char *widget_input_text(const struct widget *w) {
  return (w->ibuf != NULL) ? w->ibuf : "";
}

static void input_reserve(struct widget *w, int extra) {
  if (w->ilen + extra + 1 <= w->icap) return;
  while (w->icap < w->ilen + extra + 1) w->icap *= 2;
  w->ibuf = xrealloc(w->ibuf, (size_t)w->icap);
}

/* Insert one byte at the cursor.
 *
 * One byte rather than one character, deliberately: the input layer delivers a
 * multi-byte character as its bytes in order, and inserting each at a cursor that
 * then steps past it reassembles the character where it belongs. */
static void input_insert_byte(struct widget *w, int c) {
  input_reserve(w, 1);
  memmove(w->ibuf + w->icur + 1, w->ibuf + w->icur,
          (size_t)(w->ilen - w->icur + 1));
  w->ibuf[w->icur] = (char)c;
  w->ilen++;
  w->icur++;
}

static void input_delete_range(struct widget *w, int from, int to) {
  if (from < 0 || to > w->ilen || from >= to) return;
  memmove(w->ibuf + from, w->ibuf + to, (size_t)(w->ilen - to + 1));
  w->ilen -= to - from;
  w->icur = from;
}

/* Act on one key. Returns 1 if the field took it, so the caller's loop can tell a
 * key that edited the text from one that means something to the dialog around it --
 * which is how the palette gives Home and End to the text and the arrows to the
 * list without either having to know about the other. */
int widget_input_key(struct widget *w, int c) {
  if (w->ibuf == NULL) return 0;
  switch (c) {
    case BACKSPACE:
    case CTRL_KEY('h'):
      if (w->icur > 0) input_delete_range(w, utf8_prev(w->ibuf, w->icur), w->icur);
      return 1;
    case DEL_KEY:
      if (w->icur < w->ilen)
        input_delete_range(w, w->icur,
                           w->icur + utf8_char_bytes(w->ibuf, w->ilen, w->icur));
      return 1;
    case ARROW_LEFT:
      w->icur = utf8_prev(w->ibuf, w->icur);
      return 1;
    case ARROW_RIGHT:
      if (w->icur < w->ilen)
        w->icur += utf8_char_bytes(w->ibuf, w->ilen, w->icur);
      return 1;
    case HOME_KEY:
      w->icur = 0;
      return 1;
    case END_KEY:
      w->icur = w->ilen;
      return 1;
    /* It only fills in the text: nothing in it can accept the field, so a paste
     * cannot answer a question or name a file to save over. */
    case PASTE_KEY: {
      int n, i;
      const char *text = input_paste_text(&n);
      for (i = 0; i < n; i++)
        if (text[i] != '\n' && text[i] != '\t') input_insert_byte(w, (unsigned char)text[i]);
      return 1;
    }
    default:
      break;
  }
  /* Any byte that is neither a control code nor one of the >= 1000 key codes, so a
   * typed or pasted UTF-8 character enters the field intact. */
  if (c >= 32 && c != 127 && c < 256) {
    input_insert_byte(w, c);
    return 1;
  }
  return 0;
}


/* Truncate to fit `cols` columns without splitting a character. Names are
 * arbitrary bytes and the box has fixed columns; display width is honored so a
 * CJK name cannot overflow by a column. */
static int fit_cols(const char *s, int cols, char *dst, size_t dsz) {
  int i = 0, used = 0;
  size_t out = 0;
  if (cols <= 0 || dsz == 0) {
    if (dsz) dst[0] = '\0';
    return 0;
  }
  while (s[i] != '\0') {
    int nb, wd;
    int len = (int)strlen(s + i);
    wd = utf8_char_cols(s + i, len, 0, &nb);
    if (used + wd > cols) break;
    if (out + (size_t)nb + 1 > dsz) break;
    memcpy(dst + out, s + i, (size_t)nb);
    out += (size_t)nb;
    used += wd;
    i += nb;
  }
  dst[out] = '\0';
  return used;
}

static void draw_label(const struct widget *w, int ox, int oy) {
  char buf[512];
  if (w->text == NULL) return;
  fit_cols(w->text, w->w, buf, sizeof(buf));
  grid_role(w->role);
  grid_puts(oy + w->y, ox + w->x, buf, (int)strlen(buf));
}

static void draw_text(const struct widget *w, int ox, int oy) {
  int k;
  char buf[1024], fitted[1024];
  grid_role(w->role);
  for (k = 0; k < w->h; k++) {
    int i = w->top + k;
    int y = oy + w->y + k;
    grid_fill(y, ox + w->x, w->w, ' ');
    if (i >= w->count) continue;
    buf[0] = '\0';
    w->row(w->user, i, buf, sizeof(buf));
    fit_cols(buf, w->w, fitted, sizeof(fitted));
    grid_puts(y, ox + w->x, fitted, (int)strlen(fitted));
  }
}

static void draw_list(const struct widget *w, int ox, int oy) {
  int k;
  char buf[1024];
  for (k = 0; k < w->h; k++) {
    int i = w->top + k;
    int y = oy + w->y + k;
    int selected = (i == w->sel && w->count > 0);
    unsigned char role = w->role;
    /* A row may have a role of its own -- the palette dims a command that is not
     * available -- but the selected row takes the highlight regardless, so the bar
     * never disappears over an unavailable entry. */
    if (!selected && w->row_role != NULL && i < w->count)
      role = w->row_role(w->user, i);
    grid_role(selected ? ROLE_LIST_SELECTED : role);
    /* The selected row is highlighted across the full width, not just behind its
     * text, so the bar reads as a row rather than as a word. */
    grid_fill(y, ox + w->x, w->w, ' ');
    if (i >= w->count) continue;
    buf[0] = '\0';
    w->row(w->user, i, buf, sizeof(buf));
    {
      char fitted[1024];
      fit_cols(buf, w->w, fitted, sizeof(fitted));
      grid_puts(y, ox + w->x, fitted, (int)strlen(fitted));
    }
  }
}

/* Display columns the first `n` bytes of `s` occupy. */
static int cols_upto(const char *s, int n) {
  int i = 0, cols = 0;
  while (i < n) {
    int nb;
    cols += utf8_char_cols(s, n, i, &nb);
    i += nb;
  }
  return cols;
}

/* Draw an input field at an absolute position and claim the cursor. Returns the
 * columns its text filled, which is what lets the prompt put its hint directly after
 * the answer instead of at a fixed column.
 *
 * The horizontal scroll is settled here rather than when a key arrives, because it
 * depends on the field's width -- and the width is only known once the dialog, or in
 * the prompt's case the terminal, has been laid out for this frame. */
int widget_input_draw(struct widget *w, int y, int x, int maxcols, int claim) {
  char fitted[1024];
  int ccol, off, used;

  if (maxcols < 1 || w->ibuf == NULL) return 0;
  ccol = cols_upto(w->ibuf, w->icur);
  /* Keep the cursor in view, with the rightmost column kept for it: a cursor sitting
   * one past the last character still has to be somewhere. */
  if (ccol - w->iscroll > maxcols - 1) w->iscroll = ccol - maxcols + 1;
  if (ccol < w->iscroll) w->iscroll = ccol;
  if (w->iscroll < 0) w->iscroll = 0;

  off = utf8_col_to_byte(w->ibuf, w->iscroll);
  used = fit_cols(w->ibuf + off, maxcols, fitted, sizeof(fitted));
  grid_role(w->role);
  grid_fill(y, x, maxcols, ' ');
  grid_puts(y, x, fitted, (int)strlen(fitted));
  /* The cursor goes where the text is being typed -- but only if this field is
   * where that is. A dialog with a field and four other controls would otherwise
   * park the cursor in the field while the highlight was on a checkbox, and two
   * things would each be claiming to have the keyboard. */
  if (claim) grid_cursor_claim(y, x + (ccol - w->iscroll));
  return used;
}

static void draw_input(struct widget *w, int ox, int oy, int focused) {
  widget_input_draw(w, oy + w->y, ox + w->x, w->w, focused);
}

/* --- checkboxes, buttons, and a focus order ---------------------------------
 *
 * The three pieces the toolkit was missing, and the reason it was missing them:
 * nothing until now had more than one thing on it worth focusing, so there was no
 * focus order -- and with no focus order a button has no way to be pressed and a
 * checkbox no way to be reached. The Find dialog is the first screen with a field,
 * two settings and two buttons on it, and these arrived with it.
 *
 * Focus is shown with one role rather than with a cursor or a marker: whichever
 * control has the keyboard is drawn in ROLE_FOCUS, and the rest are not. The
 * glyphs carry the state -- [ ] against [TICK] -- and the color carries the
 * focus, which means neither has to be read to understand the other. */

#define TICK "\xe2\x9c\x93"  /* U+2713, the same tick the menus use */

void widget_check(struct widget *w, int x, int y, int cols, const char *text,
                  int on) {
  memset(w, 0, sizeof(*w));
  w->kind = W_CHECK;
  w->x = x;
  w->y = y;
  w->w = cols;
  w->h = 1;
  w->role = ROLE_DIALOG;
  w->text = text;
  w->on = (unsigned char)(on != 0);
}

/* How wide a button with this label will be, so that a caller can lay two of them
 * out side by side before it has either. */
int widget_button_cols(const char *text) {
  return utf8_str_cols(text != NULL ? text : "") + 4;
}

void widget_button(struct widget *w, int x, int y, const char *text,
                   int is_default) {
  memset(w, 0, sizeof(*w));
  w->kind = W_BUTTON;
  w->x = x;
  w->y = y;
  w->w = widget_button_cols(text);
  w->h = 1;
  w->role = ROLE_DIALOG;
  w->text = text;
  w->dflt = (unsigned char)(is_default != 0);
}

/* W_LABEL and W_TEXT are deliberately absent: a label has nothing to do with a
 * keystroke, and the one scrolling text pane molly has is driven by its own modal
 * loop rather than by a focus order. W_LIST is absent for the same reason -- the
 * command palette moves its list directly -- and belongs here the moment a dialog
 * has a list *and* something else to tab to. */
int widget_can_focus(const struct widget *w) {
  return w->kind == W_INPUT || w->kind == W_CHECK || w->kind == W_BUTTON;
}

/* Move the focus by delta, wrapping, skipping whatever cannot take it. */
void dialog_focus_move(struct dialog *d, int delta) {
  int n = d->nwidgets, k;

  if (n <= 0 || delta == 0) return;
  for (k = 1; k <= n; k++) {
    int i = ((d->focus + delta * k) % n + n) % n;
    if (widget_can_focus(&d->widgets[i])) {
      d->focus = i;
      return;
    }
  }
}

static void draw_check(const struct widget *w, int ox, int oy, int focused) {
  char buf[256], cut[256];
  int y = oy + w->y, x = ox + w->x, n;

  grid_role(focused ? ROLE_FOCUS : w->role);
  if (w->w > 0) grid_fill(y, x, w->w, ' ');
  snprintf(buf, sizeof buf, "[%s] %s", w->on ? TICK : " ",
           w->text != NULL ? w->text : "");
  n = fit_cols(buf, w->w, cut, sizeof cut);
  (void)n;
  grid_puts(y, x, cut, (int)strlen(cut));
}

/* maxcols is what is left of the content area to the right of this button. A
 * button is the one widget whose width comes from its label rather than from the
 * caller, so it is also the one that can be wider than the room for it. */
static void draw_button(const struct widget *w, int ox, int oy, int focused,
                        int maxcols) {
  char buf[256], cut[256];

  snprintf(buf, sizeof buf, "[ %s ]", w->text != NULL ? w->text : "");
  fit_cols(buf, maxcols, cut, sizeof cut);
  grid_role(focused ? ROLE_FOCUS : ROLE_BUTTON);
  /* The default button is the one Enter presses from anywhere, so it has to be
   * tellable from the others while something else has the focus. */
  if (w->dflt) grid_attrs(ATTR_BOLD);
  grid_puts(oy + w->y, ox + w->x, cut, (int)strlen(cut));
  grid_attrs(0);
}

void dialog_init(struct dialog *d, const char *title,
                 struct widget *widgets, int nwidgets, int cols, int rows) {
  memset(d, 0, sizeof(*d));
  d->title = title;
  d->widgets = widgets;
  d->nwidgets = nwidgets;
  d->cols = cols;
  d->rows = rows;
}

/* Center the dialog in the terminal, clamped to fit. The shadow is allowed to fall
 * off the right and bottom edges rather than shifting the dialog to make room for
 * it -- a dialog that moves when the terminal is one column too narrow looks like a
 * bug, and a clipped shadow looks like a shadow. */
void dialog_layout(struct dialog *d, struct layer *l) {
  int total_w = d->cols + 2 * FRAME_PAD;
  int total_h = d->rows + 2 * FRAME_PAD;
  int screen_w = grid_cols();
  int screen_h = grid_rows();

  if (total_w > screen_w) total_w = screen_w;
  if (total_h > screen_h) total_h = screen_h;
  d->cols = total_w - 2 * FRAME_PAD;
  d->rows = total_h - 2 * FRAME_PAD;
  if (d->cols < 1) d->cols = 1;
  if (d->rows < 1) d->rows = 1;

  l->x = (screen_w - total_w) / 2;
  l->y = (screen_h - total_h) / 2;
  if (l->x < 0) l->x = 0;
  if (l->y < 0) l->y = 0;
  l->w = total_w;
  l->h = total_h;
  l->draw = dialog_draw;
  l->user = d;
}

/* Draw the frame, the title and every widget. Installed as the layer's draw
 * function, so this runs from the ordinary refresh. */
void dialog_draw(const struct layer *self) {
  struct dialog *d = self->user;
  int x = self->x, y = self->y, w = self->w, h = self->h;
  int i;

  /* The shadow first, so the frame draws over the part of it that overlaps. Two
   * columns right and one row down, as Turbo Vision had it. */
  grid_shade(x + w, y + 1, 2, h, ROLE_DIALOG_SHADOW);
  grid_shade(x + 2, y + h, w, 1, ROLE_DIALOG_SHADOW);

  /* Body. */
  grid_role(ROLE_DIALOG);
  for (i = 0; i < h; i++) grid_fill(y + i, x, w, ' ');

  /* Frame. */
  grid_role(ROLE_DIALOG_FRAME);
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

  /* Title, inset in the top border with a space either side. */
  if (d->title != NULL && w > 6) {
    char buf[256];
    int tw = fit_cols(d->title, w - 6, buf, sizeof(buf));
    int tx = x + (w - tw - 2) / 2;
    grid_role(ROLE_DIALOG_FRAME);
    grid_puts(y, tx, " ", 1);
    grid_puts(y, tx + tw + 1, " ", 1);
    grid_role(ROLE_DIALOG_TITLE);
    grid_puts(y, tx + 1, buf, (int)strlen(buf));
  }

  /* Widgets, positioned relative to the content area inside the frame.
   *
   * A widget whose row is outside that area is not drawn at all. The dialog is
   * clamped to the terminal, so on a short one there are fewer content rows than
   * the caller laid out for -- and a widget that drew anyway would draw over the
   * bottom border and past it, which is what a six-row terminal used to get. A
   * caller that would rather rearrange than lose a row can read d->rows after
   * dialog_layout and move things; this is the backstop, not the plan. */
  for (i = 0; i < d->nwidgets; i++) {
    struct widget *wd = &d->widgets[i];
    int focused = (i == d->focus);
    int avail = d->cols - 2 * CONTENT_PAD - wd->x;
    if (wd->y < 0 || wd->y >= d->rows) continue;
    if (wd->x < 0 || avail <= 0) continue;
    switch (wd->kind) {
      case W_LABEL:
        draw_label(wd, x + FRAME_PAD + CONTENT_PAD, y + FRAME_PAD);
        break;
      case W_LIST:
        draw_list(wd, x + FRAME_PAD + CONTENT_PAD, y + FRAME_PAD);
        break;
      case W_TEXT:
        draw_text(wd, x + FRAME_PAD + CONTENT_PAD, y + FRAME_PAD);
        break;
      case W_INPUT:
        draw_input(wd, x + FRAME_PAD + CONTENT_PAD, y + FRAME_PAD, focused);
        break;
      case W_CHECK:
        draw_check(wd, x + FRAME_PAD + CONTENT_PAD, y + FRAME_PAD, focused);
        break;
      case W_BUTTON:
        draw_button(wd, x + FRAME_PAD + CONTENT_PAD, y + FRAME_PAD, focused,
                    avail);
        break;
      default: break;
    }
  }

  grid_role(ROLE_TEXT);

  /* Unless a field in the dialog claimed the cursor while drawing, it stays out of
   * sight: a dialog with nothing to type into has no business parking a blinking
   * cursor in the text underneath it. */
  if (!grid_cursor_claimed()) grid_cursor_hide();
}


/* --- what a dialog does with a key, before its caller sees it ---------------
 *
 * Every modal loop in molly would otherwise repeat this, and they would drift:
 * Tab would move the focus in one dialog and not in another, Enter would press
 * the default button here and the focused one there. */
int dialog_key(struct dialog *d, int c) {
  struct widget *f = (d->focus >= 0 && d->focus < d->nwidgets)
                   ? &d->widgets[d->focus] : NULL;
  int i;

  if (c == '\x1b' || c == CTRL_KEY('c')) return DIALOG_CANCEL;

  if (c == '\t' || c == BACKTAB_KEY) {
    dialog_focus_move(d, c == '\t' ? 1 : -1);
    return DIALOG_HANDLED;
  }

  /* Enter presses the button that has the focus, or the default one from
   * anywhere else -- which is what makes "type the term, press Enter" work
   * without the field knowing that a button exists. */
  if (c == '\r' || c == '\n') {
    if (f != NULL && f->kind == W_BUTTON) {
      d->pressed = d->focus;
      return DIALOG_PRESSED;
    }
    for (i = 0; i < d->nwidgets; i++) {
      if (d->widgets[i].kind == W_BUTTON && d->widgets[i].dflt) {
        d->pressed = i;
        return DIALOG_PRESSED;
      }
    }
    return DIALOG_IGNORED;
  }

  if (f != NULL && c == ' ' && f->kind == W_CHECK) {
    f->on = (unsigned char)!f->on;
    return DIALOG_HANDLED;
  }
  if (f != NULL && c == ' ' && f->kind == W_BUTTON) {
    d->pressed = d->focus;
    return DIALOG_PRESSED;
  }
  /* The field first, so that it keeps the arrows, Home, End and everything
   * printable; it says no to what it cannot use, and Up and Down fall through to
   * the focus order below. */
  if (f != NULL && f->kind == W_INPUT && widget_input_key(f, c))
    return DIALOG_HANDLED;
  if (c == ARROW_UP || c == ARROW_DOWN) {
    dialog_focus_move(d, c == ARROW_UP ? -1 : 1);
    return DIALOG_HANDLED;
  }
  return DIALOG_IGNORED;
}

/* Which widget is at this screen position, or -1. */
int dialog_hit(const struct dialog *d, const struct layer *l, int row, int col) {
  int ox = l->x + FRAME_PAD + CONTENT_PAD, oy = l->y + FRAME_PAD;
  int i;

  for (i = 0; i < d->nwidgets; i++) {
    const struct widget *w = &d->widgets[i];
    int wx = ox + w->x, wy = oy + w->y;
    if (w->w <= 0 || w->h <= 0) continue;
    if (row >= wy && row < wy + w->h && col >= wx && col < wx + w->w) return i;
  }
  return -1;
}

/* A click focuses what it landed on, and does the obvious thing as well: a
 * checkbox is what you clicked because you wanted it ticked, and a button
 * because you wanted it pressed. */
int dialog_mouse(struct dialog *d, const struct layer *l,
                 const struct mouse_event *ev) {
  int i;

  if (ev->type != MOUSE_PRESS || ev->button != MOUSE_BUTTON_LEFT)
    return DIALOG_IGNORED;
  i = dialog_hit(d, l, ev->y, ev->x);
  if (i < 0 || !widget_can_focus(&d->widgets[i])) return DIALOG_IGNORED;
  d->focus = i;
  if (d->widgets[i].kind == W_CHECK) {
    d->widgets[i].on = (unsigned char)!d->widgets[i].on;
    return DIALOG_HANDLED;
  }
  if (d->widgets[i].kind == W_BUTTON) {
    d->pressed = i;
    return DIALOG_PRESSED;
  }
  return DIALOG_HANDLED;
}
