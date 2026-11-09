#!/usr/bin/env python3
"""Formation move (tagpu_formation.c): with mobile units selected, dragging the order button lays a
path, and each unit takes one of the slots spaced evenly along it.

    formation_regression.py <instance> core      cases (a)-(j) below
    formation_regression.py <instance> type0     Interface Type 0: (k) and (l)
    formation_regression.py <instance> preview   the dots held mid-drag: the marker pass's count;
                                                 with --gdi, a shot of them and GDI's blit count
    formation_regression.py <instance> replay    the formation-basic test group's gestures
    formation_regression.py --selftest           the Python geometry against formtest.py dump

Each mode plays on scenarios/formation-basic.json, handed the level held (resource_regression.py,
IN LOCKSTEP): tareplay's `run` step hands `replay` over at its GameTime; the others take
`--launch`, which holds the run lock (tools/runlock.py), launches the instance with the launch's
hold (tagpu_hold.on) and the play defaults on the scenario, and stops it afterwards.

The cases (core), the eight mobiles selected unless named, Interface Type 1:
  (a) a right drag A->B: each unit one node at the slot the assignment gives it, MOVE_GROUND
  (VTOL_MOVE for the gunship); (b) Shift + a right drag C->D->E: each unit's (a) node and one more,
  measured from the (a) nodes; (c) with tagpu_formation.off a right click orders at the press (stock),
  and with the module on a 5-px right drag orders nothing at the press and the same orders at the
  release (`short`); (d) one mobile and the lab: stock's click at the release point (`single`), the
  lab given nothing; (e) a left press mid right-drag: nothing ordered (`drop reason=other`);
  (f) Move armed, the mobiles and the lab, a left drag: the formation, the lab one QMOVE at the
  path's end, then mode 1; (f') the same under Shift held through the release: appended, Move kept
  armed until Shift's release; (g) Move armed, a right press mid left-drag: nothing ordered, mode 1;
  (h) tagpu_formation.off: a long right drag orders at the press and nothing after, no line;
  (i) the in-game row FORMATION 0 is (h), 1 is (a); (j) the level's counters (`level end`, listcap=0).
(type0) (k) a right press deselects at the press, no gesture; (l) Move armed and a left drag form up.

What each commit orders is computed here: the path from the world point main+0x2CAA read after the
press and each move, the starts from the units (or their queues' ends under Shift), and the slots
and the assignment by the port of tagpu_formgeo.c below, line for line, which `--selftest` checks
against the native one: 3000 random cases (2..50-node paths, n in 2..20, then 255, 256, 257, 300 and
1500, and paths of 5000 and 9000 points for the decimation), compared line for line through
tagpu/ddraw/tests/formtest.py dump. The orders are read out of the engine, 16.16 positions compared
exactly; the DLL's own account of each gesture from its log (`formation_line`).
"""
import argparse
import json
import math
import random
import struct
import subprocess
import sys
import time
from pathlib import Path

from resource_regression import ACK_S, CHECK_FAILURES, TACLI, TREE, Check, tacli_api
import runlock

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




# -- driving the game -----------------------------------------------------------------------------

SCENARIO = TREE / "scenarios/formation-basic.json"
END_SCENARIO = TREE / "scenarios/replay-end.json"
MAIN = "*0x511DE8"
UNIT_STRIDE = 0x118
NEED = {"ARMPW", "ARMSTUMP", "ARMCK", "ARMBRAWL", "ARMLAB"}
MOBILES = ("pw1", "pw2", "pw3", "pw4", "stump1", "stump2", "ck", "gun")
EYE = (1050, 5900)      # the plateau (world x 1056..1999, z 5600..6655) and the units in view
HEIGHT = 55             # the plateau's height: a world point's screen row is z - 55 / 2
# the engine's names of the three orders a formation gives, the strings 0x438760 looks up
# (tagpu_formation.c, actions)
NAME_AT = {"move": 0x005052E4, "vtol": 0x005016CC, "qmove": 0x0050531C}
FORM_RX = r"formation: (armed \(|not armed)"
ROUTE_RX = r"route: (armed \(|.*not installed)"


