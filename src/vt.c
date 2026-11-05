/* vt.c -- a terminal emulator, for the integrated terminal.
 *
 * A program running in molly's terminal writes the same bytes it would write to
 * xterm: text, and escape sequences that move the cursor, color what comes
 * next, clear a line, scroll a region, switch to an alternate screen. This file
 * reads those bytes and keeps the screen they describe -- a grid of cells, a
 * cursor, a pen, the modes -- plus the lines that have scrolled off the top.
 *
 * It does nothing else. No pty, no buffer, no drawing, no keyboard: bytes in,
 * screen state out, and the few bytes a program asks the terminal to send back
 * (where is the cursor, what are you) kept for the caller to deliver. That is
 * the same split filter.c and lsp.c make, and for the same reason: every
 * sequence below is a unit test that feeds bytes and reads cells, with no
 * process and no terminal involved.
 *
 * What is understood is the subset of xterm that bash, readline, less, vim,
 * htop, git and the common build tools actually use -- found by what they emit,
 * not by reading the whole of ECMA-48. The child is told it is in an
 * `xterm-256color`, because that is what every program has a terminfo entry
 * for; the sequences an xterm-256color entry promises are all here. Anything
 * else is parsed to its end and ignored, which is what a terminal must do with
 * a sequence it does not know: an unknown sequence that leaked its parameters
 * onto the screen would be worse than one that did nothing.
 *
 * Deliberately not here:
 *   - **Reflow.** A line is as wide as the screen was when it was written, and
 *     a resize truncates or pads it. Re-wrapping needs to know which rows were
 *     one line wrapped and which were two, through every scroll, erase and
 *     insert below; and re-wrapping the live screen fights the shell, which
 *     redraws its own prompt on SIGWINCH and would then show it twice. What
 *     would be left worth having is old output in the scrollback, re-wrapped,
 *     and that is not worth the bookkeeping.
 *   - **Color depth.** A 24-bit color is kept as it is, and brought down
 *     to what the terminal can show when the frame is written. */

#include "molly.h"

#define VT_MAX_PARAMS 32
#define VT_OSC_MAX    512

/* Where the parser is in a sequence. */
enum {
  ST_GROUND = 0,
  ST_ESC,        /* after ESC */
  ST_ESC_INTER,  /* after ESC and an intermediate: ESC ( B, ESC # 8 */
  ST_CSI,        /* after ESC [ */
  ST_OSC,        /* after ESC ], until BEL or ST */
  ST_OSC_ESC,    /* an ESC inside an OSC, which may be the start of ST */
  ST_IGNORE,     /* a DCS, SOS, PM or APC: skipped to its ST */
  ST_IGNORE_ESC
};

struct vt_line {
  struct vt_cell *c;
  int n;
};

struct saved_cursor {
  int cy, cx;
  int fg, bg;
  unsigned char attrs;
  int g0_graphics;
  int origin;
};

struct vt {
  int rows, cols;
  struct vt_cell *main, *alt, *screen;   /* screen is main or alt */
  int alt_on;

  int cy, cx;
  int wrapnext;          /* the last column was written: the next char wraps */
  int fg, bg;            /* the pen */
  unsigned char attrs;
  int top, bottom;       /* the scroll region, inclusive */

  /* Modes. */
  int autowrap, insert, origin, cursor_visible, app_cursor, app_keypad;
  int bracketed_paste;
  int mouse;             /* 0, 1000, 1002 or 1003 */
  int mouse_sgr;         /* 1006: reported as CSI < ... M, with no limit on size */
  int g0_graphics, g1_graphics, shift_out;

  unsigned char *tabs;   /* one per column: a stop here */

  struct saved_cursor saved_main, saved_alt;

  /* The parser. */
  int state;
  int params[VT_MAX_PARAMS];
  unsigned char sub[VT_MAX_PARAMS];  /* joined to the one before by ':' */
  int nparams;
  int have_digit;
  char priv;             /* '?', '>', '=' or '<' at the start of a CSI */
  char inter;            /* an intermediate byte: ' ', '!', '$', '"' */
  char osc[VT_OSC_MAX];
  int osclen;
  uint32_t ucp;          /* a UTF-8 sequence being decoded */
  int uneed;

  /* Lines scrolled off the top of the main screen: a ring, oldest first. */
  struct vt_line *sb;
  int sbcap, sbn, sbhead;
  long pushed;           /* lines ever pushed, less any taken back */

  struct abuf reply;     /* for the program: cursor reports and the like */
  char title[128];
  int title_changed;
  int bell;
};

/* ---- cells and lines ----------------------------------------------------- */

static struct vt_cell blank_with(const struct vt *t) {
  struct vt_cell c;
  memset(&c, 0, sizeof(c));
  c.cp = ' ';
  c.w = 1;
  c.fg = -1;
  /* Erasing paints the current background, as xterm does ("bce"), so a
   * program that sets a background and clears the screen gets that color. */
  c.bg = t->bg;
  return c;
}

static struct vt_cell *row_ptr(struct vt *t, int y) {
  return &t->screen[(size_t)y * (size_t)t->cols];
}

static void fill(struct vt *t, int y, int x0, int x1) {
  struct vt_cell b = blank_with(t), *r;
  int x;
  if (y < 0 || y >= t->rows) return;
  if (x0 < 0) x0 = 0;
  if (x1 > t->cols) x1 = t->cols;
  r = row_ptr(t, y);
  for (x = x0; x < x1; x++) r[x] = b;
}

/* A line pushed into the scrollback: copied without its trailing blanks, which
 * is most of a shell's output, so ten thousand lines of `ls` do not each cost a
 * screen's width. */
static void sb_push(struct vt *t, const struct vt_cell *r) {
  int n = t->cols, slot;
  struct vt_line *l;
  if (t->sbcap == 0) return;
  t->pushed++;
  while (n > 0 && r[n - 1].cp == ' ' && r[n - 1].bg < 0 && r[n - 1].attrs == 0 &&
         r[n - 1].w == 1)
    n--;
  if (t->sbn < t->sbcap) {
    slot = (t->sbhead + t->sbn) % t->sbcap;
    t->sbn++;
  } else {
    slot = t->sbhead;
    t->sbhead = (t->sbhead + 1) % t->sbcap;
    free(t->sb[slot].c);
  }
  l = &t->sb[slot];
  l->n = n;
  l->c = n > 0 ? xmalloc(sizeof(*l->c) * (size_t)n) : NULL;
  if (n > 0) memcpy(l->c, r, sizeof(*l->c) * (size_t)n);
}

