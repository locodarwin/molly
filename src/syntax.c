/* syntax.c -- filetypes and the lexer that colours a row.
 *
 * Table-driven, and deliberately not a parser per language. A definition is a
 * handful of rules -- these words are keywords, this starts a comment to the end
 * of the line, this pair opens and closes one that spans rows, these characters
 * delimit a string -- and the lexer below is the one piece of code that applies
 * all of them. Olly had no highlighting at all and kilo had it hard-coded for C;
 * the cost of doing it this way is that a new language is a table entry rather
 * than a recompile of the lexer, which is the whole point.
 *
 * The seven languages molly ships with are Rust, C, C++, C#, Python, AutoIt and
 * Markdown, and that set is why the rule vocabulary looks the way it does. Every
 * field below is there because one of those seven needs it and none of them is
 * speculative:
 *
 *   - `case sensitive = off` exists for AutoIt, where `IF` and `if` are the same
 *     keyword.
 *   - `block comment` takes several pairs because AutoIt has both `#cs`/`#ce`
 *     and `#comments-start`/`#comments-end`.
 *   - `multiline strings` exists for Python's triple quotes.
 *   - `heading`, `fence`, `code span` and `emphasis` exist for Markdown, which
 *     is the language that stops the keyword/string/comment vocabulary from
 *     being enough. A prose format has no keywords at all.
 *
 * ---- the cost this feature actually has -------------------------------------
 *
 * Not the rules. The rules are cheap. The cost is that a row's colouring depends
 * on the rows above it: a block-comment opener on line 10 makes line 40 a
 * comment, and an edit on line 10 changes line 40 without touching it. So each
 * row records the lexer
 * state it was entered with and the state it left in, and the buffer records how
 * far up the file those states are known to be right (`syn_valid`).
 *
 * Invalidation is lazy, not eager. An edit lowers the watermark to the edited
 * row and stops; the renderer brings rows back up to date just before it draws
 * them. Doing it eagerly would mean that opening a comment at the top of a
 * hundred thousand lines relexed all of them on that keystroke, for the sake of
 * rows
 * nobody is looking at. Lazily it costs the rows between the watermark and the
 * bottom of the screen, and with one cursor the edit is always on screen, so
 * that distance is a screenful.
 *
 * Roles are one byte per byte of the row as stored. They were one per byte of
 * a tab-expanded copy, while rows kept one; the draw loop now walks a tab a
 * column at a time and asks for the tab's role at each, so the colours land
 * where they did and the copy is gone.
 */

#include "molly.h"

/* ---- what a definition holds ---------------------------------------------- */

#define SYN_MAX_LANGS  32
#define SYN_MAX_PAIRS  4
#define SYN_MAX_DELIMS 4
#define SYN_TOKEN      16

struct syn_pair {
  char open[SYN_TOKEN];
  char close[SYN_TOKEN];
};

/* The two keyword classes. Two and not one because every one of the seven wants
 * to say "these are control words" and "these are type names" in different
 * colours, and two and not five because a third class would need a third thing
 * for a user to be able to say, and none of the seven has one. */
#define SYN_WORDS_KEYWORD 0
#define SYN_WORDS_TYPE    1
#define SYN_WORD_CLASSES  2

struct syntax {
  char name[32];
  char exts[192];                   /* space-separated, no dots */

  /* Each class is kept as the single space-separated string it was given, not
   * split into an array of words.
   *
   * Splitting meant one allocation per keyword: about four hundred and fifty
   * across the seven built-in definitions, every one of them during startup,
   * which showed up as the allocation-failure sweep growing from 129 cases to
   * 580. Lookup already has to compare against every word in the class, so
   * walking the string costs the same walk it was doing over the array -- and
   * the arbitrary cap on how many keywords a language may have goes away with
   * it. */
  char *words[SYN_WORD_CLASSES];

  char line_comment[SYN_MAX_PAIRS][SYN_TOKEN];
  int n_line_comment;

  struct syn_pair block[SYN_MAX_PAIRS];
  int n_block;

  char strings[SYN_MAX_DELIMS];     /* single-character string delimiters */
  int n_strings;
  char escape;                      /* 0 for a language with no escape char */

  struct syn_pair mlstring[SYN_MAX_PAIRS];
  int n_mlstring;

  struct syn_pair fence[SYN_MAX_PAIRS];
  int n_fence;

  char preproc[SYN_TOKEN];          /* line-leading marker, "" for none */
  char heading[SYN_TOKEN];
  char codespan[SYN_TOKEN];
  char emphasis[SYN_MAX_DELIMS];
  int n_emphasis;

  int numbers;
  int case_sensitive;

  /* Indentation, or unset. A definition that says nothing about indentation
   * must let `[editor]` show through rather than override it with a zero, so
   * "unset" needs a value no answer uses: 0 for a width (a real one is 1-16)
   * and -1 for the flags. */
  int tab_width;      /* 0 = unset */
  int insert_spaces;  /* -1 = unset */
  int auto_indent;    /* -1 = unset */

  /* The language server: the command line that starts it, empty for none, and
   * the name the protocol knows the language by when that is not molly's own
   * ("csharp" for `cs`). See lsp.c. */
  char lsp[256];
  char lang_id[32];
};

static struct syntax langs[SYN_MAX_LANGS];
static int nlangs;

int syntax_lang_count(void) { return nlangs; }

