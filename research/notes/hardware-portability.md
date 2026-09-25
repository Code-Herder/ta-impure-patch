# Hardware portability: depth, lines, and a remote Windows test — the plan (G21)

**Written** 2026-09-25. G21a is built on its branch and waits for the owner; the rest is not
built. This plan is for the Vulkan renderer to draw every
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
| `VK_EXT_line_rasterization` with `bresenhamLines`, and `wideLines` | `tagpu_vk_mark.c`, `tagpu_vk_fx.c`; `wideLines` also `tagpu_vk_unit.c` | order lines, selection rectangles, effect lines (lasers, lightning); the nanoframe wire, a line list `ss` target pixels wide |
| `VK_EXT_depth_clip_control` (`negativeOneToOne`) | `tagpu_vk_shadow.c` | the sun-shadow map |

**Why these were required.** The comment above `vk_depth_format` says the 24-bit fixed-point
depth "is a specification rather than a preference": the depth-testing passes were measured to
0 px against the OpenGL renderer this one replaced, and a float attachment "would resolve a
z-fight the other way in exactly the cases that are too close to call". The lines asked for the
extension's `BRESENHAM` mode because Vulkan's default line rule is implementation-dependent — but
that mode is not exact either: the specification lets a Bresenham line's fragments deviate from
the ideal ones by up to one unit, so two conformant devices can still light different pixels.
It matched on the reference setup's device; it guaranteed nothing across devices. The shadow
map's projection writes z in [−1, 1].

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
4. **Every line is `DrawLine 0x4CC7AB`'s Bresenham on the game-pixel grid, drawn as triangles
   [DECIDED 2026-09-25].**
   - Each line becomes a band quad round the segment, and the fragment shader keeps a fragment
     only when its GAME pixel is one `0x4CC7AB`'s walk plots between the line's two integer
     endpoints. Each lit game pixel is covered whole, `ss` × `ss` target pixels, so a line is one
     game pixel wide at any `ss` and `wideLines` is not needed.
   - The test is **integer arithmetic on the game-pixel grid**, so no floating-point decision
     near a pixel edge can differ between vendors. That is the invariant.
   - It covers every line kind: the order markers (waypoint crosshairs), queued build sites
     (`0x438C00`), selection rectangles, effect lines (lasers, lightning) and the **nanoframe
     wire**, which is ported so that `wideLines` can go. The wire stays depth-tested; its
     fragment depth is the segment's by construction, and the +0.15 bias that puts it in front
     of the surface it traces is kept.
   - **Endpoints are quantised by one stated integer rule** (`tagpu_line.h`): the game pixel is
     `floor(Z(p))`, `Z` the wheel zoom about the view centre — the selection rect's notion of the
     grid. `p` is the engine pixel's centre `k + 0.5` where the engine draws that line from
     integers (lasers, build-site corners, selection corners), so at 1× the pixel is the
     engine's own; otherwise `p` is the fractional position (a sub-pixel anchor, a posed wire
     vertex), and at 1× that is the engine's `>> 16` truncation.
   - `VK_EXT_line_rasterization` and `wideLines` are removed from device creation, strip fully
     with no compatibility path; `VK_EXT_depth_clip_control` goes with decision 3.
   - **What main did** [SOURCE, then MEASURED 2026-09-25]: the order markers, build sites,
     effect lines and the wire were line primitives on the `ss×` TARGET grid, `ss` target pixels
     wide, in `BRESENHAM` mode. At `ss = 2` such a line straddles game-pixel boundaries — a step
     lands half a game pixel off and lights part of a game pixel's block. The selection rect
     alone already had the game-pixel test, riding a wide `BRESENHAM` line primitive as its band
     ([GPU status](gpu-status.html) §2.84). The G21b block has the counts.