/* Take the newest line back out of the scrollback, for a screen that grew. */
static int sb_pop(struct vt *t, struct vt_cell *dst) {
  struct vt_line *l;
  int x;
  if (t->sbn == 0) return 0;
  l = &t->sb[(t->sbhead + t->sbn - 1) % t->sbcap];
  for (x = 0; x < t->cols; x++) {
    if (x < l->n) dst[x] = l->c[x];
    else dst[x] = blank_with(t), dst[x].bg = -1;
  }
  free(l->c);
  l->c = NULL;
  t->sbn--;
  t->pushed--;
  return 1;
}

static void sb_clear(struct vt *t) {
  int i;
  for (i = 0; i < t->sbn; i++) free(t->sb[(t->sbhead + i) % t->sbcap].c);
  t->sbn = 0;
  t->sbhead = 0;
}

/* Rows [top, bottom] up by n, blank rows in at the bottom. Off the top of the
 * whole main screen is into the scrollback; anywhere else is gone, which is
 * what a scroll region inside an editor or a pager means. */
static void scroll_up(struct vt *t, int top, int bottom, int n) {
  int y;
  if (n <= 0) return;
  if (n > bottom - top + 1) n = bottom - top + 1;
  if (top == 0 && !t->alt_on)
    for (y = 0; y < n; y++) sb_push(t, row_ptr(t, y));
  memmove(row_ptr(t, top), row_ptr(t, top + n),
          sizeof(struct vt_cell) * (size_t)t->cols * (size_t)(bottom - top + 1 - n));
  for (y = bottom - n + 1; y <= bottom; y++) fill(t, y, 0, t->cols);
}

static void scroll_down(struct vt *t, int top, int bottom, int n) {
  int y;
  if (n <= 0) return;
  if (n > bottom - top + 1) n = bottom - top + 1;
  memmove(row_ptr(t, top + n), row_ptr(t, top),
          sizeof(struct vt_cell) * (size_t)t->cols * (size_t)(bottom - top + 1 - n));
  for (y = top; y < top + n; y++) fill(t, y, 0, t->cols);
}

/* ---- the cursor ---------------------------------------------------------- */

static void clamp_cursor(struct vt *t) {
  if (t->cx < 0) t->cx = 0;
  if (t->cx >= t->cols) t->cx = t->cols - 1;
  if (t->cy < 0) t->cy = 0;
  if (t->cy >= t->rows) t->cy = t->rows - 1;
}

/* A move resets the pending wrap: the next character goes where the cursor
 * now is, not at the start of the line after. */
static void move_to(struct vt *t, int y, int x) {
  t->cy = y;
  t->cx = x;
  t->wrapnext = 0;
  clamp_cursor(t);
}

/* Origin mode makes rows count from the top of the scroll region and keeps the
 * cursor inside it. */
static void move_abs(struct vt *t, int y, int x) {
  if (t->origin) {
    y += t->top;
    if (y > t->bottom) y = t->bottom;
  }
  move_to(t, y, x);
}

static void linefeed(struct vt *t) {
  t->wrapnext = 0;
  if (t->cy == t->bottom) scroll_up(t, t->top, t->bottom, 1);
  else if (t->cy < t->rows - 1) t->cy++;
}

static void reverse_index(struct vt *t) {
  t->wrapnext = 0;
  if (t->cy == t->top) scroll_down(t, t->top, t->bottom, 1);
  else if (t->cy > 0) t->cy--;
}

/* A count past the width moves no further, and is cut to it: a sequence of a
 * few bytes must not buy a million turns of the loop. */
static void tab_forward(struct vt *t, int n) {
  if (n > t->cols) n = t->cols;
  while (n-- > 0) {
    int x = t->cx + 1;
    while (x < t->cols - 1 && !t->tabs[x]) x++;
    if (x > t->cols - 1) x = t->cols - 1;
    t->cx = x;
  }
  t->wrapnext = 0;
}

static void tab_back(struct vt *t, int n) {
  if (n > t->cols) n = t->cols;
  while (n-- > 0) {
    int x = t->cx - 1;
    while (x > 0 && !t->tabs[x]) x--;
    t->cx = x < 0 ? 0 : x;
  }
  t->wrapnext = 0;
}

static void save_cursor(struct vt *t) {
  struct saved_cursor *s = t->alt_on ? &t->saved_alt : &t->saved_main;
  s->cy = t->cy;
  s->cx = t->cx;
  s->fg = t->fg;
  s->bg = t->bg;
  s->attrs = t->attrs;
  s->g0_graphics = t->g0_graphics;
  s->origin = t->origin;
}

static void restore_cursor(struct vt *t) {
  struct saved_cursor *s = t->alt_on ? &t->saved_alt : &t->saved_main;
  t->fg = s->fg;
  t->bg = s->bg;
  t->attrs = s->attrs;
  t->g0_graphics = s->g0_graphics;
  t->origin = s->origin;
  move_to(t, s->cy, s->cx);
}

/* ---- writing a character ------------------------------------------------- */

/* DEC special graphics: what `ESC ( 0` turns the lower case letters into, and
 * how tmux, mc and dialog draw their boxes on a terminal they cannot assume
 * UTF-8 of. 0x60 to 0x7e. */
static const uint16_t dec_graphics[31] = {
  0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0, 0x00b1,
  0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c, 0x23ba,
  0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534, 0x252c,
  0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7,
};

