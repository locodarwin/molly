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

/* ======================================================================== *
 *  Questions: where is this defined, and what is it.
 *
 *  An answer arrives whenever the server sends it, which is inside the event
 *  loop -- possibly while a menu or a dialog is open, and possibly after the
 *  user has moved on. So an answer is not acted on where it arrives. It is
 *  put in a mailbox, the main loop opens the mailbox between keys, and what
 *  is in it is acted on only if the buffer, its text and the cursor are all
 *  where they were when the question was asked. Anything else, the user has
 *  moved on, and a jump or a popup appearing out of nowhere would be worse
 *  than the answer never coming.
 * ======================================================================== */

static struct {
  int kind;               /* LSP_REQ_*, 0 for empty */
  struct buffer *buf;
  unsigned edits;
  int cy, cx;
  int utf8;

  char *path;             /* definition: the file */
  int line, ch;           /* and the position in it, in the server's units */

  char *text;             /* hover: what to show */

  struct fchange *files;  /* rename: what to change, a file at a time */
  int nfiles;
  char *refuse;           /* rename: why it will not be done, when it will not */

  struct citem *items;    /* completion: what could go here */
  int nitems;
  int incomplete;         /* the server would say more given more to go on */
  int trigger;            /* asked because this was typed, not Alt-/ */
} box;

/* One edit a rename asks for: a range, in the server's units until it is
 * applied, and what goes there. */
struct fedit {
  int sl, sc, el, ec;
  char *text;
};

/* A file's worth of them. */
struct fchange {
  char *path;
  struct fedit *e;
  int n;
};

static void files_free(struct fchange *f, int n) {
  int i, k;
  for (i = 0; i < n; i++) {
    for (k = 0; k < f[i].n; k++) free(f[i].e[k].text);
    free(f[i].e);
    free(f[i].path);
  }
  free(f);
}

/* One completion. `start` is where its text replaces from, in the server's
 * units until it is delivered and then in bytes; -1 means "the word before the
 * cursor", which is what an item with no textEdit replaces. */
struct citem {
  char *label;
  char *insert;
  char *filter;
  char *sort;
  char *detail;
  int start;
};

static void items_free(struct citem *it, int n) {
  int i;
  for (i = 0; i < n; i++) {
    free(it[i].label);
    free(it[i].insert);
    free(it[i].filter);
    free(it[i].sort);
    free(it[i].detail);
  }
  free(it);
}

static void box_clear(void) {
  free(box.path);
  free(box.text);
  items_free(box.items, box.nitems);
  files_free(box.files, box.nfiles);
  free(box.refuse);
  memset(&box, 0, sizeof(box));
}

/* A buffer is going away: anything in the mailbox about it goes too. */
static void comp_close(void);
static struct buffer *comp_buf;

void lspui_forget(struct buffer *b) {
  if (box.buf == b) box_clear();
  if (comp_buf == b) comp_close();
}

/* ---- reading an answer --------------------------------------------------- */

/* The first place a definition answer names. It may be a Location, an array
 * of Locations, or an array of LocationLinks, which name the target with
 * different keys; an empty array or null is "nowhere". */
static int read_location(const struct json *r, char **path, int *line, int *ch) {
  const struct json *range;
  const char *uri;
  if (r != NULL && r->type == JSON_ARRAY) r = json_at(r, 0);
  if (r == NULL || r->type != JSON_OBJECT) return 0;
  uri = json_str(json_get(r, "uri"));
  range = json_get(r, "range");
  if (uri == NULL) {
    uri = json_str(json_get(r, "targetUri"));
    range = json_get(r, "targetSelectionRange");
    if (range == NULL) range = json_get(r, "targetRange");
  }
  *path = lsp_uri_to_path(uri);
  if (*path == NULL) return -1;
  *line = json_int(json_get(json_get(range, "start"), "line"), 0);
  *ch = json_int(json_get(json_get(range, "start"), "character"), 0);
  return 1;
}

/* Hover contents, as text. MarkupContent ({kind, value}), a MarkedString
 * (a string, or {language, value}), or an array of MarkedStrings -- three
 * shapes from three generations of the protocol, all still sent. */
static void read_marked(const struct json *v, struct abuf *ab) {
  const char *s;
  if (v == NULL) return;
  if (v->type == JSON_ARRAY) {
    const struct json *k;
    for (k = v->kid; k != NULL; k = k->next) {
      if (ab->len > 0) ab_append(ab, "\n\n", 2);
      read_marked(k, ab);
    }
    return;
  }
  s = v->type == JSON_STRING ? v->str : json_str(json_get(v, "value"));
  if (s != NULL) ab_append(ab, s, (int)strlen(s));
}

static char *dup_or_null(const char *s) { return s != NULL ? xstrdup(s) : NULL; }

/* A completion answer: a CompletionList, or a bare array of items, or null.
 * An item's text is its textEdit's, or its insertText, or its label -- in that
 * order, which is the protocol's -- and the edit's start is kept in the
 * server's units, because only the row it is about can convert it. */
