/* theme.c -- the semantic palette.
 *
 * Drawing code names a *role* -- what a cell is for -- and never a color. The
 * gutter asks for ROLE_GUTTER; whether that is dim gray, blue, or reverse video is
 * the theme's business. Three things fall out of that:
 *
 *   - A theme is a table, so adding one touches no drawing code.
 *   - Terminals that can only manage sixteen colors get a different table rather
 *     than a different renderer.
 *   - Syntax highlighting in Phase 2 adds roles (keyword, string, comment) instead
 *     of threading colors through the row renderer.
 *
 * Colors are palette indices. 0-15 are the ANSI sixteen, which every terminal has
 * and which emit as the short SGR forms; 16-255 are the xterm cube, which emit as
 * "38;5;N". A role may also ask for reverse video, which is how molly drew its
 * status bar and selection before it had colors at all, and still the most
 * reliable way to say "stand out" on a terminal whose palette you do not control.
 *
 * The roles defined so far are only the ones something draws. Menus, dialogs and
 * highlighting add theirs when they arrive -- a role nothing uses is a theme entry
 * nothing tests. For the same reason both tables currently resolve to exactly what
 * olly drew: this file is the machinery, and the first colors that actually differ
 * between the two depths come with the first widgets that need them. */

#include "molly.h"

/* -1 means "whatever the terminal's default is", which is not the same as black:
 * a user with a light background wants their background, not ours. */
#define DEFAULT_COLOR (-1)

struct theme_entry {
  short fg;
  short bg;
  unsigned char reverse;
  unsigned char bold;
  unsigned char italic;
};

/* The 256-color theme. */
static const struct theme_entry theme_256[ROLE_COUNT] = {
  /* ROLE_TEXT      */ {DEFAULT_COLOR, DEFAULT_COLOR, 0, 0, 0},
  /* ROLE_GUTTER    */ {8,             DEFAULT_COLOR, 0, 0, 0},
  /* ROLE_SELECTION */ {DEFAULT_COLOR, DEFAULT_COLOR, 1, 0, 0},
  /* An extra cursor is amber on purpose: reverse video is the selection's, and
   * the two sit side by side after Ctrl-D. */
  /* ROLE_CURSOR    */ {16,            214,           0, 0, 0},
  /* ROLE_STATUS    */ {DEFAULT_COLOR, DEFAULT_COLOR, 1, 0, 0},
  /* ROLE_MESSAGE   */ {DEFAULT_COLOR, DEFAULT_COLOR, 0, 0, 0},
  /* ROLE_DIALOG        */ {252, 238, 0, 0, 0},
  /* ROLE_DIALOG_FRAME  */ {245, 238, 0, 0, 0},
  /* ROLE_DIALOG_TITLE  */ {231, 238, 0, 1, 0},
  /* ROLE_DIALOG_SHADOW */ {240, 234, 0, 0, 0},
  /* ROLE_LIST_SELECTED */ {234, 110, 0, 0, 0},
  /* ROLE_INPUT         */ {252, 235, 0, 0, 0},
  /* ROLE_BUTTON        */ {232, 145, 0, 0, 0},
  /* ROLE_FOCUS         */ {234, 110, 0, 1, 0},
  /* ROLE_MENUBAR          */ {252, 235, 0, 0, 0},
  /* ROLE_MENUBAR_SELECTED */ {234, 110, 0, 0, 0},
  /* ROLE_MENU             */ {252, 238, 0, 0, 0},
  /* ROLE_MENU_FRAME       */ {245, 238, 0, 0, 0},
  /* ROLE_MENU_SELECTED    */ {234, 110, 0, 0, 0},
  /* ROLE_MENU_DISABLED    */ {243, 238, 0, 0, 0},
  /* ROLE_TABBAR           */ {245, 235, 0, 0, 0},
  /* ROLE_TAB_SELECTED     */ {231, 239, 0, 1, 0},

  /* The focused window's bar is reverse video, which is the terminal's own idea
   * of maximum contrast and what molly's one status bar always was. An
   * unfocused one is a stated pair instead: light grey on dark grey reads as a
   * bar that is still a bar, but quieter, and it does so whether the terminal's
   * background is light or dark -- which reverse video cannot be relied on for
   * once it is competing against itself one row away. */
  /* ROLE_STATUS_UNFOCUSED */ {250, 240, 0, 0, 0},
  /* ROLE_SPLIT            */ {240, DEFAULT_COLOR, 0, 0, 0},

  /* Syntax, on the terminal's own background: code is the one thing on screen
   * that is not a panel, so a background here would fight whatever the user has
   * chosen. Comments are the dimmest thing in the file on purpose -- they are
   * what you skip when you are looking for code -- and strings and numbers sit
   * close together because both are literals. Italic says "comment" a second
   * way, which matters on a terminal whose 244 is closer to the text than this
   * table thinks it is. */
  /* ROLE_SYN_KEYWORD      */ {110, DEFAULT_COLOR, 0, 0, 0},
  /* ROLE_SYN_TYPE         */ {79,  DEFAULT_COLOR, 0, 0, 0},
  /* ROLE_SYN_STRING       */ {173, DEFAULT_COLOR, 0, 0, 0},
  /* ROLE_SYN_NUMBER       */ {179, DEFAULT_COLOR, 0, 0, 0},
  /* ROLE_SYN_COMMENT      */ {244, DEFAULT_COLOR, 0, 0, 1},
  /* ROLE_SYN_PREPROC      */ {140, DEFAULT_COLOR, 0, 0, 0},
  /* ROLE_SYN_HEADING      */ {110, DEFAULT_COLOR, 0, 1, 0},
  /* ROLE_SYN_EMPHASIS     */ {186, DEFAULT_COLOR, 0, 0, 0},
};

