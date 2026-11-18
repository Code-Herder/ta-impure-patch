#!/usr/bin/env python3
"""opening.py -- the unmapped opening: ImpAI's first minutes on a level it has not seen, as a network game's
computer seat starts them. A kind of game the session tier plays (tools/impai/session.py); no command of its own.

THE GAME, `unmapped-<map>-<S>`: the bar's own scenario template with `mapping` false -- a network game is not
mapped, and the template's skirmish is -- and ImpAI Hard in seat S, its commander at the seat's own start, start S
of the map (StartPos(S + 1): the fixed starts seat seat k at start k, and a network game seats its computer
players at the starts after the humans', 2 and 3 of a two-human game). Every seat below S, and seat 2 under an
ImpAI in seat 1, holds a commander at its own start with its AI off (the exploiter suite's idle seat), so every
start a network game would fill before S is filled, nothing attacks, and the game is the opening alone. Seat 0 is
the template's human with no unit. ImpAI plays arm, the idle seats core. `<map>` is a key of bar.MAPS, the base
game's; the game is played to CAP ticks, exact from its id's seed (bar.game_seed) and recorded, as an exploiter
situation is, and judged a pass while ImpAI's seat holds a unit at the cap (exploit.py's `survives`).
THE MAPPED TWIN, `mapped-<map>-<S>`: the same game, seed included, with `mapping` true. An ImpAI seat knows the
map's layout in an unmapped game (tagpu_worldq_open_layout), so the two play the opening alike but for what the
engine itself does by a player's explored map: its pathfinder takes a block the unit's owner has not explored as
passable (0x40D7B0), so a walk over unexplored ground can take another way in the unmapped game.
MEASURED 2026-11-18 (GDI, no draw, the reference setup): a game plays in about 22 s; the level's own
`impai: starts` line seats ImpAI's seat at the start the id names (`2:272,4912@3` on Steel Jungle's start 2,
`3:128,8672@4` in the four-seat game of Lava Alley's start 3: start, position, seat + 1); and on the three
openings of confirm.txt the build that first answers an unmapped seat's base ring (08ddffe6) reads BETTER on
Steel Jungle and Trout Farm against the one before it (0a4ec04e: no order, 1 unit at the cap against 10 and
12) and SAME on Lava Alley.

THE FIGURES of one game, reported beside its verdict word:
    order      the GameTime of the first order ImpAI gave a unit: its first job row written in the recording
               (IMPAI_JOB.kind; think.py, A TICK: the job a decision at GameTime t writes shows at tick t + 1)
    plant      the first plant ordered, its type and GameTime: the log's first `impai: plant` line of the seat
    extractor  the GameTime of the first job that builds an extractor (IMPAI_JOB.build)
    at the cap the seat's extractors and units standing, frames included, and its metal income (its player
               record's last pass, bar.PLAYER_METAL_IN) at the cap's own tick
A figure is `none` when the game holds no such order, and `?` when it could not be read: no recording kept, a
recording of another layout than this tree's (think.py, THE LAYOUT; session.py's --tree), or no type names for
it (think.py, A UNIT TYPE IS PRINTED BY ITS NAME). An extractor is a type the base game names one (EXTRACTOR).

THE VERDICT WORD of two games of one opening whose decisions or results differ (session.py says SAME of two that
do not, and reads a seat lost against a seat held as it reads any pass against any FAIL), from their figures:
    a MILESTONE is an order given, a plant ordered, an extractor ordered, each by the cap; a figure not read in
    either game is no milestone of either. The UNITS differ when the two counts at the cap are more than
    UNITS_MARGIN apart and more than UNITS_SHARE of the larger.
    BETTER   the test reached a milestone the base did not, or holds more units, and nothing points the other way
    WORSE    the base reached a milestone the test did not, or holds more units, and nothing points the other way
    PARTED   neither, or one of each
WHAT IT CANNOT TELL. When an order was given is reported and not judged: a plant ordered 1 500 ticks later is
PARTED, not WORSE. Nothing past the cap is seen, and nothing of a fight: no seat attacks. The level is a
skirmish's with a network game's seats and mapping, not a network game: one process, no peer, the human seat
empty."""
import json
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(HERE))
import bar  # noqa: E402

