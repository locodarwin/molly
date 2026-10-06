/* test_loop.c -- unit tests for the event loop.
 *
 * The editor registers exactly one source today, the keyboard, so nothing in the
 * pty tests would notice if waiting on a second one were broken. The language
 * server and the integrated terminal will both depend on it, and a bug found then
 * would look like "diagnostics sometimes do not arrive" rather than like a loop
 * bug. So the loop is tested directly, against pipes.
 *
 * Linked against molly's objects (all but main.o) because loop.c calls die() on a
 * failed wait, and die() belongs to the terminal code. */

#include "molly.h"
#include <poll.h>

static int failures;

static void check(const char *label, long got, long want) {
  if (got == want) {
    printf("  pass  %s\n", label);
  } else {
    printf("  FAIL  %s\n          want: %ld\n          got:  %ld\n",
           label, want, got);
    failures++;
  }
}

/* What the handlers record, kept in parallel arrays indexed by the small integer
 * each source is registered with -- this is a test, not an interface. */
static int fired[4];
static int bytes_read[4];
static int handler_fd[4];

static void drain_handler(void *user) {
  int which = *(int *)user;
  char buf[64];
  ssize_t n;
  fired[which]++;
  n = read(handler_fd[which], buf, sizeof(buf));
  if (n > 0) bytes_read[which] += (int)n;
}

int main(void) {
  int idx[4] = {0, 1, 2, 3};
  int a[2], b[2];
  int ida, idb, r;

  printf("\nevent loop: waiting on several sources\n");

  check("no sources to begin with", loop_source_count(), 0);

  if (pipe(a) != 0 || pipe(b) != 0) {
    fprintf(stderr, "pipe failed\n");
    return 2;
  }
  handler_fd[0] = a[0];
  handler_fd[1] = b[0];

  ida = loop_add(a[0], drain_handler, &idx[0]);
  idb = loop_add(b[0], drain_handler, &idx[1]);
  check("two sources registered", loop_source_count(), 2);
  check("ids are distinct", ida != idb, 1);

  /* Nothing written: the wait times out, reports nothing, and calls no handler. */
  r = loop_wait(10);
  check("an idle wait reports nothing", r, 0);
  check("an idle wait calls no handler", fired[0] + fired[1], 0);

  /* One source ready. */
  if (write(a[1], "xyz", 3) != 3) return 2;
  r = loop_wait(500);
  check("one ready source is dispatched", fired[0], 1);
  check("the other source is left alone", fired[1], 0);
  check("the handler got its bytes", bytes_read[0], 3);
  check("a non-keyboard source does not report stdin", r & LOOP_STDIN, 0);

  /* Both ready in the same wait: both handlers run before loop_wait returns,
   * which is the property an editor with a language server and a terminal
   * depends on. */
  fired[0] = fired[1] = 0;
  if (write(a[1], "1", 1) != 1 || write(b[1], "22", 2) != 2) return 2;
  r = loop_wait(500);
  check("both ready sources are dispatched in one wait",
        fired[0] + fired[1], 2);
  check("each got its own bytes", bytes_read[1], 2);

  /* A removed source is no longer waited on, even with data pending. */
  fired[0] = fired[1] = 0;
  loop_remove(idb);
  check("removing drops the count", loop_source_count(), 1);
  if (write(b[1], "ignored", 7) != 7) return 2;
  r = loop_wait(50);
  check("a removed source is not dispatched", fired[1], 0);

  /* A writer that goes away must be reported, not silently waited on forever:
   * a closed pipe polls ready at EOF, and a handler never told would spin. */
  fired[0] = 0;
  close(a[1]);
  r = loop_wait(500);
  check("a closed writer wakes the handler", fired[0] > 0, 1);

  /* The table is finite and says so rather than overflowing. */
  loop_cleanup();
  check("cleanup clears the table", loop_source_count(), 0);
  {
    int i, last = 0, extra;
    for (i = 0; i < 64; i++) {
      int id = loop_add(b[0], drain_handler, &idx[1]);
      if (id < 0) break;
      last++;
    }
    extra = loop_add(b[0], drain_handler, &idx[1]);
    check("a full table refuses rather than overflows", extra, -1);
    check("the table filled to its stated size", last, loop_source_count());
  }
  loop_cleanup();

  /* Keyboard input is reported rather than dispatched. Standard input is a pipe
   * here, so writing to it is the same thing as a keystroke as far as the loop
   * is concerned. */
  {
    int kb[2];
    fired[0] = fired[1] = 0;  /* earlier cases left these set */
    if (pipe(kb) != 0) return 2;
    if (dup2(kb[0], STDIN_FILENO) < 0) return 2;
    r = loop_wait(10);
    check("no keyboard input reports nothing", r & LOOP_STDIN, 0);
    if (write(kb[1], "k", 1) != 1) return 2;
    r = loop_wait(500);
    check("keyboard input is reported", (r & LOOP_STDIN) != 0, 1);
    check("keyboard input is not dispatched to a handler", fired[0], 0);
  }

  close(b[0]);
  close(b[1]);
  close(a[0]);

  printf("\n%s\n", failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
