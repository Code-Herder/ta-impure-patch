"""engtrace.py -- the engine side of a calibration run: its trace, and the checks that make the reload a fact.

    for res in engtrace.lane(lane_dir, arena_cfg):    # one lane-launch's recording, read once
        res.row, res.lines, res.checks, res.start_check, res.exogenous

The recording is streamed once (tarec.stream_to) and sliced by the manifest's windows; each run's
trace is `ta-calib-trace/1` (metrics.py, THE TRACE'S TIME), and each run is checked R1..R8
(research/notes/modules/tasim-calibration.md, *The per-run checks*). A failed check makes the run
unusable; nothing is tolerated silently. A packed lane's batch is read into one run a cell, with the
batch's checks P1..P5 besides (THE PACKED LANE, below).

THE ITERATION OF AN EVENT MADE ON A PAUSED FRAME. The recorder's capture point is the unit tick's
head (tagpu_recorder.c, tagpu_rec_capture): it publishes the last step's events (`ring_publish(r,
0)`) and then writes RK_TICK with the new GameTime and the images. An event is put in the ring when
the engine makes it, on the game thread (ev_rec, ev_put), whether a step is running or not. So the
apply's creates and the clear's deaths, made on the frames held after step T, come in iteration T,
after step T's own events, and the images of iteration T + 1 are the first to show them. Hence:
  - the empty point is iteration t_apply's images (before the apply), the pool the settle's hold at
    t_apply - 1 read; the apply's state is iteration t_apply + 1's; R2 reads the pool in both;
  - the apply's creates are iteration t_apply's RK_EV_CREATEs, the trace's t = 0 events;
  - the clear's deaths are iteration t_end's last death of each cleared slot: B4's slot hold
    (tagpu_patches.c, hit_held) keeps a slot freed in a step from a create in that step and the
    next, so a cleared slot has no other death in that iteration, which R1 checks.
"""

import hashlib
import json
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(TREE / "tools" / "replay"))
import calibref  # noqa: E402
import pack  # noqa: E402
import perturb  # noqa: E402

# unit image offsets (the engine's unit record; tagpu_recfmt.h REC_UNIT_IMAGE)
U_POS = 0x6A                    # x, altitude, depth: i32 16.16 each
U_NANO = 0x104                  # f32 the build fraction left
U_HP = 0x108                    # i16
U_TYPE = 0xA6                   # u16 the type's index
U_OWNER = 0xFF                  # u8 the owner's seat
U_LEDGER = ((0xBC, 4), (0xD4, 4))   # f32 energy then metal: produced, requested, granted, debt
# player image offsets (the player record, main+0x1B63 + 0x14B·s)
PL_E, PL_EINC, PL_M, PL_MINC, PL_EMAX, PL_MMAX = 0x8C, 0x90, 0x98, 0x9C, 0xA4, 0xA8
PL_EPROD, PL_MPROD = 0xAC, 0xB4  # f64 lifetime produced
PL_BASEFLAG, PL_BASE_E, PL_BASE_M = 0x149, 0xDC, 0xE0
PL_PASS = 0xF0                  # the 30-tick pass deadline: a change is an economy pass
PL_PHASED = (0xF0, 0xF4, 0xF8)  # GameTime deadlines, compared modulo 30 against the apply tick
# R4's exclusions: what a reload leaves behind and no simulation reader takes as a value
# (the module note, *What a reload does not reset*)
PL_LIFETIME = [(0xAC, 0xDC), (0xFC, 0x100), (0x104, 0x108), (0x140, 0x144)]
# +0x148, the scoreboard rank (one byte; +0x149 is the base flag, compared): display state, by its
# readers in the pristine exe (DISASSEMBLED 2026-11-13, research/notes/engine/units.md, `+0x148`):
# the activation 0x464290 writes it; the scoreboard 0x4948E0, called only from DrawGameScreen
# 0x469F65, reads and compacts it; the destructor's rank update 0x4866D0 (network and mode-2 games)
# reads it into the ranks and the lead message alone. No other code reads it, and GDI, which runs
# the scoreboard, moves it between runs where the Vulkan lane does not (owner's decision of
# 2026-11-13, given that proof).
PL_RANK = (0x148, 0x149)
PL_WIRE = [(PL_E, 4), (PL_M, 4), (PL_BASE_E, 4), (PL_BASE_M, 4), (PL_EMAX, 4), (PL_MMAX, 4)]
DEATH_HEAD = 12                 # RK_EV_DEATH: u32 ID, u32 killer, u8 player, i8, u8, u8, image

# THE EMPTY POINT'S ADVANCING SET (R6): the globals and tables a reload leaves at another value
# than run 0's and no metric depends on. An entry added here loosens R6: the owner's decision.
EMPTY_POINT_ADVANCING = {
    # GameTime itself is not in the globals image (RK_TICK carries it); the simulation RNG's
    # state continues from run to run as a fresh launch's starts from the QPC (0x49719D): both
    # arbitrary, recorded per run as exogenous.rng0
    "rngseed",
    # the wind, main+0x37EC4..+0x37EE5 (replay-globals.tsv): Impure's per-level draw continues;
    # random in both, recorded per run as exogenous.wind
    "windnext", "winddivisor", "windx", "windy", "windz", "windheading", "windspeed",
    "windratio", "windchanged",
    # added by the owner's decision of 2026-11-12, each an engine schedule or counter that
    # advances with time (replay-globals.tsv rows):
    # the pathfinder's per-player weights 0x5119E8 (pfweight, 40 bytes), the unit slots its tick
    # examined for each player since their last rewrite 0x511A10 (pfexamined, pfexamined+4, ...)
    # and its tick's calls modulo 150 0x511A38: the tick 0x40EB70 runs in every players phase,
    # counts the slot it examines and rewrites the weights every 150 calls
    "pfweight", "pfexamined", "pfcalls",
    # every moveclass's passability watermark (the 32 rows mcwatermark00..31, the 0x20-byte entries
    # at 0x512358), a GameTime (max(GameTime, 30) − 30) written at each search start 0x40E630
    # through 0x440AF0: a search-start stamp that carries over from run to run, not fight state
    # (owner's decision of 2026-11-13, extending 2026-11-12's for moveclasses 0 and 5)
    *(f"mcwatermark{k:02d}" for k in range(32)),
    # the meteor shower's schedule: its tick 0x437DE0 schedules a shower at every interval and
    # draws its points whatever the switch says (the arena's is off, so no stone falls):
    # the next shower, its end and next strike (GameTimes) and its two points
    "meteornext", "meteorend", "meteorstrike", "meteorp1", "meteorp2",
}
# The declared tables that advance:
# - B4's slot table (tagpu_patches.c, struct hit_tab and its three columns): births and frees
#   advance with every create; its only skirmish reader is the slot hold, which treats every slot
#   freed before the last tick alike (hit_held, hit_reusable);
# - the wind's generator state s_windState (tagpu_patches.c), advanced by the wind tick's draws
#   once for each GameTime value; seeded at the level load (a test launch's fixed seed) and advanced by
#   every run before, as the wind rows above are (owner's decision of 2026-11-12)
EMPTY_POINT_ADVANCING_TABLES = re.compile(r"\bs_hit(Stamp|Freed|Local)?\b|\bs_windState\b")
MODE12 = ("reclaim", "capture")     # the explored map's only simulation reader (R5)


