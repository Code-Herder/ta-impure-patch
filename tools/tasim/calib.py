#!/usr/bin/env python3
"""calib.py -- the calibration runner: engine references for tasim, and the offline check against them.

    calib.py engine --scenarios 'scenarios/calib/duel-*.json' [--runs N] [--j J]
                    [--mode reload|fresh] [--out DIR] [--dll PATH] [--runs-per-launch N]
    calib.py engine --probe-pause                 the pause's units clause, timed from a recording
    calib.py refs build RUNS_DIR [--j N] [--keep-recordings]

What it measures and why each piece is the way it is: research/notes/modules/tasim-calibration.md,
and research/notes/impai/calibration.md for the rules. The engine phase holds the machine's run lock
(tools/runlock.py) for its whole life and plays its lanes in parallel under it; nothing else here
plays a game. Exit 0 when it ran, 1 when a check failed, 2 when it could not run.
"""

import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(TREE / "tools"))
sys.path.insert(0, str(TREE / "tools" / "replay"))
import calibref  # noqa: E402
import perturb  # noqa: E402

PROBE = TREE / "scenarios" / "calib" / "probe-pause.json"


def scenario_paths(globs):
    out = []
    for g in globs:
        hits = sorted(TREE.glob(g)) if not Path(g).is_absolute() else sorted(Path("/").glob(g[1:]))
        if not hits:
            raise calibref.CalibError(f"no scenario matches {g!r}")
        out += hits
    return list(dict.fromkeys(p.resolve() for p in out))


def meteor_check(cfg):
    """The arena's .ota names no meteor weapon (a shower draws the CRT's rand, which the draw path
    shares, so no run could be independent of the frame rate). Checked against the install when
    one is found; the arena config's own assertion stands otherwise."""
    sys.path.insert(0, str(TREE / "tools" / "wiki"))
    import re
    import tavfs
    try:
        vfs = tavfs.Vfs(tavfs.find_install(None))
    except Exception as e:                                  # noqa: BLE001 -- any install trouble
        return f"meteors unchecked: {e}"
    got = vfs.open(f"maps/{cfg['map'].lower()}.ota")
    if not got or not got[0]:
        raise calibref.CalibError(f"the arena map {cfg['map']!r} has no .ota in the install")
    m = re.search(r"meteorweapon\s*=\s*([^;]*);", got[0].decode("latin-1"), re.I)
    if m and m.group(1).strip():
        raise calibref.CalibError(f"{cfg['map']}: MeteorWeapon={m.group(1).strip()}")
    return "no meteor weapon"


def the_dll(given):
    import tareplay as R
    dll, make = R.build_dll(False, False)
    dll = Path(given).resolve() if given else dll.resolve()
    if not dll.exists():
        raise R.CouldNotRun(f"no DLL at {dll} ({make})")
    if not given and dll.stat().st_mtime < R.sources_mtime():
        raise R.CouldNotRun(f"{dll} is older than its sources ({make})")
    return dll


def probe_verdict(lane_dir: Path):
    """The units clause's tick against the recording: the iteration label of the last death of a
    unit the clear did not take, and the GameTime the pause landed at."""
    import tarec
    head = json.loads((lane_dir / "manifest.jsonl").read_text().splitlines()[0])
    run = next(json.loads(ln) for ln in (lane_dir / "manifest.jsonl").read_text().splitlines()
               if '"index"' in ln)
    cleared = {s + 1 for s in run["clear"]["cleared_slots"]}
    h, bl, _stop = tarec.load(lane_dir / head["recording"], exact=True)
    last = [None]

    def on_record(w, kind, b):
        if kind == tarec.RK_EV_DEATH and w.tick is not None and run["t_apply"] <= w.tick:
            unit = int.from_bytes(b[:4], "little")
            if unit not in cleared and w.tick <= run["t_end"]:
                last[0] = w.tick
    tarec.stream_to(h, bl, run["t_end"] + 2, on_record=on_record)
    if last[0] is None:
        return f"probe: paused at {run['t_end']} ({run['end']}), no combat death in the recording"
    return (f"probe: paused at GameTime {run['t_end']} ({run['end']}); the last combat death is in "
            f"iteration {last[0]}, {run['t_end'] - last[0]} tick(s) before the pause")


