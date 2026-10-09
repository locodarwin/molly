/* config.c -- the configuration file: an INI parser, the settings table, and
 * writing the file back out.
 *
 * Olly's rule was that there is no configuration. That is the right rule for a
 * thousand-line editor with no menus and it is the wrong one here: a menu-driven
 * editor needs an Options menu, and an Options dialog that cannot persist what it
 * changed is a dialog that lies. So configuration is required rather than
 * optional, and it has to be *written* as well as read -- which is why the file
 * is INI-style sections of `key = value` and not something richer. Writing back
 * JSON or a tree would mean a serializer; writing back this is a printf per line.
 *
 * Three things live here, in the order a run uses them:
 *
 *   - The parser. Sections, `key = value`, `#` and `;` comments. It never dies:
 *     a bad line is collected as a warning and the rest of the file is read. A
 *     config file is user input, unlike the built-in keymap in cmd.c, which does
 *     die on a bad entry because that is a programming error.
 *
 *   - The settings table. One record per setting, with its type, its bounds, its
 *     default, how to push it into the editor and how to read it back out. The
 *     table exists for the same reason cmd.c's command table does: the Options
 *     dialogs, the written file and the dump hook are all that one table viewed
 *     three ways, so adding a setting is a record rather than three edits in
 *     three places.
 *
 *   - Write-back. Atomic (temp file, rename), and it rewrites the file whole.
 *     That cost is real and is stated in MANUAL.md: comments and settings molly
 *     does not know about do not survive a write. Preserving them needs a parser
 *     that keeps the original lines, which is worth building when there is an
 *     Options dialog to justify it and not before.
 *
 * Parsing is deliberately separate from applying. config_load() reads the file
 * into the table and touches nothing; config_apply() pushes it into the editor.
 * That split is what lets the dump hooks run before the terminal is in raw mode,
 * it is what an Options dialog needs (edit values, apply on OK, discard on
 * Cancel), and it keeps the terminal writes that `mouse` performs out of a unit
 * test's stdout. */

#include "molly.h"

/* ---- where the file lives -------------------------------------------------- */

/* `~/.config/molly/mollyrc`, honouring XDG_CONFIG_HOME, and overridable by
 * MOLLY_CONFIG for tests (documented in MANUAL.md). MOLLY_CONFIG set to the
 * empty string means "no configuration file at all", which is what the test
 * harness pins it to: without that, every pty test would read whatever is in the
 * developer's real config and the golden screens would depend on the machine
 * they ran on -- the same trap TERM and COLORTERM are pinned against.
 *
 * Returns NULL when there is no path to speak of: MOLLY_CONFIG empty, or no HOME
 * in the environment. */
const char *config_path(void) {
  static char path[4096];
  static int resolved;
  static int have;

  if (resolved) return have ? path : NULL;
  resolved = 1;

  const char *override = getenv("MOLLY_CONFIG");
  if (override != NULL) {
    if (*override == '\0') return NULL;
    snprintf(path, sizeof(path), "%s", override);
    have = 1;
    return path;
  }

  const char *xdg = getenv("XDG_CONFIG_HOME");
  if (xdg != NULL && *xdg != '\0') {
    snprintf(path, sizeof(path), "%s/molly/mollyrc", xdg);
    have = 1;
    return path;
  }

  const char *home = getenv("HOME");
  if (home == NULL || *home == '\0') return NULL;
  snprintf(path, sizeof(path), "%s/.config/molly/mollyrc", home);
  have = 1;
  return path;
}

/* ---- warnings -------------------------------------------------------------- */

/* A config file is user input, so a mistake in it is reported rather than fatal.
 * The cap is on what is *kept*: a file full of errors should not be able to make
 * molly allocate per line, and the count is reported in full regardless, so
 * "mollyrc: 40 problems" is still honest when only the first few are listed. */
#define CONFIG_MAX_ERRORS 16

static char *errors[CONFIG_MAX_ERRORS];
static int errors_kept;
static int errors_seen;

static void config_error(const char *fmt, ...) {
  errors_seen++;
  if (errors_kept >= CONFIG_MAX_ERRORS) return;
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  errors[errors_kept++] = xstrdup(buf);
}