static void read_completions(const struct json *r) {
  const struct json *list = r, *k;
  int n = 0;
  if (r != NULL && r->type == JSON_OBJECT) {
    box.incomplete = json_bool(json_get(r, "isIncomplete"), 0);
    list = json_get(r, "items");
  }
  if (list == NULL || list->type != JSON_ARRAY || list->n == 0) return;
  box.items = xmalloc(sizeof(*box.items) * (size_t)list->n);
  for (k = list->kid; k != NULL; k = k->next) {
    const char *label = json_str(json_get(k, "label"));
    const struct json *edit = json_get(k, "textEdit"), *range;
    struct citem *it = &box.items[n];
    if (label == NULL) continue;
    it->label = xstrdup(label);
    it->filter = dup_or_null(json_str(json_get(k, "filterText")));
    it->sort = dup_or_null(json_str(json_get(k, "sortText")));
    it->detail = dup_or_null(json_str(json_get(k, "detail")));
    it->insert = dup_or_null(json_str(json_get(k, "insertText")));
    it->start = -1;
    /* A TextEdit has a range; an InsertReplaceEdit has two, and the insert
     * one is what replaces up to the cursor. */
    range = json_get(edit, "range");
    if (range == NULL) range = json_get(edit, "insert");
    if (edit != NULL && json_str(json_get(edit, "newText")) != NULL && range != NULL) {
      free(it->insert);
      it->insert = xstrdup(json_str(json_get(edit, "newText")));
      it->start = json_int(json_get(json_get(range, "start"), "character"), -1);
    }
    n++;
  }
  box.nitems = n;
}

/* A list of TextEdits for one file. A file named twice -- the protocol does
 * not forbid it -- gets both lists. */
static void add_edits(const char *uri, const struct json *list) {
  const struct json *k;
  struct fchange *f;
  char *path = lsp_uri_to_path(uri);
  int i;
  if (path == NULL) {
    if (box.refuse == NULL) box.refuse = xstrdup("This rename reaches a file molly cannot open.");
    return;
  }
  for (i = 0; i < box.nfiles; i++)
    if (strcmp(box.files[i].path, path) == 0) break;
  if (i == box.nfiles) {
    box.files = xrealloc(box.files, sizeof(*box.files) * (size_t)(box.nfiles + 1));
    memset(&box.files[i], 0, sizeof(box.files[i]));
    box.files[i].path = path;
    box.nfiles++;
  } else {
    free(path);
  }
  f = &box.files[i];
  if (list == NULL || list->type != JSON_ARRAY) return;
  f->e = xrealloc(f->e, sizeof(*f->e) * (size_t)(f->n + list->n));
  for (k = list->kid; k != NULL; k = k->next) {
    const struct json *r = json_get(k, "range");
    const char *t = json_str(json_get(k, "newText"));
    struct fedit *e = &f->e[f->n];
    if (r == NULL || t == NULL) continue;
    e->sl = json_int(json_get(json_get(r, "start"), "line"), 0);
    e->sc = json_int(json_get(json_get(r, "start"), "character"), 0);
    e->el = json_int(json_get(json_get(r, "end"), "line"), 0);
    e->ec = json_int(json_get(json_get(r, "end"), "character"), 0);
    e->text = xstrdup(t);
    f->n++;
  }
}

/* A WorkspaceEdit: `documentChanges`, a list of edits per document that may
 * also hold operations on files, or the older `changes`, an object of edit
 * lists by URI. A rename that would create, move or delete a file is refused
 * whole: half a rename is worse than none, and molly does not move files. */
static void read_workspace_edit(const struct json *we) {
  const struct json *dc = json_get(we, "documentChanges"), *ch, *k;
  if (dc != NULL && dc->type == JSON_ARRAY) {
    for (k = dc->kid; k != NULL; k = k->next) {
      if (json_str(json_get(k, "kind")) != NULL) {
        free(box.refuse);
        box.refuse = xstrdup("This rename would create, move or delete files, "
                             "which molly does not do.");
        return;
      }
      add_edits(json_str(json_get(json_get(k, "textDocument"), "uri")),
                json_get(k, "edits"));
    }
    return;
  }
  ch = json_get(we, "changes");
  if (ch != NULL && ch->type == JSON_OBJECT)
    for (k = ch->kid; k != NULL; k = k->next) add_edits(k->key, k);
}

/* Called by lsp.c with an answer; `result` is NULL for an error. */
void lsp_answer(int kind, struct buffer *b, unsigned edits, int cy, int cx,
                const struct json *result, int utf8) {
  box_clear();
  box.kind = kind;
  box.buf = b;
  box.edits = edits;
  box.cy = cy;
  box.cx = cx;
  box.utf8 = utf8;

  if (kind == LSP_REQ_DEFINITION) {
    int got = read_location(result, &box.path, &box.line, &box.ch);
    if (got < 0) box.text = xstrdup("The definition is not in a file molly can open.");
    else if (got == 0) box.text = xstrdup("No definition found.");
  } else if (kind == LSP_REQ_HOVER) {
    struct abuf ab = ABUF_INIT;
    read_marked(json_get(result, "contents"), &ab);
    ab_append(&ab, "", 1);
    box.text = ab.b;
  } else if (kind == LSP_REQ_COMPLETION) {
    read_completions(result);
  } else if (kind == LSP_REQ_RENAME) {
    if (result != NULL && result->type == JSON_OBJECT) read_workspace_edit(result);
    else box.refuse = xstrdup("The language server cannot rename that.");
  }
  ui_request_redraw();
}

