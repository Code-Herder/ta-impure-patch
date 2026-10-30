# Input and the camera

The mouse object and thread, the cursor chain, clicks, the keyboard sampler, the camera and the viewport.

*Part of the [engine map](../exe-reverse-engineering.md). A function's entry is the section whose heading names it — the [address index](address-index.html) lists them: a new fact about a function extends its entry, in place; a new section is for an area nobody has mapped yet.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

## The camera module — mapped by us

[MEASURED 2026-09-03, this project — disassembly of the pristine build plus live reads, not
from any vendor corpus. Established while making the camera's range follow the zoom
(`gpu-status.md` §2.3c); the community corpora name only `0x41C3C0`, and name it for its tail.]

**"Eye"** is the world position the viewport's top-left corner shows. **"View"** is `W`/`H` at
`main+0x37E37`/`+0x37E3B`. The module keeps *two* positions: the eye, and a **scroll target**
the eye is eased toward.

Impure's replay reads the view at its frame visit, on the game thread, to fit a recorded seat's
camera to this screen (`eye = cam − view/2`), and its Follow takes a player's camera move from the
scroll target alone: the rows below have every eye writer copy the eye into the target, the
centre-ons and the stepper's follow source write it, and `0x41C3C0` never touch it
([perspective plan, 4c-2 as built](../tadr-port/demo-recorder-perspective-streams.html#4c-2-as-built)).

| VA | What it is |
| --- | --- |
| **`0x41C3C0`** | **The eye clamp.** `eyeX = clamp(eyeX, 0, extentW − W)`, the same for Y, `extent` the **scroll extent** `main+0x1422B`/`+0x1422F` (read at `0x41C3C6`/`0x41C3DA`) — the map less 32 and 128 px, not the map's size ("Engine defects we patch") — then `0x466B70(main+0x142CB)` to refill the minimap's view rect — every path through it ends in that one call. TADR calls it `ScrollMinimap`, which describes the tail rather than the job. **12 call sites:** `0x41C59F`, `0x41C898`, `0x41C9CC`, `0x41CC16`, `0x41CC37`, `0x41CC52`, `0x41CDE1`, `0x41D054`, `0x41D184`, `0x41D26B`, `0x41D319`, `0x41D459`. **With `zoom.on` it never runs:** `tagpu_zoom`'s `leaf_call` replaces it on every call, and the replacement clamps to the camera's centre range `[−W/2, extent − W/2]` on draws whose ground is ours and to this function's own `[0, extent − W]` otherwise (`gpu-status.md` §2.3c). Both end at the extent because stock never shows a pixel past it: the strip between the extent and the tile map's own size is unbuildable (`0x4833B0`, "LoadMap's feature placement, and a saved game") and on 9 stock maps holds the editor's blank tile, and our edge passes fold the mirrored map edge at the same line (`gpu-status.md` §2.90, "Where the map ends"). **Measured live**, 1024x768, `W = 896`, `H = 704`: 2026-09-23 on Two Continents (map 10752 x 12800, extent 10720 x 12672), each edge scrolled into (`keys mouse:1023,400` and the other three), the eye settles at `x = −448` and `y = −352` against the left and top edges at 0.25x, 1x and 8x alike — the map's corner on the view-centre pixel; 2026-10-25 on Painted Desert (extent 9216 x 9216) at `x = 8768` and `y = 8864` against the right and bottom edges, `extent − W/2` and `extent − H/2`, and on Assault on Suburbia (extent 8064 tall) at `y = 7712`. |
| `0x41C450` | The same clamp shape for the *target* pair — and it has **no callers**. Dead code; an `E8`/`E9` scan of `.text` finds nothing pointing at it. |
| `0x41C4C0` | `SetCamera(x, y, smooth)`, stdcall, `ret 0xC`. **Two arms** [DISASSEMBLED 2026-09-23]: with `smooth == 0` (`0x41C4C7 je 0x41C574`) it writes the **eye**, calls `0x41C3C0` (`0x41C59F`) and copies the clamped eye into the target; with `smooth != 0` it writes the **target** (`0x41C4D6`, `0x41C4E6`) and clamps it **inline** against `[0, extent − view]` (`0x41C4F1`, `0x41C505`) in the block `0x41C4EC..0x41C5C5`, never calling `0x41C3C0`. The eye arm falls into the tail `0x41C5C6`: `mov eax, ds:0x511DE8`, clear fog bit 3 (`and word [eax+0x14281], 0xFFF7`), `pop esi`, `ret 0xC`. The smooth block leaves by three routes with the same clear and the same `pop esi; ret 0xC`: its own early exits at `0x41C554` and `0x41C570`, and `0x41C55A`'s `jle 0x41C5CB`, which enters the tail past its `mov`. Callers: `0x495C68`, `0x495E11` (the screenshot tiler `0x495A30`), `0x497060`, `0x4978C9` (game-screen entry / load). **The smooth arm is unreachable** [VERIFIED 2026-09-23]: all four callers push `smooth = 0` (`push 0` at `0x495C5E`, `0x495DCB`, `0x49703E`, `0x4978C5`, each the first of the three pushes), and `0x41C4C0` occurs nowhere in the image as a 32-bit value — no function pointer, no table — so `0x41C4C7`'s `je` is always taken. `tagpu_zoom` therefore leaves the block alone. |
| **`0x41CA10`** | **The per-frame camera stepper.** One caller, `0x495599`. It first picks a follow source (next row); with none it takes `je 0x41CB4A`. Where eye ≠ target it sets bit 1 of `main+0x142F1` ("camera moved"), **clears bit 3 of `main+0x14281`** — the screen fog grid's own is-current flag — then moves the eye *halfway* toward the target, capped at ±`0x140` (320 px) per axis per frame, and hands the result to `0x41C3C0`. It never writes the target. *[CORRECTED 2026-09-10: this row said `0x41CA30`, which is mid-function — the byte there is the LAST byte of the 7-byte `mov WORD PTR [ecx+0x1434B], ax` at `0x41CA2A`, and an `E8`/`E9` scan of `.text` finds nothing calling it. `0x41CA10` is the prologue (`mov ecx, ds:0x511DE8; push ebp; push esi; xor ebp, ebp`).]* |
| **`0x41CA1A`…`0x41CA8D`** | **The follow selection, and it takes the FIRST of three sources that is set** [MEASURED 2026-09-10]: the countdown `main+0x1434B`, then the object `main+0x142F7` (position at `+0x4`), then the unit `main+0x142F3` = `CameraToUnit` (position at `+0x6A`). A unit whose `[+0x110] & 0x10000000` has gone away is not followed and the stepper **releases all three itself** at `0x41CA69`. The countdown is *decremented* here (`0x41CA29`, unlocked `dec` on a u16), which is what makes it expire. |
| `0x41CAC7`/`0x41CAD2` | Inside the stepper: the camera-**follow** target written from the followed source as `pos − view/2`, then clamped **inline** at `0x41CAF7` to `[0, extent − view]` (the extent read at `0x41CADD`/`0x41CAE9`). |
| `0x41CAF7`…`0x41CB43` | **The extent of that inline clamp**, and the whole of it is replaceable: it is reached only on a frame that follows something, because `0x41CA8F` jumps straight past it to `0x41CB4A` when none of the three sources is set, and nothing jumps INTO it — an `E8`/`E9`/`Jcc` scan of `.text` finds no target inside the range. `0x41CB44` is the instruction after it. Its tail clears fog bit 3 at `0x41CB3B`. G18-7's `zoom_follow_clamp` lands on `0x41CAF7` and rejoins at `0x41CB44`, which is what makes Ctrl+C arrive at any zoom. |
| **`0x41C390`** | **Release the camera follow** — a leaf with no arguments: `main+0x1434B` (u16) = 0, then `main+0x142F3` = 0, then `main+0x142F7` = 0, `ret`. Two callers, `0x4174FD` (the camera-track toggle on bit 1 of `main+0x14373`) and `0x48D709` (centre-on-unit, which releases and then calls the smooth centre-on `0x41C8E0`). The same three stores are **inlined** at `0x41C2B0`, `0x41CC60`, `0x41D091`, `0x41D1C4` and `0x41D406`. |
| `0x41C2B0` | Camera reset: `rep stosd` of `0x17` dwords from `main+0x142F3` (i.e. `0x142F3`…`0x1434E`, the whole camera block — follow slots, eye, target and countdown), preserving the byte at `main+0x1434D` across it. |
| `0x41C2E0` | `SetFollowUnit(dir)` — `0x48C190(currentFollow, dir)` picks the next/previous unit and the result is stored into `CameraToUnit`. Callers `0x48B074`, `0x4964E7` (dir 0), `0x4964F3` (dir 1) — the cycle-through-units keys. |
| `0x41C310` | Scans the unit list for one matching a per-player bitmask and stores it into `CameraToUnit`. Sole caller `0x496409`, in the command dispatch under the string `CTRL_C` — **so Ctrl+C makes the camera FOLLOW the commander, it does not merely centre on it** (confirmed live 2026-09-10: `main+0x142F3` goes from 0 to a unit pointer). |
| `0x41C6F0` | **The camera shake step** [DISASSEMBLED 2026-09-23, name INFERRED]: while bit 0 of `main+0x1434E` is set and the countdown `main+0x14333` is positive, adds a random offset scaled by `+0x14337`/`+0x1433B` over `+0x1432F` to the eye (`0x41C784`, `0x41C797`) and decrements the countdown; clears the bit when it runs out. Three call sites, `0x41CC11`, `0x41CC32`, `0x41CC4D`, each followed by `call 0x41C3C0` (`0x41CC16`, `0x41CC37`, `0x41CC52`), so a shaken eye is always clamped. |
| `0x41C7C0` | **Centre the camera on a world point**, `(x, y, smooth)`, stdcall, `ret 0xC` [DISASSEMBLED 2026-09-23]. Subtracts half the view from the point, then the same two arms as `SetCamera`: `smooth == 0` (`0x41C7F1 je 0x41C87A`) writes the eye and calls `0x41C3C0` at `0x41C898`; `smooth != 0` stores the target at `0x41C7F7`/`0x41C802` and clamps it inline in the block `0x41C808..0x41C879`; its paths out reach the tail `0x41C8BF` (the fog-bit clear, `pop esi`, `ret 0xC`) — `0x41C878 jmp 0x41C8B9` through the last target store, `0x41C876 jle 0x41C8C4` past the tail's `mov` — or the block's own early exit `0x41C862..0x41C871`, which does the same clear and the same `pop esi; ret 0xC`. The eye arm `0x41C87A..0x41C8BE` lies inside the same address range, entered by `0x41C7F1`'s `je`, and runs through `0x41C3C0` to the tail. One caller, `0x463FEA`, with `smooth = 1` and a unit's position (`+0x6C`/`+0x74`). |
| `0x41C8E0` | **Centre the camera on an object**, `(pos*, smooth)` with a 16.16 position record, stdcall, `ret 8` [DISASSEMBLED 2026-09-23]. The same two arms: `smooth == 0` (`0x41C924 je 0x41C9AE`) writes the eye and calls `0x41C3C0` at `0x41C9CC`; `smooth != 0` stores the target at `0x41C92A`/`0x41C935` and clamps it inline in the block `0x41C93B..0x41C9AD`, tail `0x41C9F3` (the fog-bit clear, `pop edi`, `pop esi`, `ret 8`), reached through the last target store `0x41C9ED` or past the tail's `mov` at `0x41C9F8`; its own early exit `0x41C995..0x41C9A5` does the same clear and pops. The eye arm `0x41C9AE..0x41C9F2`, entered by `0x41C924`'s `je`, runs through `0x41C3C0` to the tail. Callers `0x48D5A0` (the unit selected and centred, `smooth = 1`) and `0x48D714` (centre-on-unit, after `0x41C390` releases the follow). |
| `0x41CB5F` | Inside the stepper, the eye≠target branch for X (`0x41CBCF` is Y's): `or word [ecx+0x142F1], di` with `di = 2` — the minimap's dirty bit every engine eye writer sets and the command apply sets too (row "why we set `main+0x142F1` bit 1") — then the fog-bit clear at `0x41CB6B` (`0x41CBDB`), the half step toward the target, and `0x41C6F0` and `0x41C3C0` (`0x41CC11`..`0x41CC52`). The target is read and never written (`0x41CB74..0x41CC16`). DISASSEMBLED 2026-09-24. |
| `0x41CE90`…`0x41D060` | The scroll poll — see the table below. One caller, `0x496976`. *[CORRECTED 2026-09-04: this said `0x41CF10`, which is not an instruction boundary — `0x41CF0E` is `lea ebp,[esi+0x64]`.]* |
| `0x466B70` | Fills a RECT with the minimap's view box from the eye and the view size in map cells (`main+0x1423B`/`+0x1423F`). Pure computation; its only two call sites are inside `0x41C3C0`. **The two redirects measured on the presented frame 2026-09-22** (1024x768, the minimap occupying `(0,0 128x128)`): the yellow box's bounding box is `8x7` px at zoom 1.0 and `14x13` px at 0.513, so the rect does follow `1/z` — the ±1 px against the ideal `15.6x13.6` is the 1-px border on a box this small. |
| **the eye writers that copy the eye into the target** [VERIFIED by disassembly 2026-09-12; all nine 2026-09-24] | Every caller of `0x41C3C0` outside the stepper — nine of the twelve: `0x41C59F`, `0x41C898`, `0x41C9CC`, `0x41CDE1`, `0x41D054`, `0x41D184`, `0x41D26B`, `0x41D319`, `0x41D459` — sets the eye, calls the clamp, and then copies the clamped eye into the scroll target itself and clears fog bit 3; the stepper's three calls (`0x41CC16`, `0x41CC37`, `0x41CC52`) never write the target. Three of them in detail: the `call 0x41C3C0` at `0x41C59F` (inside `SetCamera 0x41C4C0`; the eye-writing arm begins at `0x41C574`, `mov ecx,ds:0x511de8`), at `0x41CDE1`, and at `0x41D054` (the scroll poll, whose eye store is `0x41D037`, `mov [eax+0x1431f],edi`); the copies start right after each call, at `0x41C5A4`, `0x41CDE6` and `0x41D059`, each with a fresh `mov eax,ds:0x511de8`; the third's next instructions, `0x41D05E mov ecx,[eax+0x1431f]` and `0x41D064 mov [eax+0x14327],ecx`, are the read of the eye and the store into the target (the zoom module's comment cites the path by that read, `0x41D05E`). This is why `zoom_eye_clamp` may leave the target alone: every caller that meant to move the camera writes it afterwards. *[The zoom module's comments cited the second path as `0x41CDB0` until 2026-09-12; that is not an instruction boundary — `0x41CDAF` is `and edx,3` — so the path is named by its clamp call now.]* |
| **the sites `tagpu_zoom.c` patches, besides the clamp** [VERIFIED by disassembly 2026-09-12] | `0x41C3C5` is `push esi`, the first instruction after the five stolen bytes of `0x41C3C0` (`a1 e8 1d 51 00`), so the `leaf_call` detour's stolen tail ends on a boundary. `0x430FAE` is `call 0x4B6A50` (`SaveSetting(section, name, dword)`), the one site that persists `ScrollSpeed` — the two pushes before it are the arguments in stdcall order: `0x430FA4` pushes `0x5046D4` = `"scrollspeed"` (the value name, the second argument) and `0x430FA9` pushes `0x5032E8` = `"Total Annihilation"` (the section, the first); redirected so the player's own value reaches the registry and never the scaled one. *[The first draft of this row had the two strings the other way round; corrected by the landing review against the `.rdata` bytes.]* `0x498EF9` is `call 0x484B50` (`GetTPosition`) inside `0x498DA0`, after `push eax; push esi; push edi` at `0x498EF6..0x498EF8`; redirected so the world point is clamped to the scroll extent `[0, extent − 1]` before the chain that dereferences its plot. **The three reachable inline target clamps** [DISASSEMBLED 2026-09-23]: `0x41C808` and `0x41C93B` each begin with `mov eax, ds:0x511DE8` (`a1 e8 1d 51 00`, exactly five bytes, so the jump that replaces it ends on a boundary) and `0x41CAF7` with `mov esi,[eax+0x14327]` (six bytes); each smooth arm stores the target immediately before its block. **No branch lands in a replaced byte** — an `rel8`/`rel32` scan of `.text` finds none — but branches do land inside the two centring blocks' address ranges: the eye arm's `je` at `0x41C7F1` and `0x41C924` targets `0x41C87A` and `0x41C9AE`, past the five replaced bytes, and that arm runs intact. Each first instruction becomes a jump to a stub (`pushfd`, `pushad`, the clamp, `popad`, `popfd`, `jmp`) that resumes at the function's tail — `0x41C8BF`, `0x41C9F3`, and `0x41CB44` for the follow — so the fog-bit clear and the frame unwind are the engine's own bytes; the early exits the stub skips (`0x41C871`, `0x41C9A5`) do the same clear and the same pops, so the side effects are those of every exit the block had. `SetCamera`'s block `0x41C4EC` is the same shape and is not patched: no caller reaches it (row `0x41C4C0`). `0x468DBA` is `call 0x418310` (the map debug overlay, one argument, `ret 4`), after `lea eax,[esp+0x34]; push eax` at `0x468DB5`; redirected so the overlay runs only for an eye in `[0, extent − W]` (row `0x418310` in "Who reads the eye"). |

**The scroll poll.** The position it tests comes from `[obj+0x196]` — the mouse object's own
record, fetched with `0x4C2340` at `0x41CEC5` — not from a fresh poll. *[CORRECTED 2026-09-04:
this said `GetCursorPos` (IAT slot `0x4FC2E0`). The poll at `0x41CEE7` is `GetCursorPos`
(`0x4FC2E4`) but serves only the off-screen warp-back below; `0x4FC2E0` is `GetFocus`.]*
Four independent directions, each firing on *hotkey* **or** *pointer on an exact screen
edge* — the hotkeys go through `KeyboardHotkeySampler` `0x4C1B80`, the same sampler `markown`
uses for SHIFT (`0xF9`):

| Direction | Hotkey id | Mouse condition | Site |
| --- | --- | --- | --- |
| left | `0xF4` | `x == 0` | `0x41CF87` |
| up | `0xF5` | `y == 0` | `0x41CFCE` |
| right | `0xF6` | `x == (main+0x37E1F) − 1`, i.e. screenW − 1 | `0x41CFA5` |
| down | `0xF7` | `y == (main+0x37E23) − 1`, i.e. screenH − 1 | `0x41CFFF` |

Two things follow, and both cost time to learn the hard way:

- **The trigger is an exact equality on the outermost pixel, not a band.** Measured on a
  1024-wide screen at 1×: a pointer at `x = 1023` scrolls right, at `x = 1020` it does not.
  (This is also why edge scroll looks dead under injected input — it is not, you just have to
  land on the last pixel. See `ta-drive`.)
- **At zoom > 1 the right edge cannot fire, and only the right edge.** `fake_GetCursorPos`
  hands the engine the *unzoomed* `u`, and three of the four screen edges lie **outside** the
  viewport rect (`L=128`, `T=32`, `B=screenH−33`), so the transform passes them through as
  identity. The screen's right column, though, *is* the viewport's right column, so it gets
  contracted toward the centre. Measured at 2× on 1024×768: a pointer at `x=1023` reaches the
  engine as **800**, so `x == 1023` is unsatisfiable, while left, up and down all still scroll.

### Fields this module owns

| Where | What |
| --- | --- |
| `main+0x1431F` / `+0x14323` | eyeX / eyeY. **Written by us only on the game thread since 2026-09-12** (the command apply, "The commands, applied in `before`" in the publish-point section — the replay's Follow and Jump among its writers since 2026-10-06, or at the replay's frame visit where no apply runs); the render thread reads them from the frame packet and never from the field |
| **the two sites that put a NON-ZERO value in `main+0x142F7`** | `0x49AE8C` (guard `0x49AE84`, 8 bytes earlier — a pointer fixup during the compaction of the record array) and `0x49C7FB` (guard `0x49C7F3`, 8 bytes earlier — the unit→object migration when `[unit+0x110] & 0x20000000`). Like the countdown's seven, the guard is read well before the store, so **neither is usable as a cross-thread interlock**: clearing the slot after the compare has been passed does not stop the write. |
| `main+0x142F3` / `+0x142F7` / `+0x1434B` | **the camera follow**, three slots the stepper takes in that priority [MEASURED 2026-09-10]. `+0x142F3` is `CameraToUnit` [CORPUS] — a followed *unit*, position at `+0x6A`, set by Ctrl+C (`0x41C310`) and the cycle keys (`0x41C2E0`); `+0x142F7` a followed *object*, position at `+0x4`, which `0x49C7FB` migrates the unit follow onto when `[unit+0x110] & 0x20000000`; `+0x1434B` a u16 **frame countdown** on the remembered position at `main+0x1433F`, both filled by `0x499E50` when a followed object is destroyed. Released together by `0x41C390`. **All seven sites in `.text` that store a NON-ZERO value into the countdown** — `0x499E8E`, `0x499F1B`, `0x499F89`, `0x49B0E5`, `0x49B992`, `0x49BCBD`, `0x49C8DD` — first compare their object against `main+0x142F7` and take the `jne` when it differs. **That guard is NOT usable as a cross-thread interlock:** each reads its guard 40-60 bytes before its store (`0x499E60`->`0x499E8E`, `0x499EF0`->`0x499F1B`, `0x49B0BA`->`0x49B0E5`), so clearing `+0x142F7` from another thread after the compare has been passed does not stop the store. Every other write to the countdown, anywhere, is a zero — apart from the stepper's own decrement at `0x41CA2A`, which stores a non-zero value whenever the countdown was 2 or more. *[The count said six until 2026-09-10; `0x499E8E` was missed. Found by the landing review.]* |
| **a manual camera move releases the follow** | The engine's own rule, not a convention: the scroll poll's eye-writing tail runs the three stores at `0x41D091`…`0x41D0AA`, and `0x41D035` skips that whole tail on a frame where the eye did **not** change — so it is released exactly when the player actually moved the camera. `tagpu_zoom`'s cursor-anchored wheel does the same, for the same reason: the stepper recomputes the target from the followed unit every frame, so a delta added to the eye is otherwise eased straight back out [MEASURED 2026-09-10 — with the release removed the same gesture moved the eye by (0, −2) instead of the exact (−150, −100)]. **Since 2026-09-12 (frame packet, landing 2) the release and the step are one act on the game thread**, in the command apply at the top of the in-play draw (`tagpu_zoom_apply`), with no stepper between them: the stepper `0x41CA10` and the scroll poll have already run for the frame (both are called from the frame callback before `0x4969CD`), so the released follow cannot be re-selected before the draw reads the stepped eye. Re-measured on the landing: Ctrl+C then −4 notches aimed at (900,600) on a 1920×1080 viewport moved the eye by exactly (58, −28) and zeroed `main+0x142F3`; the same notches aimed at the centre moved nothing and left it set. |
| `main+0x14327` / `+0x1432B` | **the scroll target** (`MapXScrollingTo`) the stepper eases the eye toward. Every reference to it in `.text` is inside `0x41C4xx`–`0x41D4xx` — 30 and 28 respectively, and **no drawing code reads it**, which is what makes it camera-local. |
| **why we set `main+0x142F1` bit 1** | Nine of the eleven engine eye writers store the eye, set it (`orb $2`), call `0x41C3C0` — which only clamps the eye and recomputes the view box through `0x466B70`, touching neither flag nor the target (`0x41C3C0..0x41C448`, DISASSEMBLED 2026-09-24) — then copy the eye into the target and clear bit 3 of `main+0x14281`. The stepper's two, `0x41CB5F` and `0x41CBCF`, set it and clear bit 3 first, then step the eye toward the target, which they never write (`0x41CB74..0x41CC16`; row `0x41CB5F`). `DrawMinimap 0x466B00` redraws only while the bit is set and **clears it at `0x466B16` in the same breath**, an unlocked read-modify-write on the game thread; it is the bit's only reader (`0x466B06..0x466B16`). Until the frame packet's landing 2 our eye moves ran on the render thread, where setting it would have raced that clear, so they did not; the command apply now runs on the game thread and sets it after any eye it moved (`tagpu_zoom_apply`, 2026-09-24), exactly as the writers do. **It decides something only on a frame that ran no tick:** the radar rebuild `0x466DC0` sets the bit unconditionally (its one exit `0x46743D` follows `0x46742F`), and `0x464F80` calls it for the viewed record (`0x465072`, after `cmp [main+0x2A43], bl`) on every sim tick — `0x464F80` is called at `0x49555F` inside the tick loop `0x495490`, which runs `[main+0x38A3B]` iterations and bumps `main+0x38A47` in each. No lag was ever measured without it: the drawn box followed an anchored gesture (2026-09-10) and three camera moves with the sim paused at `k = 1` (2026-09-24) on the build that did not set it — which setter ran while paused is not established. |
| `main+0x142F1` bit 1 | DrawMinimap `0x466B00`'s dirty flag: tested at `0x466B11`, cleared at `0x466B16`. Set by every camera-module path that moves the eye — the `or` instructions at `0x41C598`, `0x41C891`, `0x41C9C5`, `0x41CB5F`, `0x41CBCF`, `0x41CDDA`, `0x41D04D`, `0x41D17D`, `0x41D264`, `0x41D312`, `0x41D452`, one per eye writer, immediately before its `0x41C3C0` call (the stepper's two, `0x41CB5F` and `0x41CBCF`, write the eye after it) — and by `0x466DA4` (the tail of the minimap's fog composite rebuild `0x466C20`), `0x46742F` (the tail of the radar rebuild `0x466DC0`, every sim tick — row above) and `0x460144`. Bits 0 and 2 of the same word are other flags: bit 0 is the radar dots' blink phase [INFERRED] — the blink timer `0x466580` (called from the tick loop at `0x4955E5`) toggles it at `0x4665A9` when the countdown `main+0x142EF` runs out, and reloads 7; `0x466A89` clears it, and `0x466DC0` reads it (`0x466EC3`, `0x4670DF`); bit 2 asks `0x466C20` to rebuild the fog composite — it tests and clears it at `0x466C29..0x466C44` — set by `orb $4` at `0x466A6E`, `0x481905`, `0x481D3B`, `0x481D81`, `0x4822A1`. Every reader of the word is minimap code, so all three bits are display state; the simulation reads none of them. DISASSEMBLED 2026-09-24. |
| **who may clear `main+0x14281` bit 3** | **Nobody outside the game thread, and that is a hard constraint, not a preference [2026-09-10].** `0x484904` sets it with `or word [eax+0x14281], bx` — an **unlocked** read-modify-write — so a clear issued from the render thread can be swallowed whole by the engine's own store, and a swallowed clear is a fog grid left anchored at an eye that has moved, silently, until the next camera move. No atomic on the other side fixes it: a `lock and` on our byte is still clobbered by their unlocked word write. Until 2026-09-12 this is why `tagpu_zoom`'s cursor anchoring was gated on `tagpu_terrown` owning the fog draw, with a request/ack pair OR-ed into `terr_fogtick`'s condition. **Since the frame packet's landing 2 every eye move of ours happens ON the game thread** (the command apply at the top of the in-play draw, `tagpu_zoom_apply`), and there the clear is exactly what the engine's own writers do at `0x41CB6B`, `0x41CB3B`, `0x41C567` and `0x41CE0D`: `tagpu_zoom_apply` clears the bit after any eye it moved, the engine's fog draw (or terrown's replica of its lazy rebuild) rebuilds inside that same draw, the handshake is gone and the terrown gate with it. |
| **the engine grid's own slack, and why it runs out at z = 1** | The screen fog grid spans `viewW/32 + 2` columns, `+ 3` when the view is not a multiple of 32 (`0x483C1E..0x483C79`, "The allocation, at map load" below), from the eye rounded to a half cell, so its fully written cells reach a slack `s` past the far edge of the 1x rect `[eye, eye + vw]`: `s = 32 − r'` for a view that is a multiple of 32 and `64 − m − r'` for one `m` px past a multiple, `r' = (eye − 16) mod 32` — 1..32 px, 9..40 px for the 1016 rows at 1080p, 17..48 px for the 2096 at 4K. The view at level `z` has half-width `vw/(2z)`. So an eye moved by `D` between the build and the frame is covered only while `|D|` stays within `(vw/2)(1 - 1/z)` plus the grid's rounding on that side — `s` forward, `r'` back (the span starts at `eye − r'`) — and **that slack collapses to the rounding alone as `z -> 1`** [2026-09-10] — the `(vw/2)(1 - 1/z)` term, which is all of it at high zoom, is worth nothing at 1x. That is why the native pass takes the engine's grid only when the 1x rect about the drawn eye lies in its fully written cells `[org, org + 32·(cols − 1)]` — the last column is short its right corners — and the WIDE grid otherwise, rather than only below 1.0 (`tagpu_zoom_wide_fog`, the fog bound in `tagpu_zoom.c`): a 250 ms tween can cross 1.0 in one frame, and a zoom-in anchored at a viewport corner moves the eye by up to `(vw/2)(1/z0 − 1/z1)` in it. Those cells are `vw + 32` wide for a view that is a multiple of 32 (30 × 24 cells for 896 × 704 at 1024x768, MEASURED 2026-09-24 at the descriptor `*(main+0x1421F)`) and `vw − m + 64` for any other (34 rows for the 1016 at 1080p, 68 for the 2096 at 4K), so at 1x the eye it was built at always fits, for every view — a frame at rest samples the engine's grid, and the native heartbeat reads `fog=engine` there at 1080p and at 4K — and an eye moved past `s` does not. It also takes the wide grid whenever the engine grid's own window reaches more than one cell past the map (`col0 < −1` or `col0 + cols > PLOT_C/2 + 1`, per axis, judged on the grid's origin and size), because the builder's border completions sit on literal grid lines (next section, "The four border completions"). |
| `main+0x14281` bit 3 | the screen fog grid is current — already documented (`terrain-depth.md`: "if `LosType & 8` clear, first rebuild the screen fog grid"). **New here:** the camera stepper clears it at `0x41CB6B` on every frame the eye and target disagree, so a stale target becomes a per-frame grid rebuild. Also cleared at `0x41CB3B`, `0x41C567`, `0x41CE0D`. |
| **who sets `main+0x14281` bit 3, and who clears it** [DISASSEMBLED 2026-09-24, every store to the word in `.text`] | **Only a build sets it.** The one engine setter is `0x484904` (`or word [eax+0x14281], bx`), right after the lazy test `0x4848F2` and the build `call 0x4843C0` at `0x4848FA`, inside the fog draw `0x4848E0`, whose one caller is `DrawGameScreen`'s `0x469D8E`; `terr_fogtick` is the only other, right after its own call of the builder. **Clearers** (`and word [..+0x14281], 0xFFF7`), 21 stores in fifteen functions: the eye writers `0x41C54B`, `0x41C567`, `0x41C5CB` (`SetCamera 0x41C4C0`), `0x41C867`, `0x41C8C4`, `0x41C99A`, `0x41C9F8`, `0x41CE0D`, `0x41D080`, `0x41D1B3`, `0x41D297`, `0x41D347`, `0x41D485`; the stepper's `0x41CB3B`, `0x41CB6B`, `0x41CBDB`; the LOS stamps `0x481911`, `0x481D2D`, `0x481D73`, `0x482293`; and `LoadMap`'s `0x483CB7` — so a new level starts with the grid NOT current, and its first fog draw rebuilds. **Every other store to the word writes bit 3 back as it read it**: the 21 load–mask–store `mov`s `0x4166AA`, `0x416D68`, `0x416D98`, `0x495B36`, `0x495E30`, `0x495E51`, `0x496E42`, `0x496E60`, `0x496E86`, `0x497355`, `0x49737C`, `0x4973A1`, `0x497410`, `0x49742D`, `0x497450`, `0x497497`, `0x4974B4`, `0x4974D7`, `0x497633`, `0x497659`, `0x49767D` (bits 0–2, the LOS mode), and the seven `and`s that clear bit 0 or bit 1, `0x417547`, `0x417555`, `0x4656B5`, `0x4656C5`, `0x495B58`, `0x497868`, `0x497876`. That is fifty stores in all — one setter, 21 clearers, 28 that keep it — and no word or dword store at `0x1427E..0x14280` overlaps the byte (the four byte stores to `+0x14280`, `0x4915BA`, `0x495710`, `0x495728`, `0x49635D`, do not reach it). **None of the fifteen clearer functions is reachable from `DrawGameScreen` through direct calls, branches or jump tables**: a recursive-descent walk from `0x468CF0` that follows all three, its 14 jump tables resolved with no target left over, reaches 392 functions and no clearer. It does not follow the draw's 204 indirect calls (115 through the import table, 6 through data pointers, 83 through registers and vtables). None of the fifteen is itself ever referenced as a value — no 32-bit occurrence of its start anywhere in the image, no branch into its body from outside it, no call into its middle — so each is entered only by a direct call to its start; but functions that call them are referenced (the console command table among them), so an indirect path from the draw is not excluded by the image alone, and the measurement below is the evidence that none runs. No engine store to the eye lies inside the draw either (row `0x41CA10`'s neighbours, and the publish point's own note in `tagpu_packet_pub.c`). **The camera shake `0x41C6F0` moves the eye without clearing it** (`0x41C784`, `0x41C797`), and it runs from the stepper at `0x41CC4D` even on a frame whose eye already equals the target — so a shaken frame is drawn over a grid built at the eye before the shake. What this licenses: bit 3 clear before a draw and set after it means that draw rebuilt the grid at the eye it was drawn at; set before means no build, whatever the eye did since — unless an indirect path the walk does not follow clears it and the fog draw then rebuilds, which the diagnostic below would have seen as a grid that changed with the bit set throughout. The packet's publisher keeps the engine grid's build record on exactly that rule (`fog_rec_before`/`fog_rec_after`, gpu-status §2.3e). MEASURED 2026-09-24 against a diagnostic build that checksummed the grid between records and compared each record with `terr_fogtick`'s own eye: on Two Continents at 1920×1080 on Xvfb, a 72-unit fight in view with the shake on, the commander followed on a long move (the stepper moving the eye every frame) and the fog site handed back to the engine and retaken at 1×: 3 639 records, 2 947 of them at `terr_fogtick`'s rebuilds and **every one at the eye that rebuild read** (0 differ, 0 rebuilds without a record), the other 692 from the engine's own fog draw during the hand-back; the grid's bytes unchanged across all 28 641 draws between records (**no unrecorded build**); set-before-clear-after 0; and with the sim paused under `ARMOPT`, one `tacli eye` jump made exactly one record, the apply's clear. |
| `main+0x2C76` / `+0x2C7A` | the engine's mouse position, two **DWORDs**, in **SCREEN** space (measured 2026-09-03: an injected pointer at screen (400,300) reads back 400 / 300; `0x498DA0` is what makes the world point). Worth restating because `+0x2C78` looks like the y and is the high half of x; and `main+0x2C74` is an unrelated word (the battleroom lock bit, `cmdline-options.md`). |
| `main+0x37E1F` / `+0x37E23` | screen width / height — the fields `vpwide` derives the true viewport rect from, and the ones the scroll poll compares against |

### Two per-cell loops that differ, and it matters

An off-map eye is only safe where the engine bounds-checks. Both of these read map arrays from
the eye, and they do not agree:

- **`0x4843C0`** (screen fog-grid rebuild, which `terrown` calls itself) tests each cell against
  the LOS map dimensions with **unsigned** compares — `cmp`/`jae` at `0x4844B9` and `0x4844C7` —
  so a negative index is *skipped*, not faulted. This is why the camera's centre range needs no
  guard here.
- **`0x483FA0`** (the terrain pass) indexes the tile map at `main+0x1428B` with **no bounds
  check at all**: `0x48409B` does `imul` row × stride, `add` col, `lea ebp,[edx+eax*2]`. A
  negative eye of −32 or below reads before the array, one above it leaves a strip of the
  viewport unpainted, and a view larger than the map reads past its end. Our window check at
  `0x484057` bounds those reads and paints the strip ("Engine defects we patch": "The terrain pass
  reads off its tile map"). The camera keeps the eye it hands this pass inside `[0, extent − W]`:
  its centre range is in force only on draws whose terrain latch is up (`gpu-status.md` §2.3c), so
  the one route left into the window check is a view larger than the map, where that range
  inverts and is held at 0. It is the reason to keep the eye's excursion a property of *our*
  passes. **And it WRITES unbounded too**: it places its 32-px cells from the
  rect's L/T (`main+0x37E27`/`+0x37E2B`, read at `0x483FAC`/`0x483FB8`) over the view's W/H
  (`+0x37E37`/`+0x37E3B`, at `0x483FDF`/`0x484002`) and hands every whole cell to `0x4C6E70` →
  `0x4CBEF1`, which copies 32 rows of 8 dwords (`rep movsd` at `0x4CBF1D`) with no clip at all, so
  an origin moved off the offscreen — `vpwide`'s widening makes L/T negative and leaves W/H alone —
  is a heap overwrite. It has one caller, `0x468DB0`; `0x4C6E70` has one, `0x484370`. *[CORRECTED 2026-09-23: this said "moot in practice —
  `terrown` skips the whole function". It skips it only while our terrain pass owns the ground,
  and entering a new skirmish with the previous one's zoom still commanded left a few draws where
  the rect was already widened and the engine still drew the ground. MEASURED: an access violation
  at `0x4CBF1D` writing `0x07B50084`, tile at (−2380, −1105), called from `0x484370`; and, on
  another run, a later read through the overwritten memory (`0x4B8B94`, `mov ecx,[eax+8]` with
  `eax = 0xE367E3E3` — terrain palette bytes where a pointer was). Closed by making the rect wide
  only on draws the game thread has latched as ours — `tagpu_terrown_latch`, gpu-status §2.3b.]*

### Who reads the eye, and what bounds each reader [DISASSEMBLED 2026-09-23]

The audit behind the camera's centre range (`gpu-status.md` §2.3c): every function in `.text`
that loads `main+0x1431F`/`+0x14323`, asked whether an eye anywhere in `[−W/2, map − W/2]` — `map` the map's own size (the PLOT grid ×
16), whose far edges lie 32 px right of and 128 px below the scroll extent's, a range that holds
the centre range in force, `[−W/2, min(extent, map) − W/2]` (the debug `Edge` command can name an
extent past the map, and the far end takes the map's then); W the true
viewport, up to 1792x1016 — with, below 1x, `vpwide`'s rect widened to four times the
true one about its centre, can make it index memory out of bounds. A value used only to project
onto the screen and then clipped or tested against a rect is **projection only**. Grouped by
function from `objdump -d -M intel` of `pristine/TotalA.exe.pristine`.

| reader | what it is | the eye-derived index and its bound | verdict |
|---|---|---|---|
| `0x483FA0` | the terrain pass, one caller `0x468DB0` | `row0·(PLOT_C/2) + col0` from `eye >> 5`, added to `[main+0x1428B]` at `0x4840A6`/`0x4840AC` with **no compare**; the tile id read at `0x4840EF` then forms the tile-graphic pointer | **UNBOUNDED at both ends in the engine's bytes, and bounded by our window check at `0x484057`** ("Engine defects we patch": "The terrain pass reads off its tile map"), which runs the stock pass only on a window inside the tile map and draws any other itself, black past the map. The eye it reads from is kept in `[0, extent − W]` on every draw it runs: terrown's `leaf_call` skips it whenever the terrain latch is up, and the centre range is in force only on those draws. That bounds the eye, not the window: over a view larger than the map the window's last rows run past the tile map from any eye — MEASURED on Lava Run at 1920x1440 (a 1376-px view, a 1280-px map): in a level's first draws, before the latch is up, the pass runs on the stock path with the eye held at 0 (where the engine's own clamp goes negative) and rows 40–42 read past the tile map's end, faulting at `0x4CBE44` without the window check; no fault at 1920x1080 on either camera; 12 skirmish maps are affected at 3840x2160. Without the window check a view larger than the map faults in stock and on both cameras |
| `0x418310` | the map debug overlay, one caller `0x468DBA`; runs only when the debug view `main+0x14280` (cycled 0..4 by a debug key, `0x495703`) or the "Contour" command's `[0x511DD0]` is set | the cell window starts at `eyeX/16`, `eyeY/16` (`0x4183AE`/`0x4183B1`); only the upper ends are clipped (`0x4183B9 jl` against `PLOT_C − 1`, `0x4183D0 jge` against `PLOT_R − 1`). Reads the feature grid `+0x14287` (`0x418440`, `0x418486`, `0x4184DC`, `0x418538`), a player grid (`0x4185ED`) and `[+0x14207]+0x1C` (`0x418671`); no store outside its stack frame | **UNBOUNDED below.** Bounded by `tagpu_zoom`'s redirect of `0x468DBA`: the overlay is called only for an eye in `[0, extent − W]` |
| `0x4843C0` | the screen fog-grid builder (caller `0x4848FA`; `terr_fogtick` calls it too) | `col0`/`row0` from the eye; every cell tested `cmp`/`jae` against the LOS block's w/h (`0x4844BC`, `0x4844C4`, unsigned) before the LOS read `0x4844D7` and the mapped read `0x4845A9..0x4845BD`; the LOS dims are `PLOT_C/2 x PLOT_R/2` (`0x46482B..0x46484F`); every grid write is bounded against the grid's own cols/rows | **BOUNDED.** The border completions misplace their content for an off-range eye (section below); the native pass takes `fogwide`'s grid then |
| `0x4848E0` | the fog draw, one caller `0x469D8E`; terrown skips it with the terrain pass | the sub-cell offset `eye % 32` (projection) and a parity `& 3` into a four-entry table (`0x484A48`, `0x484AED`); the grid walk is `[0, cols) x [0, rows)`, unsigned | **BOUNDED** |
| `0x468CF0` `DrawGameScreen`, the row sweep `0x469748..0x4697B3` | the feature/unit sweep over the feature grid `+0x14287` and the per-row HotUnits buckets | row start `eyeY/16 − 16`, column start `eyeX/16 − 10`; a negative start is set to 0 and the count cut to match (`0x469755`, `0x469796`, signed), the far end clipped to `PLOT − 1` (`0x46977B`, `0x4697AC`); the bucket index `(unitY − eyeY)/16 + 16` refused by `js` at `0x469800` and `cmp [edi+0x54]; jge` at `0x469802`/`0x469805` | **BOUNDED.** Its other eye reads (`0x468DD9`, `0x469C70`, `0x469E1F`) are projection only |
| `0x48BAE0` | the HotUnits cull (callers `0x48D5A5`, `0x495C6D`, `0x495E5D`, `0x49697B`, and the replay's frame visit, *The draw's unit list*) | projects each unit's box and tests it against the rect `+0x37E27` with signed compares (`0x48BC25..0x48BC3B`); the list write `0x48BC6A` is bounded by the unit-array walk | **BOUNDED** |
| `0x498DA0` | pointer → world point, one caller `0x499221` | `eye + clamp(pos, L, R) − L` into `GetTPosition`, whose call `0x498EF9` `tagpu_zoom` redirects to clamp the point to the scroll extent, whose 128-px bottom margin is `GetTPosition`'s search window ("Engine defects we patch"); `GetTPosition` clamps too, to the map's own size (`0x484B5D..0x484B88`) | **BOUNDED** |
| `0x466B70` | the minimap view box, called only inside `0x41C3C0` | arithmetic into a RECT at `+0x142CB`; `tagpu_zoom`'s replacement clamps the rect to the minimap at every zoom | **BOUNDED** |
| `0x47F300` | positional sound (six callers) | the indexed reads use the sound's own position, unsigned against the LOS block (`0x47F42D`/`0x47F431`, `0x47F472`/`0x47F476`); the eye only in pan arithmetic and a signed on-view compare | **BOUNDED** |
| `0x417600` MakePoster → `0x495A30` the screenshot tiler | a console command (table `0x5019B8`, name at `0x502254`) | the poster's origin is clamped to `[0, extent − size]` (`0x4176D6..0x417705`, the extent read at `0x4176E1`/`0x4176EF`); the tiler saves the eye, `SetCamera`s each tile through our clamps, calls `0x48BAE0` and `DrawGameScreen(1, 0)` (`0x495C76`), and restores the eye (`0x495E11`) | **BOUNDED.** It draws outside the in-play gate, so on the last in-play draw's range and terrain latch, which agree |
| `0x48C390` | drag-box select, caller `0x49944C` | the eye cancels out of the compares (`0x48C51E..0x48C532`); the unit loop is bounded by pointer compares | **BOUNDED** |
| `0x48C6A0` | the unit hit test inside `GetUnitAtMouse 0x48CD80` | projects the footprint's corners and tests the point against the quad (`0x4C1320`) | projection only |
| `0x420B00`, `0x4211D0`, `0x49BE60` | explosions, debris, projectiles | projection, then `IsPositionInRect`; fxown redirects all three | projection only |
| `0x458810`, `0x46A530`, `0x46A610` | a unit's draw, the selection boxes, the feature leaf | the eye as the camera origin; the feature leaf's grid index comes from its tile arguments | projection only |
| `0x472E30` family, `0x475470` | the particle layers | the eye as an `i16` screen offset; the LOS index uses the particle's own position, bounded unsigned (`0x473608..0x473620`), at the altitude-sheared row "Line of sight at the map's edge" patches | projection only |
| `0x46A860`, `0x46B9D0`, `0x417E00` | the debug position text, a cell rect, the waypoint path (a vtable slot at `0x4FD480`) | no index | projection only |
| `0x417BB0`, `0x417C00`, `0x417C70`, `0x417D30` | world-to-screen line helpers | no call site or pointer to any of them in the image | dead code |

**The far ends moved from the extent's edge to the map's** (32 px right, 128 px down), and the
table holds for them unchanged: no row's bound reads where the centre range ends. Each is its own
compare, the terrain latch, or a test against the engine's range, which still reads the extent.

**Two stock defects beside these readers, not caused by the eye, both patched** — "Engine defects
we patch" above: the unit sort's append can run past the END of the whole sort buffer
(`0x469807..0x469825`), which our build reaches below 1× with an ordinary crowd (filling past one
row's own capacity is stock behaviour and harmless); and `GetGridPosFeature 0x421E60` faults
reading `[NULL+8]` at `0x421E64` when handed a NULL plot, which stock reaches only where its own
range `[0, extent − view]` is empty (a map shorter than the viewport plus 128 px) [INFERRED], under
the debug `Edge` command, or through `0x4815F0`'s `0xFFFE` cells on a path not audited.

### `KeyboardHotkeySampler 0x4C1B80` — why polling it twice is safe

`ui-markers.md` relies on this and it is worth having in the map. The function is a jump table
(`0x4C1C48`, index bytes at `0x4C1C6C`); the SHIFT entry `0xF9` lands at `0x4C1BB5`, which is
`push 0x10; call ds:0x4FC350` (`GetAsyncKeyState`) then **`and al,0xfe`** — masking off bit 0,
the "pressed since last call" bit — before `neg ax; sbb eax,eax; neg eax` normalises to 0/1.
All eight key stubs (`0x4C1BA1..0x4C1C3E`) do the same mask, as does the keydown translator's own
Ctrl poll at `0x4C1D56` (`0x4C1D5C`); the ninth entry, `0x4C1C41`, answers 0. So nothing in the engine
reads the consumable bit and an extra poll of our own cannot steal an edge.

**The ids [DISASSEMBLED 2026-10-25].** `id − 0x20` above `0xDB` answers 0 (`0x4C1B87`); the index
bytes map exactly eight ids onto a key, the rest onto the zero entry: `0x20` SPACE, `0xF4`..`0xF7`
LEFT, UP, RIGHT, DOWN, `0xF9` SHIFT, `0xFA` CONTROL, `0xFB` MENU (Alt). 17 direct callers. Every
key goes to `GetAsyncKeyState` (IAT `0x4FC350`), so the answer is the key's state at the call, not
the state carried by the message being handled; cnc-ddraw's `fake_GetAsyncKeyState` answers the
input Impure injects.

**Alt+Z and Alt+X do nothing in play [DISASSEMBLED 2026-10-25].** The keydown translator
`0x4C1D50` (which takes `WM_SYSKEYDOWN` too, `0x4B5DC3`) pushes a letter lower-cased (`0x4C213D`),
or as `letter + 0x69` with Ctrl held (`0x4C212D`); Alt changes nothing there. The in-game key switch
`0x495E90` (index bytes `0x496694`, jump table `0x4965F4`) sends `z` and `x`, and `Z` and `X`, to its
default `0x4965CE`, which calls the debug keys `0x4956C0(key)` (`gpu-status.md` §2.6f gates them
on a puppet) only while `main+0x3923B` bit 1 is set [not traced further]. The GUI dispatch zeroes the key it
hands its gadgets while Alt is down (`0x4AA219..0x4AA232`: `0x4C1B80(0xFB)`, then `not` and `and`),
and the window procedure returns 0 for `WM_SYSCOMMAND` with `SC_KEYMENU` (`0x4B5D9C` → `0x4B5DFC`,
`cmp eax,0xF100`), so the system menu never opens. Impure takes the two keys with a building armed
(mode 14) for the placement's spacing, before TA's window procedure sees them (`tagpu_shield.c`,
`tagpu_place_key`); outside mode 14 they reach the game as before.

## The mouse object, its event ring, and the two ways the cursor gets drawn — mapped by us

[MEASURED 2026-09-04, this project — `objdump -d -M intel` of the pristine Steam build plus live
`tacli peek` on a running game. Established while replacing the composite's cursor-moving hack
with the structural fix (`gpu-status.md` §2.3d). Every VA was read off `objdump` in that session
unless a row says otherwise. This is the layer *below* the cursor chain in the next section: that
one is "which cursor picture", this one is "where the pointer is and who drew it".]

**The object.** `0x4B6220` is exactly `mov eax, ds:0x51FBD0; ret` — the mouse/UI singleton
accessor. So the object is `*(void**)0x51FBD0`. The ring and record helpers below call it at
their head; `0x4C24B0`, `0x4C25E0`, `0x4C67C0` and `0x4C6B10` do **not** — they take the object
(or the surface) as a parameter.

| Offset | What | How established |
| --- | --- | --- |
| `+0x186` | event-ring capacity — the divisor in `idiv` at `0x4C2E44` | disassembly |
| `+0x18A` | event-ring base; entries are 6 dwords (`lea eax,[ecx+ecx*2]` then `*8` = stride 24) | disassembly |
| `+0x18E` | ring **head** (write index), advanced and wrapped at `0x4C2E78..0x4C2E82` | disassembly |
| `+0x192` | ring **tail** (read index), advanced at `0x4C2DB5` | disassembly |
| `+0x196` | the **current mouse record**, 6 dwords, laid out exactly like a ring entry. Written whole by `0x4C2360`, and x/y alone by each of the three drawing polls | disassembly + live |
| `+0x1AE` | cursor hide/nesting counter — `0x4C2870` decrements it and returns early while it is still > 0 | disassembly |
| `+0x1B2` | the current cursor sprite record: size at `+0`/`+2`, read **zero**-extended (`xor edx,edx; mov dx,…`), hotspot at `+4`/`+6`, read **sign**-extended (`movsx`) — so a hotspot may be negative and a size may not. **It is a GAF frame header, whole**, not a cursor-shaped struct that happens to start like one: `0x4C2960` loads `[obj+0x1B2]` and `0x4C297B` pushes it straight to `CopyGafToContext 0x4B7F90` behind the NULL context, so the colour key at `+0x08` and the pixel plane at `+0x10` are the ones every other GAF frame in this engine carries ([effects](../effects.html) `CopyGafToContext`). That is what lets a renderer draw the cursor with no observer on the blit at all — it reads the frame instead. **[VERIFIED 2026-09-09** by disassembly `0x4C28A5`–`0x4C2989`, and by measurement: decoding it through `tagpu_gaf_decode` and drawing it at `k = 1` reproduces the engine's own frame byte for byte ([GUI renderer](../gui-renderer.html) §17), which a wrong key or pixel offset could not.**]** | disassembly + live |
| `+0x1B6` / `+0x1BA` | the position the sprite was last DRAWN at, i.e. position − hotspot | disassembly |
| `+0x1BE` / `+0x1C2` / `+0x1C6` | saved-background rect pointers *[INFERRED]*. **Not one per draw path**: `0x4C2870`, `0x4C24B0` and `0x4C67C0` all use `+0x1BE`, `0x4C25E0` uses `+0x1C2`, and no reader of `+0x1C6` was found in this pass | disassembly |
| `+0x1CE` | a mode word, and **not a simple disable**: `0x4C67C0` returns early when it is **zero**, `0x4C2870` returns early when it is exactly **1**. The two draw paths below are selected by it *[INFERRED]* | disassembly |
| `+0x1D2` | non-zero is required by `0x4C24B0`, `0x4C25E0` and `0x4C67C0` before any of them draws | disassembly |

**The record is 6 dwords, and the fifth is the message id.** Read off TA's own window procedure,
whose `0x200..0x206` jump table at `0x4B60F4` has three arms:

| offset | field | set at (move arm `0x4B5E51`) |
| --- | --- | --- |
| `+0x00` | x | `[esp+0x04]`, the sign-extended lParam LOWORD |
| `+0x04` | y | `[esp+0x08]` |
| `+0x08` | wParam (the key/button flag word) | `[esp+0x0C]` |
| `+0x0C` | a `GetTickCount` stamp (IAT `0x4FC0DC`, KERNEL32), scaled by `[obj+0xE8]` and divided by 1000 | `0x4B5E9C` |
| `+0x10` | **the message id** | `0x4B5EA0` (`mov [esp+0x18],esi`, esi = uMsg) |
| `+0x14` | **1 only for a DOUBLE-CLICK**; 0 for a move and for every single press *and* release — see the arm split below | `0x4B5E90` / `0x4B5EF4` / `0x4B5F40` |

The jump table entries, in order `0x200..0x206`: `0x4B5E51`, `0x4B5EB2`, `0x4B5EB2`, `0x4B5EFE`,
`0x4B5EB2`, `0x4B5EB2`, `0x4B5EFE`. Three arms, and the split is **not** down-versus-up:
`0x4B5EB2` takes all four single presses and releases (0x201, 0x202, 0x204, 0x205) and writes
`+0x14 = 0`; `0x4B5EFE` takes only the two double-clicks (0x203, 0x206) and writes `1`. So
**`WM_MOUSEMOVE` alone goes to `0x4C2360`, which copies its record into `[obj+0x196]`; every
button message goes to `0x4C2E30`, which pushes onto the ring.** That asymmetry is the whole reason a move and a click have to be treated
differently on the input path (`gpu-status.md` §2.3d).

| VA | What it is |
| --- | --- |
| `0x4C2360(rec*)` | copy 6 dwords **into** `[obj+0x196]`. Two call sites: `0x4B5A88` (an init/reset path) and `0x4B5EA4`, the `WM_MOUSEMOVE` arm |
| `0x4C2E30(rec*)` | ring **push**: writes at `[obj+0x18A] + head*24`, advances and wraps head, and **drops the event when the ring is full** (`head+1 == tail`). One call site, `0x4B5F51` — the button arms |
| `0x4C2D60(rec*)` | ring **pop**: copies the tail entry out and advances the tail; when `head == tail` it copies `[obj+0x196]` instead. `ret 4` |
| `0x4C2DE0(rec*)` | the same **without** advancing the tail — a peek. Returns 1 when it took a real ring entry, 0 when it fell back to `[obj+0x196]` |
| `0x4C2340(rec*)` | copy 6 dwords **out of** `[obj+0x196]`, no ring, no poll. 5 call sites |

**After a press, IdleTick re-reads the move made before it.** With the ring empty, IdleTick's peek
and pop copy `[obj+0x196]` (`0x4C2D73`, `0x4C2DF3`) into `main+0x2C76`, and only a later
`WM_MOUSEMOVE` rewrites it (`0x4C2360` from `0x4B5EA4`). So once the press has been popped, every
IdleTick until the pointer moves hands the in-play handler the move record from **before** the
press, without `MK_LBUTTON` in its wParam. MEASURED 2026-10-25: `msg 0x200 wParam 0x4` (Shift
alone) for a second with Shift and the left button held. The records' times cannot tell the two
apart reliably: a move's (`0x4B5E70`), a press or release's (`0x4B5ED1`) and a double-click's
(`0x4B5F1D`) are each `GetTickCount() · [obj+0xE8] / 1000` inline, `0x4B6340`'s formula, at 30 a
second (`+0xE8` reads 30, *The scheduler, the clock and the test pacing*), and wrap short of 2^32; the placement
module tells them apart by the record's identity (`tagpu_place.c`, `stale_move`).

**That identity is dwords 2..5, never x and y.** `0x4C2360` is the only writer of the record's
wParam, clock, message and double-click flag (`[obj+0x19E..0x1AD]`); its x and y are rewritten
without them, on the game thread, by each drawing poll from `GetCursorPos` — `0x4C24F2`/`0x4C24F8`
in `0x4C24B0`, `0x4C2624`/`0x4C262A` in `0x4C25E0`, `0x4C28C0`/`0x4C28C6` in `0x4C2870` [DISASSEMBLED]
— and by vpwide's repair of `main+0x2C76` (`tagpu_vpwide.c`, `vpw_mouse_world_impl`). The other
`+0x196` stores in the image (`0x42B52A..0x42B566`, `0x42C165..0x42C1E0`, `0x42CDC7`) are a struct
copy and the weapon parser on other objects, not the mouse object. Zoomed, the two writers disagree:
the move message carries the unzoomed position (`tagpu_zoom_mouse_lparam`), the poll the screen one,
so a copy taken at the press and IdleTick's copy a frame later differ in x and y alone. MEASURED
2026-10-25 on the reference setup at about zoom 0.35: `1573,1382` at the press, `1841,1185` after,
the same clock `0x85E53`; a whole-record compare took 25 of 38 Shift-clicks for a lost release and
ordered nothing.

**The lParam unpack, per arm.** Each of the three arms above unpacks the position with the same
two instructions, and they are the bytes `vpwide` patches to `MOVSX ECX,CX` / `SAR EAX,0x10`:

| arm | unpack at | bytes |
| --- | --- | --- |
| `0x4B5E51` move | `0x4B5E5F` | `81 E1 FF FF 00 00` `C1 E8 10` — `AND ECX,0xffff` + `SHR EAX,0x10` |
| `0x4B5EB2` single press or release | `0x4B5EC0` | the same nine bytes |
| `0x4B5EFE` double-click | `0x4B5F0C` | the same nine bytes |

`LOWORD`/`HIWORD` is zero-extending, so a client x of −20 arrives as 65516 and the event is lost
— see `gpu-status.md` §2.3b for why that is half the addressable ring at zoom < 1.

**`0x4C6B10` — the surface clip-rect store, and it clamps nothing.**

```
4c6b10  mov eax,[esp+4] ; l          add ecx,0x1c            ; this + 0x1C
4c6b14  mov edx,[esp+8] ; t          mov [ecx],eax           ; +0x1C  l
4c6b18                               mov [ecx+4],edx         ; +0x20  t
4c6b1d  mov eax,[esp+0xC]; r         mov [ecx+8],eax         ; +0x24  r
4c6b24  mov edx,[esp+0x10]; b        mov [ecx+0xC],edx       ; +0x28  b
4c6b2e  ret 0x10                                             ; __thiscall(self,l,t,r,b)
```

Four dwords into the surface's clip rect at `+0x1C..+0x28` and not one bound check —
`SurfaceCreateNamed` initialises the same field to `(0, 0, w-1, h-1)`, which is the bound a
caller is expected to respect.

**Six call sites, and `vpwide` redirects three.** `0x468D85`, `0x46964F` and `0x469F95` are the
`DrawGameScreen` sites that feed it the viewport rect `main+0x37E27`, so those are the ones that
would hand it a widened rect; they are redirected and clamped — to the surface allocation *and*,
since 2026-09-09, to the **true viewport rect** (`gpu-status.md` §2.3b). The other
three are `0x495CAC`, `0x4A20A1` and `0x4A22DF`, and **none of them is handed the viewport
rect** — settled by two independent disassembly reads during the G13m landing review. `0x495CAC`
builds its argument block at `0x495C91..0x495CA9` from a loop accumulator, a literal `0` for `t`,
and the view size in 16-px tiles (`main+0x1423B`/`+0x1423F` — **not** map dimensions; see
below), on a **stack-local** surface (`lea ecx,[esp+0x9c]`);
its enclosing function `0x495A30` does read `main+0x37E27`/`+0x37E2B`, but into other locals.
`0x4A20A1` and `0x4A22DF` are a save/restore pair around `0x4C6AE0`, which is the matching clip
**getter** — `add ecx,0x1C` then four dwords copied OUT (verified here). Only
`0x468D85`/`0x46964F`/`0x469F95` copy `main+0x37E27` immediately before the call, and those are
the three that are redirected.

**A widened rect DOES reach engine drawers, and they DO honour this clip rect [MEASURED
2026-09-09].** Both halves were open until a 500 v 500 fight with the whole army selected at 0.42×
answered them: with the clip clamped only to the surface, the engine's own selection rect
`0x46A530` painted the side panel and the strips (5768 stray pixels, permanent and accumulating);
clamping the same three sites to the true viewport as well took it to 31 — the minimap's own view
rect — *with the same five hand-back frames still occurring*. So the drawers inside
`DrawGameScreen` read `ctx+0x1C..+0x28` and stop at it, and nothing else reaches outside the
viewport there. `gpu-status.md` §2.3b has the numbers; `ui-markers.md` §1 has the drawer.

**Still open:** `0x495A30` calls `DrawGameScreen 0x468CF0` in a loop while driving the eye, so it
renders *under* whatever rect is in force. Whether that path can be entered while the rect is wide
was not settled — the clip guard bounds it either way now.

**`GetCursorPos` is the IAT slot `ds:0x4FC2E4`, reached from six places, and only three of them
move the cursor.**

| Site | Return addr | Draws? | What it is |
| --- | --- | --- | --- |
| `0x4C28AA` | `0x4C28B0` | **yes** | inside `0x4C2870`, the hide-counter path: poll → `[obj+0x196]` → `+0x1B6/+0x1BA` = answer − hotspot → save background `0x4C6B70` → blit `0x4B7F90` |
| `0x4C24DE` | `0x4C24E4` | **yes** | inside `0x4C24B0`, same shape, saved rect at `+0x1BE` |
| `0x4C2610` | `0x4C2616` | **yes** | inside `0x4C25E0`, same shape, saved rect at `+0x1C2` |
| `0x41CEE7` | `0x41CEED` | no | inside the scroll poll: **only** the off-screen warp-back, see below |
| `0x4C2318` | — | no | a bare `GetMousePos(int*,int*)` wrapper (`0x4C2310`, `ret 8`). **DEAD: zero `call` sites and the literal `0x004C2310` does not occur anywhere in the image** |
| `0x49F7CA` | — | no | a `jmp` thunk to the same import. **DEAD by the same two tests** |

All three drawing polls take the position from **their own answer**, never from `[obj+0x196]`.
That matters, because there is a fourth draw that does the opposite.

**THE SECOND DRAW: `0x4C67C0(mouseObj, surface)`, which blits from the RECORD and never polls.**
It early-outs unless `[+0x1CE]`, `[+0x1D2]` and `[+0x1B2]` are all non-zero, then computes
`+0x1B6/+0x1BA` from `[obj+0x196]` − hotspot, saves the background through `0x4C6B70` and blits
through `0x4B7F90` — the same blit the polling paths use. Two call sites, both in the surface
present machinery: `0x4C641B` and `0x4C6544`, **both inside the same function `0x4C63A0`**
(which runs to its `add esp,0xF4; ret` at `0x4C67BA` and has **44 callers** of its own — 20 in
`0x41Fxxx`, the rest spread over `0x420xxx`–`0x49Fxxx`).

**So `[obj+0x196]` is not merely "the last mouse record" — it is a position the cursor can be
drawn at.** Whatever wrote it last decides where the sprite appears on the next present: a
`WM_MOUSEMOVE` through `0x4C2360`, or the last drawing poll. Measured, 1920×1080 at zoom 0.25×
with `fake_GetCursorPos` answering the true pointer while the move message still carried the
transformed `u`: the sprite tracked `u` across the frame at 4× the pointer's speed and off the
left edge. `0x4C2380` — which the previous pass listed as the record-drawing path — is **DEAD**
(zero `call` sites, literal absent); `0x4C67C0` is the live one.

**`0x4C67C0` IS THE SHELL'S PUBLISH POINT — observed since 2026-09-13** (`tagpu_packet_pub.c`,
the shell cursor channel; the *why* is the frame packet's: the GL UI layer needs the drawn
cursor's rect on every frame it composites, and the packet's in-play gate never runs in the
shell). **Its cursor BLIT — the `call` at `0x4C687D`, not this function — was one of the four `tagpu_cursown.c` skips (GUI renderer §24.1); that module is deleted and nothing patches the blit now.** A flag-gated leaf on the whole function was tried on 2026-09-13 and withdrawn: this function's caller restores the background it saves, unconditionally, so taking it over unpaired the restore and froze `+0x1B6/+0x1BA`. Full prologue `56 8B 74 24 08 8B 86 CE 01 00 00` — `push esi; mov esi,[esp+8]; mov
eax,[esi+0x1CE]` — 11 bytes, resuming at `0x4C67CB`; `stdcall(globals, surface)`, `ret 8`, so
`entry_esp[1]` is `*(0x51FBD0)` itself. **The three early-out words are the gate an observer must
reproduce**: `+0x1CE` then `+0x1D2` then `+0x1B2`, each `test`ed against zero with a jump to the
shared exit at `0x4C6883` ([VERIFIED by disassembly of the pristine exe, 2026-09-13] — the
`objdump -d -M intel --start-address=0x4C67C0 --stop-address=0x4C6890` above). **It runs on every
present, in the shell too**: 40 663 entries in the first ~8 s of a main-menu session on the
reference setup, ~5 000/s, with all three words non-zero and `+0x1B6/+0x1BA` equal to
`+0x196/+0x19A` at a pointer over the window [MEASURED 2026-09-13].

**But AT MOST once per present, not exactly once — three exits leave before the call.** The
DirectDraw arm skips `0x4C6544` at `0x4C666A` (no primary, `[+0xDC] == 0`), at `0x4C67B0` (the
back buffer's size disagrees with the screen's) and at `0x4C65A0` (`Lock` failed — the
DDERR_SURFACELOST arm), and on those presents the engine draws no cursor at all. An observer on
this site therefore sees nothing on them, which is the one gap a hook here cannot close: the flip
that would report "this present happened, without a cursor" is exactly the site the chain rule
refuses.

**THE ORDERING THAT MATTERS TO AN OBSERVER: `+0x1B6/+0x1BA` are written INSIDE this function**,
at `0x4C683C` and `0x4C684E`, as `movsx` of the hotspot (`record+0x4`/`+0x6`, SIGNED — a build
cursor's hotspot is routinely negative) subtracted from `+0x196`/`+0x19A`. So at its ENTRY those
two words hold what the *previous* draw left — a whole frame stale whenever the pointer moved —
and only its exit has this frame's position. An observer that needs the drawn rect must hijack
the return (`after`), not read at entry. The other three draw paths (`0x4C2870`, `0x4C24B0`,
`0x4C25E0`) write the same pair, from their own poll's answer rather than from `+0x196`.

**`+0x1B6/+0x1BA` MEANS THE SAME THING ON EVERY PATH: the drawn origin, i.e. the mouse record
MINUS the sprite's hotspot.** *[CORRECTED 2026-09-14. A revision of 2026-09-13 claimed the
opposite — that `0x4C25E0` stored the poll's raw answer where the flip's draw subtracted the
hotspot — and built a "fingerprint" on it that was then used to identify which site draws in
play and to explain why the composite's erase rect was a hotspot's width out. **All of it was
false, and it replaced a line that had been right.** `0x4C25E0` subtracts the hotspot at
`0x4C2638` (`sub edi,edx`) and `0x4C2645` (`sub ebx,ecx`), after `movsx`-ing `record+4`/`record+6`
at `0x4C2630`/`0x4C2634`; `edi`/`ebx` are untouched from there to the stores at
`0x4C284C`/`0x4C2852`. `0x4C2870` does the same at `0x4C28D4`/`0x4C28E0`. Verified twice: by the
disassembly above, and live — with the engine's draws skipped at the blit only, the pair reads
13..17 px off `+0x196/+0x19A`, which is precisely the hotspot of the move cursor at its 27..35 px
pulse sizes.]* There is therefore **no way to tell the sites apart from the value of this pair**,
and any future attempt to attribute a drawn cursor to one of them needs a different oracle — the
one that worked is the engine's own surface (`tacli shot`, which never contains our GL cursor),
with each site's blit disabled in turn.

### `0x4C25E0` — the body of the engine's MOUSE THREAD, and one of four cursor blits

[MEASURED 2026-09-14, disassembly of the pristine exe plus live A/B on the reference setup.
This section replaces one written 2026-09-13 that called this function "the cursor draw that
actually runs in play" and "the mouse object's per-frame update". Both were wrong, and the
second was wrong in a way that hid the first.]

`stdcall(mouseObj)`, `ret 4` (epilogue `0x4C2864`), prologue `83 EC 58 56 8B 74 24 60`. Two
early-outs to `0x4C2860`: `test [+0x1D2]` zero, and the context acquire `0x4C5FF0` answering
zero. Past them: `GetCursorPos` (IAT `0x4FC2E4`, called at `0x4C2610`), write the RECORD
`+0x196/+0x19A` from that answer at `0x4C2624`/`0x4C262A`, subtract the hotspot into `edi`/`ebx`, fill the saved-background descriptors at `+0x1C2`
and `+0x1C6`, **blit the sprite at `0x4C2732` into the private context `[obj+0x1C6]`** (not the
screen), and store `+0x1B6/+0x1BA`.

**IT IS A THREAD ENTRY'S CALLEE, NOT A PER-FRAME UPDATE.** Its one caller is `0x4C2A0D`, inside
`0x4C2990` — and `0x4C2990` is never `call`ed anywhere, because it is *started*: the cursor init
`0x4C2BD0` (one caller, the graphics init `0x4B5980` at `0x4B5A67`) zeroes `[obj+0x1CE]` (`0x4C2C72`),
pushes `0x4C2990` as the entry point with a 0x8000 stack and the object as its argument
(`0x4C2C80..0x4C2C86`) to `0x4B6B20` at `0x4C2C91` (a three-argument forwarder to `0x4E77D0` —
`_beginthread`), stores the handle at `[obj+0x1CA]` (`0x4C2C98`) and sets `[obj+0x1CE]` to **1**
on success (`0x4C2CA0`). The restart `0x4C2A80` (`0x4C2A8B`, `0x4C2A9A`, `0x4C2AAE`) has the same
shape and no caller. So `+0x1CE` means **"the mouse thread is running"**, and it is running from
the graphics init to the process's exit ([the mouse thread](process.md#the-engines-mouse-thread-0x4c2990-and-totalas-other-threads-during-play-disassembled-measured-2026-10-01)).
Anything of ours that runs at this site runs **on that thread**, not on the game thread.

**THE PREVIOUS PASS'S IDENTIFICATION WAS NOT EVIDENCE.** It rested on the hotspot "fingerprint"
corrected above, which does not exist, and on counters that were gated behind
`s_retDepth == 0 && !s_levelOpen` and so could not have counted an in-play draw whatever
happened. **Which of the four paths draws in a given mode is still not established here**, and
the module that took the cursor over stopped needing to know: `tagpu_cursown.c` patched the blit
`call` in all four.

**THOSE FOUR PATCHES ARE GONE [2026-09-20].** `tagpu_cursown.c` is deleted. It suppressed the
engine's cursor only while OURS was on screen, and ours was the UI layer's composite, which the
clean cut deleted ([gpu-status](../gpu-status.html) §2.81) — so the byte the four stubs compared
could only ever be 0 for the life of a session. The addresses below are still correct and are
still the four cursor blits; nothing of ours sits on any of them, and the engine draws its own
cursor into its own surface, which is where the reference frame wants it.

**THE FOUR CURSOR BLITS.** Every one of these functions contains exactly one
`call CopyGafToContext 0x4B7F90`, and they are the only cursor blits in the image:

| in | blit | its background save | notes |
| --- | --- | --- | --- |
| `0x4C67C0` (the flip's draw) | `0x4C687D` | `0x4C6862` | the blit is the last instruction before the epilogue |
| `0x4C25E0` (the mouse thread) | `0x4C2732` | — (descriptors at `+0x1C2`/`+0x1C6`) | blits into `[obj+0x1C6]` |
| `0x4C2870` | `0x4C297E` | `0x4C2937` | first gate **is** `cmp [+0x1CE], 1` — `0x4C287D` loads `edi = 1`, `0x4C2882` compares against it, `0x4C2888` exits when equal — so it early-outs for as long as the mouse thread is up, whatever its callers do; second gate is a decrement of `+0x1AE`. Its in-play callers (`0x46A3C7`, `0x496A8C`, `0x498434`) are real but reach a function that returns. *[This row said the opposite for one revision on 2026-09-14 — a "correction" that reversed a claim which had been right. Patched anyway: with the mouse thread DOWN, `+0x1CE == 0` and this is a live draw path.]* |
| `0x4C24B0` | `0x4C258C` | `0x4C256A` (`0x4CBBE0` into `+0x1BE`; `0x4C24DE` is the `GetCursorPos`, not the save) | no call site and no literal found. Patched anyway — "no caller in the image" was also true of `0x4C2990`, which is a thread entry |

**THE SAVE/RESTORE PAIRING IS THE REASON THE BLIT IS THE PATCH POINT.** `0x4C67C0` is not a
self-contained draw: it fills the descriptor, writes the position pair, calls the background SAVE
and only then blits — and **its caller restores that background unconditionally afterwards**
(`0x4C6585`, reached whenever `+0x1CE` and `+0x1D2` are non-zero at `0x4C6558`/`0x4C6565`, with no
test of whether the draw ran). Taking the whole function over therefore leaves a restore with no
save to pair against and freezes `+0x1B6/+0x1BA` for every consumer; taking the `call` over leaves
every one of those side effects running. [VERIFIED 2026-09-14: with the blit patched the pair
still tracks the pointer to the pixel, where under the function-level patch it froze.]

**`main+0x2C76` — the record the game actually acts on.** Filled once per mouse dispatch by the
routine that ends at `0x499A2A`:

```
0x499982  PeekMouseEvent(&A)                       ; 0x4C2DE0, into [esp+0x8]
0x49999C  PeekMouseEvent(&B)                       ; 0x4C2DE0, into [esp+0x20]
0x4999AB  if (A.msg == B.msg)  GetMouseEvent(main+0x2C76)     ; 0x4C2D60 — pops
          else if (A.msg == 0x202 || A.msg == 0x205) main+0x2C76 = A
          else                                       main+0x2C76 = B
0x499A1C  call [main+0x391F5]                      ; the per-input-mode handler
```

`0x499200` (input mode 6, in-game — see the next section) then copies those 6 dwords to its own
stack and passes the copy to `0x498DA0`. **The copy is not what the readers after that call
read**: `GetUnitAtMouse 0x48CD80` at `0x499278` and the mouse routing test at `0x469DE1` both
load `main+0x2C76`/`+0x2C7A` directly. Anything that corrects the mouse point inside a
`0x498DA0` redirect must therefore write the field as well as the copy.

**`0x498DA0` clamps the screen point into the viewport rect before it makes a world point**
[VERIFIED 2026-09-12]: `0x498E32`…`0x498E86` pins x into `[L, R]` and y into `[T, B]` — the
rect at `main+0x37E2B`…`+0x37E33`, not the screen — and only then adds the eye. So a point
outside the rect does not produce a world point outside the view; it produces the edge one.
That is what makes G18f's `border_to_border` free: mapping the outermost device column to the
outermost engine column costs nothing, because the engine would have clamped anything past it
to the same column anyway.
Readers of `main+0x2C76` found in the image: `0x41635D`, `0x419BF0`, `0x41A4B2`, `0x41CCAF`,
`0x41CD63`, `0x41D101`, `0x469DE7`, `0x48CD91`/`0x48CD97`, `0x496490`, `0x498D16`, `0x499210`.
The only writers are the three stores above. `0x419BF0` and `0x41A4B2` are inside the order
dispatchers `0x419BE0`/`0x41A490`, so this record does not only drive hover — a wrong value here
becomes a wrong *order*.

**The scroll poll is `0x41CE90`, not `0x41CF10`.** *[CORRECTION: `gpu-status.md` §2.3c named
`0x41CF10`, which is not an instruction boundary — `0x41CF0E` is `lea ebp,[esi+0x64]`.]* One
caller, `0x496976`. It reads the mouse position with `0x4C2340` — i.e. out of `[obj+0x196]`, not
from a fresh poll — into `[esp+0x1C]`/`[esp+0x20]`, and then tests, against the SCREEN dimensions
at `main+0x37E1F`/`+0x37E23`:

```
0x41CF8B  x == 0            -> eye.x -= step        (or hotkey 0xF4, tested 0x41CF7A)
0x41CFC8  x == screenW - 1  -> eye.x += step        (or hotkey 0xF6, tested 0x41CFA5)
0x41CFE9  y == 0            -> eye.y -= step        (or hotkey 0xF5, tested 0x41CFCE)
0x41D021  y == screenH - 1  -> eye.y += step        (or hotkey 0xF7, tested 0x41CFFF)
0x41D054  call 0x41C3C0                             ; the eye clamp
```

The `GetCursorPos` at `0x41CEE7` is **not** that test. It runs first and only handles the pointer
having gone 1..100 px *past* the screen: `(x >= screenW || y >= screenH) && x < screenW+100 &&
y < screenH+100`, and then — if `GetFocus` (IAT `0x4FC2E0`) matches `[obj+0x40]` — it overwrites
`[esp+0x1C]`/`[esp+0x20]` with the polled position clamped to `screenW-1`/`screenH-1`. So the
edge scroll fires off whatever `[obj+0x196]` holds, which is why answering the true pointer at
the three drawing polls is what makes the right edge work at zoom > 1 (`gpu-status.md` §2.3c).

## The cursor chain — mapped by us

[MEASURED 2026-09-03, this project — disassembly of the pristine Steam build (`objdump -d -M
intel`) plus live `tacli peek` reads on a running game. Established while fixing "selecting a
unit does not give the move cursor" (`field-notes.md`, our patch 2). Every VA below was read
off `objdump` in this session unless a row says otherwise.]

**The chain, top to bottom.**

| VA | What it is | How established |
| --- | --- | --- |
| `0x490B30` | `SetInputMode(mode)` *[INFERRED]*. Stores `mode` at `main+0x391F1` and installs the matching per-event handler into `main+0x391F5`, off a jump table at `0x490C14` (modes 0–7). 11 call sites. **Mode 6 is in-game** — the value a dozen sites elsewhere test for (`0x4289F1`, `0x45053A`, `0x476C75`, `0x476CA5`, `0x476E5B`, `0x478DB6`) | disassembly |
| `0x499200` | the **mode-6 (in-game) mouse handler**: `rep movsd` six dwords from `main+0x2C76` onto the stack, `call 0x498DA0`, then choose and set the cursor. **No `call` site anywhere**: the 4-byte literal `0x00499200` occurs in the image only as the immediate of the two `mov dword [main+0x391F5],0x499200` stores at `0x490BC5` (mode 6’s arm) and `0x498455` | disassembly + a scan for the literal across all sections |
| `0x498DA0` | mouse → world point, map cell and hovered feature. One call site, `0x499221` — the one `vpwide` redirects (`gpu-status.md` §2.3b) | disassembly |
| `0x48CD80` | `GetUnitAtMouse`, no arguments, `ret`; its return is stored as a WORD to `main+0x2CBA` at `0x499283`. Callers `0x491D18` and `0x499278`; **in build placement the handler skips it** (`0x499241` jumps past `0x499278`). It reads the dispatched point `main+0x2C76`/`+0x2C7A`. Inside the viewport rect `main+0x37E27` (`0x4B6720`) it walks the HotUnits ids (`main+0x1435F`, count `+0x14367`), skips a unit whose `+0xA6` is 0, hit-tests each through `0x48C6A0(unit, &point)`, ranks the hits by a key computed from the `+0x92` record's `+0x176`..`+0x17E` [the key's meaning not traced], and returns the winner's `+0xA8` word. Inside the minimap rect `main+0x142BB` it returns the id of the nearest 10-byte entry of `main+0x14363` (count `+0x1436B`) whose squared distance from the point is below 4. Otherwise 0. Its own stores go to its stack frame | disassembly, 2026-10-25 |
| `0x48D220` | `CorretCursor_InGame(orderByte)` — returns the cursor index to show. Two call sites: `0x491D36` and `0x499297`. `ret 4` | name [CORPUS] (TADR, `tools/ta_symbols.txt`); body from disassembly |
| `0x43E490` | the per-candidate cursor mapper: `(orderByte, candidate, hoveredUnit, &worldPos)` → cursor index, `ret 0x10`. **Exactly one caller, `0x48D3E4`**, and the literal `0x0043E490` appears nowhere in the image — no dispatch table, no indirect call | disassembly + literal scan |
| `0x4AB400` | `SetUICursor(uiCtx, gafSequence)` — 12 call sites, of which **five are inside `0x499200`**: `0x4992C8` (the cursor block below) plus `0x499560`, `0x4995A3`, `0x49966D`, `0x4997F7` in its button handling | name [CORPUS]; call sites from disassembly |

**What `0x499200` does, and the gate that decides whether a cursor is chosen at all.** This is
the head of the handler, not the whole of it: there is no `ret` between `0x499200` and the `c3`
at `0x499876`, so `done` below means "jump to `0x4992CD`", where the same function carries on
into `call 0x41C180`, the `main+0x2CC6 & 0x20` test at `0x4992D8` and the mouse-button handling
— which is where two of the four ordering readers of `main+0x37EFA` (`0x499352`, `0x499567`)
live. *[Extent FROM REVIEW 2026-09-03; the block below was first written as though each `done`
returned.]*

```
edx  = main                       ; ds:0x511DE8
copy 6 dwords main+0x2C76 -> stack ; the mouse POINT and four more dwords
call 0x498DA0(&copy)              ; fills the world point / cell / hovered feature
cl   = main[0x2CC6]
if ((cl & 2) && main[0x2CC3] == 0x0E) { call 0x4197D0; done }   ; build placement mode
if (!(cl & 2) && !(cl & 1)) {                                  ; pointer on NEITHER region
    if (main[0x2CBE] != 0x13) { main[0x2CBE] = 0x13;
                                SetUICursor(main+0x519, *(main+0x148CB)); }   ; cursornormal
    done
}
main[0x2CBA] = (WORD)0x48CD80()                       ; the unit under the cursor
eax = CorretCursor_InGame(main[0x2CC3])               ; the index it wants
if (eax != main[0x2CBE]) { main[0x2CBE] = eax;
                           SetUICursor(main+0x519, *(main+0x1487F + eax*4)); }
```

Two things fall out. The store-if-changed guard at `0x4992A2` is the address TADR's corpus
names `SuppressShowReclaimCursorAddr` — independent corroboration that `main+0x2CBE` is the
current cursor index. And `*(main+0x148CB)` is `cursor_ary[0x13]`, which the loader table below
independently makes `cursornormal`: the off-region default and index 19 agree.

**`0x498DA0` and the mouse-region bits of `main+0x2CC6`.** The function tests the pointer against
two rects with `0x4B6720` (point-in-rect *[INFERRED]*) and records which one it landed in:

| bit | Meaning | Where |
| --- | --- | --- |
| `0x01` | pointer is inside the rect at `main+0x142BB` — the minimap's click area *[INFERRED]*: it is the rect this arm scales the pointer by the map size against, and it sits immediately below the minimap view RECT at `main+0x142CB` (`gpu-status.md` §2.5) | set `or bl,1` at `0x498DE4`; cleared `and bl,0xFE` at `0x498E8E`. **Disassembly only — not confirmed live** |
| `0x02` | pointer is inside the **world viewport** rect `main+0x37E27` | cleared `and …,0xFD` at `0x498E26` on the minimap path; set from `PtInRect` at `0x498EAD..0x498EBC`. Live: reads **6** with the pointer anywhere on the world, **0** on the lower side panel |
| `0x04` | set when **bit 0 or bit 1** is — "the pointer is on one of them" | computed at `0x498ECE..0x498EE6` |
| `0x08` | already documented elsewhere; gates the minimap branch at `0x498DD5`: while it is set the minimap rect is not consulted at all. `ui-markers.md` §4 has it as the drag/band "rect forced on" bit; the two readings are consistent | disassembly |

The minimap arm scales the pointer by the minimap rect (`main+0x142E7..0x142ED`) against the
scroll extent (`main+0x1422B`/`+0x1422F`, the map less 32 and 128 px — §"Engine defects we
patch"); the viewport arm is the `world = eye + clamp(pos, L, R) − L`
form already documented in `gpu-status.md` §2.3b. The tail converts the world point to a cell
pair at `main+0x2C8E` (`>> 0x14`) and stores the **hovered feature id** as a WORD at
`main+0x2CBC` — live: `0xFFFF` over open ground, `87` over a lab wreck.

**`CorretCursor_InGame 0x48D220(orderByte)`.** It gathers candidates, maps each one and keeps
the lowest index:

- hovered unit: `main+0x2CBA` != 0 → `unit = *(main+0x14357) + id*0x118` (`0x48D242`: `id*8 − id`,
  then `*5`, then `*8` — 280 = `0x118`, the same unit stride `gpu-status.md` §2.5 records);
- the local player's selected units: `esi = main + 331*p + 0x1B63` where `p = main[0x2A42]`
  (`0x48D280..0x48D292`: `p + (33p)*5*2 + 0x1B63`), then walk `[esi+0x67] .. [esi+0x6B]` in
  `0x118` steps, calling `0x48DDC0` for each entry whose `[unit+0x110]` has bit 4 set. The
  begin/end pair is the same `PlayerStruct+0x67/+0x6B` `ui-markers.md`'s appendix records for
  the order-marker walk; bit 4 meaning "selected" is *[INFERRED]* from the use;
- the hovered unit adds its own candidate through `0x480100`;
- then `for each candidate: idx = min(idx, 0x43E490(order, cand, hovered, main+0x2CAA))`,
  starting from `0x13`. **With no candidates at all** it returns `0x0F` when the hovered unit is
  the player's own and passes four field tests (`+0xFF == player`, `+0x110 & 0x20`,
  `+0x104 < *0x4FD758`, `+0xFB == 0`), else `0x13`.

**The order → case jump table at `0x43F0A8`.** `0x43E490` dispatches on `orderByte − 1`, `ja
0x43F098` above 13, so **only order bytes 1..14 have cases**:

| order | case VA | order | case VA |
| --- | --- | --- | --- |
| 1 | `0x43E505` | 8 | `0x43E5FA` |
| 2 | `0x43E8BB` | 9 | `0x43E5DE` |
| 3 | `0x43E545` | 10 | `0x43F098` (the default) |
| 4 | `0x43E850` | 11 | `0x43E8AE` |
| 5 | `0x43E80C` | 12 | `0x43E65C` |
| 6 | `0x43E7D3` | 13 | `0x43E797` |
| 7 | `0x43E615` | 14 | `0x43E828` |

`main+0x2CC3` holds the current order byte. Measured live by clicking each order button and
reading it back: **1** = contextual (no command button pressed), **2** = Move, **3** = Attack,
**7** = Guard, **8** = Repair, **9** = Patrol, **12** = Reclaim, **13** = Capture, **14** =
build placement (the `0x0E` that `ui-markers.md` §4 already records as "build mode").

**The Interface Type gate — the bug.** Order 1's case opens with

```
0043E505  83 BB FA 7E 03 00 01   cmp dword [ebx+0x37EFA], 1     ; Interface Type
0043E50C  0F 84 F0 05 00 00      je  0x43EB02                   ; right-mouse-orders branch
```

and `0x43EB02` can only ever return `0x0F` `cursorselect`, `0x11` `cursorred`, `0x12`
`cursorgrn` or fall through to `0x13` `cursornormal`. The classic branch at `0x43E512` instead
re-dispatches: `[esp+0x18] = 3` at `0x43E520` and `= 0xC` at `0x43E53B` rewrite the order byte
and jump back to `0x43E49F`, so the contextual cursor becomes the **attack** and **reclaim**
cases, with the move case reached through `0x43EDB6`.

Measured on a **stock** instance (nothing armed), commander selected, on `shadow-mix`:

| pointer over | Interface Type 1 | Interface Type 0 |
| --- | --- | --- |
| empty ground | `0x13` `cursornormal` | `0x0E` `cursormove` |
| lab wreck | `0x12` `cursorgrn` | `0x0B` `cursorreclamate` |
| own building | `0x0F` `cursorselect` | `0x0F` `cursorselect` |

`main+0x37EFA` is the `Interface Type` registry value (already in `resolution.md`), read at
`0x42F9AF` and clamped to ≤ 1 at `0x42F9CD`. It is **read at nine sites**: `0x42F9F6` and `0x430F08` (the settings
round-trip), `0x45EF6D` (the options screen pushing the value into its `LEFTCLICK` gadget,
`0x4A1080("LEFTCLICK", value)`), `0x43E505` (this one, cursor only), `0x43F9EF`, and `0x499046`
/ `0x499162` / `0x499352` / `0x499567` — the last two inside `0x499200`'s own button handling.
Those four are where left-vs-right ordering lives *[INFERRED from their position in the input
path; not traced instruction by instruction]*.

**The licence for the patch is the one-caller fact above, not this enumeration.** `0x43E490`
contains no store to `main` at all and is reached from exactly one place, so neutering
`0x43E50C` changes which sprite is chosen and can reach nothing else — that holds however many
readers of `main+0x37EFA` turn out to exist. *[The ninth reader is FROM REVIEW 2026-09-03; the
list first said eight and the split was called "the whole licence", which overstated what an
enumeration can prove.]*

**`cursor_ary` — index → GAF sequence.** Base `main+0x1487F`, entry `idx*4`, matching
`ui-markers.md`'s `cursor_ary[0x15]` [CORPUS]. The mapping is read out of the loader at
`0x429C84..0x429E94`, which opens the `cursors` GAF (`0x429700`, handle to `main+0x14903`) and
then calls `0x4B8D40(handle, name)` once per sequence, storing **the previous call's** result —
so the name pushed at a site belongs to the offset stored at the *next* one:

| idx | name | idx | name | idx | name |
| --- | --- | --- | --- | --- | --- |
| 1 | `cursorattack` ✔ | 8 | `cursorpickup` | 15 | `cursorselect` ✔ |
| 2 | `cursorairstrike` | 9 | `cursorteleport` | 16 | `cursorfindsite` |
| 3 | `cursortoofar` | 10 | `cursorrevive` | 17 | `cursorred` |
| 4 | `cursorcapture` | 11 | `cursorreclamate` ✔ | 18 | `cursorgrn` ✔ |
| 5 | `cursordefend` ✔ | 12 | `cursorload` | 19 | `cursornormal` ✔ |
| 6 | `cursorrepair` | 13 | `cursorunload` | 20 | `cursorhourglass` |
| 7 | `cursorpatrol` ✔ | 14 | `cursormove` ✔ | 21 | `pathicon` |

Index **0** (`main+0x1487F` itself) is written by nothing in that loader and was not chased.
✔ marks the eight confirmed by behaviour rather than by the loader's instruction order alone —
seven by driving the game and reading `main+0x2CBE` back, and `cursormove` and
`cursorreclamate` additionally by reading their sprites out of the GL framebuffer. `pathicon` at
21 agrees with `ui-markers.md` §3, which already had the route dots at `main+0x148D3`.

**How the installed cursor animates** (DISASSEMBLED 2026-10-27, for the in-game wiki's stage,
which draws the same frames by the same rule: `tagpu_wikistage_cursor_frame`).

| VA | What | How established |
| --- | --- | --- |
| `0x4AB400` | `SetUICursor(ctx, seq)`, `ret 8`, ctx `main+0x519` at every in-game site: `ctx+0x2C = seq` (`0x4AB410`), the animation state at `ctx+0x30` started by `0x4B8B30(ctx+0x30, seq, 0)` (`0x4AB414`), `ctx+0x5C` bit 0 set (animating, `0x4AB41D`), then the frame `0x4B7EE0` names handed to `0x4C2B20` (`0x4AB422..0x4AB428`). It starts the sequence over on every call; each in-game caller stores `main+0x2CBE` and calls only when the index changed (above), so an unchanged cursor keeps animating | disassembly |
| `0x4B8B30` | animation state init `(state, seq, frame)`, `ret 0xC`: `+0x00` the frame (0 when the asked one is not below the sequence's u16 count `seq+0`, `0x4B8B40..0x4B8B4E`), `+0x08` the sequence, `+0x02` the countdown = that frame's u16 ticks `seq+0x2C+8·frame` (`0x4B8B5B`; `0xFFFF` with no sequence), `+0x04` the loop byte `seq+2` | disassembly |
| `0x4B8BF0` | animation advance `(state, dt)`, `ret 8`: returns at once with no sequence or a count ≤ 1 (`0x4B8BFB`, unsigned); else subtracts dt from the signed 16-bit countdown (`0x4B8C06`) and, while it is ≤ 0, steps the frame (`0x4B8C11`) and adds the new frame's ticks (`0x4B8C3D`). Past the last frame it goes back to 0 when the loop byte is set (`0x4B8C23`), else clears the sequence (`0x4B8C4B`): nothing is drawn after a non-looping sequence ends | disassembly |
| `0x4B8B90` | a one-tick step of the same state: a countdown of 2 or more is decremented (`0x4B8BE1`); below that the frame steps with the same wrap or clear and the countdown is reloaded [its callers not traced] | disassembly |
| `0x4B7EE0` | the state's current frame: `[seq + 0x28 + 8·frame]`, 0 with no sequence | disassembly |
| `0x4AB5D0` | the UI context's update, from `0x4AA00A` in `0x4A9FD0`: with `ctx+0x5C` bit 0 set it calls `0x4B8BF0(ctx+0x30, ctx+0x9A)` (`0x4AB5F0`) and, when the frame changed, hands the new one to `0x4C2B20` (`0x4AB5FF..0x4AB605`) | disassembly |
| `0x4A9FD0` | the GUI pump: `ctx+0x9A = 0x4B6340() − ctx+0x96`, then `ctx+0x96` = now (`0x4A9FEE..0x4AA004`): the cursor counts down in the 30 Hz ticks of `0x4B6340` elapsed since the last pump | disassembly |

So a frame's second dword in the GAF's sequence table is its duration in 30 Hz ticks, read as a
u16, and the sequence's byte `+2` is its loop flag. In `cursors.gaf` (retail, `rev31.gp3`):
`cursornormal` is one 10x20 frame, hotspot 0,0; `cursormove` is eight frames of 8 ticks, looping,
27x27 to 35x35 and back (hotspots 13,13 to 17,17), frames 5..7 repeating 3..1 texel for texel.

**Which cursor build placement shows** (MEASURED 2026-10-27, `main+0x2CBE` read back on Show Down,
1024x768, Interface Type 1, through `tacli peek`): nothing selected, 19 `cursornormal`; an ARMCK
selected, over ground, 14 `cursormove`, the sprite record changing between reads (animating);
ARMMEX armed from the build menu (`main+0x2CC3` = 14), at every probe point over the map, 19
`cursornormal`, the record constant. It follows from the gate above: in mode `0x0E` over the
viewport `0x499200` calls the build cursor `0x4197D0` and installs nothing, and `0x4197D0` calls
no `SetUICursor`, so the cursor is the one the click on the menu left — `cursornormal`, which the
handler installs off both regions (`0x49925D`). The same holds for the twin `0x491CC0`. The mapper's
order-14 case `0x43E828` (`cursorfindsite`, 16) is reached only through `CorretCursor_InGame`,
which build placement over the viewport skips. **Not measured:** a constructor over a deposit with
Impure's Quick Build ghost showing; Quick Build writes only the packet (`tagpu_resource.c`), so
the engine's contextual cursor stands [INFERRED: `cursormove`].

**`0x429700` serves the cursors** as it serves every animation bank: `0x429C9A` asks it for
`"cursors"`, and a miss builds `<anims>\cursors.GAF` (`0x502E30`) through `0x4290F0`, the
language variant first, and loads it with `0x4B8C60` (the section on the animation-bank cache
above). The wiki's asset load resolves the same path (`tagpu_wikiasset.c` `cursors_load`).

**Globals this chain owns.**

| Field | What |
| --- | --- |
| `main+0x2CBA` | WORD, unit under the cursor (0 = none). Written at `0x499283` |
| `main+0x2CBC` | WORD, **feature under the cursor** (`0xFFFF` = none). Written at `0x498F5D` |
| `main+0x2CBE` | BYTE, **the cursor index currently installed**. Written at `0x49925D` / `0x4992AD` |
| `main+0x2CC3` | BYTE, current order byte (the jump-table selector above) |
| `main+0x2CC6` | BYTE, mouse-region flags — bits 0/1/2 above, 3/6 in `ui-markers.md` §4 |
| `main+0x2CAA` | the world point under the cursor, filled by `0x484B50` from inside `0x498DA0` |
| `main+0x2C8E` / `+0x2C90` | the map cell pair, `world >> 0x14` |
| `main+0x391F1` / `+0x391F5` | input mode, and the handler pointer for it |
| `main+0x14903` | the `cursors` GAF handle |
| `main+0x2A42` / `+0x2A43` | the controlled player index / the viewed player index, whose LOS bit `0x43EBF4` tests (`shl 1, cl`, `cl` = `+0x2A43` at `0x43EBE5`); `+View` writes only `+0x2A43` |

**Negative results worth the line.**

- **`0x43E490` has one caller and no address literal in the image.** That is what makes a patch
  there provably cursor-only, and it is the fact the whole fix rests on.
- **`0x499200` is never called.** Only installed, by mode 6. Anything hunting for "where the
  cursor is chosen" by following calls will not find it.
- **The handler is dispatched by `call dword [eax+0x391F5]` at `0x499A1C`.** Of the 24
  references to that displacement in `.text`, 23 are stores — the nine arms of `0x490B30`'s mode
  table plus `0x491617`, `0x49297E`, `0x492A7F`, `0x496AFF`, `0x496B94`, `0x496BF6`, `0x496C3D`,
  `0x496CA4`, `0x496D75`, `0x496D9D`, `0x496DFF`, `0x498457`, `0x4996DF`, `0x499852` — and that
  one is the read. *[FROM REVIEW 2026-09-03: this line first claimed there was no reader at all.
  It was written off a `grep | head -12`, so the twelfth match was the last one seen and the
  absence was an artefact of the pipe. Confirmed by re-running it unbounded.]*
- **Interface Type is not one switch with one meaning.** It gates the cursor at exactly one
  site and ordering at four others, and those are independent — which is why the game can be
  left on right-mouse orders and still show the classic cursors.
  *[CORRECTED 2026-09-07: independent is exactly what they are NOT. The cursor site chooses the
  index, `0x4992AD` stores it in `main+0x2CBE`, and the left click's own dispatch reads that
  byte at `0x499027` — so changing the sprite changed the button. The section below is the
  path this line missed, and it cost a shipped bug: from G13j until 2026-09-07 a left click at
  Interface Type 1 issued a move order instead of deselecting.]*
- **The contextual cursor is the only one the interface type touches.** Order bytes 2, 3, 9, 12
  … reach their cases without consulting `main+0x37EFA`, so the Move/Attack/Patrol/Reclaim/Guard
  buttons produced correct cursors at Interface Type 1 before the patch. Measured.

## The in-game mouse buttons — what a click actually does — mapped by us

[MEASURED 2026-09-07, this project — `objdump -d -M intel` of the pristine Steam build, plus a
live A/B on `tacli` instance `clickfix` (Two Continents, one commander, Interface Type 1 and 0).
Established while fixing "left click and right click both issue a move order", which turned out
to be our own contextual-cursor patch (`field-notes.md` patch 2) reaching further than its
licence said. Read together with the cursor chain above: this is the same handler's tail.]

**`main+0x2CBE` is not a picture, it is the click's state.** That is the whole finding. The
cursor index the chain above computes is stored at `0x4992AD` and then *dispatched on* by the
left button, so "which sprite is shown" and "what the left button does" are one decision in
this engine, made once per mouse move and consumed on the next press.

**The dispatch.** `0x499200` — the mode-6 handler, whose head chooses the cursor — carries on
past `0x4992CD` into the button handling, with the **message id in `[esp+0x1C]`** (`0x201`
`WM_LBUTTONDOWN` … `0x205` `WM_RBUTTONUP`) and the region/drag flags in `main+0x2CC6`:

```
0x499348  al = main[0x2CC6]
          if (al & 0x10) {                       ; a button is held on the minimap
0x499352      it = main[0x37EFA]                 ; Interface Type
              if (msg == (it ? 0x202 : 0x205))   ; ITS release: left at type 1, right at type 0
                  { main[0x2CC6] &= ~0x10; }     ; drag over
              else call 0x41D0F0                 ; keep dragging  [INFERRED: the view scroll]
              done
          }
0x499387  if (main[0x2CDF]) { call 0x41CD50; done }         ; another mode owns the mouse
0x49939B  if (msg == 0x204) { call 0x499100(&rec); done }   ; RBUTTONDOWN — below
0x4993B6  if (main[0x2CC3] != 1)                            ; a command button is pressed
              { if (msg == 0x201) call 0x498F70(&rec); done }   ; LBUTTONDOWN executes it
0x4993D5  if (al & 0x08) {                        ; a drag-select box is open
              if (msg != 0x202) { update the box corners; done }
0x4993E9      main[0x2CC6] &= ~0x08
              if (now < main[0x2CB6] + 0x19 && |dx| < 0x20 && |dy| < 0x20)
                  call 0x498F70(&rec)             ; short and still: a CLICK, not a drag
              else if (!0x48C390(&rec))           ; box select; nothing caught ->
                  { 0x48BD00(); 0x491D70(1); }    ; deselect everything
              done
          }
0x4994A0  if (msg != 0x201) done                  ; from here: LBUTTONDOWN, contextual order
0x4994AC  if (al & 0x02)                          ; on the world viewport
              { main[0x2CC6] |= 0x08;             ; open a drag-select box, remember when/where
                main[0x2CB6] = 0x4B6340(); … ; cursor 0x13; done }
0x499567  if (main[0x37EFA] == 1)
              { if (al & 0x01) main[0x2CC6] |= 0x10; done }   ; minimap: start the drag
0x4995AA  if (al & 0x01) call 0x498F70(&rec)      ; type 0, minimap: act on it
```

So **`main+0x2CC6` bit 3 (`0x08`) is "a drag-select box is open"** — set on `WM_LBUTTONDOWN`
over the viewport at `0x4994B4`, cleared on the release at `0x4993E9` — and **bit 4 (`0x10`) is
"a button is held on the minimap"**, set at `0x49957x` / `0x49919D` and cleared only by that
button's own release. Both extend the bit table in the cursor section above; bit 3 agrees with
`ui-markers.md` §4's "drag/band rect forced on".

**A left click is a short, still drag.** There is no click event: the press opens a box and the
release decides. `0x4B6340` is the clock it times that with — `GetTickCount()` (import
`0x4FC0DC`, resolved from the import table) multiplied by `*(0x51FBD0)+0xE8` and divided by
1000 (`0x4B6340..0x4B6361`), so it counts wall time in units of one rate-th of a second; the rate
`*(0x51FBD0)+0xE8` was MEASURED at 30 (2026-10-28), so the `0x19` = 25 units are about 833 ms.
The press (`0x4994B4..0x499533`) stamps `main+0x2CB6` with the clock (the call at `0x4994BC`, the
store `0x4994C7`) and stores the cursor's world point -- the words `main+0x2CAC`, `+0x2CB0`,
`+0x2CB4`, x, height and z -- at `+0x2C92`, `+0x2C96`, `+0x2C9A` and again at `+0x2C9E`,
`+0x2CA2`, `+0x2CA6`. While the box is open every message but the release rewrites the second
three from the current point (`0x49946A..0x49949B`). The release (`0x4993E9..0x499441`) compares
`|+0x2C92 − +0x2C9E|` and `|+0x2C9A − +0x2CA6|` with 0x20: the world x and z of the press against
the last point before the release, so the 32 are world units on each axis, and a cursor that
wanders and comes back still clicks. DISASSEMBLED. The wiki's simulations take the same window
(`sim.js` `CLICK_TIME`, `CLICK_BOX`) at both Interface Types, in ticks, 30 a second.

**`0x498F70` — the left click's action, and the only reader of `main+0x2CBE` that is not a
change test.** One call site, `0x4995B3`, which every left-button path above jumps to.

```
0x498F70  cl = main[0x2CC3]                       ; the order byte
          if (cl == 0x0E) { …build placement…; done }
0x499027  dl = main[0x2CBE]                       ; THE INSTALLED CURSOR INDEX
0x49902D  if (dl == 0x0F) { 0x48C7F0(&rec); done }        ; cursorselect -> select that unit
0x499041  if (dl < 0x11) goto act                         ; an ACTION cursor -> issue the order
0x499046  if (main[0x37EFA] == 1 && cl == 1)
0x49905C      { 0x48BD00(); 0x491D70(1); done }           ; -> deselect everything
          done                                            ; -> nothing at all
0x49906D  act: 0x48CF30(&rec, cl, main+0x2CAA, 0, 0, 0)   ; issue order `cl` at the world point
          main[0x2CC3] = 1; clear 0x2CC6 bit 5; reset STOP's radio ("STOP" 0x502714)
```

`cmp dl, 0x11` is the whole of the left button's interface-type behaviour, standing on the fact
that **`0x43EB02` — the Interface Type 1 arm of the contextual case — returns 15, 17, 18 or 19
and nothing else**. Verified by walking every branch reachable from `0x43EB02` (220 blocks) and
collecting the returns: `0x43EB56` → 15, `0x43EB67` → 17, `0x43EB78` → 18, `0x43EC8D` → 18,
`0x43EDA9` → 18, `0x43EE42` → 15, `0x43F09B` → 19. The action indexes (`0x43EDDF` → 6 repair,
`0x43EF5B` → 10 revive, `0x43F06F` → 11 reclaim, and the computed 14-or-19 at `0x43F07C`) all
belong to the classic arm and are **not reachable** from `0x43EB02`. So at type 1 the engine's
own rule is exactly "15 selects the unit, everything else deselects", and the compare expresses
it without a second interface-type read.

**`0x499100` — the right button down.** Also one call site, `0x4993AC`.

```
0x499100  if (main[0x2CC3] != 1)                  ; a command button is pressed ->
              { main[0x2CC3] = 1; clear 0x2CC6 bit 5;     ; right click CANCELS it
                reset STOP's radio (0x49FE60(menu, "STOP" 0x502714), 0x4A6A40); done }
0x499162  if (main[0x37EFA] != 0) {               ; Interface Type 1: right orders
0x4991D5      if (main[0x2CC6] & 0x04)            ; pointer on the viewport or the minimap
                  0x48CF30(&rec, 1, main+0x2CAA, 0, 0, 0)   ; issue the contextual order
              done
          }
0x499172  if (al & 0x02)                          ; type 0, on the world:
              { (rec[8] & 8) ? 0x41CC60() : (0x48BD00(), 0x491D70(1)); done }  ; deselect
0x499199  if (al & 0x01) main[0x2CC6] |= 0x10     ; type 0, on the minimap: start the drag
```

Note the asymmetry: the right button issues its order with a **hard-coded order byte 1**, the
left button with `main+0x2CC3`. The right button therefore never executes a pressed command
button — it cancels it — while the left button always does.

**The helpers, and how each was established.**

| VA | What | How |
| --- | --- | --- |
| `0x48CF30` | issue the order — `(rec, orderByte, &worldPoint, 0, 0, 0)`, the one call both buttons make | **16 call sites in the image**; the two on this path are `0x499087` (left) and `0x4991F5` (right). Plus live: the click that reaches it walks the unit to the point clicked. *[The count was “two call sites” until the 2026-09-07 review; two is how many are in THIS path, not in the image.]* |
| `0x48BD00` | **deselect everything** — walks the whole unit array `main+0x14357 .. main+0x1435B` in `0x118` steps clearing bits `0xD0` of `[unit+0x110]`, whose bit 4 is "selected" | disassembly; live, the selection drops (`ARMCOM1.GUI` → `ARMMAIN2.GUI`) |
| `0x491D70(1)` | the selection-changed UI refresh *[INFERRED]*; reads the GUI-variant word `main+0x37EBE`. Always called immediately after `0x48BD00` | call sites |
| `0x48C7F0` | select the unit under the cursor *[INFERRED]* — reached only from the `0x0F` arm | call site + live: a left click on an own unit selects it at either interface type |
| `0x48C390` | the drag-box selection *[INFERRED]*; returns 0 when the box caught nothing, which is what turns an empty drag into a deselect | call site |
| `0x4B6340` | `GetTickCount() * (*(0x51FBD0)+0xE8) / 1000` — the click/drag timer | disassembly + the import table |

**The behaviour table, measured live** on `clickfix` (commander selected, `one-unit` on Two
Continents), with our contextual-cursor patch armed *and* the companion patch below:

| | Interface Type 1 | Interface Type 0 |
| --- | --- | --- |
| left click, empty ground | deselects, no order | **issues the move order** |
| left click, own unit | selects it | selects it |
| left click, wreck | deselects, no reclaim | reclaims |
| right click, empty ground | **issues the move order** | deselects |
| right click, wreck | reclaims | deselects |
| Move button, then left click | issues the move order | issues the move order |

**Our patch — 27 bytes at `0x499041`,** armed with the cursor patch and under the same
`tagpu_curs.off`. It decides the *contextual* left click on the interface type directly instead
of on the cursor index that used to stand in for it, in the same 27 bytes
(`0x499041..0x49905B`, the three destinations all pre-existing):

```
00499041  cmp dword [eax+0x37EFA], 1      ; Interface Type
00499048  jne 0x499051                    ; type 0: classic, decide on dl
0049904A  cmp cl, 1                       ; order byte: contextual?
0049904D  jne 0x499051                    ; a pressed command button acts
0049904F  jmp 0x49905C                    ; -> deselect  (the engine's own arm)
00499051  cmp dl, 0x11                    ; the classic test, unchanged
00499054  jl  0x49906D                    ; -> issue the order
00499056  jmp 0x4990F6                    ; -> nothing; 0x4990F6 is `pop esi / pop
0049905B  nop                             ;    ecx / ret 4`, this function's exit
```

`0x49905C`, `0x49906D` and `0x4990F6` are the same three destinations the stock code branches
to; only which test picks them changes, and `0x499051` is the one new label — the stock `cmp
dl,0x11` moved 16 bytes down. Equivalent
to stock on a stock cursor state — at type 0 it is the original test unchanged, and at type 1
the only indexes the engine can leave in `main+0x2CBE` are 15 (taken above it), 17, 18, 19 and
the hourglass 20, and all of those stock deselects when the order byte is 1.

**What makes that enumeration sound is not the list of stores — it is that the click always
reads an index this same message just produced.** *[CORRECTED 2026-09-07 by review: the first
version of this paragraph argued from a list of the direct stores to `main+0x2CBE`, which is not
a closed set — `0x491C80` is a generic `SetCursor(idx)` with **42 call sites**, two of which
(`0x491C95`, `0x491D4C`) store a caller-supplied index. The list also omitted `0x491CF8` and
`0x4991B4`, and filed `0x492593` under 19 when `0x492589` loads `cl` with `0x14`.]* The real
guarantee is structural, and it is in `0x499200`'s head: every message that can reach the click
passes through it first, and it has exactly three exits —

| head exit | what it leaves in `main+0x2CBE` | why the click is safe |
| --- | --- | --- |
| build placement (`0x499238`: region bit 1 and order byte `0x0E`) | untouched | `0x498F70` returns at `0x498F80` on order `0x0E`, before the read |
| pointer on neither region (`0x49924B`) | `0x13`, stored at `0x49925D` | ≥ `0x11` |
| otherwise (`0x499278`) | `CorretCursor_InGame`'s return, stored at `0x4992AD` | the type-1 arm returns only 15/17/18/19 |

So whatever any other setter in the image left there is overwritten before the button tail runs,
and only these three sources can be what `0x499027` reads.

**Negative results worth the line.**

- **`main+0x2CBE` has exactly one behavioural reader.** It has **28** `.text` references: 13
  stores, 13 store-if-changed tests immediately in front of one (`0x491C8A`, `0x491CEC`,
  `0x491D41`, `0x49258B`, `0x496A67`, `0x497F94`, `0x4991AC`, `0x499254`, `0x4992A2`,
  `0x49953E`, `0x499581`, `0x49964B`, `0x4997D5`; `0x499254` compares with 19 directly, while
  `0x4992A2` loads the byte with `movsx` and `0x4992A9` compares it with the index `0x48D220` returned
  for the mode `main+0x2CC3`), the save at `0x443702` that `0x4437B5`
  restores through `0x491C80`, and `0x499027` — the one place the value decides anything.
  **Two of the 13 stores take a caller's index rather than a literal** (`0x491C95`,
  `0x491D4C`, both inside `SetCursor 0x491C80`), so the set of *values* is not closed by this
  census; what closes it for the click is the table above. Of the literal stores, `0x49925D`,
  `0x499547`, `0x49958A`, `0x499654`, `0x491CF8` and `0x4991B4` write 19 and `0x492593`,
  `0x496A6F`, `0x497F9C`, `0x4997DE` write the hourglass 20. That is why the bug had a
  single 27-byte fix and not a rewrite.
- **`0x498F70` and `0x499100` have one call site each** (`0x4995B3`, `0x4993AC`), both inside
  `0x499200`. Neither address appears as a literal elsewhere. *[`0x499100`'s was written
  `0x49939B` until the 2026-09-07 review — that is the head of the block that tests the message,
  not the `call`.]*
- **There is no click message.** Nothing in this path handles `WM_LBUTTONDBLCLK` either; the
  engine's own dispatch covers `0x200..0x206` and the double-click arrives as another down.
- **The right button cannot execute a command button, by construction** — `0x499100` returns at
  its first test whenever `main+0x2CC3 != 1`, and its order call passes the literal 1.

**A build placement and the units it orders (B9).** A build button arms the placement: the build
menus' handler `0x41AA00` (address-taken, no direct caller) writes the order byte `main+0x2CC3` =
`0x0E` at `0x41AB89` and `BuildUnitID` `main+0x2CC4` at `0x41AB9C`. It is reached through the
GUI's dispatch `0x4A9FD0`, which IdleTick `0x499890` calls at `0x499992` before it enters the
state's handler (`0x499A1C`). **The placement's click orders the selection, not the tracked
unit**: `0x419670` walks the controlled player's block (`main+0x2A43` is the viewed player) —
record `main+0x1B63 + 0x14B·main+0x2A42`, units
`+0x67` to `+0x6B` inclusive (`0x41972A..0x419753`) — and orders every unit that is selected
(`+0x110` bit `0x10`, `0x419755`) and whose type (`+0x92`) has `+0x241` bit `0x40`
(`0x41975E..0x41976A`), through `0x438760` and `0x43AFC0` (`0x4197A4`, `0x4197A9`). The free clears
bits 4 and 5 of `+0x110` (`0x486DE8`), so a freed unit leaves that set.

The engine ends an order mode in six places, each writing `0x2CC3` = 1, clearing `0x2CC6` bit 5
(`0x20`, the order kept while a modifier is held; the release test is `0x4992D8`) and resetting
the radio group of the menu's STOP gadget — `0x49FE60(menu, "STOP")` (the name at `0x502714`)
finds it and `0x4A6A40` resets it: the right button `0x499110` (in `0x499100`, called at
`0x4993AC`), the modifier's release `0x4992FA`, a key's case of the in-game key switch
(`0x495ECD`, table `0x496694`) at `0x495F36`, a command mode's click `0x4990AE` (in the click
handler `0x498F70`, after `0x48CF30` has issued the command at `0x499087`, for a cursor type
`main+0x2CBE` below `0x11` other than `0x0F`; type `0x0F` hands the click to `0x48C7F0` and returns at `0x49903E`,
and from `0x11` up it returns at `0x4990F6` unless `0x37EFA` is 1 and the byte 1), and the placement's
own left click on a clear site `0x498FC0`, after `0x419670` has issued the build
(`0x498F93..0x498FA4`); and the helper `0x495860` (the write at `0x49586A`), whose six callers include
the group recall (below). In both clicks a press whose record has `+8` bit 2 keeps the mode and
sets the flag instead (`0x498FB4`, `0x499097`). A left click on a blocked site plays the refusal and keeps it
(`0x499016`). `0x4958D7`, in `0x4958C0`, writes the byte too: `0x4958C0` has no caller in the
image and its address is stored nowhere, and the reclaim sweep calls it as stock's click tail
(*The reclaim sweep's engine side*, below). The setter `0x419BC0`, which stores its argument at
`0x419BC9`, has no caller either. The command modes are armed by the orders menu's handler, one value per gadget
(the names compared through `0x4E49B0`): MOVE 2 (`0x419C6A`), ATTACK 3 (`0x419D44`), BLAST 4
(`0x419DB9`), UNLOAD 5 (`0x41A066`), LOAD 6 (`0x41A0D7`), DEFEND 7 (`0x419E2D`), REPAIR 8
(`0x419EA1`), PATROL 9 (`0x419F16`), RECLAIM `0xC` (`0x419F8A`), CAPTURE `0xD` (`0x419FFB`); pressing
the armed one again writes 1 (`0x419C52` and the like). `0xA` and `0xB` have no writer. **A command
mode's click** is `0x48CF30(msg, mode, …)` (from `0x499087`): it walks the same block, counts the
selected units other than the one under the pointer (`main+0x2CBA`) and returns when there are
none (`0x48CFDF..0x48D011`), then asks the order resolver `0x43F0E0` for each unit with the
click's target and position (`0x48D0A0`) and skips a unit it answers 0 for. None of the cancels writes `0x2CC4`; the load's UI reset
`0x4917D0` (called at `0x497581`) writes both, `0x4917F9` and `0x491805`. Every engine reader of
`0x2CC4` (`0x419686` in `0x419670`, `0x4197DD` in the build cursor `0x4197D0`) runs only with
`0x2CC3 == 0x0E`, and every reader of `0x2CC3` (`0x469DD8`, `0x469E51`, `0x469EF4`, `0x48D3DB`,
`0x491CD2`, `0x491D2F`, `0x495F2D`, `0x498F77`, `0x499107`, `0x499238`, `0x499290`, `0x4993B6`)
is the UI [DISASSEMBLED].

The per-frame check at `0x4995C3..0x4995F1`, after `GameFrame_InGame 0x496790` (`0x4995B8`),
reads the tracked unit; when its slot's `+0xA6` word is 0 (`0x4995E4`) it calls `0x491D70(0)`
(`push 0` at `0x4995EF`, the call at `0x4995F1`), which writes neither byte. **That call does not
always drop the unit**: when `0x37EBE & 0x865` or `0x2BEE & 0xE0` is set and the argument is 0
(`0x491D76..0x491D95`, the second flag test at `0x491D86`), it only sets `0x37EBE` bit `0x10` and returns (`0x491D97..0x491DA2`),
leaving the tracked unit and its menu up; the frame drops it later (`0x496986..0x4969C9`: through
`0x41B2E0` once the flags clear, or by zeroing `0x37E9C` while they hold). Otherwise it clears
`0x37E9C` (`0x491DA5`) and pops the GUI. The keys `0x495E90` write the byte only at their cancel
`0x495F36`, and **recalling a group disarms the placement**: the recall `0x48D9A0` (one caller,
`0x495FFD` in the key switch) ends with an unconditional `call 0x495860` at `0x48DBE2` (every
branch of the function rejoins before it; its one `ret` is `0x48DC10`), and `0x495860` writes
`0x2CC3` = 1, clears `0x2CC6` bit 5 and resets the STOP radio as the cancels above do
[DISASSEMBLED 2026-10-25]. MEASURED 2026-10-25: with a building armed for one ARMCK, `alt+3`
recalled a group and `0x2CC3` went from 14 to 1. On that instance the plain digits were weapon keys
(`main+0x37F07` bit 0 clear) and the groups went through Alt. `0x495860` has six callers
(`0x48BDE6`, `0x48BF09`, `0x48C00A`, `0x48C11E`, `0x48D721`, `0x48DBE2`); the other five were not
traced. **In stock a
placement outlives the units it would order** — MEASURED 2026-09-26 with
`tools/b6-tracked-death.sh` on main `8d033d1`: after the CORCK's death `0x2CC3` stayed `0x0E`
and `0x2CC4` 246, and the square and our build ghost stayed on the pointer.

**B9 checks it at two points of the frame, for a placement and for every command mode.** The in-play handler `0x499200` handles the mouse
first (the build cursor at `0x499241`, the cursor choice `0x48D220` at `0x499297`, the click
routing `0x4993B6`, the placement's click `0x498F70` at `0x4995B3`), then calls the frame
`0x496790` (`0x4995B8`), whose ticks (`0x495490` at `0x49680C`, `0x49693E`) free units, whose keys
(`0x495E90` at `0x496971`) recall groups, and whose draw `0x468CF0` (`0x4969CD`) reads the byte
and publishes our packet; the head holds no `ret` before `0x4995B8`. With the GUI's dispatch
before the handler, no one point follows every writer and precedes every reader, so
`order_check` (`tagpu_patches.c`) runs at two: the frame's `call 0x48BAE0` at `0x49697B`
(the stub tail-jumps to `0x48BAE0`, whose `ret` comes back to `0x496980`), which every path of
`0x496790` reaches (`0x49691E` and `0x49696F` jump to it; its one `ret` is `0x496A56`) after the ticks,
the keys and the scroll poll `0x41CE90` and before the cull and the draw; and `0x499226`, the
handler's first instruction after `0x498DA0` (the mouse's world position, `0x499221`; the stub runs
the stolen `mov edx,[0x511DE8]` and rejoins at `0x49922C`), after the GUI's dispatch and before the
head's first reader. When the byte is `0x0E` and the placement walk's own test, or a command
mode and `0x48CF30`'s first test (a selected unit), finds nobody, its values bounded (the
controlled player below 10, the block inside the array and on its stride, each type inside
`UNITINFOCount`), it calls `0x499100`: with the byte not 1 that is the right button's cancel for
every order, the same writes whatever the mode, and it reads nothing of its message. MEASURED on the fixed
build: see [the plan's B9](../tadr-port/sim-fixes.md).

**Where the cancel cannot be called.** `0x499100` reads the menu `[[main+0x531]+4]` at `0x49913B`
with no test, and passes it to `0x49FE60(menu, "STOP")` (`0x49913F`); the engine guards the same
read in `0x491D70` (`0x491DB3..0x491DBB`). Of `0x496790`'s three callers, the network game's end
`0x4996A5` runs after the stack was popped, possibly to NULL, and the loading screen's `0x49842F`
runs after the load's reset wrote the byte to 1 (`0x4917F9`), so `order_check` returns while
`main+0x531` is NULL. The other is a modal screen: under one, `0x49FE60` searches the modal
screen, returns -1, and the order button underneath stays drawn pressed with the byte 1. The
engine's own test for deferring menu work is `0x37EBE & 0x865` or `0x2BEE & 0xE0` — in
`0x491D70(0)` (`0x491D76..0x491D8D`) and in the frame before the deferred drop
(`0x496986..0x4969A4`, the drop `0x41B2E0` at `0x4969B4`). Its named bits: bit 0, the options
stack (`ARMOPT`, `EXITMENU`, `YESORNO`, the preferences), set at `0x4961C1` (the key that pushes
`ARMOPT`), `0x49477E` (beside `"OPTIONS"`) and `0x45D002` (`0x45CFC0`); bit 2, set at
`0x49412C`, the chat `TALK.GUI`, pushed by `0x494050` (`0x49410A`) from the Enter key's case
`0x4964FD` of the in-game key switch and cleared by its handler `0x493BF0` (`0x493C18`); bit 6,
`SHARE.GUI` (`0x49374F`). Bits 5 and
11 of the word and `0x2BEE`'s three were not identified. `order_check` defers on the same test,
and the replay HUD's `tagpu_menu_modal` (`tagpu_menu.c`) gives every click to the engine under
it, adding `YESORNO.GUI` found on the stack by name for the Leave question it opens outside the
options stack ([The replay HUD in game](../hud-in-game.html), *The menu over the world*). The build
pages stack on the base screen with neither bit set, so a test of the top screen's name would
not serve [INFERRED: the build pages' bits not traced].
In single player the frame skips the ticks while bit 0 is set (`0x496918` → `0x49697B`), so no
unit dies under the options stack; with `main+0x2A44` bit 0 [INFERRED: a network game] they run
(`0x4967E7`). The chat does not stop them: MEASURED 2026-09-26 with `tools/b9-under-chat.sh`, a
CORAK died under `TALK.GUI` with ATTACK armed; the word read `0x2006` with the chat up and `0x2016`
after the death (the drop pending), the byte stayed 3 until Escape closed the chat, then read 1,
and the engine's deferred drop took the dead unit's menu down (`CORMAIN2.GUI` on top).

**The remote give.** `UNITS_GiveUnit 0x488570` to a remote player builds a `0x14` record
(`0x488603`), sends it (`0x451DF0` at `0x488693`) and kills the unit at `0x4886A4` (`0x489BB0`)
over `0x4885E9..0x4886A4`; to a local player it creates the unit for the receiver (`0x488700`)
and kills the old one with 30 000 damage (`0x4887D0`). The capture reaches it at `0x4046C5`.

### The placement's mouse path, and several buildings in one gesture [DISASSEMBLED + MEASURED 2026-10-25]

What a build placement (order byte `0x0E`) does with one mouse record, in the order the in-play
handler `0x499200` runs it, and where `tagpu_place.c` hooks it ([gpu-status
§2.109](../gpu-status.html)).

**The record and the world point.** The head copies the six dwords of `main+0x2C76` to its stack
(`0x499210..0x49921A`, `rep movsd` into `[esp+0xc]`) and passes the copy to `0x498DA0`
(`0x499221`). `0x498DA0` reads only the copy's x and y, through `ebp` (`[ebp+0]`, `[ebp+4]`), and
writes no dword through it; its own stores into the main block are `+0x2CC6` (the region bits),
`+0x2C8E` (the cell, `0x498F2E`) and `+0x2CBC` (the feature, `0x498F5D`), and it hands `+0x2CAA` to
`0x484B50` as the out-parameter for the world point (`0x498EF1`). Nothing between the copy and the
build cursor writes `main+0x2C76`, so the record the build cursor's call sees and the copy the
click routing hands on hold the same six dwords: an observer of `0x4197D0` can tell the record it
evaluated by value. With region bit 1 and the byte `0x0E` the head calls `0x4197D0` (`0x499241`)
and jumps to `0x4992CD`, past the hover query `0x48CD80` and the cursor choice `0x48D220`. The
Shift-release test `0x4992D8` (bit 5 set and `0x4C1B80(0xF9)` up → the cancel at `0x4992FA`) runs
next, on the same record, before the click routing.

**The red test is the engine's own call.** The build cursor judges its site with whatever the
call at `0x4198C6` targets — stock `0x47D2E0`, or `build_site_test` when queued_site is armed
(`tagpu_patches.c`) — with the arguments `0x4198A0..0x4198C6` push: the def, the cell word, 0 and
the controlled player's record `main + 0x1B63 + main[0x2A42] · 0x14B` (`0x4198BA`, `lea
ecx,[ecx+eax*2+0x1b63]` with `ecx = main + p` and `eax = 165·p`). Its bit 0 becomes `0x2CC6` bit 6
(`0x4198D1..0x4198E1`), the bit the click tests at `0x498F86`.

**The click routing, byte for byte.** The message is the copy's fifth dword, `[esp+0x1C]`
(`0x49939B`). A right press is taken first (`0x49939F` → `0x499100`, the cancel). Then:

```
0x4993B6  80 B9 C3 2C 00 00 01   cmp byte [ecx+0x2CC3],1
0x4993BD  74 16                  je  0x4993D5            ; no order armed: the contextual path
0x4993BF  81 FA 01 02 00 00      cmp edx,0x201
0x4993C5  0F 85 ED 01 00 00      jne 0x4995B8            ; anything but a press: the frame
0x4993CB  8D 4C 24 0C            lea ecx,[esp+0xC]       ; the handler's copy of the record
0x4993CF  51                     push ecx
0x4993D0  E9 DE 01 00 00         jmp 0x4995B3            ; call 0x498F70, the click
```

So **stock places on the press** (`WM_LBUTTONDOWN`) and ignores the moves, the release (`0x202`)
and the double click (`0x203`, which Windows sends in place of the second press): each fails the
`cmp` and goes straight to the frame. `0x4995B3` is `call 0x498F70`, the click's one call site
(the demo recorder redirects it, `tagpu_recorder.c` `CLICK_LEFT_CALL`); `0x4995B8` is `call
0x496790`, the frame, and reads no flag before it; nor does `0x4993CB` before its call. No branch
in `.text` lands on `0x4993BF..0x4993D4` and no absolute reference in the file names an address
there (an instruction-level scan and a literal scan, 2026-10-25).

**Impure's three hooks** (all on the game thread inside this handler, written at attach; the
entry observer and the stub are the router's, `tagpu_route.c`, which hands a mode-14 record to
`tagpu_place.c` and a mode-12 one to the reclaim sweep): a before-observer on the handler's entry
`0x499200` (stolen `A1 E8 1D 51 00 83 EC 18`) whose placement client ends a gesture whenever a
record is handled in a mode other than 14 — in mode 1 the routing
skips the route (`0x4993BD`) and `0x4197D0` is not called, so neither other hook sees a disarm
(the right press `0x499100`, Shift up after a Shift placement `0x4992D8`, a group recall
`0x48D9A0` → `0x495860`, the order check) or the release after it, and re-arming takes a left
click on the build menu, which cannot come while the gesture's own button is held; an after-observer on `0x4197D0`, chained after the resource module's before-observer,
which evaluates the gesture once the engine has written the rectangle, the altitude and bit 6
for the record; and a 12-byte stub over `0x4993BF` (`E9 rel32` and seven `90`, written after all
22 bytes above match) that asks the armed mode's client about each press and release and takes
one of three ways: consumed (to `0x4995B8`), stock's click on this record (to `0x4993CB`), or the two original
instructions. The stub keeps every register (`pushad`/`popad`); only EFLAGS differ on the first
two ways, and neither target reads a flag. What the commit then calls is the order roots of
*Contextual cancellation through the native order roots* (`0x439F80`, `0x43ADC0`), the sound
`0x47F1A0("oktobuild", 0)` and the bit-5 store of `0x498FB4`.

**What the module reads for it.** `0x4C1B80` for Shift, Ctrl and Alt at each evaluation (the ids:
*`KeyboardHotkeySampler 0x4C1B80`*); `0x48CD80` for the ring's unit, asked on the same record that
the handler skipped it for; `0x438760` for the two build actions, with the engine's own names at
`0x502654` and `0x502640`; each selected builder's order chain, bounded and owner-checked; the
site test, `0x47C780` and `0x47D820` per site, with `0x51E684`/`0x51E688` saved and put back
(`0x47C770` is `mov eax,[0x51E688]; ret`, called at `0x40A4D6`, `0x40A4E5` and `0x40A76A`, AI code
that runs from the frame's ticks after the handler [INFERRED: not traced to the tick]). The record
re-read after a press is the mouse object's (*After a press, IdleTick re-reads the move made
before it*); the module recognises it by its six dwords.

**Gaps.** The other five callers of the disarm `0x495860` were not identified, and `0x4956C0`'s
debug keys were not traced (*Alt+Z and Alt+X do nothing in play*). The ranking key of `0x48CD80`'s
hits is not established.

### The viewport rect at game entry — what builds it, and why it will not patch [BINARY-VERIFIED 2026-09-11]

Read for [GUI renderer](../gui-renderer.html) §22 (HUD scale), which tried to make the engine
reserve a bigger HUD and found out it cannot be done this way — see the projection section
below, and §22.5. The disassembly here is unaffected by that; nothing of ours writes this rect
any more. `objdump -d -M intel` of the pristine Steam build, `0x498170..0x498240`.

The rect is eight stores, and the first two are what the other six are built from:

| VA | instruction | field |
|---|---|---|
| `0x4981A7` | `mov ecx,[eax+0x37F1B]` → `0x4981AD` `mov [eax+0x37E1F],ecx` | **screen W**, copied from the Screen Size row's own field |
| `0x4981B8` | `mov edx,[eax+0x37F1F]` → `0x4981BE` `mov [eax+0x37E23],edx` | **screen H**, likewise |
| `0x4981C9` | `c7 80 27 7e 03 00 80 00 00 00` — `mov [eax+0x37E27],0x80` | left = **128**, `imm32` |
| `0x4981D9` | `c7 81 2b 7e 03 00 20 00 00 00` — `mov [ecx+0x37E2B],0x20` | top = **32**, `imm32` |
| `0x4981EE` | `dec edx` → `0x4981EF` `mov [eax+0x37E2F],edx` | right = **W − 1** |
| `0x498200` | `83 e9 21` — `sub ecx,0x21` → `0x498203` store | bottom = **H − 33**, **`imm8`** |
| `0x49820E` / `0x498214` | `mov edx,[eax+0x37E2F]` / `mov esi,[eax+0x37E27]` — R and L **re-read from the fields** just stored | (the re-read that made a render-thread store of L racy until 2026-09-12; `tagpu_vpwide` writes L on the game thread now) |
| `0x49821A` | `sub edx,esi` ; `0x49821C` **`inc edx`** ; `0x49821D` store | viewW = **right − left + 1** |
| `0x498234` | `sub ecx,ebx` ; `0x498236` **`inc ecx`** ; `0x498237` store | viewH = **bottom − top + 1** |

**The `inc` matters.** viewW and viewH are inclusive spans, `R−L+1` and `B−T+1`, not `R−L`:
at 640×480 they are 512 and 416, not 511 and 415, which is what
[resolution](../resolution.html) §3 and `tagpu_vpwide.c`'s `true_rect_of` have always agreed on.
Recorded because the HUD-scale handoff wrote them without the `+1` and a rect one pixel short
in each axis would have been invisible in a screenshot and wrong in every clamp.

**The three immediates do not all patch, and the bottom one is why.** `0x4981C9` and
`0x4981D9` carry their constants as `imm32` and would take any value. `0x498200`'s
`sub ecx,0x21` is a **sign-extended `imm8`**, so the bottom inset caps at 127 — i.e.
`32s + 1 ≤ 127`, `s ≤ 3.94`, against a 4K ceiling of `s = 4.5`. Widening it to
`81 e9 imm32` needs three bytes the next instruction owns. §20 therefore wrote all six fields
from an observer instead — and that turned out to be the wrong thing to want, because moving
`L`/`T` moves only `0x498DA0`'s origin and not the projection (next section). **Nothing writes
this rect today**; the observer is gone. The encoding facts are kept because they are the
answer for anything that does want to move the rect.

**`0x4288D0` — the background-picture loader, and the one call that is a clock.** stdcall,
`ret 0x10`, prologue `83 ec 30 8b 44 24 38` (7 bytes, `sub esp,0x30` + `mov eax,[esp+0x38]`).
**42 call sites** in the image (full `E8` rel32 scan). The one at **`0x49823D`**, returning to
**`0x498242`**, is `0x4288D0("loadgame2bg", 0, 0, 0)` — the first instruction after the last
store of the rect, and **before the loader thread is created at `0x4982CA`**, so LoadMap's
derivations and the SORT allocations see whatever the rect holds by then. An observer on
`0x4288D0` that acts only when its return address is `0x498242` therefore runs exactly once
per game entry, at exactly the point a rewritten rect has to land, without a stub of its own
shape at an arbitrary address. HUD scale used it for one day and no longer needs it; the site
is recorded because "runs once, on the game thread, at game entry, before the loader thread"
is a useful hook to have found and it cost a 42-site call scan to identify.

### The world→screen projection is NOT derived from the viewport rect [MEASURED 2026-09-11]

**The rect's `L`/`T` are the screen→world origin inside `0x498DA0` and nowhere else.** Every
site that projects the other way carries `+0x80` / `+0x20` as **baked immediates**:

```
screenX = worldX − eyeX + 0x80
screenY = worldZ − (altitude >> 1) − eyeY + 0x20
```

— the unit-under-pointer probe, band select, build placement, the feature blits at
`(col+8)*16 / (row+2)*16`, the health bars. So a rect whose `L` is not `0x80` makes the two
directions disagree, and everything that goes out through one and comes back through the other
lands `(L − 0x80, T − 0x20)` away from where it was drawn.

**Measured** by writing `L = 204`, `T = 51` at 1024×768 and reading the engine's own
unit-under-pointer field `main+0x2CBA` at two pointer positions for one `ARMSTUMP` at world
(6180,12140), eye (5796,11731):

| pointer | `+0x2CBA` |
|---|---|
| (512,384) = `world − eye + 0x80` | `0xFFFF0002` — the unit |
| (588,403) = `world − eye + L` | `0xFFFF0000` — nothing |

The engine answers about `0x80`, not about `L`. `tagpu_vpwide.c` has always relied on this
without saying so: it widens `L` for the clamp while holding its own `VP_TRUE_L` at `0x80` and
re-doing `0x498DA0`'s arithmetic with the true origin, which is exactly what keeps a widened
rect from tearing the picture. Anything that wants to move the world's screen origin has to
patch the immediates — every one of them — not the rect.

### Who reads the viewport dimensions `main+0x37E37`/`+0x37E3B` [MEASURED 2026-09-11]

A full-image displacement scan, for §20's question of whether a changed viewport rect can reach
the simulation. **Seventeen sites, twelve functions**, and every one that can be named is
view-side:

| function | what it is |
|---|---|
| `0x41C3C0`, `0x41C4C0`, `0x41C7C0`, `0x41C8E0`, `0x41CA10`, `0x41D0F0`, `0x41D1F0` | the camera / scroll cluster — `SetCamera`, smooth centre-on, the per-frame scroll, the minimap drag |
| `0x483610` | LoadMap |
| `0x483FA0` | the terrain pass |
| `0x496EE0`, `0x497180` | game setup (`0x497180` folds the skirmish settings; it is `0x496EE0`'s only caller) |
| `0x497CE0` | contains the write site itself — `0x498237` is the store, not a read |

And `main+0x1423B`/`+0x1423F`: **thirteen reads, no write** at that displacement, from
`0x4161F0` (which calls the flyby `0x495A30`), the map debug overlay `0x418310`, the minimap box
filler `0x466B70`, `0x47F300` (called from the in-play frame handlers `0x499EB0`, `0x49B720`,
`0x49C740`) and `0x495A30`. `main+0x14243`, `+0x14247`, `+0x1424B`, `+0x1424F` have **no
references at all**, which is worth recording as a negative result:
[resolution](../resolution.html) §3 lists all six as LoadMap's derivations from viewW/H, and this
scan neither confirms the writer nor finds a reader for four of them.

**SETTLED 2026-09-11: they are the view size in 16-px tiles, and this page was wrong.**
[resolution](../resolution.html) §3 is right and the "map dimensions" reading above is not.
HUD scale gave a cleaner experiment than the two-resolutions one this was waiting on: it
changes viewW/viewH **without** changing the screen mode or the map, so the two candidate
sources are separated outright. Same map, same 1920×1080 surface, one lever:

| | viewW `+0x37E37` | viewH `+0x37E3B` | scroll extent `+0x1422B`/`+0x1422F` | `+0x1423B` | `+0x1423F` |
|---|---|---|---|---|---|
| stock HUD | 1792 | 1016 | 10720 × 12672 | **112** | **63** |
| HUD scale Auto (`s` = 2.25) | 1632 | 936 | 10720 × 12672 (unchanged) | **102** | **58** |

`1792 >> 4 = 112`, `1016 >> 4 = 63`, `1632 >> 4 = 102`, `936 >> 4 = 58` — all four exact.
The scroll extent (the map less 32 and 128 px) did not move; these did. The minimap's view-box filler `0x466B70` above is
therefore reading *view tiles*, which is what a view box is drawn from, and the write the scan
could not find is `0x483BD6`/`0x483BE4` inside LoadMap reaching them through `ebp = main+0x141FB`
at `+0x40`/`+0x44` — a base the displacement scan cannot see, which is exactly why it found
thirteen reads and no write.
