# Drawing the frame: blits, rasterisers, shadows and markers

The offscreen and flip, `DrawGameScreen`'s sweep, the unit blit and its shadows, the rasterisers, the order markers.

*Part of the [engine map](../exe-reverse-engineering.md). A function's entry is the section whose heading names it — the [address index](address-index.html) lists them: a new fact about a function extends its entry, in place; a new section is for an area nobody has mapped yet.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### Renderer scene probe [SOURCE + MEASURED 2026-09-28]

The recorder's [direct scene experiment](../tadr-port/demo-recorder-exploration.md#6b-direct-scene-playback-and-portable-unit-poses)
used the existing `tagpu_packet_pub.c:fill_world` ownership boundary, widening only the
unit-piece capture condition to cover every live unit. No new engine byte patch was added.
The packet layout was measured with a 32-bit `offsetof`/`sizeof` executable: header 2312 B,
unit row 100 B (104 B since the unit row gained the owner's logo colour, 2026-09-29; the header stays 2312 B, the colour of the local player taking the minimap group's spare byte), piece row 24 B, wreck row 44 B; tick +16, unit count/offset +1608/+1612,
piece count/offset +1616/+1620, wreck count/offset +1624/+1628, truncation flags +2300.
These are **Impure packet offsets**, not offsets in the retail engine.

`TAGPU_PK_UNIT.o3_key` (+20) is an Object3do identity value used by interpolation;
`TAGPU_PK_PIECE.node` (+20) is a dereferenced model-template pointer. Source reads in
`tagpu_native.c:pose_accum_body`, `tagpu_lerp.c` and the packet header confirm that copying
these values to disk cannot establish portability. The unit Object3do is freed by `0x45AAA0`,
whereas the model template belongs to the level teardown `0x42DB90`; their existing maps and
the reclaim fence provide the lifetime argument, not the older pointer probes inside the
consumer. No new disassembly of either destructor was performed in this experiment.

A temporary fresh-process importer retained no recorded asset addresses. It resolved unit
piece indices against the new level's validated published templates, with matching type/model,
name, piece count and base piece. All 59 template-piece addresses across the four measured
types changed, while the checked unit-formation image matched. The reader retained the new
session's other world/UI layers; it neither restored simulation nor demonstrated full-world
portability. The stopped fresh session's existing main fields were inspected through `tacli`:
`[0x511DE8]+0x38A47` = tick 521 and `+0x38A51` = pause byte 1. The temporary publisher and
consumer probes were removed after the experiment; details and bounds are in the linked note.

The genuine-construction follow-up used the same existing unit-array fields
`main+0x14357/+0x1435B` to bound slot 3 (stride `0x118`) and read its `+0x104` float and
`+0x108` HP word. A normal GUI build order created the ARMSOLAR: at GameTime 4308 it had
fraction 1 / HP 0, at 4429 fraction 0.5911845 / HP 134, and at 4594 fraction 0 / HP 326.
No fraction write was made by the test. The publisher's packet held all five model pieces
and flags 15 at each state (including the depth-plane flag). The scene probe restored the
partial and complete images exactly in the unit crop; see the exploration's construction
subsection for its deliberately limited claim.

### The unit sort's append can run past its buffer — `0x469807..0x469825` in `DrawGameScreen 0x468CF0`

**The buffers.** `DrawGameScreen` holds `edi = main+0x141FB` through its unit binning and both
sweeps. LoadMap sizes everything through `ebp` = the same address, once per map, and is the only
writer of the two dimensions (a displacement scan plus the base-register scan of
[resolution](../resolution.html) §3.2):

| field | what | set at |
|---|---|---|
| `[edi+0x00]` = `main+0x141FB` | SORT_UNIT_LIST: `rows × cap` unit pointers | `MEM_Alloc 0x4D83B0` of `rows·cap·4` bytes (tag `0x508B28`) at `0x483D45`, stored at `0x483D4A` |
| `[edi+0x04]` = `main+0x141FF` | SORT_INDICES: one write cursor per row | `rows·4` bytes (tag `0x508B18`) at `0x483D5C`, stored at `0x483D6F` |
| `[edi+0x08]` = `main+0x14203` | SORT_LINE_COUNT: one `u16` per row | `rows·2` bytes (tag `0x508B08`) at `0x483D72`, stored at `0x483D7A` |
| `[edi+0x50]` = `main+0x1424B` | `cap` = view width in 16-px cells + 12 | `0x483D2A`, `+0xC` at `0x483D30`, stored at `0x483D42` |
| `[edi+0x54]` = `main+0x1424F` | `rows` = view height in 16-px cells + 32 | `0x483D27`, `+0x20` at `0x483D2D`, stored at `0x483D33` |

At 1024×768 that is `cap` 68, `rows` 76 and a 20 672-byte list [MEASURED].

**Every frame.** `0x46971B..0x46973D` points `cursor[r]` at `base + r·cap·4` (`0x46972A`) and zeroes
`count[r]` (`0x469731`) for every row. The binning loop `0x4697CF..0x469840` then walks HotUnits
(`u16` unit indices at `main+0x1435F`, `NumHotUnits` at `main+0x14367`, filled by the cull
`0x48BAE0`) and files each unit under the 16-px row of its feet:

```
4697ed  movsx eax, word [unit+0x74]    ; the feet's world z
4697f2  sub   eax, [main+0x14323]      ; - eyeY; then sar 4, add 0x10
469800  js    0x46982c                 ; row < 0: not filed
469802  cmp   eax, [edi+0x54]
469805  jge   0x46982c                 ; row >= rows: not filed
469807  mov   edx, [edi+0x4]           ; SORT_INDICES
46980a  lea   ecx, [edx+eax*4]         ; &cursor[row]
46980d  mov   edx, [edi+0x8]           ; SORT_LINE_COUNT
469810  inc   word [edx+eax*2]         ; count[row]++
469814  lea   eax, [edx+eax*2]
469817  mov   eax, [ecx]
469819  test  eax, eax
46981b  je    0x469826                 ; a NULL cursor skips the store
46981d  mov   [eax], ebp               ; *cursor[row] = unit
46981f  mov   eax, [ecx]
469821  add   eax, 4
469824  mov   [ecx], eax               ; cursor[row] += 4
469826  mov   esi, ds:0x511de8         ; the join
```

**Nothing compares `count[row]` with anything**, and the two readers do not either: site A's loop
(`0x4699A8`, ground units, the swept rows) and site B's (`0x469B54`, everything else, all rows)
each start at `base + row·cap·4` and walk `count[row]` slots (§"The sweep order inside
`DrawGameScreen`", below). So:

- **A row holding more than `cap` units runs on into the next row's slots.** That is stock
  behaviour and it is harmless: every slot a row counts was written this frame, by that row or by
  the one it ran into. When the next row has units of its own, the two rows write the same slots
  and one unit is drawn twice and another not at all. Beside an empty row, the full row is drawn
  whole. MEASURED without the patch at 1×: the fixture's line at eye (1616, 1206) puts 116 hot
  units in row 40 against a `cap` of 68, and the engine draws the whole line.
- **A row near the end runs past the end of SORT_UNIT_LIST.** The write lands in whatever the
  allocator put next, and what it writes is unit pointers. The last 16 rows are the margin below
  the viewport (`rows` = 16 above + the view + 16 below). At stock 1× a unit is filed there only
  when its feet are below the view and its box still reaches into it. Past the end of the list
  therefore takes more than `cap` such units in one of the last rows, or a run-on long enough to
  reach them. [INFERRED from the cull and the row formula; not reproduced at stock 1×.]
- **Our build reaches it with an ordinary crowd.** At zoom < 1 `vpwide` widens the rect the cull
  tests (gpu-status §2.3b), but `cap` and `rows` stay the 1× values LoadMap computed. The rows
  below the 1× view then hold every unit the wider view shows. MEASURED without the patch, the
  eye held at (1552, 650) so that the line sits in row 75 (the last) and the scripted zoom at 0.5:
  150 hot units are filed in row 75 against a `cap` of 68. That writes **82 pointers, 328 bytes,
  past the end of the list on every frame**, over the next heap block's header (the dump at the
  end address reads unit pointers where it read `5B 00 00 00 26 00 75 00 …` before). Nothing
  faulted: the simulation kept ticking and the level exited cleanly to the main menu. It is a
  silent corruption of memory the engine owns elsewhere.

**The patch bounds the append by the allocation, not by the row.** A `jmp` at `0x469807` to a stub
of ours, the rest of the 31-byte block NOPped up to the join:

```
mov   edx,[edi+0x54]      ; rows
sub   edx,eax             ; rows - row, at least 1 (0x469805)
imul  edx,[edi+0x50]      ; the slots from this row's start to the end of the list
mov   ecx,[edi+0x8]
lea   ecx,[ecx+eax*2]     ; &count[row]
movzx esi,word [ecx]
cmp   esi,edx
jge   join                ; the next slot is past the end: not filed
inc   word [ecx]
mov   edx,[edi+0x4]
mov   ecx,[edx+eax*4]     ; the row's cursor
jecxz join                ; stock's NULL-cursor skip
mov   [ecx],ebp
add   dword [edx+eax*4],4
join: jmp 0x469826
```

- **The invariant:** `count[row] ≤ (rows − row)·cap` after every append. The cursor and the count
  start together each frame and move together only here, so the slot written is
  `base + (row·cap + count)·4 < base + rows·cap·4`. Every slot a reader walks is inside the list
  and was written this frame.
- **What it rests on:** `[edi+0x50]`/`[edi+0x54]` are the values the allocation was made with.
  LoadMap is their only writer and nothing of ours writes them. `eax`, `ecx`, `edx` and `esi` are
  dead at `0x469826`, which reloads `esi`; `0x46982C` reloads `eax` and `ecx`, and `edx` is written
  at `0x4697D1` before its next read. No branch from outside the block lands in
  `0x469808..0x469825`, and stock's own `0x46981B → 0x469826` is the only branch to the join [a
  rel8/rel32 scan of `.text`].
- **The count cannot wrap.** It is a `u16`, and the cull `0x48BAE0` files each slot of the unit
  array (stride `0x118`, walked from `main+0x14357` to `main+0x1435B`, one `u16` index each at
  `0x48BC6A`) at most once per call. A frame's appends to any row are therefore at most the unit
  slots, `10 ×` the per-player cap `+ 1`, far below 65 536.
- **What it changes:** nothing for a unit whose slot is inside the list, the run-on included. A
  unit whose slot would be past the end is not drawn by the engine's sweep that frame.
- **Why not the row:** a bound of `count[row] < cap` was built first and measured. It erased
  **7 066 px** of the fixture's line at 1×: the run-on into an empty neighbour that stock draws
  correctly.
- **Measured with the patch:** at the same held eye and zoom 0.5, row 75 holds 68
  (`(76 − 75)·68`), none of the 102 dwords after the list's end is a unit pointer, and the heap
  header there is intact. At 1×, against a build without the patch: the fixture is 0 px apart in
  the engine's own frame (the golden-source A/B of `ta-drive`'s measuring reference, three shots
  each), and the fixture and `selbox-facings` are both 0 of 3 145 728 apart in the world unit pass
  at `ss=2`.
- **The simulation:** the list is read only by `DrawGameScreen`'s two draw loops, so the
  simulation reads nothing different. The stock overwrite is the only part of this that could ever
  reach simulation state.

