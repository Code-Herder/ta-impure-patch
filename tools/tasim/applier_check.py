#!/usr/bin/env python3
"""applier_check.py -- the `calib-applier` replay fixture's assertion (tools/replay/fixtures.json).

    tools/tasim/applier_check.py check <instance>

Run by tareplay as the fixture's `run` step, on the live lane held at its GameTime (120): every
write the calibration keys of the scenario applier make (tagpu_scenario.c) is read back from the
engine and compared with what the fixture's scenarios ask for.

- the objective disarmed: `[main+0x391ED]+0x88 == 0`;
- seat 1's AI off: its record's `+0x73 == 1` and its PlayerInfo's `+0x94 == 1`;
- both seats against the last apply that named them (the step at 60, which kills seat 1's
  D-gun commander and states every seat's storage and levels again): `+0x149` bit 0 set and
  `+0xDC`/`+0xE0` equal to the scenario's, and the levels `+0x8C`/`+0x98` and the maxima
  `+0xA4`/`+0xA8` at least that past the first economy pass after it -- every seat, none excused;
- the feature grid holds no feature the BurnAll walker would burn (a dump read with tarec);
- the delayed orders' one report line, its counts computed from the fixture's scenarios and step
  GameTimes, never a literal;
- each queued chain in the unit's main list `+0x5C`: exactly its orders' Attack_Chase nodes, each on
  the unit its order names, in file order (a `queue` order goes through 0x43AFC0 with Shift);
- `tacli units --json` carries the engine's costs and max HP.

Exit 0 when every check holds, 1 with the first that does not, 2 when it could not run. The puppet
lanes (P, R, S) then show the recording carries each of these writes.
"""

import json
import re
import struct
import subprocess
import sys
import time
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
TACLI = TREE / "tools" / "tacli"
SCENARIOS = TREE / "scenarios"
FIXTURE = "calib-applier"
sys.path.insert(0, str(TREE / "tools"))
sys.path.insert(0, str(TREE / "tools" / "replay"))
import talog  # noqa: E402
import tarec  # noqa: E402

_SPEC = spec_from_loader("_applier_cli", SourceFileLoader("_applier_cli", str(TACLI)))
CLI = module_from_spec(_SPEC)
sys.modules[_SPEC.name] = CLI
_SPEC.loader.exec_module(CLI)

MAIN = 0x511DE8
OFF_PLAYERS, PL_STRIDE = 0x1B63, 0x14B
OFF_OBJECTIVE, OBJ_ARMED = 0x391ED, 0x88
OFF_GAMETIME = 0x38A47
PL_TYPE, PL_INFO, INFO_TYPE = 0x73, 0x27, 0x94
PL_ENERGY, PL_METAL, PL_MAXE, PL_MAXM = 0x8C, 0x98, 0xA4, 0xA8
PL_BASEE, PL_BASEM, PL_BASEFLAG = 0xDC, 0xE0, 0x149
PASS_EVERY = 30                 # the economy pass's period; it falls at GameTime 1 (mod 30)
ACK_S = 60
CELL, CELL_DEF = tarec.REC_CELL_BYTES, 8
DUMP = "calibfeat"
OFF_BEGIN, OFF_END, UNIT_STRIDE = 0x14357, 0x1435B, 0x118
PL_FIRSTUNIT, PL_LASTUNIT = 0x67, 0x6B
U_ORDERS, U_XPOS, U_YPOS, U_OWNER, U_STATE = 0x5C, 0x6C, 0x74, 0xFF, 0x110
ON_ACTION, ON_TARGET, ON_NEXT = 0x04, 0x16, 0x4A
ATTACK_CHASE = 6                # the action a mover's attack order takes (MEASURED, engine/weapons.md)
LIST_MAX = 16                   # nodes walked a list, at most
NEAR = 24                       # world units: a unit found by where the scenario stood it


class Fail(Exception):
    pass


