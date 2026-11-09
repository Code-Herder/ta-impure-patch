# Mods: a mod run from its package

`mod=<id>` in `impure.cfg` runs catalog mod `<id>` on the player's own exe from `mods\<id>\`, and the
result is the mod's drop-in folder: the same unit types, feature and weapon tables and build menus, and a
network game with a drop-in player whose handshake agrees on every unit's scripts, GUIs and downloads. With no `mod=` none of this does anything unless a mutator of the folder's own game is on (*Mutators*). The design and its reasons are in
[the mod manager plan](../mod-manager-plan.html); the sites are in [the hook map](hook-map.html). The player reaches all of it from the
main menu's MODS button: *The screen*, below.

*A module note: it states the module as it is now.*

## The files

- `tagpu_modload.c/.h`: the start-up sequence, the mount and the loose step.
- `tagpu_modinst.c/.h`: the download, the unpack, the recipe and the swap at start-up.
- `tagpu_modtar.c/.h`: the package's tar reader. Pure C.
- `tagpu_modpak.c/.h`: pure C, the pack `mods/patches.bin`, `file_key`, the image in memory and its
  hash. `tools/modpack/modpakcheck.c` runs it on the host, and `modpack.py verify` compares it with the
  Python that wrote the pack, on every known exe.
- `tagpu_modcat.c`: the catalog's `hide` list and option rules, and a mutator's entry and its verdicts: stale by the file, clash by the path. Pure C.
- `tagpu_modfit.c/.h`: what a base holds at a path, by reading its archives: files only, on any one thread.
- `tagpu_modprint.c/.h`: the fingerprint: the walk on its own thread, what counts, the data a peer reads. Names no engine memory.
- `tagpu_modgate.c/.h`: the battle room's Start against the fingerprints, and the line that says why.
- `tagpu_modmem.c/.h`: the exe's large-address bit on disk. Files only.
- `tagpu_modsui.c/.h`: the screen's state and input, on the game thread. Names no engine memory.
- `tagpu_modslay.c/.h`: the screen's window, a pure function of the page. Pure C.
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

The module acts only in the game: an exe that is not named `TotalA.exe` and lacks the engine's three sites in its file leaves
`mod=` alone, which is what any tool loading a `ddraw.dll` from this folder gets. Otherwise each refusal goes through `tagpu_refuse`: the box,
`log\startup-failure.txt`, exit. `mod=` is cleared and the store flushed first, so a player who kills the box
still gets the folder's own game next time, except when the mod is missing while another copy of the game holds the install lock.

At the entry point, before any game code:
- a name that is not a mod's, or one the catalog does not have;
- an exe no key knows, or a `dplayx.dll` holding `[Settings]` that no key knows;
- `mods\<id>\<id>.hpi` or `<id>.originals.hpi` missing, or a folder so deep the package's path passes the smaller of the
  mounter's two buffers (0x100 at `0x4BE0D1`, 0x104 at `0x4BDDC1`);
- the hash, which no known exe reaches: it guards against a pack that disagrees with itself;
- a build with no pack, no memory for the image, or a write that fails.

At the first mount, on the game thread:
- a package that does not open;
- no `totala1.hpi`, one that cannot be read, or one whose hash is not an original's: another edition of the game. Every other
  archive that is not original is only left out and named in the log.

## What the game sees

**The mount list.** The detour on the mounter `0x4BE0B0` runs on the game thread inside
`InitTAHPIAry 0x41D4C0`. Its first call mounts the package and its originals; the list is append-only
(`0x4BE162`), so being first is what puts them ahead of everything the engine's own patterns add. After that a name is mounted only if it is
`impure-patch.ufo` or an original archive by SHA-256, and not in the mod's `hide`. A refused name returns 0,
which is what the engine sees for one already mounted, so it does not count toward `*.HPI`'s ten.
Archive hashes are cached in `mods\archives.cache` by name, size and date. The candidates in a retail folder
total about 900 MB, hashed once on the game thread, and `*.HPI`'s ten-a-call means some of it falls at a later
`InitTAHPIAry` call: a menu click or Start. A full table or a long name costs a hash on every start, and a file that
cannot be read stores nothing.

