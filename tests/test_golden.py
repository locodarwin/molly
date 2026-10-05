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


def check(name, keys, rows=24, cols=80, files=None, args=None, settle=0.5):
    """Run molly against a fresh copy of `files`, snapshot the screen, compare."""
    global passed

    # A fixed filename, because it is drawn in the status bar: a temp path would
    # put a different string in the snapshot on every run.
    for fname, content in (files or {}).items():
        with open(fname, "w") as f:
            f.write(content)

    argv = [MOLLY] + (args if args is not None else ["sample.txt"])
    out = pty_harness.run(argv, keys, rows=rows, cols=cols, settle=settle)

    scr = Screen(rows, cols)
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
    # Run from a scratch directory so the status bar shows a stable name.
    import tempfile
    tmp = tempfile.mkdtemp(prefix="molly-golden-")
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