def cmd_engine(a):
    import devpy
    devpy.ensure()
    import runlock
    import engrun
    import tareplay as R
    try:
        cfg = calibref.arena()
        print(f"arena {cfg['map']}: {meteor_check(cfg)}")
        build = calibref.git_build()
        if build.endswith("-dirty"):
            raise calibref.CalibError(f"the tree is dirty ({build}): a reference names its commit")
        dll = the_dll(a.dll)
        paths = [PROBE] if a.probe_pause else scenario_paths(a.scenarios or [])
        if not paths:
            raise calibref.CalibError("no scenario: --scenarios GLOB or --probe-pause")
    except (calibref.CalibError, R.CouldNotRun) as e:
        print(f"calib engine: could not run: {e}")
        return 2
    runlock.hold(TREE, "calib engine " + " ".join(sys.argv[2:]))
    out = engrun.new_out(Path(a.out) if a.out else None)
    print(f"calib engine: {len(paths)} scenario(s), j={a.j}, mode {a.mode}, out {out}")
    dirs, errors = engrun.run_engine(paths, 1 if a.probe_pause else a.runs, a.j, a.mode, out,
                                     dll, a.runs_per_launch)
    for e in errors:
        print(f"could not run: {e}")
    for d in dirs:
        head = json.loads((d / "manifest.jsonl").read_text().splitlines()[0])
        print(f"{d.name}: {head['lane_seconds']} s, {head['ticks']} ticks, "
              f"{head['recording_bytes']} recording bytes")
    if a.probe_pause and dirs and not errors:
        print(probe_verdict(dirs[0]))
    return 2 if errors or not dirs else 0


def _lane_job(args):
    """One lane-launch traced, checked and measured: a pool's job, so it returns plain data."""
    import time
    import engtrace
    import metrics
    lane_dir, arena_cfg, R = args
    t0 = time.monotonic()
    try:
        ln, runs = engtrace.lane(lane_dir, arena_cfg)
    except (calibref.CalibError, OSError) as e:
        return {"lane": str(lane_dir), "error": str(e)}
    out = []
    ticks = 0
    for r in runs:
        lines = r.trace()
        ticks += r.t_end - r.t0
        res = {"row": r.row, "start_check": r.failed or "ok", "checks": r.checks,
               "exogenous": r.exogenous, "block": r.block}
        if r.failed is None:
            try:
                res["metrics"] = metrics.compute(lines, r.block, ln.values, R)
            except metrics.TraceError as e:
                res["start_check"] = f"metrics: {e}"
        if "exact" in r.block["kind"]:
            if getattr(r, "sight", None) is not None:
                lines[0]["sight"] = r.sight
            res["trace"] = lines
        out.append(res)
    return {"lane": str(lane_dir), "head": ln.head, "values": ln.values,
            "catalogue": ln.catalogue, "runs": out, "ticks": ticks,
            "seconds": time.monotonic() - t0}


