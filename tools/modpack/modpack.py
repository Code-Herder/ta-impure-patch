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
that takes it to stock. A loader's image is restored from the exe on disk, because what a loader writes is partly rel32
displacements into its own DLLs, which no table can hold (research/notes/mod-manager-plan.md); it names its catalog mod's XOR,
or none, only so the fingerprint can tell which mod a drop-in folder is.

PACKAGES. Each is one tar.zst holding <id>.hpi, its recipe <id>.originals.tsv and the plain files read by plain path that a
mod needs: Icon/, its strategic icons, and the options file the image names, whose [Preferences] the engine reads. The fixture setup is simulated twice by the engine's rules (tools/wiki/tavfs.py), over the
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

mods/patches.bin, little-endian, zstd compressed (read by tagpu_modpak.c):
    "MPAT" | u32 version | u32 nxors | u32 nmods | u32 nknown | u32 nmask | u32 noriginals | 32 B file_key of stock 3.1
    xor:      u32 nruns | runs
    mod:      u8 idlen | id | 32 B image hash | u32 xor | u8 1 when a loader writes it after Impure's attach
    known:    u8 kind (0 exe file, 1 loader DLL) | 32 B key (file_key of it) | i32 xor taking it to stock; a loader's is its catalog mod's, -1 with none
    mask:     u32 va | u32 len, sorted: the import structures of stock and every known exe, which no hash covers
    original: 32 B SHA-256 of an original archive of the game
    run:      u32 va | u16 len | bytes
"""
import argparse
import fcntl
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
RADIUS_MAX = 8                                  # MODRULES_RADIUS_MAX, tagpu_modcat.h


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


def known_mask(stock: Pe, known_exes: list) -> list:
    """The sorted (va, len) ranges of the import structures of stock and every known exe."""
    vas = sorted(masked_set(stock, *known_exes))
    out = []
    for va in vas:
        if out and out[-1][0] + out[-1][1] == va:
            out[-1][1] += 1
        else:
            out.append([va, 1])
    return [tuple(r) for r in out]


def pack_patches(stock_key: bytes, mods: list, known: list, mask: list, originals: list) -> bytes:
    """mods/patches.bin before compression, from [(id, image hash, {va: xor}, loader)], known_files, known_mask and the
    original archives' SHA-256. Equal XORs are one entry, so a mod and its exe share theirs: the pack holds no table beside
    the same table's revert."""
    xors, index = [], {}

    def xor_id(x: dict) -> int:
        key = tuple(sorted(x.items()))
        if key not in index:
            index[key] = len(xors)
            xors.append(x)
        return index[key]

    mod_rows = [(mid, h, xor_id(x), loader) for mid, h, x, loader in mods]
    known_rows = [(kind, key, -1 if x is None else xor_id(x)) for kind, _, key, x in known]
    blob = bytearray(b"MPAT" + struct.pack("<IIIIII", VERSION, len(xors), len(mod_rows), len(known_rows), len(mask), len(originals)) + stock_key)
    for x in xors:
        runs = runs_of(x)
        blob += struct.pack("<I", len(runs))
        for va, b in runs:
            blob += struct.pack("<IH", va, len(b)) + b
    for mid, h, i, loader in mod_rows:
        blob += bytes([len(mid)]) + mid.encode() + h + struct.pack("<IB", i, loader)
    for kind, key, i in known_rows:
        blob += bytes([kind]) + key + struct.pack("<i", i)
    for va, n in mask:
        blob += struct.pack("<II", va, n)
    for sha in originals:
        blob += sha
    return bytes(blob)


def read_patches(blob: bytes) -> dict:
    """The format above, read to its exact end, as the DLL does."""
    if blob[:4] != b"MPAT":
        die("patches.bin: no magic")
    version, nx, nm, nk, nmask, nor = struct.unpack_from("<IIIIII", blob, 4)
    if version != VERSION:
        die(f"patches.bin: version {version}")
    stock_key = blob[28:60]
    at = 60
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
        i, loader = struct.unpack_from("<IB", blob, at + 33 + ln)
        mods.append((mid, h, i, loader))
        at += 38 + ln
    known = []
    for _ in range(nk):
        known.append((blob[at], blob[at + 1:at + 33], struct.unpack_from("<i", blob, at + 33)[0]))
        at += 37
    mask = []
    for _ in range(nmask):
        mask.append(struct.unpack_from("<II", blob, at))
        at += 8
    originals = [blob[at + 32 * i:at + 32 * (i + 1)] for i in range(nor)]
    at += 32 * nor
    if at != len(blob):
        die(f"patches.bin: {len(blob) - at} B past the last entry")
    return {"stock_key": stock_key, "xors": xors, "mods": mods, "known": known, "mask": mask, "originals": originals}


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


# The folder names the engine pushes when it reads game data, which a mod's image can rename; tagpu_modload.c's FOLDER_NAMES.
FOLDER_NAMES = (0x502820, 0x503360, 0x505FE4, 0x503730, 0x50925C, 0x5031BC, 0x5031C8,
                0x503920, 0x503720, 0x504734, 0x5036E4, 0x503740, 0x502E30, 0x502C7C, 0x50392C, 0x504B10)
DOWNLOAD_FOLDER, WEAPON_FOLDER = 0x503730, 0x50392C


def place_read_folders(img) -> tuple:
    """The folders whose files the engine reads by enumeration, where a file's place can decide: features (0x502C7C), the
    weapons, the build menus and the textures (0x503720)."""
    return tuple(image_string(img, va).lower() + "/" for va in (0x502C7C, WEAPON_FOLDER, DOWNLOAD_FOLDER, 0x503720))