class CannotRun(Exception):
    pass


def peek(instance, *specs):
    """The values of `specs` (tacli peek's grammar), as unsigned ints."""
    r = subprocess.run([sys.executable, str(TACLI), "peek", instance, "--json", *specs],
                       capture_output=True, text=True, timeout=ACK_S + 30)
    if r.returncode:
        raise CannotRun(f"tacli peek: {(r.stdout + r.stderr).strip()[-200:]}")
    rows = json.loads(r.stdout)["peeks"]
    if len(rows) != len(specs):
        raise CannotRun(f"tacli peek answered {len(rows)} of {len(specs)}")
    out = []
    for row in rows:
        v = CLI._peek_uint(row)
        if v is None:
            raise CannotRun(f"peek {row['spec']}: {row['value']}")
        out.append(v)
    return out


def f32(u):
    return struct.unpack("<f", struct.pack("<I", u & 0xFFFFFFFF))[0]


def fixture():
    fx = json.loads((TREE / "tools" / "replay" / "fixtures.json").read_text())["fixtures"]
    return fx[FIXTURE]


def applies(fix):
    """[(GameTime, compiled scenario)]: the load at the launch's hold, GameTime 1, then each
    `apply` step."""
    out = [(1, CLI._scn_compile(str(SCENARIOS / fix["load"])))]
    for T, kind, arg in sorted(fix.get("steps", []), key=lambda s: s[0]):
        if kind == "apply":
            out.append((T, CLI._scn_compile(str(SCENARIOS / arg))))
    return out


def expected_delays(seq, now):
    """The report line's counts by the delayed-order rules (tagpu_scenario.c): an order with
    `after` a is due at its apply's GameTime plus a; it is cancelled as a death when its unit is
    the target of a `blast` in the same scenario (the D-gun kills it outright), as cleared when a
    later apply with clear_existing comes first, and issued otherwise: the fixture asks for
    no order its unit refuses, so `refused` is 0. Only orders due by `now` or cancelled by then
    are in the line, which is written when the table first empties."""
    issued = deaths = cleared = of = 0
    for k, (T, exp) in enumerate(seq):
        handles = {u["id"]: i for i, u in enumerate(exp["units"])}
        blasted = {handles[o["target"]] for u in exp["units"] for o in u["orders"]
                   if o["cmd"] == "blast" and o["target"] in handles}
        next_clear = next((T2 for T2, e2 in seq[k + 1:] if e2["setup"]["clear_existing"]), None)
        for i, u in enumerate(exp["units"]):
            for o in u["orders"]:
                if not o["after"]:
                    continue
                of += 1
                due = T + o["after"]
                if i in blasted:
                    deaths += 1
                elif next_clear is not None and due > next_clear:
                    cleared += 1
                elif due <= now:
                    issued += 1
                else:
                    raise CannotRun(f"the order of {u['id']} is due at {due}, after the check")
    return issued, of, 0, deaths + cleared, deaths, cleared, 0


def check_objective(instance, ta):
    obj, = peek(instance, f"0x{ta + OFF_OBJECTIVE:X}:4")
    if not obj:
        raise Fail("the level has no objective")
    armed, = peek(instance, f"0x{obj + OBJ_ARMED:X}:4")
    if armed != 0:
        raise Fail(f"the objective's +0x88 is {armed}, not disarmed")
    return "objective off"


def next_pass(T):
    """The first economy pass after an apply at GameTime T."""
    return T + PASS_EVERY - (T - 1) % PASS_EVERY


def seat_states(seq, now):
    """{seat: (the GameTime of the last apply that named it by `now`, its player entry, whether
    any apply turned its AI off, whether that last apply's scenario gives the seat a blast)}."""
    out = {}
    for T, exp in seq:
        if T > now:
            continue
        for p in exp["setup"]["players"]:
            ai_off = p.get("ai") == "off" or (p["slot"] in out and out[p["slot"]][2])
            blast = any(o["cmd"] == "blast" for u in exp["units"] if u["owner"] == p["slot"]
                        for o in u["orders"])
            out[p["slot"]] = (T, p, ai_off, blast)
    return out


