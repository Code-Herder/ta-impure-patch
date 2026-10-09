"""One pass over the whole stream that keeps every live unit's image, for the events and cells
that the unit images could predict:
  - each death's image against the unit's image at the iteration's images (dwords that differ);
  - each shot's start and target point against the shooter's and the target's positions;
  - each tick-input cell whose slot A changed, against the footprints (+0x76, +0x78, +0x7E,
    +0x80, i16 each) of the iteration's images and of the next iteration's.
Writes units_pass.pkl for the codecs."""
import collections
import pickle
import struct

from common import T, OUT, load

UD = T.REC_UNIT_DWORDS
from common import GRID as _GRID
W, H = _GRID
CB = T.REC_CELL_BYTES


def foot(img):
    x, z = struct.unpack_from("<hh", img, 0x76)
    w, h = struct.unpack_from("<hh", img, 0x7E)
    state = struct.unpack_from("<I", img, 0x110)[0]
    return x, z, w, h, state


def pos(img):
    return struct.unpack_from("<iii", img, 0x6A)


def main():
    bl = load()
    units = {}
    tick = None
    bracket = 0
    deaths = []         # (tick, slot, death image, unit image at the images)
    shots = []          # (tick, body, shooter pos, target pos or None)
    tin = []            # (tick, cell, old slotA, new slotA)
    feet = {}           # tick -> {slot: footprint} at that iteration's images
    grid_slot = {}
    cur_feet_tick = None
    for kind, first, last, payload in bl:
        if kind != T.RB_STREAM:
            continue
        for rk, b in T.records(payload):
            if rk == T.RK_TICK:
                if tick is not None:
                    feet[tick] = {s: foot(u) for s, u in units.items()}
                tick = struct.unpack_from("<I", b)[0]
                continue
            if rk == T.RK_UNIT:
                units[struct.unpack_from("<H", b)[0]] = bytearray(b[4:4 + T.REC_UNIT_IMAGE])
            elif rk == T.RK_UNIT_DELTA:
                s = struct.unpack_from("<H", b)[0]
                T.World._delta(units[s], b[2:2 + T.REC_UNIT_BITS], UD, b[T.REC_UNIT_DELTA_HEAD:])
            elif rk == T.RK_UNIT_GONE:
                units.pop(struct.unpack_from("<H", b)[0], None)
            elif rk == T.RK_EV_DEATH:
                bracket += 1
                uid = struct.unpack_from("<I", b)[0]
                u = units.get(uid - 1)
                deaths.append((tick, uid - 1, bytes(b[12:]), bytes(u) if u else None))
            elif rk == T.RK_EV_DEATH_END:
                bracket -= 1
            elif rk == T.RK_EV_FEATURE:
                bracket += 1
            elif rk == T.RK_EV_FEATURE_END:
                bracket -= 1
            elif rk == T.RK_EV_SHOT:
                sh = struct.unpack_from("<H", b, 0x21)[0]
                tg = struct.unpack_from("<H", b, 0x1F)[0]
                su = units.get(sh - 1) if sh else None
                tu = units.get(tg - 1) if tg else None
                shots.append((tick, bytes(b), pos(su) if su else None, pos(tu) if tu else None))
            elif rk == T.RK_CELLS:
                x, z, n, _ = struct.unpack_from("<HHHH", b)
                for k in range(n):
                    cell = z * W + x + k
                    a = struct.unpack_from("<H", b, 8 + k * CB)[0]
                    if bracket:
                        tin.append((tick, cell, grid_slot.get(cell, 0), a))
                    else:
                        grid_slot[cell] = a
    feet[tick] = {s: foot(u) for s, u in units.items()}
    with open(OUT / "units_pass.pkl", "wb") as f:
        pickle.dump({"deaths": deaths, "shots": shots, "tin": tin, "feet": feet}, f, protocol=4)
    print(f"deaths {len(deaths)}, shots {len(shots)}, tick-input cells {len(tin)}, "
          f"iterations {len(feet)}")


if __name__ == "__main__":
    main()
