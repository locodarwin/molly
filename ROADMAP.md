# Molly Roadmap

Molly is olly's big sister: olly's code and feel, with the ceiling lifted. The
model is **[Fresh](https://github.com/sinelaw/fresh)** — a terminal editor that
behaves like a GUI one: menu bar, mouse, dialogs, splits, discoverable
everything. Molly aims at that experience in C, keeping olly's zero runtime
dependencies and instant start.

**Current state:** molly is olly at `b258ebe`, renamed. 3,311 lines in one file,
298 tests passing, `0.1.0-dev`. No new features yet.

**Reaching commands:** Ctrl keys *and* Alt keys, plus a menu system on Alt.
That decision is settled and it drives Phases 0-1 below.

---

## 1. What the menu system changes

Deciding on a menu bar is not a feature decision, it is an architecture
decision. Four consequences, three of which revise the previous roadmap:

**It makes the command table mandatory, not optional.** A menu entry needs a
label, an accelerator, a shortcut hint drawn right-aligned, an enabled/disabled
state, and an action. A `switch` on keypresses (molly.c:2955) can give us none
of those. So every action becomes a record:

```c
struct command {
    const char *name;      /* "search.regex" — stable, for config and macros */
    const char *label;     /* "Regular expression…"  (menu text) */
    int mnemonic;          /* offset of the underlined letter */
    const char *shortcut;  /* "Ctrl-F" — displayed, not parsed */
    int (*enabled)(void);  /* greys the item out */
    void (*run)(void);
};
```

Menu bar, keybindings, remapping, macros and the help screen then all derive
from one table. This is the single highest-leverage thing in the whole plan:
Fresh's command palette, keybinding editor and menus are all the same data
viewed three ways.

**It changes the renderer more than I first said.** Previously I proposed
attribute-aware *rows*. That is not enough. Drop-down menus and dialogs paint
*over* the text and must restore what was beneath; olly's cache holds one
rendered string per screen line (molly.c:347) and cannot express that. The
renderer needs to become a **cell grid**: a 2D array of `(codepoint, width,
fg, bg, attrs)`, which every element — text area, gutter, menu, dialog, status
bar — draws into, diffed against the previous frame to emit escape sequences.
This is how both Turbo Vision and ncurses work. It makes menus, dialogs, splits
and highlighting straightforward instead of painful, and it is strictly more
work than what I described before.

**It adds a whole subsystem I had omitted: a widget toolkit.** Molly needs
framed windows with z-order, drop-down menus, and dialogs with buttons, text
fields, checkboxes, radio groups and list boxes, with focus and tab order and a
modal event loop. This is bigger than any single editor feature on the list
below. The upside: olly already hand-rolls three full-screen UIs — the help
screen (molly.c:2522), the open-file dialog (molly.c:2747) and the prompt line —
and porting them onto the toolkit should *delete* code. That port is the proof
the toolkit works.

**It promotes mouse support to essential.** A menu bar you cannot click is
broken. Mouse moves from "Phase 2, nice to have" to Phase 1.

### The Alt-key collision, and how to resolve it

Alt is being asked to do two jobs: open menus (Alt-F for File) and run commands
(Fresh uses Alt-U for uppercase, Alt-L for lowercase, Alt-↑/↓ to move lines,
Alt-0..9 for bookmarks). Fresh hits this too — its docs note that disabling
menu-bar mnemonics is what "frees the Alt+letter keys."

Resolution: **the menu bar reserves only its eight title letters**
(`F E S V G T O H`), and every other Alt-letter is free for commands. Cheap, and
it means no feature has to be dropped over the conflict.

### Esc is the real input-layer hazard

`Alt-x` arrives at the terminal as `ESC x` — indistinguishable from the user
pressing Esc and then typing `x` except by timing. Molly already uses Esc to
cancel prompts, and Fresh additionally uses it to drop secondary cursors. The
standard fix is a short read timeout after a bare ESC, and it is a classic
source of bugs on slow SSH links.

This needs a dedicated input layer with its own tests, not a patch to
`editor_read_key` (molly.c:560). Proposed: **F10 opens the menu bar**, Esc stays
"cancel", Alt-letter opens a menu directly.

---

## 2. What we inherit, and what it costs

Three properties of olly's code are load-bearing for olly and in molly's way.
None are bugs; all are consequences of olly's rules.

**One global editor state.** `struct editorConfig E` (molly.c:50) mixes three
lifetimes, with undo as a second global (`U`) that is really a buffer property:

| Belongs to | Fields |
| --- | --- |
| the **buffer** (per file) | `row`, `numrows`, `rowcap`, `filename`, `dirty`, `eol_crlf`, `final_newline`, saved snapshot, recovery path, disk mtime, **and `U`** |
| the **view** (per window) | `cx`, `cy`, `rx`, `rowoff`, `coloff`, `screenrows`, `screencols`, `sel_active`, `sx`, `sy`, `linenum`, `gutter_w` |
| the **app** (global) | `clipboard`, `clipsize`, `statusmsg`, `orig_termios` |

Tabs, splits and multi-cursor are all blocked on separating these. Building any
of them first means building them twice.

**The render cache assumes one full-width text area** and carries no attributes.
Covered above.

**Memory is ~3.5× the file size.** Each `erow` keeps *two* copies of every line
— `chars` plus a tab-expanded `render` (molly.c:42) — and `editor_fill_rows`
(molly.c:2402) loads the whole file eagerly. Measured on this machine:

| File | Lines | Peak RSS | Ratio |
| --- | --- | --- | --- |
| 7.0 MB | 100,000 | 26.0 MB | 3.75× |
| 28.1 MB | 400,000 | 99.2 MB | 3.53× |

Fresh advertises multi-gigabyte files with negligible overhead. On this base a
1 GB file wants ~3.5 GB of RAM. **Matching Fresh there is not a tuning job, it
is a buffer rewrite** — see §5.

---

## 3. Phase 0 — Foundations — **done**

The boring phase, and the only one where we could *prove* we broke nothing. Molly
behaves exactly as it did; the suite grew from 298 checks to 367 (292 pty, 15
golden screens, 20 event-loop unit tests, 40 fuzz cases) plus the
allocation-failure sweep, and every file is clean under
`-Wall -Wextra -Wpedantic -Wshadow`.

- [x] **Golden-screen harness** (`tests/screen.py`, `tests/test_golden.py`). A
      terminal emulator the output is replayed through, so a snapshot records the
      text, a per-cell attribute map and the cursor position. This is what made
      the rest of the phase provable rather than hopeful — the renderer was
      replaced wholesale and the snapshots did not move.
- [x] **Cursor list.** `struct cursor` (position + own anchor) in a list, N==1,
      reached through `CUR`, clamped once per frame from `editor_scroll`.
- [x] **Display-row view model.** Scroll offset, draw loop, row renderer and
      vertical movement address display rows; unwrapped is the identity mapping.
- [x] **State split** into `struct buffer` / `struct view` / app globals, with
      undo moved inside the buffer and row access behind `row_count()`/`row_at()`,
      which resolve through the view's buffer rather than naming the global.
- [x] **Modularized**: one 3,446-line file became `src/molly.h` plus sixteen `.c`
      files. `sys.c` and `term.c` are deliberately where the calls live that a
      Windows port has to replace. Five statics that were being shared across the
      new boundaries each became an API rather than an `extern`.
- [x] **Cell-grid renderer** (`src/grid.c`): the screen as cells of
      `(bytes, width, attributes)` in two planes, diffed once per frame. The
      per-line byte cache, the row scratch buffer and the welcome-line buffer all
      went with it.
- [x] **Command table** (`src/cmd.c`): thirty named commands with labels, help
      text and groups; the keymap written as chord text and parsed at startup; the
      help screen's key column and descriptions generated from it. The dispatcher
      never names an action.
- [x] **Input layer**: Esc/Alt disambiguation, Alt-printable, F1-F12 in all three
      forms terminals send them, and reads inside a sequence that wait ~300ms
      instead of giving up after one tick — which is what mangled sequences split
      by a slow link.
- [x] **Event loop** (`src/loop.c`): waits on the keyboard and any registered
      descriptor together, behind our own interface rather than `poll()`, because
      Windows needs `WaitForMultipleObjects`. LSP and the integrated terminal both
      depend on this existing.

### What Phase 0 deliberately did not finish

Each of these is cheap to do when the feature that needs it arrives, and would
have been speculative now:

- **Shift is not part of a chord.** It arrives out of band from the input layer
  because it modifies movement rather than selecting a different command. Folding
  it into the key code changes the input layer's contract, and belongs with the
  remappable keymap.
- **The help screen and open-file dialog are not on the grid.** They still write
  escape sequences directly and then ask for a full repaint, which is what keeps
  the grid honest about a screen it did not draw. Phase 1's widget toolkit brings
  them in — and should delete code doing it.
- **`erow` is still visible to the renderer and to search**, so the buffer is not
  fully isolated. Closing that gap is part of the big-file rewrite, not of
  laying a seam for it.
- **The keymap is still olly's.** The CUA moves settled on in §6 are feature work.

## 4. Phase 1 — The UI shell

- Windows with z-order on the cell grid; save/restore underneath.
- Menu bar: titles, drop-downs, mnemonics, shortcut hints, greyed-out items,
  keyboard and mouse navigation, F10 activation.
- Dialog toolkit: frame, button, input field, checkbox, radio group, list box,
  focus/tab order, modal loop.
- Mouse (SGR 1006): click menus and dialogs, click to position the cursor, drag
  to select, wheel to scroll.
- **Port olly's help screen, open-file dialog and prompt line onto the toolkit.**
  Net lines of code should go *down*.
- Command palette with fuzzy matching — nearly free once the table exists, and
  it is how you reach the long tail of commands without memorizing anything.

---

## 5. The feature set, with the arguments

Verdicts: **IN** = planned, **LATER** = after the core lands, **NO** = declined
on principle, **CALL** = needs your decision.

### Cheap and clearly worth it — IN, no real argument against

Each is small *given Phases 0-1*, and most are one menu entry plus a function.
Grouped by where they live:

- **Edit:** select all / word / line, delete word back/forward, delete to end of
  line, transpose, duplicate line, move line up/down, join lines, toggle
  comment, indent/dedent with Tab/Shift-Tab, case conversion, sort selected
  lines, trim trailing whitespace.
- **Smart editing:** smart Home (first non-blank ↔ column 0), smart Backspace
  (eat one indent level), auto-indent on Enter, auto-close brackets and quotes,
  surround selection — all configurable, because each one annoys somebody.
- **Navigate:** go to matching bracket, word-wise movement, position history
  (jump back where you were), bookmarks 0-9.
- **View:** whitespace indicators, indent guides, current-line highlight,
  vertical rulers, occurrence highlighting of the word under the cursor. These
  are nearly free once the grid has attributes, and they are most of what makes
  an editor *feel* modern.
- **File:** recent files, read-only mode (automatic for system files).

### Regex search and replace — **IN, first feature**

- **For:** highest value per line on the list. POSIX `<regex.h>` is already in
  libc — I compiled a test to confirm it needs no package and no flags. Olly
  excluded regex to avoid "a matching engine as a dependency"; that reason does
  not survive the fact that it is already linked.
- **Against:** POSIX ERE is not PCRE. No `\d`, no lazy quantifiers, no
  lookaround, no named groups. Users who know PCRE will file bugs.
- **Decided: a hand-rolled PCRE-subset engine**, not POSIX ERE. ERE was the free
  option but it has no `\d`, no lazy quantifiers and no lookaround, and those
  are what people actually reach for. Scope: character classes and shorthands,
  greedy and lazy quantifiers, groups with captures, alternation, anchors, word
  boundaries, and UTF-8-aware matching over the raw byte buffer.
- **The risk to design for is catastrophic backtracking.** A pattern like
  `(a+)+b` against a long line will hang a naive backtracker, and this one runs
  on every keystroke of an incremental search. So: a step budget that aborts the
  match and reports "pattern too complex" rather than freezing the editor, and a
  fuzz target from day one, in the spirit of the existing replace-all fuzz.
- Capture groups in replacements (`\1`).

### Multiple buffers and tabs — **IN**

- **For:** the most-missed thing about olly, and mostly bookkeeping once Phase 0
  separates buffer from view. Prerequisite for File→Close, the file explorer,
  and grep results.
- **Against:** every "are you sure?" path multiplies — unsaved changes on quit
  becomes a list, not a question. Crash recovery has to cover N buffers.

### Window splits — **IN**

- **For:** the other half of what a GUI-like editor means. Free-ish after the
  cell grid and `struct view`.
- **Against:** the gutter, horizontal scroll and wide-character alignment all
  measure against `TEXT_COLS()` (molly.c:112), which reads globals; every one of
  those needs to become per-view. Expect a long tail of off-by-one bugs, which
  is exactly what the golden-screen harness is for.

### Syntax highlighting — **IN**

- **For:** table-driven keyword/string/comment/number rules per filetype. Kilo
  had it; olly dropped it. Visible payoff on every single keystroke of use.
- **Against:** a naive line-at-a-time lexer gets multi-line strings and comments
  wrong, so rows need a cached lexer state and invalidation on edit. That is the
  real cost, not the rules themselves.
- **Scope:** regex- or table-driven definitions in config, so adding a language
  is not a recompile. Explicitly *not* a parser per language.

### Indentation, filetypes and configuration — **IN**

- **For:** tabs vs spaces, tab width, auto-indent, per-filetype defaults. Olly's
  "no configuration" was its most limiting rule, and a menu-driven editor needs
  an Options menu with dialogs that persist — so config is required, not
  optional. `~/.config/molly/mollyrc`.
- **Against:** the moment config exists, every bug report needs a config file
  attached, and "molly behaves the same on every machine" is over. Mitigation:
  ship defaults that match today's behavior exactly, and make the file
  machine-writable from the Options dialogs rather than hand-edited.

### Shell filter — **IN**

- **For:** pipe the selection through a command, replace it with the output.
  Roughly a hundred lines for an unbounded amount of capability — `sort`, `jq`,
  `fmt`, `black`, anything. The best value-per-line item on the list after
  regex. Fresh binds Alt-|.
- **Against:** needs care around a command that hangs or floods output
  (timeout + cancel key + output cap), and it is the first place molly executes
  something on the user's behalf.

### Soft wrap — **IN** (structural groundwork landed)

- **For:** people expect it; prose and Markdown are unusable without it.
- **Against:** it breaks the one-row-equals-one-line assumption that cursor
  movement, scrolling, selection, the gutter and the renderer all rest on. In
  my estimation this is the second most invasive feature on the list, behind
  multi-cursor. Done late it is a refit of everything; done in Phase 0 — by
  making the view address *display rows*, not file rows, from the start — it is
  much cheaper.
- **Landed:** the view now addresses *display rows* rather than file rows. The
  scroll offset, the draw loop, the row renderer and vertical movement all speak
  display rows, with the unwrapped mapping being the identity. Turning wrap on
  means implementing three functions (`drow_total`, `drow_of_cursor`, `drow_at`)
  instead of revisiting every caller. A slice is expressed in *render columns*,
  not byte offsets, because wrapping happens where glyphs land on screen.

### Multi-cursor and block selection — **IN** (model landed, feature later)

- **For:** the headline feature of modern editors, and Fresh has it (Ctrl-D).
  Once you have it you use it constantly.
- **Against:** the most invasive item here. Every edit path, every undo group and
  the entire selection model change: one cursor and one anchor become N of each,
  edits must apply without invalidating each other's positions, and undo has to
  collapse N simultaneous edits into one step.
- **Landed:** the cursor is now `struct cursor` (position + its own selection
  anchor) held in a list, with `E.ncursors == 1` and a `CUR` macro for the
  primary. `cursors_clamp()` runs from `editor_scroll`, so every cursor is valid
  before each frame regardless of which edit path ran, and `cursors_collapse()`
  drops the extras when positions stop meaning anything (loading another file).
  The feature itself — Ctrl-D, N simultaneous edits, undo collapsing them into
  one step — comes after buffers and splits. Block selection follows it.

### Search in files / grep — **LATER**

- **For:** big win; can shell out to `grep` or `git grep` for zero dependency.
- **Against:** needs a results panel, so it is gated on splits and buffers.
  Shelling out means depending on the host's tools; implementing it internally
  means a recursive walk plus ignore-file handling.

### Keyboard macros — **LATER, cheap**

- **For:** record a command sequence and replay it; nearly free once commands are
  named table entries, since recording is just logging names. Registers 0-9.
- **Against:** macros that span prompts and dialogs need those recorded too,
  which is where this stops being free.

### Code folding — **LATER**

Same row-mapping problem as soft wrap, and it should reuse whatever mechanism
soft wrap establishes rather than inventing a second one. Indent-based folding
only; LSP folding is out until LSP is in.

### Git integration — **LATER**

Shelling out to `git` for blame, log and diff gutter marks costs no dependency
and is genuinely useful. Not core, and gated on panels.

### Integrated terminal — **IN**

- **For:** Fresh has one; it is a real part of the "never leave the editor" pitch.
- **Against:** a pty, a second input loop, scrollback, resize handling and
  terminal emulation — effectively a second application inside molly. It is the
  largest item on this list that is not LSP.
- **Decided: in.** It is the largest non-LSP item here, so it comes late, after
  splits and panels exist to host it. Two things to get right early: the main
  event loop must already be able to wait on a child's output alongside keyboard
  input (the same requirement LSP has, so they share it), and this is the first
  feature that is genuinely hard to port to Windows, where it needs ConPTY.
- Suspend-to-shell (Ctrl-Z job control) is worth having anyway and costs almost
  nothing.

### LSP — **IN**

- **For:** completion, diagnostics, go-to-definition and rename are the line
  between "comfortable editor" and "development environment." Doable in C with
  no external library: hand-rolled JSON is a few hundred lines, and the protocol
  is line-delimited over a pipe.
- **Against:** easily the largest subsystem in the project — async I/O
  interleaved with the input loop, subprocess lifecycle, per-language server
  configuration, a completion popup UI, and protocol churn to track forever. It
  also pulls in its own UI surface (hover, diagnostics panel, signature help).
- **Decided: in.** Which makes two things non-negotiable in Phase 0 that would
  otherwise have been optional: the event loop must multiplex keyboard input with
  child-process I/O from the start (retrofitting that into a blocking read loop
  is painful), and the widget toolkit needs a popup/overlay primitive for
  completion and hover.
- Build order: diagnostics first (display-only, no new UI interaction), then
  go-to-definition and hover, then completion last, since completion needs the
  most UI and is the most sensitive to latency.

### Plugin runtime — **NO**

- **For:** Fresh ships TypeScript plugins in a sandboxed QuickJS, and it is how
  an editor grows features without the maintainer writing them.
- **Against:** embedding a JS engine ends "zero runtime dependencies" and
  "starts instantly" in a single commit. It is the one item on this list that
  contradicts molly's inherited premise outright.
- **Molly's answer instead:** the command table, remappable keys, config-defined
  syntax rules and the shell filter. That covers a large fraction of what people
  write plugins for, with no engine.

### Also declined

- **Coding agents / workspace orchestrator** (Fresh's differentiator) — a
  separate application, not an editor feature.
- **Markdown preview, custom backgrounds, i18n** — out of character for the
  effort.
- **A GUI**, a daemon, or anything that cannot work on a bare terminal over SSH.

### Big-file handling — **IN**

Replacing the `erow[]` array with a piece table or gap buffer, plus dropping the
duplicated `render` string, gets molly from 3.5× file size toward ~1×, and is
the only route to Fresh's multi-gigabyte claim.

- **Against:** weeks of invisible work; users see nothing. Most real source files
  are under a megabyte, where 3.5× is irrelevant.
- **For:** log files and data dumps are exactly where a fast terminal editor
  should win, and that is where molly currently falls over.
- **Decided: in.** Phase 0 still builds `struct buffer` as a seam with all row
  access behind accessors, because that is what makes the swap survivable — but
  the swap is now planned work rather than a maybe. Dropping the duplicated
  `render` string is the cheaper half and can come first: it is most of the 3.5×
  and does not need a new data structure, only rendering on demand.

---

## 6. Keymap: CUA bindings, fully customizable

Fresh is CUA-style, and olly already agrees on the important ones (`Ctrl-S/O/Q/
F/R/G/Z/Y/C/X/V`, Shift-arrow selection). Four genuine conflicts:

| Key | olly today | Fresh | Suggested resolution |
| --- | --- | --- | --- |
| `Ctrl-L` | repaint | select current line | Fresh wins; repaint → View menu / `Ctrl-Shift-L` |
| `Ctrl-T` | toggle case-sensitive search | transpose characters | Fresh wins; case becomes a **checkbox in the Find dialog**, which is better UI than a hidden toggle |
| `Ctrl-N` | find next | — (`F3`) | `F3`/`Shift-F3` primary, keep `Ctrl-N/P` as aliases |
| `Ctrl-?` | help | `F1` | `F1` primary, keep `Ctrl-?` |

Newly claimed by Fresh conventions: `Ctrl-A` select all, `Ctrl-W` select word,
`Ctrl-D` add cursor, `Ctrl-K` delete to end of line, `Ctrl-/` toggle comment.
None of these are bound in olly, so they are free.

Note that two of these resolutions *improve* the UI rather than trading it away
— a Find dialog with a visible "Match case" checkbox is strictly better than
`Ctrl-T` with a status-bar message.

**Every binding is remappable**, and that is a requirement rather than a feature,
because it decides the shape of the keymap code. It means:

- No keypress may be handled by a hardcoded `switch`. The table maps a *chord* to
  a *command name*, and the dispatcher never names an action directly. This is
  the same table the menu bar, the command palette and the help screen read, so
  there is exactly one place a binding exists.
- A chord needs a real representation — modifiers plus a key, including `F1`-`F12`,
  Alt-letter, and multi-key sequences like `Ctrl-K Ctrl-F` — with a parser and a
  printer, because the config file writes them as text (`ctrl-shift-l`) and the
  menus display them.
- Conflicts are data, not bugs: two commands can want one chord, so binding
  resolution needs a defined precedence (user config beats the built-in keymap)
  and the keybinding dialog has to show and resolve collisions.
- Named keymaps, so an olly-compatible set can ship alongside the CUA default
  and olly muscle memory is a config line rather than a fork.

The one thing *not* remappable is the eight Alt-letter menu titles, since those
are structural to the menu bar.

---

## 7. Decisions made

| Question | Decision |
| --- | --- |
| Command access | Ctrl **and** Alt keys, plus a menu system on Alt |
| Soft wrap | **In.** Display-row view model landed |
| Multi-cursor | **In.** Cursor-list model landed; feature after buffers and splits |
| LSP | **In.** Forces an async event loop and overlay widgets into Phase 0 |
| Integrated terminal | **In**, late — needs panels first |
| Big files | **In.** Buffer rewrite is planned, not optional |
| Keymap | CUA on the four conflicts, and **fully customizable** |
| Regex | **Hand-rolled PCRE subset**, with a step budget and a fuzz target |
| Platforms | **Linux now. Windows possibly later. No macOS.** |
| Plugin runtime | **No** — it would end zero-dependency and instant start |

### What "Windows later" costs, and what to do about it now

Not planning for it at all would be a mistake, and planning for it fully would
slow everything down. The middle path: keep every platform-specific call behind
the `term` module and one small `sys` module, and do not let the rest of molly
call POSIX directly.

The things that will not port, so that they are isolated rather than scattered:

- **`termios` raw mode and SIGWINCH** (molly.c:522, molly.c:493) → Windows uses
  `SetConsoleMode` and a resize event in the input queue. This is why the input
  layer is a Phase 0 module rather than a function.
- **The integrated terminal** → ConPTY instead of `forkpty`, which is the single
  biggest port item.
- **Atomic save** (molly.c:2244) → `rename` over an existing file fails on
  Windows; it needs `MoveFileEx` with `MOVEFILE_REPLACE_EXISTING`. Preserving
  mode and owner means something different there too.
- **Crash recovery via signal handlers** (molly.c:480) → no `SIGHUP`/`SIGTERM`;
  a console control handler instead.
- **`select`/`poll` on a console handle** does not work; async input and child
  I/O need `WaitForMultipleObjects`. Since LSP and the terminal both depend on
  that loop, its interface should be ours, not POSIX's, from the start.

No Windows code gets written now. The point is that the seams land in Phase 0,
where they are free, instead of being carved out later.

## 8. Still open

- **Phase 2 ordering** beyond "regex first".
- **Which languages** ship with syntax definitions initially.
- **Config file syntax** — it has to be machine-writable, since the Options
  dialogs save to it.
