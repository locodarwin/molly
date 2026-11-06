#!/usr/bin/env python3
"""Saving a file shared between users, tested with real, separate user IDs.

Run with:  make test          (or: python3 tests/shared_files.py ./molly)

The UIDs come from a private user namespace on a private tmpfs, so no privileges
are needed and nothing outside is touched. Where namespaces aren't available,
the checks are skipped, unless MOLLY_REQUIRE_NAMESPACES is set, as it is in CI, so
that they can't quietly stop running there.
"""

import os
import shutil
import signal
import stat
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pty_harness import run, run_until_signal

ROOT = "/mnt/molly"
ALICE, BOB, BOB_GROUP, TEAM, OUTSIDERS = 1000, 1001, 1001, 2000, 3000
SAVE = "\x13"
WAIT = 0.3
FAILURES = 0


def check(label, got, want):
    global FAILURES
    if got == want:
        print("  pass  %s" % label)
    else:
        FAILURES += 1
        print("  FAIL  %s\n          want: %r\n          got:  %r" % (label, want, got))


def owner(path):
    st = os.stat(path)
    return (st.st_uid, st.st_gid, stat.S_IMODE(st.st_mode))


def read(path):
    with open(path) as fh:
        return fh.read()


def make(path, uid, gid, mode):
    with open(path, "w") as fh:
        fh.write("data\n")
    os.chown(path, uid, gid)
    os.chmod(path, mode)
    return path