**The loose step.** The `fopen` call in `0x4BB2E0` and the `_findfirst` call in `0x4BC4B0` are retargeted. A
read or a listing whose first path component is a game-data folder finds nothing; everything else goes to
the CRT, so saves still work. A game-data folder is one of modpack's `GAME_DIRS` or a name the image gives one
(`FOLDER_NAMES`: `ZUnits`, `guiM`, `weaponE`, `zi`), with a `-<language>` suffix ignored. The log names each refused
file or listing that exists, once, up to 32. The wiki's reader (`tagpu_vfs_close_loose`) has the same rule. A `palettes\`
folder without the mod's `PALETTE.ALP` makes the engine rebuild that table and write it at each start
(`0x42E165`, `0x42E1BF`); the rebuilt table is not measured against the file.

**A file's place.** Every path has one answer, and the package reproduces it. But a name inside a file can depend on where
the file lies in the mount, and one archive cannot keep the drop-in folder's order. Two readers take the first file listed
that holds a name: a feature section (LoadFeature, over the recursive walk) and a texture's sequence (the bank filled in
listing order from `textures\*.GAF`, searched from index 0). Mods do hold a name twice with different values: MEASURED
2026-11-06, the first holder changes for 1 section in Mayhem, 2 in ProTA, 22 in TA Zero, 33 in Twilight and 0 in Escalation,
and for one texture, Twilight's `wheel`, which 22 models name. `modpack.py packages` (`settle_places`) keeps the drop-in's winner
without shipping a byte of the original game:
- a copy that comes first in the package is **renamed where it stands**, `[name~]` in a TDF and `name~` in a GAF's entry, since
  nothing asks for the new name. Those files are the mod's own. Names renamed: Escalation 0, Mayhem 0, ProTA 1, TA Zero 22, Twilight 34;
- a winner that is an original file behind another original **goes in the recipe**, ahead of retail: `features\urban\cars.tdf`
  in Mayhem and ProTA, whose archives list `cars2.tdf` the other way round from the mods' own.

Weapons, where a later section with one ID overwrites, and build-menu cells come out the same whichever file wins, so they
need nothing. `modpack.py verify` reads all four by the engine's grammar over each image's real folders, and undoes each rename
to prove the file otherwise equals the drop-in's.

**The replays** name a managed mod as they name its drop-in folder, through `mod_by_archive`: the first archive's
`<id>.hpi`. Twilight has no archive pattern and is retail by name on both routes, told apart by its version text.

