#!/usr/bin/env python3
"""The CPU one GDI calibration lane costs, measured: `calib.py engine` on one lane in a cgroup of
its own, the cgroup's CPU time read every second, set beside a Vulkan lane's figure.

    lanecpu.py --against THREADS [--scenario PATH] [--runs N] [--out DIR]

THE CLAIM. A calibration lane renders with GDI by default (tareplay.GDI_LEVER) and makes no Vulkan
device, so it costs the machine no more CPU than a lane on the default renderer, whose peak the
caller gives as --against (threads: CPU seconds a wall second). The run lock sizes a lane by its
renderer's weight (tools/runlock.py, THE LANE WEIGHTS; tools/runlock-lanes.cfg), and the mean this
reports is a measurement of the GDI figure there.

HOW. `systemd-run --user --scope` starts `calib.py engine --j 1` in a scope cgroup: every process
the lane makes -- the harness, tacli, its Xvfb, Wine and the game -- lands in it and stays there
when it is reparented, and the cgroup's cpu.stat `usage_usec` counts their CPU time, an exited
process's included. A wineserver another game started before this one serves this game from
outside the scope, so its share is not counted: run it when no other game holds the prefix.

THE VERDICT, each a line of the report:
  * calib engine played every run (its exit 0);
  * every lane-launch's log names the GDI renderer (`cfg: ... renderer GDI (tagpu_gdi.on)`) and has
    no `vk: ` line: the lane made no Vulkan device;
  * the peak over WINDOW_S seconds, in threads, is at most --against.
It reports the mean over the run and the one-second peak beside them. Exit 0 when every line holds,
1 when one does not, 2 when the run could not be made. The calibration takes the run lock itself.
"""
import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parent
CALIB = HERE / "tasim" / "calib.py"
SCENARIO = TREE / "scenarios" / "calib" / "duel-armpw-corak.json"
SAMPLE_S = 1.0
WINDOW_S = 10                   # the peak is the highest mean over this many samples
GDI_LINE = re.compile(r"cfg: .*; renderer GDI \(tagpu_gdi\.on\)")
VK_LINE = re.compile(r"^vk: ", re.M)


class CouldNotRun(RuntimeError):
    pass


def say(text):
    print(f"lanecpu: {text}", flush=True)


def cgroup_of(unit: str) -> Path:
    """The scope's cgroup. systemd-run --scope --unit=NAME makes NAME.scope, and systemctl reads a bare
    name as NAME.service, which has none, so the suffix is spelled out."""
    r = subprocess.run(["systemctl", "--user", "show", "-p", "ControlGroup", "--value", f"{unit}.scope"],
                       capture_output=True, text=True, timeout=30)
    cg = r.stdout.strip()
    if r.returncode or not cg:
        raise CouldNotRun(f"no cgroup for {unit}: {r.stderr.strip() or 'systemctl said nothing'}")
    return Path("/sys/fs/cgroup") / cg.lstrip("/")


def usage_usec(cpu_stat: str) -> int:
    for ln in cpu_stat.splitlines():
        k, _, v = ln.partition(" ")
        if k == "usage_usec":
            return int(v)
    raise CouldNotRun("cpu.stat has no usage_usec")


def threads(samples) -> list:
    """[(t, threads)] a sample: CPU seconds over wall seconds between consecutive (t, usage_usec)."""
    return [(t1, (u1 - u0) / 1e6 / (t1 - t0)) for (t0, u0), (t1, u1) in zip(samples, samples[1:])
            if t1 > t0]


def peak(rates, window: int) -> float:
    """The highest mean of `window` consecutive rates (all of them when there are fewer)."""
    v = [r for _t, r in rates]
    if not v:
        return 0.0
    w = min(window, len(v))
    return max(sum(v[i:i + w]) / w for i in range(len(v) - w + 1))


