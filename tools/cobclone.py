#!/usr/bin/env python3
"""cobclone — give a compiled TA COB per-weapon copies of its primary scripts.

The extra-weapons module looks slot N up as AimWeaponN / FireWeaponN /
QueryWeaponN / AimFromWeaponN. `cobalias` can point those names at the primary
scripts' code, but every Cavedog aim script opens with `signal M; set-signal-
mask M`, so two slots sharing one aim script kill each other's aim threads and
only the last starter ever fires. This tool instead *clones* the four primary
scripts once per extra weapon, relocating the absolute jump targets and giving
each clone its own signal bit, so N weapons aim concurrently — with no COB
compiler in the loop. The clones drive the same pieces (one turret, N shots).

    tools/cobclone.py in.cob out.cob --weapons 10        # AimWeapon4..10 etc.
    tools/cobclone.py in.cob --dump AimPrimary           # opcode listing

Bytecode facts used (Cavedog COB, TA 3.1): code is an array of 32-bit words;
JUMP 0x10064000 and JUMP_NOT_EQUAL 0x10066000 carry an absolute word address;
PUSH_CONSTANT 0x10021001 carries the value; SIGNAL 0x10067000 and
SET_SIGNAL_MASK 0x10068000 pop it. Unknown opcodes abort the clone rather than
guess an operand count.
"""

import struct
import sys
from pathlib import Path
from importlib.machinery import SourceFileLoader

cobalias = SourceFileLoader("cobalias", str(Path(__file__).with_name("cobalias.py"))).load_module()

# opcode -> (mnemonic, operand words)
OPS = {
    0x10001000: ("MOVE", 2), 0x10002000: ("TURN", 2), 0x10003000: ("SPIN", 2),
    0x10004000: ("STOP_SPIN", 2), 0x10005000: ("SHOW", 1), 0x10006000: ("HIDE", 1),
    0x10007000: ("CACHE", 1), 0x10008000: ("DONT_CACHE", 1), 0x1000B000: ("MOVE_NOW", 2),
    0x1000C000: ("TURN_NOW", 2), 0x1000D000: ("SHADE", 1), 0x1000E000: ("DONT_SHADE", 1),
    0x1000F000: ("EMIT_SFX", 1), 0x10011000: ("WAIT_FOR_TURN", 2), 0x10012000: ("WAIT_FOR_MOVE", 2),
    0x10013000: ("SLEEP", 0), 0x10021001: ("PUSH_CONSTANT", 1), 0x10021002: ("PUSH_LOCAL_VAR", 1),
    0x10021004: ("PUSH_STATIC_VAR", 1), 0x10022000: ("CREATE_LOCAL_VAR", 0),
    0x10023002: ("POP_LOCAL_VAR", 1), 0x10023004: ("POP_STATIC_VAR", 1),
    0x10031000: ("ADD", 0), 0x10032000: ("SUB", 0), 0x10033000: ("MUL", 0), 0x10034000: ("DIV", 0),
    0x10035000: ("BITWISE_AND", 0), 0x10036000: ("BITWISE_OR", 0), 0x10037000: ("BITWISE_XOR", 0),
    0x10038000: ("BITWISE_NOT", 0), 0x10041000: ("RAND", 0), 0x10042000: ("GET_UNIT_VALUE", 0),
    0x10043000: ("GET", 0), 0x10051000: ("LESS", 0), 0x10052000: ("LESS_EQUAL", 0),
    0x10053000: ("GREATER", 0), 0x10054000: ("GREATER_EQUAL", 0), 0x10055000: ("EQUAL", 0),
    0x10056000: ("NOT_EQUAL", 0), 0x10057000: ("AND", 0), 0x10058000: ("OR", 0),
    0x10059000: ("XOR", 0), 0x1005A000: ("NOT", 0), 0x10061000: ("START_SCRIPT", 2),
    0x10062000: ("CALL_SCRIPT", 2), 0x10064000: ("JUMP", 1), 0x10065000: ("RETURN", 0),
    0x10066000: ("JUMP_NOT_EQUAL", 1), 0x10067000: ("SIGNAL", 0), 0x10068000: ("SET_SIGNAL_MASK", 0),
    0x10071000: ("EXPLODE", 1), 0x10072000: ("PLAY_SOUND", 1), 0x10073000: ("MAP_COMMAND", 2),
    0x10082000: ("SET", 0), 0x10083000: ("ATTACH_UNIT", 0), 0x10084000: ("DROP_UNIT", 0),
}
JUMPS = {0x10064000, 0x10066000}
SIGNALS = {0x10067000, 0x10068000}
PRIMARY = [("AimPrimary", "AimWeapon{n}"), ("FirePrimary", "FireWeapon{n}"),
           ("QueryPrimary", "QueryWeapon{n}"), ("AimFromPrimary", "AimFromWeapon{n}")]


def script_ranges(cob):
    """[(name, start, end)] in code order; a script ends where the next begins."""
    starts = sorted((c, n) for n, c in cob["scripts"])
    total = len(cob["code"]) // 4
    return [(n, c, (starts[i + 1][0] if i + 1 < len(starts) else total))
            for i, (c, n) in enumerate(starts)]


def decode(words, start, end):
    """Yield (address, opcode, operands) over one script."""
    pc = start
    while pc < end:
        op = words[pc]
        if op not in OPS:
            raise SystemExit(f"cobclone: unknown opcode 0x{op:08X} at word {pc}")
        n = OPS[op][1]
        yield pc, op, list(words[pc + 1:pc + 1 + n])
        pc += 1 + n


def clone(words, start, end, new_start, mask):
    out = []
    prev_push = None
    for pc, op, args in decode(words, start, end):
        if op in JUMPS:
            tgt = args[0]
            if start <= tgt < end:
                args = [tgt - start + new_start]
        if op in SIGNALS and prev_push is not None:
            out[prev_push + 1] = mask          # the constant that feeds signal / set-signal-mask
        prev_push = len(out) if op == 0x10021001 else None
        out.append(op)
        out.extend(args)
    return out


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    src = Path(argv[1]).read_bytes()
    cob = cobalias.parse(src)
    words = list(struct.unpack(f"<{len(cob['code']) // 4}I", cob["code"]))
    ranges = {n: (s, e) for n, s, e in script_ranges(cob)}
    if argv[2] == "--dump":
        s, e = ranges[argv[3]]
        for pc, op, args in decode(words, s, e):
            print(f"{pc:5d}  {OPS[op][0]:<18} {' '.join(f'{a:#x}' for a in args)}")
        return 0
    dst = Path(argv[2])
    nweapons = int(argv[argv.index("--weapons") + 1]) if "--weapons" in argv else 4
    have = {n.lower() for n, _ in cob["scripts"]}
    for n in range(4, nweapons + 1):
        mask = 1 << (4 + n)                      # bits 8.. : clear of every stock mask
        for prim, fmt in PRIMARY:
            name = fmt.format(n=n)
            if name.lower() in have:
                continue
            if prim not in ranges:
                raise SystemExit(f"cobclone: no {prim} in {argv[1]}")
            s, e = ranges[prim]
            new_start = len(words)
            words.extend(clone(words, s, e, new_start, mask))
            cob["scripts"].append((name, new_start))
            have.add(name.lower())
    cob["code"] = struct.pack(f"<{len(words)}I", *words)
    out = cobalias.build(cob)
    chk = cobalias.parse(out)
    assert chk["scripts"] == cob["scripts"] and chk["code"] == cob["code"]
    dst.write_bytes(out)
    print(f"{dst}: {len(cob['scripts'])} scripts, {len(words)} code words, {len(out)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
