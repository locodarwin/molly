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

    int c = ui_wait_key();

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
/* The help screen's text comes from the command table (cmd.c): a rebinding or a
 * new command updates it with nothing to keep in step here. What remains is the
 * layout -- two columns when there is room -- and an index of where each group
 * starts in the flat entry list, since the column split is by group. */

#define HELP_MAXGROUPS 16
static const char *help_group_title[HELP_MAXGROUPS];
static int help_group_start[HELP_MAXGROUPS];
static int help_group_n;

static void help_index_groups(void) {
  int i, n = help_entry_count();
  char key[64];
  const char *group, *desc;
  help_group_n = 0;
  for (i = 0; i < n; i++) {
    help_entry_at(i, &group, key, sizeof(key), &desc);
    if (group != NULL && help_group_n < HELP_MAXGROUPS) {
      help_group_title[help_group_n] = group;
      help_group_start[help_group_n] = i;
      help_group_n++;
    }
  }
}

/* Where group g's entries end (exclusive). */
static int help_group_end(int g) {
  return (g + 1 < help_group_n) ? help_group_start[g + 1] : help_entry_count();
}

/* Two columns at or above this pane width. Lower than the 80 it was, because the
 * pane now sits inside a frame with padding and so is six columns narrower than the
 * terminal: a standard 80-column terminal should still get two columns. */
#define HELP_WIDE 72
#define HELP_GAP 4        /* blank columns between the two columns */
#define HELP_KEYW 14      /* width reserved for the key name */
#define HELP_MAXROWS 48
#define HELP_MAXW 512

static char help_lcol[HELP_MAXROWS][HELP_MAXW];
static char help_rcol[HELP_MAXROWS][HELP_MAXW];

