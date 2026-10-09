#!/bin/sh
# Conformance tests for cpsh.
#
#   tests/run_tests.sh path/to/cpsh
#
# Each case runs a script with the shell under test and compares its combined
# output and exit status with the expected values. Run it with any POSIX
# shell; pass another shell (e.g. dash) to check the expectations themselves.

SH=${1:?usage: run_tests.sh path/to/shell}
case $SH in /*) ;; *) SH=$(pwd)/$SH ;; esac
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cpsh-test.XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT
pass=0
fail=0

# t NAME SCRIPT EXPECTED [STATUS]
t() {
  printf '%s\n' "$2" > "$TMP/script.sh"
  rm -rf "$TMP/w"
  mkdir "$TMP/w"
  actual=$(cd "$TMP/w" && HOME=/home/tester CPSH="$SH" "$SH" "$TMP/script.sh" 2>&1 </dev/null)
  status=$?
  # diagnostics name the script by path; compare them by basename
  actual=$(printf '%s\n' "$actual" | sed "s|$TMP/||g")
  if [ "$actual" = "$3" ] && [ "$status" = "${4:-0}" ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    printf 'FAIL: %s\n--- expected (status %s):\n%s\n--- actual (status %s):\n%s\n\n' \
      "$1" "${4:-0}" "$3" "$status" "$actual"
  fi
}

# ---------------------------------------------------------------- basics
t 'simple command' 'echo hello world' 'hello world'
t 'exit status' 'false; echo $?; true; echo $?' '1
0'
t 'command not found' 'nonexistent_cmd_xyz 2>/dev/null; echo $?' '127'
t 'exit builtin' 'exit 3' '' 3
t 'exit default status' 'false; exit' '' 1
t 'comments' 'echo a # comment
# whole line
echo b#c' 'a
b#c'
t 'line continuation' 'echo a\
b' 'ab'
t 'semicolon list' 'echo a; echo b;echo c' 'a
b
c'

# ---------------------------------------------------------------- quoting
t 'single quotes' "echo 'a  \$x \\ \"b\"'" 'a  $x \ "b"'
t 'double quotes' 'x=1; echo "a  $x \$x \\ \"q\" `echo bq`"' 'a  1 $x \ "q" bq'
t 'backslash' 'echo \$x a\ \ b \\' '$x a  b \'
t 'empty quoted arg' 'set -- "" a ""; echo $#' '3'
t 'adjacent quoting' "echo 'a'\"b\"c\\d" 'abcd'
t 'quoted newline' "echo 'a
b'" 'a
b'
t 'backslash in dquote' 'printf "%s\n" "a\b\$\\"' 'a\b$\'

# ---------------------------------------------------------------- variables
t 'assignment' 'x=hello; echo $x ${x}' 'hello hello'
t 'multiple assignments' 'a=1 b=2; echo $a$b' '12'
t 'unset var empty' 'echo "[$nope]"' '[]'
t 'special params' 'set -- a b c; echo $# $1 $3; echo "$*"' '3 a c
a b c'
t 'positional > 9' 'set -- 1 2 3 4 5 6 7 8 9 ten; echo ${10} $10' 'ten 10'
t 'dollar-at quoting' 'set -- "a b" c; for i in "$@"; do echo "[$i]"; done' '[a b]
[c]'
t 'dollar-star IFS join' 'set -- a b c; IFS=:; echo "$*"' 'a:b:c'
t 'empty dollar-at' 'set --; for i in "$@"; do echo x; done; echo done' 'done'
t 'shell pid' 'test "$$" -gt 0 && echo ok' 'ok'
t 'last bg pid' 'sleep 0 & test -n "$!" && echo ok; wait' 'ok'
t 'prefix assignment temporary' 'x=1; x=2 true; echo $x' '1'
t 'prefix assignment exported' 'x=1 sh -c "echo \$x"' '1'
t 'export' 'export Y=7; sh -c "echo \$Y"' '7'
t 'readonly' 'readonly R=1; (R=2) 2>/dev/null; echo $R' '1'
t 'readonly assignment is fatal' 'readonly R=1; R=2; echo not reached' 'script.sh: R: is read only' 2
t 'unset' 'x=1; unset x; echo "[${x-unset}]"' '[unset]'
t 'set -u' 'set -u; echo $undefined_var' 'script.sh: undefined_var: parameter not set' 2

# ---------------------------------------------------------------- parameter expansion
t 'default value' 'echo ${u-def} ${u:-def2}; e=; echo "[${e-x}]" ${e:-y}' 'def def2
[] y'
t 'assign default' 'echo ${v=set}; echo $v' 'set
set'
t 'alternate value' 'x=1; echo ${x+alt} "[${u+alt}]"' 'alt []'
t 'error if unset' '(: ${u?custom msg}) 2>&1; echo after' 'script.sh: u: custom msg
after'
t 'string length' 'x=hello; echo ${#x}' '5'
t 'remove suffix' 'f=file.tar.gz; echo ${f%.*} ${f%%.*}' 'file.tar file'
t 'remove prefix' 'p=/a/b/c; echo ${p#*/} ${p##*/}' 'a/b/c c'
t 'quoted pattern' 'x="a*b"; echo ${x%"*b"}' 'a'
t 'nested expansion' 'x=; y=inner; echo ${x:-${y}-out}' 'inner-out'

# ---------------------------------------------------------------- command substitution
t 'dollar paren' 'echo $(echo a b)' 'a b'
t 'backquotes' 'echo `echo x`' 'x'
t 'nested cmdsub' 'echo $(echo $(echo deep))' 'deep'
t 'trailing newlines stripped' 'x=$(printf "a\n\n\n"); echo "[$x]"' '[a]'
t 'cmdsub status' 'x=$(exit 4); echo $?' '4'
t 'cmdsub with case' 'echo $(case a in a) echo yes;; esac)' 'yes'
t 'cmdsub quoted' 'echo "$(echo "a  b")"' 'a  b'

# ---------------------------------------------------------------- arithmetic
t 'arithmetic' 'echo $((1+2*3)) $(( (1+2)*3 )) $((7/2)) $((7%3)) $((-5+2))' '7 9 3 1 -3'
t 'arith variables' 'x=4; echo $((x*2)) $(($x+1))' '8 5'
t 'arith assignment' 'x=1; : $((x+=5)); echo $x; echo $((y=3)) $y' '6
3 3'
t 'arith compare logic' 'echo $((3>2)) $((1==2)) $((1&&0)) $((0||2)) $((!0))' '1 0 0 1 1'
t 'arith ternary bits' 'echo $((1?5:6)) $((0?5:6)) $((6&3)) $((6|1)) $((6^3)) $((1<<4)) $((~0))' '5 6 2 7 5 16 -1'
t 'arith hex octal' 'echo $((0x10)) $((010))' '16 8'
t 'arith div by zero' 'echo $((1/0))' 'script.sh: arithmetic expression: division by zero' 2

# ---------------------------------------------------------------- field splitting
t 'IFS split' 'x="a b   c"; set -- $x; echo $#' '3'
t 'custom IFS' 'IFS=:; x="a::b"; set -- $x; echo $#' '3'
t 'IFS no split quoted' 'x="a b"; set -- "$x"; echo $#' '1'
t 'empty IFS' 'IFS=; x="a b"; set -- $x; echo $#' '1'
t 'unquoted empty vanishes' 'e=; set -- $e; echo $#' '0'

# ---------------------------------------------------------------- pathname expansion
t 'glob star' 'touch a.c b.c d.h; echo *.c' 'a.c b.c'
t 'glob question' 'touch ab ac; echo a?' 'ab ac'
t 'glob bracket' 'touch x1 x2 x3; echo x[12] x[!1]' 'x1 x2 x2 x3'
t 'glob no match' 'echo *.none' '*.none'
t 'glob quoted' 'touch a.c; echo "*.c" \*.c' '*.c *.c'
t 'set -f' 'touch a.c; set -f; echo *.c' '*.c'
t 'glob from variable' 'touch a.c; p="*.c"; echo $p "$p"' 'a.c *.c'

# ---------------------------------------------------------------- tilde
t 'tilde' 'echo ~ ~/x' '/home/tester /home/tester/x'
t 'tilde quoted' 'echo "~" \~' '~ ~'
t 'tilde in assignment' 'p=~/bin:~/lib; echo $p' '/home/tester/bin:/home/tester/lib'

# ---------------------------------------------------------------- pipelines & lists
t 'pipeline' 'echo hello | tr h j' 'jello'
t 'long pipeline' 'printf "c\nb\na\n" | sort | head -n 2 | tr -d "\n"; echo' 'ab'
t 'pipeline status' 'true | false; echo $?; false | true; echo $?' '1
0'
t 'pipefail' 'set -o pipefail; false | true; echo $?' '1'
t 'negation' '! false; echo $?; ! true; echo $?' '0
1'
t 'and or' 'true && echo a; false && echo b; false || echo c; true || echo d' 'a
c'
t 'and or chain' 'false && echo x || echo y' 'y'
t 'background' 'echo bg > out & wait; cat out' 'bg'
t 'wait status' 'sh -c "exit 5" & wait $!; echo $?' '5'
t 'builtin in pipeline' 'echo abc | read x; echo "[$x]"' '[]'

# ---------------------------------------------------------------- redirections
t 'output redirect' 'echo hi > f; cat f' 'hi'
t 'append' 'echo a > f; echo b >> f; cat f' 'a
b'
t 'input redirect' 'echo data > f; tr a o < f' 'doto'
t 'stderr redirect' 'ls /nonexistent_dir 2> err; test -s err && echo ok' 'ok'
t 'dup fd' 'sh -c "echo e >&2" 2>&1 | tr e E' 'E'
t 'close fd' 'echo x >&- 2>/dev/null; echo $?' '1'
t 'redirect order' '{ echo out; echo err >&2; } > f 2>&1; sort f' 'err
out'
t 'io number' 'echo x 3>f 1>&3; cat f' 'x'
t 'noclobber' 'echo a > f; set -C; { echo b > f; } 2>/dev/null; echo $?; echo c >| f; cat f' '1
c'
t 'read write' 'echo abc > f; cat <> f' 'abc'
t 'redirect on compound' 'for i in 1 2; do echo $i; done > f; cat f' '1
2'
t 'exec redirect' 'exec 3> f; echo via3 >&3; exec 3>&-; cat f' 'via3'
t 'redirect failure' '{ cat < /nonexistent; } 2>/dev/null; echo $?' '1'

# ---------------------------------------------------------------- here-documents
t 'heredoc' 'cat <<EOF
a $((1+1))
  b
EOF' 'a 2
  b'
t 'heredoc quoted' 'x=1; cat <<"EOF"
$x `echo n`
EOF' '$x `echo n`'
t 'heredoc strip tabs' "cat <<-EOF
	tabbed
	EOF" 'tabbed'
t 'two heredocs' 'cat <<A; cat <<B
one
A
two
B' 'one
two'
t 'heredoc in function' 'f() { cat <<E
in $1
E
}; f x' 'in x'
t 'heredoc escapes' 'cat <<E
\$x \\ \a
E' '$x \ \a'

