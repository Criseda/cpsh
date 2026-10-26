#include "lineedit.h"

#include <dirent.h>
#include <glob.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <wchar.h>
#include <wctype.h>

#include "alias.h"
#include "builtins.h"
#include "common.h"
#include "exec.h"
#include "history.h"
#include "jobs.h"
#include "shell.h"
#include "trap.h"
#include "vars.h"

struct editor {
  strbuf buf;
  size_t pos;
  const char *full;   /* the whole prompt */
  size_t head;        /* length of its lines before the last */
  const char *prompt; /* last line of the prompt */
  size_t pwidth;      /* its visible width */
  int cols;
  int histidx; /* history entry shown, or 0 for the line being edited */
  char *saved; /* the line being edited while browsing history */
  int last_was_tab;
  int unget; /* a key pushed back, or -1 */
  struct termios *orig, *raw;

  /* vi mode */
  int vi, cmd;         /* vi mode; in command mode */
  int ins_count;       /* repeat the text inserted this many times */
  int ins_change;      /* insert mode belongs to a change command */
  size_t ins_start;    /* where insert mode began */
  int replace;         /* R: typing overwrites */
  char *rorig;         /* the line before R */
  strbuf rec;          /* keys of the command being run, for `.` */
  strbuf replay;       /* keys to read before the terminal: `.` and @x */
  size_t replaypos;
  int macros;          /* @x expansions since the last key typed */
  char *undo;          /* the line before the last change, for u */
  size_t undo_pos;
  char *orig_line;     /* for U: the line when command mode was first entered,
                        * or as taken from the history */
};

static int tty_cols(void) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
    return ws.ws_col;
  return 80;
}

/* ---- characters ----
 * The line is kept as bytes, but the cursor moves and edits act on whole
 * UTF-8 characters, and the display allows for their widths. */

static int is_cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

/* Start of the character after the one at p. */
static size_t char_next(const char *s, size_t len, size_t p) {
  if (p < len) p++;
  while (p < len && is_cont(s[p])) p++;
  return p;
}

/* Start of the character before p. */
static size_t char_prev(const char *s, size_t p) {
  if (p > 0) p--;
  while (p > 0 && is_cont(s[p])) p--;
  return p;
}

/* Columns taken on the terminal by the n-byte character at s. */
static size_t char_width(const char *s, size_t n) {
  mbstate_t st;
  memset(&st, 0, sizeof(st));
  wchar_t wc;
  size_t r = mbrtowc(&wc, s, n, &st);
  if (r == (size_t)-1 || r == (size_t)-2) return 1;
  int w = wcwidth(wc);
  return w < 0 ? 1 : (size_t)w;
}

/* Width of text, skipping ANSI escape sequences. */
static size_t visible_width(const char *s) {
  size_t w = 0, len = strlen(s), p = 0;
  while (p < len) {
    if (s[p] == '\x1b') {
      p++;
      if (s[p] == '[') {
        p++;
        while (p < len && !isalpha((unsigned char)s[p])) p++;
        if (p < len) p++;
      }
      continue;
    }
    size_t q = char_next(s, len, p);
    w += char_width(s + p, q - p);
    p = q;
  }
  return w;
}

static void out(const char *s, size_t n) { xwrite(STDOUT_FILENO, s, n); }
static void outs(const char *s) { out(s, strlen(s)); }

