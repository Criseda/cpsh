#include "common.h"
#include "exec.h"
#include "history.h"
#include "vars.h"

/* fc: list, edit and run commands from the history.
 *
 *   fc [-r] [-e editor] [first [last]]
 *   fc -l [-nr] [first [last]]
 *   fc -s [old=new] [first]
 *
 * first and last are a number (an entry), a negative number (counted back
 * from the fc command itself) or a string (the newest command starting with
 * it). The fc command itself is never selected. */

/* Resolve an operand to an entry between oldest and newest. Numbers out of
 * range are moved into it. 0 on success, -1 after an error. */
static int find_event(const char *s, int cur, int oldest, int newest, int *out) {
  const char *d = *s == '-' || *s == '+' ? s + 1 : s;
  if (is_number(d)) {
    long n = atol(d);
    long ev = *s == '-' ? cur - n : n;
    if (ev < oldest) ev = oldest;
    if (ev > newest) ev = newest;
    *out = (int)ev;
    return 0;
  }
  size_t len = strlen(s);
  for (int i = newest; i >= oldest; i--) {
    if (strncmp(history_get(i), s, len) == 0) {
      *out = i;
      return 0;
    }
  }
  sh_warn("fc: %s: no command found", s);
  return -1;
}

/* Run commands taken from the history, entering them in place of the fc
 * command. */
static int rerun(const char *text) {
  fputs(text, stderr);
  if (!*text || text[strlen(text) - 1] != '\n') fputc('\n', stderr);
  fflush(stderr);
  history_replace_current(text);
  return evalstring(text, 0);
}

/* Write entries first..last (stepping by dir) to a temporary file, run the
 * editor on it and return what it holds afterwards, or NULL if the editor
 * failed (*status is then its status). */
static char *edit(const char *editor, int first, int last, int dir, int *status) {
  const char *tmpdir = var_get("TMPDIR");
  if (!tmpdir || !*tmpdir) tmpdir = "/tmp";
  strbuf path;
  sb_init(&path);
  sb_puts(&path, tmpdir);
  sb_puts(&path, "/cpsh-fcXXXXXX");
  int fd = mkstemp(path.s);
  if (fd < 0) {
    sh_warn("fc: %s: %s", path.s, strerror(errno));
    sb_free(&path);
    *status = 1;
    return NULL;
  }
  strbuf sb;
  sb_init(&sb);
  for (int i = first;; i += dir) {
    sb_puts(&sb, history_get(i));
    sb_putc(&sb, '\n');
    if (i == last) break;
  }
  int ok = xwrite(fd, sb.s, sb.len) == 0;
  close(fd);
  sb.len = 0;
  if (!ok) {
    sh_warn("fc: %s: %s", path.s, strerror(errno));
    unlink(path.s);
    sb_free(&path);
    sb_free(&sb);
    *status = 1;
    return NULL;
  }

  /* the editor is a command line, as in $FCEDIT="vi -n" */
  sb_puts(&sb, editor);
  sb_putc(&sb, ' ');
  sh_quote(&sb, path.s);
  char *volatile cmd = sb_detach(&sb);
  struct jmploc jl, *saved = handler;
  if (setjmp(jl.buf)) {
    handler = saved;
    unlink(path.s);
    sb_free(&path);
    free(cmd);
    raise_exception(exception);
  }
  handler = &jl;
  *status = evalstring(cmd, 0);
  handler = saved;
  free(cmd);

  char *text = NULL;
  if (*status == 0) {
    FILE *f = fopen(path.s, "r");
    if (f) {
      sb_init(&sb);
      char buf[4096];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf), f)) > 0) sb_putn(&sb, buf, n);
      fclose(f);
      text = sb_detach(&sb);
    } else {
      sh_warn("fc: %s: %s", path.s, strerror(errno));
      *status = 1;
    }
  }
  unlink(path.s);
  sb_free(&path);
  return text;
}

