import sys
from pathlib import Path
p = Path(sys.argv[1]); s = p.read_text()
R = [
("| TEN_A | TEN_B |", "| 103.3 | 14.3 |"),
("| TEN_C | TEN_D |", "| 126.6 | 16.5 |"),
("| TEN_E | **TEN_F** |", "| 124.8 | **22.7** |"),
("""- **Through the codec, today's two copies come to 168 KiB on the two-peer file; the merged
  world to 92.**
- **Composed remote units cost about 20 % more than their owners' own sequences:** 43.3 against
  35.7 (two-peer), 32.1 against 26.8 (chatfix) [I: the local part subtracted]. Arrival bunching
  defeats the integrator rules.""",
"""- **Through the codec, today's two copies come to 168 (two-peer) and 30.8 (ten-seat); the merged
  world to 92 and 22.7.**
- **Composed remote units cost 20–50 % more than their owners' own sequences:** 43.3 against 35.7
  (two-peer), 32.1/26.8 (chatfix), 18.6/12.4 (ten-seat) [I: the local part subtracted]. Arrival
  bunching defeats the integrator rules; carrying each origin's source tick per iteration would let
  them step on the owner's clock [I]."""),
("| ten-seat | TEN_STACK_TODAY | TEN_STACK_MERGED |", "| ten-seat | ≈ 716 (78 % packets) | ≈ 175–190 |"),
("""**Disk** basis: the real files against my prototype merged file (`mp_merge.py`, deflated as the
writer does).""", """**Disk** basis: the real files against my prototype merged file (`mp_merge.py`: units, players
and lists composed, the map left as recorded; deflated as the writer does)."""),
]
for a, b in R:
    assert a in s, a[:50]
    s = s.replace(a, b)
p.write_text(s)
