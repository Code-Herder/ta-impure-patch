#!/usr/bin/env python3
"""hpipack — write a Total Annihilation HAPI archive (.hpi/.ufo/.ccx/.gp3) from files.

Files are stored as the engine's own SQSH chunks (64 KiB, zlib, chunk-level
encryption, byte-sum checksum) because every archive Cavedog shipped uses them
and a raw (compression 0) entry is not something the engine was ever asked to
read. The header key is 0 like the official rev31.gp3 (`--key 0xAF` for the
whole-file XOR the other shipped archives carry). The layout is the canonical one HPIPack produces —
the whole directory (directory records, entry arrays, names, file records)
first, then the file payloads — so `directory size` in the header is exactly
the byte count a reader must load to walk the tree. Format reference:
research/notes/file-formats.md §5; ta3do's HpiArchive reads the result.

    header    "HAPI" | version 0x00010000 | directory size | header key | root dir offset
    trailer   the literal "Copyright 1997 Cavedog Entertainment" as the last 36 bytes
    dir rec   entry count | entries offset
    entry     name offset | data offset | flag   (9 bytes; flag 1 = sub directory)
    file rec  payload offset | size | compression (2)
    payload   u32 chunk sizes, then per chunk: "SQSH" v2 method2 enc1 csize dsize sum | body

    tools/hpipack.py out.ufo SRC_DIR                                  # a directory tree
    tools/hpipack.py out.ufo units/X.fbi=path/to/X.fbi scripts/X.cob=… # explicit entries
    tools/hpipack.py --key 0xAF out.ufo …                             # obfuscated like Cavedog's packs
    tools/hpipack.py --zlib out.ufo …                                 # zlib chunks like rev31.gp3
    tools/hpipack.py --list archive.hpi                               # what is inside
    tools/hpipack.py --extract archive.hpi units/armpw.fbi [OUT_DIR]  # pull files out
"""

import struct
import sys
import zlib
from pathlib import Path

HEADER = 20
CHUNK = 65536
# The engine's archive-open routine (0x4BDD70) seeks to the end of the file and
# compares the last 36 bytes with this string; anything else is not an archive.
# It is read raw, so it goes after the obfuscated body, never through the XOR.
TRAILER = b"Copyright 1997 Cavedog Entertainment"


def lz77_store(piece: bytes) -> bytes:
    """Method-1 (LZSS, 4 KiB window) stream that only emits literals: tag byte
    0x00 then eight literals, and a set tag bit with offset 0 to terminate.
    No compression, but a stream every method-1 decoder accepts."""
    out = bytearray()
    for i in range(0, len(piece), 8):
        group = piece[i:i + 8]
        if len(group) == 8:
            out.append(0x00)
            out += group
        else:
            out.append(0xFF << len(group) & 0xFF)     # literals, then a set bit
            out += group
            out += b"\x00\x00"                         # offset 0 = end of stream
            return bytes(out)
    out += b"\x01\x00\x00"                            # tag: first bit set -> end
    return bytes(out)


def pack_file(data: bytes, method: int = 1) -> bytes:
    """SQSH chunks as the shipped archives store them: method 1 (LZ77, what
    every Cavedog .ufo/.hpi uses) or 2 (zlib, what rev31.gp3 uses); body bytes
    encrypted as ((plain ^ i) + i); checksum = sum of the stored body bytes."""
    pieces = [data[i:i + CHUNK] for i in range(0, len(data), CHUNK)] or [b""]
    blobs = []
    for piece in pieces:
        comp = lz77_store(piece) if method == 1 else zlib.compress(piece, 9)
        body = bytes(((b ^ i) + i) & 0xFF for i, b in enumerate(comp))
        head = struct.pack("<IBBBIII", 0x48535153, 2, method, 1, len(body), len(piece),
                           sum(body) & 0xFFFFFFFF)
        blobs.append(head + body)
    table = struct.pack(f"<{len(blobs)}I", *[len(b) for b in blobs])
    return table + b"".join(blobs)