static void put_char(struct vt *t, uint32_t cp) {
  struct vt_cell *r, c;
  int w;

  if ((t->shift_out ? t->g1_graphics : t->g0_graphics) && cp >= 0x60 && cp <= 0x7e)
    cp = dec_graphics[cp - 0x60];
  w = utf8_cp_cols(cp);

  if (w == 0) {
    /* A combining mark joins the character before it; a second one is
     * dropped, which shows the base character rather than nothing. */
    int x = t->wrapnext ? t->cx : t->cx - 1;
    r = row_ptr(t, t->cy);
    while (x > 0 && r[x].w == 0) x--;
    if (x >= 0 && r[x].comb == 0) r[x].comb = cp;
    return;
  }

  if (t->wrapnext) {
    if (t->autowrap) {
      t->cx = 0;
      linefeed(t);
    }
    t->wrapnext = 0;
  }
  /* A wide character with one column left goes to the next line whole. */
  if (w == 2 && t->cx == t->cols - 1) {
    if (t->autowrap) {
      fill(t, t->cy, t->cx, t->cols);
      t->cx = 0;
      linefeed(t);
    } else {
      w = 1;
      cp = ' ';
    }
  }
  if (w > t->cols) return;

  r = row_ptr(t, t->cy);
  if (t->insert) {
    memmove(&r[t->cx + w], &r[t->cx],
            sizeof(*r) * (size_t)(t->cols - t->cx - w));
  }
  /* Overwriting half of a wide character leaves the other half meaningless:
   * it becomes a blank. */
  if (r[t->cx].w == 0 && t->cx > 0) r[t->cx - 1] = blank_with(t);
  if (t->cx + w < t->cols && r[t->cx + w].w == 0) r[t->cx + w] = blank_with(t);

  memset(&c, 0, sizeof(c));
  c.cp = cp;
  c.w = (unsigned char)w;
  c.fg = t->fg;
  c.bg = t->bg;
  c.attrs = t->attrs;
  r[t->cx] = c;
  if (w == 2) {
    c.cp = 0;
    c.w = 0;
    r[t->cx + 1] = c;
  }
  if (t->cx + w >= t->cols) {
    t->cx = t->cols - 1;
    t->wrapnext = 1;
  } else {
    t->cx += w;
  }
}

/* ---- erasing and editing ------------------------------------------------- */

static void erase_display(struct vt *t, int how) {
  int y;
  if (how == 0) {
    fill(t, t->cy, t->cx, t->cols);
    for (y = t->cy + 1; y < t->rows; y++) fill(t, y, 0, t->cols);
  } else if (how == 1) {
    for (y = 0; y < t->cy; y++) fill(t, y, 0, t->cols);
    fill(t, t->cy, 0, t->cx + 1);
  } else if (how == 2) {
    for (y = 0; y < t->rows; y++) fill(t, y, 0, t->cols);
  } else if (how == 3) {
    /* `clear` sends this after 2: the scrollback goes too. */
    sb_clear(t);
  }
}

static void erase_line(struct vt *t, int how) {
  if (how == 0) fill(t, t->cy, t->cx, t->cols);
  else if (how == 1) fill(t, t->cy, 0, t->cx + 1);
  else if (how == 2) fill(t, t->cy, 0, t->cols);
}

static void insert_chars(struct vt *t, int n) {
  struct vt_cell *r = row_ptr(t, t->cy);
  if (n > t->cols - t->cx) n = t->cols - t->cx;
  memmove(&r[t->cx + n], &r[t->cx], sizeof(*r) * (size_t)(t->cols - t->cx - n));
  fill(t, t->cy, t->cx, t->cx + n);
  t->wrapnext = 0;
}

static void delete_chars(struct vt *t, int n) {
  struct vt_cell *r = row_ptr(t, t->cy);
  if (n > t->cols - t->cx) n = t->cols - t->cx;
  memmove(&r[t->cx], &r[t->cx + n], sizeof(*r) * (size_t)(t->cols - t->cx - n));
  fill(t, t->cy, t->cols - n, t->cols);
  t->wrapnext = 0;
}

/* Insert and delete lines happen only inside the scroll region, and only with
 * the cursor in it; outside, they do nothing. */
static void insert_lines(struct vt *t, int n) {
  if (t->cy < t->top || t->cy > t->bottom) return;
  scroll_down(t, t->cy, t->bottom, n);
  t->cx = 0;
  t->wrapnext = 0;
}

static void delete_lines(struct vt *t, int n) {
  if (t->cy < t->top || t->cy > t->bottom) return;
  /* Within the region, so nothing goes to the scrollback even at the top. */
  {
    int was = t->alt_on;
    t->alt_on = 1;
    scroll_up(t, t->cy, t->bottom, n);
    t->alt_on = was;
  }
  t->cx = 0;
  t->wrapnext = 0;
}

/* ---- the alternate screen ------------------------------------------------ */

/* Where a full-screen program draws, so that leaving it puts the shell's
 * screen back as it was. No scrollback: what vim draws is not history. */
static void alt_screen(struct vt *t, int on, int clear) {
  int y;
  if (on == t->alt_on) return;
  t->alt_on = on;
  t->screen = on ? t->alt : t->main;
  if (on && clear)
    for (y = 0; y < t->rows; y++) fill(t, y, 0, t->cols);
  t->wrapnext = 0;
}

/* ---- the pen ------------------------------------------------------------- */

/* 38 or 48 and what follows it: `5;n` for an index, `2;r;g;b` for a color.
 * The same with colons, where `2` may be followed by a color-space id that
 * is empty in practice. Returns how many parameters after `i` it used. */
static int ext_color(const struct vt *t, int i, int *out) {
  int k = i + 1, kind;
  if (k >= t->nparams) return 0;
  kind = t->params[k];
  if (kind == 5 && k + 1 < t->nparams) {
    int c = t->params[k + 1];
    if (c >= 0 && c <= 255) *out = c;
    return 2;
  }
  if (kind == 2) {
    int first = k + 1;
    /* With colons, `38:2::r:g:b` has an empty color-space slot; with
     * semicolons there is none. Four sub-parameters means the slot is there. */
    if (t->sub[k + 1] && k + 4 < t->nparams && t->sub[k + 4]) first = k + 2;
    if (first + 2 < t->nparams) {
      int r = t->params[first], g = t->params[first + 1], b = t->params[first + 2];
      if (r < 0) r = 0;
      if (g < 0) g = 0;
      if (b < 0) b = 0;
      /* Kept as the color it is: the grid brings it down to what the
       * terminal can show as the frame is written (theme_fit_color). */
      *out = COLOR_RGB | ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) |
             (b > 255 ? 255 : b);
      return first + 2 - i;
    }
  }
  return 1;
}

