#!/usr/bin/env python3
"""packcheck.py -- the packed lane's statistical equivalence proof: packed references against unpacked.

    packcheck.py --u U_REFS --p P_REFS [--predfit-u U.json --predfit-p P.json] [--json OUT]

U_REFS and P_REFS are two reference folders (calib.py refs build's: <stem>@<hash>/<build>.json), the
unpacked arm U and the packed arm P of the same jobs: each fight's runs share their index, seed, jitter,
after-draws and swap (perturb.run_wire), so a pair differs only by the simulation RNG, the map and the
packing. The pairs are made by scenario id and run index, and a pair whose perturbation records differ is
refused: it was not the same job. The design: research/notes/modules/tasim-calibration.md, *The packed
lane*; the alphas below are the owner's (2026-11-13), their nulls from cluster relabellings too
(2026-11-14), and a threshold changed after seeing P is the owner's decision.

THE CLUSTERS. Co-batched cells share their batch's random inputs -- the simulation RNG and the wind -- so their
outcomes are not independent, and neither are the fights that meet in batch after batch. Every test below
takes its null from relabellings of whole CLUSTERS instead of single runs or fights, so the dependence prices
itself into each threshold and no test assumes that fights are independent. A pair's cluster is its P run's
batch (`pack.batch_wire`, the batch's wire hash); an unpacked P run is a cluster of its own; and clusters whose
runs share a stream seed (a U run's or an unpacked run's own, a batch's pack.batch_seed) are merged, so a
32-bit collision of two seeds cannot make two clusters share their inputs unseen. That clusters share no
random input otherwise rests on THE STREAMS (engrun.py): every apply seeds every stream from its own wire's
seed, before its creates, wherever its lane-launch puts it, which a reference records as `setup.streams`
"apply" (a DLL that seeds after the creates cannot make that head: engrun.seed_report requires its log line's
"before the creates"). Refused, could
not run: a P cell without `batch_wire`; a reference of either arm with neither `streams` "apply" nor an
engine commit that does not descend from STREAMS_FIXED (a test launch's fixed seeds, under which every
lane-launch plays one wind schedule; an engine before it seeds each launch from the clock); and two runs of
one fight in one batch.
One seeded matrix F of SIGN_FLIPS rows of +-1, a column a cluster, serves every test, fight and metric: row b
flips (T2, T3) or exchanges P and U in (T1, T4) every pair of each cluster whose entry is -1. Under the null P
and U of a pair are exchangeable and the clusters independent, and the relabellings carry the statistic's
covariance, cross-products inside a batch included: a Rademacher wild cluster null, valid as the number of
clusters grows (a generation has about 1 000), not exact at any number. The size at the owner's alphas is
measured, not proven: on synthetic shared-batch arms at most 0.0039 for T2 and 0.0031 for T3 against 0.0025
(one-sided 95 %, 2 000 replications; tests/test_packcheck.py, Size); T1's and T4's are not measured. With every run its own cluster the relabellings are each
fight's independent sign flips. The alphas stay the owner's; each threshold moves by the dependence the data
carry. Who a fight's batch partners are does not enter the clusters; MEASURED on synthetic arms (the module
note), partners drawn afresh each round leave T2's inflation and T1's and T4's limits where fixed partners put
them and lower T3's by under one fight.

THE FOUR TESTS, each per metric (winner, margin, ttk, survivors; a fight counts for a metric its block
grades):
  T1  check.py's rules with P as the candidate and U as the reference, the engine against itself: the OUT
      count over n fights. Pass: at most the T1_Q quantile of the OUT counts over F's exchanges
      (check.grade_swaps). U's self-test false-alarm rate, when it carries one, gives the Binomial(n, alpha)
      limit beside it, for reading.
  T2  per fight, the mean paired difference P - U (winner scored 1/0/0.5, ttk its value, censored at the
      limit); its one-sided mid-p against the fight's flipped means M = (F[:, c] * d).mean(axis=1), c its pairs'
      clusters (so a fight whose pairs never differ gives z = 0); z = Phi^-1(1 - p); Stouffer's
      S = sum z / sqrt(n). Each row b of F gives every fight a null p and z by the rank of M[b] in M, and so a
      null S_b. Pass: |S| below the 1 - T2_ALPHA quantile of |S_b| (two-sided alpha 0.0025, 0.01 over the four
      metrics); var(S_b) is the inflation, 1 with independent clusters, and S_MAX the normal limit beside it.
  T3  per metric, the fights whose two-sided flip p is below P_FIGHT; the same count over each row's null p's.
      Pass: at most the T3_Q quantile of the null counts, Binomial(n, P_FIGHT)'s beside it: a large bias in a
      few fights, which T2's average dilutes.
  T4  the decisive labels (A's win rate above 0.8 or below 0.2, predfit's): P's disagreements with U at most
      the T4_Q quantile of the disagreements over F's exchanges; and PREDFIT'S ERRORS: one set of coefficients'
      margins on each arm (`predfit.py check --evaluation --json` of each arm's references, given as --predfit-u
      and --predfit-p), and its errors on each -- a fight decisive in that arm whose margin is 0 or of the other
      sign, as predfit's judge counts them -- with |E_P - E_U| at most the T4_Q quantile of the same over F's
      exchanges. Both arms are fresh reference sets that the coefficients were not fitted to, so neither report
      is predfit's own check: the question is whether the packing changes predfit's errors, not whether the
      coefficients pass. A margin is a function of the fight's definition, the content and the coefficients
      alone, so the two reports must name one set of coefficients and one content and give every fight one
      margin; the difference is then the labels' alone, which the exchanges relabel.
Passing is all four for every metric. Exit 0 pass, 1 a test failed (each named), 2 could not run (no pair,
a refused pair or cluster, or T4 without two predfit reports of one set of coefficients).
"""