def check_seats(instance, ta, seq, now):
    """Each seat against the last apply that named it: its AI off when one turned it off, and
    its base storage, maxima and levels as that apply asked, past the economy pass after it. The
    levels hold exactly that far only for a seat that spends nothing: the pass clamps each level
    to the storage it recomputes, the units' own plus the base storage, so without the base
    storage it would take the level down to the units' storage. A seat whose last apply has it
    fire a D-gun spends energy, so the fixture's last apply gives no seat one."""
    lines = []
    for s, (T, p, ai_off, blast) in sorted(seat_states(seq, now).items()):
        rec = ta + OFF_PLAYERS + s * PL_STRIDE
        kind, info, flag = peek(instance, f"0x{rec + PL_TYPE:X}:1", f"0x{rec + PL_INFO:X}:4",
                                f"0x{rec + PL_BASEFLAG:X}:1")
        if ai_off:
            itype, = peek(instance, f"0x{info + INFO_TYPE:X}:1")
            if kind != 1 or itype != 1:
                raise Fail(f"seat {s}: controller {kind}, PlayerInfo {itype}: the AI is not off")
            lines.append(f"seat {s} ai off")
        st = p.get("storage")
        if st is None:
            continue
        be, bm, e, m, xe, xm = (f32(v) for v in peek(
            instance, *(f"0x{rec + off:X}:4" for off in
                        (PL_BASEE, PL_BASEM, PL_ENERGY, PL_METAL, PL_MAXE, PL_MAXM))))
        if not flag & 1:
            raise Fail(f"seat {s}: +0x149 bit 0 is clear")
        if be != float(st["energy"]) or bm != float(st["metal"]):
            raise Fail(f"seat {s}: base storage {bm}/{be}, the scenario asks {st}")
        if now <= next_pass(T):
            raise CannotRun(f"the check runs at GameTime {now}, before the economy pass after "
                            f"the apply at {T}")
        # the pass recomputes the maxima from the units plus the base storage: never under it
        if xm < st["metal"] or xe < st["energy"]:
            raise Fail(f"seat {s} at GameTime {now}: maxima {xm}/{xe} under the base storage {st}")
        if blast:
            raise CannotRun(f"seat {s}: the apply at {T} has it fire a D-gun, which spends: its "
                            f"levels cannot be held to the apply's")
        if (p["metal"] is not None and m < p["metal"]) or (p["energy"] is not None and e < p["energy"]):
            raise Fail(f"seat {s} at GameTime {now}: level {m}/{e}, asked {p['metal']}/"
                       f"{p['energy']} at {T}: the clamp took it")
        lines.append(f"seat {s} storage {bm:g}/{be:g}, level {m:g}/{e:g}")
    return "; ".join(lines)


def wait_line(cursor, rx, what):
    rx = re.compile(rx)
    end = time.monotonic() + ACK_S
    text = ""
    while time.monotonic() < end:
        text += cursor.text(advance=True)
        for ln in text.splitlines():
            if rx.search(ln):
                return ln
        time.sleep(0.1)
    raise CannotRun(f"{what}: no line in {ACK_S} s")