**Plain paths.** The strategic icons read their folder relative to `mods\<id>\` under a mod
(`iconset_load`), so `Icon\` comes from the package. The options file the image names (`0x5098A3`) is copied
beside the exe when none is there, and never overwritten.

## Mutators

A mutator is game files over one base mod: its catalog entry has `base`, a mod's id or `none` for the folder's own game, `files`, `base_hashes` and `hides`. Its package is one archive,
`mods\\<id>\\<id>.hpi`, and no recipe runs. **No mutator ships until a real one exists**, so the committed catalog has none: `modpack.py mutator FOLDER --id .. --base ..` writes the package and an entry in `mods/mutators/`,
which `modpack.py catalog` adds. It reads the base as the mount does and records the SHA-256 of each file the base holds at a path the mutator carries, and it refuses a file of the original game by content.

**Which are on** is in the base's `profiles\\<base>.ini`: `mutator.<id>=on`, with no key off. A mutator is in play whole or not at all: nothing chooses its files one by one, and nothing the player sets takes one of the base's archives out (*The Mods screen*).

**The mount.** The first call of the mounter runs `mount_mutators` ahead of everything, in the catalog's order. A mutator mounts only if it is on and installed, no file still in play differs from what it was built on, and no mutator already mounted
holds one of its paths. The hash check runs at every start because a base can be swapped at that very start, and what the screen shows is only what this will do. A hide
leaves the base's two archives right after each mounts, by editing the engine's directory ([the engine map](../engine/process.html), *The directory in memory*). Each pointer read there is held inside the block first. A hide reaches `<base>.hpi` and `<base>.originals.hpi` only, so the folder's
original archives answer the path next. `tagpu_modload_each_gone` tells the wiki's reader the same removals, through `tagpu_hpi_forget`.

With no `mod=`, `tagpu_modload_end` takes the mounter's detour only when a mutator of `none` is on, and never on a control launch. Nothing else is filtered and the loose step stays open: that launch's other detours pass everything through. An exe whose mounter is not the expected bytes keeps its own game, with a log line.

**The fit on the screen.** Each opening and after each install, `fit_thread` reads the base's files once the archive scan has ended (`scan_thread`, which hashes the folder's archives off the game thread and keeps up to 40 it finds original), and hands back up to eight stale paths per mutator; the pane names the first.
Manage lists a mutator under its base's row, only for the base chosen, with a check. A stale one reads Stale with no check and its pane names the file; one that clashes with what the mount will take reads Conflict and names both: `walk_taken` walks them as the mount does, and one that is on gives way only to those taken before it, one that is not to any taken. One already on keeps its check, so a player can always turn it off. Explore lists the chosen base's mutators; Updates lists one with a newer
version, and one a base update broke as "Needs an update for X", which the tab's count includes.

**The replays.** `RT_MUTATORS` holds what the start mounted, `name version, name version`, and no record means none. The browser's index line keeps it (`INFO_GEN` 2). Where the fingerprint differs, the hint says "The installed archives differ.", and where the lists differ too the footer, the only line with the width, names them: "Recorded with X 1.0. You have none."; a selection that needs neither puts the disk line back.

## The fingerprint

Two Impure players with different game files do not start together. Stock would: its per-unit handshake disables the types that differ on both sides with no message, and starts ([networking](../networking-lobbies.html)). The fingerprint is two 16-byte hashes and a label, compared in the battle room.

**The content's hash** walks the files under every folder the mounted archives have, each path once, loose or from the archive it opens from: `tagpu_vfs`, the same reader the wiki uses, over the engine's mount list after the mount's removals. So a mutator and what it hides reach it with nothing here knowing them. Each file hashes as its lower-cased path and its bytes, in path order.

**One mod has one print however it is installed.** The managed mount answers every path as the drop-in folder does, with one exception by design: a package can rename a section so the engine's order finds the drop-in's winner, and that file's bytes differ. The catalog's `same_as` names each such file with both hashes, and the walk counts one as the other only when the bytes are exactly the package's, so a mutator on that path still differs. `modpack.py catalog` derives the list and `modpack.py verify` compares the two sets of counted files for every mod.

- **Left out:** `.gaf`, `.pcx`, `.bmp`, `.wav`, `.fnt`; a `.gui` no unit names, where a unit's menu page is `<unit><n>.gui`; all of `maps\`, since stock compares the map itself (`0x4373A0` against PlayerInfo `+0xA9`, `0x4481BE`) and players' map packs differ; and all of `camps\`, since a network game runs no campaign [INFERRED]. MEASURED: a retail folder holds loose examples there that a managed mod's closed loose step does not see, so counting them stops a managed player and a drop-in player of one mod. An unknown type counts.
- **Loose files** count only under a folder some archive has at its root, so `log\`, `mods\` and what sits beside the exe never do. A managed mod's closed loose step is honoured.
- **A file at an archive's own root** does not count: MEASURED over retail and every fixture there are two, `Copyright.txt` and `GAME NOTES.txt`, and the first would stop anyone without `Example.ufo`.

**The exe's hash** is the image hash of the mod's table, `MODPATCH.hash`, when the player runs a catalog mod. A drop-in folder finds its mod through the pack: its exe, or stock with a known Patch Loader, names the same XOR as the mod. Anything else hashes the exe's `file_key` and any Patch Loader's, which matches only a folder with those same files.

**When.** `InitTAHPIAry` mounts at most ten `.hpi` a call, so `tagpu_modprint_start` calls it until a call mounts nothing, on the game thread, when the main menu's MULTI is pressed; the walk then runs on its own thread. `mp_create_player` waits for it, which is an ordering: no player reaches a room without its print, and a lobby launch that skipped the menu starts the walk there. MEASURED 2026-11-06 on the reference setup, Wine: ProTA, 3530 files of 6074, 242 MB, 2.5 s. A mutator, a hide or a `file.` key takes a restart, so one print serves the process. An archive dropped into the folder while the game runs is the hole: a later call mounts it and the print does not see it. Each top folder's digest goes to the log, so two players told they differ can find the folder.

**The carrier** is the data DirectPlay already binds to each player: `CreatePlayer`'s 21 bytes, the recorder's 280-byte key, and the print's 160-byte tail ([network](../engine/network.html)). The tail begins `IMFP` and a version. A value in it is data: `tagpu_modprint_tail_read` takes it apart with a bounded length, and the label comes back terminated and printable.

**The gate.** Both calls of the everyone-ready test `0x456760` also ask `everyone_agrees`, and so does the click on START at `0x44871D`, since its action asks about game CDs and teams but not readiness, and a stale grayed state is all that stands in front of it. Each asks DirectPlay's record of each remote seat, so a player who leaves stops counting at once. A seat with no tail is compared with nobody: an older Impure. Each flip in `LOUNGE2.GUI`, each player gets three chat lines per differing seat, once per seat and print: who, then *There* and *Here* when the labels differ.

- **A player who sends none.** A joiner sending stock's 21 bytes, or an older Impure's 301, is taken by a current one and compared with nobody. A stock host refuses an Impure joiner, since it takes exactly 21 bytes: that has been so since the key.
- **A player of this build cannot join a host older than it.** The older host's stub takes 21 or exactly 301 bytes, and MEASURED on Wine the joiner lands back on Select Game with no message: the refusal is the older build's, so nothing in the new one can explain it. A current host's stub takes the key with up to 1024 bytes of tail, so a later, longer print will not repeat this.

## 4 GB

The large-address bit has to be in the exe's header before the system loads the process, so a mod that needs the room gets it at Confirm. A catalog entry's `memory` says what its exe needs: `4gb` (Escalation), `bit-off`, or nothing.

**`4gb`, on an exe without the bit.** `tagpu_modmem_apply` writes a copy with the bit set and the header's checksum fixed to `TotalA.exe.4gb.tmp`, renames the running `TotalA.exe` to `TotalA.exe.4gb-old`, the copy to `TotalA.exe` and only then the old file over `TotalA.exe.pre-4gb`, and the restart that Confirm always does runs it. A swap refused at any step leaves `.pre-4gb` as it was. Only the bit and the checksum differ, and `file_key` leaves both out, so the exe is still the exe every table knows, a drop-in's and a managed mod's fingerprint included.

**Before the choice is saved.** A folder that will not take the file refuses Confirm with the reason and nothing changed, but for the swap whose old file could not go back, since the other order would leave `mod=` naming a mod its exe cannot load. A choice that then fails to save keeps the mod that was running and the exe as changed. That leaves the exe differing from what the running mod's `memory` asks, which opening the screen offers to settle, or a bit on under a mod with no `memory`, which every row has run with.

**The bit stays on.** An entry with no `memory` leaves the exe as it is, and so does the folder's own game. The price is a row for each: `laa-<setup>` in the [compatibility suite](../compat/setups.html) runs retail, each mod's drop-in and each managed mod with the bit, on Wine, and Escalation's own managed row already does. Retail and a drop-in have no mark to take the bit off: a failure there is a bug to fix.

**`bit-off`** is for an entry that fails its row until it is fixed: Confirm puts the `.pre-4gb` copy back when that copy is this exe, byte for byte what was there, and otherwise clears the bit in place. The running file goes to `TotalA.exe.4gb-old`, removed at a later start. No entry is marked so today.

**A reverted exe.** A file check that restores the old exe leaves `mod=` naming a mod without its bit. Opening the screen compares them, and "Restart to apply" is offered with nothing else to apply. Until then the game runs on the 2 GB it has, and such a mod runs out of memory loading a level. `TotalA.exe.pre-4gb` is replaced by the file that was reverted to.

## Download and install

`tagpu_modinst.c` installs a catalog mod beside the live folder, on one background thread that touches only files: no
engine memory and no render state, so it is in no list of `thread-split.allow`. Its progress is three atomics
(`tagpu_modinst_status`), and it holds a reference to the DLL until its last instruction.

1. **The download** goes to `mods\<id>.part` through WinHTTP, loaded when a download runs so that nothing about a normal
   start changes. Bytes past the catalog's size are never written. The address is the package's `url`, resolved against the
   entry's own `base_url` (`tagpu_modbase.on` beside the game stands in for it, for the harness). Only ProTA's entry names one:
   `https://github.com/Code-Herder/ta-impure-mods/releases/download`, one GitHub release per mod tagged with its id, the package
   its asset. GitHub answers with a redirect to its asset host, which WinHTTP follows. An entry with no `base_url` is not
   offered (*The tabs*), and a download of one reports that the catalog names no place for it.
