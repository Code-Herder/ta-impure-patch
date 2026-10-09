#!/usr/bin/env python3
"""Exercise contextual mex upgrades across every Comet Catcher deposit.

prepare reads installed map content and writes a private resource-supply unit
and a scenario. run uses real configured mouse commands, then measures reclaim,
nanoframe creation and completion. Original content stays in the game folder.
Every command holds the world and advances it by exact ticks (resource_regression.py,
IN LOCKSTEP): the clicks land on a held world and each observation reads one.
"""
import argparse
import json
import math
import re
import struct
import subprocess
import sys
from pathlib import Path

import hpipack
from resource_regression import Check, TACLI

OBSERVE_STEP = 150   # ticks between two observations of a stress run
CORE_STEP = 30       # ticks between two observations of the CORE handoff, to T+600


def supply(gamedir):
    # The constructor and mex definitions are left intact; supply alone is synthetic.
    archives = [hpipack.Archive(gamedir / name) for name in
                ("totala1.hpi", "ccdata.ccx", "rev31.gp3")]
    content = {name: a for a in archives for name in a.files}
    fbi = content["units/armmstor.fbi"].read("units/armmstor.fbi").decode("latin-1")
    for key, value in {"UnitName": "RSSTRESS", "MetalStorage": 200000,
                       "EnergyStorage": 200000}.items():
        fbi, n = re.subn(rf"\b{key}\s*=[^;]*;", f"{key}={value};", fbi, count=1, flags=re.I)
        if not n:
            assert key != "UnitName", "supply fixture lacks its name"
            fbi = fbi.replace("{", "{" + f"{key}={value};", 1)
    for key in ("MetalMake", "EnergyMake"):
        fbi, n = re.subn(rf"\b{key}\s*=[^;]*;", f"{key}=20000;", fbi, count=1, flags=re.I)
        if not n:
            fbi = fbi.replace("{", "{" + f"{key}=20000;", 1)
    tree = {}
    hpipack.insert(tree, "units/rsstress.fbi", fbi.encode("latin-1"))
    hpipack.insert(tree, "scripts/rsstress.cob",
                   content["scripts/armmstor.cob"].read("scripts/armmstor.cob"))
    (gamedir / "resource-stress.ufo").write_bytes(hpipack.build(tree, method=2))


