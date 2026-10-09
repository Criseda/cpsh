#!/usr/bin/env python3
"""Job control tests for cpsh.

    tests/jobctl_test.py path/to/cpsh

Job control needs a terminal, so each case runs an interactive shell on a
pseudo-terminal, types into it (including ^Z and ^C) and waits for the
expected output. The `fc` cases are here too, as history is only kept by
interactive shells.
"""

import os
import pty
import re
import select
import signal
import sys
import time

SH = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else sys.exit(
    "usage: jobctl_test.py path/to/shell")
TIMEOUT = 10


class Failure(Exception):
    pass


class Shell:
    def __init__(self, term="dumb", wrapped=False):
        env = dict(os.environ, PS1="$ ", PS2="> ", TERM=term,
                   HISTFILE="/dev/null", ENV="", COLUMNS="200")
        env.pop("MAIL", None)
        env.pop("MAILPATH", None)
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            if wrapped:
                # started from another shell, cpsh is not the session leader
                # and the kernel does not discard ^Z sent to its process
                # group as it does for a session leader's (an orphaned one)
                # (bash, /bin/sh on macOS, drops PS1 and PS2 when it is not
                # interactive, so they are passed on the command line)
                env["CPSH"] = SH
                os.execve("/bin/sh", ["sh", "-c",
                                      'PS1="$ " PS2="> " "$CPSH"; :'], env)
            os.execve(SH, [SH], env)
        self.buf = ""
        self.expect(r"\$ ")

    def send(self, text):
        os.write(self.fd, text.encode())

    def line(self, text):
        self.send(text + "\n")

    def expect(self, pattern, timeout=TIMEOUT):
        """Wait for a regex in the output; consume through its end."""
        rx = re.compile(pattern)
        end = time.time() + timeout
        while True:
            m = rx.search(self.buf)
            if m:
                self.buf = self.buf[m.end():]
                return m
            left = end - time.time()
            if left <= 0:
                raise Failure("timed out waiting for %r; output so far:\n%s"
                              % (pattern, self.buf))
            r, _, _ = select.select([self.fd], [], [], left)
            if r:
                try:
                    data = os.read(self.fd, 4096)
                except OSError:
                    data = b""
                if not data:
                    raise Failure("shell exited waiting for %r; output:\n%s"
                                  % (pattern, self.buf))
                self.buf += data.decode(errors="replace").replace("\r", "")

    def run(self, cmd):
        """Run a command and return its output (before the next prompt)."""
        self.line(cmd)
        self.expect(re.escape(cmd) + "\n")
        return self.expect(r"((?:.|\n)*?)\$ ").group(1)

    def close(self):
        if self.pid:
            try:
                os.kill(self.pid, signal.SIGKILL)
                os.waitpid(self.pid, 0)
            except (ProcessLookupError, ChildProcessError):
                pass
        os.close(self.fd)


shells = []


def new_shell(**kw):
    sh = Shell(**kw)
    shells.append(sh)
    return sh


def reported(sh, cmd):
    """Output of cmd plus what the shell reports before the prompt after
    it: a job it ends is reported at one of those two prompts."""
    out = sh.run(cmd)
    time.sleep(0.3)
    return out + sh.run("true")


def check(actual, expected):
    if actual != expected:
        raise Failure("expected:\n%r\nactual:\n%r" % (expected, actual))


# ---------------------------------------------------------------- cases

def t_option_on():
    sh = new_shell(wrapped=True)
    check(sh.run("case $- in *m*) echo on;; esac"), "on\n")


def t_own_process_group():
    # a foreground job leads its own process group, which owns the
    # terminal; the shell leads another
    sh = new_shell(wrapped=True)
    out = sh.run("sh -c 'ps -o pid=,pgid=,tpgid= -p $$'; "
                 "ps -o pid=,pgid= -p $$")
    (cpid, cpgid, ctpgid), (spid, spgid) = \
        [l.split() for l in out.strip().splitlines()]
    check((cpgid, ctpgid, spgid), (cpid, cpid, spid))


def t_stop_and_resume():
    sh = new_shell(wrapped=True)
    sh.line("sleep 30")
    time.sleep(0.3)
    sh.send("\x1a")
    sh.expect(r"\[1\]\+  Stopped                 sleep 30\n\$ ")
    check(sh.run("echo $?"), "148\n")
    check(sh.run("jobs"), "[1]+  Stopped                 sleep 30\n")
    check(sh.run("bg"), "[1]+ sleep 30 &\n")
    check(sh.run("jobs"), "[1]+  Running                 sleep 30 &\n")
    check(reported(sh, "kill %1"), "[1]+  Terminated              sleep 30\n")


