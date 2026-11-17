#!/usr/bin/env python3
"""The run lock's GPU bound, shown: N harness games of one heavy scenario at once, each game's
device memory sampled from nvidia-smi every second, judged against the figures its GPU memory is
sized on (the lock's runslots/vram.cfg, seeded from tools/runlock-vram.cfg). And a suite's peak,
measured: `watch` samples every game a suite's command plays.

    vramproof.py [--scenario 200v200] [-n N] [--play S] [--out DIR]
    vramproof.py watch [--cap N] [--out DIR] -- CMD ...

THE CLAIM. A game launched inside a run-lock hold runs under the GPU memory cap (`gpumem=`,
tools/tacli write_gpumem): the DLL charges every device-local allocation against it and refuses one
past it (inc/tagpu_vkcap.h), so the game's own allocations never sum past cap_mib, and with what
the driver holds beside them it stays within B = cap_mib + driver_mib (runlock.game_bound_mib). A
game takes B plus a margin of the GPU budget, so the games the run lock admits together stay
within the device's memory less reserve_mib. This plays the case that tests it hardest: the play
defaults (`--defaults`: ss 2, the restorer, shadowres 2048) on a battle scenario, as many games at
once as the budget allows.

THE VERDICT, each a line of the report and a failure when it does not hold:
  * every game was seen on the GPU and logged the cap the run lock sized it on
    (`vk: gpumem: this device's allocations are capped at <cap_mib> MiB`): the cap applied;
  * every game's peak, nvidia-smi per process, is at most B;
  * at every sample the games together hold at most the device's memory less reserve_mib;
  * no game's cap refused an allocation (`vk: gpumem: the cap refuses`): a refusal is the bound
    holding, but it also means the cap took memory from a pass that was not sized against free
    memory, so the cap is too small for that game and changed what it draws.
It also reports, per game, the ledger's own peak (the DLL's `held at most` line) beside the
process's, whose difference is what the driver holds outside the ledger (driver_mib's allowance),
and whether the terrain restored by neighbourhood or per tile under the cap.

Holds the run lock for N lanes (default: as many games as the GPU budget holds), so it waits for
the machine like any other run. Each game has its own Xvfb and its own instance,
`vramproof-<tag>-<k>`, made afresh and removed at the end; the samples, the games' logs and
report.json go to --out. Exit 0 when every line holds, 1 when one does not, 2 when the run could not
be made.

WATCH: a suite's peak, the figure its cap in tools/runlock-suites.cfg is set from. CMD runs as given
and takes the run lock itself; with --cap N it runs with RUNLOCK_CAP_MIB=N (tools/runlock.py, A
MEASUREMENT'S CAP), so the run takes the GPU memory of its games at N, fewer of them at once, and
launches every game at N: a suite measured under a cap above its peak is not cut at the cap being
measured, and no tracked file changes for it. Without --cap the games run under the suite's present
cap. Every TotalA.exe on the machine is sampled once a second, and a sample counts only the games
of the watched run: the run whose holder is CMD or a process below it (its tag, runlock TAGS), and
of that run's games those whose process holds one of its seats. A game of any other run -- another
suite beside it, a hand launch -- is not counted, so the figures together are the suite's own. Each
counted game's DLL log is read as it plays for the ledger's peak (`held at most`, its high waters).
The report gives, per instance and over all of them, the most a process held (nvidia-smi), the most
its ledger held, the most the run's games held together, and every refusal of the cap. Exit: CMD's;
2 when the watch could not run.

WHAT A MEASUREMENT NEEDS, in the worktree the suite runs from:
  * Outside every run: watch refuses inside one (RUNLOCK_HELD, or `runlock.py run`), whose cap and
    seats the watched run would take as its own.
  * The suite's own setup. The wiki gate draws on art derived from the game's files, which is
    gitignored and so absent from a fresh worktree: run `.venv-undither/bin/python
    tools/wiki/gfx.py`, then `.venv-undither/bin/python tools/wiki/sidepacks.py`, first. Without
    them the gate's game lanes fail (no gfx/cursors.json, `no sidebar pack for <mod>`) and the
    measurement has fewer games than the gate plays.
  * --cap at least the suite's present cap, and above what it is expected to hold: a cap the suite
    reaches shows as refusals in the report, which means measure again higher.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import threading
import time
import xml.etree.ElementTree as ET
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE / "compat"))
import runlock  # noqa: E402  the machine's run lock and the GPU figures
import talog  # noqa: E402  the DLL's logs across their rotations
import tacompat  # noqa: E402  start_xvfb / stop_xvfb: a display owned through -displayfd

TACLI = HERE / "tacli"
SCENARIOS = TREE / "scenarios"


def tacli_instances() -> Path:
    """The folder tacli makes its instances in: tacli's own INSTANCES, read by loading the tacli
    this tool runs, so the two agree wherever this runs from -- in a linked worktree tacli's
    repo_root is the main checkout, and TACLI_ROOT moves both."""
    spec = spec_from_loader("_vramproof_cli", SourceFileLoader("_vramproof_cli", str(TACLI)))
    cli = module_from_spec(spec)
    spec.loader.exec_module(cli)
    return cli.INSTANCES


INSTANCES = tacli_instances()
PREFIX = "vramproof"
SAMPLE_S = 1
NVIDIA_S = 30                   # a hang bound on one nvidia-smi call, deciding nothing
LOAD_TIMEOUT = 900              # one `scenario load`, launch and level included

CAPPED = re.compile(r"^vk: gpumem: this device's allocations are capped at (\d+) MiB", re.M)
REFUSED = re.compile(r"^vk: gpumem: the cap refuses .*$", re.M)
HIGH = re.compile(r"^vk: gpumem: (\d+) MiB held of the \d+ MiB cap", re.M)
DOWN = re.compile(r"^vk: gpumem: the device is down - it held at most (\d+) MiB", re.M)
TERRAIN = re.compile(r"^vk: terr: (the terrain restores by neighbourhood .*|"
                     r"the \d+x\d+ neighbourhood atlas .* does not fit: .*)$", re.M)


class CouldNotRun(RuntimeError):
    pass


def say(text):
    print(f"vramproof: {text}", flush=True)


def tacli(*args, timeout=300):
    r = subprocess.run([sys.executable, str(TACLI), *map(str, args)], capture_output=True,
                       text=True, timeout=timeout)
    return r.returncode, r.stdout + r.stderr


def last(text):
    lines = text.strip().splitlines()
    return lines[-1] if lines else ""


# ---- the samples -------------------------------------------------------------------------------

def gpu_processes():
    """(device MiB used, device MiB total, {pid: MiB}) of the largest GPU, nvidia-smi's XML."""
    r = subprocess.run(["nvidia-smi", "-q", "-x"], capture_output=True, text=True,
                       timeout=NVIDIA_S)
    if r.returncode:
        raise CouldNotRun(f"nvidia-smi: {(r.stderr or r.stdout).strip()[:200]}")
    best = None
    for gpu in ET.fromstring(r.stdout).iter("gpu"):
        fb = gpu.find("fb_memory_usage")
        total = int(fb.findtext("total").split()[0])
        used = int(fb.findtext("used").split()[0])
        procs = {}
        for p in gpu.iter("process_info"):
            mib = p.findtext("used_memory") or ""
            if mib.split() and mib.split()[0].isdigit():
                procs[int(p.findtext("pid"))] = int(mib.split()[0])
        if best is None or total > best[1]:
            best = (used, total, procs)
    if best is None:
        raise CouldNotRun("nvidia-smi lists no GPU")
    return best


