#!/usr/bin/env python3
"""calib.py -- the calibration runner: engine references for tasim, and the offline check against them.

    calib.py engine --scenarios 'scenarios/calib/duel-*.json' [--runs N] [--j J]
                    [--mode reload|fresh] [--out DIR] [--dll PATH] [--runs-per-launch N]
    calib.py engine --probe-pause                 the pause's units clause, timed from a recording
    calib.py refs build RUNS_DIR [--j N] [--keep-recordings]
    calib.py check [--tasim PATH] [--only GLOB] [--require-fresh] [--json OUT]
    calib.py selftest [--only GLOB] [--against REF]
    calib.py stale [--since REV] [--json] [--catalogue FILE]

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


def arena_check(cfg):
    """The configured arena re-derived from the install (arenascan.py check): its meteor shower
    off by the engine's rule in every schema, its flat rectangle and layout as the TNT gives
    them. Without an install the lanes' own launch check of the switch still stands."""
    import arenascan
    try:
        vfs = arenascan.install(None)
    except Exception as e:                                  # noqa: BLE001 -- any install trouble
        return f"unchecked against the install: {e}"
    return arenascan.check(cfg, vfs)


def laid_out(cfg, paths):
    """CalibError unless the tracked placed scenarios hold their places on this arena and every
    scenario a lane will play -- these, and the lane's own end -- stays on its flat rectangle."""
    import arenascan
    import engrun
    arenascan.check_placed(cfg)
    for p in list(paths) + [engrun.END]:
        calibref.check_positions(json.loads(Path(p).read_text()), cfg, Path(p).name)


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
            f"the step that ended at GameTime {last[0]} (its iteration label), "
            f"{run['t_end'] - last[0]} tick(s) before the pause")


def cmd_engine(a):
    import runlock
    import engrun
    import tareplay as R
    try:
        cfg = calibref.arena()
        print(f"arena {cfg['map']}: {arena_check(cfg)}")
        dll = the_dll(a.dll)
        paths = [PROBE] if a.probe_pause else scenario_paths(a.scenarios or [])
        if not paths:
            raise calibref.CalibError("no scenario: --scenarios GLOB or --probe-pause")
        laid_out(cfg, paths)
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
        print(f"{d.name}: played by {head['dll']['build']}, {head['lane_seconds']} s, "
              f"{head['ticks']} ticks, {head['recording_bytes']} recording bytes")
    if a.probe_pause and dirs and not errors:
        print(probe_verdict(dirs[0]))
    return 2 if errors or not dirs else 0


def _lane_job(args):
    """One lane-launch traced, checked and measured: a pool's job, so it returns plain data. The
    traces of exact runs are kept, or of every run with `all_traces` (fake_tasim.py's table)."""
    import time
    import engtrace
    import metrics
    lane_dir, arena_cfg, R, *rest = args
    all_traces = bool(rest and rest[0])
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
               "failures": r.fields, "exogenous": r.exogenous, "block": r.block}
        if r.failed is None:
            try:
                res["metrics"] = metrics.compute(lines, r.block, ln.values, R)
            except metrics.TraceError as e:
                res["start_check"] = f"metrics: {e}"
        if all_traces or "exact" in r.block["kind"]:
            if getattr(r, "sight", None) is not None:
                lines[0]["sight"] = r.sight
            res["trace"] = lines
        out.append(res)
    return {"lane": str(lane_dir), "head": ln.head, "values": ln.values,
            "catalogue": ln.catalogue, "runs": out, "ticks": ticks,
            "seconds": time.monotonic() - t0}


def failure_tally(bad, n_runs):
    """The failed runs of one scenario as a count per check and, under it, per field, each with
    its first run: the whole picture, however many runs failed."""
    from collections import Counter
    checks, fields, first = Counter(), Counter(), {}
    for r in bad:
        got = r.get("failures") or {r["start_check"].split(":")[0]: [r["start_check"]]}
        for name, what in got.items():
            checks[name] += 1
            for f in sorted(set(what)):
                fields[name, f] += 1
                first.setdefault((name, f), r["row"]["index"])
    out = [f"{len(bad)} of {n_runs} runs failed a start check"]
    for name in sorted(checks):
        out.append(f"  {name}: {checks[name]} runs")
        for (c, f), n in sorted(fields.items(), key=lambda kv: (-kv[1], kv[0][1])):
            if c == name:
                out.append(f"    {n:4d}  {f} (first: run {first[c, f]})")
    return "\n".join(out)


