#!/usr/bin/env python3
"""roster2scenario — one unit dump of a game's `tagpu.log` as a `tacli scenario` file.

The DLL logs every living unit every 5 s (`roster_log`, tagpu_packet_pub.c):

      u001 armcom       own=0 idx=1 world=(1647,4666,146) screen=(...) nano=0.00

type, owner seat, world x, world y, altitude, and the fraction of the build still to do. That
is a situation `tacli scenario load` can spawn: the same units, of the same seats, where they
stood. It is not the game: health, facing, orders, kills, wrecks, resources and what each AI
was doing are not in the log, and a unit in flight is created on the ground under it.

    tools/roster2scenario.py LOG [LOG ...] --map NAME -o scenarios/NAME.json
        --dump N          which dump of the files given, in the order given; -1 (default) the last
        --catalogue FILE  an instance's catalogue.json: a type it does not list is left out, and counted
        --ai ai|impai     who plays seats 1..9 (default ai, the engine's own)
        --human SLOT      the seat the scenario gives the human (default 0, the log's `me=`)
        --list            the dumps found: index, units, units a seat

A dump is one block of the log and never spans two files, so the files need no order to be
read whole; `--dump` counts them in the order given (oldest first is `tagpu.8.log … tagpu.1.log`).
"""
import argparse
import collections
import json
import re
import sys

ROW = re.compile(rb"^  u\d+ (\S+)\s+own=(\d+) idx=(\d+) world=\((-?\d+),(-?\d+),(-?\d+)\) "
                 rb"screen=\(-?\d+,-?\d+\) nano=([\d.]+)")
UNIT_CAP = 1500            # MaxUnitNumberPerPlayer's ceiling under this DLL (tagpu_limits.h)
SEAT_ROOM = UNIT_CAP - 1   # the apply clears the seat's starting commander in the same tick as it
                           # creates, and the cleared unit still holds its slot: 1499 are free
TERR = re.compile(rb"^terr: .* map=(\d+)x(\d+) ")     # the map in 32-unit cells
EDGE = 16                  # a unit in flight can be past the edge; the applier refuses one there


def dumps(paths):
    """Every dump of the files, in order: a list of (type, owner, x, y, altitude, nano) rows.
    And the map's size in world units, from the terrain pass's line, or None."""
    out = []
    size = None
    for path in paths:
        cur = []
        with open(path, "rb") as f:
            for line in f:
                m = ROW.match(line)
                if m:
                    cur.append((m[1].decode("latin-1"), int(m[2]), int(m[4]), int(m[5]), int(m[6]),
                                float(m[7])))
                    continue
                if cur:
                    out.append(cur)
                    cur = []
                if size is None:
                    t = TERR.match(line)
                    if t:
                        size = (int(t[1]) * 32, int(t[2]) * 32)
        if cur:
            out.append(cur)
    return out, size