def image_string(img, va) -> str:
    for v, buf in img:
        if v <= va < v + len(buf):
            b = bytes(buf[va - v:])
            return b[:b.index(b"\0")].decode("latin-1")
    die(f"0x{va:X} is in no section")


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
        # the options file the image names (0x5098A3): its [Preferences] reach the engine (0x49F5F9) as well as TADR's keys
        ini = next((bytes(buf[0x5098A3 - v:0x5098B0 - v]) for v, buf in self.img if v <= 0x5098A3 < v + len(buf)), b"")
        options_file = ini.split(b"\0")[0].decode("latin-1").lower()
        game_dirs = GAME_DIRS | {image_string(self.img, va).lower() for va in FOLDER_NAMES}
        self.loose = {}                         # the mod's loose game data: lower path -> (name as installed, file)
        self.plain = []                         # (name, why dropped)
        self.kept = {}                          # the plain files the package carries: name -> file
        for low, (name, p) in sorted(self.files.items()):
            ext = low.rsplit(".", 1)[-1] if "." in low.rsplit("/", 1)[-1] else ""
            if ext in ("dll", "exe"):
                self.plain.append((name, "code: mod DLLs are never installed"))
            elif "/" not in low.replace("\\", "/") and ext in ("gp3", "ccx", "ufo", "hpi"):
                pass                            # an archive of the mount
            elif "/" in low.replace("\\", "/") and low.split("/")[0].split("-")[0] in game_dirs:
                self.loose[low.replace("\\", "/")] = (name.replace("\\", "/"), p)
            elif low.replace("\\", "/").split("/")[0] in PLAIN_KEPT or low == options_file:
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
GAME_DIRS = {"unittextures", "anims", "bitmaps", "camera", "camps", "cdplayer", "download", "features", "fonts", "gamedata", "guis", "maps", "objects3d",
             "palettes", "scripts", "sections", "sounds", "textures", "unitpics", "units", "ai", "weapons", "unitscript", "unitsb", "cursors"}


# The folders of plain files a package carries, because Impure reads them itself: a mod's strategic icons.
PLAIN_KEPT = {"icon", "zicon"}


# Every path has one answer, which the managed mount reproduces, but a name INSIDE a file can depend on the file's PLACE in the
# mount, and one archive cannot keep the drop-in folder's order. Two readers take the first file listed that holds a name: a
# feature section (LoadFeature 0x4224B0, over 0x4BCB50's recursive walk), and a texture's sequence (the bank filled in listing
# order from textures\\*.GAF at 0x42A4AD and 0x42A504, searched from index 0 at 0x42A1A8). settle_places keeps the drop-in's
# winner without shipping a byte of the original game: a copy that comes first in the package is renamed where it stands, since
# nothing asks for the new name, and a winner that is an original file behind another original goes in the recipe.
RENAMED = "~"


def tdf_spans(data: bytes) -> list:
    """[(lower name, start of "[", position after the name's "]", end of the section)] of a TDF's top level, by the grammar
    of tavfs.tdf_sections, the engine's: the text ends at its first NUL, comments are blanks, a stray top-level "}" ends it."""
    end = data.find(b"\0")
    t = bytearray(data if end < 0 else data[:end])
    tavfs()._strip_comments(t)
    s, n, i, out = bytes(t), len(t), 0, []
    while i < n:
        c = s[i]
        if c == 0x7D:
            break
        if c != 0x5B:
            i += 1
            continue
        close = s.find(b"]", i)
        brace = s.find(b"{", close) if close >= 0 else -1
        if brace < 0:
            break
        depth, j = 0, brace
        while j < n:
            depth += (s[j] == 0x7B) - (s[j] == 0x7D)
            if not depth:
                break
            j += 1
        out.append((s[i + 1:close].strip().decode("latin-1").lower(), i, close, min(j + 1, n)))
        i = j + 1
    return out


def tdf_sections(data: bytes) -> list:
    """[(lower name, the section's own bytes)] in file order."""
    return [(name, data[a:z]) for name, a, _, z in tdf_spans(data)]


def first_section(walk) -> dict:
    """{section: (path, its bytes)} by the first TDF that holds it, over (path, bytes) in enumeration order."""
    out = {}
    for path, data in walk:
        for name, raw in tdf_sections(data):
            out.setdefault(name, (path, raw))
    return out


def section_value(raw: bytes):
    """A section as the engine reads it, for comparing two copies."""
    try:
        sections = tavfs().tdf_sections(raw)
    except tavfs().TdfError:
        return raw
    return [section_value_of(x) for x in sections]


def section_value_of(sec):
    return (sec["keys"], [section_value_of(x) for x in sec["sub"]])


def st_recipe(lines) -> list:
    """originals.tsv's lines as the recipe's rows."""
    rows = []
    for line in lines:
        path, md5, _size, arc, member, _sha = line.split("\t")
        rows.append((path, md5, arc, member))
    return rows


def same_section(a: bytes, b: bytes) -> bool:
    return section_value(a) == section_value(b)


def gaf_entries(data: bytes) -> list:
    """[(lower sequence name, offset of its 32-byte name)] of a GAF, file order. A GAF that does not fit reads as none."""
    if len(data) < 12 or struct.unpack_from("<I", data, 0)[0] != 0x00010100:
        return []
    count = struct.unpack_from("<I", data, 4)[0]
    if 12 + count * 4 > len(data):
        return []
    out = []
    for i in range(count):
        at = struct.unpack_from("<I", data, 12 + i * 4)[0] + 8
        if at + 32 > len(data):
            return []
        raw = data[at:at + 32]
        out.append((raw.split(b"\0")[0].decode("latin-1").lower(), at))
    return out


_gaf_names = {}


def gaf_names(key, data_of) -> list:
    """A file's sequence names; retail's are read once across the mods of a run."""
    if key not in _gaf_names:
        _gaf_names[key] = [n for n, _ in gaf_entries(data_of())]
    return _gaf_names[key]


