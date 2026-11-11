# Molly's Design

Molly is olly's big sister: olly's code and feel, with the ceiling lifted. It is
modeled on [Fresh](https://github.com/sinelaw/fresh), a terminal editor that
behaves like a GUI one, and aims at that experience in C while keeping olly's
zero runtime dependencies and instant start.

This file says why molly is built the way it is, what it decided where there was
a choice, and what it has ruled out. [`MANUAL.md`](MANUAL.md) says what it does.

## The premise

Molly should never become slow to start, dependent on a runtime, or impossible
to use without reading the manual first. Features are welcome; the feel stays
the same. Every proposal is weighed against that sentence first, and the one
feature it ruled out entirely (a plugin runtime, below) was ruled out by it.

## One command table

A menu entry needs a label, an underlined letter, a shortcut drawn beside it, an
enabled state and an action. A `switch` on keypresses can give none of those,
so every action in molly is a record in one table, with a stable name
(`search.regex`) that the config file and macros use.

The menu bar, the keymap, the command palette, macros and the help screen are
all views of that table, so a command and its binding exist in exactly one
place. A command that changes text is flagged `CMD_EDITS`, and one that needs
the whole file in memory `CMD_LOADED`; `cmd_run()` refuses them where they
cannot apply (a terminal, a read-only buffer, a file shown from the disk).

## A cell grid, and a widget toolkit

Drop-down menus and dialogs paint over the text and must restore what was
beneath. olly's renderer kept one string per screen line and could not express
that, so molly draws into a grid of cells (character, width, colors,
attributes), which every element writes into and which is diffed against the
previous frame. On top of it sits a small toolkit of framed windows in a layer
stack, with buttons, fields, checkboxes, lists, focus order and a modal loop.
The help screen, the open dialog and the prompt line were ported onto it, which
is how the toolkit proved itself.

A menu bar you cannot click is broken, so the mouse (SGR 1006) is part of the
shell, not an extra.

## Input: Alt, menus and Esc

Alt is asked to do two jobs: open menus (`Alt-F` for File) and run commands
(`Alt-Up` moves a line, `Alt-1` goes to a bookmark). The menu bar reserves only
its title letters, read off the menu table, and leaves every other Alt chord to
commands. `F10` also opens the menu bar.

`Alt-x` reaches the program as `ESC x`, which only timing tells apart from Esc
followed by `x`. Molly has an input layer of its own, with its own tests, that
waits briefly after a bare ESC; Esc means cancel everywhere. Anything that polls
for Esc while it works (a search, a filter, opening a big file) must survive
stdin at end of file and must not drop the other keys typed meanwhile.

## Keys: CUA, and every binding remappable

Fresh is CUA-style, and olly already agreed on most of it (`Ctrl-S`, `O`, `Q`,
`F`, `R`, `G`, `Z`, `Y`, `C`, `X`, `V`, and Shift with the arrows to select).
Four of olly's keys disagreed, and were settled like this:

| Key | olly | Molly |
| --- | --- | --- |
| `Ctrl-L` | repaint | select the line; repaint moved to `F5` and the View menu |
| `Ctrl-T` | toggle case-sensitive search | transpose; case is a checkbox in the Find dialog |
| `Ctrl-N` | find next | New; `F3` and `Shift-F3` find next and previous |
| `Ctrl-?` | help | still help, with `F1` the primary |

Two of those improved the interface: a visible Match case checkbox beats a
hidden toggle with a status-bar message.

What a terminal can carry decides the rest:

- **Shift and Ctrl ride only on the CSI forms**, which are the function keys
  and the navigation keys. `Ctrl-Shift-L` sends the same byte as `Ctrl-L`, which
  is why repaint went to `F5` rather than there. So `shift-f3`, `shift-right`
  and `ctrl-pgup` are bindable, and `shift-a` and `ctrl-shift-l` are refused
  when the config is read, rather than accepted and never fired.
- **`Ctrl-/` is the same byte as `Ctrl-?`**, which is help, so toggling a
  comment is `Alt-;`, as in Emacs.
- **A shifted movement extends the selection.** That holds for every movement at
  once and is not a binding: the dispatcher tries the key with Shift first and
  the bare key second.

`Ctrl-W` closes the buffer and `Ctrl-N` makes a new one, because that is what
they mean in every editor and browser with tabs. Anyone who disagrees is one
line of `mollyrc` away.

Every binding is remappable. No key is handled by a hardcoded `switch`: the
keymap maps a chord to a command name, and the user's config beats the built-in
keymap. Only the menu titles' Alt letters are fixed, since they are structural
to the menu bar.

## Decisions

| Question | Decision |
| --- | --- |
| Command access | Ctrl **and** Alt keys, plus a menu bar on Alt |
| Platforms | **Linux.** Windows possibly later; **no macOS** |
| Plugin runtime | **No.** It would end zero dependencies and instant start |
| Config file syntax | **INI-style sections**, `key = value`: small to parse and to write back |
| Keymap | CUA on the four conflicts, and **fully customizable** |
| Regex | **A hand-rolled PCRE subset**, with a step budget and a fuzz target |
| What a search matches | **The row as stored**, for plain searches and patterns alike (see below) |
| Syntax languages shipped | **Rust, C, C++, C#, Python, AutoIt (`.au3`) and Markdown**; any other is a `[syntax <name>]` block |
| Where a per-filetype setting lives | **In its `[syntax <name>]` block**, the block that already names the language |
| Tabs versus spaces default | **A literal tab**, because the setting decides what bytes a save writes |
| Auto-indent default | **On**, because it changes only what a keystroke does |
| `Enter` inside leading whitespace | **The text keeps its column**: only the whitespace behind the cursor is copied |
| What a dedent takes off | **Back to the previous tab stop**, whole characters off the end of the whitespace run nearest the text; a tab that straddles the stop comes off whole |
| A blank row inside an indented block | **Left alone**: an indent on nothing is trailing whitespace |
| When `Backspace` eats a level | **Only in the leading whitespace, and only with more than one character to take**, so a tab-indented file never notices |
| Where indent and dedent live | **Edit ▸ Indent and Edit ▸ Dedent**, plus the palette, the help screen and the keymap |
| What a drop-down does with a short terminal | **Scrolls, inside the text area.** The box and its shadow stay above the status bar, an arrow in the border says there is more that way, and the window moves only as far as the highlight makes it |
| Where a wrapped line breaks | **After the last space that fits**, and at the window edge for a word with no space in it |
| The space it broke after | **Stays on the row above.** Slices have to cover every column of a row, or a cursor in a trimmed run would have no cell to sit in |
| The column a line breaks at | **Belongs to the row below**, so a cursor there is drawn at the start of the continuation; a step into a shorter row stops on its last column |
| What `↑`/`↓` keep | **The screen column when wrapped, the byte offset when not.** Between two slices of one line the byte offset is a windowful away, and changing the unwrapped answer would move behavior no one asked about |
| What `Home`/`End` reach | **The line, not the display row**: the file's shape is what those two keys are about |
| A wrap column | **No.** Lines wrap at the window's width; a narrower wrap is a narrower window |
| A hanging indent on continuations | **No**: a continuation starts at the left edge |
| `Ctrl-Up`/`Ctrl-Down` with wrap on | **Still file rows.** A column of cursors is about lines of code lining up in the file |
| Whose setting wrap is | **The window's**, like the gutter and the colors: a split can wrap one half and not the other |
| Where the wrap toggle lives | **`Alt-Z`, which is VS Code's**, plus the palette, the help screen and View ▸ Soft wrap, ticked when it is on |
| What Ctrl-D matches | **Literal text, case-sensitively**, and not through the search code; the Find dialog's modes must not change what it finds |
| Skip a match / drop a cursor / all matches | **Alt-D, Alt-U, Alt-A**: a terminal cannot carry `Ctrl-K Ctrl-D`, and `Ctrl-U` is the gutter |
| Which cursor those three work from | **The one added last**, which after a wrap is not the last one in the file, so a cursor carries the order it was added in |
| Cursors after an undo | **One.** An undo record restores a position, not a list |
| Where the multi-cursor commands live | **The palette and the help screen, not the Edit menu**: they are keyboard operations |
| The buffer | **Line spans over the file as read, not a piece table** (see below) |
| Files larger than memory | **Shown read-only from the disk**, past half of the memory available |
| Language servers | **Found on `PATH`, silent when missing**, one per command line (so C and C++ share clangd), and started only once the project is trusted |
| Server answers | **Through a mailbox the main loop opens**, never acted on where they arrive |
| Integrated terminal | **A terminal is a buffer**; keys go to the program except the ways out, which are found by command; exit closes it |

### What a search matches

olly searched the tab-expanded text on screen, so a run of eight spaces found a
tab. Molly searches the stored bytes in both modes, so a search matches exactly
the characters asked for. A tab is named `\t` in a pattern, because a prompt
cannot take a Tab keystroke.

### The buffer

olly kept two copies of every line, the text and its tab-expanded rendering,
about 3.5 times a file's size in memory. Molly reads a file in one piece and
makes each row a span of that block, ended in place, so an unchanged row costs
no copy and a changed row gets one of its own. A piece table was the other
candidate, and was turned down: every one of molly's places that read text
reads a row as one contiguous, NUL-terminated string by index, and a piece table
would have made each of those a lookup that might copy, with a rule about how
long the pointer lives. A 743 MB, ten-million-line file opens in under half a
second in about 1.5 times its size.

A file bigger than half of the memory available is not read at all. Its rows
are read from the disk as they are asked for, into a cache of pages, with an
index of where one line in 1,024 starts. Line numbers stay line numbers, so the
cursor, selection, splits, bookmarks and history work unchanged; what needs
every row in memory (editing, saving, syntax, wrap, folding, cursors at matches,
the language server, the git gutter) is off there. Reads are `pread`, not a
mapping, so a log truncated underneath shortens rather than killing molly.

## Ruled out

- **A plugin runtime.** Fresh ships TypeScript plugins in a sandboxed engine,
  and it is how an editor grows features its maintainer did not write. But
  embedding a JavaScript engine ends zero dependencies and instant start in a
  single commit. Molly's answer is the command table, remappable keys, syntax
  rules in the config and the shell filter, which together cover much of what
  people write plugins for.
- **Coding agents or a workspace orchestrator**: a separate application, not an
  editor feature.
- **Markdown preview, custom backgrounds and translations**: out of character
  for the effort.
- **A GUI, a daemon**, or anything that cannot work on a bare terminal over SSH.
- **macOS.**

## Possible later

None of these is promised; each would be weighed against the premise first.

- **Editing a file larger than memory.** This needs the edits kept apart from
  the file and a save that streams it.
- **Following a file as it grows**, as `tail -f` does.
- **Windows.** No Windows code exists, but every platform call stays behind
  `term.c` and `sys.c` so that a port is a module rather than a hunt. What will
  not port: raw mode and `SIGWINCH` (`SetConsoleMode` and a resize event in the
  input queue instead), the integrated terminal (ConPTY instead of `forkpty`,
  the biggest piece), an atomic save over an existing file (`MoveFileEx` with
  `MOVEFILE_REPLACE_EXISTING`, and a different meaning for mode and owner),
  recovery on a signal (a console control handler), and `poll` on a console
  handle (`WaitForMultipleObjects`), which the language server and the
  terminal both depend on.
- **Multi-key chords** such as `Ctrl-K Ctrl-F`, which would make a chord a list
  rather than a single key. Nothing needs one yet.
- **Named keymaps**, so that an olly-compatible set could ship beside the CUA
  default. `[keys]` in `mollyrc` already makes olly's four keys four lines.
