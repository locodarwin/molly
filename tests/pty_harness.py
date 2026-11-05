"""Drive molly inside a real pty and capture what it writes.

Molly only runs on a terminal (it calls tcgetattr on stdin at startup), so the
tests give it one rather than a pipe.
"""

import fcntl
import os
import pty
import re
import select
import signal
import struct
import sys
import termios
import time


def run(argv, keys, rows=24, cols=80, settle=0.4, env=None, status=False,
        exit_wait=0.0):
    """Start argv under a pty, send `keys`, return everything it printed.

    A float in `keys` is treated as a pause in seconds rather than input. A
    (rows, cols) tuple resizes the pty in place -- the kernel then raises
    SIGWINCH in the child, so a resize can be exercised mid-session. With
    status=True, return (output, wait status) instead. A program still running
    at the end is killed, so its status reports SIGKILL.
    """
    pid, fd = pty.fork()
    if pid == 0:
        # TERM and COLORTERM are both pinned, not just TERM: molly picks its color
        # depth from COLORTERM first, so inheriting it from whatever shell ran the
        # tests made the golden screens depend on the ambient environment -- they
        # came out sixteen-color on one machine and 256-color on the next. A test
        # that wants a depth asks for it through `env`.
        # MOLLY_CONFIG is pinned empty for the same reason: molly reads
        # ~/.config/molly/mollyrc at startup, so without this every test would
        # depend on the configuration of the machine it ran on -- a developer
        # with `line numbers = on` would see three dozen failures that nobody
        # else could reproduce. Empty means "read no configuration file"; a test
        # that wants one points MOLLY_CONFIG at it through `env`.
        # MOLLY_LSP is pinned off for the same reason: C files open in dozens of
        # tests, and on a machine with clangd installed each would start one --
        # and draw its diagnostics into screens that a machine without it
        # does not have. A test of the client asks for it back through `env`.
        # MOLLY_STATE likewise: the recent-files list is state molly writes as
        # it works, and a test run must neither read a person's list nor add
        # to it.
        environ = dict(os.environ, TERM="xterm", COLORTERM="", MOLLY_CONFIG="",
                       MOLLY_LSP="off", MOLLY_STATE="")
        if env:
            environ.update(env)
        os.execve(argv[0], argv, environ)

    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    out = b""

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

    # Wait for molly's opening frame -- not for a fixed interval, and not merely
    # for its first byte.
    #
    # It enters raw mode with TCSAFLUSH, which throws away input that arrived
    # before it got there, so typing on a timer races the start: on a loaded CI
    # runner molly took longer than the 0.3s this used to sleep and the first
    # keystrokes vanished. The first byte molly writes ("\x1b[?2004h", from
    # enable_raw_mode) comes just after that tcsetattr, so its arrival does
    # prove the flush is behind us -- but it also comes *before* molly has
    # loaded the file, applied its configuration or painted anything, and a
    # molly still inside startup is silent in exactly the way a molly with
    # nothing left to say is. The wait below then read that silence as "done",
    # the kill came, and every keystroke went with it.
    #
    # That is the whole of the "lost every keystroke" flake: five runs of CI
    # over three days, never once on a developer's machine, where startup is ten
    # milliseconds. It reproduces in a second against a stand-in that writes one
    # byte and then stays quiet for longer than `settle` -- see the note in
    # HANDOFF.md.
    #
    # So: the first byte, and then a frame. "Stopped growing" cannot tell the
    # two apart -- the gap between them is exactly what a finished program looks
    # like -- so this waits for something only a paint produces: molly puts the
    # cursor somewhere on every frame, and nothing it writes before one does.
    # If no frame arrives it carries on anyway rather than raising, because the
    # guard after the keys is the net for that, and a slow start is not a bug.
    reaped = None
    deadline = time.time() + 5.0
    while not out and time.time() < deadline:
        drain(0.1)
    if not out:
        os.kill(pid, signal.SIGKILL)
        _, wstatus = os.waitpid(pid, 0)
        raise RuntimeError("%s painted nothing in 5s (wait status %d)"
                           % (argv[0], wstatus))
    while time.time() < deadline and not re.search(rb"\x1b\[[0-9;]*H", out):
        drain(0.05)
        # Not everything molly is run as paints: `--version` prints and exits,
        # and so does a refusal to open a directory. Those have said all they
        # have to say, and the wait above would otherwise sit out its deadline
        # for each of them.
        done, st = os.waitpid(pid, os.WNOHANG)
        if done:
            reaped = st
            drain(0.05)
            break

    # Whether anything was actually sent, and how much had been printed before
    # the most recent key went out -- which is what tells "it answered" from
    # "it has not got there yet" below. Taken before the write and not after
    # the loop, because molly usually answers inside the 20ms drain that
    # follows a key, and that answer has to count as one.
    typed = any(isinstance(k, (str, bytes)) and k for k in keys)
    mark = len(out)

    for key in keys:
        if isinstance(key, float):
            time.sleep(key)
            continue
        if isinstance(key, tuple):
            r, c = key
            fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", r, c, 0, 0))
            # molly only notices a SIGWINCH on its ~100ms poll, so give it time
            # to observe the change and repaint before the next input.
            time.sleep(0.25)
            drain(0.15)
            continue
        try:
            mark = len(out)
            os.write(fd, key if isinstance(key, bytes) else key.encode())
        except OSError:
            break  # the program has already exited
        time.sleep(0.06)
        drain(0.02)

    # Wait for molly to go quiet rather than for a fixed interval. `settle` is
    # the floor -- a test that wants a particular frame needs that long -- and
    # past it the wait continues while output is still arriving, because a key
    # that has been written is not a key that has been read: killing molly
    # mid-repaint can kill it before the Ctrl-S the test was about, and the
    # check then compares a file nobody saved against the one it wrote. The cap
    # is what stops a molly that will not stop talking from stalling the suite.
    #
    # And quiet only counts once something has come back *since the last key*.
    # Without that, a stall anywhere in the session is the startup window
    # again: no output is not evidence of nothing to do. Sessions that send no
    # keys at all -- most of the golden screens -- have nothing to wait for and
    # skip the condition.
    floor, cap = time.time() + settle, time.time() + settle + 3.0
    gave_up = reaped is None
    while reaped is None and time.time() < cap:
        before = len(out)
        drain(0.15)
        # A program that has exited has nothing left to say, which is the one
        # kind of silence that needs no patience: Ctrl-Q ends the session, and
        # waiting out the cap for a frame that cannot come would cost every
        # quitting test three and a half seconds and warn about each of them.
        done, st = os.waitpid(pid, os.WNOHANG)
        if done:
            reaped = st
            drain(0.05)   # whatever it wrote on the way out
            gave_up = False
            break
        if (len(out) == before and time.time() >= floor
                and (len(out) > mark or not typed)):
            gave_up = False
            break
    # A session that was typed at and never answered is the shape of the bug
    # above, one stall later. It is not always wrong -- a last key that changes
    # nothing on screen prints nothing -- so this says so rather than raising,
    # and a silent kill stops being indistinguishable from a passing test.
    if gave_up and typed and len(out) == mark:
        sys.stderr.write(
            "pty_harness: %s said nothing for %.1fs after the last key, so what"
            " follows judges a session it may never have read (keys: %r)\n"
            % (os.path.basename(argv[0]), settle + 3.0, keys))

    # For a session whose whole point is that it ends: the quit may simply not have
    # been reached yet, on a loaded runner with a save and its fsync in front of it,
    # and the kill below would then be reported as the program failing to quit.
    deadline = time.time() + exit_wait
    while reaped is None and time.time() < deadline:
        drain(0.05)
        done, st = os.waitpid(pid, os.WNOHANG)
        if done:
            reaped = st
            drain(0.05)

    if reaped is None:
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass
        _, wstatus = os.waitpid(pid, 0)
    else:
        wstatus = reaped
    try:
        os.close(fd)
    except OSError:
        pass
    return (out, wstatus) if status else out


