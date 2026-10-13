# Handoff

Written 2026-10-12. Everything is committed on `main`, which is ahead of
`origin/main` until Shawn says to push; the working tree is clean and nothing
is half-finished. The last work on it was the rest of the roadmap, below. The
2026-10-10 handoff's story — the menu drop-down's height, soft wrap, and the
test harness's keystroke flake — is further down: see "What only CI sees".

This file is for whoever picks molly up next. `ROADMAP.md` is the argument and
the plan; this is the state, the order, and the things that will cost you an
hour if nobody tells you.

**Latest:** the rest of the roadmap, after Shawn said on 2026-10-12 "just
keep going - let's finish everything". In commit order after formatting:
words (Ctrl-Left/Right, Alt-Backspace, Ctrl-Delete, Ctrl-K); lines
(`src/lines.c`: Alt-C, Alt-Up/Down, Ctrl-J, sort, trim, case, Alt-;); smart
editing (`src/smart.c`: smart Home, auto-close and surround); navigation
(`f72207a`, `src/nav.c`: Ctrl-], bookmarks on Alt-J/Alt-0..9, and one jump
history for Alt-Left/Alt-Right that F12's back stack moved into); then one
batch commit of view aids (whitespace, indent guides, current line, highlight
word, ruler), recent files (`src/recent.c`, Alt-O) and read-only mode,
keyboard macros (recorded at `ui_wait_key()`), folding (`src/fold.c`, through
`wrap.c`'s display-row map), git (`src/git.c`: gutter signs, blame, file
history), suspend-to-shell and true colour; then the terminal extras (mouse
reporting via `vt_mouse()`, drag-to-copy, scrollback find, wheel-as-arrows on
the alternate screen) and the language-server extras (code actions on
Alt-Enter with resolve, `executeCommand` and `workspace/applyEdit`; format on
save; completion while typing; a server per project root). Reflow was
declined, with the reasons in `src/vt.c` and ROADMAP §5. Pty checks
`test_view_aids`, `test_files`, `test_macros`, `test_folding`, `test_git`,
`test_terminal_extras`, `test_lsp_extras`; units cover `vt_mouse()`,
`vt_pushed()`, `json_write()` and the true-colour fitting.

Chosen without asking, each a setting or a line to change: **smart home,
auto close, format on save and complete while typing are all off**, and every
view aid is off, while the git gutter is on (it costs a `git` only for
tracked files). Alt-Enter for code actions (JetBrains'; Ctrl-. cannot be
sent), and **not in a menu**, because every letter of "Code actions" is
already spent in Edit. One macro of 4,096 keys rather than ten registers.
Suspend is a palette command, since Ctrl-Z is undo. Recent files live in
`$XDG_STATE_HOME/molly/recent` as state, not configuration. The File menu now
shows its shortcuts: they were always bound, but every label was shorter than
the six columns the shortcut squeeze keeps for a label, so the column was
dropped at natural width — "Recent files…" made it wide enough.

**Before that:** formatting (Alt-Q), which Shawn picked next on 2026-10-12. The
whole file, or the selection where the server does ranges (else the file,
said so), with the file's tab width and spaces sent as the options; the edits
go through `apply_edits()`, shared with rename, which now carries the cursor's
row and skips edits that would put back what is there. Pty checks
`test_lsp_format`; checked against clangd. No format-on-save.

