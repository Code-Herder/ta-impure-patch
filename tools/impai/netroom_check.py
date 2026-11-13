#!/usr/bin/env python3
"""netroom_check -- the battle room's AI button and the ImpAI: name, on both peers, then the level's marks.

    tools/impai/netroom_check.py --out DIR [--dll PATH] [--port N] [--gpu]

One two-player game on Two Continents (twopeers.py: the run's two instances on a private display and a
DirectPlay port leased for the game), under the run lock for two game lanes (tools/runlock.py), both
peers rendering with GDI and taking no GPU token; --gpu renders them on the default renderer and takes
a token a peer (twopeers.py, THE RENDERER). Both peers
launch with `--netai impai`, so a new AI row's first step is ImpAI (tagpu_skirmish.c, THE BATTLE ROOM). In the
battle room, each click followed by the labels read on both peers (`tacli ui`: each PLAYER<n> button's text,
which 0x448C70 sets from its record's long name):
  - the host's row 2: Unused -> Blocked -> ImpAI (`ImpAI:` on the host and, as another peer's seat, on the
    joiner), then, past the engine's one-second guard, -> OTA AI (`AI:` on both);
  - the joiner's row 3: Unused -> ImpAI, -> OTA AI, -> Unused (no AI row of the joiner's on either peer),
    -> ImpAI again.
A label the other peer shows comes through DirectPlay (the add's name at the join, the rename's
SetPlayerName after it), so each read waits for it within LABEL_S and fails by name when it never comes.
Then both start, and each peer's level init must mark what was chosen: the host none (its AI is OTA AI's)
and the joiner's AI as ImpAI's on the joiner, and each must name the joiner's seat among the other peers'
ImpAI seats on the host. Exit 0 when every check passes, 1 when one fails, 2 when the game could not be run.
"""
import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(TREE / "tools"))

GAMES = 2                               # the host and the joiner: the run lock's lanes
GUARD_S = 1.5                           # past the engine's 30 ticks of 0x4B6340 since the add (+8)
LABEL_S = 60                            # the bound on a name reaching the other peer
IMPAI, OTA = "ImpAI:", "AI:"
SEATS = re.compile(r"impai: seats (\S+) \(netai=(impai|ota), a network game, level (\d+)\) of (\d+) records; "
                   r"other peers' AI seats (\S+), ImpAI's (\S+)")


def ai_labels(snap: dict) -> dict:
    """{row: label} of the PLAYER<n> buttons whose text is an AI's name, from a `tacli ui --json` snapshot."""
    out = {}
    for g in snap.get("gadgets") or []:
        m = re.fullmatch(r"PLAYER(\d)", g.get("name") or "")
        text = g.get("text") or ""
        if m and (text.startswith(IMPAI) or text.startswith(OTA)):
            out[int(m.group(1))] = text
    return out


def kind(label: str) -> str:
    return "impai" if label.startswith(IMPAI) else "ota" if label.startswith(OTA) else "none"


def seat_line(text: str):
    """The one network seat line's (marks, remote AI seats, remote ImpAI seats), None unless exactly one."""
    got = SEATS.findall(text)
    if len(got) != 1:
        return None
    marks, _, _, _, remote, rimpai = got[0]
    as_list = (lambda t: [] if t == "none" else [int(x) for x in t.split(",")])
    return as_list(marks), as_list(remote), as_list(rimpai)


def judge_level(host_log: str, join_log: str) -> list:
    """The level's marks after the room's choices: the host's AI OTA AI's, the joiner's ImpAI's."""
    bad = []
    h, j = seat_line(host_log), seat_line(join_log)
    if h is None or j is None:
        return [f"{'host' if h is None else 'join'}: not exactly one network seat line"]
    if h[0]:
        bad.append(f"host: marked {h[0]}, though its AI row was set to OTA AI")
    if len(j[0]) != 1:
        bad.append(f"join: marked {j[0] or 'none'}, not its one AI row set to ImpAI")
    if len(h[2]) != 1 or h[2] != h[1]:
        bad.append(f"host: other peers' ImpAI seats {h[2] or 'none'}, not the joiner's one AI seat {h[1]}")
    if j[2]:
        bad.append(f"join: other peers' ImpAI seats {j[2]}, though the host's AI is OTA AI's")
    return bad


