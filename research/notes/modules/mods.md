# Mods: a mod run from its package

`mod=<id>` in `impure.cfg` runs catalog mod `<id>` on the player's own exe from `mods\\<id>\\`, and the
result is the mod's drop-in folder: the same unit types, feature and weapon tables and build menus, and a
network game with a drop-in player whose handshake agrees on every unit's scripts, GUIs and downloads. With no `mod=` none of this does anything. The design and its reasons are in
[the mod manager plan](../mod-manager-plan.html); the sites are in [the hook map](hook-map.html).

*A module note: it states the module as it is now.*

## The files

- `tagpu_modload.c/.h`: the start-up sequence, the mount and the loose step.
- `tagpu_modpak.c/.h`: pure C, the pack `mods/patches.bin`, `file_key`, the image in memory and its
  hash. `tools/modpack/modpakcheck.c` runs it on the host, and `modpack.py verify` compares it with the
  Python that wrote the pack, on every known exe.
- `tagpu_modcat.c`: the catalog's `hide` list and option rules.
- `tools/modpack/modpack.py install ID GAMEDIR`: a package installed by hand, its recipe run from that
  folder's archives into `<id>.originals.hpi`.

## The sequence

The order matters more than any step. A mod that ships its changes in its exe has them in place before
Impure patches, and a Patch Loader writes after Impure's DllMain. Both orders are kept, whatever the
folder holds:

| when | what | why there |
|---|---|---|
| `DllMain`, after `cfg_load`, before `tagpu_apply_patches` | identify the exe and any loader; build stock + the mod's XOR from the file; compare the hash; write the mod's image or, for a loader mod, stock | Impure's limit table reads the image and compares it with a reference |
| last in `DllMain` | a loader mod's bytes by value; the mount and loose detours; a snapshot of each section's raw bytes; the jump at the entry point | a loader's list lands over Impure's bytes in the drop-in folder too. MEASURED overlap of Impure's writes with each mod's table: Escalation 2 B, Mayhem 3, ProTA 0, TA Zero 7, Twilight 3; of these, Mayhem and ProTA ship a loader |
| the exe's entry point | refuse, or put back every byte that differs from the snapshot, import slots included | after every other DllMain and outside the loader lock, before any game code |
| the first DirectDraw call | refuse if the entry stage never ran | the case it exists for is another DLL's jump over ours; a DLL calling DirectDraw from its own DllMain would reach it too |

**The reference.** The limit table (`tagpu_takeover_file_at`) and the takeover's image check compare the
process with "the exe file". Under a mod, that file is `tagpu_modload_reference_file`: the player's exe
turned into what the drop-in folder would hold, the target for an exe mod and stock for a loader mod. The
file on disk would say every table byte is foreign, and the limit table would refuse.

**The hash covers what the pack names.** `tagpu_modimg_hash` skips the pack's mask, the import structures
of stock and every known exe, which no two exes share. An exe with any other byte changed has another
`file_key` and refuses first.

## What refuses

The module acts only in the game: an exe without the engine's three sites in its file (retail's `TAE.EXE`
imports DirectDraw too) leaves `mod=` alone. In the game, each refusal goes through `tagpu_refuse`: the box,
`log\\startup-failure.txt`, exit. `mod=` is cleared and the store flushed first, so a player who kills the box
still gets the folder's own game next time.

At the entry point, before any game code:
- a name that is not a mod's, or one the catalog does not have;
- an exe no key knows, or a `dplayx.dll` holding `[Settings]` that no key knows;
- `mods\\<id>\\<id>.hpi` or `<id>.originals.hpi` missing, or a folder so deep the package's path passes the
  mounter's 0x100 bytes (`0x4BE0D1`), which would lose it at the next mount pass;
- the hash, which no known exe reaches: it guards against a pack that disagrees with itself;
- a build with no pack, no memory for the image, or a write that fails.

At the first mount, on the game thread:
- a package that does not open;
- a `totala1.hpi` whose hash is not an original's: another edition of the game. Every other archive that is
  not original is only left out and named in the log.

## What the game sees