def watched_games(root, lock):
    """({pid: instance} of the watched run's games, the run's tags): every TotalA.exe whose process
    holds a seat of a run held by `root` or a process below it, on the lock `lock` (WATCH)."""
    tags = runlock.tags_held_by(lock, set(tacompat.descendants(root)))
    if not tags:
        return {}, tags
    return {pid: name for pid, name in game_pids(None).items()
            if runlock.seats_of(pid, lock) & tags}, tags


def game_pids(names):
    """{pid: instance} of every TotalA.exe process of the instances `names` (None: of any instance,
    named by its Wine prefix's folder), by the Wine prefix its environment names."""
    prefixes = {str(INSTANCES / n / "prefix"): n for n in names or ()}
    found = {}
    for d in Path("/proc").iterdir():
        if not d.name.isdigit():
            continue
        try:
            if b"totala.exe" not in (d / "cmdline").read_bytes().lower():
                continue
            env = (d / "environ").read_bytes().split(b"\0")
        except OSError:
            continue
        for e in env:
            if e.startswith(b"WINEPREFIX="):
                prefix = e[len(b"WINEPREFIX="):].decode(errors="replace").rstrip("/")
                name = prefixes.get(prefix) if names is not None else str(Path(prefix).parent)
                if name:
                    found[int(d.name)] = name
    return found