class Room:
    def __init__(self, tp):
        self.tp, self.bad = tp, []

    def labels(self, name) -> dict:
        return ai_labels(json.loads(self.tp.tacli("ui", name, "--json", timeout=60)))

    def expect(self, what, host_kinds: list, join_kinds: list):
        """Both peers' AI labels, as sorted kinds, equal what is expected within LABEL_S; a miss is named."""
        end, got = time.monotonic() + LABEL_S, None
        while True:
            h, j = self.labels(self.tp.host), self.labels(self.tp.join)
            got = (sorted(kind(t) for t in h.values()), sorted(kind(t) for t in j.values()))
            if got == (sorted(host_kinds), sorted(join_kinds)):
                print(f"  {what}: host {h}, join {j}")
                return h, j
            if time.monotonic() >= end:
                self.bad.append(f"{what}: the AI labels read host {h}, join {j}; wanted host {host_kinds}, "
                                f"join {join_kinds}")
                return h, j
            time.sleep(1)

    def click(self, name, row, guard=False):
        if guard:
            time.sleep(GUARD_S)
        self.tp.click(name, f"PLAYER{row}")


def run(out, dll, port, gpu):
    from twopeers import TwoPeers
    with TwoPeers(out, dll, port, "nr", gpu) as tp:
        tp.launch(host_args=["--netai", "impai"], join_args=["--netai", "impai"])
        tp.battle_room()
        room = Room(tp)
        room.click(tp.host, 2)
        room.click(tp.host, 2)
        room.expect("the host's row 2 added", ["impai"], ["impai"])
        room.click(tp.host, 2, guard=True)
        room.expect("the host's row 2 stepped", ["ota"], ["ota"])
        room.click(tp.join, 3)
        room.expect("the joiner's row 3 added", ["ota", "impai"], ["ota", "impai"])
        room.click(tp.join, 3, guard=True)
        room.expect("the joiner's row 3 stepped", ["ota", "ota"], ["ota", "ota"])
        room.click(tp.join, 3, guard=True)
        room.expect("the joiner's row 3 removed", ["ota"], ["ota"])
        room.click(tp.join, 3)
        room.expect("the joiner's row 3 added again", ["ota", "impai"], ["ota", "impai"])
        tp.start()
        for name in (tp.host, tp.join):
            tp.leave(name)
        logs = {name: tp.run_log(name) for name in (tp.host, tp.join)}
        for name, text in logs.items():
            for line in text.splitlines():
                if "impai: seats" in line or "the battle room's AI row" in line:
                    print(f"  {name}: {line.strip()}")
        return room.bad + judge_level(logs[tp.host], logs[tp.join])


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--dll", type=Path, default=TREE / "tagpu/ddraw/ddraw.dll")
    p.add_argument("--port", type=int, default=None, help="this DirectPlay port (default: the first free lease)")
    p.add_argument("--gpu", action="store_true",
                   help="render both peers on the default renderer and take a GPU token each, not GDI")
    a = p.parse_args()
    import runlock
    from twopeers import CouldNotRun
    runlock.hold(TREE, "netroom_check " + " ".join(sys.argv[1:]), GAMES, gpu=a.gpu)
    why = runlock.together(GAMES, "netroom_check")       # the peers meet in one lobby: both seats at once
    if why:
        print(f"could not run: {why}", file=sys.stderr)
        return 2
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    print(f"evidence: {out}", flush=True)
    try:
        bad = run(out, a.dll.resolve(), a.port, a.gpu)
    except (OSError, ValueError, subprocess.SubprocessError, CouldNotRun) as e:
        print(f"COULD NOT RUN: {e}; inspect {out}", file=sys.stderr)
        return 2
    for b in bad:
        print(f"FAIL: {b}")
    print("PASS: the battle room's AI button and names, and the level's marks" if not bad else
          f"{len(bad)} check(s) failed; inspect {out}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