/* Format into a fixed buffer, truncating if it does not fit.
 *
 * Through vsnprintf rather than snprintf on purpose: every caller is formatting a
 * path into a message, a path can be longer than the message buffer, and a
 * truncated warning is the right answer. snprintf would have the compiler point
 * out that the truncation can happen, which is true and is not news. */
static void msgf(char *dst, size_t dsz, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(dst, dsz, fmt, ap);
  va_end(ap);
}

static void config_errors_clear(void) {
  int i;
  for (i = 0; i < errors_kept; i++) {
    free(errors[i]);
    errors[i] = NULL;
  }
  errors_kept = 0;
  errors_seen = 0;
}

int config_error_count(void) { return errors_seen; }

const char *config_error_at(int i) {
  if (i < 0 || i >= errors_kept) return NULL;
  return errors[i];
}

/* ---- the settings table ---------------------------------------------------- */

/* Every setting is an integer: a flag, or a bounded number. No setting needs a
 * string yet, and a string one brings ownership and quoting with it, so the type
 * arrives with the first setting that genuinely wants it rather than in advance. */

enum setting_type { SET_BOOL = 0, SET_INT };

struct setting {
  const char *section;
  const char *name;      /* canonical spelling, lower case, words spaced */
  int type;
  int lo, hi;            /* SET_INT: inclusive bounds. Unused for SET_BOOL. */
  int dflt;
  int value;             /* what the file said, or the default */
  const char *comment;   /* the line written above it in a generated file */
  void (*apply)(int v);  /* push into the editor */
  int (*live)(void);     /* read back from the editor, for write-back */
};

/* --- the editor side of each setting.
 *
 * These are thin on purpose. A setting's apply function is the only thing that
 * knows a setting exists; everything it calls is an ordinary editor function
 * that works the same when a menu item calls it. */

static void apply_line_numbers(int v) {
  V.linenum = v;
  screen_force_repaint();
}
static int live_line_numbers(void) { return V.linenum; }

static void apply_menu_bar(int v) {
  E.menubar = v;
  screen_force_repaint();
}
static int live_menu_bar(void) { return E.menubar; }

static void apply_tab_bar(int v) {
  E.tabbar = v;
  screen_force_repaint();
}
static int live_tab_bar(void) { return E.tabbar; }

static void apply_syntax(int v) {
  V.syntax = v;
  screen_force_repaint();
}
static int live_syntax(void) { return V.syntax; }

static void apply_mouse(int v) { term_mouse_set(v); }
static int live_mouse(void) { return term_mouse_is_on(); }

static void apply_tab_width(int v) { tab_width_set(v); }
static int live_tab_width(void) { return tab_width(); }

static void apply_undo_steps(int v) { undo_set_max_steps(v); }
static int live_undo_steps(void) { return undo_max_steps(); }

static void apply_match_case(int v) { search_set_case(v); }
static void apply_regex(int v) { search_set_regex(v); }

/* The table. Sections group what a user would look for together, which is also
 * what a written file reads as. Order within a section is the order it is
 * written in. */
static struct setting settings[] = {
  {"editor", "tab width", SET_INT, 1, 16, KILO_TAB_STOP, 0,
   "columns a tab character occupies (1-16)",
   apply_tab_width, live_tab_width},
  {"editor", "undo steps", SET_INT, 1, 1000000, 32768, 0,
   "how many undo steps to keep",
   apply_undo_steps, live_undo_steps},

  {"view", "line numbers", SET_BOOL, 0, 0, 0, 0,
   "show the line-number gutter",
   apply_line_numbers, live_line_numbers},
  {"view", "menu bar", SET_BOOL, 0, 0, 1, 0,
   "give the menu bar its row",
   apply_menu_bar, live_menu_bar},
  {"view", "tab bar", SET_BOOL, 0, 0, 1, 0,
   "show a tab per open file (only ever drawn with two or more)",
   apply_tab_bar, live_tab_bar},
  {"view", "syntax", SET_BOOL, 0, 0, 1, 0,
   "colour the text by filetype",
   apply_syntax, live_syntax},

  {"input", "mouse", SET_BOOL, 0, 0, 1, 0,
   "report the mouse to molly rather than leaving it to the terminal",
   apply_mouse, live_mouse},

