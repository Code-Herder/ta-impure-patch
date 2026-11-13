#!/usr/bin/env python3
"""netprobe -- the live checks of scout batch N-d (network games), each one two-peer game.

    tools/impai/netprobe.py difficulty --out DIR [--dll PATH] [--port N]            N-d1-L
    tools/impai/netprobe.py activation --out DIR [--dll PATH] [--port N]            N-d2-L
    tools/impai/netprobe.py departure  --out DIR [--route kill|menu] [--dll PATH] [--port N]   N-d3-L

Each plays one game of two instances (twopeers.py: Two Continents, the run's tag, a private display and a
leased DirectPlay port) under the run lock for two game lanes (tools/runlock.py), writes what it read to
DIR/probe.json and prints it, and compares it with what the disassembly predicts. It measures; it does not
judge Impure: a reading that differs from the prediction is a finding for the engine map, exit 1. Exit 0
when the reading matches, 2 when the game could not be run.

  difficulty  The joiner's registry Difficulty is 0 and the host's 1 before launch; each peer adds one
              computer seat and the host starts. Read once both are in the level: main+0x37EEE on each peer,
              and the host's registry Difficulty. Predicted: the host 2 (its Start's store 0x448A0B), the
              joiner 0 (no writer on its way in), the host's registry still 1 (the settings save 0x430F00 at
              0x448A00 precedes the store).
  activation  In the battle room the joiner clicks PLAYER3 once; its record 3's `[rec]`, +0x73, +4 and +0x1C
              are read as fast as the peek answers for 5 s. Predicted: +0x73 2 and +4 not -1 at the first
              read (the add-AI path 0x451220), then `[rec]` 0 -> 1 and +0x1C set (the activation 0x450A10 from
              the create-player message 0x454143).
  departure   Each peer adds one computer seat, the game plays to GameTime 1800, then the joiner goes: its
              processes killed (`kill`, DirectPlay's drop of the player) or through its menus (`menu`, its
              0x1B). For 60 s the host reads the joiner's human and AI records (`[rec]`, +0x73, +4, +0x0C,
              +0x22) and which owners still have a live unit (tacli units). Predicted: both records freed --
              `[rec]` 0, +0x73 0, +4 -1, +0x0C 0, +0x22 the reason (1 for `kill`) -- and neither owner with a
              unit, within seconds of the drop (0x453010 -> 0x452CC0, the kill-all 0x486F10).
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(TREE / "tools"))

GAMES = 2                               # the host and the joiner: the run lock's lanes
DIFFICULTY = "*0x511DE8+0x37EEE:4"
WATCH_S = 5                             # activation: how long the record is read
GONE_S = 60                             # departure: how long the host is read after the drop
DEPART_AT = 1800
NO_DPID = 0xFFFFFFFF


def registry_difficulty(gamedir: Path):
    """The instance store's `Total Annihilation\\Difficulty` dword, None when it holds none."""
    import taremote
    data = (gamedir / taremote.STATE_DIR / taremote.STORE).read_bytes()
    got = taremote.RegStore.parse(data).get(taremote.STORE_TA, "Difficulty")
    return None if got is None else int.from_bytes(got[2][:4], "little")


def difficulty(tp):
    tp.launch(registry={"H": ["Difficulty=1"], "J": ["Difficulty=0"]})
    tp.battle_room()
    tp.add_ais()
    tp.start()
    got = {"host": tp.peek(tp.host, DIFFICULTY)[0], "join": tp.peek(tp.join, DIFFICULTY)[0],
           "host_registry": registry_difficulty(tp.gamedir(tp.host))}
    want = {"host": 2, "join": 0, "host_registry": 1}
    return got, want


def activation(tp):
    from twopeers import field
    tp.launch()
    tp.battle_room()
    specs = [field(3, 0), field(3, 0x73, 1), field(3, 4), field(3, 0x1C)]
    tp.click(tp.join, "PLAYER3")
    t0, reads = time.monotonic(), []
    while time.monotonic() - t0 < WATCH_S:
        rec, ctrl, dpid, f1c = tp.peek(tp.join, *specs)
        reads.append({"t": round(time.monotonic() - t0, 2), "rec": rec, "control": ctrl, "dpid": dpid, "0x1C": f1c})
    first, last = reads[0], reads[-1]
    got = {"first_control": first["control"], "first_dpid_set": first["dpid"] != NO_DPID,
           "active_at_end": last["rec"] != 0, "0x1C_set_at_end": last["0x1C"] != 0, "reads": reads}
    want = {"first_control": 2, "first_dpid_set": True, "active_at_end": True, "0x1C_set_at_end": True}
    return got, want


