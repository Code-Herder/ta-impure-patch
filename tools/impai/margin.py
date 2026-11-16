#!/usr/bin/env python3
"""margin.py -- the paired margin test: whether a candidate DLL wins its games against OTA AI faster than the
champion, by a sequential test on the same seeds.

    tools/impai/margin.py judge BASE TEST [--mode improve|notworse]
    tools/impai/margin.py run --base DLL --test DLL --mode improve|notworse --out DIR [--lanes L] [--blocks N]
    tools/impai/margin.py asn [--mode improve|notworse]

WHY. ImpAI wins every fair game against OTA AI, so a count of wins no longer tells two DLLs apart. The time a game
takes to win does, in every game, and on the same seed the two arms play the same game until their code differs.

THE GAMES. BASE is the champion, TEST the candidate; both play the same game ids, ImpAI against OTA AI at Hard,
through `bar.py play --dll` (the bar's ids, seeds, seating and results). The ids are ORDER: the bar's `hota` ids in
a fixed shuffled order (ORDER_SEED), then its `noh` ids so shuffled, cut at CAP_PAIRS; every run sees the same
sequence. They are played in blocks of BLOCK ids, both arms of a block together.

THE MEASURE of a seat-game is its time to win: the `end_tick` of a game ImpAI won, the cap for a game at the cap
(a draw at one look included), and none for a loss. A TEST loss is never averaged away: it fails the test
outright, as Hard's 100 of 100 rules. A pair's sign: TEST better when its time is shorter than BASE's by more
than TIE_BAND ticks (300: a decision at Hard is 75-150 ticks, and identical code ends on the same tick), or BASE
lost and TEST did not; worse when longer by more than the band; otherwise -- inside the band, or both at the cap
-- a tie, which is dropped.

THE PAIRING, by id and seed (as paired.py pairs): a TEST result pairs with the BASE result of the same id, and
each states the seed it played. A pair whose seeds differ, a result with no partner, an id twice in one arm, or a
game that could not run, and the test COULD NOT JUDGE. A result whose id is not in ORDER is not read.

THE SEQUENTIAL TEST is Wald's SPRT on the sign of the non-tied pairs, a binomial in p = P(TEST better), alpha =
beta = 0.05: the log-likelihood ratio of H1 to H0 against the bounds ln(beta / (1 - alpha)) and ln((1 - beta) /
alpha), +-ln 19.
    improve    H0 p = 0.5 against H1 p = 0.6: accept, TEST is better; reject, it is not measurably better.
    notworse   H0 p = 0.4 against H1 p = 0.5: accept, TEST is not worse (a fix batch's pass); reject, worse.
The test is read after each whole block, in ORDER, both arms' every game of it played: the first block at which
the ratio reaches a bound decides, and the blocks after it are not read. With every block of ORDER read and no
bound reached, the verdict is NO DECISION. The verdict is a function of the games' results alone.

REPORTED, not judged: the paired mean of ln(end_tick), TEST - BASE, over the pairs both arms won; and with the
results' `values` (`bar.py play --values`, which `run` asks at VALUE_TIMES), the paired mean of ln(value A /
value B) at each GameTime both games reached.

`run` copies both DLLs into DIR first, so a DLL rebuilt elsewhere mid-run changes no game, and refuses a DIR whose
copies differ from the DLLs it is given. It then plays block after block -- one `bar.py play` an arm, both at
once, each under the run lock with L lanes -- judges after each, and stops at a verdict, at the end of ORDER, or
after its first N blocks (`--blocks`: a run in pieces, each resumed by the next, the games played skipped). A
block with a game that could not run is played once more (bar.py's resume plays only those). It registers one
runs-viewer run, a step a block (tools/tarun.py), whose id the games inherit (TARUN_RUN).

`asn` prints the SPRT's operating characteristic and expected number of non-tied pairs (Wald's ASN) by true p.

Exit 0 accept, 1 reject or a TEST loss, 2 could not judge (or run), 3 no decision (yet, or at the end of ORDER)."""
import argparse
import hashlib
import json
import math
import os
import random
import shutil
import signal
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(TREE / "tools"))
import bar  # noqa: E402

TIE_BAND = 300
BLOCK = 16
CAP_PAIRS = 200
ORDER_SEED = 20261115
ALPHA = BETA = 0.05
MODES = {"improve": (0.5, 0.6), "notworse": (0.4, 0.5)}
VERDICTS = {"improve": ("TEST is better", "TEST is not measurably better"),
            "notworse": ("TEST is not worse", "TEST is worse")}
VALUE_TIMES = (15000, 30000)
ACCEPT, REJECT, COULD_NOT, UNDECIDED = 0, 1, 2, 3


class CouldNotJudge(Exception):
    pass