/* ---- going there and back ------------------------------------------------ */

#define JUMPS_MAX 32

/* Where F12 jumped from, most recent last. A buffer by its id rather than by
 * pointer, because a buffer can be closed between the jump and the way back
 * and an id is safe to look for afterwards. */
static struct { int buf_id, cy, cx; } jumps[JUMPS_MAX];
static int njumps;

static void jump_push(void) {
  if (njumps == JUMPS_MAX) {
    memmove(&jumps[0], &jumps[1], sizeof(jumps[0]) * (JUMPS_MAX - 1));
    njumps--;
  }
  jumps[njumps].buf_id = B.id;
  jumps[njumps].cy = CUR.cy;
  jumps[njumps].cx = CUR.cx;
  njumps++;
}

static void cursor_to(int cy, int cx) {
  cursors_collapse();
  CUR.sel_active = 0;
  if (cy >= row_count()) cy = row_count() > 0 ? row_count() - 1 : 0;
  if (cy < 0) cy = 0;
  CUR.cy = cy;
  if (cy >= row_count()) cx = 0;
  else if (cx > row_at(cy)->size) cx = row_at(cy)->size;
  CUR.cx = cx < 0 ? 0 : cx;
}

int lspui_can_back(void) { return njumps > 0; }

int lspui_back(void) {
  while (njumps > 0) {
    int i;
    njumps--;
    for (i = 0; i < buf_count(); i++) {
      if (buf_at(i)->id != jumps[njumps].buf_id) continue;
      if (V.buf != buf_at(i)) buf_show(i);
      cursor_to(jumps[njumps].cy, jumps[njumps].cx);
      return 1;
    }
  }
  editor_set_status_message("Nowhere to go back to.");
  return 0;
}

/* The open buffer on this file, by what the name resolves to: the server says
 * /home/me/proj/src/a.c where the user typed src/a.c. -1 for none. */
static int buffer_on(const char *path) {
  int i;
  for (i = 0; i < buf_count(); i++) {
    const char *name = buf_at(i)->filename;
    char *real;
    int same;
    if (name == NULL) continue;
    real = realpath(name, NULL);
    same = real != NULL && strcmp(real, path) == 0;
    free(real);
    if (same) return i;
  }
  return -1;
}

static void deliver_definition(void) {
  int at = buffer_on(box.path), y, x;
  char cwd[4096];
  size_t n;

  jump_push();
  if (at >= 0) {
    if (V.buf != buf_at(at)) buf_show(at);
  } else {
    /* Under the working directory, by the name the user would have typed; so
     * the tab says src/a.c and not the whole path. */
    const char *name = box.path;
    if (getcwd(cwd, sizeof(cwd)) != NULL && (n = strlen(cwd)) > 1 &&
        strncmp(box.path, cwd, n) == 0 && box.path[n] == '/')
      name = box.path + n + 1;
    editor_open_in_new_buffer(xstrdup(name));
  }
  y = box.line;
  x = 0;
  if (y >= 0 && y < row_count()) {
    erow *r = row_at(y);
    x = lsp_units_to_col(r->chars, r->size, box.ch, box.utf8);
  }
  cursor_to(y, x);
  editor_set_status_message("Definition: %s:%d", buf_name(V.buf), CUR.cy + 1);
}

/* ---- renaming ------------------------------------------------------------ */

static char rename_to[128];   /* the name asked for, for what is said after */

/* Last first: applying an edit moves the text after it, so the edits are
 * applied from the end of the file back, and every one still to come is
 * where the server said. */
static int fedit_cmp(const void *a, const void *b) {
  const struct fedit *p = a, *q = b;
  if (p->sl != q->sl) return q->sl < p->sl ? -1 : 1;
  if (p->sc != q->sc) return q->sc < p->sc ? -1 : 1;
  return 0;
}

/* A position from the server in the current buffer, in bytes: a row past
 * the end is the end of the last row. */
static void edit_pos(int line, int ch, int utf8, int *y, int *x) {
  erow *r;
  if (row_count() == 0) {
    *y = *x = 0;
    return;
  }
  if (line >= row_count()) {
    *y = row_count() - 1;
    *x = row_at(*y)->size;
    return;
  }
  if (line < 0) line = 0;
  r = row_at(line);
  *y = line;
  *x = lsp_units_to_col(r->chars, r->size, ch, utf8);
}

/* One file's edits, into the current buffer, as one undo step. `track` is a
 * column on row `track_y` to carry along: where the cursor was, moved by the
 * edits before it on its row, or to the start of the one it was inside. */
static int apply_edits(struct fchange *f, int utf8, int track_y, int *track) {
  int k, n = 0;
  qsort(f->e, (size_t)f->n, sizeof(*f->e), fedit_cmp);
  cursors_collapse();
  B.undo.locked = 1;
  undo_group_begin();
  for (k = 0; k < f->n; k++) {
    struct fedit *e = &f->e[k];
    int sy, sx, ey, ex, len = (int)strlen(e->text);
    edit_pos(e->sl, e->sc, utf8, &sy, &sx);
    edit_pos(e->el, e->ec, utf8, &ey, &ex);
    if (ey < sy || (ey == sy && ex < sx)) continue;
    if (track != NULL && sy == track_y && ey == sy && strchr(e->text, '\n') == NULL) {
      if (ex <= *track) *track += len - (ex - sx);
      else if (sx <= *track) *track = sx;
    }
    CUR.sy = sy;
    CUR.sx = sx;
    CUR.cy = ey;
    CUR.cx = ex;
    CUR.sel_active = (sy != ey || sx != ex);
    editor_insert_text(e->text, len);
    n++;
  }
  undo_group_end();
  CUR.sel_active = 0;
  return n;
}

