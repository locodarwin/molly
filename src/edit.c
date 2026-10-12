/* edit.c -- The editing operations -- insert, newline, backspace, delete -- which turn a
   keystroke into row changes plus the undo records that reverse them. */

#include "molly.h"

void editor_insert_char(int c) {
  int pre_cx = CUR.cx, pre_cy = CUR.cy;
  if (CUR.cy == row_count()) {
    editor_insert_row(row_count(), "", 0);
    struct uchange *ur = uc_new(UC_NEWROW, CUR.cy, 0, "", 0);
    ur->cx0 = pre_cx;
    ur->cy0 = pre_cy;
    ur->cx1 = pre_cx;
    ur->cy1 = pre_cy;
    undo_push(ur);
  }
  editor_row_insert_char(row_at(CUR.cy), CUR.cx, c);
  CUR.cx++;

  struct uchange *top = B.undo.undo_n ? B.undo.undo[B.undo.undo_n - 1] : NULL;
  if (!B.undo.locked && top && top->kind == UC_INSERT && top->row == CUR.cy &&
      top->at + (int)top->len == CUR.cx - 1) {
    top->text = xrealloc(top->text, top->len + 2);
    top->text[top->len++] = c;
    top->text[top->len] = '\0';
    B.undo.undo_bytes++;
    top->cx1 = CUR.cx;
    top->cy1 = CUR.cy;
  } else {
    char ch = c;
    struct uchange *u = uc_new(UC_INSERT, CUR.cy, CUR.cx - 1, &ch, 1);
    u->cx0 = pre_cx;
    u->cy0 = pre_cy;
    u->cx1 = CUR.cx;
    u->cy1 = CUR.cy;
    undo_push(u);
    clear_redo();
  }
  B.undo.locked = 0;
}

void editor_insert_newline(void) {
  if (CUR.cx == 0) {
    struct uchange *u = uc_new(UC_NEWROW, CUR.cy, 0, "", 0);
    u->cx0 = 0;
    u->cy0 = CUR.cy;
    u->cx1 = 0;
    u->cy1 = CUR.cy + 1;
    undo_push(u);
    editor_insert_row(CUR.cy, "", 0);
  } else {
    erow *row = row_at(CUR.cy);
    struct uchange *u =
        uc_new(UC_SPLIT, CUR.cy, CUR.cx, row->chars + CUR.cx, row->size - CUR.cx);
    u->cx0 = CUR.cx;
    u->cy0 = CUR.cy;
    u->cx1 = 0;
    u->cy1 = CUR.cy + 1;
    undo_push(u);
    editor_insert_row(CUR.cy + 1, &row->chars[CUR.cx], row->size - CUR.cx);
    editor_row_truncate(row_at(CUR.cy), CUR.cx);
  }
  CUR.cy++;
  CUR.cx = 0;
  clear_redo();
  B.undo.locked = 0;
}

void editor_delete_char(void) {
  if (CUR.cy > row_count()) return;
  int pre_cx = CUR.cx, pre_cy = CUR.cy;

  /* The cursor rests on the phantom row past the last line (where typing
   * starts a new line): backspace deletes from the end of the real last
   * line instead. The undo record still restores the phantom position. */
  if (CUR.cy == row_count()) {
    if (row_count() == 0) return;
    CUR.cy = row_count() - 1;
    CUR.cx = (*row_at(CUR.cy)).size;
  }
  if (CUR.cx == 0 && CUR.cy == 0) return;

  erow *row = row_at(CUR.cy);
  if (CUR.cx > 0) {
    int at = utf8_prev(row->chars, CUR.cx);
    int nbytes = CUR.cx - at;
    struct uchange *top = B.undo.undo_n ? B.undo.undo[B.undo.undo_n - 1] : NULL;
    /* A run of single-byte backspaces coalesces into one undo step. A
     * multibyte character is deleted as its own step -- simpler, and rare
     * enough that grouping it buys little. */
    if (nbytes == 1 && !B.undo.locked && top && top->kind == UC_DELETE &&
        top->row == CUR.cy && top->at == CUR.cx && top->back) {
      char *nt = xmalloc(top->len + 2);
      nt[0] = row->chars[at];
      memcpy(nt + 1, top->text, top->len);
      nt[top->len + 1] = '\0';
      free(top->text);
      top->text = nt;
      top->len++;
      B.undo.undo_bytes++;
      top->at--;
      top->cx1 = at;
      top->cy1 = CUR.cy;
    } else {
      struct uchange *u = uc_new(UC_DELETE, CUR.cy, at, row->chars + at,
                                 (size_t)nbytes);
      u->back = 1;
      u->cx0 = pre_cx;
      u->cy0 = pre_cy;
      u->cx1 = at;
      u->cy1 = CUR.cy;
      undo_push(u);
      clear_redo();
    }
    editor_row_delete_n(row, at, nbytes);
    CUR.cx = at;
  } else {
    erow *prev = row_at(CUR.cy - 1);
    struct uchange *u =
        uc_new(UC_JOIN, CUR.cy - 1, prev->size, row->chars, row->size);
    u->cx0 = pre_cx;
    u->cy0 = pre_cy;
    u->cx1 = prev->size;
    u->cy1 = CUR.cy - 1;
    undo_push(u);
    clear_redo();
    CUR.cx = prev->size;
    editor_row_append_string(prev, row->chars, row->size);
    editor_delete_row(CUR.cy);
    CUR.cy--;
  }
  B.undo.locked = 0;
}

void editor_del_char(void) {
  if (CUR.cy >= row_count()) return;
  int pre_cx = CUR.cx, pre_cy = CUR.cy;
  erow *row = row_at(CUR.cy);
  if (CUR.cx < row->size) {
    int nbytes = utf8_char_bytes(row->chars, row->size, CUR.cx);
    struct uchange *top = B.undo.undo_n ? B.undo.undo[B.undo.undo_n - 1] : NULL;
    if (nbytes == 1 && !B.undo.locked && top && top->kind == UC_DELETE &&
        top->row == CUR.cy && top->at == CUR.cx && !top->back) {
      size_t at = CUR.cx;
      size_t l = top->len;
      top->text = xrealloc(top->text, l + 2);
      top->text[l] = row->chars[at];
      top->text[l + 1] = '\0';
      top->len++;
      B.undo.undo_bytes++;
      top->cx1 = CUR.cx;
      top->cy1 = CUR.cy;
    } else {
      struct uchange *u = uc_new(UC_DELETE, CUR.cy, CUR.cx, row->chars + CUR.cx,
                                 (size_t)nbytes);
      u->back = 0;
      u->cx0 = pre_cx;
      u->cy0 = pre_cy;
      u->cx1 = CUR.cx;
      u->cy1 = CUR.cy;
      undo_push(u);
      clear_redo();
    }
    editor_row_delete_n(row, CUR.cx, nbytes);
  } else if (CUR.cy < row_count() - 1) {
    struct uchange *u =
        uc_new(UC_JOIN, CUR.cy, row->size, (*row_at(CUR.cy + 1)).chars,
            (*row_at(CUR.cy + 1)).size);
    u->cx0 = pre_cx;
    u->cy0 = pre_cy;
    u->cx1 = row->size;
    u->cy1 = CUR.cy;
    undo_push(u);
    clear_redo();
    editor_row_append_string(row, (*row_at(CUR.cy + 1)).chars, (*row_at(CUR.cy + 1)).size);
    editor_delete_row(CUR.cy + 1);
  }
  B.undo.locked = 0;
}
