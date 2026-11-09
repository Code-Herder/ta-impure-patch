#!/usr/bin/env python3
"""Formation move (tagpu_formation.c): with mobile units selected, dragging the order button lays a
path, and each unit takes one of the slots spaced evenly along it.

    formation_regression.py --selftest     the Python geometry against formtest.py dump, no game

The geometry -- the path, its length, the slots and the assignment -- is a port of
tagpu_formgeo.c, line for line, so that a game mode can predict every order a drag makes from the
nodes the drag recorded and the units' positions. `--selftest` checks the port against the native
one: 3000 random cases (2..50-node paths, n in 2..20, then 255, 256, 257, 300 and 1500, and paths of
5000 and 9000 points for the decimation), compared line for line through
tagpu/ddraw/tests/formtest.py dump.
"""
import argparse
import math
import random
import subprocess
import sys

from resource_regression import CHECK_FAILURES, TREE

FORMTEST = TREE / "tagpu/ddraw/tests/formtest.py"

# -- the geometry: tagpu_formgeo.c, line for line -------------------------------------------------
# A point is (x, alt, z), 16.16 world. Python's `>>` floors, as the C's floor16 does.

FORM_MAX_UNITS = 1500
FORM_HUNGARIAN_MAX = 256
FORM_MAX_NODES = 4096
FORM_UNCROSS_PASSES = 4
FORM_UNCROSS_WORK = 2000000
FORM_COST_MAX = 0x7FFF
HUNG_INF = 0x3FFFFFFF

isqrt = math.isqrt                                 # form_isqrt64


def span(a, b):
    """floor(|b - a|) in 16.16 over x and z."""
    dx, dz = b[0] - a[0], b[2] - a[2]
    return isqrt(dx * dx + dz * dz)


def path_add(node, p):
    """form_path_add on the list `node`, in place; the new count."""
    m = len(node)
    if m and node[m - 1][0] == p[0] and node[m - 1][2] == p[2]:
        return m
    if m >= FORM_MAX_NODES:
        node[:] = node[0:m - 1:2] + [node[m - 1]]
    node.append(p)
    return len(node)


def path_length(node):
    return sum(span(node[k - 1], node[k]) for k in range(1, len(node)))


def lerp(a, b, frac):
    return a + (((b - a) * frac) >> 16)


def slots(node, n):
    """form_slots: `n` slots evenly by arc length on the nodes, both ends included."""
    m = len(node)
    if m == 0:
        return []
    if m == 1 or n < 2:
        return [node[0]] * n
    L = path_length(node)
    e, d0, d1 = 1, 0, span(node[0], node[1])
    out = []
    for k in range(n):
        t = L * k // (n - 1)
        while e < m - 1 and d1 < t:
            e += 1
            d0 = d1
            d1 += span(node[e - 1], node[e])
        seg = d1 - d0
        frac = ((t - d0) << 16) // seg if seg else 0
        frac = min(max(frac, 0), 65536)
        a, b = node[e - 1], node[e]
        out.append((lerp(a[0], b[0], frac), lerp(a[1], b[1], frac), lerp(a[2], b[2], frac)))
    return out


def cost(a, b):
    """form_cost: the distance in whole wu, rounded to nearest, capped."""
    wu = (span(a, b) + 0x8000) >> 16
    return FORM_COST_MAX if wu > FORM_COST_MAX else wu


def hungarian(start, slot):
    """form_hungarian: slot_of, the optimal assignment, ties to the lowest slot."""
    n = len(start)
    if n > FORM_HUNGARIAN_MAX:
        return nox(start, slot)
    a = [[0] * (n + 1)] + [[0] + [cost(start[i - 1], slot[j - 1]) for j in range(1, n + 1)]
                           for i in range(1, n + 1)]
    u, v, p, way = [0] * (n + 1), [0] * (n + 1), [0] * (n + 1), [0] * (n + 1)
    for i in range(1, n + 1):
        p[0] = i
        j0 = 0
        minv, used = [HUNG_INF] * (n + 1), [False] * (n + 1)
        while True:
            used[j0] = True
            i0 = p[j0]
            delta = HUNG_INF
            j1 = 0
            row, ui0 = a[i0], u[i0]
            for j in range(1, n + 1):
                if used[j]:
                    continue
                cur = row[j] - ui0 - v[j]
                if cur < minv[j]:
                    minv[j] = cur
                    way[j] = j0
                if minv[j] < delta:
                    delta = minv[j]
                    j1 = j
            for j in range(n + 1):
                if used[j]:
                    u[p[j]] += delta
                    v[j] -= delta
                else:
                    minv[j] -= delta
            j0 = j1
            if p[j0] == 0:
                break
        while True:
            j1 = way[j0]
            p[j0] = p[j1]
            j0 = j1
            if not j0:
                break
    slot_of = [0] * n
    for j in range(1, n + 1):
        slot_of[p[j] - 1] = j - 1
    return slot_of


