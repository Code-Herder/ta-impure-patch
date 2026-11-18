#!/usr/bin/env python3
"""thinkview.py -- one page that shows what ImpAI was thinking through recorded games: the war room and the
flight recorder as one instrument.

    tools/impai/thinkview.py REC [REC...] [--seat S] [--tree DIR]... [--gamedir DIR]... [--content FILE]...
                             --out DIR [--step N] [--max-bytes B] [--bare]

Writes DIR/index.html and, for each recording, DIR/<name>.js and DIR/<map>.<crc>.map.webp: the page (thinkview.html,
beside this file), one data file a recording and seat, which the page loads when it is picked, and the map's
picture, one a map file. A recording several ImpAI seats played is a data file a seat (`<name>-seat<S>.js`) unless --seat names
one: the page lists a recording once, as a GAME, and a button a seat beside it. The page needs no server and no network but its fonts: open index.html. --bare writes the page without the
document around it (doctype, head, body), for a host that wraps what it is given in its own.

THE VIEW is two halves of one clock. THE FLIGHT RECORDER is the whole game on one time axis: a lane a squad
with its tasks as spans, a lane a judgement of the seat's (naval, island, dry hunt, lead, pressure, base threat,
what the enemy base's estimate rests on), a row of events, and curves. THE WAR ROOM is the map at the moment the
recorder's cursor stands on: the seat's anchor and base box, its units by job, each squad with its task and goal
line, the enemy base's estimate, and the enemy as the seat knows it (in sight, a radar or sonar contact, a
remembered ghost) over the enemy as it is, which the page draws faintly and can hide.

THE GROUND UNDER THE WAR ROOM (thinkmap.py). The map's picture is rendered from the game's own map file, found
in the archives the recording's header lists (--gamedir DIR names a game directory to look in; by default the
one the recording lies in, `<gamedir>/replays/`, then the install tavfs.py finds). The heights, the metal and the
features are the recording's own feature grid: the metal and the heights as the first sample holds them, a
feature from the sample it is first seen at to the sample it is gone at. A unit type's facts -- it extracts
metal, it floats, it flies -- are its FBI's in the same archives, by the name the content file gives its number
(--content FILE names a content file of predfit.py's for a mod; the first whose count is the recording's names
the types). What could not be read is a line of the page, never a guess. THE PICTURE IS DERIVED FROM THE
ORIGINAL GAME: it is written into --out and nowhere else.

WHO PLAYS (seat_facts). Every seat of the recording's header is a row of the data's `who` and of the page's
list: its side, who played it (the header's controller byte, player record `+0x73`: 1 a human at the recording
machine, 2 a computer there, 3 a player on another machine; and the header's own byte for ImpAI), its COLOUR
(the colour index, PlayerInfo `+0x96`, and that index's colour as the recording's archives draw it,
thinkmap.team_colours; STOCK_RGB, the unmodded game's, when the archives could not be read, and the page says
which), its ALLIES (the seats whose alliance byte it holds, player record `+0x108`, and that hold its own: the
alliances the game starts with) and its START: where its first unit that outlives the level's first ticks -- its
commander in a skirmish -- first stands, as a compass word over the map's thirds. The header's rows are tarec.py's
(think.py's extract keeps a seat's name, side and ImpAI byte alone), read from the file's first block before
anything is counted (header_rows). A fact that could not be read is None in the row and a line of the page, never
a guess.

AN ALLY'S ROW IS NO ENEMY'S. The seat's table of what it knows holds its allies' units beside its enemies', and
the brain's rules read "a row whose owner is neither the seat nor an ally"; so do the counts here (known_counts:
the `known` curve, the first contact, an enemy coming into sight) and the page, which draws an ally's row in the
ally's colour. WHOSE A ROW IS is the truth's as well as the table's: a row is a seat's when its owner field names
the seat or when its slot lies in the seat's block of units, since the table holds an ally's radar contact under
an owner not yet identified, which the page marks as such. A seat whose alliances could not be read counts every
other seat's row, and the page says so.

EVERYTHING ELSE IS READ THROUGH think.py: the brain's state by field name through the compiler's layout
(thinklayout.py), so no offset of ImpAI's is written here, and a recording of another layout is refused by name
(--tree DIR names the tree that made it). THE TREE AT THE RECORDING'S BUILD READS IT: sizes are all think.py
compares, and two builds of the same sizes may number an enum's members otherwise (a job's kinds have), so of
the trees that fit, one checked out at the commit the recording's header names is taken first, then each --tree
in the order given, and this tree last; the page says so when none is at the recording's build. What think.py's extract does not keep -- where each unit stands, and
each seat's metal income -- is read from the same stream in one more pass (sample), through tarec.py's own
accessors.

EXACT AND SAMPLED. The lanes, the events, the squads' state and every scalar of the seat's are EXACT at every
tick: they are the ticks at which a field changes (think.changes). The map's units, the builders' jobs, the rows
of what the seat knows and the held ground are SAMPLED: at every DECISION TICK (a tick at which a squad's task or
goal changes, or an event falls) and every --step ticks between (default: about 240 over the game). The page
says which sample its map shows when the cursor stands between two.

A TICK is think.py's: tick T holds what the step from GameTime T - 1 wrote.

THE SIZE BOUND. A data file is held to --max-bytes (default 3 000 000): when it is larger, every second sample
that is no decision tick is dropped until it fits, the lanes untouched; a game whose decision ticks alone pass
the bound is a message and exit 2.

UNITS OF PLACE (impai.h): a squad's point, the estimate and a known row are 32-px blocks; an anchor, a job's
site and a unit image's footprint are 16-px cells; held ground is 256-px cells, a bit a cell in rows of
IMPAI_GRID_MAX. The page draws in pixels and prints blocks, as think.py does.

Exit 0, 2 (could not run)."""
import argparse
import json
import math
import re
import struct
import subprocess
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import think  # noqa: E402
import thinkmap  # noqa: E402

