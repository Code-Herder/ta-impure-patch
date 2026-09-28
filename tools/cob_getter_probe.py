#!/usr/bin/env python3
"""Execute the eight required COB getters in an isolated Escalation skirmish.

    python3 tools/cob_getter_probe.py --out /tmp/cob-getters --expect stock-zero
    python3 tools/cob_getter_probe.py --out /tmp/cob-getters-port --expect implemented

The second invocation is a red test until the port exists. A generated unit uses
the mod's solar model/FBI, with an entirely generated script. Nothing is installed
in the player's mod. Both the human and local AI get a probe. This covers ordinary
self queries only, not bounds, multiplayer, veterancy changes or build progress.
The output contains local paths and derived mod assets: keep it outside the repo.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import sys
import time
import uuid

import cob_audit
import hpipack
import talog

sys.path.insert(0, str(Path(__file__).parent / "compat"))
import tacompat as compat

GETTERS = (32, 69, 70, 71, 72, 73, 74, 75)
TYPE = "COBFPROBE"


def probe_cob(pieces):
    """Sequential CALLs need two records, with no dependency on extended getters."""
    op = cob_audit.tacob.OP
    code = [op["PUSH_CONSTANT"], 1000, op["SLEEP"]]
    for index in range(1, len(GETTERS) + 1):
        code += [op["CALL_SCRIPT"], index, 0]
    code += [op["PUSH_CONSTANT"], 0, op["RETURN"]]
    scripts = [("Create", 0)]
    for getter in GETTERS:
        scripts.append((f"Probe{getter}", len(code)))
        code += [op["PUSH_CONSTANT"], getter]
        if getter >= 72:
            code += [op["PUSH_CONSTANT"], 71, op["GET_UNIT_VALUE"]]
            code += [op["PUSH_CONSTANT"], 0] * 3 + [op["GET"]]
        else:
            code += [op["GET_UNIT_VALUE"]]
        code += [op["RETURN"]]
    return cob_audit.tacob.write_cob(cob_audit.tacob.Cob(scripts, pieces, 0, code))


def fixture(archive):
    original = cob_audit.tacob.read_cob(archive.read("scripts/armsolar.cob"))
    fbi = archive.read("unitse/armsolar.fbi").decode("latin-1")
    fbi, changed = re.subn(r"(?im)(\bUnitName\s*=\s*)[^;]+;", rf"\g<1>{TYPE};", fbi)
    if changed != 1:
        raise ValueError("solar FBI must have exactly one UnitName")
    return hpipack.build({"unitse": {TYPE + ".fbi": fbi.encode("latin-1")},
                          "scripts": {TYPE + ".cob": probe_cob(original.pieces)}})


def returns(trace):
    rows = {}
    for line in trace.splitlines():
        fields = line.split("\t")
        if len(fields) == 6 and fields[0] == "R" and re.fullmatch(r"Probe\d+", fields[4]):
            key = (int(fields[2]), int(fields[4][5:]))
            if key in rows:
                raise ValueError(f"duplicate probe return: {key}")
            rows[key] = int(fields[5])
    return rows


def judge(rows, units, slots, mode):
    checks = []
    for unit, owner in units.items():
        expected = {32: 0, 69: 1, 70: slots - 1, 71: unit, 72: owner,
                    73: 0, 74: 1, 75: 1}
        for getter in GETTERS:
            want = 0 if mode == "stock-zero" else expected[getter]
            got = rows.get((unit, getter))
            checks.append(dict(unit=unit, owner=owner, getter=getter,
                               expected=want, actual=got, ok=got == want))
    return checks


def command(*args, timeout=60):
    result = compat.tacli(*args, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"tacli {' '.join(args)}: {result.stderr or result.stdout}")
    return result.stdout


def enter_skirmish(name):
    for gadget in ("SINGLE", "Skirmish", "Start"):
        deadline = time.monotonic() + 60
        while True:
            ui = compat.tacli("ui", name, timeout=20).stdout
            if compat.IN_GAME_PANEL.search((ui.splitlines() or [""])[0]):
                return
            result = compat.tacli("ui", name, "click", gadget, timeout=60)
            if result.returncode == 0:
                break
            if time.monotonic() >= deadline:
                raise RuntimeError(f"cannot enter skirmish: {result.stderr or result.stdout}")
            time.sleep(1)
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        ui = compat.tacli("ui", name, timeout=20).stdout
        if compat.IN_GAME_PANEL.search((ui.splitlines() or [""])[0]):
            return
        time.sleep(1)
    raise RuntimeError("skirmish never reached the game screen")


def run(out, mode, dll):
    setup = compat.pick_setups(["escalation"])[0]
    problems = [p for entry in setup["add"] for p in compat.fixture_problems(entry["fixture"])]
    if problems:
        raise RuntimeError(str(problems))
    compat.PREFIX = "cobf-" + uuid.uuid4().hex[:8] + "-"
    name = compat.inst_name(setup["name"])
    if (compat.INSTANCES / name).exists():
        raise RuntimeError("probe instance already exists; refusing to replace it")
    display = compat.free_display(set())
    game, inst = None, None
    report = dict(mode=mode, instance=name, setup="escalation", dll_sha256=
                  hashlib.sha256(dll.read_bytes()).hexdigest())
    try:
        print(f"Preparing {name}", flush=True)
        inst = compat.prepare_wine(setup, dll, display)
        archive = hpipack.Archive(compat.FIXTURES / "escalation-gold-10.2.0/TAESC.gp3")
        (inst["gamedir"] / "cob-getter-probe.ufo").write_bytes(fixture(archive))
        (inst["gamedir"] / "tagpu_cobtrace.on").write_text(TYPE)
        print("Starting Escalation and entering skirmish", flush=True)
        game = compat.start_wine(inst, display)
        enter_skirmish(name)
        content = json.loads(command("units", name, "--json"))
        (out / "units.json").write_text(json.dumps(content, indent=2))
        scenario = dict(format="ta-scenario/1", seed=7, on_error="abort",
                        setup=dict(clear_existing=True), units=[
                            dict(id="human", type=TYPE, owner=0, pos=[2600, 1200]),
                            dict(id="ai", type=TYPE, owner=1, pos=[4200, 2400])])
        scenario_path = out / "scenario.json"
        scenario_path.write_text(json.dumps(scenario, indent=2))
        applied = json.loads(command("scenario", "apply", name, str(scenario_path), "--json", timeout=180))
        (out / "apply.json").write_text(json.dumps(applied, indent=2))
        units = {applied["units"][handle]["engine_index"]: owner
                 for owner, handle in enumerate(("human", "ai"))}
        if len(units) != 2:
            raise RuntimeError("the scenario did not create two distinct probe units")
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            rows = returns(talog.run_text(inst["gamedir"], "tagpu_cobtrace"))
            if all((u, g) in rows for u in units for g in GETTERS):
                break
            if game["proc"].poll() is not None:
                raise RuntimeError("game exited before probe completion")
            time.sleep(0.2)
        hooks = compat.exe_hooks(game["proc"].pid, inst["gamedir"])
        report["hooks"] = hooks
        if hooks.get("why"):
            raise RuntimeError(hooks["why"])
        with open(f"/proc/{hooks['pid']}/mem", "rb") as memory:
            memory.seek(0x511DE8)
            main = struct.unpack("<I", memory.read(4))[0]
            memory.seek(main + 0x14351)
            slots = struct.unpack("<H", memory.read(2))[0]
            memory.seek(main + 0x14357)
            base = struct.unpack("<I", memory.read(4))[0]
            report["native_units"] = []
            for unit, owner in units.items():
                if not 0 < unit < slots:
                    raise RuntimeError(f"probe ID {unit} is outside the native array")
                memory.seek(base + unit * 0x118)
                data = memory.read(0x118)
                snapshot = dict(unit=struct.unpack_from("<H", data, 0xA8)[0],
                                owner=data[0xFF], kills=struct.unpack_from("<H", data, 0xB8)[0],
                                nano=struct.unpack_from("<f", data, 0x104)[0],
                                alive=bool(struct.unpack_from("<I", data, 0x110)[0] & 0x10000000))
                report["native_units"].append(snapshot)
                if snapshot != dict(unit=unit, owner=owner, kills=0, nano=0.0, alive=True):
                    raise RuntimeError(f"probe preconditions differ from the scenario: {snapshot}")
        report["unit_slots"] = slots
        report["checks"] = judge(rows, units, slots, mode)
        report["ok"] = all(row["ok"] for row in report["checks"])
        # The compatibility checker validates both code hooks and recorder logs.
        report["tadr_evidence"] = compat.hook_evidence(hooks)
        tdraw_path = inst["gamedir"] / "tdrawlog.txt"
        tdraw = tdraw_path.read_text(errors="replace") if tdraw_path.exists() else None
        report["tdraw_started_only"] = compat._started_only(tdraw)
        report["tadr_evidence"] += compat.tadr_evidence(
            None if compat._started_only(tdraw) else tdraw,
            compat.other_logs(inst["gamedir"] / "log", 0))
        if report["tadr_evidence"]:
            report["ok"] = False
    except Exception as exc:
        report.update(ok=False, error=str(exc))
    finally:
        if game:
            wine_log = compat.stop_wine(game, keep=True)
            shutil.move(wine_log, out / "wine.log")
        if inst:
            for stream in ("tagpu", "tagpu_cobtrace"):
                (out / (stream + ".log")).write_text(talog.run_text(inst["gamedir"], stream))
        (out / "result.json").write_text(json.dumps(report, indent=2) + "\n")
        # Remove only the fresh, uniquely named instance created by this run.
        compat.tacli("rm", name, "--force")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True, help="new output directory outside the repo")
    parser.add_argument("--expect", choices=("stock-zero", "implemented"), required=True)
    parser.add_argument("--dll", type=Path, default=Path(__file__).resolve().parents[1] / "tagpu/ddraw/ddraw.dll")
    args = parser.parse_args()
    output = args.out.resolve()
    for root in (compat.TREE, compat.main_checkout()):
        if output == root or root in output.parents:
            parser.error("probe output must be outside the repository")
    output.mkdir(parents=True, exist_ok=False)
    report = run(output, args.expect, args.dll.resolve())
    print(json.dumps({k: v for k, v in report.items() if k != "hooks"}, indent=2))
    print(f"Evidence: {output}")
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
