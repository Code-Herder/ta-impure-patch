"""Shared helpers for the discrete-state size experiments (orders, cells, events, globals,
snapshots) on size-battle.imprec. Read-only on the recording."""
import os
import struct
import sys
import zlib
from pathlib import Path

REPO = Path(str(__import__("pathlib").Path(__file__).resolve().parents[3]))
sys.path.insert(0, str(REPO / "tools/replay"))
import tarec as T  # noqa: E402

SCRATCH = Path(os.environ.get("SIZELAB", "."))     # the data folder: size-battle.imprec
FILE = Path(os.environ.get("REC_FILE", SCRATCH / "size-battle.imprec"))
OUT = Path(os.environ.get("REC_OUT", SCRATCH / "research/discrete"))

try:
    import zstandard
except ImportError:
    zstandard = None


def deflate_writer(data):
    """lodepng's default: dynamic Huffman, LZ77, a 2,048-byte window (recsize.py's model)."""
    c = zlib.compressobj(6, zlib.DEFLATED, 11)
    return len(c.compress(data) + c.flush())


def zstd19(data, level=19):
    return len(zstandard.ZstdCompressor(level=level).compress(data)) if data else 0


def rec_bytes(kind, body):
    """A record as the stream holds it: header, body, padding to a dword."""
    return struct.pack("<HH", kind, len(body)) + bytes(body) + bytes(-len(body) % 4)


def load(path=FILE):
    bl, stop = T.blocks(str(path))
    assert stop is None, stop
    return bl


def mb(n):
    return f"{n / 1048576:,.2f} MB"


def kb(n):
    return f"{n / 1024:,.0f} KB"