import argparse
import json
import math
import statistics
import subprocess
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
T2_ALPHA = 0.0025
S_MAX = 3.02                    # Phi^-1(1 - 0.0025 / 2): T2's limit were the fights independent
P_FIGHT = 0.01
T3_Q = 0.9975
DECISIVE = 0.8                  # predfit.DECISIVE
T4_Q = 0.99
SEED = 20261113                 # every draw here: the cluster flips
# the engine commit from which a test launch seeds every level from fixed values, so every lane-launch plays
# one wind schedule (tagpu_patches.c, A HARNESS'S RANDOM STREAMS); an engine before it seeds each launch from
# the clock (THE CLUSTERS)
STREAMS_FIXED = "2f272cd9"


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


def z_table(denom2: int):
    """z_of(k / denom2) for every k in 0..denom2 - 1, k = 0 unused: the mid-p values a rank can take are
    half-integers over the reference set's size, so 2·(that size) is the denominator and the table needs no
    inverse CDF per draw."""
    import numpy
    return numpy.array([0.0] + [z_of(k / denom2) for k in range(1, denom2)])


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


# ---- the clusters -----------------------------------------------------------------------------

def descends_from_fixed(commit: str) -> bool:
    """Whether the engine commit descends from STREAMS_FIXED; CouldNotRun when git cannot tell."""
    rc = subprocess.run(["git", "-C", str(TREE), "merge-base", "--is-ancestor", STREAMS_FIXED, commit],
                        capture_output=True).returncode
    if rc not in (0, 1):
        raise CouldNotRun(f"engine commit {commit}: git cannot tell whether it descends from {STREAMS_FIXED}")
    return rc == 0


def streams_problem(arm: str, sid: str, ref: dict, older: dict):
    """Why a reference's runs may share random inputs with runs of other clusters, or None. `older` caches
    descends_from_fixed by commit."""
    if (ref.get("setup") or {}).get("streams") == "apply":
        return None
    commit = (ref.get("engine") or {}).get("commit")
    if not commit:
        return f"{arm} {sid}: no setup.streams and no engine commit"
    if commit not in older:
        older[commit] = descends_from_fixed(commit)
    if older[commit]:
        return (f"{arm} {sid}: played by {commit[:12]}, after {STREAMS_FIXED}'s fixed launch seeds, without "
                f"setup.streams \"apply\": its runs share their lane-launch's wind schedule (engrun.py, THE STREAMS)")
    return None


