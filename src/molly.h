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
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#define KILO_TAB_STOP 8
#define MOLLY_VERSION "1.0.0"

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
  PASTE_KEY,  /* nor this one -- pasted text; see input_paste_text */
  BACKTAB_KEY, /* Shift-Tab, which arrives as its own sequence rather than as
                * Tab with a modifier, and which a focus order needs in order to
                * have a way back round */

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

/* Shift and Ctrl, as bits on a key code, so that "shift-f3" and "ctrl-pagedown"
 * are chords the keymap can bind.
 *
 * Flags rather than blocks of their own codes because both are only
 * *distinguishable* on some keys. A terminal reports a modifier as a parameter
 * inside the CSI forms -- which is the function keys and the navigation keys, so
 * "\x1b[5;5~" is Ctrl-PgUp and "\x1b[1;2C" is Shift-Right. On a letter there is
 * no parameter and nothing to report: Shift-A *is* 'A', and Ctrl-Shift-L is the
 * same single byte as Ctrl-L. So Ctrl on a letter stays what it has always been,
 * a control byte, and chord_parse refuses the combinations that could never fire
 * rather than accepting a binding that silently never happens.
 *
 * Both still arrive out of band from the input layer
 * (input_last_key_had_shift/_ctrl), because a shifted *movement* extends a
 * selection rather than running a different command -- that is true of every
 * movement at once rather than being a binding. The dispatcher composes the two:
 * it tries the key with its modifiers first and the bare key second, which is
 * what keeps Shift-Right extending a selection while Shift-F3 is its own
 * binding. */
#define KEY_ALT          0x2000  /* on a navigation key only; see chord_parse */
#define KEY_SHIFT        0x4000
#define KEY_CTRL         0x8000
#define KEY_MODS         (KEY_ALT | KEY_SHIFT | KEY_CTRL)
#define KEY_BASE(k)      ((k) & ~KEY_MODS)

/* One line of a buffer. There is one of these per line of the file, so its
 * size is most of what a big file costs beyond its own bytes: the fields are in
 * the order that packs them, and the assertion below is there so that adding one
 * is a decision rather than an accident. */
typedef struct erow {
  char *chars;
  int size;

  /* Soft wrap: how many display rows this row occupies, and the text width that
   * was measured at. A row is its own cache because the answer depends on its
   * text, the tab width and nothing else about the file -- so a row keeps it
   * across an insert above that moves it, and a window of another width
   * invalidates it by disagreeing with `wcols`. editor_update_row() clears it,
   * which is every path that changes the row's text, and indent_sync() clears
   * every row's when the tab width moves. See wrap.c. */
  int wcols;
  int wrows;

  /* Syntax highlighting: the lexer state this row is entered with and the one
   * it leaves in, which is all a row keeps -- its colors are worked out from
   * `syn_in` when it is drawn (syntax_roles). The buffer's `syn_valid` says how
   * many rows' worth of these are known to be right. A byte each, because a
   * state is a tag and an index that fit in six bits (see syntax.c). */
  unsigned char syn_in;
  unsigned char syn_out;

  /* The line break after this row is \r\n rather than \n. Per row because a file
   * with mixed endings keeps them, on lines nobody edited. */
  unsigned char crlf;
} erow;

_Static_assert(sizeof(void *) != 8 || sizeof(erow) <= 24,
               "a row is per line of the file: keep it at 24 bytes");

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
  /* The order this one was added in, counting from the view's `cursorseq`. The
   * list itself is kept in document order, so "the one I just added" is not the
   * last entry and cannot be recovered from the positions -- and three commands
   * are about exactly that one: skip this match, take the last one back, and
   * which cursor the next match is searched from. */
  int seq;
};


/* The editing operations a multi-cursor edit can be. Named rather than passed as
 * a function pointer because they do not share a signature -- typing carries a
 * character and the rest carry nothing -- and because cursors.c has to treat
 * the forward delete differently from the others. See src/cursors.c. */
#define MC_INSERT      0
#define MC_INDENT      1
#define MC_NEWLINE     2
#define MC_DELETE_BACK 3
#define MC_DELETE_FWD  4
#define MC_PASTE       5
#define MC_TEXT        6   /* the text cursors_insert_text() was given */
#define MC_EACH        7   /* a text per cursor, from cursors_insert_each() */
#define MC_DEL_WORD_BACK 8 /* to the start of the word before the cursor */
#define MC_DEL_WORD_FWD  9 /* to the end of the word after it */
#define MC_DEL_TO_EOL   10 /* to the end of the row, or the row's end if there */

#define UC_INSERT 0
#define UC_DELETE 1
#define UC_JOIN 2
#define UC_SPLIT 3
#define UC_NEWROW 4
#define UC_EOL 5     /* a row's line break, changed: `at` was, `crlf` is */

struct uchange {
  int kind;
  int row;
  int at;
  int back;
  char *text;
  size_t len;
  int cx0, cy0, cx1, cy1;
  int group;  /* 0 = standalone; otherwise shared id of a multi-edit group */
  unsigned char crlf;  /* UC_JOIN, UC_SPLIT: the line break that goes or comes; UC_EOL: the new one */
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
  int group_depth;   /* nesting depth of undo_group_begin/end */
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

/* One thing a language server said is wrong, in the buffer's own terms: rows
 * and byte offsets, converted from the server's positions when it arrived.
 * The end is exclusive, and may equal the start for a problem with no width. */
struct diag {
  int y0, x0, y1, x1;
  int severity;        /* 1 error, 2 warning, 3 information, 4 hint */
  char *msg;
};

/* One open file. */
struct buffer {
  /* The rows, as a gap array: row[0 .. gap) are rows 0 .. gap, and the rest sit
   * at the top of the array after rowcap - numrows free slots. row_at() and
   * buf_row() are the translation; nothing else should index row[]. */
  erow *row;
  int numrows;
  int rowcap;           /* allocated slots in row[], rows and gap together */
  int gap;              /* the slot the gap starts at, which is a row index */

  /* The block the file was read into, which rows borrow their text from until
   * they are first changed, and which is also the saved snapshot below. See
   * buffer.c. `textlen` covers every byte a row can point at. */
  char *text;
  size_t textlen;
  char *filename;
  int dirty;
  int eol_crlf;         /* a new line break is \r\n, as the file's first line is */
  int final_newline;    /* the file ended with a newline (so the save should) */
  struct undoState undo;

  /* The filetype, and how far up the file the per-row lexer states are known to
   * be right. An edit on row N lowers `syn_valid` to N and stops; the renderer
   * brings rows back up to date just before it draws them. Eager invalidation
   * would relex everything below an edit on every keystroke, for the sake of
   * rows nobody is looking at. */
  const struct syntax *syn;
  int syn_valid;

  /* The tab width this buffer's rows were last measured for wrap at. Per
   * buffer and not one global, because the width can come from the filetype
   * and two windows can be showing two filetypes in the same frame. indent_sync()
   * reconciles it with the width in force. */
  int tabw;

  /* The last-saved contents, so "modified" means really different rather than
   * merely edited, and the mtime seen then, so an overwrite can be questioned.
   * The contents are `text`: line i of them starts at saved_off[i], and
   * saved_off[saved_numrows] is where the last one ends. -1 rows means there is
   * no snapshot and nothing matches. */
  size_t *saved_off;
  unsigned char *saved_crlf;  /* each saved line's break: \r\n or not */
  int saved_numrows;
  struct timespec on_disk_mtime;
  int have_disk_mtime;

