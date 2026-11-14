#!/usr/bin/env python3
"""packcheck.py -- the packed lane's statistical equivalence proof: packed references against unpacked.

    packcheck.py --u U_REFS --p P_REFS [--predfit-u U.json --predfit-p P.json] [--json OUT]

U_REFS and P_REFS are two reference folders (calib.py refs build's: <stem>@<hash>/<build>.json), the
unpacked arm U and the packed arm P of the same jobs: each fight's runs share their index, seed, jitter,
after-draws and swap (perturb.run_wire), so a pair differs only by the simulation RNG, the map and the
packing. The pairs are made by scenario id and run index, and a pair whose perturbation records differ is
refused: it was not the same job. The design: research/notes/modules/tasim-calibration.md, *The packed
lane*; the thresholds below are the owner's (2026-11-13), and a threshold changed after seeing P is the
owner's decision too.

THE FOUR TESTS, each per metric (winner, margin, ttk, survivors; a fight counts for a metric its block
grades):
  T1  check.py's rules with P as the candidate and U as the reference, the engine against itself, which is
      the self-test's null. Pass: the OUT count over n fights is at most the 99 % quantile of
      Binomial(n, alpha_m), alpha_m the metric's false-alarm rate over U's self-test (`calib.py selftest`
      writes it into each reference; U must carry it).
  T2  per fight, the mean paired difference P - U (winner scored 1/0/0.5, ttk its value, censored at the
      limit); its one-sided p from SIGN_FLIPS seeded sign flips (mid-p, so a fight whose pairs never differ
      gives z = 0); z = Phi^-1(1 - p); Stouffer's S = sum z / sqrt(n). Pass: |S| < S_MAX (two-sided alpha
      0.0025, 0.01 over the four metrics).
  T3  per metric, the fights whose two-sided sign-flip p is below P_FIGHT. Pass: at most the T3_Q quantile
      of Binomial(n, P_FIGHT): a large bias in a few fights, which T2's average dilutes.
  T4  the decisive labels (A's win rate above 0.8 or below 0.2, predfit's): P's disagreements with U at most
      the 99 % quantile of their null, a seeded parametric bootstrap of two independent Binomial(n, p_U)
      samples per fight; and both arms pass predfit's own 90 % check (`predfit.py check --json` of each
      arm's references, given as --predfit-u and --predfit-p).
Passing is all four for every metric. Exit 0 pass, 1 a test failed (each named), 2 could not run (no pair,
a refused pair, U without its self-test, or T4 without the two predfit reports).
"""

import argparse
import json
import math
import statistics
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(HERE))
import calibref  # noqa: E402
import check  # noqa: E402
import metrics  # noqa: E402

METRICS = ("winner", "margin", "ttk", "survivors")
T1_Q = 0.99
SIGN_FLIPS = 10000
S_MAX = 3.02                    # Phi^-1(1 - 0.0025 / 2)
P_FIGHT = 0.01
T3_Q = 0.9975
DECISIVE = 0.8                  # predfit.DECISIVE
T4_Q = 0.99
BOOTSTRAP = 10000
SEED = 20261113                 # every draw here: the flips, the bootstrap


class CouldNotRun(Exception):
    pass


# ---- the distributions ------------------------------------------------------------------------

def binom_ppf(q: float, n: int, p: float) -> int:
    """The least k with P(Binomial(n, p) <= k) >= q."""
    if p <= 0.0:
        return 0
    if p >= 1.0:
        return n
    cdf = 0.0
    for k in range(n + 1):
        cdf += math.exp(math.lgamma(n + 1) - math.lgamma(k + 1) - math.lgamma(n - k + 1)
                        + k * math.log(p) + (n - k) * math.log1p(-p))
        if cdf >= q - 1e-12:
            return k
    return n


def z_of(p: float) -> float:
    return statistics.NormalDist().inv_cdf(1.0 - p)


# ---- the references ---------------------------------------------------------------------------

