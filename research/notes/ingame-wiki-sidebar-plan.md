# The in-game wiki's stage with the game's left sidebar, planned

**Written** 2026-10-27, before any of it is built. The specification is still
[the in-game wiki](ingame-wiki.html) and its mockup, which stays the 1:1 spec of the DLL wiki: every
decision below lands in the mockup first. Engine facts carry their address and how they were
established; the ones marked *scout* come from a read-only disassembly and archive census
(2026-10-27) and are re-verified by the landing that first relies on them. Like the 3D stage's
plan, this file is folded into the wiki's note, `gpu-status.md` and `roadmap.md` when the work
closes, and then deleted.

## The owner's decisions (2026-10-27)

1. **The sidebar is inside the stage, at the map's scale.** The stage is a slice of the game's
   screen: the running mod's left sidebar, then the map, one game pixel each. A demo that shows the
   sidebar gets a wider stage; the map's framing is unchanged.
2. **The wiki's own build column is retired.** The mod's sidebar is the only build menu in the
   wiki. The sidebar is a per-demo flag, on by default when the demo's scene has a constructor of
   the player's, off otherwise. **Try it always shows it when the player has a builder on the
   stage**, whatever the demo it started from did.
3. **A click on a build button shows the game's pressed frame.** In a demo it is held about half a
   second so it reads; in Try it it is the game's behaviour, pressed while the mouse button is down.
4. **A demo names the unit, never a position.** "Press ARMMEX's build button" is resolved against
   the running mod's pages for the selected builder; a button on another page is reached through
   PREV/NEXT, each pressed the same way. A builder that cannot build the unit in this mod shows the
   entry's "Turned off in this mod" state instead of clicking anything.
5. **One picture rule for every mod, never a list of mod names.** For each button, the first of:
   the page's GAF frame (the stock path); the unit's download picture; `unitpics\<UNIT>.PCX`; a
   labelled button. Before landing, each compat setup's sidebar is captured in game with a
   construction kbot selected and compared with the wiki's; the rule is corrected if a mod draws
   something else. TA Zero's build menu (filled from its own `ZBuildMenu` data) is researched on
   its own; if no general rule reads it, it goes back to the owner rather than becoming a special
   case.
6. **The minimap comes with it.** The whole sidebar: the 128 x 128 minimap (the demo's map, unit
   dots in team colours, the camera rectangle), then the page. Live in Try it, not clickable.
7. **In Try it, these buttons work**: build buttons, PREV/NEXT, the ORDERS/BUILD tabs, and STOP.
   Every other order button is drawn with the game's greyed frame. An entry that needs an order
   adds it to the simulation, and its button comes alive then.
8. **HUD scale is ignored.** The stage keeps TA's native proportions; the whole stage is already
   enlarged to fit the wiki window.
9. **The stage maps become 40 x 30 cells** (480 rows), so stock TA's sidebar fits exactly beside
   the map. A 768-row sidebar (Escalation, Mayhem, ProTA, TA Zero) leaves a band under the map,
   letterboxed in the stage's background colour, the map top-aligned as the game's view is.
10. **A `build` input with the sidebar shown plays out**: the cursor glides to the button (through
    the pagers when needed), presses it, and returns toward the map. Every existing demo is
    re-timed once by hand to make room; the data's ticks stay exactly what plays, in the mockup and
    in the game. Demos without the sidebar keep today's instant arming.

11. **Build buttons the entry does not define are greyed** (2026-10-27, the same rule as decision 7):
    a later entry that defines more units brings their buttons alive.
12. **A demo whose build has no equivalent in the mod stops at that tick with the entry's badge.**
    It stops only when the mod truly has no equivalent (decision 13).

## Per-mod tutorials (the owner's decisions, 2026-10-27; the landing after the sidebar)

13. **Each mod gets a tutorial that works in it**, built from the mod's own equivalents of the stock
    units the entry uses (its construction kbot, its metal extractor, ...), wherever the feature is on.
14. **Equivalents come from a general rule over each unit's own data**, never a list of mod names
    and no override files: for example the mex is the structure the builder can build that extracts
    metal, on the player's side, the lowest tech level; the builder a mobile unit of that side that
    builds it, of the demo's movement class. Two equal candidates resolve to the alphabetically
    first unit name. The in-game check per mod shows what the rule picked.
15. **The mod's own figures**: footprint, extraction, build time, speed and build distance come from
    the mod's unit files, so the demo shows what really happens in that mod.
16. **Captions and the entry's text name roles** (`{builder}`, `{mex}`), filled in with the unit's
    in-game display name in the running mod, never its internal name.
17. **The tutorial uses the player's own side in the current game**; the mockup gets a side
    selector beside its mod selector.

**Settled from the engine, not asked:** the page shown follows the engine's own router (nothing
selected: the mod's `MAIN2` screen; exactly one unit selected, a builder: its numbered pages; anything
else: the side's `GEN` page). A build arms when its button is released inside it. A constructor's queued
build carries no `+N` caption (its count is 0), and the main screen's KILLS/LOSSES/TOTALUNITS labels
have no writer. The side's art (ARM or
CORE) follows the side of the player's units.

