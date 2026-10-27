#!/usr/bin/env python3
"""sweeptest.py -- build tests/sweeptest.c with the host's gcc and check tagpu_sweepgeo.c.

    tagpu/ddraw/tests/sweeptest.py

tagpu_sweepgeo.c is engine-free and Win32-free, so it builds natively, with AddressSanitizer and
UndefinedBehaviorSanitizer, and needs no Wine. The cases and their answers are here: ring
membership on and off the circle against exact integer arithmetic, the target order's tie rule,
the Reclaim handler's ticks, and the split against `order_sweep`, a port of the owner's mockup
(the reclaim UI mockup's reclaim-sweep.js, `orderSweep`), on fixed tables and seeded random
cases. tools/sweep_regression.py imports `order_sweep` and `reclaim_ticks` for its expectations.
Exit 0 when nothing failed."""
import math
import random
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DDRAW = HERE.parent
EXE = HERE / "sweeptest-native.exe"   # `.exe` so the tree's *.exe ignore covers it
SRCS = [HERE / "sweeptest.c", DDRAW / "src/tagpu_sweepgeo.c", DDRAW / "src/tagpu_sweepgeo.h"]
MAX_TARGETS = 1024                    # SWEEP_MAX_TARGETS


# -- the rules, in Python ---------------------------------------------------------------------

def inside(cx, cz, r, x, z):
    return r >= 0 and (x - cx) ** 2 + (z - cz) ** 2 <= r * r


def order(targets, cx, cz):
    """sweep_order: nearest the centre first, ties by z, then x, then index."""
    return sorted(range(len(targets)),
                  key=lambda i: ((targets[i][0] - cx) ** 2 + (targets[i][1] - cz) ** 2,
                                 targets[i][1], targets[i][0], i))


def reclaim_ticks(energy, metal):
    """The Reclaim handler's countdown, 15 + (energy + metal) / 2, at 2 a working call, rounded
    up (sweep_reclaim_ticks; 0x404BBA..0x404BDC and 0x404C7C). Negative or NaN counts as 0."""
    e = energy if energy > 0 else 0.0
    m = metal if metal > 0 else 0.0
    return int(math.ceil((15.0 + (e + m) / 2.0) / 2.0))


def order_sweep(builders, targets):
    """The mockup's orderSweep, with its REACH, `u.speed` and valueTime replaced by the builder's
    `reach`, `speed` and the target's `ticks`. Its loop, spelled the same way: while targets are
    left, the builder with the least time (`s.t`, here `load`) takes the target nearest its point,
    then moves there and its time grows by max(0, d - reach) / speed + the target's time.

    Three rules are the DLL's, where the mockup has none: a builder takes part only while it has
    `room`, and one with speed <= 0 only while a target lies within `reach` of it (it then
    travels 0); and ties go to the lowest index, for the builder as for the target -- the mockup
    re-sorts its builders by time each pass, and JavaScript's stable sort leaves a tie to the
    previous pass's order. Distances are compared as exact squared integers and d is its square
    root, as in tagpu_sweepgeo.c, where the mockup takes Math.hypot.

    builders: [x, z, speed, reach, load, room]; targets: (x, z, ticks). Returns (pairs,
    builders after), pairs the (builder, target) assignments in the order they were made."""
    st = [list(b) for b in builders]
    left = list(range(len(targets)))
    pairs = []

    def d2(s, k):
        return (targets[k][0] - s[0]) ** 2 + (targets[k][1] - s[1]) ** 2

    while left:
        able = [i for i, s in enumerate(st) if s[5] > 0 and
                (s[2] > 0 or any(math.sqrt(float(d2(s, k))) <= s[3] for k in left))]
        if not able:
            break
        i = min(able, key=lambda i: (st[i][4], i))
        s = st[i]
        k = min(left, key=lambda k: (d2(s, k), k))
        d = math.sqrt(float(d2(s, k)))
        travel = ((d - s[3]) / s[2] if d > s[3] else 0.0) if s[2] > 0 else 0.0
        s[4] += travel + targets[k][2]
        s[0], s[1] = targets[k][0], targets[k][1]
        s[5] -= 1
        left.remove(k)
        pairs.append((i, k))
    return pairs, st