  char recovery_path[4096];
  /* Where the recovery file goes when recovery_path is someone else's file:
   * the same name with this user's ID after it. See write_recovery_file(). */
  char recovery_alt[4096 + 16];

  /* Small integer, unique among the buffers open at the time it was made. It is
   * what keeps two unnamed buffers from sharing one recovery file, and it is not
   * a list index: closing a buffer does not renumber the others. */
  int id;

  /* Where the cursor and the scroll were when this buffer was last shown, so
   * that switching away and back returns to where you were rather than to the
   * top of the file.
   *
   * View state kept on the buffer, which is the wrong home in principle and the
   * right one in practice: it is a *remembered* position rather than a live
   * view's, and when splits arrive each view keeps its own live position while
   * this stays as what a view that opens the buffer fresh should start from. */
  int last_cx, last_cy, last_drowoff, last_coloff;

  /* Bumped whenever a row's wrap measurement is thrown away, which is every
   * path that can change how many display rows this buffer has. wrap.c keeps
   * one step of a walk and this is how it knows the walk is still valid. */
  unsigned wrapgen;

  /* Display rows in each block of WRAP_BLOCK rows, -1 for a block not counted
   * since one of its rows changed, so that the walks in wrap.c step over a
   * block at a time. Counted for one width (0 with wrap off), and all stale
   * when `wrapall` has moved on from `wblk_all`: a load, folding, a new tab
   * width. See wrap.c. */
  int *wblk;
  int nwblk;
  int wblk_cols;
  unsigned wblk_all, wrapall;

  /* What the buffer is called where a file name would go, when it has none.
   * Only the find-in-files results set it: a list of matches is not a file, and
   * "[No Name]" on its status bar and tab would say nothing about what it is. */
  char *title;

  /* This is the find-in-files results buffer. See grep.c. */
  int grep_results;

  /* This is the problems list. See lspui.c. */
  int problems;

  /* Not to be edited: set by the user, or because the file cannot be
   * written. Every command that changes text refuses, as on a terminal. */
  int readonly;

  /* What git says: tracked at all, and a mark a row against HEAD (GIT_*),
   * as of `git_edits`. git_view is a buffer of git's own output: 1 a file's
   * history, 2 a commit. See git.c. */
  int git_tracked;
  unsigned git_edits;
  unsigned char *gitmark;
  int ngitmark;
  int git_view;
  char *git_dir;       /* where a git_view's commands are run */

  /* Folded blocks: rows start+1 .. end are hidden behind row start. See
   * fold.c, and wrap.c, which counts a hidden row as no display rows. */
  struct fold { int start, end; } *folds;
  int nfolds;

  /* Bookmarks 0-9: a place each, moved with the text. See nav.c. */
  struct { int y, x; unsigned char set; } marks[10];

  /* Bumped by every change to the text, at the row primitives. What the
   * language server was last sent is `lsp_sent`, so the two differing is what
   * "the server is behind" means -- and nothing that edits has to know there
   * is a server. */
  unsigned edits;

  /* The language server's side of this buffer. See lsp.c. `lsp_srv` is the
   * server's slot plus one, 0 for none, and -1 for a file too big to send;
   * `lsp_name` is the file name the URI was made from, which is how a rename
   * is noticed. */
  int lsp_srv;
  unsigned lsp_sent;
  int lsp_version;
  /* The text the server has, exactly as it was sent, when it takes range
   * edits: what a change is measured against. NULL otherwise. */
  char *lsp_text;
  size_t lsp_text_len;
  char *lsp_uri;
  char *lsp_name;
  struct diag *diag;   /* sorted by start */
  int ndiag;

  /* This buffer is an integrated terminal, and its text is the terminal's
   * screen rather than rows. See tty.c. */
  struct tty *tty;

  /* This buffer is a file too big to read into memory, shown read-only: its
   * rows are read from the file as they are asked for, and `row` is unused.
   * See bigfile.c. */
  struct big *big;
};

/* One window onto a buffer. */
struct view {
  struct buffer *buf;      /* the buffer shown here */
  struct cursor *cursors;  /* never NULL once init_editor has run */
  int ncursors;            /* >= 1 */
  int cursorcap;
  int primary;             /* index into cursors[]; the one that scrolls */
  int cursorseq;           /* last `seq` handed to a cursor; 0 when collapsed */
  int drowoff;             /* first *display* row on screen (see struct drow) */
  int coloff;
  int screenrows;
  int screencols;
  int linenum;             /* line-number gutter shown (Ctrl-U toggles it) */
  int syntax;              /* color the text by filetype */
  int wrap;                /* long lines wrap at the window edge (see wrap.c) */
  int gutter_w;            /* gutter width in columns for this frame, 0 when off */
  int screenx, screeny;    /* top-left cell of this view's text area on the grid */

  /* Where the cursor was when this view last scrolled, so editor_scroll can tell
   * a cursor that has moved from a view that was scrolled with the wheel and
   * should stay put. These were two file-scope statics in view.c -- which is
   * exactly the kind of global that had to become per-view before there could be
   * a second window: scrolling one would have told the other that its own cursor
   * had moved. -1 means "not scrolled yet", so a new view snaps to its cursor. */
  int last_scroll_cx, last_scroll_cy;
};

/* --- windows: the tree the views hang off ------------------------------------
 *
 * A window is either a leaf, which shows one view, or a split of two children.
 * That makes the shape of the screen a binary tree, which is what lets "split
 * this one again" work whichever way the first split went: splitting right and
 * then below is a WIN_COLS node whose second child is a WIN_ROWS node, and no
 * case in the layout has to know how deep it is.
 *
 * A flat list of windows would have been less code and would have capped molly
 * at one row or one column of them forever. The tree costs about forty lines
 * more than the list and nothing else.
 *
 * `frac` rather than a stored column count: a split keeps its *proportion* when
 * the terminal is resized, and nothing has to remember the size it was laid out
 * at last time in order to scale. A new split is WIN_FRAC_HALF; moving its
 * border is what changes it.
 *
 * In 1/65536ths rather than the thousandths it started in. A border moved by
 * hand has to be able to sit on every cell, and a share rounds back to the
 * cell it was taken from only while one unit of it is under half a cell --
 * which thousandths stop being past two thousand columns, where the border
 * then skipped cells it could not express. */
#define WIN_FRAC_ONE  65536
#define WIN_FRAC_HALF (WIN_FRAC_ONE / 2)
enum win_kind {
  WIN_LEAF,   /* shows `view` */
  WIN_COLS,   /* kid[0] left of kid[1], with a separator column between */
  WIN_ROWS    /* kid[0] above kid[1]; kid[0]'s status row is the separator */
};

struct window {
  int kind;
  struct view *view;        /* WIN_LEAF only */
  struct window *kid[2];
  struct window *parent;    /* NULL at the root */
  int frac;                 /* kid[0]'s share of the split axis, of WIN_FRAC_ONE */
  int x, y, w, h;           /* this frame's rect; window_layout() fills it in */
};

/* Shared by every buffer and view. */
struct editorConfig {
  char *clipboard;    /* in-editor clipboard for Ctrl-C/X/V; kept across files */
  int clipsize;       /* bytes held in clipboard (not necessarily NUL-terminated) */
  int clip_block;     /* it came from a block selection: a line per row */
  char statusmsg[128];
  time_t statusmsg_time;
  /* Where the cursor was when the message was set. A diagnostic under the
   * cursor takes the bar from a message once the cursor has moved off where
   * it was said: see draw_message_bar. */
  const struct buffer *statusmsg_buf;
  int statusmsg_cx, statusmsg_cy;
  struct termios orig_termios;

