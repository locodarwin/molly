/* window.c -- the tree of windows, and which view has the keyboard.
 *
 * Molly had one window from kilo onwards, and `struct view` was shaped in Phase
 * 0 for the day it stopped being one. This is that day. What the shaping bought
 * is that almost nothing outside this file changed: `V` is a macro for the
 * focused view, exactly as `B` is a macro for the current view's buffer, so the
 * three hundred-odd `V.something` references across the editor went on meaning
 * "the window I am working in" without being touched.
 *
 * What did have to change were the globals that were really per-window state in
 * disguise. The roadmap named the gutter, the horizontal scroll and the
 * wide-character alignment, and all three turned out to be fine: they measure
 * against TEXT_COLS(), which reads V, which is now the right view. The two that
 * were genuinely wrong were view.c's pair of scroll-memory statics -- scrolling
 * one window would have told the next one its cursor had moved -- and the menu
 * and tab bar flags, which were fields on the view although the rows they take
 * are above every window. Those moved to E.
 *
 * The screen is a binary tree: a leaf shows a view, a WIN_COLS node puts its
 * children side by side with a separator column between them, a WIN_ROWS node
 * stacks them. Each window's last row is its own status bar, which is why one
 * window looks exactly as it did before this file existed -- the bar lands on
 * the same row it always did -- and why a stacked split needs no separator: the
 * upper window's status bar already is one.
 *
 * Layout is recomputed from the terminal size every frame rather than stored
 * and patched up. A resize is then not a special case, and neither is a split
 * or a close: all three end with the same walk over the same tree.
 */

#include "molly.h"

/* A window needs a row of text and a row of status bar to be worth anything,
 * and enough columns that the text in it is readable rather than a stack of
 * single letters. Below these a split is refused rather than made and then
 * drawn as a smear. */
#define WIN_MIN_ROWS 2
#define WIN_MIN_COLS 8

/* The ceiling on splits. Each window takes at least WIN_MIN_ROWS rows and
 * WIN_MIN_COLS columns, so a terminal big enough for sixty-four of them is
 * already bigger than any screen; the cap is here so that the leaf array below
 * is a fixed size and no walk of the tree can allocate. */
#define WINDOWS_MAX 64

/* The separator's glyph: U+2502, the same light vertical the line-number gutter
 * draws, because it is the same kind of line and a second one would be a second
 * thing to recognise. */
#define SPLIT_BAR "\xe2\x94\x82"

/* The view state.c defines statically. Captured rather than exported, because
 * knowing which view not to free is this file's business and nobody else's. */
static struct view *view_first;

/* ---- the current view ---------------------------------------------------- */

/* Make `v` the current view and return the one that was.
 *
 * The idiom is "do this as if that window had the keyboard", and the drawing
 * walk, the layout walk and the mouse handler all use it: each of them needs to
 * run code that is written against V -- the row renderer, the gutter width, the
 * click-to-cursor arithmetic -- for a window that is not the focused one. The
 * alternative was threading a view argument through every one of those, which
 * is a change to the code the piece-table rewrite replaces anyway.
 *
 * Callers restore what they are given, always, including on the paths that
 * return early. */
struct view *view_select(struct view *v) {
  struct view *was = E.curview;
  if (v != NULL) E.curview = v;
  return was;
}

/* ---- views --------------------------------------------------------------- */

/* A second view onto whatever `src` is showing, at the same place in it.
 *
 * Splitting should show you the same text twice, not drop you at the top of the
 * file in the new half: the cursor, the scroll offsets and the per-window
 * toggles are all copied. The two views then diverge the moment either one
 * moves, which is the point of having two. */
static struct view *view_new_like(const struct view *src) {
  struct view *v = xmalloc(sizeof *v);
  struct view *was;

  memset(v, 0, sizeof *v);
  v->buf = src->buf;
  v->drowoff = src->drowoff;
  v->coloff = src->coloff;
  v->linenum = src->linenum;
  v->syntax = src->syntax;
  v->wrap = src->wrap;
  v->last_scroll_cx = -1;
  v->last_scroll_cy = -1;

  /* cursors_init() allocates for the current view, so it is lent this one. */
  was = view_select(v);
  cursors_init();
  view_select(was);
  v->cursors[0] = src->cursors[src->primary];
  return v;
}

