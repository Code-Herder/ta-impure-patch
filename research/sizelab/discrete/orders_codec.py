"""Order lists re-encoded: a re-sent list as the nodes that changed, each as a mask of its changed
dwords and their differences from the last image of the same node, by column; a new or resized
list whole. Decoded back and compared with every list the stream sends (lossless check).

Variants: rows (one column, record order) / columns; values as new dwords / as differences."""
import collections
import pickle
import struct

from common import T, OUT, mb, kb
from codec import Cols, varint, read_varint, zz, unzz, wdiff, packed, HOWS

IMG = T.REC_ORDER_IMAGE
ND = IMG // 4


def lists_of(keep):
    """(block, tick, slot, list, image) for every list the stream sends, assembled"""
    out = []
    pend = None
    for bi, tick, rk, b in keep:
        if rk != T.RK_ORDERS:
            continue
        slot, lst, total, first = struct.unpack_from("<HBxHH", b)
        if first == 0:
            pend = bytearray()
        pend += b[8:]
        if first + (len(b) - 8) // IMG >= total:
            out.append((bi, tick, slot, lst, bytes(pend)))
            pend = None
    return out


def encode(sends, columns=True, diffs=True):
    """{block: Cols}"""
    state = {}
    blocks = collections.defaultdict(Cols)
    last_slot = {}
    for bi, tick, slot, lst, img in sends:
        c = blocks[bi]
        col = (lambda k: c[k]) if columns else (lambda k: c["all"])
        varint(col("key"), zz(slot - last_slot.get(bi, 0)))
        last_slot[bi] = slot
        old = state.get((slot, lst))
        n = len(img) // IMG
        if old is not None and len(old) == len(img) and n:
            col("key").append(lst)                      # mode 0: by node
            for i in range(n):
                o = struct.unpack_from(f"<{ND}I", old, i * IMG)
                w = struct.unpack_from(f"<{ND}I", img, i * IMG)
                mask = 0
                for d in range(ND):
                    if o[d] != w[d]:
                        mask |= 1 << d
                varint(col("mask"), mask)
                for d in range(ND):
                    if mask >> d & 1:
                        if diffs:
                            varint(col("val"), zz(wdiff(w[d], o[d])))
                        else:
                            col("val").extend(struct.pack("<I", w[d]))
        else:
            col("key").append(lst | 0x80)               # mode 1: whole
            varint(col("key"), n)
            col("whole").extend(img)
        state[(slot, lst)] = img
    return blocks


def decode(blocks, columns=True, diffs=True):
    """the list images back, in order"""
    state = {}
    out = []
    for bi in sorted(blocks):
        c = blocks[bi]
        if columns:
            bufs = {k: bytes(c[k]) for k in ("key", "mask", "val", "whole")}
            at = dict.fromkeys(bufs, 0)
        else:
            buf = bytes(c["all"])
            bufs = dict.fromkeys(("key", "mask", "val", "whole"), buf)
            shared = [0]
        def rd(k):
            if columns:
                v, at[k] = read_varint(bufs[k], at[k])
            else:
                v, shared[0] = read_varint(buf, shared[0])
            return v
        def rbyte(k):
            if columns:
                v = bufs[k][at[k]]
                at[k] += 1
            else:
                v = buf[shared[0]]
                shared[0] += 1
            return v
        def rbytes(k, n):
            if columns:
                v = bufs[k][at[k]:at[k] + n]
                at[k] += n
            else:
                v = buf[shared[0]:shared[0] + n]
                shared[0] += n
            return v
        slot = 0
        end = len(bufs["key"]) if columns else len(buf)
        while (at["key"] if columns else shared[0]) < end:
            slot += unzz(rd("key"))
            m = rbyte("key")
            lst = m & 0x7F
            if m & 0x80:
                n = rd("key")
                img = bytes(rbytes("whole", n * IMG))
            else:
                old = state[(slot, lst)]
                img = bytearray(old)
                for i in range(len(old) // IMG):
                    mask = rd("mask")
                    for d in range(ND):
                        if mask >> d & 1:
                            if diffs:
                                o = struct.unpack_from("<I", old, i * IMG + 4 * d)[0]
                                v = (o + unzz(rd("val"))) & 0xFFFFFFFF
                            else:
                                v = struct.unpack("<I", rbytes("val", 4))[0]
                            struct.pack_into("<I", img, i * IMG + 4 * d, v)
                img = bytes(img)
            state[(slot, lst)] = img
            out.append((slot, lst, img))
    return out


def baseline(keep):
    parts = collections.defaultdict(bytearray)
    for bi, tick, rk, b in keep:
        if rk == T.RK_ORDERS:
            parts[bi] += struct.pack("<HH", rk, len(b)) + b + bytes(-len(b) % 4)
    return {h: sum(packed(bytes(p), h) for p in parts.values()) for h in HOWS}, \
        sum(len(p) for p in parts.values())


def main():
    keep = pickle.load(open(OUT / "kinds.pkl", "rb"))["keep"]
    sends = lists_of(keep)
    base, braw = baseline(keep)
    print(f"order lists: {len(sends):,} sends; today raw {mb(braw)}, "
          + ", ".join(f"{h} {kb(v)}" for h, v in base.items()))
    want = [(s, l, i) for _, _, s, l, i in sends]
    for columns in (False, True):
        for diffs in (False, True):
            blocks = encode(sends, columns, diffs)
            got = decode(blocks, columns, diffs)
            assert got == want, "not lossless"
            raw = sum(c.raw() for c in blocks.values())
            res = {h: sum(packed(c.blob(), h) for c in blocks.values()) for h in HOWS}
            print(f"  {'columns' if columns else 'rows   '} {'diffs ' if diffs else 'values'}: raw "
                  f"{mb(raw)}, " + ", ".join(f"{h} {kb(v)} ({v / base[h]:.2f}x)" for h, v in
                                             res.items()) + "  [decoded back: equal]")


if __name__ == "__main__":
    main()