def indices_problem(block: dict, indices: list):
    """Why a scenario's run indices cannot make a reference, or None. An exact scenario's are
    exactly 0 and 1 (reload) and 2 (fresh, engrun.jobs_of); another's are exactly 0..n-1 with n at
    least the block's `runs`, so a lane that lost runs, or a run played twice, is refused rather
    than graded as a smaller or a lopsided set (the swap alternates with the index)."""
    if "exact" in block["kind"]:
        want = [0, 1, 2]
    else:
        want = list(range(max(len(indices), block["runs"] or 0)))
    if indices == want:
        return None
    got, need = set(indices), set(want)
    parts = []
    if need - got:
        miss = sorted(need - got)
        parts.append(f"{len(miss)} missing ({miss[:10]}{' ...' if len(miss) > 10 else ''})")
    if len(got) != len(indices):
        parts.append("an index appears twice")
    if got - need:
        parts.append(f"unexpected {sorted(got - need)[:10]}")
    return f"run indices are not exactly {want[0]}..{want[-1]}: " + ", ".join(parts)


def lane_jobs(runs_dir: Path, j: int, all_traces: bool = False):
    """Every lane-launch under runs_dir through _lane_job in a pool of j: (the lanes, the jobs)."""
    from multiprocessing import Pool
    lanes = sorted(p.parent for p in runs_dir.glob("lane*/manifest.jsonl"))
    if not lanes:
        raise calibref.CalibError(f"no lane manifest under {runs_dir}")
    arena_cfg, R = calibref.arena(), calibref.exchange_rate()
    with Pool(max(1, min(j, len(lanes)))) as pool:
        return lanes, pool.map(_lane_job, [(d, arena_cfg, R, all_traces) for d in lanes])


