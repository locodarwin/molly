/* screen.c -- Drawing: the append buffer, the partial-refresh cache, the gutter, and the
   text, status and message rows. */

#include "molly.h"

/* Make room for `extra` more bytes. Capacity doubles, so a frame costs a few
 * reallocs in total; growing by exactly the appended length cost one realloc
 * per append, and the status bar appended its padding a byte at a time. */
static void ab_reserve(struct abuf *ab, int extra) {
  if (ab->b != NULL && ab->len + extra <= ab->cap) return;
  int cap = ab->cap ? ab->cap : 4096;
  while (cap < ab->len + extra) cap *= 2;
  char *new = realloc(ab->b, (size_t)cap);
  if (new == NULL) die("ab_append");
  ab->b = new;
  ab->cap = cap;
}

void ab_append(struct abuf *ab, const char *s, int len) {
  ab_reserve(ab, len);
  memcpy(&ab->b[ab->len], s, len);
  ab->len += len;
}


/* The renderer's remaining state. The grid holds the screen itself (grid.c);
 * what is left here is whether the next frame must repaint everything and the
 * last terminal size we saw. */
/* How many rows the chrome takes above and below the text area. Declared up here
 * because the size calculation needs them and they are defined with the layout. */
static int chrome_rows_above(void);
static int chrome_rows_below(void);

/* One window needs a text row and a status row; below that the layout would be
 * handing out negative heights. The grid clips, so a terminal this small draws
 * a mess rather than crashing -- which is the same thing molly did before. */
#define WIN_AREA_MIN_ROWS 2

static int force_full;
static int last_wsrows;
static int last_wscols;

/* Width of the line-number gutter for this frame: 0 when the toggle is off,
 * else the digits of the largest line number plus the three-column " │ "
 * separator ("123 │ text"). Sized from numrows so it stays put while typing
 * and only widens when the buffer grows past the next power of ten. Collapses
 * to 0 when the window is too narrow to leave even one text column, so a
 * 3-column terminal never asks the row renderer to fit text into a
 * non-positive width. */
int screen_gutter_width(void) {
  /* The git sign column first, a column of its own; then the numbers. */
  int w = git_signs();
  if (V.linenum && V.buf->tty == NULL) {
    int digits = 1, n = row_count();
    while (n >= 10) { n /= 10; digits++; }
    w += digits + 3;
  }
  if (V.screencols - w < 1) return 0;
  return w;
}

/* The sign column's cell for a row: a bar for a line added or changed since
 * the last commit, a wedge where lines were taken out. */
static void gutter_draw_sign(int y, int filerow) {
  int m = filerow >= 0 ? git_mark(V.buf, filerow) : 0;
  if (m == GIT_ADDED || m == GIT_CHANGED) {
    grid_role(m == GIT_ADDED ? ROLE_GIT_ADDED : ROLE_GIT_CHANGED);
    grid_puts(y, V.screenx, "\xe2\x96\x8e", 3);
  } else if (m == GIT_DELETED) {
    grid_role(ROLE_GIT_DELETED);
    grid_puts(y, V.screenx, "\xe2\x96\xb8", 3);
  } else {
    grid_role(ROLE_TEXT);
    grid_puts(y, V.screenx, " ", 1);
  }
  grid_role(ROLE_TEXT);
}

/* The gutter takes ROLE_GUTTER so the numbers and the U+2502
 * bar stay quieter than the text. On the grid these are plain column writes; the
 * byte-counting the old builders had to do -- escape sequences and the three-byte
 * separator occupy no columns -- is gone with the byte strings. */
#define GUTTER_SEP "\xe2\x94\x82"

/* Text row: the line number right-aligned, then " | " drawn with the bar glyph:
 * "123 | ". Occupies exactly gutter_w columns.
 *
 * Columns are counted from V.screenx and not from zero. These three functions
 * already read the view for the gutter's width, the horizontal scroll and the
 * text width, so where the view starts on the screen is one more measurement
 * from the same place -- and it is 0 for every window until one is split.
 *
 * A wrapped line's continuation rows (frag > 0) get the separator and a blank
 * number field: they are the same line, and a column of the number repeated
 * down the gutter would say they were not. */
static void gutter_draw_number(int y, int filerow, int gutter_w, int frag) {
  char num[32];
  int n, s = (gutter_w > 0) ? git_signs() : 0;
  if (gutter_w <= 0) return;
  if (s) {
    gutter_draw_sign(y, frag > 0 ? -1 : filerow);
    gutter_w -= s;
    if (gutter_w <= 0) return;
  }
  if (frag > 0)
    n = snprintf(num, sizeof(num), "%*s ", gutter_w - 3, "");
  else
    n = snprintf(num, sizeof(num), "%*d ", gutter_w - 3, filerow + 1);
  grid_role(ROLE_GUTTER);
  grid_puts(y, V.screenx + s, num, n);
  grid_puts(y, V.screenx + s + gutter_w - 2, GUTTER_SEP " ", 4);
  grid_role(ROLE_TEXT);
}

