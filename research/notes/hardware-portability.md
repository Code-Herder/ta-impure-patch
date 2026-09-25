# Hardware portability: depth, lines, and a remote Windows test — the plan (G21)

**Written** 2026-09-25. G21a is built on its branch and waits for the owner; G21c is built
(§3); the rest is not built. This plan is for the Vulkan renderer to draw every
pass on the GPUs players actually have. Today it requires three things that a common class of
card does not offer, and when one is missing the passes that need it stand down. When a gate
here closes, its facts move to [GPU status](gpu-status.html), and this note is folded into it
the way `g19f-plan.md` was.

Evidence tags as elsewhere: **[SOURCE]** read from the code named, **[MEASURED]** with the
numbers, **[INFERRED]** not established, **[DECIDED]** the owner's call with a date, **[OPEN]**
not settled.

---

## 1. What stands down, and why

**[MEASURED 2026-09-25]** on the Windows test setup: an AMD Radeon R9 200-series card (GCN) on
its Windows driver, running main at `8cbecb6` from a player's folder with no `impure.cfg`. Its
`tagpu.log` says:

- `vk: no 24-bit depth format on this device - the passes that depth-test will not arm`. The
  card's Windows driver offers neither 24-bit format.
- `vk: VK_EXT_line_rasterization is not offered - a ported pass that draws LINES will stand down`.
- `vk: VK_EXT_depth_clip_control is not offered - a ported pass whose shader writes a clip z
  below 0 will stand down`.

The menus draw; the census at the shell shows `gui=1` and no world pass, as expected there.

**What each missing feature takes down [SOURCE]:**

| missing | who asks for it | what stands down |
|---|---|---|
| a 24-bit depth format | `vk_depth_format` (`tagpu_vk.c`) offers `D24_UNORM_S8_UINT` or `X8_D24_UNORM_PACK32`, else nothing | the world target (`tagpu_vk_world.c` refuses without `dfmt`), and with it every world pass: terrain, features, units, effects, the markers that depth-test. The units' hard shadows also need the stencil plane (`d->stencilok`) |
| a 24-bit **sampled** depth format | `tagpu_vk_shadow_format` (`tagpu_vk_shadow.c`) | the sun-shadow map behind `shadows=hard` |
| `VK_EXT_line_rasterization` with `bresenhamLines`, and `wideLines` | `tagpu_vk_mark.c`, `tagpu_vk_fx.c` | order lines, selection rectangles, effect lines (lasers, lightning) |
| `VK_EXT_depth_clip_control` (`negativeOneToOne`) | `tagpu_vk_shadow.c` | the sun-shadow map |

**Why these were required.** The comment above `vk_depth_format` says the 24-bit fixed-point
depth "is a specification rather than a preference": the depth-testing passes were measured to
0 px against the OpenGL renderer this one replaced, and a float attachment "would resolve a
z-fight the other way in exactly the cases that are too close to call". The lines follow the
diamond-exit rule, which only the extension's `BRESENHAM` mode guarantees (Vulkan's default line
rule is implementation-dependent). The shadow map's projection writes z in [−1, 1].

**What the Vulkan specification guarantees** [SOURCE: the specification's required format
support]. Every conformant device supports 16-bit depth, and
at least one of `X8_D24_UNORM_PACK32` and `D32_SFLOAT`. It also supports at least one of
`D24_UNORM_S8_UINT` and `D32_SFLOAT_S8_UINT`. It guarantees no 24-bit format in particular, and no
line rule for the default mode.

---

## 2. The decisions [DECIDED 2026-09-25]

1. **Depth is 32-bit float, one path on every GPU.** The world's depth attachment prefers
   `D32_SFLOAT_S8_UINT`, then `D24_UNORM_S8_UINT` (the specification guarantees one of the two).
   There is no shader rounding to a 24-bit grid.
   - The owner first chose exact 24-bit rounding in the shader, then took plain float once its
     cost was set out:
     - writing depth per pixel switches off the GPU's early depth rejection;
     - plain float changes only pixels where two *different* keys are closer than one 24-bit
       step (about 2⁻²⁴). [MEASURED 2026-09-25: it changes none on the reference setup, and why
       is in §3 G21a.]
   - Equal keys still tie exactly, so the mirror's partner keys stack as before.
   - The rule that depth is 24-bit or nothing, and its log line, are deleted.
