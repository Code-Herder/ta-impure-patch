# Resolution — how TA sets, derives and persists its display mode

*Static map of the full resolution pipeline: where the mode is chosen (video-options
UI, battleroom), where it is stored, how the viewport rect and every view-derived
dimension is computed from it, how it persists (registry), and how the mode list is
enumerated — for the GPU-renderer / native-res work. All addresses are VAs for our
pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`);
disassembly via `objdump -d -M intel` (`.text` VA→file = `VA−0x400C00`), decompiler
output from a scratch copy of `tools/ghidra-projects/TA.gpr`.*

Evidence tags as in `terrain-depth.md`: **[BINARY-VERIFIED]** = instructions/
decompiler output read for this build this session. **[CORPUS]** = TADR/`tamem.h`
cross-checked against our bytes. **[INFERRED]** = my reading, not yet
runtime-confirmed. `main` = `*(void**)0x511DE8` (the `TAdynmemStruct`);
`TAProgram` = `*(void**)0x51FBD0` — which this session pins to the **static block
at `0x51F320`** (stored at `0x4B5990` inside `CreateMainWindowAndDXInit 0x4B5980`).
[BINARY-VERIFIED]

---

## Summary — the seven things to know

1. **There are two resolution variables, and they are distinct.**
   `TAProgram+0xD4/+0xD8` = the *current physical* screen W/H (what
   `SetDisplayMode` was last given). `main+0x37F1B/+0x37F1F` = the *desired*
   resolution (registry-backed options value). The UI writes only the desired
   pair; the physical mode catches up at defined sync points. [BINARY-VERIFIED]
2. **Persistence is registry-only**:
   `HKCU\Software\Cavedog Entertainment\Total Annihilation`, values
   **`DisplaymodeWidth`** / **`DisplaymodeHeight`** (REG_DWORD), loaded raw —
   no clamp — by the options loader `0x42F9A0` (defaults 640/480 written back
   when absent), saved by `REGISTRY_SaveSettings 0x430F00`. **No command-line
   resolution switch exists; totala.ini `[Preferences]` has none either.**
   `DisplaymodeDepth` is *not* a bit-depth setting — bpp is hard-coded 8; the
   value participates only in an easter-egg check (`==256` + `Games==1`).
   [BINARY-VERIFIED]
3. **The app always boots at 640×480** (hard-coded in `CoreStartupAndMainLoop`
   at `0x49E91C/0x49E93B`); the whole front end runs at 640×480, and leaving a
   game restores 640×480 (`0x491ADC` block). The desired mode is applied **only
   on game entry**, by the play-screen state handler **`0x497F40`**: draw the
   640×480 loading screen → set the *logical* screen dims + viewport rect from
   the desired mode → run LoadMap (all view-derived dims computed) → when the
   loader thread finishes, `SetWindowPos` + `NewTAScreen(w,h)` + recreate the
   OFFSCREEN. So every derived quantity is coherent *before* the physical
   switch. [BINARY-VERIFIED]
4. **Viewport rect derivation is trivial and constant**: left=128, top=32
   always; right=W−1, bottom=H−33; hence viewW=W−128, viewH=H−64 (left HUD
   panel 128 px, top bar 32 px, bottom bar 32 px — all resolution-independent).
   Written in exactly ONE place, `0x4981C9..0x498237` inside `0x497F40`.
   [BINARY-VERIFIED]
5. **All tile-view / sweep dims derive inside LoadMap** from viewW/H px:
   `>>4` → `main+0x1423B/3F`, `>>5` → `0x14243/47`, `+0xC/+0x20` →
   `0x1424B/4F`, and the SORT_* buffers are allocated from them. A mid-game
   mode change (which stock TA cannot do — see §6) would leave them stale.
   [BINARY-VERIFIED]
6. **Mode enumeration** (`0x4B5370`): QI the IDirectDraw at `TAProgram+0x84`
   for **IID_IDirectDraw2** (`0x4FCCD8`), `EnumDisplayModes(0, NULL, list,
   callback 0x4B5330)`. The callback's ONLY filter is
   **`ddpfPixelFormat.dwRGBBitCount == 8`**; a post-pass `0x45E4C0` sorts
   ascending by (w,h) and drops anything **below 640×480**. No upper bound, no
   aspect check — but the list buffer holds **at most 100 modes** (0x4B0
   bytes, 12 B/entry) and the callback does NOT bounds-check. Our cnc-ddraw
   fork must serve ≤100 8-bpp modes. [BINARY-VERIFIED]
7. **The chosen resolution is broadcast to other players** (word W/H at
   `PlayerInfo+0x8B/+0x8D`, `PlayerInfo = *(PlayerStruct+0x27)`, via
   `REPORTER_PlayerInfo 0x450F90`, HAPI message type 0x20) — resolution is
   network-visible state, set from the battleroom mode picker.
   [BINARY-VERIFIED]

---

## 1. The bottom of the stack — `NewTAScreen` and the DDraw init

### 1.1 `NewTAScreen 0x4B5940` [BINARY-VERIFIED, name CORPUS (TADR MenuResolution.cpp calls it with `PUSH Height; PUSH Width`)]

stdcall(w, h), ret 8:

```c
TAProgram->curW /*+0xD4*/ = w;
TAProgram->curH /*+0xD8*/ = h;
DDrawDeviceCreateAndCaps((TAProgram->flagsF0 /*+0xF0*/ >> 1) & 1);  // bit1 = fullscreen
```

### 1.2 `DDrawDeviceCreateAndCaps 0x4B5510` [BINARY-VERIFIED, name CORPUS]

stdcall(int fullscreen), ret 4. Serialised by the `'NIAM'` interlock at
`0x52A4E8..F0`. Frees the old GDI objects (`TAProgram+0x44/48/4C`), then
`SetWindowPos(hwnd=TAProgram+0x40, 0,0, curW, curH, SWP_NOZORDER|SWP_NOMOVE=6)`
and branches:

- **fullscreen (arg=1)**: sets `+0xF0 |= 2`; `DirectDrawCreate(0, &TAProgram+0x84, 0)`
  (import thunk `0x49F710`) → `SetCooperativeLevel(hwnd, 0x53)` (`call [vt+0x50]`
  @`0x4B5613`; 0x53 = EXCLUSIVE|FULLSCREEN|ALLOWMODEX|ALLOWREBOOT) →
  **`SetDisplayMode(curW, curH, 8)`** — `call [vt+0x54]` @`0x4B5639`, result test
  at the known `0x4B563C` (TADR label `SetDisplayMode_callsite`). **bpp is the
  immediate constant 8** (`push 0x8` @`0x4B5626`) — TA can never ask for
  anything but 8-bpp. Then `CreateSurface` @`0x4B569C` (caps `0x218` =
  PRIMARYSURFACE|FLIP|COMPLEX, backbuffer count 1 → primary at `+0x88`),
  `GetAttachedSurface` @`0x4B56BD` (backbuffer → `+0x8C`), `CreateClipper`
  @`0x4B56DA` (→`+0x90`) + `SetHWnd` @`0x4B56EF` + `SetClipper` @`0x4B5706`,
  `CreatePalette(DDPCAPS_8BIT=4, TAProgram+0x214)` @`0x4B5726` (→`+0x94`) +
  `SetPalette` @`0x4B5735`.
- **windowed (arg=0)**: clears `+0xF0` bit1; `CreateCompatibleDC` +
  **`CreateDIBSection`** 8-bpp top-down `curW×curH` (bitmap → `+0x44`, DC →
  `+0x48`, bits wired into the primary context via `0x4C6A60(TAProgram+0x50, w,
  h, (w+3)&~3, bits)` — **DIB pitch is 4-aligned**), `SetWindowPos` to
  `HWND_NOTOPMOST`.

Callers of `0x4B5510` [BINARY-VERIFIED, full-image scan]:
`0x4B5924/0x4B592C` (= `ToggleFullscreenWindowed 0x4B5910`: flips to the *other*
mode based on `+0xF0` bit1), `0x4B5971` (NewTAScreen), `0x4B5C3B/0x4B5CF3`
(inside `CreateMainWindowAndDXInit 0x4B5980` — initial screen creation),
`0x4B6250` (fatal-error path `0x4B6230`: drop to windowed so `MessageBoxA` can
show, then `PostMessage(WM_DESTROY)`).

### 1.3 Who calls `NewTAScreen` — all three sites [BINARY-VERIFIED]

| Call site | w,h pushed | Meaning |
|---|---|---|
| `0x4980B7` | 640,480 const | game-entry handler `0x497F40`: force 640×480 for the loading screen (`loadgame2bg`) if not already there |
| `0x4983EA` | `main+0x37F1B/1F` | same handler, after load completes: **switch to the desired mode** |
| `0x491B0B` | 640,480 const | leave-game path (`0x491ADC` block): restore 640×480 for the front end |

Every site is bracketed by the same OFFSCREEN dance: free `*(main+0x37E1B)`,
`NewTAScreen`, then `OFFSCREEN = 0x4C69F0("OFFSCREEN", main+0x37E1F, main+0x37E23)`
→ store to `main+0x37E1B`. **The OFFSCREEN pointer changes identity on every mode
switch** — never cache it across one.

`GetTA_ScreenWidth 0x4B6700` / `0x4B6710` (TADR calls it `ViewportHeightGet`,
really *screen* height) are trivial getters of `TAProgram+0xD4/+0xD8`. [BINARY-VERIFIED]

---

## 2. The lifecycle — startup → front end → game entry → back

### 2.1 Startup [BINARY-VERIFIED]

`CoreStartupAndMainLoop 0x49E830`:
- `0x49E91C/0x49E93B`: `*(0x51F51A)=0x280; *(0x51F51E)=0x1E0` — the window
  creation params (`TAProgram+0x1FA/+0x1FE`) are **hard-coded 640×480**; the
  registry resolution plays no part at boot.
- fullscreen decision `0x49E8FD..0x49E955`: flags word `0x51F522`
  (`TAProgram+0x202`) bit0 = `~(*(0x51FB48)) & 1`, then `|= 0x3F2`. `0x51FB48`
  is 0 by default → fullscreen; the **`-d` command-line switch** sets it to 3
  (windowed debug) and **`-df`/`-dF`** to 2 (debug, still fullscreen) — handler
  `0x49F0B2`. This is the only windowed path.
- `CreateMainWindowAndDXInit 0x4B5980`: publishes `TAProgram` (`0x51F320` →
  `0x51FBD0`), copies `+0x1FA/+0x1FE` → `+0xD4/+0xD8` (`0x4B5ACB/0x4B5AD6`),
  assembles `+0xF0` from `+0x202` (bit0→bit1 fullscreen), creates the screen.
- The options loader `0x42F9A0` (called from `0x4273E8`, `0x47BB52`, `0x4913F6`)
  then fills `main+0x37F1B/1F` from the registry (§4). The front end stays at
  640×480 regardless.

Command line: `CmdlineArgsNormalize 0x49EE30`, switch jump table `0x49F494`
(index bytes `0x49F500`, chars `B C D E F H L N P R S T W`, case-insensitive).
**None writes `0x37F1B/0x37F1F`** — no resolution switch. [BINARY-VERIFIED]
totala.ini (`GetIniFileInt 0x49F5A0`, `[Preferences]` next to the exe) is read
only for `NoDirectSound`, `UseWindowsSound`, `UnitLimit`. [BINARY-VERIFIED]

### 2.2 Game entry — the state handler `0x497F40` [BINARY-VERIFIED]

Invoked via the state-machine function pointer `main+0x391F5` (set to `0x497F40`
at `0x490BB3`, `0x496C3D`, `0x496D75`, `0x496D9D`, `0x496DFF` — game-start and
return-from-menu transitions [sites verified, semantics INFERRED]). Flow, keyed
on `main+0x38D75` (bit0 = play screen initialised, bit1 = map load complete):

```
0x497F58  bit0 set? ──yes──► 0x498340 (skip init)
   │ no