/* Below-EOF row: a blank number field, the separator, then the gray tilde:
 * "    | ~", occupying gutter_w + 1 columns. */
static void gutter_draw_tilde(int y, int gutter_w) {
  int s = (gutter_w > 0) ? git_signs() : 0;
  grid_role(ROLE_GUTTER);
  if (gutter_w - s <= 0) {
    grid_puts(y, V.screenx + s, "~", 1);
  } else if (s) {
    char blank[32];
    int n = snprintf(blank, sizeof(blank), "%*s ", gutter_w - s - 3, "");
    grid_puts(y, V.screenx + s, blank, n);
    grid_puts(y, V.screenx + gutter_w - 2, GUTTER_SEP " ~", 5);
  } else {
    char blank[32];
    int n = snprintf(blank, sizeof(blank), "%*s ", gutter_w - 3, "");
    grid_puts(y, V.screenx, blank, n);
    grid_puts(y, V.screenx + gutter_w - 2, GUTTER_SEP " ~", 5);
  }
  grid_role(ROLE_TEXT);
}

/* Draw one text row into the grid, honoring display width: horizontal scroll
 * (coloff) and the screen width are measured in columns, so a slice starts and
 * ends on character boundaries. A wide glyph split by either edge is dropped and
 * its columns left blank, keeping every following column aligned.
 *
 * Characters whose display-column range overlaps [sel0, sel1) take ROLE_SELECTION.
 * On the grid that is a property of the cell rather than an escape sequence spliced
 * into the row's bytes, so the renderer no longer has to reason about sequences
 * that occupy no columns. */
/* ---- the view's aids ------------------------------------------------------
 *
 * Five things that help reading code and change nothing in it, each a [view]
 * setting and each off until asked for: whitespace drawn as marks, indent
 * guides, the cursor's line tinted, a ruler column, and the word under the
 * cursor tinted wherever else it appears. Per screen rather than per window,
 * like the colours, because they are how the screen is drawn. */
static int vw_ws, vw_guides, vw_curline, vw_occ, vw_ruler;

int view_whitespace(void) { return vw_ws; }
int view_guides(void) { return vw_guides; }
int view_curline(void) { return vw_curline; }
int view_occurrences(void) { return vw_occ; }
int view_ruler(void) { return vw_ruler; }
void view_set_whitespace(int on) { vw_ws = on ? 1 : 0; }
void view_set_guides(int on) { vw_guides = on ? 1 : 0; }
void view_set_curline(int on) { vw_curline = on ? 1 : 0; }
void view_set_occurrences(int on) { vw_occ = on ? 1 : 0; }
void view_set_ruler(int col) { vw_ruler = col > 0 ? col : 0; }

static int word_byte_at(const char *s, int i) {
  unsigned char c = (unsigned char)s[i];
  return isalnum(c) || c == '_' || c >= 0x80;
}

/* The word under the cursor of the view being drawn, as bytes of its row: 0
 * when the cursor is not on one, or it is a single character -- a word of one
 * letter is everywhere, and tinting every `i` says nothing. */
static int cursor_word(const char **w, int *len) {
  erow *r;
  int a, b;
  if (CUR.cy >= row_count()) return 0;
  r = row_at(CUR.cy);
  a = b = CUR.cx;
  while (a > 0 && word_byte_at(r->chars, a - 1)) a--;
  while (b < r->size && word_byte_at(r->chars, b)) b++;
  if (b - a < 2) return 0;
  *w = r->chars + a;
  *len = b - a;
  return 1;
}

/* Where the word occurs in this row as a whole word: up to `max` ranges. */
static int occurrences(const char *s, int size, const char *w, int len,
                       int *from, int max) {
  int i, n = 0;
  for (i = 0; i + len <= size && n < max; i++) {
    if (memcmp(s + i, w, (size_t)len) != 0) continue;
    if (i > 0 && word_byte_at(s, i - 1)) continue;
    if (i + len < size && word_byte_at(s, i + len)) continue;
    from[n++] = i;
    i += len - 1;
  }
  return n;
}

