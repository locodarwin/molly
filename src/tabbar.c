/* tabbar.c -- the row of tabs, one per open buffer.
 *
 * The status bar's "[2/3]" said how many buffers there were and which one you
 * were in; it could not say what the others were called. That is the whole job
 * here, and it is why the tab bar is worth a row: with three files open, the
 * question is not "how many" but "which".
 *
 * Shown only when there is more than one buffer. A single tab is a row of chrome
 * spent restating what the status bar already says, and molly is meant to work
 * on a 24-row terminal -- so the row appears when it earns itself and goes away
 * again when the second buffer closes. `[view] tab bar = off` declines it
 * entirely.
 *
 * Two things here are the same shape as menu.c, deliberately: the drawn extent
 * of each tab is recorded as it is drawn, so hit-testing a click is a lookup
 * rather than a second layout pass that could disagree with the first; and
 * everything is measured in display columns rather than bytes, because a file
 * called "日本語.txt" is nine bytes and five columns and a bar built on strlen
 * would be wrong by four.
 */

#include "molly.h"

/* Where each tab landed, for the mouse. Parallel arrays indexed by buffer, with
 * -1 for a buffer that is scrolled out of view -- a click can only be on a tab
 * that was drawn. */
#define TABS_MAX 256
static int tab_x[TABS_MAX];
static int tab_w[TABS_MAX];
static int tab_first;   /* first buffer drawn, for the scroll indicators */
static int tab_last;    /* last buffer drawn, inclusive */

/* The arrows that say there are tabs off the side. One column each. */
#define TAB_LEFT_ARROW  "\xe2\x80\xb9"   /* U+2039 */
#define TAB_RIGHT_ARROW "\xe2\x80\xba"   /* U+203A */

/* A tab's text, without its surrounding spaces.
 *
 * The basename, because the path is what the status bar is for: with
 * src/buffer.c and tests/buffer.c open, four tabs' worth of "src/" tells the
 * user nothing and the two names they need are off the right-hand edge. An
 * unsaved buffer takes a trailing asterisk, which is the one marker every editor
 * agrees on. */
static void tab_label(const struct buffer *b, char *dst, size_t dsz) {
  const char *name = buf_name(b);
  if (b->filename != NULL) {
    const char *slash = strrchr(b->filename, '/');
    name = (slash != NULL && slash[1] != '\0') ? slash + 1 : b->filename;
  }
  snprintf(dst, dsz, "%s%s", name, b->dirty ? "*" : "");
}

/* Columns a tab occupies: its text plus a space on each side. */
static int tab_cols(const struct buffer *b) {
  char label[256];
  tab_label(b, label, sizeof(label));
  return utf8_str_cols(label) + 2;
}

/* Does the tab bar have a row this frame? */
int tabbar_rows(void) {
  return (E.tabbar && buf_count() > 1) ? 1 : 0;
}

/* Which buffer to start drawing from, so that the current one is visible.
 *
 * Scrolled rather than shrunk. Twelve files open on an eighty-column terminal
 * cannot all show a readable name, and tabs cut to three characters are worse
 * than tabs you have to scroll: a window around the current buffer keeps the
 * names you are working with legible and says, with an arrow, that the rest are
 * there. The window is widened leftwards from the current tab after the
 * rightwards pass, so that moving to the last buffer fills the row rather than
 * leaving the current tab alone on the left. */
static int tab_scroll_start(int cols) {
  int cur = buf_current();
  int start = cur, end = cur;
  int used = tab_cols(buf_at(cur));

  if (used > cols) return cur;   /* one tab, too wide for the row: show its head */

  for (;;) {
    int grew = 0;
    if (end + 1 < buf_count()) {
      int w = tab_cols(buf_at(end + 1));
      if (used + w <= cols) { used += w; end++; grew = 1; }
    }
    if (start > 0) {
      int w = tab_cols(buf_at(start - 1));
      if (used + w <= cols) { used += w; start--; grew = 1; }
    }
    if (!grew) break;
  }
  return start;
}

