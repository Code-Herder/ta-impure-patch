"""reccache -- a recording's decoded blocks, kept between tarec calls on the same bytes.

    TAREC_CACHE=DIR tools/replay/tarec.py check FILE    # a second call on FILE reads DIR

Every command that reads a recording's stream decodes it in Python through reccodec.py: about
3 s for a Town & Country battle (MEASURED 2026-10-08, numba's cache warm), and tareplay.py runs
tarec a dozen times and more on each fixture's recording. With TAREC_CACHE naming a directory,
tarec.blocks() keeps what the decoding walk yields, every Block in order, and the next call on the
same bytes reads that instead. Unset or empty, nothing is read or written here. tareplay.py points
it at a folder of its run and removes the folder when the run ends.

PIECES: a file is kept as pieces, one per segment -- from an RB_HEADER block (the file's first
block for the first piece) to the next RB_HEADER or the end of the file's last complete block. A
takeover appends a segment to the file it was played from and leaves the bytes before it as they
were, so a fork's file reads its parent's pieces and decodes only what was appended (the forks
lane checks its file after every one of six forks). A piece's walk resumes at its header
(reccodec.iter_blocks' `start`): a header starts new frames, a new zstd run and a new snapshot
chain (tagpu_recfmt.h, THE RUN AND THE CHAIN), so a piece decodes from its own first block alone,
and what else the walk carries across -- the block ordinal and the first header's slots -- is read
off the pieces before it.

AN ENTRY CANNOT BE STALE, by its key: the SHA-256 of the file's bytes from its start to the
piece's end, the very bytes the walk then decodes (the file is read once and handed to
iter_blocks), and the decoder's version, the SHA-256 of the sources the walk runs: reccodec.py,
tarec.py, replay-lanes-gen.py and this file. The walk reads no byte past a block's end to yield
it, so the blocks of a piece are a function of the bytes its key hashes: two files that agree to
the end of a piece walk it alike, and a file that differs anywhere before that end has another
key. Why a walk stops short of the file's end is no piece's: it is read off the file itself by a
walk that does not decode (the same block checks, and the only ones that stop a walk rather than
refuse it). The version is those sources as they stand on disk when a process first asks for it;
an edit landing between Python's reading a module and this reading it is not seen, which is why a
tareplay run allows no edits while it runs.

AN ENTRY IS NEVER TRUSTED: a plain prefix that names its key, then one zstd frame with its content
checksum, whose content ends with the SHA-256 of all of it before. Every length in it is read in
chunks against the bytes actually there, so a corrupt length cannot claim more memory than the
entry holds; an entry cut short, corrupt, failing either checksum, with bytes after its end, or
whose blocks do not tile its piece one after another from the ordinal the pieces before it end on
is a miss. An entry is written to a temporary file in the same folder and renamed into place, so a
reader sees a whole entry or none, whatever other tarec process writes the same one.

Only the walk tarec.blocks() asks for with decode=True comes through here. The decoder's own tests
(tagpu/ddraw/tests/codectest.py verify and fuzzdiff) and recsize.py call reccodec.iter_blocks
directly and always decode.

Standard library at import; zstandard where an entry is read or written (without it there is no
cache, and the walk decodes as it would unset).
"""
import hashlib
import os
import struct
import tempfile

ENV = "TAREC_CACHE"
MAGIC = b"TARECDC\x02"                  # the entry's layout: a change to it is a new last byte
BLOCK = struct.Struct("<IIHHIIIII")     # ordinal offset kind flags first last raw packed, length
CHUNK = 1 << 20

_version = None


def version(codec):
    """The decoder's version: the SHA-256 of the sources a decoding walk runs."""
    global _version
    if _version is None:
        h = hashlib.sha256(MAGIC)
        for src in (codec.__file__, codec.tarec.__file__, codec._GEN.__file__, __file__):
            with open(src, "rb") as f:
                h.update(hashlib.sha256(f.read()).digest())
        _version = h.digest()
    return _version


def collect(walk):
    """A block walk run to its end: (the blocks, why it stopped)."""
    out = []
    while True:
        try:
            out.append(next(walk))
        except StopIteration as stop:
            return out, stop.value


