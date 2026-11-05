/* lsp.c -- talking to language servers.
 *
 * A language server is a program that knows a language -- clangd for C and
 * C++, rust-analyzer, pylsp -- and answers questions about a file over a pipe:
 * what is wrong with it, where a name is defined, what could come next. The
 * protocol is JSON-RPC with a Content-Length header in front of each message,
 * on the server's standard input and output. This file is the client half:
 * starting servers, keeping each one's idea of an open file in step with the
 * buffer, and turning what comes back into something the rest of molly can
 * draw.
 *
 * Three decisions shape it.
 *
 *   - **One server per command, not per language.** C and C++ both name
 *     `clangd`, and two clangds indexing one project would be twice the memory
 *     for the same answers. A server is found by the command line that starts
 *     it, so two filetypes that name the same one share it.
 *
 *   - **Documents are synchronized by a sweep, not by hooks.** A buffer can
 *     gain a server by being opened, by being saved under a new name, by a
 *     config change or by its server finishing its handshake; it can lose one
 *     by being closed, renamed or retyped. Hooking each of those would be a
 *     dozen call sites, each of which a new feature could forget. Instead
 *     lsp_idle() compares every buffer with what its server has been told and
 *     sends the difference -- open, change or close. It runs when the keyboard
 *     has been quiet for a tick, so typing a word sends nothing until the word
 *     is finished, and a request flushes its own buffer first so a question is
 *     never asked about stale text.
 *
 *   - **The whole text, every time.** `didChange` can carry a range edit or the
 *     full document; this sends the full document. Range edits would mean
 *     recording every change at the row primitives, in the server's position
 *     encoding, in order -- and getting one wrong desynchronizes the server
 *     silently, which is the worst way for this to fail. A source file is tens
 *     of kilobytes and a pipe moves that in microseconds, and a buffer bigger
 *     than LSP_MAX_DOC is not sent at all.
 *
 * Positions: the protocol counts a column in UTF-16 code units unless the
 * server agrees to something else, and molly counts bytes. The client offers
 * UTF-8 first, which clangd and rust-analyzer accept, and converts when a
 * server insists on UTF-16. */

#include "molly.h"
#include <poll.h>

#define LSP_MAX_SERVERS 6

/* The largest message molly will accept, and the largest document it will
 * send. A completion list from clangd is a few hundred kilobytes; anything
 * past these is a server that has gone wrong or a file no server will thank
 * molly for. */
#define LSP_MAX_MESSAGE (64 << 20)
#define LSP_MAX_DOC     (16 << 20)

#define LSP_MAX_PENDING 16

/* How long a server gets at exit to go of its own accord, after being asked. */
#define LSP_EXIT_GRACE_MS 300

enum srv_state {
  SRV_FREE = 0,
  SRV_MISSING,   /* its program is not installed: tried once, quietly */
  SRV_STARTING,  /* `initialize` sent, no answer yet */
  SRV_READY,
  SRV_DEAD       /* it exited, or broke the protocol; restart brings it back */
};

struct pending {
  int id;
  int kind;                /* LSP_REQ_* */
  struct buffer *buf;      /* what it was asked about; NULL for none */
  unsigned edits;          /* the buffer's `edits` when it was asked */
  int cx, cy;              /* and the cursor */
};

struct server {
  int state;
  char *cmd;
  char *root;              /* the project it was started for, a directory */
  char name[32];           /* the program, for messages */
  pid_t pid;

  int in_fd, out_fd, err_fd;
  int in_id, out_id, err_id;

  struct abuf wq;          /* waiting to be written */
  int wdone;               /* how much of it has been */

  char *rb;                /* read, not yet a whole message */
  size_t rlen, rcap;

  int next_id;
  struct pending pend[LSP_MAX_PENDING];
  int npend;

  /* What it said it can do. */
  int utf8;                /* columns are bytes, not UTF-16 units */
  int sync;                /* 0 none, 1 full, 2 incremental */
  int save;                /* wants didSave */
  int hover, definition, completion, rename, references, signature;
  int format, format_range;
  int actions, resolve;    /* code actions, and filling one in afterwards */
  char sig_triggers[16];   /* signature help's, trigger and retrigger both */
  char triggers[16];       /* completion trigger characters, the one-byte ones */

  char lasterr[160];       /* the last line it wrote to standard error */
};

static struct server servers[LSP_MAX_SERVERS];
static int enabled = 1;

/* ---- the clock ----------------------------------------------------------- */

static long now_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- positions ----------------------------------------------------------- */

/* A byte offset into a row, as the column the server counts. */
int lsp_col_to_units(const char *s, int size, int col, int utf8) {
  int i = 0, units = 0;
  if (col > size) col = size;
  if (utf8) return col < 0 ? 0 : col;
  while (i < col) {
    int n = utf8_char_bytes(s, size, i);
    units += (n == 4) ? 2 : 1;
    i += n;
  }
  return units;
}

/* The server's column, as a byte offset into the row: the start of the
 * character it lands in, and the end of the row if it is past it. A column in
 * the middle of a surrogate pair is the character it is half of. */
int lsp_units_to_col(const char *s, int size, int units, int utf8) {
  int i = 0, u = 0;
  if (units <= 0) return 0;
  if (utf8) {
    if (units >= size) return size;
    while (units > 0 && utf8_is_cont((unsigned char)s[units])) units--;
    return units;
  }
  while (i < size) {
    int n = utf8_char_bytes(s, size, i);
    int w = (n == 4) ? 2 : 1;
    if (u + w > units) break;
    u += w;
    i += n;
  }
  return i;
}

/* ---- file names and URIs ------------------------------------------------- */

static int uri_safe(unsigned char c) {
  return isalnum(c) || c == '/' || c == '-' || c == '.' || c == '_' || c == '~';
}

/* The absolute path a buffer's name means. A file that does not exist yet --
 * a new buffer saved under a name in a directory that does -- cannot be
 * resolved by realpath(), so it is the working directory joined on. */
static char *abs_path(const char *path) {
  char *real = realpath(path, NULL);
  char cwd[4096];
  char *out;
  size_t n;
  if (real != NULL) {
    /* realpath() allocates with malloc, which the allocation-failure sweep
     * cannot see; copy it so every allocation here is one molly made. */
    out = xstrdup(real);
    free(real);
    return out;
  }
  if (path[0] == '/') return xstrdup(path);
  if (getcwd(cwd, sizeof(cwd)) == NULL) return xstrdup(path);
  n = strlen(cwd) + strlen(path) + 2;
  out = xmalloc(n);
  snprintf(out, n, "%s/%s", cwd, path);
  return out;
}

char *lsp_path_to_uri(const char *path) {
  char *abs = abs_path(path);
  size_t n = strlen(abs), i, o = 0;
  char *uri = xmalloc(7 + n * 3 + 1);
  memcpy(uri, "file://", 7);
  o = 7;
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char)abs[i];
    if (uri_safe(c)) {
      uri[o++] = (char)c;
    } else {
      static const char hex[] = "0123456789ABCDEF";
      uri[o++] = '%';
      uri[o++] = hex[c >> 4];
      uri[o++] = hex[c & 15];
    }
  }
  uri[o] = '\0';
  free(abs);
  return uri;
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* The path a file: URI names, or NULL for any other scheme. */
char *lsp_uri_to_path(const char *uri) {
  size_t n, i, o = 0;
  char *path;
  if (uri == NULL || strncmp(uri, "file://", 7) != 0) return NULL;
  uri += 7;
  /* "file://host/path": only an empty host or localhost is this machine. */
  if (strncmp(uri, "localhost/", 10) == 0) uri += 9;
  if (uri[0] != '/') return NULL;
  n = strlen(uri);
  path = xmalloc(n + 1);
  for (i = 0; i < n; i++) {
    if (uri[i] == '%' && i + 2 < n + 1 && hexval(uri[i + 1]) >= 0 &&
        hexval(uri[i + 2]) >= 0) {
      path[o++] = (char)(hexval(uri[i + 1]) * 16 + hexval(uri[i + 2]));
      i += 2;
    } else {
      path[o++] = uri[i];
    }
  }
  path[o] = '\0';
  return path;
}

/* ---- the pipes ----------------------------------------------------------- */

static void drop(int *fd, int *id) {
  if (*id >= 0) {
    loop_remove(*id);
    *id = -1;
  }
  if (*fd >= 0) {
    close(*fd);
    *fd = -1;
  }
}

static void on_writable(void *user);
static void on_readable(void *user);
static void on_stderr(void *user);

