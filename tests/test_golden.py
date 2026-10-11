#!/usr/bin/env python3
"""Golden-screen tests: drive molly, render what it drew, diff against a snapshot.

These exist to make the Phase 0 rewrites provable. Splitting molly into modules,
separating buffer from view state and replacing the per-line render cache with a
cell grid are all supposed to change *nothing* on screen, and the existing tests
mostly check file contents and individual escape sequences -- they would not
notice a row left stale or a selection highlight lost at one edge.

Run with MOLLY_UPDATE_GOLDEN=1 to write the snapshots instead of comparing. Do
that only when a change to the output is intended, and read the resulting diff.

Usage: test_golden.py ./molly
"""

import difflib
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pty_harness
from screen import Screen

GOLDEN_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "golden")
UPDATE = os.environ.get("MOLLY_UPDATE_GOLDEN") == "1"

MOLLY = None
VERSION = None

passed = 0
failed = []


def normalize(text):
    """Remove the things that legitimately vary between runs and machines."""
    if VERSION:
        text = text.replace(VERSION, "<VERSION>")
    return text


def check(name, keys, rows=24, cols=80, files=None, args=None, settle=0.5,
          final=None, env=None):
    """Run molly against a fresh copy of `files`, snapshot the screen, compare.

    `final` is the (rows, cols) the terminal ends at, for a case that resizes
    mid-session: molly repaints in full after a resize, so replaying the whole
    stream into a grid of the final size still lands on the right screen.
    """
    global passed

    # A fixed filename, because it is drawn in the status bar: a temp path would
    # put a different string in the snapshot on every run.
    for fname, content in (files or {}).items():
        with open(fname, "w") as f:
            f.write(content)

    argv = [MOLLY] + (args if args is not None else ["sample.txt"])
    out = pty_harness.run(argv, keys, rows=rows, cols=cols, settle=settle, env=env)

    scr = Screen(*(final if final else (rows, cols)))
    scr.feed(out)
    got = normalize(scr.snapshot())

    if scr.unhandled:
        failed.append((name, "unhandled escape sequences: %r" % (scr.unhandled[:5],)))
        return

    path = os.path.join(GOLDEN_DIR, name + ".txt")
    if UPDATE:
        with open(path, "w") as f:
            f.write(got)
        print("  wrote %s" % name)
        passed += 1
        return

    if not os.path.exists(path):
        failed.append((name, "no golden file; run with MOLLY_UPDATE_GOLDEN=1"))
        return

    with open(path) as f:
        want = f.read()

    if got == want:
        print("  pass  %s" % name)
        passed += 1
    else:
        diff = "".join(difflib.unified_diff(
            want.splitlines(True), got.splitlines(True),
            fromfile="golden", tofile="actual"))
        failed.append((name, diff))


THREE_LINES = "alpha\nbeta\ngamma\n"
# Two occurrences of one word on different rows, which is what Ctrl-D is for.
MATCHES = "red fish\nblue fish\nred bird\n"
WIDE = "日本語 text\n\ttabbed\ncafé combining\n"
LONG = "x" * 200 + "\n" + "short\n"


