/* search.c -- Finding and replacing text, and going to a line. */

#include "molly.h"

static char *last_query = NULL;

/* Off by default, matching the search behavior this project already
 * shipped; toggled globally with Ctrl-T rather than per-prompt, since
 * editor_prompt's format string is fixed for the life of the prompt and
 * has no channel back to the caller besides the typed buffer. */
static int search_case_sensitive = 0;

/* Regex is a mode rather than a second set of commands: the same Ctrl-F, Ctrl-N
 * and Ctrl-R, reading the query as a pattern instead of as text. Alt-R turns it
 * on, the Search menu ticks it, and search.regex is the name a config file will
 * set once there is one.
 *
 * Both modes match the row as it is *stored*. olly matched the tab-expanded
 * render buffer, so a query of eight spaces found a tab; both are the row's own
 * bytes now, which means a search matches the characters it was asked for and a
 * match offset is a cursor position already, with no render mapping in between. A
 * tab is still only nameable as \t in a pattern, because a prompt has no way to
 * accept a Tab keystroke. */
static int search_use_regex = 0;

/* Whether there is a term to find again, which is what greys out Find Next and
 * Find Previous until the first search. */
int search_has_query(void) {
  return last_query != NULL && last_query[0] != '\0';
}

int search_case_is_on(void) {
  return search_case_sensitive;
}

int search_regex_is_on(void) {
  return search_use_regex;
}

void editor_toggle_regex(void) {
  search_use_regex = !search_use_regex;
  editor_set_status_message("Search is now %s", search_use_regex
      ? "a regular expression" : "plain text");
}

/* The pattern for one operation, or NULL having said in the message bar why not.
 * Compiled per operation rather than cached: a pattern is small, and the only
 * loop that would notice is a replace-all, which compiles once and sweeps with
 * what it compiled. */
static struct regex *search_compile(const char *pat) {
  const char *err = NULL;
  struct regex *re = regex_compile(pat,
      search_case_sensitive ? 0 : REGEX_ICASE, &err);

  if (re == NULL)
    editor_set_status_message("Bad pattern: %s", err ? err : "not understood");
  return re;
}

/* One character on from a chars-space offset. A scan that steps by a byte could
 * stop inside a character; a scan that does not step at all is how an empty match
 * turns into a loop. */
static int next_char_offset(erow *row, int at) {
  if (at >= row->size) return at + 1;
  return at + utf8_char_bytes(row->chars, row->size, at);
}

/* The first match in this row starting at or after `from`. Returns 1 and fills
 * *m, 0 for nothing in this row, or -1 if the matcher gave up -- which the caller
 * has to pass on rather than read as "not here", because a sweep that read it as
 * "not here" would go on to ask the same impossible question of every row left in
 * the file. */
static int regex_row_first(const struct regex *re, erow *row, int from,
                           struct regex_match *m) {
  int r = regex_exec(re, row->chars, row->size, from, m);

  if (r == REGEX_TOO_HARD) return -1;
  return r == REGEX_MATCH;
}

/* The last match in this row that *starts* at or before `limit`, which is what
 * searching backward wants. Matches are enumerated forward -- regex_exec only
 * answers leftmost -- stepping one character past each match's start rather than
 * past its end, so that two overlapping matches are both seen. */
static int regex_row_last(const struct regex *re, erow *row, int limit,
                          struct regex_match *m) {
  struct regex_match try;
  int at = 0, found = 0;

  while (at <= row->size) {
    int r = regex_exec(re, row->chars, row->size, at, &try);
    if (r == REGEX_TOO_HARD) return -1;
    if (r == REGEX_NOMATCH || try.so[0] > limit) break;
    *m = try;
    found = 1;
    at = next_char_offset(row, try.so[0]);
  }
  return found;
}

/* Release the remembered search term. */
void search_cleanup(void) {
  free(last_query);
  last_query = NULL;
}

