/* undo.c -- Undo and redo: the change records, the two stacks, grouping and the caps. */

#include "molly.h"

struct uchange *uc_new(int kind, int row, int at, const char *text, size_t len) {
  struct uchange *u = xmalloc(sizeof(struct uchange));
  u->kind = kind;
  u->row = row;
  u->at = at;
  u->back = 0;
  u->len = len;
  u->text = xmalloc(len + 1);
  if (len) memcpy(u->text, text, len);
  u->text[len] = '\0';
  u->cx0 = 0;
  u->cy0 = 0;
  u->cx1 = 0;
  u->cy1 = 0;
  u->group = B.undo.active_group;
  u->crlf = 0;
  return u;
}

/* --- history caps ----------------------------------------------------------
 * Both stacks evict their oldest entries past the caps, so an editing bug
 * (or a pathological session) that pushes forever can degrade undo depth
 * but never consume unbounded memory. The step cap can be lowered through
 * MOLLY_UNDO_STEPS for testing. */
#define UNDO_MAX_STEPS 32768
#define UNDO_MAX_BYTES (16 * 1024 * 1024)
static int undo_max_steps_v = UNDO_MAX_STEPS;

static void undo_stack_trim(struct uchange **s, int *n, size_t *bytes);
void uc_free(struct uchange *u);

/* Whether there is anything to undo or redo, which is what grays out the two menu
 * items rather than letting them look available and do nothing. */
int undo_can_undo(void) {
  return B.undo.undo_n > 0;
}

int undo_can_redo(void) {
  return B.undo.redo_n > 0;
}

/* Lower the step cap, so that history eviction is reachable in a test without
 * editing for an hour. Only ever shrinks it. */
int undo_max_steps(void) { return undo_max_steps_v; }

void undo_set_max_steps(int n) {
  if (n > 0) undo_max_steps_v = n;
}

void undo_push(struct uchange *u) {
  if (B.undo.undo_n == B.undo.undo_cap) {
    B.undo.undo_cap = B.undo.undo_cap ? B.undo.undo_cap * 2 : 32;
    B.undo.undo = xrealloc(B.undo.undo, sizeof(struct uchange *) * (size_t)B.undo.undo_cap);
  }
  B.undo.undo[B.undo.undo_n++] = u;
  B.undo.undo_bytes += u->len + sizeof(struct uchange);
  undo_stack_trim(B.undo.undo, &B.undo.undo_n, &B.undo.undo_bytes);
}

void redo_push(struct uchange *u) {
  if (B.undo.redo_n == B.undo.redo_cap) {
    B.undo.redo_cap = B.undo.redo_cap ? B.undo.redo_cap * 2 : 32;
    B.undo.redo = xrealloc(B.undo.redo, sizeof(struct uchange *) * (size_t)B.undo.redo_cap);
  }
  B.undo.redo[B.undo.redo_n++] = u;
  B.undo.redo_bytes += u->len + sizeof(struct uchange);
  undo_stack_trim(B.undo.redo, &B.undo.redo_n, &B.undo.redo_bytes);
}

void clear_redo(void) {
  int i;
  for (i = 0; i < B.undo.redo_n; i++) {
    free(B.undo.redo[i]->text);
    free(B.undo.redo[i]);
  }
  B.undo.redo_n = 0;
  B.undo.redo_bytes = 0;
}

/* Multi-edit operations (replace-all) push several uchanges that must undo
 * and redo as one step. Every uchange created between begin/end is stamped
 * with the same group id (in uc_new); editor_undo/editor_redo then pop the
 * whole run in one call instead of stopping after the first entry. The undo
 * and redo stacks are LIFO, so popping the undo stack already visits a
 * group in the reverse of its push order -- and pushing those same popped
 * entries onto the redo stack, in that same pop order, leaves the redo
 * stack ready to replay the group in its original forward order. No extra
 * bookkeeping is needed for either direction. */
/* Groups nest, by depth rather than by id.
 *
 * They did not, and the shape that wanted them to is "replace this selection
 * with that text": the delete is a group of its own, so an operation built out
 * of selection_delete() plus a run of inserts came out as two undo steps.
 * Pasting over a selection was already doing that, which made README's claim
 * that a paste undoes as one step true only when nothing was selected. The
 * shell filter is the same shape and could not have been one step at all.
 *
 * The depth lives on the buffer rather than in a file static because
 * active_group does, and two records of one nesting in different places is how
 * they come to disagree. */
void undo_group_begin(void) {
  if (B.undo.group_depth++ > 0) return;
  B.undo.group_seq++;
  B.undo.active_group = B.undo.group_seq;
}

void undo_group_end(void) {
  if (B.undo.group_depth > 0) B.undo.group_depth--;
  if (B.undo.group_depth > 0) return;
  B.undo.active_group = 0;
  /* Without this, the next typed character could coalesce into the group's
   * trailing insert (see the !B.undo.locked check in editor_insert_char),
   * silently pulling an unrelated keystroke inside the group's undo step. */
  B.undo.locked = 1;
}

void editor_free_undo_redo(void) {
  int i;
  for (i = 0; i < B.undo.undo_n; i++) {
    free(B.undo.undo[i]->text);
    free(B.undo.undo[i]);
  }
  for (i = 0; i < B.undo.redo_n; i++) {
    free(B.undo.redo[i]->text);
    free(B.undo.redo[i]);
  }
  free(B.undo.undo);
  free(B.undo.redo);
  B.undo.undo = NULL;
  B.undo.redo = NULL;
  B.undo.undo_n = 0;
  B.undo.undo_cap = 0;
  B.undo.undo_bytes = 0;
  B.undo.redo_n = 0;
  B.undo.redo_cap = 0;
  B.undo.redo_bytes = 0;
  B.undo.locked = 0;
}