2. **The stencil fallback is 24-bit.** A device without `D32_SFLOAT_S8_UINT` takes
   `D24_UNORM_S8_UINT` and keeps the units' hard shadows; its ties then fall as they do today.
   The log names the format chosen.
3. **The sun-shadow map follows the same policy.**
   - Its format prefers `D32_SFLOAT` (depth attachment, sampled, and linear filtering for the
     PCF tap), then the 24-bit ones.
   - The shadow projection's z is remapped to Vulkan's [0, 1] in the vertex shader
     (`z' = (z + w) / 2`), so `VK_EXT_depth_clip_control` is no longer needed.
   - The depth bias is re-checked against float depth.
4. **Lines are drawn as triangles, and the fragment shader decides which pixels are on the
   line.**
   - Each line becomes a thin quad covering the band of game pixels it can touch.
   - The fragment shader tests its game pixel against the same diamond-exit rule the lines have
     today. The test is **integer arithmetic on the game-pixel grid**, so no floating-point
     decision near a pixel edge can differ between vendors. That is the invariant.
   - A line's width is one screen pixel, `ss` target pixels, and falls out of the band test, so
     `wideLines` is not needed.
   - `VK_EXT_line_rasterization`, `wideLines` and `VK_EXT_depth_clip_control` are then removed
     from device creation (strip fully, no compatibility path).