# -- the native side --------------------------------------------------------------------------

def build():
    if EXE.exists() and all(EXE.stat().st_mtime >= s.stat().st_mtime for s in SRCS):
        return
    print("sweeptest: building", EXE.name, flush=True)
    subprocess.run(["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-Isrc", "-O1", "-g",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-o", str(EXE), "tests/sweeptest.c", "src/tagpu_sweepgeo.c", "-lm"],
                   cwd=DDRAW, check=True)


def native(lines):
    r = subprocess.run([str(EXE)], input="".join(ln + "\n" for ln in lines), capture_output=True,
                       text=True, timeout=600)
    if r.returncode:
        raise SystemExit(f"sweeptest: the driver failed ({r.returncode}): {r.stderr.strip()}")
    out = r.stdout.splitlines()
    if len(out) != len(lines):
        raise SystemExit(f"sweeptest: the driver answered {len(out)} of {len(lines)} cases")
    return out


def split_line(builders, targets):
    return (f"split {len(builders)} {len(targets)} " +
            " ".join(f"{x} {z} {sp!r} {re!r} {ld!r} {rm}" for x, z, sp, re, ld, rm in builders) +
            " " + " ".join(f"{x} {z} {t}" for x, z, t in targets))


def parse_split(text):
    head, *rest = text.split(" | ")
    words = head.split()
    m = int(words[0])
    pairs = [tuple(map(int, w.split(":"))) for w in words[1:]]
    after = []
    for b in rest:
        x, z, load, room = b.split()
        after.append([int(x), int(z), float(load), int(room)])
    return m, pairs, after


# -- the cases --------------------------------------------------------------------------------

class Tally:
    def __init__(self):
        self.checks = self.failed = 0

    def expect(self, ok, what):
        self.checks += 1
        if not ok:
            self.failed += 1
            if self.failed <= 30:
                print(f"FAIL {what}")


def membership_cases(rng):
    cases = []
    # exactly on the circle (Pythagorean triples scaled), one past it, one short of it
    for a, b, c in ((3, 4, 5), (5, 12, 13), (8, 15, 17), (7, 24, 25), (20, 21, 29)):
        for k in (1, 7, 100, 4096 // c):
            r = c * k
            for sx, sz in ((1, 1), (-1, 1), (1, -1), (-1, -1)):
                cx, cz = rng.randint(-2 ** 20, 2 ** 20), rng.randint(-2 ** 20, 2 ** 20)
                cases.append((cx, cz, r, cx + sx * a * k, cz + sz * b * k))
                cases.append((cx, cz, r, cx + sx * a * k + sx, cz + sz * b * k))
                cases.append((cx, cz, r - 1, cx + sx * a * k, cz + sz * b * k))
    for r in (0, 1, 4095, 4096):
        cases.append((0, 0, r, r, 0))
        cases.append((0, 0, r, r + 1, 0))
        cases.append((0, 0, r, 0, -r))
    cases.append((0, 0, -1, 0, 0))
    # the int32 extremes, where an unguarded product overflows
    big = 2 ** 31 - 1
    cases += [(-big - 1, -big - 1, 4096, big, big), (big, big, 4096, big - 4000, big),
              (-big - 1, 0, 4096, -big - 1 + 4096, 0), (0, -big - 1, 4096, 0, -big - 1 + 4097)]
    for _ in range(4000):
        cx, cz = rng.randint(-2 ** 31, 2 ** 31 - 1), rng.randint(-2 ** 31, 2 ** 31 - 1)
        r = rng.randint(0, 4096)
        x = max(-2 ** 31, min(2 ** 31 - 1, cx + rng.randint(-5000, 5000)))
        z = max(-2 ** 31, min(2 ** 31 - 1, cz + rng.randint(-5000, 5000)))
        cases.append((cx, cz, r, x, z))
    return cases


def order_cases(rng):
    cases = []
    # every tie at once: one ring of equal distances, equal z on both sides, repeated points
    ring = [(5, 0), (-5, 0), (0, 5), (0, -5), (3, 4), (-3, 4), (3, -4), (-3, -4), (4, 3), (-4, 3),
            (4, -3), (-4, -3), (3, 4), (0, 0), (0, 0)]
    cases.append((0, 0, ring))
    cases.append((100, -50, [(100 + x, -50 + z) for x, z in ring[::-1]]))
    for _ in range(300):
        n = rng.randint(0, 60)
        span = rng.choice([3, 20, 3000])
        cx, cz = rng.randint(-5000, 5000), rng.randint(-5000, 5000)
        cases.append((cx, cz, [(cx + rng.randint(-span, span), cz + rng.randint(-span, span))
                               for _ in range(n)]))
    cases.append((0, 0, [(rng.randint(-4096, 4096), rng.randint(-4096, 4096))
                         for _ in range(MAX_TARGETS)]))
    return cases


def fixed_split_cases():
    cases = []
    for nb in (1, 2, 3, 7):
        for nt in range(1, 41):
            builders = [[100 * i, 50 * (i % 2), 1.0 + 0.25 * i, 46.0 + 8 * i, 0.0, 1024]
                        for i in range(nb)]
            targets = [(37 * k % 400, 23 * k % 300, 8 + 3 * k) for k in range(nt)]
            cases.append(("fixed", builders, targets))
    # equal loads and equal distances: every choice is a tie
    cases.append(("equal loads", [[0, 0, 1.0, 46.0, 0.0, 1024] for _ in range(4)],
                  [(10, 0, 8), (-10, 0, 8), (0, 10, 8), (0, -10, 8), (10, 0, 8)]))
    cases.append(("equal loads, spread", [[0, 0, 2.0, 46.0, 5.0, 1024], [0, 0, 2.0, 46.0, 5.0, 1024],
                                          [300, 0, 2.0, 46.0, 5.0, 1024]],
                  [(k * 20, (k % 3) * 20, 10) for k in range(12)]))
    # one static builder beside a moving one; a static one alone with targets out of reach
    cases.append(("one static", [[0, 0, 0.0, 100.0, 0.0, 1024], [500, 0, 1.5, 46.0, 0.0, 1024]],
                  [(50, 0, 20), (90, 0, 20), (150, 0, 20), (480, 0, 20), (700, 0, 20)]))
    cases.append(("static, nothing in reach", [[0, 0, 0.0, 30.0, 0.0, 1024]],
                  [(50, 0, 20), (90, 0, 20)]))
    cases.append(("static, negative speed", [[0, 0, -1.0, 60.0, 0.0, 1024]],
                  [(50, 0, 20), (90, 0, 20), (0, 59, 3)]))
    # room 0 and 1
    cases.append(("room 0", [[0, 0, 1.0, 46.0, 0.0, 0], [100, 0, 1.0, 46.0, 0.0, 3]],
                  [(k * 10, 0, 8) for k in range(6)]))
    cases.append(("room 1", [[0, 0, 1.0, 46.0, 0.0, 1], [100, 0, 1.0, 46.0, 0.0, 1],
                             [200, 0, 1.0, 46.0, 0.0, 1]], [(k * 10, 0, 8) for k in range(6)]))
    cases.append(("no room anywhere", [[0, 0, 1.0, 46.0, 0.0, 0]], [(10, 0, 8)]))
    cases.append(("no targets", [[0, 0, 1.0, 46.0, 0.0, 5]], []))
    cases.append(("no builders", [], [(10, 0, 8)]))
    cases.append(("queued loads", [[0, 0, 1.0, 46.0, 900.0, 1024], [0, 0, 1.0, 46.0, 0.0, 1024]],
                  [(k * 30, 0, 50) for k in range(10)]))
    return cases


def random_split_cases(rng, count):
    cases = []
    for _ in range(count):
        nb = rng.randint(1, 8)
        nt = rng.randint(0, 60)
        span = rng.choice([64, 400, 3000])
        builders = []
        for _ in range(nb):
            speed = rng.choice([0.0, 0.5, 1.0, 1.25, 2.9, rng.uniform(0.01, 4.0)])
            builders.append([rng.randint(-span, span), rng.randint(-span, span), speed,
                             float(rng.choice([0, 46, 60, 100, rng.randint(1, 300)])),
                             rng.choice([0.0, 0.0, float(rng.randint(0, 2000)),
                                         rng.uniform(0, 500)]),
                             rng.choice([0, 1, 2, 5, 1024])])
        targets = [(rng.randint(-span, span), rng.randint(-span, span),
                    reclaim_ticks(rng.choice([0, 250, 125.5]), rng.choice([0, 204, 33.25])))
                   for _ in range(nt)]
        cases.append(("random", builders, targets))
    # the target cap, with one builder and a crowd of them
    cases.append(("the cap", [[0, 0, 1.0, 46.0, 0.0, 1024]],
                  [(rng.randint(-2000, 2000), rng.randint(-2000, 2000), 8) for _ in range(MAX_TARGETS)]))
    cases.append(("the cap, many builders", [[rng.randint(-2000, 2000), rng.randint(-2000, 2000),
                                              1.0, 46.0, 0.0, 200] for _ in range(48)],
                  [(rng.randint(-2000, 2000), rng.randint(-2000, 2000), 8) for _ in range(MAX_TARGETS)]))
    return cases


def main(argv):
    build()
    rng = random.Random(20261026)
    t = Tally()

    members = membership_cases(rng)
    got = native([f"inside {cx} {cz} {r} {x} {z}" for cx, cz, r, x, z in members])
    for case, ans in zip(members, got):
        t.expect(int(ans) == int(inside(*case)), f"inside{case}: native {ans}")

    orders = order_cases(rng)
    got = native([f"order {cx} {cz} {len(ts)} " + " ".join(f"{x} {z}" for x, z in ts)
                  for cx, cz, ts in orders])
    for (cx, cz, ts), ans in zip(orders, got):
        want = order(ts, cx, cz)
        mine = [int(w) for w in ans.split()]
        t.expect(mine == want, f"order around ({cx},{cz}) of {len(ts)}: native {mine[:12]}, "
                               f"want {want[:12]}")

    ticks = [((0, 0), 8), ((0, 250), 70), ((204, 0), 59), ((2900, 0), 733), ((-5, 0), 8),
             ((float("nan"), 0), 8), ((125.5, 33.25), 48)]
    got = native([f"ticks {e} {m}" for (e, m), _ in ticks])
    for ((e, m), want), ans in zip(ticks, got):
        t.expect(int(ans) == want and reclaim_ticks(e, m) == want,
                 f"ticks({e}, {m}): native {ans}, python {reclaim_ticks(e, m)}, want {want}")

    splits = fixed_split_cases() + random_split_cases(rng, 2000)
    got = native([split_line(b, ts) for _, b, ts in splits])
    for (what, builders, targets), ans in zip(splits, got):
        m, pairs, after = parse_split(ans)
        want_pairs, want_after = order_sweep(builders, targets)
        label = f"split ({what}, {len(builders)} builders, {len(targets)} targets)"
        t.expect(m == len(pairs) and pairs == want_pairs,
                 f"{label}: native {pairs[:8]}, mockup {want_pairs[:8]}")
        t.expect([[a[0], a[1], a[2], a[3]] for a in after] ==
                 [[s[0], s[1], s[4], s[5]] for s in want_after],
                 f"{label}: builders after {after[:3]}, mockup {[s[:2] + s[4:] for s in want_after][:3]}")
        seqs = [k for _, k in pairs]
        t.expect(len(seqs) == len(set(seqs)) and all(0 <= k < len(targets) for k in seqs),
                 f"{label}: a target assigned twice or out of range: {seqs}")
        for i, b in enumerate(builders):
            t.expect(sum(1 for w, _ in pairs if w == i) <= b[5], f"{label}: builder {i} past its room")
        # a builder able to move takes every target while any builder has room
        if any(b[2] > 0 for b in builders) and \
                sum(b[5] for b in builders if b[2] > 0) >= len(targets):
            t.expect(m == len(targets), f"{label}: {m} of {len(targets)} assigned")

    print(f"sweeptest: {t.checks} checks, {t.failed} failed")
    return 1 if t.failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