void editor_draw_text_row(int y, erow *row, const struct drow *d,
                          int sel0, int sel1) {
  const char *r = row->chars;
  int rs = row->size;
  int tabw = tab_width();
  int col = 0, i = 0;
  const unsigned char *roles = syntax_roles(d->filerow);

  /* First display column to show: where this slice starts, plus the horizontal
   * scroll. Wrap and horizontal scrolling are alternatives -- a wrapped view has
   * nothing to scroll sideways to -- so only one of the two is ever nonzero. */
  int skip = d->rcol0 + V.coloff;

  /* Skip whole characters that lie entirely left of the viewport. A tab is
   * walked a column at a time (see text_cell), so its visible part draws. */
  while (i < rs && col < skip) {
    int nb, w = text_cell(r, rs, i, col, tabw, &nb);
    if (col + w > skip) break;
    col += w;
    i += nb;
  }
  /* A wide glyph straddling the left edge is dropped; its visible right half
   * becomes that many blank columns. */
  int left_pad = 0;
  if (i < rs && col < skip) {
    int nb, w = text_cell(r, rs, i, col, tabw, &nb);
    left_pad = (col + w) - skip;
    i += nb;
    col += w;
  }

  int has_sel = (sel1 > sel0);
  /* What a language server says is wrong on this row. A handful of spans, so
   * each character asks each of them; a row with forty problems on it shows
   * the first eight underlined and the status bar still counts them all. */
  struct diag_span spans[8];
  int nspans = (d->filerow < V.buf->numrows)
      ? lspui_row_spans(V.buf, d->filerow, row, spans, 8) : 0;
  int gw = V.gutter_w;
  int text_cols = TEXT_COLS();
  /* Stop at the end of the slice as well as the edge of the screen. Unwrapped,
   * the slice is the whole row (DROW_ALL) and the screen is the only limit. */
  if (d->rcols < text_cols) text_cols = d->rcols;

  gutter_draw_number(y, d->filerow, gw, d->frag);

  int x = V.screenx + gw + left_pad;  /* the pad columns stay blank */
  int dcol = col;         /* absolute display column of the character at i */
  int vis = left_pad;     /* visible columns emitted so far */

  /* The aids' facts about this row: where its indentation ends and its
   * trailing blanks begin, whether it is the cursor's, and where the word
   * under the cursor is in it. */
  int lead = 0, trail = rs, prev_i = -1;
  int on_cursor_line = vw_curline && d->filerow == CUR.cy;
  int occ_from[32], nocc = 0, occ_len = 0;
  while (lead < rs && (r[lead] == ' ' || r[lead] == '\t')) lead++;
  while (trail > 0 && (r[trail - 1] == ' ' || r[trail - 1] == '\t')) trail--;
  if (vw_occ) {
    const char *wd;
    if (cursor_word(&wd, &occ_len)) nocc = occurrences(r, rs, wd, occ_len, occ_from, 32);
  }

  while (i < rs && vis < text_cols) {
    int nb, w = text_cell(r, rs, i, dcol, tabw, &nb);
    int tint = -1, mark = 0, o;
    if (vis + w > text_cols) break;  /* would overflow the right edge */
    /* A selection outranks the syntax colouring rather than blending with it:
     * reverse video over a keyword's own colour is unreadable, and what the
     * user is being told at that moment is "this is selected". Without a
     * filetype, or with highlighting off, there are no roles and this is the
     * line it always was. */
    int selected = has_sel && dcol >= sel0 && dcol < sel1;
    int sev = 0, k;
    for (k = 0; k < nspans; k++)
      if (i >= spans[k].x0 && i < spans[k].x1 &&
          (sev == 0 || spans[k].severity < sev))
        sev = spans[k].severity;
    /* An error or a warning takes its colour over the syntax's, and the other
     * two keep the syntax's; all four are underlined. The selection outranks
     * both, and keeps the underline, so selecting a problem does not hide
     * where it is. */
    /* Whitespace marks and indent guides: a tab's first column is an arrow,
     * a trailing blank a dot, and a tab stop inside the indentation a bar.
     * Drawn in place of the blank, never written to the file. */
    if (vw_ws && r[i] == '\t' && i != prev_i) mark = 1;
    else if (vw_ws && r[i] == ' ' && i >= trail) mark = 2;
    else if (vw_guides && i < lead && dcol > 0 && dcol % tabw == 0) mark = 3;
    for (o = 0; o < nocc; o++)
      if (i >= occ_from[o] && i < occ_from[o] + occ_len) tint = ROLE_OCCURRENCE;
    if (tint < 0 && vw_ruler && dcol == vw_ruler - 1) tint = ROLE_RULER;
    if (tint < 0 && on_cursor_line) tint = ROLE_CURRENT_LINE;
    grid_role(selected ? ROLE_SELECTION
              : (sev == 1 || sev == 2) ? lspui_severity_role(sev)
              : mark ? ROLE_WHITESPACE
              : roles ? roles[i] : ROLE_TEXT);
    if (sev) grid_attrs(ATTR_UNDERLINE);
    if (!selected) grid_tint(tint);
    prev_i = i;
    if (mark == 1)
      grid_put_char(y, x, "\xe2\x86\x92", 3, 1);
    else if (mark == 2)
      grid_put_char(y, x, "\xc2\xb7", 2, 1);
    else if (mark == 3)
      grid_put_char(y, x, "\xe2\x94\x82", 3, 1);
    else if (r[i] == '\t')
      grid_put_char(y, x, " ", 1, 1);
    else
      grid_put_char(y, x, r + i, nb, w);
    x += w;    /* a combining mark has w == 0 and joins the cell to its left */
    vis += w;
    dcol += w;
    i += nb;
  }
  /* A folded block's first row says what it hides, after its text. */
  if (V.buf->nfolds > 0 && d->rcols == DROW_ALL && d->filerow < V.buf->numrows) {
    int f = fold_at(V.buf, d->filerow);
    if (f >= 0 && vis < text_cols) {
      char mark[48];
      int len = snprintf(mark, sizeof(mark), " \xe2\x8b\xaf %d line%s",
                         fold_size(V.buf, f), fold_size(V.buf, f) == 1 ? "" : "s");
      int used;
      grid_role(ROLE_WHITESPACE);
      used = grid_puts(y, x, mark, len);
      if (used > text_cols - vis) used = text_cols - vis;
      x += used;
      vis += used;
      dcol += used;
    }
  }
  /* Past the text: the current line's tint goes to the window's edge, and
   * the ruler's column is drawn whether the line reaches it or not. */
  if (on_cursor_line || vw_ruler) {
    while (vis < text_cols) {
      int tint = (vw_ruler && dcol == vw_ruler - 1) ? ROLE_RULER
               : on_cursor_line ? ROLE_CURRENT_LINE : -1;
      if (tint >= 0 && !(has_sel && dcol >= sel0 && dcol < sel1)) {
        grid_role(ROLE_TEXT);
        grid_tint(tint);
        grid_put_char(y, x, " ", 1, 1);
      }
      x++;
      vis++;
      dcol++;
    }
  }
  grid_role(ROLE_TEXT);
}

