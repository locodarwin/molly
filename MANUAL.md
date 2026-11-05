# Molly - A Command-Line Text Editor

> **Note:** molly is at `0.1.0-dev`. Earlier versions of this note listed ten
> features it did not have: more than one file open at once, regular-expression
> search, syntax highlighting, a configuration file, window splits, a shell
> filter, multi-cursor editing, soft wrap, a language server and an integrated
> terminal. It now has all ten.
> This manual describes what is there today. For what is coming, and in what
> order, see [`ROADMAP.md`](ROADMAP.md).

Molly is a small, dependency-free text editor for the terminal, written in C.
It provides modeless editing in the style of tools like nano (type to insert,
the arrow keys move around, and the `Ctrl` combinations do the work) along with
the discoverability of a GUI editor: every command has a name, a menu entry
and a searchable label, so you never need to know a key to reach a command.

## Table of Contents

- [Building](#building)
- [Installing](#installing)
- [Getting Started](#getting-started)
- [Things Worth Knowing First](#things-worth-knowing-first)
- [The Menu Bar](#the-menu-bar)
- [The Command Palette](#the-command-palette)
- [The Mouse](#the-mouse)
- [Colors](#colors)
- [Syntax Highlighting](#syntax-highlighting)
- [Prompts](#prompts)
- [Editing](#editing)
- [Indentation](#indentation)
- [Moving Around](#moving-around)
- [Getting Back](#getting-back)
- [Help Screen](#help-screen)
- [Line Numbers](#line-numbers)
- [Soft Wrap](#soft-wrap)
- [Seeing More](#seeing-more)
- [Folding](#folding)
- [Selecting Text](#selecting-text)
- [Several Cursors](#several-cursors)
- [Block Selection](#block-selection)
- [Searching](#searching)
- [Regular Expressions](#regular-expressions)
- [Search and Replace](#search-and-replace)
- [Undo and Redo](#undo-and-redo)
- [Keyboard Macros](#keyboard-macros)
- [Opening Another File](#opening-another-file)
- [Several Files at Once](#several-files-at-once)
- [Filtering Through a Command](#filtering-through-a-command)
- [Finding in Files](#finding-in-files)
- [Window Splits](#window-splits)
- [The Terminal](#the-terminal)
- [Language Servers](#language-servers)
- [Git](#git)
- [Saving and Quitting](#saving-and-quitting)
- [Keyboard Shortcuts](#keyboard-shortcuts)
- [Configuration](#configuration)
- [Environment Variables](#environment-variables)
- [File Format Notes](#file-format-notes)

## Building

Molly builds with any C11 compiler and has no external dependencies.

    make

This produces the `molly` binary in the current directory.

## Installing

    make install

The default install location is `/usr/local/bin`. If that directory is not
writable by your user (a common situation), `make install` detects it and
falls back to `~/.local/bin`, which is usually already on your `PATH`, so no
`sudo` is required.

You can control where the binary goes:

- `make install PREFIX=/some/dir` installs to `/some/dir/bin`.
- `make install DESTDIR=/wherever` installs into a staging directory instead,
  useful for packaging.

To remove the installed binary:

    make uninstall

## Getting Started

    molly                      # start with an unnamed buffer
    molly file.txt             # open an existing file
    molly brand-new.txt        # open a file that does not exist yet (created
                              # empty; saved only when you save it)
    molly file.txt +42         # open with the cursor on line 42 (the number
                              # may also come before the file name, and a
                              # line past the end clamps to the last line)
    molly --version            # print the version and exit (-v works too)

Only one file is edited at a time; any further file arguments are ignored,
and the status bar reports that they were. To change files from inside Molly,
use `Ctrl-O` (see [Opening Another File](#opening-another-file)).

If the file does not exist, Molly opens an empty buffer.
A path that exists but cannot be read in full (a directory, for example) is
refused with an error instead of being opened as an empty or partial buffer
that a later save would write back over the original.
The status bar (the highlighted line at the bottom) shows the file name, the
number of lines, whether the buffer is modified, and the cursor position as
`Ln <line>, Col <column>`.

## Things Worth Knowing First

Four things about molly may surprise you.

**The mouse is on.** Molly asks the terminal for mouse reporting at startup, so
clicks, drags and the wheel go to molly instead of to your terminal. Your
terminal's own swipe-to-select and middle-click-to-paste stop working inside
molly. Hold `Shift` while dragging and most terminals handle the selection
themselves. This is the standard bypass, and it is how you copy text out of
molly with the terminal instead of with `Ctrl-C`. To turn molly's mouse
handling off, set `mouse = off` in the `[input]` section (see [The
Mouse](#the-mouse)).

**Alt keys need the terminal to send them as `Esc`-prefixed.** That is the
default in xterm, GNOME Terminal, Konsole, kitty, Alacritty, WezTerm and tmux.
If your terminal is set to "Alt sends the eighth bit" (sometimes called *8-bit
meta*) instead, `Alt-F` and similar keys will not reach molly. Everything they
do is also available from `F10` and the menus.

**`Ctrl-Space` may not get through.** It arrives as a NUL byte, which most
terminals send and a few swallow. For that reason, `Alt-X` also opens the
command palette.

**Every binding is remappable**, in the `[keys]` section of your
[configuration](#configuration) file, except the `Alt`-letter menu titles (seven
today, one per menu).
The bindings below follow CUA, the convention most editors of the last thirty
years use: `Ctrl-A` selects everything, `Ctrl-L` selects the line,
`Ctrl-T` transposes the two characters at the cursor, `F1` is help and
`F3`/`Shift-F3` step through matches. If you are used to olly's keys,
`Ctrl-L = view.repaint` puts repaint back.

## The Menu Bar

The band across the top of the screen holds seven menus: **File**, **Edit**,
**Search**, **View**, **Go**, **Window** and **Help**. (*Tools* and *Options*
come later; their `Alt` letters are already reserved, so adding them will not
move anything.)

| Key | Action |
| --- | --- |
| `F10` | Open the menu bar |
| `Alt-F` `Alt-E` `Alt-S` `Alt-V` `Alt-G` `Alt-W` `Alt-H` | Open that menu directly |
| `←` / `→` | Move to the previous / next menu |
| `↑` / `↓` | Move within the open menu |
| `Home` / `End` | First / last item |
| An underlined letter | Run that item |
| `Enter` | Run the highlighted item |
| `Esc` | Close without running anything |

Nine `Alt` letters are reserved: `F E S V G W H`, one for each menu, plus `T`
and `O` for the two menus still to come. Every other `Alt`+letter is free for
commands.

Every entry is built from molly's command table, so a menu cannot drift out of
step with the editor: the label, the shortcut shown down the right-hand side,
whether the item is available and whether it is ticked all come from the command
itself. An item molly cannot run right now (`Paste` with an empty clipboard,
`Find Next` before you have searched for anything) is grayed out, skipped by the
arrows, and ignores its own letter. Items that toggle something show a tick when
it is on.

**A menu taller than the screen scrolls.** The Edit menu needs twenty-two rows
and the View menu eighteen, more than a short terminal has. The box stays inside
the text area (neither it nor its drop shadow covers the status bar), and the
items that do not fit are scrolled instead of drawn off the bottom edge. A small
`▴` or `▾` on the border shows there is more in that direction; the arrows and
`Home`/`End` scroll the list as the highlight reaches the end of it, and clicking
a `▴` or a `▾` shows the next item past that edge. An item's underlined letter
runs it whether or not there was room to draw the item, so every command stays
reachable on a short terminal. (Below about eight rows, the box shrinks to a
border, one item and a border, and takes the status row if it must.)

**A terminal too narrow for the band shortens it instead of cutting it off.**
The seven titles need 43 columns. Below that, the gaps between them close to
one space, which works down to 37; narrower still, each title becomes its
underlined letter (` F E S V G W H`), which is also the `Alt` key that opens
it, and clicking a letter opens its menu. The open menu's title is written out
in full over its drop-down, so you can still see which one you are in.

Running a command from a menu behaves exactly like running it from its key. For
example, choosing **Quit** from the File menu with unsaved changes opens the
same dialog `Ctrl-Q` does.

The bar can be switched off with **View → Menu bar**, which gives its row back to
the text, a useful option on a 24-row terminal. While it is hidden, opening a menu
draws the band *over* the first line of the file instead of moving the whole
screen down a row.

## The Command Palette

`Ctrl-Space` (or `Alt-X`) opens a list of every command molly has, with a filter
box at the top and the key each one is bound to beside it. Use it to reach a
command without knowing its key or searching through the menus. Unlike the
menus, which show a curated selection, it lists every command.

| Key | Action |
| --- | --- |
| Type anything | Filter the list |
| `↑` / `↓`, `PgUp` / `PgDn` | Move the selection |
| `←` `→` `Home` `End` `Backspace` `Delete` | Edit the filter text |
| `Enter` | Run the selected command |
| Click a row | Run that command |
| `Esc` | Close without running anything |

The filter is fuzzy: the letters you type have to appear in order but need not be
adjacent, so `ln` finds **Line numbers** and `rep` finds **Replace…**. Matches
are ranked roughly by how closely they resemble a typed abbreviation: letters
that start a word count for more than letters in the middle of one, and a run of
adjacent letters counts for more still. The search also covers each command's
internal name and its group, so `clip` finds **Copy**, **Cut** and **Paste**
even though none of them says "clipboard".

Commands molly cannot run at the moment still appear in the list, dimmed.
Choosing one shows why it cannot run and leaves the palette open.

## The Mouse

| Action | Effect |
| --- | --- |
| Click in the text | Put the cursor there |
| Drag in the text | Select |
| `Alt` + click in the text | Add a cursor there |
| `Alt` + drag in the text | Select a block - see [Block Selection](#block-selection) |
| Click a line number | Go to the start of that line |
| Wheel | Scroll the view without moving the cursor |
| Click a menu title | Open that menu |
| Click a menu item | Run it |
| Click a menu's `▴` or `▾` | Scroll the open menu, when it is taller than the screen |
| Click outside a menu | Close it |
| Click a list row | Select it (in the file browser) or run it (in the palette) |
| Wheel over a list | Scroll it |
| `Shift` + drag | Hand the selection back to your terminal |

A selection made with the mouse is the same selection `Shift`+arrows make, so
`Ctrl-C` and `Ctrl-X` work on it the same way.

If you prefer your terminal's own click-and-drag behavior, put `mouse = off` in
the `[input]` section of your [configuration](#configuration) file. Molly then
never asks the terminal for mouse reports, and none of the table above applies.
Molly and the terminal cannot both handle the same mouse gesture.

## Colors

Molly draws in one of two palettes and picks between them at startup: a
**256-color** one if `COLORTERM` says `truecolor`/`24bit` or `TERM` mentions
`256color`, and a **16-color** fallback otherwise. The fallback uses a different
color table with the same renderer.

Dialogs and menus paint their own background instead of using the terminal's,
so they stand out as panels above the text and look the same wherever molly
runs. The text area itself still uses your terminal's own foreground and
background, so molly works with a light or a dark theme without any setting.

A 24-bit color from a program in the [terminal](#the-terminal) is passed through
unchanged only when `COLORTERM` says `truecolor` or `24bit`, which tells molly
the terminal can display it. Otherwise it is reduced to the nearest of the 256
colors, or of the sixteen.

If a dialog looks wrong, check what your terminal claims:

    echo "TERM=$TERM COLORTERM=$COLORTERM"

## Syntax Highlighting

Molly colors the text according to the file's type, which it works out from the
extension. Seven are built in:

| Language | Extensions |
| --- | --- |
| C | `.c` `.h` |
| C++ | `.cc` `.cpp` `.cxx` `.hh` `.hpp` `.hxx` `.ipp` |
| C# | `.cs` |
| Rust | `.rs` |
| Python | `.py` `.pyw` `.pyi` |
| AutoIt | `.au3` |
| Markdown | `.md` `.markdown` `.mdown` `.mkd` |

`.h` is the only extension both C and C++ claim; molly treats it as C.

When molly recognizes a file, the status bar names the language in brackets:

    [2/3] parser.c - 410 lines [c] (modified)

A file with no color either has no definition in molly or has highlighting
turned off, and the bracket tells you which.

`Alt-Y`, `View ▸ Syntax highlighting`, or `syntax = off` in the `[view]` section
of your [configuration](#configuration) file turn it off. A selection is always
drawn over the coloring, since reverse video on top of a keyword's color is hard
to read.

Comments are drawn in italic as well as in their own dimmer color, so they
read as comments even where your terminal's palette puts that color close to
the text's. Molly leaves italic off where `TERM` names a terminal that would
draw it badly: the Linux console, which paints italic in a substitute color,
and a `screen` or `screen-256color` TERM, under which tmux draws italic as
reverse video and a comment looks selected. If you run tmux,
`set -g default-terminal tmux-256color` gives you italic back. `italic = off` in
`[view]`, or **View ▸ Options…**, turns it off everywhere; the color stays.

### Teaching molly a language

A `[syntax <name>]` section in your [configuration](#configuration) file defines
a language, or adds to one molly already knows. Molly's built-in definitions use
the same rules, so yours can do anything they do.

```ini
[syntax lua]
extensions = lua
line comment = --
block comment = --[[ ]]
strings = " '
string escape = \
keywords = function end if then else for while do local return
types = string table math io pairs ipairs tostring true false nil
```

| Rule | What it takes |
| --- | --- |
| `extensions` | Extensions, without dots, that pick this language |
| `keywords` | Words colored as keywords |
| `types` | Words colored as type names - the second word class |
| `line comment` | One or more markers that start a comment to the end of the line |
| `block comment` | Pairs of open and close markers, for comments that span rows |
| `strings` | Single characters that delimit a string |
| `string escape` | The character that escapes a delimiter inside a string |
| `multiline strings` | Pairs, for string regions that span rows (Python's `"""`) |
| `numbers` | `on` or `off` - whether number literals are colored |
| `preproc` | A line-leading marker whose whole line is a directive |
| `case sensitive` | `off` makes keyword matching ignore case (AutoIt) |
| `heading` | A line-leading marker whose whole line is a heading (Markdown) |
| `fence` | Pairs that open and close a verbatim region (Markdown's triple backtick) |
| `code span` | A delimiter for inline verbatim text (Markdown's backtick) |
| `emphasis` | Single characters that delimit emphasis (Markdown's `*` and `_`) |
| `tab width` | Columns a tab occupies in this language's files, 1 to 16 |
| `insert spaces` | `on` or `off`, for this language only |
| `auto indent` | `on` or `off`, for this language only |
| `language server` | The command that starts this language's [server](#language-servers), or `none` |
| `language id` | The name the server protocol knows the language by, when it is not the section's (`csharp` for `cs`) |

A section naming a language molly ships adds to it, so

```ini
[syntax c]
types = mytype_t othertype_t
```

colors two more words in every `.c` file without disturbing anything else. A
rule molly does not recognize produces a warning naming the line, like any other
configuration mistake.

The `tab width`, `insert spaces` and `auto indent` rules are not about color:
they are the [indentation](#indentation) settings, set per language. A section
that leaves one out keeps whatever `[editor]` says, so

```ini
[editor]
tab width = 8
[syntax python]
tab width = 4
insert spaces = on
```

gives Python files four-column tabs and spaces, and leaves every other file
with eight-column tabs and a real tab character.

### What it deliberately does not do

- **An unterminated string stops at the end of its row.** This keeps a stray
  quote from coloring the rest of the file as a string.
- **It is not a parser.** Molly cannot tell whether a name is a function or a
  variable in any language; that would need a grammar per language instead of
  a table of rules.
- **Only the extension picks the language.** A `#!/usr/bin/env python` line on
  an extensionless script is not read.

## Prompts

Anything molly asks you on one line (a line number, a file name to save as, a
replacement) is an editable field on the bottom line, with a real cursor in it.
(Find asks for more than one thing at a time and so gets a dialog instead; see
[Searching](#searching).)

| Key | Action |
| --- | --- |
| `←` / `→` | Move within the answer |
| `Home` / `End` | Start / end of the answer |
| `Backspace` / `Delete` | Delete before / at the cursor |
| `Enter` | Accept |
| `Esc` | Cancel |

Pasting from your terminal only fills in the answer. Line breaks in it are
dropped and nothing in it accepts the prompt, so `Enter` is still yours to press.

An answer longer than the room for it scrolls sideways, so the hint at the end of
the line stays where it is. On a terminal too narrow for both, the hint is dropped
and the whole width goes to the answer. Molly has no prompt history yet.

## Editing

Molly is modeless: whatever you type is inserted at the cursor.

| Key | Action |
| --- | --- |
| Any character | Insert at the cursor |
| `Tab` | Insert one indent step, or indent the selected rows - see [Indentation](#indentation) |
| `Shift-Tab` | Dedent the selected rows, or the cursor's row |
| `Enter` | Split the line / insert a new line, keeping the indentation |
| `Ctrl-D` | Add a cursor at the next match - see [Several Cursors](#several-cursors) |
| `Backspace` | Delete the character before the cursor, or one indent level inside the indentation |
| `Delete` | Delete the character at the cursor |

If the cursor is on the empty row below the last line of text, typing creates
that row. If the cursor is at the start of a line, `Backspace` joins the
current line onto the previous one; `Delete` at the end of a line joins the
next line onto it.

### Words

| Key | Action |
| --- | --- |
| `Ctrl-←` / `Ctrl-→` | Move a word left / right (with `Shift`, select as you go) |
| `Alt-Backspace` | Delete the word before the cursor |
| `Ctrl-Delete` | Delete the word after the cursor |
| `Ctrl-K` | Delete from the cursor to the end of the line |

A word is a run of one kind of character: letters, digits, `_` and anything
past ASCII are one kind, punctuation another, so `foo->bar` is three stops.
Moving right goes over any blanks and then to the end of the run that
follows; moving left, back over blanks to the start of the run before. At the
end of a line the next stop is the start of the next line, as it is for `→`.
`Ctrl-K` at the end of a line joins the next one on. All of them work at every
cursor when there are several, and each is one undo step.

### Whole lines

| Key | Action |
| --- | --- |
| `Alt-C` | Duplicate the line (or the selected lines) below itself |
| `Alt-↑` / `Alt-↓` | Move the line (or the selected lines) up / down |
| `Ctrl-J` | Join the next line onto this one (or all the selected lines) |
| `Alt-;` | Comment the lines out, or back in |

**Edit ▸ Sort lines**, **Edit ▸ Trim whitespace**, and **Upper case** and
**Lower case** in the palette do the rest:

- **Sort** puts the selected lines in byte order (what `sort` gives with
  `LC_ALL=C`, an order that does not depend on the machine). It needs two
  lines or more selected.
- **Trim** takes the blanks off the ends of the selected lines, or of every line
  in the file without a selection.
- **Upper case** and **Lower case** change the selection, or the word under the
  cursor (ASCII letters only).

The lines an operation acts on are the selection's, where a selection that
ends at the start of a line does not include that line (the same rule as
`Tab`), or the cursor's own line without one. **Duplicate** leaves the cursor on
the copy, so pressing it again makes a third. **Join** turns the next line's
leading blanks into one space and leaves the cursor on that space, as vi's `J`
does. **Comment** uses the filetype's line comment (`//`, `#`, `--`, `;`): if
every non-blank line already starts with it, it comes off; otherwise it goes on,
at the shallowest indentation of the lines so the block stays lined up, with a
space after it. Blank lines are left alone either way.

Each of these is a single undo step. An operation that would leave the text as
it was, such as sorting a sorted block, changes nothing, so the file is not
marked modified. With several cursors they act on the main cursor's lines.

### Brackets, quotes and `Home`

Two settings, both off by default:

- **`[editor] smart home = on`** sends `Home` to where the line's text starts,
  and a second `Home` from there to column 0, as VS Code does. Off, `Home`
  goes to column 0.
- **`[editor] auto close = on`** types the closing half of `(`, `[`, `{`, `"`,
  `'` and `` ` `` along with the opening one, with the cursor between them.
  Typing the closer when it is already next to the cursor steps over it instead
  of typing a second; `Backspace` between an empty pair deletes both halves.
  A quote is not doubled inside a word, so `don't` stays as typed. Typing an
  opener with text selected wraps the selection in the pair instead of
  replacing it, and leaves the text selected, so `(` then `"` gives
  `("text")`.

Both are off by default because each changes familiar behavior: where `Home`
goes, and what typing a bracket puts in the file.

## Indentation

Three settings control indentation. Each can be set in `[editor]` for all files
or in `[syntax <name>]` for one language; see [Configuration](#configuration) and
[Teaching molly a language](#teaching-molly-a-language).

| Setting | Default | What it decides |
| --- | --- | --- |
| `tab width` | `8` | How many columns a tab character occupies on screen |
| `insert spaces` | `off` | Whether `Tab` types spaces instead of a tab |
| `auto indent` | `on` | Whether `Enter` starts the new line under the old one |

`tab width` affects only the display: changing it changes how a tab already in
your file is drawn and nothing else. `insert spaces` affects the file: turning
it on changes what `Tab` writes, and so the bytes a save puts on disk. It is off
by default so that a file indented with tabs does not quietly start gaining
spaces because you opened it in molly.

### What one `Tab` does

With `insert spaces` off, `Tab` types one tab character.

With it on, `Tab` inserts as many spaces as it takes to reach the next tab stop.
From column 0 of an eight-column stop that is eight spaces; from column 2 it is
six, so the text that follows lands on the stop either way. A tab already on the
row counts as the columns it occupies, not as one character, so rows indented
with a mix of tabs and spaces come out right.

The spaces are one undo step, so `Ctrl-Z` after a `Tab` removes all of them.

### Indenting a block

Select text that reaches into a second row and `Tab` indents every row the
selection touches instead of replacing it. `Shift-Tab` takes a level back off.
The selection survives both, so you can press `Tab` twice to indent twice.
**Edit ▸ Indent** and **Edit ▸ Dedent** are the same two commands from the
menu.

- A selection that stops at **column 0** of a row has not reached into that
  row, and that row is left alone. This lets you select whole lines by
  dragging down the left edge without indenting the line you stopped on.
- **Blank rows in the middle of a block are left alone**, since an indent on an
  empty row is trailing whitespace.
- With a selection on **one row only**, `Tab` replaces it.
- `Shift-Tab` needs no selection: on its own it dedents the row the cursor is
  on. With [several cursors](#several-cursors), it dedents each cursor's row.
- Either way the whole block is one undo step.

A dedent takes the row's indentation back to the **previous tab stop**, and it
removes whole characters from the end of the whitespace run nearest the text.
At a width of 4:

- `······alpha` (six spaces) becomes `····alpha`, not `··alpha`.
- `→··alpha` (a tab and two spaces) loses the two spaces.
- `→→alpha` loses the second tab.

A tab that straddles the stop is removed whole, which leaves the row short of
the stop instead of splitting the tab. Rows with no indentation are left as they
are, without an error.

### What `Backspace` does in the indentation

With the cursor in a row's leading whitespace and nothing but whitespace behind
it, `Backspace` deletes back to the previous tab stop instead of one space at
a time. One press takes a level off a row indented with spaces, instead of
four or eight.

Everywhere else, `Backspace` deletes one character. That includes the case
where only one character stands between the cursor and the stop, so in a
tab-indented file, where one level is one character, nothing changes.

### What `Enter` does

With `auto indent` on, the new line starts with the same leading whitespace as
the line you left: tabs as tabs, spaces as spaces, mixed as it was. The split
and the indentation are one undo step, so `Ctrl-Z` undoes both instead of
leaving an indented blank line behind.

Only the whitespace *behind the cursor* is copied, and the text that moves down
keeps its own. The text stays in the same column:

- `Enter` at the end of `····alpha` gives you a new line starting at column 4.
- `Enter` in the middle, at `····alpha|beta`, puts `beta` at column 4 too.
- `Enter` at column 0 copies nothing, so it inserts a blank line above the
  current one.
- `Enter` inside the indentation, at `··|··alpha`, leaves `alpha` at column 4
  where it was, on a row of its own.

Text that arrives from somewhere else is never re-indented: pasting
(`Ctrl-V`) and [filtering through a command](#filtering-through-a-command) both
insert their lines as they are, so code that is already indented does not come
in indented twice.

### What it deliberately does not do

- **It does not know your language.** The new line copies the old line's
  indentation; it does not add a level after a `{` or an `if`, and it does not
  take one away before a `}`. That would need a grammar per language instead of
  a rule per filetype.
- **It does not reindent a block to match its surroundings.** `Tab` and
  `Shift-Tab` move the rows you selected by one level each; nothing works out
  what level they *should* be at, which would need the same grammar.
- **A setting does not rewrite what is already there.** Turning
  `insert spaces` on does not convert a file's existing tabs, and turning it
  off does not convert its spaces. To convert, use the commands below.

### Converting a file's indentation

`Edit ▸ Tabs to spaces` and `Edit ▸ Spaces to tabs` rewrite the existing
indentation of the lines a selection touches, or of the whole file when nothing
is selected. Both are in the command palette too, and neither has a key, since
conversion is usually done once per file.

They change **only the indentation**, the whitespace in front of the first
character on each line. A tab after that is left alone, because it may be part
of the content: a column in a tab-separated file, or the tab a Makefile recipe
needs. A line that is nothing but whitespace is left alone too.

The width used is the tab width in force for the file, so the text does not
move on screen. Converting to tabs puts in as many tabs as there are whole tab
stops and keeps whatever is left over as spaces (ten columns at a width of
eight is a tab and two spaces), so alignment that was not on a tab stop stays
where it was. The conversion is one undo step, and the selection is kept, so
you can convert a block and convert it back with two commands.

To have `Tab` go on writing the same kind of indentation afterwards, set
[`insert spaces`](#configuration) to match.

## Moving Around

| Key | Action |
| --- | --- |
| `←` `↑` `↓` `→` | Move the cursor |
| `Ctrl-←` / `Ctrl-→` | Move a word left / right |
| `Home` | Go to the start of the line (with smart Home on, to where its text starts first) |
| `End` | Go to the end of the line |
| `PgUp` / `PgDn` | Move up / down a screenful |
| `Ctrl-G` | Go to a line number |
| `Ctrl-]` | Go to the bracket that matches this one |
| `F5` | Repaint the screen |
| `Ctrl-U` | Toggle the line-number gutter |
| `Alt-Z` | Toggle soft wrap |

Hold `Shift` while using the arrows, `Home`/`End`, or `PgUp`/`PgDn` to move the
cursor and select text along the way; see [Selecting Text](#selecting-text).

`Ctrl-G` prompts for a line number and moves the cursor to the start of that
line. A number past the last line goes to the last line instead; a number
below 1 goes to the first line. Anything that is not a valid number leaves
the cursor where it was.

With soft wrap on, `↑` and `↓` move a row of the *screen* instead of a line of
the file, so they walk down a long line before reaching the next one.
`Home` and `End` still go to the start and end of the line; see
[Soft Wrap](#soft-wrap).

You can resize the terminal while Molly is running. The display adjusts to the
new size immediately, without waiting for a keypress.

## Getting Back

| Key | Action |
| --- | --- |
| `Alt-←` | Back to where the last jump left from |
| `Alt-→` | Forward again |
| `Ctrl-]` | To the bracket matching the one at the cursor |
| `Alt-J`, then a digit | Put bookmark 0 to 9 on this line |
| `Alt-0` … `Alt-9` | Go to that bookmark |

**The jump history works like a browser's.** A *jump* (`Ctrl-G`, a find that
moved the cursor, `Enter` on a find-in-files result or a problem, `F12`, a
bookmark) remembers where it left from. `Alt-←` goes back there, and keeps going
back, sixty-four places deep, across files; `Alt-→` goes forward again, until
the next jump starts a new branch. Arrows, `PgDn` and `F3` are not jumps, since
they move within sight of where you were and recording every one would make the
history too long to step back through. A place in a file you have since closed
is skipped. **Go ▸ Back** and **Go ▸ Forward** are the same two commands.

**`Ctrl-]`** works on `()`, `[]` and `{}`: with the cursor on a bracket, or
immediately after one, it goes to its partner, counting the pairs between. It
does not know about strings or comments, so a bracket inside one counts.

**Bookmarks** are ten per file. `Alt-J` asks for a digit and puts that bookmark
on the cursor's line; `Alt-` and the digit comes back to it. A bookmark moves
with its line when you add or delete lines above it. They last as long as the
buffer: molly does not keep them when the file is closed.

## Help Screen

`F1` (or `Ctrl-?`, which still works) opens a keyboard reference in a framed
dialog over the editor. It lays
out in two columns on a wide terminal and drops to a single column when the
window is narrower, scrolling with the arrows, `PgUp`/`PgDn`, `Home`/`End` when
the content is taller than the screen. At 24 rows it is, so expect to page down
to see all of it. Resizing the terminal while the help screen is open reflows
it: it re-reads the size and switches between the one- and two-column layouts
to match, without leaving the screen. Press any other key to return to the
editor.

Its key column is generated from the same keymap the editor runs on, so it always
matches the actual bindings. **Help → Keyboard help** opens it too.

## Line Numbers

`Ctrl-U` toggles a line-number gutter (off by default). Each line is prefixed
with its number, right-aligned in a field as wide as the largest line number in
the file, followed by a gray vertical separator: `123 │ text`. The field width
is that number's digits plus the separator, so the column stays stable while
you type and only widens when the file crosses the next power of ten. The
separator is a continuous gray line: it runs through every row, including the
blank `~` rows below the end of the file and the welcome screen.

The gutter takes its space from the text area instead of being drawn over it:
showing it narrows the usable text width, and the cursor, horizontal scrolling
and the wide-character alignment all shift to match. Toggling it repaints the
whole screen. If the window is too narrow to leave even one text column, the
gutter is suppressed for that frame.

## Soft Wrap

`Alt-Z` toggles soft wrap (off by default). **View ▸ Soft wrap** has it too,
with a tick when it is on; the command palette lists it as **Soft wrap**, and
`soft wrap = on` in `[view]` starts that way.

With it off, a line longer than the window runs off the right-hand edge and the
view scrolls sideways to follow the cursor. With it on, the line continues at
the start of the next row of the screen, and there is no sideways scrolling.
Turning wrap on resets the horizontal scroll to column zero.

Soft wrap changes only how a long line is displayed. Saving writes the same
bytes that were there before, with no newline anywhere you did not type one.

**Where the break goes.** A line breaks after the last space that fits. A word
longer than the window breaks at the edge. The space it broke after stays at
the end of the row above, so a continuation row can begin one column further on
than you might expect from counting characters.

Tabs are expanded to spaces before wrapping, so an indent is a possible break
point like any other run of spaces: a deeply indented line whose first word
does not fit beside its indent puts that indent on a row of its own.

**The cursor.** The column a line breaks at belongs to the row below it, so a
cursor resting there is drawn at the start of the continuation instead of one
past the end of the row above. Moving down (or up) into a continuation row that
is shorter than the column you were at stops on that row's last column.

Clicking works on what you see: a click on a continuation row lands in the
middle of the line it continues, and a click past the end of one lands on that
row's last column instead of carrying on into the text below it.

**The gutter.** A wrapped line is numbered once, on the row it starts at. Its
continuation rows keep the gray separator and leave the number blank, so the
number column reads as one line per number: `12 │ a long line…` then
`   │ …and the rest of it`.

**Per window.** Wrap is a property of the window, like the gutter and the
colors. Splitting copies it to the new window, and each half can be toggled
on its own, so a split can show the same file wrapped on one side and not on
the other.

### What it deliberately does not do

- **No hard wrap.** Nothing inserts a newline into your text. For that, filter
  the text through `fmt -w 72` with `Alt-|`, which is one undo step.
- **No wrap column.** Lines wrap at the window's own width, not at a margin you
  set. For a narrower wrap, make the window narrower with `F7`.
- **No hanging indent.** A continuation row starts at the left edge of the text
  area, not under the indentation of the line it continues.
- **No sticky column.** Several `↓` presses in a row each keep the column they
  land in, not the one the first press started from, the same as with wrap off.

## Seeing More

Five display aids, all in the **View** menu and all off by default, since each
changes how every line looks and which ones help is a matter of taste:

| View menu | Setting | What it shows |
| --- | --- | --- |
| Whitespace (`Alt-.`) | `[view] whitespace` | Tabs as `→` and trailing blanks as `·`, dimmed |
| Indent guides | `[view] indent guides` | A faint `│` at each tab stop inside a line's indentation |
| Current line | `[view] current line` | The cursor's line tinted, across the window |
| Highlight word | `[view] highlight word` | The word under the cursor tinted wherever else it is on screen |
| - | `[view] ruler = 80` | Column 80 tinted, to show a line length (0, the default, for none) |

The ruler is a number, not a switch, so it is set in the Options dialog or in
`mollyrc` instead of from the menu. All five are drawn under the syntax colors
and a selection, never over them.

## Folding

`Alt--` folds the block the cursor is in: the line that opens it stays, and the
lines after it that are indented further are hidden behind a dimmed `⋯ 12
lines` at its end. `Alt--` on a folded line opens it again. **Fold all** and
**Unfold all** in the palette do every block in the file at once.

Folding works by indentation, so it behaves the same in C, Python, YAML and a
Markdown list, and needs nothing from a language definition. Blank lines inside
a block belong to it. On a line that opens no block, `Alt--` folds the block the
line is *inside*, and moves the cursor to its first line.

Everything that moves down the screen (the arrows, `PgDn`, the mouse wheel)
skips over a fold. Anything that sends you *into* one, such as a search,
`Ctrl-G` or a definition, opens it. Folds belong to the file, so two windows on
it fold together, and they move with their lines as you edit above them.

## Selecting Text

Hold `Shift` and use the movement keys to select a region between the anchor
(where the selection started) and the cursor. The selected text is shown in
reverse video as you extend it, and the status bar shows a `Sel <bytes>` count
while a selection is active.

| Key | Action |
| --- | --- |
| `Shift` + `←` `↑` `↓` `→` | Extend or shrink the selection |
| `Shift` + `Home` / `End` | Select to the start / end of the line |
| `Shift` + `PgUp` / `PgDn` | Extend the selection up / down a screenful |
| `Ctrl-C` | Copy the selection to the clipboard |
| `Ctrl-X` | Cut the selection (copy, then delete it) |
| `Ctrl-V` | Paste the clipboard at the cursor |

A selection is a character-wise range (for a rectangle, see
[Block Selection](#block-selection)): it always grows and
shrinks on whole character boundaries, so it never splits a multi-byte (UTF-8)
character. Pressing any movement key **without** `Shift`, or clicking/editing
elsewhere, clears the selection first.

Editing replaces the selection in one step: typing a character, `Enter`,
`Backspace` or `Delete` deletes the selected range and then does its normal
thing. `Ctrl-C` leaves the selection in place so you can copy the same range
again.

Pasting inserts the clipboard at the cursor (replacing any current selection);
embedded newlines split lines. A cut and a paste each undo (and redo) as a
single step with `Ctrl-Z` / `Ctrl-Y`.

Copying and cutting also push the selection to your **system (terminal)
clipboard** with the OSC 52 escape sequence, so it can be pasted into other
applications, or back into Molly with the terminal's own paste binding
(right-click, `Ctrl-Shift-V`, …). This helps when a terminal intercepts
`Ctrl-V` and would otherwise paste an unrelated application's clipboard. It
depends on your terminal honoring OSC 52 (some disable it by default for
security); where it is off, Molly's own `Ctrl-V` still pastes its internal
buffer. Molly does not *read* the system clipboard: `Ctrl-V` pastes only what
Molly last copied or cut.

### Pasting from your terminal

Your terminal's own paste (`Ctrl-Shift-V`, middle click) arrives as text and not
as typing. It goes in as one undo step, at every cursor, replacing any selection,
with its line breaks and tabs. Other control characters in it are dropped, so text
copied from a web page cannot run a command. It is not auto-indented and pairs are
not closed, since it brings its own. This relies on your terminal's bracketed paste;
one without it delivers a paste as typing.

## Several Cursors

Molly can put the cursor in several places at once and type in all of them.

| Key | Action |
| --- | --- |
| `Ctrl-D` | Select the word under the cursor; press again for the next occurrence |
| `Alt-D` | Skip this match and take the next one instead |
| `Alt-U` | Take the last cursor back |
| `Alt-A` | A cursor on every match at once |
| `Ctrl-Down` | Add a cursor on the row below |
| `Ctrl-Up` | Add a cursor on the row above |
| `Alt`-click | Add a cursor where you click |
| `Esc` | Back to one cursor |

The terminal has one real cursor, and the one you started with keeps it. The
others are drawn as colored blocks, since a terminal has no other way to show
more than one cursor. The status bar shows how many there are, which helps when
some of them have scrolled off the screen.

### Ctrl-D, one press at a time

The first `Ctrl-D` selects the word under the cursor and does nothing else, so
you see what is about to be matched before anything is multiplied. Each press
after that puts another cursor on the next occurrence, wrapping round the end
of the file. When there are no more, it says so and leaves the cursors you
have.

`Ctrl-D` matches literal text, and case matters. It ignores the Find dialog's
**Match case** and **Regular expression** checkboxes, which belong to
[searching](#searching); if `Ctrl-D` used them, turning patterns on would make
a word with a `.` in it stop matching itself.

`Ctrl-D` also works from a selection you made yourself: select any text and it
finds the next copy of exactly that, metacharacters included. The selection
has to be on one line.

### Skipping a match, and taking one back

`Alt-D` moves the cursor you most recently added on to the next occurrence,
leaving the match it was on unchanged. Use it when a press lands somewhere you
did not mean to change. If the match it moves to already has a cursor on it,
the two merge, since two cursors in one place would type everything twice.

`Alt-U` takes the last cursor back, one press per cursor, down to the one you
started with. Together they let you undo an extra press or an unwanted match
without pressing `Esc` and starting again.

Both work from the most recently added cursor, which is not necessarily the
last one in the file. After a search has wrapped round the end of the file, the
newest cursor is at the top, and it is the one you are working at.

### A cursor on every match

`Alt-A` takes the word under the cursor (or the selection, if you have one) and
puts a cursor on every occurrence of it in the file, in one press. Unlike
`Ctrl-D`, it does not use a first press to show you what will be matched; one
press does it all, and `Ctrl-Z` or `Esc` takes it back.

Pressing it again adds nothing, because every match already has a cursor. A
word that occurs once leaves you with the one cursor you had. There is a
ceiling of 512 cursors; in a file with more matches than that, it stops there
and says so.

### Typing with several cursors

Everything that edits reaches every cursor: typing, `Tab`, `Enter`,
`Backspace`, `Delete` and `Ctrl-V`. Where a cursor has a selection (as
`Ctrl-D` leaves), typing replaces it, so `Ctrl-D` `Ctrl-D` `Ctrl-D` followed by
a word renames three occurrences of something.

The whole edit is one undo step: however many cursors were involved, one
`Ctrl-Z` puts all of it back. Undo then leaves you with one cursor, because an
undo step records positions for the edit it reversed, not for the cursors you
had put on it.

Moving reaches every cursor as well: the arrows, `Home` and `End` all move the
whole set, and `Home`/`End` go to each cursor's *own* line edge, which is
useful for a column of cursors on ragged lines. Holding `Shift` extends a
selection at every cursor, each from where that cursor was.

Two cursors that land in the same place merge into one, since otherwise they
would type every character twice.

`Ctrl-V` puts the whole clipboard in at every cursor. It does not deal out one
copied line per cursor when the counts match, as some editors do. The exception
is a copied *block*, described under [Block Selection](#block-selection).

### What it deliberately does not do

- **`Alt-U` takes cursors back in the order they were added.** There is no way
  to pick one cursor out of the middle of the set and drop only it.
- **Only `Esc`, a plain click and undo collapse the set.** Switching files or
  windows keeps your cursors where they were.
- **The commands are not in the Edit menu**, because they are keyboard
  operations (Transpose is left out for the same reason). They are in the
  command palette (`Ctrl-Space`) and on the [help screen](#help-screen) with
  their keys.

## Block Selection

A block is a rectangle of text: the same columns on each of a run of rows.

| Key | Action |
| --- | --- |
| `Alt-Shift` + `↑` `↓` `←` `→` | Start a block at the cursor, or grow it |
| `Alt` + drag | Select the block between where you pressed and where you are |
| `Ctrl-C` / `Ctrl-X` | Copy / cut the block, a line per row |
| `Ctrl-V` | Paste a copied block as a block |
| `Esc` | Back to one cursor |

The status bar says how big the block is: `Block: 3 rows, 5 columns`. If you
had a selection when you pressed the first `Alt-Shift`-arrow, the block starts
from that selection's other end, so you can select with `Shift` and then turn
it into a rectangle.

A block is several cursors, one per row, each selecting its row's slice, so
everything in [Several Cursors](#several-cursors) applies to it: typing
replaces every row's slice at once, `Backspace` and `Delete` remove them, and
`Ctrl-Z` undoes the lot in one step. A row shorter than the block still gets a
cursor, at the end of the row, so typing into a block writes on every row of
it. Pressing anything other than `Alt-Shift`-arrow ends the block and leaves
the cursors; the next `Alt-Shift`-arrow starts a new block from where the
cursor is.

Block columns are screen columns, not bytes or characters, so a block over
lines indented with tabs, or with wide characters in them, is the rectangle you
see on screen. A tab or a wide character that straddles the block's edge is
taken whole.

**Pasting a block.** `Ctrl-C` and `Ctrl-X` put the block on the clipboard as
one line per row, which is also what other programs receive. Molly remembers
that it was a block, and `Ctrl-V` then puts it back as one:

- **With one cursor,** the lines go down as a rectangle: each on its own row,
  starting at the cursor's column, rows too short for that column padded with
  spaces, and new rows added at the end of the file if it runs out.
- **With a block (or any cursors) of the same height,** one line goes to each
  cursor.
- **With a different number of cursors,** it is the ordinary paste: the whole
  clipboard at every cursor.

Text copied any other way is never pasted as a block.

Not every terminal passes `Alt-Shift`-arrows or `Alt`-drag through: macOS's
Terminal and some window managers keep them for themselves. The commands are
in the [command palette](#the-command-palette) as **Extend block up**,
**down**, **left** and **right**, and can be bound to other keys in your
[configuration](#configuration) file as `block.up`, `block.down`, `block.left`
and `block.right`.

## Searching

`Ctrl-F` opens the **Find dialog**: a field for what to look for, a checkbox for
each of the two settings that decide what it means, and Find and Cancel buttons.
Type and press `Enter`, which presses Find from wherever you are in the dialog.
Matching begins from the current cursor position and wraps around the end of
the file.

`Tab` and `Shift-Tab` move between the field, the checkboxes and the buttons.
`Space` ticks the checkbox that has the keyboard, or presses the button that has
it. `Esc` closes the dialog without searching, and a click does whatever you
clicked on. The checkboxes start from the current settings and are applied when
you press Find, so canceling changes nothing. To change a setting without
searching, use `Search ▸ Match case` or `Alt-R`.

Leaving the field empty and pressing Find repeats the last search, and the
dialog's hint row shows that term. On a terminal too short for the whole
dialog, the hint goes first and then a checkbox; the buttons keep the last
row.

| Key | Action |
| --- | --- |
| `Ctrl-F` | Open the Find dialog; `Enter` finds the first match |
| `Ctrl-F` `Enter` (empty field) | Repeat the last search forward |
| `Tab` / `Shift-Tab` | Move between the field, the checkboxes and the buttons |
| `Space` | Tick the checkbox, or press the button, that has the keyboard |
| `F3` | Find the next instance of the search term |
| `Shift-F3` | Find the previous instance of the search term |
| `Ctrl-N` / `Ctrl-P` | The same two, for fingers that already know them |
| `Alt-R` | Toggle regular-expression search |
| `Esc` | Cancel the search prompt |

The same term is remembered for the whole session, so you can keep tapping
`F3` / `Shift-F3` to step through every occurrence. If there is no previous
search, Molly tells you and asks you to press `Ctrl-F` again.

Matching is case-insensitive by default. The Find dialog's **Match case**
checkbox (or `Search ▸ Match case`) turns case-sensitive matching on or off at
any time, not only while a search prompt is open; the status bar reports which
mode is now active. The setting applies to `Ctrl-F`, `F3`, `Shift-F3`, and
search-and-replace alike. The case-insensitive fold covers ASCII `a`-`z` only;
every other byte is compared as-is, so accented letters match only themselves.
A match always spans whole characters: a query that would begin or end inside a
UTF-8 character never matches, so a replace can never split a character in
half.

A search matches the line as it is *stored*, not as it is drawn: a run of
spaces never matches a tab. (To match a tab, use `\t` with `Alt-R` on; see
[Regular Expressions](#regular-expressions).)

## Regular Expressions

`Alt-R` switches the search between plain text and a regular expression, and the
status bar says which mode it is now in. The mode sticks until you switch it
back, and it applies to `Ctrl-F`, `F3`, `Shift-F3` and `Ctrl-R` alike. The
prompt says `Pattern:` instead of `Search:` so you can tell at a glance. It is
also in the Search menu, with a tick when it is on.

### What Molly understands

| | |
| --- | --- |
| `.` | any one character |
| `[abc]` `[^abc]` `[a-z]` | a character class, or anything but one |
| `\d` `\w` `\s` | a digit, a word character, a space - and `\D` `\W` `\S` for the opposite |
| `*` `+` `?` | none or more, one or more, none or one |
| `{2}` `{2,}` `{2,5}` `{,5}` | a count, a floor, a range |
| `*?` `+?` `{2,5}?` | the lazy form of any of them: as few as will do |
| `(…)` | a group, which `\1` can refer to in a replacement |
| `(?:…)` | a group that is only for grouping |
| `\|` | either side |
| `^` `$` | the start and the end of the line |
| `\b` `\B` | a word boundary, or anywhere that is not one |
| `\t` `\.` `\*` `\\` | a tab, and a backslash in front of anything with a meaning |

The following are not supported, and a pattern that uses them is reported as an
error: lookaround (`(?=…)`), backreferences inside the pattern itself, named
groups, possessive quantifiers, `\x41`-style escapes and POSIX `[:alpha:]`
classes. A pattern Molly cannot read is refused with the reason
(`Bad pattern: missing ) in pattern`), and nothing is searched or changed.

Everything above ASCII is a word character, so `\w` and `\b` work on `café` and
on `naïve`. Case folding is still ASCII-only, as it is for a plain search, so
`[a-z]` finds `A` but `é` never matches `É`.

### What a search matches against

Both a plain search and a pattern match the line as it is *stored*, not as it is
drawn, so a run of spaces never matches a tab, and `.` is one character
whatever width it occupies on screen. A tab is written `\t`, which only works
in a pattern, because a prompt cannot accept a Tab keystroke.

`.` never matches a line ending, and `^` and `$` are the ends of a line rather
than of the file, because Molly matches one line at a time. A pattern cannot
match across a line break.

### Patterns that cannot be answered quickly

Some patterns take exponential time to decide; `(a+)+$` against a long run of
`a`s is the classic example. Molly gives each line a budget, and when a pattern
runs through it says **`Pattern too complex. Try a simpler one`** and leaves
everything where it was. A replace-all that runs out partway says how many
replacements it had already made; one `Ctrl-Z` undoes all of them.

Because the budget is per line, the same pattern may work on a shorter line.
Making the pattern more specific, for example with a literal character it must
contain or a narrower class, usually fixes it.

## Search and Replace

`Ctrl-R` prompts for a search term, then a replacement, then asks whether to
replace just the next match (`n`) or every match in the file (`a`).

- **Next** replaces the first match found from the cursor position forward
  and stops there.
- **All** replaces every match in the file, from the start, and reports how
  many occurrences it changed.

A replace-all undoes and redoes as a single step, no matter how many
occurrences it touched; one `Ctrl-Z` restores the whole file. Replacement
respects the current case-sensitivity setting (**Match case**) and the current
pattern setting (`Alt-R`).

### Groups in the replacement

With `Alt-R` on, `\1` to `\9` in the replacement stand for what the
correspondingly numbered group matched, and `\0` for the whole match. Searching
for `(\w+)=(\w+)` and replacing with `\2=\1` turns `key=value` into
`value=key`. A group that did not take part in the match (the other side of a
`|`, for example) is replaced with nothing. `\\` is a backslash, `\t` is a tab,
and a backslash in front of anything else is that character.

A replacement cannot contain a newline, because Molly works one line at a time
and splicing one in would make two lines; `\n` is refused with
`Bad replacement: a replacement cannot contain a newline`.

A pattern can match an empty string (`x*` matches between every pair of
characters). A replace-all then inserts at each of those places and steps over
a character each time, as `sed` does: replacing `x*` with `-` in `abc` gives
`-a-b-c-`. It does not loop or replace anything twice.

## Undo and Redo

| Key | Action |
| --- | --- |
| `Ctrl-Z` | Undo the last change |
| `Ctrl-Y` | Redo an undone change |

Consecutive typing of characters (and consecutive partial deletions) are
grouped into a single change, so undoing a whole typed word is one press.
Undoing a change puts it back onto the redo stack, so `Ctrl-Y` replays it.

The history is bounded: once a stack holds more than 32,768 entries or
16 MB of stored text, Molly forgets its oldest entries. A long session
therefore limits how far back `Ctrl-Z` reaches but cannot exhaust memory
through history. (The step limit can be lowered through the
`MOLLY_UNDO_STEPS` environment variable, which the test suite uses to
exercise eviction.)

Changes are aggregated this way:

- A run of typed characters undoes as one step.
- A run of backspaces undoes as one step.
- Line splits, line joins, and row insertions likewise undo as one step.
- A replace-all (see [Search and Replace](#search-and-replace)) undoes and
  redoes as one step, however many occurrences it changed.
- A cut (`Ctrl-X`) and a paste (`Ctrl-V`) each undo and redo as one step,
  however many characters or lines they span.

Typing a character on a brand-new auto-created row records two changes (the
row itself, then the character), so it takes two `Ctrl-Z` presses to fully
undo it. Any edit made after an undo clears the redo history, so `Ctrl-Y`
cannot restore changes that newer edits replaced.

## Keyboard Macros

| Key | Action |
| --- | --- |
| `Alt-(` | Start recording; press it again to stop |
| `Alt-)` | Play the recording back |

While recording, the status bar says `REC`. Every key you press is recorded
(typing, commands, answers to prompts, keys pressed in a dialog) and played
back the same way it was entered, so a macro can open the Find dialog, type a
search and go on from where it lands. There is one macro, of up to 4,096 keys;
recording another replaces it. Resizes and mouse actions are not recorded,
since their effect depends on the screen at the time. Pressing `Alt-)` while
recording tells you to stop recording first instead of playing the macro into
itself.

## Opening Another File

`Ctrl-O` opens a file browser. It starts in Molly's working directory (the
directory you started it from), so `cd` to where you want to browse before
starting it.

    Ctrl-O                 Open the file browser
    Up/Down, Ctrl-P/Ctrl-N Move one entry
    PgUp/PgDn, Home/End    Jump around the list
    Enter                  Enter a directory, or open the selected file
    Esc                    Cancel and return to the buffer

Directories are listed before files, each group alphabetically, with a
trailing `/` on directory names. Every list offers `..` at the top as the way
back up; other dot-files are hidden.

A file opens in a buffer of its own, alongside anything already open (see
[Several Files at Once](#several-files-at-once)). The current buffer is kept,
so the browser no longer refuses to run while there are unsaved changes. A file
that is already open is switched to instead of being opened a second time: two
buffers on one file would each have their own undo history and their own idea
of what is on disk, and the second save would silently undo the first.

If the buffer you are in is empty and unnamed (a fresh `molly` with nothing
typed into it), the file opens into *that* buffer instead of beside it, so you
get one tab, not two.

### Recent files

`Alt-O` (or **File ▸ Recent files…**) lists the last twenty files you opened,
newest first, with your home directory written `~`. `Enter` opens one; this
counts as a jump, so `Alt-←` comes back. The list is kept between sessions in
`$XDG_STATE_HOME/molly/recent` (`~/.local/state/molly/recent` when that is not
set), since it is state molly writes as you work rather than configuration.
`MOLLY_STATE` names another directory for it; set it empty to keep no list at
all.

### Files you cannot write

A file molly cannot write to (someone else's, or one you have made read-only)
opens **read-only**: the status bar says `[read-only]`, every command that
would change the text says so and does nothing, and `Ctrl-S` refuses instead of
failing halfway. **View ▸ Read only** turns it off, for when you mean to save
somewhere else, or on, to look at a file without risk of changing it.

## Several Files at Once

Molly holds any number of files open at once. Each one is a **buffer**: its own
text, its own cursor position, its own undo history and its own idea of what is
on disk.

    molly one.c two.c three.h

Every file named on the command line opens in its own buffer, and the first is
the one you land in. A name repeated opens once.

    Ctrl-N        New empty buffer
    Ctrl-W        Close this buffer
    Ctrl-PgDn     Next buffer
    Ctrl-PgUp     Previous buffer

`File ▸ New`, `File ▸ Close` and `Go ▸ Next/Previous buffer` do the same, and so
does the command palette.

### The tab bar

With more than one buffer open, a row of tabs appears under the menu bar:

     one.c  two.c*  three.h

One tab per buffer, showing the file's name rather than its path; the current
one is highlighted, and an asterisk means unsaved changes. Click a tab to switch
to it, or roll the wheel over the row to step through them.

The row appears when there is a second buffer and goes away again when there is
not: a single tab restates what the status bar already says, and molly is meant
to be usable on a 24-row terminal. `Alt-B`, `View ▸ Tab bar`, or
`tab bar = off` in the `[view]` section of your
[configuration](#configuration) file turn it off entirely.

When there are more tabs than fit, molly shows the tabs around the current
buffer, with a `‹` or `›` at the edge when there are more in that direction.
The row scrolls instead of squeezing the tabs, because tabs cut to three
characters are harder to read than tabs you have to scroll past. Clicking an
arrow steps one buffer in that direction.

The status bar also carries a counter on the left:

    [2/3] two.c - 140 lines (modified)

With one buffer there is no counter, since `[1/1]` would add nothing.

Switching away and back returns you to where the cursor was, not to the top of
the file.

### Closing

`Ctrl-W` closes the current buffer. If it has unsaved changes, molly says so and
does nothing; press it again and it closes anyway. The warning applies to that
buffer only: if you switch to another unsaved buffer and press `Ctrl-W` once,
it warns about that one too, even though you already dismissed a warning
elsewhere.

Closing the only buffer empties it instead of leaving molly with nothing to
draw, as other editors with tabs do.

### Quitting

`Ctrl-Q` checks every buffer, not only the one on screen. If any has unsaved
changes, a dialog lists them by name with three buttons:

- **Save All** (the default; `S` or Enter) saves each one through the ordinary
  save, so a nameless buffer asks for its name. If any save is aborted the
  editor stays open, on that buffer.
- **Discard** (`D`) quits and throws the changes away.
- **Cancel** (`Esc`) goes back to editing.

A clean session quits at once.

### Picking a buffer

`Ctrl-B`, or **Go → Buffers…**, lists every open buffer, with `*` marking the
unsaved ones. Up and Down move, Enter shows the chosen buffer, Esc leaves things
as they were. A click on a buffer shows it at once, and the wheel scrolls the
list.

### Crashes

A crash or a `SIGHUP` writes a recovery file for every buffer that has unsaved
changes, not only the one that was on screen. See
[Recovery of unsaved work](#recovery-of-unsaved-work); the prompt comes up once
per buffer that has data to recover, with that buffer on screen so you can see
which file it is asking about.

## Filtering Through a Command

`Alt-|` (or `Edit ▸ Filter through…`) sends text to a command's standard input
and replaces it with the command's standard output.

    Filter through: sort

With text selected, that is what gets sent. With nothing selected, the whole
file is sent, since running `sort` on a whole file is the most common use and
you should not have to press `Ctrl-A` first.

The command goes to `/bin/sh`, so a pipeline, a redirect and a glob all work:

    Filter through: sort -u | tr a-z A-Z
    Filter through: jq .
    Filter through: fmt -w 72
    Filter through: python3 -m json.tool
    Filter through: black -q -

Press `Enter` on an empty prompt to run the last command again, as `Ctrl-F`
does for repeating a search. The whole thing is one undo step, however many
lines it changed.

### When the command fails, your text stays

A command that exits non-zero does **not** replace anything. The status bar says
what went wrong, in the command's own words where it left any:

    jq .: jq: error (at <stdin>:3): Objects must consist of key:value pairs

Your text is left in place instead of being replaced and left for undo to
recover. This means a command whose non-zero exit is meaningful, such as `grep`
(which exits 1 when it matches nothing), cannot be used to empty a selection.
Use `Delete` for that.

Molly adds a line terminator back only if your text had one: filtering the end
of a file through `sort` does not grow a blank line each time.

### Three ways out of a command that misbehaves

An external command is the only thing that can hang molly, so there are three
ways to stop one:

- **`Esc` cancels.** While a command runs the message bar says so, and `Esc`
  stops it. The process group is sent `SIGTERM`, then `SIGKILL` if it does not
  exit, so a pipeline goes with its shell.
- **A timeout.** `[shell] timeout`, in seconds, default 10. A command still
  running after that is stopped the same way.
- **An output cap.** `[shell] output limit`, in kilobytes, default 8192. The cap
  is checked as the output arrives, so a command that floods output is stopped
  before it is all held in memory.

In all three cases the text is left exactly as it was. A command's standard
error never reaches the screen directly; it is captured, and the message bar
shows its first line.

The command runs with the same privileges as your shell. Molly runs only
commands you type.

### Inserting a command's output

`Alt-!` (or `Edit ▸ Insert output…`) runs a command with nothing on its standard
input and puts what it prints at the cursor:

    Insert output of: date
    Insert output of: git rev-parse HEAD
    Insert output of: uuidgen
    Insert output of: ls *.c

One trailing newline comes off what the command printed, so a one-line answer
goes into the middle of the line you are on instead of splitting it, for
example a hash in a commit message or a date in a header. Output of several
lines leaves the cursor at the end of the last one, where pasting the same text
would.

A selection is replaced, as typing or pasting over it would replace it; to send
the selection to the command instead, use `Alt-|`. With
[several cursors](#several-cursors) the command runs once and its output goes
in at every one of them.

The filter's other behavior applies here too: `Enter` on an empty prompt runs
the last command again (this command remembers its own, not the filter's), it
is one undo step, `Esc`, the timeout and the output cap stop it, and a command
that exits non-zero inserts nothing. For example, `git rev-parse HEAD` outside
a repository puts `fatal: not a git repository` on the status bar, not in your
file.

## Finding in Files

`Ctrl-E` (or `Search ▸ Find in files…`) searches every file under the directory
molly was started in, and lists what it found in a window of its own:

    Find in files: parse_config

    src/config.c:212:int parse_config(const char *path) {
    src/main.c:48:  if (parse_config(rc) != 0)
    tests/test_units.c:3102:  check("empty file", parse_config(empty), 0);

Each row is a file, a line number and the line. `Enter` on a row opens that
file at that line, with the cursor on the match, in the window you searched
from. The list stays where it is, so you can go back to it and pick another.
`F4` goes to the next match and `Shift-F4` to the previous one, from any window,
without going back to the list; the list's cursor follows along so you can see
where you are.

The pattern is read the way [Find](#searching) reads one: plain text, ignoring
case, unless you have turned on **Match case** or
[pattern search](#regular-expressions) (`Alt-R`). Pattern search is handed to
`grep -E`, which agrees with molly's own patterns on common syntax but is a
different engine. A pattern that relies on a feature only one of them supports
can list a line where molly then cannot find the match; the cursor lands at the
start of the line instead.

`Enter` on an empty prompt runs the last search again.

### What is searched

Inside a git work tree, molly runs `git grep`, so it searches the files git
tracks and nothing your `.gitignore` leaves out. Anywhere else it runs
`grep -r`, which searches everything below the current directory except `.git`.
Binary files are skipped by both. Either way it goes through the same machinery
as [filtering](#filtering-through-a-command), so `Esc` stops a search that is
taking too long, and the `[shell] timeout` and `output limit` settings apply.

### The results are a buffer

The list is an ordinary buffer named `grep: <pattern>`, so it has a tab, it can
be scrolled and searched with `Ctrl-F`, split, and saved with `Ctrl-S` if you
want to keep it. A new search replaces it instead of opening another. It does
not count as unsaved work, so quitting or closing it does not prompt, unless
you edit it; then it is a modified buffer like any other. Lines longer than 400
characters are cut short in the list, which only matters for minified files.

## Window Splits

A **buffer** is a file; a **window** is a view onto one. Until you split, there
is one window and the distinction does not come up. After a split you can look
at two files, or at two parts of the same file.

    F7            Split, new window to the right
    F8            Split, new window below
    F6            Next window
    Shift-F6      Previous window
    F9            Close this window
    Ctrl-Shift-←  Move the border beside this window left (→ for right)
    Ctrl-Shift-↑  Move the border above or below it up (↓ for down)

`Window ▸ Split right`, `Window ▸ Split below` and the rest do the same, plus
`Window ▸ Only this window`, which closes all but the one you are in. The
command palette reaches all ten by name.

The new window starts out showing the same file at the same place, with the
keyboard in it. From there the two diverge: each window has its own cursor, its
own scroll position and its own line-number gutter, so `Ctrl-U` turns the gutter
on in the window you are in and leaves the other alone.

### Which window has the keyboard

Every window has its own status bar on its last row, showing the file's name,
its length, whether it is modified, and that window's own `Ln`/`Col`. The
window with the keyboard has the bright bar; the others are dimmed. With one
window the bar is where it always was, at the bottom of the screen above the
message line.

Side-by-side windows are divided by a `│`. Stacked windows need no divider: the
upper window's status bar is already a line across the screen.

Click anywhere in a window, including on its status bar, to move the keyboard
there. Clicking in the text moves that window's cursor as well; clicking the
status bar only moves the keyboard.

### Two windows on one file

Splitting without switching files gives you two views of one buffer. Type in
either window and the text changes in both, because the text belongs to the
buffer and only the cursor and the scroll belong to the window. Undo belongs to
the buffer too, so `Ctrl-Z` in either window undoes the same edit.

To put a different file in a window, split and then switch buffers
(`Ctrl-PgDn`, or click a tab):

     one.c  two.c
    #include <stdio.h>     │static int count;
    int main(void) {       │
    …                      │void bump(void) {
    [1/2] one.c - 40 lines │[2/2] two.c - 12 lines

### Windows and buffers are closed separately

`F9` closes the **window**; the file stays open and is still on the tab bar.
`Ctrl-W` closes the **buffer**; any window showing it moves to another file
instead of closing. Closing the last window is refused; molly always has at
least one window, as it always has at least one buffer.

Closing a window gives its space back to the window it was split from, so
closing the second of two restores exactly the screen you had before the split.

### Making one window bigger

A split starts out even. To change that, move the border between the windows
with the keyboard or by dragging it.

`Ctrl-Shift` and an arrow moves the border beside the window you are in by one
column, or the border above or below it by one row. The arrow gives the
direction the border moves, not whether your window grows: `Ctrl-Shift-←` moves
the line between two side-by-side windows to the left whichever of them you are
in, so it makes the left one narrower and the right one wider either way. If a
window has borders on both sides, the one that moves is the one it was split
along last.

With the mouse, drag the `│` between side-by-side windows, or the status bar of
the upper of two stacked windows; that bar is the border between them. The
bottom window's status bar is above nothing and does not move. A click without
a drag only moves the keyboard.

A border stops where a window would go below its minimum, including windows
split inside the half it is moving into; when you use the keyboard, the message
line says so. The border keeps its *proportion*, not its column, so resizing
the terminal afterwards scales your layout instead of resetting it.

`Window ▸ Move border left` (and right, up and down) do the same from the menu.
On the help screen the four chords are two rows, `Ctrl-Shift-←→` and
`Ctrl-Shift-↑↓`, and those two rows show the default chords even if you rebind
them.

### When there is no room

A window needs at least one row of text plus its status bar, and eight columns.
A split that would leave less than that is refused with a message saying which
way it would not fit. Shrinking the terminal under a split you already have
shares out what is left in the same proportions, and a window squeezed below
the minimum is left blank until there is room for it again, so the window you
are typing in stays legible while you drag a corner.

## The Terminal

`Alt-T` (or `Window ▸ Terminal`) opens a shell in a window under the one you
are in, a third of its height. Type in it as in any terminal: everything you
type goes to the program running there, and its output, including colors and
full-screen programs like `less`, `vim` and `htop`, is drawn in the window.

    Alt-T          To the terminal, opening one if there is none; from it, back
    Shift-PgUp     Scroll back through what has gone off the top (PgDn: forward)
    F6 / Shift-F6  To the next / previous window, as anywhere
    Ctrl-PgDn/PgUp To the next / previous buffer
    F10            The menu bar
    Ctrl-Space     The command palette (Alt-X does the same)
    Ctrl-Q         Quit molly

Those are the only keys a terminal leaves to molly, and they are the ways out
of it. Every
other key, `Ctrl-C`, `Ctrl-W`, `Ctrl-S`, `Alt` with a letter and the function
keys among them, goes to the program. A key you have rebound moves with its
command, because what is left to molly is decided by command, not by chord.

A terminal is a buffer, so it has a tab, it is in the buffer list, it can be
split into two windows or moved into the one you are in with `Ctrl-PgUp`, and
`Window ▸ Close` closes its window without stopping it. **New terminal** in the
palette opens another, in the current window. The shell is `$SHELL`, started in
the directory molly was, and told its terminal is an `xterm-256color`.

When the shell exits, its terminal closes: the buffer, and the window it was
in. A shell that exits with an error says so on the message bar.

### Copying, pasting and scrolling back

- **Paste** (`Edit ▸ Paste`, or Paste in the palette) types molly's clipboard
  into the program, and so does pasting from your own terminal. A shell that asked
  for bracketed paste gets it, so a pasted command is not run until you press
  `Enter`.
- **Copying out of a terminal**: drag across it with the mouse. What you drag
  over is highlighted, and letting go copies it to molly's clipboard, and to
  your own terminal's through OSC 52 where it supports that, as `Ctrl-C` does in a
  file. Each line's trailing blanks are left behind, and a drag over several
  lines copies them as lines. A click without a drag lets the selection go, and
  so does typing. The scrollback can be dragged over as well as the screen.
- **The scrollback** keeps the last 5,000 lines. While you are scrolled back the
  status bar says how far, new output does not move what you are reading, and
  any key you type goes back to the bottom. The mouse wheel scrolls it too.

### Finding in the scrollback

**Find in terminal** in the palette asks for some text and shows the newest
line above the bottom of the window that has it, scrolled into sight with the
match highlighted. Case does not matter, and the text is matched as typed,
with no patterns. Once you are scrolled back the find keys are molly's
again, since nothing you typed there would reach a prompt you cannot see:

    Ctrl-F         Find in the scrollback (the same as Find in terminal)
    F3             The next older match
    Shift-F3       The next newer match
    Esc            Back down to the bottom, letting the match go

While a match is showing, the status bar says `F3 older  Esc`. Finding goes
upward, into the scrollback. F3 at the oldest match says there are no older
ones; it does not wrap around to the bottom.

### Programs that use the mouse

A program that asks for the mouse (`less --mouse`, `vim` with `mouse=a`,
`htop`, `mc`) is sent it: clicks, releases, the wheel, and drags if it asked
for those, in the encoding it asked for (the SGR form, or the original one for
a program that did not ask for SGR, which cannot name a cell past column 223).
Molly's own mouse handling in the text area is off for as long as the program
wants the mouse.

Hold `Shift` to take the mouse back, as in xterm: a `Shift`-drag selects and
copies even in a program that has the mouse.

On the **alternate screen** (where `less`, `man` and the other full-screen
programs draw, and where there is no scrollback), the wheel sends the program
`↑` and `↓`, three a notch, when it did not ask for the mouse. This lets a
pager without mouse support scroll with the wheel.

### Running programs, and quitting

The status bar says **running** while something other than the shell has the
terminal, such as a build, an editor or a pager. Quitting molly then, or closing
that terminal's buffer, asks first: `y` stops the program, anything else keeps
it. A terminal at its prompt closes without asking.

### What it deliberately does not do

- **Nothing that edits text.** A terminal's screen is not text molly owns, so
  undo, cut, indent, the filter and the rest say so and do nothing, and a
  terminal cannot be saved.
- **No reflow.** A line is as wide as the window was when it was written; a
  window made narrower cuts old lines, and wider pads them. Re-wrapping would
  mean knowing which rows were one long line and which were two, through every
  scroll, erase and insert a program makes; and re-wrapping the screen itself
  fights the shell, which redraws its own prompt when the window changes size
  and would then show it twice. Full-screen programs redraw on a resize anyway,
  so reflow would only fix old output in the scrollback, which is not worth
  the bookkeeping.
- **24-bit color is passed through only where your terminal says it can take
  it** (`COLORTERM=truecolor` or `24bit`). Otherwise it goes out as the nearest
  of 256, or of sixteen. A program in the terminal is told `COLORTERM=truecolor`
  only when that is true of the terminal molly is in.
- **No regular expressions in the scrollback find.** The search is plain text
  and ignores case.
- **F10 and F6 are molly's.** A program that wants them has to be given
  another key: `htop` quits on F10, so use `q` there.

## Language Servers

A **language server** is a program that knows a language (clangd for C and
C++, rust-analyzer for Rust, pylsp for Python) and answers questions about the
file you are editing: what is wrong with it, where a name is defined, what a
name is, and what could come next. Molly starts the one your file's language
names the first time you open such a file, tells it about every change, and
shows you what it says.

    F2 / Shift-F2  Next / previous problem
    Ctrl-F2        The problems list: every problem in every open file
    F12            Go to the definition of what is under the cursor
    Alt-Left       Back to where F12 jumped from (Alt-Right: forward again)
    Shift-F12      Every place the name under the cursor is used
    Alt-K          What is under the cursor: its type, signature and docs
    Alt-/          Complete the word before the cursor
    Alt-P          The signature of the call the cursor is in
    Alt-N          Rename what is under the cursor, everywhere
    Alt-Q          Format the file, or the selection
    Alt-Enter      Code actions: fixes and refactorings for here

The navigation ones are also in the **Go** menu, and all of them are in the
command palette, along with **Language server status** and **Restart
language servers**.

### Which server, and when

| Language | Server it starts |
| --- | --- |
| C, C++ | `clangd` |
| Rust | `rust-analyzer` |
| Python | `pylsp` |

Molly looks for the program on your `PATH` before starting anything, and if
the server is not installed, molly stays quiet: nothing is underlined or
counted, and no message about it appears each time you open a file. To check,
use **Language server status** in the palette. It says which server a file would
use and whether it is running, starting, stopped or not installed. C and C++
files share one clangd.

To name a different server, or one for a language that has none, give the
language's `[syntax]` section a `language server` line; `none` turns one off:

```ini
[syntax python]
language server = pyright-langserver --stdio

[syntax c]
language server = clangd --background-index --header-insertion=never

[syntax rust]
language server = none
```

The line is run by the shell, so arguments, a variable in front and `~` all
work. `[editor] language servers = off` turns every server off at once, and is
in the [Options dialog](#the-options-dialog).

### Which project

A server is started for a **project**, and told where its root is; the server
reads the build files there to learn how the code fits together. The root is
found from the file, not from where molly was started, so `molly
~/src/other/main.c` gets a server rooted in that project:

1. The nearest directory above the file with a build file or a compilation
   database in it: `compile_commands.json`, `compile_flags.txt`, `Cargo.toml`,
   `go.mod`, `pyproject.toml`, `setup.py`, `setup.cfg`, `package.json`,
   `tsconfig.json`, `deno.json`, `build.zig`, `Gemfile`, `pom.xml`,
   `build.gradle`, `CMakeLists.txt`, `meson.build` or `Makefile`.
2. Failing that, the nearest repository (a directory with `.git` or `.hg`).
3. Failing that, the directory molly was started in.

Molly takes the nearest, not the outermost, because a repository with several
projects in it has a build file for each, and each project's files want that
project's server. Files from two projects get two servers, one each, even of the
same program; files of one project share one.

### Problems

What a server says is wrong is shown three ways:

- **In the text, underlined.** An error is red and a warning amber, over the
  syntax color; information and hints are only underlined, in the color the
  text already had. A selection over one keeps the underline.
- **On the status bar**, as `E:2 W:1` before the cursor's position. This
  counts the whole file, including what is off the screen.
- **On the message bar**, when the cursor is on one: `error: use of undeclared
  identifier 'y'`. Moving onto a problem shows it straight away; a message
  that is the answer to something you just did keeps the bar until you move.

`F2` goes to the next problem and `Shift-F2` to the previous one, round the end
of the file, saying `2 of 5: error: …` as they go. A problem moves with its
text when you insert or delete lines above it, and the server's next report
replaces it a moment after you stop typing. Molly sends the server your
changes when the keyboard has been quiet for a tenth of a second, not on
every key.

### The problems list

`Ctrl-F2` (or `Go ▸ Problems`) opens a window below with every problem in every
open file, one row each (`src/a.c:12:5: error: expected ';'`), with errors red
and warnings amber, sorted by file and line, and the totals on the list's status
bar. The list stays current: a moment after you stop typing, a fixed problem is
gone from it, and a server's new report on any open file rewrites it. `Enter` on
a row goes to that problem, in the window you opened the list from, opening the
file's buffer if it is not the one there. `Ctrl-F2` again, from anywhere, goes
back to the list.

The list is a buffer called `problems`: it scrolls, searches and splits like
any other, and it is never unsaved work. Editing it does nothing useful, since
the next refresh writes over it, and `Enter` in it never inserts a line.

### Going to a definition, and back

`F12` asks where the name under the cursor is defined and goes there, in
another file if need be, opening the file or switching to it if it is already
open. `Alt-Left` goes back to where you were, and keeps going back
through as many jumps as you have made, up to sixty-four; `Alt-Right` goes
forward again. That history covers every jump, not only F12's; see [Getting
Back](#getting-back).

### Every place a name is used

`Shift-F12` asks where the name under the cursor is used, its declaration
included, and lists the answer the way [find in files](#finding-in-files)
does: a `file:line:text` row each, sorted, in the same results buffer.
`Enter` opens one, and `F4` and `Shift-F4` walk them from anywhere. Each lands
on its own column, so two uses on one line are two stops.

### What is under the cursor

`Alt-K` opens a box under the cursor with what the server knows about the name
there: its type or signature, and its documentation. Servers write this in
Markdown. Molly does not render it; it shows code as code without the fences
and drops the emphasis marks. A box
too long for the screen ends in `…`. The next key closes it and then has its
usual effect; `Esc` only closes it.

### Completion

`Alt-/` lists what could finish the word before the cursor. The list sits
under the word, with each item's type or signature dimmed on the right:

- **Type** to narrow it. It is matched the way the [command
  palette](#the-command-palette) matches, so `gsz` finds `get_size`.
- `Up` and `Down` choose, `PgUp` and `PgDn` move a boxful.
- `Enter` or `Tab` puts the chosen one in place of the word typed so far.
- `Esc`, or any key that is not part of a word, closes it.

Typing a character the server says starts a completion (`.` after a struct,
`->` or `::` in C++) opens the list without `Alt-/`. Taking a completion is
one undo step, separate from the typing before it: `Ctrl-Z` takes the
completion back and leaves the word as you typed it.

**`[editor] complete while typing = on`** opens it as you type a word, too:
once the word has two letters, and from then on as long as there is anything
to offer. It is off by default because, with it on, the `Enter` at the end of
a line you have just typed a word on takes a completion instead of starting the
next line whenever the list is open. When you did not ask and there is nothing
to offer, no message is shown.

### The signature of a call

Typing the `(` of a call (or whatever characters the server says start one,
usually including `,`) opens a box above the line with the function's
signature, the parameter you are typing picked out, and the first line of what
the server says about it. It follows along as you type: after a comma the next
parameter is picked out, and closing the call closes it, as do `Esc` and
leaving the line. `Alt-P` opens it again. Where there is no room above, it goes
below; where there is room for neither, it drops the note.

### Formatting

`Alt-Q` (or `Edit ▸ Format`) asks the server to format the file, or only the
selection if there is one and the server can format part of a file. Where it
cannot, the whole file is formatted and the message says so. The server is told
the tab width and whether `Tab` inserts spaces, as they are for that file's
language (see [Indentation](#indentation)), and most formatters start from
those. The changes are one undo step, the cursor stays on its line, and a file
that needs nothing is left unmodified: "Already formatted."

clangd formats by a `.clang-format` file when it finds one above the file, and
in LLVM's style when it does not.

**`[editor] format on save = on`** formats the whole file every time it is
saved, before it is written, so what reaches the disk is the formatted text. The
save waits up to two seconds for the server's answer (`Esc` stops waiting),
and the message says what happened: `1203 bytes written, formatted (3
changes)`, or `…, not formatted: no answer in time` when the server was too
slow, in which case the file is saved as it was. A file whose server cannot
format, or that has no server, is saved as usual without a word. It is off
by default, so that saving never changes the text unless you asked for that.

### Code actions

`Alt-Enter` asks the server what it can do at the cursor, or over the
selection, and lists the results: the fix for the problem under the cursor
(molly sends the server the problems at that place, so it can offer fixes for
them), and refactorings such as extracting a function or organizing the
imports. `Up`, `Down` and `Enter` choose one; `Esc` chooses none.

Molly carries out the chosen action in whatever form the server gives it: an
action that comes with its edit has that edit applied; one the server works out
only when it is chosen is requested then and applied; one that is a command the
server runs has the server send its edits back, and those are applied too. The
edits are made the way a rename's are: each file's as one undo step, and a file
that is not open is opened and left unsaved. The message names the action and
counts the changes. An action the server lists as unavailable here is left off
the list.

`Alt-Enter` is JetBrains' key for the same list; VS Code's `Ctrl-.` is not a
chord a terminal can send.

### Renaming

`Alt-N` asks for a new name for what is under the cursor, and the server works
out every place that changes, in this file and in others. Each file's changes are
one undo step. A file that is not open is opened, changed and left unsaved, so
nothing is written before you have seen it. The tabs with a `*` are the files
it changed, and the message says how many. A rename that would create, move or
delete a file is refused entirely; it is never applied in part.

### When a server goes wrong

- **It stops.** The message bar says so once, with the last line the server
  wrote to its error output, which usually explains why. Its marks are
  removed, and molly does not restart it on its own: **Restart language
  servers** in the palette does, once you have fixed the cause.
- **It is slow.** Nothing waits for it. An answer that arrives after you have
  moved on (the cursor elsewhere, or the text changed) is dropped, so a slow
  definition lookup cannot pull you away from where you have since gone. A
  completion still arrives if all you did was keep typing the word.
- **It is noisy.** Only the errors and warnings it shows as messages reach the
  message bar; its log does not.

### What it deliberately does not do

- **Every change sends the whole file.** The protocol also allows sending
  only what changed, but a mistake there would leave the server and molly
  with different text and no sign of it. A source file goes down a
  pipe in microseconds; a file over 16 MB is not sent to a server at all.
- **Completion does not open while you type a word** unless you turn on
  `[editor] complete while typing`, for the reason given under
  [Completion](#completion).
- **No snippets.** Completion asks for plain text, so taking a function inserts
  only its name, without placeholders for the arguments.
- **One cursor.** Completion is offered at the main cursor only; with several,
  `Alt-/` says so.
- **Problems only from open files.** A server reports on what it has been
  sent, and molly sends the files you have open; a project-wide check is the
  build's job, in the [terminal](#the-terminal).
- **Not on save, unless you ask.** `[editor] format on save` is off, and with
  it on a save waits two seconds at most.
- **No file operations.** A rename or code action that would create, move or
  delete a file is refused entirely.

## Git

In a file git tracks, a narrow column beside the text marks what differs from
the last commit: a green bar `▎` for an added line, an amber one for a changed
line, and a red `▸` where lines were taken out. It compares the *buffer* with
what is committed, not the file on disk, so an unsaved edit is marked a second
and a half after you stop typing; it is worked out again when the file is
opened and saved too. A file git does not track has no column at all, and one
it does always has one, so the text does not shift sideways at the first edit.
`[view] git gutter = off` turns it off.

Two more, from the palette:

- **Git blame line** says who last changed the cursor's line, when, and in
  which commit: `3f2a91c Ann Lee, 2026-09-30: Fix the parser`. It reads the
  file as it is on disk, so it asks you to save first when there are unsaved
  changes.
- **Git file history** opens a buffer with the file's commits, newest first,
  one a line: a hash, a date, an author and a summary. `Enter` on one opens that
  commit as a diff, in a buffer of its own.

Everything is asked of the `git` program, through the same path as the shell
filter, so a slow repository is held to the [shell timeout](#configuration) and
`Esc` stops it; without git, nothing is shown.

## Saving and Quitting

| Key | Action |
| --- | --- |
| `Ctrl-S` | Save the file |
| `Ctrl-Q` | Quit |

If the buffer has no file name yet, `Ctrl-S` first asks where to save. An
empty answer cancels the save, so the buffer stays unnamed and `Ctrl-S` will
ask again next time.

### How a save is written

Molly does not write over your file directly. It creates a uniquely named
temporary file beside the target, writes the buffer there, flushes it to disk,
and then renames it into place. A save interrupted by a crash, a full disk or a
power failure therefore leaves the original file untouched. The temporary name
is unpredictable and created exclusively, so it cannot collide with a file of
your own and cannot be redirected by anything else in the directory.

A rename only asks for a writable directory, so Molly uses one only when the
temporary file can stand in for the original completely. If it cannot, Molly
rewrites the file where it is and says `written (in place, not atomic)`. Your
work is saved, and the file keeps everything below, but that write has no crash
protection. That happens when:

- the file has other **hard links**, which go on seeing the same contents;
- it belongs to someone else, or to a group you are not in, as with a teammate's
  group-writable file, whose owner and group do not change;
- it has **extended attributes** or an ACL that the temporary file would not have;
- its directory is not writable, or takes no new file.

Molly will not save over a file that is not yours to write, even where a rename
would get through. It says why and offers another name, as it does for anything
that is not a regular file, such as a directory or a FIFO. `Esc` at that prompt
cancels the save. A read-only file of your own asks first, since the permission
is yours to change, and so does naming a new buffer after a file that exists. A
name you type becomes the buffer's only once a save to it has worked.

If there is no room on the disk for the temporary file, the save fails and your
file is left as it was. The mode is kept, including setuid and setgid.

When the file name is a symbolic link, Molly follows it and replaces the file
it points at, leaving the link itself alone.

If another process has changed the file on disk since Molly opened it (or
since the last save), `Ctrl-S` asks before overwriting that change. Answering
`n` aborts the save; `y` overwrites it.

When there are unsaved changes, the first `Ctrl-Q` lists the unsaved buffers and
offers Save All, Discard or Cancel. A clean session quits on the first `Ctrl-Q`.

### Suspending

**Suspend to shell**, in the palette, stops molly the way `Ctrl-Z` stops any
program in a shell: the screen is handed back as it was found, and the shell's
`fg` brings molly back, redrawn, with everything as you left it. It is not on
`Ctrl-Z`, because `Ctrl-Z` is undo in molly, as in every CUA editor. Bind it to
a key in `[keys]` if you use it often.

If Molly is stopped from outside instead (its terminal window is closed, or it
receives `SIGTERM`, `SIGHUP`, `SIGINT` or `SIGQUIT`), it restores your
terminal's settings on the way out, so the shell you return to echoes and edits
normally.

### Recovery of unsaved work

If Molly is killed by one of those signals, or exits because it ran out of
memory, and the buffer had unsaved changes, it writes what was in the buffer
to a recovery file before it goes. The recovery file sits next to the file you
were editing, named after it with a `.molly-recover` suffix (for an unnamed
buffer it is `molly-recover.<pid>` in the current directory, and
`molly-recover.<pid>-<n>` for any further unnamed buffers, so two scratch
buffers cannot write over each other's data). Every buffer with unsaved changes
gets one, not only the one that was on screen.

The next time you open that file, Molly notices the recovery file and asks:

    Unsaved changes from a crashed session. R = restore | any key = drop

`R` loads the recovered contents over the buffer and marks them unsaved, so
`Ctrl-S` keeps them; until you save, the buffer holds the recovered data, not
what is on disk. Any other key continues with the file as it is on disk.
Either way the recovery file is then removed by the first successful save or
`Ctrl-Q` quit, so decide at the prompt; after that the data is gone. (This
prompt only covers named files; for an unnamed buffer, inspect the
`molly-recover.<pid>` file yourself and rename it over whatever it belongs to.)

After saving, the undo history is kept, but the dirty flag is cleared.

## Keyboard Shortcuts

    F10      Open the menu bar
    Alt-F/E/S/V/G/W/H  Open the File/Edit/Search/View/Go/Window/Help menu
    Ctrl-Space  Command palette (Alt-X does the same)
    Ctrl-S   Save file
    Ctrl-O   Open a file (directory browser)
    Alt-O    Open a file you opened recently
    Ctrl-N   New buffer
    Ctrl-W   Close this buffer
    Ctrl-PgDn / Ctrl-PgUp  Next / previous buffer
    F7       Split, new window to the right
    F8       Split, new window below
    F6 / Shift-F6  Next / previous window
    F9       Close this window
    Alt-T    To the terminal, opening one if there is none; from it, back
    Ctrl-Shift-Arrow  Move the border beside, above or below this window
    Ctrl-Q   Quit (asks what to do with unsaved changes)
    Ctrl-B   List the open buffers
    Ctrl-F   Open the Find dialog (case-insensitive by default)
    F3       Find next instance of the search
    Shift-F3 Find previous instance of the search
    Ctrl-N/P The same two, for fingers that already know them
    Alt-R    Toggle regular-expression search
    Ctrl-R   Search and replace (next or all)
    Ctrl-E   Find in files
    F4 / Shift-F4  Next / previous find-in-files match
    Ctrl-G   Go to line
    Ctrl-]   Go to the bracket matching this one
    Alt-J    Set a bookmark (then a digit, 0-9)
    Alt-0..9 Go to that bookmark
    F2 / Shift-F2  Next / previous problem a language server found
    F12      Go to the definition of what is under the cursor
    Alt-Left Back to where the last jump left from
    Alt-Right  Forward again
    Alt-K    What is under the cursor, from the language server
    Alt-/    Complete the word before the cursor
    Shift-F12  Every place the name under the cursor is used
    Ctrl-F2  Every problem in every open file, kept up to date
    Alt-P    The signature of the call the cursor is in
    Alt-N    Rename what is under the cursor, everywhere
    Alt-Q    Format the file, or the selection, with the language server
    Alt-Enter  Code actions: fixes and refactorings, from the language server
    Ctrl-A   Select everything
    Ctrl-L   Select the current line
    Ctrl-C   Copy the selection
    Ctrl-X   Cut the selection
    Ctrl-V   Paste the clipboard at the cursor
    Ctrl-T   Swap the two characters at the cursor
    Ctrl-Left / Ctrl-Right  Move a word left / right
    Alt-Backspace  Delete the word before the cursor
    Ctrl-Del Delete the word after the cursor
    Ctrl-K   Delete to the end of the line
    Alt-C    Duplicate the line or the selected lines
    Alt-Up / Alt-Down  Move the line or the selected lines up / down
    Ctrl-J   Join the next line onto this one
    Alt-;    Comment the lines out, or back in
    Ctrl-D   Add a cursor at the next occurrence of the word or selection
    Alt-D    Skip this match, take the next one
    Alt-U    Take the last cursor back
    Alt-A    A cursor on every match at once
    Ctrl-Down / Ctrl-Up  Add a cursor on the row below / above
    Alt-Shift-arrows  Start or grow a block selection
    Tab      Indent the selected rows (or insert one indent step)
    Shift-Tab  Dedent the selected rows, or the cursor's row
    Alt-|    Filter the selection (or the file) through a command
    Alt-!    Insert a command's output at the cursor
    Ctrl-Z   Undo the last change
    Ctrl-Y   Redo an undone change
    Alt-(    Start or stop recording a macro
    Alt-)    Play the macro back
    F1       Show this help screen (Ctrl-? does the same)
    F5       Repaint the screen
    Ctrl-U   Toggle the line-number gutter
    Alt-M    Toggle the menu bar
    Alt-B    Toggle the tab bar
    Alt-Y    Toggle syntax highlighting
    Alt-Z    Toggle soft wrap
    Alt-.    Toggle showing tabs and trailing blanks
    Alt--    Fold the block the cursor is in, or open the fold
    Tab      In a dialog: move to the next field, checkbox or button
    Shift-Tab  In a dialog: move back to the previous one
    Space    In a dialog: tick the checkbox or press the button with the keyboard
    Shift    Hold with arrows / Home / End / PgUp / PgDn to select
    Shift    Hold while dragging to select with the terminal, not molly
    Shift    In a terminal whose program has the mouse: hold to take it back
    Esc      Cancel the current prompt, menu or dialog; or go back to one cursor

To see the live keymap rather than this list:

    MOLLY_DUMP_KEYMAP=1 molly

## Configuration

Molly reads one file at startup:

```text
~/.config/molly/mollyrc
```

If `XDG_CONFIG_HOME` is set, molly looks under that instead of `~/.config`.
There is no file to begin with, and molly runs normally without one. Every
setting below lists its default, which is how molly behaved before there was a
configuration file.

### What the file looks like

INI-style: sections in square brackets, then `key = value`. Blank lines are
ignored, and so is anything after a `#` or a `;` at the start of a line.

```ini
[editor]
tab width = 4

[view]
line numbers = on
menu bar = on

[input]
mouse = on

[search]
match case = off
regular expression = off

[keys]
F9 = file.save
Ctrl-S = none
```

Names are matched on their words, so `line numbers`, `Line-Numbers` and
`line_numbers` all reach the same setting. A flag accepts `on`/`off`,
`true`/`false`, `yes`/`no` and `1`/`0`. Molly writes `on` and `off`.

### The settings

| Section | Setting | Default | Effect |
| --- | --- | --- | --- |
| `editor` | `tab width` | `8` | Columns a tab character occupies, 1 to 16. Changing it re-renders the open file immediately. |
| `editor` | `insert spaces` | `off` | `Tab` inserts spaces as far as the next tab stop rather than a tab character. See [Indentation](#indentation). |
| `editor` | `auto indent` | `on` | A new line starts at the indentation of the line above it. |
| `editor` | `smart home` | `off` | `Home` goes to where the line's text starts, then to column 0. See [Editing](#brackets-quotes-and-home). |
| `editor` | `auto close` | `off` | Typing a bracket or quote types its pair, and wraps a selection in it. |
| `editor` | `undo steps` | `32768` | How many undo steps to keep. |
| `editor` | `language servers` | `on` | Start the language server a file's language names, where it is installed. See [Language Servers](#language-servers). |
| `editor` | `format on save` | `off` | Saving formats the file with its language server first. See [Formatting](#formatting). |
| `editor` | `complete while typing` | `off` | The completion list opens by itself as a word is typed. See [Completion](#completion). |
| `view` | `line numbers` | `off` | Show the line-number gutter (`Ctrl-U` toggles it). |
| `view` | `menu bar` | `on` | Give the menu bar its row (`View ▸ Menu Bar` toggles it). |
| `view` | `tab bar` | `on` | Show a tab per open file (`Alt-B` toggles it). Only ever drawn with two or more buffers open. |
| `view` | `syntax` | `on` | Color the text by filetype (`Alt-Y` toggles it). See [Syntax Highlighting](#syntax-highlighting). |
| `view` | `italic` | `on` | Draw comments in italic, on terminals that can (not the Linux console, not under a `screen` TERM). See [Syntax Highlighting](#syntax-highlighting). |
| `view` | `soft wrap` | `off` | Wrap long lines at the window edge instead of scrolling sideways (`Alt-Z` toggles it). See [Soft Wrap](#soft-wrap). |
| `view` | `whitespace` | `off` | Draw tabs as arrows and trailing blanks as dots (`Alt-.` toggles it). See [Seeing More](#seeing-more). |
| `view` | `indent guides` | `off` | A faint bar at each tab stop inside a line's indentation. |
| `view` | `current line` | `off` | Tint the line the cursor is on. |
| `view` | `highlight word` | `off` | Tint the word under the cursor wherever else it appears on screen. |
| `view` | `ruler` | `0` | Tint this column, to show a line length; 0 for none. |
| `view` | `git gutter` | `on` | Mark lines changed since the last commit, for files git tracks. See [Git](#git). |
| `shell` | `timeout` | `10` | Seconds a filter command may run before molly stops it, 1 to 3600. See [Filtering Through a Command](#filtering-through-a-command). |
| `shell` | `output limit` | `8192` | Kilobytes of output a filter command may produce, 1 to 1048576. |
| `input` | `mouse` | `on` | Report the mouse to molly. Turning it off hands clicks and drag-select back to the terminal for good, rather than only while `Shift` is held. See [The Mouse](#the-mouse). |
| `search` | `match case` | `off` | Starting state of the Find dialog's **Match case** checkbox. |
| `search` | `regular expression` | `off` | Starting state of its **Regular expression** checkbox. |

### Rebinding keys

`[keys]` takes a chord on the left and a command name on the right. `none`
leaves the chord unbound.

```ini
[keys]
F9 = file.save
Ctrl-L = view.repaint
Shift-F3 = search.prev
Ctrl-S = none
```

A binding in the file replaces molly's own, so `Ctrl-L = view.repaint` moves
repaint back onto the key olly had it on. The menus and the help screen show
your binding rather than the default.

Command names are the ones in the right-hand column of the command palette
(`Ctrl-Space`), and `MOLLY_DUMP_KEYMAP` prints every binding
with the command it runs.

A chord is written as modifiers and a key, joined by `-`, in any order and in
any case:

| Form | Examples |
| --- | --- |
| Control | `Ctrl-S`, `ctrl-s`, `Ctrl-?`, `Ctrl-Space` |
| Alt | `Alt-X`, `alt-r` |
| Function keys | `F1` … `F12` |
| Navigation | `Left`, `Right`, `Up`, `Down`, `Home`, `End`, `PgUp`, `PgDn`, `Del`, `Backspace`, `Enter`, `Tab`, `Shift-Tab`, `Esc` |
| Shifted | `Shift-F3`, `Shift-Right`, `Shift-PgDn` |
| Control on those | `Ctrl-PgUp`, `Ctrl-PgDn`, `Ctrl-F5`, `Ctrl-Right` |
| A bare character | `a`, `}` |

Two limits come from the terminal, not from molly:

- **`Shift` can only be named on the function keys and the navigation keys**,
  and `Ctrl` on those keys means something different from `Ctrl` on a letter.
  A terminal reports a modifier as a parameter inside those sequences, so
  `Ctrl-PgUp` and `Shift-Right` arrive as themselves. On a letter there is no
  parameter: `Shift-A` *is* `A`, and `Ctrl-Shift-L` is the same single byte as
  `Ctrl-L`. Molly refuses chords that could never fire, so no binding
  silently fails to work. `Ctrl-Alt-` is refused for
  the same reason.
- **`Shift` with an arrow still extends the selection.** Nothing is bound to
  `Shift-Right`, so it falls through to `move.right` with the modifier noted,
  and that extends the selection. If you bind `Shift-Right` to something, that
  key no longer extends the selection.

The `Alt`-letter menu titles cannot be rebound. There are seven today, one per
menu, and the set is taken from the menu table. The menu bar underlines the
letter whether or not it still opens the menu, so rebinding one would make the
bar wrong.

Multi-key sequences (`Ctrl-K Ctrl-F`) are not supported yet.

### When molly disagrees with the file

A mistake in `mollyrc` produces a warning; molly still starts. Molly reads the
rest of the file, applies every line it understood, and reports the first
problem on the message bar with a count of how many there were:

```text
mollyrc:4: tab width must be 1 to 16, not 99 (2 problems)
```

Every warning names the line it came from. `MOLLY_DUMP_CONFIG` prints the whole
parsed configuration and every warning without starting the editor, which is
the quickest way to check a file you have just edited.

### The Options dialog

**View ▸ Options…** lists every setting in the table above on one screen, with
its section, its current value and, for the selected one, what it does:

     editor  tab width                         8
     editor  insert spaces                   off
     shell   timeout                          10
    [editor] columns a tab character occupies (1-16)

| Key | Action |
| --- | --- |
| `↑` `↓` `PgUp` `PgDn` | Choose a setting |
| `Space`, or a click | Turn an on/off setting on or off |
| `←` `→` | Turn it off / on, or step a number down / up |
| Digits | Type a number; `Backspace` takes one back |
| `Enter` | Keep the changes for this session |
| `Ctrl-S` | Keep them and save them to `mollyrc` |
| `Esc` | Put back everything as it was when the dialog opened |

Changes take effect as you make them, so you can see what a setting does before
deciding to keep it; `Esc` undoes all of them at once. A typed number takes
effect when you move off it or close the dialog, and is held to the setting's
range. The dialog changes the `[editor]` defaults: a `[syntax <name>]`
override in your `mollyrc` still wins for that filetype.

### Saving settings

**View ▸ Save Settings** writes the current state of everything in the table
above to `mollyrc`, creating `~/.config/molly/` if it is not there. To keep a
setting, change it the usual way (`Ctrl-U` for the gutter, `View ▸ Menu Bar`
for the bar, the Find dialog's checkboxes) and then save.
Key bindings you put in the file by hand are written back too; molly's own
defaults are not, so a later version can move a key you never changed.

The write is atomic: molly writes a temporary file beside the target and renames
it, so an interrupted save cannot leave a half-written file that fails to parse
the next time molly starts.

A save rewrites the whole file. Your comments, blank lines and any setting this
version of molly does not recognize are gone afterwards. Molly puts its own
comments back, explaining each setting and its default.

## Environment Variables

| Variable | Effect |
| --- | --- |
| `TERM`, `COLORTERM` | Choose the 256-color or the 16-color palette; see [Colors](#colors) |
| `MOLLY_DUMP_KEYMAP` | Print the keymap as `chord<TAB>command` lines and exit, without touching the terminal. Every binding in it was parsed from chord text at startup and printed back, so it is both a reference for what is bound and how the test suite checks the chord code. `[keys]` from your `mollyrc` is applied first, so this is also how to check a rebinding. |
| `MOLLY_DUMP_CONFIG` | Print the parsed configuration (the file's path, every setting's value, the key bindings it asked for and any warnings) and exit, without touching the terminal. See [Configuration](#configuration). |
| `MOLLY_CONFIG` | Read configuration from this path instead of `~/.config/molly/mollyrc`. Set to the empty string to read none at all, which is what the test suite does so that its results do not depend on the configuration of the machine it runs on. |
| `MOLLY_UNDO_STEPS` | Lower the undo history's step limit, which is how the test suite exercises its eviction. |
| `MOLLY_STATE` | Keep molly's state (the [recent files](#recent-files) list) in this directory instead of `$XDG_STATE_HOME/molly`. Set to the empty string to keep none; the test suite does, so that running it leaves nothing behind and does not see what you opened. |
| `MOLLY_LSP` | Set to `off` to start no language server whatever the configuration says. The test suite sets it, so that a machine with clangd installed draws the same screens as one without; its own language-server tests set it back and name a fake server instead. |

## File Format Notes

- Line endings are preserved, line by line. A file with mixed `CRLF` and `LF`
  endings keeps them on every line you do not edit, and a new line takes the
  ending of the file's first line. A brand-new file uses `LF`. Exactly one line
  ending comes off a line when it is read, so any other carriage return is text
  and is saved as it was.
- A trailing newline is preserved too: if the file you opened ended without
  one, the save leaves it without one. A brand-new file is given a trailing
  newline.
- Text is stored and saved as raw bytes, so UTF-8 (and any other encoding)
  round-trips unchanged. The cursor, `Backspace` and `Delete` operate on whole
  UTF-8 characters rather than individual bytes, and the `Col` indicator counts
  characters. Display width is honored: East Asian wide characters take two
  columns, combining marks take none, so the cursor and horizontal scrolling
  stay aligned with what the terminal shows.
- A byte that is not part of a whole UTF-8 character is one character by itself,
  and the cursor can stop on it from either side.
- Control characters and malformed UTF-8 are never sent to your terminal, where
  an escape sequence in a file could set the window title or the clipboard, or
  repaint the screen. In the text each one draws as a reversed `?`, one column
  wide. In a file name or a message it is a plain `?`. The file keeps the real
  bytes.
- Only regular files open. A FIFO, a device or a directory named on the command
  line ends Molly with the reason. From the file browser, the current buffer stays
  and the message bar says why, as it does for a file you may not read.
- The status bar shows the file name capped at 20 characters.
- A file is read into memory whole, and Molly needs about one and a
  half times its size: a 743 MB log of ten million lines opens in under half a
  second and takes about 1.1 GB. Lines you have not changed are not copied, so
  scrolling, searching and editing a few places in a big file add little to
  that. A file larger than the memory you have will not open.
- With [soft wrap](#soft-wrap) on, the first time a window lays out a very long
  file it measures every line, and does it again whenever the window's width
  changes: about two seconds for ten million lines. Unwrapped, nothing walks
  the file.
