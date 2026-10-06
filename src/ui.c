/* ui.c -- The parts the user interacts with that are not the text itself: the prompt
   line, the help screen and the open-file dialog. These three are hand-rolled
   full-screen UIs today; Phase 1 replaces them with the widget toolkit. */

#include "molly.h"

char *editor_prompt(char *prompt, void (*callback)(char *, int)) {
  size_t bufsize = 128;
  char *buf = xmalloc(bufsize);
  size_t buflen = 0;
  buf[0] = '\0';

  while (1) {
    editor_set_status_message(prompt, buf);
    editor_refresh_screen();

    int c = editor_read_key();

    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      if (buflen != 0) {
        buflen = (size_t)utf8_prev(buf, (int)buflen);
        buf[buflen] = '\0';
      }
    } else if (c == '\x1b') {
      editor_set_status_message("");
      if (callback) callback(buf, c);
      free(buf);
      return NULL;
    } else if (c == '\r') {
      if (callback) callback(buf, c);
      return buf;
    } else if (c >= 32 && c != 127 && c < 256) {
      /* Any byte that is not a control code or one of the >=1000 key codes,
       * so a pasted or typed UTF-8 character enters the prompt intact. */
      if (buflen == bufsize - 1) {
        bufsize *= 2;
        buf = xrealloc(buf, bufsize);
      }
      buf[buflen++] = c;
      buf[buflen] = '\0';
    }

    if (callback) callback(buf, c);
  }
}


/* ---- help screen: data + layout --------------------------------------- *
 * Every keybinding lives here, grouped by function. The renderer drops to a
 * single column when the terminal is narrow and scrolls when the content is
 * taller than the screen, so nothing is ever clipped. */
struct help_item { const char *key; const char *desc; };
struct help_group { const char *title; const struct help_item *items; };

static const struct help_item help_items_files[] = {
  {"Ctrl-S",   "Save file"},
  {"Ctrl-O",   "Open a file"},
  {"Ctrl-Q",   "Quit"},
  {"Ctrl-?",   "This help screen"},
  {NULL, NULL}
};
static const struct help_item help_items_movement[] = {
  {"Arrows",       "Move cursor"},
  {"PgUp/PgDn",    "Page up / down"},
  {"Home/End",     "Line start / end"},
  {"Ctrl-G",       "Go to line"},
  {NULL, NULL}
};
static const struct help_item help_items_selection[] = {
  {"Shift+Arrows",   "Extend selection"},
  {"Shift+Home/End", "To line edge"},
  {"Shift+PgUp/Dn",  "Extend by page"},
  {NULL, NULL}
};
static const struct help_item help_items_editing[] = {
  {"Enter",      "Insert new line"},
  {"Tab",        "Insert tab"},
  {"Bksp/Ctrl-H","Delete before"},
  {"Del",        "Delete at cursor"},
  {NULL, NULL}
};
static const struct help_item help_items_find[] = {
  {"Ctrl-F", "Find forward"},
  {"Ctrl-N", "Find next"},
  {"Ctrl-P", "Find previous"},
  {"Ctrl-T", "Case sensitivity"},
  {"Ctrl-R", "Find & replace"},
  {NULL, NULL}
};
static const struct help_item help_items_clip[] = {
  {"Ctrl-C", "Copy selection"},
  {"Ctrl-X", "Cut selection"},
  {"Ctrl-V", "Paste at cursor"},
  {NULL, NULL}
};
static const struct help_item help_items_history[] = {
  {"Ctrl-Z", "Undo"},
  {"Ctrl-Y", "Redo"},
  {NULL, NULL}
};
static const struct help_item help_items_view[] = {
  {"Ctrl-L", "Redraw screen"},
  {"Ctrl-U", "Line numbers"},
  {NULL, NULL}
};

/* Order matters: the two-column split takes the first half vs the second. */
static const struct help_group help_groups[] = {
  {"FILES",          help_items_files},
  {"MOVEMENT",       help_items_movement},
  {"SELECTION",      help_items_selection},
  {"EDITING",        help_items_editing},
  {"FIND & REPLACE", help_items_find},
  {"CLIPBOARD",      help_items_clip},
  {"HISTORY",        help_items_history},
  {"VIEW",           help_items_view},
};

#define HELP_WIDE 80      /* two columns at or above this width */
#define HELP_GAP 4        /* blank columns between the two columns */
#define HELP_KEYW 14      /* width reserved for the key name */
#define HELP_MAXROWS 48
#define HELP_MAXW 512

static char help_lcol[HELP_MAXROWS][HELP_MAXW];
static char help_rcol[HELP_MAXROWS][HELP_MAXW];
static char help_line[HELP_MAXW];

