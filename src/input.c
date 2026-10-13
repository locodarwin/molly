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

/* And with Ctrl. Same mechanism, added when Ctrl-PgUp and Ctrl-PgDn became the
 * keys that switch buffers: the modifier rides in the same CSI parameter, and
 * without reading it Ctrl-PgUp arrived as a plain PgUp and scrolled a page. */
static int last_key_ctrl = 0;

/* And with Alt, for the one family of chords that needs it on a navigation key:
 * Alt-Shift and an arrow, which draws a block selection. Alt on a letter is a
 * different key code altogether (ALT_KEY), as it always was. */
static int last_key_alt = 0;

/* Whether the key just returned arrived with Shift held. Read once, right after
 * editor_read_key, by the code that decides whether to extend a selection. */
int input_last_key_had_shift(void) {
  return last_key_shift;
}

int input_last_key_had_ctrl(void) {
  return last_key_ctrl;
}

int input_last_key_had_alt(void) {
  return last_key_alt;
}

/* The mouse report that came with the last MOUSE_KEY. Out of band for the same
 * reason the Shift flag is: an event carrying coordinates cannot be a chord that a
 * keymap binds to a command. */
static struct mouse_event last_mouse;

void input_last_mouse(struct mouse_event *out) {
  *out = last_mouse;
}

/* Decode an SGR mouse report: "<b;x;yM" for a press or drag, "<b;x;ym" for a
 * release. The button byte packs the button in its low two bits, the modifiers
 * above that, motion at 32 and the wheel at 64. Returns MOUSE_KEY, or Esc if the
 * report makes no sense -- the sequence has already been consumed either way, so a
 * malformed one cannot leak into the buffer. */
static int decode_mouse(const char *params) {
  int b, x, y;
  const char *p = params;

  if (*p != '<') return '\x1b';
  p++;
  if (sscanf(p, "%d;%d;%d", &b, &x, &y) != 3) return '\x1b';

  last_mouse.mods = 0;
  if (b & 4) last_mouse.mods |= MOUSE_MOD_SHIFT;
  if (b & 8) last_mouse.mods |= MOUSE_MOD_ALT;
  if (b & 16) last_mouse.mods |= MOUSE_MOD_CTRL;

  /* The terminal counts from one; everything inside molly counts from zero. */
  last_mouse.x = x - 1;
  last_mouse.y = y - 1;
  last_mouse.button = b & 0x03;

  if (b & 64) {
    /* Wheel. Buttons 64 and 65 are up and down; 66 and 67 are horizontal tilt on
     * terminals that report it, and are ignored rather than mistaken for a click. */
    if ((b & 0x03) == 0) last_mouse.type = MOUSE_WHEEL_UP;
    else if ((b & 0x03) == 1) last_mouse.type = MOUSE_WHEEL_DOWN;
    else return '\x1b';
    last_mouse.button = MOUSE_BUTTON_LEFT;
    return MOUSE_KEY;
  }
  if (b & 32) {
    last_mouse.type = MOUSE_DRAG;
    return MOUSE_KEY;
  }
  last_mouse.type = MOUSE_PRESS;   /* corrected to RELEASE by the caller on 'm' */
  return MOUSE_KEY;
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

/* Static, so that nothing can bypass the event loop by reading the terminal
 * directly: ui_wait_key below is the only caller, and the only way in. */
static int decode_key(unsigned char c);

static int editor_read_key(void) {
  int nread;
  unsigned char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
    /* VMIN=0/VTIME=1 already wakes this loop roughly every 100ms even with
     * no key pressed; piggyback on that wakeup to notice a resize instead of
     * waiting for the next real keystroke to redraw at the new size. */
    if (term_take_winch()) return WINCH_KEY;
  }
  return decode_key(c);
}

