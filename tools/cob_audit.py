#!/usr/bin/env python3
"""Audit compiled COB control flow, stack demand and GET/SET use without running it.

    python3 tools/cob_audit.py path/to/mod.gp3 path/to/addon.ufo --json result.json

Each archive is audited independently: duplicate paths are retained with their
content hashes, never resolved by guessed archive precedence. No game assets are
written to the report. Stack peaks are relative to a script entry with sp=-1,
assuming successful child allocation; engine entry arguments and refused START
need a separate execution audit. This is research tooling, not a runtime validator.
"""

import argparse
from collections import Counter, deque
import hashlib
from importlib.machinery import SourceFileLoader
import json
import operator
from pathlib import Path
import sys

import hpipack

tacob = SourceFileLoader("cob_audit_tacob", str(Path(__file__).with_name("tacob"))).load_module()

BINARY = set(tacob.BINARY_OPS.values()) | {"XOR", "RAND"}
UNARY = {"BITWISE_NOT", "NOT"}
POP_ONE = {"STOP_SPIN", "MOVE_NOW", "TURN_NOW", "EMIT_SFX", "SLEEP",
           "POP_LOCAL_VAR", "POP_STATIC_VAR", "POP_STACK", "SIGNAL",
           "SET_SIGNAL_MASK", "EXPLODE", "DROP_UNIT"}
POP_TWO = {"MOVE", "TURN", "SPIN", "SET"}
NO_STACK = {"SHOW", "HIDE", "CACHE", "DONT_CACHE", "SHADE", "DONT_SHADE",
            "WAIT_FOR_TURN", "WAIT_FOR_MOVE", "JUMP"}
PIECE_OPS = NO_STACK - {"JUMP"} | {"MOVE", "TURN", "SPIN", "STOP_SPIN",
                                           "MOVE_NOW", "TURN_NOW", "EMIT_SFX", "EXPLODE"}
AXIS_OPS = {"MOVE", "TURN", "SPIN", "STOP_SPIN", "MOVE_NOW", "TURN_NOW",
            "WAIT_FOR_TURN", "WAIT_FOR_MOVE"}
LITERAL_BINARY = {"ADD": operator.add, "SUB": operator.sub, "MUL": operator.mul,
                  "BITWISE_AND": operator.and_, "BITWISE_OR": operator.or_,
                  "BITWISE_XOR": operator.xor, "XOR": operator.xor,
                  "EQUAL": operator.eq, "NOT_EQUAL": operator.ne, "LESS": operator.lt,
                  "LESS_EQUAL": operator.le, "GREATER": operator.gt, "GREATER_EQUAL": operator.ge,
                  "AND": lambda a, b: bool(a) and bool(b),
                  "OR": lambda a, b: bool(a) or bool(b)}


def literal_result(name, stack):
    """Keep unmodelled arithmetic unknown; never guess a value to prune a path."""
    if name in LITERAL_BINARY and len(stack) >= 2 and None not in stack[-2:]:
        return tacob.signed(int(LITERAL_BINARY[name](*stack[-2:])))
    if stack and stack[-1] is not None:
        if name == "BITWISE_NOT":
            return tacob.signed(~stack[-1])
        if name == "NOT":
            return int(not stack[-1])
    return None


