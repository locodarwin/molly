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

/* The two search flags, set rather than toggled. The configuration file and the
 * Find dialog's checkboxes both want to say what the flag *is* rather than flip
 * it, and nothing outside this file should be reaching into the statics. */
void search_set_case(int on) { search_case_sensitive = on ? 1 : 0; }
void search_set_regex(int on) { search_use_regex = on ? 1 : 0; }

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

/* --- the Find dialog --------------------------------------------------------
 *
 * Ctrl-F used to open a prompt on the message row, which can ask one thing at a
 * time: the term, and nothing about how to look for it. The two settings that
 * decide what the term *means* lived on Ctrl-T and Alt-R, where you had to know
 * they were. A dialog puts all three in one place, and that is most of the
 * argument for having built a widget toolkit at all.
 *
 * The checkboxes start from the current modes and are committed only by Find, so
 * cancelling leaves everything as it was. Ctrl-T and Alt-R stay as they are: the
 * way to change a mode without searching.
 *
 * The field starts empty rather than pre-filled with the last term. A dialog
 * that pre-fills normally selects what it pre-filled, so that typing replaces
 * it; molly's field has no selection yet, so a pre-filled one would have to be
 * cleared by hand before it could be retyped. Empty keeps the prompt's own
 * bargain -- Enter on an empty field repeats the last search -- and the hint row
 * says so, which is more than the prompt ever did. */

enum {
  FIND_LABEL = 0,
  FIND_FIELD,
  FIND_CASE,
  FIND_REGEX,
  FIND_HINT,
  FIND_FIND,
  FIND_CANCEL,
  FIND_NWIDGETS
};

/* Returns the term to search for, or NULL if the dialog was cancelled. Commits
 * the two settings on the way out, and only then. */
static char *find_dialog(void) {
  struct widget w[FIND_NWIDGETS];
  struct dialog d;
  struct layer l;
  char hint[256];
  char *answer = NULL;
  int depth, li, done = 0;

  widget_label(&w[FIND_LABEL], 0, 0, 1, "Find what:");
  widget_input(&w[FIND_FIELD], 0, 1, 1);
  widget_check(&w[FIND_CASE], 0, 2, 1, "Match case", search_case_sensitive);
  widget_check(&w[FIND_REGEX], 0, 3, 1, "Regular expression", search_use_regex);
  widget_label(&w[FIND_HINT], 0, 4, 1, "");
  widget_button(&w[FIND_FIND], 0, 5, "Find", 1);
  widget_button(&w[FIND_CANCEL], 0, 5, "Cancel", 0);

  if (search_has_query())
    snprintf(hint, sizeof hint, "Enter alone repeats \"%s\"", last_query);
  else
    snprintf(hint, sizeof hint, "Tab moves   Space ticks   Esc cancels");
  w[FIND_HINT].text = hint;

  depth = layer_count();
  memset(&l, 0, sizeof(l));
  li = layer_push(&l);
  if (li < 0) {
    widget_input_free(&w[FIND_FIELD]);
    return NULL;
  }

  /* Once, outside the loop: dialog_init zeroes the dialog, and the focus is
   * state that has to survive a redraw. */
  dialog_init(&d, "Find", w, FIND_NWIDGETS, 1, 1);
  d.focus = FIND_FIELD;

  while (!done) {
    struct layer *lp;
    int cols, contentw, bw, bx, y_btn, c;

    /* Before measuring anything: see screen_sync_size. */
    screen_sync_size();
    cols = grid_cols() - 8;
    if (cols > 52) cols = 52;
    if (cols < 24) cols = 24;
    d.cols = cols - 2;
    d.rows = 6;
    lp = layer_get(li);
    dialog_layout(&d, lp);

    /* Widths come from what the layout *granted*, not from what was asked for: on
     * a terminal narrower than the floor above, the dialog is clamped and a widget
     * sized to the request would run through the frame. */
    contentw = d.cols - 2;
    if (contentw < 1) contentw = 1;
    w[FIND_LABEL].w = contentw;
    w[FIND_FIELD].w = contentw;
    w[FIND_CASE].w = contentw;
    w[FIND_REGEX].w = contentw;
    w[FIND_HINT].w = contentw;

    /* And so do the rows. The buttons keep the last row whatever happens, because
     * a dialog whose buttons are off the bottom looks broken even when Enter still
     * presses the default one; the hint goes first when there is no room for
     * everything, and then a setting, which is worth less than being able to see
     * what you are about to press. */
    y_btn = d.rows - 1;
    if (y_btn > 5) y_btn = 5;
    w[FIND_CASE].y = 2;
    w[FIND_REGEX].y = 3;
    w[FIND_HINT].y = (y_btn >= 5) ? 4 : d.rows;
    if (w[FIND_CASE].y >= y_btn) w[FIND_CASE].y = d.rows;
    if (w[FIND_REGEX].y >= y_btn) w[FIND_REGEX].y = d.rows;
    w[FIND_FIND].y = y_btn;
    w[FIND_CANCEL].y = y_btn;

    /* The two buttons centred as a pair, with two columns between them -- or Find
     * alone when the pair will not fit, since Esc cancels and a Cancel button
     * sawn off at the frame is worse than no Cancel button. */
    bw = w[FIND_FIND].w + 2 + w[FIND_CANCEL].w;
    if (bw > contentw) {
      w[FIND_CANCEL].y = d.rows;
      bw = w[FIND_FIND].w;
    }
    bx = (contentw - bw) / 2;
    if (bx < 0) bx = 0;
    w[FIND_FIND].x = bx;
    w[FIND_CANCEL].x = bx + w[FIND_FIND].w + 2;

    editor_refresh_screen();

    c = ui_wait_key();
    if (c == WINCH_KEY) continue;

    if (c == MOUSE_KEY) {
      struct mouse_event ev;
      input_last_mouse(&ev);
      if (dialog_mouse(&d, lp, &ev) != DIALOG_PRESSED) continue;
    } else {
      int r = dialog_key(&d, c);
      if (r == DIALOG_CANCEL) break;
      if (r != DIALOG_PRESSED) continue;
    }

    if (d.pressed == FIND_FIND) {
      answer = xstrdup(widget_input_text(&w[FIND_FIELD]));
      search_set_case(w[FIND_CASE].on);
      search_set_regex(w[FIND_REGEX].on);
    }
    done = 1;
  }

  widget_input_free(&w[FIND_FIELD]);
  layer_pop_to(depth);
  return answer;
}

