#!/usr/bin/env python3
"""predfit.py -- ImpAI's fight predictor fitted to the engine's fights, and its 90 % check.

    predfit.py content --out FILE [--dll PATH]       one held launch: the fight content read out of the engine
    predfit.py fit --content FILE [--refs DIR] --out FIT.json
                                                     the coefficients, fitted to the training half
    predfit.py emit (--fit FIT.json | --start) [--out FILE]
                                                     the coefficients as src/impai_fitparams.c
    predfit.py check --content FILE [--refs DIR] [--params FILE] [--json OUT]
                                                     the 90 % rule on the check half
    predfit.py selftest                              a reference set the model makes at known coefficients:
                                                     the fit reaches their loss (within 2 %) and margins
                                                     (within 0.05 on average), the check passes

The design: research/notes/impai/brain.md, The fight predictor; the rule: research/notes/impai/calibration.md.

ONE MODEL. Nothing here computes a fight: every margin comes from the DLL's own C (tagpu/ddraw/src/
impai_fight.c) through tools/impai/fightlib.c, built by tools/tasim/native.py fightlib and loaded with ctypes,
and the content it reads is the engine's own tables, peeked whole out of a running game (`content`), so the
fit, the check and the game read the same numbers by construction.

THE CONTENT. `content` holds the run lock for one lane (tools/runlock.py), launches the calibration arena's
map (tools/tasim/calib-arena.json) held on a tareplay worker of its own, rendering with GDI with seat 1 an ImpAI seat and the
check lever tagpu_impai_check.on, reads the level init's `impai: fight` line for the tables' addresses and
counts and the `impai: role` lines for the types' names, peeks the three tables whole, and requires their
FNV-1a 64 to equal the line's digest. It then compares every weapon the types name -- range, reload, burst,
damage, area, edge and velocity -- and every type's hp and speed with the images `tasim_run defs` prints for
the same install (tools/tasim/native.py defs), by value; type 0, the engine's reserved `None`, has no files to
compare with. FILE is `impai-fight-content/1`: the names and the three tables' bytes in hex, as the DLL holds
them, written only when every check passed (an earlier FILE is removed first).

THE FIGHTS are the calibration references (tools/tasim/calibref.py, refs/) whose scenario is graded for the
winner: C1's duels and A2a's group, naval and defence fights (tools/tasim/duelgen.py). A fight's sides are its
scenario's owners 0 (A) and 1 (B), each type with its count and health; its distance the two sides' centroids';
a side closes when any of its units has an order. W is side A's wins (a draw a half) of its n usable runs, M
the mean of the runs' `margin` (side A's value share kept less side B's, tools/tasim/metrics.py combat) when
every run carries one.

THE SPLIT: a fight is in the CHECK half when the FNV-1a 32 of its scenario's stem is odd, else in the TRAINING
half. The fit sees only the training half.

THE FIT minimises
    L = -sum_f [W_f log s(m_f / tau) + (n_f - W_f) log(1 - s(m_f / tau))] + 4 sum_f n_f (m_f - M_f)^2
over the training fights (the margin term only where M_f is known), s the logistic function, in the box BOX
(hit coefficients -4..4, close, focus and splash 0..4, tau 0.01..2): CMA-ES (tools/impai/cmaes.py, seed
CMA_SEED, CMA_POP samples a generation, sigma 0.15 of the box) from the fixed start (hit biases 2, lead and
spread -1, area +1, close 1, focus 0.5, splash 0.5, tau 0.2) until CMA_STALL generations improve the best loss
by less than 1e-6 of it, then Nelder-Mead (scipy, adaptive) from the best point, restarted until a run gains
less than 1e-4 of the loss, then coordinate steps of +-10 % until none improves. Deterministic. The global
stage is there because the loss is not convex in the coefficients and a local search from the start stops in
the first basin: on the self-test's 400 training fights Nelder-Mead and Powell alone stopped at a loss of 1537
against 1378 at the true coefficients, CMA-ES at 1392 (measured 2026-11-12). A coefficient that moves no
training fight's margin (a weapon kind no fight fires, a beam's lead, which the model holds at 0) is held at
its start: it cannot be fitted, and leaving it free would let it drift.

THE CHECK: on the check half's DECISIVE fights (the engine's side-A win rate W/n above 0.8 or below 0.2),
sign(m) agrees with the engine's majority on at least 90 %, and there are at least 40 of them; fewer is
"could not run" (the generators make more). The report gives the same on the training half and on all
fights, the duels' figure alone, the agreement by kind and role pair, and every disagreeing fight with its
engine rate and m.

Exit 0 when the command ran (check: passed), 1 when the check failed, 2 when it could not run.
"""
import argparse
import ctypes
import json
import math
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(TREE / "tools" / "tasim"))
sys.path.insert(0, str(TREE / "tools"))
sys.path.insert(0, str(TREE / "tools" / "replay"))
import calibref  # noqa: E402
import native  # noqa: E402