# ---------------------------------------------------------------- compound commands
t 'if elif else' 'for n in 1 2 3; do if [ $n = 1 ]; then echo one; elif [ $n = 2 ]; then echo two; else echo other; fi; done' 'one
two
other'
t 'while' 'i=0; while [ $i -lt 3 ]; do echo $i; i=$((i+1)); done' '0
1
2'
t 'until' 'i=0; until [ $i -ge 2 ]; do i=$((i+1)); done; echo $i' '2'
t 'for without in' 'set -- x y; for a; do echo $a; done' 'x
y'
t 'for newline' 'for a in 1 2
do
  echo $a
done' '1
2'
t 'case patterns' 'for w in apple Banana cherry 42; do case $w in a*|b*) echo ab;; [A-Z]*) echo upper;; *[0-9]) echo num;; *) echo other;; esac; done' 'ab
upper
other
num'
t 'case quoted pattern' 'case "*" in "*") echo lit;; esac; case x in "*") echo no;; *) echo star;; esac' 'lit
star'
t 'case empty body' 'case a in a) ;; esac; echo $?' '0'
t 'case paren pattern' 'case b in (a) echo a;; (b) echo b;; esac' 'b'
t 'subshell' 'x=1; (x=2; echo $x); echo $x' '2
1'
t 'subshell cd' '(cd /); pwd | grep -c "^/$"' '0' 1
t 'brace group' '{ echo a; echo b; } | wc -l | tr -d " "' '2'
t 'break continue' 'for i in 1 2 3 4; do [ $i = 2 ] && continue; [ $i = 4 ] && break; echo $i; done' '1
3'
t 'break n' 'for i in 1 2; do for j in a b; do echo $i$j; break 2; done; done' '1a'
t 'loop status' 'for i in; do :; done; echo $?' '0'
t 'nested loops' 'for i in 1 2; do for j in a b; do printf "%s%s " $i $j; done; done; echo' '1a 1b 2a 2b '