int fc_builtin(int argc, char **argv) {
  int lflag = 0, nflag_ = 0, rflag = 0, sflag = 0;
  const char *editor = NULL;
  int i = 1;
  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--") == 0) {
      i++;
      break;
    }
    if (isdigit((unsigned char)a[1])) break; /* a negative number */
    for (const char *p = a + 1; *p; p++) {
      if (*p == 'l') {
        lflag = 1;
      } else if (*p == 'n') {
        nflag_ = 1;
      } else if (*p == 'r') {
        rflag = 1;
      } else if (*p == 's') {
        sflag = 1;
      } else if (*p == 'e') {
        if (p[1]) {
          editor = p + 1;
        } else if (++i < argc) {
          editor = argv[i];
        } else {
          sh_warn("fc: -e: option requires an argument");
          return 2;
        }
        break;
      } else {
        sh_warn("fc: -%c: invalid option", *p);
        fputs("usage: fc [-r] [-e editor] [first [last]]\n"
              "       fc -l [-nr] [first [last]]\n"
              "       fc -s [old=new] [first]\n",
              stderr);
        return 2;
      }
    }
  }
  if (editor && strcmp(editor, "-") == 0) { /* the historical fc -e - */
    sflag = 1;
    editor = NULL;
  }

  int cur = history_current();
  int oldest = history_first(), newest = cur - 1;
  if (newest < oldest || newest < 1) {
    sh_warn("fc: no history");
    return 1;
  }

  if (sflag) {
    const char *sub = i < argc && strchr(argv[i], '=') ? argv[i++] : NULL;
    if (argc - i > 1) {
      sh_warn("fc: too many arguments");
      return 2;
    }
    int ev = newest;
    if (i < argc && find_event(argv[i], cur, oldest, newest, &ev) < 0) return 1;
    const char *cmd = history_get(ev);
    strbuf sb;
    sb_init(&sb);
    const char *at = NULL;
    size_t oldlen = 0;
    if (sub) {
      oldlen = (size_t)(strchr(sub, '=') - sub);
      if (oldlen) {
        char *old = xstrndup(sub, oldlen);
        at = strstr(cmd, old);
        free(old);
      }
    }
    if (at) {
      sb_putn(&sb, cmd, (size_t)(at - cmd));
      sb_puts(&sb, sub + oldlen + 1);
      sb_puts(&sb, at + oldlen);
    } else {
      sb_puts(&sb, cmd);
    }
    sb_putc(&sb, '\n');
    char *text = sb_detach(&sb);
    struct jmploc jl, *saved = handler;
    if (setjmp(jl.buf)) {
      handler = saved;
      free(text);
      raise_exception(exception);
    }
    handler = &jl;
    int status = rerun(text);
    handler = saved;
    free(text);
    return status;
  }

  if (argc - i > 2) {
    sh_warn("fc: too many arguments");
    return 2;
  }
  int first, last;
  if (i < argc) {
    if (find_event(argv[i], cur, oldest, newest, &first) < 0) return 1;
    if (i + 1 < argc) {
      if (find_event(argv[i + 1], cur, oldest, newest, &last) < 0) return 1;
    } else {
      last = lflag ? newest : first;
    }
  } else if (lflag) {
    first = newest - 15 < oldest ? oldest : newest - 15;
    last = newest;
  } else {
    first = last = newest;
  }
  if (rflag) {
    int t = first;
    first = last;
    last = t;
  }
  int dir = first <= last ? 1 : -1;

  if (lflag) {
    for (int ev = first;; ev += dir) {
      if (nflag_)
        printf("\t%s\n", history_get(ev));
      else
        printf("%d\t%s\n", ev, history_get(ev));
      if (ev == last) break;
    }
    return 0;
  }

  if (!editor) editor = var_get("FCEDIT");
  if (!editor || !*editor) editor = "ed";
  int status;
  char *text = edit(editor, first, last, dir, &status);
  if (!text) return status;
  struct jmploc jl, *saved = handler;
  if (setjmp(jl.buf)) {
    handler = saved;
    free(text);
    raise_exception(exception);
  }
  handler = &jl;
  status = *text ? rerun(text) : 0;
  handler = saved;
  free(text);
  return status;
}
