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

static void ab_append_fill(struct abuf *ab, char c, int n) {
  if (n <= 0) return;
  ab_reserve(ab, n);
  memset(&ab->b[ab->len], c, (size_t)n);
  ab->len += n;
}


/* --- partial screen refresh: rendered rows are cached per screen line, and
 * only lines whose content actually changed are re-emitted (positioned
 * explicitly). Full redraws happen only on start, resize, Ctrl-L or after
 * the help overlay. --- */

static char **cache_lines;
static int *cache_lens;
static int *cache_caps;
static int cache_alloc;
static int force_full;
static int last_wsrows;
static int last_wscols;
static char *welcome_buf;
static int welcome_cap;

void cache_invalidate(void) {
  int i;
  for (i = 0; i < cache_alloc; i++) {
    free(cache_lines[i]);
    cache_lines[i] = NULL;
    cache_lens[i] = -1;
    cache_caps[i] = 0;
  }
}

void cache_ensure(int y) {
  int i, na;
  char **nl;
  int *nlens, *ncaps;
  if (y < cache_alloc) return;
  na = cache_alloc ? cache_alloc * 2 : 32;
  if (na <= y) na = y + 1;
  nl = realloc(cache_lines, sizeof(char *) * (size_t)na);
  nlens = realloc(cache_lens, sizeof(int) * (size_t)na);
  ncaps = realloc(cache_caps, sizeof(int) * (size_t)na);
  if (nl == NULL || nlens == NULL || ncaps == NULL) die("cache_ensure");
  cache_lines = nl;
  cache_lens = nlens;
  cache_caps = ncaps;
  for (i = cache_alloc; i < na; i++) {
    cache_lines[i] = NULL;
    cache_lens[i] = -1;
    cache_caps[i] = 0;
  }
  cache_alloc = na;
}

void cache_line_draw(struct abuf *ab, int y, const char *s, int len, int force) {
  char pos[32];
  char *nb;
  cache_ensure(y);
  if (!force && cache_lens[y] == len &&
      (len == 0 || memcmp(cache_lines[y], s, (size_t)len) == 0))
    return;
  {
    int plen = snprintf(pos, sizeof(pos), "\x1b[%d;1H", y + 1);
    ab_append(ab, pos, plen);
  }
  ab_append(ab, s, len);
  ab_append(ab, "\x1b[K", 3);
  if (cache_caps[y] < len + 1) {
    nb = realloc(cache_lines[y], (size_t)len + 1);
    if (nb == NULL) die("cache_line_draw");
    cache_lines[y] = nb;
    cache_caps[y] = len + 1;
  }
  if (len > 0) memcpy(cache_lines[y], s, (size_t)len);
  cache_lines[y][len] = '\0';
  cache_lens[y] = len;
}


/* Scratch buffer for the bytes of one drawn row (a horizontal slice, possibly
 * with a leading pad space where a wide glyph is cut by the scroll edge). */
static char *drawbuf;
static int drawbuf_cap;

static void drawbuf_ensure(int n) {
  if (drawbuf_cap >= n) return;
  drawbuf_cap = drawbuf_cap ? drawbuf_cap * 2 : 256;
  if (drawbuf_cap < n) drawbuf_cap = n;
  drawbuf = xrealloc(drawbuf, (size_t)drawbuf_cap);
}

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

/* The gutter prefix, dimmed gray (SGR 90) and reset at the end so text and
 * selections keep the terminal default. The escape sequences and the three-
 * byte UTF-8 U+2502 bar occupy no display columns, so the prefix is wider in
 * bytes than the gutter_w columns it fills; both builders return the byte
 * length written (0 when the gutter is off), and the caller tracks byte
 * offsets with that length rather than with gutter_w. */
#define GUTTER_GRAY "\x1b[90m"
#define GUTTER_SEP "\xe2\x94\x82"
#define GUTTER_RESET "\x1b[m"

/* Text row: the line number right-aligned to gutter_w-3 columns, the " │ "
 * separator, then the trailing separator space: "123 │ ". dst must hold at
 * least gutter_w + 11 bytes. */
static int gutter_number_str(int filerow, char *dst, int gutter_w) {
  if (gutter_w <= 0) return 0;
  return snprintf(dst, (size_t)gutter_w + 11,
                  GUTTER_GRAY "%*d " GUTTER_SEP " " GUTTER_RESET,
                  gutter_w - 3, filerow + 1);
}