def clusters(u: dict, p: dict, prs: dict) -> tuple:
    """(the number of clusters C, {sid: [column of each pair, in prs's order]}) by THE CLUSTERS' rule; CouldNotRun
    for each refusal there."""
    older = {}
    for arm, refs in (("U", u), ("P", p)):
        for sid in prs:
            why = streams_problem(arm, sid, refs[sid], older)
            if why:
                raise CouldNotRun(why)
    key_of, seeds = {}, []
    for sid, (_block, both) in prs.items():
        keys = []
        for a, b in both:
            pk = b.get("pack")
            if pk is None:
                key = f"r:{sid}:{b['index']}"
                seeds.append((key, b.get("seed")))
            elif not pk.get("batch_wire"):
                raise CouldNotRun(f"{sid} run {b['index']}: a packed cell without its batch_wire")
            else:
                key = pk["batch_wire"]
                seeds.append((key, pk.get("batch_seed")))
            seeds.append((key, a.get("seed")))
            keys.append(key)
        if len(set(keys)) != len(keys):
            raise CouldNotRun(f"{sid}: two of its runs in one batch")
        key_of[sid] = keys
    parent = {k: k for keys in key_of.values() for k in keys}

    def root(k):
        while parent[k] != k:
            parent[k] = parent[parent[k]]
            k = parent[k]
        return k
    by_seed = {}
    for key, seed in seeds:
        if seed is None:
            continue
        other = by_seed.setdefault(seed, key)
        ra, rb = root(key), root(other)
        if ra != rb:
            parent[max(ra, rb)] = min(ra, rb)
    roots = sorted({root(k) for k in parent})
    col = {r: i for i, r in enumerate(roots)}
    return len(roots), {sid: [col[root(k)] for k in keys] for sid, keys in key_of.items()}


def flip_matrix(n_clusters: int, flips: int = SIGN_FLIPS):
    """F: `flips` rows of +-1 (int8), a column a cluster, seeded."""
    import numpy
    return rng_for("clusters").integers(0, 2, size=(flips, n_clusters), dtype=numpy.int8) * 2 - 1


# ---- the tests --------------------------------------------------------------------------------

def false_alarm_rates(u: dict, sids) -> dict:
    """{metric: alpha} over U's stored self-test (check.false_alarms, `calib.py selftest`); {} when a fight
    carries none."""
    tally = {}
    for sid in sids:
        fa = (u[sid].get("selftest") or {}).get("false_alarms")
        if not fa:
            return {}
        for metric, row in fa.items():
            t = tally.setdefault(metric, [0, 0])
            t[0] += row["out"]
            t[1] += row["splits"]
    return {m: o / s for m, (o, s) in tally.items() if s}


def t1(u: dict, prs: dict, cols: dict, F) -> dict:
    import numpy
    alpha = false_alarm_rates(u, prs)
    outs, nulls = {}, {}
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
        exchange = F[:, cols[sid]] < 0
        for m, out in check.grade_swaps(block, e_runs, t_runs, ref["shuffle"], swaps, exchange,
                                        set(METRICS)).items():
            nulls[m] = nulls.get(m, 0) + out.astype(numpy.int32)
    res = {}
    for m, (out, n, which) in sorted(outs.items()):
        limit = int(numpy.quantile(nulls[m], T1_Q, method="higher"))
        res[m] = {"out": out, "fights": n, "limit": limit, "pass": out <= limit, "out_fights": which,
                  "alpha": alpha.get(m), "binomial": binom_ppf(T1_Q, n, alpha[m]) if m in alpha else None}
    return res