static void editor_set_last_query(const char *q) {
  if (last_query) free(last_query);
  last_query = q ? xstrdup(q) : NULL;
}

/* Explicit ASCII case folding. Deliberately not strncasecmp: that folds
 * according to the current locale, so the same query would match differently
 * under, say, a Turkish locale, and it folds non-ASCII bytes in ways Molly
 * does not control. Molly compares raw bytes everywhere else, so the case
 * insensitive fold folds exactly A-Z/a-z and compares every other byte
 * as-is; non-ASCII letters are never case-folded. */
static int ascii_fold(unsigned char c) {
  return (c >= 'a' && c <= 'z') ? (unsigned char)(c - ('a' - 'A')) : c;
}

static int bytes_match(const char *hay, const char *needle, int n) {
  int i;

  if (search_case_sensitive)
    return strncmp(hay, needle, (size_t)n) == 0;
  for (i = 0; i < n; i++)
    if (ascii_fold((unsigned char)hay[i]) !=
        ascii_fold((unsigned char)needle[i]))
      return 0;
  return 1;
}

/* A match is only accepted when it spans whole characters: its first byte
 * must not be a UTF-8 continuation byte (otherwise the cursor would land
 * inside a character), and the byte just past it must not be one either
 * (otherwise the match ends mid-character and a replace would splice a
 * character in half). chars is NUL-terminated, so hay[n] is in bounds and
 * reads as a non-continuation byte at end of row. */
static int match_is_char_aligned(const char *hay, int n) {
  return !utf8_is_cont((unsigned char)hay[0]) &&
         !utf8_is_cont((unsigned char)hay[n]);
}

/* What a sweep comes back with. TOO_HARD is the regex engine's step budget or
 * its recursion depth running out, which is a different thing from not finding
 * anything and has to be said differently -- the text may well contain what was
 * asked for. */
#define SEARCH_NOT_FOUND 0
#define SEARCH_FOUND     1
#define SEARCH_TOO_HARD (-1)

/* How far the sweep may run before giving up. */
enum search_sweep {
  SEARCH_WRAP,      /* full circle, starting row revisited from its start */
  SEARCH_ONE_PASS,  /* every row once, the starting row only from the cursor */
  SEARCH_NO_WRAP    /* from the cursor to the last row only, never backward */
};

/* dir = 1 search forward, -1 search backward. exclude_current causes the
 * search to skip a match exactly at the cursor. The sweep mode decides the
 * reach: SEARCH_WRAP wraps (plain find, so a term before the cursor is
 * reachable), SEARCH_ONE_PASS visits each row once, and SEARCH_NO_WRAP
 * never moves past the cursor toward the start of the file -- replace-all
 * needs that stronger guarantee: with a weaker sweep the wrap can revisit
 * an earlier row from column 0 and re-match a replacement that contains
 * the search term, looping forever as the row grows without bound.
 *
 * `re` non-NULL is a regex search, and then `query` is only the thing it was
 * compiled from. The caller compiles, so that one sweep over a hundred thousand
 * rows compiles once; and `out`, when it is not NULL, brings the match back for
 * replace to expand \1 from. */