def run_until_signal(argv, sig, rows=24, cols=80, startup=0.5, keys=()):
    """Start argv on a pty, let it paint, optionally type `keys`, then `sig`.

    Returns (modes before start, modes while running, modes after exit, wait
    status, output), each set of modes as termios.tcgetattr reports them. The
    slave end stays open here so the terminal can still be inspected once the
    program has gone -- its modes then are what the user is left with. The output
    is kept because the modes are only half of what the user is left with: the
    other half is the escape sequences the program wrote on its way out.
    """
    master, slave = os.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    before = termios.tcgetattr(slave)
    pid = os.fork()
    if pid == 0:
        try:
            os.close(master)
            os.setsid()
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
            for fd in (0, 1, 2):
                os.dup2(slave, fd)
            if slave > 2:
                os.close(slave)
            os.execve(argv[0], argv, dict(os.environ, TERM="xterm", COLORTERM="",
                                          MOLLY_LSP="off", MOLLY_STATE=""))
        finally:
            os._exit(127)

    out = b""

    def drain(timeout):
        nonlocal out
        while select.select([master], [], [], timeout)[0]:
            try:
                chunk = os.read(master, 1 << 20)
            except OSError:
                return
            if not chunk:
                return
            out += chunk

    # `startup` is the floor, and then a frame -- the same race run() has, and
    # the same answer: a program still starting up has not read anything yet,
    # and the recovery tests here type text and then expect to find it. See the
    # long note in run().
    deadline = time.time() + startup
    while time.time() < deadline:
        drain(0.05)
    deadline = time.time() + 5.0
    while time.time() < deadline and not re.search(rb"\x1b\[[0-9;]*H", out):
        drain(0.05)
    for key in keys:
        mark = len(out)
        os.write(master, key if isinstance(key, bytes) else key.encode())
        time.sleep(0.06)
        drain(0.02)
        # And let it answer before the next key or the signal: what this
        # function proves is what the program did with the keys, which it
        # cannot have done while it was still being handed them.
        deadline = time.time() + 2.0
        while len(out) == mark and time.time() < deadline:
            drain(0.05)
    during = termios.tcgetattr(slave)
    os.kill(pid, sig)

    deadline = time.time() + 3
    while True:
        drain(0.05)
        done, wstatus = os.waitpid(pid, os.WNOHANG)
        if done:
            break
        if time.time() > deadline:
            os.kill(pid, signal.SIGKILL)
            _, wstatus = os.waitpid(pid, 0)
            break
    after = termios.tcgetattr(slave)
    os.close(master)
    os.close(slave)
    return before, during, after, wstatus, out