const char *syntax_lang_name(int i) {
  if (i < 0 || i >= nlangs) return NULL;
  return langs[i].name;
}

/* ---- the multi-row state --------------------------------------------------- */

/* What a row can be in the middle of when it starts. A tag and an index, packed
 * into the byte that each row stores, because a row carrying a struct would make
 * erow bigger for a value that is always one of a dozen possibilities.
 *
 * SYN_ST_NONE is zero on purpose: a row that has never been lexed, and every
 * row of a buffer with no filetype, reads as "not inside anything". */
#define SYN_ST_NONE   0
#define SYN_ST_BLOCK  1
#define SYN_ST_MLSTR  2
#define SYN_ST_FENCE  3

#define SYN_ST(tag, idx) ((tag) * 16 + (idx))
#define SYN_ST_TAG(st)   ((st) / 16)
#define SYN_ST_IDX(st)   ((st) % 16)

_Static_assert(SYN_ST(SYN_ST_FENCE, 15) <= 255,
               "a lexer state has to fit the byte a row keeps it in");

/* ---- building a definition ------------------------------------------------- */

static struct syntax *lang_find(const char *name) {
  int i;
  for (i = 0; i < nlangs; i++)
    if (strcmp(langs[i].name, name) == 0) return &langs[i];
  return NULL;
}

/* The definition named, created empty if it is new. NULL when there is no room,
 * which the caller reports rather than ignores. */
static struct syntax *lang_get(const char *name) {
  struct syntax *s = lang_find(name);
  if (s != NULL) return s;
  if (nlangs == SYN_MAX_LANGS) return NULL;
  s = &langs[nlangs++];
  memset(s, 0, sizeof(*s));
  snprintf(s->name, sizeof(s->name), "%s", name);
  /* The defaults that are not zero. Numbers are worth colouring in all seven,
   * and only AutoIt wants case folding, so both defaults are the common case. */
  s->numbers = 1;
  s->case_sensitive = 1;
  /* Indentation is unset until a line says otherwise; see struct syntax. */
  s->insert_spaces = -1;
  s->auto_indent = -1;
  return s;
}

/* Walk a space-separated value. Returns the token length, or 0 at the end. */
static int next_token(const char **p, char *dst, size_t dsz) {
  const char *q = *p;
  size_t n = 0;
  while (*q == ' ' || *q == '\t') q++;
  if (*q == '\0') { *p = q; return 0; }
  while (*q != '\0' && *q != ' ' && *q != '\t') {
    if (n + 1 < dsz) dst[n++] = *q;
    q++;
  }
  dst[n] = '\0';
  *p = q;
  return (int)n;
}

/* Appended rather than replaced, so a second `keywords =` line for the same
 * language adds to it -- which is what `[syntax c] keywords = ...` in a
 * configuration file is for. */
static void add_words(struct syntax *s, int cls, const char *value) {
  size_t have = (s->words[cls] != NULL) ? strlen(s->words[cls]) : 0;
  size_t add = strlen(value);
  char *p = xrealloc(s->words[cls], have + add + 2);
  if (have > 0) p[have++] = ' ';
  memcpy(p + have, value, add + 1);
  s->words[cls] = p;
}

/* A list of open/close pairs: two tokens make one pair, so `#cs #ce` is one pair
 * and four tokens are two. An odd number of tokens is the mistake worth
 * catching, because it means a marker has been given no closer. */
static int add_pairs(struct syn_pair *dst, int *n, const char *value) {
  char a[SYN_TOKEN], b[SYN_TOKEN];
  const char *p = value;
  for (;;) {
    if (next_token(&p, a, sizeof(a)) == 0) return 1;
    if (next_token(&p, b, sizeof(b)) == 0) return 0;   /* unpaired */
    if (*n == SYN_MAX_PAIRS) return 0;
    snprintf(dst[*n].open, SYN_TOKEN, "%s", a);
    snprintf(dst[*n].close, SYN_TOKEN, "%s", b);
    (*n)++;
  }
}

static int add_tokens(char dst[][SYN_TOKEN], int *n, const char *value) {
  char tok[SYN_TOKEN];
  const char *p = value;
  while (next_token(&p, tok, sizeof(tok)) > 0) {
    if (*n == SYN_MAX_PAIRS) return 0;
    snprintf(dst[*n], SYN_TOKEN, "%s", tok);
    (*n)++;
  }
  return 1;
}

/* Single characters, given either run together ("\"'") or spaced ("\" '"). */
static int add_chars(char *dst, int *n, int cap, const char *value) {
  char tok[SYN_TOKEN];
  const char *p = value;
  while (next_token(&p, tok, sizeof(tok)) > 0) {
    size_t i;
    for (i = 0; tok[i] != '\0'; i++) {
      if (*n == cap) return 0;
      dst[(*n)++] = tok[i];
    }
  }
  return 1;
}

static int parse_bool_word(const char *v, int *out) {
  if (strcasecmp(v, "on") == 0 || strcasecmp(v, "true") == 0 ||
      strcasecmp(v, "yes") == 0 || strcmp(v, "1") == 0) { *out = 1; return 1; }
  if (strcasecmp(v, "off") == 0 || strcasecmp(v, "false") == 0 ||
      strcasecmp(v, "no") == 0 || strcmp(v, "0") == 0) { *out = 0; return 1; }
  return 0;
}