  /* The open buffers. See buflist.c.
   *
   * An array of *pointers* rather than of structs, and that is the one design
   * decision in this list worth arguing: struct view holds a struct buffer *, so
   * an array of structs would invalidate every view's pointer the moment the
   * list grew past its capacity. With pointers a buffer's address is fixed for
   * as long as it is open, closing one is a memmove of pointers rather than of
   * buffers, and a view that is looking at a buffer keeps looking at it however
   * the list is rearranged around it. */
  struct buffer **bufs;
  int nbufs, bufcap;
  int next_buf_id;    /* the id the next buffer made will take */

  /* The window tree and the view that has the keyboard. See window.c.
   *
   * `curview` is never NULL: state.c gives it the address of a view it defines
   * statically, before main runs, so that every null-check written against
   * `V.buf` still means what it says. Splits allocate the views after the first;
   * window.c knows which one it must not free. */
  struct window *winroot;
  struct view *curview;

  /* Chrome that belongs to the screen and not to a window. Both were fields on
   * struct view, which was right while there was one view and wrong the moment
   * there were two: the rows the menu and tab bars take are above every window,
   * so which window has focus cannot be what decides whether they are there. */
  int menubar;        /* the menu bar occupies a row */
  int tabbar;         /* a tab bar is wanted; it only takes a row with 2+ buffers */
};


/* The editor's state. */
extern struct editorConfig E;

/* The current view: whichever window has the keyboard.
 *
 * This was a global struct of its own, and every one of some three hundred
 * `V.something` references assumed there was only ever one window. Making it a
 * macro for the focused view -- exactly what was done to B when buffers became
 * plural -- is what let all of them stay as they were while the number of
 * windows stopped being one. The two definitions stack: B is the current view's
 * buffer, so "the current buffer" follows the focus with no further work. */
#define V (*E.curview)

/* The current buffer: whichever one the current view is looking at.
 *
 * This was a global struct of its own, and nearly two hundred references to
 * `B.something` across the editor assumed there was only one. Making it the
 * view's buffer rather than a buffer of its own is what let those references
 * stay exactly as they were while the number of buffers stopped being one --
 * and it is the correct definition for splits too, where "the current buffer"
 * can only ever mean "the current view's".
 *
 * Code that owns the row array -- insert, delete, load, save -- writes through
 * this rather than taking a buffer argument, and borrows the view to act on a
 * buffer that is not current (see buf_free). */
#define B (*V.buf)

/* The cursor that scrolls the view and owns the status-bar position. Every
 * single-cursor code path uses this; a path that must act on all of them loops
 * over V.cursors[0 .. V.ncursors) instead. */
#define CUR (V.cursors[V.primary])
/* Rows, reached through the view's buffer.
 *
 * The array is a gap array (see struct buffer and buffer.c), so a row's index
 * and its slot differ past the gap, and these are the only translation. Nothing
 * outside buffer.c indexes `row[]`; buf_row() is for a buffer that is not the
 * current one.
 *
 * A row's `chars` is contiguous and NUL-terminated wherever it lives -- in the
 * block the file was read into, or in an allocation of its own -- so the code
 * that reads rows did not change when the storage did. Code that *writes* a
 * row goes through the editor_row_* functions, which give it text of its own
 * first; writing through `chars` directly would change the saved snapshot. */
erow *big_row(const struct buffer *b, int i);
static inline erow *buf_row(const struct buffer *b, int i) {
  if (b->big != NULL) return big_row(b, i);
  return &b->row[i < b->gap ? i : i + (b->rowcap - b->numrows)];
}
static inline int row_count(void) { return V.buf->numrows; }
/* Whether this window wraps long lines: its setting, which a file too big to
 * read into memory overrules (bigfile.c). */
static inline int view_wraps(void) { return V.wrap && V.buf->big == NULL; }
static inline erow *row_at(int i) { return buf_row(V.buf, i); }

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
 * covering a range of display columns. That is why a slice is expressed in
 * display columns and not byte offsets: wrapping happens where the glyphs land on screen,
 * not where the bytes fall. */
struct drow {
  int filerow;  /* file row this slice belongs to; may be >= numrows past EOF */
  int rcol0;    /* first display column of that row shown on this line */
  int rcols;    /* display columns shown; DROW_ALL means to the end of the row */
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
  W_INPUT, /* an editable line: what editor_prompt was, without its loop */
  W_CHECK, /* a setting: [ ] off, [tick] on, Space toggles it */
  W_BUTTON /* [ Find ]: Enter or Space presses it, so does a click */
};

/* A list asks its owner for a row's text rather than owning the data, so nothing
 * has to be copied into the widget to be shown. */
typedef void (*widget_row_fn)(void *user, int i, char *dst, size_t dsz);

/* The role a particular row takes, for a list whose rows are not all alike: the
 * command palette dims the commands that are unavailable, exactly as a menu grays
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

  unsigned char on;    /* W_CHECK: ticked */
  unsigned char dflt;  /* W_BUTTON: the default one, which Enter presses from
                        * anywhere in the dialog */
};

struct dialog {
  const char *title;
  int cols, rows;       /* content size; the frame adds one on each side */
  struct widget *widgets;
  int nwidgets;

  /* Which widget has the keyboard, and which button was last pressed. A dialog
   * with one thing on it needs neither, which is why neither existed until a
   * dialog had five. dialog_init zeroes them, so a caller that wants the focus
   * somewhere other than the first widget sets it afterwards. */
  int focus;
  int pressed;
};

/* What dialog_key and dialog_mouse made of a key or a click. */
#define DIALOG_IGNORED 0  /* the caller's own business */
#define DIALOG_HANDLED 1  /* consumed: redraw and wait again */
#define DIALOG_CANCEL  2  /* Esc, or Ctrl-C */
#define DIALOG_PRESSED 3  /* a button went in; d->pressed says which */

/* --- the shell filter -------------------------------------------------------
 *
 * Text in, a command, text out. The one place molly runs something on the
 * user's behalf, which is why the limits are arguments rather than policy
 * buried inside: a caller says how long it will wait and how much output it
 * will take, and a test says milliseconds and kilobytes where the editor says
 * seconds and megabytes.
 *
 * Deliberately knows nothing about buffers, selections, the keymap or the
 * prompt. It takes a string and returns a string, which is also what makes it
 * testable without a terminal -- the editing half is in cmd.c. */
#define FILTER_OK        0  /* ran and exited 0; `out` is its output */
#define FILTER_FAILED    1  /* ran and exited non-zero, or died on a signal */
#define FILTER_TIMEOUT   2  /* still going after timeout_ms */
#define FILTER_CANCELED 3  /* the cancel callback asked for it to stop */
#define FILTER_TOOBIG    4  /* produced more than max_out bytes */
#define FILTER_NOSTART   5  /* a pipe or fork failed; nothing ran */

struct filter_io {
  /* In. */
  const char *cmd;    /* passed to `sh -c`, so a pipeline is one command */
  const char *in;     /* text for the command's standard input */
  int inlen;
  int timeout_ms;     /* 0 for no limit */
  int max_out;        /* bytes of output accepted; 0 for no limit */

