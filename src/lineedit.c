#include "lineedit.h"

#include <dirent.h>
#include <sys/ioctl.h>
#include <termios.h>

#include "alias.h"
#include "builtins.h"
#include "common.h"
#include "exec.h"
#include "history.h"
#include "trap.h"
#include "vars.h"

struct editor {
  strbuf buf;
  size_t pos;
  const char *prompt; /* last line of the prompt */
  size_t pwidth;      /* its visible width */
  int cols;
  int histidx; /* history entry shown, or 0 for the line being edited */
  char *saved; /* the line being edited while browsing history */
  int last_was_tab;
};

static int tty_cols(void) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
    return ws.ws_col;
  return 80;
}

/* Width of text, skipping ANSI escape sequences. */
static size_t visible_width(const char *s) {
  size_t w = 0;
  while (*s) {
    if (*s == '\x1b') {
      s++;
      if (*s == '[') {
        s++;
        while (*s && !isalpha((unsigned char)*s)) s++;
        if (*s) s++;
      }
      continue;
    }
    if (((unsigned char)*s & 0xC0) != 0x80) w++;
    s++;
  }
  return w;
}

static void out(const char *s, size_t n) { xwrite(STDOUT_FILENO, s, n); }
static void outs(const char *s) { out(s, strlen(s)); }

static void refresh(struct editor *e) {
  strbuf sb;
  sb_init(&sb);
  size_t avail = (size_t)e->cols > e->pwidth + 1 ? e->cols - e->pwidth - 1 : 1;
  /* scroll horizontally so the cursor stays visible */
  size_t off = e->pos >= avail ? e->pos - avail + 1 : 0;
  size_t len = e->buf.len - off;
  if (len > avail) len = avail;
  sb_puts(&sb, "\r");
  sb_puts(&sb, e->prompt);
  sb_putn(&sb, e->buf.s ? e->buf.s + off : "", len);
  sb_puts(&sb, "\x1b[K\r");
  size_t col = e->pwidth + (e->pos - off);
  if (col) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "\x1b[%zuC", col);
    sb_puts(&sb, tmp);
  }
  out(sb.s, sb.len);
  sb_free(&sb);
}

static void insert(struct editor *e, const char *s, size_t n) {
  strbuf *b = &e->buf;
  sb_putn(b, s, n); /* grow */
  memmove(b->s + e->pos + n, b->s + e->pos, b->len - n - e->pos);
  memcpy(b->s + e->pos, s, n);
  e->pos += n;
}

static void delete_range(struct editor *e, size_t from, size_t to) {
  strbuf *b = &e->buf;
  memmove(b->s + from, b->s + to, b->len - to);
  b->len -= to - from;
  b->s[b->len] = '\0';
  if (e->pos > to)
    e->pos -= to - from;
  else if (e->pos > from)
    e->pos = from;
}

static void set_line(struct editor *e, const char *s) {
  e->buf.len = 0;
  sb_puts(&e->buf, s);
  e->pos = e->buf.len;
}

static void history_move(struct editor *e, int dir) {
  int last = history_last();
  if (!last) return;
  int idx = e->histidx;
  if (dir < 0) {
    idx = idx == 0 ? last : idx - 1;
    if (idx < history_first()) return;
  } else {
    if (idx == 0) return;
    idx = idx + 1 > last ? 0 : idx + 1;
  }
  if (e->histidx == 0) {
    free(e->saved);
    e->saved = xstrdup(e->buf.s ? e->buf.s : "");
  }
  e->histidx = idx;
  set_line(e, idx ? history_get(idx) : e->saved);
}

/* ---- completion ---- */

struct cands {
  char **v;
  size_t n, cap;
};

static void cand_add(struct cands *c, const char *s, size_t n) {
  for (size_t i = 0; i < c->n; i++)
    if (strlen(c->v[i]) == n && strncmp(c->v[i], s, n) == 0) return;
  if (c->n == c->cap) {
    c->cap = c->cap ? c->cap * 2 : 32;
    c->v = xrealloc(c->v, c->cap * sizeof(char *));
  }
  c->v[c->n++] = xstrndup(s, n);
}

