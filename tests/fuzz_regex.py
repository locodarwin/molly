#!/usr/bin/env python3
"""Differential fuzz of molly's regex engine against Python's re.

Run with:  make test    (standalone: python3 tests/fuzz_regex.py ./tests/regex_cli)

Each case generates a random pattern from the subset molly implements, runs it
against a random subject through tests/regex_cli, and requires molly's answer --
match or not, and the byte offsets of the whole match and of every group -- to
equal what Python's re reports. A backtracking engine has a great many paths
through it and almost all of them are reached by some three-character pattern
nobody would think to write down; this finds those.

Python's re is driven in *bytes* mode, because that is the only mode whose
offsets are byte offsets and whose \\w, \\b and IGNORECASE are ASCII-only, which
is what molly implements. Patterns and subjects are therefore ASCII: the
UTF-8-aware half of the engine, where `.` consumes a character rather than a
byte, cannot be checked against a bytes-mode re and is covered by the unit tests
in tests/test_units.c instead.

Newlines are kept out of subjects. Python's `$` also matches just before a
trailing newline and molly's does not, because molly matches one row at a time
and a row has no newline in it to be before.

Where the two engines disagree with each other, molly cannot match both, and one
shape is like that: a lazy quantifier as the body of a repetition. For
(a*?){1,2}b against "ab", molly and Perl put group 1 at (1,1) and Python puts it
at (0,1) -- the same overall match by a different path through the repetition.
Molly follows Perl there; it is not trying to be bit-compatible with either
engine in corners where the major engines differ, and tests/test_units.c pins
what it does.

Such a case is still compared in full, and a difference is allowed only if
molly's answer survives a weaker check that does not depend on either engine's
taste -- see plausible(). Whether a match exists at a given position, and so
where the leftmost one starts, is a property of the pattern rather than of the
engine, so those must agree no matter what; what may differ is which of the
matches available at that position is the one found. Every allowed difference is
counted and reported, and too many of them fail the run.

(The other known difference runs the other way and does not reach this test: a
capture inside a repeated group keeps its last value in molly and in Python, and
is cleared at every iteration by Perl and by JavaScript. Python being the
reference here, those cases compare clean; test_units.c pins them too.)

Deterministic: case i draws from random.Random(SEED * 1000003 + i), so a failure
reproduces with the printed command, and two seeds explore disjoint sets of
cases rather than overlapping windows of one. Set MOLLY_FUZZ_SEED to explore
elsewhere and MOLLY_FUZZ_N for more cases.
"""

import os
import random
import re
import subprocess
import sys

N = int(os.environ.get("MOLLY_FUZZ_N", "4000"))
SEED = int(os.environ.get("MOLLY_FUZZ_SEED", "42"))
BLOCK = 500

# Python's \B did not match the empty input string until 3.14, where it became
# the plain opposite of \b (gh-124130); molly, Perl and PCRE have always matched
# there. On an older Python the reference is wrong about exactly this one shape,
# so an empty subject under a pattern holding \B is not comparable and is
# counted and skipped instead. Nothing else about \B changed: the old
# special case was for a zero-length subject only.
PY_B_MATCHES_EMPTY = sys.version_info >= (3, 14)

# Literal characters that need no escaping, and the subject alphabet. Both are
# small on purpose: a four-letter alphabet is what makes a random pattern hit a
# random subject often enough for the comparison to be about matching rather
# than about missing.
LITERALS = "abc01_ -:"
SUBJECT = "aabbc01_ -:\t"
CLASS_ITEMS = "abc01_"
SHORTHANDS = ["\\d", "\\D", "\\w", "\\W", "\\s", "\\S"]
QUANTS = ["*", "+", "?", "{0,1}", "{1,2}", "{2}", "{0,2}", "{1,}", "{2,3}"]


