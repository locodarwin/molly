#!/usr/bin/env python3
"""Regression tests for molly.

Run with:  make test        (or: python3 tests/test_molly.py ./molly)

Every test here pins down a bug that was actually reachable from the keyboard,
so a failure means a real regression rather than a style drift.
"""

import fcntl
import multiprocessing
import os
import pty
import re
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pty_harness import run, run_until_signal, status_lines, cursor_col
from screen import Screen

MOLLY = None
SAVE = "\x13"
UNDO = "\x1a"
REDO = "\x19"
GOTO = "\x07"
TOGGLE_CASE = "\x1bsc"  # Search > Match case: the chord went to transpose
REGEX = "\x1br"  # Alt-R: read the query as a pattern
REPLACE = "\x12"
FAILURES = []
PASSED = 0

HOME, END = "\x1b[H", "\x1b[F"
UP, DOWN, RIGHT, LEFT = "\x1b[A", "\x1b[B", "\x1b[C", "\x1b[D"
# Shifted variants: CSI form "\x1b[1;2<final>" -- the ";2" is the Shift
# modifier. These are the keys that extend a selection.
SH_UP, SH_DOWN, SH_RIGHT, SH_LEFT = ("\x1b[1;2A", "\x1b[1;2B",
                                     "\x1b[1;2C", "\x1b[1;2D")
SH_HOME, SH_END = "\x1b[1;2H", "\x1b[1;2F"

# Clipboard keys (raw mode clears ISIG, so Ctrl-C reaches the editor as a byte).
COPY, CUT, PASTE = "\x03", "\x18", "\x16"


def run_interleaved(argv, before_keys, touch, after_keys, rows=24, cols=80):
    """Like pty_harness.run, but calls touch() between the two key batches --
    used to simulate another process modifying the file while molly still has
    it open, which is not expressible as a single flat key list."""
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(argv[0], argv, dict(os.environ, TERM="xterm"))

    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    out = b""
    time.sleep(0.3)

    def drain(timeout):
        nonlocal out
        while select.select([fd], [], [], timeout)[0]:
            try:
                chunk = os.read(fd, 1 << 20)
            except OSError:
                return
            if not chunk:
                return
            out += chunk

    def send(keys):
        for key in keys:
            if isinstance(key, float):
                time.sleep(key)
                continue
            try:
                os.write(fd, key.encode() if isinstance(key, str) else key)
            except OSError:
                break
            time.sleep(0.06)
            drain(0.02)

    send(before_keys)
    touch()
    send(after_keys)

    deadline = time.time() + 0.4
    while time.time() < deadline:
        drain(0.1)
    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass
    os.waitpid(pid, 0)
    try:
        os.close(fd)
    except OSError:
        pass
    return out


def check(label, got, want):
    global PASSED
    if got == want:
        PASSED += 1
        print("  pass  %s" % label)
    else:
        FAILURES.append(label)
        print("  FAIL  %s\n          want: %r\n          got:  %r"
              % (label, want, got))


def edit(tmpdir, original, keys, name="f.txt"):
    """Open a file with `original` in it, send `keys`, return it afterwards."""
    path = os.path.join(tmpdir, name)
    with open(path, "w") as fh:
        fh.write(original)
    run([MOLLY, path], keys)
    with open(path) as fh:
        return fh.read()


def edit_bytes(tmpdir, original, keys, name="f.txt"):
    """Like edit(), but writes and reads raw bytes -- for content that is not
    valid text (CRLF endings, no trailing newline, multibyte characters)."""
    path = os.path.join(tmpdir, name)
    with open(path, "wb") as fh:
        fh.write(original)
    run([MOLLY, path], keys)
    with open(path, "rb") as fh:
        return fh.read()


# --------------------------------------------------------------- input ----

def test_keys(tmpdir):
    print("\ninput: cursor keys move, they do not type")
    check("arrow right", edit(tmpdir, "ab\n", [RIGHT, "X", SAVE]), "aXb\n")
    check("arrow left", edit(tmpdir, "ab\n", [RIGHT, RIGHT, LEFT, "X", SAVE]),
          "aXb\n")
    check("arrow down", edit(tmpdir, "ab\ncd\n", [DOWN, "X", SAVE]),
          "ab\nXcd\n")
    check("arrow up", edit(tmpdir, "ab\ncd\n", [DOWN, UP, "X", SAVE]),
          "Xab\ncd\n")
    check("home", edit(tmpdir, "ab\n", [END, HOME, "X", SAVE]), "Xab\n")
    check("end", edit(tmpdir, "ab\n", [END, "X", SAVE]), "abX\n")
    check("home via \\x1b[1~",
          edit(tmpdir, "ab\n", [RIGHT, "\x1b[1~", "X", SAVE]), "Xab\n")
    check("end via \\x1b[4~", edit(tmpdir, "ab\n", ["\x1b[4~", "X", SAVE]),
          "abX\n")
    check("delete key", edit(tmpdir, "abc\n", ["\x1b[3~", SAVE]), "bc\n")
    check("page down", edit(tmpdir, "a\nb\nc\n", ["\x1b[6~", "X", SAVE]),
          "a\nb\nc\nX\n")
    check("page up",
          edit(tmpdir, "a\nb\nc\n", [DOWN, DOWN, "\x1b[5~", "X", SAVE]),
          "Xa\nb\nc\n")

    print("\ninput: SS3 (application cursor mode)")
    check("SS3 right", edit(tmpdir, "ab\n", ["\x1bOC", "X", SAVE]), "aXb\n")
    check("SS3 home", edit(tmpdir, "ab\n", [END, "\x1bOH", "X", SAVE]),
          "Xab\n")
    check("SS3 end", edit(tmpdir, "ab\n", ["\x1bOF", "X", SAVE]), "abX\n")

    # Regression: a partly-consumed escape sequence used to leave its tail in
    # the input stream, where it was inserted as ordinary text. Ctrl-Right
    # ("\x1b[1;5C") typed a literal "5C" into the file.
    print("\ninput: unmapped escape sequences are swallowed, never typed")
    for label, seq in [("ctrl-right", "\x1b[1;5C"),
                       ("shift-left", "\x1b[1;2D"),
                       ("ctrl-shift-up", "\x1b[1;6A"),
                       ("F5", "\x1b[15~"),
                       ("F12", "\x1b[24~"),
                       ("bracketed paste on", "\x1b[200~"),
                       ("bracketed paste off", "\x1b[201~"),
                       ("SGR mouse", "\x1b[<0;1;1M"),
                       ("private CSI", "\x1b[?1049h"),
                       ("bare ESC", "\x1b")]:
        check(label + " inserts nothing",
              edit(tmpdir, "ab\n", [seq, SAVE]), "ab\n")

    # Regression: the cursor can move down onto the phantom row past the
    # last line (typing there starts a new line), but Backspace did nothing
    # at all from that spot.
    print("\ninput: backspace from the phantom row past the last line")
    check("deletes the last character of the file",
          edit(tmpdir, "one\ntwo\n", [DOWN, DOWN, "\x7f", SAVE]), "one\ntw\n")
    check("works when the file has no trailing newline",
          edit(tmpdir, "one\ntwo", [DOWN, DOWN, "\x7f", SAVE]), "one\ntw")
    check("a run of them keeps deleting backwards",
          edit(tmpdir, "one\ntwo\n", [DOWN, DOWN, "\x7f\x7f", SAVE]), "one\nt\n")
    check("joins up when the last line is empty",
          edit(tmpdir, "one\n\n", [DOWN, DOWN, "\x7f", SAVE]), "one\n")
    check("undo restores what it deleted",
          edit(tmpdir, "one\ntwo\n", [DOWN, DOWN, "\x7f", UNDO, SAVE]),
          "one\ntwo\n")
    check("still does nothing in an empty buffer",
          edit(tmpdir, "", [DOWN, "\x7f", "X", SAVE]), "X\n")


def test_tab(tmpdir):
    print("\ninput: the Tab key types a tab")
    # Regression: Tab is a control character, and the keypress handler threw
    # away every control character it had no binding for, so a tab could not
    # be typed at all -- even though tabs already in a file displayed fine.
    check("tab at line start", edit(tmpdir, "ab\n", ["\t", SAVE]), "\tab\n")
    check("typing continues after the tab",
          edit(tmpdir, "ab\n", [RIGHT, "\t", "X", SAVE]), "a\tXb\n")
    check("tab undoes", edit(tmpdir, "ab\n", ["\t", "\x1a", SAVE]), "ab\n")
    check("unbound control characters are still not typed",
          edit(tmpdir, "ab\n", ["\x01\x02\x0b", SAVE]), "ab\n")


# -------------------------------------------------------------- search ----

def test_search(tmpdir):
    print("\nsearch: wraps all the way around, including the cursor's own row")
    path = os.path.join(tmpdir, "s.txt")

    def find(body, keys):
        with open(path, "w") as fh:
            fh.write(body)
        return (status_lines(run([MOLLY, path], keys)) or [""])[-1]

    # Regression: the sweep used to visit the starting row once, scanning only
    # from the cursor forward, so a match earlier on that row was unreachable.
    check("match before cursor, single line",
          find("alpha beta\n", [END, "\x06", "alpha", "\r"]).startswith("Found"),
          True)
    check("match before cursor, multi line",
          find("alpha beta\nsecond line\n",
               [END, "\x06", "alpha", "\r"]).startswith("Found"),
          True)
    check("match after cursor still found",
          find("alpha beta\n", ["\x06", "beta", "\r"]).startswith("Found"),
          True)
    check("genuinely absent term reports not found",
          find("alpha beta\n", ["\x06", "zzzz", "\r"]).startswith("Not found"),
          True)
    check("search is case-insensitive",
          find("Alpha\n", [END, "\x06", "alpha", "\r"]).startswith("Found"),
          True)


def test_case_sensitive_search(tmpdir):
    print("\nsearch: Search > Match case toggles case-sensitive matching")
    path = os.path.join(tmpdir, "cs.txt")

    def find(body, keys):
        with open(path, "w") as fh:
            fh.write(body)
        return (status_lines(run([MOLLY, path], keys)) or [""])[-1]

    check("toggling on reports the new mode",
          find("Alpha\n", [TOGGLE_CASE]),
          "Search is now case-sensitive")
    check("toggling again reports insensitive",
          find("Alpha\n", [TOGGLE_CASE, TOGGLE_CASE]),
          "Search is now case-insensitive")
    check("case-sensitive search misses a differently-cased match",
          find("Alpha\n", [TOGGLE_CASE, HOME, "\x06", "alpha", "\r"])
              .startswith("Not found"),
          True)
    check("case-sensitive search still finds an exact-case match",
          find("Alpha\n", [TOGGLE_CASE, HOME, "\x06", "Alpha", "\r"])
              .startswith("Found"),
          True)


def test_char_aligned_search(tmpdir):
    print("\nsearch: a match may only span whole characters")
    path = os.path.join(tmpdir, "u8.txt")

    def find(body, keys):
        with open(path, "wb") as fh:
            fh.write(body)
        return (status_lines(run([MOLLY, path], keys)) or [""])[-1]

    # Regression: search compared raw bytes, so a query made of UTF-8
    # continuation or lead bytes could match inside a multibyte character
    # and park the cursor (or cut a replace) in the middle of it.
    check("a query equal to a continuation byte finds nothing",
          find(b"caf\xc3\xa9\n", ["\x06", b"\xa9", "\r"]).startswith("Not found"),
          True)
    check("a query that ends mid-character finds nothing",
          find(b"a\xc3\xa9b\n", ["\x06", b"a\xc3", "\r"]).startswith("Not found"),
          True)
    check("a query that starts mid-character finds nothing",
          find(b"a\xc3\xa9b\n", ["\x06", b"\xa9b", "\r"]).startswith("Not found"),
          True)
    check("a whole multibyte character still matches",
          find(b"caf\xc3\xa9\n", ["\x06", b"\xc3\xa9", "\r"]).startswith("Found"),
          True)
    check("case folding covers ASCII letters only, never raw bytes",
          find(b"caf\xc3\xa9\n", ["\x06", b"CAF\xc3\x89", "\r"])
              .startswith("Not found"),
          True)
    check("ASCII case-insensitive matching is unaffected",
          find(b"ALPHA caf\xc3\xa9\n", ["\x06", "alpha", "\r"]).startswith("Found"),
          True)


# --------------------------------------------------------------- goto ----

def test_goto_line(tmpdir):
    print("\ngoto: Ctrl-G moves the cursor to a line, clamped and validated")
    body = "one\ntwo\nthree\nfour\nfive\n"
    check("goes to an existing line",
          edit(tmpdir, body, [GOTO, "3", "\r", "X", SAVE]),
          "one\ntwo\nXthree\nfour\nfive\n")
    check("out-of-range input clamps to the last line",
          edit(tmpdir, body, [GOTO, "999", "\r", "X", SAVE]),
          "one\ntwo\nthree\nfour\nXfive\n")
    check("line 0 clamps to the first line",
          edit(tmpdir, body, [GOTO, "0", "\r", "X", SAVE]),
          "Xone\ntwo\nthree\nfour\nfive\n")
    check("a negative line clamps to the first line",
          edit(tmpdir, body, [GOTO, "-1", "\r", "X", SAVE]),
          "Xone\ntwo\nthree\nfour\nfive\n")
    # Regression: atoi() on non-numeric input silently returns 0, which (as
    # line - 1) sent the cursor to row -1 -- an out-of-bounds row access.
    # strtol-based validation rejects it outright instead.
    check("non-numeric input is rejected, cursor left unmoved",
          edit(tmpdir, body, [GOTO, "abc", "\r", "X", SAVE]),
          "Xone\ntwo\nthree\nfour\nfive\n")
    check("ESC cancels without moving the cursor",
          edit(tmpdir, body, [GOTO, "3", "\x1b", 0.15, "X", SAVE]),
          "Xone\ntwo\nthree\nfour\nfive\n")
    check("an empty buffer does not crash (E.cy == E.numrows == 0)",
          edit(tmpdir, "", [GOTO, "5", "\r", "X", SAVE]),
          "X\n")


# ------------------------------------------------------------- replace ----