def texture_bank(walk, img) -> list:
    """The GAFs a level's texture bank searches, over (path, bytes) in listing order: the folder's own files, less the logos
    file, which is a bank of its own; with none in the listing, the count's -1 leaves out the last file (0x42A440)."""
    top = image_string(img, 0x503720).lower() + "/"
    logos = top + image_string(img, 0x503704).lower()
    files = [(k, d) for k, d in walk if "/" not in k[len(top):]]
    kept = [(k, d) for k, d in files if k != logos]
    return kept if len(kept) < len(files) else kept[:-1]


def rename_tdf(data: bytes, names: set) -> bytes:
    out, at = [], 0
    for name, _, close, _ in tdf_spans(data):
        if name in names:
            out += [data[at:close], RENAMED.encode()]
            at = close
    return b"".join(out) + data[at:]


def rename_gaf(data: bytes, names: set) -> bytes:
    d = bytearray(data)
    for name, at in gaf_entries(data):
        if name in names:
            raw = d[at:at + 32].split(b"\0")[0] + RENAMED.encode()
            if len(raw) >= 32:
                die(f"sequence {name} has no room for its new name")
            d[at:at + len(raw) + 1] = raw + b"\0"
    return bytes(d)


def undo_rename(path: str, data: bytes) -> bytes:
    """A renamed file as the drop-in folder holds it, for verify."""
    if path.endswith(".tdf"):
        return re.sub(rb"\[([^\]\r\n]*)" + RENAMED.encode() + rb"\]", rb"[\1]", data)
    d = bytearray(data)
    for name, at in gaf_entries(data):
        if name.endswith(RENAMED):
            raw = d[at:at + 32].split(b"\0")[0][:-1]
            d[at:at + len(raw) + 2] = raw + b"\0\0"
    return bytes(d)


def tree_set(tree: dict, low: str, data: bytes):
    parts = low.split("/")
    d = tree
    for part in parts[:-1]:
        d = d[next(n for n in d if n.lower() == part)]
    d[next(n for n in d if n.lower() == parts[-1])] = data


def tree_get(tree: dict, low: str) -> bytes:
    parts = low.split("/")
    d = tree
    for part in parts:
        d = d[next(n for n in d if n.lower() == part)]
    return d


def archive_walk(path: Path, prefix: str, suffix: str):
    """(lower path, bytes) of what an archive holds under `prefix`, in its directory's order."""
    arc = hpipack().Archive(path)
    for k in arc.files:
        if k.startswith(prefix) and k.endswith(suffix):
            yield k, arc.read(k)


def tree_walk(tree: dict, at: str = ""):
    """(lower path, bytes) of a tree as hpipack.build will lay it out: each folder sorted without case, depth first."""
    for name in sorted(tree, key=str.lower):
        low = at + name.lower()
        if isinstance(tree[name], dict):
            yield from tree_walk(tree[name], low + "/")
        else:
            yield low, tree[name]


def enumerated(sources, prefix: str, suffix: str):
    """The engine's enumeration over sources in mount order: each path once, from the first that holds it."""
    seen = set()
    for source in sources:
        for k, data in source(prefix, suffix):
            if k not in seen:
                seen.add(k)
                yield k, data


def dropin_sources(plan) -> list:
    def loose(prefix, suffix):
        return ((k, p.read_bytes()) for k, (_, p) in sorted(plan.loose.items()) if k.startswith(prefix) and k.endswith(suffix))
    return [loose] + [lambda pre, suf, p=p: archive_walk(p, pre, suf) for _, p, _ in plan.mount if tavfs()._has_trailer(p)]


def managed_sources(plan, package: dict, originals: dict) -> list:
    def tree(t):
        return lambda pre, suf: ((k, d) for k, d in tree_walk(t) if k.startswith(pre) and k.endswith(suf))
    retail = [plan.retail[name] for name in plan.managed_retail]
    return [tree(package), tree(originals)] + [lambda pre, suf, p=p: archive_walk(p, pre, suf) for p in retail if tavfs()._has_trailer(p)]


def originals_tree(recipe, plan) -> dict:
    """The recipe's files that an enumeration reads by place, as the install assembles them from the player's archives."""
    tree, opened = {}, {}
    tops = place_read_folders(plan.img)
    for path, _md5, arc, member in recipe:
        a = opened.get(arc)
        if a is None:
            a = opened[arc] = hpipack().Archive(plan.retail[arc])
        if path.lower().startswith(tops):
            insert_cased(tree, path, a.read(member.lower()))
    return tree


def settle_places(plan, tree: dict, st: dict) -> tuple:
    """Rename and place until the managed mount's first holder of every feature section and texture sequence is the drop-in's.
    Returns ({lower path: names renamed}, [paths placed in the recipe]); `tree` and st["recipe"] are updated."""
    features, _, _, textures = place_read_folders(plan.img)
    winners = first_section(enumerated(dropin_sources(plan), features, ".tdf"))
    sequences = {}
    for k, d in texture_bank(enumerated(dropin_sources(plan), textures, ".gaf"), plan.img):
        for name in gaf_names(("dropin", plan.src["id"], k), lambda d=d: d):
            sequences.setdefault(name, k)
    renamed, placed = {}, []
    retail_row = {}
    for name in plan.managed_retail:
        for k, (md5, stored) in st["retail"][name.lower()].items():
            retail_row.setdefault(k, (stored, md5, plan.retail_name(name.lower()), stored))
    for _ in range(16):
        originals = originals_tree(st["recipe"], plan)
        package = {k for k, _d in tree_walk(tree)}
        in_recipe = {r[0].lower() for r in st["recipe"]}
        rename, place = {}, set()

        def before_winner(path, name, winner):
            if path in package:
                rename.setdefault(path, set()).add(name)
            elif winner in package or winner in in_recipe or winner not in retail_row:
                die(f"{plan.src['id']}: {name}: {path} comes before {winner}, and neither can move")
            else:
                place.add(winner)
        seen = set()
        for path, data in enumerated(managed_sources(plan, tree, originals), features, ".tdf"):
            for name, raw in tdf_sections(data):
                if name in seen or name.endswith(RENAMED):
                    continue
                if name not in winners:
                    die(f"{plan.src['id']}: the managed mount alone defines feature {name}, in {path}")
                if same_section(raw, winners[name][1]):
                    seen.add(name)
                else:
                    before_winner(path, name, winners[name][0])
        missing = set(winners) - seen - {n for names in rename.values() for n in names}
        if missing and not rename and not place:
            die(f"{plan.src['id']}: feature sections the managed mount lacks: {sorted(missing)[:5]}")
        seen = set()
        for path, data in texture_bank(enumerated(managed_sources(plan, tree, originals), textures, ".gaf"), plan.img):
            key = (path, plan.src["id"]) if path in package else ("retail", path)
            for name in gaf_names(key, lambda d=data: d):
                if name in seen or name.endswith(RENAMED) or name not in sequences:
                    continue
                if sequences[name] == path:
                    seen.add(name)
                else:
                    before_winner(path, name, sequences[name])
        if not rename and not place:
            return renamed, placed
        for path, names in rename.items():
            data = tree_get(tree, path)
            tree_set(tree, path, rename_gaf(data, names) if path.endswith(".gaf") else rename_tdf(data, names))
            _gaf_names.pop((path, plan.src["id"]), None)
            renamed.setdefault(path, set()).update(names)
        for path in place:
            st["recipe"].append(retail_row[path])
            placed.append(path)
    die(f"{plan.src['id']}: the places did not settle")


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

