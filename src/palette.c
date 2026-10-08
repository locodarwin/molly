/* palette.c -- the command palette.
 *
 * One key, a fuzzy filter over every command molly has, and the chord each one is
 * bound to shown down the right-hand side. The roadmap predicted this would be nearly
 * free once the command table and the list box existed, and it was: the table already
 * holds the labels, the keymap already knows the chords, the list box already scrolls
 * and the input field already edits. What is actually written here is the matcher and
 * a dialog around it.
 *
 * It is the answer to a problem the menu bar only half solves. A menu is good at
 * showing you what exists, but it has to be curated -- six menus list eighteen of the
 * thirty-odd commands, and the long tail will only get longer. The palette lists
 * everything, with no second place to keep a name in step: a command is in the
 * palette the moment it is in the table.
 *
 * Unavailable commands are listed rather than hidden, dimmed exactly as a menu greys
 * them, because a palette that silently omits Paste when the clipboard is empty looks
 * broken rather than careful. Choosing one says why it cannot run and stays open. */

#include "molly.h"

/* What matched, and how well. `order` is the command's position in the table, which
 * is the final tiebreak -- without it a sort over equal scores would depend on
 * qsort's internals and the list would reshuffle itself between keystrokes. */
struct palette_hit {
  const struct command *cmd;
  int label_match;   /* the label matched, rather than only the name or the group */
  int score;
  int order;
  int cols;          /* the label's display columns, for the shorter-wins tiebreak */
};

struct palette_pane {
  struct palette_hit *hits;
  int n;
  int width;         /* the list's width in columns, for the right-aligned chord */
};

/* --- the matcher ---------------------------------------------------------- */

/* Characters a word can start after. A command label is words separated by spaces;
 * a command name is words separated by dots. */
static int is_sep(unsigned char c) {
  return !isalnum(c);
}

static int fold(unsigned char c) {
  return tolower(c);
}

/* One pass of the matcher. `prefer_word_start` decides what to do when the next
 * occurrence of a pattern character is stranded in the middle of a word and a later
 * one begins a word: the plain pass takes the first, this one skips to the later.
 * Returns 0 if the pattern is not a subsequence at all, else its score.
 *
 * The run-up to the first match is penalised, capped, so a prefix beats a mention
 * halfway along without a long path being able to grow the penalty without limit. */
static int match_pass(const char *s, const char *pat, int prefer_word_start) {
  int i = 0, j = 0, score = 0, first = -1, prev = -2, run = 0;

  while (pat[j] != '\0') {
    int at = -1, k;
    for (k = i; s[k] != '\0'; k++) {
      if (fold((unsigned char)s[k]) != fold((unsigned char)pat[j])) continue;
      at = k;
      break;
    }
    if (at < 0) return 0;
    if (prefer_word_start && at > 0 && !is_sep((unsigned char)s[at - 1])) {
      for (k = at + 1; s[k] != '\0'; k++) {
        if (fold((unsigned char)s[k]) != fold((unsigned char)pat[j])) continue;
        if (is_sep((unsigned char)s[k - 1])) { at = k; break; }
      }
    }
    if (first < 0) first = at;
    if (at == 0) {
      score += 12;                                      /* starts the string */
      run = 0;
    } else if (is_sep((unsigned char)s[at - 1])) {
      score += 10;                                      /* starts a word */
      run = 0;
    } else if (prev == at - 1) {
      /* Continues the match, and the longer the run the better: a run of them is
       * someone typing the start of a word, and should beat the same letters picked
       * out of three different words. "red" means Redraw, not Reorder dates. */
      run++;
      score += 8 + 2 * run;
    } else {
      score += 2;                                       /* stranded mid-word */
      run = 0;
    }
    prev = at;
    i = at + 1;
    j++;
  }

  score -= (first < 8) ? first : 8;
  return score > 0 ? score : 1;
}

/* Is `pat` a subsequence of `s`, ignoring case, and how good a match is it? Returns 0
 * for no match, else a positive score.
 *
 * The scoring is what decides whether the palette feels right or feels random: typing
 * "sav" should put Save at the top, not the first command that happens to contain an
 * s, an a and a v. So a character that starts a word scores most, one that continues
 * the previous match next, and a letter stranded in the middle of a word least.
 *
 * Both passes are run and the better taken, because neither alone is right. Matching
 * the first occurrence of each character is wrong for the way people actually
 * abbreviate -- "ln" means Line numbers, and the plain pass finds its n in "Line" and
 * scores it no better than Line end. Preferring the n that starts a word fixes that,
 * but it can skip past the only match there was ("aba" against "ab a b"), so it
 * cannot be the only pass. Two linear walks and the maximum gets both.
 *
 * Neither pass is an optimal matcher; that is a dynamic program, and this runs over
 * every command on every keystroke. If the ranking ever feels wrong in practice, that
 * is the point to pay for it, and not before.
 *
 * Exposed because the test suite checks the ranking directly, which is easier to be
 * sure of than inferring it from a screenful of list rows. */
