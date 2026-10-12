/* lspui.c -- what a language server's answers look like in molly.
 *
 * lsp.c talks to servers and keeps what they say on the buffer; this is the
 * other half, which knows about cursors, the screen and the message bar. The
 * same split filter.c makes between running a command and editing with it,
 * and for the same reason: everything that can go wrong with a server is a
 * unit test against a fake one, and nothing here has to know there is a pipe.
 *
 * Diagnostics are drawn three ways, each answering a different question:
 *
 *   - **In the text**, underlined -- red for an error, amber for a warning,
 *     the text's own colour for the quieter two. Where is it?
 *   - **On the status bar**, as a count of errors and warnings. Is there
 *     anything wrong in this file at all, including off screen?
 *   - **On the message bar**, when the cursor is on one and nothing else is
 *     being said there. What is wrong with this?
 *
 * F2 and Shift-F2 walk them, the way F3 walks search matches and F4 walks
 * find-in-files results -- and unlike F4, round the end, because a list of
 * problems is something to be worked through until it is empty. */

#include "molly.h"

static const char *severity_word(int sev) {
  switch (sev) {
  case 1: return "error";
  case 2: return "warning";
  case 3: return "info";
  }
  return "hint";
}

unsigned char lspui_severity_role(int sev) {
  return sev == 1 ? ROLE_DIAG_ERROR : sev == 2 ? ROLE_DIAG_WARNING : ROLE_MESSAGE;
}

/* The pieces of `filerow` that diagnostics cover, as byte ranges, worst first
 * where two overlap. A problem with no width is given its character, and one
 * at or past the end of the row is given the row's last character: an
 * underline has to cover something to be seen, and "missing ';'" is reported
 * at the end of a line. An empty row has nothing to underline, and relies on
 * the status bar and the message bar. */
int lspui_row_spans(const struct buffer *b, int filerow, const erow *row,
                    struct diag_span *out, int max) {
  int i, n = 0;
  for (i = 0; i < b->ndiag && n < max; i++) {
    const struct diag *d = &b->diag[i];
    int x0, x1;
    if (d->y0 > filerow) break;          /* sorted by start */
    if (d->y1 < filerow) continue;
    x0 = (d->y0 == filerow) ? d->x0 : 0;
    x1 = (d->y1 == filerow) ? d->x1 : row->size;
    if (x0 >= row->size) {
      if (row->size == 0) continue;
      x0 = utf8_prev(row->chars, row->size);
      x1 = row->size;
    }
    if (x1 <= x0) x1 = x0 + utf8_char_bytes(row->chars, row->size, x0);
    out[n].x0 = x0;
    out[n].x1 = x1;
    out[n].severity = d->severity;
    n++;
  }
  return n;
}

/* The diagnostic the cursor is on: one whose range holds it, the worst of
 * those; failing that, the first on its row, since a row with a problem on it
 * is what the user is looking at. NULL for none. */
static const struct diag *at_cursor(const struct buffer *b, int cy, int cx) {
  const struct diag *best = NULL, *first = NULL;
  int i;
  for (i = 0; i < b->ndiag; i++) {
    const struct diag *d = &b->diag[i];
    int after_start, before_end;
    if (d->y0 > cy) break;
    if (d->y1 < cy) continue;
    if (first == NULL && d->y0 == cy) first = d;
    after_start = d->y0 < cy || cx >= d->x0;
    before_end = d->y1 > cy || cx < d->x1 || (cx == d->x0 && d->x1 == d->x0);
    if (after_start && before_end && (best == NULL || d->severity < best->severity))
      best = d;
  }
  return best != NULL ? best : first;
}

/* A message as one line: cut at its first newline, because a bar is one row
 * and clangd's second lines are notes about the first, and with any other
 * control character made a space, because a tab drawn raw is not a column. */
static void one_line(char *s) {
  s[strcspn(s, "\n")] = '\0';
  for (; *s; s++)
    if ((unsigned char)*s < 0x20 || *s == 0x7f) *s = ' ';
}

/* "error: expected ';'", for the message bar. Returns the severity, or 0 with
 * nothing written when the cursor is not on a problem. */
int lspui_cursor_message(char *dst, size_t n) {
  const struct diag *d = at_cursor(V.buf, CUR.cy, CUR.cx);
  if (d == NULL) return 0;
  snprintf(dst, n, "%s: %s", severity_word(d->severity), d->msg);
  one_line(dst);
  return d->severity;
}

/* How many of each severity the buffer has; for the status bar. */
void lspui_counts(const struct buffer *b, int *errors, int *warnings) {
  int i;
  *errors = *warnings = 0;
  for (i = 0; i < b->ndiag; i++) {
    if (b->diag[i].severity == 1) (*errors)++;
    else if (b->diag[i].severity == 2) (*warnings)++;
  }
}

/* To the next problem after the cursor (dir > 0) or the one before it, round
 * the end of the file. Returns 1 if it moved. */
int lspui_problem_jump(int dir) {
  const struct diag *to = NULL;
  int i, at = 0;

  if (B.ndiag == 0) {
    editor_set_status_message(B.lsp_srv > 0 ? "No problems."
                              : "No problems known: no language server here.");
    return 0;
  }
  if (dir > 0) {
    for (i = 0; i < B.ndiag; i++) {
      const struct diag *d = &B.diag[i];
      if (d->y0 > CUR.cy || (d->y0 == CUR.cy && d->x0 > CUR.cx)) {
        to = d;
        at = i;
        break;
      }
    }
    if (to == NULL) {
      to = &B.diag[0];
      at = 0;
    }
  } else {
    for (i = B.ndiag - 1; i >= 0; i--) {
      const struct diag *d = &B.diag[i];
      if (d->y0 < CUR.cy || (d->y0 == CUR.cy && d->x0 < CUR.cx)) {
        to = d;
        at = i;
        break;
      }
    }
    if (to == NULL) {
      to = &B.diag[B.ndiag - 1];
      at = B.ndiag - 1;
    }
  }

  cursors_collapse();
  CUR.sel_active = 0;
  CUR.cy = to->y0 < row_count() ? to->y0 : (row_count() > 0 ? row_count() - 1 : 0);
  CUR.cx = (CUR.cy < row_count() && to->x0 <= row_at(CUR.cy)->size) ? to->x0 : 0;
  {
    char msg[100];
    snprintf(msg, sizeof(msg), "%s", to->msg);
    one_line(msg);
    editor_set_status_message("%d of %d: %s: %s", at + 1, B.ndiag,
                              severity_word(to->severity), msg);
  }
  return 1;
}

void lspui_status(void) {
  char msg[160];
  lsp_describe(V.buf, msg, sizeof(msg));
  editor_set_status_message("%s", msg);
}

void lspui_restart(void) {
  lsp_stop_all();
  lsp_idle();
  editor_set_status_message(lsp_enabled() ? "Language servers restarted."
                                          : "Language servers are off.");
}