# What tagpu_modload.c detours under a managed mod, each at a stock byte a table must not change: the mounter's prologue and
# the two rel32s of the loose step, in the path open and the enumeration.
MOUNT_SITES = ((0x4BE0B0, 6), (0x4BB32D, 5), (0x4BC5D3, 5))


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

    sites = {va + i for va, n in MOUNT_SITES for i in range(n)}
    # each table reproduces its mod's exe from stock; a loader's reproduces what its list says, by construction, and
    # `loaderdump` measures it against a process
    for s in mod_sources(a.ids or None):
        tab, exe = mod_table(s, stock)
        m = masked_set(stock, exe)
        img = apply(stock, tab)
        if s["exe"] == "file":
            report(image_hash(img, m) == image_hash(image(exe), m), f"{s['id']}: the table on stock gives the mod's exe, imports aside")
        hit = sorted(set(tab) & sites)
        report(not hit, f"{s['id']}: the table leaves the sites Impure's mount and loose step use" + (f"; first 0x{hit[0]:X}" if hit else ""))
        report(all(va not in mask for va in tab), f"{s['id']}: the table writes no import structure ({len(tab)} B)")
    # the committed pack, read as the DLL reads it: each XOR gives its mod from stock and stock from its known exe, under
    # the pack's own mask, and tagpu_modpak.c gives the same answers on every known exe
    pack_bytes = subprocess.run(["zstd", "-dqc", str(MODS / "patches.bin")], capture_output=True, check=True).stdout
    pack = read_patches(pack_bytes)
    pmask = {va + i for va, n in pack["mask"] for i in range(n)}
    report(pack["stock_key"] == file_key(stock.data, True), "the pack names stock 3.1's key")
    for mid, h, i, _ in pack["mods"]:
        img = image(stock)
        for va, b in pack["xors"][i]:
            image_xor(img, va, b)
        report(image_hash(img, pmask) == h, f"{mid}: its XOR on stock gives the image hash the pack names")
        touched = sum(va + j in pmask for va, b in pack["xors"][i] for j in range(len(b)))
        report(not touched, f"{mid}: its XOR writes no known exe's import structure ({touched} B)")
    exes = {file_key(Path(p).read_bytes(), True): Path(p) for st in tacompat().SETUPS for dest, p in tacompat().overlay(st) if dest.lower() == "totala.exe"}
    stock_hash = image_hash(image(stock), pmask)
    for kind, key, i in pack["known"]:
        if kind:
            # a catalog mod's loader names that mod's XOR, for the fingerprint alone; any other names none
            report(i == -1 or any(i == x for _, _, x, _ in pack["mods"]), f"loader {key.hex()[:12]}: names no table, or a catalog mod's")
            continue
        src = Pe(exes[key].read_bytes())
        img = image(src)
        for va, b in pack["xors"][i]:
            image_xor(img, va, b)
        report(image_hash(img, pmask) == stock_hash, f"exe {key.hex()[:12]}: its XOR gives stock 3.1")
    with tempfile.TemporaryDirectory() as d:
        src_dir = ROOT / "tagpu/ddraw/src"
        subprocess.run(["gcc", "-std=c99", "-O1", "-I", str(src_dir), "-o", d + "/check", str(HERE / "modpakcheck.c"), str(src_dir / "tagpu_modpak.c")], check=True)
        (Path(d) / "pack").write_bytes(pack_bytes)
        want = {mid: h.hex() for mid, h, _, _ in pack["mods"]}
        for path in [main_checkout() / "pristine/TotalA.exe.pristine", *[exes[k] for k, _, _ in ((key, 0, 0) for kind, key, _ in pack["known"] if not kind)]]:
            out = subprocess.run([d + "/check", d + "/pack", str(path)], capture_output=True, text=True).stdout.splitlines()
            report(out and out[0] == file_key(path.read_bytes(), True).hex() and {l.split()[0]: l.split()[1] for l in out[1:]} == want,
                   f"tagpu_modpak.c on {path.parent.name}/{path.name}: its key and every mod's hash")
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
            listed = (OUT / s["id"] / "listing.txt").read_text().splitlines()
            renamed = [ln.split("\t")[1] for ln in listed if ln.startswith("renamed\t")]
            arc = hpipack().Archive(ha) if renamed else None
            for k in renamed:
                got[k] = (hashlib.md5(undo_rename(k, arc.read(k))).hexdigest(), got[k][1])
            recipe = (Path(d) / f"{s['id']}.originals.tsv").read_text(encoding="latin-1").split("\n")[:-1]
            carried = [hashlib.md5((Path(d) / n).read_bytes()).hexdigest() for n in names]
        hit = [n for n, h in zip(names, carried) if h in hashes]
        report(not hit, f"{s['id']}: no file of the package is an original file ({len(names)} files)" + (f"; first {hit[0]}" if hit else ""))
        hits = [k for k, (md5, _) in got.items() if md5 in hashes]
        report(not hits, f"{s['id']}: no member of its archive is an original file ({len(got)} members)" + (f"; first {hits[0]}" if hits else ""))
        # the recipe, as the install runs it: every member named is the player's own, with the bytes it says
        assembled, bad_recipe = {}, []
        for line in recipe:
            path, md5, size, arc, member, _sha = line.split("\t")
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
        # what a file's place decides, by the engine's reader: the feature sections, the texture sequences
        with tempfile.TemporaryDirectory(dir=OUT) as d:
            subprocess.run(["zstd", "-dqf", str(tar), "-o", d + "/p.tar"], check=True)
            with tarfile.open(d + "/p.tar") as t:
                t.extract(f"{s['id']}.hpi", d, filter="data")
            package = {}
            for top in place_read_folders(plan.img):
                for k, data in archive_walk(Path(d) / f"{s['id']}.hpi", top, ""):
                    insert_cased(package, k, data)
        originals = originals_tree(st_recipe(recipe), plan)
        features, weapons, download, textures = place_read_folders(plan.img)
        def engine_first(sources):
            out = {}
            for k, data in enumerated(sources, features, ".tdf"):
                try:
                    secs = tavfs().tdf_sections(data)
                except tavfs().TdfError:
                    continue
                for sec in secs:
                    out.setdefault(sec["_name"].lower(), sec)
            return {n: section_value_of(v) for n, v in out.items() if not n.endswith(RENAMED)}
        one, two = engine_first(dropin_sources(plan)), engine_first(managed_sources(plan, package, originals))
        moved = [k for k in one if one[k] != two.get(k)]
        report(not moved and set(one) == set(two), f"{s['id']}: {len(one)} feature sections read as in the drop-in folder"
               + (f"; differ {len(moved)}, first {moved[:4]}" if moved else ""))
        def first_gaf(sources):
            out = {}
            for k, data in texture_bank(enumerated(sources, textures, ".gaf"), plan.img):
                for name, _ in gaf_entries(data):
                    if not name.endswith(RENAMED):
                        out.setdefault(name, k)
            return out
        one, two = first_gaf(dropin_sources(plan)), first_gaf(managed_sources(plan, package, originals))
        moved = [k for k in one if one[k] != two.get(k)]
        report(not moved and set(one) == set(two), f"{s['id']}: {len(one)} texture sequences come from the drop-in folder's file"
               + (f"; differ {len(moved)}, first {moved[:4]}" if moved else ""))
        parse = lambda raw: tavfs().tascene.parse_tdf(raw.decode("latin-1"))
        # weapons by name, and a build menu's cell by what it names: what would change if an enumeration's order decided it
        def resolved(source, top, key):
            first, last = {}, {}
            for k, data in enumerated(source, top, ".tdf"):
                for name, raw in tdf_sections(data):
                    body = parse(raw).get(name) or next(iter(parse(raw).values()), {})
                    cell = key(name, body)
                    if cell:
                        first.setdefault(cell, body)
                        last[cell] = body
            return first, last
        def menu_cell(_name, body):
            b = {x.lower(): str(y).lower() for x, y in body.items()} if isinstance(body, dict) else {}
            return (b.get("unitmenu"), b.get("menu"), b.get("button")) if b.get("unitmenu") else None
        def weapon_slot(_name, body):
            b = {x.lower(): str(y) for x, y in body.items()} if isinstance(body, dict) else {}
            return b.get("id")
        for top, key in ((weapons, weapon_slot), (download, menu_cell)):
            one_r = resolved(dropin_sources(plan), top, key)
            two_r = resolved(managed_sources(plan, package, originals), top, key)
            flipped = [c for w in (0, 1) for c in one_r[w] if one_r[w][c] != two_r[w].get(c)]
            report(not flipped, f"{s['id']}: {top} resolves as in the drop-in folder, {len(one_r[0])} entries, whichever file wins"
                   + (f"; differ {len(flipped)}, first {flipped[:3]}" if flipped else ""))
    catalog = {e["id"]: e for e in json.loads((MODS / "catalog.json").read_text())["mods"]}
    for s in mod_sources():
        if a.ids and s["id"] not in a.ids or not (OUT / s["id"] / "installed").is_dir():
            continue
        plan = Plan(s, stock)
        one = print_table([p for _, p, _ in plan.mount if tavfs()._has_trailer(p)], plan.loose, [])
        two = print_table(base_sources(s["id"]), {}, catalog[s["id"]].get("same_as", []))
        wrong = sorted(k for k in set(one) | set(two) if one.get(k) != two.get(k))
        report(not wrong, f"{s['id']}: the fingerprint counts {len(one)} files, and managed and drop-in agree"
               + (f"; differ {len(wrong)}, first {wrong[:3]}" if wrong else ""))
    print("verify: " + ("all passed" if not bad else f"{bad} FAILED"))
    return 1 if bad else 0