def scenarios(molly_source):
    subprocess.run(["mount", "-t", "tmpfs", "tmpfs", "/mnt"], check=True)
    os.chmod("/mnt", 0o755)
    os.makedirs(ROOT)
    os.chmod(ROOT, 0o755)
    molly = os.path.join(ROOT, "molly")
    shutil.copy(molly_source, molly)
    os.chmod(molly, 0o755)

    home = os.path.join(ROOT, "bob")
    os.makedirs(home, mode=0o700)
    os.chown(home, BOB, BOB_GROUP)
    env = {"HOME": home, "XDG_STATE_HOME": os.path.join(home, "state")}
    # Under a sanitizer, reports must go somewhere the other user can write.
    san_logs = None
    if "ASAN_OPTIONS" in os.environ:
        san_logs = os.path.join(ROOT, "sanitizer")
        os.makedirs(san_logs)
        os.chmod(san_logs, 0o1777)
        env["ASAN_OPTIONS"] = "detect_leaks=0:log_path=%s/asan" % san_logs
        env["UBSAN_OPTIONS"] = "print_stacktrace=1:log_path=%s/ubsan" % san_logs
    bob = ["/usr/bin/setpriv", "--reuid=%d" % BOB, "--regid=%d" % BOB_GROUP,
           "--groups=%d" % TEAM, molly]

    shared = os.path.join(ROOT, "shared")
    os.makedirs(shared)
    os.chown(shared, ALICE, TEAM)
    os.chmod(shared, 0o775)

    print("a teammate's group-writable file")
    p = make(os.path.join(shared, "team.txt"), ALICE, TEAM, 0o660)
    ino = os.stat(p).st_ino
    out = run(bob + [p], ["X", SAVE, WAIT], env=env)
    check("the edit is saved", read(p), "Xdata\n")
    check("owner, group and mode are unchanged", owner(p), (ALICE, TEAM, 0o660))
    check("it stays the same file", os.stat(p).st_ino, ino)
    check("the in-place write is reported", b"in place" in out, True)

    print("someone else's file we may not write, in a directory we may")
    p = make(os.path.join(shared, "alice.txt"), ALICE, TEAM, 0o644)
    # It opens read-only, so turn that off first: that is what the rename used to
    # let through, and being told it was read-only was the only thing in the way.
    writable = ["\x00", "read only", "\r", WAIT]
    out = run(bob + [p], writable + ["X", SAVE, WAIT, "\x1b", WAIT], env=env)
    check("the file is not replaced", read(p), "data\n")
    check("owner, group and mode are unchanged", owner(p), (ALICE, TEAM, 0o644))
    check("the refusal says why", b"Permission denied" in out, True)
    copy = os.path.join(home, "mine.txt")
    run(bob + [p], writable + ["X", SAVE, WAIT, copy, "\r", WAIT], env=env)
    check("the work can be saved elsewhere", read(copy), "Xdata\n")
    check("while the original stays as it was", read(p), "data\n")

    print("a group-writable file in a sticky directory")
    sticky = os.path.join(ROOT, "sticky")
    os.makedirs(sticky)
    os.chown(sticky, ALICE, TEAM)
    os.chmod(sticky, 0o1775)
    p = make(os.path.join(sticky, "team.txt"), ALICE, TEAM, 0o660)
    run(bob + [p], ["X", SAVE, WAIT], env=env)
    check("the edit is saved", read(p), "Xdata\n")
    check("owner, group and mode are unchanged", owner(p), (ALICE, TEAM, 0o660))

    print("our own file, in a group we belong to")
    p = make(os.path.join(shared, "own.txt"), BOB, TEAM, 0o640)
    ino = os.stat(p).st_ino
    out = run(bob + [p], ["X", SAVE, WAIT], env=env)
    check("the edit is saved", read(p), "Xdata\n")
    check("owner, group and mode are unchanged", owner(p), (BOB, TEAM, 0o640))
    check("the save is still atomic", os.stat(p).st_ino != ino, True)
    check("with no in-place warning", b"in place" in out, False)

    print("someone else's recovery file beside a file we may edit")
    p = make(os.path.join(shared, "notes.txt"), ALICE, TEAM, 0o660)
    rec = p + ".molly-recover"
    with open(rec, "w") as fh:
        fh.write("planted\n")
    os.chown(rec, ALICE, TEAM)
    os.chmod(rec, 0o644)
    out = run(bob + [p], ["r", WAIT, "\x11", WAIT, "\x11", WAIT], env=env)
    check("it is not offered as ours", b"crashed session" in out, False)
    check("its text does not reach the buffer", b"planted" in out, False)
    run(bob + [p], ["\x1b[B", WAIT, "\x11", WAIT], env=env)
    check("and quitting does not delete it", os.path.exists(rec), True)

    print("someone else's writable recovery file, when we crash")
    p = make(os.path.join(shared, "plans.txt"), ALICE, TEAM, 0o660)
    rec = p + ".molly-recover"
    with open(rec, "w") as fh:
        fh.write("alice's\n")
    os.chown(rec, ALICE, TEAM)
    os.chmod(rec, 0o666)
    run_until_signal(bob + [p], signal.SIGTERM, keys=["SECRET"])
    check("our unsaved text is not written into it", read(rec), "alice's\n")
    ours = "%s.%d" % (rec, BOB)
    check("it goes to a recovery file of our own beside it",
          os.path.exists(ours) and read(ours), "SECRETdata\n")
    check("which only we can read", os.path.exists(ours) and owner(ours)[::2],
          (BOB, 0o600))
    out = run(bob + [p], ["r", WAIT, SAVE, WAIT], env=env)
    check("it is offered the next time", b"crashed session" in out, True)
    check("and restored", read(p), "SECRETdata\n")
    check("saving removes ours", os.path.exists(ours), False)
    check("and leaves theirs", read(rec), "alice's\n")

    print("our own file, in a group we don't belong to")
    p = make(os.path.join(shared, "theirs.txt"), BOB, OUTSIDERS, 0o640)
    run(bob + [p], ["X", SAVE, WAIT], env=env)
    check("the edit is saved", read(p), "Xdata\n")
    check("the group is unchanged", owner(p), (BOB, OUTSIDERS, 0o640))

    if san_logs:
        reports = os.listdir(san_logs)
        check("no sanitizer reports", reports, [])
        for name in reports:
            print(read(os.path.join(san_logs, name)))

    print("\n%d failed" % FAILURES)
    return 1 if FAILURES else 0


def main():
    molly = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./molly")
    if os.environ.get("MOLLY_IN_NAMESPACE"):
        return scenarios(molly)

    wrap = ["unshare", "--map-auto", "--map-root-user", "--mount"]
    try:
        probe = subprocess.run(wrap + ["mount", "-t", "tmpfs", "tmpfs", "/mnt"],
                               capture_output=True, text=True)
        problem = probe.stderr.strip() if probe.returncode != 0 else None
    except OSError as err:
        problem = str(err)
    if problem is None and not os.path.exists("/usr/bin/setpriv"):
        problem = "no /usr/bin/setpriv"
    if problem is not None:
        print("shared files: no user namespace with several UIDs here: %s" % problem)
        if os.environ.get("MOLLY_REQUIRE_NAMESPACES"):
            print("shared files: FAILED, MOLLY_REQUIRE_NAMESPACES is set")
            return 1
        print("shared files: skipped")
        return 0
    return subprocess.run(wrap + [sys.executable, os.path.abspath(__file__), molly],
                          env=dict(os.environ, MOLLY_IN_NAMESPACE="1")).returncode


if __name__ == "__main__":
    sys.exit(main())
