"""Small building blocks for the re-encodings: varints, zigzag, column sets, and the per-block
compression of a set of columns."""
import struct
import zlib

from common import deflate_writer, zstandard


def zz(v):
    """zigzag of a signed 32-bit value"""
    v = (v + 0x80000000) % 0x100000000 - 0x80000000
    return 2 * v if v >= 0 else -2 * v - 1


def unzz(u):
    return (u >> 1) ^ -(u & 1)


def varint(out, u):
    while u >= 0x80:
        out.append(u & 0x7F | 0x80)
        u >>= 7
    out.append(u)


def read_varint(buf, at):
    shift = v = 0
    while True:
        b = buf[at]
        at += 1
        v |= (b & 0x7F) << shift
        if b < 0x80:
            return v, at
        shift += 7


def wdiff(new, old):
    """the wrapping difference of two u32, as a signed value"""
    return (new - old + 0x80000000) % 0x100000000 - 0x80000000


class Cols:
    """Named byte columns; a block's encoding is their concatenation with a length table."""

    def __init__(self):
        self.c = {}

    def __getitem__(self, k):
        return self.c.setdefault(k, bytearray())

    def blob(self):
        out = bytearray()
        for k in sorted(self.c):
            varint(out, len(self.c[k]))
        for k in sorted(self.c):
            out += self.c[k]
        return bytes(out)

    def raw(self):
        return sum(len(v) for v in self.c.values())


def packed(data, how):
    if not data:
        return 0
    if how == "deflate":
        return deflate_writer(data)
    if how == "zlib9":
        return len(zlib.compress(data, 9))
    if how.startswith("zstd"):
        return len(zstandard.ZstdCompressor(level=int(how[4:])).compress(data))
    raise ValueError(how)


HOWS = ("deflate", "zstd3", "zstd19")
