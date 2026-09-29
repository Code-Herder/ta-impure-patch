#!/usr/bin/env python3
"""Execute the eight required COB getters in an isolated Escalation skirmish.

    python3 tools/cob_getter_probe.py --out /tmp/cob-getters --expect stock-zero
    python3 tools/cob_getter_probe.py --out /tmp/cob-getters-port --expect implemented

The implemented mode checks the guarded runtime. A generated unit uses
the mod's solar model/FBI, with an entirely generated script. Nothing is installed
in the player's mod. Both the human and local AI get a probe. This covers ordinary
queries; --extended adds signed-ID bounds, kills, build progress, a 102-word
stack and a 100-argument call. --roundtrip checks expanded saved records;
--quarantine checks exclusion and the native chat message. Multiplayer is separate.
The output contains local paths and derived mod assets: keep it outside the repo.
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
import uuid

import cob_audit
import hpipack
import talog

sys.path.insert(0, str(Path(__file__).parent / "compat"))
import tacompat as compat

GETTERS = (32, 69, 70, 71, 72, 73, 74, 75)
TYPE = "COBFPROBE"
BOUND_ARGS = (-2147483648, -1, 0, 65535, 65536, 65537, 2147483647, None)


def probe_cob(pieces, extended=False, roundtrip=False, thread_exhaustion=None):
    """Sequential CALLs need two records, with no dependency on extended getters."""
    op = cob_audit.tacob.OP
    code = [op["PUSH_CONSTANT"], 1000, op["SLEEP"]]
    extra = 2 + 4 * len(BOUND_ARGS) if extended else 0
    for index in range(1, len(GETTERS) + extra + 1):
        argc = 100 if extended and index == len(GETTERS) + 2 else 0
        for argument in range(argc):
            code += [op['PUSH_CONSTANT'], (0x80000000 + argument)]
        code += [op["CALL_SCRIPT"], index, argc]
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
    if extended:
        scripts.append(("Probe900", len(code)))
        code += [op["CREATE_LOCAL_VAR"]] * 101
        code += [op["PUSH_CONSTANT"], 9876, op["POP_LOCAL_VAR"], 100]
        if roundtrip:
            code += [op['PUSH_CONSTANT'], 30000, op['SLEEP']]
        code += [op["PUSH_LOCAL_VAR"], 100, op["RETURN"]]
        scripts.append(('Probe901', len(code)))
        code += [op['CREATE_LOCAL_VAR']] * 100
        code += [op['PUSH_LOCAL_VAR'], 99, op['RETURN']]
        for getter in range(72, 76):
            for index, argument in enumerate(BOUND_ARGS):
                scripts.append((f"Probe{getter * 100 + index}", len(code)))
                code += [op["PUSH_CONSTANT"], getter]
                if argument is None:
                    code += [op["PUSH_CONSTANT"], 70, op["GET_UNIT_VALUE"],
                             op["PUSH_CONSTANT"], 1, op["ADD"]]
                else:
                    code += [op["PUSH_CONSTANT"], argument & 0xffffffff]
                code += [op["PUSH_CONSTANT"], 0] * 3 + [op["GET"], op["RETURN"]]
    if thread_exhaustion:
        worker = len(scripts)
        prefix = [op['PUSH_CONSTANT'], 1, op['SET_SIGNAL_MASK']]
        prefix += [op['START_SCRIPT'], worker, 0] * 7
        prefix += [op['PUSH_CONSTANT'], 0, op['SET_SIGNAL_MASK']]
        prefix += [op['PUSH_CONSTANT'], 123] * 128
        prefix += [op['START_SCRIPT'], worker, 128]
        if thread_exhaustion == 'call':
            prefix += [op['CALL_SCRIPT'], worker, 0]
        # With a leaked START argument the SIGNAL push exceeds the allocation.
        # With correct consumption it kills the workers and frees the slots for
        # the ordinary getter calls that follow.
        prefix += [op['PUSH_CONSTANT'], 1, op['SIGNAL']]
        code = prefix + code
        scripts = [(name, pc + len(prefix) if pc else 0) for name, pc in scripts]
        scripts.append(('CapacityWorker', len(code)))
        code += [op['PUSH_CONSTANT'], 60000, op['SLEEP'], op['PUSH_CONSTANT'], 0, op['RETURN']]
    return cob_audit.tacob.write_cob(cob_audit.tacob.Cob(scripts, pieces, 0, code))


def fixture(archive, extended=False, roundtrip=False, quarantine=False, thread_exhaustion=None,
            skirmish_ceiling=False, unused_model_pieces=False):
    original = cob_audit.tacob.read_cob(archive.read("scripts/armsolar.cob"))
    if unused_model_pieces:
        original.pieces += [f'unused{i}' for i in range(4096 - len(original.pieces))]
    fbi = archive.read("unitse/armsolar.fbi").decode("latin-1")
    fbi, changed = re.subn(r"(?im)(\bUnitName\s*=\s*)[^;]+;", rf"\g<1>{TYPE};", fbi)
    if changed != 1:
        raise ValueError("solar FBI must have exactly one UnitName")
    files = {"unitse": {TYPE + ".fbi": fbi.encode("latin-1")},
             "scripts": {TYPE + ".cob": probe_cob(original.pieces, extended, roundtrip, thread_exhaustion)}}
    if skirmish_ceiling:
        files['unitse']['COBFDUMMY.fbi'] = fbi.replace(TYPE, 'COBFDUMMY').encode('latin-1')
        op = cob_audit.tacob.OP
        files['scripts']['COBFDUMMY.cob'] = cob_audit.tacob.write_cob(
            cob_audit.tacob.Cob([('Create', 0)], original.pieces, 0,
                              [op['PUSH_CONSTANT'], 0, op['RETURN']]))
    if quarantine:
        files['unitse']['COBFBAD.fbi'] = fbi.replace(TYPE, 'COBFBAD').encode('latin-1')
        op = cob_audit.tacob.OP
        files['scripts']['COBFBAD.cob'] = cob_audit.tacob.write_cob(
            cob_audit.tacob.Cob([('Create', 0)], original.pieces, 0,
                              [op['PUSH_LOCAL_VAR'], 128, op['RETURN']]))
        if quarantine == 'commander':
            files['scripts']['ARMCOM.cob'] = files['scripts']['COBFBAD.cob']
    return hpipack.build(files)


def returns(trace):
    if 'INCOMPLETE' in trace:
        raise ValueError('incomplete COB trace cannot establish behavior')
    rows = {}
    for line in trace.splitlines():
        fields = line.split("\t")
        if len(fields) == 6 and fields[0] == "R" and re.fullmatch(r"Probe\d+", fields[4]):
            key = (int(fields[2]), int(fields[4][5:]))
            if key in rows:
                raise ValueError(f"duplicate probe return: {key}")
            rows[key] = int(fields[5])
    return rows


def arguments_match(trace, units):
    expected = ','.join(str(-2147483648 + i) for i in range(100))
    found = set()
    for line in trace.splitlines():
        fields = line.split('\t')
        if len(fields) == 8 and fields[0] == 'S' and fields[4] == 'Probe901':
            if fields[7] != expected:
                return False
            found.add(int(fields[2]))
    return found == set(units)


def judge(rows, units, slots, mode, extended=False):
    checks = []
    for unit, owner in units.items():
        expected = {32: 0, 69: 1, 70: slots - 1, 71: unit, 72: owner,
                    73: 0, 74: 1, 75: 1}
        if extended:
            expected.update({32: (7 + owner) * 100, 73: 50, 900: 9876, 901: -2147483549})
            for getter in range(72, 76):
                for index, argument in enumerate(BOUND_ARGS):
                    want = -1 if getter in (72, 73) else 0
                    expected[getter * 100 + index] = want
        for getter in expected:
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


def reload_probe(name, inst, game, archive, extended, roundtrip, unused_model_pieces, out):
    """A unique unreachable tail proves the native reload actually took place."""
    original = cob_audit.tacob.read_cob(archive.read('scripts/armsolar.cob'))
    pieces = original.pieces
    if unused_model_pieces:
        pieces += [f'unused{i}' for i in range(4096 - len(pieces))]
    cob = cob_audit.tacob.read_cob(probe_cob(pieces, extended, roundtrip))
    marker = 0x1b00b1e5
    cob.code.append(marker)
    scripts = inst['gamedir'] / 'scripts'
    scripts.mkdir(exist_ok=True)
    (scripts / (TYPE + '.cob')).write_bytes(cob_audit.tacob.write_cob(cob))
    command('switches', name, 'cheats=on')
    command('keys', name, 'shift', 'return')
    command('shot', name, '-o', str(out / 'reload-open.png'))
    text = '+reload ' + TYPE.lower()
    for start in range(0, len(text), 4):
        tokens = ['space' if c == ' ' else 'char:' + c for c in text[start:start + 4]]
        command('keys', name, 'shift', *tokens)
        command('shot', name, '-o', str(out / f'reload-typed-{start}.png'))
    command('keys', name, 'shift', 'return')
    hooks = compat.exe_hooks(game['proc'].pid, inst['gamedir'])
    if hooks.get('why') or compat.hook_evidence(hooks):
        raise RuntimeError('reload requires confirmed Impure-only runtime')
    deadline = time.monotonic() + 15
    with open(f"/proc/{hooks['pid']}/mem", 'rb') as memory:
        def word(address):
            memory.seek(address)
            return struct.unpack('<I', memory.read(4))[0]
        while time.monotonic() < deadline:
            main = word(0x511DE8)
            count, base = word(main + 0x1438F), word(main + 0x1439B)
            if not 0 < count <= 16384:
                raise RuntimeError('reload definition count exceeds allocation')
            for index in range(1, count):
                definition = base + index * 0x249
                memory.seek(definition + 32)
                if memory.read(32).split(b'\0')[0].upper() != TYPE.encode():
                    continue
                pointer = word(definition + 0x18E)
                words = word(pointer + 12) if pointer else 0
                if words == len(cob.code) and word(word(pointer + 36) + (words - 1) * 4) == marker:
                    return dict(observed=True, words=words, pieces=len(pieces))
            time.sleep(.1)
    raise RuntimeError('native reload did not publish the marked script')


def skirmish_ceiling_scenario():
    """Keep the native commander; fill its last player's remaining 1499 slots."""
    units = [dict(id='human', type=TYPE, owner=0, pos=[2600, 1200], kills=7, nanoframe=50)]
    units += [dict(id=f'filler{i}', type='COBFDUMMY', owner=3,
                   pos=[640 + (i % 40) * 48, 640 + (i // 40) * 48])
              for i in range(1498)]
    units.append(dict(id='ai', type=TYPE, owner=3, pos=[4200, 2400], kills=10, nanoframe=50))
    return dict(format='ta-scenario/1', seed=7, on_error='abort',
                setup=dict(clear_existing=False), units=units)


def configure_skirmish_ceiling(inst):
    """Only the new instance's copied ini and private registry are changed."""
    # Escalation's executable names taesc.ini and Software\TA Esc; these are
    # executable settings, not TADR behavior (deep-ta-esc.md).
    ini = next(p for p in inst['gamedir'].iterdir() if p.name.lower() == 'taesc.ini')
    if ini.is_symlink():
        raise RuntimeError('ceiling fixture requires a private ini copy')
    content, count = re.subn(r'(?i)(UnitLimit\s*=\s*)\d+', r'\g<1>1500', ini.read_text())
    if count == 0:
        content, count = re.subn(r'(?im)^\[Preferences\]\s*$',
                                lambda m: m[0] + '\nUnitLimit=1500', content)
    if count != 1:
        raise RuntimeError('expected one UnitLimit or Preferences section in the private ini')
    ini.write_text(content)
    env = dict(os.environ, WINEPREFIX=str(inst['prefix']))
    for owner in range(4):
        for key, value in [('Controller', 1 if owner == 0 else 2), ('Side', 0),
                           ('Metal', 0), ('Energy', 0)]:
            subprocess.run(['wine', 'reg', 'add',
                            r'HKCU\Software\TA Esc\Total Annihilation\Skirmish',
                            '/v', f'Player{owner}{key}', '/t', 'REG_DWORD', '/d', str(value), '/f'],
                           env=env, capture_output=True, check=True, timeout=120)
    subprocess.run(['wineserver', '-w'], env=env, timeout=60)


def enter_skirmish(name, failure_path=None):
    def check_failure():
        if failure_path and failure_path.exists():
            raise RuntimeError(failure_path.read_text(errors='replace'))

    for gadget in ("SINGLE", "Skirmish", "Start"):
        deadline = time.monotonic() + 60
        while True:
            check_failure()
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
        check_failure()
        ui = compat.tacli("ui", name, timeout=20).stdout
        if compat.IN_GAME_PANEL.search((ui.splitlines() or [""])[0]):
            return
        time.sleep(1)
    raise RuntimeError("skirmish never reached the game screen")


def active_records(pid, units):
    """Read the production layout while the game is paused; no state is written."""
    result = {}
    with open(f'/proc/{pid}/mem', 'rb') as memory:
        def read(address, size):
            memory.seek(address)
            data = memory.read(size)
            if len(data) != size:
                raise RuntimeError('short process-memory read')
            return data
        main = struct.unpack('<I', read(0x511DE8, 4))[0]
        if read(main + 0x38A51, 1) != b'\x01':
            raise RuntimeError('save-state snapshot requires the game to be paused')
        base = struct.unpack('<I', read(main + 0x14357, 4))[0]
        for unit in units:
            cob = struct.unpack('<I', read(base + unit * 0x118 + 0x9A, 4))[0]
            records = []
            for slot in range(8):
                words = list(struct.unpack('<137I', read(cob + 28 + slot * 548, 548)))
                if words[0]:
                    # Callback addresses are deliberately not persisted. Sleep
                    # countdown can advance between a menu action and pausing.
                    words[3] = words[8] = 0
                    records.append(dict(slot=slot, words=words))
            if not any(r['words'][0] == 0x02400000 and r['words'][2] == 100 and
                       r['words'][109] == 9876 for r in records):
                raise RuntimeError(f'unit {unit} is not sleeping on its expanded stack')
            result[unit] = records
    return result


def wait_paused(pid):
    deadline = time.monotonic() + 10
    with open(f'/proc/{pid}/mem', 'rb') as memory:
        while time.monotonic() < deadline:
            memory.seek(0x511DE8)
            main = struct.unpack('<I', memory.read(4))[0]
            memory.seek(main + 0x38A51)
            if memory.read(1) == b'\x01':
                return
            time.sleep(.05)
    raise RuntimeError('game did not acknowledge pause')


def open_options(name):
    """Enter options once, acknowledging panels rather than assuming key timing."""
    deadline = time.monotonic() + 15
    tab_sent = options_clicked = False
    while time.monotonic() < deadline:
        panel = command('ui', name).splitlines()[0]
        if re.match(r'gui (ARM|COR)OPT\.GUI\b', panel):
            return
        if 'TABMENU.GUI' in panel and not options_clicked:
            command('ui', name, 'click', 'OPTIONS')
            options_clicked = True
        elif compat.IN_GAME_PANEL.search(panel) and not tab_sent:
            command('keys', name, 'tab', 'tab')
            tab_sent = True
        time.sleep(.1)
    raise RuntimeError(f'options transition not acknowledged: {panel}')


def save_roundtrip(name, game, inst, units, out):
    save_name = 'C'

    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        trace = talog.run_text(inst['gamedir'], 'tagpu_cobtrace')
        if all(re.search(rf'^S\t\d+\t{u}\t[^\n]*\tProbe900\t', trace, re.M) for u in units):
            break
        time.sleep(.2)
    else:
        raise RuntimeError('expanded-stack sleep was not reached')
    command('keys', name, 'shift', 'pause')
    hooks = compat.exe_hooks(game['proc'].pid, inst['gamedir'])
    if hooks.get('why'):
        raise RuntimeError(hooks['why'])
    wait_paused(hooks['pid'])
    before = active_records(hooks['pid'], units)
    open_options(name)
    command('ui', name, 'click', 'SAVEGAME')
    command('ui', name, 'click', 'GAMENAME')
    filled = json.loads(command('ui', name, 'fill', 'GAMENAME', save_name, '--json'))
    if filled.get('text') != save_name:
        raise RuntimeError(f'save name was not acknowledged: {filled}')
    command('ui', name, 'click', 'LOAD')
    print('Save command acknowledged; opening load menu', flush=True)
    open_options(name)
    command('ui', name, 'click', 'LOADGAME')
    command('ui', name, 'select', 'GAMES', save_name)
    command('ui', name, 'click', 'LOAD', timeout=90)
    print('Load command acknowledged; comparing script records', flush=True)
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        ui = command('ui', name)
        if compat.IN_GAME_PANEL.search((ui.splitlines() or [''])[0]):
            break
        time.sleep(.5)
    wait_paused(hooks['pid'])
    after = active_records(hooks['pid'], units)
    evidence = dict(before=before, after=after, ok=before == after)
    (out / 'save-roundtrip.json').write_text(json.dumps(evidence, indent=2))
    if not evidence['ok']:
        raise RuntimeError('expanded script records changed across save/load')
    command('keys', name, 'shift', 'pause')
    return evidence


def run(out, mode, dll, extended=False, roundtrip=False, quarantine=False, thread_exhaustion=None,
        skirmish_ceiling=False, unused_model_pieces=False, reload=False):
    setup = compat.pick_setups(["escalation"])[0]
    problems = [p for entry in setup["add"] for p in compat.fixture_problems(entry["fixture"])]
    if problems:
        raise RuntimeError(str(problems))
    compat.PREFIX = "cobf-" + uuid.uuid4().hex[:8] + "-"
    name = compat.inst_name(setup["name"])
    if (compat.INSTANCES / name).exists():
        raise RuntimeError("probe instance already exists; refusing to replace it")
    display_server = compat.start_xvfb()
    display = display_server.tacompat_display
    game, inst = None, None
    report = dict(mode=mode, instance=name, setup="escalation", dll_sha256=
                  hashlib.sha256(dll.read_bytes()).hexdigest())
    try:
        print(f"Preparing {name}", flush=True)
        inst = compat.prepare_wine(setup, dll, display)
        archive = hpipack.Archive(compat.FIXTURES / "escalation-gold-10.2.0/TAESC.gp3")
        (inst["gamedir"] / "cob-getter-probe.ufo").write_bytes(
            fixture(archive, extended, roundtrip, quarantine, thread_exhaustion,
                    skirmish_ceiling, unused_model_pieces))
        if skirmish_ceiling:
            configure_skirmish_ceiling(inst)
        if quarantine == 'commander':
            # Existing archived script paths are overridden only by loose COBs;
            # this is confined to the fresh test instance, never the mod install.
            scripts = inst['gamedir'] / 'scripts'
            scripts.mkdir(exist_ok=True)
            op = cob_audit.tacob.OP
            (scripts / 'ARMCOM.cob').write_bytes(cob_audit.tacob.write_cob(
                cob_audit.tacob.Cob([('Create', 0)], ['base'], 0,
                                  [op['PUSH_LOCAL_VAR'], 128, op['RETURN']])))
        (inst["gamedir"] / "tagpu_cobtrace.on").write_text(TYPE)
        print("Starting Escalation and entering skirmish", flush=True)
        game = compat.start_wine(inst, display, display_server=display_server)
        capture_window = None
        if quarantine is True:
            instances = json.loads(command('ls', '--json', timeout=90))
            capture_window = next(i['window'] for i in instances if i['name'] == name)
            if not capture_window:
                raise RuntimeError('probe window has not been acknowledged for capture')
        failure_path = inst['gamedir'] / 'log/startup-failure.txt'
        try:
            enter_skirmish(name, failure_path)
        except RuntimeError:
            if quarantine != 'commander' or not failure_path.exists():
                raise
            failure = failure_path.read_text(errors='replace')
            report['required_refusal'] = failure
            report['ok'] = all(t in failure for t in
                               ('required or saved unit', 'ARMCOM', 'local index'))
            return report
        if quarantine == 'commander':
            raise RuntimeError('match started despite a rejected starting commander')
        if reload:
            print('Reloading the generated script through native chat', flush=True)
            report['reload'] = reload_probe(name, inst, game, archive, extended, roundtrip, unused_model_pieces, out)
        if quarantine is True:
            deadline = time.monotonic() + 15
            while 'cob: entry message:' not in talog.run_text(inst['gamedir']):
                if time.monotonic() >= deadline:
                    raise RuntimeError('entry message was not published for capture')
                time.sleep(.1)
            subprocess.run(['import', '-window', str(capture_window[0]), str(out / 'entry-chat.png')],
                           env=dict(os.environ, DISPLAY=f':{display}'), check=True, timeout=15)
        content = json.loads(command("units", name, "--json"))
        (out / "units.json").write_text(json.dumps(content, indent=2))
        scenario = dict(format="ta-scenario/1", seed=7, on_error="abort",
                        setup=dict(clear_existing=True), units=[
                            dict(id="human", type=TYPE, owner=0, pos=[2600, 1200]),
                            dict(id="ai", type=TYPE, owner=1, pos=[4200, 2400])])
        if extended:
            for owner, unit in enumerate(scenario['units']):
                unit.update(kills=7 + owner, nanoframe=50)
        if skirmish_ceiling:
            scenario = skirmish_ceiling_scenario()
        scenario_path = out / "scenario.json"
        scenario_path.write_text(json.dumps(scenario, indent=2))
        applied = json.loads(command("scenario", "apply", name, str(scenario_path), "--json", timeout=180))
        (out / "apply.json").write_text(json.dumps(applied, indent=2))
        units = {applied["units"][handle]["engine_index"]: owner
                 for owner, handle in ((0, 'human'), (3 if skirmish_ceiling else 1, 'ai'))}
        if len(units) != 2:
            raise RuntimeError("the scenario did not create two distinct probe units")
        if roundtrip:
            print('Saving and loading sleeping 101-word records', flush=True)
            report['save_roundtrip'] = save_roundtrip(name, game, inst, units, out)['ok']
        deadline = time.monotonic() + (60 if roundtrip else 30)
        while time.monotonic() < deadline:
            rows = returns(talog.run_text(inst["gamedir"], "tagpu_cobtrace"))
            expected_count = len(units) * (len(GETTERS) + (34 if extended else 0))
            if len(rows) >= expected_count:
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
            if quarantine:
                memory.seek(main + 0x12EF)
                chat = memory.read(30 * 72)
                lines = [chat[i:i + 64].split(b'\0')[0].decode('latin-1')
                         for i in range(0, len(chat), 72)]
                report['chat_lines'] = lines
                if not all(t in ''.join(lines) for t in ('COBFBAD', 'disabled', 'local index')):
                    raise RuntimeError(f'quarantine text absent from native chat: {lines}')
                memory.seek(main + 0x1438F)
                count = struct.unpack('<I', memory.read(4))[0]
                memory.seek(main + 0x1439B)
                defs = struct.unpack('<I', memory.read(4))[0]
                if count > 16384:
                    raise RuntimeError('definition count exceeds allocation')
                found = False
                for index in range(1, count):
                    memory.seek(defs + index * 0x249)
                    data = memory.read(0x249)
                    if data[32:64].split(b'\0')[0] == b'COBFBAD':
                        found = True
                        if struct.unpack_from('<I', data, 0x241)[0] & 0x800000:
                            raise RuntimeError('malformed type is still available')
                if not found:
                    raise RuntimeError('excluded definition lost required-unit identity')
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
                if snapshot != dict(unit=unit, owner=owner, kills=7 + owner if extended else 0,
                                    nano=0.5 if extended else 0.0, alive=True):
                    raise RuntimeError(f"probe preconditions differ from the scenario: {snapshot}")
        report["unit_slots"] = slots
        if skirmish_ceiling:
            report['skirmish_ceiling_id'] = applied['units']['ai']['engine_index']
            if slots != 15001 or report['skirmish_ceiling_id'] != 6000:
                raise RuntimeError('the probe did not occupy the four-player skirmish ceiling')
        report["checks"] = judge(rows, units, slots, mode, extended)
        report["ok"] = all(row["ok"] for row in report["checks"])
        if thread_exhaustion == 'start':
            trace = talog.run_text(inst['gamedir'], 'tagpu_cobtrace')
            for unit in units:
                refused = any(line.startswith('X\t') and line.split('\t')[2:4] == [str(unit), 'CapacityWorker']
                              for line in trace.splitlines())
                peak = any(line.startswith('H\t') and line.split('\t')[2] == str(unit) and line.endswith('\t128')
                           for line in trace.splitlines())
                if not refused or not peak:
                    raise RuntimeError('full-pool START or 128-word capacity was not exercised')
            report['thread_exhaustion'] = 'START refused, arguments consumed, SIGNAL freed slots, getter CALLs resumed'
        if extended:
            report['call_arguments'] = arguments_match(talog.run_text(inst['gamedir'], 'tagpu_cobtrace'), units)
            report['ok'] &= report['call_arguments']
        if quarantine:
            log = talog.run_text(inst['gamedir'])
            report['quarantine_message'] = bool(re.search(
                r'cob: entry message: .*COBFBAD.*disabled.*local', log, re.I))
            report['ok'] &= report['quarantine_message']
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
        if thread_exhaustion == 'call' and inst:
            failure = inst['gamedir'] / 'log/startup-failure.txt'
            if failure.exists() and 'CALL cannot obtain a child thread' in failure.read_text(errors='replace'):
                report.update(ok=True, required_refusal=failure.read_text(errors='replace'))
                report.pop('error', None)
    finally:
        if game:
            wine_log = compat.stop_wine(game, keep=True)
            shutil.move(wine_log, out / "wine.log")
        else:
            compat.stop_xvfb(display_server)
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
    parser.add_argument("--extended", action="store_true", help="exercise 102-word stack, nonzero values and invalid IDs")
    parser.add_argument("--roundtrip", action="store_true", help="save/load expanded sleeping records before checking their results")
    parser.add_argument("--quarantine", action="store_true", help="require a malformed type to be excluded while the match continues")
    parser.add_argument("--reject-required", action="store_true", help="require a malformed starting commander to refuse the match")
    parser.add_argument('--thread-exhaustion', choices=('start', 'call'), help='exercise the eight-record limit with a 128-word argument stack')
    parser.add_argument('--skirmish-ceiling', action='store_true', help='fill player three to reach native slot 6000 and run extended getter checks; global slot 15000 requires multiplayer')
    parser.add_argument('--unused-model-pieces', action='store_true', help='declare 4096 pieces over the solar model without accessing the excess; exercise safe save padding with --roundtrip')
    parser.add_argument('--reload', action='store_true', help='native +reload before creation; verify a marked script was loaded before getter/save assertions')
    args = parser.parse_args()
    output = args.out.resolve()
    for root in (compat.TREE, compat.main_checkout()):
        if output == root or root in output.parents:
            parser.error("probe output must be outside the repository")
    output.mkdir(parents=True, exist_ok=False)
    if args.roundtrip or args.skirmish_ceiling:
        args.extended = True
    if args.extended and args.expect != 'implemented':
        parser.error('--extended requires the guarded implementation')
    rejection = 'commander' if args.reject_required else args.quarantine
    report = run(output, args.expect, args.dll.resolve(), args.extended, args.roundtrip, rejection,
                 args.thread_exhaustion, args.skirmish_ceiling, args.unused_model_pieces, args.reload)
    summary = {k: v for k, v in report.items() if k not in ('hooks', 'checks', 'chat_lines')}
    summary['checks_passed'] = sum(c['ok'] for c in report.get('checks', []))
    summary['checks_failed'] = [c for c in report.get('checks', []) if not c['ok']]
    print(json.dumps(summary, indent=2))
    print(f"Evidence: {output}")
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
