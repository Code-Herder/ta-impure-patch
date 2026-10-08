"""The feature-grid cells of the stream: the first grid, the images' changed cells and the feature
tick's input cells. Per changed cell, which of its 13 bytes changed against the grid as the
images last left it; how many of the next images' cells repeat the tick input before them (same
cell, same bytes); the records' framing."""
import collections
import pickle
import struct

from common import T, OUT, deflate_writer, zstd19, rec_bytes, mb, kb

CB = T.REC_CELL_BYTES
FIELDS = {"slotA": (0, 2), "slotB": (2, 4), "height": (4, 7), "metal": (7, 8), "def": (8, 10),
          "back": (10, 12), "flags": (12, 13)}


def fields_changed(a, b):
    return tuple(n for n, (lo, hi) in FIELDS.items() if a[lo:hi] != b[lo:hi])


def main():
    keep = pickle.load(open(OUT / "kinds.pkl", "rb"))["keep"]
    W, H = 642, 648
    grid = bytearray(W * H * CB)
    bracket = 0
    first_tick = None
    first_bytes = bytearray()
    st = collections.Counter()
    img_fields = collections.Counter()
    tin_fields = collections.Counter()
    runlen = collections.Counter()
    last_tin = {}           # cell -> bytes, the tick input of the previous step
    cur_tin = {}
    tick = None
    per_block_first = collections.defaultdict(bytearray)
    for bi, tk, rk, b in keep:
        if rk == T.RK_TICK:
            tick = tk
            last_tin, cur_tin = cur_tin, {}
            if first_tick is None:
                first_tick = tick
            continue
        if rk in (T.RK_EV_FEATURE, T.RK_EV_DEATH):
            bracket += 1
            continue
        if rk in (T.RK_EV_FEATURE_END, T.RK_EV_DEATH_END):
            bracket -= 1
            continue
        if rk != T.RK_CELLS:
            continue
        x, z, n, _ = struct.unpack_from("<HHHH", b)
        at = (z * W + x) * CB
        if tick == first_tick and not bracket:
            first_bytes += rec_bytes(rk, b)
            grid[at:at + n * CB] = b[8:]
            st["first: cells"] += n
            continue
        runlen[min(n, 9)] += 1
        for k in range(n):
            cell = z * W + x + k
            new = b[8 + k * CB:8 + (k + 1) * CB]
            old = grid[(cell) * CB:(cell + 1) * CB]
            f = fields_changed(old, new)
            if bracket:
                st["tick input: cells"] += 1
                tin_fields[f] += 1
                cur_tin[cell] = bytes(new)
            else:
                st["images: cells"] += 1
                img_fields[f] += 1
                if cell in last_tin:
                    st["images: cell in the last tick input"] += 1
                    if last_tin[cell] == bytes(new):
                        st["images: same bytes as the last tick input"] += 1
                grid[cell * CB:(cell + 1) * CB] = new
        if bracket:
            st["tick input: records"] += 1
        else:
            st["images: records"] += 1
    print(f"first iteration (GameTime {first_tick}): {st['first: cells']} cells, raw "
          f"{mb(len(first_bytes))}, deflate {kb(deflate_writer(bytes(first_bytes)))}, zstd19 "
          f"{kb(zstd19(bytes(first_bytes)))}")
    for k, v in st.items():
        print(f"  {k}: {v:,}")
    print("run length (cells a record, 9 = 9+):", sorted(runlen.items()))
    print("\nimages' changed cells by the fields that changed:")
    for f, c in img_fields.most_common(14):
        print(f"  {'+'.join(f) or '(none)'}: {c:,}")
    print("\ntick input cells by the fields that differ from the images' grid:")
    for f, c in tin_fields.most_common(14):
        print(f"  {'+'.join(f) or '(none)'}: {c:,}")


if __name__ == "__main__":
    main()
