import sys
from pathlib import Path
p = Path(sys.argv[1]); s = p.read_text()
old = """            elif k == 6 and o not in local_origins:
                cplayer[origin_seat[o]] = bytearray(a["player"])"""
new = """            elif k == 6 and o not in local_origins:
                cplayer[origin_seat[o]] = bytearray(a["player"])
            elif k == 6:
                local_player[origin_seat[o]] = a["player"]"""
assert old in s; s = s.replace(old, new)
old = "    local_owner = {}\n"
new = "    local_owner = {}\n    local_player = {}\n"
assert old in s; s = s.replace(old, new, 1)
old = "        local_owner.clear()\n"
new = """        local_owner.clear()
        for seat, img in local_player.items():
            p = prim_players.get(seat)
            if p is not None and bytes(p) == img:
                checks["local player: equal"] += 1
            else:
                checks["local player: DIFFERENT"] += 1
                for d in range(T.REC_PLAYER_DWORDS):
                    if p is None or p[4 * d:4 * d + 4] != img[4 * d:4 * d + 4]:
                        diff_dwords[f"player +0x{4 * d:03X}"] += 1
        local_player.clear()
"""
assert old in s; s = s.replace(old, new, 1)
p.write_text(s)