TEMPLATE = HERE / "thinkview.html"
MAX_BYTES = 3_000_000
SAMPLES = 240                   # regular samples over a game, before the decision ticks
BLOCK_PX, CELL_PX, HELD_PX = 32, 16, 256        # UNITS OF PLACE
# the seat's scalars the page reads, exact at every tick (think.py's paths)
PATHS = ("mil.level", "mil.ebx", "mil.ebz", "mil.wave_t", "mil.scout_t", "mil.waves", "mil.naval", "mil.est_seen",
         "mil.sq[*].task", "mil.sq[*].layer", "mil.sq[*].n", "mil.sq[*].flags", "mil.sq[*].tx", "mil.sq[*].tz",
         "mil.sq[*].target", "mil.sq[*].since", "mil.sq[*].value", "mil.foe", "mil.press_until", "mil.press_v",
         "mil.press_bx", "mil.press_bz", "mil.press_base", "mil.threat_seen", "mil.radar_as", "mil.dry", "mil.lead",
         "mil.island", "mil.unreached", "mil.lost_t", "mil.mob_v", "mil.safe_until", "brain.ax", "brain.az",
         "brain.ring_grow", "brain.nway", "brain.opened", "brain.sea_refused", "brain.way[*].type",
         "brain.way[*].cx", "brain.way[*].cz")
# the judgements' lanes: (id, label, the field, {value or enumerator: (what it says, tone)}); a value not named
# draws nothing. "*" names every value but 0
JUDGEMENTS = (("naval", "naval", "mil.naval", {1: ("the map is judged naval", "info"),
                                                -1: ("naval not judged yet", "muted")}),
              ("island", "island", "mil.island", {1: ("the land army reaches no enemy ground", "info")}),
              ("dry", "dry hunt", "mil.dry", {1: ("no land enemy known for a long while", "warn")}),
              ("lead", "lead", "mil.lead", {1: ("far ahead on what it knows", "good")}),
              ("threat", "base threat", "mil.threat_seen", {"*": ("a threat stands at the base", "bad")}),
              ("estimate", "estimate", "mil.est_seen", {"START": ("the enemy base is a start position, a guess", "muted"),
                                                        "STRUCT": ("the enemy base is where structures were seen", "enemy"),
                                                        "KEPT": ("the enemy base is a centre kept from memory", "ghost")}))
FOE_GAP = 900                   # ticks between two sightings of a commander that are two events
START_TICKS = 60                # every unit's first place is kept exactly through these first ticks (sample, "first")
PLACEHOLDER_TICKS = 30          # a unit gone within these ticks of its making is the engine's placeholder, no player's
SIDES = {0: "Arm", 1: "Core"}   # the header's side byte, PlayerInfo +0x95
# who played a seat, by the header's controller byte (player record +0x73): (the data's word, what the page says)
CONTROLLERS = {1: ("human", "human"), 2: ("ai", "another AI"), 3: ("net", "a player on another machine")}
# THE UNMODDED GAME'S PLAYER COLOURS, a colour index: the middle pixel of each frame of `32xlogos` in
# textures/logos.gaf through palettes/palette.pal. MEASURED 2026-11-18 with thinkmap.team_colours on the retail
# archives (palette indices 105, 212, 80, 167, 109, 219, 208, 93, 131, 67). The stand-in when a recording's own
# archives cannot be read; a mod may draw others (Escalation does).
STOCK_RGB = ((35, 59, 203), (211, 43, 0), (251, 251, 251), (47, 119, 27), (7, 23, 99),
             (127, 87, 159), (255, 255, 0), (43, 43, 43), (131, 183, 207), (171, 171, 131))


class ViewError(think.ThinkError):
    pass


def num(v):
    """A value as the data files keep it: a float to one decimal, one not finite as 0."""
    if isinstance(v, float):
        return round(v, 1) if math.isfinite(v) else 0
    return v


def compass(x: float, z: float, w: float, h: float) -> str:
    """Where (x, z) lies on a map of w x h, as a compass word over the map's thirds: north is z = 0, as the game
    draws it, and the middle third of both is the centre."""
    ew = "west" if x * 3 < w else "east" if x * 3 >= 2 * w else ""
    ns = "north" if z * 3 < h else "south" if z * 3 >= 2 * h else ""
    return f"{ns}-{ew}" if ns and ew else ns or ew or "centre"


def colour_word(rgb) -> str:
    """A colour's everyday name, for a sentence beside its swatch: by hue, with black, white and grey apart."""
    r, g, b = (c / 255 for c in rgb)
    hi, lo = max(r, g, b), min(r, g, b)
    sat = 0 if hi == 0 else (hi - lo) / hi
    if hi < .22:
        return "black"
    if sat < .12:
        return "white" if hi > .8 else "grey"
    d = hi - lo
    hue = (60 * ((g - b) / d % 6) if hi == r else 60 * ((b - r) / d + 2) if hi == g else 60 * ((r - g) / d + 4)) % 360
    name = next(n for top, n in ((15, "red"), (45, "orange"), (70, "yellow"), (165, "green"), (190, "cyan"),
                                 (262, "blue"), (300, "purple"), (345, "pink"), (361, "red")) if hue < top)
    return ("dark " if hi < .45 else "pale " if sat < .3 else "light " if sat < .45 and hi > .7 else "") + name


def header_rows(path) -> dict:
    """{seat: the header's row of it, tarec.py's} of the recording at `path`, read from its first block alone;
    {} when it cannot be read, and every fact of WHO PLAYS that rests on it is then unread."""
    sys.path.insert(0, str(think.TREE / "tools" / "replay"))
    try:
        import tarec
        return {p["index"]: p for p in tarec.header_only(str(path))["players"]}
    except Exception:                                           # noqa: BLE001
        return {}


def allies_of(rows: dict, seat: int):
    """The seats `seat` starts allied with, by the header's rows: each holds the other's alliance byte. None
    when the seat's row was not read."""
    if (rows.get(seat) or {}).get("alliances") is None:
        return None
    holds = lambda a, b: b < len(rows[a].get("alliances") or b"") and rows[a]["alliances"][b] != 0     # noqa: E731
    return [o for o in sorted(rows) if o != seat and holds(seat, o) and holds(o, seat)]