static int editor_search_string(const char *query, int qlen,
    const struct regex *re, int dir, int exclude_current,
    enum search_sweep sweep, struct regex_match *out) {
  int saved_cx = CUR.cx;
  int saved_cy = CUR.cy;
  int saved_coloff = V.coloff;
  int saved_drowoff = V.drowoff;

  int n = row_count();
  int k;
  if (n == 0) return SEARCH_NOT_FOUND;
  /* The cursor may sit on the phantom line past the last row; anchor the
   * sweep to a real row in that case. */
  int cur = (CUR.cy >= n) ? 0 : CUR.cy;
  /* SEARCH_WRAP runs k to n inclusive, so the starting row is visited a
   * second time at the end of the wrap. Only then is it scanned from its
   * beginning, which is what makes a match earlier on the cursor's own row
   * reachable. SEARCH_ONE_PASS drops that extra pass, and SEARCH_NO_WRAP
   * additionally stops the sweep at the end of the file (or at row 0 when
   * searching backward), so no row behind the cursor is ever scanned. */
  int kmax;
  if (sweep == SEARCH_NO_WRAP)
    kmax = dir > 0 ? n - 1 - cur : cur;
  else if (sweep == SEARCH_ONE_PASS)
    kmax = n - 1;
  else
    kmax = n;
  for (k = 0; k <= kmax; k++) {
    int i = dir > 0 ? (cur + k) % n : ((cur - k) % n + n) % n;
    erow *row = row_at(i);
    int maxstart = row->size - qlen;
    int on_start_row = (i == CUR.cy) && (k == 0);

    if (re != NULL) {
      struct regex_match m;
      int got;
      if (dir > 0) {
        int from = !on_start_row ? 0
                 : (exclude_current ? next_char_offset(row, CUR.cx) : CUR.cx);
        got = (from > row->size) ? 0 : regex_row_first(re, row, from, &m);
      } else {
        int limit = !on_start_row ? row->size
                  : (exclude_current ? CUR.cx - 1 : CUR.cx);
        got = (limit < 0) ? 0 : regex_row_last(re, row, limit, &m);
      }
      if (got < 0) {
        CUR.cx = saved_cx;
        CUR.cy = saved_cy;
        V.coloff = saved_coloff;
        V.drowoff = saved_drowoff;
        return SEARCH_TOO_HARD;
      }
      if (got) {
        CUR.cy = i;
        CUR.cx = m.so[0];
        if (out) *out = m;
        return SEARCH_FOUND;
      }
      continue;
    }

    if (dir > 0) {
      int start = !on_start_row ? 0
                : (exclude_current ? next_char_offset(row, CUR.cx) : CUR.cx);
      int col;
      for (col = start; col <= maxstart; col++) {
        if (bytes_match(row->chars + col, query, qlen) &&
            match_is_char_aligned(row->chars + col, qlen)) {
          CUR.cy = i;
          CUR.cx = col;
          return SEARCH_FOUND;
        }
      }
    } else {
      int start = !on_start_row ? maxstart
                : (exclude_current ? CUR.cx - 1 : CUR.cx);
      int col;
      if (start > maxstart) start = maxstart;
      for (col = start; col >= 0; col--) {
        if (bytes_match(row->chars + col, query, qlen) &&
            match_is_char_aligned(row->chars + col, qlen)) {
          CUR.cy = i;
          CUR.cx = col;
          return SEARCH_FOUND;
        }
      }
    }
  }

  CUR.cx = saved_cx;
  CUR.cy = saved_cy;
  V.coloff = saved_coloff;
  V.drowoff = saved_drowoff;
  return SEARCH_NOT_FOUND;
}

/* What a search that did not find anything has to say for itself. The cursor is
 * back where it started either way, so "too complex" cannot point at the line it
 * gave up on and does not try to. */
static void search_report_miss(int r, const char *advice) {
  if (r == SEARCH_TOO_HARD)
    editor_set_status_message("Pattern too complex. Try a simpler one");
  else
    editor_set_status_message("%s", advice);
}

void editor_find(void) {
  char *query = editor_prompt(search_use_regex
      ? "Pattern: %s (ESC to cancel, Enter to find)"
      : "Search: %s (ESC to cancel, Enter to find)", NULL);
  const char *term;
  struct regex *re = NULL;
  int excl, r;

  if (query == NULL) return;

  if (query[0] != '\0') {
    editor_set_last_query(query);
  } else if (last_query == NULL) {
    editor_set_status_message("No previous search. Type a term to search");
    free(query);
    return;
  }

  term = query[0] != '\0' ? query : last_query;
  excl = (query[0] == '\0');
  if (search_use_regex && (re = search_compile(term)) == NULL) {
    free(query);
    return;
  }
  r = editor_search_string(term, (int)strlen(term), re, 1, excl, SEARCH_WRAP,
                           NULL);
  regex_free(re);
  if (r == SEARCH_FOUND)
    editor_set_status_message("Found");
  else
    search_report_miss(r, "Not found. Press Ctrl-N to search again");
  free(query);
}