def audit_script(cob, name, start, analysis_limit=4096, state_limit=65536):
    result = dict(script=name, entry=start, peak_words=0, peak_pc=start,
                  gets=[], sets=[], dynamic_get=False, dynamic_set=False,
                  dynamic_get_pcs=[], dynamic_set_pcs=[],
                  calls=[], issues=[])
    issues = set()

    def issue(pc, message):
        issues.add((pc, message))

    # Entry labels do not fence control flow. Compilers share tails, interleave
    # declarations with assignments, and leave unreachable data in code ranges.
    operands, instruction_starts = set(), {start}
    result["local_words"] = 0
    seen = {(start, 0): ()}
    pending = deque([(start, 0)])
    gets, sets, calls = set(), set(), set()
    while pending:
        pc, depth = pending.popleft()
        if not 0 <= pc < len(cob.code):
            issue(pc, "control flow outside code")
            continue
        if pc in operands:
            issue(pc, "control flow enters an instruction operand")
            continue
        word = cob.code[pc]
        if word not in tacob.MNEMONIC:
            issue(pc, f"unknown opcode 0x{word:08X}")
            continue
        stop = pc + 1 + tacob.INLINE[word]
        if stop > len(cob.code):
            issue(pc, "instruction operands outside code")
            continue
        if any(p in instruction_starts for p in range(pc + 1, stop)):
            issue(pc, "instruction operands overlap a control-flow target")
            continue
        operands.update(range(pc + 1, stop))
        op = tacob.decode(cob.code, pc, stop)[0]
        stack = list(seen[pc, depth])
        n, args = op.name, op.args
        if n in PIECE_OPS and args[0] >= len(cob.pieces):
            issue(pc, "piece index outside piece table")
        if n in AXIS_OPS and args[1] >= 3:
            issue(pc, "axis index outside X/Y/Z")
        if n in {"PUSH_LOCAL_VAR", "POP_LOCAL_VAR"}:
            result["local_words"] = max(result["local_words"], args[0] + 1)
            if args[0] >= analysis_limit:
                issue(pc, "local index exceeds analysis capacity")
        if n in {"PUSH_STATIC_VAR", "POP_STATIC_VAR"} and args[0] >= cob.statics:
            issue(pc, "static index outside allocation")

        if n.startswith("PUSH_") or n == "CREATE_LOCAL_VAR":
            need, produced = 0, [args[0] if n == "PUSH_CONSTANT" else None]
        elif n in BINARY:
            need, produced = 2, [literal_result(n, stack)]
        elif n in UNARY:
            need, produced = 1, [literal_result(n, stack)]
        elif n in POP_ONE:
            need, produced = 1, []
        elif n in POP_TWO:
            need, produced = 2, []
        elif n == "ATTACH_UNIT":
            need, produced = 3, []
        elif n == "GET":
            need, produced = 5, [None]
        elif n == "GET_UNIT_VALUE":
            need, produced = 1, [None]
        elif n in {"START_SCRIPT", "CALL_SCRIPT"}:
            target, argc = args
            if target >= len(cob.scripts):
                issue(pc, "script target outside entry table")
            calls.add((pc, n, target, argc))
            need, produced = argc, []
        elif n in {"RETURN", "JUMP_NOT_EQUAL"}:
            need, produced = 1, []
        elif n in NO_STACK:
            need, produced = 0, []
        else:
            issue(pc, "opcode not modelled; stack proof incomplete: " + n)
            continue
        if need > len(stack):
            issue(pc, f"stack underflow: needs {need}, has {len(stack)} at relative entry")
            continue
        if n in {"GET", "GET_UNIT_VALUE", "SET"}:
            value = stack[-need]
            is_set = n == "SET"
            if value is None:
                result["dynamic_set" if is_set else "dynamic_get"] = True
                result["dynamic_set_pcs" if is_set else "dynamic_get_pcs"].append(pc)
            else:
                (sets if is_set else gets).add(value)
        condition = stack[-1] if n == "JUMP_NOT_EQUAL" else None
        if need:
            del stack[-need:]
        stack.extend(produced)
        if len(stack) > result["peak_words"]:
            result["peak_words"], result["peak_pc"] = len(stack), pc
        if len(stack) > analysis_limit:
            issue(pc, "analysis stack limit exceeded; no finite capacity established")
            continue
        if n == "RETURN":
            continue
        successors = [args[0]] if n == "JUMP" else [op.end]
        if n == "JUMP_NOT_EQUAL":
            successors = ([args[0], op.end] if condition is None else
                          [args[0] if condition == 0 else op.end])
        for target in successors:
            if not 0 <= target < len(cob.code):
                issue(pc, f"control flow outside code: {target}")
                continue
            new = tuple(stack)
            # Conditional declarations legitimately meet at different depths,
            # e.g. an owner's Upgrade body and the non-owner's immediate RETURN.
            # Keep those frames separate instead of labelling a valid join bad.
            key = (target, len(new))
            old = seen.get(key)
            if old is not None:
                new = tuple(a if a == b else None for a, b in zip(old, new))
                if new == old:
                    continue
            elif len(seen) >= state_limit:
                issue(target, "analysis state limit exceeded; proof incomplete")
                continue
            seen[key] = new
            instruction_starts.add(target)
            pending.append(key)
    result["gets"], result["sets"] = sorted(gets), sorted(sets)
    for key in ("dynamic_get_pcs", "dynamic_set_pcs"):
        result[key] = sorted(set(result[key]))
    result["calls"] = [dict(pc=p, opcode=n, target=t, argc=a) for p, n, t, a in sorted(calls)]
    result["issues"] = [dict(pc=p, reason=r) for p, r in sorted(issues)]
    return result


def audit_blob(blob):
    cob = tacob.read_cob(blob)
    for _, start in cob.scripts:
        if start >= len(cob.code):
            raise tacob.TacobError("script entry outside code")
    scripts = [audit_script(cob, name, start) for name, start in cob.scripts]
    return dict(sha256=hashlib.sha256(blob).hexdigest(), bytes=len(blob),
                pieces=len(cob.pieces), statics=cob.statics, scripts=scripts,
                peak_words=max((s["peak_words"] for s in scripts), default=0))


def audit_paths(paths):
    files = []
    for path in paths:
        path = Path(path)
        if path.suffix.lower() == ".cob":
            sources = [(path.name, path.read_bytes())]
        else:
            archive = hpipack.Archive(path)
            sources = ((name, archive.read(name)) for name in sorted(archive.files)
                       if name.endswith(".cob"))
        for name, blob in sources:
            try:
                item = audit_blob(blob)
            except (tacob.TacobError, ValueError, IndexError) as exc:
                item = dict(error=str(exc), sha256=hashlib.sha256(blob).hexdigest())
            item.update(archive=path.name, path=name)
            files.append(item)
    unique = {f["sha256"]: f for f in files}
    gets, sets = Counter(), Counter()
    for f in unique.values():
        gets.update({i for s in f.get("scripts", []) for i in s["gets"]})
        sets.update({i for s in f.get("scripts", []) for i in s["sets"]})
    return dict(schema=1, assumptions=["archives independent; precedence not inferred",
                "stack peaks relative to sp=-1; successful child allocation",
                "engine entry arguments, thread occupancy and runtime safety not proved"],
                summary=dict(files=len(files), unique=len(unique),
                    max_relative_stack=max((f.get("peak_words", 0) for f in files), default=0),
                    with_issues=sum(bool(f.get("error")) or
                                    any(s["issues"] for s in f.get("scripts", [])) for f in unique.values()),
                    get_files=dict(sorted(gets.items())), set_files=dict(sorted(sets.items()))),
                files=files)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path)
    parser.add_argument("--json", type=Path, help="write full report, no asset bytes")
    args = parser.parse_args()
    report = audit_paths(args.paths)
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["summary"], indent=2))
    return 1 if report["summary"]["with_issues"] else 0


if __name__ == "__main__":
    sys.exit(main())
