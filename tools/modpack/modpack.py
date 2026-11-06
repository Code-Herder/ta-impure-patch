#!/usr/bin/env python3
"""modpack.py -- what the mod manager ships: the catalog, the exe patch pack and each mod's package.

    modpack.py check [CATALOG]          the catalog's schema, standard library only: the DLL's build runs it
    modpack.py tables                   derive and print each mod's exe table and each known exe's revert
    modpack.py loaderdump SETUP         what a setup's Patch Loader really writes, read from a process on Wine
    modpack.py packages [ID ...]        rebuild the packages from the local fixtures (tools/compat/fixtures.json)
    modpack.py catalog                  write mods/catalog.json and mods/patches.bin from mods/<id>.json + packages
    modpack.py verify [ID ...]          the offline proof: tables, reverts, packages against the original manifest,
                                        and the managed mount answering every path as the drop-in folder does

The design is research/notes/mod-manager-plan.md. In short:

EXE TABLES. Every mod runs on the player's own exe. A mod whose exe differs from stock 3.1 (Escalation, TA Zero, TA
Twilight) gets the byte delta, section by section, minus the PE header, .rsrc and the import structures: the renamed
imports (TAESC, EPLAYX, ZDRAW...) belong to TADR's route. A mod with a stock exe (Mayhem, ProTA) patches through a Patch
Loader built with its list inside dplayx.dll as an INI (`[Settings]` keys and `[<file offset>] patch=<hex>` sections,
vendor/Total-Annihilation-Patch-Loader/patches.c), so its table is that list applied.

ONE XOR, BOTH WAYS. A table is stored as its XOR with the stock image, the bytes it leaves as they are dropped. On stock it
gives the mod's image, and on that mod's exe it gives stock, so the pack holds neither image and no pair of its entries
gives one. Only a byte the mod sets to zero reads as the stock byte; `verify` counts them.

KNOWN FILES. The pack lists the modded exes and Patch Loaders Impure knows by file_key. A known modded exe names the XOR
that takes it to stock. A loader names none: the image is restored from the exe on disk, because what a loader writes is
partly rel32 displacements into its own DLLs, which no table can hold (research/notes/mod-manager-plan.md).

PACKAGES. Each is one tar.zst holding <id>.hpi, its recipe <id>.originals.tsv and any plain files the DLL itself reads
(Icon/, a mod's strategic icons). The fixture setup is simulated twice by the engine's rules (tools/wiki/tavfs.py), over the
retail archives as they really are: as the drop-in folder (retail files overlaid by the mod's, the mod's first mount pattern, loose files first) and as the
managed mount (the package first, then the retail archives the drop-in folder did not lose). The package's archive holds
exactly the paths where a mod file wins in the drop-in folder and its bytes are neither what the managed mount already
answers from retail nor an original file.

THE RECIPE. A mod's archive also carries hundreds of byte-identical copies of original files: under its own folders
(guim/armbuild.gui is guis/armbuild.gui), or to prefer one original version over another. Those cannot be hosted, so the
recipe says where each one is in the player's own archives ("<path> <md5> <size> <archive> <member>"), and the install
copies them into <id>.originals.hpi, mounted right after <id>.hpi. With it the two simulations agree path by path and
the package carries nothing original. DLLs and executables are dropped; what else a mod has that the engine reads by
plain path, or nobody does, is named in the package's listing.txt.

Packages, listings and anything else derived from a mod's files go to the fixture cache beside the mods, outside the
repository: third-party bytes must never be one `git add -A` away from a commit.

mods/patches.bin, little-endian, zstd compressed (read by the DLL through MODPAK):
    "MPAT" | u32 version | u32 nxors | u32 nmods | u32 nknown
    xor:   u32 nruns | runs
    mod:   u8 idlen | id | 32 B sha256 of the patched image (image_hash) | u32 xor
    known: u8 kind (0 exe file, 1 loader DLL) | 32 B key (file_key of it) | i32 xor taking it to stock, -1 for a loader
    run:   u32 va | u16 len | bytes
"""
import argparse
import fnmatch
import hashlib
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
MODS = ROOT / "mods"
CACHE = Path(os.environ.get("TACOMPAT_CACHE", Path.home() / ".local/share/ta-compat"))
OUT = CACHE / "modpack"
ID_RE = re.compile(r"^[a-z][a-z0-9_-]{0,31}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")
VERSION = 1


def main_checkout() -> Path:
    common = subprocess.run(["git", "rev-parse", "--git-common-dir"], cwd=ROOT, capture_output=True, text=True, check=True)
    return (ROOT / common.stdout.strip()).resolve().parent


def die(msg):
    sys.exit(f"modpack: {msg}")


# ---------------------------------------------------------------------------- the PE image

class Pe:
    """A 32-bit PE file: its sections and the import structures the loader fills."""

    def __init__(self, data: bytes):
        self.data = data
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        nsec, opt = struct.unpack_from("<H12xH", data, pe + 6)
        o = pe + 24
        self.base = struct.unpack_from("<I", data, o + 28)[0]
        self.imp_rva, self.imp_size = struct.unpack_from("<II", data, o + 96 + 8)
        self.sections = []                      # (name, va, raw offset, raw size), .rsrc left out
        for i in range(nsec):
            s = o + opt + i * 40
            name = data[s:s + 8].split(b"\0")[0].decode("latin-1")
            vaddr, rsize, raddr = struct.unpack_from("<III", data, s + 12)
            if name != ".rsrc" and rsize:
                self.sections.append((name, self.base + vaddr, raddr, rsize))

    def off(self, va: int):
        for _, v, r, n in self.sections:
            if v <= va < v + n:
                return r + va - v
        return None

    def cstr(self, va: int) -> bytes:
        o = self.off(va)
        return self.data[o:self.data.index(b"\0", o)]

    def mask(self) -> list:
        """The VA ranges the loader writes or that name imports: the descriptors, the lookup tables and the slots, the
        names. A loader-filled slot is no patch and an import's name is a route's, so neither is part of a table."""
        out, b = [], self.base
        d = b + self.imp_rva
        out.append((d, d + self.imp_size))
        while True:
            o = self.off(d)
            ilt, _, _, name, iat = struct.unpack_from("<IIIII", self.data, o)
            if not name:
                break
            n = self.cstr(b + name)
            out.append((b + name, b + name + len(n) + 1))
            for table in (ilt, iat):
                t = b + table
                while True:
                    v = struct.unpack_from("<I", self.data, self.off(t))[0]
                    out.append((t, t + 4))
                    t += 4
                    if not v:
                        break
                    if not v & 0x80000000:      # a hint and a name
                        h = b + v
                        s = self.cstr(h + 2)
                        out.append((h, h + 2 + len(s) + 1))
            d += 20
        return out


