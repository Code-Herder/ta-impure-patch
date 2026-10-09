"""newlanes.py -- lanes that change in a recording (per class) but never changed in size-battle
(any class), and the ref's counts for the motion lanes. Reads */classstats.json."""
import json, sys
D = sys.argv[1]
ref = json.load(open(f"{D}/ref/classstats.json"))
refl = {}
for c, d in ref["lane_changes"].items():
    for k, v in d.items():
        refl[k] = refl.get(k, 0) + v
print("ref pairs", ref["pairs"])
for n in ("battle", "naval", "air"):
    j = json.load(open(f"{D}/{n}/classstats.json"))
    print(f"== {n}: pairs {j['pairs']}")
    for c, d in j["lane_changes"].items():
        new = {k: v for k, v in d.items() if k not in refl}
        if new:
            print(f"  {c:9s} ({j['pairs'][c]} unit-its) new lanes:",
                  ", ".join(f"{k} {v} ({v/j['pairs'][c]:.1%})" for k, v in sorted(new.items(), key=lambda kv: -kv[1])))
print("ref counts of motion lanes:", {k: v for k, v in refl.items() if any(s in k for s in ("pitch", "roll", "altitude", "vy", "+0x26", "+0x28", "water", "layer"))})