# ---------------------------------------------------------------- functions
t 'function' 'greet() { echo "hi $1"; }; greet bob' 'hi bob'
t 'function return' 'f() { return 7; }; f; echo $?' '7'
t 'function positional' 'f() { echo $# $1; }; set -- a b c; f x; echo $# $1' '1 x
3 a'
t 'function recursion' 'fact() { if [ $1 -le 1 ]; then echo 1; else echo $(( $1 * $(fact $(($1-1))) )); fi; }; fact 6' '720'
t 'function local' 'x=g; f() { local x=l; echo $x; }; f; echo $x' 'l
g'
t 'function redefine' 'f() { echo 1; }; f() { echo 2; }; f' '2'
t 'unset function' 'f() { echo 1; }; unset -f f; f 2>/dev/null; echo $?' '127'
t 'function with redirect' 'f() { echo inside; } > f.out; f; cat f.out' 'inside'
t 'function subshell body' 'f() ( x=2 ); x=1; f; echo $x' '1'

# ---------------------------------------------------------------- builtins
t 'cd and pwd' 'mkdir -p d/e; cd d/e; basename "$(pwd)"; cd ..; basename "$PWD"' 'e
d'
t 'cd -' 'mkdir d; cd d; cd - >/dev/null; basename "$PWD"' 'w'
t 'cd logical' 'mkdir real; ln -s real link; cd link; basename "$PWD"; cd ..; basename "$PWD"' 'link
w'
t 'echo escapes' 'echo "a\tb"; echo -n x; echo y' 'a	b
xy'
t 'printf' 'printf "%s-%d-%03d-%x|%5s|%-3s|\n" a 42 7 255 r l' 'a-42-007-ff|    r|l  |'
t 'printf reuse' 'printf "%s\n" a b c' 'a
b
c'
t 'printf b' 'printf "%b\n" "a\tb"' 'a	b'
t 'test strings' 'test a = a && echo eq; [ a != b ] && echo ne; [ -z "" ] && echo z; [ -n x ] && echo n' 'eq
ne
z
n'
t 'test numbers' '[ 3 -gt 2 ] && [ 2 -le 2 ] && [ 1 -ne 2 ] && echo ok' 'ok'
t 'test files' 'touch f; mkdir d; [ -f f ] && [ -d d ] && [ ! -e nope ] && echo ok' 'ok'
t 'test logic' '[ 1 -eq 1 -a \( 2 -eq 3 -o 4 -eq 4 \) ] && echo ok' 'ok'
t 'test error' '[ 1 -eq x ] 2>/dev/null; echo $?' '2'
t 'eval' 'x="echo evaluated"; eval $x; eval "y=5"; echo $y' 'evaluated
5'
t 'dot' 'echo "sourced=yes" > inc.sh; . ./inc.sh; echo $sourced' 'yes'
t 'dot return' 'printf "echo a\nreturn 3\necho b\n" > inc.sh; . ./inc.sh; echo $?' 'a
3'
t 'shift' 'set -- a b c; shift; echo $*; shift 2; echo $#' 'b c
0'
t 'set positional' 'set -- x "y z"; echo $# $2' '2 y z'
t 'read' 'echo "a b c" > f; read x y < f; echo "[$x][$y]"' '[a][b c]'
t 'read raw' 'printf "a\\\\b\n" > f; read -r x < f; printf "%s\n" "$x"' 'a\b'
t 'read backslash' 'printf "a\\\\b\n" > f; read x < f; printf "%s\n" "$x"' 'ab'
t 'read loop' 'printf "1\n2\n3\n" | while read n; do echo n$n; done' 'n1
n2
n3'
t 'read eof status' 'read x < /dev/null; echo $?' '1'
t 'getopts' 'set -- -a -b val arg; while getopts ab: o; do echo "$o${OPTARG:+=$OPTARG}"; done; shift $((OPTIND-1)); echo $1' 'a
b=val
arg'
t 'getopts bad option' 'set -- -z; getopts a o 2>/dev/null; echo $o' '?'
t 'trap exit' 'trap "echo bye" EXIT; echo hi' 'hi
bye'
t 'trap signal' 'trap "echo caught" USR1; kill -USR1 $$; echo after' 'caught
after'
t 'trap reset' 'trap "echo x" EXIT; trap - EXIT; echo done' 'done'
t 'trap list' 'trap "echo hi" INT; trap' "trap -- 'echo hi' INT"
t 'command -v' 'command -v echo; command -v cd' 'echo
cd'
t 'command bypasses function' 'echo() { printf "func\n"; }; command echo builtin' 'builtin'
t 'type' 'type cd' 'cd is a shell builtin'
t 'alias' 'alias say="echo said"
say hi' 'said hi'
t 'unalias' 'alias a=echo
unalias a
a 2>/dev/null; echo $?' '127'
t 'umask' 'umask 027; umask; umask -S' '0027
u=rwx,g=rx,o='
t 'times format' 'times | wc -l | tr -d " "' '2'
t 'colon true false' ': && true && ! false && echo ok' 'ok'
t 'exec replaces' 'exec echo replaced; echo not reached' 'replaced'
t 'hash' 'hash ls; hash | grep -c /ls' '1'
t 'kill -l' 'kill -l 15' 'TERM'