def masked_set(*pes) -> set:
    s = set()
    for p in pes:
        for a, b in p.mask():
            s.update(range(a, b))
    return s


def image(pe: Pe) -> list:
    """The sections' raw bytes, [(va, bytearray)]."""
    return [(v, bytearray(pe.data[r:r + n])) for _, v, r, n in pe.sections]


def image_put(img, va: int, b: bytes):
    for v, buf in img:
        if v <= va and va + len(b) <= v + len(buf):
            buf[va - v:va - v + len(b)] = b
            return
    die(f"0x{va:X} ({len(b)} B) is in no section of the image")


def image_hash(img, mask: set) -> bytes:
    """SHA-256 of the sections' raw bytes, the masked ones as zero: the hash the DLL computes on its own mapped image."""
    h = hashlib.sha256()
    for v, buf in img:
        b = bytearray(buf)
        for va in mask:
            if v <= va < v + len(b):
                b[va - v] = 0
        h.update(b)
    return h.digest()


def runs_of(pairs: dict) -> list:
    """{va: byte} to [(va, bytes)], adjacent bytes one run, none past a u16's length."""
    runs, cur_va, cur = [], None, bytearray()
    for va in sorted(pairs):
        if cur and va == cur_va + len(cur) and len(cur) < 0xFFFF:
            cur.append(pairs[va])
        else:
            if cur:
                runs.append((cur_va, bytes(cur)))
            cur_va, cur = va, bytearray([pairs[va]])
    if cur:
        runs.append((cur_va, bytes(cur)))
    return runs


def xor_of(stock: Pe, table: dict) -> dict:
    """A table as its XOR with stock, the bytes it leaves as they are dropped."""
    out = {}
    for va, b in table.items():
        x = b ^ stock.data[stock.off(va)]
        if x:
            out[va] = x
    return out


def image_xor(img, va: int, b: bytes):
    for i, x in enumerate(b):
        for v, buf in img:
            if v <= va + i < v + len(buf):
                buf[va + i - v] ^= x
                break
        else:
            die(f"0x{va + i:X} is in no section of the image")


def delta(stock: Pe, mod: Pe, mask: set) -> dict:
    """{va: the mod's byte} wherever the mod's exe differs from stock outside the mask."""
    if [s[1:] for s in stock.sections] != [s[1:] for s in mod.sections]:
        die("a mod exe's section layout differs from stock 3.1: no byte table can hold it")
    out = {}
    for _, v, r, n in stock.sections:
        a, b = stock.data[r:r + n], mod.data[r:r + n]
        if a == b:
            continue
        for i in range(n):
            if a[i] != b[i] and v + i not in mask:
                out[v + i] = b[i]
    return out


# ---------------------------------------------------------------------------- the Patch Loader's list

# The sites of its named keys, as VAs (patches.c, patches_apply_presets).
LOADER_STR = {"registrypath": ((0x50DDFD, 0x509EB8), 21), "configfilename": ((0x5098A3,), 12), "gp3filename": ((0x5028CC,), 11),
              "downloadpath": ((0x503730,), 11), "gameversionstring": ((0x5031B4,), 7)}
LOADER_BYTE = {"multiplayerversionmajor": 0x49E9C0, "multiplayerversionminor": 0x49E9C9}
LOADER_YES = {"commanderordersnotreset": ((0x406FB4, b"\xEB\x40"),),
              "changepathfindingsearch": ((0x40EAD6, struct.pack("<I", 66650)),),
              "directxpopupelimination": ((0x4266A5, b"\xB0\x01"),),
              "cursorreclaim": ((0x43E786, b"\0\0"),),
              "disablekeylastcommandrepeat": ((0x4966E7, b"\x27"),),
              "enablef10debug": ((0x496776, b"\x24"), (0x496779, b"\x0D")),
              "increaseatmtofillresources": ((0x4FCC7F, b"\xD4"),)}
LOADER_PROXY_BYTE = (0x401064, b"\x01")     # dllmain.c: tells tdraw.dll the proxy is live


def file_offset_to_va(a: int):
    """patches.h GET_MEM_ADDRESS: the sections' offset deltas; None for an offset outside them."""
    if a >= 0x0FF600 + 0x10A00 or a < 0x400:
        return None
    d = 0x1A00 if a >= 0x0FF600 else 0x1200 if a >= 0x0FAE00 else 0xC00
    return 0x400000 + a + d


def loader_ini(dll: bytes) -> str:
    i = dll.find(b"[Settings]")
    if i < 0:
        die("no patch list inside this dplayx.dll")
    return dll[i:dll.index(b"\0", i)].decode("latin-1")


def loader_table(dll: bytes) -> dict:
    """{va: byte} of a Patch Loader's built-in list, keys and customs alike, and the proxy byte. The two DirectDraw call
    sites it also patches lead into tdraw.dll, which a managed mod does not have: they are Impure's, not the table's."""
    out = {}

    def put(va, b):
        for i, x in enumerate(b):
            if va + i in out and out[va + i] != x:
                die(f"the loader list writes 0x{va + i:X} twice")
            out[va + i] = x

    section = None
    for raw in loader_ini(dll).splitlines():
        line = raw.split(";", 1)[0].strip()
        if not line:
            continue
        m = re.match(r"^\[([^\]]+)\]$", line)
        if m:
            section = m.group(1)
            continue
        if "=" not in line:
            die(f"loader list: cannot read {line!r}")
        k, v = (p.strip() for p in line.split("=", 1))
        low = k.lower()
        if section.lower() == "settings":
            if low in LOADER_STR:
                sites, cap = LOADER_STR[low]
                if len(v) > cap:
                    die(f"loader list: {k} is longer than its {cap} characters")
                for s in sites:
                    put(s, v.encode("latin-1") + b"\0")
            elif low in LOADER_BYTE:
                put(LOADER_BYTE[low], bytes([int(v)]))
            elif low in LOADER_YES:
                if v.lower() == "yes":
                    for s, b in LOADER_YES[low]:
                        put(s, b)
                elif v.lower() != "no":
                    die(f"loader list: {k}={v}")
            else:
                die(f"loader list: unknown key {k}")
        elif low == "patch":
            va = file_offset_to_va(int(section, 16))
            if va is None:
                die(f"loader list: file offset {section} is outside the image")
            put(va, bytes.fromhex(v))
    put(*LOADER_PROXY_BYTE)
    return out


