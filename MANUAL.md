# Molly — A Command-Line Text Editor

> **Note:** molly is at `0.1.0-dev`. The eight things this note used to say it
> did not have — more than one file open at once, regular-expression search,
> syntax highlighting, a configuration file, window splits, a shell filter,
> multi-cursor editing and soft wrap — it now has. What is still missing is on
> the other side of the line: no integrated terminal and no language server.
> This manual describes what is there today and is accurate. For what is
> coming, and why in that order, see [`ROADMAP.md`](ROADMAP.md).

Molly is a small, dependency-free text editor for the terminal, written in C.
It provides modeless editing in the style of tools like nano — type to insert,
the arrow keys move around, and the `Ctrl` combinations do the work — with the
discoverability of a GUI editor on top: every command has a name, a menu entry
and a searchable label, so nothing is reachable only by knowing a secret key.

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
- [Help Screen](#help-screen)
- [Line Numbers](#line-numbers)
- [Soft Wrap](#soft-wrap)
- [Selecting Text](#selecting-text)
- [Several Cursors](#several-cursors)
- [Searching](#searching)
- [Regular Expressions](#regular-expressions)
- [Search and Replace](#search-and-replace)
- [Undo and Redo](#undo-and-redo)
- [Opening Another File](#opening-another-file)
- [Several Files at Once](#several-files-at-once)
- [Filtering Through a Command](#filtering-through-a-command)
- [Finding in Files](#finding-in-files)
- [Window Splits](#window-splits)
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
**automatically falls back to** `~/.local/bin`, which is usually already on
your `PATH` — no `sudo` required.

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
and the status bar says so rather than dropping them silently. To change
files from inside Molly, use `Ctrl-O` (see
[Opening Another File](#opening-another-file)).

If the file does not exist, Molly opens an empty buffer rather than failing.
A path that exists but cannot be read in full — a directory, for example — is
refused with an error instead of being opened as an empty or partial buffer
that a later save would write back over the original.
The status bar (the highlighted line at the bottom) shows the file name, the
number of lines, whether the buffer is modified, and the cursor position as
`Ln <line>, Col <column>`.

## Things Worth Knowing First

Four things about molly will surprise you if nobody says them first.

**The mouse is on.** Molly asks the terminal for mouse reporting at startup, so
clicks, drags and the wheel go to molly instead of to your terminal. That means
your terminal's own swipe-to-select and middle-click-to-paste stop working inside
molly. **Hold `Shift`** while dragging and most terminals hand the selection back
to themselves — that is the standard bypass and it is how you copy text out of
molly with the terminal rather than with `Ctrl-C`. There is no switch to turn the
mouse off yet; the configuration file that will carry one is a Phase 2 item.

**Alt keys need the terminal to send them as `Esc`-prefixed.** That is the
default in xterm, GNOME Terminal, Konsole, kitty, Alacritty, WezTerm and tmux.
If your terminal is set to "Alt sends the eighth bit" (sometimes called *8-bit
meta*) instead, `Alt-F` and friends will not reach molly. Everything they do is
also on `F10` and in the menus, so nothing is lost either way.

**`Ctrl-Space` may not survive the trip.** It arrives as a NUL byte, which most
terminals send and a few swallow. `Alt-X` opens the command palette too, for
exactly that reason.

**Every binding is remappable**, in the `[keys]` section of your
[configuration](#configuration) file, except the `Alt`-letter menu titles — seven
today, one per menu.
The bindings below are CUA, which is what most editors written in the last
thirty years use: `Ctrl-A` selects everything, `Ctrl-L` selects the line,
`Ctrl-T` transposes the two characters at the cursor, `F1` is help and
`F3`/`Shift-F3` step through matches. If you have olly's keys in your fingers,
`Ctrl-L = view.repaint` puts repaint back.

## The Menu Bar

The band across the top of the screen is molly's main answer to "what can this
thing do?". Seven menus: **File**, **Edit**, **Search**, **View**, **Go**,
**Window** and **Help**. (*Tools* and *Options* come later; their `Alt` letters
are held already, so adding them will not move anything.)

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

Nine letters are spoken for on `Alt` — `F E S V G W H`, one for each menu, plus
`T` and `O` held for the two that do not exist yet. **Every other `Alt`+letter
is free for commands**, which is the point of reserving no more than the
titles.

Every entry is built from molly's command table, so a menu cannot drift out of
step with the editor: the label, the shortcut shown down the right-hand side,
whether the item is available and whether it is ticked all come from the command
itself. **An item molly cannot run right now is greyed out, skipped by the
arrows, and deaf to its own letter** — `Paste` with an empty clipboard, `Find
Next` before you have searched for anything. Items that toggle something show a
tick when it is on.

**A menu taller than the screen scrolls.** The Edit menu wants sixteen rows and
the View one thirteen, which is more than a short terminal has to give. The box
is kept inside the text area — it never covers the status bar, and neither does
its drop shadow — and the items that do not fit are scrolled rather than drawn
off the bottom edge. A small `▴` or `▾` on the border says there is more that
way; the arrows and `Home`/`End` scroll the list as the highlight reaches the
end of it, and clicking a `▴` or a `▾` shows the next item past that edge. An
item's underlined letter runs it whether or not there was room to draw the item,
so a short terminal never puts a command out of reach. (Below about eight rows
there is not even room for that: the box keeps a border, one item and a border,
and takes the status row if it must — one item beats none.)

Running a command from a menu is the same as running it from its key, down to
the details: choosing **Quit** from the File menu with unsaved changes opens the
same dialog `Ctrl-Q` does.

The bar can be switched off with **View → Menu bar**, which gives its row back to
the text — worth having on a 24-row terminal. While it is hidden, opening a menu
draws the band *over* the first line of the file rather than reflowing the whole
screen down a row.

## The Command Palette

`Ctrl-Space` (or `Alt-X`) opens a list of every command molly has, with a filter
box at the top and the key each one is bound to beside it. It is the way to reach
something without knowing its key and without hunting through the menus — and
unlike the menus, which are curated, **it lists everything**.

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
are ranked, roughly, by how much they look like an abbreviation someone would
actually type — letters that start a word count for more than letters stranded in
the middle of one, and a run of adjacent letters counts for more still. The
search also covers each command's internal name and its group, so `clip` finds
**Copy**, **Cut** and **Paste** even though none of them says "clipboard".

Commands molly cannot run at the moment are **listed and dimmed rather than
hidden**, so the palette never looks as though a command has gone missing.
Choosing one says why it cannot run and leaves the palette open.

## The Mouse

| Action | Effect |
| --- | --- |
| Click in the text | Put the cursor there |
| Drag in the text | Select |
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
`Ctrl-C` and `Ctrl-X` work on it without caring how it was made.

If you would rather keep your terminal's own click-and-drag behavior, put
`mouse = off` in the `[input]` section of your [configuration](#configuration)
file. Molly then never asks the terminal for mouse reports at all, and every
row of the table above stops applying — which is the trade: molly's mouse and
the terminal's cannot both have the same gesture.

## Colors

Molly draws in one of two palettes and picks between them at startup: a
**256-color** one if `COLORTERM` says `truecolor`/`24bit` or `TERM` mentions
`256color`, and a **16-color** fallback otherwise. Nothing is lost at sixteen —
the fallback is a different table, not a different renderer.

Dialogs and menus **paint their own background** rather than inheriting the
terminal's, so they read as panels floating above the text and look the same
wherever molly runs. The text area itself still uses your terminal's own
foreground and background, so molly fits a light or a dark theme without being
told which it is.

If a dialog looks wrong, check what your terminal claims:

    echo "TERM=$TERM COLORTERM=$COLORTERM"

## Syntax Highlighting

Molly colours the text according to the file's type, which it works out from the
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

`.h` is treated as C rather than C++, which is a judgement rather than a fact
— it is the one extension both claim.

When molly recognises a file, the status bar names the language in brackets:

    [2/3] parser.c - 410 lines [c] (modified)

That is there to answer the question the colours raise. A file with no colour is
either one molly has no definition for or one you have turned highlighting off
for, and the bracket tells you which.

`Alt-Y`, `View ▸ Syntax highlighting`, or `syntax = off` in the `[view]` section
of your [configuration](#configuration) file turn it off. A selection always
wins over the colouring: reverse video on top of a keyword's own colour is
unreadable, and what you are being told at that moment is that the text is
selected.

Comments are drawn in italic as well as in their own dimmer colour, so they
read as comments even where your terminal's palette puts that colour close to
the text's. Molly leaves italic off where `TERM` names a terminal that would
draw it as something worse: the Linux console, which paints italic in a
substitute colour, and a `screen` or `screen-256color` TERM, under which tmux
draws italic as reverse video and a comment looks selected. If you run tmux,
`set -g default-terminal tmux-256color` gives you italic back. `italic = off` in
`[view]`, or **View ▸ Options…**, turns it off everywhere; the colour stays.

### Teaching molly a language

A `[syntax <name>]` section in your [configuration](#configuration) file defines
a language, or adds to one molly already knows. Molly's own definitions are
written with exactly these rules, so there is nothing a built-in can do that
yours cannot.

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
| `keywords` | Words coloured as keywords |
| `types` | Words coloured as type names — the second word class |
| `line comment` | One or more markers that start a comment to the end of the line |
| `block comment` | Pairs of open and close markers, for comments that span rows |
| `strings` | Single characters that delimit a string |
| `string escape` | The character that escapes a delimiter inside a string |
| `multiline strings` | Pairs, for string regions that span rows (Python's `"""`) |
| `numbers` | `on` or `off` — whether number literals are coloured |
| `preproc` | A line-leading marker whose whole line is a directive |
| `case sensitive` | `off` makes keyword matching ignore case (AutoIt) |
| `heading` | A line-leading marker whose whole line is a heading (Markdown) |
| `fence` | Pairs that open and close a verbatim region (Markdown's triple backtick) |
| `code span` | A delimiter for inline verbatim text (Markdown's backtick) |
| `emphasis` | Single characters that delimit emphasis (Markdown's `*` and `_`) |
| `tab width` | Columns a tab occupies in this language's files, 1 to 16 |
| `insert spaces` | `on` or `off`, for this language only |
| `auto indent` | `on` or `off`, for this language only |

A section naming a language molly ships adds to it, so

```ini
[syntax c]
types = mytype_t othertype_t
```

colours two more words in every `.c` file without disturbing anything else. A
rule molly does not recognise is a warning naming the line, as any other
configuration mistake is.

The last three rules in the table are not about colour at all: they are the
[indentation](#indentation) settings, answered per language. A section that
leaves one out keeps whatever `[editor]` says, so

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

- **An unterminated string stops at the end of its row.** A stray quote should
  not colour the rest of the file as a string — that turns a typo into what
  looks like a disaster.
- **It is not a parser.** There is no language in which molly knows that a name
  is a function rather than a variable, because that needs a grammar per
  language rather than a table of rules.
- **Only the extension picks the language.** A `#!/usr/bin/env python` line on
  an extensionless script is not read.

## Prompts

Anything molly asks you on one line — a line number, a file name to save as, a
replacement — is an editable field on the bottom line, with a real cursor in it.
(Find asks for more than one thing at a time and so gets a dialog instead; see
[Searching](#searching).)

| Key | Action |
| --- | --- |
| `←` / `→` | Move within the answer |
| `Home` / `End` | Start / end of the answer |
| `Backspace` / `Delete` | Delete before / at the cursor |
| `Enter` | Accept |
| `Esc` | Cancel |

An answer longer than the room for it scrolls sideways, so the hint at the end of
the line stays where it is. On a terminal too narrow for both, the hint is dropped
and the whole width goes to the answer. Molly has no prompt history yet.

## Editing

Molly is modeless: whatever you type is inserted at the cursor.

| Key | Action |
| --- | --- |
| Any character | Insert at the cursor |
| `Tab` | Insert one indent step, or indent the selected rows — see [Indentation](#indentation) |
| `Shift-Tab` | Dedent the selected rows, or the cursor's row |
| `Enter` | Split the line / insert a new line, keeping the indentation |
| `Ctrl-D` | Add a cursor at the next match — see [Several Cursors](#several-cursors) |
| `Backspace` | Delete the character before the cursor, or one indent level inside the indentation |
| `Delete` | Delete the character at the cursor |

If the cursor is on the empty row below the last line of text, typing creates
that row for you automatically. If the cursor is at the very start of a line,
`Backspace` joins the current line onto the previous one; `Delete` at the very
end of a line joins the next line onto it.

## Indentation

Three questions, each answerable in `[editor]` for everything or in
`[syntax <name>]` for one language — see [Configuration](#configuration) and
[Teaching molly a language](#teaching-molly-a-language).

| Setting | Default | What it decides |
| --- | --- | --- |
| `tab width` | `8` | How many columns a tab character occupies on screen |
| `insert spaces` | `off` | Whether `Tab` types spaces instead of a tab |
| `auto indent` | `on` | Whether `Enter` starts the new line under the old one |

**`tab width` is about the screen; `insert spaces` is about the file.** Changing
the width changes how a tab already in your file is drawn and nothing else.
Turning `insert spaces` on changes what `Tab` writes — so it changes the bytes
a save puts on disk, which is why it is off until you ask for it: a file
indented with tabs should not quietly start gaining spaces because you opened
it in molly.

### What one `Tab` does

With `insert spaces` off, `Tab` types one tab character, as it always has.

With it on, `Tab` inserts however many spaces reach the next tab stop — not a
fixed number. From column 0 of an eight-column stop that is eight spaces; from
column 2 it is six, so the text that follows lands on the stop either way. A
tab already on the row counts as the columns it occupies rather than as one
character, so the arithmetic is right on a row indented with both.

The whole run is one undo step: `Ctrl-Z` after a `Tab` takes back the step, not
one space of it.

### Indenting a block

Select text that reaches into a second row and `Tab` indents every row the
selection touches instead of replacing it. `Shift-Tab` takes a level back off.
The selection survives both, so you can press `Tab` twice to indent twice.
**Edit ▸ Indent** and **Edit ▸ Dedent** are the same two commands from the
menu.

- A selection that stops at **column 0** of a row has not reached into that
  row, and that row is left alone. This is what lets you select whole lines by
  dragging down the left edge without indenting the line you stopped on.
- **Blank rows in the middle of a block are left alone.** An indent on an empty
  row is trailing whitespace.
- With a selection on **one row only**, `Tab` replaces it, as it always has.
- `Shift-Tab` needs no selection: on its own it dedents the row the cursor is
  on. With [several cursors](#several-cursors), it dedents each cursor's row.
- Either way the whole block is **one undo step**.

A dedent takes the row's indentation back to the **previous tab stop**, and it
removes whole characters from the end of the whitespace run — the end nearest
the text. At a width of 4:

- `······alpha` (six spaces) becomes `····alpha`, not `··alpha`.
- `→··alpha` (a tab and two spaces) loses the two spaces.
- `→→alpha` loses the second tab.

A tab that straddles the stop is removed whole, which undershoots it: half a
tab is not something to leave behind. Rows with no indentation are left as they
are rather than being an error.

### What `Backspace` does in the indentation

With the cursor in a row's leading whitespace and nothing but whitespace behind
it, `Backspace` deletes back to the previous tab stop rather than one space at
a time — so one press takes a level off a row indented with spaces, instead of
four or eight.

Everywhere else it is the ordinary `Backspace`: one character. That includes
the case where only one character stands between the cursor and the stop, which
is what keeps it invisible in a tab-indented file — there, one level already
*is* one character.

### What `Enter` does

With `auto indent` on, the new line starts with the same leading whitespace as
the line you left — tabs as tabs, spaces as spaces, mixed as it was. The split
and the indentation are one undo step, so `Ctrl-Z` takes back the whole
gesture rather than leaving an indented blank line behind.

Only the whitespace *behind the cursor* is copied, and the text that moves down
keeps its own. The rule is that the text does not change column:

- `Enter` at the end of `····alpha` gives you a new line starting at column 4.
- `Enter` in the middle, at `····alpha|beta`, puts `beta` at column 4 too.
- `Enter` at column 0 copies nothing, so it inserts a blank line above —
  which is how you get one.
- `Enter` inside the indentation, at `··|··alpha`, leaves `alpha` at column 4
  where it was, on a row of its own.

Text that arrives from somewhere else is never re-indented: pasting
(`Ctrl-V`) and [filtering through a command](#filtering-through-a-command) both
insert their lines as they are, so code that is already indented does not come
in indented twice.

### What it deliberately does not do

- **It does not know your language.** The new line copies the old line's
  indentation; it does not add a level after a `{` or an `if`, and it does not
  take one away before a `}`. That needs a grammar per language rather than a
  rule per filetype.
- **It does not reindent a block to match its surroundings.** `Tab` and
  `Shift-Tab` move the rows you selected by one level each; nothing works out
  what level they *should* be at, which is the same missing grammar.
- **A setting does not rewrite what is already there.** Turning
  `insert spaces` on does not convert a file's existing tabs, and turning it
  off does not convert its spaces: a setting changes what the next keystroke
  writes, never what it finds. Converting is a command of its own, below.

### Converting a file's indentation

`Edit ▸ Tabs to spaces` and `Edit ▸ Spaces to tabs` rewrite the indentation
that is already there — of the lines a selection touches, or of the whole file
when nothing is selected. Both are in the command palette too, and neither has
a key, since it is something you do to a file once rather than as you type.

They change **only the indentation**, the whitespace in front of the first
character on each line. A tab after that is left alone, because it may be part
of the content: a column in a tab-separated file, or the tab a Makefile recipe
needs. A line that is nothing but whitespace is left alone too.

The width used is the tab width in force for the file, so the text does not
move on screen. Converting to tabs puts in as many tabs as there are whole tab
stops and keeps whatever is left over as spaces — ten columns at a width of
eight is a tab and two spaces — so alignment that was not on a tab stop stays
where it was. The conversion is one undo step, and the selection is kept, so
converting a block and then converting it back is two commands.

To have `Tab` go on writing the same kind of indentation afterwards, set
[`insert spaces`](#configuration) to match.

## Moving Around

| Key | Action |
| --- | --- |
| `←` `↑` `↓` `→` | Move the cursor |
| `Home` | Go to the start of the line |
| `End` | Go to the end of the line |
| `PgUp` / `PgDn` | Move up / down a screenful |
| `Ctrl-G` | Go to a line number |
| `F5` | Repaint the screen |
| `Ctrl-U` | Toggle the line-number gutter |
| `Alt-Z` | Toggle soft wrap |

Hold `Shift` while using the arrows, `Home`/`End`, or `PgUp`/`PgDn` to move the
cursor and select text along the way — see [Selecting Text](#selecting-text).

`Ctrl-G` prompts for a line number and moves the cursor to the start of that
line. A number past the last line goes to the last line instead; a number
below 1 goes to the first line. Anything that is not a valid number leaves
the cursor where it was.

With soft wrap on, `↑` and `↓` move a row of the *screen* rather than a line of
the file, so they walk down a long line before reaching the next one.
`Home` and `End` still go to the start and end of the line — see
[Soft Wrap](#soft-wrap).

The terminal can also be resized freely while Molly is running — the display
adjusts to the new size as it happens, without needing a keypress first.

## Help Screen

`F1` — or `Ctrl-?`, which still works — opens a keyboard reference, in a framed
dialog over the editor. It lays
out in two columns on a wide terminal and drops to a single column when the
window is narrower, scrolling with the arrows, `PgUp`/`PgDn`, `Home`/`End` when
the content is taller than the screen — which at 24 rows it now is, so expect to
page down to see all of it. Resizing the terminal **while the help screen is
open** reflows it live: it re-reads the size and switches between the one- and
two-column layouts to match, without leaving the screen. Press any other key to
return to the editor.

Its key column is generated from the same keymap the editor runs on, so it cannot
tell you a key that does not work. **Help → Keyboard help** opens it too.

## Line Numbers

`Ctrl-U` toggles a line-number gutter (off by default). Each line is prefixed
with its number, right-aligned in a field as wide as the largest line number in
the file, followed by a gray vertical separator: `123 │ text`. The field width
is that number's digits plus the separator, so the column stays stable while
you type and only widens when the file crosses the next power of ten. The
separator is a continuous gray line: it runs through every row, including the
blank `~` rows below the end of the file and the welcome screen.

The gutter is drawn from the text area, not on top of it: showing it narrows the
usable text width, and the cursor, horizontal scrolling and the wide-character
alignment all shift to match. Toggling it repaints the whole screen. If the
window is too narrow to leave even one text column, the gutter is suppressed for
that frame rather than squeezing the text away.

## Soft Wrap

`Alt-Z` toggles soft wrap (off by default). **View ▸ Soft wrap** has it too,
with a tick when it is on; the command palette lists it as **Soft wrap**, and
`soft wrap = on` in `[view]` starts that way.

With it off, a line longer than the window runs off the right-hand edge and the
view scrolls sideways to follow the cursor. With it on, the line carries on at
the start of the next row of the screen, and there is no sideways scroll to do:
the two are alternatives, and turning wrap on puts the horizontal scroll back
to column zero.

Nothing in your file changes. Soft wrap is a way of *showing* a long line, so
saving writes exactly the bytes that were there before — there is no newline
anywhere it was not typed.

**Where the break goes.** After the last space that fits. A word longer than
the window has nowhere better to break and so breaks at the edge instead. The
space it broke after stays at the end of the row above, which is why a
continuation row can begin one column further on than you might expect from
counting characters.

A tab is spaces by the time the wrap sees it, so an indent is a break
opportunity like any other run of spaces: a deeply indented line whose first
word does not fit beside its indent puts that indent on a row of its own.

**The cursor.** The column a line breaks at belongs to the row below it, so a
cursor resting there is drawn at the start of the continuation rather than one
past the end of the row above. Moving down (or up) into a continuation row that
is shorter than the column you were at stops on that row's last column.

Clicking works on what you see: a click on a continuation row lands in the
middle of the line it continues, and a click past the end of one lands on that
row's last column rather than carrying on into the text below it.

**The gutter.** A wrapped line is numbered once, on the row it starts at. Its
continuation rows keep the gray separator and leave the number blank, so the
number column reads as one line per number: `12 │ a long line…` then
`   │ …and the rest of it`.

**Per window.** Wrap is a property of the window, like the gutter and the
colours. Splitting copies it to the new window, and each half can be toggled
on its own — so a split can show the same file wrapped on one side and not on
the other.

### What it deliberately does not do

- **No hard wrap.** Nothing inserts a newline into your text. If that is what
  you want, `Alt-|` through `fmt -w 72` is the tool, and it is one undo step.
- **No wrap column.** Lines wrap at the window's own width, not at a margin you
  set. A narrower wrap is a narrower window, which `F7` gives you.
- **No hanging indent.** A continuation row starts at the left edge of the text
  area rather than under the indentation of the line it continues.
- **No sticky column.** Several `↓` presses in a row each keep the column they
  land in rather than the one the first press started from — the same as with
  wrap off.

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

A selection is a rectangular-free, character-wise range: it always grows and
shrinks on whole character boundaries, so it never splits a multi-byte (UTF-8)
character. Pressing any movement key **without** `Shift`, or clicking/editing
elsewhere, clears the selection first.

Editing replaces the selection in one step: typing a character, `Enter`,
`Backspace` or `Delete` deletes the selected range and then does its normal
thing. `Ctrl-C` deliberately leaves the selection in place so you can copy the
same range again.

Pasting inserts the clipboard at the cursor (replacing any current selection);
embedded newlines split lines. A cut and a paste each undo (and redo) as a
single step with `Ctrl-Z` / `Ctrl-Y`.

Copying and cutting also push the selection to your **system (terminal)
clipboard** with the OSC 52 escape sequence, so it can be pasted into other
applications — and pasted back into Molly with the terminal's own paste binding
(right-click, `Ctrl-Shift-V`, …), which is handy when a terminal intercepts
`Ctrl-V` and would otherwise paste an unrelated application's clipboard. This
depends on your terminal honoring OSC 52 (some disable it by default for
security); where it is off, Molly's own `Ctrl-V` still pastes its internal
buffer. Molly does not *read* the system clipboard — `Ctrl-V` pastes only what
Molly last copied or cut.

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
others are drawn as coloured blocks — that is the only way a terminal can show
you more than one, and it is why they look different rather than identical.
The status bar says how many there are, which matters when some of them have
scrolled off the screen.

### Ctrl-D, one press at a time

The first `Ctrl-D` selects the word under the cursor and does nothing else, so
you see what is about to be matched before anything is multiplied. Each press
after that puts another cursor on the next occurrence, wrapping round the end
of the file. When there are no more, it says so and leaves the cursors you
have.

What it matches is **literal text, and case matters.** It is not affected by
the Find dialog's **Match case** and **Regular expression** checkboxes: those
belong to [searching](#searching), and if `Ctrl-D` read them then turning
patterns on would make a word with a `.` in it stop matching itself.

`Ctrl-D` works from a selection you made yourself, too — select any text and it
will find the next copy of exactly that, metacharacters and all. The selection
has to be on one line.

### Skipping a match, and taking one back

`Alt-D` is `Ctrl-D` without the taking: it moves the cursor you most recently
added on to the next occurrence, leaving the match it was sitting on alone. Use
it when a press lands somewhere you did not mean to change. If the match it
moves to already has a cursor on it, the two merge — two cursors in one place
would type everything twice.

`Alt-U` takes the last cursor back, one press per cursor, down to the one you
started with. Between them, a press too many and a match you did not want both
have a way out that is not `Esc` and starting again.

Both work from the cursor added **last**, not the last one in the file. That
matters once a search has wrapped round the end of the file: the newest cursor
is then at the top, and it is the one you are working at.

### A cursor on every match

`Alt-A` takes the word under the cursor — or the selection, if you have one —
and puts a cursor on every occurrence of it in the file, in one press. There is
no walk to watch, so unlike `Ctrl-D` it does not need a first press to show you
what is about to be matched: one press is the whole gesture, and `Ctrl-Z` or
`Esc` is the way back.

Pressing it again adds nothing, because every match already has a cursor. A
word that occurs once leaves you with the one cursor you had. There is a
ceiling of 512 cursors; a file with more matches than that stops there and says
so, rather than turning one keystroke into a million allocations.

### Typing with several cursors

Everything that edits reaches every cursor: typing, `Tab`, `Enter`,
`Backspace`, `Delete` and `Ctrl-V`. Where a cursor has a selection — which is
what `Ctrl-D` leaves — typing replaces it, so `Ctrl-D` `Ctrl-D` `Ctrl-D` and
then a word is how you rename three occurrences of something.

**It is one undo step.** However many cursors were involved, one `Ctrl-Z` puts
all of it back. Undo then leaves you with one cursor, because an undo step
records positions for the edit it reversed and not for the cursors you had put
on it.

Moving reaches every cursor as well: the arrows, `Home` and `End` all move the
whole set, and `Home`/`End` go to each cursor's *own* line edge, which is the
point of a column of them on ragged lines. Holding `Shift` extends a selection
at every cursor, each from where that cursor was.

Two cursors that land in the same place merge into one. They have to: two
cursors in one spot would type every character twice.

`Ctrl-V` puts the whole clipboard in at every cursor. It does not deal out one
copied line per cursor the way some editors do when the counts happen to
match.

### What it deliberately does not do

- **There is no column or block selection.** Cursors can be put in a column,
  and that covers most of what block selection is used for, but a rectangular
  *selection* is a separate thing molly does not have.
- **`Alt-U` takes cursors back in the order they were added**, not in the order
  you might prefer. There is no way to pick one cursor out of the middle of the
  set and drop only it.
- **Nothing but `Esc`, a plain click and undo collapses the set.** Switching
  files or windows keeps your cursors where they were.
- **The commands are not in the Edit menu**, because they are keyboard
  operations — the same reason Transpose is not. They are in the command
  palette (`Ctrl-Space`) and on the [help screen](#help-screen) with their
  keys.

## Searching

`Ctrl-F` opens the **Find dialog**: a field for what to look for, a checkbox for
each of the two settings that decide what it means, and Find and Cancel buttons.
Type and press `Enter`, which presses Find from wherever you are in the dialog.
Matching begins from the current cursor position, and whenever the search wraps
around the end of the file, Molly just keeps going.

`Tab` and `Shift-Tab` move between the field, the checkboxes and the buttons.
`Space` ticks the checkbox that has the keyboard, or presses the button that has
it. `Esc` closes the dialog without searching, and a click does whatever you
clicked on. The checkboxes start from the current settings and are applied when
you press Find, so cancelling changes nothing — `Search ▸ Match case` and `Alt-R` remain the
way to change a setting without searching.

Leaving the field empty and pressing Find repeats the last search, and the
dialog's hint row names the term that would be. On a terminal too short for the
whole dialog, the hint goes first and then a checkbox; the buttons keep the last
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
checkbox — or `Search ▸ Match case` — turns on case-sensitive
matching on or off at any time (not just while a search prompt is open); the
status bar reports which mode is now active. The setting applies to `Ctrl-F`,
`F3`, `Shift-F3`, and search-and-replace alike. The case-insensitive fold
covers ASCII `a`-`z` only — every other byte is compared as-is, so accented
letters match only themselves. A match always spans whole characters: a
query that would begin or end inside a UTF-8 character never matches, which
also guarantees a replace can never splice a character in half.

A search matches the line as it is *stored*, not as it is drawn: a run of
spaces never matches a tab. (To match a tab, use `\t` with `Alt-R` on — see
[Regular Expressions](#regular-expressions).)

## Regular Expressions

`Alt-R` switches the search between plain text and a regular expression, and the
status bar says which mode it is now in. The mode sticks until you switch it
back, and it applies to `Ctrl-F`, `F3`, `Shift-F3` and `Ctrl-R` alike — the
prompt says `Pattern:` rather than `Search:` so you can tell at a glance. It is
also in the Search menu, with a tick when it is on.

### What Molly understands

| | |
| --- | --- |
| `.` | any one character |
| `[abc]` `[^abc]` `[a-z]` | a character class, or anything but one |
| `\d` `\w` `\s` | a digit, a word character, a space — and `\D` `\W` `\S` for the opposite |
| `*` `+` `?` | none or more, one or more, none or one |
| `{2}` `{2,}` `{2,5}` `{,5}` | a count, a floor, a range |
| `*?` `+?` `{2,5}?` | the lazy form of any of them: as few as will do |
| `(…)` | a group, which `\1` can refer to in a replacement |
| `(?:…)` | a group that is only for grouping |
| `\|` | either side |
| `^` `$` | the start and the end of the line |
| `\b` `\B` | a word boundary, or anywhere that is not one |
| `\t` `\.` `\*` `\\` | a tab, and a backslash in front of anything with a meaning |

Not understood, and reported rather than ignored: lookaround (`(?=…)`),
backreferences inside the pattern itself, named groups, possessive quantifiers,
`\x41`-style escapes and POSIX `[:alpha:]` classes. A pattern Molly cannot read
is refused with the reason — `Bad pattern: missing ) in pattern` — and nothing
is searched or changed.

Everything above ASCII is a word character, so `\w` and `\b` work on `café` and
on `naïve`. Case folding is still ASCII-only, exactly as it is for a plain
search, so `[a-z]` finds `A` but `é` never matches `É`.

### What a search matches against

Both a plain search and a pattern match the line as it is *stored*, not as it is
drawn — so a run of spaces never matches a tab, and `.` is one character
whatever width it occupies on screen. A tab is `\t`, which only a pattern can
say: a prompt has no way to type a Tab keystroke.

`.` never matches a line ending, and `^` and `$` are the ends of a line rather
than of the file, because Molly matches one line at a time. A pattern cannot
match across a line break.

### Patterns that cannot be answered quickly

Some patterns take exponential time to decide — `(a+)+$` against a long run of
`a`s is the classic — and an editor that went away to think about one would be
worse than useless. Molly gives each line a budget, and when a pattern runs
through it says **`Pattern too complex. Try a simpler one`** and leaves
everything where it was. A replace-all that runs out partway says how many
replacements it had already made; one `Ctrl-Z` undoes all of them.

This is a budget rather than a refusal, so the same pattern may well be fine on
a shorter line. Making the pattern more specific — a literal character it must
contain, a narrower class — is usually all it takes.

## Search and Replace

`Ctrl-R` prompts for a search term, then a replacement, then asks whether to
replace just the next match (`n`) or every match in the file (`a`).

- **Next** replaces the first match found from the cursor position forward
  and stops there.
- **All** replaces every match in the file, from the start, and reports how
  many occurrences it changed.

A replace-all undoes and redoes as a single step, no matter how many
occurrences it touched — one `Ctrl-Z` puts the whole file back exactly as it
was. Replacement respects the current case-sensitivity setting (**Match case**) and
the current pattern setting (`Alt-R`).

### Groups in the replacement

With `Alt-R` on, `\1` to `\9` in the replacement stand for what the
correspondingly numbered group matched, and `\0` for the whole match. Searching
for `(\w+)=(\w+)` and replacing with `\2=\1` turns `key=value` into
`value=key`. A group that did not take part — the other side of a `|`, say —
substitutes as nothing. `\\` is a backslash, `\t` is a tab, and a backslash in
front of anything else is that character.

A replacement cannot contain a newline, because a line is Molly's unit and
splicing one in would make two; `\n` is refused with
`Bad replacement: a replacement cannot contain a newline`.

A pattern can match *nothing at all* — `x*` matches between every pair of
characters — and a replace-all then inserts at every one of those places and
steps over a character each time, exactly as `sed` does: replacing `x*` with `-`
in `abc` gives `-a-b-c-`. Nothing loops, and nothing is replaced twice.

## Undo and Redo

| Key | Action |
| --- | --- |
| `Ctrl-Z` | Undo the last change |
| `Ctrl-Y` | Redo an undone change |

Consecutive typing of characters (and consecutive partial deletions) are
grouped into a single change, so undoing a whole typed word is one press.
Undoing a change puts it back onto the redo stack, so `Ctrl-Y` replays it.

The history is bounded: once a stack holds more than 32,768 entries or
16 MB of stored text, Molly forgets its oldest entries. A long session can
therefore only limit how far back `Ctrl-Z` reaches — it can never exhaust
memory on history alone. (The step limit can be lowered through the
`MOLLY_UNDO_STEPS` environment variable, which is how the test suite
exercises the eviction.)

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
undo it. Any edit made after an undo clears the redo history — a fresh
`Ctrl-Y` will not resurrect changes replaced by newer ones.

## Opening Another File

`Ctrl-O` opens a file browser. It starts in Molly's working directory — the
directory you started it from — so `cd` wherever you like and browse from
there.

    Ctrl-O                 Open the file browser
    Up/Down, Ctrl-P/Ctrl-N Move one entry
    PgUp/PgDn, Home/End    Jump around the list
    Enter                  Enter a directory, or open the selected file
    Esc                    Cancel and return to the buffer

Directories are listed before files, each group alphabetically, with a
trailing `/` on directory names. Every list offers `..` at the top as the way
back up; other dot-files are hidden.

A file opens in a buffer of its own, alongside anything already open — see
[Several Files at Once](#several-files-at-once). Nothing is discarded, so the
browser no longer refuses to run while there are unsaved changes. A file that is
already open is switched to rather than opened a second time: two buffers on one
file would each have their own undo history and their own idea of what is on
disk, and the second save would silently undo the first.

If the buffer you are in is empty and unnamed -- a fresh `molly` with nothing
typed into it — the file opens into *that* buffer rather than beside it, so you
get one tab and not two.

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
[configuration](#configuration) file decline it entirely.

When there are more tabs than fit, molly shows a window of them around the
current buffer with a `‹` or `›` at the edge to say there are more that way
— scrolled rather than squeezed, because tabs cut to three characters are
harder to read than tabs you have to scroll past. Clicking an arrow steps one
buffer in that direction.

The status bar also carries a counter on the left:

    [2/3] two.c - 140 lines (modified)

With one buffer there is no counter, because `[1/1]` is noise.

Switching away and back returns you to where the cursor was, not to the top of
the file.

### Closing

`Ctrl-W` closes the current buffer. If it has unsaved changes, molly says so and
does nothing; press it again and it closes anyway. The warning is about *that*
buffer — switching to another unsaved one and pressing `Ctrl-W` once will warn
about that one too, rather than closing it because you already dismissed a
warning elsewhere.

Closing the only buffer empties it instead of leaving molly with nothing to
draw, which is what every editor with tabs does.

### Quitting

`Ctrl-Q` looks at **every** buffer, not the one on screen. If any has unsaved
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
file does — `sort` on a file being the commonest thing anyone wants from this,
and having to press `Ctrl-A` first would be a tax on the common case.

The command goes to `/bin/sh`, so a pipeline, a redirect and a glob all work:

    Filter through: sort -u | tr a-z A-Z
    Filter through: jq .
    Filter through: fmt -w 72
    Filter through: python3 -m json.tool
    Filter through: black -q -

Press `Enter` on an empty prompt to run the last command again — the same idiom
`Ctrl-F` has for repeating a search. The whole thing is one undo step, however
many lines it changed.

### When the command fails, your text stays

A command that exits non-zero does **not** replace anything. The status bar says
what went wrong, in the command's own words where it left any:

    jq .: jq: error (at <stdin>:3): Objects must consist of key:value pairs

Undo would get the text back, but not losing it is better than getting it back.
The cost is that a command whose non-zero exit is meaningful — `grep`, which
exits 1 when it matches nothing — cannot be used to empty a selection. `Delete`
is for that.

Molly adds a line terminator back only if your text had one: filtering the end
of a file through `sort` does not grow a blank line each time.

### Three ways out of a command that misbehaves

Running something on your behalf is the one thing in molly that can hang it, so
it cannot:

- **`Esc` cancels.** While a command runs the message bar says so, and `Esc`
  stops it. The process group is sent `SIGTERM`, then `SIGKILL` if it does not
  take the hint, so a pipeline goes with its shell.
- **A timeout.** `[shell] timeout`, in seconds, default 10. A command still
  running after that is stopped the same way.
- **An output cap.** `[shell] output limit`, in kilobytes, default 8192. The cap
  is checked as the output arrives, so a command that floods is stopped rather
  than being held in memory in order to be rejected.

In all three cases the text is left exactly as it was. A command's standard
error never reaches the screen directly — it is captured, and its first line is
what the message bar shows.

The command is your own, typed deliberately, and runs with exactly the
privileges your shell has. Molly does not run anything you have not typed.

### Inserting a command's output

`Alt-!` (or `Edit ▸ Insert output…`) runs a command with nothing on its standard
input and puts what it prints at the cursor:

    Insert output of: date
    Insert output of: git rev-parse HEAD
    Insert output of: uuidgen
    Insert output of: ls *.c

One trailing newline comes off what the command printed, so a one-line answer
goes into the middle of the line you are on rather than splitting it — a hash
lands in a commit message, a date in a header. Output of several lines leaves
the cursor at the end of the last one, where pasting the same text would.

A selection is replaced, as typing or pasting over it would replace it; to send
the selection to the command instead, that is `Alt-|`. With
[several cursors](#several-cursors) the command runs once and its output goes
in at every one of them.

Everything above about the filter holds here too: `Enter` on an empty prompt
runs the last command again (this command remembers its own, not the
filter's), it is one undo step, `Esc`, the timeout and the output cap stop it,
and a command that exits non-zero inserts nothing — `git rev-parse HEAD`
outside a repository puts `fatal: not a git repository` on the status bar, not
in your file.

## Finding in Files

`Ctrl-E` (or `Search ▸ Find in files…`) searches every file under the directory
molly was started in, and lists what it found in a window of its own:

    Find in files: parse_config

    src/config.c:212:int parse_config(const char *path) {
    src/main.c:48:  if (parse_config(rc) != 0)
    tests/test_units.c:3102:  check("empty file", parse_config(empty), 0);

Each row is a file, a line number and the line. **`Enter` on a row opens that
file at that line**, with the cursor on the match, in the window you searched
from — the list stays where it is, so you can go back to it and pick another.
`F4` goes to the next match and `Shift-F4` to the previous one, from any window,
without going back to the list at all; the list's cursor follows along so you
can see where you are.

The pattern is read the way [Find](#searching) reads one: plain text, ignoring
case, unless you have turned on **Match case** or
[pattern search](#regular-expressions) (`Alt-R`). Pattern search is handed to
`grep -E`, which agrees with molly's own patterns on everything common but is
not the same engine, so a pattern that leans on something only one of them has
can list a file the cursor then cannot find a match in; it lands at the start of
the line instead.

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
want to keep it. A new search replaces it rather than opening another. It is
not unsaved work: quitting or closing it does not ask — unless you edit it,
when it is a modified buffer like any other. Lines longer than 400 characters
are cut short in the list, which only matters for minified files.



A **buffer** is a file; a **window** is a view onto one. Until you split, there
is one window and the distinction does not come up. Split, and you have two
places to look — at two files, or at two parts of the same one.

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

Every window has its own status bar on its last row — the file's name, its
length, whether it is modified, and that window's own `Ln`/`Col`. The window
with the keyboard has the bright bar; the others are dimmed. With one window the
bar is where it always was, at the bottom of the screen above the message line.

Side-by-side windows are divided by a `│`. Stacked windows need no divider: the
upper window's status bar is already a line across the screen.

Click anywhere in a window — including on its status bar — to move the keyboard
there. Clicking in the text moves that window's cursor as well; clicking the
status bar only moves the keyboard.

### Two windows on one file

Splitting without switching files gives you two views of one buffer. It is one
file: type in either window and the text changes in both, because the text
belongs to the buffer and only the cursor and the scroll belong to the window.
Undo is the buffer's too, so `Ctrl-Z` in either window undoes the same edit.

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
rather than closing. Closing the last window is refused — molly always has one,
the same way it always has one buffer.

Closing a window gives its space back to the window it was split from, so
closing the second of two restores exactly the screen you had before the split.

### Making one window bigger

A split starts out even. To change that, move the border between the windows —
with the keyboard, or by dragging it.

`Ctrl-Shift` and an arrow moves the border beside the window you are in by one
column, or the border above or below it by one row. **The arrow says which way
the border goes**, not whether your window grows: `Ctrl-Shift-←` moves the line
between two side-by-side windows to the left whichever of them you are in, so
it makes the left one narrower and the right one wider either way. If a window
has borders on both sides, the one that moves is the one it was split along
last.

With the mouse, drag the `│` between side-by-side windows, or the status bar of
the upper of two stacked windows — that bar is the border between them. The
bottom window's status bar is above nothing and does not move. A click that
does not drag only moves the keyboard, as it always did.

A border stops where a window would go below its minimum, including windows
split inside the half it is moving into; the keyboard says so on the message
line. The border keeps its *proportion* rather than its column, so resizing the
terminal afterwards scales your layout rather than resetting it.

`Window ▸ Move border left` (and right, up and down) do the same from the menu.
On the help screen the four chords are two rows, `Ctrl-Shift-←→` and
`Ctrl-Shift-↑↓`, and those two rows show the default chords even if you rebind
them.

### When there is no room

A window needs at least one row of text plus its status bar, and eight columns.
A split that would leave less than that is refused with a message saying which
way it would not fit, rather than made and left for you to undo. Shrinking the
terminal under a split you already have shares out what is left in the same
proportions, and a window squeezed below the minimum is left blank until there
is room for it again — the window you are typing in stays legible, which is
what matters while you are dragging a corner.

## Saving and Quitting

| Key | Action |
| --- | --- |
| `Ctrl-S` | Save the file |
| `Ctrl-Q` | Quit |

If the buffer has no file name yet, `Ctrl-S` first asks where to save. An
empty answer cancels the save rather than being taken as a file name, so the
buffer keeps its unnamed state and `Ctrl-S` will ask again next time.

### How a save is written

Molly does not write over your file directly. It creates a uniquely named
temporary file beside the target, writes the buffer there, flushes it to disk,
and only then renames it into place. A save that is interrupted — by a crash,
a full disk, or the power going out — therefore leaves the original file
untouched rather than half-written. The temporary name is unpredictable and
created exclusively, so it can never collide with a file of your own and
cannot be redirected by anything else in the directory.

Two consequences are worth knowing:

- Saving replaces the file, so any **hard links** to it are broken: the other
  names keep the old contents.
- Renaming requires a writable **directory**. If the file is writable but its
  directory is not, Molly falls back to rewriting the file in place and says
  `written (in place, not atomic)` — your work is saved, but that particular
  write has no crash protection.

When the file name is a symbolic link, Molly follows it and replaces the file
it points at, leaving the link itself alone.

If the file has changed on disk since Molly opened it (or since the last
save) — another process wrote to it in the meantime — `Ctrl-S` asks before
overwriting that change, rather than silently discarding it. Answering `n`
aborts the save; `y` proceeds and overwrites it as normal.

Molly protects you from losing work: when there are unsaved changes, the first
`Ctrl-Q` lists the unsaved buffers and offers Save All, Discard or Cancel. A
clean session quits on the first `Ctrl-Q`.

If Molly is stopped from outside instead — its terminal window is closed, or it
receives `SIGTERM`, `SIGHUP`, `SIGINT` or `SIGQUIT` — it puts your terminal's
settings back on the way out, so the shell you return to echoes and edits
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
`Ctrl-Q` quit, so decide at the prompt — after that the data is gone. (This
prompt only covers named files; for an unnamed buffer, inspect the
`molly-recover.<pid>` file yourself and rename it over whatever it belongs to.)

After saving, the undo history is kept, but the dirty flag is cleared.

## Keyboard Shortcuts

    F10      Open the menu bar
    Alt-F/E/S/V/G/W/H  Open the File/Edit/Search/View/Go/Window/Help menu
    Ctrl-Space  Command palette (Alt-X does the same)
    Ctrl-S   Save file
    Ctrl-O   Open a file (directory browser)
    Ctrl-N   New buffer
    Ctrl-W   Close this buffer
    Ctrl-PgDn / Ctrl-PgUp  Next / previous buffer
    F7       Split, new window to the right
    F8       Split, new window below
    F6 / Shift-F6  Next / previous window
    F9       Close this window
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
    Ctrl-A   Select everything
    Ctrl-L   Select the current line
    Ctrl-C   Copy the selection
    Ctrl-X   Cut the selection
    Ctrl-V   Paste the clipboard at the cursor
    Ctrl-T   Swap the two characters at the cursor
    Ctrl-D   Add a cursor at the next occurrence of the word or selection
    Alt-D    Skip this match, take the next one
    Alt-U    Take the last cursor back
    Alt-A    A cursor on every match at once
    Ctrl-Down / Ctrl-Up  Add a cursor on the row below / above
    Tab      Indent the selected rows (or insert one indent step)
    Shift-Tab  Dedent the selected rows, or the cursor's row
    Alt-|    Filter the selection (or the file) through a command
    Alt-!    Insert a command's output at the cursor
    Ctrl-Z   Undo the last change
    Ctrl-Y   Redo an undone change
    F1       Show this help screen (Ctrl-? does the same)
    F5       Repaint the screen
    Ctrl-U   Toggle the line-number gutter
    Alt-M    Toggle the menu bar
    Alt-B    Toggle the tab bar
    Alt-Y    Toggle syntax highlighting
    Alt-Z    Toggle soft wrap
    Tab      In a dialog: move to the next field, checkbox or button
    Shift-Tab  In a dialog: move back to the previous one
    Space    In a dialog: tick the checkbox or press the button with the keyboard
    Shift    Hold with arrows / Home / End / PgUp / PgDn to select
    Shift    Hold while dragging to select with the terminal, not molly
    Esc      Cancel the current prompt, menu or dialog; or go back to one cursor

To see the live keymap rather than this list:

    MOLLY_DUMP_KEYMAP=1 molly

## Configuration

Molly reads one file at startup:

```text
~/.config/molly/mollyrc
```

If `XDG_CONFIG_HOME` is set, molly looks under that instead of `~/.config`.
There is no file to begin with, and molly runs the same without one — every
setting below lists its default, and the defaults are what molly did before
there was a configuration file at all.

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
| `editor` | `undo steps` | `32768` | How many undo steps to keep. |
| `view` | `line numbers` | `off` | Show the line-number gutter (`Ctrl-U` toggles it). |
| `view` | `menu bar` | `on` | Give the menu bar its row (`View ▸ Menu Bar` toggles it). |
| `view` | `tab bar` | `on` | Show a tab per open file (`Alt-B` toggles it). Only ever drawn with two or more buffers open. |
| `view` | `syntax` | `on` | Colour the text by filetype (`Alt-Y` toggles it). See [Syntax Highlighting](#syntax-highlighting). |
| `view` | `italic` | `on` | Draw comments in italic, on terminals that can (not the Linux console, not under a `screen` TERM). See [Syntax Highlighting](#syntax-highlighting). |
| `view` | `soft wrap` | `off` | Wrap long lines at the window edge instead of scrolling sideways (`Alt-Z` toggles it). See [Soft Wrap](#soft-wrap). |
| `shell` | `timeout` | `10` | Seconds a filter command may run before molly stops it, 1 to 3600. See [Filtering Through a Command](#filtering-through-a-command). |
| `shell` | `output limit` | `8192` | Kilobytes of output a filter command may produce, 1 to 1048576. |
| `input` | `mouse` | `on` | Report the mouse to molly. Turning it off hands clicks and drag-select back to the terminal for good, rather than only while `Shift` is held — see [The Mouse](#the-mouse). |
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

Command names are what the command palette lists — `Ctrl-Space`, then read the
names in the right-hand column — and `MOLLY_DUMP_KEYMAP` prints every binding
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

Two limits are worth knowing, because both come from the terminal rather than
from molly:

- **`Shift` can only be named on the function keys and the navigation keys**,
  and `Ctrl` on those keys means something different from `Ctrl` on a letter.
  A terminal reports a modifier as a parameter inside those sequences, so
  `Ctrl-PgUp` and `Shift-Right` arrive as themselves. On a letter there is no
  parameter: `Shift-A` *is* `A`, and `Ctrl-Shift-L` is the same single byte as
  `Ctrl-L`. Molly refuses the chords that could never fire rather than
  accepting a binding that silently never happens. `Ctrl-Alt-` is refused for
  the same reason.
- **`Shift` with an arrow still extends the selection.** Nothing is bound to
  `Shift-Right`, so it falls through to `move.right` with the modifier noted,
  which is what extends a selection. Bind `Shift-Right` to something and you
  lose that, for that key.

The `Alt`-letter menu titles cannot be rebound — seven of them today, one per
menu, and the set is read off the menu table rather than written down. The menu
bar underlines the letter whether or not it still opens the menu, so a rebinding
would make the bar lie about itself.

Multi-key sequences (`Ctrl-K Ctrl-F`) are not supported yet.

### When molly disagrees with the file

A mistake in `mollyrc` is a warning, never a refusal to start. Molly reads the
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
its section, its current value, and — for the one selected — what it does:

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
above to `mollyrc`, creating `~/.config/molly/` if it is not there. So the way
to keep a setting is to change it the usual way — `Ctrl-U` for the gutter,
`View ▸ Menu Bar` for the bar, the Find dialog's checkboxes — and then save.
Key bindings you put in the file by hand are written back too; molly's own
defaults are not, so a later version is free to move a key you never touched.

The write is atomic: molly writes a temporary file beside the target and renames
it, so an interrupted save cannot leave a half-written file that fails to parse
the next time molly starts.

One cost, stated plainly: **a save rewrites the file whole.** Your comments, your
blank lines and any setting this version of molly does not recognize are gone
afterwards. Molly puts its own comments back, explaining each setting and its
default.

## Environment Variables

| Variable | Effect |
| --- | --- |
| `TERM`, `COLORTERM` | Choose the 256-color or the 16-color palette — see [Colors](#colors) |
| `MOLLY_DUMP_KEYMAP` | Print the keymap as `chord<TAB>command` lines and exit, without touching the terminal. Every binding in it was parsed from chord text at startup and printed back, so it is both a reference for what is bound and how the test suite checks the chord code. `[keys]` from your `mollyrc` is applied first, so this is also how to check a rebinding. |
| `MOLLY_DUMP_CONFIG` | Print the parsed configuration — the file's path, every setting's value, the key bindings it asked for and any warnings — and exit, without touching the terminal. See [Configuration](#configuration). |
| `MOLLY_CONFIG` | Read configuration from this path instead of `~/.config/molly/mollyrc`. Set to the empty string to read none at all, which is what the test suite does so that its results do not depend on the configuration of the machine it runs on. |
| `MOLLY_UNDO_STEPS` | Lower the undo history's step limit, which is how the test suite exercises its eviction. |

## File Format Notes

- Line endings are preserved. A file loaded with Windows `CRLF` endings is
  saved back with `CRLF`; a Unix `LF` file stays `LF`. A brand-new file uses
  `LF`.
- A trailing newline is preserved too: if the file you opened ended without
  one, the save leaves it without one. A brand-new file is given a trailing
  newline.
- Text is stored and saved as raw bytes, so UTF-8 (and any other encoding)
  round-trips unchanged. The cursor, `Backspace` and `Delete` operate on whole
  UTF-8 characters rather than individual bytes, and the `Col` indicator counts
  characters. Display width is honored: East Asian wide characters take two
  columns, combining marks take none, so the cursor and horizontal scrolling
  stay aligned with what the terminal actually shows.
- The status bar shows the file name capped at 20 characters.