/* One `key = value` of a `[syntax <name>]` section.
 *
 * The single way in, for the built-in definitions below and for the
 * configuration file alike -- which is the same arrangement the keymap has, and
 * for the same reason: the built-ins exercise this every run, so the first user
 * to write a definition is not the first caller to reach it. Returns 1, or 0
 * with *err set to something worth printing. */
int syntax_set_line(const char *lang, const char *key, const char *value,
                    const char **err) {
  struct syntax *s = lang_get(lang);
  if (s == NULL) {
    if (err) *err = "too many syntax definitions";
    return 0;
  }

  if (strcmp(key, "extensions") == 0) {
    snprintf(s->exts, sizeof(s->exts), "%s", value);
    return 1;
  }
  if (strcmp(key, "keywords") == 0) {
    add_words(s, SYN_WORDS_KEYWORD, value);
    return 1;
  }
  if (strcmp(key, "types") == 0) {
    add_words(s, SYN_WORDS_TYPE, value);
    return 1;
  }
  if (strcmp(key, "line comment") == 0)
    return add_tokens(s->line_comment, &s->n_line_comment, value) ? 1
        : (err ? (*err = "too many line comment markers", 0) : 0);
  if (strcmp(key, "block comment") == 0)
    return add_pairs(s->block, &s->n_block, value) ? 1
        : (err ? (*err = "block comment wants pairs of open and close", 0) : 0);
  if (strcmp(key, "multiline strings") == 0)
    return add_pairs(s->mlstring, &s->n_mlstring, value) ? 1
        : (err ? (*err = "multiline strings wants pairs", 0) : 0);
  if (strcmp(key, "fence") == 0)
    return add_pairs(s->fence, &s->n_fence, value) ? 1
        : (err ? (*err = "fence wants pairs", 0) : 0);
  if (strcmp(key, "strings") == 0)
    return add_chars(s->strings, &s->n_strings, SYN_MAX_DELIMS, value) ? 1
        : (err ? (*err = "too many string delimiters", 0) : 0);
  if (strcmp(key, "emphasis") == 0)
    return add_chars(s->emphasis, &s->n_emphasis, SYN_MAX_DELIMS, value) ? 1
        : (err ? (*err = "too many emphasis delimiters", 0) : 0);
  if (strcmp(key, "string escape") == 0) {
    s->escape = value[0];
    return 1;
  }
  if (strcmp(key, "preproc") == 0) {
    snprintf(s->preproc, SYN_TOKEN, "%s", value);
    return 1;
  }
  if (strcmp(key, "heading") == 0) {
    snprintf(s->heading, SYN_TOKEN, "%s", value);
    return 1;
  }
  if (strcmp(key, "code span") == 0) {
    snprintf(s->codespan, SYN_TOKEN, "%s", value);
    return 1;
  }
  if (strcmp(key, "numbers") == 0) {
    if (!parse_bool_word(value, &s->numbers)) {
      if (err) *err = "numbers wants on or off";
      return 0;
    }
    return 1;
  }
  if (strcmp(key, "case sensitive") == 0) {
    if (!parse_bool_word(value, &s->case_sensitive)) {
      if (err) *err = "case sensitive wants on or off";
      return 0;
    }
    return 1;
  }

  /* The three indentation rules. Bounded here and not at the setting, because
   * `[syntax c] tab width = 99` never reaches the settings table: the section
   * carries the language, so the value is parsed on this side. The bound is
   * the same 1-16 the setting uses, and it is spelled out in both places
   * rather than shared, which is two lines against a header constant for a
   * range that is a property of the renderer either way. */
  if (strcmp(key, "tab width") == 0) {
    int w = atoi(value);
    if (w < 1 || w > 16) {
      if (err) *err = "tab width wants 1 to 16";
      return 0;
    }
    s->tab_width = w;
    return 1;
  }
  if (strcmp(key, "insert spaces") == 0) {
    if (!parse_bool_word(value, &s->insert_spaces)) {
      if (err) *err = "insert spaces wants on or off";
      return 0;
    }
    return 1;
  }
  if (strcmp(key, "auto indent") == 0) {
    if (!parse_bool_word(value, &s->auto_indent)) {
      if (err) *err = "auto indent wants on or off";
      return 0;
    }
    return 1;
  }

  /* `none` as well as an empty value, because `language server =` reads like a
   * line someone forgot to finish, and turning off a built-in server is a
   * thing a person means to do. */
  if (strcmp(key, "language server") == 0) {
    if (strlen(value) >= sizeof(s->lsp)) {
      if (err) *err = "language server command is too long";
      return 0;
    }
    snprintf(s->lsp, sizeof(s->lsp), "%s",
             strcasecmp(value, "none") == 0 ? "" : value);
    return 1;
  }
  if (strcmp(key, "language id") == 0) {
    snprintf(s->lang_id, sizeof(s->lang_id), "%s", value);
    return 1;
  }

  if (err) *err = "unknown syntax rule";
  return 0;
}

/* ---- the built-in definitions ---------------------------------------------- */

/* Data rather than text, parsed through syntax_set_line above exactly as a
 * configuration file's lines are. Keyword lists are the ones worth colouring
 * rather than every word in the grammar: a list that includes every library
 * function ages badly and makes the file look like a highlighter test. */