def formation_line(line):
    """One of tagpu_formation.c's lines as (kind, fields), or None: kind is `commit`, `short`,
    `single`, `drop` or `level end`, and fields its `key=value` words (a value of digits an int):
        formation: commit button=right mode=1 itype=1 units=8 qmove=0 nodes=2 len=600 thr=27
                   shift=0 algo=h issued=8 listcap=0
        formation: short button=right len=5 thr=27
        formation: single button=right units=1
        formation: drop reason=other
        formation: level end commits=.. short=.. single=.. drops=.. listcap=.. gdi=..
    With FORM_RX (the install's verdict), this is where the module's line format is written down."""
    head = "formation: "
    at = line.find(head)
    if at < 0:
        return None
    words = line[at + len(head):].split()
    if words[:2] == ["level", "end"]:
        kind, rest = "level end", words[2:]
    elif words[:1] and words[0] in ("commit", "short", "single", "drop"):
        kind, rest = words[0], words[1:]
    else:
        return None
    fields = {}
    for w in rest:
        k, eq, v = w.partition("=")
        if eq:
            fields[k] = int(v) if v.isdigit() else v
    return kind, fields


def mark_form(line):
    """The formation dots the marker pass drew, from its status line (tagpu_mark.c,
    `mark: bars=.. ... form=N ...`, every 120 frames under mark.on=log); None for any other line."""
    if not line.lstrip().startswith("mark: bars="):
        return None
    for w in line.split():
        k, eq, v = w.partition("=")
        if eq and k == "form" and v.isdigit():
            return int(v)
    return None


def tacli(*args, timeout=600):
    r = subprocess.run([sys.executable, str(TACLI), *map(str, args)], capture_output=True,
                       text=True, timeout=timeout)
    assert r.returncode == 0, f"tacli {' '.join(map(str, args[:2]))}: {(r.stdout + r.stderr).strip()}"
    return r.stdout


class Lever:
    """A lever file present for the block (a file overrides the play defaults, tagpu_opt.c) and
    put back as it was afterwards. `word`, when given, is added to whatever the file held."""
    def __init__(self, gamedir, name, word=None):
        self.lever = gamedir / name
        self.word = word
        self.old = self.lever.read_text() if self.lever.exists() else None

    def __enter__(self):
        words = (self.old or "").split()
        if self.word and self.word not in (w.lower() for w in words):
            words.append(self.word)
        self.lever.write_text(" ".join(words) + "\n")
        return self

    def __exit__(self, *exc):
        if self.old is None:
            self.lever.unlink(missing_ok=True)
        else:
            self.lever.write_text(self.old)