  /* Out. */
  char *out;          /* the command's standard output; NUL-terminated */
  int outlen;
  char err[256];      /* the first line of its standard error, or why it did not run */
  int status;         /* its exit status, when it ran to completion */
  int signaled;      /* the signal that killed it, or 0 */
};

int filter_run(struct filter_io *io, int (*canceled)(void));
void filter_free(struct filter_io *io);

/* --- JSON -------------------------------------------------------------------
 *
 * What the language server speaks. A parsed message is a tree of these, owned by
 * the json_doc it came out of: nothing in it is freed on its own, and nothing in
 * it outlives json_free(). See json.c. */
enum json_type {
  JSON_NULL = 0,
  JSON_FALSE,
  JSON_TRUE,
  JSON_NUMBER,
  JSON_STRING,
  JSON_ARRAY,
  JSON_OBJECT
};

struct json {
  int type;
  int n;               /* children of an array or object; bytes of a string */
  double num;
  char *str;           /* decoded and NUL-terminated (it may also hold NULs) */
  const char *key;     /* this value's name, when it is an object member */
  struct json *kid;    /* first child */
  struct json *next;   /* next sibling */
};

struct json_doc;

/* What a language server can be asked. LSP_REQ_INIT and LSP_REQ_SHUTDOWN are
 * the client's own; the rest are questions about a position in a buffer. */
#define LSP_REQ_INIT       0
#define LSP_REQ_DEFINITION 1
#define LSP_REQ_HOVER      2
#define LSP_REQ_COMPLETION 3
#define LSP_REQ_SHUTDOWN   4
#define LSP_REQ_RENAME     5
#define LSP_REQ_REFERENCES 6
#define LSP_REQ_SIGNATURE  7
#define LSP_REQ_FORMAT     8
#define LSP_REQ_ACTIONS    9    /* the code actions at a place */
#define LSP_REQ_RESOLVE    10   /* one of them, filled in */
#define LSP_REQ_APPLY      11   /* not asked: the server's own workspace/applyEdit */
#define LSP_REQ_EXECUTE    12   /* a command run; its edits come as an APPLY */

/* A piece of a row that a diagnostic covers: bytes [x0, x1). See lspui.c. */
struct diag_span {
  int x0, x1;
  int severity;
};

/* --- the integrated terminal's screen -----------------------------------------
 *
 * One character cell as a program in the terminal left it. See vt.c. The
 * attribute bits are the grid's own, so drawing one is a copy and not a
 * translation; DIM and INVISIBLE have no grid equivalent and are read by the
 * drawing code. `w` is 1, 2 for a wide character, or 0 for the column a wide
 * character's right half occupies. */
#define VT_BOLD      ATTR_BOLD
#define VT_UNDERLINE ATTR_UNDERLINE
#define VT_ITALIC    ATTR_ITALIC
#define VT_REVERSE   ATTR_REVERSE
#define VT_DIM       0x20
#define VT_INVISIBLE 0x40

/* A color that is red, green and blue rather than a palette index: the bit
 * above the 24 they take. */
#define COLOR_RGB 0x1000000

struct vt_cell {
  uint32_t cp;
  uint32_t comb;        /* one combining mark, or 0 */
  int fg, bg;           /* palette indices or COLOR_RGB | rgb; -1 the default */
  unsigned char w;
  unsigned char attrs;
};

#define VT_MODE_APP_CURSOR      1
#define VT_MODE_BRACKETED_PASTE 2
#define VT_MODE_ALT_SCREEN      3
#define VT_MODE_MOUSE           4   /* 0, or 1000, 1002 or 1003: what it reports */
#define VT_MODE_MOUSE_SGR       5

struct vt;
struct tty;

/* --- the event loop --------------------------------------------------------
 *
 * What loop_wait returns. The keyboard is reported; every other source is
 * dispatched through its handler before loop_wait returns. */
#define LOOP_STDIN       0x01  /* keyboard input is waiting */
#define LOOP_INTERRUPTED 0x02  /* a signal cut the wait short (a resize, say) */

/* What a registered source is waiting for. A pipe molly is *feeding* is as much
 * a source as one it is reading -- the shell filter has one of each, and without
 * both it would have to write all of its input before reading any output, which
 * deadlocks the moment the command's output outgrows a pipe buffer. */
#define LOOP_READ  0x01
#define LOOP_WRITE 0x02

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
#define CMD_EDITS    0x04  /* changes the text: refused on a terminal's buffer,
                            * which has none -- see cmd_run() */
#define CMD_HIDDEN   0x02  /* not offered in the command palette: mechanism, not an
                            * action -- the seven menu openers and the palette
                            * itself, which has no business listing itself */
#define CMD_LOADED   0x08  /* needs the whole file in memory: refused in a file
                            * too big to read in -- see bigfile.c */

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
  /* Optional. NULL means always available and never ticked. A menu grays an item
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
  /* A cursor other than the primary one. The terminal has exactly one real
   * cursor and the primary keeps it, so every other cursor has to be a cell
   * that looks like one. Its own role rather than ROLE_SELECTION because a
   * cursor is one cell and a selection is a run, and Ctrl-D puts one of each
   * next to the other -- reusing the selection's reverse video would have made
   * the two indistinguishable exactly where telling them apart matters. */
  ROLE_CURSOR,
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
  ROLE_BUTTON,         /* a button, raised a shade above it */
  ROLE_FOCUS,          /* whichever control has the keyboard */

  /* The menu bar is a shade darker than a dialog, so it reads as chrome fixed to
   * the top rather than as a panel that happens to be parked there. */
  ROLE_MENUBAR,          /* the band */
  ROLE_MENUBAR_SELECTED, /* the title whose menu is open */
  ROLE_MENU,             /* a drop-down's body */
  ROLE_MENU_FRAME,       /* its single-line border */
  ROLE_MENU_SELECTED,    /* the highlighted item */
  ROLE_MENU_DISABLED,    /* an item its command says is unavailable */

  /* The tab bar, which reads as chrome of the same family as the menu bar: the
   * band is the menu bar's shade, and the current tab is lifted out of it the
   * way a selected menu title is. An *inactive* tab has no role of its own -- it
   * is the band, because a third shade between the band and the current tab buys
   * nothing and a role nothing distinguishes is a theme entry nothing tests. */
  ROLE_TABBAR,           /* the band, and the tabs that are not current */
  ROLE_TAB_SELECTED,     /* the buffer being edited */

  /* Splits. The status bar of the window with the keyboard keeps ROLE_STATUS,
   * so one window is drawn exactly as it was before splits existed; a window
   * without it is told apart by a quieter bar rather than by a marker in the
   * text, because the bar is already the thing the eye goes to for "which file
   * is this" and it is one row either way. */
  ROLE_STATUS_UNFOCUSED, /* the status bar of a window that has not got focus */
  ROLE_SPLIT,            /* the column between two windows side by side */

  /* Syntax highlighting. Eight roles for seven languages, which is the set that
   * falls out of what those languages actually distinguish rather than a palette
   * chosen first: two keyword classes (control words and type names), strings,
   * numbers, comments, preprocessor lines, and the two a prose format needs --
   * Markdown has no keywords but it does have headings and emphasis. */
  ROLE_SYN_KEYWORD,
  ROLE_SYN_TYPE,
  ROLE_SYN_STRING,
  ROLE_SYN_NUMBER,
  ROLE_SYN_COMMENT,
  ROLE_SYN_PREPROC,
  ROLE_SYN_HEADING,
  ROLE_SYN_EMPHASIS,

  /* What a language server says is wrong, underlined in the text. Two roles
   * and not four: an error and a warning each have a color, and information
   * and hints are only underlined, in whatever color the text already was --
   * a fourth color in the text for "consider" would shout as loud as "wrong". */
  ROLE_DIAG_ERROR,
  ROLE_DIAG_WARNING,

  /* The view's aids. Three are tints -- only their background is used, laid
   * under whatever the cell's own role says (see grid_tint) -- and one is the
   * dim ink that whitespace markers and indent guides are drawn in. */
  ROLE_CURRENT_LINE,
  ROLE_OCCURRENCE,
  ROLE_RULER,
  ROLE_WHITESPACE,

  /* The sign column's marks against the last commit. */
  ROLE_GIT_ADDED,
  ROLE_GIT_CHANGED,
  ROLE_GIT_DELETED,
  ROLE_COUNT
};

