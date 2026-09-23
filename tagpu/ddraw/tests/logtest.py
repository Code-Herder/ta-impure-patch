#!/usr/bin/env python3
"""logtest.py — run tests/logtest.exe under Wine and check every promise of the log sink.

    make -C tagpu/ddraw logtest && tagpu/ddraw/tests/logtest.py

Each case runs in its own scratch folder, so the sink creates a fresh `log\\` beside the
exe. The exe is built with a 64 KB file cap, a 512 KB total cap and 10 rotated files per
stream (the Makefile's logtest target); the DLL's real caps differ only in size.

WINEPREFIX defaults to the main checkout's `wineprefix/`. Exit 0 when every case passes.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2] / "tools"))
import talog  # noqa: E402

FILE_CAP, TOTAL_CAP, KEEP = 65536, 524288, 10
EXE = HERE / "logtest.exe"
ROT_RX = re.compile(r"^(tagpu|tagpu_cobtrace)\.(\d+)\.log$")
failures = []


def env():
    e = dict(os.environ, WINEDEBUG="-all")
    if "WINEPREFIX" not in e:
        common = subprocess.run(["git", "rev-parse", "--git-common-dir"], cwd=HERE,
                                capture_output=True, text=True).stdout.strip()
        e["WINEPREFIX"] = str((HERE / common).resolve().parent / "wineprefix")
    return e


def check(cond, what):
    print(f"  {'ok  ' if cond else 'FAIL'} {what}")
    if not cond:
        failures.append(what)


def scratch():
    d = Path(tempfile.mkdtemp(prefix="logtest-"))
    shutil.copy2(EXE, d / "logtest.exe")
    return d


def run(d, *args, timeout=300):
    r = subprocess.run(["wine", str(d / "logtest.exe"), *args], cwd=d, env=env(),
                       capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout + r.stderr


def ours(d):
    """{name: size} of the sink's own files in log/."""
    out = {}
    for p in (d / "log").iterdir():
        if p.name in ("tagpu.log", "tagpu_cobtrace.log") or ROT_RX.match(p.name):
            out[p.name] = p.stat().st_size
    return out


def check_caps(d, label):
    files = ours(d)
    check(all(v <= FILE_CAP for v in files.values()),
          f"{label}: every file <= {FILE_CAP} (largest {max(files.values(), default=0)})")
    check(sum(files.values()) <= TOTAL_CAP, f"{label}: total {sum(files.values())} <= {TOTAL_CAP}")
    for s in ("tagpu", "tagpu_cobtrace"):
        n = sum(1 for k in files if ROT_RX.match(k) and k.startswith(s + "."))
        check(n <= KEEP, f"{label}: {n} rotated {s} files <= {KEEP}")


def parts_oldest_first(d, stream="tagpu"):
    return list(reversed(list(talog.run_parts_newest_first(d, stream))))


def seqs(lines, rx):
    return [int(m.group(1)) for m in map(rx.match, lines) if m]


def contiguous(xs):
    return all(b == a + 1 for a, b in zip(xs, xs[1:]))


def case_stress():
    print("stress: two threads, both streams, blocks, a stray file, attach rotation")
    d = scratch()
    (d / "log").mkdir()
    (d / "log" / "notes.txt").write_bytes(b"n" * 1_000_000)          # not ours: never touched
    (d / "log" / "tagpu.backup.log").write_bytes(b"b" * 300_000)     # not our name pattern
    (d / "log" / "tagpu.11.log").write_bytes(b"old\r\n")             # our pattern, past KEEP
    rc, out = run(d, "stress", "20000")
    check(rc == 0 and "ok stress" in out, f"stress exits ok (in-process cap checks): {out.strip()[-200:]}")
    check_caps(d, "stress")
    check((d / "log" / "notes.txt").stat().st_size == 1_000_000, "a stray notes.txt survives untouched")
    check((d / "log" / "tagpu.backup.log").stat().st_size == 300_000, "tagpu.backup.log survives untouched")
    check(not (d / "log" / "tagpu.11.log").exists(), "tagpu.11.log (past KEEP) is deleted at attach")

    parts = parts_oldest_first(d)
    heads = [talog._header(p) for p in parts]
    nums = [h[1] for h in heads]
    check(len(parts) >= 2 and contiguous(nums), f"run parts are consecutive: {nums}")
    check(len({h[0] for h in heads}) == 1, "every part carries the same run id")
    check(all(p.endswith(b"\r\n") for p in parts), "main stream ends lines with CRLF")
    check(all(re.search(rb"log: continued in tagpu\.log \(part \d+\)\r\n$", p) for p in parts[:-1]),
          "every rotated part ends with its `continued in` line")
    lines = b"".join(parts).decode().splitlines()
    for t in (1, 2):
        xs = seqs(lines, re.compile(rf"^t{t} (\d+) "))
        check(xs and xs[-1] == 19999 and contiguous(xs),
              f"thread {t}: lines {xs[0] if xs else '-'}..{xs[-1] if xs else '-'} with no gap")
    cut = [l for l in lines if l.endswith("...[truncated]")]
    check(any(set(l[:-14]) == {"x"} and len(l) == 1024 + 14 for l in cut), "a 5000-byte line is cut to 1024 and marked")
    # blocks: begin, n lines, end -- consecutive, inside ONE file
    bad = blocks = 0
    for p in parts:
        pl = p.decode().splitlines()
        for i, l in enumerate(pl):
            m = re.match(r"^B(\d+\.\d+) begin (\d+)$", l)
            if not m:
                continue
            blocks += 1
            tag, n = m.group(1), int(m.group(2))
            want = [f"B{tag} line {k}" for k in range(n)] + [f"B{tag} end"]
            if pl[i + 1:i + 2 + n] != want:
                bad += 1
    check(blocks > 0 and bad == 0, f"{blocks} blocks, each whole and in one file ({bad} broken)")
    cob = parts_oldest_first(d, "tagpu_cobtrace")
    check(cob and all(p.startswith(b"# log: run ") for p in cob), "cobtrace parts start with `# log: run`")
    check(all(b"\r\n" not in p for p in cob), "cobtrace stream ends lines with LF")

    first_run = heads[-1][0]
    rc, out = run(d, "stress", "10")
    heads2 = [talog._header(p) for p in parts_oldest_first(d)]
    check(heads2 and heads2[0][0] != first_run and heads2[0][1] == 1,
          "a second launch starts a new run at part 1")
    prev = talog.rotated(d, "tagpu", 1).read_bytes()
    check(talog._header(prev)[0] == first_run, "the previous run's last part is now tagpu.1.log")
    check_caps(d, "second launch")
    return d


