#!/usr/bin/env python3
"""Extract + merge the TA community RE corpus into one Ghidra symbols file.

Sources (all read-only):
  1. TADR Delphi  : vendor/TADR/src/Recorder/TAMem/TA_FunctionsU.pas      (named fn ptrs -> f)
                    vendor/TADR/src/Recorder/TAMem/TA_MemoryConstants.pas (data consts  -> l)
  2. TADR C++     : vendor/TADR/src/DDraw/**.{h,cpp}  fn-ptr casts (f) + named addr vars (l)
  3. totala-re    : vendor/totala-re/data/function_catalog.csv (curated names only, auto fcn.* skipped)
                    + hand-curated address->name table transcribed from vendor/totala-re/docs/*.md
  4. ours         : live-verified addresses from the tagpu project (see research wiki)

Output (all under tools/):
  ta_symbols.txt           one "name address type" per line (type f=function, l=label)
  ta_symbols_conflicts.txt losing names for addresses claimed by >1 source
  ta_symbols_sources.tsv   address<TAB>name<TAB>source provenance

Conflict rule: TADR wins (delphi > cpp-cast > cpp-var > const), then ours, then totala-re.
"""
import csv
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]   # the repository root
TADR = ROOT / "vendor/TADR"
TRE = ROOT / "vendor/totala-re"
OUT = ROOT / "tools"

ADDR_LO, ADDR_HI = 0x400000, 0x540000

# priority: lower number wins on address conflicts
SOURCES = {
    "TADR-delphi-func": 1,
    "TADR-cpp-cast": 2,
    "TADR-delphi-const": 3,
    "TADR-cpp-var": 4,
    "ours": 5,
    "totala-re-csv": 6,
    "totala-re-docs": 7,
}

entries = []  # (addr, name, type, source)


def add(addr, name, typ, source):
    if not (ADDR_LO <= addr < ADDR_HI):
        return
    name = re.sub(r"[^A-Za-z0-9_.$]", "_", name.strip())
    if not name or re.fullmatch(r"(sub|Addr|addr)_?[0-9A-Fa-fx]*", name):
        return  # address-echo names are worthless
    entries.append((addr, name, typ, source))


# --- 1. TADR Delphi function pointers ---------------------------------------
pas = (TADR / "src/Recorder/TAMem/TA_FunctionsU.pas").read_text(errors="replace")
for m in re.finditer(r"^\s*(\w+)\s*:\s*\w+\s*=\s*\w+\(\$\s*([0-9A-Fa-f]{4,8})\)\s*;",
                     pas, re.M):
    add(int(m.group(2), 16), m.group(1), "f", "TADR-delphi-func")

# --- 1b. TADR Delphi memory constants ---------------------------------------
pas = (TADR / "src/Recorder/TAMem/TA_MemoryConstants.pas").read_text(errors="replace")
for m in re.finditer(r"^\s*(\w+)\s*=\s*\$([0-9A-Fa-f]{4,8})\s*;", pas, re.M):
    add(int(m.group(2), 16), m.group(1), "l", "TADR-delphi-const")

# --- 2. TADR C++ ------------------------------------------------------------
cast_re = re.compile(
    r"\(\s*((?:_|Fn_)[A-Za-z]\w*|[A-Za-z_]\w*_t|[A-Za-z_]\w*Handler)\s*\)\s*"
    r"0x0{0,2}([45][0-9a-fA-F]{5})\b")
var_re = re.compile(
    r"(?:unsigned\s+int|unsigned\s+long|unsigned|DWORD|LPDWORD|int)\s+"
    r"([A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?0x0{0,2}([45][0-9a-fA-F]{5})\b")