# ---------------------------------------------------------------------------- the sources

def tacompat():
    sys.path.insert(0, str(ROOT / "tools/compat"))
    import tacompat as t
    return t


def tavfs():
    sys.path.insert(0, str(ROOT / "tools/wiki"))
    import tavfs as t
    return t


def hpipack():
    sys.path.insert(0, str(ROOT / "tools"))
    import hpipack as h
    return h


def stock_exe() -> bytes:
    return (main_checkout() / "pristine/TotalA.exe.pristine").read_bytes()


def mod_sources(ids=None) -> list:
    out = []
    for p in sorted(MODS.glob("*.json")):
        if p.name == "catalog.json":
            continue
        j = json.loads(p.read_text())
        if j.get("id") != p.stem:
            die(f"{p.name}: its id is {j.get('id')!r}")
        if not ids or j["id"] in ids:
            out.append(j)
    if ids and len(out) != len(ids):
        die(f"no such mod among {sorted(ids)}")
    return out


def setup_files(setup_name: str) -> dict:
    """{lower name: (name as installed, cached file)} of a compat setup, without the retail game."""
    t = tacompat()
    for s in t.SETUPS:
        if s["name"] == setup_name:
            return {dest.lower(): (dest, Path(src)) for dest, src in t.overlay(s)}
    die(f"no compat setup {setup_name}")


def mod_table(src: dict, stock: Pe):
    """The mod's {va: byte} and its exe file's Pe (stock when it has none of its own)."""
    files = setup_files(src["setup"])
    if "totala.exe" in files:
        exe = Pe(files["totala.exe"][1].read_bytes())
    else:
        exe = stock
    if src["exe"] == "loader":
        if exe.data != stock.data:
            die(f"{src['id']}: a loader mod's exe is not stock 3.1")
        return loader_table(files["dplayx.dll"][1].read_bytes()), exe
    if src["exe"] != "file":
        die(f"{src['id']}: exe is {src['exe']!r}")
    return delta(stock, exe, masked_set(stock, exe)), exe


def apply(stock: Pe, table: dict):
    img = image(stock)
    for va, b in runs_of(table):
        image_put(img, va, b)
    return img


def file_key(data: bytes, is_exe: bool) -> bytes:
    """A known file's identity. An exe's ignores the PE checksum and Characteristics, which the 4 GB copy changes."""
    if is_exe:
        d = bytearray(data)
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        d[pe + 22:pe + 24] = b"\0\0"
        d[pe + 24 + 64:pe + 24 + 68] = b"\0\0\0\0"
        return hashlib.sha256(d).digest()
    return hashlib.sha256(data).digest()


def known_files(stock: Pe):
    """[(kind, name, key, {va: xor} or None)]: every known modded exe of a setup, with the XOR to stock, and every known
    loader, which needs none: the image comes back from the exe on disk."""
    t = tacompat()
    seen, out = set(), []
    for s in t.SETUPS:
        for dest, src in t.overlay(s):
            low, p = dest.lower(), Path(src)
            if low == "totala.exe":
                d = p.read_bytes()
                k = file_key(d, True)
                if k in seen or d == stock.data:
                    continue
                seen.add(k)
                exe = Pe(d)
                out.append((0, f"{s['name']} exe", k, xor_of(stock, delta(stock, exe, masked_set(stock, exe)))))
            elif low == "dplayx.dll":
                d = p.read_bytes()
                if b"[Settings]" not in d:
                    continue
                k = file_key(d, False)
                if k in seen:
                    continue
                seen.add(k)
                out.append((1, f"{s['name']} loader", k, None))
    return out


def pack_patches(mods: list, known: list) -> bytes:
    """mods/patches.bin before compression, from [(id, image hash, {va: xor})] and known_files. Equal XORs are one entry,
    so a mod and its exe share theirs: the pack holds no table beside the same table's revert."""
    xors, index = [], {}

    def xor_id(x: dict) -> int:
        key = tuple(sorted(x.items()))
        if key not in index:
            index[key] = len(xors)
            xors.append(x)
        return index[key]

    mod_rows = [(mid, h, xor_id(x)) for mid, h, x in mods]
    known_rows = [(kind, key, -1 if x is None else xor_id(x)) for kind, _, key, x in known]
    blob = bytearray(b"MPAT" + struct.pack("<IIII", VERSION, len(xors), len(mod_rows), len(known_rows)))
    for x in xors:
        runs = runs_of(x)
        blob += struct.pack("<I", len(runs))
        for va, b in runs:
            blob += struct.pack("<IH", va, len(b)) + b
    for mid, h, i in mod_rows:
        blob += bytes([len(mid)]) + mid.encode() + h + struct.pack("<I", i)
    for kind, key, i in known_rows:
        blob += bytes([kind]) + key + struct.pack("<i", i)
    return bytes(blob)