def load(root) -> dict:
    """{scenario id: the newest reference of it} in a reference folder."""
    out = {}
    root = Path(root)
    if not root.is_dir():
        raise CouldNotRun(f"no reference folder at {root}")
    for d in sorted(root.iterdir()):
        refs = sorted(d.glob("*.json"), key=lambda p: p.stat().st_mtime) if d.is_dir() else []
        if not refs:
            continue
        ref = json.loads(refs[-1].read_text())
        if ref.get("schema") != calibref.SCHEMA_REF:
            raise CouldNotRun(f"{refs[-1]}: not {calibref.SCHEMA_REF}")
        out[ref["scenario"]["id"]] = ref
    return out


def value(metric: str, m: dict):
    """A run's number for a metric: the winner scored for side A, ttk its value."""
    v = m.get(metric)
    if v is None:
        return None
    if metric == "winner":
        return check.win_score(v)
    return dict(metrics.scalars(metric, v))[metric]


def pairs(u: dict, p: dict) -> dict:
    """{scenario id: (block, [(U run, P run)] by index)} over the statistical fights both arms hold.
    CouldNotRun when a pair's perturbations differ, or when no fight is in both."""
    out = {}
    for sid in sorted(set(u) & set(p)):
        block = u[sid]["scenario"]["calib"]
        if "exact" in block["kind"]:
            continue
        ur = {r["index"]: r for r in u[sid]["runs"]}
        pr = {r["index"]: r for r in p[sid]["runs"]}
        both = sorted(set(ur) & set(pr))
        for i in both:
            if ur[i].get("perturbation") != pr[i].get("perturbation"):
                raise CouldNotRun(f"{sid} run {i}: the two arms' perturbations differ: not the same job")
        if both:
            out[sid] = (block, [(ur[i], pr[i]) for i in both])
    if not out:
        raise CouldNotRun("no fight is in both arms")
    return out


def rng_for(*parts):
    import numpy
    return numpy.random.default_rng([SEED] + [zlib.crc32(str(x).encode()) for x in parts])


# ---- the tests --------------------------------------------------------------------------------

def false_alarm_rates(u: dict, sids) -> dict:
    """{metric: alpha} over U's stored self-test (check.false_alarms, `calib.py selftest`)."""
    tally = {}
    for sid in sids:
        fa = (u[sid].get("selftest") or {}).get("false_alarms")
        if not fa:
            raise CouldNotRun(f"{sid}: U carries no self-test (calib.py selftest on U's references)")
        for metric, row in fa.items():
            t = tally.setdefault(metric, [0, 0])
            t[0] += row["out"]
            t[1] += row["splits"]
    return {m: o / s for m, (o, s) in tally.items() if s}


def t1(u: dict, prs: dict) -> dict:
    alpha = false_alarm_rates(u, prs)
    outs = {}
    for sid, (block, both) in prs.items():
        ref = u[sid]
        e_runs = [a["metrics"] for a, _b in both]
        t_runs = [b["metrics"] for _a, b in both]
        swaps = [a["swap"] for a, _b in both]
        for row in check.grade_runs(block, e_runs, t_runs, ref["shuffle"], (swaps, swaps)):
            if row["metric"] not in METRICS:
                continue
            o = outs.setdefault(row["metric"], [0, 0, []])
            o[1] += 1
            if row["verdict"] == "out":
                o[0] += 1
                o[2].append(sid)
    res = {}
    for m, (out, n, which) in sorted(outs.items()):
        if m not in alpha:
            raise CouldNotRun(f"T1: U's self-test has no false-alarm rate for {m}")
        limit = binom_ppf(T1_Q, n, alpha[m])
        res[m] = {"out": out, "fights": n, "alpha": alpha[m], "limit": limit, "pass": out <= limit,
                  "out_fights": which}
    return res