# ---------------------------------------------------------------- options
t 'set -e' 'set -e; echo a; false; echo b' 'a' 1
t 'set -e tested' 'set -e; if false; then :; fi; false || true; ! true; echo ok' 'ok'
t 'set -e and-or' 'set -e; false && true; echo survived' 'survived'
t 'set -x' 'set -x; echo traced' '+ echo traced
traced'
t 'set -n' 'set -n; echo not run' ''
t 'set -a' 'set -a; Z=1; sh -c "echo \$Z"' '1'
t 'dollar dash' 'set -e; case $- in *e*) echo has_e;; esac' 'has_e'
t 'set -o list' 'set -o | grep -c errexit' '1'

# ---------------------------------------------------------------- scripts & misc
t 'script args' 'printf "echo \$0 \$1 \$#\n" > s.sh; "$CPSH" s.sh a b' 's.sh a 2'
t 'dash c' '"$CPSH" -c "echo \$0 \$1" name arg' 'name arg'
t 'script without shebang' 'printf "echo from script\n" > s; chmod +x s; ./s' 'from script'
t 'syntax error' 'echo before; if then' 'script.sh: syntax error at line 1: unexpected '"'then'"'' 2
t 'unterminated quote' 'echo "abc' 'script.sh: syntax error at line 2: unterminated quoted string' 2
t 'keywords as args' 'echo if then done' 'if then done'
t 'deep recursion of lists' 'i=0; while [ $i -lt 200 ]; do i=$((i+1)); done; echo $i' '200'
t 'long line' "x=\$(printf '%0500d' 0); echo \${#x}" '500'
t 'many args' 'set -- $(seq 1 1000); echo $#' '1000'
t 'NUL bytes in script terminate' 'printf "echo a\0b\necho ok\n" > nul.sh; "$CPSH" nul.sh 2>&1 | tail -n 1' 'ok'
t 'stdin script' 'printf "echo piped; read x; echo got \$x\nline2\n" | "$CPSH"' 'piped
got line2'
t 'stdin shared with children' 'printf "head -n 1\nfirst\necho after\n" > in; "$CPSH" < in' 'first
after'

