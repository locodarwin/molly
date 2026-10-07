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
  WINCH_KEY,  /* not a real keystroke -- a SIGWINCH woke the read loop */
  MOUSE_KEY,  /* nor this one -- a mouse report; see input_last_mouse */

  /* Function keys. Recognized but deliberately unbound: the menu bar wants F10
   * and help wants F1, and both are Phase 1 bindings rather than Phase 0 ones. */
  F1_KEY = 1100,
  F2_KEY, F3_KEY, F4_KEY, F5_KEY, F6_KEY,
  F7_KEY, F8_KEY, F9_KEY, F10_KEY, F11_KEY, F12_KEY,

  /* Alt + a printable character, as ALT_KEY_BASE + that character. Alt is half
   * of molly's answer to a full Ctrl keyspace, and the menu bar reserves eight
   * of these for its titles. Also unbound for now. */
  ALT_KEY_BASE = 2000
};

/* A mouse report. Delivered as the key MOUSE_KEY, with the detail fetched through
 * input_last_mouse -- the same out-of-band trick the Shift flag uses, because a
 * mouse event is positional and so cannot be a chord bound to a command. */
enum mouse_type {
  MOUSE_PRESS = 0,
  MOUSE_RELEASE,
  MOUSE_DRAG,
  MOUSE_WHEEL_UP,
  MOUSE_WHEEL_DOWN
};

#define MOUSE_BUTTON_LEFT   0
#define MOUSE_BUTTON_MIDDLE 1
#define MOUSE_BUTTON_RIGHT  2

#define MOUSE_MOD_SHIFT 0x01
#define MOUSE_MOD_ALT   0x02
#define MOUSE_MOD_CTRL  0x04

struct mouse_event {
  int type;     /* enum mouse_type */
  int x, y;     /* zero-based cell, already converted from the terminal's 1-based */
  int button;
  unsigned mods;
};

#define ALT_KEY(c)       (ALT_KEY_BASE + (unsigned char)(c))
#define IS_ALT_KEY(k)    ((k) > ALT_KEY_BASE && (k) < ALT_KEY_BASE + 128)
#define ALT_KEY_CHAR(k)  ((k) - ALT_KEY_BASE)

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
  int menubar;             /* menu bar occupies a row (View > Menu Bar toggles it) */
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

/* --- layers -----------------------------------------------------------------
 *
 * A region of the screen that knows how to draw itself. The stack is drawn bottom
 * to top, so a menu, a dialog or a popup is a layer pushed above the chrome and
 * overlap is only draw order. See layer.c, which also explains how layer differs
 * from view and widget. */
struct layer {
  int x, y, w, h;
  void (*draw)(const struct layer *self);
  /* Optional. Returns non-zero if the event was consumed, which stops it reaching
   * anything underneath -- a click on a dialog must not also move the cursor in the
   * text behind it. */
  int (*mouse)(const struct layer *self, const struct mouse_event *ev);
  void *user;
};

/* --- dialogs and widgets -----------------------------------------------------
 *
 * A dialog is a layer with a frame, a title and some widgets. Widgets are declared
 * by the dialog's owner and positioned relative to the content area inside the
 * frame; the owner also keeps the key loop, so a dialog stays a straight-line
 * function rather than a callback. See widget.c. */

enum widget_kind {
  W_LABEL = 0,
  W_LIST,
  W_TEXT,  /* a list with nothing selected: scrolled, not navigated */
  W_INPUT  /* an editable line: what editor_prompt was, without its loop */
};

/* A list asks its owner for a row's text rather than owning the data, so nothing
 * has to be copied into the widget to be shown. */
typedef void (*widget_row_fn)(void *user, int i, char *dst, size_t dsz);

/* The role a particular row takes, for a list whose rows are not all alike: the
 * command palette dims the commands that are unavailable, exactly as a menu greys
 * them out. NULL means every row takes the widget's own role. */
typedef unsigned char (*widget_role_fn)(void *user, int i);

struct widget {
  int kind;
  int x, y, w, h;       /* relative to the dialog's content area */
  unsigned char role;   /* its background and text; ROLE_DIALOG inside a dialog */
  const char *text;     /* label */
  int count;            /* list: rows available */
  int sel, top;         /* list: selection, and the first row shown */
  widget_row_fn row;
  widget_role_fn row_role;
  void *user;

  /* W_INPUT only. The widget owns the buffer and grows it, so a long paste into a
   * prompt is not capped the way a fixed field would cap it. `icur` is a byte
   * offset into it and `iscroll` the first display column shown, which is what
   * lets a line longer than the field be typed and edited. */
  char *ibuf;
  int ilen, icap;
  int icur;
  int iscroll;
};

struct dialog {
  const char *title;
  int cols, rows;       /* content size; the frame adds one on each side */
  struct widget *widgets;
  int nwidgets;
};

/* --- the event loop --------------------------------------------------------
 *
 * What loop_wait returns. The keyboard is reported; every other source is
 * dispatched through its handler before loop_wait returns. */
