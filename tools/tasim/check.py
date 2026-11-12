"""check.py -- tasim graded against the engine's references: the rules, the scoreboard, staleness, self-tests.

    calib.py check [--tasim PATH] [--only GLOB] [--require-fresh] [--json OUT]
    calib.py selftest [--only GLOB] [--against REF]
    calib.py stale [--since REV] [--json] [--catalogue FILE]

The rules (research/notes/modules/tasim-calibration.md, *The check*), exactly:
  1. win rate: in band iff |p_t − p_e| ≤ 2·SE, SE = √(p̄(1 − p̄)(1/n_e + 1/n_t)); SE = 0 needs p_t = p_e;
  2. distribution: in band iff P10_e ≤ P50_t ≤ P90_e and 0.5 ≤ spread_t / spread_e ≤ 2, the
     quantiles numpy's linear ones; a zero engine spread needs P90_t = P10_t = P50_e;
  3. exact: the family's series equal at every tick, no tolerance;
  4. predictor: on the decisive scenarios (p_e > 0.8 or < 0.2) the call agrees with the engine's
     majority on at least 90 %.
Exit 0 every metric in band, 1 one out of band (each named), 2 could not run.
"""

import json
import math
import os
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(TREE / "tools" / "replay"))
import calibref  # noqa: E402
import metrics  # noqa: E402

BAND_SE = 2.0
SPREAD_LO, SPREAD_HI = 0.5, 2.0
DECISIVE = 0.2
PREDICTOR_AGREE = 0.9
# a reference made at commit C is stale when C..HEAD touches one of these (or its scenario)
STALE_FILES = ("tagpu/ddraw/src/tagpu_scenario.c", "tools/tacli", "tools/tasim/perturb.py",
               "tools/tasim/engrun.py", "tools/tasim/calib-arena.json", "scenarios/calib/arena.json")


class CouldNotCheck(Exception):
    pass


# ---- the rules ----------------------------------------------------------------------------------

def win_score(w):
    return 1.0 if w == "A" else 0.0 if w == "B" else 0.5


def rule_winrate(e, t):
    """(in band, margin, p_e, p_t) of side A's win rates, a draw scored 0.5."""
    ne, nt = len(e), len(t)
    pe = sum(map(win_score, e)) / ne
    pt = sum(map(win_score, t)) / nt
    pbar = (pe * ne + pt * nt) / (ne + nt)
    se = math.sqrt(pbar * (1 - pbar) * (1 / ne + 1 / nt))
    if se == 0:
        return pt == pe, (0.0 if pt == pe else math.inf), pe, pt
    margin = abs(pt - pe) / (BAND_SE * se)
    return abs(pt - pe) <= BAND_SE * se, margin, pe, pt


def quantiles(x):
    import numpy
    q = numpy.quantile(numpy.asarray(x, dtype=float), [0.1, 0.5, 0.9], method="linear")
    return [float(v) for v in q]


def rule_distribution(e, t):
    """(in band, margin {outside, spread}, engine P10/P50/P90, tasim P10/P50/P90)."""
    e10, e50, e90 = quantiles(e)
    t10, t50, t90 = quantiles(t)
    if e90 == e10:
        ok = t90 == t10 == e50
        return ok, {"outside": abs(t50 - e50), "spread": None}, [e10, e50, e90], [t10, t50, t90]
    spread = (t90 - t10) / (e90 - e10)
    outside = max(e10 - t50, t50 - e90, 0.0)
    ok = outside == 0.0 and SPREAD_LO <= spread <= SPREAD_HI
    return ok, {"outside": outside, "spread": spread}, [e10, e50, e90], [t10, t50, t90]


def rule_exact(e_series, t_series):
    """(in band, the first difference as [index, engine item, tasim item] or None)."""
    d = metrics.first_difference(e_series, t_series)
    return d is None, (list(d) if d else None)


def rule_predictor(rows):
    """rows: [(p_e, call)] of every scenario. (in band, agreement, decisive, left out)."""
    decisive = [(pe, c) for pe, c in rows if pe > 1 - DECISIVE or pe < DECISIVE]
    left = len(rows) - len(decisive)
    if not decisive:
        return True, None, 0, left
    agree = sum(1 for pe, c in decisive if (c == "A") == (pe > 0.5)) / len(decisive)
    return agree >= PREDICTOR_AGREE, agree, len(decisive), left