static int slot_of(const struct server *s) { return (int)(s - servers); }

/* Every buffer that was open with this server forgets it, and its diagnostics,
 * which were the server's opinion and are no longer anyone's. */
static void forget_server(struct server *s) {
  int i, slot = slot_of(s) + 1;
  for (i = 0; i < buf_count(); i++) {
    struct buffer *b = buf_at(i);
    if (b->lsp_srv != slot) continue;
    b->lsp_srv = 0;
    lsp_diag_clear(b);
  }
  s->npend = 0;
}

/* Close molly's ends and make sure the process is gone: a short grace to exit
 * on its own, then SIGTERM, then SIGKILL, to the whole group. Same shape as
 * filter.c's, for the same reason -- it must not be able to hang the editor. */
static void reap(struct server *s, int grace_ms) {
  long start;
  int sent_term = 0;
  /* Its input closes first, which is end of file and for most servers reason
   * enough to go. Its output stays open until it has: a server answering the
   * `shutdown` it was just sent into a pipe nobody holds dies of the broken
   * pipe before it reads the `exit` behind it. Nobody reads what it says now;
   * an answer that fills the pipe is what the grace period's SIGTERM is for. */
  drop(&s->in_fd, &s->in_id);
  if (s->out_id >= 0) {
    loop_remove(s->out_id);
    s->out_id = -1;
  }
  if (s->err_id >= 0) {
    loop_remove(s->err_id);
    s->err_id = -1;
  }
  if (s->pid <= 0) {
    drop(&s->out_fd, &s->out_id);
    drop(&s->err_fd, &s->err_id);
    return;
  }
  start = now_ms();
  for (;;) {
    int st;
    pid_t got = waitpid(s->pid, &st, WNOHANG);
    if (got == s->pid || (got < 0 && errno != EINTR)) break;
    if (now_ms() - start >= grace_ms && !sent_term) {
      kill(-s->pid, SIGTERM);
      sent_term = 1;
    } else if (now_ms() - start >= grace_ms + LSP_EXIT_GRACE_MS) {
      kill(-s->pid, SIGKILL);
    }
    poll(NULL, 0, 10);
  }
  s->pid = 0;
  drop(&s->out_fd, &s->out_id);
  drop(&s->err_fd, &s->err_id);
}

/* It exited, or said something molly cannot read. Either way it is done; the
 * user hears about it once, with its last words when it left any. */
static void server_died(struct server *s, const char *why) {
  if (s->state == SRV_DEAD || s->state == SRV_FREE) return;
  reap(s, 0);
  s->state = SRV_DEAD;
  forget_server(s);
  if (s->lasterr[0] != '\0')
    editor_set_status_message("%s %s: %s", s->name, why, s->lasterr);
  else
    editor_set_status_message("%s %s", s->name, why);
  ui_request_redraw();
}