static void help_build_col(char col[HELP_MAXROWS][HELP_MAXW], int *rn,
                           int g0, int g1, int keyw, int descw) {
  int r = 0, g;
  for (g = g0; g <= g1 && r < HELP_MAXROWS - 1; g++) {
    if (g > g0) { col[r][0] = '\0'; r++; }
    snprintf(col[r], HELP_MAXW, "%s", help_groups[g].title);
    r++;
    const struct help_item *it;
    for (it = help_groups[g].items; it->key && r < HELP_MAXROWS - 1; it++) {
      snprintf(col[r], HELP_MAXW, "  %-*.*s %.*s",
               keyw, keyw, it->key, descw, it->desc);
      r++;
    }
  }
  *rn = r;
}

/* One composed screen row: left field padded to colw, a gap, then the right
 * field truncated to whatever width remains. All help text is ASCII, so this
 * byte-wise truncation can never split a multibyte character. */
static void help_row(char *dst, int dstsz, const char *left, const char *right,
                     int colw, int gap, int total) {
  int rightw = total - colw - gap;
  if (rightw < 0) rightw = 0;
  snprintf(dst, dstsz, "%-*.*s%*s%.*s",
           colw, colw, left, gap, "", rightw, right);
}

static void help_centered(struct abuf *ab, const char *text, int row) {
  char pos[32];
  int pad = (V.screencols - (int)strlen(text)) / 2;
  if (pad < 0) pad = 0;
  snprintf(pos, sizeof(pos), "\x1b[%d;%dH", row, pad + 1);
  ab_append(ab, pos, strlen(pos));
  int len = strlen(text);
  if (len > V.screencols) len = V.screencols;
  ab_append(ab, text, len);
  ab_append(ab, "\x1b[K", 3);
}

/* Re-read the terminal size into V.screenrows/V.screencols while help is shown.
 * The editor's own refresh does this between keypresses, but help runs its own
 * loop, so without this it would keep drawing at whatever size was current when
 * it opened. Mirrors editor_refresh_screen's fallback to the last known size. */
static void help_refresh_size(void) {
  /* The return value -- whether the size changed -- does not matter here: help
   * repaints itself every pass, and sets a full repaint for the editor
   * underneath when it exits. */
  (void)screen_poll_size();
}

void editor_help(void) {
  int scroll = 0;
  for (;;) {
    /* Recompute the whole layout from the live terminal size on every pass, so
     * a resize (surfaced as WINCH_KEY) reflows one-column vs two-column, recent
     * the title, and recomputes the scroll extent to match the new window. */
    help_refresh_size();

    int wide = (V.screencols >= HELP_WIDE);
    int gap = wide ? HELP_GAP : 0;
    int colw = wide ? (V.screencols - gap) / 2 : V.screencols;
    if (colw < 1) colw = 1;
    int keyw = HELP_KEYW;
    if (keyw > colw - 3) keyw = (colw > 3) ? colw - 3 : 1;
    int descw = colw - 3 - keyw;
    if (descw < 1) descw = 1;

    int ngroups = (int)(sizeof(help_groups) / sizeof(help_groups[0]));
    int nl, nr;
    if (wide) {
      int mid = ngroups / 2;
      help_build_col(help_lcol, &nl, 0, mid - 1, keyw, descw);
      help_build_col(help_rcol, &nr, mid, ngroups - 1, keyw, descw);
    } else {
      help_build_col(help_lcol, &nl, 0, ngroups - 1, keyw, descw);
      nr = 0;
    }
    int content = (nl > nr) ? nl : nr;

    int content_top = 3;                 /* row 1 title, row 2 blank */
    int view_h = V.screenrows - (content_top - 1) - 2; /* 2 footer rows */
    if (view_h < 1) view_h = 1;
    int maxscroll = content - view_h;
    if (maxscroll < 0) maxscroll = 0;
    if (scroll > maxscroll) scroll = maxscroll;
    if (scroll < 0) scroll = 0;

    struct abuf ab = ABUF_INIT;
    ab_append(&ab, "\x1b[2J", 4);
    ab_append(&ab, "\x1b[?25l", 6);
    char pos[32];

    help_centered(&ab, "Molly - Keyboard Reference", 1);

    int k;
    for (k = 0; k < view_h; k++) {
      int idx = scroll + k;
      if (idx >= nl && idx >= nr) break;
      const char *L = (idx < nl) ? help_lcol[idx] : "";
      const char *R = (idx < nr) ? help_rcol[idx] : "";
      help_row(help_line, sizeof(help_line), L, R, colw, gap, V.screencols);
      snprintf(pos, sizeof(pos), "\x1b[%d;1H", content_top + k);
      ab_append(&ab, pos, strlen(pos));
      ab_append(&ab, help_line, strlen(help_line));
      ab_append(&ab, "\x1b[K", 3);
    }

    help_centered(&ab, "In prompts: Enter accepts   Esc cancels",
                  V.screenrows - 1);
    help_centered(&ab,
        maxscroll > 0 ? "PgUp/PgDn scroll   any other key returns"
                      : "Press any key to return to the editor",
        V.screenrows);

    write_all(STDOUT_FILENO, ab.b, (size_t)ab.len);
    free(ab.b);

    int c = editor_read_key();
    /* A resize is not a dismiss: fall through to the top of the loop, which
     * re-reads the size and re-lays the screen out at the new dimensions. */
    if (c == WINCH_KEY) continue;
    if (maxscroll == 0) break;
    int ns = scroll;
    if (c == ARROW_DOWN) ns = scroll + 1;
    else if (c == ARROW_UP) ns = scroll - 1;
    else if (c == PAGE_DOWN) ns = scroll + view_h;
    else if (c == PAGE_UP) ns = scroll - view_h;
    else if (c == HOME_KEY) ns = 0;
    else if (c == END_KEY) ns = maxscroll;
    else break;
    if (ns < 0) ns = 0;
    if (ns > maxscroll) ns = maxscroll;
    scroll = ns;
  }
  screen_force_repaint();
}