/* Find next and find previous, which differ only in direction and in what they
 * suggest trying instead. */
static void editor_find_again(int dir) {
  struct regex *re = NULL;
  int r;

  if (last_query == NULL) {
    editor_set_status_message("No previous search. Press Ctrl-F to search");
    return;
  }
  if (search_use_regex && (re = search_compile(last_query)) == NULL) return;
  r = editor_search_string(last_query, (int)strlen(last_query), re, dir, 1,
                           SEARCH_WRAP, NULL);
  regex_free(re);
  if (r == SEARCH_FOUND)
    editor_set_status_message("Found (%s)", dir > 0 ? "next" : "previous");
  else
    search_report_miss(r, dir > 0
        ? "Not found. Press Ctrl-P to search backward"
        : "Not found. Press Ctrl-N to search forward");
}

void editor_find_next(void) {
  editor_find_again(1);
}

void editor_find_prev(void) {
  editor_find_again(-1);
}

void editor_toggle_case_sensitive(void) {
  search_case_sensitive = !search_case_sensitive;
  editor_set_status_message("Search is now case-%s",
      search_case_sensitive ? "sensitive" : "insensitive");
}

void editor_goto_line(void) {
  char *input = editor_prompt("Go to line: %s (ESC to cancel)", NULL);
  if (input == NULL) return;

  char *end;
  long line = strtol(input, &end, 10);
  int valid = (end != input) && (*end == '\0');
  free(input);
  if (!valid) {
    editor_set_status_message("Go to line: not a number");
    return;
  }

  /* Clamp into the file's real rows, not the phantom line past the last
   * row (CUR.cy == row_count()) that an unclamped high value would otherwise
   * be allowed to land on for an empty request like "line 999999". */
  int max_cy = (row_count() > 0) ? row_count() - 1 : 0;
  int target = (int)line - 1;
  if (target < 0) target = 0;
  if (target > max_cy) target = max_cy;
  CUR.cy = target;
  CUR.cx = 0;
  editor_set_status_message("Ln %d", CUR.cy + 1);
}