def grade_metric(metric, kind, e_vals, t_vals):
    """One (metric, kind) graded: the scoreboard's row."""
    if kind == "winrate":
        ok, margin, pe, pt = rule_winrate(e_vals, t_vals)
        return {"metric": metric, "kind": kind, "verdict": "in" if ok else "out",
                "margin": margin, "engine": {"p": pe, "n": len(e_vals)},
                "tasim": {"p": pt, "n": len(t_vals)}}
    ok, margin, eq, tq = rule_distribution(e_vals, t_vals)
    return {"metric": metric, "kind": kind, "verdict": "in" if ok else "out", "margin": margin,
            "engine": {"q": eq, "n": len(e_vals)}, "tasim": {"q": tq, "n": len(t_vals)}}


def grade_runs(block, e_runs, t_runs):
    """Every statistical metric of a block, engine runs against the other side's: rows."""
    rows = []
    for m in block["metrics"]:
        if m == "winner":
            rows.append(grade_metric(m, "winrate", [r["winner"] for r in e_runs],
                                     [r["winner"] for r in t_runs]))
            continue
        e_s = [dict(metrics.scalars(m, r[m])) for r in e_runs]
        t_s = [dict(metrics.scalars(m, r[m])) for r in t_runs]
        for name in sorted(e_s[0]) if e_s else ():
            rows.append(grade_metric(name, "distribution", [s[name] for s in e_s],
                                     [s[name] for s in t_s]))
    return rows


# ---- references and staleness ----------------------------------------------------------------

def load_refs(only=None):
    """The newest reference of every scenario id, {id: (path, ref)}; a malformed one raises."""
    out = {}
    root = calibref.refs_dir()
    for d in sorted(root.iterdir()) if root.is_dir() else ():
        refs = sorted(d.glob("*.json"), key=lambda p: p.stat().st_mtime)
        if not refs:
            continue
        p = refs[-1]
        try:
            ref = json.loads(p.read_text())
        except json.JSONDecodeError as e:
            raise CouldNotCheck(f"{p}: {e}")
        if ref.get("schema") != calibref.SCHEMA_REF:
            raise CouldNotCheck(f"{p}: not {calibref.SCHEMA_REF}")
        if ref["engine"]["build"].endswith("-dirty"):
            raise CouldNotCheck(f"{p}: made by a dirty DLL")
        if only and not Path(ref["scenario"]["file"]).match(only):
            continue
        out[ref["scenario"]["id"]] = (p, ref)
    return out


def stale_reasons(ref, head="HEAD", since=None, catalogue=None):
    """Why a reference is stale at `head` (an empty list when it is not), through tareplay's own
    reading of the gate: a sim file's changed code lines (comments and whitespace are not code),
    a header one of them includes, the runner's own files, the scenario. With `since`, only the
    changes of since..head count: what a landing made stale."""
    import tafeat
    import tareplay as R
    commit = ref["engine"]["commit"]
    base = since or commit
    why = []
    paths = [p for p in R.git("diff", "--name-only", f"{base}...{head}").split() if p]
    sims = {R.DD + f for f in tafeat.sim_files(R.TREE / "tagpu" / "ddraw")}
    heads = set()
    for s in sims:
        heads |= R.includes_of(s)
    scn = ref["scenario"]["file"]
    for p in paths:
        if p in sims:
            if not (R.TREE / p).exists() or R.changed_lines(p, base):
                why.append(f"{p}: code changed")
        elif p in heads:
            why.append(f"{p}: a header a sim file includes")
        elif p in STALE_FILES or p == scn:
            why.append(f"{p}: changed")
    path = R.TREE / scn
    if since is None and (not path.exists() or calibref.sha256_file(path) != ref["scenario"]["sha256"]):
        why.append(f"{scn}: its sha256 differs")
    if catalogue is not None:
        types = list(ref["values"]["types"])
        if calibref.install_fingerprint(catalogue, types) != ref["engine"]["install"]:
            why.append("the install's fingerprint differs")
    return why


# ---- tasim --------------------------------------------------------------------------------------