CAP = 6000
ID = re.compile(r"(?:un)?mapped-([a-z0-9]+)-([1-9])")
SIDES = {"A": "arm", "B": "core"}
PLANT = re.compile(r"^impai: plant seat=(\d+) gametime=(\d+) .*?\bchose=(\S+)", re.M)
EXTRACTOR = re.compile(r"(?:MEX|MOHO|UWMME)$")        # the base game's extractors: ARMMEX, CORUWMEX, ARMMOHO, ...
JOB_NONE, BUILD_NONE = 0, 0xFFFF                        # IMPAI_JOB.kind's IMPAI_JOB_NONE; .build's WQ_NONE
MILESTONES = (("order", "an order"), ("plant", "a plant"), ("mex", "an extractor"))
UNITS_MARGIN, UNITS_SHARE = 2, 0.25


def is_id(gid: str) -> bool:
    return gid.startswith(("unmapped-", "mapped-"))


def mapping(gid: str) -> bool:
    """An opening's level is mapped: its MAPPED TWIN's id, `mapped-<map>-<S>`."""
    return gid.startswith("mapped-")


def parse_id(gid: str) -> tuple:
    """(the map's key, ImpAI's seat and start S) of an opening's id; BarError for any other."""
    m = ID.fullmatch(gid)
    if not m or m.group(1) not in bar.MAPS:
        raise bar.BarError(f"not an opening's id: {gid!r} ([un]mapped-<map>-<start 1..9>, a map of "
                           f"{', '.join(bar.MAPS)})")
    return m.group(1), int(m.group(2))


def idle_seats(s: int) -> list:
    """The seats that hold an idle commander beside ImpAI's seat `s`: every seat below it, seat 2 under seat 1."""
    return list(range(1, s)) or [2]


def spec(gid: str, cap: int = CAP) -> dict:
    """The bar game of opening `gid` less its scenario: what names its result (bar.game_kept), read with no
    install. `seat` is bar.seat_of's: A ImpAI's seat, B the first idle one."""
    m, s = parse_id(gid)
    return {"id": gid, "pair": "opening", "map": bar.map_name(m), "sides": dict(SIDES), "start": s,
            "seat": {"A": s, "B": idle_seats(s)[0]}, "rep": 1, "difficulty": "hard",
            "seats": {"A": {"ai": "impai", "level": "hard"}, "B": {"ai": "ai", "level": None}}, "cap": cap,
            "seed": bar.game_seed(f"unmapped-{m}-{s}"), "recordings": True, "setup": None, "values": [], "waves": [],
            "verdict": "survives"}


def game(gid: str, sdir: Path, cap: int = CAP, starts: dict = None) -> dict:
    """The bar game of opening `gid`: its spec and its scenario (`doc`), written under `sdir`. `starts` is the
    map's {StartPos N: (x, z)}, read from the install when none (bar.start_positions, for a skirmish of the
    game's seats)."""
    g = spec(gid, cap)
    s = g["seat"]["A"]
    idle = idle_seats(s)
    if starts is None:
        starts = bar.start_positions(g["map"], seats=max(idle + [s]) + 1)
    doc = json.loads(bar.TEMPLATE.read_text())
    st = doc["setup"]
    st["map"], st["difficulty"], st["mapping"] = g["map"], g["difficulty"], mapping(gid)
    doc["units"] = []
    for k in sorted(idle + [s]):
        if k + 1 not in starts:
            raise bar.BarError(f"{gid}: {g['map']} has no StartPos{k + 1}")
        side = SIDES["A" if k == s else "B"]
        p = {"slot": k, "controller": "impai" if k == s else "ai", "side": side, "color": k,
             "storage": {"metal": 1000, "energy": 1000}}
        if k != s:
            p["ai"] = "off"
        st["players"].append(p)
        doc["units"].append({"id": f"cmd{k}", "type": bar.COMMANDER[side], "owner": k, "pos": list(starts[k + 1]),
                             "facing": 0})
    doc["description"] = f"opening.py: {gid}"
    doc["seed"] = g["seed"]
    sdir.mkdir(parents=True, exist_ok=True)
    (sdir / f"{gid}.json").write_text(json.dumps(doc, indent=1) + "\n")
    return dict(g, doc=doc)


# ---- the figures ----

