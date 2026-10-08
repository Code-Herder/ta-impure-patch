"""The events re-encoded, lossless, decoded back and compared byte for byte.

One kind byte an event keeps their order; each kind's fields go to their own columns, and each
field is predicted from what a reader already holds:
  - every unit ID as the zigzag difference from the last event's unit ID in the block;
  - a value the same unit's last event of the same kind repeats (a spray's box and point, a
    start's arguments, a shot's weapon and target, a feature call's site) as a flag;
  - a script name or a reply's text as its index in the names seen so far (a literal the first
    time), not its bytes;
  - a start's arguments past argc, all zero in this file, as one flag (literal otherwise);
  - a death's unit image as the dwords that differ from the unit's image at the iteration's
    images (the reader holds it), a mask and the dwords;
  - a shot's start and target point as differences from the shooter's and the target's positions
    at the iteration's images (16.16, zigzag varints).
Measured against the events as written, all kinds deflated together per block (recsize's
attribution) and each alone."""
import collections
import pickle
import struct

from common import T, OUT, mb, kb, rec_bytes
from codec import Cols, varint, read_varint, zz, unzz, wdiff, packed, HOWS

EVK = sorted(T.EVENTS)
RESET = True        # every prediction starts afresh at a block in RESETS: a seek resumes there
RESETS = None       # None: every block; else the stream blocks a reader may start at
UD = T.REC_UNIT_DWORDS


class Enc:
    def __init__(self):
        self.last = {}
        self.names = {}
        self.texts = {}

    def uid(self, c, u):
        varint(c["uid"], zz(u - self.last.get("uid", 0)))
        self.last["uid"] = u

    def same(self, c, key, val):
        """flag 1 when val equals the last for key; else flag 0 and the caller writes it"""
        s = self.last.get(key) == val
        c["same"].append(1 if s else 0)
        self.last[key] = val
        return s

    def dictionary(self, c, table, s, col):
        if s in table:
            varint(c[col], table[s] + 1)
        else:
            varint(c[col], 0)
            varint(c[col], len(s))
            c[col + "lit"].extend(s)
            table[s] = len(table)


class Dec:
    def __init__(self, c):
        self.B = {k: bytes(v) for k, v in c.c.items()}
        self.at = collections.Counter()

    def v(self, k):
        x, self.at[k] = read_varint(self.B[k], self.at[k])
        return x

    def b(self, k, n=1):
        x = self.B[k][self.at[k]:self.at[k] + n]
        self.at[k] += n
        return x


class St:
    def __init__(self):
        self.last = {}
        self.names = []
        self.texts = []


def dict_read(d, st_list, col):
    i = d.v(col)
    if i:
        return st_list[i - 1]
    n = d.v(col)
    s = d.b(col + "lit", n)
    st_list.append(s)
    return s


