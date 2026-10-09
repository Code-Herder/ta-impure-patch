import sys
from pathlib import Path
p = Path(sys.argv[1])
s = p.read_text()
s = s.replace('stem = Path(path).parent.name + "_" + Path(path).stem',
              'stem = Path(path).parent.parent.name + "_" + Path(path).parent.name + "_" + Path(path).stem')
old = '    merged_stream = sum(deflate(b"".join(v)) + 32 for v in merged_blocks.values())\n'
new = old + '''    # the merged stream by category, each deflated alone per block (recsize's attribution)
    def category(r):
        rk, n = struct.unpack_from("<HH", r)
        slot = struct.unpack_from("<H", r, 4)[0] if n >= 2 else None
        if rk in (T.RK_UNIT, T.RK_UNIT_DELTA, T.RK_UNIT_GONE):
            return "units, remote (composed)" if slot in remote_slot else "units, local"
        if rk in (T.RK_PLAYER, T.RK_PLAYER_DELTA):
            return "players, remote (composed)" if slot in remote_seat else "players, others"
        if rk == T.RK_ORDERS:
            return "order lists, remote (owners')" if slot in remote_slot else "order lists, local"
        if rk == RK_OWNER_ROOT:
            return "owner roots (lifecycle, starts, replies, sprays)"
        if rk == RK_OWNER_TEXT:
            return "chat"
        if rk in (T.RK_POV, T.RK_CLICK, T.RK_SELECT):
            return "perspective"
        if rk in (T.RK_PROJ_HEAD, T.RK_PROJ, T.RK_PROJ_DELTA):
            return "projectiles"
        if rk == T.RK_CELLS:
            return "cells"
        return "other shared"
    cat_alone, cat_raw = collections.Counter(), collections.Counter()
    for v in merged_blocks.values():
        parts = collections.defaultdict(bytearray)
        for r in v:
            parts[category(r)] += r
        for c, d in parts.items():
            cat_alone[c] += deflate(bytes(d))
            cat_raw[c] += len(d)
'''
assert old in s
s = s.replace(old, new)
old2 = '               local_check=dict(checks), local_diff_dwords=dict(diff_dwords.most_common(12)))'
new2 = '''               local_check=dict(checks), local_diff_dwords=dict(diff_dwords.most_common(12)),
               merged_categories={c: dict(raw=cat_raw[c], alone=cat_alone[c])
                                  for c in sorted(cat_alone, key=lambda c: -cat_alone[c])})'''
assert old2 in s
s = s.replace(old2, new2)
p.write_text(s)
