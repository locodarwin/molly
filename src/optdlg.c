/* optdlg.c -- the Options dialog: every setting, on one screen.
 *
 * Presentation over config.c's table and nothing more. The rows are the table's
 * rows in the table's order, read through setting_info() and setting_live(), and
 * a change goes through setting_set() -- so a setting added to the table is in
 * this dialog without this file knowing, and there is no second list here to
 * fall out of step with the first.
 *
 * Changes take effect as they are made, the way the View menu's toggles do,
 * because a setting you cannot see the effect of is a setting you have to guess
 * about. That makes Esc the one that needs care: it puts back every value the
 * dialog opened with, so looking around in here costs nothing. Enter keeps the
 * changes for this session; Ctrl-S keeps them and writes mollyrc, which is what
 * View > Save settings does with whatever the editor is doing now.
 *
 * A number is changed with Left and Right a step at a time, or typed: digits
 * replace it, and it is put into effect -- clamped to its range -- when the
 * selection moves off it or the dialog closes. Undo steps run to a million and
 * nobody presses Right for that. */

#include "molly.h"

/* What the row callback needs that the list widget does not carry. */
struct opt_state {
  int width;         /* the list's columns, for right-aligning the value */
  int sel;           /* the row being typed into, when `typed` is not empty */
  char typed[12];    /* digits typed over a number, not yet put into effect */
};

static void opt_row(void *user, int i, char *dst, size_t dsz) {
  const struct opt_state *st = user;
  const char *sec, *name, *comment;
  char val[24];
  int is_bool, lo, hi, pad, nlen;

  if (!setting_info(i, &sec, &name, &is_bool, &lo, &hi, &comment)) {
    if (dsz) dst[0] = '\0';
    return;
  }
  if (is_bool)
    snprintf(val, sizeof(val), "%s", setting_live(i) ? "on" : "off");
  else if (st->typed[0] != '\0' && st->sel == i)
    snprintf(val, sizeof(val), "%s_", st->typed);
  else
    snprintf(val, sizeof(val), "%d", setting_live(i));
  /* The section as a column, because "timeout" on its own says nothing and
   * "shell  timeout" is also how mollyrc groups it. Seven wide: the longest
   * section is "editor" and "search", and one more is the gap. */
  /* The name gives way when the dialog is narrow, never the value: a row that
   * says "match case of" has lost the one word it was for. */
  nlen = st->width - 1 - 8 - (int)strlen(val) - 1;
  if (nlen < 1) nlen = 1;
  if (nlen > (int)strlen(name)) nlen = (int)strlen(name);
  pad = st->width - 1 - 8 - nlen - (int)strlen(val);
  if (pad < 1) pad = 1;
  snprintf(dst, dsz, " %-7s %.*s%*s%s", sec, nlen, name, pad, "", val);
}

/* Put typed digits into effect, if there are any. */
static void commit_typed(struct opt_state *st) {
  if (st->typed[0] == '\0') return;
  setting_set(st->sel, atoi(st->typed));
  st->typed[0] = '\0';
}

/* Change row i by `step`: a toggle for on/off, a step for a number. */
static void nudge(int i, int step) {
  const char *sec, *name, *comment;
  int is_bool, lo, hi;
  if (!setting_info(i, &sec, &name, &is_bool, &lo, &hi, &comment)) return;
  if (is_bool) setting_set(i, step > 0 ? 1 : (step < 0 ? 0 : !setting_live(i)));
  else setting_set(i, setting_live(i) + step);
}

static int row_is_bool(int i) {
  const char *sec, *name, *comment;
  int is_bool = 0, lo, hi;
  setting_info(i, &sec, &name, &is_bool, &lo, &hi, &comment);
  return is_bool;
}