def order() -> list:
    """ORDER: the bar's hota ids shuffled by ORDER_SEED, then its noh ids shuffled by the next draws, CAP_PAIRS of
    them."""
    rng = random.Random(ORDER_SEED)
    out = []
    for pair in ("hota", "noh"):
        ids = list(bar.ids(pair))
        rng.shuffle(ids)
        out += ids
    return out[:CAP_PAIRS]


def blocks() -> list:
    o = order()
    return [o[i:i + BLOCK] for i in range(0, len(o), BLOCK)]


def bounds() -> tuple:
    """(lower, upper) of the log-likelihood ratio: ln(beta / (1 - alpha)), ln((1 - beta) / alpha)"""
    return math.log(BETA / (1 - ALPHA)), math.log((1 - BETA) / ALPHA)


def steps(mode: str) -> tuple:
    """(the ratio's step for a pair TEST won, for one TEST lost): ln(p1 / p0), ln((1 - p1) / (1 - p0))"""
    p0, p1 = MODES[mode]
    return math.log(p1 / p0), math.log((1 - p1) / (1 - p0))


def time_to_win(r: dict):
    """THE MEASURE: end_tick of a game A won, the cap of a game at the cap, None for a loss."""
    o = r.get("outcome")
    if o == "A":
        return int(r["end_tick"])
    if o == "cap":
        return int(r.get("cap", bar.CAP))
    if o == "B":
        return None
    raise CouldNotJudge(f"{r.get('id')}: {o}: {r.get('reason', '')}")


def sign(base, test) -> int:
    """+1 TEST better, -1 worse, 0 a tie (THE MEASURE); a TEST loss is the caller's to rule on."""
    if base is None:
        return 0 if test is None else 1
    if test is None:
        return -1
    d = base - test
    return 1 if d > TIE_BAND else -1 if d < -TIE_BAND else 0


def read_arm(root: Path) -> dict:
    """{id: result} of the bar results under `root`; an id twice is refused."""
    out = {}
    for p in sorted(Path(root).rglob("bar-*.json")):
        if p.name.endswith(".scenario.json"):
            continue
        r = json.loads(p.read_text())
        if not isinstance(r, dict) or "id" not in r:
            continue
        if r["id"] in out:
            raise CouldNotJudge(f"{r['id']} twice under {root}: {out[r['id']]['_path']} and {p}")
        r["_path"] = str(p)
        out[r["id"]] = r
    return out


def pairs(base: dict, test: dict) -> dict:
    """{id: (BASE result, TEST result)} of ORDER's ids either arm holds (THE PAIRING)."""
    want = set(order())
    out = {}
    for gid in sorted((set(base) | set(test)) & want):
        if gid not in base or gid not in test:
            raise CouldNotJudge(f"{gid} has no partner in {'BASE' if gid not in base else 'TEST'}")
        b, t = base[gid], test[gid]
        if b.get("seed") is None or b.get("seed") != t.get("seed"):
            raise CouldNotJudge(f"{gid}: the arms played seeds {b.get('seed')} and {t.get('seed')}")
        for arm, r in (("BASE", b), ("TEST", t)):
            if r.get("outcome") in (None, "could_not_run"):
                raise CouldNotJudge(f"{gid}: {arm}'s game could not run: {r.get('reason', '')}")
        out[gid] = (b, t)
    return out


def verdict(base: dict, test: dict, mode: str) -> dict:
    """The test over two arms' results (THE SEQUENTIAL TEST): {"exit", "verdict", "blocks" read, "pairs" read,
    "better", "worse", "ties", "llr", "trace" [(block, llr)], "losses" (TEST's), "unread" blocks}. CouldNotJudge
    as THE PAIRING says."""
    ps = pairs(base, test)
    lo, hi = bounds()
    up, down = steps(mode)
    out = {"mode": mode, "bounds": (lo, hi), "better": 0, "worse": 0, "ties": 0, "llr": 0.0, "trace": [],
           "blocks": 0, "pairs": 0, "losses": sorted(g for g, (_b, t) in ps.items() if t.get("outcome") == "B"),
           "read": []}
    if out["losses"]:
        out.update(exit=REJECT, verdict=f"TEST lost {len(out['losses'])} game(s): a loss fails the test outright")
        return out
    decided = None
    for k, ids in enumerate(blocks(), 1):
        if not all(g in ps for g in ids):
            break
        for g in ids:
            b, t = ps[g]
            s = sign(time_to_win(b), time_to_win(t))
            out["better" if s > 0 else "worse" if s < 0 else "ties"] += 1
            out["llr"] += up if s > 0 else down if s < 0 else 0.0
            out["read"].append(g)
        out["blocks"], out["pairs"] = k, out["pairs"] + len(ids)
        out["trace"].append((k, out["llr"]))
        if out["llr"] >= hi or out["llr"] <= lo:
            decided = out["llr"] >= hi
            break
    out["unread"] = sorted(set(ps) - set(out["read"]))
    if decided is not None:
        out.update(exit=ACCEPT if decided else REJECT, verdict=VERDICTS[mode][0 if decided else 1])
    elif out["blocks"] == len(blocks()):
        out.update(exit=UNDECIDED, verdict=f"NO DECISION: every block of the order read ({out['pairs']} pairs)")
    else:
        out.update(exit=UNDECIDED, verdict=f"no decision yet after {out['blocks']} blocks")
    return out


