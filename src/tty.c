/* tty.c -- the integrated terminal.
 *
 * A shell in a molly window. vt.c knows what a terminal screen is; this file
 * is everything around it: the pseudo-terminal and the shell on the other end
 * of it, the buffer that carries it so that the window tree, the tab bar and
 * the buffer list take it for granted, drawing its cells, and deciding which
 * keys are the shell's and which are still molly's.
 *
 * A terminal is a buffer. That is the one decision everything else follows
 * from: splitting, switching, closing and the tab bar already work on buffers,
 * and a terminal that was a fourth kind of thing would have needed each of
 * them taught about it. The buffer has no rows -- its text is the emulator's
 * screen -- and every command that would edit text refuses on it (CMD_EDITS),
 * except paste, which goes to the program as if it had been typed.
 *
 * The keys: while a terminal has the keyboard, every key goes to the program
 * in it, except the few that get the user out again -- the window and buffer
 * switches, the menu bar, the palette, Quit, and Alt-T itself -- found by the
 * command they are bound to, so rebinding one moves it here too. Shift-PgUp and
 * Shift-PgDn page through the scrollback, which is what every terminal
 * emulator does with them.
 *
 * When the shell exits, its terminal closes: the buffer, and the window it was
 * opened in if there are others. A terminal with nothing in it is not worth a
 * buffer of its own, and the alternative was a dead buffer that refused every
 * key. */

#include "molly.h"
#include <poll.h>

#define TTY_SCROLLBACK 5000
#define TTY_READ_MAX   (256 * 1024)   /* per wake-up, so a flood cannot starve keys */

struct tty {
  struct vt *vt;
  pid_t pid;
  int fd;
  int rid, wid;          /* loop sources: reading always, writing when queued */
  struct abuf wq;
  int wdone;
  int exited;            /* the shell has gone; tty_tidy() closes the buffer */
  int status;            /* its wait status */
  int scroll;            /* lines back into the scrollback the view shows */
  int rows, cols;        /* the size the program has been told */
  char shell[64];
};

/* ---- the process --------------------------------------------------------- */

static void tty_flush(struct tty *t);
static void on_writable(void *user) { tty_flush(user); }

