"""Block size against ratio, with today's records: regroup the stream's iterations into blocks of
N iterations (a block also ends at each snapshot, every 900), compress each block alone or chain
the blocks of one snapshot interval into one zstd frame (flushed at each block's end), and with a
dictionary. Also: the snapshot payload as a raw-content dictionary for its chain.

    blocksize.py battle
"""
import sys, time, collections, json
import zstandard
from common import *

name = sys.argv[1] if len(sys.argv) > 1 else "battle"
bl = load(name)
# iterations in order, and the snapshot ticks (a chain ends at each snapshot)
its = []
snaps = []
snap_payload = {}
for k, f, l, p, pk in bl:
    if k == RB_STREAM:
        its += iterations(p)
    elif k == RB_SNAPSHOT:
        snaps.append(l); snap_payload[l] = p
snapset = set(snaps)
raw = sum(len(b) for _, b in its)
print(f"{len(its)} iterations, raw {raw/2**20:.1f} MiB, snapshots at {snaps}")

def groups(N, byte_cap=None):
    """blocks: lists of iteration payloads; a block ends after N iterations, after byte_cap bytes,
    or at a snapshot tick; a chain id changes after each snapshot."""
    out = []; cur = []; cur_n = 0; chain = 0; first_tick = None
    for t, b in its:
        cur.append(b); cur_n += len(b)
        if len(cur) >= N or (byte_cap and cur_n >= byte_cap) or t in snapset:
            out.append((chain, b"".join(cur), t)); cur = []; cur_n = 0
            if t in snapset: chain += 1
    if cur: out.append((chain, b"".join(cur), its[-1][0]))
    return out

def run(label, bs, level, mode, dict_data=None):
    t0 = time.perf_counter()
    tot = 0
    if mode == "alone":
        if dict_data is not None:
            c = zstandard.ZstdCompressor(level=level, dict_data=dict_data)
        else:
            c = zstandard.ZstdCompressor(level=level)
        for ch, b, t in bs:
            tot += len(c.compress(b))
    elif mode in ("chain", "chain+snap"):
        cur = None; obj = None
        c = zstandard.ZstdCompressor(level=level, write_content_size=False)
        last_snap = None
        for ch, b, t in bs:
            if ch != cur:
                if obj is not None: tot += len(obj.flush(zstandard.COMPRESSOBJ_FLUSH_FINISH))
                if mode == "chain+snap" and last_snap is not None:
                    d = zstandard.ZstdCompressionDict(snap_payload[last_snap], dict_type=zstandard.DICT_TYPE_RAWCONTENT)
                    obj = zstandard.ZstdCompressor(level=level, write_content_size=False, dict_data=d).compressobj()
                else:
                    obj = c.compressobj()
                cur = ch
            tot += len(obj.compress(b)) + len(obj.flush(zstandard.COMPRESSOBJ_FLUSH_BLOCK))
            if t in snapset: last_snap = t
        tot += len(obj.flush(zstandard.COMPRESSOBJ_FLUSH_FINISH))
    dt = time.perf_counter() - t0
    print(f"{label:<44} zstd {level:>2} {mode:<10} {len(bs):>6} blocks  {tot/2**20:8.2f} MiB  {raw/2**20/dt:6.1f} MB/s", flush=True)
    return tot

res = {}
today = []
for k, f, l, p, pk in bl:
    if k == RB_STREAM: today.append((0, p, l))
# today's blocks with chain ids by snapshot
ch = 0; today2 = []
for k, f, l, p, pk in bl:
    if k == RB_SNAPSHOT: ch += 1
    if k == RB_STREAM: today2.append((ch, p, l))
for lv in (3, 9):
    res[f"today alone {lv}"] = run("today's blocks (256 KB / 300 it)", today2, lv, "alone")
    res[f"today chain {lv}"] = run("today's blocks (256 KB / 300 it)", today2, lv, "chain")
for N in (1, 3, 10, 30, 100, 300, 900):
    bs = groups(N)
    for lv in (3, 9):
        res[f"N{N} alone {lv}"] = run(f"{N} iterations a block", bs, lv, "alone")
        res[f"N{N} chain {lv}"] = run(f"{N} iterations a block", bs, lv, "chain")
# byte-capped blocks (as the writer does) at other caps
for cap in (64 << 10, 1 << 20, 4 << 20):
    bs = groups(10 ** 9, cap)
    for lv in (3,):
        res[f"cap{cap} alone {lv}"] = run(f"{cap >> 10} KB a block", bs, lv, "alone")
        res[f"cap{cap} chain {lv}"] = run(f"{cap >> 10} KB a block", bs, lv, "chain")
# the snapshot as the chain's prefix dictionary
for lv in (3, 9):
    res[f"today chain+snap {lv}"] = run("today's blocks, snapshot as prefix", today2, lv, "chain+snap")
# trained dictionaries, today's blocks alone
for src in ("tenseat", "battle-first"):
    if src == "tenseat":
        samples = [b for _, b in sum((iterations(p) for k, f, l, p, pk in load("tenseat") if k == RB_STREAM), [])]
    else:
        samples = [b for t, b in its if t <= 900]
    for size in (16 << 10, 112 << 10):
        try:
            d = zstandard.train_dictionary(size, samples, level=3)
        except Exception as e:
            print("train failed", src, size, e); continue
        for lv in (3,):
            res[f"dict {src} {size} alone {lv}"] = run(f"dict {src} {size >> 10} KB, today's blocks", today2, lv, "alone", d)
            bs = groups(1)
            res[f"dict {src} {size} N1 alone {lv}"] = run(f"dict {src} {size >> 10} KB, 1 iteration a block", bs, lv, "alone", d)
json.dump(res, open(f"blocksize-{name}.json", "w"), indent=1)
