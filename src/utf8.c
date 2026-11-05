/* utf8.c -- UTF-8 decoding and display width. No editor state; pure functions over bytes. */

#include "molly.h"

/* --- UTF-8: the buffer holds raw bytes, but the cursor moves and deletes by
 * whole characters, so editing never splits a multibyte sequence and leaves
 * invalid bytes behind. On-screen column width for wide and combining glyphs
 * is modeled too (see molly_wcwidth below), so the cursor, horizontal scroll
 * and the row renderer all agree on where a character lands. --- */

int utf8_is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

/* Bytes in the character starting at index i, validated: anything that is not
 * well-formed UTF-8 -- a missing or wrong continuation byte, an overlong form, a
 * surrogate, a code point past U+10FFFF -- is treated as a single byte. */
int utf8_char_bytes(const char *s, int size, int i) {
  if (i < 0 || i >= size) return 1;
  unsigned char c = (unsigned char)s[i];
  int n;
  if (c >= 0xF5) return 1;
  else if (c >= 0xF0) n = 4;
  else if (c >= 0xE0) n = 3;
  else if (c >= 0xC2) n = 2;
  else return 1;
  if (i + n > size) return 1;
  int k;
  for (k = 1; k < n; k++)
    if (!utf8_is_cont((unsigned char)s[i + k])) return 1;
  unsigned char d = (unsigned char)s[i + 1];
  if ((c == 0xE0 && d < 0xA0) || (c == 0xED && d > 0x9F) ||
      (c == 0xF0 && d < 0x90) || (c == 0xF4 && d > 0x8F)) return 1;
  return n;
}

/* Whether these bytes may be written to a terminal as they stand: printable ASCII
 * and well-formed UTF-8 other than the C1 controls. Everything else in a file is
 * somebody else's escape sequence. ESC can set the title or the clipboard, make the
 * terminal answer into molly's input, or repaint the screen; C1's CSI does the same
 * in one character, and a lone 0x9B is CSI itself on a terminal not in UTF-8 mode. */
int utf8_safe(const char *s, int nbytes) {
  int i = 0;
  while (i < nbytes) {
    unsigned char c = (unsigned char)s[i];
    int n = utf8_char_bytes(s, nbytes, i);
    if (n == 1 && (c < 0x20 || c >= 0x7F)) return 0;
    if (c == 0xC2 && (unsigned char)s[i + 1] < 0xA0) return 0;
    i += n;
  }
  return 1;
}

/* Start index of the character before index i. */
int utf8_prev(const char *s, int i) {
  if (i <= 0) return 0;
  int j = i - 1;
  while (j > 0 && utf8_is_cont((unsigned char)s[j])) j--;
  return j;
}

/* Decode the character at index i to a code point, and report its byte length.
 * An invalid or truncated sequence decodes as its single lead byte. */
static uint32_t utf8_decode(const char *s, int size, int i, int *nbytes) {
  int n = utf8_char_bytes(s, size, i);
  *nbytes = n;
  unsigned char c = (unsigned char)s[i];
  if (n == 1) return c;
  uint32_t cp = c & (0x7F >> n);
  int k;
  for (k = 1; k < n; k++)
    cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
  return cp;
}

/* --- display width: how many terminal columns a code point occupies. Built
 * in rather than via wcwidth(), so it does not depend on the process locale
 * and the editor stays self-contained. Zero for combining marks, two for the
 * East Asian wide and fullwidth ranges, one otherwise. The interval tables
 * follow Markus Kuhn's reference wcwidth (public domain). --- */

struct interval { uint32_t first, last; };

static int in_table(uint32_t cp, const struct interval *t, int n) {
  int lo = 0, hi = n - 1;
  if (cp < t[0].first || cp > t[hi].last) return 0;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (cp > t[mid].last) lo = mid + 1;
    else if (cp < t[mid].first) hi = mid - 1;
    else return 1;
  }
  return 0;
}