def advancing_rows(rows):
    """EMPTY_POINT_ADVANCING against `rows`, the checkout's replay-globals.tsv: CalibError naming
    each entry no row has. R6 matches the set against the rows' names, which the list's hash does
    not cover (tools/replay-globals-gen.py, THE LIST'S HASH), so a renamed row would leave its
    entry naming nothing."""
    stale = EMPTY_POINT_ADVANCING - {r["name"] for r in rows}
    if stale:
        raise calibref.CalibError(f"EMPTY_POINT_ADVANCING names {', '.join(sorted(stale))}, which "
                                  f"no row of replay-globals.tsv has: rename the entry with its row")


def as_f32(x):
    return struct.unpack("<f", struct.pack("<f", float(x)))[0]


def f32(b, at):
    return struct.unpack_from("<f", b, at)[0]


def f64(b, at):
    return struct.unpack_from("<d", b, at)[0]


def unit_row(img):
    x, alt, depth = struct.unpack_from("<iii", img, U_POS)
    return [x, alt, depth, struct.unpack_from("<h", img, U_HP)[0], f32(img, U_NANO)]


def ledger_row(img):
    return [f32(img, at + 4 * k) for at, n in U_LEDGER for k in range(n)]


def player_row(img):
    return [f32(img, PL_E), f32(img, PL_M), f32(img, PL_EINC), f32(img, PL_MINC),
            f32(img, PL_EMAX), f32(img, PL_MMAX), f64(img, PL_EPROD), f64(img, PL_MPROD)]


class RunCheck(Exception):
    """A run's start check failed: the name of the check and what it found."""


