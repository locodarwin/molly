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

    # Search: the prompt on the message line and the match the cursor lands on.
    check("search-prompt", ["\x06", "b", "e"], files={"sample.txt": THREE_LINES})

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
    check("menu-bar-hidden", ["\x1bv", "m"], rows=16, cols=64,
          files={"sample.txt": THREE_LINES})
    check("menu-bar-hidden-open", ["\x1bv", "m", "\x1b[21~"], rows=16, cols=64,
          files={"sample.txt": THREE_LINES})
    # And at 256 colours, where the band is a shade darker than a drop-down.
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

    # The prompt, which is an input field on the message row. An answer that fits is
    # the line molly always drew; one that does not scrolls, and the hint stays on
    # screen instead of being pushed off the edge.
    check("prompt-scrolled", ["\x06"] + list("abcdefghijklmnopqrstuvwxyz0123456789"),
          rows=10, cols=50, files={"sample.txt": THREE_LINES})
    # Too narrow for both: the hint goes rather than the answer.
    check("prompt-narrow", ["\x06", "abcdefgh"], rows=8, cols=20,
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
