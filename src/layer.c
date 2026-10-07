/* layer.c -- the screen as a stack of regions, drawn bottom to top.
 *
 * Molly's drawing used to be a fixed sequence: the text rows, then the status bar,
 * then the message bar, each working out its own row number from V.screenrows. That
 * is three copies of the same arithmetic and no way to put anything on top, which
 * is a problem for a menu bar, a drop-down, a dialog, a completion popup and a
 * split -- all of which are "a rectangle that draws itself, over whatever is
 * underneath".
 *
 * So the screen is a stack of layers. Each knows its bounds and how to draw itself;
 * drawing walks the stack from the bottom, and overlap is nothing more than draw
 * order. The earlier roadmap talked about saving and restoring what a window covers,
 * which was left over from thinking in byte strings: the grid is rebuilt from blank
 * every frame, so there is nothing underneath to preserve.
 *
 * The bottom layers are molly's chrome -- text area, status bar, message bar -- and
 * they are pushed once at startup, with the layout pass adjusting their bounds as
 * the terminal resizes or the menu bar comes and goes. Everything above is
 * transient and pushed by whatever is showing it.
 *
 * Vocabulary, since three words here are close together: a *buffer* is an open
 * file, a *view* is a window onto one (struct view), a *layer* is a region of the
 * screen, and a *widget* will be a control inside a layer. */

#include "molly.h"

#define LAYER_MAX 16

static struct layer layers[LAYER_MAX];
static int layer_n;

/* Push a layer on top. Returns its index, or -1 if the stack is full -- which the
 * caller must handle rather than assume, since menus nest and dialogs stack. */
int layer_push(const struct layer *l) {
  if (layer_n >= LAYER_MAX) return -1;
  layers[layer_n] = *l;
  return layer_n++;
}

void layer_pop(void) {
  if (layer_n > 0) layer_n--;
}

/* Drop back to a remembered depth. A modal loop records layer_count() on the way
 * in and calls this on the way out, so an early exit cannot leave its layers
 * behind. */
void layer_pop_to(int count) {
  if (count < 0) count = 0;
  if (count < layer_n) layer_n = count;
}

int layer_count(void) {
  return layer_n;
}

struct layer *layer_get(int i) {
  if (i < 0 || i >= layer_n) return NULL;
  return &layers[i];
}

/* Move or resize a layer already on the stack. The layout pass uses this on the
 * chrome rather than popping and re-pushing it every frame. */
void layer_set_bounds(int i, int x, int y, int w, int h) {
  struct layer *l = layer_get(i);
  if (l == NULL) return;
  l->x = x;
  l->y = y;
  l->w = w;
  l->h = h;
}

/* Draw every layer, bottom first.
 *
 * A layer with no draw function is a placeholder -- a reserved slot, or one whose
 * content is not ready -- and is skipped rather than treated as an error. So is one
 * with no area: that is how the menu bar is hidden, and how hit-testing already
 * treated it, so drawing had better agree. Before it did, the hidden band still
 * painted itself over the first line of the file. */
void layer_draw_all(void) {
  int i;
  for (i = 0; i < layer_n; i++) {
    if (layers[i].draw == NULL) continue;
    if (layers[i].w <= 0 || layers[i].h <= 0) continue;
    layers[i].draw(&layers[i]);
  }
}

/* The topmost layer covering a point, or -1. A click belongs to whatever is visibly
 * on top at that spot, which is the stack walked from the end. */
int layer_at(int x, int y) {
  int i;
  for (i = layer_n - 1; i >= 0; i--) {
    const struct layer *l = &layers[i];
    if (x >= l->x && x < l->x + l->w && y >= l->y && y < l->y + l->h) return i;
  }
  return -1;
}

/* Offer an event to the layers, topmost first, stopping at the first that takes it.
 *
 * A layer without a mouse handler still blocks whatever is underneath it when the
 * event falls inside its bounds: a click on a dialog that ignores clicks must not
 * also move the cursor in the text behind it. Returns non-zero if the event was
 * consumed. */
int layer_dispatch_mouse(const struct mouse_event *ev) {
  int i;
  for (i = layer_n - 1; i >= 0; i--) {
    struct layer *l = &layers[i];
    if (ev->x < l->x || ev->x >= l->x + l->w) continue;
    if (ev->y < l->y || ev->y >= l->y + l->h) continue;
    if (l->mouse != NULL && l->mouse(l, ev)) return 1;
    return 0;  /* inside an uninterested layer: swallowed, not passed down */
  }
  return 0;
}