int palette_score(const char *s, const char *pat) {
  int plain, preferred;

  if (s == NULL || pat == NULL) return 0;
  if (pat[0] == '\0') return 1;   /* an empty filter matches everything, equally */

  plain = match_pass(s, pat, 0);
  preferred = match_pass(s, pat, 1);
  return (preferred > plain) ? preferred : plain;
}

/* The best of a command's three matchable texts, and whether it was the label.
 *
 * The group is matched too, which is this field's first reader: it was written down
 * for the help screen and the help screen takes its headings from its own table, so
 * until now nothing read it. Matching it means "clip" finds Copy, Cut and Paste,
 * which are grouped as CLIPBOARD and say so nowhere in their labels. */
static void score_command(const struct command *c, const char *pat,
                          int *score, int *label_match) {
  const char *label = (c->label != NULL) ? c->label : c->name;
  int s = palette_score(label, pat);
  int alt;

  if (s > 0) {
    *score = s;
    *label_match = 1;
    return;
  }
  *label_match = 0;
  *score = palette_score(c->name, pat);
  if (c->group != NULL) {
    alt = palette_score(c->group, pat);
    if (alt > *score) *score = alt;
  }
}

/* Best first. Label matches outrank name-and-group-only matches however well those
 * scored, because a name match is a guess at what the user meant and a label match is
 * what they can see. */
static int hit_cmp(const void *va, const void *vb) {
  const struct palette_hit *a = va, *b = vb;
  if (a->label_match != b->label_match) return b->label_match - a->label_match;
  if (a->score != b->score) return b->score - a->score;
  if (a->cols != b->cols) return a->cols - b->cols;   /* the shorter label wins */
  return a->order - b->order;
}

/* Rebuild the hit list for `pat`. Commands with no label are mechanism with no name
 * worth showing (the resize pseudo-command); CMD_HIDDEN ones are the menu openers and
 * the palette itself, which have no business being listed. */
static void palette_filter(struct palette_pane *pp, const char *pat) {
  int i, n = cmd_count();
  pp->n = 0;
  for (i = 0; i < n; i++) {
    const struct command *c = cmd_at(i);
    int score, label_match;
    if (c->label == NULL || (c->flags & CMD_HIDDEN)) continue;
    score_command(c, pat, &score, &label_match);
    if (score <= 0) continue;
    pp->hits[pp->n].cmd = c;
    pp->hits[pp->n].score = score;
    pp->hits[pp->n].label_match = label_match;
    pp->hits[pp->n].order = i;
    pp->hits[pp->n].cols = utf8_str_cols(c->label);
    pp->n++;
  }
  /* An empty filter is not a ranking: every command matches it equally well, so the
   * list keeps the table's order, which already groups related commands together. */
  if (pat[0] != '\0')
    qsort(pp->hits, (size_t)pp->n, sizeof(pp->hits[0]), hit_cmp);
}

/* --- the list ------------------------------------------------------------- */

/* One row: the label, then the chord it is bound to against the right-hand edge.
 * Padded in display columns and not bytes, because "Command palette…" is nineteen
 * bytes and seventeen columns and padding it by bytes puts the chord column two to
 * the left of every other row's. */
static void palette_row(void *user, int i, char *dst, size_t dsz) {
  struct palette_pane *pp = user;
  const struct command *c = pp->hits[i].cmd;
  char chord[32];
  size_t out;
  int pad, k;

  snprintf(dst, dsz, "%s", c->label);
  if (!cmd_shortcut(c->name, chord, sizeof(chord))) return;

  pad = pp->width - pp->hits[i].cols - utf8_str_cols(chord);
  if (pad < 2) pad = 2;
  out = strlen(dst);
  for (k = 0; k < pad && out + 1 < dsz; k++) dst[out++] = ' ';
  dst[out] = '\0';
  snprintf(dst + out, dsz - out, "%s", chord);
}

static unsigned char palette_row_role(void *user, int i) {
  struct palette_pane *pp = user;
  const struct command *c = pp->hits[i].cmd;
  if (c->enabled != NULL && !c->enabled()) return ROLE_MENU_DISABLED;
  return ROLE_DIALOG;
}

/* --- the dialog ----------------------------------------------------------- */

/* Open the palette and run whatever is chosen.
 *
 * Returns the chosen command's dispatcher flags, as menu_open does, so a command run
 * from here behaves exactly as it does from its key -- Ctrl-Q's countdown included --
 * and the command runs after the dialog's layer is popped, so one that opens a dialog
 * of its own does not stack it on top of a palette that is about to vanish. */