def test_replace(tmpdir):
    print("\nreplace: Ctrl-R replaces the next match or every match")
    check("replace next only touches the first match",
          edit(tmpdir, "cat cat cat\n",
               [REPLACE, "cat", "\r", "dog", "\r", "n", "\r", SAVE]),
          "dog cat cat\n")
    check("replace all touches every match",
          edit(tmpdir, "cat cat cat\n",
               [REPLACE, "cat", "\r", "dog", "\r", "a", "\r", SAVE]),
          "dog dog dog\n")
    check("empty search term aborts, buffer untouched",
          edit(tmpdir, "cat\n", [REPLACE, "\r", SAVE]),
          "cat\n")
    check("ESC at the search prompt aborts",
          edit(tmpdir, "cat\n", [REPLACE, "\x1b", 0.15, "X", SAVE]),
          "Xcat\n")
    check("ESC at the replacement prompt aborts, nothing replaced",
          edit(tmpdir, "cat\n", [REPLACE, "cat", "\r", "\x1b", SAVE]),
          "cat\n")
    # Esc used to mean "next" here, which made the next-or-all prompt the one
    # place in molly where cancelling did something.
    check("ESC at the next-or-all prompt aborts too",
          edit(tmpdir, "cat cat\n",
               [REPLACE, "cat", "\r", "dog", "\r", "\x1b", 0.15, SAVE]),
          "cat cat\n")
    check("replace-all is case-insensitive by default",
          edit(tmpdir, "Cat cat CAT\n",
               [REPLACE, "cat", "\r", "dog", "\r", "a", "\r", SAVE]),
          "dog dog dog\n")
    check("the case-sensitivity setting is honored by replace",
          edit(tmpdir, "Cat cat CAT\n",
               [TOGGLE_CASE, REPLACE, "cat", "\r", "dog", "\r", "a", "\r",
                SAVE]),
          "Cat dog CAT\n")
    # Regression: replace-all must terminate even when the replacement text
    # contains the search term -- it must not keep re-matching its own
    # output.
    check("a replacement containing the search term still terminates",
          edit(tmpdir, "a a a\n",
               [REPLACE, "a", "\r", "aa", "\r", "a", "\r", SAVE]),
          "aa aa aa\n")
    # Regression: that guard only covered the cursor's own row -- the sweep
    # still wrapped around to earlier rows and rescanned them from column 0,
    # so on a multi-row file a replacement containing the search term was
    # re-matched forever (100% CPU, unbounded memory until the OOM killer).
    check("a replacement containing the search term terminates across rows",
          edit(tmpdir, "a\na\n",
               [REPLACE, "a", "\r", "ba", "\r", "a", "\r", SAVE]), "ba\nba\n")
    check("an identity replace-all of every match terminates",
          edit(tmpdir, "ab\nba\n",
               [REPLACE, "a", "\r", "a", "\r", "a", "\r", SAVE]), "ab\nba\n")
    check("one undo restores a self-referential multi-row replace-all",
          edit(tmpdir, "a\na\n",
               [REPLACE, "a", "\r", "ba", "\r", "a", "\r", UNDO, SAVE]),
          "a\na\n")
    # Replace-next must keep wrapping: it reaches matches on rows before
    # the cursor (the forward-only sweep is replace-all's, not its own).
    check("replace-next still wraps to earlier rows",
          edit(tmpdir, "a\nb\n",
               [DOWN, REPLACE, "a", "\r", "Q", "\r", "n", "\r", SAVE]),
          "Q\nb\n")

    print("\nreplace: a match spanning a tab-expanded region deletes the "
          "right bytes")
    # This was a regression test for search matching the tab-expanded render
    # buffer, where a match's length in render bytes was not its length in the
    # row's own bytes -- deleting the one as if it were the other took eight
    # bytes out of a four-byte row. Both are the row's bytes now, so that
    # divergence is gone; what still has to hold is that a match spanning a tab
    # is deleted at the right length, and \t is the only way to name a tab,
    # since a prompt has no way to type one.
    check("a replace across a tab spans the tab as one character",
          edit(tmpdir, "ab\tc\n",
               [REGEX, REPLACE, r"b\tc", "\r", "Q", "\r", "n", "\r", SAVE]),
          "aQ\n")

    print("\nreplace: replace-all undoes and redoes as a single step")
    check("one undo restores the original after replace-all",
          edit(tmpdir, "cat cat cat\n",
               [REPLACE, "cat", "\r", "dog", "\r", "a", "\r", UNDO, SAVE]),
          "cat cat cat\n")
    check("one redo re-applies the whole replace-all",
          edit(tmpdir, "cat cat cat\n",
               [REPLACE, "cat", "\r", "dog", "\r", "a", "\r",
                UNDO, REDO, SAVE]),
          "dog dog dog\n")


# ---------------------------------------------------------------- save ----

def test_regex_search(tmpdir):
    print("\nsearch: Alt-R reads the query as a regular expression")
    path = os.path.join(tmpdir, "rx.txt")

    def go(body, keys):
        """The last message, and where the cursor ended up."""
        with open(path, "w") as fh:
            fh.write(body)
        out = run([MOLLY, path], keys)
        msg = (status_lines(out) or [""])[-1]
        pos = re.findall(r"Ln (\d+), Col (\d+)", out.decode("utf-8", "replace"))
        return msg, (pos[-1] if pos else None)

    check("the toggle says which mode it is now in",
          go("abc\n", [REGEX])[0], "Search is now a regular expression")
    check("and toggling again goes back",
          go("abc\n", [REGEX, REGEX])[0], "Search is now plain text")

    # The whole point: a query that is not the text being looked for.
    check("a shorthand class finds the digits",
          go("foo 123 bar\n", [REGEX, "\x06", r"\d+", "\r"]),
          ("Found", ("1", "5")))
    check("^ is the start of a row, not of the file",
          go("ab\nbc\n", [REGEX, "\x06", "^b", "\r"])[1], ("2", "1"))
    check("$ is the end of a row",
          go("ab\nxb\n", [REGEX, "\x06", "a$", "\r"])[0].startswith("Not found"),
          True)
    check("a pattern that is nowhere reports not found",
          go("foo\n", [REGEX, "\x06", r"\d+", "\r"])[0].startswith("Not found"),
          True)
    check("Ctrl-N steps to the next match",
          go("x1x2x3\n", [REGEX, "\x06", r"\d", "\r", "\x0e"])[1], ("1", "4"))
    check("Ctrl-P steps back to the previous one",
          go("x1x2x3\n",
             [REGEX, "\x06", r"\d", "\r", "\x0e", "\x0e", "\x10"])[1],
          ("1", "4"))
    check("a pattern matches regardless of case by default",
          go("ABC\n", [REGEX, "\x06", "[a-z]+", "\r"])[0], "Found")
    check("and Match case applies to a pattern too",
          go("ABC\n", [REGEX, TOGGLE_CASE, "\x06", "[a-z]+", "\r"])[0]
          .startswith("Not found"), True)

    # A pattern matches the row as it is stored; a plain search matches the row
    # as it is drawn. That is why \t finds a tab and spaces do not, and why
    # eight spaces find a tab in plain mode. Both are in the manual.
    check(r"\t finds a tab, which a plain search cannot look for",
          go("a\tb\n", [REGEX, "\x06", r"a\tb", "\r"])[0], "Found")
    # The tab after the a reaches the next stop at column 8, so the drawn row is
    # an a, seven spaces and a b -- and neither mode sees it that way, because
    # both match the row as it is stored. olly's search matched the drawn row and
    # so found a tab when asked for spaces.
    check("a pattern does not see the tab as spaces",
          go("a\tb\n", [REGEX, "\x06", "a" + " " * 7 + "b", "\r"])[0]
          .startswith("Not found"), True)
    check("and nor does a plain search",
          go("a\tb\n", ["\x06", "a" + " " * 7 + "b", "\r"])[0]
          .startswith("Not found"), True)

    check("a pattern that does not compile says why",
          go("abc\n", [REGEX, "\x06", "a(", "\r"])[0],
          "Bad pattern: missing ) in pattern")
    check("and so does one that cannot be given a straight answer",
          go("a" * 30 + "b\n", [REGEX, "\x06", "(a+)+$", "\r"])[0],
          "Pattern too complex. Try a simpler one")
    check("with the mode off the query is text again",
          go("a.c\nabc\n", [REGEX, REGEX, "\x06", "a.c", "\r"])[1], ("1", "1"))


def test_regex_replace(tmpdir):
    print("\nreplace: patterns, and \\1 for a group")

    check("groups come back in the replacement",
          edit(tmpdir, "k=v\n",
               [REGEX, REPLACE, r"(\w+)=(\w+)", "\r", r"\2=\1", "\r", "a",
                "\r", SAVE]),
          "v=k\n")
    check("replace-all over a class",
          edit(tmpdir, "a1 b22 c333\n",
               [REGEX, REPLACE, r"\d+", "\r", "N", "\r", "a", "\r", SAVE]),
          "aN bN cN\n")
    check("$ is every row's end",
          edit(tmpdir, "ab\ncd\n",
               [REGEX, REPLACE, "$", "\r", "!", "\r", "a", "\r", SAVE]),
          "ab!\ncd!\n")
    # . is one character, not one byte, so this replaces two accented letters
    # with two dashes rather than four.
    check(". consumes a whole character",
          edit(tmpdir, "\u00e9\u00e0\n",
               [REGEX, REPLACE, ".", "\r", "-", "\r", "a", "\r", SAVE]),
          "--\n")
    # A pattern can match nothing at all, and then the sweep has to step over a
    # character or it would insert at the same place for ever. sed puts a dash
    # before every character and one at the end; so does molly.
    check("a pattern that matches nothing still terminates",
          edit(tmpdir, "abc\n",
               [REGEX, REPLACE, "x*", "\r", "-", "\r", "a", "\r", SAVE]),
          "-a-b-c-\n")
    check("one undo puts a regex replace-all back",
          edit(tmpdir, "a1 b2 c3\n",
               [REGEX, REPLACE, r"\d", "\r", "N", "\r", "a", "\r", UNDO,
                SAVE]),
          "a1 b2 c3\n")
    check("a group number the pattern does not have changes nothing",
          edit(tmpdir, "a\n",
               [REGEX, REPLACE, "(a)", "\r", r"\2", "\r", "a", "\r", SAVE]),
          "a\n")
    check("and neither does a pattern that will not compile",
          edit(tmpdir, "abc\n", [REGEX, REPLACE, "a(", "\r", SAVE]),
          "abc\n")
    check("with the mode off, . is a dot again",
          edit(tmpdir, "a.c\n",
               [REGEX, REGEX, REPLACE, ".", "\r", "-", "\r", "a", "\r",
                SAVE]),
          "a-c\n")


def test_find_dialog(tmpdir):
    print("\nfind: Ctrl-F opens a dialog with the term and both settings on it")
    path = os.path.join(tmpdir, "fd.txt")
    BTAB = "\x1b[Z"   # Shift-Tab, which has a sequence of its own

    def go(body, keys, rows=24, cols=80):
        """The last message, where the cursor ended up, and the screen."""
        with open(path, "w") as fh:
            fh.write(body)
        out = run([MOLLY, path], keys, rows=rows, cols=cols)
        sc = Screen(rows, cols)
        sc.feed(out)
        msg = (status_lines(out, rows=rows, cols=cols) or [""])[-1]
        pos = re.findall(r"Ln (\d+), Col (\d+)", out.decode("utf-8", "replace"))
        return msg, (pos[-1] if pos else None), [l.rstrip() for l in sc.text()]

    def press(col, row, button=0):
        return "\x1b[<%d;%d;%dM" % (button, col, row)

    def release(col, row, button=0):
        return "\x1b[<%d;%d;%dm" % (button, col, row)

    screen = go("alpha beta\n", ["\x06"])[2]
    check("the dialog is titled Find",
          any("Find" in l and "\u2550" in l for l in screen), True)
    check("with a field, two settings and two buttons",
          [any("Find what:" in l for l in screen),
           any("[ ] Match case" in l for l in screen),
           any("[ ] Regular expression" in l for l in screen),
           any("[ Find ]" in l and "[ Cancel ]" in l for l in screen)],
          [True, True, True, True])

    # Enter presses the default button from anywhere, which is what makes the
    # dialog behave like the prompt it replaced: type the term, press Enter.
    check("a term and Enter finds it",
          go("alpha beta\n", ["\x06", "beta", "\r"])[:2],
          ("Found", ("1", "7")))
    check("and a term that is not there says so",
          go("alpha\n", ["\x06", "zzz", "\r"])[0].startswith("Not found"), True)
    check("Esc closes it without searching",
          go("alpha\n", ["\x06", "alpha", "\x1b", 0.15])[0].startswith("Found"),
          False)

    # Tab moves the focus, Space ticks what it lands on, and Find commits both
    # settings -- which is checked by toggling afterwards and reading what it said
    # *from*.
    check("Tab and Space tick Match case, and Find commits it",
          go("ALPHA\n", ["\x06", "alpha", "\t", " ", "\r"])[0]
          .startswith("Not found"), True)
    check("which the menu item then toggles back off",
          go("ALPHA\n", ["\x06", "alpha", "\t", " ", "\r", TOGGLE_CASE])[0],
          "Search is now case-insensitive")
    check("cancelling commits nothing",
          go("ALPHA\n", ["\x06", "\t", " ", "\x1b", 0.15, TOGGLE_CASE])[0],
          "Search is now case-sensitive")
    check("the second checkbox reads the term as a pattern",
          go("a1 b22\n", ["\x06", r"\d+", "\t", "\t", " ", "\r"])[:2],
          ("Found", ("1", "2")))

    # Shift-Tab arrives as its own sequence. Before the input layer knew it, it
    # fell through to Esc -- so going backwards through the focus order closed
    # the dialog.
    check("Shift-Tab goes back round without closing the dialog",
          any("[ Find ]" in l for l in go("alpha\n", ["\x06", BTAB])[2]), True)
    check("Space presses a focused button",
          go("alpha beta\n", ["\x06", "beta", "\t", "\t", "\t", " "])[:2],
          ("Found", ("1", "7")))
    # Cancel is three Tabs past the two checkboxes and Find. Reaching it and
    # pressing it must leave the buffer alone, which the typed X proves.
    check("the Cancel button closes without searching",
          edit(tmpdir, "alpha\n",
               ["\x06", "alpha", "\t", "\t", "\t", "\t", "\r", 0.15, "X",
                SAVE]),
          "Xalpha\n")

    # The mouse. Rows are screen rows, 1-based, as the terminal counts them: on an
    # 80x24 screen the dialog's checkboxes land on 12 and 13 and its buttons on 15.
    check("a click ticks a checkbox",
          any("[\u2713] Match case" in l
              for l in go("alpha\n", ["\x06", press(18, 12),
                                      release(18, 12)])[2]), True)
    check("a click presses Find",
          go("alpha beta\n",
             ["\x06", "beta", press(33, 15), release(33, 15)])[:2],
          ("Found", ("1", "7")))
    check("and a click on Cancel does not",
          go("alpha\n",
             ["\x06", "alpha", press(43, 15), release(43, 15)])[0]
          .startswith("Found"), False)

    # The hint row says what Enter alone will do, which the prompt never did.
    check("the hint names the term Enter would repeat",
          any('repeats "beta"' in l
              for l in go("alpha beta\n", ["\x06", "beta", "\r", "\x06"])[2]),
          True)

    # A terminal too short for all six content rows drops the hint and then a
    # setting, and keeps the buttons on the last row -- a dialog whose buttons are
    # off the bottom looks broken even though Enter still works.
    short = go("alpha\n", ["\x06"], rows=6, cols=20)[2]
    check("on a six-row terminal the buttons keep the last row",
          [any("[ Find ]" in l for l in short),
           any("Match case" in l for l in short),
           any("Space ticks" in l for l in short)],
          [True, True, False])
    check("and nothing is drawn outside the frame",
          all(l == "" or (l.startswith("\u2554") or l.startswith("\u2551")
                          or l.startswith("\u255a"))
              for l in short), True)