/* Modifiers a cell can carry on top of its role.
 *
 * Separate from the role on purpose. A menu item's mnemonic is the item's own
 * colors plus an underline; if the modifier were part of the role, every container
 * would need a second role for its mnemonic, and a third for its mnemonic while
 * selected. Bold is what a dialog title wants. Italic is what a comment wants,
 * and it is a property of the comment's role in the theme rather than one of
 * these, for the same reason bold on a heading is: nothing draws a comment
 * without it. */
#define ATTR_BOLD      0x01
#define ATTR_UNDERLINE 0x02

/* The integrated terminal's cells, which are the one thing molly draws whose
 * colors it does not choose: a program in the terminal asks for red, and red
 * is what it gets. ATTR_RAW says the cell's color is the palette index it
 * carries rather than its role's, and italic and reverse become modifiers for
 * such cells because the program turns them on character by character. See
 * grid_color(). The one other user is the text area, whose control characters
 * are ATTR_REVERSE on top of their role. */
#define ATTR_ITALIC    0x04
#define ATTR_REVERSE   0x08
#define ATTR_RAW       0x10
/* The cell's background comes from another role -- a tint -- whose index is
 * kept where a raw cell keeps its background color. See grid_tint(). */
#define ATTR_TINT      0x20

/* A resolved style: palette indices, or -1 for the terminal's own default, which
 * is not the same as black -- a light-background user wants their background. */
struct style {
  int fg;
  int bg;
  unsigned char reverse;
  unsigned char bold;
  unsigned char underline;
  unsigned char italic;
};

/* --- regular expressions ----------------------------------------------------
 *
 * A PCRE subset, hand-rolled; see regex.c for what is in it and what is not. The
 * compiled form is opaque because nothing outside regex.c has any business
 * looking at it, and matching is deliberately separable from the editor: it takes
 * a pointer and a length, which makes it testable without a terminal and reusable
 * on a row, a selection or a filename. */

struct regex;

/* A syntax definition. Opaque for the same reason struct regex is: nothing
 * outside syntax.c has any business reading one, and keeping it that way is what
 * lets the rule set grow without the rest of molly recompiling against it. */
struct syntax;

#define REGEX_ICASE 0x01  /* fold A-Z against a-z, ASCII only */

#define REGEX_MAX_GROUPS 10  /* the whole match, plus \1 to \9 */

/* What regex_exec returns. TOO_HARD is the backtracker giving up rather than
 * hanging -- see the step budget in regex.c -- and a caller has to tell it apart
 * from "no match" because the honest thing to say is that the pattern is too
 * complex, not that the text does not contain it. */
#define REGEX_NOMATCH   0
#define REGEX_MATCH     1
#define REGEX_TOO_HARD (-1)

/* Byte offsets into the text that was searched: so[0]/eo[0] are the whole match
 * and the rest are the capturing groups, with -1 for a group that did not
 * participate. Half-open, so eo - so is the length. */
struct regex_match {
  int ngroups;
  int so[REGEX_MAX_GROUPS];
  int eo[REGEX_MAX_GROUPS];
};

struct abuf {
  char *b;
  int len;
  int cap;
};

#define ABUF_INIT {NULL, 0, 0}

/* --- what each module offers the others ---------------------------- */

/* buflist.c */
int buf_close(int i);
int buf_count(void);
int buf_current(void);
int buf_dirty_count(void);
int buf_index_of(const struct buffer *b);
const char *buf_name(const struct buffer *b);
int buf_find_path(const char *path);
struct buffer *buf_at(int i);
struct buffer *buf_new(void);
void buf_show(int i);
void buflist_cleanup(void);
void buflist_init(void);

/* buffer.c */
int editor_fill_rows(FILE *fp, int adopt);
int editor_matches_saved(void);
void build_recovery_path(void);
void editor_check_recovery(void);
void editor_delete_row(int at);
void editor_free_row(erow *row);
void editor_clear_rows(void);
void editor_free_rows(void);
void editor_row_truncate(erow *row, int at);
void editor_insert_row(int at, char *s, size_t len);
int file_open_read(const char *path, const char **why);
/* A file read whole, before it has a buffer: see file_read_fd(). */
struct file_text {
  char *text;          /* len bytes and one to spare; the buffer's once opened */
  size_t len;
  struct stat st;
  int have_st;
};
int file_read_fd(int fd, struct file_text *ft);
void editor_open(char *filename, struct file_text *ft);

/* bigfile.c */
struct big;
int big_wanted(long long size);
struct big *big_open(int fd, const char *name, const char **why);
void big_attach(struct big *g, const char *filename);
void big_free(struct big *g);
long long big_line_offset(const struct buffer *b, int y);
int big_refuse(const char *what);
int big_stopped(const char *doing, long long done, long long total);
int big_row_cut(const struct buffer *b, int y);
const char *big_cut_size(void);
const char *editor_readonly_why(void);
void big_tune(int step_lines, int step_bytes, int cut, int pages);
int big_pages_held(const struct buffer *b);
int big_view_over_kb(void);
void big_set_view_over_kb(int kb);
int cmd_available(const struct command *c);
void editor_recompute_dirty(void);
void editor_row_append_string(erow *row, char *s, size_t len);
void editor_row_delete_char(erow *row, int at);
void editor_row_delete_n(erow *row, int at, int n);
void editor_row_insert_char(erow *row, int at, int c);
void editor_row_insert_n(erow *row, int at, const char *s, size_t n);
void editor_save(void);
void editor_remeasure_all(void);
void editor_update_row(erow *row);
int tab_width(void);
int tab_width_default(void);
void tab_width_set(int n);
void saved_snapshot_free(void);
void saved_snapshot_take(void);
void write_recovery_all(void);
void write_recovery_file(void);
int recovery_file_ours(const char *path, struct stat *st);
void recovery_files_remove(const struct buffer *b);

/* cmd.c */
const struct command *cmd_at(int i);
const struct command *cmd_find_by_name(const char *name);
int cmd_run(const struct command *c);
const struct command *keymap_lookup(int key);
int chord_parse(const char *s);
int cmd_count(void);
int cmd_shortcut(const char *name, char *dst, size_t n);
int help_entry_count(void);
int keymap_bind(const char *chord, const char *cmdname, const char **err);
void chord_name(int key, char *dst, size_t n);
void editor_process_key(int c);
void help_entry_at(int i, const char **group, char *key, size_t keysz, const char **desc);
void keymap_cleanup(void);
void keymap_dump(FILE *out);
void keymap_init(void);
void keymap_write_changes(FILE *out);