for f in sorted((TADR / "src/DDraw").rglob("*")):
    if f.suffix not in (".cpp", ".h") or not f.is_file():
        continue
    text = f.read_text(errors="replace")
    for m in cast_re.finditer(text):
        name = m.group(1)
        name = re.sub(r"^Fn_", "", name)
        name = re.sub(r"^_+", "", name)
        name = re.sub(r"_t$", "", name)
        if name in ("LPVOID", "LPDWORD", "DWORD"):
            continue
        add(int(m.group(2), 16), name, "f", "TADR-cpp-cast")
    for m in var_re.finditer(text):
        # patch/hook sites are usually mid-function -> label, never function
        add(int(m.group(2), 16), m.group(1), "l", "TADR-cpp-var")

# --- 3. totala-re CSV (curated names only) ----------------------------------
with open(TRE / "data/function_catalog.csv", newline="") as fh:
    for row in csv.DictReader(fh):
        name = row["name"]
        if name.startswith("fcn."):
            continue  # radare2 auto-name, no information
        name = name.replace("::", "__").replace("sub.", "thunk_").replace("method.", "vmethod_")
        add(int(row["address"], 16), name, "f", "totala-re-csv")

# --- 3b. totala-re docs, hand-curated (docs/*.md read 2026-08-31) ------------
DOCS_FUNCS = {
    0x49E830: "CoreStartupAndMainLoop",
    0x4F1830: "crt_heap_init",
    0x4EB040: "crt_tls_init",
    0x4F1460: "crt_env_cmdline_init",
    0x4F15C0: "crt_init_1",
    0x4F0C50: "crt_init_2",
    0x4F0E10: "crt_isleadbyte",
    0x4E4600: "crt_exit",
    0x4E8890: "crt_alloc",
    0x4F22A0: "crt_heap_init_helper",
    0x4B52E0: "DisplayGlobalsInitDefaults",
    0x4B5980: "CreateMainWindowAndDXInit",
    0x4B5510: "DDrawDeviceCreateAndCaps",
    0x47BF70: "DDrawInitSecondary",
    0x4C54F0: "RendererBootstrap",
    0x4C2CC0: "FrameSchedulerDrain",
    0x4C2BD0: "GameplayTimingSeed",
    0x4C1A60: "FrameIntervalClamp",
    0x4C1B80: "KeyboardHotkeySampler",
    0x4C1D50: "KeyChordProcessor",
    0x4B4F10: "WindowServicesRegister",
    0x4B4FF0: "UIDDrawResourcesCleanup",
    0x42F960: "StringRegistryHelper",
    0x4CEF90: "DSoundCreateWrapper",
    0x4CEE50: "AudioMixerInit",
    0x4916A0: "CDListsResourceRefresh",
    0x4C61F0: "GameStateAllocator",
    0x4D85A0: "ResourceListDisposer",
    0x4DA1D0: "SEHCrashHandlerInstall",
    0x49EE30: "CmdlineArgsNormalize",
    0x4B62D0: "DXCapsCheck1",
    0x41D4C0: "DXCapsCheck2",
    0x4B62C0: "DXCapsCheck3",
    0x42F980: "RegistryLoadLanguage",
    0x428BB0: "GameplayControllersInit",
    0x491200: "UIPipelinesInit",
    0x499890: "IdleTick",
    0x4CF0B0: "SchedulerMaintenance10Hz",
    0x490F80: "AudioStatePrepare",
    0x4CE680: "AudioTimestampCapture",
    0x4CE410: "AudioStreamBuffersPrime",
    0x4CE260: "AudioDevicePause",
    0x4CD9D0: "AudioResumeCallbackBind",
    0x490FE0: "AudioPostResumeRebuild",
    0x4CEDC0: "AudioMixerFlagsCopyA",
    0x4CE7A0: "AudioMixerFlagsCopyB",
    0x4CE690: "AudioTimestampRestore",
    0x4C1B00: "KeyboardRawFetch",
    0x4C1AB0: "InputStateQuery",
    0x4E42B0: "ScreenshotPathBuild",
    0x4CB170: "ScreenshotSave",
    0x4C2DE0: "MouseStateCapture",
    0x4C2D60: "MouseStateReset",
    0x4B6370: "UIDeferredWorkFlush",
    0x4B6B50: "SleepBatch100ms",
    0x4B6110: "WindowTeardown",
    0x4CFBC0: "DeferredReleaseProcess",
    0x428730: "MissionAssetCascade",
    0x4AEDA0: "AssetCacheReload1",
    0x4AEF80: "AssetCacheReload2",
    0x431920: "FreeCDListsStringTable",
    0x431A20: "FreeTrackRecords",
    0x42F8C0: "CDAssetArrayClear",
    0x47F060: "CDAssetEntryFree",
    0x42A3B0: "PtrArrayRebuild1",
    0x47EEE0: "PtrArrayRebuild2",
    0x42A010: "PtrArrayRebuild3",
    0x4C62C0: "SchedulerQueuesRepopulate",
    0x43C350: "MapObjectRegistriesRebuild",
    0x42BCC0: "ScriptListsRefresh",
    0x452370: "GameplayTasksRefresh",
    0x434B90: "AnimAudioGlueRefresh",
    0x4D8E50: "RuntimeStatePrime",
    0x41D920: "EarlyInitHelper",
    0x4E6480: "ModulePathDiscovery",
    0x4E4860: "WindowClassRegistration",
    0x4E4AC0: "GlobalConfigLoadOnce",
    0x4D35F0: "HPI_SQSH_LZ77Decompress",
    0x4FDAE0: "MainSEHHandler",
    # GUI / UI pipeline (docs/UI_PIPELINE.md)
    0x4263B0: "GUI_MainMenuShell",
    0x4AA8F0: "GUI_LoadAndParse",
    0x4A81E0: "GUI_StageUpdateDraw",
    0x4A05E0: "GUI_GafFrameBlit",
    0x4C2EA0: "TDF_CtxInit",
    0x4C2F60: "TDF_ScriptLoad",
    0x4C3E10: "TDF_SectionIterA",
    0x4C3E20: "TDF_SectionIterB",
    0x4BAFF0: "PathNormalizeSlashes",
    0x4D83B0: "GUI_ResourceAlloc",
    0x4AEAC0: "GUI_TDFParser",
    0x4AD350: "GUI_ParseCommonFields",
    0x4AD890: "GUI_ParseButton",
    0x4ADC70: "GUI_ParseImagePanel",
    0x4A5F40: "GUI_ButtonDraw",
    0x4A1B40: "GUI_ListboxBuild",
    0x4A4D70: "GUI_TextInputRender",
    0x4A3EF0: "GUI_SliderUpdate",
    0x4A56B0: "GUI_LabelDraw",
    0x4A4980: "GUI_HotspotDraw",
    0x4A4660: "GUI_TimerState",
    0x4B6700: "ViewportWidthGet",
    0x4B6710: "ViewportHeightGet",
    0x4C69F0: "SurfaceCreateNamed",
    0x4C6B70: "SurfaceCopyBackground",
    0x4B0230: "GUI_BlitToFramebuffer",
    0x4B8C60: "GAF_BankLoad",
    0x4B8D40: "GAF_EntryLookup",
    0x4C5740: "GUI_SymbolLookup",
    0x4A7960: "GUI_MsgBoxPopup",
    0x49FA50: "GUI_ShellEntry",
    0x49FB10: "GUI_MouseFocusReset",
    0x4BBC40: "ResourceHandleOpen",
    0x4BBE50: "PaletteLoad",
    0x4B6340: "ClockTicksGet",
}
DOCS_DATA = {
    0x51F320: "gAppCtx_WindowContext",
    0x51F31C: "gStartupFlags",
    0x51F400: "gFocusLostFlag",
    0x51F410: "gRuntimeFlags",
    0x509720: "gAudioStagedFlag",
    0x51FB90: "gMainStateCachePtr",
    0x51FB94: "gSchedulerLastTick",
    0x51FB48: "gLanguageScratch",
    0x51FB50: "gLanguageString",
    0x51FBD0: "gSchedulerBlock",
    0x51E828: "gCDListsBuffer",
    0x51E84C: "gChannelEnableFlags",
    0x52B524: "gGameHeapHandle",
    0x52B650: "gCmdLinePtr",
    0x529ED0: "gOSBuildNumber",
    0x529ED4: "gOSVersionPacked",
    0x529ED8: "gWinMajorVer",
    0x529EDC: "gWinMinorVer",
    0x529F3C: "gEnvNormalizedPtr",
    0x50971C: "gTitleStringPtr",
    0x4FD050: "sDirectXWarningFmt",
    0x4E6718: "MainSEHRegistration",
}
for a, n in DOCS_FUNCS.items():
    add(a, n, "f", "totala-re-docs")
