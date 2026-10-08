#!/usr/bin/env python3
"""ev_split.py -- the primary stream's event records by kind (count, raw bytes), and the stream
deflated per block (zlib 6, 2 KB window, as mp_account) with and without the kinds a mode-3 playing
never dispatches (tagpu_replay.c mp_owned_event: CREATE, START_NAMED, REPLY, SPRAY, STATE, BUILT;
NET_CREATE too, which mp_merge.py already drops).   ev_split.py FILE"""
import collections, struct, sys, zlib
from pathlib import Path
sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parents[3] / "tools/replay"))
import tarec as T
def deflate(d):
    c = zlib.compressobj(6, zlib.DEFLATED, 11); return len(c.compress(d) + c.flush())
def rec(k, b): return struct.pack("<HH", k, len(b)) + b + bytes(-len(b) % 4)
OWNED = {T.RK_EV_CREATE, T.RK_EV_START_NAMED, T.RK_EV_REPLY, T.RK_EV_SPRAY, T.RK_EV_STATE, T.RK_EV_BUILT, T.RK_EV_NET_CREATE}
names = {v: k for k, v in vars(T).items() if k.startswith("RK_") and isinstance(v, int)}
bl, _ = T.blocks(sys.argv[1])
n = collections.Counter(); b = collections.Counter(); a = c = 0
for kind, f, l, p in bl:
    if kind != T.RB_STREAM: continue
    keep = bytearray(); full = bytearray()
    for rk, body in T.records(p):
        r = rec(rk, body); full += r
        if rk in T.EVENTS or rk in OWNED:
            n[names.get(rk, rk)] += 1; b[names.get(rk, rk)] += len(r)
        if rk not in OWNED: keep += r
    a += deflate(bytes(full)); c += deflate(bytes(keep))
print(sys.argv[1])
for k in sorted(b, key=lambda k: -b[k]): print(f"  {k:<22}{n[k]:>7}{b[k]/1024:>9.1f} KiB raw")
print(f"  stream deflated {a/1024:.1f} KiB; without the undispatched kinds {c/1024:.1f} KiB ({(a-c)/1024:.1f} less)")
