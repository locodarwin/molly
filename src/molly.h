/* molly.h -- shared types and the interface each module presents to the others.
 *
 * Molly was one file when it was olly. It is several now because a menu bar, a
 * widget toolkit, syntax highlighting, LSP and an integrated terminal do not fit
 * in one, and because the compiler is more use as a reviewer when the seams are
 * explicit. The split follows the comment banners olly already had, so most of
 * this was a cut rather than a redesign.
 *
 * Three structs carry the editor's state and are declared here because nearly
 * every module touches one of them: struct buffer (an open file), struct view (a
 * window onto one) and struct editorConfig (what is shared). See the comment
 * above them for which is which. */

#ifndef MOLLY_H
#define MOLLY_H

#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3
#define MOLLY_VERSION "0.1.0-dev"

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
  BACKSPACE = 127,
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN,
  WINCH_KEY  /* not a real keystroke -- a SIGWINCH woke the read loop */
};

typedef struct erow {
  int size;
  int rsize;
  int rcap;
  char *chars;
  char *render;
} erow;

/* One cursor: a position plus its own selection anchor. Molly keeps a *list* of
 * these rather than a single cursor, even though only one is ever created so
 * far, because multi-cursor editing has to thread through every edit path and
 * the whole undo grouping. Retrofitting a list later would mean touching all of
 * that twice; carrying one from the start costs an indirection and nothing
 * else. `cx` is a byte offset into the row's chars, `rx` the display column it
 * renders to, and the anchor is meaningful only while sel_active. */
struct cursor {
  int cx, cy, rx;
  int sel_active;   /* a selection is in progress (anchor + this cursor) */
  int sx, sy;       /* selection anchor; this cursor is the far end */
};


#define UC_INSERT 0
#define UC_DELETE 1
#define UC_JOIN 2
#define UC_SPLIT 3
#define UC_NEWROW 4

struct uchange {
  int kind;
  int row;
  int at;
  int back;
  char *text;
  size_t len;
  int cx0, cy0, cx1, cy1;
  int group;  /* 0 = standalone; otherwise shared id of a multi-edit group */
};

struct undoState {
  struct uchange **undo;
  int undo_n, undo_cap;
  size_t undo_bytes;    /* retained payload, drives the cap */
  struct uchange **redo;
  int redo_n, redo_cap;
  size_t redo_bytes;
  int locked;
  int active_group;  /* group id new uchanges are stamped with, 0 = none */
  int group_seq;      /* last group id issued */
};

/* --- the three lifetimes -----------------------------------------------------
 *
 * Olly kept all of this in one struct, which is right for one file in one window
 * and wrong for anything more: tabs, splits and multi-cursor all need to know
 * which of these a given field belongs to. Splitting them while exactly one of
 * each exists is free; splitting them afterwards would mean auditing every
 * reference in the editor.
 *
 * A buffer is an open file: its text, whether it is modified, and the undo
 * history that goes with it. A view is a window onto a buffer: where the cursors
 * are and what part of the buffer is on screen -- two views can show one buffer
 * at different scroll positions, so none of this can live with the text. The app
 * holds what is shared by everything, like the clipboard. */

/* One open file. */
struct buffer {
  erow *row;
  int numrows;
  int rowcap;           /* allocated capacity of row[], grown by doubling */
  char *filename;
  int dirty;
  int eol_crlf;         /* write \r\n line endings, as the loaded file used */
  int final_newline;    /* the file ended with a newline (so the save should) */
  struct undoState undo;

  /* The last-saved contents, so "modified" means really different rather than
   * merely edited, and the mtime seen then, so an overwrite can be questioned. */
  erow *saved_rows;
  int saved_numrows;
  struct timespec on_disk_mtime;
  int have_disk_mtime;

  char recovery_path[4096];
};

