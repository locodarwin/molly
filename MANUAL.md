# Molly — A Command-Line Text Editor

> **Note:** molly is at `0.1.0-dev`. Its **interface** is molly's own — a menu
> bar, a mouse, framed dialogs and a command palette — while its **editing**
> features are still the ones it inherited from
> [olly](https://github.com/locodarwin/olly): one file at a time, plain-text
> search, no syntax highlighting and no configuration file yet. This manual
> describes what is there today and is accurate. For what is coming, and why in
> that order, see [`ROADMAP.md`](ROADMAP.md).

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
- [Prompts](#prompts)
- [Editing](#editing)
- [Moving Around](#moving-around)
- [Help Screen](#help-screen)
- [Line Numbers](#line-numbers)
- [Selecting Text](#selecting-text)
- [Searching](#searching)
- [Regular Expressions](#regular-expressions)
- [Search and Replace](#search-and-replace)
- [Undo and Redo](#undo-and-redo)
- [Opening Another File](#opening-another-file)
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

**The keymap is not configurable yet.** Every binding is already a line of text
parsed at startup — which is the groundwork for a configuration file — but the
file itself does not exist, so for now the bindings are what they are. `Ctrl-L`
still repaints the screen and `Ctrl-T` still toggles case-sensitive search; the
roadmap moves both to the CUA meanings when configuration arrives.

## The Menu Bar

The band across the top of the screen is molly's main answer to "what can this
thing do?". Six menus: **File**, **Edit**, **Search**, **View**, **Go** and
**Help**. (*Tools* and *Options* come with Phase 2; their `Alt` letters are
reserved already, so adding them will not move anything.)

| Key | Action |
| --- | --- |
| `F10` | Open the menu bar |
| `Alt-F` `Alt-E` `Alt-S` `Alt-V` `Alt-G` `Alt-H` | Open that menu directly |
| `←` / `→` | Move to the previous / next menu |
| `↑` / `↓` | Move within the open menu |
| `Home` / `End` | First / last item |
| An underlined letter | Run that item |
| `Enter` | Run the highlighted item |
| `Esc` | Close without running anything |

Eight letters are reserved on `Alt` — `F E S V G T O H`, the last two held for the
menus that do not exist yet. **Every other `Alt`+letter is free for commands**,
which is the point of reserving no more than the titles.

Every entry is built from molly's command table, so a menu cannot drift out of
step with the editor: the label, the shortcut shown down the right-hand side,
whether the item is available and whether it is ticked all come from the command
itself. **An item molly cannot run right now is greyed out, skipped by the
arrows, and deaf to its own letter** — `Paste` with an empty clipboard, `Find
Next` before you have searched for anything. Items that toggle something show a
tick when it is on.

Running a command from a menu is the same as running it from its key, down to
the details: choosing **Quit** from the File menu with unsaved changes starts the
same countdown `Ctrl-Q` does.

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
| `Tab` | Insert a tab character |
| `Enter` | Split the line / insert a new line |
| `Backspace` | Delete the character before the cursor |
| `Delete` | Delete the character at the cursor |

If the cursor is on the empty row below the last line of text, typing creates
that row for you automatically. If the cursor is at the very start of a line,
`Backspace` joins the current line onto the previous one; `Delete` at the very
end of a line joins the next line onto it.

## Moving Around

| Key | Action |
| --- | --- |
| `←` `↑` `↓` `→` | Move the cursor |
| `Home` | Go to the start of the line |
| `End` | Go to the end of the line |
| `PgUp` / `PgDn` | Move up / down a screenful |
| `Ctrl-G` | Go to a line number |
| `Ctrl-L` | Repaint the screen |
| `Ctrl-U` | Toggle the line-number gutter |

Hold `Shift` while using the arrows, `Home`/`End`, or `PgUp`/`PgDn` to move the
cursor and select text along the way — see [Selecting Text](#selecting-text).

`Ctrl-G` prompts for a line number and moves the cursor to the start of that
line. A number past the last line goes to the last line instead; a number
below 1 goes to the first line. Anything that is not a valid number leaves
the cursor where it was.

The terminal can also be resized freely while Molly is running — the display
adjusts to the new size as it happens, without needing a keypress first.

## Help Screen

`Ctrl-?` opens a keyboard reference, in a framed dialog over the editor. It lays
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
you press Find, so cancelling changes nothing — `Ctrl-T` and `Alt-R` remain the
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
| `Ctrl-N` | Find the next instance of the search term |
| `Ctrl-P` | Find the previous instance of the search term |
| `Ctrl-T` | Toggle case-sensitive search |
| `Alt-R` | Toggle regular-expression search |
| `Esc` | Cancel the search prompt |

The same term is remembered for the whole session, so you can keep tapping
`Ctrl-N` / `Ctrl-P` to step through every occurrence. If there is no previous
search, Molly tells you and asks you to press `Ctrl-F` again.

Matching is case-insensitive by default. `Ctrl-T` toggles case-sensitive
matching on or off at any time (not just while a search prompt is open); the
status bar reports which mode is now active. The setting applies to `Ctrl-F`,
`Ctrl-N`, `Ctrl-P`, and search-and-replace alike. The case-insensitive fold
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
back, and it applies to `Ctrl-F`, `Ctrl-N`, `Ctrl-P` and `Ctrl-R` alike — the
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
was. Replacement respects the current case-sensitivity setting (`Ctrl-T`) and
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

The browser replaces the buffer, so it refuses to run while there are unsaved
changes — save first, and the status bar tells you so. Molly still edits one
file at a time: opening another file discards the previous buffer along with
its undo history, and touches nothing on disk.

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
few presses of `Ctrl-Q` only confirm that you really want to quit, and a
warning shows how many more presses are needed. A clean buffer quits on the
first `Ctrl-Q`.

If Molly is stopped from outside instead — its terminal window is closed, or it
receives `SIGTERM`, `SIGHUP`, `SIGINT` or `SIGQUIT` — it puts your terminal's
settings back on the way out, so the shell you return to echoes and edits
normally.

### Recovery of unsaved work

If Molly is killed by one of those signals, or exits because it ran out of
memory, and the buffer had unsaved changes, it writes what was in the buffer
to a recovery file before it goes. The recovery file sits next to the file you
were editing, named after it with a `.molly-recover` suffix (for an unnamed
buffer it is `molly-recover.<pid>` in the current directory).

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
    Alt-F/E/S/V/G/H  Open the File/Edit/Search/View/Go/Help menu
    Ctrl-Space  Command palette (Alt-X does the same)
    Ctrl-S   Save file
    Ctrl-O   Open a file (directory browser)
    Ctrl-Q   Quit (asks several times if there are unsaved changes)
    Ctrl-F   Open the Find dialog (case-insensitive by default)
    Ctrl-N   Find next instance of the search
    Ctrl-P   Find previous instance of the search
    Ctrl-T   Toggle case-sensitive search
    Alt-R    Toggle regular-expression search
    Ctrl-R   Search and replace (next or all)
    Ctrl-G   Go to line
    Ctrl-C   Copy the selection
    Ctrl-X   Cut the selection
    Ctrl-V   Paste the clipboard at the cursor
    Ctrl-Z   Undo the last change
    Ctrl-Y   Redo an undone change
    Ctrl-?   Show this help screen
    Ctrl-L   Repaint the screen
    Ctrl-U   Toggle the line-number gutter
    Tab      In a dialog: move to the next field, checkbox or button
    Shift-Tab  In a dialog: move back to the previous one
    Space    In a dialog: tick the checkbox or press the button with the keyboard
    Shift    Hold with arrows / Home / End / PgUp / PgDn to select
    Shift    Hold while dragging to select with the terminal, not molly
    Esc      Cancel the current prompt, menu or dialog

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
| `editor` | `undo steps` | `32768` | How many undo steps to keep. |
| `view` | `line numbers` | `off` | Show the line-number gutter (`Ctrl-U` toggles it). |
| `view` | `menu bar` | `on` | Give the menu bar its row (`View ▸ Menu Bar` toggles it). |
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
| A bare character | `a`, `}` |

Two limits are worth knowing, because both come from the terminal rather than
from molly:

- **`Shift` can only be named on the function keys and the navigation keys.**
  A terminal sends plain `A` for `Shift-A`, and sends the same single byte for
  `Ctrl-Shift-L` as it does for `Ctrl-L`, so there is nothing there to bind.
  Molly refuses those chords rather than accepting a binding that could never
  fire. `Ctrl-Alt-` is refused for the same reason.
- **`Shift` with an arrow still extends the selection.** Nothing is bound to
  `Shift-Right`, so it falls through to `move.right` with the modifier noted,
  which is what extends a selection. Bind `Shift-Right` to something and you
  lose that, for that key.

The eight `Alt`-letter menu titles cannot be rebound. The menu bar underlines
the letter whether or not it still opens the menu, so a rebinding would make
the bar lie about itself.

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

### Saving settings

**File ▸ Save Settings** writes the current state of everything in the table
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