# ---------------------------------------------------------------- regressions
t 'case as an argument in $()' 'x=$(echo case esac); echo "$x"; echo "$(echo a case b)"' 'case esac
a case b'
t 'case statement in $()' 'echo $(case a in a) echo m;; esac) $(case b in (b) echo n;; esac)' 'm n'
t 'here-document in $()' 'x=$(cat <<EOF
a)b
EOF
); echo "$x"' 'a)b'
t 'comment in $()' 'echo $( # not a )
echo hi)' 'hi'
t 'nested $() with parens' 'echo $(echo $(echo "(") ")" )' '( )'
t 'alias inside $() expands when run' 'alias say=echo
x=$(say hi); echo $x' 'hi'
t 'quotes in ${} inside double quotes' 'x=; echo "${x:-"a b"}" "${x:-'"'q'"'}"; set -- ${x:-"c d"}; echo $#' "a b 'q'
1"
t 'shift error is fatal' 'set -- a; shift 2; echo notreached' "script.sh: shift: can't shift that many" 2
t 'unset readonly is fatal' 'readonly r=1; unset r; echo notreached' 'script.sh: r: is read only' 2
t 'readonly prefix assignment is fatal' 'readonly r=1; r=2 true; echo notreached' 'script.sh: r: is read only' 2
t 'command makes special built-in errors non-fatal' 'readonly r=1
command shift 5 2>/dev/null; echo $?
command unset r 2>/dev/null; echo $?
command export 1bad 2>/dev/null; echo $?
command set -Z 2>/dev/null; echo $?' '2
2
2
2'
t 'bad trap is not fatal' 'trap : NOSUCHSIG 2>/dev/null; echo $?' '1'
t 'wait interrupted by trap' 'trap "echo got" USR1; (sleep 1; kill -USR1 $$) & sleep 5 & wait $!; echo $?; kill $! 2>/dev/null' 'got
138'
t 'arithmetic wraps' 'echo $((9223372036854775807 + 1)) $((-(-9223372036854775807 - 1)))' '-9223372036854775808 -9223372036854775808'
t 'multi-line history entries' 'printf "echo one\n#cpsh:2\nfor i in 1; do echo \$i\ndone\n" > h
HISTFILE=$PWD/h "$CPSH" -i -c "history" 2>/dev/null' '    1  echo one
    2  for i in 1; do echo $i
