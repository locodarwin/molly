/* recent.c -- the files opened lately, and a list to open one from.
 *
 * Kept in a file of its own, `$XDG_STATE_HOME/molly/recent` (or
 * ~/.local/state/molly/recent): state, not configuration, which is the
 * distinction the XDG directories draw -- a list molly writes as you work is
 * not something you would put in a dotfiles repository. One absolute path a
 * line, newest first, twenty of them. A missing or unreadable file is an empty
 * list; a failed write loses nothing but the list.
 *
 * MOLLY_STATE overrides the directory, and empty turns the list off: the test
 * harness sets it empty, for the same reason it sets MOLLY_CONFIG empty -- so
 * that running the tests leaves nothing in the home directory of whoever ran
 * them, and the tests do not see what that person opened yesterday. */

#include "molly.h"

#define RECENT_MAX 20

static char *recent[RECENT_MAX];
static int nrecent;
static int loaded;

/* A file of molly's state, `name` in the directory above, or 0 when there is to
 * be none. The trusted projects (trust.c) are kept the same way. */
int state_path(char *dst, size_t n, const char *name) {
  const char *over = getenv("MOLLY_STATE"), *home;
  if (over != NULL) {
    if (over[0] == '\0') return 0;
    snprintf(dst, n, "%s/%s", over, name);
    return 1;
  }
  over = getenv("XDG_STATE_HOME");
  if (over != NULL && over[0] == '/') {
    snprintf(dst, n, "%s/molly/%s", over, name);
    return 1;
  }
  home = getenv("HOME");
  if (home == NULL || home[0] == '\0') return 0;
  snprintf(dst, n, "%s/.local/state/molly/%s", home, name);
  return 1;
}

/* Open a temporary beside the state file `path` to write it through, making
 * the directory first. NULL if it cannot be done, which loses nothing but the
 * write. Unique per call, by mkstemp: a fixed `<name>.tmp` was shared by every
 * session saving at the same moment, which could mix their writes, and it
 * clobbered any file that happened to have the name. */
FILE *state_begin(const char *path, char *tmp, size_t n) {
  char dir[4096];
  char *slash;
  int fd;
  FILE *fp;
  snprintf(dir, sizeof(dir), "%s", path);
  slash = strrchr(dir, '/');
  if (slash != NULL) {
    *slash = '\0';
    if (*dir != '\0' && mkdir_p(dir) != 0) return NULL;
  }
  if (snprintf(tmp, n, "%s.XXXXXX", path) >= (int)n) return NULL;
  fd = mkstemp(tmp);
  if (fd == -1) return NULL;
  fp = fdopen(fd, "w");
  if (fp == NULL) {
    close(fd);
    unlink(tmp);
  }
  return fp;
}

/* Finish what state_begin started: the temporary renamed over `path`, or
 * removed if the write did not complete. */
void state_commit(FILE *fp, const char *tmp, const char *path) {
  if (fclose(fp) == 0 && rename(tmp, path) == 0) return;
  unlink(tmp);
}

static int recent_path(char *dst, size_t n) { return state_path(dst, n, "recent"); }

static void load(void) {
  char path[4096], line[4096];
  FILE *fp;
  if (loaded) return;
  loaded = 1;
  if (!recent_path(path, sizeof(path))) return;
  fp = fopen(path, "r");
  if (fp == NULL) return;
  while (nrecent < RECENT_MAX && fgets(line, sizeof(line), fp) != NULL) {
    line[strcspn(line, "\n")] = '\0';
    if (line[0] == '/') recent[nrecent++] = xstrdup(line);
  }
  fclose(fp);
}

static void save(void) {
  char path[4096], tmp[4200];
  FILE *fp;
  int i;
  if (!recent_path(path, sizeof(path))) return;
  fp = state_begin(path, tmp, sizeof(tmp));
  if (fp == NULL) return;
  for (i = 0; i < nrecent; i++) fprintf(fp, "%s\n", recent[i]);
  state_commit(fp, tmp, path);
}

/* A file was opened: to the top of the list, written straight away so that
 * a crash does not lose it. */
void recent_note(const char *filename) {
  char *abs;
  int i;
  if (filename == NULL) return;
  load();
  abs = realpath(filename, NULL);
  if (abs == NULL) return;
  /* The list is a path a line, so a name with a line break in it would be
   * read back as two entries, the second naming a file nobody opened. */
  if (strchr(abs, '\n') != NULL) {
    free(abs);
    return;
  }
  for (i = 0; i < nrecent; i++) {
    if (strcmp(recent[i], abs) != 0) continue;
    free(recent[i]);
    memmove(&recent[i], &recent[i + 1], sizeof(recent[0]) * (size_t)(nrecent - i - 1));
    nrecent--;
    break;
  }
  if (nrecent == RECENT_MAX) free(recent[--nrecent]);
  memmove(&recent[1], &recent[0], sizeof(recent[0]) * (size_t)nrecent);
  recent[0] = xstrdup(abs);
  free(abs);
  nrecent++;
  save();
}