0x497F5E..  tear down front-end GUI stack
0x498025  main+0x37E1F/23 = 640×480            ← logical dims for the LOADING screen
0x498044  if (curW,curH) != (640,480):
0x498060      free OFFSCREEN, SetWindowPos(640,480), NewTAScreen(640,480),
              OFFSCREEN = 0x4C69F0("OFFSCREEN", 640,480)
0x498109  load palettes/"guipal", draw "loadgame2bg" loading screen
0x4981A2  main+0x37E1F ← main+0x37F1B; 0x37E23 ← 0x37F1F   ← DESIRED mode becomes logical
0x4981C9  viewport rect + view W/H computed (§3)            ← from the DESIRED mode
0x49823D  palette init 0x4288D0 …
0x4982CA  CreateThread(0x497C70)               ← loader thread: LoadMap etc.;
          …                                       its LAST act sets 0x38D75 bit1 (0x497C5F)
0x49832D  0x38D75 |= 1  (bit0)
0x498342  bit1 (load done)? ──no──► return (handler re-runs next frame)
   │ yes
0x498362  if (GetTA_ScreenWidth,Height) != (main+0x37F1B,1F):
0x49838C      free OFFSCREEN, SetWindowPos(desired), NewTAScreen(desired W,H),
              OFFSCREEN = 0x4C69F0("OFFSCREEN", 0x37E1F, 0x37E23)   ← now = desired