/* The text area of the current view, at the rect window_layout() gave it.
 *
 * It took a layer until there were splits, which meant one text area at a fixed
 * place on the screen. Now the rect comes from the view, there is one of these
 * per window, and window.c calls it once per leaf with that leaf's view
 * current -- so the menu bar appearing above, or a second window appearing
 * beside, are both changes to the layout rather than to this function. */
void screen_draw_text_area(void) {
  int y;
  if (V.buf->tty != NULL) {
    tty_draw();
    return;
  }
  int gw = V.gutter_w;
  int text_cols = TEXT_COLS();
  int x0 = V.screenx, y0 = V.screeny;
  for (y = 0; y < V.screenrows; y++) {
    struct drow d;
    int filerow;
    drow_at(y + V.drowoff, &d);
    filerow = d.filerow;
    if (filerow >= row_count()) {
      if (row_count() == 0 && y == V.screenrows / 3) {
        char text[96];
        int welcomelen = snprintf(text, sizeof(text),
                                  "Molly editor -- version %s", MOLLY_VERSION);
        if (welcomelen > text_cols) welcomelen = text_cols;
        int pad = (text_cols - welcomelen) / 2;
        if (pad > 0) {
          /* The tilde keeps its column and the greeting is centered in what is
           * left, which is why the text starts one column past the prefix. */
          gutter_draw_tilde(y0 + y, gw);
          grid_puts(y0 + y, x0 + (gw ? gw + 1 : 1) + pad, text, welcomelen);
        } else {
          /* Too narrow to center: the greeting takes the row. */
          grid_puts(y0 + y, x0, text, welcomelen);
        }
      } else {
        gutter_draw_tilde(y0 + y, gw);
      }
    } else {
      int s0, s1;
      selection_row_rx(filerow, &s0, &s1);
      editor_draw_text_row(y0 + y, row_at(filerow), &d, s0, s1);
      cursors_mark_row(y0 + y, filerow, &d);
    }
  }
}

/* The status bar of the current view, on the window's last row.
 *
 * One per window rather than one per screen, which is what makes a split
 * readable at all: with two files side by side the question "which is which"
 * has to be answerable without moving the keyboard, and the bar already says
 * the file's name, its length and whether it is modified. With one window it
 * lands on exactly the row it always did, which is why this change is invisible
 * until something splits. */
