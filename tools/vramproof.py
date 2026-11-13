#!/usr/bin/env python3
"""The run lock's GPU bound, shown: N harness games of one heavy scenario at once, each game's
device memory sampled from nvidia-smi every second, judged against the figures the tokens are
sized on (the lock's runslots/vram.cfg, seeded from tools/runlock-vram.cfg).

    vramproof.py [--scenario 200v200] [-n N] [--play S] [--out DIR]

THE CLAIM. A game launched inside a run-lock hold runs under the GPU memory cap (`gpumem=`,
tools/tacli write_gpumem): the DLL charges every device-local allocation against it and refuses one
past it (inc/tagpu_vkcap.h), so the game's own allocations never sum past cap_mib, and with what
the driver holds beside them it stays within B = cap_mib + driver_mib (runlock.game_bound_mib). A
token is B plus a margin, so the games the tokens admit together stay within the device's memory
less reserve_mib. This plays the case that tests it hardest: the play defaults (`--defaults`: ss 2,
the restorer, shadowres 2048) on a battle scenario, as many games at once as the tokens allow.

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

Holds the run lock for N lanes (default: every GPU token there is), so it waits for the machine
like any other run. Each game has its own Xvfb and its own instance, `vramproof-<tag>-<k>`, made
afresh and removed at the end; the samples, the games' logs and report.json go to --out.
Exit 0 when every line holds, 1 when one does not, 2 when the run could not be made.
"""
import argparse
import json
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


def game_pids(names):
    """{pid: instance} of every TotalA.exe process of the instances `names`, by the Wine prefix
    its environment names."""
    prefixes = {str(INSTANCES / n / "prefix"): n for n in names}
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
                name = prefixes.get(e[len(b"WINEPREFIX="):].decode(errors="replace").rstrip("/"))
                if name:
                    found[int(d.name)] = name
    return found


class Sampler(threading.Thread):
    """One sample a second until stopped: the time, the device's use and each game's MiB, by
    instance (the most of its processes, which is the game's one process on the GPU)."""

    def __init__(self, names, path):
        super().__init__(daemon=True)
        self.names, self.path = names, path
        self.samples, self.error = [], None
        self.halt = threading.Event()

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
                games = {}
                for pid, name in game_pids(self.names).items():
                    if pid in procs:
                        games[name] = max(games.get(name, 0), procs[pid])
                s = {"t": t, "used": used, "total": total, "games": games}
                self.samples.append(s)
                out.write(json.dumps(s) + "\n")
                out.flush()
                self.halt.wait(max(0.0, SAMPLE_S - (time.time() - t)))


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
    budget = runlock.vram_tokens(lock)
    if not budget:
        raise CouldNotRun("no GPU budget here: no device memory reads, so no bound to show")
    lanes = args.n or budget
    runlock.hold(TREE, f"vramproof {args.scenario} x{lanes}", lanes)
    n = runlock.games()
    # the figures judged are the lock's (runslots/vram.cfg), the ones the hold was admitted under
    cfg = runlock.vram_config(lock)
    budget = runlock.vram_tokens(lock, cfg)
    if cfg["cap_mib"] != runlock.held_cap_mib():
        raise CouldNotRun(f"the lock's cap moved from {runlock.held_cap_mib()} to {cfg['cap_mib']} MiB "
                          f"while this run was admitted")
    say(f"{n} games of {args.scenario} at once ({budget} GPU tokens of {runlock.game_mib(cfg)} MiB; "
        f"the cap {cfg['cap_mib']} MiB, the bound {runlock.game_bound_mib(cfg)} MiB)")
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
                          "--private-registry", timeout=300)
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
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__.split("\n\n", 2)[2])
    ap.add_argument("--scenario", default="200v200", help="a scenario of scenarios/ (200v200)")
    ap.add_argument("-n", type=int, default=None,
                    help="games at once (default: every GPU token there is), cut to the tokens")
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