def prepare(gamedir, destination, phase, sides):
    archive = hpipack.Archive(gamedir / "Cometctr.ufo")
    data = archive.read("maps/comet catcher.tnt")
    header = struct.unpack_from("<16i", data)
    w, h, attrs, count, names = header[1], header[2], header[4], header[7], header[8]
    metal = {i for i in range(count) if data[names + i * 132 + 4:names + i * 132 + 132]
             .split(b"\0")[0].lower().startswith(b"moonmetal")}
    assert metal and w == 384 and h == 480, "installed map is not the expected Comet Catcher"
    deposits = [(x, z) for z in range(h) for x in range(w)
                if struct.unpack_from("<H", data, attrs + (z * w + x) * 4 + 1)[0] in metal]
    # The map's feature anchors are the top-left of the 3x3 metal footprint.
    supply(gamedir)
    distances = (48, 64, 96, 144, 256, 384)
    units, cases = [], []
    for i, (x, z) in enumerate(deposits):
        cx, cz = x * 16 + 24, z * 16 + 24
        angle = ((i + phase) % 8) * 45
        distance = distances[((i // 8) + phase) % len(distances)]
        # Unit creation can put a bot at a border position it cannot leave.
        # Keep generated starts inside the map; record the actual approach too.
        bx = max(128, min(w * 16 - 128, round(cx + distance * math.cos(math.radians(angle)))))
        bz = max(128, min(h * 16 - 128, round(cz + distance * math.sin(math.radians(angle)))))
        side = ("ARM" if (i // 16 + phase) % 2 == 0 else "COR") if sides == "mixed" else {"arm": "ARM", "core": "COR"}[sides]
        mex, bot = f"mex{i}", f"bot{i}"
        units += [{"id": mex, "type": side + "MEX", "owner": 0, "pos": [cx, cz]},
                  {"id": bot, "type": side + "ACK", "owner": 0, "pos": [bx, bz]}]
        cases.append({"mex": mex, "bot": bot, "angle": angle, "distance": distance,
                      "mex_type": side + "MEX", "bot_type": side + "ACK",
                      "replacement": side + "MOHO"})
    units += [{"id": "supply", "type": "RSSTRESS", "owner": 0, "pos": [3000, 3800]},
              {"id": "opponent", "type": "CORSOLAR", "owner": 1, "pos": [5800, 7400]}]
    scenario = {"format": "ta-scenario/1", "seed": 20261005, "on_error": "abort",
                "description": "Every Comet Catcher metal patch, contextual Moho upgrades from varied bearings and distances.",
                "setup": {"map": "Comet Catcher", "res": "1024x768", "clear_existing": True,
                          "switches": {"noshake": True},
                          "players": [{"slot": 0, "controller": "human", "side": "core",
                                       "metal": 200000, "energy": 200000},
                                      {"slot": 1, "controller": "ai", "side": "arm"}]},
                "units": units, "camera": {"at": [3000, 3800]}}
    destination.write_text(json.dumps(scenario, indent=2) + "\n")
    destination.with_suffix(".cases.json").write_text(json.dumps(cases, indent=2) + "\n")
    print(f"prepared {len(cases)} deposits; phase={phase}; original constructors and Moho extractors")


class Stress(Check):
    def __init__(self, name, handles):
        self.name = name
        self.lockstep()
        self.base, end, count, self.defs, self.ndefs = self.values(
            "*0x511DE8+0x14357", "*0x511DE8+0x1435b", "*0x511DE8+0x14351:2",
            "*0x511DE8+0x1439b", "*0x511DE8+0x1438f")
        assert end >= self.base and (end - self.base) % 0x118 == 0
        assert (end - self.base) // 0x118 + 1 == count, "unit allocation/count mismatch"
        self.count = count
        self.units = {key: value["engine_index"] for key, value in handles.items()}
        assert all(0 < index < count for index in self.units.values()), "scenario handle outside the unit pool"
        self.types = {u["id"]: u["name"].upper() for u in self.call("units", "--json")["units"]}

    def orders(self, kind):
        owner = self.address(kind)
        node = self.values(f"0x{owner + 0x5c:x}")[0]
        rows, seen = [], set()
        while node:
            assert node not in seen and len(seen) < 100, "cyclic/excessive stress order chain"
            seen.add(node)
            b = self.bytes(f"0x{node:x}:x50")
            assert struct.unpack_from("<I", b, 0x0e)[0] == owner, "wrong order owner"
            rows.append({"action": b[4], "state": b[5], "flags": struct.unpack_from("<I", b, 6)[0],
                         "target": struct.unpack_from("<I", b, 0x16)[0],
                         "pos": struct.unpack_from("<iii", b, 0x22),
                         "type": struct.unpack_from("<H", b, 0x36)[0],
                         "placement_attempts": struct.unpack_from("<I", b, 0x3e)[0]})
            node = struct.unpack_from("<I", b, 0x4a)[0]
        return rows

    def snapshot(self, cases):
        assert self.values("*0x511DE8+0x38a51:2")[0] & 1, "order inspection requires a held simulation"
        roster = self.call("roster", "--json")["units"]
        heads = self.values(*(f"0x{self.address(c['bot']) + 0x5c:x}" for c in cases))
        pending = {c["bot"]: head for c, head in zip(cases, heads) if head}
        orders = {c["bot"]: [] for c in cases}
        seen = {c["bot"]: set() for c in cases}
        # The pause bit stops the unit tick and its order destructors; all reads
        # below are game-thread peeks with no injected order input between them.
        while pending:
            items = list(pending.items())
            peeks = self.call("peek", *(f"0x{node:x}:x50" for _, node in items), "--json")["peeks"]
            pending = {}
            for (bot, node), p in zip(items, peeks):
                assert node not in seen[bot] and len(seen[bot]) < 100, "cyclic/excessive stress order chain"
                seen[bot].add(node)
                b = bytes.fromhex(p["value"])
                assert struct.unpack_from("<I", b, 0x0e)[0] == self.address(bot), "wrong order owner"
                orders[bot].append({"action": b[4], "state": b[5], "flags": struct.unpack_from("<I", b, 6)[0],
                                    "target": struct.unpack_from("<I", b, 0x16)[0],
                                    "pos": struct.unpack_from("<iii", b, 0x22),
                                    "type": struct.unpack_from("<H", b, 0x36)[0],
                                    "placement_attempts": struct.unpack_from("<I", b, 0x3e)[0]})
                following = struct.unpack_from("<I", b, 0x4a)[0]
                if following:
                    pending[bot] = following
        # The roster is periodic. It supplies candidate IDs, never current unit
        # facts. Build references also include newly created frames before a dump.
        indices = set(self.units.values()) | {u["engine_index"] for u in roster}
        for chain in orders.values():
            for order in chain:
                if order["action"] == 25 and order["target"]:
                    offset = order["target"] - self.base
                    assert offset >= 0 and offset % 0x118 == 0 and offset // 0x118 < self.count
                    indices.add(offset // 0x118)
        indices = sorted(i for i in indices if i and 0 < i < self.count)
        live, completed = {}, {}
        for first in range(0, len(indices), 64):
            chunk = indices[first:first + 64]
            peeks = self.call("peek", *(f"0x{self.base + i * 0x118 + 0x6a:x}:xae" for i in chunk), "--json")["peeks"]
            for index, p in zip(chunk, peeks):
                b = bytes.fromhex(p["value"])
                flags = struct.unpack_from("<I", b, 0xa6)[0]
                if not flags & 0x10000000 or flags & 0x4000:
                    continue
                definition = struct.unpack_from("<I", b, 0x28)[0] - self.defs
                assert definition >= 0 and definition % 0x249 == 0 and definition // 0x249 < self.ndefs
                kind = self.types[definition // 0x249]
                x, height, z = (n >> 16 for n in struct.unpack_from("<iii", b))
                live[index] = {"engine_index": index, "type": kind, "owner": b[0x95],
                               "world": [x, z], "height": height,
                               "cells": list(struct.unpack_from("<hh", b, 0xc)),
                               "footprint": list(struct.unpack_from("<hh", b, 0x14))}
                if kind in ("ARMMOHO", "CORMOHO"):
                    completed[index] = struct.unpack_from("<f", b, 0x9a)[0]
        roster = list(live.values())
        result = []
        for case in cases:
            old = live.get(self.units[case["mex"]])
            reclaimed = not old or old["type"].upper() != case["mex_type"]
            wx, wz = case["world"]
            replacements = [u for u in roster if u["owner"] == 0 and
                            u["type"].upper() == case["replacement"] and
                            abs(u["world"][0] - wx) <= 16 and abs(u["world"][1] - wz) <= 16]
            replacement = replacements[0] if replacements else None
            result.append({**case, "reclaimed": reclaimed, "replacement": replacement,
                           "remaining": completed.get(replacement["engine_index"]) if replacement else None,
                           "builder": live.get(self.units[case["bot"]]),
                           "orders": orders[case["bot"]]})
        return result


def run(name, scenario, out, ticks):
    # apply is restricted by tacli to the map the scenario names. Every order
    # under test below comes from its real contextual mouse dispatcher.
    shell = Stress(name, {})
    shell.hold()
    reply = subprocess.run([sys.executable, str(TACLI), "scenario", "apply", name,
                            str(scenario), "--json"], capture_output=True, text=True, timeout=60)
    assert reply.returncode == 0, reply.stdout + reply.stderr
    applied = json.loads(reply.stdout)
    # the applied units' view of the same world, still held where the shell held it
    c = Stress(name, applied["units"])
    c.held, c.tick = shell.held, shell.tick
    c.call("arm", "menu.open")
    c.until(lambda: {"RESOURCE", "QUICKBUILD"} <=
            {g["name"] for g in c.call("ui", "--json")["gadgets"]}, "resource controls did not open")
    c.call("ui", "set", "RESOURCE", 3)
    c.call("ui", "set", "QUICKBUILD", 1)
    c.call("arm", "menu.open=off")
    c.until(lambda: c.call("ui", "--json")["gui"] != "RENDER.GUI", "options did not close")
    cases = json.loads(scenario.with_suffix(".cases.json").read_text())
    right = c.values("*0x511DE8+0x37efa")[0] != 0
    out.mkdir(parents=True, exist_ok=True)
    (out / "applied.json").write_text(json.dumps(applied, indent=2))
    for i, case in enumerate(cases):
        mx, mz, mh = applied["units"][case["mex"]]["actual"]
        bx, bz, bh = applied["units"][case["bot"]]["actual"]
        case["world"] = [mx, mz]
        case["actual_distance"] = math.hypot(bx - mx, bz - mz)
        case["actual_angle"] = math.degrees(math.atan2(bz - mz, bx - mx)) % 360
        ex = max(0, min(6144 - 896, (mx + bx) // 2 - 350))
        ez = max(0, min(7680 - 704, (mz + bz) // 2 - 300))
        c.eye(ex, ez)
        sx, sy = bx - ex + 128, bz - bh // 2 - ez + 32
        tx, ty = mx - ex + 128, mz - mh // 2 - ez + 32
        assert 128 < sx < 1024 and 32 < sy < 744 and 128 < tx < 1024 and 32 < ty < 744, "case is outside the native input viewport"
        c.keys("esc", f"pclick:{sx},{sy}")
        c.until(lambda: c.values(f"0x{c.address(case['bot']) + 0x110:x}")[0] & 0x10,
                "constructor selection did not reach the stress case")
        c.keys(f"{'prclick' if right else 'pclick'}:{tx},{ty}")
        orders = c.orders(case["bot"])
        case["accepted"] = (len(orders) >= 2 and orders[0]["action"] == 33 and
                            orders[0]["target"] == c.address(case["mex"]) and
                            orders[1]["action"] == 25 and
                            c.types.get(orders[1]["type"]) == case["replacement"])
        case["issued"] = orders
        print(f"{i+1}/{len(cases)} {case['bot_type']} bearing={case['angle']} distance={case['distance']} accepted={case['accepted']}", flush=True)
    (out / "issued.json").write_text(json.dumps(cases, indent=2))
    assert any(case["accepted"] for case in cases), "no contextual upgrade was accepted"
    start = c.values("*0x511DE8+0x38a47")[0]
    (out / "start.json").write_text(json.dumps({"tick": start}))
    return observe(c, cases, out, ticks, start)


def observe(c, cases, out, ticks, start):
    observed = set()
    while True:
        now = c.values("*0x511DE8+0x38a47")[0]
        rows = c.snapshot(cases)
        observed.update(r["mex"] for r in rows if r["replacement"])
        accepted = [r for r in rows if r["accepted"]]
        complete = sum(r["remaining"] == 0 for r in accepted)
        abandoned = [r for r in accepted if r["reclaimed"] and not r["replacement"] and
                     not any(o["action"] == 25 and o["type"] for o in r["orders"])]
        print(f"T+{now-start}: accepted={len(accepted)} reclaimed={sum(r['reclaimed'] for r in accepted)} started={len(observed)} completed={complete} abandoned={len(abandoned)}", flush=True)
        (out / "latest.json").write_text(json.dumps({"elapsed_ticks": now - start, "cases": rows}, indent=2))
        if abandoned or complete == len(accepted) or now - start >= ticks:
            break
        c.advance(min(OBSERVE_STEP, ticks - (now - start)))
    failures = [r for r in rows if r["accepted"] and r["remaining"] != 0]
    print(f"finished: {complete}/{len(accepted)} completed; {len(cases)-len(accepted)} refused; {len(failures)} incomplete", flush=True)
    return 1 if failures else 0


def check_core(name):
    shell = Stress(name, {})
    def handles():
        units = {u["type"].upper(): u["engine_index"] for u in shell.call("roster", "--json")["units"]
                 if u["owner"] == 0}
        return {label: {"engine_index": units[kind]} for label, kind in
                (("bot", "CORACK"), ("mex", "CORMEX"))} if {"CORACK", "CORMEX"} <= units.keys() else None
    c = Stress(name, shell.until(handles, "CORE handoff fixture units absent"))
    c.hold()
    c.menu(RESOURCE=3, QUICKBUILD=1)
    c.eye(2300, 1300)
    c.select("bot")
    c.resource_click(*c.screen("mex"))
    queued = c.orders("bot")
    assert len(queued) >= 2 and queued[0]["action"] == 33 and queued[0]["target"] == c.address("mex"), "CORE upgrade did not reclaim the old mex"
    assert queued[1]["action"] == 25 and c.types[queued[1]["type"]] == "CORMOHO", "CORE upgrade did not queue the stock Moho"
    case = {"mex": "mex", "bot": "bot", "world": [2824, 1704], "mex_type": "CORMEX",
            "bot_type": "CORACK", "replacement": "CORMOHO", "accepted": True}
    start = c.values("*0x511DE8+0x38a47")[0]
    while True:
        c.advance(CORE_STEP)
        elapsed = c.values("*0x511DE8+0x38a47")[0] - start
        row = c.snapshot([case])[0]
        abandoned = row["reclaimed"] and not row["replacement"] and not any(
            o["action"] == 25 and o["type"] for o in row["orders"])
        assert not abandoned, f"CORE reclaimed the mex but abandoned its Moho at T+{elapsed}: {row}"
        if elapsed >= 600:
            break
    assert row["reclaimed"] and row["replacement"] and row["remaining"] < 0.99, f"CORE Moho never began construction: {row}"
    assert c.values(f"0x{c.address('bot') + 0x10f:x}:1")[0] & 1, "CORE constructor never entered build stance"
    c.release()
    print(f"resource-core-upgrade: real contextual reclaim/build, native stance and stock Moho progress passed at T+{elapsed} ({row['remaining']:.6f} remaining)")


def finish_core(name):
    c = Stress(name, {})
    c.hold()
    roster = c.call("roster", "--json")["units"]
    candidates = [u for u in roster if u["owner"] == 0 and u["type"].upper() == "CORMOHO"]
    assert len(candidates) == 1, "CORE completion fixture has no unique Moho"
    index = candidates[0]["engine_index"]
    assert 0 < index < c.count, "CORE Moho outside the unit allocation"
    base = c.base + index * 0x118
    data = c.bytes(f"0x{base + 0x6a:x}:xae")
    assert struct.unpack_from("<I", data, 0xa6)[0] & 0x10000000, "CORE Moho no longer alive"
    assert c.types[struct.unpack_from("<H", data, 0x3c)[0]] == "CORMOHO", "CORE completion roster names a recycled unit"
    assert struct.unpack_from("<f", data, 0x9a)[0] == 0, "CORE upgrade never completed its Moho"
    assert all(u["type"].upper() != "CORMEX" for u in roster if u["owner"] == 0), "old CORE mex survived its upgrade"
    c.release()
    print("resource-core-upgrade: completed stock Moho passed")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest="command", required=True)
    prep = sub.add_parser("prepare")
    prep.add_argument("gamedir", type=Path)
    prep.add_argument("scenario", type=Path)
    prep.add_argument("--phase", type=int, default=0)
    prep.add_argument("--sides", choices=("mixed", "arm", "core"), default="mixed")
    content = sub.add_parser("supply")
    content.add_argument("gamedir", type=Path)
    core = sub.add_parser("check-core")
    core.add_argument("name")
    finish = sub.add_parser("finish-core")
    finish.add_argument("name")
    test = sub.add_parser("run")
    test.add_argument("name")
    test.add_argument("scenario", type=Path)
    test.add_argument("--out", type=Path, required=True)
    test.add_argument("--ticks", type=int, default=10000)
    watch = sub.add_parser("observe")
    watch.add_argument("name")
    watch.add_argument("--out", type=Path, required=True)
    watch.add_argument("--ticks", type=int, default=10000)
    a = p.parse_args()
    if a.command == "prepare":
        prepare(a.gamedir, a.scenario, a.phase, a.sides)
    elif a.command == "supply":
        supply(a.gamedir)
    elif a.command == "check-core":
        check_core(a.name)
    elif a.command == "finish-core":
        finish_core(a.name)
    elif a.command == "run":
        raise SystemExit(run(a.name, a.scenario, a.out, a.ticks))
    else:
        c = Stress(a.name, json.loads((a.out / "applied.json").read_text())["units"])
        c.hold()
        start = json.loads((a.out / "start.json").read_text())["tick"]
        cases = json.loads((a.out / "issued.json").read_text())
        raise SystemExit(observe(c, cases, a.out, a.ticks, start))


if __name__ == "__main__":
    main()
