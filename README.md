# Molly

A feature-rich, dependency-free text editor for the terminal, written in C11.

Molly is the big sister to [olly]. It has the same modeless editing, where you
start typing and you are editing, and the same zero external dependencies, but
not olly's self-imposed limits. Olly stops at one file, one cursor, no regex and
no config. Molly is modeled on [Fresh](https://github.com/sinelaw/fresh), a
terminal editor that behaves like a GUI one (menu bar, mouse, dialogs, splits,
everything discoverable), and is built on olly's base in C, with no runtime
dependencies.

**Status: early.** The groundwork is done: the editor is split into modules, the
screen is a cell grid, every action is a named command reached through a keymap,
and the event loop can wait on a child process as well as the keyboard. Features
built on that groundwork have started landing: a configuration file with a
rebindable keymap, any number of files open at once with a tab bar, syntax
highlighting for seven languages, window splits, a shell filter, per-filetype
indentation, multiple cursors, soft wrap, a language-server client, an
integrated terminal, folding, macros and git marks. 2,612 checks pass, including
golden screenshots taken before the renderer was replaced and still compared
against today. What comes next, and why, is in [`ROADMAP.md`](ROADMAP.md).

[olly]: https://github.com/locodarwin/olly

## Inherited features

Molly does everything olly does. All of the following work today:


- **Modeless editing**: type to insert, arrow keys to move, `Ctrl` keys act.
- **Search, with a case-sensitivity toggle**, that wraps around the file and
  remembers the last term:
  - `Ctrl-F` search, `F3` next match, `Shift-F3` previous match, or
    press `Ctrl-F` again with an empty query to repeat.
  - The Find dialog's **Match case** checkbox switches between
    case-insensitive (the default) and case-sensitive matching.
- **Search and replace**: `Ctrl-R` replaces the next match or every match,
  and a whole replace-all undoes or redoes as one step.
- **Find in files**: `Ctrl-E` runs `git grep` (or `grep -r` outside a
  repository) and lists the matches in a window; `Enter` opens one, `F4` and
  `Shift-F4` step through them from anywhere.
- **Go to line**: `Ctrl-G` jumps to a line number.
- **Grouped undo/redo**: a run of typed (or deleted) characters, or a
  replace-all, undoes as one step. `Ctrl-Z` undo, `Ctrl-Y` redo.
- **Selection and clipboard**: hold `Shift` and use the arrows, `Home`/`End`
  or `PgUp`/`PgDn` to select (the covered text is highlighted in reverse video);
  `Ctrl-C` copies, `Ctrl-X` cuts and `Ctrl-V` pastes. Typing or deleting over a
  selection replaces it, and a cut or paste undoes as one step. Copy and cut
  also place the text on the system clipboard (OSC 52), where the terminal
  supports it.
- **Line-number gutter**: `Ctrl-U` toggles a right-aligned line-number gutter
  behind a gray `│` separator (off by default); the text area narrows to make
  room and the cursor, horizontal scroll and wide-character alignment all
  follow it.
- **Atomic saves**: the file is written to a private temporary file and
  renamed into place, so an interrupted save never leaves it truncated. Where a
  rename would lose something (a teammate's ownership, a hard link, an ACL) the
  file is rewritten in place instead and Molly says so, and it refuses a file you
  have no right to write. Each line's ending (`LF` or `CRLF`), a missing final
  newline, and the file's mode and owner are all preserved. If the file changed on
  disk since it was opened, Molly asks before overwriting it.
- **Safe with untrusted files**: control characters and malformed UTF-8 show as
  a reversed `?` and never reach your terminal, a paste is text and cannot run a
  command, and only regular files open. The binary is built with the usual exploit
  mitigations; `make HARDEN=` turns them off for a compiler without them.
- **UTF-8-aware editing**: the cursor, `Backspace` and `Delete` move over
  whole characters, text round-trips as raw bytes, and display width is
  honored so wide (CJK) and combining characters stay aligned on screen.
- **Crash recovery**: if a signal or an out-of-memory condition kills Molly
  with unsaved changes, the buffer is written to a `.molly-recover` file.
- **Open anything**: a missing file is not an error; Molly starts an
  empty buffer.
- **Open-file dialog**: `Ctrl-O` opens a directory browser so you can browse
  to a file instead of relaunching; it replaces the current buffer (unsaved
  changes are refused first).
- **Dirty-quit protection**: when there are unsaved changes, `Ctrl-Q` asks
  several times before exiting (counted down in the status bar).
- **Any number of files open at once**, with a tab bar: `molly one.c two.c`,
  `Ctrl-N` for a new buffer, `Ctrl-W` to close one, `Ctrl-PgUp`/`Ctrl-PgDn` or a
  click on a tab to move between them. Each keeps its own cursor position and
  undo history.
- **Syntax highlighting** for Rust, C, C++, C#, Python, AutoIt and Markdown,
  table-driven, with a `[syntax <name>]` block in the config file enough to
  teach it another language. Comments are italic where the terminal draws
  italic properly.
- **Window splits**: `F7` puts a new window to the right, `F8` below, `F6`
  walks them and `F9` closes one. `Ctrl-Shift` and an arrow moves a border, and
  so does dragging it. Each window has its own cursor, scroll position, gutter
  and status bar, and two windows on one file are two views of the same text.
- **Multiple cursors**: `Ctrl-D` selects the word under the cursor and then
  each next occurrence, `Ctrl-Up`/`Ctrl-Down` build a column, `Alt`-click puts
  one anywhere, `Esc` goes back to one. Typing, deleting, `Enter`, `Tab` and
  paste all reach every cursor, as a single undo step.
- **Block selection**: `Alt-Shift` and the arrows, or `Alt`-drag, select a
  rectangle in screen columns. It is a cursor per row, so typing writes on
  every row; a copied block pastes back as a block.
- **Big files**: a file is read in one piece and its lines are not copied until
  they change, so Molly needs about 1.5 times a file's size. A 743 MB,
  ten-million-line log opens in under half a second.
- **Soft wrap**: `Alt-Z` lays a line too long for the window out over as many
  screen rows as it needs, breaking it after the last space that fits. The
  arrows walk the rows of a long line, the gutter numbers it once, and the file
  itself is not changed. Wrapping is off by default and set per window, so one
  side of a split can wrap while the other does not.
- **Indentation that follows the filetype**: `Enter` starts the new line under
  the old one, `Tab` types a tab or spaces to the next tab stop, and the tab
  width, the choice of character and auto-indent are each settable for
  everything in `[editor]` or for one language in `[syntax <name>]`.
  `Edit ▸ Tabs to spaces` and `Spaces to tabs` convert what a file already has.
- **Filter through a shell command**: `Alt-|` pipes the selection (or the whole
  file) through `sort`, `jq`, `fmt`, `black` or any pipeline, and replaces it
  with the output. One undo step, with a timeout, an output cap and `Esc` to
  cancel; a failed command leaves the text unchanged. `Alt-!` inserts a
  command's output at the cursor instead, such as `date` or
  `git rev-parse HEAD`.
- **An integrated terminal**: `Alt-T` opens your shell in a window below the
  file and switches back and forth between the two. Colors, `less`, `vim`,
  `htop` and the rest draw as they would in xterm; `Shift-PgUp` scrolls back
  5,000 lines; paste goes in bracketed. Every key goes to the program except the
  ones that get you out: window and buffer switches, the menu bar, the palette
  and quit. When the shell exits, its window closes.
- **Language servers**: open a C, C++, Rust or Python file with clangd,
  rust-analyzer or pylsp installed and molly starts it; if the server is not
  installed, molly says nothing. Problems are underlined in the text and counted
  on the status bar, `F2` walks them and `Ctrl-F2` lists them all, kept up to
  date; `F12` goes to a definition and `Alt-Left` comes back; `Alt-K` shows what
  a name is; `Alt-/` completes the word, and typing `.` asks for completions
  too; `Shift-F12` lists every use; `(` shows the signature you are filling in;
  `Alt-N` renames across files, leaving them unsaved to review; `Alt-Q` formats.
  Any other server is one `language server =` line in the config. The JSON
  handling is written by hand, with no library. `Alt-Enter` lists the code
  actions (fixes and refactorings) at the cursor and applies the one you choose.
  Format on save and completion as you type can be turned on in the settings. A
  server is started for the file's own project, which molly finds from its build
  files.
- **Words and lines**: `Ctrl-Left`/`Ctrl-Right` move by words and
  `Alt-Backspace`/`Ctrl-Delete` delete them, `Ctrl-K` deletes to the end of the
  line; `Alt-C` duplicates lines, `Alt-Up`/`Alt-Down` move them, `Ctrl-J` joins
  them, `Alt-;` comments them out and back in, and sort, trim and case changes
  are in the Edit menu and the palette. Smart `Home` and auto-closing brackets
  and quotes, which also wrap a selection, are settings, off by default.
- **Getting back**: `Alt-Left`/`Alt-Right` walk back and forward through every
  jump (go to line, a find, a result, a definition), across files; `Ctrl-]`
  goes to the matching bracket; `Alt-J` and a digit sets one of ten bookmarks
  and `Alt-` with the digit goes back to it.
- **Seeing more**: whitespace (`Alt-.`), indent guides, the current line and
  the word under the cursor everywhere it appears, each from the View menu, and
  a ruler at a column from Options. `Alt--` folds a block by its indentation.
- **Keyboard macros**: `Alt-(` records and `Alt-)` plays back typing, commands,
  prompts and dialogs.
- **Git**: a sign column marks lines added, changed or deleted since the last
  commit, as you type; blame for a line and the file's history, each commit
  opening as its diff, are in the palette.
- **Recent files and read-only**: `Alt-O` reopens one of the last twenty files;
  a file you cannot write opens read-only, and View ▸ Read only toggles it.
- **More terminal**: drag to select and copy, find in the scrollback, the mouse
  for programs that ask for it, and 24-bit color passed through where your
  terminal can show it.
- **Keyboard reference built in**: press `F1` at any time.
- **Configurable**: `~/.config/molly/mollyrc` holds the settings and the
  keymap; `View ▸ Options…` changes any setting in place, and
  `View ▸ Save Settings` writes the file.
- No runtime dependencies. Compiles with any C11 compiler.

## Install

To build, run:

```sh
make
```

Then put it somewhere on your `PATH`:

```sh
make install
```

The default install location is `/usr/local/bin`. If that directory isn't
writable, `make install` falls back to `~/.local/bin`, so no `sudo` is needed.
You can override the target with `make install PREFIX=/some/dir` or stage a
package with `make install DESTDIR=/wherever`. `make uninstall` removes it.

## Usage

```sh
molly                 # start with an unnamed buffer
molly file.txt        # open an existing file
molly new.txt         # open a buffer for a file that doesn't exist yet
molly file.txt +42    # open with the cursor on line 42 (+42 file.txt works too)
molly --version       # print the version and exit
```

You can start typing right away; every key is inserted at the cursor. Save
with `Ctrl-S`, search with `Ctrl-F`, select with `Shift` + the arrows, and press
`F1` for a key reference. The status bar shows the file name, line count,
modified state, and the cursor as `Ln <line>, Col <column>` (plus a
`Sel <bytes>` count while text is selected). The display, including the help
screen (which reflows between one and two columns), resizes live with the
terminal window.

## Keyboard shortcuts

| Key | Action |
| --- | --- |
| `Ctrl-S` | Save |
| `Ctrl-O` | Open a file (directory browser) |
| `Alt-O` | Open a recent file |
| `Ctrl-N` | New buffer |
| `Ctrl-W` | Close this buffer |
| `Ctrl-PgDn` / `Ctrl-PgUp` | Next / previous buffer |
| `F7` / `F8` | Split, new window right / below |
| `F6` / `Shift-F6` | Next / previous window |
| `F9` | Close this window |
| `Ctrl-Shift-Arrow` | Move the border beside, above or below this window |
| `Ctrl-Q` | Quit (asks several times with unsaved changes) |
| `Ctrl-F` | Search forward |
| `F3` / `Shift-F3` | Find next / previous match (`Ctrl-N` / `Ctrl-P` too) |
| `Ctrl-R` | Search and replace (next or all) |
| `Ctrl-E` | Find in files |
| `F4` / `Shift-F4` | Next / previous find-in-files match |
| `Ctrl-G` | Go to line |
| `Alt-Left` / `Alt-Right` | Back / forward through the places you jumped from |
| `Ctrl-]` | Matching bracket |
| `Alt-J` then a digit / `Alt-0`…`Alt-9` | Set / go to a bookmark |
| `Ctrl-Left` / `Ctrl-Right` | Move a word |
| `Alt-Backspace` / `Ctrl-Delete` | Delete a word back / forward |
| `Ctrl-K` | Delete to the end of the line |
| `Alt-C` | Duplicate the line |
| `Alt-Up` / `Alt-Down` | Move the line up / down |
| `Ctrl-J` | Join lines |
| `Alt-;` | Comment / uncomment |
| `Alt-(` / `Alt-)` | Record / play a macro |
| `Alt--` | Fold / unfold the block |
| `Alt-.` | Show tabs and trailing blanks |
| `Alt-Enter` | Code actions from the language server |
| `Ctrl-A` | Select everything |
| `Ctrl-L` | Select the current line |
| `Ctrl-C` | Copy selection |
| `Ctrl-X` | Cut selection |
| `Ctrl-V` | Paste at the cursor |
| `Ctrl-T` | Swap the two characters at the cursor |
| `Alt-\|` | Filter the selection (or the file) through a command |
| `Alt-!` | Insert a command's output at the cursor |
| `Ctrl-Z` | Undo |
| `Ctrl-Y` | Redo |
| `F1` | Help screen (`Ctrl-?` too) |
| `Alt-Y` | Toggle syntax highlighting |
| `F5` | Repaint |
| `Ctrl-U` | Toggle line-number gutter |
| `Esc` | Cancel a prompt |
| Arrows / `Home` / `End` / `PgUp` / `PgDn` | Move around |
| `Shift` + Arrows / `Home` / `End` / `PgUp` / `PgDn` | Extend selection |
| `Enter`, `Backspace`, `Delete` | Edit (replaces any selection) |

[`MANUAL.md`](MANUAL.md) has the full details, including the grouped-undo
rules, the fallback install details and the file-format notes.

## Design

Molly keeps the parts of olly's design that cost nothing and drops the ones
that limited it:

**Kept:**

- **No runtime dependencies.** It needs a C11 compiler and a terminal, with
  nothing else to install or pin to a version.
- **Modeless by default.** Typing inserts text. There are no modes to learn
  before you can edit, and no risk of losing work by forgetting which mode you
  are in.
- **Instant start and careful saves.** Atomic writes, preserved line endings and
  file modes, crash recovery, dirty-quit protection.
- **Discoverable.** Anything molly can do should be findable from inside molly.

**Dropped (olly's limits, which molly is meant to go past):**

- The single-source-file rule. Molly is several files and a real build.
- One file, one cursor, one window.
- No regex, no configuration, no syntax awareness.
- Ctrl-keys only: molly uses Alt as well, and puts a menu bar on it.

Molly should not become slow to start, dependent on a runtime, or impossible to
use without reading the manual first. Features will be added, but the feel
should stay the same.

For what is planned, in what order, and what has been ruled out, see
[`ROADMAP.md`](ROADMAP.md).


## Acknowledgments

Molly is a fork of [olly], which builds on the architecture of the classic
[kilo] editor by antirez. Both are credited in [`NOTICE`](NOTICE.md).

[kilo]: https://github.com/antirez/kilo


## License

MIT. See [`LICENSE`](LICENSE).
