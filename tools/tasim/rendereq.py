#!/usr/bin/env python3
"""Whether the renderer changes the simulation: the same calibration runs played with GDI and on
the default (Vulkan) renderer, compared tick by tick.

    rendereq.py [--scenarios GLOB] [--dll PATH] [--out DIR] [--j N] [--pacing nodraw]
    rendereq.py --compare GDI_RUN VULKAN_RUN     the comparison alone, of two engine runs' folders
    rendereq.py --pacing nodraw --compare DRAWN_RUN NODRAW_RUN

WHY IT IS A QUESTION. Under the Vulkan backend the game thread skips engine draws that GDI runs
(tagpu_owndraw.c, tagpu_fxown.c, tagpu_featown.c, tagpu_terrown.c, each armed by the render
thread's own pass). That those draws write no simulation state is argued site by site, not
checked; the one consumer named so far is the CRT's `rand` 0x4E4870, whose stream is per thread
and in neither recorded image (engine/sim.md, *The two random streams*): a draw the Vulkan lane
skips leaves the game thread's CRT stream elsewhere, which changes the simulation only if a
simulation step draws from that stream too.

WHAT IS COMPARED. The simulation RNG 0x51FC88 is seeded once a level from QueryPerformanceCounter
(the loader 0x497180, engine/sim.md), so two launches never share it, and a run that draws from it
differs from launch to launch whatever the renderer. The runs compared are therefore the ones whose
course draws nothing from it: the EXACT calibration scenarios, whose reload and fresh runs must
already agree (tasim-calibration.md). Each is played by `calib.py engine` twice, once with GDI
and once with --gpu, the same DLL, the same scenario files and so the same wires (perturb.run_wire
is a function of the scenario and the run index alone). For every run both renderers played, each
recording is streamed once (tarec.stream_to) and two things are set side by side at every tick from
its apply to its end, numbered from the apply:
  * the run's calibration trace (engtrace: each unit's row, its ledger, each side's pass fields and
    the events) -- what a reference is made of. This is the verdict: equal at every tick, or the
    first tick and line that differs;
  * a digest of every live unit's whole recorded image, every player record and the live part of
    the projectile pool. Reported, not judged: a unit image carries heap pointers the two processes
    lay out differently, so its bytes may differ with the simulation equal. The first tick each
    image digest differs at is printed with the differing unit offsets, for a reader to tell a
    pointer from a field.

THE PACING (`--pacing nodraw`). The same comparison of GDI against GDI: one engine run with `--draw`,
every frame drawn, and one under NO DRAW (engrun.py, THE PACING), whose turbo frames skip the game
screen's draw. The claim it tests is the one tagpu_replay.c's nodraw_gate rests on: between two batches
the draw and its observers write nothing the simulation reads, so the same runs at the same k agree at
every tick. Neither run takes a GPU token.

Each `calib.py engine` takes the run lock itself; the --gpu one takes GPU tokens. Exit 0 when every
run's trace is equal, 1 when one differs, 2 when a run could not be made or a run is missing from
one side.
"""
import argparse
import hashlib
import json
import struct
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(TREE / "tools" / "replay"))
import calibref  # noqa: E402

CALIB = HERE / "calib.py"
SCENARIOS = "scenarios/calib/exact-*.json"
RENDERERS = ("GDI", "Vulkan")           # the two sides' names, by mode
PACINGS = ("drawn", "no-draw")


def say(text):
    print(f"rendereq: {text}", flush=True)


def engine_run(out: Path, scenarios: str, dll, j: int, extra: list) -> Path:
    """One `calib.py engine` with the arguments `extra` (--gpu, --draw) into a fresh folder under `out`;
    the folder it made."""
    out.mkdir(parents=True, exist_ok=True)
    before = {p for p in out.iterdir() if p.is_dir()}
    cmd = [sys.executable, str(CALIB), "engine", "--scenarios", scenarios, "--j", str(j),
           "--out", str(out)] + (["--dll", str(dll)] if dll else []) + list(extra)
    say(" ".join(cmd))
    rc = subprocess.run(cmd).returncode
    made = sorted({p for p in out.iterdir() if p.is_dir()} - before)
    if rc or len(made) != 1:
        raise RuntimeError(f"calib engine {' '.join(extra)} exited {rc}, made {len(made)} folders")
    return made[0]


def comparable(trace: list) -> list:
    """A run's trace without what names its place in its launch: the head's absolute GameTime."""
    head = dict(trace[0])
    head.pop("t0", None)
    return [head] + trace[1:]


def traces(run_dir: Path, j: int) -> dict:
    """{(scenario, index): (comparable trace, start check)} of every finished lane-launch."""
    import calib
    _lanes, got = calib.lane_jobs(run_dir, j, all_traces=True)
    out = {}
    for g in got:
        if "error" in g:
            raise RuntimeError(f"{g['lane']}: {g['error']}")
        for r in g["runs"]:
            out[r["row"]["scenario"], r["row"]["index"]] = (comparable(r.get("trace") or [{}]),
                                                            r["start_check"])
    return out


