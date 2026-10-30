# Memory, lifetimes and teardown

The allocator, the `main` block, the per-map arrays, the destructors and the level teardown.

*Part of the [engine map](../exe-reverse-engineering.md). A function's entry is the section whose heading names it — the [address index](address-index.html) lists them: a new fact about a function extends its entry, in place; a new section is for an area nobody has mapped yet.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### The out-of-memory text — `0x49E700` [DISASSEMBLED + MEASURED 2026-09-24]

WinMain (`0x49E849`, in `0x49E830`) installs `0x49E700` as the allocator's new handler for the whole
process. On any failed allocation it appends its text to `ErrorLog.txt`, dumps the registers and
stack through a deliberate fault (`0x49E7E4` → `0x49E680`, whose `div dword [ebp-0x1C]` at `0x49E6B0`
divides by zero inside its own `__try`), shows the text in a system-modal box (`0x49E7FA`) and leaves
through `raise(SIGABRT)` (`0x49E807`). The text, "Out of memory! Your hard disk may be full" at
`0x509764`, is read at `0x49E7BD` (its length), `0x49E7CD` (the log) and `0x49E7F4` (the box), and
nowhere else. Two places clear the handler around one allocation: the save writer's compression
buffer (`0x4B3B75..0x4B3B8F`, `0x4B4146..0x4B422B`) and BIGSHOT (`0x495ABE`).

**The fix**, `fix_oom_message`: the three reads point at a static text that a call at the handler's
entry writes with `wsprintfA` (nothing allocates): the game is a 32-bit program that has used the
memory it can address, and `UNITINFOCount − 1` unit types are installed. That count is the unit
files found (`0x42AA77`) until the menu-time load's end rewrites it with the types it kept
(`0x42B2F6`), and a game load's end rewrites it with the types in play (`0x42D542`: the types
flagged `0x800000` at `+0x241`, a flag the sync clears for a type that did not sync, `0x46E1FE`).
**MEASURED**: 1500 generated types each carrying a 1 MB script: the dump names "Integer
Divide by Zero … at `0049e6b0`, Exception handler called in Out of memory handler" under our text
("1778 unit types are installed"), the box shows it, and OK exits.

### A large-address-aware exe's heap is above 2 GB — the range filters' top [MEASURED 2026-09-27]