/* Below-EOF row: a blank number field, the separator and the gray tilde:
 * "   │ ~", occupying gutter_w + 1 display columns. dst must hold at least
 * gutter_w + 12 bytes. */
static int gutter_tilde_str(char *dst, int gutter_w) {
  if (gutter_w <= 0) return 0;
  return snprintf(dst, (size_t)gutter_w + 12,
                  GUTTER_GRAY "%*s " GUTTER_SEP " ~" GUTTER_RESET,
                  gutter_w - 3, "");
}

/* Draw one text row, honoring display width: horizontal scroll (coloff) and
 * the screen width are measured in columns, so a slice starts and ends on
 * character boundaries. A wide glyph split by either edge is dropped and the
 * gap shown as a space, keeping every following column aligned.
 *
 * Characters whose display-column range overlaps [sel0, sel1) are wrapped in
 * reverse video. The escape codes occupy no display columns, so they never
 * disturb alignment, and because they live in the cached drawbuf a change of
 * selection changes the row's bytes and forces just that row to redraw. When
 * there is no selection the emitted bytes are identical to before. */
void editor_draw_text_row(struct abuf *ab, int y, erow *row,
                                 const struct drow *d, int sel0, int sel1) {
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
   * becomes that many leading spaces. */
  int left_pad = 0;
  if (i < rs && col < skip) {
    int nb, w = utf8_char_cols(r, rs, i, &nb);
    left_pad = (col + w) - skip;
    i += nb;
    col += w;
  }

  /* Emit the visible slice, toggling reverse video at the selection edges.
   * Worst case per character is one render glyph plus both escape sequences,
   * so size the buffer against the whole render length, not the visible width:
   * a run of zero-width combining marks can consume every column budget-free
   * iteration. */
  int has_sel = (sel1 > sel0);
  int gw = V.gutter_w;
  int text_cols = TEXT_COLS();
  /* Stop at the end of the slice as well as the edge of the screen. Unwrapped,
   * the slice is the whole row (DROW_ALL) and the screen is the only limit. */
  if (d->rcols < text_cols) text_cols = d->rcols;
  char gnum[32];
  int gbytes = gutter_number_str(d->filerow, gnum, gw);
  drawbuf_ensure(gbytes + left_pad + rs * 12 + 16);
  int p = 0;
  if (gbytes) {
    memcpy(drawbuf + p, gnum, (size_t)gbytes);
    p += gbytes;
  }
  while (p < gbytes + left_pad) drawbuf[p++] = ' ';

  int dcol = col;      /* absolute display column of the character at i */
  int vis = left_pad;  /* visible columns emitted so far */
  int in_sel = 0;
  while (i < rs && vis < text_cols) {
    int nb, w = utf8_char_cols(r, rs, i, &nb);
    if (vis + w > text_cols) break;  /* would overflow the right edge */
    int sel = has_sel && dcol >= sel0 && dcol < sel1;
    if (sel != in_sel) {
      if (sel) { memcpy(drawbuf + p, "\x1b[7m", 4); p += 4; }
      else { memcpy(drawbuf + p, "\x1b[27m", 5); p += 5; }
      in_sel = sel;
    }
    memcpy(drawbuf + p, r + i, (size_t)nb);
    p += nb;
    vis += w;
    dcol += w;
    i += nb;
  }
  if (in_sel) { memcpy(drawbuf + p, "\x1b[27m", 5); p += 5; }

  cache_line_draw(ab, y, drawbuf, p, 0);
}