static const struct { const char *lang, *key, *val; } builtin[] = {
  /* --- C */
  {"c", "extensions", "c h"},
  /* The servers each language's own project ships or recommends. Each one is
   * looked for on PATH before it is started, and a missing one is silence:
   * see lsp.c. clangd serves C and C++ both, and one is started for the two. */
  {"c", "language server", "clangd"},
  {"c", "line comment", "//"},
  {"c", "block comment", "/* */"},
  {"c", "strings", "\" '"},
  {"c", "string escape", "\\"},
  {"c", "preproc", "#"},
  {"c", "keywords",
   "if else for while do switch case default break continue return goto "
   "sizeof typedef struct union enum static const volatile extern inline "
   "register restrict auto _Alignas _Alignof _Atomic _Generic _Noreturn "
   "_Static_assert _Thread_local"},
  {"c", "types",
   "void char short int long float double signed unsigned _Bool _Complex "
   "size_t ssize_t ptrdiff_t intptr_t uintptr_t "
   "int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t "
   "FILE NULL bool true false"},

  /* --- C++: C's rules with its own words. */
  {"cpp", "extensions", "cc cpp cxx hh hpp hxx ipp"},
  {"cpp", "language server", "clangd"},
  {"cpp", "line comment", "//"},
  {"cpp", "block comment", "/* */"},
  {"cpp", "strings", "\" '"},
  {"cpp", "string escape", "\\"},
  {"cpp", "preproc", "#"},
  {"cpp", "keywords",
   "if else for while do switch case default break continue return goto "
   "sizeof typedef struct union enum class namespace template typename "
   "public private protected virtual override final explicit friend operator "
   "new delete this nullptr throw try catch noexcept constexpr consteval "
   "static const volatile extern inline mutable using static_cast "
   "dynamic_cast const_cast reinterpret_cast decltype concept requires "
   "co_await co_return co_yield"},
  {"cpp", "types",
   "void char short int long float double signed unsigned bool wchar_t "
   "char8_t char16_t char32_t auto size_t ssize_t ptrdiff_t "
   "string wstring vector map set unordered_map unordered_set pair tuple "
   "shared_ptr unique_ptr weak_ptr optional variant span "
   "true false"},

  /* --- C# */
  {"cs", "extensions", "cs"},
  /* No server: C#'s is launched differently by every distribution of it, so
   * there is no one command line to ship. The protocol's name for it is not
   * molly's, though, which matters to whoever configures one. */
  {"cs", "language id", "csharp"},
  {"cs", "line comment", "//"},
  {"cs", "block comment", "/* */"},
  {"cs", "strings", "\" '"},
  {"cs", "string escape", "\\"},
  /* #region, #if and friends: a line-leading marker, as in C. */
  {"cs", "preproc", "#"},
  {"cs", "keywords",
   "if else for foreach while do switch case default break continue return "
   "goto using namespace class struct interface enum record delegate event "
   "public private protected internal abstract virtual override sealed static "
   "readonly const partial async await yield lock try catch finally throw "
   "new this base is as in out ref params typeof nameof checked unchecked "
   "operator implicit explicit get set init where when"},
  {"cs", "types",
   "void bool byte sbyte char decimal double float int uint long ulong "
   "short ushort object string var dynamic nint nuint "
   "List Dictionary HashSet IEnumerable IList Task Span Nullable "
   "true false null"},

  /* --- Rust */
  {"rust", "extensions", "rs"},
  {"rust", "language server", "rust-analyzer"},
  {"rust", "line comment", "//"},
  {"rust", "block comment", "/* */"},
  {"rust", "strings", "\" '"},
  {"rust", "string escape", "\\"},
  {"rust", "keywords",
   "fn let mut const static if else match loop while for in break continue "
   "return struct enum trait impl dyn pub use mod crate super self as where "
   "type move ref unsafe extern async await yield box macro_rules"},
  {"rust", "types",
   "bool char str u8 u16 u32 u64 u128 usize i8 i16 i32 i64 i128 isize "
   "f32 f64 String Vec Option Some None Result Ok Err Box Rc Arc RefCell "
   "HashMap HashSet BTreeMap Cow true false"},

  /* --- Python */
  {"python", "extensions", "py pyw pyi"},
  {"python", "language server", "pylsp"},
  {"python", "line comment", "#"},
  /* Triple quotes are a region that spans rows, which is what makes them
   * different from the single-quote forms below. */
  {"python", "multiline strings", "\"\"\" \"\"\" ''' '''"},
  {"python", "strings", "\" '"},
  {"python", "string escape", "\\"},
  {"python", "keywords",
   "def class lambda if elif else for while break continue return yield "
   "import from as pass raise try except finally with global nonlocal del "
   "assert async await in is not and or match case"},
  {"python", "types",
   "int float complex bool str bytes bytearray list tuple dict set frozenset "
   "object type None True False self cls print len range enumerate zip "
   "isinstance Exception ValueError TypeError KeyError"},

  /* --- AutoIt. The language that needs case folding and two comment pairs. */
  {"autoit", "extensions", "au3"},
  {"autoit", "line comment", ";"},
  {"autoit", "block comment", "#cs #ce #comments-start #comments-end"},
  /* No escape character: AutoIt doubles the quote to embed one. */
  {"autoit", "strings", "\" '"},
  {"autoit", "preproc", "#"},
  {"autoit", "case sensitive", "off"},
  {"autoit", "keywords",
   "Func EndFunc Return If Then Else ElseIf EndIf For To Step Next "
   "While WEnd Do Until Select Case EndSelect Switch EndSwitch "
   "ContinueLoop ExitLoop Exit Dim Local Global Const Static ReDim Enum "
   "ByRef Volatile With EndWith And Or Not Default True False Null"},
  {"autoit", "types",
   "MsgBox ConsoleWrite FileOpen FileClose FileRead FileWrite StringLen "
   "StringLeft StringRight StringMid StringSplit StringReplace StringFormat "
   "UBound Number String Int Random Sleep Run RunWait ProcessClose "
   "GUICreate GUISetState GUICtrlCreateButton GUIGetMsg"},

  /* --- Markdown. The one that is not a programming language, and the reason
   * the vocabulary has heading, fence, code span and emphasis in it at all. */
  {"markdown", "extensions", "md markdown mdown mkd"},
  {"markdown", "heading", "#"},
  {"markdown", "fence", "``` ```"},
  {"markdown", "code span", "`"},
  {"markdown", "emphasis", "* _"},
  {"markdown", "numbers", "off"},
};

