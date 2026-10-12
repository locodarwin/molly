/* loop.c -- waiting on more than the keyboard.
 *
 * Olly's main loop had one input: a blocking read on the terminal. Two of the
 * features molly has committed to cannot live with that. A language server is a
 * child process that sends diagnostics when it feels like it, not when a key is
 * pressed; an integrated terminal is a child whose output has to appear while the
 * user is typing somewhere else. Both need the editor to wait on several things
 * at once, and retrofitting that into a blocking read means unpicking every call
 * site that assumes a keystroke is the only thing that can happen next. So the
 * wait is built now, with one source registered, rather than later with several.
 *
 * The interface is deliberately ours rather than poll()'s. Windows is a maybe on
 * the roadmap, and there a console handle cannot be polled the way a pipe can:
 * the implementation becomes WaitForMultipleObjects, which has different rules
 * about what can be waited on and returns one ready handle rather than a set.
 * Code that calls loop_wait does not have to care, whereas code that called
 * poll() directly would all have to be found and rewritten.
 *
 * The keyboard is reported rather than dispatched. Everything else gets a
 * callback, because its data belongs to whatever registered it, but the editor
 * reads keys synchronously -- editor_read_key consumes a whole escape sequence,
 * and the prompt, help screen and open dialog each run their own key loop -- so
 * the loop's job there is only to say "there is input now". */

#include "molly.h"
#include <poll.h>

/* Three per language server (its input, output and standard error), three for
 * a running filter, and the room the integrated terminal will want. */
#define LOOP_MAX_SOURCES 32

/* A free slot is one with in_use == 0, rather than a sentinel in fd. Static
 * storage zero-initializes, and zero is a perfectly good descriptor -- standard
 * input, in fact -- so a sentinel would have had to be written in before the
 * table could be used, and forgetting to would have made every slot look
 * occupied by stdin. */
struct loop_source {
  int in_use;
  int fd;
  int events;            /* LOOP_READ, LOOP_WRITE, or both */
  loop_handler on_ready;
  void *user;
};

static struct loop_source sources[LOOP_MAX_SOURCES];
static int sources_used;   /* highest slot ever used, so the scan can stop early */

/* Register a descriptor to wait on. Returns an id for loop_remove, or -1 when
 * there is no room -- which the caller must handle rather than assume, since a
 * language server per language plus a terminal or two can fill a small table.
 *
 * `events` is LOOP_READ, LOOP_WRITE or both. It was not a parameter while the
 * only sources were things molly read; the shell filter writes to one pipe
 * while reading another, and the alternative to saying so here was a second
 * poll() somewhere else -- which is exactly what this module's own comment
 * above says it exists to prevent. */
int loop_add(int fd, int events, loop_handler on_ready, void *user) {
  int i;
  if (events == 0) events = LOOP_READ;
  for (i = 0; i < LOOP_MAX_SOURCES; i++) {
    if (sources[i].in_use) continue;
    sources[i].in_use = 1;
    sources[i].fd = fd;
    sources[i].events = events;
    sources[i].on_ready = on_ready;
    sources[i].user = user;
    if (i >= sources_used) sources_used = i + 1;
    return i;
  }
  return -1;
}

void loop_remove(int id) {
  if (id < 0 || id >= LOOP_MAX_SOURCES) return;
  sources[id].in_use = 0;
  sources[id].fd = -1;
  sources[id].events = 0;
  sources[id].on_ready = NULL;
  sources[id].user = NULL;
  while (sources_used > 0 && !sources[sources_used - 1].in_use) sources_used--;
}

int loop_source_count(void) {
  int i, n = 0;
  for (i = 0; i < sources_used; i++)
    if (sources[i].in_use) n++;
  return n;
}

void loop_cleanup(void) {
  int i;
  for (i = 0; i < LOOP_MAX_SOURCES; i++) {
    sources[i].in_use = 0;
    sources[i].fd = -1;
    sources[i].events = 0;
    sources[i].on_ready = NULL;
    sources[i].user = NULL;
  }
  sources_used = 0;
}

/* Wait up to timeout_ms for the keyboard or any registered source.
 *
 * Each ready source's handler is called before this returns, so a caller only has
 * to deal with the keyboard. Returns LOOP_STDIN when there is keyboard input,
 * LOOP_INTERRUPTED when a signal cut the wait short -- which is how a resize
 * arriving while the editor is idle gets noticed promptly -- and 0 on timeout.
 *
 * A source whose handler closes and unregisters it mid-dispatch is safe: the fd
 * is re-checked against the snapshot taken before the handler ran. */
int loop_wait(int timeout_ms) {
  struct pollfd pfd[LOOP_MAX_SOURCES + 1];
  int slot[LOOP_MAX_SOURCES + 1];
  int n = 0, i, ready, result = 0;

  pfd[n].fd = STDIN_FILENO;
  pfd[n].events = POLLIN;
  pfd[n].revents = 0;
  slot[n] = -1;              /* the keyboard has no handler */
  n++;

  for (i = 0; i < sources_used; i++) {
    if (!sources[i].in_use) continue;
    pfd[n].fd = sources[i].fd;
    pfd[n].events = (short)(((sources[i].events & LOOP_READ) ? POLLIN : 0) |
                            ((sources[i].events & LOOP_WRITE) ? POLLOUT : 0));
    pfd[n].revents = 0;
    slot[n] = i;
    n++;
  }

  ready = poll(pfd, (unsigned)n, timeout_ms);
  if (ready < 0) {
    if (errno == EINTR) return LOOP_INTERRUPTED;
    die("poll");
  }
  if (ready == 0) return 0;

  for (i = 0; i < n; i++) {
    /* POLLHUP and POLLERR matter as much as POLLIN: a child that exits leaves a
     * descriptor that is readable-at-EOF, and a handler that is never told will
     * spin on it forever. The same goes the other way -- a pipe whose reader
     * has gone is POLLERR, and the writer has to be told so it can stop. */
    if (!(pfd[i].revents & (POLLIN | POLLOUT | POLLHUP | POLLERR))) continue;
    if (slot[i] < 0) {
      result |= LOOP_STDIN;
      continue;
    }
    {
      struct loop_source *s = &sources[slot[i]];
      if (s->in_use && s->fd == pfd[i].fd && s->on_ready != NULL)
        s->on_ready(s->user);
    }
  }
  return result;
}