def walk(path, codec):
    """(the blocks, why the walk stopped) of codec.iter_blocks(path) decoding every block: each
    piece from its entry under TAREC_CACHE when there is one, the rest decoded and kept there."""
    with open(path, "rb") as f:
        data = f.read()
    where = os.environ.get(ENV)
    if not where:
        return collect(codec.iter_blocks(path, data=data))
    try:
        layout, stop = collect(codec.iter_blocks(path, decode=False, data=data))
    except codec.Unreadable:
        return collect(codec.iter_blocks(path, data=data))      # refused, by the decoding walk
    if not layout:
        return collect(codec.iter_blocks(path, data=data))
    bounds = ([layout[0].offset] +
              [b.offset for b in layout[1:] if b.kind == codec.tarec.RB_HEADER] +
              [layout[-1].offset + 32 + layout[-1].packed])
    ver, h, view = version(codec), hashlib.sha256(), memoryview(data)
    blocks, keys = [], []
    for lo, hi in zip(bounds, bounds[1:]):
        h.update(view[lo:hi])
        keys.append(h.copy().digest())
    for i, key in enumerate(keys):
        got = _read(_entry(where, key), MAGIC + ver + key, codec.Block)
        if not tiles(got, bounds[i], bounds[i + 1], len(blocks)):
            break
        blocks += got
    else:
        return blocks, stop
    first = next((b for b in blocks if b.kind == codec.tarec.RB_HEADER), None)
    slots = None if first is None else codec.header_slots(codec.entries(first.payload))
    start = (bounds[i], len(blocks), slots) if blocks else None
    tail, stop = collect(codec.iter_blocks(path, data=data, start=start))
    n = len(blocks)
    for j in range(i, len(keys)):
        piece = [b for b in tail if bounds[j] <= b.offset < bounds[j + 1]]
        if tiles(piece, bounds[j], bounds[j + 1], n):
            _write(where, _entry(where, keys[j]), MAGIC + ver + keys[j], piece)
        n += len(piece)
    return blocks + tail, stop


def tiles(blocks, lo, hi, ordinal):
    """Whether blocks are a piece [lo, hi): one after another from `lo` to `hi`, numbered on from
    `ordinal`."""
    if not blocks:
        return False
    at = lo
    for b in blocks:
        ordinal += 1
        if b.offset != at or b.ordinal != ordinal:
            return False
        at += 32 + b.packed
    return at == hi


def _entry(where, key):
    return os.path.join(where, key.hex() + ".dec")


def _read(entry, prefix, Block):
    """The entry's blocks, or None when it is absent or anything about it is wrong."""
    try:
        import zstandard
    except ImportError:
        return None
    try:
        with open(entry, "rb") as f:
            if f.read(len(prefix)) != prefix:
                return None
            r = zstandard.ZstdDecompressor().stream_reader(f, read_across_frames=False)
            h = hashlib.sha256()

            def take(n, hashed=True):
                parts = []
                while n:
                    b = r.read(min(n, CHUNK))
                    if not b:
                        raise EOFError
                    parts.append(b)
                    n -= len(b)
                out = b"".join(parts)
                if hashed:
                    h.update(out)
                return out

            blocks = []
            while True:
                tag = take(1)
                if tag == b"B":
                    *fields, n = BLOCK.unpack(take(BLOCK.size))
                    blocks.append(Block(*fields, take(n)))
                elif tag == b"E":
                    break
                else:
                    return None
            if take(32, hashed=False) != h.digest() or r.read(1):
                return None
            return blocks
    except (OSError, EOFError, zstandard.ZstdError, struct.error):
        return None


def _write(where, entry, prefix, blocks):
    """The entry, whole or not at all. A cache that cannot be written costs the next call a
    decode and nothing else, so a failure here is not reported."""
    try:
        import zstandard
    except ImportError:
        return
    tmp = None
    try:
        os.makedirs(where, exist_ok=True)
        fd, tmp = tempfile.mkstemp(dir=where, prefix=".", suffix=".tmp")
        with os.fdopen(fd, "wb") as f:
            f.write(prefix)
            h = hashlib.sha256()
            with zstandard.ZstdCompressor(level=3, write_checksum=True).stream_writer(
                    f, closefd=False) as w:
                def put(b):
                    h.update(b)
                    w.write(b)
                for b in blocks:
                    put(b"B" + BLOCK.pack(*b[:8], len(b.payload)))
                    put(b.payload)
                put(b"E")
                w.write(h.digest())
        os.replace(tmp, entry)
        tmp = None
    except OSError:
        pass
    finally:
        if tmp:
            try:
                os.unlink(tmp)
            except OSError:
                pass