void screen_draw_status_bar(int focused) {
  int y = V.screeny + V.screenrows;
  int x = V.screenx, w = V.screencols;
  char status[80], rstatus[80];
  /* With more than one buffer open the bar says which of them this is. It is
   * left off entirely with one, because "[1/1]" is noise on the common case --
   * and because until the tab bar exists this counter is the only thing on
   * screen that says there are others at all. */
  char which[24] = "";
  if (buf_count() > 1)
    snprintf(which, sizeof(which), "[%d/%d] ", buf_current() + 1, buf_count());
  /* The filetype, where molly recognised one. It earns its place on a crowded
   * bar by answering the question the colours raise: when a file is not
   * highlighted, the useful thing to know is whether molly failed to recognise
   * it or highlighting is off. */
  if (B.tty != NULL) {
    /* A terminal has no lines to count or cursor to place: its name, which
     * the program in it may have set, and what it is doing. */
    grid_role(focused ? ROLE_STATUS : ROLE_STATUS_UNFOCUSED);
    grid_fill(y, x, w, ' ');
    {
      int len = snprintf(status, sizeof(status), "%s%.40s", which, buf_name(V.buf));
      int rlen;
      tty_status(rstatus, sizeof(rstatus));
      rlen = (int)strlen(rstatus);
      if (len > w) len = w;
      grid_puts(y, x, status, len);
      if (w - len > rlen) grid_puts(y, x + w - rlen, rstatus, rlen);
    }
    grid_role(ROLE_TEXT);
    return;
  }
  char lang[24] = "";
  if (V.syntax && B.syn != NULL)
    snprintf(lang, sizeof(lang), " [%s]", syntax_name(B.syn));
  int len = snprintf(status, sizeof(status), "%s%.20s - %d lines%s %s",
      which, buf_name(V.buf), row_count(), lang,
      B.readonly ? "[read-only]" : B.dirty ? "(modified)" : "");
  /* The character column: the Nth character in the line, not CUR.rx's
   * tab-stop- and glyph-width-aware display column, which jumps around
   * (a leading tab alone puts the display column at 9) and doesn't match
   * what "column" means in most editors and tools. CUR.cy == row_count() is
   * the phantom line past the last row, which has no erow to index. */
  int charcol = (CUR.cy < row_count())
      ? editor_row_cx_to_charcol(row_at(CUR.cy), CUR.cx) : 0;
  /* How many cursors, once there is more than one. The count and not a marker,
   * because with cursors scrolled off the top or bottom of the window the
   * number is the only way to know how many places the next keystroke lands
   * in -- and that is exactly the thing worth knowing before typing. */
  /* Errors and warnings, when there are any: whether this file has something
   * wrong with it anywhere, not only on screen. Information and hints are not
   * counted, which is the same line the colours in the text draw. */
  char probs[32] = "";
  {
    int ne, nw;
    lspui_counts(V.buf, &ne, &nw);
    if (ne && nw) snprintf(probs, sizeof(probs), "E:%d W:%d  ", ne, nw);
    else if (ne) snprintf(probs, sizeof(probs), "E:%d  ", ne);
    else if (nw) snprintf(probs, sizeof(probs), "W:%d  ", nw);
  }
  int rlen;
  /* Recording a macro is a mode, and a mode nobody can see is a trap: the bar
   * says so for as long as it lasts. */
  if (macro_recording()) {
    char rec[sizeof(probs)];
    snprintf(rec, sizeof(rec), "REC  %s", probs);
    snprintf(probs, sizeof(probs), "%s", rec);
  }
  if (V.ncursors > 1)
    rlen = snprintf(rstatus, sizeof(rstatus), "%sLn %d, Col %d  %d cursors",
        probs, CUR.cy + 1, charcol + 1, V.ncursors);
  else if (CUR.sel_active)
    rlen = snprintf(rstatus, sizeof(rstatus), "%sLn %d, Col %d  Sel %d",
        probs, CUR.cy + 1, charcol + 1, selection_bytes());
  else
    rlen = snprintf(rstatus, sizeof(rstatus), "%sLn %d, Col %d",
        probs, CUR.cy + 1, charcol + 1);
  if (len > w) len = w;

  /* The whole bar is reverse video, so it is filled first and written over.
   * Olly got the same effect by erasing to end of line with reverse video
   * active, which worked but meant the bar's extent was implied by a terminal
   * side effect rather than stated. */
  grid_role(focused ? ROLE_STATUS : ROLE_STATUS_UNFOCUSED);
  grid_fill(y, x, w, ' ');
  grid_puts(y, x, status, len);
  /* Right-align the cursor position when it fits; otherwise leave it off. */
  if (w - len >= rlen)
    grid_puts(y, x + w - rlen, rstatus, rlen);
  grid_role(ROLE_TEXT);
}