int recent_count(void) {
  load();
  return nrecent;
}

const char *recent_at(int i) {
  load();
  return (i >= 0 && i < nrecent) ? recent[i] : NULL;
}

void recent_cleanup(void) {
  int i;
  for (i = 0; i < nrecent; i++) free(recent[i]);
  nrecent = 0;
  loaded = 0;
}

/* A row of the dialog: the path, with the home directory as ~ so the part
 * that tells files apart is the part that fits. */
static void recent_row(void *user, int i, char *dst, size_t dsz) {
  const char *p = recent_at(i), *home = getenv("HOME");
  size_t hl = home != NULL ? strlen(home) : 0;
  (void)user;
  if (p == NULL) {
    if (dsz) dst[0] = '\0';
    return;
  }
  if (hl > 1 && strncmp(p, home, hl) == 0 && p[hl] == '/')
    snprintf(dst, dsz, "~%s", p + hl);
  else
    snprintf(dst, dsz, "%s", p);
}

/* File ▸ Recent files…: pick one and open it. */
void editor_recent_dialog(void) {
  struct widget widgets[2];
  struct dialog d;
  struct layer l;
  int depth, li, chosen = -1;

  if (recent_count() == 0) {
    editor_set_status_message("No recent files yet.");
    return;
  }
  widget_list(&widgets[0], 0, 0, 1, 1, recent_count(), recent_row, NULL);
  widget_label(&widgets[1], 0, 1, 1, "Enter open   Esc cancel");
  widgets[0].sel = 0;

  depth = layer_count();
  memset(&l, 0, sizeof(l));
  li = layer_push(&l);
  if (li < 0) return;

  for (;;) {
    struct layer *lp;
    int cols, rows, list_h, c, ns;

    screen_sync_size();
    cols = grid_cols() - 8;
    if (cols > 72) cols = 72;
    if (cols < 16) cols = 16;
    rows = grid_rows() - 4;
    if (rows > RECENT_MAX + 3) rows = RECENT_MAX + 3;
    if (rows < 5) rows = 5;
    list_h = rows - 2 - 1;
    if (list_h < 1) list_h = 1;

    widgets[0].w = cols - 4;
    widgets[0].h = list_h;
    widgets[0].count = recent_count();
    widgets[1].y = list_h;
    widgets[1].w = cols - 4;
    widget_list_clamp(&widgets[0]);

    dialog_init(&d, "Recent files", widgets, 2, cols - 2, rows - 2);
    lp = layer_get(li);
    dialog_layout(&d, lp);
    editor_refresh_screen();

    c = ui_wait_key();
    if (c == WINCH_KEY) continue;
    if (c == MOUSE_KEY) {
      struct mouse_event ev;
      input_last_mouse(&ev);
      if (ev.type == MOUSE_WHEEL_UP) widget_list_move(&widgets[0], -3);
      else if (ev.type == MOUSE_WHEEL_DOWN) widget_list_move(&widgets[0], 3);
      else if (ev.type == MOUSE_PRESS && ev.button == MOUSE_BUTTON_LEFT) {
        int k = ev.y - (lp->y + 1 + widgets[0].y);
        if (k >= 0 && k < widgets[0].h && widgets[0].top + k < recent_count()) {
          chosen = widgets[0].top + k;
          break;
        }
      }
      continue;
    }
    ns = 0;
    if (c == ARROW_DOWN || c == CTRL_KEY('n')) ns = 1;
    else if (c == ARROW_UP || c == CTRL_KEY('p')) ns = -1;
    else if (c == PAGE_DOWN) ns = list_h;
    else if (c == PAGE_UP) ns = -list_h;
    else if (c == HOME_KEY) ns = -recent_count();
    else if (c == END_KEY) ns = recent_count();
    else if (c == '\r') { chosen = widgets[0].sel; break; }
    else if (c == '\x1b' || c == CTRL_KEY('c')) break;
    else continue;
    widget_list_move(&widgets[0], ns);
  }
  layer_pop_to(depth);
  if (chosen >= 0 && recent_at(chosen) != NULL) {
    char cwd[4096];
    const char *p = recent_at(chosen);
    size_t n;
    /* Under the working directory, by the name the user would have typed. */
    if (getcwd(cwd, sizeof(cwd)) != NULL && (n = strlen(cwd)) > 1 &&
        strncmp(p, cwd, n) == 0 && p[n] == '/')
      p += n + 1;
    nav_push();
    editor_open_in_new_buffer(xstrdup(p));
  }
}