class Sampler(threading.Thread):
    """One sample a second until stopped: the time, the device's use and each game's MiB, by
    instance (the most of its processes, which is the game's one process on the GPU) -- the games of
    the instances `names`, or with `root` those of the run it holds on `lock` (watched_games)."""

    def __init__(self, names, path, ledgers=False, root=None, lock=None):
        super().__init__(daemon=True)
        self.names, self.path, self.root, self.lock = names, path, root, lock
        self.samples, self.error = [], None
        self.halt = threading.Event()
        # with `ledgers`, each game's folder: its log read every LEDGER_EVERY samples (watch)
        self.ledgers = {} if ledgers else None
        self.n = 0

    def run(self):
        with open(self.path, "w") as out:
            while not self.halt.is_set():
                t = time.time()
                try:
                    used, total, procs = gpu_processes()
                except (CouldNotRun, OSError, ValueError, ET.ParseError,
                        subprocess.TimeoutExpired) as e:
                    self.error = str(e)
                    self.halt.set()         # the run's wait ends: a run with no samples shows nothing
                    return
                if self.root is not None:
                    pids, tags = watched_games(self.root, self.lock)
                else:
                    pids, tags = game_pids(self.names), set()
                games = {}
                for pid, name in pids.items():
                    if pid in procs:
                        games[name] = max(games.get(name, 0), procs[pid])
                s = {"t": t, "used": used, "total": total, "games": games, "tags": sorted(tags)}
                self.n += 1
                if self.ledgers is not None and self.n % LEDGER_EVERY == 1:
                    self.read_ledgers(games)
                self.samples.append(s)
                out.write(json.dumps(s) + "\n")
                out.flush()
                self.halt.wait(max(0.0, SAMPLE_S - (time.time() - t)))


    def read_ledgers(self, games):
        """For each instance seen (a folder whose prefix/ the game runs in), what its DLL log says of
        its cap now: the run's log is re-read whole, so a run's figures replace its last ones and a
        later run of the same instance (a relaunch) adds to the instance's list."""
        for name in games:
            text = talog.run_text(Path(name) / "gamedir")
            if not text:
                continue                # the instance is gone: its last read stands
            run = talog.HEADER_RX.match(text.encode()[:talog.HEAD_BYTES])
            key = run.group(1).decode() if run else "?"
            self.ledgers.setdefault(name, {})[key] = read_log(text)


LEDGER_EVERY = 10               # samples between two reads of a watched game's log


# ---- the verdict -------------------------------------------------------------------------------

def read_log(text):
    """What a game's log says of its cap: the cap it applied (None), the ledger's peak in MiB (the
    teardown line where the game wrote one, else its highest high water, which is within 16 MiB
    under the peak), the cap's refusals and the terrain's choice."""
    capped = CAPPED.findall(text)
    down = [int(v) for v in DOWN.findall(text)]
    high = [int(v) for v in HIGH.findall(text)]
    return {"cap": int(capped[-1]) if capped else None,
            "ledger_peak": max(down + high) if down or high else None,
            "refused": REFUSED.findall(text),
            "terrain": TERRAIN.findall(text)}


