"""Drive molly inside a real pty and capture what it writes.

Molly only runs on a terminal (it calls tcgetattr on stdin at startup), so the
tests give it one rather than a pipe.
"""

import fcntl
import os
import pty
import select
import signal
import struct
import termios
import time


def run(argv, keys, rows=24, cols=80, settle=0.4, env=None, status=False):
    """Start argv under a pty, send `keys`, return everything it printed.

    A float in `keys` is treated as a pause in seconds rather than input. A
    (rows, cols) tuple resizes the pty in place -- the kernel then raises
    SIGWINCH in the child, so a resize can be exercised mid-session. With
    status=True, return (output, wait status) instead. A program still running
    at the end is killed, so its status reports SIGKILL.
    """
    pid, fd = pty.fork()
    if pid == 0:
        # TERM and COLORTERM are both pinned, not just TERM: molly picks its colour
        # depth from COLORTERM first, so inheriting it from whatever shell ran the
        # tests made the golden screens depend on the ambient environment -- they
        # came out sixteen-colour on one machine and 256-colour on the next. A test
        # that wants a depth asks for it through `env`.
        # MOLLY_CONFIG is pinned empty for the same reason: molly reads
        # ~/.config/molly/mollyrc at startup, so without this every test would
        # depend on the configuration of the machine it ran on -- a developer
        # with `line numbers = on` would see three dozen failures that nobody
        # else could reproduce. Empty means "read no configuration file"; a test
        # that wants one points MOLLY_CONFIG at it through `env`.
        environ = dict(os.environ, TERM="xterm", COLORTERM="", MOLLY_CONFIG="")
        if env:
            environ.update(env)
        os.execve(argv[0], argv, environ)

    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    out = b""
    time.sleep(0.3)  # let the first repaint land

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
            os.write(fd, key if isinstance(key, bytes) else key.encode())
        except OSError:
            break  # the program has already exited
        time.sleep(0.06)
        drain(0.02)

    deadline = time.time() + settle
    while time.time() < deadline:
        drain(0.1)

    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass
    _, wstatus = os.waitpid(pid, 0)
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
            os.execve(argv[0], argv, dict(os.environ, TERM="xterm", COLORTERM=""))
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

    deadline = time.time() + startup
    while time.time() < deadline:
        drain(0.05)
    for key in keys:
        os.write(master, key if isinstance(key, bytes) else key.encode())
        time.sleep(0.06)
        drain(0.02)
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