def to_wu(points):
    return [(p[0] >> 16, p[2] >> 16) for p in points]


def furthest(pt, best, d):
    """The furthest pair's squared distance and direction, strict `>`, from (best, d)."""
    n = len(pt)
    for i in range(n):
        xi, zi = pt[i]
        for j in range(i + 1, n):
            ex, ez = pt[j][0] - xi, pt[j][1] - zi
            d2 = ex * ex + ez * ez
            if d2 > best:
                best, d = d2, (ex, ez)
    return best, d


def nox_pair(P, Q):
    """Units and slots sorted along the furthest pair's direction, by (key, index), paired."""
    n = len(P)
    best, (dx, dz) = furthest(P, 0, (1, 0))
    best, (dx, dz) = furthest(Q, best, (dx, dz))
    kp = sorted(range(n), key=lambda k: (P[k][0] * dx + P[k][1] * dz, k))
    kq = sorted(range(n), key=lambda k: (Q[k][0] * dx + Q[k][1] * dz, k))
    slot_of = [0] * n
    for k in range(n):
        slot_of[kp[k]] = kq[k]
    return slot_of


def orient(a, b, c):
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])


def opposite(a, b):
    return (a > 0 and b < 0) or (a < 0 and b > 0)


def crosses(p1, q1, p2, q2):
    return (opposite(orient(p1, q1, p2), orient(p1, q1, q2)) and
            opposite(orient(p2, q2, p1), orient(p2, q2, q1)))


def nox_uncross(P, Q, slot_of):
    """The bounded uncross passes, in place; the tests run."""
    n = len(P)
    tests = 0
    for _ in range(FORM_UNCROSS_PASSES):
        swaps = 0
        for i in range(n):
            for j in range(i + 1, n):
                if tests == FORM_UNCROSS_WORK:
                    return tests
                tests += 1
                if crosses(P[i], Q[slot_of[i]], P[j], Q[slot_of[j]]):
                    slot_of[i], slot_of[j] = slot_of[j], slot_of[i]
                    swaps += 1
        if not swaps:
            break
    return tests


def nox(start, slot):
    """form_nox: slot_of, the sorted pairing, then the bounded uncross passes."""
    n = len(start)
    if n > FORM_MAX_UNITS:
        return list(range(n))
    P, Q = to_wu(start), to_wu(slot)
    slot_of = nox_pair(P, Q)
    nox_uncross(P, Q, slot_of)
    return slot_of


def assign(start, slot):
    """form_assign: ('h' or 'x', slot_of)."""
    if len(start) <= FORM_HUNGARIAN_MAX:
        return "h", hungarian(start, slot)
    return "x", nox(start, slot)


# -- offline --------------------------------------------------------------------------------------

def _pts(points, with_alt=True):
    if with_alt:
        return " ".join(f"{x} {a} {z}" for x, a, z in points)
    return " ".join(f"{x} {z}" for x, _, z in points)


def _shown(points):
    return " ".join(f"{x},{a},{z}" for x, a, z in points)


def _assigned(start, slot, slot_of):
    return " ".join([str(sum(cost(start[i], slot[s]) for i, s in enumerate(slot_of))),
                     *map(str, slot_of)])


