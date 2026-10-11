/* bufdlg.c -- the two dialogs that are lists of buffers.
 *
 * The buffer list picks one by name, which is what Ctrl-PgDn cannot do once
 * there are more tabs than fit the bar. The quit dialog lists the ones with
 * unsaved work and asks what to do with them, which is what a status-line
 * countdown could only gesture at: "3 files have unsaved changes" never said
 * which. They live together because they are the same screen -- a framed list of
 * buffer names -- and differ in what they offer to do about it.
 *
 * Both follow the open dialog's shape: a layer, a straight-line key loop kept
 * here, and widgets declared once so the selection survives a resize. */

#include "molly.h"

static const char *buf_label(const struct buffer *b) {
  return buf_name(b);
}

/* Rows of the buffer list: a marker column, so a modified buffer reads the way
 * it does in the tab bar. `user` is unused; the list is the editor's own. */
static void buflist_row(void *user, int i, char *dst, size_t dsz) {
  const struct buffer *b = buf_at(i);
  (void)user;
  if (b == NULL) {
    if (dsz) dst[0] = '\0';
    return;
  }
  snprintf(dst, dsz, "%c %s", b->dirty ? '*' : ' ', buf_label(b));
}

/* Which buffer to show, or -1 for none. Enter picks, Esc cancels. */
int editor_buffer_dialog(void) {
  struct widget widgets[2];
  struct dialog d;
  struct layer l;
  int depth, li, chosen = -1;

  widget_list(&widgets[0], 0, 0, 1, 1, buf_count(), buflist_row, NULL);
  widget_label(&widgets[1], 0, 1, 1, "Enter show   Esc cancel   * unsaved");
  widgets[0].sel = buf_current();

  depth = layer_count();
  memset(&l, 0, sizeof(l));
  li = layer_push(&l);
  if (li < 0) return -1;

  for (;;) {
    struct layer *lp;
    int cols, rows, list_h, c, ns;

    screen_sync_size();
    cols = grid_cols() - 8;
    if (cols > 60) cols = 60;
    if (cols < 16) cols = 16;
    rows = grid_rows() - 4;
    if (rows > 16) rows = 16;
    if (rows < 5) rows = 5;
    list_h = rows - 2 - 1;
    if (list_h < 1) list_h = 1;

    widgets[0].w = cols - 4;
    widgets[0].h = list_h;
    widgets[0].count = buf_count();
    widgets[1].y = list_h;
    widgets[1].w = cols - 4;
    widget_list_clamp(&widgets[0]);

    dialog_init(&d, "Buffers", widgets, 2, cols - 2, rows - 2);
    lp = layer_get(li);
    dialog_layout(&d, lp);
    editor_refresh_screen();

    c = ui_wait_key();
    if (c == WINCH_KEY) continue;

    if (c == MOUSE_KEY) {
      struct mouse_event ev;
      input_last_mouse(&ev);
      if (ev.type == MOUSE_WHEEL_UP) widget_list_move(&widgets[0], -3);
      else if (ev.type == MOUSE_WHEEL_DOWN) widget_list_move(&widgets[0], 3);
      else if (ev.type == MOUSE_PRESS && ev.button == MOUSE_BUTTON_LEFT) {
        /* A click on a row chooses it, as in the palette and unlike the Open
         * dialog: there a click has more to do -- a directory to walk into, a
         * file to look at before opening -- and here there is nothing to do with
         * a buffer but show it. */
        int k = ev.y - (lp->y + 1 + widgets[0].y);
        if (k >= 0 && k < widgets[0].h && widgets[0].top + k < buf_count()) {
          chosen = widgets[0].top + k;
          break;
        }
      }
      continue;
    }

    ns = 0;
    if (c == ARROW_DOWN || c == CTRL_KEY('n')) ns = 1;
    else if (c == ARROW_UP || c == CTRL_KEY('p')) ns = -1;
    else if (c == PAGE_DOWN) ns = list_h;
    else if (c == PAGE_UP) ns = -list_h;
    else if (c == HOME_KEY) ns = -buf_count();
    else if (c == END_KEY) ns = buf_count();
    else if (c == '\r') { chosen = widgets[0].sel; break; }
    else if (c == '\x1b' || c == CTRL_KEY('c')) break;
    else continue;
    widget_list_move(&widgets[0], ns);
  }

  layer_pop_to(depth);
  return chosen;
}