def installer_takes(path: str) -> bool:
    """modtar_path_safe in tagpu_modtar.c: what a package that the installer can take may name."""
    if not path or any(ord(c) < 0x20 or ord(c) >= 0x7F or c in "\\:" for c in path):
        return False
    for part in path.split("/"):
        if not part or part in (".", "..") or part[-1] in ". ":
            return False
        stem = part.split(".")[0].rstrip(" ").upper()
        if stem in ("CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$") or re.fullmatch(r"(COM|LPT)[0-9]", stem):
            return False
    return True


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
        renamed, placed = settle_places(plan, tree, st)
        d = OUT / s["id"]
        d.mkdir(parents=True, exist_ok=True)
        pkg = {f"{s['id']}.hpi": hpipack().build(tree, 0, 2)}
        sizes = member_sizes()
        opened = {}
        def member_sha(arc, member):
            a = opened.get(arc) or opened.setdefault(arc, hpipack().Archive(plan.retail[plan.retail_name(arc.lower())]))
            return hashlib.sha256(a.read(member.lower())).hexdigest()
        unsafe = [p for p in [r[0] for r in st["recipe"]] + [r[2] for r in st["recipe"]] + list(plan.kept) if not installer_takes(p)]
        if unsafe:
            die(f"{s['id']}: paths tagpu_modtar's modtar_path_safe refuses: {unsafe[:5]}")
        pkg[f"{s['id']}.originals.tsv"] = "".join(f"{path}\t{md5}\t{sizes[md5]}\t{arc}\t{member}\t{member_sha(arc, member)}\n"
                                                  for path, md5, arc, member in st["recipe"]).encode("latin-1")
        for name, p in plan.kept.items():
            pkg[name] = p.read_bytes()
        tar = d / f"{s['id']}.tar.zst"
        write_tar(pkg, tar)
        lines = [f"package {s['id']}: {len(wanted)} members, {cost} bytes; {st['stripped']} paths answered by retail; hide: {plan.hide}",
                 f"first mount pattern: {plan.first}"]
        lines += [f"dropped {n}: {why}" for n, why in plan.plain]
        lines += [f"original with no archive to copy it from: {k}" for k in st["loose_original"]]
        lines += [f"renamed\t{k}\t{' '.join(sorted(v))}" for k, v in sorted(renamed.items())]
        lines += [f"placed\t{k}" for k in sorted(placed)]
        (d / "listing.txt").write_text("\n".join(lines) + "\n")
        print(f"{s['id']:10} keeps the drop-in's first holder: {sum(map(len, renamed.values()))} names renamed in {len(renamed)} files, "
              f"{len(placed)} originals placed in the recipe")
        print(f"{s['id']:10} {len(wanted):6} members {cost / 1e6:8.1f} MB -> {tar.stat().st_size / 1e6:7.1f} MB in {time.time() - t0:5.1f} s; "
              f"{st['stripped']} from retail, {len(st['recipe'])} by recipe, {len(st['loose_original'])} unsourced, {len(plan.plain)} dropped; hide {plan.hide}")


