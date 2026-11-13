#!/usr/bin/env python3
"""netai_check -- ImpAI in a network game plays the computer seats its own peer added, and only there.

    tools/impai/netai_check.py --out DIR [--dll PATH] [--port N] [--until T] [--pulse]

One two-player game on Two Continents (tools/mp_lobby.sh), the run's two instances on a private display and a
DirectPlay port leased for the game (tools/dpport.py), under the run lock for two game lanes
(tools/runlock.py). The host launches with `--netai impai` and the joiner with `--netai ota`; in the battle room
the host adds a computer seat (PLAYER2 twice: Unused, Blocked, AI) and the joiner one (PLAYER3 once), as
tools/replay/recmp_create_begin_test.py's game does. The game plays to the host's GameTime T (default 6000),
then each peer leaves through its menus, so its teardown writes ImpAI's level-end lines.

What it judges (`judge`), from each peer's log and its ten player records read at T (tacli peek: `[rec]`,
the DirectPlay ID +4, the controller +0x73, the AI object +0x74):
  - each peer's level init logged one network seat line (tagpu_impai.c): the host's names its store's
    `netai=impai` and marks exactly one record, the joiner's names `netai=ota` and marks none;
  - the host's marked record is, on the host, seated, controller 2 with its AI object, and on the joiner the
    record with the same DirectPlay ID is controller 3 and is among the joiner's other peers' AI seats;
  - the joiner's own AI seat (controller 2 there) is, on the host, controller 3, unmarked and among the host's
    other peers' AI seats;
  - at the level end the host logged ImpAI turns (`impai: calls`) for its marked seat and for no other, and
    the joiner logged none;
  - with --pulse, the host logged ImpAI's pulse lines for its marked seat and the joiner none
    (`impai: pulse seat=`), else whatever pulse lines there are are reported.
Exit 0 when every check passes, 1 when one fails, 2 when the game could not be run.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

TREE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TREE / "tools" / "replay"))
sys.path.insert(0, str(TREE / "tools"))

GAMES = 2                               # the host and the joiner: the run lock's lanes
MAP = "Two Continents"
UNTIL = 6000
STALL_S = 120                           # GameTime standing still this long gives the game up
REC, STRIDE = 0x1B63, 0x14B             # main's player records (tagpu_engine.h)
RECORDS = 10
SEATS = re.compile(r"impai: seats (\S+) \(netai=(impai|ota), a network game\) of (\d+) records; "
                   r"other peers' AI seats (\S+)")
CALLS = re.compile(r"impai: calls seat=(\d+) n=(\d+)")
PULSE = re.compile(r"impai: pulse seat=(\d+)")


def seat_list(text):
    """impai_seats_list's spelling: "none" or "1,3"."""
    return [] if text == "none" else [int(x) for x in text.split(",")]


