#!/usr/bin/env python3
"""Record native constructor ordering with two humans and two hosted AIs.

Owns two fresh Wine instances, named with the run's tag (tools/runlock.py, TAGS) and --label, a
private display and a DirectPlay port leased for the game (tools/dpport.py): --port asks for one
port, refused while another run leases it, and with none the first free one is taken. Two games of
one run at once each take a --label of their own (tareplay.py's landing gives each its build's
letter, MP_LABELS), so they never make or remove each other's instances. tools/mp_lobby.sh types
each instance's name as its player's nickname, so every name fits the nickname (instance_names).
Only the host saves; both endpoints must contribute each constructor boundary.
This verifies capture and persisted causality, not multiplayer engine playback.

--reproduce installs tools/reproduce_fixture.py's archive on both peers (and keeps a copy in
--out for the playing), so the trees and shrubs the feature cursor visits reproduce; --until T
keeps the game running to GameTime T before the host leaves, since the cursor visits one cell
per tick from the map's last cell.

The coverage of each peer, `cover-<instance>.bin` beside the rest of --out, is written by the game's
exit (tagpu_cover_flush), so it holds the level's end and the recording's. A coverage DLL's game
that exits without writing it fails.

--chat has the two humans talk once the roots are in, through the game's own TALK box: a public
line from each and a private line each way (CHAT, below), so the recording carries every
communication audience a two-human game has.

--select has each human select all their units and click once, so the recording carries a
selection and a click from each.

--impai makes both hosted AIs ImpAI's: both peers launch with `netai=impai`, so the battle room's
first step of an AI row it adds is ImpAI (tagpu_skirmish.c, THE BATTLE ROOM) and the host's PLAYER2
and the joiner's PLAYER3 come up ImpAI's at the clicks that add them. Before the start each peer's
room must show both AI rows named `ImpAI:` (IMPAI_NAME; a name reaches the other peer through
DirectPlay, so the read waits up to LABEL_S for it); once the level is up, each peer's level init
must mark its own AI record and name the other's among the other peers' ImpAI seats (IMPAI_SEATS:
the host 2, the joiner 3). Every other check is the same game's.
--marks has each human hold the drawing board's key and draw a line and a pin (MARKS, below), and
waits until each peer has applied the other's (`recmp: remote marks`), so the recording carries
the recording machine's own marks and the ones it received.
"""
import argparse
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
import zlib

import devpy
import tarec
import tareplay
from recmp_file import History, RECMP_ROOT_CREATE_BEGIN

TREE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TREE / "tools"))
import dpport
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


COVER_LINE = "cover: replays\\cover-native-birth.bin:"

# SELGAME's NICKNAME field takes ten characters (MEASURED 2026-11-13: tacli's `ui fill` reads the
# field's maxchars as 10 and refused 'MB-S00-RAISED-H'), and tools/mp_lobby.sh types the
# instance's name, upper-cased, into it.
NICKNAME_CHARS = 10

# --impai: each peer's AI row by its instance's suffix, the name the battle room gives an ImpAI row
# (0x451310's format, ImpAI:%s), the bound on a name reaching the other peer, and the level init's
# network seat line (tagpu_impai.c): its first list is this peer's own ImpAI records, its last the
# other peers' AI seats named ImpAI:
AI_ROWS = {"H": 2, "J": 3}
IMPAI_NAME = "ImpAI:"
LABEL_S = 60
IMPAI_SEATS = re.compile(r"impai: seats (\S+) \(netai=impai, a network game, level \d+\) of \d+ records; "
                         r"other peers' AI seats (\S+), ImpAI's (\S+)")


def room_rows(snapshot):
    """{row: text} of the battle room's PLAYER<n> buttons, from a `tacli ui --json` snapshot."""
    out = {}
    for gadget in snapshot.get("gadgets") or []:
        m = re.fullmatch(r"PLAYER(\d)", gadget.get("name") or "")
        if m:
            out[int(m.group(1))] = gadget.get("text") or ""
    return out