def build_refs(runs_dir: Path, j: int, keep: bool):
    """Every lane-launch under runs_dir streamed once (a pool, no lock), checked R1..R8, and its
    runs merged by scenario into references. Returns (written, failed: {scenario: why})."""
    import datetime
    import hashlib
    import engrun
    R = calibref.exchange_rate()
    lanes, got = lane_jobs(runs_dir, j)
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
        bad = [r for _g, r in items if r["start_check"] != "ok"]
        if bad:
            failed[sid] = failure_tally(bad, len(items))
            continue
        g0 = items[0][0]
        builds = {g["head"]["dll"]["build"] for g, _r in items}
        shas = {g["head"]["dll"]["sha256"] for g, _r in items}
        if len(builds) != 1 or len(shas) != 1:
            failed[sid] = f"runs from more than one DLL ({sorted(builds)})"
            continue
        try:
            engine = calibref.engine_label(next(iter(builds)), next(iter(shas)))
        except calibref.CalibError as e:
            failed[sid] = str(e)
            continue
        commit = engine["commit"]
        if not engine["head_matches"]:
            print(f"{sid}: played by {engine['build']}, the tree's HEAD is {engine['head'][:12]}: "
                  f"the reference names the DLL's commit")
        block = items[0][1]["block"]
        indices = sorted(r["row"]["index"] for _g, r in items)
        why = indices_problem(block, indices)
        if why:
            failed[sid] = why
            continue
        types = sorted({u["type"].upper() for u in perturb.compile_scenario(path)[1]["units"]})
        key = g0["head"]["setup_key"]
        ref = {"schema": calibref.SCHEMA_REF,
               "scenario": {"id": sid, "file": scn, "sha256": calibref.sha256_file(path),
                            "calib": block},
               "engine": {**engine,
                          "install": calibref.install_fingerprint(g0["catalogue"], types),
                          "mod": "retail"},
               "setup": {"map": key[0], "los": key[1], "mapping": key[2],
                         "commander_death": key[6] if len(key) > 6 else None,
                         "game_speed": key[4],
                         "turbo_k": 1, "unit_limit": key[3], "apply_phase": 1,
                         "s_min": engrun.S_MIN},
               "values": {"R": R, "types": {t: g0["values"][t] for t in types}},
               "perturb": {"module_sha256": perturb_sha},
               "shuffle": calibref.shuffle_of(calibref.sha256_file(path)),
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


def cmd_check(a):
    import check
    try:
        rows, errors = check.check(check.env_tasim(a.tasim), a.only, a.require_fresh, a.j)
    except (check.CouldNotCheck, calibref.CalibError) as e:
        print(f"calib check: could not run: {e}")
        return 2
    print(check.report_text(rows))
    for e in errors:
        print(f"could not run: {e}")
    if a.json:
        Path(a.json).write_text(json.dumps(check.score_json(rows), indent=1) + "\n")
    for r in rows:
        if r["verdict"] == "out":
            print(f"OUT: {r['id']} {r['metric']} margin {check.fmt_margin(r)}")
    return check.exit_code(rows, errors)


def cmd_selftest(a):
    import check
    try:
        lines, rates, errors = check.selftest(a.only, a.against)
    except (check.CouldNotCheck, calibref.CalibError) as e:
        print(f"calib selftest: could not run: {e}")
        return 2
    print("\n".join(lines))
    for rule, (rate, n) in rates.items():
        if n:
            print(f"false-alarm rate, {rule}: {rate:.3f} of {n}")
    for e in errors:
        print(f"could not run: {e}")
    return 2 if errors else 0


def cmd_stale(a):
    import check
    try:
        refs = check.load_refs()
        cat = json.loads(Path(a.catalogue).read_text()) if a.catalogue else None
        stale = {}
        for sid, (_p, ref) in sorted(refs.items()):
            why = check.stale_reasons(ref, since=a.since, catalogue=cat)
            if why:
                stale[sid] = why
    except (check.CouldNotCheck, calibref.CalibError, OSError) as e:
        print(json.dumps({"error": str(e)}) if a.json else f"calib stale: could not run: {e}")
        return 2
    except Exception as e:                                   # noqa: BLE001 -- tareplay's git
        if type(e).__name__ != "CouldNotRun":
            raise
        print(json.dumps({"error": str(e)}) if a.json else f"calib stale: could not run: {e}")
        return 2
    if a.json:
        print(json.dumps({"references": len(refs), "stale": stale,
                          "install": "checked" if cat else "unchecked"}))
    else:
        for sid, why in stale.items():
            print(f"{sid}: stale: {'; '.join(why)}")
        print(f"{len(stale)} of {len(refs)} references stale"
              + ("" if cat else " (the install's fingerprint unchecked: --catalogue)"))
    return 0


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
    c = sub.add_parser("check", help="grade tasim against the references")
    c.add_argument("--tasim", help="the tasim_run binary (default $TASIM_RUN)")
    c.add_argument("--only", help="a glob on the scenario file")
    c.add_argument("--require-fresh", action="store_true")
    c.add_argument("--json", help="write the ta-calib-score/1 scoreboard here")
    c.add_argument("--j", type=int, default=8)
    c.set_defaults(func=cmd_check)
    st = sub.add_parser("selftest",
                        help="false alarms over splits, seat effect, exact self-consistency")
    st.add_argument("--only")
    st.add_argument("--against", help="a reference to grade against (reload against fresh)")
    st.set_defaults(func=cmd_selftest)
    sl = sub.add_parser("stale", help="the references the code made stale")
    sl.add_argument("--since", help="count only what changed since this revision")
    sl.add_argument("--json", action="store_true")
    sl.add_argument("--catalogue", help="a live `tacli units --json` to check the install against")
    sl.set_defaults(func=cmd_stale)
    a = p.parse_args(argv)
    # The commands read recordings and zstd-compressed traces, which need the shared venv:
    # devpy re-executes under it, or exits 2 naming it. The guard below catches a module devpy
    # does not test for.
    import devpy
    devpy.ensure()
    try:
        return a.func(a)
    except ModuleNotFoundError as e:
        print(f"calib.py: could not run: the module {e.name} is missing; run the tools with "
              f"{devpy.venv_python() or devpy.VENV_HINT}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