def encode(evs, side):
    """evs: [(block, tick, kind, body)], side: per event the reader's state it may use"""
    blocks = collections.defaultdict(Cols)
    e = Enc()
    names, texts = {}, {}
    cur = None
    for (bi, tick, k, b), sd in zip(evs, side):
        c = blocks[bi]
        if bi != cur:
            cur = bi
            if RESET and (RESETS is None or bi in RESETS):
                e.last.clear()
                names.clear()
                texts.clear()
            else:
                e.last.pop("uid", None)
        c["kind"].append(EVK.index(k))
        if k == T.RK_EV_SHOT:
            sh, tg = struct.unpack_from("<H", b, 0x21)[0], struct.unpack_from("<H", b, 0x1F)[0]
            e.uid(c, sh)
            varint(c["shot"], zz(tg - sh))
            slot = b[0x23]
            c["shot"].append(slot)
            key = ("shot", sh, slot & 15)
            if not e.same(c, key + ("w",), (b[0x19], b[0x1A])):
                c["shot"].extend(b[0x19:0x1B])
            sp, tp = sd
            start = struct.unpack_from("<3i", b, 1)
            tgt = struct.unpack_from("<3i", b, 13)
            for a, p in zip(start, sp or (0, 0, 0)):
                varint(c["spos"], zz(wdiff(a & 0xFFFFFFFF, p & 0xFFFFFFFF)))
            for a, p in zip(tgt, tp or (0, 0, 0)):
                varint(c["tpos"], zz(wdiff(a & 0xFFFFFFFF, p & 0xFFFFFFFF)))
            c["ang"].extend(b[0x1B:0x1F])
            c["shot"].append(b[0])
        elif k == T.RK_EV_START:
            uid, idx, argc = struct.unpack_from("<Iii", b)
            args = struct.unpack_from("<4i", b, 12)
            e.uid(c, uid)
            varint(c["start"], zz(idx))
            varint(c["start"], zz(argc))
            na = argc if 0 <= argc <= 4 else 4
            rest_zero = not any(args[na:])
            c["start"].append(1 if rest_zero else 0)
            key = ("start", uid, idx)
            prev = e.last.get(key)
            for i in range(na if rest_zero else 4):
                varint(c["sargs"], zz(wdiff(args[i] & 0xFFFFFFFF,
                                            (prev[i] if prev else 0) & 0xFFFFFFFF)))
            e.last[key] = args
        elif k == T.RK_EV_START_NAMED:
            uid = struct.unpack_from("<I", b)[0]
            argc, now = b[4], b[5]
            args = struct.unpack_from("<4i", b, 8)
            e.uid(c, uid)
            c["named"].extend(b[4:8])
            e.dictionary(c, names, bytes(b[24:]), "name")
            na = argc if argc <= 4 else 4
            rest_zero = not any(args[na:])
            c["named"].append(1 if rest_zero else 0)
            for i in range(na if rest_zero else 4):
                varint(c["nargs"], zz(args[i]))
        elif k == T.RK_EV_REPLY:
            uid = struct.unpack_from("<I", b)[0]
            e.uid(c, uid)
            varint(c["reply"], struct.unpack_from("<I", b, 4)[0])
            c["reply"].extend(b[8:12])
            if b[9]:
                e.dictionary(c, texts, bytes(b[12:]), "text")
            else:
                varint(c["reply"], len(b) - 12)
                c["reply"].extend(b[12:])
        elif k == T.RK_EV_SPRAY:
            uid = struct.unpack_from("<I", b)[0]
            e.uid(c, uid)
            key = ("spray", uid)
            prev = e.last.get(key)
            body = bytes(b[4:])
            if prev == body:
                c["spray"].append(0)
            elif prev is not None and prev[16:] == body[16:]:
                c["spray"].append(1)                      # the box, the layer, reverse: same
                for i in range(3):
                    varint(c["sprayv"], zz(wdiff(struct.unpack_from("<I", body, 4 + 4 * i)[0],
                                                 struct.unpack_from("<I", prev, 4 + 4 * i)[0])))
                c["spray"].extend(body[0:4])
            else:
                c["spray"].append(2)
                c["sprayraw"].extend(body)
            e.last[key] = body
        elif k == T.RK_EV_DEATH:
            uid, kid = struct.unpack_from("<II", b)
            e.uid(c, uid)
            varint(c["death"], zz(kid - uid))
            c["death"].extend(b[8:12])
            img = b[12:]
            ref = sd if sd is not None else bytes(T.REC_UNIT_IMAGE)
            mask = 0
            for d in range(UD):
                if img[4 * d:4 * d + 4] != ref[4 * d:4 * d + 4]:
                    mask |= 1 << d
            varint(c["dmask"], mask)
            for d in range(UD):
                if mask >> d & 1:
                    c["dval"].extend(img[4 * d:4 * d + 4])
        elif k in (T.RK_EV_FEATURE, T.RK_EV_FEATURE_END):
            c["feat"].append(b[0])
            if k == T.RK_EV_FEATURE:
                c["feat"].append(b[1])
                site = bytes(b[4:8])
                if not e.same(c, ("site", b[0]), site):
                    c["feat"].extend(site)
                c["fargs"].extend(b[8:])
            else:
                varint(c["feat"], struct.unpack_from("<I", b, 4)[0])
        elif k in (T.RK_EV_DEATH_END,):
            pass                                          # closes the open death: its ID is known
        else:
            varint(c["other"], len(b))
            c["other"].extend(b)
    return blocks


