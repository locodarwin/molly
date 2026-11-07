/* filter.c -- running a command over text.
 *
 * `sort`, `jq .`, `fmt -w 72`, `black -`, `tr a-z A-Z`: text goes to a
 * command's standard input and its standard output comes back. A hundred lines
 * here buys an unbounded amount of capability, which is why the roadmap put it
 * above everything else left on the list -- and it is the reason molly can
 * decline a plugin runtime without declining extensibility.
 *
 * It is also the first place molly runs something on the user's behalf, so this
 * file is mostly about what happens when that goes wrong. Four things can:
 *
 *   - **It never finishes.** `timeout_ms`, and a cancel callback so the user
 *     does not have to wait for the timeout to decide.
 *   - **It floods.** `max_out`, checked as the output arrives rather than after,
 *     because `yes | head -c 10G` must not be held in memory first.
 *   - **It deadlocks.** A command that writes more than a pipe will hold while
 *     molly is still feeding it stops reading, and molly's write blocks for
 *     ever. So the input pipe is a *writable* source in the event loop and the
 *     output pipe a readable one, and whichever is ready moves first.
 *   - **It writes to the terminal.** Standard error has to be a pipe too, or a
 *     mistyped command paints "sh: jqq: not found" into the middle of the text.
 *     The first line of it is worth keeping: it is the whole difference between
 *     "that did not work" and knowing why.
 *
 * What it deliberately does not know about: buffers, selections, the keymap and
 * the prompt. Text in, text out. That is what lets the editing half live in
 * cmd.c and lets every case above be a unit test rather than a pty session.
 */

#include "molly.h"
#include <poll.h>

/* Output buffer growth. Starts at a size that covers the overwhelming majority
 * of filters in one allocation and doubles, so a big output costs a handful of
 * reallocs rather than one per read. */
#define OUT_FIRST 8192

/* How much standard error is worth keeping. One line is what gets reported, and
 * a command that writes a megabyte of warnings should not be stored for the
 * sake of its first sentence. */
#define ERR_KEEP 1024

/* How long a canceled or timed-out command gets to exit on SIGTERM before it
 * is sent SIGKILL. Long enough for a shell pipeline to tear down and a command
 * to remove its temporary files; short enough that nobody waits for it. */
#define GRACE_MS 300

struct run {
  struct filter_io *io;

  int in_fd, out_fd, err_fd;    /* molly's ends; -1 once closed */
  int in_id, out_id, err_id;    /* loop source ids; -1 once removed */

  int sent;                     /* bytes of io->in written so far */

  int outcap;
  char *errbuf;
  int errlen;

  int toobig;                   /* the output cap was hit */
};

/* ---- the clock ----------------------------------------------------------- */

/* Monotonic, because a filter's deadline must not move when the system clock
 * does. A command that survives an NTP step backwards is not a feature. */
static long now_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- the three pipes ----------------------------------------------------- */