void editor_draw_rows(struct abuf *ab) {
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
          int need = gw + pad + welcomelen + 14;
          if (welcome_cap < need) {
            char *nb = realloc(welcome_buf, (size_t)need);
            if (nb == NULL) {
              cache_line_draw(ab, y, text, welcomelen, 1);
              continue;
            }
            welcome_buf = nb;
            welcome_cap = need;
          }
          {
            int n;
            if (gw) {
              n = gutter_tilde_str(welcome_buf, gw);
            } else {
              memcpy(welcome_buf, GUTTER_GRAY "~" GUTTER_RESET, 9);
              n = 9;
            }
            int base = n;  /* bytes of prefix; the pad follows in columns */
            while (n < base + pad) welcome_buf[n++] = ' ';
            memcpy(welcome_buf + n, text, (size_t)welcomelen);
            n += welcomelen;
            cache_line_draw(ab, y, welcome_buf, n, 1);
          }
        } else {
          cache_line_draw(ab, y, text, welcomelen, 1);
        }
      } else if (gw) {
        char blank[32];
        int n = gutter_tilde_str(blank, gw);
        cache_line_draw(ab, y, blank, n, 0);
      } else {
        cache_line_draw(ab, y, GUTTER_GRAY "~" GUTTER_RESET, 9, 0);
      }
    } else {
      int s0, s1;
      selection_row_rx(filerow, &s0, &s1);
      editor_draw_text_row(ab, y, row_at(filerow), &d, s0, s1);
    }
  }
}

void editor_draw_status_bar(struct abuf *ab) {
  char rp[32];
  int rplen = snprintf(rp, sizeof(rp), "\x1b[%d;1H", V.screenrows + 1);
  ab_append(ab, rp, rplen);
  ab_append(ab, "\x1b[7m", 4);
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
  ab_append(ab, status, len);
  /* Right-align the cursor position when it fits; otherwise pad to the edge. */
  if (V.screencols - len >= rlen) {
    ab_append_fill(ab, ' ', V.screencols - len - rlen);
    ab_append(ab, rstatus, rlen);
  } else {
    ab_append_fill(ab, ' ', V.screencols - len);
  }
  ab_append(ab, "\x1b[K", 3);
  ab_append(ab, "\x1b[m", 3);
}

void editor_draw_message_bar(struct abuf *ab) {
  char rp[32];
  int rplen = snprintf(rp, sizeof(rp), "\x1b[%d;1H", V.screenrows + 2);
  ab_append(ab, rp, rplen);
  ab_append(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);
  if (msglen > V.screencols) msglen = V.screencols;
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    ab_append(ab, E.statusmsg, msglen);
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
  cache_invalidate();
  free(cache_lines);
  free(cache_lens);
  free(cache_caps);
  cache_lines = NULL;
  cache_lens = NULL;
  cache_caps = NULL;
  cache_alloc = 0;
  free(welcome_buf);
  welcome_buf = NULL;
  welcome_cap = 0;
  free(drawbuf);
  drawbuf = NULL;
  drawbuf_cap = 0;
}

void editor_refresh_screen(void) {
  if (screen_poll_size()) force_full = 1;
  V.gutter_w = gutter_width();
  editor_scroll();

  struct abuf ab = ABUF_INIT;
  ab_append(&ab, "\x1b[?25l", 6);
  if (force_full) {
    cache_invalidate();
    ab_append(&ab, "\x1b[2J", 4);
    ab_append(&ab, "\x1b[H", 3);
    force_full = 0;
  } else {
    /* The status bar and message bar live just past the last text row, so they
     * are invalidated here rather than through cache_line_draw. Nothing above
     * has grown the cache that far: the draw path below only ever reaches
     * cache_ensure(y) for y < screenrows, and cache_ensure doubles from 32, so
     * in a terminal exactly 32 (or 64, 128, ...) text rows tall -- or one row
     * short of one -- these two slots were being written past the end of all
     * three arrays. Ensure the capacity before indexing them. */
    cache_ensure(V.screenrows + 1);
    cache_lines[V.screenrows] = NULL;
    cache_lens[V.screenrows] = -1;
    cache_caps[V.screenrows] = 0;
    cache_lines[V.screenrows + 1] = NULL;
    cache_lens[V.screenrows + 1] = -1;
    cache_caps[V.screenrows + 1] = 0;
  }

  editor_draw_rows(&ab);
  editor_draw_status_bar(&ab);
  editor_draw_message_bar(&ab);

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
      (drow_of_cursor(&CUR) - V.drowoff) + 1,
      (CUR.rx - V.coloff) + 1 + V.gutter_w);
  ab_append(&ab, buf, strlen(buf));

  ab_append(&ab, "\x1b[?25h", 6);
  write_all(STDOUT_FILENO, ab.b, (size_t)ab.len);
  free(ab.b);
}

void editor_set_status_message(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}
