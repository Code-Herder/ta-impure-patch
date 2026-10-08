"""The small kinds, re-encoded the one general way (the last image of the same key, a mask of the
dwords that changed, the changed dwords as values or as differences, by column), lossless and
decoded back:
  wrecks (RK_WRECK, key the record), players (RK_PLAYER_DELTA, key the player), the feature heads
  (RK_FEATURES), the globals (RK_GLOBALS_DELTA), the tables (RK_TABLE_SPAN), the iteration marks.
And two predictions on top, priced apart:
  globals: each changed dword as its difference from its last difference (delta of delta), and
           rngseed as the count of Park-Miller steps (16807 mod 2^31-1, 0x4B6C30) from the last;
  features: the reproduction cursor main+0x14257 one less than the last (0x4240AA), wrapping to
           W*H-1 (0x4240CC), as a flag."""
import collections
import pickle
import struct

from common import T, OUT, FILE, mb, kb, rec_bytes
from codec import Cols, varint, read_varint, zz, unzz, wdiff, packed, HOWS

P = 0x7FFFFFFF
W, H = 642, 648


def steps(a, b, cap=1 << 20):
    s = a
    for n in range(cap + 1):
        if s == b:
            return n
        s = s * 16807 % P
    return None


def step_n(a, n):
    return a * pow(16807, n, P) % P


def wreck_fields(keep):
    """which bytes of a wreck record change between two images of the same record"""
    last = {}
    ctr = collections.Counter()
    for bi, t, k, b in keep:
        if k != T.RK_WRECK:
            continue
        i = struct.unpack_from("<H", b)[0]
        img = b[4:]
        o = last.get(i)
        if o is not None:
            ctr[tuple(j for j in range(0, 48, 2) if o[j:j + 2] != img[j:j + 2])] += 1
        else:
            ctr["new"] += 1
        last[i] = img
    return ctr


