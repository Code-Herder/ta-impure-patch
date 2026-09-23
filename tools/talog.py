#!/usr/bin/env python3
"""talog — read the DLL's logs across their rotations.

The DLL writes `<gamedir>/log/tagpu.log` and `log/tagpu_cobtrace.log` through one sink
(tagpu/ddraw/inc/tagpu_log.h) that rotates them: at every launch, and whenever a file
reaches its cap, the current file becomes `<stream>.1.log` and the history shifts up
(`.2`, … `.10`). So "the log" is a set of files, and a reader that remembers a byte
offset in `tagpu.log` loses its place at the first rotation. Everything here follows a
file by its INODE instead: under Wine a rename keeps it, so the file a cursor was
reading is found again under its new name.

A RUN is one launch. The first line of every file the sink writes is

    log: run <id> part <n> of <stream>.log, started ...     (cobtrace: "# log: ...")

so the parts of the current run are `<stream>.log` and the rotated files in front of it
that carry the same run id, back to part 1.

    talog.py mark  <gamedir> [--stream S]           print a cursor token for "now"
    talog.py since <gamedir> <token> [--stream S]   print what was written after it
    talog.py run   <gamedir> [--stream S]           print the current run, all its parts
    talog.py tail  <gamedir> [-n N] [--stream S]    the run's last N lines
"""
import argparse
import os
import re
import sys
from pathlib import Path

KEEP = 10                     # TLOG_KEEP in tagpu_log.c
STREAMS = ("tagpu", "tagpu_cobtrace")
HEADER_RX = re.compile(rb"^(?:# )?log: run (\S+) part (\d+) ")


def log_dir(gamedir) -> Path:
    return Path(gamedir) / "log"


def current(gamedir, stream="tagpu") -> Path:
    return log_dir(gamedir) / f"{stream}.log"


def rotated(gamedir, stream="tagpu", n=1) -> Path:
    return log_dir(gamedir) / f"{stream}.{n}.log"


def newest_first(gamedir, stream="tagpu"):
    """The stream's files that exist, newest first: the current file, then .1 .. .KEEP."""
    paths = [current(gamedir, stream)] + [rotated(gamedir, stream, n) for n in range(1, KEEP + 1)]
    return [p for p in paths if p.exists()]


def _open(p: Path):
    """(file, inode) or (None, None) when it vanished between the listing and the open."""
    try:
        f = open(p, "rb")
    except FileNotFoundError:
        return None, None
    return f, os.fstat(f.fileno()).st_ino


def _header(data: bytes):
    m = HEADER_RX.match(data)
    return (m.group(1), int(m.group(2))) if m else (None, None)


def _run_files(gamedir, stream):
    """(inode, bytes) of each part of the current run, newest first. Stops at part 1, at a
    file of another run, or at a file with no header (not the sink's)."""
    paths = newest_first(gamedir, stream)
    if not paths or paths[0] != current(gamedir, stream):
        return
    run = None
    for p in paths:
        f, ino = _open(p)
        if f is None:
            continue
        with f:
            data = f.read()
        rid, part = _header(data)
        if rid is None or (run is not None and rid != run):
            return
        run = rid
        yield ino, data
        if part <= 1:
            return


def run_parts_newest_first(gamedir, stream="tagpu"):
    """The bytes of each part of the current run, newest first."""
    for _, data in _run_files(gamedir, stream):
        yield data


def run_bytes(gamedir, stream="tagpu") -> bytes:
    return b"".join(reversed(list(run_parts_newest_first(gamedir, stream))))


def run_text(gamedir, stream="tagpu") -> str:
    return run_bytes(gamedir, stream).decode("utf-8", "replace")


def tail_lines(gamedir, n, stream="tagpu"):
    """The current run's last `n` lines, reading back only as many parts as it takes."""
    lines = []
    for data in run_parts_newest_first(gamedir, stream):
        lines = data.decode("utf-8", "replace").splitlines() + lines
        if len(lines) >= n:
            break
    return lines[-n:] if n > 0 else lines


class Cursor:
    """A place in a stream that survives rotation: the inode and offset of the current file
    when it was taken, plus the inodes of the history files that already existed then, so
    read() returns exactly the bytes written since — across any number of rotations, as long
    as the total cap has not deleted the file the cursor was in."""

    def __init__(self, gamedir, stream="tagpu", token=None):
        self.gamedir, self.stream = gamedir, stream
        if token:
            ino, off, known = token.split(":")
            self.ino = int(ino) if ino else None
            self.off = int(off)
            self.known = {int(k) for k in known.split(",") if k}
        else:
            self.mark()

    @classmethod
    def run_start(cls, gamedir, stream="tagpu"):
        """A cursor at the start of the current run: read() returns the whole run so far."""
        c = cls(gamedir, stream)
        parts = {ino for ino, _ in _run_files(gamedir, stream)}
        c.known = {ino for ino in c.known | ({c.ino} if c.ino else set()) if ino not in parts}
        c.ino, c.off = None, 0
        return c

    def token(self) -> str:
        return f"{self.ino or ''}:{self.off}:{','.join(str(k) for k in sorted(self.known))}"

    def mark(self):
        self.ino, self.off, self.known = None, 0, set()
        for p in newest_first(self.gamedir, self.stream):
            try:
                st = p.stat()
            except FileNotFoundError:
                continue
            if p == current(self.gamedir, self.stream):
                self.ino, self.off = st.st_ino, st.st_size
            else:
                self.known.add(st.st_ino)

    def read(self, advance=False) -> bytes:
        """The bytes written since the mark, oldest first. With advance, the mark moves to
        the end of what was returned, so successive calls return successive pieces."""
        chunks, newest = [], None
        for p in newest_first(self.gamedir, self.stream):
            f, ino = _open(p)
            if f is None:
                continue
            with f:
                if ino == self.ino:
                    f.seek(self.off)
                    data = f.read()
                elif ino in self.known:
                    break
                else:
                    data = f.read()
            if newest is None:
                newest = (ino, len(data) + (self.off if ino == self.ino else 0))
            chunks.append(data)
            if ino == self.ino:
                break
        if advance and newest is not None:
            self.known |= {self.ino} if self.ino is not None else set()
            self.ino, self.off = newest
            self.known.discard(self.ino)
        return b"".join(reversed(chunks))

    def text(self, advance=False) -> str:
        return self.read(advance).decode("utf-8", "replace")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("mark", "since", "run", "tail"):
        sp = sub.add_parser(name)
        sp.add_argument("gamedir")
        if name == "since":
            sp.add_argument("token")
        if name == "tail":
            sp.add_argument("-n", type=int, default=40)
        sp.add_argument("--stream", default="tagpu", choices=STREAMS)
    a = ap.parse_args(argv)
    out = sys.stdout.buffer
    if a.cmd == "mark":
        print(Cursor(a.gamedir, a.stream).token())
    elif a.cmd == "since":
        out.write(Cursor(a.gamedir, a.stream, a.token).read())
    elif a.cmd == "run":
        out.write(run_bytes(a.gamedir, a.stream))
    else:
        print("\n".join(tail_lines(a.gamedir, a.n, a.stream)))


if __name__ == "__main__":
    main()