#define LOOP_STDIN       0x01  /* keyboard input is waiting */
#define LOOP_INTERRUPTED 0x02  /* a signal cut the wait short (a resize, say) */

typedef void (*loop_handler)(void *user);

/* How long a wait sits before looking around again. A resize interrupts it, so
 * this is not how quickly one is noticed; it is the ceiling on how long molly can
 * be unaware that anything has happened. */
#define LOOP_TICK_MS 100

/* --- commands ---------------------------------------------------------------
 *
 * Every action the editor can perform is one of these, reached by name. The
 * dispatcher looks a key up in the keymap and runs what it finds; it never names
 * an action itself, which is what lets the menu bar, a command palette, a macro
 * recorder and a configurable keymap all be built on the same table. */

/* Flags on a command. */
#define CMD_MOVEMENT 0x01  /* a shifted press of this extends the selection */
#define CMD_HIDDEN   0x02  /* not offered in the command palette: mechanism, not an
                            * action -- the seven menu openers and the palette
                            * itself, which has no business listing itself */

/* What a command tells the dispatcher to do afterwards. */
#define CMD_KEEP_SELECTION  0x01  /* do not clear the selection (copy) */
#define CMD_KEEP_QUIT_COUNT 0x02  /* do not reset Ctrl-Q's countdown */

struct command {
  const char *name;   /* "file.save": stable, for config, macros and the palette */
  const char *label;  /* menu text; NULL for commands with no menu entry */
  const char *help;   /* help-screen description; NULL = not listed there */
  const char *group;  /* help-screen group heading */
  unsigned flags;     /* CMD_MOVEMENT */
  int (*run)(void);   /* returns CMD_KEEP_* flags */
  /* Optional. NULL means always available and never ticked. A menu greys an item
   * its command says is unavailable, and ticks one its command says is on; a
   * command palette will want both for the same reason. */
  int (*enabled)(void);
  int (*checked)(void);
};

/* One line of the help screen. A literal key covers several keys at once, or
 * behavior that is not a command; otherwise both columns come from the command. */
struct help_entry {
  const char *group;  /* non-NULL starts a new group */
  const char *key;
  const char *desc;
  const char *cmd;
};

/* --- the palette -------------------------------------------------------------
 *
 * A cell records what it is *for*, not what color it is. The theme turns a role
 * into a style; drawing code never names a color. See theme.c.
 *
 * Only roles something actually draws are defined: menus, dialogs and syntax
 * highlighting add theirs when they arrive, because a role nothing uses is a
 * theme entry nothing tests. */
enum cell_role {
  ROLE_TEXT = 0,   /* buffer text, and anything else unremarkable */
  ROLE_GUTTER,     /* line numbers and the separator bar */
  ROLE_SELECTION,  /* selected buffer text */
  ROLE_STATUS,     /* the status bar */
  ROLE_MESSAGE,    /* the message bar */

  /* Dialogs paint their own background rather than inheriting the terminal's, so
   * they read as panels floating over the text and look the same wherever molly
   * runs. Turbo Vision's chrome, in colors from this century. */
  ROLE_DIALOG,         /* a dialog's body */
  ROLE_DIALOG_FRAME,   /* its double-line border */
  ROLE_DIALOG_TITLE,   /* the title inset in the top border */
  ROLE_DIALOG_SHADOW,  /* the text it overshadows, dimmed */
  ROLE_LIST_SELECTED,  /* the current row of a list */
  ROLE_INPUT,          /* an editable field, sunk a shade below the dialog */

  /* The menu bar is a shade darker than a dialog, so it reads as chrome fixed to
   * the top rather than as a panel that happens to be parked there. */
  ROLE_MENUBAR,          /* the band */
  ROLE_MENUBAR_SELECTED, /* the title whose menu is open */
  ROLE_MENU,             /* a drop-down's body */
  ROLE_MENU_FRAME,       /* its single-line border */
  ROLE_MENU_SELECTED,    /* the highlighted item */
  ROLE_MENU_DISABLED,    /* an item its command says is unavailable */
  ROLE_COUNT
};

/* Modifiers a cell can carry on top of its role.
 *
 * Separate from the role on purpose. A menu item's mnemonic is the item's own
 * colors plus an underline; if the modifier were part of the role, every container
 * would need a second role for its mnemonic, and a third for its mnemonic while
 * selected. Bold is what a dialog title wants, and italic will be what a comment
 * wants once there is syntax highlighting. */
#define ATTR_BOLD      0x01
#define ATTR_UNDERLINE 0x02

/* A resolved style: palette indices, or -1 for the terminal's own default, which
 * is not the same as black -- a light-background user wants their background. */
struct style {
  short fg;
  short bg;
  unsigned char reverse;
  unsigned char bold;
  unsigned char underline;
};

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

/* cmd.c */
const struct command *cmd_at(int i);
const struct command *cmd_find_by_name(const char *name);
const struct command *keymap_lookup(int key);
int chord_parse(const char *s);
int cmd_count(void);
int cmd_shortcut(const char *name, char *dst, size_t n);
int help_entry_count(void);
void chord_name(int key, char *dst, size_t n);
void editor_process_key(int c);
void help_entry_at(int i, const char **group, char *key, size_t keysz, const char **desc);
void keymap_dump(FILE *out);
void keymap_init(void);