static void sgr(struct vt *t) {
  int i;
  if (t->nparams == 0) {
    t->fg = t->bg = -1;
    t->attrs = 0;
    return;
  }
  for (i = 0; i < t->nparams; i++) {
    int p = t->params[i] < 0 ? 0 : t->params[i];
    /* A sub-parameter belongs to the parameter before it and is read there. */
    if (t->sub[i]) continue;
    switch (p) {
    case 0: t->fg = t->bg = -1; t->attrs = 0; break;
    case 1: t->attrs |= VT_BOLD; break;
    case 2: t->attrs |= VT_DIM; break;
    case 3: t->attrs |= VT_ITALIC; break;
    case 4:
      /* 4:0 is "no underline" in the colon form; any other style is one. */
      if (i + 1 < t->nparams && t->sub[i + 1] && t->params[i + 1] == 0)
        t->attrs &= (unsigned char)~VT_UNDERLINE;
      else
        t->attrs |= VT_UNDERLINE;
      break;
    case 7: t->attrs |= VT_REVERSE; break;
    case 8: t->attrs |= VT_INVISIBLE; break;
    case 21: t->attrs |= VT_UNDERLINE; break;
    case 22: t->attrs &= (unsigned char)~(VT_BOLD | VT_DIM); break;
    case 23: t->attrs &= (unsigned char)~VT_ITALIC; break;
    case 24: t->attrs &= (unsigned char)~VT_UNDERLINE; break;
    case 27: t->attrs &= (unsigned char)~VT_REVERSE; break;
    case 28: t->attrs &= (unsigned char)~VT_INVISIBLE; break;
    case 38: i += ext_color(t, i, &t->fg); break;
    case 39: t->fg = -1; break;
    case 48: i += ext_color(t, i, &t->bg); break;
    case 49: t->bg = -1; break;
    default:
      if (p >= 30 && p <= 37) t->fg = p - 30;
      else if (p >= 40 && p <= 47) t->bg = p - 40;
      else if (p >= 90 && p <= 97) t->fg = p - 90 + 8;
      else if (p >= 100 && p <= 107) t->bg = p - 100 + 8;
      /* 5 blink, 9 strike, 53 overline and the rest: not drawn. */
      break;
    }
  }
}

/* ---- modes --------------------------------------------------------------- */

static void reset(struct vt *t);

static void set_mode(struct vt *t, int on) {
  int i;
  for (i = 0; i < t->nparams; i++) {
    int p = t->params[i];
    if (t->priv == '?') {
      switch (p) {
      case 1: t->app_cursor = on; break;
      case 6:
        t->origin = on;
        move_abs(t, 0, 0);
        break;
      case 7: t->autowrap = on; if (!on) t->wrapnext = 0; break;
      case 25: t->cursor_visible = on; break;
      case 47:
      case 1047:
        alt_screen(t, on, p == 1047 && on);
        break;
      case 1048:
        if (on) save_cursor(t);
        else restore_cursor(t);
        break;
      case 1049:
        /* Save, switch and clear going in; switch back and restore coming out:
         * the order is what puts the shell's cursor back where it was. */
        if (on) {
          save_cursor(t);
          alt_screen(t, 1, 1);
        } else {
          alt_screen(t, 0, 0);
          restore_cursor(t);
        }
        break;
      case 2004: t->bracketed_paste = on; break;
      case 1000:
      case 1002:
      case 1003:
        /* One level, the last asked for: turning off any of them turns off
         * reporting, which is what programs that set one and reset it expect. */
        t->mouse = on ? p : 0;
        break;
      case 1006: t->mouse_sgr = on; break;
      default: break;  /* focus events, blink, the other mouse encodings */
      }
    } else if (t->priv == 0) {
      if (p == 4) t->insert = on;
    }
  }
}

/* ---- dispatching a sequence ---------------------------------------------- */

static int param(const struct vt *t, int i, int dflt) {
  if (i >= t->nparams || t->params[i] <= 0) return dflt;
  return t->params[i];
}

/* An answer for the program. Kept to a few kilobytes between reads: a program
 * that asks and never reads the answers -- `cat` of a file full of questions --
 * would otherwise have them pile up at twice the size of what it wrote. A
 * program that reads them never has more than one or two waiting. */
#define VT_REPLY_MAX 4096

static void reply(struct vt *t, const char *s) {
  int n = (int)strlen(s);
  if (t->reply.len + n > VT_REPLY_MAX) return;
  ab_append(&t->reply, s, n);
}