5. **Remote instances in `tacli`.** One set of verbs drives a game on another machine over SSH.
   - Remote driving works because every channel between `tacli` and the DLL is a file in the
     game folder: `tagpu_keys.txt`, `tagpu_eye.txt`, the lever files, the `.ab` captures and
     `log\`. The DLL on Windows reads and writes the same files.
   - **Its own test folder**: `tacli remote add` copies the player's game folder once into a
     separate folder on the remote machine. A test never touches the folder the player plays
     from.
   - **In test mode the game writes nothing into TA's settings key, and `tacli` writes no
     registry value. [DECIDED 2026-09-25]** A test folder carries `tacli-state\registry.txt`, and
     the DLL answers the registry imports of `TotalA.exe` and `win32.dll` from that file: TA's own
     key is read and written there, every other key is read-only. The game's one registry write
     outside those imports, the `-r` switch, is closed. Nothing has to be restored. This replaces
     "the registry is saved and restored": a launch killed mid-test (a reboot) left the player's
     key on test values until `tacli` ran again, and a later restore wiped whatever the player
     had changed in between, a gap no save-and-restore design closes.
   - **Outside that guarantee**, because no hook reaches it: the system DLLs the game uses and
     loads by name; the other DLLs it loads at run time (`online.dll`, the extension DLLs
     `online.dll` loads into the game's process, `reporter.dll`, `DebugHelper.dll`); the
     programs the game starts (what `ShellExecuteA` opens, what `online.dll` starts); and
     Windows' own records (the Task Scheduler's of the instance's task while it exists, and
     those of the programs it runs). The list, with where each lives, is in [tacli
     design](tacli-design.html) §"The registry: a file in the test folder".
   - **The agent drives it fully**: launch, menus, `scenario load`, camera, captures (the DLL's
     own `.ab` PNGs, fetched back), log, stop. No desktop screenshot is needed.
   - The remote machine's address, account and key live in the instance's metadata under
     `tagpu/instances/`, which is gitignored. None of it reaches tracked content.
6. **Verification is split.**
   - Pixel identity is proven on the reference setup, against main.
   - "Every pass arms and draws" is proven on the Windows test setup, through the remote
     instance.
   - The owner sees the depth A/B's numbers before the depth landing lands.

---

## 3. The landings

The first three landings are independent and are built in parallel, each by an agent in its own
worktree. The fourth follows once they are on main.

### G21a — float depth, the world and the shadow map

- `vk_depth_format` takes the preference order of decision 1, and the world target, the seam's
  attachment and `stencilok` follow it unchanged. `vk_depth_has_stencil` already knows every
  format involved.
- `tagpu_vk_shadow_format` takes decision 3's order. The shadow vertex shader remaps z, the
  `negativeOneToOne` pipeline state goes, and `zclipok` and its extension ask go.
- **Comments** stating the 24-bit specification (above `vk_depth_format`, in
  `tagpu_vk_shadow.c`, in each pass that refuses without a format) are rewritten to the new rule.
  They are not annotated.
- **Gate, on the reference setup, against main's DLL (24-bit), both presets.**
  - The `.ab` world captures at 1× and at one wheel level (0.877×), on the standard fixtures:
    terrain, features, units, effects, markers, and a scene with `shadows=hard`.
  - Each changed pixel is counted per pass. Each class of change is classified against the
    engine's own frame at 1×: closer to it, further from it, or neither.
  - The report goes to the owner, who decides whether it lands.
  - Frame time is compared too: GPU p50 and p99 at 1080p.
- **Status: BUILT 2026-09-25, not landed** — the owner decides on the numbers below.
  [GPU status](gpu-status.html) §2.91 has the whole of it.
- **The result [MEASURED].** 0 changed pixels on every capture a paused frame repeats: terrain,
  features, units and markers one pass at a time, and the presented frame with `shadows=hard`,
  both presets and both zooms; the table is §2.91's. So there is no class of change to classify
  against the engine's frame. The effects' live fight cannot be paused on one frame twice, and
  its difference sits inside each build's own run-to-run floor.
- **Why none [SOURCE, checked numerically].** The plan expected changes where two keys lie within
  one 24-bit step. There are none, because every tested depth lies in (0.5, 1]: the viewport's
  range is [0.5, 1] (`minDepth 0.5`) and no depth-tested key comes near its bottom. A float32
  there is `j·2⁻²⁴`, and D24's conversion of it is `j − 1` when the device rounds to nearest or
  toward zero: one D24 value per float, in the same order, so a depth test between two float
  fragment depths answers the same on both attachments. A device that may return either
  neighbouring integer still never reverses two depths; at most it could tie two adjacent floats.
  At 0.5 itself the conversion is not one-to-one, which is why the range is open there. The
  neighbouring flats of one row (§2.90) still tie as floats and still draw the first one on top;
  float does not resolve that class toward the engine. Only a rasteriser that computes a D24
  target's depth other than as a float32 could differ [INFERRED].
- **The shadow map's remap and format are built but cannot be run**: nothing produces its
  hand-over (§2.83). The depth bias is the receiver's, in stored units the remap keeps, and
  neither caster pipeline has a rasteriser bias, so no constant changes.
- **Frame time and memory [MEASURED].** No measurable change in GPU frame time at 1080p. The
  process holds 121 MiB more video memory at 1920 × 1080 `ss=2` (599 against 478 MiB): the
  driver lays the float depth-stencil out at eight bytes a pixel, D24's at four [INFERRED from
  the total]. Since the two
  formats draw the same picture, decision 1's order buys one format on every GPU at that price
  where D24 exists. Preferring D24 and falling back to float would draw the same on any
  rasteriser that computes depth as a float32, and cost nothing where D24 exists; what it would
  give up is one format on every GPU. That is the owner's call.
- **Not covered:** a rasteriser other than the reference setup's under float depth, which is
  G21d's Windows card.
- Review: medium (`tagpu/ddraw/**`, no engine state, no thread synchronisation).

### G21b — lines as triangles

- The mark pass's order lines and selection rectangles and the effects pass's lines, drawn as
  band quads with the integer diamond-exit test in the fragment shader (decision 4). The line
  pipelines, the `LINE_LIST` topology and `lineok`, `wideok` and `maxLineWidth` go.
- **Gate**: 0 px against main on the reference setup (which has the extension), both presets,
  at `ss=2` and at 1× and a wheel level:
  - `scenarios/marker-mix.json` for order lines, dots and the band box;
  - a selection;
  - `fx-lasers` for effect lines.

  A line is a rule, not a z-fight, so anything but 0 px is a bug.
- Review: medium.

### G21c — `tacli` remote instances

**Built 2026-09-25; not landed.** The first build met the gate. The high landing review moved
TA's registry key into a launch wrapper, and the high re-review found the gap no save-and-restore
design closes (decision 5). **[DECIDED 2026-09-25] In test mode TA's settings key is never
written:** the registry is a file in the test folder, and there is no wrapper, no export and no
restore. The third high review scoped that claim to what the hooks reach (decision 5 lists the
rest) and made the test-mode decision fail closed. What it is and how it works is in
[tacli design](tacli-design.html) §"Remote instances" and in `tagpu_regstore.h`; this block keeps
the plan's shape and what the work settled.

- `tacli remote add <name> --ssh <user>@<host> [--key <file>] --from <player folder>` copies the
  player's folder once into a test folder (default `<user profile>\tacli\<name>`, or `--to`) and
  records the transport only in the gitignored metadata. It compares the two folders as the
  file system names them (long names, no junction, no `subst` alias) before its first write,
  and writes the metadata and the test folder's marker before the copy. Last, it **seeds
  `tacli-state\registry.txt` by reading the player's key** (`RegistryKey.OpenSubKey(name,
  $false)`, read-only handles). Every verb routes by instance type, keyed by its handler function
  (a positional of `order` is named like the subcommand). The file channels go over one
  PowerShell session per command (`tools/taremote.py`).
- **The registry store (`tagpu_regstore.c`).** **Test mode has two signals, either enough**:
  the token `-xtacli-test` on TotalA.exe's command line, which every remote launch passes, and a
  `tacli-state` folder beside `TotalA.exe` (`GetModuleFileNameW(NULL)`, never the working
  directory). The engine skips the token: `CmdlineArgsNormalize 0x49EE30` dispatches a switch on
  its second character less `'B'`, and `x` (0x36) takes `ja 0x49F461`, the loop tail of every
  unknown switch; no debug switch of `0x4DA0E0`'s table is a prefix of it [DISASSEMBLED].
  **Real mode needs both absent.** A folder that cannot be looked at, without the token, is real
  mode, so a player's folder on a share or behind an access rule stays inert; a `tacli` launch
  always carries the token. Real mode logs `registry: real (no -xtacli-test token, and ...)` and
  does nothing else. The decision is the first thing `DllMain` does, before cnc-ddraw's
  config-tool return, so an inherited `cnc_ddraw_config_init` cannot skip it. **Test mode fails
  closed**: a store that is missing, a folder, unreadable or not loaded whole, no memory, or a
  registry import of `TotalA.exe` or `win32.dll` the hooks do not answer ends the process at
  attach, logged first (`registry: TEST MODE, entered by ..., but ...: the game is not run`).
  Otherwise it replaces TotalA.exe's nine ADVAPI32 imports and `win32.dll`'s two
  (`hook_patch_iat`) before the game's entry point runs:
  - every key under `HKCU\Software\Cavedog Entertainment` is served from the store, and each
    change rewrites the file whole (a temporary file, flushed, moved over it), so a kill leaves
    a whole file;
  - every other key is read-only: a writable open, a create and a value write are refused, and a
    read goes to the real registry. The hooks call no registry function that writes;
  - the game's one registry write outside its imports, the `-r` switch's DirectPlay
    registration through `dsetup.dll`, is closed: `tagpu_patches.c` points the switch's two
    jump-table entries at the parser's loop tail, and an exe that differs there is not run;
  - The keys and values the game reads and writes, and each call site, are in the engine map
    ([exe reverse engineering](exe-reverse-engineering.html) §"The registry").
- **Launch** reads and checks the store first: a test folder whose store is missing, does not
  parse or passes one of the DLL's limits is refused with nothing written. It is a scheduled task
  of the instance's own, `\tacli\<name>`, that **runs TotalA.exe itself** with `-xtacli-test`
  ahead of `--arg`'s switches: an interactive principal for the console user, the test folder
  as working directory, no time limit, and `-Priority 4` (normal: the task default, 7, is below
  normal and the game inherits it). Nothing but the registry needed a wrapper, so there is none.
  Before the task, `launch` puts its test values (sound off, `Interface Type`, the skirmish
  flags, …) into the store, and refuses a DLL that does not fail closed: the build and the test
  folder's copy must both carry the fail-closed test-mode line and the `-r` closure's line
  (`taremote.TEST_MODE_MARKS`), which a build without the store, without the closure, or with
  the earlier single-signal decision lacks. `--arg` refuses any switch whose second character is
  `r` or `d`, the character the engine acts on (`-register` is `-r`), and the token itself. A
  task that starts no game is reported with its `LastTaskResult`. `rm` removes the task, and the
  task folder `\tacli\` when nothing else is left in it.
- **Nothing tacli writes into the test folder is ever missing.** Each file goes under a
  temporary name first and is put in place with `[IO.File]::Replace` or, onto a free name,
  `[IO.File]::Move`. Windows PowerShell 5.1's `Move-Item -Force` deletes the target and then
  moves, which left a moment with no store at every store update.
- **`RegStore` holds to the DLL's limits**, measured against a copy of the DLL's reader (the
  review's figures were one past each): a key path of up to 511 bytes, a value name of up to
  1023, a value of up to 65 536 bytes, 512 values a key, 1024 keys, a file of 4 MiB. `remote
  add` refuses a player's key that would pass one, and `launch` a store that does.
- **The one-statement rule lives in `ps_script`**, which refuses a statement that could span
  lines. A sequence counter makes a line PowerShell skipped stop every line after it, and every
  statement must print a DONE marker. A skipped statement is an error, not an empty answer.
- **The verbs a remote instance answers:**
  - `remote add` and `rm`;
  - `launch` and `stop`;
  - `arm`, whose `<lever>=off` form is the disarm;
  - `keys` and `ui`;
  - `eye` and `shield`;
  - `scenario load`;
  - `log`;
  - `ab`, the capture and its fetch (a new verb, which works locally too);
  - `crash`.

  Every other verb refuses before it sends a statement or touches a local file. An
  `instance.json` that does not read is an unusable instance that every verb refuses (read as
  empty it would look local, and `rm` would delete it), and it is replaced whole on every save,
  through a temporary file of the saving process's own (`tempfile.mkstemp` in the instance's
  folder, then `os.replace`), so two commands saving one instance at once cannot tear it.
- **The shield is the same file with the same meaning**: the DLL drops that desktop's hardware
  input, and `tacli shield <name> off` hands the game over.
- **Gate:**
  - `make -C tagpu/ddraw -j$(nproc)`: `tagpu_regstore.c` names no engine address, so the
    thread-split check needs no line for it.
  - `python3 tools/test_tacli.py`: the remote tests run against an in-memory machine behind the
    real `Session`. Every batch is made by `ps_script`, run by a model of PowerShell (the
    sequence counter, the catch clause, a wrapped .NET exception, a skipped line), and parsed
    by the real marker reader. The model holds TA's real key and records any statement that
    would write it; none does. The store's format is tested for every type and escape, for
    the lines the DLL refuses, and for each of its limits, at the limit and one past it. Four
    threads saving one instance 60 times each leave a whole `instance.json` and no temporary
    file; the shared temporary name it replaced failed that test on 5 runs of 5. 288 tests; the
    one failure, `test_over_tas_stock_cap_only_warns`, fails on main as well.
  - **Local, under wine** (a private Xvfb display, a private copy of the prefix's registry):
    - with the store: `registry: TEST MODE ... 4 keys, 103 values loaded; hooks: TotalA.exe 9 of
      9 registry imports, win32.dll 2 of 2`. The menus, then a skirmish started through `tacli
      ui`, wrote 77 distinct values, all to the store; the file was rewritten for the two values
      that changed,
      and the rest of it was byte-identical to what Python had written (every type and escape
      of the format included). TA's section of the prefix's `user.reg` was identical before and
      after;
    - **the fail-closed paths** (`WINEDEBUG=+relay`, the calls counted from `TotalA.exe` and
      `ddraw.dll`): the token with no `tacli-state`, `tacli-state\registry.txt` as a folder, and
      `tacli-state` with no store each ended the process with exit code 1 at attach (1.5 s),
      after one line, `registry: TEST MODE, entered by the -xtacli-test token, but there is no
      tacli-state\registry.txt beside TotalA.exe: the game is not run` (and its two
      counterparts), and with no registry call from either module;
    - **neither signal**: the one line `registry: real (no -xtacli-test token, and no tacli-state
      folder beside TotalA.exe)`, nothing hooked, and the game calling ADVAPI32 itself as stock
      does;
    - the token and the store: `entered by the -xtacli-test token and the tacli-state folder`,
      and the real registry asked for 3 read-only calls (the DirectX version check) and no
      write. The same with `cnc_ddraw_config_init=1` in the environment: test mode, and the game
      runs;
    - `TotalA.exe -r` by hand (tacli refuses the switch) in the test-mode instance: `the -r
      switch (DirectPlay registration through dsetup.dll) is ignored`, and the game stayed in
      its front end until stopped 45 s later, where stock quits.
  - **Live, on the Windows test setup** (this branch's DLL with the token and the fail-closed
    decision; the fail-closed paths themselves were run only under wine, above):
    - `remote add`: 94 files copied, the store seeded with 3 keys and 79 values;
    - `launch`: the log reads `registry: TEST MODE, entered by the -xtacli-test token and the
      tacli-state folder -- ... 3 keys, 79 values loaded; hooks: TotalA.exe 9 of 9 registry
      imports, win32.dll 2 of 2`, then the `-r` closure's line (the byte-matched patch met the
      same bytes in that machine's `TotalA.exe`), and `TotalA.exe runs at priority Normal`;
    - `scenario load cob-building --restart`, which clicks through the skirmish menu: the store
      served the game's 47 writes, `MixingBuffers` among them (the save `0x430F00`), and the
      file then differed from the player's key in `launch`'s eight test values and in the three
      the game changed (`SingleMapping`, `SingleLineOfSight`, `SkirmishMapping`), nothing else.
      The last count reads `the real registry was asked for 1 read-only opens, 1 reads and 1
      closes, and for no write ... 1 writes were refused`: the DirectX version check, and the
      CD autoplay key. No `.tacli-old` or temporary file was left in the test folder;
    - after `stop`, and again after `rm`: `HKCU\Software\Cavedog Entertainment` exported
      byte-identical to its export before the add (22 990 bytes), and so was the Indeo codecs'
      `Drivers32` key; all 94 files of the player's folder had identical hashes, sizes and
      write times. `rm` reported `the task folder \tacli\ removed, as it held no other task`,
      and the scheduler's root then listed no `\tacli` folder; no test folder and no metadata
      were left.
  - The first live gate's log (the DLL before the `-r` closure) previews G21d on that card:
    `vk: depth format: D32_SFLOAT_S8_UINT (130)`.
- **Not closed.** Decision 5's list lies outside the guarantee: the system DLLs the game uses
  and loads by name; the other DLLs it loads at run time (`online.dll`, its extension DLLs,
  `reporter.dll`, `DebugHelper.dll`); the programs the game starts; and Windows' own records,
  the Task Scheduler's `TaskCache` among them. Which of them write during a test run is not
  measured.
- **The third review's LOWs** (a focused high review of the fail-closed round, nothing HIGH or
  MEDIUM): `launch` succeeds only on the run's served line, and fails with a refusal's line
  (a refused run writes its header and can still be seen as a process); the store is read after
  the running-game check; `rs_refuse_run` cannot return (`noreturn`, and `ExitProcess` in a
  loop after `TerminateProcess`); detach returns early exactly when attach did, so a test
  launch with `cnc_ddraw_config_init` inherited logs `registry: test mode, at exit` (under
  wine, ended by a close request: `the store served 168 opens … 1 writes`), and the refusal
  line names the process; a half-failed `Replace` is named and healed by the next write.
- Review: high (a new DLL hook on TotalA.exe's imports; `tools/tacli`, `tools/taremote.py`; the
  player's registry and folder).

### G21d — the Windows gate

On the Windows test setup, through a remote instance, with main carrying G21a–c:

- the log names the depth format chosen, and no pass refuses;
- the census shows every world pass drawing in a skirmish;
- the `.ab` captures of the G21a fixtures are fetched and put in front of the owner;
- frame time is recorded at the card's native resolution.

Cross-GPU pixel identity is not an exit criterion, since two vendors' rasterisers need not agree
on float depth. Where a Windows capture and a reference-setup capture of the same scene differ,
the difference is counted and reported, not gated.

---

## 4. Documentation each landing carries

- [GPU status](gpu-status.html): the device requirements (depth formats, the removed
  extensions), the shadow map's projection, the line passes. The flat-key paragraph in §2.90 is
  updated if G21a changes that class.
- [Roadmap](roadmap.html): the G21 rows.
- `research/notes/tacli-design.md` and the ta-drive skill for G21c.
- The comments named in each landing, rewritten in the present tense.

## 5. Not in this plan

- **A release.** A release after G21 is the owner's decision, including its number.
- **Other hardware classes.** NVIDIA, Intel and a software rasteriser are not in the test set.
  The log line naming the chosen format is how a report from one would be read.
- **The class-2 pathing difference after an in-game load.** It is a stock behaviour
  ([engine map](exe-reverse-engineering.html)), unrelated to this plan.