def obfuscate(data: bytes, header_key: int) -> bytes:
    """Encode everything after the 20-byte header the way TA's reader decodes it:
    plain = (offset ^ key ^ ~byte) & 0xFF, offsets absolute from file start."""
    if not header_key:
        return data
    key = (~((header_key * 4) | (header_key >> 6))) & 0xFF
    out = bytearray(data)
    for i in range(HEADER, len(out)):
        out[i] = (~(out[i] ^ i ^ key)) & 0xFF
    return bytes(out)


def build(tree: dict, header_key: int = 0, method: int = 1) -> bytes:
    """tree: {name: bytes | dict} — directories are dicts, files are bytes."""
    directory = bytearray()          # absolute offsets = HEADER + position
    payloads = []                    # (file-record position in `directory`, data)

    def dalloc(b: bytes) -> int:
        off = HEADER + len(directory)
        directory.extend(b)
        return off

    def write_dir(d: dict) -> int:
        names = sorted(d, key=str.lower)
        rec = dalloc(b"\0" * 8)
        entries = dalloc(b"\0" * (9 * len(names)))
        packed = bytearray()
        for name in names:
            name_off = dalloc(name.encode("latin-1") + b"\0")
            child = d[name]
            if isinstance(child, dict):
                packed += struct.pack("<IIB", name_off, write_dir(child), 1)
            else:
                data = bytes(child)
                frec = dalloc(struct.pack("<IIB", 0, len(data), method))
                payloads.append((frec - HEADER, pack_file(data, method)))
                packed += struct.pack("<IIB", name_off, frec, 0)
        struct.pack_into("<II", directory, rec - HEADER, len(names), entries)
        directory[entries - HEADER:entries - HEADER + len(packed)] = packed
        return rec

    root = write_dir(tree)
    dir_size = HEADER + len(directory)
    body = bytearray()
    for frec_pos, data in payloads:
        struct.pack_into("<I", directory, frec_pos, dir_size + len(body))
        body += data
    header = b"HAPI" + struct.pack("<IIII", 0x00010000, dir_size, header_key, root)
    return obfuscate(header + bytes(directory) + bytes(body), header_key) + TRAILER


# ---------------------------------------------------------------- reading
# The same container read back (any shipped .hpi/.ccx/.ufo/.gp3): whole-file
# XOR with the derived key, the directory walk, and SQSH chunks (method 1 LZSS
# with a 4 KiB window, method 2 zlib), so a fixture can be rebuilt from the
# game's own files with this one tool.

class Archive:
    def __init__(self, path):
        self.raw = Path(path).read_bytes()
        if self.raw[:4] != b"HAPI":
            raise SystemExit(f"{path}: not a HAPI archive")
        version, self.dir_size, hk, self.start = struct.unpack_from("<4xIIII", self.raw, 0)
        self.key = (~((hk * 4) | (hk >> 6)) & 0xFF) if hk else 0
        self.files = {}
        self._walk(self.start, "")

    def _bytes(self, off, n):
        chunk = self.raw[off:off + n]
        if not self.key:
            return chunk
        k = self.key
        return bytes(((off + i) ^ k ^ (~b & 0xFF)) & 0xFF for i, b in enumerate(chunk))

    def _u32(self, off):
        return struct.unpack("<I", self._bytes(off, 4))[0]

    def _cstr(self, off):
        out = bytearray()
        while True:
            b = self._bytes(off, 1)
            if not b or b == b"\0":
                return out.decode("latin-1")
            out += b
            off += 1

    def _walk(self, off, prefix):
        count, entries = self._u32(off), self._u32(off + 4)
        for i in range(count):
            e = entries + i * 9
            name_off, data_off = struct.unpack("<II", self._bytes(e, 8))
            flag = self._bytes(e + 8, 1)[0]
            name = prefix + self._cstr(name_off)
            if flag == 1:
                self._walk(data_off, name + "/")
            else:
                self.files[name.lower()] = struct.unpack("<IIB", self._bytes(data_off, 9)) + (name,)

    def read(self, name):
        off, size, comp, _ = self.files[name.lower().replace("\\", "/")]
        if comp == 0:
            return self._bytes(off, size)
        n = (size + CHUNK - 1) // CHUNK
        table = struct.unpack(f"<{n}I", self._bytes(off, n * 4))
        pos, out = off + n * 4, bytearray()
        for length in table:
            out += unchunk(self._bytes(pos, length))
            pos += length
        if len(out) != size:
            raise SystemExit(f"{name}: unpacked {len(out)} bytes, expected {size}")
        return bytes(out)


