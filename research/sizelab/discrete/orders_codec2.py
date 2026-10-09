"""Two more order-list variants, after orders_codec.py's best (dword mask + new values, by column):
  bytes: the changed bytes' offsets as gaps and their new values;
  dod:   the dword mask, and each changed dword as its difference from the node's last change of
         that dword (delta of delta: a stamp that advances by the same step costs zero), zigzag;
  tick:  the dword mask + values, but a u16 at an even offset whose new value is the iteration's
         GameTime is sent as a flag (the +0x0A stamp, a hand rule: measured to price it).
Each decoded back and compared."""
import collections
import pickle
import struct

from common import T, OUT, mb, kb
from codec import Cols, varint, read_varint, zz, unzz, wdiff, packed, HOWS
from orders_codec import lists_of, baseline

IMG = T.REC_ORDER_IMAGE
ND = IMG // 4


def enc_bytes(sends):
    state, blocks, last_slot = {}, collections.defaultdict(Cols), {}
    for bi, tick, slot, lst, img in sends:
        c = blocks[bi]
        varint(c["key"], zz(slot - last_slot.get(bi, 0)))
        last_slot[bi] = slot
        old = state.get((slot, lst))
        if old is not None and len(old) == len(img) and img:
            c["key"].append(lst)
            ch = [k for k in range(len(img)) if img[k] != old[k]]
            varint(c["n"], len(ch))
            p = -1
            for k in ch:
                varint(c["off"], k - p - 1)
                p = k
                c["val"].append(img[k])
        else:
            c["key"].append(lst | 0x80)
            varint(c["key"], len(img) // IMG)
            c["whole"].extend(img)
        state[(slot, lst)] = img
    return blocks


def dec_bytes(blocks):
    state, out = {}, []
    for bi in sorted(blocks):
        c = blocks[bi]
        B = {k: bytes(c[k]) for k in ("key", "n", "off", "val", "whole")}
        at = dict.fromkeys(B, 0)

        def rd(k):
            v, at[k] = read_varint(B[k], at[k])
            return v
        slot = 0
        while at["key"] < len(B["key"]):
            slot += unzz(rd("key"))
            m = B["key"][at["key"]]
            at["key"] += 1
            if m & 0x80:
                n = rd("key")
                img = B["whole"][at["whole"]:at["whole"] + n * IMG]
                at["whole"] += n * IMG
            else:
                img = bytearray(state[(slot, m)])
                p = -1
                for _ in range(rd("n")):
                    p += rd("off") + 1
                    img[p] = B["val"][at["val"]]
                    at["val"] += 1
                img = bytes(img)
            state[(slot, m & 0x7F)] = img
            out.append((slot, m & 0x7F, img))
    return out


def enc_dod(sends, tickrule=False):
    state, step, blocks, last_slot = {}, {}, collections.defaultdict(Cols), {}
    for bi, tick, slot, lst, img in sends:
        c = blocks[bi]
        varint(c["key"], zz(slot - last_slot.get(bi, 0)))
        last_slot[bi] = slot
        old = state.get((slot, lst))
        n = len(img) // IMG
        if old is not None and len(old) == len(img) and n:
            c["key"].append(lst)
            for i in range(n):
                o = struct.unpack_from(f"<{ND}I", old, i * IMG)
                w = struct.unpack_from(f"<{ND}I", img, i * IMG)
                mask = sum(1 << d for d in range(ND) if o[d] != w[d])
                varint(c["mask"], mask)
                for d in range(ND):
                    if not mask >> d & 1:
                        continue
                    if tickrule:
                        # a u16 half equal to the GameTime (low 16 bits) is a flag bit
                        lo, hi = w[d] & 0xFFFF, w[d] >> 16
                        t16 = tick & 0xFFFF
                        f = (lo == t16) | (hi == t16) << 1
                        c["tflag"].append(f)
                        rest = (0 if f & 1 else lo) | (0 if f & 2 else hi) << 16
                        c["val"].extend(struct.pack("<I", rest))
                    else:
                        dd = wdiff(w[d], o[d])
                        k = (slot, lst, i, d)
                        varint(c["val"], zz(dd - step.get(k, 0)))
                        step[k] = dd
        else:
            c["key"].append(lst | 0x80)
            varint(c["key"], n)
            c["whole"].extend(img)
            for i in range(n):
                for d in range(ND):
                    step.pop((slot, lst, i, d), None)
        state[(slot, lst)] = img
    return blocks


def dec_dod(blocks, ticks, tickrule=False):
    """ticks: the GameTime of each send, in order (the reader knows it from RK_TICK)"""
    state, step, out = {}, {}, []
    ti = 0
    for bi in sorted(blocks):
        c = blocks[bi]
        B = {k: bytes(c[k]) for k in ("key", "mask", "val", "whole", "tflag")}
        at = dict.fromkeys(B, 0)

        def rd(k):
            v, at[k] = read_varint(B[k], at[k])
            return v
        slot = 0
        while at["key"] < len(B["key"]):
            tick = ticks[ti]
            ti += 1
            slot += unzz(rd("key"))
            m = B["key"][at["key"]]
            at["key"] += 1
            lst = m & 0x7F
            if m & 0x80:
                n = rd("key")
                img = B["whole"][at["whole"]:at["whole"] + n * IMG]
                at["whole"] += n * IMG
                for i in range(n):
                    for d in range(ND):
                        step.pop((slot, lst, i, d), None)
            else:
                old = state[(slot, lst)]
                img = bytearray(old)
                for i in range(len(old) // IMG):
                    mask = rd("mask")
                    for d in range(ND):
                        if not mask >> d & 1:
                            continue
                        o = struct.unpack_from("<I", old, i * IMG + 4 * d)[0]
                        if tickrule:
                            f = B["tflag"][at["tflag"]]
                            at["tflag"] += 1
                            rest = struct.unpack_from("<I", B["val"], at["val"])[0]
                            at["val"] += 4
                            t16 = tick & 0xFFFF
                            v = (t16 if f & 1 else rest & 0xFFFF) | \
                                (t16 if f & 2 else rest >> 16) << 16
                        else:
                            k = (slot, lst, i, d)
                            dd = unzz(rd("val")) + step.get(k, 0)
                            step[k] = dd
                            v = (o + dd) & 0xFFFFFFFF
                        struct.pack_into("<I", img, i * IMG + 4 * d, v)
                img = bytes(img)
            state[(slot, lst)] = img
            out.append((slot, lst, img))
    return out


def main():
    keep = pickle.load(open(OUT / "kinds.pkl", "rb"))["keep"]
    sends = lists_of(keep)
    base, braw = baseline(keep)
    want = [(s, l, i) for _, _, s, l, i in sends]
    ticks = [t for _, t, _, _, _ in sends]
    print(f"today: raw {mb(braw)}, " + ", ".join(f"{h} {kb(v)}" for h, v in base.items()))
    for name, blocks, got in (
            ("bytes", b := enc_bytes(sends), dec_bytes(b)),
            ("dod", d := enc_dod(sends), dec_dod(d, ticks)),
            ("tick", t := enc_dod(sends, True), dec_dod(t, ticks, True))):
        assert got == want, name
        raw = sum(c.raw() for c in blocks.values())
        res = {h: sum(packed(c.blob(), h) for c in blocks.values()) for h in HOWS}
        print(f"  {name:<6} raw {mb(raw)}, " + ", ".join(
            f"{h} {kb(v)} ({v / base[h]:.2f}x)" for h, v in res.items()) + "  [decoded: equal]")


if __name__ == "__main__":
    main()
