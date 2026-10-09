import sys
from pathlib import Path
p = Path(sys.argv[1]); s = p.read_text()
R = [
("""L1** (the container and decode-before-write), so that the merged units go through the unit
codec.""", """L1** (the container and decode-before-write), so the merged units get the unit codec."""),
("""No loosening is involved: no exclusion, tolerance or expected-absent row. The comparison's holds
are the ones already approved.""", """No loosening is involved (no exclusion, tolerance or expected-absent row); the comparison keeps
its approved holds."""),
("""Arrival
  bunching defeats the integrator rules; carrying each origin's source tick per iteration would let
  them step on the owner's clock [I].""", """Arrival
  bunching defeats the integrator rules [I]; a per-origin source tick could restore them."""),
("""The alternative is safe by construction. The writer thread already receives both inputs in their
order and validates every packet. It can assemble""", """The alternative is safe by construction. The writer thread already receives both inputs in
order and validates every packet, so it can assemble"""),
]
for a, b in R:
    assert a in s, a[:60]
    s = s.replace(a, b)
p.write_text(s)
