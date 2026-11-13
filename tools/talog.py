#!/usr/bin/env python3
"""talog — read the DLL's logs across their rotations.

The DLL writes `<gamedir>/log/tagpu.log` and `log/tagpu_cobtrace.log` through one sink
(tagpu/ddraw/inc/tagpu_log.h) that rotates them: at every launch, and whenever a file
reaches its cap, the current file becomes `<stream>.1.log` and the history shifts up
(`.2`, … `.10`). So "the log" is a set of files, and a reader that remembers a byte
offset in `tagpu.log` loses its place at the first rotation. Everything here follows a
file by the `run/part` in its first line instead, so the file a cursor was reading is
found again under its new name.

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


HEAD_BYTES = 256              # the header line is well inside this (NOTE_MAX in tagpu_log.c)


def _open(p: Path):
    """The open file, or None when it vanished between the listing and the open."""
    try:
        return open(p, "rb")
    except FileNotFoundError:
        return None


def _keys_now(gamedir, stream):
    """(name, key) of every file of the stream, newest first (headers only). The names
    matter: the sink creates the new current file on its first line, so for a moment after a
    rotation the keys alone read the same as before -- only shifted one name up."""
    out = []
    for p in newest_first(gamedir, stream):
        f = _open(p)
        if f is not None:
            with f:
                out.append((p.name, _key(f.read(HEAD_BYTES))))
    return out


def _stable(gamedir, stream, fn):
    """fn() over a listing no rotation moved. A listing and the opens after it are not one
    act: a rotation between them renames files under the reader, and a pass can then read a
    part twice or miss one. A rotation moves every key to another name and the sink never
    reuses a key, so an unchanged (name, key) list before and after the pass proves no
    rotation happened during it; otherwise the pass runs again. After eight moved passes --
    a rotation per pass, sustained -- the last result is returned as it is: a reader that
    waits or tails is better served by a rare repeated or missing part than by an error."""
    for _ in range(8):
        before = _keys_now(gamedir, stream)
        result = fn()
        if _keys_now(gamedir, stream) == before:
            return result
    return result


def _header(data: bytes):
    m = HEADER_RX.match(data)
    return (m.group(1), int(m.group(2))) if m else (None, None)


def _run_files(gamedir, stream):
    """The bytes of each part of the current run, newest first. Stops at part 1, at a
    file of another run, or at a file with no header (not the sink's)."""
    paths = newest_first(gamedir, stream)
    if not paths or paths[0] != current(gamedir, stream):
        return
    run = None
    for p in paths:
        f = _open(p)
        if f is None:
            continue
        with f:
            data = f.read()
        rid, part = _header(data)
        if rid is None or (run is not None and rid != run):
            return
        run = rid
        yield data
        if part <= 1:
            return


def run_parts_newest_first(gamedir, stream="tagpu", enough=None):
    """The bytes of each part of the current run, newest first -- all of them, or only as many
    as it takes for enough(parts_so_far) to say so."""
    def one_pass():
        parts = []
        for data in _run_files(gamedir, stream):
            parts.append(data)
            if enough and enough(parts):
                break
        return parts
    return _stable(gamedir, stream, one_pass)


def run_bytes(gamedir, stream="tagpu") -> bytes:
    return b"".join(reversed(list(run_parts_newest_first(gamedir, stream))))


def run_text(gamedir, stream="tagpu") -> str:
    return run_bytes(gamedir, stream).decode("utf-8", "replace")


# THE GPU MEMORY CAP REFUSED AN ALLOCATION (tagpu_vk.c full_AllocateMemory, `gpumem=`): a pass asked
# for memory past the cap the run lock sized the game's GPU memory on. The renderer's own
# adaptation to a lower cap -- the terrain's neighbourhood atlas and its restored chain, which ask
# the cap's headroom first (tagpu_vk_restore_fits) -- never reaches the door and logs no such line,
# so every one is a pass that lost memory it was not sized to do without: the cap is too small for
# that game, and every harness that plays a game under a cap fails it by name (tools/runlock.py,
# THE GPU MEMORY): on the lines its waits read, and on the game's whole log once it has ended --
# cap_failure on one run's text, CapWatch across the launches of one game folder.
CAP_REFUSED_RX = re.compile(r"^vk: gpumem: the cap refuses [^\r\n]*", re.M)
CAP_FAILED = "the GPU memory cap refused"     # how every failure cap_failure names begins


def cap_refusals(text: str) -> list:
    """Every refusal of the GPU memory cap in `text`, the DLL's log or part of it, in order, each
    line without its line end."""
    return [m.group(0) for m in CAP_REFUSED_RX.finditer(text or "")]


def cap_failure(text: str, game: str = "") -> "str | None":
    """The one line a harness fails a game by when `text`, its log, holds a refusal of the GPU
    memory cap -- `game` names it where a harness plays several -- or None when it holds none."""
    return _failure(cap_refusals(text), game)


def _failure(refused, game):
    if not refused:
        return None
    more = f" ({len(refused)} refusals)" if len(refused) > 1 else ""
    return f"{CAP_FAILED} {game + ' ' if game else ''}an allocation{more}: {refused[0]}"


class CapWatch:
    """The refusals of the GPU memory cap (cap_refusals) logged in one game folder from the watch's
    start on, across every launch and rotation between: what a harness that plays one game or
    several in a folder fails them by, the lines after its last wait included. sweep() reads only
    what was written since the last sweep (a Cursor), and a rotation moves the file it is reading
    to a name the Cursor finds by its key, so every byte the sink wrote is read while the sink
    keeps it: up to KEEP rotations between two sweeps. A harness sweeps at each launch and at each
    end of a game, which keeps that to the launch's one rotation and the ones a game's size makes."""

    def __init__(self, gamedir):
        self.cursor = Cursor(gamedir)
        self.part = ""
        self.refused = []

    def sweep(self) -> list:
        """The refusals read since the last failure(), the complete lines written since the last
        sweep included."""
        text = self.part + self.cursor.text(advance=True)
        whole = text.rfind("\n") + 1
        self.part = text[whole:]
        self.refused += cap_refusals(text[:whole])
        return self.refused

    def failure(self, game: str = "") -> "str | None":
        """cap_failure over the refusals logged since the last failure(), or since the watch
        began, asked once the games they judge have ended: a last line a game never finished is
        read as it stands. Each refusal is named by one failure() only."""
        self.sweep()
        cut = cap_refusals(self.part)
        if cut:
            self.part = ""
        refused, self.refused = self.refused + cut, []
        return _failure(refused, game)


def tail_lines(gamedir, n, stream="tagpu"):
    """The current run's last `n` lines, reading back only as many parts as it takes."""
    enough = (lambda parts: sum(p.count(b"\n") for p in parts) >= n) if n > 0 else None
    lines = []
    for data in run_parts_newest_first(gamedir, stream, enough):
        lines = data.decode("utf-8", "replace").splitlines() + lines
    return lines[-n:] if n > 0 else lines


def _key(data: bytes):
    """A file's identity: `run/part` from its header. None for a file with no header yet --
    the sink creates a file and writes its header right after, so a reader can catch it
    empty; such a file holds nothing, and reading it whole is always right."""
    rid, part = _header(data)
    return f"{rid.decode()}/{part}" if rid else None


class Cursor:
    """A place in a stream that survives rotation. A file is known by its header's
    `run/part`, which the sink never repeats, and NOT by its inode: once history is full the
    sink deletes files, and the filesystem hands a deleted file's inode to the next new one.
    The cursor holds the key and offset of the current file when it was taken, plus the keys
    of the history files that already existed then, so read() returns exactly the bytes
    written since -- across any number of rotations, as long as the total cap has not deleted
    the file the cursor was in."""

    def __init__(self, gamedir, stream="tagpu", token=None):
        self.gamedir, self.stream = gamedir, stream
        if token:
            key, off, known = token.split(":")
            self.key = key or None
            self.off = int(off)
            self.known = {k for k in known.split(",") if k}
        else:
            self.mark()

    @classmethod
    def run_start(cls, gamedir, stream="tagpu"):
        """A cursor at the start of the current run: read() returns the whole run so far."""
        c = cls(gamedir, stream)
        parts = {_key(data[:HEAD_BYTES]) for data in run_parts_newest_first(gamedir, stream)}
        c.known = {k for k in c.known | ({c.key} if c.key else set()) if k not in parts}
        c.key, c.off = None, 0
        return c

    def token(self) -> str:
        return f"{self.key or ''}:{self.off}:{','.join(sorted(self.known))}"

    def mark(self):
        _stable(self.gamedir, self.stream, self._mark)

    def _mark(self):
        self.key, self.off, self.known = None, 0, set()
        for p in newest_first(self.gamedir, self.stream):
            f = _open(p)
            if f is None:
                continue
            with f:
                key = _key(f.read(HEAD_BYTES))
                size = os.fstat(f.fileno()).st_size
            if p == current(self.gamedir, self.stream):
                self.key, self.off = key, (size if key else 0)
            elif key:
                self.known.add(key)

    def read(self, advance=False) -> bytes:
        """The bytes written since the mark, oldest first. With advance, the mark moves to
        the end of what was returned, so successive calls return successive pieces."""
        got = self._same_file()
        chunks, newest = got if got else _stable(self.gamedir, self.stream, self._pass)
        if advance and newest is not None:
            if self.key:
                self.known.add(self.key)
            self.key, self.off = newest if newest[0] else (None, 0)
            self.known.discard(self.key)
        return b"".join(reversed(chunks))

    def _same_file(self):
        """_pass's answer from one open, when the current file is still the one the mark is
        in, else None. The sink writes only to the current file and never reuses a key, so a
        current file that carries the mark's key has not been rotated since the mark, and every
        byte written since is in it past the mark's offset -- which is what _pass reads first
        and stops at. A rotation after the open leaves the open file whole, and the next read
        finds it under its new name by its key. This is what makes a poll cheap: one open
        instead of the listing _stable takes three times (MEASURED 2026-11-13 on a stream of
        11 files and about 105 MB: 405 us of CPU a read through _stable, 20 us through this;
        research/notes/logging.md)."""
        if self.key is None:
            return None
        f = _open(current(self.gamedir, self.stream))
        if f is None:
            return None
        with f:
            if _key(f.read(HEAD_BYTES)) != self.key:
                return None
            f.seek(self.off)
            data = f.read()
        return [data], (self.key, self.off + len(data))

    def _pass(self):
        chunks, newest = [], None
        for p in newest_first(self.gamedir, self.stream):
            f = _open(p)
            if f is None:
                continue
            with f:
                key = _key(f.read(HEAD_BYTES))
                mine = key is not None and key == self.key
                if not mine and key is not None and key in self.known:
                    break
                f.seek(self.off if mine else 0)
                data = f.read()
            if newest is None:
                newest = (key, len(data) + (self.off if mine else 0))
            chunks.append(data)
            if mine:
                break
        return chunks, newest

    def text(self, advance=False) -> str:
        return self.read(advance).decode("utf-8", "replace")


class Tail:
    """One file's bytes, as a whole read of it returns them, read whole once and then only as
    they grow: the reader of a wait that polls a single file (Cursor follows a stream across
    its rotations). The sink only appends to a file, so a file whose first bytes are not the
    ones read before, or that is shorter than what was read, is another file -- the sink
    rotated it, or it was truncated -- and is read whole again, which is what a whole read
    would return at that moment. A look then costs one open and the bytes appended, not the
    file's size (MEASURED 2026-11-13: 13 us, against 17 ms of CPU for a whole read of a 15 MB
    tagpu.log and a search of it)."""

    def __init__(self, path):
        self.path = Path(path)
        self.data = bytearray()

    def read(self) -> int:
        """`data` brought up to the file; returns the offset in it where the bytes new since the
        last read begin, 0 when the file was read whole. Raises OSError as a whole read would,
        and then holds nothing."""
        try:
            f = open(self.path, "rb")
        except OSError:
            self.data = bytearray()
            raise
        with f:
            had = len(self.data)
            if had and (f.read(min(HEAD_BYTES, had)) != self.data[:HEAD_BYTES]
                        or os.fstat(f.fileno()).st_size < had):
                self.data, had = bytearray(), 0
            f.seek(had)
            self.data += f.read()
        return had


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