The DLL's passes range-test every pointer they take out of engine memory before reading through it
(`ptr_ok` and its siblings, a filter on a value and never the reason a read is safe). Their upper end
is **the process's own top, `GetSystemInfo`'s `lpMaximumApplicationAddress`** (`tagpu_addr.h`), not a
2 GB constant; it is taken once, first thing in DllMain (`tagpu_addr_init`), and never written again. Retail's exe is not large-address-aware (Characteristics `0x10B`) and gets `0x7FFEFFFF`;
TA:ESC's sets `IMAGE_FILE_LARGE_ADDRESS_AWARE` (`0x12B`, [deep-ta-esc](../deep-ta-esc.html)), gets
nearly 4 GB under a 64-bit Windows or Wine, and its heap is placed up there: **the unit array began
at `0x9FD50020`** on a TA:ESC skirmish under Wine, against `0x0E8D0020` on retail. With the constant
every pass read that array as garbage and skipped every unit — the world drew no units in single
player or a network game while the sim ran them. The bound on what is read out of those pointers
(an index against the engine's own count, and so on) is unchanged; only the value filter moved.

## `main` is deliberately MISALIGNED, and it is redrawn at every launch — mapped by us

[MEASURED 2026-09-10, this project — `objdump -d -M intel` of the pristine build, plus the
arithmetic over all 1000 tick residues. Found by the landing review of G13u, which caught a
change that assumed the opposite.]

**Nothing at `main+0xNNN` can be assumed aligned, so no cross-thread access to an engine field
is atomic by virtue of its width — a READ as much as a write.** `0x41D920` is the only *function*
that stores into `ds:0x511DE8`, and it pads the allocation by a random amount first:

```
0x41D924  call ds:0x4FC0DC        ; GetTickCount  (confirmed in the import table)
0x41D92C  mov  ecx, 0x3E8         ; div by 1000
0x41D935  shl  esi,3 / sub esi,edx ; pad = (tick % 1000) * 7      -> 0..6993
0x41D93A  lea  edi,[esi+0x3924D]  ; malloc(0x3924D + pad)
0x41D965  add  esi, ebx           ; main = base + pad
0x41D9D5  mov  ds:0x511DE8, esi
```

It stores twice: `0x41D9D5` above, and `0x41D9E1` (`mov ds:0x511DE8, ebp` with `ebp` zero) on the
allocation-failure branch taken at `0x41D96E` — so **`main` can legitimately be NULL**, which is
why every reader in the fork tests it before use rather than out of habit.

`7 * n mod 4` walks every residue, so the struct's alignment is drawn afresh at each launch and
then **fixed for that session** — which is the worst possible diagnostic signature, because a
bug of this shape reproduces perfectly inside one launch and not at all in the next. Why the
engine does it is not established; the effect is a per-run jitter of every field's address.

For a dword field at `main+N` with `N ≡ 3 mod 4` — which every offset this project reads across
threads is (`0x142F3`, `0x142F7`, `0x1431F`, `0x1421F`, `0x14357`, `0x1435B`) — over the 1000
reachable pads:

| | share of launches |
| --- | --- |
| 4-aligned | **25 %** — exact, and independent of the allocator's own base |
| tear-capable on **Intel's** documented guarantee: the dword crosses a 64-byte line | **~4.7 %** — SDM 3A §8.1.1: a P6-or-later core keeps any access that fits inside one cache line atomic, aligned or not |
| tear-capable on **AMD's** documented guarantee: the dword crosses an aligned 8-byte boundary | **37.5 %** — APM vol. 2 §7.3.2: single loads and stores are "naturally atomic … as long as they do not cross an aligned 8-byte boundary" |
| misaligned at all | 75 % — **not a tearing condition on either vendor** |

*[CORRECTED 2026-09-11. Until then this section called the 75 % "the load-bearing number" and
quoted "about one launch in twenty" as the tearing rate. The first is wrong: both vendors document
atomicity for misaligned accesses that stay inside their unit, so misalignment alone tears nothing.
The second is Intel's figure only; the reference setup is an AMD part, and on its documented
guarantee more than a third of launches leave each of these fields tear-capable. Whether a Zen core
actually splits an access inside a cache line has not been measured here — assume the documented
figure.]* The cache-line figure assumes a 64-aligned malloc base, which nobody has established, and
`GetTickCount`'s ~15.6 ms granularity makes the reachable residue set coarser than 1000 values; the
8-byte figure has neither caveat, since `7n mod 8` walks every residue.

**The tear is not the mechanism behind any realistic failure, though.** Every multi-word engine
read — a `{begin, end}` pair, a `{buf, cols, rows, cells}` descriptor — is several loads that are
never atomic *as a set*, in 100 % of launches, aligned or not. That skew is what the audit in
[cross-thread engine reads](../cross-thread-engine-reads.md) is about; a torn single load is a rarer
variant of the same outcome. **The rule this gives:**

* "it is one aligned 32-bit slot, which x86 loads and stores atomically" is true of **our own
  statics** (the compiler aligns them — `tagpu_zoom.h`'s published view is fine) and **false of
  every engine field**.
* A torn **coordinate** is survivable when the consumer bounds it — which is why the eye and the
  scroll target are written from the render thread anyway, behind `clamp_pair()`.
* A torn **pointer, length or index** is not: it is a wild read. `main+0x142F3`/`+0x142F7` are
  dereferenced by the camera stepper at `0x41CA58` and `0x41CA95`, which is why G13u releases the
  camera follow from the **game thread** rather than storing zero into them from ours.
* A `lock`-prefixed store on our side does not rescue it, because the engine's own plain **split
  load** can still straddle an atomic store.

**The fog overlay draw `0x4848E0` has exactly one call site, `0x469D8E`, inside the per-frame
world draw.** That is what makes `tagpu_terrown`'s `terr_fogtick` — and everything hung off it,
including `tagpu_zoom`'s follow release — run at the DRAW rate and not the sim rate. It keeps
running with the sim paused, and it stops only when the game stops drawing. Measured on the
heartbeat's `ticks=` counter: **443/s at 1920x1080 and ~900/s at 1024x768 against a presenter
holding 60**, i.e. `DrawGameScreen` turns over roughly 7-15x per presented frame. *[2026-09-10.
A note in the ta-drive skill claimed the opposite — that pausing the sim stops it — and was
wrong.]*

**Where this may already have bitten — and the argument AGAINST the obvious suspect.**
`tagpu_fog_at`'s guard exists because the render thread faulted twice on 2026-09-03 reading the
fog descriptor `{u16* buf; int cols; int rows; int cells}` out of `*(main+0x1421F)` off a base of
`-9` = `0xFFFFFFF7` (earlier `-318` = `0xFFFFFEC2`), root cause never found. A torn `buf` looks
like the answer and probably is not: composing either value needs **three `0xFF` bytes**, and
neither operand of that store has an `0xFF` byte up there at any split offset — a userland heap
pointer's top byte is `0x00`–`0x7F`, and the value it replaces is the previous such pointer or
zero. No split offset composes either observed value out of two valid pointers, so the tear
reading survives only if the slot already held `0xFF` poison — in which case the interesting bug
is the freed-and-reused descriptor, i.e. a **lifetime** fault, not an alignment one. Both readings
are recorded in `fog-grid-sizing.md` §2. The test that separates them in one fault: log the other
three descriptor fields at the guard trip — a tear gives one bad field and three consistent ones,
a reused slot gives four that are garbage together. The defensive shape
already in `tagpu_native.c` is the one to copy for any multi-field engine struct read across
threads: bound every field AND check a **relation the builder guarantees** —
`cells == ((cols*rows + 7) & ~7)`, the round-up at `0x483C84` — because a tear almost certainly
breaks the relation even when each field looks individually plausible.

**A relation is a filter, not a proof, and the distinction matters here.** Two pointers into
unrelated allocations satisfy `(end − begin) % stride == 0` about one time in `stride`, so such a
check catches most skews and ships the rest — which by this project's own standard is the bug with
better odds rather than a fix. Use it as a cheap refusal on top of a real argument (a lifetime
fence, a thread), never as the argument itself.

**Swept and audited [2026-09-11].** The review sweep of 2026-09-10 found nine places where an
engine field is written by one thread and read by the other; the audit of them, writer by writer
against this binary, is [cross-thread engine reads](../cross-thread-engine-reads.md) §5, and the writers
it established are the next section here. The short form: the unit array's `begin`/`end` pair
(`main+0x14357`/`+0x1435B`) is the one open hazard, and its window is not the teardown — which
`tagpu_reclaim` fences — but the next level's **load**, where `0x4854A0` stores `begin`, memsets the
array, and only then stores `end`, with `end` never nulled in between (`0x4855D6` is its only store
in the binary). The fog descriptor, tile set, tile map, feature map, minimap surfaces and projectile
array are per-map allocations the fenced cascade frees and the load republishes in one store each,
with `LoadMap` writing the dimensions before the pointers, so they are safe by ordering; the
particle layers' vectors grow mid-play and were already catalogued as `tagpu_reclaim`'s next
client. The 2026-09-03 fog faults above predate the fence by three days (`55bc8e5` on 09-03,
`2062b19` on 09-06), and the descriptor is freed only by that cascade — consistent with the
lifetime reading, not proof of it.

## The per-map arrays — who allocates, who frees, and in what order — mapped by us

[MEASURED 2026-09-11, this project — `objdump -d -M intel` of the pristine build: every direct
store to each slot of `main` listed below, then the routines around them. Established by the
cross-thread audit ([cross-thread engine reads](../cross-thread-engine-reads.md) §4), which needed to
know, for each field the render thread reads, whether it changes per tick, per map, or never.]

**The two routines that bracket a level.** The teardown cascade
([`0x491B60`](#0x491b60-the-level-teardown), the whole call list there) frees the per-map arrays in this order: `0x485980` the unit array (call at `0x491B95`),
`0x471DE0` the particle layer table (`0x491B9A`), `0x464A00` the players' LOS counters, AI
objects and `+0xEC` records (`0x491BA9` [DISASSEMBLED 2026-09-28]), `0x466AA0` the minimap
surfaces (`0x491BAE`),
`0x483DD0` the map (`0x491BB3` — it reaches the feature teardown `0x422170` and frees the tile
set, tile map, feature map, wreck records and fog descriptor), `0x42DB90` the model templates
(`0x491C21`), `0x499A80` the projectile array (`0x491C30`). The level load — `LoadGameData_Main
0x4917D0`, the routine that calls `LoadMap` at `0x4918C0`, called at `0x497581` from the loader
body `0x497180` **on the loader thread** (see "The in-play publish point, the loader thread and the
load flags", traced 2026-09-12) — allocates in this order: `0x471D90` layers
(`0x4918B1`), `0x499A30` projectiles (`0x4918B6`), `0x483610` `LoadMap` (`0x4918C0`), `0x4854A0`
the unit array (`0x4918D4`), and later `0x4669B0` the minimap (`0x4919C3`) and `0x464990` the
players' LOS counters (`0x4919C8`). Between the two the
render thread is live: the reclaim post hook clears its flag as soon as `0x491B60` returns.

**The LOS counters are allocated in the level load and nowhere else** [DISASSEMBLED 2026-09-24,
the pristine build]. `0x464990` walks the ten players and calls `0x464700` (at `0x4649B1`) for each
whose byte `Players[i]+0x73` (`[main+0x1BD6+i*0x14B]`, tested at `0x4649AC`) is non-zero, then
calls `0x4648E0`. `0x464700` frees the block's old buffer (`0x464855`, `0x4B4F20` on
`[player+0x7C]`), stores the dimensions as the PLOT size halved (`w` at `0x464849`, `h` at
`0x46484F`, `+0x80`/`+0x84`), allocates `w*h` rounded up to 8 (`0x46486F`, `0x4B4F10`; the
rounded size at `+0x88`), stores the pointer at `+0x7C` (`0x46487B`) and zeroes it (`rep stos`
at `0x46488F`). The image holds one direct call of each (`0x4919C8` for `0x464990`, `0x4649B1`
for `0x464700`) and no other reference to either address; a call through a table or a register
is not excluded by that search. Before the counters `0x464700` stamps `+0xF0`/`+0xF4`/`+0xF8`
with GameTime (`0x46470C..0x464765`); zeroes current, production, expense and storage
`+0x8C..+0xA8`, the six totals and the share limits `+0xE4`/`+0xE8` (`0x46473F..0x4647C5`),
leaving the base storage `+0xDC`/`+0xE0`; allocates the `0x34`-byte `+0xEC` record when there is
none (`0x4647CD..0x4647D7`) and resets it through `0x401070(+0x146)` (`0x4647EA`, which zeroes
its `0x34` bytes and stores at `+0x30` a pointer to the player record of that index,
`main+0x1B63 + idx·0x14B` (`0x40109B..0x4010A3`), the owner record that `0x464C60` (`0x464D2E`)
and `0x401260` read back; `0x401070`'s other caller, `0x485D0A`, does the same for a unit's
`+0xBC` record); clears `+0x149` bit 0 (`0x4647EF`); and zeroes
`+0xFC`/`+0xFE`/`+0x104`/`+0x106`, setting `+0x100`/`+0x102` to −1 (`0x4647FB..0x46481E`). After
them it calls `0x480190(player)` (`0x464898`) and, for every record but an active type 3
(`0x46489D..0x4648A5`), builds the two per-seat AI objects: `0x3D` bytes through `0x408CB0` into
`+0x74` (`0x4648A7..0x4648C1`) and `0x40B320(+0x146)` (`0x4648CD`) [DISASSEMBLED 2026-09-28]. A
seat that is to get resources, sight and those objects needs its `+0x73` before `0x464990` runs.

**The LOS counters and MAPPED both end in the teardown, the counters first** [DISASSEMBLED
2026-09-24 (MAPPED) and 2026-09-28 (the counters), the pristine build]. MAPPED (`main+0x14273`) is allocated at `0x483CF6` in `LoadMap`
and freed at `0x483E70` (`0x4D85A0` on `[main+0x14273]`) by the map-free routine `0x483DD0`,
inside the teardown. The teardown frees the LOS counters first: `0x464A00`, whose one caller is
`0x491BA9` in `0x491B60` (before `0x466AA0` and `0x483DD0`), walks the eleven records
`main+0x1B63..+0x2851` and for each frees `+0x7C` (`0x464A2D`) and zeroes
`+0x7C`/`+0x80`/`+0x84`/`+0x88` (`0x464A24..0x464A38`), calls `0x4801F0(record)`, frees the AI
objects when `+0x74` is set (`0x40B390(+0x146)`, then `0x408F10` and the free, `+0x74` zeroed at
`0x464A6B`), and frees and nulls `+0xEC` (`0x464A6E..0x464A7E`) [DISASSEMBLED 2026-09-28].
`0x464700` frees whatever `+0x7C` holds at the next level's load (`0x464855`), and the player
record's destructor frees it too. The constructor
`0x463BE0` zeroes `[this]`, `+0x73`, `+0x80`/`+0x84`/`+0x88` and `+0x7C`, and allocates `0xB9`
bytes into `+0x27` (zeroed with `rep stos`); it is called at `0x41D98B`, a loop of eleven (`ebx =
0xB`) over the records from `main+0x1B63` at stride `0x14B`, and at `0x454436` on a stack-local
record at `[esp+0x2E0]`. The destructor `0x463C40` frees `[this+0x27]` and `[this+0x7C]` through
`0x4B4F20` and nulls neither; its two calls, `0x454538` and `0x454607`, destroy that stack-local
record, whose `+0x7C` the constructor zeroed and `0x464700` (whose one caller walks `main`'s
records) never fills. So the local player's counters, which the builder `0x4843C0` and
`tagpu_fogwide` read (`0x4843F0`, the block at `main+0x1B63+id*0x14B+0x7C`), live from the level's
load to its teardown, and MAPPED from the map load to the teardown: every in-play draw falls inside
both. `tagpu_fogwide` reads them from inside every in-play draw, at the fog site or, on the
Vulkan renderer, in the publisher's `after` (gpu-status §2.3e).

**Each per-map field, who publishes it and who frees it:**

| Field | What | Published by the load | Freed / nulled by the teardown |
| --- | --- | --- | --- |
| `main+0x14351` | the unit array's **slot count**, `u16` = `10·[main+0x37EE6] + 1` (the name is <span class="pill pill-warn">INFERRED</span>; the arithmetic is not) | `0x4854EF`, **before** `begin` | untouched |
| `main+0x14357` / `+0x1435B` | unit array `begin` / `end`, stride `0x118` | `0x4854A0`: `MEM_Alloc 0x4D83B0` of `count·0x118`; `begin` at `0x485525`; `rep stos` over the whole array; two more allocations (`+0x1435F` at `0x485596`, `+0x14363` at `0x4855AA`); `end = begin + (count−1)·0x118` at `0x4855D6` | `0x485980`: `MEM_Free 0x4D85A0` at `0x485A17`, `begin = 0` at `0x485A27`, `+0x1435F`/`+0x14363` nulled. **`end` is never nulled — `0x4855D6` is the only store to `+0x1435B` in the binary** |
| `main+0x14233` / `+0x14237` | map W / H in 16-px tiles | `LoadMap`, through `ebp = main+0x141FB` (`0x483627`): `[ebp+0x38]` at `0x483881`, `[ebp+0x3C]` at `0x483892` — **before every pointer below** | untouched; stale until the next load |
| `main+0x1428B` | `TILE_MAP` | `[ebp+0x90]` at `0x483969` | `0x483DD0`: nulled at `0x483EED` |
| `main+0x14287` | `FeatureMap` | `[ebp+0x8C]` at `0x4839A2` | nulled at `0x483EE2` |
| `main+0x14283` | `TILE_SET` `{count, pixels}` | `[ebp+0x88]` at `0x483B68` | nulled at `0x483ECA` |
| `main+0x1421F` | fog descriptor `{buf, cols, rows, cells}` | `[ebp+0x24]` at `0x483C28` (struct `0x483C03`, buffer `0x483C96`) | `free 0x4B4F20` twice at `0x483F06`/`0x483F0F`, nulled at `0x483F1C` |
| `main+0x1420B` | wreck records, stride `0x30`, **a fixed pool for the level: 2048 in stock, 8192 under the raised limits** ([The wreck pool](map.md#the-wreck-pool-and-a-feature-paid-for-and-left-standing)) | `0x421F20`, through `esi = main+0x141FB`: `MEM_Alloc(0x18000)` at `0x421F29`/`0x421F39`, pointer into `[esi+0x10]` at `0x421F47`, `rep stos` of `0x6000` dwords, then a doubly-linked free list threaded through every record — `0x421F5F..0x421F7E` steps `eax` by `0x30` from 0 to `0x18000` writing `u16 [rec+0x00] = i+1` (next) and `u16 [rec+0x02] = i−1` (prev), so **`0x18000 / 0x30` = 2048 records, 0..2047**; `0x421F8D` terminates the prev chain at record 0 and `0x421F94` the next chain at `+0x17FD0` = record 2047. `[esi+0x18]`/`[esi+0x1C]` (`main+0x14213`/`+0x14217`) are two live-list heads, both −1; `[esi+0x20]` (`+0x1421B`) the free head, 0. The four size constants (`0x421F2A`, `0x421F41`, `0x421F7A`, `0x421F97`) are limits-table sites | `0x4221F1`→`0x4221F8` frees it and `0x422214` **nulls** it, inside the feature teardown, after `0x42219A`'s loop has walked both live lists calling `FreeObjectState 0x45AAA0` on each record's `+0x04` |
| `main+0x141F7` / `+0x141F3` | projectile array / live count, stride `0x6B` | `0x499A30`: `MEM_Alloc(0x7D64)` = **300 slots** (the `push` operand at `0x499A32`; 3000 under the raised limits), pointer at `0x499A49`, count zeroed at `0x499A6A`; the count is rewritten by the sim at 13 sites (`0x49AF76` … `0x49DF3F`) | `0x499A80`: `MEM_Free`, nulled at `0x499A9A` |
| `main+0x38D77` | particle layer table, 10 × `{…, begin, end}` | `0x471D90`, pointer at `0x471DCB`; each layer's vector and every object's sub-vector grow **mid-play** (`0x4732E0`) | `0x471DE0`, nulled at `0x471E97` |
| `main+0x142DB` / `+0x142DF` / `+0x142E3` | minimap composite / fogged base / scaled map (8bpp offscreens) | the minimap build `0x4669B0` (one caller, the level init's `0x4919C3`): `0x4669DA`, `0x466A05`, `0x46682E` (in `0x466780`, its first call); the fog composite `0x466C20` rewrites `+0x142DF` in place | `0x466AA0` (one caller, the teardown's `0x491BAE`): `0x466AF5`, `0x466AE9`, `0x466ADD` |
| `main+0x37E37` / `+0x37E3B` | view W / H | `0x49821D` / `0x498237` (the view-size setter), plus the fork's `tagpu_vpwide` from the render thread | — |

The map-free routine also nulls `+0x141FB`, `+0x141FF`, `+0x14203`, `+0x14273`, `+0x14293` and
`+0x14297` (`0x483EA7..0x483F2E`). **Negative results:** no store to `+0x14233`/`+0x14237` exists
outside `LoadMap`'s `ebp` form; no routine other than `0x4854A0` and `0x485980` touches `+0x14357`;
nothing resizes the fog grid between map loads.

## The unit-death path, the object destructor and the level teardown — mapped by us

Mapped 2026-09-06 to close the render thread's use-after-free on a dying unit's model object
([Thread-safe destruction](../thread-safe-destruction.html)). Everything here is **[VERIFIED]** by
disassembly of the retail exe unless marked `[INFERRED]`; the fork's `tagpu_reclaim.c` patches
exactly two of these addresses (`0x45AAA0`, `0x491B60`) and reads none of the others at runtime.

### `0x4866D0` — the unit destructor `[INFERRED name: UNITS_Destroy]`

`stdcall (rec, mode)`: frame `sub esp,0x68`, two stack arguments (a death-message record, a mode),
epilogue from `0x486E59`, `ret 8` at `0x486E60`. **Two callers, and an E8/E9 scan finds no
other**: `0x486679` inside `Send_UnitDeath 0x4864B0` (mode 1, on the record Send_UnitDeath has just
built, `0x486672..0x486679`) and `0x455423` in the `0x0C` case `0x45541C` of the message dispatcher
(mode 0). So every peer runs it for every death: the victim's owner from Send_UnitDeath, every
other peer from the `0x0C`. **Deaths do not nest**: nothing the destructor calls calls it, and a
passenger its cargo loop kills dies later, through the reaper (below) [DISASSEMBLED].

**The slot.** At entry `[rec+1]` (word) is the unit id and the unit is `main+0x14357` base
`+ id·0x118` (`0x4866EE..0x486703`); nothing bounds the id against the array. Id 0 makes `esi = 0`
(`0x4866E5..0x4866EC`), so the alive test at `0x486706` (`[esi+0x110] & 0x10000000`) reads
through NULL; a slot that is not alive returns (`0x486710` → `0x486E59`). Impure's bound (B3, `tagpu_patches.c`
`wire_s0c`) is a five-byte branch at `0x4866E5` that drops id 0 and an id past the array to that
epilogue — stock's `push edi` at `0x4866E4` stays, so the epilogue's `pop edi` balances (the spans
are [network.md](network.md)'s B3 list); TADR's `NullUnitDeathVictim` patches `0x4866E8` for the id-0
case only.

**The body, in order** (a call with the kind, the severity and the corpse all 0 — C4's replayed
death, mode 0 — runs every step but the statistics, `Killed`, the explosion and the corpse):

- `0x486748`: a temporary sight record when the owner is the viewed player.
- `0x486753..0x486787`: the killer `+0xF0` and the killer's player `+0xF4`, from the record (*Who
  counts a kill*, below).
- The objective's death listeners `0x4904C0` (`0x486799`), which do nothing unless the map's `.ota`
  carries campaign condition keys; the order purge `0x439EB0`; the reply queue purged of the unit
  (`0x47F8C0`, `0x4867A7`); the control-group setter with −1 (`0x4867AF`); the projectile purge
  (`0x4867B5`).
- The cargo (below): a carried unit detached (`0x4867BA..0x4867CB`), then the cargo loop
  `0x4867D0..0x48682A`.
- The grid's clear and the bucket's unlink, `0x47CBD0` at `0x48682D` (→ `0x47D0E0`); the sight
  removal under true LOS (`0x486845`).
- **`Killed`, in mode 0 alone** and when the severity `rec+9` is above 0 (`0x48684A..0x48685A`; the
  severity read signed, `movsx` and `jle` at `0x486855`): `0x4B0A70("Killed", 0, run-now 1, argc 1,
  severity, 0, 0, 0)` at `0x486877`. The paths that skip it (`0x486853`, `0x48685A`) join it at
  `0x48687C`, so nothing after relies on what the call leaves in `eax`, `ecx` or `edx`. The owner's
  peer ran `Killed` already, as Send_UnitDeath's query ([cob.md](cob.md), *A death's Killed*).
- The statistics by kind (`0x486880`, *Who counts a kill*, below); the reclaim credit
  (`0x486C9E..0x486D1E`); the death explosion (`0x486D24..0x486D50`); the corpse
  (`0x486D55..0x486D6F`).
- The tail, in this order:

| addr | instruction | what |
|---|---|---|
| `0x486D75` | `call 0x489740` | `FreeUnitOrders`: the references to the unit unlinked |
| `0x486D8A` | `call [vt+0x50]` (1) | delete the COB script object `unit+0x9A` (the deleting destructor `0x485E30`); nulled at `0x486D8D` |
| `0x486D93..0x486D9B` | `mov eax,[esi+0x9E]`; `cmp eax,ebx` at `0x486D99`; `je` | a NULL model skips the free |
| **`0x486D9E`** | `call 0x45AAA0` | **`FreeObjectState(unit+0x9E)` — the model object is freed here** |
| **`0x486DA3`** | `mov [esi+0x9E],ebx` (0) | **the pointer is nulled — the instruction after the free returns** |
| `0x486DB1` / `0x486DB7` | `call 0x43DD10`; `call 0x4B4F20` | `FreeMoveClass` + free of `[unit+0]`, nulled at `0x486DBF` |
| `0x486DC7` | `mov word [esi+0xA6],0` | the unit's type (the UNITINFO index, the model it draws) cleared: 0 is what first-free's test at `0x486036` takes as a free slot. B4 stamps the free just before it, at `0x486DC1` |
| **`0x486DCE`** | `and ebp,0xEFFFFFFF` → `[esi+0x110]` | **the alive bit `0x10000000` cleared — after the free** |
| `0x486DE8` | `and al,0xCF` → `[esi+0x110]` | bits 4 and 5 (the selection) cleared |
| `0x486DF6` | `mov [esi+0x92],[main+0x1439B]` | the type def reset to the type-0 def |
| `0x486DFC` | `dec word [player+0x144]` | the owner's live-unit count, which both creates increment with the dword `+0x140` (`0x486187`/`0x48619C`, `0x486322`/`0x486338`) |
| `0x486E09..0x486E54` | at a count of 0 | the elimination: `0x450380([owner+4])` at `0x486E32` when `0x435100` answers 3 (a network game), `0x47BD70(owner)` at `0x486E54` when it answers 2 (a skirmish: the CRT `rand` picks a text, `0x47BD96`, the message ring is written, `0x463CA0` at `0x47BDD5`, and "MessageArrived" plays; `ret 4` at `0x47BDE2`) |

HP `+0x108` and the owner `+0x96` are left as they were [DISASSEMBLED]. The type-0 def is named
"None" and its maxHP `+0x1FA` is 0 (MEASURED 2026-10-09), so an emptied slot that keeps a positive HP
is one a reader dividing by maxHP must never be handed: the draw's health bars do
([units.md](units.md#the-draws-unit-list-hotunits-its-cull-0x48bae0-and-its-readers-disassembled-2026-10-09)).

So a reader that gates on the alive bit sees the object freed while the bit still reads set
(measured live: dead units logged `st=80284101` with `unit+0x9E` already null). Re-reading
`unit+0x9E` is the correct guard; re-reading the bit is not.

**The cargo, and the corpse** [DISASSEMBLED + MEASURED 2026-09-25]. A unit that is itself cargo
(`+0x86`, the transporter) is detached first (`0x48AAC0(unit, 0, −1, 1)` at `0x4867CB`). While the
unit carries cargo (`+0x8A`, the first passenger), the loop deals that passenger 30 000 (`0x7530`)
through `0x489BB0(attacker, victim, damage, kind, 0)` at `0x48680B`, the attacker its own last
attacker `+0xF0`, the kind 3 when the record's kind nibble is 3 and 6 otherwise
(`0x4867DA..0x4867EC`), reloads `+0x8A` (`0x486810`) and detaches it (`0x48681D`). The loop runs on
every peer, and both detaches broadcast a `0x0A` from every peer that runs them
([units.md](units.md#what-a-death-sends-and-who-removes-a-player)). `0x489BB0` applies the hit
through `0x489CE0` (`0x489C89`). A hit the receiver accepts writes its kind into the victim's
`+0xF5` (`0x489DAC`); one it drops at `0x489D39..0x489D53` (no unit, not alive, bit 14 already set,
`0x489D45`, `0x489D50`) does not, so such a passenger keeps its own kind; nor does a repair (kind
`0x0A`, which adds HP and returns at `0x489D8A`). It sets the pending bit (`+0x110` bit 14,
`0x489EE2`) only when the HP reaches 0 and the victim's player is local (`0x489ECC..0x489ED8`), so
a passenger dies of the loop only on its owner's peer. **Kind 6 comes from nowhere else**: of
`0x489BB0`'s twelve callers the others pass 3, 4, 5, 9, 10, 11 or the weapon kind 1 or 2
(`0x499E37`). The unit tick reaps a pending-death unit through `Send_UnitDeath 0x4864B0(unit,
+0xF5)` (`0x48AFD1`).
`Send_UnitDeath` takes the severity as `(100·(−hp)/maxhp + byte +0xF7) / 2`, clamped to 1..100
(`0x48655E..0x4865AB`; maxhp the def's `+0x1FA`), and asks the `Killed` script for the corpse type
(`0x4865C3`). Kind 7 skips the script with corpse type 1 and severity 0; kinds 4, 5 and 9, and a unit
with HP left, give 0 and 0; a nanoframe (`+0x104` not 0.0) gives corpse type 0. The `0x0C` record
carries the severity at `rec+9` and the kind and corpse type as the high and low nibbles of
`rec+0x0A` (`0x4865E9..0x486621`). The destructor hands a corpse type above 0 to `0x486360(unit,
type, kind ≠ 7)` (`0x486D55..0x486D6F`), which starts from the def's corpse feature `+0x1BC`, steps
`type − 1` times through each feature def's FeatureDead (`+0xF4`; the defs at `main+0x1426F`,
stride `0x100`), and spawns nothing once the index is `0xFFFB` or above; otherwise it spawns at the
unit's footprint cell (`0x481550(+0x76, +0x78)`, `0x423C50` at `0x4863F9`). Its other four reads of
`rec+0x0A` take the high nibble alone (`0x4867DA`, `0x486880`, `0x486CA2`, `0x486D42`)
([map.md](map.md#the-demo-recorders-features-the-grid-the-wreck-pool-and-the-calls-that-change-them-disassembled-measured-2026-09-30),
*What a playing's corpse call rests on*).

So a carried unit dies at severity 100 whatever its HP was. ARMSTUMP's `Killed` gives corpse type 3
above severity 50 (`tacob decompile ARMSTUMP`), and `armstump_dead` (feature 123) → `armstump_heap`
(339) → `0xFFFF` (`+0xF4` read live), so it leaves nothing — which is why an ARMSTUMP killed inside
its ARMATLAS over land left no wreck record in B2's measurements: by design, not a lost wreck. A
cargo type whose `Killed` gives 1 or 2 at severity 100 would spawn at its own `+0x76`/`+0x78`; that
path was not measured.

**Who counts a kill, and why two peers can disagree** [DISASSEMBLED 2026-09-26; section B's
[B8 plan](../tadr-port/sim-fixes.html)]. Every peer runs this destructor for every death: the victim's
owner from `Send_UnitDeath` (mode 1), every other peer from the `0x0C` case (mode 0). The record's
fields reach the unit on every peer before the stats are counted. `0x486753..0x486778` sets
`+0xF0` to the unit at `rec+7` (0 → NULL; the scaling is `0x486760..0x486775`). `0x48677E..0x486787` sets `+0xF4` to `0x44FE40([rec+3])`.
`Send_UnitDeath` wrote `rec+3` as `0x44FFD0(+0xF4)` at `0x486625` and `rec+7` as the killer's `+0xA8`
at `0x48662E..0x486646`. `0x44FFD0(index)` returns the player record's DirectPlay id `+4`, or −1 for an
inactive record; `0x44FE40(id)` returns the index whose `+4` matches, or 10. So a record's killer and
killer's player resolve to the same unit and player on every peer. The kind (`rec+0x0A >> 4`)
selects the stats through the jump table `0x486E64` (`0x486880..0x486892`, index kind − 1):

| kind | target | counts |
|---|---|---|
| 1 (a weapon), 6 (a dying transport's cargo) | `0x4868B3` | losses, then kills |
| 5 (the unit reclaim, `0x404981` and `0x414B8D`) | `0x486899` | as 1, after refusing `+0xF4` of 10 or equal to `+0xFF` |
| 3 (self-destruct, defeat) | `0x4869F5` | the victim player's losses only (`0x486A33`), and its `+0x106` for a unit of its side's commander type (`0x486A6F..0x486A8C`); nothing when the controlled player's byte `+0x129` + the victim owner's `+0x146` is set (`0x486A03..0x486A31`) [DISASSEMBLED 2026-09-28] |
| 2, 4 (ownership change), 7..11, 0 | `0x486A98` | nothing |

On the counting path:

- `0x4868C1`: the victim's player `+0xFE` (losses) + 1, ungated.
- `0x4868C8..0x486906`: the killer's player `+0xFC` (kills: `main + idx·0x14B + 0x1C5F`) + 1. Gated on
  `+0xF4 ≠ 10`, **the unit's `+0x104 == 0.0` (`0x4868D3`)**, and `+0xFF ≠ +0xF4`.
- `0x486915..0x486996`: `_stricmp 0x4F8A70` compares, case-insensitively, the unit's type name with a name at `main+0x37F5F`
  indexed by its player's side [INFERRED: the side's commander]. On a match, the killer's player
  `+0x104` + 1 (`0x486981`) and the victim's player `+0x106` + 1 (`0x486996`).
- `0x48699D..0x4869CA`: **the unit kill count** — `inc word [ecx+0xB8]` at `0x4869CA`, `ecx` the
  killer `+0xF0`, the kill count's one increment. Gated on `+0xF0` non-NULL
  (`0x48699D..0x4869A5`), **`+0x104 == 0.0`** (`fld [esi+0x104]; fcomp [0x4FD6F8]`,
  `0x4869A7..0x4869B8`; the dword at `0x4FD6F8` is 0), and the victim's owner `+0xFF` ≠ the
  killer's player `+0xF4` (`0x4869BA..0x4869C8`).

Two more reads of `+0x104` come after the stats. `0x486C9E..0x486D1E` (kind 5 with a killer) credits
`(1.0 − +0x104) × def+0x18A` to the killer's `+0xD4` at `0x486CBD`. The credit is scaled by a factor that
depends on `main+0x37EEE` when the killer's `+0xEC` record is active and its `+0x73` is 2. [INFERRED: `+0xD4` is the
reclaimer's metal intake, `def+0x18A` the metal cost.] `0x486D24..0x486D50` runs the death explosion
`0x49B000` only when `rec+9 > 0` and `+0x104 == 0.0` (`0x486D2F`). The explosion's damage is computed
only on the owner's peer (C's evidence), so elsewhere it is a picture. The severity `rec+9` is read
signed twice: for the `Killed` script in mode 0 (`0x486855`, `movsx`, `jle`) and for the explosion
(`0x486D28`, `test`, `jle`). The `0x0C`'s entry in the dispatch table `0x455F84` is at `0x455FAC`
(→ `0x45541C`).

So the only per-peer input to all four gates is **that peer's copy of the victim's `+0x104`**. The
owner holds its own value; a copy's value comes from exactly these writers:

- **`CreateFromNetwork` makes every copy unfinished.** It calls `0x485A40(unit, pos, finished = 0)`
  (`0x486299`). With the flag clear, `0x485A40` sets `+0x104 = 1.0`, `+0x100 = 0` and HP 0
  (`0x485B27..0x485B37`). A set flag gives `+0x104 = 0` and HP = def `+0x1FA` (`0x485B09..0x485B25`).
- **The owner's `0x12` finishes it.** `0x4560C0(builder, built)` sends `{0x12, u16 built, u16
  builder}`. The receiving case `0x4555BA` (dispatch slot `0x12` of `0x455F84`) calls
  `0x41B8D0(builder, built)` at `0x4555F1` or `0x455610`, which clears `+0x104` (`0x41B933`) only when
  the builder is alive and **its def has a build list, `+0x156`** (`0x41B8F1..0x41B8FF`).
  - The owner's local create `0x485F50` takes `finished` as its sixth argument (`0x486075`, handed
    to `0x485A40` at `0x4860A0`). It sends the `0x09` (`0x456050` at `0x486115`), and for a finished
    unit whose def `+0x22F` is 0 it sends a `0x12` **naming the unit as its own builder**
    (`0x4560C0(unit, unit)`, `0x48611A..0x48612A`). So a finished create of a type with no build
    list is refused on every receiver, and its copies stay `+0x104 = 1.0`, HP 0. A finished type
    whose `+0x22F` is not 0 gets **no `0x12` at all**, builder or not: 1 is the value that
    allocates a mover at `0x4860BB..0x4860ED` [INFERRED: every mobile unit]. Its copies stay
    unfinished the same way, which is why the ARMCK victims of the measurements below, builders,
    lagged.
  - The callers passing `finished = 1` (DIS): `0x4977BB` (the level load's commanders), `0x497002`
    (the level load), `0x4653D9` (the Deathmatch respawn), `0x488700` (the capture, an ownership
    change) and `0x488462` (the map's placed units). After the sends, `0x488700` sets the new unit's HP and `+0x104` either from the `0x14`
    record (`0x488729..0x48873D`, the fraction `fild` from an integer the sender truncated at
    `0x488614..0x488623`) or from the old unit (`0x488774..0x48878B`). `0x48718E` (the saved-game restore), `0x41794F` [role not traced], and
    `tacli`'s scenario applier (`tagpu_scenario.c`, `create_units`). The build paths `0x4028EA`,
    `0x403D5B`, `0x405104` and `0x41409B` pass 0.
  - **Four callers set the new unit's state after the create returns**, so after its `0x09` has
    left (DIS): `0x488700` as above; `0x488462` (the map's placed units, `0x488310` from the loader
    at `0x497B40`), HP = `def+0x1FA × (i16)[esi+0x1A] / 100` at `0x48848E..0x4884A5` (the divide is
    `mul 0x51EB851F; shr edx,5`); the resurrection `0x405104` in the order code `0x404DB0`, which
    creates unfinished, links the unit to the order (`0x489690`, a leaf), looks the wreck up
    (`0x4815F0`, `0x421E60`, leaves) and, when it is there (`0x405153`), removes it, sends a `0x0F`
    in a network game (`0x405210`) and writes `+0x104 = 0.0` and HP 1 (`0x405219`, `0x405226`) —
    a straight line from `0x405164`; its other paths, no unit linked (`0x405119`, the fall-through of
    the test of the order's link `+0x16` at `0x405117`, before its own `0x47F780` at `0x405121`) and no wreck
    (`0x405155`, return 8), leave the unit as created; and the saved-game restore `0x487080`,
    HP at `0x4871B5`, straight after the create, and `+0x104` at `0x48727C`, after it has restored
    the units its record names through itself (`0x4871DD`, `0x48720D`) and attached (`0x48AAC0`). The restore is reached only
    from a saved game's load (`0x497B29` → `0x432610` → `0x486FD0`) and from itself.
  - **What one local create sends** [DISASSEMBLED 2026-09-26, every direct call from `0x485F50`
    followed]. Every NULL return (the jumps to `0x4861BD` from `0x485F7C`..`0x486043`, and
    `0x48605A`) comes before its first call (`0x4860A0`), so a NULL create sends nothing. A unit
    returned sends the `0x09` (23 bytes, `0x486115` → `0x4560AE`), the self-named `0x12` (5,
    `0x4560F9`) as above, and through `UNITS_SetStateMask 0x48B090` (`0x486148`, for a def with
    `+0x241` bit 18) a `0x11`. The `0x12`, that activation and a def with `+0x241`
    bit 24's `+0xF5 = 7` and `+0x110` bit 14 (`0x48614D..0x486171`) all sit behind `finished`
    (`test edi,edi` at `0x48611A`, else to `0x486177`); every create runs the sight stamp
    `0x482AC0` (`0x486178`) and counts `+0x144` and `+0x140` (`0x486187`, `0x48619C`)
    [DISASSEMBLED 2026-09-28]. The create calls `0x48B090` as `0x48B090(1, 1)`, which newly sets
    bit 0 only, so of its per-bit paths only bit 0's runs: the unit's `Activate` script started
    by name through `0x4B0940` (`0x48B106`, the name at `0x501280`) and reply kind 3 queued
    (`0x48B110` → `0x47F780(unit, 3, 0)`), which sends nothing; its bit-2 path
    (`0x48B16E..0x48B1AA`, reply kinds 14 and 15 and the `call [eax]` loop at `0x48B195`) is not
    reached. Then the `0x11` (4, `0x48B1F3`, a local player's unit). **So a create's direct calls
    send at most three messages**: 32 bytes in stock, and 74 in Impure's build, where the `0x09`
    goes as B8's 65-byte `0x4A`. B8's queue holds four messages and 92 bytes, a bound that counted
    a `0x13` this path never sends. Not followed:
    the COB interpreter a started script runs in (`0x4B0DA0`, 19 callbacks, and `0x4B1C00`, 4), and
    `0x49059A` and `0x4905BC` (`0x490580`, at `0x4861AE`) [INFERRED: a unit's script calls]. A
    Create or Activate script can send a `0x13`, `0x11` or `0x0A` through the host vtable `0x4FD698`
    [INFERRED], which that count does not cover: B8 sends it through its late path.
  - **The send `0x451DF0`** (`stdcall(net, msg, len)`, `ret 0xC`, 1 when sent) begins `push ebx;
    push ebp; mov ebp,[esp+0xC]` (`53 55 8B 6C 24 0C`): six bytes on an instruction boundary. It has
    57 direct callers and no branch into `0x451DF1..0x451DF5`. It sends only in a network game
    (`main+0x2A44` bit 0, `0x451EAB`) and from a local player's record (`+0x73` 1 or 2).
  - The owner's completion `0x41B8D0` sends the `0x12` for a local unit (`0x41BA0E..0x41BA26`), so
    a unit built by a builder is finished on every copy when the message lands.
  - `0x12` is refused in net state 5 like `0x09`: `0x451FD0` gives it mask 4 at `0x452029` (and
    `0x11`, `0x14`, `0x16` at `0x452023`, `0x45202F`, `0x452035`).
  - **The case bounds neither index**: `rec+1` and `rec+3` are scaled into the unit array at
    `0x4555C4..0x4555E2` and `0x4555FB..0x45560C`. B3's census of the receivers did not include it.
- **The owner's round robin refreshes it**, at most once every N owner ticks.
  - The sender `0x48B200` writes 0 for a `+0x104` of exactly 0.0 (`0x48B246..0x48B257`, `0x4FD748`).
    Otherwise it writes `1 − ftol(+0x104 × −254.0)` (`0x48B259..0x48B271`, `0x4FD74C`), which is 1..255:
    a fraction left never rounds to finished.
  - The receiver `0x48B3F0` stores `value × 1/255` (`0x4FD750`) when it differs
    (`0x48B4B9..0x48B4E3`). It writes HP from 16 bits at `0x48B4A9..0x48B4B2`, the only `0x2C`
    writer of a copy's HP.
  - A dirty create (`0x48BA05`) goes through `CreateFromNetwork` and is unfinished like any copy.
    The round robin's own create (`0x48B497`) is corrected at once by the entry it came with.

`0x489CE0` never finishes a remote copy either. A hit that takes its HP to 0 or below sets pending
death only for a local owner (`0x489EC6..0x489EEE`); a remote copy is clamped to 0 (`0x489EF1`).

**Consequence** (on stock and before B8): the kills of a unit that was created finished — a mobile
type, which gets no `0x12`, or a structure without a build list, whose `0x12` is refused — and dies
within N of its owner's ticks are counted only on its owner's peer: the unit kill count, the killer
player's `+0xFC`, and (for a reclaim) the credit. On every other peer the copy still reads
`+0x104 = 1.0` and the gates refuse. MEASURED by the C3 session (two peers, 2026-09-26): a host
VTLLT1's own copy stayed at 10 through two kills of the joiner's scenario-spawned units (ARMMSTOR,
VTTGT0, VTKROG0: none a builder) while the joiner's copy went 0 → 2. It read `+0x104 = 0.0` on both
peers' copies at a time not tied to the kills, which this analysis predicts is after the round
robin; the value at the kill was not read. MEASURED on three peers on main (`tadr-port/sim-fixes.md`,
*Kill counts across peers — measured for B8*): ARMCK dead ~1 s after their create read `+0x104` 1.0 on both non-owners and added 0 / 4 / 0 kills to the towers' copies
(killer's owner / victims' owner / bystander), those dead 40–94 s after it the same on all three;
the non-owners' copies turned 0.0 at 26–37 s.

Other readers of a unit's `+0x104` see the same stale copy in the window. One is `0x40408E`, which
returns 5 only for a target at 0.0, in the order code before the capture order's tick [role
INFERRED]; the draw is another.

**B8's patch** (`fix_kill_counts`, `fix_built_bounds`; [sim-fixes.md](../tadr-port/sim-fixes.html)
B8 has the argument and the measurements). Rows of the fail-closed table unless marked local:

- **The death.** `call 0x451DF0` at `0x48666D` → `kill_tx_death`, the `0x0C`'s length `push 0xB`
  at `0x486669` compared. A received `0x4C` enters `0x4866D0(rec, 0)` from B4's `0x05` stub with
  the return address `0x455428` (`jmp 0x455F50`, compared), as the case `0x45541C` calls it (its
  twelve bytes `0x45541C..0x455427`, `mov edx,[esp+0x10]; push 0; push edx; call 0x4866D0`,
  compared too). The
  `0x0C`'s dispatch slot `0x455FAC` → a stub that counts a bare `0x0C` and goes on at `0x455F50`.
- **The create's state.** Written into a copy at `CreateFromNetwork`'s exit (B4's `0x48634F`
  stub) as the round robin writes it: `[+0x9E]+0x10 = 0` (`0x48B4A6`; the create's own last store
  of it is `0x485DF2`), HP (`0x48B4B2`), the fraction and bit 13 of `+0x110` when it differs
  (`0x48B4D0..0x48B4EC`).
- **The hold.** The send's entry `0x451DF0` (6 bytes) → the gate, which goes on at `0x451DF6`.
  The creates `0x488700`, `0x488462` and `0x405104` → one wrapper. The flushes, each running its
  displaced instructions and going on after them: `0x488743` → `0x48874B` and `0x488791` →
  `0x488796` (the capture; spans `0x488705..0x488742` and `0x488774..0x488790` compared),
  `0x4884AC` → `0x4884B4` (the placed unit; span `0x488467..0x4884AB`), `0x405119` → `0x40511E`,
  `0x405155` → `0x40515A` and `0x405164` → `0x40516A` (the resurrection; spans
  `0x405109..0x405118`, `0x405141..0x405154`, `0x40515A..0x405163` and its straight line
  `0x40516A..0x40522E` in three rows from `0x40516A`, `0x4051B9` and `0x405202`). The capture's
  function `0x488570` has three callers: the order code (`0x4046C5`, under the unit tick
  `0x43C334`), the dispatcher's case at `0x45577B`, and `0x4933E0` (`0x493486`).
- **Local: the `0x12` bound.** `0x4555BA`'s first two instructions (10 bytes; the case is entry
  `0x455FC4` of the table `0x455F84`) → a stub that drops a record whose `rec+1` or `rec+3` is past
  the array to `0x455F50` and otherwise runs them and goes on at `0x4555C4`.
- **B5's `0x45477F`** now only counts a bare `0x0C` refused in state 5; a carried one refused
  there reaches B5's cancel and dying mark through B8's receiver, and the dying mark writes the
  carried `+0x104` into the copy first, so the unit tick's destroy (`0x48AFB9` → `0x4864B0`)
  decides on the owner's value.

### `0x45AAA0` `FreeObjectState` — the `Object3do` destructor

`__stdcall`, one argument (the object), single exit `ret 4` at `0x45AB01`; the body is
`0x45AAA0..0x45AB01` (`0x45AB10` is a different function). Prologue `53 8B 5C 24 08`
(`push ebx; mov ebx,[esp+8]`): five bytes ending on an instruction boundary at `0x45AAA5`, no
branch into `0x45AAA1..A4`, and the body never calls `0x45AAA0` — cleanly detourable at entry.

What it frees: the loop at `0x45AAB2` runs `MEM_Free 0x4D85A0` on `[obj+0x44 + i·0x36]` for
each of `[obj+0]` prims — every prim's **posed vertex buffer** (prim 0 at `obj+0x22`, the buffer
at prim`+0x22`); then, if `[main+0x1437B]` is non-null, `0x437C90(&obj+0x10)` and
`0x437C90(&obj+0x14)`, which **do not free** (next entry); then `MEM_Free(obj)` at `0x45AAF6`.

Exactly three callers, each pushing the object:

| caller | path | after the call |
|---|---|---|
| `0x486D9E` | unit death, the destructor's tail (above; a NULL `+0x9E` is skipped at `0x486D99`) | `[unit+0x9E] = 0` at `0x486DA3` |
| `0x42474F` | `FEATURES_Destroy 0x4246B0`, a 3DO wreck | `[rec+4] = 0` at `0x424754` |
| `0x4221C4` | the bulk feature purge `0x422170` (one caller, the map free `0x483DD0` at `0x483DE6`), which walks the active and settled wreck lists through the records' i16 `+0x00` (`0x4221CF`) freeing each 3DO record's object, then frees the pool — **reachable only from the level teardown** (below), so not callable mid-level | nothing nulled per object; the pool pointer is nulled at `0x422214` |

So a unit's `+0x9E` is freed only by the destructor; its other writers are the create's stores of
a new object (`0x485DCC` with a COB, `0x485E0E` without). A freed object's block can come back as
another one: the 3D wreck draw's scratch keeps naming a freed husk
([render.md](render.md#the-3d-wreck-draw-a-husk-borrows-the-unit-pipeline-through-one-shared-scratch-verified-2026-09-14)).

**The fork detours this entry** (`tagpu_reclaim.c`): while armed the call enqueues the object
instead of freeing it; the real body runs later through a trampoline over the stolen five bytes.

### `0x437C90` — unregister a slot from the composite registry `[INFERRED role]`

`thiscall`, `ecx = *(main+0x1437B)` (the composite draw context, the `this` of `0x458810` —
frame-composition.md), one argument: a slot address. It walks `[ecx+4]`'s `{ptr, size}` entries
(advancing by `size`, `[ecx]` the total) and zeroes `entry.ptr` where it equals the argument.
**The routine contains no `call` — it frees nothing.** The composite frame at `obj+0x10` therefore
has an owner other than `FreeObjectState`: it is a block of the context's ring, allocated by
`0x437BE0` or `0x437B50` through the ring allocator `0x437A30`, and the ring is freed with the context by the
level teardown (*The composite scratch frame*, above). When the frame's memory can be reused
before then — the allocator evicting it on a wrap — is still the open item of
[Thread-safe destruction](../thread-safe-destruction.html) §10. The registry pointer
`main+0x1437B` is written at `0x42D3DD` (set) and `0x42DCA3` (`mov [ecx+0x1437B],ebx`, `ebx = 0`
`[INFERRED]` — the teardown nulls it, and `FreeObjectState`'s guard then skips the walk).

### `0x45A8D0` / `0x45A950` — the `Object3do` builders

`0x45A8D0(template)` is `stdcall`, `ret 4` (`0x45A941`); `0x45A950(model, scriptfile, unit)` is
`stdcall`, `ret 0xC` (`0x45AA8F`). Size `0x22 + 0x36·n`, `n = 1 + children(node+0x30) +
siblings(node+0x2C)` counted by `0x45AE80` (`stdcall`, `ret 4`; it counts the root and recursively
follows child `+0x30` and sibling `+0x2C`, so it includes the root's own siblings) — ARMPW 15 parts =
844 bytes, CORAK 16 = 898, ARMROCK 13 = 736, the one-piece `armpw_dead` / `corak_dead` = 88.
Allocated by `0x4D83B0(tag "Object State" @0x506614, size)` (`0x45A90B`, `0x45A98F`; the size at
`0x45A97F..0x45A98F` in the scripted builder) and zeroed; `+0x00` = the posed count, `+0x08` = 1,
`+0x0C` = the unit (`0x45A9C8`, the scripted builder only; a unit without a script gets it written
by the create, `0x485E14`), `+0x1E` = the prim tree root from `0x45AEC0` / `0x45AF90`.

**The scripted builder binds the primitives to the COB's piece order** [DISASSEMBLED 2026-09-25].
After `0x45AEC0` has laid the primitives out in pre-order, `0x45A950`'s loop (entered at
`0x45A9CD` with the script's piece count `[scriptfile+8]`, body and latch `0x45A9E3..0x45AA70`)
runs over the COB's pieces: for piece *i* below the primitive count
(`0x45A9E8`; ordinals at or above it are skipped, so the loop never enlarges the object), it
searches the primitives from index *i* up for the first whose node name `_stricmp`s equal to piece
*i*'s (`0x45A9FD` → `0x4F8A70`), swaps the two whole `0x36`-byte primitives when that is at *j* ≠ *i*
(`0x45AA1A..0x45AA51`; no match, or *j* = *i*, goes on at the latch `0x45AA57`), and `0x45AF90` relinks the tree afterwards (`0x45AA7E`). What that order
means for the opcodes, and the primitive layout `0x45AEC0` writes: [cob.md](cob.md#the-posed-model-the-opcodes-write-into-0x45aec0-and-the-piece-order).

**Callers.** `0x45A950` from `0x485DC0` (`UNITS_CreateModelScripts 0x485D40`, the object stored at
`unit+0x9E` at `0x485DCC`); `0x45A8D0` from `0x485E09` (a unit without a script, stored at
`0x485E0E`) and `0x423EC2` (features: the spawn's 3D branch `0x423D96..0x423EDB` reads the def's
template `FeatureDef+0x98` at `0x423EBB`, tests nothing about it, and stores the object at wreck
record `+0x04` at `0x423ECB`). **No per-frame temporaries** — objects are built at creation and
freed at death only.

### `0x4246B0` `FEATURES_Destroy` — the wreck path

`stdcall (cell, force)`, `ret 8`; prologue `53 8B 0D E8 1D 51 00` (`push ebx; mov ecx,[0x511DE8]`)
[DISASSEMBLED 2026-10-29]. In order:

- A cell whose def is `0xFFFE` (a footprint cell) is resolved to its anchor through its `+0x0A`
  (dz) and `+0x0B` (dx) and the map width `main+0x14233` (`0x4246BE..0x4246E6`).
- **Refusals, returning 0**: a def at `0xFFFB` or above — no feature, the void and border markers
  (`0x4246EC`, `0x4246F2`); and, when `force` is 0, a FeatureDef (`main+0x1426F + def·0x100`) with
  `+0xFF` bit 1, the indestructible flag (`0x424715`, `0x42471E`).
- **The record**, when the cell's flags `+0x0C` bit 0 says it holds one (`0x424727`): `0x42472D`
  loads the wreck pool `main+0x1420B`, record = pool `+ [cell+0x0A]·0x30`; `test [def+0xFE],1`
  (`0x424741`) splits GAF from 3DO features, and a 3DO feature's object is freed at `0x42474F`
  (`FreeObjectState`) and nulled at `0x424754`; then, for either kind, `0x4232F0(index,
  &main+0x1421B)` (`0x42476F`) frees the record, the free list's head `main+0x1421B` its second argument.
- **The cells**: the anchor's def is set to `0xFFFF` and its flags bit 0 cleared (`0x42477C` /
  `0x424780`), then every `0xFFFE` cell of the def's `+0x94` × `+0x96` rectangle the same way
  (`0x4247B3..0x4247BE`). **The cell is unlinked last**, so the fork's wreck gather (which reads
  the cell, then the record, then the object) sees the same free-before-unlink shape as the unit
  path.
- `0x440A40(the anchor's x and z, the def's size)` (`0x424822`), and 1 (`0x42482A`).

**Callers** (MEASURED over `.text`, `call` and `jmp`, and over the image for a stored address): the
resurrect (`0x40519E`); the console command BurnOne `0x4163A0` (`0x4163C8`, with `force` 1); the
swap `0x423710` (`0x423792`, `0x4237B1`); the spawn `0x423C50`'s footprint (`0x423D6A`, `force` 0,
which abandons the spawn on a refusal and keeps what it destroyed so far); the feature tick
`0x424050` (`0x42442F`, `force` 0, when a burn's sequence ends); the clear of every feature
`0x424840` (`0x42487A`), itself called only by the console command BurnAll `0x416390` (stored in
the command table at `0x502040`). The demo recorder observes the entry
([map.md](map.md#the-demo-recorders-features-the-grid-the-wreck-pool-and-the-calls-that-change-them-disassembled-measured-2026-09-30)),
and the puppet gate refuses it from the level's first step outside the replay's own calls
([recorder.md](recorder.md#the-puppet-host-its-gates-the-30-tick-deadline-and-the-sight-loop-disassembled-measured-2026-09-30)).

### `0x491B60` — the level teardown

**No stack arguments**; six bare call sites (`0x460630`, `0x491C6A`, `0x49262C`, `0x4996AA`,
`0x49971D`, `0x4997AF`; *Who enters it*, below), none pushing for it; first five bytes
`A1 E8 1D 51 00` (`mov eax,[0x511DE8]`), no branch into `0x491B61..64`, resume at `0x491B65`.
**Two exits**: `ret` at `0x491C59`, and a tail-jump at `0x491C54` (`jmp 0x450DD0`, taken when
`0x435100` on `[main+0x391E9]` returns 3) — `0x450DD0` reads no stack argument either and
returns with a plain `ret` at `0x450E19`, so a `call` through the stolen tail returns to the
caller on both paths and code can run after it.

**The body, in order** [DISASSEMBLED 2026-10-29, the pristine build]:

- `main+0x2A44 &= ~4` (`0x491B65`); `0x4CED40` and `0x4CE690(4)`, both on `ecx = [main+0x10]`
  (`0x491B76`, `0x491B86`); `0x41DC20` (`0x491B8B`); `0x437D30` (`0x491B90`).
- **`0x485980`** (`0x491B95`), the unit teardown: it walks the unit array
  (`main+0x14357..+0x1435B`, stride `0x118`, `0x48599B..0x4859A8`) and calls `Send_UnitDeath
  0x4864B0(unit, 8)` for **every unit whose model index `[unit+0xA6]` is non-zero**, so every live
  unit's object is freed through the destructor (`0x4866D0` → `0x486D9E`) — hundreds of
  `FreeObjectState` calls — then three `MEM_Free`s of the unit arrays (the per-map table, above).
- `0x471DE0` (`0x491B9A`), every particle layer destroyed; `0x420960` (`0x491B9F`; it frees the
  projectile drawer's two-plane frame at `0x420A09`, [render.md](render.md)); `0x44F6E0`
  (`0x491BA4`); `0x464A00` (`0x491BA9`), the players' LOS counters, AI objects and `+0xEC`
  records; `0x466AA0` (`0x491BAE`), the minimap surfaces.
- **`0x483DD0`** (`0x491BB3`), the map free → `0x422170` → the bulk wreck `FreeObjectState` loop
  at `0x4221C4`. `0x483DD0`'s only caller is `0x491BB3` and `0x422170`'s only caller is
  `0x483DE6`, so this is the sole path to that third caller.
- `MEM_Free` of `main+0x1438B`, `+0x14387`, `+0x14383` (`0x491BC5`, `0x491BD9`, `0x491BED`),
  the transform's three scratch arrays the level load allocates
  ([render.md](render.md)'s `0x46BAE0` row), nulled at `0x491BFE..0x491C16`.
- `0x42E120` (`0x491C1C`); **`0x42DB90`** (`0x491C21`), the model templates and the unit-def
  array (below); `0x42F3A0` (`0x491C26`); `0x42A570` (`0x491C2B`); `0x499A80` (`0x491C30`), the
  projectile array; `0x440A00` (`0x491C35`); `0x488BF0` (`0x491C3A`); then the exit test above.

**The fork wraps this entry** (`tagpu_reclaim.c`) with a pre hook (hold the render thread off,
wait for it to leave its pass, flush the deferred queue while the registry is alive — or, if it
does not leave within a second, keep the queue and keep deferring through the cascade) and a post
hook (release it); the render thread is live again as soon as the teardown returns.

#### Who enters it, and on which thread

*[BINARY-VERIFIED 2026-09-20]*

**The entry set is closed to direct calls.** `0x491B60`'s address is never taken: the 32-bit
constant `0x00491B60` appears at no offset of the 1 178 624-byte image, and there is no `.reloc`
section, so a stored address would have to be literal. It therefore cannot be reached through a
function pointer, a vtable or a dispatch table, and the `E8` sites are all of them. There are
exactly **six**:

| call site | in function | what it does |
| --- | --- | --- |
| `0x460630` | `0x4605C0` | the in-game menu's quit path |
| `0x491C6A` | `0x491C60` | a three-call shutdown stub (`0x4257A0`, `0x451B60`, the teardown); its only caller is `0x460626`, also in `0x4605C0` |
| `0x49262C` | `0x492360` | |
| `0x4996AA` | `0x499200` | sets state **7**, handler `0x499880` |
| `0x49971D`, `0x4997AF` | `0x499200` | teardown, then `0x491D70(1)`, `0x4A9660`, `0x4257A0`, then state **2** with handler `0x496BB0` — the shell. This is EXITMENU→MAINMENU and EXITMENU→RESTART |

**None of those four functions has a direct caller.** Every one is address-taken and entered
through the engine's **state-handler slot `[main+0x391F5]`**, dispatched at the single site
`0x499A1C` (`call dword ptr [eax+0x391F5]`) inside `0x499890`. `0x499200` is installed as the
in-game handler by the loading-screen handler `0x497F40` (`mov [ecx+0x391F5], 0x499200` at
`0x498455`, beside `mov [ecx+0x391F1], 6`), and also at `0x490BC5`. The same slot holds
`0x496BB0`, `0x496B10`, `0x496CE0`, `0x496DB0`, `0x497F40` and `0x499880` — it is the engine's
whole state machine. `0x496790`, the in-game frame callback, is *not* address-taken; it has
three direct callers: `0x499200` at `0x4995B8` (in play) and `0x4996A5` (a network game's end,
after `0x491D70(1)` at `0x499674` and `GUI_Pop 0x4A9660` at `0x499686`), and the loading screen
at `0x49842F`.

**But THREE OF THE SIX are pinned to the game thread by construction, in the strongest way this
binary allows.** `0x499200` — the function holding `0x4996AA`, `0x49971D` and `0x4997AF` — also
calls the in-game frame callback **`0x496790` directly**, at `0x4995B8` and at `0x4996A5`. The
second of those is the instruction immediately before the teardown call:

```
0x4996A5   call 0x496790        ; draw the frame
0x4996AA   call 0x491B60        ; and tear the level down
```

Same function, same straight line, no branch between them. And `0x496790` is precisely the
function whose `DrawGameScreen(1, 1)` at `0x4969CD` the fork's publisher gates with
`on_game_thread()` — a test it applies to *every in-play draw*, hundreds of thousands per
session, and which has never once counted a foreign one (`foreign=0`). So whatever thread runs
the frame callback runs these three teardown calls, and that thread is measured continuously,
not sampled.

**The other three are not pinned statically.** `0x460630` and `0x491C6A` (in `0x4605C0`) and
`0x49262C` (in `0x492360`) call nothing that is independently tied to a thread; their functions
are reached through the state slot like the rest, and the graph stops there. For those, the
evidence is the measurement and the standing instrument, nothing more. Note the asymmetry cuts
against comfort rather than for it: by `0x499200`'s behaviour the measured teardowns went
through the three sites that are *also* pinned by construction, so the three that rest on
measurement alone are the ones least exercised.

What the static work establishes overall is the bound: the entry set is closed, so those six
sites are all the ways in.

**What the loader thread result really is.** The direct-call transitive closure of `0x497C70` /
`0x497180` is 1 208 functions and contains neither `0x491B60`, nor the dispatcher `0x499890`,
nor the state-setter `0x4B4FD0`, nor `0x499200`. That is a **direct-call** result and not a
proof: the same closure holds 690 indirect call sites across 260 functions.

**Thread creation.** `CreateThread` is the only thread-creation import — no `_beginthread`,
`_beginthreadex`, `TerminateThread` or fiber API — with exactly **three call sites**
(`0x4DA27A`, `0x4E616E`, `0x4E780C`). That bounds the spawn *sites*, not the number of threads:
`0x4E77D0` is a generic CRT spawn wrapper, reachable through the `ret 0xC` wrapper `0x4B6B20`,
so one site can make many threads. The loader is `push 0x497C70; call 0x4B6B20` at `0x4982C5`,
through that wrapper, with `0x497C70` calling the body `0x497180` at `0x497CA1`.

**The identity itself is MEASURED, and re-measured on every run.** Four teardowns over two
distinct UI paths — EXITMENU→MAINMENU ×3 and EXITMENU→RESTART ×1 — all entered `0x491B60` on
the game thread, with the loader thread alive on a different id at the same moment. A grep of
the 299 surviving instance logs finds 50 teardowns and no foreign level-end ever recorded. The
fork now names the entering thread in its teardown line and carries `tdforeign=` in the reclaim
heartbeat, so the claim is re-checked on every launch, including configurations that session
could not reproduce.

**Conclusion: the level teardown is game-thread only, on the evidence of the measurement and the
standing instrument — not of the call graph, which stops at a function pointer.**

**A trap for every call-site table in these notes.** Attributing a call site to the **nearest
preceding `E8` target** assumes a function starts where something calls it. The main-loop
functions here are *never* called — they are stored in `[main+0x391F5]` and dispatched — so that
heuristic cannot see them, and their call sites are charged to whatever ordinary helper sits above
them: `0x499100` above `0x499200` (it is an ordinary `stdcall` helper running
`0x499100..0x4991FB`, `ret 4` then two `nop`s, and contains no call to the teardown), `0x4604A0`
above `0x4605C0`, `0x491EC0` above `0x492360`. **Take function starts from address-taken
constants as well as call targets, and check the claimed function contains the call site** — an
`objdump` of the boundary costs one command.

### `0x42D5xx` — the model-pointer table is BUILT here, and every slot in it is written

`[BINARY-VERIFIED 2026-09-09]` The other half of `0x42DB90` below, and the half a reader needs:
it is what makes "the ModelId is in range" a safety property rather than a guess.

- **The count comes first.** `main+0x1438F` (`UNITINFOCount`) is already set when the table is
  allocated: `0x42D684` loads it, `0x42D68A` shifts it left 2, and `0x42D693` calls the named
  allocator `0x4D83B0` for exactly `count * 4` bytes, storing the result to `main+0x14377`
  (`0x42D6AA`). So the count is the table's **length**, not merely a related number — the same
  value bounds the unit defs at `main+0x1439B` at stride `0x249`, which is why one count serves
  both arrays.
- **The allocator does not zero** ([`MEM_Alloc`](#0x4d83b0-mem_alloc-the-named-allocator-mem_allocnamed)).
  Nothing about an untouched slot is therefore safe to assume — which is why the next point is the
  load-bearing one.
- **Every slot in `[1, count)` is written.** The load loop runs `esi = 1` while
  `esi < [main+0x1438F]` (`0x42D6B6` / `0x42D6BC`) and stores the loaded model to
  `[table + esi*4]` at `0x42D7A2` on every iteration — including the failure path, where
  `0x4CB560` returned NULL and `0x4B6290` reported it: `edi` is stored either way.
- **Slot 0 is never touched** by either loop — neither the fill (which starts at 1) nor the free
  (`0x42DBCA`, same range). It is the "no model" entry, and `unit+0xA6 == 0` is how the engine
  and the fork both spell "this unit has no model".

**The invariant this gives a reader.** For `1 <= mid < [main+0x1438F]`, `[main+0x14377][mid]` is
a slot the engine wrote at load and nulls at teardown, so it holds **NULL or a live template of
the level that is loaded** — never the allocator's leftovers, and never off the end of the
allocation. `unit+0xA6` itself is bounded by nothing, and the unit array it comes from is a
Mode B object (recycled in place), so a slot that died under a render pass hands over another
unit's ModelId or a torn one. Bounding it against this count is what keeps that inside the Mode B
contract — one wrong frame, never a fault. `tagpu_native.c`'s `model_root` is that bound, and
[thread-safe destruction](../thread-safe-destruction.html) §2 is the contract.

**What it does NOT give you** is a lifetime: the templates are still freed by `0x42DB90` at the
level teardown, so a reader also has to be outside that window. That is the render thread's
`teardown_active()` gate, and its one hole is the pre hook's timeout (§6b).

### `0x42D2E0` — the level's unit-data load, the FBI loader `0x42BF40`, and the one-type reload `0x42D1F0`

`[DISASSEMBLED 2026-09-25, objdump of the pristine build; the reload MEASURED the same day]`
Read for TADR section C ([data keys](../tadr-port/data-keys.html)), whose unit-key reader hooks the
FBI loader.

- **`0x42D2E0` runs on the LOADER thread.** Its one caller is `0x4918CA`, inside
  `LoadGameData_Main 0x4917D0`, which the loader body `0x497180` calls. Its per-type loop
  (`esi = 1` while `esi < UNITINFOCount`, `0x42D6B6` / `0x42D915`) does three things per type, in
  this order: the FBI — its path (`0x4290F0`), its size (`0x4BBC40`) and, **only when that
  finds the file** (`0x42D71A` jumps past it on 0), **the FBI loader `0x42BF40(path, def)` at
  `0x42D722`**; the model — `0x4CB560` open, `0x4CB590` parse, `0x42A140`
  texture-match, stored to `MODEL_PTRS[esi]` at `0x42D7A2` (the section above); and the script —
  `scripts\<name>.COB` (`0x42D8E5`) through `0x4B2450` into **`def+0x18E`** at `0x42D8F4`.
- **Filtering and sorting precede all COB loads.** `0x42D485` makes the definition array
  writable; `0x42D493..0x42D510` filters availability bit `def+0x241 & 0x800000`, compacting
  retained definitions with `0x42B370` at `0x42D4DC..0x42D510`. `0x42D542` stores the reduced
  count. The sort at `0x42D533..0x42D610` uses name comparator `0x42DB60`, calls `0x432FB0`
  for up to 16 elements and `0x432D40` plus insertion work for larger arrays. IDs are assigned
  at `0x42D634` before the per-type load loop. Thus checksum-incompatible types removed by
  native multiplayer sync never reach COB validation, and rejection indices recorded during
  COB loading are not subsequently moved by this compaction/sort. [DISASSEMBLED 2026-09-28;
  MEASURED: host-only and joiner-only malformed COB fixtures absent on both peers.]
- **What a level's type list, and so `RT_UNITDEFS`, is a function of** [DISASSEMBLED
  2026-10-02]. The filter keeps the order of what it keeps (a `remove_if`: `0x42D4AD..0x42D4D2`
  finds the first type without bit 23, `0x42D4DC..0x42D50E` copies each later type that has it
  down). Bit 23 is the loader's verdict (`0x42A8D0`: set when the FBI's `Version` is not above
  the game's, `0x42B066..0x42B0A7`, cleared again at `0x42B0AD..0x42B0DC` and
  `0x42B14B..0x42B162`; `None` gets it at `0x42AB02`; the loader's tail `0x42B248..0x42B2E8`
  drops every type without it), and afterwards the battleroom's restriction store's (`0x46E203`,
  `0x46E23B`, in `0x46E160`, *A type's own cap* above, which marks its running at game start
  INFERRED). Nothing else writes it: the other writers of `+0x241` are the def copy `0x42B370`
  and the FBI loader `0x42BF40`, whose bit-field writes never touch bit 23.
  The sort covers `[1, count)` (`0x42D54F`, `0x42D557`), so `None`, type 0, stays first. Its
  comparator `0x42DB60` is `_stricmp(a+0x20, b+0x20) < 0` through the CRT's `0x4F8A70`, which,
  with `__lc_handle[LC_CTYPE]` (`0x52A018`) at 0, folds `A`..`Z` alone to lower case and compares
  unsigned bytes; the CRT's locale setter `0x4ED9A0` has no reference in the exe (no call, no
  pointer to it), so that is the branch taken. Types whose names are equal after that folding
  tie, and the sort (the partition `0x432D40`, then insertion) is not stable: tied types keep no
  defined order. Then the per-type loop reads each type's FBI again by its name,
  `units\<UnitName>.FBI` (`0x42D709`), and `0x42BF40` reads `unitname` into `+0x20` once more
  (`0x42BFA7`, 0x20 bytes, default empty): a type whose file is named otherwise than its
  `UnitName` takes the name from whatever file bears its name. So a level's names are the
  restriction store's verdict over the loader's types, sorted as above, each renamed by the file
  its name points to; in retail the result was the shell's 279 names sorted (above, *The shell
  and a level hold the unit types in different orders*).
- **`0x42BF40(path, def)`** (`sub esp,0x518`) opens the file (`0x4C2F60`) and finds `[UNITINFO]`
  (`0x4C3410` with `0x503914`); at `0x42BF8C..0x42BF93` it takes **`ebp = def`**
  (`[esp+0x530]`) and **`ecx = the section`** (`[esp+0x14]`), and at `0x42BF97` begins its first
  read, `UnitName` into `def+0x20` (`0x42BFA7`). Nothing has been read from the file at
  `0x42BF97`, and the stolen `push 0x5119B8` carries no relative operand: that is
  `tagpu_datakeys.c`'s observer. **Two callers**: `0x42D722` (above) and `0x42D269` (below).
- **`0x4C48C0`, the TDF string reader**, `thiscall(section, buf, key, len, dflt)`, `ret 0x10`:
  a binary search of the section's keys with `_stricmp 0x4F8A70`. Found: `strncpy` (`0x4E4760`)
  of `len` bytes, then `buf[len-1] = 0`, and it returns 1 — a value that fills the buffer is cut
  silently. Absent: an **unbounded** copy of `dflt` (`0x4C4963..0x4C4988`), and it returns 0.
- **`0x42D1F0(type)`, the one-type reload**, `stdcall`, `ret 4`. It skips type 0 and a type
  whose def lacks the in-play flag `0x800000` at `+0x241`, makes the def array writable
  (`0x4D8780`: `PAGE_READWRITE` over its heap block, through `0x4D8720` → `0x4D86B0`; the array
  is read-only in play), rebuilds the FBI path, and — **only when `0x4BBC40` finds the file**
  (`0x42D261`; otherwise it re-protects and returns, `0x42D2BF`) — re-runs **`0x42BF40` at
  `0x42D269`**, frees the COB (`0x4B2540`, `0x42D275`) whether or not that loader got as far as
  reading anything, loads it anew (`0x42D294`) into `def+0x18E` (`0x42D299`), and seals the array
  again (`0x4D8710`: `PAGE_READONLY`). The model is **not** reloaded. Its one caller is `0x4174BE` in the console
  command `Reload` (`0x417490`, *Built-in cheat/console command surface* above), so it runs on
  the **GAME thread**, mid-play. The freed COB is reallocated at once, so it can come back at
  the same address: a cache keyed on the COB pointer alone would not see the reload.
  **MEASURED 2026-09-25**: `+reload ppllt` with SoftwareDebugMode `0x2` set re-ran the FBI loader
  (`tagpu_datakeys`' reader logged it a second time), no crash. The console kills every unit of
  the type first (`0x486E80`, *Built-in cheat/console command surface*), so the units that run
  the new COB are all created after it. **MEASURED 2026-09-25** with the extra-weapons module installed: after
  `+reload armllt10`, two `ARMLLT10`s spawned into the same game re-asked their per-type piece
  caches (`AimFromWeaponN`/`QueryWeaponN` = pieces 2 and 1 on slots 3–9, as before the reload)
  and launched from slots 0, 3 and 4; `violation`, `mismatch` and `cob_full` stayed 0. Again with
  a loose `units/ARMLLT10.fbi` holding no `[UNITINFO]` placed before the reload: `0x4BBC40` found
  it, `0x42BF40` ran and left before its weapon reads (no second loader line from the module's
  `0x42CEF2` splice), the def kept its weapons and its CRC, and the COB was replaced all the same —
  new towers asked their pieces again and launched from slots 0, 3 and 4.
- **A table keyed on the def pointer alone can outlive its game.** The teardown frees the def
  array (`0x42DCCB`) and the menu-time loader `0x42A8D0` allocates it again from the same count
  before the next game (`0x42AA8A`), so the address can repeat; and the FBI loader can stop before it reads anything (the open at
  `0x42BF66`, no `[UNITINFO]` at `0x42BF7C`) or not run for a slot at all (`0x42D71A`). A row
  written for slot *k* in one game then answers for slot *k*'s def in the next.
  `tagpu_datakeys.c` closes it by observing the two entries as well: `0x42D2E0`'s (`sub
  esp,0x610`) empties its records at every unit-data load, and `0x42BF40`'s (`sub esp,0x518`)
  resets the slot's record with a fresh serial before the open, so the read site fills only a
  record its own call reset. `tagpu_weapons.c` observes `0x42D2E0`'s entry too (the two observers
  chain through `tagpu_detour_observe`) and empties its def records there, so a slot this load
  did not write reads as stock. At the loader's entry it forgets only the record's piece caches
  (the COB's `AimFromWeaponN` / `QueryWeaponN` answers): a `Reload` whose FBI fails to open
  leaves the def's own `weapon1..3` as they were, so the record's weapons stay consistent with
  them, but it replaces the COB anyway (`0x42D275` / `0x42D294`). Every def copy of the load runs
  after that entry and before the FBI loop (`0x42D6B6`): `0x42D501`; the quicksort `0x432D40`
  (called at `0x42D57D` and by itself, `0x432F54` / `0x432F64`) and `0x432FB0` (called at
  `0x42D573` and `0x42D590`); and the insertion pass `0x42D599..0x42D60E` (`0x42D5CD`,
  `0x42D5FF`, through a stack temp). **MEASURED 2026-09-25**: two skirmishes in one
  process: the second load logged 284 records emptied before its loader
  wrote ARMLLT10's ten weapons, and the towers fired slots 3–9 in the second game
  (`violation=0 mismatch=0`).

### `0x42DB90` — the model templates are freed here, and only here

`[BINARY-VERIFIED 2026-09-08]` Called once from the teardown cascade, `0x491C21`, after the unit
walk `0x485980` and the map free `0x483DD0` ([the call list](#0x491b60-the-level-teardown)). It is what makes a `Model3DONode` tree's lifetime the **level**:

- it walks the model-pointer table `[main+0x14377]` — the same table the fork calls
  `OFF_MODELPTRS` and indexes with `unit+0xA6` — bounded by the count at `main+0x1438F`, with
  `esi` running over the unit defs at `main+0x1439B` (`0x42DB98`..`0x42DBEE`);
- per entry: `MEM_Free 0x4D85A0` (`0x42DC01`), then the slot is nulled (`0x42DC15`,
  `mov [eax+edi], ebx` with `ebx = 0`); two further `MEM_Free`s follow in the same body
  (`0x42DC23`, `0x42DC52`);
- then the table itself is freed (`0x42DCB6`) and `main+0x14377` nulled (`0x42DCD8`);
- and **last, `0x42DCCB` frees `main+0x1439B` — the whole UnitDef array** (`mov edx,
  [ecx+0x1439B]` at `0x42DCC4`), with the pointer nulled at `0x42DCE6`. **Five `MEM_Free`
  calls in this body, not four** (verified by disassembly 2026-09-09).

**Both frees are ours since 2026-09-09.** `0x42DC01` and `0x42DCB6` are redirected to
`tagpu_reclaim`'s ring (the byte check is `E8` with a rel32 that resolves to `0x4D85A0`), so a
template outlives any render pass still walking it; the body itself is untouched and still nulls
every slot and the table pointer. `0x42DC23`, `0x42DC52` and `0x42DCCB` are left alone. The
first two are unit-def fields no pass of ours reads; **`0x42DCCB` frees the UnitDef array we
read all over** (`tagpu_order`, `tagpu_cat`, `tagpu_weapons`, `tagpu_scenario`) and is safe
only because every one of those readers, and this cascade, is on the **game thread**. [Thread-safe destruction](../thread-safe-destruction.html) §6c.

**Why it matters to us.** `FreeObjectState 0x45AAA0` — the funnel `tagpu_reclaim` defers — never
reaches these blocks: a template is not owned by any unit, it is shared by every unit of a type.
So the deferral makes a dying unit's `Object3do` safe and does nothing for the tree, and anything
that CACHES a template pointer across frames is holding an address that this function returns to
`MEM_Free`, i.e. to the small-block heap documented above as recycling aggressively. That is the
whole rationale for the fork's level generation
([thread-safe destruction](../thread-safe-destruction.html) §6a).

### Why the drain has no tick to ride — `0x4969D2`

The only game-thread hook the fork owned, until 2026-09-12, was the scenario applier's
`Game_MainLoopTick` detour at `0x4969D2` (stolen `A1 E8 1D 51 00`, `tagpu_scenario.c`), and it is
installed **only while a scenario is being applied**, then left to its one-shot state machine — it
cannot host a per-tick drain, and two detours cannot share the site. *(Since the frame packet's
landing 1 there IS a per-frame game-thread hook that rides every in-play draw: the observer on
`DrawGameScreen 0x468CF0` gated on the return address `0x4969D2` — see "The in-play publish
point" above. The drain has not been moved onto it; it could be.)* So `tagpu_reclaim.c` drains from inside
`FreeObjectState` itself (every death first frees what became safe), which is why the most recent
death's object is held until the next death or the level ends.

### `0x4D83B0` `MEM_Alloc` — the named allocator `[MEM_AllocNamed]`

`0x4D83B0(tag, size)`, `cdecl` — the caller pops both arguments. **It drops the tag**: it reads only
`[esp+0x8]`, the size, calls `0x4D83C0(size)` (`0x4D83B5`) and returns its result
[DISASSEMBLED 2026-10-29]. The tag is a heap tag at a higher level; nothing looks a block up by it.
`0x4B4F10` → `0x4D8660` → `0x4D83C0` is the untagged path to the same body (the partner of
`0x4B4F20`, *`MEM_Free`* below).

`0x4D83C0(size)` [DISASSEMBLED 2026-10-29]:

- enters the allocator's critical section (`0x4DA780` returns it, `0x528A28`; `EnterCriticalSection`
  through `ds:0x4FC198` at `0x4D83CB`);
- **the dispatch at `0x4D83D5`**: `call 0x4D80D0; test al,al` — the "fussy" heap option, the byte
  `0x5289A4`, which `0x4D80D0` registers on first use with the command-line switches `-memfussy` /
  `-memnofussy` / `-memfrontalign` — to the private heap `0x4DACF0(size, 0)` (its blocks freed
  through `0x4DB7D0`), else the CRT's `malloc 0x4E8890(size)` and, on success, the accounting
  `0x4DA7D0(size)`;
- a NULL calls the new handler at `[0x5289BC]` and retries from `0x4D83D5` while one is installed
  (`0x4D8409..0x4D841F`); the installed handler `0x49E700` ends the process (*The out-of-memory
  text*, above);
- **it does not zero.** A fill runs only when the option `0x4D8200` answers (`0x4D8425`; its
  switches are `-memset` / `-memnoset`, the fill value from `0x4D8260`, `setvalue`), through
  `0x4D82C0` (`0x4D8436`) — off unless the switch is given [INFERRED: the default the registration
  passes is 0]. Nothing about an untouched block can be assumed;
- leaves the section (`LeaveCriticalSection` through `ds:0x4FC194`, `0x4D843F`).

Code that allocates a block the engine will free must take it from here: `scratch_alloc`
([render.md](render.md#the-composite-scratch-frame-and-the-rasterisers-span-tables)) runs the success path by hand inside the same section,
and a DLL-heap block handed to the engine's `MEM_Free` is a block of the wrong heap.

### `0x4D85A0` `MEM_Free` — the one way any engine allocation dies

[VERIFIED 2026-09-12] A two-line wrapper (`mov eax,[esp+4]; push eax; call 0x4D85B0; add esp,4;
ret`, prologue `8B 44 24 04 50` — five relocatable bytes, which is what makes it detourable), and
**`0x4D85B0` has exactly one caller: this.** `0x4D85B0` returns at once for NULL (`0x4D85B7`), takes
the allocator's critical section (`0x4DA780`, then `EnterCriticalSection` through `ds:0x4FC198` at
`0x4D85C2` [DISASSEMBLED 2026-09-29]) and frees through the same dispatch as the allocation: the
fussy heap `0x4DB7D0(p, 0)` (`0x4D85D4`) or, by default, `MEM_Size 0x4D8360` (`0x4D85E7`) → the
accounting `0x4DA840` (`0x4D85F0`) → the CRT `free 0x4E8820` (`0x4D85F9`), which itself brackets
with `_lock(9)` / `_unlock(9)` (`0x4EAC60` / `0x4EACE0`, a lazily created critical section); it
leaves the section (`ds:0x4FC194`) on both paths — **thread-safe from any thread**.

363 sites call `0x4D85A0`, `SurfaceFree 0x4C6AC0` among them (`0x4C6ACF`); `0x4B4F20` →
`0x4D8670` → `0x4D85A0` is the untagged path [DISASSEMBLED 2026-10-29]. MEASURED in game
(2026-09-12): **~10 500 calls a second**, ~675 a second in a 200v200 battle on Two Continents
(2026-09-26). **The loader thread makes most of them**: 33 191 frees on threads other than the game
thread in one Two Continents load (MEASURED 2026-09-29, two counters at the observer; 33 842 was the
2026-09-12 figure). An observer at this entry is a destructor for every engine block — the GUI lane's
`before_memfree` is one ([render.md](render.md#the-offscreen-object-verified-0x4c69f0)).

### `0x4D8360` `MEM_Size` — the allocator knows a block's size

[VERIFIED 2026-09-16, objdump of the pristine build] `MEM_Size 0x4D8360(p)`, `cdecl`: 0 for a NULL
block (`0x4D8365..0x4D836C`), otherwise a two-way dispatch on the same mode predicate the rest of
the allocator uses — `call 0x4D80D0; test al,al` — to `0x4DBAE0(p)` (the private heap's sizer) or
`0x4E8FB0(p)` (the CRT `_msize`), the size in `eax`. `MEM_Alloc`'s dispatch at `0x4D83D5` tests the
same predicate, so the two agree by construction about which heap a block came from. `0x4D85B0`
calls it on the free path at `0x4D85E7` and feeds the result to `0x4DA840`, the accounting.
**It takes no lock of its own, but both of its callees do**: `0x4DBAE0` enters the allocator's
critical section `0x528A28` first (`0x4DA780`, then `EnterCriticalSection`), and `_msize` takes CRT
heap lock 9 (`0x4EAC60(9)`). So it is safe to call from an observer at `MEM_Free`'s entry, and the
GUI lane's destructor calls it inside the allocator's section, as `0x4D85B0` does. It costs
**~85 ns a call** (MEASURED 2026-09-26, 200v200 on Two Continents, 80 144 calls), so even the
~10 500 frees a second above are under 0.1 % of one core.

### The CRT heap beneath them

The CRT `_heap_alloc 0x4E8900` sends `(size+15) & ~15 <= __sbh_threshold` (`[0x5109EC] = 0x1E0`,
480 bytes; `_set_sbh_threshold 0x4F2280` has no callers) to the **small-block heap**
`__sbh_alloc_block 0x4F2600` (4 KB pages in 4 MB regions) and everything larger to
`HeapAlloc(_crtheap [0x52B524])`; `_heap_init 0x4F1830` creates that heap with `HeapCreate(0,
0x1000, 0)` — serialised.

**The small-block heap decommits pages inside a free.** `__sbh_free_block 0x4F25A0` counts wholly
free pages (`[0x52A42C]`); when the count reaches 32 it calls `0x4F2470(0x10)`, which walks the
region's page table from the top and `VirtualFree(page, 0x1000, MEM_DECOMMIT)`s up to sixteen
wholly free pages (`0x4F24A9`), and `0x4F2410` releases a region whose `0x400` pages are all gone
(`VirtualFree(base, 0, MEM_RELEASE)`). A freed block of ≤ 480 bytes can therefore become
**unreadable in the very free that returned it** — which is where the one-piece wreck objects
(88 bytes) live. Unit objects (736–898 bytes) are wine-heap blocks, whose pages go only when
wine 9.0's `heap_free_block` decommits a subheap's free tail past its `0x10000` hysteresis or
releases a subheap that has emptied.