def seat_facts(ex, rows, colours, world: dict, first: dict, dims) -> list:
    """WHO PLAYS: a row a seat of the recording, [{"seat", "name", "side": its side's name, "kind": "impai" |
    "human" | "ai" | "net" | None, "by": the same as the page says it, "logo": its colour index, "rgb": [r, g, b],
    "colour": the colour's word, "allies": [seat, ...], "start": [x, z] in px, "start_t": the tick it is read at,
    "start_exact": that tick is the unit's first, "where": the compass word}, ...]; None for what could not be
    read. `rows` are header_rows' ({} when the header was not read),
    `colours` {"rgb": [a colour an index]} or None, `world` sample's samples and `first` its {slot: [(tick, x, z), ...]}
    of the first ticks."""
    lives = [l for l in unit_lives(ex) if l[3] is None or l[3] - l[2] > PLACEHOLDER_TICKS]
    table = (colours or {}).get("rgb") or []
    out = []
    for p in ex.players:
        seat, row = p["index"], rows.get(p["index"]) or {}
        kind, by = ("impai", "ImpAI") if p.get("ai") == 1 else CONTROLLERS.get(row.get("controller"), (None, None))
        logo = row.get("logo")
        rgb = table[logo] if logo is not None and logo < len(table) and table[logo] else None
        allies = allies_of(rows, seat)
        start = start_t = exact = None
        life = min((l for l in lives if ex.seat_of(l[0]) == seat), key=lambda l: (l[2], l[0]), default=None)
        if life is not None:
            early = next((e for e in first.get(life[0], ()) if e[0] >= life[2]), None)
            if early:
                start_t, exact, start = early[0], True, [early[1], early[2]]
            else:                                               # made after the first ticks: its first sample
                for t in sorted(t for t in world if t >= life[2] and (life[3] is None or t < life[3])):
                    at_t = next((u for u in world[t]["units"] if u[0] == life[0]), None)
                    if at_t:
                        start_t, exact, start = t, False, [at_t[1], at_t[2]]
                        break
        side = p.get("side")
        out.append({"seat": seat, "name": p.get("name") or f"seat {seat}",
                    "side": None if side is None else SIDES.get(side, f"side {side}"), "kind": kind, "by": by,
                    "logo": logo, "rgb": list(rgb) if rgb else None, "colour": colour_word(rgb) if rgb else None,
                    "allies": allies, "start": start, "start_t": start_t, "start_exact": exact,
                    "where": compass(start[0], start[1], dims[0], dims[1]) if start else None})
    return out


def at(track, t, default=0):
    """A track's value at tick `t`: its last change at or before it; `default` before the first (a field
    never written reads as zeros)."""
    if not track:
        return default
    lo, hi = 0, len(track[0])
    while lo < hi:
        mid = (lo + hi) // 2
        if track[0][mid] <= t:
            lo = mid + 1
        else:
            hi = mid
    return track[1][lo - 1] if lo else default


def tracks(ex, lay, seat, paths) -> tuple:
    """({field's name: [[tick, ...], [value, ...]]} of every scalar `paths` name, at each tick it changes; the
    paths this layout or this recording does not have; the names of the scalars it has, changed or not)."""
    fields, missing = {}, []
    for path in paths:
        try:
            for sel in think.select(ex, lay, seat, [path]):
                fields.setdefault(sel.part, {}).update({f.name: f for f in sel.fields})
        except think.ThinkError:
            missing.append(path)
    sels = [think.Selection(ex, lay, part, seat, list(fs.values())) for part, fs in fields.items()]
    _start, events = think.changes(ex, sels)
    out = {}
    for t, part, row, f, _old, raw in events:
        tr = out.setdefault(think.label(part, row, f), [[], []])
        tr[0].append(t)
        tr[1].append(num(f.value(raw)))
    have = {think.label(sel.part, sel.rows.start, f) for sel in sels for f in sel.fields}
    return out, missing, have


def runs(track, end: int) -> list:
    """[[from, to, value], ...]: the stretches of ticks 0..end a track holds one value over."""
    out, t0, v0 = [], 0, 0
    for t, v in zip(*(track or [[], []])):
        if t > end:
            break
        if v != v0:
            if t > t0:
                out.append([t0, t, v0])
            t0, v0 = t, v
    if end > t0:
        out.append([t0, end, v0])
    return out


def lanes(episodes: list, end: int) -> list:
    """The squads' lanes: [{"k": squad, "spans": [[from, to, task, index of the episode], ...]}, ...], a squad
    that ever left FREE a lane, each task it held one span (think.squad_episodes), a task still held at the end
    ending there."""
    by = {}
    for i, e in enumerate(episodes):
        if e["from"] < end:
            to = end if e["to"] is None else min(e["to"], end)
            by.setdefault(e["squad"], []).append([e["from"], to, e["task"], i])
    return [{"k": k, "spans": by[k]} for k in sorted(by)]


def judgement_lanes(trk: dict, enums: dict, end: int, have=None) -> list:
    """The judgements' lanes: [{"id", "label", "spans": [[from, to, what it says, tone], ...]}, ...] of JUDGEMENTS'
    fields this recording has (`have`: tracks' third answer; by default those that changed), and the pressure's
    (pressure_spans). A judgement that never stood is a lane with no span."""
    out = []
    have = set(trk) | set(enums) if have is None else have
    for ident, label, path, says in JUDGEMENTS:
        if path not in have:
            continue
        names = enums.get(path, {})
        spans = []
        for t0, t1, v in runs(trk.get(path), end):
            said = says.get(v) or says.get(names.get(str(v))) or (says.get("*") if v else None)
            if said:
                if spans and spans[-1][1] == t0 and spans[-1][2:] == list(said):
                    spans[-1][1] = t1
                else:
                    spans.append([t0, t1, *said])
        out.append({"id": ident, "label": label, "spans": spans})
    if "mil.press_until" in trk:
        out.insert(min(4, len(out)), {"id": "pressure", "label": "pressure",
                                      "spans": [[a, b, "outmatched near the base: constructors held", "bad"]
                                                for a, b in pressure_spans(trk["mil.press_until"], end)]})
    return out


def pressure_spans(track, end: int) -> list:
    """[[from, to], ...]: the ticks the pressure holds over. A stamp written at tick t holds to the GameTime it
    names (press_until); stamps that meet are one span."""
    out = []
    for t, until in zip(*track):
        if t >= end or until <= t:
            continue
        to = min(until + 1, end)
        if out and t <= out[-1][1]:
            out[-1][1] = max(out[-1][1], to)
        else:
            out.append([t, to])
    return out