/* The key that begins with byte `c`, reading the rest of its sequence. */
static int decode_key(unsigned char c) {
  last_key_shift = 0;
  last_key_ctrl = 0;
  last_key_alt = 0;
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

    /* A mouse report is a CSI whose parameters start with '<'. It is checked before
     * the final-byte switch because it uses 'M' and 'm' as finals, which mean
     * nothing else to molly, and because its parameters are not the modifier form
     * the code below expects. */
    if (params[0] == '<') {
      int k = decode_mouse(params);
      if (k == MOUSE_KEY && final == 'm') last_mouse.type = MOUSE_RELEASE;
      return k;
    }

    /* The modifier, if any, rides after the ';' in the parameters ("\x1b[1;2C"
     * = Shift+Right). The value is one more than a bitfield (shift=1, alt=2,
     * ctrl=4), so a set low bit means Shift was held. Only consumed as a
     * side signal; the returned key is still selected by `final`, and the
     * sequence is always read to its terminator so nothing leaks to the
     * buffer regardless. */
    char *semi = strchr(params, ';');
    if (semi != NULL) {
      int mod = atoi(semi + 1);
      if (mod >= 2) {
        last_key_shift = ((mod - 1) & 0x01) ? 1 : 0;
        last_key_ctrl = ((mod - 1) & 0x04) ? 1 : 0;
        last_key_alt = ((mod - 1) & 0x02) ? 1 : 0;
      }
    }

    switch (final) {
      case 'A': return ARROW_UP;
      case 'B': return ARROW_DOWN;
      case 'C': return ARROW_RIGHT;
      case 'D': return ARROW_LEFT;
      case 'H': return HOME_KEY;
      case 'F': return END_KEY;
      /* Shift-Tab. Tab itself is a plain control byte, but its shifted form is a
       * sequence of its own, and without this it fell through to Esc -- which in
       * a dialog meant that going backwards through the focus order cancelled
       * the dialog. */
      case 'Z': return BACKTAB_KEY;
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
  /* Alt-Backspace, which a terminal sends as Esc and DEL: the one Alt chord on
   * a key that is not printable, and the one readline and every shell already
   * use for deleting a word. */
  if (b == 0x7f) return ALT_KEY(0x7f);
  /* And Alt-Enter, Esc and CR, which is code actions' key. */
  if (b == '\r') return ALT_KEY('\r');

  return '\x1b';
}

/* Wait for a keystroke, servicing the event loop while we wait.
 *
 * This is what every loop in the editor should call, and the difference from
 * editor_read_key is the whole point: read_key blocks on the terminal, so while a
 * prompt, the help screen or the open dialog was up, nothing else in molly ran.
 * That was harmless while the keyboard was the only thing that could happen. It
 * stops being harmless the moment a language server is sending diagnostics or an
 * integrated terminal is producing output, either of which would simply stall for
 * as long as a dialog was open.
 *
 * A resize is reported as WINCH_KEY, exactly as read_key reports it, so callers
 * that repaint on a resize keep working. It is checked first so a resize is not
 * held up behind a keystroke; a key that arrives at the same moment stays in the
 * terminal's buffer and is read on the next call. */
/* Has Esc been pressed, without waiting for it?
 *
 * For code that is in the middle of something long and wants to be
 * interruptible -- the shell filter, which consults this between polls of a
 * running command. A blocking read here would hang the editor on the keyboard
 * instead of on the command, so the question is asked of loop_wait with no
 * timeout, which is the one place that knows how to ask it.
 *
 * It consumes the keystroke it finds, Esc or not. That is deliberate: keys
 * pressed while a command is running have nowhere to go, and queuing them to
 * be typed into the text afterwards would be worse than dropping them. */
int input_escape_pressed(void) {
  unsigned char c;
  if (!(loop_wait(0) & LOOP_STDIN)) return 0;
  /* One read, not editor_read_key's wait for a key: standard input at end of
   * file is readable for ever and reads nothing, and waiting there would hold
   * the command being cancelled -- and its time limit -- for ever with it. */
  if (read(STDIN_FILENO, &c, 1) != 1) return 0;
  return decode_key(c) == '\x1b';
}

/* Something that is not a keystroke has changed what is on screen -- a language
 * server's diagnostics arriving, say -- and the frame should be drawn again.
 *
 * Reported as WINCH_KEY, which every key loop in molly already treats as "draw
 * again and keep waiting": the main loop dispatches it to a command that does
 * nothing, and every modal loop `continue`s on it to the top, where it paints.
 * A key of its own would have meant teaching all eleven loops about it. */
static int redraw_wanted;

void ui_request_redraw(void) { redraw_wanted = 1; }

/* ---- keyboard macros ------------------------------------------------------
 *
 * Recorded here, in the one place molly waits for a key, rather than as a
 * list of commands: so typing, a prompt's answer and a dialog's choices are
 * all in a macro without anything else knowing there is one, and a replay
 * feeds the keys back through the same door they came in by, modifiers and
 * all. One macro, of up to MACRO_MAX keys. Resizes and the mouse are not
 * recorded: neither means the same thing twice. */
#define MACRO_MAX 4096

static struct { int key; unsigned char shift, ctrl, alt; } macro[MACRO_MAX];
static int macro_len, macro_pos = -1, recording;

int macro_recording(void) { return recording; }
int macro_playing(void) { return macro_pos >= 0; }

/* Alt-(: start recording, or stop. The key that stops it was recorded on
 * its way in, and is taken back off. Returns the keys recorded on stopping. */
int macro_toggle(void) {
  if (macro_pos >= 0) return -1;
  if (!recording) {
    recording = 1;
    macro_len = 0;
    return 0;
  }
  recording = 0;
  if (macro_len > 0) macro_len--;
  return macro_len;
}

/* Alt-): replay it. The keys come out of ui_wait_key() from here on, ahead of
 * the keyboard, until they run out. */
int macro_play(void) {
  if (recording || macro_len == 0 || macro_pos >= 0) return 0;
  macro_pos = 0;
  return 1;
}

int ui_wait_key(void) {
  if (macro_pos >= 0) {
    if (macro_pos < macro_len) {
      int k = macro[macro_pos].key;
      last_key_shift = macro[macro_pos].shift;
      last_key_ctrl = macro[macro_pos].ctrl;
      last_key_alt = macro[macro_pos].alt;
      macro_pos++;
      return k;
    }
    macro_pos = -1;
  }
  for (;;) {
    int ready = loop_wait(LOOP_TICK_MS);
    if (term_take_winch()) return WINCH_KEY;
    if (ready & LOOP_STDIN) {
      int k = editor_read_key();
      if (recording && k != WINCH_KEY && k != MOUSE_KEY && macro_len < MACRO_MAX) {
        macro[macro_len].key = k;
        macro[macro_len].shift = (unsigned char)last_key_shift;
        macro[macro_len].ctrl = (unsigned char)last_key_ctrl;
        macro[macro_len].alt = (unsigned char)last_key_alt;
        macro_len++;
      }
      return k;
    }
    /* A whole tick with no key in it: the user has paused, which is when the
     * language servers are told what has changed. Not on every key, so that a
     * word typed quickly is sent once and not once per letter. */
    if (ready == 0) {
      lsp_idle();
      git_idle();
    }
    if (redraw_wanted) {
      redraw_wanted = 0;
      return WINCH_KEY;
    }
  }
}
