"""What changes inside an order list that is re-sent: by byte of the 0x58-byte node image, how
often it differs from the last copy of the same node, and the change's size; and the node's
type (+0x04) for the changed nodes."""
import collections
import pickle
import struct

from common import T, OUT

IMG = T.REC_ORDER_IMAGE


def main():
    keep = pickle.load(open(OUT / "kinds.pkl", "rb"))["keep"]
    lists = {}
    pend = None
    byte_changes = collections.Counter()
    dword_changes = collections.Counter()
    dword_diffs = collections.defaultdict(collections.Counter)
    pattern = collections.Counter()      # which dwords changed together, per changed node
    node_index = collections.Counter()   # position of the changed node in its list
    list_len = collections.Counter()
    types = collections.Counter()
    sent = changed_one = 0
    for bi, tick, rk, b in keep:
        if rk != T.RK_ORDERS:
            continue
        slot, lst, total, first = struct.unpack_from("<HBxHH", b)
        key = (slot, lst)
        if first == 0:
            pend = bytearray()
            sent += 1
        pend += b[8:]
        if first + (len(b) - 8) // IMG < total:
            continue
        new = bytes(pend)
        old = lists.get(key)
        lists[key] = new
        if old is None or len(old) != len(new):
            continue
        ch = [i for i in range(0, len(new), IMG) if old[i:i + IMG] != new[i:i + IMG]]
        list_len[len(new) // IMG] += 1
        if len(ch) == 1:
            changed_one += 1
        for i in ch:
            node_index[i // IMG] += 1
            o, nw = old[i:i + IMG], new[i:i + IMG]
            types[struct.unpack_from("<H", nw, 4)[0]] += 1
            ds = []
            for d in range(IMG // 4):
                a = struct.unpack_from("<I", o, 4 * d)[0]
                c = struct.unpack_from("<I", nw, 4 * d)[0]
                if a != c:
                    dword_changes[d] += 1
                    ds.append(d)
                    diff = (c - a + 0x80000000) % 0x100000000 - 0x80000000
                    dword_diffs[d][diff] += 1
            pattern[tuple(ds)] += 1
            for k in range(IMG):
                if o[k] != nw[k]:
                    byte_changes[k] += 1
    print(f"lists sent {sent}, same length with one node changed {changed_one}")
    print("list lengths of same-length re-sends:", list_len.most_common(10))
    print("changed node position:", node_index.most_common(8))
    print("changed node type (+0x04 u16):", types.most_common(12))
    print("\ndword changes (offset: count, top diffs):")
    for d, c in sorted(dword_changes.items(), key=lambda kv: -kv[1]):
        print(f"  +0x{4 * d:02X}: {c:>8}  {dword_diffs[d].most_common(6)}")
    print("\nbyte changes:", [(f"+0x{k:02X}", c) for k, c in byte_changes.most_common(24)])
    print("\npatterns (dwords changed together):")
    for p, c in pattern.most_common(12):
        print(f"  {[hex(4 * d) for d in p]}: {c}")


if __name__ == "__main__":
    main()