def tasim_play(tasim, ref, run, workdir: Path, values, arena_cfg):
    """One run played by tasim on the reference's inputs: (its trace's lines, its JSON)."""
    import perturb
    path = TREE / ref["scenario"]["file"]
    _exp, record, wire, clauses = perturb.run_wire(path, run["index"], values, arena_cfg)
    import hashlib
    if hashlib.sha256(wire.encode()).hexdigest() != run["wire_sha256"] or \
            record != run["perturbation"]:
        raise CouldNotCheck(f"{ref['scenario']['id']} run {run['index']}: the perturbation "
                            f"differs from the reference's (perturb.py changed: re-measure)")
    d = workdir / f"{run['index']}"
    d.mkdir(parents=True, exist_ok=True)
    (d / "wire.txt").write_text(wire)
    (d / "setup.json").write_text(json.dumps(ref["setup"]))
    (d / "exogenous.json").write_text(json.dumps(run["exogenous"] or {}))
    trace = d / "trace.jsonl"
    cmd = [str(tasim), "play", "--setup", str(d / "setup.json"), "--wire", str(d / "wire.txt"),
           "--run-seed", str(run["seed"]), "--exogenous", str(d / "exogenous.json"),
           "--limit", str(ref["scenario"]["calib"]["limit"]),
           "--end", ",".join(f"{s}:{n}" for s, n in clauses), "--trace", str(trace), "--json"]
    if str(tasim).endswith(".py"):
        cmd = [sys.executable] + cmd
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if r.returncode != 0:
        raise CouldNotCheck(f"{ref['scenario']['id']} run {run['index']}: tasim exited "
                            f"{r.returncode}: {(r.stdout + r.stderr).strip()[-300:]}")
    try:
        out = json.loads(r.stdout)
    except json.JSONDecodeError:
        raise CouldNotCheck(f"tasim printed no JSON: {r.stdout.strip()[-200:]}")
    return metrics.read_trace(trace), out


def engine_traces(ref, path: Path):
    """An exact reference's engine traces by run index."""
    sid, commit = ref["scenario"]["id"], ref["engine"]["commit"]
    lines = metrics.read_trace(calibref.traces_path(sid, commit))
    by, cur = {}, None
    for ln in lines:
        if ln.get("schema") == calibref.SCHEMA_TRACE:
            cur = by.setdefault(ln["run"], [])
        cur.append(ln)
    return by


def check(tasim, only=None, require_fresh=False, j=8):
    """The scoreboard: (rows, could-not-run messages)."""
    arena_cfg = calibref.arena()
    refs = load_refs(only)
    if not refs:
        raise CouldNotCheck("no reference" + (f" matches {only}" if only else ""))
    rows, errors = [], []
    with tempfile.TemporaryDirectory(prefix="tacalib-") as tmp:
        for sid, (p, ref) in sorted(refs.items()):
            why = stale_reasons(ref)
            if why and require_fresh:
                errors.append(f"{sid}: stale ({'; '.join(why)})")
                continue
            block = ref["scenario"]["calib"]
            values = {t: dict(v) for t, v in ref["values"]["types"].items()}
            try:
                with ThreadPoolExecutor(j) as ex:
                    played = list(ex.map(lambda run: tasim_play(
                        tasim, ref, run, Path(tmp) / sid, values, arena_cfg), ref["runs"]))
            except CouldNotCheck as e:
                errors.append(str(e))
                continue
            area = ref["scenario"]["calib"]["area"]
            stale = "stale" if why else None
            if "exact" in block["kind"]:
                eng = engine_traces(ref, p)
                for fam in block["metrics"]:
                    for run, (lines, _o) in zip(ref["runs"], played):
                        ok, diff = rule_exact(metrics.exact_series(eng[run["index"]], fam),
                                              metrics.exact_series(lines, fam))
                        rows.append({"id": sid, "area": area, "metric": fam, "kind": "exact",
                                     "run": run["index"],
                                     "verdict": stale or ("in" if ok else "out"),
                                     "margin": diff, "engine": None, "tasim": None})
                continue
            R = ref["values"]["R"]
            t_runs = [metrics.compute(lines, block, values, R) for lines, _o in played]
            for row in grade_runs(block, [r["metrics"] for r in ref["runs"]], t_runs):
                row.update({"id": sid, "area": area})
                if stale:
                    row["verdict"] = "stale"
                rows.append(row)
    return rows, errors


def area_of(area: str) -> str:
    parts = area.split("/")
    return "/".join(parts[:-1]) if len(parts) > 1 else area


def fmt_side(row, side):
    v = row.get(side)
    if not v:
        return "—"
    if "p" in v:
        return f"A {v['p']:.2f} ({v['n']})"
    q = v["q"]
    return f"{q[0]:g}/{q[1]:g}/{q[2]:g}"


def fmt_margin(row):
    m = row["margin"]
    if row["kind"] == "exact":
        return "—" if m is None else f"first diff #{m[0]}"
    if isinstance(m, dict):
        sp = "-" if m["spread"] is None else f"{m['spread']:.2f}"
        return f"{m['outside']:g} spread {sp}"
    return f"{m:.2f}"


