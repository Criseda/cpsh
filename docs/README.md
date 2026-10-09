# cpsh

Lightweight POSIX command-line shell made in C.

cpsh implements most of the POSIX shell command language (XCU chapter 2)
and can run ordinary `/bin/sh` scripts as well as serve as an interactive
shell with line editing (emacs-style keys, or `set -o vi`), history and tab
completion.

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
| Quoting | `'single'`, `"double"`, `$'escapes\n'`, `\` escapes, line continuation |
| Lists | `;` `&` `&&` `\|\|` newlines, `!` pipelines, pipes `\|` |
| Compound commands | `( )` subshells, `{ }` groups, `if/elif/else`, `while`, `until`, `for`, `case` (with `;;` and `;&`) |
| Functions | `name() compound-command`, positional parameters, `return`, `local` |
| Redirection | `<` `>` `>>` `>\|` `<>` `<&` `>&` `n>&-`, here-documents `<<` and `<<-` |
| Parameters | variables, `$1`…`${10}`, `$@ $* $# $? $- $$ $! $0`, `$LINENO`, `export`, `readonly` |
| Parameter expansion | `${x}` `${x:-w}` `${x-w}` `${x:=w}` `${x:?w}` `${x:+w}` `${#x}` `${x%p}` `${x%%p}` `${x#p}` `${x##p}` |
| Other expansions | tilde, `$(…)` and `` `…` `` command substitution, `$((…))` arithmetic (full C operator set), IFS field splitting, pathname expansion (`* ? [...]`) |
| Aliases | `alias`, `unalias` |
| Traps & signals | `trap` for `EXIT` and signals, `kill`, `wait` |
| Job control | `set -m`, Ctrl-Z, `jobs`, `fg`, `bg`, job IDs `%n %+ %% %- %string %?string` (also in `kill` and `wait`), `set -b` |
| Options (`set`) | `-a -b -C -e -f -m -n -u -v -x`, `-o pipefail`, `-o ignoreeof`, `set -o` / `set +o` |
| Scripts | script files, `-c`, `-s`, stdin, `.` (dot), `eval`, `$ENV` for interactive shells, scripts without `#!` |

### Built-in utilities

Special built-ins: `.` `:` `break` `continue` `eval` `exec` `exit` `export`
`readonly` `return` `set` `shift` `times` `trap` `unset`.

Regular built-ins: `alias` `bg` `cd` `command` `echo` `false` `fc`
`fg` `getopts` `hash` `history` `jobs` `kill` `local` `printf` `pwd`
`read` `test`/`[` `true` `type` `ulimit` `umask` `unalias` `wait`.

### Interactive features

- Line editing: arrow keys, Home/End, Delete, `^A ^E ^B ^F ^K ^U ^W ^L`,
  and `^V` to insert the next key as it is.
- `set -o vi`: POSIX vi editing. ESC enters command mode, with counts,
  motions (`h l w W b B e E 0 ^ $ | f F t T ; ,`), operators (`c d y` and
  `cc dd yy`), `a A i I R C D S x X r ~ p P Y`, undo (`u U`), repeat (`.`),
  history (`k j - + G`, `/` and `?` searches, `n N`), `_` (last word of the
  previous command), `#`, `\` `*` `=` (completion and pathname expansion),
  `@x` (alias `_x` as keys) and `v` (edit the line with `$VISUAL`, else
  `$EDITOR`, else `vi`).
- History: up/down (`^P`/`^N`), saved to `~/.cpsh_history` (or `$HISTFILE`,
  size `$HISTSIZE`), the `history` built-in, and `!!`, `!n`, `!-n`, `!prefix`
  expansion (so `sudo !!` works). `fc` lists (`-l`), edits (`$FCEDIT`,
  else `ed`) and re-runs (`-s old=new`) earlier commands.
- Tab completion of commands (built-ins, keywords, `$PATH`) and file names;
  a second tab lists the candidates.
- `PS1`/`PS2` prompts with parameter and command expansion; when `PS1` is not
  set, cpsh shows the working directory and `user@host>`.
- Mail notices for `$MAIL` and `$MAILPATH`, checked every `$MAILCHECK`
  seconds (default 600).
- Job control, on by default when the shell runs on a terminal: Ctrl-Z stops
  the foreground job, `fg` and `bg` continue it, and `jobs` lists them
  (`jobs -l` with the process ID of each command of a pipeline).
  Finished jobs are reported before the next prompt, or at once with
  `set -b`. `+m` (or `set +m`) turns job control off.

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
expectations themselves. Job control needs a terminal, so its tests
(`python3 tests/jobctl_test.py bin/cpsh`, also run by `ctest`) drive the
shell on a pseudo-terminal.