static void draw_message_bar(const struct layer *self) {
  int y = self->y;
  int msglen = strlen(E.statusmsg);
  char diag[256];
  int sev;
  int fresh = msglen && time(NULL) - E.statusmsg_time < 5;
  int moved = E.statusmsg_buf != V.buf || E.statusmsg_cx != CUR.cx ||
              E.statusmsg_cy != CUR.cy;
  if (msglen > self->w) msglen = self->w;
  grid_role(ROLE_MESSAGE);
  sev = lspui_cursor_message(diag, sizeof(diag));
  if (fresh && !(sev && moved)) {
    grid_puts(y, self->x, E.statusmsg, msglen);
  } else if (sev) {
    /* The bar says what is wrong where the cursor is -- the severity in its
     * colour, the message in the bar's. A message the user was just shown
     * keeps the bar for its five seconds while the cursor stays where it was
     * said, because it is the answer to something they did; moving onto a
     * problem is asking about that instead. */
    int cols;
    int wlen = (int)strcspn(diag, ":");
    int dlen = utf8_fit_cols(diag, self->w, &cols);
    if (wlen > dlen) wlen = dlen;
    grid_role(lspui_severity_role(sev));
    grid_puts(y, self->x, diag, wlen);
    grid_role(ROLE_MESSAGE);
    grid_puts(y, self->x + wlen, diag + wlen, dlen - wlen);
  }
  grid_role(ROLE_TEXT);
}

/* Make the next frame repaint every row instead of only the rows the cache
 * believes changed. Needed by Ctrl-L, by a resize, and by anything that paints
 * over the text area and then leaves -- the help screen, the open dialog. */
void screen_force_repaint(void) {
  force_full = 1;
}

/* Re-read the terminal size into the view, keeping the last known size when the
 * query fails rather than whatever half-set values it left behind. Returns 1 if
 * the size changed since the previous call.
 *
 * The help screen runs its own event loop and so needs this too; it used to carry
 * a copy of this logic, which is how the two could drift apart. */
int screen_poll_size(void) {
  int wsrows, wscols, changed;
  if (get_window_size(&wsrows, &wscols) == -1) {
    wsrows = last_wsrows;
    wscols = last_wscols;
  }
  changed = (wsrows != last_wsrows || wscols != last_wscols);
  last_wsrows = wsrows;
  last_wscols = wscols;
  /* No longer the place a view learns its size: the terminal's rows are shared
   * out between windows, which is window_layout()'s job. What is kept here is
   * the terminal's own size, which is what the layout divides up. */
  return changed;
}

/* The rect the window tree has to lay itself out in: the whole terminal less
 * the chrome above it and the message bar below. The status bar is not in that
 * reckoning any more -- each window carries its own on its last row, so one
 * window still puts a status bar exactly where the screen-wide one used to be
 * and gets the message bar underneath it. */
void screen_window_area(int *x, int *y, int *w, int *h) {
  int top = chrome_rows_above();
  *x = 0;
  *y = top;
  *w = last_wscols;
  *h = last_wsrows - top - chrome_rows_below();
  if (*h < WIN_AREA_MIN_ROWS) *h = WIN_AREA_MIN_ROWS;
  if (*w < 1) *w = 1;
}

/* The size at startup. Unlike screen_poll_size there is no last known size to
 * fall back on, so a failed query is fatal. Ends with a layout pass, so that
 * the geometry every view reads is valid before the first frame -- and before
 * anything can ask whether there is room to split. */
void screen_init_size(void) {
  if (get_window_size(&last_wsrows, &last_wscols) == -1)
    die("get_window_size");
  force_full = 1;
  V.gutter_w = screen_gutter_width();
  window_layout();
}

/* Release the renderer's buffers. Teardown lives here so that editor_cleanup
 * does not have to know what the renderer allocates. */
void screen_cleanup(void) {
  grid_cleanup();
}

/* Where an Alt-press landed, for a drag that follows it. */
static int alt_pressed = 0;
static int alt_press_row, alt_press_rx;

/* A click, drag or wheel in the text area.
 *
 * A press puts the cursor where it was clicked and drops any selection; a drag
 * extends a selection from where the press landed, which is the same anchor-plus-
 * cursor model the Shift+arrow keys use, so cutting and copying work on a
 * mouse-made selection without knowing it was made with a mouse. The wheel scrolls
 * the view without moving the cursor, as every other editor does.
 *
 * Which window the event is in was decided before this is called: window.c
 * finds the leaf, moves the keyboard there and lends it to this, so everything
 * below reads the view it should and the cell-to-position arithmetic is the
 * same whether there is one window or six. */