static void set_nonblocking(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Close one of molly's ends and take it out of the event loop. Both at once,
 * always: a source left registered on a closed descriptor is a poll() on a
 * descriptor that may since have been handed to something else. */
static void drop(int *fd, int *id) {
  if (*id >= 0) {
    loop_remove(*id);
    *id = -1;
  }
  if (*fd >= 0) {
    close(*fd);
    *fd = -1;
  }
}

/* ---- the handlers -------------------------------------------------------- */

/* Feed the command. One write per readiness, of whatever the pipe will take.
 *
 * EPIPE is not a failure: `head -1` reads one line and closes its input, and
 * the right response is to stop writing, not to report an error. It arrives as
 * an error here rather than as a signal because molly ignores SIGPIPE -- which
 * it has to, or this would kill the editor. */
static void on_writable(void *user) {
  struct run *r = user;
  ssize_t n;
  int left = r->io->inlen - r->sent;

  if (left <= 0) {
    drop(&r->in_fd, &r->in_id);
    return;
  }
  n = write(r->in_fd, r->io->in + r->sent, (size_t)left);
  if (n > 0) r->sent += (int)n;
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
  if (n < 0 || r->sent >= r->io->inlen) {
    /* Done, or the command stopped listening. Either way its input is over,
     * and closing is what tells it so. */
    drop(&r->in_fd, &r->in_id);
  }
}

static void on_readable(void *user) {
  struct run *r = user;
  struct filter_io *io = r->io;
  ssize_t n;

  for (;;) {
    if (io->outlen + 1 >= r->outcap) {
      int cap = r->outcap ? r->outcap * 2 : OUT_FIRST;
      /* One byte over the cap is enough to know it was exceeded, and stopping
       * here is what keeps a runaway command from being held in memory before
       * being rejected. */
      if (io->max_out > 0 && cap > io->max_out + 1) cap = io->max_out + 1;
      if (cap <= io->outlen + 1) {
        r->toobig = 1;
        return;
      }
      io->out = xrealloc(io->out, (size_t)cap);
      r->outcap = cap;
    }
    n = read(r->out_fd, io->out + io->outlen, (size_t)(r->outcap - io->outlen - 1));
    if (n > 0) {
      io->outlen += (int)n;
      if (io->max_out > 0 && io->outlen > io->max_out) {
        r->toobig = 1;
        return;
      }
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    /* 0 is end of file, anything else is an error on a pipe we were reading;
     * both mean there is no more output. */
    drop(&r->out_fd, &r->out_id);
    return;
  }
}

static void on_stderr(void *user) {
  struct run *r = user;
  ssize_t n;
  char scratch[512];

  for (;;) {
    n = read(r->err_fd, scratch, sizeof(scratch));
    if (n > 0) {
      int room = ERR_KEEP - r->errlen;
      /* Past the first kilobyte the bytes are read and dropped. Not reading
       * them would block a command that writes a lot to standard error. */
      if (room > 0) {
        int take = (n < room) ? (int)n : room;
        memcpy(r->errbuf + r->errlen, scratch, (size_t)take);
        r->errlen += take;
      }
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    drop(&r->err_fd, &r->err_id);
    return;
  }
}

/* ---- reporting ----------------------------------------------------------- */

/* The first line of what the command wrote to standard error, trimmed, into
 * io->err. One line because it goes on a one-row message bar, and the first
 * because that is where a shell or a tool puts the thing that went wrong. */
static void first_line(struct filter_io *io, const char *s, int len) {
  int i = 0, n = 0;
  /* Leading blanks and control bytes are skipped, not kept: a message that
   * starts with an escape sequence would otherwise begin with the space that
   * replaces it, and one row of message bar has no room to waste. */
  while (i < len && (unsigned char)s[i] <= ' ') i++;
  while (i < len && s[i] != '\n' && s[i] != '\r' &&
         n < (int)sizeof(io->err) - 1) {
    /* Control bytes would move the cursor or change the color of the message
     * bar: a command's error text is not trusted to be printable. */
    io->err[n++] = (unsigned char)s[i] < 0x20 ? ' ' : s[i];
    i++;
  }
  while (n > 0 && io->err[n - 1] == ' ') n--;
  io->err[n] = '\0';
}

static void reason(struct filter_io *io, const char *text) {
  snprintf(io->err, sizeof(io->err), "%s", text);
}

/* ---- running it ---------------------------------------------------------- */

void filter_free(struct filter_io *io) {
  free(io->out);
  io->out = NULL;
  io->outlen = 0;
}

int filter_run(struct filter_io *io, int (*canceled)(void)) {
  int inp[2], outp[2], errp[2];
  pid_t pid;
  struct run r;
  char errbuf[ERR_KEEP];
  long deadline = 0, killed_at = 0;
  int result = FILTER_OK, killing = 0;
  int wstatus = 0;
  void (*old_sigpipe)(int);

  io->out = NULL;
  io->outlen = 0;
  io->status = 0;
  io->signaled = 0;
  io->err[0] = '\0';

  if (io->cmd == NULL || io->cmd[0] == '\0') {
    reason(io, "no command");
    return FILTER_NOSTART;
  }

  /* SIGPIPE ignored for the duration, and restored on the way out.
   *
   * Writing to a command that has stopped reading is normal -- `head -1` takes
   * one line and closes its input -- and its default action is to kill the
   * process doing the writing, which here is the editor. Set here rather than
   * once at startup for two reasons: this is the module that writes to pipes,
   * so this is where the reason lives; and a unit test that links filter.c
   * without ever starting a terminal gets the same protection the editor does,
   * which is the difference between the awkward cases being tested and being
   * hoped about. */
  old_sigpipe = signal(SIGPIPE, SIG_IGN);

  if (pipe(inp) != 0) {
    reason(io, strerror(errno));
    signal(SIGPIPE, old_sigpipe);
    return FILTER_NOSTART;
  }
  if (pipe(outp) != 0) {
    reason(io, strerror(errno));
    close(inp[0]); close(inp[1]);
    signal(SIGPIPE, old_sigpipe);
    return FILTER_NOSTART;
  }
  if (pipe(errp) != 0) {
    reason(io, strerror(errno));
    close(inp[0]); close(inp[1]);
    close(outp[0]); close(outp[1]);
    signal(SIGPIPE, old_sigpipe);
    return FILTER_NOSTART;
  }

  pid = fork();
  if (pid < 0) {
    reason(io, strerror(errno));
    close(inp[0]); close(inp[1]);
    close(outp[0]); close(outp[1]);
    close(errp[0]); close(errp[1]);
    signal(SIGPIPE, old_sigpipe);
    return FILTER_NOSTART;
  }

  if (pid == 0) {
    /* The child. Its own process group, so that canceling kills the whole
     * pipeline rather than the `sh` that started it and leaving the real work
     * running with nowhere to write. */
    setpgid(0, 0);
    /* SIGPIPE back to its default, because exec keeps an *ignored* signal
     * ignored and the parent has just ignored this one. A command whose reader
     * goes away has to die of it the way it would in a shell -- that is how
     * `yes | head` ends -- rather than spinning on a write that keeps failing. */
    signal(SIGPIPE, SIG_DFL);
    dup2(inp[0], STDIN_FILENO);
    dup2(outp[1], STDOUT_FILENO);
    dup2(errp[1], STDERR_FILENO);
    close(inp[0]); close(inp[1]);
    close(outp[0]); close(outp[1]);
    close(errp[0]); close(errp[1]);
    /* Through a shell, so that a pipeline, a redirect and a glob all work --
     * which is what a user typing into a "filter through" prompt expects. The
     * command came from that prompt: it is the user's own, typed deliberately,
     * and no more privileged than what they would type at their own shell. */
    execl("/bin/sh", "sh", "-c", io->cmd, (char *)NULL);
    _exit(127);
  }

  /* The parent keeps the writing end of the input pipe and the reading ends of
   * the other two; the child's copies have to go, or the output pipe never
   * reaches end of file because molly is still holding it open. */
  close(inp[0]);
  close(outp[1]);
  close(errp[1]);

  /* Also in the child's group from this side, because whichever of fork's two
   * returns runs first must not leave a window where the group does not exist. */
  setpgid(pid, pid);

  memset(&r, 0, sizeof(r));
  r.io = io;
  r.in_fd = inp[1];
  r.out_fd = outp[0];
  r.err_fd = errp[0];
  r.in_id = r.out_id = r.err_id = -1;
  r.errbuf = errbuf;

  set_nonblocking(r.in_fd);
  set_nonblocking(r.out_fd);
  set_nonblocking(r.err_fd);

  if (io->inlen <= 0) drop(&r.in_fd, &r.in_id);

  if (r.in_fd >= 0) r.in_id = loop_add(r.in_fd, LOOP_WRITE, on_writable, &r);
  r.out_id = loop_add(r.out_fd, LOOP_READ, on_readable, &r);
  r.err_id = loop_add(r.err_fd, LOOP_READ, on_stderr, &r);
  if (r.out_id < 0 || r.err_id < 0 || (r.in_fd >= 0 && r.in_id < 0)) {
    /* The loop's table is full. Said rather than worked around: a filter that
     * silently wrote all its input first would deadlock on a big output. */
    reason(io, "too many event sources to run a filter");
    result = FILTER_NOSTART;
    goto done;
  }

  if (io->timeout_ms > 0) deadline = now_ms() + io->timeout_ms;

  /* Let it run, until its output and its standard error have both ended. Not
   * until the child exits: a child can exit with output still sitting in a
   * pipe, and reading to end of file is what says there is none left.
   *
   * Every way of stopping early *leaves* this loop rather than carrying on in
   * a stopping state. That was not the first version, and the first version was
   * wrong: a loop that kept polling after the output cap was hit polled a pipe
   * that on_readable now refused to read -- which is readable for ever, so
   * loop_wait returned instantly every time and a flooding command became a
   * spin with no way out. Leaving at once also makes the reaping below one
   * shape for all four ways of stopping. */
  while (r.out_fd >= 0 || r.err_fd >= 0) {
    long t = now_ms();
    int budget = LOOP_TICK_MS;

    if (r.toobig) {
      result = FILTER_TOOBIG;
      reason(io, "too much output");
      break;
    }
    if (deadline != 0) {
      long until = deadline - t;
      if (until <= 0) {
        result = FILTER_TIMEOUT;
        reason(io, "took too long");
        break;
      }
      if (until < budget) budget = (int)until;
    }
    if (canceled != NULL && canceled()) {
      result = FILTER_CANCELED;
      reason(io, "canceled");
      break;
    }
    loop_wait(budget);
  }

done:
  /* Molly's ends go before the child is waited for. A descriptor still open on
   * a pipe the child is writing to is a command that blocks for ever instead of
   * exiting, and the whole point of the wait below is that it cannot hang. */
  drop(&r.in_fd, &r.in_id);
  drop(&r.out_fd, &r.out_id);
  drop(&r.err_fd, &r.err_id);

  /* Make sure it is gone.
   *
   * A command that finished on its own is reaped on the first pass. One that
   * was stopped -- timed out, canceled, flooding, or never started -- gets
   * SIGTERM, a grace period to tear its pipeline down, and then SIGKILL, which
   * cannot be declined. The process *group*, so a pipeline goes with its shell:
   * killing the `sh` alone would leave the real work running with nowhere to
   * write. WNOHANG throughout, because a blocking wait here would hand a
   * command that closed its output and then slept the power to hang the editor
   * -- the one failure none of the three limits above would catch. */
  if (result != FILTER_OK) {
    kill(-pid, SIGTERM);
    killing = 1;
    killed_at = now_ms();
  }
  for (;;) {
    pid_t got = waitpid(pid, &wstatus, WNOHANG);
    if (got == pid) break;
    if (got < 0) {
      if (errno == EINTR) continue;
      break;                      /* not ours any more; nothing left to reap */
    }
    /* Still running. A command whose output has ended but which has not exited
     * is given the same grace as one that was stopped: it has had its say, and
     * the alternative is waiting on it for ever. */
    if (!killing) {
      killing = 1;
      killed_at = now_ms();
    } else if (now_ms() - killed_at >= GRACE_MS) {
      kill(-pid, SIGTERM);
      if (now_ms() - killed_at >= 2 * GRACE_MS) kill(-pid, SIGKILL);
    }
    loop_wait(20);                /* a short sleep that still drains the loop */
  }
  if (WIFEXITED(wstatus)) io->status = WEXITSTATUS(wstatus);
  if (WIFSIGNALED(wstatus)) io->signaled = WTERMSIG(wstatus);

  signal(SIGPIPE, old_sigpipe);

  if (io->out != NULL) io->out[io->outlen] = '\0';

  /* The command's own words beat molly's, when it left any: "sh: jqq: not
   * found" tells the user what to fix and "exited 127" does not. */
  if (r.errlen > 0 && result != FILTER_CANCELED) first_line(io, errbuf, r.errlen);

  if (result == FILTER_OK && (io->status != 0 || io->signaled != 0)) {
    result = FILTER_FAILED;
    if (io->err[0] == '\0') {
      if (io->signaled != 0)
        snprintf(io->err, sizeof(io->err), "killed by signal %d", io->signaled);
      else
        snprintf(io->err, sizeof(io->err), "exited %d", io->status);
    }
  }
  return result;
}

/* ======================================================================== *
 *  The editing half.
 *
 *  Everything above this line is text in and text out: not one function up
 *  there mentions a buffer, a selection or a key, which is what lets every
 *  way a command can misbehave be a unit test instead of a pty session.
 *  Below it is the part that knows what molly is -- the prompt, what gets
 *  sent, and what happens to the answer.
 * ======================================================================== */

/* Esc while a command is running. */
static int filter_canceled(void) {
  return input_escape_pressed();
}

/* Lines in a run of bytes, counting an unterminated last line as one. */
static int count_lines(const char *s, int len) {
  int i, n = 0;
  for (i = 0; i < len; i++)
    if (s[i] == '\n') n++;
  if (len > 0 && s[len - 1] != '\n') n++;
  return n;
}

/* Ask for a command, with Enter on an empty prompt meaning the last one -- the
 * idiom Ctrl-F already has for repeating a search. `last` is the caller's, so
 * that the filter and the output insert each remember their own: `sort` and
 * `date` are not answers to the same question. Returns 0 if there is nothing
 * to run. */
static int ask_command(char *prompt, const char *last, char *cmd,
                       size_t cmdsz) {
  char *input = editor_prompt(prompt, NULL);
  if (input == NULL) return 0;
  if (input[0] == '\0') {
    free(input);
    if (last[0] == '\0') return 0;
    snprintf(cmd, cmdsz, "%s", last);
  } else {
    snprintf(cmd, cmdsz, "%s", input);
    free(input);
  }
  return 1;
}

/* Filter the selection -- or the whole buffer, when nothing is selected --
 * through a command, and replace it with the output.
 *
 * With nothing selected it selects everything first rather than taking a
 * separate path for the whole buffer: `sort` on a file is the commonest thing
 * anyone will do with this, and one path through selection_dup() and
 * selection_delete() is one path to get wrong. The selection is put back
 * exactly as it was if anything goes wrong, so a failed filter leaves the
 * screen as it found it.
 *
 * The command is not run on a non-zero exit. A filter means "replace my text
 * with this", and a command that failed has almost certainly produced nothing
 * or half of something -- `jq` on a syntax error writes the error and exits 5.
 * Undo would get the text back, but not losing it is better than getting it
 * back, and the status bar can say what the command said instead. The cost is
 * that `grep`, which exits 1 when it matches nothing, cannot be used to empty
 * a selection; emptying a selection is what Delete is for. */
void editor_filter(void) {
  /* The last command, so that running it again is Enter on an empty prompt --
   * the same idiom Ctrl-F already has for repeating a search. */
  static char last[512];
  struct filter_io io;
  struct cursor saved = CUR;
  char cmd[512];
  char *text;
  int len = 0, result, lines_in, lines_out, i;
  int had_newline;

  if (row_count() == 0) {
    editor_set_status_message("Nothing to filter");
    return;
  }

  if (!ask_command("Filter through: %s (ESC to cancel)", last, cmd, sizeof(cmd)))
    return;
  /* One cursor, keeping its selection: the output replaces text the others
   * are in, and they would be left at offsets into what is gone. See
   * editor_replace(). */
  cursors_collapse();

  if (!CUR.sel_active) {
    CUR.sy = 0;
    CUR.sx = 0;
    CUR.cy = row_count() - 1;
    CUR.cx = row_at(CUR.cy)->size;
    CUR.sel_active = 1;
  }
  text = selection_dup(&len);
  if (text == NULL) {
    CUR = saved;
    editor_set_status_message("Nothing to filter");
    return;
  }
  had_newline = (len > 0 && text[len - 1] == '\n');
  lines_in = count_lines(text, len);

  /* Drawn before the wait, not after it: this message is the only thing that
   * says the editor is busy rather than wedged, and the only thing that says
   * Esc will get out of it. */
  editor_set_status_message("Running %.40s ... (ESC cancels)", cmd);
  editor_refresh_screen();

  memset(&io, 0, sizeof(io));
  io.cmd = cmd;
  io.in = text;
  io.inlen = len;
  io.timeout_ms = shell_timeout_ms();
  io.max_out = shell_output_max();
  result = filter_run(&io, filter_canceled);
  free(text);

  if (result != FILTER_OK) {
    CUR = saved;
    editor_set_status_message("%.30s: %s", cmd,
                              io.err[0] != '\0' ? io.err : "did not run");
    filter_free(&io);
    return;
  }

  snprintf(last, sizeof(last), "%s", cmd);

  /* A command puts a line terminator after its last line. If what it was given
   * did not have one, the output should not either, or filtering the end of a
   * file through `sort` quietly grows a blank line every time. */
  if (!had_newline && io.outlen > 0 && io.out[io.outlen - 1] == '\n')
    io.outlen--;
  lines_out = count_lines(io.out, io.outlen);

  /* One undo step for the whole thing, which is what made undo groups nest:
   * the delete is a group of its own and the inserts were a second one. */
  undo_group_begin();
  selection_delete();
  for (i = 0; i < io.outlen; i++) {
    if (io.out[i] == '\n') editor_insert_newline();
    else editor_insert_char((unsigned char)io.out[i]);
  }
  undo_group_end();

  editor_set_status_message("%.30s: %d line%s in, %d out", cmd,
                            lines_in, lines_in == 1 ? "" : "s", lines_out);
  filter_free(&io);
}

/* Run a command and insert what it prints at the cursor -- at every cursor, when
 * there are several -- without sending it anything: `date`, `git rev-parse
 * HEAD`, `uuidgen`, `ls`.
 *
 * Standard input is empty rather than the selection. A selection is replaced by
 * the output, as typing or pasting over one replaces it, so the command was
 * never going to be a filter of it; the filter is Alt-| and already does that.
 *
 * One trailing newline is dropped, always. What these commands answer is
 * mostly one line, and the place it is wanted is mostly the middle of one --
 * a hash in a commit message, a date in a header -- where the terminator the
 * command adds would split the line it lands in. A command whose output is
 * several lines leaves the cursor at the end of the last one, which is where
 * pasting the same text would have left it; Enter is one key away, and a
 * blank line that was not asked for is a key to remove.
 *
 * As with the filter, a non-zero exit inserts nothing: `git rev-parse HEAD`
 * outside a repository writes "fatal: not a git repository" to standard error
 * and exits 128, and that sentence belongs on the status bar rather than in the
 * file. */
void editor_insert_output(void) {
  static char last[512];
  struct filter_io io;
  char cmd[512];
  int len, lines, result;

  if (!ask_command("Insert output of: %s (ESC to cancel)", last, cmd, sizeof(cmd)))
    return;

  editor_set_status_message("Running %.40s ... (ESC cancels)", cmd);
  editor_refresh_screen();

  memset(&io, 0, sizeof(io));
  io.cmd = cmd;
  io.in = "";
  io.inlen = 0;
  io.timeout_ms = shell_timeout_ms();
  io.max_out = shell_output_max();
  result = filter_run(&io, filter_canceled);

  if (result != FILTER_OK) {
    editor_set_status_message("%.30s: %s", cmd,
                              io.err[0] != '\0' ? io.err : "did not run");
    filter_free(&io);
    return;
  }

  snprintf(last, sizeof(last), "%s", cmd);

  len = io.outlen;
  if (len > 0 && io.out[len - 1] == '\n') len--;
  if (len == 0) {
    editor_set_status_message("%.30s: no output", cmd);
    filter_free(&io);
    return;
  }
  lines = count_lines(io.out, len);

  /* One undo step, the replaced selection included -- the filter's rule. */
  undo_group_begin();
  cursors_insert_text(io.out, len);
  undo_group_end();

  editor_set_status_message("%.30s: %d line%s inserted", cmd,
                            lines, lines == 1 ? "" : "s");
  filter_free(&io);
}