class Formation(Check):
    """The scenario's units by their ids (Check.units keeps one per type: `pw1`..`pw4` are four
    of one), the drag, and the orders a commit is checked against."""
    def __init__(self, name):
        super().__init__(name, NEED)
        self.down = []                      # the keys and buttons this run holds, in order
        self.types, self.ids = self.identify()
        self.units = self.ids               # Check's address, chain, screen and select take ids
        self.names, self.masks = {}, {}
        self.order_name = {k: self.cstring(a).upper() for k, a in NAME_AT.items()}
        for rx, want in ((FORM_RX, "formation: armed ("), (ROUTE_RX, "right 0x49939B")):
            got = self.call("log", "--grep", rx, "--tail", 1, "--json")["lines"]
            assert got and want in got[-1], f"the install: {got[-1] if got else 'no line this run'}"

    def identify(self):
        """Each scenario unit's engine index: the owner-0 unit of its type nearest its scenario
        position (they stand 50 wu or more apart, and none has moved under the launch's hold)."""
        units = json.loads(SCENARIO.read_text())["units"]
        roster = [u for u in self.call("roster", "--json")["units"] if u["owner"] == 0]
        types, ids = {}, {}
        for u in units:
            cands = [r["engine_index"] for r in roster
                     if r["type"].upper() == u["type"] and r["engine_index"] not in ids.values()]
            assert cands, f"no {u['type']} in the roster for {u['id']}"
            def dist(idx, want=u["pos"]):
                x, _, z = self.position_at(idx)
                return ((x >> 16) - want[0]) ** 2 + ((z >> 16) - want[1]) ** 2
            ids[u["id"]] = min(cands, key=dist)
            types[u["id"]] = u["type"]
        return types, ids

    def cstring(self, address):
        return self.bytes(f"0x{address:x}:x20").split(b"\0")[0].decode("latin-1")

    # -- input
    def press(self, *tokens):
        """`down:` each token, remembered so a failed case lets go of it (let_go)."""
        if tokens:
            self.keys(*(f"down:{t}" for t in tokens))
            self.down += tokens

    def lift(self, *tokens):
        if tokens:
            self.keys(*(f"up:{t}" for t in tokens))
            for t in tokens:
                self.down.remove(t)

    def let_go(self):
        held, self.down = self.down[::-1], []
        if held:
            self.keys(*(f"up:{t}" for t in held))

    def screen_at(self, x, z):
        """The screen point over world (x, z) on the plateau, at zoom 1: the projection
        x - eyeX + 0x80, z - altitude / 2 - eyeY + 0x20 (0x469C63..0x469C94)."""
        ex, ez = self.values(f"{MAIN}+0x1431f", f"{MAIN}+0x14323")
        return x - ex + 128, z - HEIGHT // 2 - ez + 32

    def aim(self, x, z):
        sx, sy = self.screen_at(x, z)
        self.keys(f"pmove:{sx},{sy}")

    def drag(self, button, path, mods=(), keep=False):
        """`mods` held, the pointer at path[0], `button` down, a move to each further point,
        `button` up, then `mods` up unless `keep`. Returns the path as the DLL builds it -- the
        world point main+0x2CAA read after the press and after each move, through path_add --
        and the log index before the drag."""
        start = self.mark()
        self.press(*mods)
        self.aim(*path[0])
        self.press(button)
        nodes = []
        path_add(nodes, self.point())
        for w in path[1:]:
            self.aim(*w)
            path_add(nodes, self.point())
        self.lift(button)
        if not keep:
            self.lift(*reversed(mods))
        return nodes, start

    # -- engine state
    def point(self):
        """The handler's world point for the last record, main+0x2CAA: 16.16 x, altitude, z."""
        return struct.unpack("<iii", self.bytes(f"{MAIN}+0x2caa:xc"))

    def mode(self):
        return self.values(f"{MAIN}+0x2cc3:1")[0]

    def flags(self):
        return self.values(f"{MAIN}+0x2cc6:1")[0]

    def itype(self):
        return self.values(f"{MAIN}+0x37efa")[0]

    def position_at(self, idx):
        return struct.unpack("<iii", self.bytes(f"0x{self.base + idx * UNIT_STRIDE + 0x6a:x}:xc"))

    def position(self, uid):
        return self.position_at(self.ids[uid])

    def positions(self, uids):
        """Each unit's (x, altitude, z), all read in one snapshot."""
        got = self.snapshot(*(f"0x{self.address(u) + 0x6a:x}:xc" for u in uids))
        return {u: struct.unpack("<iii", bytes.fromhex(v)) for u, v in zip(uids, got)}

    def selection(self, uids):
        """The units of `uids` selected (+0x110 bit 4), read in one snapshot."""
        got = self.snapshot(*(f"0x{self.address(u) + 0x110:x}" for u in uids))
        return {u for u, v in zip(uids, got) if int(v.split()[0]) & 0x10}

    def select(self, *uids):
        """Exactly `uids` selected: Esc, then a click on each (Shift after the first), in one
        input batch, at the screen points read in one snapshot. Every unit stands in view at EYE
        and none moves under the hold."""
        pos = self.positions(uids)
        ex, ez = self.values(f"{MAIN}+0x1431f", f"{MAIN}+0x14323")
        tokens = ["esc"]
        for i, u in enumerate(uids):
            x, h, z = (v >> 16 for v in pos[u])
            sx, sy = x - ex + 128, z - h // 2 - ez + 32
            assert 128 < sx < 1024 and 32 < sy < 736, f"{u} is out of view at ({sx}, {sy})"
            tokens += [f"pclick:{sx},{sy}"] if not i else ["down:shift", f"pclick:{sx},{sy}", "up:shift"]
        self.keys(*tokens)
        got = self.selection(tuple(self.ids))
        self.once(got == set(uids), f"the selection is {sorted(got)}, want {sorted(uids)}")

    def action_name(self, action):
        """The order descriptor's name (0x438760 compares the char* at descriptor +0x15)."""
        if action not in self.names:
            desc = self.values("0x512344")[0] + action * 0x19
            self.names[action] = self.cstring(self.values(f"0x{desc + 0x15:x}")[0]).upper()
        return self.names[action]

    def chains(self, uids):
        """Each unit's order nodes (0x50 bytes each), every unit's chain read together: the heads
        (unit +0x5C) in one snapshot, then each round's next nodes in one more. The world is held,
        so nothing changes between two rounds; a node not owned by its unit (+0x0E) or a chain
        past 100 nodes fails."""
        heads = self.snapshot(*(f"0x{self.address(u) + 0x5c:x}" for u in uids))
        nxt = {u: int(v.split()[0]) for u, v in zip(uids, heads)}
        out = {u: [] for u in uids}
        while any(nxt.values()):
            live = [u for u in uids if nxt[u]]
            got = self.snapshot(*(f"0x{nxt[u]:x}:x50" for u in live))
            for u, v in zip(live, got):
                assert not v.startswith("<"), f"{u}'s order chain links unreadable memory"
                b = bytes.fromhex(v)
                assert struct.unpack_from("<I", b, 0x0e)[0] == self.address(u), \
                    f"{u}'s order chain holds another unit's node"
                out[u].append(b)
                assert len(out[u]) < 100, f"{u}'s order chain runs past 100 nodes"
                nxt[u] = struct.unpack_from("<I", b, 0x4a)[0]
        return out

    def queues(self, uids):
        """Each unit's orders as (name, (x, altitude, z), target)."""
        return {u: [(self.action_name(b[4]), struct.unpack_from("<iii", b, 0x22),
                     struct.unpack_from("<I", b, 0x16)[0]) for b in c]
                for u, c in self.chains(uids).items()}

    def queue(self, uid):
        return self.queues((uid,))[uid]

    def mask(self, action):
        """The order descriptor's marker mask, +0xC."""
        if action not in self.masks:
            desc = self.values("0x512344")[0] + action * 0x19
            self.masks[action] = self.values(f"0x{desc + 0xc:x}")[0]
        return self.masks[action]

    def queue_ends(self, uids):
        """Where each unit's queue ends, as tagpu_formation.c's queue_end walks it (the order
        markers' walker 0x439B30): the last node whose descriptor's marker mask (+0xC bits 0..3)
        is set, at its target unit's position when it has one, else its own; none, the unit's."""
        pos, out = self.positions(uids), {}
        for u, chain in self.chains(uids).items():
            p = pos[u]
            for b in chain:
                if not self.mask(b[4]) & 0x0F:
                    continue
                t = struct.unpack_from("<I", b, 0x16)[0]
                if t and t >= self.base and (t - self.base) % UNIT_STRIDE == 0:
                    p = self.position_at((t - self.base) // UNIT_STRIDE)
                else:
                    p = struct.unpack_from("<iii", b, 0x22)
            out[u] = p
        return out

    def stop_all(self, uids):
        """No order but Standby for `uids`, which are exactly the selection: the order applier's
        stop to everything selected (0x43ADC0 without Shift), selection and mode left alone."""
        got = self.selection(tuple(self.ids))
        assert got == set(uids), f"stop: the selection is {sorted(got)}, want {sorted(uids)}"
        self.call("order", "--sel", "stop")

    def arm_move(self):
        self.call("ui", "click", "ARMMOVE")
        self.once(self.mode() == 2, f"ARMMOVE did not arm Move (mode {self.mode()})")

    # -- the log
    def mark(self):
        self.log += [ln.rstrip() for ln in self.reader()]
        return len(self.log)

    def lines(self, start, kind=None):
        """tagpu_formation.c's lines from log index `start` on (of `kind`), complete once `tacli
        keys` has returned: the DLL logs inside the engine's handling of the event, and the
        batch's `done` line comes after the engine consumed its last event, through the one
        sink (tagpu_log.h)."""
        self.mark()
        return [(k, f) for k, f in filter(None, map(formation_line, self.log[start:]))
                if kind is None or k == kind]

    def any_line(self, start):
        self.mark()
        return [ln for ln in self.log[start:] if "formation: " in ln]


# -- the cases ------------------------------------------------------------------------------------

def move_name(f, uid):
    return f.order_name["vtol" if f.types[uid] == "ARMBRAWL" else "move"]


def commit_case(f, what, path, ids=MOBILES, button="rbutton", mods=(), keep=False, qmove=()):
    """A drag that commits: each of `ids` (all selected, all movers) one order at the slot the
    port assigns it, appended under Shift; each of `qmove` (a factory, mode 2) QMOVE at the
    path's end; and the commit line saying so."""
    shift = "shift" in mods
    mode, itype = f.mode(), f.itype()
    parts = sorted(ids, key=lambda u: f.ids[u])         # the DLL's order: ascending slot
    before = f.queues((*parts, *qmove))
    ends = f.queue_ends(parts) if shift else f.positions(parts)
    starts = [ends[u] for u in parts]
    nodes, start = f.drag(button, path, mods, keep)
    slot = slots(nodes, len(parts))
    algo, slot_of = assign(starts, slot)
    after = f.queues((*parts, *qmove))
    for k, u in enumerate(parts):
        got = [(n, p) for n, p, _ in after[u]]
        want = [(n, p) for n, p, _ in (before[u] if shift else [])] + \
               [(move_name(f, u), tuple(slot[slot_of[k]]))]
        assert got == want, f"{what}: {u}'s orders {got}, want {want}"
    for u in qmove:
        got = [(n, p) for n, p, _ in after[u]]
        want = [(n, p) for n, p, _ in (before[u] if shift else [])] + \
               [(f.order_name["qmove"], tuple(nodes[-1]))]
        assert got == want, f"{what}: {u}'s orders {got}, want {want}"
    got = f.lines(start)
    commits = [fl for k, fl in got if k == "commit"]
    assert len(commits) == 1 and len(got) == 1, f"{what}: the formation lines {got}, want one commit"
    c = commits[0]
    want = {"button": "right" if button == "rbutton" else "left", "mode": mode, "itype": int(itype != 0),
            "units": len(parts), "qmove": len(qmove), "nodes": len(nodes), "shift": int(shift),
            "algo": algo, "issued": len(parts), "listcap": 0}
    wrong = {k: (c.get(k), v) for k, v in want.items() if c.get(k) != v}
    assert not wrong, f"{what}: commit line {c}: (got, want) {wrong}"
    assert c["len"] >= c["thr"], f"{what}: a {c['len']} wu drag under the {c['thr']} wu threshold"
    return nodes, c


def case_a(f, path=((1100, 6200), (1700, 6200)), what="(a) a right drag A->B"):
    f.select(*MOBILES)
    nodes, c = commit_case(f, what, path)
    assert len(nodes) == 2 and c["len"] >= 300, f"{what}: {len(nodes)} nodes, {c['len']} wu"
    return c


def case_b(f):
    what = "(b) Shift + a right drag C->D->E"
    nodes, _ = commit_case(f, what, ((1100, 6100), (1400, 6000), (1650, 6100)), mods=("shift",))
    assert len(nodes) == 3, f"{what}: {len(nodes)} nodes, want 3"


def case_c(f):
    """(c) A plain right click with the module off is stock's order at the press; a 5-px right
    drag with it on orders nothing at the press and stock's same orders at the release."""
    what = "(c) a click and a short drag"
    F = (1300, 6150)
    f.stop_all(MOBILES)
    start = f.mark()
    with Lever(f.gamedir, "tagpu_formation.off"):
        f.aim(*F)
        f.press("rbutton")
        off = f.queues(MOBILES)
        assert all(any(n == move_name(f, u) for n, _, _ in q) for u, q in off.items()), \
            f"{what}: the control's press gave no move: {off}"
        f.lift("rbutton")
        assert f.queues(MOBILES) == off, f"{what}: the control's release changed orders"
    assert not f.any_line(start), f"{what}: the module logged with its lever off: {f.any_line(start)}"
    f.stop_all(MOBILES)
    stopped = f.queues(MOBILES)
    start = f.mark()
    f.aim(*F)
    f.press("rbutton")
    assert f.queues(MOBILES) == stopped, f"{what}: the deferred press ordered"
    sx, sy = f.screen_at(*F)
    f.keys(f"pmove:{sx + 5},{sy}")
    f.lift("rbutton")
    on = f.queues(MOBILES)
    assert on == off, f"{what}: the short drag's orders {on}, stock's {off}"
    got = f.lines(start)
    assert [k for k, _ in got] == ["short"], f"{what}: the formation lines {got}, want one short"
    assert got[0][1]["len"] < got[0][1]["thr"], f"{what}: {got[0][1]}"


def case_d(f):
    """(d) One mobile and the lab: the lab has no mover, so one unit takes part and the release is
    stock's click there. 0x48CF30's group offset (centroid of every selected unit, kept when
    (dx^2 >> 32) + (dz^2 >> 32) <= 3000 n) is refused with pw1 and the lab 600 wu apart, so pw1's
    node is the point itself."""
    what = "(d) one mobile and the lab"
    f.select("pw1", "lab")
    lab = f.queue("lab")
    pos = f.positions(("pw1", "lab"))
    (px, _, pz), (lx, _, lz) = pos["pw1"], pos["lab"]
    cx, cz = (px + lx) // 2, (pz + lz) // 2
    assert ((px - cx) ** 2 >> 32) + ((pz - cz) ** 2 >> 32) > 3000 * 2, f"{what}: the offset is kept"
    nodes, start = f.drag("rbutton", ((1150, 6300), (1600, 6300)))
    got = [(n, p) for n, p, _ in f.queue("pw1")]
    want = [(f.order_name["move"], tuple(nodes[-1]))]
    assert got == want, f"{what}: pw1's orders {got}, want {want}"
    assert f.queue("lab") == lab, f"{what}: the lab's orders changed: {f.queue('lab')}"
    got = f.lines(start)
    assert [k for k, _ in got] == ["single"] and got[0][1].get("units") == 1, \
        f"{what}: the formation lines {got}, want one single units=1"


def case_e(f):
    """(e) A left press mid right-drag ends the gesture: nothing is ordered."""
    what = "(e) a left press mid right-drag"
    f.select(*MOBILES)
    before = f.queues(MOBILES)
    start = f.mark()
    f.aim(1100, 6250)
    f.press("rbutton")
    f.aim(1400, 6250)
    f.press("lbutton")
    f.lift("lbutton")
    f.aim(1600, 6250)
    f.lift("rbutton")
    assert f.queues(MOBILES) == before, f"{what}: orders changed"
    got = f.lines(start)
    assert [k for k, _ in got] == ["drop"] and got[0][1].get("reason") == "other", \
        f"{what}: the formation lines {got}, want one drop reason=other"


def case_f(f):
    """(f) Move armed, the mobiles and the lab, a left drag: the formation as (a), the lab one
    QMOVE at the path's end, and stock's tail: mode 1, main+0x2CC6 bit 5 clear. (f') the same
    with Shift held through the release: appended, mode 2 and bit 5 kept, and Shift's release
    disarms (0x4992D8)."""
    f.select(*MOBILES, "lab")
    f.arm_move()
    commit_case(f, "(f) Move armed, a left drag", ((1100, 6150), (1650, 6150)), button="lbutton",
                qmove=("lab",))
    assert f.mode() == 1 and not f.flags() & 0x20, \
        f"(f) mode {f.mode()}, main+0x2CC6 0x{f.flags():02x} after the release"
    f.arm_move()
    commit_case(f, "(f') Shift, Move armed, a left drag", ((1150, 6050), (1550, 6000)),
                button="lbutton", mods=("shift",), keep=True, qmove=("lab",))
    assert f.mode() == 2 and f.flags() & 0x20, \
        f"(f') mode {f.mode()}, main+0x2CC6 0x{f.flags():02x} with Shift held"
    f.lift("shift")
    f.until(lambda: f.mode() == 1, "(f') Shift's release did not disarm Move")


def case_g(f):
    """(g) Move armed, a right press mid left-drag: stock's cancel (0x499100), nothing ordered."""
    what = "(g) a right press mid left-drag"
    f.arm_move()
    before = f.queues((*MOBILES, "lab"))
    start = f.mark()
    f.aim(1100, 6200)
    f.press("lbutton")
    f.aim(1500, 6200)
    f.press("rbutton")
    f.lift("rbutton")
    f.lift("lbutton")
    assert f.queues((*MOBILES, "lab")) == before, f"{what}: orders changed"
    assert not f.lines(start, "commit"), f"{what}: committed {f.lines(start, 'commit')}"
    assert f.mode() == 1, f"{what}: mode {f.mode()}"


def stock_drag(f, what):
    """Off, a long right drag is stock's: the orders at the press, none at the moves or the
    release, and no formation line."""
    f.select(*MOBILES)
    f.stop_all(MOBILES)
    stopped = f.queues(MOBILES)
    start = f.mark()
    f.aim(1200, 6100)
    f.press("rbutton")
    pressed = f.queues(MOBILES)
    assert all(pressed[u] != stopped[u] for u in MOBILES), f"{what}: the press did not order"
    f.aim(1400, 6100)
    f.aim(1600, 6100)
    f.lift("rbutton")
    assert f.queues(MOBILES) == pressed, f"{what}: the moves or the release ordered"
    assert not f.any_line(start), f"{what}: the module logged {f.any_line(start)}"


def case_h(f):
    with Lever(f.gamedir, "tagpu_formation.off"):
        stock_drag(f, "(h) the lever off")


def case_i(f):
    """(i) The in-game row: FORMATION 0 is (h), FORMATION 1 is (a)."""
    f.menu(FORMATION=0)
    stock_drag(f, "(i) the row off")
    f.menu(FORMATION=1)
    case_a(f, ((1100, 6250), (1650, 6050)), "(i) the row on")


def level_end(f):
    """(j) The level ended (replay-end.json, as tareplay ends a fixture; the module writes its
    counters at the teardown) and its counters read; returns the line's fields."""
    start = f.mark()
    tacli("scenario", "apply", f.name, END_SCENARIO, timeout=300)
    f._resume()
    f.held = False
    end, results = time.monotonic() + 300, False
    while not (got := f.lines(start, "level end")):
        assert time.monotonic() < end, "(j) no `formation: level end` line in 300 s"
        if not results and f.call("ui", "--json")["gui"].upper() == "ENDMSN.GUI":
            f.call("ui", "click", "MainMenu")
            results = True
        time.sleep(1)
    fl = got[-1][1]
    assert fl.get("listcap") == 0, f"(j) the level ended with listcap={fl.get('listcap')}: {fl}"
    return fl


def start_case(f):
    f.hold()
    f.eye(*EYE)
    assert f.itype() != 0, "the launch did not force Interface Type 1"


def core(f, a):
    start_case(f)
    case_a(f)
    case_b(f)
    case_c(f)
    case_d(f)
    case_e(f)
    case_f(f)
    case_g(f)
    case_h(f)
    case_i(f)
    fl = level_end(f)
    print(f"formation-core: (a)-(j) passed; level end {fl}")


def type0(f, a):
    """Interface Type 0 through PREFS (resource_regression.mex's steps; the next launch forces
    Type 1 again): (k) a right press deselects at the press, no gesture; (l) Move armed and a
    left drag form up as (a)."""
    f.keys("tab")
    f.call("ui", "click", "PREFS")
    f.call("ui", "click", "SPEEDS")
    f.call("ui", "set", "LEFTCLICK", 0)
    f.call("ui", "click", "PREV")
    f.call("ui", "click", "OK")
    f.until(lambda: f.itype() == 0, "PREFS did not reach Interface Type 0")
    f.hold()
    f.eye(*EYE)
    what = "(k) a Type-0 right press"
    f.select(*MOBILES)
    start = f.mark()
    f.aim(1300, 6150)
    f.press("rbutton")
    assert not f.selection(MOBILES), f"{what}: the press did not deselect"
    f.lift("rbutton")
    assert not f.any_line(start), f"{what}: the module logged {f.any_line(start)}"
    f.select(*MOBILES)
    f.arm_move()
    commit_case(f, "(l) Type 0, Move armed, a left drag", ((1100, 6200), (1700, 6200)),
                button="lbutton")
    assert f.mode() == 1, f"(l) mode {f.mode()} after the release"
    print("formation-type0: (k) and (l) passed")


def centre(f, at):
    """The camera centred on `at` through a scenario's camera line, which the command apply places
    on either backend. `tacli eye`'s hold is not applied under GDI (MEASURED 2026-11-09: the eye
    stayed at the scenario's (1066, 5621) for 60 s), so a GDI run frames the units this way."""
    doc = f.gamedir / "formation-camera.json"
    # an apply clears every unit unless told not to
    doc.write_text(json.dumps({"format": "ta-scenario/1", "setup": {"clear_existing": False},
                               "camera": {"at": list(at)}}))
    try:
        before = f.values(f"{MAIN}+0x1431f", f"{MAIN}+0x14323")
        tacli("scenario", "apply", f.name, doc, timeout=300)
        f.until(lambda: f.values(f"{MAIN}+0x1431f", f"{MAIN}+0x14323") != before,
                f"the camera did not move to centre {at}")
    finally:
        doc.unlink(missing_ok=True)


def held_count(f, what):
    """The second marker-pass status line's form= from now (the first may have drawn a packet
    filled before the last input was handled)."""
    start = f.mark()
    end = time.monotonic() + 3 * ACK_S
    while True:
        f.mark()
        counts = [c for c in map(mark_form, f.log[start:]) if c is not None]
        if len(counts) >= 2:
            return counts[1]
        assert time.monotonic() < end, f"{what}: no two `mark:` status lines with form= in {3 * ACK_S} s"
        time.sleep(0.2)


def preview(f, a):
    """The dots: none at the press, one per unit from the first move (below the threshold too),
    none after the release. On Vulkan the marker pass's count (mark.on=log); on GDI (--gdi) a
    shot of the held drag and the level end's blit count."""
    if a.gdi:
        f.hold()
        centre(f, (1400, 6300))
    else:
        start_case(f)
    f.select(*MOBILES)
    A, B = (1100, 6200), (1700, 6200)
    if a.gdi:
        f.aim(*A)
        f.press("rbutton")
        f.aim(1300, 6200)
        f.aim(*B)
        a.shot.parent.mkdir(parents=True, exist_ok=True)
        tacli("shot", f.name, "-o", a.shot, timeout=120)
        f.lift("rbutton")
        fl = level_end(f)
        assert fl.get("gdi", 0) > 0, f"GDI: the level drew no formation dot: {fl}"
        print(f"formation-preview: GDI blitted {fl['gdi']} dots; the held drag is {a.shot}")
        return
    with Lever(f.gamedir, "tagpu_mark.on", "log"):
        f.aim(*A)
        f.press("rbutton")
        assert held_count(f, "the press") == 0, "dots before any move"
        sx, sy = f.screen_at(*A)
        f.keys(f"pmove:{sx + 10},{sy}")
        n = held_count(f, "a move under the threshold")
        assert n == len(MOBILES), f"{n} dots after a move under the threshold, want {len(MOBILES)}"
        f.aim(*B)
        n = held_count(f, "a long drag")
        assert n == len(MOBILES), f"{n} dots on a long drag, want {len(MOBILES)}"
        start = f.mark()
        f.lift("rbutton")
        assert [k for k, _ in f.lines(start)] == ["commit"], f"the release: {f.lines(start)}"
        assert held_count(f, "the release") == 0, "dots after the release"
    print(f"formation-preview: no dot at the press, {len(MOBILES)} from the first move, none after")


def replay(f, a):
    """The test group's gestures: (a), (b) and (f), then the world handed back, so the units walk
    to their slots for the rest of the level and the recording carries it."""
    start_case(f)
    case_a(f)
    case_b(f)
    case_f(f)
    print("formation-replay: (a), (b) and (f) committed")


def no_formation_key(gamedir):
    """The instance's store without a `formation=` line: (i) writes it through the menu, and a
    failed run must not hand the next launch the module off."""
    store = gamedir / "impure.cfg"
    if store.exists():
        kept = [ln for ln in store.read_text().splitlines(keepends=True)
                if not ln.strip().lower().startswith("formation=")]
        store.write_text("".join(kept))


def launched(instance, mode, gdi=False):
    """The instance launched on the scenario with the launch's hold (and GDI's lever, read at
    attach), under the run lock; the callable that stops it and takes the levers back."""
    runlock.hold(TREE, f"formation_regression {mode} on {instance}")
    gamedir = tacli_api().Instance(instance).gamedir
    levers = ["tagpu_hold.on"] + (["tagpu_gdi.on"] if gdi else [])
    had = {name: (gamedir / name).exists() for name in levers}
    for name in levers:
        (gamedir / name).write_text("")
    (gamedir / "tagpu_formation.off").unlink(missing_ok=True)
    no_formation_key(gamedir)
    tacli("scenario", "load", instance, SCENARIO, "--defaults", timeout=900)

    def stop():
        try:
            tacli("stop", instance, timeout=300)
        finally:
            no_formation_key(gamedir)
            for name in levers:
                if not had[name]:
                    (gamedir / name).unlink(missing_ok=True)
    return stop


# Each game mode: its name -> a function of the Formation check and the parsed arguments.
MODES = {"core": core, "type0": type0, "preview": preview, "replay": replay}


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("instance", nargs="?")
    p.add_argument("mode", nargs="?", choices=sorted(MODES))
    p.add_argument("--launch", action="store_true",
                   help="core/type0/preview: launch the instance on formation-basic.json first "
                        "(run lock)")
    p.add_argument("--gdi", action="store_true", help="preview: on the GDI backend (with --launch)")
    p.add_argument("--shot", type=Path, default=None,
                   help="preview --gdi: where the held drag's shot goes (default: the gamedir)")
    p.add_argument("--selftest", action="store_true", help="geometry against formtest.py, no game")
    a = p.parse_args()
    if a.selftest:
        try:
            selftest()
        except CHECK_FAILURES as e:
            print(f"formation selftest FAILED: {e}", file=sys.stderr)
            return 1
        return 0
    if not a.instance or not a.mode:
        p.error("an instance and a mode, or --selftest")
    if a.launch and a.mode == "replay":
        p.error("replay runs inside its test group (tools/replay/fixtures.json, formation-basic)")
    if a.gdi and (a.mode != "preview" or not a.launch):
        p.error("--gdi is preview's, and the backend is chosen at launch: it takes --launch")
    stop = None
    try:
        if a.launch:
            stop = launched(a.instance, a.mode, a.gdi)
        check = Formation(a.instance)
        if a.shot is None:
            a.shot = check.gamedir / "formation-preview-gdi.png"
        # As place_regression.py: the case's own failure is the exit reason and is printed first;
        # a cleanup step that fails after it is reported beside it, never in its place.
        failures = []
        try:
            MODES[a.mode](check, a)
        except CHECK_FAILURES as e:
            failures.append(("", e))
        finally:
            for what, step in (("let go", check.let_go), ("release", check.release)):
                try:
                    step()
                except CHECK_FAILURES as e:
                    failures.append((what, e))
            for i, (what, e) in enumerate(failures):
                if i == 0:
                    print(f"formation regression FAILED: {what + ': ' if what else ''}{e}",
                          file=sys.stderr)
                else:
                    print(f"formation regression: {what} failed as well: {e}", file=sys.stderr)
        if failures:
            return 1
    except CHECK_FAILURES as e:
        print(f"formation regression FAILED: {e}", file=sys.stderr)
        return 1
    finally:
        if stop:
            stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