**Before that:** the problems list (Ctrl-F2), which Shawn asked for next on
2026-10-12. A buffer flagged `problems`, rewritten from the main loop
(`lspui_problems_refresh()`) whenever `lsp.c` says diagnostics changed
(`lspui_problems_changed()`), by borrowing the view's buffer pointer as
`buf_free` does, so no other window's cursor moves. Rows are `path:line:col:
severity: message`; each carries its own diagnostic over the severity word, so
the ordinary underline drawing colours it. Enter goes to the problem and never
inserts. Pty checks `test_lsp_problems`; goldens `lsp-problems*`.

**Before that:** the three language-server features Shawn queued after the
terminal on 2026-10-12, in his order. `dc16047` is rename (Alt-N): the
server's WorkspaceEdit applied a file at a time, last edit first, as one undo
step per file; a file that is not open is opened and left unsaved; a rename
that would move a file is refused whole. `d041369` is find references
(Shift-F12): the answer goes into the find-in-files results buffer through
`grep_show_list()`, which now takes a column per row. The signature-help
commit opens a box above the line on the server's trigger characters and asks
again after every key, with `sig_pending` so a key typed before the first
answer still asks; Alt-P asks by hand. It also fixed `json_putf`, which
formatted into 512 bytes and cut the client's capabilities short once they
grew past that -- the fake server died on the invalid JSON, which is how it
was found. Pty checks `test_lsp_rename`, `test_lsp_references`,
`test_lsp_signature`; unit `test_lsp_rename_apply`; goldens `lsp-signature*`.
All three checked against clangd. Next in Shawn's list: a problems list.

**Before that:** the integrated terminal, the first of four Shawn asked for in order
on 2026-10-12 ("the integrated terminal, then rename, find references, then
signature help", with a problems list after). Three code commits. `4204fb4`
lets a grid cell carry a colour a program chose (`grid_color`, `ATTR_RAW`),
brought down to sixteen as the frame is written. `de02e59` is `src/vt.c`, an
xterm-subset emulator that is bytes in and screen state out, 95 unit checks
plus a random-sequence fuzz. `730c5c0` is `src/tty.c`: a pty and `$SHELL`
behind a buffer, Alt-T to and from it (a split below, a third of the height),
keys to the program except the ways out (found by command), Shift-PgUp
scrollback, the size told to the program after layout, exit closing it from
the main loop, and quit/close asking while something other than the shell
runs. Commands that edit text refuse on a terminal through `cmd_run()`, which
is now where keys, the menu bar and the palette all run commands. Pty checks
`test_terminal` against a real `/bin/sh`; goldens `terminal*` against
`tests/fake_shell.sh`, a fixed screen, because dash and bash prompt
differently. Checked by hand with less, vim and htop.

**Before that:** the language server, which Shawn named on 2026-10-12, built in the
order `ROADMAP.md` already gave it: diagnostics, then definition and hover, then
completion. Four commits. `7521f49` is `src/json.c`, a tree reader and a string
writer, nothing calling it. `b897ef3` is the client, `src/lsp.c`, reachable only
from a `[syntax <lang>] language server =` line: servers found by command line
(so C and C++ share one clangd), looked for on PATH first and silent when
missing, documents synchronised by a sweep (`lsp_idle()`) on every quiet tick
rather than by hooks, the whole text on every change, UTF-8 positions offered
first and UTF-16 converted. `dc0b35e` draws diagnostics -- underlined in the
text, `E:n W:n` on the status bar, the one under the cursor on the message bar
-- walks them with F2/Shift-F2, names clangd/rust-analyzer/pylsp in the
built-in definitions, and adds `[editor] language servers`. `672f313` is F12,
Alt-Left, Alt-K and Alt-/ in `src/lspui.c`, with answers going through a
mailbox the main loop opens (`lspui_deliver()`) and only if the cursor has not
moved on. Everything is tested against `tests/fake_lsp.py`, a server whose
answers come from the text and whose failures are chosen by environment
variable; checked by hand against clangd. Unit checks `test_json`, `test_lsp`,
`test_lsp_answers`; pty `test_lsp`, `test_lsp_questions`, `test_lsp_complete`;
goldens `lsp-diagnostics`, `lsp-message`, `lsp-hover`, `lsp-complete` at both
depths, plus `lsp-narrow`, `lsp-hover-narrow`, `lsp-complete-narrow`. All of it
clean under clang's ASan+UBSan, units and pty both. The keys were chosen without
asking -- see `ROADMAP.md` §5's LSP section for each and why.

**Before that:** the big-file rewrite, which Shawn picked on 2026-10-11 and then
chose the shape of: line spans rather than a true piece table, because every
reader in molly takes a row as one contiguous string by index and a piece
table would have put a lookup and a pointer-lifetime rule under all ~250 of
them. Four commits. `c6f7f06` drops the tab-expanded `render` copy:
`text_cell()` walks a tab a column at a time, so no golden moved. `b14b93f`
lexes a row when it is drawn (`syntax_roles()`) instead of keeping a role per
byte for every row ever lexed, and packs `erow` to 24 bytes behind a
`_Static_assert`. `5228d36` is the storage: the file is read whole into
`B.text`, each row is a NUL-terminated span of it, a row is copied the first
time it changes (`row_own`), the block doubles as the saved snapshot through
`B.saved_off[]`, and the rows are a gap array (`B.gap`; `row_at`/`buf_row`
translate). The fourth makes a save compact the rows into the block in place
instead of beside it. Measured: 400,000 lines went from 3.1× to 1.48×; ten
million lines (743 MB) load in 0.42 s at 1.40× and save at 1.66×. Unit checks
`test_storage_load`, `test_storage_edit`, `test_storage_gap` (3,000 random
edits against a plain list, with a save every 97); `test_syntax_roles`; pty
`test_tab_cells`. The pty suite also passes under clang's ASan+UBSan — gcc's
`libasan` is not installed here, so use `clang`.

**Before that:** block selection, which Shawn picked on 2026-10-11. A block is a
cursor per row, each with a selection over the same display columns, so no
edit path changed: `src/block.c` keeps two corners (row, display column),
lays the cursors out from them, and calls the block live only while the
cursors are exactly what it laid out — any other command ends it by changing
them. Rows shorter than the block get a cursor at their end with an *empty*
selection, which is what stops Backspace deleting there. A tab or wide
character straddling either edge is taken whole. Copy/Cut of a live block
put its rows on the clipboard a line each and set `E.clip_block`
(`clipboard_set()` clears it); `block_paste()` then lays it down as a
rectangle with one cursor (padding short rows, appending rows past the end),
a line per cursor when the counts match, and otherwise defers to the ordinary
whole-at-every-cursor paste. Keys: `Alt-Shift`-arrows — which needed
`KEY_ALT`, a third modifier bit for navigation keys, decoded from the CSI
parameter and named in chords (`alt-shift-up`) — and `Alt`-drag, decided by
the press so letting go of Alt mid-drag does not switch modes. Chosen without
asking: those chords (Sublime/Notepad++/JetBrains; VS Code's adds Ctrl, which
a terminal cannot carry with Alt), short rows included rather than skipped,
the moving corner capped at the longest covered row, and palette labels
"Extend block …" (the first wording, "Block selection up", fuzzy-matched the
palette test's "clip"). Unit checks `test_block`; pty checks `test_block`;
goldens `block-keys` (both depths) and `block-wide`.

**Before that:** the narrow menu band, which Shawn decided on 2026-10-11 from three
options ("squeeze, then letters", over an overflow marker or leaving it
clipped). `menu_band_layout()` in `src/menu.c` picks one of four layouts by
width: two-space gaps (43 columns and up), one-space gaps (down to 37), letters
with the open menu's title in full, and letters only. The drop-down's layout
calls it again rather than reading the last draw, because in the third layout
switching menus moves the titles. The open title is drawn last, so its
highlight's padding is not painted over by a neighbour's one-column gap. Unit
sweep `test_menu_band` (every width from 14 to 120, every open menu); pty
checks `test_menu_band`; goldens `menu-band-squeezed`, `menu-band-letters`,
`menu-band-letters-open` (and `-256`). 35 existing narrow goldens changed in
their band row and nowhere else — checked section by section, not eyeballed.

**Before that:** italic comments, picked by Shawn on 2026-10-11. Italic is a
property of a theme role, as bold already was — `theme_entry` and `struct style`
gained a field, only `ROLE_SYN_COMMENT` sets it, and `grid.c` emits SGR 3/23 —
not a fourth `ATTR_` cell modifier, because nothing draws a comment without it.
`theme_detect_italic(TERM)` is a list of noes (`linux`, `screen*`, `vt*`,
`dumb`): those draw italic as a colour or as reverse video, which on a comment
reads as a selection. `[view] italic` (default on, so in the Options dialog
too) is what the user asked for and is what gets saved; drawn is that *and*
the terminal. Chosen without asking: comments only (not Markdown emphasis,
whose role also covers bold text), and no override to force italic under a
`screen` TERM — the manual points tmux users at `tmux-256color` instead.
`tests/screen.py` reads 3/23 into an `i` modifier. Pty checks are
`test_italic`; the C, AutoIt, Python and selection syntax goldens gained their
`i` runs.

**Before that:** the Options dialog (View → Options…), the last of the three.
`src/optdlg.c` lists config.c's table — section, name, value, and the selected
row's comment — and changes go through `setting_set()`, so a setting added to
the table appears here untouched. Changes are live; Enter keeps them, Ctrl-S
also writes mollyrc, Esc restores every value the dialog opened with. Numbers
step with ←/→ or take typed digits, applied on leaving the row. Chosen without
asking: no chord, View menu rather than a new Options menu, and Esc as
undo-all rather than close-keeping. The three indentation defaults still marked
"chosen without asking" are now one Space each in this dialog. Pty checks are
`test_options`; goldens `options` at both depths and `options-narrow`.

**Before that:** converting indentation (Edit → Tabs to spaces / Spaces to tabs),
the second of the three. `indent_convert()` rewrites only the leading
whitespace — a tab after the text may be data — of the selected rows or the
whole file, at the tab width in force, as one undo step. Chosen without asking:
no chord (it is a once-per-file thing, like Save settings), not on the help
screen (an unbound command there reads as a blank key), and a converted-to-tabs
width that is not a whole number of stops keeps its remainder as spaces rather
than rounding. Pty checks are `test_indent_convert`.

**Before that:** find in files (`Ctrl-E`, Search → Find in files…), the first of
three items Shawn asked for together on 2026-10-11 ("all the medium items":
this, converting indentation, and an Options dialog). `src/grep.c` builds one
`sh` line — `git grep` in a work tree, `grep -r` outside — and runs it through
`filter_run()`; the matches go into a buffer marked `grep_results`, loaded like
a file so it is never "modified" unless edited. Enter on a row opens the file
in the window the search ran from; `F4`/`Shift-F4` step through from anywhere
(VS Code's pair). A buffer may now have a `title` where it has no file name, and
`buf_name()` is the one place that decides what a buffer is called. Chosen
without asking: `Ctrl-E` (Ctrl-Shift-F reaches a terminal program as Ctrl-F),
results in a split below rather than replacing the current window, and no
wrap-around for F4. Unit tests run the real command in a scratch directory;
pty checks are `test_grep`; goldens `grep-results` at both depths and narrow.

**Before that:** resizing a split, picked by Shawn on 2026-10-11 as "#2" from the
same menu. `window_move_border()` in `src/window.c` moves the innermost split
along the arrow's axis one cell, tmux's rule: the arrow says which way the
*border* goes, so one chord means the same line from either window. A border
is held to the minimum of every window in the half it moves into, nested ones
included (`min_extent()`), which `layout_node()` alone never promised. Dragging
works on the `│` and on an upper window's status bar. Two things found on the
way and fixed: a split's share was in thousandths, which cannot put a border on
every cell past two thousand columns (it is 1/65536ths now, swept to 2047 in the
unit tests); and the help screen padded keys in bytes, which nothing noticed
until a key had an arrow glyph in it. Chosen without asking: **Ctrl-Shift-arrows**
as the chords — which costs the CUA word selection if molly ever gets word
movement — and two literal help rows (`Ctrl-Shift-←→`, `Ctrl-Shift-↑↓`) in
place of four, because "Ctrl-Shift-Right" overflows the 14-column key column;
those two rows do not follow a rebinding. The Window menu is nine columns wider
for the same chord name. Pty checks are `test_split_resize`; the golden is
`help-windows`.

**Before that:** inserting a command's output (`Alt-!`, Edit → Insert output…),
picked by Shawn on 2026-10-11 as "#1" from a ranked menu. `editor_insert_output()`
in `src/filter.c` runs the command with empty input through `filter_run()` and
hands the result to `cursors_insert_text()`, so it lands at every cursor and the
command runs once. Paste now goes through the same `editor_insert_text()`. Three
calls made without asking, any of them a line to change: one trailing newline
always comes off (so `date` lands inline); a selection is replaced rather than
sent; and it keeps its own last command, separate from the filter's. The Edit
menu is now seventeen rows, so at the 20-row size its goldens use it scrolls by
one; at 24 rows it still fits. Its pty checks are `test_insert_output`.

**Before that:** the
buffer-list dialog (`Ctrl-B`, Go → Buffers…) and the quit dialog (Save All /
Discard / Cancel) are done, in `src/bufdlg.c`, picked by Shawn on 2026-10-10 as
"1" from a ranked menu. The Ctrl-Q countdown, `quit_times` and
`KILO_QUIT_TIMES` are gone; `CMD_KEEP_QUIT_COUNT` stays because the close
warning still uses it. Save All walks the dirty buffers through `editor_save()`
and refuses to quit if any is still dirty afterwards. The full `make test`
passes on it: the pty stage includes the fixed quit test, and six goldens (the Go
menu and the palette, which gained `Buffers…`) were regenerated after the feature
commit, which had left them stale. The counts in the table below predate this feature. Both dialogs' mouse paths
were tested and both added to `make test-oom` on 2026-10-11;
a click in the buffer list now chooses, as the palette's does
(Shawn's call); the Open dialog's click still only selects, on purpose.

**Nothing else is in flight.** The drop-down's height is done end to end, in the arc
this repo keeps: `803c5fe` the geometry and the scroll with no menu changed,
`8a93dec` the three items that were waiting on it, and the documentation commit
after them. Soft wrap before it went the same way: `eaa8626` the display-row map
with nothing able to turn it on, `500c644` the key, the setting and the three
views of the command table that had room for it, `bf35a8f` the documentation.

**The three candidates the first handoff named are all done**, and so is the
drop-down item that was the best value-to-work ratio left. What is left is the
smaller work under "Next by dependency", none of it confirmed as next — ask
before starting any of it. The big-file rewrite and the language server are
both done, and so are the integrated terminal, rename, find references and
signature help, and the problems list he queued after them.

---

## Where we are

Phases 0 and 1 are done. Phase 2 has twelve features finished end to end:
regular expressions, the configuration file, the CUA keymap, multiple buffers,
the tab bar, syntax highlighting, window splits, the shell filter, indentation,
multiple cursors, soft wrap, and the language-server client. Molly searches and replaces with patterns on
an engine of its own; it reads `~/.config/molly/mollyrc` and can write it back;
its keys are CUA and remappable; it holds any number of files open at once with
a tab per file; it colours seven languages from table-driven definitions a
config file can add to; it splits its screen into windows that each keep their
own cursor, scroll and gutter; it pipes text through any shell command; it
indents a new line under the old one, with the width, the character and
auto-indent each answerable per filetype; it puts the cursor in as many places
as you like and edits in all of them as one undo step; and it lays a line too
long for the window out over as many screen rows as it needs without touching a
byte of the file. And every menu now works on a terminal shorter than the menu
is, which is what let the last three commands into theirs.

**Measured, not remembered** (all of these taken at the last commit from a
clean build on 2026-10-10; re-measure before quoting them anywhere):

| | |
| --- | --- |
| Source | 15,760 lines, `src/molly.h` plus 32 `.c` files |
| Checks | 1,642 — 750 pty, 105 golden screens, 787 unit |
| Fuzz | 4,040 cases per `make test` (40 replace, 4,000 regex, seed 42) |
| Warnings | clean under `-Wall -Wextra -Wpedantic -Wshadow` from a **clean** build; `regex.c` also under `-Wconversion` |
| OOM sweep | `make test-oom` green: 147 allocations failed in turn, 135 through `die()`, 12 kept running |
| Version | still `0.1.0-dev` |

The gate is `make test`: the pty suite, the golden screens, both fuzzers and
the unit binary. `make test-oom` is the allocation-failure sweep and needs
`cc` at runtime; it is not part of `make test`. Green at `8a93dec`, the last
commit to change code:

```text
750 passed, 0 failed
105 passed, 0 failed
40 cases, 0 failed (seed 42)
4000 cases, 0 failed (seed 42)
```

CI returned the same four numbers on `bf35a8f` (run 38084380677, 14m33s) and
then went red on the documentation commit after it, with two pty checks losing
every keystroke. That run is what closed the keystroke flake: the binary was
identical to the green one, so the fault could only be in the harness. See
"What only CI sees".

Since that fix, CI has returned its four numbers on every push: `a2ccf87` in
12m38s, `aa2ad2c` in 12m40s, `38528f1`, and `8424bae` — this session's head —
in 12m41s (run 38100739091, with the OOM sweep green behind it), against 14m31s
for the run before the fix on the same molly — the harness no longer waits out a session whose program has
already exited, which is most of two minutes across 737 of them. Those are also
the first runs whose log has no `pty_harness:` line in it, and that line is the
thing to grep a red run for now: it is what a session that was typed at and
answered nothing says about itself, which is the shape the flake had.

The pty suite now takes long enough that `make test` runs past ten minutes on a
cold build, most of it the pty stage alone — about seventeen minutes of it
before the harness fix and a couple of minutes less after. That is 750 pty
sessions each with a settle time, not a regression; budget for it rather than
assuming a hang, and start it in the background rather than waiting on it.
Its output is block-buffered when redirected to a file, so a log that has not
grown for minutes is not a hang either: `grep -c pass` on it undercounts by up
to a 4K block.

### What exists that did not exist a week ago

- A **cell grid** renderer with semantic roles, resolved by `theme.c` at both
  256 and 16 colours. Both depths are snapshotted so neither rots.
- A **layer stack**, and one key wait (`ui_wait_key()`) that pumps the event
  loop, so a modal dialog still resizes and still redraws.
- A **widget toolkit**: label, list, text, input, checkbox, button, with focus
  order, Tab/Shift-Tab, and SGR 1006 mouse hit-testing. `W_LIST` is
  deliberately *not* in the focus order — nothing needs it yet, and a role
  nothing uses is a theme entry nothing tests.
- A **command table**. The menu bar, the keymap, the palette and the help
  screen are all that one table viewed four ways. A new action is a record, not
  a `switch` case.
- A **menu bar**, a **mouse**, a **command palette** (Ctrl-Space), a generated
  **help screen** (Ctrl-?), the **open-file dialog** and the **Find dialog**, all
  on the toolkit.
- A **regex engine** (`src/regex.c`, 1,143 lines): a backtracking PCRE subset
  with counted repetition, captures, a step budget and two prefilters. It is
  fuzzed differentially against Python's `re` in bytes mode.
- A **configuration file** (`src/config.c`, 724 lines): an INI parser that warns
  and carries on, a settings table that owns every default, atomic write-back,
  and a `[keys]` section. `View ▸ Save Settings` writes it.
- A **CUA keymap**, fully remappable, with `Shift` and `Ctrl` nameable in a
  chord on the keys where a terminal reports them.
- A **buffer list** (`src/buflist.c`, 252 lines) and a **tab bar**
  (`src/tabbar.c`, 194 lines): any number of files open at once, each with its
  own undo history and its own remembered cursor position.
- **Syntax highlighting** (`src/syntax.c`): a table-driven lexer for Rust, C,
  C++, C#, Python, AutoIt and Markdown, with per-row cached lexer state and lazy
  invalidation. `[syntax lua]` in `mollyrc` is enough to add a language.
- **Soft wrap** (`src/wrap.c`, 302 lines): `Alt-Z` lays a long line out over
  several screen rows, breaking it after the last space that fits. One file,
  because the view had addressed display rows since Phase 0 — the draw loop, the
  renderer, the selection and every edit path were already asking the right
  questions. Off by default, per window, and a view and not an edit: the file
  keeps its bytes.
- **Window splits** (`src/window.c`): a binary tree of windows, each leaf a
  view. `F7`/`F8` split right and below, `F6`/`Shift-F6` walk them, `F9` closes
  one, and a Window menu holds those plus Only this window. Each window has its
  own cursor, scroll offset, gutter and status bar.
- **Multiple cursors** (`src/cursors.c`): `Ctrl-D` takes the word under the
  cursor and then each next occurrence, `Ctrl-Up`/`Ctrl-Down` build a column,
  `Alt`-click puts one anywhere, `Esc` goes back to one. Every edit and every
  movement reaches all of them, as a single undo step. Not one existing editing
  path was rewritten for it — see the two gotchas about the order and the
  replace-range rule below, which are the whole trick.
- **Indentation** (`src/indent.c`): `Enter` starts the new line under the old
  one and `Tab` types a tab or spaces to the next tab stop. The tab width, the
  choice of character and auto-indent are each a `[editor]` default that a
  `[syntax <name>]` block may override, which is what makes them the first
  per-filetype settings. The tab width stopped being one global in the process
  — see the gotcha about `indent_sync()` below.
- **A language-server client** (`src/json.c`, `src/lsp.c`, `src/lspui.c`):
  clangd, rust-analyzer or pylsp started for the files that name them, where
  installed; problems underlined and walked with F2; F12 to a definition and
  Alt-Left back; Alt-K for what a name is; Alt-/ to complete. Hand-rolled JSON,
  whole-document sync, answers acted on only from the main loop.
- **A shell filter** (`src/filter.c`): `Alt-|` pipes the selection, or the whole
  buffer, through `/bin/sh` and replaces it with the output, as one undo step.
  The file is in two halves with a banner between them: above it is text in and
  text out with no mention of a buffer, which is why the timeout, the output
  cap, the cancel, the deadlock and the error reporting are unit tests rather
  than pty sessions.

### Decisions already settled — do not reopen without asking

Full table in `ROADMAP.md` §7. The ones that bite soonest:

- **A search matches the row as stored**, in both plain and pattern mode. Olly
  searched the tab-expanded render buffer, so eight spaces found a tab. That is
  gone, and so are the two functions that mapped between the two spaces.
- **Config file is INI-style sections, `key = value`.** Chosen for being small
  to parse *and* small to write back, because the Options dialogs have to
  persist.
- **Keymap is CUA on the four conflicts, and fully remappable** except the
  Alt-letter menu titles, which are seven today and read off the menu table.
- **Multi-cursor edits in document order, and undo collapses to one cursor.**
  The order is what makes one fixup rule enough (see the gotchas); the collapse
  is because an undo record restores one position and not a list, so after
  undoing an edit made at several cursors the extras would be pointing at
  nothing the user put them on — and the next keystroke would land there.
- **`Ctrl-D` matches literal text, case-sensitively, and does not read the Find
  dialog's checkboxes.** Routing it through `search.c` would have been less
  code and would have meant that turning on regular expressions made a word
  with a `.` in it stop matching itself. What Ctrl-D looks for is text that is
  already selected, which is never a pattern.
- **The multi-cursor commands are not in the Edit menu.** They are keyboard
  operations, which is the reason transpose is not there either, and the
  palette and the help screen are both generated from the command table so
  they find them anyway. It also keeps four rows off molly's tallest drop-down,
  which is the first to scroll on a short terminal.
- **`Enter` inside leading whitespace keeps the text in its column.** Only the
  whitespace behind the cursor is copied and the tail keeps its own, so
  splitting `····alpha` after two spaces leaves `alpha` at column four. The
  same rule is what makes `Enter` at column zero insert a blank line above
  rather than an indented one, and it is why `editor_newline_and_indent()` is
  a second function rather than a change to `editor_insert_newline()` — paste
  and the shell filter both split rows through that one, and text that arrives
  indented must not be indented twice.
- **A drop-down is measured against the text area, not the grid**, and the row
  after the box belongs to its drop shadow. So the box and the shadow both stay
  off the status bar, which is what had kept three commands out of two menus,
  and a menu with more items than rows scrolls instead of losing its tail. The
  window follows the highlight and nothing else: that is the whole of the state,
  and it is why clicking a scroll arrow moves the highlight rather than the
  view.
- **A wrapped line breaks after the last space that fits**, and at the window
  edge for a word with no space in it. The space it broke after stays on the row
  above, because the slices of a row have to cover every one of its columns:
  trimming the run would leave a cursor in it with no screen cell to sit in. The
  column the break falls at belongs to the row *below*.
- **Vertical movement keeps the screen column when wrapped and the byte offset
  when not.** Between two slices of one line a byte offset is a windowful away,
  so wrapped there is no other sane reading of "down"; unwrapped it is what
  molly has always done and what every arrow-key test is written against.
  `ROADMAP.md` §7 has the asymmetry written down.
- **Wrap is the window's setting, not the buffer's** — so a split can wrap one
  half and not the other. Same rule as the gutter and the colours: per-view or
  per-screen is decided by where the thing is drawn.
- **Linux now. Windows possibly later. No macOS.** No Windows code gets
  written, but platform calls stay behind `term.c` and `sys.c`.
- **Every default lives in the settings table and nowhere else.** The menu bar
  used to declare its default in `screen_init_layers` as well, and the mouse was
  enabled in `enable_raw_mode` and taken back afterwards if the file said off.
  Both now have one home, applied by `config_apply()` straight after
  `init_editor()`.
- **`B` is a macro for `(*V.buf)`** — the current buffer is the current
  *view's* buffer. That is what let nearly two hundred `B.something` references
  survive buffers becoming plural, and it is the only definition that still
  works once there are splits. The buffer list holds **pointers**, not structs,
  because `struct view` holds a `struct buffer *` and an array of structs would
  move every buffer when it grew.
- **`V` is a macro for `(*E.curview)`** — the same trick one level up, and the
  reason splits touched almost nothing. Three hundred-odd `V.something`
  references went on meaning "the window I am working in". The two macros stack:
  `B` is the current view's buffer, so the current buffer follows the focus.
- **Each window's last row is its own status bar.** That is what makes a split
  readable, and it is also why one window is byte-identical to before splits
  existed: the bar lands exactly where the screen-wide one used to be. Seventy
  golden snapshots went on matching through the mechanism commit because of it.
- **A window tree, not a window list**, and a split stores a **fraction** rather
  than a column count. A list would have capped molly at one row or one column
  of windows forever; a fraction means a resize keeps the proportion and nothing
  has to remember the size it was laid out at last time.
- **A filter that fails does not replace anything.** A non-zero exit means the
  command produced nothing or half of something; undo would get the text back,
  but not losing it is better than getting it back. The accepted cost is that
  `grep`, whose exit 1 means "no matches", cannot be used to empty a selection.
- **The shell filter goes through `sh -c`.** A user typing into a prompt expects
  a pipeline, a redirect and a glob to work, and the command is their own with
  exactly their shell's privileges. Molly runs nothing the user has not typed.
- **Undo groups nest, by depth, on the buffer.** "Replace this selection with
  that text" is a delete group plus a run of inserts; without nesting it is two
  undo steps, which is what pasting over a selection had quietly been.
- **Per-view or per-screen is decided by where the thing is drawn.** The gutter,
  the scroll and highlighting are per view, so `Ctrl-U` and `Alt-Y` affect the
  window you are in. The menu bar and tab bar are per screen (`E.menubar`,
  `E.tabbar`), because the rows they take are above every window and which
  window has focus cannot be what decides whether they exist.
- **Closing the last buffer empties it** rather than removing it. An editor with
  no buffer has nothing to draw, and every caller would need a null check for a
  state the user cannot usefully be in.
- **Highlighting is computed over `render`, not `chars`** — a role per byte of
  the tab-expanded string, which is what the draw loop walks. That is the
  opposite of what a *search* matches, and deliberately so: one is a display
  question and the other is not.
- **An unterminated string does not span rows.** A stray quote colouring the
  rest of a file turns a typo into what looks like a disaster. Block comments,
  Python's triple quotes and Markdown's fences do span rows; a plain string
  never does.
- **Syntax definitions ship for Rust, C, C++, C#, Python, AutoIt (`.au3`) and
  Markdown.** Seven, decided 2026-10-08. Two of them are what make a cached
  per-row lexer state unavoidable rather than merely correct: AutoIt's
  `#cs`/`#ce` blocks and Markdown's fenced code blocks both open a region whose
  end is many rows away. Markdown also means the definition format cannot assume
  keyword/string/comment rules are enough.