static int cmp_cand(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

static void complete_files(struct cands *c, const char *word, int dirs_and_exec) {
  const char *slash = strrchr(word, '/');
  char *dir;
  const char *prefix;
  if (slash) {
    dir = xstrndup(word, (size_t)(slash - word + 1));
    prefix = slash + 1;
  } else {
    dir = xstrdup("");
    prefix = word;
  }
  /* expand a leading ~ for the lookup only */
  char *lookdir;
  const char *home = var_get("HOME");
  if (dir[0] == '~' && (dir[1] == '/' || dir[1] == '\0') && home) {
    size_t n = strlen(home) + strlen(dir);
    lookdir = xmalloc(n + 1);
    snprintf(lookdir, n + 1, "%s%s", home, dir + 1);
  } else {
    lookdir = xstrdup(*dir ? dir : ".");
  }
  DIR *d = opendir(lookdir);
  if (d) {
    size_t pl = strlen(prefix);
    struct dirent *de;
    while ((de = readdir(d))) {
      const char *nm = de->d_name;
      if (strncmp(nm, prefix, pl) != 0) continue;
      if (nm[0] == '.' && prefix[0] != '.') continue;
      if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;
      strbuf full;
      sb_init(&full);
      sb_puts(&full, lookdir);
      sb_putc(&full, '/');
      sb_puts(&full, nm);
      struct stat st;
      int isdir = stat(full.s, &st) == 0 && S_ISDIR(st.st_mode);
      if (dirs_and_exec && !isdir && access(full.s, X_OK) != 0) {
        sb_free(&full);
        continue;
      }
      sb_free(&full);
      strbuf cand;
      sb_init(&cand);
      sb_puts(&cand, dir);
      sb_puts(&cand, nm);
      if (isdir) sb_putc(&cand, '/');
      cand_add(c, cand.s, cand.len);
      sb_free(&cand);
    }
    closedir(d);
  }
  free(dir);
  free(lookdir);
}

static void complete_commands(struct cands *c, const char *word) {
  size_t wl = strlen(word);
  for (const struct builtin *b = builtin_table(); b->name; b++)
    if (strncmp(b->name, word, wl) == 0) cand_add(c, b->name, strlen(b->name));
  static const char *const kws[] = {"if",   "then", "else", "elif", "fi",
                                    "case", "esac", "for",  "while",
                                    "until", "do",  "done", NULL};
  for (int i = 0; kws[i]; i++)
    if (strncmp(kws[i], word, wl) == 0) cand_add(c, kws[i], strlen(kws[i]));
  const char *path = var_get("PATH");
  if (!path) return;
  for (const char *p = path;;) {
    const char *colon = strchr(p, ':');
    size_t dl = colon ? (size_t)(colon - p) : strlen(p);
    char *dir = dl ? xstrndup(p, dl) : xstrdup(".");
    DIR *d = opendir(dir);
    if (d) {
      struct dirent *de;
      while ((de = readdir(d))) {
        if (strncmp(de->d_name, word, wl) != 0 || de->d_name[0] == '.') continue;
        size_t n = strlen(dir) + strlen(de->d_name) + 2;
        char *full = xmalloc(n);
        snprintf(full, n, "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode) &&
            access(full, X_OK) == 0)
          cand_add(c, de->d_name, strlen(de->d_name));
        free(full);
      }
      closedir(d);
    }
    free(dir);
    if (!colon) break;
    p = colon + 1;
  }
}

static void insert_escaped(struct editor *e, const char *s) {
  for (; *s; s++) {
    if (strchr(" \t'\"\\$`&|;<>()*?[]#!{}", *s)) insert(e, "\\", 1);
    insert(e, s, 1);
  }
}

static void show_candidates(struct editor *e, struct cands *c) {
  size_t maxw = 0;
  for (size_t i = 0; i < c->n; i++)
    if (strlen(c->v[i]) > maxw) maxw = strlen(c->v[i]);
  size_t colw = maxw + 2;
  size_t ncols = (size_t)e->cols / colw;
  if (ncols == 0) ncols = 1;
  size_t nrows = (c->n + ncols - 1) / ncols;
  outs("\r\n");
  for (size_t r = 0; r < nrows; r++) {
    for (size_t col = 0; col < ncols; col++) {
      size_t i = col * nrows + r;
      if (i >= c->n) break;
      outs(c->v[i]);
      if (col + 1 < ncols && (col + 1) * nrows + r < c->n)
        for (size_t k = strlen(c->v[i]); k < colw; k++) out(" ", 1);
    }
    outs("\r\n");
  }
}

static void complete(struct editor *e) {
  char *b = e->buf.s ? e->buf.s : "";
  size_t start = e->pos;
  while (start > 0 && !strchr(" \t;|&(<>", b[start - 1])) start--;
  /* command position: only blanks and separators before the word */
  size_t k = start;
  while (k > 0 && (b[k - 1] == ' ' || b[k - 1] == '\t')) k--;
  int cmdpos = k == 0 || strchr(";|&(", b[k - 1]);

  /* the word, with backslash escapes removed */
  strbuf w;
  sb_init(&w);
  for (size_t i = start; i < e->pos; i++) {
    if (b[i] == '\\' && i + 1 < e->pos) i++;
    sb_putc(&w, b[i]);
  }
  const char *word = w.s ? w.s : "";

  struct cands c = {0};
  if (cmdpos && !strchr(word, '/'))
    complete_commands(&c, word);
  else
    complete_files(&c, word, cmdpos);

  if (c.n == 1) {
    delete_range(e, start, e->pos);
    insert_escaped(e, c.v[0]);
    size_t l = strlen(c.v[0]);
    if (l && c.v[0][l - 1] != '/') insert(e, " ", 1);
  } else if (c.n > 1) {
    qsort(c.v, c.n, sizeof(char *), cmp_cand);
    size_t common = strlen(c.v[0]);
    for (size_t i = 1; i < c.n; i++) {
      size_t j = 0;
      while (j < common && c.v[i][j] == c.v[0][j]) j++;
      common = j;
    }
    if (common > strlen(word)) {
      char *pre = xstrndup(c.v[0], common);
      delete_range(e, start, e->pos);
      insert_escaped(e, pre);
      free(pre);
    } else if (e->last_was_tab) {
      show_candidates(e, &c);
    } else {
      out("\a", 1);
    }
  } else {
    out("\a", 1);
  }
  for (size_t i = 0; i < c.n; i++) free(c.v[i]);
  free(c.v);
  sb_free(&w);
}

/* ---- main loop ---- */

static int read_byte(void) {
  unsigned char c;
  for (;;) {
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == 1) return c;
    if (n < 0 && errno == EINTR) {
      if (got_sigint) return 3;
      continue;
    }
    return -1;
  }
}

