/* sys.c -- Allocation that dies rather than returns NULL, robust writes, and the
   filesystem odds and ends. Everything here is a candidate to grow a Windows
   variant later; nothing else should call the OS directly. */

#include "molly.h"

/* Out of memory is fatal. Routing every allocation through these means it
 * exits through die(), restoring the terminal, rather than dereferencing
 * NULL a moment later. */
void *xmalloc(size_t n) {
  void *p = malloc(n);
  if (p == NULL) die("malloc");
  return p;
}

void *xrealloc(void *p, size_t n) {
  void *np = realloc(p, n);
  if (np == NULL) die("realloc");
  return np;
}

/* mkdir -p: create `path` and any missing parent of it, 0700 for a directory
 * molly makes on the user's behalf under their home. Returns 0 on success, or -1
 * with errno set by whichever mkdir failed. An existing directory is success,
 * which is the whole point of the -p; an existing *file* in the way is not, and
 * comes back as EEXIST from the mkdir that found it.
 *
 * Writes into a copy, so the caller's string is not mutated -- a caller whose
 * path is a string literal would otherwise be a crash rather than a bug. */
int mkdir_p(const char *path) {
  char buf[4096];
  char *p;
  if (path == NULL || *path == '\0') {
    errno = EINVAL;
    return -1;
  }
  if (snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  /* Skip the leading slash so an absolute path does not try to mkdir "". */
  for (p = buf + 1; *p != '\0'; p++) {
    if (*p != '/') continue;
    *p = '\0';
    if (mkdir(buf, 0700) != 0 && errno != EEXIST) return -1;
    *p = '/';
  }
  if (mkdir(buf, 0700) != 0 && errno != EEXIST) return -1;
  return 0;
}

char *xstrdup(const char *s) {
  char *p = strdup(s);
  if (p == NULL) die("strdup");
  return p;
}


/* --- robust full write: retries on EINTR and loops over partial writes --- */

int write_all(int fd, const void *p0, size_t n) {
  const char *p = p0;
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w == -1) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (w == 0) return -1;
    p += w;
    n -= (size_t)w;
  }
  return 0;
}


void fsync_parent_dir(const char *path) {
  char *copy = strdup(path);
  if (copy == NULL) return;

  char *slash = strrchr(copy, '/');
  const char *dir;
  if (slash == NULL) dir = ".";
  else if (slash == copy) dir = "/";
  else { *slash = '\0'; dir = copy; }

  int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd != -1) {
    fsync(dfd);
    close(dfd);
  }
  free(copy);
}

/* The mode a newly created file should end up with, honoring the umask. */
mode_t default_file_mode(void) {
  mode_t um = umask(0);
  umask(um);
  return 0666 & ~um;
}
