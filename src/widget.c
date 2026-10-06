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
 * Widgets arrive with the thing that needs them. This file holds the label and the
 * list box, which is what the open-file dialog uses; the input field comes with the
 * prompt, the text pane with the help screen, and buttons with the first dialog that
 * has two things to focus. Building the full set first would mean building most of
 * it blind.
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
  grid_role(ROLE_DIALOG);
  grid_puts(oy + w->y, ox + w->x, buf, (int)strlen(buf));
}

static void draw_list(const struct widget *w, int ox, int oy) {
  int k;
  char buf[1024];
  for (k = 0; k < w->h; k++) {
    int i = w->top + k;
    int y = oy + w->y + k;
    int selected = (i == w->sel && w->count > 0);
    grid_role(selected ? ROLE_LIST_SELECTED : ROLE_DIALOG);
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

void dialog_init(struct dialog *d, const char *title,
                 struct widget *widgets, int nwidgets, int cols, int rows) {
  memset(d, 0, sizeof(*d));
  d->title = title;
  d->widgets = widgets;
  d->nwidgets = nwidgets;
  d->cols = cols;
  d->rows = rows;
}

/* Centre the dialog in the terminal, clamped to fit. The shadow is allowed to fall
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

  /* Widgets, positioned relative to the content area inside the frame. */
  for (i = 0; i < d->nwidgets; i++) {
    struct widget *wd = &d->widgets[i];
    switch (wd->kind) {
      case W_LABEL:
        draw_label(wd, x + FRAME_PAD + CONTENT_PAD, y + FRAME_PAD);
        break;
      case W_LIST:
        draw_list(wd, x + FRAME_PAD + CONTENT_PAD, y + FRAME_PAD);
        break;
      default: break;
    }
  }

  grid_role(ROLE_TEXT);

  /* Nothing in this dialog is typed into, so the cursor stays out of sight rather
   * than blinking in the text underneath. */
  grid_cursor_hide();
}
