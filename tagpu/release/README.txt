Total Annihilation - Impure Patch                                 build @VERSION@
https://github.com/Code-Herder/ta-impure-patch


WHAT IT IS

  A replacement ddraw.dll for the retail Total Annihilation (the 3.1 TotalA.exe
  that Steam and GOG ship). It patches the game in memory when it starts -- the
  files of your install are never modified -- and renders it through Vulkan:

    - every unit, wreck, feature, effect and the terrain drawn by the patch,
      at the screen's resolution, instead of the 1997 software renderer
    - Classic++: the game's art restored to true colour, lit, with cast shadows
    - a strategic zoom on the mouse wheel
    - 0 to N weapons per unit, for units built for it
    - the game's own UI drawn by the patch too

  Every patch checks the bytes it replaces before writing any of them. It needs
  the original 3.1 TotalA.exe: on any other build (3.9.02, TA: Escalation and
  other community patches ship a modified exe) it says why in a message box and
  closes the game, rather than let it play by different rules from other players.

  Needs a GPU and driver with Vulkan (1.0 or later). Runs under Wine too;
  that is where it is developed.


INSTALL

  Copy every file of this folder next to TotalA.exe, then start the game as usual:

    ddraw.dll       the patch
    full.w32.bin    the Classic++ restorer's weights (the DLL reads them beside
    tiny.w32.bin    the exe; without them Classic++ keeps the indexed colours)

  If another ddraw.dll is already there (an older cnc-ddraw, a resolution fix),
  this one replaces it. The patch reads no ddraw.ini: one left there by an
  earlier version, or by cnc-ddraw, is ignored and can be deleted.


OPTIONS -- UNTIL THE GAME HAS A MENU FOR THEM

  THE SETTINGS are the in-game menu's: the cog at the top right of the screen
  in a game, and Options > Visuals in the main menu. They are saved in
  impure.cfg next to TotalA.exe and start at the Classic++ look, on the most
  powerful GPU (the GPU row's Auto), capped at the monitor's refresh rate, and
  fullscreen on the primary monitor. If you ran an earlier version, the first
  start of this one renames the files its menu wrote to *.migrated, lists what
  it did in impure-migration.txt, and starts from the defaults. Delete impure.cfg to go
  back to the defaults later; that does not repeat the migration.

  Everything is ON by default. A pass is turned off by an empty file next to
  TotalA.exe named tagpu_<pass>.off, and back on by deleting that file. The
  passes:

    native      the units and 3D wrecks           (turning it off turns owndraw off)
    terr        the terrain and the fog
    feat        trees, rocks, wreckage
    fx / sfx    weapon fire and explosions / the particle layers
    mark        health bars, cursor, band box, group digits
    order       the shift-held order overlay
    zoom        the mouse-wheel zoom              (turning it off turns vpwide off)
    gui         the UI layer
    classicpp   the restored true-colour art, its lighting and shadows;
                off = the original palette look, still drawn by the patch
    weapons     the extra weapon slots

  e.g. tagpu_zoom.off for no zoom. (The classic look is the menu's Renderer row;
  tagpu_classicpp.off also works, and holds that row until it is deleted.)

  tagpu_defaults.off turns the whole list off at once, and ignores impure.cfg
  but for the display mode, vsync and the window's position: the menu's
  rows are greyed while it is there, except Shadows, which is then
  the game's own shadow option, kept with the game's other options as before. Two things stay on because
  they are fixes rather than modes, each with its own switch:

    tagpu_reclaim.off   a crash fix: the engine's model frees are deferred so the
                        render thread never reads a freed unit or wreck
    tagpu_curs.off      contextual cursors at any Interface Type, and the left
                        click that goes with them

  With all three files present the game is the stock one through cnc-ddraw.
  The same names with .on instead of .off are what the patch's own tooling writes;
  a .on file present wins over both the default and a .off.

  Lighting and shadow knobs go in tagpu_classicpp.cfg, key=value tokens on one
  line or several, re-read live while the game runs:

    sun=AZ,EL  unitsun=AZ,EL  amb=A  shadows=0|1  shadowsun=AZ,EL  penumbra=K
    shadowlen=A,B|off  shade=S  terrainshadow=0|1  shadowres=N
    airshadow=len|physical|drop

  No file = the defaults the art was tuned with. The menu's Shadows row switches
  the game's own shadows too.


WHAT IT WRITES

  impure.cfg next to TotalA.exe: the menu's settings, one key=value per line --
  the gamma and the screen size included, which the game still also saves in
  its registry keys as before.
  On the first start only, impure-migration.txt and the *.migrated backups.

  log\tagpu.log in the game folder: one "ARMED" line per pass at start, the
  options line ("opt: play defaults ON ..."), the settings line ("settings:
  impure.cfg: ..."), and timings. Attach it to a bug report. Each launch starts
  a new file and keeps the previous ones as tagpu.1.log, tagpu.2.log and so on;
  the logs never take more than 128 MB. Nothing else is written unless you ask
  for a dump.


MULTIPLAYER

  The patch changes the simulation: it raises the game's limits (up to 1500 units
  a player), fixes defects of the stock engine, and gives units built for it
  extra weapons. In multiplayer every player needs this same version of the
  patch and the same unit files.


LICENCE

  MIT for the patch (LICENSE in the repository). Total Annihilation is the work of
  Cavedog Entertainment; this folder contains no part of the game. Built on
  cnc-ddraw (MIT, FunkyFr3sh) and Microsoft Detours (MIT).
