#include "prompt.h"

#include <pwd.h>

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
  stackmark m = stmark();
  struct jmploc jl, *saved = handler;
  if (setjmp(jl.buf) == 0) {
    handler = &jl;
    sb_puts(&sb, expand_prompt(raw.s ? raw.s : ""));
  } else {
    sb_puts(&sb, raw.s ? raw.s : "");
  }
  handler = saved;
  strelease(m);
  sb_free(&raw);
  return sb_detach(&sb);
}