class Checked:
    """What a run, a packed batch or a cell keeps of its checks: each check's verdict (`checks`), what each failure
    names (`fields`, for the build's tally) and the first failure (`failed`); and the empty point's and the apply's
    checks a run and a batch share."""

    def fail(self, name, why, fields=None):
        self.checks[name] = why
        self.fields.setdefault(name, []).extend(fields or [why])
        if self.failed is None:
            self.failed = f"{name}: {why}"

    def ok(self, name):
        self.checks.setdefault(name, "ok")

    def empty_point(self, units, phead, globals_rows, tables, mapped):
        """R1 (no live unit), R2 (no projectile), and the state R4..R6 compare with run 0's."""
        if units:
            self.fail("R1", f"{len(units)} live unit(s) at the empty point, slots "
                            f"{sorted(units)[:8]}")
        else:
            self.ok("R1")
        if phead:
            self.fail("R2", f"{phead} projectile(s) at the empty point")
        else:
            self.ok("R2")
        self.empty = {"globals": globals_rows, "tables": tables, "mapped": mapped}

    def apply_point(self, phead):
        """R2's second read: no projectile in iteration t0 + 1's images, the apply's state before any
        of its units has stepped (engrun.py, THE EMPTY POINT'S POOL)."""
        if phead:
            self.fail("R2", f"{phead} projectile(s) at the apply")

    def compare_run0(self, base, names, table_names, needs_mapped, seats_base=None):
        """R4 against seats_base, the launch's first run of the same scenario and swap (THE SAME
        SCENARIO AND SWAP, Lane.stream), and R5, R6 against base, run 0 of the launch."""
        seats_base = seats_base or base
        if self.players_after is None or seats_base.players_after is None:
            self.fail("R4", "no player images after the apply")
            return
        d = phased_diff(seats_base.players_after, self.players_after, seats_base.t0, self.t0)
        if d:
            self.fail("R4", "; ".join(d[:3]) + (f" (and {len(d) - 3} more)" if len(d) > 3 else ""),
                      d)
        else:
            self.ok("R4")
        if base.empty["mapped"] != self.empty["mapped"]:
            if needs_mapped:
                self.fail("R5", "the explored map differs from run 0's and the scenario reclaims "
                                "or captures")
            else:
                self.checks["R5"] = "differs from run 0's (no mode-12 order: recorded)"
        else:
            self.ok("R5")
        bad = []
        g0, g1 = base.empty["globals"], self.empty["globals"]
        if not g0 or not g1 or len(g0) != len(g1):
            bad.append(f"the globals image ({len(g0)} bytes in run 0, {len(g1)} here)")
        for i in range(min(len(g0), len(g1)) // 4):
            if g0[4 * i:4 * i + 4] != g1[4 * i:4 * i + 4]:
                name = names.get(i, f"dword {i}")
                if not all(p in EMPTY_POINT_ADVANCING for p in re.split(r"[^\w]+", name) if p
                           and not p.isdigit()):
                    bad.append(name)
        for ix, img in self.empty["tables"].items():
            nm = table_names.get(ix, str(ix))
            if base.empty["tables"].get(ix) != img and not EMPTY_POINT_ADVANCING_TABLES.search(nm):
                bad.append(f"table {nm}")
        if bad:
            self.fail("R6", "differs from run 0's empty point: " + ", ".join(sorted(set(bad))),
                      sorted(set(bad)))
        else:
            self.ok("R6")


class Run(Checked):
    """One run's window of the stream: its trace lines and its checks, fed iteration by iteration
    by Lane (or a test's synthetic iterations)."""

    def __init__(self, row: dict, exp: dict, block: dict, values: dict, catalogue_ids: dict):
        self.row, self.exp, self.block, self.values = row, exp, block, values
        self.t0, self.t_end = row["t_apply"], row["t_end"]
        self.swap = row["swap"]
        self.ids = catalogue_ids
        self.lines = {}
        self.slot_handle = {}
        self.made = 0
        self.last_u, self.last_l = {}, {}
        self.checks = {}
        self.fields = {}                # check -> what each failure names, for the build's tally
        self.failed = None
        self.exogenous = None
        self.empty = None               # the empty point: what run 0 of the launch compares with
        self.seat = {"A": 1 if self.swap else 0, "B": 0 if self.swap else 1}
        self.side_of_seat = {v: k for k, v in self.seat.items()}
        self.cleared = set(row["clear"]["cleared_slots"])
        handles = {u["id"]: u for u in exp["units"]}
        self.units = {}
        for h, u in handles.items():
            dest = next((o["to"] for o in u["orders"] if o["cmd"] == "move" and o.get("to")), None)
            # exp is the run's own, its owners already swapped: side A is the scenario's owner 0
            self.units[h] = {"type": u["type"].upper(), "side": "A" if (u["owner"] == 0) != self.swap
                             else "B", "seat": u["owner"], "dest": dest}
        self.delayed = [(u["id"], o["after"], o.get("target")) for u in exp["units"]
                        for o in u["orders"] if o["after"]]
        self.players_after = None

    # ---- the trace --------------------------------------------------------------------------

    def line(self, t):
        return self.lines.setdefault(t, {"t": t})

    def handle(self, slot):
        return self.slot_handle.get(slot)

    def head(self):
        return {"schema": calibref.SCHEMA_TRACE, "scenario": self.row["scenario"],
                "run": self.row["index"], "source": "engine", "t0": self.t0,
                "limit": self.block["limit"],
                "sides": {s: [h for h, u in self.units.items() if u["side"] == s] for s in "AB"},
                "units": self.units}

    def trace(self):
        return [self.head()] + [self.lines[t] for t in sorted(self.lines)
                                if len(self.lines[t]) > 1 or t == 0]

    # ---- iterations -------------------------------------------------------------------------

    def images(self, L, units, players, prev_players, orders, w_extra):
        """Iteration L's images: the state at the end of step L - 1."""
        t = L - 1 - self.t0
        if t < 0 or L > self.t_end:
            return
        ln = self.line(t)
        exact = "exact" in self.block["kind"]
        for slot, h in list(self.slot_handle.items()):
            img = units.get(slot)
            if img is None:
                continue
            r = unit_row(img)
            if self.last_u.get(h) != r:
                ln.setdefault("u", {})[h] = r
                self.last_u[h] = r
            if exact:
                lr = ledger_row(img)
                if any(lr) and self.last_l.get(h) != lr:
                    ln.setdefault("l", {})[h] = lr
                    self.last_l[h] = lr
        for side, seat in self.seat.items():
            img = players[seat]
            if t == 0 or (prev_players is not None and
                          prev_players[seat][PL_PASS:PL_PASS + 4] != img[PL_PASS:PL_PASS + 4]):
                ln.setdefault("p", {})[side] = player_row(img)
        if L == self.t0 + 1:
            self.after_apply(units, players, w_extra)
        if self.delayed and orders is not None:
            self.orders_seen(L, orders)

    def events(self, L, evs):
        """Iteration L's events: what step L did, and what the frames held after it made."""
        if L < self.t0 or L > self.t_end:
            return
        t = L - self.t0
        ln = self.line(t)
        out = []
        clear_deaths = {}
        if L == self.t_end:
            for i, (kind, b) in enumerate(evs):
                if kind == "death":
                    slot = struct.unpack_from("<I", b)[0] - 1
                    if slot in self.cleared:
                        clear_deaths.setdefault(slot, []).append(i)
            for slot in self.cleared:
                n = len(clear_deaths.get(slot, ()))
                if n != 1:
                    self.fail("R1", f"cleared slot {slot} has {n} deaths in iteration {L}")
        last_clear = {v[-1] for v in clear_deaths.values()}
        creates = []
        for i, (kind, b) in enumerate(evs):
            if kind == "create":
                uid, typ, owner = struct.unpack_from("<IHB", b)
                creates.append((uid - 1, typ, owner, b))
                if L == self.t0:
                    continue            # the apply's: R1 names them by the result
                if uid - 1 in self.watched() and \
                        L <= self.t0 + max(a for _h, a, _t in self.delayed):
                    self.fail("R8", f"a create in watched slot {uid - 1} at GameTime {L}")
                self.made += 1
                h = f"built:{self.made}"
                self.slot_handle[uid - 1] = h
                side = self.side_of_seat.get(owner)
                x, y, z = struct.unpack_from("<iii", b, 12)
                out.append(["create", h, self.ids.get(typ, str(typ)), side,
                            struct.unpack_from("<I", b, 8)[0], x, y, z])
            elif kind == "death":
                uid, killer = struct.unpack_from("<II", b)
                slot = uid - 1
                h = self.handle(slot)
                if h is None:
                    continue
                if i in last_clear:
                    # the clear: the survivor's state at the end of step t_end, not a death
                    self.line(t).setdefault("u", {})[h] = unit_row(b[DEATH_HEAD:])
                    continue
                out.append(["death", h, self.handle(killer - 1) if killer else None])
                self.slot_handle.pop(slot, None)
            elif kind == "shot":
                tgt, shooter = struct.unpack_from("<HH", b, 0x1F)
                weapon = b[0x19] | (b[0x23] >> 4) << 8
                h = self.handle(shooter - 1)
                if h is not None:
                    out.append(["shot", h, weapon,
                                self.handle(tgt - 1) if tgt != 0xFFFF else None])
            elif kind == "built":
                builder, built = struct.unpack_from("<II", b)
                if self.handle(built - 1) is not None:
                    out.append(["built", self.handle(builder - 1), self.handle(built - 1)])
        if L == self.t0:
            self.apply_creates(creates, ln)
        if out:
            ln.setdefault("ev", []).extend(out)

    def slot_of(self, h):
        return self.row["apply"]["units"][h][0] if h in self.row["apply"]["units"] else None

    def watched(self):
        """The slots the delayed orders' watch keeps: each delayed order's subject and target."""
        return {self.slot_of(x) for h, _a, tgt in self.delayed for x in (h, tgt)
                if x is not None and self.slot_of(x) is not None}

    # ---- the checks ---------------------------------------------------------------------------

    def apply_creates(self, creates, ln):
        """R1: the apply's creates are exactly the wire's units: ID, type and owner."""
        want = {}
        for h, r in self.row["apply"]["units"].items():
            u = self.units[h]
            want[r[0]] = (h, self.values[u["type"]]["id"], self.seat[u["side"]])
        got = {slot: (typ, owner) for slot, typ, owner, _b in creates}
        body = {slot: b for slot, _t, _o, b in creates}
        if set(got) != set(want):
            self.fail("R1", f"the apply made slots {sorted(got)}, the result names {sorted(want)}")
            return
        ev = []
        for slot, (h, typ, owner) in sorted(want.items(), key=lambda kv: kv[1][0]):
            if got[slot] != (typ, owner):
                self.fail("R1", f"{h}: created as type {got[slot][0]} owner {got[slot][1]}, "
                                f"expected {typ} owner {owner}")
                return
            self.slot_handle[slot] = h
            b = body[slot]
            ev.append(["create", h, self.units[h]["type"], self.units[h]["side"],
                       struct.unpack_from("<I", b, 8)[0], *struct.unpack_from("<iii", b, 12)])
        ln.setdefault("ev", []).extend(ev)

    def after_apply(self, units, players, w_extra):
        """R1's positions, R3 and R4's wire values, on the images that first show the apply."""
        for h, r in self.row["apply"]["units"].items():
            img = units.get(r[0])
            if img is None:
                self.fail("R1", f"{h} (slot {r[0]}) has no image after the apply")
                return
            x, depth = struct.unpack_from("<H", img, 0x6C)[0], struct.unpack_from("<H", img, 0x74)[0]
            if (x, depth) != (r[1], r[2]):
                self.fail("R1", f"{h} at ({x}, {depth}), the result says ({r[1]}, {r[2]})")
                return
        self.ok("R1")
        feats = w_extra.get("feature_defs", [])
        want = sorted(self.row["apply"].get("features", []))
        if sorted(set(feats)) != sorted(set(want)):
            self.fail("R3", f"feature defs on the grid {sorted(set(feats))[:8]}, the scenario's "
                            f"{want[:8]}")
        else:
            self.ok("R3")
        setup = self.exp["setup"]["players"]
        for p in setup:
            img = players[p["slot"]]
            st = p["storage"]
            got = (f32(img, PL_BASE_M), f32(img, PL_BASE_E), img[PL_BASEFLAG] & 1,
                   f32(img, PL_M), f32(img, PL_E))
            # the engine stores each as an f32, so the wire's number is compared as one
            exp = (as_f32(st["metal"]), as_f32(st["energy"]), 1, as_f32(p["metal"]),
                   as_f32(p["energy"]))
            if got != exp:
                self.fail("R4", f"seat {p['slot']}: storage, flag and levels {got}, the wire's {exp}")
                return
        self.players_after = [bytes(players[p["slot"]]) for p in setup]

    def orders_seen(self, L, orders):
        """R8: a delayed order's subject's main list changes in iteration T + after + 1."""
        for h, a, _t in self.delayed:
            if L == self.t0 + a + 1:
                slot = self.slot_of(h)
                self.order_hits = getattr(self, "order_hits", 0) + ((slot, 0) in orders)

    def finish_delayed(self):
        if not self.delayed:
            return
        rep = self.row.get("delayed") or {}
        missing = len(self.delayed) - getattr(self, "order_hits", 0)
        if missing > rep.get("cancelled", 0):
            self.fail("R8", f"{missing} delayed order(s) changed no list at T + after + 1, "
                            f"{rep.get('cancelled', 0)} cancelled")
        else:
            self.ok("R8")


def phased_diff(a_imgs, b_imgs, ta, tb):
    """R4: the player images after two applies differ only where a reload may leave them: the
    lifetime totals, and the GameTime deadlines, which must agree modulo 30 with each apply tick.
    Returns every difference, named by its aligned dword."""
    out = []
    skip = set()
    for lo, hi in PL_LIFETIME + [PL_RANK]:
        skip.update(range(lo, hi))
    for at in PL_PHASED:
        skip.update(range(at, at + 4))
    for seat, (a, b) in enumerate(zip(a_imgs, b_imgs)):
        for at in PL_PHASED:
            va, vb = struct.unpack_from("<i", a, at)[0], struct.unpack_from("<i", b, at)[0]
            if (va - ta) % 30 != (vb - tb) % 30:
                out.append(f"seat image {seat} +0x{at:X}: phase {(va - ta) % 30} against "
                           f"{(vb - tb) % 30}")
        seen = set()
        for k in range(len(a)):
            if k not in skip and a[k] != b[k] and k & ~3 not in seen:
                seen.add(k & ~3)
                out.append(f"seat image {seat} differs at +0x{k & ~3:X}")
    return out


def feature_defs(w):
    """The feature defs on the grid: every cell's def word below 0xFFFB, and 0xFFFE (R3)."""
    import tarec
    out = []
    for k in range(0, len(w.grid), tarec.REC_CELL_BYTES):
        d = struct.unpack_from("<H", w.grid, k + 8)[0]
        if d < 0xFFFB or d == 0xFFFE:
            out.append(d)
    return out


def exogenous_of(g, names):
    """The run's exogenous inputs in a globals image: the simulation RNG's state and the wind's rows."""
    return {names.get(i, str(i)): struct.unpack_from("<I", g, 4 * i)[0] for i in range(len(g) // 4)
            if any(p in names.get(i, "") for p in ("rngseed", "wind"))}


# ---- THE PACKED LANE -------------------------------------------------------------------------------------
# A batch row (engrun.py, Lane.run_batch) is read back into one CellRun a cell: the trace an unpacked run of its
# job writes, so metrics.combat and the references take it as they take a run. The batch's own checks are P1..P5
# (tasim-calibration.md, *The packed lane*), and each fails THE GENERATION, never the batch alone: a check that
# fires on something a fight did (a chase out of its box, a burst of searches) is never grounds to drop that batch
# and play it again, since keeping only the batches whose outcomes did not fire it would bias the runs kept. The
# fix for a failure is a tighter bound in pack.py.

PL_LIVE = 0x144                 # u16 a seat's live units: incremented by the create, decremented in the destructor
# a projectile record's image (the record's own offsets, tagpu_recfmt.h REC_PROJ_IMAGE): the position x and z,
# 16.16 (the init 0x49C740 copies the start there; the interceptor reads a quarry's at +0x04, 0x49B47A),
# the attacker as a unit ID (replay-layout.tsv `id:unit`: slot + 1, 0 for none), and the burst's u16 count of
# copies still to make, nonzero on a parent (`mov [esi+0x60],ax` at 0x49CB79; DISASSEMBLED)
PROJ_X, PROJ_Z, PROJ_ATTACKER, PROJ_BURST = 0x04, 0x0C, 0x52, 0x60
PF_PERIOD = 150                 # pfcalls wraps at 150 (0x40EBA5..0x40EC0C)
# P4c's window: a temporary sight record made at GameTime d lives through d + 60 and is gone from the first frame
# at d + 61 on (engrun.py, THE EMPTY POINT'S SIGHT), so a death at d meets the records of the deaths at d - 60..d;
# the window takes d - 61 too, so a reading one tick off errs toward a failure
SIGHT_SPAN = 61
# every check that fails the generation (calib.build_refs reads this list alone). P5 is a cell's own verdict
# (CellRun.finish): the batch records it ok, and verdicts keeps a cell's failure over it
P_CHECKS = ("P1", "P2", "P3", "P4a", "P4b", "P4c", "P4d", "P4e", "P5")


class CellRun(Run):
    """One cell of a packed batch as the run it stands for: fed its own units' images and events by its Batch, it
    writes the trace an unpacked run of its job writes -- the run's handles, positions translated back by the cell's
    offset, t counted from its create at t0 = the apply + at -- with no seat (`p`) rows, which every cell of a batch
    shares (pack.CELL_METRICS). Its end is read here, offline: the first iteration after its create in which one of
    its seats is down to that seat's end_units in the cell, else its limit (P5 holds the online clause to it). Its
    row is a run's, with `pack` naming the batch and its place in it."""

    def __init__(self, brow: dict, crow: dict, exp: dict, block: dict, values: dict, ids: dict, first: dict):
        cell = pack.cell_from_json(crow["cell"])
        self.cell, self.k = cell, cell.k
        self.t0 = brow["t_apply"] + cell.at
        self.lim = self.t0 + block["limit"]
        self.fx, self.fz = cell.offset[0] << 16, cell.offset[1] << 16
        self.exp, self.block, self.values, self.ids = exp, block, values, ids
        self.swap = crow["swap"]
        self.lines, self.last_u = {}, {}
        self.checks, self.fields, self.failed = {}, {}, None
        self.exogenous = None
        self.units, self.expect, self.slot_of = {}, {}, {}
        rank = {s: 0 for s in pack.SEATS}
        for u in exp["units"]:
            seat = u["owner"]
            slot = first[seat] + cell.pos[seat] + rank[seat]
            rank[seat] += 1
            dest = next((o["to"] for o in u["orders"] if o["cmd"] == "move" and o.get("to")), None)
            self.units[u["id"]] = {"type": u["type"].upper(), "side": "A" if (seat == 0) != self.swap else "B",
                                   "seat": seat, "dest": dest}
            self.expect[slot] = (u["id"], values[u["type"].upper()]["id"], seat,
                                 (u["pos"][0] + cell.offset[0], u["pos"][1] + cell.offset[1]))
            self.slot_of[u["id"]] = slot
        self.slot_handle = {}
        self.creates = []
        self.live = {s: 0 for s in pack.SEATS}
        self.ends = perturb.end_clauses(block, self.swap)
        self.t_end = self.end = None
        self.delayed = [(u["id"], o["after"]) for u in exp["units"] for o in u["orders"] if o["after"]]
        self.order_hits = 0
        self.box16 =tuple(v << 16 for v in (cell.box[0] - pack.E, cell.box[1] - pack.E,
                                             cell.box[2] + pack.E, cell.box[3] + pack.E))
        self.zone16 = tuple(v << 16 for v in cell.zone)
        self.bound = crow["proj_bound"]
        self.row = {"scenario": crow["scenario"], "scenario_sha256": crow["scenario_sha256"],
                    "index": crow["index"], "seed": crow["seed"], "swap": crow["swap"], "mode": "packed",
                    "perturbation": crow["perturbation"], "wire_sha256": crow["wire_sha256"],
                    "t_apply": self.t0, "t_end": None, "end": None,
                    "pack": {"K": brow["K"], "batch": brow["batch"], "k": cell.k, "at": cell.at,
                             "offset": list(cell.offset), "site": cell.site, "t_batch": brow["t_apply"],
                             "t_pause": brow["t_end"], "decided": crow.get("decided")},
                    "wall": {"total": round((brow.get("wall") or {}).get("total", 0.0) / len(brow["cells"]), 3)}}

    def unit_row(self, img):
        r = unit_row(img)
        r[0] -= self.fx
        r[2] -= self.fz
        return r

    def images(self, L, units):
        """Iteration L's images of the cell's units: the state at the end of step L - 1, from the one after its
        create to the one after its limit."""
        if not self.t0 + 1 <= L <= self.lim + 1:
            return
        t = L - 1 - self.t0
        for slot, h in self.slot_handle.items():
            img = units.get(slot)
            if img is None:
                continue
            r = self.unit_row(img)
            if self.last_u.get(h) != r:
                self.line(t).setdefault("u", {})[h] = r
                self.last_u[h] = r
        if L == self.t0 + 1:
            for slot, (h, _typ, _seat, pos) in sorted(self.expect.items()):
                img = units.get(slot)
                if img is None:
                    self.fail("R1", f"{h} (slot {slot}) has no image after its create")
                    return
                got = (struct.unpack_from("<H", img, 0x6C)[0], struct.unpack_from("<H", img, 0x74)[0])
                if got != pos:
                    self.fail("R1", f"{h} at {got}, its wire's {pos}")
                    return
            self.ok("R1")

    def create(self, L, slot, typ, owner, b):
        """R1: one of the cell's creates, in iteration t0 exactly, the type and owner the wire asks in its slot."""
        h, want_typ, seat, _pos = self.expect[slot]
        if L != self.t0:
            self.fail("R1", f"{h} (slot {slot}) created in iteration {L}, the cell's creates are in {self.t0}")
            return
        if (typ, owner) != (want_typ, seat) or slot in self.slot_handle:
            self.fail("R1", f"{h} (slot {slot}): created as type {typ} owner {owner}, expected {want_typ} "
                            f"owner {seat}, once")
            return
        self.slot_handle[slot] = h
        self.live[seat] += 1
        x, y, z = struct.unpack_from("<iii", b, 12)
        self.creates.append(["create", h, self.units[h]["type"], self.units[h]["side"],
                             struct.unpack_from("<I", b, 8)[0], x - self.fx, y, z - self.fz])

    def creates_done(self):
        """The end of iteration t0: every slot the wire asks made, and the creates at t = 0 in handle order ahead of
        the iteration's other events, as an unpacked run's apply writes them (Run.apply_creates)."""
        if set(self.slot_handle) != set(self.expect):
            self.fail("R1", f"the creates made slots {sorted(self.slot_handle)}, the cell's are {sorted(self.expect)}")
        ln = self.line(0)
        ln["ev"] = sorted(self.creates, key=lambda e: e[1]) + ln.get("ev", [])

    def death(self, L, slot, killer, b, clear):
        """A death of one of the cell's units; `clear`, the batch's clear: the survivor's state at the end of step
        L, not a death. The first iteration after the create that leaves one of the cell's seats at its end_units
        is the cell's end."""
        h = self.slot_handle.get(slot)
        if h is None:
            return
        if clear:
            if L <= self.lim:
                self.line(L - self.t0).setdefault("u", {})[h] = self.unit_row(b[DEATH_HEAD:])
            return
        if L <= self.lim:
            self.line(L - self.t0).setdefault("ev", []).append(
                ["death", h, self.slot_handle.get(killer) if killer is not None else None])
        self.slot_handle.pop(slot)
        self.live[self.expect[slot][2]] -= 1
        if self.t_end is None and self.t0 < L <= self.lim and \
                any(self.live[s] <= n for s, n in self.ends):
            self.t_end, self.end = L, "units"

    def shot(self, L, shooter, b):
        tgt = struct.unpack_from("<H", b, 0x1F)[0]
        h = self.slot_handle.get(shooter)
        if h is not None and L <= self.lim:
            weapon = b[0x19] | (b[0x23] >> 4) << 8
            self.line(L - self.t0).setdefault("ev", []).append(
                ["shot", h, weapon, self.slot_handle.get(tgt - 1) if tgt != 0xFFFF else None])

    def built(self, L, builder, built):
        if self.slot_handle.get(built) is not None and L <= self.lim:
            self.line(L - self.t0).setdefault("ev", []).append(
                ["built", self.slot_handle.get(builder), self.slot_handle[built]])

    def orders_seen(self, L, orders):
        """R8: a delayed order's subject's main list changes in iteration t0 + after + 1."""
        for h, a in self.delayed:
            if L == self.t0 + a + 1:
                self.order_hits += (self.slot_of[h], 0) in orders

    def finish(self, t_pause):
        """The cell's end (P5: at or before the batch's pause), its trace cut there, and every survivor's state at
        the end of that step at t_end - t0, as an unpacked run's clear writes it."""
        if self.t_end is None:
            if self.lim <= t_pause:
                self.t_end, self.end = self.lim, "limit"
            else:
                self.fail("P5", f"the batch paused at GameTime {t_pause}, before the cell's end: no seat at its "
                                f"end_units, and its limit at {self.lim}")
                self.t_end, self.end = t_pause, "limit"
        cut = self.t_end - self.t0
        self.lines = {t: ln for t, ln in self.lines.items() if t <= cut}
        rows, alive = {}, set()
        for t in sorted(self.lines):
            for ev in self.lines[t].get("ev", ()):
                if ev[0] == "create":
                    alive.add(ev[1])
                elif ev[0] == "death":
                    alive.discard(ev[1])
            rows.update(self.lines[t].get("u", {}))
        end = self.line(cut)
        for h in sorted(alive):
            if h in rows:
                end.setdefault("u", {})[h] = rows[h]
        self.row["t_end"], self.row["end"] = self.t_end, self.end


class Batch(Checked):
    """One packed batch's window of the stream (THE PACKED LANE): its cells, each fed its own units' images and
    events, and the batch's checks -- R1..R6 as a run's on the batch's apply and clear, R8 over its cells, and P1
    to P5 -- which every cell carries into its own checks (verdicts)."""

    def __init__(self, row: dict, head: dict, arena_cfg: dict, values: dict, ids: dict):
        pk = head.get("pack")
        if not pk:
            raise calibref.CalibError(f"batch {row['batch']}: its lane-launch's head has no packed reads")
        self.row, self.i = row, row["batch"]
        self.t0, self.t_end = row["t_apply"], row["t_end"]
        self.first = dict(zip(pack.SEATS, pk["first"]))
        self.cap, self.sight_cap, self.S = pk["proj_cap"], pk["sight_cap"], pk["S"]
        self.checks, self.fields, self.failed = {}, {}, None
        self.empty = self.players_after = None
        cells = [pack.cell_from_json(c["cell"]) for c in row["cells"]]
        if hashlib.sha256(pack.wire(cells, arena_cfg, values).encode()).hexdigest() != row["wire_sha256"]:
            raise calibref.CalibError(f"batch {self.i}: pack.py no longer makes the wire it applied")
        self.cells, self.slot_cell = [], {}
        for crow in row["cells"]:
            path = TREE / crow["scenario"]
            if calibref.sha256_file(path) != crow["scenario_sha256"]:
                raise calibref.CalibError(f"{crow['scenario']}: the file changed since its batch")
            exp, record, wire, _cl = perturb.run_wire(path, crow["index"], values, arena_cfg)
            if hashlib.sha256(wire.encode()).hexdigest() != crow["wire_sha256"] or record != crow["perturbation"]:
                raise calibref.CalibError(f"{crow['scenario']} run {crow['index']}: perturb.py no longer makes "
                                          f"the run its batch applied")
            block = calibref.calib_block(json.loads(path.read_text()), path.name, arena_cfg)
            c = CellRun(row, crow, exp, block, values, ids, self.first)
            self.cells.append(c)
            for slot in c.expect:
                self.slot_cell[slot] = c
        # P4a's appends a step: a live unit's armed slots and its armed slots with a burst (a burst fired this step
        # can add a copy in the same step, PC4)
        self.arms = {}
        for slot, c in self.slot_cell.items():
            t = row["types"][c.units[c.expect[slot][0]]["type"]]
            self.arms[slot] = t["slots"] + t["bursts"]
        self.storage = {p["slot"]: (p["storage"]["metal"], p["storage"]["energy"])
                        for p in self.cells[0].exp["setup"]["players"]}
        self.cell0 = {s: sum(1 for e in self.cells[0].expect.values() if e[2] == s) for s in pack.SEATS}
        self.cleared = set(row["clear"]["cleared_slots"])
        self.tally = {}
        self.prev_pf = None
        self.headroom = None
        self.viewed = []
        self.lut = None

    def note(self, name, L, what):
        """One iteration's failure of a P check: counted, its first kept."""
        n, first = self.tally.get(name, (0, None))
        self.tally[name] = (n + 1, first or f"iteration {L}: {what}")

    def feed(self, L, w, players, evs, orders, names, pf):
        """Iteration L of the stream: the empty point at t0, the apply's state at t0 + 1, the cells' images, the
        P checks on every iteration of the batch, then the iteration's events and orders."""
        if L == self.t0:
            self.empty_point(dict(w.units), (w.phead or (0, 0))[0], bytes(w.globals or b""),
                             {ix: bytes(v) for ix, v in w.tables.items()}, bytes(w.mapped))
        if L == self.t0 + 1:
            self.apply_point((w.phead or (0, 0))[0])
            self.after_apply(w, players)
        for c in self.cells:
            c.images(L, w.units)
            if L == c.t0 + 1:
                c.exogenous = exogenous_of(bytes(w.globals or b""), names)
        if self.t0 < L <= self.t_end:
            self.tick(L, w, players, pf)
        self.events(L, evs)
        if orders:
            for c in self.cells:
                c.orders_seen(L, orders)

    def after_apply(self, w, players):
        """R3 and R4 on the images that first show the apply: no feature, each seat's storage, flag and levels the
        wire's, and its live count +0x144 cell 0's units of it -- the apply makes cell 0 alone."""
        feats = feature_defs(w)
        want = sorted(self.row["apply"].get("features", []))
        if sorted(set(feats)) != sorted(set(want)):
            self.fail("R3", f"feature defs on the grid {sorted(set(feats))[:8]}, the batch's {want[:8]}")
        else:
            self.ok("R3")
        setup = self.cells[0].exp["setup"]["players"]
        for p in setup:
            img = players[p["slot"]]
            st = p["storage"]
            got = (f32(img, PL_BASE_M), f32(img, PL_BASE_E), img[PL_BASEFLAG] & 1, f32(img, PL_M), f32(img, PL_E))
            exp = (as_f32(st["metal"]), as_f32(st["energy"]), 1, as_f32(p["metal"]), as_f32(p["energy"]))
            if got != exp:
                self.fail("R4", f"seat {p['slot']}: storage, flag and levels {got}, the wire's {exp}")
                return
            live = struct.unpack_from("<H", img, PL_LIVE)[0]
            if p["slot"] in self.cell0 and live != self.cell0[p["slot"]]:
                self.fail("R4", f"seat {p['slot']}: {live} live units after the apply, cell 0 makes "
                                f"{self.cell0[p['slot']]}")
                return
        self.players_after = [bytes(players[p["slot"]]) for p in setup]

    def tick(self, L, w, players, pf):
        """P1 to P4 on iteration L's images (the state at the end of step L - 1)."""
        import numpy as np
        import tarec
        for slot, img in w.units.items():
            c = self.slot_cell.get(slot)
            if c is None:
                self.note("P1", L, f"a live unit in slot {slot}, of no cell")
                continue
            x, z = struct.unpack_from("<i", img, U_POS)[0], struct.unpack_from("<i", img, U_POS + 8)[0]
            b = c.box16
            if not (b[0] <= x <= b[2] and b[1] <= z <= b[3]):
                self.note("P1", L, f"cell {c.k}'s {c.expect[slot][0]} (slot {slot}) at ({x >> 16}, {z >> 16}), "
                                   f"outside its box ⊕ {pack.E} {tuple(v >> 16 for v in b)}")
        count = (w.phead or (0, 0))[0]
        parents = 0
        if count:
            if self.lut is None:
                self.lut = np.full(max(self.slot_cell) + 2, -1, dtype=np.int64)
                for slot, c in self.slot_cell.items():
                    self.lut[slot + 1] = c.k
            recs = np.frombuffer(bytes(w.proj[:count * tarec.REC_PROJ_IMAGE]), dtype=np.uint8).reshape(
                count, tarec.REC_PROJ_IMAGE)
            att = recs[:, PROJ_ATTACKER:PROJ_ATTACKER + 4].copy().view("<u4").ravel()
            px = recs[:, PROJ_X:PROJ_X + 4].copy().view("<i4").ravel().astype(np.int64)
            pz = recs[:, PROJ_Z:PROJ_Z + 4].copy().view("<i4").ravel().astype(np.int64)
            parents = int((recs[:, PROJ_BURST:PROJ_BURST + 2].copy().view("<u2").ravel() != 0).sum())
            if (att == 0).any():
                self.note("P2", L, f"record {int(np.argmax(att == 0))} has no attacker (+0x52 is 0)")
            known = att < len(self.lut)
            cell = np.where(known, self.lut[np.where(known, att, 0)], -1)
            stray = (cell < 0) & (att != 0)
            if stray.any():
                r = int(np.argmax(stray))
                self.note("P2", L, f"record {r}'s attacker, slot {int(att[r]) - 1}, is of no cell")
            for c in self.cells:
                mine = cell == c.k
                n = int(mine.sum())
                if n > c.bound:
                    self.note("P4e", L, f"cell {c.k} holds {n} records, its bound {c.bound}")
                z0x, z0z, z1x, z1z = c.zone16
                out = mine & ((px < z0x) | (px > z1x) | (pz < z0z) | (pz > z1z))
                if out.any():
                    r = int(np.argmax(out))
                    self.note("P2", L, f"cell {c.k}'s record {r} at ({int(px[r]) >> 16}, {int(pz[r]) >> 16}), "
                                       f"outside its zone {tuple(v >> 16 for v in c.zone16)}")
        arms = sum(self.arms.get(slot, 2 * len(pack.WEAPON_SLOTS)) for slot in w.units)
        if count + arms + parents > self.cap:
            self.note("P4a", L, f"{count} records, {arms} armed and burst slots and {parents} burst parents, "
                                f"past the cap {self.cap}")
        for s, (metal, energy) in self.storage.items():
            e, m = f32(players[s], PL_E), f32(players[s], PL_M)
            if e < pack.STORAGE_FRAC * energy or m < pack.STORAGE_FRAC * metal:
                self.note("P4b", L, f"seat {s} holds energy {e:.0f} and metal {m:.0f}, below "
                                    f"{pack.STORAGE_FRAC:.0%} of {energy:.0f} and {metal:.0f}")
        if w.heads is None or w.heads[2] == -1:
            self.note("P4d", L, "the wreck pool's free list is empty (freehead -1)"
                      if w.heads else "no RK_FEATURES heads")
        g = w.globals
        if g is None or pf is None:
            self.note("P3", L, "the globals image holds no pathfinder counters")
            return
        calls = struct.unpack_from("<I", g, 4 * pf["calls"])[0]
        exam = {s: struct.unpack_from("<I", g, 4 * pf[s])[0] for s in pack.SEATS}
        if self.prev_pf is not None:
            pc, pe = self.prev_pf
            if calls != (pc + 1) % PF_PERIOD:
                self.note("P3", L, f"pfcalls went from {pc} to {calls}")
            for s in pack.SEATS:
                e = exam[s] if calls == 0 else exam[s] - pe[s]
                room = e - self.S - 1
                self.headroom = room if self.headroom is None else min(self.headroom, room)
                if room < 0:
                    self.note("P3", L, f"seat {s} examined {e} slots in its step, under S + 1 = {self.S + 1}")
        self.prev_pf = (calls, exam)

    def events(self, L, evs):
        """Iteration L's events, each to its cell by its unit's slot; at the batch's end the clear's deaths, one a
        cleared slot (R1); a create of a slot of no cell fails R1."""
        if not self.t0 <= L <= self.t_end:
            return
        last_clear = set()
        if L == self.t_end:
            per = {}
            for i, (kind, b) in enumerate(evs):
                if kind == "death" and struct.unpack_from("<I", b)[0] - 1 in self.cleared:
                    per.setdefault(struct.unpack_from("<I", b)[0] - 1, []).append(i)
            for slot in self.cleared:
                n = len(per.get(slot, ()))
                if n != 1:
                    self.fail("R1", f"cleared slot {slot} has {n} deaths in iteration {L}")
            last_clear = {v[-1] for v in per.values()}
        for i, (kind, b) in enumerate(evs):
            if kind == "create":
                uid, typ, owner = struct.unpack_from("<IHB", b)
                c = self.slot_cell.get(uid - 1)
                if c is None:
                    self.fail("R1", f"a create in slot {uid - 1} in iteration {L}, in no cell")
                    continue
                c.create(L, uid - 1, typ, owner, b)
            elif kind == "death":
                uid, killer = struct.unpack_from("<II", b)
                c = self.slot_cell.get(uid - 1)
                if c is None:
                    continue
                clear = i in last_clear
                if not clear and c.expect[uid - 1][2] == pack.VIEWED and uid - 1 in c.slot_handle:
                    self.viewed.append(L)
                c.death(L, uid - 1, killer - 1 if killer else None, b, clear)
            elif kind == "shot":
                shooter = struct.unpack_from("<H", b, 0x21)[0]
                c = self.slot_cell.get(shooter - 1)
                if c is not None:
                    c.shot(L, shooter - 1, b)
            elif kind == "built":
                builder, built = struct.unpack_from("<II", b)
                c = self.slot_cell.get(built - 1)
                if c is not None:
                    c.built(L, builder - 1, built - 1)
        for c in self.cells:
            if L == c.t0:
                c.creates_done()

    def finish(self):
        """After the stream: P4c over the viewed seat's deaths, every cell's end (P5), R8 over the batch's delayed
        orders, and each P check's verdict."""
        ds = sorted(self.viewed)
        lo = 0
        for i, d in enumerate(ds):
            while ds[lo] < d - SIGHT_SPAN:
                lo += 1
            if i - lo >= self.sight_cap:
                self.note("P4c", d, f"a death of the viewed seat meets {i - lo} earlier ones within "
                                    f"{SIGHT_SPAN} ticks: the sight records' cap is {self.sight_cap}")
                break
        for c in self.cells:
            c.finish(self.t_end)
        rep = self.row.get("delayed") or {}
        missing = {c: len(c.delayed) - c.order_hits for c in self.cells if c.delayed}
        over = sum(missing.values()) > rep.get("cancelled", 0)
        for c, n in missing.items():
            if over and n:
                c.fail("R8", f"{n} delayed order(s) changed no list at t0 + after + 1, "
                             f"{rep.get('cancelled', 0)} cancelled in the batch")
            else:
                c.ok("R8")
        for name in P_CHECKS:
            if name in self.tally:
                n, first = self.tally[name]
                self.fail(name, f"{first} ({n} iteration(s))")
            else:
                self.ok(name)

    def verdicts(self):
        """The batch's checks into every cell's: a failure named with the batch, a verdict as it is."""
        for c in self.cells:
            for name, why in self.checks.items():
                if name in self.fields:
                    c.fail(name, f"batch {self.i}: {why}", self.fields[name])
                else:
                    c.checks.setdefault(name, why)
            c.row["pack"]["p3_headroom"] = self.headroom


class Lane:
    """One lane-launch: its manifest, its catalogue, its recording streamed once."""

    def __init__(self, lane_dir: Path, arena_cfg: dict):
        self.dir = Path(lane_dir)
        rows = [json.loads(ln) for ln in (self.dir / "manifest.jsonl").read_text().splitlines()
                if ln.strip()]
        self.head = rows[0]
        self.runs = [r for r in rows[1:] if "index" in r]
        self.dumps = {r["dump"]: r["t"] for r in rows[1:] if "dump" in r and "index" not in r}
        cat = json.loads((self.dir / "units.json").read_text())
        self.catalogue = cat
        self.values = {u["name"].upper(): {"metal": u["cost"][0], "energy": u["cost"][1],
                                           "maxdamage": u["maxdamage"], "buildtime": u["buildtime"],
                                           "workertime": u["workertime"], "bmcode": u["bmcode"],
                                           "footprint": u["footprint"], "id": u["id"],
                                           "key": u["key"], "crc_all": u["crc_all"]}
                       for u in cat["units"]}
        self.ids = {u["id"]: u["name"].upper() for u in cat["units"]}
        self.arena = arena_cfg
        self.objs = []
        for row in self.runs:
            path = TREE / row["scenario"]
            if calibref.sha256_file(path) != row["scenario_sha256"]:
                raise calibref.CalibError(f"{row['scenario']}: the file changed since its runs")
            exp, record, wire, _cl = perturb.run_wire(path, row["index"], self.values, arena_cfg)
            if hashlib.sha256(wire.encode()).hexdigest() != row["wire_sha256"] or \
                    record != row["perturbation"]:
                raise calibref.CalibError(f"{row['scenario']} run {row['index']}: perturb.py no "
                                          f"longer makes the wire its run applied")
            block = calibref.calib_block(json.loads(path.read_text()), path.name, arena_cfg)
            self.objs.append(Run(row, exp, block, self.values, self.ids))
        self.batches = [Batch(r, self.head, arena_cfg, self.values, self.ids) for r in rows[1:] if "cells" in r]

    def stream(self):
        """Every run and every batch's cell, traced and checked: [Run or CellRun]."""
        import tarec
        h, bl, _stop = tarec.load(self.dir / self.head["recording"], exact=True)
        _gen, rows, slots, _digest = tarec.globals_list(h["globals"], h.get("globals_raw"))
        advancing_rows(rows)
        names = tarec.globals_names(slots)
        table_names = {ix: t["name"] for ix, t in h["tables"].items()}
        st = {"evs": [], "prev_players": None, "orders": set()}
        kinds = {tarec.RK_EV_CREATE: "create", tarec.RK_EV_DEATH: "death",
                 tarec.RK_EV_SHOT: "shot", tarec.RK_EV_BUILT: "built"}
        runs = sorted(self.objs, key=lambda r: r.t0)
        batches = sorted(self.batches, key=lambda b: b.t0)
        # P3's counters by their rows' names (replay-globals.tsv: pfcalls, and pfexamined's dword a player)
        index = {n: i for i, n in names.items()}
        pf = {"calls": index["pfcalls"], 0: index["pfexamined"], 1: index["pfexamined+4"]} \
            if {"pfcalls", "pfexamined", "pfexamined+4"} <= index.keys() else None

        def done(w, L):
            players = [bytes(p) for p in w.players]
            for r in runs:
                if r.t0 - 1 <= L <= r.t_end + 1:
                    extra = {"feature_defs": feature_defs(w)} if L == r.t0 + 1 else {}
                    r.images(L, w.units, players, st["prev_players"], st["orders"], extra)
                    if L == r.t0:
                        r.empty_point(dict(w.units), (w.phead or (0, 0))[0],
                                      bytes(w.globals or b""),
                                      {ix: bytes(v) for ix, v in w.tables.items()},
                                      bytes(w.mapped))
                        r.exogenous = None
                    if L == r.t0 + 1:
                        r.apply_point((w.phead or (0, 0))[0])
                        r.exogenous = exogenous_of(bytes(w.globals or b""), names)
                    r.events(L, st["evs"])
            for b in batches:
                if b.t0 <= L <= b.t_end + 1:
                    b.feed(L, w, players, st["evs"], st["orders"], names, pf)
            st["prev_players"] = players
            st["evs"] = []
            st["orders"] = set()

        def on_record(w, kind, b):
            if kind is None:
                if w.tick is not None:
                    done(w, w.tick)
            elif kind == tarec.RB_BOUNDARY:
                raise calibref.CalibError("a lane's recording holds a fork: it is never forked")
            elif kind in kinds:
                st["evs"].append((kinds[kind], bytes(b)))
            elif kind == tarec.RK_ORDERS:
                st["orders"].add(struct.unpack_from("<HB", b)[:2])
        w = tarec.stream_to(h, bl, None, on_record=on_record)
        if w.tick is not None:
            done(w, w.tick)
        base = {}
        first_of = {}
        for r in runs:
            r.finish_delayed()
            if r.empty is None:
                r.fail("R1", "the recording ends before the run's apply")
                continue
            b0 = base.setdefault("launch", r)
            needs = any(o["cmd"] in MODE12 for u in r.exp["units"] for o in u["orders"])
            # THE SAME SCENARIO AND SWAP. A run's slots (R1) and its seats' images after the apply
            # (R4) are compared with the launch's first run of its scenario and swap: each player's
            # units come from its own block of the unit array (the player record's +0x67..+0x6B,
            # set at 0x4858BD and 0x4858E0; engine/units.md), so swapping the owners moves every
            # unit to the other seat's block and its count +0x144 to the other seat's image, and
            # another scenario creates other units. The empty point (R5, R6) holds no unit, so it
            # is compared with run 0 of the launch.
            f = first_of.setdefault((r.row["scenario"], r.swap), r)
            r.compare_run0(b0, names, table_names, needs, f)
            if f is not r and [f.slot_of(h) for h in sorted(f.units)] != \
                    [r.slot_of(h) for h in sorted(r.units)]:
                r.fail("R1", "its slots differ from the first run of its scenario and swap in "
                             "this launch")
        # A batch's apply makes cell 0 alone, so its seats' images after the apply (R4) are compared with the
        # launch's first batch whose cell 0 is the same scenario and swap (THE SAME SCENARIO AND SWAP), and its
        # empty point (R5, R6) with the launch's first batch.
        first_of = {}
        base = next((b for b in batches if b.empty is not None), None)
        for b in batches:
            if b.empty is None:
                b.fail("R1", "the recording ends before the batch's apply")
            else:
                c0 = b.cells[0]
                f = first_of.setdefault((c0.row["scenario"], c0.swap), b)
                b.compare_run0(base, names, table_names, False, f)
            b.finish()
            b.verdicts()
        return runs + [c for b in batches for c in b.cells]

    def sight_check(self):
        """R7: every seat's sight counters zero in the launch's e0 and eN dumps."""
        import tarec
        rec = self.dir / self.head["recording"]
        for name in ("e0", "eN"):
            path = self.dir / f"dump-{name}.bin"
            if not path.exists():
                return f"no dump {name}"
            _dh, world = tarec.dump_world(path, tarec.header_only(rec))
            for rec_ix, counters in world.sight.items():
                if any(counters):
                    return f"dump {name}: seat {rec_ix}'s sight counters are not zero"
        return None


def lane(lane_dir, arena_cfg):
    """Every run of one lane-launch, traced and checked."""
    ln = Lane(lane_dir, arena_cfg)
    runs = ln.stream()
    r7 = ln.sight_check()
    for r in runs:
        if r7:
            r.fail("R7", r7)
        else:
            r.ok("R7")
        if r.row.get("dump"):
            r.sight = sight_cells(ln, r)
    return ln, runs


def sight_cells(ln: Lane, r: Run):
    """The sight family: the cells with a non-zero counter for the shooter side's seat in the
    run's dump at t = 2, as indices into the counters' grid."""
    import tarec
    path = ln.dir / f"dump-{r.row['dump']}.bin"
    _dh, world = tarec.dump_world(path, tarec.header_only(ln.dir / ln.head["recording"]))
    out = {}
    for side, seat in r.seat.items():
        c = world.sight.get(seat, b"")
        out[side] = [i for i, v in enumerate(c) if v]
    return out