def cmd_install(a):
    """A mod installed by hand: its package into <gamedir>/mods/<id>/, and its recipe run into <id>.originals.hpi from that
    folder's own archives, as the installer will. With --link the result is built once beside the package, from the retail
    folder, and linked in: the compatibility suite installs each mod into many folders."""
    gamedir = Path(a.gamedir)
    tar = OUT / a.id / f"{a.id}.tar.zst"
    if not tar.is_file():
        die(f"no package at {tar}")

    def build(dest: Path, source: Path):
        shutil.rmtree(dest, ignore_errors=True)
        dest.mkdir(parents=True)
        with tempfile.TemporaryDirectory(dir=OUT) as d:
            subprocess.run(["zstd", "-dqf", str(tar), "-o", d + "/p.tar"], check=True)
            with tarfile.open(d + "/p.tar") as t:
                t.extractall(dest, filter="data")
        archives, tree = {}, {}
        names = {p.name.lower(): p for p in source.iterdir() if p.is_file()}
        for line in (dest / f"{a.id}.originals.tsv").read_text(encoding="latin-1").split("\n")[:-1]:
            path, md5, size, arc, member, _sha = line.split("\t")
            h = archives.get(arc.lower()) or hpipack().Archive(names[arc.lower()])
            archives[arc.lower()] = h
            data = h.read(member)
            if hashlib.md5(data).hexdigest() != md5:
                die(f"{path}: {arc} does not hold the bytes the recipe names")
            insert_cased(tree, path, data)
        (dest / f"{a.id}.originals.hpi").write_bytes(hpipack().build(tree, 0, 2))

    dest = gamedir / "mods" / a.id
    if a.link:
        built = OUT / a.id / "installed"
        # the suite prepares several folders at once: one builds, under the lock, and each folder links a finished result
        with open(OUT / a.id / "install.lock", "w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            if not built.is_dir() or built.stat().st_mtime < tar.stat().st_mtime:
                fresh = OUT / a.id / "installed.new"
                build(fresh, tavfs().find_install(None))        # a build that dies leaves no half result to link
                shutil.rmtree(built, ignore_errors=True)
                fresh.rename(built)
        shutil.rmtree(dest, ignore_errors=True)
        dest.mkdir(parents=True)
        for p in built.iterdir():
            (dest / p.name).symlink_to(p)
    else:
        build(dest, gamedir)
    print(f"{a.id}: installed into {dest}")


def sha_of(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


MUTATORS = MODS / "mutators"            # one <id>.json per mutator, written by `modpack.py mutator`; none until a real one exists


def base_sources(base: str) -> list:
    """The archives a base mod answers from, in the mount's order, as the running game finds them: its own two, then the retail
    archives its catalog entry does not hide. "none" is the retail folder's mount alone."""
    v = tavfs()
    retail = {p.name: p for p in v.find_install(None).iterdir() if p.is_file()}
    hide = set()
    out = []
    if base != "none":
        src = next((s for s in mod_sources() if s["id"] == base), None)
        if not src:
            die(f"base {base} is no mod of the catalog")
        plan = Plan(src, Pe(stock_exe()))
        hide = {h.lower() for h in plan.hide}
        built = OUT / base / "installed"
        if not (built / f"{base}.originals.hpi").is_file():
            die(f"{base}: no installed copy at {built}: modpack.py install {base} <dir> --link")
        out += [built / f"{base}.hpi", built / f"{base}.originals.hpi"]
    for n in mount_order(list(retail), "rev31.gp3"):
        if n.lower() not in hide:
            out.append(retail[n])
    return out


def base_answer(sources: list, path: str, opened: dict):
    """The bytes the first source holding `path` gives, or None."""
    for p in sources:
        a = opened.get(p) or opened.setdefault(p, hpipack().Archive(p))
        try:
            return a.read(path.lower())
        except KeyError:
            continue
    return None


def cmd_mutator(a):
    """A mutator from a folder of game files: its package, one archive, and its entry in mods/mutators/. Every file the base holds at
    that path gets the hash of the base's file, which the game compares at each start. A file of the original game is refused by content."""
    src = Path(a.folder)
    hashes = manifest()
    files = {p.relative_to(src).as_posix(): p.read_bytes() for p in sorted(src.rglob("*")) if p.is_file()}
    if not files and not a.hide:
        die("a mutator with no files and no hides changes nothing")
    if a.base == "none" and a.hide:
        die("hides name a base mod's files, and no mod has none")
    for k, data in files.items():
        if hashlib.md5(data).hexdigest() in hashes:
            die(f"{k} is a file of the original game")
    sources = base_sources(a.base)
    opened, base_hashes = {}, {}
    for k in files:
        got = base_answer(sources, k, opened)
        if got is not None:
            base_hashes[k] = hashlib.sha256(got).hexdigest()
    tree = {}
    for k, data in files.items():
        insert_cased(tree, k, data)
    d = OUT / a.id
    d.mkdir(parents=True, exist_ok=True)
    arc = f"{a.id}.hpi"
    (d / arc).write_bytes(hpipack().build(tree, 0, 2))
    tar = d / f"{a.id}.tar.zst"
    write_tar({arc: (d / arc).read_bytes()}, tar)
    version = a.version
    e = {"id": a.id, "kind": "mutator", "name": a.name, "author": a.author, "version": version, "summary": a.summary,
         "homepage": a.homepage, "permission": a.permission,
         "packages": [{"url": f"{a.id}/{a.id}-{version.replace(' ', '-')}.tar.zst", "sha256": sha_of(tar), "size": tar.stat().st_size,
                       "files": [{"path": arc, "sha256": sha_of(d / arc), "size": (d / arc).stat().st_size}]}],
         "base": a.base, "base_hashes": base_hashes, "files": list(files), "hides": a.hide}
    out = Path(a.out) if a.out else MUTATORS / f"{a.id}.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(e, indent=2) + "\n")
    print(f"{a.id}: {len(files)} files, {len(base_hashes)} replace {a.base}'s, {len(a.hide)} hides; {tar}")


# What the fingerprint leaves out (tagpu_modprint.c, tagpu_modprint_counts); a .gui is decided by print_counted's unit names.
PRINT_COSMETIC = (".gaf", ".pcx", ".bmp", ".wav", ".fnt")


def print_counted(path: str, units: set) -> bool:
    """tagpu_modprint_counts, for a lower-case path with '/' separators."""
    if path.startswith(("maps/", "camps/")):
        return False
    base = path.rsplit("/", 1)[-1]
    stem, dot, ext = base.rpartition(".")
    if not dot:
        return True
    if "." + ext in PRINT_COSMETIC:
        return False
    if ext == "gui":
        cut = stem
        while cut and cut[-1].isdigit():
            cut = cut[:-1]
            if cut and cut in units:
                return True
        return False
    return True


def dropin_answer(plan, path: str, opened: dict):
    """The bytes a mod's drop-in folder answers `path` with: its own loose file, then its mount."""
    loose = plan.loose.get(path)
    if loose:
        return loose[1].read_bytes()
    return base_answer([p for _, p, _ in plan.mount if tavfs()._has_trailer(p)], path, opened)


def same_as_of(plan, package: Path) -> list:
    """Each file of the package that the fingerprint counts and whose bytes are not the drop-in folder's: settle_places's renames."""
    arc, opened, out = hpipack().Archive(package), {}, []
    for path in sorted(arc.files):
        if "." + path.rpartition(".")[2] in PRINT_COSMETIC or path.startswith(("maps/", "camps/")):
            continue
        here = arc.read(path)
        there = dropin_answer(plan, path, opened)
        if there is None:
            die(f"{plan.src['id']}: the package holds {path}, which its drop-in folder does not")
        if here != there:
            out.append({"path": path, "here": hashlib.sha256(here).hexdigest(), "dropin": hashlib.sha256(there).hexdigest()})
    return out


def print_table(sources: list, loose: dict, same_as: list) -> dict:
    """{path: sha256} of what a fingerprint counts over these sources in order, as the game would find each."""
    arcs = []
    for p in sources:
        try:
            arcs.append(tavfs().ta3do.HpiArchive(p))
        except (tavfs().ta3do.HpiError, struct.error, IndexError):
            continue
    first = {}
    for a in arcs:
        for f in a.files:
            if "/" in f:
                first.setdefault(f, a)
    tops = {f.split("/")[0] for f in first}
    held = {k: p for k, (_, p) in loose.items() if k.split("/")[0] in tops}
    names = set(first) | set(held)
    units = {n.rsplit("/", 1)[-1][:-4] for n in names if n.endswith(".fbi")}
    same = {e["path"]: e for e in same_as}
    out = {}
    for n in names:
        if not print_counted(n, units):
            continue
        h = hashlib.sha256(held[n].read_bytes() if n in held else first[n].read(n)).hexdigest()
        out[n] = same[n]["dropin"] if n in same and same[n]["here"] == h else h
    return out


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
                same_as = same_as_of(plan, Path(d) / f"{s['id']}.hpi")
                declared = [{"path": n, "sha256": sha_of(Path(d) / n), "size": (Path(d) / n).stat().st_size} for n in t.getnames()]
        e = {k: s[k] for k in ("id", "kind", "name", "author", "version", "summary", "homepage", "permission")}
        e.update({"packages": [{"url": f"{s['id']}/{s['id']}-{s['version'].replace(' ', '-')}.tar.zst", "sha256": sha_of(tar), "size": tar.stat().st_size,
                                "files": declared}],
                  "exe": s["id"], "hide": plan.hide})
        if same_as:
            e["same_as"] = same_as
        for k in ("memory", "options"):
            if k in s:
                e[k] = s[k]
        entries.append(e)
        mods_bin.append((s["id"], img, xor_of(stock, tab), 1 if s["exe"] == "loader" else 0))
    entries += [json.loads(p.read_text()) for p in sorted(MUTATORS.glob("*.json"))]
    (MODS / "catalog.json").write_text(json.dumps({"version": VERSION, "mods": entries}, indent=2) + "\n")
    known = known_files(stock)
    known_exes = [Pe(p.read_bytes()) for p in {Path(src) for st in tacompat().SETUPS for dest, src in tacompat().overlay(st) if dest.lower() == "totala.exe"}]
    mask = known_mask(stock, known_exes)
    pmask = {va + i for va, n in mask for i in range(n)}
    mods_bin = [(mid, image_hash(img, pmask), x, loader) for mid, img, x, loader in mods_bin]
    # a loader names its mod's XOR, by which a drop-in folder's fingerprint finds the image a managed player of that mod hashes as
    loader_xor = {}
    for mid, _h, x, loader in mods_bin:
        if loader:
            src = next(s for s in mod_sources() if s["id"] == mid)
            loader_xor[file_key(setup_files(src["setup"])["dplayx.dll"][1].read_bytes(), False)] = x
    known = [(kind, name, key, loader_xor.get(key) if kind == 1 else x) for kind, name, key, x in known]
    retail_dir = tavfs().find_install(None)
    originals = sorted(bytes.fromhex(sha_of(p)) for p in retail_dir.iterdir() if p.is_file() and p.suffix.lower() in (".gp3", ".ccx", ".ufo", ".hpi") and tavfs()._has_trailer(p))
    blob = pack_patches(file_key(stock.data, True), mods_bin, known, mask, originals)
    p = subprocess.run(["zstd", "-q", "-19", "--no-check", "-c"], input=blob, capture_output=True, check=True)
    (MODS / "patches.bin").write_bytes(p.stdout)
    print(f"catalog: {len(entries)} mods; patches.bin {len(blob)} B -> {len(p.stdout)} B, {len(read_patches(blob)['xors'])} XORs, {len(known)} known files, {len(originals)} original archives")


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


def check_options(n: str, o) -> list:
    """A mod's play options, by the rules tagpu_modcat.c reads them under."""
    errs = []
    if not isinstance(o, dict):
        return [f"{n}: a mod has no options"]
    for name, default_ok in (("resource_snapping", lambda v: isinstance(v, int) and not isinstance(v, bool)), ("quick_build", lambda v: isinstance(v, bool))):
        p = o.get(name)
        if not isinstance(p, dict) or not isinstance(p.get("allowed"), bool):
            errs.append(f"{n}: option {name} does not say whether it is allowed")
            continue
        if not p["allowed"] and not (isinstance(p.get("reason"), str) and p["reason"]):
            errs.append(f"{n}: option {name} is off with no reason")
        if not default_ok(p.get("default")):
            errs.append(f"{n}: option {name} has no default")
    p = o.get("resource_snapping")
    if isinstance(p, dict):
        d, top = p.get("default"), p.get("max")
        if not isinstance(top, int) or isinstance(top, bool) or not 0 <= top <= RADIUS_MAX:
            errs.append(f"{n}: option resource_snapping's max is outside 0..{RADIUS_MAX}")
        elif isinstance(d, int) and not 0 <= d <= top:
            errs.append(f"{n}: option resource_snapping's default is outside 0..its max")
    return errs


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
            for f in e.get("same_as", []):
                if len(f.get("path", "")) >= 96 or not installer_takes(f.get("path", "")) or not HEX64.match(str(f.get("here", ""))) or not HEX64.match(str(f.get("dropin", ""))):
                    errs.append(f"{n}: a same_as entry nothing could act on: {f}")
            errs += check_options(n, e.get("options"))
            if e.get("memory", "4gb") not in ("4gb", "bit-off"):
                errs.append(f"{n}: memory is 4gb or bit-off, not {e['memory']!r}")
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
            else:
                for path in list(e["files"]) + list(e.get("hides", [])):
                    if not isinstance(path, str) or len(path) >= 96 or "=" in path or not installer_takes(path):
                        errs.append(f"{n}: path {path!r} is unsafe")
                if isinstance(e.get("base_hashes"), dict):
                    for path, h in e["base_hashes"].items():
                        if path not in e["files"]:
                            errs.append(f"{n}: base_hashes names {path!r}, which it does not carry")
                        if not HEX64.match(str(h)):
                            errs.append(f"{n}: base_hashes of {path!r} is malformed")
            if e.get("hides") and e.get("base") == "none":
                errs.append(f"{n}: hides name a base mod's files, and no mod has none")
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
    mu = sub.add_parser("mutator")
    mu.add_argument("folder")
    mu.add_argument("--id", required=True)
    mu.add_argument("--base", required=True)
    mu.add_argument("--name", required=True)
    mu.add_argument("--version", required=True)
    mu.add_argument("--author", required=True)
    mu.add_argument("--summary", required=True)
    mu.add_argument("--homepage", required=True)
    mu.add_argument("--permission", required=True)
    mu.add_argument("--hide", action="append", default=[])
    mu.add_argument("--out")
    sub.add_parser("verify").add_argument("ids", nargs="*")
    i = sub.add_parser("install")
    i.add_argument("id")
    i.add_argument("gamedir")
    i.add_argument("--link", action="store_true")
    a = p.parse_args()
    return {"check": cmd_check, "tables": cmd_tables, "loaderdump": cmd_loaderdump, "packages": cmd_packages, "catalog": cmd_catalog, "mutator": cmd_mutator, "verify": cmd_verify, "install": cmd_install}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main() or 0)
