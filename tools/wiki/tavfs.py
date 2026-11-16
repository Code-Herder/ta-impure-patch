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
- **An enumeration** lists loose files first, then each archive in mount order (0x4BCA30, 0x4BCB50), and **each path
  once, from the first source that holds it**: the copy 0x4BB2E0 opens. The engine does not de-duplicate in the
  enumerators; InitTAHPIAry ends with 0x4BE320, which sets bit 1 of every directory entry a loose file or an earlier
  archive already holds as a file (`or cl,2` at 0x4BE658), and the find-next 0x4BC640 skips an entry with that bit
  (`test byte [edi+8],2` at 0x4BC735). Directories are never shadowed. Within one source the order is the
  directory's: an archive's forwards (0x4BC6FF..0x4BC754), the loose files as the host's `_findfirst` lists them,
  here sorted without case as NTFS lists them [INFERRED for the loose order]. DISASSEMBLED 2026-10-26 (the 3D
  stage's scout answers, A1).
- **A feature section** comes from the first TDF of `0x4BCB50("features", "*.tdf", -1, 1)` that holds it
  (LoadFeature 0x4224B0, cited).

- **A file exists** for the engine when its size is above zero: the open test 0x4BBC40 returns the size (the archive
  entry's, or filelength for a loose file) and every probe tests that value, so a 0-byte file is a missing one.
  ProTA ships 32 0-byte .GUI pages as page stops (MEASURED, the Q1 scout answer).

The readers are the repository's: tools/ta3do's HPI reader and tools/tascene's TDF and TNT readers, and `tdf_sections`
below for the files the engine reads by position.

**The running image and the profile.** The names the engine's readers use -- the mount patterns, the units, weapons,
gamedata, download, gui and features folders, their patterns and extensions -- and the menu-time pre-scan's
constants (the version threshold, the archive flag, the Copyright reference) are read from the image the process runs,
each where the engine's code reads it (`PROFILE_STRINGS`, `PROFILE_CODE`). That image is the folder's TotalA.exe with
the list of each Patch Loader the exe imports from the folder applied (`running_image`); `profile` gathers the values
and `write_profile` hands them to the offline loader (tools/tasim/tasim_load.h, `tasim-profile/1`). It is exact by
construction:

- ORDERING. The names are final before the engine reads them: Windows runs each static import's DllMain before the
  exe's entry point, and the Patch Loader is a static import (DPLAYX.dll) that writes its list there
  (vendor/Total-Annihilation-Patch-Loader/dllmain.c, patches.c). InitTAHPIAry and the pre-scan run from WinMain, and the
  menus later.
- BOUND. Every value is read where the engine's code reads it, and every one of those 48 instruction sites is checked to
  hold that instruction, so an image whose code points elsewhere is refused rather than read at the wrong place. Each
  string must end within its field, and the C reader bounds every value again.
- COVERAGE. Nothing else writes these bytes. The static list equals the process: modpack.py's `loaderdump` read
  0 list bytes missing from a running Total Mayhem 11.3.0 and ProTA 4.8 (MEASURED 2026-11-05,
  research/notes/deep-patch-loader.md, *What a built loader writes, measured*). TADR's modules make no engine write
  under Impure's takeover (no hook slot leads into one in any compat run, MEASURED 2026-11-13). Impure writes none of
  the sites (research/notes/modules/hook-map.md). The one route where Impure rewrites the image and the mounts at run
  time, a managed mod (impure.cfg `mod=`, tagpu_modload), is refused: this profile does not model it.
- NOT CLAIMED. The registry's `language` value is not read: no install the suite runs has a `<dir>-<language>` folder
  for any folder the loader reads, so it cannot change a read there, and `language=` sets it.
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
STORE_NAME = "impure.cfg"

# The profile's strings: (key, the string's VA, the sites that push it, each `68 <va>`). DISASSEMBLED from retail 3.1;
# every site holds its push in all six images the compat suite runs (retail, the Total Mayhem 11.3.0 and ProTA 4.8
# loader-patched images, the Escalation, TA Zero alpha5 and TA Twilight 2.0b98 exes), MEASURED 2026-11-13.
PROFILE_STRINGS = (
    ("first_pattern",     0x5028CC, (0x41D4EC,)),        # InitTAHPIAry 0x41D4C0's first pattern, sprintf'd at 0x41D4F2
    ("first_arg",         0x5028D8, (0x41D4E0,)),        # its argument ("31" on retail)
    ("pattern_ccx",       0x5028C4, (0x41D540,)),        # InitTAHPIAry's second pattern
    ("pattern_ufo",       0x5028BC, (0x41D57A,)),        # its third
    ("pattern_hpi",       0x5028B4, (0x41D5B4,)),        # its fourth
    ("units",             0x503920, (0x42AA25,           # the pre-scan 0x42A8D0's unit listing,
                                     0x42AB68,           # its per-file open,
                                     0x42ABDC,           # its .OVR override,
                                     0x42D6F1)),         # and the level's by-name re-read, 0x42D6E9..0x42D722
    ("all_pattern",       0x50372C, (0x42AA1C,           # "*": the pre-scan's unit listing
                                     0x42DD03)),         # and the download listing, 0x42DCFE..0x42DD34
    ("unit_ext",          0x503928, (0x42AA17, 0x42AB3E, 0x42D6CF)),   # "FBI", at the three unit sites above
    ("weapons",           0x50392C, (0x42A98B,           # the folder each weapon TDF opens from: the pre-scan
                                     0x42E39B)),         # and the level's weapon load 0x42E310
    ("weapon_pattern",    0x503934, (0x42A8EC,           # the weapon listing 0x4BCA30: the pre-scan
                                     0x42E350)),         # and the level's weapon load
    ("tdf_ext",           0x50341C, (0x42A980, 0x42E391,                # "TDF": the two weapon opens,
                                     0x42D2F3,                          # moveinfo,
                                     0x42D937, 0x429EA3,                # sidedata (level, menu),
                                     0x42DCFE, 0x42DDA3)),              # the download listing and its opens
    ("gamedata",          0x503360, (0x42D301, 0x42D945, 0x429EB1)),   # moveinfo's and sidedata's folder
    ("moveinfo",          0x503F48, (0x42D2F8,)),        # the move classes, 0x42D2F3..0x42D307
    ("sidedata",          0x503410, (0x42D93C, 0x429EA8)),   # the sides, 0x42D937..0x42D94B and 0x429EA3..0x429EB7
    ("download",          0x503730, (0x42DD0E, 0x42DDB0)),   # the download entries' listing and opens
    ("gui",               0x502820, (0x42D82A,           # the GUI pages: the page probe 0x42D824..0x42D83F
                                     0x41AD2C)),         # and the page loader 0x41AD17..0x41AD5F
    ("gui_ext",           0x502828, (0x42D824, 0x41AD17)),   # "GUI", at the same two sites
    ("features",          0x502C7C, (0x422324,)),        # the feature listing 0x4BCB50 at 0x422310..0x422331
    ("feature_pattern",   0x502C88, (0x42231F,)),        # its filter
    ("scripts",           0x503740, (0x42D8DF,)),        # a type's script, built by 0x4290F0 at 0x42D8D2..0x42D8EF
    ("script_ext",        0x503EC8, (0x42D8D5,)),        # "COB", its extension
    ("objects",           0x5036E4, (0x42D753,)),        # a type's model, built at 0x42D74D..0x42D766 from +0x80
    ("object_ext",        0x5036F0, (0x42D74D,)),        # "3DO", its extension
    ("copyright",         0x5038CC, (0x42AABF,)),        # the pre-scan's Copyright reference (strstr at 0x42AB0C)
    ("copyright_year",    0x50390C, (0x42AAB7,)),        # the year template strstr finds in it
    ("copyright_default", 0x5037BC, (0x42B0E6,)),        # the value an FBI without `Copyright` is compared as
    ("language",          0x504AB8, ()),                 # read through the mov at 0x49EA04 (PROFILE_CODE)
)
# The other readers: (site, the bytes it must hold), DISASSEMBLED, MEASURED in the same six images.
PROFILE_CODE = (
    (0x42B107, b"\xBE" + (0x5038CC).to_bytes(4, "little")),   # mov esi, the Copyright reference (0x42B0E2..0x42B162)
    (0x42B102, b"\xA1" + (0x50390C).to_bytes(4, "little")),   # mov eax, [the year template]
    (0x41D8A0, b"\xA1" + (0x50289C).to_bytes(4, "little")),   # the archive flag's getter, which the pre-scan tests
    (0x49E9BD, b"\xC6\x42\x01"),                              # WinMain: mov byte [edx+1], imm8, the version's major
    (0x49E9C6, b"\xC6\x40\x02"),                              # mov byte [eax+2], imm8, its minor
    (0x49EA04, b"\xBF" + (0x504AB8).to_bytes(4, "little")),   # mov edi, the default language copied to 0x51FB50
)
VA_VERSION_MAJOR, VA_VERSION_MINOR = 0x49E9C0, 0x49E9C9      # the two immediates
VA_ARCHIVE_FLAG = 0x50289C
PROFILE_TEXT = 0x80          # the Copyright reference and its default: 0x42B0FD copies 0x80 bytes
PROFILE_NAME = 64            # a folder, pattern, extension or language, its NUL included (TASIM_PROFILE_NAME)
COPYRIGHT_KEYS = ("copyright", "copyright_default")
# The profile file's name keys, in the order write_profile writes them (tools/tasim/tasim_load.h, tasim-profile/1).
PROFILE_NAMES = ("units", "all_pattern", "unit_ext", "weapons", "weapon_pattern", "tdf_ext", "gamedata", "moveinfo",
                 "sidedata", "download", "gui", "gui_ext", "features", "feature_pattern", "scripts", "script_ext",
                 "objects", "object_ext")

# The Patch Loader (vendor/Total-Annihilation-Patch-Loader): its DllMain exits the game unless the 10 bytes at
# 0x410000 are retail's and either 0x4FF618 names DDRAW.dll or the version bytes are 3 and 1 (dllmain.c).
LOADER_GATE_VA, LOADER_GATE = 0x410000, b"\x14\x68\x78\x1B\x50\x00\x8D\x4C\x24\x1B"
LOADER_DDRAW_VA = 0x4FF618
# A Patch Loader's list: `[Settings]` once, beside its own error text. Of every DLL of the compat suite's fixtures only
# the three Patch Loader dplayx.dll (Total Mayhem, ProTA, the public 1.3.0.0) hold both, each `[Settings]` once
# (MEASURED 2026-11-13).
LOADER_MARK, LOADER_TEXT = b"[Settings]", b"Failed to apply game patches"


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
    raw = exe_bytes(blob, va, limit)
    end = raw.find(b"\0")
    if end < 0:
        raise VfsError(f"no string end within {limit} bytes at 0x{va:X}")
    return raw[:end].decode("latin-1")


def _pe(blob: bytes):
    """(image base, optional header offset, its size, [(vsize, vaddr, rsize, raddr)]) of a PE file."""
    if blob[:2] != b"MZ" or len(blob) < 0x40:
        raise VfsError("not a PE executable")
    pe = struct.unpack_from("<I", blob, 0x3C)[0]
    if pe + 24 > len(blob) or blob[pe:pe + 4] != b"PE\0\0":
        raise VfsError("no PE header")
    nsec, opt_size = struct.unpack_from("<H12xH", blob, pe + 6)
    opt = pe + 24
    if opt + opt_size > len(blob) or opt_size < 32:
        raise VfsError("PE optional header past the file")
    secs = []
    for i in range(nsec):
        sec = opt + opt_size + i * 40
        if sec + 40 > len(blob):
            break
        secs.append(struct.unpack_from("<IIII", blob, sec + 8))
    return struct.unpack_from("<I", blob, opt + 28)[0], opt, opt_size, secs


def _rva_span(blob: bytes, rva: int, what: str):
    """(file offset, the end of its section's raw data) of `rva`; VfsError naming `what` when no file byte holds it."""
    for vsize, vaddr, rsize, raddr in _pe(blob)[3]:
        if vaddr <= rva < vaddr + max(vsize, rsize):
            off = raddr + rva - vaddr
            end = min(len(blob), raddr + rsize)
            if raddr <= off < end:
                return off, end
            break
    raise VfsError(f"{what} is in no section of the executable")


def exe_bytes(blob: bytes, va: int, n: int) -> bytes:
    """Up to `n` bytes of a PE image's file from virtual address `va`, through its section table: fewer
    where the section's raw data ends first."""
    off, end = _rva_span(blob, va - _pe(blob)[0], f"0x{va:X}")
    return blob[off:min(off + n, end)]


def exe_imports(blob: bytes) -> list:
    """The DLL names of a PE file's import directory (data directory 1), in its order. Every RVA is mapped through the
    section table and a name must end within its section; a table that does not read so is a VfsError."""
    _base, opt, opt_size, _secs = _pe(blob)
    if opt_size < 96 + 16:
        raise VfsError("the PE optional header holds no import directory")
    rva = struct.unpack_from("<I", blob, opt + 96 + 8)[0]
    if not rva:
        return []
    off, end = _rva_span(blob, rva, f"the import directory (RVA 0x{rva:X})")
    out = []
    while True:
        if off + 20 > end:
            raise VfsError("the import directory runs past its section")
        desc = struct.unpack_from("<5I", blob, off)
        if not any(desc):
            return out
        noff, nend = _rva_span(blob, desc[3], f"an import's name (RVA 0x{desc[3]:X})")
        stop = blob.find(b"\0", noff, nend)
        if stop <= noff:
            raise VfsError(f"the import name at RVA 0x{desc[3]:X} is empty or does not end within its section")
        out.append(blob[noff:stop].decode("latin-1"))
        off += 20


_MODPACK = None


def _modpack():
    """tools/modpack/modpack.py, loaded on first use as tascene is above: the mod manager's own reader of a Patch
    Loader's list, so the two stay one implementation."""
    global _MODPACK
    if _MODPACK is None:
        _MODPACK = _load("modpack", TOOLS / "modpack" / "modpack.py")
    return _MODPACK


def patch_loader_list(dll: bytes):
    """{va: byte} a Patch Loader writes into the image from its built-in list (modpack.loader_table), or None when
    `dll` is not one: it must hold LOADER_MARK exactly once, and LOADER_TEXT."""
    if dll.count(LOADER_MARK) != 1 or LOADER_TEXT not in dll:
        return None
    try:
        return _modpack().loader_table(dll)
    except SystemExit as e:                     # modpack.die
        raise VfsError(str(e.code)) from None


def loose_path(root: Path, path: str):
    """The file at `path` under `root`, each component matched without case as Windows matches it, or None."""
    cur = Path(root)
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


_CONTROL = "".join(map(chr, range(33)))         # what tagpu_settings.c's trim cuts from a line's end


def managed_mod(root: Path) -> str:
    """The `mod` value of the folder's impure.cfg as tagpu_settings.c reads it, "" for none: the first 8191 bytes up to
    a NUL, lines split at LF, `key = value` split at the first `=`, the key and the value each trimmed of blanks before
    and of every byte up to a space after, the key compared without case, a key starting with `#` or `;` skipped, the
    last line winning."""
    cfg = loose_path(root, STORE_NAME)
    if cfg is None:
        return ""
    text = cfg.read_bytes()[:8191].split(b"\0", 1)[0].decode("latin-1")
    mod = ""
    for line in text.split("\n"):
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        k = k.lstrip(" \t").rstrip(_CONTROL)
        if not k or k[0] in "#;":
            continue
        if k.lower() == "mod":
            mod = v.lstrip(" \t").rstrip(_CONTROL)
    return mod


def running_image(root: Path):
    """(the image the game in `root` runs, its description): the folder's TotalA.exe with the list of each Patch Loader
    the exe imports from the folder applied, as its DllMain applies it before the engine runs (the module's header).
    VfsError for a managed mod, for a loader whose own gates would stop the game, and for a list byte outside the
    file's image."""
    mod = managed_mod(root)
    if mod:
        raise VfsError(f"a managed mod ({STORE_NAME} mod={mod}): the DLL rewrites the image and mounts the package at "
                       "run time, which this profile does not model")
    exe = loose_path(root, EXE_NAME)
    if exe is None:
        raise VfsError(f"no {EXE_NAME} in {root}")
    img = bytearray(exe.read_bytes())
    what = [EXE_NAME]
    for name in exe_imports(bytes(img)):
        dll = loose_path(root, name)              # Windows loads an import from the application's folder first
        table = patch_loader_list(dll.read_bytes()) if dll else None
        if table is None:
            continue
        cur = bytes(img)
        if exe_bytes(cur, LOADER_GATE_VA, len(LOADER_GATE)) != LOADER_GATE or (
                exe_cstr(cur, LOADER_DDRAW_VA) != "DDRAW.dll" and
                exe_bytes(cur, VA_VERSION_MAJOR, 1) + exe_bytes(cur, VA_VERSION_MINOR, 1) != b"\x03\x01"):
            raise VfsError(f"{name}'s Patch Loader refuses this exe: the game exits before the engine runs")
        pe = _modpack().Pe(cur)
        for va, b in table.items():
            off = pe.off(va)
            if off is None:
                raise VfsError(f"{name}'s Patch Loader list writes 0x{va:X}, which no byte of {EXE_NAME} holds")
            img[off] = b
        what.append(f"{name}'s Patch Loader list")
    return bytes(img), " + ".join(what)


def first_pattern(fmt: str, arg: str) -> str:
    """InitTAHPIAry's first pattern as its sprintf 0x4E42B0 formats it, for the one format modelled: at most one `%s`,
    which becomes `arg`, and `%%`. Any other `%` is a VfsError."""
    out, i, used = [], 0, False
    while i < len(fmt):
        c = fmt[i]
        if c != "%":
            out.append(c)
            i += 1
            continue
        nxt = fmt[i + 1:i + 2]
        if nxt == "%":
            out.append("%")
        elif nxt == "s" and not used:
            out.append(arg)
            used = True
        else:
            raise VfsError(f"the first mount pattern {fmt!r} holds a format other than one %s")
        i += 2
    return "".join(out)


def mount_patterns(exe: bytes, first: str = None):
    """InitTAHPIAry's four patterns as the image spells them, the first formatted with its argument unless `first`
    names it."""
    if first is None:
        first = first_pattern(exe_cstr(exe, VA_STR_ARCHIVE), exe_cstr(exe, VA_STR_ARCHIVE_ARG))
    return [first] + [exe_cstr(exe, va) for va in (0x5028C4, 0x5028BC, 0x5028B4)]


def profile(root: Path, language: str = None) -> dict:
    """The names and constants the engine reads in `root`, from its running image (the module's header): every
    PROFILE_STRINGS key, `patterns` (the four mount patterns, the first formatted), `root`, `image` (the image's
    description), `language` (`language`, else the image's default), `version_major` and `version_minor` (signed),
    `archive_only` and `mounts` (the archives' full paths in mount order). VfsError when a site does not hold its
    instruction or a value is past its bound."""
    root = Path(root).absolute()
    img, what = running_image(root)
    p = {"root": str(root), "image": what}
    for key, va, sites in PROFILE_STRINGS:
        want = b"\x68" + va.to_bytes(4, "little")
        for site in sites:
            got = exe_bytes(img, site, 5)
            if got != want:
                raise VfsError(f"{key}: 0x{site:X} holds {got.hex(' ')}, not push 0x{va:X}: this image's reader "
                               "is not the one modelled")
        limit = PROFILE_TEXT if key in COPYRIGHT_KEYS else PROFILE_NAME
        try:
            s = exe_cstr(img, va, limit)
        except VfsError:
            raise VfsError(f"{key}: the string at 0x{va:X} does not end within {limit} bytes") from None
        if not s or any(c in s for c in "\t\r\n"):
            raise VfsError(f"{key}: the string at 0x{va:X} is empty or holds a TAB or a line break")
        p[key] = s
    for site, want in PROFILE_CODE:
        got = exe_bytes(img, site, len(want))
        if got != want:
            raise VfsError(f"0x{site:X} holds {got.hex(' ')}, not {want.hex(' ')}: this image's reader is not the "
                           "one modelled")
    p["version_major"] = struct.unpack("<b", exe_bytes(img, VA_VERSION_MAJOR, 1))[0]
    p["version_minor"] = struct.unpack("<b", exe_bytes(img, VA_VERSION_MINOR, 1))[0]
    p["archive_only"] = int(struct.unpack("<I", exe_bytes(img, VA_ARCHIVE_FLAG, 4))[0] != 0)
    p["patterns"] = [first_pattern(p["first_pattern"], p["first_arg"]), p["pattern_ccx"], p["pattern_ufo"],
                     p["pattern_hpi"]]
    cut = p["weapon_pattern"].rfind("\\")
    if cut <= 0 or cut == len(p["weapon_pattern"]) - 1:
        raise VfsError(f"weapon_pattern {p['weapon_pattern']!r} is not a folder and a filter split at its last `\\`")
    if len(p["copyright_year"]) != 4 or not 0 <= p["copyright"].find(p["copyright_year"]) <= 0x7B:
        raise VfsError(f"copyright_year {p['copyright_year']!r} is not four bytes found in the reference at an "
                       "offset of at most 0x7B")
    if language is not None:
        if not language or len(language) >= PROFILE_NAME or any(c in language for c in "\t\r\n"):
            raise VfsError(f"language {language!r} is empty, too long or holds a TAB or a line break")
        p["language"] = language
    p["mounts"] = [str(root / name) for name, _arc in Vfs(root, image=img).archives]
    return p


def write_profile(p: dict, path: Path):
    """The `tasim-profile/1` file of profile `p` (tools/tasim/tasim_load.h): UTF-8, the format line, then one
    `key<TAB>value` a line, `mount` once per archive in mount order."""
    rows = [("root", p["root"]), ("image", p["image"]), ("language", p["language"])]
    rows += [("mount", m) for m in p["mounts"]]
    rows += [(k, p[k]) for k in PROFILE_NAMES]
    rows += [(k, str(p[k])) for k in ("version_major", "version_minor", "archive_only")]
    rows += [(k, p[k]) for k in ("copyright", "copyright_year", "copyright_default")]
    for k, v in rows:
        if any(c in v for c in "\t\r\n"):
            raise VfsError(f"profile {k}: {v!r} holds a TAB or a line break")
    Path(path).write_text("tasim-profile/1\n" + "".join(f"{k}\t{v}\n" for k, v in rows), encoding="utf-8")


# --------------------------------------------------------------------------- the engine's TDF grammar, by position

class TdfError(Exception):
    """A file the engine's parser refuses: in the game this is the "Parse error in .TDF File!" box and exit(1)."""


_BLANK = b" \t\r\n"


def _strip_comments(t: bytearray):
    """0x4C33A0, once over the whole text before parsing: `//` blanks up to (not including) the next LF; `/*` blanks
    up to and including the first `*/` (not nested; unterminated, it blanks to the end except the last byte).
    Comments are recognised anywhere, inside a value too."""
    n, i = len(t), 0
    while i < n:
        if t[i] == 0x2F and i + 1 < n and t[i + 1] == 0x2F:
            while i < n and t[i] != 0x0A:
                t[i] = 0x20
                i += 1
            continue
        if t[i] == 0x2F and i + 1 < n and t[i + 1] == 0x2A:
            t[i] = t[i + 1] = 0x20
            i += 2
            # the closing test looks at the byte before i, which the opener's own '*' never is: it is blanked
            while i < n:
                if t[i - 1] == 0x2A and t[i] == 0x2F:
                    t[i - 1] = t[i] = 0x20
                    i += 1
                    break
                t[i - 1] = 0x20
                i += 1
            continue
        i += 1


def _trim(b: bytes) -> str:
    return b.strip(_BLANK).decode("latin-1")


def tdf_sections(blob):
    """The top-level sections of a TDF-grammar file in FILE ORDER, as the engine's parser 0x4C2F60 builds them, each
    `{"_name": str, "keys": {lowercased key: str}, "sub": [nested sections, in order]}`.

    Not tascene.parse_tdf and not tagpu_tdf: those merge sections that share a name, and the engine's .GUI reader
    0x4AEAC0 walks the sections by index (0x4C3490(i)), one gadget each, so retail ARMCK1.GUI's repeated GADGET2..6
    are distinct gadgets. The grammar is the engine's (DISASSEMBLED, the Q1 scout answer):

    - the text ends at its first NUL byte (0x4C30A8 makes the file a C string); comments as `_strip_comments`;
    - blanks are space, TAB, CR and LF only;
    - `[name]` searches for `]` through the rest of the file; the name is trimmed and may be empty or repeated; the
      next non-blank byte must be `{`;
    - `}` closes a section; at the top level a stray `}` ends the whole parse silently;
    - anything else is a key: `=` and then `;` are searched through the rest of the file, so a value missing its `;`
      runs on to the next `;` anywhere later (retail ARMGEN.GUI's `attribs=256` swallows `colorf=15`);
    - a key given twice keeps the LAST value (0x4C3F98..0x4C403C replaces it), matched without case;
    - end of text inside a section, a missing `]`, `{`, `=` or `;` raise TdfError (the engine exits).

    A 0-byte file is a failed load for the engine (0x4C2F9A), not an empty one: TdfError too."""
    if isinstance(blob, str):
        blob = blob.encode("latin-1")
    if not blob:
        raise TdfError("an empty file, which the engine does not load")
    end = blob.find(b"\0")
    t = bytearray(blob if end < 0 else blob[:end])
    _strip_comments(t)
    s, n, pos = bytes(t), len(t), 0
    root = {"_name": "root", "keys": {}, "sub": []}
    stack = [root]
    while True:
        while pos < n and s[pos] in _BLANK:
            pos += 1
        cur = stack[-1]
        if pos >= n:
            if len(stack) > 1:
                raise TdfError(f"end of file inside [{cur['_name']}]")
            break
        c = s[pos]
        if c == 0x5B:                                   # '['
            j = s.find(b"]", pos)
            if j < 0:
                raise TdfError("a section's closing ']' is not found")
            name = _trim(s[pos + 1:j])
            k = j + 1
            while k < n and s[k] in _BLANK:
                k += 1
            if k >= n or s[k] != 0x7B:
                raise TdfError(f"no '{{' after [{name}]")
            sec = {"_name": name, "keys": {}, "sub": []}
            cur["sub"].append(sec)
            stack.append(sec)
            pos = k + 1
            continue
        if c == 0x7D:                                   # '}'
            if len(stack) == 1:
                break
            stack.pop()
            pos += 1
            continue
        j = s.find(b"=", pos)
        if j < 0:
            raise TdfError(f"a key in [{cur['_name']}] has no '='")
        k = s.find(b";", j + 1)
        if k < 0:
            raise TdfError(f"a value in [{cur['_name']}] has no ';'")
        cur["keys"][_trim(s[pos:j]).lower()] = _trim(s[j + 1:k])
        pos = k + 1
    return root["sub"]


def gui_sections(blob):
    """A .GUI file's gadgets as the reader 0x4AEAC0 takes them: `tdf_sections`, one gadget per top-level section,
    whatever the names. The engine's gadget count is the section count: it overwrites HEADER's `totalgadgets` with
    count − 1 (0x4AED48), so the number a file states is not read. A gadget's `[COMMON]` is `gadget_common`."""
    return tdf_sections(blob)


def gadget_common(sec):
    """The section's first nested section named COMMON without case (0x4AD355 → 0x4C3410), or None: the gadget's
    record then stays zero, which is id 0."""
    return next((s for s in sec["sub"] if s["_name"].lower() == "common"), None)


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

    def __init__(self, root: Path, exe: Path = None, archive: str = None, image: bytes = None):
        """The patterns come from `image`, the running image's bytes (`running_image`), when it is given, else from the
        executable's file; `archive` replaces the first mount pattern either spells (`mount_patterns`)."""
        self.root = Path(root)
        if not self.root.is_dir():
            raise VfsError(f"no game directory at {self.root}")
        if image is None:
            exe = Path(exe) if exe else self._loose_path(EXE_NAME)
            if not exe or not exe.is_file():
                raise VfsError(f"no {EXE_NAME} in {self.root} to read the archive patterns from (--exe)")
            image = exe.read_bytes()
        self.patterns = mount_patterns(image, archive)
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
        return loose_path(self.root, path)

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

    def size(self, path: str) -> int:
        """What the open test 0x4BBC40 answers for `path`: the size of the copy the engine opens, 0 when nothing holds
        it. A 0-byte file is as absent as a missing one to every probe that tests this value."""
        loose = self._loose_path(path)
        if loose:
            return loose.stat().st_size
        key = _key(path)
        for _name, arc in self.archives:
            if key in arc.files:
                return arc.files[key][1]
        return 0

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
        order, each path once, from the first source that holds it (the module's header: the shadow bit)."""
        pat = pattern.lower()
        out = [(rel, "loose") for rel, _p in self._loose_walk(top, recursive)
               if fnmatch.fnmatch(rel.rsplit("/", 1)[-1], pat)]
        seen = {rel for rel, _w in out}
        prefix = _key(top) + "/"
        for name, arc in self.archives:
            for path in arc.files:
                if not path.startswith(prefix) or path in seen:
                    continue
                rest = path[len(prefix):]
                if "/" in rest and not recursive:
                    continue
                if fnmatch.fnmatch(rest.rsplit("/", 1)[-1], pat):
                    out.append((path, name))
                    seen.add(path)
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
        return self.features().get(name.lower())

    def features(self):
        """{lowercased section name: (section, tdf path, where)} of every feature section, each from the first TDF that
        holds it, in the order the TDFs enumerate."""
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
        return self._features