  {"search", "match case", SET_BOOL, 0, 0, 0, 0,
   "searches are case sensitive",
   apply_match_case, search_case_is_on},
  {"search", "regular expression", SET_BOOL, 0, 0, 0, 0,
   "read the search query as a pattern",
   apply_regex, search_regex_is_on},
};

#define NSETTINGS ((int)(sizeof(settings) / sizeof(settings[0])))

int setting_count(void) { return NSETTINGS; }

/* Look a setting up by its section and canonical name. */
static struct setting *setting_find(const char *section, const char *name) {
  int i;
  for (i = 0; i < NSETTINGS; i++)
    if (strcmp(settings[i].section, section) == 0 &&
        strcmp(settings[i].name, name) == 0)
      return &settings[i];
  return NULL;
}

int setting_get(const char *section, const char *name) {
  const struct setting *s = setting_find(section, name);
  return (s != NULL) ? s->value : -1;
}

void config_defaults(void) {
  int i;
  for (i = 0; i < NSETTINGS; i++) settings[i].value = settings[i].dflt;
}

/* ---- pending key bindings -------------------------------------------------- */

/* The `[keys]` section is not a setting: its left-hand side is a chord rather
 * than a known name, and it is applied to the keymap in cmd.c rather than to a
 * variable here. It is held as text until config_apply_keys() runs, because the
 * keymap does not exist yet when the file is parsed -- and because an Options
 * dialog will want to show what the file asked for before deciding to apply it. */
struct pending_bind {
  char chord[32];
  char cmd[64];
};

static struct pending_bind *binds;
static int binds_n, binds_cap;

static void binds_clear(void) {
  free(binds);
  binds = NULL;
  binds_n = 0;
  binds_cap = 0;
}

/* Returns 0 if either name is too long to hold, which the caller reports. Both
 * are already known to be a real chord and a real command by this point, so this
 * is a bound on the record rather than on the syntax. */
static int bind_add(const char *chord, const char *cmd) {
  if (strlen(chord) >= sizeof(binds[0].chord) ||
      strlen(cmd) >= sizeof(binds[0].cmd))
    return 0;
  if (binds_n == binds_cap) {
    binds_cap = binds_cap ? binds_cap * 2 : 16;
    binds = xrealloc(binds, (size_t)binds_cap * sizeof(*binds));
  }
  memcpy(binds[binds_n].chord, chord, strlen(chord) + 1);
  memcpy(binds[binds_n].cmd, cmd, strlen(cmd) + 1);
  binds_n++;
  return 1;
}

/* A `[syntax <name>]` rule, held until the built-in definitions exist.
 *
 * Deferred for the same reason the key bindings are, plus one of its own:
 * syntax_init() wipes and rebuilds the definitions, and it runs inside
 * init_editor() -- *after* the configuration file is read. Applying a rule at
 * parse time would hand it to a table that is about to be thrown away.
 *
 * The line number travels with the rule, because the rule's name cannot be
 * checked here without keeping a second copy of syntax.c's key list, and
 * "mollyrc:31: unknown syntax rule" is worth a struct field. */
struct pending_syn {
  char lang[32];
  char key[48];
  char val[1024];
  int lineno;
};

static struct pending_syn *syns;
static int syns_n, syns_cap;

static void syns_clear(void) {
  free(syns);
  syns = NULL;
  syns_n = 0;
  syns_cap = 0;
}

static int syn_add(const char *lang, const char *key, const char *val,
                   int lineno) {
  if (strlen(lang) >= sizeof(syns[0].lang) ||
      strlen(key) >= sizeof(syns[0].key) ||
      strlen(val) >= sizeof(syns[0].val))
    return 0;
  if (syns_n == syns_cap) {
    syns_cap = syns_cap ? syns_cap * 2 : 16;
    syns = xrealloc(syns, (size_t)syns_cap * sizeof(*syns));
  }
  memcpy(syns[syns_n].lang, lang, strlen(lang) + 1);
  memcpy(syns[syns_n].key, key, strlen(key) + 1);
  memcpy(syns[syns_n].val, val, strlen(val) + 1);
  syns[syns_n].lineno = lineno;
  syns_n++;
  return 1;
}

/* ---- the parser ------------------------------------------------------------ */