def known_counts(ex, lay, seat, allies=None) -> dict:
    """What the seat knows of its enemies, exact: {"now", "pass", "ghost": [[ticks], [rows]]} -- the rows it
    holds in sight (its own or an ally's), as a radar or sonar contact alone, and as a ghost -- and "first", the
    first tick it knows any, None never. Its own rows are not counted, nor a row of the seat's or of one of
    `allies` (AN ALLY'S ROW IS NO ENEMY'S; None, the alliances unread: every row not flagged its own is counted,
    whoever owns it). A row is a seat's when the table's own owner field names it at that tick, or when the row's
    slot lies in that seat's block of units: the table holds an ally's radar contact under an owner not yet
    identified (255), and a ghost of the seat's own lost unit with the own flag gone."""
    try:
        fields = {f.name: f for f in lay.seat_fields("known", seat) if f.name in ("how", "owner")}
        sel = think.Selection(ex, lay, "known", seat, [fields["how"]] + ([fields["owner"]] if "owner" in fields else []))
        _start, events = think.changes(ex, [sel])
    except (think.ThinkError, KeyError):
        return {}
    m = lay.macros
    own, ghost = m.get("IMPAI_OWN", 0), m.get("IMPAI_GHOST", 0)
    now = m.get("IMPAI_LOS", 0) | m.get("IMPAI_SHARED", 0)
    friends = set() if allies is None else set(allies) | {seat}

    def klass(v, owner, row):
        if not v or v & own or owner in friends or ex.seat_of(row) in friends:
            return None
        return "now" if v & now else "ghost" if v & ghost else "pass"
    count = {"now": 0, "pass": 0, "ghost": 0}
    out = {k: [[], []] for k in count}
    first = None
    by_tick, held = {}, {}                  # held: {row: [how, owner]} as the last change leaves each; never written, zeros
    for t, _part, row, f, old, raw in events:
        by_tick.setdefault(t, []).append((row, f.name, f.value(old), f.value(raw)))
    for t in sorted(by_tick):
        before, was = dict(count), {}
        for row, name, old, new in by_tick[t]:
            cur = held.setdefault(row, [0, 0])
            if row not in was:
                was[row] = klass(*cur, row)
            cur[0 if name == "how" else 1] = new
        for row, a in was.items():
            b = klass(*held[row], row)
            if a:
                count[a] -= 1
            if b:
                count[b] += 1
        if first is None and any(count.values()):
            first = t
        for k in count:
            if count[k] != before[k]:
                out[k][0].append(t)
                out[k][1].append(count[k])
    return dict(out, first=first)


def place(bx, bz) -> str:
    return f"({bx},{bz})"


def events(ex, trk: dict, episodes: list, known: dict, enums: dict, names: dict, end: int) -> list:
    """The recorder's events, [[tick, kind, text, squad or -1], ...] in tick order. Kinds: `task` a squad takes
    a task, `leg` it takes another goal, `wave` an attack wave starts, `lost` a prediction is lost (a squad's
    retreat or the home's), `plant` a plant's way out is kept (the plant is sited), `contact` the first enemy
    row, `sight` an enemy comes into sight when none was, `cmdr` an enemy commander seen, `judge` a judgement or
    the estimate's ground changes, `free` a squad lets its task go and takes none."""
    out = []
    lost = set(trk.get("mil.lost_t", [[]])[0])
    wave = set(trk.get("mil.wave_t", [[]])[0])
    for e in episodes:
        t0 = e["from"]
        goal = next(((g, s) for _t, g, s in e["legs"] if any(g) or s is not None), None)
        n = e["n"][1]
        text = f"sq[{e['squad']}] {e['task']} {e['layer'].lower()}, {n} unit{'s' if n != 1 else ''}"
        if goal:
            owner = ex.seat_of(goal[1]) if goal[1] is not None else None
            text += f", goal {place(*goal[0])}" + (f" slot {goal[1]}" if goal[1] is not None else "") \
                + (f" of seat {owner}" if owner is not None else "")
        if e["task"] == "RETREAT" and t0 in lost:
            text += ", on a lost prediction"
        out.append([t0, "task", text, e["squad"]])
        for t, g, s in e["legs"]:
            if t != t0 and (any(g) or s is not None):
                out.append([t, "leg", f"sq[{e['squad']}] {e['task']} goes for {place(*g)}"
                            + (f" slot {s}" if s is not None else ""), e["squad"]])
    by_start = {}
    for e in episodes:
        by_start.setdefault(e["from"], []).append(e)
    for e in episodes:
        if e["to"] is not None and not any(n["squad"] == e["squad"] for n in by_start.get(e["to"], [])):
            out.append([e["to"], "free", f"sq[{e['squad']}] lets its {e['task']} go: FREE", e["squad"]])
    for t in sorted(wave):
        went = [f"sq[{e['squad']}], {e['n'][1]} unit{'s' if e['n'][1] != 1 else ''}"
                for e in by_start.get(t, []) if e["task"] == "ATTACK"]
        out.append([t, "wave", "an attack wave starts" + (": " + "; ".join(went) if went else ""), -1])
    for t in sorted(lost):
        back = [f"sq[{e['squad']}]" for e in by_start.get(t, []) if e["task"] == "RETREAT"]
        out.append([t, "lost", ("a prediction is lost: " + ", ".join(back) + " retreat" + ("s" if len(back) == 1 else ""))
                    if back else "a prediction is lost at home", -1])
    prev = 0
    for t, v in zip(*trk.get("brain.nway", [[], []])):
        for i in range(prev, v):
            typ = at(trk.get(f"brain.way[{i}].type"), t, None)
            cx, cz = at(trk.get(f"brain.way[{i}].cx"), t), at(trk.get(f"brain.way[{i}].cz"), t)
            what = names.get(typ, f"type {typ}") if typ is not None else "a plant"
            out.append([t, "plant", f"{what} sited at block {place(cx * CELL_PX // BLOCK_PX, cz * CELL_PX // BLOCK_PX)}"
                                    ": its way out is kept", -1])
        prev = max(prev, v)
    if known.get("first") is not None:
        out.append([known["first"], "contact", "the first enemy row: the seat knows of an enemy", -1])
    before = 0
    for t, n in zip(*known.get("now", [[], []])):
        if n and not before and t != known.get("first"):
            out.append([t, "sight", "an enemy comes into sight: none was", -1])
        before = n
    for key in sorted(trk):
        m = re.match(r"^mil\.foe\[(\d+)\]\.t$", key)
        if not m:
            continue
        i, last = m.group(1), None
        for t, seen in zip(*trk[key]):
            if seen and (last is None or t - last > FOE_GAP):
                bx, bz = at(trk.get(f"mil.foe[{i}].bx"), t), at(trk.get(f"mil.foe[{i}].bz"), t)
                out.append([t, "cmdr", f"seat {i}'s commander seen at {place(bx, bz)}", -1])
            if seen:
                last = t
    for ident, _label, path, says in JUDGEMENTS:
        if ident == "threat":
            continue
        cnames = enums.get(path, {})
        first = True
        for t0, _t1, v in runs(trk.get(path), end):
            said = says.get(v) or says.get(cnames.get(str(v)))
            if t0 > 1 and (said or not first):
                out.append([t0, "judge", said[0] if said else f"{_label}: no longer", -1])
            first = False
    for t, v in zip(*trk.get("brain.opened", [[], []])):
        if v:
            out.append([t, "judge", "the first factory is counted: the opening is over", -1])
    order = {"wave": 0, "lost": 1, "task": 2, "leg": 3, "free": 5}
    out = [e for e in out if 0 <= e[0] <= end]
    out.sort(key=lambda e: (e[0], order.get(e[1], 4), e[3]))
    return out