/* A rename's edits, a file at a time: an open file in its buffer, and one
 * that is not opened into one and left there unsaved -- a rename that wrote
 * files behind the user's back would be the one edit molly made that undo
 * could not take back. The user is put back where they were. */
static void deliver_rename(void) {
  struct buffer *home = V.buf;
  int hy = CUR.cy, hx = CUR.cx, i, changes = 0, others = 0, opened = 0;
  int home_changes = 0;

  if (box.refuse != NULL) {
    editor_set_status_message("%s", box.refuse);
    return;
  }
  for (i = 0; i < box.nfiles; i++) changes += box.files[i].n;
  if (changes == 0) {
    editor_set_status_message("Nothing to rename there.");
    return;
  }
  changes = 0;
  for (i = 0; i < box.nfiles; i++) {
    struct fchange *f = &box.files[i];
    int at = buffer_on(f->path);
    if (f->n == 0) continue;
    if (at >= 0) {
      if (V.buf != buf_at(at)) buf_show(at);
    } else {
      char cwd[4096];
      size_t n;
      const char *name = f->path;
      if (getcwd(cwd, sizeof(cwd)) != NULL && (n = strlen(cwd)) > 1 &&
          strncmp(f->path, cwd, n) == 0 && f->path[n] == '/')
        name = f->path + n + 1;
      editor_open_in_new_buffer(xstrdup(name));
      opened++;
    }
    {
      int n = apply_edits(f, box.utf8, V.buf == home ? hy : -1,
                          V.buf == home ? &hx : NULL);
      changes += n;
      if (V.buf == home) home_changes += n;
    }
    if (V.buf != home) others++;
  }
  if (V.buf != home) buf_show(buf_index_of(home));
  cursor_to(hy, hx);
  /* The other files are what needs saying: they changed where the user is
   * not looking, and are not saved. Their tabs say which, with a `*`. */
  (void)opened;
  if (others > 0)
    editor_set_status_message("Renamed to %s: %d change%s, %d in %d other file%s, unsaved.",
                              rename_to, changes, changes == 1 ? "" : "s",
                              changes - home_changes, others, others == 1 ? "" : "s");
  else
    editor_set_status_message("Renamed to %s: %d change%s.", rename_to, changes,
                              changes == 1 ? "" : "s");
}

/* Alt-N: ask for the new name of what is under the cursor, then ask the
 * server what that means everywhere. */
int lspui_rename(void) {
  char prompt[200], word[64];
  char *name;
  int a, b;
  erow *r;

  if (!lsp_can(V.buf, LSP_REQ_RENAME)) {
    char why[160];
    if (V.buf->lsp_srv > 0)
      editor_set_status_message("This file's language server cannot rename things.");
    else {
      lsp_describe(V.buf, why, sizeof(why));
      editor_set_status_message("%s", why);
    }
    return 0;
  }
  word[0] = '\0';
  if (CUR.cy < row_count()) {
    r = row_at(CUR.cy);
    a = b = CUR.cx;
    while (a > 0 && (isalnum((unsigned char)r->chars[a - 1]) || r->chars[a - 1] == '_' ||
                     (unsigned char)r->chars[a - 1] >= 0x80)) a--;
    while (b < r->size && (isalnum((unsigned char)r->chars[b]) || r->chars[b] == '_' ||
                           (unsigned char)r->chars[b] >= 0x80)) b++;
    if (b - a > 0 && b - a < (int)sizeof(word)) {
      memcpy(word, r->chars + a, (size_t)(b - a));
      word[b - a] = '\0';
    }
  }
  if (word[0] == '\0') {
    editor_set_status_message("Put the cursor on a name to rename it.");
    return 0;
  }
  snprintf(prompt, sizeof(prompt), "Rename %s to: %%s (ESC to cancel)", word);
  name = editor_prompt(prompt, NULL);
  if (name == NULL) return 0;
  if (name[0] == '\0' || strcmp(name, word) == 0) {
    free(name);
    return 0;
  }
  snprintf(rename_to, sizeof(rename_to), "%s", name);
  if (!lsp_ask(V.buf, LSP_REQ_RENAME, CUR.cy, CUR.cx, 0, name))
    editor_set_status_message("The language server is not ready yet.");
  free(name);
  return 0;
}

/* ---- the hover popup ----------------------------------------------------- */

#define POPUP_MAXW 72
#define POPUP_MAXH 14

/* The popup's text, a line per entry, already wrapped to its width. */
static char **pop_lines;
static int pop_n, pop_w;
static int pop_layer = -1;

static void popup_free(void) {
  int i;
  for (i = 0; i < pop_n; i++) free(pop_lines[i]);
  free(pop_lines);
  pop_lines = NULL;
  pop_n = 0;
}