def sign_flip(diffs, *seed):
    """(one-sided mid-p of the mean being as large as observed, two-sided p) from SIGN_FLIPS seeded flips."""
    import numpy
    d = numpy.asarray(diffs, float)
    obs = d.mean()
    flips = rng_for(*seed).integers(0, 2, size=(SIGN_FLIPS, len(d))) * 2 - 1
    means = (flips * d).mean(axis=1)
    eps = 1e-12 * max(1.0, float(numpy.abs(d).max()))
    above = int((means > obs + eps).sum())
    tie = int((numpy.abs(means - obs) <= eps).sum())
    p_one = (above + 0.5 * tie + 0.5) / (SIGN_FLIPS + 1)
    p_two = (int((numpy.abs(means) >= abs(obs) - eps).sum()) + 1) / (SIGN_FLIPS + 1)
    return p_one, min(1.0, p_two)


def per_fight(prs: dict) -> dict:
    """{metric: [(sid, mean difference, one-sided p, two-sided p)]}."""
    out = {}
    for sid, (block, both) in prs.items():
        for m in METRICS:
            if m not in block["metrics"]:
                continue
            diffs = []
            for a, b in both:
                va, vb = value(m, a["metrics"]), value(m, b["metrics"])
                if va is not None and vb is not None:
                    diffs.append(vb - va)
            if not diffs:
                continue
            p1, p2 = sign_flip(diffs, sid, m)
            out.setdefault(m, []).append((sid, sum(diffs) / len(diffs), p1, p2))
    return out


def t2(fights: dict) -> dict:
    res = {}
    for m, rows in sorted(fights.items()):
        zs = [z_of(p1) for _sid, _d, p1, _p2 in rows]
        s = sum(zs) / math.sqrt(len(zs))
        res[m] = {"S": s, "fights": len(zs), "limit": S_MAX, "pass": abs(s) < S_MAX}
    return res


def t3(fights: dict) -> dict:
    res = {}
    for m, rows in sorted(fights.items()):
        low = [sid for sid, _d, _p1, p2 in rows if p2 < P_FIGHT]
        limit = binom_ppf(T3_Q, len(rows), P_FIGHT)
        res[m] = {"fights_below": len(low), "fights": len(rows), "limit": limit, "pass": len(low) <= limit,
                  "below": low}
    return res


def label(rate: float):
    return "A" if rate > DECISIVE else "B" if rate < 1 - DECISIVE else None


def win_rate(runs) -> float:
    return sum(check.win_score(r["metrics"]["winner"]) for r in runs) / len(runs)


def predfit_pass(doc: dict) -> tuple:
    """(passed, why) of a `predfit.py check --json` report, by predfit's own rule (check_report)."""
    sys.path.insert(0, str(TREE / "tools" / "impai"))
    import predfit
    half = doc["halves"]["check"]
    if half["decisive"] < predfit.MIN_DECISIVE:
        return False, f"{half['decisive']} decisive fights in the check half, {predfit.MIN_DECISIVE} needed"
    failed = [k for k, v in doc.get("kinds", {}).items() if v["judged"] and not v["passed"]]
    ok = half["agree"] >= predfit.PASS * half["decisive"] and not failed
    return ok, f"{half['agree']}/{half['decisive']} agree" + (f", failed kinds {failed}" if failed else "")