/* The sixteen-color theme, for terminals that cannot do better. */
static const struct theme_entry theme_16[ROLE_COUNT] = {
  /* ROLE_TEXT      */ {DEFAULT_COLOR, DEFAULT_COLOR, 0, 0, 0},
  /* ROLE_GUTTER    */ {8,             DEFAULT_COLOR, 0, 0, 0},
  /* ROLE_SELECTION */ {DEFAULT_COLOR, DEFAULT_COLOR, 1, 0, 0},
  /* Sixteen colors have no amber, so an extra cursor is yellow-backed: still a
   * block, still not the selection's reverse video. */
  /* ROLE_CURSOR    */ {0,             3,             0, 0, 0},
  /* ROLE_STATUS    */ {DEFAULT_COLOR, DEFAULT_COLOR, 1, 0, 0},
  /* ROLE_MESSAGE   */ {DEFAULT_COLOR, DEFAULT_COLOR, 0, 0, 0},
  /* ROLE_DIALOG        */ {15, 8, 0, 0, 0},
  /* ROLE_DIALOG_FRAME  */ {7,  8, 0, 0, 0},
  /* ROLE_DIALOG_TITLE  */ {15, 8, 0, 1, 0},
  /* ROLE_DIALOG_SHADOW */ {8,  0, 0, 0, 0},
  /* ROLE_LIST_SELECTED */ {15, 4, 0, 0, 0},
  /* ROLE_INPUT         */ {15, 0, 0, 0, 0},
  /* ROLE_BUTTON        */ {0,  7, 0, 0, 0},
  /* ROLE_FOCUS         */ {15, 4, 0, 1, 0},
  /* ROLE_MENUBAR          */ {7,  0, 0, 0, 0},
  /* ROLE_MENUBAR_SELECTED */ {15, 4, 0, 0, 0},
  /* ROLE_MENU             */ {15, 8, 0, 0, 0},
  /* ROLE_MENU_FRAME       */ {7,  8, 0, 0, 0},
  /* ROLE_MENU_SELECTED    */ {15, 4, 0, 0, 0},
  /* ROLE_MENU_DISABLED    */ {0,  8, 0, 0, 0},
  /* ROLE_TABBAR           */ {7,  0, 0, 0, 0},
  /* ROLE_TAB_SELECTED     */ {15, 8, 0, 1, 0},

  /* The same pair, out of the sixteen: light grey on the bright black that is
   * the only dim background there is. The separator takes the colour the gutter
   * bar already uses, because it is the same kind of line. */
  /* ROLE_STATUS_UNFOCUSED */ {7, 8, 0, 0, 0},
  /* ROLE_SPLIT            */ {8, DEFAULT_COLOR, 0, 0, 0},

  /* Eight roles onto eight usable foregrounds, which is the whole of what
   * sixteen colours leaves once the background is the terminal's. Comments take
   * the bright black that is the only dim colour there is; a heading leans on
   * bold instead of on a colour of its own, because there was not one left. */
  /* ROLE_SYN_KEYWORD      */ {12, DEFAULT_COLOR, 0, 0, 0},
  /* ROLE_SYN_TYPE         */ {14, DEFAULT_COLOR, 0, 0, 0},
  /* ROLE_SYN_STRING       */ {3,  DEFAULT_COLOR, 0, 0, 0},
  /* ROLE_SYN_NUMBER       */ {11, DEFAULT_COLOR, 0, 0, 0},
  /* ROLE_SYN_COMMENT      */ {8,  DEFAULT_COLOR, 0, 0, 1},
  /* ROLE_SYN_PREPROC      */ {13, DEFAULT_COLOR, 0, 0, 0},
  /* ROLE_SYN_HEADING      */ {12, DEFAULT_COLOR, 0, 1, 0},
  /* ROLE_SYN_EMPHASIS     */ {14, DEFAULT_COLOR, 0, 0, 0},
};