done'
t 'multi-line history survives a save' 'export HISTFILE=$PWD/h2
"$CPSH" -i -c "history -a \"\$(printf \"a\nb\")\"; history -a c" 2>/dev/null
"$CPSH" -i -c "history" 2>/dev/null' '    1  a
b
    2  c'
t 'printf too wide with no arguments' 'printf "%0600d" | wc -c' '600'

# ---------------------------------------------------------------- LINENO
t 'LINENO' 'echo $LINENO
f() {
  echo $LINENO
}
f
echo $(echo $LINENO) \
  $LINENO' '1
3
6 6'
t 'LINENO in eval' 'x=1
eval "echo \$LINENO
echo \$LINENO"' '2
3'
t 'unset LINENO' 'unset LINENO; echo "[$LINENO]"' '[]'

# ---------------------------------------------------------------- ulimit
t 'ulimit -f set and read' 'ulimit -f 100; ulimit -f; ulimit -Sf 50; ulimit -Sf; ulimit -Hf' '100
50
100'
t 'ulimit default is -f' 'ulimit -f 77; ulimit' '77'
t 'ulimit -n in a child' 'ulimit -n 64; "$CPSH" -c "ulimit -n"' '64'
t 'ulimit -a lists -n' 'ulimit -a | grep -c "^-n: "' '1'
t 'ulimit errors' 'ulimit -n abc 2>/dev/null; echo $?; ulimit -x 2>/dev/null; echo $?' '1
2'

# ---------------------------------------------------------------- traps in subshells
t 'trap lists the parent traps in $()' 'trap "echo hi" USR1; trap "" USR2; x=$(trap); echo "$x"' "trap -- 'echo hi' USR1
trap -- '' USR2"
t 'trap in a subshell after a change' 'trap "echo hi" USR1; (trap "echo z" TERM; trap)' "trap -- 'echo z' TERM"
t 'last subshell does not keep traps' 'trap "echo hi" USR1; (trap)' "trap -- 'echo hi' USR1"