---

## What is left

### Still open — needs Shawn

`ROADMAP.md` §8 is empty: the last item, which languages ship with syntax
definitions, was answered on 2026-10-08 and is recorded in §7.

Three of indentation's answers were **chosen while building it rather than
asked about**, because none of them changes what the work was. They are in §7
with their reasoning, and any of them is a one-line change if Shawn disagrees:

- `insert spaces` defaults **off** (a literal tab), because the setting decides
  what bytes a save writes.
- `auto indent` defaults **on**, because it changes only what a keystroke does.
- `Enter` inside leading whitespace leaves the text in its column, which is the
  case the references disagree about. Pinned in both suites.

Soft wrap's four are the same shape — chosen while building it, in §7 with the
reasoning, and each a small change if Shawn disagrees:

- `soft wrap` defaults **off**, because molly's own answer for a long line is
  the horizontal scroll and the file it is most often editing is code.
- The break is a **word wrap** — after the last space that fits — and not a
  column wrap. A word with no space in it still breaks at the edge.
- `Home` and `End` reach the **line**, not the display row. VS Code goes to the
  display row; this is the one place molly deliberately does not follow it.
- Vertical movement keeps the **screen column wrapped and the byte offset
  unwrapped**. Making both the screen column would be the consistent answer and
  would move arrow-key behaviour on any row with a tab before the cursor, which
  is why it was not done quietly.

