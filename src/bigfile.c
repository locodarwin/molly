/* bigfile.c -- a file too big to read into memory, shown read-only.
 *
 * A file is read whole when it opens (buffer.c), at about 1.4 times its size.
 * One bigger than half of the memory available, or one whose read runs out of
 * memory, opens here instead, without being read in. It cannot be edited; it
 * can be read, scrolled, searched, selected from and copied out of.
 *
 * Nothing else in molly knows the difference, because the rows are still
 * rows. A buffer with `big` set has as many as the file has lines, and
 * buf_row() hands each one out as it is asked for, read from the file into a
 * page of rows it keeps for a while. So a line number is a line number
 * everywhere -- the cursor, the scroll, a selection, a bookmark, a split, Go
 * to line, the history -- and none of that code changed.
 *
 *   - **The index.** Opening reads the file through once to count its lines,
 *     and keeps where one line in every 1,024 starts -- or one a megabyte on
 *     from the last, if that comes first, so that a page is never much more
 *     than a megabyte. 8 GB of 140-byte lines is 61 million lines and half a
 *     megabyte of index; the read takes as long as the disk, and Esc stops it.
 *
 *   - **The pages.** A page is the lines from one mark of the index to the
 *     next, read with pread() into rows of its own. Up to 64 MB of them are
 *     kept, the one used longest ago going first, and never fewer than eight,
 *     so a row handed out stays where it is while the code that asked for it
 *     asks for a few more.
 *
 *   - **Read, not mapped.** A file mapped into memory that something else
 *     truncates -- a log being rotated -- kills the process that touches the
 *     part that has gone. A read past the end only comes back short, and the
 *     lines it did not get are empty.
 *
 *   - **Long lines are cut.** A row's length is an int, and one line can be
 *     the whole file, so a line is kept to its first megabyte, for showing and
 *     searching alike, and the status bar says when the cursor is on one.
 *
 * What needs the whole file in memory is off, by the CMD_LOADED flag on its
 * command or a check where it starts: editing of every kind, saving, syntax
 * colors (a line's colors depend on every line above it), soft wrap and
 * folding (both count every row's display rows), the language server, the git
 * gutter, and cursors at matches. Opening, a search and a jump to a
 * matching bracket each walk the file, and each says how far it has got and
 * stops at Esc. */

#include "molly.h"

/* The shape of the index and the pages. Variables rather than constants so
 * that the tests can make them small; big_tune() sets them. */
static int step_lines = 1024;      /* lines from one mark to the next, at most */
static int step_bytes = 1 << 20;   /* bytes after which a mark comes sooner */
static int cut_bytes = 1 << 20;    /* the most of a line that is kept */
static int max_pages = 256;        /* pages kept, at most */

#define PAGE_BUDGET (64u << 20)    /* bytes of pages kept, beyond PAGES_KEPT */
#define PAGES_KEPT 8               /* pages kept whatever they cost */
#define SCAN_CHUNK (8 << 20)       /* bytes read at a time while indexing */
#define PAGE_CHUNK (1 << 20)       /* and while reading a page */

/* Where a line starts. */
struct mark {
  long long off;
  int line;
};

struct page {
  int k;                 /* the mark it starts at; -1 for an empty slot */
  int n;                 /* rows */
  erow *rows;
  long long *offs;       /* where in the file each row's line starts */
  unsigned char *cut;    /* the line was longer than its row */
  char *text;            /* the rows' bytes, each followed by a NUL */
  size_t bytes;          /* what all of that costs */
  unsigned long used;    /* when it was last asked for */
};

struct big {
  int fd;
  long long size;        /* bytes indexed; nothing after them is read */
  int nlines;
  struct mark *ck;
  int nck, ckcap;
  int *slot;             /* the page holding each mark's rows, or -1 */
  struct page *pages;    /* max_pages slots */
  int held;
  size_t held_bytes;
  int last;              /* the slot last asked for */
  unsigned long clock;
  char *chunk;           /* PAGE_CHUNK bytes to read into */
  int eol_crlf, final_newline;
  struct stat st;
};