def decode(blocks, side):
    out = []
    st = St()
    si = 0
    deaths = []
    for bi in sorted(blocks):
        if RESET and (RESETS is None or bi in RESETS):
            st = St()
        d = Dec(blocks[bi])
        last_uid = 0

        def uid():
            nonlocal last_uid
            last_uid += unzz(d.v("uid"))
            return last_uid

        def same(key, val_reader):
            s = d.b("same")[0]
            if s:
                return st.last[key]
            v = val_reader()
            st.last[key] = v
            return v
        for kc in d.B.get("kind", b""):
            d.at["kind"] += 1
            k = EVK[kc]
            sd = side[si]
            si += 1
            if k == T.RK_EV_SHOT:
                sh = uid()
                tg = sh + unzz(d.v("shot"))
                slot = d.b("shot")[0]
                key = ("shot", sh, slot & 15)
                w = same(key + ("w",), lambda: tuple(d.b("shot", 2)))
                sp, tp = sd
                start = [(p + unzz(d.v("spos"))) & 0xFFFFFFFF for p in (sp or (0, 0, 0))]
                tgt = [(p + unzz(d.v("tpos"))) & 0xFFFFFFFF for p in (tp or (0, 0, 0))]
                ang = d.b("ang", 4)
                b0 = d.b("shot")[0]
                body = bytes([b0]) + struct.pack("<3I", *start) + struct.pack("<3I", *tgt) + \
                    bytes(w) + ang + struct.pack("<HH", tg, sh) + bytes([slot])
            elif k == T.RK_EV_START:
                u = uid()
                idx = unzz(d.v("start"))
                argc = unzz(d.v("start"))
                rest_zero = d.b("start")[0]
                na = argc if 0 <= argc <= 4 else 4
                key = ("start", u, idx)
                prev = st.last.get(key)
                args = [0, 0, 0, 0]
                for i in range(na if rest_zero else 4):
                    p = (prev[i] if prev else 0) & 0xFFFFFFFF
                    v = (p + unzz(d.v("sargs"))) & 0xFFFFFFFF
                    args[i] = v - (1 << 32) if v >> 31 else v
                st.last[key] = tuple(args)
                body = struct.pack("<Iii4i", u, idx, argc, *args)
            elif k == T.RK_EV_START_NAMED:
                u = uid()
                hdr = d.b("named", 4)
                name = dict_read(d, st.names, "name")
                rest_zero = d.b("named")[0]
                argc = hdr[0]
                na = argc if argc <= 4 else 4
                args = [0, 0, 0, 0]
                for i in range(na if rest_zero else 4):
                    args[i] = unzz(d.v("nargs"))
                body = struct.pack("<I", u) + hdr + struct.pack("<4i", *args) + name
            elif k == T.RK_EV_REPLY:
                u = uid()
                kind = d.v("reply")
                r4 = d.b("reply", 4)
                if r4[1]:
                    text = dict_read(d, st.texts, "text")
                else:
                    text = d.b("reply", d.v("reply"))
                body = struct.pack("<II", u, kind) + r4 + text
            elif k == T.RK_EV_SPRAY:
                u = uid()
                key = ("spray", u)
                m = d.b("spray")[0]
                prev = st.last.get(key)
                if m == 0:
                    rest = prev
                elif m == 1:
                    pt = [(struct.unpack_from("<I", prev, 4 + 4 * i)[0] + unzz(d.v("sprayv")))
                          & 0xFFFFFFFF for i in range(3)]
                    rest = d.b("spray", 4) + struct.pack("<3I", *pt) + prev[16:]
                else:
                    rest = d.b("sprayraw", 44)
                st.last[key] = rest
                body = struct.pack("<I", u) + rest
            elif k == T.RK_EV_DEATH:
                u = uid()
                kid = u + unzz(d.v("death"))
                mid = d.b("death", 4)
                ref = bytearray(sd if sd is not None else bytes(T.REC_UNIT_IMAGE))
                mask = d.v("dmask")
                for dd in range(UD):
                    if mask >> dd & 1:
                        ref[4 * dd:4 * dd + 4] = d.b("dval", 4)
                body = struct.pack("<II", u, kid) + mid + bytes(ref)
                deaths.append(u)
            elif k == T.RK_EV_DEATH_END:
                body = struct.pack("<I", deaths.pop())
            elif k in (T.RK_EV_FEATURE, T.RK_EV_FEATURE_END):
                root = d.b("feat")[0]
                if k == T.RK_EV_FEATURE:
                    phase = d.b("feat")[0]
                    site = same(("site", root), lambda: bytes(d.b("feat", 4)))
                    args = d.b("fargs", T.ROOT_ARGS[root])
                    body = bytes([root, phase, 0, 0]) + site + args
                else:
                    body = bytes([root, 0, 0, 0]) + struct.pack("<I", d.v("feat"))
            else:
                body = d.b("other", d.v("other"))
            out.append((k, bytes(body)))
    return out