0x49842A  0x467D70 (paint HUD panels), 0x496790 (timers), 0x4C2870
0x498445  state ← 6, handler ← 0x499200 (the in-play frame handler)
```

**Coherency guarantee**: the viewport rect, view W/H, and everything LoadMap
derives (§3) are computed from the *desired* mode while the machine still shows
the 640×480 loading screen; the physical `SetDisplayMode` is the *last* step.
There is no window where derived state and mode disagree on screen.

### 2.3 Leaving a game [BINARY-VERIFIED]

The `0x491ADC` block (inside the game-exit path): `SetWindowPos(640,480)` →
`NewTAScreen(640,480)` @`0x491B0B` → recreate OFFSCREEN at `0x37E1F/23` (reset
to 640×480 by the surrounding code) — the front end is always 640×480. TADR's
`MenuResolution.cpp` patches exactly these constants (`0x49802B`, `0x4980AD`
region) to run menus hi-res. [CORPUS]

---

## 3. Viewport rect & every derived dimension

### 3.1 The one write site — `0x4981C9..0x498237` [BINARY-VERIFIED]

Inside `0x497F40` only. With `W = main+0x37E1F`, `H = main+0x37E23` (copied from
the desired `0x37F1B/1F` at `0x4981A2/0x4981B8`):

```
0x4981C9  main+0x37E27 (left)   = 0x80            ← 128, CONSTANT (left HUD panel)
0x4981D9  main+0x37E2B (top)    = 0x20            ← 32,  CONSTANT (top bar)
0x4981EF  main+0x37E2F (right)  = W − 1           ← inclusive
0x498203  main+0x37E33 (bottom) = H − 0x21        ← H−33 inclusive ⇒ 32-px BOTTOM bar
0x49821D  main+0x37E37 (viewW)  = right−left+1  = W − 128
0x498237  main+0x37E3B (viewH)  = bottom−top+1  = H − 64
```

At 640×480: rect {128,32,639,447}, view 512×416. **The panel/bars are fixed
pixel sizes, not proportional and not read from UI resources** — at 1600×1200
the view is 1472×1136. There is no other writer of these six fields in the
binary (full-image scan of the displacements).

### 3.1b Every consumer of the DESIRED pair in the game-entry path [BINARY-VERIFIED 2026-09-09, phase 2 / G17b]

The full-image scan finds the displacements `+0x37F1B` (W) and `+0x37F1F` (H)
**19 times each**. **Eight** of those are the game-entry path inside `0x497F40`,
and they are the complete set of places the desired mode is *consumed* on the way
to a running game — which is what phase 2 has to redirect if the engine is to run
at `window / k` (GL UI renderer §13.1):

| VA | instruction | what it does |
|---|---|---|
| `0x4981A7` | `mov ecx,[eax+0x37F1B]` → `mov [eax+0x37E1F],ecx` | desired W becomes the **logical** screen W |
| `0x4981B8` | `mov edx,[eax+0x37F1F]` → `mov [eax+0x37E23],edx` | desired H becomes the **logical** screen H |
| `0x49836D` | `cmp eax,[ecx+0x37F1B]` after `0x4B6700()` | "is the physical W already right?" — `jne 0x49838C` |
| `0x498380` | `cmp eax,[ecx+0x37F1F]` after `0x4B6710()` | the same for H |
| `0x4983BF` | `mov ecx,[eax+0x37F1B]` | W for **`SetWindowPos`** alone (`call [0x4fc2f0]` at `0x4983D1`) |
| `0x4983B9` | `mov edx,[eax+0x37F1F]` | H for the same call, pushed first |
| `0x4983E2` | `mov edx,[eax+0x37F1B]` | W for **`NewTAScreen`** (`call 0x4B5940` at `0x4983EA`) |
| `0x4983DC` | `mov ecx,[eax+0x37F1F]` | H for the same call, pushed first |

So §3.1's rect and every dimension §3.2 derives come from `0x4981A7`/`0x4981B8`
alone; the physical switch is decided at `0x49836D`/`0x498380`, the window is
resized from `0x4983BF`/`0x4983B9` and **the mode itself is set from the separate
pair `0x4983E2`/`0x4983DC`**.

> **Corrected 2026-09-09 by a landing reviewer, and it matters.** This table
> first listed six sites and glossed `0x4983BF`/`0x4983B9` as "pushed for the
> `SetWindowPos` + `NewTAScreen` pair". They are not a pair: the disassembly
> reloads `main` at `0x4983D7` and reads both fields **again** at
> `0x4983DC`/`0x4983E2` for `NewTAScreen`. An implementation that redirected only
> the six would have resized the window and then handed `NewTAScreen` the
> player's unmodified mode — the one read that actually sets the physical
> screen. Verified against the pristine exe.

**The persistence hazard, and why the desired pair must not simply be
overwritten** [MEASURED 2026-09-09]. `REGISTRY_SaveSettings 0x430F00` writes back
**every** option from memory, taking W/H straight from `0x37F1B/1F`
(`0x430F27`/`0x430F43`), and §4.2 records that it is called from **~30
option-change sites**. An in-memory override therefore reaches the player's
registry the first time they touch *any* setting — the same class of bug as the
`ScrollSpeed` write-back a landing review caught on G13e. Two designs avoid it:
redirect the six reads above and never touch `0x37F1B/1F`, or wrap
`REGISTRY_SaveSettings` so it always writes the player's own pair. **Not decided
here**; recorded so that whichever is taken is taken deliberately.

### 3.1c The leave-game `SetWindowPos` is already inert under our fork [BINARY-VERIFIED 2026-09-09]

`0x491ADC..0x491B0B`, the block §2.3 describes, pushes the window resize and the
screen re-create back to back:

```
0x491AE2  push 4          ; uFlags  = SWP_NOZORDER, and nothing else
0x491AE4  push 0x1E0      ; cy = 480
0x491AE9  push 0x280      ; cx = 640
0x491AFB  call [0x4FC2F0] ; SetWindowPos
0x491B01  push 0x1E0 / push 0x280 / call 0x4B5940   ; NewTAScreen(640, 480)
```

**That call does nothing under our fork**, and has not since the fork existed.
`SetWindowPos` is IAT-hooked (`hook.c`), and `fake_SetWindowPos`
(`winapi_hooks.c`) returns TRUE **without calling through** whenever the target
is `g_ddraw.hwnd` and the flags do not carry all of
`SWP_NOSIZE|SWP_NOMOVE|SWP_NOZORDER` (`0x7`). The engine passes `0x4`, so
`(0x4 & 0x7) != 0x7` and the resize is swallowed.

**Consequence for phase 2.** [GL UI renderer](gui-renderer.html) §13.7 proposed
"a byte patch at `0x491AFB`" as the fix for the window shrinking when a game is
left, and G17b's gate row names it. **No engine patch is needed**: the actor
that actually resizes the window is `NewTAScreen(640, 480)` → the fork's own
`dd_SetDisplayMode`, which recomputes `g_ddraw.render.width/height` from
`g_config.window_rect` and then maxes them against the new mode (`dd.c`). The
window policy is therefore a *fork* concern — keep a configured client size
across a mode change — and not a patch on a call that is already a no-op.

### 3.1d Leaving a game at 1280x720 crashes, and it is not phase 2's doing [MEASURED 2026-09-09]

Driving `ARMOPT -> EXIT -> MAINMENU -> CHOICE1` out of a skirmish:

| game mode | window | k | teardowns | result |
|---|---|---|---|---|
| 1024x768 | 1024x768 | 1 | 3 (the G15d cycle walk) | clean, no `ErrorLog.txt` |
| 1280x720 | 1920x1080 | 1.5 | 1 | **Access Violation** at `0x79426297`, read of `0x06F18E29` |
| 1280x720 | 1280x720 | **1** | 1 | **the same crash, same IP, same fault address** |

**So `k` is exonerated** — the control at `k = 1` fails identically, which is why
it was run. What the two failing rows share is the **1280x720 mode**, and the
crash address is outside `TotalA.exe` (image `0x400000..0x520000`) in a wine
module, with `cdaudio` / `stop` / `open` / `settimeformat` MCI strings on the
stack. `NoDirectSound=1` and `cdmode`/`musicmode` = 0 were set on every run, so
the sound path is nominally off and this is **not diagnosed further here**.

Practical consequence, and it is the useful part: **phase 2's cycle exit must be
walked at a mode known to survive a teardown.** `--res 1024x768 --window
1536x1152` gives `k = 1.5` exactly on a mode with three clean teardowns on
record, and is the configuration to use rather than 1280x720.

### 3.2 LoadMap's derivations — `0x483610` [BINARY-VERIFIED]

With `ebp = main+0x141FB` (so `+0x40 = main+0x1423B` etc.), at `0x483BBF`:

```
0x483BD6  main+0x1423B = viewW >> 4     ; view width  in 16-px tiles (512→32)
0x483BE4  main+0x1423F = viewH >> 4     ; view height in 16-px tiles (416→26)
0x483BF2  main+0x14243 = viewW >> 5     ; in 32-px tiles (16)
0x483C00  main+0x14247 = viewH >> 5     ; (13)
0x483D42  main+0x1424B = (viewW>>4) + 0xC    ; sweep cols
0x483D33  main+0x1424F = (viewH>>4) + 0x20   ; sweep rows
```

and immediately allocates from them: `SORT_UNIT_LIST` = rows·cols·4
(`0x483D45`), `SORT_INDICES` = rows·4 (`0x483D5C`), `SORT_LINE_COUNT` = rows·2
(`0x483D72`). The screen fog grid (`main+0x1421F`) and `EYEBALL_MEMORY` are also
(re)built here. **These update coherently after a mode change only because a
mode change (in stock TA) always passes through game entry, which runs LoadMap
on the loader thread before the physical switch (§2.2).** Any injected mid-game
mode switch must re-run these derivations *and* reallocate the three SORT
buffers, or the row sweep will bin/draw with stale extents. [BINARY-VERIFIED
derivations; the mid-game caveat is INFERRED but the write sites are exhaustive]

### 3.3 The OFFSCREEN — `0x4C69F0(tag, w, h)` [BINARY-VERIFIED]

stdcall ret 0xC. Allocates `w*h + 0x30`: 0x30-byte header + pixels.
Header: `+0 = w`, `+4 = h`, **`+8 = pitch = w exactly (no alignment)**,
`+0xC = pixel base`, clip rect `+0x1C..+0x28 = {0, 0, w−1, h−1}`,
`+0x10 = 0x2710`, `+0x14 = −1`, `+0x2C` flags `|1 &~2`. Full-screen sized
(`0x37E1F × 0x37E23`), recreated on every mode change; the windowed-mode DIB
(§1.2) is the only 4-aligned-pitch surface.