static char *skip_space(char *p) {
  while (*p == ' ' || *p == '\t') p++;
  return p;
}

static void trim_end(char *s) {
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
    s[--n] = '\0';
}

/* Fold a key or section name to its canonical spelling: lower case, with runs of
 * space, '-' and '_' collapsed to one space. So `Line Numbers`, `line-numbers`
 * and `line_numbers` all name the setting written as `line numbers`. Being
 * liberal here costs one function and spares the user a spelling rule; the
 * canonical form is what config_write emits. */
static void canon_name(const char *src, char *dst, size_t dsz) {
  size_t o = 0;
  int sep = 0;
  for (; *src != '\0' && o + 1 < dsz; src++) {
    unsigned char c = (unsigned char)*src;
    if (c == ' ' || c == '\t' || c == '-' || c == '_') {
      sep = (o > 0);  /* never a leading separator */
      continue;
    }
    if (sep) {
      dst[o++] = ' ';
      sep = 0;
      if (o + 1 >= dsz) break;
    }
    dst[o++] = (char)tolower(c);
  }
  dst[o] = '\0';
}

/* A flag's value. Generous about spelling for the same reason canon_name is;
 * config_write emits `on` and `off`. */
static int parse_bool(const char *v, int *out) {
  if (strcasecmp(v, "on") == 0 || strcasecmp(v, "true") == 0 ||
      strcasecmp(v, "yes") == 0 || strcmp(v, "1") == 0) {
    *out = 1;
    return 1;
  }
  if (strcasecmp(v, "off") == 0 || strcasecmp(v, "false") == 0 ||
      strcasecmp(v, "no") == 0 || strcmp(v, "0") == 0) {
    *out = 0;
    return 1;
  }
  return 0;
}

static int parse_int(const char *v, int *out) {
  char *end;
  long n;
  if (*v == '\0') return 0;
  errno = 0;
  n = strtol(v, &end, 10);
  if (errno != 0 || *end != '\0') return 0;
  if (n < INT_MIN || n > INT_MAX) return 0;
  *out = (int)n;
  return 1;
}

/* Is this section one molly knows? Derived from the settings table, plus the one
 * section that is not settings at all. */
/* `[syntax c]` and the like: the section name carries the language, which is
 * what keeps a definition in one block rather than spread over a `language =`
 * key that could disagree with the block it is in. */
static const char *section_syntax_lang(const char *name) {
  if (strncmp(name, "syntax ", 7) != 0) return NULL;
  return (name[7] != '\0') ? name + 7 : NULL;
}

static int section_known(const char *name) {
  int i;
  if (strcmp(name, "keys") == 0) return 1;
  if (section_syntax_lang(name) != NULL) return 1;
  for (i = 0; i < NSETTINGS; i++)
    if (strcmp(settings[i].section, name) == 0) return 1;
  return 0;
}

/* One `key = value` line, in a known section. */
static void config_set_line(const char *origin, int lineno, const char *section,
                            const char *key, const char *value) {
  if (strcmp(section, "keys") == 0) {
    /* Validated here rather than at apply time so the line number is still in
     * hand: "mollyrc:12: no such command" is useful and "no such command" is
     * not. The binding is still applied later, because the keymap does not exist
     * yet. */
    if (chord_parse(key) < 0) {
      config_error("%s:%d: not a key: %s", origin, lineno, key);
      return;
    }
    if (strcasecmp(value, "none") != 0 && cmd_find_by_name(value) == NULL) {
      config_error("%s:%d: no such command: %s", origin, lineno, value);
      return;
    }
    if (!bind_add(key, value))
      config_error("%s:%d: binding is too long to hold", origin, lineno);
    return;
  }

  {
    const char *lang = section_syntax_lang(section);
    if (lang != NULL) {
      /* Not validated here: see struct pending_syn. */
      if (!syn_add(lang, key, value, lineno))
        config_error("%s:%d: syntax rule is too long to hold", origin, lineno);
      return;
    }
  }

  struct setting *s = setting_find(section, key);
  if (s == NULL) {
    config_error("%s:%d: unknown setting: [%s] %s", origin, lineno, section, key);
    return;
  }

  int v;
  if (s->type == SET_BOOL) {
    if (!parse_bool(value, &v)) {
      config_error("%s:%d: %s wants on or off, not \"%s\"", origin, lineno,
                   s->name, value);
      return;
    }
  } else {
    if (!parse_int(value, &v)) {
      config_error("%s:%d: %s wants a number, not \"%s\"", origin, lineno,
                   s->name, value);
      return;
    }
    if (v < s->lo || v > s->hi) {
      config_error("%s:%d: %s must be %d to %d, not %d", origin, lineno, s->name,
                   s->lo, s->hi, v);
      return;
    }
  }
  s->value = v;
}