def departure(tp, route):
    tp.launch()
    tp.battle_room()
    tp.add_ais()
    tp.start()
    tp.play_to(DEPART_AT)
    # the joiner's own records, its human (type 1) and its AI (type 2), by DirectPlay ID
    mine = {r[1]: i for i, r in tp.records(tp.join, ((0, 4), (4, 4), (0x73, 1))).items() if r[0] and r[2] in (1, 2)}
    fields = ((0, 4), (4, 4), (0x73, 1), (0x0C, 4), (0x22, 1))
    before = tp.records(tp.host, fields)
    rows = sorted(i for i, r in before.items() if r[0] and r[1] in mine)
    if len(rows) != 2:
        raise RuntimeError(f"the host holds {len(rows)} records with the joiner's DirectPlay IDs, not two")
    if route == "kill":
        subprocess.run([sys.executable, str(tp.cli), "stop", tp.join, "--hard"], stdout=tp.log,
                       stderr=subprocess.STDOUT, check=False, timeout=120)
    else:
        tp.leave(tp.join)
    t0, reads = time.monotonic(), []
    while time.monotonic() - t0 < GONE_S:
        recs = tp.records(tp.host, fields)
        units = json.loads(tp.tacli("units", tp.host, "--json", "--limit", "0", timeout=90))
        reads.append({"t": round(time.monotonic() - t0, 1), "rows": {i: recs[i] for i in rows},
                      "owners_with_units": [o for o in units.get("unit_owners", []) if o in rows]})
        time.sleep(1)
    end = reads[-1]
    reason = 1 if route == "kill" else None
    got = {"rows": rows, "before": {i: before[i] for i in rows},
           "freed": all(r[0] == 0 and r[2] == 0 and r[1] == NO_DPID and r[3] == 0 for r in end["rows"].values()),
           "reason": sorted({r[4] for r in end["rows"].values()}),
           "units_left": end["owners_with_units"],
           "freed_after_s": next((x["t"] for x in reads if all(r[0] == 0 for r in x["rows"].values())), None),
           "reads": reads}
    want = {"freed": True, "units_left": []}
    if reason is not None:
        want["reason"] = [reason]
    return got, want


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("probe", choices=("difficulty", "activation", "departure"))
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--dll", type=Path, default=TREE / "tagpu/ddraw/ddraw.dll")
    p.add_argument("--port", type=int, default=None, help="this DirectPlay port (default: the first free lease)")
    p.add_argument("--route", choices=("kill", "menu"), default="kill", help="departure: how the joiner goes")
    a = p.parse_args()
    import runlock
    from twopeers import TwoPeers, CouldNotRun
    runlock.hold(TREE, "netprobe " + " ".join(sys.argv[1:]), GAMES)
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    print(f"evidence: {out}", flush=True)
    try:
        with TwoPeers(out, a.dll.resolve(), a.port, "np") as tp:
            got, want = (difficulty(tp) if a.probe == "difficulty" else activation(tp) if a.probe == "activation"
                         else departure(tp, a.route))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, CouldNotRun) as e:
        print(f"COULD NOT RUN: {e}; inspect {out}", file=sys.stderr)
        return 2
    (out / "probe.json").write_text(json.dumps({"probe": a.probe, "got": got, "want": want}, indent=1) + "\n")
    differs = {k: (got.get(k), v) for k, v in want.items() if got.get(k) != v}
    for k, v in got.items():
        if k != "reads":
            print(f"  {k}: {v}")
    for k, (g, w) in differs.items():
        print(f"DIFFERS: {k} read {g}, predicted {w}")
    print("AS PREDICTED" if not differs else f"{len(differs)} reading(s) differ from the prediction; see {out}")
    return 1 if differs else 0


if __name__ == "__main__":
    sys.exit(main())