def suite_report(samples, ledgers):
    """(lines, figures) of a watched suite: per instance and over all, the most one process held
    (nvidia-smi), the most its ledger held, and the cap's refusals."""
    peaks = {}
    for s in samples:
        for name, mib in s["games"].items():
            peaks[name] = max(peaks.get(name, 0), mib)
    lines, worst_ledger, refused = [], None, []
    for name in sorted(set(peaks) | set(ledgers)):
        runs = ledgers.get(name, {})
        ledger = max((r["ledger_peak"] for r in runs.values() if r["ledger_peak"] is not None), default=None)
        caps = sorted({r["cap"] for r in runs.values() if r["cap"] is not None})
        bad = [x for r in runs.values() for x in r["refused"]]
        refused += [f"{Path(name).name}: {x}" for x in bad]
        if ledger is not None:
            worst_ledger = max(worst_ledger or 0, ledger)
        lines.append(f"{Path(name).name}: a process at most {peaks.get(name, '?')} MiB; the ledger at most "
                     f"{ledger if ledger is not None else '?'} MiB over {len(runs)} run(s), capped at "
                     f"{', '.join(map(str, caps)) or 'nothing (no gpumem=)'}"
                     + (f"; the cap refused {len(bad)}" if bad else ""))
    worst = max(peaks.values(), default=None)
    together = max((sum(s["games"].values()) for s in samples), default=0)
    lines.append(f"over every game: a process at most {worst} MiB, a ledger at most {worst_ledger} MiB, the "
                 f"games together at most {together} MiB at once; {len(refused)} refusal(s) of the cap")
    lines += [f"REFUSED {x}" for x in refused]
    return lines, {"process_peak": worst, "ledger_peak": worst_ledger, "together": together,
                   "refused": refused, "games": len(peaks)}


def watch(cmd, out, cap=None):
    """CMD run, at the cap `cap` when one is given, while the games of the run it holds are sampled
    (WATCH); CMD's exit code."""
    if not cmd:
        raise CouldNotRun("watch: no command after --")
    if runlock.holding():
        raise CouldNotRun("watch inside a run: the watched command would nest in it, take its seats "
                          "and play under its cap; run the watch outside every run")
    env = dict(os.environ)
    if cap is not None:
        if not runlock.CAP_MIN_MIB <= cap <= runlock.CAP_MAX_MIB:
            raise CouldNotRun(f"--cap {cap}: the DLL takes {runlock.CAP_MIN_MIB}.."
                              f"{runlock.CAP_MAX_MIB} MiB")
        env[runlock.CAP_ENV] = str(cap)
    t0 = time.time()
    proc = subprocess.Popen(cmd, env=env)
    sampler = Sampler(None, out / "samples.jsonl", ledgers=True, root=proc.pid,
                      lock=runlock.lock_path(TREE))
    sampler.start()
    try:
        code = proc.wait()
    finally:
        sampler.halt.set()
        sampler.join(NVIDIA_S * 2)
        # the last word of every game seen, read once more after the suite: its teardown line
        sampler.read_ledgers({name for smp in sampler.samples for name in smp["games"]})
    if sampler.error:
        raise CouldNotRun(f"the sampler stopped: {sampler.error}")
    lines, figures = suite_report(sampler.samples, sampler.ledgers)
    tags = sorted({t for smp in sampler.samples for t in smp["tags"]})
    lines.insert(0, f"the watched run{'s' if len(tags) != 1 else ''}: {', '.join(tags)}, its games alone "
                 f"counted, at a cap of {cap if cap is not None else 'its suite'}" if tags else
                 "no run of the command held GPU memory: no game was counted")
    (out / "report.json").write_text(json.dumps(
        {"cmd": cmd, "cap": cap, "runs": tags, "seconds": round(time.time() - t0), "exit": code,
         **figures, "lines": lines, "ledgers": sampler.ledgers}, indent=2, default=str))
    for ln in lines:
        print(ln)
    say(f"the command exited {code}; the report is {out / 'report.json'}")
    return code


