# Measuring: A/Bs, pixel diffs and frame-time figures

Read this before any comparison whose answer is a number. Every rule here was paid for by a
measurement that looked like a result and was not; the stories are in `research/notes/`, the
rules are here.

1. [The four gates](#the-four-gates)
2. [What moves in a "static" frame](#what-moves-in-a-static-frame)
3. [Pick the fixture per oracle](#pick-the-fixture-per-oracle)
4. [Cross-build: two DLLs, one instance](#cross-build-two-dlls-one-instance)
5. [The `.ab` capture of the presented frame](#the-ab-capture-of-the-presented-frame)
6. [Frame-time A/Bs](#frame-time-abs)
7. [Movies](#movies)
8. [Log oracles](#log-oracles)
9. [The restore-dump byte oracle](#the-restore-dump-byte-oracle)

## The four gates

A cross-build pixel figure means something only after three controls, taken in this order:

1. **Settled.** Wait for the census, not for a clock: `vk: census: frame N: 6 pass(es) drew …`
   with the play arm set. Fewer passes right after a load is the level not being up yet (the
   terrain pass and the palette come late, and the world renders greyscale until they do) — a
   capture then reads as a catastrophic regression and is not one.
2. **Static.** Three grabs of the window a few seconds apart. The **settled pair** is whichever
   two agree byte for byte, whatever their index — a run does not necessarily settle and stay
   settled, and the last grab can be the outlier. If no two of three agree, the fixture is
   drifting: stop, do not pick a representative.
3. **The cross-run floor.** The same DLL launched twice differs on most fixtures (a commander's
   minimap blip lands elsewhere, the AI built one more unit). Measure it, and **interleave the
   runs** — `main, branch, main, branch` — rather than three of one then three of the other:
   whatever drifts is slow, so batched runs land in the same state and a build difference is
   invented. Alternating is the control, read down the other diagonal of the same four runs.
4. **Then the number**, reported **inside and outside the mask separately** ("0 px outside the
   minimap" is the sentence that proves a landing).

**Match the game state, not the frame number**: `units: alive=N onscreen=M` is the handle.

**Identify an outlier before explaining it.** A single grab that differs by tens of thousands
of pixels from two byte-identical neighbours is usually a transient the live skirmish produced.
Colour the differing pixels in both frames, look at the two crops side by side, and ask the
census whether the layer that differs is one our passes drew at all. Twelve grabs seeing a thing
once identifies the event and puts no rate on it.

**Pause the sim** when the fixture animates: `tacli keys <i> tab` opens `ARMOPT`, which pauses
the game and writes `PAUSED` across the viewport. Confirm with two peeks of the tick
(`*0x511DE8+0x38A47:4`) four seconds apart. Note that a wheel's camera apply still runs while
paused (it rides the in-play draw, not the tick). **Under `ARMOPT` the keyboard does not reach the
game**: `ctrl+a` selects nothing and `ctrl+1` tags nothing. To pause and keep selecting, ordering
and placing, send `tacli keys <i> pause` instead. It pauses with no menu, and a paused game still
takes selections, `tacli order`, a held drag and a build placement.

**A second skirmish started by clicks can come up paused.** Left through Tab's `EXIT` and started
again with `ui click SINGLE`, `Skirmish`, `Start`, the new game sat at tick 0 (the packet
heartbeat's `paused=1`) and nothing fired or moved; `scenario load` does not leave it so. One
`tacli keys <i> pause` resumes it; read `paused=0` back before measuring anything that runs.

## What moves in a "static" frame

Mask these, or catch both halves in the same phase. Coordinates are 1024x768.

| what | where | note |
|---|---|---|
| the minimap | `[0:126, 0:128]` | 126 is the engine's surface height; 128 is the panel width chosen to cover it, so the rect has ~22 columns of slack |
| the cursor hotspot | the pixel at (512, 384) with the pointer parked there | it pulses; with the UI layer up a "static" `one-unit` frame is four frames, not two, until this pixel and the minimap are masked |
| the parked cursor sprite | ~26x36 around wherever it is parked | animates between launches even when nothing else does |
| the `PAUSED` banner | x 502..636, y 372..400 | blinks free-running; two paused instances differ by the whole word or by nothing |
| the "forces have been obliterated" lines | x 138..430, y 52..106 | written by `clear_existing`; the wording is random per boot; they expire on **ticks**, ~60 s unpaused at speed 10 — pausing straight after a load freezes them on screen |
| the message band | y < 130 | crop it off for a cross-boot comparison |
| the frame's right edge on `selbox-facings` | x 1017..1023, y 236..277 | a two-state 71-px artefact that reproduces DLL against itself |

**Crop the region and look at it before explaining a floor.** A 180x60 crop showing `PAUSED` in
one capture and grass in the other took a minute and saved a fix for a problem that did not
exist.

## Pick the fixture per oracle

A fixture carries either pixels or counters; wanting one to do both is how a landing measures
something that does not touch the code it is named for.

| fixture | what it is good for | trap |
|---|---|---|
| `one-unit` | a settled pixel A/B on the placed unit's box (`world=(1600,1600,91)`, `screen=(512,384)` at the standard camera) | it is a live skirmish: 15–17 units across three AI opponents; only the placed unit is still, and our commander's minimap blip lands in a different place per run |
| `crowd-static` | a **log oracle at scale** — 16 unit types, so atlas feeds and repacks become visible | static as a *situation*, not as pixels: its 256 idle COB scripts move ~12 000 px between grabs; it cannot carry a pixel A/B |
| `selbox-facings`, `selbox-slope` | frozen frames without pausing: still tanks, a 0-px floor within seconds, the same md5 across relaunches | the cursor sprite and, at zoom < 1, a stray animating feature |
| `static-terrain` | terrain and the cast-shadow map: its two towers sit in opposite corners, so one caster is in view from either | the default sun elevation makes ground self-shadowing nearly invisible; `shadowsun=225,8` is the fixture for the map |
| `feat-forest` | features; `tacli eye <i> 1400 1600` puts the map's one 3D wreck off screen with features still in view | a walking commander gives it a ~4 000 px cross-launch floor |
| `nanoframe-ladder` | every build-state stage at once: solars at 5/25/50/75/95 %, a lab at 40 %, a Stumpy at 60 % | the units are created finished and then have `+0x104` written, so a mobile one never gets the engine's depth-plane bake and the golden source draws it textured; watch a real build (a commander placing a solar, a lab building a kbot) for the engine's mobile nanoframe |
| `exit-sort` | the one scene that reproduces across **boots**: 0–1 px below y = 130 | the message band above it differs by ~5 200 px of random wording |
| `fx-rockets` | effects that stay alive for minutes | `fx-lasers` burns out in tens of seconds: `fx: proj=0 … lines=0` means nothing to capture |
| `pose-inventory`, `pose-inventory-sea` | 69 units in four clusters covering every pose class, all owned by player 0 so nothing fires; camera stops `tacli eye <i> 1716 806` (ground), `2716 806` (structures), `3616 806` (the extremes), `2216 1606` (air) | — |
| `tascene-parity`, `tascene-parity-core` | the UI walk's in-game fixture, ARM and CORE | — |
| `200v200`, `500v500` | load and stress | two loads diverge to different survivors; a battle cannot be paired |
| `shadow-lab`, `shadow-struct` | shadow pictures | mex spinners and blades put 3 000–4 000 px between shots one second apart; pause first |

**Colour counts are not an oracle across boots** (the same build gave 2 727 and 498 colours on
two boots of one fixture as the map revealed more). Use a count to tell a game from a lever
colour, never to compare two runs.

**A capture that is meant to prove a picture is right must assert something positive** — a
colour count, an expected run of pixels — never only the absence of a sentinel. An all-black grab
scores 0 magenta and shows no game.

## Cross-build: two DLLs, one instance

- **Build both in a real worktree**, not in a `git archive` copy: `spirv-check.sh` fails outside a
  git repository, so the archive never links. Commit first, then
  `git checkout HEAD~1 -- <the landing's files>`, `make`, copy the DLL aside,
  `git checkout HEAD -- <same files>`, `make`, copy the second aside; `git status --porcelain`
  must be empty before and after.
- **Clean-build both sides**: `make -C tagpu/ddraw clean` before each. An incremental build has
  produced a `.text` 17 760 bytes different from a clean build of the byte-identical tree; the
  cause is not established and the rule does not depend on it. Record `stat -c%s` and
  `objdump -h ddraw.dll | grep .text` for both — two builds of one tree differ in the PE
  timestamp but must agree on `.text`.
- **Swap the DLL in the same instance**: replace the worktree's `tagpu/ddraw/ddraw.dll` before
  `scenario load` (tacli copies it into the gamedir at launch; editing the gamedir's copy is
  overwritten), or `cp` it over `<gamedir>/ddraw.dll` and launch with `--keep-dll`. `md5sum` the
  gamedir copy in the script's own output: that line says the run tested what you think. A build
  that still reads `ddraw.ini` is refused: run it with a tree of its own age.
- **The stock engine limits are a second build of the same tree**: `make -C tagpu/ddraw -j$(nproc)
  LIMITS=stock` writes `ddraw-stocklimits.dll` beside `ddraw.dll`, from objects of its own, so the
  two never mix. `cp` it over `<gamedir>/ddraw.dll`, launch with `--keep-dll`, and read
  `limits: stock build -- nothing raised` in the log before believing the run.
- **The unit limit is 1500 a player, and an instance keeps the last one it was given.** With no
  `UnitLimit` in the instance's `totala.ini` the game runs at 1500; `--unit-limit` or
  `setup.unit_limit` writes one, and it stays until another launch writes a different one, so a
  fixture with no `unit_limit` inherits the instance's last value (the fork then refuses the
  apply with "this game's cap is 500"). Write it in the fixture. Under `ddraw-stocklimits.dll` the
  engine clamps to 500 again and defaults to 250, which `tacli`'s schema does not know: `scenario
  validate` passes up to 1500 a player and 15 000 entities, and the apply then refuses. The apply's
  own check is against the cap in force (`main+0x37EE6`), which is the host's in a network game.
- **The GDI lane is `tacli arm <i> gdi.on`**, before the launch and read at attach; every launch
  path keeps it, `scenario load` included. Confirm with `tacli log <i> -g 'renderer GDI'`, and
  clear it with `gdi.on=off` afterwards.
- **Sound goes to a null device, never to the human's speakers.** Export
  `PULSE_SERVER=unix:/nonexistent/pulse` and `ALSA_CONFIG_PATH=<a file holding
  pcm.!default { type null }>` in the shell that runs `tacli`, and set `"sound": true` in the
  instance's `instance.json` (`scenario load` has no `--sound`; set it back to false after). Effect
  sounds also need the registry's `fxvol` above 0, and every silent launch writes it to 0 in the
  instance's registry: `tacli registry <i> fxvol=<n>` before the sound launch (which leaves it
  alone). The sound object is `*(main+0x10)`: `+0x2C` MixingBuffers, `+0x30` the sounds in use.
  Any launch of the store's DLL saves its `mixingbuffers` (32) into the instance's registry, so a
  control launch of that instance reads 32, not stock's 8: `tacli registry <i> MixingBuffers=8`
  before a sound A/B that needs stock's.
- **A single-player fight does not reproduce run to run, even under one DLL**, so a COB-trace or
  roster timeline compared across two runs measures noise from the first impact on. Compare what
  is a function of the build (a static frame, a pose, a table), or two peers of one network game.
- **A foreign tree's `tacli` cannot make instances** (no wine prefix template outside a real
  checkout). Make the instance from a real checkout and swap the DLL as above. If you do set up a
  second tree, `wineprefix` there must be a `cp -al` clone and not a symlink — `cp -al` on a
  symlink copies the symlink, every instance then points at the shared template, and the game
  exits during launch with no `ErrorLog.txt` and a healthy-looking `tagpu.log`.
- **The `*own` levers are written by `scenario load`** from the passes already armed, and it
  prints `auto-armed …` for each. An instance armed by hand with the same pass list and a plain
  `tacli launch` also gets them. Compare a build against itself in the same instance rather than
  standing up a second one.
- **Check the arm took before believing a UI figure**: `gui: ARMED flip@0x4C63A0=1 leaves=17/17`
  in the log, not `gui: trigger host only`. A whole UI A/B has been run, labelled and written up
  with the layer off; the frames were fine and covered none of the code under test.

## The `.ab` capture of the presented frame

`tagpu_<pass>.ab` in the gamedir (`terr`, `feat`, `fx`, `mark`, `posedraw` for units, `scaffold`,
`fps`, `gui`) makes the Vulkan lane write one frame as `tagpu_<pass>_vk.ppm`. World passes are
captured from the world target, `gw*ss x gh*ss` — 2048x1536 at 1024x768 with the shipped `ss=2`;
the UI-side passes are the window's size. Compare two builds' files with
`tools/vk-ab.py <old.ppm> <new.ppm>`, which reports them as `A` and `B` (a gamedir argument is
refused). Its exit status is 0 only when every pixel agrees. It does not check that a capture is
from this run; `tacli ab` removes the old `.ppm` before it arms, so take captures with it.

```bash
tools/tacli arm <i> terr.on                                          # ONE pass; the clear is black
tools/tacli scenario load <i> feat-forest --restart --res 1024x768 --vsync off
sleep 10
tools/tacli ab <i> terr -o /tmp/terr-B.ppm          # "terr: 2048x1536 -> /tmp/terr-B.ppm"
```

`tacli ab <i> <pass>` removes the old `.ppm`, creates the lever, and waits for `vk: shot: wrote`
(or fails on the refusal line, quoted). Then it removes the lever again. On a remote instance the
file is fetched to `tagpu/instances/<i>/ab/`. **A pass re-arms only once its own poll has seen
the lever gone** (every 30 frames; the UI layer every 500 ms), and nothing logs that moment. So a
lever removed less than `--settle` seconds ago (default 2) is kept gone for the rest of that time
first. That covers a leftover, and the previous `ab` of the same pass, whose removal time the
instance's metadata keeps. Below 15 fps, raise `--settle`. A settle that is too short makes the
capture time out; it never returns a stale file.

- **Arm one pass and nothing else.** The lane refuses a frame more than one pass drew into, and
  says so: `vk: N A/B levers claimed this frame and M passes drew into it - nothing captured`. The
  full play set therefore produces no file, which looks like a broken build and is not. Units need
  `native.on`; effects need `native.on` for the call site, so give it a type filter that matches
  nothing (`native.on=nosuchunit`) or the unit pass draws too; features need `native.on … wrecks`
  and a camera with the 3D wreck off screen. The readout needs the font, which `markown` produces:
  launch with `mark.on`, then `arm <i> mark.on=off` and wait a few seconds before arming the `.ab`.
- **The lever latches until its file goes away**, and a file already there re-arms nothing, which
  is why `ab` removes it first. Armed before a `--restart` it is consumed before the pass is
  ready: capture after the load.
- **A missing `_vk.ppm` means "no capture"**: the pass unlinks the target when it latches the claim
  and writes it only after `vk: shot: wrote …`. Every refusal names itself (`could not be removed
  (error N) - this arming is REFUSED` — usually an image viewer holding the last capture;
  `a capture is already in flight`; `outside 1..8192`; `the N-byte staging buffer was refused`).
- **Both captures need the same clear colour**: it is black by default, and a `vk.on=color=` on one
  side only differs in every pixel that is not ink. A DLL built before the default became black
  clears magenta: arm `vk.on=color=0,0,0` on that side.
- **The effects pass cannot be compared across two runs** — it draws transient projectiles, and
  two captures agree only if the same ones are alive. Arm the `.ab` about four seconds after the
  apply, while `fx: … flashq=` is non-zero.
- **The unit pass has a stated bar**: ~0.03 % of unit ink, single pixels on texture-row
  boundaries, deterministic; a *structured* difference (a whole unit, a face, a shift) is real.
  Take it with `aniso=1` in `classicpp.cfg` (anisotropic sample placement is implementation-
  defined; 4 is what ships) and `shadows=0`, or the soft-shadow PCF's 1 px is in the figure.
- **Lines have their own oracle.** A claimed frame of `mark`, `fx` or `posedraw` also writes
  `tagpu_<pass>_lines.txt`: a header `# <pass> <grid w> <grid h> <w>`, then every line with its
  integer ends in LINE-GRID pixels (the world target's own, `w` = `ss` to a game pixel) — for
  `mark` and `fx` the ends BEFORE DrawLine's clip, with the clip rect (`<L> <T> <R> <B>`, line
  grid) after `<w>`; `tools/line-oracle.py <capture.ppm> <lines.txt>` clips (`0x4BEA20`,
  `0x4CC650`), walks them with its own reading of `DrawLine 0x4CC7AB`, thickens to `w` and
  compares lit pixels, 0 px or not. `tacli arm <name> ss.off` gives the game grid (`w = 1`),
  where the rule is the engine's line. To put
  lines across the viewport edge, pin the camera with `tacli eye` after staging. The capture must hold lines ONLY
  (anything else is "only in the capture", and bodies hide the wire), which the shipped build
  cannot arrange: it needs a build whose recorders skip every non-line draw. The waypoint
  crosshairs change size with game time, so two runs paused on different ticks do not pair.
- **A world capture at 1080p with `ss=2` is 3840x2160, 33 MB**, in a 32-bit process; the lane
  refuses and logs rather than faulting. 1024x768 is 9.4 MB and nothing to think about.

## Frame-time A/Bs

- **`--vsync off` on both sides**, or you measure the refresh rate: vsync is the only pacing, and
  on it holds both sides at it. It is a sticky launch knob; confirm it with `grep vsync
  <gamedir>/impure.cfg` (`vsync=off`) or `tacli log <i> -g '^frame cap'` (`none (vsync off)`).
- **Pause the sim first** (`tab`, then peek the tick twice): on a fighting scenario units die under
  the measurement and the second half draws a smaller scene.
- **`ftime.on`** logs `ftime: vk p50 <ms> p99 <ms> (n=…)`, the GPU frame time on the Vulkan lane
  from two timestamps. **`fps.on`** puts the readout on screen. The `units:` line cannot meter
  frames: it is written every 500 ms of wall time, not every N frames.
- The `*own` halves save the engine's CPU (it stops rasterising what our pass draws) at the price
  of holes in the golden source — arm them for a frame-time figure, not for a comparison.
  **`terrown` is a play default**, so a defaults run already carries its hole and its saving; the
  A/B is `terrown.off`, and that costs the zoomed-out fog rather than the picture
  (`references/levers.md`, the play defaults).

## Movies

- **Only an `--intro` instance has one.** `tacli create <i> --intro` keeps `Data/1.ZRB`/`2.zrb` in
  the mirror: the intro `1.zrb` (20 s) plays at every launch, and the main menu's `INTRO` gadget
  (`tacli ui <i> click INTRO`) plays the cinematic `2.zrb` (135 s) on demand. A key (`WM_CHAR`)
  ends a movie. The launch's intro starts before the Vulkan lane is up, so its first dozen frames
  are never sent; measure on the INTRO button's movie.
- **The armed shield answers `GetFocus` with the game window**, and the movie's frame routine
  decodes nothing without focus. So an armed instance plays its movies unfocused, and a
  `--no-shield` one holds its first frame until its window is focused, which you may not do.
- **A movie is paced by its audio clock** (`SmackWait`), so a cadence needs `--sound` into a
  sink that runs in real time. Silent, it runs unpaced: the launch's intro has flipped before the
  lane records, and the cinematic's frames are nearly all skipped in flight. Into ALSA's `null`
  device (the recipe above) its intervals scatter whatever the DLL does. The rig that works is a
  private PulseAudio null sink: `pulseaudio -n -F <file> --daemonize=no --exit-idle-time=-1`,
  with the file loading `module-null-sink` and `module-native-protocol-unix auth-anonymous=1
  socket=<scratch>/native`, `HOME`/`XDG_RUNTIME_DIR` in the scratchpad and
  `DBUS_SESSION_BUS_ADDRESS` unset, run in the background; then launch with
  `PULSE_SERVER=unix:<scratch>/native` and `ALSA_CONFIG_PATH` unset. Nothing reaches a speaker.
- **Read the lane off the log**: `gui: movie <W>x<H> at (0,<top>) on surface …` at a movie's
  first carried frame; `gui: movie ended after N flip(s): R frame(s) recorded, S skipped in
  flight, U not sent (the lane was not recording), F refused` at the next flip that is not a
  movie's. Each movie logs its own (the player flips once before it); a looping movie's repeats
  count as one. A skip means the render thread had not drained the frame before; `movie=` in the
  `gui:` heartbeat counts the frames that reached the Vulkan lane.
- **See it on a private Xvfb.** `tacli create <i> --display :N` on an `Xvfb :N -screen 0
  1280x1024x24` of your own: the GPU presents into it, `DISPLAY=:N import -window <id>` captures
  the presented picture, and `ffmpeg -f x11grab -framerate 60 -video_size 640x480 -i :N+0,0`
  records it. Count the distinct frames of the recording: a 30 fps movie under FIFO at 60 Hz
  holds each for exactly two captures. On the live display a monitor in DPMS power save
  (`xset q`: `Monitor is Off`) throttles FIFO presents to about one a second on the reference
  setup and `import -window` returns black; never wake or force it.

## Log oracles

- **A log line beats a picture whenever the thing under test has a log line.** A lane that stands
  down does so silently on screen and loudly in the log.
- **A pixel diff cannot cover an instrument.** Anything that writes to `tagpu.log` rather than the
  framebuffer — a heartbeat, the census, `ftime.on` — is invisible to a frame comparison; a landing
  that changed one has not verified it by capturing frames. Arm it, let it report, paste the line.
- **Wait for the lines, never for a clock.** A `sleep` asserts nothing; a poll that requires every
  expected line and then a quiet window (10 s with no new line and the output files' sizes
  stable) is both faster and stronger. Keep the old settle value as the *timeout*.
- **Wait on a condition, not a duration, when the machine is busy**:
  `until tools/tacli roster <i> | grep -q '^u1'; do sleep 3; done`.
- **A/B-ing any live lever: wait for a FRESH heartbeat before the second shot.** The `native:`
  line is written every 300 frames; a lever flipped and shot three seconds later is read against
  the previous setting's counters. Count the lines, flip, wait until the count has moved by two.
- **Slice the log with a cursor** to attribute a run: `M=$(tools/talog.py mark <gamedir>)`
  before, `tools/talog.py since <gamedir> "$M"` after. A byte offset breaks at the first
  rotation, and a grep over the whole run can hand you the previous game's lines after a reload.

## The restore-dump byte oracle

`tagpu_restoredump.on` makes the Vulkan restorer write each atlas's restored twin once its queue
drains, with the source it restored from beside it: `tagpu_restore_<tag>_vk.rgba` (`.mips` for the
units, whose twin carries a mip chain) and `tagpu_restore_<tag>_vk.base` — the RGBA8 base atlas
the world passes draw from — for the terrain, features, effects and units; the UI's source is its
R8 atlas, `.r8`. Beside them, `.pal` (the palette) and `.idx` (`# atlas W H`, then one line per
painted frame: `dx dy w h key wrap border padR padB`). It reads a finished image off the device, so it needs no window, no parked pointer and no settle
heuristics, and it answers whether two builds restore the **same bytes** for the whole atlas.

**Whether the bytes are right** is `tools/restore-dumpcheck.py <gamedir>` (the venv's python):
it restores every frame the `.idx` lists again from the dumped source with the torch model in
strict fp32 and the DLL's colour-key stand-in, and holds the twin to it — at most one level on
under 0.01 % of opaque bytes, keyed texels `(0,0,0,0)`, the ring a copy of the edge, the mips the
exact box average. One line per job, `PASS`/`FAIL`; `--tag terr` for one job, `--json`; exit 0, 1,
or 2 when nothing was checked. It needs no pack, so it works on any scenario.

```bash
tools/tacli arm <i> classicpp.on 'native.on=all wrecks' terr.on feat.on fx.on gui.on \
                    restoredump.on 'restoreglsl.on=log'
tools/tacli scenario load <i> feat-forest --restart --res 1024x768 --vsync off
# poll tagpu.log for the dump lines, then cmp each tagpu_restore_<tag>_vk.rgba across builds
```

- **Check for the dump file and the atlas count, not for the arm line.** `feat.on` alone makes no
  feature atlas exist — without `native.on … wrecks` the pass never owns its leaf and the log says
  `atlas=0` and `nothing emitted: native.on needs "wrecks"`. `feat` and `fx` arm their jobs on most
  fixtures and often never drain inside the window.
- **Diff the `.base` too** — a twin that differs over a source that does not is the restorer's;
  one whose source differs is the atlas upload's, and the restore is only repeating it.
- **Compare the code-determined counts** in the restorer's lines (frames, batches, dispatches), never
  the timings: the slice budget is wall-clock driven.
- **A difference that is a multiple of the cell pitch squared is a dropped frame, not a wrong
  pixel**: `34² = 1156` for terrain (32 px tile + 1 px border each side). Cluster by
  `(y / 34, x / 34)` before theorising.
- **When a dependent lane's picture is wrong, measure its input at the moment of use** before
  reasoning about its arithmetic — a temporary shader edit that writes the index it sampled is a
  one-run answer. Editing a shader string re-hashes the SPIR-V: run `tools/spirv-gen.py` after the
  edit and `git checkout` both the header and `inc/spirv/` afterwards.