def side_of(types):
    """arm or core, by what the seat fields most of."""
    arm = sum(n for t, n in types.items() if t.lower().startswith("arm"))
    core = sum(n for t, n in types.items() if t.lower().startswith("cor"))
    return "arm" if arm >= core else "core"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--map")
    ap.add_argument("-o", "--out")
    ap.add_argument("--dump", type=int, default=-1)
    ap.add_argument("--catalogue")
    ap.add_argument("--ai", choices=("ai", "impai"), default="ai")
    ap.add_argument("--human", type=int, default=0)
    ap.add_argument("--res", default="1920x1080")
    ap.add_argument("--map-size", metavar="WxH", help="the map in world units, when the log has no terr: line")
    ap.add_argument("--description")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()

    found, size = dumps(a.logs)
    if a.map_size:
        size = tuple(int(v) for v in a.map_size.lower().split("x"))
    if not found:
        sys.exit("no unit dump in these files (a dump is the `  uNNN type own=...` block)")
    if a.list:
        for i, d in enumerate(found):
            seats = collections.Counter(r[1] for r in d)
            print(f"{i:4d}  {len(d):6d} units  " + " ".join(f"{s}:{n}" for s, n in sorted(seats.items())))
        return
    if not a.map or not a.out:
        sys.exit("--map and -o are required to write a scenario")
    try:
        rows = found[a.dump]
    except IndexError:
        sys.exit(f"--dump {a.dump}: the files hold {len(found)} dumps")

    known = None
    if a.catalogue:
        with open(a.catalogue) as f:
            known = {u["name"].upper() for u in json.load(f)["units"]}

    if size is None:
        sys.exit("the files have no `terr:` line to size the map from: give --map-size WxH (world units)")
    by_seat = collections.defaultdict(collections.Counter)
    dropped = collections.Counter()
    full = collections.Counter()
    moved = 0
    units = []
    for typ, owner, x, y, _alt, nano in rows:
        if known is not None and typ.upper() not in known:
            dropped[typ] += 1
            continue
        if sum(by_seat[owner].values()) >= SEAT_ROOM:
            full[owner] += 1
            continue
        inside = (min(max(x, EDGE), size[0] - EDGE), min(max(y, EDGE), size[1] - EDGE))
        moved += inside != (x, y)
        x, y = inside
        by_seat[owner][typ] += 1
        u = {"type": typ.upper(), "owner": owner, "pos": [x, y]}
        if nano > 0.0:
            # the log has the fraction still to build; the file wants the percentage built
            u["nanoframe"] = min(99, max(1, round((1.0 - nano) * 100)))
        units.append(u)

    if a.human not in by_seat:
        sys.exit(f"--human {a.human}: that seat has no unit in this dump (seats {sorted(by_seat)})")

    players = []
    for seat in range(max(by_seat) + 1):
        if seat not in by_seat:
            players.append({"slot": seat, "controller": "off"})
            continue
        players.append({"slot": seat, "controller": "human" if seat == a.human else a.ai,
                        "side": side_of(by_seat[seat]), "color": seat})

    mine = [u["pos"] for u in units if u["owner"] == a.human]
    centre = [sum(p[0] for p in mine) // len(mine), sum(p[1] for p in mine) // len(mine)]
    doc = {
        "format": "ta-scenario/1",
        "description": a.description or
            f"{len(units)} units of {len(by_seat)} seats on {a.map}, from a game's unit dump",
        "on_error": "abort",
        "setup": {
            "map": a.map, "res": a.res, "unit_limit": UNIT_CAP, "clear_existing": True,
            "switches": {"shootall": False, "noshake": False},
            "players": players,
        },
        "units": units,
        "camera": {"at": centre},
    }
    # one unit a line: 13 000 of them indented four ways is a file nobody can diff
    head = json.dumps({k: v for k, v in doc.items() if k != "units"}, indent=2)
    body = ",\n".join("    " + json.dumps(u, separators=(", ", ": ")) for u in units)
    text = head[:-2].rstrip() + ',\n  "units": [\n' + body + "\n  ]\n}\n"
    json.loads(text)
    with open(a.out, "w") as f:
        f.write(text)

    print(f"{a.out}: {len(units)} units, {len(by_seat)} seats, camera at {centre}")
    for seat in sorted(by_seat):
        p = players[seat]
        print(f"  seat {seat}: {sum(by_seat[seat].values()):5d} units, {p['side']}, {p['controller']}")
    if moved:
        print(f"  moved inside the map's {size[0]}x{size[1]}: {moved} units")
    if full:
        print(f"  left out, the seat's {SEAT_ROOM} slots taken: "
              + ", ".join(f"seat {s} x{n}" for s, n in sorted(full.items())))
    if dropped:
        print(f"  left out, not in the catalogue: {sum(dropped.values())} units of {len(dropped)} types: "
              + ", ".join(f"{t} x{n}" for t, n in dropped.most_common()))


if __name__ == "__main__":
    main()
