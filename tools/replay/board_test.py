#!/usr/bin/env python3
"""board_test -- the drawing board's operations and store (tagpu_boardwire.h, tagpu_boardstore.h):
builds board_test.c on the host with the address and undefined-behaviour sanitizers, runs it, and
parses the body it writes with recmp_file.board_ops, the file tools' reader of RECMP_MARK.
Exit 0 when both pass."""
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(HERE))
import recmp_file  # noqa: E402


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe, body = Path(tmp) / "board_test", Path(tmp) / "body.bin"
        subprocess.run(["gcc", "-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-I", TREE / "tagpu/ddraw/inc", "-o", exe, HERE / "board_test.c",
                        TREE / "tagpu/ddraw/src/tagpu_boardwire.c",
                        TREE / "tagpu/ddraw/src/tagpu_boardstore.c"], check=True)
        if subprocess.run([exe, body]).returncode:
            return 1
        ops = recmp_file.board_ops(body.read_bytes())
        want = [(1, 3 << 24 | 7, (100, 20, -300), None, "Go here"),
                (2, 1 << 24 | 2, None, None, "edited ~"),
                (3, 1 << 24 | 2, (-0x100000, 0, 0x100000), None, None),
                (4, 9 << 24 | 0xFFFFFF, None, None, None),
                (5, None, (1, 2, 3), (4, 5, 6), None),
                (6, None, (7, 8, 9), None, None)]
        if ops != want:
            print(f"FAIL: recmp_file.board_ops read {ops}")
            return 1
        for bad in (body.read_bytes()[:-1], b"\x02\x03" + body.read_bytes()[2:]):
            try:
                recmp_file.board_ops(bad)
            except ValueError:
                continue
            print("FAIL: recmp_file.board_ops took a malformed body")
            return 1
    print("board_test.py: PASS (C wire and store, Python reader)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