def report_text(rows, predictor=None):
    out = []
    groups = {}
    for r in rows:
        groups.setdefault(area_of(r["area"]), []).append(r)
    for area, rs in sorted(groups.items()):
        n_in = sum(1 for r in rs if r["verdict"] == "in")
        out.append(f"area {area:<48} in band {n_in}/{len(rs)}")
        out.append(f"  {'scenario':<32} {'metric':<22} {'engine':<21} {'tasim':<21} margin")
        for r in rs:
            out.append(f"  {r['id'][:32]:<32} {r['metric']:<22} {fmt_side(r, 'engine'):<21} "
                       f"{fmt_side(r, 'tasim'):<21} {fmt_margin(r)}  {r['verdict'].upper()}")
    if predictor:
        out.append(f"predictor: {predictor}")
    return "\n".join(out)


def score_json(rows, predictor=None):
    areas = {}
    for r in rows:
        a = areas.setdefault(area_of(r["area"]), {"in": 0, "of": 0})
        a["of"] += 1
        a["in"] += r["verdict"] == "in"
    return {"schema": calibref.SCHEMA_SCORE, "scenarios": rows, "areas": areas,
            "predictor": predictor}


def exit_code(rows, errors):
    if errors:
        return 2
    return 1 if any(r["verdict"] == "out" for r in rows) else 0


# ---- self-tests -------------------------------------------------------------------------------

def split_half(ref):
    """H2 graded as if it were tasim against H1 (i mod 4 in {0, 1} against {2, 3}: each half
    holds as many swapped runs as unswapped)."""
    runs = ref["runs"]
    h1 = [r["metrics"] for r in runs if r["index"] % 4 in (0, 1)]
    h2 = [r["metrics"] for r in runs if r["index"] % 4 in (2, 3)]
    return grade_runs(ref["scenario"]["calib"], h1, h2)


def seat_effect(ref):
    runs = ref["runs"]
    return grade_runs(ref["scenario"]["calib"], [r["metrics"] for r in runs if not r["swap"]],
                      [r["metrics"] for r in runs if r["swap"]])


def self_consistency(ref, path):
    """An exact scenario: runs 0 and 1 (reload) and 2 (fresh) give the same series for each of
    its metrics."""
    eng = engine_traces(ref, path)
    if 0 not in eng:
        return "not exact: no run 0 in the reference"
    for fam in ref["scenario"]["calib"]["metrics"]:
        base = metrics.exact_series(eng[0], fam)
        for k in sorted(eng):
            if k == 0:
                continue
            d = metrics.first_difference(base, metrics.exact_series(eng[k], fam))
            if d:
                return (f"not exact: {fam}: runs 0 and {k} differ at item {d[0]} ({d[1]} against "
                        f"{d[2]})")
    return None


def selftest(only=None, against=None):
    """(the report's lines, the false-alarm rates per rule, could-not-run messages); writes each
    reference's `selftest`."""
    refs = load_refs(only)
    if not refs:
        # a self-test over nothing tested nothing: could not run, as `check` says
        raise CouldNotCheck("no reference" + (f" matches {only}" if only else ""))
    lines, errors = [], []
    alarms = {"winrate": [0, 0], "distribution": [0, 0]}
    for sid, (p, ref) in sorted(refs.items()):
        block = ref["scenario"]["calib"]
        if "exact" in block["kind"]:
            why = self_consistency(ref, p)
            ref["selftest"] = {"self_consistency": why or "ok"}
            if why:
                errors.append(f"{sid}: {why}")
            lines.append(f"{sid}: self-consistency {why or 'ok'}")
        else:
            sh, se = split_half(ref), seat_effect(ref)
            for r in sh:
                alarms[r["kind"]][0] += r["verdict"] == "out"
                alarms[r["kind"]][1] += 1
            ref["selftest"] = {"split_half": sh, "seat_effect": se}
            for name, rows in (("split-half", sh), ("seat effect", se)):
                for r in rows:
                    lines.append(f"{sid}: {name} {r['metric']} {r['verdict'].upper()} "
                                 f"margin {fmt_margin(r)}")
        if against is not None:
            other = json.loads(Path(against).read_text())
            rows = grade_runs(block, [r["metrics"] for r in other["runs"]],
                              [r["metrics"] for r in ref["runs"]])
            ref["selftest"]["reload_vs_fresh"] = rows
            for r in rows:
                lines.append(f"{sid}: reload against fresh {r['metric']} {r['verdict'].upper()} "
                             f"margin {fmt_margin(r)}")
        p.write_text(json.dumps(ref, indent=1) + "\n")
    rates = {k: (v[0] / v[1] if v[1] else None, v[1]) for k, v in alarms.items()}
    return lines, rates, errors


def env_tasim(given):
    t = given or os.environ.get("TASIM_RUN")
    if not t:
        raise CouldNotCheck("no tasim: --tasim PATH or $TASIM_RUN (tasim_run lands in T1)")
    return Path(t)