def reported(base: dict, test: dict, ids) -> dict:
    """REPORTED: the paired mean of ln(end_tick) over the pairs both arms won, and of ln(value A / value B) at each
    GameTime both games' `values` hold; each {"n", "mean"} TEST - BASE."""
    out = {}
    d = [math.log(test[g]["end_tick"]) - math.log(base[g]["end_tick"]) for g in ids
         if base[g].get("outcome") == "A" and test[g].get("outcome") == "A"]
    out["ln_end_tick"] = {"n": len(d), "mean": sum(d) / len(d) if d else None}

    def ratio(r, t):
        v = (r.get("values") or {}).get(str(t))
        if not v or v["A"]["live"] <= 0 or v["B"]["live"] <= 0:
            return None
        return math.log(v["A"]["live"] / v["B"]["live"])
    for t in VALUE_TIMES:
        d = [ratio(test[g], t) - ratio(base[g], t) for g in ids
             if ratio(test[g], t) is not None and ratio(base[g], t) is not None]
        out[f"ln_value_ratio_{t}"] = {"n": len(d), "mean": sum(d) / len(d) if d else None}
    return out


def judge(base_dir: Path, test_dir: Path, mode: str) -> int:
    try:
        base, test = read_arm(base_dir), read_arm(test_dir)
        v = verdict(base, test, mode)
    except CouldNotJudge as e:
        print(f"margin judge: COULD NOT JUDGE: {e}")
        return COULD_NOT
    p0, p1 = MODES[mode]
    print(f"margin judge ({mode}: H0 p = {p0}, H1 p = {p1}, alpha = beta = {ALPHA}; tie band {TIE_BAND} ticks)")
    print(f"  {v['blocks']} blocks read, {v['pairs']} pairs: TEST better {v['better']}, worse {v['worse']}, "
          f"ties {v['ties']}")
    print(f"  log-likelihood ratio {v['llr']:+.3f}, bounds [{v['bounds'][0]:+.3f}, {v['bounds'][1]:+.3f}]; by block: "
          + " ".join(f"{k}:{x:+.2f}" for k, x in v["trace"]))
    if v.get("unread"):
        print(f"  not read (after the decision, or past an incomplete block): {len(v['unread'])} pairs")
    if v["read"]:
        for name, x in reported(base, test, v["read"]).items():
            print(f"  reported, TEST - BASE: {name}: " + (f"{x['mean']:+.4f} over {x['n']} pairs" if x["n"] else
                                                          "no pair"))
    print(f"margin judge: {v['verdict']}")
    return v["exit"]


# ---- the expected cost (Wald) ----

def oc_asn(p: float, mode: str) -> tuple:
    """(L(p), E[N]): the probability the SPRT accepts H0 at a true p, and its expected number of non-tied pairs,
    by Wald's approximations: L = (A^h - 1) / (A^h - B^h) with h != 0 the root of p a^h + (1 - p) b^h = 1, and E[N]
    = (L ln B + (1 - L) ln A) / E[z], or -ln A ln B / E[z^2] where E[z] = 0."""
    up, down = steps(mode)
    lo, hi = bounds()
    ez = p * up + (1 - p) * down
    if abs(ez) < 1e-12:
        return hi / (hi - lo), -lo * hi / (p * up * up + (1 - p) * down * down)

    def f(h):
        return p * math.exp(h * up) + (1 - p) * math.exp(h * down) - 1
    a, b = (1e-9, 60.0) if ez < 0 else (-60.0, -1e-9)
    for _ in range(200):
        m = (a + b) / 2
        if (f(a) < 0) == (f(m) < 0):
            a = m
        else:
            b = m
    h = (a + b) / 2
    L = (math.exp(h * hi) - 1) / (math.exp(h * hi) - math.exp(h * lo))
    return L, (L * lo + (1 - L) * hi) / ez


def cmd_asn(mode: str) -> int:
    print(f"{mode}: true p, P(accept H1), expected non-tied pairs (Wald)")
    for p in (0.3, 0.35, 0.4, 0.45, 0.5, 0.55, 0.6, 0.65, 0.7, 0.8):
        L, n = oc_asn(p, mode)
        print(f"  p {p:.2f}: accept {1 - L:.3f}, pairs {n:.0f}")
    return 0