void uc_free(struct uchange *u) {
  free(u->text);
  free(u);
}

/* Evict oldest entries until both caps hold again. Dropping the bottom of
 * the stack can only truncate the oldest undo step (possibly the tail of a
 * group); the group walk on undo already stops at the stack bottom, so a
 * truncated group simply undoes fewer changes and stays consistent. */
static void undo_stack_trim(struct uchange **s, int *n, size_t *bytes) {
  while (*n > 1 && (*n > undo_max_steps_v || *bytes > UNDO_MAX_BYTES)) {
    *bytes -= s[0]->len + sizeof(struct uchange);
    uc_free(s[0]);
    memmove(s, s + 1, sizeof(struct uchange *) * (size_t)(*n - 1));
    (*n)--;
  }
}

void uc_do_insert(struct uchange *u) {
  editor_row_insert_n(row_at(u->row), u->at, u->text, u->len);
}

void uc_do_delete(struct uchange *u) {
  editor_row_delete_n(row_at(u->row), u->at, u->len);
}

/* A row's line break belongs to the end of its text: a join keeps the second
 * row's and loses the first's, and a split gives the old one to the new row and
 * the first row the one in the record. */
void uc_do_join(struct uchange *u) {
  unsigned char tail = row_at(u->row + 1)->crlf;
  editor_row_append_string(row_at(u->row), (*row_at(u->row + 1)).chars,
      (*row_at(u->row + 1)).size);
  editor_delete_row(u->row + 1);
  row_at(u->row)->crlf = tail;
}

void uc_do_split(struct uchange *u) {
  erow *row = row_at(u->row);
  unsigned char tail = row->crlf;
  editor_insert_row(u->row + 1, row->chars + u->at, row->size - u->at);
  editor_row_truncate(row_at(u->row), u->at);
  row_at(u->row + 1)->crlf = tail;
  row_at(u->row)->crlf = u->crlf;
}

void uc_do_newrow(struct uchange *u) {
  editor_insert_row(u->row, "", 0);
}

void uc_do_delrow(struct uchange *u) {
  editor_delete_row(u->row);
}

void uc_apply(struct uchange *u, int redo) {
  if (redo) {
    switch (u->kind) {
      case UC_INSERT: uc_do_insert(u); break;
      case UC_DELETE: uc_do_delete(u); break;
      case UC_JOIN: uc_do_join(u); break;
      case UC_SPLIT: uc_do_split(u); break;
      case UC_NEWROW: uc_do_newrow(u); break;
      case UC_EOL: row_at(u->row)->crlf = u->crlf; break;
    }
    CUR.cx = u->cx1;
    CUR.cy = u->cy1;
  } else {
    switch (u->kind) {
      case UC_INSERT: uc_do_delete(u); break;
      case UC_DELETE: uc_do_insert(u); break;
      case UC_JOIN: uc_do_split(u); break;
      case UC_SPLIT: uc_do_join(u); break;
      case UC_NEWROW: uc_do_delrow(u); break;
      case UC_EOL: row_at(u->row)->crlf = (unsigned char)u->at; break;
    }
    CUR.cx = u->cx0;
    CUR.cy = u->cy0;
  }
  editor_recompute_dirty();
}

/* Off the top of a stack, taking its size with it. Moving a record between the
 * stacks added its size to one count and left it in the other, so going back and
 * forth reached the byte cap and evicted history well within it. */
static struct uchange *undo_pop(struct uchange **s, int *n, size_t *bytes) {
  struct uchange *u = s[--*n];
  *bytes -= u->len + sizeof(struct uchange);
  return u;
}

void editor_undo(void) {
  if (B.undo.undo_n == 0) {
    editor_set_status_message("Nothing to undo");
    return;
  }
  struct uchange *u = undo_pop(B.undo.undo, &B.undo.undo_n, &B.undo.undo_bytes);
  int group = u->group;
  uc_apply(u, 0);
  redo_push(u);
  /* Keep popping while the top of the stack belongs to the same group --
   * already in reverse push order, since both stacks are LIFO. */
  while (group != 0 && B.undo.undo_n > 0 && B.undo.undo[B.undo.undo_n - 1]->group == group) {
    u = undo_pop(B.undo.undo, &B.undo.undo_n, &B.undo.undo_bytes);
    uc_apply(u, 0);
    redo_push(u);
  }
  B.undo.locked = 1;
  /* Back to one cursor. A record restores one position, so after undoing an
   * edit that happened at several the extra cursors are pointing at wherever
   * they happened to be rather than at anything the user put them on -- and
   * the next keystroke would land in those places. This is the same answer
   * loading another file gets, and for the same reason. */
  cursors_collapse();
  editor_set_status_message("Undo");
}

void editor_redo(void) {
  if (B.undo.redo_n == 0) {
    editor_set_status_message("Nothing to redo");
    return;
  }
  struct uchange *u = undo_pop(B.undo.redo, &B.undo.redo_n, &B.undo.redo_bytes);
  int group = u->group;
  uc_apply(u, 1);
  undo_push(u);
  while (group != 0 && B.undo.redo_n > 0 && B.undo.redo[B.undo.redo_n - 1]->group == group) {
    u = undo_pop(B.undo.redo, &B.undo.redo_n, &B.undo.redo_bytes);
    uc_apply(u, 1);
    undo_push(u);
  }
  B.undo.locked = 1;
  cursors_collapse();
  editor_set_status_message("Redo");
}