void big_tune(int lines, int bytes, int cut, int pages) {
  step_lines = lines > 0 ? lines : 1024;
  step_bytes = bytes > 0 ? bytes : 1 << 20;
  cut_bytes = cut > 0 ? cut : 1 << 20;
  max_pages = pages >= PAGES_KEPT ? pages : 256;
}

/* ---- when a file opens this way --------------------------------------------- */

/* The `[files] big file` setting, in kilobytes; 0 is half of the memory
 * available as the file opens. */
static int view_over_kb;

int big_view_over_kb(void) { return view_over_kb; }
void big_set_view_over_kb(int kb) { view_over_kb = kb > 0 ? kb : 0; }

/* Bytes of memory that can be had without swapping, as the kernel reckons it,
 * or -1 for not known. */
static long long mem_available(void) {
  FILE *fp = fopen("/proc/meminfo", "r");
  if (fp != NULL) {
    char line[256];
    long long kb;
    while (fgets(line, sizeof line, fp) != NULL) {
      if (sscanf(line, "MemAvailable: %lld kB", &kb) == 1) {
        fclose(fp);
        return kb * 1024;
      }
    }
    fclose(fp);
  }
#ifdef _SC_AVPHYS_PAGES
  {
    long pages = sysconf(_SC_AVPHYS_PAGES), psz = sysconf(_SC_PAGESIZE);
    if (pages > 0 && psz > 0) return (long long)pages * psz;
  }
#endif
  return -1;
}

int big_wanted(long long size) {
  long long avail;
  if (view_over_kb > 0) return size > (long long)view_over_kb * 1024;
  avail = mem_available();
  return avail > 0 && size > avail / 2;
}

/* ---- progress ------------------------------------------------------------- */