#define NBUILTIN ((int)(sizeof(builtin) / sizeof(builtin[0])))

/* Parse the built-in definitions. A bad one is a programming error in the table
 * above, so this exits rather than carrying on with a language half-defined --
 * the same rule keymap_init() follows, and for the same reason. */
void syntax_init(void) {
  int i;
  syntax_cleanup();
  for (i = 0; i < NBUILTIN; i++) {
    const char *err = NULL;
    if (!syntax_set_line(builtin[i].lang, builtin[i].key, builtin[i].val, &err))
      die("syntax_init: bad built-in definition");
  }
}

/* The one row's roles the draw loop is working from; see syntax_roles(). Up
 * here because cleanup frees them. */
static unsigned char *roles;
static int rolecap;
static struct {
  const struct buffer *buf;
  const struct syntax *syn;
  int row, in;
} lexed;

void syntax_cleanup(void) {
  int i, c;
  for (i = 0; i < nlangs; i++)
    for (c = 0; c < SYN_WORD_CLASSES; c++) {
      free(langs[i].words[c]);
      langs[i].words[c] = NULL;
    }
  nlangs = 0;
  free(roles);
  roles = NULL;
  rolecap = 0;
  lexed.buf = NULL;
}

/* ---- picking a definition for a file -------------------------------------- */

/* The definition whose extension list contains this file's, or NULL.
 *
 * Matched on the extension alone. A shebang line would catch an extensionless
 * script and is the obvious next rule, but it is a rule about the file's
 * *contents* rather than its name, and nothing in the seven needs it. */
const struct syntax *syntax_for_filename(const char *path) {
  const char *base, *dot;
  int i;

  if (path == NULL) return NULL;
  base = strrchr(path, '/');
  base = (base != NULL) ? base + 1 : path;
  dot = strrchr(base, '.');
  if (dot == NULL || dot[1] == '\0') return NULL;
  dot++;

  for (i = 0; i < nlangs; i++) {
    const char *p = langs[i].exts;
    char tok[SYN_TOKEN];
    while (next_token(&p, tok, sizeof(tok)) > 0)
      if (strcasecmp(tok, dot) == 0) return &langs[i];
  }
  return NULL;
}

const char *syntax_name(const struct syntax *s) {
  return (s != NULL) ? s->name : NULL;
}

/* The first line-comment marker, or "" for a language with none. */
const char *syntax_line_comment(const struct syntax *s) {
  return (s != NULL && s->n_line_comment > 0) ? s->line_comment[0] : "";
}

/* The command that starts this language's server, or "" for none. */
const char *syntax_lsp(const struct syntax *s) {
  return (s != NULL) ? s->lsp : "";
}

/* The protocol's name for the language: its own `language id`, or molly's. */
const char *syntax_lang_id(const struct syntax *s) {
  if (s == NULL) return "plaintext";
  return s->lang_id[0] != '\0' ? s->lang_id : s->name;
}

/* The indentation a definition asks for, or the unset value. Three functions
 * rather than one struct returned by value, because indent.c wants each answer
 * at a different moment and a buffer with no filetype at all has to read as
 * unset -- which a NULL `s` gives for free here and would not through a struct
 * the caller has to allocate. */
int syntax_tab_width(const struct syntax *s) {
  return (s != NULL) ? s->tab_width : 0;
}
int syntax_insert_spaces(const struct syntax *s) {
  return (s != NULL) ? s->insert_spaces : -1;
}
int syntax_auto_indent(const struct syntax *s) {
  return (s != NULL) ? s->auto_indent : -1;
}

/* ---- the lexer ------------------------------------------------------------- */

static int is_word_char(unsigned char c) {
  return isalnum(c) || c == '_';
}

/* A keyword must be a whole word, so the character before it cannot be one that
 * could belong to the same identifier. */
static int word_boundary_before(const char *r, int i) {
  return i == 0 || !is_word_char((unsigned char)r[i - 1]);
}

static int match_at(const char *r, int rs, int i, const char *tok) {
  int n = (int)strlen(tok);
  if (n == 0 || i + n > rs) return 0;
  return memcmp(r + i, tok, (size_t)n) == 0;
}

