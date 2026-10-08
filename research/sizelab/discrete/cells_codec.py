"""Feature-grid cells re-encoded, lossless, decoded back and compared:
  A  the next images' cells that repeat the last tick input, same cell and same bytes, as one
     flag a step (or a bit a cell when not all do) instead of their bytes again;
  B  A, and every cell (images and tick input) as the gap to the last cell of its list, a mask of
     the fields that changed against the grid the reader holds, and the changed fields only --
     slot A as 0 (cleared) or the new slot -- by column.
The first iteration's whole grid is kept as it is (one record set, priced apart).
The cells' order within a list is kept: a reader gets them as the stream had them."""
import collections
import pickle
import struct

from common import T, OUT, mb, kb, rec_bytes
from codec import Cols, varint, read_varint, zz, unzz, packed, HOWS

CB = T.REC_CELL_BYTES
W, H = 642, 648
FIELDS = ((0, 2), (2, 4), (4, 7), (7, 8), (8, 10), (10, 12), (12, 13))


def gather(keep):
    """the stream's cell lists in order: ('first'|'img'|'tin', block, tick, [(cell, bytes)])"""
    out = []
    bracket = 0
    tick = first = None
    cur = None
    for bi, tk, rk, b in keep:
        if rk == T.RK_TICK:
            tick = tk
            if first is None:
                first = tick
            cur = None
            continue
        if rk in (T.RK_EV_FEATURE, T.RK_EV_DEATH):
            bracket += 1
            cur = None
            continue
        if rk in (T.RK_EV_FEATURE_END, T.RK_EV_DEATH_END):
            bracket -= 1
            cur = None
            continue
        if rk != T.RK_CELLS:
            if rk not in (T.RK_EV_DRAW,):
                cur = None if rk != T.RK_CELLS else cur
            continue
        what = "tin" if bracket else ("first" if tick == first else "img")
        if cur is None or cur[0] != what or cur[2] != tick:
            cur = [what, bi, tick, []]
            out.append(cur)
        x, z, n, _ = struct.unpack_from("<HHHH", b)
        for k in range(n):
            cur[3].append((z * W + x + k, bytes(b[8 + k * CB:8 + (k + 1) * CB])))
    return out


