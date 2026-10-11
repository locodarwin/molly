# Molly Roadmap

Molly is olly's big sister: olly's code and feel, with the ceiling lifted. The
model is **[Fresh](https://github.com/sinelaw/fresh)** — a terminal editor that
behaves like a GUI one: menu bar, mouse, dialogs, splits, discoverable
everything. Molly aims at that experience in C, keeping olly's zero runtime
dependencies and instant start.

**Current state:** Phases 0 and 1 are done, and Phase 2 has eleven features
finished end to end: regular-expression search and replace on an engine of its
own, a configuration file with a remappable keymap, the CUA keys, any number of
files open at once, a tab bar, syntax highlighting for seven languages, window
splits, a shell filter, indentation that follows the filetype, multiple cursors
and soft wrap. Molly is 15,760 lines across `src/molly.h` and thirty-two `.c`
files, with a menu bar whose drop-downs scroll when the terminal is shorter than
they are, a mouse, framed dialogs that have buttons and checkboxes on them, a
command palette and a keyboard reference generated from its own command table.
1,642 checks pass (750 pty, 105 golden screens, 787 unit) plus 4,040 fuzz
cases and the allocation-failure sweep, and every file is clean under `-Wall -Wextra -Wpedantic -Wshadow`. Still `0.1.0-dev`: the *interface* is
molly's own, and the *editing* features are mostly still olly's.

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

Tabs, splits and multi-cursor were all blocked on separating these. Building any
of them first would have meant building them twice; all three have since landed
on the separation this section argued for, and none of them had to revisit it.

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

Phase 0 built the substrate: a cell grid, named commands, an input layer that
knows Alt from Esc, and a loop that can wait on a child process. Phase 1 builds
the thing that substrate was for — a menu bar, dialogs and a mouse — and proves
it by deleting the three hand-rolled full-screen UIs olly left behind.

Ordered so that each step stands on a finished one, and so the port that proves
the toolkit happens before the menu bar that depends on it.

### 1. Color in the grid — **done**

A cell's attributes are two bits today, reverse and gray, which is all olly ever
needed. Menus, dialogs and syntax highlighting all need color, and that is a
change to the grid's cell and to the SGR encoder, so it comes first.

- Cells carry a foreground and background from a **semantic palette** — roles like
  `menu`, `menu.selected`, `menu.disabled`, `dialog.frame`, `dialog.focus`,
  `status`, `gutter` — never raw color numbers at the call site. A theme then
  becomes a palette, and syntax highlighting in Phase 2 adds roles rather than
  touching the renderer.
- The SGR encoder keeps emitting minimal transitions, as it does now for reverse
  and gray.
- Nothing visible changes yet: the existing roles map to exactly what is drawn
  today, so the fifteen golden screens must still pass untouched.
- `tests/screen.py` learns to record foreground and background per cell, and the
  golden snapshots gain a color map. Without this the rest of the phase is
  untestable, which is the same reason the golden harness came first in Phase 0.

### 2. A layer stack — **done**

Olly's three UIs each cleared the screen, drew, and asked for a full repaint on the
way out. With a grid that is rebuilt every frame there is nothing to save and
restore — the roadmap's earlier wording about preserving what is underneath was a
holdover from thinking in byte-strings. What is needed is only an ordered list of
things to draw.

- A view is bounds plus a draw function: the text area, a menu, a dialog, later a
  completion popup or a split.
- Drawing walks the stack bottom to top, so overlap is just draw order.
- The text area becomes the bottom view rather than a special case, which is the
  groundwork splits need in Phase 2.

### 3. One key wait, through the event loop — **done**

`editor_prompt`, `editor_help` and `editor_open_dialog` each run `while (1) { draw;
editor_read_key(); }`. That read blocks, so while any of them is open the event
loop is not running and nothing else is serviced. Harmless now; a language server
whose diagnostics stop arriving whenever a dialog is open, or an integrated
terminal that freezes mid-build, is not.

- One `ui_wait_key()` that waits through `loop_wait`, so registered sources are
  serviced whatever is on screen, and every modal loop uses it.
- See **Decisions, A** for whether this stays a modal loop at all.

**What landed, and where it differs from the plan above.** Cells carry a *role*,
and `theme.c` turns a role into a style, so nothing outside the theme names a
color; depth comes from `COLORTERM` then `TERM`, and palette indices 0-15 keep the
short SGR forms, which is what kept every byte molly already emitted identical. The
stack is called a *layer* stack rather than a view stack, because `struct view` was
already taken by a window onto a buffer: buffer → view → layer → widget. And
`ui_wait_key` is now the single place molly waits for input, with
`editor_read_key` made static so the compiler enforces that nothing bypasses the
loop.

### Look and feel, settled

- **Turbo Vision's structure, a modern palette.** Double-line frames, titles and
  drop shadows — the chrome of a Borland dialog — drawn in restrained modern
  colors rather than blue-on-cyan. The shadow is the real giveaway and costs
  almost nothing: it is the cells underneath redrawn in a dim role, text intact.
- **Dialogs and menus paint their own background** from the theme rather than
  inheriting the terminal's. They read as panels floating above the text, and
  molly looks the same wherever it runs, which was rather olly's point.
- **Menu titles: File, Edit, Search, View, Go, Tools, Options, Help.** Those eight
  letters (`F E S V G T O H`) are reserved on Alt; every other Alt-letter stays
  free. Menus list only commands that exist — thin menus beat placeholders that
  do nothing.
- **The command palette is `Ctrl-Space`.** It arrives as NUL, which is what
  `CTRL_KEY(' ')` already produces, so it needs a spelling in the chord parser and
  nothing else.

### 4. The widget toolkit — **done**

Only what the ported UIs and the menus actually need, which is less than it looks:

- **Frame** — a bordered, optionally titled box, with the shadow that makes a
  dialog read as floating.
- **Label**, **button** — static text and something to press.
- **Input field** — an editable line with a cursor, which is `editor_prompt`'s
  content minus its loop.
- **List box** — a scrollable selection, which is the open dialog's content and
  the command palette's.
- **Text pane** — scrollable read-only text, which is the help screen's content.
- **Focus and tab order** across a dialog's widgets, and a modal result
  (accepted / cancelled / which button).

Checkbox and radio group wait for the Find dialog in Phase 2 — including the
"Match case" checkbox that replaces `Ctrl-T`. (It did not replace it: the dialog's
checkbox and the key are two ways to the same setting, and keeping the key means the
setting can be changed without opening a dialog to do it.)

**Landed in Phase 1:** frame with title and drop shadow, label, list box (the
open-file dialog and the palette), text pane (the help screen) and input field (the
prompt and the palette's filter). **Landed with Phase 2's Find dialog**, as planned:
the button, the checkbox and the focus order, which is the first thing molly has
with five widgets worth moving between. The radio group is still unbuilt, because
nothing has yet needed one — a direction radio was considered for the Find dialog
and dropped, since Find Next and Find Previous already are that choice.

Two things the toolkit grew that the plan did not predict. A widget carries a
**role**, rather than every widget assuming it is inside a dialog, which is what lets
the prompt's field sit on the message row and look like the message row. And a list
may give a **role per row**, which is how the palette dims a command that cannot run;
without it a list could only be uniform, and the menu would have needed its own list.

**The prompt stays on the message line.** Turbo Vision would have made it a dialog
— "Find what: [____] [OK] [Cancel]" — but Phase 2 replaces the find and replace
prompts with exactly that dialog, checkbox and all, so framing them now means
building them twice. Porting the prompt to an input-field widget on the message
line is the part that is not wasted. It landed that way: the format string keeps its
`%s`, the field goes where the `%s` was, and the row is byte-identical to what molly
drew before for any answer that fits — so the port is provable rather than hopeful,
and what it adds is a real cursor, the editing keys a prompt never had, and
horizontal scrolling where the line used to run off the edge.

### 5. Mouse (SGR 1006) — **done**

Before the menu bar, so the menu bar is clickable the day it exists.

- Decode `ESC [ < b ; x ; y M/m` in the input layer, as press, release, drag and
  wheel, with the button and modifiers. The input layer already consumes CSI
  sequences to their terminator, so an undecoded mouse report cannot leak — this
  adds meaning rather than safety.
- In the text area: click to position, drag to select, wheel to scroll.
- Hit-testing against the view stack, so menus and dialogs get their own clicks.
- See **Decisions, E**.

### 6. Port the three UIs onto the toolkit — **done**

The step that proves the toolkit, and the one that should make the diff negative:
`editor_prompt` becomes an input field, `editor_help` a text pane, the open-file
dialog a framed list box. All three stop writing escape sequences directly, which
also removes the last reason the grid has to be told to distrust the screen.

**Interleaved with step 4, not after it.** A toolkit built with no consumer is a
toolkit nobody has checked, and the open-file dialog — frame, shadow, list box,
buttons — exercises nearly all of it. So: the grid and theme groundwork lands
first, then each widget arrives with the UI that needed it, the open dialog first
and the help screen and prompt behind it.

### 7. The menu bar — **done**

- Titles with underlined mnemonics, reserving only the eight title letters on Alt
  (§1) so every other Alt-letter stays free for commands.
- Drop-downs with the shortcut column on the right, separators, and items that
  grey themselves out through the command's `enabled` hook — which the command
  table has a slot for and nothing fills yet.
- `F10` and Alt-letter to open, arrows and mnemonics to navigate, mouse to click.
- Built from the command table, so a menu entry is a command name and nothing is
  written down twice. Menus list only what exists; placeholders that never work
  are worse than absence.
- **Landed later, a drop-down that handles its own height** (2026-10-10). The box
  is measured against the *text area* rather than the grid, with the row after it
  reserved for its drop shadow, so neither the box nor the shadow can reach the
  status bar; what does not fit is scrolled, with an arrow in the border at each
  end that has more behind it and a window that moves only as far as it must to
  keep the highlighted item in view. The two sums are pure and unit-tested.
  Before it, a menu taller than the screen lost its tail off the bottom -- where
  the keyboard could still select an item nobody could see -- and that is what
  had kept three commands out of two menus.

### 8. The command palette — **done**

Nearly free once the table and the list box exist, and the answer to reaching the
long tail without memorizing it: one key, fuzzy-match command labels, show the
bound chord beside each. It was as cheap as predicted; the only part with any real
content is the matcher, and the only real decision was what to do about commands
that cannot run — **listed and dimmed, not hidden**, because a palette that silently
drops Paste when the clipboard is empty looks broken rather than careful.

Three details worth recording. `Ctrl-Space` is bound **and so is `Alt-X`**, because
not every terminal sends NUL for `Ctrl-Space` and `Alt-X` has meant "run a command by
name" for forty years; X is not one of the letters the menu bar reserves. The
matcher runs **two passes** and takes the better — matching each character's first
occurrence ranks "ln" no better in *Line numbers* than in *Line end*, and preferring
the one that starts a word fixes that but can walk past the only match there was, so
neither pass alone is right. And the filter searches each command's **group** as well
as its label and name, which made `struct command`'s `group` field load-bearing: it
was written down for the help screen, the help screen takes its headings from its own
table, and until now nothing read it at all.

### What Phase 1 cost, and what it found

The phase was predicted to *delete* code, on the grounds that three hand-rolled
full-screen UIs would shrink once they sat on a toolkit. **That prediction was
wrong, and not marginally**: every port added lines rather than removing them — +24
for the open dialog, +7 for the help screen, +91 for the prompt, on top of the ~175
the reusable input field cost in `widget.c`. The escape sequences and the manual
cursor arithmetic did go, which was the point; what replaced them is layout code
that is longer and says what it means, plus a comment explaining each decision. The
toolkit pays for itself in what comes *next* — the palette is 382 lines of which
only 246 are code, and a third of those are its matcher, because the list box, the
field and the frame already existed — not in what it replaced.

Seven bugs the phase surfaced, each of which is the sort that only appears once
something new sits on old foundations:

- **A resize while a dialog was open laid the dialog out against the previous
  size**, because the grid only learns about a resize when a refresh begins. A help
  dialog centred for 100 columns and drawn into 40 put its title off the edge and
  left it there. Fixed with `screen_sync_size()`, which every modal loop now calls
  before it measures anything.
- **`V.screenrows` did not account for the row above the text area**, so the status
  and message bars fell off the bottom of the grid the moment the menu bar existed.
- **A zero-height layer still drew itself**, which is how the hidden menu band kept
  painting over the first line of the file. Hit-testing already skipped empty
  layers; drawing now agrees with it.
- **Wheel scrolling was undone every frame**, because the view was welded to the
  cursor. `editor_scroll` now only re-attaches when the cursor itself moved.
- **The golden snapshots depended on the ambient `COLORTERM`**, so they came out
  sixteen-colour on one machine and 256-colour on the next. Pinned in the harness,
  with a snapshot at each depth so neither rots.
- **A prompt on a terminal narrower than its own hint drew the hint over the
  prompt**, leaving a row reading " (ESC to cancel, Ent" with no sign of what was
  being asked. Found by probing the new field at twenty columns, which is the width
  nothing had been tried at before. The hint is now dropped rather than squeezed,
  because the answer matters more than the advice about it does.
- **A molly killed by a signal left mouse reporting on**, so the next click in that
  window typed an escape sequence at the shell prompt. The atexit path had always
  turned it off; the fatal-signal handler had not, because the handler is Phase 0
  code and the mouse is Phase 1's — which is exactly the shape of bug that gets
  past a test suite organised by feature. Found by asking what a user is actually
  left with after a crash, which is the question `test_signals` was written to
  answer and had only half answered.

And one claim in this document was wrong until it was built: the roadmap said the
command table "has a slot for" an `enabled` hook that "nothing fills yet". It had no
such slot. `enabled` and `checked` both exist now, and ten commands fill them.

### Decisions, settled

**A. Modal loops that pump the event loop, or a focus stack?** The one that
changes the most code.

A *modal loop* is what molly has: `editor_find` calls `editor_prompt`, gets a
string back, and carries on. Keeping that shape and only changing what it waits on
means search, replace, go-to-line, Save As, the recovery prompt and the dirty-quit
countdown all stay as they read now, and background sources still get serviced
while a dialog is open. The costs are nested loops and that a dialog cannot be
left open while you do something else.

A *focus stack* has one loop; a dialog is a view that happens to be taking the
keys. Nothing nests, and non-modal UI becomes possible. The cost is that every
`result = prompt(...)` becomes a continuation, and incremental search — which
feeds the prompt a callback per keystroke — is the awkward one. That is a rewrite
of most of search.c, replace, and the save flow.

**Decided: modal loops that pump the event loop.** It gets the property
that actually matters, which is that a language server or a terminal keeps running
while a dialog is open, for a fraction of the churn. The one thing it cannot do is
a *non-modal* overlay, and LSP completion is exactly that — a popup while you keep
typing in the buffer. That works anyway, because a completion popup is a view that
draws without taking input, which the view stack already provides; it is only a
dialog that needs to own the keys. If a later feature genuinely needs a
backgroundable dialog, that is the point to reconsider.

**B. Is the menu bar always visible? Decided: it is an option, visible by
default.** Visible by default because the menu bar is molly's main
discoverability affordance and hiding it by default would waste it; optional
because it costs a text row permanently (`screenrows` goes from `wsrows - 2` to
`wsrows - 3`) and on a 24-row terminal that row is worth something.

Two consequences, neither a problem:

- **When hidden, an activated menu bar draws *over* the top text row rather than
  reserving one.** Reflowing the text area every time a menu opens would jump the
  whole screen by a row, and the view stack makes an overlay the easier of the two
  anyway. So hidden mode is genuinely zero-cost until a menu is open.
- **The toggle ships as a command before it is a setting.** Configuration is a
  Phase 2 item, so `view.menu-bar` lands as a command in the View menu with the
  rest of Phase 1 — exactly as `Ctrl-U` already toggles the line-number gutter —
  and reads its default from the config file once that exists. Nothing about the
  option has to wait for the settings file.

Changing the row count at runtime is already a supported path: the grid is
reallocated and fully repainted on every terminal resize, and a menu bar appearing
is the same event with a different cause.

**C. Do the ported UIs get reframed, or keep their current look?** The open-file
dialog is full-screen today; as a toolkit dialog it would be a centered framed box
with a shadow. Reframing is the visible half of looking like Fresh, and it means
the golden snapshots for those screens change — deliberately, with the diff read
rather than waved through. Keeping the current look makes step 6 a pure
refactor with the snapshots unchanged, and defers the appearance change.
**Decided: reframe.** Done as two commits — the port keeping the current look,
then the reframe — so that "the toolkit produces the identical screen" and "the
screen changes on purpose" stay two reviewable facts rather than one tangled one.
The second commit is where the golden snapshots for those screens change, with the
diff read rather than waved through.

**D. How many colors?** Sixteen ANSI colors work on anything, including whatever
is on the far end of an SSH session into a switch. 256 gives dialogs that look
like Fresh's rather than like 1993. Because the palette is semantic, the depth is
a property of the theme and not of the drawing code, so this is reversible — but
the first theme has to pick one. **Decided: author the palette at 256
colors with a 16-color fallback chosen from `$COLORTERM`/`$TERM`**, and keep a
golden screen at each depth so neither rots.

**E. Is the mouse on by default?** Turning on SGR 1006 tracking takes drag-select
away from the terminal: the user's reflex of swiping text and middle-clicking it
elsewhere stops working, because molly receives those events instead. Most
terminals still bypass with Shift held, but not all. **Decided: on by
default**, since a menu bar you cannot click is the thing we are trying not to
build, with a config key to turn it off once configuration exists in Phase 2 — and
the Shift bypass mentioned in the help screen.

---

## 5. The feature set, with the arguments

Verdicts: **IN** = planned, **LATER** = after the core lands, **NO** = declined
on principle, **CALL** = needs your decision.

### Cheap and clearly worth it — IN, no real argument against

Each is small *given Phases 0-1*, and most are one menu entry plus a function.
Grouped by where they live:

- **Edit:** select all / word / line, delete word back/forward, delete to end of
  line, transpose, duplicate line, move line up/down, join lines, toggle
  comment, case conversion, sort selected lines, trim trailing whitespace.
  Indent/dedent with Tab/Shift-Tab is **done**.
- **Smart editing:** smart Home (first non-blank ↔ column 0), auto-close
  brackets and quotes, surround selection — all configurable, because each one
  annoys somebody. Auto-indent on Enter is **done**, with the tab width and
  tabs-versus-spaces, and each of the three is answerable per filetype. Smart
  Backspace (eat one indent level) is **done** and is not configurable: it only
  acts where more than one character stands between the cursor and the stop, so
  in a tab-indented file there is nothing to turn off.
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

#### The engine — **done**

`src/regex.c`, 1,143 lines. It takes a pointer and a length rather than a row, so
it is testable without a terminal and reusable on a selection or a filename
later. Everything in the scope above is in it, plus `{,n}` for `{0,n}` because
both Python and PCRE2 read it that way. Out: lookaround, backreferences inside
the pattern, named groups, possessive quantifiers, `\x` escapes and POSIX
`[:alpha:]` classes.

**What the fuzz found.** `tests/fuzz_regex.py` compares the engine against
Python's `re` on random patterns — 800,000 of them during development, 4,000 per
`make test` — in bytes mode, which is the only mode whose offsets are byte
offsets and whose `\w` and `IGNORECASE` are ASCII-only. It found two real bugs
within minutes of existing:

- **`{m,n}` was built by copying the subtree** m times and adding n−m optional
  copies, which is the obvious way and is wrong: a repetition has a rule that a
  row of optional copies cannot express, which is that an iteration matching
  nothing ends the repetition instead of going round again. Rebuilt as a counted
  loop, which is both correct and smaller — `rx_clone` and the expansion went
  away, and `a{1000}` is now one node rather than a thousand.
- **`{,3}` was being read as a literal brace**, where both Python and PCRE2 read
  it as `{0,3}`.

**Two corners have no right answer**, because Perl and Python disagree with each
other, and a differential test cannot arbitrate what the references dispute.
Molly follows Perl on which iteration's capture survives when a repetition's body
is lazy, and Python on whether a capture inside a repeated group is cleared at
every iteration (it is not — clearing it would cost a save and restore per
iteration in the matcher's hot path). Both are pinned in `tests/test_units.c`
rather than left to drift, and the fuzz allows a difference only where molly's
answer passes a check that does not depend on either engine's taste: the two must
agree that there is a match and on where it starts, since neither is a matter of
search order, and molly's extent has to be a real match of the pattern. That
exemption is used about twice per 100,000 cases and the run fails if it is needed
more.

**What keeps it from hanging**, measured rather than assumed:

| Guard | Value | Why that value |
| --- | --- | --- |
| Step budget | 2,000,000 node visits per call | 9 ms on this machine for the worst case, which is the ceiling on how long one row can take |
| Recursion depth | 10,000 frames | measured at exactly 160 bytes of C stack per unit of depth, so a 1.6 MB ceiling — a small fraction of a Linux main thread's 8 MB, and sized with a Windows main thread's 1 MB in mind |
| Required characters | up to 4 | a character every match must contain, looked for with `memchr` before the scan runs |

The last one is not a micro-optimisation: without it, `.*zzz` against a
200 KB line spends the entire step budget and then reports "pattern too complex"
for a line that simply contains no `z`. With it, the same search answers
correctly in under a tenth of a millisecond. A quantified single character
(`.*`, `[0-9]+`) is also walked rather than recursed down, which is what keeps
`^a*$` over 60,000 characters from needing 60,000 stack frames.

What the depth limit still costs: a repeated *group* does recurse once per
iteration, so `^(ab)*$` matches over a thousand iterations and gives up over
twenty thousand. Lifting that means replacing C recursion with an explicit
backtrack stack, which is a rewrite of the matcher and is not worth it until
something needs it.

#### The Find dialog — **done**

`Ctrl-F` opened a prompt on the message row, which can ask one thing at a time: the
term, and nothing about how to look for it. The two settings that decide what the
term *means* lived on `Ctrl-T` and `Alt-R`, where you had to know they were. The
dialog puts all three in one place, which is most of the argument for having built a
toolkit at all — and it brought the toolkit's last three pieces with it: a button, a
checkbox, and a focus order.

Decisions inside it, each the sort that only comes up once there is more than one
thing on a screen:

- **Focus is a role, not a marker.** Whichever control has the keyboard is drawn in
  `ROLE_FOCUS`; the glyphs carry the state (`[ ]` against `[✓]`) and the colour
  carries the focus, so neither has to be read to understand the other.
- **The cursor belongs to the focused field only.** `widget_input_draw` used to
  claim it unconditionally, which was right when the only field in molly was the
  whole of the prompt. In a dialog it meant the cursor sat in the field while the
  highlight was on a checkbox, and two controls each claimed to have the keyboard.
- **Enter presses the default button from anywhere**, which is what lets "type the
  term, press Enter" keep working without the field knowing a button exists.
- **The settings are committed by Find, not by ticking.** Cancelling changes
  nothing, and `Ctrl-T` and `Alt-R` stay as the way to change a mode without
  searching.
- **The field starts empty** rather than pre-filled with the last term. A dialog
  that pre-fills normally selects what it pre-filled so that typing replaces it;
  molly's field has no selection yet, so a pre-filled one would have to be cleared
  by hand. Empty keeps the prompt's bargain — Enter on an empty field repeats the
  last search — and the hint row names the term it would repeat, which is more than
  the prompt ever did.

Two bugs it found, both about what happens when a dialog does not fit:

- **A widget whose row was outside the content area drew anyway**, over the bottom
  border and past it, which is what a six-row terminal got. `dialog_draw` now skips
  one, and the Find dialog drops the hint and then a checkbox rather than letting
  them be dropped for it, so the buttons keep the last row.
- **A button was the one widget whose width came from its label** rather than from
  the caller, so it was the one that could be wider than the room for it: at twenty
  columns the pair ran through the right frame. Buttons are clipped now, and the
  dialog shows Find alone when the pair will not fit, since Esc cancels and a Cancel
  button sawn off at the frame is worse than none.

And one thing the input layer was missing: **Shift-Tab**, which arrives as a
sequence of its own rather than as Tab with a modifier, and which fell through to
Esc — so going backwards through the focus order closed the dialog.

#### Find and replace, on patterns — **done**

Regex is a *mode*, not a second set of commands: `Alt-R` switches it, the Search
menu ticks it, `search.regex` is the name a config file will set, and Ctrl-F,
Ctrl-N, Ctrl-P and Ctrl-R all read the query as a pattern while it is on. The
prompt says `Pattern:` instead of `Search:` so the mode is visible where it
matters. `\1` to `\9` and `\0` work in a replacement.

**Both modes match the row as it is stored.** A pattern never matched anything
else; a plain search matched the tab-expanded render buffer, inherited from olly,
which is why eight spaces used to find a tab. Both are the row's own bytes now
(§7), so a match offset *is* a cursor position with no render mapping in between, and a match length needs no conversion before a replace
deletes it — `editor_row_cx_to_rbyte` and `editor_render_byte_to_cx` existed for
nothing else and went with it.

**Three things only a pattern can do to a sweep**, each of which needed its own
handling:

- **A pattern can match nothing.** `x*` matches between every pair of
  characters, so a replace-all has to step over a character after an empty match
  or it inserts at the same place for ever — the same failure the replace-all
  fuzz was written for, arriving by a new route. Replacing `x*` with `-` in `abc`
  gives `-a-b-c-`, which is what sed does.
- **Searching backward cannot run backward.** The engine answers leftmost, so
  "the last match starting at or before here" is found by enumerating forward and
  keeping the last one — stepping one character past each match's *start* rather
  than its end, so that two overlapping matches are both seen.
- **A sweep can be told "I don't know".** The step budget running out is not
  "not found", and a sweep that read it as "not found" would go on to ask the
  same impossible question of every remaining row in the file. It stops, says
  `Pattern too complex. Try a simpler one`, and leaves the cursor alone; a
  replace-all that gets partway says how many it managed, and one Ctrl-Z undoes
  all of them.

Half of `tests/fuzz_replace.py` now runs in pattern mode against `re.sub`
applied line by line, which is what checks the empty-match rule across a
multi-row buffer: `b?` against a six-line file is 57 matches and both engines
agree on every one. Its termination witness is unchanged and still the point —
molly runs under `ulimit -v` and has to reach a clean quit.

### Multiple buffers and tabs — **done**

- **For:** the most-missed thing about olly, and mostly bookkeeping once Phase 0
  separates buffer from view. Prerequisite for File—Close, the file explorer,
  and grep results.
- **Against:** every "are you sure?" path multiplies — unsaved changes on quit
  becomes a list, not a question. Crash recovery has to cover N buffers.
- **Landed:** the buffer list, `File ▸ New`/`Close`, `Ctrl-PgUp`/`Ctrl-PgDn`,
  several files from the command line, opening into a buffer of its own, a
  remembered cursor position per buffer, and the `[2/3]` counter in the status
  bar. It *was* mostly bookkeeping, for one reason worth recording: making `B` a
  macro for `(*V.buf)` meant nearly two hundred existing references to the one
  buffer kept working unchanged, and that definition — the current buffer is
  the current view's — is the one splits need anyway.
- **Both costs came true.** Crash recovery writes a file per unsaved buffer now,
  and quit counts them all. The quit warning is a *message* with a count rather
  than the list in a dialog this section asks for; that wants a dialog with
  Save All / Discard / Cancel on it, which is a feature rather than a message.
- **The tab bar landed too:** one tab per buffer under the menu bar, with the
  file's name rather than its path, an asterisk for unsaved, click and wheel to
  switch, and a scrolled window with arrows when there are more tabs than fit.
  It takes a row only when there is a second buffer, and `[view] tab bar = off`
  declines it. Scrolled rather than squeezed on purpose: tabs cut to three
  characters are harder to read than tabs you have to scroll past.
- **Still to come:** a buffer-list dialog for picking one by name rather than by
  walking the list, which is also what the quit-with-unsaved-work dialog wants
  to be built on.

### Window splits — **done**

- **For:** the other half of what a GUI-like editor means. Free-ish after the
  cell grid and `struct view`.
- **Against:** the gutter, horizontal scroll and wide-character alignment all
  measure against `TEXT_COLS()` (molly.c:112), which reads globals; every one of
  those needs to become per-view. Expect a long tail of off-by-one bugs, which
  is exactly what the golden-screen harness is for.
- **Landed:** a binary tree of windows, `F7`/`F8` to split right and below,
  `F6`/`Shift-F6` to walk them, `F9` to close one, a Window menu, and a status
  bar per window. Each window keeps its own cursor, scroll offset and gutter, so
  two windows on one buffer are two views of one text.
- **The against was wrong about which globals.** All three things this section
  named measure against `TEXT_COLS()`, which reads `V` — and `V` became a macro
  for the focused view, exactly as `B` had become a macro for the current view's
  buffer, so all three were right without being touched. What was actually
  per-view state in disguise was not on the list: view.c kept "has the cursor
  moved since the last scroll" in two file-scope statics, so scrolling one
  window would have told the next one its own cursor had moved. The menu and tab
  bar flags were the opposite error — fields on the view, although the rows they
  take are above every window, so which window had focus would have decided
  whether the chrome existed at all.
- **The long tail of off-by-one was real and the goldens caught it**, which is
  what this section said they were for. Sixteen new snapshots: both colour
  depths, the gutter in half a window, wide characters in half a window, one
  file per window under a tab bar, a narrow screen, one too narrow to split, and
  a resize.
- **One window is byte-identical to before splits existed.** That is a design
  decision rather than a happy accident: each window's last row is its own
  status bar, so with one window the bar lands exactly where the screen-wide one
  used to be and all seventy existing snapshots went on matching through the
  mechanism commit.
- **A tree, not a list.** A flat list of windows would have been less code and
  would have capped molly at one row or one column of them forever; splitting
  right and then below is a node whose second child is a node, and no case in
  the layout has to know how deep it is. Splits store a *fraction* rather than a
  column count, so a resize keeps the proportion and nothing has to remember the
  size it was laid out at last time.
- **Not done:** dragging a split to resize it, and a grow/shrink command. The
  fraction is the field either would set; what is missing is the gesture and the
  chord. Named windows, window-local filetypes and a window list are not
  planned.

### Syntax highlighting — **done**

- **For:** table-driven keyword/string/comment/number rules per filetype. Kilo
  had it; olly dropped it. Visible payoff on every single keystroke of use.
- **Against:** a naive line-at-a-time lexer gets multi-line strings and comments
  wrong, so rows need a cached lexer state and invalidation on edit. That is the
  real cost, not the rules themselves.
- **Scope:** regex- or table-driven definitions in config, so adding a language
  is not a recompile. Explicitly *not* a parser per language.
- **Shipped with:** Rust, C, C++, C#, Python, AutoIt (`.au3`) and Markdown.
  Seven definitions, decided 2026-10-08. Two of them are the ones that make the
  cached per-row lexer state unavoidable rather than merely correct: AutoIt's
  `#cs`/`#ce` block comments and Markdown's fenced code blocks both nest a
  region whose end is many rows away. Markdown also means the definition format
  cannot assume keyword/string/comment rules are enough — it needs emphasis
  and headings, which are not any of the three.
- **The against came true, and the answer is laziness.** Each row stores the
  lexer state it was entered and left with, and the buffer stores how far up the
  file those are known to be right. An edit lowers that watermark and stops; the
  renderer relexes just before drawing. Eager invalidation would relex a hundred
  thousand rows on the keystroke that opens a comment at the top of a file, for
  the sake of rows nobody is looking at.
- **Definitions are text, and the built-ins go through the same door.**
  `[syntax lua]` in `mollyrc` defines a language; `[syntax c]` adds to one molly
  ships. Both reach syntax_set_line(), which is also how the built-ins are
  loaded — the same arrangement the keymap has, so the path a user's first
  definition takes is exercised on every start.
- **Not done, and not planned:** a shebang line to type an extensionless
  script, and italic for comments. Italic would mean a fourth cell modifier,
  the SGR for it, and the snapshot harness learning to read it; the colour is
  doing the work for now.

### Indentation, filetypes and configuration — **done**

- **For:** tabs vs spaces, tab width, auto-indent, per-filetype defaults. Olly's
  "no configuration" was its most limiting rule, and a menu-driven editor needs
  an Options menu with dialogs that persist — so config is required, not
  optional. `~/.config/molly/mollyrc`.
- **Against:** the moment config exists, every bug report needs a config file
  attached, and "molly behaves the same on every machine" is over. Mitigation:
  ship defaults that match today's behavior exactly, and make the file
  machine-writable from the Options dialogs rather than hand-edited.
- **Landed:** the file, the parser, the settings table, write-back and the
  `[keys]` section. Seven settings at the time — tab width, undo steps, line
  numbers, menu bar, mouse, match case, regular expression — each with its
  default in the table and nowhere else, which is what made the menu bar and
  the mouse stop declaring theirs twice. The test harness pins `MOLLY_CONFIG`
  empty so the suite does not read the developer's own file.
- **Landed since:** auto-indent, tabs-versus-spaces and per-filetype defaults.
  Thirteen settings now, the tab bar, syntax and the filter's two limits
  having arrived with their own features in between. The three indentation
  ones are the first that a filetype can answer for itself, and the answer
  lives in the `[syntax <name>]` block rather than in a section of its own —
  that block already names the language, and a second place to name it would
  be a second place for the two to disagree. `[editor]` holds each default,
  `[syntax <name>]` may override it, and `indent.c` reconciles the two.
- **What that cost:** the tab width could no longer be one global. It is a
  function of the filetype, and two windows can show two filetypes in the same
  frame, so a single "current width" had each window re-rendering a whole file
  to undo the other's work. The width a buffer's rows were built at is now a
  field on the buffer, reconciled by `indent_sync()` — once per window per
  frame, and a no-op unless something moved.
- **Landed after it:** indenting and dedenting a block with `Tab`/`Shift-Tab`,
  and Backspace eating one level. Two answers the references do not settle, both
  pinned in the suites: a blank row inside a block is left alone, because an
  indent on nothing is trailing whitespace; and a dedent goes back to the
  previous tab stop, taking whole characters off the end of the whitespace run
  nearest the text — so six spaces at a width of four leave four, and a tab that
  straddles the stop comes off whole and undershoots.
- **In the Edit menu since 2026-10-10**, in a group of their own between the
  commands that work on a selection and the filter. They were out of it for a
  while on arithmetic rather than taste -- two more items plus a separator put
  that drop-down's shadow on the status bar of a sixteen-row screen -- and what
  let them in was the drop-down learning to scroll and to measure itself against
  the text area. The menu is molly's tallest at sixteen rows, which is now a
  question of how much of it a short terminal shows at once.
- **Still to come:** the Options dialogs, which are what write-back was built
  for. Nothing here knows a language: the new line copies the old line's
  indentation and does not add a level after a brace, and nothing works out what
  level a block *should* be at.

### Shell filter — **done**

- **For:** pipe the selection through a command, replace it with the output.
  Roughly a hundred lines for an unbounded amount of capability — `sort`, `jq`,
  `fmt`, `black`, anything. The best value-per-line item on the list after
  regex. Fresh binds Alt-|.
- **Against:** needs care around a command that hangs or floods output
  (timeout + cancel key + output cap), and it is the first place molly executes
  something on the user's behalf.
- **Landed:** `Alt-|`, `Edit ▸ Filter through…`, the selection or the whole
  buffer when nothing is selected, through `/bin/sh` so a pipeline works, as one
  undo step. `Enter` on an empty prompt repeats the last command. `[shell]
  timeout` and `[shell] output limit` are the two limits, in seconds and
  kilobytes.
- **The against was the whole job.** The feature is about forty lines; the care
  is three hundred. All three guards asked for are there — and a fourth that was
  not on the list and is the one that actually matters: **deadlock.** A command
  whose output outgrows a pipe buffer while molly is still feeding it stops
  reading, and molly's write blocks for ever. That is why `loop_add()` takes an
  events mask now: the input pipe is a writable source and the output pipe a
  readable one, and whichever is ready moves first. A filter that wrote all its
  input before reading any output would have looked fine on every small file.
- **A fifth, found by writing it:** the reaping cannot block either. A command
  that closes its output and then sleeps is past every one of the limits — they
  all watch the pipes — and a blocking `waitpid` on it hangs the editor. So the
  wait is `WNOHANG`, with SIGTERM, a grace period and then SIGKILL, to the
  process *group*, so a pipeline goes with the shell that started it.
- **Standard error is a pipe too**, which was not on the list and is not
  optional: a mistyped command would otherwise paint `sh: jqq: not found` into
  the middle of the text. Its first line is kept and its control bytes defanged,
  because "sh: jqq-does-not-exist: command not found" tells the user what to fix
  and "exited 127" does not.
- **A non-zero exit does not replace anything.** A filter means "replace my text
  with this", and a command that failed has produced nothing or half of
  something. Undo would get the text back; not losing it is better. The cost,
  accepted: `grep`, which exits 1 when it matches nothing, cannot be used to
  empty a selection.
- **Undo groups nest now**, because they had to. "Replace this selection with
  that text" is a delete group plus a run of inserts, which came out as two undo
  steps — so a filter could not have been one, and pasting over a selection had
  quietly been two all along, which made README's claim about it true only when
  nothing was selected.
- **`sh -c`, not a split-and-exec.** A user typing into a "filter through"
  prompt expects a pipeline, a redirect and a glob to work, and the command is
  their own with exactly their shell's privileges. Molly runs nothing the user
  has not typed.

### Soft wrap — **done**

- **For:** people expect it; prose and Markdown are unusable without it.
- **Against:** it breaks the one-row-equals-one-line assumption that cursor
  movement, scrolling, selection, the gutter and the renderer all rest on. In
  my estimation this is the second most invasive feature on the list, behind
  multi-cursor. Done late it is a refit of everything; done in Phase 0 — by
  making the view address *display rows*, not file rows, from the start — it is
  much cheaper.
- **Landed in Phase 0, the model:** the view addresses *display rows* rather
  than file rows. The scroll offset, the draw loop, the row renderer and
  vertical movement all speak display rows, with the unwrapped mapping being the
  identity. A slice is expressed in *render columns*, not byte offsets, because
  wrapping happens where glyphs land on screen.
- **Landed, the feature:** `Alt-Z`, the palette, the help screen, and
  `soft wrap` in `[view]`. The View menu came a few commits later the same day,
  once a drop-down could handle its own height. A line breaks after the last space that fits; the gutter numbers it
  once and leaves the continuations' number field blank; `↑`/`↓` walk the rows
  of a long line; a click on a continuation row lands where it looks; and
  nothing about the file changes, so a save writes the bytes that were there.
- **And the bet paid.** `src/wrap.c` is the whole of it: the three functions the
  groundwork promised, plus the column the terminal's own cursor goes in (which
  the old code took straight from `CUR.rx`, and which is no longer the same
  number), the vertical step, and the one line that drops a row's measurement.
  The step lives here rather than in `view.c` so that it can stay local to the
  row and its neighbour — an arrow key must not get slower because the file is
  long, and everything else in the file walks the buffer. Four call sites
  outside it changed: vertical movement, the horizontal scroll (which wrap
  turns off, since the two are alternatives), the gutter's number, and the
  click-to-position arithmetic. The draw loop, the row renderer, the selection,
  the multi-cursor marks and every edit path were already asking the right
  questions and did not move — none of the 93 golden screens changed while the
  mechanism landed.
- **What it costs, measured.** With wrap off every function in `wrap.c` returns
  on its first line, so the frame is what it was. With wrap on a frame is a walk
  of the file's rows: 0.03 ms at 10,000 rows and 0.24 ms at 100,000, with every
  row wrapping into two. A row caches its own slice count, and `drow_at` keeps
  one step of its walk so that a screenful costs one pass rather than one per
  row — without that last part the same frame was 2.9 ms. The remaining walk is
  the flat `erow[]` array's to lose: see big-file handling below.

### Multi-cursor — **done.** Block selection — **LATER**

- **For:** the headline feature of modern editors, and Fresh has it (Ctrl-D).
  Once you have it you use it constantly.
- **Against:** the most invasive item here. Every edit path, every undo group and
  the entire selection model change: one cursor and one anchor become N of each,
  edits must apply without invalidating each other's positions, and undo has to
  collapse N simultaneous edits into one step.
- **Landed, the model:** the cursor is `struct cursor` (position + its own
  selection anchor) held in a list, with a `CUR` macro for the primary.
  `cursors_clamp()` runs from `editor_scroll`, so every cursor is valid before
  each frame regardless of which edit path ran.
- **Landed, the feature:** Ctrl-D for the word and then each next occurrence,
  Ctrl-Up/Ctrl-Down for a column, Alt-click for anywhere, Esc for one again.
  Typing, Tab, Enter, Backspace, Delete and paste reach every cursor; so do the
  arrows, Home and End, and Shift extends a selection at each one from where
  that cursor was.
- **What it cost, which was less than the warning above:** not one editing path
  was rewritten. Two things did that. Cursors run in document order, so every
  edit is strictly earlier than every cursor still to come and nothing an edit
  does reaches text before it -- which makes a processed cursor final. Then one
  rule moves the rest: every operation is a *replace*, so with (r1,c1) the end
  of the removed range and (nr,nc) the cursor afterwards, a position after it
  goes to (nr, nc + (col-c1)) on row r1 or down by (nr-r1) below. Deleting
  forward is the only operation whose removed range ends anywhere but the
  cursor it started at.
- **And undo needed nothing at all.** `undo_group_begin()` already nested, so
  the group opened around the whole gesture swallows the ones `selection_delete`
  and the newline open, every record carries one group id, and `editor_undo()`
  already popped a group at a time. Undo then collapses to one cursor, because a
  record restores one position rather than a list -- the answer
  `cursors_collapse()` already gave for loading another file.
- **Landed after it:** Alt-D to skip a match, Alt-U to take the last cursor
  back, Alt-A for every match at once. The chords are what a terminal can
  carry — the editors with these use `Ctrl-K Ctrl-D` and molly has no two-chord
  sequences, and VS Code's `Ctrl-U` went to the line-number gutter here long
  ago.
- **What they needed was an order.** The list is kept in document order, so
  after a search wraps to the top of the file the newest cursor sorts first and
  "the last one" cannot be recovered from the positions. A cursor now carries
  the order it was added in. That fixed Ctrl-D as well: it had searched from the
  last cursor in document order, which after a wrap is not the one the user is
  looking at, so the next press walked down from a match already taken.
- **Block selection is still later.** A column of cursors covers most of what
  people use it for, and a rectangular *selection* is a different model again:
  every selection in molly is a pair of positions, and a rectangle is not.

### Search in files / grep — **LATER**

- **For:** big win; can shell out to `grep` or `git grep` for zero dependency.
- **Against:** needs a results panel, so it was gated on splits and buffers —
  both of which now exist, and a results panel is a window showing a buffer of
  matches. Shelling out means depending on the host's tools; implementing it
  internally means a recursive walk plus ignore-file handling.

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
  syntax rules and the shell filter — all four of which now exist. That covers a
  large fraction of what people write plugins for, with no engine.

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
- **Soft wrap left it a second question to answer.** "Which file row is display
  row N" is a walk of the rows while the rows are a flat array, and `wrap.c`
  pays it once per frame rather than once per screen line. A piece table can
  carry a count per node and answer it in a step; a flat array cannot, and no
  cache over the flat array would survive an edit. So the number in `wrap.c` is
  the bound to beat, not a target to optimise where it is.

---

## 6. Keymap: CUA bindings, fully customizable

Fresh is CUA-style, and olly already agrees on the important ones (`Ctrl-S/O/Q/
F/R/G/Z/Y/C/X/V`, Shift-arrow selection). Four genuine conflicts:

| Key | olly today | Fresh | Suggested resolution |
| --- | --- | --- | --- |
| `Ctrl-L` | repaint | select current line | Fresh wins; repaint → View menu / `F5` (see below) |
| `Ctrl-T` | toggle case-sensitive search | transpose characters | Fresh wins; case becomes a **checkbox in the Find dialog**, which is better UI than a hidden toggle |
| `Ctrl-N` | find next | — (`F3`) | `F3`/`Shift-F3` primary, keep `Ctrl-N/P` as aliases |
| `Ctrl-?` | help | `F1` | `F1` primary, keep `Ctrl-?` |

Newly claimed by Fresh conventions: `Ctrl-A` select all, `Ctrl-W` select word,
`Ctrl-D` add cursor, `Ctrl-K` delete to end of line, `Ctrl-/` toggle comment.
None of these are bound in olly, so they are free.

Note that two of these resolutions *improve* the UI rather than trading it away
— a Find dialog with a visible "Match case" checkbox is strictly better than
`Ctrl-T` with a status-bar message.

**`Ctrl-Shift-L` was not available**, which this table originally assumed it
was. A terminal sends the same single byte for `Ctrl-Shift-L` as it does for
`Ctrl-L`: the modifier parameter that carries Shift only rides on the CSI forms,
which is the function keys and the navigation keys. So repaint went to `F5`
instead — what every browser and file manager has used for "refresh" since the
nineties — and keeps its View menu entry.

The same limit decides where Shift can appear in a chord at all. `shift-f3` and
`shift-right` are bindable; `shift-a` and `ctrl-shift-l` are refused at parse
time rather than accepted and silently never fired. This is also why Shift stays
out of band for movement: a shifted *movement* extends the selection, which is
true of every movement at once rather than being a binding, so the dispatcher
tries `key | KEY_SHIFT` first and the bare key second.

Of the chords Fresh newly claims, `Ctrl-A` (select all) is bound. `Ctrl-K`
(delete to end of line) and `Ctrl-/` (toggle comment) need commands that do not
exist yet, and a chord bound before there is something to bind it to is how a
keymap acquires entries nobody can explain.

**`Ctrl-W` went to Close rather than to select-word**, which is a deliberate
departure from the list above. Close-tab is what `Ctrl-W` means in every editor
and browser with tabs; select-word does not exist and has no date; and the cost
of being wrong is one line of `mollyrc`. `Ctrl-N` likewise went to New, as CUA
has it — it was only ever an alias for find-next, and `F3` is the primary now.

`Ctrl` also became nameable in a chord, on the same keys as `Shift` and for the
same reason: the modifier rides in the CSI parameter, so `Ctrl-PgUp` arrives as
itself while `Ctrl-Shift-L` cannot. Without it `Ctrl-PgUp` arrived as a plain
`PgUp` and scrolled a page.

**Every binding is remappable**, and that is a requirement rather than a feature,
because it decides the shape of the keymap code. It means:

- No keypress may be handled by a hardcoded `switch`. The table maps a *chord* to
  a *command name*, and the dispatcher never names an action directly. This is
  the same table the menu bar, the command palette and the help screen read, so
  there is exactly one place a binding exists.
- A chord needs a real representation — modifiers plus a key, including `F1`-`F12`,
  Alt-letter, and multi-key sequences like `Ctrl-K Ctrl-F` — with a parser and a
  printer, because the config file writes them as text (`shift-f3`) and the
  menus display them. **Done, except the multi-key sequences**, which would make
  a chord a list rather than an int; nothing needs one yet.
- Conflicts are data, not bugs: two commands can want one chord, so binding
  resolution needs a defined precedence (user config beats the built-in keymap)
  and the keybinding dialog has to show and resolve collisions.
- Named keymaps, so an olly-compatible set can ship alongside the CUA default
  and olly muscle memory is a config line rather than a fork. **Not done**, and
  less urgent than it looked: `[keys]` in `mollyrc` makes olly's four keys four
  lines, so a named set is a convenience rather than the only way back.

The one thing *not* remappable is the Alt-letter menu titles -- seven today, one
per menu, read off the menu table rather than written down -- since those are
structural to the menu bar.

---

## 7. Decisions made

| Question | Decision |
| --- | --- |
| Command access | Ctrl **and** Alt keys, plus a menu system on Alt |
| Soft wrap | **Done.** The display-row model it was planned around took it in one file |
| Where a wrapped line breaks | **After the last space that fits**, and at the window edge for a word with no space in it |
| The space it broke after | **Stays on the row above.** Slices have to cover every column of a row, or a cursor in a trimmed run would have no cell to sit in |
| The column a line breaks at | **Belongs to the row below**, so a cursor there is drawn at the start of the continuation; a step into a shorter row stops on its last column |
| What `↑`/`↓` keep | **The screen column when wrapped, the byte offset when not** — between two slices of one line the byte offset is a windowful away, and changing the unwrapped answer would move behaviour no one asked about |
| What `Home`/`End` reach | **The line, not the display row** — the file's shape is what those two keys are about |
| A wrap column | **No.** Lines wrap at the window's width; a narrower wrap is a narrower window |
| A hanging indent on continuations | **No** — a continuation starts at the left edge |
| `Ctrl-Up`/`Ctrl-Down` with wrap on | **Still file rows.** A column of cursors is about lines of code lining up in the file, and that gesture already keeps the render column rather than the byte offset |
| Whose setting wrap is | **The window's**, like the gutter and the colours: a split can wrap one half and not the other |
| Where the toggle lives | **`Alt-Z`, which is VS Code's**, plus the palette, the help screen and — since the drop-down learned its own height — **View ▸ Soft wrap**, ticked when it is on |
| What a wrapped frame costs | **A walk of the file's rows**, 0.24 ms at 100,000 of them; the row count is cached per row and the walk keeps one step of itself |
| Multi-cursor | **Done.** Cursors run in document order so one replace-range rule fixes up the rest |
| What Ctrl-D matches | **Literal text, case-sensitively**, and not through `search.c` -- the Find dialog's modes must not change what it finds |
| Cursors after an undo | **One.** A record restores a position, not a list |
| Where the multi-cursor commands live | **The palette and the help screen, not the Edit menu** -- keyboard operations, as transpose already was |
| LSP | **In.** Forces an async event loop and overlay widgets into Phase 0 |
| Integrated terminal | **In**, late — needs panels first |
| Big files | **In.** Buffer rewrite is planned, not optional |
| Keymap | CUA on the four conflicts, and **fully customizable** |
| Regex | **Hand-rolled PCRE subset**, with a step budget and a fuzz target |
| Platforms | **Linux now. Windows possibly later. No macOS.** |
| Plugin runtime | **No** — it would end zero-dependency and instant start |
| What a search matches | **The row as stored**, for plain searches as well as patterns |
| Config file syntax | **INI-style sections**, `key = value` — small to parse and to write back |
| Phase 2 order | **The Find dialog next**, which is what forces buttons, focus order and the checkbox into the toolkit |
| Initial syntax languages | **Rust, C, C++, C#, Python, AutoIt (`.au3`), Markdown** |
| Where a per-filetype setting lives | **In its `[syntax <name>]` block**, not a section of its own — the block that already names the language |
| Tabs versus spaces default | **A literal tab**, because the setting decides what bytes a save writes |
| Auto-indent default | **On**, because it changes only what a keystroke does |
| `Enter` inside leading whitespace | **The text keeps its column** — only the whitespace behind the cursor is copied |
| What a dedent takes off | **Back to the previous tab stop**, whole characters off the end of the whitespace run nearest the text; a tab that straddles the stop comes off whole |
| A blank row inside an indented block | **Left alone** — an indent on nothing is trailing whitespace |
| When `Backspace` eats a level | **Only in the leading whitespace, and only with more than one character to take** — so a tab-indented file never notices |
| Where indent and dedent live | **Edit ▸ Indent and Edit ▸ Dedent**, plus the palette, the help screen and the keymap — a group of their own between the selection commands and the filter |
| What a drop-down does with a short terminal | **Scrolls, inside the text area.** The box and its shadow stay above the status bar, an arrow in the border says there is more that way, and the window moves only as far as the highlight makes it |
| Skip a match / drop a cursor / all matches | **Alt-D, Alt-U, Alt-A** — a terminal cannot carry `Ctrl-K Ctrl-D`, and `Ctrl-U` was spent on the gutter |
| Which cursor those three work from | **The one added last**, which after a wrap is not the last one in the file — so a cursor carries the order it was added in |

The first of those changed shipped behaviour: olly searched the tab-expanded
render buffer, so a run of eight spaces found a tab. Unifying both modes on the
stored bytes means a search now means exactly the characters asked for, and it
deleted the only two functions that mapped between the two spaces. A tab is still
only nameable as `\t` in a pattern, because a prompt cannot accept a Tab
keystroke — which is also why `tests/fuzz_replace.py` puts tabs in the text it
generates but never in a query or a replacement.

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

Nothing. The last item — which languages ship with syntax definitions
initially — was answered on 2026-10-08 and is recorded in §7.