def build_refs(runs_dir: Path, j: int, keep: bool):
    """Every lane-launch under runs_dir streamed once (a pool, no lock), checked R1..R8, and its
    runs merged by scenario into references. Returns (written, failed: {scenario: why})."""
    import datetime
    import hashlib
    from multiprocessing import Pool
    import engrun
    arena_cfg = calibref.arena()
    R = calibref.exchange_rate()
    lanes = sorted(p.parent for p in runs_dir.glob("lane*/manifest.jsonl"))
    if not lanes:
        raise calibref.CalibError(f"no lane manifest under {runs_dir}")
    with Pool(max(1, min(j, len(lanes)))) as pool:
        got = pool.map(_lane_job, [(d, arena_cfg, R) for d in lanes])
    failed, by = {}, {}
    for g in got:
        if "error" in g:
            failed[g["lane"]] = g["error"]
            continue
        print(f"{Path(g['lane']).name}: {g['ticks']} ticks in {g['seconds']:.1f} s "
              f"({g['ticks'] / max(g['seconds'], 1e-9):.0f} ticks/s)")
        for r in g["runs"]:
            by.setdefault(r["row"]["scenario"], []).append((g, r))
    perturb_sha = calibref.sha256_file(HERE / "perturb.py")
    written = []
    for scn, items in sorted(by.items()):
        path = TREE / scn
        sid = calibref.scenario_id(path)
        bad = [f"run {r['row']['index']}: {r['start_check']}" for _g, r in items
               if r["start_check"] != "ok"]
        if bad:
            failed[sid] = "; ".join(bad[:5])
            continue
        g0 = items[0][0]
        builds = {g["head"]["dll"]["build"] for g, _r in items}
        shas = {g["head"]["dll"]["sha256"] for g, _r in items}
        if len(builds) != 1 or len(shas) != 1:
            failed[sid] = f"runs from more than one DLL ({sorted(builds)})"
            continue
        try:
            commit = calibref.resolve_commit(next(iter(builds)))
        except calibref.CalibError as e:
            failed[sid] = str(e)
            continue
        block = items[0][1]["block"]
        indices = sorted(r["row"]["index"] for _g, r in items)
        if len(set(indices)) != len(indices):
            failed[sid] = f"a run index appears twice: {indices}"
            continue
        types = sorted({u["type"].upper() for u in perturb.compile_scenario(path)[1]["units"]})
        key = g0["head"]["setup_key"]
        ref = {"schema": calibref.SCHEMA_REF,
               "scenario": {"id": sid, "file": scn, "sha256": calibref.sha256_file(path),
                            "calib": block},
               "engine": {"build": g0["head"]["dll"]["build"], "commit": commit,
                          "dll_sha256": g0["head"]["dll"]["sha256"],
                          "install": calibref.install_fingerprint(g0["catalogue"], types),
                          "mod": "retail"},
               "setup": {"map": key[0], "los": key[1], "mapping": key[2], "game_speed": key[4],
                         "turbo_k": 1, "unit_limit": key[3], "apply_phase": 1,
                         "s_min": engrun.S_MIN},
               "values": {"R": R, "types": {t: g0["values"][t] for t in types}},
               "perturb": {"module_sha256": perturb_sha},
               "runs": [], "selftest": {},
               "cost": {"lane_seconds": 0.0, "ticks": 0, "runs": len(items)},
               "created": datetime.date.today().isoformat()}
        traces = []
        for g, r in sorted(items, key=lambda it: it[1]["row"]["index"]):
            row = r["row"]
            ref["runs"].append({
                "index": row["index"], "seed": row["seed"], "swap": row["swap"],
                "mode": row["mode"], "lane": g["head"]["lane"], "recording": g["head"]["recording"],
                "t_apply": row["t_apply"], "t_end": row["t_end"], "end": row["end"],
                "wire_sha256": row["wire_sha256"], "perturbation": row["perturbation"],
                "start_check": r["start_check"], "checks": r["checks"],
                "metrics": r.get("metrics", {}), "exogenous": r["exogenous"],
                "wall": row.get("wall")})
            ref["cost"]["ticks"] += row["t_end"] - row["t_apply"]
            ref["cost"]["lane_seconds"] += (row.get("wall") or {}).get("total", 0.0)
            if "trace" in r:
                traces.extend(r["trace"])
        out = calibref.ref_path(sid, commit)
        out.parent.mkdir(parents=True, exist_ok=True)
        if traces:
            import zstandard
            text = "".join(json.dumps(ln) + "\n" for ln in traces)
            calibref.traces_path(sid, commit).write_bytes(
                zstandard.ZstdCompressor(level=10).compress(text.encode()))
            ref["traces_sha256"] = hashlib.sha256(text.encode()).hexdigest()
        out.write_text(json.dumps(ref, indent=1) + "\n")
        written.append(out)
    if not keep and not failed:
        engrun.remove_recordings(lanes)
    return written, failed


def cmd_refs(a):
    import devpy
    devpy.ensure()
    try:
        written, failed = build_refs(Path(a.runs_dir), a.j, a.keep_recordings)
    except calibref.CalibError as e:
        print(f"calib refs build: could not run: {e}")
        return 2
    for p in written:
        print(f"wrote {p}")
    for k, why in failed.items():
        print(f"could not build {k}: {why}")
    return 2 if failed else 0


def main(argv=None):
    import engrun
    p = argparse.ArgumentParser(prog="calib.py", description=__doc__.strip().splitlines()[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("engine", help="play scenarios in the engine; write lane manifests")
    e.add_argument("--scenarios", action="append", help="a glob under the tree (repeatable)")
    e.add_argument("--runs", type=int, help="runs a scenario (default: its calib block's)")
    e.add_argument("--j", type=int, default=engrun.J_DEFAULT, help="lanes, at most 16")
    e.add_argument("--mode", choices=("reload", "fresh"), default="reload")
    e.add_argument("--out", help="the runs folder (default: <calib dir>/runs/<stamp>)")
    e.add_argument("--dll", help="the DLL (default: this tree's build, current)")
    e.add_argument("--runs-per-launch", type=int, default=engrun.RUNS_PER_LAUNCH)
    e.add_argument("--probe-pause", action="store_true",
                   help="one duel ended by the units clause, and its tick from the recording")
    e.set_defaults(func=cmd_engine)
    r = sub.add_parser("refs", help="build references from an engine run's lane manifests")
    rs = r.add_subparsers(dest="refs_cmd", required=True)
    rb = rs.add_parser("build", help="stream each lane's recording once, check R1..R8, merge")
    rb.add_argument("runs_dir")
    rb.add_argument("--j", type=int, default=8, help="processes")
    rb.add_argument("--keep-recordings", action="store_true")
    rb.set_defaults(func=cmd_refs)
    a = p.parse_args(argv)
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