/* Parse a whole file's text. `origin` is what warnings name -- a path, or
 * something like "<test>" when the text did not come from a file.
 *
 * Mutates nothing of the caller's: the text is copied line by line into a fixed
 * buffer, and a line longer than that is reported rather than silently split,
 * because a silently split line would turn into two confusing warnings instead
 * of one clear one. */
void config_parse_text(const char *text, const char *origin) {
  char section[64] = "";
  char line[1024];
  const char *p = text;
  int lineno = 0;

  while (*p != '\0' || lineno == 0) {
    const char *nl = strchr(p, '\n');
    size_t len = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
    lineno++;
    if (len >= sizeof(line)) {
      config_error("%s:%d: line too long (%zu bytes)", origin, lineno, len);
      if (nl == NULL) break;
      p = nl + 1;
      continue;
    }
    memcpy(line, p, len);
    line[len] = '\0';
    if (nl == NULL) {
      p += len;
    } else {
      p = nl + 1;
    }

    char *q = skip_space(line);
    trim_end(q);
    if (*q == '\0' || *q == '#' || *q == ';') {
      if (nl == NULL) break;
      continue;
    }

    if (*q == '[') {
      char *close = strchr(q, ']');
      if (close == NULL) {
        config_error("%s:%d: section is missing its ]", origin, lineno);
      } else {
        *close = '\0';
        char name[64];
        canon_name(q + 1, name, sizeof(name));
        if (*name == '\0') {
          config_error("%s:%d: empty section name", origin, lineno);
          section[0] = '\0';
        } else if (!section_known(name)) {
          config_error("%s:%d: unknown section: [%s]", origin, lineno, name);
          /* Remembered anyway, so its keys produce one warning about the
           * section rather than one per line beneath it. */
          snprintf(section, sizeof(section), "%s", name);
        } else {
          snprintf(section, sizeof(section), "%s", name);
        }
      }
      if (nl == NULL) break;
      continue;
    }

    char *eq = strchr(q, '=');
    if (eq == NULL) {
      config_error("%s:%d: expected key = value", origin, lineno);
      if (nl == NULL) break;
      continue;
    }
    *eq = '\0';
    trim_end(q);
    char *value = skip_space(eq + 1);

    if (*section == '\0') {
      config_error("%s:%d: %s is outside any section", origin, lineno, q);
      if (nl == NULL) break;
      continue;
    }
    if (!section_known(section)) {
      /* The section was already reported; its keys are skipped quietly. */
      if (nl == NULL) break;
      continue;
    }

    char key[96];
    /* A chord keeps its spelling: chord_parse is already case-insensitive, and
     * "ctrl-s" must not have its dash folded into a space. */
    if (strcmp(section, "keys") == 0) {
      msgf(key, sizeof(key), "%s", q);
    } else {
      canon_name(q, key, sizeof(key));
    }
    if (*key == '\0') {
      config_error("%s:%d: empty key", origin, lineno);
      if (nl == NULL) break;
      continue;
    }
    config_set_line(origin, lineno, section, key, value);

    if (nl == NULL) break;
  }
}

/* Read the configuration file. Returns 1 if a file was read, 0 if there was
 * none to read -- which is the normal case on a fresh install and is not a
 * warning. Resets everything first, so calling it twice is the same as calling
 * it once. */