def lane_logs(out: Path) -> dict:
    return {d.name: (d / "tagpu.log").read_text(errors="replace") if (d / "tagpu.log").exists() else ""
            for d in sorted(out.glob("lane*")) if d.is_dir()}


def judge(rc: int, logs: dict, rates, against: float) -> tuple:
    """(lines, ok): the verdict's lines, each starting PASS or FAIL."""
    lines = [f"{'PASS' if rc == 0 else 'FAIL'}: calib engine exited {rc}"]
    if not logs:
        lines.append("FAIL: no lane-launch log")
    for name, text in logs.items():
        gdi, vk = bool(GDI_LINE.search(text)), len(VK_LINE.findall(text))
        lines.append(f"{'PASS' if gdi and not vk else 'FAIL'}: {name}: "
                     f"{'renderer GDI' if gdi else 'no GDI renderer line'}, {vk} vk: lines")
    p = peak(rates, WINDOW_S)
    mean = sum(r for _t, r in rates) / len(rates) if rates else 0.0
    p1 = max((r for _t, r in rates), default=0.0)
    lines.append(f"{'PASS' if rates and p <= against else 'FAIL'}: peak {p:.2f} threads over {WINDOW_S} s "
                 f"(one-second peak {p1:.2f}, mean {mean:.2f}, {len(rates)} s), the Vulkan lane's {against:.2f}")
    return lines, all(ln.startswith("PASS") for ln in lines)


def play(a, out: Path):
    unit = f"lanecpu-{time.strftime('%Y%m%d-%H%M%S')}"
    cmd = ["systemd-run", "--user", "--scope", "--quiet", f"--unit={unit}", "--",
           sys.executable, str(CALIB), "engine", "--scenarios", str(Path(a.scenario).resolve().relative_to(TREE)),
           "--runs", str(a.runs), "--j", "1", "--out", str(out)]
    say(" ".join(cmd))
    log = (out / "calib.log").open("w")
    proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
    try:
        cg = None
        deadline = time.monotonic() + 30
        while cg is None:
            try:
                cg = cgroup_of(unit)
            except CouldNotRun:
                if proc.poll() is not None or time.monotonic() > deadline:
                    raise
                time.sleep(0.2)
        samples = []
        while proc.poll() is None:
            try:
                samples.append((time.monotonic(), usage_usec((cg / "cpu.stat").read_text())))
            except OSError:
                break                       # the scope ended with its last process
            time.sleep(SAMPLE_S)
        return proc.wait(), samples
    finally:
        # every way out stops the scope, which kills every process in it -- the calibration among them, so
        # its run-lock slots are released and no game outlives the measurement
        if proc.poll() is None:
            subprocess.run(["systemctl", "--user", "stop", f"{unit}.scope"], capture_output=True, timeout=60)
            try:
                proc.wait(timeout=60)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        log.close()


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--against", type=float, required=True,
                    help="the Vulkan calibration lane's peak, threads, the figure this is set beside")
    ap.add_argument("--scenario", default=str(SCENARIO))
    ap.add_argument("--runs", type=int, default=50)
    ap.add_argument("--out", type=Path, default=None)
    a = ap.parse_args(argv)
    root = a.out or TREE / ".data" / "lanecpu" / time.strftime("%Y%m%d-%H%M%S")
    root.mkdir(parents=True, exist_ok=True)
    try:
        rc, samples = play(a, root)
    except (CouldNotRun, OSError, ValueError) as e:
        say(f"COULD NOT RUN: {e}")
        return 2
    runs = sorted(p for p in root.iterdir() if p.is_dir())
    logs = lane_logs(runs[-1]) if runs else {}
    rates = threads(samples)
    lines, ok = judge(rc, logs, rates, a.against)
    for ln in lines:
        print(ln)
    (root / "report.json").write_text(json.dumps({"rc": rc, "samples": samples, "lines": lines}, indent=1) + "\n")
    if rc == 2 and not logs:
        return 2
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