def check_features(instance, gamedir):
    """A dump taken now, on the held frame: no cell holds a def the BurnAll walker burns
    (below 0xFFFB or 0xFFFE, its test at 0x424869..0x424875)."""
    rd = gamedir / "replays"
    recs = sorted(rd.glob("*.imprec"), key=lambda p: p.stat().st_mtime)
    if not recs:
        raise CannotRun("no recording in the replays folder")
    cursor = talog.Cursor(gamedir)
    trig = gamedir / "tagpu_replay.trigger"
    tmp = trig.with_name(trig.name + ".tmp")
    tmp.write_text(f"dump {DUMP} now\n")
    tmp.replace(trig)
    written = CLI.dump_written_rx(DUMP)
    got = wait_line(cursor, f"{written}|{CLI.dump_refused_rx(DUMP)}", f"dump {DUMP}")
    if not re.search(written, got):
        raise CannotRun(f"dump {DUMP}: {got.strip()}")
    path = rd / f"dump-{DUMP}.bin"
    _dh, world = tarec.dump_world(path, tarec.header_only(recs[-1]))
    burnable = 0
    for at in range(0, len(world.grid), CELL):
        d = struct.unpack_from("<H", world.grid, at + CELL_DEF)[0]
        if d < 0xFFFB or d == 0xFFFE:
            burnable += 1
    path.unlink(missing_ok=True)
    if burnable:
        raise Fail(f"{burnable} feature cells left after clear_features")
    return "no feature left"


def check_delays(gamedir, seq, now):
    want = expected_delays(seq, now)
    rx = re.compile(r"scenario: delayed orders: issued (\d+) of (\d+), refused (\d+), "
                    r"cancelled (\d+) \(deaths (\d+), cleared (\d+), skipped (\d+)\)")
    got = [tuple(int(g) for g in m.groups())
           for m in (rx.search(ln) for ln in talog.run_text(gamedir).splitlines()) if m]
    if got != [want]:
        raise Fail(f"delayed-order lines {got}, expected one {want}")
    return ("delayed orders: issued %d of %d, refused %d, cancelled %d (deaths %d, cleared %d, "
            "skipped %d)" % want)


def chains(seq, now):
    """[(the subject's scenario unit, [the target units its orders name])] of every unit with a queued
    order created by the last clearing apply by `now` or after it, each target the scenario's unit."""
    last = max(T for T, exp in seq if T <= now and exp["setup"]["clear_existing"])
    out = []
    for T, exp in seq:
        if last <= T <= now:
            by = {u["id"]: u for u in exp["units"]}
            out += [(u, [by[o["target"]] for o in u["orders"]]) for u in exp["units"]
                    if any(o.get("queue") for o in u["orders"])]
    return out


def seat_units(instance, ta, owner):
    """[(pointer, x, y, the main list's head)] of the seat's live units: its record's first..last,
    inclusive, bounded by the unit array."""
    rec = ta + OFF_PLAYERS + owner * PL_STRIDE
    first, last, beg, end = peek(instance, f"0x{rec + PL_FIRSTUNIT:X}:4", f"0x{rec + PL_LASTUNIT:X}:4",
                                 f"0x{ta + OFF_BEGIN:X}:4", f"0x{ta + OFF_END:X}:4")
    if not (beg <= first <= last <= end) or (first - beg) % UNIT_STRIDE or (last - first) // UNIT_STRIDE > 64:
        raise CannotRun(f"seat {owner}: its units 0x{first:X}..0x{last:X} are not a short run of the array")
    ptrs = list(range(first, last + 1, UNIT_STRIDE))
    vals = peek(instance, *(f"0x{u + off:X}:{n}" for u in ptrs
                            for off, n in ((U_STATE, 4), (U_XPOS, 2), (U_YPOS, 2), (U_ORDERS, 4))))
    out = []
    for k, u in enumerate(ptrs):
        st, x, y, head = vals[4 * k:4 * k + 4]
        if st & 0x10000000:
            out.append((u, x, y, head))
    return out, beg, end


def near(x, y, pos):
    return abs(x - pos[0]) <= NEAR and abs(y - pos[1]) <= NEAR