A raised unit cap (the TADR port's later landings) puts more units on screen and makes the stock
overwrite easier to reach. The patch does not depend on the cap.

### The composite scratch frame and the rasterisers' span tables

`[DISASSEMBLED + MEASURED 2026-09-24]` The frame, its five writers and their callers are in *The
raised effect pools* (below, "The composite scratch frame"): one frame a level at
`*(main+0x1437B)+0x10`, laid out by `0x4B8E00` as a `0x18`-byte header and two planes of `A`
pixels, `A = [+0x14] − [+0x10]`. Two ceilings bound what may be written there, and stock checks
neither.

- **Area.** A writer sets a header and writes that many pixels into both planes: the build-state
  copy `w·h`, the frame copy its source's `w·h`, the shadow build `w·h` and then its encode, which
  `0x4B9E60` writes into the depth plane at up to `2h(w+1)` bytes (two a row and two a pixel from
  `0x4BA000`), and the 2× bakes `4·w·h`. A sixth, `0x459170`, is a twin of the frame copy (the
  source's header and `w·h` of both planes into `[this+0x10]`) with no caller and no pointer to it
  anywhere in the image: it never runs, and nothing patches it.
- **Rows.** Polygons are then drawn into the frame, and every rasteriser that draws them
  keeps its spans on its own stack, one `0x28`-byte entry a row, appended from the polygon's first
  row. Each takes its destination as the first argument and clips rows only to `[0, h − 1)`
  (`0x4C8CB6..0x4C8CE1` in `0x4C8BB0`: `[frame+2] − 1`), so a destination `h` rows tall writes up to
  `h − 1` entries.

| rasteriser | stack frame | table | entries | reached from |
|---|---|---|---|---|
| `0x4C8BB0`, the textured quad | `0x7D60` | `[esp+0x70]` | 800 | `0x45A39C` in the bake `0x459C70`, its one caller |
| `0x4C8760` | `0x7D58` | `[esp+0x68]` | 800 | `0x459B96` in the bake `0x459830`, on its 1× path too — which draws into the build-state copy's frame (below) |
| `0x4C1000` | `0x14028` | `[esp+0x38]` | 2048 | `0x459BB1` in the bake `0x459830`; `0x45A750` in the shadow's `0x45A610` |
| `0x4C0C70` | `0x1402C` | `[esp+0x3C]` | 2048 | `0x45A3BA` in the bake `0x459C70` |
| `0x4C0820` | `0x14024` | `[esp+0x34]` | 2048 | `0x459128` in `0x458FA0`, from `0x458DD0`: the build-state copy's last call (`0x458D0E`) and the cargo loop's (`0x459686`) |

Each table holds exactly its entries — 800 for the first two, 2048 for the other three: the frame
is `_chkstk`-probed (`0x4E4B20`) and four registers pushed, so for `0x4C8760` the table ends at
`0x58 + 800·0x28 = 0x7D58`, the frame's size — and an `h`-row destination writes at most `h − 1`
(each side covers `[max(y0,0), min(y1,h−1))`, `0x4C899B..0x4C89E1`). So a destination may be 800 rows
tall for the first two and 2048 for the other three, one row inside the table. Past that the entries
run upward over the return address and then the arguments.
MEASURED: a solar scaled ×11, whose 2× bake grew the frame to 1548 rows, faulted at `0x4C8035`
reading `[NULL+0x10]` for its polygon's texture, `0x4C8BB0`'s argument at `[esp+0x7D78]`, which its
own table had overwritten. Stock reaches it too: area bounds a 2× frame, not its height, so a narrow
structure whose own frame is more than 400 rows tall gets there at 600² as well.

The unit draw also hands the frame to the bake as its own source: `0x459641` calls
`0x459830(ctx+0x10, obj, colour, 0)` — for every unit without the structure bit, and for a
structure whose `+0x104` is `0.0` (`0x459610..0x45962D`) — and the mode argument 0 sends the bake
down its 1× path before anything is sized (`0x459875`). The 1× path rasterises into its source by
the source's own header (`0x459B8E..0x459B96`: `0x4C8760`'s destination is the first argument), so
the header the build-state copy set is drawn under by `0x4C8760` as well as by `0x4C0820`, and it is
held to 800 rows.

**The fix** (`fix_composite_scratch`, always on) runs a check before each writer forms its size,
through a stub that saves every register around a C call (`pushad`, the check, `popad`). The
merge's stub is a `jmp` to `scratch_merge`, a `__stdcall` that keeps `ebx`, `esi`, `edi` and `ebp`
and clobbers `eax`, `ecx` and `edx` as `0x4B90A0` does; nothing from `0x4596DD` on reads those
three before writing them.

| site | writer | stolen bytes | what the check computes | refused |
|---|---|---|---|---|
| `0x458B87` | the build-state copy | `8B 4B 10 F7 D8` | `esi × edx`, the header about to be written; rows `edx` against 800 | sets a flag and leaves through the epilogue `0x458D13` |
| the call at `0x459608` | its one caller | `E8` rel32 | — | a wrapper clears the flag, makes the call, and on the flag drops the return and both arguments and jumps to `0x4597D8`, the unit draw's own "no frame" exit (`0x459212`): the unit is not drawn that frame |
| `0x45A470` | the frame copy, at its entry | `8B 44 24 04 53` | the source's `w·h` | the frame becomes 1 × 1 holding the source's key, with its hotspot and key, and it returns the frame as stock does (`mov eax,[ecx+0x10]; ret 4`) |
| `0x45A7B9` | the shadow build | `8B 4D 10 66 8B 54 24 10` | `2h(w+1)` from the bounds `0x45A510` left at `[esp+0x10]` and `[esp+0x20]`; rows `h` against 2048 | the frame becomes 1 × 1 holding its own key, hotspot 0, and it resumes at the encode `0x45A853`: the cached shadow is one transparent pixel |
| `0x459875`, `0x459CB5` | the 2× bakes, on the 2× path only | `85 DB` / `85 C0`, then `0F 84` rel32 | `4·w·h` from the source at `[esp+0x5F18]` / `[esp+0x159E8]`; rows `2h` against 800; `w` and `h` below `0x8000`, since the doubled header is a `u16` | the 1× path, `0x459913` / `0x459D57`, which draws into the source by its own header |
| the call at `0x4596D8` | the cargo merge `0x4B90A0`, its one caller | `E8` rel32 | the rectangle it paints: `x0 = dst.hotx − src.hotx + sx`, `y0` likewise; it runs when `x0 + src.w ≤ dst.w`, `y0 + src.h ≤ dst.h` and `dst.w·dst.h ≤ A` | the cargo is left out of this frame's composite (`scratch_merge` returns as `ret 0x14` does) |

A check that does not fit grows the frame: a block of `0x18 + 2·px` bytes, `px` the need rounded up
to 256 Ki pixels and at most 2048 × 2048 (4 194 304), with the header and both planes copied and the
planes laid out as `0x4B8E00` lays them. `ctx+0x10` is repointed, and the old block freed through
`0x4D85A0`, the free the teardown `0x4581C0` uses. The grow is refused, and the writer takes its
fallback, when the need is past 2048 × 2048, when the rows are past the table, when the frame is the
writer's own source, when the allocator has no block, and under `tagpu_scratch.nogrow`. A grown frame
lasts the level.

**The cargo merge.** `0x4B90A0(src, dst, sx, sy, dbias)` paints a carried unit's own frame into the
scratch at `(x0, y0)` by the scratch's header width and refuses only a negative origin
(`0x4B90CF..0x4B90DD`); nothing clips its right or bottom edge. In stock it fits, because the
build-state copy's box is the union of the carrier's and every cargo's boxes, taken with the same
projection and the same ±2 margins (`0x4581E0`, `0x458310`). Between the two, though, the cargo's own
bake runs (`0x459670`, `0x4586A0(cargo, 1, −1)`), and its 2× path would leave the scratch's header at
the cargo's doubled box. The attach guard should make that unreachable — a structure is never
attached (`0x48ABC7..0x48AC1D`) — and the bound does not depend on it: `scratch_merge` checks the
rectangle against the header it is about to be painted by.

**The allocator path.** The engine's `malloc` `0x4D83B0(name, size)` → `0x4D83C0(size)` enters the
allocator's critical section (`0x4DA780` returns it, `0x528A28`), takes a debug heap when the option
`0x4D80D0` answers so (`0x4DACF0`, freed through `0x4DB7D0`), else the CRT's `malloc` `0x4E8890`,
counts the block (`0x4DA7D0(size)`), and applies a fill when the option `0x4D8200` is set
(`0x4D82C0`). A NULL calls the handler at `[0x5289BC]` in a loop, and the installed handler
`0x49E700` ends the process. `scratch_alloc` runs the success path by hand, inside the same critical
section: no block under the debug heap, else `0x4E8890` and, on success, `0x4DA7D0`.
`0x4D85A0(ptr)` → `0x4D85B0` frees such a block through `0x4D8360`, `0x4DA840` and `0x4E8820`. The
fill is not applied: every plane is written before it is read.

**The invariant.** Every header a writer sets has `w·h ≤ A` (`2h(w+1) ≤ A` for the shadow) and no
more rows than the smallest table under it, and the merge paints only inside the header's box. It rests on a bound: `A` comes from the frame's own two
pointers, and the frame has exactly two producers, `0x4B8E00` and the grow, both laying it out this
way. The layout test (colour at `base + 0x18`, depth above it) is a filter on those values; a frame
failing it is left to stock and logged once. On an ordering: each writer reads `ctx+0x10` only after
its check, its callers re-read it after every call (`0x459342`, `0x4594E3`, `0x4595B8`, `0x4595D9`,
`0x459639`, `0x4596BA`), and no function a writer calls reaches another writer: `0x458310` is a leaf
(`0x458310..0x458426`, `ret 0x20`, no call), and neither `0x458DD0`, `0x4B8A80`, `0x4B7F90`, `0x45A510`,
`0x45A610`, `0x4B96A0`, `0x4B9D70`, `0x4B9E60`, `0x437B50` nor the bakes' callees reach one. (The call to `0x459200`
at `0x458468` belongs to `0x458430`, a function with no caller, jump or pointer to it, which a
nearest-preceding-target walk folds into `0x458310`.)
A frame is never regrown under a writer reading from it. And on a lifetime: the old block is freed
only once `ctx+0x10` holds the new one, and nothing else holds it (the render thread never reads the
frame; our tracer logs only the pointer's value). Every fallback's 1 × 1 fits: `A` is 360 000 or more
from `0x4B8E00` and at least 64 from a grow. Identical to stock for every unit whose box fits.

**Levers**, read once at attach. `tagpu_scratch.stress` treats every frame as too small, so every
writer call regrows it to exactly `max(need, 64)` pixels and frees the old one, after setting the old
block's two plane pointers to the middle of a 16 MB region the lever reserves with no access at
attach: a freed block the heap keeps mapped would hand a stale reader its old bytes rather than
fault, and with the largest plane offset (8 MB) on either side the region makes one fault at any
offset, forwards or backwards. It is reserved rather
than taken from above 2 GB because that span is no one's only while the exe is not
large-address-aware: retail's is not (PE characteristics `0x10B`), TA:ESC's is (`0x12B`), and there
the heap is up there (below). With no region the lever is refused and logged. A reader that kept a
plane pointer of its own is not caught. `tagpu_scratch.nogrow` refuses every grow; both together send every writer to its fallback.
Each grow and refusal is logged (`enginefix: composite scratch grown|refused (<reason>) for <writer>:
N px in R rows asked, A held, P now (n so far)`, and `composite scratch merge refused: a WxH cargo at
(x,y) is past the WxH frame (A held)`), each kind's first 16, then every 1024th.

**Measured**, GDI lane, Two Continents, 1024×768, the nanoframe ladder plus oversized units made
locally from stock models:

- **Below the cap nothing changes.** The ladder on its own, both builds paused and the tick written
  to 500 in each (the build-state pulse is a function of the tick): the world is pixel-identical,
  and the 4845 pixels that differ are the chat line's randomly chosen defeat message.
- **Past it.** Flat giants 1450 to 6525 pixels wide and under 800 rows tall: the build before the fix
  faults at `0x459EAA` reading `[NULL+0x241]` through a unit record the overrun had rewritten. With
  the fix they draw: the 2× bake grew the frame to 2 097 152 pixels for 708 rows, the shadow to
  3 407 872, the build-state copy of two giant nanoframes to 2 883 584 and then 4 194 304. Refused
  over the cap were a 5 522 140-pixel shadow and a 4.37-million-pixel nanoframe, and over the rows
  2× bakes of 1268 and 1332.
- `tagpu_scratch.nogrow`, and `stress` with `nogrow`: every writer took its fallback, no fault.
  `stress` alone, with the freed frames' planes at `0x80000000`: 40 s on the giant nanoframes, 53 248
  regrows by the build-state copy; 35 s of an ARMATLAS carrying a Peewee, 251 904 by the build-state
  copy and 137 216 by the frame copy; no fault. With the planes NULLed instead, 40 s of a lab building
  Kbots on its pad beside the transport: 364 544 and 236 544, no fault and no merge refused. On the ladder with a ×6 solar, before the poisoning, 182 000 in 12 s.
- **The merge.** The same lab and transport on this build: no merge refused. The check's pass is a
  tail call into `0x4B90A0` with the same five arguments, so every merge ran as stock. A local build
  whose check refuses every merge: the carried Peewee and the pad's nanoframe are left out of their
  carriers' composites (the log names them, `49×49 at (27,28)` in a `104×109` frame), no fault.
- The 2× path of `0x459830` is taken from `0x4586A0` only when the shading bit (`main+0x37F06` bit 5)
  is clear — `0x45874A` picks which bake that function calls — and the settings store never leaves
  it clear. A local build that clears it measured the grow for 708 rows and the refusal for 1268
  there.
- The Vulkan lane runs the same writers: the same grows and refusals, no fault.

**What it does not cover.** A unit's own frame (`Object3do+0x10`), which the 1× bakes draw into
through the same rasterisers, is sized by the model alone (`0x4586A0` → `0x437BE0` / `0x437B50`), so
a model more than 800 rows tall overflows `0x4C8760`'s or `0x4C8BB0`'s table in stock and with the fix
alike. MEASURED: a solar scaled ×22, a 1544-row frame, faults at `0x4C8035` with the fix (its 2× path
refused) and at `0x459D1D` without it. No stock unit is near: the tallest 1× frame is 239 rows
(`cordev1`, [limits-evidence §10](../tadr-port/limits-evidence.md#10-composite-buffer-6002-12802-0x458195)).
Drawing such a frame in bands of at most 800 rows at those call sites would close it.

### The unit shade: the lit rasteriser `0x459C70` shades structures only [DISASSEMBLED, MEASURED 2026-09-29]

**Who gets it.** The sprite builder chooses the rasteriser at `0x45873C..0x45874A`: `unit+0x110 &
0x20000000` (the structure bit) **and** `main+0x37F06` bit 5 (the
registry's `Shading`) pick `0x459C70`, everything else the plain `0x459830`. `0x459C70` has one
caller, `0x458765` (a scan of `.text` for every `call`/`jmp` to it, and no raw pointer to it
anywhere in the image). Its lit callees — the textured quad `0x4C8BB0` (called at `0x45A39C`), the
colour n-gon `0x4C0C70` (`0x45A3BA`) and the scanline core `0x4C8020` (`0x4C9095`) — have no other
callers either. **MEASURED** with the GDI backend (`tagpu_gdi.on`), Shading on against off, paused,
the same scene: 0 changed pixels on a peewee, thud, flash, stump, raider, hawk and a commander, and
1 692 / 4 706 / 4 192 / 3 642 / 308 on a solar, a kbot lab, a vehicle plant, an air plant and a
laser tower. **A mobile unit is never shaded.** `0x459C70` also tests `main+0x37F06` bit 1 at
`0x459C82..0x459C8E`: set, it takes the doubled-coordinate scratch path at `0x459C94..0x459D55` (the
2× supersample bake, [build state](../build-state.html) §2 and §4), clear it goes on at `0x459D57`. That
gate is independent of the Shading bit and of the intensity below.

**The arithmetic** (`0x459F79..0x45A2F3`). Per piece, over its own vertex list:

1. Each face's normal comes from its **first three** vertex indices only (`0x459F7F..0x45A02B`); a
   face that repeats an index gets `(0,1,0)` (`0x45A0FD`, `0x45A101`). The edge helper `0x4B6F00`
   returns `arg2 − arg1`, `0x4B6F70` is the cross product and `0x4B6FF0` normalises with no test for
   a zero length (a NaN then reaches every vertex the face names).
2. Each face's normal is added to **every vertex the face names**, with a count per vertex
   (`0x45A177..0x45A1D7`), skipping face 0 when the piece has a selection primitive (`node+0xC !=
   −1`, `0x45A136..0x45A150`), and each vertex's sum is divided by its count (`0x45A1FD..0x45A22A`). It is
   **not renormalised**: a vertex between differing faces has a normal shorter than 1.
3. `intensity = ftol(dot(N, L) · 5.0) & 0x1F` when the piece's shade flag (`piece+0x28` bit 2, COB
   `shade`/`dont-shade`) is set, else `0xF` (`0x45A2A5..0x45A2F3`). `L = (−0.8, 1.0, 0.25)` at
   `0x5065F8` (**not normalised**), the scale is the float 5.0 at `0x4FD4CC`. `ftol` `0x4E43A0` sets
   the FPU control word's rounding to truncate (`or ah, 0xC`, `fistp`), so it truncates toward zero.
   **The mask wraps**: −1 is row 31, −5 row 27, and any product in (−1, 1) is row 0, which is black
   in the stock `PALETTE.SHD` (multiplier 0.000).
4. The rasteriser interpolates the vertex intensities across each span in 16.16 and reads
   `shadeTable[(i >> 16)·256 + texel]` per pixel (`0x4C8175..0x4C81C6`, the table at
   `*(prog+0xC4)`). It is Gouraud, and a face between vertices at rows 31 and 3 sweeps through all
   of them.
5. **A quad's rows are spread along its edges, not across a plane** [DISASSEMBLED 2026-09-29]. The
   textured quad `0x4C8BB0` walks the two edge chains from the top vertex (the table in the next
   section) and at every scanline writes five interpolants, each `(v1 − v0) << 16 / dy` per edge, into
   a `0x28`-byte row entry (five left/right pairs): `x`, the texel `u` and `v` (from the caller's
   coordinate pairs), and the vertex record's `+0x08` and `+0x0C` (`0x4C8D5E..0x4C8E93`,
   `0x4C8F16..0x4C9048`). The scanline core
   `0x4C8020` steps them across the row, and reads row `+0x20/+0x24` — the vertex record's `+0x0C`,
   the intensity — as the shade row (`0x4C812B`, `0x4C81A7`); row `+0x18/+0x1C`, from the record's
   `+0x08`, is compared with the fetched texel at `0x4C8187` and not traced further. So a quad whose
   corners hold rows 0, 30, 0, 1 blends all four corners, where two triangles each take a plane
   through three: split on the wrong diagonal, one of them is rows 0, 0, 1 and draws black.
   A face the colour n-gon `0x4C0C70` draws (an untextured face) is not read here.

**The light is fixed.** `0x4597F0(x, y, z)` stores three ints × 0.01 at `0x5065F8`; its only caller,
`0x4166E9`, is inside the chat command `Light` (`0x4166C0`, `0x500FFC`), which reads the three from
what the player types. Nothing reads a map for it.

**The winding's side was fitted, not decoded.** The cross helpers' by-value struct arguments were
not followed through to the sign. Against the GDI frames the engine's normal is
`−(V1 − V0) × (V2 − V0)` of the piece's rest vertices: with the other sign flat top faces come out
black where the engine draws them bright (a vehicle plant's wings, 55 against 36 mean luminance
error), with this one four structures drop to 17–23 (from 36–56). The wrap then reproduces the
solar's black roof panel, which the engine draws with Shading on and not off.

### The rasterisers paint only faces that run clockwise on screen [DISASSEMBLED 2026-09-26]

There is no back-face test anywhere in the unit draw: no normal, no cross product, no flag. The
cull falls out of how every polygon rasteriser the bakes use builds its span table. Each finds
the first vertex at the least `y` (top) and the first at the greatest (bottom), walks the chain
from top to bottom **stepping −1** (wrapping to `n − 1`) into each row's `+0x00`, walks it again
**stepping +1** (wrapping to 0) into `+0x04`, and then fills a row only when `+0x04 − +0x00 > 0`.
On a face whose vertices, in index order, run clockwise on the engine's y-down screen, the +1
chain is the right one and every row fills; wound the other way, the chains swap sides and every
row is empty. A face seen from behind therefore paints nothing, and the same face seen from the
front paints normally.

| rasteriser | used by | −1 chain → `+0x00` | +1 chain → `+0x04` | the fill test |
|---|---|---|---|---|
| `0x4C8760`, the textured quad (`n = 4`, `and 3` wrap) | the unit bake `0x459830` (`0x459B96`) | `0x4C88A0` | `0x4C8A18` | `0x4C8B6F..0x4C8B78` → span `0x4C7A20` |
| `0x4C1000`, the flat n-gon | `0x459830` (`0x459BB1`); the shadow `0x45A610` (`0x45A750`) | `0x4C1110` | `0x4C1206` | `0x4C12E2..0x4C12EB` → span `0x4C06E0` |
| `0x4C8BB0`, the textured quad | the structure/nanoframe bake `0x459C70` (`0x45A39C`) | `0x4C8CF7` | `0x4C8EC8` | `0x4C9078..0x4C9081` → span `0x4C8020` |
| `0x4C0C70`, the Gouraud n-gon | `0x459C70` (`0x45A3BA`) | `0x4C0D80` | `0x4C0EAA` | `0x4C0FBC..0x4C0FC5` |
| `0x4C0820`, the wire's edge walk | the nanoframe wire `0x458FA0` | `0x4C08BB` | `0x4C0988` | `0x4C0A53..0x4C0A5C`, and again in the row writer `0x4C0A90` (`0x4C0A9E..0x4C0AAC`) |

So the build-state wire outlines only the faces that face the viewer, too: "every face" in
[build-state](../build-state.html) §`0x458FA0` is every face the walk visits, and the rasteriser then
drops the back ones.

**The unit bake's face walk, `0x459A9F..0x459BCB`** (`0x459830`, per visible piece; the node is
`[prim+0x00]`, loaded at `0x45998B`):

- From face 1 when `node+0x0C` is not −1, else from face 0 (`0x459AA2..0x459AB4`) — the rule
  `0x45A610`, `0x46BAE0` and `0x458FA0` share. Which face the loader leaves at index 0 was not
  read. In the files, 538 of the 3 014 pieces of the 608 stock models carry a selection
  primitive and in 302 of them it is not primitive 0; `tagpu_posebake.c`'s body and wire ranges
  start at face 0 and only its slant range skips it, so if the loader keeps the file's order, the
  engine skips a real face on those pieces and we draw it. [OPEN — a live read of a node's faces
  settles it.]
- Each face's projected vertices are copied **in index order** (`0x459AD5..0x459B02`, 12 bytes a
  vertex from the piece's projected array at `[esp+0x154]`), so the winding the rasteriser sees is
  the file's.
- Flag bit 0 (`face+0x1C`) → `0x4C1000(frame, verts, face+0x04 count, face+0x00 colour)`; else a
  face of other than 4 vertices is skipped (`0x459B1A`); else `0x4C8760(frame, texture, verts, 0)`
  with the texture the flags name (`0x459B0F..0x459B86`, DISASSEMBLED): **flag bit 1 clear** — the
  `GAFFrame` at `face+0x10`. **Bit 1 set and bit 2 set** — the team-coloured face:
  `0x4B7F30(face+0x18, PlayerInfo[id]+0x96)`, the sequence's frame for the id's **logo colour**,
  with `PlayerInfo[id]` = `*(main + id*0x14B + 0x1B8A)` (`0x459B34..0x459B58`; `id` is a stack
  argument of this routine, `[esp+0x5F20]` — [INFERRED] the unit's owner byte `unit+0xFF`). The
  same rule is coded a second time at `0x458603..0x45862E` (the PlayerInfo load is `0x45861C`),
  where `id` is masked to a byte first (`and eax,0xFF` at `0x458603`) and the bit-1-set,
  bit-2-clear case tests a stack argument, `[esp+0x3F84]`, in place of `ebx`. **Bit 1
  set, bit 2 clear** — `0x4B7F30(face+0x18, 0)` when `ebx` is non-zero (`ebx` = the stack argument `[esp+0x5F24]`, loaded at `0x459B08`; its meaning is not traced), else
  `0x4B7EE0(&face+0x10)`, the animation state's current frame. The colour is the **player's chosen
  colour, not the slot**: a skirmish with slot 0 on colour 4 and slot 1 on colour 0 draws the
  slot-1 badge in colour 0 [MEASURED 2026-09-29, retail, players' logo bytes 4/0/2/7, engine
  frame vs ours]. A logo at or past the sequence's count makes `0x4B7F30` return 0, and
  `0x4C8760` paints nothing for a NULL frame. In TA Zero's `ArmT1Solar` the team faces carry
  flags `6`, `face+0x10 = 0x000A0000`, `face+0x14 = 1`, `face+0x18` a 10-frame sequence [MEASURED
  2026-09-29].

**What modellers did with it.** Two faces over the same vertices, wound apart, make a panel with a
different skin on each side, and only the side facing the camera is ever painted. Of the 608 stock
models, 255 carry such a pair and 209 give the two sides a different texture or colour (1 021 such
pairs; a scan of every `.3do` for primitives in one piece over the same vertex set with opposite
Newell normals). ARMCV's `door1` and `door2` are one each: `noise3a`, grey plating, facing up and
out, and `bluenoise1`, dark machinery with lamps, facing down and in — the inside of the hatch. A renderer that draws both faces of a pair gets whichever side wins
the depth tie (`gpu-status.md` §2.98).

### How `0x4C8760` lays a texture on a quad [DISASSEMBLED 2026-09-26]

`0x4C8760(frame, texture, verts, uvs)`, `ret 0x10`, called by the unit bake at `0x459B96` with
`uvs = 0`.

- **The corners.** With `uvs = 0` (`0x4C879D`) the function builds its own four on the stack
  (`0x4C87A6..0x4C87DA`): `(0,0)`, `(w−1, 0)`, `(w−1, h−1)`, `(0, h−1)`, where `w` and `h` are
  the texture's `u16` at `+0x00` and `+0x02`. So a face's vertex 0 takes the frame's top-left
  texel and the corners follow the face's index order. Our bake puts them on the frame's edges,
  `0` and `w`, rather than on the last texel's.
- **The chains.** Each edge chain carries x (16.16, `+0xFFFF` added, so the ceiling), u, v and the
  vertex's third coordinate, each in 16.16, into a 0x28-byte row entry: `+0x00` xl, `+0x04` xr,
  `+0x08`/`+0x0C` u and v from the −1 chain, `+0x10`/`+0x14` from the +1 chain, and `+0x18`/`+0x1C`
  the third coordinate at the two ends (`0x4C89AF..0x4C89C4`, `0x4C8B18..0x4C8B2E`).
- **The row.** The span `0x4C7A20` divides each quantity's difference by `xr − xl`
  (`0x4C7A48..0x4C7A7C`) and steps it linearly along the row. A row starting left of the frame is
  clipped at 0, with u, v and the third coordinate advanced by as many steps
  (`0x4C7A80..0x4C7AB9`), and xr is clamped to the frame's last column, its `u16 +0x00` less one
  (`0x4C7ABC..0x4C7ACD`).

So the texture is linear along each edge and along each screen row. That is the quad's bilinear
map in two cases: when two opposite sides lie along rows, and for a parallelogram whichever way
it sits (its bilinear map is affine, and linear steps reproduce an affine map exactly). For any
other quad the layout depends on how it sits on screen and moves as the unit turns. The Vulkan unit pass maps a convex textured quad by its bilinear map in the
model's own frame instead, not by this per-row rule (`gpu-status.md` §2.99).

### A range circle of radius 1 — `0x438EDE` [DISASSEMBLED 2026-09-25]

**The defect.** `DrawRangeCircle 0x438EA0` (19 callers, `0x439106` … `0x439943`) divides `0x10000`
by its segment count N at `0x438EEE`. Its only guard, `jl 0x43904D` at `0x438EDE`, catches a
negative N, and radius 1 gives N = 0 (see the function's own entry below). TADR's `jle` sends
N = 0 to `0x43904D`, which draws the circle's label at the screen's corner (0, 4).

**The fix.** A local site: the six-byte `jl` becomes a `jmp` to `jl 0x43904D; je 0x43908F;
jmp 0x438EE4`, on the flags of `0x438ED8`'s compare, which the `mov` at `0x438EDA` leaves alone.
- N = 0 goes to `0x43908F`, the epilogue that radius 0 takes, with the same stack: no circle and
  no label.
- A negative N keeps stock's path.

No branch or absolute literal points into any of the three local sites' replaced bytes. It is
not run: the `ShowRanges` cheat could not be typed under injected input, so it rests on the
disassembly.

## The blend LUT and the marker composites — mapped by us

[MEASURED 2026-09-03, this project — disassembly of the pristine Steam build plus live reads.
Established while fixing the waypoint star, which rendered teal because the engine's alpha
composite was blending it against our fill key (`ui-markers.md` §6). Every VA below was read
off `objdump` in this session unless a row says otherwise.]

**The graphics globals block.** `0x4B6220` is the whole accessor: `mov eax,ds:0x51FBD0; ret`.
(`0x4B6230` starts with the same load and is **not** a second accessor — it does
`or byte [eax+0xF1],0x8` and continues into a longer routine gated on `[eax+0xF0]` bit 1. A
mutator; do not call it for the pointer.) `0x51FBD0` sits past `.data`'s raw end in the BSS
region, mapped at run time — the same region as `TA_MAINPP 0x511DE8`. Live read: `*0x51FBD0`
= `0x0051F320`, so the block itself is static and only its contents move.

| Field | What it is |
| --- | --- |
| **`[globals+0xC0]`** | **Pointer to the 64 KB blend LUT.** Not a borrowable pointer — the block is *owned* (allocated, freed and refilled; see below). Live read: `0x02C91D68`, i.e. heap. |
| `[globals+0xF0]` bit 5 | Gates every LUT user: `test cl,0x20` at `0x4B8519`, and `shr cl,5; test cl,1` at `0x4BAADB` / `0x4BA765`. Name *[INFERRED]* — "translucency available". |

**The LUT's shape.** `out = tab[(src << 8) | dst]`, so it is 256 rows of 256, indexed
source-major. The inner loop is `0x4CBF99..0x4CBFAC`:

```
edx = [ebp+0x1C]        ; the table, arg 6
al  = [esi]             ; source texel
cmp al,[ebp+0x18]       ; the sprite's transparent index
je  skip                ;   -> leave the destination alone
ebx = eax; shl ebx,8    ; src << 8
al  = [edi]             ; DESTINATION texel  <-- this is what made the star teal
add ebx,edx
al  = [eax+ebx]         ; tab[(src<<8) + dst]
[edi] = al
```

Only **three call sites read the LUT** at all: `0x4CBF2C` from `0x4B8499` and `0x4B8682`, and
`0x4CC057` from `0x4B86AA`. `0x4B8682`/`0x4B86AA` are inside `0x4B8500`; `0x4B8499` is in a
neighbouring function that takes its table from its own argument and never reads
`[globals+0xC0]`.

**The LUT's lifetime — the reason a pointer of ours must never outlive one call.**

| VA | What it does to `[globals+0xC0]` |
| --- | --- |
| `0x4BA5C0` | **Allocates it**: `push 0x10000; push 0x50A430` (a tag string); `call 0x4D83B0` (TA's allocator); `mov [ecx+0xC0],eax`. |
| `0x4BA5F0` | **Frees it**: `mov ecx,[eax+0xC0]; push ecx; call 0x4D85A0` (TA's free). |
| `0x4BAAD0` | **Overwrites it wholesale**: `mov edi,[eax+0xC0]; mov ecx,0x4000; rep movsd` — 64 KB copied *in*. |
| `0x4BA750` | Builds its contents (0x510 bytes of stack scratch; same bit-5 gate). |

So the slot is a heap buffer with a lifetime, not a hook point. Leaving our own table's address
there across a frame would let a reload `rep movsd` 64 KB into *our* buffer — silently undoing
the fix while leaving the engine's real table stale for every other blend in the session — or
let the teardown hand a DLL-heap block to TA's static-CRT free. `0x4D83B0` / `0x4D85A0` are
TA's own malloc/free.

##### The three tables are ONE family, and each is rewritten IN PLACE behind its own caps bit [DISASSEMBLED 2026-09-21, by landing 8d's review]

`0x4BAAD0` above is not a one-off. Three consecutive five-byte-aligned stubs do the identical
thing to three different slots of the graphics globals, and reading them together is what says
what `[globals+0xF0]` actually means:

| VA | slot | caps bit tested | dwords copied | bytes |
| --- | --- | --- | --- | --- |
| `0x4BAAD0` | `+0xC0` — the ALP blend LUT | `shr cl,5` → **bit 5** | `0x4000` | 65536 |
| `0x4BAB00` | `+0xC4` — `GFX_SHD`, the PALETTE.SHD **darken** table | `shr cl,6` → **bit 6** | `0x800` | 8192 |
| `0x4BAB30` | `+0xC8` — `PROG_LHT`, the **lighten** table | `shr cl,7` → **bit 7** | `0x800` | 8192 |

Each is `stdcall(src)` `ret 4`: `call 0x4B6220` for the globals, `mov cl,[eax+0xF0]`, shift its
bit into bit 0, `test cl,1`, and on clear return without writing anything. On set it loads the
EXISTING pointer out of the slot and `rep movsd`s the caller's buffer into it. **Bit 6 is new
here** — the table at line 3421 named bits 5 and 7 and had nothing for the darken table.

Two consequences, and landing 8d walked into both:

- **The caps bit says the table is enabled, not that it is filled.** The init writes the caps
  word `[globals+0xF0]` at `0x4B5AC4`, before either allocation; then a set bit 6 allocates the
  SHD's 8192 bytes (`0x4BA610` at `0x4B5ADF`) and a set bit 7 the LHT's (`0x4BA660` at
  `0x4B5B0F`: `push 0x2000; push 0x50A448; call 0x4D83B0`, the pointer stored at `[ecx+0xC8]`,
  the caps word not touched), and a clear bit stores NULL instead (`0x4B5AE6`, `0x4B5B16`). The
  bytes come later, from the setters at UI init (`0x4BAB00` from `0x42E21B`, `0x4BAB30` from
  `0x42E2AB`), which copy only when the bit is set. So a set bit and a non-NULL pointer are the
  same fact, and neither says the bytes have been written. `0x4BEC70` — the tint's own writer —
  tests only the pointer (`0x4BEC7B`), which is the bit's test too; `tagpu_packet_pub.c`'s
  `lht_snapshot` tests the bit, and the GUI queue's copy re-sends the rows by content when the
  setter fills them.
- **The rewrite keeps the pointer.** `0x4BAB30`'s only caller is `0x42E2AB`, which loads
  PALETTE.LHT into a heap buffer through `0x4BBC40`/`0x4BBE50`, calls the setter, and frees the
  source immediately at `0x42E2B1`. The table's address is therefore a *poor identity*: a
  regeneration changes all 8192 bytes and moves nothing, so anything latched on the pointer sees
  no change at all. This is the same hazard the `+0xC0` paragraph above describes from the other
  side — there the danger is our buffer being written *into*, here it is the engine's buffer
  being rewritten *under* a cached copy.

**The composite, and who reaches it.** `AlphaCompsteBuf2OFFScreen 0x4B8500` re-reads
`[globals+0xC0]` *inside* the call, at `0x4B8665` and `0x4B8691`, which is what makes a swap
bracketed around the call visible to it. It has **24 call sites**: `0x4399BB`, `0x459319`,
`0x459353`, `0x4593BA`, `0x4595E9`, `0x4597D3`, `0x46A7AF`, `0x46A807`, `0x46A840`, `0x4736B1`,
`0x474291`, `0x474CA3`, `0x475080`, `0x4755C6`, `0x475757`, `0x49C0F5`, `0x49C24A`, `0x49C2DC`,
`0x49C40E`, `0x49C46F`, `0x4B7FFE`, `0x4B81BE`, `0x4B838A`, `0x4B8579`. All 24 accounted for,
because this survey is the evidence that a bracketed swap cannot be observed by anything else:

| Sites | Where | Inside the capture window? |
| --- | --- | --- |
| `0x4399BB` | the target sprite `0x439740` | **yes, always** — this is the one we wrap |
| `0x4B7FFE` | `CopyGafToContext`'s composite branch, taken when a sub-frame's `+0xB` is non-zero | **yes, but GAF-data-gated** — the route dots; stock `pathicon` frames do not take it |
| `0x459319`, `0x459353`, `0x4593BA`, `0x4595E9`, `0x4597D3` | the unit row sweep | no — earlier in the frame |
| `0x4736B1`, `0x474291`, `0x474CA3`, `0x475080`, `0x4755C6`, `0x475757` | the effect-object handlers, reached through `0x471F90`'s indirect `call [edx+8]` at `0x471FBB` (vtable `0x4FD638` slot 8 = `0x475700`) | no — but note **both hooks call `0x471F90` themselves**: hook 8 draws layer 8 *before* opening the window and hook 9 draws layer 9 *after* closing it, so they sit outside it by ordering rather than by address. That ordering is load-bearing; see `tagpu_markown.c`. |
| `0x49C0F5`, `0x49C24A`, `0x49C2DC`, `0x49C40E`, `0x49C46F` | the projectile pass `0x49BE60`, called at `0x469B22` | no — before hook 8 |
| `0x46A7AF`, `0x46A807`, `0x46A840` | past `DrawGameScreen`'s `ret` at `0x46A3FD` | no — different function |
| `0x4B81BE`, `0x4B838A`, `0x4B8579` | inside the `0x4B8xxx` composite family itself (self/sibling recursion) | only as children of a site above |

| VA | What it is |
| --- | --- |
| **`0x439740`** | **The order pass's target sprite** — the pulsing star at a move/attack waypoint, and the only alpha-composited marker. `stdcall(ctx, view, node, pos, flag)`, `ret 0x14`. Two direct `E8` callers: `0x439516` (inside the route-dot drawer `0x4394E0`) and `0x439C7D` (the walker `0x439B30`'s bit-3 dispatch). Its address is **also** in `.rdata` 19 times as the `+8` field of the 25-byte order-descriptor records behind `*(u32*)0x512344` — first occurrences `0x4FC4B1`, then `0x4FC754`/`0x4FC76D`/`0x4FC786`/`0x4FC79F` at stride 25. That field is reported unread in this build (the dispatcher loading only `+0`, `+4`, `+0xC`, `+0x10`, `+0x14`, `+0x15`) — *that* half is [FROM REVIEW, not re-derived here]; the 19 records and the stride are measured. If it were ever brought into use, a wrapper on the two `E8` sites would be bypassed silently. |
| `0x4B7F90` | `CopyGafToContext` — the route dots' blitter, and **usually** a masked copy. But `0x4B7FF7` reads each sub-frame's byte at `+0xB` and `jbe`-skips only when it is zero: non-zero routes into `0x4B8500` at `0x4B7FFE`. So whether the dots blend is a property of the **GAF data**, not of the code. Stock `pathicon` frames do not carry it, which is why they render solid. |
| `0x4BF8C0` | `DrawTranspRectangle` — named for its hollow centre, **not** for translucency. It opens by acquiring a drawing context through `0x4C5E70` (`ret 4`: calls `0x4B6220` for the graphics globals, and when `globals+0xDC` is set copies 0xC dwords from `globals+0xBC` into the caller's stack block and returns 1) and **abandons the whole draw when that returns 0** (`0x4BF8D7`, `je 0x4BFD49`); the companion release is `0x4C5FA0`. Then four edges, each clipped by `0x4BEA20` and written by the store-only `0x4CC7AB` — ten calls to each across the body — and it reaches no alpha composite. Corrects a long-standing claim in `ui-markers.md` and `tagpu_markown.h` that its "transparent edges" read the destination. [The store-only writer was FROM REVIEW and is now disassembled — see "the post-fog build cursor and band box" below, which also has the argument list, the edge split and the surface bound that clips it. The `0x4C5E70`/`0x4C5FA0` pair came from the aircraft-shadow landing's own read of this function and is kept here rather than lost to the merge.] |
| `0x4C14F0` | `DrawTextCustomFont` (the group digit and the range labels, drawn inside the same block). Calls `0x4B6220`, `0x4B6750`, `0x4C5E70`, `0x4C5FA0`, `0x4C6AE0`, `0x4CCF60`, `0x4E4760` — it blits through `0x4CCF60` and never touches the LUT, so an identity table cannot affect it. Full anatomy in §"The in-game bitmap font". |

### `DrawGameScreen 0x468CF0` — its arguments, and the branch that skips hook 9

Prologue `sub esp,0x214` then four pushes (`ebx`, `ebp`, `esi`, `edi`), so inside the function
argument 1 is at `[esp+0x228]` — which is the `ebx` the marker block tests. Signature confirmed
as `(drawUnits, blitScreen)` stdcall.

**`0x469C01 test ebx,ebx / 0x469C03 je 0x469D38` jumps PAST hook 9 at `0x469D2C`.** A
`drawUnits == 0` frame therefore runs hook 8 and never reaches hook 9 — and the order markers
are drawn at `0x469BFC`, *before* that branch, so such a frame does reach the sprite. Any
state a capture opens at hook 8 must be able to survive not being closed.

| Caller | Passes | Which is it |
| --- | --- | --- |
| `0x495C76` | `drawUnits=1`, `blitScreen=0` | literal `push 0x1` |
| `0x495E66` | `1`, `1` | literal |
| **`0x4962C2`** | **`drawUnits=ebx=0`**, `1` | **TA's movie recorder.** Function starts `0x495E88`, `xor ebx,ebx` at `0x495EA1` and no other write to `ebx` before the call. Strings: `"%s\\MOVIE%03i"` `0x509500`, `"%s\\MOVIE*"` `0x509510`, `"FRAM"` `0x5094F8`. |
| `0x4969CD` | `1`, `1` | the in-game frame callback `0x496790`, which sets `mov ebx,1` at `0x4967CF` and uses `ebx` as its constant 1 throughout. |

Live counter-check on the played path: hook-8 opens and hook-9 closes were equal across
~50 000 blocks of a skirmish, so nothing in normal play takes the `drawUnits == 0` route.

### The sweep order inside `DrawGameScreen`, by call site

[BINARY-VERIFIED 2026-09-04 — an `objdump` of `0x468CF0..0x469C60` filtered for these four
targets, re-read for the aircraft work rather than taken from the earlier note.]

| VA | Calls | What |
| --- | --- | --- |
| `0x469920`, `0x46992F`, `0x469ABB` | `0x46A610` | features |
| `0x469A00` | `0x45AC20` DrawUnit | **site A** — ground units, `(state&3)==1`, per sort row |
| `0x469B22` | `0x49BE60` | weapons: laser lines and projectile GAFs |
| `0x469B2C` | `0x420B00` | explosions and effects |
| `0x469BA3` | `0x45AC20` DrawUnit | **site B** — everything `(state&3) != 1`, over ALL rows |

#### The TWO WORLD SPANS, and why the draw is not one run [BINARY-VERIFIED 2026-09-22]

Read for the UI layer's world phase (`tagpu_gui_hook.c`, gpu-status §2.81): the question was
"where does `DrawGameScreen`'s world draw begin and end", and the answer is that it has **two**
straight-line spans with UI between them, not one. The whole call list of
`0x468CF0..0x46A400`, filtered to what writes pixels, in address order:

| VA | Calls | What | phase |
| --- | --- | --- | --- |
| `0x468D30` | `0x4C69A0` | makes `main+0x37E1B` the back buffer | — |
| **`0x468DB0`** | `0x483FA0` | **the terrain grid blit** | **WORLD opens** |
| `0x468DBA` | `0x418310` | the map debug overlay, normally a no-op | WORLD |
| `0x468E16`, `0x468E30` | `0x4BE950` | **two lines at an EYE-RELATIVE position** — `esi`/`edi` are `main+0x2CAC/+0x2CB0/+0x2CB4` minus the eye `+0x1431F`/`+0x14323` with the `+0x80`/`+0x20` bias, i.e. the world-space projection idiom. Gated on `main+0x14280 == 2` (`0x468DFF jne 0x468E35`). Colour `[ebx+0x0F]` | WORLD |
| **`0x468E3A`** | `0x4C69C0` | the clip reset; the join of that branch, so it is reached on every call | **WORLD closes** |
| `0x468E40..0x469610` | — | the METAL/ENERGY resource block | UI |
| `0x469615` | `0x46A860` | `stdcall(ctx) ret 4`, sole caller. Reads `main+0x37E23` (screen H) and `main+0x147A7` and draws along the bottom of the screen; `chrome_emit` (`tagpu_gui_hook.c`) records that its two GAF sites redraw the bar's FIRST tile rather than the continuation, so it is **not** the function that lays the repeats. [INFERRED: the bottom HUD bar.] Not disassembled further | UI |
| `0x46961F` | `0x466B00` `DrawMinimap` | the minimap | UI |
| `0x46964F` | `0x4C6B10` | the draw context re-seeded from `main+0x37E27` | — |
| `0x4696CD`/`0x4696E7` | `0x4C1420`/`0x4C13A0` | the marker block's font and text-colour latch | — |
| **`0x469849`** | `0x471F90(ctx, 0)` | **particle layer 0** | **WORLD opens** |
| `0x469855`, `0x469861` | `0x471F90` | particle layers 1 and 2 | WORLD |
| `0x469920`… | see the table above | features, units, weapons, explosions | WORLD |
| `0x469BD7`…`0x469D2C` | `0x471F90(ctx, 8/9)` | the marker block: order markers, health bars, the group digit | WORLD |
| `0x469D8E` | `0x4848E0` | the fog overlay | WORLD |
| `0x469EC5`, `0x469F1E` | `0x4BF8C0` | the build cursor and the drag band box — the last world-anchored draw | WORLD |
| **`0x469F36`** | `0x435100` | a mode query on `main+0x391E9` | **WORLD closes** |
| `0x469F40..0x46A3E0` | — | the HUD extras tail (below) | UI |

**The two closers are reached on EVERY call**, which is what lets a flag set at an opener be
relied on to come down inside the same function. `0x468E3A` is the join of `0x468DFF jne
0x468E35`; `0x469F36` is the join of the whole build-cursor block — `0x469E0D je 0x469F30` from
the gate and a fall-through from `0x469F23` when it did draw, both landing on `0x469F30`. Neither
is inside a loop, and `0x469849` is reached straight-line from `0x4696E7` (the intervening
`0x4696EC..0x469842` is arithmetic and two loops with no call in them at all).

**`0x435100` is `mov eax,[ecx]; ret`** — one argument, in ECX, nothing on the stack, no pop:
`__fastcall` for a single pointer in GCC's i386 spelling. Its caller reloads both ECX and EDX
immediately afterwards (`0x469F40`/`0x469F46`) and uses only EAX from it. `0x4C69C0` is
`stdcall(ctx) ret 4`, `0x471F90` `stdcall(ctx, n) ret 8`, `0x483FA0` `stdcall(ctx) ret 4`.

**NEGATIVE RESULT, and it is the one that settled the design.** The obvious single bracket —
open at the terrain blit `0x468DB0` and close at the tail — is WRONG: the resource block and the
minimap are painted *after* the terrain and *before* the unit sweep, so one span from `0x468DB0`
to `0x469F36` takes the metal/energy readout and the minimap with it. The terrain blit's own
subtree is also provably incapable of leaking through an observed leaf: `0x483FA0..0x4843C0`
calls `0x4B8150` ×4 and `0x4C6E70` once and nothing else, neither of which is a UI leaf. The
feature leaf `0x46A610..0x46A860` is the opposite case — `0x4B7F90` ×5, `0x4B8500` ×3,
`0x4B7EE0` ×4, `0x4B7F30` ×2 and `0x45AC20` once — of which **two**, `0x4B7F90` and `0x4B8500`,
are in `tagpu_gui_leaves.h`, which is why features were the measured leak. (`0x4B7EE0` was
omitted and the count given as "three" in the first revision of this section, 2026-09-22; both
are corrected here against `objdump` over the range.)

#### The resource block, and the 33-byte memo that skips it [DISASSEMBLED 2026-09-21]

`DrawGameScreen` runs every frame, but its METAL/ENERGY block at `0x468E40..0x469610` usually does
not. (The extent is the skip's own target: `je 0x469610` jumps over the block, and `0x469610` is
past the `0x4692C0` this section first gave as its end.) The block seeds a stack local from a **33-byte memo at `main+0x37E3F`** (`0x468E51`, `rep
movsl` ×8 + `movsb`), overwrites the live fields, then `repz cmpsb`s all 33 bytes against that memo
at `0x468FD9` and **skips the whole block when they match** — `je 0x469610` at `0x468FDB`. Only on a
difference does it copy the fresh state back (`0x468FEC`) and draw.

**The memo has THREE references in the binary** — `0x468E51` (seed), `0x468FC6`
(compare/update) and `0x4679A6`, an `add eax,0x37e3f` inside the function at `0x4679A0` that
zeroes four dwords of the memo (`[eax+0x1D]`, `[eax+0x19]`, `[eax+0x0D]`, `[eax+0x01]`) before
going on to clear `+0x37E90`/`+0x37E94`. It does **not** write byte 0, so nothing here opposes the
poke.

*This line read "exactly two references" until the landing review of 2026-09-21 disproved it, and
the mistake is instructive: the evidence quoted for it was `objdump -d | grep '0x37e3f('`, and that
trailing parenthesis only matches the memory-operand forms `0x37e3f(%reg)`. The third site is a
plain immediate — `add eax,0x37e3f` — so the grep that "proved" the claim was built to miss it. The
search to run is `grep 37e3f`, unanchored.* It is a display memo, not sim state.

**Which bytes are read back and which are not.** Byte 0 of the local is written at `0x468E7F` from
`cl`, a byte taken out of the player record, and is never read from the memo. The byte is
**`PlayerStruct+0x146`** of the local player: `0x468E4B` takes p = `main[0x2A43]`, and
`0x468E6C` loads `[main + p + 330p + 0x1CA9]`, i.e. `main + 0x1B63 + 331p + 0x146` — TADR's
`PlayerAryIndex` *[INFERRED name]*; it read **0x00** for player 0 (MEASURED 2026-09-23, the memo
read back after a draw). p is used unbounded. **Every writer of `+0x146` stores 0..10**
[DISASSEMBLED 2026-09-23, two landing reviewers independently]: `0x463C05` the constant 10 (the
player constructor), `0x44541C` / `0x445565` / `0x44A8F6` the record's own slot or 10 for an
absent one (each loop counts every record, `inc ebx` at `0x44543C`, `0x445585`, `0x44A916`, and
stores at `0x445426`, `0x44556F`, `0x44A900` [DISASSEMBLED 2026-09-28]), `0x46434D` the slot
`0x464290` is called for (with `+0x147`/`+0x148`) — so 10 is
"no player", and a byte outside 0..10 is one the block never computes. Bytes 1..4 ARE read
from it — `flds 0x79(%esp)` at `0x468E7B`, before byte 0 is written — and are the displayed
number's animation state: `0x468E83`/`0x468E90` convert the old and new values and `0x468E9F`/
`0x468EB3` divide the difference by 8, so the number eases toward its target over frames. That
asymmetry is what makes byte 0 the only one a fork may poison to force a redraw without
fabricating a value the engine then displays.

What the block draws, all through leaves the GUI observer already has: four rects via `DrawBar
0x4BF6F0`, the six numbers via `DrawTextCustomFont 0x4C14F0` ×3 — which
reaches the hooked glyph blitter `0x4CCF60` internally — and the player's colour badge via
`0x467C00` at `0x4690AB`.

The four rects [DISASSEMBLED 2026-10-07]:

- **The energy fill**, at `0x46912E`. Its right edge is the bar rect's left plus its width times the
  eased value at `[esp+0x79]` (the memo's bytes 1..4 above), over the storage `[ebp+0xa4]`.
- **The metal fill**, at `0x469412`. The same, with `[esp+0x85]` over `[ebp+0xa8]`; that
  `[esp+0x85]` is metal's eased value is [INFERRED] from the energy case.
- **Their colours.** Each fill takes the side data's colour: `RaceSideData +0x222 energycolor` and
  `+0x226 metalcolor` (`tools/tamem_ghidra.h`). `sidedata.tdf` sets them to 208 and 224, which
  `PALETTE.PAL` holds as `#ffff00` and `#d3dbff`.
- **A tick after each fill**, 2 px wide (`0x4691B2` energy, `0x469492` metal). It sits at a second
  level over the same storage (`[ebp+0xe8]`, `[ebp+0xe4]`), in colour byte `[edi+0xc]`. That the
  level is the share setting is [INFERRED].

The `0x4691B2` call is the energy tick, not a fill. The guard above the first bar — `fcomps 0x4FD568` at `0x4690BD`, `jne 0x4691B9` at `0x4690ED`
— is a divide-by-zero check on max storage, not a dirty test.

#### `0x467C00` — the player's colour badge, and where its colour comes from [DISASSEMBLED 2026-09-21]

Reads a byte at **`player+0x96`** (`0x467C18`), the field next to the side byte at `+0x95`, then
indexes the table at **`main+0x148DB`** with **stride 8** and takes the GAF frame at **`+0x28`**
(`0x467C24`). That frame is pushed straight to `GAF_DrawTransformed 0x4C7580` at `0x467C8F`. So
**each player colour is a different GAF frame, not a palette remap of one** — an observer that
captures the frame the engine passed follows the colour with no colour logic of its own. Measured
across five colours at 1024×768, every badge exact against the engine's own frame.

#### `GAF_DrawTransformed 0x4C7580` — the vertex roles, and the half-open span [MEASURED 2026-09-21]

It takes **FOUR vertices, not three** — `0x4C763D..0x4C7679` loops `ecx = 0..3` over arg3 with
`add edx,0x8`, so the arrays are `xy[8]`/`uv[8]`, and when `uv` is NULL it synthesises its own quad
`(0,0) (w-1,0) (w-1,h-1) (0,h-1)` at `[esp+0x4C..0x68]` (note `w-1`, not `w`). The roles are
**origin, `+u`, `+u+v`, `+v`**: the in-game badge is
`xy=(132,5)(152,5)(152,25)(132,25)` with `uv=(0,0)(32,0)(32,32)(0,32)`, a 32×32 frame onto a 20×20
quad, and `0x467C00` builds it by storing the same register twice — `edx` into `xy[0]` and `xy[6]`,
`ecx` into `xy[5]` and `xy[7]` — which is what makes the fourth corner checkable.
*[CORRECTED 2026-09-21 by the landing review; this section first said "three vertices … not an
arbitrary triangle", having looked only at the first three.]* **The
span is half-open in both axes** — the far vertex is the edge the fill stops before, not a pixel:
that badge covers x 132..151, twenty columns, where the vertices' bounding box says 132..152. The
sampling steps a **16.16 accumulator**, not `(x * sw) / dw`: with `sw=32, dw=20` the truncated step
is 104857, so x=5 accumulates 524285 and shifts to 7 where the division gives 8, and sixteen of the
badge's 400 pixels differ on exactly that.

**And the uv quad is a WINDOW, not necessarily the whole frame** [MEASURED 2026-09-21, landing 8e].
The badge is the degenerate case and reading only it makes `uv` look like a formality.
`SKIRMISH.GUI`'s four player-colour swatches use the same function with a genuine sub-rectangle:

```
xy = (214,94)(233,94)(233,113)(214,113)        the destination, 19 x 19 half-open
uv = (1,1)(31,1)(31,31)(1,31)                  a 30 x 30 window inset one texel
frame 32x32, comp=0, sub=0/0
```

The u/v corners carry the same roles and the same half-open convention as the screen ones, so the
source is the rectangle `(uv[0], uv[1])` with extents `uv[2]-uv[0]` by `uv[5]-uv[1]`. A consumer
that reads the frame's own dimensions as the source — which is what ours did until 8e — resamples
32 texels where the engine resamples 30 and is a texel out over most of the span. **The inset is
not decoration:** these frames are 32×32 tiles whose outer ring is border, and the caller trims it.

Note the asymmetry this leaves: a caller that passes `uv == NULL` gets the synthesised
`(0,0) (w-1,0) (w-1,h-1) (0,h-1)` above, whose extent is `w-1` and not `w`.

**There is only ONE convention, not two, and the substitution is what proves it**
*[CORRECTED 2026-09-21 by 8e's review, which disassembled the path this first called "not
established"]*. `0x4C75EB..0x4C761F` builds the quad into `[esp+0x4C..0x68]` and then
**overwrites the argument slot in place** — `mov DWORD PTR [esp+0x7dac],edx` at `0x4C75F4` — so
from there on the function cannot tell a synthesised quad from a caller's. The single
interpolation path follows: the edge walk at `0x4C77C2..0x4C77FA` steps `((uv_b-uv_a)<<16)/dy`
from `uv_a<<16`, and the span filler `0x4C7310` steps `(uR-uL)/(xR-xL)` from `uL`. The
synthesised quad is therefore the SAME half-open convention naming a `(w-1)×(h-1)` **window** —
the engine's own default drops the frame's last row and column — and `0x4C75FB`/`0x4C7616`'s
`dec eax` is where that comes from, against `0x467C34`/`0x467C4A`, which store `WORD[frame+0]`
and `[+2]` undecremented for the badge. The asymmetry is real; the hedge about which rule applies
was not.

The fork still **refuses** a NULL-uv draw and publishes its box. That is now a choice rather than
an unknown: no measured caller passes NULL, so supporting it would be code whose output nothing
could be compared against, and an unverified branch is worse than a named gap.

#### `main+0x1426B` — the level's minimap picture is a PADDED square [MEASURED 2026-09-21]

The frame's header is 252×252, but the map only fills an aspect-correct region inside it: on a
336×400 map the last column that is not internally constant is **212**, and 213..251 are one
uniform colour, while rows run to 251. `0x46684F` loads it and `0x46685F` hands it to the stretch
`0x4B95A0` with the box-sized context built at `0x466845`. A consumer that stretches the whole
frame into the box puts 40 columns of padding on screen.

#### The 3D wreck draw: a husk borrows the UNIT pipeline through one shared scratch [VERIFIED 2026-09-14]

The feature draw `0x46A610` (arg = a feature-grid cell) splits twice. `0x46A6B5`
`test [cell+0xC],1` separates wreck cells from plain features; `0x46A6DC`
`test [def+0xFE],1` then separates **GAF wrecks** (bit set, `0x46A6E1`, their own shadow through
`0x4B7EE0`/`0x4B7F90` on `rec+0x10`, gated by the FShadow option `[main+0x37F06]&0x10` at
`0x46A6ED`) from **3D wrecks** (bit clear, `0x46A721`).

There is no separate shadow path for a 3D wreck. It is drawn as a UNIT, through a single
**scratch feature-unit** — a 0x118-byte record allocated once at feature init
(`0x421F83 MEM_Alloc(0x118)`, stored at `main+0x1420F`, zeroed; `0x421FD3` then sets
`[scratch+0x110] |= 0x20000000` permanently, i.e. the structure bit, for the life of the
process). Per husk, `0x46A721` fills it in and draws:

```
0x46A721  mov eax,[main+0x1420F]   ; the scratch feature-unit
0x46A727  mov ecx,[rec+0x4]        ; this husk's Object3do
0x46A72B  mov [eax+0x9E],ecx       ; scratch -> Object3do
0x46A731  mov [ecx+0x0C],eax       ; Object3do+0x0C -> scratch   (the unit field)
0x46A73D..0x46A75A                 ; rec+0x20..0x25 -> scratch+0x64; rec+0x08/0C/10 -> +0x6A/6E/72
0x46A762  call 0x45AC20            ; DrawUnit(ctx, scratch)
```

`DrawUnit` reads `[scratch+0x9E]` at `0x45AE65` and calls `0x458810`, which reaches the blit
`0x459200` at `0x458948` — **so a husk does reach the blit's three shadow emit sites.** Which one
depends on us: the scratch's permanent `0x20000000` puts both silhouette sites on the *bit-clear*
side of the `je`s at `0x4592C6` and `0x45952C`, and those are exactly the two branches
`tagpu_owndraw.c` takes over under target `all` (flipped to `EB` until 2026-09-18, detoured from
`0x4592BF`/`0x459522` behind a runtime flag since). **Unpatched, a husk reaches only `0x4594DB`**
(itself gated at `0x4594D0` on `[[scratch+0x92]+0x241] & 0x40000000`, where `+0x92` is
`[main+0x1439B]`, the UnitInfo array base, copied in once at feature init by `0x422003`
(`mov ecx,[eax+0x1439B]`) and `0x422009` (`mov [edx+0x92],ecx`) — so that test reads `UnitInfo[0]`'s flags, an
arbitrary loaded def with nothing to do with wrecks).

**`Object3do+0x0C` on a husk is a real pointer, to the shared scratch** — not to a unit record,
and not null. Any predicate that only checks it is a plausible pointer passes for a husk. The
live-unit analogue is `0x485E14 mov [eax+0xc],esi`.

**The scratch is never cleared, which is an ABA.** `FEATURES_Destroy 0x42474F` calls
`FreeObjectState 0x45AAA0` on the husk's `Object3do` and nulls `[rec+4]` at `0x424754`, but leaves
`scratch+0x9E` naming the freed block. If the allocator returns that block for a live unit's
`Object3do` (`0x485DCC` / `0x485E0E`), a recogniser testing only `*(scratch+0x9E) == obj3do`
matches that live unit and goes on matching until the next 3D-wreck draw. Testing
`*(obj3do+0x0C) == *(main+0x1420F)` as well closes it, because a block that has become a live
unit's carries that unit's own record at `+0x0C`. `tagpu_owndraw.c`'s `is_wreck_draw()` tests
both. Residual, not closed: the constructor path `0x485DCC` / `0x45A950` was not observed writing
`+0x0C` within the first 0x60 bytes, so a reused block taken down that path could still carry a
stale scratch there.

Site B is last, and a unit's shadow is blitted inside that same `DrawUnit` call (the branch
table above), through `0x4B8500`, which has **no depth test** — the ALP blit writes every
non-key pixel of its source. So an aircraft's ground shadow composites **above** the ground
units, the features, the projectiles and the explosions. That is the engine quirk
`shadows-cloak.md` §4 flags; it is settled by this ordering, and not by a screenshot — several
staged attempts to catch a shadow lying across a fireball never lined the two up.

### `0x469DB4..0x469F23` — the post-fog build cursor and band box, and why no capture can reach them

[MEASURED 2026-09-04, this project — disassembly of the pristine Steam build, plus a live A/B
at 1x, 2.144x and 0.467x. This is the block `tagpu_mark.c` now re-draws.]

The last world-anchored thing `DrawGameScreen` paints, after its fog overlay: one
double-outlined rectangle that is *either* the build-placement footprint *or* the drag band
box. One gate, one projection, two `DrawTranspRectangle` calls.

**The gate** (`ebx` is the `drawUnits` argument — `0x469DB4 test ebx,ebx`):

```
0x469DC2  test byte [main+0x2CC6],8   -> non-zero: draw  (the band box, forced on)
0x469DD8  cmp  byte [main+0x2CC3],0xE -> not 14: do not draw
0x469DE1  IsPositionInRect(main+0x37E27, main[0x2C76], main[0x2C7A])
0x469E0B  test eax,eax / je 0x469F30
```

`IsPositionInRect 0x4B6720` is `stdcall(RECT*, x, y) ret 0xC` and is **inclusive on all four
edges**: `x < r[0]` or `x > r[2]` or `y < r[1]` or `y > r[3]` returns 0, else 1. The rect it
reads is the viewport rect — the field `tagpu_vpwide` deliberately widens at zoom < 1, which
is why a placement out in the ring passes this gate at all.

**The projection** (`0x469E13..0x469E68`), all six source fields read as DWORDs:

```
l = main[0x2C92] - eyeX + 0x80              eyeX = main[0x1431F]
r = main[0x2C9E] - eyeX + 0x80              eyeY = main[0x14323]
t = main[0x2C9A] - (main[0x2C96] >> 1) - eyeY + 0x20      (SAR, signed)
b = main[0x2CA6] - (main[0x2CA2] >> 1) - eyeY + 0x20
if (r < l) swap;  if (b < t) swap           0x469E92 / 0x469EA0
```

`+0x80`/`+0x20` are **baked immediates** — the TRUE viewport origin, not a read of `L`/`T`.
So the block projects against the true origin while its gate tests the widened rect; that
combination is what makes the rect correct in the ring and is why `tagpu_vpwide`'s `0x498DA0`
stub redoes the mouse conversion with the true origin and the wide clamp.

**The unit being placed — `BuildUnitID main+0x2CC4` [VERIFIED LIVE 2026-09-12].** A u16,
the UnitDef index of the building the placement cursor is carrying — the same index space as
`UnitStruct+0xA6`'s `ModelId` and `OrderNode+0x36`'s build type, so it indexes both UnitDefs
and `MODEL_PTRS` directly. TADR's `tamem.h` names it `BuildUnitID`, sitting between the mode
byte `0x2CC3` and the flags byte `0x2CC6` (layout 1+2+1). Verified on the build-ghost
landing: with a commander selected, clicking `ARMMEX` on the build page puts the mode byte at
14 and the word at 78, the mex's in-game UnitDef index, while `0x2C92..` holds its 48×48
footprint. A build-menu click changes no GUI state — only the mode byte moves — so the type
is latched here, not in any `.GUI` gadget record. The frame packet carries it as
`build_unit_id` for the ghost pass.

**`0x4197D0` — who fills `0x2C92..0x2CA6`: the footprint snapped around the world point
[DISASSEMBLED 2026-09-23; MEASURED the same day].** `cdecl()`, no arguments, `ret`. Two call
sites, both behind "pointer on the world (`0x2CC6` bit 1) and mode `0x0E`": `0x499241` in the
mode-6 mouse handler, right after `0x498DA0`, and `0x491CDB` inside `0x491CC0` (`ret 4`). It
reads nothing about the screen — only the world point `main+0x2CAA` that `0x498DA0` just
left there (x, y, z in 16.16) — so **the square follows the world point, and every question of
where it lands relative to the pointer is a question about `0x498DA0`'s input and about where
the world is drawn.**

```
def   = *(main+0x1439B) + BuildUnitID*0x249          ; the UnitDef, stride 585
fx,fz = i16 [def+0x14A], i16 [def+0x14C]             ; footprint in 16-px cells
cx0   = (TPos.x - fx<<19 + 0x80000) >> 20            ; round((x - 8fx) / 16)
cz0   = (TPos.z - fz<<19 + 0x80000) >> 20            ;   TPos.y is loaded and not used
main[0x2C92] = cx0*16          main[0x2C9E] = cx0*16 + fx*16
main[0x2C9A] = cz0*16          main[0x2CA6] = cz0*16 + fz*16
ok    = 0x47D2E0(def, cx0 | cz0<<16, 0, main+0x1B63+331*player) & 1
main[0x2CC6] bit 6 = ok                               ; green vs blocked (the colour 0x469E6B picks)
alt   = ok ? 0x47C780() : 0x47D820(def, cx0 | cz0<<16)   ; a byte
main[0x2C96] = main[0x2CA2] = alt                     ; both corners' altitude
return ok
```

So the square's centre is within **8 world px** of the world point on each axis, at every zoom:
that is the snap, and at 7.7× it is up to 62 screen px. Measured at 1920×1080 on `ARMSOLAR`
(5×5 cells), the pointer's engine point at world x 1940: corners `1904..1984`, centre 1944. `0x47D2E0` is the site
test (disassembled, *A queued site is taken ground*, below); `0x47C780` / `0x47D820` are the
placement altitude for a valid and a blocked site *[INFERRED]* — neither was disassembled.

**A queued site is taken ground — `0x47D2E0`, the stamp `0x47CC30` and the click's orders
[DISASSEMBLED 2026-09-29; MEASURED the same day].** The build cursor asks `0x47D2E0` (stdcall
`(def, cell, 0, player)`, `ret 0x10`, the call at `0x4198C6`) and shows its answer as `main+0x2CC6`
bit 6; a click on red plays `notoktobuild` (`0x509650`) and orders nothing (`0x498F86` →
`0x499016`), on green `0x419670` issues the order. The test walks the footprint's cells and, per cell,
takes the def's yardmap byte (`def+0x14E`, `fx·fz` bytes, row-major): a byte with bit 2 or 4 whose
cell's unit word (`+0x00`) is non-zero and not the caller's own id refuses the site (`0x47D54C..0x47D561`).
That word is written by the stamp `0x47CC30` of a unit that exists, into the cells whose byte has bit 4
(bit 2 instead when the unit's `+0x10F` has bit 2; `0x47CD8E..0x47CD9D`). **A site that is queued and not
yet built stamps nothing**, so a second site over it reads green, is ordered and fails when the first
is built. The yardmap chars are `.`=`00` `c`=`2D` `o`=`2F` `C`=`35` `O`=`2B` `w`=`37` `y`=`29` `Y`=`31` `f`=`6F`
`G`=`8F` (the table under *A yardmap parsed past its string*): `c`, `o`, `C`, `w`, `f` and `G` stamp and
test, `O` only tests, `.`, `y` and `Y` do neither.

What a click orders, for the queue's sake. `0x419670` orders each unit of the controlled player's block
(`main+0x2A42`) that is selected (`+0x110` bit 4) and whose def has `+0x241` bit 6, with the action name
`MOBILEBUILD` (`0x502654`) or, when the def's `+0x241` bit 11 is set, `VTOL_MOBILEBUILD` (`0x502640`),
through `0x438760` and `0x43AFC0`; the position is `((2·cx + fx) << 19, alt << 16, (2·cz + fz) << 19)`
with `cx`, `cz` the footprint's first cell. `0x43AFC0` with Shift (the click's `+8` bit 2, the mouse
message's `MK_SHIFT`) first scans the unit's `+0x5C` list for a node of the same action whose x and z lie
within ±`0x100000` (16 world units) of the new position (`0x43B00F..0x43B029`) and, on a match,
destroys it instead of adding one; without Shift `0x43ADC0` destroys every order of the unit whose flags
`+0x42` lack bit 4. The descriptors (`0x4FC6E8` family, `+0x0C` mask, `+0x11` flags): MobileBuild mask `0x13`
flags `0x100508`, VTOL_MobileBuild `0x03` / `0x100508`, HelpBuild `0x18` / `0x100208`, BuildingBuild `0x00` /
`0x10010C` (bit 4 set: a factory's queue survives the wipe), BuildWeapon `0x00` / `0x0C0140`. A factory's or
a mobile factory's queue is count nodes made with a NULL position, which `0x43A0C0` zeroes (`0x43A14E`).
`0x4C1B80(0xF9)` (stdcall, `ret 4`) is `GetKeyState(VK_SHIFT)` through the table at `0x4C1C48` (`0x4C1BB5`),
the modifier test of the order mode's release (`0x4992E6`) and of the menu's ±5.

**The fix** (`build_site_test`, `tagpu_patches.c`, local): the call at `0x4198C6` goes to a wrapper that
runs stock's test and answers no when the footprint shares a cell with a queued site of the controlled
player's units — one yardmap byte with bit 4, the other with bit 2 or 4, both directions since either
site may be built first — a site being a node whose descriptor mask has bit 0 (the one `0x438C00` draws a
rectangle for), a non-zero build type (`+0x36`) and x, z above 0. A unit the click would order does not
block with its own orders the click removes: without Shift the ones lacking flag 4, with Shift the one
within 16 units, so re-placing a plan and Shift-clicking a site to remove it work as in stock. Allies'
and enemies' plans, dead (`+0x110` bit 28 clear) and excluded (bit 14) units are not read. **MEASURED**
with `tools/queued-site-check.sh` (two CORCK, CORSOLAR, Wine, private Xvfb; 7 of 7 as expected): green on
open ground; with Shift and a site queued, red 40 px over it, green 140 px away and 8 px away; without
Shift green over the selected builder's own plan, red over an unselected builder's.
The refusal holds in fog too: stock's test reads a cell's unit word only where the player can see (`0x47D366`,
the LOS and mapped grids), and this reads the player's own plans, which the player knows.
**Gaps:** the alternate stamp mask (`+0x10F` bit 2) is not read, since the unit does not exist yet; the AI's
site tests (`0x40A4CD`, no player) and orders that do not come from the cursor — `tacli order`, scenarios,
a peer's packets — are stock's, so they can still queue over each other, and the second fails at build
time as before; the plans of a unit in a network game that is not the controlled player's are not
looked at (allies and enemies, by design).

**`0x46A530 DrawUnitSelectBoxRect` has NO ModelId test — a negative result that cost a landing
review to establish [BINARY-VERIFIED 2026-09-10].** Its only early-out is the `SelBoxes` option
bit:

```
46a538  mov  cl,BYTE PTR [eax+0x37f2f]     ; the UI gate byte
46a541  test cl,0x1
46a544  je   0x46a602                      ; the ONLY early return
46a54a  mov  eax,DWORD PTR [eax+0x14377]   ; MODEL_PTRS
46a55f  mov  dx,WORD PTR [esi+0xa6]        ; ModelId -- not tested
46a56b  mov  ecx,DWORD PTR [eax+edx*4]     ; MODEL_PTRS[ModelId], unconditional
46a56f  call 0x4cb650                      ; bound it, unconditional
```

So a unit whose `ModelId` is 0, or out of `UNITINFOCount`'s range, **still gets a box** — bounded
by whatever `MODEL_PTRS[0]` holds. Anything of ours that suppresses this call must therefore
either draw the box itself or leave the unit entirely alone; "it has no model, so the engine
draws nothing" is false, and believing it left such a unit permanently unmarked
(`ui-markers.md` §1, `gpu-status.md` §2.3a-bis).

**The colours.** `[esp+0x74]` is `main+0xDCB`, the GUI colour byte array — written once in the
prologue, `0x468D49 lea ebx,[eax+0xDCB]` / `0x468D51 mov [esp+0x74],ebx`. The outer index is
picked at `0x469E6B` from the MODE (not from which arm of the gate fired):

```
mode == 0xE:  and cl,0x40 / neg cl / sbb ecx,ecx / and ecx,6 / add ecx,4
              -> 0xA when main[0x2CC6] & 0x40 (site OK, green), else 4 (blocked)
otherwise:    0xF
```

and the inner rect, `{l+1, t+1, r-1, b-1}`, re-tests `main[0x2CC3]` at `0x469EF4`: build mode
reuses the same colour byte, anything else takes `gui[0]`. So build placement is a 2 px
single-colour outline and the band box is white over black.

**`DrawTranspRectangle 0x4BF8C0`** — `stdcall(ctx, RECT*, colour) ret 0xC`. Two paths on
`ctx == NULL` (`0x4BF8C6`): the null path fetches a global draw context through `0x4C5E70`
(which copies 12 dwords out of `[globals+0xBC]` or `globals+0x50`); the one `DrawGameScreen`
takes is `0x4BFC5F`, with the caller's context. Its four edges do **not** all go the same way:

| Edge | From | Via |
| --- | --- | --- |
| top `(l,t)-(r,t)` | `0x4BFC93` | `0x4BEA20` (clip) then `0x4CC7AB` directly |
| right `(r,t)-(r,b)` | `0x4BFCF5` | the same pair |
| bottom `(l,b)-(r,b)` | `0x4BFD2D` | `DrawLine 0x4BE950`, which does the same `0x4BEA20`+`0x4CC7AB` internally |
| left `(l,t)-(l,b)` | `0x4BFD40` | `DrawLine 0x4BE950` |

That asymmetry is a code-generation artefact, not a difference in output: all four reach the
same writer. `ui-markers.md` used to call two of them a "transparent line variant"; they are
the same line through one more frame. The two `test eax,eax / jne` on a `lea` of a stack slot
(`0x4BF905`, `0x4BF9CF`) are always taken, so the null-context arms below them are dead in
this build.

**`0x4BEC70` and `0x4CC8DF`, the TINTED line — and the focus rectangle is made of four of them**

**COUNT THE ARMS, NOT THE CALL SITES.** `0x4BF7B0`'s body contains eight `call 0x4BEC70`, and the
first version of this section read that as "eight edges, two concentric boxes". It is **four edges
of one box**: `0x4BF7BD` is `test edi,edi` on the **ctx argument** and `jne 0x4BF854`, so
`0x4BF7F3`..`0x4BF839` is the arm that ACQUIRES a context (`0x4C5E70`, released by `0x4C5FA0` at
`0x4BF843`) and returns at `0x4BF851`, while `0x4BF86A`..`0x4BF8A4` is the arm that uses the one it
was given. Both arms draw `(l,t,r,t)`, `(r,t,r,b)`, `(l,b,r,b)`, `(l,t,l,b)` — top, right, bottom,
left of the same rect. **This is the second time on this gate that a static call count was read as
a shape** (`0x4BF4D0`'s "three fills" was the first), and the cure is the same: follow the control
flow before believing an arithmetic of `call` instructions. [CORRECTED 2026-09-18 by landing 8b's
review.]

[DISASSEMBLED 2026-09-18, this project, for the vulkan-only plan's landing 8b].
`0x4BEC70(ctx, x0, y0, x1, y1, ?)` `ret 0x18` fetches the graphics globals through `0x4B6220`,
**returns 0 immediately if `[globals+0xC8]` is NULL** (`0x4BEC7B`), clips through `0x4BEA20`, and
hands `[globals+0xC8]` to `0x4CC8DF` as an argument. That is the **same lighten table `0x4BF4D0`
selects from**, and `0x4CC8DF` uses it the same way — per pixel:

```
shl eax,0x8          ; row << 8
mov al,BYTE PTR [edi]        ; the DESTINATION pixel, into the low byte
mov al,BYTE PTR [eax+esi*1]  ; esi = the table base -> LUT[row*256 + dest]
mov BYTE PTR [edi],al        ; write it back
```

**So a focus rectangle tints what is under it and has no colour of its own.** It cannot be
published as a box and an index; the consumer has to be able to read the surface it writes.

**And the index here is UNSIGNED where `0x4BF4D0`'s is SIGNED.** `0x4CC8DF` builds
`(row << 8) | dest` by overwriting the low byte of a register, so `dest` is 0..255 unsigned;
`0x4BF4D0` does `movsx ebx,BYTE PTR [ecx]` and indexes `[row_base + ebx]`, so its 0x80..0xFF land
*before* the row base. Two functions, one table, two addressing conventions — a consumer that
reimplements either must not copy the other's.

##### The full argument map, and the row is carried through unmasked [DISASSEMBLED 2026-09-21, for landing 8d]

`0x4BEC70` is **`(ctx, x0, y0, x1, y1, row)` stdcall `ret 0x18`**, and `0x4CC8DF` is
**`(ctx, x0, y0, x1, y1, row, lut)` cdecl** — seven dwords, `add esp,0x1c` at `0x4BECF7`. The
sixth argument of the first becomes the sixth of the second (`mov ecx,[esp+0x50]` at `0x4BECD3`,
pushed at `0x4BECE0`) and `[globals+0xC8]` becomes the seventh. Both arms of `0x4BEC70` — the one
that acquires a context through `0x4C5E70` and the one handed a live one — do the same thing;
`0x4BEA20` clips **in place on the caller's own argument slots**, which is why the clipped
coordinates are simply pushed again. The five `lea`s that build its argument list are at
`0x4BECAB`..`0x4BECC3` and read `[esp+0x4c]`, `[esp+0x48]`, `[esp+0x48]`, `[esp+0x48]`,
`[esp+0x14]` — the same displacement three times because each intervening `push` moves `esp`
under it, so against the frame at entry they are `&x0`..`&y1` at `S+0x08`..`S+0x14` plus the ctx
local, i.e. `(&ctx_local, &x0, &y0, &x1, &y1)`. *[CORRECTED 2026-09-21 by the landing review:
this said "`lea` of `[esp+0x48..0x50]`", a range that appears nowhere in the function. The
semantic claim was right and the encoding quoted for it was not — and an encoding is exactly
what the next reader will try to match against the bytes.]*

**Nothing between the caller and the table bounds the row.** `0x4BF7B0` passes its third argument
straight down and `0x4CC8DF` does `shl eax,0x8` on it, so a level outside 0..31 addresses past a
32-row table. `0x4BF4D0` clamps (`<= 0x1F` at `0x4BF595`) and this path does not — **an asymmetry
worth knowing before reimplementing either**, and the reason this fork refuses such an op rather
than reproducing an off-table read.

**The pixel loops are inclusive at both endpoints, like `0x4CC7AB`'s.** `0x4CC944` (dx == 0) does
`sub ecx,eax` / `inc ecx` and `0x4CC983` (dy == 0) the same, so an edge covers both of its corners
— which is what makes the four edges of a focus rectangle overlap at four pixels and tint each of
them **twice**.

**A negative result about addressing, and it is an engine quirk rather than ours.** The two
axis-aligned cases compute the same address from different fields: the horizontal one uses
`mov edx,[edi+8]` — `ctx+0x08`, the **pitch** — and the vertical one `mov edx,[edi]`, `ctx+0x00`,
the **width** (`0x4CC958`). They agree only while pitch == width. Every surface this has been
observed on satisfies that, so it has never been visible; a reimplementation that uses the pitch
for both is doing the right thing and will differ from the engine on any surface where it does
not.

##### `0x4A16F0` draws SIX rectangles, not one: the focus glow [DISASSEMBLED 2026-09-21]

`0x4BF7B0` has exactly **one** caller in the image (`0x4A17EF`, by an exhaustive `E8` scan of
`.text`), and it is inside a loop:

```
4a1799:  mov  edi,0x1f            ; level = 31
4a17a6:  xor  esi,esi             ; i = 0
         ; rect = { x, y, x+w-1, y+h-1 } from gadget +0x13/+0x15 (x,y), +0x17/+0x19 (w,h)
4a17b8:  ...                      ; loop top
4a17c8:  dec edx / dec ecx / inc eax / inc ebx      ; l--, t--, r++, b++ -- expand by one
4a17ef:  call 0x4bf7b0            ; (panel+0xBC, &rect, level)
4a17f4:  mov  eax,0xfffffffd
4a17f9:  sub  eax,esi             ; -3 - i
4a17fb:  add  edi,eax             ; level += -3 - i
4a17fd:  inc  esi
4a17fe:  cmp  esi,0x6 / jl        ; six iterations
```

So the focus highlight is **six concentric tinted rectangles**, each one pixel further out than
the last, at lighten levels **31, 28, 24, 19, 13, 6** — a glow that fades outward over six pixels.
Twenty-four tinted edges per focused gadget per draw. Gadget types 3, 5 and 2 return before the
loop (`0x4A1756`, `0x4A176C`, `0x4A1775`); type 3 writes `0x1E` to `record+0x1F` and draws
nothing.

**Every level it produces is inside the table**, which is why the unbounded row above has never
been reached in practice — but the bound belongs to the consumer, not to the observation.

**`0x4CC7AB`** is `cdecl(ctx, x0, y0, x1, y1, colour)` and it is a **store-only Bresenham**:
`stos byte` with no read of the destination, so nothing here touches the blend LUT. **Its colour
argument is `[ebp+0x1C]`, loaded as a DWORD and stored `stos BYTE al` / `rep stos BYTE al`
(`0x4CC85C`, `0x4CC879`, `0x4CC894`) — the low byte alone**, the same conclusion as `0x4CCDEA`
reached by a different instruction, which is why `0x4BF8C0`'s colour is a palette index and fits
in one `unsigned char` [DISASSEMBLED 2026-09-18 for landing 8b]. Its two
axis-aligned special cases count **both endpoints** — `0x4CC83B` (dx == 0) does
`sub ecx,eax / inc ecx`, `0x4CC866` (dy == 0) the same — so a rect edge is inclusive at every
corner. `ctx+0x08` is the pitch and `ctx+0x0C` the pixel base, the same two fields
`tagpu_markown.c` swaps.

**The general case, which is the rule a redraw of any engine line has to reproduce**
[DISASSEMBLED 2026-09-23 for the Vulkan selection rect]. After the clip:

- **it always walks from the smaller x.** `0x4CC7F1` (`jns` on `x1 − x0`) falls into a swap of
  both endpoint pairs (`0x4CC7F3`..`0x4CC804`) when dx is negative, so the call's endpoint order
  never matters;
- **dy's sign becomes the step direction**: `0x4CC80F`..`0x4CC813` negate dy and the pitch in
  `esi`, which is pushed as the minor step;
- **the major axis is chosen by `cmp ebx,ecx / jle` at `0x4CC81D`** — x-major (`0x4CC880`) when
  `|dy| <= dx`, so an exact 45° line is x-major; y-major (`0x4CC8AC`) otherwise, with the two
  counts exchanged;
- **the error term starts at `2·minor − major`** (`0x4CC82A`..`0x4CC835`: `[ebp-4] = 2·minor`,
  `esi = 2·minor − major`, `[ebp-8] = 2·minor − 2·major`), and **the minor step is taken when it
  is not negative** — `or esi,esi` at `0x4CC898` / `jns` at `0x4CC89A` (x-major) and `0x4CC8C6` /
  `0x4CC8C8` (y-major) — so a
  tie steps. Both loops run `major + 1` pixels (`inc ecx`), inclusive at both ends.

Unrolled: pixel `i` of the walk sits at major offset `i` and minor offset
`(2·minor·i + major) / (2·major)` in integer division — round-half-up of `i·minor/major`, measured
from the smaller-x end. `tagpu_glsl.h`'s `taOnLine` evaluates exactly this for every line the
Vulkan lane draws (G21b: order markers, build sites, selection rects, lasers and lightning, the
nanoframe wire), per pixel of the world target (`ss` to a game pixel), and thickens it to `ss`
pixels across the minor axis; at `ss = 1` it is this walk on game pixels.
`tools/line-band-check.py` checks that closed form against a step-by-step transcription of the
two loops above, pixel for pixel, at thickness 1, 2, 3 and 4 — every shape with one end at
the origin and the other in a box of 24, 12, 10 and 8 pixels respectively — and on long random
lines out to ±16383.

**The clip `0x4CC650` MOVES ENDPOINTS; it does not only reject** [DISASSEMBLED 2026-09-25 for
G21b]. Its fast path (`0x4CC66A`..) accepts the line untouched when all four coordinates are in
`[0,w) x [0,h)` — each compared both signed (`jge`) and unsigned (`jae`), so a negative one fails
too. Otherwise it rejects a line wholly off one side (`0x4CC693` → `xor eax,eax`) and else moves
each end that lies outside onto the edge: the other coordinate advances by
`(distance past the edge) · d(other) / d(this)` in `imul`/`idiv` — **a signed division that
truncates toward zero** — and the moved coordinate becomes `0` or `w − 1` / `h − 1`
(`0x4CC6D0`..`0x4CC798`), then it loops back to the test. The deltas are re-taken from the
current ends on every pass, and the product is the one-operand `imul` (64-bit `edx:eax`). So a
line that crosses the SURFACE edge is walked from the moved end. It is the second of DrawLine's
two clips and acts only at the surface: every DrawLine caller has already been through
`0x4BEA20` (below), whose rect lies inside the surface, so on the world's lines this half moves
an end only where `0x4BEA20`'s single pass left one just past a corner.

**`0x4BEA20` — DrawLine's clip to the context's clip rect, the one that shapes an
edge-crossing line** [DISASSEMBLED 2026-09-25, `objdump` of the pristine exe]. `stdcall(ctx,
&x0, &y0, &x1, &y1)`, `ret 0x14`, clips the four ints in place and returns 1 to draw, 0 not to.
Every call of `0x4CC7AB` in the binary but one helper's four (`0x4CCD34..0x4CCD79`) is
preceded by it — DrawLine `0x4BE950` (`0x4BE989`/`0x4BE9E0`, 83 callers) and the other call
sites from `0x4BEECF` to `0x4C02AB` (`DrawTranspRectangle 0x4BF8C0` among them).
- **The rect** is read by `0x4C6AE0` (thiscall, copies `ctx+0x1C..+0x28` out): L, T, R, B,
  **inclusive on every side** (`x < L` and `x > R` are the tests). In the world's context it is
  the viewport: `DrawGameScreen` stores `main+0x37E27` = {128, 32, W − 1, H − 33} there through
  `0x4C6B10` at `0x468D85`, and at zoom < 1 `vpwide`'s `vpw_setclip` clamps the widened rect
  back to the true viewport and to the surface.
- **The rule.** `xle = x0 ≤ x1`, `yle = y0 ≤ y1` (`0x4BEA3E`/`0x4BEA4F`, `setle`); `dx = x1 − x0`,
  `dy = y1 − y0`, taken once and never updated. One pass: end 0 against `x < L` (`0x4BEA72`),
  `y < T` (`0x4BEAAD`), `x > R` (`0x4BEAE7`), `y > B` (`0x4BEB23`), then end 1 against the same
  four (`0x4BEB5E`, `0x4BEBA1`, `0x4BEBDC`, `0x4BEC11`). An end off a side the line runs away from
  — end 0 left of L with `!xle`, end 1 left of L with `xle`, and so on — or a zero delta on that
  axis returns 0. Otherwise the other coordinate moves by `(edge − end) · d(other) / d(this)`,
  two-operand `imul` then `cdq`/`idiv` (**truncating toward zero**), and the end lands on the edge.
  There is no second pass, so an end can be left just past a corner: 33 of 149 282 accepted
  random lines in a scratch run.
- **Its pixels.** A line crossing the viewport edge is walked from the moved end, and that walk
  differs from the unclipped one along the whole visible part — ten pixels a line on average over
  50 000 random short edge-crossing lines. The Vulkan lane reproduces both clips in integers
  (`tagpu_line.h` `tagpu_line_clip`), on the world target's pixels against this rect scaled by
  `ss` — at `ss = 1` exactly this routine's inputs — and `tools/line-band-check.py` checks it against a Python
  transcription of this entry and of `0x4CC650` above on 90 000 lines, same verdict and ends on
  all. Measured against the engine's own frame at 1x: on the one changed line of an edge view,
  the clipped walk's 5 pixels of its own are all the engine's, the unclipped walk's 4 none.

**THE NEGATIVE RESULT, and it is the load-bearing one.** `0x4CC7AB` opens by calling
**`0x4CC650`**, which reads `edi = [ctx+0x00]` and `esi = [ctx+0x04]` — the surface's WIDTH and
HEIGHT — and clips the line to `[0,w) x [0,h)`, returning 0 for a line wholly outside; the
caller then `je`s past the draw (`0x4CC7DD`). **The bound is the surface's own dimensions, read
below the clip rect at `+0x1C..+0x28`, so nothing done to the clip rect can widen it.** The
offscreen is screen-sized, and at zoom < 1 the rect above is projected at coordinates
`vpwide` made addressable — which run far past it. Measured on a 1024x768 frame at 0.467x:
with the pointer at screen (880,400) the engine's own mouse point is (1228,418), the six rect
globals are populated and `main[0x2CC3]` is 14, and **not one pixel is drawn**; at screen
(170,250) the engine point is (-294,97), same result. A capture window can therefore never
carry this rect into the outer ring, however wide its own buffer is — which is why it is
re-drawn instead (`tagpu_mark.c`, "THE BUILD CURSOR").

**And it NORMALISES BEFORE IT INSETS, which is not interchangeable.** `0x469E92`
(`cmp edi,esi / jge`) and `0x469EA0` (`cmp edx,eax / jge`) swap the x and y pairs, the swapped
values are what gets stored, and only then does `0x469ECA..0x469EDD` read them back and
`inc edi / inc esi / dec edx / dec ecx` for the inner rect. Insetting first and normalising
afterwards turns the inset into an OUTSET for any rect stored right-to-left or bottom-to-top —
a band box dragged up or left — where the inner outline then lands one pixel *outside* the
outer one. Ours had exactly that bug until the G13o landing review; the G13n A/B that passed
"0 differing pixels" had only ever dragged down-right. [FROM REVIEW, then confirmed against the
disassembly and re-measured: an up-left drag now diffs to 0 px against the engine's own.]

## The order-marker chain — mapped by us

[MEASURED 2026-09-04, this project — `objdump -d -M intel` of the pristine Steam build
(md5 `8e74a1dffa1f5988624c52048f5b20cd`), every function below read instruction by
instruction for this landing, plus a live 16 650-record trace against the engine's own
drawer calls. This is the block `tagpu_order.c` now re-draws instead of capturing;
`ui-markers.md` §3 is the same chain written from the marker's point of view.]

The shift-held overlay is one driver, one list walker and five leaf drawers, reached from
a single call site inside `DrawGameScreen`.

```
0x4699EB  call 0x46A530                             selection rect, ground sweep
0x469B8A  call 0x46A530                             selection rect, air sweep
0x469BD7  hook 8                                    (was capture window A; G13p
                                                     retired it — the hook now only
                                                     brackets our order arena's block
                                                     and latches the text font)
0x469BE1  KeyboardHotkeySampler(0xF9)  -> SHIFT?    (0x469BE8 je 0x469C01)
0x469BFC  call 0x48CC30(ctx, main+0x142F3)          <- the driver, SHIFT-gated
0x469C03  je 0x469D38                               (drawUnits == 0 exits past hook 9)
0x469CB9  call 0x46A430                             health bars, AFTER the markers
0x469CF9  call 0x4C14F0                             group digits (G13p: SKIPPED)
0x469D2C  hook 9
0x469EC5  call 0x4BF8C0                             build cursor / band box, outer
0x469F1E  call 0x4BF8C0                             ...and the inner rect, after fog
```

### `0x48CC30` — the driver, and the three selection rules

`stdcall(OFFSCREEN* ctx, void* viewStruct)`, `ret 8` @ `0x48CD70`, sole caller `0x469BFC`.
Five register pushes, so `ctx` is `[esp+0x18]` and `viewStruct` `[esp+0x1c]` inside.

It resolves three units before the loop, and each is `0` when its id is `0`:

| What | Where | Read at |
| --- | --- | --- |
| `CameraToUnit` | `viewStruct+0` (`main+0x142F3`) | `0x48CCB0` |
| tracked unit | `units(main+0x14357) + main[0x37E9C]·0x118` | `0x48CC58..0x48CC81` |
| hovered unit | `units + main[0x2CBA]·0x118` | `0x48CC84..0x48CCA9` |

`bl` is set at `0x48CCEE` iff ANY of those three has a non-null `UnitDef+0x156`
(`CANBUILD_ptr`) — the "hover a constructor and see everyone's claimed build sites" rule.
Note the three tests at `0x48CCB6/0x48CCCA/0x48CCDE` dereference `unit+0x92` without a null
check; ours adds one.

The walk is over the WATCHED player's own unit range —
`PlayerStruct = main+0x1B63 + main[0x2A42]·0x14B` (`0x48CC3B..0x48CC51` builds the stride
without a multiply, as `cl + ((cl·33)·5)·2` = `cl·331`), first `+0x67`, last `+0x6B`, step
`0x118`, bounds compared **unsigned**
(`ja` @ `0x48CCFC`, `jbe` @ `0x48CD69`) and the end pointer **re-read every iteration**.

```
st = unit[0x110]
if (!(st & 0x10000000)) continue                    alive        0x48CD08
if (st & 0x4000)        continue                    excluded     0x48CD10 (test ch,0x40)
if (unit == CameraToUnit)                mask=0x1F flag=1        0x48CD15
else if (unit[0xA8] == main[0x37E9C])    mask=0x1F flag=1        0x48CD20
else if (unit[0xA8] == main[0x2CBA])     mask=0x1F flag=1        0x48CD29
else if (st & 0x10)                      mask=0x1F flag=0        0x48CD32 (shr ecx,4; test cl,1)
else if (bl)                             mask=0x01 flag=1        0x48CD3E
else                                     continue
0x439B30(unit, mask, ctx, viewStruct, flag)                      0x48CD51
```

The two id comparisons are made **without** first checking the id is non-zero, so a unit
whose `UnitInGameIndex` is 0 matches a tracked/hovered id of 0. Reproduced as-is.

### `0x439B30` — the walker, its dispatch table, and where `pos` is restored

`ret 0x14`, args `(unit, mask, ctx, view, flag)`. `sub esp,0xC` then four pushes: the three
dwords at `[esp+0x10..0x18]` are `pos`, seeded from the unit's own 16.16 triple at `+0x6A`
(`0x439B40..0x439B62`), and **the `unit` argument slot `[esp+0x20]` is reused as the
range-circle guard byte** (cleared at `0x439B47`, set at `0x439CC3`).

Per node — head `unit+0x5C`, next `node+0x4A` (`0x439CC8`) — the order type byte `node+0x4`
indexes the descriptor array behind `*(u32*)0x512344`:

```
eax = node[0x4];  eax *= 5;  edx = base + eax*4;  mask = [eax + edx + 0xC]
                                     i.e.  base + type*0x19 + 0xC
```
recomputed from scratch ahead of every one of the five tests (`0x439B6C`, `0x439BB1`,
`0x439BF7`, `0x439C3C`, `0x439C82`), ANDed with the caller's mask each time.

| bit | drawer | call site |
| --- | --- | --- |
| 0 | `0x438C00` build-site footprint rect | `0x439BAC` |
| 1 | `0x4394E0` route dots (delegates to bit 3 first, unconditionally) | `0x439BF2` |
| 2 | `0x4399F0` circle around the order target | `0x439C37` |
| 3 | `0x439740` animated target sprite | `0x439C7D` |
| 4 | `0x4390A0` per-unit range circles, behind the guard byte | `0x439CBE` |

**`pos` IS RESTORED TO THE NODE'S ENTRY VALUE BEFORE BITS 0..3 AND NOT BEFORE BIT 4.** The
walker keeps a register copy of the node-entry position in `edi`/`ebp`/`ebx` (reloaded from
`pos` at `0x439CCB..0x439CD3`, i.e. at the END of each node) and writes it back into `pos`
with the three stores `mov [esp+0x24],edi` / `[esp+0x28],ebp` / `[esp+0x2c],ebx` immediately
before the calls at `0x439BAC`, `0x439BF2`, `0x439C37` and `0x439C7D`. There is **no such
triple ahead of `0x439CBE`**. So every one of bits 0..3 starts from the same point, the chain
advances by whichever of them ran LAST, and the range-circle drawer sees whatever they left.
[This is the fact the trace was built to check, and it holds: 16 650 records, zero
disagreements — `git log` "Three things the live bring-up found".]

### `0x512344` — the order-descriptor array

`0x512344` begin, `0x512348` end, `0x51234C` capacity: a vector of **25-byte** records, all
three zeroed by the constructor at `0x438450` and freed at `0x438480` through `0x4B4F20`. A
lookup by name walks it with `_stricmp 0x4F8A70`, case-insensitive, against the pointer at `record+0x15`
(`0x4387A9`, `0x4387E4`), which is what fixes both the stride and that last field.

**How it is filled** [DISASSEMBLED 2026-09-29]. `0x43C050`, called once from `0x491200`
(`0x49136E`), appends the record at `0x4FD288` (empty name, status "Ready", handler `0x439EA0`,
which returns 5) and then runs the three registrations `0x43BC90(src, count)`, in this order
(`0x43C325..0x43C334`): `0x4FC6E8` (22 records, called at `0x406BF7`), `0x4FCA18` (22,
`0x415B27`) and `0x4FC490` (23, `0x403187`). After every append the whole vector is re-sorted by
name with the comparator `0x43C020`, `_stricmp(a+0x15, b+0x15) < 0`. The 68 records and their case-folded names are fixed for the
process, so **an order's type byte `+4` is its record's rank in that sorted vector**, not its
position in `.rdata`: `BuildingBuild` is `0x0C` only because the compare folds case (under a
case-sensitive compare `BuildWeapon` would come first). The engine reads only these heap copies;
rewriting a handler dword in `.rdata` after `0x43C050` has run changes nothing.

| Offset | What | Read at |
| --- | --- | --- |
| `+0x00` | `char*` status text ("Ready", "Nanolathing", "Repairing"…); `0x439DF0` returns it, record 0 when a unit has no order | `0x439E13` |
| `+0x04` | the order's handler, `stdcall(unit, order, events)`, `ret 0xC` — called only from the three dispatch sites (*The nano spray* in the replay section) | `0x43A21A`, `0x43B87C`, `0x43BB21` |
| `+0x08` | the target-sprite drawer's own address, **never read in this build** | — |
| `+0x0C` | u32 marker-capability mask (bits per the table above) | `0x439B7D` etc. |
| `+0x10` | u8 `cursor_ary` index; 0 = this order type draws no sprite | `0x4397F3`, `0x439965` |
| `+0x11` | u32 flags, copied into a new node's `+0x42` (`0x43A18F`); bit `0x40000` puts the order on the sub list `+0x60`, else the main list `+0x5C` (`0x43AC6A`, `0x43ACBA`, `0x43AD19`) | `0x43A17C`, `0x438B9A`, `0x439E34`, `0x43B0C0`, `0x43B149` |
| `+0x14` | the flags' top byte, read alone | `0x43A56D` |
| `+0x15` | `char*` name, for the console lookup, the saved game's `<key>_name` and the sort | `0x4387A9` |

The engine indexes this with the raw type byte and no bound at all, here and at the three
handler dispatches. With a live end pointer in `0x512348` the bound costs two loads, so ours
takes it.

### `0x438C00` — the build-site footprint rect

**`node+0x16` TELLS AN EMPTY SITE FROM ONE ALREADY UNDER CONSTRUCTION** [MEASURED LIVE
2026-09-21]. It is the order node's ordinary target-unit field — the one `0x4399F0`'s prologue
reads for the target circle and the sprite resolver reads for the last-seen cache — and on a
BUILD node it is NULL while the site is still empty ground and holds the created unit from the
moment construction begins. The node itself survives the whole build, which is why the engine
goes on drawing a footprint rect over a nanoframe: `0x438C00` gates on `node+0x36` alone and
never asks this. Established behaviourally, not from the writer: with the build ghost skipping
every entry whose `+0x16` resolves, the site being nanolathed lost its ghost on the frame the
frame appeared while the next site in the queue kept its own, over a three-site queue, and the
pass's `built=` counter tracked exactly the sites with a frame standing on them. **The store
itself was not disassembled** — what is established is the field's state before and after the
transition, which is what the consumer needs. The build ghost's `TAGPU_PK_BUILD.started` is
this field, resolved on the game thread by `tagpu_order.c`'s walk.

`ret 0x14`. Nothing at all unless `node+0x36` (the build target's unit type id, u16) is
non-zero — the early exit at `0x438C0E` skips the `pos` chaining as well as the draw.
`def = UnitDefs(main+0x1439B) + type·0x249` (`0x438C1D..0x438C38`, the multiply written as
`(type·65)·9`).

```
P0 = (t.x + def[0x15E],  t.y + def[0x162],  t.z + def[0x166])     t = node+0x22 (16.16)
P1 = (t.x + def[0x16A],   --                t.z + def[0x172])     def[0x16E] is never read
alt = (s16)(P0.y >> 16)
x0  = (s16)(P0.x>>16) - view[0x2C] + 0x80        x1 likewise from P1.x
z0  = (s16)(P0.z>>16) - (alt>>1) - view[0x30] + 0x20      z1 likewise from P1.z
```

Then the ten-tick animation (`0x438CB7..0x438D3D`), and **it sweeps the four edges INWARD,
it does not grow an inner rect** — `ui-markers.md` said "grows" and that was wrong:

```
t   = gameTime(main+0x38A47) - node[0x46]
t   = ((unsigned)t >= 10) ? 10 : t        <- UNSIGNED: a negative age reads as FINISHED
dxg = (x1-x0)*t/10        dzg = (z1-z0)*t/10        both /10 by the 0x66666667 magic
xg0 = x0+dxg   xg1 = x1-dxg   zg0 = z0+dzg   zg1 = z1-dzg
```

At `t=0` the animated positions sit on the rect's own edges; at `t=10` they have swapped to
the opposite ones exactly — `xg0 = x1` and `xg1 = x0` — so the colour-B lines land ON those
edges and the colour-A pair, drawn one pixel outside each animated position, lands one pixel
inside them. Colour pair from the ISSUING unit `node+0xE`'s
`stateMask & 0x10` (`0x438D41..0x438D85`): selected → `main[0xDCE]`(gui 3) + `main[0xDD5]`
(gui 0xA), not → `main[0xDCC]`(gui 1) + `main[0xDD4]`(gui 9).

Eight `DrawLine 0x4BE950` calls, four in colour A one pixel outside the animated positions
and spanning one pixel past the corners, four in colour B exactly on them
(`0x438DAA`, `0x438DCE`, `0x438DF5`, `0x438E18`, `0x438E34`, `0x438E47`, `0x438E5E`,
`0x438E6D`). Finally `pos ← node+0x22..0x2A` (`0x438E74..0x438E8B`).

In draw order [DISASSEMBLED 2026-09-25 for G21b], with every coordinate an integer:

```
colour A   (xg0-1, z0-1) -> (xg0-1, z1+1)      (xg1+1, z0-1) -> (xg1+1, z1+1)
           (x0-1, zg0-1) -> (x1+1, zg0-1)      (x0-1, zg1+1) -> (x1+1, zg1+1)
colour B   (xg0, z0) -> (xg0, z1)              (xg1, z0) -> (xg1, z1)
           (x0, zg0) -> (x1, zg0)              (x0, zg1) -> (x1, zg1)
```

`alt >> 1` is a `sar`, so a negative altitude halves toward minus infinity, and the `/10` is the
`0x66666667` multiply with the sign correction, a division that truncates toward zero.
`tagpu_order.c`'s `draw_build` reproduces all of it in integers and hands the eight lines on as
integer endpoints (`tagpu_line.h`: the engine pixel's centre, then the zoom, then `ss`). The
colour-A lines are the one departure, and only at a wheel zoom: they are handed on as the
colour-B pixels `(xg0, z0)` … with their `±1` taken AFTER the zoom, one game pixel of the zoomed
frame (`tagpu_line_nudge`), so that the pair stays adjacent at every zoom; at 1× that is the
engine's own `±1` (GPU status §2.93).

**`DrawLine 0x4BE950` is `stdcall(ctx, x0, y0, x1, y1, colour)`** — fixed by those eight
call sites, where the first and third pushed values are the two x's.

### `0x485A40` — the spawn's rotation words, and `BuildAngle`

[MEASURED 2026-09-21, this project — `objdump -d -M intel` of the pristine build, plus a live
A/B in the game.] **A newly created unit does NOT face `0x0000`, and it does not face a fixed
direction either.** `0x485A40` is the spawn initialiser, `ret 0x14` (five arguments, the unit
first). It takes the type at `unit+0xA6`, resolves `def = UnitDefs(main+0x1439B) + type·0x249`
with the same `(type·65)·9` multiply `0x438C1D` uses (`0x485A4B..0x485A68`), stores it at
`unit+0x92`, ORs the alive bit `0x10000000` into `unit+0x110` — and then writes the rotation
triple at `unit+0x64`:

```
0x485B92   pitch  unit+0x68 = 0                 (bx, zeroed at 0x485A49 and never reloaded)
0x485C02   bank   unit+0x64 = 0                 (same register)
0x485BD6   ax     = def[0x210]                  BuildAngle
0x485BDE   call 0x4B6C30(ax)                    the sim RNG: seed%n, 0 for n < 2
0x485BF0   ecx    = 0x8000 - def[0x210] / 2     (shr dx,1 / sub)
0x485C00   eax    = rand + ecx
0x485C06   head   unit+0x66 = ax
```

so

> **`unit+0x66 = 0x8000 − BuildAngle/2 + rand(BuildAngle)`, and bank and pitch are 0.**

`rand` is the shared sim PRNG `0x4B6C30` (Park–Miller, state `ds:0x51FC88`, §"The COB engine"),
which is why the value is not reproducible from outside the simulation and why nothing outside
it may draw from that seed.

**Its three call sites are the complete set** — `0x485EF8`, `0x4860A0`, `0x4862B8`, all direct,
and the address is never taken anywhere in the image — so every unit that exists was rotated
here, whatever created it. (A scenario applier that writes `+0x66` afterwards, as ours does,
therefore overwrites this; `tacli`'s `facing` degrees are measured from `0x8000`, which is why
that is the zero in `scenario-format.md`.)

**`UnitDef+0x210` is the FBI tag `BuildAngle`**, parsed at `0x42C56D`: the FBI reader pushes a
tag name and calls `0x4C46C0`, and each store lands one instruction group AFTER the call whose
value it keeps — `0x503C00` `buildangle` → `+0x210`, with `0x503C0C` `mincloakdistance` →
`+0x208` and `0x503BF0` `builddistance` → `+0x212` on either side of it as the check that the
pairing is not off by one. Add it to `UnitDefStruct`'s field list: `0x210` u16 `BuildAngle`.

**What stock content puts there** [measured from the reference install's archives, 2026-09-21]:
105 of the 278 unique unit `.fbi` files set the tag at all, and the values cluster by class:

| value | ± | n | who |
|---|---|---|---|
| `0` | 0° | 2 | `armfort` `corfort` |
| `1024` | 2.8° | 8 | the plants and labs (`armvp` `armasp` `armavp` `coralab` `corasp` `coravp` `corlab`) and `corwin` |
| `2048` | 5.6° | 8 | `armalab` `corvp`, both gates, both geothermals, both mohos, `cormex` |
| `4096` | 11.25° | 17 | `armsolar` `armestor` `armmstor` `armlab`, the fusions, the big defences (`armanni` `cordoom` `armbrtha` `corhlt`), both targeting facilities |
| `8192` | 22.5° | 40 | `armmex` `armwin`, the metal makers, radars, sonars, silos, dragon's teeth, the underwater set |
| `8196` | 22.5° | 1 | `corestor` — four more than its neighbours, and nothing turns on it |
| `16384` | 45° | 24 | the naval buildings and the big ships |
| `29096` | 79.9° | 2 | `armvulc` `corbuzz` |
| `32768` | 90° | 3 | `corsolar` `armllt` `corllt` |

The 173 files with no tag get `0`, hence `rand(0) = 0` and an exact `0x8000`.

**Why this was chased**: the build ghost drew its preview at the model's rest orientation,
which is heading `0x0000` — half a turn from every building the engine places. It now draws at
`0x8000` and the per-instance draw above is a named residual, not a bug
([gpu-status](../gpu-status.html) §2.23).

### `0x4394E0` — the route dots

`ret 0x14`. Saves `pos` into locals, calls `0x439740` with the SAME `pos` pointer
(`0x439516`) so the sprite's resolved position becomes the segment's far end, then returns
at once unless `flag == 1` (`0x43951D`) — a merely *selected* unit gets no route line.

```
len   = (int)sqrt(dx² + dy² + dz²)      dx,dy,dz = pos - saved, 16.16   0x43956A..0x43957C
if (len < 0x10000) return                                               0x439587
age   = gameTime - node[0x46]
phase = ((age % 30) * 0x300000) / 30            48.0 world units per 30 ticks
seq   = *(main+0x148D3)                          the `pathicon` GAF sequence
period= (u16)seq[0x2C] ? that : 1                = frametab[0]'s duration dword
frame = (age / period) % (u16)seq[0]
for (cursor = phase; cursor < len; cursor += 0x300000) {
    p = saved + delta * ((int64)cursor << 16) / len >> 16      __alldiv/__allmul/__allshr
    CopyGafToContext 0x4B7F90(ctx, seq[0x28 + frame*8], px, pz)
    frame = (frame + 1) % nframes
}
```

`seq[0x2C]` is not a field of its own: `TAGPU_SQ_TAB` is `0x28` with stride 8, so it is the
u32 half of frame 0's table entry.

### `0x4399F0` — the circle around the order target

`ret 0x14`. Centre and radius: with `node+0x16` non-zero, the target unit's own 16.16 triple
and `(s16)def[0x178]`; with a ground target, `node+0x22..0x2A` and a flat `0x20`
(`0x4399F7..0x439A4C`). The y radius is `(int)(R * 0.89)` — the double at **`0x4FD2C0`**.
Sixteen chords, angle `0x1000` to `0x10000` step `0x1000`, vertices
`(cx + TurnZLookup 0x4B7123(angle,R), cz + TurnXLookup 0x4B70EF(angle,R2))`, colour
`main[0xDD7]` (gui 0xC). Both lookups are **cdecl** (`add esp,8` after each). `pos ← centre`.

### `0x439740` — the target sprite, and the one sim write in the block

`ret 0x14`. Resolves the position first:

```
tgt = node[0x16]
if (!tgt)                       p = node[0x22..0x2A]              ground target
else if (UnitInPlayerLOS 0x465AC0(node[0xE][0x96], tgt))          stdcall(player, unit), ret 8
                                p = tgt[0x6A..0x72]
                                node[0x32] = (s16)(p.x>>16)       <- WRITES THE CACHE
                                node[0x34] = (s16)(p.z>>16)
                                node[0x42] |= 0x200000
else if (node[0x42] & 0x200000) p = ((s16)node[0x32]<<16, tgt[0x6E], (s16)node[0x34]<<16)
else                            the live branch above
```

That cache write is the only thing in the whole marker block that touches sim-side state,
and it is what stops a waypoint marker following a target the player can no longer see. A
port that drops it leaks the target's live position; ours reproduces it on the game thread
at the same instant (`tagpu_order.c`, `resolve_sprite`).

Then, only if the descriptor's `+0x10` cursor index is non-zero (tested at `0x4397F7`, with
`0x4397F9` the branch past the early exit that otherwise chains `pos` and returns — so a zero
index skips the ShowRanges limb below as well as the sprite), and reaching its own sequence
lookup at `0x439952`, frame `(gameTime / (2·period)) % nframes` of
`cursor_ary[idx]` = `*(main+0x1487F + idx*4)` is alpha-blitted through
`AlphaCompsteBuf2OFFScreen 0x4B8500` (`0x4399BB`). `pos ← p`.

`0x439811..0x439948` is a `ShowRanges`-only limb, and it draws CIRCLES as well as labels: for
descriptor cursor index 1 or 2 it emits, through `0x438EA0` and **at the resolved target
position**, each live weapon's AoE (`w+0xD6`) and `coverage` (`w+0xE0`, stored at `0x42E540` from the
`coverage` key; the label is "weapon %d - coverage") plus
`def[0x216]`, in the same `gameTime & 1` flash colour. Its weapon-slot flags are the regular
`0x1F + i·0x1C` (`0x439879`), so this drawer does **not** carry `0x4390A0`'s third-slot quirk
— the two disagree about the same question in the same build.

Its labels are **formatted at draw time**, through the engine's own `sprintf 0x4E42B0`
(called at `0x43989B` and `0x4398DE`):
`"weapon %d - area of effect"` (`0x5051C4`, at label slot 0) and `"weapon %d - coverage"`
(`0x5051AC`, slot 1), both taking the weapon INDEX 0..2, plus the literal `"attack length"`
(`0x50519C`, slot 2) for `def[0x216]`. [BINARY-VERIFIED 2026-09-05]

### `0x4390A0` — the per-unit range circles

`ret 0x14`. `unit = node+0xE`, `def = unit[0x92]`. With `ShowRanges` (`main+0x391BF`) clear:

```
if (def[0x208] && (unit[0x10E] & 4))                                 cloaked
    DrawRangeCircle(ctx, view, unit+0x6A, (s16)def[0x208], main[0xDDA], 0, 0)
if (!(def[0x241] & 0x10000000)) return                               not kamikaze
w = def[0x220]; if (!w) return                                       ExplodeAs
aoe = (u16)w[0xD6] >> 1
r   = ((gameTime % 60) * aoe * 2) / 60      unsigned throughout      0x43913E..0x43915E
r   = max(8, min(r, aoe))
DrawRangeCircle(..., r,                       main[0xDD7], 0, 0)
DrawRangeCircle(..., unit[0x0] ? (u16)def[0x218] : (s16)def[0x202], main[0xDD7], 0, 0)
```

With `ShowRanges` set it draws a labelled set of NINE instead, and the first of them is
`def[0x208]` — the cloak radius — at `0x43921A`, gated **only on the value being non-zero and
not on the unit's cloak flag**, which is the normal branch's rule and not this one. Then
`def[0x202]` sight, `+0x204` radar,
`+0x206` sonar, `+0x20A` radar jam, `+0x20C` sonar jam, `+0x212` builddistance, `+0x214`
maneuver, `+0x218` kamikazedistance, all in `main[0xDD9]` (gui 0xE) with label strings at
`0x505190/88/80/78/6C/60/50/44` and `0x503A0C` — `"mincloak"`, `"sight"`, `"radar"`,
`"sonar"`, `"radarjam"`, `"sonarjam"`, `"build distance"`, `"maneuver"`,
`"kamikazedistance"`; then the three weapon ranges (`unit+0x10`,
`+0x2C`, `+0x48`, each `+0xDC`) flashing `main[0xDCF]`/`main[0xDD7]` on `gameTime & 1`, the
first gated at `0x439443`, labelled `"weapon1 range"` `0x505134`, `"weapon2 range"`
`0x505124`, `"weapon3 range"` `0x505114`.

**The `labelSlot` each one passes** [BINARY-VERIFIED 2026-09-05]: for the nine it is `esi`,
a running count of the circles actually drawn — 0 for the cloak radius (a literal, with
`mov esi,1` after it at `0x439236`), then `mov eax,esi; inc esi` for each of the next seven,
and a bare `push esi` for kamikazedistance because it is the last. For the three weapon
ranges it is a **literal 0, 1, 2** (`0x439457`, `0x439485`, `0x4394B0`), so a unit with all
twelve draws two labels at slot 0, two at 1 and two at 2.

**Those nine radii are read with MIXED sign, and the mix is not tidy** — worth having written
down, because a blanket cast either way is a guess: `movsx` for cloak `0x439229`, sight
`0x439267`, radar `0x4392A1`, sonar `0x4392DB`, radar-jam `0x439315` and sonar-jam `0x43934F`
(each after a `mov dx,WORD PTR [eax+…]` at `0x43924A`, `0x439284`, `0x4392BE`, `0x4392F8`,
`0x439332`), and `and 0xffff` for builddistance `0x43937A`, maneuver `0x4393B7` and
kamikazedistance `0x439404`. Inert for any value below 32768, which every stock one is.

**An engine quirk worth knowing before it looks like a bug in a port:** weapon 1 is gated on
`unit[0x1F] & 2` and weapon 2 on `unit[0x3B] & 2` (= `0x1F + 0x1C`), but weapon 3 is gated on
`unit[0x1F] & 2` **again** at `0x43949D`, where `0x1F + 0x38 = 0x57` was meant. Reproduced
rather than corrected — a "fix" would draw a circle the engine never draws.

### `DrawRangeCircle 0x438EA0` — the terrain-following circle

`ret 0x1C`: `(ctx, view, POS16_16* centre, radius, colour, char* label, labelSlot)`. A zero
radius returns at once (`cmp ebx,ebp` at `0x438EAC`, `je 0x43908F` at `0x438EAF`). The
segment count is `(int)(radius · 2π · 0.125)` — the doubles at **`0x4FD2B0`** (2π) and
**`0x4FD2B8`** (0.125) — i.e. one segment per 8 world units of circumference, and the loop
runs `i = 0..N` inclusive, stepping `0x10000/N` in angle units per segment.

**It is a ROUND circle — there is no isometric squash here** [BINARY-VERIFIED 2026-09-05].
`ebx` is reloaded with `radius<<16` at the top of every iteration (`0x438F08` from
`[esp+0x18]`) and handed to **both** `TurnXLookup` and `TurnZLookup`. The 0.89 at `0x4FD2C0`
belongs to the TARGET circle `0x4399F0`, which multiplies **only** its y radius by it
(`[esp+0x18]` there, against the unsquashed `[esp+0x34]` on x). G13o applied the squash to
both drawers and drew every range circle 11 % flat until G13p; the projection maps world z to
screen y 1:1, so a round circle in world space is a round circle on screen.

**And stock divides by zero there — patched.** `mov eax,0x10000` at `0x438EE4`, `cdq`, then
**`idiv ecx` at `0x438EEE`** with `ecx` = N, guarded only by `jl` against a *negative* N
(`0x438EDE`). A radius of 1 gives `(int)(1 · 0.7854) == 0` and faults inside TA. Nothing in stock
content is that small. `fix_range_circle` sends N = 0 to the radius-0 epilogue `0x43908F` — see
[A range circle of radius 1](#a-range-circle-of-radius-1-0x438ede-disassembled-2026-09-25).

**`TurnXLookup 0x4B70EF` is a SINE and `TurnZLookup 0x4B7123` a COSINE**, off one shared
table at **`0x509F00`**: 512 `s16` entries, `8192 = 1.0` (`shrd …,0xD` after a `+0x1000`
round), indexed `((angle + 0x20) >> 6) & 0x3FE`, and TurnZ adds a quarter turn (`+0x4020`)
before the same shift; the table is `round(8192 · sin(2πi / 512))` on all 512 entries, none
within 0.0016 of a rounding tie (MEASURED 2026-10-08 against `pristine/TotalA.exe.pristine`; the
demo recorder's unit and pool frames carry it as a literal of that formula). Both are **cdecl** —
the callers `add esp,8`; `0x4B70EF` has 67 call sites in `.text` and `0x4B7123` 69 (`objdump -d`,
2026-10-08), among them the projectile tick's guided velocity `0x49BA7D`, `0x49BA91`, `0x49BAA1`
and `0x49BAB4` (*The demo recorder's pool frame*, above). So the engine's
parameterisation is `p.x = centre.x + r·sin(a)`, `p.z = centre.z + r·cos(a)`, which is the
mirror of the usual cos/sin pair: on a round circle it is the same circle entered a quarter
turn along, and it matters only when you want a point on it at the engine's own angle.

Each endpoint is `centre ± TurnX/TurnZLookup(angle, radius<<16)` and then, and this is what
makes these circles hug the ground, its altitude is raised:

```
p.y_hi = max((s16)centre[+6], GetPosHeight 0x485070(&p))            0x438F47, 0x438F5C
```

before the standard `- alt/2 - eyeY + 0x20` projection. `GetPosHeight` is
`stdcall(POS16_16*)`, `ret 4`, and reads only the two high words `[p+2]` and `[p+0xA]`; it is
a pure bilinear read of the height grid at `main+0x14287` with dims `main+0x14233` /
`main+0x14237`, no writes and no globals of its own, which is why our render thread may call
it directly.

**The label's anchor, exactly** [BINARY-VERIFIED 2026-09-05]. `labelSlot` is multiplied by
three once, before the loop (`lea eax,[eax+eax*2]` at `0x438EFF`), and the loop remembers
`(x1, y1)` — the **second** endpoint, at angle `(i+1)·step` — of the iteration whose index
equals it (`0x43902E`). So the anchor is the point at angle `(labelSlot·3 + 1)·(0x10000/N)`.
If nothing matched (`labelSlot·3 > N`) both remembered values are still 0 and `0x43906D`
falls back to the LAST endpoint computed, i.e. `(N+1)·step`. The string is then
`DrawTextCustomFont 0x4C14F0(ctx, str, x, y + 4, -1)` — `add edx,0x4` at `0x43907D` — in
whatever colour `SetTextColors` last set, **not** the circle's.

### `0x469CD1..0x469CF9` — the group digit

[BINARY-VERIFIED 2026-09-05. The block `tagpu_mark.c` now draws.]

```
0x469C4A  if (!(main[0x37F06] & 1) && *(u32*)(unit+0xAC) == 0) continue;
0x469C63  sx = (s16)unit[0x6C] - main[0x1431F] + 0x80
          sy = (s16)unit[0x74] - main[0x14323] - ((s16)unit[0x70] >> 1) + 0x20
0x469C6B  buf[1] = 0                                   the string's terminator
0x469C97  if (!(main[0x37F06] & 1)) continue;           "damagebars" gates the WHOLE unit
0x469CB9  if (unit[0x96]->id == p) DrawHealthBars(ctx, unit, sx, sy + 0x0A)
0x469CD1  if (unit[0x96]->id == p && *(u32*)(unit+0xAC))
0x469CF9      DrawTextCustomFont(ctx, {'0' + (u8)unit[0xAC], 0}, sx, sy + 0x0E, -1)
```

**`p` is `main+0x2A43`, and it is NOT the byte the order driver uses**
[BINARY-VERIFIED 2026-09-05]. `DrawGameScreen` loads it into a local at
`0x46967D` (`mov al,[edx+0x2a43]`) and stores it at `0x469689`; both the bar
(`0x469CA6`) and the digit (`0x469CC9`) compare `[[unit+0x96]+0x146]` against
that `[esp+0x70]`. The order-marker driver `0x48CC30`, in the same block, picks
its player range from **`main+0x2A42`** instead (`0x48CC3B`). The pair is written
independently — `0x416B25` and `0x416B38`, from two separate calls in one loader
function — so the two can hold different values, and code that reproduces either
loop has to use the byte that loop uses.

**`+0x2A42` is the controlled player and `+0x2A43` the viewed one** [DISASSEMBLED 2026-09-25]. The
two writers are cheat commands: `+Control N` (`0x416AB0`, run level 4, entry `0x501FDC` of the
table `0x501FD0`) writes N to both bytes (`0x416B25`, `0x416B38`), and `+View N` (`0x416B50`, run
level 2, entry `0x501F60` of the table `0x501F48`) writes only
`+0x2A43` (`0x416BC1`). The LOS bit test at `0x43EBF4` shifts by `+0x2A43` (`0x43EBE5`). So `+0x2A42`
is the player whose units the order driver and the selection use, and `+0x2A43` the player whose line
of sight, fog and bars the screen shows. TADR names them the same way (`tamem.h`:
`LocalHumanPlayer_PlayerID`, `LOS_Sight_PlayerID`). They are equal in ordinary play [INFERRED]. Some
notes and code comments still call `+0x2A43` "local" and `+0x2A42` "watched"; the addresses in the
code are right, only those names are wrong.

Two further things are not obvious and both matter to a port. **The squad tag is
tested as a DWORD** — `mov ecx,[edi+0xac]; test ecx,ecx` at both `0x469C55` and `0x469CD1` — and only
then used as a byte, so a unit whose `0xAD..0xAF` are non-zero draws a `'0'`. And **the digit
sits four rows below the bar**, `sy + 0x0E` against the bar's `sy + 0x0A`. There is no health
test on the digit: `DrawHealthBars` returns early on a dead unit, but the digit is drawn from
the caller and does not care.

## The unit blit's shadow branches — mapped by us

[MEASURED 2026-09-03, this project — `objdump` of the pristine Steam build, plus the live A/B in
`shadows-cloak.md` §"Structure shadows, owned". Established while fixing the teal structure
shadows (G13k): the engine's cached slant shadow is drawn through the ALP blend blit and,
inside a key-filled viewport, blends against the fill key. Every VA below was read off the
disassembly in this session.]

**`0x459200` — the per-unit composite blit** (`ret 0x18`), reached from `0x458810` under
`DrawUnit 0x45AC20`. It splits on the composite's depth-plane pointer at `0x45927E..0x459282`
(`mov eax,[esi+0x14]; test eax,eax; jne 0x45949D`): **path A** (colour-only composite) at
`0x459288`, **path B** (colour + depth) at `0x45949D`. Both open with the same shadow decision
tree, drawn before the body. Every site in it:

| Path A | Path B | What it is |
| --- | --- | --- |
| `0x45928E` `mov ax,[ecx+0x37F06]` | `0x4594A2` | the graphics-option word; `test al,4` (Shadow) right after, `je` to the body |
| `0x4592A0..0x4592AC` | `0x4594B4..0x4594C0` | `unit+0x92 → UnitDefStruct`, `+0x241` type mask, `test …,0x2000000` = `noshadow` → skip |
| **`0x4592BF`** `test byte [ecx+0x113],0x20` | **`0x459522`** `test dword [ecx+0x110],0x20000000` | **the structure bit** of `unit+0x110` — and, since 2026-09-18, the two addresses `owndraw all` **detours** (5-byte `E9` + `nop` fill over the `test`+`je`: 9 bytes at A, 12 at B) |
| `0x4592C6` `74 5C` `je 0x459324` | `0x45952C` `74 4A` `je 0x459578` | not a structure → the COMPLETED branch. Until 2026-09-18 `owndraw all` rewrote both to `EB` (`jmp`) for the life of the process; they are now inside the stolen range and the stub re-emits the `je` behind a runtime flag, so the suppression is per draw and stands down on a lane that never paints. Still verified byte-for-byte before the write, still installed as a pair or not at all |
| — | `0x4594D6` `74 4A` `je 0x459522` | **the only branch in the image into either stolen range** (a path-B instruction, hence the column), and it lands on the FIRST byte — our `E9` — so the detour is transparent to it. [MEASURED 2026-09-18: `objdump -d -M intel` over the whole image, every byte of `0x4592BF..0x4592C7` and `0x459522..0x45952D` searched for an incoming `j*`/`call`; and each of the four addresses searched through the raw image as a little-endian 32-bit datum, **0 occurrences**, so no jump table points at one either. A `je` flip is immune to an incoming branch and a detour is not, which is why this row exists] |
| `0x4592C8` `test [esp+0x10],0x40000000` (after the structure test) | `0x4594D0` `shr edx,0x1E; test dl,1` (**before** the structure test) | `digger`. Path A sends it to the COMPLETED branch, TShadow and `canhover`/`floater` tests included. Path B takes an **inline** branch `0x4594D8..0x45951D` — `0x45A470` silhouette, `0x4BA1B0(scratch, 0x7D)` ground clip, then `jmp 0x4595E9` to the shared blit — and applies neither test. So a digger never reaches the cached branch in either path |
| `0x4592D5` `cmp word [eax+0xA6],0` | `0x45952E` | `unit+0xA6` — the **model index** (`U_MODELID` in the native pass), not a unit id. Zero → |
| `0x4592E4` `movzx cx,byte [eax+0x1427F]` | `0x45953D` | sea level; `cmp word [esp+0x42],cx ; jl` skips the shadow for a model-0 unit below it |
| `0x4592FE` `call 0x45A790` | `0x45955B` | build the cached slant shadow when `Object3do+0x14` is NULL |
| **`0x459319`** `call 0x4B8500` | `0x459576` `jmp 0x4595E9` → **`0x4595E9`** | blit the cached shadow, at `sx + 0x85` (`add edx,0x85` at `0x45930B` / `0x45956C`). Path B shares one call site between the digger, structure and completed branches; path A has one per branch |
| `0x459324` `shr al,3; test al,1` | `0x459578` | the COMPLETED branch: TShadow bit, then `test …,0x81000` (`canhover`/`floater`), `0x45A470` (scratch := composite silhouette), blit at `0x459353` / `0x4595E9` |
| `0x4593BA` `call 0x4B8500` | `0x4597D3` | the body blit, further down each path. Its source is the SAME composite plane the shadow sites read — `esi`, loaded once at `0x45920D` and pushed at **`0x4593A2`** (for `0x4593A4 call 0x4B7F90`) and again at `0x4593B8` — which is why emptying the composite at the shadow takes the BODY with it whenever the wipe's predicate says yes and the pass drawing the replacement says no |

So the five "unit row sweep" call sites of `0x4B8500` in the blend-LUT survey above are:
`0x459319` structure shadow (A), `0x459353` completed shadow (A), `0x4593BA` body (A),
`0x4595E9` every shadow (B), `0x4597D3` body (B).

**The slant builders.** [BINARY-VERIFIED 2026-09-03; read in full 2026-09-07 for G14j —
`0x45A470..0x45A8A0`, `0x4B9D70..0x4B9E60`, `0x4C1000..0x4C1180`, `0x480D60..0x480E30`,
`0x458880..0x458930`.] `0x45A510` (the ground-projection AABB, `ret 0x14`: the object and four
out-pointers — width, height, hot X, hot Y) walks the prims at stride `0x36` from
`Object3do+0x22` and tests only flag bit0 (`test byte [ecx+0x28],1` at `0x45A55B`). It takes
the posed 16.16 verts at `prim+0x22`, snaps each to whole units by its high word (`sar 0x10;
movsx`), forms `gx = x + (y >> 2)` and `gy = (−z) − (y >> 2)` — the `neg` comes *before* the
shift, so it is floor(−z), and the quarter is an arithmetic shift of the whole-unit y — pads
the box by 2 on every side and returns `W = maxX − minX + 4`, `hot = 2 − min`. `0x45A610` (the
silhouette raster, `ret 8`: object, frame) tests **bit0 and bit1** (`test cl,1; je` at
`0x45A64C`, `test cl,2; je` at `0x45A655`), projects every vertex the same way plus the frame's
hot X/Y (`0x45A672..0x45A6CA`; a third component `y + 0x19` that the fill ignores), then walks
the node's face records (`node+0x28`, stride `0x20`) **from face 1 when `node+0x0C`, the
selection primitive, is not −1** and from face 0 otherwise (`0x45A6D4..0x45A6E9` — the same
rule as the body rasterisers and the effects renderer), copies each face's vertex list
(`face+0x04` count, `face+0x08` indices) and flat-fills it through `0x4C1000(frame, verts,
count, 0)` at `0x45A750`. **No material is consulted**: a textured face, a face with neither
colour nor texture (the footprint quad the body rasteriser skips), an n-gon — all filled, so
the cached shadow has no texture-key holes and no quad-only rule. `0x4C1000` takes the
polygon's integer bounding box, returns 0 without drawing when it lies outside the frame
(`minX > W−1`, `maxY < 0`, `minY > H−1`) or when `minY == maxY` (a flat row fills nothing),
clamps it and scan-converts with 16.16 edge steps rounded up (`add ecx,0xffff` at `0x4C1162`).
The projection is `gx = x + y/4`, `gy = −z − y/4` against the body's `−z − y/2`, so a point at
height `y` casts `y/4` right of and `y/4` below its drawn position.

**Bit1 is the piece's `cached` flag** ([build-state](../build-state.html) §4: on by default at
`0x45AED4`, cleared by the COB `dont-cache` handler `0x480DB0`, which masks the word at
`prim+0x28` — `obj + 0x4A + piece·0x36` — with `0xFFFD` and ORs the argument in, then nulls
`Object3do+0x10`; `0x480DF0` does the same for bit2 with `0xFFFB`; neither touches `+0x14`).
That is why a completed CORE wind generator's mast and rotor cast nothing: its script marks
`cradle` and `fan` dont-cache at Create — `tools/ta3do`'s `script_dontcache` walks the same
Create prologue as `script_hidden` and finds them, ARMMEX's `arms` likewise, and nothing on
ARMLAB, whose Create is fifteen `dont-shade`s. (Open until 2026-09-07 in this note; the
build-state page had it from its own reading.)

**The cache follows every composite rebake.** `0x458905` in the dispatch `0x458810` stores 0 to
`Object3do+0x14` immediately before calling the builder `0x4586A0` at `0x45890C`, on every
path that rebuilds the composite — so the shadow at `+0x14` is never older than the
composite, and a cached piece turning 8 angle units (the pose-sync at `0x45ADBA`), a
`show`/`hide`, a `cache`/`dont-cache` or a nanoframe pulse all refresh it at the next blit
through `0x45A790`. Reading the prims' bits live every frame, as the native pass does, is the
same rule. (The old `+0x14` block is not freed there; whether the builder frees it was not
read.)

**The punch-out `0x4B9D70(body, scratch, dx, dy)`** (`ret 0x10`) is aligned in screen space:
the column shift is `dx + (body.hotX − scratch.hotX)`, the row shift `scratch.hotY −
body.hotY − dy`, both clipped to the two frames, and for every body pixel that is not the
body's key it writes the scratch's key at the shifted position. With `dx = 5` and the shadow
blitted at `sx+0x85` against the body's `sx+0x80`, a body pixel and the scratch pixel it
erases land on the same screen pixel — the body's own footprint is cut out of the shadow
exactly where the body is then drawn. [DERIVED from the disassembly, not measured on its own;
the G13k note's "the engine erases it from +5" does not follow from these operands, and the
2026-09-07 toggle masks show no strip.]

**Neither path erases a structure's shadow at the waterline.** `0x459319` (A) and `0x4595E9`
via `0x459576` (B) blit `Object3do+0x14` straight after `0x45A790` returns; the `0x4BA1B0`
erase belongs to the COMPLETED branch (`sub + 0x32`) and the digger's inline branch (`0x7D`)
only. [MEASURED 2026-09-07: an applier-created, complete ARMLAB at altitude 63 on Two
Continents, sea level `TA+0x1427F` = 75 — its composite 102×114 **with** a depth plane, so
path B, which the "colour-only for stationary buildings" reading of the split above does not
predict — casts its whole slant in the stock engine (997 px in its own Shadows-toggle mask);
the native pass, which erased every shadow fragment at model height ≤ 12 as it does the
silhouette's, kept the nano arms' tops (515 px), and reads 1094 once the slant is exempt —
[shadows & cloak](../shadows-cloak.html) §"Structure shadows, parity".]

### The option word's own checkbox moves three bits [MEASURED 2026-09-22]

`main+0x37F06` bit2 `Shadow`, bit3 `TShadow` and bit4 `FShadow` are three bits and **one
control**. Driven through TA's own Options → Visuals on a live game, the `BSHADOWS` toggle takes
the word from `0x000C003F` to `0x000C0023` and back: all three clear together and all three set
together. There is no separate checkbox for the feature shadows or for the completed-unit one,
whatever the three names suggest — so a consumer that gates on bit2 alone and a consumer that
gates on bit3 alone are, from the player's side, the same switch. (The UNDO path's `0x45CAE0`
XORs the same word back from the saved copy at `0x512F38`, which is the other half of the same
story.)

What that settles for a renderer of our own: honouring the player's toggle is honouring bit2.
Testing bit3 for the silhouette, as `0x459324` does, reproduces the engine's branch and can never
disagree with the player's intent, because the two bits cannot be set differently through the UI.

**Why the branch flip is safe.** Both `je`s are 2-byte short jumps whose fall-through and
target both continue with `eax` still holding the option word the target tests (`shr al,3`),
and the COMPLETED branch is the engine's own path for every non-structure unit; under
`owndraw all` its composite is blank, so `0x45A470` builds an all-key silhouette and the blit
writes nothing. Nothing else reads `Object3do+0x14`; it is simply never allocated. Without
`all` the bytes are left alone and structures keep the engine's cached shadow.

**The two rasterisers `owndraw` already detours**, for completeness of this map: `0x459830`
(opaque, `thiscall`, `ret 0x10`; called from the builder `0x45878B` and from the blit's
build-state path `0x459641`) and `0x459C70` (nanoframe, called from the builder `0x458765`).
Both open `mov eax,imm32` (5 bytes) before `call __chkstk`, which is the detour boundary;
evidence and the classify-then-`ret 0x10` stub: `own-the-draw.md`, `tagpu_owndraw.c`.

**The completed-unit shadow's three emit sites — `owndraw`'s fourth detour until 2026-09-23.**
[MEASURED 2026-09-13, from play: the commander kept one teal silhouette on its own body from
map entry until it first moved. Read here off `objdump` of the pristine build.]

**We no longer patch `0x459338`, `0x45958C`, `0x4594DB` or touch `0x45A470`'s callers:** the
detour's wipe was gated on `tagpu_posedraw_live()`, which is the constant 0, so it replayed the
engine's call and did nothing else — the three sites keep the engine's original bytes. The
engine facts below stand; the detour paragraphs are its record.

Under `owndraw all` the engine's completed-unit shadow is supposed to be nothing: the
COMPLETED branch is the one every unit takes after the two `je`s are flipped, and with the
composite blank `0x45A470` builds an all-key silhouette. That last clause is the whole
guarantee, and it is not maintained by the composite being blank on every frame. Instrumented
for one session: **not one of 7047 wipes in a 60-frame window found the plane already empty**
(an empty plane reads 0 NON-Key bytes — the wipe fills it with the ColorKey, palette index 1 —
and reading the same plane back right after the wipe's own `memset` read 0), and the plane each
window's last wipe sampled held **557 non-Key bytes, all of them palette indices rather than the
zeros a blackened shadow leaves**. That 557 is also the number of teal pixels the silhouette
measured on screen; whether the two counts are the same shape is NOT established — the
instrumented plane and the drawn silhouette are read at different places. So the shadow was being built from a
real sprite while the classifier was skipping the rasterise that would have painted it. What
refills the plane that way is not settled by this reading and is left open here; the detour
below removes the dependency instead of answering it.

The sites, one per branch — all three are the same two instructions and a call:

| VA | Branch | Shape |
| --- | --- | --- |
| `0x459338` | path A, the completed branch from `0x459324` | `0x459335` `56` `push esi` · `0x459336` `8B CF` `mov ecx,edi` · `0x459338` `E8 33 11 00 00` `call 0x45A470` · resume `0x45933D` |
| `0x45958C` | path B, the completed branch from `0x459578` | same three instructions at `0x459589` / `0x45958A` / `0x45958C`, `call` bytes `E8 DF 0E 00 00`, resume `0x459591` |
| `0x4594DB` | path B's inline digger branch `0x4594D8..0x45951D` | same three instructions at `0x4594D8` / `0x4594D9` / `0x4594DB`, `call` bytes `E8 90 0F 00 00`, resume `0x4594E0` |

`0x45A470(this, composite)` is `ret 4` and writes **only the scratch** (`this+0x10`): it copies
the composite's header, colour and depth planes into the scratch and calls `0x4B96A0`, which
rewrites every non-ColorKey texel to palette index 0 — the unit's own silhouette in black.
Each site then blits the scratch at `sx + 0x85` (`0x459353`, `0x4595E9`) through the ALP blend
`0x4B8500`, after the path-B clips. **The composite is read, never written, so emptying it at
the call is the whole silhouette** — and the blit draws its BODY from the composite too
(`0x459373` `mov esi,[ebp+0x10]`), which is why the wipe is gated on `tagpu_posedraw_live()`:
on the frames the engine is the only renderer, emptying the composite would take the body with
the shadow.

`ebp` carries the blit's Object3do — the blit's **second** stack argument (`0x459205
mov ebp,[esp+0x30]`, before the remaining two pushes, so entry esp + 8; `[ebp+0xc]` is the unit
and `[ebp+0x10]` the composite) — down to all three sites. It is loaded once and **never
reloaded on any path that reaches them**: every earlier use is a read or a push (`[ebp+0xc]`,
`[ebp+0x14]`, `[ebp+0x10]`). The two cargo loops are the only reloads, and neither is in front
of a site — and in fact only path A's is an `ebp` reload at all. Path A's
(`0x459415 mov ebp,[edx+0x8A]`, `0x459489 mov ebp,[ebp+0x8E]`) exits into
the function's OWN epilogue (`0x459495 pop ebp`, `ret 0x18` at `0x45949A`), and path B is
entered at `0x45949D` by the `jne` at `0x459282`, so it never runs that loop. Path B's own
cargo loop at `0x459649` walks the same list in **esi** (`8b b2 8a 00 00 00`
= `mov esi,[edx+0x8A]`), not `ebp`; this note called it a second `ebp` reload until the
2026-09-14 review disassembled it. The corrected fact is the stronger one: `ebp` is loaded
once at `0x459205` and no path in the function reloads it before a site. (`0x4593FC push ebp` is an argument to the pose helper
`0x4584D0`, not the open of a bracket — an earlier note here said otherwise.) Because "it
happens to survive" is not an argument, the stub passes `ebp` and the helper checks it:
`*(Object3do+0x10)` must be the very composite the site is about to read, and a mismatch is a
no-op.

Two facts about the blit's own rasterise call, `0x459641` — and the first corrects an earlier
reading of this note. **It is taken for every MOBILE unit, every frame.** The gate is
`0x459610 test [ecx+0x110],0x20000000` / `0x45961A je 0x45962F`: the structure bit CLEAR jumps
straight to the call, so the `fcomp` below it is never reached for a unit. Only for a STRUCTURE
does `0x45961C fld [ecx+0x104]` / `0x459622 fcomp [0x4FD4C0]` (the constant is `0.0f`) /
`0x45962D je 0x459646` run, and it skips the call when Nanoframe != 0 — `test ah,0x40` tests C3,
which `fcomp` CLEARS on inequality, so ZF is set by *not* equal and the `je` is taken for an
under-construction structure. **Its first argument is not the composite**: `0x459639
mov ecx,[edi+0x10]` takes the blit's own frame — `edi` is the blit's `this`, which is not `ebp`
(the Object3do) — so this call and the builder's pass DIFFERENT planes, and the classify stub is
therefore called on two planes per unit per frame. The second fact: the only things that write a
composite's planes are the rasterisers and the allocators `0x437B50` / `0x437BE0` (chosen at
`0x458702` / `0x458719`) — `0x4581E0` and `0x4584D0`, the AABB/pose helpers, call neither.

The one case where the wipe's predicate and the classifier's differ is a husk: the classifier
recognises a 3D-wreck draw by the scratch feature-unit `*(main+0x1420F)` holding this Object3do
at `+0x9E` `[documented where the wreck path is: features.md §1 and its table, terrain-depth.md
§3.4]`, and hands it to the engine unless the `wrecks` token is armed; the detour asks the same
question, in the same order. A/B'd on `one-wreck` with `native.on` = `all` and no `wrecks` token:
with the clause disabled in a test build the husk rendered either way, so the unit predicate does
not answer yes to a husk there — the clause is an invariant, not a fix for a measured blanking.

The detour empties the composite at the site, before the replayed `0x45A470`, so the silhouette
is all-key on every frame our pass draws the unit; the engine cannot repaint it in between,
because the two are adjacent instructions of one call. Measured 2026-09-13 on a fresh skirmish, the same map and
the same starting spot either side: one 129-frame `glshot` burst spanning the load, the GL
context change and the first seconds of play — **no `(0,128,128)` pixel in any frame**. Before
the change the same burst found **557 in nearly every in-play frame**, and 0 in every frame
once the commander was ordered to move — the latch the play report describes.

**`0x459228..0x45927A` — where the body and the shadow are placed, register by register.**
[BINARY-VERIFIED 2026-09-04, read for the aircraft work.] `[ebp+0xc]` is the unit; `+0x6A`,
`+0x6E`, `+0x72` are X, altitude and depth as 16.16.

```
459233  mov ebx,[eax+0x6a]  / 459239 sub ebx,[esp+0x3c]   ; X - eyeX
45923d  push ecx            ; ecx = &unit.position (eax+0x6a)
459242  mov ebx,[eax+0x6e]  ; altitude          -> saved
459245  mov eax,[eax+0x72]  / 45924c sub eax,edx          ; Z - eyeY
459252  call 0x485070       ; GetPosHeight(&pos) -> eax = the GROUND under the unit
459257  movsx edx,[esp+0x42]  ; altitude, high word = whole world units
45925c  movsx ecx,[esp+0x46]  ; (Z - eyeY) high word
459261  mov ebx,edx           ; ebx := altitude       <- these two copies are what
459263  mov [esp+0x14],edx    ;   the altitude the waterline code re-reads at 0x45959F
459267  sar ebx,1             ; ebx := altitude / 2
459269  mov edx,ecx           ; edx := (Z - eyeY)     <- ...make the next lines read right
45926b  sar eax,1             ; eax := GROUND / 2   (0x485070's return)
45926d  sub edx,ebx  / 459274 add edx,0x20    ; bodyY   = (Z-eyeY) - altitude/2 + 0x20
45926f  sub ecx,eax  / 459277 add ecx,0x20    ; shadowY = (Z-eyeY) - ground/2   + 0x20
```

So the two Y values differ **only** in which height is halved, and the shadow's x is the body's
`sx + 0x80` plus 5 (`add edx,0x85` in the branch table above). For anything on the ground
`GetPosHeight` returns the unit's own altitude and the two coincide; for an aircraft the shadow
stays on the ground and trails the body down the screen by exactly `(altitude − ground) / 2`.
Measured in play the same day — see [shadows & cloak](../shadows-cloak.html) §"Aircraft, measured".

**`[esp+0x42]` is the altitude.** `0x459257 movsx edx,word [esp+0x42]` is stored at
`0x459263 mov [esp+0x14],edx`, and `[esp+0x14]` is what the waterline code subtracts from sea
level at `0x45959F` — the same word, sign-extended (the first draft of this section called them
different words; the G13k review corrected it). The model-0 skip therefore compares the unit's
altitude with sea level, which is what the native pass's `fz` gate does.

**Negative results.** The body punch-out `0x4B9D70(body, scratch, 5, 0)` inside `0x45A790` was read, not
replicated. `0x459200` itself was not disassembled past `0x459900`.

**A unit under construction casts no shadow until it is nearly finished, and this tree does not
say why.** [MEASURED 2026-09-03] One ARM solar, same ground, same camera, held at 25 / 50 / 75 /
85 / 89 / 95 / 99 / 100 % built by the scenario applier and captured off the stock renderer (no
passes armed). Taking the pixels the *completed* unit darkens by half as the shadow lobe, and its
own 25 % frame as the bare-terrain reference, the mean luminance over that lobe reads **1.00** at
25 %, **0.82** at 75 %, **0.84** at 85 %, **0.87** at 89 %, **0.70** at 95 %, **0.71** at 99 % and
**0.48** complete — no shadow for most of a build, something partial in the last few per cent.
**Caveat on the fixture:** the applier creates the unit complete and then writes `+0x104`, so its
composite and cached shadow have a history a lathed unit's does not; the recolour classifies this
model identically at `p` 28 and 12, so the 89→95 % step is not explained by the staging and may
be an artefact of that history. Nothing in the branch table above tests
`Nanoframe`: a building is a structure whether finished or not (see the state-bit measurement
below), so it reaches `0x45955B`/`0x4592FE` and blits `Object3do+0x14` either way. The
resolution is therefore in what that cached sprite CONTAINS while the unit is a nanoframe —
whose composite the dispatch `0x458810` throws away on every progress pulse
([build-state](../build-state.html) §1) — and that was not chased. Recorded as behaviour: our own
pass suppresses the shadow on `Nanoframe != 0`, which matches every row to 89 % and is
conservative for the last two.

**`unit+0x110 & 0x20000000` is the STRUCTURE bit.** [MEASURED 2026-09-03] Read live in one game
from `*(main+0x14357) + idx*0x118 + 0x110`: complete mobile ARMCOM `0x91600371` (clear),
complete building ARMSOLAR `0x30282321` (set), ARMLAB **under construction** `0x30E42321` (set).
So the table above is right to call it the structure bit — as the factory-built check of
2026-09-02 in [shadows & cloak](../shadows-cloak.html) already found from the other side (a
Peewee out of an ARMLAB reads it CLEAR) — and [build-state](../build-state.html)'s
"under-construction/nanoframe state" reading was wrong, corrected there. The consequences are
larger than a name: `0x45873C` selects the Gouraud rasteriser `0x459C70` for **structures**, not
for nanoframes, and the only state that means *under construction* is `Nanoframe != 0` at
`+0x104`. The spawn site `0x485AFE..0x485B03`, which that page cited as where the bit is set,
computes `(UnitDef+0x241 & 0x200) << 0x15` = bit **`0x40000000`** and writes only that.

## `0x458DD0` — the blit-time build-state effect — mapped by us

[MEASURED 2026-09-03, this project — `objdump` of the pristine Steam build, and detoured live.]
The whole nanoframe look — the height-threshold recolour `0x458D30` and the wireframe
`0x458FA0` — is applied at **blit** time to a scratch copy of the composite, every frame
([build-state](../build-state.html)). `thiscall(this, GAFFrame* frame, Object3do* obj)`, `ret 8`;
`obj` is at `[esp+8]` on entry. Two early-outs, both `xor eax,eax; ret 8`:

| VA | Bytes | What |
| --- | --- | --- |
| `0x458DD0` | `53 55 8B 6C 24 0C` | `push ebx; push ebp; mov ebp,[esp+0xC]` — `ebp` := the GAFFrame arg. **Six whole bytes, the detour boundary** (a 5-byte steal would split the `mov`) |
| `0x458DDA` | `8b 45 14 / 85 c0 / 75 09` | `frame+0x14` — no depth plane, return 0 |
| `0x458DEA..0x458E06` | `8b 44 24 18 / 8b 50 0c / d9 82 04 01 00 00 / d8 1d c0 d4 4f 00` | `obj+0x0C` → unit, `fld [unit+0x104]`, `fcomp ds:0x4FD4C0` (0.0f) — **`Nanoframe == 0` returns 0**, so this function only ever does anything for a unit under construction |
| `0x458E11..0x458E1F` | `66 8b 8a a8 00 00 00` | `unit+0xA8` (the slot index) into the oscillator maths |

Our detour (`tagpu_owndraw.c`, the third one) replays those six bytes after `popad` — at the
entry `esp`, so the esp-relative `mov` reads what it always read — and takes `xor eax,0; ret 8`
for units the native pass owns, which is the callee's own "did nothing" return. Call sites:
`0x458D0E` (the last `call` in `0x4589C0`, followed by that function's own
`pop edi/esi/ebp/ebx; add esp,0x68; ret 8` — a plain call, not a tail call) for the unit's own
scratch, and `0x459686` for each cargo composite, so a factory's unit-in-progress is covered by
the same skip. Both call sites discard `eax`, so the stub's `xor eax,eax` cannot be observed even
where the real function would have returned 1 (`0x458F88`).

The resume address is **`0x458DD6`**; the six stolen bytes end exactly on an instruction
boundary there.

## `0x459646..0x4596DD` — the cargo loop, and `0x4B90A0` the z-merge — mapped by us

[MEASURED 2026-09-03, this project — `objdump -d -M intel` of the pristine Steam build. Read,
not patched, while fixing "the unit is being built UNDER the lab". The mechanism and the
consequences for our pass are in [build-state](../build-state.html) §7, and the whole carry
relationship — what a factory does with the unit on its pad, and what happens when it lets go —
is on [factories](../factory-build.html); this is the address-level record.]

**A unit inside a factory is never sorted as its own sprite.** The blit walks the parent's cargo
chain and merges each member's composite *into the parent's scratch*, per pixel:

| VA | What |
| --- | --- |
| `0x459646` | `mov edx,[ebp+0xC]` (the unit) → `mov esi,[edx+0x8A]` — the cargo head; `je 0x4596EB` exits when the chain is empty |
| `0x459657` | `test dword [esi+0x110],0x20000` / `jne 0x4596DD` — **the chain skip**: a member with that bit is stepped over undrawn. Also the loop's re-entry target |
| `0x459670` | `call 0x4586A0(cargo_obj, 1, -1)` — rebuild the cargo composite, **every frame** |
| `0x459686` | `call 0x458DD0(cargo_composite, cargo_obj)` — the build-state effect on the cargo (the section above) |
| `0x45968B..0x4596D3` | the position delta: `cargo+0x6A/+0x6E/+0x72` minus the parent's same three dwords, each taken as its **high word** (the whole-world-unit part) |
| `0x4596D8` | `call 0x4B90A0` |
| `0x4596DD` | `mov esi,[esi+0x8E]` — next in chain; `jne 0x459657` loops |

**`0x4B90A0(srcFrame, dstFrame, sx, sy, dbias)`, `ret 0x14`** — a depth-tested 8bpp paint of one
`GAFFrame` into another ([composite-buffer](../composite-buffer.html) has the header layout). The
five arguments come out of the push order at `0x4596B5..0x4596D7`:

- `srcFrame` = `cargoObj3do+0x10`, `dstFrame` = `this+0x10` (the blitter's shared scratch);
- `sx` = `HIWORD(dx)`, `sy` = `HIWORD(dz) − HIWORD(dy)/2` — **the isometric projection of the
  delta**, not a raw dy, so the merge lands the cargo where the camera would put it;
- `dbias` = `HIWORD(dy)`, the pure height delta, applied to the *depth* plane.

The inner loop (`0x4B9130..0x4B9170`), per pixel: skip if the source colour equals the frame's
key byte at `srcFrame+0x8`; otherwise compare `dstDepth` against `srcDepth + dbias` and
**`jg` keeps the destination** — i.e. the source wins on `dstDepth <= srcDepth + dbias`, larger
depth = higher/nearer, the same convention the intra-model rasteriser uses. On a win it writes
the colour and sets `dstDepth = srcDepth + dbias`.

**Two negative results.**

- **`0x4B90A0` has exactly one caller in the entire image** — `0x4596D8`, the line above. A
  disassembly of every section and a scan for the literal find no other call and no pointer to
  it. The cargo merge is the only thing in the game that composites two sprites by depth.
- **The compare and the store disagree about width.** The compare adds `dbias` as a full dword
  to a zero-extended source byte (`0x4B913D..0x4B914D`), but the store re-reads `dbias` as a
  *byte* and does an 8-bit `add cl,dl` (`0x4B914F..0x4B9159`), so a stored depth wraps where the
  comparison did not. Cargo sits within a few world units of its parent, so `dbias` is small and
  this never fires in stock play; it is recorded because it is read, not exercised.

**Why this cost us a bug.** Our native pass first sorted a factory's cargo as an ordinary unit,
which put it on its own tile row — an ARM lab at world y 1072 building a Hammer at 1068 is one
16-unit row apart, four whole depth keys behind the lab, which then covered it at every pixel.
Matching the engine means giving every chain member the parent's row and band and letting the
two models sort against each other by `md`, our intra-model view depth, **plus the merge's height
offset** as a bias on the cargo's `md` (`tagpu_native.c` `cargo_md_bias`, applied by the posed
vertex stage as `uMdBias` inside `md`'s ±1.8 clamp). In depth-plane units a cargo pixel stands
`baseC − baseP + dbias` above its parent's, where `base` is the per-type plane base below; `md`
counts height at 2/256, so the bias is `2·(baseC − baseP + dbias)/256`. MEASURED: a commander
carried by an ARMATLAS hangs at `dbias` = −40 (bias −0.31) and is covered by it as in the engine;
an ARMATLAS on an ARMAP pad has `dbias` 0 and the same base, bias 0. It still **approximates** the
merge: `md`'s z term has no counterpart in `0x4B90A0`, which compares height alone.

**The plane's base, `0x459A29..0x459A3C`** (and its twins at `0x459A56..0x459A69` and in the lit
rasteriser, `0x459EAA`/`0x459ED7`) — `mov edx,[def+0x241]` / `shr edx,0x1e` / `and dl,1` /
`neg dl` / `sbb edx,edx` / `and edx,0x4B` / `add edx,0x32`: each pixel's depth byte is its model
height plus **0x32, or 0x7D when `UnitDef+0x241` bit 30 is set**. Bit 30 is the digger bit
(`tagpu_native.c` `UD_DIGGER`), not "airborne": MEASURED clear on ARMATLAS (`0x00808889`) and
ARMCOM (`0x0495C0C8`). The higher base leaves room below the origin for the `0x7D` clip to
erase.

### The graphics globals block — `globals = *(void**)0x51FBD0` [VERIFIED]

`0x4B6220` is `mov eax,[0x51FBD0]; ret`. The fields the UI path uses:

| Offset | Meaning | Evidence |
|---|---|---|
| `+0x44` | non-zero ⇒ GDI/windowed present; `+0x50..+0x7F` is then an inline OFFSCREEN over the DIB | `0x4C5EA4..0x4C5EC3`, `0x4C63C0` |
| `+0x88` | `IDirectDrawSurface*` primary (`Lock` vtbl+0x64 at `0x4C6509`, `Unlock` vtbl+0x80 at `0x4C6595`, `Flip` vtbl+0x2C at `0x4C6682`) | `0x4C64E7`, `0x4C658A`, `0x4C6676` |
| `+0x8C` | `IDirectDrawSurface*` used when there is no system back buffer | `0x4C5EC6..0x4C5F92` |
| `+0x98` | OFFSCREEN\* written by `0x4C61F0(surface)`; read **only** on the flip's `DDERR_SURFACELOST` restore paths (`0x4C65E3`, `0x4C675B`). Not the normal source | `0x4C61F0..0x4C61FF` |
| `+0xA0..+0xAF` | clip rect copied into the context `0x4C5E70` builds over a locked DD surface | `0x4C5F53..0x4C5F6A` |
| **`+0xBC`** | **OFFSCREEN\* system-memory back buffer — what every flip presents and every NULL-context blit draws into.** Written by `0x4C69A0(surface)` together with `+0xDC = 1` | `0x4C69A0..0x4C69B9`; read at `0x4C6405`, `0x4C6485`, `0x4C5E86`, `0x4C68B5` |
| `+0xC0` / `+0xC8` | blend LUT / shade table (the ALP remap) | `0x4B8665`, `0x4B847A` |
| `+0xD4` / `+0xD8` | screen width / height (`0x4B6700` / `0x4B6710`) | `0x4B6700..0x4B671B` |
| **`+0xDC`** | 1 ⇒ `+0xBC` is valid; set by `0x4C69A0`, cleared at `0x4C638C` | |
| `+0xF0` bit 1 | DirectDraw present (clear ⇒ GDI); bits 5 / 7 gate `0x4B8500` / `0x4B8310` | `0x4C63B8`, `0x4B8519`, `0x4B8329` |

`0x4C69A0` has 28 callers; **`0x468D30` inside `DrawGameScreen` makes `*(main+0x37E1B)` the
back buffer every frame**, and `0x467D7F` (the HUD painter) does the same. **In the shell the
back buffer is the startup `"OFFSCREEN" 640×480`**, a different object; the census confirmed
both by address (`main+0x37E1B == *(globals+0xBC) == 0x04490020` in a 1024×768 game).

### The OFFSCREEN object [VERIFIED, `0x4C69F0`]

```
+0x00 width   +0x04 height   +0x08 pitch (= width)   +0x0C pixel base (= this + 0x30, inline)
+0x10 0x2710  +0x14 -1       +0x18 s16 originX  +0x1A s16 originY   (0 on create; the GAF
                                                  hotspot when 0x4B8A80 built the header
                                                  over a GAF frame)
+0x1C..+0x28  clip L,T,R,B, INCLUSIVE, (0,0,w-1,h-1) on create
+0x2C         flags: bit0 = heap object, freed by 0x4D85A0
+0x30..       pixels, w*h bytes;  allocation = w*h + 0x30 via 0x4D83B0(tag, size)
```

- **`SurfaceCreateNamed 0x4C69F0(const char* tag, int w, int h)`** — `stdcall`, `ret 0xC`,
  returns the object. Prologue `53 56 8B 74 24 10`. **Header and pixels are ONE block**:
  `imul eax,esi; add eax,0x30` at `0x4C6A01`/`0x4C6A04` is the size it asks `0x4D83B0` for,
  and `lea edx,[eax+0x30]` / `mov [eax+0xC],edx` at `0x4C6A0E`/`0x4C6A14` is what makes the
  pixel base `object + 0x30`. That identity is what lets an observer on `MEM_Free` retire a
  surface from the block pointer alone.

  **EVERY ONE OF THE 18 CALL SITES AND THE TAG IT PUSHES** — enumerated out of the binary
  2026-09-21 rather than sampled from a trace, because the tag is what `PK_ASSET` keys on and
  a list that is merely *mostly* complete cannot answer "can this prefix alias anything else":

  | site(s) | tag | string |
  |---|---|---|
  | `0x41F85E` | `0x502BE0` | `"Copy of last game frame"` |
  | `0x460190` | `0x506C9C` | `"FLIPSURFACE"` |
  | `0x46020E` | `0x506C90` | `"BKUPSURFACE"` |
  | `0x466823` | `0x5074F8` | `"RADAR PICTURE"` |
  | `0x466881` | `0x5074E8` | `"RADARPIC TEMP"` |
  | `0x4669CF` | `0x507518` | `"RADAR FINAL"` |
  | `0x4669FA` | `0x507508` | `"RADAR MAPPED"` |
  | `0x490AD8`, `0x491255`, `0x491B28`, `0x4980D4`, `0x498407` | `0x5091D4` | `"OFFSCREEN"` ×5 |
  | **`0x4A907C`** | **`ebp+2`** (a `lea`, not a load) | the screen's own name — `"MAINMENU.GUI"`, `"SINGLE.GUI"`, … |
  | `0x4A90B5` | `0x509914` | `"SAVE UNDER"` |
  | `0x4C2C22`, `0x4C2C39`, `0x4C2C50` | `0x50A5E8`/`0x50A5DC`/`0x50A5D0` | `"SAVEMOUSE 1/2/3"` |
  | **`0x4CAFB3`** | **`ebx`** | the PCX loader's path — `"bitmaps\FrontendX.PCX"`, … |

  **`"GUI SURFACE"` (`0x509920`) IS DEAD CODE, and this is the negative result that matters.**
  At `0x4A906C` it looks exactly like the screen surface's tag, and reading the string at that
  address "confirms" it. It is the else-arm of a null test the compiler could not fold:

  ```
  4a9065: lea  eax,[ebp+0x2]     ; the GUIMEMSTRUCT's name field
  4a9068: test eax,eax
  4a906a: jne  0x4a9071          ; ALWAYS taken — an address of ebp+2 is never 0
  4a906c: mov  eax,0x509920      ; "GUI SURFACE" — unreachable
  4a9071: …
  4a907b: push eax               ; the tag actually pushed
  ```

  A trace of a shell session names `"MAINMENU.GUI"`, `"SINGLE.GUI"` and `"SKIRMISH.GUI"` and
  never once `"GUI SURFACE"`, which is the live half of the same fact. **A string sitting at a
  `mov` before the `push` is not evidence it is pushed**; this one cost a review a wrong
  finding and nearly cost this file a correct line.

  **The tags name the surfaces**, and a session's inventory reads (MEASURED): `"OFFSCREEN"
  640×480` (shell) and `1024×768` (game), one `"<SCREEN>.GUI"` per pushed screen
  (`MAINMENU.GUI 640×480`, `ARMCOM1.GUI 128×352`, `PREFS.GUI 128×354`, **`VISUALRT.GUI
  278×354`**, `TALK.GUI 512×33`, …), a `"SAVE UNDER"` per screen, `"SAVEMOUSE 1..3"`,
  `"FLIPSURFACE" 128×352`, `"BKUPSURFACE" 300×480`, and one `"bitmaps\<name>.PCX" 640×480`
  per shell background loaded.

  **NO LITERAL TAG BEGINS WITH `bitmaps\`** — checked against all twelve strings above. That is
  what stops the fork's eight-character prefix test aliasing one of the engine's composed or
  scratch surfaces, which is the case that would be a correctness bug.

  **IT IS NOT A TEST FOR "A SHELL BACKDROP", AND SAYING SO WOULD BE THE OVER-REACH.** The twelve
  literals are only twelve of the fourteen tags; the two REGISTER sites are not bounded by that
  check, and one of them reaches further than the shell. `0x4CAF30` has three callers, and
  `0x4292BD` sits inside **`0x429290`** — a generic loader that formats `bitmaps` (`0x503384`)
  with the caller's name and `PCX` (`0x502974`) and loads the result, and which has **four**
  callers of its own, one of them (`0x41DBB2`) working out of `"bitmaps\glamour"` (`0x502964`).
  So every surface `0x429290` creates carries a `bitmaps\…PCX` tag and the prefix claims that
  whole class — not only the three shell backgrounds. The other two direct callers of `0x4CAF30`
  build `unitpics\<name>.PCX` and do not match.

  **That is the right class rather than a leak, and the distinction is the point**: what the tag
  establishes is *"the PCX loader filled this surface"*, never *"this is a menu background"*. The
  fork's `tag_is_shell_bg` is named more narrowly than it behaves; what it actually claims is a
  loader-decoded PCX, `op_add` re-checks that no observed draw covers a pixel of it, and a
  surface that is never a copy source costs nothing whether it is claimed or not. [Reach found
  by the landing review of 381465c; the caller counts and the three strings verified here.]
- **`SurfaceFree 0x4C6AC0(OFFSCREEN*)`** — `stdcall`, `ret 4`: `if (p && p[+0x2C] & 1)
  0x4D85A0(p)`. Prologue `8B 44 24 04 85 C0`. 23 callers; the GUI's are `0x4A9537`
  (`panel+0xB8`) and `0x4A9549` (`panel+0xBC`) in the teardown arm.
- **`SurfaceAttach 0x4C6A60(OFFSCREEN* out, w, h, pitch, base)`** — `stdcall`, `ret 0x14`: the
  same header laid over memory the object does NOT own. Its flags at `+0x2C` are the ones
  `0x4C69F0` and the frame-context builder `0x4B8A80` write — bit 1 cleared, bit 0 SET
  (`0x4C6A7C`, `0x4C6A84`) — so the flags do not tell an attached header from an owned one, and
  `SurfaceFree 0x4C6AC0`, which hands the object to `MEM_Free` whenever bit 0 is set
  (`0x4C6AC9`), does not either; whether anything ever frees an attached object that way was not
  checked. **One caller, `0x4B5897`**: right after `[0x4FC06C]` (the
  DirectDraw `Lock`) it wraps the locked surface's bits, `w`/`h` from `globals+0xD4`/`+0xD8`
  and `pitch = (w+3) & ~3`, in the object at `that+0x50`. So the *primary* can be a drawing
  destination too — although MEASURED 2026-09-12 across a whole session, no UI blit named it:
  every destination the Phase E observers saw was a `0x4C69F0` object.
- **`MEM_Free 0x4D85A0(p)` is the one way any engine allocation dies** [VERIFIED 2026-09-12].
  It is a two-line wrapper (`mov eax,[esp+4]; push eax; call 0x4D85B0; add esp,4; ret`,
  prologue `8B 44 24 04 50` — five relocatable bytes, which is what makes it detourable), and
  **`0x4D85B0` has exactly one caller: this**, and it is thread-safe: it takes the allocator's
  critical section (`0x4DA780`, then `EnterCriticalSection` through `ds:0x4FC198` at `0x4D85C2`
  [DISASSEMBLED 2026-09-29]) before it touches the heap. 363 sites call `0x4D85A0`, `SurfaceFree
  0x4C6AC0` among them (`0x4C6ACF`). Its partner is `MEM_Alloc 0x4D83B0(tag, size)` →
  `0x4D83C0`, which ignores the tag in the shipping build and goes to `0x4DACF0(size, 0)` or
  `0x4E8890(size)`.
- **THE ALLOCATOR DOES KNOW A BLOCK'S SIZE: `MEM_Size 0x4D8360(p)`** [VERIFIED 2026-09-16,
  objdump of the pristine build]. `0x4D85B0` calls it on the free path at `0x4D85E7` and feeds
  the result to `0x4DA840`, which is the accounting. Its body is a two-way dispatch on the same
  mode predicate the rest of the allocator uses — `call 0x4D80D0; test al,al` — to
  `0x4DBAE0(p)` (the private heap's sizer) or `0x4E8FB0(p)` (the CRT `_msize`); it returns 0 for
  a NULL block and the size in `eax` otherwise. `MEM_Alloc`'s dispatch at `0x4D83D5` tests the
  same predicate, so the two agree by construction about which heap a block came from.
  **It takes no lock of its own, but both of its callees do**: `0x4DBAE0` enters the allocator's
  critical section `0x528A28` first (`0x4DA780`, then `EnterCriticalSection`), and `_msize` takes
  CRT heap lock 9 (`0x4EAC60(9)`). So it is safe to call from an observer at `MEM_Free`'s entry;
  the G19f-7 reason for not using it — that it read the heap outside any lock — was wrong. The
  GUI lane's destructor calls it inside the allocator's section, as `0x4D85B0` does (below).
  Because a surface's header and pixels are ONE block (`w*h+0x30`, above),
  `block + 0x30 == the pixel base`, and an observer at this function's entry is exactly a
  surface destructor — which is what `tagpu_gui_hook.c`'s `before_memfree` is (G18-8): the
  publisher reads `s->base` at the flip, and what makes that safe is that the table entry
  cannot outlive the block. MEASURED in game (2026-09-12): **~10 500 calls a second**.
  **The loader thread makes most of them**: 33 191 frees on threads other than the game thread in
  one Two Continents load (MEASURED 2026-09-29, two counters at the observer; 33 842 was the
  2026-09-12 figure), and not one of those blocks held a tracked surface's base. The thread that
  frees a block is not the thread that draws into it, so an observer that fires off the game
  thread asks a mirror of the tracked bases instead of the table (`live_hit`,
  [GUI renderer](../gui-renderer.html) §31).
- **A drawing destination lies inside one block, at an offset that depends on what made it,**
  and the context over it does not say which (every builder writes the same `+0x2C` flags):
  - `0x4C69F0` surfaces: the pixels at `block + 0x30`.
  - frames from `0x4B8DA0` (`w·h + 0x18`) and `0x4B8E00` (`2·w·h + 0x18`, two planes): the
    pixel pointer `+0x10` = `block + 0x18` (`0x4B8DD0`, `0x4B8E1F`). `0x4B8A80(ctx, frame)`
    (`ret 8`) builds the context — `w`, `h`, `pitch = w`, the base from `frame+0x10`, the clip
    reset through `0x4C69C0` — and each of its five callers draws into a frame one of those two
    made: `0x458C2E` the unit composite (`[obj+0x10]`, stored by `0x4581A4` from
    `0x4B8E00("…", 600, 600)`), `0x4666B1` and `0x4666E2` SELMAP's scratch copy and preview
    frame (*SELMAP's map preview*), `0x483918` the level's minimap picture, `0x495BCF` a frame
    from `0x495ACD`/`0x495AEE`.
  - two-plane frames from `0x4B91B0` (one caller, `0x42063A`, `(22, 22, 8)`): the header width
    is `2w`, `+0x10` = `block + 0x18` and `+0x14` = `block + 0x18 + 2w·h` (`0x4B91E8..0x4B91F3`).
    `0x4B9360` (one caller, the projectile drawer's render-type-2 arm at `0x49C1E8`, the frame at
    `[main+0x1AB9B]`) swaps `+0x10` and `+0x14` on every call (`0x4B936D..0x4B9376`) and builds a
    context INLINE over the first (`0x4B937F..0x4B93C8`), the destination of the rect copy
    `0x4C6D20` at `0x4B9433`. The frame dies at leave-game: `0x491B9F` → `0x420960` →
    `MEM_Free` at `0x420A09`. (`0x4B94C0` builds the same inline context and has no caller.)

  A list of offsets is closed only until the next builder is found, so `before_memfree` retires
  every entry whose base lies in `[block, block + MEM_Size(block))` (`surf_dies_with`; the size
  asked inside the allocator's section, `mem_block_size`) — containment, so it neither misses an
  entry nor retires another block's. MEASURED 2026-09-26 with the `gui trace`: every SELMAP pick
  retires the 125×125 preview frame at the free returning to `0x444AF9` and the 252×252 scratch
  copy at `0x466769`. With `block + 0x30` alone neither was retired, and an arrow walk through the
  map list — several picks between two flips — crashed in the publisher's seed of a freed preview
  frame: a read access violation at the end of its heap segment, 95 rows into a 125-pitch entry.
  The size query costs **~85 ns a call** (MEASURED 2026-09-26, 200v200 on Two Continents, 80 144
  calls; ~675 frees a second in that battle), so even the ~10 500 a second above is under 0.1 % of
  one core. No fixture draws the render-type-2 effect, so `0x4B9360`'s frame was not seen
  retired live; the test that retires it is the one measured on the SELMAP frames.
- **`SurfaceFill 0x4C6890(surface, colour)`** — `stdcall`, `ret 8`: fills `h·pitch` bytes at
  `+0xC`; `NULL` ⇒ the back buffer. Prologue `83 EC 64 53 55 56 57`.
- **`GetContext 0x4C5E70(OFFSCREEN* out)`** — `stdcall`, `ret 4`, prologue `83 EC 6C 56 57`
  (then `E8`): the NULL-context path every blitter takes. Arm 1, `[globals+0xDC] != 0` →
  `rep movs 0xC` from `*(globals+0xBC)`, return 1 — **the arm taken in game and in the shell**;
  arm 2 the GDI block; arm 3 locks `[globals+0x8C]`. `0x4C5FA0(ctx)` releases arm 3 only. 51
  call sites each. **So a blitter given `ctx == NULL` draws into the system back buffer.**
- `0x4C6AE0 GetClipRect` — `thiscall(ecx = ctx, RECT* out)`, `ret 4`, four dwords from
  `ctx+0x1C`. `0x4B7E60 ClipRectPair(dst, src, clip)` — `stdcall`, `ret 0xC`.

### `FlipOffscreenToPrimary 0x4C63A0` — no arguments, and where the cursor goes [VERIFIED]

`void (void)`, plain `ret`; prologue `81 EC F4 00 00 00` (6, or 10 with the four pushes). 44
`call`s and 3 tail `jmp`s (`0x4257BA`, `0x425B7B`, `0x45CFB8`): `0x46A3DB` in DrawGameScreen,
`0x49FA32` in the message-box loop `0x49F9C0`, `0x467E41` in the HUD painter, and 41 more.
The DirectDraw arm (`0x4C6475`) requires `[globals+0xDC]`, takes **`edi = *(globals+0xBC)`** as
the source, checks it against the screen size, locks the primary, builds a context over it at
`[esp+0x38]`, **draws the cursor into the back buffer with `0x4C67C0(globals, edi)`**, copies
back buffer → primary with `0x4CBBE0(&ctx, edi, 0, 0)`, **restores the cursor's background
with `0x4C6B70(edi, [globals+0x1BE], [globals+0x1B6], [globals+0x1BA])`**, and unlocks. So the
back buffer holds the cursor only between two calls inside the flip: at the flip's entry it
does not, and a diff taken there never sees the cursor. The surface-lost arm re-copies
`[globals+0x98]`; the GDI arm copies into `globals+0x50` and presents with `StretchDIBits`
[INFERRED from the IAT slot]. **MEASURED: the shell flips about 5 000 times a second** on the
reference setup (31 678 flips in the first 6 s of a launch); in game once per `DrawGameScreen`.

#### The tagged lock at `0x52A4E8`, and why the cursor words can be read at all [VERIFIED 2026-09-14]

The flip and the engine's **mouse thread** take the same lock, under different tags, and that —
not anything of ours — is what orders their writes to the cursor position words `+0x1B6/+0x1BA`.
The bytes, disassembled from the pristine exe:

| where | bytes | what |
|---|---|---|
| `0x4C63CC` | `bf 4e 49 41 4d` | `mov edi,0x4D41494E` — the tag `'MAIN'` (first char in the high byte) |
| `0x4C63D2` | `68 e8 a4 52 00` | `push 0x52A4E8` — the lock object |
| `0x4C63D7` | `ff d5` | `call ebp` — acquire: `InterlockedExchange` (`ebp` loaded from the IAT slot `0x4FC174` at `0x4C63C0`) |
| `0x4C63DD` | `39 3d ec a4 52 00` | `cmp ds:0x52A4EC,edi` — the current-owner tag against ours |
| `0x4C6641` | `68 e8 a4 52 00` | release, part 1: `push 0x52A4E8` — the same object |
| `0x4C6646` | `c7 05 ec a4 52 00 00 00 00 00` | release, part 2: `mov ds:0x52A4EC,0x0` — the owner tag cleared |
| `0x4C29C8` | `bf 53 55 4f 4d` | `mov edi,0x4D4F5553` — the tag `'MOUS'` |
| `0x4C29CE` | `68 e8 a4 52 00` | `push 0x52A4E8` — **the same object** |
| `0x4C29D3` | `ff d3` | `call ebx` — acquire, `InterlockedExchange` again (`ebx` from `0x4FC174` at `0x4C29BB`), around the mouse thread's own `call 0x4C25E0` |

So `0x52A4E8` is the lock word and `0x52A4EC` holds the owner's four-character tag, zeroed on
release. Both tags pack the first character in the most significant byte, which is consistent
across the two and is the evidence for reading them as `'MAIN'` and `'MOUS'` at all. **The
protocol** (DISASSEMBLED 2026-10-01, both sides): `InterlockedExchange(0x52A4E8, tag)`; an old value
of 0 means the lock is taken, and the taker stores its tag in `0x52A4EC` (`0x4C63F1`, `0x4C29F2`);
an owner tag equal to its own means it already holds it; otherwise it waits on the event
`*(0x52A4F0)` with `WaitForSingleObject(…, INFINITE)` and exchanges again. Release clears the tag,
exchanges 0 into the word and sets the event (the mouse thread's `0x4C2A16..0x4C2A2B`). So **the
owner is the tag word**: the lock word holds the last exchanger's tag, which can be a waiter's while
another thread holds the lock.

**Why it matters.** `+0x1B6/+0x1BA` have two writers — the flip's cursor draw `0x4C67C0`
(`0x4C683C`/`0x4C684E`) and `0x4C25E0` at `0x4C284C`/`0x4C2852`, the mouse thread's body, which
loops about every 33 ms for the whole process, play included. Anything reading that pair as two plain
dwords is racing the mouse thread unless it is inside the flip's hold, which spans `0x4C67C0`
and runs to `0x4C6641`. `tagpu_packet_pub.c`'s shell cursor channel is inside it (and since
2026-09-23 reads neither word — the packet carries `+0x1B2` alone); its landing
(2026-09-13) shipped with a different and insufficient argument — "read the position *this* draw
wrote" — which rules out only the game thread's own previous draw. Corrected 2026-09-14. **A
future reader of these words outside the flip does not inherit this ordering.**

### The effects models — who poses them, the two draws, and the three rasterisers [DISASSEMBLED 2026-09-24, objdump of the pristine build; MEASURED 2026-09-24]

Every 3DO model an effect draws — a weapon's body and its thrust flame, a debris piece, an
explosion's body — goes through one rotation and one of two face walks, and each face through
one of two integer rasterisers. The fork reproduces the chain end to end: the frame packet's
publisher calls the rotation itself on the game thread, and `tagpu_fxmodel.c` transcribes the
projection and the rasterisers on the render thread ([GPU status](../gpu-status.html) §2.89 has the
design and the measurements). Nothing here is patched; `fxown`'s detours on `0x46BAE0` and
`0x4211D0` (§2.1 of GPU status) are what stop the engine's own draw.

| Address | What it is, and how it was established |
| --- | --- |
| `0x4B6CC0` | **the rotation by a triple**, `void __stdcall (const int32 src[3], int32 dst[3], const int16 turn[3])`, `ret 0xc`. Three calls of the pair rotator: `turn[0]` on `(x, y)` (`0x4B6CE3`), then `turn[2]` on `(y', z)` (`0x4B6D04`), then `turn[1]` on `(x', z')` (`0x4B6D29`); `dst[1]` is stored after the second, `dst[0]` and `dst[2]` after the third. **Pure**: it reads its two inputs and the constant below and writes `dst` alone, so the publisher calls it with a `dst` of its own. DISASSEMBLED (`0x4B6CC0..0x4B6D43`) |
| `0x4B7173` | the pair rotator, `cdecl (int16 angle, int32 pair[2])`: a zero angle returns untouched (`cmp word [ebp+8],0 / je`); otherwise `a' = a·cos − b·sin`, `b' = a·sin + b·cos` in x87 (`fild`, `fsincos`) and stored back with `fistp`, so the result is rounded by the thread's control word. The angle is `fild word` — signed. DISASSEMBLED |
| `0x509EF8` | the double `2π/65536` the angle is scaled by (`fmul qword [0x509EF8]` at `0x4B718C`). DISASSEMBLED |
| `0x46BAE0` | **the model draw** of the projectile and explosion passes, `stdcall (ctx, pos[3], node, turn[3])`, `ret 0x10`. `pos` is EYE-RELATIVE: its callers subtract `eye << 16` from x and y first (`0x49C0A1..0x49C0CE` for rendertype 1). Per vertex (`0x46BB0F..0x46BB74`) it rotates `node+0x24` into the scratch `*(main+0x14383)` (12 bytes a vertex, **200 of them**) and projects into `*(main+0x14387)` (8 bytes, **200**), with no test of the node's count against either (the three blocks are sized below, `main+0x14383` row): `sx = (int16)((v.x + P.x) >> 16) + 0x80`, `sy = (int16)((P.y − v.z) >> 16) − ((int16)((v.y + P.alt) >> 16) >> 1) + 0x20` — the `(int16)` the `movsx` at `0x46BB3B`, `0x46BB43`, `0x46BB51`, so a vertex 32 768 px or more from the eye lands modulo 65 536. A vertex count of 0 or less skips the rotation and the projection (`test ecx,ecx / jle 0x46BB76` at `0x46BB07`) and still walks the faces, over whatever the scratch holds. Then the faces (`0x46BB76..0x46BC4E`): face 0 skipped when `node+0x0C != −1`; each face's vertices gathered by its `u16` indices into `*(main+0x1438B)` **with no bound on the index, nor on the count against the block's 20 points**; flag bit 0 → `0x4C0310(ctx, xy, n, face+0x00)`; otherwise only a 4-vertex face draws, `0x4C7580(ctx, frame, xy, NULL)`, the frame `face+0x10` or, with flag bit 1, `0x4B7EE0(face+0x10)`. No shade table anywhere. DISASSEMBLED |
| `0x4211D0` | **the debris piece draw**, `stdcall (ctx, sys, piece)`, `ret 0xC`, reached only from `0x421550`. It takes the eye off the piece's own position (`main+0x1431F`/`+0x14323`), draws nothing unless that point is in the viewport rect (`0x4B6720(main+0x37E27, sx, sy)` at `0x42123F`), and projects the vertices `0x421550` already rotated into `piece+0x22` with the same arithmetic as `0x46BAE0`, into **its own stack frame** of `0x3F58` bytes (the stack probe `0x4E4B20` allocates it with `eax = 0x3F58`, then four pushes, so the locals are `esp+0x10..0x3F68` and the return address is at `esp+0x3F68`): the projected vertices from `esp+0xE8` at 8 bytes each (the `lea` at `0x421256` is vertex 0's y at `esp+0xEC`; the face gather reads `[esp+8·i+0xE8]` and `[+0xEC]`, `0x4212EC`), room for **exactly 2000**, the next one over the return address; and one face's gathered points at `esp+0x20..0xE8` (`0x4212DC`), **25 of them** before the vertices begin — no push between the two `lea`s, so a face of 26 points or more writes over the node's first projected vertex. Neither count is tested; a count of 0 or less skips the projection (`test edx,edx / jle 0x4212A3` at `0x421252`) and still walks the faces. The face rules are `0x46BAE0`'s, plus flag bits 1 **and** 2: `0x4B7F30(face+0x18, logo)` with the logo colour reached as `sys+0` (the unit the piece flew off) `→ +0x96` (its PlayerStruct) `→ +0x27` (PlayerInfo) `→ +0x96` (`0x42132F..0x421346`), every link unchecked. DISASSEMBLED |
| `0x421550` | the piece's update and draw, `stdcall (ctx, sys)`, `ret 8`, from the explosion pass's slot loop: smoke (`0x472810`) and fire (`0x472AB0`) by `sys+0x28` bits 1 and 0, then the triple built **reversed** as `{w14, w12, w10}` (`0x4215AF..0x4215C5`), every vertex of `*(piece+0)` rotated into `piece+0x22` from the last to the first (`0x4215EE`), then `0x4211D0`. The tick `0x4213B0` spins exactly those three words (`0x421509..0x421519`). DISASSEMBLED |
| `0x421620` | a piece off a dying unit (callers `0x420F14`, `0x48123C`): with `sys+0x28` bit 2 it hands the piece to `0x421700`; otherwise it takes a free flying-piece slot, allocates the system with room for the rotated vertices (`0x66 + 12·nverts`), copies the 0x30-byte header (its `+0x00`, the unit, at `0x4216AA`) and the 0x36-byte piece, points `piece+0x22` at the vertices behind it and adds the unit's position. DISASSEMBLED |
| `0x421700` | **the piece-explosion bodies**: one explosion per 4-vertex, non-flat face of the piece's node other than its selection primitive, up to the explosion cap (`0x421771`, raised), each with a record from the pool below as its `+0x00` body, taken at `0x4217DE..0x4217F1` (the same first-free scan as `0x420920`) and filled from the face. DISASSEMBLED |
| `0x420620` | at the level load (`0x4919D2`): among its tables, **the piece-explosion pool** — 300 records of `0x34` bytes at `main+0x1AB9F` (`0x4207FB..0x42088F`), each node-shaped: `+0x00` the free byte (`0xFF` free, `0` taken), `+0x04` 8 vertices, `+0x08` 6 faces, `+0x0C` −1, `+0x24` its own 96 bytes of vertices, `+0x28` its own six `0x20`-byte faces — **whose index pointers are the executable's `.data` table `0x502BF8`**, 6 × 4 `u16` (`0x42089C..0x4208F2`). Also the explosion sequence table `main+0x1AB8F`. DISASSEMBLED |
| `0x420920` | the pool's first-free scan: the first record whose byte is `0xFF`, marked `0` and returned; `0` when all 300 are taken. No direct `call` to it in the image; `tagpu_patches.c`'s limits block redirects it and the inline copy at `0x4217DE` to a larger pool of its own with the same layout. DISASSEMBLED |
| `0x420AE6` | the ordinary explosion add site `0x420A30` writes its `+0x00` body as **0** — only `0x421700`'s explosions carry one. DISASSEMBLED |
| `0x4210A3` | the explosion tick `0x420F30` frees a body by writing `0xFF` over its first byte (after re-adding an explosion through `0x420A30`), then zeroes the explosion's `+0x00`. DISASSEMBLED |
| `0x420C53..0x420C68` | the explosion pass's body draw, after the anchor passed `0x4B6720(main+0x37E27, …)`: `+0x00` non-zero → `0x46BAE0(ctx, pos, body, +0x4C)`. DISASSEMBLED |
| `0x49C0FA..0x49C11A` | rendertype 1's triple: the projectile's `+0x34` words with `0x8000` added to the second and third; `0x49C127` draws the weapon's `+0x74` node. `0x49C12C..0x49C185`: the thrust flame, the node's `+0x30` child, while `+0x46` (death) is after the tick, with `+0x64` as its first word when the weapon's `+0x111` has bit 21. `0x49C477`: rendertype 6 passes the words as they are. `0x49C252`: rendertype 3 passes `[esp+0x38]`, a local nothing in `0x49BE60` writes. DISASSEMBLED |
| `0x4B7EE0` | the anim-state frame: `state+8` is the sequence, tested first — a NULL sequence returns 0 (`0x4B7EE6..0x4B7EEB`), which `0x4C7580` then draws nothing for; otherwise `*(seq + 0x28 + 8·frame)` with the `u16` at `state+0` **unbounded**. `ret 4`. DISASSEMBLED |
| `0x4B7F30` | the sequence frame by index, `stdcall (seq, idx)`, `ret 8`: `0 ≤ idx < u16 *seq`, else 0; the frame is **`*(u32*)(seq + idx*8 + 0x28)`** — the table at `seq+0x28` is `{GAFFrame*, u32}` pairs, stride 8, and each `GAFFrame` header is followed by its own pixel plane (a frame's `+0x10` points at `frame+0x18` for an uncompressed frame). It reads the count **before** its NULL test on `seq`. DISASSEMBLED; the layout MEASURED 2026-09-29 (TA Zero, `ArmT1Solar` team sequence: frame 0 16x16 uncompressed at `f`, pixels at `f+0x18`) |
| `0x4C0310` → `0x4C0330` | **the flat fill**, `stdcall (ctx, xy[], n, colour)`, `ret 0x10`, any vertex count; fills in the colour word's LOW BYTE (`mov al,[esp+0x14080]` at `0x4C0678`). The chains and the clip are `0x4C7580`'s without the texture coordinates. `0x4C0330..0x4C06D1`, DISASSEMBLED |
| `0x4C7580` | **`GAF_DrawTransformed`** (its vertex roles, the half-open span and the `uv == NULL` window are in *`GAF_DrawTransformed 0x4C7580`* above). The walk, `0x4C7580..0x4C7A1C`: the FIRST vertex at the strict least y is the top and the first at the strict greatest y the bottom; the left chain runs backwards (`cur − 1`, wrapping to `n − 1`), the right forwards. An edge is skipped when it ends at or above the clip's top or does not go down; x starts at `(xa << 16) + 0xFFFF`, `dx = ((xb − xa) << 16) / dy` and the texture steps by the same **truncating** `idiv`; a clipped top advances all three in ONE multiply; each covered scanline appends one 40-byte row to that side's buffer, and the fill reads the buffers BY POSITION, row `i` at `max(ymin, T) + i`. All of it wraps at 32 bits and shifts arithmetically. DISASSEMBLED; the transcription reproduces the engine's pixels, MEASURED below |
| `0x4C7310` | the textured span, `0x4C7310..0x4C74E5`: `du = (uR − uL) / (xR − xL)` from the UNCLIPPED width, a left clip advancing u and v by one multiply, `xR` cut to the clip's right, a fill only when `xR − xL > 0`, one texel per pixel **copied whatever its value — the frame's key colour included** (`mov dl,[edi+ebx]` / `mov [esi-1],dl` at `0x4C74C7`..`0x4C74CE`, no compare between them; re-read 2026-09-26 for the GUI's quad stamps, which is why a `GAF_DrawTransformed` stamp crosses to the Vulkan lane as a box of indices and not as a keyed sprite). The texel's address by the frame's width (`0x4C73BD`, table `0x4C7500` into `0x4C74E8`): 8 inline; 16, 32, 64 and 128 through `0x4CD962`, `0x4CD91E`, `0x4CD8DA`, `0x4CD896`, which mask `v` shifted by `log2 w` and add `u` before the shift; any other width `(v >> 16)·w + (u >> 16)`. DISASSEMBLED |
| `0x4C6AE0`, `0x4C6B10`, `0x468D85` | the clip every one of these reads is the context's `+0x1C` rect, fetched through `0x4C6AE0`; `DrawGameScreen` sets it at `0x468D85` (`0x4C6B10`) to the viewport rect `main+0x37E27` — the frame packet's `vp_addr`. L and T are the first column and row drawn, R and B the first NOT drawn. DISASSEMBLED |
| `0x4C5E70`, `0x4C5FA0` | when a rasteriser is handed no context (`ctx == 0`) it builds one from the graphics globals through `0x4C5E70` and gives it back through `0x4C5FA0` on each exit; both effects draws always pass the frame's. DISASSEMBLED |
| `main+0x14383`, `+0x14387`, `+0x1438B` | `0x46BAE0`'s three scratch arrays, heap blocks the level load allocates at `0x491908..0x49195C` through the named allocator `0x4D83B0(name, size)`: **"TEMP XFORM PTS"** `0x960` bytes (`0x509298`; 200 rotated vertices at 12), **"TEMP PROJECTED PTS"** `0x640` (`0x509284`; 200 projected at 8) and **"ASSEM PTS"** `0xA0` (`0x509278`; one face's 20 points at 8). `0x46BAE0` bounds none of them, so a node of more than 200 vertices or a face of more than 20 writes past its block into the heap's next, and what it then draws is whatever that leaves. No stock model reaches them. Engine state on the game thread; the fork reads none of them. DISASSEMBLED, the names read out of `.rdata` |
| `0x49BE60` model cases | **the projectile pass poses every model projectile it draws, on screen or not**: rendertypes 1, 3 and 6 call `0x46BAE0` at `0x49C127` (the body), `0x49C16F`/`0x49C185` (the flame), `0x49C25E` and `0x49C482` after the LOS gate alone, and the rasterisers clip. Its one `0x4B6720` rect test (`0x49C1C7`) is rendertype 2's refraction sprite. The explosion pass tests the anchor first (`0x420C4A`) and so does the debris draw (`0x42123F`). DISASSEMBLED |
| `main+0x1B63` | the ten `PlayerStruct`s inline, stride `0x14B` (`0x49BEDF`'s `lea [edx+eax*2+0x1B63]` with `eax = id·0xA5`): what a unit's `+0x96` player pointer must be one of before the publisher follows it. DISASSEMBLED |

**What the publisher reads for a model**, on the game thread, per draw of the tick it gathers:
the node's `+0x04` vertex count (past 2048 it is not carried; with none and a face the walk
paints it is refused, because both draws then walk the faces over points nothing projected this
call), `+0x08` face count (a node with no face the walk paints draws nothing and carries no
model; past 512 not carried), `+0x0C`, `+0x24`, `+0x28`
and `+0x30`; per face `+0x00`, `+0x04`, `+0x0C` (every index checked against the vertex count —
the engine's walk does not), `+0x10`, `+0x18` and `+0x1C`; a sequence's `u16` count and its
`+0x28` frames (every index bounded by the count, `0x4B7EE0`'s unbounded one included); for a
piece the chain above, each link validated as data — the unit a stride-aligned member of the unit
array inside the slot count, the player pointer one of the ten `main+0x1B63` records — before the
next is read. A frame that is RLE-compressed is refused: `0x4C7310` copies the pixel plane as raw
rows. The unit that owned a piece is usually dead by the time it is read, and its slot may hold
another; the engine reads whatever the slot holds, and so does the publisher.

**A model is carried whole or not at all.** A face the engine draws nothing for is left out, and
that is the engine's picture: a flat face of fewer than three points (no span), a textured face
of other than four (never rasterised), a frame pointer that is NULL, `0x4B7EE0`'s answer for a
NULL sequence, or `0x4B7F30`'s out-of-range 0 (`0x4C7580` returns at `0x4C7597`). A face the
engine does draw and the publisher cannot carry — an index at or past the vertex count (so every
painted face of a node with no vertex), a flat face of more than 32 points, an index pointer that
is not one, an unreadable or RLE frame, `0x4B7EE0`'s index past its table, a piece whose logo
chain did not validate or whose sequence is NULL (`0x4B7F30` reads the count before its NULL
test, so the engine faults there) — refuses the whole model, and the record it belongs to is
taken back whole. **Past the engine's capacities** above (200 vertices or a 20-point face in `0x46BAE0`, a
25-point face in `0x4211D0`) the engine's picture is whatever the overrun leaves; the publisher
carries such a node as the arithmetic describes it, uncorrupted, up to its own bounds of 2048
vertices, 512 faces and 32 points a face.

**It poses only what a frame can show.** Before the rotation, and before it reads a face, the
publisher tests the model against the frame's reach — the published window at the zoom floor
plus the lead and the fog grids, the rect every frame drawn from that packet stays inside — with
a margin from the node's rest vertices (`2·r + 4` pixels, `r` their largest `|x|+|y|+|z|`), and a
model outside it is not posed, whatever its faces would have answered. It differs from the
engine only in cost: `0x49BE60` poses every model projectile, and none the publisher skips could
have put a pixel on the frame (MEASURED below). The reach and the projection keep the whole
pixel where the engine cuts it to 16 bits (`0x46BAE0` above), so they agree while the model is
within 32 768 px of the eye — on every stock map, the largest being 20 480 px square.

**MEASURED 2026-09-24** (paused, 1×, 1024×768, the engine's own draw with `fxown` off as the
oracle): the transcription paints the engine's pixels — `fx-rockets` 28 of 28, 18 of 18, 2 of 2,
15 of 15 and 19 of 19 model-only pixels; `big-battle` 1292 of 1352, 487 of 492 and 587 of 588,
the others where the engine's finished frame shows a health bar or a sprite's edge drawn after
the models (the comparison lays the models over that frame). The reach test, in a
test build that posed every model anyway and checked each posed vertex: over about 2 million
vertices of `big-battle`, none outside the margin its node's rest vertices give, and of about
150 000 models the test would have skipped, none with a pixel inside the reach.