def cursor_col(out):
    """The column of the last cursor-position escape the program emitted.

    Each frame ends by moving the hardware cursor with \x1b[<row>;<col>H, so
    the final such escape in the output is where the cursor came to rest.
    """
    import re

    text = out.decode("latin-1")
    matches = re.findall(r"\x1b\[(\d+);(\d+)H", text)
    return int(matches[-1][1]) if matches else None


def status_lines(out, rows=24, cols=80):
    """The message-bar texts molly painted, oldest first.

    Read off the rendered screen rather than matched in the byte stream. This
    used to look for "position to the message row, erase the line, then the
    text", which is how olly happened to emit it; the cell-grid renderer writes
    the text and then clears the tail, and a future renderer may do something
    else again. What the row *said* is the thing these tests care about.

    Molly hides the cursor at the start of every frame, so the stream splits into
    frames on that, and the message row is sampled once per frame to recover the
    sequence of messages rather than only the last one.
    """
    from screen import Screen

    scr = Screen(rows, cols)
    seen = []
    frames = out.split(b"\x1b[?25l")
    for i, frame in enumerate(frames):
        scr.feed((b"\x1b[?25l" if i else b"") + frame)
        row = scr.text()[rows - 1] if len(scr.text()) >= rows else ""
        if row.strip() and (not seen or seen[-1] != row):
            seen.append(row)
    return seen