def check_queues(instance, ta, seq, now):
    """Each queued chain's subject, found nearest its scenario position (a subject has walked well under
    NEAR world units a tick since), holds exactly its chain: Attack_Chase nodes through +0x4A, each
    target a slot of the array standing where its scenario unit stood, of that unit's owner."""
    said = []
    for u, targets in chains(seq, now):
        live, beg, end = seat_units(instance, ta, u["owner"])
        ordered = [(p, x, y, h) for p, x, y, h in live if h]
        if not ordered:
            raise Fail(f"{u['id']}: no unit of seat {u['owner']} holds an order")
        p, x, y, node = min(ordered, key=lambda r: (r[1] - u["pos"][0]) ** 2 + (r[2] - u["pos"][1]) ** 2)
        if (x - u["pos"][0]) ** 2 + (y - u["pos"][1]) ** 2 > 200 ** 2:
            raise Fail(f"{u['id']}: the nearest ordered unit of seat {u['owner']} is at ({x}, {y})")
        got = []
        while node and len(got) <= LIST_MAX:
            act, tgt, nxt = peek(instance, f"0x{node + ON_ACTION:X}:1", f"0x{node + ON_TARGET:X}:4",
                                 f"0x{node + ON_NEXT:X}:4")
            got.append((act, tgt))
            node = nxt
        if len(got) != len(targets):
            raise Fail(f"{u['id']}: its main list holds {len(got)} nodes, its chain {len(targets)}")
        for k, ((act, tgt), t) in enumerate(zip(got, targets)):
            if act != ATTACK_CHASE:
                raise Fail(f"{u['id']}: node {k} is action {act}, not Attack_Chase")
            if not (beg <= tgt <= end) or (tgt - beg) % UNIT_STRIDE:
                raise Fail(f"{u['id']}: node {k}'s target 0x{tgt:X} is not a slot of the array")
            owner, tx, ty = peek(instance, f"0x{tgt + U_OWNER:X}:1", f"0x{tgt + U_XPOS:X}:2",
                                 f"0x{tgt + U_YPOS:X}:2")
            if owner != t["owner"] or not near(tx, ty, t["pos"]):
                raise Fail(f"{u['id']}: node {k} targets seat {owner}'s unit at ({tx}, {ty}), not "
                           f"{t['id']} at {t['pos']}")
        said.append(f"{u['id']} queues {'/'.join(t['id'] for t in targets)}")
    return "; ".join(said)


def check_catalogue(instance):
    r = subprocess.run([sys.executable, str(TACLI), "units", instance, "--json"],
                       capture_output=True, text=True, timeout=ACK_S + 30)
    if r.returncode:
        raise CannotRun(f"tacli units: {(r.stdout + r.stderr).strip()[-200:]}")
    rows = {u["name"].upper(): u for u in json.loads(r.stdout).get("units", [])}
    pw = rows.get("ARMPW")
    if not pw or len(pw.get("cost") or []) != 2 or not pw.get("maxdamage"):
        raise Fail(f"tacli units: ARMPW carries no cost or maxdamage: {pw}")
    return f"ARMPW cost {pw['cost']}, maxdamage {pw['maxdamage']}"


def check(instance):
    inst = CLI.Instance(instance)
    seq = applies(fixture())
    ta, = peek(instance, f"0x{MAIN:X}:4")
    now, = peek(instance, f"0x{ta + OFF_GAMETIME:X}:4")
    said = [check_objective(instance, ta), check_seats(instance, ta, seq, now),
            check_features(instance, inst.gamedir), check_delays(inst.gamedir, seq, now),
            check_queues(instance, ta, seq, now), check_catalogue(instance)]
    print(f"applier_check at GameTime {now}: " + "; ".join(s for s in said if s))


def main(argv):
    if len(argv) != 2 or argv[0] != "check":
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    import devpy                        # the recording's blocks need the shared venv
    devpy.ensure()
    try:
        check(argv[1])
    except Fail as e:
        print(f"applier_check: FAILED: {e}")
        return 1
    except (CannotRun, SystemExit) as e:
        print(f"applier_check: could not run: {e}")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