static void view_free(struct view *v) {
  if (v == NULL) return;
  free(v->cursors);
  v->cursors = NULL;
  v->ncursors = 0;
  v->cursorcap = 0;
  v->primary = 0;
  if (v != view_first) free(v);
}

/* ---- the tree ------------------------------------------------------------ */

static struct window *win_leaf(struct view *v) {
  struct window *w = xmalloc(sizeof *w);
  memset(w, 0, sizeof *w);
  w->kind = WIN_LEAF;
  w->view = v;
  return w;
}

/* The leaves in screen order: left before right, top before bottom. That is
 * what makes "next window" read as moving across the screen rather than as
 * walking a data structure, and it falls out of visiting kid[0] first. */
static int collect(struct window *w, struct window **out, int n) {
  if (w == NULL) return n;
  if (w->kind == WIN_LEAF) {
    if (n < WINDOWS_MAX) out[n] = w;
    return n + 1;
  }
  n = collect(w->kid[0], out, n);
  return collect(w->kid[1], out, n);
}

static int leaves(struct window **out) {
  int n = collect(E.winroot, out, 0);
  return (n > WINDOWS_MAX) ? WINDOWS_MAX : n;
}

int window_count(void) {
  struct window *l[WINDOWS_MAX];
  return leaves(l);
}

struct view *window_view_at(int i) {
  struct window *l[WINDOWS_MAX];
  int n = leaves(l);
  if (i < 0 || i >= n) return NULL;
  return l[i]->view;
}

int window_index_of(const struct view *v) {
  struct window *l[WINDOWS_MAX];
  int n = leaves(l), i;
  for (i = 0; i < n; i++)
    if (l[i]->view == v) return i;
  return -1;
}

/* The leaf the keyboard is in. Derived from E.curview rather than stored, for
 * the reason buf_current() is derived from V.buf: two records of one fact
 * disagree the first time something closes from under one of them. */
static struct window *current_leaf(void) {
  struct window *l[WINDOWS_MAX];
  int n = leaves(l), i;
  for (i = 0; i < n; i++)
    if (l[i]->view == E.curview) return l[i];
  return (n > 0) ? l[0] : NULL;
}

static struct window *first_leaf(struct window *w) {
  while (w != NULL && w->kind != WIN_LEAF) w = w->kid[0];
  return w;
}

/* ---- focus --------------------------------------------------------------- */

void window_focus(int i) {
  struct view *v = window_view_at(i);
  if (v == NULL || v == E.curview) return;
  E.curview = v;
  /* The gutter, the tab bar's highlight and the status bar all change, and the
   * cell cache cannot see that the *meaning* of unchanged cells has. */
  screen_force_repaint();
}

/* Move the keyboard `delta` windows along, wrapping. Wrapping rather than
 * stopping at the end, because with two windows -- which is the common case --
 * one key that goes back and forth is worth more than two that each only work
 * in one direction. */
void window_move_focus(int delta) {
  int n = window_count();
  int i = window_index_of(E.curview);
  if (n < 2 || i < 0) return;
  i = (i + delta) % n;
  if (i < 0) i += n;
  window_focus(i);
}

/* ---- splitting and closing ----------------------------------------------- */

/* Split the current window, putting the new one right of or below it.
 *
 * Returns 1 if it happened. The refusals are all about room: a window below the
 * minimum is not a window, and a split that produced one would have to be
 * undone by the user rather than by the editor. The message says which way it
 * would not fit, because "not enough room" with two directions available tells
 * the user nothing about which one to try. */
