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
static int gutter_width(void) {
  if (!V.linenum) return 0;
  int digits = 1, n = row_count();
  while (n >= 10) { n /= 10; digits++; }
  int w = digits + 3;
  if (V.screencols - w < 1) return 0;
  return w;
}

/* The gutter is drawn gray (SGR 90 via ATTR_GRAY) so the numbers and the U+2502
 * bar stay quieter than the text. On the grid these are plain column writes; the
 * byte-counting the old builders had to do -- escape sequences and the three-byte
 * separator occupy no columns -- is gone with the byte strings. */
#define GUTTER_SEP "\xe2\x94\x82"

/* Text row: the line number right-aligned, then " | " drawn with the bar glyph:
 * "123 | ". Occupies exactly gutter_w columns. */
static void gutter_draw_number(int y, int filerow, int gutter_w) {
  char num[32];
  int n;
  if (gutter_w <= 0) return;
  n = snprintf(num, sizeof(num), "%*d ", gutter_w - 3, filerow + 1);
  grid_pen(ATTR_GRAY);
  grid_puts(y, 0, num, n);
  grid_puts(y, gutter_w - 2, GUTTER_SEP " ", 4);
  grid_pen(0);
}

/* Below-EOF row: a blank number field, the separator, then the gray tilde:
 * "    | ~", occupying gutter_w + 1 columns. */
static void gutter_draw_tilde(int y, int gutter_w) {
  grid_pen(ATTR_GRAY);
  if (gutter_w <= 0) {
    grid_puts(y, 0, "~", 1);
  } else {
    char blank[32];
    int n = snprintf(blank, sizeof(blank), "%*s ", gutter_w - 3, "");
    grid_puts(y, 0, blank, n);
    grid_puts(y, gutter_w - 2, GUTTER_SEP " ~", 5);
  }
  grid_pen(0);
}

/* Draw one text row into the grid, honoring display width: horizontal scroll
 * (coloff) and the screen width are measured in columns, so a slice starts and
 * ends on character boundaries. A wide glyph split by either edge is dropped and
 * its columns left blank, keeping every following column aligned.
 *
 * Characters whose display-column range overlaps [sel0, sel1) get ATTR_REVERSE.
 * On the grid that is a property of the cell rather than an escape sequence spliced
 * into the row's bytes, so the renderer no longer has to reason about sequences
 * that occupy no columns. */
void editor_draw_text_row(int y, erow *row, const struct drow *d,
                          int sel0, int sel1) {
  const char *r = row->render;
  int rs = row->rsize;
  int col = 0, i = 0;

  /* First render column to show: where this slice starts, plus the horizontal
   * scroll. Wrap and horizontal scrolling are alternatives -- a wrapped view has
   * nothing to scroll sideways to -- so only one of the two is ever nonzero. */
  int skip = d->rcol0 + V.coloff;

  /* Skip whole characters that lie entirely left of the viewport. */
  while (i < rs && col < skip) {
    int nb, w = utf8_char_cols(r, rs, i, &nb);
    if (col + w > skip) break;
    col += w;
    i += nb;
  }
  /* A wide glyph straddling the left edge is dropped; its visible right half
   * becomes that many blank columns. */
  int left_pad = 0;
  if (i < rs && col < skip) {
    int nb, w = utf8_char_cols(r, rs, i, &nb);
    left_pad = (col + w) - skip;
    i += nb;
    col += w;
  }

  int has_sel = (sel1 > sel0);
  int gw = V.gutter_w;
  int text_cols = TEXT_COLS();
  /* Stop at the end of the slice as well as the edge of the screen. Unwrapped,
   * the slice is the whole row (DROW_ALL) and the screen is the only limit. */
  if (d->rcols < text_cols) text_cols = d->rcols;

  gutter_draw_number(y, d->filerow, gw);

  int x = gw + left_pad;  /* the pad columns stay blank */
  int dcol = col;         /* absolute display column of the character at i */
  int vis = left_pad;     /* visible columns emitted so far */
  while (i < rs && vis < text_cols) {
    int nb, w = utf8_char_cols(r, rs, i, &nb);
    if (vis + w > text_cols) break;  /* would overflow the right edge */
    grid_pen((has_sel && dcol >= sel0 && dcol < sel1) ? ATTR_REVERSE : 0);
    grid_put_char(y, x, r + i, nb, w);
    x += w;    /* a combining mark has w == 0 and joins the cell to its left */
    vis += w;
    dcol += w;
    i += nb;
  }
  grid_pen(0);
}

