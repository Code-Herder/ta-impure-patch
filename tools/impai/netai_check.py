#!/usr/bin/env python3
"""netai_check -- ImpAI in a network game plays the computer seats its own peer added, and only there.

    tools/impai/netai_check.py --out DIR [--dll PATH] [--port N] [--until T] [--pulse]

One two-player game on Two Continents (twopeers.py: the run's two instances on a private display and a
DirectPlay port leased for the game), under the run lock for two game lanes (tools/runlock.py). The host
launches with `--netai impai` and the joiner with `--netai ota`; in the battle room the host adds a computer seat (PLAYER2 twice: Unused, Blocked, AI) and the joiner one (PLAYER3 once), as
tools/replay/recmp_create_begin_test.py's game does. The game plays to the host's GameTime T (default 6000),
then each peer leaves through its menus, so its teardown writes ImpAI's level-end lines.

What it judges (`judge`), from each peer's log and its ten player records read at T (tacli peek: `[rec]`,
the DirectPlay ID +4, the controller +0x73, the AI object +0x74):
  - each peer's level init logged one network seat line (tagpu_impai.c): the host's names its store's
    `netai=impai` and marks exactly one record, the joiner's names `netai=ota` and marks none, and both
    name level 2, Hard, whatever each peer's Difficulty. `netai` is the battle room's first computer step,
    so the host's AI row is named ImpAI: and the joiner's AI:, and the level marks by that name;
  - the joiner names the host's AI seat among the other peers' ImpAI seats (its name came through
    DirectPlay) and the host names none;
  - the host's marked record is, on the host, seated, controller 2 with its AI object, and on the joiner the
    record with the same DirectPlay ID is controller 3 and is among the joiner's other peers' AI seats;
  - the joiner's own AI seat (controller 2 there) is, on the host, controller 3, unmarked and among the host's
    other peers' AI seats;
  - at the level end the host logged ImpAI turns (`impai: calls`) for its marked seat and for no other, and
    the joiner logged none;
  - with --pulse, the host logged ImpAI's pulse lines for its marked seat and the joiner none
    (`impai: pulse seat=`), else whatever pulse lines there are are reported. The line is the military's,
    written once a game minute for each seat ImpAI plays (tagpu_impai.c, THE PULSE), so a game shorter than
    1800 ticks has none.
Exit 0 when every check passes, 1 when one fails, 2 when the game could not be run.
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

TREE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TREE / "tools" / "replay"))
sys.path.insert(0, str(TREE / "tools"))

GAMES = 2                               # the host and the joiner: the run lock's lanes
UNTIL = 6000
SEATS = re.compile(r"impai: seats (\S+) \(netai=(impai|ota), a network game, level (\d+)\) of (\d+) records; "
                   r"other peers' AI seats (\S+), ImpAI's (\S+)")
LEVEL_HARD = 2                          # impai_seats_level: Hard in every network game
CALLS = re.compile(r"impai: calls seat=(\d+) n=(\d+)")
PULSE = re.compile(r"impai: pulse seat=(\d+)")


def seat_list(text):
    """impai_seats_list's spelling: "none" or "1,3"."""
    return [] if text == "none" else [int(x) for x in text.split(",")]


def seats_lines(text):
    """Every network seat line of the log: (marks, netai, level, live, remote AI seats, remote ImpAI seats)."""
    return [(seat_list(m.group(1)), m.group(2), int(m.group(3)), int(m.group(4)), seat_list(m.group(5)),
             seat_list(m.group(6)))
            for m in SEATS.finditer(text)]


def calls(text):
    """{seat: turns} from the level-end lines."""
    return {int(m.group(1)): int(m.group(2)) for m in CALLS.finditer(text)}


def pulses(text):
    """The seats that logged a pulse line."""
    return sorted({int(m.group(1)) for m in PULSE.finditer(text)})


