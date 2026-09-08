# Ghidra project for TotalA.exe — bootstrap notes

Created 2026-08-31. Ghidra **12.1.3** at `tools/ghidra/` (system JDK 25 / Zulu — works;
the `sun.misc.Unsafe` warnings on every launch are harmless).

## What exists

- **Project:** `tools/ghidra-projects/TA.gpr` (+ `TA.rep/`), program name
  **`TotalA.exe.pristine`** — imported from `pristine/TotalA.exe.pristine`
  (md5 `8e74a1dffa1f5988624c52048f5b20cd`, untouched; Ghidra copies the bytes into its
  own DB). 32-bit PE, ImageBase 0x400000. Full auto-analysis ran (~34 s).
- **613 community symbols applied** (412 named functions — 26 of them function starts
  Ghidra's auto-analysis had missed — and 201 labels), plus **the TADR `tamem.h` struct
  corpus in the data-type manager** (145 structures / 18 enums incl. built-ins; all
  TADR layout-asserted sizes verified: `UnitStruct` 0x118, `UnitDefStruct` 0x249,
  `WeaponStruct` 0x115, `ProjectileStruct` 0x6B, `ExplosionStruct` 0x54,
  `DebrisStruct` 0x34, `TAdynmemStruct` 0x3923D, plus `Object3doStruct` 0x22,
  `Model3DONode` 0x40, `Model3DOFace` 0x20, `PrimitiveStruct` 0x36,
  `TAProgramStruct` 0x729).

## Files in tools/

| File | What |
|---|---|
| `ta_symbols.txt` | merged symbols, one `name address type` per line (`f` function, `l` label) — the classic ImportSymbolsScript format |
| `ta_symbols_sources.tsv` | provenance: `address<TAB>name<TAB>source` for every extracted pair (710 rows before de-dup) |
| `ta_symbols_conflicts.txt` | 83 addresses claimed by >1 source; the kept + alternate names (TADR wins) |
| `extract_ta_symbols.py` | regenerates the three files above from the vendor corpora |
| `make_tamem_ghidra.py` / `tamem_ghidra.h` | generator + generated C-only trim of TADR's `tamem.h` for Ghidra's CParser |
| `ghidra-scripts/ImportTASymbols.java` | post-script: applies a symbols file (equivalent of the retired stock `ImportSymbolsScript.py`) |
| `ghidra-scripts/ExportTASymbols.java` | post-script: dumps all USER_DEFINED symbols for verification |
| `ghidra-scripts/ImportTAStructs.java` | post-script: CParses a header into the program DTM |
| `ghidra-scripts/VerifyTAStructs.java` | post-script: asserts the key struct sizes above |
| `ghidra-scripts/DecompileTAFuncs.java` | post-script: decompile a csv of function addresses to a file (`funcs, out`) |
| `ghidra-scripts/WeaponSurvey.java` | post-script: scalar-operand scan + string-xref scan (follows pointer tables) + data-address xrefs (`out, scalars, string-regex, data-addrs`) |
| `ghidra-scripts/WeaponSurvey2.java` | post-script: instruction-text regex scan grouped by function (`out, regex;;regex…`) |
| `ghidra-scripts/CallersOf.java` | post-script: code xrefs to each function entry (`out, funcs`) |
| `ghidra-scripts/DisasmWindow.java` | post-script: disassembly windows around addresses Ghidra has no function for (`out, addr:before:after,…`) |

## Exact working commands (run from `tools/`)

```bash
REPO=$(git rev-parse --show-toplevel)   # analyzeHeadless wants absolute paths

# 1. one-time import + auto-analysis (already done; re-running would duplicate the program)
./ghidra/support/analyzeHeadless ghidra-projects TA \
    -import $REPO/pristine/TotalA.exe.pristine \
    -analysisTimeoutPerFile 3000

# 2. regenerate the merged symbols file from the corpora
python3 extract_ta_symbols.py

# 3. apply symbols to the existing program (idempotent)
./ghidra/support/analyzeHeadless ghidra-projects TA -process TotalA.exe.pristine -noanalysis \
    -scriptPath $REPO/tools/ghidra-scripts \
    -postScript ImportTASymbols.java $REPO/tools/ta_symbols.txt

# 4. verify: export user symbols and diff against ta_symbols.txt
./ghidra/support/analyzeHeadless ghidra-projects TA -process TotalA.exe.pristine -noanalysis \
    -scriptPath $REPO/tools/ghidra-scripts \
    -postScript ExportTASymbols.java /tmp/ta_symbols_export.txt

# 5. structs: regenerate the C-only header, parse it into the DTM, verify sizes
python3 make_tamem_ghidra.py
./ghidra/support/analyzeHeadless ghidra-projects TA -process TotalA.exe.pristine -noanalysis \
    -scriptPath $REPO/tools/ghidra-scripts \
    -postScript ImportTAStructs.java $REPO/tools/tamem_ghidra.h
./ghidra/support/analyzeHeadless ghidra-projects TA -process TotalA.exe.pristine -noanalysis \
    -scriptPath $REPO/tools/ghidra-scripts \
    -postScript VerifyTAStructs.java

# GUI on the same project
./ghidra/ghidraRun ghidra-projects/TA.gpr
```

Ad-hoc analysis pattern: write a GhidraScript (Java) into `ghidra-scripts/` and run it
with the `-process TotalA.exe.pristine -noanalysis -scriptPath … -postScript X.java args…`
form above. Script args arrive via `getScriptArgs()`.

## Symbol counts (extracted → won the address after de-dup)

| Source | Extracted | Kept |
|---|---:|---:|
| TADR Delphi fn-ptr table (`src/Recorder/TAMem/TA_FunctionsU.pas`) | 249 | 241 |
| TADR C++ fn-ptr casts (`src/DDraw/**`) | 108 | 47 |
| TADR Delphi consts (`TA_MemoryConstants.pas`) | 18 | 18 |
| TADR C++ named addr vars (hook/patch sites → labels) | 172 | 157 |
| ours (live-verified, tagpu) | 8 | 7 |
| totala-re CSV (curated names only) | 19 | 19 |
| totala-re docs (hand-transcribed from `docs/*.md`) | 136 | 124 |
| **total unique addresses** | | **613** |

Conflict rule: TADR wins (delphi-func > cpp-cast > delphi-const > cpp-var), then ours,
then totala-re; losers recorded in `ta_symbols_conflicts.txt`. Note the two
interpretation clashes worth remembering: TADR says 0x41D4C0 = `InitTAHPIAry`
(totala-re thought "DX caps check") and 0x4B4F10 = TA's own `malloc` (totala-re thought
"window services registration") — TADR is battle-tested, its names were kept.