/* Which word class this identifier belongs to, or -1.
 *
 * Walks the class's word string in place rather than copying each word out of
 * it: the length is compared first, so most words are rejected without looking
 * at a single character of them. A linear scan over a couple of hundred words
 * per identifier would be worth worrying about if it ran per keystroke, and it
 * does not -- a row is lexed when it changes and then its roles are cached. */
static int word_class(const struct syntax *s, const char *r, int start, int len) {
  int c;
  for (c = 0; c < SYN_WORD_CLASSES; c++) {
    const char *p = s->words[c];
    if (p == NULL) continue;
    while (*p != '\0') {
      const char *w;
      int wl;
      while (*p == ' ') p++;
      if (*p == '\0') break;
      w = p;
      while (*p != '\0' && *p != ' ') p++;
      wl = (int)(p - w);
      if (wl != len) continue;
      if (s->case_sensitive
              ? (memcmp(r + start, w, (size_t)len) == 0)
              : (strncasecmp(r + start, w, (size_t)len) == 0))
        return c;
    }
  }
  return -1;
}

static void fill(unsigned char *out, int from, int to, unsigned char role) {
  int i;
  for (i = from; i < to; i++) out[i] = role;
}

/* Is everything before `i` on this row blank? Line-leading rules -- a heading, a
 * preprocessor directive -- apply after indentation and nowhere else, so that a
 * `#` inside a string is not a directive. */
static int only_blanks_before(const char *r, int i) {
  int j;
  for (j = 0; j < i; j++)
    if (r[j] != ' ' && r[j] != '\t') return 0;
  return 1;
}

/* Colour one row.
 *
 * `r`/`rs` is the row's text, `state_in` the state it is entered with,
 * `out` receives one role per byte, and `*state_out` the state the next row
 * starts in. Every byte of `out` is written, so a caller never sees a stale role
 * from a previous, longer row.
 *
 * The order of the tests below is the precedence, and it is the part worth
 * reading twice: a region that spans rows is looked for first, because being
 * inside a block comment outranks everything a row could otherwise contain; a
 * line comment outranks a string, so `// "` is not an unterminated string; and a
 * string outranks a keyword, so `"if"` is not a keyword. */