def unit_lives(ex) -> list:
    """Every unit's life, exact: [[slot, type index in ImpAI's tables, the tick it appears, the tick it is gone
    or None], ...] in the order they appear. A slot whose type changes is a new life."""
    lives, cur = [], {}
    for i in range(len(ex.uticks)):
        slot, t = ex.uslots[i], ex.uticks[i]
        if slot in cur:
            lives[cur.pop(slot)][3] = t
        if ex.uborn[i]:
            cur[slot] = len(lives)
            lives.append([slot, ex.utypes[i] - 1, t, None])
    return lives


def sample(path, ticks) -> tuple:
    """((the map's width, height in px), {tick: {"units": [(slot, x, z, structure, frame), ...], "metal": {seat:
    income}}}, the ground: {"track": thinkmap.Track of the feature grid over the samples or None without numpy,
    "archives": the header's, "map": its map's name, "first":
    {slot: [(tick, x, z), ...]}, each unit's place at each of the first START_TICKS ticks, whatever `ticks` holds}) at
    each of `ticks`, read from the recording's stream
    through tarec.py: a unit's place is its
    footprint's centre in px (tarec.rect), `structure` tarec.structure, `frame` 1 while its build fraction is not
    0; a seat's income its player record's last pass (bar.py's PLAYER_METAL_IN), {} when bar.py cannot be
    loaded. The world is taken as tick T's last record leaves it, which is what think.py's tick T holds."""
    sys.path.insert(0, str(think.TREE / "tools" / "replay"))
    import tarec
    try:
        import bar
        metal_at = bar.PLAYER_METAL_IN
    except Exception:                                           # noqa: BLE001
        metal_at = None
    want, out, track, first = set(ticks), {}, None, {}
    try:
        h, bl, _stop = tarec.load(str(path))
        seats = [p["index"] for p in h["players"]]
        built = struct.Struct("<f")
        gw, gh = h["features"][0], h["features"][1]
        try:
            track = thinkmap.Track(gw, gh)
        except ImportError:
            track = None

        def places(w):
            units = []
            for slot, u in w.units.items():
                cells = tarec.rect(u)
                if not cells:
                    continue
                xs, zs = [c[0] for c in cells], [c[1] for c in cells]
                units.append((slot, (min(xs) + max(xs) + 1) * CELL_PX // 2, (min(zs) + max(zs) + 1) * CELL_PX // 2,
                              1 if tarec.structure(u) else 0, 1 if built.unpack_from(u, tarec.U_BUILT)[0] else 0))
            return units

        def take(w):
            units = places(w)
            metal = {}
            if metal_at is not None:
                metal = {s: num(struct.unpack_from("<f", w.players[s], metal_at)[0]) for s in seats}
            out[w.tick] = {"units": units, "metal": metal}
            if track is not None:
                track.take(w.tick, w.grid)

        def on_record(w, kind, _b):
            if kind is not None:
                return
            if w.tick is not None and w.tick <= START_TICKS:
                for slot, x, z, _structure, _frame in places(w):
                    first.setdefault(slot, []).append((w.tick, x, z))
            if w.tick in want and w.tick not in out:
                take(w)
        w = tarec.stream_to(h, bl, on_record=on_record)
        if w.tick in want and w.tick not in out:
            take(w)
    except Exception as e:                                      # noqa: BLE001
        raise ViewError(f"{path}: its stream could not be sampled ({type(e).__name__}: {e})") from None
    return (gw * CELL_PX, gh * CELL_PX), out, {"track": track, "archives": h.get("archives"), "map": h.get("map"),
                                               "first": first}


def tree_build(tree) -> str:
    """The commit a source tree is checked out at, as a recording's header names a build (8 hex digits); "" when
    git names none."""
    try:
        return subprocess.run(["git", "-C", str(tree), "rev-parse", "--short=8", "HEAD"], capture_output=True, text=True,
                              timeout=30).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def layout_for(ex, lays: list, builds=None) -> tuple:
    """(the layout `ex` is read by, {"build": its tree's commit, "exact": the tree is at the recording's build}):
    THE TREE AT THE RECORDING'S BUILD READS IT; with none, the first --tree that fits, in the order given, and
    this tree (`lays`' first, think.layouts) last: a --tree is named because this tree reads a recording wrongly.
    A build named `-dirty` was made from edits no commit holds, so no tree is exactly its."""
    builds = {} if builds is None else builds
    for lay in lays:
        if lay.tree not in builds:
            builds[lay.tree] = tree_build(lay.tree)
    made = ex.build.split("-")[0]
    own = [lay for lay in lays if builds[lay.tree] and builds[lay.tree] == made]
    lay = think.fit(ex, own + [x for x in lays[1:] + lays[:1] if x not in own])
    return lay, {"build": builds[lay.tree], "exact": lay in own and made == ex.build}


def game_dirs(recs: list, given: list) -> list:
    """The game directories a map file is looked for in: --gamedir's, else each recording's own (it lies in
    `<gamedir>/replays/`) and the install tavfs.py finds."""
    if given:
        return [Path(d) for d in given]
    out = [Path(r).resolve().parent.parent for r in recs if Path(r).resolve().parent.name.lower() == "replays"]
    try:
        out.append(Path(thinkmap._tavfs().find_install(None)))
    except (Exception, SystemExit):                             # noqa: BLE001
        pass
    return list(dict.fromkeys(d for d in out if d.is_dir()))


