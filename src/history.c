#include "history.h"

#include "common.h"
#include "vars.h"

/* Ring buffer of the last `cap` commands. Entries are numbered from
 * `base` (the oldest) upwards, and numbers stay stable as old entries drop
 * off, so `!n` always means the same command. */
/* In the history file, an entry spanning several lines is preceded by a
 * line "#cpsh:N" giving its number of lines. Every other line is one entry,
 * so files written by older versions read the same. */
#define MULTILINE_MARK "#cpsh:"

static char **ring;
static int cap, start, count;
static int base = 1;
static int entered; /* the newest entry is the command being run */

static void ensure_ring(void) {
  if (ring) return;
  const char *hs = var_get("HISTSIZE");
  cap = hs && is_number(hs) && atoi(hs) > 0 ? atoi(hs) : HISTORY_SIZE;
  ring = xcalloc((size_t)cap, sizeof(char *));
}

static char *history_path(void) {
  const char *f = var_get("HISTFILE");
  if (f && *f) return xstrdup(f);
  const char *home = var_get("HOME");
  if (!home || !*home) return NULL;
  size_t n = strlen(home) + sizeof(HISTORY_FILE);
  char *p = xmalloc(n);
  snprintf(p, n, "%s%s", home, HISTORY_FILE);
  return p;
}

static void push(const char *line, size_t len) {
  ensure_ring();
  if (count == cap) {
    free(ring[start]);
    ring[start] = NULL;
    start = (start + 1) % cap;
    count--;
    base++;
  }
  ring[(start + count) % cap] = xstrndup(line, len);
  count++;
}

int history_first(void) { return base; }
int history_last(void) { return count ? base + count - 1 : 0; }

const char *history_get(int n) {
  if (n < base || n >= base + count) return NULL;
  return ring[(start + n - base) % cap];
}

void history_add(const char *line) {
  size_t len = strlen(line);
  while (len > 0 && line[len - 1] == '\n') len--;
  size_t i = 0;
  while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
  if (i == len) return; /* blank */
  entered = 1;
  const char *last = count ? history_get(history_last()) : NULL;
  if (last && strlen(last) == len && memcmp(last, line, len) == 0) return;
  push(line, len);
}

int history_current(void) {
  return entered && count ? history_last() : history_last() + 1;
}

void history_replace_current(const char *text) {
  if (!entered || !count) return;
  size_t len = strlen(text);
  while (len > 0 && text[len - 1] == '\n') len--;
  int i = (start + count - 1) % cap;
  free(ring[i]);
  ring[i] = xstrndup(text, len);
}

void history_clear(void) {
  for (int i = 0; i < count; i++) {
    free(ring[(start + i) % cap]);
    ring[(start + i) % cap] = NULL;
  }
  base += count;
  start = count = 0;
}

static void load_file(void) {
  char *path = history_path();
  if (!path) return;
  FILE *f = fopen(path, "r");
  free(path);
  if (!f) return;
  char *line = NULL;
  size_t lcap = 0;
  ssize_t n;
  strbuf entry;
  sb_init(&entry);
  int more = 0; /* lines still to join into a multi-line entry */
  while ((n = getline(&line, &lcap, f)) > 0) {
    while (n > 0 && line[n - 1] == '\n') n--;
    line[n] = '\0';
    if (more > 0) {
      if (entry.len) sb_putc(&entry, '\n');
      sb_putn(&entry, line, (size_t)n);
      if (--more == 0) push(entry.s, entry.len);
      continue;
    }
    const size_t ml = sizeof(MULTILINE_MARK) - 1;
    if (strncmp(line, MULTILINE_MARK, ml) == 0 && is_number(line + ml) &&
        atoi(line + ml) > 1) {
      more = atoi(line + ml);
      entry.len = 0;
      continue;
    }
    if (n > 0) push(line, (size_t)n);
  }
  if (more > 0 && entry.len) push(entry.s, entry.len); /* truncated file */
  sb_free(&entry);
  free(line);
  fclose(f);
}

void history_init(void) {
  ensure_ring();
  load_file();
}

/* Write the history to fd, which is closed; 0 on success. */
static int write_entries(int fd) {
  FILE *f = fdopen(fd, "w");
  if (!f) {
    close(fd);
    return -1;
  }
  for (int i = 0; i < count; i++) {
    const char *h = ring[(start + i) % cap];
    int lines = 1;
    for (const char *c = h; *c; c++) lines += *c == '\n';
    if (lines > 1) fprintf(f, "%s%d\n", MULTILINE_MARK, lines);
    fprintf(f, "%s\n", h);
  }
  return fclose(f) == 0 ? 0 : -1;
}