def impai_seat_problems(texts):
    """--impai's level check on {suffix: the peer's log}: [] when each peer marked exactly its own AI
    row and named the other peer's among the other peers' ImpAI seats, else each miss by name."""
    bad = []
    for suffix, text in sorted(texts.items()):
        got = IMPAI_SEATS.findall(text)
        if len(got) != 1:
            bad.append(f"{suffix}: {len(got)} network ImpAI seat lines, not one")
            continue
        marks, _, remote_impai = got[0]
        own, other = AI_ROWS[suffix], AI_ROWS["J" if suffix == "H" else "H"]
        if marks != str(own):
            bad.append(f"{suffix}: ImpAI seats {marks}, not its own AI row {own}")
        if remote_impai != str(other):
            bad.append(f"{suffix}: the other peers' ImpAI seats {remote_impai}, not the other peer's AI row {other}")
    return bad


def instance_names(tag, label=""):
    """(host, joiner): the game's two instances, `mb-<tag>-<label>-H` and `-J`. The longest tag is
    three characters (tools/runlock.py, TAGS: `s<NN>`) and a label one (label_arg), so the longest
    name is NICKNAME_CHARS, and the tag and label keep every run's and every game's names apart."""
    token = f"mb-{tag}-" + (f"{label}-" if label else "")
    return token + "H", token + "J"


def dump_stacks(instances, owned, output):
    """Every thread's stack of each peer's Wine processes, before a failed game is stopped: a
    peer that stopped stepping (a freeze seen 2026-11-11, the joiner's private session open hung
    in its second DirectPlay join and its game thread stopped at tick 10, not reproduced since)
    then fails with its cause beside its log."""
    for name in owned:
        prefix = instances / name / "prefix"
        if not prefix.is_dir():
            continue
        with (output / f"{name}-stacks.txt").open("w") as f:
            for command in ("info proc", "bt all"):
                f.write(f"=== winedbg {command}\n")
                f.flush()
                try:
                    subprocess.run(["winedbg", "--command", command], stdout=f,
                                   stderr=subprocess.STDOUT, timeout=60,
                                   env=dict(os.environ, WINEPREFIX=str(prefix)))
                except (OSError, subprocess.SubprocessError) as error:
                    f.write(f"winedbg failed: {error}\n")


# the drawing board's gesture each human makes: hold `\`, drag a line, a middle click for a pin
MARKS = ("down:backslash", "mouse:400,300", "down:lbutton", "mouse:450,320", "mouse:500,340",
         "up:lbutton", "mouse:600,400", "down:mbutton", "up:mbutton", "up:backslash")


