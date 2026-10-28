# The in-game wiki's stage with the game's left sidebar, planned

**Status (2026-10-27): decisions 1 to 12 are built** on the wiki's worktree branch, not landed.
What they became is [the in-game wiki](ingame-wiki.html), *The left sidebar on the stage*, which
is the specification now. The engine facts are the engine map's [*The left sidebar: its pages,
its buttons and who takes a
press*](exe-reverse-engineering.html#the-left-sidebar-its-pages-its-buttons-and-who-takes-a-press-disassembled-measured-2026-10-27),
where every scout fact this plan rested on was re-read. **Decisions 13 to 19 are the next
landing's**, and this note stays until that landing closes. Then it is folded into the wiki's
note and `roadmap.md` and deleted.

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
18. **The wiki follows the player's interface type** (left-click or right-click orders, the engine's
    `main+0x37EFA`), in demos and in Try it (2026-10-27). Demos name actions (select, order, cancel),
    which are mapped to the player's buttons, and captions name the button the player uses. The
    mockup gets an interface selector. Same landing as the per-mod tutorials, since both rewrite
    the demo data and its captions; the sidebar landing ships the right-click interface.
19. **Keys shown in captions and the keys strip follow the player's key bindings**, where Impure or
    the mod lets them be rebound (2026-10-27, the same landing). Game speed is not followed: the
    wiki keeps its own 1x/2x.
20. **The equivalence rule ranks by tech level, then the mod's own menu order** (2026-10-28, amending
    decision 14's alphabetical tie-break), and "of the demo's movement class" is dropped: no unit field
    tells movement class across mods. Still no mod names, lists or overrides.
21. **Decision 19 is closed**: nothing is rebindable today (TA's keys are a table in the exe, impure.cfg
    has none, the mods' key files are TADR's). Key names live in one table the captions read, so a
    future rebinding feature changes one place (2026-10-28).
22. **In a replay the tutorial follows the controlled player's side**, the one the engine's own
    sidebar follows (2026-10-28).
23. **Quick Build picks from the build menu the player sees**, not the engine's AI build list
    (CANBUILD), if the live check confirms it reads the AI list: a separate simulation landing, before
    the tutorials landing (2026-10-28).

**Settled from the engine, not asked:** the page shown follows the engine's own router (nothing
selected: the mod's `MAIN2` screen; exactly one unit selected, a builder: its numbered pages; anything
else: the side's `GEN` page). A build arms when its button is released inside it. A constructor's queued
build carries no `+N` caption (its count is 0), and the main screen's KILLS/LOSSES/TOTALUNITS labels
have no writer. The side's art (ARM or
CORE) follows the side of the player's units.

## What was built, and where it departs from the decisions

The work ran in two phases.

- **Phase 1, the mockup's sidebar**, the specification:
    - the positional .GUI reader and the picture rule in `tools/wiki/` (`tavfs.py`; `gfx.py
      --side-only`, written per installed mod by `sidepacks.py`);
    - the router, the buttons, the minimap and the build trip in `sim.js`, and the column's
      composer `side.js`;
    - the wiki's own build column retired, the maps at 40 × 30 and the demos re-timed.

    The composed column was compared with each compatibility setup's own sidebar, captured in the
    game. The rule was corrected where a setup drew otherwise, until all six matched.

- **Phase 2, the DLL**:
    - the layout's C struct and its one check (`WIKILAYOUT`, `tagpu_wikilayout_check`);
    - the sidebar in `tagpu_wikisim.c`, bit for bit with `sim.js`;
    - the running mod's sidebar read on the asset loader's thread into one immutable block, with
      the folder names a mod renames copied on the game thread;
    - the column in the stage's fit and ops and composed as `side.js` does (`tagpu_wikiside.c`,
      held to it by `sidecheck.mjs`);
    - the whole wired into the game: the demos wait for the loader's answer, `tagpu_wiki.check`
      writes the layout it played on, a demo that cannot reach its build stops under the badge, and
      Try it arms through the stage's sidebar.

**Where the build departs from the decisions as written:**

- **Decision 2's builder** is a *mobile* builder of the player's (MaxVelocity > 0, WorkerTime > 0).
- **The player's side** is taken from the wiki's first entry's units, since the stage has no
  PlayerInfo. Decision 17 replaces it.
- **The stage plays the right-click interface.** Decision 18 replaces it.
- **A press off the routed screen.** On the minimap, the column's rows outside the screen, or the
  band under a short map, a press follows the engine's rule for a press outside the GUI's window:
  a right press cancels an armed building, and an armed left press places it once at the cursor
  clamped onto the map. The engine map's *What each Interface Type does with a press* is the
  measured table.

## What the scout got wrong, corrected when the work re-read it

- **Mayhem's and ProTA's download folders.** Their loaders point it at `downloadsM` and
  `downloadP`, which neither ships, so both load no download entries; neither reads retail's.
- **Mayhem's and ProTA's build pictures.** They are not `unitpics` PCX files drawn by their DLLs.
  The pictures are in each mod's own `commongui.GAF`, under the gadget's name, which is the
  engine's second lookup, after the screen's own GAF. MEASURED from the packs `gfx.py` resolves.
  Their pages are read from the folders their loaders rename (`guiM`, `guiP`).
- **Mayhem's patch at `0x419CFE..0x419D4E`** lies in the ATTACK branch, not STOP's.
- **TA Zero's `ZBuildMenu`.** Only its `_a`/`_d` files lack `MENU` and `BUTTON`. Its `_h` files
  fill its template pages by the stock rule, which no TA Zero file patches.

## What is still not known

- Whether a mod that patches one of the format strings (`%s%d.GUI`, `%sDL`, `_gadget`) exists.
  None of the six setups does.
- The archive pattern Mayhem's and ProTA's loaders rewrite (`0x5028CC`), not read live.
- What STOP does with Shift held, and the rest of the order resolver's case for order type 1
  (`0x43F9E9`).
- The cursor over the column, which no capture compared.
