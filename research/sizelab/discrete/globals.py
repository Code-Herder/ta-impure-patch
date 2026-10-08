"""The globals deltas: which dwords change each iteration, their values and differences, and how
many Park-Miller steps (16807, 2^31-1: 0x4B6C30, DISASSEMBLED) take each rngseed to the next."""
import collections
import pickle
import struct
import sys

from common import T, OUT, FILE, REPO

sys.path.insert(0, str(REPO / "tools/replay"))

P = 0x7FFFFFFF


def steps(a, b, cap=200000):
    s = a
    for n in range(cap + 1):
        if s == b:
            return n
        s = s * 16807 % P
    return None


def main():
    keep = pickle.load(open(OUT / "kinds.pkl", "rb"))["keep"]
    h = T.header_only(str(FILE))
    names = T.known_names(h)
    img = None
    changed = collections.Counter()
    series = collections.defaultdict(list)
    nsteps = []
    for bi, tick, rk, b in keep:
        if rk == T.RK_GLOBALS:
            img = list(struct.unpack(f"<{len(b) // 4}I", b))
            print(f"globals image: {len(img)} dwords")
            continue
        if rk != T.RK_GLOBALS_DELTA:
            continue
        nd = len(img)
        nbits = (nd + 7) // 8
        bits = b[:nbits]
        vals = b[(nbits + 3) & ~3:]
        k = 0
        for d in range(nd):
            if bits[d >> 3] >> (d & 7) & 1:
                v = struct.unpack_from("<I", vals, 4 * k)[0]
                k += 1
                changed[d] += 1
                nm = names[d] if names and d < len(names) else f"dword {d}"
                if nm == "rngseed":
                    nsteps.append(steps(img[d], v))
                series[nm].append((tick, img[d], v))
                img[d] = v
    for d, c in changed.most_common(20):
        nm = names[d] if names and d < len(names) else f"dword {d}"
        s = series[nm]
        print(f"{nm:<18}{c:>6}  first values: {[(t, v) for t, _, v in s[:6]]}")
    ok = [n for n in nsteps if n is not None]
    print(f"\nrngseed: {len(nsteps)} changes, {len(ok)} found within the cap; steps per iteration "
          f"min {min(ok)}, median {sorted(ok)[len(ok) // 2]}, p99 {sorted(ok)[int(len(ok) * .99)]}, "
          f"max {max(ok)}")
    hist = collections.Counter(n.bit_length() for n in ok)
    print("  bits of the step count:", sorted(hist.items()))
    pf = series.get("pfsearches")
    if pf:
        print("pfsearches series:", [(t, a, v) for t, a, v in pf[:40]])
    pc = series.get("pfcalls")
    if pc:
        print("pfcalls series:", [(t, v) for t, a, v in pc[:20]])


if __name__ == "__main__":
    main()
