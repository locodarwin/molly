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
 *     the text's own color for the quieter two. Where is it?
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
  /* And the projects said no to are asked about again. */
  trust_forget_noes();
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

  struct fedit *refs;     /* references: sl/sc used, text holding the path */
  int nrefs;

  char *sig;              /* signature help: the active signature's label */
  int sig_p0, sig_p1;     /* the active parameter in it, bytes; -1 for none */
  char *sig_doc;          /* the first line of what is said about it */
  int sig_n, sig_i;       /* how many signatures, and which this is */

  struct citem *items;    /* completion: what could go here */
  int nitems;
  int incomplete;         /* the server would say more given more to go on */
  int trigger;            /* asked because this was typed, not Alt-/ */

  char **atitle;          /* code actions: what each is called */
  char **ajson;           /* and each as the server sent it */
  int nactions;
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
  free(box.sig);
  free(box.sig_doc);
  {
    int i;
    for (i = 0; i < box.nrefs; i++) free(box.refs[i].text);
    free(box.refs);
  }
  free(box.refuse);
  {
    int i;
    for (i = 0; i < box.nactions; i++) {
      free(box.atitle[i]);
      free(box.ajson[i]);
    }
    free(box.atitle);
    free(box.ajson);
  }
  memset(&box, 0, sizeof(box));
}

/* A buffer is going away: anything in the mailbox about it goes too. */
static void comp_close(void);
static struct buffer *comp_buf;

static void sig_close(void);
static struct buffer *sig_buf;