def run(output, dll, port, reproduce=False, until=0, chat=False, select=False, label="", refused=None,
        impai=False, allies=False, marks=False):
    """The game, its evidence in `output`; into `refused`, {peer: failure}, each peer whose GPU
    memory cap refused it an allocation (talog.cap_failure), read from its whole log once the game
    has ended, whether the game passed or not."""
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

    with dpport.lease(port) as lease:
        port = lease.port
        print(f"DirectPlay port {port}", flush=True)
        try:
            display = tareplay.start_xvfb()
            digest = hashlib.sha256(dll.read_bytes()).hexdigest()
            shutil.copyfile(dll, output / "tested-ddraw.dll")
            if dll.with_suffix(".map").exists():
                shutil.copyfile(dll.with_suffix(".map"), output / "tested-ddraw.map")
            (output / "build-sha.txt").write_text(digest+"\n")
            if reproduce:
                command(sys.executable, TREE / "tools/reproduce_fixture.py", output / "zzrepro.ufo")
            # an instance of the run's tag and this label is a dead run's: no live run shares the
            # tag, and no game of this run at once shares the label
            for suffix, name in zip(("H", "J"), instance_names(tareplay.runlock.tag(), label)):
                subprocess.run([sys.executable, str(cli), "rm", name, "--force"], stdout=log,
                               stderr=subprocess.STDOUT, check=False, timeout=120)
                owned.append(name)
                command(sys.executable, cli, "create", name, "--display", display.display, "--res", "1024x768")
                game = instances / name / "gamedir"
                shutil.copyfile(dll, game / "ddraw.dll")
                if reproduce:
                    shutil.copyfile(output / "zzrepro.ufo", game / "zzrepro.ufo")
                # the two diagnostics only a network game reaches, armed so the landing's reach check sees
                # them run: the unit stream's measurement (stream_sent, stream_recv, stream_log in
                # tagpu_patches.c) and the wire predicates' unit tests at attach (wire_selfcheck). Both
                # log and change nothing the game does.
                for lever in ("tagpu_netlog.on", "tagpu_wirecheck.on"):
                    (game / lever).touch()
                if suffix == "H":
                    (game / "tagpu_recorder.on").touch()
                elif (game / "tagpu_recorder.on").exists():
                    raise RuntimeError("saving must be off on the contributing peer")
                command(sys.executable, cli, "registry", name, "MultiCommanderDeath=0", "gamespeed=20")
                command(sys.executable, cli, "launch", name, "--keep-dll", "--dplay",
                        "--dplay-port", port, *(("--netai", "impai") if impai else ()))
                command(sys.executable, cli, "ui", name, "wait", "--gui", "MAINMENU", "--timeout", "45")
                if hashlib.sha256((game / "ddraw.dll").read_bytes()).hexdigest() != digest:
                    raise RuntimeError("fixture DLL changed")
            host, join = owned
            command("bash", TREE / "tools/mp_lobby.sh", "--map", "Two Continents",
                    *(["--allies"] if allies else []), *owned, env=dict(os.environ, MP_NO_START="1"))
            for _ in range(2):
                command(sys.executable, cli, "ui", host, "click", "PLAYER2")
            command(sys.executable, cli, "ui", join, "click", "PLAYER3")
            if impai:
                rows = {}

                def named():
                    for name in owned:
                        rows[name] = room_rows(json.loads(command(sys.executable, cli, "ui", name, "--json")))
                    return all(rows[n].get(r, "").startswith(IMPAI_NAME) for n in owned for r in AI_ROWS.values())

                try:
                    wait_for(named, "", LABEL_S)
                except RuntimeError:
                    raise RuntimeError(f"the AI rows {sorted(AI_ROWS.values())} are not both {IMPAI_NAME} on "
                                       f"both peers: {rows}")
                print(f"both AI rows {IMPAI_NAME} on both peers: {rows[host]}", flush=True)
            for name in owned:
                command(sys.executable, cli, "ui", name, "click", "READY0")
            command(sys.executable, cli, "ui", host, "click", "START", "--timeout", "30")
            for name in owned:
                command(sys.executable, cli, "wait", name, "alive=[1-9]", "--timeout", "120")
            if impai:
                bad = impai_seat_problems({s: talog.run_text(instances / n / "gamedir")
                                           for s, n in zip(AI_ROWS, owned)})
                if bad:
                    raise RuntimeError("ImpAI's network seats: " + "; ".join(bad))
                print("PASS: each peer's level marks its own AI row ImpAI's and names the other's", flush=True)
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
                # the channel's own end, not "private session open failed": a lost listener-port
                # race logs that too and opens again (tagpu_recmp.c, MP_OPEN_RACES)
                if any("recmp: contribution refused" in t for t in texts):
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
                wait_for(lambda: COVER_LINE in cursor.text(),
                         "native coverage not published", 30)
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
            if select:
                for name in owned:
                    command(sys.executable, cli, "keys", name, "ctrl+a", "pclick:512,384")
                print("each human selected and clicked", flush=True)
            if marks:
                for name in owned:
                    command(sys.executable, cli, "keys", name, *MARKS)
                wait_for(lambda: all("recmp: remote marks" in talog.run_text(instances / n / "gamedir")
                                     for n in owned), "a peer applied none of the other's marks", 60)
                print("each human drew, and each peer applied the other's marks", flush=True)

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
            for name in owned:
                game = instances / name / "gamedir"
                written = lambda: talog.run_text(game).count(COVER_LINE)
                before = written()
                # the click can end the process before tacli hears back, so the verdict is the
                # exit's own line, written after the file is closed (cover_dump_locked)
                subprocess.run([sys.executable, str(cli), "ui", name, "click", "EXIT", "--timeout", "5"],
                               stdout=log, stderr=subprocess.STDOUT, check=False, timeout=30)
                wait_for(lambda: written() > before, "the exit wrote no coverage", 30)
                shutil.copyfile(game / "replays/cover-native-birth.bin", output / f"cover-{name}.bin")
            audit(recording, output)
        except Exception:               # a failure, not an interrupt: an interrupted run stops
            dump_stacks(instances, owned, output)   # its games at once, with the run lock held
            raise
        finally:
            for name in owned:
                game = instances / name / "gamedir"
                if (instances / name / "instance.json").exists():
                    subprocess.run([sys.executable, str(cli), "stop", name], stdout=log,
                                   stderr=subprocess.STDOUT, check=False, timeout=30)
                # read once the peer has stopped, so its last lines are in it
                if (game / "log/tagpu.log").exists():
                    text = talog.run_text(game)
                    (output / (name+".log")).write_text(text)
                    why = talog.cap_failure(text, name)
                    if why and refused is not None:
                        refused[name] = why
                if (instances / name / "instance.json").exists():
                    subprocess.run([sys.executable, str(cli), "rm", name], stdout=log,
                                   stderr=subprocess.STDOUT, check=False, timeout=30)
            if display:
                tareplay.stop_xvfb(display)
            log.close()