def terrain(found: dict, names: dict, dirs: list, pictures: dict) -> dict:
    """The ground's part of a recording's data (thinkview.html, THE GROUND): {"ground": the recording's heights,
    metal and features with the defs' names, "map": the picture's entry or None, "kinds": {type: its facts},
    "colours": {"rgb": the players' colours a colour index, "from": the archive they are read from, None for
    STOCK_RGB, "why": what kept the recording's own from being read}, "said": what could not be read or
    rendered}. `found` is sample's third answer, `names` {type index: name} of
    the types the data names; the picture, rendered once a map file, is kept in `pictures` by the file's CRC as
    (bytes, entry) for the command to write under the entry's name, `<map>.<crc>.map.webp`."""
    out = {"ground": {}, "map": None, "kinds": {}, "said": [], "colours": {"rgb": STOCK_RGB, "from": None, "why": ""}}
    src = None
    try:
        src = thinkmap.Source((found or {}).get("archives"), dirs)
        rgb, where = thinkmap.team_colours(src)
        out["colours"] = {"rgb": rgb, "from": where, "why": ""}
    except thinkmap.MapError as e:
        out["colours"]["why"] = str(e)
    track = (found or {}).get("track")
    if track is None or track.defs is None:
        out["said"].append("no sample of the recording's feature grid (numpy is needed): no heights, metal or features")
        return out
    try:
        if src is None:
            src = thinkmap.Source(found.get("archives"), dirs)
        tnt, where = thinkmap.tnt_of(src, found.get("map") or "")
        out["said"] += thinkmap.checked(tnt, track)
    except thinkmap.MapError as e:
        out["ground"] = dict(track.ground(), defs={}, named=0)
        out["said"].append(f"the map's picture is not drawn: {e}")
        return out
    table, lost = thinkmap.feature_table(src, tnt.feature_names())
    out["ground"] = dict(track.ground(tnt.sealevel), defs=table, named=len(table))
    if lost:
        out["said"].append("no features/**.tdf holds " + ", ".join(lost[:6]) + (f" and {len(lost) - 6} more" if len(lost) > 6 else ""))
    out["kinds"] = {str(i): k for i, k in thinkmap.unit_kinds(src, names).items()}
    if names and not out["kinds"]:
        out["said"].append("no units/<name>.fbi of the types' names in the recording's archives: extractors are not told apart")
    key = zlib.crc32(tnt.blob)
    file = (re.sub(r"[^A-Za-z0-9._-]+", "-", found.get("map") or "").strip("-") or "map") + f".{key:08x}.map.webp"
    if key not in pictures:
        try:
            blob, _where = src.open("palettes/palette.pal")
            img, n = thinkmap.render(tnt, thinkmap.palette(blob))
            raw, q, (w, h) = thinkmap.encode(img)
            pictures[key] = (raw, {"file": file, "w": w, "h": h, "px": [tnt.mw * thinkmap.TILE_PX, tnt.mh * thinkmap.TILE_PX],
                                   "from": where, "tile": n, "quality": q, "bytes": len(raw),
                                   "archives": src.notes() if src.listed else ["the recording lists no archive: the install's own mount order"]})
        except ImportError:
            pictures[key] = (None, None)
            out["said"].append("the map's picture is not drawn: Pillow and numpy are needed")
        except thinkmap.MapError as e:
            pictures[key] = (None, None)
            out["said"].append(f"the map's picture is not drawn: {e}")
    out["map"] = pictures[key][1]
    return out


def seat_frames(ex, lay, seat, ticks, used: set) -> dict:
    """{tick: {"j": the seat's jobs, flat [slot, kind, build, cx, cz, squad, target, ...] of each row that
    holds one, "k": what it knows of the others, flat [slot, type, how, owner, bx, bz, ...], "held": the held
    256-px cells [gz x IMPAI_GRID_MAX + gx, ...]}} at each of `ticks`, the type indices it names added to `used`."""
    none = lay.macros.get("WQ_NONE", 0xFFFF)
    own = lay.macros.get("IMPAI_OWN", 1)
    out = {t: {"j": [], "k": [], "held": []} for t in ticks}

    def reader(part, want):
        if part not in ex.stride:
            return None, None
        fs = {f.name: f for f in lay.seat_fields(part, seat)}
        return think.Image(ex, part), [fs.get(n) for n in want]

    def val(img, row, f, default=0):
        raw = img.raw(row, f) if f is not None else None
        return f.value(raw) if raw is not None else default
    jimg, jf = reader("job", ("kind", "build", "cx", "cz", "squad", "target"))
    kimg, kf = reader("known", ("type", "how", "owner", "bx", "bz"))
    bimg = think.Image(ex, "brain") if "brain" in ex.stride else None
    held = sorted((f for f in lay.fields("brain") if re.match(r"^held\[\d+\]$", f.name)), key=lambda f: f.off)
    for t in sorted(ticks):
        fr = out[t]
        if jimg is not None and jf[0] is not None:
            jimg.to(t)
            for row in think.seat_rows(ex, "job", seat):
                kind = val(jimg, row, jf[0])
                if kind:
                    build = val(jimg, row, jf[1], none)
                    used.add(build)
                    fr["j"] += [row, kind, build, val(jimg, row, jf[2]), val(jimg, row, jf[3]), val(jimg, row, jf[4]),
                                val(jimg, row, jf[5], none)]
        if kimg is not None and kf[1] is not None:
            kimg.to(t)
            st, hoff = kimg.stride, kf[1].off
            for row in range(min(kimg.rows(), max(ex.slots, 0))):
                how = kimg.img[row * st + hoff]
                if how and not how & own:
                    typ = val(kimg, row, kf[0], none)
                    used.add(typ)
                    fr["k"] += [row, typ, how, val(kimg, row, kf[2], 255), val(kimg, row, kf[3]), val(kimg, row, kf[4])]
        if bimg is not None and held:
            bimg.to(t)
            for i, f in enumerate(held):
                raw = bimg.raw(seat, f)
                if raw and raw[0]:
                    fr["held"] += [i * 8 + b for b in range(8) if raw[0] >> b & 1]
    return out


def interned(table: dict, flat: list, n: int) -> list:
    """`flat`'s rows of `n` values as their indices in `table` ({row: index}, grown here): a builder's job or a
    remembered row stands for many samples, and is written once."""
    return [table.setdefault(tuple(flat[i:i + n]), len(table)) for i in range(0, len(flat), n)]


def held_steps(sets: list) -> list:
    """The held grounds' cell lists as steps, [[cells added, cells removed], ...], each from the one before it and
    the first from none: held ground grows and shrinks a cell or two at a time."""
    out, before = [], set()
    for cells in sets:
        now = set(cells)
        out.append([sorted(now - before), sorted(before - now)])
        before = now
    return out


