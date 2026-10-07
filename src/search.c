/* search.c -- Finding and replacing text, and going to a line. */

#include "molly.h"

static char *last_query = NULL;

/* Off by default, matching the search behavior this project already
 * shipped; toggled globally with Ctrl-T rather than per-prompt, since
 * editor_prompt's format string is fixed for the life of the prompt and
 * has no channel back to the caller besides the typed buffer. */
static int search_case_sensitive = 0;

/* Whether there is a term to find again, which is what greys out Find Next and
 * Find Previous until the first search. */
int search_has_query(void) {
  return last_query != NULL && last_query[0] != '\0';
}

int search_case_is_on(void) {
  return search_case_sensitive;
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

static int render_match(const char *hay, const char *needle, int n) {
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
 * character in half). render is NUL-terminated, so hay[n] is in bounds and
 * reads as a non-continuation byte at end of row. */
static int match_is_char_aligned(const char *hay, int n) {
  return !utf8_is_cont((unsigned char)hay[0]) &&
         !utf8_is_cont((unsigned char)hay[n]);
}

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
 * the search term, looping forever as the row grows without bound. */
static int editor_search_string(const char *query, int qlen, int dir,
    int exclude_current, enum search_sweep sweep) {
  int saved_cx = CUR.cx;
  int saved_cy = CUR.cy;
  int saved_coloff = V.coloff;
  int saved_drowoff = V.drowoff;

  int n = row_count();
  int k;
  if (n == 0) return 0;
  /* The cursor may sit on the phantom line past the last row; anchor the
   * sweep to a real row in that case. */
  int cur = (CUR.cy >= n) ? 0 : CUR.cy;
  /* The cursor's position as a render byte offset -- render is what the
   * search scans, and the cursor's display column (CUR.rx) is no longer the
   * same thing once a line holds wide or combining characters. */
  int cur_rb = editor_row_cx_to_rbyte(row_at(cur), CUR.cx);
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
    int maxstart = row->rsize - qlen;
    int on_start_row = (i == CUR.cy) && (k == 0);
    if (dir > 0) {
      int start = on_start_row ? (exclude_current ? cur_rb + 1 : cur_rb) : 0;
      int col;
      for (col = start; col <= maxstart; col++) {
        if (render_match(row->render + col, query, qlen) &&
            match_is_char_aligned(row->render + col, qlen)) {
          CUR.cy = i;
          CUR.cx = editor_render_byte_to_cx(row, col);
          return 1;
        }
      }
    } else {
      int start = on_start_row ? (exclude_current ? cur_rb - 1 : cur_rb)
                               : maxstart;
      if (start > maxstart) start = maxstart;
      int col;
      for (col = start; col >= 0; col--) {
        if (render_match(row->render + col, query, qlen) &&
            match_is_char_aligned(row->render + col, qlen)) {
          CUR.cy = i;
          CUR.cx = editor_render_byte_to_cx(row, col);
          return 1;
        }
      }
    }
  }

  CUR.cx = saved_cx;
  CUR.cy = saved_cy;
  V.coloff = saved_coloff;
  V.drowoff = saved_drowoff;
  return 0;
}

void editor_find(void) {
  char *query = editor_prompt("Search: %s (ESC to cancel, Enter to find)", NULL);
  if (query == NULL) return;

  if (query[0] != '\0') {
    editor_set_last_query(query);
  } else if (last_query == NULL) {
    editor_set_status_message("No previous search. Type a term to search");
    free(query);
    return;
  }

  const char *term = query[0] != '\0' ? query : last_query;
  int excl = (query[0] == '\0');
  if (editor_search_string(term, strlen(term), 1, excl, SEARCH_WRAP))
    editor_set_status_message("Found");
  else
    editor_set_status_message("Not found. Press Ctrl-N to search again");
  free(query);
}

void editor_find_next(void) {
  if (last_query == NULL) {
    editor_set_status_message("No previous search. Press Ctrl-F to search");
    return;
  }
  if (editor_search_string(last_query, strlen(last_query), 1, 1, SEARCH_WRAP))
    editor_set_status_message("Found (next)");
  else
    editor_set_status_message("Not found. Press Ctrl-P to search backward");
}

void editor_find_prev(void) {
  if (last_query == NULL) {
    editor_set_status_message("No previous search. Press Ctrl-F to search");
    return;
  }
  if (editor_search_string(last_query, strlen(last_query), -1, 1, SEARCH_WRAP))
    editor_set_status_message("Found (previous)");
  else
    editor_set_status_message("Not found. Press Ctrl-N to search forward");
}

void editor_toggle_case_sensitive(void) {
  search_case_sensitive = !search_case_sensitive;
  editor_set_status_message("Search is now case-%s",
      search_case_sensitive ? "sensitive" : "insensitive");
}

/* Length, in chars-space bytes, of a match that started at cx in row and is
 * qlen bytes long in render-space. The two spaces diverge whenever a tab is
 * in play (one byte in chars, up to KILO_TAB_STOP in render), so the match
 * length cannot simply be reused as the delete length -- it must be
 * remeasured through the same rbyte<->cx mapping search itself uses. */
static int editor_match_char_len(erow *row, int cx, int qlen) {
  int rb = editor_row_cx_to_rbyte(row, cx);
  int cx_end = editor_render_byte_to_cx(row, rb + qlen);
  return cx_end - cx;
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
  char *query = editor_prompt("Replace - search: %s (ESC to cancel)", NULL);
  if (query == NULL) return;
  if (query[0] == '\0') {
    editor_set_status_message("Replace aborted: empty search term");
    free(query);
    return;
  }

  char *repl = editor_prompt("Replace with: %s (ESC to cancel)", NULL);
  if (repl == NULL) {
    free(query);
    editor_set_status_message("Replace aborted");
    return;
  }

  char *mode = editor_prompt("Replace: (n)ext or (a)ll? %s", NULL);
  int all = (mode != NULL && (mode[0] == 'a' || mode[0] == 'A'));
  free(mode);

  int qlen = (int)strlen(query);
  size_t rlen = strlen(repl);
  int count = 0;
  int grouped = 0;

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
  while (editor_search_string(query, qlen, 1, 0, sweep)) {
    if (!grouped) {
      undo_group_begin();
      grouped = 1;
    }

    erow *row = row_at(CUR.cy);
    int cx_start = CUR.cx;
    int mlen = editor_match_char_len(row, cx_start, qlen);

    struct uchange *ud = uc_new(UC_DELETE, CUR.cy, cx_start,
        row->chars + cx_start, (size_t)mlen);
    ud->back = 0;
    ud->cx0 = cx_start;
    ud->cy0 = CUR.cy;
    ud->cx1 = cx_start;
    ud->cy1 = CUR.cy;
    undo_push(ud);
    editor_row_delete_n(row, cx_start, mlen);

    struct uchange *ui = uc_new(UC_INSERT, CUR.cy, cx_start, repl, rlen);
    ui->cx0 = cx_start;
    ui->cy0 = CUR.cy;
    ui->cx1 = cx_start + (int)rlen;
    ui->cy1 = CUR.cy;
    undo_push(ui);
    editor_row_insert_n(row, cx_start, repl, rlen);

    count++;
    CUR.cx = cx_start + (int)rlen;

    if (!all) break;
  }
  if (grouped) undo_group_end();

  free(query);
  free(repl);

  if (count == 0)
    editor_set_status_message("No matches");
  else
    editor_set_status_message("Replaced %d occurrence%s", count,
        count == 1 ? "" : "s");
}