/* edit.c */
void editor_del_char(void);
void editor_delete_char(void);
void editor_insert_char(int c);
void editor_insert_newline(void);

/* grid.c */
int grid_cols(void);
int grid_cursor_claimed(void);
int grid_put_char(int y, int x, const char *s, int nbytes, int w);
int grid_puts(int y, int x, const char *s, int len);
int grid_rows(void);
void grid_attrs(unsigned char attrs);
void grid_begin(void);
void grid_cleanup(void);
void grid_cursor_claim(int y, int x);
void grid_cursor_hide(void);
void grid_cursor_to(int y, int x);
void grid_fill(int y, int x, int n, char c);
void grid_flush(void);
void grid_invalidate(void);
void grid_resize(int rows, int cols);
void grid_role(unsigned char role);
void grid_shade(int x, int y, int w, int h, unsigned char role);

/* input.c */
int input_last_key_had_shift(void);
int ui_wait_key(void);
void input_last_mouse(struct mouse_event *out);

/* layer.c */
int layer_at(int x, int y);
int layer_count(void);
int layer_dispatch_mouse(const struct mouse_event *ev);
int layer_push(const struct layer *l);
struct layer *layer_get(int i);
void layer_draw_all(void);
void layer_pop(void);
void layer_pop_to(int count);
void layer_set_bounds(int i, int x, int y, int w, int h);

/* loop.c */
int loop_add(int fd, loop_handler on_ready, void *user);
int loop_source_count(void);
int loop_wait(int timeout_ms);
void loop_cleanup(void);
void loop_remove(int id);

/* main.c */
void editor_cleanup(void);
void init_editor(void);

/* menu.c */
int menu_bar_mouse(const struct layer *self, const struct mouse_event *ev);
int menu_for_mnemonic(int ch);
int menu_is_open(void);
int menu_open(int index);
void menu_bar_draw(const struct layer *self);

/* palette.c */
int palette_open(void);
int palette_score(const char *s, const char *pat);

/* screen.c */
int screen_poll_size(void);
void ab_append(struct abuf *ab, const char *s, int len);
void editor_draw_text_row(int y, erow *row, const struct drow *d, int sel0, int sel1);
void editor_refresh_screen(void);
void editor_set_status_message(const char *fmt, ...);
void screen_cleanup(void);
void screen_force_repaint(void);
void screen_init_layers(void);
void screen_init_size(void);
void screen_sync_size(void);

/* search.c */
int search_case_is_on(void);
int search_has_query(void);
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

/* theme.c */
int style_eq(const struct style *a, const struct style *b);
int style_is_default(const struct style *s);
int theme_depth(void);
int theme_detect_depth(const char *term, const char *colorterm);
void style_default(struct style *out);
void style_emit_color(struct abuf *ab, int color, int is_bg);
void theme_init(void);
void theme_style(unsigned char role, struct style *out);

/* ui.c */
char *editor_open_dialog(void);
char *editor_prompt(char *prompt, void (*callback)(char *, int));
void editor_help(void);
void editor_switch_file(char *path);

/* undo.c */
int undo_can_redo(void);
int undo_can_undo(void);
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
int utf8_col_to_byte(const char *s, int col);
int utf8_is_cont(unsigned char c);
int utf8_prev(const char *s, int i);
int utf8_str_cols(const char *s);

/* view.c */
int drow_of_cursor(const struct cursor *c);
int drow_total(void);
int editor_render_byte_to_cx(erow *row, int rbyte);
int editor_row_cx_to_charcol(erow *row, int cx);
int editor_row_cx_to_rbyte(erow *row, int cx);
int editor_row_cx_to_rx(erow *row, int cx);
int editor_row_rx_to_cx(erow *row, int rx);
void cursors_collapse(void);
void cursors_init(void);
void drow_at(int index, struct drow *out);
void editor_move_cursor(int key);
void editor_scroll(void);

/* widget.c */
int widget_text_maxtop(const struct widget *w);
void dialog_draw(const struct layer *self);
void dialog_init(struct dialog *d, const char *title, struct widget *widgets, int nwidgets, int cols, int rows);
void dialog_layout(struct dialog *d, struct layer *l);
void widget_label(struct widget *w, int x, int y, int cols, const char *text);
void widget_list(struct widget *w, int x, int y, int cols, int rows, int count, widget_row_fn row, void *user);
void widget_list_clamp(struct widget *w);
void widget_list_move(struct widget *w, int delta);
void widget_text(struct widget *w, int x, int y, int cols, int rows, int count, widget_row_fn row, void *user);
void widget_text_scroll(struct widget *w, int delta);
void widget_input(struct widget *w, int x, int y, int cols);
void widget_input_free(struct widget *w);
int widget_input_key(struct widget *w, int c);
int widget_input_draw(struct widget *w, int y, int x, int maxcols);
const char *widget_input_text(const struct widget *w);

#endif /* MOLLY_H */