def read_patches(blob: bytes) -> dict:
    """The format above, read to its exact end, as the DLL must."""
    if blob[:4] != b"MPAT":
        die("patches.bin: no magic")
    version, nx, nm, nk = struct.unpack_from("<IIII", blob, 4)
    if version != VERSION:
        die(f"patches.bin: version {version}")
    at = 20
    xors = []
    for _ in range(nx):
        n = struct.unpack_from("<I", blob, at)[0]
        at += 4
        runs = []
        for _ in range(n):
            va, ln = struct.unpack_from("<IH", blob, at)
            runs.append((va, blob[at + 6:at + 6 + ln]))
            at += 6 + ln
        xors.append(runs)
    mods = []
    for _ in range(nm):
        ln = blob[at]
        mid, h = blob[at + 1:at + 1 + ln].decode(), blob[at + 1 + ln:at + 33 + ln]
        mods.append((mid, h, struct.unpack_from("<I", blob, at + 33 + ln)[0]))
        at += 37 + ln
    known = []
    for _ in range(nk):
        known.append((blob[at], blob[at + 1:at + 33], struct.unpack_from("<i", blob, at + 33)[0]))
        at += 37
    if at != len(blob):
        die(f"patches.bin: {len(blob) - at} B past the last entry")
    return {"xors": xors, "mods": mods, "known": known}


# ---------------------------------------------------------------------------- the mount, simulated

def manifest() -> set:
    """The hash of every file of the original game, _local/original-manifest.tsv. One row per hash, so it says WHETHER some
    bytes are original and never what an archive holds: retail_tables reads that from the archives."""
    hashes = set()
    for line in (main_checkout() / "_local/original-manifest.tsv").read_text().splitlines():
        md5, size, _ = line.split("\t", 2)
        if int(size):
            hashes.add(md5)
    return hashes


def member_sizes() -> dict:
    """{md5: size} of the original game's files."""
    out = {}
    for line in (main_checkout() / "_local/original-manifest.tsv").read_text().splitlines():
        md5, size, _ = line.split("\t", 2)
        out[md5] = int(size)
    return out


def member_table(path: Path) -> dict:
    """{lower path: (md5, name as stored)} of an archive; a file too short to be one has none."""
    if not tavfs()._has_trailer(path):
        return {}
    a = hpipack().Archive(path)
    return {k: (hashlib.md5(a.read(k)).hexdigest(), a.files[k][3]) for k in a.files}


def retail_tables(names: dict) -> dict:
    """{archive lower: member_table} of the retail folder's archives, {name: path}. Read once and kept beside the packages,
    by name, size and date: every archive decodes in Python, which takes minutes."""
    cache = OUT / "retail-members.json"
    old = json.loads(cache.read_text()) if cache.is_file() else {}
    out, new = {}, {}
    for name, p in sorted(names.items()):
        if p.suffix.lower() not in (".gp3", ".ccx", ".ufo", ".hpi"):
            continue
        st = p.stat()
        stamp = [st.st_size, st.st_mtime_ns]
        row = old.get(name.lower())
        if not row or row["stamp"] != stamp:
            row = {"stamp": stamp, "members": member_table(p)}
        new[name.lower()] = row
        out[name.lower()] = {k: tuple(v) for k, v in row["members"].items()}
    if new != old:
        OUT.mkdir(parents=True, exist_ok=True)
        cache.write_text(json.dumps(new))
    return out


def leaf(stored: str) -> str:
    """The name an enumeration reads (0x4BCA30): its case is the player's to see, as a map's name in the list."""
    return stored.rsplit("/", 1)[-1]


def mount_order(names, first):
    """InitTAHPIAry's order over bare file names (tavfs.py, the mount list), each once."""
    out, seen = [], set()
    ordered = sorted(names, key=str.lower)
    for pat in (first, "*.ccx", "*.ufo", "*.hpi"):
        for n in ordered:
            if fnmatch.fnmatch(n.lower(), pat.lower()) and n.lower() not in seen:
                seen.add(n.lower())
                out.append(n)
    return out


def first_pattern(img, pe: Pe) -> str:
    """The first mount pattern as the patched image spells it (0x5028CC, formatted with 0x5028D8)."""
    def s(va):
        for v, buf in img:
            if v <= va < v + len(buf):
                b = bytes(buf[va - v:])
                return b[:b.index(b"\0")].decode("latin-1")
        die(f"0x{va:X} is in no section")
    f = s(0x5028CC)
    return f.replace("%s", s(0x5028D8), 1) if "%s" in f else f


class Plan:
    """One mod's drop-in folder and managed mount, simulated."""

    def __init__(self, src: dict, stock: Pe):
        v = tavfs()
        self.src = src
        self.files = setup_files(src["setup"])
        table, exe = mod_table(src, stock)
        self.table = table
        self.img = apply(stock, table)
        self.first = first_pattern(self.img, stock)
        retail_dir = v.find_install(None)
        self.retail = {p.name: p for p in retail_dir.iterdir() if p.is_file()}
        # the folder, by bare name: the mod's file replaces a retail one of the same name, without case
        folder = {n.lower(): (n, p, True) for n, p in self.retail.items()}
        for low, (name, p) in self.files.items():
            if "/" not in name.replace("\\", "/"):
                folder[low] = (name, p, False)
        self.mount = []                         # drop-in: (name, path, is the mod's)
        for n in mount_order([f[0] for f in folder.values()], self.first):
            name, p, is_retail = folder[n.lower()]
            self.mount.append((name, p, not is_retail))
        # the retail archives the drop-in folder lost: replaced, or no longer named by a pattern
        kept = {n.lower() for n, _, mine in self.mount if not mine}
        self.retail_mount = [n for n in mount_order(list(self.retail), "rev31.gp3")]
        self.hide = sorted(n for n in self.retail_mount if n.lower() not in kept)
        self.managed_retail = [n for n in self.retail_mount if n.lower() in kept]
        self.loose = {}                         # the mod's loose game data: lower path -> (name as installed, file)
        self.plain = []                         # (name, why dropped)
        self.kept = {}                          # the plain files the package carries: name -> file
        for low, (name, p) in sorted(self.files.items()):
            ext = low.rsplit(".", 1)[-1] if "." in low.rsplit("/", 1)[-1] else ""
            if ext in ("dll", "exe"):
                self.plain.append((name, "code: mod DLLs are never installed"))
            elif "/" not in low.replace("\\", "/") and ext in ("gp3", "ccx", "ufo", "hpi"):
                pass                            # an archive of the mount
            elif "/" in low.replace("\\", "/") and low.split("/")[0] in GAME_DIRS:
                self.loose[low.replace("\\", "/")] = (name.replace("\\", "/"), p)
            elif low.replace("\\", "/").split("/")[0] in PLAIN_KEPT:
                self.kept[name.replace("\\", "/")] = p
            else:
                self.plain.append((name, "read by plain path or not at all"))

    def retail_name(self, low: str) -> str:
        return next(n for n in self.retail if n.lower() == low)

    def dropin_winners(self, tables) -> dict:
        """{lower path: (md5, source, name as stored)} as the drop-in folder answers: loose first, then the first archive
        in mount order."""
        out = {}
        for k, (name, p) in self.loose.items():
            out[k] = (hashlib.md5(p.read_bytes()).hexdigest(), "loose", name)
        for name, p, mine in self.mount:
            for k, (md5, stored) in tables(name, p, mine).items():
                out.setdefault(k, (md5, name, stored))
        return out