def judge(samples, logs, cfg, total):
    """(ok, lines): the verdict on `samples` (Sampler's) and `logs` ({instance: read_log}) under
    the figures `cfg` (runlock.vram_config) on a device of `total` MiB."""
    bound = runlock.game_bound_mib(cfg)
    room = total - cfg["reserve_mib"]
    ok, lines = True, []
    peaks = {}
    for s in samples:
        for name, mib in s["games"].items():
            peaks[name] = max(peaks.get(name, 0), mib)
    for name in sorted(logs):
        got, peak = logs[name], peaks.get(name)
        if peak is None:
            ok = False
            lines.append(f"FAIL {name}: never seen on the GPU")
            continue
        if got["cap"] != cfg["cap_mib"]:
            ok = False
            lines.append(f"FAIL {name}: the cap it logged is {got['cap']}, not the "
                         f"{cfg['cap_mib']} MiB the run lock sized it on")
        verdict = "ok  " if peak <= bound else "FAIL"
        ok &= peak <= bound
        ledger = got["ledger_peak"]
        outside = f", {peak - ledger} MiB outside it" if ledger is not None else ""
        lines.append(f"{verdict} {name}: peak {peak} MiB of the {bound} MiB bound; the ledger held "
                     f"at most {ledger if ledger is not None else '?'} MiB{outside} "
                     f"(driver_mib allows {cfg['driver_mib']})")
        if got["refused"]:
            ok = False
            lines.append(f"FAIL {name}: the cap refused {len(got['refused'])} allocation(s), the "
                         f"first: {got['refused'][0]}")
        for t in got["terrain"]:
            lines.append(f"     {name}: {t}")
    worst = max((sum(s["games"].values()) for s in samples), default=0)
    verdict = "ok  " if worst <= room else "FAIL"
    ok &= worst <= room
    lines.append(f"{verdict} together: at most {worst} MiB at once, of the {room} MiB the device "
                 f"leaves the runs ({total} less the {cfg['reserve_mib']} MiB reserve)")
    return ok, lines


# ---- the run -----------------------------------------------------------------------------------

def scenario_res(path):
    setup = json.loads(path.read_text()).get("setup", {})
    m = re.fullmatch(r"(\d+)x(\d+)", str(setup.get("res") or "1024x768"))
    return int(m.group(1)), int(m.group(2))