def test_save(tmpdir):
    print("\nsave: naming")
    # Regression: answering the "Save as" prompt with an empty line used to set
    # the filename to "", which could never be written and, being non-NULL,
    # suppressed the prompt forever -- stranding the buffer with no way out.
    work = tempfile.mkdtemp(dir=tmpdir)
    # Type, Ctrl-S, answer the prompt with an empty line, then Ctrl-S again:
    # the second Ctrl-S must re-prompt, so the named file ends up written.
    cwd = os.getcwd()
    os.chdir(work)
    try:
        run([MOLLY], ["abc", SAVE, "\r", SAVE, "named.txt", "\r"])
        check("empty answer re-prompts instead of stranding the buffer",
              os.path.exists(os.path.join(work, "named.txt")), True)
        if os.path.exists(os.path.join(work, "named.txt")):
            with open(os.path.join(work, "named.txt")) as fh:
                check("re-prompted save wrote the buffer", fh.read(), "abc\n")
    finally:
        os.chdir(cwd)

    print("\nsave: the temp file must not collide with the user's own files")
    # Regression: the temp file was a fixed "<target>.tmp", unlinked without
    # looking, so saving notes.txt destroyed an unrelated notes.txt.tmp.
    d = tempfile.mkdtemp(dir=tmpdir)
    with open(os.path.join(d, "notes.txt"), "w") as fh:
        fh.write("data\n")
    with open(os.path.join(d, "notes.txt.tmp"), "w") as fh:
        fh.write("A REAL FILE\n")
    run([MOLLY, os.path.join(d, "notes.txt")], ["X", SAVE])
    check("unrelated .tmp file survives",
          os.path.exists(os.path.join(d, "notes.txt.tmp")), True)
    with open(os.path.join(d, "notes.txt")) as fh:
        check("target still saved correctly", fh.read(), "Xdata\n")

    print("\nsave: metadata and fallbacks")
    d = tempfile.mkdtemp(dir=tmpdir)
    priv = os.path.join(d, "priv.txt")
    with open(priv, "w") as fh:
        fh.write("secret\n")
    os.chmod(priv, 0o600)
    run([MOLLY, priv], ["X", SAVE])
    check("restrictive mode preserved", os.stat(priv).st_mode & 0o7777, 0o600)

    # A writable file inside a read-only directory has no room for a sibling
    # temp file; molly falls back to an in-place write rather than refusing.
    d = tempfile.mkdtemp(dir=tmpdir)
    rod = os.path.join(d, "rod")
    os.makedirs(rod)
    target = os.path.join(rod, "c.txt")
    with open(target, "w") as fh:
        fh.write("data\n")
    os.chmod(rod, 0o555)
    try:
        run([MOLLY, target], ["X", SAVE])
        with open(target) as fh:
            check("writable file in read-only dir still saves",
                  fh.read(), "Xdata\n")
    finally:
        os.chmod(rod, 0o755)

    print("\nsave: large files round-trip through the write buffer")
    # Save batches rows into a 64 KB buffer. Rows that exactly fill it, spill
    # one byte past it, and dwarf it take three different paths. Kept to a few
    # thousand rows: loading grows the row array one row at a time, which is
    # quadratic under allocators that always move on realloc (ASan's among
    # them), and the suite should stay usable with a sanitizer build.
    rows = ["line %d %s" % (i, "x" * (i % 97)) for i in range(3000)]
    rows[1000] = "a" * 65535
    rows[1001] = "b" * 65536
    rows[1002] = "c" * 200000
    body = "\n".join(rows) + "\n"
    check("rows at and past the buffer size save intact",
          edit(tmpdir, body, ["X", SAVE], name="big.txt") == "X" + body, True)

    print("\nsave: content round-trips")
    check("NUL bytes survive",
          edit(tmpdir, "ab\x00cd\n", [SAVE]), "ab\x00cd\n")
    check("empty buffer saves", edit(tmpdir, "", [SAVE]), "")


# ------------------------------------------------ external file change ----

def test_external_change(tmpdir):
    print("\nsave: warns before overwriting a file changed on disk")
    path = os.path.join(tmpdir, "ext.txt")

    def touched(new_content, delay=0.05):
        def touch():
            time.sleep(delay)
            with open(path, "w") as fh:
                fh.write(new_content)
        return touch

    with open(path, "w") as fh:
        fh.write("orig\n")
    out = run_interleaved([MOLLY, path], ["X"], touched("external\n"),
                           [SAVE, "n", "\r"])
    msgs = status_lines(out)
    check("declining reports the save was aborted",
          any(m.startswith("Save aborted: file changed on disk")
              for m in msgs),
          True)
    with open(path) as fh:
        check("declining leaves the externally-written content on disk",
              fh.read(), "external\n")

    with open(path, "w") as fh:
        fh.write("orig\n")
    out = run_interleaved([MOLLY, path], ["X"], touched("external\n"),
                           [SAVE, "y", "\r"])
    msgs = status_lines(out)
    check("confirming reports bytes written",
          any(m.endswith("bytes written") for m in msgs), True)
    with open(path) as fh:
        check("confirming overwrites with the editor's own content",
              fh.read(), "Xorig\n")

    result = edit(tmpdir, "orig\n", ["X", SAVE], name="ext.txt")
    check("no external change means no warning at all", result, "Xorig\n")


# ------------------------------------------------------------ security ----

def _attacker(stop, link_path, victim):
    while not stop.is_set():
        try:
            os.symlink(victim, link_path)
        except OSError:
            pass


def test_symlink_race(tmpdir):
    print("\nsecurity: the save temp file must not be hijackable")
    # Regression: the temp file had a predictable name and was opened without
    # O_EXCL after an unlink. A local attacker who could create files in the
    # directory could win the race between unlink and open, redirecting the
    # write through a symlink -- an arbitrary file overwrite as the editing
    # user, and the attacker's symlink then got renamed over the real file.
    d = tempfile.mkdtemp(dir=tmpdir)
    doc = os.path.join(d, "doc.txt")
    victim = os.path.join(d, "victim.txt")
    link = os.path.join(d, "doc.txt.tmp")
    with open(doc, "w") as fh:
        fh.write("data\n")
    with open(victim, "w") as fh:
        fh.write("SECRET-ORIGINAL\n")

    stop = multiprocessing.Event()
    procs = [multiprocessing.Process(target=_attacker, args=(stop, link, victim))
             for _ in range(4)]
    for p in procs:
        p.start()
    keys = []
    for _ in range(25):
        keys += ["Z", SAVE, 0.05]
    try:
        run([MOLLY, doc], keys, settle=1.0)
    finally:
        stop.set()
        for p in procs:
            p.join(timeout=2)
            if p.is_alive():
                p.terminate()

    with open(victim) as fh:
        check("attacker cannot redirect the write", fh.read(),
              "SECRET-ORIGINAL\n")
    check("attacker cannot replace the real file with a symlink",
          os.path.islink(doc), False)


# ------------------------------------------------------------- signals ----

def test_signals(tmpdir):
    print("\nsignals: a killed editor hands back a usable terminal")
    # Regression: a fatal signal skips atexit, so closing the terminal window
    # or a plain `kill` left the terminal in raw mode -- no echo, no line
    # editing -- until the user blindly typed `reset`.
    path = os.path.join(tmpdir, "sig.txt")
    with open(path, "w") as fh:
        fh.write("data\n")
    for name in ("SIGTERM", "SIGHUP", "SIGINT"):
        sig = getattr(signal, name)
        before, during, after, st, out = run_until_signal([MOLLY, path], sig)
        check(name + ": the editor had switched the terminal to raw mode",
              during != before, True)
        check(name + ": terminal modes restored", after, before)
        check(name + ": still exits by the signal",
              os.WIFSIGNALED(st) and os.WTERMSIG(st) == sig, True)
        # And mouse reporting switched off. Terminal modes are only half of what
        # the user is left with: a terminal still reporting clicks types an escape
        # sequence at the shell prompt every time they click in that window.
        check(name + ": mouse reporting switched off",
              b"\x1b[?1002l" in out and b"\x1b[?1006l" in out, True)


# ---------------------------------------------------------------- utf-8 ----

def test_utf8(tmpdir):
    print("\nutf-8: editing steps over whole characters")
    cafe = "café\n".encode("utf-8")  # é is two bytes
    # Regression: backspace/delete used to remove a single byte, leaving an
    # invalid UTF-8 sequence behind (café -> "caf\xc3").
    check("backspace removes a whole multibyte char",
          edit_bytes(tmpdir, cafe, [END, "\x7f", SAVE]), b"caf\n")
    check("forward delete removes a whole multibyte char",
          edit_bytes(tmpdir, cafe, [HOME, RIGHT, RIGHT, RIGHT, "\x1b[3~", SAVE]),
          b"caf\n")
    check("right arrow crosses a multibyte char in one step",
          edit_bytes(tmpdir, "aéb\n".encode(), [HOME, RIGHT, RIGHT, "X", SAVE]),
          "aéXb\n".encode())
    check("undo restores a deleted multibyte char",
          edit_bytes(tmpdir, cafe, [END, "\x7f", "\x1a", SAVE]), cafe)

    path = os.path.join(tmpdir, "col.txt")
    with open(path, "wb") as fh:
        fh.write(cafe)
    # café is four columns but five bytes; the status bar's Col reports the
    # display column (5 at end of line), never the byte position (6).
    out = run([MOLLY, path], [END])
    check("column reflects display position, not bytes",
          b"Col 5" in out and b"Col 6" not in out, True)
    found = (status_lines(run([MOLLY, path], [HOME, "\x06", "é", "\r"]))
             or [""])[-1]
    check("a multibyte search term is accepted and found",
          found.startswith("Found"), True)


# ------------------------------------------------- wide-char rendering ----

def test_wide(tmpdir):
    print("\nwide chars: the cursor column tracks display width, not bytes")

    def cursor_after(body, keys):
        path = os.path.join(tmpdir, "w.txt")
        with open(path, "wb") as fh:
            fh.write(body.encode("utf-8"))
        return cursor_col(run([MOLLY, path], keys))

    # A wide glyph (U+3042, HIRAGANA A) occupies two columns but three bytes.
    # End of "aあb" is at display column 4, so the cursor sits at screen col 5
    # -- byte counting would wrongly place it at 6.
    check("wide glyph counts as two columns",
          cursor_after("aあb\n", [END]), 5)
    # A combining mark (U+0301) adds no column: "e" + acute is one column wide.
    check("combining mark counts as zero columns",
          cursor_after("é\n", [END]), 2)
    # Stepping onto a wide glyph lands the cursor past its full width.
    check("cursor steps across a wide glyph by two columns",
          cursor_after("あい\n", [HOME, RIGHT]), 3)
    # In a narrow window full of wide glyphs the cursor never runs off the
    # right edge (a split glyph is dropped, not half-drawn).
    col = cursor_after("あ" * 40 + "\n", [END], )
    check("cursor stays within a narrow window of wide glyphs",
          col is not None and col <= 80, True)


def test_status_col(tmpdir):
    print("\nstatus bar: Col reports the character column, not the display "
          "column")

    def status_col(body, keys):
        path = os.path.join(tmpdir, "sc.txt")
        with open(path, "wb") as fh:
            fh.write(body.encode("utf-8"))
        text = run([MOLLY, path], keys).decode("utf-8", "replace")
        m = re.findall(r"Ln \d+, Col (\d+)", text)
        return int(m[-1]) if m else None

    # Col is the 1-based index of the Nth character in the line -- a tab or a
    # double-width glyph is one character, regardless of how many terminal
    # columns it occupies on screen. It intentionally does NOT match the
    # cursor's on-screen column on a line with a tab or a wide glyph (that
    # number is tracked separately, internally, as the render column).
    check("plain ASCII end of line", status_col("hello\n", [END]), 6)
    check("a leading tab is one character",
          status_col("\thello\n", [END]), 7)
    check("a tab mid-line is one character",
          status_col("a\tb\n", [END]), 4)
    check("a wide glyph is one character",
          status_col("aあb\n", [END]), 4)
    check("home is always column 1", status_col("\thello\n", [END, HOME]), 1)


# ---------------------------------------------------------- selection ----

