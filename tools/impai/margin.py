#!/usr/bin/env python3
"""margin.py -- the paired margin test: whether a candidate DLL wins its games against OTA AI faster than the
champion, by a sequential test on the same seeds.

    tools/impai/margin.py judge BASE TEST [--mode improve|notworse]
    tools/impai/margin.py run --base DLL --test DLL --mode improve|notworse --out DIR [--lanes L] [--blocks N]
    tools/impai/margin.py asn [--mode improve|notworse] [--width W] [--alpha A] [--runs R]

WHY. ImpAI wins every fair game against OTA AI, so a count of wins no longer tells two DLLs apart. The time a game
takes to win does, in every game, and on the same seed the two arms play the same game until their code differs.

THE GAMES. BASE is the champion, TEST the candidate; both play the same game ids, ImpAI against OTA AI at Hard,
through `bar.py play --dll` (the bar's ids, seeds, seating and results). The ids are ORDER: the bar's `hota` ids in
a fixed shuffled order (ORDER_SEED), then its `noh` ids so shuffled, cut at CAP_PAIRS; every run sees the same
sequence. They are played in blocks of BLOCK ids, both arms of a block together.

THE MEASURE of a seat-game is its time to win: the `end_tick` of a game ImpAI won, the cap for a game at the cap
(a draw at one look included), and none for a loss. A TEST loss is never averaged away: it fails the test
outright, as Hard's 100 of 100 rules. A pair's score: 1, TEST better, when its time is shorter than BASE's by more
than TIE_BAND ticks (300: a decision at Hard is 75-150 ticks, and identical code ends on the same tick), or BASE
lost and TEST did not; 0, worse, when longer by more than the band; otherwise -- inside the band, or both at the
cap -- 0.5, a tie.

THE PAIRING, by id and seed (as paired.py pairs): a TEST result pairs with the BASE result of the same id, and
each states the seed it played. A pair whose seeds differ, a result with no partner, an id twice in one arm, or a
game that could not run, and the test COULD NOT JUDGE. A result whose id is not in ORDER is not read.

THE SEQUENTIAL TEST is a GSPRT on the mean pair score s, the normal approximation engine-testing frameworks use:
over n pairs of mean m and variance v,

    LLR = n (s1 - s0) (2 m - s0 - s1) / (2 v)

against the bounds ln(beta / (1 - alpha)) and ln((1 - beta) / alpha), +-ln 19 at alpha = beta = 0.05.
    improve    H0 s = 0.50 against H1 s = 0.60: accept, TEST is better; reject, it is not measurably better.
    notworse   H0 s = 0.40 against H1 s = 0.50: accept, TEST is not worse (a fix batch's pass); reject, worse.
A tie scores 0.5 and stays in: it lowers the variance, so a run of ties is evidence the arms play alike rather
than no evidence. THE FLOOR: m and v are taken over the pairs and one pseudo-pair, half a pair better and half a
pair worse. It moves no mean and gives v the spread of one non-tied pair, so an all-tie run divides by no zero:
n ties read +-(s1 - s0)^2 (n + 1)^2 / 0.5, which at the hypotheses' width 0.10 first reaches the bound at 12 ties.
THE MINIMUM SAMPLE: the ratio is not read before block MIN_BLOCKS (2), since one block of 16 seeds is too few to
call a change not worse even when every game ties; one block of ties reads +-5.78 and two +-21.78.
The test is read after each whole block from the MIN_BLOCKS'th on, in ORDER, both arms' every game of it played:
the first block at which the ratio reaches a bound decides, and the blocks after it are not read. With every block
of ORDER read and no bound reached, the verdict is NO DECISION. The verdict is a function of the games' results
alone.

REPORTED, not judged: the paired mean of ln(end_tick), TEST - BASE, over the pairs both arms won; and with the
results' `values` (`bar.py play --values`, which `run` asks at VALUE_TIMES), the paired mean of ln(value A /
value B) at each GameTime both games reached.

`run` copies both DLLs into DIR first, so a DLL rebuilt elsewhere mid-run changes no game, and refuses a DIR whose
copies differ from the DLLs it is given. It then plays block after block -- one `bar.py play` an arm, both at
once, each under the run lock with L lanes (at most MAX_LANES: a GDI lane weighs 2 of the lock's 28 slots, so
two arms of 7 fill it) -- judges after each, and stops at a verdict, at the end of ORDER, or after its first N
blocks (`--blocks`: a run in pieces, each resumed by the next, the games played skipped). A block with a game that
could not run is played once more (bar.py's resume plays only those). It registers one runs-viewer run, a step a
block (tools/tarun.py), whose id the games inherit (TARUN_RUN).

`asn` simulates the test (seeded): for each true mean score and tie rate, the share of runs that accept, reject
or reach CAP_PAIRS with no decision, the mean pairs played under the cap, and the mean pairs the open-ended test
would play. --width and --alpha set other hypotheses (s1 - s0, and alpha = beta) to price them.

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
MODES = {"improve": (0.50, 0.60), "notworse": (0.40, 0.50)}
VERDICTS = {"improve": ("TEST is better", "TEST is not measurably better"),
            "notworse": ("TEST is not worse", "TEST is worse")}
PSEUDO = 0.5                # THE FLOOR: the weight of the pseudo-pair's better half, and of its worse half
MIN_BLOCKS = 2              # THE MINIMUM SAMPLE: the ratio is not read before this block
VALUE_TIMES = (15000, 30000)
MAX_LANES = 7
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


def bounds(alpha: float = ALPHA, beta: float = BETA) -> tuple:
    """(lower, upper) of the log-likelihood ratio: ln(beta / (1 - alpha)), ln((1 - beta) / alpha)"""
    return math.log(beta / (1 - alpha)), math.log((1 - beta) / alpha)


def llr(better: int, ties: int, worse: int, s0: float, s1: float) -> float:
    """THE SEQUENTIAL TEST's ratio over these counts, with THE FLOOR's pseudo-pair"""
    b, d, w = better + PSEUDO, ties, worse + PSEUDO
    n = b + d + w
    m = (b + 0.5 * d) / n
    v = (b * (1 - m) ** 2 + d * (0.5 - m) ** 2 + w * m ** 2) / n
    return n * (s1 - s0) * (2 * m - s0 - s1) / (2 * v)


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
    "better", "worse", "ties", "mean" score, "llr", "trace" [(block, llr)], "losses" (TEST's), "unread" pairs}.
    CouldNotJudge as THE PAIRING says."""
    ps = pairs(base, test)
    lo, hi = bounds()
    s0, s1 = MODES[mode]
    out = {"mode": mode, "bounds": (lo, hi), "better": 0, "worse": 0, "ties": 0, "llr": 0.0, "mean": None,
           "trace": [], "blocks": 0, "pairs": 0, "read": [],
           "losses": sorted(g for g, (_b, t) in ps.items() if t.get("outcome") == "B")}
    if out["losses"]:
        out.update(exit=REJECT, verdict=f"TEST lost {len(out['losses'])} game(s): a loss fails the test outright",
                   unread=[])
        return out
    decided = None
    for k, ids in enumerate(blocks(), 1):
        if not all(g in ps for g in ids):
            break
        for g in ids:
            b, t = ps[g]
            s = sign(time_to_win(b), time_to_win(t))
            out["better" if s > 0 else "worse" if s < 0 else "ties"] += 1
            out["read"].append(g)
        out["blocks"], out["pairs"] = k, out["pairs"] + len(ids)
        out["llr"] = llr(out["better"], out["ties"], out["worse"], s0, s1)
        out["mean"] = (out["better"] + 0.5 * out["ties"]) / out["pairs"]
        out["trace"].append((k, out["llr"]))
        if k >= MIN_BLOCKS and (out["llr"] >= hi or out["llr"] <= lo):
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
    s0, s1 = MODES[mode]
    print(f"margin judge ({mode}: H0 s = {s0}, H1 s = {s1}, alpha = beta = {ALPHA}; tie band {TIE_BAND} ticks)")
    print(f"  {v['blocks']} blocks read, {v['pairs']} pairs: TEST better {v['better']}, worse {v['worse']}, "
          f"ties {v['ties']}" + (f"; mean score {v['mean']:.4f}" if v["mean"] is not None else ""))
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