static void pop_add(const char *s, int len) {
  pop_lines = xrealloc(pop_lines, sizeof(*pop_lines) * (size_t)(pop_n + 1));
  pop_lines[pop_n] = xmalloc((size_t)len + 1);
  memcpy(pop_lines[pop_n], s, (size_t)len);
  pop_lines[pop_n][len] = '\0';
  pop_n++;
}

/* Markdown, read for a terminal. Code fences go (their contents stay, as
 * the code they are), as do heading marks, `**` and backticks, and a `---`
 * rule becomes a blank line. Not a Markdown renderer: what servers put in a
 * hover is a signature and a paragraph, and this is what it takes to read
 * those without the punctuation. A line is cut to the popup's width at a
 * space where it can be, and anywhere where it cannot. */
static void popup_set(const char *md, int width) {
  const char *p = md;
  int blank = 1;

  popup_free();
  while (*p != '\0') {
    const char *eol = strchr(p, '\n');
    size_t len = eol != NULL ? (size_t)(eol - p) : strlen(p);
    char *line = xmalloc(len + 1), *o = line;
    const char *q = p, *end = p + len;
    int cols;

    if (len >= 3 && strncmp(p, "```", 3) == 0) {
      free(line);
      p = eol != NULL ? eol + 1 : end;
      continue;
    }
    if (len == 3 && strncmp(p, "---", 3) == 0) q = end;
    while (q < end && *q == '#') q++;
    if (q > p && q < end && *q == ' ' && *p == '#') q++;
    for (; q < end; q++) {
      if (*q == '`') continue;
      if (*q == '*' && q + 1 < end && q[1] == '*') {
        q++;
        continue;
      }
      if (*q == '\\' && q + 1 < end && ispunct((unsigned char)q[1])) q++;
      *o++ = (unsigned char)*q < 0x20 ? ' ' : *q;
    }
    *o = '\0';
    while (o > line && o[-1] == ' ') *--o = '\0';

    /* Runs of blank lines are one, and none at the start. */
    if (line[0] == '\0') {
      if (!blank) pop_add("", 0);
      blank = 1;
    } else {
      const char *s = line;
      blank = 0;
      while (*s != '\0') {
        int take = utf8_fit_cols(s, width, &cols), cut = take;
        if (s[take] != '\0') {
          int k;
          for (k = take; k > 0 && s[k] != ' '; k--) {}
          if (k > 0) cut = k;
        }
        if (cut == 0) cut = utf8_char_bytes(s, (int)strlen(s), 0);
        pop_add(s, cut);
        s += cut;
        while (*s == ' ') s++;
      }
    }
    free(line);
    p = eol != NULL ? eol + 1 : end;
  }
  while (pop_n > 0 && pop_lines[pop_n - 1][0] == '\0') free(pop_lines[--pop_n]);

  pop_w = 0;
  {
    int i;
    for (i = 0; i < pop_n; i++) {
      int c = utf8_str_cols(pop_lines[i]);
      if (c > pop_w) pop_w = c;
    }
  }
}

#define POP_TL "\xe2\x94\x8c"
#define POP_TR "\xe2\x94\x90"
#define POP_BL "\xe2\x94\x94"
#define POP_BR "\xe2\x94\x98"
#define POP_H  "\xe2\x94\x80"
#define POP_V  "\xe2\x94\x82"

/* A single-line box in the menu's colours: a popup is the same kind of thing
 * as a drop-down, a panel that is there until the next key. */
void lspui_box(int x, int y, int w, int h) {
  int i;
  grid_role(ROLE_MENU);
  for (i = 0; i < h; i++) grid_fill(y + i, x, w, ' ');
  grid_role(ROLE_MENU_FRAME);
  grid_puts(y, x, POP_TL, 3);
  grid_puts(y, x + w - 1, POP_TR, 3);
  grid_puts(y + h - 1, x, POP_BL, 3);
  grid_puts(y + h - 1, x + w - 1, POP_BR, 3);
  for (i = 1; i < w - 1; i++) {
    grid_puts(y, x + i, POP_H, 3);
    grid_puts(y + h - 1, x + i, POP_H, 3);
  }
  for (i = 1; i < h - 1; i++) {
    grid_puts(y + i, x, POP_V, 3);
    grid_puts(y + i, x + w - 1, POP_V, 3);
  }
}

/* Where a popup of w x h goes: under the cursor's row if it fits there, over
 * it if it fits there instead, and in whichever has more room if neither --
 * in which case it is cut short. Left-aligned on the cursor, and moved left as
 * far as it has to be to stay on the screen. Worked out from the cursor every
 * frame, so a resize cannot strand it. Returns the rows it may have. */
int lspui_place(int w, int h, int *px, int *py) {
  int cy = V.screeny + drow_of_cursor(&CUR) - V.drowoff;
  int cx = V.screenx + (drow_cursor_col(&CUR) - V.coloff) + V.gutter_w;
  int top = V.screeny, bottom = V.screeny + V.screenrows;  /* the text area */
  int below = bottom - (cy + 1), above = cy - top;
  int cols = grid_cols();

  if (h <= below || below >= above) {
    if (h > below) h = below;
    *py = cy + 1;
  } else {
    if (h > above) h = above;
    *py = cy - h;
  }
  if (cx + w > cols) cx = cols - w;
  if (cx < 0) cx = 0;
  *px = cx;
  return h;
}