void syntax_row(const struct syntax *s, const char *r, int rs, int state_in,
                unsigned char *out, int *state_out) {
  int i = 0;
  int state = state_in;

  if (s == NULL) {
    fill(out, 0, rs, ROLE_TEXT);
    if (state_out) *state_out = SYN_ST_NONE;
    return;
  }

  /* Finish whatever the row was entered inside. */
  if (state != SYN_ST_NONE) {
    int tag = SYN_ST_TAG(state), idx = SYN_ST_IDX(state);
    const char *close = NULL;
    unsigned char role = ROLE_SYN_COMMENT;
    if (tag == SYN_ST_BLOCK && idx < s->n_block) {
      close = s->block[idx].close;
      role = ROLE_SYN_COMMENT;
    } else if (tag == SYN_ST_MLSTR && idx < s->n_mlstring) {
      close = s->mlstring[idx].close;
      role = ROLE_SYN_STRING;
    } else if (tag == SYN_ST_FENCE && idx < s->n_fence) {
      close = s->fence[idx].close;
      role = ROLE_SYN_STRING;
    }
    if (close == NULL) {
      /* The definition changed under a row that was inside a region it no longer
       * has. Treat the row as plain and start again from nothing. */
      state = SYN_ST_NONE;
    } else {
      while (i < rs) {
        if (match_at(r, rs, i, close)) {
          int n = (int)strlen(close);
          fill(out, i, i + n, role);
          i += n;
          state = SYN_ST_NONE;
          break;
        }
        out[i++] = role;
      }
      if (state != SYN_ST_NONE) {
        /* Still open at the end of the row. */
        if (state_out) *state_out = state;
        return;
      }
    }
  }

  while (i < rs) {
    int k, n;

    /* A line-leading marker claims the whole rest of the row. */
    if (s->heading[0] != '\0' && only_blanks_before(r, i) &&
        match_at(r, rs, i, s->heading)) {
      fill(out, i, rs, ROLE_SYN_HEADING);
      i = rs;
      break;
    }
    if (s->preproc[0] != '\0' && only_blanks_before(r, i) &&
        match_at(r, rs, i, s->preproc)) {
      /* Not for AutoIt's #cs, which is a comment opener and is checked first
       * below -- so this runs only when no block opener matched here. */
      int is_block = 0;
      for (k = 0; k < s->n_block; k++)
        if (match_at(r, rs, i, s->block[k].open)) is_block = 1;
      if (!is_block) {
        fill(out, i, rs, ROLE_SYN_PREPROC);
        i = rs;
        break;
      }
    }

    /* A region that spans rows, opening here. */
    for (k = 0; k < s->n_block; k++) {
      if (!match_at(r, rs, i, s->block[k].open)) continue;
      n = (int)strlen(s->block[k].open);
      fill(out, i, i + n, ROLE_SYN_COMMENT);
      i += n;
      state = SYN_ST(SYN_ST_BLOCK, k);
      goto region;
    }
    for (k = 0; k < s->n_mlstring; k++) {
      if (!match_at(r, rs, i, s->mlstring[k].open)) continue;
      n = (int)strlen(s->mlstring[k].open);
      fill(out, i, i + n, ROLE_SYN_STRING);
      i += n;
      state = SYN_ST(SYN_ST_MLSTR, k);
      goto region;
    }
    for (k = 0; k < s->n_fence; k++) {
      if (!match_at(r, rs, i, s->fence[k].open)) continue;
      n = (int)strlen(s->fence[k].open);
      fill(out, i, rs, ROLE_SYN_STRING);   /* the fence line is all marker */
      i = rs;
      state = SYN_ST(SYN_ST_FENCE, k);
      goto done;
    }

    /* A comment to the end of the row. After the block openers, so that a
     * two-character block opener is not read as a one-character line comment in
     * a language whose markers share a first character. */
    for (k = 0; k < s->n_line_comment; k++) {
      if (!match_at(r, rs, i, s->line_comment[k])) continue;
      fill(out, i, rs, ROLE_SYN_COMMENT);
      i = rs;
      goto done;
    }

    /* A string, which ends on this row: an unterminated one is a mistake in the
     * file rather than a region, and colouring the rest of the file as string
     * because of a stray quote is the classic way a highlighter makes a typo
     * look like a disaster. */
    for (k = 0; k < s->n_strings; k++) {
      char q = s->strings[k];
      int j;
      if (r[i] != q) continue;
      out[i] = ROLE_SYN_STRING;
      j = i + 1;
      while (j < rs) {
        if (s->escape != '\0' && r[j] == s->escape && j + 1 < rs) {
          out[j] = ROLE_SYN_STRING;
          out[j + 1] = ROLE_SYN_STRING;
          j += 2;
          continue;
        }
        out[j] = ROLE_SYN_STRING;
        if (r[j] == q) { j++; break; }
        j++;
      }
      i = j;
      goto next;
    }

    /* Markdown's inline verbatim, which behaves like a string and ends on the
     * row for the same reason. */
    if (s->codespan[0] != '\0' && match_at(r, rs, i, s->codespan)) {
      int cn = (int)strlen(s->codespan);
      int j = i + cn;
      while (j < rs && !match_at(r, rs, j, s->codespan)) j++;
      if (j < rs) j += cn;
      fill(out, i, j, ROLE_SYN_STRING);
      i = j;
      goto next;
    }

    /* Emphasis: a delimiter, text, the same delimiter, on one row. A lone `*`
     * in prose is left plain rather than opening something that never closes. */
    for (k = 0; k < s->n_emphasis; k++) {
      char d = s->emphasis[k];
      int j;
      if (r[i] != d) continue;
      j = i + 1;
      while (j < rs && r[j] != d) j++;
      if (j >= rs) continue;             /* no closer: not emphasis */
      fill(out, i, j + 1, ROLE_SYN_EMPHASIS);
      i = j + 1;
      goto next;
    }

    /* A number, which has to start a word: the `1` in `x1` is part of an
     * identifier rather than a literal. */
    if (s->numbers && isdigit((unsigned char)r[i]) &&
        word_boundary_before(r, i)) {
      int j = i;
      while (j < rs && (is_word_char((unsigned char)r[j]) || r[j] == '.' ||
                        ((r[j] == '+' || r[j] == '-') && j > i &&
                         (r[j - 1] == 'e' || r[j - 1] == 'E'))))
        j++;
      fill(out, i, j, ROLE_SYN_NUMBER);
      i = j;
      goto next;
    }

    /* An identifier, which may be a keyword. */
    if (is_word_char((unsigned char)r[i]) && word_boundary_before(r, i)) {
      int j = i;
      int cls;
      while (j < rs && is_word_char((unsigned char)r[j])) j++;
      cls = word_class(s, r, i, j - i);
      fill(out, i, j,
           (cls == SYN_WORDS_KEYWORD) ? ROLE_SYN_KEYWORD
           : (cls == SYN_WORDS_TYPE) ? ROLE_SYN_TYPE : ROLE_TEXT);
      i = j;
      goto next;
    }

    out[i] = ROLE_TEXT;
    i++;
next:
    continue;
region:
    /* Inside a region that opened on this row: colour to its closer or to the
     * end of the row, then carry on (a block comment can close and be followed
     * by code). */
    {
      int tag = SYN_ST_TAG(state), idx = SYN_ST_IDX(state);
      const char *close = (tag == SYN_ST_BLOCK) ? s->block[idx].close
                                                : s->mlstring[idx].close;
      unsigned char role = (tag == SYN_ST_BLOCK) ? ROLE_SYN_COMMENT
                                                 : ROLE_SYN_STRING;
      while (i < rs) {
        if (match_at(r, rs, i, close)) {
          int cn = (int)strlen(close);
          fill(out, i, i + cn, role);
          i += cn;
          state = SYN_ST_NONE;
          break;
        }
        out[i++] = role;
      }
      if (state != SYN_ST_NONE) break;   /* open at the end of the row */
    }
  }

done:
  fill(out, i, rs, ROLE_TEXT);
  if (state_out) *state_out = state;
}

/* ---- keeping rows up to date ----------------------------------------------- */

