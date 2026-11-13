"""engtrace.py -- the engine side of a calibration run: its trace, and the checks that make the reload a fact.

    for res in engtrace.lane(lane_dir, arena_cfg):    # one lane-launch's recording, read once
        res.row, res.lines, res.checks, res.start_check, res.exogenous

The recording is streamed once (tarec.stream_to) and sliced by the manifest's windows; each run's
trace is `ta-calib-trace/1` (metrics.py, THE TRACE'S TIME), and each run is checked R1..R8
(research/notes/modules/tasim-calibration.md, *The per-run checks*). A failed check makes the run
unusable; nothing is tolerated silently.

THE ITERATION OF AN EVENT MADE ON A PAUSED FRAME. The recorder's capture point is the unit tick's
head (tagpu_recorder.c, tagpu_rec_capture): it publishes the last step's events (`ring_publish(r,
0)`) and then writes RK_TICK with the new GameTime and the images. An event is put in the ring when
the engine makes it, on the game thread (ev_rec, ev_put), whether a step is running or not. So the
apply's creates and the clear's deaths, made on the frames held after step T, come in iteration T,
after step T's own events, and the images of iteration T + 1 are the first to show them. Hence:
  - the empty point is iteration t_apply's images (before the apply);
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
    # moveclasses 0 and 5's passability watermarks, a GameTime (max(GameTime, 30) − 30) written
    # at each search start 0x40E630 through 0x440AF0
    "mcwatermark00", "mcwatermark05",
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
#   once for each GameTime value; random from launch to launch as the wind rows above are
#   (owner's decision of 2026-11-12)
EMPTY_POINT_ADVANCING_TABLES = re.compile(r"\bs_hit(Stamp|Freed|Local)?\b|\bs_windState\b")
MODE12 = ("reclaim", "capture")     # the explored map's only simulation reader (R5)


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


class Run:
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

    def fail(self, name, why, fields=None):
        self.checks[name] = why
        self.fields.setdefault(name, []).extend(fields or [why])
        if self.failed is None:
            self.failed = f"{name}: {why}"

    def ok(self, name):
        self.checks.setdefault(name, "ok")

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


def phased_diff(a_imgs, b_imgs, ta, tb):
    """R4: the player images after two applies differ only where a reload may leave them: the
    lifetime totals, and the GameTime deadlines, which must agree modulo 30 with each apply tick.
    Returns every difference, named by its aligned dword."""
    out = []
    skip = set()
    for lo, hi in PL_LIFETIME:
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

    def stream(self):
        import tarec
        h, bl, _stop = tarec.load(self.dir / self.head["recording"], exact=True)
        names = tarec.globals_names(tarec.globals_list(h["globals"])[2])
        table_names = {ix: t["name"] for ix, t in h["tables"].items()}
        st = {"evs": [], "prev_players": None, "orders": set()}
        kinds = {tarec.RK_EV_CREATE: "create", tarec.RK_EV_DEATH: "death",
                 tarec.RK_EV_SHOT: "shot", tarec.RK_EV_BUILT: "built"}
        runs = sorted(self.objs, key=lambda r: r.t0)

        def feature_defs(w):
            out = []
            for k in range(0, len(w.grid), tarec.REC_CELL_BYTES):
                d = struct.unpack_from("<H", w.grid, k + 8)[0]
                if d < 0xFFFB or d == 0xFFFE:
                    out.append(d)
            return out

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
                        g = bytes(w.globals or b"")
                        r.exogenous = {names.get(i, str(i)): struct.unpack_from("<I", g, 4 * i)[0]
                                       for i in range(len(g) // 4)
                                       if any(p in names.get(i, "") for p in
                                              ("rngseed", "wind"))}
                    r.events(L, st["evs"])
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
        return runs

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
