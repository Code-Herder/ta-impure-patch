"""metrics.py -- the calibration metrics, computed once from a `ta-calib-trace/1` trace, engine's or tasim's.

    lines = metrics.read_trace(path)                  # or engtrace's lines in memory
    m = metrics.compute(lines, block, values, R)      # {"winner": "A", "ttk": {...}, ...}
    s = metrics.exact_series(lines, "damage")         # the family's values, compared for equality

The definitions are research/notes/modules/tasim-calibration.md's *Metrics*, which this module is
the one implementation of; engtrace.py writes the engine's traces and tasim_run will write its own,
so a metric cannot differ between the two sides by being computed twice.

THE TRACE'S TIME. A line's `t` is GameTime less the run's apply tick t0. Its `u`, `p` and `l` rows
are the state at the end of step t0 + t (t = 0: as applied), its `ev` what step t0 + t did. A
survivor's last `u` row is at t_end - t0 (its state at the end of the last step, from the clear).
Values are raw: ints as the engine holds them (x, altitude and depth in 16.16, HP a short), floats
as float64 reprs of the engine's float32 or double.
"""

import json
import math
from pathlib import Path

U_X, U_ALT, U_DEPTH, U_HP, U_NANO = range(5)
P_E, P_M, P_EINC, P_MINC, P_EMAX, P_MMAX, P_EPROD, P_MPROD = range(8)
TTK_LEFT = 0.10                 # ttk: the loser has lost 90 % of its value
FIXED = 1 << 16                 # x, altitude and depth are 16.16


class TraceError(Exception):
    pass


def read_trace(path):
    p = Path(path)
    if p.suffix == ".zst":
        import zstandard
        text = zstandard.ZstdDecompressor().decompress(p.read_bytes()).decode()
    else:
        text = p.read_text()
    return [json.loads(ln) for ln in text.splitlines() if ln.strip()]


def value_of(values: dict, type_name: str, R: float) -> float:
    """value(type) = metal + energy / R, R the mod's exchange rate (tasim_roles)."""
    row = values[type_name.upper()]
    return row["metal"] + row["energy"] / R


class Replay:
    """The trace's units walked tick by tick: each live unit's last row, its side and type."""

    def __init__(self, lines):
        if not lines or lines[0].get("schema") != "ta-calib-trace/1":
            raise TraceError("not a ta-calib-trace/1 trace")
        self.head = lines[0]
        self.ticks = lines[1:]
        self.limit = self.head["limit"]
        self.meta = dict(self.head["units"])
        self.rows = {}
        self.live = set()

    def walk(self):
        """Yields (t, the line) with the state applied: a create makes a unit live, a death ends
        it, a `u` row is its state from then on."""
        for line in self.ticks:
            for ev in line.get("ev", ()):
                if ev[0] == "create":
                    h = ev[1]
                    self.meta.setdefault(h, {"type": ev[2], "side": ev[3]})
                    self.live.add(h)
                elif ev[0] == "death":
                    self.live.discard(ev[1])
            for h, row in line.get("u", {}).items():
                self.rows[h] = row
            yield line["t"], line


def hv(row, value, maxdamage):
    """hv(unit) = value × hp / maxdamage × (1 − nano fraction left)."""
    return value * max(row[U_HP], 0) / maxdamage * (1.0 - row[U_NANO])


def side_hv(rp: Replay, side: str, values: dict, R: float) -> float:
    out = 0.0
    for h in rp.live:
        m = rp.meta[h]
        if m["side"] != side or h not in rp.rows:
            continue
        t = m["type"].upper()
        out += hv(rp.rows[h], value_of(values, t, R), values[t]["maxdamage"])
    return out


def hv_curve(lines, values, R):
    """[(t, HV_A, HV_B, live A, live B)] at every line of the trace."""
    rp = Replay(lines)
    out = []
    for t, _line in rp.walk():
        la = sum(1 for h in rp.live if rp.meta[h]["side"] == "A")
        lb = sum(1 for h in rp.live if rp.meta[h]["side"] == "B")
        out.append((t, side_hv(rp, "A", values, R), side_hv(rp, "B", values, R), la, lb))
    return out


def combat(lines, values, R):
    """winner, ttk, survivors and margin (the module note's definitions): margin is side A's
    HV(t_e)/HV(0) less side B's, the measure ImpAI's fight predictor is fitted to."""
    curve = hv_curve(lines, values, R)
    if not curve or curve[0][0] != 0:
        raise TraceError("the trace has no line at t = 0")
    L = Replay(lines).limit
    _t0, a0, b0, _la0, _lb0 = curve[0]
    if a0 <= 0 or b0 <= 0:
        raise TraceError(f"a side starts with no value (A {a0}, B {b0})")
    elim = next((t for t, _a, _b, la, lb in curve[1:] if la == 0 or lb == 0), None)
    t_e = min(elim, L) if elim is not None else L
    at = [c for c in curve if c[0] <= t_e][-1]
    ra, rb = at[1] / a0, at[2] / b0
    winner = "A" if ra > rb else "B" if rb > ra else "draw"
    losers = ("B",) if winner == "A" else ("A",) if winner == "B" else ("A", "B")
    ttk = None
    for t, ha, hb, _la, _lb in curve:
        if any((ha if s == "A" else hb) <= TTK_LEFT * (a0 if s == "A" else b0) for s in losers):
            ttk = t
            break
    survivors = rb if winner == "B" else ra
    return {"winner": winner, "margin": ra - rb,
            "ttk": {"value": ttk if ttk is not None and ttk <= L else L,
                    "censored": ttk is None or ttk > L},
            "survivors": survivors, "t_e": t_e}


