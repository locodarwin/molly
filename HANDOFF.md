# Handoff

Written 2026-10-08 at `79f586a`, which is `main` and matches `origin/main`.
This file is for whoever picks molly up next. `ROADMAP.md` is the argument and
the plan; this is the state, the order, and the things that will cost you an
hour if nobody tells you.

---

## Where we are

Phases 0 and 1 are done. Phase 2 has started and its first feature — regular
expressions — is finished end to end: molly searches and replaces with
patterns, on an engine of its own, driven from a Find dialog.

**Measured, not remembered** (re-measure before quoting these anywhere):

| | |
| --- | --- |
| Source | 8,843 lines, `src/molly.h` plus 23 `.c` files |
| Checks | 658 — 415 pty, 36 golden screens, 207 unit |
| Fuzz | 4,040 cases per `make test` (40 replace, 4,000 regex, seed 42) |
| Warnings | clean under `-Wall -Wextra -Wpedantic -Wshadow`; `regex.c` also under `-Wconversion` |
| Version | still `0.1.0-dev` |

The gate is `make test`: the pty suite, the golden screens, both fuzzers and
the unit binary. `make test-oom` is the allocation-failure sweep and needs
`cc` at runtime; it is not part of `make test`. Green at the last commit:

```text
415 passed, 0 failed
36 passed, 0 failed
40 cases, 0 failed (seed 42)
4000 cases, 0 failed (seed 42)
```

### What exists that did not exist a week ago

- A **cell grid** renderer with semantic roles, resolved by `theme.c` at both
  256 and 16 colours. Both depths are snapshotted so neither rots.
- A **layer stack**, and one key wait (`ui_wait_key()`) that pumps the event
  loop, so a modal dialog still resizes and still redraws.
- A **widget toolkit**: label, list, text, input, checkbox, button, with focus
  order, Tab/Shift-Tab, and SGR 1006 mouse hit-testing. `W_LIST` is
  deliberately *not* in the focus order — nothing needs it yet, and a role
  nothing uses is a theme entry nothing tests.
- A **command table**. The menu bar, the keymap, the palette and the help
  screen are all that one table viewed four ways. A new action is a record, not
  a `switch` case.
- A **menu bar**, a **mouse**, a **command palette** (Ctrl-Space), a generated
  **help screen** (Ctrl-?), the **open-file dialog** and the **Find dialog**, all
  on the toolkit.
- A **regex engine** (`src/regex.c`, 1,143 lines): a backtracking PCRE subset
  with counted repetition, captures, a step budget and two prefilters. It is
  fuzzed differentially against Python's `re` in bytes mode.

### Decisions already settled — do not reopen without asking

Full table in `ROADMAP.md` §7. The ones that bite soonest:

- **A search matches the row as stored**, in both plain and pattern mode. Olly
  searched the tab-expanded render buffer, so eight spaces found a tab. That is
  gone, and so are the two functions that mapped between the two spaces.
- **Config file is INI-style sections, `key = value`.** Chosen for being small
  to parse *and* small to write back, because the Options dialogs have to
  persist.
- **Keymap is CUA on the four conflicts, and fully remappable** except the
  eight Alt-letter menu titles.
- **Linux now. Windows possibly later. No macOS.** No Windows code gets
  written, but platform calls stay behind `term.c` and `sys.c`.

---

## What is left

### Still open — needs Shawn

- **Which languages ship with syntax definitions initially.** This is the only
  item left in `ROADMAP.md` §8, and it blocks nothing until syntax highlighting
  starts.

### Next by dependency

This order was stated to Shawn and not yet agreed as a task. Confirm before
starting.

1. **The config file and the CUA keymap.** These go together: config is what
   gives `mouse off`, `menu bar hidden` and a remapped chord somewhere to live,
   and the keymap is already chord *text* in the command table, so parsing a
   chord back out of a file is the missing half. Syntax INI, decided. Path
   `~/.config/molly/mollyrc`.
2. **Multiple buffers and tabs.** Mostly bookkeeping now that buffer and view
   are separate, and a prerequisite for File→Close, the explorer and grep
   results. Expect every "are you sure?" path to become a list.
3. **Window splits.** Free-ish after the grid and `struct view`, with a long
   tail of off-by-one bugs — which is what the golden harness is for.
4. **Syntax highlighting.** The rules are cheap; the cost is a cached lexer
   state per row and invalidating it on edit.
