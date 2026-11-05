/* trust.c -- the projects a language server may be started in.
 *
 * A language server reads the project it is started for, and some do more than
 * read: rust-analyzer builds it, which runs the project's build scripts and
 * macros, and others load plugins or settings from it. Opening a file is not
 * consent to that. A project that came in an archive, or that someone else
 * wrote, would run its own code the moment one of its files was looked at. So
 * the first time a server is wanted in a project, the user is asked, and the
 * answer is kept.
 *
 * Kept in `projects`, beside the recent-files list (see state_path): one answer
 * a line, `yes` or `no` and a directory. An answer covers the directory and
 * everything under it, and the nearest one up from a project is the one that
 * counts -- so `yes /home/me/src` answers for every project there, and `yes /`
 * for all of them, which is how to never be asked. With no state directory the
 * answers last the session. The file is read once, and again just before each
 * write, so that one molly session does not write over another's answers.
 *
 * Restart language servers forgets the noes, for a user who said no and has
 * changed their mind: the next file opened in such a project asks again. */

#include "molly.h"

struct answer {
  char *dir;
  int yes;
};

static struct answer *answers;
static int nanswers;
static int loaded;

static void clear(void) {
  int i;
  for (i = 0; i < nanswers; i++) free(answers[i].dir);
  free(answers);
  answers = NULL;
  nanswers = 0;
}

static void add(const char *dir, int yes) {
  int i;
  for (i = 0; i < nanswers; i++) {
    if (strcmp(answers[i].dir, dir) == 0) {
      answers[i].yes = yes;
      return;
    }
  }
  answers = xrealloc(answers, sizeof(*answers) * (size_t)(nanswers + 1));
  answers[nanswers].dir = xstrdup(dir);
  answers[nanswers].yes = yes;
  nanswers++;
}

/* The file's answers, merged into what is held: the file's win, being the
 * newer when another session wrote them. */
static void read_file(void) {
  char path[4096], line[4200];
  FILE *fp;
  if (!state_path(path, sizeof(path), "projects")) return;
  fp = fopen(path, "r");
  if (fp == NULL) return;
  while (fgets(line, sizeof(line), fp) != NULL) {
    line[strcspn(line, "\n")] = '\0';
    if (strncmp(line, "yes /", 5) == 0) add(line + 4, 1);
    else if (strncmp(line, "no /", 4) == 0) add(line + 3, 0);
  }
  fclose(fp);
}

static void save(void) {
  char path[4096], dir[4096], tmp[4200];
  FILE *fp;
  int i;
  if (!state_path(path, sizeof(path), "projects")) return;
  snprintf(dir, sizeof(dir), "%s", path);
  *strrchr(dir, '/') = '\0';
  if (mkdir_p(dir) != 0) return;
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  fp = fopen(tmp, "w");
  if (fp == NULL) return;
  for (i = 0; i < nanswers; i++)
    fprintf(fp, "%s %s\n", answers[i].yes ? "yes" : "no", answers[i].dir);
  if (fclose(fp) == 0) rename(tmp, path);
  else unlink(tmp);
}

static void load(void) {
  if (loaded) return;
  loaded = 1;
  read_file();
}

/* Is `dir` the directory `root`, or one above it? */
static int covers(const char *dir, const char *root) {
  size_t n = strlen(dir);
  if (strcmp(dir, "/") == 0) return 1;
  return strncmp(dir, root, n) == 0 && (root[n] == '\0' || root[n] == '/');
}

/* 1 when a server may be started for the project at `root`, 0 when the user
 * said not, -1 when nobody has been asked. */
int trust_lookup(const char *root) {
  int i, best = -1;
  size_t bestlen = 0;
  load();
  for (i = 0; i < nanswers; i++) {
    size_t n = strlen(answers[i].dir);
    if (covers(answers[i].dir, root) && (best < 0 || n > bestlen)) {
      best = i;
      bestlen = n;
    }
  }
  return best < 0 ? -1 : answers[best].yes;
}

/* The user's answer for the project at `root`, kept. */
void trust_set(const char *root, int yes) {
  load();
  read_file();
  add(root, yes);
  save();
}

/* Every no forgotten, so those projects are asked about again. */
void trust_forget_noes(void) {
  int i, o = 0;
  load();
  read_file();
  for (i = 0; i < nanswers; i++) {
    if (!answers[i].yes) {
      free(answers[i].dir);
      continue;
    }
    answers[o++] = answers[i];
  }
  if (o == nanswers) return;
  nanswers = o;
  save();
}

void trust_cleanup(void) {
  clear();
  loaded = 0;
}