The drop-down's height came with five of the same kind. The first four are in
`src/menu.c` and none is more than a few lines to change; the fifth is in
`tests/test_golden.py`:

- **The arrows are `▴` and `▾`, two columns in from the right-hand corner**, in
  the border row at each end that has more behind it. The small triangles rather
  than the big ones, to sit in a line of box drawing the way the tab bar's `‹`
  and `›` sit in a row of tabs; the inset lines the pair up like the thumb of a
  scroll bar. Nothing says *how far* the list is scrolled.
- **Clicking an arrow moves the highlight to the first item past that edge**,
  because the window follows the highlight and nothing else — so a click with
  the highlight in the middle of the box jumps it to the edge rather than
  scrolling under it. Scrolling the view independently would mean a second piece
  of state and a rule for what happens to a highlight it scrolls away from.
- **A scrolled box starts one item later rather than on a separator**, when the
  row it gives up is not the last it can show and the highlight stays inside. A
  divider directly under the top border reads as a second border, the group
  above it having scrolled away.
- **Indent and Dedent went between the selection commands and the filter**, in a
  group of their own, on `I` and `D`; **Soft wrap went under Syntax highlighting**
  on `W`, beside the other toggles that change how the text is drawn. Both are
  one line in the item tables.
- **Nine golden screens moved to taller terminals** — four Edit ones to twenty
  rows, five View ones to eighteen. They are snapshots of dimming, ticks,
  separators and the column squeeze, and a sixteen-row screen no longer shows
  the whole of either menu. What stays at twelve rows is `menu-edit-narrow`
  and the two `menu-edit-scrolled` ones, which are the short screen on purpose.

### Next by dependency

All eight of the first handoff's items are done: the config file with the CUA
keymap, multiple buffers with tabs, syntax highlighting, window splits, the
shell filter, indentation with per-filetype defaults, multiple cursors, and
soft wrap. Nothing on the list below has been confirmed as next.

**The roadmap is finished** as of 2026-10-12: every IN and LATER item in §5
is done, reflow is declined with its reasons, and what the language server
still leaves is snippets and range edits for very large files. What comes next
is Shawn's to name.

