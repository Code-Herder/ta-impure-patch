# Mods: a mod run from its package

`mod=<id>` in `impure.cfg` runs catalog mod `<id>` on the player's own exe from `mods\\<id>\\`, and the
result is the mod's drop-in folder: the same unit types with the same CRC, in the menu, a fight and a
network game. With no `mod=` none of this does anything. The design and its reasons are in
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
| last in `DllMain` | a loader mod's bytes by value; the mount and loose detours; a snapshot of every section; the jump at the entry point | a loader's list lands over Impure's bytes in the drop-in folder too. MEASURED overlap with Impure's writes: Escalation 2 B, Mayhem 3, ProTA 0, TA Zero 7, Twilight 3 |
| the exe's entry point | refuse, or put back every byte that differs from the snapshot, import slots included | after every other DllMain and outside the loader lock, before any game code |
| the first DirectDraw call | refuse if the entry stage never ran | another DLL's jump over ours is the only way to get here without it |

**The reference.** The limit table (`tagpu_takeover_file_at`) and the takeover's image check compare the
process with "the exe file". Under a mod, that file is `tagpu_modload_reference_file`: the player's exe
turned into what the drop-in folder would hold, the target for an exe mod and stock for a loader mod. The
file on disk would say every table byte is foreign, and the limit table would refuse.

**The hash covers what the pack names.** `tagpu_modimg_hash` skips the pack's mask, the import structures
of stock and every known exe, which no two exes share. An exe with any other byte changed has another
`file_key` and refuses first.

## What refuses

Each goes through `tagpu_refuse`: the box, `log\\startup-failure.txt`, exit. `mod=` is cleared and the store
flushed first, so a player who kills the box still gets the folder's own game next time.

- a name that is not a mod's, or one the catalog does not have;
- an exe no key knows, or a `dplayx.dll` holding `[Settings]` that no key knows;
- `mods\\<id>\\<id>.hpi` or `<id>.originals.hpi` missing;
- the hash, which no known exe reaches: it is the guard against a pack that disagrees with itself.

## What the game sees

**The mount list.** The detour on the mounter `0x4BE0B0` runs on the game thread inside
`InitTAHPIAry 0x41D4C0`. Its first call mounts the package and its originals with flag 1, which puts them
ahead of everything the engine's own patterns add. After that a name is mounted only if it is
`impure-patch.ufo` or an original archive by SHA-256, and not in the mod's `hide`. A refused name returns 0,
which is what the engine sees for one already mounted, so it does not count toward `*.HPI`'s ten.
Archive hashes are cached in `mods\\archives.cache` by name, size and date; the first start hashes about
700 MB on the game thread, inside start-up. A full table or a long name costs a hash on every start, never
a wrong answer.

**The loose step.** The `fopen` call in `0x4BB2E0` and the `_findfirst` call in `0x4BC4B0` are retargeted. A
read or a listing whose first path component is a game-data folder finds nothing; everything else goes to
the CRT, so saves still work. The log names each refused file that exists. The wiki's reader
(`tagpu_vfs_close_loose`) has the same rule.

**Plain paths.** The strategic icons read their folder relative to `mods\\<id>\\` under a mod
(`iconset_load`), so `Icon\\` comes from the package. The options file the image names (`0x5098A3`) is copied
beside the exe when none is there, and never overwritten.

## How it is tested

`tools/compat/tacompat.py wine` has three kinds of managed row (the `ta-compat-check` skill):

- **`managed-<id>`**, in the default list: installed into a clean stock folder, and compared with the
  drop-in row of the same run. Every unit type and its `CRC_all` must match. A decoy `units\\ARMCOM.FBI` lies
  in the folder, which would remove the unit if the loose step were open, and the log must name it.
- **`matrix-<id>-on-<base>`**, by the name `matrix`: each mod in a folder another mod's drop-in install left.
- **`managed-refuses-<why>`**, in the default list: the box must name the reason and `mod=` must be gone.

## Gaps

- Wine only so far.
- A jump over the entry point by another DLL has no setup that produces it, so `tagpu_modload_check`'s
  refusal has not been run.
- Managed Escalation needs the large-address bit to load a game. Its rows set it by hand until the 4 GB landing.
- The mount and the loose step are not yet part of any recording's identity; the fingerprint is a later landing.