/* One window onto a buffer. */
struct view {
  struct buffer *buf;      /* the buffer shown here */
  struct cursor *cursors;  /* never NULL once init_editor has run */
  int ncursors;            /* >= 1 */
  int cursorcap;
  int primary;             /* index into cursors[]; the one that scrolls */
  int drowoff;             /* first *display* row on screen (see struct drow) */
  int coloff;
  int screenrows;
  int screencols;
  int linenum;             /* line-number gutter shown (Ctrl-U toggles it) */
  int gutter_w;            /* gutter width in columns for this frame, 0 when off */
};

/* Shared by every buffer and view. */
struct editorConfig {
  char *clipboard;    /* in-editor clipboard for Ctrl-C/X/V; kept across files */
  int clipsize;       /* bytes held in clipboard (not necessarily NUL-terminated) */
  char statusmsg[128];
  time_t statusmsg_time;
  struct termios orig_termios;
};


/* The editor's state. One buffer and one view for now; the types are shaped so
 * that becomes a list without the rest of molly noticing. */
extern struct editorConfig E;
extern struct buffer B;
extern struct view V;

/* The cursor that scrolls the view and owns the status-bar position. Every
 * single-cursor code path uses this; a path that must act on all of them loops
 * over V.cursors[0 .. V.ncursors) instead. */
#define CUR (V.cursors[V.primary])
/* Rows, reached through the view's buffer rather than the global.
 *
 * Two reasons, both about later work. Editor code that asks the *view* for its
 * rows keeps working when there is more than one buffer, whereas code that names
 * B does not. And the planned big-file rewrite replaces the flat erow array with
 * a piece table: the fewer places that index B.row directly, the smaller that
 * change is. Functions that own the array -- insert, delete, load, save -- still
 * use B directly, because they are what gets replaced.
 *
 * erow itself is still visible to the renderer and to search, so this is not yet
 * full isolation; closing that gap is part of the buffer rewrite, not of Phase 0. */
static inline int row_count(void) { return V.buf->numrows; }
static inline erow *row_at(int i) { return &V.buf->row[i]; }

/* Width available for text once the line-number gutter takes its columns. The
 * horizontal scroll, the row draw budget and the cursor column are all measured
 * against this, not the raw screen width, so the text area simply becomes a
 * narrower window while the gutter is on and is byte-identical while it is off
 * (gutter_w is 0). */
/* --- display rows: the view addresses screen lines, not file lines ---------
 *
 * A "display row" is one line of the text area. With soft wrap off there is
 * exactly one per file row, spanning the whole row, and every function below is
 * the identity mapping -- which is the point: the view, the scroll offset, the
 * draw loop and vertical movement all speak display rows *now*, so turning wrap
 * on later means changing these three functions rather than every caller.
 *
 * Wrap will make a file row produce ceil(width / text_cols) display rows, each
 * covering a range of render columns. That is why a slice is expressed in render
 * columns and not byte offsets: wrapping happens where the glyphs land on screen,
 * not where the bytes fall. */
struct drow {
  int filerow;  /* file row this slice belongs to; may be >= numrows past EOF */
  int rcol0;    /* first render column of that row shown on this line */
  int rcols;    /* render columns shown; DROW_ALL means to the end of the row */
  int frag;     /* 0 for the first slice of a file row, 1.. for continuations */
};

#define DROW_ALL INT_MAX

#define TEXT_COLS() (V.screencols - V.gutter_w)

struct abuf {
  char *b;
  int len;
  int cap;
};

#define ABUF_INIT {NULL, 0, 0}

/* --- what each module offers the others ---------------------------- */

/* buffer.c */
int editor_fill_rows(FILE *fp);
int editor_matches_saved(void);
void build_recovery_path(void);
void editor_check_recovery(void);
void editor_delete_row(int at);
void editor_free_row(erow *row);
void editor_insert_row(int at, char *s, size_t len);
void editor_open(char *filename);
void editor_recompute_dirty(void);
void editor_row_append_string(erow *row, char *s, size_t len);
void editor_row_delete_char(erow *row, int at);
void editor_row_delete_n(erow *row, int at, int n);
void editor_row_insert_char(erow *row, int at, int c);
void editor_row_insert_n(erow *row, int at, const char *s, size_t n);
void editor_save(void);
void editor_update_row(erow *row);
void saved_snapshot_free(void);
void saved_snapshot_take(void);
void write_recovery_file(void);