static void csi_dispatch(struct vt *t, char f) {
  int n = param(t, 0, 1);
  char buf[48];

  if (t->priv == '>' || t->priv == '=' || t->priv == '<') {
    /* Secondary device attributes: a VT220, version 10. Programs that ask are
     * deciding what they may send, and a plausible answer is all they need. */
    if (f == 'c' && t->priv == '>') reply(t, "\x1b[>1;10;0c");
    return;
  }
  if (t->inter != 0) {
    /* DECSTR, CSI ! p: a soft reset. Cursor style, DECRQM and the like: no. */
    if (t->inter == '!' && f == 'p') {
      t->autowrap = 1;
      t->insert = 0;
      t->origin = 0;
      t->cursor_visible = 1;
      t->app_cursor = 0;
      t->top = 0;
      t->bottom = t->rows - 1;
      t->fg = t->bg = -1;
      t->attrs = 0;
    }
    return;
  }

  switch (f) {
  case '@': if (t->priv == 0) insert_chars(t, n); break;
  case 'A': move_to(t, t->cy - n < (t->cy >= t->top ? t->top : 0) ? (t->cy >= t->top ? t->top : 0) : t->cy - n, t->cx); break;
  case 'B': case 'e':
    move_to(t, t->cy + n > (t->cy <= t->bottom ? t->bottom : t->rows - 1)
                   ? (t->cy <= t->bottom ? t->bottom : t->rows - 1) : t->cy + n, t->cx);
    break;
  case 'C': case 'a': move_to(t, t->cy, t->cx + n); break;
  case 'D': move_to(t, t->cy, t->cx - n); break;
  case 'E': move_to(t, t->cy + n, 0); break;
  case 'F': move_to(t, t->cy - n, 0); break;
  case 'G': case '`': move_to(t, t->cy, n - 1); break;
  case 'H': case 'f': move_abs(t, param(t, 0, 1) - 1, param(t, 1, 1) - 1); break;
  case 'I': tab_forward(t, n); break;
  case 'J':
    if (t->priv == 0 || t->priv == '?') erase_display(t, t->nparams ? t->params[0] < 0 ? 0 : t->params[0] : 0);
    break;
  case 'K':
    if (t->priv == 0 || t->priv == '?') erase_line(t, t->nparams ? t->params[0] < 0 ? 0 : t->params[0] : 0);
    break;
  case 'L': insert_lines(t, n); break;
  case 'M': delete_lines(t, n); break;
  case 'P': delete_chars(t, n); break;
  case 'S': if (t->priv == 0) scroll_up(t, t->top, t->bottom, n); break;
  case 'T': if (t->priv == 0 && t->nparams <= 1) scroll_down(t, t->top, t->bottom, n); break;
  case 'X': {
    int x1 = t->cx + n > t->cols ? t->cols : t->cx + n;
    fill(t, t->cy, t->cx, x1);
    t->wrapnext = 0;
    break;
  }
  case 'Z': tab_back(t, n); break;
  case 'b': {
    /* REP: the character before the cursor, again. At most a screenful: more
     * only scrolls copies of one line through the scrollback, and ten bytes
     * asking for 65,535 of them made output a thousand times its size. */
    struct vt_cell *r = row_ptr(t, t->cy);
    int x = t->wrapnext ? t->cx : t->cx - 1;
    if (x >= 0 && r[x].w > 0) {
      uint32_t cp = r[x].cp;
      int save_g0 = t->g0_graphics, save_g1 = t->g1_graphics;
      t->g0_graphics = t->g1_graphics = 0;   /* already translated */
      if (n > t->rows * t->cols) n = t->rows * t->cols;
      while (n-- > 0) put_char(t, cp);
      t->g0_graphics = save_g0;
      t->g1_graphics = save_g1;
    }
    break;
  }
  case 'c':
    /* Primary device attributes: a VT100 with advanced video. */
    if (param(t, 0, 0) == 0) reply(t, "\x1b[?1;2c");
    break;
  case 'd': move_abs(t, n - 1, t->cx); break;
  case 'g':
    if (param(t, 0, 0) == 0) t->tabs[t->cx] = 0;
    else if (t->nparams && t->params[0] == 3) memset(t->tabs, 0, (size_t)t->cols);
    break;
  case 'h': set_mode(t, 1); break;
  case 'l': set_mode(t, 0); break;
  case 'm': if (t->priv == 0) sgr(t); break;
  case 'n':
    if (t->priv != 0) break;
    if (param(t, 0, 0) == 5) {
      reply(t, "\x1b[0n");
    } else if (param(t, 0, 0) == 6) {
      int row = t->cy + 1 - (t->origin ? t->top : 0);
      snprintf(buf, sizeof(buf), "\x1b[%d;%dR", row, t->cx + 1);
      reply(t, buf);
    }
    break;
  case 'r': {
    int top = param(t, 0, 1) - 1, bottom = param(t, 1, t->rows) - 1;
    if (t->priv != 0) break;
    if (bottom >= t->rows) bottom = t->rows - 1;
    if (top < bottom) {
      t->top = top;
      t->bottom = bottom;
      move_abs(t, 0, 0);
    }
    break;
  }
  case 's': if (t->priv == 0 && t->nparams == 0) save_cursor(t); break;
  case 'u': if (t->priv == 0) restore_cursor(t); break;
  default: break;
  }
}

static void esc_dispatch(struct vt *t, char f) {
  switch (f) {
  case '7': save_cursor(t); break;
  case '8': restore_cursor(t); break;
  case 'D': linefeed(t); break;
  case 'E': t->cx = 0; linefeed(t); break;
  case 'H': t->tabs[t->cx] = 1; break;
  case 'M': reverse_index(t); break;
  case 'c': reset(t); break;
  case '=': t->app_keypad = 1; break;
  case '>': t->app_keypad = 0; break;
  default: break;
  }
}

static void esc_inter_dispatch(struct vt *t, char inter, char f) {
  if (inter == '(') t->g0_graphics = (f == '0');
  else if (inter == ')') t->g1_graphics = (f == '0');
  else if (inter == '#' && f == '8') {
    /* DECALN: the screen full of E, which is how vttest checks alignment. */
    int y, x;
    for (y = 0; y < t->rows; y++)
      for (x = 0; x < t->cols; x++) {
        struct vt_cell *c = &row_ptr(t, y)[x];
        *c = blank_with(t);
        c->cp = 'E';
      }
    move_to(t, 0, 0);
  }
}

static void osc_dispatch(struct vt *t) {
  int ps = 0, i = 0;
  t->osc[t->osclen] = '\0';
  while (i < t->osclen && isdigit((unsigned char)t->osc[i])) ps = ps * 10 + (t->osc[i++] - '0');
  if (i >= t->osclen || t->osc[i] != ';') return;
  i++;
  /* 0 and 2 are the window title, which molly shows as the buffer's name. The
   * rest -- colors, the clipboard, hyperlinks, the working directory -- are
   * not taken: OSC 52 especially, which would let any program that writes to
   * the terminal set molly's clipboard. */
  if (ps == 0 || ps == 2) {
    int k, o = 0;
    for (k = i; k < t->osclen && o + 1 < (int)sizeof(t->title); k++)
      if ((unsigned char)t->osc[k] >= 0x20) t->title[o++] = t->osc[k];
    t->title[o] = '\0';
    t->title_changed = 1;
  }
}

/* ---- the parser ---------------------------------------------------------- */

static void csi_begin(struct vt *t) {
  t->state = ST_CSI;
  t->nparams = 0;
  t->have_digit = 0;
  t->priv = 0;
  t->inter = 0;
  memset(t->sub, 0, sizeof(t->sub));
}