def judge(host, join, pulse=False):
    """The checks' failures, [] when the game shows ImpAI on its own peer only. `host` and `join` are dicts:
    `log`, the peer's run text; `records`, {i: (active, dpid, control, ai_object)} read at T."""
    bad = []
    lines = {}
    for role, peer, netai in (("host", host, "impai"), ("join", join, "ota")):
        got = seats_lines(peer["log"])
        if len(got) != 1:
            bad.append(f"{role}: {len(got)} network seat lines, not one")
            continue
        lines[role] = got[0]
        if got[0][1] != netai:
            bad.append(f"{role}: its level init read netai={got[0][1]}, not {netai}")
        if got[0][2] != LEVEL_HARD:
            bad.append(f"{role}: ImpAI's level {got[0][2]} in a network game, not Hard")
    if len(lines) != 2:
        return bad
    marks, _, _, _, host_remote, host_rimpai = lines["host"]
    jmarks, _, _, _, join_remote, join_rimpai = lines["join"]
    if host_rimpai:
        bad.append(f"host: other peers' ImpAI seats {host_rimpai}, though the joiner's AI is OTA AI's")
    if jmarks:
        bad.append(f"join: marked {jmarks} with netai=ota")
    if len(marks) != 1:
        bad.append(f"host: marked {marks or 'none'}, not the one computer seat it added")
    else:
        m = marks[0]
        active, dpid, control, aiobj = host["records"][m]
        if not active or control != 2 or not aiobj:
            bad.append(f"host: marked record {m} is active={active} controller={control} ai_object={aiobj}")
        twin = [i for i, r in join["records"].items() if r[0] and r[1] == dpid]
        if len(twin) != 1:
            bad.append(f"join: {len(twin)} records hold the host AI's DirectPlay ID {dpid:#x}")
        else:
            j = twin[0]
            if join["records"][j][2] != 3:
                bad.append(f"join: record {j}, the host's AI seat, is controller {join['records'][j][2]}, not 3")
            if j not in join_remote:
                bad.append(f"join: record {j}, the host's AI seat, is not among its other peers' AI seats "
                           f"{join_remote}")
            if join_rimpai != [j]:
                bad.append(f"join: other peers' ImpAI seats {join_rimpai or 'none'}, not record {j}, the host's "
                           f"ImpAI seat by its name")
    own = [i for i, r in join["records"].items() if r[0] and r[2] == 2]
    if len(own) != 1:
        bad.append(f"join: {len(own)} controller-2 records, not its one computer seat")
    else:
        dpid = join["records"][own[0]][1]
        twin = [i for i, r in host["records"].items() if r[0] and r[1] == dpid]
        if len(twin) != 1:
            bad.append(f"host: {len(twin)} records hold the joiner AI's DirectPlay ID {dpid:#x}")
        else:
            h = twin[0]
            if host["records"][h][2] != 3 or h in marks or h not in host_remote:
                bad.append(f"host: record {h}, the joiner's AI seat, is controller {host['records'][h][2]}, "
                           f"marked={h in marks}, among the other peers' AI seats={h in host_remote}")
    hc, jc = calls(host["log"]), calls(join["log"])
    if len(marks) == 1 and (set(hc) != set(marks) or not hc.get(marks[0])):
        bad.append(f"host: ImpAI's turns {hc}, not for its marked seat {marks[0]} alone")
    if jc:
        bad.append(f"join: ImpAI's turns {jc} with no seat marked")
    hp, jp = pulses(host["log"]), pulses(join["log"])
    if jp:
        bad.append(f"join: pulse lines for seats {jp} with no seat marked")
    if pulse and len(marks) == 1 and hp != marks:
        bad.append(f"host: pulse lines for seats {hp or 'none'}, not its marked seat {marks[0]}")
    return bad


def run(out, dll, port, until, pulse):
    from twopeers import TwoPeers
    with TwoPeers(out, dll, port, "na") as tp:
        tp.launch(host_args=["--netai", "impai"], join_args=["--netai", "ota"])
        tp.battle_room()
        tp.add_ais()
        tp.start()
        tp.play_to(until)
        peers = {name: {"records": {i: (r[0] != 0, r[1], r[2], r[3] != 0) for i, r in tp.records(name).items()}}
                 for name in (tp.host, tp.join)}
        (out / "records.json").write_text(json.dumps({n: {str(i): r for i, r in p["records"].items()}
                                                      for n, p in peers.items()}, indent=1) + "\n")
        for name in (tp.host, tp.join):
            tp.leave(name)
        for name in (tp.host, tp.join):
            peers[name]["log"] = tp.run_log(name)
            for line in peers[name]["log"].splitlines():
                if "impai: seats" in line or "impai: calls" in line or "impai: pulse" in line:
                    print(f"  {name}: {line.strip()}")
        return judge(peers[tp.host], peers[tp.join], pulse)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--dll", type=Path, default=TREE / "tagpu/ddraw/ddraw.dll")
    p.add_argument("--port", type=int, default=None, help="this DirectPlay port (default: the first free lease)")
    p.add_argument("--until", type=int, default=UNTIL, help="the host's GameTime the game plays to")
    p.add_argument("--pulse", action="store_true", help="require the host ImpAI seat's pulse lines, once a game minute")
    a = p.parse_args()
    import runlock
    import tareplay
    runlock.hold(TREE, "netai_check " + " ".join(sys.argv[1:]), GAMES)
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    print(f"evidence: {out}", flush=True)
    try:
        bad = run(out, a.dll.resolve(), a.port, a.until, a.pulse)
    except (OSError, ValueError, subprocess.SubprocessError, tareplay.CouldNotRun) as e:
        print(f"COULD NOT RUN: {e}; inspect {out}", file=sys.stderr)
        return 2
    for b in bad:
        print(f"FAIL: {b}")
    print("PASS: ImpAI marked the host's computer seat on the host alone" if not bad else
          f"{len(bad)} check(s) failed; inspect {out}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
