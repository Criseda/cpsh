#include "parser.h"

/* Turn a syntax tree back into shell text, for job listings. Words are kept
 * as they were written, so only the layout is normalised. Here-document
 * bodies are left out. */

static void text(strbuf *sb, struct node *n);

/* Separate two commands of a list: an `&` already ends the previous one. */
static void sep(strbuf *sb) {
  if (sb->len && sb->s[sb->len - 1] == '&')
    sb_putc(sb, ' ');
  else
    sb_puts(sb, "; ");
}

/* Space before the next word, unless one is already there. */
static void space(strbuf *sb) {
  if (sb->len && sb->s[sb->len - 1] != ' ' && sb->s[sb->len - 1] != '(')
    sb_putc(sb, ' ');
}

static void redirs(strbuf *sb, struct redir *r) {
  static const char *const ops[] = {"<", ">", ">|", ">>", "<>", "<&", ">&", "<<"};
  static const int defaultfd[] = {0, 1, 1, 1, 0, 0, 1, 0};
  for (; r; r = r->next) {
    space(sb);
    if (r->fd != defaultfd[r->type]) {
      char buf[16];
      snprintf(buf, sizeof(buf), "%d", r->fd);
      sb_puts(sb, buf);
    }
    sb_puts(sb, ops[r->type]);
    if (r->type == R_HEREDOC && r->hd_strip) sb_putc(sb, '-');
    sb_puts(sb, r->word);
  }
}

static void words(strbuf *sb, char **v, int n) {
  for (int i = 0; i < n; i++) {
    space(sb);
    sb_puts(sb, v[i]);
  }
}

static void text(strbuf *sb, struct node *n) {
  if (!n) return;
  switch (n->type) {
    case N_SIMPLE:
      words(sb, n->u.simple.assigns, n->u.simple.nassigns);
      words(sb, n->u.simple.argv, n->u.simple.argc);
      break;
    case N_PIPE:
    case N_LIST:
      for (int i = 0; i < n->u.list.n; i++) {
        if (i) {
          if (n->type == N_PIPE)
            sb_puts(sb, " | ");
          else
            sep(sb);
        }
        text(sb, n->u.list.items[i]);
      }
      break;
    case N_AND:
    case N_OR:
      text(sb, n->u.bin.left);
      sb_puts(sb, n->type == N_AND ? " && " : " || ");
      text(sb, n->u.bin.right);
      break;
    case N_BG:
      text(sb, n->u.body);
      sb_puts(sb, " &");
      break;
    case N_NOT:
      sb_puts(sb, "! ");
      text(sb, n->u.body);
      break;
    case N_SUBSHELL:
      sb_puts(sb, "(");
      text(sb, n->u.body);
      sb_puts(sb, ")");
      break;
    case N_GROUP:
      sb_puts(sb, "{ ");
      text(sb, n->u.body);
      sep(sb);
      sb_puts(sb, "}");
      break;
    case N_IF:
      sb_puts(sb, "if ");
      for (;;) {
        text(sb, n->u.ifn.cond);
        sep(sb);
        sb_puts(sb, "then ");
        text(sb, n->u.ifn.then);
        sep(sb);
        struct node *e = n->u.ifn.els;
        if (e && e->type == N_IF && !e->redirs) {
          sb_puts(sb, "elif ");
          n = e;
          continue;
        }
        if (e) {
          sb_puts(sb, "else ");
          text(sb, e);
          sep(sb);
        }
        break;
      }
      sb_puts(sb, "fi");
      break;
    case N_WHILE:
    case N_UNTIL:
      sb_puts(sb, n->type == N_WHILE ? "while " : "until ");
      text(sb, n->u.loop.cond);
      sep(sb);
      sb_puts(sb, "do ");
      text(sb, n->u.loop.body);
      sep(sb);
      sb_puts(sb, "done");
      break;
    case N_FOR:
      sb_puts(sb, "for ");
      sb_puts(sb, n->u.forn.var);
      if (n->u.forn.words) {
        sb_puts(sb, " in");
        words(sb, n->u.forn.words, n->u.forn.nwords);
      }
      sb_puts(sb, "; do ");
      text(sb, n->u.forn.body);
      sep(sb);
      sb_puts(sb, "done");
      break;
    case N_CASE:
      sb_puts(sb, "case ");
      sb_puts(sb, n->u.casen.word);
      sb_puts(sb, " in");
      for (struct caseitem *ci = n->u.casen.items; ci; ci = ci->next) {
        sb_putc(sb, ' ');
        for (int i = 0; i < ci->npats; i++) {
          if (i) sb_putc(sb, '|');
          sb_puts(sb, ci->pats[i]);
        }
        sb_puts(sb, ") ");
        text(sb, ci->body);
        sb_puts(sb, ci->fallthrough ? " ;&" : " ;;");
      }
      sb_puts(sb, " esac");
      break;
    case N_FUNCDEF:
      sb_puts(sb, n->u.func.name);
      sb_puts(sb, "() ");
      text(sb, n->u.func.body);
      break;
  }
  redirs(sb, n->redirs);
}

char *node_text(struct node *n) {
  strbuf sb;
  sb_init(&sb);
  text(&sb, n);
  return sb_detach(&sb);
}
