#!/usr/bin/env python3
"""Compare the wall time and peak memory of two shell binaries.

    python3 bench/benchmark.py OLD_SHELL NEW_SHELL [--runs N]

Every workload is a plain list of simple commands fed on standard input and
terminated by `exit`, so that it runs on the original cpsh as well (which
supports nothing more). Each workload runs N times per shell, alternating
between the shells, and the median is reported.
"""
import argparse
import os
import resource
import shutil
import statistics
import subprocess
import tempfile
import time

N = 2000


def workloads():
    return {
        # fork/exec cost: an external command given by absolute path
        "external /bin/true": ["/bin/true"] * N,
        # PATH search on every command
        "external via PATH (uname)": ["uname"] * N,
        # arguments and output
        "external with args (/bin/echo)": [f"/bin/echo line {i} of output" for i in range(N)],
        # commands that the shell now runs without forking
        "builtins (echo/true/cd/pwd)": ["echo hello", "true", "cd /tmp", "pwd"] * (N // 4),
        # parsing/history bookkeeping with distinct lines, no processes
        "builtin cd, 10k distinct lines": [f"cd /tmp/{'.' if i % 2 else '..'}" for i in range(5 * N)],
    }


def run(shell, script, home):
    env = {"PATH": "/usr/local/bin:/usr/bin:/bin", "HOME": home, "USER": "bench"}
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    start = time.perf_counter()
    with open(script, "rb") as stdin:
        subprocess.run([shell], stdin=stdin, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, env=env, check=False)
    elapsed = time.perf_counter() - start
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    return elapsed, after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime


def peak_rss_kb(shell, script, home):
    """Peak RSS of the shell process itself (via /usr/bin/time if present)."""
    timebin = shutil.which("time") or "/usr/bin/time"
    if not os.path.exists(timebin):
        return None
    env = {"PATH": "/usr/bin:/bin", "HOME": home, "USER": "bench"}
    with open(script, "rb") as stdin:
        p = subprocess.run([timebin, "-f", "%M", shell], stdin=stdin,
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
    try:
        return int(p.stderr.decode().strip().splitlines()[-1])
    except (ValueError, IndexError):
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("old")
    ap.add_argument("new")
    ap.add_argument("--runs", type=int, default=7)
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="cpsh-bench.")
    home = os.path.join(tmp, "home")
    os.makedirs(home)
    print(f"{'workload':34} {'old (s)':>9} {'new (s)':>9} {'speedup':>8} {'faster':>8}")
    total_old = total_new = 0.0
    for name, lines in workloads().items():
        script = os.path.join(tmp, "script")
        with open(script, "w") as f:
            f.write("\n".join(lines + ["exit"]) + "\n")
        times = {"old": [], "new": []}
        for _ in range(args.runs):
            for key in ("old", "new"):
                # start each run with an empty history file
                for fn in os.listdir(home):
                    os.remove(os.path.join(home, fn))
                times[key].append(run(getattr(args, key), script, home)[0])
        old = statistics.median(times["old"])
        new = statistics.median(times["new"])
        total_old += old
        total_new += new
        print(f"{name:34} {old:9.3f} {new:9.3f} {old / new:7.2f}x {100 * (1 - new / old):7.1f}%")
    print(f"{'TOTAL':34} {total_old:9.3f} {total_new:9.3f} {total_old / total_new:7.2f}x "
          f"{100 * (1 - total_new / total_old):7.1f}%")

    script = os.path.join(tmp, "script")
    with open(script, "w") as f:
        f.write("\n".join(workloads()["builtin cd, 10k distinct lines"] + ["exit"]) + "\n")
    old_rss = peak_rss_kb(args.old, script, home)
    new_rss = peak_rss_kb(args.new, script, home)
    if old_rss and new_rss:
        print(f"\npeak RSS, 10k-line session: old {old_rss} KiB, new {new_rss} KiB "
              f"({100 * (1 - new_rss / old_rss):.1f}% less)")
    shutil.rmtree(tmp)


if __name__ == "__main__":
    main()