static void help_build_col(char col[HELP_MAXROWS][HELP_MAXW], int *rn,
                           int g0, int g1, int keyw, int descw) {
  int r = 0, g;
  for (g = g0; g <= g1 && r < HELP_MAXROWS - 1; g++) {
    if (g > g0) { col[r][0] = '\0'; r++; }
    snprintf(col[r], HELP_MAXW, "%s", help_group_title[g]);
    r++;
    int i, end = help_group_end(g);
    for (i = help_group_start[g]; i < end && r < HELP_MAXROWS - 1; i++) {
      const char *group, *desc;
      char key[64];
      help_entry_at(i, &group, key, sizeof(key), &desc);
      snprintf(col[r], HELP_MAXW, "  %-*.*s %.*s",
               keyw, keyw, key, descw, desc);
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

/* What a pane row says: the left column padded, the gap, then the right column.
 * Composed on demand rather than stored, so the pane holds no copy of the text. */
struct help_pane {
  int nl, nr;      /* lines built in each column */
  int colw, gap;   /* column width and the space between */
  int total;       /* the pane's width */
};

static void help_pane_row(void *user, int i, char *dst, size_t dsz) {
  struct help_pane *hp = user;
  const char *L = (i < hp->nl) ? help_lcol[i] : "";
  const char *R = (i < hp->nr) ? help_rcol[i] : "";
  help_row(dst, dsz, L, R, hp->colw, hp->gap, hp->total);
}

/* The keyboard reference: a scrollable pane in a framed dialog.
 *
 * Like the open dialog it is a layer now, so it draws cells rather than escape
 * sequences and needs no full repaint when it closes. It takes nearly the whole
 * screen, because a key reference wants the room, and reflows between one and two
 * columns on the pane's width. */
void editor_help(void) {
  struct help_pane hp;
  struct widget widgets[3];
  struct dialog d;
  struct layer l;
  int depth, li;

  help_index_groups();

  widget_text(&widgets[0], 0, 0, 1, 1, 0, help_pane_row, &hp);
  widget_label(&widgets[1], 0, 1, 1, "In prompts: Enter accepts   Esc cancels");
  widget_label(&widgets[2], 0, 2, 1, "");

  depth = layer_count();
  memset(&l, 0, sizeof(l));
  li = layer_push(&l);
  if (li < 0) return;

  for (;;) {
    struct layer *lp;
    int cols, rows, pane_w, pane_h, wide, keyw, descw, ngroups, c, max;

    /* Before measuring anything: see above screen_sync_size. */
    screen_sync_size();

    /* Nearly the whole screen -- a key reference is long -- but not quite: leaving
     * the status and message bars uncovered stops their ends peeking out past the
     * dialog's edges, and gives the shadow somewhere to fall. At 80 columns this
     * leaves the pane exactly wide enough for two columns. */
    cols = grid_cols() - 4;
    rows = grid_rows() - 4;
    if (cols < 16) cols = 16;
    if (rows < 6) rows = 6;
    pane_w = cols - 2 - 2 * 1;   /* frame, then a column of padding each side */
    if (pane_w < 1) pane_w = 1;
    pane_h = rows - 2 - 2;       /* frame, then the two footer rows */
    if (pane_h < 1) pane_h = 1;

    /* Rebuilt every pass, so a resize reflows the columns and the scroll extent. */
    wide = (pane_w >= HELP_WIDE);
    hp.gap = wide ? HELP_GAP : 0;
    hp.colw = wide ? (pane_w - hp.gap) / 2 : pane_w;
    if (hp.colw < 1) hp.colw = 1;
    hp.total = pane_w;
    keyw = HELP_KEYW;
    if (keyw > hp.colw - 3) keyw = (hp.colw > 3) ? hp.colw - 3 : 1;
    descw = hp.colw - 3 - keyw;
    if (descw < 1) descw = 1;

    ngroups = help_group_n;
    if (wide) {
      int mid = ngroups / 2;
      help_build_col(help_lcol, &hp.nl, 0, mid - 1, keyw, descw);
      help_build_col(help_rcol, &hp.nr, mid, ngroups - 1, keyw, descw);
    } else {
      help_build_col(help_lcol, &hp.nl, 0, ngroups - 1, keyw, descw);
      hp.nr = 0;
    }

    widgets[0].w = pane_w;
    widgets[0].h = pane_h;
    widgets[0].count = (hp.nl > hp.nr) ? hp.nl : hp.nr;
    widget_text_scroll(&widgets[0], 0);   /* clamp after a reflow */
    max = widget_text_maxtop(&widgets[0]);
    widgets[1].y = pane_h;
    widgets[1].w = pane_w;
    widgets[2].y = pane_h + 1;
    widgets[2].w = pane_w;
    widgets[2].text = max > 0 ? "PgUp/PgDn scroll   any other key returns"
                              : "Press any key to return to the editor";

    dialog_init(&d, "Keyboard Reference", widgets, 3, cols - 2, rows - 2);
    lp = layer_get(li);
    dialog_layout(&d, lp);
    editor_refresh_screen();

    c = ui_wait_key();
    /* A resize is not a dismiss: go round again at the new size. */
    if (c == WINCH_KEY) continue;
    /* Nor is a mouse event. The wheel scrolls, as it would anywhere else; a click
     * is ignored rather than treated as the "any other key" that returns, because
     * closing the reference on a stray click would be a nasty surprise. */
    if (c == MOUSE_KEY) {
      struct mouse_event ev;
      input_last_mouse(&ev);
      if (ev.type == MOUSE_WHEEL_UP) widget_text_scroll(&widgets[0], -3);
      else if (ev.type == MOUSE_WHEEL_DOWN) widget_text_scroll(&widgets[0], 3);
      continue;
    }
    if (max == 0) break;
    if (c == ARROW_DOWN) widget_text_scroll(&widgets[0], 1);
    else if (c == ARROW_UP) widget_text_scroll(&widgets[0], -1);
    else if (c == PAGE_DOWN) widget_text_scroll(&widgets[0], pane_h);
    else if (c == PAGE_UP) widget_text_scroll(&widgets[0], -pane_h);
    else if (c == HOME_KEY) widget_text_scroll(&widgets[0], -widgets[0].count);
    else if (c == END_KEY) widget_text_scroll(&widgets[0], widgets[0].count);
    else break;
  }

  layer_pop_to(depth);
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

/* The list asks for a row rather than being handed one, so nothing is copied into
 * the widget to be displayed. Directories are marked with a trailing slash, which
 * is also how they sort apart from files. */
struct browser_state {
  struct browser_entry *ents;
  int n;
};

static void browser_row(void *user, int i, char *dst, size_t dsz) {
  struct browser_state *bs = user;
  if (i < 0 || i >= bs->n) {
    if (dsz) dst[0] = '\0';
    return;
  }
  snprintf(dst, dsz, "%s%s", bs->ents[i].name,
           bs->ents[i].is_dir ? "/" : "");
}

/* Show the tail of an over-long path: which corner of the tree we are in matters
 * more than which root, and the cut lands on a character boundary. */
static const char *path_tail(const char *dir, int cols) {
  int len = (int)strlen(dir);
  const char *p = dir;
  if (cols < 1) cols = 1;
  if (len > cols) {
    p = dir + len - cols;
    while (*p != '\0' && ((unsigned char)*p & 0xC0) == 0x80) p++;
  }
  return p;
}

/* The open-file dialog: returns a malloc'd path to open, or NULL if cancelled.
 *
 * A framed dialog on the layer stack now, rather than a full-screen takeover. It
 * draws no escape sequences and clears nothing: it fills in cells, and the ordinary
 * refresh paints it over the editor with a shadow. The loop stays here -- laying
 * out, refreshing and reading a key -- which is what keeps this a straight-line
 * function instead of a set of callbacks. */
char *editor_open_dialog(void) {
  char dir[PATH_MAX];
  struct browser_state bs;
  struct widget widgets[4];
  struct dialog d;
  struct layer l;
  const char *msg = "";
  char *chosen = NULL;
  int depth, li;

  if (getcwd(dir, sizeof(dir)) == NULL) snprintf(dir, sizeof(dir), ".");
  bs.ents = NULL;
  bs.n = browser_load(dir, &bs.ents);
  if (bs.n == -1) {
    snprintf(dir, sizeof(dir), ".");
    bs.n = browser_load(dir, &bs.ents);
    if (bs.n == -1) return NULL;
  }

  /* Built once so the selection and scroll position survive a resize; only their
   * geometry is recomputed per pass. */
  widget_label(&widgets[0], 0, 0, 1, dir);
  widget_list(&widgets[1], 0, 1, 1, 1, bs.n, browser_row, &bs);
  widget_label(&widgets[2], 0, 2, 1, "");
  widget_label(&widgets[3], 0, 3, 1,
               "Enter open   Esc cancel");

  depth = layer_count();
  memset(&l, 0, sizeof(l));
  li = layer_push(&l);
  if (li < 0) {
    browser_free(bs.ents, bs.n);
    return NULL;
  }

  for (;;) {
    struct layer *lp;
    int cols, rows, list_h, c, ns;

    /* Before measuring anything: see above screen_sync_size. */
    screen_sync_size();

    /* Sized to the terminal, with a ceiling so it stays a dialog on a large
     * screen rather than growing into one. */
    cols = grid_cols() - 8;
    if (cols > 72) cols = 72;
    if (cols < 16) cols = 16;
    rows = grid_rows() - 4;
    if (rows > 20) rows = 20;
    if (rows < 6) rows = 6;

    /* Content rows: the path, the list, a message line, the key hints. */
    list_h = rows - 2 - 3;
    if (list_h < 1) list_h = 1;

    widgets[0].w = cols - 4;
    widgets[0].text = path_tail(dir, cols - 4);
    widgets[1].y = 1;
    widgets[1].w = cols - 4;
    widgets[1].h = list_h;
    widgets[1].count = bs.n;
    widgets[2].y = 1 + list_h;
    widgets[2].w = cols - 4;
    widgets[2].text = msg;
    widgets[3].y = 2 + list_h;
    widgets[3].w = cols - 4;
    widget_list_clamp(&widgets[1]);

    dialog_init(&d, "Open File", widgets, 4, cols - 2, rows - 2);
    lp = layer_get(li);
    dialog_layout(&d, lp);
    editor_refresh_screen();

    c = ui_wait_key();
    msg = "";
    if (c == WINCH_KEY) continue;

    if (c == MOUSE_KEY) {
      struct mouse_event ev;
      input_last_mouse(&ev);
      if (ev.type == MOUSE_WHEEL_UP) widget_list_move(&widgets[1], -3);
      else if (ev.type == MOUSE_WHEEL_DOWN) widget_list_move(&widgets[1], 3);
      else if (ev.type == MOUSE_PRESS && ev.button == MOUSE_BUTTON_LEFT) {
        /* The list's rows in screen terms: the layer's origin, past the frame and
         * the row the path occupies. A click outside them is ignored rather than
         * clamped, so clicking the frame does not jump the selection. */
        int row0 = lp->y + 1 + widgets[1].y;
        int k = ev.y - row0;
        if (k >= 0 && k < widgets[1].h && widgets[1].top + k < bs.n) {
          widgets[1].sel = widgets[1].top + k;
          widget_list_clamp(&widgets[1]);
        }
      }
      continue;
    }

    ns = 0;
    if (c == ARROW_DOWN || c == CTRL_KEY('n')) ns = 1;
    else if (c == ARROW_UP || c == CTRL_KEY('p')) ns = -1;
    else if (c == PAGE_DOWN) ns = list_h;
    else if (c == PAGE_UP) ns = -list_h;
    else if (c == HOME_KEY) ns = -bs.n;
    else if (c == END_KEY) ns = bs.n;
    else if (c == '\r') {
      char path[PATH_MAX];
      struct browser_entry *ne;
      int nn;
      if (bs.n == 0) continue;
      snprintf(path, sizeof(path), "%s/%s", dir, bs.ents[widgets[1].sel].name);
      if (!bs.ents[widgets[1].sel].is_dir) {
        chosen = xstrdup(path);
        break;
      }
      nn = browser_load(path, &ne);
      if (nn == -1) {
        msg = "Cannot enter that directory";
        continue;
      }
      browser_free(bs.ents, bs.n);
      bs.ents = ne;
      bs.n = nn;
      snprintf(dir, sizeof(dir), "%s", path);
      widgets[1].sel = 0;
      widgets[1].top = 0;
      continue;
    } else if (c == '\x1b' || c == CTRL_KEY('c')) {
      break;
    } else {
      continue;
    }
    widget_list_move(&widgets[1], ns);
  }

  layer_pop_to(depth);
  browser_free(bs.ents, bs.n);
  return chosen;
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