def gen_atom(rng, depth, state):
    """One atom, with no quantifier on it yet."""
    choices = ["lit", "lit", "lit", "dot", "class", "short", "esc"]
    if depth > 0:
        choices += ["group", "group", "ncgroup"]
    if depth > 0:
        choices += ["bound"]
    kind = rng.choice(choices)
    if kind == "lit":
        return re.escape(rng.choice(LITERALS))
    if kind == "dot":
        return "."
    if kind == "esc":
        return "\\" + rng.choice(".*+?[]()|^$\\-")
    if kind == "short":
        return rng.choice(SHORTHANDS)
    if kind == "bound":
        return rng.choice(["\\b", "\\B"])
    if kind == "class":
        # Ranges first, then single items, then at most one trailing hyphen.
        # Ordering inside a class means nothing to either engine, and this
        # ordering is the one where neither has to guess: a hyphen right after a
        # shorthand reads as a literal to molly and as a broken range to Python,
        # which is a disagreement about syntax rather than about matching, and
        # the unit tests are where molly's own reading of it is pinned.
        items = ""
        if rng.random() < 0.4:
            lo, hi = sorted(rng.sample("abc012", 2))
            items += "%s-%s" % (lo, hi)
        for _ in range(rng.randint(1, 2)):
            items += (rng.choice(SHORTHANDS) if rng.random() < 0.3
                      else rng.choice(CLASS_ITEMS))
        if rng.random() < 0.15:
            items += "-"
        return "[%s%s]" % ("^" if rng.random() < 0.3 else "", items)
    # A group. Capturing ones are counted so the pattern stays inside the nine
    # that \1..\9 can name, which is also all molly compiles.
    inner = gen_alt(rng, depth - 1, state)
    if kind == "ncgroup" or state["caps"] >= 9:
        return "(?:%s)" % inner
    state["caps"] += 1
    return "(%s)" % inner


def gen_piece(rng, depth, state):
    lazy_before = state["lazy"]
    atom = gen_atom(rng, depth, state)
    if atom in ("\\b", "\\B"):
        return atom  # quantifying a boundary is legal but says nothing
    if rng.random() < 0.4:
        # A lazy quantifier generated *inside* this atom is about to end up
        # inside the body of a repetition, which is the one shape where molly
        # and Python take different paths to the same match. Noted exactly here
        # rather than guessed at from the finished pattern.
        if state["lazy"] > lazy_before:
            state["lazy_in_rep"] = True
        atom += rng.choice(QUANTS)
        if rng.random() < 0.35:
            atom += "?"  # the lazy form
            state["lazy"] += 1
    return atom


def gen_cat(rng, depth, state):
    out = ""
    if depth >= 1 and rng.random() < 0.12:
        out += "^"
    for _ in range(rng.randint(1, 4)):
        out += gen_piece(rng, depth, state)
    if depth >= 1 and rng.random() < 0.12:
        out += "$"
    return out


def gen_alt(rng, depth, state):
    out = gen_cat(rng, depth, state)
    while rng.random() < 0.25:
        out += "|" + gen_cat(rng, depth, state)
    return out


def gen_case(rng):
    state = {"caps": 0, "lazy": 0, "lazy_in_rep": False}
    pat = gen_alt(rng, rng.randint(1, 3), state)
    text = "".join(rng.choice(SUBJECT) for _ in range(rng.randint(0, 24)))
    # Mostly from the beginning, sometimes from part way in: a resumed search is
    # what Find Next does, and `^` still only matches at the real start of the
    # row in both engines, which is the agreement worth checking.
    start = 0 if rng.random() < 0.7 or not text else rng.randint(0, len(text))
    return pat, text, rng.random() < 0.25, start, state["lazy_in_rep"]


def py_answer(pat, text, icase, start):
    """Python's verdict, as the same tuple molly's output parses into."""
    flags = re.IGNORECASE if icase else 0
    rx = re.compile(pat.encode(), flags)
    m = rx.search(text.encode(), start)
    if m is None:
        return None
    return tuple(m.span(i) for i in range(rx.groups + 1))


def disagreement(got, want):
    """What is wrong with molly's answer, or None."""
    if isinstance(got, str) and got != "nomatch":
        return "molly said %r" % got          # an error line: a parse bug
    if got == "nomatch":
        return (None if want is None else
                "Python matched %r, molly did not" % (want,))
    if want is None:
        return "molly matched %r, Python did not" % (got,)
    if got != want:
        return "offsets differ: molly %r, Python %r" % (got, want)
    return None


