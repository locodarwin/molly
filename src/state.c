/* state.c -- the editor's state, defined once.
 *
 * These live in a file of their own rather than in main.c so that nothing has to
 * link main.c to get at them: the test harness and any future tool can pull in
 * the modules it needs.
 *
 * Neither of the two things the editor used to keep here is a single object any
 * more. The buffer became a list, held in E and managed by buflist.c, with `B` a
 * macro for whichever one the current view is looking at. The view has now gone
 * the same way: window.c holds a tree of them and `V` is a macro for the one
 * with the keyboard.
 *
 * The first view is still defined statically, here, rather than allocated by
 * window_init(). That is deliberate: E.curview points at it before main runs, so
 * `V` is safe to evaluate from the very first line of startup -- which matters
 * because die() writes recovery files, and it can be reached from inside the
 * allocation that would otherwise have been making the first view. Every window
 * after the first is allocated; window.c is the only thing that needs to know
 * that one of them must not be freed. */

#include "molly.h"

static struct view first_view;

struct editorConfig E = {.curview = &first_view};