void lspui_forget(struct buffer *b) {
  if (box.buf == b) box_clear();
  if (comp_buf == b) comp_close();
  if (sig_buf == b) sig_close();
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
static void read_text_edits(struct fchange *f, const struct json *list);

/* What a WorkspaceEdit being read is for, to say why it is refused. */
static const char *edit_what = "rename";

static void add_edits(const char *uri, const struct json *list) {
  char *path = lsp_uri_to_path(uri);
  int i;
  if (path == NULL) {
    if (box.refuse == NULL) {
      char why[120];
      snprintf(why, sizeof(why), "This %s reaches a file molly cannot open.", edit_what);
      box.refuse = xstrdup(why);
    }
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
  read_text_edits(&box.files[i], list);
}

/* A list of TextEdits, onto the end of a file's. */
static void read_text_edits(struct fchange *f, const struct json *list) {
  const struct json *k;
  if (list == NULL || list->type != JSON_ARRAY || list->n == 0) return;
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
        char why[160];
        snprintf(why, sizeof(why), "This %s would create, move or delete files, "
                 "which molly does not do.", edit_what);
        free(box.refuse);
        box.refuse = xstrdup(why);
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

/* Documentation, which is a string or MarkupContent, as its first line that
 * says something: a fence or a blank opens a lot of them. */
static char *first_doc_line(const struct json *d) {
  const char *s = d == NULL ? NULL : d->type == JSON_STRING ? d->str
                                    : json_str(json_get(d, "value"));
  while (s != NULL && *s != '\0') {
    size_t n = strcspn(s, "\n");
    const char *p = s;
    while (p < s + n && (*p == ' ' || *p == '\t')) p++;
    if (p < s + n && strncmp(p, "```", 3) != 0) {
      char *out = xmalloc(n + 1);
      memcpy(out, s, n);
      out[n] = '\0';
      return out;
    }
    s += n;
    if (*s == '\n') s++;
  }
  return NULL;
}

/* SignatureHelp: the signatures of the call the cursor is in, which one is
 * active, and which of its parameters. A parameter's label is either a
 * string, to be found in the signature's label, or a pair of offsets into it
 * -- in UTF-16 units, as the protocol counts everything. */
static void read_signature(const struct json *r) {
  const struct json *sigs = json_get(r, "signatures"), *sg, *params, *pl;
  const char *label;
  int active, ap, len;
  box.sig_p0 = box.sig_p1 = -1;
  if (sigs == NULL || sigs->type != JSON_ARRAY || sigs->n == 0) return;
  active = json_int(json_get(r, "activeSignature"), 0);
  if (active < 0 || active >= sigs->n) active = 0;
  sg = json_at(sigs, active);
  label = json_str(json_get(sg, "label"));
  if (label == NULL) return;
  box.sig = xstrdup(label);
  box.sig_n = sigs->n;
  box.sig_i = active;
  len = (int)strlen(label);
  ap = json_int(json_get(sg, "activeParameter"), json_int(json_get(r, "activeParameter"), 0));
  params = json_get(sg, "parameters");
  pl = json_get(json_at(params, ap), "label");
  if (pl != NULL && pl->type == JSON_STRING && pl->n > 0) {
    const char *at = strstr(label, pl->str);
    if (at != NULL) {
      box.sig_p0 = (int)(at - label);
      box.sig_p1 = box.sig_p0 + pl->n;
    }
  } else if (pl != NULL && pl->type == JSON_ARRAY && pl->n == 2) {
    box.sig_p0 = lsp_units_to_col(label, len, json_int(json_at(pl, 0), 0), 0);
    box.sig_p1 = lsp_units_to_col(label, len, json_int(json_at(pl, 1), 0), 0);
    if (box.sig_p1 <= box.sig_p0) box.sig_p0 = box.sig_p1 = -1;
  }
  box.sig_doc = first_doc_line(json_get(json_at(params, ap), "documentation"));
  if (box.sig_doc == NULL) box.sig_doc = first_doc_line(json_get(sg, "documentation"));
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
  } else if (kind == LSP_REQ_SIGNATURE) {
    read_signature(result);
  } else if (kind == LSP_REQ_REFERENCES) {
    const struct json *k;
    if (result != NULL && result->type == JSON_ARRAY && result->n > 0) {
      box.refs = xmalloc(sizeof(*box.refs) * (size_t)result->n);
      for (k = result->kid; k != NULL; k = k->next) {
        char *path = lsp_uri_to_path(json_str(json_get(k, "uri")));
        const struct json *st = json_get(json_get(k, "range"), "start");
        if (path == NULL) continue;
        box.refs[box.nrefs].text = path;
        box.refs[box.nrefs].sl = json_int(json_get(st, "line"), 0);
        box.refs[box.nrefs].sc = json_int(json_get(st, "character"), 0);
        box.nrefs++;
      }
    }
  } else if (kind == LSP_REQ_FORMAT) {
    /* The edits are to the buffer that asked, which has no need of a path. */
    box.files = xmalloc(sizeof(*box.files));
    memset(box.files, 0, sizeof(*box.files));
    box.files[0].path = xstrdup("");
    box.nfiles = 1;
    if (result == NULL) box.refuse = xstrdup("The language server could not format this.");
    else read_text_edits(&box.files[0], result);
  } else if (kind == LSP_REQ_RENAME) {
    edit_what = "rename";
    if (result != NULL && result->type == JSON_OBJECT) read_workspace_edit(result);
    else box.refuse = xstrdup("The language server cannot rename that.");
  } else if (kind == LSP_REQ_ACTIONS) {
    const struct json *k;
    int n = (result != NULL && result->type == JSON_ARRAY) ? result->n : 0;
    if (n > 0) {
      box.atitle = xmalloc(sizeof(char *) * (size_t)n);
      box.ajson = xmalloc(sizeof(char *) * (size_t)n);
    }
    for (k = n > 0 ? result->kid : NULL; k != NULL; k = k->next) {
      const char *t = json_str(json_get(k, "title"));
      struct abuf ab = ABUF_INIT;
      /* A disabled action is one the server shows to say why it cannot be
       * done here; molly has nowhere to say that but the list, and an entry
       * that does nothing when chosen is worse than none. */
      if (t == NULL || json_get(k, "disabled") != NULL) continue;
      json_write(&ab, k);
      ab_append(&ab, "", 1);
      box.atitle[box.nactions] = xstrdup(t);
      box.ajson[box.nactions++] = ab.b;
    }
  } else if (kind == LSP_REQ_RESOLVE || kind == LSP_REQ_APPLY) {
    /* A resolved CodeAction holds its edit; an applyEdit is the edit. */
    const struct json *we = kind == LSP_REQ_RESOLVE ? json_get(result, "edit") : result;
    edit_what = "change";
    if (we != NULL && we->type == JSON_OBJECT) read_workspace_edit(we);
    if (kind == LSP_REQ_RESOLVE && json_get(result, "command") != NULL) {
      struct abuf ab = ABUF_INIT;
      json_write(&ab, result);
      ab_append(&ab, "", 1);
      box.text = ab.b;          /* still to run, once the edit is made */
    }
  }
  ui_request_redraw();
}

/* ---- going there and back ------------------------------------------------ */

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

int lspui_can_back(void) { return nav_can_back(); }

int lspui_back(void) { return nav_back(); }

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

  nav_push();
  if (at >= 0) {
    if (V.buf != buf_at(at)) buf_show(at);
  } else {
    /* Under the working directory, by the name the user would have typed; so
     * the tab says src/a.c and not the whole path. */
    const char *name = box.path;
    if (getcwd(cwd, sizeof(cwd)) != NULL && (n = strlen(cwd)) > 1 &&
        strncmp(box.path, cwd, n) == 0 && box.path[n] == '/')
      name = box.path + n + 1;
    if (!editor_open_in_new_buffer(xstrdup(name))) return;
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

/* ---- the problems list --------------------------------------------------- */

/* Every problem in every open file, a row each -- `path:line:col: error: what`
 * -- in a buffer of its own, kept up to date: a server's new opinion of any
 * file rewrites it. Enter on a row goes there.
 *
 * Built from the main loop, as every other answer is acted on: a server's
 * diagnostics arrive inside the event loop, and only say the list is out of
 * date (lspui_problems_changed). The rows are a buffer's like any other, so
 * the list scrolls, searches and splits; editing it is allowed and pointless,
 * since the next refresh writes over it.
 *
 * Each row carries a diagnostic of its own, over its severity word, so the
 * word is drawn in the problem's color by the code that draws every other
 * problem -- and the status bar's counts, over this buffer, are the totals. */

static int problems_stale;
/* The column each row's problem starts at, for Enter; trusted only while the
 * list has as many rows as when it was written, as find-in-files' are. */
static int *prob_cols;
static int nprob_cols;

void lspui_problems_changed(void) { problems_stale = 1; }

static struct buffer *problems_buffer(void) {
  int i;
  for (i = 0; i < buf_count(); i++)
    if (buf_at(i)->problems) return buf_at(i);
  return NULL;
}

/* Rewrite the list's rows from every buffer's diagnostics. Works on the list
 * by borrowing the current view's buffer pointer for the duration, as
 * buf_free does, and touches no cursor but the list's own windows'. */
static void problems_fill(struct buffer *pb) {
  struct buffer *was = V.buf;
  struct diag *marks = NULL;
  int nmarks = 0, i, k, total = 0;

  for (i = 0; i < buf_count(); i++)
    if (!buf_at(i)->problems) total += buf_at(i)->ndiag;
  free(prob_cols);
  prob_cols = xmalloc(sizeof(*prob_cols) * (size_t)(total > 0 ? total : 1));
  nprob_cols = 0;
  if (total > 0) marks = xmalloc(sizeof(*marks) * (size_t)total);

  V.buf = pb;
  editor_clear_rows();
  for (i = 0; i < buf_count(); i++) {
    struct buffer *b = buf_at(i);
    if (b->problems) continue;
    for (k = 0; k < b->ndiag; k++) {
      const struct diag *d = &b->diag[k];
      char row[400], msg[300];
      int col = 0, n, w0;
      static const char *const words[] = {"", "error", "warning", "info", "hint"};
      if (d->y0 < b->numrows) {
        erow *r = buf_row(b, d->y0);
        col = editor_row_cx_to_charcol(r, d->x0 <= r->size ? d->x0 : r->size);
      }
      snprintf(msg, sizeof(msg), "%s", d->msg);
      msg[strcspn(msg, "\n")] = '\0';
      for (n = 0; msg[n]; n++)
        if ((unsigned char)msg[n] < 0x20) msg[n] = ' ';
      n = snprintf(row, sizeof(row), "%.200s:%d:%d: ", buf_name(b), d->y0 + 1, col + 1);
      w0 = n;
      n += snprintf(row + n, sizeof(row) - (size_t)n, "%s: %s", words[d->severity], msg);
      if (n >= (int)sizeof(row)) n = (int)sizeof(row) - 1;
      editor_insert_row(row_count(), row, (size_t)n);
      marks[nmarks].y0 = marks[nmarks].y1 = row_count() - 1;
      marks[nmarks].x0 = w0;
      marks[nmarks].x1 = w0 + (int)strlen(words[d->severity]);
      marks[nmarks].severity = d->severity;
      marks[nmarks].msg = xstrdup(d->msg);
      nmarks++;
      prob_cols[nprob_cols++] = d->x0;
    }
  }
  editor_free_undo_redo();
  saved_snapshot_free();
  saved_snapshot_take();
  B.dirty = 0;
  lsp_diag_clear(V.buf);
  V.buf->diag = marks;
  V.buf->ndiag = nmarks;
  V.buf = was;

  /* Windows on the list keep their place where it still exists. */
  for (i = 0; i < window_count(); i++) {
    struct view *v = window_view_at(i);
    if (v->buf != pb) continue;
    for (k = 0; k < v->ncursors; k++) {
      struct cursor *c = &v->cursors[k];
      c->sel_active = 0;
      if (c->cy >= pb->numrows) c->cy = pb->numrows > 0 ? pb->numrows - 1 : 0;
      if (c->cy < pb->numrows && c->cx > buf_row(pb, c->cy)->size)
        c->cx = buf_row(pb, c->cy)->size;
    }
  }
}

/* From the main loop: bring the list up to date if there is one and it is not. */
void lspui_problems_refresh(void) {
  struct buffer *pb;
  if (!problems_stale) return;
  pb = problems_buffer();
  if (pb != NULL) problems_fill(pb);
  problems_stale = 0;
}

/* Ctrl-F2: the list, in a window below -- or the window already showing it. */
int lspui_problems(void) {
  struct buffer *pb = problems_buffer();
  int i, n = 0;
  for (i = 0; i < window_count(); i++) {
    if (window_view_at(i)->buf == pb && pb != NULL) {
      window_focus(i);
      return 0;
    }
  }
  if (pb == NULL) {
    pb = buf_new();
    pb->problems = 1;
    pb->title = xstrdup("problems");
  }
  problems_fill(pb);
  if (window_split(WIN_ROWS)) buf_show(buf_index_of(pb));
  else buf_show(buf_index_of(pb));
  memset(&CUR, 0, sizeof(CUR));
  V.drowoff = V.coloff = 0;
  screen_force_repaint();
  n = pb->numrows;
  if (n == 0)
    editor_set_status_message("No problems in the open files. The list stays up to date.");
  else
    editor_set_status_message("%d problem%s -- Enter goes to one; the list stays up to date",
                              n, n == 1 ? "" : "s");
  return 0;
}

/* Enter in the list: to the problem on this row, in the window the list was
 * opened from when there is one, as find in files does. Returns 1 when it
 * went; a row that names nothing is not an Enter either way. */
int lspui_problems_jump(void) {
  int at = CUR.cy, p0, pl, ln, col;
  struct buffer *pb = V.buf;
  erow *r;
  char *path;
  if (!pb->problems || at >= pb->numrows) return 1;
  r = row_at(at);
  if (!grep_parse_line(r->chars, r->size, &p0, &pl, &ln)) return 1;
  path = xmalloc((size_t)pl + 1);
  memcpy(path, r->chars + p0, (size_t)pl);
  path[pl] = '\0';
  col = (nprob_cols == pb->numrows) ? prob_cols[at] : 0;
  if (window_count() > 1) window_move_focus(-1);
  nav_push();
  {
    int i, found = -1;
    for (i = 0; i < buf_count(); i++)
      if (buf_at(i)->filename != NULL && strcmp(buf_at(i)->filename, path) == 0) found = i;
    if (found >= 0) {
      if (V.buf != buf_at(found)) buf_show(found);
      free(path);
    } else if (!editor_open_in_new_buffer(path)) {
      return 1;
    }
  }
  cursor_to(ln - 1, col);
  return 1;
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

/* One file's edits, into the current buffer, as one undo step. `track_y` and
 * `track` are a position to carry along -- where the cursor was -- moved down
 * or up by the lines edits above it add or take away, and along its row by
 * the edits before it there, or to the start of the one it was inside. Either
 * may be NULL. An edit that would put back exactly what is there is skipped:
 * a formatter returns a file's every line, and a file it did not change must
 * not come back modified. */
static int apply_edits(struct fchange *f, int utf8, int *track_y, int *track) {
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
    if (sy == ey && sy < row_count() && ex - sx == len &&
        memcmp(row_at(sy)->chars + sx, e->text, (size_t)len) == 0)
      continue;
    if (track_y != NULL) {
      int lines = 0;
      const char *p;
      for (p = e->text; *p; p++)
        if (*p == '\n') lines++;
      if (ey < *track_y) {
        *track_y += lines - (ey - sy);
      } else if (track != NULL && sy == *track_y && ey == sy && lines == 0) {
        if (ex <= *track) *track += len - (ex - sx);
        else if (sx <= *track) *track = sx;
      }
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
/* The mailbox's edits, made: each file's in its buffer, opened when it is not
 * open, and the cursor kept where it was in the buffer the user is in. How
 * many changes in all, and how many of those and how many other files were
 * outside that buffer, into *changes, *elsewhere and *others. */
static int apply_refused;   /* files the last of these could not open */

static void apply_workspace(int *changes, int *elsewhere, int *others) {
  struct buffer *home = V.buf;
  int hy = CUR.cy, hx = CUR.cx, i, home_changes = 0;
  *changes = *others = 0;
  apply_refused = 0;
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
      if (!editor_open_in_new_buffer(xstrdup(name))) {
        apply_refused++;
        continue;
      }
    }
    {
      int n = apply_edits(f, box.utf8, V.buf == home ? &hy : NULL,
                          V.buf == home ? &hx : NULL);
      *changes += n;
      if (V.buf == home) home_changes += n;
    }
    if (V.buf != home) (*others)++;
  }
  if (V.buf != home) buf_show(buf_index_of(home));
  cursor_to(hy, hx);
  *elsewhere = *changes - home_changes;
}

/* Said last, over what was done, because it is what the user must not miss. */
static void say_refused(void) {
  if (apply_refused > 0)
    editor_set_status_message("%d file%s could not be opened, so %s not changed.",
                              apply_refused, apply_refused == 1 ? "" : "s",
                              apply_refused == 1 ? "it was" : "they were");
}

static void deliver_rename(void) {
  int i, changes = 0, others = 0, elsewhere = 0;

  if (box.refuse != NULL) {
    editor_set_status_message("%s", box.refuse);
    return;
  }
  for (i = 0; i < box.nfiles; i++) changes += box.files[i].n;
  if (changes == 0) {
    editor_set_status_message("Nothing to rename there.");
    return;
  }
  apply_workspace(&changes, &elsewhere, &others);
  /* The other files are what needs saying: they changed where the user is
   * not looking, and are not saved. Their tabs say which, with a `*`. */
  if (others > 0)
    editor_set_status_message("Renamed to %s: %d change%s, %d in %d other file%s, unsaved.",
                              rename_to, changes, changes == 1 ? "" : "s",
                              elsewhere, others, others == 1 ? "" : "s");
  else
    editor_set_status_message("Renamed to %s: %d change%s.", rename_to, changes,
                              changes == 1 ? "" : "s");
  say_refused();
}

/* ---- code actions ------------------------------------------------------ */

static void ask(int kind, const char *what);

/* The action last chosen, to say what its edits were for when they come back
 * from a resolve or a command. */
static char action_title[120];

/* Made: the mailbox's edits, said with what they were for. */
static void deliver_change(const char *what) {
  int i, changes = 0, others = 0, elsewhere = 0;
  if (box.refuse != NULL) {
    editor_set_status_message("%s", box.refuse);
    return;
  }
  for (i = 0; i < box.nfiles; i++) changes += box.files[i].n;
  if (changes == 0) {
    if (what != NULL) editor_set_status_message("%s: nothing to change.", what);
    return;
  }
  apply_workspace(&changes, &elsewhere, &others);
  if (others > 0)
    editor_set_status_message("%s: %d change%s, %d in %d other file%s, unsaved.",
                              what != NULL ? what : "Changed", changes,
                              changes == 1 ? "" : "s", elsewhere, others,
                              others == 1 ? "" : "s");
  else
    editor_set_status_message("%s: %d change%s.", what != NULL ? what : "Changed",
                              changes, changes == 1 ? "" : "s");
  say_refused();
}

static char **pick_titles;

static void pick_row(void *user, int i, char *dst, size_t dsz) {
  (void)user;
  snprintf(dst, dsz, "%s", pick_titles[i]);
}

/* Which of `n` titles, from a list; -1 for none. */
static int pick(const char *title, char **titles, int n) {
  struct widget widgets[2];
  struct dialog d;
  struct layer l;
  int depth, li, chosen = -1;

  pick_titles = titles;
  widget_list(&widgets[0], 0, 0, 1, 1, n, pick_row, NULL);
  widget_label(&widgets[1], 0, 1, 1, "Enter apply   Esc cancel");
  widgets[0].sel = 0;
  depth = layer_count();
  memset(&l, 0, sizeof(l));
  li = layer_push(&l);
  if (li < 0) return -1;
  for (;;) {
    struct layer *lp;
    int cols, rows, list_h, c, ns = 0, i, w = 24;
    screen_sync_size();
    for (i = 0; i < n; i++)
      if (utf8_str_cols(titles[i]) + 6 > w) w = utf8_str_cols(titles[i]) + 6;
    cols = grid_cols() - 4;
    if (cols > w) cols = w;
    if (cols < 16) cols = 16;
    rows = n + 3 + 2;
    if (rows > grid_rows() - 4) rows = grid_rows() - 4;
    if (rows < 5) rows = 5;
    list_h = rows - 2 - 1;
    widgets[0].w = cols - 4;
    widgets[0].h = list_h;
    widgets[1].y = list_h;
    widgets[1].w = cols - 4;
    widget_list_clamp(&widgets[0]);
    dialog_init(&d, title, widgets, 2, cols - 2, rows - 2);
    lp = layer_get(li);
    dialog_layout(&d, lp);
    editor_refresh_screen();
    c = ui_wait_key();
    if (c == WINCH_KEY) continue;
    if (c == MOUSE_KEY) {
      struct mouse_event ev;
      input_last_mouse(&ev);
      if (ev.type == MOUSE_PRESS && ev.button == MOUSE_BUTTON_LEFT) {
        int k = ev.y - (lp->y + 1 + widgets[0].y);
        if (k >= 0 && k < widgets[0].h && widgets[0].top + k < n) {
          chosen = widgets[0].top + k;
          break;
        }
      }
      continue;
    }
    if (c == ARROW_DOWN || c == CTRL_KEY('n')) ns = 1;
    else if (c == ARROW_UP || c == CTRL_KEY('p')) ns = -1;
    else if (c == PAGE_DOWN) ns = list_h;
    else if (c == PAGE_UP) ns = -list_h;
    else if (c == '\r') { chosen = widgets[0].sel; break; }
    else if (c == '\x1b' || c == CTRL_KEY('c')) break;
    else continue;
    widget_list_move(&widgets[0], ns);
  }
  layer_pop_to(depth);
  return chosen;
}

/* One action, chosen: its edit made when it came with one, then its command
 * run when it has one; filled in by the server first when it has neither. */
static void run_action(const char *title, const char *json, int utf8) {
  struct json_doc *doc = json_parse(json, strlen(json));
  snprintf(action_title, sizeof(action_title), "%s", title);
  const struct json *a = json_root(doc), *we = json_get(a, "edit");
  const struct json *cmd = json_get(a, "command");
  if (a == NULL) {
    json_free(doc);
    return;
  }
  if (we != NULL && we->type == JSON_OBJECT) {
    box_clear();
    box.utf8 = utf8;
    edit_what = "change";
    read_workspace_edit(we);
    deliver_change(title);
    box_clear();
  }
  if (cmd != NULL) {
    if (!lsp_code_action_run(V.buf, json, 0, CUR.cy, CUR.cx))
      editor_set_status_message("The language server is not ready.");
    else if (we == NULL)
      editor_set_status_message("%s…", title);
  } else if (we == NULL && !lsp_code_action_run(V.buf, json, 1, CUR.cy, CUR.cx)) {
    editor_set_status_message("The language server gave nothing to do for that.");
  }
  json_free(doc);
}

static void deliver_actions(void) {
  char **titles, **jsons;
  int n = box.nactions, i, k, utf8 = box.utf8;
  if (n == 0) {
    editor_set_status_message("No actions here.");
    return;
  }
  /* Out of the mailbox, which an answer arriving while the list is up would
   * empty under it. */
  titles = box.atitle;
  jsons = box.ajson;
  box.atitle = box.ajson = NULL;
  box.nactions = 0;
  box_clear();
  k = pick("Code actions", titles, n);
  if (k >= 0) run_action(titles[k], jsons[k], utf8);
  for (i = 0; i < n; i++) {
    free(titles[i]);
    free(jsons[i]);
  }
  free(titles);
  free(jsons);
}

/* Alt-Enter: what the server can do here -- fixes for the problem under the
 * cursor, refactorings of the selection. */
int lspui_code_actions(void) {
  int y0 = CUR.cy, x0 = CUR.cx, y1 = CUR.cy, x1 = CUR.cx;
  if (!lsp_can(V.buf, LSP_REQ_ACTIONS)) {
    ask(LSP_REQ_ACTIONS, "offer code actions");
    return 0;
  }
  if (CUR.sel_active) selection_span(&x0, &y0, &x1, &y1);
  if (!lsp_code_actions(V.buf, y0, x0, y1, x1, CUR.cy, CUR.cx))
    editor_set_status_message("The language server is not ready yet.");
  return 0;
}

/* The name under the cursor, or "" -- letters, digits, underscores and
 * anything past ASCII, which is what an identifier is in every language molly
 * has a server for. */
static void cursor_word(char *word, size_t n) {
  int a, b;
  erow *r;
  word[0] = '\0';
  if (CUR.cy >= row_count()) return;
  r = row_at(CUR.cy);
  a = b = CUR.cx;
  while (a > 0 && (isalnum((unsigned char)r->chars[a - 1]) || r->chars[a - 1] == '_' ||
                   (unsigned char)r->chars[a - 1] >= 0x80)) a--;
  while (b < r->size && (isalnum((unsigned char)r->chars[b]) || r->chars[b] == '_' ||
                         (unsigned char)r->chars[b] >= 0x80)) b++;
  if (b - a > 0 && b - a < (int)n) {
    memcpy(word, r->chars + a, (size_t)(b - a));
    word[b - a] = '\0';
  }
}

/* ---- references ---------------------------------------------------------- */

static void ask(int kind, const char *what);

static char refs_of[64];   /* the name asked about */

static int ref_cmp(const void *a, const void *b) {
  const struct fedit *p = a, *q = b;
  int c = strcmp(p->text, q->text);
  if (c != 0) return c;
  return p->sl != q->sl ? (p->sl < q->sl ? -1 : 1) : 0;
}

/* Line `n` of a file on disk, into dst; "" when there is no such line. The
 * whole file is read for each distinct file, once: references come sorted by
 * file, so `cache` holds the one being read. */
static void disk_line(const char *path, int n, char *dst, size_t dsz,
                      char **cache, char **cached_path) {
  const char *p;
  int line = 0;
  dst[0] = '\0';
  if (*cached_path == NULL || strcmp(*cached_path, path) != 0) {
    /* The name came from a server, and could be a FIFO. */
    const char *why;
    int fd = file_open_read(path, &why);
    FILE *fp = fd == -1 ? NULL : fdopen(fd, "r");
    if (fp == NULL && fd != -1) close(fd);
    free(*cache);
    free(*cached_path);
    *cache = NULL;
    *cached_path = xstrdup(path);
    if (fp != NULL) {
      size_t len = 0, cap = 0, got;
      char buf[8192];
      while ((got = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (len + got + 1 > cap) {
          cap = (len + got + 1) * 2;
          *cache = xrealloc(*cache, cap);
        }
        memcpy(*cache + len, buf, got);
        len += got;
      }
      fclose(fp);
      if (*cache != NULL) (*cache)[len] = '\0';
    }
  }
  if (*cache == NULL) return;
  for (p = *cache; *p != '\0' && line < n; p++)
    if (*p == '\n') line++;
  if (line == n) {
    size_t k = strcspn(p, "\r\n");
    if (k >= dsz) k = dsz - 1;
    memcpy(dst, p, k);
    dst[k] = '\0';
  }
}

/* The references, as find-in-files results: a `path:line:text` row each,
 * sorted by file and line, in the same buffer -- so Enter opens one and F4
 * walks them, and nothing about a list of places had to be built twice. A
 * file that is open is read from its buffer, which is what the server was
 * told about; one that is not, from disk. */
static void deliver_references(void) {
  struct abuf ab = ABUF_INIT;
  char cwd[4096], line[512], title[96];
  char *cache = NULL, *cached = NULL;
  size_t cl = 0;
  int i, files, n = box.nrefs, *cols;

  if (n == 0) {
    editor_set_status_message("No references to %s found.", refs_of);
    return;
  }
  if (getcwd(cwd, sizeof(cwd)) != NULL) cl = strlen(cwd);
  qsort(box.refs, (size_t)n, sizeof(*box.refs), ref_cmp);
  cols = xmalloc(sizeof(*cols) * (size_t)n);
  for (i = 0; i < n; i++) {
    const struct fedit *r = &box.refs[i];
    const char *shown = r->text;
    int at = buffer_on(r->text);
    if (cl > 1 && strncmp(r->text, cwd, cl) == 0 && r->text[cl] == '/')
      shown = r->text + cl + 1;
    if (at >= 0) {
      struct buffer *b = buf_at(at);
      line[0] = '\0';
      if (r->sl < b->numrows) {
        erow *row = buf_row(b, r->sl);
        snprintf(line, sizeof(line), "%.*s", row->size, row->chars);
      }
    } else {
      disk_line(r->text, r->sl, line, sizeof(line), &cache, &cached);
    }
    cols[i] = lsp_units_to_col(line, (int)strlen(line), r->sc, box.utf8);
    json_putf(&ab, "%.300s:%d:", shown, r->sl + 1);
    ab_append(&ab, line, (int)strlen(line));
    ab_append(&ab, "\n", 1);
  }
  free(cache);
  free(cached);
  snprintf(title, sizeof(title), "references: %s", refs_of);
  files = grep_show_list(title, ab.b, ab.len, refs_of, cols, n);
  free(cols);
  free(ab.b);
  editor_set_status_message("%d reference%s to %.40s in %d file%s -- Enter opens one, F4 the next",
                            n, n == 1 ? "" : "s", refs_of, files, files == 1 ? "" : "s");
}

/* Shift-F12: where the name under the cursor is used. */
int lspui_references(void) {
  cursor_word(refs_of, sizeof(refs_of));
  if (refs_of[0] == '\0') {
    editor_set_status_message("Put the cursor on a name to find where it is used.");
    return 0;
  }
  ask(LSP_REQ_REFERENCES, "find references");
  return 0;
}

/* ---- formatting ---------------------------------------------------------- */

static int format_whole;   /* a selection was asked for and the file was sent */

static void deliver_format(void) {
  int y = CUR.cy, x = CUR.cx, n;
  if (box.refuse != NULL) {
    editor_set_status_message("%s", box.refuse);
    return;
  }
  n = apply_edits(&box.files[0], box.utf8, &y, &x);
  cursor_to(y, x);
  if (n == 0)
    editor_set_status_message("Already formatted.");
  else
    editor_set_status_message("Formatted%s: %d change%s, one undo step.",
                              format_whole ? " the whole file" : "",
                              n, n == 1 ? "" : "s");
}

static int format_on_save;
int lspui_format_on_save(void) { return format_on_save; }
void lspui_set_format_on_save(int on) { format_on_save = on ? 1 : 0; }

#define FORMAT_WAIT_MS 2000

/* Format on save, done before the save: how many changes it made, 0 when it
 * made none or there was nobody to make them, -1 when the server did not
 * answer in time. Waits for the server's answer,
 * up to FORMAT_WAIT_MS or Esc -- a save is a moment the user is waiting on
 * anyway, and a format that arrived after the file was written would leave
 * the buffer dirty with the formatted text unsaved. */
int lspui_format_before_save(void) {
  struct timespec t0, t1;
  int y = CUR.cy, x = CUR.cx, n;
  if (!format_on_save || !lsp_can(V.buf, LSP_REQ_FORMAT) ||
      !lsp_format(V.buf, 0, 0, 0, 0, 0, tab_width(), indent_insert_spaces(), CUR.cy, CUR.cx))
    return 0;
  box_clear();
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (;;) {
    long ms;
    loop_wait(20);
    if (box.kind == LSP_REQ_FORMAT) break;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    if (ms >= FORMAT_WAIT_MS || input_escape_pressed()) return -1;
  }
  if (box.buf != V.buf || box.edits != V.buf->edits || box.refuse != NULL) {
    box_clear();
    return 0;
  }
  n = apply_edits(&box.files[0], box.utf8, &y, &x);
  cursor_to(y, x);
  box_clear();
  return n;
}

/* Alt-Q: the language server's formatting -- of the selection when there is
 * one and the server can format a range, of the file otherwise. */
int lspui_format(void) {
  int range = CUR.sel_active && lsp_can_format_range(V.buf);
  int ay = 0, ax = 0, by = 0, bx = 0;
  if (!lsp_can(V.buf, LSP_REQ_FORMAT)) {
    ask(LSP_REQ_FORMAT, "format");
    return 0;
  }
  if (range) selection_span(&ax, &ay, &bx, &by);
  format_whole = CUR.sel_active && !range;
  if (!lsp_format(V.buf, range, ay, ax, by, bx, tab_width(), indent_insert_spaces(),
                  CUR.cy, CUR.cx))
    editor_set_status_message("The language server is not ready yet.");
  else if (format_whole)
    editor_set_status_message("This file's server formats whole files only; formatting it all.");
  return 0;
}

/* Alt-N: ask for the new name of what is under the cursor, then ask the
 * server what that means everywhere. */
int lspui_rename(void) {
  char prompt[200], word[64];
  char *name;

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
  cursor_word(word, sizeof(word));
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

/* A single-line box in the menu's colors: a popup is the same kind of thing
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

static int complete_typing;
int lspui_complete_typing(void) { return complete_typing; }
void lspui_set_complete_typing(int on) { complete_typing = on ? 1 : 0; }

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

/* Is the open list still about where the keyboard is: the same buffer, the same
 * row, which still exists, with the cursor in or after the word it began at?
 * Something other than a key can change that -- the keyboard moving to another
 * window, an edit a server sends -- and a list drawn from a row number in some
 * other buffer is a row that may not exist, and Enter on it puts a word from
 * one file into another. */
static int comp_current(void) {
  return comp_open_p() && V.buf == comp_buf && V.ncursors == 1 && CUR.cy == comp_cy &&
         comp_cy < row_count() && comp_start <= row_at(comp_cy)->size &&
         CUR.cx >= comp_start && CUR.cx <= row_at(comp_cy)->size;
}

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
  if (!comp_current()) return;   /* closed by the next key (lspui_key) */

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

/* ---- the signature popup ------------------------------------------------- */

/* The call the cursor is in, above the line it is on: the signature, the
 * parameter being typed picked out, and what is said about it. Above, so a
 * completion list can open under the same word without the two overlapping;
 * open until the server has nothing to say, the cursor leaves the line, or
 * Esc. It is asked again after every key while it is open, so the
 * parameter picked out follows the comma. */
static char *sig_label, *sig_doc;
static int sig_p0 = -1, sig_p1 = -1, sig_n, sig_i;
static int sig_layer = -1;
static int sig_cy;
static int sig_manual;   /* asked for with Alt-P, so "nothing" is said */
/* Asked and not yet answered. Typing goes on while the server thinks, and a
 * key typed then must ask again just as one typed with the box open does --
 * or the answer to `go(` arrives after `go(1)` and opens a box for a call
 * that is already closed. */
static int sig_pending;

static void sig_close(void) {
  if (sig_layer >= 0) layer_pop_to(sig_layer);
  sig_layer = -1;
  sig_pending = 0;
  free(sig_label);
  free(sig_doc);
  sig_label = sig_doc = NULL;
  sig_buf = NULL;
}

static void sig_draw(const struct layer *self) {
  int cy = V.screeny + drow_of_cursor(&CUR) - V.drowoff;
  int cx = V.screenx + (drow_cursor_col(&CUR) - V.coloff) + V.gutter_w;
  int cols = grid_cols(), w, h, x, y, off = 0, inner, used, len, i;
  char head[32] = "";
  (void)self;
  if (sig_label == NULL || V.buf != sig_buf) return;

  len = (int)strlen(sig_label);
  if (sig_n > 1) snprintf(head, sizeof(head), "%d/%d ", sig_i + 1, sig_n);
  w = utf8_str_cols(sig_label) + (int)strlen(head) + 4;
  if (sig_doc != NULL && utf8_str_cols(sig_doc) + 4 > w) w = utf8_str_cols(sig_doc) + 4;
  if (w > cols) w = cols;
  if (w < 8) return;
  /* Above if it fits, below if not; and where neither has room for the note
   * too, without the note -- the signature is the part worth the room. */
  {
    int full = sig_doc != NULL ? 4 : 3, top = V.screeny, bottom = V.screeny + V.screenrows;
    for (h = full; h >= 3; h--) {
      if (cy - h >= top) { y = cy - h; break; }
      if (cy + 1 + h <= bottom) { y = cy + 1; break; }
    }
    if (h < 3) return;
  }
  x = cx - 2;
  if (x + w > cols) x = cols - w;
  if (x < 0) x = 0;
  inner = w - 4 - (int)strlen(head);

  /* A signature wider than the box is shown from where the active parameter
   * can be seen, with an ellipsis for what is cut off the front. */
  if (sig_p1 > 0) {
    int c0 = 0;
    for (i = 0; i < sig_p1; i += utf8_char_bytes(sig_label, len, i)) c0++;
    if (c0 > inner - 2) {
      int want = sig_p0;
      for (i = 0; i < 6 && want > 0; i++) want = utf8_prev(sig_label, want);
      off = want;
    }
  }

  lspui_box(x, y, w, h);
  grid_role(ROLE_MENU);
  grid_puts(y + 1, x + 2, head, (int)strlen(head));
  {
    int px = x + 2 + (int)strlen(head), limit = x + w - 2, b;
    if (off > 0) px += grid_puts(y + 1, px, "\xe2\x80\xa6", 3);
    for (b = off; b < len && px < limit;) {
      int nb, cw = utf8_char_cols(sig_label, len, b, &nb);
      if (px + cw > limit) break;
      grid_role(b >= sig_p0 && b < sig_p1 ? ROLE_MENU_SELECTED : ROLE_MENU);
      px += grid_put_char(y + 1, px, sig_label + b, nb, cw);
      b += nb;
    }
  }
  if (sig_doc != NULL && h == 4) {
    int take = utf8_fit_cols(sig_doc, w - 4, &used);
    grid_role(ROLE_MENU_DISABLED);
    grid_puts(y + 2, x + 2, sig_doc, take);
  }
  grid_role(ROLE_TEXT);
}

static void sig_ask(int manual) {
  sig_manual = manual;
  sig_pending = lsp_ask(V.buf, LSP_REQ_SIGNATURE, CUR.cy, CUR.cx, 0, NULL);
  sig_buf = V.buf;
  sig_cy = CUR.cy;
}

static void deliver_signature(void) {
  sig_pending = 0;
  if (box.sig == NULL) {
    sig_close();
    if (sig_manual) editor_set_status_message("No signature to show here.");
    return;
  }
  free(sig_label);
  free(sig_doc);
  sig_label = box.sig;
  sig_doc = box.sig_doc;
  box.sig = box.sig_doc = NULL;
  sig_p0 = box.sig_p0;
  sig_p1 = box.sig_p1;
  sig_n = box.sig_n;
  sig_i = box.sig_i;
  sig_buf = V.buf;
  sig_cy = CUR.cy;
  if (sig_layer < 0) {
    struct layer l;
    memset(&l, 0, sizeof(l));
    l.draw = sig_draw;
    l.w = grid_cols();
    l.h = grid_rows();
    sig_layer = layer_push(&l);
  }
}

/* Alt-P: the signature of the call the cursor is in, asked for. */
int lspui_signature(void) {
  if (!lsp_can(V.buf, LSP_REQ_SIGNATURE)) {
    ask(LSP_REQ_SIGNATURE, "show signatures");
    return 0;
  }
  sig_ask(1);
  return 0;
}

/* After a key: keep an open signature current, or open one when a trigger
 * character was typed. */
static void sig_after_key(int c) {
  if (c == WINCH_KEY || c == MOUSE_KEY) return;
  if (sig_layer >= 0 || sig_pending) {
    if (V.buf != sig_buf || CUR.cy != sig_cy || V.ncursors > 1) sig_close();
    else sig_ask(sig_manual);
    return;
  }
  if (c <= 0 || c > 127 || V.ncursors > 1 || CUR.cx == 0 || CUR.cy >= row_count() ||
      row_at(CUR.cy)->chars[CUR.cx - 1] != c)
    return;
  if (lsp_is_sig_trigger(V.buf, c)) sig_ask(0);
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
  /* A list no longer about here is closed, and the key does what it does. */
  if (comp_open_p() && !comp_current()) comp_close();
  /* Esc closes the completion list first when both are open, and the
   * signature after; any other key leaves the signature where it is. */
  if (!comp_open_p()) {
    if (sig_layer >= 0 && c == '\x1b' && !mods) {
      sig_close();
      return 1;
    }
    return 0;
  }
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
  sig_after_key(c);
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
  if (lsp_is_trigger(V.buf, c)) {
    comp_ask(c);
  } else if (complete_typing && ident_byte((unsigned char)c) &&
             CUR.cx - word_start() >= 2 && lsp_can(V.buf, LSP_REQ_COMPLETION)) {
    /* Asked as if by Alt-/, but quietly: nothing to offer two letters into a
     * word is not news. */
    last_trigger = 1;
    lsp_ask(V.buf, LSP_REQ_COMPLETION, CUR.cy, CUR.cx, 0, NULL);
  }
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
  /* A signature is about the call on this line, wherever on it the cursor
   * has got to since: that it moved is why it was asked again. */
  if (box.kind == LSP_REQ_SIGNATURE) {
    if (box.buf == V.buf && CUR.cy == box.cy && V.ncursors == 1) deliver_signature();
    box_clear();
    return;
  }
  /* The server's own edit, from a command it was asked to run, is made
   * wherever the user has got to: it is to files, not to the cursor. */
  if (box.kind == LSP_REQ_APPLY) {
    deliver_change(action_title[0] ? action_title : NULL);
    box_clear();
    return;
  }
  if (box.buf != V.buf || box.buf->edits != box.edits || CUR.cy != box.cy ||
      CUR.cx != box.cx) {
    box_clear();
    return;
  }
  if (box.kind == LSP_REQ_ACTIONS) {
    deliver_actions();
    return;
  }
  if (box.kind == LSP_REQ_RESOLVE) {
    char *then = box.text;
    box.text = NULL;
    deliver_change(action_title);
    if (then != NULL && !lsp_code_action_run(V.buf, then, 0, CUR.cy, CUR.cx))
      editor_set_status_message("The language server is not ready.");
    free(then);
    box_clear();
    return;
  }
  if (box.kind == LSP_REQ_RENAME) {
    deliver_rename();
  } else if (box.kind == LSP_REQ_FORMAT) {
    deliver_format();
  } else if (box.kind == LSP_REQ_REFERENCES) {
    deliver_references();
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