def plan_value(tree, name: str):
    """A number the plan's defaults give `name` in impai_brain.c's text (`.base_r = 1536`), None when the tree
    names none: the plan is no recorded state, so its text is the one place the figure stands."""
    try:
        text = (Path(tree) / "tagpu" / "ddraw" / "src" / "impai_brain.c").read_text(errors="replace")
    except OSError:
        return None
    m = re.search(rf"\.{re.escape(name)}\s*=\s*(\d+)", text)
    return int(m.group(1)) if m else None


def result_of(rec: Path) -> dict:
    """What the harness wrote beside a recording (bar.py's <name>.json): {"end_tick", "outcome", "winner"
    (a seat, None), "cap"}; {} with none. A harness ends a game by a scenario and clears the level in the last
    ticks, so a game's end is its result's end_tick, not the recording's last."""
    try:
        doc = json.loads(rec.with_suffix(".json").read_text())
        seat = (doc.get("seats") or {}).get(doc.get("outcome")) or {}
        return {"end_tick": int(doc["end_tick"]), "outcome": str(doc.get("outcome")), "winner": seat.get("seat"),
                "cap": doc.get("cap")}
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        return {}


def enum_names(lay, seat) -> dict:
    """{field's name: {value: name}} of the enums the page prints, squads' members under `mil.sq.<member>`."""
    out = {}
    for part in ("mil", "brain", "job"):
        try:
            fields = lay.seat_fields(part, seat)
        except (think.ThinkError, KeyError):
            continue
        for f in fields:
            if f.names is not None and f.names is not lay.unit_names:
                out.setdefault(part + "." + re.sub(r"\[\d+\]", "", f.name), {str(v): n for v, n in f.names.items()})
    return out