void tabbar_draw(const struct layer *self) {
  int x = self->x, y = self->y, w = self->w;
  int i, cur, start;
  int left_arrow, right_arrow;
  int avail, col;

  if (self->h <= 0 || w <= 0) return;
  for (i = 0; i < TABS_MAX; i++) tab_x[i] = -1;
  tab_first = 0;
  tab_last = -1;

  cur = buf_current();
  if (cur < 0) return;

  grid_role(ROLE_TABBAR);
  grid_fill(y, x, w, ' ');

  /* The arrows cost a column each, so whether they are needed changes how much
   * room the tabs have -- which changes whether they are needed. Resolved by
   * deciding on the full width first and then laying out inside what is left;
   * one pass the other way round can oscillate. */
  start = tab_scroll_start(w);
  left_arrow = (start > 0);
  if (left_arrow) {
    start = tab_scroll_start(w - 1);
    left_arrow = (start > 0);
  }
  avail = w - (left_arrow ? 1 : 0);

  col = x + (left_arrow ? 1 : 0);
  tab_first = start;
  for (i = start; i < buf_count() && i < TABS_MAX; i++) {
    char label[256];
    int tw = tab_cols(buf_at(i));
    unsigned char role = (i == cur) ? ROLE_TAB_SELECTED : ROLE_TABBAR;
    int shown, scols;

    /* The last tab may be cut by the right-hand arrow's column; a tab that
     * cannot show at least its first character is not drawn at all. */
    if (col - x + tw > avail) break;

    tab_label(buf_at(i), label, sizeof(label));
    grid_role(role);
    grid_fill(y, col, tw, ' ');
    shown = utf8_fit_cols(label, tw - 2, &scols);
    (void)scols;
    grid_puts(y, col + 1, label, shown);
    tab_x[i] = col;
    tab_w[i] = tw;
    tab_last = i;
    col += tw;
  }

  right_arrow = (tab_last >= 0 && tab_last + 1 < buf_count());
  grid_role(ROLE_TABBAR);
  if (left_arrow) grid_puts(y, x, TAB_LEFT_ARROW, 3);
  if (right_arrow) grid_puts(y, x + w - 1, TAB_RIGHT_ARROW, 3);

  grid_role(ROLE_TEXT);
}

/* A click on a tab switches to it. The arrows are clickable too, and move by one
 * buffer rather than by a row of them: the arrow means "there are more that
 * way", and one step is what makes the next one appear. */
int tabbar_mouse(const struct layer *self, const struct mouse_event *ev) {
  int i;
  if (self->h <= 0 || ev->y != self->y) return 0;
  if (ev->type != MOUSE_PRESS && ev->type != MOUSE_WHEEL_UP &&
      ev->type != MOUSE_WHEEL_DOWN)
    return 1;   /* consumed: a drag on the bar must not reach the text */

  if (ev->type == MOUSE_WHEEL_UP) {
    if (buf_current() > 0) buf_show(buf_current() - 1);
    return 1;
  }
  if (ev->type == MOUSE_WHEEL_DOWN) {
    if (buf_current() + 1 < buf_count()) buf_show(buf_current() + 1);
    return 1;
  }

  for (i = 0; i < buf_count() && i < TABS_MAX; i++) {
    if (tab_x[i] < 0) continue;
    if (ev->x >= tab_x[i] && ev->x < tab_x[i] + tab_w[i]) {
      buf_show(i);
      return 1;
    }
  }

  /* Not on a tab: the arrows, which are the only other thing on the row. */
  if (tab_first > 0 && ev->x == self->x) {
    buf_show(buf_current() - 1 >= 0 ? buf_current() - 1 : 0);
    return 1;
  }
  if (tab_last >= 0 && tab_last + 1 < buf_count() && ev->x == self->x + self->w - 1) {
    buf_show(buf_current() + 1 < buf_count() ? buf_current() + 1 : buf_current());
    return 1;
  }
  return 1;
}
