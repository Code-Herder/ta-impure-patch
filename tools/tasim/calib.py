#!/usr/bin/env python3
"""calib.py -- the calibration runner: engine references for tasim, and the offline check against them.

    calib.py engine --scenarios 'scenarios/calib/duel-*.json' [--runs N] [--j J]
                    [--mode reload|fresh] [--out DIR | --resume DIR] [--dll PATH]
                    [--runs-per-launch N] [--gpu] [--draw] [--arena FILE] [--setup NAME]
    calib.py engine --probe-pause                the pause's units clause, timed from a recording
    calib.py refs build RUNS_DIR [--j N] [--keep-recordings] [--check-only] [--arena FILE]
    calib.py check [--tasim PATH] [--only GLOB] [--require-fresh] [--json OUT] [--arena FILE]
    calib.py selftest [--only GLOB] [--against REF]
    calib.py stale [--since REV] [--json] [--catalogue FILE]

What it measures and why each piece is the way it is: research/notes/modules/tasim-calibration.md,
and research/notes/impai/calibration.md for the rules. The engine phase holds the machine's run lock
(tools/runlock.py) for its whole life, declaring its lanes, and plays them in parallel under it;
nothing else here plays a game. Its lanes render with GDI and take no GPU token; `--gpu` renders them
on the default renderer for a debugging run, and each manifest names its renderer. A GDI lane runs no
draw while the turbo runs (engrun.py, THE PACING); `--draw` renders every frame, and each manifest
names its pacing. `--resume DIR` plays only the runs DIR's finished lane-launches lack, and refuses
runs another DLL, arena, setup, renderer or pacing played (engrun.py, RESUMING). --arena plays (and builds references from) another arena
than tools/tasim/calib-arena.json, the water arena of the naval fights
(tools/tasim/calib-arena-naval.json): the engine run, its `refs build` and a `check` of its references
name the same file. `refs build` refuses a runs folder outside the calibration root ($TACALIB_DIR, else
<main checkout>/.data/calib), whose references it writes (calibref.runs_in_root). `--setup NAME` plays a compat
setup's own folder (engrun.py, ON A COMPAT SETUP) into its own root, $TACALIB_DIR exactly
<main checkout>/.data/calib-mods/<setup>, from scenarios of that root's generation alone; `refs build` takes the
setup from the lane-launches' heads and every other command from $TACALIB_SETUP, and every command refuses a root
that is not its setup's (calibref.calib_dir, THE ROOT INVARIANT). Exit 0 when it ran, 1 when a check failed or a
resume was refused, 2 when it could not run.
"""

import argparse
import json
import os
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
    """The configured arena checked: a generated map (`synthmap`) by synthmap.check, which builds it and
    asserts its sites, its void and its meteor shower off; the land arena re-derived from the install
    (arenascan.py check): its meteor shower off by the engine's rule in every schema, its flat rectangle
    and layout as the TNT gives them. Without an install the lanes' own launch check of the switch still
    stands, and it is all a water arena gets: arenascan.py grades dry rectangles only."""
    if cfg.get("synthmap"):
        import synthmap
        kind = cfg["synthmap"]
        try:
            line = synthmap.check(kind)
        except ValueError as e:
            raise calibref.CalibError(f"the generated map: {e}")
        return f"a generated map ({kind}, sha256 {synthmap.sha256(kind)[:12]}): {line}"
    if not calibref.default_arena():
        return "a water arena: its rectangle unchecked by arenascan.py, the shower checked at each launch"
    import arenascan
    try:
        vfs = arenascan.install(None)
    except Exception as e:                                  # noqa: BLE001 -- any install trouble
        return f"unchecked against the install: {e}"
    return arenascan.check(cfg, vfs)


def retail_twin(cfg) -> dict:
    """The retail arena a generated arena's fights are authored on: the land arena for the land map, the
    water arena for the naval map. Their files keep the retail coordinates; the packed lane translates them."""
    return calibref.arena(calibref.ARENA if cfg["synthmap"] == "land" else calibref.NAVAL_ARENA)


