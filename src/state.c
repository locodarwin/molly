/* state.c -- the editor's state, defined once.
 *
 * These live in a file of their own rather than in main.c so that nothing has to
 * link main.c to get at them: the test harness and any future tool can pull in
 * the modules it needs.
 *
 * The buffer that used to live here is gone: there is a list of them now, held
 * in E and managed by buflist.c, and `B` is a macro for whichever one the
 * current view is looking at. One view still, which is what splits will grow. */

#include "molly.h"

struct editorConfig E;
struct view V;