## Gotchas

- **`-readOnly` takes no value** — `-readOnly false` kills the launch with
  `Bad argument: false`. Omit the flag entirely.
- **There is no stock `ImportSymbolsScript.py` in Ghidra 12.x** (Jython scripts are
  gone). `ghidra-scripts/ImportTASymbols.java` is the drop-in equivalent and reads the
  same `name address type` format.
- **A failed CParser post-script still saves partial types** into the program DTM
  (headless saves on script error). Harmless here — re-parsing is idempotent — but
  don't trust "added=N" deltas after a failed attempt.
- CParser is **C, not C++**: `tamem.h` needed `#include`s → stub typedefs,
  `static_assert` stripped, `bool` → `unsigned char`, `enum class` → `enum`, and the
  three trailing `namespace { enum }` wrappers unwrapped. All automated in
  `make_tamem_ghidra.py`; rerun it if TADR updates `tamem.h`, then re-run step 5.
- Only one headless/GUI instance may hold the project **lock** at a time
  (`TA.rep/…lock`); a crashed run may leave a stale lock file to delete.
- The 2,589 `fcn.00xxxxxx` auto-names in totala-re's CSV were deliberately **not**
  imported — they carry no information and would mask Ghidra's own `FUN_` naming.
- `totala-re`'s "391 annotated functions" (from the handoff) is really 2,608 auto
  boundaries + ~20 real names in the CSV; the value is in `docs/*.md`, transcribed here.

## Where the frame-composition work starts (Phase A / G3)

The corpus already brackets the render path: `DrawGameScreen` (0x468CF0) with
`ADDR_DrawGameScreen_PostDrawGUI` (0x46A308) and the end-of-frame blit
`DrawTAScreenBlitAddr` (0x46A3CE); our live-verified `TAFrame_Lock_callsite`
(0x4C650C) / `TAFrame_Unlock_callsite` (0x4C659B); the per-unit path `DrawUnit`
(0x45AC20) and the model-composite buffers `AlphaCompsteBuf2OFFScreen` (0x4B8500) /
`CompositeBuf2_OFFSCREEN` (0x4B8A80) next to the 0x458xxx rasteriser region
(`kNanoframeStart` 0x458DF1; TADR's composite-buffer size patch lives at 0x458195);
sprites via `GAF_DrawTransformed` (0x4C7580), `CopyGafToContext` (0x4B7F90),
`GAF_SequenceIndex2Frame` (0x4B7F30); geometry via `Open3DOFile`/`Parse3DOFile`
(0x4CB560/0x4CB590), `TextureMatch3DO` (0x42A140), `UNITS_CreateModelScripts`
(0x485D40).
