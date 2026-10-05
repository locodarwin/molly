# Molly Roadmap

Molly is olly's big sister. Olly's feature list ends where its design rules
begin: one C file, one cursor, one file at a time, no regex, no config, no
syntax awareness. Molly inherits olly's code and keeps its *feel* — instant
start, zero runtime dependencies, modeless, careful about your data — while
lifting the ceiling.

**Current state:** molly is olly at commit `b258ebe`, renamed. 3,311 lines in
one file, 298 tests passing, version `0.1.0-dev`. Nothing new yet.

---

## What we inherit, and what it costs us

Three things in olly's code are load-bearing for *olly* and in the way for
*molly*. These are not bugs; they are the consequence of olly's rules.

**1. One global editor state.** `struct editorConfig E` (molly.c:50) mixes
three different lifetimes in one struct:

| Really belongs to | Fields |
| --- | --- |
| the **buffer** (per open file) | `row`, `numrows`, `rowcap`, `filename`, `dirty`, `eol_crlf`, `final_newline`, saved snapshot, recovery path, on-disk mtime |
| the **view** (per window onto a buffer) | `cx`, `cy`, `rx`, `rowoff`, `coloff`, `screenrows`, `screencols`, `sel_active`, `sx`, `sy`, `linenum`, `gutter_w` |
| the **app** (global) | `clipboard`, `clipsize`, `statusmsg`, `orig_termios` |

Plus `struct undoState U` as a second global, which is really a *buffer*
property. Multiple buffers, splits, and multi-cursor are all blocked on
separating these. Doing any of those features before the split means writing
them twice.

**2. The render cache assumes one full-width text area.** Partial refresh
(molly.c:347-416) caches one rendered string per *screen line* and diffs against
it. `TEXT_COLS()` (molly.c:112) reads the globals directly. It is a good design
for one window and the wrong shape for two. It also carries no attribute
information, so syntax highlighting has nowhere to live.

**3. The row array.** Rows are a flat `erow[]`; inserting or deleting a line
`memmove`s the array, and each row keeps a second `render` string with tabs
expanded. Fine to a few hundred thousand lines. This is the one item I would
*not* rush to replace — see Phase 5.

So: **Phase 0 is structural, and everything else sits on top of it.**

---

## Phase 0 — Foundations (no user-visible change)

The deliberately boring phase. Ends with molly behaving identically and all 298
tests still green, which is the whole point: it is the only phase where we can
prove we broke nothing.

- Split `molly.c` into modules with a real build. Proposed seams, which fall
  almost exactly along olly's existing comment banners: `utf8`, `term`,
  `buffer` (rows + load/save), `undo`, `view` (scroll + render), `screen`
  (draw + cache), `search`, `select`, `input`, `ui` (help, prompts, browser),
  `main`.
- Introduce `struct buffer` and `struct view`; move `U` into `struct buffer`.
  Keep exactly one of each at first, so behavior cannot change.
- Make the renderer attribute-aware: a row renders to cells carrying
  `(byte, width, attr)`, not a raw byte string. Highlighting, selection, search
  match emphasis and the gutter then all use one mechanism instead of three.
- Keep the test suite passing at every commit; add a golden-screen test harness
  so later phases can assert on what the screen *looks like*.

## Phase 1 — Reaching the features (the gating UX decision)

Olly spends **18 of ~26 usable Ctrl keys**. Molly cannot add two dozen commands
to that keyboard. Before any feature lands we need the answer to "how do I
invoke this?" — see **Decisions needed, #1**. Likely components:

- A command dispatch layer: every action becomes a named command
  (`buffer.next`, `search.regex`, `indent.reindent`) with one table mapping
  keys to names. Remappable config and a command palette both fall out of this
  for free; hardcoded `switch` cases give us neither.
- Discoverability to match: the help screen generated *from* the command table
  rather than hand-maintained alongside it (molly.c:2522).

## Phase 2 — The features that need no new dependency

Ranked by value-per-line, and all reachable once Phases 0-1 land.