def sign_flip(d, M):
    """(one-sided mid-p of the mean of `d` being as large as observed, two-sided p), against M, the flipped
    means."""
    import numpy
    d = numpy.asarray(d, float)
    obs = d.mean()
    B = len(M)
    eps = 1e-12 * max(1.0, float(numpy.abs(d).max()))
    above = int((M > obs + eps).sum())
    tie = int((numpy.abs(M - obs) <= eps).sum())
    p_one = (above + 0.5 * tie + 0.5) / (B + 1)
    p_two = (int((numpy.abs(M) >= abs(obs) - eps).sum()) + 1) / (B + 1)
    return p_one, min(1.0, p_two)


def null_ranks(d, M):
    """For each row b, M[b]'s (one-sided mid-p numerator doubled, two-sided p) against the other B - 1 means and
    itself, sign_flip's rules with M[b] as the observed: the mid-p is that numerator over 2B."""
    import numpy
    d = numpy.asarray(d, float)
    B = len(M)
    eps = 1e-12 * max(1.0, float(numpy.abs(d).max()))
    s = numpy.sort(M)
    le_hi = numpy.searchsorted(s, M + eps, side="right")
    tie = le_hi - numpy.searchsorted(s, M - eps, side="left")      # itself included
    above = B - le_hi
    a = numpy.sort(numpy.abs(M))
    p_two = (B - numpy.searchsorted(a, numpy.abs(M) - eps, side="left")) / B
    return 2 * above + tie, numpy.minimum(1.0, p_two)


def per_fight(prs: dict, cols: dict, F) -> dict:
    """{metric: {"rows": [(sid, mean difference, one-sided p, two-sided p)], "S_null": S_b's numerator,
    "L_null": the null counts of T3}}, every fight's flips the columns of its pairs' clusters in F."""
    import numpy
    B = len(F)
    ztab = z_table(2 * B)
    out = {}
    for sid, (block, both) in prs.items():
        for m in METRICS:
            if m not in block["metrics"]:
                continue
            diffs, c = [], []
            for (a, b), col in zip(both, cols[sid]):
                va, vb = value(m, a["metrics"]), value(m, b["metrics"])
                if va is not None and vb is not None:
                    diffs.append(vb - va)
                    c.append(col)
            if not diffs:
                continue
            d = numpy.asarray(diffs, float)
            M = (F[:, c] * d).mean(axis=1)
            p1, p2 = sign_flip(d, M)
            num2, p2b = null_ranks(d, M)
            o = out.setdefault(m, {"rows": [], "S_null": numpy.zeros(B), "L_null": numpy.zeros(B, numpy.int32)})
            o["rows"].append((sid, float(d.mean()), p1, p2))
            o["S_null"] += ztab[num2]
            o["L_null"] += p2b < P_FIGHT
    return out


def t2(fights: dict) -> dict:
    import numpy
    res = {}
    for m, f in sorted(fights.items()):
        rows = f["rows"]
        root_n = math.sqrt(len(rows))
        s = sum(z_of(p1) for _sid, _d, p1, _p2 in rows) / root_n
        s_null = f["S_null"] / root_n
        limit = float(numpy.quantile(numpy.abs(s_null), 1 - T2_ALPHA, method="higher"))
        res[m] = {"S": s, "fights": len(rows), "limit": limit, "inflation": float(s_null.var()),
                  "normal": S_MAX, "pass": abs(s) < limit}
    return res


def t3(fights: dict) -> dict:
    import numpy
    res = {}
    for m, f in sorted(fights.items()):
        rows = f["rows"]
        low = [sid for sid, _d, _p1, p2 in rows if p2 < P_FIGHT]
        limit = int(numpy.quantile(f["L_null"], T3_Q, method="higher"))
        res[m] = {"fights_below": len(low), "fights": len(rows), "limit": limit,
                  "binomial": binom_ppf(T3_Q, len(rows), P_FIGHT), "pass": len(low) <= limit, "below": low}
    return res


def label(rate: float):
    return "A" if rate > DECISIVE else "B" if rate < 1 - DECISIVE else None