void screen_text_mouse(const struct mouse_event *ev) {
  int drow, filerow, rcol;

  if (V.buf->tty != NULL) {
    tty_mouse(ev);
    return;
  }

  if (ev->type == MOUSE_WHEEL_UP || ev->type == MOUSE_WHEEL_DOWN) {
    int step = (ev->type == MOUSE_WHEEL_UP) ? -3 : 3;
    V.drowoff += step;
    if (V.drowoff > drow_total()) V.drowoff = drow_total();
    if (V.drowoff < 0) V.drowoff = 0;
    return;
  }
  if (ev->type == MOUSE_RELEASE) return;  /* nothing to do, but it is ours */
  if (ev->button != MOUSE_BUTTON_LEFT) return;

  /* Screen cell to buffer position. The gutter is not text: a click in it lands at
   * the start of the line, which is what clicking a line number should do. */
  drow = ev->y - V.screeny + V.drowoff;
  if (drow < 0) drow = 0;
  {
    /* The slice that was drawn on this screen line, which is what says both
     * which file row was clicked and which of its columns the x lands in.
     * Unwrapped the slice starts at column zero and runs to the end of the row,
     * so this is the identity it always was. */
    struct drow d;
    drow_at(drow, &d);
    filerow = d.filerow;
    if (filerow > row_count()) filerow = row_count();

    rcol = ev->x - V.screenx - V.gutter_w + V.coloff + d.rcol0;
    if (rcol < d.rcol0) rcol = d.rcol0;
    /* A click past the end of a continuation that is not the row's last lands
     * on its final column rather than running into the text below it. */
    if (d.rcols != DROW_ALL && rcol > d.rcol0 + d.rcols - 1)
      rcol = d.rcol0 + d.rcols - 1;
  }

  if (ev->type == MOUSE_PRESS) {
    int cx = (filerow < row_count())
        ? editor_row_rx_to_cx(row_at(filerow), rcol) : 0;
    /* Alt held adds a cursor where the click landed instead of moving the one
     * there is -- the mouse half of Ctrl-D, and the only way to put cursors
     * somewhere that is neither a match nor straight above or below. The
     * modifier already arrives decoded on the event; see struct mouse_event. */
    if (ev->mods & MOUSE_MOD_ALT) {
      cursors_add(filerow, cx);
      editor_set_status_message("%d cursors", V.ncursors);
      /* And if it then drags, the press was the corner of a block. */
      alt_press_row = filerow;
      alt_press_rx = rcol;
      alt_pressed = 1;
      return;
    }
    alt_pressed = 0;
    /* A plain click is how you get out of a multi-cursor gesture with the
     * mouse, so it puts the one cursor where it was clicked. */
    cursors_collapse();
    selection_clear();
    CUR.cy = filerow;
    CUR.cx = cx;
    return;
  }

  /* An Alt-press that moves is a block from where it was pressed. Decided by
   * the press, not by the drag's own report, so that letting go of Alt half
   * way through a drag does not turn a block into a stream selection. */
  if (alt_pressed) {
    block_set(alt_press_row, alt_press_rx, filerow, rcol);
    return;
  }

  /* MOUSE_DRAG: seed the anchor on the first movement, then track. */
  if (!CUR.sel_active) {
    CUR.sx = CUR.cx;
    CUR.sy = CUR.cy;
    CUR.sel_active = 1;
  }
  CUR.cy = filerow;
  CUR.cx = (filerow < row_count())
      ? editor_row_rx_to_cx(row_at(filerow), rcol) : 0;
}

/* The chrome's layers, pushed once and then only resized. Their indices are kept
 * so the layout pass can move them; everything above them on the stack is pushed
 * by whatever is showing it. */
static int layer_windows = -1;
static int layer_message = -1;
static int layer_menubar = -1;
static int layer_tabbar = -1;

/* Rows of chrome above and below the text area. The status and message bars are
 * always there; the menu bar will add one above, which is the whole reason the
 * text area's origin is a variable rather than zero. */
/* The menu bar's row, when it has one. While the bar is hidden it does not reserve a
 * row at all: an open menu draws its band *over* the top text row instead, because
 * reflowing the whole screen by a row every time a menu opens would be worse than
 * briefly covering a line of text. */
static int chrome_rows_above(void) {
  return (E.menubar ? 1 : 0) + tabbar_rows();
}

static int chrome_rows_below(void) {
  return 1;  /* the message bar; each window carries its own status row */
}