static void control(struct vt *t, unsigned char c) {
  switch (c) {
  case 0x07: t->bell = 1; break;
  case 0x08:
    if (t->cx > 0) t->cx--;
    t->wrapnext = 0;
    break;
  case 0x09: tab_forward(t, 1); break;
  case 0x0a: case 0x0b: case 0x0c: linefeed(t); break;
  case 0x0d: t->cx = 0; t->wrapnext = 0; break;
  case 0x0e: t->shift_out = 1; break;
  case 0x0f: t->shift_out = 0; break;
  default: break;
  }
}

static void feed_byte(struct vt *t, unsigned char c) {
  /* CAN and SUB abandon whatever sequence is in progress, and ESC starts a new
   * one from anywhere -- except inside a string, where ESC may be its end. */
  if (t->state != ST_OSC && t->state != ST_IGNORE &&
      t->state != ST_OSC_ESC && t->state != ST_IGNORE_ESC) {
    if (c == 0x18 || c == 0x1a) {
      t->state = ST_GROUND;
      return;
    }
    if (c == 0x1b) {
      t->state = ST_ESC;
      t->uneed = 0;
      return;
    }
  }

  switch (t->state) {
  case ST_GROUND:
    if (t->uneed > 0) {
      if ((c & 0xC0) == 0x80) {
        t->ucp = (t->ucp << 6) | (c & 0x3F);
        if (--t->uneed == 0) put_char(t, t->ucp);
        return;
      }
      /* A sequence cut short: what it had is lost, the byte is read fresh. */
      t->uneed = 0;
      put_char(t, 0xFFFD);
    }
    if (c < 0x20) control(t, c);
    else if (c < 0x7f) put_char(t, c);
    else if (c == 0x7f) { /* DEL: nothing */ }
    else if (c >= 0xC2 && c <= 0xDF) { t->ucp = c & 0x1F; t->uneed = 1; }
    else if (c >= 0xE0 && c <= 0xEF) { t->ucp = c & 0x0F; t->uneed = 2; }
    else if (c >= 0xF0 && c <= 0xF4) { t->ucp = c & 0x07; t->uneed = 3; }
    else put_char(t, 0xFFFD);
    return;

  case ST_ESC:
    if (c == '[') csi_begin(t);
    else if (c == ']') { t->state = ST_OSC; t->osclen = 0; }
    else if (c == 'P' || c == 'X' || c == '^' || c == '_') t->state = ST_IGNORE;
    else if (c >= 0x20 && c <= 0x2f) { t->inter = (char)c; t->state = ST_ESC_INTER; }
    else if (c < 0x20) control(t, c);
    else { esc_dispatch(t, (char)c); t->state = ST_GROUND; }
    return;

  case ST_ESC_INTER:
    if (c >= 0x30 && c <= 0x7e) {
      esc_inter_dispatch(t, t->inter, (char)c);
      t->state = ST_GROUND;
    } else if (c < 0x20) {
      control(t, c);
    }
    return;

  case ST_CSI:
    if (c >= '0' && c <= '9') {
      if (t->nparams == 0) t->nparams = 1, t->params[0] = -1;
      if (t->params[t->nparams - 1] < 0) t->params[t->nparams - 1] = 0;
      if (t->params[t->nparams - 1] < 100000)
        t->params[t->nparams - 1] = t->params[t->nparams - 1] * 10 + (c - '0');
    } else if (c == ';' || c == ':') {
      if (t->nparams == 0) t->nparams = 1, t->params[0] = -1;
      if (t->nparams < VT_MAX_PARAMS) {
        t->sub[t->nparams] = (c == ':');
        t->params[t->nparams++] = -1;
      }
    } else if (c >= 0x3c && c <= 0x3f) {
      t->priv = (char)c;
    } else if (c >= 0x20 && c <= 0x2f) {
      t->inter = (char)c;
    } else if (c >= 0x40 && c <= 0x7e) {
      csi_dispatch(t, (char)c);
      t->state = ST_GROUND;
    } else if (c < 0x20) {
      control(t, c);   /* executed in the middle of a sequence, as VTs do */
    }
    return;

  case ST_OSC:
    if (c == 0x07) {
      osc_dispatch(t);
      t->state = ST_GROUND;
    } else if (c == 0x1b) {
      t->state = ST_OSC_ESC;
    } else if (t->osclen + 1 < VT_OSC_MAX) {
      t->osc[t->osclen++] = (char)c;
    }
    return;

  case ST_OSC_ESC:
    if (c == '\\') osc_dispatch(t);
    t->state = ST_GROUND;
    if (c != '\\') feed_byte(t, c);
    return;

  case ST_IGNORE:
    if (c == 0x1b) t->state = ST_IGNORE_ESC;
    else if (c == 0x07) t->state = ST_GROUND;
    return;

  case ST_IGNORE_ESC:
    t->state = (c == '\\') ? ST_GROUND : ST_IGNORE;
    return;
  }
}

void vt_feed(struct vt *t, const char *s, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) feed_byte(t, (unsigned char)s[i]);
}

/* ---- making one ---------------------------------------------------------- */

static void tabs_default(struct vt *t, int from) {
  int x;
  for (x = from; x < t->cols; x++) t->tabs[x] = (x % 8 == 0 && x > 0);
}

static void reset(struct vt *t) {
  int y;
  t->alt_on = 1;
  t->screen = t->alt;
  t->fg = t->bg = -1;
  t->attrs = 0;
  for (y = 0; y < t->rows; y++) fill(t, y, 0, t->cols);
  t->alt_on = 0;
  t->screen = t->main;
  for (y = 0; y < t->rows; y++) fill(t, y, 0, t->cols);
  t->cy = t->cx = 0;
  t->wrapnext = 0;
  t->top = 0;
  t->bottom = t->rows - 1;
  t->autowrap = 1;
  t->insert = 0;
  t->origin = 0;
  t->cursor_visible = 1;
  t->app_cursor = 0;
  t->app_keypad = 0;
  t->bracketed_paste = 0;
  t->mouse = t->mouse_sgr = 0;
  t->g0_graphics = t->g1_graphics = t->shift_out = 0;
  memset(&t->saved_main, 0, sizeof(t->saved_main));
  memset(&t->saved_alt, 0, sizeof(t->saved_alt));
  t->saved_main.fg = t->saved_main.bg = t->saved_alt.fg = t->saved_alt.bg = -1;
  tabs_default(t, 0);
  t->state = ST_GROUND;
  t->uneed = 0;
}