def t_fg_gives_terminal():
    sh = new_shell(wrapped=True)
    sh.line("cat")
    time.sleep(0.3)
    sh.send("\x1a")
    sh.expect(r"Stopped                 cat\n\$ ")
    sh.line("fg")
    sh.expect(r"fg\ncat\n")
    sh.line("hello")
    sh.expect(r"hello\nhello\n")  # echoed by the terminal, then by cat
    sh.send("\x04")
    sh.expect(r"\$ ")
    check(sh.run("echo $?; jobs"), "0\n")


def t_pipeline_job():
    sh = new_shell(wrapped=True)
    sh.line("sleep 30 | sleep 31")
    time.sleep(0.3)
    sh.send("\x1a")
    sh.expect(r"\[1\]\+  Stopped                 sleep 30 \| sleep 31\n\$ ")
    # jobs -l: a line per process
    out = sh.run("jobs -l")
    if not re.fullmatch(r"\[1\]\+ (\d+) Stopped                 sleep 30\n"
                        r" +(\d+)                         \| sleep 31\n", out):
        raise Failure("jobs -l:\n%r" % out)
    sh.line("fg %1")
    sh.expect(r"sleep 30 \| sleep 31\n")
    time.sleep(0.3)
    sh.send("\x03")
    sh.expect(r"\$ ")
    check(sh.run("echo $?; jobs"), "130\n")


def t_fc_list():
    sh = new_shell()
    for c in ("echo one", "echo two", "echo three"):
        sh.run(c)
    check(sh.run("fc -l"), "1\techo one\n2\techo two\n3\techo three\n")
    check(sh.run("fc -ln -2"), "\techo three\n\tfc -l\n")
    check(sh.run("fc -lr 1 2"), "2\techo two\n1\techo one\n")
    check(sh.run("fc -l 0 2"), "1\techo one\n2\techo two\n")
    check(sh.run("fc -l echo\\ t -1"), "3\techo three\n4\tfc -l\n"
          "5\tfc -ln -2\n6\tfc -lr 1 2\n7\tfc -l 0 2\n")


def t_fc_rerun():
    sh = new_shell()
    sh.run("echo one")
    check(sh.run("fc -s one=1"), "echo 1\n1\n")
    check(sh.run("fc -s ec"), "echo 1\n1\n")
    # the commands run replace fc in the history
    check(sh.run("fc -l"), "1\techo one\n2\techo 1\n3\techo 1\n")
    check(sh.run("fc -e 'sed -i.bak s/1/2/' 3"), "echo 2\n2\n")
    check(sh.run("fc -e false; echo $?"), "1\n")
    check(sh.run("fc -l -2"), "5\techo 2\n6\tfc -e false; echo $?\n")


ESC = "\x1b"
UP = " | tr a-z A-Z"  # output that cannot be mistaken for the line typed


def vi_line(sh, keys):
    """Type keys at a vi-mode prompt; the output of the line they run."""
    sh.buf = ""
    sh.send(keys)
    out = sh.expect(r"\n([^\n\x1b]*)\n").group(1)
    sh.expect(r"\$ ")
    return out


def t_editing_keys():
    sh = new_shell(term="xterm")
    check(vi_line(sh, "echo ab" + UP + "\x01" + "\x1b[C" * 6 + "x\r"), "AXB")
    # history, Delete
    check(vi_line(sh, "\x1b[A\x01" + "\x1b[C" * 5 + "\x1b[3~\r"), "XB")
    # ^V inserts a tab rather than completing
    check(vi_line(sh, "echo 'a\x16\tb' | tr '\\t' T\r"), "aTb")


