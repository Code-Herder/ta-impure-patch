"""Time series of the head order node's fields for a few units: when each re-send happens and
what +0x0A (u16), +0x0C (u16), +0x08 (byte), +0x4E (u32 event bits) and the action/state hold."""
import collections
import pickle
import struct

from common import T, OUT

IMG = T.REC_ORDER_IMAGE


def main():
    keep = pickle.load(open(OUT / "kinds.pkl", "rb"))["keep"]
    series = collections.defaultdict(list)
    for bi, tick, rk, b in keep:
        if rk != T.RK_ORDERS:
            continue
        slot, lst, total, first = struct.unpack_from("<HBxHH", b)
        if first or total != 1 or len(b) != 8 + IMG:
            continue
        n = b[8:]
        series[(slot, lst)].append((tick, n[4], n[5], struct.unpack_from("<I", n, 6)[0],
                                    struct.unpack_from("<H", n, 0xA)[0],
                                    struct.unpack_from("<H", n, 0xC)[0],
                                    struct.unpack_from("<I", n, 0x4E)[0],
                                    struct.unpack_from("<I", n, 0x46)[0], n[0x52]))
    # the longest series
    gaps = collections.Counter()
    for key, s in series.items():
        for a, c in zip(s, s[1:]):
            gaps[c[0] - a[0]] += 1
    print("ticks between two re-sends of the same list:", gaps.most_common(12))
    for key, s in sorted(series.items(), key=lambda kv: -len(kv[1]))[:3]:
        print(f"\nslot {key}: {len(s)} re-sends")
        print("  tick act st flags06 u16_0A u16_0C ev4E stamp46 owned")
        for row in s[:60]:
            print("  " + " ".join(f"{v:#x}" if i in (3, 6) else str(v) for i, v in enumerate(row)))


if __name__ == "__main__":
    main()