/* config.c */
const char *config_error_at(int i);
const char *config_path(void);
int config_error_count(void);
int config_load(void);
int config_write(const char *path, const char **err);
int setting_count(void);
int setting_info(int i, const char **section, const char **name, int *is_bool,
                 int *lo, int *hi, const char **comment);
int setting_live(int i);
void setting_set(int i, int v);
int shell_output_max(void);
int shell_timeout_ms(void);
int setting_get(const char *section, const char *name);
void config_apply(void);
void config_apply_keys(void);
void config_apply_syntax(void);
void config_cleanup(void);
void config_defaults(void);
void config_dump(FILE *out);
void config_parse_text(const char *text, const char *origin);

/* indent.c */
int indent_auto(void);
int indent_auto_default(void);
int indent_insert_spaces(void);
int indent_insert_spaces_default(void);
void editor_newline_and_indent(void);
void indent_shift(int dir);
int indent_backspace(void);
int indent_convert(int to_spaces);
void indent_insert_step(void);
void indent_set_auto(int v);
void indent_set_insert_spaces(int v);
void indent_sync(void);

/* edit.c */
void editor_del_char(void);
void editor_delete_char(void);
void editor_delete_back_to(int at);
void editor_set_row_eol(int y, int crlf);
void editor_insert_char(int c);
void editor_insert_newline(void);

/* grid.c */
int grid_cols(void);
int grid_cursor_claimed(void);
int grid_put_char(int y, int x, const char *s, int nbytes, int w);
int grid_puts(int y, int x, const char *s, int len);
int grid_rows(void);
void grid_attrs(unsigned char attrs);
void grid_color(int fg, int bg, unsigned char attrs);
void grid_tint(int role);
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
int input_escape_pressed(void);
int input_last_key_had_alt(void);
int input_last_key_had_ctrl(void);
int input_last_key_had_shift(void);
int ui_wait_key(void);
void input_last_mouse(struct mouse_event *out);
const char *input_paste_text(int *len);
void ui_request_redraw(void);
int macro_play(void);
int macro_playing(void);
int macro_recording(void);
int macro_toggle(void);

/* json.c */
const struct json *json_at(const struct json *arr, int i);
const struct json *json_get(const struct json *obj, const char *key);
const struct json *json_root(const struct json_doc *doc);
const char *json_str(const struct json *v);
int json_bool(const struct json *v, int dflt);
int json_int(const struct json *v, int dflt);
struct json_doc *json_parse(const char *s, size_t len);
void json_free(struct json_doc *doc);
void json_put_str(struct abuf *ab, const char *s, int len);
long json_str_units(const char *s, int len, int utf8);
void json_write(struct abuf *ab, const struct json *v);
void json_putf(struct abuf *ab, const char *fmt, ...);

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
int loop_add(int fd, int events, loop_handler on_ready, void *user);
int loop_source_count(void);
int loop_wait(int timeout_ms);
void loop_cleanup(void);
void loop_remove(int id);

/* lines.c */
void lines_case(int upper);
void lines_comment(void);
void lines_duplicate(void);
void lines_join(void);
void lines_move(int dir);
void lines_sort(void);
void lines_trim(void);

/* smart.c */
int smart_auto_close(void);
int smart_backspace(void);
int smart_home_on(void);
int smart_home_target(const erow *r, int cx);
int smart_insert(int c, int *adjust);
int smart_surround(int c);
void smart_set_auto_close(int on);
void smart_set_home(int on);

/* recent.c */
const char *recent_at(int i);
int recent_count(void);
void editor_recent_dialog(void);
void recent_cleanup(void);
int state_path(char *dst, size_t n, const char *name);
FILE *state_begin(const char *path, char *tmp, size_t n);
void state_commit(FILE *fp, const char *tmp, const char *path);

/* trust.c: the projects a language server may be started in */
int trust_lookup(const char *root);
void trust_set(const char *root, int yes);
void trust_forget_noes(void);
void trust_cleanup(void);
void recent_note(const char *filename);

/* git.c */
#define GIT_ADDED   1
#define GIT_CHANGED 2
#define GIT_DELETED 3
int git_gutter_on(void);
int git_log_enter(void);
int git_mark(const struct buffer *b, int y);
int git_signs(void);
void git_blame_line(void);
void git_idle(void);
void git_log_file(void);
void git_refresh(struct buffer *b);
void git_set_gutter(int on);

/* fold.c */
int fold_at(const struct buffer *b, int y);
int fold_hidden(const struct buffer *b, int y);
int fold_size(const struct buffer *b, int i);
void fold_all(void);
void fold_fix_cursor(int prev_cy);
void fold_none(void);
void fold_rows_moved(struct buffer *b, int at, int delta);
void fold_toggle(void);

/* nav.c */
int nav_back(void);
int nav_can_back(void);
int nav_can_forward(void);
int nav_forward(void);
int nav_match(int cy, int cx, int *my, int *mx);
void nav_mark_go(int n);
void nav_mark_set(int n);
void nav_match_jump(void);
void nav_push(void);
void nav_record(int buf_id, int cy, int cx);
void nav_rows_moved(struct buffer *b, int at, int delta);

/* lsp.c */
char *lsp_path_to_uri(const char *path);
int lsp_ask(struct buffer *b, int kind, int cy, int cx, int trigger,
            const char *arg);
int lsp_can(struct buffer *b, int kind);
int lsp_is_trigger(struct buffer *b, int c);
int lsp_is_sig_trigger(struct buffer *b, int c);
int lsp_can_format_range(struct buffer *b);
int lsp_code_actions(struct buffer *b, int y0, int x0, int y1, int x1, int cy, int cx);
int lsp_code_action_run(struct buffer *b, const char *action, int resolve, int cy, int cx);
int lsp_format(struct buffer *b, int range, int y0, int x0, int y1, int x1,
               int tab_size, int spaces, int cy, int cx);
char *lsp_uri_to_path(const char *uri);
int lsp_col_to_units(const char *s, int size, int col, int utf8);
int lsp_enabled(void);
int lsp_units_to_col(const char *s, int size, int units, int utf8);
void lsp_buffer_closed(struct buffer *b);
void lsp_buffer_saved(struct buffer *b);
void lsp_cleanup(void);
void lsp_describe(struct buffer *b, char *dst, size_t n);
void lsp_diag_clear(struct buffer *b);
void lsp_idle(void);
void lsp_trust_ask(void);
void lsp_rows_moved(struct buffer *b, int at, int delta);
void lsp_set_enabled(int on);
int lsp_setting(void);
void lsp_stop_all(void);

/* snippet.c */
/* A field of a snippet: where its text is in the expanded text, and the
 * placeholder it is inside, as an index into the same list (-1 for none). */
struct snip_stop {
  int num, off, len, parent;
};
struct snippet {
  char *text;
  int len;
  struct snip_stop *stops;     /* in the order they appear */
  int nstops;
};
void snippet_expand(const char *src, const char *indent, const char *tab,
                    struct snippet *out);