def build(ex, lay, seat, sampler, names=(), result=None, step=None, budget=MAX_BYTES, ground=None, layout=None,
          rows=None) -> dict:
    """One recording's data, as the page reads it (thinkview.html, THE DATA). `sampler(ticks)` is sample's
    answer for the recording; `names` think.type_names'; `ground(found, names)` terrain's answer for sample's
    third and the types the data names ({index: name}), None for a page with no ground; `layout` layout_for's
    second answer; `rows` header_rows' ({} or None: who plays beyond the extract's own is unread, and no seat is
    an ally). ViewError when the decision ticks alone pass `budget`."""
    result = result or {}
    end = result.get("end_tick") or ex.end
    end = min(max(int(end), 1), ex.end) if ex.end else 0
    name_of = {i: n for i, n in enumerate(names)}
    trk, missing, have = tracks(ex, lay, seat, PATHS)
    enums = enum_names(lay, seat)
    episodes = think.squad_episodes(ex, lay, seat)
    rows = rows or {}
    known = known_counts(ex, lay, seat, allies_of(rows, seat))
    evs = events(ex, trk, episodes, known, enums, name_of, end)
    decisions = sorted({e[0] for e in evs})
    if step is None:
        step = max(30, -(-end // SAMPLES // 30) * 30)
    if step < 1:
        raise ViewError("--step: at least one tick")
    regular = sorted(set(range(0, end + 1, step)) | {end})
    ticks = sorted(set(regular) | set(decisions))
    dims, world, *found = sampler(ticks)
    used = set()
    seat_side = seat_frames(ex, lay, seat, ticks, used)
    lives = [life + [0, 0, 0, None] for life in unit_lives(ex)]     # + structure, x, z, the tick it is complete
    at_slot, li, frames, helds, jrows, krows = {}, 0, [], {}, {}, {}
    seats = sorted({p["index"] for p in ex.players})
    counts = ex.alive(ticks=ticks)[1]
    for i, t in enumerate(ticks):
        while li < len(lives) and lives[li][2] <= t:
            at_slot[lives[li][0]] = li
            li += 1
        w = world.get(t) or {"units": [], "metal": {}}
        mobile = []
        for slot, x, z, structure, frame in w["units"]:
            k = at_slot.get(slot)
            if k is None or (lives[k][3] is not None and lives[k][3] <= t):
                continue
            life = lives[k]
            used.add(life[1])
            if structure:
                life[4], life[5], life[6] = 1, x, z
            else:
                mobile += [k, x, z]
            if not frame and life[7] is None:
                life[7] = t
        side = seat_side[t]
        cells = tuple(side["held"])
        frames.append({"t": t, "m": mobile, "j": interned(jrows, side["j"], 7), "k": interned(krows, side["k"], 6),
                       "h": helds.setdefault(cells, len(helds)),
                       "a": [counts.get(s, [0] * len(ticks))[i] for s in seats],
                       "i": [w["metal"].get(s, 0) for s in seats] if w["metal"] else []})
    for path in ("brain.way[*].type", "mil.radar_as"):
        for key, tr in trk.items():
            if re.sub(r"\[\d+\]", "[*]", key) == path:
                used.update(tr[1])
    under = ground(found[0] if found else None, {i: name_of[i] for i in sorted(used) if i in name_of}) if ground else {}
    shades = under.get("colours") or {"rgb": STOCK_RGB, "from": None, "why": "no game directory was read"}
    read = found[0] if found and found[0] else {}
    who = seat_facts(ex, rows, shades, world, read.get("first") or {}, dims)
    data = {"v": 2, "ground": under.get("ground") or {}, "picture": under.get("map"), "kinds": under.get("kinds") or {},
            "said": under.get("said") or [], "layout": layout, "name": ex.name, "build": ex.build, "map": ex.map, "seat": seat, "each": ex.each,
            "players": ex.players, "who": who, "colours": {"from": shades["from"], "why": shades["why"]},
            "seats": seats, "end": end, "last": ex.end, "result": result, "stop": ex.stop,
            "px": list(dims), "units": {"block": BLOCK_PX, "cell": CELL_PX, "held": HELD_PX,
                                        "held_row": lay.macros.get("IMPAI_GRID_MAX", 128)},
            "base_r": plan_value(lay.tree, "base_r"),
            "macros": {k: lay.macros[k] for k in ("WQ_NONE", "IMPAI_OWN", "IMPAI_LOS", "IMPAI_SHARED", "IMPAI_RADAR",
                                                  "IMPAI_SONAR", "IMPAI_GHOST", "IMPAI_NO_ANCHOR", "IMPAI_SQUADS",
                                                  "IMPAI_FOE_FRESH") if k in lay.macros},
            "enums": enums, "names": {str(i): name_of[i] for i in sorted(used) if i in name_of},
            "named": bool(name_of), "tracks": trk, "known": {k: v for k, v in known.items() if k != "first"},
            "missing": missing,
            "episodes": [{"k": e["squad"], "task": e["task"], "layer": e["layer"], "from": e["from"], "to": e["to"],
                          "n": list(e["n"]), "value": num(float(e["value"])),
                          "legs": [[t, g[0], g[1], -1 if s is None else s] for t, g, s in e["legs"]]}
                         for e in episodes],
            "lanes": lanes(episodes, end), "judgements": judgement_lanes(trk, enums, end, have),
            "events": evs, "decisions": decisions, "step": step,
            "lives": lives, "helds": held_steps(sorted(helds, key=helds.get)), "jrows": [list(r) for r in jrows],
            "krows": [list(r) for r in krows], "frames": frames}
    keep = set(decisions) | {0, end}
    while True:
        size = len(json.dumps(data, separators=(",", ":")))
        if size <= budget:
            data["bytes"] = size
            return data
        loose = [f for f in data["frames"] if f["t"] not in keep]
        if not loose:
            raise ViewError(f"{ex.name}: its data is {size} bytes at its {len(data['frames'])} decision ticks "
                            f"alone, past --max-bytes {budget}")
        drop = {f["t"] for f in loose[1::2]} or {loose[0]["t"]}
        data["frames"] = [f for f in data["frames"] if f["t"] not in drop]
        data["step"] *= 2


SKELETON = ('<!doctype html>\n<html lang="en">\n<head>\n<meta charset="utf-8">\n'
            '<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">\n</head>\n<body>\n'
            "{}\n</body>\n</html>\n")


def page(manifest: list, bare=False) -> str:
    """index.html: the template with the recordings' list in it, in a document of its own unless `bare`. The
    list is a row a GAME: [{"id", "name", "map", "end", "who": seat_facts' rows, "seats": [{"seat", "id", "file"}
    of each seat a data file was written for]}, ...]."""
    try:
        text = TEMPLATE.read_text()
    except OSError as e:
        raise ViewError(f"no {TEMPLATE.name} beside thinkview.py: {e.strerror}") from None
    mark = "/*MANIFEST*/[]"
    if text.count(mark) != 1:
        raise ViewError(f"{TEMPLATE.name} has no one {mark}")
    text = text.replace(mark, json.dumps(manifest, separators=(",", ":")).replace("</", "<\\/"))
    return text if bare else SKELETON.format(text)


def data_file(ident: str, data: dict) -> str:
    """A recording's data file: one call the page's loader answers."""
    return f"thinkviewData({json.dumps(ident)},{json.dumps(data, separators=(',', ':'))});\n"


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("rec", type=Path, nargs="+")
    p.add_argument("--seat", type=int, default=None, help="the seat (default: each of a recording's ImpAI seats)")
    p.add_argument("--gamedir", action="append", default=[], metavar="DIR",
                   help="a game directory the map file is looked for in (repeatable; default: the recording's own "
                        "and the install)")
    p.add_argument("--content", action="append", default=[], metavar="FILE",
                   help="a content file that names a mod's unit types (repeatable; the first whose count fits)")
    p.add_argument("--tree", action="append", default=[], metavar="DIR",
                   help="a source tree whose layout a recording may be read by (repeatable)")
    p.add_argument("--out", type=Path, required=True, metavar="DIR")
    p.add_argument("--step", type=int, default=None, metavar="N", help="ticks between two regular samples")
    p.add_argument("--max-bytes", type=int, default=MAX_BYTES, metavar="B", help="a data file's bound")
    p.add_argument("--bare", action="store_true", help="the page without the document around it")
    a = p.parse_args(argv)
    try:
        lays = think.layouts(a.tree)
        manifest, files, pictures, stems, builds = [], {}, {}, set(), {}
        dirs = game_dirs(a.rec, a.gamedir)
        for rec in a.rec:
            ex = think.extract(rec)
            lay, layout = layout_for(ex, lays, builds)
            rows = header_rows(rec)
            seats = [think.pick_seat(ex, a.seat)] if a.seat is not None or len(ex.impai_seats()) < 2 else ex.impai_seats()
            names = next((n for n in [think.type_names(ex)] + [think.type_names(ex, c) for c in a.content] if n), [])
            stem = base = re.sub(r"[^A-Za-z0-9._-]", "-", rec.stem)
            n = 1
            while stem in stems:
                n += 1
                stem = f"{base}-{n}"
            stems.add(stem)
            game = {"id": stem, "name": ex.name, "map": ex.map, "seats": []}
            manifest.append(game)
            for seat in seats:
                ident = stem if len(seats) == 1 else f"{stem}-seat{seat}"
                data = build(ex, lay, seat, lambda ticks, rec=rec: sample(rec, ticks), names, result_of(rec),
                             a.step, a.max_bytes,
                             lambda found, used: terrain(found, used, dirs, pictures), layout, rows)
                files[ident] = data_file(ident, data)
                game["seats"].append({"seat": seat, "id": ident, "file": f"{ident}.js"})
                game.update(end=data["end"], who=data["who"])
                m = data["picture"]
                print(f"thinkview: {ex.name}, {ex.seat_label(seat)}: {len(data['frames'])} samples every {data['step']} "
                      f"ticks and at {len(data['decisions'])} decision ticks, {len(data['lanes'])} squads' lanes, "
                      f"{len(data['events'])} events, {len(files[ident])} bytes"
                      + (f"; map {m['w']} x {m['h']} px from {m['from']}, {m['bytes']} bytes" if m else "")
                      + ("" if names else "; no name table fits: types by number")
                      + ("" if layout["exact"] else f"; read by the layout of the tree at {layout['build'] or 'no commit'}, "
                                                    f"not its build {ex.build}")
                      + (f"; not in this layout: {', '.join(data['missing'])}" if data["missing"] else "")
                      + "".join(f"\n  {line}" for line in data["said"]))
        html = page(manifest, a.bare)
        a.out.mkdir(parents=True, exist_ok=True)
        for ident, text in files.items():
            (a.out / f"{ident}.js").write_text(text)
        for raw, entry in pictures.values():
            if raw:
                (a.out / entry["file"]).write_bytes(raw)
        (a.out / "index.html").write_text(html)
        print(f"thinkview: {a.out / 'index.html'}, {len(html)} bytes")
        return 0
    except think.ThinkError as e:
        print(f"thinkview: COULD NOT RUN: {e}")
        return 2
    except OSError as e:
        print(f"thinkview: COULD NOT RUN: {e.filename or ''}: {e.strerror or e}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
