/* term.c -- The terminal: raw mode, signals, size queries, one byte of input, and die().
   Together with sys.c this is what a Windows port has to replace. */

#include "molly.h"

_Noreturn void die(const char *s) {
  write_recovery_all();
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);
  perror(s);
  exit(1);
}


/* Runs from atexit, where die() -- which calls exit() again -- would be
 * undefined behavior. If the terminal is already gone there is nothing left
 * to restore anyway. */
void disable_raw_mode(void) {
  /* Mouse reporting off before the terminal modes go back, or the shell inherits a
   * terminal that sends it escape sequences whenever the user clicks. Writing from
   * an atexit handler is allowed; failing is not worth checking for, since there is
   * nothing useful to do about it at this point. */
  write_all(STDOUT_FILENO, "\x1b[?1006l\x1b[?1002l", 16);
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
}

/* A fatal signal bypasses atexit, so closing the terminal window or a plain
 * `kill` used to leave the terminal in raw mode, with no echo and no line
 * editing. Restore it, then re-raise so the process still dies by the signal.
 * Only async-signal-safe calls belong here, which write and tcsetattr are.
 *
 * Mouse reporting is switched off here as well as in disable_raw_mode. Missing it
 * was not a cosmetic slip: a molly killed by a signal left the shell with a
 * terminal still reporting clicks, so the next click in that window typed an escape
 * sequence at the prompt. The atexit path had always handled it; this one had not,
 * and nothing noticed because the mouse was Phase 1 work and this handler was
 * Phase 0's. */
static void handle_fatal_signal(int sig) {
  static const char reset[] = "\x1b[?1006l\x1b[?1002l\x1b[?25h\x1b[2J\x1b[H";
  /* All of them, and still async-signal-safe: the loop is plain reads of the
   * buffer list and a pointer assignment, and what it calls per buffer is the
   * open/write/fsync/close it always was. */
  write_recovery_all();
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
  write(STDOUT_FILENO, reset, sizeof(reset) - 1);
  raise(sig);
}

/* Set from handle_winch, read from editor_read_key's poll loop. Only a flag
 * write, so it is safe from a signal handler; the actual resize (which
 * reallocates the render cache) happens back in normal context. */
static volatile sig_atomic_t got_winch = 0;

static void handle_winch(int sig) {
  (void)sig;
  got_winch = 1;
}

/* Whether a resize has arrived since this was last asked, clearing the flag.
 * The editor treats a resize as though it were a key, so the input layer asks
 * here rather than reading a signal flag that belongs to the terminal code. */
int term_take_winch(void) {
  if (!got_winch) return 0;
  got_winch = 0;
  return 1;
}

static void install_signal_handlers(void) {
  static const int sigs[] = {SIGHUP, SIGINT, SIGQUIT, SIGTERM};
  struct sigaction sa;
  size_t i;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handle_fatal_signal;
  sigemptyset(&sa.sa_mask);
  /* The handler is reset to the default on entry, so the raise() in it ends
   * the process with the signal's normal action. */
  sa.sa_flags = SA_RESETHAND;
  for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
    sigaction(sigs[i], &sa, NULL);

  /* SA_RESTART: a SIGWINCH landing mid-read transparently restarts the read
   * instead of failing it with EINTR, which editor_read_key would otherwise
   * treat as a fatal error. */
  struct sigaction wsa;
  memset(&wsa, 0, sizeof(wsa));
  wsa.sa_handler = handle_winch;
  sigemptyset(&wsa.sa_mask);
  wsa.sa_flags = SA_RESTART;
  sigaction(SIGWINCH, &wsa, NULL);
}

void enable_raw_mode(void) {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
  atexit(disable_raw_mode);
  install_signal_handlers();

  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");

  /* Turn bracketed paste off even if the terminal or a parent shell left it
   * on. Molly does not parse the "\x1b[200~...\x1b[201~" wrappers, so with the
   * mode on a paste would arrive as a stray DEL key (the "\x1b[...~" prefix)
   * plus literal text; off, a paste is just literal keystrokes and inserts
   * cleanly. */
  write_all(STDOUT_FILENO, "\x1b[?2004l", 8);

  /* Mouse reporting: 1002 reports presses, releases and motion while a button is
   * held -- enough for click, drag and the wheel, without the flood that 1003's
   * any-motion tracking produces. 1006 asks for the SGR encoding, whose
   * coordinates are plain decimal and so are not capped at column 223 the way the
   * original protocol's single bytes were.
   *
   * This takes drag-to-select away from the terminal, which is a real cost: the
   * reflex of swiping text and middle-clicking it elsewhere stops working, because
   * molly receives those events instead. Most terminals still pass the gesture
   * through when Shift is held, which is the escape hatch, and the help screen says
   * so. Which is also why it is a setting -- `[input] mouse = off` in mollyrc hands
   * the gesture back to the terminal for good.
   *
   * Not enabled here. `[input] mouse` owns it, default on, and config_apply()
   * turns it on immediately after init_editor() -- the same arrangement as the
   * menu bar, and for the same reason: asking for the mouse here and then taking
   * it back a moment later would put the request in the stream whatever the user
   * asked for, which is both a flicker and a lie to anyone reading the output. */
}

/* Mouse reporting, on or off. Separate from raw mode because it is the one part
 * of the terminal's state that a user gets to choose: see `[input] mouse`. */
static int mouse_on;

int term_mouse_is_on(void) { return mouse_on; }

void term_mouse_set(int on) {
  if (on == mouse_on) return;
  mouse_on = on ? 1 : 0;
  if (mouse_on) write_all(STDOUT_FILENO, "\x1b[?1002h\x1b[?1006h", 16);
  else write_all(STDOUT_FILENO, "\x1b[?1006l\x1b[?1002l", 16);
}

int read_byte(unsigned char *c) {
  return read(STDIN_FILENO, c, 1) == 1;
}


int get_cursor_position(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
    if (buf[i] == 'R') break;
    i++;
  }
  buf[i] = '\0';

  if (buf[0] != '\x1b' || buf[1] != '[') return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
  return 0;
}

int get_window_size(int *rows, int *cols) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    return get_cursor_position(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}