Nothing large is left in `ROADMAP.md`: the integrated terminal is done too. The big-file rewrite is done;
what it leaves (wrap measuring every row of a huge file once per width) is at
the end of §5's big-file section. The language server is done; what it leaves
-- rename, references, signature help, a problems list, completion as a word is
typed -- is at the end of §5's LSP section, and each is a request, an answer
and a way to show it on top of what exists.

Smaller things the last six features left behind, each a self-contained
commit:

- ~~**Block selection**~~ **Done** on 2026-10-11: a cursor per row, in
  `src/block.c`. See "Latest" above.

### Found along the way, not fixed

- ~~**Nothing converts a file's existing indentation.**~~ **Done** on
  2026-10-11: Edit → Tabs to spaces / Spaces to tabs, `indent_convert()` in
  `src/indent.c`. Leading whitespace only, at the file's tab width, remainder
  kept as spaces when converting to tabs.
- ~~**The quit warning is a message, not a list.**~~ **Fixed** by the quit
  dialog (`c3dd3b4`), which lists every unsaved buffer.
- ~~**The help screen's columns truncate on bytes, not columns.**~~ **Fixed**
  with split resizing (`0f79f3d`), when the border chords put the first arrow
  glyph in a key: both columns are now cut and padded with `utf8_fit_cols()`.
- ~~**A drop-down taller than the screen loses its tail.**~~ **Fixed**
  (`803c5fe`), and it was the twenty-five lines the estimate said, plus the
  arrows and the mouse. The box is measured against the text area with the
  shadow's row left inside it, and what does not fit is scrolled.
  `menu-edit-narrow` is still the snapshot at twelve rows, now of the scroll
  rather than of the clipping, and `menu-edit-scrolled` is the other arrow
  state at both colour depths.
- ~~**The menu band itself still clips.**~~ **Done** on 2026-10-11: below 43
  columns the gaps close, below 37 the titles become their mnemonic letters,
  and the open one is written out. `menu_band_layout()` in `src/menu.c`.
- ~~**An Options dialog has nothing to do but exist.**~~ **Done** on
  2026-10-11: View → Options…, `src/optdlg.c`, over `setting_info()`,
  `setting_live()` and `setting_set()` in `config.c`.

Everything else — the cheap Edit/View/Navigate items, soft wrap,
the integrated terminal, LSP, big-file handling — is argued in `ROADMAP.md` §5
with its verdict. Soft wrap already has its *model* landed; the feature comes
later.

---

## How to work on this

### Conventions this repo keeps

- **Separate the mechanism commit from the visible-change commit.** The engine
  landed unreachable from the editor; the feature that reached it came next.
- **Iterate on one pty test, not on the suite.** The whole suite is seventeen
  minutes and stops at the first failing stage, which is a terrible loop to
  debug a new check in. `tests/test_molly.py` is importable: set `MOLLY` to the
  binary, make a temp directory, call the one test function, read
  `PASSED`/`FAILURES`. Three lines in a scratch file, a second per run. Then run
  the real gate once, at the end, in the background.
- **Measure rather than guess.** 160 bytes per recursion depth came from an
  instrumented scratch build. The 9 ms worst-case budget was timed. Do not put
  an estimate in a comment.
- **Probe at sizes nothing has been tried at.** 6×20, 7×22 and 8×24 are what
  found both Find-dialog fit bugs. Every dialog needs a narrow golden.
- **Lift the arithmetic out of the drawing, so a terminal is not needed to
  prove it.** `wrap.c`'s display-row map and `menu.c`'s `menu_dropdown_height()`
  and `menu_dropdown_scroll()` are both pure over numbers, and both carry the
  cases a snapshot can only reach by running molly at a size nobody runs — a
  five-row terminal, a list of sixteen items in a box of three. The unit test
  then sweeps every shape instead of asserting three of them.
- **Prefer the test a reference engine can arbitrate.** Where the references
  disagree, pin molly's answer in a unit test and say so in the comment. Two
  such corners are named at the top of `src/regex.c`.
- **A new UI element needs a golden screen at both colour depths**, and a narrow
  one. Snapshots have separate blocks for screen, attrs, colors, modifiers and
  cursor, so a lost highlight or a stray cursor claim is caught, not averaged
  away.
- Commits go straight to `main`, no PRs, and Shawn approves each one. Git
  identity is configured nowhere on this machine: commit with
  `git -c user.name=shawn -c user.email=locodarwin@gmail.com`, rather than
  writing config nobody asked for.

### Gotchas

- **A JSON literal's length is counted by the compiler, not by hand.** Two
  hand-counted `ab_append` lengths and one fixed-size `json_putf` each sent a
  server invalid JSON this session. Use `json_putf` for anything with a quote
  in it; a script in the session that compared every literal append with its
  string found the rest correct.

- **`GOLDEN_ONLY="name name"` runs just those golden cases**, with or without
  `MOLLY_UPDATE_GOLDEN=1`: the way to rerun one that failed, or regenerate the
  handful a change was meant to change without rewriting the rest.
- **Anything that polls for Esc must survive standard input at end of file.**
  `input_escape_pressed()` used `editor_read_key()`, which loops until a byte
  arrives; with stdin at EOF (a unit test run with `< /dev/null`, or a hung-up
  terminal) that loop never ended, and `git_refresh()` from `editor_save()`
  hung the unit binary inside `filter_run()`. It now makes one `read()` and
  decodes with `decode_key()`.
- **The unit binary's colour checks depend on `COLORTERM`.** The true-colour
  ones set it and `TERM` themselves and put `TERM=xterm` back, as the grid tests
  do; a check that calls `theme_fit_color()` without doing that passes or fails
  with the shell it is run from.
- **Run commands through `cmd_run()`, never `cmd->run()`.** It is what refuses
  `CMD_EDITS` commands on a terminal. A new command that changes text needs
  the flag, or it will edit a terminal's empty buffer from the menu.
- **A terminal's golden needs `tests/fake_shell.sh`, not a shell.** Its pty
  checks use `/bin/sh` with `PS1` pinned and `ENV` empty, which is enough for
  dash and bash to agree on what is typed and printed -- not on the screen
  byte for byte.
- **Keep scratch files out of `/tmp` and out of the tree.** This session's
  were under `.git/tt/`, which git never shows.

- **The pty harness pins `MOLLY_LSP=off`.** C files open in dozens of tests, and
  on a machine with clangd each would start one and draw its diagnostics. A
  language-server test sets `MOLLY_LSP` back to empty and names
  `tests/fake_lsp.py` in a mollyrc. The Options dialog shows the setting's own
  value (`lsp_setting()`), not the environment's, so Save Settings never writes
  a harness's "off" into anyone's file.
- **Never act on a server's answer where it arrives.** `lsp_answer()` runs inside
  the event loop, which a menu or a dialog may be pumping. It fills the mailbox;
  `lspui_deliver()`, called from the main loop only, acts on it if the cursor
  has not moved on. A new kind of question goes through the same two steps.
- **A popup closes in `lspui_key()`, before the key is dispatched.** It is a
  layer the size of the grid, so a click would otherwise be swallowed by it;
  closing first means the click lands on the text. `WINCH_KEY` must not close
  one: it is also what `ui_request_redraw()` sends when an answer arrives.
- **Search the fake server's log for a method's full name.** `"didSave"` alone
  is in the capabilities `initialize` sends, so a check for it passed before
  anything was saved, until ASan's slower run showed it.
- **`/tmp/molly-golden` is one fixed directory.** Two golden runs at once delete
  each other's files. A `TMPDIR` of its own keeps a second run apart, at the
  cost of the four Open-dialog goldens, which print the directory.

- **Never write a row's `chars` directly.** A row that has not been changed
  since the file was read (or saved) is a span of `B.text`, which is also the
  saved snapshot; writing into it changes what "unmodified" is compared
  against, and nothing fails until undo back to the saved state stops clearing
  the modified flag. Every `editor_row_*` mutator and `editor_row_truncate()`
  calls `row_own()` first. A new mutator has to as well.
- **Nothing outside `buffer.c` indexes `B.row[]`.** It is a gap array, so slot
  and index differ past `B.gap`; use `row_at()`, or `buf_row()` for a buffer
  that is not current. And an `erow *` does not survive an insert or delete of
  *any* row now, above or below it — the gap moves rows on both sides. Fetch it
  again after.
- **Borrowed rows have to stay in file order**: the in-place compaction in
  `saved_snapshot_take()` depends on it. It holds because only a load or a save
  makes a row borrowed and every inserted row gets text of its own — so code
  that swapped two `erow` structs to move a line would break saving, quietly.

- **`pty_harness.run()` execs via `os.execve`** — the binary path must be
  absolute. A relative path fails silently in the child and every assertion
  comes back False.
- **`screen_sync_size()` before measuring anything.** A dialog that lays itself
  out against a stale size draws outside its frame.