void screen_init_layers(void) {
  struct layer l;
  memset(&l, 0, sizeof(l));
  /* One layer for every window. The tree decides what goes where inside it, so
   * splitting is not a change to the layer stack -- which is what keeps menus,
   * dialogs and the help screen working over a split without knowing about it. */
  l.draw = window_draw;
  l.mouse = window_mouse;
  layer_windows = layer_push(&l);
  l.mouse = NULL;
  l.draw = draw_message_bar;
  layer_message = layer_push(&l);
  /* Above the chrome, so that while the bar is hidden an open menu's band covers the
   * text rather than being covered by it. Dialogs push higher still. */
  /* Below the menu bar in the stack as well as on the screen, so that an open
   * menu's drop-down covers the tabs rather than being covered by them. */
  l.draw = tabbar_draw;
  l.mouse = tabbar_mouse;
  layer_tabbar = layer_push(&l);
  l.draw = menu_bar_draw;
  l.mouse = menu_bar_mouse;
  layer_menubar = layer_push(&l);
  if (layer_windows < 0 || layer_message < 0 ||
      layer_menubar < 0 || layer_tabbar < 0)
    die("screen_init_layers");

  /* Not set here any more. The menu bar is visible by default -- it is molly's
   * main way of showing what it can do, and hiding it by default would waste
   * that -- but that default is now one line in config.c's settings table, and
   * config_apply() applies it immediately after init_editor(). Declaring it in
   * both places is how the two come to disagree. */
}

/* Give the chrome its bounds for this frame. One place does the row arithmetic
 * that the three draw functions each used to do for themselves. */
static void screen_layout(void) {
  int wx, wy, ww, wh;
  screen_window_area(&wx, &wy, &ww, &wh);
  layer_set_bounds(layer_windows, wx, wy, ww, wh);
  layer_set_bounds(layer_message, 0, wy + wh, ww, 1);
  /* The band occupies row zero when shown, and nothing at all when hidden -- except
   * while a menu is open, when it appears over the first text row. A layer with no
   * height is skipped by both drawing and hit-testing. */
  layer_set_bounds(layer_menubar, 0, 0, ww,
                   (E.menubar || menu_is_open()) ? 1 : 0);
  /* Directly under the band when there is one, and at row zero when there is
   * not. Its row is already counted in the window area's origin. */
  layer_set_bounds(layer_tabbar, 0, E.menubar ? 1 : 0, ww, tabbar_rows());
}

/* Bring the view and the grid in step with the terminal, without drawing anything.
 *
 * A modal loop that sizes itself from the screen has to call this before it does:
 * the grid only learns about a resize when a refresh begins, so laying out first
 * means laying out against the size from before the resize. That is not a cosmetic
 * slip -- the help dialog was centred for a 100-column terminal and drawn into a
 * 40-column one, which put its title off the right-hand edge and left it there,
 * because nothing repaints again until the next keystroke. */
void screen_sync_size(void) {
  int wx, wy, ww, wh;
  if (screen_poll_size()) force_full = 1;
  /* Share the terminal out between the windows, which is also where each view
   * learns its own size and its own gutter width. */
  window_layout();
  /* Each terminal is told the size of the window showing it, which is only
   * known once the windows have their sizes. */
  tty_layout();
  screen_window_area(&wx, &wy, &ww, &wh);
  /* A size change drops the grid's contents, so it repaints in full by itself. */
  grid_resize(wy + wh + chrome_rows_below(), ww);
}

void editor_refresh_screen(void) {
  screen_sync_size();
  /* Every window, not only the focused one: an edit in one moves the text under
   * another showing the same buffer. Scrolling settles the visible range, which
   * is what the lexing inside it then has to cover, so both happen here and
   * before anything is painted. */
  window_scroll_all();

  if (force_full) {
    grid_invalidate();
    force_full = 0;
  }

  screen_layout();
  grid_begin();
  layer_draw_all();
  /* The text cursor goes where the cursor in the buffer is -- unless something above
   * claimed it, which an input field does: while a prompt is open the cursor belongs
   * in the prompt, not back in the text behind it. */
  if (!grid_cursor_claimed()) {
    int ty, tx;
    if (V.buf->tty == NULL)
      grid_cursor_to(V.screeny + drow_of_cursor(&CUR) - V.drowoff,
                     V.screenx + (drow_cursor_col(&CUR) - V.coloff) + V.gutter_w);
    else if (tty_cursor(&ty, &tx))
      grid_cursor_to(ty, tx);
    else
      grid_cursor_hide();
  }
  grid_flush();
}

void editor_set_status_message(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
  E.statusmsg_buf = V.buf;
  if (V.buf != NULL && V.cursors != NULL) {
    E.statusmsg_cx = CUR.cx;
    E.statusmsg_cy = CUR.cy;
  }
}