static void refresh(struct editor *e) {
  strbuf sb;
  sb_init(&sb);
  const char *s = e->buf.s ? e->buf.s : "";
  size_t avail = (size_t)e->cols > e->pwidth + 1 ? e->cols - e->pwidth - 1 : 1;
  /* scroll horizontally so the cursor stays visible: show as much of the
   * line before it as fits, then what fits after */
  size_t off = e->pos, before = 0;
  while (off > 0) {
    size_t p = char_prev(s, off);
    size_t w = char_width(s + p, off - p);
    if (before + w >= avail) break;
    before += w;
    off = p;
  }
  size_t end = off, shown = 0;
  while (end < e->buf.len) {
    size_t q = char_next(s, e->buf.len, end);
    size_t w = char_width(s + end, q - end);
    if (shown + w > avail) break;
    shown += w;
    end = q;
  }
  sb_puts(&sb, "\r");
  sb_puts(&sb, e->prompt);
  sb_putn(&sb, s + off, end - off);
  sb_puts(&sb, "\x1b[K\r");
  size_t col = e->pwidth + before;
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

/* Show history entry idx, or with 0 the line being edited. */
static void history_goto(struct editor *e, int idx) {
  if (e->histidx == 0) {
    free(e->saved);
    e->saved = xstrdup(e->buf.s ? e->buf.s : "");
  }
  e->histidx = idx;
  set_line(e, idx ? history_get(idx) : e->saved);
  free(e->orig_line);
  e->orig_line = xstrdup(e->buf.s);
}

/* Move dir entries through the history; 0 if there is none further. */
static int history_move(struct editor *e, int dir) {
  int last = history_last();
  if (!last) return 0;
  int idx = e->histidx;
  if (dir < 0) {
    idx = idx == 0 ? last : idx - 1;
    if (idx < history_first()) return 0;
  } else {
    if (idx == 0) return 0;
    idx = idx + 1 > last ? 0 : idx + 1;
  }
  history_goto(e, idx);
  return 1;
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

/* ---- input ---- */

#define JOB_EVENT (-3) /* read_byte(): a child changed state (set -b) */

static int read_byte(void) {
  unsigned char c;
  for (;;) {
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == 1) return c;
    if (n < 0 && errno == EINTR) {
      if (got_sigint) return 3;
      if (got_sigchld) return JOB_EVENT;
      continue;
    }
    return -1;
  }
}

/* Whether input arrives within ms milliseconds: tells a lone ESC from the
 * start of an escape sequence. */
static int input_pending(int ms) {
  struct pollfd p = {STDIN_FILENO, POLLIN, 0};
  return poll(&p, 1, ms) > 0;
}

/* The next key: pushed back, replayed (vi `.` and @x) or typed. Job
 * reports (set -b) are written over the line, which is then drawn again.
 * In vi mode keys are recorded, for `.`. */
static int getkey(struct editor *e) {
  int c;
  if (e->unget >= 0) {
    c = e->unget;
    e->unget = -1;
  } else if (e->replaypos < e->replay.len) {
    c = (unsigned char)e->replay.s[e->replaypos++];
    if (e->replaypos == e->replay.len) e->replay.len = e->replaypos = 0;
  } else {
    e->macros = 0;
    for (;;) {
      c = read_byte();
      if (c != JOB_EVENT) break;
      outs("\r\x1b[K");
      if (jobs_async_pending() && e->head) out(e->full, e->head);
      refresh(e);
    }
  }
  if (e->vi && c >= 0) sb_putc(&e->rec, (char)c);
  return c;
}

static void ungetkey(struct editor *e, int c) {
  e->unget = c;
  if (e->vi && c >= 0 && e->rec.len) e->rec.len--;
}

/* Keys read from escape sequences. */
enum { K_UP = 256, K_DOWN, K_RIGHT, K_LEFT, K_HOME, K_END, K_DEL, K_NONE };

/* Decode an escape sequence; a is the byte after ESC. */
static int read_escape(struct editor *e, int a) {
  if (a != '[' && a != 'O') return K_NONE;
  int b = getkey(e);
  if (b >= '0' && b <= '9') {
    if (getkey(e) != '~') return K_NONE;
    switch (b) {
      case '3': return K_DEL;
      case '1': case '7': return K_HOME;
      case '4': case '8': return K_END;
    }
    return K_NONE;
  }
  switch (b) {
    case 'A': return K_UP;
    case 'B': return K_DOWN;
    case 'C': return K_RIGHT;
    case 'D': return K_LEFT;
    case 'H': return K_HOME;
    case 'F': return K_END;
  }
  return K_NONE;
}

static void special_key(struct editor *e, int k) {
  switch (k) {
    case K_UP: history_move(e, -1); break;
    case K_DOWN: history_move(e, 1); break;
    case K_RIGHT: e->pos = char_next(e->buf.s, e->buf.len, e->pos); break;
    case K_LEFT: e->pos = char_prev(e->buf.s, e->pos); break;
    case K_HOME: e->pos = 0; break;
    case K_END: e->pos = e->buf.len; break;
    case K_DEL:
      delete_range(e, e->pos, char_next(e->buf.s, e->buf.len, e->pos));
      break;
  }
}

static int interrupt(struct editor *e) {
  e->pos = e->buf.len;
  refresh(e);
  outs("^C");
  return -2;
}

static void redraw_screen(struct editor *e) {
  outs("\x1b[H\x1b[2J");
  if (e->head) out(e->full, e->head);
}

/* ---- vi mode ---- */

/* Kept from line to line, as in vi. */
static char *vi_change;    /* keys of the last change, for `.` */
static char *vi_yank;      /* text last deleted or yanked */
static char *vi_pattern;   /* last history search */
static int vi_searchdir;   /* its direction: -1 older (`/`), 1 newer */
static int vi_findcmd;     /* last f F t T */
static char vi_findch[4];  /* and the character it looked for */
static size_t vi_findlen;

static void beep(void) { out("\a", 1); }

static int is_blank(int c) { return c == ' ' || c == '\t'; }

/* Character class for word motions: blank, word or punctuation. A bigword
 * is any run of non-blanks. */
static int cls(char ch, int big) {
  unsigned char c = (unsigned char)ch;
  if (is_blank(c)) return 0;
  return big || isalnum(c) || c == '_' || c >= 0x80 ? 1 : 2;
}

static size_t word_fwd(const struct editor *e, size_t p, int big) {
  const char *s = e->buf.s;
  size_t len = e->buf.len;
  if (p >= len) return len;
  int k = cls(s[p], big);
  if (k)
    while (p < len && cls(s[p], big) == k) p++;
  while (p < len && is_blank(s[p])) p++;
  return p;
}

static size_t word_back(const struct editor *e, size_t p, int big) {
  const char *s = e->buf.s;
  while (p > 0 && is_blank(s[p - 1])) p--;
  if (p > 0) {
    int k = cls(s[p - 1], big);
    while (p > 0 && cls(s[p - 1], big) == k) p--;
  }
  return p;
}

static size_t word_end(const struct editor *e, size_t p, int big) {
  const char *s = e->buf.s;
  size_t len = e->buf.len;
  if (p + 1 >= len) return p;
  p++;
  while (p < len && is_blank(s[p])) p++;
  if (p >= len) return len - 1;
  int k = cls(s[p], big);
  while (p + 1 < len && cls(s[p + 1], big) == k) p++;
  while (p > 0 && is_cont(s[p])) p--;
  return p;
}

/* Read the rest of the character whose first byte is c into buf (which
 * holds 4 bytes); its length. */
static size_t read_char(struct editor *e, int c, char *buf) {
  size_t n = 0;
  buf[n++] = (char)c;
  int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
  while (more-- > 0) {
    int k = getkey(e);
    if (k < 0 || !is_cont((char)k)) {
      if (k >= 0) ungetkey(e, k);
      break;
    }
    buf[n++] = (char)k;
  }
  return n;
}

/* The count'th character ch (n bytes) for f F t T. */
static int find_char(const struct editor *e, int cmd, const char *ch, size_t n,
                     int count, size_t *to) {
  const char *s = e->buf.s;
  size_t len = e->buf.len, p = e->pos;
  int fwd = cmd == 'f' || cmd == 't';
  while (count-- > 0) {
    size_t q = p;
    for (;;) {
      if (fwd) {
        q = char_next(s, len, q);
        if (q >= len) return 0;
      } else {
        if (q == 0) return 0;
        q = char_prev(s, q);
      }
      if (q + n <= len && memcmp(s + q, ch, n) == 0 &&
          (q + n == len || !is_cont(s[q + n])))
        break;
    }
    p = q;
  }
  if (cmd == 't') p = char_prev(s, p);
  if (cmd == 'T') p = char_next(s, len, p);
  *to = p;
  return 1;
}

/* Where motion c takes the cursor, count times. *incl is set when the
 * character it lands on belongs to an operator's range; op is the
 * operator (d c y), or 0 for a plain move. 0 if c is no motion or cannot
 * move. */
static int vi_motion(struct editor *e, int c, int count, int op, size_t *to,
                     int *incl) {
  const char *s = e->buf.s;
  size_t len = e->buf.len, p = e->pos;
  size_t last = char_prev(s, len);
  *incl = 0;
  switch (c) {
    case 'h':
    case 8:
    case 127:
      if (p == 0) return 0;
      while (count-- > 0 && p > 0) p = char_prev(s, p);
      *to = p;
      return 1;
    case 'l':
    case ' ':
      if (op ? p >= len : char_next(s, len, p) >= len) return 0;
      while (count-- > 0 && p < (op ? len : last)) p = char_next(s, len, p);
      *to = p;
      return 1;
    case 'w':
    case 'W':
      /* cw changes to the end of the word, like ce */
      if (op == 'c' && p < len && !is_blank(s[p]))
        return vi_motion(e, c == 'w' ? 'e' : 'E', count, op, to, incl);
      while (count-- > 0) p = word_fwd(e, p, c == 'W');
      *to = !op && p > last ? last : p;
      return 1;
    case 'b':
    case 'B':
      if (p == 0) return 0;
      while (count-- > 0) p = word_back(e, p, c == 'B');
      *to = p;
      return 1;
    case 'e':
    case 'E':
      if (!len) return 0;
      while (count-- > 0) p = word_end(e, p, c == 'E');
      *to = p;
      *incl = 1;
      return 1;
    case '0':
      *to = 0;
      return 1;
    case '^':
      p = 0;
      while (p < len && is_blank(s[p])) p++;
      *to = !op && p > last ? last : p;
      return 1;
    case '$':
      *to = last;
      *incl = len > 0;
      return 1;
    case '|':
      p = 0;
      while (--count > 0 && p < last) p = char_next(s, len, p);
      *to = p;
      return 1;
    case 'f':
    case 'F':
    case 't':
    case 'T': {
      int ch = getkey(e);
      if (ch < 0 || ch == 3 || ch == 27) return 0;
      vi_findcmd = c;
      vi_findlen = read_char(e, ch, vi_findch);
      if (!find_char(e, c, vi_findch, vi_findlen, count, to)) return 0;
      *incl = c == 'f' || c == 't';
      return 1;
    }
    case ';':
    case ',': {
      int cmd = vi_findcmd;
      if (!cmd) return 0;
      if (c == ',') {
        static const char rev[] = "fFFftTTt";
        cmd = rev[(strchr(rev, cmd) - rev) ^ 1];
      }
      if (!find_char(e, cmd, vi_findch, vi_findlen, count, to)) return 0;
      *incl = cmd == 'f' || cmd == 't';
      return 1;
    }
  }
  return 0;
}

static void save_undo(struct editor *e) {
  free(e->undo);
  e->undo = xstrdup(e->buf.s);
  e->undo_pos = e->pos;
}

static void vi_insert_mode(struct editor *e, int count, int change) {
  e->cmd = 0;
  e->ins_count = count;
  e->ins_start = e->pos;
  e->ins_change = change;
}

/* ESC in insert mode: repeat the text inserted as the count asked, and
 * go to command mode on the last character inserted. */
static void vi_command_mode(struct editor *e) {
  if (e->ins_count > 1 && !e->replace && e->pos > e->ins_start) {
    char *t = xstrndup(e->buf.s + e->ins_start, e->pos - e->ins_start);
    for (int i = 1; i < e->ins_count; i++) insert(e, t, strlen(t));
    free(t);
  }
  if (e->ins_change) {
    free(vi_change);
    vi_change = xstrndup(e->rec.s, e->rec.len);
  }
  if (!e->orig_line) e->orig_line = xstrdup(e->buf.s);
  e->ins_count = e->ins_change = e->replace = 0;
  free(e->rorig);
  e->rorig = NULL;
  e->cmd = 1;
  e->pos = char_prev(e->buf.s, e->pos);
}

/* Apply operator op (d c y) to [from, to). */
static void vi_op(struct editor *e, int op, size_t from, size_t to) {
  if (to > e->buf.len) to = e->buf.len;
  free(vi_yank);
  vi_yank = xstrndup(e->buf.s + from, to - from);
  if (op != 'y') {
    save_undo(e);
    delete_range(e, from, to);
  }
  e->pos = from;
  if (op == 'c') vi_insert_mode(e, 1, 1);
}

/* Read keys to be read before the terminal's. */
static void replay_push(struct editor *e, const char *s, size_t n) {
  strbuf nb;
  sb_init(&nb);
  sb_putn(&nb, s, n);
  if (e->replaypos < e->replay.len)
    sb_putn(&nb, e->replay.s + e->replaypos, e->replay.len - e->replaypos);
  sb_free(&e->replay);
  e->replay = nb;
  e->replaypos = 0;
}

/* Read a search pattern after / or ?, shown in place of the line. 1 when
 * entered, 0 when abandoned, -2 on interrupt. */
static int read_pattern(struct editor *e, int lead, strbuf *pat) {
  for (;;) {
    strbuf sb;
    sb_init(&sb);
    sb_puts(&sb, "\r");
    sb_putc(&sb, (char)lead);
    if (pat->len) sb_putn(&sb, pat->s, pat->len);
    sb_puts(&sb, "\x1b[K");
    out(sb.s, sb.len);
    sb_free(&sb);
    int c = getkey(e);
    if (c == 3) return -2;
    if (c < 0 || c == 27) return 0;
    if (c == '\r' || c == '\n') return 1;
    if (c == 127 || c == 8) {
      if (!pat->len) return 0;
      pat->len = char_prev(pat->s, pat->len);
      pat->s[pat->len] = '\0';
    } else if (c >= 32) {
      sb_putc(pat, (char)c);
    }
  }
}

/* Find pattern in the history from the entry shown, in direction dir. A
 * leading ^ anchors it to the start of the command. */
static int vi_search(struct editor *e, const char *pat, int dir) {
  int anchored = *pat == '^';
  if (anchored) pat++;
  size_t n = strlen(pat);
  if (!e->histidx && dir > 0) return 0;
  int i = e->histidx ? e->histidx : history_last() + 1;
  for (i += dir; i >= history_first() && i <= history_last() && i > 0; i += dir) {
    const char *h = history_get(i);
    if (anchored ? strncmp(h, pat, n) == 0 : strstr(h, pat) != NULL) {
      history_goto(e, i);
      e->pos = 0;
      return 1;
    }
  }
  return 0;
}

/* The bigword under the cursor, or just before it. */
static void bigword_at(const struct editor *e, size_t *from, size_t *to) {
  const char *s = e->buf.s;
  size_t len = e->buf.len, p = e->pos;
  if (p >= len) p = len ? len - 1 : 0;
  if (len && is_blank(s[p]) && p > 0 && !is_blank(s[p - 1])) p--;
  size_t a = p, b = p;
  while (a > 0 && !is_blank(s[a - 1])) a--;
  while (b < len && !is_blank(s[b])) b++;
  *from = a;
  *to = b;
}

/* Pathnames matching the bigword at the cursor, taken as a pattern with
 * a * appended when it has no pattern characters (= and *). */
static int vi_glob(const struct editor *e, size_t *from, size_t *to,
                   glob_t *g) {
  bigword_at(e, from, to);
  strbuf pat;
  sb_init(&pat);
  sb_putn(&pat, e->buf.s + *from, *to - *from);
  if (!strpbrk(pat.s ? pat.s : "", "*?[")) sb_putc(&pat, '*');
  int r = pglob(pat.s, g);
  sb_free(&pat);
  if (r != 0) {
    if (r == GLOB_NOMATCH) globfree(g);
    return 0;
  }
  return 1;
}

/* v: edit the line (or history entry count) with $VISUAL, else $EDITOR,
 * else vi; the result is the command line. 1 to accept it, 0 to stay. */
static int vi_edit(struct editor *e, int count) {
  const char *text = count ? history_get(count) : e->buf.s;
  if (!text) return 0;
  const char *ed = var_get("VISUAL");
  if (!ed || !*ed) ed = var_get("EDITOR");
  if (!ed || !*ed) ed = "vi";
  const char *tmpdir = var_get("TMPDIR");
  if (!tmpdir || !*tmpdir) tmpdir = "/tmp";
  strbuf path;
  sb_init(&path);
  sb_puts(&path, tmpdir);
  sb_puts(&path, "/cpsh-viXXXXXX");
  int fd = mkstemp(path.s);
  if (fd < 0) {
    sb_free(&path);
    return 0;
  }
  int ok = xwrite(fd, text, strlen(text)) == 0 && xwrite(fd, "\n", 1) == 0;
  close(fd);
  int accepted = 0;
  if (ok) {
    strbuf cmd;
    sb_init(&cmd);
    sb_puts(&cmd, ed);
    sb_putc(&cmd, ' ');
    sh_quote(&cmd, path.s);
    tcsetattr(STDIN_FILENO, TCSADRAIN, e->orig);
    outs("\n");
    pid_t pid = forkshell(NULL, FORK_NOJOB);
    if (pid == 0) {
      execl("/bin/sh", "sh", "-c", cmd.s, (char *)NULL);
      _exit(127);
    }
    sb_free(&cmd);
    FILE *f = waitforpid(pid) == 0 ? fopen(path.s, "r") : NULL;
    if (f) {
      strbuf sb;
      sb_init(&sb);
      char b[4096];
      size_t n;
      while ((n = fread(b, 1, sizeof(b), f)) > 0) sb_putn(&sb, b, n);
      fclose(f);
      while (sb.len && sb.s[sb.len - 1] == '\n') sb.len--;
      set_line(e, "");
      if (sb.len) sb_putn(&e->buf, sb.s, sb.len);
      sb_free(&sb);
      out(e->buf.s, e->buf.len); /* show what runs */
      accepted = 1;
    }
    tcsetattr(STDIN_FILENO, TCSADRAIN, e->raw);
    if (!accepted && e->head) out(e->full, e->head);
  }
  unlink(path.s);
  sb_free(&path);
  return accepted;
}

/* Run one command-mode command, starting with key c. Returns as the main
 * loop's result: 0 to go on, 1 to accept the line, -1 at end of input,
 * -2 on interrupt. */
static int vi_command(struct editor *e, int c) {
#define NEXT(k)                              \
  do {                                       \
    (k) = getkey(e);                         \
    if ((k) == 3) return interrupt(e);       \
    if ((k) < 0) return e->buf.len ? 1 : -1; \
  } while (0)
  int count = 0;
  if (c == 3) return interrupt(e);
  if (c < 0) return e->buf.len ? 1 : -1;
  while ((c >= '1' && c <= '9') || (count && c == '0')) {
    count = count * 10 + (c - '0');
    NEXT(c);
  }
  if (c == 27) {
    int a = e->replaypos >= e->replay.len && e->unget < 0 && input_pending(30)
                ? getkey(e)
                : -1;
    if (a != '[' && a != 'O') {
      if (a >= 0) ungetkey(e, a);
    } else {
      switch (read_escape(e, a)) {
        case K_UP: c = 'k'; break;
        case K_DOWN: c = 'j'; break;
        case K_RIGHT: c = 'l'; break;
        case K_LEFT: c = 'h'; break;
        case K_HOME: c = '0'; break;
        case K_END: c = '$'; break;
        case K_DEL: c = 'x'; break;
      }
    }
    if (c == 27) {
      beep();
      return 0;
    }
  }
  int n = count ? count : 1;
  size_t len = e->buf.len, pos = e->pos;
  int change = 0; /* a change that `.` repeats */
  size_t to;
  int incl;
  switch (c) {
    case '\r':
    case '\n':
      return 1;
    case 4: /* ^D */
      if (!len) return -1;
      break;
    case 12: /* ^L */
      redraw_screen(e);
      break;
    case 'i':
    case 'a':
    case 'I':
    case 'A':
      save_undo(e);
      if (c == 'a') e->pos = char_next(e->buf.s, len, pos);
      if (c == 'I') e->pos = 0;
      if (c == 'A') e->pos = len;
      vi_insert_mode(e, n, 1);
      break;
    case 'R':
      save_undo(e);
      vi_insert_mode(e, 1, 1);
      e->replace = 1;
      e->rorig = xstrdup(e->buf.s);
      break;
    case 'x':
    case 'X': {
      if (c == 'x' ? !len : !pos) goto fail;
      size_t p = pos;
      for (int i = 0; i < n; i++)
        p = c == 'x' ? char_next(e->buf.s, len, p) : char_prev(e->buf.s, p);
      if (c == 'x')
        vi_op(e, 'd', pos, p);
      else
        vi_op(e, 'd', p, pos);
      change = 1;
      break;
    }
    case 'D':
    case 'C':
      vi_op(e, c == 'D' ? 'd' : 'c', pos, len);
      change = 1;
      break;
    case 'S':
      vi_op(e, 'c', 0, len);
      change = 1;
      break;
    case 'Y':
      vi_op(e, 'y', pos, len);
      break;
    case 'd':
    case 'c':
    case 'y': {
      int m, count2 = 0;
      NEXT(m);
      while ((m >= '1' && m <= '9') || (count2 && m == '0')) {
        count2 = count2 * 10 + (m - '0');
        NEXT(m);
      }
      if (m == c) { /* dd cc yy: the whole line */
        vi_op(e, c, 0, len);
      } else {
        if (!vi_motion(e, m, n * (count2 ? count2 : 1), c, &to, &incl))
          goto fail;
        if (to < pos)
          vi_op(e, c, to, pos);
        else
          vi_op(e, c, pos, incl ? char_next(e->buf.s, len, to) : to);
      }
      change = c != 'y';
      break;
    }
    case 'p':
    case 'P':
      if (!vi_yank || !*vi_yank) goto fail;
      save_undo(e);
      if (c == 'p') e->pos = char_next(e->buf.s, len, pos);
      for (int i = 0; i < n; i++) insert(e, vi_yank, strlen(vi_yank));
      e->pos = char_prev(e->buf.s, e->pos);
      change = 1;
      break;
    case 'r': {
      char rc[4];
      int ch;
      NEXT(ch);
      if (ch == 27) break;
      size_t rn = read_char(e, ch, rc);
      size_t end = pos;
      for (int i = 0; i < n; i++) {
        if (end >= len) goto fail;
        end = char_next(e->buf.s, len, end);
      }
      save_undo(e);
      delete_range(e, pos, end);
      e->pos = pos;
      for (int i = 0; i < n; i++) insert(e, rc, rn);
      e->pos -= rn;
      change = 1;
      break;
    }
    case '~':
      if (!len) goto fail;
      save_undo(e);
      for (int i = 0; i < n && e->pos < e->buf.len; i++) {
        size_t at = e->pos, next = char_next(e->buf.s, e->buf.len, at);
        mbstate_t st;
        memset(&st, 0, sizeof(st));
        wchar_t wc;
        size_t r = mbrtowc(&wc, e->buf.s + at, next - at, &st);
        if (r != (size_t)-1 && r != (size_t)-2 && r != 0) {
          wchar_t other = iswlower((wint_t)wc) ? (wchar_t)towupper((wint_t)wc)
                                               : (wchar_t)towlower((wint_t)wc);
          char mb[MB_LEN_MAX];
          memset(&st, 0, sizeof(st));
          size_t mn = wcrtomb(mb, other, &st);
          if (other != wc && mn != (size_t)-1) {
            delete_range(e, at, next);
            e->pos = at;
            insert(e, mb, mn);
            next = e->pos;
          }
        }
        e->pos = next;
      }
      change = 1;
      break;
    case 'u': {
      if (!e->undo) goto fail;
      char *cur = xstrdup(e->buf.s);
      set_line(e, e->undo);
      e->pos = e->undo_pos;
      free(e->undo);
      e->undo = cur;
      e->undo_pos = pos;
      break;
    }
    case 'U':
      save_undo(e);
      set_line(e, e->orig_line ? e->orig_line : "");
      e->pos = 0;
      break;
    case '.': {
      if (!vi_change) goto fail;
      const char *k = vi_change;
      strbuf r;
      sb_init(&r);
      if (count) {
        char b[16];
        snprintf(b, sizeof(b), "%d", count);
        sb_puts(&r, b);
        while (isdigit((unsigned char)*k)) k++;
      }
      sb_puts(&r, k);
      replay_push(e, r.s, r.len);
      sb_free(&r);
      break;
    }
    case '_': {
      /* the count'th bigword of the previous command, else its last */
      const char *h = history_last() ? history_get(history_last()) : NULL;
      const char *w = NULL;
      size_t wl = 0;
      for (int i = 1; h && *h; i++) {
        while (is_blank(*h) || *h == '\n') h++;
        if (!*h) break;
        const char *s = h;
        while (*h && !is_blank(*h) && *h != '\n') h++;
        w = s;
        wl = (size_t)(h - s);
        if (i == count) break;
      }
      if (!w) goto fail;
      save_undo(e);
      if (len) {
        e->pos++;
        insert(e, " ", 1);
      }
      insert(e, w, wl);
      vi_insert_mode(e, 1, 1);
      break;
    }
    case '#':
      e->pos = 0;
      insert(e, "#", 1);
      return 1;
    case '\\': {
      size_t from;
      bigword_at(e, &from, &to);
      e->pos = to;
      complete(e);
      vi_insert_mode(e, 1, 0);
      break;
    }
    case '=':
    case '*': {
      size_t from;
      glob_t g;
      if (!vi_glob(e, &from, &to, &g)) goto fail;
      if (c == '=') {
        struct cands cs = {g.gl_pathv, g.gl_pathc, g.gl_pathc};
        show_candidates(e, &cs);
        if (e->head) out(e->full, e->head);
      } else {
        save_undo(e);
        delete_range(e, from, to);
        e->pos = from;
        for (size_t i = 0; i < g.gl_pathc; i++) {
          insert_escaped(e, g.gl_pathv[i]);
          insert(e, " ", 1);
        }
        vi_insert_mode(e, 1, 1);
      }
      globfree(&g);
      break;
    }
    case '@': {
      int letter;
      NEXT(letter);
      char name[3] = {'_', (char)letter, '\0'};
      struct alias *a = alias_lookup(name);
      if (!a || ++e->macros > 100) goto fail;
      replay_push(e, a->value, strlen(a->value));
      break;
    }
    case 'v':
      if (vi_edit(e, count)) return 1;
      break;
    case 'k':
    case '-':
    case 'j':
    case '+': {
      int moved = 0;
      for (int i = 0; i < n && history_move(e, c == 'k' || c == '-' ? -1 : 1); i++)
        moved = 1;
      if (!moved) goto fail;
      e->pos = 0;
      break;
    }
    case 'G': {
      int idx = count ? count : history_first();
      if (!history_get(idx)) goto fail;
      history_goto(e, idx);
      e->pos = 0;
      break;
    }
    case '/':
    case '?': {
      strbuf pat;
      sb_init(&pat);
      sb_puts(&pat, "");
      int r = read_pattern(e, c, &pat);
      if (r == -2) {
        sb_free(&pat);
        return interrupt(e);
      }
      if (r == 1 && pat.len) {
        free(vi_pattern);
        vi_pattern = xstrdup(pat.s);
      }
      sb_free(&pat);
      if (r == 0) break;
      vi_searchdir = c == '/' ? -1 : 1;
      if (!vi_pattern || !vi_search(e, vi_pattern, vi_searchdir)) goto fail;
      break;
    }
    case 'n':
    case 'N':
      if (!vi_pattern ||
          !vi_search(e, vi_pattern, c == 'n' ? vi_searchdir : -vi_searchdir))
        goto fail;
      break;
    default:
      if (!vi_motion(e, c, n, 0, &to, &incl)) goto fail;
      e->pos = to;
      break;
  }
  if (change && e->cmd) {
    free(vi_change);
    vi_change = xstrndup(e->rec.s, e->rec.len);
  }
  return 0;
fail:
  beep();
  return 0;
#undef NEXT
}

/* ---- main loop ---- */

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
  jobs_async_begin();

  struct editor e;
  memset(&e, 0, sizeof(e));
  sb_init(&e.buf);
  sb_puts(&e.buf, "");
  sb_init(&e.rec);
  sb_init(&e.replay);
  e.unget = -1;
  e.orig = &orig;
  e.raw = &raw;
  e.vi = optval[OPT_vi];
  e.cols = tty_cols();
  /* print the full prompt once; redraws only repeat its last line */
  const char *nl = strrchr(prompt, '\n');
  e.full = prompt;
  e.head = nl ? (size_t)(nl - prompt + 1) : 0;
  if (e.head) out(prompt, e.head);
  e.prompt = prompt + e.head;
  e.pwidth = visible_width(e.prompt);
  refresh(&e);

  int result = 0; /* 1 = line, -1 = EOF, -2 = interrupt */
  while (!result) {
    if (e.cmd) e.rec.len = 0;
    int c = getkey(&e);
    if (e.cmd) {
      result = vi_command(&e, c);
      if (!result && e.cmd && e.buf.len && e.pos >= e.buf.len)
        e.pos = char_prev(e.buf.s, e.buf.len);
      while (e.pos > 0 && e.pos < e.buf.len && is_cont(e.buf.s[e.pos])) e.pos--;
      if (!result) refresh(&e);
      continue;
    }
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
        result = interrupt(&e);
        break;
      case 4: /* ^D */
        if (e.buf.len == 0)
          result = -1;
        else
          delete_range(&e, e.pos, char_next(e.buf.s, e.buf.len, e.pos));
        break;
      case 127:
      case 8:
        if (e.replace) {
          /* R: back over what was typed, putting the old text back: the
           * line before the cursor, then the old line after as many
           * characters as are left typed */
          if (e.pos > e.ins_start) {
            size_t p = char_prev(e.buf.s, e.pos);
            size_t q = e.ins_start, rl = strlen(e.rorig);
            for (size_t i = e.ins_start; i < p; i = char_next(e.buf.s, p, i))
              q = char_next(e.rorig, rl, q);
            e.buf.len = p;
            sb_puts(&e.buf, e.rorig + q);
            e.pos = p;
          }
        } else {
          delete_range(&e, char_prev(e.buf.s, e.pos), e.pos);
        }
        break;
      case 1: /* ^A */
        e.pos = 0;
        break;
      case 5: /* ^E */
        e.pos = e.buf.len;
        break;
      case 2: /* ^B */
        e.pos = char_prev(e.buf.s, e.pos);
        break;
      case 6: /* ^F */
        e.pos = char_next(e.buf.s, e.buf.len, e.pos);
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
        redraw_screen(&e);
        break;
      case 16: /* ^P */
        history_move(&e, -1);
        break;
      case 14: /* ^N */
        history_move(&e, 1);
        break;
      case 22: { /* ^V: the next key as it is */
        int k = getkey(&e);
        if (k >= 0) {
          char ch = (char)k;
          insert(&e, &ch, 1);
        }
        break;
      }
      case '\t':
        e.last_was_tab = was_tab; /* a second tab lists the candidates */
        complete(&e);
        e.last_was_tab = 1;
        break;
      case 27: /* escape sequences; in vi mode a lone ESC ends insert mode */
        if (e.vi) {
          if (e.replaypos >= e.replay.len && e.unget < 0 && input_pending(30)) {
            int a = getkey(&e);
            if (a == '[' || a == 'O') {
              special_key(&e, read_escape(&e, a));
              break;
            }
            ungetkey(&e, a);
          }
          vi_command_mode(&e);
        } else {
          special_key(&e, read_escape(&e, getkey(&e)));
        }
        break;
      default:
        if (c >= 32 && c < 256) {
          char ch = (char)c;
          if (e.replace && e.pos < e.buf.len) {
            /* a character's first byte takes the place of the one under
             * the cursor; the rest of its bytes follow it */
            if (!is_cont(ch))
              delete_range(&e, e.pos, char_next(e.buf.s, e.buf.len, e.pos));
            insert(&e, &ch, 1);
            break;
          }
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
  jobs_async_end();
  tcsetattr(STDIN_FILENO, TCSADRAIN, &orig);
  free(e.saved);
  free(e.undo);
  free(e.rorig);
  free(e.orig_line);
  sb_free(&e.rec);
  sb_free(&e.replay);
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