int config_load(void) {
  config_errors_clear();
  config_defaults();
  binds_clear();
  syns_clear();

  const char *path = config_path();
  if (path == NULL) return 0;

  FILE *fp = fopen(path, "r");
  if (fp == NULL) {
    /* Absent is normal, and a fresh install has no file at all. Anything else --
     * no permission, a broken symlink -- is worth saying, because silently
     * ignoring it looks exactly like the file not being read. */
    if (errno != ENOENT) config_error("%s: %s", path, strerror(errno));
    return 0;
  }

  /* fopen() succeeds on a directory; it is the first read that fails. Without
   * this, MOLLY_CONFIG pointing at a directory read as an empty file and molly
   * started up silently with every default -- which looks identical to the file
   * being read and agreeing with them. */
  {
    struct stat st;
    if (fstat(fileno(fp), &st) != 0) {
      config_error("%s: %s", path, strerror(errno));
      fclose(fp);
      return 0;
    }
    if (!S_ISREG(st.st_mode)) {
      config_error("%s: not a regular file", path);
      fclose(fp);
      return 0;
    }
  }

  /* Read it whole: a config file is kilobytes, and parsing from one buffer means
   * the parser needs no notion of a partial line. The cap is a sanity limit, not
   * a budget -- a file this large is a mistake, not a configuration. */
  size_t cap = 64 * 1024;
  char *text = xmalloc(cap);
  size_t n = fread(text, 1, cap - 1, fp);
  int overflow = (n == cap - 1 && fgetc(fp) != EOF);
  fclose(fp);
  text[n] = '\0';

  if (overflow) {
    config_error("%s: file is larger than %zu bytes; ignored", path, cap - 1);
    free(text);
    return 0;
  }

  config_parse_text(text, path);
  free(text);
  return 1;
}

/* ---- applying -------------------------------------------------------------- */

/* Push the `[keys]` bindings onto the keymap. Separate from config_apply because
 * it has to run after keymap_init(), and because the dump hooks want the keymap
 * without wanting the terminal writes that the settings perform. */
void config_apply_keys(void) {
  int i;
  for (i = 0; i < binds_n; i++) {
    const char *err = NULL;
    if (!keymap_bind(binds[i].chord, binds[i].cmd, &err))
      config_error("%s: %s = %s: %s", config_path() ? config_path() : "config",
                   binds[i].chord, binds[i].cmd, err ? err : "rejected");
  }
}

/* Hand the file's `[syntax ...]` rules to syntax.c, on top of the built-ins --
 * so a block for a language molly already knows adds to it (more keywords) or
 * replaces what it names (a different comment marker), and a block for a new
 * language defines one. */
void config_apply_syntax(void) {
  const char *path = config_path();
  int i;
  for (i = 0; i < syns_n; i++) {
    const char *err = NULL;
    if (!syntax_set_line(syns[i].lang, syns[i].key, syns[i].val, &err))
      config_error("%s:%d: [syntax %s] %s: %s", path ? path : "config",
                   syns[i].lineno, syns[i].lang, syns[i].key,
                   err ? err : "rejected");
  }
}

void config_apply(void) {
  int i;
  for (i = 0; i < NSETTINGS; i++)
    if (settings[i].apply != NULL) settings[i].apply(settings[i].value);
  config_apply_keys();
  config_apply_syntax();
}

/* ---- writing it back ------------------------------------------------------- */

/* The value to write: what the editor is doing now, where a setting can say,
 * falling back to what was parsed. This is what makes "toggle it, then save" do
 * what it looks like it does -- Ctrl-U changes V.linenum without going anywhere
 * near this table, and the table finds out by asking. */
static int setting_current(const struct setting *s) {
  return (s->live != NULL) ? s->live() : s->value;
}

static void write_value(FILE *fp, const struct setting *s, int v) {
  if (s->type == SET_BOOL) fprintf(fp, "%s", v ? "on" : "off");
  else fprintf(fp, "%d", v);
}

/* Write the current settings and any non-default key bindings to `path`.
 *
 * Atomic: a temporary beside the target, then rename, so an interrupted write
 * cannot leave a half-written config that fails to parse on the next start --
 * the same reason editor_save does it for a file. Returns 0 on success, or a
 * negative errno-ish -1 with the reason left in *err. */
