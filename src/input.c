/* input.c -- Keys. Turning a byte stream into the editorKey enum, escape sequences and
   all. The Alt and Esc disambiguation lands here. */

#include "molly.h"

/* Escape sequences are consumed in full, to their terminating byte, even when
 * the result is a key we do not map. A sequence that was only partly consumed
 * used to leave its tail in the input stream, where it arrived as ordinary
 * characters and was inserted into the buffer -- pressing Ctrl-Right, for
 * example, typed a literal "5C" into the file. */
/* Set by editor_read_key when the key just returned arrived with the Shift
 * modifier (CSI form, e.g. Shift+Right is "\x1b[1;2C"). Read by
 * editor_process_keypress to extend a selection; smuggled out-of-band like
 * WINCH_KEY so the key enum stays a plain int. Reset on every call. */
static int last_key_shift = 0;

/* Whether the key just returned arrived with Shift held. Read once, right after
 * editor_read_key, by the code that decides whether to extend a selection. */
int input_last_key_had_shift(void) {
  return last_key_shift;
}

/* Read one byte, waiting up to `ticks` of the terminal's VTIME (100ms each).
 *
 * Only used once an escape sequence has begun. Giving up after a single tick is
 * the classic way to mangle input over a slow link: the sequence arrives split,
 * the first half is consumed, and the tail is delivered as ordinary characters --
 * which is how "\x1b[1;5C" used to type a literal "5C" into the buffer. Waiting
 * longer costs nothing, because a sequence that has started will finish. The
 * resize flag is deliberately not checked here: clearing it would lose the
 * resize, and a partly-read sequence has to be finished first anyway. */
static int read_byte_patient(unsigned char *c, int ticks) {
  int i;
  for (i = 0; i < ticks; i++)
    if (read_byte(c)) return 1;
  return 0;
}

/* How long to keep waiting for the rest of a sequence: ~300ms. Long enough for a
 * laggy connection, short enough that a genuinely truncated sequence does not
 * visibly hang the editor. */
#define SEQ_TICKS 3

int editor_read_key(void) {
  int nread;
  unsigned char c;
  last_key_shift = 0;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
    /* VMIN=0/VTIME=1 already wakes this loop roughly every 100ms even with
     * no key pressed; piggyback on that wakeup to notice a resize instead of
     * waiting for the next real keystroke to redraw at the new size. */
    if (term_take_winch()) return WINCH_KEY;
  }

  if (c != '\x1b') return c;

  unsigned char b;
  /* Esc and Alt are the same bytes: Alt-x arrives as ESC then 'x', and so does
   * Esc followed by typing x. Nothing distinguishes them but timing, so a bare
   * Esc is one that has nothing following it within a tick. This is what every
   * terminal editor does, and it is why Esc acts on a slight delay. */
  if (!read_byte(&b)) return '\x1b';

  /* Esc Esc: two presses, or Alt-Esc. Report one Esc and leave it at that. */
  if (b == '\x1b') return '\x1b';

  if (b == '[') {
    /* CSI: parameter bytes 0x30-0x3f, intermediates 0x20-0x2f, then a final
     * byte in 0x40-0x7e. Modifiers ride in the parameters (Ctrl-Right is
     * "\x1b[1;5C"), so the final byte alone selects the key. */
    char params[24];
    size_t n = 0;
    unsigned char final;
    for (;;) {
      if (!read_byte_patient(&b, SEQ_TICKS)) return '\x1b';
      if (b >= 0x40 && b <= 0x7e) { final = b; break; }
      if (n < sizeof(params) - 1) params[n++] = (char)b;
    }
    params[n] = '\0';

    /* The modifier, if any, rides after the ';' in the parameters ("\x1b[1;2C"
     * = Shift+Right). The value is one more than a bitfield (shift=1, alt=2,
     * ctrl=4), so a set low bit means Shift was held. Only consumed as a
     * side signal; the returned key is still selected by `final`, and the
     * sequence is always read to its terminator so nothing leaks to the
     * buffer regardless. */
    char *semi = strchr(params, ';');
    if (semi != NULL) {
      int mod = atoi(semi + 1);
      if (mod >= 2) last_key_shift = (mod - 1) & 0x01;
    }

    switch (final) {
      case 'A': return ARROW_UP;
      case 'B': return ARROW_DOWN;
      case 'C': return ARROW_RIGHT;
      case 'D': return ARROW_LEFT;
      case 'H': return HOME_KEY;
      case 'F': return END_KEY;
      /* F1-F4 also appear as CSI finals on some terminals, with modifiers
       * riding in the parameters exactly as they do for the arrows. */
      case 'P': return F1_KEY;
      case 'Q': return F2_KEY;
      case 'R': return F3_KEY;
      case 'S': return F4_KEY;
      case '~':
        switch (atoi(params)) {
          case 1: return HOME_KEY;
          case 3: return DEL_KEY;
          case 4: return END_KEY;
          case 5: return PAGE_UP;
          case 6: return PAGE_DOWN;
          case 7: return HOME_KEY;
          case 8: return END_KEY;
          /* Function keys. 11-14 are the Linux console's F1-F4; xterm sends
           * those as SS3 and starts at 15. There is no 16 or 22. */
          case 11: return F1_KEY;
          case 12: return F2_KEY;
          case 13: return F3_KEY;
          case 14: return F4_KEY;
          case 15: return F5_KEY;
          case 17: return F6_KEY;
          case 18: return F7_KEY;
          case 19: return F8_KEY;
          case 20: return F9_KEY;
          case 21: return F10_KEY;
          case 23: return F11_KEY;
          case 24: return F12_KEY;
        }
        break;
    }
    return '\x1b';
  }

  if (b == 'O') {
    /* SS3, sent for the cursor keys in application mode and for F1-F4. */
    if (!read_byte_patient(&b, SEQ_TICKS)) return '\x1b';
    switch (b) {
      case 'A': return ARROW_UP;
      case 'B': return ARROW_DOWN;
      case 'C': return ARROW_RIGHT;
      case 'D': return ARROW_LEFT;
      case 'H': return HOME_KEY;
      case 'F': return END_KEY;
      case 'P': return F1_KEY;
      case 'Q': return F2_KEY;
      case 'R': return F3_KEY;
      case 'S': return F4_KEY;
    }
    return '\x1b';
  }

  /* Alt + a printable character. Anything else following Esc -- a control byte,
   * say -- is consumed and reported as a plain Esc, as it always was. */
  if (b >= 0x20 && b <= 0x7e) return ALT_KEY(b);

  return '\x1b';
}