def t4(prs: dict, predfit_u=None, predfit_p=None) -> dict:
    import numpy
    rows = []
    for sid, (block, both) in prs.items():
        if "winner" not in block["metrics"]:
            continue
        ru, rp = win_rate([a for a, _b in both]), win_rate([b for _a, b in both])
        rows.append((sid, len(both), ru, rp))
    disagree = [sid for sid, _n, ru, rp in rows if label(ru) != label(rp)]
    rng = rng_for("t4")
    n = numpy.array([r[1] for r in rows])
    pu = numpy.array([r[2] for r in rows])

    def labels(w):
        rate = w / n
        return numpy.where(rate > DECISIVE, 1, numpy.where(rate < 1 - DECISIVE, -1, 0))
    shape = (BOOTSTRAP, len(rows))
    null = (labels(rng.binomial(n, pu, size=shape)) != labels(rng.binomial(n, pu, size=shape))).sum(axis=1)
    limit = int(numpy.quantile(null, T4_Q, method="higher"))
    res = {"disagree": len(disagree), "fights": len(rows), "limit": limit, "which": disagree,
           "labels_pass": len(disagree) <= limit}
    if predfit_u is None or predfit_p is None:
        res["predfit"] = None
        res["pass"] = None
        return res
    pf = {arm: predfit_pass(json.loads(Path(f).read_text())) for arm, f in (("U", predfit_u), ("P", predfit_p))}
    res["predfit"] = {arm: {"pass": ok, "why": why} for arm, (ok, why) in pf.items()}
    res["pass"] = res["labels_pass"] and all(ok for ok, _why in pf.values())
    return res


def run(u_root, p_root, predfit_u=None, predfit_p=None) -> dict:
    u, p = load(u_root), load(p_root)
    prs = pairs(u, p)
    fights = per_fight(prs)
    return {"fights": len(prs), "T1": t1(u, prs), "T2": t2(fights), "T3": t3(fights),
            "T4": t4(prs, predfit_u, predfit_p)}


def verdict(res: dict) -> int:
    """0 pass, 1 a test failed, 2 T4 could not be completed."""
    failed = any(not r["pass"] for t in ("T1", "T2", "T3") for r in res[t].values())
    t4r = res["T4"]
    if failed or not t4r["labels_pass"] or t4r["pass"] is False:
        return 1
    return 2 if t4r["pass"] is None else 0


def report(res: dict, say=print):
    say(f"packcheck: {res['fights']} fights in both arms")
    for m, r in res["T1"].items():
        say(f"T1 {m:9s} OUT {r['out']}/{r['fights']} (alpha {r['alpha']:.3f}, limit {r['limit']}): "
            f"{'pass' if r['pass'] else 'FAIL'}")
    for m, r in res["T2"].items():
        say(f"T2 {m:9s} S {r['S']:+.3f} over {r['fights']} (|S| < {r['limit']}): {'pass' if r['pass'] else 'FAIL'}")
    for m, r in res["T3"].items():
        say(f"T3 {m:9s} p < {P_FIGHT} in {r['fights_below']}/{r['fights']} (limit {r['limit']}): "
            f"{'pass' if r['pass'] else 'FAIL'}")
    t = res["T4"]
    say(f"T4 labels   {t['disagree']} disagree of {t['fights']} (limit {t['limit']}): "
        f"{'pass' if t['labels_pass'] else 'FAIL'}")
    if t["predfit"] is None:
        say("T4 predfit  not given (--predfit-u, --predfit-p): could not run")
    else:
        for arm, r in t["predfit"].items():
            say(f"T4 predfit  {arm}: {r['why']}: {'pass' if r['pass'] else 'FAIL'}")
    code = verdict(res)
    say("packcheck: " + ("PASS" if code == 0 else "FAIL" if code == 1 else "COULD NOT RUN (T4 incomplete)"))
    return code


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--u", required=True, help="the unpacked arm's reference folder")
    ap.add_argument("--p", required=True, help="the packed arm's reference folder")
    ap.add_argument("--predfit-u", help="predfit.py check --json of U's references")
    ap.add_argument("--predfit-p", help="predfit.py check --json of P's references")
    ap.add_argument("--json", help="write the results here")
    a = ap.parse_args(argv)
    try:
        res = run(a.u, a.p, a.predfit_u, a.predfit_p)
    except (CouldNotRun, check.CouldNotCheck) as e:
        print(f"packcheck: could not run: {e}")
        return 2
    if a.json:
        Path(a.json).write_text(json.dumps(res, indent=1) + "\n")
    return report(res)


if __name__ == "__main__":
    sys.exit(main())
