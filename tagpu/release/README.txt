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
    - every game recorded, with a Replays browser, replay controls, taking
      over a replay as any player, and rewinding a skirmish
    - building helpers: lines, rectangles and rings of buildings in one drag,
      metal extractors that snap to the best spot, Quick Build on metal spots
      and vents, and a reclaim sweep
    - an in-game wiki with interactive tutorials for the building helpers

  Every patch checks the bytes it replaces before writing any of them. It runs
  on the original 3.1 TotalA.exe and beside the community's builds and mods:
  the 3.9.02 exe, TADR, the Community Patch Loader, Total Mayhem, ProTA, TA Zero,
  TA Twilight and TA: Escalation. For a mod, copy it into the mod's folder and
  leave the mod's files as they are, TADR's included. TADR's own code does not
  run while the patch is installed: no TADR demo recording (the patch records
  games itself, see REPLAYS), no TADR mega map or overlays.
  gammata's drop-in package is not supported yet.

  Needs a GPU and driver with Vulkan (1.0 or later). Runs under Wine too;
  that is where it is developed.


INSTALL

  Copy every file of this folder next to TotalA.exe, then start the game as usual:

    ddraw.dll       the patch
    OFL-*.txt       the licences of the fonts inside ddraw.dll (see LICENCE)

  If another ddraw.dll is already there (an older cnc-ddraw, a resolution fix),
  this one replaces it. The patch reads no ddraw.ini: one left there by an
  earlier version, or by cnc-ddraw, is ignored and can be deleted. The same goes
  for full.w32.bin and tiny.w32.bin from an earlier version: the restorer's
  models are inside ddraw.dll now.


OPTIONS -- UNTIL THE GAME HAS A MENU FOR THEM

  THE SETTINGS are the in-game menu's: the cog at the top right of the screen
  in a game, and Options > Visuals in the main menu. They are saved in
  impure.cfg next to TotalA.exe and start at the Classic++ look, on the most
  powerful GPU (the GPU row's Auto), capped at the monitor's refresh rate, and
  fullscreen on the primary monitor. If you ran an earlier version, the first
  start of this one renames the files its menu wrote to *.migrated, lists what
  it did in impure-migration.txt, and starts from the defaults. Delete impure.cfg to go
  back to the defaults later; that does not repeat the migration.

  The Visuals rows "Resource snapping" and "Quick mex/geo" are kept per mod.
  They start on in stock TA, Total Mayhem, ProTA and TA Twilight, and locked
  off in TA: Escalation, TA Zero and any other mod. The per-mod rules are the
  files in profiles\, which the patch creates when they are missing.

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
    recorder    recording every game into replays\

  e.g. tagpu_zoom.off for no zoom. (The classic look is the menu's Renderer row;
  tagpu_classicpp.off also works, and holds that row until it is deleted.)

  tagpu_defaults.off turns the whole list off at once, and ignores impure.cfg
  but for the display mode, vsync and the window's position: the menu's
  rows are greyed while it is there, except Shadows, which is then
  the game's own shadow option, kept with the game's other options as before.
  It also stops recording and turns resource snapping and Quick Build off. Two
  things stay on because they are fixes rather than modes, each with its own
  switch:

    tagpu_reclaim.off   a crash fix: the engine's model frees are deferred so the
                        render thread never reads a freed unit or wreck
    tagpu_curs.off      contextual cursors at any Interface Type, and the left
                        click that goes with them

  Some things have no switch and stay on whatever files are present: the fixes
  to the stock engine's defects, the raised limits, the extra weapons, the
  building drags, the reclaim sweep and the main menu's REPLAYS button
  (tagpu_menu.off removes the menu screens and the button). The same names with .on instead of .off are what the patch's own tooling writes;
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
  the logs never take more than 128 MB.

  replays\ in the game folder: one .imprec file per game. A new game deletes
  the oldest recordings beyond the newest 50, never a favourite, a bookmarked
  one or one with a taken-over branch.

  profiles\ in the game folder: the per-mod rules for resource snapping and
  Quick Build, created at start when missing.

  impure-patch.ufo next to TotalA.exe: the patch's own menu screens, written at
  every start.

  Nothing else is written unless you ask for a dump.


REPLAYS

  Every skirmish and multiplayer game is recorded (tagpu_recorder.off stops
  it). The REPLAYS button in the main menu lists the recordings of the mod
  you are running. While one plays, the controls on screen pause, change the
  speed, seek on the timeline, set bookmarks, show one player's view or the
  whole map, follow a player's camera, and open economy and army charts.
  Take over picks a player and continues the game from that moment as a
  skirmish; each continuation is kept beside the recording. In a skirmish,
  the rewind button beside the cog goes back in the game you are playing.
  The controls, take over and rewind need the Vulkan renderer.


BUILDING

  With a building selected for placement:

    Shift-drag            a line of them (add Ctrl to keep it straight)
    Shift+Alt-drag        a filled rectangle (add Ctrl for a hollow one)
    Shift+Ctrl-drag       a ring around the unit or queued building under
                          the cursor
    Alt+Z / Alt+X         less / more space between them, kept per building

  A metal extractor or geothermal plant placed by hand snaps to the best spot
  nearby; hold Alt to place it exactly. Quick Build: with a constructor
  selected, give an order on a metal spot or a vent and it builds the right
  extractor from its own build menu, or upgrades your extractor there. Shift
  queues several.

  Reclaim sweep: with Reclaim selected (E), choose METAL, ENERGY or ALL in the
  popup beside the button (E again cycles it), then drag a ring: the selected
  builders share out what is inside it, nearest first.

  The wiki button in the top bar opens tutorials for all of these, played in
  the game with your mod's units. It pauses a single-player game. The reclaim
  sweep and the wiki need the Vulkan renderer.


MULTIPLAYER

  The patch changes the simulation: it raises the game's limits (up to 1500 units
  a player), fixes defects of the stock engine, and gives units built for it
  extra weapons. In multiplayer every player needs this same version of the
  patch and the same unit files.


LICENCE

  MIT for the patch (LICENSE in the repository). Total Annihilation is the work of
  Cavedog Entertainment; this folder contains no part of the game. Built on
  cnc-ddraw (MIT, FunkyFr3sh) and Microsoft Detours (MIT). It contains the
  fonts Oswald (Copyright 2016 The Oswald Project Authors) and Barlow Condensed
  (Copyright 2017 The Barlow Project Authors), under the SIL Open Font License
  1.1, whose full text is in OFL-Oswald.txt and OFL-BarlowCondensed.txt; and
  Zstandard (BSD, Meta Platforms, Inc.), whose licence asks for this notice:

    Copyright (c) Meta Platforms, Inc. and affiliates. All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

     * Redistributions of source code must retain the above copyright notice,
       this list of conditions and the following disclaimer.

     * Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in the
       documentation and/or other materials provided with the distribution.

     * Neither the name Facebook, nor Meta, nor the names of its contributors
       may be used to endorse or promote products derived from this software
       without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
    ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
    LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.