## The engine facts this rests on (scout, to re-verify)

- **The sidebar, top to bottom**: the minimap at (0,0), 128 x 128; the side's `PANELSIDE` art in
  one blit (`0x467D70`), from `gamedata\sidedata.tdf` `[SIDE%d] intgaf` (`0x429E9E..0x429FE1`); the
  top GUI screen at (0,128). Nothing in the exe scales the panel; the world viewport is fixed at
  left 128, top 32 (`0x4981C9..0x498237`).
- **The page router** `0x41B2E0` (one caller, `0x4969B4`): a single builder with pages opens
  `"%s%d.GUI"` through `0x41ACE0` (one call, `0x41B83A`); any other selection `%sGEN.GUI` through
  `0x41B0F0`; nothing selected `<prefix>MAIN2.GUI` (`0x497BA4`).
- **Build pages are shipped files**, one set per builder; the engine never generates one from
  `CANBUILD`. Page count: the def load probes `guis\<NAME>0.GUI`, `1`, ... into `def+0x22E`
  (`0x42D800..0x42D8CB`); download units raise it to their `MENU` (`0x42DF60..0x42DFB4`) and fill
  gadget `BUTTON+4` of page `MENU-1`, or `%sDL.GUI` when the page file is missing (`0x41ACE0`).
- **The .GUI sections are read by position** (`0x4AEAC0` through `0x4C3490(i)`), and retail pages
  repeat section names (`GADGET2..6` in `ARMCK1.GUI`): **`tagpu_tdf`, which merges sections of one
  name, cannot read them as written**. The wiki needs a positional reader.
- **Pictures**: the panel's `panel=` in `anims\<screen>.GAF`, then commongui, then `BackTile`
  (`0x4A85C9..0x4A8658`); a button's in `anims\<PAGE>.GAF` by gadget name, then commongui
  (`0x4A8BA1..0x4A8BF7`); download slots `anims\<NAME>_gadget.GAF` (`0x4A83E2..0x4A84D8`).
  `unitpics\*.PCX` is not the stock build-menu picture (its string `0x505FE4` has two users,
  RESTRICT2 and UNITINFOx).
- **Frames** (`GUI_ButtonDraw 0x4A5F40`, `0x4A6071..0x4A616D`): 0 normal, 1 pressed, 2 greyed.
  The engine never marks the armed build type on the menu (`0x41AA00` sets `main+0x2CC3 = 0x0E`,
  `main+0x2CC4 = type` and plays `addbuild`, `0x41AB89..0x41ABA3`).
- **Paging**: PREV/NEXT/ORDERS/BUILD by substring into `main+0x37EBE`/`0x37EBF`; `0x41C180` (from
  `0x4992CD`) calls NEXT `0x41BDE0` or PREV `0x41BF10`; the page is `unit+0x110` bits 23..25 (bit
  22 = has a page), wraps at `def+0x22E - 1`, starts at 1 (`0x485CD5`); a page swap reloads the
  screen file.
- **Sizes by setup** (measured from the archives): retail pages 128 x 352, 2 x 3 grid with tabs;
  Escalation, Mayhem, ProTA 128 x 640, 2 x 6; TA Zero 128 x 640 templates and 128 x 768 panel art;
  `MAIN2` 128 x 352 everywhere. Mayhem and ProTA ship empty page GAFs (their pictures come from
  somewhere else, [INFERRED] their DLLs).

## The landings (a first cut; re-cut as the work learns)

1. **The mockup's sidebar**, the spec: a positional .GUI reader and the picture rule in
   `tools/wiki/` (`tavfs.py`, `gfx.py` packs per installed mod), the page router, the minimap, the
   pressed/greyed frames, the `build` input's trip, the column retired, the maps at 40 x 30 and the
   demos re-timed, with `simparity`, `stagecheck` and `wikilaycheck` kept equal. The owner reviews
   it in the browser before the DLL copies it.
2. **The DLL reads the sidebar**: the positional reader and the picture rule in the wiki's asset
   loader, into the immutable arena; the folder names a mod renames (gui, gamedata, unitpics,
   download) added beside the existing ones.
3. **The DLL draws it**: the stage layout widened, the sidebar's ops, the minimap, the demo trip
   and the pressed frame; the hit map takes the sidebar's buttons in Try it.
4. **Every mod checked in game**: the compat setups' live sidebars captured and compared; TA Zero's
   research and its answer.

## What this plan does not know yet

- Where Mayhem's and ProTA's build pictures and their GUI folders come from at run time.
- How TA Zero places its `ZBuildMenu` units.
- Whether the engine keeps duplicate-named .GUI sections in practice (the file order says so; not
  measured live).
- Which sidedata key fills the side prefix at `main+0x37F5B` (the file names suggest `nameprefix`).
- What `0x491D70` does ([INFERRED] the pop back to `MAIN2`).
- The pager rows' and orders' exact positions per mod, beyond the census parser (33 ProTA .GUI
  files failed it; the engine's own reader was not run on them).
