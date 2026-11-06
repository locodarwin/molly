<h1 align="center">Molly</h1>

<p align="center">
  <b>A modeless, dependency-free text editor for the terminal, written in C11.</b><br>
  Menus, mouse, splits, multiple cursors, an integrated terminal, git and
  language servers, with nothing to install but Molly itself.
</p>

<p align="center">
  <a href="#quick-start">Quick start</a> ·
  <a href="#features">Features</a> ·
  <a href="#keys">Keys</a> ·
  <a href="MANUAL.md">Manual</a> ·
  <a href="ROADMAP.md">Roadmap</a>
</p>

---

Molly is the big sister to [olly]. It keeps olly's modeless editing (start
typing and you are editing) and its zero external dependencies, but not olly's
self-imposed limits of one file, one cursor, no regex and no config. It is
modeled on [Fresh](https://github.com/sinelaw/fresh), a terminal editor that
behaves like a GUI one: a menu bar, the mouse, dialogs, splits, and everything
discoverable from inside the editor.

> **Status: early.** The groundwork is done and features are landing fast.
> What comes next, and why, is in [`ROADMAP.md`](ROADMAP.md).

## Quick start

```sh
make              # build
make install      # install to /usr/local/bin, or ~/.local/bin without sudo
molly file.c      # open a file
```

Start typing; every key goes in at the cursor. Save with `Ctrl-S`, search with
`Ctrl-F`, and select with `Shift` and the arrows. `F10` or a click opens the
menu bar, `Ctrl-Space` lists every command, and `F1` shows the keys.

```sh
molly                 # an unnamed buffer
molly new.txt         # a file that doesn't exist yet opens empty
molly one.c two.c     # several files, one tab each
molly file.txt +42    # cursor on line 42 (+42 file.txt works too)
molly --version       # print the version and exit
```

## Features

### Editing

- **Modeless**: type to insert, arrows to move, `Ctrl` and `Alt` keys act.
- **Grouped undo and redo**: a run of typing, a paste or a whole replace-all
  undoes as one step.
- **Multiple cursors**: `Ctrl-D` adds the next occurrence of the word,
  `Ctrl-Up`/`Ctrl-Down` build a column, `Alt`-click puts one anywhere. Typing,
  deleting, `Enter`, `Tab` and paste reach every cursor as a single undo step.
- **Block selection**: `Alt-Shift` and the arrows, or `Alt`-drag, select a
  rectangle; a copied block pastes back as a block.
- **Words and lines**: move and delete by words; duplicate, move, join, comment,
  sort, trim and change the case of lines. Smart `Home` and auto-closing
  brackets and quotes are settings.
- **Indentation that follows the filetype**: tab width, tabs or spaces and
  auto-indent are set for everything or per language, and existing files
  convert either way from the Edit menu.
- **Keyboard macros**: `Alt-(` records and `Alt-)` plays back typing, commands,
  prompts and dialogs.
- **UTF-8 throughout**: the cursor moves over whole characters, wide (CJK) and
  combining characters stay aligned, and text round-trips as raw bytes.
- **System clipboard**: copy and cut also reach the terminal's clipboard
  through OSC 52, where the terminal supports it.

### Search and navigation

- **Find**, with match-case and regular-expression toggles, wrapping around the
  file. **Replace** the next match or all of them.
- **Find in files** with `git grep` (or `grep -r` outside a repository), with
  `F4` stepping through the results from anywhere.
- **Back and forward** through every jump, across files; go to a line, the
  matching bracket, or one of ten bookmarks.
- **Folding** by indentation, **soft wrap** per window, and from the View menu
  whitespace, indent guides, the current line, the word under the cursor and a
  ruler.

### Files and buffers

- **Any number of files** in a tab bar, each with its own cursor and undo
  history. **Window splits** side by side or stacked, resized with the keyboard
  or by dragging; two windows on one file are two views of the same text.
- **Open dialog** (`Ctrl-O`) to browse to a file, and **recent files**
  (`Alt-O`) for the last twenty.
- **Big files**: a 743 MB, ten-million-line log opens in under half a second,
  in about 1.5 times its size in memory.

### Programming tools

- **Syntax highlighting** for Rust, C, C++, C#, Python, AutoIt and Markdown.
  It is table-driven, so a `[syntax <name>]` block in the config teaches it
  another language.
- **Language servers**: with clangd, rust-analyzer or pylsp installed, Molly
  gives you diagnostics, go to definition, hover, completion, signature help,
  find references, rename, formatting and code actions. It asks once per
  project before starting a server, since a server may run the project's code.
  Any other server is one line in the config.
- **Git**: a sign column marks lines added, changed or deleted as you type;
  blame and file history are in the palette. Molly runs git with the
  repository's own programs (filters, fsmonitor, external diff) turned off.
- **Integrated terminal**: `Alt-T` opens your shell below the file. Colors,
  `less`, `vim` and `htop` draw as they would in xterm, with 5,000 lines of
  scrollback, the mouse, 24-bit color and bracketed paste.
- **Shell filters**: `Alt-|` pipes the selection or file through `sort`, `jq`,
  `fmt` or any pipeline; `Alt-!` inserts a command's output at the cursor. A
  failed command leaves the text unchanged.

### Safe with your files

- **Atomic saves** through a private temporary file. Where a rename would lose
  something (another user's ownership, a hard link, an ACL), Molly rewrites in
  place and says so.
- **Nothing lost in translation**: each line's ending (`LF` or `CRLF`), a
  missing final newline, and the file's mode and owner are preserved. A file
  changed on disk since it was opened is not overwritten without asking.
- **Crash recovery**: a signal or out-of-memory condition writes unsaved changes
  to a `.molly-recover` file, offered back the next time you open the file.
- **Safe with untrusted files**: control characters, malformed UTF-8 and the
  invisible characters that reorder right-to-left text show as a reversed `?`
  and never reach your terminal, a paste is only ever text, and the binary is
  built with the usual exploit mitigations.
- **Dirty-quit protection**: with unsaved changes, `Ctrl-Q` asks before it
  exits.

### Making it yours

`~/.config/molly/mollyrc` holds the settings and the keymap. `View ▸ Options…`
changes any setting in place and `View ▸ Save Settings` writes the file. Every
action is a named command, so any key can be rebound, and the command palette
(`Ctrl-Space`) lists them all.

## Keys

The essentials are below; `F1` inside Molly shows them all, and
[`MANUAL.md`](MANUAL.md) explains each one.

<table>
<tr><td valign="top">

**Files and buffers**

| Key | Action |
| --- | --- |
| `Ctrl-S` | Save |
| `Ctrl-O` | Open a file |
| `Alt-O` | Open a recent file |
| `Ctrl-N` | New buffer |
| `Ctrl-W` | Close the buffer |
| `Ctrl-PgDn` / `Ctrl-PgUp` | Next / previous buffer |
| `Ctrl-Q` | Quit |

**Windows**

| Key | Action |
| --- | --- |
| `F7` / `F8` | Split right / below |
| `F6` / `Shift-F6` | Next / previous window |
| `F9` | Close the window |
| `Ctrl-Shift-Arrow` | Move a border |
| `Alt-T` | Terminal |

**Search and jump**

| Key | Action |
| --- | --- |
| `Ctrl-F` | Find |
| `F3` / `Shift-F3` | Next / previous match |
| `Ctrl-R` | Replace |
| `Ctrl-E` | Find in files |
| `F4` / `Shift-F4` | Next / previous result |
| `Ctrl-G` | Go to line |
| `Alt-Left` / `Alt-Right` | Back / forward |
| `Ctrl-]` | Matching bracket |
| `Alt-J` *n* / `Alt-`*n* | Set / go to bookmark *n* |

</td><td valign="top">

**Editing**

| Key | Action |
| --- | --- |
| `Ctrl-Z` / `Ctrl-Y` | Undo / redo |
| `Ctrl-C` / `Ctrl-X` / `Ctrl-V` | Copy / cut / paste |
| `Ctrl-A` / `Ctrl-L` | Select all / the line |
| `Ctrl-D` | Add the next occurrence |
| `Ctrl-K` | Delete to end of line |
| `Alt-C` | Duplicate the line |
| `Alt-Up` / `Alt-Down` | Move the line |
| `Ctrl-J` | Join lines |
| `Alt-;` | Comment / uncomment |
| `Ctrl-T` | Swap two characters |
| `Alt-(` / `Alt-)` | Record / play a macro |
| `Alt-\|` / `Alt-!` | Filter / insert command output |

**Code**

| Key | Action |
| --- | --- |
| `F2` / `Ctrl-F2` | Next problem / all problems |
| `F12` / `Shift-F12` | Definition / references |
| `Alt-K` | What is this? |
| `Alt-/` | Complete |
| `Alt-N` | Rename |
| `Alt-Q` | Format |
| `Alt-Enter` | Code actions |

**View**

| Key | Action |
| --- | --- |
| `F1` | Help |
| `F10` | Menu bar |
| `Ctrl-Space` | Command palette |
| `Ctrl-U` | Line numbers |
| `Alt-Z` | Soft wrap |
| `Alt--` | Fold / unfold |
| `Alt-.` | Show whitespace |
| `Alt-Y` | Syntax highlighting |

</td></tr>
</table>

## Install

`make` builds Molly with any C11 compiler; there is nothing else to install.
`make install` copies it to `/usr/local/bin`, or to `~/.local/bin` when that
isn't writable, so no `sudo` is needed.

| Command | Does |
| --- | --- |
| `make install PREFIX=/some/dir` | Install somewhere else |
| `make install DESTDIR=/wherever` | Stage a package |
| `make uninstall` | Remove it |
| `make HARDEN=` | Build without exploit mitigations, for a compiler that lacks them |
| `make test` | Run the test suite |

## Design

Molly keeps the parts of olly's design that cost nothing and drops the ones
that limited it.

| Kept | Dropped |
| --- | --- |
| No runtime dependencies: a C11 compiler and a terminal are all it needs | The single-source-file rule: Molly is several files and a real build |
| Modeless by default: no modes to learn, none to forget you are in | One file, one cursor, one window |
| Instant start and careful saves | No regex, no configuration, no syntax awareness |
| Discoverable: anything Molly can do can be found from inside Molly | `Ctrl` keys only: Molly uses `Alt` too, and puts a menu bar on it |

Molly should never become slow to start, dependent on a runtime, or impossible
to use without reading the manual first. Features will be added; the feel
should stay the same. What is planned, in what order, and what has been ruled
out is in [`ROADMAP.md`](ROADMAP.md).

## Acknowledgments

Molly is a fork of [olly], which builds on the architecture of antirez's
classic [kilo] editor. Both are credited in [`NOTICE`](NOTICE.md).

## License

MIT. See [`LICENSE`](LICENSE).

[olly]: https://github.com/locodarwin/olly
[kilo]: https://github.com/antirez/kilo