static void hover_draw(const struct layer *self) {
  int w = pop_w + 4, h = pop_n + 2, x, y, i;
  (void)self;
  if (w > grid_cols()) w = grid_cols();
  h = lspui_place(w, h > POPUP_MAXH ? POPUP_MAXH : h, &x, &y);
  if (h < 3 || w < 5) return;
  lspui_box(x, y, w, h);
  grid_role(ROLE_MENU);
  for (i = 0; i < h - 2 && i < pop_n; i++) {
    int cols;
    int take = utf8_fit_cols(pop_lines[i], w - 4, &cols);
    /* The last row that fits says there is more, when there is -- unless it
     * is the only row, which is better spent on the first line. */
    if (i == h - 3 && pop_n > h - 2 && h > 3)
      grid_puts(y + 1 + i, x + 2, "\xe2\x80\xa6", 3);
    else grid_puts(y + 1 + i, x + 2, pop_lines[i], take);
  }
}

static void comp_close(void);

static void hover_open(const char *text) {
  struct layer l;
  comp_close();
  int width = POPUP_MAXW - 4;
  if (grid_cols() - 4 < width) width = grid_cols() - 4;
  if (width < 8) width = 8;
  popup_set(text, width);
  if (pop_n == 0) {
    editor_set_status_message("Nothing to show here.");
    return;
  }
  memset(&l, 0, sizeof(l));
  l.draw = hover_draw;
  /* The whole grid, because a layer with no area is not drawn and where the
   * box goes is worked out from the cursor as it is drawn. It never sees a
   * click: the click closes it before it is dispatched (lspui_key). */
  l.w = grid_cols();
  l.h = grid_rows();
  pop_layer = layer_push(&l);
}

static void hover_close(void) {
  if (pop_layer < 0) return;
  layer_pop_to(pop_layer);
  pop_layer = -1;
  popup_free();
}

/* ---- the completion popup ------------------------------------------------ */

#define COMP_ROWS 10   /* items shown at once */
#define COMP_MAXW 60

/* What is on offer, and which of it the word typed so far still matches.
 * `items` is everything the server sent; `shown` indexes the matching ones in
 * the order they are listed. Filtered here rather than by asking again,
 * because the server's answer is for the word as it was and a keystroke is
 * not worth a round trip -- unless the server said its list was incomplete. */
static struct citem *items;
static int nitems, incomplete;
static int *shown;
static int nshown, csel, ctop;
static int comp_layer = -1;
static int comp_cy, comp_start;   /* the word being completed starts here */
static int last_trigger;          /* what the last question was asked for */

/* Display columns of `n` bytes, which need not end in a NUL. */
static int utf8_str_cols_n(const char *s, int n) {
  int i = 0, cols = 0;
  while (i < n) {
    int nb;
    cols += utf8_char_cols(s, n, i, &nb);
    i += nb;
  }
  return cols;
}

static int ident_byte(unsigned char c) {
  return isalnum(c) || c == '_' || c >= 0x80;
}

/* Where the word before the cursor starts. */
static int word_start(void) {
  int x = CUR.cx;
  erow *r;
  if (CUR.cy >= row_count()) return x;
  r = row_at(CUR.cy);
  while (x > 0 && ident_byte((unsigned char)r->chars[x - 1])) x--;
  return x;
}

static void comp_close(void) {
  if (comp_layer >= 0) layer_pop_to(comp_layer);
  comp_layer = -1;
  items_free(items, nitems);
  items = NULL;
  nitems = 0;
  free(shown);
  shown = NULL;
  nshown = 0;
  comp_buf = NULL;
}

static int comp_open_p(void) { return comp_layer >= 0; }

static const char *item_key(const struct citem *it) {
  return it->filter != NULL ? it->filter : it->label;
}

/* The order the matches are listed in: the better match first, and the
 * server's own order (sortText, then the label) between equals. */
static const char *sort_prefix;
static int item_cmp(const void *a, const void *b) {
  const struct citem *p = &items[*(const int *)a], *q = &items[*(const int *)b];
  int sp = palette_score(item_key(p), sort_prefix);
  int sq = palette_score(item_key(q), sort_prefix);
  int c;
  if (sp != sq) return sq - sp;
  c = strcmp(p->sort != NULL ? p->sort : p->label, q->sort != NULL ? q->sort : q->label);
  return c != 0 ? c : strcmp(p->label, q->label);
}

/* Match the items against the word as it is now. Returns how many match. */
static int comp_filter(void) {
  char prefix[256];
  int i, len = CUR.cx - comp_start;
  erow *r = row_at(comp_cy);

  if (len < 0) len = 0;
  if (len >= (int)sizeof(prefix)) len = (int)sizeof(prefix) - 1;
  memcpy(prefix, r->chars + comp_start, (size_t)len);
  prefix[len] = '\0';

  free(shown);
  shown = xmalloc(sizeof(*shown) * (size_t)(nitems > 0 ? nitems : 1));
  nshown = 0;
  for (i = 0; i < nitems; i++) {
    /* The word itself, typed out in full, is not something to offer. */
    if (len > 0 && strcmp(items[i].label, prefix) == 0) continue;
    if (palette_score(item_key(&items[i]), prefix) > 0) shown[nshown++] = i;
  }
  sort_prefix = prefix;
  qsort(shown, (size_t)nshown, sizeof(*shown), item_cmp);
  csel = 0;
  ctop = 0;
  return nshown;
}