def encode(lists, fieldwise):
    grid = bytearray(W * H * CB)
    blocks = collections.defaultdict(Cols)
    last_tin = []
    for what, bi, tick, cells in lists:
        c = blocks[bi]
        if what == "first":
            for cell, v in cells:
                grid[cell * CB:(cell + 1) * CB] = v
            c["first"].extend(b"".join(v for _, v in cells))   # priced apart below
            continue
        c["list"].append({"img": 0, "tin": 1}[what])
        todo = cells
        if what == "img":
            have = dict(cells)
            stand = [have.get(cell) == v for cell, v in last_tin]
            # the stream order of the images' cells is ascending (row-major) and so is the tick
            # input's: the cells that stand are taken out and the reader merges them back
            if all(stand):
                c["list"].append(1)
            else:
                c["list"].append(0)
                bits = bytearray((len(stand) + 7) // 8)
                for i, s in enumerate(stand):
                    if s:
                        bits[i >> 3] |= 1 << (i & 7)
                c["stand"].extend(bits)
            standing = {cell for (cell, v), s in zip(last_tin, stand) if s}
            todo = [(cell, v) for cell, v in cells if cell not in standing]
        varint(c["n"], len(todo))
        prev = -1
        for cell, v in todo:
            varint(c["gap"], cell - prev - 1)
            prev = cell
            old = grid[cell * CB:(cell + 1) * CB] if what == "img" else tin_base(grid, cell)
            if fieldwise:
                mask = 0
                for f, (lo, hi) in enumerate(FIELDS):
                    if old[lo:hi] != v[lo:hi]:
                        mask |= 1 << f
                if mask == 1 and v[0:2] == b"\0\0":
                    c["fmask"].append(0x80)                 # slot A cleared
                else:
                    c["fmask"].append(mask)
                    for f, (lo, hi) in enumerate(FIELDS):
                        if mask >> f & 1:
                            c[f"f{f}"].extend(v[lo:hi])
            else:
                c["cell"].extend(v)
        if what == "img":
            for cell, v in cells:
                grid[cell * CB:(cell + 1) * CB] = v
            last_tin = []
        else:
            last_tin = cells
    return blocks


def tin_base(grid, cell):
    return grid[cell * CB:(cell + 1) * CB]


def decode(blocks, nfirst):
    grid = bytearray(W * H * CB)
    out = []
    last_tin = []
    for bi in sorted(blocks):
        c = blocks[bi]
        B = {k: bytes(v) for k, v in c.c.items()}
        at = collections.Counter()

        def rd(k):
            v, at[k] = read_varint(B[k], at[k])
            return v

        def take(k, n):
            v = B[k][at[k]:at[k] + n]
            at[k] += n
            return v
        if "first" in B:
            fb = B["first"]
            for i in range(len(fb) // CB):
                grid[i * CB:(i + 1) * CB] = fb[i * CB:(i + 1) * CB]
            out.append(("first", len(fb) // CB))
        while at["list"] < len(B.get("list", b"")):
            what = "tin" if take("list", 1)[0] else "img"
            standing = []
            if what == "img":
                if take("list", 1)[0]:
                    standing = list(last_tin)
                else:
                    bits = take("stand", (len(last_tin) + 7) // 8)
                    standing = [cv for i, cv in enumerate(last_tin) if bits[i >> 3] >> (i & 7) & 1]
            cells = []
            prev = -1
            for _ in range(rd("n")):
                cell = prev + rd("gap") + 1
                prev = cell
                old = grid[cell * CB:(cell + 1) * CB]
                if "cell" in B:
                    v = take("cell", CB)
                else:
                    m = take("fmask", 1)[0]
                    v = bytearray(old)
                    if m == 0x80:
                        v[0:2] = b"\0\0"
                    else:
                        for f, (lo, hi) in enumerate(FIELDS):
                            if m >> f & 1:
                                v[lo:hi] = take(f"f{f}", hi - lo)
                    v = bytes(v)
                cells.append((cell, v))
            if what == "img":
                cells = sorted(cells + standing)
                for cell, v in cells:
                    grid[cell * CB:(cell + 1) * CB] = v
                last_tin = []
            else:
                last_tin = cells
            out.append((what, cells))
    return out


def baseline(keep):
    parts = collections.defaultdict(bytearray)
    first = bytearray()
    tick0 = None
    for bi, tk, rk, b in keep:
        if tick0 is None and rk == T.RK_TICK:
            tick0 = tk
        if rk == T.RK_CELLS:
            if tk == tick0:
                first += rec_bytes(rk, b)
            else:
                parts[bi] += rec_bytes(rk, b)
    return ({h: sum(packed(bytes(p), h) for p in parts.values()) for h in HOWS},
            sum(len(p) for p in parts.values()),
            {h: packed(bytes(first), h) for h in HOWS}, len(first))


def main():
    keep = pickle.load(open(OUT / "kinds.pkl", "rb"))["keep"]
    lists = gather(keep)
    base, braw, fbase, fraw = baseline(keep)
    print(f"the first grid: raw {mb(fraw)}, " + ", ".join(f"{h} {kb(v)}" for h, v in fbase.items()))
    print(f"the rest today: raw {mb(braw)}, " + ", ".join(f"{h} {kb(v)}" for h, v in base.items()))
    want = [("first", sum(len(c) for w, _, _, c in lists if w == "first"))] + \
        [(w, sorted(c)) for w, _, _, c in lists if w != "first"]
    for fieldwise in (False, True):
        blocks = encode(lists, fieldwise)
        got = decode(blocks, 0)
        gotn = [got[0]] + [(w, sorted(c)) for w, c in got[1:]]
        assert gotn == want, "not lossless"
        for c in blocks.values():
            c.c.pop("first", None)
        raw = sum(c.raw() for c in blocks.values())
        res = {h: sum(packed(c.blob(), h) for c in blocks.values()) for h in HOWS}
        print(f"  {'B fieldwise' if fieldwise else 'A dedupe   '}: raw {mb(raw)}, " + ", ".join(
            f"{h} {kb(v)} ({v / base[h]:.2f}x)" for h, v in res.items()) + "  [decoded: equal]")


if __name__ == "__main__":
    main()