void snippet_free(struct snippet *s);
void snippet_begin(struct snippet *sn, int y, int x);
int snippet_key(int c);
void snippet_after_key(void);
int snippet_active(void);
void snippet_end(void);
void snippet_forget(struct buffer *b);

/* lspui.c */
int lspui_back(void);
int lspui_can(int kind);
int lspui_can_back(void);
int lspui_cursor_message(char *dst, size_t n);
int lspui_complete(void);
int lspui_definition(void);
int lspui_references(void);
int lspui_format(void);
int lspui_code_actions(void);
int lspui_format_before_save(void);
int lspui_format_on_save(void);
void lspui_set_format_on_save(int on);
int lspui_complete_typing(void);
void lspui_set_complete_typing(int on);
int lspui_problems(void);
int lspui_problems_jump(void);
int lspui_rename(void);
void lspui_problems_changed(void);
void lspui_problems_refresh(void);
int lspui_signature(void);
int lspui_hover(void);
int lspui_key(int c);
int lspui_place(int w, int h, int *px, int *py);
void lspui_after_key(int c);
void lspui_box(int x, int y, int w, int h);
void lspui_deliver(void);
void lspui_forget(struct buffer *b);
void lsp_answer(int kind, struct buffer *b, unsigned edits, int cy, int cx,
                const struct json *result, int utf8);
int lspui_problem_jump(int dir);
int lspui_row_spans(const struct buffer *b, int filerow, const erow *row,
                    struct diag_span *out, int max);
unsigned char lspui_severity_role(int sev);
void lspui_counts(const struct buffer *b, int *errors, int *warnings);
void lspui_restart(void);
void lspui_status(void);

/* tty.c */
int tty_busy(const struct tty *t);
int tty_busy_count(void);
int tty_cursor(int *sy, int *sx);
int tty_key(int c);
int tty_open_here(void);
int tty_toggle(void);
void tty_cleanup(void);
void tty_draw(void);
void tty_free(struct tty *t);
void tty_layout(void);
void tty_mouse(const struct mouse_event *ev);
int tty_find(void);
void tty_paste(const char *s, int len);
void tty_status(char *dst, size_t n);
void tty_tidy(void);

/* main.c */
void editor_cleanup(void);
void init_editor(void);

/* menu.c */
int menu_bar_mouse(const struct layer *self, const struct mouse_event *ev);
int menu_band_layout(int width, int open, int *col, int *cols);
int menu_count(void);
/* A drop-down's own height and its scroll position: arithmetic over row numbers
 * and item counts, with no screen in it, so the cases that only appear on a
 * terminal too short for a menu are provable without one. */
int menu_dropdown_height(int nitems, int top, int last_row);
int menu_dropdown_scroll(int nitems, int vis, int sel, int off);
int menu_for_mnemonic(int ch);
int menu_mnemonic_char(int i);
int menu_is_open(void);
int menu_open(int index);
void menu_bar_draw(const struct layer *self);

/* palette.c */
int palette_open(void);
int palette_score(const char *s, const char *pat);

/* regex.c */
char *regex_expand(const char *tmpl, const char *text,
                   const struct regex_match *m, int *outlen, const char **err);
int regex_exec(const struct regex *re, const char *text, int len, int start,
               struct regex_match *m);
int regex_ngroups(const struct regex *re);
struct regex *regex_compile(const char *pat, int flags, const char **err);
void regex_free(struct regex *re);

/* screen.c */
int screen_gutter_width(void);
int screen_poll_size(void);
void ab_append(struct abuf *ab, const char *s, int len);
void screen_draw_status_bar(int focused);
void screen_draw_text_area(void);
void screen_text_mouse(const struct mouse_event *ev);
void screen_window_area(int *x, int *y, int *w, int *h);
void editor_draw_text_row(int y, erow *row, const struct drow *d, int sel0, int sel1);
void editor_refresh_screen(void);
void editor_filter(void);
void editor_insert_output(void);

/* grep.c */
char *grep_command(const char *pat, int icase, int regex);
int grep_parse_line(const char *s, int len, int *path0, int *pathlen, int *line);
int grep_result_count(void);
int grep_jump(int delta);
/* Where a row of a list goes, for a finder that knows: see grep_show_list(). */
struct grep_place {
  const char *path;
  int line, col;     /* line from 1; col a byte offset */
};
int grep_show_list(const char *title, const char *text, int len, const char *query,
                   const struct grep_place *places, int nplaces);
void editor_grep(void);
void grep_cleanup(void);
void editor_set_status_message(const char *fmt, ...);
void screen_cleanup(void);
void screen_force_repaint(void);
void screen_init_layers(void);
void screen_init_size(void);
void screen_sync_size(void);

/* screen.c: the view's aids */
int view_curline(void);
int view_guides(void);
int view_occurrences(void);
int view_ruler(void);
int view_whitespace(void);
void view_set_curline(int on);
void view_set_guides(int on);
void view_set_occurrences(int on);
void view_set_ruler(int col);
void view_set_whitespace(int on);

/* search.c */
int search_case_is_on(void);
int search_has_query(void);
int search_regex_is_on(void);
void search_set_case(int on);
void search_set_regex(int on);
void editor_find(void);
void editor_find_next(void);
void editor_find_prev(void);
void editor_goto_line(void);
void editor_replace(void);
void editor_toggle_case_sensitive(void);
void editor_toggle_regex(void);
void search_cleanup(void);
int search_find_in_row(int cy, const char *query);

/* select.c */
/* The most a copy may hold: the clipboard's length is an int. A selection or
 * block bigger than this is refused, with *outlen -1 rather than 0. */
#define CLIP_MAX (INT_MAX - 1)
/* The most a copy may hold from a file too big to read into memory, where
 * the selection is read from the disk to be copied. */
#define BIG_COPY_MAX (64 << 20)
char *selection_dup(int *outlen);
int selection_bytes(void);
int selection_span(int *ax, int *ay, int *bx, int *by);
int selection_span_of(const struct cursor *c, int *ax, int *ay, int *bx, int *by);
void selection_row_rx_of(const struct cursor *c, int filerow, int *out0, int *out1);
void clipboard_set(const char *s, int len);
void clipboard_sync_system(void);
void editor_paste(void);
void editor_insert_text(const char *s, int len);
void selection_clear(void);
void selection_delete(void);
void selection_row_rx(int filerow, int *out0, int *out1);

/* sys.c */
char *xstrdup(const char *s);
int mkdir_p(const char *path);
int write_all(int fd, const void *p0, size_t n);
mode_t default_file_mode(void);
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
void fsync_parent_dir(const char *path);

/* tabbar.c */
int tabbar_mouse(const struct layer *self, const struct mouse_event *ev);
int tabbar_rows(void);
void tabbar_draw(const struct layer *self);

/* syntax.c */
const char *syntax_lang_id(const struct syntax *s);
const char *syntax_lang_name(int i);
const char *syntax_lsp(const struct syntax *s);
const char *syntax_line_comment(const struct syntax *s);
const char *syntax_name(const struct syntax *s);
const struct syntax *syntax_for_filename(const char *path);
int syntax_lang_count(void);
int syntax_auto_indent(const struct syntax *s);
int syntax_insert_spaces(const struct syntax *s);
int syntax_tab_width(const struct syntax *s);
int syntax_set_line(const char *lang, const char *key, const char *value,
                    const char **err);