def win_rate(runs) -> float:
    return sum(check.win_score(r["metrics"]["winner"]) for r in runs) / len(runs)


def predfit_margins(doc_u: dict, doc_p: dict, sids) -> dict:
    """{sid: the margin} both `predfit.py check --json` reports give each fight of `sids`; CouldNotRun unless both
    name one set of coefficients and one content, hold every fight, and give each the same margin (T4)."""
    for arm, doc in (("U", doc_u), ("P", doc_p)):
        if "margins" not in doc or "params" not in doc or "content" not in doc:
            raise CouldNotRun(f"T4: the {arm} report holds no margins: `predfit.py check --evaluation --json` again")
    if (doc_u["params"], doc_u["content"]) != (doc_p["params"], doc_p["content"]):
        raise CouldNotRun(f"T4: the two reports are of other coefficients or content: U {doc_u['params']} on "
                          f"{doc_u['content']}, P {doc_p['params']} on {doc_p['content']}")
    out = {}
    for sid in sids:
        mu, mp = doc_u["margins"].get(sid), doc_p["margins"].get(sid)
        if mu is None or mp is None:
            raise CouldNotRun(f"T4: predfit gives {sid} no margin in {'U' if mu is None else 'P'}'s report")
        if mu != mp:
            raise CouldNotRun(f"T4: predfit gives {sid} the margin {mu} in U and {mp} in P: not one fight")
        out[sid] = mu
    return out


def predfit_errors(lab, m):
    """predfit's judge on labels `lab` (+1 A, -1 B, 0 not decisive): an error is a decisive fight whose margin is 0
    or of the other sign."""
    return (lab != 0) & ((m == 0) | ((m > 0) != (lab > 0)))


def t4(prs: dict, cols: dict, F, predfit_u=None, predfit_p=None) -> dict:
    import numpy

    def labels(rate):
        return numpy.where(rate > DECISIVE, 1, numpy.where(rate < 1 - DECISIVE, -1, 0))
    disagree, null, n_fights = [], numpy.zeros(len(F), numpy.int32), 0
    for sid, (block, both) in prs.items():
        if "winner" not in block["metrics"]:
            continue
        n_fights += 1
        ru, rp = win_rate([a for a, _b in both]), win_rate([b for _a, b in both])
        if label(ru) != label(rp):
            disagree.append(sid)
        wu = numpy.array([check.win_score(a["metrics"]["winner"]) for a, _b in both])
        wp = numpy.array([check.win_score(b["metrics"]["winner"]) for _a, b in both])
        ex = F[:, cols[sid]] < 0
        n = len(both)
        null += labels(numpy.where(ex, wp, wu).sum(axis=1) / n) != labels(numpy.where(ex, wu, wp).sum(axis=1) / n)
    limit = int(numpy.quantile(null, T4_Q, method="higher"))
    res = {"disagree": len(disagree), "fights": n_fights, "limit": limit, "which": disagree,
           "labels_pass": len(disagree) <= limit}
    if predfit_u is None or predfit_p is None:
        res["predfit"] = None
        res["pass"] = None
        return res
    sids = [sid for sid, (block, _both) in prs.items() if "winner" in block["metrics"]]
    margin = predfit_margins(json.loads(Path(predfit_u).read_text()), json.loads(Path(predfit_p).read_text()),
                             sids)
    e_u = e_p = 0
    d_null = numpy.zeros(len(F), numpy.int32)
    for sid in sids:
        both, m = prs[sid][1], margin[sid]
        wu = numpy.array([check.win_score(a["metrics"]["winner"]) for a, _b in both])
        wp = numpy.array([check.win_score(b["metrics"]["winner"]) for _a, b in both])
        n = len(both)
        e_u += int(predfit_errors(labels(numpy.array([wu.sum() / n])), m)[0])
        e_p += int(predfit_errors(labels(numpy.array([wp.sum() / n])), m)[0])
        ex = F[:, cols[sid]] < 0
        d_null += (predfit_errors(labels(numpy.where(ex, wu, wp).sum(axis=1) / n), m).astype(numpy.int32)
                   - predfit_errors(labels(numpy.where(ex, wp, wu).sum(axis=1) / n), m).astype(numpy.int32))
    plimit = int(numpy.quantile(numpy.abs(d_null), T4_Q, method="higher"))
    res["predfit"] = {"errors_u": e_u, "errors_p": e_p, "fights": len(sids), "limit": plimit,
                      "pass": abs(e_p - e_u) <= plimit}
    res["pass"] = res["labels_pass"] and res["predfit"]["pass"]
    return res