def case_block():
    print("block: a handle without FILE_SHARE_DELETE holds tagpu.log for 2.5 s")
    d = scratch()
    rc, out = run(d, "block")
    check(rc == 0 and "ok block" in out, f"block exits ok (in-process cap checks): {out.strip()[-200:]}")
    check_caps(d, "block")
    lines = b"".join(parts_oldest_first(d)).decode().splitlines()
    notes = [re.search(r"log: (\d+) lines dropped over (\d+) ms", l) for l in lines]
    dropped = sum(int(m.group(1)) for m in notes if m)
    check(dropped > 0, f"the gap is recorded: {dropped} lines dropped")
    last = [int(m.group(1)) for m in map(re.compile(r"^block: last (\d+)").match, lines) if m]
    xs = seqs(lines, re.compile(r"^L (\d+) "))
    check(last and xs and xs[-1] == last[0], "writing resumed after the handle closed")
    # history may have evicted the start; count the gap inside what survives
    missing = (xs[-1] - xs[0] + 1) - len(xs)
    check(missing == dropped, f"lines missing from the surviving range ({missing}) == lines counted dropped ({dropped})")
    return d


def case_second():
    print("second: a second process in the same folder writes nothing and changes nothing")
    d = scratch()
    holder = subprocess.Popen(["wine", str(d / "logtest.exe"), "hold", "8000"], cwd=d, env=env(),
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    line = holder.stdout.readline()
    check("ok holding" in line, f"the first process owns log\\ ({line.strip()})")
    snap = lambda: {p.name: (p.stat().st_size, p.stat().st_mtime_ns) for p in (d / "log").iterdir()}
    before = snap()
    rc, out = run(d, "second", "1000")
    check(rc == 0 and "ok second" in out, "the second process runs")
    check(snap() == before, f"log\\ is unchanged by the second process ({sorted(before)})")
    holder.wait(timeout=60)
    return d


def case_kill():
    print("kill: a process killed mid-stream loses no line it had written")
    d = scratch()
    p = subprocess.Popen(["wine", str(d / "logtest.exe"), "forever"], cwd=d, env=env(),
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    p.stdout.readline()
    time.sleep(2.0)
    os.kill(p.pid, signal.SIGKILL)
    p.wait(timeout=30)
    time.sleep(1.0)
    check_caps(d, "kill")
    parts = parts_oldest_first(d)
    data = b"".join(parts)
    lines = data.decode().splitlines()
    xs = seqs(lines, re.compile(r"^F (\d+) "))
    complete = data.endswith(b"\r\n") or re.search(rb"F \d+ pad=0+$", data.rsplit(b"\r\n", 1)[-1]) is None
    check(xs and contiguous(xs), f"lines {xs[0] if xs else '-'}..{xs[-1] if xs else '-'} with no gap")
    check(complete, "no torn line at the end")
    return d


def case_cursor():
    print("cursor: talog.Cursor follows three rotations after history is full")
    d = scratch()
    p = subprocess.Popen(["wine", str(d / "logtest.exe"), "slow"], cwd=d, env=env(),
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    p.stdout.readline()

    def part():
        try:
            return talog._header(talog.current(d).read_bytes()[:256])[1] or 0
        except FileNotFoundError:
            return 0

    t0 = time.time()
    while part() < KEEP + 4 and time.time() - t0 < 120:      # history full: eviction runs,
        time.sleep(0.1)                                       # and inodes get reused
    c = talog.Cursor(d)
    key, off, n = c.key, c.off, part()
    while part() < n + 3 and time.time() - t0 < 180:
        time.sleep(0.05)
    got = c.read()
    os.kill(p.pid, signal.SIGKILL)
    p.wait(timeout=30)
    parts = list(talog.run_parts_newest_first(d))
    i = next((i for i, x in enumerate(parts) if talog._key(x[:256]) == key), None)
    expected = parts[i][off:] + b"".join(reversed(parts[:i])) if i is not None else b""
    check(n > KEEP and i is not None and expected.startswith(got) and len(got) > 3 * 60000,
          f"parts {n}..{part()}: the cursor returned exactly the {len(got)} bytes written after it")
    return d


def main():
    if not EXE.exists():
        sys.exit(f"{EXE} is missing: make -C tagpu/ddraw logtest")
    keep = "--keep" in sys.argv
    dirs = [case_stress(), case_block(), case_second(), case_kill(), case_cursor()]
    if not keep:
        for d in dirs:
            shutil.rmtree(d, ignore_errors=True)
    print(f"\n{'FAILED: ' + '; '.join(failures) if failures else 'all cases passed'}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