void history_save(void) {
  char *path = history_path();
  if (!path || !ring) {
    free(path);
    return;
  }
  /* A regular file is replaced by a complete new one, so a crash never
   * truncates the existing history. Anything else is written in place:
   * renaming over it would replace a symbolic link with a file, or, for a
   * shell run as root, HISTFILE=/dev/null with a regular file. */
  struct stat st;
  int replace = lstat(path, &st) == 0 ? S_ISREG(st.st_mode) : errno == ENOENT;
  if (replace) {
    size_t n = strlen(path) + 8;
    char *tmp = xmalloc(n);
    snprintf(tmp, n, "%s.XXXXXX", path);
    int fd = mkstemp(tmp); /* a name of its own: other shells may be saving */
    if (fd >= 0 && (write_entries(fd) != 0 || rename(tmp, path) != 0))
      unlink(tmp);
    free(tmp);
  } else {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) write_entries(fd);
  }
  free(path);
}

static const char *find_prefix(const char *prefix, size_t n) {
  for (int i = history_last(); i >= base && i > 0; i--) {
    const char *h = history_get(i);
    if (strncmp(h, prefix, n) == 0) return h;
  }
  return NULL;
}

char *history_expand(const char *line, int *changed) {
  strbuf sb;
  sb_init(&sb);
  *changed = 0;
  int squote = 0, dquote = 0;
  for (const char *p = line; *p; p++) {
    char c = *p;
    if (c == '\\' && !squote && p[1]) {
      sb_putc(&sb, c);
      sb_putc(&sb, *++p);
      continue;
    }
    if (c == '\'' && !dquote) squote = !squote;
    if (c == '"' && !squote) dquote = !dquote;
    if (c != '!' || squote) {
      sb_putc(&sb, c);
      continue;
    }
    const char *ev = NULL;
    const char *q = p + 1;
    if (*q == '!') {
      ev = count ? history_get(history_last()) : NULL;
      q++;
    } else if (isdigit((unsigned char)*q) ||
               (*q == '-' && isdigit((unsigned char)q[1]))) {
      int neg = *q == '-';
      if (neg) q++;
      int n = atoi(q);
      while (isdigit((unsigned char)*q)) q++;
      ev = history_get(neg ? history_last() + 1 - n : n);
    } else if (isalpha((unsigned char)*q) || *q == '_' || *q == '.' ||
               *q == '/') {
      const char *e = q;
      while (*e && !strchr(" \t\n;&|()<>\"'", *e)) e++;
      ev = find_prefix(q, (size_t)(e - q));
      q = e;
    } else {
      sb_putc(&sb, c); /* lone '!' (e.g. pipeline negation) */
      continue;
    }
    if (!ev) {
      sh_warn("%.*s: event not found", (int)(q - p), p);
      sb_free(&sb);
      return NULL;
    }
    sb_puts(&sb, ev);
    *changed = 1;
    p = q - 1;
  }
  return sb_detach(&sb);
}

int history_builtin(int argc, char **argv) {
  ensure_ring();
  if (argc < 2) {
    for (int i = base; i <= history_last(); i++)
      printf("%5d  %s\n", i, history_get(i));
    return 0;
  }
  const char *opt = argv[1];
  if (strcmp(opt, "-c") == 0) {
    history_clear();
  } else if (strcmp(opt, "-w") == 0) {
    history_save();
  } else if (strcmp(opt, "-r") == 0) {
    load_file();
  } else if (strcmp(opt, "-a") == 0) {
    if (argc < 3) {
      sh_warn("history: -a: missing argument");
      return 2;
    }
    strbuf sb;
    sb_init(&sb);
    for (int i = 2; i < argc; i++) {
      if (i > 2) sb_putc(&sb, ' ');
      sb_puts(&sb, argv[i]);
    }
    history_add(sb.s);
    sb_free(&sb);
  } else if (strcmp(opt, "-n") == 0) {
    if (argc < 3 || !is_number(argv[2])) {
      sh_warn("history: -n: missing or invalid number");
      return 2;
    }
    int n = atoi(argv[2]);
    const char *h = history_get(n);
    if (!h) {
      sh_warn("history: %d: no such entry", n);
      return 1;
    }
    printf("%5d  %s\n", n, h);
  } else if (strcmp(opt, "-s") == 0) {
    if (argc < 3) {
      sh_warn("history: -s: missing argument");
      return 2;
    }
    for (int i = base; i <= history_last(); i++)
      if (strstr(history_get(i), argv[2])) printf("%5d  %s\n", i, history_get(i));
  } else if (is_number(opt)) {
    /* last n entries */
    int n = atoi(opt);
    int from = history_last() - n + 1;
    if (from < base) from = base;
    for (int i = from; i <= history_last() && i > 0; i++)
      printf("%5d  %s\n", i, history_get(i));
  } else {
    sh_warn("history: %s: invalid option", opt);
    fputs("usage: history [n | -c | -w | -r | -a cmd | -n n | -s word]\n",
          stderr);
    return 2;
  }
  return 0;
}
