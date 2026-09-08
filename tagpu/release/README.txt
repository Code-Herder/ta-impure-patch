Total Annihilation - Impure Patch                                 build @VERSION@
https://github.com/Code-Herder/ta-impure-patch


WHAT IT IS

  A replacement ddraw.dll for the retail Total Annihilation (the 3.1 TotalA.exe
  that Steam and GOG ship). It patches the game in memory when it starts -- the
  files of your install are never modified -- and renders it through OpenGL:

    - every unit, wreck, feature, effect and the terrain drawn by the patch,
      at the screen's resolution, instead of the 1997 software renderer
    - Classic++: the game's art restored to true colour, lit, with cast shadows
    - a strategic zoom on the mouse wheel
    - 0 to N weapons per unit, for units built for it
    - the game's own UI drawn by the patch too

  Every patch checks the bytes it replaces before writing any of them, so on a
  different build of the exe it arms nothing and you get the stock game through
  the plain cnc-ddraw renderer.

  Needs OpenGL 3.3 (any GPU of the last fifteen years). Runs under Wine too;
  that is where it is developed.


INSTALL

  Copy every file of this folder next to TotalA.exe, then start the game as usual:

    ddraw.dll       the patch
    ddraw.ini       its settings; keep renderer=openglcore
    full.w32.bin    the Classic++ restorer's weights (the DLL reads them beside
    tiny.w32.bin    the exe; without them Classic++ keeps the indexed colours)

  If another ddraw.dll is already there (an older cnc-ddraw, a resolution fix),
  this one replaces it.


OPTIONS -- UNTIL THE GAME HAS A MENU FOR THEM

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

  e.g. tagpu_classicpp.off for the classic look, tagpu_zoom.off for no zoom.

  tagpu_defaults.off turns the whole list off at once. Two things stay on because
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

  No file = the defaults the art was tuned with. Cast shadows also need the
  game's own Shadows option on.


WHAT IT WRITES

  tagpu.log next to TotalA.exe: one "ARMED" line per pass at start, the options
  line ("opt: play defaults ON ..."), and timings. Attach it to a bug report.
  Nothing else is written unless you ask for a dump.


MULTIPLAYER

  Units with extra weapons change the simulation, so in multiplayer every player
  needs the patch and the same unit files. Stock units simulate exactly as before.


LICENCE

  MIT for the patch (LICENSE in the repository). Total Annihilation is the work of
  Cavedog Entertainment; this folder contains no part of the game. Built on
  cnc-ddraw (MIT, FunkyFr3sh) and Microsoft Detours (MIT).