- **`dialog_init` memsets.** Call it *once*, outside the modal loop, or it wipes
  `focus` on every iteration. Set `d.focus` after it, not before.
- **Derive widget widths from `d.cols` after `dialog_layout`,** not from the
  terminal.
- **An input field keeps its rightmost column for the cursor**, so a narrow
  expectation wants N−1 characters, not N.
- **Only the focused input claims the cursor** (`widget_input_draw(..., claim)`).
  A dialog whose field should keep the cursor has to say so — the palette sets
  `d.focus = 1` for exactly this reason.
- **A Tab keystroke is dropped by the inline prompt.** This is why
  `tests/fuzz_replace.py` puts tabs in the *text* it generates but never in a
  query or a replacement (`TYPEABLE`), and why a tab is only nameable as `\t` in
  a pattern.
- **The regex prefilters can defeat a budget test.** `(a+)+b` on thirty a's is
  now an instant no-match, because `b` is a required character. Budget tests
  anchor instead: `(a+)+$`.
- **The help column truncates.** It is 17 columns and cuts in silence; check the
  help screen after adding a command. "Swap the two characters at the cursor"
  came out as "Swap the two char".
- **The help screen's goldens stop above the last group.** `help-wide` and
  `help-narrow` snapshot the first screenful, and the VIEW group is at the
  bottom of the reference — so a new VIEW command's help entry is not proved by
  any golden, and neither of them moves when one is added. What proves it is a
  pty check that opens the reference and pages down twice, which is what the
  soft-wrap test does. The *description* is what renders there, not the label:
  the check is for "Wrap long lines", not "Soft wrap".
- **A `pty.fork()` in a script fed on stdin forks the interpreter.** Python
  reads such a script incrementally from fd 0, and the child's fd 0 is the pty
  slave — so the child goes looking for the rest of the script in the terminal,
  finds whatever the parent typed at it, and re-runs whatever it can parse.
  Seven interpreters and a `kill -9` later: write the probe to a file and run
  `python3 -I` on it, which is what every test in `tests/` already does.
- **A pty test that walks a menu by counting `Down` keystrokes breaks when the
  menu gains an item.** Soft wrap joining the View menu turned six Downs from
  Save Settings into Redraw, and the failure reads as "saving settings reports
  where it wrote: want True, got False" with nothing about a menu in it. Worse,
  the check after it opens the file that was never written, so the suite stops
  there with a traceback and every later group goes unrun — a second gate run,
  not a second failure. Press `End` for the last item or its mnemonic for any
  other, both of which say what they mean. While you are there, check the golden
  screens still show the whole menu: nine of them had to move to taller
  terminals when these three items landed, because a sixteen-row screen stopped
  being tall enough for the Edit or the View menu.
- **A local in a pty test can shadow a keystroke.** `tests/test_molly.py` keeps
  the keys as short module-level names — `CUT`, `SAVE`, `HOME`, `END`, `COPY`,
  `PASTE` — so a test-local `CUT = "alpha beta…"` silently turns `[CUT]` from
  Ctrl-X into forty characters of typing. It fails as a *content* mismatch with
  no hint of why: the file came back with the string in it. Name locals in a
  test function something a key is not.
- **`edit()` runs at 80×24.** It is the right helper when the file afterwards is
  the whole proof, and the wrong one for anything whose subject is the window:
  a 57-column line does not wrap at 80, so every soft-wrap check written on
  `edit()` passed on the unwrapped path and proved nothing. A test about width
  writes its own runner with `rows=`/`cols=`.
- **The status bar drops `Ln`/`Col` before it truncates.** The file name is cut
  to 20 columns (`%.20s`) and the cursor block is only drawn when what is left
  fits it, so at 40 columns there is no position on the bar at all. A test that
  reads the cursor position from the status bar needs 80; one at 40 reads the
  terminal's own cursor escape instead (`cursor_col`, or the last `ESC[r;cH`).
- **A command with no chord has no business on the help screen.** It renders as a
  blank key beside a description, which reads as a bug. Match case is reachable
  from the Search menu and the palette instead.
- **`config_write` saves what the editor is doing, not what the file said.** Each
  setting may name a function to read its live value back, which is what makes
  "toggle it, then save" work. A test that parses a file and writes it straight
  back out without applying it will see the live defaults, not the parsed values.
- **A modifier is only nameable where a terminal reports it** — the function and
  navigation keys. `shift-a` and `ctrl-shift-l` are refused at parse time, which
  is also why the roadmap's `Ctrl-Shift-L` for repaint could not be used.
- **A toggle has to be read where the thing is *drawn*, not only where it is
  computed.** `V.syntax` was checked where rows are lexed but not where their
  roles are consulted, so turning highlighting off stopped relexing and left
  every role already computed on the screen. The same shape will come up again
  for anything else that caches what it draws.
- **A display row is not a file row, and the code that forgets it looks
  right.** With wrap off the two are the same number, so arithmetic that treats
  a screen row as a file row passes every test until the day wrap is on.
  `syntax_ensure_view()` was doing exactly that — `V.drowoff + V.screenrows` as
  a row to lex — and the fix was to ask `drow_at()` for the slice and read its
  `filerow`. Anything that turns a screen `y` into a position goes through
  `drow_at`, and anything that puts a cursor on screen goes through
  `drow_cursor_col()`, which is no longer `CUR.rx`.
- **A row's wrap count is cached on the row, and `editor_update_row()` is where
  it is dropped.** That is the one place `render` is rebuilt, which is the only
  thing that can change the count at a fixed width — so a new row mutator that
  does its own rendering would paint a stale line and nothing would say so. The
  invalidation sits *above* the no-tabs early return in that function, which is
  a path that changes the row's width just as much as the other one.
- **`drow_at()` keeps one step of its walk, and four things invalidate it**: the
  buffer, the width, the row count and a generation the buffer bumps whenever a
  row's measurement is dropped. The last two overlap deliberately — deleting a
  row changes the count without re-measuring anything, and an edit re-measures
  without changing the count. Without the memo a screenful cost a walk of the
  file per row rather than one walk: 2.9 ms a frame at 100,000 rows instead of
  0.24.
- **How that was measured, so it can be measured again.** There is no perf
  harness in the repo and this did not earn one. A scratch `main()` linked
  against `$(LIBOBJ)` (the Makefile already builds that list for the unit
  binary) calls `window_init(); buflist_init(); syntax_init()`, inserts N rows
  of about 120 columns of words, sets `V.screencols = 80`, `V.screenrows = 40`,
  `V.gutter_w = 0`, puts the cursor on the last row and the scroll offset 40
  display rows above the end — the worst case, because every walk is the whole
  file — then times 100 iterations of what one frame asks for: `drow_total()`,
  `drow_of_cursor()`, and `drow_at()` once per screen row. Wrap off is the
  control and comes out at zero. Anything that changes `wrap.c` should re-run
  something like it rather than trust the numbers above.
- **The wrap width is `TEXT_COLS()`, which depends on `V.gutter_w`** — set by
  the layout pass, not by the view. So anything that measures display rows has
  to run after `window_layout()`, which `window_scroll_all()` and
  `screen_sync_size()` already arrange. Toggling the gutter re-wraps every row
  by changing that width, which is why both toggles repaint in full.
- **The help screen's row cap is a silent truncation waiting to happen.** It is
  128 now and the content is about 58 lines in one column, but when it is next
  outgrown the screen says "help truncated" rather than stopping quietly. That
  message existing is the fix; the number is not.
- **`B` is a macro, so freeing a buffer that is not current borrows `V.buf`.**
  `buf_free()` points the view at the buffer it is freeing and puts it back
  afterwards, because the undo and snapshot frees are written against `B`. It is
  ugly and it is deliberate: the alternative is a buffer argument on every one of
  those functions, which the planned piece-table rewrite will replace anyway.
  `write_recovery_all()` borrows it the same way.
- **`die()` can run before there is a buffer.** It calls
  `write_recovery_all()`, and an allocation failure while the buffer list is
  being built reaches it with `V.buf` still NULL. Anything new on the `die()`
  path needs the same guard, and `make test-oom` is what catches a missing one.
- **The tab bar costs the text area a row**, but only with two or more buffers
  open. A test that reads a fixed screen row for the first line of text is right
  with one buffer and one row out with two. `chrome_rows_above()` is the one
  place that arithmetic lives.
- **`V` is a macro too, so drawing a window that is not focused borrows it.**
  `view_select()` makes a view current and returns the one that was; the draw
  walk, the layout walk and the mouse handler all use it so that code written
  against `V` — the row renderer, the gutter width, the click-to-cursor
  arithmetic — runs for the window it should. Every caller restores what it was
  given, on every path. Code that needs to know which window actually has the
  keyboard has to read that *before* the walk starts, or it will be told "this
  one" every time; `window_draw()` does exactly that.