void editor_options_dialog(void) {
  struct widget widgets[3];
  struct opt_state st;
  struct dialog d;
  struct layer l;
  char desc[256];
  int n = setting_count();
  int *was = xmalloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
  int depth, li, i, keep = 0, save = 0;

  for (i = 0; i < n; i++) was[i] = setting_live(i);
  memset(&st, 0, sizeof(st));

  widget_list(&widgets[0], 0, 0, 1, 1, n, opt_row, &st);
  widget_label(&widgets[1], 0, 1, 1, desc);
  widget_label(&widgets[2], 0, 2, 1,
               "Space/\xe2\x86\x90\xe2\x86\x92 change  Enter keep  Ctrl-S save  Esc undo all");
  widgets[0].sel = 0;
  desc[0] = '\0';

  depth = layer_count();
  memset(&l, 0, sizeof(l));
  li = layer_push(&l);
  if (li < 0) {
    free(was);
    return;
  }

  for (;;) {
    struct layer *lp;
    const char *sec, *name, *comment;
    int cols, rows, list_h, c, ns, is_bool, lo, hi, cur;

    screen_sync_size();
    cols = grid_cols() - 8;
    if (cols > 64) cols = 64;
    if (cols < 24) cols = 24;
    rows = n + 2 + 2;                 /* the rows, the frame, and two lines below */
    if (rows > grid_rows() - 2) rows = grid_rows() - 2;
    if (rows < 6) rows = 6;
    list_h = rows - 2 - 2;
    if (list_h < 1) list_h = 1;

    st.width = cols - 4;
    widgets[0].w = cols - 4;
    widgets[0].h = list_h;
    widgets[0].count = n;
    widgets[1].y = list_h;
    widgets[1].w = cols - 4;
    widgets[2].y = list_h + 1;
    widgets[2].w = cols - 4;
    widget_list_clamp(&widgets[0]);

    cur = widgets[0].sel;
    /* The section as mollyrc spells it, so the line also says where to find
     * the setting in the file. */
    if (setting_info(cur, &sec, &name, &is_bool, &lo, &hi, &comment))
      snprintf(desc, sizeof(desc), "[%s] %s", sec, comment);

    dialog_init(&d, "Options", widgets, 3, cols - 2, rows - 2);
    lp = layer_get(li);
    dialog_layout(&d, lp);
    editor_refresh_screen();

    c = ui_wait_key();
    if (c == WINCH_KEY) continue;

    if (c == MOUSE_KEY) {
      struct mouse_event ev;
      input_last_mouse(&ev);
      if (ev.type == MOUSE_WHEEL_UP || ev.type == MOUSE_WHEEL_DOWN) {
        commit_typed(&st);
        widget_list_move(&widgets[0], ev.type == MOUSE_WHEEL_UP ? -3 : 3);
      } else if (ev.type == MOUSE_PRESS && ev.button == MOUSE_BUTTON_LEFT) {
        /* A click selects a row, and on an on/off row it flips it too: the
         * click was on the thing to change, as on a checkbox. */
        int k = ev.y - (lp->y + 1 + widgets[0].y);
        if (k >= 0 && k < widgets[0].h && widgets[0].top + k < n) {
          commit_typed(&st);
          widgets[0].sel = widgets[0].top + k;
          if (row_is_bool(widgets[0].sel)) nudge(widgets[0].sel, 0);
        }
      }
      continue;
    }

    if (c == '\x1b' || c == CTRL_KEY('c')) break;
    if (c == '\r') { commit_typed(&st); keep = 1; break; }
    if (c == CTRL_KEY('s')) { commit_typed(&st); keep = save = 1; break; }

    if (c >= '0' && c <= '9' && !row_is_bool(cur)) {
      size_t len;
      if (st.typed[0] != '\0' && st.sel != cur) commit_typed(&st);
      len = strlen(st.typed);
      st.sel = cur;
      if (len + 1 < sizeof(st.typed) && len < 7) {
        st.typed[len] = (char)c;
        st.typed[len + 1] = '\0';
      }
      continue;
    }
    if ((c == BACKSPACE || c == CTRL_KEY('h')) && st.typed[0] != '\0') {
      st.typed[strlen(st.typed) - 1] = '\0';
      continue;
    }
    if (c == ' ') {
      if (row_is_bool(cur)) nudge(cur, 0);
      continue;
    }
    if (c == ARROW_LEFT || c == ARROW_RIGHT) {
      commit_typed(&st);
      nudge(cur, c == ARROW_RIGHT ? 1 : -1);
      continue;
    }

    ns = 0;
    if (c == ARROW_DOWN || c == CTRL_KEY('n')) ns = 1;
    else if (c == ARROW_UP || c == CTRL_KEY('p')) ns = -1;
    else if (c == PAGE_DOWN) ns = list_h;
    else if (c == PAGE_UP) ns = -list_h;
    else if (c == HOME_KEY) ns = -n;
    else if (c == END_KEY) ns = n;
    else continue;
    commit_typed(&st);
    widget_list_move(&widgets[0], ns);
  }

  layer_pop_to(depth);

  if (!keep) {
    for (i = 0; i < n; i++)
      if (setting_live(i) != was[i]) setting_set(i, was[i]);
    editor_set_status_message("Options unchanged");
  } else if (save) {
    const char *err = NULL;
    const char *path = config_path();
    if (config_write(path, &err) == 0)
      editor_set_status_message("Options saved to %s", path);
    else
      editor_set_status_message("Options kept, but not saved: %s",
                                err != NULL ? err : "unknown error");
  } else {
    editor_set_status_message("Options kept for this session (Ctrl-S in Options saves them)");
  }
  free(was);
  screen_force_repaint();
}