def main():
    global MOLLY, VERSION
    if len(sys.argv) < 2:
        print("usage: test_golden.py ./molly", file=sys.stderr)
        return 2
    MOLLY = os.path.abspath(sys.argv[1])
    VERSION = subprocess.run([MOLLY, "--version"], capture_output=True,
                             text=True).stdout.strip().split()[-1]

    os.makedirs(GOLDEN_DIR, exist_ok=True)
    # Run from a scratch directory with a *fixed* name, not a random one: the
    # status bar shows the file name and the open dialog shows the directory
    # path, so a random temp directory would put a different string in the
    # snapshot on every run.
    import shutil
    import tempfile
    tmp = os.path.join(tempfile.gettempdir(), "molly-golden")
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    os.chdir(tmp)

    print("\ngolden screens (molly %s)" % VERSION)

    # An empty buffer: the welcome line, the tildes, both bars.
    check("empty-buffer", [], args=[])

    # A file, the plainest case there is.
    check("three-lines", [], files={"sample.txt": THREE_LINES})

    # The line-number gutter: gray separator, right-aligned numbers, gray tildes.
    check("gutter-on", ["\x15"], files={"sample.txt": THREE_LINES})

    # A selection, which must come back as reverse-video attributes.
    check("selection", ["\x1b[1;2C", "\x1b[1;2C", "\x1b[1;2C"],
          files={"sample.txt": THREE_LINES})

    # Selection plus the gutter, where the column arithmetic is easiest to break.
    check("selection-gutter", ["\x15", "\x1b[1;2C", "\x1b[1;2C"],
          files={"sample.txt": THREE_LINES})

    # A modified buffer: the status bar grows "(modified)" and Col moves.
    check("modified", ["Z", "Z"], files={"sample.txt": THREE_LINES})

    # Wide glyphs, a tab, and a combining mark: display-width handling.
    check("wide-and-tabs", [], files={"sample.txt": WIDE})

    # Horizontal scroll: a long line with the cursor driven off the right edge.
    check("hscroll", ["\x1b[F"], files={"sample.txt": LONG})

    # The help screen, which reflows between one and two columns.
    check("help-wide", ["\x1f"], files={"sample.txt": THREE_LINES})
    check("help-narrow", ["\x1f"], cols=50, files={"sample.txt": THREE_LINES})
    # Paged to WINDOWS, whose border chords are the first keys with a glyph in
    # them: both columns are cut and padded in display columns, which this is
    # the only screen to see. Three pages down is where the group lands at 80.
    check("help-windows", ["\x1f", "\x1b[6~", "\x1b[6~", "\x1b[6~"],
          files={"sample.txt": THREE_LINES})

    # The Find dialog: a field, two checkboxes and two buttons, with the field
    # holding the focus and the cursor, and the default button in bold.
    check("find-dialog", ["\x06", "b", "e"], files={"sample.txt": THREE_LINES})
    # The focus moved on to a checkbox and ticked it, which is the only way to see
    # that a tick and the focus are two different things.
    check("find-dialog-focus", ["\x06", "be", "\t", " "],
          files={"sample.txt": THREE_LINES})
    # At 256 colours: the field sunk, the buttons raised, the focus accented.
    check("find-dialog-256", ["\x06", "be", "\t"], rows=20, cols=72,
          files={"sample.txt": THREE_LINES},
          env={"TERM": "xterm-256color"})
    # Narrow enough that the hint line goes and the pair of buttons becomes one.
    check("find-dialog-narrow", ["\x06"], rows=8, cols=24,
          files={"sample.txt": THREE_LINES})

    # A small terminal, where the row budget is tightest.
    check("tiny-terminal", [], rows=6, cols=24, files={"sample.txt": THREE_LINES})

    # The open-file dialog: a framed, shadowed layer over the editor. The
    # listing is this scratch directory, so it is exactly what the test made.
    os.makedirs("subdir", exist_ok=True)
    check("open-dialog", ["\x0f"], rows=20, cols=64,
          files={"sample.txt": THREE_LINES})
    check("open-dialog-narrow", ["\x0f"], rows=12, cols=40,
          files={"sample.txt": THREE_LINES})
    # The same dialog at 256 colours. Both depths are kept so neither rots: the
    # sixteen-colour theme is what an old terminal gets, and nothing else would
    # notice if it drifted.
    check("open-dialog-256", ["\x0f"], rows=20, cols=64,
          files={"sample.txt": THREE_LINES},
          env={"TERM": "xterm-256color"})

    # Moving the selection must move the highlight and nothing else.
    check("open-dialog-selected", ["\x0f", "\x1b[B", "\x1b[B"], rows=20, cols=64,
          files={"sample.txt": THREE_LINES})

    # The menu bar: the band at rest, a drop-down open, the states an item can be
    # in, and the two things the hide toggle does.
    check("menu-bar", [], rows=16, cols=64, files={"sample.txt": THREE_LINES})
    check("menu-file-open", ["\x1b[21~"], rows=16, cols=64,
          files={"sample.txt": THREE_LINES})
    # Find Next and Find Previous are greyed until something has been searched for,
    # and Match Case is unticked.
    check("menu-search-disabled", ["\x1bs"], rows=16, cols=64,
          files={"sample.txt": THREE_LINES})
    # After a search they are available; after Ctrl-T Match Case is ticked.
    check("menu-search-enabled", ["\x06", "beta", "\r", "\x1bs"], rows=16, cols=64,
          files={"sample.txt": THREE_LINES})
    check("menu-search-checked", ["\x14", "\x1bs"], rows=16, cols=64,
          files={"sample.txt": THREE_LINES})
    # Hidden, the band gives its row back to the text; opened while hidden, it draws
    # over the first line rather than reflowing the screen.
    # A band narrower than its seven titles: first the gaps close, then each
    # title becomes its letter, and the open one is written out over its menu.
    check("menu-band-squeezed", [], rows=8, cols=40, files={"sample.txt": THREE_LINES})
    check("menu-band-letters", [], rows=8, cols=30, files={"sample.txt": THREE_LINES})
    check("menu-band-letters-open", ["\x1bs"], rows=12, cols=30,
          files={"sample.txt": THREE_LINES})
    check("menu-band-letters-open-256", ["\x1bs"], rows=12, cols=30,
          files={"sample.txt": THREE_LINES}, env={"TERM": "xterm-256color"})
    check("menu-bar-hidden", ["\x1bv", "m"], rows=16, cols=64,
          files={"sample.txt": THREE_LINES})
    check("menu-bar-hidden-open", ["\x1bv", "m", "\x1b[21~"], rows=16, cols=64,
          files={"sample.txt": THREE_LINES})
    # Syntax highlighting, which is nothing *but* colour -- there is not a
    # character on screen that a snapshot of the text alone would catch. So
    # these are the only thing standing between a theme edit and a palette
    # nobody looks at, and all four of the interesting languages are here.
    C_SAMPLE = ("#include <stdio.h>\n"
                "/* a block\n"
                "   comment */\n"
                "int main(void) {\n"
                "  char *s = \"hi\\n\";\n"
                "  return 42;  // done\n"
                "}\n")
    check("syntax-c", [], rows=14, cols=60, files={"demo.c": C_SAMPLE},
          args=["demo.c"])
    check("syntax-c-256", [], rows=14, cols=60, files={"demo.c": C_SAMPLE},
          args=["demo.c"], env={"TERM": "xterm-256color"})
    # Python, for the triple-quoted region that spans rows.
    check("syntax-python", [], rows=14, cols=60,
          files={"demo.py": ("import os\n"
                             "\n"
                             "def f(n):\n"
                             "    \"\"\"doc\n"
                             "    string\"\"\"\n"
                             "    return n * 2  # twice\n")},
          args=["demo.py"])
    # AutoIt, for case-folded keywords and the #cs/#ce pair.
    check("syntax-autoit", [], rows=14, cols=60,
          files={"demo.au3": ("#cs\n"
                              "  If this were code\n"
                              "#ce\n"
                              "#include <x.au3>\n"
                              "IF $x THEN\n"
                              "  MsgBox(0, \"hi\", \"there\")\n"
                              "ENDIF\n")},
          args=["demo.au3"])
    # Markdown, which is the one with no keywords at all: a bold heading, inline
    # emphasis, and a fenced block whose `#` is not a heading.
    MD_SAMPLE = ("# Title\n"
                 "\n"
                 "Some *emphasis* and `code`.\n"
                 "\n"
                 "```c\n"
                 "# not a heading\n"
                 "```\n"
                 "\n"
                 "Done.\n")
    check("syntax-markdown", [], rows=14, cols=60,
          files={"demo.md": MD_SAMPLE}, args=["demo.md"])
    check("syntax-markdown-256", [], rows=14, cols=60,
          files={"demo.md": MD_SAMPLE}, args=["demo.md"],
          env={"TERM": "xterm-256color"})
    # A comment opened by an edit, which is the invalidation path: rows 2 and 3
    # turn without being touched.
    check("syntax-opened-comment", ["/*"], rows=14, cols=60,
          files={"multi.c": "int a;\nint b;\nint c;\n"}, args=["multi.c"])
    # A selection over coloured text: reverse replaces the colour rather than
    # layering over it.
    check("syntax-selection", ["\x1b[1;2C", "\x1b[1;2C", "\x1b[1;2C"],
          rows=14, cols=60, files={"demo.c": C_SAMPLE}, args=["demo.c"])
    # And off, which has to be the screen molly drew before any of this existed.
    check("syntax-off", ["\x1by"], rows=14, cols=60,
          files={"demo.c": C_SAMPLE}, args=["demo.c"])

    # Soft wrap. The pair the hscroll case above belongs to: the same long line,
    # running off the edge there and laid out over two rows here. The character
    # at the edge is not a space, so the cut and the break fall in two different
    # places and the snapshots cannot be confused for each other.
    WRAPPY = ("alpha beta gamma delta epsilon zeta etax theta iota kappa\n"
              "second\n")
    check("wrap-off", [], rows=12, cols=40, files={"sample.txt": WRAPPY})
    check("wrap-on", ["\x1bz"], rows=12, cols=40, files={"sample.txt": WRAPPY})
    # With the gutter, which is where the continuation row has to keep the gray
    # separator and leave the number blank -- and at both depths, because the
    # gutter is the one part of this that is a colour.
    check("wrap-gutter", ["\x1bz", "\x15"], rows=12, cols=40,
          files={"sample.txt": WRAPPY})
    check("wrap-gutter-256", ["\x1bz", "\x15"], rows=12, cols=40,
          files={"sample.txt": WRAPPY}, env={"TERM": "xterm-256color"})
    # Narrow, where nearly every row has to choose between a space and the edge.
    check("wrap-narrow", ["\x1bz"], rows=10, cols=20,
          files={"sample.txt": WRAPPY})
    # Wide glyphs, a tab and a combining mark, wrapped at a width that splits
    # them: a slice boundary is a column, so a glyph straddling one is dropped
    # and its columns left blank.
    check("wrap-wide", ["\x1bz"], rows=10, cols=12, files={"sample.txt": WIDE})
    # The cursor at the end of a wrapped line, which is on the continuation row
    # rather than off the right-hand edge of the first one.
    check("wrap-cursor-end", ["\x1bz", "\x1b[F"], rows=12, cols=40,
          files={"sample.txt": WRAPPY})
    # A selection across a wrap: all of the first row and one cell of the
    # second, which is what "the break column belongs to the row below" looks
    # like on screen.
    check("wrap-selection", ["\x1bz", "\x1b[1;2B", "\x1b[1;2C"], rows=12,
          cols=40, files={"sample.txt": WRAPPY})
    # Prose with highlighting on, which is what the feature is for -- and where
    # a slice has to carry the roles of the columns it shows and not of the
    # columns it starts at. The emphasis and the code span are deliberately on
    # different rows of the same line, and the fence opens a block that a wrap
    # must not end.
    WRAP_MD = ("# A heading long enough to have to wrap somewhere\n"
               "\n"
               "Some *emphasis* early in a paragraph that is far too long for "
               "this window, and `code` near the end of it.\n"
               "\n"
               "```c\n"
               "int wrapped = 1;  /* a long comment that has to wrap too */\n"
               "```\n")
    check("wrap-markdown", ["\x1bz"], rows=14, cols=40,
          files={"demo.md": WRAP_MD}, args=["demo.md"])
    # One window wrapped and the other not, which is what "wrap is the window's
    # setting" means on screen. F7 splits right, then Alt-Z in the new window.
    check("wrap-split", ["\x1b[18~", "\x1bz"], rows=14, cols=60,
          files={"sample.txt": WRAPPY})

    # The tab bar. Which tab is current is a colour rather than a character, so
    # this is the snapshot that proves it at all -- and both depths are here
    # because the band and the lifted current tab are two shades of the same
    # family, which is exactly the relationship a 16-colour fallback can lose.
    TABFILES = {"sample.txt": THREE_LINES, "second.txt": "two\n",
                "third.txt": "three\n"}
    TABARGS = ["sample.txt", "second.txt", "third.txt"]
    check("tabbar", [], rows=16, cols=64, files=TABFILES, args=TABARGS)
    check("tabbar-256", [], rows=16, cols=64, files=TABFILES, args=TABARGS,
          env={"TERM": "xterm-256color"})
    # On another tab, so the highlight is proved to move rather than to be
    # painted on the first one.
    check("tabbar-switched", ["\x1b[6;5~"], rows=16, cols=64, files=TABFILES,
          args=TABARGS)
    # The unsaved marker, which is the one thing on the row that is a character.
    check("tabbar-modified", ["X"], rows=16, cols=64, files=TABFILES,
          args=TABARGS)
    # Narrow: more tabs than fit, scrolled with an arrow rather than squeezed.
    # Two of them, because the window and the arrow differ at each end.
    check("tabbar-narrow", [], rows=12, cols=26, files=TABFILES, args=TABARGS)
    check("tabbar-narrow-end", ["\x1b[5;5~"], rows=12, cols=26,
          files=TABFILES, args=TABARGS)
    # An open menu has to cover the tabs, not the other way round -- the tab bar
    # is pushed below the menu bar in the layer stack for exactly this.
    check("tabbar-under-menu", ["\x1bf"], rows=16, cols=64, files=TABFILES,
          args=TABARGS)
    # And with the menu bar hidden, the tabs take row zero.
    check("tabbar-no-menubar", ["\x1bm"], rows=16, cols=64, files=TABFILES,
          args=TABARGS)

    # Several buffers open. Nothing captured the status bar's "[2/3]" counter,
    # and until the tab bar exists that counter is the only thing on screen
    # saying there is more than one file open at all -- so it is worth a snapshot
    # of its own rather than relying on a pty check of one substring.
    check("buffers-status", [], rows=16, cols=64,
          files={"sample.txt": THREE_LINES, "second.txt": "two\n",
                 "third.txt": "three\n"},
          args=["sample.txt", "second.txt", "third.txt"])
    # And after switching, since the counter and the file name both move.
    check("buffers-switched", ["\x1b[6;5~"], rows=16, cols=64,
          files={"sample.txt": THREE_LINES, "second.txt": "two\n",
                 "third.txt": "three\n"},
          args=["sample.txt", "second.txt", "third.txt"])
    # With one file the counter is left off, which is the common case and the one
    # a regression would be least likely to be noticed in.
    check("buffers-single", [], rows=16, cols=64,
          files={"sample.txt": THREE_LINES})

    # The Go menu, which the two buffer commands joined and which nothing had
    # captured. Both of them grey out with one buffer open, so this snapshot is
    # the dimmed state and the one below is the live one.
    check("menu-go", ["\x1bg"], rows=16, cols=64,
          files={"sample.txt": THREE_LINES})
    check("menu-go-buffers", ["\x1bg"], rows=16, cols=64,
          files={"sample.txt": THREE_LINES, "second.txt": "two\n"},
          args=["sample.txt", "second.txt"])
    check("menu-go-narrow", ["\x1bg"], rows=12, cols=26,
          files={"sample.txt": THREE_LINES})
    check("menu-go-256", ["\x1bg"], rows=16, cols=64,
          files={"sample.txt": THREE_LINES},
          env={"TERM": "xterm-256color"})

    # Find in files. The goldens run in a scratch directory that is not a work
    # tree, so this is grep -r over every file the run has written so far --
    # which is why the word is one no other golden's text contains, and the
    # file it is in is the only match whatever order the directory lists in.
    # View > Options: every setting, the first row selected, at both depths;
    # and at a size where the list has to scroll inside the dialog.
    check("options", ["\x1bv", "o"], files={"sample.txt": THREE_LINES})
    check("options-256", ["\x1bv", "o"], files={"sample.txt": THREE_LINES},
          env={"TERM": "xterm-256color"})
    check("options-narrow", ["\x1bv", "o", "\x1b[F"], rows=12, cols=34,
          files={"sample.txt": THREE_LINES})

    QUOKKA = {"sample.txt": THREE_LINES, "found.txt": "a\nthe quokka\nb\n"}
    check("grep-results", ["\x05", "quokka", "\r"], files=QUOKKA)
    check("grep-results-256", ["\x05", "quokka", "\r"], files=QUOKKA,
          env={"TERM": "xterm-256color"})
    check("grep-results-narrow", ["\x05", "quokka", "\r"], rows=12, cols=30,
          files=QUOKKA)

    # The Edit menu, which Select all and Select line joined. Nothing had
    # captured it either -- the same gap the View menu had, and the reason both
    # are here now: Cut and Copy are greyed out without a selection, so the
    # dimming and the separator placement are only provable by snapshot.
    # Twenty rows rather than sixteen, which is what this menu now needs to show
    # itself whole: a drop-down is clamped to the text area, the row after the
    # box belongs to its drop shadow, and thirteen rows of Edit menu plus a
    # shadow do not fit above a sixteen-row terminal's status bar. The snapshot
    # is here for the dimming and the separators, so it is given the room; what
    # the menu does when it has less is menu-edit-narrow, below.
    check("menu-edit", ["\x1be"], rows=20, cols=64,
          files={"sample.txt": THREE_LINES})
    # With a selection, so the three items that depend on one are live. This is
    # the pair menu-search-disabled / menu-search-enabled already makes for the
    # Search menu.
    check("menu-edit-selected", ["\x1b[1;2C", "\x1b[1;2C", "\x1be"],
          rows=20, cols=64, files={"sample.txt": THREE_LINES})
    # Narrower *and* shorter than the drop-down. The Edit menu needs thirteen
    # rows -- two borders, eight items and three separators -- and this screen
    # has eight to give it: the band takes one, the status row and the message
    # bar take two, and the drop shadow's row is the slack the height leaves.
    # So the box is eight rows, the list is scrolled to keep the highlighted
    # item in it, and an arrow sits on the border at each end that has more
    # behind it. This used to be the snapshot of the box being clamped to the
    # *grid* instead: the tail was drawn off the bottom where it could be
    # selected but not seen, and the shadow dimmed half the status bar.
    check("menu-edit-narrow", ["\x1be"], rows=12, cols=28,
          files={"sample.txt": THREE_LINES})
    # The same menu scrolled to its end, which is the other arrow state: End
    # takes the highlight to the last item, the box follows it down, and the
    # bottom arrow goes out because there is nothing behind it any more.
    check("menu-edit-scrolled", ["\x1be", "\x1b[F"], rows=12, cols=28,
          files={"sample.txt": THREE_LINES})
    check("menu-edit-scrolled-256", ["\x1be", "\x1b[F"], rows=12, cols=28,
          files={"sample.txt": THREE_LINES},
          env={"TERM": "xterm-256color"})
    # The same menu on a screen tall enough for it, which is what the narrow
    # width alone was meant to be testing.
    check("menu-edit-short-cols", ["\x1be"], rows=20, cols=28,
          files={"sample.txt": THREE_LINES})
    check("menu-edit-256", ["\x1be"], rows=20, cols=64,
          files={"sample.txt": THREE_LINES},
          env={"TERM": "xterm-256color"})

    # The View menu, which is where the ticks and Save Settings live: two checked
    # items whose state shows as a tick rather than as text, a separator above
    # and below Redraw, and an item that writes a file. Nothing captured this menu
    # until Save Settings was added to it, which is exactly the gap a new item in
    # an uncaptured menu leaves -- a lost tick or a separator in the wrong place
    # would have gone unnoticed.
    # Eighteen rows, which is what the menu needs now that Soft wrap is in it:
    # thirteen for the box and one for the shadow, above the status row and the
    # message bar. At sixteen the last item scrolls out, and this snapshot is
    # the one that proves Save Settings is there.
    check("menu-view", ["\x1bv"], rows=18, cols=64,
          files={"sample.txt": THREE_LINES})
    # With both of its ticks on, so the tick column is proved rather than assumed.
    check("menu-view-checked", ["\x15", "\x1bv"], rows=18, cols=64,
          files={"sample.txt": THREE_LINES})
    # Narrow, which is where a drop-down wider than the terminal shows. There are
    # two of these because there are two ways the box can be squeezed, and they
    # look nothing alike: at 24 columns the labels are cut and the shortcuts stay
    # aligned down the right, and at 18 there is no label worth reading left
    # beside a shortcut, so the shortcut column goes entirely and the labels get
    # the whole interior.
    #
    # Both used to be broken in the same way -- the box was clamped, the label was
    # drawn in full anyway, and the shortcut was painted over its tail, so the
    # first item read "Command pCtrl-Space". What these pin now is a gap between
    # the two columns at every width, and a column of padding before the right
    # border so a squeezed label does not look like it has broken out of the box.
    # Eighteen rows rather than twelve, for the reason menu-edit was given
    # twenty: these two are about the *width*, and on twelve rows the box is
    # scrolled as well as squeezed, which proves the squeeze over four items
    # instead of ten. menu-edit-narrow is where a short screen is the subject.
    check("menu-view-narrow", ["\x1bv"], rows=18, cols=24,
          files={"sample.txt": THREE_LINES})
    check("menu-view-tiny", ["\x1bv"], rows=18, cols=18,
          files={"sample.txt": THREE_LINES})
    # The Search menu is the one with the widest shortcut (Shift-F3), so it is
    # where the squeeze starts earliest.
    check("menu-search-narrow", ["\x1bs"], rows=14, cols=30,
          files={"sample.txt": THREE_LINES})
    # And at 256 colours, where the band is a shade darker than a drop-down.
    check("menu-view-256", ["\x1bv"], rows=18, cols=64,
          files={"sample.txt": THREE_LINES},
          env={"TERM": "xterm-256color"})
    check("menu-search-256", ["\x1bs"], rows=16, cols=64,
          files={"sample.txt": THREE_LINES},
          env={"TERM": "xterm-256color"})

    # The command palette: a framed dialog with a field at the top, the list of
    # commands under it and the chord each is bound to down the right-hand side.
    check("palette", ["\x00"], rows=20, cols=72,
          files={"sample.txt": THREE_LINES})
    # Filtered, which is where the ranking shows: "li" puts Line numbers first.
    check("palette-filtered", ["\x00", "li"], rows=20, cols=72,
          files={"sample.txt": THREE_LINES})
    # An unavailable command is listed and dimmed rather than hidden, which is a
    # colour difference and so only a snapshot would catch it going away.
    check("palette-dimmed", ["\x00", "paste"], rows=20, cols=72,
          files={"sample.txt": THREE_LINES},
          env={"TERM": "xterm-256color"})
    # At 256 colours the field is sunk a shade below the dialog around it.
    check("palette-256", ["\x00"], rows=20, cols=72,
          files={"sample.txt": THREE_LINES},
          env={"TERM": "xterm-256color"})

    # The prompt, which is an input field on the message row -- reached through Go
    # to line, now that Find is a dialog. An answer that fits is the line molly
    # always drew; one that does not scrolls, and the hint stays on screen instead
    # of being pushed off the edge.
    check("prompt-scrolled", ["\x07"] + list("abcdefghijklmnopqrstuvwxyz0123456789"),
          rows=10, cols=50, files={"sample.txt": THREE_LINES})
    # Too narrow for both: the hint goes rather than the answer.
    check("prompt-narrow", ["\x07", "abcdefgh"], rows=8, cols=20,
          files={"sample.txt": THREE_LINES})

    # Resizing: the grid is reallocated and the frame repainted in full. Shrinking
    # has to drop rows and columns without leaving anything behind; growing has to
    # fill what was not there before.
    check("resize-shrink", [(12, 40)], rows=24, cols=80, final=(12, 40),
          files={"sample.txt": THREE_LINES})
    check("resize-grow", [(28, 100)], rows=12, cols=40, final=(28, 100),
          files={"sample.txt": THREE_LINES})
    # A resize with the gutter on, where the column arithmetic moves with it.
    check("resize-gutter", ["\x15", (14, 36)], rows=24, cols=80, final=(14, 36),
          files={"sample.txt": THREE_LINES})

    # Window splits. Snapshots carry most of the weight here for the reason the
    # roadmap predicted: what splits change is where every column of every row
    # lands, and the long tail of that is off-by-one. A pty check that the text
    # is on screen twice would pass with the separator in the wrong column, the
    # gutter measured against the screen instead of the window, or the second
    # window's status bar a row out.
    SPLIT_RIGHT = "\x1b[18~"   # F7
    SPLIT_BELOW = "\x1b[19~"   # F8
    NEXT_WINDOW = "\x1b[17~"   # F6
    CLOSE_WINDOW = "\x1b[20~"  # F9

    check("split-right", [SPLIT_RIGHT], rows=16, cols=64,
          files={"sample.txt": THREE_LINES})
    check("split-right-256", [SPLIT_RIGHT], rows=16, cols=64,
          files={"sample.txt": THREE_LINES},
          env={"TERM": "xterm-256color"})
    check("split-below", [SPLIT_BELOW], rows=16, cols=64,
          files={"sample.txt": THREE_LINES})
    # Right, then below, so the second cut is inside the first one's half: the
    # case a flat list of windows could not have produced at all.
    check("split-nested", [SPLIT_RIGHT, SPLIT_BELOW], rows=16, cols=64,
          files={"sample.txt": THREE_LINES})
    # The keyboard back in the first window, which is the only state in which
    # the unfocused status bar is on the right-hand side -- and the one that
    # shows the two bars are told apart at all.
    check("split-focus", [SPLIT_RIGHT, NEXT_WINDOW], rows=16, cols=64,
          files={"sample.txt": THREE_LINES})
    check("split-focus-256", [SPLIT_RIGHT, NEXT_WINDOW], rows=16, cols=64,
          files={"sample.txt": THREE_LINES},
          env={"TERM": "xterm-256color"})
    # The gutter in a split window. Its width is measured from the window and
    # not from the screen, and the horizontal scroll and the cursor column are
    # both measured from the gutter -- the three things the roadmap named.
    check("split-gutter", ["\x15", SPLIT_RIGHT], rows=16, cols=64,
          files={"sample.txt": THREE_LINES})
    # Two files, one per window, with the tab bar above both. The status bars
    # are the only thing on screen that says which window holds which file.
    check("split-two-files", [SPLIT_RIGHT, "\x1b[6;5~"], rows=16, cols=64,
          files={"sample.txt": THREE_LINES, "second.txt": "two\nlines\n"},
          args=["sample.txt", "second.txt"])
    # Wide characters and a tab in a window half the screen wide, because the
    # alignment is measured in columns from the window's left edge.
    check("split-wide", [SPLIT_RIGHT], rows=14, cols=48,
          files={"sample.txt": WIDE})
    # Narrow enough that the halves are near the minimum, and then too narrow
    # to split at all -- which is a message rather than a window.
    check("split-narrow", [SPLIT_RIGHT], rows=12, cols=26,
          files={"sample.txt": THREE_LINES})
    check("split-refused", [SPLIT_RIGHT], rows=12, cols=16,
          files={"sample.txt": THREE_LINES})
    # Closing puts the survivor back across the whole screen, parent split node
    # and all, which is the half of the tree surgery nothing else would show.
    check("split-closed", [SPLIT_RIGHT, CLOSE_WINDOW], rows=16, cols=64,
          files={"sample.txt": THREE_LINES})
    # A resize under a split: the cut keeps its proportion rather than its
    # column count, which is why the node stores a fraction.
    check("split-resized", [SPLIT_RIGHT, (12, 40)], rows=16, cols=64,
          final=(12, 40), files={"sample.txt": THREE_LINES})

    # The Window menu. Everything but the two splits greys out with one window
    # open, so this is the dimmed state and the one below is the live one.
    check("menu-window", ["\x1bw"], rows=16, cols=64,
          files={"sample.txt": THREE_LINES})
    check("menu-window-split", [SPLIT_RIGHT, "\x1bw"], rows=16, cols=64,
          files={"sample.txt": THREE_LINES})
    check("menu-window-256", ["\x1bw"], rows=16, cols=64,
          files={"sample.txt": THREE_LINES},
          env={"TERM": "xterm-256color"})

    # Multi-cursor. The terminal has one real cursor and the primary keeps it,
    # so every other cursor is a cell in a role of its own -- which means the
    # only proof it is drawn at all is a snapshot, and the only proof the two
    # colour depths agree is one of each.
    #
    # Ctrl-D twice: the first press selects the word under the cursor, the
    # second puts a cursor on the next occurrence. So this holds two reverse
    # video runs (the selections) and one coloured cell (the far cursor), on
    # two different rows, which is the whole feature in one screen.
    # A block selection: a cursor and a highlighted slice on every row it
    # spans, including a short one, where the cursor sits at the row's end.
    # The second is over WIDE, where the same display columns are different
    # bytes on each row and a tab and a wide character straddle the edges.
    BLOCK = ["\x1b[C", "\x1b[1;4B", "\x1b[1;4B", "\x1b[1;4C", "\x1b[1;4C",
             "\x1b[1;4C"]
    check("block-keys", BLOCK, rows=10, cols=40, files={"sample.txt": THREE_LINES})
    check("block-keys-256", BLOCK, rows=10, cols=40,
          files={"sample.txt": THREE_LINES}, env={"TERM": "xterm-256color"})
    check("block-wide", ["\x1b[C", "\x1b[1;4B", "\x1b[1;4B"] + ["\x1b[1;4C"] * 4,
          rows=10, cols=40, files={"sample.txt": WIDE})
    check("cursors-two", ["\x04", "\x04"], rows=12, cols=40,
          files={"sample.txt": MATCHES})
    check("cursors-two-256", ["\x04", "\x04"], rows=12, cols=40,
          files={"sample.txt": MATCHES},
          env={"TERM": "xterm-256color"})
    # Cursors added straight down with Ctrl-Down, with no selections on them:
    # the blocks on their own, and the status bar's count.
    check("cursors-column", ["\x1b[1;5B", "\x1b[1;5B"], rows=12, cols=40,
          files={"sample.txt": MATCHES})
    check("cursors-column-256", ["\x1b[1;5B", "\x1b[1;5B"], rows=12, cols=40,
          files={"sample.txt": MATCHES},
          env={"TERM": "xterm-256color"})
    # The gutter on, where the column arithmetic for a shaded cell is easiest
    # to get wrong -- a cursor drawn one column out lands in the line numbers.
    check("cursors-gutter", ["\x15", "\x04", "\x04"], rows=12, cols=40,
          files={"sample.txt": MATCHES})
    # Narrow enough that the status bar's cursor count is what gets squeezed.
    check("cursors-narrow", ["\x04", "\x04"], rows=8, cols=24,
          files={"sample.txt": MATCHES})

    print()
    if failed:
        for name, detail in failed:
            print("  FAIL  %s" % name)
            print(detail)
        print("%d passed, %d failed" % (passed, len(failed)))
        return 1
    print("%d passed, 0 failed" % passed)
    return 0


if __name__ == "__main__":
    sys.exit(main())