def image_digests(run_dir: Path) -> dict:
    """{(scenario, index): {t: (digest, {slot: unit image})}}, t from the run's apply."""
    import engrun
    import tarec
    out = {}
    for d in engrun.finished_lanes(run_dir):
        rows = [json.loads(ln) for ln in (d / "manifest.jsonl").read_text().splitlines() if ln.strip()]
        runs = [r for r in rows[1:] if "index" in r]
        for r in runs:
            out[r["scenario"], r["index"]] = {}
        h, bl, _stop = tarec.load(d / rows[0]["recording"], exact=True)

        def at(w):
            L = w.tick
            for r in runs:
                if r["t_apply"] <= L <= r["t_end"]:
                    units = {s: bytes(img) for s, img in sorted(w.units.items())}
                    n = (w.phead or (0, 0))[0]
                    m = hashlib.sha256()
                    for s, img in units.items():
                        m.update(struct.pack("<I", s) + img)
                    for p in w.players:
                        m.update(bytes(p))
                    m.update(bytes(w.proj[:n * tarec.REC_PROJ_IMAGE]))
                    out[r["scenario"], r["index"]][L - r["t_apply"]] = (m.hexdigest(), units)

        def on_record(w, kind, _b):
            if kind is None and w.tick is not None:
                at(w)
        w = tarec.stream_to(h, bl, None, on_record=on_record)
        if w.tick is not None:
            at(w)
    return out


def first_trace_difference(a: list, b: list):
    """None, or (line number, a's line, b's line) of the first line that differs."""
    for i in range(max(len(a), len(b))):
        la = a[i] if i < len(a) else None
        lb = b[i] if i < len(b) else None
        if la != lb:
            return i, la, lb
    return None


def image_report(a: dict, b: dict, labels=RENDERERS) -> str:
    """The first tick the image digests differ at, and the unit offsets that differ there."""
    for t in sorted(set(a) | set(b)):
        if t not in a or t not in b:
            return f"t {t}: present in {labels[0] if t in a else labels[1]} only"
        if a[t][0] == b[t][0]:
            continue
        ua, ub = a[t][1], b[t][1]
        if set(ua) != set(ub):
            return f"t {t}: other live slots ({sorted(set(ua) ^ set(ub))[:8]})"
        offs = sorted({k for s in ua for k in range(len(ua[s])) if ua[s][k] != ub[s][k]})
        what = (f"unit offsets {', '.join(f'+0x{k:X}' for k in offs[:16])}{' ...' if len(offs) > 16 else ''}"
                if offs else "the player records or the projectile pool")
        return f"first differs at t {t}: {what}"
    return "equal at every tick"


def compare(a_run: Path, b_run: Path, j: int, labels=RENDERERS) -> int:
    """The runs of two engine runs' folders set side by side; `labels` names the two sides, the
    renderers' ("GDI", "Vulkan") or the pacings' ("drawn", "no-draw")."""
    la_, lb_ = labels
    w = max(len(la_), len(lb_))
    ta, tb = traces(a_run, j), traces(b_run, j)
    ia, ib = image_digests(a_run), image_digests(b_run)
    missing = sorted(set(ta) ^ set(tb))
    status = 2 if missing or not ta else 0
    for k in missing:
        print(f"MISSING: {k[0]} run {k[1]}: played by {la_ if k in ta else lb_} only")
    for k in sorted(set(ta) & set(tb)):
        (a, ca), (b, cb) = ta[k], tb[k]
        name = f"{k[0]} run {k[1]}"
        if ca != "ok" or cb != "ok":
            print(f"COULD NOT RUN: {name}: start checks {la_} {ca}, {lb_} {cb}")
            status = 2
            continue
        diff = first_trace_difference(a, b)
        if diff:
            i, la, lb = diff
            print(f"DIFFERS: {name}: trace line {i}\n  {la_:<{w}} {json.dumps(la)[:400]}\n"
                  f"  {lb_:<{w}} {json.dumps(lb)[:400]}")
            status = max(status, 1)
        else:
            print(f"EQUAL: {name}: {len(a) - 1} trace lines")
        print(f"  images: {image_report(ia.get(k, {}), ib.get(k, {}), labels)}")
    what = "the renderers" if labels == RENDERERS else "the pacings"
    print(f"rendereq: {what + ' agree' if status == 0 else 'see above'} ({len(ta)} {la_} runs, "
          f"{len(tb)} {lb_} runs)")
    return status


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--scenarios", default=SCENARIOS, help=f"a glob under the tree (default {SCENARIOS})")
    ap.add_argument("--dll", help="the DLL both play (default this tree's build)")
    ap.add_argument("--out", type=Path, default=None)
    ap.add_argument("--j", type=int, default=2, help="lanes a side, and processes for the streaming")
    ap.add_argument("--pacing", choices=("nodraw",),
                    help="compare GDI drawn (--draw) against GDI under no draw, not GDI against Vulkan")
    ap.add_argument("--compare", nargs=2, type=Path, metavar=("A_RUN", "B_RUN"),
                    help="GDI and Vulkan runs, or with --pacing nodraw the drawn and the no-draw run")
    a = ap.parse_args(argv)
    labels = PACINGS if a.pacing else RENDERERS
    try:
        if a.compare:
            return compare(*a.compare, a.j, labels)
        stamp = time.strftime("%Y%m%d-%H%M%S")
        if a.pacing:
            out = a.out or calibref.calib_dir() / "rendereq-nodraw" / stamp
            first = engine_run(out / "drawn", a.scenarios, a.dll, a.j, ["--draw"])
            second = engine_run(out / "nodraw", a.scenarios, a.dll, a.j, [])
        else:
            out = a.out or calibref.calib_dir() / "rendereq" / stamp
            first = engine_run(out / "gdi", a.scenarios, a.dll, a.j, [])
            second = engine_run(out / "vulkan", a.scenarios, a.dll, a.j, ["--gpu"])
        say(f"compare {first} {second}")
        return compare(first, second, a.j, labels)
    except (RuntimeError, calibref.CalibError, OSError) as e:
        say(f"COULD NOT RUN: {e}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