struct vt *vt_new(int rows, int cols, int scrollback) {
  struct vt *t = xmalloc(sizeof(*t));
  if (rows < 1) rows = 1;
  if (cols < 1) cols = 1;
  memset(t, 0, sizeof(*t));
  t->rows = rows;
  t->cols = cols;
  t->main = xmalloc(sizeof(*t->main) * (size_t)rows * (size_t)cols);
  t->alt = xmalloc(sizeof(*t->alt) * (size_t)rows * (size_t)cols);
  t->tabs = xmalloc((size_t)cols);
  t->sbcap = scrollback > 0 ? scrollback : 0;
  if (t->sbcap > 0) {
    t->sb = xmalloc(sizeof(*t->sb) * (size_t)t->sbcap);
    memset(t->sb, 0, sizeof(*t->sb) * (size_t)t->sbcap);
  }
  reset(t);
  return t;
}

void vt_free(struct vt *t) {
  if (t == NULL) return;
  sb_clear(t);
  free(t->sb);
  free(t->main);
  free(t->alt);
  free(t->tabs);
  free(t->reply.b);
  free(t);
}

/* A new size. Rows that no longer fit go to the scrollback from the top when
 * the cursor would otherwise fall off the bottom, which keeps the prompt on
 * screen; a taller screen takes lines back out of the scrollback, as xterm
 * does, so growing a window shows more history rather than blank rows. Columns
 * are cut or padded, never re-wrapped. */
void vt_resize(struct vt *t, int rows, int cols) {
  struct vt_cell *nm, *na;
  int y, x, shift = 0, pulled = 0, oldrows = t->rows, oldcols = t->cols;
  int alt = t->alt_on;

  if (rows < 1) rows = 1;
  if (cols < 1) cols = 1;
  if (rows == t->rows && cols == t->cols) return;

  /* Shrinking: how many rows of the main screen go up into the scrollback. */
  if (rows < oldrows) {
    int cy = alt ? t->saved_main.cy : t->cy;
    if (cy >= rows) shift = cy - rows + 1;
  }
  if (!alt && shift > 0) {
    for (y = 0; y < shift; y++) sb_push(t, &t->main[(size_t)y * (size_t)oldcols]);
  }

  nm = xmalloc(sizeof(*nm) * (size_t)rows * (size_t)cols);
  na = xmalloc(sizeof(*na) * (size_t)rows * (size_t)cols);
  t->rows = rows;
  t->cols = cols;
  {
    int bg = t->bg;
    t->bg = -1;
    for (y = 0; y < rows; y++) {
      for (x = 0; x < cols; x++) {
        nm[(size_t)y * cols + x] = blank_with(t);
        na[(size_t)y * cols + x] = blank_with(t);
      }
    }
    t->bg = bg;
  }
  /* Growing: lines back from the scrollback, above what was there. */
  if (rows > oldrows && !alt) {
    t->screen = nm;
    t->cols = cols;
    while (pulled < rows - oldrows && sb_pop(t, &nm[(size_t)(rows - oldrows - pulled - 1) * cols]))
      pulled++;
    if (pulled < rows - oldrows) {
      /* Fewer than wanted: what came back moves to the top. */
      memmove(nm, &nm[(size_t)(rows - oldrows - pulled) * cols],
              sizeof(*nm) * (size_t)pulled * (size_t)cols);
    }
  }
  for (y = 0; y < oldrows; y++) {
    int ny = y - shift + pulled, n = oldcols < cols ? oldcols : cols;
    if (ny < 0 || ny >= rows) continue;
    memcpy(&nm[(size_t)ny * cols], &t->main[(size_t)y * oldcols], sizeof(*nm) * (size_t)n);
    if (y < rows) memcpy(&na[(size_t)y * cols], &t->alt[(size_t)y * oldcols], sizeof(*na) * (size_t)n);
    /* A wide character cut in half at the new edge is a blank. */
    if (n < oldcols && n > 0 && nm[(size_t)ny * cols + n - 1].w == 2)
      nm[(size_t)ny * cols + n - 1].cp = ' ', nm[(size_t)ny * cols + n - 1].w = 1;
  }
  free(t->main);
  free(t->alt);
  t->main = nm;
  t->alt = na;
  t->screen = alt ? na : nm;

  t->tabs = xrealloc(t->tabs, (size_t)cols);
  if (cols > oldcols) tabs_default(t, oldcols);

  if (alt) {
    t->saved_main.cy += pulled - shift;
  } else {
    t->cy += pulled - shift;
  }
  t->top = 0;
  t->bottom = rows - 1;
  t->wrapnext = 0;
  clamp_cursor(t);
  if (t->saved_main.cy >= rows) t->saved_main.cy = rows - 1;
  if (t->saved_main.cy < 0) t->saved_main.cy = 0;
  if (t->saved_main.cx >= cols) t->saved_main.cx = cols - 1;
}

/* ---- reading it ---------------------------------------------------------- */

int vt_rows(const struct vt *t) { return t->rows; }
int vt_cols(const struct vt *t) { return t->cols; }
int vt_scrollback(const struct vt *t) { return t->sbn; }

/* How many lines have scrolled off the top, ever: what a line's index moves by,
 * which the scrollback's length stops saying once the ring is full. */
long vt_pushed(const struct vt *t) { return t->pushed; }

/* Line `i` of what can be seen: 0 .. rows-1 is the screen, and -1, -2 and so
 * on are the scrollback, newest first. *n is how many cells the line has; a
 * scrollback line may have fewer than the screen is wide, and the rest of it is
 * blank. NULL past either end. */
const struct vt_cell *vt_line(const struct vt *t, int i, int *n) {
  if (i >= 0) {
    if (i >= t->rows) return NULL;
    *n = t->cols;
    return &t->screen[(size_t)i * (size_t)t->cols];
  }
  if (-i > t->sbn) return NULL;
  {
    const struct vt_line *l = &t->sb[(t->sbhead + t->sbn + i) % t->sbcap];
    *n = l->n;
    return l->c;
  }
}