def selftest_cases(cases=3000, seed=20261109):
    """The dump's input and, per line, what the port answers: `cases` random paths, each added
    point by point, its slots, and a random set of starts assigned to them by every algorithm."""
    rng = random.Random(seed)
    text, want = [], []

    def point(box):
        x0, z0, w = box
        return (x0 + rng.randrange(w), rng.randrange(-(1 << 24), 1 << 24), z0 + rng.randrange(w))

    def case(k, n, box, kinds):
        pts = []
        for _ in range(k):
            r = rng.random()
            if pts and r < 0.08:
                # the last node's x and z at another altitude: a duplicate
                pts.append((pts[-1][0], pts[-1][1] + rng.randrange(1 << 16), pts[-1][2]))
            elif pts and r < 0.12:
                pts.append(rng.choice(pts))                                    # back to a node
            else:
                pts.append(point(box))
        node = []
        for p in pts:
            path_add(node, p)
        text.append(f"add {len(pts)} {_pts(pts)}")
        want.append(" ".join([f"{len(node)} {path_length(node)}", _shown(node)]))
        slot = slots(node, n)
        text.append(f"slots {len(node)} {n} {_pts(node)}")
        want.append(_shown(slot))
        shape = rng.random()
        if shape < 0.05:
            start = [point(box)] * n                                           # all on one spot
        elif shape < 0.1:
            z = point(box)[2]
            start = [(point(box)[0], 0, z) for _ in range(n)]                  # on one line
        else:
            start = [point(box) for _ in range(n)]
        for kind in kinds:
            text.append(f"{kind} {n} {_pts(start, False)} {_pts(slot, False)}")
            if kind == "hung":
                want.append(_assigned(start, slot, hungarian(start, slot)))
            elif kind == "nox":
                want.append(_assigned(start, slot, nox(start, slot)))
            else:
                algo, slot_of = assign(start, slot)
                want.append(f"{algo} {_assigned(start, slot, slot_of)}")

    def box(w=None):
        """A region of the map in 16.16: the whole map, a few hundred wu, or a few wu (ties)."""
        w = w or rng.choice([32768 << 16, 600 << 16, 4 << 16])
        return rng.randrange((32768 << 16) - w + 1), rng.randrange((32768 << 16) - w + 1), w

    for _ in range(cases):
        case(rng.randint(2, 50), rng.randint(2, 20), box(), ("hung", "nox"))
    # the sizes either side of FORM_HUNGARIAN_MAX and the largest, on the whole map and on a
    # crowded 600 wu square
    for w in (32768 << 16, 600 << 16):
        for n in (255, 256):
            case(rng.randint(2, 50), n, box(w), ("assign",))
        for n in (257, 300, 1500):
            case(rng.randint(2, 50), n, box(w), ("nox", "assign"))
    case(5000, 1500, box(), ())
    case(9000, 64, box(), ("hung",))
    return text, want


def selftest(cases=3000, seed=20261109, runner=(sys.executable, str(FORMTEST))):
    """The port against tagpu_formgeo.c through `formtest.py dump`, line for line."""
    text, want = selftest_cases(cases, seed)
    r = subprocess.run([*runner, "dump"], input="\n".join(text) + "\n",
                       capture_output=True, text=True, timeout=600)
    assert r.returncode == 0, f"formtest.py dump: {(r.stdout + r.stderr).strip()[-2000:]}"
    native = [ln for ln in r.stdout.splitlines() if not ln.startswith("formtest:")]
    assert len(native) == len(want), f"formtest.py dump answered {len(native)} of {len(want)} lines"
    for case, w, got in zip(text, want, native):
        assert got == w, f"{case[:200]}: python {w[:200]!r}, native {got[:200]!r}"
    print(f"formation selftest: {len(want)} lines from {cases + 12} paths equal tagpu_formgeo.c's")


# -- the modes ------------------------------------------------------------------------------------

# Each game mode: its name -> a function of the parsed arguments that raises one of
# CHECK_FAILURES on a failed check.
MODES = {}


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("instance", nargs="?")
    p.add_argument("mode", nargs="?", choices=sorted(MODES) or None)
    p.add_argument("--selftest", action="store_true", help="geometry against formtest.py, no game")
    a = p.parse_args()
    if a.selftest:
        try:
            selftest()
        except CHECK_FAILURES as e:
            print(f"formation selftest FAILED: {e}", file=sys.stderr)
            return 1
        return 0
    if not a.instance or a.mode not in MODES:
        p.error("an instance and a mode, or --selftest")
    try:
        MODES[a.mode](a)
    except CHECK_FAILURES as e:
        print(f"formation {a.mode} FAILED: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
