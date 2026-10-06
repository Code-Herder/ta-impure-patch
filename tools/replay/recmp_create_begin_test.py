#!/usr/bin/env python3
"""Record native constructor ordering with two humans and two hosted AIs.

Owns two fresh Wine instances, a private display and the chosen DirectPlay port.
Only the host saves; both endpoints must contribute each constructor boundary.
This verifies capture and persisted causality, not multiplayer engine playback.

--reproduce installs tools/reproduce_fixture.py's archive on both peers (and keeps a copy in
--out for the playing), so the trees and shrubs the feature cursor visits reproduce; --until T
keeps the game running to GameTime T before the host leaves, since the cursor visits one cell
per tick from the map's last cell.

--chat has the two humans talk once the roots are in, through the game's own TALK box: a public
line from each and a private line each way (CHAT, below), so the recording carries every
communication audience a two-human game has.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time
import uuid
import zlib

import tarec
import tareplay
from recmp_file import History, RECMP_ROOT_CREATE_BEGIN

TREE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TREE / "tools"))
import talog


def audit(path, output):
    blocks, stopped = tarec.blocks(path)
    if stopped:
        raise RuntimeError(stopped)
    segment = tarec.segments(blocks)[0]
    if segment["header"]["mode"] != 3 or segment["end_reason"] != 1:
        raise RuntimeError("native recording is not complete")
    history = History(segment["header"])
    for kind, _, _, raw in blocks:
        if kind == tarec.RB_CONTRIBUTIONS:
            history.block(raw)
        elif kind == tarec.RB_END:
            footers = [b for tag, b in tarec.entries(raw) if tag == tarec.RT_MP_END]
            if len(footers) != 1:
                raise RuntimeError("native recording has no unique contribution footer")
            history.footer(footers[0])
    begins, ends, children, packets = {}, {}, [], {}
    crc, size = 0, 0
    for _, raw in history.applications:
        crc, size = zlib.crc32(raw, crc), size + len(raw)
        ordinal, applied, phase = struct.unpack_from("<QII", raw)
        _, origin, source, sequence, kind = struct.unpack_from("<IIIII", raw, 28)
        if kind != 10:
            continue
        packets[origin, sequence] = (source, raw[80:])
        count = struct.unpack_from("<I", raw, 80)[0]
        at = 84
        for index in range(count):
            root, span, before, _, birth = struct.unpack_from("<HHB3sI", raw, at)
            at += 12
            image = raw[at:at+span]
            at += span
            key = (origin, struct.unpack_from("<I", image)[0]-1, birth)
            event = dict(ordinal=ordinal, index=index, source=source, applied=applied,
                         sequence=sequence, phase=phase, before=before, image=image.hex())
            if root == RECMP_ROOT_CREATE_BEGIN:
                if key in begins:
                    raise RuntimeError("repeated constructor begin for one incarnation")
                begins[key] = event
            elif root == tarec.RK_EV_CREATE:
                begin = begins.get(key)
                if not begin or key in ends:
                    raise RuntimeError("constructor success has no preceding unique begin")
                if any(begin[field] != event[field] for field in ("source", "before", "image")):
                    raise RuntimeError("constructor arguments, water, source tick or phase changed")
                ends[key] = event
            elif key in begins and key not in ends:
                children.append(dict(origin=origin, slot=key[1], birth=birth, kind=root, **event))
        if at != len(raw):
            raise RuntimeError("owner root batch has trailing bytes")
    actors = {a[0] for a in history.roster.actors}
    if not begins or begins.keys() != ends.keys() or {k[0] for k in begins} != actors:
        raise RuntimeError("begin/success pairs do not cover every human and hosted AI")
    if not children:
        raise RuntimeError("no constructor child root observed between begin and success")
    observations = json.loads((output / "first-roots.json").read_text())
    for observation in observations:
        source, sequence, count, span, checksum, index, kind, unit, birth, before = observation["observation"]
        persisted = packets.get((int(observation["origin"]), int(sequence)))
        if not persisted:
            raise RuntimeError("native first root observation missing from application ledger")
        tick, body = persisted
        if (tick != int(source) or len(body) != int(span) or
                zlib.crc32(body) != int(checksum, 16) or
                struct.unpack_from("<I", body)[0] != int(count)):
            raise RuntimeError("persisted root batch differs from native source/recipient bytes")
        at = 4
        for _ in range(int(index)):
            at += 12+struct.unpack_from("<H", body, at+2)[0]
        root, _, phase, _, stamp = struct.unpack_from("<HHB3sI", body, at)
        if (root, stamp, phase, struct.unpack_from("<I", body, at+12)[0]) != \
                (int(kind), int(birth), int(before), int(unit)):
            raise RuntimeError("persisted root identity/index differs from native observation")
    pairs = [dict(origin=k[0], slot=k[1], birth=k[2], begin=begins[k], end=ends[k])
             for k in begins]
    (output / "constructor-pairs.json").write_text(json.dumps(pairs, indent=2)+"\n")
    (output / "constructor-children.json").write_text(json.dumps(children, indent=2)+"\n")
    summary = dict(bytes=path.stat().st_size, first=segment["first"], last=segment["last"],
                   actors=len(actors), constructors=len(pairs), children=len(children),
                   applications=history.ledger.records, application_bytes=size,
                   application_crc=f"{crc:08X}")
    (output / "summary.json").write_text(json.dumps(summary, indent=2)+"\n")
    print("PASS: exact owner constructor begin precedes child roots and matching success", summary, flush=True)


# who says what, in order: a digit selects the n-th other player of the talk box's list, which
# skips the sender's own row (0x493AE0), so `1:` is the other human for both (rows 0 and 1)
CHAT = (("join", "public from the joiner"), ("host", "1:private to the joiner"),
        ("join", "1:private to the host"), ("host", "public from the host"))


def run(output, dll, port, reproduce=False, until=0, chat=False):
    root = tareplay.main_checkout()
    instances, cli = root / "tagpu/instances", TREE / "tools/tacli"
    owned, display = [], None
    log = (output / "driver.log").open("w")

    def command(*args, env=None, timeout=180):
        result = subprocess.run(list(map(str, args)), capture_output=True, text=True,
                                env=env, timeout=timeout)
        log.write(result.stdout+result.stderr)
        log.flush()
        if result.returncode:
            raise RuntimeError(f"command failed ({result.returncode}): {args}; see driver.log")
        return result.stdout

    def wait_for(predicate, description, timeout=90):
        deadline = time.monotonic()+timeout
        while not predicate():
            if time.monotonic() >= deadline:
                raise RuntimeError(description)
            time.sleep(0.1)

    with Path(f"/tmp/tacli-dplay-{port}.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            display = tareplay.start_xvfb()
            digest = hashlib.sha256(dll.read_bytes()).hexdigest()
            shutil.copyfile(dll, output / "tested-ddraw.dll")
            if dll.with_suffix(".map").exists():
                shutil.copyfile(dll.with_suffix(".map"), output / "tested-ddraw.map")
            (output / "build-sha.txt").write_text(digest+"\n")
            if reproduce:
                command(sys.executable, TREE / "tools/reproduce_fixture.py", output / "zzrepro.ufo")
            token = "mb"+uuid.uuid4().hex[:6]
            for suffix in ("H", "J"):
                name = token+suffix
                if (instances / name).exists():
                    raise RuntimeError("fixture instance already exists")
                owned.append(name)
                command(sys.executable, cli, "create", name, "--private-registry",
                        "--display", display.display, "--res", "1024x768")
                game = instances / name / "gamedir"
                shutil.copyfile(dll, game / "ddraw.dll")
                if reproduce:
                    shutil.copyfile(output / "zzrepro.ufo", game / "zzrepro.ufo")
                if suffix == "H":
                    (game / "tagpu_recorder.on").touch()
                elif (game / "tagpu_recorder.on").exists():
                    raise RuntimeError("saving must be off on the contributing peer")
                command(sys.executable, cli, "registry", name, "MultiCommanderDeath=0", "gamespeed=20")
                command(sys.executable, cli, "launch", name, "--keep-dll", "--dplay",
                        "--dplay-port", port)
                command(sys.executable, cli, "ui", name, "wait", "--gui", "MAINMENU", "--timeout", "45")
                if hashlib.sha256((game / "ddraw.dll").read_bytes()).hexdigest() != digest:
                    raise RuntimeError("fixture DLL changed")
            host, join = owned
            command("bash", TREE / "tools/mp_lobby.sh", "--map", "Two Continents", *owned,
                    env=dict(os.environ, MP_NO_START="1"))
            for _ in range(2):
                command(sys.executable, cli, "ui", host, "click", "PLAYER2")
            command(sys.executable, cli, "ui", join, "click", "PLAYER3")
            for name in owned:
                command(sys.executable, cli, "ui", name, "click", "READY0")
            command(sys.executable, cli, "ui", host, "click", "START", "--timeout", "30")
            for name in owned:
                command(sys.executable, cli, "wait", name, "alive=[1-9]", "--timeout", "120")
            for name, unit_type, x, ai in ((host, "ARMFLASH", 2300, 2), (join, "CORAK", 2460, 3)):
                roster = json.loads(command(sys.executable, cli, "roster", name, "--json"))
                bases = {u["owner"]: u["world"] for u in roster["units"] if u["type"].upper() == "ARMCOM"}
                hx, hz = bases[0][:2]
                ax, az = bases[ai][:2]
                scenario = dict(format="ta-scenario/1", seed=20261005, on_error="abort",
                    setup=dict(clear_existing=False, switches=dict(shootall=True, noshake=True)),
                    groups=[dict(id="human", owner=0, at=[x, 1200],
                                 pattern=dict(kind="grid", cols=5, spacing=32),
                                 composition=[dict(type=unit_type, count=20)], stance="hold"),
                            dict(id="ai", owner=ai, at=[x, 1400],
                                 pattern=dict(kind="grid", cols=5, spacing=32),
                                 composition=[dict(type=unit_type, count=10)], stance="hold")],
                    units=[dict(id="human_builder", type="ARMCOM", owner=0, pos=[hx+256, hz],
                                orders=[dict(cmd="reclaim", target="human_solar")]),
                           dict(id="human_solar", type="ARMSOLAR", owner=0, pos=[hx+352, hz]),
                           dict(id="ai_builder", type="ARMCOM", owner=ai, pos=[ax+256, az],
                                orders=[dict(cmd="reclaim", target="ai_solar")]),
                           dict(id="ai_solar", type="ARMSOLAR", owner=ai, pos=[ax+352, az])])
                path = output / (name+".json")
                path.write_text(json.dumps(scenario))
                command(sys.executable, cli, "scenario", "apply", name, path, "--json")
            print("four owner actors active; waiting for seven root kinds per actor", flush=True)

            def contributed():
                texts = [talog.run_text(instances / n / "gamedir") for n in owned]
                if any("recmp: contribution refused" in t or "recmp: private session open failed" in t for t in texts):
                    raise RuntimeError("native contributions failed")
                return all(t.count("recmp: local owned roots") == 14 and
                           t.count("recmp: remote owned roots") == 14 for t in texts)

            wait_for(contributed, "missing begin/create/death/return/script/reply/spray observations")
            local, remote = {}, {}
            pattern = r"(local|remote) owned roots ID (\d+) source tick (\d+) seq (\d+) applied tick (\d+), (\d+) calls (\d+) bytes crc ([0-9A-F]+), event (\d+) kind (\d+) unit (\d+) birth (\d+) before (\d+)"
            for name in owned:
                text = talog.run_text(instances / name / "gamedir")
                for fields in re.findall(pattern, text):
                    direction, origin, source, sequence, applied, *body = fields
                    target = local if direction == "local" else remote
                    key = (origin, body[4])
                    if key in target:
                        raise RuntimeError("duplicate first owner root observation")
                    target[key] = (source, sequence, *body)
                game = instances / name / "gamedir"
                cursor = talog.Cursor(game)
                (game / "tagpu_replay.trigger").write_text("cover native-birth\n")
                wait_for(lambda: "cover: replays\\cover-native-birth.bin:" in cursor.text(),
                         "native coverage not published", 30)
                shutil.copyfile(game / "replays/cover-native-birth.bin", output / (name+"-cover.bin"))
            if len(local) != 28 or local != remote:
                raise RuntimeError("source/recipient root bytes or identity differ")
            (output / "first-roots.json").write_text(json.dumps(
                [dict(origin=k[0], kind=k[1], observation=v) for k, v in local.items()], indent=2)+"\n")
            print("PASS: all 28 first owner root observations match recipient bytes and source provenance", flush=True)
            if chat:
                who = dict(host=host, join=join)
                for speaker, text in CHAT:
                    name = who[speaker]
                    command(sys.executable, cli, "keys", name, "return")
                    command(sys.executable, cli, "ui", name, "wait", "--gui", "TALK", "--timeout", "20")
                    command(sys.executable, cli, "ui", name, "fill", "TALK", text, "--json")
                    command(sys.executable, cli, "keys", name, "return")
                    command(sys.executable, cli, "ui", name, "wait", "--gui", "ARMMAIN2", "--timeout", "20")
                print(f"said {len(CHAT)} lines", flush=True)

            def gametime():
                out = command(sys.executable, cli, "peek", host, tareplay.GAMETIME, "--json", timeout=60)
                return int(str(json.loads(out[out.index("{"):])["peeks"][0]["value"]).split()[0])

            wait_for(lambda: gametime() >= until, f"the game did not reach GameTime {until}", 1800)
            command("bash", TREE / "tools/mp_leave.sh", host)
            wait_for(lambda: " closed:" in talog.run_text(instances / host / "gamedir"),
                     "native writer did not close", 30)
            files = list((instances / host / "gamedir/replays").glob("*.imprec"))
            if len(files) != 1 or list((instances / join / "gamedir/replays").glob("*.imprec")):
                raise RuntimeError("host save/peer opt-out mismatch")
            recording = output / "native.imprec"
            shutil.copyfile(files[0], recording)
            gui = json.loads(command(sys.executable, cli, "ui", join, "--json"))["gui"].upper()
            if gui == "ENDMSN.GUI":
                command(sys.executable, cli, "ui", join, "click", "MainMenu", "--timeout", "30")
            elif gui != "MAINMENU.GUI":
                command("bash", TREE / "tools/mp_leave.sh", join)
            for name in owned:
                command(sys.executable, cli, "ui", name, "wait", "--gui", "MAINMENU", "--timeout", "30")
            audit(recording, output)
        finally:
            for name in owned:
                game = instances / name / "gamedir"
                if (game / "log/tagpu.log").exists():
                    (output / (name+".log")).write_text(talog.run_text(game))
                if (instances / name / "instance.json").exists():
                    subprocess.run([sys.executable, str(cli), "stop", name], stdout=log,
                                   stderr=subprocess.STDOUT, check=False, timeout=30)
                    subprocess.run([sys.executable, str(cli), "rm", name], stdout=log,
                                   stderr=subprocess.STDOUT, check=False, timeout=30)
            if display:
                tareplay.stop_xvfb(display)
            log.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--dll", type=Path, default=TREE / "tagpu/ddraw/ddraw-cover.dll")
    parser.add_argument("--port", type=int, default=47818)
    parser.add_argument("--reproduce", action="store_true",
                        help="install the reproducing trees and shrubs on both peers")
    parser.add_argument("--until", type=int, default=0, help="play to this GameTime before leaving")
    parser.add_argument("--chat", action="store_true", help="the humans talk (CHAT)")
    args = parser.parse_args()
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=False)
    print(f"evidence: {output}", flush=True)
    try:
        run(output, args.dll.resolve(), args.port, args.reproduce, args.until, args.chat)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, tareplay.CouldNotRun) as error:
        print(f"FAIL: {error}; inspect {output}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