# The folders the engine reads through its file open (0x4BB2E0): a loose file in one of them is game data.
GAME_DIRS = {"anims", "bitmaps", "camera", "camps", "cdplayer", "download", "features", "fonts", "gamedata", "guis", "maps", "objects3d",
             "palettes", "scripts", "sections", "sounds", "textures", "unitpics", "units", "ai", "weapons", "unitscript", "unitsb", "cursors"}


# The folders of plain files a package carries, because Impure reads them itself: a mod's strategic icons.
PLAIN_KEPT = {"icon", "zicon"}


def plan_package(src, stock, hashes):
    """The package's members, by the module's rule: the paths where a mod file wins in the drop-in folder, unless retail
    already answers them with the same bytes and name under the managed mount. Returns (plan, wanted, dropin, stats)."""
    plan = Plan(src, stock)
    retail = retail_tables(plan.retail)
    cache = {}

    def tables(name, p, mine):
        if mine and name not in cache:
            cache[name] = member_table(p)
        return cache[name] if mine else retail.get(name.lower(), {})

    dropin = plan.dropin_winners(tables)
    managed_retail = {}
    for name in plan.managed_retail:
        for k, row in retail[name.lower()].items():
            managed_retail.setdefault(k, row)
    # where the install finds each original file: the first archive in the retail mount order, the rest after it, and
    # a retail archive only, because a recipe can copy from nothing else
    by_md5 = {}
    mounted = [n.lower() for n in plan.retail_mount]
    for arc in mounted + sorted(set(retail) - set(mounted)):
        for k, (md5, stored) in retail[arc].items():
            by_md5.setdefault(md5, (plan.retail_name(arc), stored))
    wanted, stripped, recipe, loose_original = {}, 0, [], []
    for k, (md5, where, stored) in sorted(dropin.items()):
        row = managed_retail.get(k)
        if row and row[0] == md5 and leaf(row[1]) == leaf(stored):
            stripped += 1
        elif md5 not in hashes:
            wanted[k] = (where, stored)
        elif md5 in by_md5:
            recipe.append((stored, md5, *by_md5[md5]))   # the original game's bytes, not at this path under the managed mount
        else:
            loose_original.append(k)                # an original file with no archive to copy it from
    return plan, wanted, dropin, {"stripped": stripped, "recipe": recipe, "loose_original": loose_original,
                                  "managed_retail": managed_retail, "retail": retail}


# ---------------------------------------------------------------------------- the commands

def cmd_tables(_):
    stock = Pe(stock_exe())
    for s in mod_sources():
        tab, _ = mod_table(s, stock)
        runs = runs_of(tab)
        print(f"{s['id']:10} exe={s['exe']:6} {len(tab):5} bytes in {len(runs):4} runs; first pattern {first_pattern(apply(stock, tab), stock)}")
    for kind, name, key, xor in known_files(stock):
        print(f"known {'exe   ' if kind == 0 else 'loader'} {name:32} {key.hex()[:12]}" + ("" if xor is None else f" {len(xor):5} bytes in {len(runs_of(xor)):4} runs"))