int window_split(int kind) {
  struct window *cur = current_leaf();
  struct window *node, *leaf, *parent;
  struct view *v;
  int slot;

  if (cur == NULL) return 0;
  if (window_count() >= WINDOWS_MAX) {
    editor_set_status_message("Already %d windows; that is the limit.",
                              WINDOWS_MAX);
    return 0;
  }
  if (kind == WIN_COLS) {
    if (cur->w < 2 * WIN_MIN_COLS + 1) {
      editor_set_status_message("Not wide enough to split this window.");
      return 0;
    }
  } else {
    if (cur->h < 2 * WIN_MIN_ROWS) {
      editor_set_status_message("Not tall enough to split this window.");
      return 0;
    }
  }

  /* The slot the current window occupies in its parent, read before anything is
   * rewired -- afterwards cur->parent is the new node and the answer is gone. */
  parent = cur->parent;
  slot = (parent != NULL && parent->kid[1] == cur) ? 1 : 0;

  v = view_new_like(E.curview);
  node = xmalloc(sizeof *node);
  memset(node, 0, sizeof *node);
  leaf = win_leaf(v);

  node->kind = kind;
  node->frac = 500;
  node->parent = parent;
  node->kid[0] = cur;
  node->kid[1] = leaf;
  cur->parent = node;
  leaf->parent = node;
  if (parent == NULL) E.winroot = node;
  else parent->kid[slot] = node;

  /* The keyboard follows into the new window. It is showing the same text at
   * the same place, so nothing is lost, and it is what says which half is new. */
  E.curview = v;
  window_layout();
  screen_force_repaint();
  return 1;
}

/* Close the current window. Returns 1 if it happened.
 *
 * The parent goes with it: a split node with one child is not a split, so the
 * sibling takes the parent's place in the tree and its share of the screen.
 * That is what makes closing the second of two windows restore exactly the
 * screen the first split produced. */
int window_close_current(void) {
  struct window *cur = current_leaf();
  struct window *parent, *sibling, *grand;
  struct view *dying;
  int slot;

  if (cur == NULL || cur->parent == NULL) {
    editor_set_status_message("This is the only window.");
    return 0;
  }

  parent = cur->parent;
  sibling = (parent->kid[0] == cur) ? parent->kid[1] : parent->kid[0];
  grand = parent->parent;
  slot = (grand != NULL && grand->kid[1] == parent) ? 1 : 0;

  sibling->parent = grand;
  if (grand == NULL) E.winroot = sibling;
  else grand->kid[slot] = sibling;

  /* The keyboard moves before the view is freed, so that V is a live view for
   * every instruction in between -- including any that die() might reach. */
  dying = cur->view;
  E.curview = first_leaf(sibling)->view;
  free(parent);
  free(cur);
  view_free(dying);

  window_layout();
  screen_force_repaint();
  return 1;
}

/* Everything in the tree except `keep`, which is left detached. */
static void free_tree_except(struct window *w, struct window *keep) {
  if (w == NULL || w == keep) return;
  if (w->kind == WIN_LEAF) {
    view_free(w->view);
    free(w);
    return;
  }
  free_tree_except(w->kid[0], keep);
  free_tree_except(w->kid[1], keep);
  free(w);
}

/* Close every window but this one. */
void window_only(void) {
  struct window *cur = current_leaf();
  if (cur == NULL || cur->parent == NULL) return;
  free_tree_except(E.winroot, cur);
  cur->parent = NULL;
  E.winroot = cur;
  window_layout();
  screen_force_repaint();
}

/* ---- buffers closing under a window -------------------------------------- */

/* Point every view showing `from` at `to` instead.
 *
 * This is the one thing splits add to closing a buffer, and it is the kind of
 * bug that does not announce itself: a second window left pointing at a freed
 * buffer draws whatever is in that memory now, days later, rather than failing
 * an assertion. buf_show() does the position restoring, so each view is lent
 * the keyboard for the length of one call rather than having its fields set
 * here -- there is one copy of "arrive in a buffer at a sensible place" and
 * this is not a second. */
void window_repoint_views(struct buffer *from, struct buffer *to) {
  struct window *l[WINDOWS_MAX];
  int n = leaves(l), i, index = buf_index_of(to);
  struct view *was = E.curview;

  if (index < 0) return;
  for (i = 0; i < n; i++) {
    if (l[i]->view->buf != from) continue;
    E.curview = l[i]->view;
    buf_show(index);
  }
  E.curview = was;
}

/* ---- layout -------------------------------------------------------------- */

/* Give one leaf its share of the screen.
 *
 * The window's last row is its status bar, so the view's text area is one row
 * shorter than the window. Sizes are floored at one: a window squeezed past its
 * minimum by a resize is not drawn at all (see draw_node), but the view inside
 * it still has its scroll clamped and its gutter measured every frame, and both
 * divide by the width. */