def plausible(pat, text, icase, got, want):
    r"""molly disagrees with Python: is its answer nonetheless a real match?

    The two must agree that there is a match and on where it starts, because
    neither is a matter of search order. The extent may differ, so molly's extent
    has to be checkable on its own terms: the pattern is asked to match at that
    start with the rest of the subject pinned behind a lookahead, which is true
    exactly when some match of that extent exists there. (The obvious way to ask
    -- fullmatch with an endpos -- is wrong, because truncating the subject moves
    the end of it, and \b, \B and $ all read differently at a boundary that was
    invented for the question.) The groups then only have to sit inside the match
    and be the right way round.
    """
    if want is None or isinstance(got, str):
        return False
    if got[0][0] != want[0][0]:
        return False
    so, eo = got[0]
    subject = text.encode()
    flags = re.IGNORECASE if icase else 0
    pinned = re.compile(b"(?:" + pat.encode() + b")(?=" +
                        re.escape(subject[eo:]) + b"$)", flags)
    if pinned.match(subject, so) is None:
        return False
    for gs, ge in got[1:]:
        if gs == -1 and ge == -1:
            continue
        if not so <= gs <= ge <= eo:
            return False
    return True


def parse_line(line):
    if line.startswith("match "):
        return tuple(tuple(int(v) for v in f.split(","))
                     for f in line.split()[1:])
    return line.strip()


def main():
    if len(sys.argv) != 2:
        print("usage: fuzz_regex.py ./tests/regex_cli")
        return 2
    driver = sys.argv[1]

    cases = [gen_case(random.Random(SEED * 1000003 + i)) for i in range(N)]
    feed = "".join("%d %d %s %s\n" % (1 if icase else 0, start,
                                      pat.encode().hex(), text.encode().hex())
                   for pat, text, icase, start, _ in cases)
    proc = subprocess.run([driver], input=feed, capture_output=True, text=True)
    lines = proc.stdout.splitlines()
    if len(lines) != N:
        print("  FAIL  driver answered %d of %d cases (stderr: %r)"
              % (len(lines), N, proc.stderr[-400:]))
        return 1

    failures = hard = known = stale = 0
    for i, ((pat, text, icase, start, lazy_in_rep), line) in enumerate(
            zip(cases, lines)):
        got = parse_line(line)
        if got == "toohard":
            hard += 1
            continue
        want = py_answer(pat, text, icase, start)
        problem = disagreement(got, want)
        # The documented shape, and only where it actually bites.
        if problem and lazy_in_rep and plausible(pat, text, icase, got, want):
            known += 1
            problem = None
        # And the shape this Python's \B is simply older than.
        if problem and not PY_B_MATCHES_EMPTY and text == "" and "\\B" in pat:
            stale += 1
            problem = None
        if problem:
            failures += 1
            if failures <= 20:
                print("  FAIL  case %d: %s" % (i, problem))
                print("          pattern: %r  text: %r  from: %d%s"
                      % (pat, text, start,
                         "  (case-insensitive)" if icase else ""))
        elif (i + 1) % BLOCK == 0:
            print("  pass  cases %d-%d" % (i + 2 - BLOCK, i + 1))

    if known:
        print("  note  %d case(s) found a different but legitimate match, in "
              "the documented lazy-quantifier-inside-a-repetition shape" % known)
    if known > 5 + N // 2000:
        print("  FAIL  that shape is being exempted too often: %d of %d"
              % (known, N))
        failures += 1
    if stale:
        print("  note  %d case(s) skipped: this Python's \\B predates 3.14 and "
              "does not match the empty subject" % stale)
    if hard:
        print("  note  %d case(s) exceeded the step budget and were skipped"
              % hard)
    # A budget that most random patterns hit would mean the comparison above is
    # mostly not happening, which is the way this test would quietly stop
    # testing anything.
    if hard > N // 20:
        print("  FAIL  too many cases gave up: %d of %d" % (hard, N))
        failures += 1
    print("\n%d cases, %d failed (seed %d)" % (N, failures, SEED))
    if failures:
        print("reproduce: MOLLY_FUZZ_SEED=%d python3 tests/fuzz_regex.py %s"
              % (SEED, driver))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