def t_vi_mode():
    sh = new_shell(term="xterm")
    sh.run("set -o vi")
    for keys, out in [
        # motions and operators, with counts
        ("echo abc def" + UP + ESC + "0wcwxyz" + ESC + "\r", "XYZ DEF"),
        ("echo abc def ghi" + UP + ESC + "0wd2w\r", "GHI"),
        ("echo abc def ghi" + UP + ESC + "0w2dw\r", "GHI"),
        ("echo abc def ghi" + UP + ESC + "0wdfe\r", "F GHI"),
        ("echo abc def ghi" + UP + ESC + "0wdtg\r", "GHI"),
        ("echo abc;def" + UP + ESC + "0wwdw\r", "ABCDEF"),
        ("echo abcdef" + UP + ESC + "8|x\r", "ABDEF"),
        ("echo abc ab" + UP + ESC + "0wwcwx" + ESC + "0w;\r", "ABC X"),
        # insert, append, replace, with counts
        ("echo abc" + ESC + "a def" + UP + ESC + "\r", "ABC DEF"),
        ("echo ab" + UP + ESC + "0w3ix" + ESC + "\r", "XXXAB"),
        ("echo abc" + UP + ESC + "0wRxy\x7f" + ESC + "\r", "XBC"),
        ("echo abc" + UP + ESC + "0wfcrz\r", "ABZ"),
        ("echo abc" + UP + ESC + "Secho xy" + UP + ESC + "\r", "XY"),
        # yank and put, undo, repeat
        ("echo abc def" + UP + ESC + "0wywwP\r", "ABC ABC DEF"),
        ("echo abc def" + UP + ESC + "0wdwu\r", "ABC DEF"),
        ("echo abc def" + UP + ESC + "0wdwwU\r", "ABC DEF"),
        ("echo abc def" + UP + ESC + "0wcwx" + ESC + "w.\r", "X X"),
        ("echo abcdef" + UP + ESC + "0w2x3.\r", "F"),
        # an arrow key after ESC is still an arrow key
        ("echo ab" + ESC + "\x1b[D" + "ix" + ESC + "A" + UP + ESC + "\r", "XAB"),
        # history: k, j, G, / and n
        ("echo zero" + UP + "\r", "ZERO"),
        ("echo one" + UP + "\r", "ONE"),
        (ESC + "kk\r", "ZERO"),
        (ESC + "kkkj\r", "ONE"),
        (ESC + "/zer\r\r", "ZERO"),
        (ESC + "/^echo o\rn\r", "ONE"),
        ("echo q" + ESC + "_" + ESC + "a" + UP + ESC + "\r", "Q A-Z"),
    ]:
        check(vi_line(sh, keys), out)
    # @x runs alias _x as keys; v edits the line with $VISUAL
    sh.run("alias _q=Aqq; VISUAL='sed -i.bak s/a/b/'")
    check(vi_line(sh, "echo b" + ESC + "@q" + ESC + "a" + UP + ESC + "\r"),
          "BQQ")
    sh.buf = ""
    sh.send("echo abc" + UP + ESC + "v")
    sh.expect(r"\necho bbc \| tr a-z A-Z\nBBC\n")


def t_interrupt_ends_list():
    # ^C reaches only the foreground job, but the shell must still stop
    # running the rest of the command line
    sh = new_shell(wrapped=True)
    sh.line("for i in 1 2 3; do sleep 5; echo $i; done; echo after")
    time.sleep(0.3)
    sh.send("\x03")
    sh.expect(r"\$ ")
    check(sh.run("echo $?"), "130\n")


def t_command_substitution():
    # a command substitution runs in the shell's process group, where ^Z
    # must not stop it: nothing could continue it
    sh = new_shell(wrapped=True)
    sh.line("echo $(sleep 30)")
    time.sleep(0.3)
    sh.send("\x1a")
    time.sleep(0.3)
    sh.send("\x03")
    sh.expect(r"\$ ")
    check(sh.run("echo ok"), "ok\n")


def t_background_tty_read():
    sh = new_shell(wrapped=True)
    sh.line("cat &")
    sh.expect(r"\[1\] \d+\n\$ ")
    time.sleep(0.5)
    out = sh.run("true")
    check(out, "[1]+  Stopped (SIGTTIN)       cat\n")
    check(reported(sh, "kill -9 %1"), "[1]+  Killed                  cat\n")


def t_current_and_previous():
    sh = new_shell(wrapped=True)
    for n in (31, 32):
        sh.line("sleep %d" % n)
        time.sleep(0.3)
        sh.send("\x1a")
        sh.expect(r"Stopped                 sleep %d\n\$ " % n)
    check(sh.run("jobs"),
          "[1]-  Stopped                 sleep 31\n"
          "[2]+  Stopped                 sleep 32\n")
    check(sh.run("bg %-"), "[1]- sleep 31 &\n")
    out = sh.run("jobs %?31; jobs %%")
    check(out, "[1]-  Running                 sleep 31 &\n"
               "[2]+  Stopped                 sleep 32\n")
    sh.line("kill -9 %1 %2")
    sh.expect(r"\$ ")


