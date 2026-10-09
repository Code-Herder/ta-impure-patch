import sys
from pathlib import Path
p = Path(sys.argv[1]); s = p.read_text()
old = """            v = [bytearray() for _ in range(4)]
"""
new = """            v = [bytearray() for _ in range(4)]
            kinds = {}
"""
assert old in s; s = s.replace(old, new)
old = """                        v[3] += struct.pack("<IIBBH", app, tick, index[origin], rk, span - 64) + r[80:]
"""
new = """                        v[3] += struct.pack("<IIBBH", app, tick, index[origin], rk, span - 64) + r[80:]
                        kinds.setdefault(rk, bytearray()).extend(struct.pack("<IIBBH", app, tick, index[origin], rk, span - 64) + r[80:])
"""
assert old in s; s = s.replace(old, new)
old = """            for i in range(4):
                tot[i] += deflate(bytes(v[i])) + (32 if v[i] else 0)
"""
new = """            for i in range(4):
                tot[i] += deflate(bytes(v[i])) + (32 if v[i] else 0)
            for k, d in kinds.items():
                alone[k] = alone.get(k, 0) + deflate(bytes(d))
"""
assert old in s; s = s.replace(old, new)
old = """        tot = [0, 0, 0, 0]
"""
new = """        tot = [0, 0, 0, 0]
        alone = {}
"""
assert old in s; s = s.replace(old, new)
old = """              f"V1 {tot[1] / 1024:,.1f}, V2 {tot[2] / 1024:,.1f}, V3 {tot[3] / 1024:,.1f} KiB")
"""
new = """              f"V1 {tot[1] / 1024:,.1f}, V2 {tot[2] / 1024:,.1f}, V3 {tot[3] / 1024:,.1f} KiB; "
              f"V3 kinds deflated alone {sum(alone.values()) / 1024:,.1f} KiB: " +
              ", ".join(f"{k} {v / 1024:,.1f}" for k, v in sorted(alone.items(), key=lambda kv: -kv[1])))
"""
assert old in s; s = s.replace(old, new)
p.write_text(s)