static int molly_wcwidth(uint32_t cp) {
  static const struct interval zero[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x0610, 0x061A},
    {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4},
    {0x0711, 0x0711}, {0x0730, 0x074A}, {0x07A6, 0x07B0}, {0x07EB, 0x07F3},
    {0x0816, 0x0823}, {0x0900, 0x0903}, {0x093A, 0x094F}, {0x0951, 0x0957},
    {0x0E31, 0x0E31}, {0x0E34, 0x0E3A}, {0x0EB1, 0x0EB1}, {0x0EB4, 0x0EBC},
    {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x20D0, 0x20F0}, {0xFE20, 0xFE2F},
    {0x1F3FB, 0x1F3FF}, {0xE0100, 0xE01EF}
  };
  static const struct interval wide[] = {
    {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC},
    {0x25FD, 0x25FE}, {0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F},
    {0x2693, 0x2693}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE},
    {0x2753, 0x2755}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2E80, 0x303E},
    {0x3041, 0x33FF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xA000, 0xA4CF},
    {0xAC00, 0xD7A3}, {0xF900, 0xFAFF}, {0xFE10, 0xFE19}, {0xFE30, 0xFE6F},
    {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6}, {0x1F300, 0x1F64F}, {0x1F900, 0x1F9FF},
    {0x20000, 0x3FFFD}
  };
  if (cp == 0) return 0;
  if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 1;  /* control: 1 cell */
  if (in_table(cp, zero, (int)(sizeof(zero) / sizeof(zero[0])))) return 0;
  if (in_table(cp, wide, (int)(sizeof(wide) / sizeof(wide[0])))) return 2;
  return 1;
}

/* Display columns of one code point, for a caller that has decoded it already:
 * the integrated terminal, whose input arrives as a stream rather than a row. */
int utf8_cp_cols(uint32_t cp) { return molly_wcwidth(cp); }

/* A code point as UTF-8 into dst, which has room for four bytes. Returns the
 * bytes written. */
int utf8_encode(uint32_t cp, char *dst) {
  if (cp < 0x80) { dst[0] = (char)cp; return 1; }
  if (cp < 0x800) {
    dst[0] = (char)(0xC0 | (cp >> 6));
    dst[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    dst[0] = (char)(0xE0 | (cp >> 12));
    dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    dst[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  dst[0] = (char)(0xF0 | (cp >> 18));
  dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  dst[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

/* Display columns of the character at index i (tabs are the caller's job). */
int utf8_char_cols(const char *s, int size, int i, int *nbytes) {
  uint32_t cp = utf8_decode(s, size, i, nbytes);
  /* A NUL or a byte that is no character draws as one '?' (see grid_put_char),
   * whatever the table would say about the code point of the same number. */
  if (*nbytes == 1 && (cp == 0 || cp >= 0x80)) return 1;
  return molly_wcwidth(cp);
}

/* Display columns a whole string occupies.
 *
 * Not strlen: "Find…" is seven bytes and five columns, and a menu sized with
 * the byte count comes out two columns too wide -- or misaligns outright once a
 * mnemonic sits after a multibyte character. Anything laying out a box around text
 * measures it with this. */
int utf8_str_cols(const char *s) {
  int i = 0, cols = 0, len = (int)strlen(s);
  while (i < len) {
    int nb;
    cols += utf8_char_cols(s, len, i, &nb);
    i += nb;
  }
  return cols;
}

/* The byte offset of the character at display column `col`, or the string's length
 * if it is shorter than that. The inverse of the walk above, for finding the one
 * character a mnemonic underlines. */
/* Bytes of `s` that fit within `cols` display columns, never exceeding it.
 *
 * The difference from utf8_col_to_byte, which is what a caller wanting to
 * *truncate* needs: that one stops at or just past a column, so a wide glyph
 * straddling the limit is included and lands one column outside whatever was
 * being fitted. This one leaves it out. `*out_cols` receives the columns the
 * returned bytes actually occupy, which is one less than `cols` when a
 * double-width character was the thing that would not fit; a caller that
 * measures anything against the truncated text wants that rather than `cols`. */
int utf8_fit_cols(const char *s, int cols, int *out_cols) {
  int i = 0, used = 0, len = (int)strlen(s);
  while (i < len) {
    int nb;
    int w = utf8_char_cols(s, len, i, &nb);
    if (used + w > cols) break;
    used += w;
    i += nb;
  }
  if (out_cols != NULL) *out_cols = used;
  return i;
}

int utf8_col_to_byte(const char *s, int col) {
  int i = 0, cols = 0, len = (int)strlen(s);
  while (i < len && cols < col) {
    int nb;
    cols += utf8_char_cols(s, len, i, &nb);
    i += nb;
  }
  return i;
}