SCHEMA = "impai-fight-content/1"
PARAMS = TREE / "tagpu" / "ddraw" / "src" / "impai_fitparams.c"
KINDS = ("beam", "direct", "ballistic", "guided", "dropped")
HITS = ("bias", "lead", "spread", "area")
NAMES = [f"hit.{k}.{h}" for k in KINDS for h in HITS] + ["close", "focus", "splash", "tau"]
NCOEF = len(NAMES)                       # IMPAI_FIT_COEFFS
TAU = NCOEF - 1
START = [v for _k in KINDS for v in (2.0, -1.0, -1.0, 1.0)] + [1.0, 0.5, 0.5, 0.2]
LAMBDA = 4.0
DECISIVE = 0.8
PASS = 0.90
MIN_DECISIVE = 40
FSIDE_TYPES = 16
SIZES = [40, 36, 8, 98, 96, 16]          # IMPAI_FTYPE, FWEAPON, FDMG, FSIDE, FIT, FRESULT
SIDE = struct.Struct("<" + "HHH" * FSIDE_TYPES + "BB")
RESULT = struct.Struct("<fffH2x")
FIT = struct.Struct("<" + "f" * NCOEF)
NM_FEV = 4000                            # evaluations a Nelder-Mead run
NM_RUNS = 25                             # restarts at most
COORD_ROUNDS = 200                       # coordinate rounds at most
BOX = [(-4.0, 4.0)] * 20 + [(0.0, 4.0)] * 3 + [(0.01, 2.0)]
CMA_SEED, CMA_POP, CMA_SIGMA = 1, 24, 0.15
CMA_GENS, CMA_STALL = 1500, 60
PEEK_CHUNK = 256                         # bytes a peek line reads (tagpu_peek.h)
RESERVED_TYPE = "None"                   # unit type 0's name in the engine (compare_defs)


class CouldNotRun(Exception):
    pass


def f32(x: float) -> float:
    return struct.unpack("<f", struct.pack("<f", x))[0]


def fnv1a32(text: str) -> int:
    h = 0x811C9DC5
    for b in text.encode():
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def fnv1a64(data: bytes, h: int = 0xCBF29CE484222325) -> int:
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def in_check_half(stem: str) -> bool:
    return fnv1a32(stem) & 1 == 1


# ---- the content and the library ----

class Content:
    def __init__(self, names, t: bytes, w: bytes, d: bytes):
        if len(t) % SIZES[0] or len(w) % SIZES[1] or len(d) % SIZES[2]:
            raise CouldNotRun("the content's tables are not whole rows")
        self.names, self.t, self.w, self.d = list(names), t, w, d
        self.nt, self.nw, self.nd = len(t) // SIZES[0], len(w) // SIZES[1], len(d) // SIZES[2]
        if len(self.names) != self.nt:
            raise CouldNotRun(f"{len(self.names)} names for {self.nt} types")
        self.index = {}
        for i, n in enumerate(self.names):
            self.index.setdefault(n.upper(), i)

    @classmethod
    def load(cls, path):
        doc = json.loads(Path(path).read_text())
        if doc.get("schema") != SCHEMA:
            raise CouldNotRun(f"{path}: not {SCHEMA}")
        return cls(doc["names"], bytes.fromhex(doc["ftype"]), bytes.fromhex(doc["fweapon"]),
                   bytes.fromhex(doc["fdmg"]))

    def digest(self) -> int:
        return fnv1a64(self.d, fnv1a64(self.w, fnv1a64(self.t)))


class Lib:
    """tools/impai/fightlib.c, loaded; refuses a library whose struct sizes are not the ones packed here."""

    def __init__(self):
        self.so = ctypes.CDLL(str(native.fightlib()))
        got = (ctypes.c_uint32 * 6)()
        self.so.fightlib_sizes(got)
        if list(got) != SIZES:
            raise CouldNotRun(f"fightlib's struct sizes {list(got)} are not {SIZES}")
        self.so.fightlib_batch.restype = ctypes.c_uint
        self.batches = {}

    def compiled_params(self):
        buf = ctypes.create_string_buffer(FIT.size)
        self.so.fightlib_params(buf)
        return list(FIT.unpack(buf.raw))

    def margins(self, content: Content, theta, fights, key=None):
        """[(rc, sa, sb, m, steps)] of the fights under theta; the packed fights are kept under `key`."""
        if key is None or key not in self.batches:
            a = b"".join(pack_side(f["a"], f["a_closes"]) for f in fights)
            b = b"".join(pack_side(f["b"], f["b_closes"]) for f in fights)
            dist = struct.pack(f"<{len(fights)}i", *(int(f["dist"]) for f in fights))
            packed = (ctypes.create_string_buffer(a, len(a)), ctypes.create_string_buffer(b, len(b)),
                      ctypes.create_string_buffer(dist, len(dist)))
            if key is not None:
                self.batches[key] = packed
        else:
            packed = self.batches[key]
        n = len(fights)
        out = ctypes.create_string_buffer(RESULT.size * n)
        rc = (ctypes.c_int32 * n)()
        p = ctypes.create_string_buffer(FIT.pack(*theta), FIT.size)
        self.so.fightlib_batch(content.t, ctypes.c_uint32(content.nt), content.w, ctypes.c_uint32(content.nw),
                               content.d, ctypes.c_uint32(content.nd), p, packed[0], packed[1], packed[2],
                               ctypes.c_uint32(n), out, rc)
        return [(rc[k],) + RESULT.unpack_from(out.raw, k * RESULT.size) for k in range(n)]


def pack_side(entries, closes) -> bytes:
    if len(entries) > FSIDE_TYPES:
        raise CouldNotRun(f"a side of {len(entries)} types: the fights here hold at most {FSIDE_TYPES}")
    flat = []
    for t, n, hp in entries:
        flat += [t, n, hp]
    flat += [0, 0, 0] * (FSIDE_TYPES - len(entries))
    return SIDE.pack(*flat, len(entries), 1 if closes else 0)


# ---- the fights, from the references ----