int config_write(const char *path, const char **err) {
  static char reason[256];
  char tmp[4096];
  int i;

  if (path == NULL) {
    if (err) *err = "no configuration path (set HOME or MOLLY_CONFIG)";
    return -1;
  }

  /* The directory may not exist yet; a fresh install has no ~/.config/molly. */
  char dir[4096];
  snprintf(dir, sizeof(dir), "%s", path);
  char *slash = strrchr(dir, '/');
  if (slash != NULL) {
    *slash = '\0';
    if (*dir != '\0' && mkdir_p(dir) != 0) {
      msgf(reason, sizeof(reason), "%s: %s", dir, strerror(errno));
      if (err) *err = reason;
      return -1;
    }
  }

  if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
    if (err) *err = "path too long";
    return -1;
  }

  FILE *fp = fopen(tmp, "w");
  if (fp == NULL) {
    msgf(reason, sizeof(reason), "%s: %s", tmp, strerror(errno));
    if (err) *err = reason;
    return -1;
  }

  fprintf(fp, "# molly %s -- configuration\n", MOLLY_VERSION);
  fprintf(fp,
          "#\n"
          "# Written by molly. Anything molly does not recognise, including\n"
          "# your comments, is lost the next time it writes this file.\n");

  const char *section = NULL;
  for (i = 0; i < NSETTINGS; i++) {
    const struct setting *s = &settings[i];
    if (section == NULL || strcmp(section, s->section) != 0) {
      section = s->section;
      fprintf(fp, "\n[%s]\n", section);
    }
    fprintf(fp, "\n# %s\n", s->comment);
    fprintf(fp, "# default: ");
    write_value(fp, s, s->dflt);
    fprintf(fp, "\n%s = ", s->name);
    write_value(fp, s, setting_current(s));
    fprintf(fp, "\n");
  }

  /* Only the bindings that differ from the built-in keymap, so the file stays
   * about what the user changed rather than restating forty defaults -- and so
   * that a later molly with different defaults is not pinned to this one's. */
  fprintf(fp,
          "\n[keys]\n"
          "\n"
          "# Bindings that differ from molly's defaults. A chord on the left, a\n"
          "# command name on the right, or \"none\" to leave the chord unbound.\n"
          "# Ctrl-? shows what is bound; the command palette (Ctrl-Space) lists\n"
          "# every command name.\n");
  keymap_write_changes(fp);

  if (fflush(fp) != 0 || fsync(fileno(fp)) != 0) {
    msgf(reason, sizeof(reason), "%s: %s", tmp, strerror(errno));
    if (err) *err = reason;
    fclose(fp);
    unlink(tmp);
    return -1;
  }
  if (fclose(fp) != 0) {
    msgf(reason, sizeof(reason), "%s: %s", tmp, strerror(errno));
    if (err) *err = reason;
    unlink(tmp);
    return -1;
  }
  if (rename(tmp, path) != 0) {
    msgf(reason, sizeof(reason), "%s: %s", path, strerror(errno));
    if (err) *err = reason;
    unlink(tmp);
    return -1;
  }
  fsync_parent_dir(path);
  return 0;
}

/* ---- the dump hook --------------------------------------------------------- */

/* Print the parsed configuration as tab-separated lines, for the MOLLY_DUMP_CONFIG
 * test hook documented in MANUAL.md. It runs before the terminal is touched and
 * before anything is applied, so it tests the parser rather than the editor --
 * which is the point: a settings test should not need a pty.
 *
 * Shaped like keymap_dump's output: one record per line, name then value. */
void config_dump(FILE *out) {
  int i;
  const char *path = config_path();
  fprintf(out, "path\t%s\n", path ? path : "(none)");
  for (i = 0; i < NSETTINGS; i++) {
    fprintf(out, "%s.%s\t", settings[i].section, settings[i].name);
    write_value(out, &settings[i], settings[i].value);
    fprintf(out, "\n");
  }
  for (i = 0; i < binds_n; i++)
    fprintf(out, "bind\t%s\t%s\n", binds[i].chord, binds[i].cmd);
  for (i = 0; i < syns_n; i++)
    fprintf(out, "syntax\t%s\t%s\t%s\n", syns[i].lang, syns[i].key,
            syns[i].val);
  fprintf(out, "errors\t%d\n", errors_seen);
  for (i = 0; i < errors_kept; i++) fprintf(out, "error\t%s\n", errors[i]);
}

void config_cleanup(void) {
  config_errors_clear();
  binds_clear();
  syns_clear();
}