# ---------------------------------------------------------------- POSIX.1-2024
t "\$'...' escapes" "printf '%s|' \$'a\\tb' \$'q\\'q' \$'\\x41\\x4a' \$'\\101\\60' \$'x\\0y' \$'\\z' \"\$'no'\" \$''" "a	b|q'q|AJ|A0|x|\\z|\$'no'||"
t "\$'...' is quoted" "\$'{' 2>/dev/null; echo \$?; x=\$'a  b'; echo \"[\$x]\"" '127
[a  b]'
t "\$'\\c' control characters" "printf %s \$'\\cA\\c?' | od -An -tx1 | tr -d ' '" '017f'
t 'case ;& falls through' 'for v in a b c; do
case $v in
  a) echo A ;&
  b) echo B ;;
  c) echo C ;&
esac
done' 'A
B
B
C'
t 'read -d' 'printf "a:b" | { read -d : v; echo "$v"; }
printf "a b\0c" | { read -d "" v; echo "$v"; }
printf "xyz" | { read -rd y v; echo "$v"; }' 'a
a b
x'
t 'printf numbered arguments' 'printf "%2\$s %1\$s\n" a b c d; printf "%2\$*1\$d|\n" 4 7' 'b a
d c
   7|'
t 'cd -e' 'cd -eP /; echo $? $PWD' '0 /'

# ---------------------------------------------------------------- mail
t 'MAIL' ': > box
printf "echo x >> box\necho y\n" | MAIL=$PWD/box MAILCHECK=0 PS1= "$CPSH" -i 2>&1' 'you have mail
y'
t 'MAILPATH message' ': > box
printf "echo x >> box\necho y\n" | MAILPATH="$PWD/box%new in \$HOME" MAILCHECK=0 PS1= "$CPSH" -i 2>&1' 'new in /home/tester
y'
t 'MAILCHECK interval' ': > box
printf "echo x >> box\necho y\n" | MAIL=$PWD/box MAILCHECK=600 PS1= "$CPSH" -i 2>&1' 'y'

# ---------------------------------------------------------------- jobs
# (job control itself needs a terminal: tests/jobctl_test.py)
t 'jobs lists background jobs' 'sleep 5 & sleep 6 & jobs; kill %1 %2' '[1]-  Running                 sleep 5 &
[2]+  Running                 sleep 6 &'
t 'jobs command text' '{ sleep 5; echo "a  b"; } >/dev/null 2>&1 & jobs; kill %1' \
  '[1]+  Running                 { sleep 5; echo "a  b"; } >/dev/null 2>&1 &'
t 'jobs reports finished jobs once' '(exit 3) & true & sleep 1; jobs; jobs; echo end' '[1]-  Done(3)                 (exit 3)
[2]+  Done                    true
end'
t 'jobs reports signals' 'sleep 5 & kill %1; sleep 1; jobs' '[1]+  Terminated              sleep 5'
t 'jobs -p' 'sleep 5 & test "$(jobs -p)" = "$!" && echo ok; kill %%' 'ok'
t 'jobs -l' 'sleep 5 & jobs -l | grep -q "^\[1\]+ $! Running" && echo ok; kill %%' 'ok'
t 'jobs -l pipeline' 'sleep 5 | sleep 6 & jobs -l | sed "s/ [0-9][0-9]* / N /"; kill %%' \
  '[1]+ N Running                 sleep 5
     N                         | sleep 6 &'
t 'background pipeline' 'set -o pipefail; false | true & wait $!; echo $?
sleep 5 | sleep 6 & test "$(jobs -p)" != "$!" && echo ok; kill %%' '1
ok'
t 'job ids' 'sleep 5 & sleep 6 & sleep 7 &
kill %sleep
kill %?6; wait %2; echo $?; jobs %+ %-; kill %+ %-' 'script.sh: kill: %sleep: ambiguous job
143
[3]+  Running                 sleep 7 &
[1]-  Running                 sleep 5 &'
t 'wait for a job' 'sh -c "exit 4" & wait %1; echo $?; wait %1; echo $?' '4
script.sh: wait: %1: no such job
127'
t 'wait without operands' '(exit 3) & wait; echo $?' '0'
t 'fg and bg need job control' 'fg; echo $?; bg %1; echo $?' 'script.sh: fg: no job control
1
script.sh: bg: no job control
1'

echo "passed: $pass, failed: $fail"
[ "$fail" -eq 0 ]