def play(args, out):
    scenario = SCENARIOS / f"{args.scenario}.json"
    if not scenario.exists():
        raise CouldNotRun(f"no scenario {scenario}")
    lock = runlock.lock_path(TREE)
    if runlock.cap_override() is not None:
        raise CouldNotRun(f"{runlock.CAP_ENV} is set: the proof plays the lock's own cap_mib, "
                          f"which the figures it judges are")
    if not runlock.gpu_budget(lock):
        raise CouldNotRun("no GPU budget here: no device memory reads, so no bound to show")
    lanes = args.n or runlock.plan(None, path=lock)[2]
    runlock.hold(TREE, f"vramproof {args.scenario} x{lanes}", lanes)
    n = runlock.games()
    # the figures judged are the lock's (runslots/vram.cfg), the ones the hold was admitted under
    cfg = runlock.vram_config(lock)
    budget = runlock.gpu_budget(lock, cfg)
    if runlock.held_cap_mib() is None:
        raise CouldNotRun("this run holds no GPU memory of its own (it is inside a run that took none), "
                          "so its games run uncapped: no bound to show")
    if cfg["cap_mib"] != runlock.held_cap_mib():
        raise CouldNotRun(f"the lock's cap moved from {runlock.held_cap_mib()} to {cfg['cap_mib']} MiB "
                          f"while this run was admitted")
    say(f"{n} games of {args.scenario} at once ({runlock.game_mib(cfg)} MiB each of the "
        f"{budget * runlock.GRAIN_MIB} MiB GPU budget; the cap {cfg['cap_mib']} MiB, the bound "
        f"{runlock.game_bound_mib(cfg)} MiB)")
    w, h = scenario_res(scenario)
    screen = f"{max(w, 1280)}x{max(h, 1024) + 120}x24"
    names = [f"{PREFIX}-{runlock.tag()}-{k}" for k in range(n)]
    servers, made = [], []
    sampler = Sampler(names, out / "samples.jsonl")
    try:
        for name in names:
            server = tacompat.start_xvfb(None, screen=screen)
            servers.append(server)
            tacli("rm", name, "--force", timeout=120)
            rc, o = tacli("create", name, "--display", f":{server.tacompat_display}",
                          timeout=300)
            if rc:
                raise CouldNotRun(f"tacli create {name}: {last(o)}")
            made.append(name)
        sampler.start()
        loads = {}

        def load(name):
            loads[name] = tacli("scenario", "load", name, scenario, "--defaults", "--restart",
                                timeout=LOAD_TIMEOUT)
        threads = [threading.Thread(target=load, args=(name,)) for name in names]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        bad = {k: last(v[1]) for k, v in loads.items() if v[0]}
        if bad:
            raise CouldNotRun("scenario load: " + "; ".join(f"{k}: {v}" for k, v in bad.items()))
        say(f"every game is in its battle; playing {args.play} s")
        sampler.halt.wait(args.play)
        if sampler.error:
            raise CouldNotRun(f"the sampler stopped: {sampler.error}")
    finally:
        for name in made:
            tacli("stop", name, timeout=120)
        sampler.halt.set()
        if sampler.is_alive():
            sampler.join(NVIDIA_S * 2)
        logs = {}
        for name in made:
            text = talog.run_text(INSTANCES / name / "gamedir")
            (out / f"{name}-tagpu.log").write_text(text)
            logs[name] = read_log(text)
            tacli("rm", name, "--force", timeout=120)
        for server in servers:
            tacompat.stop_xvfb(server)
    total = max((s["total"] for s in sampler.samples), default=0)
    ok, lines = judge(sampler.samples, logs, cfg, total)
    (out / "report.json").write_text(json.dumps(
        {"scenario": args.scenario, "games": n, "cfg": cfg, "device_mib": total, "ok": ok,
         "lines": lines, "logs": logs}, indent=2))
    for ln in lines:
        print(ln)
    say("PASS: the bound holds" if ok else "FAIL")
    return 0 if ok else 1


def main(argv=None):
    argv = sys.argv[1:] if argv is None else list(argv)
    if argv[:1] == ["watch"]:
        wa = argparse.ArgumentParser(prog="vramproof.py watch", description="a suite's peak",
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=__doc__[__doc__.index("WATCH:"):])
        wa.add_argument("--cap", type=int, default=None, metavar="N",
                        help="the cap in MiB every game of the watched run is launched at, and its GPU "
                             "memory taken at (default: the suite's own)")
        wa.add_argument("--out", type=Path, default=None,
                        help="where the samples and report go (default: .data/vramproof/watch-<time>)")
        wa.add_argument("cmd", nargs=argparse.REMAINDER, help="-- and the suite's command")
        a = wa.parse_args(argv[1:])
        cmd = a.cmd[1:] if a.cmd[:1] == ["--"] else a.cmd
        out = a.out or TREE / ".data" / "vramproof" / time.strftime("watch-%Y%m%d-%H%M%S")
        out.mkdir(parents=True, exist_ok=True)
        try:
            return watch(cmd, out, a.cap)
        except CouldNotRun as e:
            say(f"could not run: {e}")
            return 2
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__.split("\n\n", 2)[2])
    ap.add_argument("--scenario", default="200v200", help="a scenario of scenarios/ (200v200)")
    ap.add_argument("-n", type=int, default=None,
                    help="games at once (default: as many as the GPU budget holds), cut to the budget")
    ap.add_argument("--play", type=int, default=180, help="seconds of battle once every game is in")
    ap.add_argument("--out", type=Path, default=None,
                    help="where the samples, logs and report go (default: .data/vramproof/<time>)")
    args = ap.parse_args(argv)
    out = args.out or TREE / ".data" / "vramproof" / time.strftime("%Y%m%d-%H%M%S")
    out.mkdir(parents=True, exist_ok=True)
    try:
        return play(args, out)
    except CouldNotRun as e:
        say(f"could not run: {e}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
