#include "prompt.h"

#include <pwd.h>
#include <time.h>

#include "common.h"
#include "expand.h"
#include "history.h"
#include "vars.h"

/* Directory for display: $HOME (only as a whole path component) becomes ~. */
static void display_cwd(strbuf *sb) {
  const char *pwd = var_get("PWD");
  char buf[PATH_MAX];
  if (!pwd || *pwd != '/') pwd = getcwd(buf, sizeof(buf)) ? buf : "?";
  const char *home = var_get("HOME");
  size_t hl = home ? strlen(home) : 0;
  while (hl > 1 && home[hl - 1] == '/') hl--;
  if (hl > 1 && strncmp(pwd, home, hl) == 0 &&
      (pwd[hl] == '\0' || pwd[hl] == '/')) {
    sb_putc(sb, '~');
    sb_puts(sb, pwd + hl);
  } else {
    sb_puts(sb, pwd);
  }
}

static const char *username(void) {
  static char *cached;
  if (cached) return cached;
  const char *u = var_get("USER");
  if (!u) u = var_get("LOGNAME");
  if (!u) {
    struct passwd *pw = getpwuid(getuid());
    u = pw ? pw->pw_name : "unknown";
  }
  return cached = xstrdup(u);
}

static const char *hostname(void) {
  static char host[256];
  if (!host[0] && gethostname(host, sizeof(host) - 1) != 0)
    strcpy(host, "unknown");
  return host;
}

/* Expand a message (as for PS1); on an expansion error use it as is. */
static void put_expanded(strbuf *sb, const char *s) {
  stackmark m = stmark();
  struct jmploc jl, *saved = handler;
  if (setjmp(jl.buf) == 0) {
    handler = &jl;
    sb_puts(sb, expand_prompt(s));
  } else {
    sb_puts(sb, s);
  }
  handler = saved;
  strelease(m);
}

/* ---- mail checking ---- */

static struct mailfile {
  char *path;
  time_t mtime;
} *mailfiles;
static size_t nmailfiles;
static time_t last_mail_check;

/* Has the file changed since it was last seen? A file seen for the first
 * time only has its time recorded. */
static int mail_changed(const char *path) {
  struct stat st;
  time_t mtime = stat(path, &st) == 0 && st.st_size > 0 ? st.st_mtime : 0;
  for (size_t i = 0; i < nmailfiles; i++) {
    if (strcmp(mailfiles[i].path, path) != 0) continue;
    int changed = mtime != 0 && mtime != mailfiles[i].mtime;
    mailfiles[i].mtime = mtime;
    return changed;
  }
  mailfiles = xrealloc(mailfiles, (nmailfiles + 1) * sizeof(*mailfiles));
  mailfiles[nmailfiles].path = xstrdup(path);
  mailfiles[nmailfiles].mtime = mtime;
  nmailfiles++;
  return 0;
}

void mail_check(void) {
  const char *mc = var_get("MAILCHECK");
  long interval = mc && is_number(mc) ? atol(mc) : 600;
  time_t now = time(NULL);
  if (last_mail_check && now - last_mail_check < interval) return;
  last_mail_check = now;
  const char *mailpath = var_get("MAILPATH");
  if (!mailpath || !*mailpath) {
    const char *mail = var_get("MAIL");
    if (mail && *mail && mail_changed(mail)) fputs("you have mail\n", stderr);
    return;
  }
  /* MAILPATH: path[%message]:...; \% is a literal % in the path */
  strbuf path, msg;
  sb_init(&path);
  sb_init(&msg);
  for (const char *p = mailpath;;) {
    path.len = msg.len = 0;
    for (; *p && *p != ':' && *p != '%'; p++) {
      if (*p == '\\' && p[1] == '%') p++;
      sb_putc(&path, *p);
    }
    if (*p == '%') {
      for (p++; *p && *p != ':'; p++) sb_putc(&msg, *p);
    }
    if (path.len && mail_changed(path.s)) {
      strbuf out;
      sb_init(&out);
      put_expanded(&out, msg.len ? msg.s : "you have mail");
      fprintf(stderr, "%s\n", out.s ? out.s : "");
      sb_free(&out);
    }
    if (!*p) break;
    p++;
  }
  sb_free(&path);
  sb_free(&msg);
}

char *prompt_string(int which) {
  strbuf sb;
  sb_init(&sb);
  const char *ps = var_get(which == 1 ? "PS1" : "PS2");
  if (which == 2) {
    sb_puts(&sb, ps ? ps : "> ");
    return sb_detach(&sb);
  }
  if (!ps) {
    /* cpsh's default: blank line, directory, then user@host> */
    sb_putc(&sb, '\n');
    display_cwd(&sb);
    sb_putc(&sb, '\n');
    sb_puts(&sb, username());
    sb_putc(&sb, '@');
    sb_puts(&sb, hostname());
    sb_puts(&sb, geteuid() == 0 ? "# " : "> ");
    return sb_detach(&sb);
  }
  /* POSIX: "!" is the history number ("!!" is a literal "!"), then
   * parameter expansion */
  strbuf raw;
  sb_init(&raw);
  for (const char *p = ps; *p; p++) {
    if (*p == '!' && p[1] == '!') {
      sb_putc(&raw, '!');
      p++;
    } else if (*p == '!') {
      char num[16];
      snprintf(num, sizeof(num), "%d", history_last() + 1);
      sb_puts(&raw, num);
    } else {
      sb_putc(&raw, *p);
    }
  }
  put_expanded(&sb, raw.s ? raw.s : "");
  sb_free(&raw);
  return sb_detach(&sb);
}