def test_selection(tmpdir):
    print("\nselection: Shift+arrow extends a selection (status-bar byte count)")

    def sel(body, keys, name="sel.txt"):
        path = os.path.join(tmpdir, name)
        with open(path, "wb") as fh:
            fh.write(body.encode("utf-8"))
        text = run([MOLLY, path], keys).decode("utf-8", "replace")
        # The status bar is the reverse-video line at row `rows-1`; take its
        # last frame so earlier frames' "Sel" text cannot mask a clear.
        ms = re.findall(r"\x1b\[%d;1H\x1b\[7m([^\x1b]*)" % 23, text)
        if not ms:
            return None
        m = re.search(r"Sel (\d+)", ms[-1])
        return int(m.group(1)) if m else None

    # Extending right counts bytes to the right of the anchor.
    check("shift-right x2 selects 2", sel("abcd\n", [SH_RIGHT, SH_RIGHT]), 2)
    check("shift-right x3 selects 3",
          sel("abcd\n", [SH_RIGHT, SH_RIGHT, SH_RIGHT]), 3)
    check("shift-end selects to end of line", sel("abcd\n", [RIGHT, SH_END]), 3)
    check("shift-left selects backwards",
          sel("abcd\n", [END, SH_LEFT, SH_LEFT]), 2)
    # Crossing one line break adds one byte for that break.
    check("shift-down spans the first row plus a break", sel("ab\ncd\n", [SH_DOWN]), 3)

    # A plain move, or an edit, drops the selection (no "Sel" in the bar).
    check("a plain arrow clears the selection",
          sel("abcd\n", [SH_RIGHT, SH_RIGHT, RIGHT]), None)
    check("typing clears the selection", sel("abcd\n", [SH_RIGHT, "X"]), None)

    # Shifted keys must never type into the buffer nor corrupt it.
    check("shifted keys type nothing",
          edit(tmpdir, "abcd\n", [SH_RIGHT, SH_RIGHT, SH_LEFT, SAVE]), "abcd\n")


# -------------------------------------------------------- clipboard ----

def test_clipboard(tmpdir):
    print("\nclipboard: Ctrl-C/X/V copy, cut and paste the selection")
    sel_ab = [SH_RIGHT, SH_RIGHT]      # select "ab" from column 0

    check("copy then paste", edit(tmpdir, "abcd\n", sel_ab + [COPY, END, PASTE, SAVE]),
          "abcdab\n")
    check("cut removes the selection",
          edit(tmpdir, "abcd\n", sel_ab + [CUT, SAVE]), "cd\n")
    check("cut then paste at another spot",
          edit(tmpdir, "abcd\nef\n", sel_ab + [CUT, DOWN, END, PASTE, SAVE]),
          "cd\nefab\n")
    check("multi-line copy and paste",
          edit(tmpdir, "ab\ncd\n", [SH_DOWN, SH_RIGHT, SH_RIGHT, COPY, DOWN, PASTE, SAVE]),
          "ab\ncd\nab\ncd\n")

    # Editing keys consume the selection (replace-on-type).
    check("typing replaces selection",
          edit(tmpdir, "hello\n", [SH_RIGHT] * 5 + ["X", SAVE]), "X\n")
    check("backspace deletes selection",
          edit(tmpdir, "hello\n", [SH_RIGHT] * 5 + ["\x1b[3~", SAVE]), "\n")
    check("enter replaces selection",
          edit(tmpdir, "hello\n", [SH_RIGHT] * 5 + ["\r", SAVE]), "\n\n")

    # Copy keeps the selection alive, so the next edit still replaces it.
    check("copy keeps the selection",
          edit(tmpdir, "abcd\n", sel_ab + [COPY, "Z", SAVE]), "Zcd\n")

    # Each is a single undo step.
    check("cut undoes as one step",
          edit(tmpdir, "hello world\n", [SH_RIGHT] * 5 + [CUT, UNDO, SAVE]),
          "hello world\n")
    check("paste undoes as one step",
          edit(tmpdir, "ab\n", sel_ab + [COPY, END, PASTE, UNDO, SAVE]), "ab\n")

    # Pasting with an empty clipboard is a no-op, not a crash.
    check("paste of empty clipboard", edit(tmpdir, "hello\n", [PASTE, SAVE]),
          "hello\n")


# ------------------------------------------------- selection highlight ----

def test_selection_highlight(tmpdir):
    print("\nselection highlight: selected text is drawn in reverse video")

    def raw(body, keys, name="hl.txt"):
        path = os.path.join(tmpdir, name)
        with open(path, "wb") as fh:
            fh.write(body.encode("utf-8"))
        return run([MOLLY, path], keys).decode("utf-8", "replace")

    # The status bar paints in reverse video too, but resets with ESC[m, so
    # ESC[27m -- which only the row highlight emits -- uniquely marks selected
    # buffer text. Selecting "abc" must wrap exactly those glyphs.
    sel = raw("abcd\n", [SH_RIGHT, SH_RIGHT, SH_RIGHT])
    check("selected glyphs are highlighted", "\x1b[7mabc\x1b[27m" in sel, True)
    check("highlight ends with a reverse-video reset", "\x1b[27m" in sel, True)
    check("unselected tail stays outside the highlight",
          "\x1b[7mabc\x1b[27md" in sel, True)

    # With no selection there is no reverse-video reset anywhere in the stream.
    plain = raw("abcd\n", ["\x1b[15~"])  # F5 = full repaint (Ctrl-L selects a line now)
    check("no highlight without a selection", "\x1b[27m" in plain, False)

    # A multi-line selection highlights the top row's tail and the next row's
    # head as two independent reverse-video runs.
    multi = raw("abcd\nefgh\n", [SH_RIGHT, SH_DOWN, SH_RIGHT])
    check("multi-line: top row tail highlighted",
          "\x1b[7mabcd\x1b[27m" in multi, True)
    check("multi-line: next row head highlighted",
          "\x1b[7mef\x1b[27mgh" in multi, True)


# ------------------------------------------------------- osc 52 clipboard ----

def test_osc52_clipboard(tmpdir):
    print("\nosc52: copy/cut also set the system clipboard; paste is literal")

    def raw(body, keys, name="osc.txt"):
        path = os.path.join(tmpdir, name)
        with open(path, "wb") as fh:
            fh.write(body.encode("utf-8"))
        return run([MOLLY, path], keys).decode("latin-1")

    # OSC 52 wraps the base64 of the clipboard: ESC ] 52 ; c ; <b64> BEL.
    # base64("ab") == "YWI=". Selecting the first two chars and copying must
    # push exactly that, so a terminal-native paste yields what was copied.
    o = raw("abcd\n", [SH_RIGHT, SH_RIGHT, COPY])
    check("copy emits an OSC 52 clipboard set", "\x1b]52;c;YWI=\x07" in o, True)

    # Cut pushes it too (and still deletes the selection).
    oc = raw("abcd\n", [SH_RIGHT, SH_RIGHT, CUT])
    check("cut emits an OSC 52 clipboard set", "\x1b]52;c;YWI=\x07" in oc, True)

    # A copy must not touch the buffer, and copy+paste still round-trips.
    check("copy leaves the buffer intact",
          edit(tmpdir, "abcd\n", [SH_RIGHT, SH_RIGHT, COPY, SAVE]), "abcd\n")
    check("copy then paste is unaffected",
          edit(tmpdir, "abcd\n", [SH_RIGHT, SH_RIGHT, COPY, END, PASTE, SAVE]),
          "abcdab\n")

    # Bracketed paste is disabled at startup, so terminal-pasted text arrives as
    # plain keystrokes rather than ESC[200~...~ wrappers Molly can't parse.
    check("bracketed paste disabled at startup",
          "\x1b[?2004l" in raw("x\n", []), True)


# ------------------------------------------------------------- gutter ----

LINENUM = "\x15"  # Ctrl-U toggles the line-number gutter

# The gutter prefix as bytes: SGR-dimmed number, a space, the U+2502 bar and
# a reset. Output is decoded latin-1, so the bar is its three raw UTF-8 bytes.
GRAY, SEP, RST = "\x1b[90m", "\xe2\x94\x82", "\x1b[m"

# The screen row the buffer's first line is drawn on, one-based. The menu bar takes
# row 1, so the text starts at row 2. Named once rather than written out in every
# assertion, because the chrome above the text area is exactly the sort of thing that
# changes again.
TEXT_ROW = 2


def at_row(n):
    """The cursor-position escape for the nth line of the buffer, one-based."""
    return "\x1b[%d;1H" % (TEXT_ROW + n - 1)


def test_gutter(tmpdir):
    print("\ngutter: Ctrl-U shows a right-aligned line-number gutter behind a "
          "gray separator and shifts the text area right by its width")

    def raw(body, keys, name="g.txt", **kw):
        path = os.path.join(tmpdir, name)
        with open(path, "wb") as fh:
            fh.write(body.encode("utf-8"))
        return run([MOLLY, path], keys, **kw).decode("latin-1")

    # Default off: text starts at screen column 1, no leading number.
    off = raw("alpha\nbeta\ngamma\n", [])
    check("off by default (the text starts at column 1)",
          at_row(1) + "alpha" in off, True)
    check("off by default (no gutter number)", at_row(1) + "1 " in off, False)
    check("off by default (no gutter separator)", SEP in off, False)

    # The below-EOF tilde is tinted gray even with the gutter off, matching
    # the tilde drawn inside the gutter.
    off_t = raw("one\n", [])
    check("tilde is gray in no-gutter mode",
          at_row(2) + GRAY + "~" + RST in off_t, True)

    # Toggle on: each row is prefixed by the line number right-aligned to the
    # digits of the largest line number, then " │ ": "1 │ alpha". The whole
    # gutter is gray and reset before the text.
    on = raw("alpha\nbeta\ngamma\n", [LINENUM])
    pre = GRAY + "%d " + SEP + " " + RST
    check("row 1 numbered", at_row(1) + (pre % 1) + "alpha" in on, True)
    check("row 2 numbered", at_row(2) + (pre % 2) + "beta" in on, True)
    check("row 3 numbered", at_row(3) + (pre % 3) + "gamma" in on, True)

    # Toggling again reverts: the final paint has text at column 1 again.
    off2 = raw("alpha\nbeta\ngamma\n", [LINENUM, LINENUM])
    check("toggle back to off", at_row(1) + "alpha" in off2, True)

    # Two-digit line numbers widen the field and stay right-aligned (numrows
    # 12 -> a 2-digit field). Line 3 is padded with a leading space; line 10 is
    # not.
    body12 = "".join("A%02d\n" % i for i in range(1, 13))
    two = raw(body12, [LINENUM])
    check("single digit right-aligned in 2-wide field",
          at_row(3) + GRAY + " 3 " + SEP + " " + RST + "A03" in two, True)
    check("double digit fills the field",
          at_row(10) + GRAY + "10 " + SEP + " " + RST + "A10" in two, True)

    # The separator continues through the blank rows below EOF: a gray blank
    # field, the bar and the tilde, then reset.
    tilde = raw("one\n", [LINENUM])
    check("separator continues through below-EOF rows",
          at_row(2) + GRAY + "  " + SEP + " ~" + RST in tilde, True)

    # ...and through the welcome screen: the separator row carries the tilde
    # and the centered banner follows in the text area (row 24 - 2 status rows,
    # screenrows/3 + 1 = 8).
    wel = raw("", [LINENUM])
    welrow = wel[wel.rfind("\x1b[8;1H") + len("\x1b[8;1H"):]
    check("separator continues through the welcome screen",
          welrow.startswith(GRAY + "  " + SEP + " ~" + RST), True)
    check("welcome banner follows the separator", "Molly editor" in welrow, True)

    # The cursor is pushed right by exactly the gutter width (digit + " │ ").
    a = cursor_col(run([MOLLY, _gpath(tmpdir, "hello\n", "c1.txt")], [END]))
    b = cursor_col(run([MOLLY, _gpath(tmpdir, "hello\n", "c2.txt")],
                       [LINENUM, END]))
    check("cursor shifted by gutter width", b - a, 4)

    # Wide glyphs still count as two columns *plus* the gutter offset, so the
    # cursor lands past the full display width and the gutter (not the byte
    # count) -- the two width adjustments compose.
    w = cursor_col(run([MOLLY, _gpath(tmpdir, "\u3042\u3042\n", "c3.txt")],
                       [LINENUM, END]))
    check("gutter + wide-glyph width compose on the cursor", w, 9)

    # A selection highlight composes with the gutter: the gray number and its
    # separator precede the reverse-video run, which is emitted from the text
    # columns exactly as when the gutter is off.
    sel = raw("abcd\n", [LINENUM, SH_RIGHT, SH_RIGHT])
    check("gutter and selection highlight coexist",
          at_row(1) + (pre % 1) + "\x1b[7mab\x1b[27mcd" in sel, True)


def _gpath(tmpdir, body, name):
    path = os.path.join(tmpdir, name)
    with open(path, "wb") as fh:
        fh.write(body.encode("utf-8"))
    return path


# --------------------------------------------------------------- help ----

HELP = "\x1f"  # Ctrl-?

def test_help(tmpdir):
    print("\nhelp: documents every keybind; two columns wide, one narrow")
    path = os.path.join(tmpdir, "help.txt")
    with open(path, "w") as fh:
        fh.write("x\n")

    def out(cols, rows=40, keys=(HELP,)):
        return run([MOLLY, path], list(keys), rows=rows, cols=cols).decode("utf-8", "replace")

    wide = out(cols=80)
    check("help has a title", "Keyboard Reference" in wide, True)
    # Every keybinding and group heading must be documented somewhere.
    #
    # The chords listed are the ones the help screen *prints*, which is the first
    # binding of each command -- so F1 rather than Ctrl-?, and F3 / Shift-F3
    # rather than Ctrl-N / Ctrl-P. Those three still work; they are aliases, and
    # a key reference that showed every alias would be twice as long for no
    # extra information.
    for token in ["FILES", "MOVEMENT", "SELECTION", "EDITING",
                  "FIND & REPLACE", "CLIPBOARD", "HISTORY", "VIEW",
                  "Ctrl-S", "Ctrl-Q", "F1", "Ctrl-G", "Ctrl-F", "F3",
                  "Shift-F3", "Ctrl-T", "Ctrl-R", "Ctrl-Z", "Ctrl-Y", "Ctrl-C",
                  "Ctrl-X", "Ctrl-V", "Ctrl-A", "Ctrl-L", "F5", "Ctrl-U",
                  "Shift+Arrows", "PgUp/PgDn",
                  "Home/End", "Bksp/Ctrl-H", "Enter accepts", "Esc cancels"]:
        check("help lists %s" % token, token in wide, True)

    # Wide screens use the two-column layout; narrow ones fall back to one.
    check("wide uses two columns", "FILES" in wide and "CLIPBOARD" in wide, True)
    narrow = out(cols=48)
    check("narrow starts at the top", "FILES" in narrow, True)
    # One column means more lines than fit, so the tail is scrolled to rather than
    # shown. What matters is that nothing is dropped: End reaches the last group.
    # (This used to fit without scrolling, before help gained a frame.)
    narrow_end = out(cols=48, keys=(HELP, END))
    check("narrow drops nothing -- the end is reachable",
          "Ctrl-V" in narrow_end and "CLIPBOARD" in narrow_end, True)

    # A short screen gains a scroll hint (content taller than the screen).
    short = out(cols=80, rows=8)
    check("short screen offers scrolling", "scroll" in short, True)

    # Two-column means a single physical row carries a left and a right group
    # title together; one-column never does. help_row composes each row into one
    # write, so splitting the stream at every row-set cursor move isolates one
    # physical row per fragment.
    # Two-column means one physical row carries two group titles; one-column never
    # does. Which two is not the point and changes whenever a group is added, so any
    # pair counts.
    GROUPS = ["FILES", "MENUS", "MOVEMENT", "SELECTION", "EDITING",
              "FIND & REPLACE", "CLIPBOARD", "HISTORY", "VIEW"]

    def two_col(text):
        for seg in re.split(r"\x1b\[\d+;\d+H", text):
            if sum(1 for g in GROUPS if g in seg) >= 2:
                return True
        return False

    check("wide help is two columns", two_col(wide), True)
    check("narrow help is one column", two_col(narrow), False)

    # Resizing while help is open must reflow it live, not keep the size it
    # opened at. Open wide, then shrink below the two-column threshold; the last
    # repaint (taken after the final full-screen clear) is the post-resize one.
    live = run([MOLLY, path], [HELP, (24, 40)],
               rows=24, cols=100).decode("utf-8", "replace")
    check("live resize: two columns while wide", two_col(live), True)
    last = live.rsplit("\x1b[2J", 1)[-1]
    check("live resize: still help after the resize",
          "Keyboard Reference" in last, True)
    check("live resize: reflowed to one column when narrow",
          two_col(last), False)
    check("live resize: content survives the reflow", "FILES" in last, True)