static void tty_flush(struct tty *t) {
  while (t->fd >= 0 && t->wdone < t->wq.len) {
    ssize_t n = write(t->fd, t->wq.b + t->wdone, (size_t)(t->wq.len - t->wdone));
    if (n > 0) {
      t->wdone += (int)n;
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    t->wq.len = t->wdone = 0;   /* the other end is gone; reading will say so */
    break;
  }
  if (t->wdone >= t->wq.len) {
    t->wq.len = t->wdone = 0;
    if (t->wid >= 0) {
      loop_remove(t->wid);
      t->wid = -1;
    }
  } else if (t->wid < 0 && t->fd >= 0) {
    t->wid = loop_add(t->fd, LOOP_WRITE, on_writable, t);
  }
}

static void tty_write(struct tty *t, const char *s, int n) {
  if (t->fd < 0 || n <= 0) return;
  ab_append(&t->wq, s, n);
  tty_flush(t);
}

/* The shell has closed the terminal: reaped, or given a moment and then made
 * to go, so a background job holding nothing open cannot keep it a zombie. */
static void tty_ended(struct tty *t) {
  int i, st = 0;
  if (t->rid >= 0) loop_remove(t->rid);
  if (t->wid >= 0) loop_remove(t->wid);
  t->rid = t->wid = -1;
  if (t->fd >= 0) close(t->fd);
  t->fd = -1;
  for (i = 0; i < 50; i++) {
    pid_t got = waitpid(t->pid, &st, WNOHANG);
    if (got == t->pid || (got < 0 && errno != EINTR)) break;
    if (i == 20) kill(-t->pid, SIGHUP);
    poll(NULL, 0, 10);
  }
  if (i == 50) {
    kill(-t->pid, SIGKILL);
    waitpid(t->pid, &st, 0);
  }
  t->status = st;
  t->exited = 1;
  ui_request_redraw();
}

static void on_output(void *user) {
  struct tty *t = user;
  char buf[16384];
  int total = 0, before = vt_scrollback(t->vt);

  while (total < TTY_READ_MAX) {
    ssize_t n = read(t->fd, buf, sizeof(buf));
    if (n > 0) {
      vt_feed(t->vt, buf, (size_t)n);
      total += (int)n;
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    /* EIO is how Linux says the last holder of the other side has closed it:
     * the shell exited. End of file says the same elsewhere. */
    tty_ended(t);
    return;
  }
  {
    int len;
    const char *r = vt_reply(t->vt, &len);
    if (len > 0) tty_write(t, r, len);
    vt_reply_done(t->vt);
  }
  /* Scrolled back, the view stays on the lines it was showing rather than
   * sliding as new ones arrive underneath. */
  if (t->scroll > 0) {
    t->scroll += vt_scrollback(t->vt) - before;
    if (t->scroll > vt_scrollback(t->vt)) t->scroll = vt_scrollback(t->vt);
    if (t->scroll < 0) t->scroll = 0;
  }
  ui_request_redraw();
}

static void set_size(struct tty *t, int rows, int cols) {
  struct winsize ws;
  if (rows < 1) rows = 1;
  if (cols < 1) cols = 1;
  if (rows == t->rows && cols == t->cols) return;
  t->rows = rows;
  t->cols = cols;
  vt_resize(t->vt, rows, cols);
  if (t->fd >= 0) {
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    /* The kernel sends the program SIGWINCH, which is how it learns. */
    ioctl(t->fd, TIOCSWINSZ, &ws);
  }
}

/* The shell: $SHELL, or /bin/sh when that is unset or not runnable. */
static const char *shell_path(void) {
  const char *s = getenv("SHELL");
  if (s != NULL && s[0] == '/' && access(s, X_OK) == 0) return s;
  return "/bin/sh";
}

static struct tty *tty_start(int rows, int cols) {
  struct tty *t;
  struct winsize ws;
  const char *sh = shell_path(), *base;
  char *slave;
  int fd;
  pid_t pid;

  fd = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (fd < 0 || grantpt(fd) != 0 || unlockpt(fd) != 0 || (slave = ptsname(fd)) == NULL) {
    editor_set_status_message("No terminal: %s", strerror(errno));
    if (fd >= 0) close(fd);
    return NULL;
  }
  if (rows < 1) rows = 1;
  if (cols < 1) cols = 1;
  memset(&ws, 0, sizeof(ws));
  ws.ws_row = (unsigned short)rows;
  ws.ws_col = (unsigned short)cols;
  ioctl(fd, TIOCSWINSZ, &ws);

  pid = fork();
  if (pid < 0) {
    editor_set_status_message("No terminal: %s", strerror(errno));
    close(fd);
    return NULL;
  }
  if (pid == 0) {
    int s;
    /* A session of its own, with the new terminal as its controlling one, so
     * the shell has job control and Ctrl-C reaches what it is running rather
     * than molly. */
    setsid();
    s = open(slave, O_RDWR);
    if (s < 0) _exit(127);
    ioctl(s, TIOCSCTTY, 0);
    dup2(s, 0);
    dup2(s, 1);
    dup2(s, 2);
    if (s > 2) close(s);
    signal(SIGPIPE, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
    signal(SIGWINCH, SIG_DFL);
    /* What the shell is told it is in: an xterm with 256 colours, which is
     * what vt.c is. COLORTERM goes, because molly brings 24-bit colour down to
     * the 256 and a program that knew that would choose its own colours
     * better. MOLLY_LSP and MOLLY_CONFIG are the editor's, not the shell's. */
    setenv("TERM", "xterm-256color", 1);
    setenv("TERM_PROGRAM", "molly", 1);
    unsetenv("COLORTERM");
    unsetenv("LINES");
    unsetenv("COLUMNS");
    unsetenv("MOLLY_LSP");
    unsetenv("MOLLY_CONFIG");
    base = strrchr(sh, '/');
    execl(sh, base != NULL ? base + 1 : sh, (char *)NULL);
    _exit(127);
  }

  t = xmalloc(sizeof(*t));
  memset(t, 0, sizeof(*t));
  t->vt = vt_new(rows, cols, TTY_SCROLLBACK);
  t->rows = rows;
  t->cols = cols;
  t->pid = pid;
  t->fd = fd;
  t->wid = -1;
  base = strrchr(sh, '/');
  snprintf(t->shell, sizeof(t->shell), "%s", base != NULL ? base + 1 : sh);
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  t->rid = loop_add(fd, LOOP_READ, on_output, t);
  if (t->rid < 0) {
    editor_set_status_message("No terminal: too many things to wait on");
    tty_ended(t);
  }
  return t;
}

/* A buffer is being freed: its program goes with it. SIGHUP, as closing a
 * terminal window has always sent, to the whole session. */
void tty_free(struct tty *t) {
  if (t == NULL) return;
  if (!t->exited && t->pid > 0) {
    kill(-t->pid, SIGHUP);
    kill(t->pid, SIGHUP);
    tty_ended(t);
  }
  vt_free(t->vt);
  free(t->wq.b);
  free(t);
}

/* Is a program other than the shell running in it? The terminal's foreground
 * process group says: at a prompt it is the shell's own. */
int tty_busy(const struct tty *t) {
  pid_t fg;
  if (t == NULL || t->exited || t->fd < 0) return 0;
  fg = tcgetpgrp(t->fd);
  return fg > 0 && fg != t->pid;
}

int tty_busy_count(void) {
  int i, n = 0;
  for (i = 0; i < buf_count(); i++)
    if (tty_busy(buf_at(i)->tty)) n++;
  return n;
}

/* ---- opening and closing one --------------------------------------------- */

/* A new terminal in a buffer of its own, shown in the current window, at the
 * size of that window. Returns 1 if it started. */
int tty_open_here(void) {
  struct buffer *b;
  struct tty *t;
  char title[96];

  screen_sync_size();
  t = tty_start(V.screenrows, V.screencols);
  if (t == NULL) return 0;
  b = buf_new();
  b->tty = t;
  snprintf(title, sizeof(title), "terminal: %s", t->shell);
  b->title = xstrdup(title);
  buf_show(buf_index_of(b));
  cursors_collapse();
  memset(&CUR, 0, sizeof(CUR));
  V.drowoff = V.coloff = 0;
  screen_force_repaint();
  return 1;
}

/* The share of a split's height a new terminal takes: a third, so the file
 * stays the main thing on screen. */
#define TTY_SPLIT_SHARE (WIN_FRAC_ONE / 3)

/* Alt-T: the terminal, from wherever you are.
 *
 *   - In a terminal, back to the window you came from.
 *   - A window showing a terminal: to it.
 *   - A terminal open but not showing: into a new window below, showing it.
 *   - None at all: a new one, in a new window below.
 *
 * Below, and a third of the height, because that is where every editor with
 * a terminal puts it. Where there is no room to split, it takes the window. */
int tty_toggle(void) {
  int i;
  if (V.buf->tty != NULL) {
    if (window_count() > 1) window_move_focus(-1);
    return 0;
  }
  for (i = 0; i < window_count(); i++) {
    if (window_view_at(i)->buf->tty != NULL) {
      window_focus(i);
      return 0;
    }
  }
  for (i = 0; i < buf_count(); i++) {
    if (buf_at(i)->tty != NULL) {
      if (window_split_share(WIN_ROWS, TTY_SPLIT_SHARE)) {
        buf_show(i);
        screen_force_repaint();
      } else {
        buf_show(i);
      }
      return 0;
    }
  }
  if (window_split_share(WIN_ROWS, TTY_SPLIT_SHARE)) {
    if (!tty_open_here()) window_close_current();
  } else {
    tty_open_here();
  }
  return 0;
}

/* Between keys, in the main loop: close the terminals whose shells have
 * exited. Here and not where the exit is noticed, because that is inside the
 * event loop, which a menu or a dialog may be pumping -- the same reason the
 * language server's answers wait for the main loop. */
void tty_tidy(void) {
  int i;
  for (i = 0; i < buf_count(); i++) {
    struct buffer *b = buf_at(i);
    struct tty *t = b->tty;
    int w, st;
    if (t == NULL || !t->exited) continue;
    st = t->status;
    /* Its windows close first, while there are others to take the space. */
    for (w = window_count() - 1; w >= 0 && window_count() > 1; w--) {
      if (window_view_at(w)->buf != b) continue;
      window_focus(w);
      window_close_current();
    }
    buf_close(buf_index_of(b));
    if (WIFEXITED(st) && WEXITSTATUS(st) != 0)
      editor_set_status_message("Terminal closed: the shell exited %d.", WEXITSTATUS(st));
    else if (WIFSIGNALED(st))
      editor_set_status_message("Terminal closed: the shell was killed by signal %d.",
                                WTERMSIG(st));
    else
      editor_set_status_message("Terminal closed.");
    i = -1;   /* the list moved; start again */
  }
}

/* ---- keys and the mouse -------------------------------------------------- */

/* The commands a key keeps while a terminal has the keyboard: the ways out. */
static int stays_molly(const struct command *c) {
  static const char *const names[] = {
    "window.next", "window.prev", "menu.open", "terminal.toggle",
    "buffer.next", "buffer.prev", "app.palette", "file.quit",
  };
  int i;
  if (c == NULL) return 0;
  for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
    if (strcmp(c->name, names[i]) == 0) return 1;
  return 0;
}

static void scroll_by(struct tty *t, int n) {
  t->scroll += n;
  if (t->scroll > vt_scrollback(t->vt)) t->scroll = vt_scrollback(t->vt);
  if (t->scroll < 0) t->scroll = 0;
}

/* A key arrived while the current buffer is a terminal. Returns 1 when the
 * terminal has used it; 0 sends it on to molly's own dispatch. */
int tty_key(int c) {
  struct tty *t = V.buf->tty;
  int shift, ctrl, alt, mods, n;
  const struct command *cmd = NULL;
  char out[16];

  if (t == NULL || c == WINCH_KEY || c == MOUSE_KEY) return 0;
  shift = input_last_key_had_shift();
  ctrl = input_last_key_had_ctrl();
  alt = input_last_key_had_alt();
  mods = (shift ? KEY_SHIFT : 0) | (ctrl ? KEY_CTRL : 0) | (alt ? KEY_ALT : 0);
  if (mods) cmd = keymap_lookup(c | mods);
  if (cmd == NULL) cmd = keymap_lookup(c);
  if (stays_molly(cmd)) return 0;

  if (shift && !ctrl && !alt && (c == PAGE_UP || c == PAGE_DOWN)) {
    int page = t->rows > 2 ? t->rows - 1 : 1;
    scroll_by(t, c == PAGE_UP ? page : -page);
    return 1;
  }
  /* A dead terminal is about to close (tty_tidy); a key in between is not
   * text for an empty buffer. */
  if (t->exited) return 1;

  n = vt_key(t->vt, c, c < 256 ? 0 : (shift ? 1 : 0) | (alt ? 2 : 0) | (ctrl ? 4 : 0), out);
  if (n > 0) {
    t->scroll = 0;
    tty_write(t, out, n);
  }
  return 1;
}

/* Paste, into the program: wrapped in the bracketed-paste markers when it has
 * asked for them, so a shell does not run the lines as they arrive. */
void tty_paste(const char *s, int len) {
  struct tty *t = V.buf->tty;
  if (t == NULL || t->exited) return;
  t->scroll = 0;
  if (vt_mode(t->vt, VT_MODE_BRACKETED_PASTE)) tty_write(t, "\x1b[200~", 6);
  tty_write(t, s, len);
  if (vt_mode(t->vt, VT_MODE_BRACKETED_PASTE)) tty_write(t, "\x1b[201~", 6);
}

/* The wheel scrolls the scrollback; nothing else in the text area is the
 * terminal's business. */
void tty_mouse(const struct mouse_event *ev) {
  struct tty *t = V.buf->tty;
  if (t == NULL) return;
  if (ev->type == MOUSE_WHEEL_UP) scroll_by(t, 3);
  else if (ev->type == MOUSE_WHEEL_DOWN) scroll_by(t, -3);
}

/* ---- drawing ------------------------------------------------------------- */

/* Every terminal told the size of the window showing it, after the windows
 * have been laid out. The window with the keyboard decides when two show the
 * same terminal -- they cannot both, and the one being typed into is the one
 * that should fit -- and otherwise the first that shows it. A terminal nobody
 * is looking at keeps the size it had. */
void tty_layout(void) {
  int i, k;
  struct view *focus = E.curview;
  if (focus->buf->tty != NULL)
    set_size(focus->buf->tty, focus->screenrows, focus->screencols);
  for (i = 0; i < buf_count(); i++) {
    struct tty *t = buf_at(i)->tty;
    if (t == NULL || focus->buf->tty == t) continue;
    for (k = 0; k < window_count(); k++) {
      struct view *v = window_view_at(k);
      if (v->buf->tty != t) continue;
      set_size(t, v->screenrows, v->screencols);
      break;
    }
  }
}

/* The current view's text area, showing its terminal: as much of the screen
 * as fits, and blank where the window is bigger than the terminal (a second
 * window on one terminal, larger than the first). */
void tty_draw(void) {
  struct tty *t = V.buf->tty;
  int y, x, rows = V.screenrows, cols = V.screencols;

  if (t == NULL) return;
  {
    const char *title = vt_take_title(t->vt);
    if (title != NULL && title[0] != '\0') {
      char name[160];
      snprintf(name, sizeof(name), "terminal: %s", title);
      free(V.buf->title);
      V.buf->title = xstrdup(name);
    }
  }
  for (y = 0; y < rows; y++) {
    int n = 0;
    const struct vt_cell *line = vt_line(t->vt, y - t->scroll, &n);
    for (x = 0; x < cols; x++) {
      char ch[12];
      int nb = 1;
      if (line != NULL && x < n) {
        const struct vt_cell *c = &line[x];
        if (c->w == 0) continue;   /* drawn with its left half */
        grid_color(c->fg, c->bg,
                   c->attrs & (VT_BOLD | VT_UNDERLINE | VT_ITALIC | VT_REVERSE));
        if (c->cp == 0 || c->cp < 0x20 || (c->attrs & VT_INVISIBLE)) {
          ch[0] = ' ';
        } else {
          nb = utf8_encode(c->cp, ch);
          if (c->comb) nb += utf8_encode(c->comb, ch + nb);
        }
        grid_put_char(V.screeny + y, V.screenx + x, ch, nb,
                      c->w == 2 && x + 1 < cols ? 2 : 1);
      } else {
        grid_role(ROLE_TEXT);
        grid_put_char(V.screeny + y, V.screenx + x, " ", 1, 1);
      }
    }
  }
  grid_role(ROLE_TEXT);
}

/* Where the terminal's cursor is on the screen, for the window with the
 * keyboard. 0 when it should not be seen: the program hid it, or the view is
 * scrolled back from where it is. */
int tty_cursor(int *sy, int *sx) {
  struct tty *t = V.buf->tty;
  int y, x, visible;
  if (t == NULL || t->scroll > 0 || t->exited) return 0;
  vt_cursor(t->vt, &y, &x, &visible);
  if (!visible || y >= V.screenrows || x >= V.screencols) return 0;
  *sy = V.screeny + y;
  *sx = V.screenx + x;
  return 1;
}

/* The right-hand side of a terminal's status bar. */
void tty_status(char *dst, size_t n) {
  struct tty *t = V.buf->tty;
  if (t == NULL) {
    dst[0] = '\0';
  } else if (t->scroll > 0) {
    snprintf(dst, n, "%d lines back  Shift-PgDn", t->scroll);
  } else if (tty_busy(t)) {
    snprintf(dst, n, "running");
  } else {
    snprintf(dst, n, "%dx%d", t->cols, t->rows);
  }
}

/* Every terminal's shell, on the way out of molly. */
void tty_cleanup(void) {
  int i;
  for (i = 0; i < buf_count(); i++) {
    struct buffer *b = buf_at(i);
    if (b->tty == NULL) continue;
    tty_free(b->tty);
    b->tty = NULL;
  }
}