ECHO = 'stty -a | grep -o -- " -*echo "'


def t_terminal_modes():
    # a stopped job's terminal modes are put aside while the shell runs,
    # and given back by fg
    sh = new_shell(wrapped=True)
    sh.line("sh -c 'stty -echo; kill -STOP $$; %s; stty echo'" % ECHO)
    sh.expect(r"Stopped.*\n\$ ")
    check(sh.run(ECHO), " echo \n")
    sh.line("fg")
    sh.expect(r" -echo \n\$ ")


def t_killed_job_modes():
    # modes left behind by a job a signal killed are undone
    sh = new_shell(wrapped=True)
    sh.line("sh -c 'stty -echo; sleep 30'")
    time.sleep(0.5)
    sh.send("\x03")
    sh.expect(r"\$ ")
    check(sh.run(ECHO), " echo \n")


def t_kill_stopped_job():
    # SIGTERM reaches a stopped job only once it is continued
    sh = new_shell(wrapped=True)
    sh.line("sleep 30")
    time.sleep(0.3)
    sh.send("\x1a")
    sh.expect(r"Stopped                 sleep 30\n\$ ")
    check(reported(sh, "kill %1"), "[1]+  Terminated              sleep 30\n")


def t_exec_keeps_terminal():
    sh = new_shell(wrapped=True)
    sh.line("exec sh -c 'ps -o pgid=,tpgid= -p $$'")
    pgid, tpgid = sh.expect(r"(\d+) +(\d+)\n").groups()
    check(pgid, tpgid)


def t_exit_with_stopped_jobs():
    sh = new_shell(wrapped=True)
    sh.line("sleep 30")
    time.sleep(0.3)
    sh.send("\x1a")
    sh.expect(r"Stopped                 sleep 30\n\$ ")
    check(sh.run("exit"), "You have stopped jobs.\n")
    sh.line("exit")
    _, status = os.waitpid(sh.pid, 0)
    check(os.WIFEXITED(status), True)
    sh.pid = 0


def t_done_notification():
    sh = new_shell(wrapped=True)
    sh.line("sleep 0.2 &")
    sh.expect(r"\[1\] \d+\n\$ ")
    time.sleep(0.6)
    check(sh.run("true"), "[1]+  Done                    sleep 0.2\n")
    check(sh.run("jobs"), "")


def t_async_notification():
    # set -b: report while the line editor waits for input
    sh = new_shell(term="xterm")
    sh.line("set -b; sleep 0.3 &")
    sh.expect(r"\[1\] \d+\n")
    sh.expect(r"\[1\]\+  Done                    sleep 0.3\n")


def t_monitor_off():
    sh = new_shell(wrapped=True)
    sh.run("set +m")
    check(sh.run("case $- in *m*) echo on;; *) echo off;; esac"), "off\n")
    check(sh.run("fg 2>&1"), "cpsh: fg: no job control\n")
    sh.line("sleep 30")
    time.sleep(0.3)
    sh.send("\x1a")   # the shell and the job share a process group: no stop
    sh.send("\x03")
    sh.expect(r"\$ ")
    check(sh.run("echo ok"), "ok\n")


def t_noninteractive_set_m():
    # set -m in a script run on a terminal: jobs get process groups
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(SH, [SH, "-c",
                      "set -m; sleep 5 & "
                      "test \"$(ps -o pgid= -p $!)\" -eq $! && echo own group; "
                      "kill %1"])
    out = b""
    while True:
        try:
            data = os.read(fd, 4096)
        except OSError:
            break
        if not data:
            break
        out += data
    os.waitpid(pid, 0)
    check(out.decode().replace("\r", ""), "own group\n")


def main():
    cases = [(k, v) for k, v in globals().items() if k.startswith("t_")]
    passed = failed = 0
    for name, fn in cases:
        try:
            fn()
            passed += 1
        except Failure as e:
            failed += 1
            print("FAIL: %s\n%s\n" % (name[2:], e))
        while shells:
            shells.pop().close()
    print("passed: %d, failed: %d" % (passed, failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