void vt_cursor(const struct vt *t, int *y, int *x, int *visible) {
  *y = t->cy;
  *x = t->cx;
  *visible = t->cursor_visible;
}

int vt_mode(const struct vt *t, int mode) {
  switch (mode) {
  case VT_MODE_APP_CURSOR: return t->app_cursor;
  case VT_MODE_BRACKETED_PASTE: return t->bracketed_paste;
  case VT_MODE_ALT_SCREEN: return t->alt_on;
  case VT_MODE_MOUSE: return t->mouse;
  case VT_MODE_MOUSE_SGR: return t->mouse_sgr;
  }
  return 0;
}

/* A mouse event at cell (x, y) of the screen, as the program asked to be told
 * of it, into `out` (32 bytes is enough); 0 when it did not ask for this one.
 *
 * Button numbers are xterm's: 0-2 the buttons, 64 and 65 the wheel, plus 32 for
 * a drag and 4, 8, 16 for Shift, Alt and Ctrl. The SGR form says which button
 * was released; the old one says only 3, "a button", and cannot name a cell past
 * column or row 223, where a click is dropped rather than sent mangled. */
int vt_mouse(const struct vt *t, const struct mouse_event *ev, int x, int y, char *out) {
  int b, release = 0;
  if (t->mouse == 0 || x < 0 || y < 0 || x >= t->cols || y >= t->rows) return 0;
  switch (ev->type) {
  case MOUSE_WHEEL_UP: b = 64; break;
  case MOUSE_WHEEL_DOWN: b = 65; break;
  case MOUSE_RELEASE: b = ev->button; release = 1; break;
  case MOUSE_DRAG:
    if (t->mouse == 1000) return 0;
    b = ev->button + 32;
    break;
  default: b = ev->button; break;
  }
  if (ev->mods & MOUSE_MOD_SHIFT) b |= 4;
  if (ev->mods & MOUSE_MOD_ALT) b |= 8;
  if (ev->mods & MOUSE_MOD_CTRL) b |= 16;
  if (t->mouse_sgr)
    return snprintf(out, 32, "\x1b[<%d;%d;%d%c", b, x + 1, y + 1, release ? 'm' : 'M');
  if (x + 1 > 223 || y + 1 > 223) return 0;
  if (release) b = (b & ~3) | 3;
  out[0] = '\x1b';
  out[1] = '[';
  out[2] = 'M';
  out[3] = (char)(32 + b);
  out[4] = (char)(32 + x + 1);
  out[5] = (char)(32 + y + 1);
  return 6;
}

/* The title the program set, once: returns NULL when it has not changed since
 * the last call. */
const char *vt_take_title(struct vt *t) {
  if (!t->title_changed) return NULL;
  t->title_changed = 0;
  return t->title;
}

/* What the program asked the terminal to send back, and how much; the caller
 * writes it to the program and then calls vt_reply_done. */
const char *vt_reply(const struct vt *t, int *len) {
  *len = t->reply.len;
  return t->reply.b;
}

void vt_reply_done(struct vt *t) { t->reply.len = 0; }

int vt_take_bell(struct vt *t) {
  int b = t->bell;
  t->bell = 0;
  return b;
}

/* ---- the keyboard, going the other way ----------------------------------- */

/* The bytes a key sends to the program in the terminal, as an xterm would send
 * them: molly has already decoded the escape sequence the outer terminal sent,
 * so this puts one back together, in the form the program's mode asks for.
 * `mods` is the xterm modifier bits: 1 Shift, 2 Alt, 4 Ctrl. Returns the
 * number of bytes written to out, which has room for 16; 0 for a key with no
 * meaning to a terminal program. */
int vt_key(const struct vt *t, int key, int mods, char *out) {
  static const struct { int key; const char *seq; char final; } tilde[] = {
    {PAGE_UP, "5", '~'}, {PAGE_DOWN, "6", '~'}, {DEL_KEY, "3", '~'},
    {F5_KEY, "15", '~'}, {F6_KEY, "17", '~'}, {F7_KEY, "18", '~'},
    {F8_KEY, "19", '~'}, {F9_KEY, "20", '~'}, {F10_KEY, "21", '~'},
    {F11_KEY, "23", '~'}, {F12_KEY, "24", '~'},
  };
  char final = 0;
  int i, m = mods ? mods + 1 : 0;

  if (IS_ALT_KEY(key)) {
    out[0] = '\x1b';
    out[1] = (char)ALT_KEY_CHAR(key);
    return 2;
  }
  if (key >= 0 && key < 256) {
    int n = 0;
    if (mods & 2) out[n++] = '\x1b';
    out[n++] = (char)key;
    return n;
  }
  switch (key) {
  case ARROW_UP: final = 'A'; break;
  case ARROW_DOWN: final = 'B'; break;
  case ARROW_RIGHT: final = 'C'; break;
  case ARROW_LEFT: final = 'D'; break;
  case HOME_KEY: final = 'H'; break;
  case END_KEY: final = 'F'; break;
  case F1_KEY: final = 'P'; break;
  case F2_KEY: final = 'Q'; break;
  case F3_KEY: final = 'R'; break;
  case F4_KEY: final = 'S'; break;
  case BACKTAB_KEY: return snprintf(out, 16, "\x1b[Z");
  default: break;
  }
  if (final != 0) {
    int is_f = (key >= F1_KEY && key <= F4_KEY);
    if (m) return snprintf(out, 16, "\x1b[1;%d%c", m, final);
    /* Application cursor mode is what vim and less ask for; the function keys
     * are SS3 either way. */
    if (is_f || t->app_cursor) return snprintf(out, 16, "\x1bO%c", final);
    return snprintf(out, 16, "\x1b[%c", final);
  }
  for (i = 0; i < (int)(sizeof(tilde) / sizeof(tilde[0])); i++) {
    if (tilde[i].key != key) continue;
    if (m) return snprintf(out, 16, "\x1b[%s;%d%c", tilde[i].seq, m, tilde[i].final);
    return snprintf(out, 16, "\x1b[%s%c", tilde[i].seq, tilde[i].final);
  }
  return 0;
}