5. **Remote instances in `tacli`.** One set of verbs drives a game on another machine over SSH.
   - Remote driving works because every channel between `tacli` and the DLL is a file in the
     game folder: `tagpu_keys.txt`, `tagpu_eye.txt`, the lever files, the `.ab` captures and
     `log\`. The DLL on Windows reads and writes the same files.
   - **Its own test folder**: `tacli remote add` copies the player's game folder once into a
     separate folder on the remote machine. A test never touches the folder the player plays
     from.
   - **The registry is saved and restored.** Before a remote launch, `tacli` exports TA's
     registry key on the remote machine. It restores it when the instance stops, and on the next
     remote command if a stop was missed, and it says when it restores.
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

**[DECIDED 2026-09-25; BUILT 2026-09-25 on its branch, not landed]**

- **What it is.** Every line kind of decision 4 is an instanced record drawn as a six-vertex
  band (`tagpu_glsl.h` `taBand`, radius 2 game px) whose fragment stage keeps the game pixels
  `0x4CC7AB`'s walk plots (`taGamePx`, `taOnLine`). The endpoints are decided on the CPU
  (`tagpu_line.h`); the queued build site is `0x438C00`'s integer geometry exactly
  (`tagpu_order.c` `draw_build`); the nanoframe wire is posed on the CPU into line records in the
  pose buffer (`tagpu_vk_unit.c` `wire_records`) and drawn by its own vertex stage
  (`tagpu_posedraw.c` `WVS`). The line pipelines, the `LINE_LIST` topology, `vkCmdSetLineWidth`,
  `lineok`, `wideok`, `maxLineWidth`, the two device asks and their log lines go; so does
  `tagpu_vk_world_scale`, which only the line width read.
- **The gate: 0 px against an independent model of the rule.** An A/B frame of the marker,
  effects or unit pass also writes `tagpu_<pass>_lines.txt`, the lines it drew and their integer
  ends. `tools/line-oracle.py` walks each with its own transcription of `0x4CC7AB`'s loops (the
  one in `tools/line-band-check.py`, which also checks the shader's closed form against it and the
  band's coverage by brute force) and compares the lit pixels with the capture. The captures hold
  the lines alone: a diagnostic build, never committed, drew only the line draws while a file was
  present. [MEASURED 2026-09-25, reference setup, `ss = 2`, `--defaults`] **0 px in every cell**:

  | fixture | lines | Classic 1× | Classic 0.877× | Classic++ 1× | Classic++ 0.877× |
  |---|---|---|---|---|---|
  | `marker-mix` (4 selection rects, a queued build site, 12 waypoint crosshairs) | 72 | 0 of 4 752 px | 0 of 4 216 | 0 of 4 752 | 0 of 4 216 |
  | `selbox-facings`, three tanks selected | 12 | 0 of 1 216 | 0 of 1 072 | 0 of 1 216 | 0 of 1 072 |
  | `fx-lasers`, three lasers in flight | 6 | 0 of 1 056 | 0 of 836 | 0 of 1 056 | 0 of 836 |
  | `nanoframe-ladder`, every wire | 1 384 | 0 of 27 536 | 0 of 23 688 | 0 of 27 536 | 0 of 23 688 |

  The log names no line refusal and neither `VK_EXT_line_rasterization` nor `wideLines`; main's
  `vk: mark: up … bresenham lines yes` is gone.
- **The difference from main** [MEASURED 2026-09-25, lines-only captures, Classic], target px
  lit only in main / only in the branch, and at 1× each differing pixel classified against the
  engine's own frame (`tacli shot`, markers handed back with `mark.on=passive`):

  | fixture | 1× only main / only branch | 1× closer / further / neither | 0.877× only main / only branch |
  |---|---|---|---|
  | `marker-mix`, rects and build site | 643 / 651 | 1 294 / 0 / 0 | 708 / 712 |
  | `selbox-facings` | 0 / 0 | — | 0 / 0 |
  | `fx-lasers` | 99 / 115 | 180 / 12 / 22 | 91 / 135 |
  | `nanoframe-ladder` | 6 624 / 5 503 | not established | 4 973 / 4 244 |

  - The build site is the whole marker-mix difference (every differing pixel is inside its box;
    the selection rects are identical, as `selbox-facings` shows): main's edges straddle game-pixel rows,
    the branch's sit on the engine's.
  - The waypoint crosshairs are left out of the marker-mix row: their size follows game time, and
    the two runs paused on different ticks, so they are not a pair. The oracle covers them.
  - The fx row is paired by construction: a main build that also wrote its line list, run
    through the branch's rule, against main's own capture of that frame. The 12 "further" and
    22 "neither" pixels lie where the engine draws a muzzle flash over the laser, in colours the
    laser shares **[INFERRED from the crops]**.
  - **The wire is not classified.** Its engine pixels were matched by colour, and the nanoframe
    body the engine draws under it shares the wire's palette entries: the match found 14 855
    engine pixels against the wire's 6 884 game pixels, and the two runs' engine frames gave
    different answers. The counts stand; the direction is **[OPEN]**.
- **Gaps it does not close.** The engine's clip `0x4CC650` moves an end that lies off the
  surface onto its edge; the lane walks the unclipped line, so a line crossing the viewport edge
  can differ from the engine's by a pixel along the part both draw (`tagpu_line.h`). Not measured.
- Review: medium. No engine state is written and no byte patch added; the wire's records are
  built on the render thread, in `upload_draw`, from the same pose hand-over and bake the body
  upload reads [SOURCE].

### G21c — `tacli` remote instances

- `tacli remote add <name> --ssh <user>@<host> [--key <file>] --from <player folder>` creates the
  test folder on the remote machine (decision 5) and records the transport in the instance's
  metadata. Every verb then routes by instance type. The file channels go over SSH.
- **Launch** is the scheduled task in `.claude/skills/ta-drive/references/modules.md`: an
  interactive principal for the console user, the test folder as working directory, no time
  limit. The registry export comes before it.
- **Scripts go over SSH as PowerShell on stdin, one statement a line.** A statement that spans
  lines is skipped silently. `tacli` generates these scripts, so the rule lives in one function.
- **The minimum set of verbs for G21d:**
  - `remote add` and `rm`;
  - `launch` and `stop`;
  - `arm` and `disarm`;
  - `keys` and `ui`;
  - `eye`;
  - `scenario load`;
  - `log`;
  - the `.ab` capture and its fetch;
  - `crash`.

  Every other verb refuses on a remote instance with a message, rather than acting on the local
  filesystem.
- The shield is on by default, as it is locally.
- **Gate:**
  - `python3 tools/test_tacli.py` covers the script generation and the routing offline;
  - live, on the Windows test setup: one scenario loaded, one capture fetched, the log read, the
    instance stopped;
  - the player's folder is byte-identical before and after, and the registry key is
    byte-identical before and after.
- The ta-drive skill's by-hand section in `references/modules.md` is replaced by the verbs,
  per the skill's own rules.
- Review: medium (`tools/tacli`).

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