def recorded(rec: Path, seat: int, cap: int, trees=None) -> dict:
    """THE FIGURES a recording holds, each key present only when read: `mexes` and `income` at tick `cap`, `order`
    and `mex` from the seat's job rows up to it. Raises what think.py and tarec.py raise of a recording they
    cannot read."""
    import think
    out = {}
    ex = think.extract(rec)
    names = think.type_names(ex)
    rows = think.seat_rows(ex, "job", seat)
    if names:
        live = {}
        for i in range(len(ex.uticks)):
            if ex.uticks[i] > cap:
                break
            if ex.uslots[i] in rows:
                if ex.uborn[i]:
                    live[ex.uslots[i]] = ex.utypes[i]
                else:
                    live.pop(ex.uslots[i], None)
        # a unit image's type ID is its index + 1 (think.type_names)
        out["mexes"] = sum(bool(0 < t <= len(names) and EXTRACTOR.search(names[t - 1])) for t in live.values())
    try:
        lay = think.fit(ex, think.layouts(trees))
        _start, events = think.changes(ex, think.select(ex, lay, seat, ["job.kind", "job.build"]), 1, cap + 1)
        out["order"] = next((t - 1 for t, _p, _row, f, _old, raw in events
                             if f.name == "kind" and f.value(raw) != JOB_NONE), False)
        if names:
            built = ((t, f.value(raw)) for t, _p, _row, f, _old, raw in events if f.name == "build")
            out["mex"] = next((t - 1 for t, v in built if v != BUILD_NONE and v < len(names)
                               and EXTRACTOR.search(names[v])), False)
    except think.ThinkError:
        pass            # another layout's recording: its job rows stay unread, the rest is read
    sys.path.insert(0, str(TREE / "tools" / "replay"))
    import tarec
    h, bl, _stop = tarec.load(rec)
    w = tarec.stream_to(h, bl, want=cap)
    if w.tick == cap and seat < len(w.players):
        out["income"] = round(struct.unpack_from("<f", w.players[seat], bar.PLAYER_METAL_IN)[0], 2)
    return out


def figures(r: dict, log: str, rec=None, trees=None, recorded=recorded) -> dict:
    """THE FIGURES of one game from its result `r`, its log's text and its recording `rec`: {"order", "plant"
    ([GameTime, type]), "mex", "mexes", "units", "income"}, each None when not read and the three orders False
    when the game holds none. `recorded` is what reads the recording."""
    out = {"order": None, "plant": None, "mex": None, "mexes": None, "units": None, "income": None}
    seat = ((r.get("seats") or {}).get("A") or {}).get("seat")
    end = r.get("end_tick")
    if not isinstance(seat, int) or not isinstance(end, int):
        return out
    if "impai:" in (log or ""):
        out["plant"] = next(([int(m.group(2)), m.group(3)] for m in PLANT.finditer(log) if int(m.group(1)) == seat),
                            False)
    last = (((r.get("looks") or [{}])[-1].get("seats") or {}).get("A") or {})
    out["units"] = last.get("alive")
    if rec is not None:
        try:
            out.update(recorded(rec, seat, end, trees))
        except Exception:                                       # noqa: BLE001
            pass        # a figure not read is `?`, and judges nothing: a recording that does not read fails no game
    return out


def figures_text(f: dict) -> str:
    """One game's figures on a line: `order 44; plant ARMVP 374; extractor none; at the cap 0 extractors, 12
    units, metal 2.5`."""
    def tick(v):
        return "?" if v is None else "none" if v is False else str(v)

    def n(v):
        return "?" if v is None else f"{v:g}"
    plant = f.get("plant")
    return (f"order {tick(f.get('order'))}; plant {f'{plant[1]} {plant[0]}' if plant else tick(plant)}; "
            f"extractor {tick(f.get('mex'))}; at the cap {n(f.get('mexes'))} extractors, {n(f.get('units'))} units, "
            f"metal {n(f.get('income'))}")


def verdict(base: dict, test: dict) -> tuple:
    """THE VERDICT WORD of two games' figures, as (1 BETTER, -1 WORSE, 0 PARTED; why)."""
    both = [(k, what) for k, what in MILESTONES if base.get(k) is not None and test.get(k) is not None]
    gained = [what for k, what in both if test[k] and not base[k]]
    lost = [what for k, what in both if base[k] and not test[k]]
    ub, ut = base.get("units"), test.get("units")
    more = 0
    if isinstance(ub, int) and isinstance(ut, int) and abs(ut - ub) > max(UNITS_MARGIN, UNITS_SHARE * max(ub, ut)):
        more = 1 if ut > ub else -1
    why = [f"the test alone gives {', '.join(gained)}"] if gained else []
    why += [f"the base alone gives {', '.join(lost)}"] if lost else []
    why += [f"units at the cap {ub} | {ut}"] if more else []
    up, down = bool(gained) or more > 0, bool(lost) or more < 0
    return (up - down, "; ".join(why) or "the same milestones, the units within the margin")