/* ---- quitting with unsaved work -------------------------------------------- */

/* The dirty buffers, in order, so row i of the list is the i-th unsaved one. */
static void quit_row(void *user, int i, char *dst, size_t dsz) {
  int k, n = 0;
  (void)user;
  for (k = 0; k < buf_count(); k++) {
    if (!buf_at(k)->dirty) continue;
    if (n++ == i) {
      snprintf(dst, dsz, "%s", buf_label(buf_at(k)));
      return;
    }
  }
  if (dsz) dst[0] = '\0';
}

enum { Q_LIST, Q_SAVE, Q_DISCARD, Q_CANCEL, Q_N };

/* QUIT_SAVE, QUIT_DISCARD or QUIT_CANCEL. Save All is the default, because it is
 * the one that cannot lose anything; Esc cancels. */
int editor_quit_dialog(void) {
  struct widget w[Q_N];
  struct dialog d;
  struct layer l;
  int depth, li, answer = QUIT_CANCEL;
  int focus = Q_SAVE; /* dialog_init zeroes it, so it is kept across passes */

  widget_text(&w[Q_LIST], 0, 0, 1, 1, buf_dirty_count(), quit_row, NULL);
  widget_button(&w[Q_SAVE], 0, 0, "Save All", 1);
  widget_button(&w[Q_DISCARD], 0, 0, "Discard", 0);
  widget_button(&w[Q_CANCEL], 0, 0, "Cancel", 0);

  depth = layer_count();
  memset(&l, 0, sizeof(l));
  li = layer_push(&l);
  if (li < 0) return QUIT_CANCEL;

  for (;;) {
    struct layer *lp;
    int cols, rows, c, bw, bx, r, y_btn;

    screen_sync_size();
    cols = grid_cols() - 8;
    if (cols > 56) cols = 56;
    if (cols < 34) cols = 34;
    rows = grid_rows() - 4;
    if (rows > 14) rows = 14;
    if (rows < 5) rows = 5;

    dialog_init(&d, "Unsaved changes", w, Q_N, cols - 2, rows - 2);
    d.focus = focus;
    y_btn = d.rows - 1;
    w[Q_LIST].w = cols - 4;
    w[Q_LIST].h = y_btn - 1 > 0 ? y_btn - 1 : 1;
    w[Q_LIST].count = buf_dirty_count();
    w[Q_SAVE].y = w[Q_DISCARD].y = w[Q_CANCEL].y = y_btn;
    bw = w[Q_SAVE].w + 2 + w[Q_DISCARD].w + 2 + w[Q_CANCEL].w;
    bx = (cols - 4 - bw) / 2;
    if (bx < 0) bx = 0;
    w[Q_SAVE].x = bx;
    w[Q_DISCARD].x = bx + w[Q_SAVE].w + 2;
    w[Q_CANCEL].x = w[Q_DISCARD].x + w[Q_DISCARD].w + 2;

    lp = layer_get(li);
    dialog_layout(&d, lp);
    editor_refresh_screen();

    c = ui_wait_key();
    if (c == WINCH_KEY) continue;
    if (c == MOUSE_KEY) {
      struct mouse_event ev;
      input_last_mouse(&ev);
      if (dialog_mouse(&d, lp, &ev) != DIALOG_PRESSED) continue;
    } else {
      if (c == 's' || c == 'S') d.pressed = Q_SAVE;
      else if (c == 'd' || c == 'D') d.pressed = Q_DISCARD;
      else {
        r = dialog_key(&d, c);
        if (r == DIALOG_CANCEL) break;
        focus = d.focus;
        if (r != DIALOG_PRESSED) continue;
      }
    }
    answer = d.pressed == Q_SAVE ? QUIT_SAVE
           : d.pressed == Q_DISCARD ? QUIT_DISCARD : QUIT_CANCEL;
    break;
  }

  layer_pop_to(depth);
  return answer;
}