for a, n in DOCS_DATA.items():
    add(a, n, "l", "totala-re-docs")

# --- 4. ours (live-verified, tagpu project) ----------------------------------
OURS = [
    (0x511DE8, "g_mainstruct_ptr", "l"),
    (0x4C650C, "TAFrame_Lock_callsite", "l"),
    (0x4C659B, "TAFrame_Unlock_callsite", "l"),
    (0x4B563C, "SetDisplayMode_callsite", "l"),
    (0x4B5070, "CheckDirectXVersion", "f"),
    (0x4266A7, "DXWarning_branch_byte", "l"),
    (0x4ABD90, "TA_DialogBox_fn", "f"),
    (0x4D94E0, "CrashReport_Writer", "f"),
]
for a, n, t in OURS:
    add(a, n, t, "ours")

# --- merge -------------------------------------------------------------------
entries.sort(key=lambda e: (e[0], SOURCES[e[3]], e[1]))
winners = {}       # addr -> (name, type, source)
conflicts = []     # lines for conflicts file
provenance = []    # every extracted pair, deduped
seen_pairs = set()

for addr, name, typ, source in entries:
    if (addr, name, source) not in seen_pairs:
        seen_pairs.add((addr, name, source))
        provenance.append((addr, name, source))
    if addr not in winners:
        winners[addr] = (name, typ, source)
    else:
        wname, wtyp, wsrc = winners[addr]
        if name != wname:
            conflicts.append(
                f"0x{addr:06X}\tkept={wname} ({wsrc})\talternate={name} ({source})")