def label_arg(value):
    """--label: one lower-case letter or digit, which keeps an instance's name inside the nickname
    (instance_names)."""
    if not re.fullmatch(r"[a-z0-9]", value):
        raise argparse.ArgumentTypeError(f"{value!r} is not one lower-case letter or digit")
    return value


def main():
    devpy.ensure()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--dll", type=Path, default=TREE / "tagpu/ddraw/ddraw-cover.dll")
    parser.add_argument("--port", type=int, default=None,
                        help="this DirectPlay port (default: the first free lease, tools/dpport.py)")
    parser.add_argument("--reproduce", action="store_true",
                        help="install the reproducing trees and shrubs on both peers")
    parser.add_argument("--until", type=int, default=0, help="play to this GameTime before leaving")
    parser.add_argument("--chat", action="store_true", help="the humans talk (CHAT)")
    parser.add_argument("--select", action="store_true", help="the humans select and click")
    parser.add_argument("--label", default="", type=label_arg,
                        help="the instances' names carry it: one each game a run plays at once")
    parser.add_argument("--impai", action="store_true",
                        help="both hosted AIs ImpAI's, by the battle room's first step (netai=impai)")
    parser.add_argument("--marks", action="store_true", help="the humans draw on the board (MARKS)")
    parser.add_argument("--allies", action="store_true",
                        help="the two humans allied both ways (each still fights its own AI): the "
                             "allies' ribbon reads each other's economy from the channel")
    args = parser.parse_args()
    tareplay.runlock.hold(TREE, "recmp_create_begin_test " + " ".join(sys.argv[1:]), 2,
                          suite="replay")  # host, joiner
    why = tareplay.runlock.together(2, "recmp_create_begin_test")      # they meet in one lobby
    if why:
        print(f"could not run: {why}", file=sys.stderr)
        return 2
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=False)
    print(f"evidence: {output}", flush=True)
    refused, failed = {}, False
    try:
        run(output, args.dll.resolve(), args.port, args.reproduce, args.until, args.chat, args.select,
            args.label, refused, args.impai, args.allies, args.marks)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, tareplay.CouldNotRun) as error:
        print(f"FAIL: {error}; inspect {output}", file=sys.stderr, flush=True)
        failed = True
    # each peer's refusals last: the line a caller reads as the game's failure names them
    for why in refused.values():
        print(f"FAIL: {why}; inspect {output}", file=sys.stderr, flush=True)
    return 1 if failed or refused else 0


if __name__ == "__main__":
    sys.exit(main())