static char *read_plain(const char *prompt) {
  /* not a terminal: no editing, read byte by byte so we never consume
   * input meant for the commands we run */
  xwrite(STDERR_FILENO, prompt, strlen(prompt));
  strbuf sb;
  sb_init(&sb);
  for (;;) {
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n < 0 && errno == EINTR) {
      if (got_sigint) {
        got_sigint = 0;
        sb_free(&sb);
        errno = EINTR;
        return NULL;
      }
      continue;
    }
    if (n <= 0) {
      if (sb.len == 0) {
        sb_free(&sb);
        errno = 0;
        return NULL;
      }
      sb_putc(&sb, '\n');
      break;
    }
    sb_putc(&sb, (char)c);
    if (c == '\n') break;
  }
  return sb_detach(&sb);
}

char *lineedit_read(const char *prompt) {
  got_sigint = 0;
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return read_plain(prompt);
  const char *term = getenv("TERM");
  if (term && strcmp(term, "dumb") == 0) return read_plain(prompt);

  struct termios orig, raw;
  if (tcgetattr(STDIN_FILENO, &orig) < 0) return read_plain(prompt);
  raw = orig;
  raw.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSADRAIN, &raw);

  struct editor e;
  memset(&e, 0, sizeof(e));
  sb_init(&e.buf);
  sb_puts(&e.buf, "");
  e.cols = tty_cols();
  /* print the full prompt once; redraws only repeat its last line */
  const char *nl = strrchr(prompt, '\n');
  if (nl) out(prompt, (size_t)(nl - prompt + 1));
  e.prompt = nl ? nl + 1 : prompt;
  e.pwidth = visible_width(e.prompt);
  refresh(&e);

  int result = 0; /* 1 = line, -1 = EOF, -2 = interrupt */
  while (!result) {
    int c = read_byte();
    int was_tab = e.last_was_tab;
    e.last_was_tab = 0;
    switch (c) {
      case -1:
        result = e.buf.len ? 1 : -1;
        break;
      case '\r':
      case '\n':
        result = 1;
        break;
      case 3: /* ^C */
        e.pos = e.buf.len;
        refresh(&e);
        outs("^C");
        result = -2;
        break;
      case 4: /* ^D */
        if (e.buf.len == 0)
          result = -1;
        else if (e.pos < e.buf.len)
          delete_range(&e, e.pos, e.pos + 1);
        break;
      case 127:
      case 8:
        if (e.pos > 0) delete_range(&e, e.pos - 1, e.pos);
        break;
      case 1: /* ^A */
        e.pos = 0;
        break;
      case 5: /* ^E */
        e.pos = e.buf.len;
        break;
      case 2: /* ^B */
        if (e.pos > 0) e.pos--;
        break;
      case 6: /* ^F */
        if (e.pos < e.buf.len) e.pos++;
        break;
      case 11: /* ^K */
        delete_range(&e, e.pos, e.buf.len);
        break;
      case 21: /* ^U */
        delete_range(&e, 0, e.pos);
        break;
      case 23: { /* ^W */
        size_t p = e.pos;
        while (p > 0 && e.buf.s[p - 1] == ' ') p--;
        while (p > 0 && e.buf.s[p - 1] != ' ') p--;
        delete_range(&e, p, e.pos);
        break;
      }
      case 12: /* ^L */
        outs("\x1b[H\x1b[2J");
        if (nl) out(prompt, (size_t)(nl - prompt + 1));
        break;
      case 16: /* ^P */
        history_move(&e, -1);
        break;
      case 14: /* ^N */
        history_move(&e, 1);
        break;
      case '\t':
        e.last_was_tab = was_tab; /* a second tab lists the candidates */
        complete(&e);
        e.last_was_tab = 1;
        break;
      case 27: { /* escape sequences */
        int a = read_byte(), b = read_byte();
        if (a == '[' || a == 'O') {
          if (b >= '0' && b <= '9') {
            int t = read_byte();
            if (t == '~') {
              if (b == '3' && e.pos < e.buf.len)
                delete_range(&e, e.pos, e.pos + 1);
              else if (b == '1' || b == '7')
                e.pos = 0;
              else if (b == '4' || b == '8')
                e.pos = e.buf.len;
            }
          } else if (b == 'A') {
            history_move(&e, -1);
          } else if (b == 'B') {
            history_move(&e, 1);
          } else if (b == 'C') {
            if (e.pos < e.buf.len) e.pos++;
          } else if (b == 'D') {
            if (e.pos > 0) e.pos--;
          } else if (b == 'H') {
            e.pos = 0;
          } else if (b == 'F') {
            e.pos = e.buf.len;
          }
        }
        break;
      }
      default:
        if (c >= 32) {
          char ch = (char)c;
          int at_end = e.pos == e.buf.len;
          insert(&e, &ch, 1);
          /* typing at the end of a line that fits: just echo it */
          if (at_end && e.pwidth + e.buf.len + 1 < (size_t)e.cols) {
            out(&ch, 1);
            continue;
          }
        }
    }
    if (!result) refresh(&e);
  }
  outs("\r\n");
  tcsetattr(STDIN_FILENO, TCSADRAIN, &orig);
  free(e.saved);
  if (result == -2) {
    got_sigint = 0;
    sb_free(&e.buf);
    errno = EINTR;
    return NULL;
  }
  if (result == -1) {
    sb_free(&e.buf);
    errno = 0;
    return NULL;
  }
  sb_putc(&e.buf, '\n');
  return sb_detach(&e.buf);
}