def generic(keep, kinds, keyfn, imgfn, diffs):
    """encode, decode and compare one family of records: (block, key, image) by the dword mask"""
    state = {}
    blocks = collections.defaultdict(Cols)
    items = []
    for bi, t, k, b in keep:
        if k not in kinds:
            continue
        key, img = keyfn(k, b), imgfn(k, b)
        items.append((bi, key, img))
        c = blocks[bi]
        old = state.get(key)
        varint(c["key"], key if isinstance(key, int) else 0)
        if old is None or len(old) != len(img):
            c["mode"].append(1)
            varint(c["mode"], len(img))
            c["whole"].extend(img)
        else:
            c["mode"].append(0)
            nd = len(img) // 4
            mask = 0
            for d in range(nd):
                if img[4 * d:4 * d + 4] != old[4 * d:4 * d + 4]:
                    mask |= 1 << d
            varint(c["mask"], mask)
            for d in range(nd):
                if mask >> d & 1:
                    if diffs:
                        varint(c["val"], zz(wdiff(struct.unpack_from("<I", img, 4 * d)[0],
                                                  struct.unpack_from("<I", old, 4 * d)[0])))
                    else:
                        c["val"].extend(img[4 * d:4 * d + 4])
        state[key] = img
    # decode
    st = {}
    got = []
    for bi in sorted(blocks):
        c = blocks[bi]
        B = {k: bytes(v) for k, v in c.c.items()}
        at = collections.Counter()

        def rd(k):
            v, at[k] = read_varint(B[k], at[k])
            return v
        while at["key"] < len(B.get("key", b"")):
            key = rd("key")
            m = B["mode"][at["mode"]]
            at["mode"] += 1
            if m:
                n = rd("mode")
                img = B["whole"][at["whole"]:at["whole"] + n]
                at["whole"] += n
            else:
                old = st[key]
                img = bytearray(old)
                mask = rd("mask")
                for d in range(len(old) // 4):
                    if mask >> d & 1:
                        if diffs:
                            v = (struct.unpack_from("<I", old, 4 * d)[0] + unzz(rd("val"))) \
                                & 0xFFFFFFFF
                            struct.pack_into("<I", img, 4 * d, v)
                        else:
                            img[4 * d:4 * d + 4] = B["val"][at["val"]:at["val"] + 4]
                            at["val"] += 4
                img = bytes(img)
            st[key] = img
            got.append(img)
    return blocks, got == [img for _, _, img in items]


def base_of(keep, kinds):
    parts = collections.defaultdict(bytearray)
    for bi, t, k, b in keep:
        if k in kinds:
            parts[bi] += rec_bytes(k, b)
    return {h: sum(packed(bytes(p), h) for p in parts.values()) for h in HOWS}, \
        sum(len(p) for p in parts.values())


def report(name, keep, kinds, blocks, ok):
    base, braw = base_of(keep, kinds)
    raw = sum(c.raw() for c in blocks.values())
    res = {h: sum(packed(c.blob(), h) for c in blocks.values()) for h in HOWS}
    print(f"{name:<34} today raw {mb(braw)}, " + ", ".join(f"{h} {kb(v)}" for h, v in base.items()))
    print(f"{'':<34} new   raw {mb(raw)}, " + ", ".join(
        f"{h} {kb(v)} ({v / base[h]:.2f}x)" for h, v in res.items())
        + ("  [decoded: equal]" if ok else "  [NOT EQUAL]"))


def globals_predicted(keep):
    """globals deltas: the mask, then each changed dword as delta-of-delta, rngseed as steps"""
    img = None
    dd = {}
    blocks = collections.defaultdict(Cols)
    items = []
    rng = None
    for bi, t, k, b in keep:
        if k == T.RK_GLOBALS:
            img = list(struct.unpack(f"<{len(b) // 4}I", b))
            h = T.header_only(str(FILE))
            names = T.known_names(h)
            rng = [i for i, n in names.items() if n == "rngseed"][0]
            continue
        if k != T.RK_GLOBALS_DELTA:
            continue
        nd = len(img)
        nb = (nd + 7) // 8
        bits = b[:nb]
        vals = b[(nb + 3) & ~3:]
        c = blocks[bi]
        c["mask"].extend(bits)
        kk = 0
        new = list(img)
        for d in range(nd):
            if bits[d >> 3] >> (d & 7) & 1:
                v = struct.unpack_from("<I", vals, 4 * kk)[0]
                kk += 1
                new[d] = v
                if d == rng:
                    n = steps(img[d], v)
                    if n is not None:
                        varint(c["rng"], n + 1)
                        continue
                    varint(c["rng"], 0)
                diff = wdiff(v, img[d])
                varint(c["val"], zz(diff - dd.get(d, 0)))
                dd[d] = diff
        items.append(bytes(b))
        img = new
    # decode
    img2 = None
    got = []
    dd2 = {}
    for bi, t, k, b in keep:
        if k == T.RK_GLOBALS:
            img2 = list(struct.unpack(f"<{len(b) // 4}I", b))
            break
    for bi in sorted(blocks):
        c = blocks[bi]
        B = {k: bytes(v) for k, v in c.c.items()}
        at = collections.Counter()

        def rd(k):
            v, at[k] = read_varint(B[k], at[k])
            return v
        nd = len(img2)
        nb = (nd + 7) // 8
        while at["mask"] < len(B.get("mask", b"")):
            bits = B["mask"][at["mask"]:at["mask"] + nb]
            at["mask"] += nb
            vals = bytearray()
            for d in range(nd):
                if bits[d >> 3] >> (d & 7) & 1:
                    if d == rng:
                        n = rd("rng")
                        if n:
                            v = step_n(img2[d], n - 1)
                            vals += struct.pack("<I", v)
                            img2[d] = v
                            continue
                    diff = unzz(rd("val")) + dd2.get(d, 0)
                    dd2[d] = diff
                    v = (img2[d] + diff) & 0xFFFFFFFF
                    vals += struct.pack("<I", v)
                    img2[d] = v
            rec = bytes(bits) + bytes(-nb % 4) + bytes(vals)
            got.append(rec)
    return blocks, got == items


def features_predicted(keep):
    """RK_FEATURES: the three heads as values when they change, the cursor as a flag when it is
    the last one less one (wrapping), else its value"""
    blocks = collections.defaultdict(Cols)
    last = None
    items = []
    for bi, t, k, b in keep:
        if k != T.RK_FEATURES:
            continue
        v = struct.unpack("<iiiI", b)
        items.append(v)
        c = blocks[bi]
        if last is None:
            c["f"].append(3)
            c["v"].extend(b)
        else:
            pred = last[3] - 1 if last[3] > 0 else W * H - 1
            heads_same = v[:3] == last[:3]
            cur_ok = v[3] == pred
            c["f"].append((0 if heads_same else 1) | (0 if cur_ok else 2))
            if not heads_same:
                c["v"].extend(struct.pack("<iii", *v[:3]))
            if not cur_ok:
                c["v"].extend(struct.pack("<I", v[3]))
        last = v
    got = []
    last = None
    for bi in sorted(blocks):
        c = blocks[bi]
        f, vv = bytes(c["f"]), bytes(c["v"])
        at = 0
        for fl in f:
            if last is None:
                v = struct.unpack_from("<iiiI", vv, at)
                at += 16
            else:
                heads = last[:3]
                cur = last[3] - 1 if last[3] > 0 else W * H - 1
                if fl & 1:
                    heads = struct.unpack_from("<iii", vv, at)
                    at += 12
                if fl & 2:
                    cur = struct.unpack_from("<I", vv, at)[0]
                    at += 4
                v = tuple(heads) + (cur,)
            got.append(v)
            last = v
    return blocks, got == items


def main():
    keep = pickle.load(open(OUT / "kinds.pkl", "rb"))["keep"]
    print("wreck records, which u16s changed between two images of a record:")
    for f, n in wreck_fields(keep).most_common(10):
        print(f"   {[hex(x) for x in f] if f != 'new' else f}: {n:,}")
    fams = {
        "wrecks": ({T.RK_WRECK}, lambda k, b: struct.unpack_from("<H", b)[0], lambda k, b: bytes(b[4:])),
        "players": ({T.RK_PLAYER, T.RK_PLAYER_DELTA}, lambda k, b: struct.unpack_from("<H", b)[0],
                    None),
    }
    for name, (kinds, keyfn, imgfn) in fams.items():
        if imgfn is None:
            continue
        for diffs in (False, True):
            blocks, ok = generic(keep, kinds, keyfn, imgfn, diffs)
            report(f"{name} ({'diffs' if diffs else 'values'})", keep, kinds, blocks, ok)
    # players: rebuild whole images from the deltas, then the generic way
    pl = {}
    pkeep = []
    for bi, t, k, b in keep:
        if k == T.RK_PLAYER:
            i = struct.unpack_from("<H", b)[0]
            pl[i] = bytearray(b[4:4 + T.REC_PLAYER_IMAGE])
            pkeep.append((bi, t, k, bytes(pl[i])))
        elif k == T.RK_PLAYER_DELTA:
            i = struct.unpack_from("<H", b)[0]
            T.World._delta(pl[i], b[2:2 + T.REC_PLAYER_BITS], T.REC_PLAYER_DWORDS,
                           b[T.REC_PLAYER_DELTA_HEAD:])
            pkeep.append((bi, t, k, bytes(pl[i])))
    keymap = {}
    for diffs in (False, True):
        ki = iter(range(10 ** 9))
        idx = [i for i in range(len(pkeep))]
        blocks, ok = generic([(bi, t, k, b) for bi, t, k, b in pkeep], {T.RK_PLAYER, T.RK_PLAYER_DELTA},
                             lambda k, b, it=iter(keep): None, lambda k, b: b, diffs) \
            if False else (None, None)
    # the player key is in the original record; pair it up
    pk = [(bi, t, k, struct.unpack_from("<H", ob)[0], img)
          for (bi, t, k, ob), (_, _, _, img) in zip([x for x in keep if x[2] in
                                                     (T.RK_PLAYER, T.RK_PLAYER_DELTA)], pkeep)]
    for diffs in (False, True):
        it = iter(pk)
        recs = [(bi, t, k, (p, img)) for bi, t, k, p, img in pk]
        blocks, ok = generic(recs, {T.RK_PLAYER, T.RK_PLAYER_DELTA}, lambda k, b: b[0],
                             lambda k, b: b[1], diffs)
        report(f"players ({'diffs' if diffs else 'values'})", keep,
               {T.RK_PLAYER, T.RK_PLAYER_DELTA}, blocks, ok)
    gb, ok = globals_predicted(keep)
    report("globals (dod + rng steps)", keep, {T.RK_GLOBALS, T.RK_GLOBALS_DELTA}, gb, ok)
    fb, ok = features_predicted(keep)
    report("feature heads (cursor predicted)", keep, {T.RK_FEATURES}, fb, ok)


if __name__ == "__main__":
    main()