**The mount list.** The detour on the mounter `0x4BE0B0` runs on the game thread inside
`InitTAHPIAry 0x41D4C0`. Its first call mounts the package and its originals; the list is append-only
(`0x4BE162`), so being first is what puts them ahead of everything the engine's own patterns add. After that a name is mounted only if it is
`impure-patch.ufo` or an original archive by SHA-256, and not in the mod's `hide`. A refused name returns 0,
which is what the engine sees for one already mounted, so it does not count toward `*.HPI`'s ten.
Archive hashes are cached in `mods\\archives.cache` by name, size and date. The candidates in a retail folder
total about 900 MB, hashed once on the game thread, and `*.HPI`'s ten-a-call means some of it falls at a later
`InitTAHPIAry` call: a menu click or Start. A full table or a long name costs a hash on every start, and a read
error stores nothing.

**The loose step.** The `fopen` call in `0x4BB2E0` and the `_findfirst` call in `0x4BC4B0` are retargeted. A
read or a listing whose first path component is a game-data folder finds nothing; everything else goes to
the CRT, so saves still work. A game-data folder is one of modpack's `GAME_DIRS` or a name the image gives one
(`FOLDER_NAMES`: `ZUnits`, `guiM`, `weaponE`, `zi`), with a `-<language>` suffix ignored. The log names each refused
file or listing that exists. The wiki's reader (`tagpu_vfs_close_loose`) has the same rule. A `palettes\\` folder
without the mod's `PALETTE.ALP` makes the engine rebuild that table at each start, and the result is correct.

**A file's place.** Every path has one answer, and the package reproduces it. Where the mount order alone decides,
one archive cannot keep the drop-in folder's order: LoadFeature takes a section from the first TDF enumerated that
holds it, and mods define a section twice with different values. MEASURED 2026-11-06: 1 section in Mayhem, 2 in ProTA,
22 in TA Zero, 33 in Twilight, 0 in Escalation. `modpack.py packages` copies those sections, as the drop-in folder reads
them, into `features\\!!section-winners.tdf`, which the managed enumeration reaches first: the package mounts first, the
name sorts first in its folder, and the walk takes a folder's entries in order, recursing at once. Weapons, where a later
section with one ID overwrites, and build-menu cells come out the same whichever file wins, so nothing is needed for them;
`modpack.py verify` checks all three over each image's real folders.

**The replays** name a managed mod as they name its drop-in folder, through `mod_by_archive`: the first archive's
`<id>.hpi`. Twilight has no archive pattern and is retail by name on both routes, told apart by its version text.

**Plain paths.** The strategic icons read their folder relative to `mods\\<id>\\` under a mod
(`iconset_load`), so `Icon\\` comes from the package. The options file the image names (`0x5098A3`) is copied
beside the exe when none is there, and never overwritten.

## How it is tested

`modpack.py verify` proves offline that the managed mount answers every path as the drop-in folder does, and reads every
feature section, weapon ID and build-menu cell the same. `tools/compat/tacompat.py wine` then has three kinds of managed
row (the `ta-compat-check` skill):

- **`managed-<id>`**, in the default list: installed into a clean stock folder, and compared with the drop-in row of the
  same run. At the menu: every unit type and the FBI's key at `+0x13E`. In a level: each type's ID, the weapon and feature
  tables by the recording's fingerprints, and each feature's name, metal, energy and dead chain. In a network game whose other
  player is the drop-in folder: every type's `CRC_all`, which the handshake alone fills, so the row fails if none is
  non-zero. Decoy unit files lie in the folder, which would remove the unit if the loose step were open, and the log must
  name one as refused. The log must also name the same mod for the replays.
- **`matrix-<id>-on-<base>`**, by the name `matrix`: each mod in a folder another mod's drop-in install left.
- **`managed-refuses-<why>`**, in the default list: the box must name the reason and `mod=` must be gone.

The rows run on Wine only: the Windows path has no install, so `tacompat.py windows` leaves them out.

The mount list itself is not compared with the drop-in folder's, because it differs by design. Its result is, above.

## Gaps

- Wine only so far.
- A jump over the entry point by another DLL has no setup that produces it, so `tagpu_modload_check`'s refusal has not
  been run.
- Managed Escalation needs the large-address bit to load a game. Its rows set it by hand until the 4 GB landing.
- The originals list is one install's hashes. A folder from another edition whose `totala1.hpi` matches, and whose other
  archives do not, loses those archives with only a log line.
- The mount and the loose step are not yet part of any recording's identity; the fingerprint is a later landing.