def seats_lines(text):
    """Every network seat line of the log: (marks, netai, live, remote AI seats)."""
    return [(seat_list(m.group(1)), m.group(2), int(m.group(3)), seat_list(m.group(4)))
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
    if len(lines) != 2:
        return bad
    marks, _, _, host_remote = lines["host"]
    jmarks, _, _, join_remote = lines["join"]
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
    import dpport
    import runlock
    import talog
    import tareplay

    root = tareplay.main_checkout()
    instances, cli = root / "tagpu/instances", TREE / "tools/tacli"
    owned, display = [], None
    log = (out / "driver.log").open("w")

    def command(*args, timeout=180):
        r = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=timeout)
        log.write(r.stdout + r.stderr)
        log.flush()
        if r.returncode:
            raise tareplay.CouldNotRun(f"command failed ({r.returncode}): {args}; see driver.log")
        return r.stdout

    def peek(name, *specs):
        o = command(sys.executable, cli, "peek", name, *specs, "--json", timeout=60)
        return [int(str(p["value"]).split()[0]) for p in json.loads(o[o.index("{"):])["peeks"]]

    def records(name):
        specs = []
        for i in range(RECORDS):
            base = REC + i * STRIDE
            specs += [f"*0x511DE8+0x{base:X}:4", f"*0x511DE8+0x{base + 4:X}:4",
                      f"*0x511DE8+0x{base + 0x73:X}:1", f"*0x511DE8+0x{base + 0x74:X}:4"]
        v = peek(name, *specs)
        return {i: (v[4 * i] != 0, v[4 * i + 1], v[4 * i + 2], v[4 * i + 3] != 0) for i in range(RECORDS)}

    with dpport.lease(port) as lease:
        port = lease.port
        print(f"DirectPlay port {port}", flush=True)
        try:
            display = tareplay.start_xvfb()
            (out / "build-sha.txt").write_text(hashlib.sha256(dll.read_bytes()).hexdigest() + "\n")
            token = f"na-{runlock.tag()}-"     # an instance of the run's tag is a dead run's
            for suffix, netai in (("H", "impai"), ("J", "ota")):
                name = token + suffix
                subprocess.run([sys.executable, str(cli), "rm", name, "--force"], stdout=log,
                               stderr=subprocess.STDOUT, check=False, timeout=120)
                owned.append(name)
                command(sys.executable, cli, "create", name, "--private-registry", "--display", display.display,
                        "--res", "1024x768")
                shutil.copyfile(dll, instances / name / "gamedir" / "ddraw.dll")
                # recmp_create_begin_test.py's settings: no commander's death ends the game
                command(sys.executable, cli, "registry", name, "MultiCommanderDeath=0", "gamespeed=20")
                command(sys.executable, cli, "launch", name, "--keep-dll", "--dplay", "--dplay-port", port,
                        "--netai", netai)
                command(sys.executable, cli, "ui", name, "wait", "--gui", "MAINMENU", "--timeout", "45")
            host, join = owned
            subprocess.run(["bash", str(TREE / "tools/mp_lobby.sh"), "--map", MAP, *owned], stdout=log,
                           stderr=subprocess.STDOUT, check=True, timeout=900,
                           env=dict(os.environ, MP_NO_START="1"))
            for _ in range(2):
                command(sys.executable, cli, "ui", host, "click", "PLAYER2")
            command(sys.executable, cli, "ui", join, "click", "PLAYER3")
            for name in owned:
                command(sys.executable, cli, "ui", name, "click", "READY0")
            command(sys.executable, cli, "ui", host, "click", "START", "--timeout", "30")
            for name in owned:
                command(sys.executable, cli, "wait", name, "alive=[1-9]", "--timeout", "300", timeout=360)
            last, moved = -1, time.monotonic()
            while True:
                now = peek(host, tareplay.GAMETIME)[0]
                if now >= until:
                    break
                if now != last:
                    last, moved = now, time.monotonic()
                elif time.monotonic() - moved > STALL_S:
                    raise tareplay.CouldNotRun(f"the host's GameTime stood at {now} for {STALL_S} s")
                time.sleep(2)
            peers = {name: {"records": records(name)} for name in owned}
            (out / "records.json").write_text(json.dumps({n: {str(i): r for i, r in p["records"].items()}
                                                          for n, p in peers.items()}, indent=1) + "\n")
            for name in owned:
                gui = json.loads(command(sys.executable, cli, "ui", name, "--json"))["gui"].upper()
                if gui == "ENDMSN.GUI":
                    command(sys.executable, cli, "ui", name, "click", "MainMenu", "--timeout", "30")
                elif gui != "MAINMENU.GUI":
                    command("bash", TREE / "tools/mp_leave.sh", name, timeout=300)
            for name in owned:
                peers[name]["log"] = talog.run_text(instances / name / "gamedir")
            bad = judge(peers[host], peers[join], pulse)
            for name in owned:
                for line in peers[name]["log"].splitlines():
                    if "impai: seats" in line or "impai: calls" in line or "impai: pulse" in line:
                        print(f"  {name}: {line.strip()}")
            return bad
        finally:
            for name in owned:
                game = instances / name / "gamedir"
                if (game / "log/tagpu.log").exists():
                    (out / f"{name}.log").write_text(talog.run_text(game))
                if (instances / name / "instance.json").exists():
                    subprocess.run([sys.executable, str(cli), "stop", name], stdout=log, stderr=subprocess.STDOUT,
                                   check=False, timeout=60)
                    subprocess.run([sys.executable, str(cli), "rm", name], stdout=log, stderr=subprocess.STDOUT,
                                   check=False, timeout=60)
            if display:
                tareplay.stop_xvfb(display)
            log.close()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--dll", type=Path, default=TREE / "tagpu/ddraw/ddraw.dll")
    p.add_argument("--port", type=int, default=None, help="this DirectPlay port (default: the first free lease)")
    p.add_argument("--until", type=int, default=UNTIL, help="the host's GameTime the game plays to")
    p.add_argument("--pulse", action="store_true", help="require the host ImpAI seat's pulse lines")
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