### 3.4 HUD panel painting after a switch — `0x467D70` [BINARY-VERIFIED]

Called at `0x49842A` right after the switch: draws three GAF frames from the
side-indexed sequences — top bar `*(main+0x1481F + side*4)` at
x = frame.XPos+0x81, bottom bar `*(main+0x14833 + side*4)` at
y = frame.YPos + **GetTA_ScreenHeight() − 0x20** (anchored to the real screen
bottom), side panel `*(main+0x14847 + side*4)` at its own hotspot. So the
bottom bar tracks the mode; the panel art itself is fixed-size GAF frames —
how the art covers ≥1024-wide bars is an asset question, not engine code
[INFERRED — verify live at a wide mode].

---

## 4. Persistence — registry, exhaustively

### 4.1 The helpers [BINARY-VERIFIED]

- **`0x4B6880`** raw accessor, stdcall ×6 `(subkey, valueName, buf, &size,
  regType, direction)`, ret 0x18: `RegCreateKeyExA` chain
  `HKCU\Software` → `\Cavedog Entertainment` → `\<subkey>` (sam = KEY_READ
  `0x20019` for reads, `0x20006|0x13` for writes), then `RegQueryValueExA`
  (direction=1) or `RegSetValueExA` (direction=0). Returns 1 on success (also on
  `ERROR_MORE_DATA`).
