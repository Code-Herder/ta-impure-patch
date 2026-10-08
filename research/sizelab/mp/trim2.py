import sys
from pathlib import Path
p = Path(sys.argv[1]); s = p.read_text()
R = [
("**M** = MEASURED (scripts and outputs in `research/mp/`)", "**M** = MEASURED (scripts and outputs beside this file)"),
("""It costs a lot: every remote unit is stored twice; the recorder's own outgoing packets sit
beside its stream, byte-identical to it; seals""",
"""It costs a lot: every remote unit is stored twice; the recorder's own outgoing packets repeat
its stream (§2); seals"""),
("""**Takeover.** It already runs on the composed world (`tk_bound`, `tk_orphans`), so it needs
nothing that only the packets carry.""",
"""**Takeover** already runs on the composed world (`tk_bound`, `tk_orphans`); nothing it needs is
only in the packets."""),
("""- **The recorder's own packets duplicate its stream exactly (M).**
  - Two-peer: 24,288 of 24,288 local unit images and 153 of 153 player images are equal at their
    tick.
  - Ten-seat: 2,711/2,711 and 206/206.
- **The observed remote copy gives the playback only three things:**
  - the level-start images, until each owner's first image;
  - the comparison's contact bits;
  - research audits.""",
"""- **The recorder's own packets repeat its stream exactly (M):** 24,288/24,288 local unit images
  and 153/153 player images equal at their tick (two-peer); 2,711/2,711 and 206/206 (ten-seat).
  Its own order lists and map were not compared.
- **The observed remote copy gives the playback only** the level-start images (until each
  owner's first image), the comparison's contact bits, and research audits."""),
("""**Disk** basis: the real files against my prototype merged file (`mp_merge.py`: units, players
and lists composed, the map left as recorded; deflated as the writer does). **Category** basis: each category deflated alone per block (the sectioned
container).""",
"""**Disk** basis: the real files against my prototype merged file (`mp_merge.py`: units, players
and lists composed, the map left as recorded; deflated as the writer does). **Category** basis:
each category of the stream and the contributions deflated alone per block (the sectioned
container; snapshots, minimap and header excluded)."""),
("""After the codec, the seals cost more than the units. That belongs to stage 6.""",
"""After the codec the seals outweigh the units: stage 6's matter."""),
("""- **The tools change:** `tarec check`'s transition rules; `recmp_compare.py`, which becomes a plain
  compare with the mode-3 holds; `recmp_file.py`; and the browser, which then reads the footer alone
  (closing that residual).""",
"""- **The tools change:** `tarec check`'s transition rules; `recmp_compare.py` (a plain compare
  with the mode-3 holds); `recmp_file.py`; the browser (it reads the footer alone)."""),
]
for a, b in R:
    assert a in s, a[:60]
    s = s.replace(a, b)
p.write_text(s)
