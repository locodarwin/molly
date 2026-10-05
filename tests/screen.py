"""A small terminal emulator, enough of one to assert on what molly draws.

The pty harness in pty_harness.py returns the raw byte stream molly wrote, which
is fine for asserting that a particular escape sequence was emitted but poor at
answering "what does the screen look like?". That question matters more and more
as molly grows: a partial-refresh renderer only rewrites the lines it thinks
changed, so a bug shows up as a *stale* cell rather than as a wrong byte, and no
amount of grepping the stream will catch it.

So this feeds the stream through a grid the way a terminal would, and tests
compare the resulting screen against a stored snapshot.

Only what molly actually emits is implemented -- CUP, EL, ED, SGR for reverse
video and gray, and cursor show/hide. Anything else is consumed and ignored
rather than guessed at, so an unimplemented sequence shows up as a visibly wrong
snapshot instead of a silent misparse.

Erase-to-end-of-line deliberately fills with the *current* attributes, not with
plain cells: molly draws the status bar by turning on reverse video and then
erasing the rest of the line, so getting this wrong would un-highlight it.
"""

import unicodedata


def char_width(ch):
    """Display columns a character occupies, matching molly's olly_wcwidth."""
    if unicodedata.combining(ch):
        return 0
    if unicodedata.category(ch) in ("Mn", "Me", "Cf"):
        return 0
    if unicodedata.east_asian_width(ch) in ("W", "F"):
        return 2
    return 1


# A cell's attributes, as a single character so a whole screen of them can be
# compared and printed as text:
#   '.' plain      'R' reverse      'g' gray      'G' gray + reverse
def _attr_char(reverse, gray):
    if reverse and gray:
        return "G"
    if reverse:
        return "R"
    if gray:
        return "g"
    return "."


