# Handoff

Written 2026-10-08 at `35cf7dc`, which is `main`.
This file is for whoever picks molly up next. `ROADMAP.md` is the argument and
the plan; this is the state, the order, and the things that will cost you an
hour if nobody tells you.

---

## Where we are

Phases 0 and 1 are done. Phase 2 has three features finished end to end:
regular expressions, the configuration file, and the CUA keymap. Molly searches
and replaces with patterns on an engine of its own; it reads
`~/.config/molly/mollyrc` and can write it back; and its keys are CUA and
remappable.

**Measured, not remembered** (re-measure before quoting these anywhere):

| | |
| --- | --- |
| Source | 10,143 lines, `src/molly.h` plus 24 `.c` files |
| Checks | 858 — 480 pty, 46 golden screens, 332 unit |
| Fuzz | 4,040 cases per `make test` (40 replace, 4,000 regex, seed 42) |
| Warnings | clean under `-Wall -Wextra -Wpedantic -Wshadow`; `regex.c` also under `-Wconversion` |
| OOM sweep | `make test-oom` green: 126 allocations failed in turn, 114 through `die()`, 12 kept running |
| Version | still `0.1.0-dev` |

The gate is `make test`: the pty suite, the golden screens, both fuzzers and
the unit binary. `make test-oom` is the allocation-failure sweep and needs
`cc` at runtime; it is not part of `make test`. Green at the last commit:

```text
480 passed, 0 failed
46 passed, 0 failed
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
- A **configuration file** (`src/config.c`, 724 lines): an INI parser that warns
  and carries on, a settings table that owns every default, atomic write-back,
  and a `[keys]` section. `View ▸ Save Settings` writes it.
- A **CUA keymap**, fully remappable, with `Shift` nameable in a chord.

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
- **Every default lives in the settings table and nowhere else.** The menu bar
  used to declare its default in `screen_init_layers` as well, and the mouse was
  enabled in `enable_raw_mode` and taken back afterwards if the file said off.
  Both now have one home, applied by `config_apply()` straight after
  `init_editor()`.
- **Syntax definitions ship for Rust, C, C++, C#, Python, AutoIt (`.au3`) and
  Markdown.** Seven, decided 2026-10-08. Two of them are what make a cached
  per-row lexer state unavoidable rather than merely correct: AutoIt's
  `#cs`/`#ce` blocks and Markdown's fenced code blocks both open a region whose
  end is many rows away. Markdown also means the definition format cannot assume
  keyword/string/comment rules are enough.

---

## What is left

### Still open — needs Shawn

Nothing. `ROADMAP.md` §8 is empty: the last item, which languages ship with
syntax definitions, was answered on 2026-10-08 and is recorded in §7.

### Next by dependency

Item 1 of the previous handoff — the config file and the CUA keymap — is done.
The rest of that order stands, and still wants confirming before it is started.

1. **Multiple buffers and tabs.** Mostly bookkeeping now that buffer and view
   are separate, and a prerequisite for File→Close, the explorer and grep
   results. Expect every "are you sure?" path to become a list.
2. **Window splits.** Free-ish after the grid and `struct view`, with a long
   tail of off-by-one bugs — which is what the golden harness is for.
3. **Syntax highlighting.** The rules are cheap; the cost is a cached lexer
   state per row and invalidating it on edit. The seven languages are settled;
   AutoIt and Markdown are the two that force the cached state.
4. **Shell filter.** Best value-per-line left on the list after regex. Needs a
   timeout, a cancel key and an output cap, and it is the first place molly runs
   something on the user's behalf.

### Found along the way, not fixed

- **Tabs versus spaces is not a setting yet.** `[editor] tab width` changes how
  a tab *renders*; molly still inserts a literal tab. "Insert spaces instead" is
  an editing change, and it belongs with the indentation work.
- **The help screen's columns truncate on bytes, not columns.** `ui.c` cuts with
  `%.*s`, so a description with a multibyte character in it can be cut mid
  character. No shipped `.help` string has one, so nothing shows it today;
  `utf8_fit_cols()` is what it wants when one does.
- **The menu band itself still clips.** At 18 columns the bar reads
  `File  Edit  Searc`. That is the grid clipping a layer rather than a layout
  decision, and unlike the drop-downs there is no obvious right answer — the
  titles are also the Alt mnemonics, so dropping one hides a menu.
- **An Options dialog has nothing to do but exist.** Write-back, the settings
  table and the live-value read-back are all in place and all exercised by
  `View ▸ Save Settings`; the dialog is now presentation over a finished
  mechanism.

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
- **The help column truncates.** It is 17 columns and cuts in silence; check the
  help screen after adding a command. "Swap the two characters at the cursor"
  came out as "Swap the two char".
- **A command with no chord has no business on the help screen.** It renders as a
  blank key beside a description, which reads as a bug. Match case is reachable
  from the Search menu and the palette instead.
- **`config_write` saves what the editor is doing, not what the file said.** Each
  setting may name a function to read its live value back, which is what makes
  "toggle it, then save" work. A test that parses a file and writes it straight
  back out without applying it will see the live defaults, not the parsed values.
- **Shift is only nameable where a terminal reports it** — the function and
  navigation keys. `shift-a` and `ctrl-shift-l` are refused at parse time, which
  is also why the roadmap's `Ctrl-Shift-L` for repaint could not be used.
- **A new setting applied in `config_apply()` can silently defeat an env test
  hook.** `undo steps` becoming a setting made `MOLLY_UNDO_STEPS` useless,
  because `config_apply()` ran after `init_editor()` and put the cap back. The
  precedence is now default, then file, then environment, and the hook sits
  after `config_apply()` in `main()` to say so.

### Operational mistakes to not repeat

These all cost real time in the last session.

- **Do not rebuild `./molly` while a test run is in flight.** It invalidated
  three separate runs before I noticed. `make test` recompiles the unit binary,
  so editing anything under `src/` or `tests/` mid-run counts as rebuilding.
- **Read the screen before writing the assertion.** Six of my first pty checks
  failed on my guesses rather than on molly: the gutter reads `1 │ alpha` and not
  four spaces, `Sel` is on the status row and not the message row, a selection of
  three lines counts 16 and not 19, and `a` occurs twice in `alpha` so "the next
  match" was never going to be the next line. One peek script would have saved
  all six.
- **`edit()` in the pty tests does not save.** It reads the file back off disk,
  so a key list without `SAVE` in it compares the original against itself and
  passes for the wrong reason.
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

Two more hooks, both documented in `MANUAL.md` and both cheaper than a pty:

```bash
MOLLY_DUMP_CONFIG=1 ./molly    # the parsed file, every setting, every warning
MOLLY_DUMP_KEYMAP=1 ./molly    # every binding, with [keys] already applied
MOLLY_CONFIG=/path/to/rc ./molly   # read that file instead; empty = read none
```

The test harness pins `MOLLY_CONFIG` empty, so the suite never reads your own
`mollyrc`. If you are debugging a config problem through the tests, set it
through `env=` rather than exporting it.

What to try first, since these are the new surfaces: **Ctrl-F** for the Find
dialog (Tab between field, the two checkboxes and the buttons, Space toggles),
**Alt-R** to read the query as a pattern, **Ctrl-Space** (or Alt-X) for the palette,
**Alt-F** and friends for the menus, **F1** for help, **Ctrl-A** / **Ctrl-L** /
**Ctrl-T** for the new CUA keys, **F3** / **Shift-F3** to walk matches, and
**View ▸ Save Settings** then a restart to watch a setting survive.
`MANUAL.md` has the regular-expression syntax table, what is not supported, what
a search matches against, and the configuration reference.