void editor_replace(void) {
  struct regex *re = NULL;
  struct regex_match m;
  int r;

  char *query = editor_prompt(search_use_regex
      ? "Replace - pattern: %s (ESC to cancel)"
      : "Replace - search: %s (ESC to cancel)", NULL);
  if (query == NULL) return;
  if (query[0] == '\0') {
    editor_set_status_message("Replace aborted: empty search term");
    free(query);
    return;
  }

  /* Compiled before the replacement is asked for, so that a pattern with a typo
   * in it is reported before the user types what to replace it with. */
  if (search_use_regex && (re = search_compile(query)) == NULL) {
    free(query);
    return;
  }

  char *repl = editor_prompt(re != NULL
      ? "Replace with: %s (\\1 for a group, ESC to cancel)"
      : "Replace with: %s (ESC to cancel)", NULL);
  if (repl == NULL) {
    free(query);
    regex_free(re);
    editor_set_status_message("Replace aborted");
    return;
  }

  char *mode = editor_prompt("Replace: (n)ext or (a)ll? %s (ESC to cancel)",
                             NULL);
  /* Esc cancels, as it does at every other prompt. It used to mean "next",
   * which made this the one place in molly where cancelling did something. */
  if (mode == NULL) {
    free(query);
    free(repl);
    regex_free(re);
    editor_set_status_message("Replace aborted");
    return;
  }
  int all = (mode[0] == 'a' || mode[0] == 'A');
  free(mode);

  int qlen = (int)strlen(query);
  size_t rlen = strlen(repl);
  int count = 0;
  int grouped = 0;
  const char *trouble = NULL;

  if (all) {
    CUR.cy = 0;
    CUR.cx = 0;
  }

  /* replace-all sweeps with SEARCH_NO_WRAP: the cursor only ever moves
   * forward and the scan restarts strictly behind it, so the replacement
   * text -- even when it contains the search term -- can never be matched
   * again, and with only finitely many rows ahead of the cursor the loop
   * provably terminates. The cursor reset to (0, 0) above makes that
   * forward-only sweep cover the whole file. replace-next keeps the
   * one-pass wrap, so it reaches matches on the rows before the cursor. */
  const enum search_sweep sweep = all ? SEARCH_NO_WRAP : SEARCH_ONE_PASS;
  while ((r = editor_search_string(query, qlen, re, 1, 0, sweep, &m))
             == SEARCH_FOUND) {
    erow *row = row_at(CUR.cy);
    int cx_start = CUR.cx;
    char *expanded = NULL;
    const char *ins = repl;
    size_t ilen = rlen;
    int mlen;

    if (re != NULL) {
      /* The match is against this row's stored bytes, so its offsets are cursor
       * positions and its length needs no conversion. The expansion is taken
       * before anything is edited, because it reads the row the match was found
       * in and the edit is about to change it. */
      int elen = 0;
      mlen = m.eo[0] - m.so[0];
      expanded = regex_expand(repl, row->chars, &m, &elen, &trouble);
      if (expanded == NULL) break;
      ins = expanded;
      ilen = (size_t)elen;
    } else {
      /* A plain match is as long as the thing matched, now that both are
       * measured in the row's own bytes. */
      mlen = qlen;
    }

    if (!grouped) {
      undo_group_begin();
      grouped = 1;
    }

    if (mlen > 0) {
      struct uchange *ud = uc_new(UC_DELETE, CUR.cy, cx_start,
          row->chars + cx_start, (size_t)mlen);
      ud->back = 0;
      ud->cx0 = cx_start;
      ud->cy0 = CUR.cy;
      ud->cx1 = cx_start;
      ud->cy1 = CUR.cy;
      undo_push(ud);
      editor_row_delete_n(row, cx_start, mlen);
    }

    if (ilen > 0) {
      struct uchange *ui = uc_new(UC_INSERT, CUR.cy, cx_start, ins, ilen);
      ui->cx0 = cx_start;
      ui->cy0 = CUR.cy;
      ui->cx1 = cx_start + (int)ilen;
      ui->cy1 = CUR.cy;
      undo_push(ui);
      editor_row_insert_n(row, cx_start, ins, ilen);
    }

    free(expanded);
    count++;
    CUR.cx = cx_start + (int)ilen;

    /* A pattern can match nothing at all -- x* matches between every pair of
     * characters -- and then nothing was deleted and the same empty match is
     * still sitting at the cursor. Stepping over one character is what makes the
     * sweep finish; it is also what sed does, so x* replaced with - puts a dash
     * before every character and one at the end. */
    if (all && mlen == 0) {
      erow *at = row_at(CUR.cy);
      if (CUR.cx < at->size) {
        CUR.cx = next_char_offset(at, CUR.cx);
      } else if (CUR.cy + 1 < row_count()) {
        CUR.cy++;
        CUR.cx = 0;
      } else {
        break;
      }
    }

    if (!all) break;
  }
  if (grouped) undo_group_end();

  free(query);
  free(repl);
  regex_free(re);

  if (trouble != NULL)
    editor_set_status_message("Bad replacement: %s", trouble);
  else if (r == SEARCH_TOO_HARD && count > 0)
    editor_set_status_message("Pattern too complex. Stopped after %d "
        "replacement%s", count, count == 1 ? "" : "s");
  else if (r == SEARCH_TOO_HARD)
    editor_set_status_message("Pattern too complex. Try a simpler one");
  else if (count == 0)
    editor_set_status_message("No matches");
  else
    editor_set_status_message("Replaced %d occurrence%s", count,
        count == 1 ? "" : "s");
}
