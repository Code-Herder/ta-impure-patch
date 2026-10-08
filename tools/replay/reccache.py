"""reccache -- a recording's decoded blocks, kept between tarec calls on the same file.

    TAREC_CACHE=DIR tools/replay/tarec.py check FILE    # a second call on FILE reads DIR

Every command that reads a recording's stream decodes it in Python through reccodec.py: about
3 s for a Town & Country battle (MEASURED 2026-10-08, numba's cache warm), and tareplay.py runs
tarec a dozen times and more on each fixture's recording. With TAREC_CACHE naming a directory, tarec.blocks() keeps what the decoding
walk yields, every Block in order and why the walk stopped, and the next call on the same bytes
reads that instead. Unset or empty, nothing is read or written here. tareplay.py points it at a
folder of its run and removes the folder when the run ends.

AN ENTRY CANNOT BE STALE, by its key: the SHA-256 of the recording's bytes, the very bytes the walk
then decodes (the file is read once and handed to iter_blocks), and the decoder's version, the
SHA-256 of the sources the walk runs: reccodec.py, tarec.py, replay-lanes-gen.py and this file. A
different key is a miss, and the walk's result replaces the entry. The version is those sources as
they stand on disk when a process first asks for it; an edit landing between Python's reading a
module and this reading it is not seen, which is why a tareplay run allows no edits while it runs.

AN ENTRY IS NEVER TRUSTED: a plain prefix that names its key, then one zstd frame with its content
checksum, whose content ends with the SHA-256 of all of it before. Every length in it is read in
chunks against the bytes actually there, so a corrupt length cannot claim more memory than the
entry holds; an entry cut short, corrupt, failing either checksum or with bytes after its end is a
miss. An entry is written to a temporary file in the same folder and renamed into place, so a
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
MAGIC = b"TARECDC\x01"                  # the entry's layout: a change to it is a new last byte
BLOCK = struct.Struct("<IIHHIIIII")     # ordinal offset kind flags first last raw packed, length
NO_STOP = 0xFFFFFFFF                    # the stop's length when the walk reached a clean end
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
    """(the blocks, why the walk stopped) of codec.iter_blocks(path) decoding every block: from
    the entry under TAREC_CACHE when it holds this file's, else decoded and kept there."""
    with open(path, "rb") as f:
        data = f.read()
    where = os.environ.get(ENV)
    if not where:
        return collect(codec.iter_blocks(path, data=data))
    key, ver = hashlib.sha256(data).digest(), version(codec)
    entry = os.path.join(where, key.hex() + ".dec")
    got = _read(entry, MAGIC + ver + key, codec.Block)
    if got is None:
        got = collect(codec.iter_blocks(path, data=data))
        _write(where, entry, MAGIC + ver + key, *got)
    return got


def _read(entry, prefix, Block):
    """The entry's blocks and stop, or None when it is absent or anything about it is wrong."""
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
                    n, = struct.unpack("<I", take(4))
                    stop = None if n == NO_STOP else take(n).decode("utf-8")
                    break
                else:
                    return None
            if take(32, hashed=False) != h.digest() or r.read(1):
                return None
            return blocks, stop
    except (OSError, EOFError, zstandard.ZstdError, struct.error, UnicodeDecodeError):
        return None


def _write(where, entry, prefix, blocks, stop):
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
                s = b"" if stop is None else stop.encode("utf-8")
                put(b"E" + struct.pack("<I", NO_STOP if stop is None else len(s)) + s)
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