# uniquify duplicate names living at different addresses
used_names = {}
final = []
for addr in sorted(winners):
    name, typ, source = winners[addr]
    if name in used_names:
        name = f"{name}_{addr:06X}"
    used_names[name] = addr
    final.append((addr, name, typ, source))

with open(OUT / "ta_symbols.txt", "w") as fh:
    for addr, name, typ, _ in final:
        fh.write(f"{name} 0x{addr:06X} {typ}\n")

with open(OUT / "ta_symbols_conflicts.txt", "w") as fh:
    fh.write("# losing names for addresses claimed by more than one source\n")
    fh.write("\n".join(conflicts) + "\n")

with open(OUT / "ta_symbols_sources.tsv", "w") as fh:
    for addr, name, source in sorted(provenance):
        fh.write(f"0x{addr:06X}\t{name}\t{source}\n")

# report
from collections import Counter
c_win = Counter(s for _, _, _, s in final)
c_all = Counter(s for _, _, s in provenance)
print(f"unique addresses written : {len(final)}")
print(f"functions (f)            : {sum(1 for e in final if e[2] == 'f')}")
print(f"labels (l)               : {sum(1 for e in final if e[2] == 'l')}")
print(f"conflicts recorded       : {len(conflicts)}")
print("\nper-source (extracted -> won address):")
for s in SOURCES:
    print(f"  {s:20s} {c_all.get(s, 0):4d} -> {c_win.get(s, 0):4d}")