def newest_ref(d: Path):
    best = None
    for p in d.glob("*.json"):
        try:
            doc = json.loads(p.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        key = (doc.get("created", ""), p.stat().st_mtime)
        if best is None or key > best[0]:
            best = (key, p, doc)
    return best


def scenario_fight(doc: dict, content: Content, label: str) -> dict:
    sides = {0: {}, 1: {}}
    pos = {0: [], 1: []}
    closes = {0: False, 1: False}
    for u in doc.get("units") or []:
        o = u.get("owner")
        if o not in sides:
            raise CouldNotRun(f"{label}: unit {u.get('id')} of owner {o}: a fight has owners 0 and 1")
        t = content.index.get(str(u["type"]).upper())
        if t is None:
            raise CouldNotRun(f"{label}: {u['type']} is no type of the content")
        hp = max(1, min(65535, round(65535 * float(u.get("health", 100)) / 100.0)))
        key = (t, hp)
        sides[o][key] = sides[o].get(key, 0) + 1
        pos[o].append(u["pos"])
        closes[o] |= bool(u.get("orders"))
    if not pos[0] or not pos[1]:
        raise CouldNotRun(f"{label}: a side has no unit")
    ca = [sum(p[k] for p in pos[0]) / len(pos[0]) for k in (0, 1)]
    cb = [sum(p[k] for p in pos[1]) / len(pos[1]) for k in (0, 1)]
    return {"a": [(t, n, hp) for (t, hp), n in sorted(sides[0].items())], "a_closes": closes[0],
            "b": [(t, n, hp) for (t, hp), n in sorted(sides[1].items())], "b_closes": closes[1],
            "dist": round(math.hypot(ca[0] - cb[0], ca[1] - cb[1]))}


def load_fights(refs: Path, content: Content):
    """(fights, skipped [(id, why)]): every reference graded for the winner, its newest file."""
    fights, skipped = [], []
    for d in sorted(p for p in refs.iterdir() if p.is_dir()) if refs.is_dir() else []:
        got = newest_ref(d)
        if not got:
            continue
        _key, path, ref = got
        scn = ref.get("scenario") or {}
        if "winner" not in (scn.get("calib") or {}).get("metrics", []):
            continue
        f = Path(scn.get("file", ""))
        f = f if f.is_absolute() else TREE / f
        stem = f.stem
        if not f.exists() or calibref.sha256_file(f) != scn.get("sha256"):
            skipped.append((stem, "its scenario file is gone or changed since the reference"))
            continue
        runs = [r for r in ref.get("runs", []) if r.get("start_check") == "ok"
                and isinstance(r.get("metrics"), dict) and "winner" in r["metrics"]]
        if not runs:
            skipped.append((stem, "no usable run"))
            continue
        try:
            fight = scenario_fight(json.loads(f.read_text()), content, stem)
        except CouldNotRun as e:
            skipped.append((stem, str(e)))
            continue
        w = sum(1.0 if r["metrics"]["winner"] == "A" else 0.5 if r["metrics"]["winner"] == "draw" else 0.0
                for r in runs)
        ms = [r["metrics"]["margin"] for r in runs if isinstance(r["metrics"].get("margin"), (int, float))]
        fight.update({"id": stem, "kind": stem.split("-", 1)[0], "area": scn["calib"].get("area", ""),
                      "W": w, "n": len(runs), "M": sum(ms) / len(ms) if len(ms) == len(runs) else None,
                      "ref": f"{d.name}/{path.name}"})
        fights.append(fight)
    return fights, skipped


def refset_hash(fights) -> str:
    import hashlib
    return hashlib.sha256("\n".join(sorted(f["ref"] for f in fights)).encode()).hexdigest()[:12]


# ---- the fit ----

def loss_of(ms, fights, tau):
    import numpy as np
    m = np.asarray(ms, dtype=np.float64)
    W = np.asarray([f["W"] for f in fights], dtype=np.float64)
    n = np.asarray([f["n"] for f in fights], dtype=np.float64)
    x = m / max(tau, 1e-3)
    nll = -(W * -np.logaddexp(0.0, -x) + (n - W) * -np.logaddexp(0.0, x)).sum()
    have = [k for k, f in enumerate(fights) if f["M"] is not None]
    if have:
        M = np.asarray([fights[k]["M"] for k in have], dtype=np.float64)
        nll += LAMBDA * (n[have] * (m[have] - M) ** 2).sum()
    return float(nll)


class Fitter:
    def __init__(self, lib: Lib, content: Content, fights, start=START, say=print):
        self.lib, self.content, self.fights, self.start, self.say = lib, content, fights, list(start), say
        self.evals = 0
        base = self.ms(self.start)
        self.free = [TAU]
        for k in range(NCOEF - 1):
            th = list(self.start)
            th[k] = th[k] * 1.5 if th[k] else 0.5
            if self.ms(th) != base:
                self.free.append(k)
        self.free.sort()
        self.held = [NAMES[k] for k in range(NCOEF) if k not in self.free]

    def ms(self, theta):
        self.evals += 1
        got = self.lib.margins(self.content, theta, self.fights, key="fit")
        bad = [self.fights[k]["id"] for k, g in enumerate(got) if g[0]]
        if bad:
            raise CouldNotRun(f"the predictor refused {len(bad)} fights, e.g. {bad[:3]}")
        return [g[3] for g in got]

    def theta(self, x):
        th = list(self.start)
        for k, v in zip(self.free, x):
            th[k] = f32(float(v))
        return th

    def loss(self, x):
        th = self.theta(x)
        return loss_of(self.ms(th), self.fights, th[TAU])

    def global_stage(self, x, best):
        """CMA-ES over the free coefficients' box, from x; the best point it evaluated, if better."""
        import numpy as np
        import cmaes
        lo = np.asarray([BOX[k][0] for k in self.free])
        hi = np.asarray([BOX[k][1] for k in self.free])

        def real(y):
            return lo + (cmaes.CMAES.project(y) + 1.0) / 2.0 * (hi - lo)
        es = cmaes.CMAES(len(x), CMA_POP, CMA_SEED, CMA_SIGMA, mean=(x - lo) / (hi - lo) * 2.0 - 1.0)
        stall = 0
        for gen in range(CMA_GENS):
            ys = es.ask()
            xs = [real(y) for y in ys]
            fs = [self.loss(v) for v in xs]
            es.tell(ys, fs)
            i = int(np.argmin(fs))
            if fs[i] < best - 1e-6 * abs(best):
                stall = 0
            else:
                stall += 1
            if fs[i] < best:
                x, best = np.asarray(xs[i]), float(fs[i])
            if stall >= CMA_STALL:
                break
        self.say(f"predfit: CMA-ES: loss {best:.6f} after {gen + 1} generations")
        return x, best

    def run(self):
        import numpy as np
        from scipy.optimize import minimize
        x = np.asarray([self.start[k] for k in self.free], dtype=np.float64)
        best = self.loss(x)
        self.say(f"predfit: {len(self.fights)} training fights, {len(self.free)} free coefficients"
                 f"{' (held: ' + ', '.join(self.held) + ')' if self.held else ''}; start loss {best:.6f}")
        x, best = self.global_stage(x, best)
        for r in range(NM_RUNS):
            res = minimize(self.loss, x, method="Nelder-Mead",
                           options={"maxfev": NM_FEV, "xatol": 1e-5, "fatol": 1e-9, "adaptive": True})
            gain = best - float(res.fun)
            if res.fun < best:
                x, best = np.asarray(res.x, dtype=np.float64), float(res.fun)
            self.say(f"predfit: Nelder-Mead run {r + 1}: loss {best:.6f} ({res.nfev} evaluations)")
            if gain < 1e-4 * max(abs(best), 1e-12):
                break
        for _round in range(COORD_ROUNDS):
            moved = False
            for i in range(len(x)):
                for factor in (1.1, 0.9):
                    y = x.copy()
                    y[i] = y[i] * factor if y[i] else (0.1 if factor > 1 else -0.1)
                    v = self.loss(y)
                    if v < best:
                        x, best, moved = y, v, True
            if not moved:
                break
        self.say(f"predfit: final loss {best:.6f} after {self.evals} evaluations")
        return self.theta(x), best


# ---- the coefficients as C ----

def c_float(v: float) -> str:
    v = f32(v)
    if v == 0.0:
        return "0x0p+0f"
    h = float(v).hex()                        # -0x1.99999a0000000p-3
    sign = "-" if h.startswith("-") else ""
    mant, exp = h.lstrip("-")[2:].split("p")
    whole, _, frac = mant.partition(".")
    frac = frac.rstrip("0")
    return f"{sign}0x{whole}{'.' + frac if frac else ''}p{exp}f"


def emit_c(theta, what: str) -> str:
    rows = []
    for k, name in enumerate(KINDS):
        vals = ", ".join(c_float(theta[k * 4 + h]) for h in range(4))
        rows.append(f"        {{ {vals} }},   /* {name} */")
    tail = [f"    {c_float(theta[20 + i])},   /* {n} */" for i, n in enumerate(("close", "focus", "splash", "tau"))]
    return ("/* impai_fitparams.c -- the fight predictor's coefficients (impai_fight.h, IMPAI_FIT), as\n"
            f"   {what} */\n"
            "#include \"impai_fight.h\"\n\n"
            "const IMPAI_FIT impai_fitparams = {\n"
            "    {   /* hit: bias, lead, spread, area */\n" + "\n".join(rows) + "\n    },\n" + "\n".join(tail) +
            "\n};\n")


def read_params(path: Path):
    text = Path(path).read_text()
    body = text[text.index("impai_fitparams"):]
    vals = re.findall(r"-?0x[0-9a-fA-F]+(?:\.[0-9a-fA-F]*)?p[+-]?\d+f", body)
    if len(vals) != NCOEF:
        raise CouldNotRun(f"{path}: {len(vals)} coefficients, not {NCOEF}")
    return [f32(float.fromhex(v[:-1])) for v in vals]


# ---- the check ----

def judge(fights, ms):
    """(agreeing, decisive, disagreeing [(fight, m)]) over the fights' decisive ones."""
    agree, dec, bad = 0, 0, []
    for f, m in zip(fights, ms):
        rate = f["W"] / f["n"]
        if DECISIVE >= rate >= 1 - DECISIVE:
            continue
        dec += 1
        if (m > 0) == (rate > 0.5) and m != 0:
            agree += 1
        else:
            bad.append((f, m))
    return agree, dec, bad


def pct(a, b):
    return f"{a}/{b} ({100.0 * a / b:.1f} %)" if b else "0/0"


def check_report(fights, ms, say=print):
    """The report; (passed, could_not_run, doc)."""
    half = [in_check_half(f["id"]) for f in fights]
    groups = {"check": [k for k, h in enumerate(half) if h], "training": [k for k, h in enumerate(half) if not h],
              "all": list(range(len(fights)))}
    doc = {"halves": {}, "by": {}, "disagree": []}
    for name, ks in groups.items():
        a, d, _bad = judge([fights[k] for k in ks], [ms[k] for k in ks])
        doc["halves"][name] = {"fights": len(ks), "decisive": d, "agree": a}
        say(f"{name:8s}: {len(ks)} fights, decisive {pct(a, d)}")
    ks = [k for k in groups["check"] if fights[k]["kind"] == "duel"]
    a, d, _ = judge([fights[k] for k in ks], [ms[k] for k in ks])
    say(f"duels alone (check half): decisive {pct(a, d)}")
    doc["duels"] = {"decisive": d, "agree": a}
    by = {}
    for k in groups["check"]:
        f = fights[k]
        pair = f["area"].rsplit("/", 1)[-1] if f["area"] else "?"
        by.setdefault((f["kind"], pair), []).append(k)
    for key in sorted(by):
        a, d, _ = judge([fights[k] for k in by[key]], [ms[k] for k in by[key]])
        if d:
            say(f"  {key[0]:8s} {key[1]:24s} {pct(a, d)}")
            doc["by"][f"{key[0]}/{key[1]}"] = {"decisive": d, "agree": a}
    a, d, bad = judge([fights[k] for k in groups["check"]], [ms[k] for k in groups["check"]])
    for f, m in bad:
        say(f"  DISAGREE {f['id']}: engine A wins {f['W']:g}/{f['n']}, m {m:+.4f}")
        doc["disagree"].append({"id": f["id"], "W": f["W"], "n": f["n"], "m": m})
    if d < MIN_DECISIVE:
        say(f"predfit check: COULD NOT RUN: {d} decisive fights in the check half, at least {MIN_DECISIVE} "
            f"needed (generate more)")
        return False, True, doc
    ok = a >= PASS * d
    say(f"predfit check: {'PASS' if ok else 'FAIL'}: {pct(a, d)} of the check half's decisive fights, "
        f"{PASS:.0%} needed")
    return ok, False, doc


# ---- the content, read out of the engine ----

ROLE_RX = re.compile(r"impai: role (\S+) ")
FIGHT_RX = re.compile(r"impai: fight types=(\d+) weapons=(\d+) dmgrows=(\d+) at=0x([0-9A-Fa-f]{8}) "
                      r"0x([0-9A-Fa-f]{8}) 0x([0-9A-Fa-f]{8})")
DIGEST_RX = re.compile(r"impai: fight digest=([0-9A-Fa-f]{16})")


def peek_bytes(R, lane_log, w, addr: int, n: int) -> bytes:
    """n bytes at addr, read by the game's own peek in PEEK_CHUNK lines of one trigger."""
    import time
    if n == 0:
        return b""
    # the peek reads a spec's size in hexadecimal, as its address (tagpu_peek.h)
    sizes = {a: min(PEEK_CHUNK, addr + n - a) for a in range(addr, addr + n, PEEK_CHUNK)}
    specs = [f"0x{a:08X}:x{k:X}" for a, k in sizes.items()]
    lane_log.read()
    start = len(lane_log.lines)
    R._CLI_API.put_trigger(w.gamedir / "tagpu_peek.trigger", "\n".join(specs) + "\n")
    end = time.monotonic() + R.ACK_S * 4
    while True:
        lane_log.read()
        rows = R._CLI_API._peek_rows("\n".join(lane_log.lines[start:]), specs)
        if len(rows) == len(specs):
            break
        if time.monotonic() >= end:
            raise CouldNotRun(f"peek of {n} bytes at 0x{addr:08X}: {len(rows)} of {len(specs)} lines answered")
        R.nap(lane_log.NAP_S)
        lane_log.alive()
    out = b""
    for row, (a, k) in zip(rows, sizes.items()):
        try:
            got = bytes.fromhex(row["value"].replace(" ", ""))
        except ValueError:
            raise CouldNotRun(f"peek {row['spec']}: {row['value']}")
        if len(got) != k:
            raise CouldNotRun(f"peek {row['spec']}: {len(got)} bytes for the {k} asked at 0x{a:08X}")
        out += got
    return out


def compare_defs(content: Content) -> list:
    """Every difference between the content's numbers and `tasim_run defs`' images of the same install: each type
    by its name (type i is the engine's def i, the `impai: role` lines' order), each weapon a type names by its id,
    every field compared as the number its image row holds, whatever the row's width."""
    rc, out, err = native.run("defs")
    if rc:
        raise CouldNotRun(f"tasim_run defs could not run ({rc}): {err.strip()[-300:]}")
    doc = json.loads(out)
    fails = []

    def image(entry):
        return {row[0]: bytes.fromhex(row[2]) for row in entry["image"]}

    def want(img, off, got: bytes, what, signed=False):
        if off not in img:
            fails.append(f"{what}: +0x{off:X} is not in tasim_run's image")
            return
        a = int.from_bytes(got, "little", signed=signed)
        b = int.from_bytes(img[off], "little", signed=signed)
        if a != b:
            fails.append(f"{what}: +0x{off:X} engine {a} files {b}")

    units = {u["unitname"].upper(): u for u in doc["units"]}
    # Type 0 is the engine's reserved type `None`, which no FBI defines: the loader gives it the kept bit
    # at 0x42AB02 and the name sort covers [1, count) (0x42D54F, 0x42D557), so it stays first
    # (engine/memory.md). It has no files to compare with; a type 0 of any other name is a failure.
    if content.nt and content.names[0] != RESERVED_TYPE:
        fails.append(f"type 0 is {content.names[0]}, not the engine's reserved {RESERVED_TYPE}")
    for i in range(1, content.nt):
        row = content.t[i * SIZES[0]:(i + 1) * SIZES[0]]
        u = units.get(content.names[i].upper())
        if u is None:
            fails.append(f"type {i} {content.names[i]}: no such unit in the files")
            continue
        img = image(u)
        want(img, 0x1FA, row[0:4], f"{content.names[i]} maxdamage")
        want(img, 0x192, row[8:12], f"{content.names[i]} maxvelocity", signed=True)
    weapons = {w["id"]: w for w in doc["weapons"]}
    used = set()
    for i in range(1, content.nt):
        used |= {x for x in struct.unpack_from("<10H", content.t, i * SIZES[0] + 12) if x}
    for wi in sorted(used):
        row = content.w[wi * SIZES[1]:(wi + 1) * SIZES[1]]
        if wi not in weapons:
            fails.append(f"weapon {wi}: no such id in the files")
            continue
        img, name = image(weapons[wi]), weapons[wi].get("section", str(wi))
        for off, at, size, what, signed in ((0xDC, 0, 4, "range", True), (0x68, 4, 4, "velocity", True),
                                            (0xE4, 8, 2, "reload", False), (0xEA, 10, 2, "burst", False),
                                            (0xD4, 12, 2, "damage", False), (0xD6, 14, 2, "area", False),
                                            (0xD8, 16, 4, "edge", False)):
            want(img, off, row[at:at + size], f"{name} {what}", signed)
    return fails


def cmd_content(a) -> int:
    import runlock
    import talog
    import tareplay as R
    cfg = calibref.arena()
    scn = json.loads((TREE / cfg["scenario"]).read_text())
    for p in scn["setup"]["players"]:
        if p.get("slot") == 1:
            p["controller"] = "impai"
            p.pop("ai", None)
    out = Path(a.out).resolve()
    work = out.parent / (out.stem + ".run")
    work.mkdir(parents=True, exist_ok=True)
    launch = work / "fight-content-scenario.json"
    launch.write_text(json.dumps(scn, indent=1) + "\n")
    dll = Path(a.dll).resolve() if a.dll else TREE / "tagpu" / "ddraw" / "ddraw.dll"
    # GDI, as every calibration lane: the content is the same whatever renders, and GDI takes no GPU token
    runlock.hold(TREE, "predfit content", 1, gpu=False)
    w = R.Worker(work, R.TEST_GAME_SPEED)
    lever = None
    try:
        w.gdi = True
        w.held = True
        w.turbo = 1
        w.place({}, dll)
        lever = Path(w.gamedir) / "tagpu_impai_check.on"
        if lever.is_symlink():
            raise CouldNotRun(f"{lever} is a link; refusing to write through it")
        lever.write_text("")
        log = R.Log(w)
        rc, o = R.tacli("scenario", "load", w.name, launch, "--defaults", "--restart", "--keep-dll",
                        "--unit-limit", str(cfg["unit_limit"]), timeout=900)
        if rc:
            raise CouldNotRun(f"scenario load: {R.last(o)}")
        R.held_start(w, log)
        line = log.wait(r"impai: fight (types=|none)", R.ACK_S)
        if not line or not FIGHT_RX.search(line):
            raise CouldNotRun(f"no `impai: fight types=` line: {line or 'none'}")
        m = FIGHT_RX.search(line)
        nt, nw, nd = int(m.group(1)), int(m.group(2)), int(m.group(3))
        at = [int(m.group(k), 16) for k in (4, 5, 6)]
        dl = log.wait(DIGEST_RX.pattern, R.ACK_S)
        if not dl:
            raise CouldNotRun("no `impai: fight digest=` line: the check lever was not read")
        digest = int(DIGEST_RX.search(dl).group(1), 16)
        names = [r.group(1) for r in map(ROLE_RX.search, log.lines) if r]
        if len(names) != nt:
            raise CouldNotRun(f"{len(names)} `impai: role` lines for {nt} fight types: one level init, one each")
        t = peek_bytes(R, log, w, at[0], nt * SIZES[0])
        wb = peek_bytes(R, log, w, at[1], nw * SIZES[1])
        d = peek_bytes(R, log, w, at[2], nd * SIZES[2])
        (work / "tagpu.log").write_text(talog.run_text(w.gamedir))
        w.stop(hard=True)
    except (CouldNotRun, R.CouldNotRun, R.Failed) as e:
        print(f"predfit content: COULD NOT RUN: {e}")
        return 2
    finally:
        try:
            if lever is not None:
                lever.unlink(missing_ok=True)
        finally:
            w.close()
    return finish_content(out, names, t, wb, d, digest, dll)


def finish_content(out: Path, names, t: bytes, wb: bytes, d: bytes, digest: int, dll: Path,
                   compare=compare_defs) -> int:
    """The peeked tables checked -- whole rows, one name a type, their digest the log line's -- compared with
    the files (`compare`, compare_defs), and on PASS alone written to `out` as SCHEMA with the DLL's sha256 and
    the tree's commit, through a temporary name renamed into place: a check that fails leaves no `out`, and an
    `out` from an earlier run is removed first. The exit code: 0 PASS, 1 a check failed, 2 could not run."""
    out = Path(out)
    out.unlink(missing_ok=True)
    try:
        content = Content(names, t, wb, d)
    except CouldNotRun as e:
        print(f"predfit content: FAIL: {e}")
        return 1
    if content.digest() != digest:
        print(f"predfit content: FAIL: the peeked tables' digest {content.digest():016X} is not the line's "
              f"{digest:016X}")
        return 1
    try:
        fails = compare(content)
    except CouldNotRun as e:
        print(f"predfit content: COULD NOT RUN: {e}")
        return 2
    print(f"predfit content: {content.nt} types, {content.nw} weapon rows, {content.nd} [DAMAGE] rows; digest "
          f"{digest:016X} equal")
    for f in fails:
        print(f"  FAIL {f}")
    if fails:
        print("predfit content: FAIL: the engine's numbers against tasim_run defs; nothing written")
        return 1
    tmp = out.with_name(out.name + ".partial")
    tmp.write_text(json.dumps({"schema": SCHEMA, "tree": calibref.git_commit(), "dll": str(dll),
                               "dll_sha256": calibref.sha256_file(dll), "digest": f"{digest:016X}",
                               "names": list(names), "ftype": t.hex(), "fweapon": wb.hex(), "fdmg": d.hex()}) + "\n")
    tmp.replace(out)
    print(f"predfit content: PASS: the engine's numbers against tasim_run defs; written to {out}")
    return 0


# ---- the commands ----

def cmd_fit(a) -> int:
    content = Content.load(a.content)
    fights, skipped = load_fights(Path(a.refs) if a.refs else calibref.refs_dir(), content)
    for s, why in skipped:
        print(f"skipped {s}: {why}")
    train = [f for f in fights if not in_check_half(f["id"])]
    if not train:
        print("predfit fit: COULD NOT RUN: no training fight")
        return 2
    lib = Lib()
    theta, loss = Fitter(lib, content, train).run()
    Path(a.out).write_text(json.dumps({"coefficients": dict(zip(NAMES, theta)), "theta": theta, "loss": loss,
                                       "training_fights": len(train), "refset": refset_hash(fights),
                                       "content_digest": f"{content.digest():016X}"}, indent=1) + "\n")
    print(f"predfit fit: written to {a.out}")
    return 0


def cmd_emit(a) -> int:
    if a.start:
        text = emit_c(START, "`tools/impai/predfit.py emit --start` writes them: the fit's starting point, "
                             "fitted to no reference.")
    else:
        fit = json.loads(Path(a.fit).read_text())
        text = emit_c(fit["theta"], f"`tools/impai/predfit.py emit --fit` writes them: fitted to the training "
                                    f"half ({fit['training_fights']} fights) of reference set {fit['refset']}, "
                                    f"content {fit['content_digest']}.")
    Path(a.out).write_text(text)
    print(f"predfit emit: {a.out}")
    return 0


def cmd_check(a) -> int:
    try:
        content = Content.load(a.content)
        theta = read_params(Path(a.params))
        lib = Lib()
        if Path(a.params).resolve() == PARAMS and lib.compiled_params() != theta:
            raise CouldNotRun("the fight library was built from other coefficients than the file's")
        fights, skipped = load_fights(Path(a.refs) if a.refs else calibref.refs_dir(), content)
        got = lib.margins(content, theta, fights)
    except CouldNotRun as e:
        print(f"predfit check: COULD NOT RUN: {e}")
        return 2
    for s, why in skipped:
        print(f"skipped {s}: {why}")
    refused = [fights[k]["id"] for k, g in enumerate(got) if g[0]]
    if refused:
        print(f"predfit check: COULD NOT RUN: the predictor refused {len(refused)} fights, e.g. {refused[:3]}")
        return 2
    ok, cnr, doc = check_report(fights, [g[3] for g in got])
    if a.json:
        Path(a.json).write_text(json.dumps(doc, indent=1) + "\n")
    return 2 if cnr else 0 if ok else 1


# ---- the self-test ----

SYN_TRUE = [1.2, -0.8, -1.3, 0.6,  0.9, -0.6, -1.1, 0.8,  0.6, -0.9, -0.8, 0.7,  1.0, -0.4, -0.5, 0.5,
            0.5, -0.7, -1.0, 0.9,  0.8, 0.7, 0.35, 0.15]
SYN_FIGHTS = 800
SYN_RUNS = 25
LOSS_SLACK = 0.02              # the fit's training loss at most this share above the truth's
MARGIN_TOL = 0.05               # the fitted margins' mean distance from the true ones on the check half


def synthetic_content(seed=7):
    """A content the model can tell every coefficient apart on: ground types with weapons of every kind,
    ranges, speeds, accuracies and areas spread wide; a few with [DAMAGE] rows."""
    import random
    r = random.Random(seed)
    kinds = [1 << 3, 0, 1 << 1, 1 << 12, 1 << 8]           # beam, direct, ballistic, guided, dropped
    weapons = [bytes(36)]
    dmg = b""
    nd = 0
    for i in range(1, 61):
        k = kinds[i % 5]
        rng = r.randint(220, 950)
        vel = 0 if k == 1 << 3 else round(r.uniform(2.5, 14.0) * 65536)
        rows = b""
        nrows = 0
        if i % 7 == 0:
            for t in sorted(r.sample(range(1, 31), 3)):
                rows += struct.pack("<HHi", t, 0, r.randint(20, 400))
                nrows += 1
        weapons.append(struct.pack("<ii4HfHHIIH2B", rng, vel, r.randint(20, 120), r.randint(1, 3),
                                   r.randint(30, 320), r.choice((0, 0, 24, 48, 96)), r.uniform(0.0, 0.6),
                                   r.randint(0, 700), r.randint(0, 900), k, nd, nrows, [0, 1, 2, 3, 4][i % 5], 0))
        dmg += rows
        nd += nrows
    types = [bytes(40)]
    names = ["NONE"]
    for i in range(1, 31):
        ws = [0] * 10
        for s in range(r.randint(1, 2)):
            ws[s] = r.randint(1, 60)
        mobile = i <= 26
        types.append(struct.pack("<Ifi10H4BH2B", r.randint(1500, 9000), float(r.randint(60, 900)),
                                 round(r.uniform(0.6, 2.6) * 65536) if mobile else 0, *ws, 0, r.randint(2, 4),
                                 r.randint(2, 4), 1 if mobile else 0, 0, 6 if mobile else 1, 255))
        names.append(f"SYN{i}")
    return Content(names, b"".join(types), b"".join(weapons), dmg)


def synthetic_fights(lib: Lib, content: Content, theta, seed=11):
    """Fights the way duelgen.py makes them: one to three types a side, side B scaled to a value ratio of
    RATIOS to side A, so that most are close enough for the coefficients to move their outcome."""
    import random
    r = random.Random(seed)
    value = [struct.unpack_from("<f", content.t, i * 40 + 4)[0] for i in range(content.nt)]
    fights = []
    for k in range(SYN_FIGHTS):
        def side(structures):
            pool = list(range(1, 27)) + (list(range(27, 31)) if structures else [])
            return [(t, r.randint(2, 10), 65535 if r.random() < 0.8 else r.randint(30000, 65535))
                    for t in r.sample(pool, r.randint(1, 3))]
        a, b = side(False), side(k % 4 == 0)
        scale = r.choice((0.5, 0.75, 1.0, 1.33, 2.0)) * sum(value[t] * n for t, n, _h in a) / \
            sum(value[t] * n for t, n, _h in b)
        b = [(t, max(1, min(60, round(n * scale))), hp) for t, n, hp in b]
        fights.append({"id": f"syn-{k:04d}", "kind": "syn", "area": f"syn/{len(b)}", "a": a, "a_closes": True,
                       "b": b, "b_closes": r.random() < 0.85, "dist": r.randint(150, 1500), "n": SYN_RUNS,
                       "ref": f"syn-{k:04d}"})
    got = lib.margins(content, theta, fights)
    for f, g in zip(fights, got):
        if g[0]:
            raise CouldNotRun(f"the synthetic fight {f['id']} was refused")
        p = 1.0 / (1.0 + math.exp(-g[3] / theta[TAU]))
        f["W"], f["M"] = SYN_RUNS * p, g[3]
    return fights


def cmd_selftest(_a) -> int:
    """The fit on fights the model itself made at SYN_TRUE: it must find the truth's basin -- a training loss at
    most LOSS_SLACK above the truth's -- reproduce the truth's margins on the check half within MARGIN_TOL on
    average, and pass the check. Each coefficient's distance from its true value is printed but not judged:
    the coefficients are strongly confounded (the kinds' bias, lead and spread move one hit probability, and
    focus, splash and close trade against them), so many coefficient sets give the same margins. Measured
    2026-11-12 on this set: the fit reached a loss of 1402 against 1378 at the truth, every margin's sign
    right, with single coefficients up to 3x off; an inverse Hessian at the truth came out indefinite, the
    loss being piecewise at that scale, so no standard error could tell the identifiable ones apart."""
    try:
        lib = Lib()
        content = synthetic_content()
        truth = [f32(v) for v in SYN_TRUE]
        fights = synthetic_fights(lib, content, truth)
        train = [f for f in fights if not in_check_half(f["id"])]
        check = [f for f in fights if in_check_half(f["id"])]
        fitter = Fitter(lib, content, train)
        truth_loss = loss_of(fitter.ms(truth), train, truth[TAU])
        theta, loss = fitter.run()
    except CouldNotRun as e:
        print(f"predfit selftest: COULD NOT RUN: {e}")
        return 2
    for k in range(NCOEF):
        if k not in fitter.free:
            print(f"  held {NAMES[k]:20s} start {START[k]:+.4f} (true {truth[k]:+.4f}): moves no fight")
        else:
            print(f"       {NAMES[k]:20s} fit {theta[k]:+.5f} true {truth[k]:+.5f} "
                  f"({100 * abs(theta[k] - truth[k]) / abs(truth[k]):.1f} %)")
    got = lib.margins(content, theta, fights)
    want = lib.margins(content, truth, check)
    fitted = {f["id"]: g[3] for f, g in zip(fights, got)}
    dm = sum(abs(fitted[f["id"]] - g[3]) for f, g in zip(check, want)) / len(check)
    ok, cnr, _doc = check_report(fights, [g[3] for g in got])
    fails = []
    if loss > truth_loss * (1 + LOSS_SLACK):
        fails.append(f"training loss {loss:.3f}, more than {LOSS_SLACK:.0%} above the truth's {truth_loss:.3f}")
    if dm > MARGIN_TOL:
        fails.append(f"the check half's margins {dm:.4f} from the truth's on average, past {MARGIN_TOL}")
    if not ok or cnr:
        fails.append("the check did not pass")
    print(f"predfit selftest: training loss {loss:.3f} (truth {truth_loss:.3f}); check-half margins "
          f"{dm:.4f} from the truth's on average")
    if fails:
        print(f"predfit selftest: FAIL: {'; '.join(fails)}")
        return 1
    print("predfit selftest: PASS: the fit reaches the truth's loss, reproduces its margins and passes the check")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("content")
    c.add_argument("--out", required=True)
    c.add_argument("--dll")
    f = sub.add_parser("fit")
    f.add_argument("--content", required=True)
    f.add_argument("--refs")
    f.add_argument("--out", required=True)
    e = sub.add_parser("emit")
    g = e.add_mutually_exclusive_group(required=True)
    g.add_argument("--fit")
    g.add_argument("--start", action="store_true")
    e.add_argument("--out", default=str(PARAMS))
    k = sub.add_parser("check")
    k.add_argument("--content", required=True)
    k.add_argument("--refs")
    k.add_argument("--params", default=str(PARAMS))
    k.add_argument("--json")
    sub.add_parser("selftest")
    a = ap.parse_args(argv)
    return {"content": cmd_content, "fit": cmd_fit, "emit": cmd_emit, "check": cmd_check,
            "selftest": cmd_selftest}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