2. **The check**: the file's size and SHA-256 against the catalog, by `modsha`, before anything reads it as a package.
3. **The unpack** into `mods\<id>.new\`, zstd with its window bounded to 128 MiB and then `tagpu_modtar.c`, whose list of
   acceptable files is the catalog's. A member must have a safe path, be declared, come once and be a regular file of the
   declared size and hash; the archive must reach its own end, with every declared file seen.
4. **The recipe** (`<id>.originals.tsv`) copies the original files out of the folder's own archives through `tagpu_hpi`, each
   checked by SHA-256, into `<id>.originals.hpi`, written by `tagpu_ufo.c`. An archive that is missing is named; one that holds
   other bytes is "not the game's", so a modded folder's replaced `rev31.gp3` stops the install with a reason.
5. **The marker**, `installed.txt`, written last and after every file is flushed, holds the version. A package cannot
   declare a file of that name. After the swap it is how `tagpu_modinst_installed` knows the version.

**The swap**, `tagpu_modinst_sweep`, runs at process attach before `tagpu_modload_begin`, so before anything looks in
`mods\`, and with `mod=` or without. It acts in the exe's own folder, never the current directory, only in the game
(`tagpu_modload_is_game`), and only on a catalog id's `.part`, `.new` and `.old`: any other name in `mods\` is the player's. It deletes the
`.part` and a `.new` without its marker, then moves the live folder to `<id>.old`,
the new one into its place and deletes the old. A crash between any two steps leaves folders with one answer: a finished new
wins, `.old` beside a live folder is deleted, and `.old` alone goes back. A marker always goes before the rest of its folder,
and one that cannot be deleted keeps the whole folder, because a folder with a marker is a whole version to the sweep. A waiting
update goes only once the next package's download has passed its hash.
The player's profile (`profiles\<id>.ini`) is outside all of this. On Windows a rename is refused while a file inside is open, which
leaves it all for another start; that has not run, and Wine may rename a folder another copy has open.

**The lock**: `mods\install.lock`, opened with no sharing and deleted on close. The system releases it with the process however
that ends, so a start can tell another copy's running install from what a dead one left, and the sweep then leaves `mods\` alone.
A start that finds its mod missing while the lock is held says so and keeps `mod=`.

**The way in**, until the Mods screen: in the game, a file `tagpu_modinst.on` holding `<id> <base url>` installs that mod at each start
while it exists.

## The screen

The MODS button lies under REPLAYS on each menu, a second record appended where REPLAYS' is (`tagpu_replays.c` `mainmenu_visit`), with quick key `O`
and the plate grown to hold both: `art/ui/*/plate.png` is 115 x 58. Twilight has no plate. No mod's menu needed another move for it:
MEASURED on each v1 mod's main menu and on retail, all but Twilight at (272,416), Twilight at (317,416).

**Drawn on the wiki's layer, in the shell.** `tagpu_wikistate_present` takes a frame with no level in it to `mods_present`, which
builds the page with `tagpu_modslay_build` over the wiki's kit and art, and everything after that is the wiki's: its ops, its pass (`tagpu_vk_hud.c`, built at its first use, its art's load started in the shell), its
hit map (`tagpu_wikistate_drawn`) and its cursor. The page is drawn over a full-screen backdrop, in wiki units.

**What crosses.** The page goes to the render thread and the hit map comes back, each copied under one lock held over a memcpy, in `tagpu_modsui.c`.
No packet carries it: a shell packet holds a header and no area (`fill_shell`), and `tagpu_cmd_take` runs only at an in-play draw, so neither
way is open at the menu. A click reads the map of the frame last drawn, so it lands on what the player saw, and a page whose rows or pane have changed since that frame drops it: a map carries the generation of the page it was drawn from, which a change of the tab, the selection or the rows' mods advances, and press and release must both find it current. An index out of a hit key
is bounded against the page it is about before it is used.

**Modal, by the wiki's capture.** `tagpu_wiki_capture` holds the engine's pointer where it was and reads its key polls up, and the window's messages
and the injected path stop at `tagpu_modsui_*` (`tagpu_wiki_message`, `tagpu_wiki_mouse`, `tagpu_wiki_key`). A press taken owes its release, and only that
release is taken: the one of the key that opened the screen is the engine's. Of Alt's chords only Alt+F4 goes on, to the system; the others stop here, since TA's window procedure pushes the letter and its GUI dispatch's test of the Alt key reads up under the capture. Controls act on the release. Moves call
`mouse_note_client`, since no move reaches cnc-ddraw's store under the capture and the drawn cursor follows that point.

**The tabs.** Manage is `This folder` first, then what is installed, each with a radio. Explore is the catalog less that, each with Install. Updates is what is
installed at another version than the catalog's, each with Update, and Update all queues them. Explore and Updates offer only an
entry that names its `base_url`, or every entry while `tagpu_modbase.on` names a server (read at each open); a mod installed
without one shows on Manage alone. The worker is `tagpu_modinst`, one install at a time; a finished one is
`ready` for this session and waits in `mods\\<id>.new` for the next start. A failure's reason stands in the pane.

**The pane.** Nothing on the screen is a game setting: the only choices it takes are which mod runs and which of its mutators are on. Play options are the mod's to decide, so the pane shows them and sets none: each row is its label and the value in force, from `tagpu_modcat_rules`, or the running
policy for this folder, over a value `profiles\\<id>.ini` holds (the in-game Impure options still write one), with no control and no hit. A forbidden one is grey with the mod's reason. Nor does the player choose archives or files: under a mod the mount
leaves out what the mod's `hide` list names (never `totala1.hpi`, which every mod needs) and what does not hash as an original, and nothing else; a mutator is mounted whole or not at all. A `hide.<archive>` or `file.<path>` key an older build wrote is
passed over by the profile reader like any key it does not read, and stays in the file.

**The showcase.** A mod whose catalog entry has a `show` (Total Mayhem and ProTA) gets it in place of the pane's title, meta and summary: its logo, the version and its authors, its website, a rule, a banner, then its text. `build_show` parses the catalog and `tagpu_modslay_show` fills `MODSSHOW` (`tagpu_modslay.h`): up to 64 blocks, a list's items each one, and 6144 bytes of strings, the bounds `tools/modpack/modpack.py` `check_show` holds the catalog to, so a showcase is drawn whole or not at all. A block is a paragraph, a section's opening line (one blank line above it), a list's item (a list is filled down its columns, the kit's dot for a bullet) or a paragraph ending in a link. The pictures are the kit's whole parts `logo_<id>`, `banner_<id>` and `backdrop_<id>`, each drawn when the kit has it and sized by its own shape; with a backdrop the head lies over it from the left and the backdrop covers the head from its right. Nothing is read from the web: the pictures are packed with the kit and the text with the catalog. A link is its run's colour (`LINK_RGB`) with no underline, and its key `link:<n>` (0 the website, then each link block): a click opens the catalog's https address in the player's browser, `ShellExecuteA` on a thread of its own, through shell32 loaded then, which the DLL does not import. A test launch (`tagpu_regstore_active`) logs the address instead. The note stands under the text and the play options under it.

**Confirm's restart.** Confirm shows only with something to do: another mod chosen, a mutator of the running mod turned on or off, or its update ready. It writes `mod=` (nothing for this folder), flushes the store and goes on only when
`tagpu_settings_saved` says the file holds it; otherwise a note says so and the game stays. `tagpu_rguard_restart_self` starts the same exe with the same command line, naming this process in
`TAGPU_MODS_RESTART`, and `tagpu_rguard_attach` waits for it, since `WinMain` refuses to start beside the semaphore the old process holds. **The old process is terminated, not exited.** `ExitProcess` kills the render thread
wherever it stands and then runs the detaches, the graphics driver's among them, and MEASURED on Wine with an NVIDIA device a process that did so stayed in its detach for minutes, holding that semaphore. The engine's own exit
releases the device first, which a click cannot reach. Terminating runs no detach, so the window's place is saved ahead of it with `cfg_save`, and the player's files before that. The new process is not a relaunch
after a crash, so it may still make one.

**When it cannot draw.** The pass is built at its first use and can refuse then: on llvmpipe, whose device memory is mapped into the process, with the restorer on, the 2 GB address space has no 12 MB run within seconds of the menu, and `vkMapMemory` returns success and `(void*)-1` (MEASURED: largest free run 63 MB at the first frame, 10 MB six seconds on). The pass tests that pointer and refuses, the open screen closes itself, and the next click gets the box; `tagpu_hudrefuse.on` makes the build refuse on demand for the test.

**The boxes.** Before opening the button asks whether the exe is one the pack knows (`tagpu_modload_exe_known`), and then whether the layer can draw: the Vulkan renderer, the wiki's art loaded with the screen's layout in it. Either failing gets the engine's OK box, `0x4ABD90`
([the UI map](../engine/ui.html)), and the menu goes on after it. The art still loading is no failure: the screen opens and draws with its first frame after.

**The art.** `art/mods/layout.json` lies beside the wiki's, packed in `WIKIPAK` as `mods/layout.json`. Thirteen parts are not painted: `tools/mods/genparts.py` derives the check, radio, progress, tab and badge parts from the kit, and appends them to `art/wiki/kit/`. `fit.py --set wiki`
runs it after fitting. A painted part of the same name replaces one with no code change. It also makes the showcase's pictures parts, from a mod's own art filed under `art/mods/` by its id (`logos/`, `banners/`, `backdrops/`): a logo and a banner as filed, a backdrop darkened from its left, where the head's logo and text lie over it.


`tools/mods/modslaycheck.py` pairs the layout with `tools/mods/mockup/`, every part, control, text line and baseline at five screens and 63 cases, both showcases among them (`--only <word>` runs the cases whose state holds it); `--check-only` is the check the DLL runs at the load: the fullest page, each array at its
longest, builds whole. `tools/mods/modsscreen.py` plays the screen in the game on a private display, and `tools/mods/modmp.py` plays the fingerprint's two-player cases. With no GPU to use, `--software` runs it on llvmpipe with the restorer off and only that driver loaded, since the screen's pass takes about 70 MB there and the loader's other drivers about 240 MB. `flow` clicks the tabs, sees the pane take no click but a showcase's link, finds ProTA's showcase whole from the catalog and its website's click logged by the test launch, scrolls it with the wheel, Install against a catalog that names no place and then a local server, Update and Confirm, and sees the game
restart on the chosen mod with the update swapped in and an archive an older build's `hide.` key names still mounted, and back. `real` does part of it by hardware events in a window larger than the shell, which is the only way the drawn cursor and the messages are run. `boxes` is both refusals,
`menus` photographs each mod's menu and the screen over it, for the eye, and `mutators` builds three test mutators on ProTA, installs them from Explore, finds the stale and the conflicting one named, and compares a skirmish's picture against the base's with one on, mounted whole under an older build's `file.` key for its file, and with a hide.
`hosted`, run only when asked for since it downloads from the internet, installs each entry with a `base_url` from that real
address through the screen, with no local server, and starts the game on it: Total Mayhem and ProTA from GitHub, through its redirect,
MEASURED 2026-11-08 on Wine (Explore offering those two alone, each install, each restart mounting `mods\<id>\<id>.hpi`); ProTA's
alone ran on Windows on 2026-11-08.

`tools/modpack/installtest.py` needs no game: the tar reader natively under the sanitizers, and the module itself under Wine with its
real WinHTTP, against a local server. `tests/modinsttest.exe` is built with a hook that ends the process at a chosen step, as a
crash would. It checks every bad download, each file that can be wrong inside a package whose outer hash is right, a death during the
download, the unpack and the recipe, before the marker and just after a waiting marker's deletion, an update, a marker held open, the swap's
states, a second copy during an install, a missing or foreign archive, the real DLL's attach in a tool, in the game and from another
current directory, and that all five mods install to what `modpack.py install` builds. `managed-fetched-prota` in the suite takes one mod all the way in the
real DLL: downloaded at one start, swapped in at the next, played and compared with its drop-in row.


`modpack.py verify` proves offline that the managed mount answers every path as the drop-in folder does, and reads every
feature section, weapon ID and build-menu cell the same. `tools/compat/tacompat.py wine` then has three kinds of managed
row (the `ta-compat-check` skill):

- **`managed-<id>`**, in the default list, needing the fight and the network game (with `--battle 0` or `--mp 0` it fails, as
  nothing was compared): installed into a clean stock folder, and compared with the drop-in row of the same run. At the menu: every unit type and the FBI's key at `+0x13E`. In a level: each type's ID, the weapon and feature
  tables by the recording's fingerprints, and each feature's name, metal, energy and dead chain. In a network game whose other
  player is the drop-in folder: every type's `CRC_all`, which the handshake alone fills, so the row fails if none is
  non-zero. Decoy unit files lie in the folder, which would remove the unit if the loose step were open, and the log must
  name one as refused. The log must also name the same mod for the replays.
- **`matrix-<id>-on-<base>`**, by the name `matrix`: each mod in a folder another mod's drop-in install left.
- **`managed-refuses-<why>`**, in the default list: the box must name the reason and `mod=` must be gone.

The rows run on Wine only: the Windows path has no install, so `tacompat.py windows` leaves them out.

The mount list itself is not compared with the drop-in folder's, because it differs by design. Its result is, above.

## Gaps

- Wine only so far, the install included, and only over plain HTTP: no download has run through TLS on any system, and the
  catalog names no server yet.
- The refusal that keeps `mod=` when another copy is mid-install has its lock test run (`modinsttest locked`) but not its box, in a game.
- A catalog id dropped by a later catalog is never settled, so a lone `<id>.old` from a crash stays where it is.
- In a folder whose name is outside the ANSI code page, `game_root` finds no `mods\`: the sweep does nothing and an install is refused.
- A launcher that starts the game from another current directory reads `impure.cfg` and tests `mods\` there, while the sweep acts in the exe's
  folder and the engine moves to it at its first mount pass (`0x49F540`, engine map).
- One install at a time, with no way to stop one but ending the game. A queue runs on with the screen shut, starting its next install only in the shell, and is lost at an exit with no word.
- The `(void*)-1` test covers the HUD's pass and the staging buffers. The other passes' `vkMapMemory` still trust the success, and a process can vanish with no ErrorLog on llvmpipe around the restorer's self-test, on a build without the screen too.
- The screen has run on Wine only, windowed, on an NVIDIA device and on llvmpipe. Steam loses track of a game Confirm restarts, as the plan accepts.
- The thirteen generated parts stand until the owner paints them.
- No real mutator exists, so everything about them has run on test mutators the harness builds.
- **A map pack with more than maps counts.** Only `maps\` is left out, so a `.ufo` that also ships its own features, sections, textures or models stops Start against anyone without it, even when nobody plays that map. Retail's own map archives hold nothing else that counts. No community pack has been measured. Whether their additions should count is the owner's decision: features are simulation data, so leaving them out is not obviously right.
- Other retail editions have not been compared. `install\` and `installres\` count, as an unknown type does, so an edition that differs there stops Start.
- The stop test can end early when one call prunes an archive and mounts another, leaving the count unchanged (`tagpu_replays.c`), and the print then misses what a later call adds. It also stops at 64 calls, 640 `.hpi`, and the walk at 4096 archives, past which only the count changes the print.
- A loose file in a folder no archive has is not counted, though the game can open it.
- `tagpu_modprint_stock.on` passes the gate for whoever creates it: the fingerprint keeps honest players from a game that would go wrong, and is no defence against a cheat.
- The fingerprint counts an archive on a drive other than the game's, which is what stock mounts from a CD: a player with a retail CD in the drive differs from one without. Not run.
- Chat lines need a `ScreenChat` setting above zero (`0x463CA0` returns otherwise), so with it at zero Start stays off with nothing said.
- A file that fails to read hashes as its path and size, so that player differs from one whose copy reads. An archive this reader refuses counts by its name and size alone, and the log says so.
- The fingerprint is compared on Wine alone so far. No real stock peer was run: the test's joiner sends stock's 21 bytes through Impure (`tagpu_modprint_stock.on`), and an older Impure build stands for one with the key and no print.
- A loose file in the folder answers before a mutator under `none`, and the fit does not read loose files.
- The screen holds `MAX_MOD`, 15 catalog entries with the mods: past that a mutator is on no tab, though the mount honours its key. The catalog has five.
- The fit on the screen reads a base as it is now, so one whose update waits in `mods\\<id>.new` is judged on the old files until the next start; the mount checks the new ones.
- The screen says nothing about a mod another copy of the game is installing beyond what the worker's refusal says.
- A download starts again from nothing after any interruption: there is no resume.
- A jump over the entry point by another DLL has no setup that produces it, so `tagpu_modload_check`'s refusal has not
  been run.
- The 4 GB swap has run on Wine alone. A new file at the exe's path may meet Windows' first-launch compatibility hook ([the takeover](../compat/takeover.html)) at the very restart Confirm does; not run, and worth a Windows skirmish before a tag.
- A machine that stops between the swap's first two renames leaves no `TotalA.exe` and the old file as `.4gb-old`: renaming it back loses nothing, and a file check restores the store's exe instead. A second rename and its rollback both failing leaves the same state, and then Confirm and the MODS button name the file. The sentence has run on an exe a test took away; the failing rollback has not, as nothing on Wine makes the second rename fail.
- If the last rename of a swap fails, or a machine stops before it, the old file is swept at the next start, so `.pre-4gb` holds an older copy or none. Bit-off then restores that copy if it is this exe, and otherwise clears the bit in place.
- The `laa-*` rows set the bit by hand, as the managed rows do, on Wine only: `tacompat.py windows` leaves them out, so Windows has run no setup with the bit that Impure set. Only `tools/mods/modmem.py` sets it through Confirm, on Escalation.
- A mod that starts with its bit reverted says nothing until the screen is opened.
- The originals list is one install's hashes. A folder from another edition whose `totala1.hpi` matches, and whose other
  archives do not, loses those archives with only a log line.
- The mount and the loose step are not part of any recording's identity, and neither is the fingerprint.
- The shell's own orders are not compared: the map list (`Maps\*.ota`, `0x434CE5`) and the unit types at the menu, whose IDs
  only mean something in a level. The level's are compared.
- A game exe of another patch level, named `TotalA.exe`, gets the box; one renamed gets the log line and its own game.