# ---------------------------------------------------------- file format ----

def test_line_endings(tmpdir):
    print("\nfile format: line endings and final newline are preserved")
    # Regression: CRLF was rewritten to LF, and a missing final newline was
    # added, so a Windows-origin file showed as entirely modified in git.
    check("a CRLF file stays CRLF",
          edit_bytes(tmpdir, b"one\r\ntwo\r\n", [SAVE]), b"one\r\ntwo\r\n")
    check("editing a CRLF file keeps CRLF",
          edit_bytes(tmpdir, b"one\r\ntwo\r\n", ["X", SAVE]),
          b"Xone\r\ntwo\r\n")
    check("a missing final newline stays missing",
          edit_bytes(tmpdir, b"no newline", [SAVE]), b"no newline")
    check("an LF file stays LF", edit_bytes(tmpdir, b"a\nb\n", [SAVE]),
          b"a\nb\n")

    work = tempfile.mkdtemp(dir=tmpdir)
    cwd = os.getcwd()
    os.chdir(work)
    try:
        run([MOLLY, "new.txt"], ["hi", SAVE])
        with open(os.path.join(work, "new.txt"), "rb") as fh:
            check("a brand-new file is saved with LF and a trailing newline",
                  fh.read(), b"hi\n")
    finally:
        os.chdir(cwd)


# ------------------------------------------------------------ recovery ----

def test_recovery(tmpdir):
    print("\nrecovery: a signal with unsaved changes writes a recovery file")
    path = os.path.join(tmpdir, "doc.txt")
    with open(path, "w") as fh:
        fh.write("original\n")
    rec = path + ".molly-recover"

    # Regression/feature: work used to vanish when a signal killed the editor.
    run_until_signal([MOLLY, path], signal.SIGTERM, keys=["HELLO"])
    check("recovery file is created", os.path.exists(rec), True)
    if os.path.exists(rec):
        with open(rec) as fh:
            check("recovery file holds the unsaved buffer",
                  fh.read(), "HELLOoriginal\n")
        os.remove(rec)

    with open(rec, "w") as fh:
        fh.write("stale\n")
    run([MOLLY, path], ["X", SAVE])
    check("a successful save clears the recovery file",
          os.path.exists(rec), False)

    run_until_signal([MOLLY, path], signal.SIGTERM)
    check("no recovery file when nothing is unsaved",
          os.path.exists(rec), False)


def test_recovery_prompt(tmpdir):
    print("\nrecovery: startup offers to restore data left by a crash")
    path = os.path.join(tmpdir, "doc.txt")
    rec = path + ".molly-recover"

    def setup():
        with open(path, "w") as fh:
            fh.write("on disk\n")
        with open(rec, "w") as fh:
            fh.write("rescued\n")

    setup()
    out, _ = run([MOLLY, path], [], status=True)
    check("a leftover recovery file triggers a restore prompt",
          any("R = restore" in m for m in status_lines(out)), True)
    check("the recovered data is visible to no keystroke yet",
          b"rescued" in out, False)

    os.remove(rec)
    out, _ = run([MOLLY, path], [], status=True)
    check("no prompt when there is no recovery file",
          any("restore" in m for m in status_lines(out)), False)
    setup()

    # Any other key continues with the on-disk file; the recovery file then
    # disappears on quit exactly as it always did.
    out = run([MOLLY, path], [DOWN, "\x11"])
    check("dismissing the prompt keeps the on-disk buffer",
          b"on disk" in out, True)
    check("a dismissed recovery file is removed on quit",
          os.path.exists(rec), False)

    setup()
    out = run([MOLLY, path], ["r", SAVE, "\x11"])
    with open(path) as fh:
        body = fh.read()
    check("R loads the recovered data and Ctrl-S saves it",
          body, "rescued\n")
    check("saving after a restore removes the recovery file",
          os.path.exists(rec), False)
    check("the restore announces itself in the status bar",
          any("Restored unsaved changes" in m for m in status_lines(out)),
          True)

    setup()
    out = run([MOLLY, path], ["r", "\x11"])
    check("a restored buffer counts as unsaved work (quit warns)",
          any("unsaved" in m.lower() for m in status_lines(out)), True)

    setup()
    run([MOLLY, path], ["r", "\x11", "\x11", "\x11", "\x11"])
    check("quitting a restored buffer without saving cleans up anyway",
          os.path.exists(rec), False)

    # The on-disk file having vanished since the crash must not stop the
    # restore: the recovery data is all that is left of it.
    os.remove(path)
    with open(rec, "w") as fh:
        fh.write("rescued\n")
    run([MOLLY, path], ["r", SAVE, "\x11"])
    with open(path) as fh:
        body = fh.read()
    check("restore recreates a file that no longer exists",
          body, "rescued\n")


# ------------------------------------------------------------ command ----

def test_cli_args(tmpdir):
    print("\ncommand line: --version, +line, and ignored extra arguments")
    path = os.path.join(tmpdir, "five.txt")
    with open(path, "w") as fh:
        fh.write("one\ntwo\nthree\nfour\nfive\n")

    def cursor_row(out):
        """The buffer line the cursor ended on, one-based.

        Converted from the screen row, so these tests say which line of the file
        the cursor is on rather than which row of the terminal it landed on.
        """
        text = out.decode("latin-1")
        m = re.findall(r"\x1b\[(\d+);(\d+)H", text)
        return int(m[-1][0]) - (TEXT_ROW - 1) if m else None

    out, st = run([MOLLY, "--version"], [], status=True)
    check("--version prints the version and exits cleanly",
          bool(re.match(r"^molly \d", out.strip().decode())), True)
    check("--version exits with status 0", os.WIFEXITED(st)
          and os.WEXITSTATUS(st) == 0, True)
    out, _ = run([MOLLY, "-v"], [], status=True)
    check("-v is the short form", out.strip().decode().startswith("molly "),
          True)

    check("+N opens with the cursor on that line",
          cursor_row(run([MOLLY, path, "+3"], [])), 3)
    check("+N also works before the file name",
          cursor_row(run([MOLLY, "+2", path], [])), 2)
    check("+1 lands on the first line",
          cursor_row(run([MOLLY, path, "+1"], [])), 1)
    check("a line past the end clamps to the last row",
          cursor_row(run([MOLLY, path, "+999"], [])), 5)
    check("a high line scrolls the last row into view",
          b"five" in run([MOLLY, path, "+999"], []), True)
    # "+x" is not a line request: as the file it opens, as an extra it warns.
    out, _ = run([MOLLY, path, "+x"], [], status=True)
    check("+x is not swallowed as a line request",
          any("+x" in m for m in status_lines(out)), True)

    # An argument beyond the file is reported rather than silently dropped.
    extra = os.path.join(tmpdir, "other.txt")
    out, _ = run([MOLLY, path, extra], [], status=True)
    msgs = status_lines(out)
    check("an extra argument warns on the status bar",
          any("extra argument" in m for m in msgs), True)
    check("the warning names the ignored argument",
          any("other.txt" in m for m in msgs), True)
    out, _ = run([MOLLY, path, extra, extra, extra], [], status=True)
    check("several extras are counted",
          any("3 extra arguments" in m for m in status_lines(out)), True)

    # With no file at all, +N just starts an empty buffer at the top.
    out, _ = run([MOLLY, "+50"], [], status=True)
    check("+N with no file opens an empty buffer without complaint",
           any("Warning" in m for m in status_lines(out)), False)


# ---------------------------------------------------------------- open ----

def test_open_dialog(tmpdir):
    print("\nopen dialog: Ctrl-O browses from the working directory")
    OPEN = "\x0f"
    cwd = os.getcwd()
    # A private directory: the browser lists its cwd, so leftover files from
    # other tests in the shared tmpdir would shift the list positions.
    d = tempfile.mkdtemp(dir=tmpdir)
    try:
        os.chdir(d)
        os.mkdir(os.path.join(d, "sub"))
        with open(os.path.join(d, "sub", "inner.txt"), "w") as fh:
            fh.write("inner body\n")
        alpha = os.path.join(d, "alpha.txt")
        zeta = os.path.join(d, "zeta.txt")
        with open(alpha, "w") as fh:
            fh.write("alpha body\n")
        with open(zeta, "w") as fh:
            fh.write("zeta body\n")

        # List order: ".." first, then the directory, then files.
        out = run([MOLLY], [OPEN])
        check("Ctrl-O opens the file browser", b"Open File" in out, True)
        check("the browser starts in the working directory",
              d.encode() in out, True)
        check("directories are marked and files listed",
              b"sub/" in out and b"alpha.txt" in out, True)
        check("the directory sorts before the files",
              out.find(b"sub") < out.find(b"alpha.txt"), True)
        check("'..' is offered as the way back up", b"../" in out, True)

        out = run([MOLLY], [OPEN, DOWN, DOWN, "\r"])
        check("Enter on a file opens it", b"alpha body" in out, True)
        check("the status bar reports the opened file",
              any("Opened" in m and "alpha.txt" in m
                  for m in status_lines(out)), True)

        out = run([MOLLY], [OPEN, DOWN, "\r"])
        check("Enter on a directory descends into it",
              b"inner.txt" in out, True)
        out = run([MOLLY], [OPEN, DOWN, "\r", "\r"])
        check("'..' climbs back out", b"alpha.txt" in out, True)
        out = run([MOLLY], [OPEN, DOWN, "\r", DOWN, "\r"])
        check("a file two levels deep opens", b"inner body" in out, True)

        out = run([MOLLY, zeta], [OPEN, "\x1b"])
        check("Esc closes the dialog without opening anything",
              any("Opened" in m for m in status_lines(out)), False)
        check("the original buffer survives the cancel",
              b"zeta body" in out, True)

        out = run([MOLLY, zeta], ["X", OPEN])
        check("unsaved changes refuse the dialog outright",
              any("Open aborted" in m and "unsaved" in m
                  for m in status_lines(out)), True)
        check("and the browser never opens", b"Open File" in out, False)

        # After the switch, writes and history belong to the new file.
        run([MOLLY, alpha], [OPEN, DOWN, DOWN, DOWN, "\r", "!", SAVE,
                            "\x11"])
        with open(alpha) as fh:
            check("the switch leaves the old file alone",
                  fh.read(), "alpha body\n")
        with open(zeta) as fh:
            check("the new file takes the edit", fh.read(), "!zeta body\n")

        # A pristine target, so the expected undo result is unambiguous.
        beta = os.path.join(d, "beta.txt")
        with open(beta, "w") as fh:
            fh.write("beta body\n")
        # beta is list index 3: "..", sub/, alpha.txt, beta.txt.
        run([MOLLY, alpha], [OPEN, DOWN, DOWN, DOWN, "\r", "!", UNDO, SAVE,
                            "\x11"])
        with open(beta) as fh:
            check("undo does not cross the switch back to the old buffer",
                  fh.read(), "beta body\n")
    finally:
        os.chdir(cwd)


# --------------------------------------------------------- robustness ----

def test_directory_refused(tmpdir):
    print("\nrobustness: a directory is refused, not opened as a buffer")
    d = tempfile.mkdtemp(dir=tmpdir)
    _, st = run([MOLLY, d], [SAVE, "\x11\x11\x11\x11"], status=True)
    check("opening a directory exits with an error",
          os.WIFEXITED(st) and os.WEXITSTATUS(st) == 1, True)


# ---------------------------------------------------------------- undo ----

def test_undo(tmpdir):
    print("\nundo: a buffer undone to the start matches the original")
    undo = "\x1a"
    cases = [
        ("typed run undoes as one step", "hello world\n", ["abc"], 1),
        ("enter at column 0", "hello world\n", ["\r"], 1),
        ("enter mid-line", "hello world\n", [RIGHT * 3, "\r"], 1),
        ("backspace joining lines", "one\ntwo\n", [DOWN, "\x7f"], 1),
        ("delete joining lines", "one\ntwo\n", [END, "\x1b[3~"], 1),
        ("backspace run", "hello\n", [END, "\x7f\x7f\x7f"], 1),
        ("mixed edits", "abc\ndef\n", ["x", "\r", "y", DOWN, "z"], 5),
    ]
    for label, original, keys, undos in cases:
        check(label, edit(tmpdir, original, keys + [undo] * undos + [SAVE]),
              original)