# ---- the run ----

def md5(path: Path) -> str:
    return hashlib.md5(Path(path).read_bytes()).hexdigest()


def fix_dlls(out: Path, base: Path, test: Path) -> dict:
    """{arm: the DLL copied into out}: copied at the first run, refused when a copy differs from the DLL given."""
    d = out / "dll"
    d.mkdir(parents=True, exist_ok=True)
    got = {}
    for arm, src in (("base", base), ("test", test)):
        dst = d / f"{arm}.dll"
        if dst.exists():
            if md5(dst) != md5(src):
                raise CouldNotJudge(f"{dst} is another DLL than {src}: a run's arms are fixed at its first block")
        else:
            shutil.copyfile(src, dst)
        got[arm] = dst
    return got


def play_block(k: int, ids: list, dlls: dict, out: Path, lanes: int) -> int:
    """Both arms of block k, one `bar.py play` each, at once: the worse exit code."""
    f = out / "blocks" / f"block-{k:02d}.ids"
    f.parent.mkdir(parents=True, exist_ok=True)
    f.write_text("\n".join(ids) + "\n")
    procs = []
    try:
        for arm in ("base", "test"):
            procs.append(subprocess.Popen(
                [sys.executable, str(HERE / "bar.py"), "play", "--ids", str(f), "--lanes", str(lanes),
                 "--dll", str(dlls[arm]), "--out", str(out / arm),
                 "--values", ",".join(str(t) for t in VALUE_TIMES)], cwd=str(TREE)))
        return max(p.wait() for p in procs)
    except KeyboardInterrupt:
        for p in procs:
            if p.poll() is None:
                p.send_signal(signal.SIGINT)        # bar.py's stop: no game starts, each one playing is stopped
        for p in procs:
            p.wait()
        raise


def run(a, play=play_block) -> int:
    import tarun
    out = Path(a.out)
    try:
        dlls = fix_dlls(out, Path(a.base), Path(a.test))
    except (CouldNotJudge, OSError) as e:
        print(f"margin run: COULD NOT RUN: {e}")
        return COULD_NOT
    bs = blocks()[:a.blocks] if getattr(a, "blocks", None) else blocks()
    jr = tarun.begin("margin", [("margin", [{"id": f"block {k}"} for k in range(1, len(bs) + 1)])],
                     title=f"margin {a.mode} {out.name}", gate=True)
    rc, k = UNDECIDED, 0
    try:
        for k, ids in enumerate(bs, 1):
            jr.start("margin", f"block {k}")
            for attempt in (1, 2):
                prc = play(k, ids, dlls, out, a.lanes)
                rc = judge(out / "base", out / "test", a.mode)
                if rc != COULD_NOT or attempt == 2 or prc == 0:
                    break
                print(f"margin run: block {k} again, for its games that could not run")
            jr.finish("margin", f"block {k}", {ACCEPT: "pass", REJECT: "fail", COULD_NOT: "unrun"}.get(rc, "done"))
            if rc != UNDECIDED:
                break
    except KeyboardInterrupt:
        jr.finish("margin", f"block {k}", "unrun", "stopped")
        jr.end("stopped")
        return COULD_NOT
    for j in range(k + 1, len(bs) + 1):
        jr.finish("margin", f"block {j}", "skipped")
    jr.end({ACCEPT: "pass", REJECT: "fail"}.get(rc, "unrun"))         # no decision is no verdict
    return rc


def main(argv=None) -> int:
    # The players write to the same log as their own processes: line buffering keeps each verdict beside its block.
    sys.stdout.reconfigure(line_buffering=True)
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="verb", required=True)
    j = sub.add_parser("judge", help="the test over two arms' results")
    j.add_argument("base", type=Path)
    j.add_argument("test", type=Path)
    r = sub.add_parser("run", help="play blocks until the test decides")
    r.add_argument("--base", required=True)
    r.add_argument("--test", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--lanes", type=int, default=4, help="lanes an arm (2 slots a lane; both arms at once)")
    r.add_argument("--blocks", type=int, default=None,
                   help="play the order's first N blocks at most: a run in pieces, each resumed by the next "
                        "(the games already played are skipped)")
    s = sub.add_parser("asn", help="the expected cost by true p")
    for x in (j, s):
        x.add_argument("--mode", choices=sorted(MODES), default="improve")
    r.add_argument("--mode", choices=sorted(MODES), required=True)
    a = p.parse_args(argv)
    if a.verb == "judge":
        return judge(a.base, a.test, a.mode)
    if a.verb == "asn":
        return cmd_asn(a.mode)
    return run(a)


if __name__ == "__main__":
    sys.exit(main())
