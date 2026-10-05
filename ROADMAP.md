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

## 3. Phase 0 — Foundations (no user-visible change)

The boring phase, and the only one where we can *prove* we broke nothing: it
ends with molly behaving identically and all 298 tests green.

1. **Modularize.** Split along olly's existing comment banners: `utf8`, `term`,
   `input`, `buffer`, `undo`, `view`, `screen`, `search`, `select`, `ui`,
   `cmd`, `main`.
2. **Split the state** into `struct buffer` / `struct view` / app globals; move
   `U` into `struct buffer`. Keep exactly one of each at first, so behavior
   cannot change.
3. **Cell-grid renderer** with attributes and a frame diff, replacing the
   per-line string cache.
4. **Command table**, with the existing ~20 actions as its first entries and the
   help screen generated from it.
5. **Input layer** with ESC/Alt disambiguation and tests.
6. **Golden-screen test harness**, so later phases can assert on what the screen
   looks like rather than only on file contents.

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
- **Open:** POSIX ERE (free, now) or a hand-rolled PCRE-subset engine (~1,000
  lines, no dependency, ours to debug forever). I recommend shipping ERE and
  revisiting only if it genuinely chafes.
- Capture groups in replacements (`\1`) either way.

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

### Soft wrap — **CALL** (recommend IN, scheduled deliberately)

- **For:** people expect it; prose and Markdown are unusable without it.
- **Against:** it breaks the one-row-equals-one-line assumption that cursor
  movement, scrolling, selection, the gutter and the renderer all rest on. In
  my estimation this is the second most invasive feature on the list, behind
  multi-cursor. Done late it is a refit of everything; done in Phase 0 — by
  making the view address *display rows*, not file rows, from the start — it is
  much cheaper.
- **So the decision is needed early even if the feature ships late.**

### Multi-cursor and block selection — **CALL** (recommend LATER)

- **For:** the headline feature of modern editors, and Fresh has it (Ctrl-D).
  Once you have it you use it constantly.
- **Against:** the most invasive item here. Every edit path, every undo group and
  the entire selection model change: one cursor and one anchor become N of each,
  edits must apply without invalidating each other's positions, and undo has to
  collapse N simultaneous edits into one step.
- **Compromise I recommend:** in Phase 0, make the selection model a *list* of
  cursors with N==1. Costs almost nothing now, turns multi-cursor from a rewrite
  into a feature later. Block selection then comes nearly free with it.

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

### Integrated terminal — **CALL** (recommend NO for v1)

- **For:** Fresh has one; it is a real part of the "never leave the editor" pitch.
- **Against:** a pty, a second input loop, scrollback, resize handling and
  terminal emulation — effectively a second application inside molly. It is the
  largest item on this list that is not LSP.
- **Cheaper 80%:** suspend to the shell (Ctrl-Z job control) and the shell
  filter, which together cover most of what people actually use it for.

### LSP — **CALL** (genuinely open; recommend "architect for it, defer it")

- **For:** completion, diagnostics, go-to-definition and rename are the line
  between "comfortable editor" and "development environment." Doable in C with
  no external library: hand-rolled JSON is a few hundred lines, and the protocol
  is line-delimited over a pipe.
- **Against:** easily the largest subsystem in the project — async I/O
  interleaved with the input loop, subprocess lifecycle, per-language server
  configuration, a completion popup UI, and protocol churn to track forever. It
  also pulls in its own UI surface (hover, diagnostics panel, signature help).
- **Recommendation:** keep the event loop and panel system LSP-capable, build
  nothing yet, and decide once the editor is pleasant without it.

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

### Big-file handling — **CALL**

Replacing the `erow[]` array with a piece table or gap buffer, plus dropping the
duplicated `render` string, gets molly from 3.5× file size toward ~1×, and is
the only route to Fresh's multi-gigabyte claim.

- **Against:** weeks of invisible work; users see nothing. Most real source files
  are under a megabyte, where 3.5× is irrelevant.
- **For:** log files and data dumps are exactly where a fast terminal editor
  should win, and that is where molly currently falls over.
- **Recommendation:** build `struct buffer` in Phase 0 as a clean seam with all
  row access behind accessors, then swap the implementation later without
  touching the rest of molly. Build the seam now; use it when a real file is
  measurably slow.

---

## 6. Keymap: CUA/Fresh vs olly muscle memory — **CALL**

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

---

## 7. Open decisions

1. **Soft wrap** — in or out? Needed early (it shapes the view model) even if it
   ships late.
2. **Multi-cursor** — accept the cursor-list model in Phase 0 and build the
   feature later, or commit to it properly, or drop it?
3. **LSP** — architect-and-defer (my recommendation), commit now, or rule out?
4. **Integrated terminal** — or settle for suspend + shell filter?
5. **Big files** — does multi-gigabyte matter enough to rewrite the buffer?
6. **Keymap** — adopt CUA/Fresh on the four conflicts above, or preserve olly's
   bindings?
7. **Regex flavor** — POSIX ERE now, or hand-roll a PCRE subset?
8. **Platforms** — olly's CI is Ubuntu only. If macOS and the BSDs matter, that
   constrains terminal and filesystem code from the start.
