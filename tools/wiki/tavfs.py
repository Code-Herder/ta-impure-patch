"""tavfs.py -- the game's files as the running engine finds them: loose first, then the first archive in mount order.

    from tavfs import Vfs, find_install
    vfs = Vfs(find_install(None))
    blob, where = vfs.open("maps\\Crossing Two Rivers.tnt")   # None when nothing holds it
    vfs.feature("lushmetal3")                                  # (section dict, the TDF's path, where) or None

The rules are the engine's (research/notes/ingame-wiki-3d-stage-plan.md, *How the engine opens a file*, and
exe-reverse-engineering.md, *Where the engine looks for archives*):

- **The mount list.** InitTAHPIAry 0x41D4C0 mounts four bare relative patterns from the game's directory, in this
  order: the executable's main-archive pattern (`"rev%s.GP3"` at 0x5028CC formatted with `"31"` at 0x5028D8 on
  retail; a mod's executable renames it, `TAE%s.GP3`, `may%s.GP3`, `ProTA.gp3`, ...), `*.CCX`, `*.UFO`, `*.HPI`.
  The pattern is read out of the executable at those addresses, so a mod's own archive mounts first here as it does
  in the game. Within one pattern the files mount in name order without case [INFERRED: the order FindFirstFile
  lists on NTFS]. The mounter 0x4BE0B0 skips a path already mounted, and OpenHAPIFile 0x4BDD70 refuses, silently, a
  file whose last 36 bytes are not `Copyright <year> Cavedog Entertainment` for any four year bytes: it copies
  "0000" (0x50390C) over the file's year before comparing with the template at 0x4FDBF0, which reads
  "Copyright 0000 ..." (0x4BDE72..0x4BDED4, DISASSEMBLED 2026-10-26; the retail archives carry 1997 to 2000). The
  drive roots' `%c:\\*.hpi` are not mounted here: they are the CD's. A level's list holds every archive by then, so
  `*.HPI`'s ten mounts a call (0x41D5C0) do not bound it here.
- **A path** opens loose first, relative to the game's directory as the engine's `fopen` (0x4E4990) takes it
  relative to the process's current directory, then from the first archive in mount order that holds it as a file
  (0x4BB2E0 walks the list from index 0 and stops at the first file entry, 0x4BB378). Names compare without case
  [INFERRED for 0x4F8A70], and the path splits at `\\` only (0x4BB4EA); `/` is accepted here as a convenience.
- **An enumeration** lists loose files first, then each archive in mount order (0x4BCA30, 0x4BCB50, cited). Within
  one source the order is the directory's: the loose files sorted without case, as NTFS lists them, and an
  archive's in its directory order [INFERRED for both; QUESTIONS batch A1].
- **A feature section** comes from the first TDF of `0x4BCB50("features", "*.tdf", -1, 1)` that holds it
  (LoadFeature 0x4224B0, cited).

The readers are the repository's: tools/ta3do's HPI reader and tools/tascene's TDF and TNT readers.
"""

import fnmatch
import importlib.util
import os
import struct
from importlib.machinery import SourceFileLoader
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent


def _load(name, path):
    loader = SourceFileLoader(name, str(path))
    spec = importlib.util.spec_from_loader(name, loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


tascene = _load("tascene", TOOLS / "tascene")
ta3do = tascene.ta3do

VA_STR_ARCHIVE = 0x5028CC       # "rev%s.GP3" on retail: the first pattern InitTAHPIAry mounts (0x41D4E0)
VA_STR_ARCHIVE_ARG = 0x5028D8   # "31", the argument it is formatted with
TRAILER_HEAD, TRAILER_TAIL = b"Copyright ", b" Cavedog Entertainment"   # a four-byte year between them
TRAILER_LEN = len(TRAILER_HEAD) + 4 + len(TRAILER_TAIL)
EXE_NAME = "TotalA.exe"


class VfsError(Exception):
    pass


def find_install(arg):
    """--gamedir, else TA3DO_GAMEDIR, else Steam's default library, else ta3do's gamedir."""
    for cand in (arg, os.environ.get("TA3DO_GAMEDIR"),
                 Path.home() / ".local/share/Steam/steamapps/common/Total Annihilation"):
        if cand and Path(cand).expanduser().is_dir():
            return Path(cand).expanduser()
    return ta3do.game_dir()


def exe_cstr(blob: bytes, va: int, limit: int = 64) -> str:
    """The NUL-terminated string at virtual address `va` of a PE image, through its section table."""
    if blob[:2] != b"MZ" or len(blob) < 0x40:
        raise VfsError("not a PE executable")
    pe = struct.unpack_from("<I", blob, 0x3C)[0]
    if pe + 24 > len(blob) or blob[pe:pe + 4] != b"PE\0\0":
        raise VfsError("no PE header")
    nsec, opt_size = struct.unpack_from("<H12xH", blob, pe + 6)
    opt = pe + 24
    if opt + opt_size > len(blob):
        raise VfsError("PE optional header past the file")
    base = struct.unpack_from("<I", blob, opt + 28)[0]
    rva = va - base
    for i in range(nsec):
        sec = opt + opt_size + i * 40
        if sec + 40 > len(blob):
            break
        vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", blob, sec + 8)
        if vaddr <= rva < vaddr + max(vsize, rsize):
            off = raddr + rva - vaddr
            if not raddr <= off < min(len(blob), raddr + rsize):
                break
            raw = blob[off:off + limit]
            end = raw.find(b"\0")
            if end < 0:
                raise VfsError(f"no string end within {limit} bytes at 0x{va:X}")
            return raw[:end].decode("latin-1")
    raise VfsError(f"0x{va:X} is in no section of the executable")


def mount_patterns(exe: bytes):
    """InitTAHPIAry's four patterns, the first as this executable spells it."""
    first = exe_cstr(exe, VA_STR_ARCHIVE)
    if "%s" in first:
        first = first.replace("%s", exe_cstr(exe, VA_STR_ARCHIVE_ARG), 1)
    return [first, "*.CCX", "*.UFO", "*.HPI"]


def _has_trailer(path: Path) -> bool:
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            if f.tell() < TRAILER_LEN:
                return False
            f.seek(-TRAILER_LEN, os.SEEK_END)
            tail = f.read()
            return tail.startswith(TRAILER_HEAD) and tail.endswith(TRAILER_TAIL)
    except OSError:
        return False


def _key(path: str) -> str:
    return path.replace("\\", "/").lower().strip("/")


class Vfs:
    """The archives of one install in the engine's mount order, and its loose files."""

    def __init__(self, root: Path, exe: Path = None):
        self.root = Path(root)
        if not self.root.is_dir():
            raise VfsError(f"no game directory at {self.root}")
        exe = Path(exe) if exe else self._loose_path(EXE_NAME)
        if not exe or not exe.is_file():
            raise VfsError(f"no {EXE_NAME} in {self.root} to read the archive patterns from (--exe)")
        self.patterns = mount_patterns(exe.read_bytes())
        self.archives = []                      # (file name, ta3do.HpiArchive), in mount order
        mounted = set()
        names = sorted((p for p in self.root.iterdir() if p.is_file()), key=lambda p: p.name.lower())
        for pattern in self.patterns:
            for path in names:
                if not fnmatch.fnmatch(path.name.lower(), pattern.lower()):
                    continue
                full = str(path.resolve()).lower()
                if full in mounted or not _has_trailer(path):
                    continue
                try:
                    arc = ta3do.HpiArchive(path)
                except (ta3do.HpiError, struct.error, IndexError):
                    continue
                mounted.add(full)
                self.archives.append((path.name, arc))
        self._features = None

    # -- loose files ----------------------------------------------------------

    def _loose_path(self, path: str):
        """The loose file at `path` under the game's directory, each component matched without case, or None."""
        cur = self.root
        parts = [p for p in _key(path).split("/") if p]
        for i, part in enumerate(parts):
            try:
                hit = next((c for c in cur.iterdir() if c.name.lower() == part), None)
            except OSError:
                return None
            if hit is None:
                return None
            if i < len(parts) - 1 and not hit.is_dir():
                return None
            cur = hit
        return cur if parts and cur.is_file() else None

    def _loose_walk(self, top: str, recursive: bool):
        base = self._loose_path_dir(top)
        if base is None:
            return
        entries = sorted(base.iterdir(), key=lambda p: p.name.lower())
        for e in entries:
            rel = f"{_key(top)}/{e.name.lower()}"
            if e.is_file():
                yield rel, e
        if recursive:
            for e in entries:
                if e.is_dir():
                    yield from self._loose_walk(f"{_key(top)}/{e.name.lower()}", True)

    def _loose_path_dir(self, path: str):
        cur = self.root
        for part in [p for p in _key(path).split("/") if p]:
            hit = next((c for c in cur.iterdir() if c.name.lower() == part and c.is_dir()), None)
            if hit is None:
                return None
            cur = hit
        return cur

    # -- the engine's questions -------------------------------------------------

    def where(self, path: str):
        """Where `path` opens from: "loose" or the archive's file name; None when nothing holds it."""
        if self._loose_path(path):
            return "loose"
        key = _key(path)
        for name, arc in self.archives:
            if key in arc.files:
                return name
        return None

    def open(self, path: str):
        """(bytes, where) for `path` by the engine's precedence, or None."""
        loose = self._loose_path(path)
        if loose:
            return loose.read_bytes(), "loose"
        key = _key(path)
        for name, arc in self.archives:
            if key in arc.files:
                return arc.read(key), name
        return None

    def enumerate(self, top: str, pattern: str, recursive: bool):
        """[(path, where)] of every file under `top` matching `pattern`, loose first, then the archives in mount
        order. A path two sources hold is listed once per source, as each source lists it."""
        pat = pattern.lower()
        out = [(rel, "loose") for rel, _p in self._loose_walk(top, recursive)
               if fnmatch.fnmatch(rel.rsplit("/", 1)[-1], pat)]
        prefix = _key(top) + "/"
        for name, arc in self.archives:
            for path in arc.files:
                if not path.startswith(prefix):
                    continue
                rest = path[len(prefix):]
                if "/" in rest and not recursive:
                    continue
                if fnmatch.fnmatch(rest.rsplit("/", 1)[-1], pat):
                    out.append((path, name))
        return out

    def read_from(self, path: str, where: str) -> bytes:
        """The copy of `path` that `where` (as enumerate names it) holds."""
        if where == "loose":
            return self._loose_path(path).read_bytes()
        for name, arc in self.archives:
            if name == where:
                return arc.read(_key(path))
        raise VfsError(f"no archive {where}")

    def feature(self, name: str):
        """(section, tdf path, where) of the feature section `name` from the first TDF that holds it, or None."""
        if self._features is None:
            self._features = {}
            for path, where in self.enumerate("features", "*.tdf", True):
                try:
                    text = self.read_from(path, where).decode("latin-1")
                except (ta3do.HpiError, VfsError, struct.error, OSError):
                    continue
                for sec, body in tascene.parse_tdf(text).items():
                    if isinstance(body, dict) and body:
                        self._features.setdefault(sec, (body, path, where))
        return self._features.get(name.lower())
