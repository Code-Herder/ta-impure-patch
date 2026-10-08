"""Extract every block of a recording into blocks.bin + blocks.idx (kind, first, last, raw, packed, offset)."""
import sys, struct, pickle
sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parents[3] / "tools/replay"))
import tarec
src, out = sys.argv[1], sys.argv[2]
sizes = []
bl, stop = tarec.blocks(src, sizes)
print("stop:", stop, "blocks", len(bl))
idx = []
with open(out + ".bin", "wb") as f:
    off = 0
    for (kind, first, last, payload), (_, _, _, raw, packed) in zip(bl, sizes):
        f.write(payload)
        idx.append((kind, first, last, raw, packed, off))
        off += len(payload)
pickle.dump(idx, open(out + ".idx", "wb"))
print("raw total", off)