class Screen:
    """A rows x cols grid that `feed` drives with molly's output."""

    # Continuation cell of a double-width glyph; it holds no character of its own.
    WIDE_TAIL = "\0"

    def __init__(self, rows=24, cols=80):
        self.rows = rows
        self.cols = cols
        self.cy = 0
        self.cx = 0
        self.reverse = False
        self.gray = False
        self.cursor_visible = True
        self.unhandled = []
        self._clear_all()
        # incremental parser state
        self._pending = b""

    # -- grid helpers --------------------------------------------------------

    def _blank_row(self):
        return [" "] * self.cols

    def _blank_attrs(self):
        return ["."] * self.cols

    def _clear_all(self):
        self.grid = [self._blank_row() for _ in range(self.rows)]
        self.attrs = [self._blank_attrs() for _ in range(self.rows)]

    def resize(self, rows, cols):
        """Follow a pty resize. Content is dropped; molly repaints in full."""
        self.rows, self.cols = rows, cols
        self.cy = min(self.cy, rows - 1)
        self.cx = min(self.cx, cols - 1)
        self._clear_all()

    def _put(self, ch):
        w = char_width(ch)
        if self.cx >= self.cols:
            return  # molly never draws past the edge; drop rather than wrap
        if self.cy >= self.rows:
            return
        a = _attr_char(self.reverse, self.gray)
        if w == 0:
            # Combining mark: attach to the cell to the left, as a terminal does.
            x = self.cx - 1
            while x >= 0 and self.grid[self.cy][x] == self.WIDE_TAIL:
                x -= 1
            if x >= 0:
                self.grid[self.cy][x] += ch
            return
        self.grid[self.cy][self.cx] = ch
        self.attrs[self.cy][self.cx] = a
        self.cx += 1
        if w == 2 and self.cx < self.cols:
            self.grid[self.cy][self.cx] = self.WIDE_TAIL
            self.attrs[self.cy][self.cx] = a
            self.cx += 1

    # -- the parser ----------------------------------------------------------

    def feed(self, data):
        """Consume a chunk of output. Safe to call repeatedly."""
        if isinstance(data, bytes):
            # Hold back a trailing partial UTF-8 sequence or partial escape.
            data = self._pending + data
            self._pending = b""
            try:
                text = data.decode("utf-8")
            except UnicodeDecodeError as exc:
                text = data[: exc.start].decode("utf-8")
                self._pending = data[exc.start:]
        else:
            text = data

        i = 0
        n = len(text)
        while i < n:
            ch = text[i]
            if ch == "\x1b":
                consumed = self._escape(text, i)
                if consumed == 0:      # incomplete; wait for more output
                    self._pending = text[i:].encode("utf-8") + self._pending
                    return
                i += consumed
                continue
            if ch == "\r":
                self.cx = 0
            elif ch == "\n":
                self.cy = min(self.cy + 1, self.rows - 1)
            elif ch == "\b":
                self.cx = max(self.cx - 1, 0)
            elif ch == "\t":
                self.cx = min((self.cx // 8 + 1) * 8, self.cols - 1)
            elif ch < " ":
                pass               # other control bytes are not drawn
            else:
                self._put(ch)
            i += 1

    def _escape(self, text, i):
        """Handle the escape sequence at text[i]. Returns bytes consumed, or 0."""
        if i + 1 >= len(text):
            return 0
        if text[i + 1] != "[":
            # molly emits no non-CSI escapes to the screen; skip ESC + one byte.
            return 2
        j = i + 2
        while j < len(text) and not ("@" <= text[j] <= "~"):
            j += 1
        if j >= len(text):
            return 0               # final byte has not arrived yet
        body = text[i + 2:j]
        final = text[j]
        self._csi(body, final)
        return j - i + 1

    def _csi(self, body, final):
        private = body.startswith("?")
        if private:
            if final in ("h", "l") and body == "?25":
                self.cursor_visible = (final == "h")
            # bracketed paste (?2004) and friends: no effect on the grid
            return

        parts = [p for p in body.split(";")]
        nums = []
        for p in parts:
            try:
                nums.append(int(p) if p else 0)
            except ValueError:
                nums.append(0)

        if final == "H" or final == "f":
            row = (nums[0] if len(nums) > 0 and nums[0] else 1) - 1
            col = (nums[1] if len(nums) > 1 and nums[1] else 1) - 1
            self.cy = max(0, min(row, self.rows - 1))
            self.cx = max(0, min(col, self.cols - 1))
        elif final == "K":
            mode = nums[0] if nums else 0
            a = _attr_char(self.reverse, self.gray)
            if mode == 0:
                rng = range(self.cx, self.cols)
            elif mode == 1:
                rng = range(0, self.cx + 1)
            else:
                rng = range(0, self.cols)
            for x in rng:
                self.grid[self.cy][x] = " "
                self.attrs[self.cy][x] = a
        elif final == "J":
            mode = nums[0] if nums else 0
            if mode == 2:
                self._clear_all()
            elif mode == 0:
                for x in range(self.cx, self.cols):
                    self.grid[self.cy][x] = " "
                    self.attrs[self.cy][x] = "."
                for y in range(self.cy + 1, self.rows):
                    self.grid[y] = self._blank_row()
                    self.attrs[y] = self._blank_attrs()
        elif final == "m":
            if not nums:
                nums = [0]
            for v in nums:
                if v == 0:
                    self.reverse = False
                    self.gray = False
                elif v == 7:
                    self.reverse = True
                elif v == 27:
                    self.reverse = False
                elif v == 90:
                    self.gray = True
                elif v == 39:
                    self.gray = False
        elif final == "C":
            self.cx = min(self.cx + max(1, nums[0] if nums else 1), self.cols - 1)
        elif final == "D":
            self.cx = max(self.cx - max(1, nums[0] if nums else 1), 0)
        elif final == "A":
            self.cy = max(self.cy - max(1, nums[0] if nums else 1), 0)
        elif final == "B":
            self.cy = min(self.cy + max(1, nums[0] if nums else 1), self.rows - 1)
        elif final == "n":
            pass                   # cursor-position query; the pty answers it
        else:
            self.unhandled.append(body + final)

    # -- what tests look at --------------------------------------------------

    def text(self):
        """The screen as a list of row strings, trailing blanks removed."""
        out = []
        for row in self.grid:
            s = "".join(c for c in row if c != self.WIDE_TAIL)
            out.append(s.rstrip())
        return out

    def render(self):
        """The screen as one string, with a visible right margin."""
        return "\n".join(self.text())

    def attr_rows(self):
        """Per-cell attributes as row strings, trailing plain cells removed."""
        return ["".join(a).rstrip(".") for a in self.attrs]

    def snapshot(self):
        """Screen plus attributes, in the form the golden files store."""
        lines = ["--- screen ---"]
        lines += self.text()
        lines.append("--- attrs ---")
        lines += self.attr_rows()
        lines.append("--- cursor ---")
        lines.append("row %d col %d visible %d"
                     % (self.cy + 1, self.cx + 1, int(self.cursor_visible)))
        # Strip trailing blank lines so an empty tail cannot cause a diff.
        while len(lines) > 1 and lines[-1] == "":
            lines.pop()
        return "\n".join(lines) + "\n"