- **`REGISTRY_ReadInteger 0x4B69D0`** stdcall(subkey, name, out) ret 0xC —
  4-byte read.
- **`REGISTRY_WriteInteger 0x4B6A50`** stdcall(subkey, name, value) ret 0xC —
  REG_DWORD, value by value.
- `0x4B6A20` = string write (REG_SZ), `0x4B6A00` = binary write; `0x42F980` =
  string read (used for `language`, `Nickname`, …).

### 4.2 Resolution values [BINARY-VERIFIED]

Subkey **`Total Annihilation`** (string `0x5032E8`) ⇒ full path
`HKCU\Software\Cavedog Entertainment\Total Annihilation`:

| Value | Type | Loaded at | Stored to | Default (written back if absent) |
|---|---|---|---|---|
| `DisplaymodeWidth` (`0x5046F4`) | DWORD | `0x42FA1B`, store `0x42FA2E` | `main+0x37F1B` | 0x280 (`0x42FA3C`) |
| `DisplaymodeHeight` (`0x5046E0`) | DWORD | `0x42FA70`, store `0x42FA83` | `main+0x37F1F` | 0x1E0 (`0x42FA91`) |
| `Interface Type` (`0x504708`) | DWORD | `0x42F9C0` | `main+0x37EFA` (clamped ≤1) | 0 |
| `DisplaymodeDepth` (`0x504384`) | DWORD | `0x430E27` | — | easter egg only: `==0x100` && `Games==1` ⇒ `main+0x37F2F |= 2` (`0x430E68`) — **bpp is never configurable** |

- **Loader** = `0x42F9A0` (one giant function reading every option;
  `REGISTRY_ReadInteger` fails ⇒ default stored to memory *and* written back to
  the registry). **The W/H are used unclamped** — a registry value of 5000×5000
  goes straight into `0x37F1B/1F`. (TADR's patch labels
  `DisplayModeMinWidth1024RegAddr 0x42FA2E` etc. name these instructions/
  immediates as patch points. [CORPUS])
- **Saver** = `REGISTRY_SaveSettings 0x430F00`: writes back every option from
  memory, `DisplaymodeWidth/Height` read from `0x37F1B/1F` at `0x430F27/0x430F43`.
  Called from ~30 option-change sites, incl. both mode pickers (§5).
- The other registry cluster in `CoreStartupAndMainLoop` (`0x49EA8D..0x49ED17`)
  is the **AudioCD shell key** (HKLM `SOFTWARE\Classes\AudioCD\shell`) — not
  resolution. The `0x4B5100` HKLM reader is the **DirectX version check**
  (`Software\Microsoft\DirectX`, `Version`/`InstalledVersion`). Ruled out.
  [BINARY-VERIFIED]

### 4.3 The CavedogLibrary window settings (separate, debug-only) [BINARY-VERIFIED]