def laid_out(cfg, paths, facts=None):
    """CalibError unless the tracked placed scenarios hold their places on this arena and every
    scenario a lane will play -- these, and the lane's own end -- stays on its flat rectangle. On a
    generated arena the scenarios are checked against their retail twin's rectangle, every one must pass
    the packed lane's admission (pack.load_fights, with `facts` or the install's own), and the lane end's
    unit must stand on an open cell of the map, not on its void (synthmap.open_cells)."""
    import arenascan
    import engrun
    if cfg.get("synthmap"):
        import pack
        import synthmap
        twin = retail_twin(cfg)
        for p in paths:
            calibref.check_positions(json.loads(Path(p).read_text()), twin, Path(p).name)
        try:
            facts = facts if facts is not None else pack.install_facts(setup=calibref.setup_name())
            _geoms, refused = pack.load_fights(paths, cfg, facts)
        except pack.PackError as e:
            raise calibref.CalibError(f"the packed lane's admission: {e}")
        if refused:
            p, why = sorted(refused.items())[0]
            raise calibref.CalibError(f"{len(refused)} scenario(s) the packed lane refuses, e.g. {Path(p).name}: "
                                      f"{why}")
        if "end" not in cfg:
            raise calibref.CalibError("a generated arena names its lane end's point (`end`): the retail one is void")
        x, z = cfg["end"]["pos"]
        if not synthmap.open_cells(cfg["synthmap"])[(z // 16) * synthmap.CELLS + x // 16]:
            raise calibref.CalibError(f"the lane end's unit at {[x, z]} stands on the map's void")
        return
    if calibref.default_arena():
        arenascan.check_placed(cfg)
    for p in list(paths) + ([] if "end" in cfg else [engrun.END]):
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
        calibref.check_scenario_root(paths)
        laid_out(cfg, paths)
        setup = calibref.setup_record(a.setup) if a.setup else None
    except (calibref.CalibError, R.CouldNotRun) as e:
        print(f"calib engine: could not run: {e}")
        return 2
    runs = 1 if a.probe_pause else a.runs
    done, launches = set(), 0
    if a.resume:
        out = Path(a.resume)
        try:
            if a.out:
                raise engrun.ResumeRefused("--resume continues its own folder; --out names a new one")
            if not out.is_dir():
                raise engrun.ResumeRefused(f"no engine run at {out}")
            done, launches, replayed = engrun.done_runs(out, paths, runs, a.mode, cfg, dll, a.gpu, a.draw)
        except engrun.ResumeRefused as e:
            print(f"calib engine: the resume is refused:\n{e}")
            return 1
        print(f"calib engine: resuming {out}: {len(done)} runs kept, {replayed} runs of unfinished "
              f"lane-launches played again")
    jobs = engrun.jobs_of(paths, runs, a.mode, cfg, done)
    if not jobs:
        print("calib engine: no run left to play" + (f": every run is already in {out}" if a.resume else ""))
        return 0
    runlock.hold(TREE, "calib engine " + " ".join(sys.argv[2:]), engrun.lane_count(jobs, a.j),
                 gpu=a.gpu)
    a.j = min(a.j, runlock.games())     # no more lanes than the run lock's games
    if not a.resume:
        out = engrun.new_out(Path(a.out) if a.out else None)
    print(f"calib engine: {len(paths)} scenario(s), j={a.j}, mode {a.mode}, "
          f"renderer {engrun.renderer(a.gpu)}, pacing {engrun.pacing(a.gpu, a.draw)}, "
          f"setup {a.setup or 'retail'}, out {out}")
    dirs, errors = engrun.run_engine(paths, runs, a.j, a.mode, out,
                                     dll, a.runs_per_launch, done, a.gpu, launches, setup, a.draw)
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
    """Every finished lane-launch under runs_dir (engrun.finished_lanes, the ones a resume counts)
    through _lane_job in a pool of at most j processes, each a slot of the run lock, and no more
    than it grants (tools/runlock.py, OFFLINE WORK): (the lanes, the jobs)."""
    from multiprocessing import Pool
    import engrun
    import runlock
    lanes = engrun.finished_lanes(runs_dir)
    if not lanes:
        raise calibref.CalibError(f"no lane manifest under {runs_dir}")
    arena_cfg, R = calibref.arena(), calibref.exchange_rate()
    with runlock.offline(TREE, f"calib trace read {runs_dir.name}", min(j, len(lanes))) as phase, \
            Pool(phase.n) as pool:
        return lanes, pool.map(_lane_job, [(d, arena_cfg, R, all_traces) for d in lanes])


def manifest_rows(lanes) -> list:
    """(the lane folder, its head, every other row) of each finished lane-launch."""
    out = []
    for d in lanes:
        rows = [json.loads(ln) for ln in (Path(d) / "manifest.jsonl").read_text().splitlines() if ln.strip()]
        out.append((Path(d), rows[0], rows[1:]))
    return out


def cost_by_kind(lanes) -> dict:
    """{kind: (runs, lane-seconds a run, run-seconds a run)} over the finished lane-launches: a kind is the
    scenario's name up to its first `-` (duel, group, defence, naval); a lane-launch's seconds -- its launch,
    its settles, its runs and its end -- are shared out over its runs, so lane-seconds a run is what sizes a
    piece, and run-seconds a run (the runs' own `wall.total`) what the game time costs."""
    acc = {}
    for _d, head, rows in manifest_rows(lanes):
        runs = [r for r in rows if "index" in r]
        if not runs:
            continue
        share = (head.get("lane_seconds") or 0.0) / len(runs)
        for r in runs:
            kind = Path(r["scenario"]).stem.split("-", 1)[0]
            a = acc.setdefault(kind, [0, 0.0, 0.0])
            a[0] += 1
            a[1] += share
            a[2] += (r.get("wall") or {}).get("total", 0.0)
    return {k: (n, lane / n, run / n) for k, (n, lane, run) in sorted(acc.items())}


def build_refs(runs_dir: Path, j: int, keep: bool, check_only: bool = False):
    """Every lane-launch under runs_dir streamed once (lane_jobs's pool, on slots of the run lock),
    checked R1..R8, and its runs merged by scenario into references. Returns (written, failed: {scenario: why}). A
    scenario is all or nothing: one refused, missing or failed run refuses its reference and no
    other. `check_only` streams and checks every run the same and writes nothing, so a run of
    fewer runs than the scenarios ask (a smoke run) is checked without making a reference of it."""
    import datetime
    import hashlib
    import engrun
    R = calibref.exchange_rate()
    lanes, got = lane_jobs(runs_dir, j)
    failed, by = {}, {}
    refused = {}
    for _d, _head, rows in manifest_rows(lanes):
        for r in rows:
            if "refused" in r:
                refused.setdefault(r["refused"], f"refused at run {r['refused_index']}: {r['why']}")
    for scn, why in sorted(refused.items()):
        failed[calibref.scenario_id(TREE / scn)] = why
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
        if sid in failed:
            continue
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
        # a head without a renderer was played before the field, on the default renderer
        renderers = {g["head"].get("renderer", engrun.VULKAN) for g, _r in items}
        if len(renderers) != 1:
            failed[sid] = f"runs of more than one renderer ({' and '.join(sorted(renderers))})"
            continue
        # one arena (its hash carries the setup, calibref.arena) and one gravity read back at the
        # launches (engrun.engine_reads; None in a head from before the reads), which the reference
        # carries for the fight predictor (predfit.load_fights)
        arenas = {g["head"].get("arena_sha256") for g, _r in items}
        if len(arenas) != 1:
            failed[sid] = f"runs of more than one arena ({len(arenas)} arena hashes)"
            continue
        gravities = {(g["head"].get("engine_reads") or {}).get("gravity") for g, _r in items}
        if len(gravities) != 1:
            failed[sid] = f"runs at more than one gravity ({sorted(gravities, key=str)})"
            continue
        gravity = next(iter(gravities))
        mod = g0["head"].get("setup") or "retail"
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
        if check_only:
            print(f"checked {sid}: {len(items)} runs, every start check ok")
            continue
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
                          "mod": mod},
               "setup": {"map": key[0], "los": key[1], "mapping": key[2],
                         "commander_death": key[6] if len(key) > 6 else None,
                         "game_speed": key[4],
                         "turbo_k": 1, "unit_limit": key[3], "apply_phase": 1,
                         "s_min": engrun.S_MIN, "renderer": next(iter(renderers)),
                         **({"gravity": gravity} if gravity is not None else {})},
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
    if not keep and not failed and not check_only:
        engrun.remove_recordings(lanes)
    return written, failed


def runs_setup(runs_dir: Path):
    """The compat setup the finished lane-launches under runs_dir played (their heads' `setup`), None for
    retail; CalibError when they name more than one: a setup's references go to its own root alone."""
    import engrun
    names = {head.get("setup") for _d, head, _rows in manifest_rows(engrun.finished_lanes(runs_dir))}
    if len(names) > 1:
        raise calibref.CalibError(f"{runs_dir}: lane-launches of more than one setup "
                                  f"({', '.join(sorted(n or 'retail' for n in names))})")
    return next(iter(names), None)


def cmd_refs(a):
    try:
        written, failed = build_refs(calibref.runs_in_root(a.runs_dir), a.j, a.keep_recordings, a.check_only)
        import engrun
        cost = cost_by_kind(engrun.finished_lanes(Path(a.runs_dir)))
    except calibref.CalibError as e:
        print(f"calib refs build: could not run: {e}")
        return 2
    for p in written:
        print(f"wrote {p}")
    for k, why in failed.items():
        print(f"could not build {k}: {why}")
    for kind, (n, lane, run) in cost.items():
        print(f"cost {kind}: {n} runs, {lane:.2f} lane-seconds a run, {run:.2f} run-seconds a run")
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
        stale, changes = {}, {}
        for sid, (_p, ref) in sorted(refs.items()):
            why = check.stale_reasons(ref, since=a.since, catalogue=cat, changes=changes)
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
    e.add_argument("--arena", help="the arena's config (default tools/tasim/calib-arena.json)")
    e.add_argument("--resume", metavar="DIR",
                   help="continue the engine run in DIR: play only the runs its finished "
                        "lane-launches lack, refused when they were played by another DLL, arena, "
                        "setup, renderer or pacing (engrun.py, RESUMING)")
    e.add_argument("--gpu", action="store_true",
                   help="render on the default renderer, for debugging, taking a GPU token a lane; "
                        "without it the lanes render with GDI and take none")
    e.add_argument("--draw", action="store_true",
                   help="render every frame, for debugging (engrun.py, THE PACING); --gpu implies it")
    e.add_argument("--setup", help="a compat setup (tools/compat/setups.json): every lane in its own folder, "
                                   "TACALIB_DIR its root <main checkout>/.data/calib-mods/<setup> exactly")
    e.set_defaults(func=cmd_engine)
    r = sub.add_parser("refs", help="build references from an engine run's lane manifests")
    rs = r.add_subparsers(dest="refs_cmd", required=True)
    rb = rs.add_parser("build", help="stream each lane's recording once, check R1..R8, merge")
    rb.add_argument("runs_dir")
    rb.add_argument("--j", type=int, default=8, help="processes")
    rb.add_argument("--keep-recordings", action="store_true")
    rb.add_argument("--arena", help="the arena the runs played, as calib.py engine was given it")
    rb.add_argument("--check-only", action="store_true",
                    help="stream and check every run, write no reference (a smoke run of fewer runs "
                         "than its scenarios ask)")
    rb.set_defaults(func=cmd_refs)
    c = sub.add_parser("check", help="grade tasim against the references")
    c.add_argument("--tasim", help="the tasim_run binary (default $TASIM_RUN)")
    c.add_argument("--only", help="a glob on the scenario file")
    c.add_argument("--require-fresh", action="store_true")
    c.add_argument("--json", help="write the ta-calib-score/1 scoreboard here")
    c.add_argument("--j", type=int, default=8)
    c.add_argument("--arena", help="the arena the references played (a naval set: tools/tasim/calib-arena-naval.json)")
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
    if getattr(a, "arena", None):
        # the environment, so the lanes' and the reference builder's pools read the same arena
        os.environ[calibref.ARENA_ENV] = str(Path(a.arena).resolve())
    # THE ROOT INVARIANT (calibref.calib_dir), checked once before anything reads a root: the setup comes
    # from --setup on `engine`, from the lane-launches' heads on `refs build`, and from $TACALIB_SETUP on
    # the other commands; the environment carries it to the reference builder's pool
    try:
        setup = getattr(a, "setup", None)
        if a.cmd == "refs":
            setup = runs_setup(Path(a.runs_dir))
        if (a.cmd == "engine" or a.cmd == "refs") and calibref.setup_name() not in (None, setup):
            raise calibref.CalibError(f"{calibref.SETUP_ENV} is {calibref.setup_name()}, the run's setup "
                                      f"{setup or 'retail'}")
        if a.cmd in ("engine", "refs"):
            os.environ.pop(calibref.SETUP_ENV, None)
            if setup:
                os.environ[calibref.SETUP_ENV] = setup
        calibref.calib_dir()
    except calibref.CalibError as e:
        print(f"calib.py: could not run: {e}", file=sys.stderr)
        return 2
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