void editor_find(void) {
  char *query = find_dialog();
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
  {
    /* Where the search started, for Alt-Left -- recorded only if it found
     * something, so a miss leaves no step back to take. */
    int sy = CUR.cy, sx = CUR.cx, sb = B.id;
  if (search_use_regex && (re = search_compile(term)) == NULL) {
    free(query);
    return;
  }
  r = editor_search_string(term, (int)strlen(term), re, 1, excl, SEARCH_WRAP,
                           NULL);
  if (r == SEARCH_FOUND && (CUR.cy != sy || CUR.cx != sx)) nav_record(sb, sy, sx);
  }
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
  nav_push();
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
      /* This edit is a new future: whatever was on the redo stack described a
       * buffer that no longer exists. Every path in edit.c clears it, and this
       * one -- which writes rows through the buffer primitives rather than
       * through editor_insert_char() -- did not, so after an undo a replace
       * left the undone record there for Ctrl-Y to put back on top of the
       * replaced text. */
      clear_redo();
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

/* The column of the first match of `query` in row `cy`, or -1, read the way
 * Find would read it -- plain or pattern, folded or not, by the toggles. The
 * query becomes the remembered one, so F3 goes on to the next match from
 * wherever the caller puts the cursor.
 *
 * For find in files, which knows the row a match is on from `grep` and wants
 * the column. grep's -E is not molly's dialect, so the two can disagree about a
 * pattern; this is molly's answer, and -1 means the caller keeps column 0. */
int search_find_in_row(int cy, const char *query) {
  erow *row;
  int qlen, i;

  editor_set_last_query(query);
  if (query == NULL || query[0] == '\0' || cy < 0 || cy >= row_count())
    return -1;
  row = row_at(cy);
  if (search_use_regex) {
    struct regex_match m;
    struct regex *re = search_compile(query);
    int r;
    if (re == NULL) return -1;
    r = regex_row_first(re, row, 0, &m);
    regex_free(re);
    return r == 1 ? m.so[0] : -1;
  }
  qlen = (int)strlen(query);
  for (i = 0; i + qlen <= row->size; i++)
    if (bytes_match(row->chars + i, query, qlen) &&
        match_is_char_aligned(row->chars + i, qlen))
      return i;
  return -1;
}