def unchunk(blob):
    marker, version, method, enc, csize, dsize, _sum = struct.unpack_from("<IBBBIII", blob, 0)
    if marker != 0x48535153:
        raise SystemExit("bad SQSH chunk")
    body = blob[19:19 + csize]
    if enc:
        body = bytes(((b - i) ^ i) & 0xFF for i, b in enumerate(body))
    if method == 2:
        return zlib.decompress(body)
    if method != 1:
        raise SystemExit(f"unknown chunk method {method}")
    window, wpos, out, i = bytearray(4096), 1, bytearray(), 0
    while i < len(body) and len(out) < dsize:
        tag = body[i]
        i += 1
        for bit in range(8):
            if not tag & (1 << bit):
                if i >= len(body):
                    return bytes(out)
                out.append(body[i]); window[wpos] = body[i]; wpos = (wpos + 1) & 0xFFF; i += 1
            else:
                if i + 1 >= len(body):
                    return bytes(out)
                val = body[i] | (body[i + 1] << 8); i += 2
                src = val >> 4
                if src == 0:
                    return bytes(out)
                for _ in range((val & 15) + 2):
                    b = window[src]; out.append(b); window[wpos] = b
                    wpos = (wpos + 1) & 0xFFF; src = (src + 1) & 0xFFF
    return bytes(out)


def insert(tree: dict, rel: str, data: bytes):
    parts = rel.replace("\\", "/").strip("/").split("/")
    d = tree
    for p in parts[:-1]:
        d = d.setdefault(p, {})
    d[parts[-1]] = data


def walk(d, prefix=""):
    for k, v in d.items():
        if isinstance(v, dict):
            yield from walk(v, prefix + k + "/")
        else:
            yield prefix + k


def main(argv):
    if len(argv) >= 3 and argv[1] == "--list":
        a = Archive(argv[2])
        for key, (off, size, comp, name) in sorted(a.files.items()):
            print(f"{name:<40} {size:>8}  method {comp}")
        return 0
    if len(argv) >= 4 and argv[1] == "--extract":
        a = Archive(argv[2])
        out_dir = Path(argv[4]) if len(argv) > 4 else Path(".")
        for name in argv[3].split(","):
            dst = out_dir / Path(name).name
            dst.write_bytes(a.read(name))
            print(f"{name} -> {dst}")
        return 0
    if len(argv) < 3:
        print(__doc__)
        return 2
    out = Path(argv[1])
    tree = {}
    header_key = 0
    method = 1
    if "--zlib" in argv:
        method = 2
        argv = [a for a in argv if a != "--zlib"]
    if "--key" in argv:
        i = argv.index("--key")
        header_key = int(argv[i + 1], 0)
        del argv[i:i + 2]
    for spec in argv[2:]:
        if "=" in spec:
            rel, _, src = spec.partition("=")
            insert(tree, rel, Path(src).read_bytes())
        else:
            base = Path(spec)
            for f in sorted(base.rglob("*")):
                if f.is_file():
                    insert(tree, str(f.relative_to(base)), f.read_bytes())
    data = build(tree, header_key, method)
    out.write_bytes(data)
    print(f"{out}: {sum(1 for _ in walk(tree))} files, {len(data)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
