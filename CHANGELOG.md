# Changelog

All notable changes to cpsh are documented in this file. The format is based
on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project
uses [Semantic Versioning](https://semver.org/).

## [1.0.0] - 2026-10-09

A rewrite of the shell around a proper lexer, parser and evaluator, so that
cpsh implements most of the POSIX shell command language. Job control, `fc`,
`vi` editing mode and mail checking are not implemented yet.

### Added

- Quoting (single, double, backslash, `$'...'`) and line continuation.
- Pipelines, `;`, `&`, `&&`, `||`, `!`, subshells `( )` and groups `{ }`.
- `if`, `while`, `until`, `for` and `case` (including `;&` fall-through);
  functions with positional parameters, `return` and `local`.
- All POSIX redirections, including `n>&m`, `n>&-`, `<>`, `>|` and
  here-documents (`<<`, `<<-`).
- Variables, positional and special parameters, `$LINENO`, `export`,
  `readonly`, `unset`, and prefix assignments (`VAR=x cmd`).
- Parameter expansion with all POSIX operators, command substitution
  (`$(...)` and backquotes), arithmetic expansion, tilde expansion, IFS field
  splitting and pathname expansion.
- Aliases.
- Script execution (`cpsh script`, `-c`, `-s`, stdin), `.`, `eval`, `exec`,
  `$ENV`, and scripts without a `#!` line.
- `set` options `-a -C -e -f -n -u -v -x`, `-o pipefail`, `-o ignoreeof`.
- `trap` (signals and `EXIT`; in a subshell, `trap` lists the parent's traps
  until one is changed), `wait`, `kill`, background jobs and `$!`.
- Built-ins: `:` `break` `continue` `command` `echo` `false` `getopts`
  `hash` `printf` `pwd` `read` `shift` `test`/`[` `times` `true` `type`
  `ulimit` `umask` `alias` `unalias` `export` `readonly` `unset` `set`
  `local`.
- From POSIX.1-2024: `read -d`, `cd -e`, `printf` argument numbers
  (`%1$s`, `*1$`), and `ulimit -H -S -a -c -d -n -s -t -v`.
- Interactive line editor with cursor movement, history navigation and tab
  completion of commands and file names (#3, #11).
- `!-n` and `!prefix` history expansion anywhere in a line, `history n`,
  `$HISTFILE` and `$HISTSIZE`.
- `PS1`/`PS2` prompts.
- Conformance test suite (`tests/run_tests.sh`, run by `ctest`) and a
  benchmark script (`bench/benchmark.py`) (#7).
- This changelog (#12).

### Changed

- External commands are started with `posix_spawn` instead of `fork`, their
  paths are cached, and the exported environment is cached; overall the
  benchmark suite runs about a third faster and uses 28 % less memory.
- History is kept in a ring buffer (O(1) appends) instead of a linked list
  walked on every command, and entries are no longer fixed 1 KiB blocks.
  Commands typed over several lines are kept as one entry; in the history
  file they are preceded by a `#cpsh:N` line giving their number of lines.
  Files written by older versions still load as before.
- The prompt is only computed for interactive shells.
- `exit` takes a status; exit statuses follow POSIX (126/127, 128+signal).
- CMake builds an optimised binary by default, with warnings enabled, an
  optional sanitizer build (`-DCPSH_SANITIZE=ON`), `make install` and tests.

### Fixed

- End of input (Ctrl-D) made the shell loop forever.
- `!!`, `!n`, `sudo !!` and `sudo !n` crashed when the history was empty or
  the entry did not exist, and `!n` leaked memory.
- `exit` did not save the history.
- `history -r` always aborted the shell with "History is already loaded".
- `history -s` skipped the first entry and crashed on an empty history.
- The shell exited at startup when `$HOME` was not set.
- The prompt abbreviated `/home/al` inside `/home/alice` as `~ice`.
- Ctrl-C at the prompt ran the partially typed line.
- Every `exec` failure was reported as "command not found".
- A failed `exec` in the child could flush the parent's buffered output a
  second time.
- Stopped children were left behind by `waitpid(..., WUNTRACED)`.
- History entries were truncated to 1023 characters.