static void comp_draw(const struct layer *self) {
  int i, w = 12, h, x, y, back;
  int cx = V.screenx + (drow_cursor_col(&CUR) - V.coloff) + V.gutter_w;
  (void)self;

  for (i = 0; i < nshown; i++) {
    const struct citem *it = &items[shown[i]];
    int c = utf8_str_cols(it->label) + 4;
    if (it->detail != NULL) c += utf8_str_cols(it->detail) + 2;
    if (c > w) w = c;
  }
  if (w > COMP_MAXW) w = COMP_MAXW;
  if (w > grid_cols()) w = grid_cols();
  h = (nshown < COMP_ROWS ? nshown : COMP_ROWS) + 2;
  /* Lined up with the word rather than the cursor: the box's text column is
   * the word's first, which is where what is chosen will start. */
  back = utf8_str_cols_n(row_at(comp_cy)->chars + comp_start, CUR.cx - comp_start);
  h = lspui_place(w, h, &x, &y);
  /* As near as the screen's edges allow: a word in the first column cannot
   * have a box two columns to its left. */
  x = cx - back - 2;
  if (x + w > grid_cols()) x = grid_cols() - w;
  if (x < 0) x = 0;
  if (h < 3) return;

  if (csel < ctop) ctop = csel;
  if (csel >= ctop + h - 2) ctop = csel - (h - 3);
  lspui_box(x, y, w, h);
  for (i = 0; i < h - 2 && ctop + i < nshown; i++) {
    const struct citem *it = &items[shown[ctop + i]];
    int row = y + 1 + i, cols, used, room = w - 4;
    int take = utf8_fit_cols(it->label, room, &used);
    grid_role(ctop + i == csel ? ROLE_MENU_SELECTED : ROLE_MENU);
    grid_fill(row, x + 1, w - 2, ' ');
    grid_puts(row, x + 2, it->label, take);
    /* The detail -- a type, a signature -- right-aligned in what is left,
     * two columns clear of the label, and cut short where the box is. */
    if (it->detail != NULL && room - used >= 3) {
      int dtake = utf8_fit_cols(it->detail, room - used - 2, &cols);
      grid_role(ctop + i == csel ? ROLE_MENU_SELECTED : ROLE_MENU_DISABLED);
      grid_puts(row, x + 2 + room - cols, it->detail, dtake);
    }
  }
}

/* Put the chosen item in, over the word typed so far: one undo step, and the
 * same path a paste takes, so it lands as text arriving and not as Enter. */
static void comp_accept(void) {
  const struct citem *it;
  const char *text;
  int from;
  if (csel < 0 || csel >= nshown) {
    comp_close();
    return;
  }
  it = &items[shown[csel]];
  text = it->insert != NULL ? it->insert : it->label;
  from = (it->start >= 0 && it->start <= CUR.cx) ? it->start : comp_start;
  {
    char *copy = xstrdup(text);
    comp_close();
    /* The word typed so far is its own undo step, closed here so the
     * completion cannot coalesce into it; the completion -- the word out,
     * the choice in -- is one more. Ctrl-Z then takes back the choice and
     * leaves what was typed, which is what it was before Alt-/. */
    B.undo.locked = 1;
    undo_group_begin();
    if (from < CUR.cx) {
      CUR.sx = from;
      CUR.sy = CUR.cy;
      CUR.sel_active = 1;
    }
    editor_insert_text(copy, (int)strlen(copy));
    undo_group_end();
    free(copy);
  }
}

static void comp_show(void) {
  struct layer l;
  if (comp_layer >= 0) return;
  memset(&l, 0, sizeof(l));
  l.draw = comp_draw;
  l.w = grid_cols();
  l.h = grid_rows();
  comp_layer = layer_push(&l);
}

/* ---- keys ---------------------------------------------------------------- */

static int refilter_pending;

/* A key arrived. A hover is there until the next key, which closes it and
 * then does what it does -- except Esc, which only closes it. Not a resize,
 * and not the redraw an answer asks for, both of which arrive as WINCH_KEY.
 *
 * A completion list keeps the keys that mean something to a list -- the
 * arrows, the page keys, Enter and Tab to take, Esc to leave -- and lets the
 * ones that edit the word through, to be filtered on afterwards in
 * lspui_after_key(). Anything else closes it and does what it does. Returns 1
 * when the key has been used up. */
int lspui_key(int c) {
  int mods = input_last_key_had_shift() || input_last_key_had_ctrl() ||
             input_last_key_had_alt();
  if (c == WINCH_KEY) return 0;
  if (pop_layer >= 0) {
    hover_close();
    return c == '\x1b';
  }
  if (!comp_open_p()) return 0;
  if (!mods) {
    switch (c) {
    case ARROW_DOWN: csel = (csel + 1) % nshown; return 1;
    case ARROW_UP: csel = (csel + nshown - 1) % nshown; return 1;
    case PAGE_DOWN:
      csel += COMP_ROWS;
      if (csel >= nshown) csel = nshown - 1;
      return 1;
    case PAGE_UP:
      csel -= COMP_ROWS;
      if (csel < 0) csel = 0;
      return 1;
    case '\r':
    case '\t':
      comp_accept();
      return 1;
    case '\x1b':
      comp_close();
      return 1;
    case BACKSPACE:
    case CTRL_KEY('h'):
      refilter_pending = 1;
      return 0;
    }
    if (c < 256 && ident_byte((unsigned char)c)) {
      refilter_pending = 1;
      return 0;
    }
  }
  comp_close();
  return 0;
}