def income(lines, res: str, at: int):
    """The side's produced(c) − produced(0), per side: the lifetime double +0xB4 (metal) or +0xAC
    (energy), from the last economy-pass row at or before c."""
    idx = P_MPROD if res == "metal" else P_EPROD
    last, first = {}, {}
    for line in lines[1:]:
        if line["t"] > at:
            break
        for side, row in line.get("p", {}).items():
            first.setdefault(side, row[idx])
            last[side] = row[idx]
    return {s: last[s] - first[s] for s in sorted(last)}


def travel(lines, fp_cells: int, L: int):
    """The ticks at which each unit with a move order (its `dest` in the trace's head) first comes
    within R_arr = 16·(fp+1)·⌈√n⌉ world units of it: the first, the ⌈n/2⌉-th and the last, each
    censored at L. fp is the movers' largest footprint, in cells."""
    rp = Replay(lines)
    movers = [h for h, m in rp.meta.items() if m.get("dest") is not None]
    if not movers:
        raise TraceError("a travel metric needs units with a move order")
    n = len(movers)
    r_arr = 16 * (fp_cells + 1) * math.ceil(math.sqrt(n))
    arrived = {}
    for t, _line in rp.walk():
        for h in movers:
            row = rp.rows.get(h)
            if h in arrived or row is None:
                continue
            dest = rp.meta[h]["dest"]
            dx = row[U_X] / FIXED - dest[0]
            dz = row[U_DEPTH] / FIXED - dest[1]
            if dx * dx + dz * dz <= r_arr * r_arr:
                arrived[h] = t
    ts = sorted(arrived.values()) + [None] * (n - len(arrived))

    def pick(k):
        v = ts[k]
        return {"value": v if v is not None else L, "censored": v is None}
    return {"first": pick(0), "median": pick(math.ceil(n / 2) - 1), "last": pick(n - 1)}


def compute(lines, block: dict, values: dict, R: float) -> dict:
    """Every metric the scenario's block names, but the exact ones (exact_series)."""
    out = {}
    want = set(block["metrics"])
    if want & {"winner", "ttk", "survivors", "margin"}:
        c = combat(lines, values, R)
        out.update({k: c[k] for k in ("winner", "ttk", "survivors", "margin") if k in want})
    for m in block["metrics"]:
        if m.startswith("income."):
            base, _, at = m.partition("@")
            out[m] = income(lines, base.split(".")[1], int(at))
    if want & {"travel.first", "travel.median", "travel.last"}:
        movers = [m["type"].upper() for m in lines[0]["units"].values() if m.get("dest")]
        fp = max((max(values[t]["footprint"]) for t in movers), default=1)
        tr = travel(lines, fp, block["limit"])
        for k in ("first", "median", "last"):
            if f"travel.{k}" in want:
                out[f"travel.{k}"] = tr[k]
    return out


def scalars(metric: str, v):
    """[(name, number)] a distribution rule grades: a censored value enters as its limit, and an
    income is graded per side, `income.metal@1800/A`."""
    if isinstance(v, dict) and "value" in v:
        return [(metric, v["value"])]
    if isinstance(v, dict):
        return [(f"{metric}/{s}", v[s]) for s in sorted(v)]
    return [(metric, v)]


def exact_series(lines, family: str):
    """The family's values, tick by tick, in the trace's raw representation: two runs (or the
    engine and tasim) agree on the family exactly when these lists are equal."""
    out = []
    rows = {}
    for line in lines[1:]:
        t = line["t"]
        for h, row in line.get("u", {}).items():
            prev = rows.get(h)
            if family == "damage" and prev is not None and row[U_HP] < prev[U_HP]:
                out.append([t, h, prev[U_HP], row[U_HP]])
            rows[h] = row
        for ev in line.get("ev", ()):
            if family in ("reload", "range") and ev[0] == "shot":
                out.append([t, ev[1], ev[2], ev[3]])
            elif family == "build_time" and ev[0] in ("create", "built"):
                out.append([t] + ev)
        if family == "build_time":
            for h, row in line.get("u", {}).items():
                if row[U_NANO] != 0.0:
                    out.append([t, h, row[U_NANO]])
        elif family == "cost_per_tick":
            for h, row in sorted(line.get("l", {}).items()):
                out.append([t, h] + row)
        elif family in ("extractor", "storage"):
            for side, row in sorted(line.get("p", {}).items()):
                keep = (row[P_MINC], row[P_MPROD]) if family == "extractor" else \
                       (row[P_EMAX], row[P_MMAX])
                out.append([t, side, *keep])
    if family == "range":
        first = {}
        for t, h, _w, tgt in out:
            if tgt is not None and tgt not in first:
                first[tgt] = t
        return sorted(first.items())
    if family == "storage":
        # a seat row comes at t = 0 (the applied state) and at each economy pass after it
        # (engtrace.Run.images), so the rows past t = 0 are the passes' storage
        return [row for row in out if row[0] > 0]
    if family == "sight":
        sight = lines[0].get("sight")
        if sight is None:
            raise TraceError("a sight scenario's trace carries the dump's cells in its head")
        return sight
    return out


def first_difference(a, b):
    """(index, engine item, other item) of the first place two series differ, or None."""
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i, x, y
    if len(a) != len(b):
        i = min(len(a), len(b))
        return i, a[i] if i < len(a) else None, b[i] if i < len(b) else None
    return None