/* edit.c */
void editor_del_char(void);
void editor_delete_char(void);
void editor_insert_char(int c);
void editor_insert_newline(void);

/* input.c */
int editor_read_key(void);
int input_last_key_had_shift(void);

/* main.c */
void editor_cleanup(void);
void editor_process_keypress(void);
void init_editor(void);

/* screen.c */
int screen_poll_size(void);
void ab_append(struct abuf *ab, const char *s, int len);
void cache_ensure(int y);
void cache_invalidate(void);
void cache_line_draw(struct abuf *ab, int y, const char *s, int len, int force);
void editor_draw_message_bar(struct abuf *ab);
void editor_draw_rows(struct abuf *ab);
void editor_draw_status_bar(struct abuf *ab);
void editor_draw_text_row(struct abuf *ab, int y, erow *row, const struct drow *d, int sel0, int sel1);
void editor_refresh_screen(void);
void editor_set_status_message(const char *fmt, ...);
void screen_cleanup(void);
void screen_force_repaint(void);
void screen_init_size(void);

/* search.c */
void editor_find(void);
void editor_find_next(void);
void editor_find_prev(void);
void editor_goto_line(void);
void editor_replace(void);
void editor_toggle_case_sensitive(void);
void search_cleanup(void);

/* select.c */
char *selection_dup(int *outlen);
int selection_bytes(void);
void clipboard_set(const char *s, int len);
void clipboard_sync_system(void);
void editor_paste(void);
void selection_clear(void);
void selection_delete(void);
void selection_row_rx(int filerow, int *out0, int *out1);

/* sys.c */
char *xstrdup(const char *s);
int write_all(int fd, const void *p0, size_t n);
mode_t default_file_mode(void);
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
void fsync_parent_dir(const char *path);

/* term.c */
_Noreturn void die(const char *s);
int get_cursor_position(int *rows, int *cols);
int get_window_size(int *rows, int *cols);
int read_byte(unsigned char *c);
int term_take_winch(void);
void disable_raw_mode(void);
void enable_raw_mode(void);

/* ui.c */
char *editor_open_dialog(void);
char *editor_prompt(char *prompt, void (*callback)(char *, int));
void editor_help(void);
void editor_switch_file(char *path);

/* undo.c */
struct uchange *uc_new(int kind, int row, int at, const char *text, size_t len);
void clear_redo(void);
void editor_free_undo_redo(void);
void editor_redo(void);
void editor_undo(void);
void redo_push(struct uchange *u);
void uc_apply(struct uchange *u, int redo);
void uc_do_delete(struct uchange *u);
void uc_do_delrow(struct uchange *u);
void uc_do_insert(struct uchange *u);
void uc_do_join(struct uchange *u);
void uc_do_newrow(struct uchange *u);
void uc_do_split(struct uchange *u);
void uc_free(struct uchange *u);
void undo_group_begin(void);
void undo_group_end(void);
void undo_push(struct uchange *u);
void undo_set_max_steps(int n);

/* utf8.c */
int utf8_char_bytes(const char *s, int size, int i);
int utf8_char_cols(const char *s, int size, int i, int *nbytes);
int utf8_is_cont(unsigned char c);
int utf8_prev(const char *s, int i);

/* view.c */
int drow_of_cursor(const struct cursor *c);
int editor_render_byte_to_cx(erow *row, int rbyte);
int editor_row_cx_to_charcol(erow *row, int cx);
int editor_row_cx_to_rbyte(erow *row, int cx);
int editor_row_cx_to_rx(erow *row, int cx);
void cursors_collapse(void);
void cursors_init(void);
void drow_at(int index, struct drow *out);
void editor_move_cursor(int key);
void editor_scroll(void);

#endif /* MOLLY_H */