static void layout_leaf(struct window *w) {
  struct view *v = w->view;
  struct view *was;

  v->screenx = w->x;
  v->screeny = w->y;
  v->screencols = (w->w > 1) ? w->w : 1;
  v->screenrows = (w->h > 2) ? w->h - 1 : 1;

  was = view_select(v);
  v->gutter_w = screen_gutter_width();
  view_select(was);
}

static void layout_node(struct window *w, int x, int y, int cw, int ch) {
  int avail, cut;

  if (w == NULL) return;
  w->x = x;
  w->y = y;
  w->w = cw;
  w->h = ch;

  if (w->kind == WIN_LEAF) {
    layout_leaf(w);
    return;
  }

  if (w->kind == WIN_COLS) {
    avail = cw - 1;                 /* the separator column is not either child's */
    if (avail < 2) {
      /* Narrower than two columns of window: the second child gets nothing and
       * goes undrawn. Reachable only by shrinking the terminal under a split
       * that was legal when it was made. */
      layout_node(w->kid[0], x, y, (avail > 0) ? avail : 0, ch);
      layout_node(w->kid[1], x + cw, y, 0, ch);
      return;
    }
    cut = (avail * w->frac + 500) / 1000;
    if (cut < WIN_MIN_COLS) cut = WIN_MIN_COLS;
    if (cut > avail - WIN_MIN_COLS) cut = avail - WIN_MIN_COLS;
    if (cut < 1) cut = avail / 2;   /* too narrow for two minimums: halve it */
    if (cut < 1) cut = 1;
    if (cut > avail - 1) cut = avail - 1;
    layout_node(w->kid[0], x, y, cut, ch);
    layout_node(w->kid[1], x + cut + 1, y, avail - cut, ch);
    return;
  }

  avail = ch;                       /* stacked: no separator row to pay for */
  if (avail < 2) {
    layout_node(w->kid[0], x, y, cw, (avail > 0) ? avail : 0);
    layout_node(w->kid[1], x, y + ch, cw, 0);
    return;
  }
  cut = (avail * w->frac + 500) / 1000;
  if (cut < WIN_MIN_ROWS) cut = WIN_MIN_ROWS;
  if (cut > avail - WIN_MIN_ROWS) cut = avail - WIN_MIN_ROWS;
  if (cut < 1) cut = avail / 2;
  if (cut < 1) cut = 1;
  if (cut > avail - 1) cut = avail - 1;
  layout_node(w->kid[0], x, y, cw, cut);
  layout_node(w->kid[1], x, y + cut, cw, avail - cut);
}

/* Share out a given rect. Separate from window_layout() because the rect and
 * the sharing-out are two different questions, and only the first one needs a
 * terminal: a test can hand this the screen it wants to reason about without
 * standing up a pty. */
void window_layout_rect(int x, int y, int w, int h) {
  layout_node(E.winroot, x, y, w, h);
}

void window_layout(void) {
  int x, y, w, h;
  screen_window_area(&x, &y, &w, &h);
  window_layout_rect(x, y, w, h);
}

/* ---- scrolling ----------------------------------------------------------- */

/* Settle every window's scroll, and lex what each of them is about to show.
 *
 * Every window, not just the focused one: an edit in one window moves the text
 * under another that is showing the same buffer, and a window nobody has
 * touched still has to follow a resize. Before anything is painted rather than
 * inside the draw walk, because lexing allocates and because a row's colouring
 * depends on the row above it. */
void window_scroll_all(void) {
  struct window *l[WINDOWS_MAX];
  int n = leaves(l), i;
  struct view *was = E.curview;

  for (i = 0; i < n; i++) {
    E.curview = l[i]->view;
    /* Before the scroll, which measures the cursor against the render string
     * this may replace. The backstop for a buffer becoming current without its
     * name or the default having changed -- switching tabs, or a split showing
     * a second filetype. A no-op unless the width actually moved. */
    indent_sync();
    editor_scroll();
    syntax_ensure_view();
  }
  E.curview = was;
}

/* ---- drawing ------------------------------------------------------------- */

/* The column between two windows side by side.
 *
 * Over the whole node, status rows included, so the two bars read as two bars
 * rather than as one long one with a change of shade in the middle. A stacked
 * split needs nothing here: the upper window's status bar is already a line
 * across the screen, which is why WIN_ROWS pays no row for a separator and
 * WIN_COLS pays a column. */