- **Regex search and replace.** POSIX `<regex.h>` is in libc — I verified it
  compiles with no extra flags or packages. Olly excluded regex to avoid "a
  matching engine as a dependency"; that reason does not survive contact with
  the fact that it is already linked. Capture groups in replacements (`\1`).
- **Multiple buffers.** Open several files, switch, a buffer list. Phase 0 makes
  this mostly bookkeeping.
- **Window splits.** Horizontal and vertical, several views on one buffer or on
  different buffers.
- **Syntax highlighting.** Table-driven language definitions (keywords, strings,
  comments, numbers) — kilo had this and olly dropped it. Needs Phase 0's
  attribute cells.
- **Indentation that works.** Tabs vs spaces, tab width, auto-indent, reindent
  region, and per-filetype defaults.
- **Configuration file.** `~/.config/molly/mollyrc`, plus a vendored default so
  molly with no config behaves exactly like molly today.
- **Mouse support.** SGR 1006: click to position, drag to select, wheel to
  scroll. Cheap, and immediately expected by anyone who tries it.
- **Multi-cursor.** The one item here that is genuinely invasive: every edit
  path and the undo grouping have to cope with N cursors.
- **Line and text operations.** Duplicate, move line up/down, join, toggle
  comment, sort selection, upper/lower, rectangular selection.
- **Soft wrap**, bracket matching, auto-close pairs, trailing-whitespace
  handling.
- **Shell filter.** Pipe the selection through a command and replace it with the
  output. Enormous capability for about a hundred lines.
- **Persistent state.** Remembered cursor position per file, recent files.

## Phase 3 — Polish

Themes and true color, a configurable status line, jump list and marks, macro
record/replay, undo tree navigation, better large-file feedback.

## Phase 4 — Deliberate non-goals (still)

Molly drops olly's limits, not its judgment. I would keep saying no to:

- **A plugin runtime / embedded scripting language.** This is the one that ends
  "zero dependencies" and "starts instantly" in a single commit.
- **Anything that makes startup perceptibly slow**, needs a daemon, or cannot
  work on a bare terminal over SSH.
- **A GUI.**

**LSP is the genuinely open question**, not a settled no — completion and
diagnostics are what separate "comfortable editor" from "development
environment", but it means JSON, subprocess management and async I/O. That is a
scope decision, not a technical one: see **Decisions needed, #5**.

## Phase 5 — Only if the files demand it

Replacing the row array with a piece table or gap buffer. This is weeks of work
that users cannot see, and it is justified only once a real file is measurably
slow. Phase 0's `struct buffer` is the seam that lets us do it later without
touching the rest of molly — building that seam is the actual deliverable here;
using it is optional.

---

## Decisions needed from you

These change the order and the shape of the work, so they come before code.

**1. How do you reach ~40 commands from a keyboard with 8 free Ctrl keys?**
The gating question. Options: a command palette; a leader/prefix key
(`Ctrl-K` then a letter); an Alt/Meta layer; or a `:` command line. This sets
molly's feel more than any single feature.

**2. Does molly stay strictly modeless?** Olly's "no modal editing" was a
promise to the user, not a limitation of the code. Keeping it is my
recommendation; an *optional* mode is possible but doubles the input layer.

**3. One file or modules?** I recommend modules — the alternative is a
10,000-line single file. But olly's one-file rule was partly an aesthetic you
may want molly to keep.

**4. Which Phase 2 features matter most to you?** My ordering is
regex → buffers → splits → highlighting → indentation → config → mouse, on
value-per-line. Yours may differ, and multi-cursor in particular is expensive
enough that it should be a deliberate choice rather than a default.

**5. Where is molly's ceiling?** "A really comfortable nano" and "an
IDE-lite with LSP and completion" are different projects. This decides Phase 4.

**6. Must molly stay keybinding-compatible with olly**, so muscle memory
carries over? Cheap to promise now, expensive to retrofit.

**7. Platforms.** Olly's CI is Ubuntu only. If macOS and the BSDs matter, that
constrains terminal and filesystem code from the start rather than later.
