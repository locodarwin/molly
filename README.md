# Molly

A feature-rich, dependency-free text editor for the terminal, written in C11.

Molly is the big sister to [olly]: same modeless, start-typing-and-you're-editing
feel, same zero external dependencies, but without olly's self-imposed ceiling.
Where olly deliberately stops — one file, one cursor, no regex, no config — the
model is [Fresh](https://github.com/sinelaw/fresh): a terminal editor that
behaves like a GUI one — menu bar, mouse, dialogs, splits, discoverable everything — built on
olly's base in C, with no runtime dependencies.

**Status: early, but no longer only foundations.** The groundwork is done — the
editor is split into modules, the screen is a cell grid, every action is a named
command reached through a keymap, and the event loop can wait on a child process
as well as the keyboard — and the features it was groundwork *for* have started
landing: a configuration file with a rebindable keymap, any number of files open
at once with a tab bar, syntax highlighting for seven languages, window splits,
a shell filter, per-filetype indentation, multiple cursors and soft wrap. 1,642
checks pass, including golden screenshots taken before the renderer was replaced
and still compared against today. What comes next, and why, is in
[`ROADMAP.md`](ROADMAP.md).

[olly]: https://github.com/locodarwin/olly

## Inherited features

Everything olly does, molly does. These all work today:


- **Modeless editing**: type to insert, arrow keys to move, `Ctrl` keys act.
- **Search, with a case-sensitivity toggle**, that wraps around the file and
  remembers the last term:
  - `Ctrl-F` search, `F3` next match, `Shift-F3` previous match, or just
    press `Ctrl-F` again with an empty query to repeat.
  - The Find dialog's **Match case** checkbox switches between
    case-insensitive (the default) and case-sensitive matching.
- **Search and replace**: `Ctrl-R` replaces the next match or every match,
  and a whole replace-all undoes or redoes as one step.
- **Find in files**: `Ctrl-E` runs `git grep` (or `grep -r` outside a
  repository) and lists the matches in a window; `Enter` opens one, `F4` and
  `Shift-F4` step through them from anywhere.
- **Go to line**: `Ctrl-G` jumps straight to a line number.
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
  renamed into place, so an interrupted save never leaves it truncated. Line
  endings (`LF` or `CRLF`), a missing final newline, and the file's mode and
  owner are all preserved. If the file changed on disk since it was opened,
  Molly asks before overwriting it.
- **UTF-8-aware editing**: the cursor, `Backspace` and `Delete` move over
  whole characters, text round-trips as raw bytes, and display width is
  honored so wide (CJK) and combining characters stay aligned on screen.
- **Crash recovery**: if a signal or an out-of-memory condition kills Molly
  with unsaved changes, the buffer is written to a `.molly-recover` file.
- **Open anything**: never errors on a missing file; it simply starts an
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
- **Soft wrap**: `Alt-Z` lays a line too long for the window out over as many
  screen rows as it needs, breaking it after the last space that fits. The
  arrows walk the rows of a long line, the gutter numbers it once, and nothing
  in the file changes — it is a way of showing a line, not of rewriting it.
  Off by default, and a property of the window, so a split can wrap one half.
- **Indentation that follows the filetype**: `Enter` starts the new line under
  the old one, `Tab` types a tab or spaces to the next tab stop, and the tab
  width, the choice of character and auto-indent are each settable for
  everything in `[editor]` or for one language in `[syntax <name>]`.
  `Edit ▸ Tabs to spaces` and `Spaces to tabs` convert what a file already has.
- **Filter through a shell command**: `Alt-|` pipes the selection — or the whole
  file — through `sort`, `jq`, `fmt`, `black` or any pipeline, and replaces it
  with the output. One undo step, with a timeout, an output cap and `Esc` to
  cancel; a command that fails leaves your text alone. `Alt-!` inserts a
  command's output at the cursor instead — `date`, `git rev-parse HEAD`.
- **Keyboard reference built in**: press `F1` at any time.
- **Configurable**: `~/.config/molly/mollyrc` holds the settings and the
  keymap; `View ▸ Options…` changes any setting in place, and
  `View ▸ Save Settings` writes the file for you.
- No runtime dependencies. Compiles with any C11 compiler.

## Install

Just run:

```sh
make
```

Then put it somewhere on your `PATH`:

```sh
make install
```

Its default install location is `/usr/local/bin`; if that directory isn't
writable (a very common situation), `make install` detects this and falls
back to `~/.local/bin` automatically - no `sudo` needed. You can override the
target with `make install PREFIX=/some/dir` or stage a package with
`make install DESTDIR=/wherever`. `make uninstall` to remove it.

## Usage

```sh
molly                 # start with an unnamed buffer
molly file.txt        # open an existing file
molly new.txt         # open a buffer for a file that doesn't exist yet
molly file.txt +42    # open with the cursor on line 42 (+42 file.txt works too)
molly --version       # print the version and exit
```

Edit immediately. Every key you type is inserted at the cursor. Save with
`Ctrl-S`, search with `Ctrl-F`, select with `Shift` + the arrows, and press
`F1` for a key reference. Keep an eye on the status bar: it shows the file
name, line count, modified state, and the cursor as `Ln <line>, Col <column>`
(plus a `Sel <bytes>` count while text is selected). The display — including the
help screen, which reflows between one and two columns — resizes live as you
resize the terminal window.

## Keyboard shortcuts

| Key | Action |
| --- | --- |
| `Ctrl-S` | Save |
| `Ctrl-O` | Open a file (directory browser) |
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

The full write-up — including the grouped-undo rules, the fallback install
details, and the file-format notes — is in [`MANUAL.md`](MANUAL.md).

## Design

Molly keeps the parts of olly's discipline that cost nothing and drops the ones
that capped it:

**Kept:**

- **No runtime dependencies.** C11 and a terminal. Nothing to install, nothing
  to version-pin, nothing that rots.
- **Modeless by default.** Type to insert. Nothing to learn before you can edit,
  and no way to lose work by forgetting which mode you are in.
- **Instant start and careful saves.** Atomic writes, preserved line endings and
  file modes, crash recovery, dirty-quit protection.
- **Discoverable.** Anything molly can do should be findable from inside molly.

**Dropped (these were olly's limits, and they are molly's job):**

- The single-source-file rule. Molly is several files and a real build.
- One file, one cursor, one window.
- No regex, no configuration, no syntax awareness.
- Ctrl-keys only: molly uses Alt as well, and puts a menu bar on it.

What molly must *not* become: slow to start, dependent on a runtime, or
impossible to use without reading the manual first. The features change; the
feel should not.

For what is planned, in what order, and what has been ruled out, see
[`ROADMAP.md`](ROADMAP.md).


## Acknowledgements

Molly is a fork of [olly], which builds on the architecture of the classic
[kilo] editor by antirez. Both are credited in [`NOTICE`](NOTICE.md).

[kilo]: https://github.com/antirez/kilo


## License

MIT — see [`LICENSE`](LICENSE).
