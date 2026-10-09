# cpsh

Lightweight POSIX command-line shell made in C.

cpsh implements most of the POSIX shell command language (XCU chapter 2)
and can run ordinary `/bin/sh` scripts as well as serve as an interactive
shell with line editing, history and tab completion. It is not yet fully
POSIX compliant: see [Not implemented](#not-implemented).

## [How to install](INSTALLATION.md#installation)

## [How to uninstall](INSTALLATION.md#uninstalling)

## Usage

```sh
cpsh                      # interactive shell
cpsh script.sh [args...]  # run a script
cpsh -c 'command' [name [args...]]
cpsh -s [args...] < script
```

Options are the `set` options below, e.g. `cpsh -ex script.sh`.

## Language support

| Area | Supported |
|---|---|
| Quoting | `'single'`, `"double"`, `\` escapes, line continuation |
| Lists | `;` `&` `&&` `\|\|` newlines, `!` pipelines, pipes `\|` |
| Compound commands | `( )` subshells, `{ }` groups, `if/elif/else`, `while`, `until`, `for`, `case` |
| Functions | `name() compound-command`, positional parameters, `return`, `local` |
| Redirection | `<` `>` `>>` `>\|` `<>` `<&` `>&` `n>&-`, here-documents `<<` and `<<-` |
| Parameters | variables, `$1`…`${10}`, `$@ $* $# $? $- $$ $! $0`, `export`, `readonly` |
| Parameter expansion | `${x}` `${x:-w}` `${x-w}` `${x:=w}` `${x:?w}` `${x:+w}` `${#x}` `${x%p}` `${x%%p}` `${x#p}` `${x##p}` |
| Other expansions | tilde, `$(…)` and `` `…` `` command substitution, `$((…))` arithmetic (full C operator set), IFS field splitting, pathname expansion (`* ? [...]`) |
| Aliases | `alias`, `unalias` |
| Traps & signals | `trap` for `EXIT` and signals, `kill`, `wait` |
| Options (`set`) | `-a -C -e -f -n -u -v -x`, `-o pipefail`, `-o ignoreeof`, `set -o` / `set +o` |
| Scripts | script files, `-c`, `-s`, stdin, `.` (dot), `eval`, `$ENV` for interactive shells, scripts without `#!` |

### Built-in utilities

Special built-ins: `.` `:` `break` `continue` `eval` `exec` `exit` `export`
`readonly` `return` `set` `shift` `times` `trap` `unset`.

Regular built-ins: `alias` `cd` `command` `echo` `false` `getopts` `hash`
`history` `kill` `local` `printf` `pwd` `read` `test`/`[` `true` `type`
`umask` `unalias` `wait`.

### Interactive features

- Line editing: arrow keys, Home/End, Delete, `^A ^E ^B ^F ^K ^U ^W ^L`.
- History: up/down (`^P`/`^N`), saved to `~/.cpsh_history` (or `$HISTFILE`,
  size `$HISTSIZE`), the `history` built-in, and `!!`, `!n`, `!-n`, `!prefix`
  expansion (so `sudo !!` works).
- Tab completion of commands (built-ins, keywords, `$PATH`) and file names;
  a second tab lists the candidates.
- `PS1`/`PS2` prompts with parameter and command expansion; when `PS1` is not
  set, cpsh shows the working directory and `user@host>`.

### Not implemented

- Job control (`set -m`, `fg`, `bg`, `jobs`, `%n` job IDs in `kill` and
  `wait`): background jobs run, can be waited for by process ID, and are
  reported through `$!`, but cannot be moved between foreground and
  background. `set -m` is accepted and does nothing.
- The `fc` and `ulimit` built-ins, and `vi` editing mode (`set -o vi` is
  accepted and does nothing).
- The `LINENO` variable, and mail checking (`MAIL`, `MAILCHECK`, `MAILPATH`).
- `trap` with no operands inside a command substitution prints nothing,
  rather than the traps of the parent shell.
- Additions in POSIX.1-2024: `$'...'` quoting, `;&` in `case`, `read -d`,
  `cd -e` and `printf` argument numbers (`%1$s`).

## Performance

Measured against the previous release (`bench/benchmark.py`, median of 11
runs, both built with `-O2`, Linux on WSL2):

| Workload | before | after | faster |
|---|---|---|---|
| 2000 × `/bin/true` | 0.688 s | 0.587 s | 14.6 % |
| 2000 × `uname` (PATH search) | 0.734 s | 0.627 s | 14.6 % |
| 2000 × `/bin/echo` with arguments | 0.741 s | 0.620 s | 16.3 % |
| 2000 × `echo`/`true`/`cd`/`pwd` | 0.581 s | 0.002 s | 99.6 % |
| 10 000 distinct command lines | 0.025 s | 0.011 s | 55.7 % |
| **Total** | **2.769 s** | **1.848 s** | **33.3 %** |

Peak memory for a 10 000-line session dropped from 2348 KiB to 1688 KiB.

External commands are started with `posix_spawn` (vfork-style, so its cost
does not grow with the shell's size), command paths are cached in a hash
table, the exported environment is cached until a variable changes, common
utilities run as built-ins without forking, parse trees and temporary
strings come from arena allocators, and history is a ring buffer with O(1)
appends. For external commands cpsh runs within about 1 % of a bare
`posix_spawn` + `waitpid` loop; the remaining time is the kernel starting
the program.

Run the benchmark yourself with:

```sh
python3 bench/benchmark.py path/to/old/cpsh bin/cpsh
```

## Tests

```sh
cd build && ctest            # or: sh tests/run_tests.sh bin/cpsh
```

The suite can also be pointed at another shell (e.g. `dash`) to check the
expectations themselves.
