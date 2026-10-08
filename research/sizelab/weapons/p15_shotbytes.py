"""p15: the shot and interception events' bytes: raw, and deflated alone per stream block with
the writer's settings (an attribution, like recsize.py's categories)."""
import sys, struct, zlib
sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parents[3] / "tools/replay"))
import tarec as T
def writer(b): c = zlib.compressobj(6, zlib.DEFLATED, 11); return len(c.compress(b) + c.flush())
bl, _ = T.blocks(sys.argv[1])
raw = packed = n = 0
for kind, first, last, payload in bl:
    if kind != T.RB_STREAM: continue
    buf = bytearray()
    for rk, b in T.records(payload):
        if rk in (T.RK_EV_SHOT, T.RK_EV_INTERCEPT):
            buf += struct.pack("<HH", rk, len(b)) + b + bytes(-len(b) % 4); n += 1
    raw += len(buf); packed += writer(bytes(buf)) if buf else 0
print(f"shot+intercept events: {n} records, raw {raw/1048576:.3f} MiB, deflated alone per block {packed/1048576:.3f} MiB")