def test_undo_cap(tmpdir):
    print("\nundo: history evicts its oldest steps past the cap")
    path = os.path.join(tmpdir, "cap.txt")
    once = [REPLACE, "x", "\r", "Q", "\r", "n", "\r"]

    def play(keys, env=None):
        with open(path, "wb") as fh:
            fh.write(b"x x x\n")
        run([MOLLY, path], keys, env=env)
        with open(path, "rb") as fh:
            return fh.read()

    # Each replace-next stores two entries, so MOLLY_UNDO_STEPS=2 keeps only
    # the last replacement. Reaching past the cap reports exhaustion instead
    # of undoing, and everything the cap evicted stays applied.
    capped = {"MOLLY_UNDO_STEPS": "2"}
    check("capped history undoes only the kept steps",
          play(once * 3 + [UNDO, UNDO, SAVE], capped), b"Q Q x\n")
    check("undoing past the cap reports nothing left",
          play(once * 3 + [UNDO, UNDO, UNDO, SAVE], capped), b"Q Q x\n")
    check("uncapped, the same session undoes every step",
          play(once * 3 + [UNDO, UNDO, UNDO, SAVE]), b"x x x\n")


def test_input_decoding(tmpdir):
    print("\ninput: Esc, Alt and function keys")

    # Alt-x arrives as ESC then 'x'. Nothing is bound to it yet, so the test is
    # that it stays inert: the letter must not reach the buffer, which is also
    # what olly did with the byte after an Esc.
    check("alt-letter inserts nothing",
          edit(tmpdir, "ab\n", ["\x1bx", SAVE]), "ab\n")
    check("alt-shift-letter inserts nothing",
          edit(tmpdir, "ab\n", ["\x1bX", SAVE]), "ab\n")
    check("alt-digit inserts nothing",
          edit(tmpdir, "ab\n", ["\x1b7", SAVE]), "ab\n")

    # Esc on its own, and Esc twice, are still just Esc.
    check("bare esc inserts nothing",
          edit(tmpdir, "ab\n", ["\x1b", 0.3, SAVE]), "ab\n")
    check("esc esc inserts nothing",
          edit(tmpdir, "ab\n", ["\x1b\x1b", SAVE]), "ab\n")

    # Function keys are recognized and deliberately unbound; recognized matters
    # because an unparsed sequence would leak its tail into the buffer.
    for label, seq in [("F1 (SS3)", "\x1bOP"),
                       ("F4 (SS3)", "\x1bOS"),
                       ("F5 (CSI)", "\x1b[15~"),
                       ("F10 (CSI)", "\x1b[21~"),
                       ("F12 (CSI)", "\x1b[24~"),
                       ("F1 (CSI final)", "\x1b[P")]:
        check("%s inserts nothing" % label,
              edit(tmpdir, "ab\n", [seq, SAVE]), "ab\n")

    # A sequence that arrives in pieces, as it does over a slow link. Giving up
    # after one VTIME tick consumed the head and delivered the tail as text, so
    # a split "\x1b[C" used to type a literal "C" into the buffer instead of
    # moving right. The pause here is longer than one tick and shorter than the
    # patient read's budget.
    check("a split escape sequence is still one key",
          edit(tmpdir, "ab\n", ["\x1b[", 0.15, "C", "X", SAVE]), "aXb\n")
    check("a split SS3 sequence is still one key",
          edit(tmpdir, "ab\n", ["\x1bO", 0.15, "C", "X", SAVE]), "aXb\n")

    # A sequence truncated for good must not hang or leak: the editor waits out
    # the budget, reports Esc, and the following keys are ordinary input.
    check("an abandoned sequence degrades to esc, and typing continues",
          edit(tmpdir, "ab\n", ["\x1b[", 0.6, "X", SAVE]), "Xab\n")


def test_mouse(tmpdir):
    print("\nmouse: SGR 1006 reports move the cursor, select, and scroll")

    # SGR mouse reports: ESC [ < button ; col ; row  then M for press/drag or m for
    # release. Columns and rows are 1-based, as the terminal counts them.
    # Rows here are rows *of the buffer*, converted to screen rows, so these tests
    # say what they mean and do not move when the chrome above the text does.
    def srow(row):
        return TEXT_ROW + row - 1

    def press(col, row, button=0):
        return "\x1b[<%d;%d;%dM" % (button, col, srow(row))

    def release(col, row, button=0):
        return "\x1b[<%d;%d;%dm" % (button, col, srow(row))

    def drag(col, row):
        return "\x1b[<%d;%d;%dM" % (32, col, srow(row))   # 32 = motion while held

    def wheel(col, row, down=True):
        return "\x1b[<%d;%d;%dM" % (65 if down else 64, col, srow(row))

    # A click puts the cursor where it landed: column 3 of row 1 is before the "c".
    check("a click positions the cursor",
          edit(tmpdir, "abcd\n", [press(3, 1), release(3, 1), "X", SAVE]),
          "abXcd\n")
    # And on the row it landed on.
    check("a click picks the row too",
          edit(tmpdir, "ab\ncd\n", [press(2, 2), release(2, 2), "X", SAVE]),
          "ab\ncXd\n")
    # Past the end of a line, the cursor goes to the end of that line.
    check("a click past the text goes to end of line",
          edit(tmpdir, "ab\n", [press(40, 1), release(40, 1), "X", SAVE]),
          "abX\n")
    # Below the last line, the cursor lands on the phantom row past the end.
    check("a click below the text stays in the buffer",
          edit(tmpdir, "ab\n", [press(1, 10), release(1, 10), "X", SAVE]),
          "ab\nX\n")

    # Dragging selects, using the same anchor-plus-cursor model Shift+arrows use,
    # so typing over it replaces the selection.
    check("a drag selects, and typing replaces the selection",
          edit(tmpdir, "abcd\n", [press(1, 1), drag(3, 1), release(3, 1), "Z", SAVE]),
          "Zcd\n")
    # A drag leaves a real selection, so Ctrl-X cuts it.
    check("a dragged selection can be cut",
          edit(tmpdir, "abcd\n", [press(1, 1), drag(3, 1), release(3, 1),
                                   "\x18", SAVE]),
          "cd\n")

    # The middle button is not a text gesture and must not move the cursor.
    check("a middle click does not move the cursor",
          edit(tmpdir, "abcd\n", [press(3, 1, button=1), release(3, 1, button=1),
                                   "X", SAVE]),
          "Xabcd\n")

    # With the gutter on, a click in it goes to the start of that line rather than
    # into the line numbers.
    check("a click in the gutter goes to the line start",
          edit(tmpdir, "ab\ncd\n", ["\x15", press(2, 2), release(2, 2), "X", SAVE]),
          "ab\nXcd\n")

    # The wheel scrolls the view without moving the cursor, so nothing is typed
    # anywhere new.
    body = "".join("line%d\n" % i for i in range(1, 60))
    scrolled = run([MOLLY, os.path.join(tmpdir, "w.txt")], [])
    with open(os.path.join(tmpdir, "w.txt"), "w") as fh:
        fh.write(body)
    out = run([MOLLY, os.path.join(tmpdir, "w.txt")],
              [wheel(5, 5)] * 3)
    scr = Screen(24, 80)
    scr.feed(out)
    top = scr.text()[TEXT_ROW - 1]
    check("the wheel scrolls the view", top.startswith("line"), True)
    check("the wheel scrolled past the first line", top != "line1", True)
    # The cursor did not move with it: typing still edits line 1.
    check("the wheel leaves the cursor alone",
          edit(tmpdir, body, [wheel(5, 5), wheel(5, 5), "X", SAVE], name="w2.txt")
          .startswith("Xline1"), True)


def test_menu(tmpdir):
    print("\nmenu bar: built from the command table, reached by key and by mouse")

    path = os.path.join(tmpdir, "m.txt")

    def msg(keys, body="alpha\nbeta\ngamma\n", rows=16, cols=64):
        with open(path, "w") as fh:
            fh.write(body)
        out = run([MOLLY, path], list(keys), rows=rows, cols=cols)
        return (status_lines(out, rows=rows, cols=cols) or [""])[-1]

    def screen(keys, body="alpha\nbeta\ngamma\n", rows=16, cols=64):
        with open(path, "w") as fh:
            fh.write(body)
        scr = Screen(rows, cols)
        scr.feed(run([MOLLY, path], list(keys), rows=rows, cols=cols))
        return scr

    F10 = "\x1b[21~"

    # The band is there by default, with every title on it.
    band = screen([]).text()[0]
    for title in ("File", "Edit", "Search", "View", "Go", "Help"):
        check("the band lists %s" % title, title in band, True)
    # Tools and Options have nothing in them yet and so are not shown, though their
    # Alt letters stay reserved.
    check("Tools is not shown while it is empty", "Tools" in band, False)
    check("Options is not shown while it is empty", "Options" in band, False)

    # F10 opens the first menu; Alt-letter opens a named one.
    check("F10 opens the File menu", "Open" in screen([F10]).text()[2], True)
    check("Alt-s opens the Search menu",
          "Find" in screen(["\x1bs"]).text()[2], True)
    check("Alt-e opens the Edit menu",
          "Undo" in screen(["\x1be"]).text()[2], True)

    # Enter runs the highlighted item; a mnemonic runs its own. The Search menu's
    # first item is Find, which opens a dialog rather than leaving a message, so
    # what proves it ran is the dialog.
    check("Enter runs the highlighted item",
          any("Find what:" in l for l in screen(["\x1bs", "\r"]).text()), True)
    check("a mnemonic runs its item", msg(["\x1bv", "m"]), "Menu bar: off")
    # Arrows move within a menu and between menus. The View menu opens on the command
    # palette, with a separator under it, so one Down lands on Line numbers.
    check("Down then Enter takes the next available item",
          msg(["\x1bv", DOWN, "\r"]), "Line numbers: on")
    check("Down twice takes the one after that",
          msg(["\x1bv", DOWN, DOWN, "\r"]), "Menu bar: off")
    check("Right moves to the next menu",
          msg(["\x1bv", RIGHT, "\r"]).startswith("Go to line"), True)

    # Esc closes without running anything.
    check("Esc closes the menu", msg(["\x1bv", "\x1b", 0.3]).startswith("HELP:"),
          True)

    # A menu choice is the same command as its key, dispatcher flags included: Quit
    # from the File menu counts down exactly as Ctrl-Q does.
    check("Quit from the menu keeps the quit countdown",
          msg(["X", "\x1bf", "q"]).startswith("WARNING!!!"), True)

    # Unavailable items are skipped, not merely greyed: with nothing searched for,
    # Down from Find goes past Find Next and Find Previous to Match Case.
    check("Down skips unavailable items",
          msg(["\x1bs", DOWN, "\r"]).startswith("Search is now"), True)
    # And their mnemonics do nothing.
    check("an unavailable item's mnemonic does nothing",
          msg(["\x1bs", "n", 0.3]).startswith("HELP:"), True)

    # The mouse: a click on a title opens that menu, a click on an item runs it, and
    # a click outside closes. Search's title sits at column 20 of the band.
    check("clicking a title opens its menu",
          "Find" in screen(["\x1b[<0;20;1M", "\x1b[<0;20;1m"]).text()[2], True)
    check("clicking an item runs it",
          any("Find what:" in l
              for l in screen(["\x1b[<0;20;1M", "\x1b[<0;20;1m", 0.2,
                               "\x1b[<0;20;3M", "\x1b[<0;20;3m"]).text()), True)
    check("clicking outside closes the menu",
          msg(["\x1bv", 0.2, "\x1b[<0;2;8M", "\x1b[<0;2;8m",
               0.3]).startswith("HELP:"), True)

    # Hiding the band gives its row back to the text; opening a menu while it is
    # hidden draws the band over the first line instead of reflowing the screen.
    hidden = screen(["\x1bv", "m"])
    check("hidden: the text starts at row 1", hidden.text()[0], "alpha")
    check("hidden: an extra line of the file fits",
          hidden.text()[2], "gamma")
    overlay = screen(["\x1bv", "m", F10])
    check("hidden: opening a menu puts the band back over row 1",
          "File" in overlay.text()[0], True)
    check("hidden: the drop-down is still there",
          "Open" in overlay.text()[2], True)


def test_prompt_field(tmpdir):
    print("\nprompt: an input field on the message line, with a cursor in it")

    path = os.path.join(tmpdir, "pf.txt")

    def line(keys, rows=10, cols=50, body="alpha\nbeta\ngamma\n"):
        with open(path, "w") as fh:
            fh.write(body)
        out = run([MOLLY, path], list(keys), rows=rows, cols=cols)
        scr = Screen(rows, cols)
        scr.feed(out)
        return scr.text()[-1], cursor_col(out)

    DEL = "\x1b[3~"
    # Go to line, because Find is a dialog now and this is about the field on the
    # message row. Any prompt exercises it: the field is the same widget whatever
    # is being asked.
    ASK = GOTO
    LEAD = "Go to line: "

    # The line itself is what molly always drew: the prompt, the answer, the hint.
    text, col = line([ASK, "bet"])
    check("the prompt reads as one line",
          text, LEAD + "bet (ESC to cancel)")
    # And the cursor is in the answer rather than back in the buffer, which is the
    # part that needed the grid to let a layer claim it.
    check("the cursor sits after the answer", col, len(LEAD) + 4)

    # Keys a prompt never had before. Each of these did nothing at all until the
    # field existed, because the old prompt only understood typing and Backspace.
    check("Left moves back in the answer",
          line([ASK, "bet", LEFT, LEFT, "X"])[0].startswith(LEAD + "bXet"), True)
    check("Home goes to the start",
          line([ASK, "bet", HOME, "Z"])[0].startswith(LEAD + "Zbet"), True)
    check("End comes back to the finish",
          line([ASK, "bet", HOME, END, "!"])[0].startswith(LEAD + "bet!"), True)
    check("Delete takes the character at the cursor",
          line([ASK, "bet", HOME, DEL])[0].startswith(LEAD + "et"), True)
    check("Backspace still takes the one before it",
          line([ASK, "bet", "\x7f"])[0].startswith(LEAD + "be"), True)

    # An answer longer than the room for it scrolls, keeping the hint on screen.
    # Before the field it simply ran off the right-hand edge, hint and all.
    text, col = line([ASK] + list("abcdefghijklmnopqrstuvwxyz0123456789"))
    check("a long answer keeps its tail in view",
          text.endswith("0123456789 (ESC to cancel)"), True)
    check("and the hint is still there", "(ESC to cancel" in text, True)

    # Too narrow for both, and the hint goes rather than the answer. It used to be
    # drawn over the prompt and the answer alike, which left the row reading
    # " (ESC to cancel, Ent" with no sign of what was being asked.
    # Seven characters, not eight: twenty columns less "Go to line: " leaves eight,
    # and the field keeps the rightmost one for the cursor.
    check("a narrow terminal keeps the answer and drops the hint",
          line([ASK, "abcdefg"], rows=8, cols=20)[0], LEAD + "abcdefg")
    check("and the answer still scrolls there",
          line([ASK] + list("abcdefghijklmnopqrst"), rows=8, cols=20)[0],
          LEAD + "nopqrst")

    # A multi-byte character arrives as its bytes, one key at a time, and has to
    # reassemble in the field rather than scatter.
    check("a multi-byte character enters whole",
          line([ASK, "café"])[0].startswith(LEAD + "café"), True)

    # And the field is still a prompt: Enter answers, Esc cancels.
    check("Enter answers the prompt",
          line([ASK, "2", "\r"])[0].startswith("Ln 2"), True)
    check("Esc cancels it", line([ASK, "2", "\x1b", 0.3])[0], "")

    # The answer reaches the caller intact after being edited, not just displayed:
    # go-to-line parses what the field holds.
    with open(path, "w") as fh:
        fh.write("one\ntwo\nthree\nfour\nfive\n")
    out = run([MOLLY, path], [GOTO, "x", "5", HOME, DEL, "\r"])
    check("an edited answer is what the caller gets",
          (status_lines(out, rows=24, cols=80) or [""])[-1], "Ln 5")


