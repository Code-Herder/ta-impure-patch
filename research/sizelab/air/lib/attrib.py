"""attrib.py <run dir>... -- split each run's deflated unit bytes between aircraft and the rest:
a lane's column (deflated alone per block) by its varint bytes per class, the framing (gaps, mask
dictionary, mask ids) by the listed unit-iterations per class.  Per-column deflate sums to a
little more than the writer's whole-section deflate; the shares are what is reported."""
import json, sys, os
MiB = 1 << 20
for d in sys.argv[1:]:
    tag = os.path.basename(d.rstrip("/"))
    L = json.load(open(os.path.join(d, f"{tag}.lanes.json")))
    r = json.loads(open(os.path.join(d, "run.out")).read().strip().splitlines()[-1])
    air = oth = 0.0
    top = []
    for x in L["lanes"]:
        a, o = x["air_varint_KiB"], x["other_varint_KiB"]
        k = x["deflated_KiB"] * 1024
        fa = a / (a + o) if a + o else 0
        air += k * fa
        oth += k * (1 - fa)
        top.append((k * fa, x["lane"]))
    fr = sum(L["framing"].values())
    fa = r["air_units"] / max(r["units"], 1)
    air_f, oth_f = air + fr * fa, oth + fr * (1 - fa)
    tot = air_f + oth_f
    w = r["writer"]
    top.sort(reverse=True)
    print(f"{tag:14s} writer {w / MiB:.3f}  columns+framing {tot / MiB:.3f}  aircraft {air_f / MiB:.3f} "
          f"({air_f / tot:.0%}; lanes {air / MiB:.3f} + framing {fr * fa / MiB:.3f})  rest {oth_f / MiB:.3f}  "
          f"=> aircraft at writer scale {w * air_f / tot / MiB:.3f}")
    print("    top aircraft lanes (MiB):", ", ".join(f"{n} {v / MiB:.3f}" for v, n in top[:8]))