static double now_s(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* What a long walk says while it goes, at most five times a second, and
 * whether Esc has been pressed. `done` of `total`. */
static double said_at;

static int progress(const char *doing, long long done, long long total) {
  double t = now_s();
  if (t - said_at >= 0.2) {
    said_at = t;
    editor_set_status_message("%s %d%% (Esc stops)", doing,
                              total > 0 ? (int)(done * 100 / total) : 0);
    editor_refresh_screen();
  }
  return input_escape_pressed();
}

int big_stopped(const char *doing, long long done, long long total) {
  if (V.buf == NULL || V.buf->big == NULL) return 0;
  return progress(doing, done, total);
}

/* ---- the index -------------------------------------------------------------- */

static void add_mark(struct big *g, long long off, int line) {
  if (g->nck == g->ckcap) {
    g->ckcap = g->ckcap ? g->ckcap * 2 : 1024;
    g->ck = xrealloc(g->ck, sizeof(*g->ck) * (size_t)g->ckcap);
  }
  g->ck[g->nck].off = off;
  g->ck[g->nck].line = line;
  g->nck++;
}

static ssize_t read_at(int fd, char *buf, size_t n, long long off) {
  ssize_t r;
  do r = pread(fd, buf, n, (off_t)off);
  while (r < 0 && errno == EINTR);
  return r;
}

void big_free(struct big *g) {
  int i;
  if (g == NULL) return;
  if (g->fd >= 0) close(g->fd);
  for (i = 0; g->pages != NULL && i < max_pages; i++) {
    free(g->pages[i].rows);
    free(g->pages[i].offs);
    free(g->pages[i].cut);
    free(g->pages[i].text);
  }
  free(g->pages);
  free(g->slot);
  free(g->ck);
  free(g->chunk);
  free(g);
}

/* The file open on `fd`, indexed, or NULL with *why saying why not -- Esc
 * among the reasons. Takes the descriptor, whichever. */
struct big *big_open(int fd, const char *name, const char **why) {
  struct big *g = xmalloc(sizeof *g);
  char *buf = NULL;
  const char *slash = strrchr(name, '/');
  char doing[96];
  long long off = 0, lines = 0, first_nl = -1;
  unsigned char lastc = '\n';
  int i;

  memset(g, 0, sizeof *g);
  g->fd = fd;
  *why = NULL;
  snprintf(doing, sizeof doing, "Reading %.40s:", slash && slash[1] ? slash + 1 : name);
  if (fstat(fd, &g->st) == -1) {
    *why = strerror(errno);
    goto fail;
  }
  buf = malloc(SCAN_CHUNK);
  g->chunk = malloc(PAGE_CHUNK);
  if (buf == NULL || g->chunk == NULL) {
    *why = strerror(ENOMEM);
    goto fail;
  }
#ifdef POSIX_FADV_SEQUENTIAL
  posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
  said_at = now_s();
  add_mark(g, 0, 0);
  for (;;) {
    long long want = (long long)g->st.st_size - off;
    ssize_t n;
    const char *p, *q, *end;
    if (want <= 0) break;
    n = read_at(fd, buf, want < SCAN_CHUNK ? (size_t)want : SCAN_CHUNK, off);
    if (n < 0) {
      *why = strerror(errno);
      goto fail;
    }
    if (n == 0) break;   /* shorter than it was: what was read is the file */
    end = buf + n;
    for (p = buf; (q = memchr(p, '\n', (size_t)(end - p))) != NULL; p = q + 1) {
      long long at = off + (q - buf) + 1;
      const struct mark *m = &g->ck[g->nck - 1];
      if (first_nl < 0) first_nl = at - 1;
      if (++lines >= INT_MAX - 1) {
        *why = "too many lines";
        goto fail;
      }
      if (at < (long long)g->st.st_size &&
          (lines - m->line >= step_lines || at - m->off >= step_bytes))
        add_mark(g, at, (int)lines);
    }
    lastc = (unsigned char)end[-1];
    off += n;
    if (progress(doing, off, (long long)g->st.st_size)) {
      *why = "stopped by Esc";
      goto fail;
    }
  }
  if (off > 0 && lastc != '\n') lines++;
  g->size = off;
  g->nlines = (int)lines;
  g->final_newline = (off == 0 || lastc == '\n');
  if (first_nl > 0) {
    char c;
    g->eol_crlf = read_at(fd, &c, 1, first_nl - 1) == 1 && c == '\r';
  }
  free(buf);

  g->slot = xmalloc(sizeof(*g->slot) * (size_t)g->nck);
  for (i = 0; i < g->nck; i++) g->slot[i] = -1;
  g->pages = xmalloc(sizeof(*g->pages) * (size_t)max_pages);
  memset(g->pages, 0, sizeof(*g->pages) * (size_t)max_pages);
  for (i = 0; i < max_pages; i++) g->pages[i].k = -1;
  editor_set_status_message("");
  return g;

fail:
  free(buf);
  big_free(g);
  return NULL;
}

/* Into the current buffer, which is new and empty, under `filename`. */
void big_attach(struct big *g, const char *filename) {
  free(B.filename);
  B.filename = xstrdup(filename);
  build_recovery_path();
  B.big = g;
  B.numrows = g->nlines;
  B.gap = 0;
  B.readonly = 1;
  B.dirty = 0;
  B.lsp_srv = -1;
  B.eol_crlf = g->eol_crlf;
  B.final_newline = g->final_newline;
  B.on_disk_mtime = g->st.st_mtim;
  B.have_disk_mtime = 1;
  B.wrapgen++;
  wrap_map_changed(V.buf);
  syntax_retype();
}

/* ---- the pages -------------------------------------------------------------- */

/* The mark at or before line `y`. */
static int mark_of(const struct big *g, int y) {
  int lo = 0, hi = g->nck - 1;
  while (lo < hi) {
    int mid = lo + (hi - lo + 1) / 2;
    if (g->ck[mid].line <= y) lo = mid;
    else hi = mid - 1;
  }
  return lo;
}

static void page_drop(struct big *g, int s) {
  struct page *p = &g->pages[s];
  if (p->k < 0) return;
  g->slot[p->k] = -1;
  g->held--;
  g->held_bytes -= p->bytes;
  free(p->rows);
  free(p->offs);
  free(p->cut);
  free(p->text);
  memset(p, 0, sizeof *p);
  p->k = -1;
}

/* A slot to read a page into, emptied of the page used longest ago if the
 * pages kept are at their limit. */
static int page_slot(struct big *g) {
  int s, free_slot = -1;
  while (g->held >= max_pages ||
         (g->held_bytes > PAGE_BUDGET && g->held > PAGES_KEPT)) {
    int old = -1;
    for (s = 0; s < max_pages; s++)
      if (g->pages[s].k >= 0 && (old < 0 || g->pages[s].used < g->pages[old].used))
        old = s;
    page_drop(g, old);
  }
  for (s = 0; s < max_pages && free_slot < 0; s++)
    if (g->pages[s].k < 0) free_slot = s;
  return free_slot;
}

/* A page as it is read: the bytes kept so far, each line's first cut_bytes
 * and one more, which is what says whether a character was cut in half; where
 * each row starts in them; and the line being read. */
struct reader {
  struct page *p;
  char *b;
  size_t len, cap;
  size_t *start;
  int j;               /* the row being read */
  size_t kept;         /* of its bytes */
  long long raw;       /* bytes of its line so far */
  unsigned char prev;  /* the last of them */
};

static void reader_put(struct reader *r, const char *s, size_t n) {
  if (r->len + n + 1 > r->cap) {
    while (r->len + n + 1 > r->cap) r->cap = r->cap ? r->cap * 2 : 4096;
    r->b = xrealloc(r->b, r->cap);
  }
  memcpy(r->b + r->len, s, n);
  r->len += n;
}

/* The row being read is done: less the carriage return its newline took, if
 * `cr_taken`, kept to cut_bytes short of a split character, and with `crlf`
 * as its line ending. */
static void reader_row_done(struct reader *r, int cr_taken, int crlf) {
  long long have = r->raw - cr_taken;
  size_t keep = have < (long long)cut_bytes ? (size_t)have : (size_t)cut_bytes;
  if (have > (long long)cut_bytes) {
    /* The byte at `keep` was kept as well, to look at. */
    while (keep > 0 && utf8_is_cont((unsigned char)r->b[r->start[r->j] + keep])) keep--;
    r->p->cut[r->j] = 1;
  }
  r->len = r->start[r->j] + keep;
  reader_put(r, "", 1);
  r->p->rows[r->j].size = (int)keep;
  r->p->rows[r->j].crlf = (unsigned char)crlf;
  r->j++;
  r->start[r->j] = r->len;
  r->kept = 0;
  r->raw = 0;
  r->prev = 0;
}

/* Read mark k's lines into slot s. A line gives up one carriage return before
 * its newline, as buffer.c's rows_from_text() has a line do, and is kept to
 * its first cut_bytes, less whatever of a character that would cut in half.
 * A read that comes back short leaves the lines it did not reach empty. */
static void page_read(struct big *g, int s, int k) {
  struct page *p = &g->pages[s];
  long long off = g->ck[k].off;
  long long end = k + 1 < g->nck ? g->ck[k + 1].off : g->size;
  int n = (k + 1 < g->nck ? g->ck[k + 1].line : g->nlines) - g->ck[k].line, j;
  size_t most = (size_t)step_bytes + (size_t)cut_bytes + (size_t)n + 2;
  struct reader r;

  memset(&r, 0, sizeof r);
  r.p = p;
  r.cap = (size_t)(end - off) + (size_t)n + 1;
  if (r.cap > most) r.cap = most;
  r.b = xmalloc(r.cap);
  r.start = xmalloc(sizeof(*r.start) * (size_t)(n + 1));
  r.start[0] = 0;
  p->k = k;
  p->n = n;
  p->rows = xmalloc(sizeof(*p->rows) * (size_t)n);
  p->offs = xmalloc(sizeof(*p->offs) * (size_t)n);
  p->cut = xmalloc((size_t)n);
  memset(p->cut, 0, (size_t)n);
  p->offs[0] = off;

  while (off < end && r.j < n) {
    long long want = end - off;
    ssize_t got = read_at(g->fd, g->chunk, want < PAGE_CHUNK ? (size_t)want : PAGE_CHUNK, off);
    const char *q, *e;
    if (got <= 0) break;
    e = g->chunk + got;
    for (q = g->chunk; q < e && r.j < n;) {
      const char *nl = memchr(q, '\n', (size_t)(e - q));
      const char *se = nl != NULL ? nl : e;
      size_t len = (size_t)(se - q);
      if (r.kept < (size_t)cut_bytes + 1) {
        size_t take = (size_t)cut_bytes + 1 - r.kept;
        if (take > len) take = len;
        reader_put(&r, q, take);
        r.kept += take;
      }
      if (len > 0) r.prev = (unsigned char)se[-1];
      r.raw += (long long)len;
      if (nl == NULL) break;
      {
        int cr = r.raw > 0 && r.prev == '\r';
        reader_row_done(&r, cr, cr);
      }
      q = nl + 1;
      if (r.j < n) p->offs[r.j] = off + (q - g->chunk);
    }
    off += got;
  }
  /* The file's last line, if it has no newline -- which takes the file's
   * line ending, as rows_from_text() gives it, for if it is ever given one --
   * and then anything a short read did not reach. */
  while (r.j < n) {
    int last = k + 1 == g->nck && r.j == n - 1 && !g->final_newline;
    if (r.j > 0 && r.raw == 0) p->offs[r.j] = off;
    reader_row_done(&r, 0, last ? g->eol_crlf : 0);
  }

  for (j = 0; j < n; j++) {
    erow *row = &p->rows[j];
    row->chars = r.b + r.start[j];
    row->wcols = -1;
    row->wrows = 1;
    row->syn_in = 0;
    row->syn_out = 0;
  }
  free(r.start);
  p->text = r.b;
  p->bytes = r.cap + (size_t)n * (sizeof(erow) + sizeof(long long) + 1);
  g->held++;
  g->held_bytes += p->bytes;
  g->slot[k] = s;
}

static struct page *page_of(struct big *g, int y) {
  struct page *p = &g->pages[g->last];
  int k, s;
  if (p->k >= 0 && y >= g->ck[p->k].line && y - g->ck[p->k].line < p->n) {
    p->used = ++g->clock;
    return p;
  }
  k = mark_of(g, y);
  s = g->slot[k];
  if (s < 0) {
    s = page_slot(g);
    page_read(g, s, k);
  }
  g->last = s;
  p = &g->pages[s];
  p->used = ++g->clock;
  return p;
}

/* What a row outside the file reads as: nothing, as the phantom row past the
 * last one is. Rewritten each time, since the caller has a pointer it could
 * have written through. */
static char nothing[1];
static erow empty_row;

erow *big_row(const struct buffer *b, int y) {
  struct big *g = b->big;
  struct page *p;
  if (y < 0 || y >= g->nlines) {
    nothing[0] = '\0';
    memset(&empty_row, 0, sizeof empty_row);
    empty_row.chars = nothing;
    empty_row.wcols = -1;
    empty_row.wrows = 1;
    return &empty_row;
  }
  p = page_of(g, y);
  return &p->rows[y - g->ck[p->k].line];
}

/* Where line y starts in the file; the file's length for one past the end. */
long long big_line_offset(const struct buffer *b, int y) {
  struct big *g = b->big;
  struct page *p;
  if (y >= g->nlines) return g->size;
  if (y < 0) return 0;
  p = page_of(g, y);
  return p->offs[y - g->ck[p->k].line];
}

/* Whether row y is only the start of its line. */
int big_row_cut(const struct buffer *b, int y) {
  struct big *g = b->big;
  struct page *p;
  if (g == NULL || y < 0 || y >= g->nlines) return 0;
  p = page_of(g, y);
  return p->cut[y - g->ck[p->k].line];
}

/* How much of a line is kept, as the status bar says it. */
const char *big_cut_size(void) {
  static char said[32];
  if (cut_bytes >= 1 << 20 && cut_bytes % (1 << 20) == 0)
    snprintf(said, sizeof said, "%d MB", cut_bytes >> 20);
  else
    snprintf(said, sizeof said, "%d bytes", cut_bytes);
  return said;
}

int big_pages_held(const struct buffer *b) { return b->big ? b->big->held : 0; }

/* ---- what is off ------------------------------------------------------------ */

#define TOO_BIG "a file too big to read into memory"

int big_refuse(const char *what) {
  if (V.buf == NULL || V.buf->big == NULL) return 0;
  editor_set_status_message("%s is off in " TOO_BIG ".", what);
  return 1;
}

const char *editor_readonly_why(void) {
  if (V.buf != NULL && V.buf->big != NULL)
    return "This is " TOO_BIG ", so it is shown read-only.";
  return "This file is read-only. View > Read only turns that off.";
}