def cmd_verify(a):
    stock = Pe(stock_exe())
    mask = masked_set(stock)
    bad = 0

    def report(ok, line):
        nonlocal bad
        bad += not ok
        print(("ok   " if ok else "FAIL ") + line)

    # each table reproduces its mod's exe from stock; a loader's reproduces what its list says, by construction, and
    # `loaderdump` measures it against a process
    for s in mod_sources(a.ids or None):
        tab, exe = mod_table(s, stock)
        m = masked_set(stock, exe)
        img = apply(stock, tab)
        if s["exe"] == "file":
            report(image_hash(img, m) == image_hash(image(exe), m), f"{s['id']}: the table on stock gives the mod's exe, imports aside")
        report(all(va not in mask for va in tab), f"{s['id']}: the table writes no import structure ({len(tab)} B)")
    # the committed pack, read as the DLL reads it: each XOR gives its mod from stock and stock from its known exe
    pack = read_patches(subprocess.run(["zstd", "-dqc", str(MODS / "patches.bin")], capture_output=True, check=True).stdout)
    for mid, h, i in pack["mods"]:
        img = image(stock)
        for va, b in pack["xors"][i]:
            image_xor(img, va, b)
        report(image_hash(img, mask) == h, f"{mid}: its XOR on stock gives the image hash the pack names")
    exes = {file_key(Path(p).read_bytes(), True): Path(p).read_bytes() for st in tacompat().SETUPS for dest, p in tacompat().overlay(st) if dest.lower() == "totala.exe"}
    for kind, key, i in pack["known"]:
        if kind:
            report(i == -1, f"loader {key.hex()[:12]}: named with no table")
            continue
        src = Pe(exes[key])
        img = image(src)
        m = masked_set(stock, src)
        for va, b in pack["xors"][i]:
            image_xor(img, va, b)
        report(image_hash(img, m) == image_hash(image(stock), m), f"exe {key.hex()[:12]}: its XOR gives stock 3.1")
    # what the pack reads as stock on its own: the bytes a mod sets to zero, and nothing else
    worst, total = 0, 0
    for runs in pack["xors"]:
        for va, b in runs:
            at = 0
            for i, x in enumerate(b):
                if stock.data[stock.off(va + i)] == x:
                    total += 1
                    at += 1
                    worst = max(worst, at)
                else:
                    at = 0
    print(f"note: {total} B of the pack's XORs read as the stock byte (a mod sets them to zero); longest run {worst} B")
    # the packages against the original manifest, and the managed mount against the drop-in folder
    hashes = manifest()
    for s in mod_sources(a.ids or None):
        tar = OUT / s["id"] / f"{s['id']}.tar.zst"
        if not tar.is_file():
            report(False, f"{s['id']}: no package at {tar}: modpack.py packages")
            continue
        plan, wanted, dropin, st = plan_package(s, stock, hashes)
        with tempfile.TemporaryDirectory(dir=OUT) as d:
            subprocess.run(["zstd", "-dqf", str(tar), "-o", d + "/p.tar"], check=True)
            with tarfile.open(d + "/p.tar") as t:
                t.extractall(d, filter="data")
                names = t.getnames()
            ha = Path(d) / f"{s['id']}.hpi"
            got = member_table(ha)
            recipe = (Path(d) / f"{s['id']}.originals.tsv").read_text().splitlines()
            carried = [hashlib.md5((Path(d) / n).read_bytes()).hexdigest() for n in names]
        hit = [n for n, h in zip(names, carried) if h in hashes]
        report(not hit, f"{s['id']}: no file of the package is an original file ({len(names)} files)" + (f"; first {hit[0]}" if hit else ""))
        hits = [k for k, (md5, _) in got.items() if md5 in hashes]
        report(not hits, f"{s['id']}: no member of its archive is an original file ({len(got)} members)" + (f"; first {hits[0]}" if hits else ""))
        # the recipe, as the install runs it: every member named is the player's own, with the bytes it says
        assembled, bad_recipe = {}, []
        for line in recipe:
            path, md5, size, arc, member = line.split("\t")
            if st["retail"].get(arc.lower(), {}).get(member.lower(), (None,))[0] != md5:
                bad_recipe.append(path)
            assembled[path.lower()] = (md5, path)
        report(not bad_recipe, f"{s['id']}: the recipe's {len(recipe)} files are in the retail archives it names"
               + (f"; first {bad_recipe[0]}" if bad_recipe else ""))
        managed = dict(st["managed_retail"])
        managed.update(assembled)
        managed.update(got)
        wrong = [k for k, (md5, _, stored) in dropin.items() if k not in managed or managed[k][0] != md5]
        cased = [k for k, (md5, _, stored) in dropin.items() if k in managed and leaf(managed[k][1]) != leaf(stored)]
        extra = [k for k in managed if k not in dropin]
        report(not wrong and not extra, f"{s['id']}: the managed mount answers {len(dropin)} paths as the drop-in folder does"
               + (f"; differ {len(wrong)}, first {wrong[0]}" if wrong else "") + (f"; only managed {len(extra)}, first {extra[0]}" if extra else ""))
        report(not cased, f"{s['id']}: every file name keeps its case" + (f"; differ {len(cased)}, first {cased[0]}" if cased else ""))
    print("verify: " + ("all passed" if not bad else f"{bad} FAILED"))
    return 1 if bad else 0


def write_tar(files: dict, out: Path):
    """{name: bytes} in a tar, by name, their headers fixed, zstd compressed: the same inputs give the same package."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as t:
        for name in sorted(files):
            ti = tarfile.TarInfo(name)
            ti.size, ti.mtime, ti.mode = len(files[name]), 0, 0o644
            t.addfile(ti, io.BytesIO(files[name]))
    tmp = out.with_suffix(".tar")
    tmp.write_bytes(buf.getvalue())
    subprocess.run(["zstd", "-qf", "-15", "-T0", str(tmp), "-o", str(out)], check=True)
    tmp.unlink()


def insert_cased(tree: dict, stored: str, data: bytes):
    """A file under its stored name, in the folders already there whatever their case: the engine reads an archive's folder
    as one sorted list without case, so "Maps" beside "maps" would hide one of them."""
    parts = stored.split("/")
    d = tree
    for part in parts[:-1]:
        name = next((n for n in d if n.lower() == part.lower()), part)
        d = d.setdefault(name, {})
    d[parts[-1]] = data


def cmd_packages(a):
    stock = Pe(stock_exe())
    hashes = manifest()
    for s in mod_sources(a.ids or None):
        t0 = time.time()
        plan, wanted, dropin, st = plan_package(s, stock, hashes)
        tree, opened, cost = {}, {}, 0
        src_by_name = {n: p for n, p, _ in plan.mount}
        for k, (where, stored) in sorted(wanted.items()):
            if where == "loose":
                data = plan.loose[k][1].read_bytes()
            else:
                arc = opened.get(where) or hpipack().Archive(src_by_name[where])
                opened[where] = arc
                data = arc.read(k)
            insert_cased(tree, stored, data)
            cost += len(data)
        d = OUT / s["id"]
        d.mkdir(parents=True, exist_ok=True)
        pkg = {f"{s['id']}.hpi": hpipack().build(tree, 0, 2)}
        sizes = member_sizes()
        pkg[f"{s['id']}.originals.tsv"] = "".join(f"{path}\t{md5}\t{sizes[md5]}\t{arc}\t{member}\n" for path, md5, arc, member in st["recipe"]).encode()
        for name, p in plan.kept.items():
            pkg[name] = p.read_bytes()
        tar = d / f"{s['id']}.tar.zst"
        write_tar(pkg, tar)
        lines = [f"package {s['id']}: {len(wanted)} members, {cost} bytes; {st['stripped']} paths answered by retail; hide: {plan.hide}",
                 f"first mount pattern: {plan.first}"]
        lines += [f"dropped {n}: {why}" for n, why in plan.plain]
        lines += [f"original with no archive to copy it from: {k}" for k in st["loose_original"]]
        (d / "listing.txt").write_text("\n".join(lines) + "\n")
        print(f"{s['id']:10} {len(wanted):6} members {cost / 1e6:8.1f} MB -> {tar.stat().st_size / 1e6:7.1f} MB in {time.time() - t0:5.1f} s; "
              f"{st['stripped']} from retail, {len(st['recipe'])} by recipe, {len(st['loose_original'])} unsourced, {len(plan.plain)} dropped; hide {plan.hide}")


def sha_of(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def cmd_catalog(_):
    stock = Pe(stock_exe())
    mask = masked_set(stock)
    entries, mods_bin = [], []
    for s in mod_sources():
        tab, _ = mod_table(s, stock)
        img = apply(stock, tab)
        plan = Plan(s, stock)
        tar = OUT / s["id"] / f"{s['id']}.tar.zst"
        if not tar.is_file():
            die(f"{s['id']}: no package: modpack.py packages")
        with tempfile.TemporaryDirectory(dir=OUT) as d:
            subprocess.run(["zstd", "-dqf", str(tar), "-o", d + "/p.tar"], check=True)
            with tarfile.open(d + "/p.tar") as t:
                t.extractall(d, filter="data")
                declared = [{"path": n, "sha256": sha_of(Path(d) / n), "size": (Path(d) / n).stat().st_size} for n in t.getnames()]
        e = {k: s[k] for k in ("id", "kind", "name", "author", "version", "summary", "homepage", "permission")}
        e.update({"packages": [{"url": f"{s['id']}/{s['id']}-{s['version'].replace(' ', '-')}.tar.zst", "sha256": sha_of(tar), "size": tar.stat().st_size,
                                "files": declared}],
                  "exe": s["id"], "hide": plan.hide})
        for k in ("memory", "options"):
            if k in s:
                e[k] = s[k]
        entries.append(e)
        mods_bin.append((s["id"], image_hash(img, mask), xor_of(stock, tab)))
    (MODS / "catalog.json").write_text(json.dumps({"version": VERSION, "mods": entries}, indent=2) + "\n")
    known = known_files(stock)
    blob = pack_patches(mods_bin, known)
    p = subprocess.run(["zstd", "-q", "-19", "--no-check", "-c"], input=blob, capture_output=True, check=True)
    (MODS / "patches.bin").write_bytes(p.stdout)
    print(f"catalog: {len(entries)} mods; patches.bin {len(blob)} B -> {len(p.stdout)} B, {len(read_patches(blob)['xors'])} XORs, {len(known)} known files")


STUB_C = "__declspec(dllexport) long __stdcall DirectDrawCreate(void* a, void* b, void* c) { return 1; }\n"
STUB_PLAY = """LIBRARY tplayx.dll
EXPORTS
    DirectPlayCreate @1
    DirectPlayEnumerateA @2
    DirectPlayEnumerateW @3
    DirectPlayLobbyCreateA @4
    DirectPlayLobbyCreateW @5
    gdwDPlaySPRefCount @6 DATA
    DirectPlayEnumerate @9
    DllCanUnloadNow PRIVATE
    DllGetClassObject PRIVATE