/* The roles of one row, worked out when it is drawn rather than kept.
 *
 * Rows used to keep a role per byte from the moment they were lexed, which put
 * a second copy's worth of memory beside every row a highlighted file had ever
 * scrolled past -- the whole file, after a jump to its end. What a row has to
 * keep is only the state it is entered with: from that, its roles are one pass
 * over its bytes, and a frame only ever needs a screenful of them.
 *
 * One row's worth, remembered by which row it was and what it was lexed from,
 * because a wrapped row is drawn a slice at a time and each slice asks for the
 * same roles. Nothing in the key says whether the row's text has changed, and
 * nothing needs to: an edit lowers the watermark to the row it touched, so the
 * row is either below it -- and drawn plain until relexed -- or has been relexed
 * by syntax_ensure, which works in this same buffer and so forgets what it
 * held. */
static unsigned char *roles_room(int n) {
  if (n < 1) n = 1;
  if (rolecap < n) {
    roles = xrealloc(roles, (size_t)n);
    rolecap = n;
  }
  /* Whatever was in there is about to be overwritten. */
  lexed.buf = NULL;
  return roles;
}

/* An edit on `row` makes every row below it suspect, because a comment opened
 * here can turn the next fifty rows into comment. Lowering the watermark is the
 * whole of
 * invalidation; the relexing happens in syntax_ensure, just before a draw.
 *
 * Clamped rather than checked: a caller that passes a row past the end is saying
 * "nothing above this changed", which is already true. */
void syntax_invalidate_from(int row) {
  if (V.buf == NULL) return;
  if (row < 0) row = 0;
  if (row < B.syn_valid) B.syn_valid = row;
}

/* Bring rows 0..upto into a state where their colouring can be drawn.
 *
 * Costs the rows between the watermark and `upto`, which is a screenful in the
 * ordinary case: an edit is always on screen because the cursor is, so the
 * watermark is never far above what is being drawn. A jump to the end of a large
 * file is the case that costs the most, and it costs it once. */
void syntax_ensure(int upto) {
  int i;
  if (V.buf == NULL || B.syn == NULL) return;
  if (upto >= row_count()) upto = row_count() - 1;
  for (i = B.syn_valid; i <= upto; i++) {
    erow *row = row_at(i);
    int in = (i > 0) ? row_at(i - 1)->syn_out : SYN_ST_NONE, out;
    row->syn_in = (unsigned char)in;
    syntax_row(B.syn, row->chars, row->size, in, roles_room(row->size), &out);
    row->syn_out = (unsigned char)out;
  }
  if (upto + 1 > B.syn_valid) B.syn_valid = upto + 1;
}

/* Choose the definition for this buffer's file name, and start again.
 *
 * Called whenever the name changes -- an open, a save-as -- rather than only at
 * load, because "which language is this" is a function of the name and the name
 * can change under a buffer that already has rows in it. */
void syntax_retype(void) {
  if (V.buf == NULL) return;
  B.syn = syntax_for_filename(B.filename);
  B.syn_valid = 0;
  /* A buffer freed and another made at the same address could otherwise be
   * handed the roles of a row of the first. Every open comes through here. */
  lexed.buf = NULL;
  /* The filetype is what the tab width can hang off, so a new name can mean a
   * new width. Here rather than at the call sites, because every one of them
   * changes the name for the same reason. On an open this runs before the rows
   * are read, so they are measured at the right width the first time. */
  indent_sync();
}

/* Make sure every row the current view is about to show has been lexed.
 *
 * Called once per window per frame, from window_scroll_all(), after the scroll
 * has settled the visible range and before anything is painted. Not from inside
 * the draw loop: this allocates, and a row's colouring depends on the row above
 * it, so lexing one row at a time as the loop reached it would be the same work
 * in a worse order. One row past the bottom, because `syn_out` of the last
 * visible row is what the next scroll step starts from. */
void syntax_ensure_view(void) {
  int last;
  struct drow d;
  if (V.buf == NULL || !V.syntax) return;
  /* The file row of the display row one past the bottom, which with wrap on is
   * nearer than the count of rows suggests: a screenful of a wrapped line is
   * one row to lex, not a screenful of them. Unwrapped the slice's filerow is
   * the index itself, which is the arithmetic this line used to do. */
  drow_at(V.drowoff + V.screenrows, &d);
  last = d.filerow;
  if (last >= row_count()) last = row_count() - 1;
  if (last >= 0) syntax_ensure(last);
}

/* The roles of file row `filerow` of the current buffer, one per byte, for the
 * draw loop -- or NULL for a row drawn in plain text: no filetype, highlighting
 * off, or a row the watermark says has not been lexed. The pointer is good until
 * the next call.
 *
 * NULL rather than a run of ROLE_TEXT, so a buffer with no filetype costs the
 * draw loop one test per row and nothing per byte. */
const unsigned char *syntax_roles(int filerow) {
  erow *row;
  int out;
  /* `V.syntax` is read here and not only in syntax_ensure_view: turning
   * highlighting off has to stop the colours being *drawn*, which is not the
   * same thing as no longer lexing. */
  if (V.buf == NULL || !V.syntax || B.syn == NULL) return NULL;
  if (filerow < 0 || filerow >= B.syn_valid || filerow >= row_count())
    return NULL;
  row = row_at(filerow);
  if (lexed.buf == V.buf && lexed.syn == B.syn && lexed.row == filerow &&
      lexed.in == row->syn_in)
    return roles;
  syntax_row(B.syn, row->chars, row->size, row->syn_in, roles_room(row->size),
             &out);
  lexed.buf = V.buf;
  lexed.syn = B.syn;
  lexed.row = filerow;
  lexed.in = row->syn_in;
  return roles;
}