/* ---- open-file dialog -------------------------------------------------- *
 * A modal read-only directory browser: Ctrl-O opens it in the working
 * directory, the arrows move through the list, Enter descends into a
 * directory or picks a file, Esc cancels. Directories sort before files,
 * each half alphabetically; dotfiles are hidden, with ".." kept as the way
 * back up. Only readdir/stat are used: the dialog never writes anything. */

struct browser_entry {
  char *name;
  int is_dir;
};

static int browser_cmp(const void *a, const void *b) {
  const struct browser_entry *ea = a, *eb = b;
  if (ea->is_dir != eb->is_dir) return eb->is_dir - ea->is_dir;
  return strcmp(ea->name, eb->name);
}

static void browser_free(struct browser_entry *ents, int n) {
  int i;
  for (i = 0; i < n; i++) free(ents[i].name);
  free(ents);
}

/* Read one directory into a freshly allocated, sorted entry array. Returns
 * the entry count, or -1 if the directory cannot be opened (it may have
 * been removed since we listed its parent). */
static int browser_load(const char *dir, struct browser_entry **out) {
  DIR *dp = opendir(dir);
  if (dp == NULL) return -1;
  struct browser_entry *ents = NULL;
  int n = 0, cap = 0;
  struct dirent *de;
  while ((de = readdir(dp)) != NULL) {
    if (de->d_name[0] == '.' && strcmp(de->d_name, "..") != 0) continue;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
    struct stat st;
    int is_dir = (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    if (n == cap) {
      cap = cap ? cap * 2 : 32;
      struct browser_entry *ne = realloc(ents, sizeof(*ents) * (size_t)cap);
      if (ne == NULL) die("browser_load");
      ents = ne;
    }
    ents[n].name = xstrdup(de->d_name);
    ents[n].is_dir = is_dir;
    n++;
  }
  closedir(dp);
  if (n > 1) qsort(ents, (size_t)n, sizeof(*ents), browser_cmp);
  *out = ents;
  return n;
}

/* Truncate a name to fit a table cell without splitting a UTF-8 sequence:
 * a file name is arbitrary bytes and the box has fixed columns. */
static void browser_fit(const char *s, int w, char *dst, size_t dsz) {
  int n = 0;
  while (s[n] != '\0' && n < w) n++;
  while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
  snprintf(dst, dsz, "%.*s", n, s);
}

/* The dialog itself: returns a malloc'd path to open, or NULL if the user
 * canceled. Like the help screen it owns the terminal until dismissed, so
 * it re-reads the window size itself and repaints the whole frame per pass. */
char *editor_open_dialog(void) {
  char dir[PATH_MAX];
  if (getcwd(dir, sizeof(dir)) == NULL) snprintf(dir, sizeof(dir), ".");

  struct browser_entry *ents;
  int n = browser_load(dir, &ents);
  if (n == -1) {
    snprintf(dir, sizeof(dir), ".");
    n = browser_load(dir, &ents);
    if (n == -1) return NULL;
  }

  int sel = 0, top = 0;
  const char *msg = "";
  for (;;) {
    help_refresh_size();
    int view_h = V.screenrows - 4;   /* title, dir line, two footer rows */
    if (view_h < 1) view_h = 1;
    if (sel >= n) sel = (n > 0) ? n - 1 : 0;
    if (sel < top) top = sel;
    if (sel >= top + view_h) top = sel - view_h + 1;

    struct abuf ab = ABUF_INIT;
    ab_append(&ab, "\x1b[2J", 4);
    ab_append(&ab, "\x1b[?25l", 6);
    char pos[32];
    help_centered(&ab, "Molly - Open File", 1);

    /* Current directory on row 2; if it does not fit, the tail still shows
     * which corner of the tree we are in, cut only at a UTF-8 boundary. */
    int dl = (int)strlen(dir);
    int wmax = V.screencols - 2;
    if (wmax < 1) wmax = 1;
    const char *dshow = dir;
    if (dl > wmax) {
      dshow = dir + dl - wmax;
      while (*dshow != '\0' && ((unsigned char)*dshow & 0xC0) == 0x80)
        dshow++;
    }
    snprintf(pos, sizeof(pos), "\x1b[2;2H");
    ab_append(&ab, pos, strlen(pos));
    ab_append(&ab, dshow, (int)strlen(dshow));
    ab_append(&ab, "\x1b[K", 3);

    int k;
    for (k = 0; k < view_h && top + k < n; k++) {
      struct browser_entry *e = &ents[top + k];
      char name[PATH_MAX];
      browser_fit(e->name, V.screencols - 2, name, sizeof(name));
      snprintf(pos, sizeof(pos), "\x1b[%d;2H%s", 4 + k,
               e == &ents[sel] ? "\x1b[7m" : "");
      ab_append(&ab, pos, strlen(pos));
      if (e->is_dir) {
        ab_append(&ab, name, (int)strlen(name));
        ab_append(&ab, "/", 1);
      } else {
        ab_append(&ab, name, (int)strlen(name));
      }
      ab_append(&ab, e == &ents[sel] ? "\x1b[27m\x1b[K" : "\x1b[K",
                e == &ents[sel] ? 8 : 3);
    }
    if (n == 0) {
      help_centered(&ab, "(empty directory)", 4);
    }

    help_centered(&ab,
        "Arrows/PgUp/PgDn move  Enter select  Esc cancel",
        V.screenrows - 1);
    help_centered(&ab, msg, V.screenrows);

    write_all(STDOUT_FILENO, ab.b, (size_t)ab.len);
    free(ab.b);
    msg = "";

    int c = editor_read_key();
    if (c == WINCH_KEY) continue;
    int ns = sel;
    if (c == ARROW_DOWN || c == CTRL_KEY('n')) ns = sel + 1;
    else if (c == ARROW_UP || c == CTRL_KEY('p')) ns = sel - 1;
    else if (c == PAGE_DOWN) ns = sel + view_h;
    else if (c == PAGE_UP) ns = sel - view_h;
    else if (c == HOME_KEY) ns = 0;
    else if (c == END_KEY) ns = n - 1;
    else if (c == '\r') {
      if (n == 0) continue;
      char path[PATH_MAX];
      snprintf(path, sizeof(path), "%s/%s", dir, ents[sel].name);
      if (!ents[sel].is_dir) {
        browser_free(ents, n);
        return xstrdup(path);
      }
      struct browser_entry *ne;
      int nn = browser_load(path, &ne);
      if (nn == -1) {
        msg = "Cannot enter that directory";
        continue;
      }
      browser_free(ents, n);
      ents = ne;
      n = nn;
      snprintf(dir, sizeof(dir), "%s", path);
      sel = 0;
      top = 0;
      continue;
    } else if (c == 27 || c == CTRL_KEY('c')) {
      break;
    } else {
      continue;
    }
    if (ns < 0) ns = 0;
    if (ns > n - 1) ns = (n > 0) ? n - 1 : 0;
    sel = ns;
  }
  browser_free(ents, n);
  screen_force_repaint();
  return NULL;
}

/* Replace the whole buffer with another file: the point of the dialog. The
 * editor only ever edits one file, so everything that belonged to the old
 * one -- rows, cursor, selection, undo history, line-ending flags -- goes
 * with it. The caller guarantees no unsaved changes. */
void editor_switch_file(char *path) {
  int i;
  for (i = 0; i < row_count(); i++) editor_free_row(row_at(i));
  B.numrows = 0;
  cursors_collapse();
  memset(&CUR, 0, sizeof CUR);
  V.drowoff = 0;
  V.coloff = 0;
  editor_free_undo_redo();
  B.eol_crlf = 0;
  B.final_newline = 1;
  editor_open(path);
  free(path);
  editor_set_status_message("Opened %s - %d lines",
      B.filename ? B.filename : "(unnamed)", row_count());
}