def main():
    keep = pickle.load(open(OUT / "kinds.pkl", "rb"))["keep"]
    up = pickle.load(open(OUT / "units_pass.pkl", "rb"))
    evs = [(bi, t, k, b) for bi, t, k, b in keep if k in T.EVENTS]
    dq = iter(up["deaths"])
    sq = iter(up["shots"])
    side = []
    for bi, t, k, b in evs:
        if k == T.RK_EV_DEATH:
            side.append(next(dq)[3])
        elif k == T.RK_EV_SHOT:
            s = next(sq)
            side.append((s[2], s[3]))
        else:
            side.append(None)
    # today: all kinds together per block, and each alone
    together = collections.defaultdict(bytearray)
    alone = collections.defaultdict(lambda: collections.defaultdict(bytearray))
    for bi, t, k, b in evs:
        rb = rec_bytes(k, b)
        together[bi] += rb
        alone[k][bi] += rb
    base = {h: sum(packed(bytes(p), h) for p in together.values()) for h in HOWS}
    print(f"events today: {len(evs):,}, raw {mb(sum(len(p) for p in together.values()))}, "
          + ", ".join(f"{h} {kb(v)}" for h, v in base.items()))
    blocks = encode(evs, side)
    got = decode(blocks, side)
    want = [(k, bytes(b)) for _, _, k, b in evs]
    bad = [i for i, (g, w) in enumerate(zip(got, want)) if g != w]
    assert len(got) == len(want) and not bad, (len(got), len(want), bad[:3],
                                               [(got[i], want[i]) for i in bad[:2]])
    raw = sum(c.raw() for c in blocks.values())
    res = {h: sum(packed(c.blob(), h) for c in blocks.values()) for h in HOWS}
    print(f"re-encoded: raw {mb(raw)}, " + ", ".join(
        f"{h} {kb(v)} ({v / base[h]:.2f}x)" for h, v in res.items()) + "  [decoded: equal]")
    # each kind's share of the re-encoding: its columns alone, per block
    groups = {"shot": ("shot", "spos", "tpos", "ang"), "start": ("start", "sargs"),
              "named": ("named", "name", "namelit", "nargs"),
              "reply": ("reply", "text", "textlit"), "spray": ("spray", "sprayv", "sprayraw"),
              "death": ("death", "dmask", "dval"), "feature": ("feat", "fargs"),
              "kind, uid, same": ("kind", "uid", "same")}
    print("by column group (deflate per block, zstd19 per block):")
    for g, cols in groups.items():
        dsum = zsum = rsum = 0
        for c in blocks.values():
            data = b"".join(bytes(c.c.get(k, b"")) for k in cols)
            rsum += len(data)
            dsum += packed(data, "deflate")
            zsum += packed(data, "zstd19")
        print(f"  {g:<16} raw {kb(rsum):>9}  deflate {kb(dsum):>8}  zstd19 {kb(zsum):>8}")
    print("today, each kind alone (deflate, zstd19):")
    for k, parts in alone.items():
        print(f"  {T.EVENTS[k]:<20} {kb(sum(packed(bytes(p), 'deflate') for p in parts.values())):>8}"
              f" {kb(sum(packed(bytes(p), 'zstd19') for p in parts.values())):>8}")


if __name__ == "__main__":
    main()