5. **Shell filter.** Best value-per-line left on the list after regex. Needs a
   timeout, a cancel key and an output cap, and it is the first place molly runs
   something on the user's behalf.

Everything else — the cheap Edit/View/Navigate items, soft wrap, multi-cursor,
the integrated terminal, LSP, big-file handling — is argued in `ROADMAP.md` §5
with its verdict. Soft wrap and multi-cursor already have their *models*
landed; the features come later.

---

## How to work on this

### Conventions this repo keeps

- **Separate the mechanism commit from the visible-change commit.** The engine
  landed unreachable from the editor; the feature that reached it came next.
- **Measure rather than guess.** 160 bytes per recursion depth came from an
  instrumented scratch build. The 9 ms worst-case budget was timed. Do not put
  an estimate in a comment.
- **Probe at sizes nothing has been tried at.** 6×20, 7×22 and 8×24 are what
  found both Find-dialog fit bugs. Every dialog needs a narrow golden.
- **Prefer the test a reference engine can arbitrate.** Where the references
  disagree, pin molly's answer in a unit test and say so in the comment. Two
  such corners are named at the top of `src/regex.c`.
- **A new UI element needs a golden screen at both colour depths**, and a narrow
  one. Snapshots have separate blocks for screen, attrs, colors, modifiers and
  cursor, so a lost highlight or a stray cursor claim is caught, not averaged
  away.
- Commits go straight to `main`, no PRs, and Shawn approves each one. Git
  identity is configured nowhere on this machine: commit with
  `git -c user.name=shawn -c user.email=locodarwin@gmail.com`, rather than
  writing config nobody asked for.

### Gotchas

- **`pty_harness.run()` execs via `os.execve`** — the binary path must be
  absolute. A relative path fails silently in the child and every assertion
  comes back False.
- **`screen_sync_size()` before measuring anything.** A dialog that lays itself
  out against a stale size draws outside its frame.
- **`dialog_init` memsets.** Call it *once*, outside the modal loop, or it wipes
  `focus` on every iteration. Set `d.focus` after it, not before.
- **Derive widget widths from `d.cols` after `dialog_layout`,** not from the
  terminal.
- **An input field keeps its rightmost column for the cursor**, so a narrow
  expectation wants N−1 characters, not N.
- **Only the focused input claims the cursor** (`widget_input_draw(..., claim)`).
  A dialog whose field should keep the cursor has to say so — the palette sets
  `d.focus = 1` for exactly this reason.
- **A Tab keystroke is dropped by the inline prompt.** This is why
  `tests/fuzz_replace.py` puts tabs in the *text* it generates but never in a
  query or a replacement (`TYPEABLE`), and why a tab is only nameable as `\t` in
  a pattern.
- **The regex prefilters can defeat a budget test.** `(a+)+b` on thirty a's is
  now an instant no-match, because `b` is a required character. Budget tests
  anchor instead: `(a+)+$`.
- **The help column truncates.** A `.help` string longer than the column is
  silently cut; check the help screen after adding a command.

### Operational mistakes to not repeat

These all cost real time in the last session.

- **Do not rebuild `./molly` while a test run is in flight.** It invalidated
  three separate runs before I noticed.
- **Kill by exact PID.** `pkill -f "..."` matched its own wrapper shell and
  killed it — exit 144 — twice.
- **Background task output files stay empty until the command completes.**
  Reading one immediately tells you nothing.
- **Check what is still running.** A stale ten-seed fuzz loop ate CPU for about
  thirty-seven minutes unnoticed.

---

## Running it live

```bash
make            # cc -std=c11, per-module .o, no dependencies
./molly FILE
make test       # the gate
make test-oom   # allocation-failure sweep, needs cc at runtime
make golden     # re-run the golden screens alone
MOLLY_UPDATE_GOLDEN=1 python3 tests/test_golden.py ./molly   # rewrite snapshots
```

Rewrite snapshots only when the change to the output is intended, and read the
resulting diff.

What to try first, since these are the new surfaces: **Ctrl-F** for the Find
dialog (Tab between field, the two checkboxes and the buttons, Space toggles),
**Alt-R** to read the query as a pattern, **Ctrl-Space** (or Alt-X) for the palette,
**Alt-F** and friends for the menus, **Ctrl-?** for help, and the mouse on all
of it. `MANUAL.md` has the regular-expression syntax table, what is not
supported, and what a search matches against.