def test_palette(tmpdir):
    print("\ncommand palette: a fuzzy filter over the whole command table")

    path = os.path.join(tmpdir, "pal.txt")
    PALETTE = "\x00"       # Ctrl-Space arrives as NUL
    ALT_X = "\x1bx"

    def screen(keys, body="alpha\nbeta\ngamma\n", rows=20, cols=72):
        with open(path, "w") as fh:
            fh.write(body)
        out = run([MOLLY, path], list(keys), rows=rows, cols=cols)
        scr = Screen(rows, cols)
        scr.feed(out)
        return scr

    def rows_of(keys, **kw):
        """The list's rows, in order, with the padding and the chord column
        stripped -- what the palette is offering, and in what order."""
        scr = screen(keys, **kw)
        out = []
        for row in scr.text():
            if "║" not in row:
                continue
            inner = row.split("║")[1].strip()
            if inner and not inner.startswith("›") and "choose" not in inner \
                    and "No matching" not in inner and "Not available" not in inner:
                out.append(inner.split("  ")[0].strip())
        return out

    def msg(keys, body="alpha\nbeta\ngamma\n", rows=20, cols=72):
        with open(path, "w") as fh:
            fh.write(body)
        out = run([MOLLY, path], list(keys), rows=rows, cols=cols)
        return (status_lines(out, rows=rows, cols=cols) or [""])[-1]

    # Both chords open it, because not every terminal sends NUL for Ctrl-Space.
    for label, key in (("Ctrl-Space", PALETTE), ("Alt-X", ALT_X)):
        text = "\n".join(screen([key]).text())
        check("%s opens the palette" % label, "Commands" in text, True)

    # Every listed command shows the chord it is bound to, taken from the keymap.
    opened = "\n".join(screen([PALETTE]).text())
    check("a listed command shows its chord", "Save" in opened and "Ctrl-S" in opened,
          True)

    # Unfiltered, the list is the command table's own order rather than a ranking.
    check("the unfiltered list starts where the table does",
          rows_of([PALETTE])[:3], ["Save", "Open…", "Quit"])

    # The filter narrows, and ranks a label match above a name-only one: "sav"
    # matches Save's label and search.prev's name, in that order.
    check("the filter puts the best label match first",
          rows_of([PALETTE, "sav"])[0], "Save")
    check("a word start beats a letter stranded mid-word",
          rows_of([PALETTE, "ln"])[0], "Line numbers")

    # The group is matched too, which is the only way "clip" reaches these three.
    clip = rows_of([PALETTE, "clip"])
    check("the group is searchable", sorted(clip), ["Copy", "Cut", "Paste"])

    # Nothing matching says so rather than showing an empty box.
    check("no match says so",
          "No matching command" in "\n".join(screen([PALETTE, "zzqq"]).text()), True)

    # Enter runs the selection; the arrows move it.
    check("Enter runs the selected command",
          msg([PALETTE, "line numbers", "\r"]), "Line numbers: on")
    check("Down moves the selection",
          msg([PALETTE, "li", DOWN, "\r"]) != "Line numbers: on", True)

    # A command the palette lists but cannot run says why and stays open, rather
    # than being hidden from the list in the first place.
    check("an unavailable command is refused, not hidden",
          "Paste" in "\n".join(screen([PALETTE, "paste"]).text()), True)
    check("choosing it says why",
          "Not available right now"
          in "\n".join(screen([PALETTE, "paste", "\r"]).text()), True)

    # Esc closes it without running anything.
    check("Esc closes the palette",
          msg([PALETTE, "line numbers", "\x1b", 0.3]).startswith("HELP:"), True)

    # A command run from the palette is the same command as from its key,
    # dispatcher flags included: Quit still counts down on a dirty buffer.
    check("Quit from the palette keeps the quit countdown",
          msg(["X", PALETTE, "quit", "\r"]).startswith("WARNING!!!"), True)

    # A click on a row chooses it, as a click on a menu item does.
    scr = screen([PALETTE, "line numbers"])
    row = next(i for i, r in enumerate(scr.text()) if "Line numbers" in r)
    check("clicking a row runs it",
          msg([PALETTE, "line numbers", 0.2,
               "\x1b[<0;20;%dM" % (row + 1), "\x1b[<0;20;%dm" % (row + 1)]),
          "Line numbers: on")

    # The palette lists only what is worth running: the menu openers and the
    # palette itself are mechanism and stay out of it.
    everything = rows_of([PALETTE], rows=40, cols=72)
    check("the menu openers are not listed",
          [r for r in everything if r.endswith("menu")], [])
    check("the palette does not list itself",
          [r for r in everything if r.startswith("Command palette")], [])

    # And it is in the View menu, which is where it is discoverable without
    # knowing the chord. Reached that way it is a modal loop inside a modal loop,
    # which is the shape the roadmap settled on and the first place molly nests
    # three deep: menu, palette, and whatever the command opens.
    check("the View menu offers it",
          "Command palette" in "\n".join(screen(["\x1bv"]).text()), True)
    check("and running it from the menu works",
          msg(["\x1bv", "c", 0.2, "line numbers", "\r"]), "Line numbers: on")
    check("a command that opens a prompt works from the palette",
          msg([PALETTE, "find", "\r", 0.2, "beta", "\r"]), "Found")
    check("so does one that opens a dialog",
          "Open File" in "\n".join(screen([PALETTE, "open", "\r"]).text()), True)


def test_command_table(tmpdir):
    print("\ncommands: the keymap is parsed from chord text")

    env = dict(os.environ, MOLLY_DUMP_KEYMAP="1")
    out = subprocess.run([MOLLY], capture_output=True, text=True, env=env)
    check("the dump exits cleanly", out.returncode, 0)
    lines = [l for l in out.stdout.split("\n") if l.strip()]
    pairs = [tuple(l.split("\t")) for l in lines]
    bound = dict(pairs)

    # Every binding has been through chord_parse on the way in and chord_name on
    # the way out, so what comes back proves the round trip. Spot-check each
    # chord shape the parser accepts: a control key, the one named with a
    # punctuation character, named keys, and the editing keys.
    for chord, cmd in [("Ctrl-S", "file.save"),
                       ("Ctrl-Q", "file.quit"),
                       ("Ctrl-?", "view.help"),
                       ("Ctrl-G", "goto.line"),
                       ("Enter", "edit.newline"),
                       ("Backspace", "edit.delete-back"),
                       ("Ctrl-H", "edit.delete-back"),
                       ("Del", "edit.delete-forward"),
                       ("Left", "move.left"),
                       ("PgUp", "move.page-up"),
                       ("PgDn", "move.page-down"),
                       ("Home", "move.line-start"),
                       ("End", "move.line-end"),
                       ("Ctrl-U", "view.line-numbers")]:
        check("%s is bound to %s" % (chord, cmd), bound.get(chord), cmd)

    # Two chords may share one command, so the list is longer than the set of
    # commands it names; and no chord may be bound twice.
    chords = [c for c, _ in pairs]
    check("no chord is bound twice", len(chords), len(set(chords)))
    check("every binding names a command",
          all(name and "." in name for _, name in pairs), True)

    # The help screen takes its key column from this same keymap, so what it
    # shows for a command has to be the chord that actually runs it. The reference is
    # taller than a 24-row terminal, so the second screenful counts too -- which is
    # what the footer's "PgUp/PgDn scroll" is there for.
    def help_text(keys):
        scr = Screen(24, 80)
        scr.feed(run([MOLLY, os.path.join(tmpdir, "h.txt")], keys))
        return "\n".join(scr.text())

    shown = help_text(["\x1f"]) + help_text(["\x1f", "\x1b[6~"])
    check("help shows the bound chord for save", "Ctrl-S" in shown, True)
    check("help shows the bound chord for help", "F1" in shown, True)
    check("help shows the bound chord for go to line", "Ctrl-G" in shown, True)
    check("help shows the bound chord for the palette",
          "Ctrl-Space" in shown, True)


# ------------------------------------------------------- configuration ----

def test_config(tmpdir):
    """The configuration file, from the outside: does it change the editor?

    The parser itself is covered by the unit tests, which can read a bad line and
    check the warning without a terminal. What only a pty can show is the part
    that matters to a user -- that `line numbers = on` actually paints a gutter,
    that `mouse = off` actually stops molly claiming the mouse, and that a
    rebound chord actually runs the command.
    """
    print("\nconfiguration: mollyrc changes the editor")

    body = "alpha\nbeta\ngamma\n"
    path = os.path.join(tmpdir, "cfg.txt")
    with open(path, "w") as fh:
        fh.write(body)

    def with_config(text, keys, rows=24, cols=80):
        rc = os.path.join(tmpdir, "mollyrc")
        with open(rc, "w") as fh:
            fh.write(text)
        return run([MOLLY, path], keys, rows=rows, cols=cols,
                   env={"MOLLY_CONFIG": rc})

    def screen_of(out, rows=24, cols=80):
        scr = Screen(rows, cols)
        scr.feed(out)
        return scr.text()

    def status_row(out, rows=24, cols=80):
        """The status bar, which is the row above the message bar.

        status_lines() reads the message row; the selection byte count and the
        cursor position live one row up, so a test about either has to look
        here instead.
        """
        return screen_of(out, rows, cols)[rows - 2]

    # Line numbers, which are off by default: the gutter is the visible proof
    # that a setting reached the editor rather than only the settings table.
    out = with_config("[view]\nline numbers = on\n", [])
    check("line numbers = on paints the gutter",
          screen_of(out)[TEXT_ROW - 1].startswith("1 │ alpha"), True)
    out = with_config("", [])
    check("and without it there is no gutter",
          screen_of(out)[TEXT_ROW - 1].startswith("alpha"), True)

    # The menu bar, which is on by default. Its default now lives in the
    # settings table rather than in screen_init_layers, so this is also the
    # check that moving it did not lose it.
    out = with_config("", [])
    check("the menu bar is on by default", "File" in screen_of(out)[0], True)
    out = with_config("[view]\nmenu bar = off\n", [])
    check("menu bar = off takes its row back", "File" in screen_of(out)[0],
          False)
    check("and the text moves up into it", screen_of(out)[0].startswith("alpha"),
          True)

    # Tab width. A tab is eight columns by default; four means the character
    # after it lands four columns in.
    tabbed = os.path.join(tmpdir, "tabbed.txt")
    with open(tabbed, "w") as fh:
        fh.write("\tx\n")
    rc = os.path.join(tmpdir, "mollyrc")
    with open(rc, "w") as fh:
        fh.write("[editor]\ntab width = 4\n")
    out = run([MOLLY, tabbed], [], env={"MOLLY_CONFIG": rc})
    check("tab width = 4 renders a tab as four columns",
          screen_of(out)[TEXT_ROW - 1][:5], "    x")
    out = run([MOLLY, tabbed], [], env={"MOLLY_CONFIG": ""})
    check("and eight by default", screen_of(out)[TEXT_ROW - 1][:9], "        x")

    # The mouse. With reporting on, molly asks the terminal for it at startup;
    # with it off, molly must not ask -- that is the whole of the setting, and
    # the request is visible in the byte stream.
    out = with_config("", [])
    check("molly claims the mouse by default", b"\x1b[?1006h" in out, True)
    out = with_config("[input]\nmouse = off\n", [])
    check("mouse = off leaves it to the terminal", b"\x1b[?1006h" in out, False)

    # A rebound chord has to actually run the command. F9 is bound to nothing by
    # default, so this is unambiguous: the file is dirty, and if F9 saved it the
    # file on disk has the edit in it.
    rc = os.path.join(tmpdir, "keyrc")
    with open(rc, "w") as fh:
        fh.write("[keys]\nf9 = file.save\n")
    target = os.path.join(tmpdir, "bound.txt")
    with open(target, "w") as fh:
        fh.write(body)
    run([MOLLY, target], ["Z", "\x1b[20~"], env={"MOLLY_CONFIG": rc})
    with open(target) as fh:
        check("a chord bound in the file runs its command", fh.read(),
              "Zalpha\nbeta\ngamma\n")

    # And an unbound one has to stop working. Ctrl-O opens a file; unbound, it
    # must do nothing at all rather than fall through to inserting a character.
    out = with_config("[keys]\nctrl-o = none\n", ["\x0f"])
    check("= none really unbinds a chord",
          any("Open" in line for line in screen_of(out)), False)

    # Shift-F3, which is the reason Shift became nameable: the shifted form must
    # reach its own binding while the unshifted key keeps its own.
    rc = os.path.join(tmpdir, "shiftrc")
    with open(rc, "w") as fh:
        fh.write("[keys]\nf3 = search.next\nshift-f3 = view.line-numbers\n")
    out = run([MOLLY, path], ["\x1b[1;2R"], env={"MOLLY_CONFIG": rc})
    check("Shift-F3 reaches its own binding",
          any("Line numbers: on" in m for m in status_lines(out)), True)
    out = run([MOLLY, path], ["\x1bOR"], env={"MOLLY_CONFIG": rc})
    check("and plain F3 reaches its own",
          any("Line numbers" in m for m in status_lines(out)), False)

    # Shift with an arrow still extends a selection, because nothing is bound to
    # it -- the fallback that makes a shifted binding possible without taking
    # selection away.
    out = with_config("", ["\x1b[1;2C", "\x1b[1;2C"])
    check("Shift-Right still extends the selection",
          "Sel 2" in status_row(out), True)

    print("\nconfiguration: a file molly disagrees with")

    # A bad line is a warning on the message bar, not a refusal to start, and
    # every good line around it still applies.
    out = with_config(
        "[editor]\ntab width = 99\n[view]\nline numbers = on\n", [])
    msgs = status_lines(out)
    check("a bad value is reported on the message bar",
          any("must be 1 to 16" in m for m in msgs), True)
    check("naming the line it was on", any("mollyrc:2" in m for m in msgs), True)
    check("and the good lines still applied",
          screen_of(out)[TEXT_ROW - 1].startswith("1 │ alpha"), True)

    # Wider than the rest, because the warning names the configuration file by
    # its full path and the suite's temporary directory is long: at 80 columns
    # the count falls off the right-hand edge of the message bar. A real
    # mollyrc lives at ~/.config/molly/mollyrc and fits with room to spare.
    out = with_config("[editor]\ntab width = 99\nundo steps = nonsense\n", [],
                      cols=160)
    check("several problems are counted",
          any("(2 problems)" in m for m in status_lines(out, cols=160)), True)

    out = with_config("[keys]\nalt-f = file.save\n", [])
    check("rebinding a menu letter is refused out loud",
          any("reserved by the menu bar" in m for m in status_lines(out)), True)
    check("and Alt-F still opens the File menu",
          any("Undo" not in line and "Save" in line
              for line in screen_of(run([MOLLY, path], ["\x1bf"],
                                        env={"MOLLY_CONFIG": ""}))), True)

    # A missing file is the normal case and must say nothing at all.
    out = run([MOLLY, path], [],
              env={"MOLLY_CONFIG": os.path.join(tmpdir, "no-such-rc")})
    check("no configuration file is not a warning",
          any("mollyrc" in m or "problem" in m for m in status_lines(out)),
          False)

    # A directory where the file should be is not the normal case, and saying
    # nothing would look exactly like the file not being read.
    d = os.path.join(tmpdir, "rc-is-a-dir")
    os.mkdir(d)
    out = run([MOLLY, path], [], env={"MOLLY_CONFIG": d})
    check("but a config path that cannot be read is",
          any("rc-is-a-dir" in m for m in status_lines(out)), True)