`0x4E1B10` reads `HKCU\Software\Cavedog Entertainment\CavedogLibrary\
PerformanceSettings`: `DisplayInWindow` (→`0x529E64`, default 1),
`DisplayInDebugger`, `RaisePriority`, … — the shared Cavedog library's own
window plumbing (window placement save/restore `0x4E3080` uses `WindowPositions\`
+ `Width`/`Height` `0x50DE24/0x50DE1C`). These affect the debug/windowed window
only, not the game's mode selection.

---

## 5. The UI flows — who writes the desired resolution

Only **four** writers of `main+0x37F1B/0x37F1F` exist beyond the loader/defaults
[BINARY-VERIFIED, from the full displacement scan]:

1. **Front-end video-mode slider** — `VISUALS.GUI` + `SELVMODE.GUI`
   (`0x45E5E0` builds it; slider `VIDSLDR`, label `VIDVAL`, format
   `"%d X %d"` `0x506864`). Slider callback **`0x45BBF0`** (stdcall ret 8):
   index = pos·(count−1) scaled → `modes[idx]` → writes `0x37F1B/1F` at
   `0x45BC88/0x45BC97` **live on every slider move**. The dialog handler
   `OnCommand_VISUALRT_GUI 0x45E100` [CORPUS name]:
   - `UNDO` (`0x506998`) → `0x45CAE0`: restores the stash `0x512F4D/0x512F51`
     into `0x37F1B/1F` — **skipped when in-game** (`main+0x2A44 & 4`).
     (Stash filled on dialog entry; writer uses register addressing, site not
     pinned. [INFERRED])
   - `RESTORE` (defaults): `0x37F1B/1F = 640×480` at `0x45E3AD/0x45E3BD`
     (and the twin default-reset at `0x45C79D/0x45C7AD`) — again skipped
     in-game.
   - The in-game variant `VISUALRT.GUI` (`main+0x37EBE & 1`) **has no video-mode
     selector** — the mode-list branch of `0x45E5E0` runs only for the front-end
     GUI. **Stock TA cannot change resolution mid-game.**
2. **Battleroom mode listbox** — OnCommand `0x4461D0` (gadget `MODES`
   `0x505C38`): selected row → `modes[idx]` → `0x37F1B/1F` at
   `0x4462C7/0x4462D6` **and** the network words `PlayerInfo+0x8B/+0x8D`
   (`0x4462E2/0x4462F0`) → `REPORTER_PlayerInfo 0x450F90` (broadcast, msg 0x20)
   → `REGISTRY_SaveSettings 0x430F00`.
3. **Battleroom mode cycle** — `0x446310`: rebuilds the list, finds the current
   mode by matching `PlayerInfo+0x8B/8D`, steps ±1 with wrap (direction from
   `[main+0x531]+0x37 == 2`), writes PlayerInfo, broadcasts, then `0x37F1B/1F`
   at `0x44641F/0x44642E`. (No registry save on this path.)
4. **The options loader / defaults** (§4.2).

In every case the write is to the *desired* pair only; the physical switch
happens at the next pass through `0x497F40` (§2.2) — i.e. when the game launches.

---

## 6. Mode enumeration — `0x4B5370` + callback `0x4B5330`

### 6.1 The list object [BINARY-VERIFIED]

Transient — built fresh on each dialog open / cycle press, freed on close
(`0x4461E6..0x446204`). Layout (alloc tags in quotes):

```
list        ("SELECT VIDEO MODE", 0x20 B):  +0 count, +4 modes*, +0x14 names*
modes       ("DISPLAY MODES", 0x4B0 B):     count × {int w, int h, int refresh}   ← 12 B/entry, MAX 100
names       ("AVAILABLE MODES", count×0x100)
```

### 6.2 `0x4B5370` — stdcall(list*), ret 4 [BINARY-VERIFIED]

- **Windowed** (`TAProgram+0x44` non-null ⇒ the DIB exists): hard-coded list —
  640×480, 800×600, 1024×768, plus 1280×1024 if desktop ≥1280×1024
  (`GetSystemMetrics(0/1)`), plus 1600×1200 if desktop ≥1600×1200. No ddraw
  involvement.
- **Fullscreen**: `QueryInterface(IID_IDirectDraw2)` — GUID at `0x4FCCD8` =
  `{B3A6F3E0-2B43-11CF-A2DE-00AA00B93356}` — then
  **`EnumDisplayModes(flags=0, pDDSD=NULL, ctx=list, cb=0x4B5330)`**
  (`call [vt+0x20]` @`0x4B54EB`), then `Release`.

### 6.3 The callback `0x4B5330` — stdcall(DDSURFACEDESC*, void* list), ret 8 [BINARY-VERIFIED]

```c
if (desc->ddpfPixelFormat.dwRGBBitCount /*+0x54*/ == 8) {   // ← the ONLY filter
    e = modes + count*12;
    e[0] = desc->dwWidth;    // +0x0C
    e[1] = desc->dwHeight;   // +0x08
    e[2] = desc->+0x18;      // dwRefreshRate union (flags=0 ⇒ 0); never used
    count++;                 // ← NO bounds check against the 100-entry buffer
}
return DDENUMRET_OK;         // always continue
```

### 6.4 Post-filter `0x45E4C0` — stdcall(list), ret 4 [BINARY-VERIFIED]

Bubble-sorts ascending by width then height, then deletes every entry with
**w < 0x280 or h < 0x1E0** (immediates at `0x45E580`/`0x45E589` — TADR's
`DisplayModeMinHeight768EnumAddr` patch point [CORPUS]). No de-dup, no upper
cap, no aspect filter, no bpp re-check.

**What our cnc-ddraw fork must serve for a custom mode to be selectable**:
report it from `EnumDisplayModes` with `dwRGBBitCount == 8`, dimensions
≥ 640×480, and keep the total number of 8-bpp modes **≤ 100** (see §7.1).
Refresh-rate variants are harmless only if they don't multiply entries past the
cap (flags=0 normally suppresses them; entries differing only in refresh would
otherwise appear as duplicate rows — the matcher compares w/h only).

---

## 7. Risks & watch-list for a non-640×480 mode

1. **Mode-list buffer overflow** [BINARY-VERIFIED]: "DISPLAY MODES" is a fixed
   `0x4B0`-byte MEM alloc = 100 entries; callback `0x4B5330` appends unchecked.
   \>100 8-bpp modes from our ddraw ⇒ heap corruption in the options screen.
   Cap the served list.
2. **Registry values are unclamped** [BINARY-VERIFIED]: whatever we write to
   `DisplaymodeWidth/Height` is what `SetDisplayMode` gets (mod the §6 UI
   filter, which only gates the *picker*). Setting the registry directly is a
   legitimate side-channel to force any mode our ddraw accepts — bypasses the
   100-mode UI entirely.
3. **Mid-game switches (injection) are unsupported by stock flow**: the §3.2
   LoadMap derivations + SORT allocations + fog grid + `EYEBALL_MEMORY` update
   only via game entry. An injected `NewTAScreen` mid-game must also rewrite
   `0x37E1F/23`, the rect block (`0x4981C9` equivalents), rerun the `>>4/>>5`
   dims, realloc the three SORT buffers, invalidate the fog grid
   (`main+0x14281` bit3), and recreate the OFFSCREEN — or crash/misdraw.
   [INFERRED from exhaustive write-site scans]
4. **OFFSCREEN pointer identity** changes at every switch (`main+0x37E1B`
   freed + realloced at `0x49838C..0x498425`, `0x498060..`, `0x491ADC..`) —
   any hook caching the pixel base must re-read it per frame. Pitch == width
   exactly (§3.3); the windowed DIB is 4-aligned instead — don't mix the two
   assumptions.
5. **HUD art coverage at wide modes**: top/side/bottom panels are fixed GAF
   frames drawn once (`0x467D70`), bottom bar anchored to screen height. Whether
   the art tiles/stretches beyond its native width is asset-side — check live at
   1024+ for garbage strips right of the top bar and below the side panel.
   [INFERRED]
6. **Front end is atom-locked to 640×480** (forced at `0x498025..0x498108` and
   `0x491ADC..`): a taller default mode never shows in menus. TADR's
   MenuResolution patch is the precedent if we want hi-res menus. [CORPUS]
7. **Resolution is network-visible** (`PlayerInfo+0x8B/8D`, u16 each,
   broadcast type 0x20): fine up to 65535 px, but other clients (and TADR
   tooling) read these — keep them truthful if we resize behind the engine's
   back. [BINARY-VERIFIED fields; consumer behaviour on other clients INFERRED]
8. **bpp is frozen at 8** (`push 0x8` @`0x4B5626`; `DisplaymodeDepth` is an
   easter egg, §4.2): our ddraw must accept `SetDisplayMode(w,h,8)` for any
   mode we add.

---

## Appendix — address & offset tables

### Functions

| VA | Role | Convention |
|---|---|---|
| `0x4B5510` | **DDrawDeviceCreateAndCaps** — SetCoopLevel `0x53` @`0x4B5613`, `SetDisplayMode(w,h,8)` @`0x4B5639` (test @`0x4B563C`), surfaces/clipper/palette; windowed DIB path | stdcall(int fullscreen), ret 4 |
| `0x4B5940` | **NewTAScreen** — stores `TAProgram+0xD4/D8`, re-inits screen | stdcall(w,h), ret 8 |
| `0x4B5910` | ToggleFullscreenWindowed (callers `0x417B2E`, `0x45172D`, `0x4519CC`, `0x4998BB`) | void |
| `0x4B6700`/`0x4B6710` | GetTA_ScreenWidth / ScreenHeight (`TAProgram+0xD4/+0xD8`) | void→eax |
| `0x4B5980` | CreateMainWindowAndDXInit — publishes `TAProgram=0x51F320`, `+0x1FA/FE`→`+0xD4/D8` @`0x4B5ACB/AD6` | (params*) |
| `0x4B5370` | **BuildVideoModeList** — windowed: hardcoded; fullscreen: QI IDirectDraw2 (`0x4FCCD8`) → EnumDisplayModes @`0x4B54EB` | stdcall(list*), ret 4 |
| `0x4B5330` | **EnumModesCallback** — accept iff bpp==8; entry {w,h,refresh} | stdcall(DDSD*,ctx), ret 8 |
| `0x45E4C0` | mode-list sort + drop <640×480 (imms `0x45E580/0x45E589`) | stdcall(list*), ret 4 |
| `0x45E5E0` | visual-options dialog build (VISUALS/VISUALRT/SELVMODE.GUI) | stdcall(int selvmode), ret 4 |
| `0x45BBF0` | VIDSLDR slider callback → writes `0x37F1B/1F` @`0x45BC88/97` | stdcall, ret 8 |
| `0x45E100` | OnCommand_VISUALRT_GUI (UNDO→`0x45CAE0`, RESTORE 640×480 @`0x45E3AD/BD`) | stdcall(gui*), ret 4 |
| `0x4461D0` | battleroom MODES listbox OnCommand — writes @`0x4462C7/D6` + PlayerInfo @`0x4462E2/F0` | stdcall(gui*), ret 4 |
| `0x446310` | battleroom mode cycle — writes @`0x44641F/2E` | cdecl-ish, ret |
| `0x450F90` | REPORTER_PlayerInfo — broadcast PlayerInfo (msg 0x20, 0xB9 B) | void |
| `0x42F9A0` | **registry options load** (res @`0x42FA0C..0x42FAB6`; tail to `0x430EFF`) | void |
| `0x430F00` | **REGISTRY_SaveSettings** (res reads @`0x430F27/0x430F43`) | void |
| `0x4B69D0`/`0x4B6A50`/`0x4B6A20` | REGISTRY_ReadInteger / WriteInteger / WriteString | stdcall, ret 0xC |
| `0x4B6880` | raw HKCU\Software\Cavedog Entertainment\<subkey> accessor | stdcall ×6, ret 0x18 |
| `0x497F40` | **play-screen state handler** — §2.2 choreography; viewport writes `0x4981C9..0x498237`; desired-mode switch `0x498362..0x498429` | state fn |
| `0x497C70` | loader thread proc — sets `0x38D75` bit1 @`0x497C5F` when done | thread |
| `0x483610` | LoadMap — view-tile dims @`0x483BBF..0x483C00`, sweep dims @`0x483D27..0x483D42`, SORT allocs | — |
| `0x491ADC` | leave-game 640×480 restore (NewTAScreen @`0x491B0B`) | — |
| `0x4C69F0` | OFFSCREEN alloc(tag,w,h) — pitch=w, clip {0,0,w−1,h−1} | stdcall, ret 0xC |
| `0x467D70` | HUD panel painter (bottom bar anchored to screen H − 0x20) | void |
| `0x4B6230` | fatal path: drop windowed → MessageBoxA → WM_DESTROY | stdcall(msg), ret 4 |
| `0x49EE30` | CmdlineArgsNormalize — table `0x49F494`/`0x49F500`; `-d`(=3 windowed)/`-df`(=2) @`0x49F0B2`→`0x51FB48` | stdcall, ret 8 |
| `0x49E830` | CoreStartupAndMainLoop — 640×480 boot @`0x49E91C/3B`; fullscreen bit @`0x49E8FD..55` | — |
| `0x4E1B10` | CavedogLibrary PerformanceSettings loader (DisplayInWindow→`0x529E64`) | cdecl |

### Data

| Location | What |
|---|---|
| `TAProgram = *(0x51FBD0) = 0x51F320` | +0x40 hwnd · +0x44/48 DIB bmp/DC (windowed) · +0x84 IDirectDraw* · +0x88 primary · +0x8C backbuffer · +0x90 clipper · +0x94 palette obj · **+0xD4/+0xD8 current screen W/H** · +0xF0 bit1 = fullscreen · +0x1FA/+0x1FE creation-request W/H · +0x202 creation flags (bit0 fullscreen) · +0x214 palette entries |
| `main+0x37F1B/0x37F1F` | **desired W/H** (registry `DisplaymodeWidth/Height`) |
| `main+0x37E1F/0x37E23` | logical screen W/H (OFFSCREEN + viewport source; = desired once in-game, 640×480 in front end) |
| `main+0x37E27..0x37E33` | viewport rect {128, 32, W−1, H−33} — sole writer `0x4981C9..` |
| `main+0x37E37/0x37E3B` | view W/H px = W−128 / H−64 |
| `main+0x37E1B` | current OFFSCREEN* (freed/realloced each switch) |
| `main+0x1423B/3F`, `0x14243/47`, `0x1424B/4F` | view tiles 16px / 32px / sweep dims — LoadMap-only writers (§3.2) |
| `main+0x37EFA` | Interface Type (0/1) · `main+0x37EBE` bit0 = RT (in-game) GUI variant |
| `main+0x38D75` | bit0 play-screen init'd · bit1 map-load complete · bit2/3 sim-thread sync |
| `main+0x2A44` | bit0 net-active · bit2 in-game (gates prefs GUI + res restore skip) |
| `main+0x391F1/0x391F5` | game-state id / handler ptr (6/`0x499200` in play; `0x497F40` = entry) |
| `PlayerStruct+0x27 → PlayerInfo` | **+0x8B/+0x8D u16 screen W/H** (network-shared) |
| `0x512F38..0x512F59` | options-dialog UNDO stash (res dwords at `0x512F4D/0x512F51`) |
| `0x51FB48` | debug-mode from `-d`/`-df` (0 none, 2 dbg-fullscreen, 3 dbg-windowed) |
| `0x529E64` | CavedogLibrary `DisplayInWindow` |
| `0x4FCCD8` | IID_IDirectDraw2 |

### Strings (VA → text)

`0x5046F4` DisplaymodeWidth · `0x5046E0` DisplaymodeHeight · `0x504384`
DisplaymodeDepth · `0x504708` Interface Type · `0x5032E8` Total Annihilation ·
`0x509ED0` Software · `0x509EB8` Cavedog Entertainment · `0x505C60` SELECT
VIDEO MODE · `0x505C50` DISPLAY MODES · `0x505C40` AVAILABLE MODES · `0x505C38`
MODES · `0x506A78` SELVMODE.GUI · `0x506874` VIDSLDR · `0x50686C` VIDVAL ·
`0x506864` "%d X %d" · `0x509644` loadgame2bg · `0x5091D4` OFFSCREEN ·
`0x50DDF4` Software\Cavedog Entertainment · `0x50DD3C` DisplayInWindow ·
`0x5098A0` "%s\totala.ini".

### Ruled out

- Command line: no resolution switch (full jump-table decode; only `-d`/`-df`
  touch display-adjacent state).
- totala.ini: `[Preferences]` NoDirectSound / UseWindowsSound / UnitLimit only.
- `0x49EA8D` registry cluster = AudioCD shell; `0x4B5100` = DirectX version
  check; `0x4E3080` = generic window-placement save (debug window).
- `DisplaymodeDepth` = easter-egg gate, not bpp.