void editor_draw_rows(void) {
  int y;
  int gw = V.gutter_w;
  int text_cols = TEXT_COLS();
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
          gutter_draw_tilde(y, gw);
          grid_puts(y, (gw ? gw + 1 : 1) + pad, text, welcomelen);
        } else {
          /* Too narrow to center: the greeting takes the row. */
          grid_puts(y, 0, text, welcomelen);
        }
      } else {
        gutter_draw_tilde(y, gw);
      }
    } else {
      int s0, s1;
      selection_row_rx(filerow, &s0, &s1);
      editor_draw_text_row(y, row_at(filerow), &d, s0, s1);
    }
  }
}

void editor_draw_status_bar(void) {
  int y = V.screenrows;
  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
      B.filename ? B.filename : "[No Name]", row_count(),
      B.dirty ? "(modified)" : "");
  /* The character column: the Nth character in the line, not CUR.rx's
   * tab-stop- and glyph-width-aware display column, which jumps around
   * (a leading tab alone puts the display column at 9) and doesn't match
   * what "column" means in most editors and tools. CUR.cy == row_count() is
   * the phantom line past the last row, which has no erow to index. */
  int charcol = (CUR.cy < row_count())
      ? editor_row_cx_to_charcol(row_at(CUR.cy), CUR.cx) : 0;
  int rlen = CUR.sel_active
      ? snprintf(rstatus, sizeof(rstatus), "Ln %d, Col %d  Sel %d",
          CUR.cy + 1, charcol + 1, selection_bytes())
      : snprintf(rstatus, sizeof(rstatus), "Ln %d, Col %d",
          CUR.cy + 1, charcol + 1);
  if (len > V.screencols) len = V.screencols;

  /* The whole bar is reverse video, so it is filled first and written over.
   * Olly got the same effect by erasing to end of line with reverse video
   * active, which worked but meant the bar's extent was implied by a terminal
   * side effect rather than stated. */
  grid_pen(ATTR_REVERSE);
  grid_fill(y, 0, V.screencols, ' ');
  grid_puts(y, 0, status, len);
  /* Right-align the cursor position when it fits; otherwise leave it off. */
  if (V.screencols - len >= rlen)
    grid_puts(y, V.screencols - rlen, rstatus, rlen);
  grid_pen(0);
}

void editor_draw_message_bar(void) {
  int y = V.screenrows + 1;
  int msglen = strlen(E.statusmsg);
  if (msglen > V.screencols) msglen = V.screencols;
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    grid_puts(y, 0, E.statusmsg, msglen);
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
  V.screenrows = wsrows - 2;
  if (V.screenrows < 1) V.screenrows = 1;
  V.screencols = wscols;
  return changed;
}

/* The size at startup. Unlike screen_poll_size there is no last known size to
 * fall back on, so a failed query is fatal. */
void screen_init_size(void) {
  if (get_window_size(&V.screenrows, &V.screencols) == -1)
    die("get_window_size");
  V.screenrows -= 2;
  if (V.screenrows < 1) V.screenrows = 1;
  last_wsrows = V.screenrows + 2;
  last_wscols = V.screencols;
  force_full = 1;
}

/* Release the renderer's buffers. Teardown lives here so that editor_cleanup
 * does not have to know what the renderer allocates. */
void screen_cleanup(void) {
  grid_cleanup();
}

void editor_refresh_screen(void) {
  if (screen_poll_size()) force_full = 1;
  V.gutter_w = gutter_width();
  editor_scroll();

  /* Two rows past the text area for the status and message bars. A size change
   * drops the grid's contents, so it repaints in full by itself. */
  grid_resize(V.screenrows + 2, V.screencols);
  if (force_full) {
    grid_invalidate();
    force_full = 0;
  }

  grid_begin();
  editor_draw_rows();
  editor_draw_status_bar();
  editor_draw_message_bar();
  grid_cursor_to(drow_of_cursor(&CUR) - V.drowoff,
                 (CUR.rx - V.coloff) + V.gutter_w);
  grid_flush();
}

void editor_set_status_message(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}