- **`window_init()` runs before `buflist_init()`**, because `V` is a view in the
  tree and `buflist_init()` points that view at the first buffer. It also gives
  the first view its cursor list, so there is no order of startup in which `V`
  has a buffer but nowhere to put the cursor. The first view itself is
  `state.c`'s, declared statically, so `E.curview` is non-NULL before main runs
  and every existing null-check on `V.buf` still means what it says.
- **The unit tests share one process, so a test that tears the editor down
  breaks the next one.** `test_windows_buffer_closed()` ends with
  `buflist_cleanup()` to check that no window is left pointing at a buffer, and
  then has to stand the editor back up: `tab_width_set()` in the config tests
  walks the current buffer's rows, and with the list gone it reads through a null
  view buffer. The crash lands four tests later, in a file that has nothing to
  do with it.
- **Block-buffered stdout moves a crash.** `make test` pipes the unit binary, so
  its output is flushed in 4K blocks and the last line printed is not where the
  segfault was — twice this sent me looking at the lexer for a bug in the buffer
  list. `stdbuf -o0 ./tests/units_test` is what tells you where it really is.
- **A pipe nobody reads is readable for ever.** `filter.c`'s run loop leaves on
  the first sign of trouble for this reason: the first version carried on
  polling in a "stopping" state, and once the output cap was hit `on_readable`
  refused to read, so `poll` reported the pipe ready every time, `loop_wait`
  returned instantly, and a flooding command became a spin with no way out.
  Anything that stops consuming a source must stop polling it in the same
  breath.
- **Reaping a child must not block.** A command that closes its output and then
  sleeps is past every limit that watches pipes, and a blocking `waitpid` on it
  hangs the editor. `WNOHANG`, SIGTERM, a grace period, SIGKILL — and to the
  process *group*, or a pipeline outlives the shell that started it.
- **SIGPIPE is ignored inside `filter_run`, not at startup.** Writing to a
  command that has stopped reading is normal (`head -1` does it) and its default
  action kills the writer, which is the editor. Setting it where raw mode is set
  left the unit tests unprotected — the check for exactly that case killed the
  test binary — so the module that writes to pipes saves, ignores and restores
  it, and the child puts it back to the default so a command still dies of it
  the way it would in a shell.
- **A new setting applied in `config_apply()` can silently defeat an env test
  hook.** `undo steps` becoming a setting made `MOLLY_UNDO_STEPS` useless,
  because `config_apply()` ran after `init_editor()` and put the cap back. The
  precedence is now default, then file, then environment, and the hook sits
  after `config_apply()` in `main()` to say so.
- **The tab width is not a global, and `tab_width()` is not what the renderer
  reads.** A width that can come from the filetype is per buffer, because two
  windows can show two filetypes in one frame — with one global, each window
  re-rendered a whole file to undo the other's work, every frame.
  `editor_update_row()` reads `B.tabw`, the width that buffer's render strings
  were actually built at; `tab_width()` is the width that *should* be in force;
  and `indent_sync()` is the only thing that moves one to the other. It runs
  from `syntax_retype()`, from `tab_width_set()`, and once per window per frame
  in `window_scroll_all()` — before `editor_scroll()`, because the scroll
  measures the cursor against the render string the sync may replace. Anything
  new that changes which buffer is current needs no call: the per-frame one
  catches it.
- **Cursors are edited in document order, and that is load-bearing.** An edit
  at one cursor moves the text under the others, so `cursors_edit()` sorts them
  and runs top to bottom: every edit is then strictly earlier in the document
  than every cursor still to come, and nothing an edit does reaches text before
  it, so a cursor is final the moment it has been processed. Run them in any
  other order and the fixups below stop being enough. The loop also sets
  `V.primary` to each cursor in turn so the ordinary editing functions, which
  all work on `CUR`, do the work — which is why none of them needed changing.
- **One rule moves the cursors an edit has not reached yet, and it is derived
  from where the cursor landed.** Every operation is a *replace*: it removes a
  range and leaves the cursor where the text that followed now begins. With
  (r1,c1) the end of the removed range and (nr,nc) the cursor afterwards, a
  position after it goes to `(nr, nc + (col - c1))` on row r1, or down by
  `(nr - r1)` below it. That covers typing, Tab, Enter, Backspace, Delete and
  paste without any of them reporting anything. **Deleting forward is the one
  exception**: its removed range ends past the cursor it started at, which is
  why `range_end()` has a case for it. Anything new that edits at N cursors
  either fits that shape or needs its own case there.
- **`selection_clear()` is every cursor; `selection_delete()` is one.** They
  look like a pair and are not. The dispatcher calls `selection_clear()` to mean
  "the selection is over", which with several cursors means all of them.
  `selection_delete()` is called *once per cursor* from inside the edit loop, so
  it clears only `CUR` — making it call `selection_clear()` meant the first
  cursor's edit threw away the selections of every cursor whose turn had not
  come, and Ctrl-D then replaced one occurrence and merely typed at the rest.
  That one cost an hour.
- **The extra cursors are drawn as a second pass over the cells, not in the
  draw loop.** `cursors_mark_row()` re-roles the handful of cells it owns with
  `grid_shade()` after `editor_draw_text_row()` has run, and returns
  immediately when there is one cursor — which is why not one of the 87 golden
  screens that existed before moved. The per-character loop runs on every row
  of every window every frame and multi-cursor is rare and purely additive, so
  it does not get a branch. The primary is skipped because it has the
  terminal's own cursor; drawing a block there too would show two.
- **A `live` function in the settings table must return the default, not the
  value in force.** The three indentation settings can each be overridden per
  filetype, so `live_tab_width()` returning `tab_width()` would have written
  C's width into `mollyrc` as everybody's the first time settings were saved
  with a `.c` file open. `tab_width_default()` and the two
  `indent_*_default()` functions exist only for this. It is not enough to read
  the settings table either: a unit test calls `tab_width_set()` directly and
  expects write-back to see it, which is the whole point of `live`.

### What only CI sees

CI is `.github/workflows/test.yml`: `make`, `make test` and `make test-oom` on
`ubuntu-latest` at every push. It was red for eleven runs straight, from
2026-10-08 to 2026-10-09, and red once more on 2026-10-10 — a run of a
documentation-only commit, whose binary was identical to the green one before
it, which is what finally said the fault was in the test harness. Not one of the
twelve was a bug in molly. Everything below is closed.

- **`gh run list` in this working tree answers for the wrong repository.** There
  are two remotes -- `origin` (molly) and `upstream` (olly) -- and with no
  default set `gh` reported olly's runs, which are green. Eleven red molly runs
  read as "CI is fine" for a day. Always `gh run list -R locodarwin/molly`.
- **`/bin/sh` is bash here and dash on the runner.** The filter check for a
  command that does not exist asked for `"command not found"`, which is bash's
  wording; dash says `"not found"`. It now asks for the substring both shells
  share, because what it tests is that molly passes the shell's own words
  through, not which shell `/bin/sh` is.
- **Python's `\B` only began matching the empty string in 3.14** (gh-124130).
  The regex fuzz is differential against `re`, this machine runs 3.14 and
  ubuntu-24.04 runs 3.12, so two cases of seed 42 -- `\B` against `""` -- failed
  there and passed here. Molly follows Perl, PCRE and 3.14;
  `PY_B_MATCHES_EMPTY` in `tests/fuzz_regex.py` skips that one shape when the
  reference predates it, and counts what it skipped.
- **Five pty checks lost every keystroke on the runner and never here —
  diagnosed and closed on 2026-10-10.** "arrow up", "SS3 home", "arrow down",
  "home" and "page up", across five runs; each reported a file nobody had typed
  into, with no stray characters in it. Three causes, all in the harness and
  none of them in molly:
  - it slept 0.3s before typing, and molly enters raw mode with `TCSAFLUSH`,
    which discards input that arrived before it got there — so it waits for
    molly's first byte, written just after that `tcsetattr`;
  - it killed molly on a timer `settle` seconds after the last key, which can
    land before molly has read the Ctrl-S the check was about — so it waits for
    the output to go quiet, with `settle` as the floor and a 3s cap;
  - and the one that was actually doing it: **the first byte is not the first
    frame.** `\x1b[?2004l` is written inside `enable_raw_mode()`, before
    `init_editor()` loads the file, before `config_apply()`, and before anything
    is painted. So the harness typed at an editor still starting up, the keys
    sat unread in the tty buffer, and then the wait after them read that silence
    as "finished" and killed molly with every keystroke still in the queue. The
    window is about six tenths of a second wide — the 0.4s `settle` floor plus a
    0.15s poll. Startup here is ten milliseconds; on a loaded two-core runner,
    five times it was not.
  The harness now waits for a *frame* before it types — something only a paint
  produces, since molly positions the cursor on every one and nothing it writes
  earlier does — and after the last key it will not accept quiet until something
  has come back since that key. A program that has exited is the one silence it
  does not wait on. A session that was typed at and answered nothing now prints
  a line to stderr saying so, so a silent kill can never again be
  indistinguishable from a passing test. `run_until_signal` had the same window
  and the same fix, which the recovery tests depend on.
  **It reproduces in a second**, which is what two sessions of it never managed:
  `test_harness_startup` runs a stand-in that writes one byte and then says
  nothing for up to three seconds, and the harness still has to get the keys in.
  It fails against the old harness at 1.5s. The earlier attempts failed because
  they looked for a race in the *typing* -- 25 runs pinned to one CPU against six
  busy loops -- and the race was in the startup. CI has agreed twice since, on
  `a2ccf87` and `aa2ad2c`, and neither log has a `pty_harness:` line in it.
