#!/usr/bin/env python3
"""dpport.py -- move a wine prefix's DirectPlay off UDP/TCP 47624, so several games host at once.

DirectPlay's name server (dplaysvr.exe) binds 47624 for the whole machine, and the TCP/IP
transport (dpwsockx.dll) sends every session enumeration there: two prefixes hosting at once
collide however separate they are. The port is not a setting in either file -- it is seven
immediates in their code, each already in network byte order (0x08BA is htons(47624)):

    dplaysvr.exe  0x010022FD  push 8BAh        0x010023E2  push 8BAh
                  0x01002DE8  mov ebx,8BAh
    dpwsockx.dll  0x5DF08300  mov word [ebp-12h],8BAh      0x5DF08D8C  push 8BAh
                  0x5DF091C5  mov word [edi+0Ch],8BAh      0x5DF0921D  mov word [edi+2],8BAh

(DISASSEMBLED from the files tools/dpinstall.sh installs; the only other 08BA/BA08 pair in the
eight files is the tail of a call's rel32 in dplayx.dll.) Every peer of one game must carry
the same port: the host's name server listens on it and the joiner's transport asks it.

    dpport.py <wineprefix> <port>      # 47624 puts the stock bytes back

REFUSES A BUILD IT DOES NOT KNOW. A file is patched only when, with those sites masked, it is
byte-identical to the one the offsets were read from -- whatever port it carries now. And it
is written as a NEW file: tacli clones prefixes with `cp -al`, so an in-place write could reach
the template and every other instance through the hardlink.
"""

import hashlib
import os
import sys
from pathlib import Path

STOCK = 47624
# name -> (file offsets of the two port bytes, md5 of the file with those bytes zeroed)
SITES = {
    "dplaysvr.exe": ((0x16FD, 0x17E2, 0x21E8), "757fff243f8741ebe0c5f115c8caa29d"),
    "dpwsockx.dll": ((0x7700, 0x818C, 0x85C5, 0x861D), "7c0932d9aab3ef77393c9b3e70a9ad41"),
}


def masked_md5(data: bytes, offsets) -> str:
    b = bytearray(data)
    for o in offsets:
        b[o:o + 2] = b"\0\0"
    return hashlib.md5(b).hexdigest()


def patch_file(path: Path, port: int) -> str:
    offsets, want = SITES[path.name.lower()]
    data = path.read_bytes()
    if max(offsets) + 2 > len(data) or masked_md5(data, offsets) != want:
        raise SystemExit(f"dpport: {path} is not the DirectPlay build this knows; nothing written")
    b = bytearray(data)
    for o in offsets:
        b[o:o + 2] = port.to_bytes(2, "big")          # network byte order, as the code expects
    if bytes(b) == data:
        return "already"
    tmp = path.with_name(path.name + ".dpport-tmp")
    tmp.write_bytes(b)
    os.replace(tmp, path)                             # a new inode: never through a hardlink
    return "patched"


def targets(prefix: Path) -> list:
    """Every copy dpinstall.sh put in, and nothing else: on a 64-bit prefix both files in
    syswow64 and the name server it also drops in system32 (whose dpwsockx.dll is wine's own
    64-bit builtin, never Microsoft's); on a 32-bit prefix both in system32."""
    win = prefix / "drive_c" / "windows"
    if (win / "syswow64").is_dir():
        want = [win / "syswow64" / n for n in SITES] + [win / "system32" / "dplaysvr.exe"]
    else:
        want = [win / "system32" / n for n in SITES]
    return [p for p in want if p.is_file()]


def set_port(prefix, port: int) -> list:
    """Put `port` into every DirectPlay copy in the prefix. Returns (path, result) pairs."""
    if not 1024 <= port <= 65535:
        raise SystemExit(f"dpport: {port} is not a port a game can bind (1024..65535)")
    found = targets(Path(prefix))
    if not any(p.name.lower() == "dpwsockx.dll" for p in found):
        raise SystemExit(f"dpport: no native DirectPlay in {prefix} (tools/dpinstall.sh first)")
    return [(p, patch_file(p, port)) for p in found]


def main() -> int:
    if len(sys.argv) != 3 or not sys.argv[2].isdigit():
        print(__doc__.split("\n\n")[-2].strip(), file=sys.stderr)
        return 2
    for p, what in set_port(sys.argv[1], int(sys.argv[2])):
        print(f"{what:8} {p}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