# ---- the expected cost (simulated) ----

OPEN_LIMIT = 4000           # the open-ended test's simulated runs stop here, counted at the limit


def simulate(mean: float, tie: float, s0: float, s1: float, alpha: float, runs: int, rng) -> dict:
    """The test on `runs` simulated runs whose pairs score 1, 0.5 and 0 with chances mean - tie/2, tie and
    1 - mean - tie/2, read after each block as `verdict` reads: {"accept", "reject", "none"} shares at CAP_PAIRS,
    "pairs" (the mean played under the cap), "open" (the mean the open-ended test plays) and "open_none" (the share
    still undecided at OPEN_LIMIT)."""
    pb, pt = mean - tie / 2, tie
    lo, hi = bounds(alpha, alpha)
    sizes = [len(b) for b in blocks()]
    sizes += [BLOCK] * ((OPEN_LIMIT - sum(sizes)) // BLOCK)
    tally = {"accept": 0, "reject": 0, "none": 0, "pairs": 0, "open": 0, "open_none": 0}
    for _ in range(runs):
        c, n, capped = [0, 0, 0], 0, None
        for k, size in enumerate(sizes, 1):
            for _ in range(size):
                u = rng.random()
                c[0 if u < pb else 1 if u < pb + pt else 2] += 1
            n += size
            x = llr(c[0], c[1], c[2], s0, s1)
            done = None if k < MIN_BLOCKS else "accept" if x >= hi else "reject" if x <= lo else None
            if capped is None and (done or n >= CAP_PAIRS):
                capped = done or "none"
                tally[capped] += 1
                tally["pairs"] += n
            if done:
                break
        else:
            tally["open_none"] += 1
        tally["open"] += n
    return {k: v / runs for k, v in tally.items()}


def asn_rows(mode: str, width: float = None, alpha: float = ALPHA, runs: int = 1000, seed: int = 1) -> list:
    """[(mean, tie, simulate's dict)] over true means 0.400-0.600 by 0.025 and tie rates 0, 0.3, 0.6, 0.9, where
    the chances exist; the mode's hypotheses, or with `width` s1 - s0 = width about the mode's shared edge 0.5."""
    s0, s1 = MODES[mode]
    if width is not None:
        s0, s1 = (0.5, 0.5 + width) if mode == "improve" else (0.5 - width, 0.5)
    rng = random.Random(seed)
    rows = []
    for i in range(9):
        mean = 0.4 + 0.025 * i
        for tie in (0.0, 0.3, 0.6, 0.9):
            if mean - tie / 2 >= -1e-12 and 1 - mean - tie / 2 >= -1e-12:
                rows.append((mean, tie, simulate(mean, tie, s0, s1, alpha, runs, rng)))
    return rows


def cmd_asn(mode: str, width: float, alpha: float, runs: int) -> int:
    s0, s1 = MODES[mode]
    if width is not None:
        s0, s1 = (0.5, 0.5 + width) if mode == "improve" else (0.5 - width, 0.5)
    print(f"{mode}: H0 s = {s0:.3f}, H1 s = {s1:.3f}, alpha = beta = {alpha}; {runs} simulated runs a row, "
          f"blocks of {BLOCK}, cap {CAP_PAIRS} pairs")
    print("  mean   tie   accept  reject  none@cap  pairs(capped)  pairs(open)")
    for mean, tie, r in asn_rows(mode, width, alpha, runs):
        print(f"  {mean:.3f}  {tie:.1f}   {r['accept']:.3f}   {r['reject']:.3f}   {r['none']:.3f}     "
              f"{r['pairs']:6.0f}        {r['open']:6.0f}" + (f"  ({r['open_none']:.3f} open at {OPEN_LIMIT})"
                                                               if r["open_none"] else ""))
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
    r.add_argument("--lanes", type=int, default=6, choices=range(1, MAX_LANES + 1), metavar=f"1..{MAX_LANES}",
                   help="lanes an arm (a GDI lane weighs 2 of the lock's 28 slots; both arms at once)")
    r.add_argument("--blocks", type=int, default=None,
                   help="play the order's first N blocks at most: a run in pieces, each resumed by the next "
                        "(the games already played are skipped)")
    s = sub.add_parser("asn", help="the expected cost by true mean score and tie rate (simulated)")
    s.add_argument("--width", type=float, default=None, help="s1 - s0 about 0.5 (default: the mode's 0.10)")
    s.add_argument("--alpha", type=float, default=ALPHA, help="alpha = beta")
    s.add_argument("--runs", type=int, default=1000, help="simulated runs a row")
    for x in (j, s):
        x.add_argument("--mode", choices=sorted(MODES), default="improve")
    r.add_argument("--mode", choices=sorted(MODES), required=True)
    a = p.parse_args(argv)
    if a.verb == "judge":
        return judge(a.base, a.test, a.mode)
    if a.verb == "asn":
        return cmd_asn(a.mode, a.width, a.alpha, a.runs)
    return run(a)


if __name__ == "__main__":
    sys.exit(main())
