#!/usr/bin/env python3
"""Check native unit-movement delivery through the owned primary receiver.

Two humans play in owned Wine prefixes, with saving off and a 20-unit limit.
Both command their local commander; both native observers must see each unit
move. --raw uses the unqueued primary transport. This is a live receive test,
not saved-file completeness, replay application, bandwidth or loading chat.
"""
import argparse
import fcntl
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import uuid

import tareplay

TREE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TREE / "tools"))
import talog


def run(args, output):
    instances = tareplay.main_checkout() / "tagpu/instances"
    with (instances / f"recmp-net-port-{args.port}.lock").open("a") as lease:
        fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
        display = tareplay.start_xvfb()
        owned, samples = [], []
        try:
            dll = output / "tested-ddraw-cover.dll"
            shutil.copyfile(TREE / "tagpu/ddraw/ddraw-cover.dll", dll)
            shutil.copyfile(TREE / "tagpu/ddraw/ddraw-cover.map", output / "tested-ddraw-cover.map")
            (output / "build-sha.txt").write_text(hashlib.sha256(dll.read_bytes()).hexdigest() + "\n")
            with (output / "driver.log").open("w") as log:
                def command(*argv):
                    result = subprocess.run(list(map(str, argv)), capture_output=True,
                                            text=True, timeout=180)
                    log.write(repr(argv) + "\n" + result.stdout + result.stderr)
                    log.flush()
                    if result.returncode:
                        raise RuntimeError(result.stdout + result.stderr)
                    return result.stdout

                def cli(*argv):
                    return command(sys.executable, TREE / "tools/tacli", *argv)

                token = uuid.uuid4().hex[:8]
                names = [token + role for role in ("H", "J")]
                for name in names:
                    (instances / name).mkdir(exist_ok=False)
                    owned.append(name)
                    cli("create", name, "--private-registry", "--display", display.display,
                        "--res", "1024x768")
                    shutil.copyfile(dll, instances / name / "gamedir/ddraw.dll")
                    cli("arm", name, "recorder.off")
                    cli("launch", name, "--keep-dll", "--dplay", "--dplay-port", args.port,
                        "--unit-limit", 20, *(["--arg=-p", "--arg=-1"] if args.raw else []))
                    cli("ui", name, "wait", "--gui", "MAINMENU", "--timeout", 45)
                command("bash", TREE / "tools/mp_lobby.sh", "--map", "Two Continents", *names)
                for name in names:
                    cli("wait", name, "alive=[1-9]", "--timeout", 120)

                def snapshot():
                    result = {}
                    for name in names:
                        roster = json.loads(cli("roster", name, "--json"))
                        values = json.loads(cli("peek", name, "*0x511DE8+0x38A47:4",
                                                "0x506DBC:4", "--json"))["peeks"]
                        numbers = [int(re.search(r"\((0x[0-9A-Fa-f]+)\)", v["value"]).group(1), 16)
                                   for v in values]
                        if bool(numbers[1]) == args.raw:
                            raise RuntimeError("native primary transport mode differs from request")
                        result[name] = dict(tick=numbers[0], queued=numbers[1], units=roster["units"])
                    samples.append(result)
                    (output / "samples.json").write_text(json.dumps(samples, indent=2))
                    return result

                first, targets = snapshot(), {}
                for name in names:
                    local = [unit for unit in first[name]["units"] if unit["owner"] == 0]
                    if len(local) != 1 or local[0]["type"].upper() != "ARMCOM":
                        raise RuntimeError("no unique local commander")
                    unit = local[0]
                    x, y = unit["world"]
                    goal = (x + 96 if x < 4096 else x - 96, y)
                    targets[name] = dict(index=unit["engine_index"], start=(x, y), target=goal)
                    cli("order", name, "--unit", unit["engine_index"], "--expect", "ARMCOM",
                        "move", "pos", *goal)
                (output / "targets.json").write_text(json.dumps(targets, indent=2))
                deadline = time.monotonic() + 75
                while True:
                    state = snapshot()
                    moved = []
                    for owner in names:
                        target = targets[owner]
                        for observer in names:
                            units = [u for u in state[observer]["units"]
                                     if u["engine_index"] == target["index"]]
                            if len(units) != 1:
                                raise RuntimeError("native unit disappeared")
                            x, y = units[0]["world"]
                            moved.append(abs(x-target["start"][0]) + abs(y-target["start"][1]) >= 48)
                    if all(moved):
                        break
                    if time.monotonic() > deadline:
                        raise RuntimeError("native remote movement not observed; inspect samples.json")
                    time.sleep(.5)
                for name in names:
                    text = talog.run_text(instances / name / "gamedir")
                    if "recmp: ARMED owned primary receive frames and serialized dispatch" not in text:
                        raise RuntimeError("owned primary receiver did not arm")
                    if "recmp: contribution refused" in text:
                        raise RuntimeError("mandatory contributions failed before test completion")
                    wire = re.findall(r"\| wire: in .*? 2c=(\d+).*? stale=(\d+)", text)
                    if not wire or int(wire[-1][0]) == 0 or any(int(stale) for _, stale in wire):
                        raise RuntimeError("native movement decoder did not accept exact receive spans")
                print("PASS: both owners move on both native observers; movement decoder receives owned spans",
                      flush=True)
        finally:
            cleanup_errors = []
            for name in owned:
                game = instances / name / "gamedir"
                try:
                    if game.exists():
                        (output / (name + "-tagpu.log")).write_text(talog.run_text(game))
                except (OSError, ValueError) as error:
                    cleanup_errors.append(f"{name} log: {error}")
                for verb in ("stop", "rm"):
                    try:
                        subprocess.run([sys.executable, str(TREE / "tools/tacli"), verb, name],
                                       capture_output=True, text=True, timeout=45, check=True)
                    except (OSError, subprocess.SubprocessError) as error:
                        cleanup_errors.append(f"{name} {verb}: {error}")
            try:
                tareplay.stop_xvfb(display)
            except (OSError, subprocess.SubprocessError) as error:
                cleanup_errors.append(f"display cleanup: {error}")
            if cleanup_errors:
                raise RuntimeError("; ".join(cleanup_errors))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--raw", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("port must be 1..65535")
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=False)
    print(f"evidence: {output}", flush=True)
    try:
        run(args, output)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, tareplay.CouldNotRun) as error:
        print(f"FAIL: {error}; inspect {output}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