int palette_open(void) {
  struct palette_pane pp;
  struct widget widgets[4];
  struct dialog d;
  struct layer l;
  const struct command *chosen = NULL;
  const char *msg = "";
  char *last;
  int depth, li, ncmds = cmd_count();

  pp.hits = xmalloc(sizeof(struct palette_hit) * (size_t)(ncmds > 0 ? ncmds : 1));
  pp.width = 1;
  pp.n = 0;

  widget_label(&widgets[0], 0, 0, 1, "\xe2\x80\xba");   /* U+203A, a single guillemet */
  widget_input(&widgets[1], 2, 0, 1);
  widget_list(&widgets[2], 0, 1, 1, 1, 0, palette_row, &pp);
  widgets[2].row_role = palette_row_role;
  widget_label(&widgets[3], 0, 2, 1, "");

  last = xstrdup("");
  palette_filter(&pp, last);

  depth = layer_count();
  memset(&l, 0, sizeof(l));
  li = layer_push(&l);
  if (li < 0) {
    widget_input_free(&widgets[1]);
    free(pp.hits);
    free(last);
    return 0;
  }

  for (;;) {
    struct layer *lp;
    int cols, rows, list_h, contentw, c;

    /* Before measuring anything: see screen_sync_size. */
    screen_sync_size();

    /* Sized to the terminal with a ceiling, so it stays a dialog on a large screen
     * rather than growing into one. Wider than the open-file dialog is tall-ish:
     * a command label plus its chord needs the columns more than the rows. */
    cols = grid_cols() - 8;
    if (cols > 64) cols = 64;
    if (cols < 20) cols = 20;
    rows = grid_rows() - 4;
    if (rows > 18) rows = 18;
    if (rows < 6) rows = 6;
    contentw = cols - 4;
    list_h = rows - 2 - 2;   /* the frame, the field, and the hint line */
    if (list_h < 1) list_h = 1;

    /* The filter is only re-run when the text changed, so moving the selection does
     * not throw it away -- and so the selection survives a resize untouched. */
    if (strcmp(last, widget_input_text(&widgets[1])) != 0) {
      free(last);
      last = xstrdup(widget_input_text(&widgets[1]));
      palette_filter(&pp, last);
      widgets[2].sel = 0;
      widgets[2].top = 0;
    }

    pp.width = contentw;
    widgets[1].x = 2;
    widgets[1].w = contentw - 2;
    widgets[2].y = 1;
    widgets[2].w = contentw;
    widgets[2].h = list_h;
    widgets[2].count = pp.n;
    widgets[3].y = 1 + list_h;
    widgets[3].w = contentw;
    widgets[3].text = (msg[0] != '\0') ? msg
        : (pp.n > 0 ? "\xe2\x86\x91\xe2\x86\x93 choose   Enter run   Esc cancel"
                    : "No matching command");
    widget_list_clamp(&widgets[2]);

    dialog_init(&d, "Commands", widgets, 4, cols - 2, rows - 2);
    /* The palette has one typing target and drives its list itself, so the focus
     * never moves -- but it has to be *somewhere*, or the field would not claim
     * the cursor. */
    d.focus = 1;
    lp = layer_get(li);
    dialog_layout(&d, lp);
    editor_refresh_screen();

    c = ui_wait_key();
    msg = "";
    if (c == WINCH_KEY) continue;

    if (c == MOUSE_KEY) {
      struct mouse_event ev;
      input_last_mouse(&ev);
      if (ev.type == MOUSE_WHEEL_UP) {
        widget_list_move(&widgets[2], -3);
      } else if (ev.type == MOUSE_WHEEL_DOWN) {
        widget_list_move(&widgets[2], 3);
      } else if (ev.type == MOUSE_PRESS && ev.button == MOUSE_BUTTON_LEFT) {
        /* A click on a row chooses it, as a click on a menu item does: you clicked
         * the command you wanted, not the row you wanted highlighted. */
        int k = ev.y - (lp->y + 1 + widgets[2].y);
        if (k >= 0 && k < widgets[2].h && widgets[2].top + k < pp.n) {
          widgets[2].sel = widgets[2].top + k;
          chosen = pp.hits[widgets[2].sel].cmd;
          break;
        }
      }
      continue;
    }

    if (c == '\x1b' || c == CTRL_KEY('c')) break;

    /* The arrows move the list and Home/End edit the text, which is the one place
     * the two could have fought over a key. Typing is the primary activity in a
     * palette, so the text keeps Home and End; the list keeps PgUp and PgDn, which
     * the field has no use for. */
    if (c == ARROW_DOWN) {
      widget_list_move(&widgets[2], 1);
      continue;
    }
    if (c == ARROW_UP) {
      widget_list_move(&widgets[2], -1);
      continue;
    }
    if (c == PAGE_DOWN) {
      widget_list_move(&widgets[2], list_h);
      continue;
    }
    if (c == PAGE_UP) {
      widget_list_move(&widgets[2], -list_h);
      continue;
    }
    if (c == '\r') {
      const struct command *sel;
      if (pp.n == 0) continue;
      sel = pp.hits[widgets[2].sel].cmd;
      if (sel->enabled != NULL && !sel->enabled()) {
        msg = "Not available right now";
        continue;
      }
      chosen = sel;
      break;
    }
    widget_input_key(&widgets[1], c);
  }

  widget_input_free(&widgets[1]);
  free(pp.hits);
  free(last);
  layer_pop_to(depth);

  if (chosen != NULL && chosen->run != NULL) return chosen->run();
  return 0;
}