void syntax_cleanup(void);
void syntax_ensure(int upto);
void syntax_ensure_view(void);
void syntax_init(void);
void syntax_invalidate_from(int row);
void syntax_retype(void);
const unsigned char *syntax_roles(int filerow);
void syntax_row(const struct syntax *s, const char *r, int rs, int state_in,
                unsigned char *out, int *state_out);

/* term.c */
_Noreturn void die(const char *s);
_Noreturn void die_because(const char *s, const char *why);
int get_window_size(int *rows, int *cols);
void term_guess_size(int *rows, int *cols);
int read_byte(unsigned char *c);
int term_mouse_is_on(void);
int term_take_winch(void);
void term_suspend(void);
void term_mouse_set(int on);
void disable_raw_mode(void);
void enable_raw_mode(void);

/* theme.c */
int style_eq(const struct style *a, const struct style *b);
int style_is_default(const struct style *s);
int theme_depth(void);
int theme_fit_color(int c);
int theme_rgb_to_256(int r, int g, int b);
int theme_truecolor(void);
void theme_index_rgb(int c, int *r, int *g, int *b);
int theme_detect_depth(const char *term, const char *colorterm);
int theme_detect_italic(const char *term);
int theme_italic(void);
void theme_set_italic(int on);
void style_default(struct style *out);
void style_emit_color(struct abuf *ab, int color, int is_bg);
void theme_init(void);
void theme_style(unsigned char role, struct style *out);

/* ui.c */
char *editor_open_dialog(void);
int editor_buffer_dialog(void);
void editor_options_dialog(void);
int editor_quit_dialog(void);
#define QUIT_SAVE    1
#define QUIT_DISCARD 2
#define QUIT_CANCEL  0
char *editor_prompt(char *prompt, void (*callback)(char *, int));
void editor_help(void);
int editor_open_in_new_buffer(char *path);
const char *editor_open_failure(void);

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
int undo_max_steps(void);
void undo_set_max_steps(int n);

/* utf8.c */
int utf8_char_bytes(const char *s, int size, int i);
int utf8_cp_cols(uint32_t cp);
int utf8_encode(uint32_t cp, char *dst);
int utf8_char_cols(const char *s, int size, int i, int *nbytes);
int utf8_col_to_byte(const char *s, int col);
int utf8_fit_cols(const char *s, int cols, int *out_cols);
int utf8_is_cont(unsigned char c);
int utf8_safe(const char *s, int nbytes);
int utf8_prev(const char *s, int i);
int utf8_boundary(const char *s, int size, int i);
int utf8_str_cols(const char *s);

/* vt.c */
const char *vt_reply(const struct vt *t, int *len);
const char *vt_take_title(struct vt *t);
const struct vt_cell *vt_line(const struct vt *t, int i, int *n);
int vt_cols(const struct vt *t);
int vt_key(const struct vt *t, int key, int mods, char *out);
int vt_mode(const struct vt *t, int mode);
int vt_mouse(const struct vt *t, const struct mouse_event *ev, int x, int y, char *out);
int vt_rows(const struct vt *t);
int vt_scrollback(const struct vt *t);
long vt_pushed(const struct vt *t);
int vt_take_bell(struct vt *t);
struct vt *vt_new(int rows, int cols, int scrollback);
void vt_cursor(const struct vt *t, int *y, int *x, int *visible);
void vt_feed(struct vt *t, const char *s, size_t n);
void vt_free(struct vt *t);
void vt_reply_done(struct vt *t);
void vt_resize(struct vt *t, int rows, int cols);

/* window.c */
int window_close_current(void);
int window_count(void);
void window_drag_end(void);
int window_index_of(const struct view *v);
int window_mouse(const struct layer *self, const struct mouse_event *ev);
int window_move_border(int dx, int dy);
int window_split(int kind);
int window_split_share(int kind, int share);
struct view *view_select(struct view *v);
struct view *window_view_at(int i);
void window_cleanup(void);
void window_draw(const struct layer *self);
void window_focus(int i);
void window_init(void);
void window_layout(void);
void window_layout_rect(int x, int y, int w, int h);
void window_move_focus(int delta);
void window_only(void);
void window_repoint_views(struct buffer *from, struct buffer *to);
void window_scroll_all(void);

/* wrap.c */
int drow_cursor_col(const struct cursor *c);
int drow_of_cursor(const struct cursor *c);
int drow_total(void);
void drow_at(int index, struct drow *out);
void wrap_row_invalidate(erow *row);
void wrap_row_changed(int at);
void wrap_rows_moved(struct buffer *b, int at);
void wrap_map_changed(struct buffer *b);
void wrap_step(struct cursor *c, int dir);

/* view.c */
int editor_row_cx_to_charcol(erow *row, int cx);
int editor_row_cx_to_rx(erow *row, int cx);
int text_cell(const char *s, int len, int i, int col, int tabw, int *nb);
int editor_row_rx_to_cx(erow *row, int rx);
void cursors_collapse(void);
void cursor_place(int cy, int cx);
int cursors_newest(void);
int cursors_skip_next(void);
int cursors_drop_last(void);
int cursors_add_all(void);
void cursors_init(void);

/* cursors.c */
int cursors_add(int cy, int cx);
int cursors_add_sel(int cy, int cx, int ex);
void cursors_dedup(void);
void cursors_mark_row(int y, int filerow, const struct drow *d);
int cursors_add_next(void);
int cursors_add_vertical(int down);
void cursors_line_edge(int to_end);
void cursors_move_all(int key);
void cursors_word(int dir);
void word_step(int *cy, int *cx, int dir);
void cursors_edit(int what, int arg);
void cursors_insert_text(const char *s, int len);
void cursors_insert_each(const char **s, const int *len);
int block_is_live(void);
void block_set(int ay, int arx, int cy, int crx);
void block_extend(int dy, int drx);
char *block_text(int *outlen);
int block_paste(void);
void cursors_sort(void);
void editor_move_cursor(int key);
void editor_scroll(void);

/* widget.c */
int dialog_hit(const struct dialog *d, const struct layer *l, int row, int col);
int dialog_key(struct dialog *d, int c);
int dialog_mouse(struct dialog *d, const struct layer *l, const struct mouse_event *ev);
int widget_button_cols(const char *text);
int widget_can_focus(const struct widget *w);
int widget_text_maxtop(const struct widget *w);
void dialog_draw(const struct layer *self);
void dialog_focus_move(struct dialog *d, int delta);
void dialog_init(struct dialog *d, const char *title, struct widget *widgets, int nwidgets, int cols, int rows);
void dialog_layout(struct dialog *d, struct layer *l);
void widget_button(struct widget *w, int x, int y, const char *text, int is_default);
void widget_check(struct widget *w, int x, int y, int cols, const char *text, int on);
void widget_label(struct widget *w, int x, int y, int cols, const char *text);
void widget_list(struct widget *w, int x, int y, int cols, int rows, int count, widget_row_fn row, void *user);
void widget_list_clamp(struct widget *w);
void widget_list_move(struct widget *w, int delta);
void widget_text(struct widget *w, int x, int y, int cols, int rows, int count, widget_row_fn row, void *user);
void widget_text_scroll(struct widget *w, int delta);
void widget_input(struct widget *w, int x, int y, int cols);
void widget_input_free(struct widget *w);
int widget_input_key(struct widget *w, int c);
int widget_input_draw(struct widget *w, int y, int x, int maxcols, int claim);
const char *widget_input_text(const struct widget *w);

#endif /* MOLLY_H */