static void comp_ask(int trigger) {
  last_trigger = trigger;
  lsp_ask(V.buf, LSP_REQ_COMPLETION, CUR.cy, CUR.cx, trigger, NULL);
}

/* After a key has done what it does. With a list open, the word it edited is
 * matched again -- and the list closes when the cursor has left the word or
 * nothing matches. Without one, a character the server named as a trigger
 * asks it what could come next. */
void lspui_after_key(int c) {
  if (comp_open_p() && refilter_pending) {
    refilter_pending = 0;
    if (V.buf != comp_buf || CUR.cy != comp_cy || CUR.cx < comp_start ||
        word_start() != comp_start) {
      comp_close();
    } else if (incomplete) {
      comp_ask(0);
    } else if (comp_filter() == 0) {
      comp_close();
    }
    return;
  }
  refilter_pending = 0;
  if (comp_open_p() || c <= 0 || c > 127 || V.ncursors > 1) return;
  if (CUR.cx == 0 || CUR.cy >= row_count() ||
      row_at(CUR.cy)->chars[CUR.cx - 1] != c)
    return;
  if (lsp_is_trigger(V.buf, c)) comp_ask(c);
}

/* ---- the mailbox --------------------------------------------------------- */

/* A completion answer is still good if the cursor has only gone on through
 * the word it was asked about: the list is for a prefix of what is there now,
 * and filtering it is what typing would have done anyway. */
static int completion_current(void) {
  int i;
  erow *r;
  if (box.buf != V.buf || CUR.cy != box.cy || CUR.cx < box.cx ||
      V.ncursors > 1 || CUR.cy >= row_count())
    return 0;
  r = row_at(CUR.cy);
  for (i = box.cx; i < CUR.cx; i++)
    if (!ident_byte((unsigned char)r->chars[i])) return 0;
  return 1;
}

static void deliver_completion(void) {
  erow *r = row_at(CUR.cy);
  int i;
  comp_close();
  items = box.items;
  nitems = box.nitems;
  incomplete = box.incomplete;
  box.items = NULL;
  box.nitems = 0;
  for (i = 0; i < nitems; i++)
    if (items[i].start >= 0)
      items[i].start = lsp_units_to_col(r->chars, r->size, items[i].start, box.utf8);
  comp_buf = V.buf;
  comp_cy = CUR.cy;
  comp_start = word_start();
  if (comp_filter() == 0) {
    comp_close();
    /* Asked for, and nothing to offer: say so. Offered unasked after a dot,
     * and nothing: that is not worth a message. */
    if (!last_trigger) editor_set_status_message("No completions here.");
    return;
  }
  comp_show();
}

/* Between keys, in the main loop: act on an answer if it is still about where
 * the user is. Only the main loop calls this, which is also what keeps a menu
 * or a dialog from having a jump happen underneath it: those run loops of
 * their own, inside a key, and the mailbox waits until they have closed --
 * by which time the cursor check below usually says the moment has passed. */
void lspui_deliver(void) {
  if (box.kind == 0) return;
  if (box.kind == LSP_REQ_COMPLETION) {
    if (completion_current()) deliver_completion();
    box_clear();
    return;
  }
  if (box.buf != V.buf || box.buf->edits != box.edits || CUR.cy != box.cy ||
      CUR.cx != box.cx) {
    box_clear();
    return;
  }
  if (box.kind == LSP_REQ_RENAME) {
    deliver_rename();
  } else if (box.kind == LSP_REQ_DEFINITION) {
    if (box.path != NULL) deliver_definition();
    else if (box.text != NULL) editor_set_status_message("%s", box.text);
  } else if (box.kind == LSP_REQ_HOVER) {
    if (box.text == NULL || box.text[0] == '\0')
      editor_set_status_message("Nothing to show here.");
    else
      hover_open(box.text);
  }
  box_clear();
}

/* ---- asking -------------------------------------------------------------- */

static void ask(int kind, const char *what) {
  if (!lsp_ask(V.buf, kind, CUR.cy, CUR.cx, 0, NULL)) {
    char why[160];
    if (V.buf->lsp_srv > 0 && !lsp_can(V.buf, kind))
      editor_set_status_message("This file's language server cannot %s.", what);
    else {
      lsp_describe(V.buf, why, sizeof(why));
      editor_set_status_message("%s", why);
    }
    return;
  }
  hover_close();
  comp_close();
}

int lspui_definition(void) {
  ask(LSP_REQ_DEFINITION, "find definitions");
  return 0;
}

int lspui_hover(void) {
  ask(LSP_REQ_HOVER, "describe things");
  return 0;
}

int lspui_complete(void) {
  if (V.ncursors > 1) {
    editor_set_status_message("Completion works at one cursor, not several.");
    return 0;
  }
  last_trigger = 0;
  ask(LSP_REQ_COMPLETION, "complete");
  return 0;
}

int lspui_can(int kind) { return lsp_can(V.buf, kind); }