static const struct theme_entry *active = theme_16;
static int active_depth = 16;
static int italic_wanted = 1;   /* the [view] italic setting */
static int italic_able = 1;     /* what TERM says the terminal can do */

/* How many colors the terminal claims.
 *
 * COLORTERM is set by terminals that support direct color and is the strongest
 * signal; otherwise the only portable hint is the terminal name. Deliberately not
 * terminfo: that is a dependency, and this question has two answers. */
int theme_detect_depth(const char *term, const char *colorterm) {
  if (colorterm != NULL && *colorterm != '\0') {
    if (strstr(colorterm, "truecolor") != NULL ||
        strstr(colorterm, "24bit") != NULL)
      return 256;
  }
  if (term == NULL || *term == '\0') return 16;
  if (strstr(term, "256") != NULL) return 256;
  if (strstr(term, "direct") != NULL) return 256;
  /* A few that support 256 without saying so in their name. */
  if (strcmp(term, "xterm-kitty") == 0) return 256;
  if (strcmp(term, "alacritty") == 0) return 256;
  if (strcmp(term, "foot") == 0) return 256;
  return 16;
}

/* Whether the terminal draws SGR 3 as italic, by name.
 *
 * The answer is yes far more often than not, and the exceptions are the ones
 * that do something worse than ignore it: the Linux console paints italic in a
 * substitute colour, and tmux running under a "screen" TERM draws it as reverse
 * video -- which in a comment looks like a selection. GNU screen and the old
 * hardware terminals have no italic to give. So this is a list of noes rather
 * than of yeses, and tmux's own "tmux-256color" is not on it. */
int theme_detect_italic(const char *term) {
  if (term == NULL || *term == '\0') return 0;
  if (strcmp(term, "linux") == 0 || strncmp(term, "linux-", 6) == 0) return 0;
  if (strcmp(term, "screen") == 0 || strncmp(term, "screen-", 7) == 0 ||
      strncmp(term, "screen.", 7) == 0)
    return 0;
  if (strncmp(term, "vt", 2) == 0) return 0;
  if (strcmp(term, "dumb") == 0 || strcmp(term, "cons25") == 0) return 0;
  return 1;
}

void theme_init(void) {
  const char *term = getenv("TERM");
  active_depth = theme_detect_depth(term, getenv("COLORTERM"));
  active = (active_depth >= 256) ? theme_256 : theme_16;
  italic_able = theme_detect_italic(term);
}

/* The setting is what the user asked for, which is what gets saved; whether
 * italic is drawn is that and the terminal both. */
int theme_italic(void) { return italic_wanted; }

void theme_set_italic(int on) {
  italic_wanted = on ? 1 : 0;
  screen_force_repaint();
}

int theme_depth(void) {
  return active_depth;
}

void theme_style(unsigned char role, struct style *out) {
  const struct theme_entry *e;
  if (role >= ROLE_COUNT) role = ROLE_TEXT;
  e = &active[role];
  out->fg = e->fg;
  out->bg = e->bg;
  out->reverse = e->reverse;
  out->bold = e->bold;
  out->underline = 0;  /* never a property of a role; see ATTR_UNDERLINE */
  out->italic = e->italic && italic_wanted && italic_able;
}

int style_eq(const struct style *a, const struct style *b) {
  return a->fg == b->fg && a->bg == b->bg && a->reverse == b->reverse &&
         a->bold == b->bold && a->underline == b->underline &&
         a->italic == b->italic;
}

void style_default(struct style *out) {
  out->fg = DEFAULT_COLOR;
  out->bg = DEFAULT_COLOR;
  out->reverse = 0;
  out->bold = 0;
  out->underline = 0;
  out->italic = 0;
}

int style_is_default(const struct style *s) {
  return s->fg == DEFAULT_COLOR && s->bg == DEFAULT_COLOR && !s->reverse &&
         !s->bold && !s->underline && !s->italic;
}

/* Append the SGR for one color. Indices 0-15 use the short forms -- which is both
 * shorter and what every terminal has understood since before the 256-color cube
 * existed -- so a gray gutter is still "ESC[90m" whichever theme is loaded. */
void style_emit_color(struct abuf *ab, int color, int is_bg) {
  char buf[24];
  int n;
  if (color < 0) return;
  if (color < 8)
    n = snprintf(buf, sizeof(buf), "\x1b[%dm", (is_bg ? 40 : 30) + color);
  else if (color < 16)
    n = snprintf(buf, sizeof(buf), "\x1b[%dm", (is_bg ? 100 : 90) + color - 8);
  else
    n = snprintf(buf, sizeof(buf), "\x1b[%d;5;%dm", is_bg ? 48 : 38, color);
  ab_append(ab, buf, n);
}