"""
STUB_PLAY_C = "".join(f"void __stdcall {n}(void) {{}}\n" for n in ("DirectPlayCreate", "DirectPlayEnumerateA", "DirectPlayEnumerateW",
                      "DirectPlayLobbyCreateA", "DirectPlayLobbyCreateW", "DirectPlayEnumerate", "DllCanUnloadNow", "DllGetClassObject")) + "int gdwDPlaySPRefCount;\n"


def cmd_loaderdump(a):
    """Measure what a setup's dplayx.dll writes: its folder with stubs for ddraw.dll, tdraw.dll and tplayx.dll, so only the
    loader's and win32.dll's DllMains write the image, and the exe's entry point setting a sentinel byte and spinning. Every
    static import's DllMain has returned before the entry point runs, so the image read after the sentinel holds all they
    wrote while loading; a process that never sets it is a failure, never a short dump. Headless: no DISPLAY, so no window
    can reach the desktop."""
    stock = Pe(stock_exe())
    mask = masked_set(stock)
    files = setup_files(a.setup)
    gcc = "i686-w64-mingw32-gcc"
    with tempfile.TemporaryDirectory(prefix="loaderdump") as d:
        d = Path(d)
        names = {q.name.lower(): (q.name, q) for q in tavfs().find_install(None).iterdir() if q.is_file() and q.suffix.lower() == ".dll"}
        for low, (name, p) in files.items():
            if "/" not in name.replace("\\", "/") and not low.endswith((".gp3", ".ufo", ".ccx", ".hpi")):
                names[low] = (name, p)
        for name, p in names.values():
            (d / name).symlink_to(p)
        for n in ("ddraw.dll", "tdraw.dll", "tplayx.dll", "TotalA.exe"):
            q = next((x for x in d.iterdir() if x.name.lower() == n.lower()), None)
            if q:
                q.unlink()
        (d / "stub.c").write_text(STUB_C)
        (d / "play.c").write_text(STUB_PLAY_C)
        (d / "play.def").write_text(STUB_PLAY)
        for out in ("ddraw.dll", "tdraw.dll"):
            subprocess.run([gcc, "-shared", "-s", "-Wl,--kill-at", "-o", str(d / out), str(d / "stub.c")], check=True)
        subprocess.run([gcc, "-shared", "-s", "-Wl,--enable-stdcall-fixup", "-o", str(d / "tplayx.dll"), str(d / "play.c"), str(d / "play.def")], check=True)
        exe = bytearray(stock.data)
        entry = 0x4E6FA0
        data = next(sec for sec in stock.sections if sec[0] == ".data")
        sentinel = data[1] + data[3] - 1        # .data's last raw byte
        spin = b"\xC6\x05" + struct.pack("<I", sentinel) + b"\xA5\xEB\xFE"       # mov byte [sentinel], 0xA5; jmp $
        exe[stock.off(entry):stock.off(entry) + len(spin)] = spin
        (d / "TotalA.exe").write_bytes(exe)
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
        # a prefix of its own: the instances' registries share one inode, and nothing here should write to it
        env.update(WINEPREFIX=str(OUT / "prefix"), WINEDEBUG="-all", WINEDLLOVERRIDES="ddraw,dplayx,tdraw,tplayx=n,b;winemenubuilder.exe=d")
        OUT.mkdir(parents=True, exist_ok=True)
        if not (OUT / "prefix" / "system.reg").exists():
            subprocess.run(["wineboot", "-i"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True, timeout=120)
        proc = subprocess.Popen(["wine", "TotalA.exe"], cwd=d, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        pid, dump = None, None
        try:
            for _ in range(120):
                time.sleep(0.5)
                pid = pid or next((int(x.name) for x in Path("/proc").iterdir() if x.name.isdigit() and _exe_mapped(x, d)), None)
                if pid:
                    with open(f"/proc/{pid}/mem", "rb") as m:
                        m.seek(sentinel)
                        if m.read(1) == b"\xA5":
                            break
            else:
                die("the entry point never ran; a loader's error box needs a display, so run it there once" if pid else "the game's process did not appear")
            dump = []
            with open(f"/proc/{pid}/mem", "rb") as m:
                for _, v, _, n in stock.sections:
                    m.seek(v)
                    dump.append((v, m.read(n)))
        finally:
            for victim in (pid, proc.pid):
                try:
                    if victim:
                        os.kill(victim, 9)
                except OSError:
                    pass
    table = loader_table(files["dplayx.dll"][1].read_bytes())
    got = {}
    for (name, v, r, n), (_, mem) in zip(stock.sections, dump):
        ref = stock.data[r:r + n]
        for i in range(n):
            if ref[i] != mem[i] and v + i not in mask and not entry <= v + i < entry + len(spin) and v + i != sentinel:
                got[v + i] = mem[i]
    missing = sorted(va for va, b in table.items() if got.get(va, stock.data[stock.off(va)]) != b)
    extra = {va: b for va, b in got.items() if table.get(va) != b}
    print(f"{a.setup}: the list writes {len(table)} B; the process differs from stock by {len(got)} B outside the imports")
    print(f"  in the list but not in the process: {len(missing)}" + (f", first 0x{missing[0]:X}" if missing else ""))
    print(f"  in the process but not in the list: {len(extra)} B")
    for va, b in runs_of(extra):
        print(f"    0x{va:X}: {b.hex()}")
    return 1 if missing else 0


def _exe_mapped(proc: Path, folder: Path) -> bool:
    try:
        return str(folder / "TotalA.exe") in (proc / "maps").read_text()
    except OSError:
        return False


def check_catalog(j) -> list:
    """Every way the catalog can be wrong, by name. Standard library only: the DLL's build runs it."""
    errs = []
    if j.get("version") != VERSION:
        errs.append(f"version is {j.get('version')!r}")
    seen, mods = set(), j.get("mods")
    if not isinstance(mods, list):
        return errs + ["no mods list"]
    ids = {e.get("id") for e in mods if isinstance(e, dict) and e.get("kind") == "mod"}
    for i, e in enumerate(mods):
        n = e.get("id", f"#{i}")
        for k in ("id", "kind", "name", "author", "version", "summary", "homepage", "permission"):
            if not isinstance(e.get(k), str) or not e[k]:
                errs.append(f"{n}: {k} is missing")
        if not ID_RE.match(str(e.get("id", ""))):
            errs.append(f"{n}: id is not a lower-case name")
        if e.get("id") == "none":
            errs.append(f"{n}: id none is the base of a mutator for no mod")
        if n in seen:
            errs.append(f"{n}: id appears twice")
        seen.add(n)
        pk = e.get("packages")
        if not isinstance(pk, list) or not pk:
            errs.append(f"{n}: no packages")
        else:
            for p in pk:
                if not HEX64.match(str(p.get("sha256", ""))):
                    errs.append(f"{n}: a package's sha256 is missing or malformed")
                if not isinstance(p.get("size"), int) or p["size"] <= 0:
                    errs.append(f"{n}: a package's size is missing")
                if not isinstance(p.get("url"), str) or not p["url"]:
                    errs.append(f"{n}: a package's url is missing")
                files = p.get("files")
                if not isinstance(files, list) or not files:
                    errs.append(f"{n}: a package declares no files")
                else:
                    for f in files:
                        path = str(f.get("path", ""))
                        if not path or path.startswith(("/", "\\")) or ".." in re.split(r"[\\/]", path) or ":" in path:
                            errs.append(f"{n}: a package file's path {path!r} is unsafe")
                        if not HEX64.match(str(f.get("sha256", ""))) or not isinstance(f.get("size"), int):
                            errs.append(f"{n}: package file {path!r} has no sha256 or size")
        kind = e.get("kind")
        if kind == "mod":
            if not isinstance(e.get("exe"), str) or not e["exe"]:
                errs.append(f"{n}: a mod names no exe table")
            if not isinstance(e.get("hide"), list):
                errs.append(f"{n}: a mod has no hide list")
        elif kind == "mutator":
            if not isinstance(e.get("base"), str) or not e["base"]:
                errs.append(f"{n}: a mutator names no base")
            elif e["base"] not in ids and e["base"] != "none":
                errs.append(f"{n}: base {e['base']} is no mod of the catalog")
            if not isinstance(e.get("base_hashes"), dict):
                errs.append(f"{n}: a mutator has no base_hashes")
            if not isinstance(e.get("files"), list):
                errs.append(f"{n}: a mutator has no files")
            if "exe" in e:
                errs.append(f"{n}: a mutator carries no exe patch")
        else:
            errs.append(f"{n}: kind is {kind!r}")
    return errs


def cmd_check(a):
    p = Path(a.catalog) if a.catalog else MODS / "catalog.json"
    try:
        j = json.loads(p.read_text())
    except (OSError, ValueError) as x:
        print(f"modpack check: {p}: {x}", file=sys.stderr)
        return 1
    errs = check_catalog(j)
    for e in errs:
        print(f"modpack check: {e}", file=sys.stderr)
    if not errs:
        print(f"modpack check: clean -- {len(j['mods'])} entries")
    return 1 if errs else 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("check").add_argument("catalog", nargs="?")
    sub.add_parser("tables")
    sub.add_parser("loaderdump").add_argument("setup")
    sub.add_parser("packages").add_argument("ids", nargs="*")
    sub.add_parser("catalog")
    sub.add_parser("verify").add_argument("ids", nargs="*")
    a = p.parse_args()
    return {"check": cmd_check, "tables": cmd_tables, "loaderdump": cmd_loaderdump, "packages": cmd_packages, "catalog": cmd_catalog, "verify": cmd_verify}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main() or 0)