def test_save_settings(tmpdir):
    """View > Save Settings, and what comes back on the next start.

    This is the round trip the whole settings table exists for: change something
    the ordinary way, save, start again, and find it still changed. It is tested
    through the editor rather than through config_write because what it has to
    prove is that the editor's live state is what gets written -- the unit test
    can only prove that the file parses.
    """
    print("\nconfiguration: saving settings")

    body = "alpha\nbeta\n"
    path = os.path.join(tmpdir, "s.txt")
    with open(path, "w") as fh:
        fh.write(body)
    rc = os.path.join(tmpdir, "saved", "mollyrc")

    def screen_of(out, rows=24, cols=80):
        scr = Screen(rows, cols)
        scr.feed(out)
        return scr.text()

    # Turn the gutter on with Ctrl-U, then save settings from the View menu:
    # Alt-V, then the last item.
    out = run([MOLLY, path], ["\x15", "\x1bv", "\x1b[B" * 6, "\r"],
              env={"MOLLY_CONFIG": rc})
    check("saving settings reports where it wrote",
          any("Settings saved" in m for m in status_lines(out)), True)
    check("and creates the directory it needed", os.path.exists(rc), True)

    with open(rc) as fh:
        text = fh.read()
    check("the file records the setting as it was left",
          "line numbers = on" in text, True)
    check("and the ones nobody touched at their defaults",
          "menu bar = on" in text, True)
    check("it is commented, so it can be hand-edited afterwards",
          text.lstrip().startswith("# molly"), True)

    # The point of all of it: the next start comes up with the gutter on.
    out = run([MOLLY, path], [], env={"MOLLY_CONFIG": rc})
    check("and the next start has the setting",
          screen_of(out)[TEXT_ROW - 1].startswith("1 │ alpha"), True)
    check("without a warning about its own file",
          any("problem" in m for m in status_lines(out)), False)

    # Written twice is still valid: the second write must not double anything or
    # trip over the file it is replacing.
    out = run([MOLLY, path], ["\x15", "\x1bv", "\x1b[B" * 6, "\r"],
              env={"MOLLY_CONFIG": rc})
    check("saving over an existing file works",
          any("Settings saved" in m for m in status_lines(out)), True)
    out = run([MOLLY, path], [], env={"MOLLY_CONFIG": rc})
    check("and turns the setting back off again",
          screen_of(out)[TEXT_ROW - 1].startswith("alpha"), True)
    check("still without a warning",
          any("problem" in m for m in status_lines(out)), False)

    # A path that cannot be written is reported rather than lost.
    out = run([MOLLY, path], ["\x1bv", "\x1b[B" * 6, "\r"],
              env={"MOLLY_CONFIG": "/proc/version"})
    check("a write that cannot happen says so",
          any("Could not save settings" in m for m in status_lines(out)), True)


# ----------------------------------------------------------- CUA keymap ----

def test_cua_keymap(tmpdir):
    """The four bindings olly disagreed with CUA on, and the three commands the
    resolutions needed.

    Each of these is a key that used to do something else, so each one is two
    checks: that the new meaning works, and that the old meaning has gone from
    that key rather than lingering alongside it.
    """
    print("\nCUA keymap: the keys that moved")

    body = "alpha\nbeta\ngamma\n"
    path = os.path.join(tmpdir, "cua.txt")
    with open(path, "w") as fh:
        fh.write(body)

    def screen_of(out, rows=24, cols=80):
        scr = Screen(rows, cols)
        scr.feed(out)
        return scr.text()

    def status_row(out, rows=24, cols=80):
        return screen_of(out, rows, cols)[rows - 2]

    # F1 is help now; Ctrl-? still is.
    out = run([MOLLY, path], ["\x1b[11~"])
    check("F1 opens the help screen",
          any("Keyboard" in line or "FILES" in line for line in screen_of(out)),
          True)
    out = run([MOLLY, path], ["\x1f"])
    check("and Ctrl-? still does",
          any("Keyboard" in line or "FILES" in line for line in screen_of(out)),
          True)
    check("with F1 the key the help screen prints",
          any(re.search(r"\bF1\b", line) for line in screen_of(out)), True)

    # F3 and Shift-F3 are find next and previous. The file is one match per
    # line on purpose: searching a file where a letter appears twice on one line
    # makes "the next match" a statement about columns rather than about the
    # binding, which is not what is being tested here.
    marks = os.path.join(tmpdir, "marks.txt")
    with open(marks, "w") as fh:
        fh.write("x one\nx two\nx three\n")

    def find_then(keys):
        return run([MOLLY, marks], ["\x06", "x", "\r"] + keys)

    out = find_then([])
    check("a search lands on the first match", "Ln 1" in status_row(out), True)
    out = find_then(["\x1b[13~"])
    check("F3 finds the next match", "Ln 2" in status_row(out), True)
    out = find_then(["\x1b[13~", "\x1b[13~"])
    check("and again the one after that", "Ln 3" in status_row(out), True)
    out = find_then(["\x1b[13~", "\x1b[1;2R"])
    check("Shift-F3 finds the previous one", "Ln 1" in status_row(out), True)
    out = find_then(["\x0e"])
    check("Ctrl-N is still an alias for next", "Ln 2" in status_row(out), True)
    out = find_then(["\x1b[13~", "\x10"])
    check("and Ctrl-P for previous", "Ln 1" in status_row(out), True)

    # F5 is repaint, which Ctrl-L used to be. A repaint is invisible by nature,
    # so what is checked is that the binding reached the command: the View menu
    # prints the chord for view.repaint down its right-hand side, and it has to
    # be F5 now. (The help screen would do too, but VIEW is below the fold at 24
    # rows, and scrolling it here would be testing the help screen.)
    out = run([MOLLY, path], ["\x1bv"])
    check("the View menu prints F5 for Redraw",
          any("Redraw" in line and "F5" in line for line in screen_of(out)),
          True)
    check("and no longer Ctrl-L",
          any("Redraw" in line and "Ctrl-L" in line
              for line in screen_of(out)), False)

    print("\nCUA keymap: select all, select line, transpose")

    # Ctrl-A selects the whole buffer: 17 bytes of text plus the two internal
    # line breaks that selection_bytes counts.
    # 16, not 17: the three rows are 14 characters and the two line breaks
    # between them count one each, but the newline that ends the file is past
    # the end of the last row and so is not inside the selection.
    out = run([MOLLY, path], ["\x01"])
    check("Ctrl-A selects the whole file", "Sel 16" in status_row(out), True)
    check("and leaves the cursor at the end", "Ln 3, Col 6" in status_row(out),
          True)

    # Ctrl-L selects the line, which is what it does instead of repainting.
    out = run([MOLLY, path], ["\x1b[B", "\x0c"])
    check("Ctrl-L selects the current line", "Sel 4" in status_row(out), True)
    check("and the cursor lands at the end of it",
          "Ln 2, Col 5" in status_row(out), True)

    # Selecting then cutting is the point of both, so check one end to end.
    # A newline, not nothing: the same fact as the 16 above. The selection stops
    # at the end of the last row, so what is left is one empty line, and molly
    # preserves the trailing newline the file was loaded with.
    check("Ctrl-A then Ctrl-X empties the file",
          edit(tmpdir, body, ["\x01", "\x18", SAVE], name="cut-all.txt"), "\n")
    check("Ctrl-L then Ctrl-X empties the line",
          edit(tmpdir, body, ["\x0c", "\x18", SAVE], name="cut-line.txt"),
          "\nbeta\ngamma\n")

    # Ctrl-T transposes, which is what it does instead of toggling case
    # sensitivity -- that is a checkbox in the Find dialog now.
    check("Ctrl-T swaps the characters around the cursor",
          edit(tmpdir, "ab\n", ["\x1b[C", "\x14", SAVE], name="t1.txt"), "ba\n")
    check("at the end of a line it swaps the last two",
          edit(tmpdir, "ab\n", ["\x1b[F", "\x14", SAVE], name="t2.txt"), "ba\n")
    check("in the middle of a word it swaps across the cursor",
          edit(tmpdir, "abcd\n", ["\x1b[C", "\x1b[C", "\x14", SAVE], name="t3.txt"),
          "acbd\n")
    check("and leaves the cursor after both",
          edit(tmpdir, "abcd\n", ["\x1b[C", "\x1b[C", "\x14", "X", SAVE],
               name="t4.txt"), "acbXd\n")
    check("at the start of a line it does nothing",
          edit(tmpdir, "ab\n", ["\x14", SAVE], name="t5.txt"), "ab\n")
    check("on a one-character line it does nothing",
          edit(tmpdir, "a\n", ["\x1b[F", "\x14", SAVE], name="t6.txt"), "a\n")
    check("on the phantom row past the end it does nothing",
          edit(tmpdir, "ab\n", ["\x1b[B", "\x14", SAVE], name="t7.txt"), "ab\n")
    check("one Ctrl-Z undoes the whole swap",
          edit(tmpdir, "abcd\n", ["\x1b[C", "\x14", "\x1a", SAVE], name="t8.txt"),
          "abcd\n")
    check("a multibyte character transposes whole",
          edit_bytes(tmpdir, b"a\xc3\xa9b\n", ["\x1b[C", "\x14", SAVE],
                     name="t9.txt"), b"\xc3\xa9ab\n")
    check("and so does one on the other side of the cursor",
          edit_bytes(tmpdir, b"\xc3\xa9ab\n", ["\x1b[C", "\x14", SAVE],
                     name="t10.txt"), b"a\xc3\xa9b\n")

    # Ctrl-T must no longer toggle the search's case sensitivity. If it still
    # did, this would say so on the message bar.
    out = run([MOLLY, path], ["\x14"])
    check("Ctrl-T no longer toggles case sensitivity",
          any("case" in m.lower() for m in status_lines(out)), False)

    # Match case keeps its menu entry and its checkbox; only the chord went.
    out = run([MOLLY, path], ["\x1bs"])
    check("Match case is still in the Search menu",
          any("Match case" in line for line in screen_of(out)), True)
    out = run([MOLLY, path], ["\x1bs"])
    check("and shows no chord beside it now",
          any("Match case" in line and "Ctrl-T" in line
              for line in screen_of(out)), False)


def main():
    global MOLLY
    MOLLY = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./molly")
    if not os.path.exists(MOLLY):
        sys.exit("no molly binary at %s -- run make first" % MOLLY)

    tmpdir = tempfile.mkdtemp(prefix="molly-tests-")
    try:
        test_keys(tmpdir)
        test_input_decoding(tmpdir)
        test_mouse(tmpdir)
        test_menu(tmpdir)
        test_prompt_field(tmpdir)
        test_palette(tmpdir)
        test_command_table(tmpdir)
        test_tab(tmpdir)
        test_utf8(tmpdir)
        test_wide(tmpdir)
        test_status_col(tmpdir)
        test_selection(tmpdir)
        test_clipboard(tmpdir)
        test_selection_highlight(tmpdir)
        test_osc52_clipboard(tmpdir)
        test_gutter(tmpdir)
        test_help(tmpdir)
        test_search(tmpdir)
        test_case_sensitive_search(tmpdir)
        test_regex_search(tmpdir)
        test_find_dialog(tmpdir)
        test_char_aligned_search(tmpdir)
        test_goto_line(tmpdir)
        test_replace(tmpdir)
        test_regex_replace(tmpdir)
        test_save(tmpdir)
        test_external_change(tmpdir)
        test_line_endings(tmpdir)
        test_symlink_race(tmpdir)
        test_signals(tmpdir)
        test_recovery(tmpdir)
        test_recovery_prompt(tmpdir)
        test_cli_args(tmpdir)
        test_open_dialog(tmpdir)
        test_directory_refused(tmpdir)
        test_undo(tmpdir)
        test_undo_cap(tmpdir)
        test_config(tmpdir)
        test_save_settings(tmpdir)
        test_cua_keymap(tmpdir)
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    print("\n%d passed, %d failed" % (PASSED, len(FAILURES)))
    for f in FAILURES:
        print("  failed: %s" % f)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