- **The newest cursor is not the last entry in the list.** The list is kept in
  document order, so a match found by wrapping to the top of the file sorts
  first. Skip, drop-last and the search Ctrl-D runs are all about the cursor
  added last, which is why a cursor carries `seq` and `cursors_newest()` exists.
  Reading it off the end of the array is what Ctrl-D used to do, and after a
  wrap it searched from a cursor the user had left behind.
- **A selection that stops at column zero of a row has not reached into that
  row**, and `indent_shift()` leaves that row alone — which is what lets you
  drag down the left edge to take whole lines without indenting the one you
  stopped on. Two of my own unit fixtures were written as "select two rows" by
  putting the cursor at column zero of the second one, and both silently tested
  one row until they failed. The rule is pinned in its own check now.
- **Every path that changes the buffer has to clear the redo stack**, and
  `search.c` was the one that did not: `clear_redo()` is called at six places
  in `edit.c` and nowhere else, so after an undo a replace-all left the undone
  record on the redo stack and `Ctrl-Y` put it back on top of the replaced
  text. Three keystrokes to reproduce, and it had been shipping since
  replace-all landed. The row primitives in `buffer.c` do not record undo or
  touch redo on purpose — a caller that uses them owns both.
- **The help screen is one of the four views of the command table, and it is
  the one that gets forgotten.** `MANUAL.md` had claimed since multi-cursor
  landed that the cursor commands were on it; they were in the table and the
  palette, and `help_entries[]` was never touched. When a command is added,
  check all four: the table, the menu, the palette and `help_entries[]`.

### Operational mistakes to not repeat

These all cost real time in the last session.

- **Judge warning-cleanliness from a clean build.** An incremental `make test`
  does not recompile what it has not touched, so a warning in a test file can
  sit there for several green runs. One was:
  `"a\xe6\x9c\xa8b"` is not five bytes, because `\xa8b` reads as a single
  out-of-range hex escape — the `b` never reached the string, and the check
  named after it was re-testing the line above. `make clean && make test`
  before claiming the tree is warning-free.
- **Do not rebuild `./molly` while a test run is in flight.** It invalidated
  three separate runs before I noticed. `make test` recompiles the unit binary,
  so editing anything under `src/` or `tests/` mid-run counts as rebuilding.
- **Four of my own assertions about a split screen were wrong**, which is the
  same lesson as last time in a new place: the status bar is on row 22 of 24 and
  not row 21, and a click past the end of a line puts the cursor *after* the
  text. Read the screen before writing the assertion.
- **A role-per-character sketch is easy to miscount.** Nine of the lexer tests
  failed on the sketch's length rather than on the lexer. `syn_check()` now
  reports a length mismatch as a miscount in the test, which is the only reason
  the rest were quick to write.
- **Do not let a test fixture's contents contain its own file name.** Two of my
  tab-bar checks asked "is this name on the tab row?" against files whose text
  was their own name, so they passed whenever the row was missing entirely.
- **A test that saves changes the fixture for every test after it.** One
  tab-bar check pressed Ctrl-S, and three later ones compared the file against
  contents it no longer had. Rewrite the fixtures, or use a private copy.
- **Read the screen before writing the assertion.** Six of my first pty checks
  failed on my guesses rather than on molly: the gutter reads `1 │ alpha` and not
  four spaces, `Sel` is on the status row and not the message row, a selection of
  three lines counts 16 and not 19, and `a` occurs twice in `alpha` so "the next
  match" was never going to be the next line. One peek script would have saved
  all six.
- **`edit()` in the pty tests does not save.** It reads the file back off disk,
  so a key list without `SAVE` in it compares the original against itself and
  passes for the wrong reason.
- **Kill by exact PID.** `pkill -f "..."` matched its own wrapper shell and
  killed it — exit 144 — twice.
- **Background task output files stay empty until the command completes.**
  Reading one immediately tells you nothing.
- **`gh` in this repo resolves to olly, not molly.** There is an `upstream`
  remote pointing at `locodarwin/olly`, and `gh` prefers it, so a bare
  `gh run list` here lists *olly's* workflow runs — the newest of them from
  2026-10-09, which reads exactly like molly's CI having not run. Every run id
  in this file 404s against that resolution. Pass `-R locodarwin/molly`.
- **A `make test` put in the background with `&` is not a tracked task.** The
  shell the tool runs detaches it, so the "completed" notification arrives
  seconds later and describes the launch, not the run — and the run is still
  going. Have the command echo its exit code into the log and wait on that:
  `until grep -q '^EXIT' log; do sleep 15; done`.
- **Check what is still running.** A stale ten-seed fuzz loop ate CPU for about
  thirty-seven minutes unnoticed.
- **`pkill -f tests/fake_lsp.py` kills more than you meant.** Besides its own
  shell, it killed a golden run's fake server mid-case, which failed
  `lsp-diagnostics-256` with "python3 exited". Rerun the one case
  (`GOLDEN_ONLY`) rather than trusting or regenerating it.
- **`tests/fake_lsp.py` is read live by a running suite.** Editing it while a
  gate runs changes what later LSP cases talk to; keep such edits additive or
  wait for the gate.
- **`make test-oom` depends on `molly`**, so running it after editing `src/`
  relinks the binary from the new sources. Run it on a `git checkout-index`
  export of what is staged when the working tree has moved on.

---

## Running it live

```bash
make            # cc -std=c11, per-module .o, no dependencies
./molly FILE
make test       # the gate
make test-oom   # allocation-failure sweep, needs cc at runtime
make golden     # re-run the golden screens alone
MOLLY_UPDATE_GOLDEN=1 python3 tests/test_golden.py ./molly   # rewrite snapshots
```

Rewrite snapshots only when the change to the output is intended, and read the
resulting diff.

Two more hooks, both documented in `MANUAL.md` and both cheaper than a pty:

```bash
MOLLY_DUMP_CONFIG=1 ./molly    # the parsed file, every setting, every warning
MOLLY_DUMP_KEYMAP=1 ./molly    # every binding, with [keys] already applied
MOLLY_CONFIG=/path/to/rc ./molly   # read that file instead; empty = read none
```

The test harness pins `MOLLY_CONFIG` empty, so the suite never reads your own
`mollyrc`. If you are debugging a config problem through the tests, set it
through `env=` rather than exporting it.

What to try first, since these are the new surfaces: **Ctrl-F** for the Find
dialog (Tab between field, the two checkboxes and the buttons, Space toggles),
**Alt-R** to read the query as a pattern, **Ctrl-Space** (or Alt-X) for the palette,
**Alt-F** and friends for the menus, **F1** for help, **Ctrl-A** / **Ctrl-L** /
**Ctrl-T** for the new CUA keys, **F3** / **Shift-F3** to walk matches, and
**View ▸ Save Settings** then a restart to watch a setting survive. For the
newest surfaces: `molly src/*.c` for the tab bar, **Ctrl-PgUp**/**Ctrl-PgDn** and
a click on a tab to move between files, **Ctrl-N**/**Ctrl-W** for new and close,
**Alt-B** to hide the tabs, and a narrow terminal to see them scroll. For
highlighting: `molly src/syntax.c`, then **Alt-Y** to turn the colour off and on,
and `molly MANUAL.md` for the one language with no keywords in it.
For the filter: `molly MANUAL.md`, then **Alt-|** and `wc -l` to see the whole
file replaced by its own line count (and **Ctrl-Z** to get it back in one step);
select a few lines with **Shift-Down** and send them through `sort`; try `jqq`
to see a command that does not exist report itself; and `sleep 30` to watch the
timeout stop it after ten seconds, or **Esc** to stop it yourself.
For splits: `molly src/window.c`, then **F7** and **F8** to cut the screen up,
**F6** to walk the windows, **Ctrl-U** in one of them to see that the gutter is
the window's and not the screen's, and **F9** to put it back. `molly src/*.c`
then **F7** and **Ctrl-PgDn** puts a different file in each half. Resize the
terminal under a split to watch the cut keep its proportion, and shrink it past
17 columns to see a split refused rather than drawn.
For the menus at their new height: open **Alt-E** on a terminal twelve or
fourteen rows tall and watch the list scroll under the arrows, with the status
bar still readable under the box; the same menu at 24 rows shows all sixteen of
its rows at once. **Edit ▸ Indent**, **Edit ▸ Dedent** and **View ▸ Soft wrap**
are the three items that were waiting for it.
`MANUAL.md` has the regular-expression syntax table, what is not supported, what
a search matches against, and the configuration reference.
