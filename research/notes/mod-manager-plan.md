# The mod manager, planned

**Written** 2026-11-01, after the design grill. The landings table says what is built. Nothing here
has been measured in the game except where it says so. Engine facts carry their address and how they were
established: DISASSEMBLED, MEASURED, cited from [the engine map](exe-reverse-engineering.html), or
[INFERRED]. When the work closes, this file is folded into the module notes, `roadmap.md` and
[compatibility](compat/setups.html), and then deleted.

## What it is

A **Mods** screen in the style of Factorio's: Manage, Explore and Updates tabs, a detail pane, and
Back and Confirm buttons. It opens from a **MODS** button under REPLAYS on the main menu. From it the
player downloads, updates and switches total-conversion mods and small **mutators**. All of them run
on the player's own `TotalA.exe`, the one they double-click in their TA folder. No exe is ever
hosted. The drop-in route (Impure's DLL dropped into a folder already modded by hand) keeps working
unchanged, and it is the reference the managed route is tested against.

## Decisions

Each decision is listed with what it was chosen over.

### Switching

- **Confirm restarts the same exe with `mod=` written to `impure.cfg`.** We chose this over swapping
  mods inside the running process, which the engine does not allow:
  - the mount list is append-only (unmount `0x4BE270` has no caller; the prune is `0x4BE180`);
  - Patch Loader patches, the registry root and the exe bytes are fixed at start-up.

  The compute restorer's relaunch (CreateProcessA of TotalA.exe itself) is the precedent. Steam
  losing track of the game across the relaunch is accepted.
- **Every mod runs on the player's own exe, patched in memory.** Two alternatives were rejected:
  - one full game folder per mod, rejected as clumsy;
  - handing off to the mod's own exe, accepted at first and then dropped, because a package that
    contains `TotalA.exe` cannot be hosted (the exe is Cavedog's copyright).

### Exes

- **Every mod's exe is the stock 3.1 exe plus a small byte delta.** MEASURED 2026-10-29, section by
  section against `pristine/TotalA.exe.pristine`. All of them have identical section sizes (`.text`
  0xFA92A, `.rdata` 0x468C, `.data` 0x10A00, `.tls` 0x14):

  | setup | delta from stock 3.1 |
  |---|---|
  | Total Mayhem 11.3.0, ProTA 4.8 | none: byte-identical (they patch through the Patch Loader's table) |
  | TA Twilight 2.0b98, gammata | header 4 B, `.text` 12–14 B, `.rdata` 8 B, `.data` 15–19 B |
  | TA Zero Alpha 5 | header 12 B, `.text` 823 B in 79 runs, `.rdata` 12 B, `.data` 154 B, `.tls` 403 B [INFERRED: a code cave], `.rsrc` icon 3072 → 39936 B |
  | Escalation 10.2 | header 8 B including the LAA bit (Characteristics 0x12B), `.text` 2875 B in 211 runs, `.rdata` 34 B, `.data` 95 B |

  The renamed imports (`TAESC`/`EMUSI`/`EPLAYX`, `ZDRAW`/`ZMUSI`/`ZPLAYX`, `TDRAW`/`TMUSI`/`TPLAYX`)
  belong to TADR's route and are dropped from the delta.
- **A mod's exe patch is a table derived once by us** (a diff against stock 3.1). For Mayhem and
  ProTA it is their Patch Loader table.
- **A known exe gives the stock image, and whatever other DLLs write is taken back at the entry point.**
  A folder's Patch Loader and a mod's `win32.dll` write `rel32` displacements into their own DLLs
  (`0x47BFA3`, `0x4B55FC`, the three thunks: [the Patch Loader](deep-patch-loader.html)). Those bytes
  depend on where the DLL loaded, so no fixed table can take them back, and Impure patches the same
  image during its own attach, so the order is what makes a process with a mod equal to its drop-in
  folder ([the module](modules/mods.html)):
  1. **At attach, before Impure's first patch** (`tagpu_modload_begin`): identify the exe on disk and
     any loader beside it by `file_key`, build stock + the mod's XOR from the file, hash it, and write
     the image the mod's kind sees here: the target for a mod that ships an exe, stock for one that
     ships a Patch Loader.
  2. **Last in attach** (`tagpu_modload_end`): a loader mod's bytes, Impure's mount and loose detours,
     then a snapshot of every section with the import slots in it, then a jump on the exe's entry point.
  3. **At the entry point, after every DllMain:** every byte that differs from the snapshot goes back,
     so nothing has to be listed: loader writes, thunks and hooks on import slots all go the same way.
     **MEASURED 2026-11-05:** a Mayhem folder's Patch Loader had written 704 B by then.

  A mismatch refuses at the entry point, before any game code runs: a box naming the reason, `mod=`
  cleared so the next start is the folder's own, and exit. We chose this over testing every mod's
  table against every modded exe for byte clashes.
- **An unknown exe makes the MODS button open an engine message box**, in the same style as the
  DirectX version box (the box at `0x4266A9`, gated on the check `0x4B5070` called at `0x4266A0`;
  Impure suppresses that box at `0x4266A7`). The box says mods are not available because this exe is not one Impure knows.

### Content, choices and options

- **`mods\<id>\` holds only downloaded content**, replaced wholesale on update.
- **Per-mod play options come in three layers, each with one home:**
  1. **Impure's built-in defaults**, in code.
  2. **The mod's config**, a per-mod source file `mods/<id>.json` merged into the catalog by the
     packing tool. For each option it says whether the mod **allows** it and what its **default**
     is. It replaces the hardcoded `defaults(mod)` in `tagpu_profiles.c` and the old rule files'
     keys (`manual_snapping`, `quick_build`, `default_radius`, `max_radius`, `quick_build_default`) and
     their `@<version>` overrides. The stock game and an unidentified mod are the built-in layer. A drop-in folder identified
     as a catalog mod reads the same entry.
  3. **The player's changes, in `profiles\<id>.ini`.** A missing key means "the mod's default". The
     file took in `quick_build.<mod>` and `resource_radius.<mod>` from `impure.cfg`, moved once at
     attach; old rule files are renamed `.migrated`. What did not finish runs again next launch: a key
     leaves `impure.cfg` only once its value is in the file, so the move keeps no record of its own
     (`impure-migration.txt` is the store's). The file will hold the content switches `file.<path>=on|off` and `hide.<archive>=on|off`.

  `impure.cfg` keeps only global settings. Profile names are the fixed seven, which every v1 mod's
  id is one of; a mod beyond v1 needs them from the catalog, with its row in the replays' mod table. A `file.` key naming a file the current version no longer has is ignored with a
  log line, not rejected. We rejected keeping the player's per-mod choices split between
  `impure.cfg` and `profiles\`, and keeping mod rules hardcoded in the DLL.
- **Play options are per mod; visuals are not.** Building, placement, snapping, reclaim and orders
  are per mod: Quick Build and resource snapping today, with placement spacing and the reclaim sweep
  as candidates once they are declared. Visuals, display and audio stay global in the Visuals menu.
- **The exe patch is all or nothing.** It is not a switch: the hash check only holds for the whole
  table. Turning a mod "off" means choosing another mod.

### One mod plus mutators

- **One base mod at a time, or none**, plus small **mutators** (QOL, small fixes, a unit's look).
  We chose this over Factorio-style stacking, because exe tables cannot stack and total conversions
  replace the same files. A mutator declares `base=<mod id>`; "no mod" counts as an id.
- **Mutators carry game files only, never exe patches.** Code-level QOL is Impure's own job. A
  mutator may hide base-mod files.
- **A mutator's fit is checked file by file, not against version lists.** The packing tool records
  the hash of every base file the mutator replaces. If an installed base file differs, the mutator
  is greyed out and the pane names the file. New files and hides need no check. Mutators are listed
  only under their base mod.
- **Two mutators replacing the same file are mutually exclusive,** and the pane names the file. We
  chose this over a load order where the last one wins silently.

### Catalog and packages

- **The catalog is `mods/catalog.json` in the repo.** Each entry has:
  - `id`, `kind` (`mod` or `mutator`), `name`, `author`, `version`, `summary`, `homepage`;
  - `packages`: an ordered list of `{url, sha256, size}`;
  - mods: `exe` (the name of its table in the patch pack) and `hide` (the TA-folder archives hidden
    by default), plus the play options;
  - mutators: `base`, `base_hashes` and `files`;
  - `permission`: where the author's permission to host is recorded. No entry ships without one.
- **A binary patch pack goes beside the catalog**, zstd-compressed. It holds one XOR with the stock
  image per mod, with the stock+mod hash it must produce, and the files Impure knows by hash: a known
  modded exe names the XOR that takes it to stock, and a known loader names none. On stock an XOR gives
  the mod's image and on its exe it gives stock, so the pack holds neither image. A byte a mod sets to
  zero reads as the stock byte: 824 B across the pack, 15 B at the longest (`modpack.py verify` counts them).
- **Both are built by `tools/modpack/` and packed into the DLL** as a `MODPAK` RCDATA, like `HUDPAK`
  and `WIKIPAK` in `res.rc`. The JSON reader `tagpu_json.c` reads them.
- **Exe patches only ever ship inside the DLL.** In v1, updates arrive with a new Impure release.
  A later website serves a signed `catalog.json`.
- **One package format, `.tar.zst`.** The packing tool rebuilds every mod's original package, so
  the DLL never reads `.rar` or `.zip`: there is no unRAR licence and no miniz. While rebuilding,
  the tool:
  - packs all files that win in the drop-in folder, loose ones included, into the package's own
    archive `<id>.hpi`, except those the managed mount already answers from the player's archives
    with the same bytes;
  - leaves out every file whose hash is in the original-game manifest, and writes those it still
    needs into the recipe below;
  - drops mod DLLs and keeps the plain files Impure reads itself (`Icon/`, `ZIcon/`).
- **A recipe rebuilds what cannot be hosted.** MEASURED by `modpack.py packages`: every mod's archive
  carries byte-identical copies of original files under its own folders or at another path
  (`guim/armbuild.gui` is `guis/armbuild.gui`), or an older original version of a path: 356 in
  Escalation, 329 in Mayhem, 426 in ProTA, 83 in TA Zero, 393 in Twilight. `<id>.originals.tsv` names
  each one's path, MD5, size, where it is in the player's own archives and SHA-256, and the install copies them
  into `mods\<id>\<id>.originals.hpi`, mounted right after `<id>.hpi`. We chose this over keeping a
  hidden original archive mounted, because those files are at other paths and the mod's first-wins
  order differs from the original's.
- **`hide` is measured, not declared.** It is every original archive the drop-in folder lost: replaced
  by a file of the same name, or no longer named by a mount pattern because the exe table renames the
  first one (`rev31.gp3` in all five, plus the seven more Twilight replaces with empty files or its own).
- **The catalog's URLs are relative** (`<id>/<id>-<version>.tar.zst`), and resolve against a top-level `base_url`, absent until
  there is a CDN to name; a download then reports that the catalog names no place to fetch from.

### Download and install

- **WinHTTP on a background thread that touches only files.** The Mods screen reads its progress
  from a few atomic counters. WinHTTP is loaded when a download runs, so the DLL gains no import.
- **Nothing unpacks from an unverified file.** The download goes to `mods\<id>.part` and is checked
  against the catalog's size and SHA-256 (the module's own `modsha`, already in the DLL, rather than BCrypt)
  before anything is unpacked.
- **The tar reader is bounded to the package's declared file list.** It refuses absolute paths,
  `..`, undeclared files and truncation, and takes regular files only.
- **The recipe runs after the unpack**, from the player's original archives into
  `<id>.originals.hpi`, each file checked by SHA-256. A folder without an original archive it names cannot install
  that mod, and the reason says which archive is missing.
- **Unpacking goes into `mods\<id>.new\`**, which gets a done marker, `installed.txt` holding the version, when complete. It
  is swapped in at start-up, before any archive is mounted. A `.new` without the marker is deleted. The live
  folder is always either the old version or the new one, and a lock file that dies with its process keeps a second copy of the
  game out of a running install.
- **A failed install says why, in words for the player**, and the Mods screen shows that text.

### Updates

- **Updates are manual only:** an Update button per row and an Update all. There is no background
  check.
- **One version of each mod is installed.** The player's profile survives the update.
- **Mutators broken by a base update** are listed as "needs an update for X".
- **Old replays keep today's hint**, not a refusal (`tagpu_replays.c`, `mod_lines`: "The installed
  archives differ."). Replay records gain the mutator list, and the hint names the exact versions.

### Archives and loose files

These are the engine rules the design rests on:
- **Opening a file tries the loose file first.** `0x4BB2E0(path, mode)` tries the loose path
  relative to the current directory first. It then walks the archives in mount order, and the first
  archive that has the file wins. DISASSEMBLED 2026-10-26
  ([3D stage plan](ingame-wiki-3d-stage-plan.html)).
- **`InitTAHPIAry 0x41D4C0` mounts in a fixed order:** `rev31.gp3`, `*.ccx`, `*.ufo`, `*.hpi`. The
  mount list is `*(0x51FBD0)+0x618`, with its count at `+0x61C`. That order is why a `.ufo` cannot
  override a path a stock archive has (MEASURED, [file formats](file-formats.html) §5).
- **Loose files behave differently by type.** A loose `units\*.fbi` makes its unit vanish (the
  "came from an archive" test `0x4BB650` in `0x42A8D0`). A loose `.cob` overrides.
- **Enumerations list loose files first.** That covers the unit definitions (`0x4BCA30`) and the
  feature TDFs.

What follows from those rules:
- **Impure builds the mount list:** mutators first, then the base mod's `<id>.hpi` and
  `<id>.originals.hpi`, then the TA folder's original archives in the engine's order, less the mod's
  `hide`. Precedence is then one rule for every file type. `modpack.py verify` simulates this and the
  drop-in folder over every path by the engine's rules (`tools/wiki/tavfs.py`), over what the retail
  archives really hold, and they agree in bytes and in each file's case: 8293 paths in Escalation, 7539 in
  Mayhem, 6064 in ProTA, 8551 in TA Zero, 8740 in Twilight.
- **Where a file's place decides, the package keeps the drop-in's answer.** A feature section and a texture sequence come from
  the first file listed that holds them, and one archive cannot keep the folder's order. A copy that would come first is renamed
  where it stands in the mod's file, and an original file that must come first goes in the recipe, so no original text is
  shipped ([the module](modules/mods.html), *A file's place*). `modpack.py verify` checks them, the weapon IDs and the build-menu
  cells; the managed column compares the result in the game.
- **Under a managed mod, the engine's loose-file step is closed** for both opens and enumerations.
  It is closed by folder: a read whose first path component is one of the folders modpack treats as
  game data (`GAME_DIRS`) or a name the image gives one finds no loose file, so saved games and everything else stay open. Retail's
  own loose files in those folders are four examples (`camps\\EXAMPLE.TDF`, its brief, its picture),
  and no run named one. The log names each loose file refused. The drop-in route and "no mod" keep
  the loose step.
- **Files read by plain path** rather than through `0x4BB2E0` are listed in each package's
  `listing.txt`. The v1 packages keep the strategic icon sets Impure reads (`Icon/`, `ZIcon/`) and drop
  the rest: changelogs and readmes, cnc-ddraw's `ddraw.ini` and `Shaders/`, `wgmus.ini`, Twilight's
  `aqrit.cfg`, `ChatMacro.ini` and `Settings.ini`.
- **Each mod's options file is kept** (`mayhem.ini`, `ProTA.ini`, `TAESC.ini`, `TAZero.ini`, `TA.ini`).
  The engine reads its `[Preferences]` beside the exe (`0x49F5F9`) and the strategic icons read their
  folder from it. It is copied there at start-up when the folder has none, and is the player's from
  then on.

### Drop-in folders

- **The folder's own setup is a fixed first row**, "This folder (Mayhem 11.3)". It is identified as
  the replays and profiles identify it today, and it runs when `mod=` is empty.
- **Under a managed mod, the TA folder contributes only the original game's archives.** They are
  recognised by the SHA-256 of the ~30 original archives, which the DLL carries as hashes only, never
  content. The owner accepted this as a new class of published data. Each archive is hashed once and
  cached by name, size and date. We rejected recognising archives by name, because mods reuse and
  replace archive names.
- **A folder's Patch Loader follows the exe rule above:** beside a known exe, a known loader's writes
  go when the process is restored, and an unknown one gets the box.

### Multiplayer

- **What stock TA does:** its only check is the per-unit CRC handshake (`0x1A UNIT_DATA`,
  `CRC_all`/`CRC_weapons`). On a mismatch it **disables the affected unit types on both peers and
  starts anyway, with no message** (MEASURED 2026-09-02, [networking](networking-lobbies.html)).
- **Impure players compare a content fingerprint.** It combines:
  - the stock+mod exe hash;
  - the hash of every mounted file, except known-cosmetic types (`.gaf`, `.pcx`, `.bmp`, `.wav`,
    fonts, menu `.gui`). Unknown types count.

  That lets drop-in and managed players of the same mod match, and a cosmetic mutator mixes.
- **A mismatch between Impure players blocks Start**, with a line naming who differs and how.
- **A player without Impure sends no fingerprint and passes as today.**

### The screen

- **A custom Impure UI, like the in-game wiki:** full colour, scaled to the player's resolution and
  aspect ratio in wiki units (screen height / 720). The owner chose this over an engine `MODS.GUI`
  built the way REPLAYS is.
- **The widgets are the kit's, shared with the wiki:** `tagpu_uikit.c` (text wrap, scroll panels,
  rows, buttons, badges, art parts, solids and the hit map) and, for the mockup, `tools/uikit/`.
- **The Mods screen is built mockup-first on that kit:** `tools/mods/mockup/`,
  `art/mods/layout.json`, a pure `tagpu_modslay.c` and a `modslaycheck`.
- **The art is the wiki's kit**, plus new parts: checkbox/radio, progress bar, three tabs, and the
  badges for active, update, greyed and conflict.
- **The screen draws on the wiki's pass, which stands at the menu too;** only the wiki's own record is tied to a level. Its page and hit map cross under
  one lock rather than in a packet ([the module](modules/mods.html), *The screen*).
- **Input is modal on the main menu** and reuses the wiki's paired press/release bookkeeping.
- **It is Vulkan only.** On GDI, the MODS button opens an engine message box: the screen needs
  Vulkan, here is the running mod, and `mod=` in `impure.cfg` can be edited by hand. Choosing and
  loading a mod is start-up code and works on every renderer.
- **The MODS button goes under REPLAYS**, through the same main-menu insertion REPLAYS uses
  (`tagpu_replays.c`: S1 `0x4AACAE`, S2 `0x42644C`, S3 `0x429290`, the builder `0x4263B0`). MEASURED on every v1 menu: it needs no moves of its
  own, only the taller plate.
- **`file.<path>` switches wait for mutators.** A v1 mod's content is one archive with thousands of paths, so before there are mutators the key has no file a
  player could sensibly turn off. `hide.<archive>` is what this screen switches.

### 4 GB (LAA)

- **The bit has to be in the exe header before Windows loads the process.** It cannot be set at run
  time.
- **The catalog flags the mods that need it**, e.g. `memory=4gb` on Escalation.
- **Confirming a flagged mod on an exe without the bit:**
  1. renames `TotalA.exe` to `TotalA.exe.pre-4gb` (Windows allows renaming a running exe);
  2. writes a copy with LAA set and the header checksum fixed;
  3. restarts.
- **Exe identification ignores that bit.**
- **The bit stays on afterwards.** Every setup must pass an LAA-on compat row. Until a setup does,
  its entry is marked bit-off and Confirm restores the `.pre-4gb` copy for it.
- **If Steam's file verification reverts the exe,** the next Confirm of a flagged mod sets the bit
  again.
- **There is no global switch in v1.**

### The v1 catalog

- **Five mods:** Total Mayhem, ProTA, Escalation, TA Zero and TA Twilight. The owner has permission
  from all their authors, and each entry records it in `permission`.
- **Not included:** the official expansions (Core Contingency and Battle Tactics are part of "no
  mod") and gammata (a DLL patch).
- **No mutator ships until a real one exists.**
- **Managed Twilight drops its `dsound.dll` proxy.** That proxy writes a jmp at `0x42DD79`, right
  after the download-menu allocation in `0x42DCF0` [INFERRED: Twilight's fix for the same
  download-record overflow Impure's `fix_download_records` fixes]. The managed column has to show
  Impure's fix covers it.

## The landings

| # | landing | what it does | verified by (run it) |
|---|---|---|---|
| 1 | Shared UI kit (built) | Moves the wiki's layout primitives into `tagpu_uikit.c` + `tools/uikit/`, with no behaviour change | `wikilaycheck` passes unchanged; `tools/wiki_regression.py` page/demos pass |
| 2 | modpack + catalog (built) | `tools/modpack/`: strips by manifest hash, packs all files into the mod's archive, writes the originals recipe, builds `.tar.zst`, derives one XOR per mod + the known files, writes `mods/catalog.json` + the patch pack, embeds `MODPAK`; catalog schema check in `make` | offline: each table reproduces its mod's exe from stock (minus TADR import renames); each known exe's XOR gives stock; no package file matches the original manifest; the schema check fails on a missing sha256 / base / permission |
| 3 | Per-mod options (built) | `mods/<id>.json` carries allowed + default per play option, replacing `defaults(mod)` and the rules in `profiles\<mod>.ini`; `profiles\<id>.ini` becomes the player's changes only; one-time migration out of `impure.cfg`; a forbidden option cannot be set | drop-in runs of every v1 setup and retail behave as today; a migrated `impure.cfg` + old profiles give the same choices with no per-mod key left in `impure.cfg`; a hand-edited forbidden option is refused with a log line |
| 4 | Managed start-up (built) | `mod=`; identify and hash at attach, then the image, Impure's patches, and the rest taken back at the entry point (refusal box); the mount list (mutators, base, its originals archive, originals by shipped SHA-256 + cache, less `hide`); the loose step closed under a managed mod | compat suite **managed column**: each v1 mod, installed by hand into a clean stock folder, equals its drop-in run (unit types and keys at the menu; type IDs, weapon and feature tables in a level; every type's CRC with a drop-in peer in one network game; menu / fight / two-player game); the base-exe × mod matrix |
| 5 | Download + install | WinHTTP thread, `.part` + sha256, bounded tar reader, the originals recipe, `.new` + done marker + start-up swap | against a local HTTP server in the harness, never the real CDN: a good package installs, a corrupt one is refused, an interrupted download or unpack leaves the live folder intact and the half `.new` is deleted at the next start, an update swaps at restart and keeps the profile; the tar reader refuses `..`, absolute paths, undeclared files and truncation |
| 6 | Mods screen (built) | The shell-time Vulkan pass; the MODS button; Manage / Explore / Updates; the detail pane with play options (marker, reset, locked rows) and hide switches; the unknown-exe and GDI boxes; Confirm's relaunch | `modslaycheck` against the mockup; a `tacli` UI regression clicks tabs, switches, Update and Confirm and sees the game restart on the chosen mod; the button and screen checked over each v1 mod's menu art |
| 7 | Mutators | Layering, `base_hashes` greying, mutual exclusion, Updates' "needs an update", the `file.` switches, the mutator list in replay records and the hint | offline: conflict and stale-base cases in modpack's tests; in game: a test mutator overrides a unit's look over its base, and a stale one is greyed with the file named |
| 8 | Multiplayer fingerprint | The content fingerprint exchanged in the battle room; Start blocked between differing Impure players | MP runs: managed and drop-in peers of the same mod start; a gameplay mutator on one side blocks Start; a cosmetic one does not; a peer without Impure passes as today |
| 9 | 4 GB (LAA) | The `memory=4gb` flag; Confirm's rename + LAA copy + restart; restore for bit-off entries | an LAA-on compat row for every v1 setup; managed Escalation starts with LAA set; a reverted exe is set again on the next Confirm |

Landings 2 and 3 are independent of each other.

## Not closed

**Needs the owner:**
- Where each author's permission is recorded. The `permission` field says today only that the owner
  reports having it, dated.
- Which CDN, and what URL scheme, will the packages live at? Needed before landing 5 points at real
  packages.
- The art of the screen's thirteen new parts. `tools/mods/genparts.py` derives plain ones from the kit, and a painted part of the same name replaces one with no code change.

**Needs research or a measurement:**
- What channel carries the fingerprint in the battle room? No Impure message exists there today, so
  this needs research before landing 8.
- Which v1 setups are safe with LAA on? Landing 9's row answers it. Managed Escalation already needs it
  to load a game: its 550 unit types run out of address space without it (MEASURED 2026-11-05, the
  engine's out-of-memory box), so its compatibility rows set the bit by hand until landing 9.
- A jump on the entry point by another DLL, written after Impure's, keeps the entry stage from running.
  The first DirectDraw call refuses that case, and no setup in the suite produces it, so that refusal
  has not been run.
- Managed start-up has run on Wine only, and the suite's Windows path has no install: it leaves the managed rows out.
- The originals are one install's hashes. Another edition with a different `totala1.hpi` gets the box; one that shares it and
  differs elsewhere loses those archives with only a log line. More editions' hashes need the owner.

**Later, outside v1:**
- the website, with a signed catalog and its coexistence with the DLL-packed one;
- an HD menu-graphics mutator, the owner's idea;
- drop-in Twilight's `dsound.dll` proxy on Windows, which stays a compat blind spot (its jmp target
  may be a heap stub that the "leads into a folder DLL" check cannot see). Managed Twilight avoids
  it.

## Facts gathered for the plan

- **Exe tables, as derived.** Escalation 2993 B in 253 runs, Mayhem 1549 B in 85, ProTA 886 B in 66,
  TA Zero 1385 B in 144, Twilight 34 B in 16. Seven known files: the four modded exes and the three loaders.
- **What a Patch Loader writes, MEASURED** by `modpack.py loaderdump` (a process with the exe's entry
  point spinning, read through `/proc`): see [the Patch Loader](deep-patch-loader.html). A table is its
  list and the proxy byte; the DirectDraw call sites and the `win32.dll` thunks stay out of it, and
  restoring the process takes them back.
- **Package sizes:** Escalation 247.7 MB, TA Zero 89.6 MB, Twilight 76.0 MB, Mayhem 18.4 MB, ProTA 6.3 MB.

- **Where the mods come from.** TA Forever's API (`https://api.taforever.com/data/featuredMod`,
  field `installPackage`) lists ordered package URLs and "folders" regex rules for each featured
  mod. Its files sit on Vultr Object Storage (`sjc1.vultrobjects.com`, which exposes
  `x-amz-meta-sha256`) and on `content.taforever.com`. Neither has a CDN in front.
- **The TAUniverse mod sites** (Mayhem, ProTA, Escalation) answer every direct file URL with a
  Cloudflare JavaScript challenge (HTTP 403), so a plain download cannot fetch them.
- **`win32.dll`** is the Steam/GOG CD-audio-to-MP3 shim, and it is already in the player's folder.
  Mayhem's copy equals `tmusi.dll` (BASS). ProTA's is a 66 048 B build using `audiere.dll`.
