/* state.c -- the editor's state, defined once.
 *
 * These live in a file of their own rather than in main.c so that nothing has to
 * link main.c to get at them: the test harness and any future tool can pull in
 * the modules it needs. One buffer and one view today; the types are shaped for
 * lists, and when they become lists this is the file that grows them. */

#include "molly.h"

struct editorConfig E;
struct buffer B;
struct view V;