static void draw_separator(const struct window *w) {
  int x = w->kid[0]->x + w->kid[0]->w;
  int y;
  if (w->kid[0]->w < 1 || x >= w->x + w->w) return;
  grid_role(ROLE_SPLIT);
  for (y = w->y; y < w->y + w->h; y++) grid_puts(y, x, SPLIT_BAR, 3);
  grid_role(ROLE_TEXT);
}

static void draw_node(struct window *w, const struct view *focus) {
  if (w == NULL) return;

  if (w->kind == WIN_LEAF) {
    struct view *was;
    /* Squeezed past its minimum by a resize: left blank rather than drawn into
     * a rect it does not fit. The window the keyboard is in is still legible,
     * which is the property that matters while someone drags a corner. */
    if (w->w < 1 || w->h < 2) return;
    was = view_select(w->view);
    screen_draw_text_area();
    screen_draw_status_bar(w->view == focus);
    view_select(was);
    return;
  }

  if (w->kind == WIN_COLS) draw_separator(w);
  draw_node(w->kid[0], focus);
  draw_node(w->kid[1], focus);
}

void window_draw(const struct layer *self) {
  (void)self;
  /* Which view has the keyboard is read once, before the walk: the walk lends
   * each window the keyboard in turn so that the code written against V draws
   * the right one, so asking inside it would answer "yes" every time. */
  draw_node(E.winroot, E.curview);
}

/* ---- the mouse ----------------------------------------------------------- */

/* The window a press landed in, remembered for the drag that follows.
 *
 * A selection started in one window and dragged into another belongs to the
 * window it started in -- the same rule as every list and text field, and
 * without it dragging past the edge would hand the drag to the neighbour and
 * leave two half-selections. */
static struct view *drag_view;

static struct window *leaf_at(int x, int y) {
  struct window *l[WINDOWS_MAX];
  int n = leaves(l), i;
  for (i = 0; i < n; i++) {
    struct window *w = l[i];
    if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h)
      return w;
  }
  return NULL;
}

int window_mouse(const struct layer *self, const struct mouse_event *ev) {
  struct window *w;
  struct view *was;
  int on_status;

  /* The layer's own rect is not consulted: it is the whole window area, and
   * which window inside it the event landed in is the tree's answer. */
  (void)self;

  if (ev->type == MOUSE_DRAG && drag_view != NULL) {
    was = view_select(drag_view);
    screen_text_mouse(ev);
    view_select(was);
    return 1;
  }

  w = leaf_at(ev->x, ev->y);
  if (w == NULL) return 1;     /* the separator, or a squeezed-out window */

  /* A click anywhere in a window moves the keyboard there, including on its
   * status bar -- which is the one row of it that is not text, and the obvious
   * place to click when the point is to focus the window rather than to put the
   * cursor somewhere in it. */
  if (ev->type == MOUSE_PRESS) {
    window_focus(window_index_of(w->view));
    drag_view = w->view;
  }

  on_status = (ev->y == w->y + w->h - 1);
  if (on_status) return 1;

  was = view_select(w->view);
  screen_text_mouse(ev);
  view_select(was);
  return 1;
}

/* ---- lifetime ------------------------------------------------------------ */

/* One window, around the view state.c defines. Called before buflist_init(),
 * because that points the current view at the first buffer and there has to be
 * a current view for it to point. */
void window_init(void) {
  /* Idempotent. Calling it a second time drops the tree the first call made,
   * which is what lets a test stand the editor up more than once in one
   * process -- and the editor itself only ever calls it once. */
  if (E.winroot != NULL) window_cleanup();
  view_first = E.curview;
  view_first->last_scroll_cx = -1;
  view_first->last_scroll_cy = -1;
  /* A view always has at least one cursor, so the first view gets its list
   * here rather than from a separate call in init_editor. Every view after this
   * one gets it from view_new_like(), which means there is no order of startup
   * in which V has a buffer but nowhere to put the cursor. */
  cursors_init();
  E.winroot = win_leaf(view_first);
  drag_view = NULL;
}

void window_cleanup(void) {
  if (E.winroot != NULL) {
    free_tree_except(E.winroot, NULL);
    E.winroot = NULL;
  }
  E.curview = view_first;
  drag_view = NULL;
}
