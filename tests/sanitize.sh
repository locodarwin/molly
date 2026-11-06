#!/bin/sh
# The unit checks, and the pty checks of what a file, a paste or another user can
# reach, against a sanitizer build, failing on any report.
#
# Reports go to files because the binary's stderr is the test's pty: a report
# there only counts if some check happens to notice the crash. Leak checking is
# off because molly leaves on purpose by re-raising signals and being killed by
# the test harness, neither of which frees anything.
#
# Not the whole pty suite, which takes an hour under a sanitizer. To run all of it:
# ASAN_OPTIONS=detect_leaks=0 python3 tests/test_molly.py ./molly_asan
set -u
binary=$1
units=$2
logs=$(mktemp -d)
trap 'rm -rf "$logs"' EXIT
ASAN_OPTIONS="detect_leaks=0:log_path=$logs/asan"
UBSAN_OPTIONS="print_stacktrace=1:log_path=$logs/ubsan"
export ASAN_OPTIONS UBSAN_OPTIONS

status=0
"$units" </dev/null || status=1
python3 - "$binary" <<'PY' || status=1
import os, shutil, sys, tempfile
sys.path.insert(0, "tests")
import test_molly as t
t.MOLLY = os.path.abspath(sys.argv[1])
d = tempfile.mkdtemp()
try:
    for name in ("test_control_bytes", "test_stray_bytes", "test_only_regular_files",
                 "test_save_keeps_the_file", "test_line_endings", "test_line_endings_per_line", "test_paste",
                 "test_signal_in_a_prompt", "test_save", "test_terminal", "test_lsp_snippets"):
        getattr(t, name)(d)
finally:
    shutil.rmtree(d, ignore_errors=True)
print("%d passed, %d failed" % (t.PASSED, len(t.FAILURES)))
sys.exit(1 if t.FAILURES else 0)
PY
python3 tests/shared_files.py "$binary" || status=1
for report in "$logs"/*; do
  [ -e "$report" ] || continue
  cat "$report"
  status=1
done
exit "$status"