def run_refs(u: dict, p: dict, predfit_u=None, predfit_p=None, flips: int = SIGN_FLIPS) -> dict:
    """The four tests over two loaded arms, {sid: reference}; `flips` F's rows."""
    prs = pairs(u, p)
    n_clusters, cols = clusters(u, p, prs)
    F = flip_matrix(n_clusters, flips)
    fights = per_fight(prs, cols, F)
    return {"fights": len(prs), "clusters": n_clusters, "flips": flips, "T1": t1(u, prs, cols, F),
            "T2": t2(fights), "T3": t3(fights), "T4": t4(prs, cols, F, predfit_u, predfit_p)}


def run(u_root, p_root, predfit_u=None, predfit_p=None, flips: int = SIGN_FLIPS) -> dict:
    return run_refs(load(u_root), load(p_root), predfit_u, predfit_p, flips)


def verdict(res: dict) -> int:
    """0 pass, 1 a test failed, 2 T4 could not be completed."""
    failed = any(not r["pass"] for t in ("T1", "T2", "T3") for r in res[t].values())
    t4r = res["T4"]
    if failed or not t4r["labels_pass"] or t4r["pass"] is False:
        return 1
    return 2 if t4r["pass"] is None else 0


def report(res: dict, say=print):
    say(f"packcheck: {res['fights']} fights in both arms, {res['clusters']} clusters, {res['flips']} flips")
    for m, r in res["T1"].items():
        beside = f", Binomial {r['binomial']} at alpha {r['alpha']:.3f}" if r["binomial"] is not None else ""
        say(f"T1 {m:9s} OUT {r['out']}/{r['fights']} (limit {r['limit']}{beside}): "
            f"{'pass' if r['pass'] else 'FAIL'}")
    for m, r in res["T2"].items():
        say(f"T2 {m:9s} S {r['S']:+.3f} over {r['fights']} (|S| < {r['limit']:.3f}; var(S_b) {r['inflation']:.2f}, "
            f"normal {r['normal']}): {'pass' if r['pass'] else 'FAIL'}")
    for m, r in res["T3"].items():
        say(f"T3 {m:9s} p < {P_FIGHT} in {r['fights_below']}/{r['fights']} (limit {r['limit']}, "
            f"Binomial {r['binomial']}): {'pass' if r['pass'] else 'FAIL'}")
    t = res["T4"]
    say(f"T4 labels   {t['disagree']} disagree of {t['fights']} (limit {t['limit']}): "
        f"{'pass' if t['labels_pass'] else 'FAIL'}")
    if t["predfit"] is None:
        say("T4 predfit  not given (--predfit-u, --predfit-p): could not run")
    else:
        r = t["predfit"]
        say(f"T4 predfit  errors P {r['errors_p']}, U {r['errors_u']} of {r['fights']} "
            f"(|P - U| limit {r['limit']}): {'pass' if r['pass'] else 'FAIL'}")
    code = verdict(res)
    say("packcheck: " + ("PASS" if code == 0 else "FAIL" if code == 1 else "COULD NOT RUN (T4 incomplete)"))
    return code


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--u", required=True, help="the unpacked arm's reference folder")
    ap.add_argument("--p", required=True, help="the packed arm's reference folder")
    ap.add_argument("--predfit-u", help="predfit.py check --evaluation --json of U's references")
    ap.add_argument("--predfit-p", help="predfit.py check --evaluation --json of P's references")
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