/* Write what is queued, as much as the pipe takes. */
static void flush(struct server *s) {
  while (s->in_fd >= 0 && s->wdone < s->wq.len) {
    ssize_t n = write(s->in_fd, s->wq.b + s->wdone, (size_t)(s->wq.len - s->wdone));
    if (n > 0) {
      s->wdone += (int)n;
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    server_died(s, "stopped reading");
    return;
  }
  if (s->wdone >= s->wq.len) {
    s->wq.len = 0;
    s->wdone = 0;
    if (s->in_id >= 0) {
      loop_remove(s->in_id);
      s->in_id = -1;
    }
  } else if (s->in_id < 0 && s->in_fd >= 0) {
    /* Waiting on a full pipe is the event loop's job: registered only while
     * there is something to write, because an empty pipe is always writable
     * and would wake the loop on every pass. */
    s->in_id = loop_add(s->in_fd, LOOP_WRITE, on_writable, s);
    if (s->in_id < 0) server_died(s, "could not be waited on");
  }
}

static void on_writable(void *user) { flush(user); }

/* Frame a message and send it. */
static void send_body(struct server *s, const struct abuf *body) {
  char hdr[48];
  int n;
  if (s->in_fd < 0) return;
  n = snprintf(hdr, sizeof(hdr), "Content-Length: %d\r\n\r\n", body->len);
  ab_append(&s->wq, hdr, n);
  ab_append(&s->wq, body->b, body->len);
  flush(s);
}

/* A notification: a method and its params, which the caller has written as
 * JSON already. */
static void notify(struct server *s, const char *method, const struct abuf *params) {
  struct abuf ab = ABUF_INIT;
  json_putf(&ab, "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":", method);
  if (params != NULL && params->len > 0) ab_append(&ab, params->b, params->len);
  else ab_append(&ab, "{}", 2);
  ab_append(&ab, "}", 1);
  send_body(s, &ab);
  free(ab.b);
}

/* A request. Returns its id, or -1 when there is no room to remember it -- a
 * server that has sixteen questions unanswered is not going to answer this one
 * promptly either. */
static int request(struct server *s, const char *method, const struct abuf *params,
                   int kind, struct buffer *b) {
  struct abuf ab = ABUF_INIT;
  struct pending *p;
  int id;
  if (s->npend == LSP_MAX_PENDING) {
    /* The oldest goes: its answer will be ignored when it comes. */
    memmove(&s->pend[0], &s->pend[1], sizeof(s->pend[0]) * (LSP_MAX_PENDING - 1));
    s->npend--;
  }
  id = ++s->next_id;
  p = &s->pend[s->npend++];
  memset(p, 0, sizeof(*p));
  p->id = id;
  p->kind = kind;
  p->buf = b;
  if (b != NULL) p->edits = b->edits;
  json_putf(&ab, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":",
            id, method);
  if (params != NULL && params->len > 0) ab_append(&ab, params->b, params->len);
  else ab_append(&ab, "{}", 2);
  ab_append(&ab, "}", 1);
  send_body(s, &ab);
  free(ab.b);
  return id;
}

/* A request of the server's own. Molly offers none of the capabilities those
 * are about, so the answer is "nothing" -- except workspace/configuration,
 * which wants one answer per item asked about, and gets that many nulls, and
 * workspace/applyEdit, which a command sends to make its changes. Those go to
 * the mailbox like an answer, and are said to have been applied: molly applies
 * an edit whole or refuses it before touching anything, and a refusal is said
 * on the message bar where the user will see it. */
static void reply(struct server *s, const struct json *id, const char *method,
                  const struct json *params) {
  struct abuf ab = ABUF_INIT;
  ab_append(&ab, "{\"jsonrpc\":\"2.0\",\"id\":", 22);
  if (id->type == JSON_STRING) json_put_str(&ab, id->str, id->n);
  else json_putf(&ab, "%d", json_int(id, 0));
  if (strcmp(method, "workspace/applyEdit") == 0) {
    lsp_answer(LSP_REQ_APPLY, NULL, 0, 0, 0, json_get(params, "edit"), s->utf8);
    ab_append(&ab, ",\"result\":{\"applied\":true}}", 27);
  } else if (strcmp(method, "workspace/configuration") == 0) {
    const struct json *items = json_get(params, "items");
    int i, n = (items != NULL && items->type == JSON_ARRAY) ? items->n : 0;
    ab_append(&ab, ",\"result\":[", 11);
    for (i = 0; i < n; i++) ab_append(&ab, i ? ",null" : "null", i ? 5 : 4);
    ab_append(&ab, "]}", 2);
  } else {
    ab_append(&ab, ",\"result\":null}", 15);
  }
  send_body(s, &ab);
  free(ab.b);
}

/* ---- starting one -------------------------------------------------------- */

/* Is the program a command line starts with installed? A server named by a
 * built-in definition is tried for everyone who opens that kind of file, and
 * most people do not have most servers: that has to be silence, not an error
 * each time a file is opened. So the program is looked for before anything is
 * started, and a missing one is remembered as missing. */
static int program_exists(const char *cmd, char *name, size_t namesz) {
  char prog[256];
  const char *p = cmd, *path;
  size_t n = 0;

  while (*p == ' ' || *p == '\t') p++;
  /* Leading NAME=value assignments are the shell's, not the program. */
  for (;;) {
    const char *q = p;
    while (*q != '\0' && *q != ' ' && *q != '\t' && *q != '=') q++;
    if (*q != '=') break;
    while (*q != '\0' && *q != ' ' && *q != '\t') q++;
    while (*q == ' ' || *q == '\t') q++;
    p = q;
  }
  while (p[n] != '\0' && p[n] != ' ' && p[n] != '\t' && n + 1 < sizeof(prog)) {
    prog[n] = p[n];
    n++;
  }
  prog[n] = '\0';
  if (n == 0) return 0;
  {
    /* Cut to fit by hand: it is a name for messages, and a long one losing
     * its tail is the intent rather than an accident to be warned about. */
    const char *base = strrchr(prog, '/');
    size_t bl;
    base = base != NULL ? base + 1 : prog;
    bl = strlen(base);
    if (bl >= namesz) bl = namesz - 1;
    memcpy(name, base, bl);
    name[bl] = '\0';
  }
  if (strchr(prog, '/') != NULL) return access(prog, X_OK) == 0;

  path = getenv("PATH");
  if (path == NULL) path = "/usr/bin:/bin";
  while (*path != '\0') {
    char full[4096];
    const char *end = strchr(path, ':');
    size_t dl = end != NULL ? (size_t)(end - path) : strlen(path);
    if (dl == 0) snprintf(full, sizeof(full), "./%s", prog);
    else snprintf(full, sizeof(full), "%.*s/%s", (int)dl, path, prog);
    if (access(full, X_OK) == 0) return 1;
    if (end == NULL) break;
    path = end + 1;
  }
  return 0;
}

static void send_initialize(struct server *s) {
  struct abuf ab = ABUF_INIT;
  char cwd[4096];
  char *uri;
  const char *base;

  snprintf(cwd, sizeof(cwd), "%s", s->root);
  uri = lsp_path_to_uri(cwd);
  base = strrchr(cwd, '/');
  base = (base != NULL && base[1] != '\0') ? base + 1 : cwd;

  json_putf(&ab, "{\"processId\":%d,\"clientInfo\":{\"name\":\"molly\","
                 "\"version\":\"%s\"},\"rootUri\":", (int)getpid(), MOLLY_VERSION);
  json_put_str(&ab, uri, (int)strlen(uri));
  ab_append(&ab, ",\"workspaceFolders\":[{\"uri\":", 28);
  json_put_str(&ab, uri, (int)strlen(uri));
  ab_append(&ab, ",\"name\":", 8);
  json_put_str(&ab, base, (int)strlen(base));
  ab_append(&ab, "}],", 3);
  /* What molly can take. UTF-8 positions first, so most servers never make it
   * convert; `offsetEncoding` is the same offer in the form clangd read before
   * the protocol had one. No snippets: a completion that inserts `$1` into the
   * text is worse than one that inserts the plain name. */
  json_putf(&ab,
      "\"capabilities\":{"
        "\"general\":{\"positionEncodings\":[\"utf-8\",\"utf-16\"]},"
        "\"offsetEncoding\":[\"utf-8\",\"utf-16\"],"
        "\"textDocument\":{"
          "\"synchronization\":{\"didSave\":true},"
          "\"publishDiagnostics\":{},"
          "\"hover\":{\"contentFormat\":[\"plaintext\",\"markdown\"]},"
          "\"definition\":{\"linkSupport\":true},"
          "\"completion\":{\"completionItem\":{\"snippetSupport\":false},"
                         "\"contextSupport\":true},"
          "\"rename\":{\"prepareSupport\":false},"
          "\"references\":{},"
          "\"signatureHelp\":{\"signatureInformation\":{"
            "\"documentationFormat\":[\"plaintext\",\"markdown\"],"
            "\"parameterInformation\":{\"labelOffsetSupport\":true}}},"
          "\"formatting\":{},\"rangeFormatting\":{},"
          /* Literals: a list of CodeActions rather than bare Commands, and
           * the edit of one filled in when it is chosen, which is how the
           * servers that compute it lazily want to be asked. */
          "\"codeAction\":{\"codeActionLiteralSupport\":{\"codeActionKind\":{"
            "\"valueSet\":[\"\",\"quickfix\",\"refactor\",\"refactor.extract\","
            "\"refactor.inline\",\"refactor.rewrite\",\"source\","
            "\"source.organizeImports\"]}},"
            "\"resolveSupport\":{\"properties\":[\"edit\"]}}"
        "},"
        /* Edits to text only: a rename that wants to create, move or delete
         * files is declined rather than half done. */
        "\"workspace\":{\"workspaceFolders\":true,\"configuration\":true,"
                      "\"applyEdit\":true,"
                      "\"workspaceEdit\":{\"documentChanges\":true}}"
      "}}");
  free(uri);
  request(s, "initialize", &ab, LSP_REQ_INIT, NULL);
  free(ab.b);
}

static int start(struct server *s) {
  int inp[2], outp[2], errp[2];
  pid_t pid;
  char *line;
  size_t n;

  if (!program_exists(s->cmd, s->name, sizeof(s->name))) {
    s->state = SRV_MISSING;
    return 0;
  }

  /* Writing to a server that has died raises SIGPIPE, whose default is to kill
   * the writer -- molly. Ignored for good once a server exists: filter.c sets
   * and restores it around a run, which restores to this. */
  signal(SIGPIPE, SIG_IGN);

  /* Close-on-exec, so a second server or a filter command started later does
   * not inherit this one's pipes. A child holding a copy of the write end of a
   * server's input would keep it from ever seeing end of file. */
  if (pipe2(inp, O_CLOEXEC) != 0) goto fail0;
  if (pipe2(outp, O_CLOEXEC) != 0) goto fail1;
  if (pipe2(errp, O_CLOEXEC) != 0) goto fail2;

  /* `exec`, so the shell becomes the server rather than waiting on it, and the
   * pid molly holds is the server's own. Through a shell at all because the
   * line is the user's, from their configuration: arguments, a variable in
   * front, a path with ~ in it. */
  n = strlen(s->cmd) + 6;
  line = xmalloc(n);
  snprintf(line, n, "exec %s", s->cmd);

  pid = fork();
  if (pid < 0) {
    free(line);
    goto fail3;
  }
  if (pid == 0) {
    setpgid(0, 0);
    signal(SIGPIPE, SIG_DFL);
    dup2(inp[0], STDIN_FILENO);
    dup2(outp[1], STDOUT_FILENO);
    dup2(errp[1], STDERR_FILENO);
    execl("/bin/sh", "sh", "-c", line, (char *)NULL);
    _exit(127);
  }
  free(line);
  setpgid(pid, pid);
  close(inp[0]);
  close(outp[1]);
  close(errp[1]);

  s->pid = pid;
  s->in_fd = inp[1];
  s->out_fd = outp[0];
  s->err_fd = errp[0];
  s->in_id = s->out_id = s->err_id = -1;
  fcntl(s->in_fd, F_SETFL, fcntl(s->in_fd, F_GETFL, 0) | O_NONBLOCK);
  fcntl(s->out_fd, F_SETFL, fcntl(s->out_fd, F_GETFL, 0) | O_NONBLOCK);
  fcntl(s->err_fd, F_SETFL, fcntl(s->err_fd, F_GETFL, 0) | O_NONBLOCK);
  s->state = SRV_STARTING;
  s->out_id = loop_add(s->out_fd, LOOP_READ, on_readable, s);
  s->err_id = loop_add(s->err_fd, LOOP_READ, on_stderr, s);
  if (s->out_id < 0 || s->err_id < 0) {
    server_died(s, "could not be waited on");
    return 0;
  }
  send_initialize(s);
  return 1;

fail3:
  close(errp[0]); close(errp[1]);
fail2:
  close(outp[0]); close(outp[1]);
fail1:
  close(inp[0]); close(inp[1]);
fail0:
  s->state = SRV_DEAD;
  snprintf(s->lasterr, sizeof(s->lasterr), "%s", strerror(errno));
  editor_set_status_message("%s could not start: %s", s->name, s->lasterr);
  return 0;
}

/* ---- the project ---------------------------------------------------------
 *
 * A server is started for a project, not for molly's working directory: it is
 * told where the project's root is, and reads its build files from there. So a
 * file opened from elsewhere -- `molly ~/src/other/main.c` -- gets a server of
 * its own, rooted in its own project, rather than one that thinks the current
 * directory is the project and cannot find a header.
 *
 * The root is the nearest directory up from the file with a build file or a
 * compilation database in it; failing that, the nearest repository; failing
 * that, the working directory, which is what it always was. Nearest rather
 * than outermost for the build files, because a repository holding several
 * projects has one per project, and each is what its server wants. */
static const char *const root_markers[] = {
  "compile_commands.json", "compile_flags.txt", "Cargo.toml", "go.mod",
  "pyproject.toml", "setup.py", "setup.cfg", "package.json", "tsconfig.json",
  "deno.json", "build.zig", "Gemfile", "pom.xml", "build.gradle", "CMakeLists.txt",
  "meson.build", "Makefile",
};

#define ROOT_CACHE 32
static struct { char *dir, *root; } root_cache[ROOT_CACHE];
static int root_next;

static int has(const char *dir, const char *name) {
  char p[4352];
  snprintf(p, sizeof(p), "%s/%s", dir, name);
  return access(p, F_OK) == 0;
}

/* The root for a file, which the caller must not free: it lives in the cache. */
static const char *project_root(const char *filename) {
  char dir[4096], found[4096] = "", repo[4096] = "";
  char *slash;
  int i, k;
  if (filename[0] == '/') {
    snprintf(dir, sizeof(dir), "%s", filename);
  } else {
    char cwd[2048];
    if (getcwd(cwd, sizeof(cwd)) == NULL) snprintf(cwd, sizeof(cwd), "/");
    snprintf(dir, sizeof(dir), "%s/%s", cwd, filename);
  }
  slash = strrchr(dir, '/');
  if (slash == dir) dir[1] = '\0';
  else if (slash != NULL) *slash = '\0';

  for (i = 0; i < ROOT_CACHE; i++)
    if (root_cache[i].dir != NULL && strcmp(root_cache[i].dir, dir) == 0)
      return root_cache[i].root;

  {
    char at[4096];
    snprintf(at, sizeof(at), "%s", dir);
    for (;;) {
      for (k = 0; found[0] == '\0' && k < (int)(sizeof(root_markers) / sizeof(*root_markers)); k++)
        if (has(at, root_markers[k])) snprintf(found, sizeof(found), "%s", at);
      if (found[0] != '\0') break;
      if (repo[0] == '\0' && (has(at, ".git") || has(at, ".hg")))
        snprintf(repo, sizeof(repo), "%s", at);
      slash = strrchr(at, '/');
      if (slash == NULL || at[1] == '\0') break;
      if (slash == at) at[1] = '\0';
      else *slash = '\0';
    }
  }
  if (found[0] == '\0' && repo[0] != '\0') snprintf(found, sizeof(found), "%s", repo);
  if (found[0] == '\0' && getcwd(found, sizeof(found)) == NULL) snprintf(found, sizeof(found), "/");

  i = root_next;
  root_next = (root_next + 1) % ROOT_CACHE;
  free(root_cache[i].dir);
  free(root_cache[i].root);
  root_cache[i].dir = xstrdup(dir);
  root_cache[i].root = xstrdup(found);
  return root_cache[i].root;
}

/* The server for a command line in a project, if it has one already. */
static struct server *server_find(const char *cmd, const char *root) {
  int i;
  for (i = 0; i < LSP_MAX_SERVERS; i++)
    if (servers[i].state != SRV_FREE && strcmp(servers[i].cmd, cmd) == 0 &&
        strcmp(servers[i].root, root) == 0)
      return &servers[i];
  return NULL;
}

/* The server for a command line in a project, started if it is new. */
static struct server *server_get(const char *cmd, const char *root) {
  int i, freeslot = -1;
  struct server *s = server_find(cmd, root);
  if (s != NULL) return s;
  for (i = 0; i < LSP_MAX_SERVERS && freeslot < 0; i++)
    if (servers[i].state == SRV_FREE) freeslot = i;
  if (freeslot < 0) return NULL;
  {
    struct server *s = &servers[freeslot];
    memset(s, 0, sizeof(*s));
    s->cmd = xstrdup(cmd);
    s->root = xstrdup(root);
    s->in_fd = s->out_fd = s->err_fd = -1;
    s->in_id = s->out_id = s->err_id = -1;
    start(s);
    return s;
  }
}

/* ---- documents ----------------------------------------------------------- */

void lsp_diag_clear(struct buffer *b) {
  int i;
  if (b == NULL) return;
  if (b->ndiag > 0 && !b->problems) lspui_problems_changed();
  for (i = 0; i < b->ndiag; i++) free(b->diag[i].msg);
  free(b->diag);
  b->diag = NULL;
  b->ndiag = 0;
}

/* The buffer's text as the server should see it: rows joined by newlines, and
 * one at the end when the file has one. Always "\n", whatever the file is
 * saved with -- positions are lines and columns, which a CR would only add a
 * column to. -1 when it is too big to send. */
static int doc_text(struct buffer *b, struct abuf *ab) {
  int i;
  size_t total = 0;
  for (i = 0; i < b->numrows; i++) total += (size_t)buf_row(b, i)->size + 1;
  if (total > LSP_MAX_DOC) return -1;
  ab->b = xmalloc(total + 1);
  ab->cap = (int)total + 1;
  ab->len = 0;
  for (i = 0; i < b->numrows; i++) {
    erow *r = buf_row(b, i);
    memcpy(ab->b + ab->len, r->chars, (size_t)r->size);
    ab->len += r->size;
    if (i + 1 < b->numrows || b->final_newline) ab->b[ab->len++] = '\n';
  }
  return 0;
}

static void text_document(struct abuf *ab, const struct buffer *b) {
  ab_append(ab, "{\"textDocument\":{\"uri\":", 23);
  json_put_str(ab, b->lsp_uri, (int)strlen(b->lsp_uri));
}

static void doc_forget(struct buffer *b) {
  free(b->lsp_uri);
  b->lsp_uri = NULL;
  free(b->lsp_name);
  b->lsp_name = NULL;
  b->lsp_srv = 0;
  b->lsp_version = 0;
}

static void doc_close(struct buffer *b) {
  if (b->lsp_srv > 0) {
    struct server *s = &servers[b->lsp_srv - 1];
    if (s->state == SRV_READY && b->lsp_uri != NULL) {
      struct abuf ab = ABUF_INIT;
      text_document(&ab, b);
      ab_append(&ab, "}}", 2);
      notify(s, "textDocument/didClose", &ab);
      free(ab.b);
    }
  }
  doc_forget(b);
  lsp_diag_clear(b);
}

static void doc_open(struct buffer *b, struct server *s) {
  struct abuf text = ABUF_INIT, ab = ABUF_INIT;
  const char *lang = syntax_lang_id(b->syn);

  free(b->lsp_name);
  b->lsp_name = xstrdup(b->filename);
  if (doc_text(b, &text) < 0) {
    /* Declined, and remembered as declined until the name changes, so the
     * sweep does not measure a huge file again every tick. */
    b->lsp_srv = -1;
    return;
  }
  free(b->lsp_uri);
  b->lsp_uri = lsp_path_to_uri(b->filename);
  b->lsp_srv = slot_of(s) + 1;
  b->lsp_version = 1;
  b->lsp_sent = b->edits;

  text_document(&ab, b);
  ab_append(&ab, ",\"languageId\":", 14);
  json_put_str(&ab, lang, (int)strlen(lang));
  ab_append(&ab, ",\"version\":1,\"text\":", 20);
  json_put_str(&ab, text.b, text.len);
  ab_append(&ab, "}}", 2);
  notify(s, "textDocument/didOpen", &ab);
  free(ab.b);
  free(text.b);
}

static void doc_change(struct buffer *b, struct server *s) {
  struct abuf text = ABUF_INIT, ab = ABUF_INIT;
  b->lsp_sent = b->edits;
  if (s->sync == 0) return;
  if (doc_text(b, &text) < 0) {
    /* It has grown past what molly sends. The server keeps the last version it
     * was given; closing would throw away diagnostics that are mostly still
     * right. */
    return;
  }
  b->lsp_version++;
  text_document(&ab, b);
  json_putf(&ab, ",\"version\":%d},\"contentChanges\":[{\"text\":", b->lsp_version);
  json_put_str(&ab, text.b, text.len);
  ab_append(&ab, "}]}", 3);
  notify(s, "textDocument/didChange", &ab);
  free(ab.b);
  free(text.b);
}

/* ---- asking first ----------------------------------------------------------
 *
 * A server reads the project it is started for, and rust-analyzer builds it,
 * which runs the project's own code: opening a file is not consent to that. So
 * a server is started in a project only once the user has said yes to it there
 * (trust.c keeps the answers). A server that is not installed is not asked
 * about: there is nothing to start.
 *
 * Asked from the main loop and not here, because here is inside the event loop
 * -- every quiet tick, under whatever menu, dialog or prompt is open -- and a
 * question cannot open inside another one. One question at a time: a second
 * project waits its turn, and the sweep finds it again once the first is
 * answered. */
static struct {
  int pending;
  char *cmd, *root;
  char name[32];
} trust_q;

static void trust_q_clear(void) {
  free(trust_q.cmd);
  free(trust_q.root);
  memset(&trust_q, 0, sizeof(trust_q));
}

/* Which server a buffer should be open with, or NULL. Starts it if need be,
 * when the project has been said yes to; asks, when nobody has been asked. */
static struct server *server_for(struct buffer *b) {
  const char *cmd, *root;
  struct server *s;
  int trusted;
  if (!lsp_enabled() || b->filename == NULL || b->syn == NULL || b->grep_results ||
      b->problems)
    return NULL;
  cmd = syntax_lsp(b->syn);
  if (cmd == NULL || cmd[0] == '\0') return NULL;
  root = project_root(b->filename);
  if ((s = server_find(cmd, root)) != NULL) return s;
  trusted = trust_lookup(root);
  if (trusted == 0) return NULL;
  if (trusted < 0) {
    char name[32];
    /* Not installed: started, which finds it missing and remembers that. */
    if (!program_exists(cmd, name, sizeof(name))) return server_get(cmd, root);
    if (!trust_q.pending) {
      trust_q.pending = 1;
      trust_q.cmd = xstrdup(cmd);
      trust_q.root = xstrdup(root);
      snprintf(trust_q.name, sizeof(trust_q.name), "%s", name);
      ui_request_redraw();   /* wakes the main loop, which asks */
    }
    return NULL;
  }
  return server_get(cmd, root);
}

/* From the main loop: the question a server is waiting on, if there is one.
 * Anything but yes is no, and kept, as Enter on (y/N) means; Restart language
 * servers asks again. */
void lsp_trust_ask(void) {
  char prompt[200], where[64];
  const char *root, *home = getenv("HOME");
  size_t hl = home != NULL ? strlen(home) : 0;
  char *ans;
  int yes;
  if (!trust_q.pending) return;
  if (!lsp_enabled() || trust_lookup(trust_q.root) >= 0) {
    trust_q_clear();
    return;
  }
  /* The project as ~/..., and the end of it when it is long: the end is the
   * part that says which. */
  root = trust_q.root;
  if (hl > 1 && strncmp(root, home, hl) == 0 && (root[hl] == '/' || root[hl] == '\0'))
    snprintf(where, sizeof(where), "~%s", root + hl);
  else
    snprintf(where, sizeof(where), "%s", root);
  if (strlen(root) > 32 && utf8_str_cols(where) > 32) {
    const char *tail = root + strlen(root) - 30;
    while (*tail != '\0' && utf8_is_cont((unsigned char)*tail)) tail++;
    snprintf(where, sizeof(where), "\xe2\x80\xa6%s", tail);
  }
  snprintf(prompt, sizeof(prompt),
           "Start %s for %s? It may run the project's code. (y/N): %%s",
           trust_q.name, where);
  ans = editor_prompt(prompt, NULL);
  yes = ans != NULL && (ans[0] == 'y' || ans[0] == 'Y');
  free(ans);
  trust_set(trust_q.root, yes);
  if (yes) {
    editor_set_status_message("Starting %s for %s.", trust_q.name, where);
    trust_q_clear();
    lsp_idle();
  } else {
    editor_set_status_message("%s not started for %s. Restart language servers to be "
                              "asked again.", trust_q.name, where);
    trust_q_clear();
  }
}

/* Bring one buffer's server up to date with it: open, change or close. */
static void sync_buffer(struct buffer *b) {
  struct server *want = server_for(b);
  int renamed = b->lsp_srv != 0 &&
      (b->lsp_name == NULL || b->filename == NULL ||
       strcmp(b->lsp_name, b->filename) != 0);

  if (b->lsp_srv == -1 && !renamed && want != NULL) return;  /* declined */
  if (b->lsp_srv != 0 &&
      (renamed || want == NULL || b->lsp_srv != slot_of(want) + 1))
    doc_close(b);
  if (want == NULL || want->state != SRV_READY) return;
  if (b->lsp_srv == 0) doc_open(b, want);
  else if (b->lsp_srv > 0 && b->edits != b->lsp_sent) doc_change(b, want);
}

void lsp_idle(void) {
  int i;
  for (i = 0; i < buf_count(); i++) sync_buffer(buf_at(i));
}

void lsp_buffer_saved(struct buffer *b) {
  struct server *s;
  struct abuf ab = ABUF_INIT;
  sync_buffer(b);
  if (b->lsp_srv <= 0) return;
  s = &servers[b->lsp_srv - 1];
  if (s->state != SRV_READY || !s->save) return;
  text_document(&ab, b);
  ab_append(&ab, "}}", 2);
  notify(s, "textDocument/didSave", &ab);
  free(ab.b);
}

/* A buffer is being freed. Anything still asked about it is forgotten, so an
 * answer arriving later cannot reach freed memory. */
void lsp_buffer_closed(struct buffer *b) {
  int i, k;
  doc_close(b);
  lspui_forget(b);
  for (i = 0; i < LSP_MAX_SERVERS; i++)
    for (k = 0; k < servers[i].npend; k++)
      if (servers[i].pend[k].buf == b) servers[i].pend[k].buf = NULL;
}

/* Rows were inserted (delta > 0) or one deleted (delta == -1) at `at`. The
 * diagnostics below move with their text, so they stay on the right line
 * until the server's next opinion arrives; one on a deleted row goes. */
void lsp_rows_moved(struct buffer *b, int at, int delta) {
  int i, o = 0;
  if (b == NULL || b->ndiag == 0) return;
  for (i = 0; i < b->ndiag; i++) {
    struct diag *d = &b->diag[i];
    if (delta > 0) {
      if (d->y0 >= at) d->y0 += delta;
      if (d->y1 >= at) d->y1 += delta;
    } else {
      if (d->y0 == at && d->y1 == at) {
        free(d->msg);
        continue;
      }
      if (d->y0 > at) d->y0--;
      if (d->y1 >= at && d->y1 > d->y0) d->y1--;
    }
    b->diag[o++] = *d;
  }
  b->ndiag = o;
}

/* ---- what comes back ----------------------------------------------------- */

/* A position, as a row and a byte offset into the buffer as it is now. */
static void position(const struct server *s, const struct buffer *b,
                     const struct json *pos, int *y, int *x) {
  int line = json_int(json_get(pos, "line"), 0);
  int ch = json_int(json_get(pos, "character"), 0);
  erow *r;
  if (line < 0) line = 0;
  if (b->numrows == 0) {
    *y = 0;
    *x = 0;
    return;
  }
  if (line >= b->numrows) {
    *y = b->numrows - 1;
    *x = buf_row(b, *y)->size;
    return;
  }
  r = buf_row(b, line);
  *y = line;
  *x = lsp_units_to_col(r->chars, r->size, ch, s->utf8);
}

static struct buffer *buffer_for_uri(const char *uri) {
  int i;
  if (uri == NULL) return NULL;
  for (i = 0; i < buf_count(); i++) {
    struct buffer *b = buf_at(i);
    if (b->lsp_uri != NULL && strcmp(b->lsp_uri, uri) == 0) return b;
  }
  return NULL;
}

static int diag_cmp(const void *a, const void *b) {
  const struct diag *p = a, *q = b;
  if (p->y0 != q->y0) return p->y0 < q->y0 ? -1 : 1;
  if (p->x0 != q->x0) return p->x0 < q->x0 ? -1 : 1;
  return p->severity - q->severity;
}

static void on_diagnostics(struct server *s, const struct json *params) {
  struct buffer *b = buffer_for_uri(json_str(json_get(params, "uri")));
  const struct json *list = json_get(params, "diagnostics"), *d;
  int n = 0;

  if (b == NULL || b->lsp_srv != slot_of(s) + 1) return;
  lsp_diag_clear(b);
  if (list == NULL || list->type != JSON_ARRAY || list->n == 0) {
    ui_request_redraw();
    return;
  }
  b->diag = xmalloc(sizeof(*b->diag) * (size_t)list->n);
  for (d = list->kid; d != NULL; d = d->next) {
    const struct json *range = json_get(d, "range");
    const char *msg = json_str(json_get(d, "message"));
    struct diag *out = &b->diag[n];
    if (range == NULL || msg == NULL) continue;
    position(s, b, json_get(range, "start"), &out->y0, &out->x0);
    position(s, b, json_get(range, "end"), &out->y1, &out->x1);
    if (out->y1 < out->y0 || (out->y1 == out->y0 && out->x1 < out->x0)) {
      out->y1 = out->y0;
      out->x1 = out->x0;
    }
    /* Severity is optional, and the protocol leaves its absence to the client:
     * an unmarked diagnostic is shown as an error, which is what servers that
     * leave it out mean in practice. */
    out->severity = json_int(json_get(d, "severity"), 1);
    if (out->severity < 1 || out->severity > 4) out->severity = 1;
    out->msg = xstrdup(msg);
    n++;
  }
  b->ndiag = n;
  qsort(b->diag, (size_t)n, sizeof(*b->diag), diag_cmp);
  lspui_problems_changed();
  ui_request_redraw();
}

/* A capability is on when it is `true` or an object of options, and off when
 * it is missing or `false`. */
static int provided(const struct json *v) {
  return v != NULL && (v->type == JSON_TRUE || v->type == JSON_OBJECT);
}

static void on_initialized(struct server *s, const struct json *result) {
  const struct json *caps = json_get(result, "capabilities");
  const struct json *sync = json_get(caps, "textDocumentSync"), *t;
  const char *enc = json_str(json_get(caps, "positionEncoding"));
  int n = 0;

  if (enc == NULL) enc = json_str(json_get(result, "offsetEncoding"));
  s->utf8 = enc != NULL && strcmp(enc, "utf-8") == 0;

  /* A number, or an object with `change` in it; and `save`, which is either a
   * boolean or an object (whose includeText molly does not need to honor,
   * having sent the text already). Absent means None. */
  if (sync != NULL && sync->type == JSON_NUMBER) {
    s->sync = json_int(sync, 0);
    s->save = 0;
  } else {
    s->sync = json_int(json_get(sync, "change"), 0);
    s->save = provided(json_get(sync, "save"));
  }
  s->hover = provided(json_get(caps, "hoverProvider"));
  s->definition = provided(json_get(caps, "definitionProvider"));
  s->completion = provided(json_get(caps, "completionProvider"));
  s->rename = provided(json_get(caps, "renameProvider"));
  s->references = provided(json_get(caps, "referencesProvider"));
  s->signature = provided(json_get(caps, "signatureHelpProvider"));
  s->format = provided(json_get(caps, "documentFormattingProvider"));
  s->format_range = provided(json_get(caps, "documentRangeFormattingProvider"));
  t = json_get(caps, "codeActionProvider");
  s->actions = provided(t);
  s->resolve = json_bool(json_get(t, "resolveProvider"), 0);
  {
    /* Triggers open it and retriggers keep it current; molly asks again on
     * every key while it is open anyway, so the two are one list here. */
    static const char *const lists[] = {"triggerCharacters", "retriggerCharacters"};
    int l, m = 0;
    for (l = 0; l < 2; l++) {
      const struct json *tl = json_get(json_get(caps, "signatureHelpProvider"), lists[l]);
      const struct json *k;
      if (tl == NULL || tl->type != JSON_ARRAY) continue;
      for (k = tl->kid; k != NULL; k = k->next)
        if (k->type == JSON_STRING && k->n == 1 && m + 1 < (int)sizeof(s->sig_triggers) &&
            strchr(s->sig_triggers, k->str[0]) == NULL) {
          s->sig_triggers[m++] = k->str[0];
          s->sig_triggers[m] = '\0';
        }
    }
  }
  t = json_get(json_get(caps, "completionProvider"), "triggerCharacters");
  if (t != NULL && t->type == JSON_ARRAY) {
    const struct json *k;
    for (k = t->kid; k != NULL; k = k->next)
      if (k->type == JSON_STRING && k->n == 1 && n + 1 < (int)sizeof(s->triggers))
        s->triggers[n++] = k->str[0];
  }
  s->triggers[n] = '\0';

  s->state = SRV_READY;
  notify(s, "initialized", NULL);
  /* Every buffer waiting on it is opened now rather than at the next quiet
   * tick, so the first diagnostics are not held up by a user who is typing. */
  lsp_idle();
}

static void on_response(struct server *s, const struct json *msg) {
  int id = json_int(json_get(msg, "id"), -1), k;
  struct pending p;
  const struct json *err = json_get(msg, "error");

  for (k = 0; k < s->npend; k++)
    if (s->pend[k].id == id) break;
  if (k == s->npend) return;  /* superseded, or never ours */
  p = s->pend[k];
  memmove(&s->pend[k], &s->pend[k + 1], sizeof(p) * (size_t)(s->npend - k - 1));
  s->npend--;

  if (p.kind == LSP_REQ_INIT) {
    if (err != NULL) {
      const char *m = json_str(json_get(err, "message"));
      snprintf(s->lasterr, sizeof(s->lasterr), "%s", m != NULL ? m : "refused");
      server_died(s, "would not start");
      return;
    }
    on_initialized(s, json_get(msg, "result"));
    return;
  }
  if (p.kind == LSP_REQ_SHUTDOWN || p.kind < 0 || p.buf == NULL) return;
  /* A command's answer says nothing molly needs -- its edits came as an
   * applyEdit just before it, which an answer in the mailbox would replace --
   * unless it is that the command failed. */
  if (p.kind == LSP_REQ_EXECUTE) {
    const char *m = json_str(json_get(err, "message"));
    if (err != NULL) {
      editor_set_status_message("%s: %s", s->name, m != NULL ? m : "the action failed");
      ui_request_redraw();
    }
    return;
  }
  /* An error is an answer of nothing: "no definition here" and "I could not
   * work that out" leave the user in the same place. */
  lsp_answer(p.kind, p.buf, p.edits, p.cy, p.cx,
             err != NULL ? NULL : json_get(msg, "result"), s->utf8);
}

static void dispatch(struct server *s, const char *body, size_t len) {
  struct json_doc *doc = json_parse(body, len);
  const struct json *msg = json_root(doc), *id;
  const char *method;

  if (msg == NULL) {
    /* One unreadable message is not worth losing the server over: it may be
     * the one notification molly did not need. */
    json_free(doc);
    return;
  }
  id = json_get(msg, "id");
  method = json_str(json_get(msg, "method"));
  if (method != NULL && id != NULL) {
    reply(s, id, method, json_get(msg, "params"));
  } else if (method != NULL) {
    if (strcmp(method, "textDocument/publishDiagnostics") == 0) {
      on_diagnostics(s, json_get(msg, "params"));
    } else if (strcmp(method, "window/showMessage") == 0) {
      /* Errors and warnings only: servers announce themselves at Info, and
       * the message bar is not their log. */
      const struct json *p = json_get(msg, "params");
      const char *m = json_str(json_get(p, "message"));
      if (m != NULL && json_int(json_get(p, "type"), 4) <= 2) {
        editor_set_status_message("%s: %s", s->name, m);
        ui_request_redraw();
      }
    }
  } else if (id != NULL) {
    on_response(s, msg);
  }
  json_free(doc);
}

/* Take whole messages off the front of what has been read. */
static void parse(struct server *s) {
  for (;;) {
    char *end, *p;
    long clen = -1;
    size_t hdr, total;

    if (s->rlen == 0) return;
    end = memmem(s->rb, s->rlen, "\r\n\r\n", 4);
    if (end == NULL) {
      if (s->rlen > 8192) server_died(s, "sent something that is not the protocol");
      return;
    }
    hdr = (size_t)(end - s->rb) + 4;
    /* Header lines, of which only Content-Length matters. Matched without
     * regard to case, as HTTP headers are. */
    for (p = s->rb; p < end;) {
      char *eol = memmem(p, (size_t)(end + 2 - p), "\r\n", 2);
      if (eol == NULL) break;
      if (eol - p > 15 && strncasecmp(p, "Content-Length:", 15) == 0) {
        char num[24];
        size_t nl = (size_t)(eol - p - 15);
        if (nl >= sizeof(num)) nl = sizeof(num) - 1;
        memcpy(num, p + 15, nl);
        num[nl] = '\0';
        clen = strtol(num, NULL, 10);
      }
      p = eol + 2;
    }
    if (clen < 0 || clen > LSP_MAX_MESSAGE) {
      server_died(s, "sent something that is not the protocol");
      return;
    }
    total = hdr + (size_t)clen;
    if (s->rlen < total) return;
    dispatch(s, s->rb + hdr, (size_t)clen);
    if (s->state == SRV_DEAD || s->rb == NULL) return;
    memmove(s->rb, s->rb + total, s->rlen - total);
    s->rlen -= total;
  }
}

static void on_readable(void *user) {
  struct server *s = user;
  for (;;) {
    ssize_t n;
    if (s->rcap - s->rlen < 65536) {
      s->rcap = s->rcap ? s->rcap * 2 : 131072;
      s->rb = xrealloc(s->rb, s->rcap);
    }
    n = read(s->out_fd, s->rb + s->rlen, s->rcap - s->rlen);
    if (n > 0) {
      s->rlen += (size_t)n;
      if (s->rlen > (size_t)LSP_MAX_MESSAGE + 8192) {
        server_died(s, "sent too much");
        return;
      }
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    /* End of file: it has exited, or closed its output, which for a server is
     * the same thing. What it had already sent is still worth reading. */
    parse(s);
    server_died(s, "exited");
    return;
  }
  parse(s);
}

/* Standard error is drained and its last line kept: a server that dies usually
 * says why there, and nothing else it writes there is molly's business. */
static void on_stderr(void *user) {
  struct server *s = user;
  char buf[4096];
  for (;;) {
    ssize_t n = read(s->err_fd, buf, sizeof(buf));
    ssize_t i, start, end;
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      if (n == 0) drop(&s->err_fd, &s->err_id);
      return;
    }
    /* The last non-empty line in this read. */
    end = n;
    while (end > 0 && (buf[end - 1] == '\n' || buf[end - 1] == '\r')) end--;
    if (end == 0) continue;
    start = end;
    while (start > 0 && buf[start - 1] != '\n') start--;
    for (i = start; i < end; i++)
      if ((unsigned char)buf[i] < 0x20) buf[i] = ' ';
    snprintf(s->lasterr, sizeof(s->lasterr), "%.*s", (int)(end - start), buf + start);
  }
}

/* ---- asking -------------------------------------------------------------- */

/* The server open with this buffer, ready for a question, or NULL -- with the
 * buffer's latest text sent first, so the answer is about what is on screen. */
static struct server *ready_for(struct buffer *b) {
  sync_buffer(b);
  if (b->lsp_srv <= 0) return NULL;
  if (servers[b->lsp_srv - 1].state != SRV_READY) return NULL;
  return &servers[b->lsp_srv - 1];
}

int lsp_can(struct buffer *b, int kind) {
  struct server *s;
  if (b->lsp_srv <= 0) return 0;
  s = &servers[b->lsp_srv - 1];
  if (s->state != SRV_READY) return 0;
  switch (kind) {
  case LSP_REQ_DEFINITION: return s->definition;
  case LSP_REQ_HOVER: return s->hover;
  case LSP_REQ_COMPLETION: return s->completion;
  case LSP_REQ_RENAME: return s->rename;
  case LSP_REQ_REFERENCES: return s->references;
  case LSP_REQ_SIGNATURE: return s->signature;
  case LSP_REQ_FORMAT: return s->format || s->format_range;
  case LSP_REQ_ACTIONS: return s->actions;
  }
  return 0;
}

/* Is `c` one of the characters after which this buffer's server offers to
 * complete without being asked? */
int lsp_is_trigger(struct buffer *b, int c) {
  struct server *s;
  if (b->lsp_srv <= 0 || c <= 0 || c > 127) return 0;
  s = &servers[b->lsp_srv - 1];
  return s->state == SRV_READY && s->completion && strchr(s->triggers, c) != NULL;
}

int lsp_can_format_range(struct buffer *b) {
  struct server *s;
  if (b->lsp_srv <= 0) return 0;
  s = &servers[b->lsp_srv - 1];
  return s->state == SRV_READY && s->format_range;
}

/* Ask for the buffer formatted: the whole of it, or with `range` the rows
 * and byte columns given. The answer is a list of edits, through
 * lsp_answer() like the rest, recorded with the cursor (cy, cx) so that an
 * answer arriving after the user has moved on is dropped. `tab_size` and `spaces` are what molly would indent this
 * file with, which every formatter takes as its starting point. */
int lsp_format(struct buffer *b, int range, int y0, int x0, int y1, int x1,
               int tab_size, int spaces, int cy, int cx) {
  struct server *s = ready_for(b);
  struct abuf ab = ABUF_INIT;
  int k;
  if (s == NULL || !(range ? s->format_range : s->format)) return 0;
  for (k = 0; k < s->npend; k++)
    if (s->pend[k].kind == LSP_REQ_FORMAT) s->pend[k].buf = NULL, s->pend[k].kind = -1;
  text_document(&ab, b);
  ab_append(&ab, "}", 1);
  if (range) {
    int u0 = 0, u1 = 0;
    if (y0 < b->numrows) {
      erow *r = buf_row(b, y0);
      u0 = lsp_col_to_units(r->chars, r->size, x0, s->utf8);
    }
    if (y1 < b->numrows) {
      erow *r = buf_row(b, y1);
      u1 = lsp_col_to_units(r->chars, r->size, x1, s->utf8);
    }
    json_putf(&ab, ",\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
                   "\"end\":{\"line\":%d,\"character\":%d}}", y0, u0, y1, u1);
  }
  json_putf(&ab, ",\"options\":{\"tabSize\":%d,\"insertSpaces\":%s,"
                 "\"trimTrailingWhitespace\":true}}", tab_size, spaces ? "true" : "false");
  request(s, range ? "textDocument/rangeFormatting" : "textDocument/formatting",
          &ab, LSP_REQ_FORMAT, b);
  free(ab.b);
  s->pend[s->npend - 1].cx = cx;
  s->pend[s->npend - 1].cy = cy;
  return 1;
}

/* The code actions for a range of `b` -- the selection, or the cursor's place
 * -- with the diagnostics that touch it, which is what lets a server offer the
 * fix for the error under the cursor. */
int lsp_code_actions(struct buffer *b, int y0, int x0, int y1, int x1, int cy, int cx) {
  struct server *s = ready_for(b);
  struct abuf ab = ABUF_INIT;
  int i, first = 1;
  if (s == NULL || !s->actions) return 0;
  text_document(&ab, b);
  ab_append(&ab, "}", 1);
  {
    erow *r0 = y0 < b->numrows ? buf_row(b, y0) : NULL;
    erow *r1 = y1 < b->numrows ? buf_row(b, y1) : NULL;
    json_putf(&ab, ",\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
                   "\"end\":{\"line\":%d,\"character\":%d}}", y0,
              r0 ? lsp_col_to_units(r0->chars, r0->size, x0, s->utf8) : 0, y1,
              r1 ? lsp_col_to_units(r1->chars, r1->size, x1, s->utf8) : 0);
  }
  json_putf(&ab, ",\"context\":{\"diagnostics\":[");
  for (i = 0; i < b->ndiag; i++) {
    const struct diag *d = &b->diag[i];
    erow *r0, *r1;
    if (d->y1 < y0 || d->y0 > y1) continue;
    if (d->y1 == y0 && d->x1 < x0) continue;
    if (d->y0 == y1 && d->x0 > x1) continue;
    r0 = d->y0 < b->numrows ? buf_row(b, d->y0) : NULL;
    r1 = d->y1 < b->numrows ? buf_row(b, d->y1) : NULL;
    if (!first) ab_append(&ab, ",", 1);
    first = 0;
    json_putf(&ab, "{\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
                   "\"end\":{\"line\":%d,\"character\":%d}},\"severity\":%d,"
                   "\"message\":", d->y0,
              r0 ? lsp_col_to_units(r0->chars, r0->size, d->x0, s->utf8) : 0, d->y1,
              r1 ? lsp_col_to_units(r1->chars, r1->size, d->x1, s->utf8) : 0,
              d->severity);
    json_put_str(&ab, d->msg, (int)strlen(d->msg));
    ab_append(&ab, "}", 1);
  }
  ab_append(&ab, "]}}", 3);
  request(s, "textDocument/codeAction", &ab, LSP_REQ_ACTIONS, b);
  free(ab.b);
  s->pend[s->npend - 1].cx = cx;
  s->pend[s->npend - 1].cy = cy;
  return 1;
}

/* One chosen action that has no edit to apply here and now: filled in by the
 * server first when `resolve` (its edit comes back as LSP_REQ_RESOLVE), or its
 * command run (whose edits come back as the server's own applyEdit). `action`
 * is the CodeAction or Command as the server sent it. 0 when there is nobody
 * to ask, or nothing to ask for. */
int lsp_code_action_run(struct buffer *b, const char *action, int resolve, int cy, int cx) {
  struct server *s = ready_for(b);
  struct json_doc *doc;
  const struct json *a, *cmd;
  struct abuf ab = ABUF_INIT;
  if (s == NULL) return 0;
  doc = json_parse(action, strlen(action));
  a = json_root(doc);
  if (a == NULL) {
    json_free(doc);
    return 0;
  }
  if (resolve) {
    if (!s->resolve) {
      json_free(doc);
      return 0;
    }
    ab_append(&ab, action, (int)strlen(action));
    request(s, "codeAction/resolve", &ab, LSP_REQ_RESOLVE, b);
  } else {
    /* A Command's name is its "command"; a CodeAction's command is an object
     * holding one. */
    cmd = json_get(a, "command");
    if (cmd != NULL && cmd->type == JSON_OBJECT) a = cmd;
    if (json_str(json_get(a, "command")) == NULL) {
      json_free(doc);
      return 0;
    }
    ab_append(&ab, "{\"command\":", 11);
    json_write(&ab, json_get(a, "command"));
    if (json_get(a, "arguments") != NULL) {
      ab_append(&ab, ",\"arguments\":", 13);
      json_write(&ab, json_get(a, "arguments"));
    }
    ab_append(&ab, "}", 1);
    request(s, "workspace/executeCommand", &ab, LSP_REQ_EXECUTE, b);
  }
  s->pend[s->npend - 1].cx = cx;
  s->pend[s->npend - 1].cy = cy;
  free(ab.b);
  json_free(doc);
  return 1;
}

/* Does typing `c` in this buffer ask for signature help? */
int lsp_is_sig_trigger(struct buffer *b, int c) {
  struct server *s;
  if (b->lsp_srv <= 0 || c <= 0 || c > 127) return 0;
  s = &servers[b->lsp_srv - 1];
  return s->state == SRV_READY && s->signature && strchr(s->sig_triggers, c) != NULL;
}

/* Ask about the position (cy, cx) in `b`. Returns 1 when the question went,
 * 0 when there is nobody to ask. The answer arrives through lsp_answer().
 * `trigger` is the character that asked for a completion, 0 for a key; `arg`
 * is the new name for a rename, NULL otherwise. */
int lsp_ask(struct buffer *b, int kind, int cy, int cx, int trigger,
            const char *arg) {
  static const char *const methods[] = {
    [LSP_REQ_DEFINITION] = "textDocument/definition",
    [LSP_REQ_HOVER] = "textDocument/hover",
    [LSP_REQ_COMPLETION] = "textDocument/completion",
    [LSP_REQ_RENAME] = "textDocument/rename",
    [LSP_REQ_REFERENCES] = "textDocument/references",
    [LSP_REQ_SIGNATURE] = "textDocument/signatureHelp",
  };
  struct server *s = ready_for(b);
  struct abuf ab = ABUF_INIT;
  int k, units = 0;

  if (s == NULL || !lsp_can(b, kind)) return 0;
  if (cy < b->numrows) {
    erow *r = buf_row(b, cy);
    units = lsp_col_to_units(r->chars, r->size, cx, s->utf8);
  }
  /* One question of each kind at a time: the answer to an older one is about
   * a cursor that has since moved. */
  for (k = 0; k < s->npend; k++)
    if (s->pend[k].kind == kind) s->pend[k].buf = NULL, s->pend[k].kind = -1;

  text_document(&ab, b);
  json_putf(&ab, "},\"position\":{\"line\":%d,\"character\":%d}", cy, units);
  if (kind == LSP_REQ_COMPLETION) {
    if (trigger > 0)
      json_putf(&ab, ",\"context\":{\"triggerKind\":2,\"triggerCharacter\":\"%c\"}",
                trigger);
    else
      ab_append(&ab, ",\"context\":{\"triggerKind\":1}", 28);
  } else if (kind == LSP_REQ_REFERENCES) {
    /* The declaration too: "where is this used" is a question about the name,
     * and where it is declared is one of the answers. */
    json_putf(&ab, ",\"context\":{\"includeDeclaration\":true}");
  } else if (kind == LSP_REQ_RENAME && arg != NULL) {
    ab_append(&ab, ",\"newName\":", 11);
    json_put_str(&ab, arg, (int)strlen(arg));
  }
  ab_append(&ab, "}", 1);
  request(s, methods[kind], &ab, kind, b);
  free(ab.b);
  s->pend[s->npend - 1].cx = cx;
  s->pend[s->npend - 1].cy = cy;
  return 1;
}

/* ---- the switch, and the end --------------------------------------------- */

static void server_stop(struct server *s) {
  if (s->state == SRV_READY) {
    /* Asked politely, and not waited on: `shutdown` then `exit` is the
     * protocol's way out, and a server that obeys it is gone within the grace
     * period reap() gives it. Written now, in one go, because there will be no
     * event loop to finish the write later. */
    request(s, "shutdown", NULL, LSP_REQ_SHUTDOWN, NULL);
    notify(s, "exit", NULL);
    if (s->in_fd >= 0) {
      fcntl(s->in_fd, F_SETFL, fcntl(s->in_fd, F_GETFL, 0) & ~O_NONBLOCK);
      if (s->wdone < s->wq.len)
        write_all(s->in_fd, s->wq.b + s->wdone, (size_t)(s->wq.len - s->wdone));
    }
    reap(s, LSP_EXIT_GRACE_MS);
  } else {
    reap(s, 0);
  }
  forget_server(s);
  free(s->cmd);
  free(s->root);
  free(s->wq.b);
  free(s->rb);
  memset(s, 0, sizeof(*s));
  s->in_fd = s->out_fd = s->err_fd = -1;
  s->in_id = s->out_id = s->err_id = -1;
}

/* Every server stopped and forgotten. The next sweep starts what is still
 * wanted afresh, which makes this the restart as well as the shutdown. */
void lsp_stop_all(void) {
  int i;
  for (i = 0; i < LSP_MAX_SERVERS; i++)
    if (servers[i].state != SRV_FREE) server_stop(&servers[i]);
  for (i = 0; i < buf_count(); i++) doc_forget(buf_at(i));
}

void lsp_cleanup(void) {
  int i;
  lsp_stop_all();
  trust_q_clear();
  trust_cleanup();
  for (i = 0; i < ROOT_CACHE; i++) {
    free(root_cache[i].dir);
    free(root_cache[i].root);
    root_cache[i].dir = root_cache[i].root = NULL;
  }
}

/* On unless something said off. The environment can say it for good: the test
 * harness does, so that a developer with clangd installed sees the same screens
 * as a machine without it. */
int lsp_enabled(void) {
  const char *env;
  if (!enabled) return 0;
  env = getenv("MOLLY_LSP");
  return env == NULL || strcmp(env, "off") != 0;
}

/* The setting as the user left it, which is what the Options dialog shows and
 * what Save Settings writes. Not lsp_enabled(): a test harness's MOLLY_LSP=off
 * must not end up in someone's mollyrc as their choice. */
int lsp_setting(void) { return enabled; }

void lsp_set_enabled(int on) {
  enabled = on ? 1 : 0;
  if (!enabled) lsp_stop_all();
}

/* What to say about the current buffer's server, for the status command. */
void lsp_describe(struct buffer *b, char *dst, size_t n) {
  const char *cmd = (b->syn != NULL) ? syntax_lsp(b->syn) : NULL;
  int i;
  if (!lsp_enabled()) {
    snprintf(dst, n, "Language servers are off.");
    return;
  }
  if (cmd == NULL || cmd[0] == '\0') {
    snprintf(dst, n, "No language server for %s.",
             b->syn != NULL ? syntax_name(b->syn) : "this file");
    return;
  }
  if (b->filename != NULL) {
    const char *root = project_root(b->filename);
    if (server_find(cmd, root) == NULL && trust_lookup(root) == 0) {
      snprintf(dst, n, "Not started for this project, as you asked. Restart language "
                       "servers to be asked again.");
      return;
    }
  }
  for (i = 0; i < LSP_MAX_SERVERS; i++) {
    struct server *s = &servers[i];
    if (s->state == SRV_FREE || strcmp(s->cmd, cmd) != 0) continue;
    switch (s->state) {
    case SRV_MISSING:
      snprintf(dst, n, "%s is not installed.", s->name);
      return;
    case SRV_STARTING:
      snprintf(dst, n, "%s is starting.", s->name);
      return;
    case SRV_READY:
      if (b->lsp_srv == -1)
        snprintf(dst, n, "%s is running; this file is too big to send it.", s->name);
      else
        snprintf(dst, n, "%s is running: %d problem%s here.", s->name, b->ndiag,
                 b->ndiag == 1 ? "" : "s");
      return;
    case SRV_DEAD:
      snprintf(dst, n, "%s has stopped%s%s", s->name,
               s->lasterr[0] ? ": " : ".", s->lasterr);
      return;
    }
  }
  snprintf(dst, n, "%s has not been started.", cmd);
}
