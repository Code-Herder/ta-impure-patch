# Orders and the AI

The order module, order fallbacks, resurrect, the AI's order activity and resource placement.

*Part of the [engine map](../exe-reverse-engineering.md). A function's entry is the section whose heading names it — the [address index](address-index.html) lists them: a new fact about a function extends its entry, in place; a new section is for an area nobody has mapped yet.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### The resurrect order `Order_Resurrect 0x404DB0`, and its exits after the unit is made

`0x404DB0` is `stdcall(builder, order, flags)`, `ret 0xC`, the handler of order descriptor 18
(record `0x4FC8AA`: "Resurrecting", the handler at `+4`, the name "Resurrect" at `+0x15`)
[DISASSEMBLED; MEASURED live: the counters below fire inside it on RESURRECT orders]. Its state is
the order's byte `+0x5`, dispatched through the table `0x4052D8` (states 0–6). The full walk, and
why TADR's post-create fix was not ported, are in
[the section B evidence, Part 3 §1](../tadr-port/sim-fixes-evidence.md). What this map keeps:

- **The entry check, every state ≤ 5** (`0x404DC6..0x404E16`): `0x421DA0(order+0x22)` answers the
  feature's def through one `0xFFFE` hop; `0xFFFF` → "Resurrection failed" (`0x501698`) and
  return 8, and a FeatureDef without `+0xFE` bit 7 → return 8.
- **The countdown** (state 3, `0x404F92..0x404FEE`): `order+0x3A = BuildTime × 0.3 /
  ⌊WorkerTime / 30⌋`, the resurrected type's UnitDef `+0x1EA` against the builder's `+0x1FE`, the
  0.3 being the double at `0x4FC948`, made an integer by `0x4E43A0` [INFERRED: `_ftol`]. State 4
  (`0x405005`) counts it down, one per call.
- **The second lookup, after `CreateUnit`** (state 5): `0x4815F0(order+0x22)` → the anchor cell,
  and `GetGridPosFeature 0x421E60` → its def. **`0x4815F0` follows the `0xFFFE` hop itself**
  (`0x481640..0x481679`, bounded), so an order on any cell of a multi-cell wreck reads the
  anchor's record at `0x40516C` — the record index at the anchor's `+0xA`, not the hop offsets.
  `0x40514F cmp ax,0xFFFB; jb 0x405164`: the **failure exit `0x405155`** returns 8 with the new
  unit left a nanoframe; the **success path `0x405164`** copies the record's `+0x20` rotation to
  `unit+0x64` (`0x40516C..0x405191`), removes the wreck with `FEATURES_Destroy 0x4246B0` (which
  hops too, `0x4246BE..0x4246E6`), and finishes the unit.
- **How a player gives it: `reclaim`** [DISASSEMBLED]. The order resolver `0x43F0E0` has no
  resurrect type among its 1–14 (jump table `0x4401EC`); its reclaim case `0x43F4F7` needs the
  unit's def `+0x245` bit `0x400` (`0x43F4FD`), finds the feature under the position (`0x4815A0`
  and the `0xFFFE` hop, `0x43F50B..0x43F58A`), and when the def also has `+0x245` bit `0x800`
  (`0x43F5A5`) — `canresurrect`, which the FBI loader reads into that bit (key `0x503A64`,
  `0x42CA15..0x42CA35`; CORNECRO is the one retail unit with that tag) — the
  cell is in the viewing player's sight (`0x43F5D1..0x43F5FF`: the word at `main+0x14273`, bit
  `main+0x2A43`) and FeatureDef `+0xFE` has bit 7 (`0x43F609`), it jumps to `0x44004C` (`0x43F612`), which
  resolves the name "RESURRECT" (`0x5052F0`) through `0x438760`. So a scenario's or `tacli order`'s
  `reclaim` of a wreck by a Necro is a resurrect order.

**MEASURED 2026-09-26: the failure exit never fired.** Scratch counters (a build never committed)
on `0x405155` and `0x405164` logged the order's position, the cell and its record index at each
pass; `ddraw.dll` at `8d033d1`, gamespeed 10, `scenarios/b1-resurrect.json` and seven variants of
it (`b1-resurrect.gen.py`: 110 wrecks of 1×1 to 5×6 on the Two Continents plateau, each with a
CORNECRO ordered to reclaim it, ten wrecks with a second Necro, a third of the multi-cell orders at
the anchor and the rest at the footprint's centre or far corner). **847 passes through `0x405164`,
0 through `0x405155`**: 440 on 1×1 wrecks, 126 on a multi-cell wreck's anchor, 136 on another cell
of one, and 145 in a first layout that was not split by footprint. Every pass read the anchor's
own def at the cell `0x4815F0` returned. About a third of the orders never reached state 5 (a
shared wreck's second Necro finds it gone at the entry check; the rest were not followed up).

### The saved-game loader's order fallback — `0x43A58D` [DISASSEMBLED 2026-09-25]

**The walk.** The order loader `0x43A420` has one caller, `0x487594`. It takes an order's type
from its `<key>_name` (`0x43C6B0`, then a case-insensitive `_stricmp 0x4F8A70` against the table). A name not found goes to
`0x43A552`, "Ready", with `dl` = 0. A record without the name falls back to its stored index. It
walks the order table `0x512344` counting records whose `+0x14` bit 0 is clear
(`0x43A556..0x43A58F`).

**The defect.** The walk runs while the record is at most the table's end `0x512348`
(`0x43A58B`, `cmp eax,esi; jbe 0x43A56D`). That is one record past the end. An index the walk
does not meet goes from `0x43A58F` to `0x43A598`, which leaves `dl` at the count
(`mov dl,cl`): 68 or 69 against a table of 68 records. That becomes the order's type byte, and a
wild dispatch on its first tick.

This exe's writer always stores the name (`0x43AA90..0x43AAE3`), so only a foreign or damaged
save reaches the walk. The table holds 68 records for the whole process:
`UIPipelinesInit 0x491200` fills it once. So the walk's first record, examined before the bound,
is always a real one.

**The fix, two bytes of one local site:**
- `jbe` → `jb` at `0x43A58D`, so the table's end is exclusive;
- the not-found `jmp` at `0x43A58F` → `0x43A552` (`EB 07` → `EB C1`), the by-name branch's own
  "Ready".

`ebx` and the other registers reach `0x43A59A` as before. Only `dl` differs, and only for an index
the table does not hold. It is not run: it rests on the disassembly.

## The order module — where an order's position lives, and in what units — mapped by us

[MEASURED 2026-09-04, this project — `objdump` of the pristine Steam build, plus a live A/B on
Two Continents. Established while fixing the scenario applier, whose orders all walked to the
map origin.]

**`ORDERS_NewMainOrder2Unit 0x43AFC0`** — stdcall, `ret 0x1C`, seven args
`(actionIndex, shift, unit, target, pos, p1, p2)`; `pos` is a pointer to **three 16.16
dwords**.

**Its first branch is on `shift`, and with `shift = 0` almost none of the function runs**
[BINARY-VERIFIED, corrected by the landing review — the first draft of this section described
the scan as unconditional]:

```
43afc0  mov  eax,[esp+0x8]      ; arg2 = shift
43afd4  test eax,eax
43afda  je   0x43b08a           ; shift == 0 -> straight to the allocator
43afe0  mov  esi,[edi+0x5c]     ; else walk the order list, head ALWAYS +0x5C
```

With `shift` 0 the `je` at `0x43AFDA` goes straight to `0x43B08A`, which re-pushes the seven
arguments unchanged — action, shift, unit, target, pos, p1, p2 (`0x43B08A..0x43B09C`) — and calls
the allocator **`0x43ADC0`** (`0x43B09D`), which takes `0x56`
bytes (`push 0x56; call 0x4B4F10` at `0x43ADC4`) and constructs the order in place with
**`0x43A0C0`** (`thiscall`, `ecx` = the new order). The scenario applier passes `shift = 0`, so
**that is the whole of its path** and everything below about the scan is context, not
description of what we do.

**What the `shift != 0` scan does when it matches is CANCEL, not merge**
[BINARY-VERIFIED `0x43B034..0x43B087`]: `0x43B03D` picks `unit+0x60` over `+0x5C` as the list
head to unlink from when the matched order's flag word `+0x42` has `0x40000` (this is the
*unlink* head — the walk itself started unconditionally at `+0x5C`), `0x43B05B..0x43B05E`
unlinks it, `0x43B075 call 0x43A1F0` destructs it, `0x43B07B call 0x4B4F20` frees it, and
`0x43B087 ret 0x1C` returns **without creating anything**. That is TA's shift-click-a-waypoint-
to-remove-it behaviour, not a merge. A node matches on its action byte `+4` (`0x43AFF2`), on its
target `+0x16` only when a target is passed (`0x43AFF7..0x43AFFE`), and on the ±16 wu position test
below only when a position is passed (`0x43B000`); with no match the seven arguments go unchanged
to `0x43ADC0` (`0x43B08A..0x43B09D`), which links the order without the wipe below
[DISASSEMBLED 2026-09-28].

**And `0x43ADC0` wipes the queue on the way in when `shift = 0`.** Only with its `shift`
argument 0 (`0x43AE02..0x43AE08`) and a new order whose flags `+0x42` lack `0x40` (`0x43AE0E`),
`0x43AE14..0x43AE59` walks `unit+0x5C` and destroys every existing order that lacks flag bit `0x4`
before linking the new one — so with `shift = 0` an order REPLACES the unit's orders rather than
queueing, and with `shift ≠ 0` the wipe is skipped. The link [DISASSEMBLED 2026-09-28,
`0x43AE5B..0x43AFB1`]: a new order without `0x40000` first drops the leading `+0x5C` orders that
carry `0x4000`; it gets `0x1`, and `0x2000` when `shift = 0`; an order with `0x20` or `0x40000` goes
to the head of its list (`+0x60` for `0x40000`, else `+0x5C`) and takes the old head's `0x4000`;
any other goes right after the `+0x5C` order carrying `0x1000`, taking that mark from it, or to the
tail when none carries it. The applier's `orders`
array therefore only ever takes effect in its last element; recorded in
[scenario format](../scenario-format.html).

**The position is copied verbatim, which is why a read-back probe cannot check it**
[BINARY-VERIFIED `0x43A164..0x43A177`]:

```
43a164  mov ecx,[eax]        ; eax = the caller's pos[]
43a169  mov [esi+0x22],ecx   ; Pos.X
43a16c  mov ecx,[eax+0x4]
43a16f  mov [edx+0x4],ecx    ; edx = esi+0x22  -> +0x26
43a172  mov eax,[eax+0x8]
43a175  mov [edx+0x8],eax    ;                 -> +0x2A
```

Three dwords, no scaling and no reordering. So writing the position and reading
`UnitOrders->Pos` back proves only that nothing mangled it — `passed == stored` is a tautology,
and the applier's phase-C "measurement" that concluded whole world units was reading exactly
that tautology.

**What settles the scale, directly.** `0x4815A0` takes a `pos` pointer and turns it into a
map cell [BINARY-VERIFIED, and this is the strongest evidence — it is a consumer, not an
inference]:

```
4815a0  mov eax,[esp+0x4]    ; the pos pointer
4815a5  mov ecx,[eax]        ; pos[0]
4815a7  mov eax,[eax+0x8]    ; pos[2]
4815aa  sar ecx,0x14         ; >> 20 = 16.16 -> world, then / 16 -> a CELL
4815ad  sar eax,0x14
4815b4..c8  bounds-check against main+0x14233 / +0x14237, the map's size IN CELLS
```

`sar 0x14` is only a cell index if the input is 16.16, and the two components it takes are
**0 and 2**, bounds-checked against the cell dimensions — so those are the ground plane and
the middle one is the altitude. `ScriptAction_Type2Index` agrees from the other side: at
`0x43FF5B..0x43FF6F` it reads the three HIGH words (`[esi+0x2]`, `[esi+0x6]`, `[esi+0xA]`) and
does `sar ebx,1` on the middle one before subtracting it from the third — altitude/2 against
depth, the engine's own screen-y term.

**The duplicate-order test at `0x43B006..0x43B029` corroborates it** (on the `shift != 0` path,
so not on ours) [BINARY-VERIFIED]:

```
43b004  mov ebp,[eax]          ; caller pos[0]
43b006  sub ebp,[esi+0x22]     ; minus the candidate order's Pos.X
43b009  add ebp,0x100000
43b00f  cmp ebp,0x200000
43b015  ja  0x43b02b           ; too far -> not the same order
43b017  mov ebp,[eax+0x8]      ; caller pos[2]
43b01a  sub ebp,[esi+0x2a]     ; minus Pos.Z
43b01d  add ebp,0x100000
43b023  cmp ebp,0x200000
43b029  jbe 0x43b034           ; near enough -> the match (cancel)
```

A tolerance of **±0x100000, which is ±16.0 in 16.16 — one map cell**. In whole world units it
would be ±1 048 576, i.e. the whole map and then some, which is not a tolerance at all; and it
too compares components **0 and 2** and never 1. The order position is therefore the same
`TPosition {x, altitude, depth}` that `UNITS_CreateUnit 0x485F50` takes. **There is no
create/order asymmetry**; `scenario-format.md` §"The coordinate asymmetry" claimed one from
TADR's `ConstructionKickout` and was wrong, and is corrected there.

**Live confirmation** [MEASURED 2026-09-04, `scenarios/shadow-air.json`, Two Continents]: with
whole world units, `1900 >> 16 == 0`, and every ordered unit set off for the map origin — a
Peewee told to `move` to `(1900, 1450)` walked north-west and five aircraft told to fly east
ended stacked in the north-west corner, one of them reading world `(1, 0)`. With the shift the
Peewee stops at `1902` and every aircraft reaches `4200`. `patrol` was broken the same way and
now loops its lane; it had looked like a separate defect and was not.

**Negative results.** `0x43A1F0` (the order destructor, called at `0x43B075`) and `0x4B4F20`
(the free, `0x43B07B`) were not chased further. Three bits of the order flag word
`unit_order+0x42` appear and none is named: `0x40000` picks the `+0x60` unlink head
(`0x43B034`), `0x10000` is OR-ed in at `0x43B068` when the cancelled order is not the head of
`unit+0x5C` (the head is kept in the fifth argument's slot, stored at `0x43AFE8`), as `0x43ADC0`'s
wipe does for every order it destroys but the old head (`0x43AE34..0x43AE3A`), and `0x4` exempts
an order from `0x43ADC0`'s wipe. `0x41074A` reads
`order+0x22`/`+0x2A`/`+0x26` and tests each against zero — an "is the position set" check, in
that order, which is itself consistent with 0 and 2 being the ground plane; what the caller
does with the answer was not chased. `0x43B0B0` is a **different** function
that also calls `0x43ADC0` — do not read the `call 0x43adc0` at `0x43B09D` and the one at
`0x43B120` as the same site. It keeps a factory's queue as count nodes (*Replay state design*,
in the demo recorder section).

### The reclaim sweep's engine side: arming, the cursor chooser's calls, the reclaim click and its tail, the resolver's reclaim case [DISASSEMBLED 2026-10-26..28]

What the reclaim sweep (`tagpu_sweep.c`, its hooks in `tagpu_route.c`; [gpu-status
§2.110](../modules/reclaim-sweep.html#2110-the-reclaim-sweep-tagpu_sweepc-tagpu_sweepgeoc-tagpu_routec-2026-10-28)) rests on, every fact from `objdump -d -M intel` of
`pristine/TotalA.exe.pristine` unless it says otherwise. The handler's head, the click routing and
the placement's own hooks are in the section above; this is what cursor mode 12 adds.

**Arming: mode 12 means "armed through the RECLAIM gadget".** The orders menu's handler (the
`0x419BE0` chain the section above lists) copies the actuated gadget's name (`0x49FED0` at
`0x419F47`: the record's 16 bytes at `+0x02`, through `0x4E4760` with a count of 16 [INFERRED: `strncpy`],
terminated at `+0x10`) and tests it with `strstr(name, "RECLAIM")` — the needle at `0x5016C4`, the call to
the C runtime's `strstr` `0x4E49B0` at `0x419F56` — so **any gadget whose name contains RECLAIM**
arms it (ARMRECLAIM, CORRECLAIM, a mod's GOKRECLAIM). With the gadget's `status_init` (`+0x138`)
0 it writes mode 1 (`0x419F71`) and jumps to the shared tail `0x41A06D`, which clears `main+0x2CC6`
bit 3 (`0x41A079`) and plays `specialorders` (`0x41A080`); otherwise mode `0x0C` at `0x419F8A` (`c6 81 c3 2c 00 00 0c`),
clears `main+0x2CC6` bit 3 (`0x419F96`) and plays `specialorders` (`0x5026E4`) through `0x47F1A0`
at `0x419F9D`, returning at `0x419FAE` (`ret 8`); a name without RECLAIM goes on at `0x419FB1`.
**So arming and disarming both play `specialorders`**, and neither touches bit 5, the
Shift-placement bit (`0x419F3D..0x419FAF`). **`0x419F8A` is the only store of `0x0C` to `main+0x2CC3`** (a scan of every store
to `+0x2CC3`), and the one register store, `0x419BC9`, is in the uncalled setter `0x419BC0`.

**The quickkey toggles it off.** The GUI pump `0x4A9FD0` hands the character ring's key to the
button handler `0x4A6AE0` (`0x4AA24C`, by the gadget-type table `0x4AA810`), which matches it against the gadget's `+0x13A` through `tolower`
`0x4E5A30` and `toupper` `0x4E71C0` (`0x4A70FB..0x4A7129`, the C-locale fast paths read as `+0x20`
/ `−0x20` on `A..Z` / `a..z`); a toggle (`+0x1B` bit `0x40`) flips `status_init`
(`0x4A7132..0x4A713E`), a radio (`0x10`) sets it, then the gadget is redrawn (`0x4A5F40`), its
radio group reset (`0x4A0340`: with a non-zero `assoc`, every other `id 1` gadget of that `assoc`
has its `status_init` zeroed and is redrawn, `0x4A036D..0x4A03E1`) and the key popped
(`0x4A716A`); the handler returns 1, the pump publishes the gadget's index at `GUI+0x60`
(`0x4AA675`) and the orders handler `0x419BE0` reads it (`0x419BFA`, `0x419F3D`). A click's release
inside the rect does the same (`0x4A6CB2..0x4A6CC8`) [DISASSEMBLED 2026-10-29]. So the RECLAIM
quickkey pressed while armed turns the toggle off and `0x419BE0`'s RECLAIM branch writes mode 1. A letter reaches that ring only through
`WM_CHAR`: the keydown translator `0x4C1D50(vk, sys)` reads Ctrl with `GetAsyncKeyState`
(`0x4C1D54..0x4C1D6A`), pushes nothing for a key with neither `sys` nor Ctrl (`0x4C20EA..0x4C20F4`),
and for `A..Z` pushes `vk + 0x69` under Ctrl (`0x4C2129..0x4C2136`) or the lower-case letter
(`0x4C213D`, reached only from `WM_SYSKEYDOWN`'s call). The sweep takes the quickkey's `WM_CHAR`
before the engine sees it, which keeps the whole key from it.

**The cursor chooser's two calls.** `0x48D220(order)` is stdcall (`ret 4` at `0x48D397`,
`0x48D3BE`, `0x48D410`) and reads its argument as a byte (`mov dl,[esp+0x28]`, `0x48D257`). It has
exactly two call sites in the image and no stored address: `0x499297` in the in-play handler,
after `push eax` at `0x499296` (`50 E8 84 3F FF FF`, the byte loaded at `0x499290`), and
`0x491D36`, after `push ecx` at `0x491D35` (`51 E8 E5 B4 FF FF`, the byte loaded at `0x491D2F`),
in `0x491D18..0x491D6C`, a copy of the handler's hover-and-cursor block inside `0x491CC0` --
**which nothing calls**: no `call` or jump in `.text` reaches it and no four-byte value anywhere in
the file falls in `0x491CC0..0x491D6F` [DISASSEMBLED 2026-10-28, an instruction scan and a literal
scan], so in the engine `0x499297` is the one call that runs. Both push a whole register of which
only the low byte is the mode, read nothing after the call but EAX, and reload `main` at once
(`0x49929C`, `0x491D3B`); each then compares the answer with `main+0x2CBE` and stores it
(`0x4992AD`, `0x491D4C`). The sweep rewrites the live call's rel32 to a wrapper that calls
`0x48D220` and may answer `0x13` instead, and leaves the dead copy alone — the engine's own "nothing", stored
by the handler at `0x49925D` and returned by `0x43E490`'s default at `0x43F098`.

**The router's hooks** (`tagpu_route.c`, written at attach, each byte-matched first): the live
call's rel32 above (`0x499298..0x49929B`; the dead copy's `0x491D37..0x491D3A` keeps the engine's
bytes), the before-observer on the handler's entry `0x499200`, the 12-byte stub over `0x4993BF`
(`E9 rel32` and seven `90`; the stub's three ways are the section above's), the right-press
branch's stub at `0x49939B` ([formation](../modules/formation.html)) and the hover query's rel32
at `0x499279..0x49927C` (the call `0x499278` to `0x48CD80`, re-pointed to a function that asks the
strategic icons' radar pick when the engine names no unit, [strategic icons](../modules/icons.html),
*The pick*; the dead copy's call `0x491D18` keeps the engine's bytes). No branch in `.text` lands on `0x4993C0..0x4993CA`,
the bytes the jump and the NOPs cover, and no absolute reference in the file names an address
there (a branch scan and a literal scan of the pristine file, 2026-10-28). The stub dispatches on
`main+0x2CC3`: 14 to the placement, 12 to the sweep, every other mode stock's.

**Mode 12's cursor, `0x43E490` case 12 (`0x43E65C`).** The unit def's `+0x245` bit `0x400`
(`0x43E65C..0x43E665`); the mapped test, inline (`0x43E66B..0x43E6CF`: the word at
`main+0x14273`, bit `main+0x2A43`, the player's `+0x80`/`+0x84` dimensions); then the feature at
the point, `0x43E6D5..0x43E754`: `0x4815A0` gives the cell; a word below `0xFFFB` is an anchor
whose def index is bounded by the count `main+0x14253` (`0x43E6F5`); `0xFFFE` is a footprint tile
whose anchor is `cell − (byte[+0xA]·W + byte[+0xB])` cells back (`0x43E71D..0x43E736`, 13 bytes a
cell), and the word found there must be below `0xFFFB` (`0x43E73C`) — **the hop's def index is not
bounded by the count, and nothing tests that the hop stays on the grid**. A def with `+0xFE` bit 7
(`0x43E75A`) answers `0x0B` (`cursorreclamate`, `0x43E763`); anything else goes on at `0x43E770`.
The sweep's own lookup (`tagpu_sweep.c`, `feature_of`) repeats the rule with the two missing
bounds added.

**The click and its tail.** With a cursor below `0x11` other than `0x0F` (select) the click
`0x498F70` issues the order
(`0x48CF30` at `0x499087`) and then, when the record's `+8` has bit 2 (Shift), sets
`main+0x2CC6` bit 5 and returns (`0x499092..0x4990A0`); otherwise it writes mode 1, clears bit 5
and resets STOP's radio group (`0x4990A3..0x4990F1`: `0x49FE60([[main+0x531]+4], "STOP")`, then
`0x4A6A40`). From `0x11` up it does nothing, but for the Interface Type 1 deselect in mode 1
(`0x499041..0x49906A`). **No sound on this path.** `0x4958C0(keep)` (`ret 4`) is that same tail as
a function: `keep` ≠ 0 → bit 5 (`0x4958CD`); else mode 1 (`0x4958D7`), bit 5 cleared, the STOP
reset (`0x4958FD..0x49591F`). It reads `[main+0x531]` and its `+4` with no test. **It has no caller
and no stored address in the image** (a call scan and a literal scan); the reclaim sweep calls it
after a commit, and the formation move after a commit made with Move armed (mode 2), each testing
`[main+0x531]` and its `+4` first.

**`0x49FE60(ctrls, name)` is a substring search.** `ret 8`; the count is `ControlsAry[0]`'s i16
`+0xB6` (`0x49FE67`), the walk starts at `ControlsAry + 0x15D`, record 1's name (`0x49FE7D`), and
each record's name is the haystack of `strstr(record name, name)` (`0x49FE83..0x49FE8F`, a
case-sensitive byte compare inside `0x4E49B0`); the first match returns its index (`0x49FEAE`),
the walk steps `0x15B` (`0x49FE99`) to the count (`0x49FEA2`), and a miss returns −1 (`0x49FEA4`).
So `"STOP"` finds the first gadget whose name contains STOP, and the sweep finds its RECLAIM
gadget by the same rule the arming uses.

**A gadget's rect after the load.** The face loader (`0x4A7F70`, [gui-gadgets](../gui-gadgets.html)
*The face a button draws*) keeps the face's GAF sequence at `+0x2F` (`0x4A8113`) and then writes
the base frame's width and height (`GAF_SequenceIndex2Frame 0x4B7F30(sequence, +0x13B)`, its
first two words) into the record's `+0x17`/`+0x19` (`0x4A812B..0x4A8136`, the store `0x4A812E`).
So a loaded button's rect is its face's, not the `.GUI` file's; the popup anchors to it.

**The order resolver's reclaim case, `0x43F0E0` mode 12.** The mode is `(byte)mode − 1` into the
jump table `0x4401EC` (`0x43F12D..0x43F14D`, table entry 11 = `0x43F4F7`); `edi` holds the target
unit throughout (`0x43F0E8`). The case: the def's `+0x245` bit `0x400` (`0x43F4FD`, else refused);
the feature at `pos` (`0x43F50B..0x43F58A`, the same lookup as above); with `+0x245` bit `0x800`,
the mapped bit and a reclaimable feature, `RESURRECT` (`0x43F5A5..0x43F612` → `0x44004C`); else
with the mapped bit (`0x43F623..0x43F67B`) and a feature whose def has `+0xFE` bit 7
(`0x43F67D..0x43F688`), `VTOL_RECLAIM` (`0x501B68`) when the def's `+0x241` bit 11 is set
(`0x43F68A..0x43F69F` → the name tail `0x4401C7`) or `RECLAIM` (`0x5016C4`, `0x43F6A5` →
`0x440183`, the name pushed at `0x440187` and resolved by `0x438760` at `0x44018F`); else, with a
target unit, `VTOL_RECLAIMUNIT` (`0x501B48`, `0x43F6B2..0x43F6C6`) or `RECLAIMUNIT` (`0x501B5C`,
`0x43FA33`); with none, the answer byte is 0 (`0x43F6AA..0x43F6AC` → `0x4401DC`, the store at
`0x4401E3`). **So with no target unit mode 12 answers a reclaim-family order or nothing.**
**The mapped test is the viewed player's, not the issuing unit's owner's**: the grid's dimensions
come from the unit's owner record (`[unit+0x96]`, `0x43F5AF`), but the explored word is
`main+0x14273` and its bit index the byte `main+0x2A43` (`0x43F5E2`, `0x43F5F0`) [DISASSEMBLED
2026-11-11]. For a unit of an AI seat, a feature's reclaim therefore resolves only where the
*watching human's* map is explored; a caller that needs the unit's own owner resolves
`RECLAIM`/`VTOL_RECLAIM` by name through `0x438760` and applies its own checks (`+0x245` bit
`0x400`, FeatureDef `+0xFE` bit 7, the feature in its owner's sight).

The block `0x43FF38..0x4401D9` is **not** mode 12's: it is reached only from the contextual case
(table entry 0, `0x43F9E9`, its Interface Type 0 arm at `0x43FE35`; a scan of every branch into
`0x43FE35..0x4401DB` from outside it finds only `0x43F9F6` and `0x43FA0D` there, and otherwise
the shared tails `0x44004C`, `0x440183`, `0x4401C2` and `0x4401C7`). It repeats the reclaim
choice — `RESURRECT` (`0x43FF40..0x44004C`), the mapped test and reclaimable feature
(`0x440065..0x44016E`, the `+0xFE` test at `0x440167`), `VTOL_RECLAIM` by `+0x241` bit 11
(`0x440170..0x440181`, the name loaded at `0x440176`; mode 12's own test is `0x43F68A..0x43F69F`, the one
`tagpu_sweep.c` cites) or `RECLAIM`
(`0x440187`) — and falls back,
for a def with `+0x245` bit `0x80` and a non-zero first dword of the unit, to `VTOL_MOVE`
(`0x5016CC`) or `MOVE_GROUND` (`0x5052E4`) by the same bit 11 (`0x44019D..0x4401D9`). That
fallback is the contextual click's move, not an answer mode 12 can give.

**`PositionInPlayerMapped 0x408090(player, pos)`** answers 0 off the player's grid: the column
`x >> 5` against `+0x80` and the row `(z − alt/2) >> 5` against `+0x84` (`0x4080B5..0x4080BF`),
`xor eax,eax; ret 8` at `0x4080C1..0x4080C4`. The line-of-sight fix's stub replaces
`0x408095..0x4080C0` (*Line of sight at the map's edge*, the census row `0x4080B9`), so with that
fix armed the bound is the stub's. The sweep asks it for every candidate feature, with the
representative reclaimer's player bounded to one of the ten player records first.

**The reclaim order's countdown, `0x404AD0`.** Each call finds the feature at `order+0x22`
(`0x421DA0` at `0x404AE9`); none (`0xFFFF`) logs "Reclamation failed" (`0x501638`, pushed at
`0x404AF8` with the kind 7, through `0x47F780`) and returns 8; a def without `+0xFE` bit 7 returns 8 (`0x404B2A`). The state byte `order+5` indexes
the table `0x404D8C` (states 0..5). State 1 sets the countdown `order+0x36 = 15 + (energy +
metal) / 2` (`0x404BBA..0x404BDC`: `fld [def+0xEC]`, `fadd [def+0xF0]`, `fmul` by −0.5 at
`0x4FC940`, `fsubr` 15.0 at `0x4FC944`, `0x4E43A0` [INFERRED: `_ftol`], the store at `0x404BDC`);
state 4 takes 2 off it per working call (`0x404C7C..0x404C84`, after `0x439E80(2)` at `0x404C77`)
and tests it at `0x404C89`: above 0 it carries on, at 0 or below it returns 1 without paying
(`0x404C8B`). The controller takes 1 as "next state" (the jump table `0x43BAA4`, entry 1 →
`0x43B897`, `inc byte [order+5]`) and loops, and the order's timer bit set by `0x439E80(2)`
(`order+6 |= 1`, `order+0xA = now + 2`) skips it there (`0x43B817..0x43B81D`) until the two ticks
are up; state 5 then pays out (`0x4237D0` at `0x404D66`, result 5) -- after the entry's lookup,
so a feature gone in between fails as above. The calls are two ticks apart and a countdown C0
takes `ceil(C0 / 2)` working calls, its payout `2 · ceil(C0 / 2)` ticks after the first
[DISASSEMBLED 2026-10-29; the timer skip assumes no `unit+0xBA` event is pending]. **No worker-time term**: nothing in `0x404AD0..0x404D8B` reads the
unit def's `+0x1FE` or `+0x212` or the feature's build time. Between the first call and the
payout the order sprays only while the countdown is above 15 (`0x404CB5`), from two reverse
emissions a call (`0x404D3B`, `0x404D4C`). **The spray's source box** (`0x404CC9..0x404D37`): the
anchor cell's corner, each cell coordinate `<< 20` (16 world units in 16.16, `0x404CCD..0x404CDA`);
the far corner that plus the def's `+0x94` and `+0x96` `<< 20` [INFERRED: FootprintX and
FootprintZ] (`0x404CEB..0x404D0D`); the bottom the ground height `0x485070` answers
(`0x404CE6`) and the top that plus the def's byte `+0xFA` `<< 16`, the height (`0x404D17..0x404D2D`);
both emissions are called with the same box and the nano piece's point (`0x404D37` stores the last
corner). The wiki's stage, whose ground is 0, sprays from the same box (`tasim_fx.c`
`spray_feature`, sim.js `sprayFeature`).

**The walk's arrival, `0x44DCB0(x, z)`** (thiscall, `ret 8`): 1 exactly on the border cells of
the rect at `this+0x08..+0x14` -- `x` equal to `+0x08` or `+0x0C` with `z` in `[+0x10, +0x14]`,
or `z` equal to `+0x10` or `+0x14` with `x` in `[+0x08, +0x0C]` (`0x44DCB9..0x44DCF7`), 0
otherwise [DISASSEMBLED 2026-10-29]. For a feature's reclaim that rect is the ring of cells round
the feature's footprint widened by the unit's [INFERRED: from the goal `0x44D8A0` builds], so the
unit works from where the footprints touch and BuildDistance is never read; the wiki's walk stops
on the same ring (`to_feat`). The sweep's split weighs a target by
`2 · ceil(C0 / 2)` ticks (`sweep_reclaim_ticks`).

**What the sweep calls and why each is safe to call there.** All from the in-play handler on the
game thread, where stock's click calls the same roots: the resolver `0x43F0E0` once a target and
builder before each order; `0x43ADC0(action, 1, unit, NULL, pos, 0, 0)` for every order (append;
`0x43AFC0` with Shift would toggle off a same-action node within ±16 units, *Contextual
cancellation*); `0x439F80` for the nodes a ring without Shift takes off, each re-found by a fresh
walk (no node pointer survives the destructor `0x43A1F0`); `0x4958C0(shift)` after a commit that
ordered or removed anything; `0x408090`, `0x485070` and `0x438760` (the three family names
`0x5016C4`, `0x501B68`, `0x5052F0`) as pure reads.

**Gaps.** The contextual block's `0x4401AA` test (the unit's first dword) is not identified. The
hovered-unit path of `0x43E490` case 12 from `0x43E770` was not traced. The radius feature scan
`0x47EA40` (callers `0x405B93`, `0x41564F`) is not used and its arguments are not mapped.

## Direct AI order activity observer

DISASSEMBLED against the pristine 3.1 executable: `0x408C40` has one direct
caller, the players phase at `0x465031`. It receives its AI object in ECX,
tests the object's owner record for active and controller 2 at
`0x408C45..0x408C50`, runs the group assignment `0x408830` every 30 calls,
and dispatches due planner objects from ten slots at AI+0x11 through their
vtable at `0x408C8E`. Group assignment classifies live units and calls
`0x480250` at `0x4088FA`; it does not directly construct orders in the
disassembled body. **The group assignment `0x408830`** [DISASSEMBLED
2026-11-01 and 2026-11-11, `0x408830..0x408914`, called at `0x408C68`]: for
each unit with `+0x110` bit 5 of its record's block `[+0x67, +0x6B]` it first
writes the standing states **directly into `+0x110`**: the move state, bits
18–19, = 1 for a def with `+0x245` bit 12 (CanCapture) and 2 otherwise
(`0x408854..0x40887E`), and the fire state, bits 20–21, = 2, fire at will
(`0x408884..0x40888E`). Then a unit still in group 0 (`+0xAC`) goes to group 1
when it is a structure (`+0x110` bit 29) -- 5 instead with `+0x110` bit 31 --
else 4 for a def with `+0x241` bit 6 (`builder`), else 8 with bit 11
(`canfly`), else 7 when def `+0x1C0` is above 0 (a signed word, `jle`
at `0x4088E9`: `minwaterdepth`, so a ship), else 3 with `+0x110` bit 31, else
none (`0x408894..0x4088FA`, each through `0x480250(unit, group)`). The def
bits' keys are the FBI loader's ([data keys](keys.html)). Groups 2, 6 and 9
are filled only by the planners' rebalancing (`0x407560` → `0x480250`, below).
**`+0xAC` is also the player's numbered group** (Ctrl+digit): the selection by
group reads it (`0x48D975..0x48DD85`), so an AI seat's group numbers and a
human's Ctrl groups are the same field [the selection code read only for that
field]. **Under Sight on an AI seat these show**: the group digit
`0x469CD1..0x469CF9` draws `'0' + +0xAC` under each unit of the viewed player
whose group is not 0 ([render.md](render.md), *the group digit*), so M0-P's
shots (2026-11-01, a replay with Sight on AI seats 1 and 2) show a white 4
under that seat's commander, and the human's commander none. `0x408BF0`
contains a similar assignment/planner loop without the controller gate;
nothing calls it (no `call`, no stored pointer to it).

`0x4089A0` is the final call on both branches of `0x408C40`: at `0x408C9A`
with argument 1 for AI, and at `0x408CA7` with 0 otherwise. These are its only
direct callers. It scans weapon targets and invokes target setters/resetters
`0x48A0A0`, `0x48A060`, `0x48A0F0`, with queries `0x49D120`, `0x48A190` and
`0x40B7B0`. **What one call scans** [DISASSEMBLED 2026-11-11,
`0x4089A0..0x408A96`]: `limit/30 + 1` units (the unit limit `main+0x37EE6`),
from the cursor `obj+0x39`, which steps through the record's block and wraps
from `+0x6B` to `+0x67` (`0x4089DC..0x4089FB`). A unit is scanned when
`+0xA6` ≠ 0, it is complete (`+0x104` = 0.0), `+0x110` bit 31 is set and its
fire state (bits 20–21) is 2 (`0x408A01..0x408A41`); each of its three weapon
slots when the slot byte `+0x1F` has bits 1 and 4 and the weapon's `+0x111`
bit 8 (`dropped`) is clear, and **with argument 0 also bit 26 (`commandfire`)
clear** (`0x408A88..0x408A96`). So the AI's argument 1 lets the scan target
CommandFire weapons on its own, and a human seat's 0 leaves them to orders. The
scan reads `obj+0`, `obj+0x39`, the record's block and the acquisition record
(`0x40B7B0`); it reads no planner slot and no group. Because human seats also run it, activity attribution ends on
entry to this phase, through the existing `tagpu_weapons.c:my_Acquire` wrapper.
A weapon reaction alone cannot prove planner liveness. Acquisition already owns
a guarded detour; adding an observer over that detour makes the limits integrity
check refuse startup. The recorder therefore calls no second patch installer there.

`0x43A0C0` is the common order constructor: ECX is the new node, returned in
EAX at `0x43A1C6`; `ret 0x18` at `0x43A1CF`. It sets action+4, state+5 and
event fields, stamps GameTime at node+0x46 (`0x43A12E`), and leaves the
owner+0x0E NULL (`0x43A193`). It runs no order handler. The high-level helper
`0x43ADC0` calls it at `0x43ADEF`; callers can insert the result inline,
so observing only list-insertion functions misses new AI orders.

The four examined insertions are `0x43AC60` (insert before the supplied node,
ret12), `0x43ACB0` (head, ret8), `0x43AD10` (tail, ret8), and `0x43AD50`
(priority position, ret8). Each stores the owner at node+0x0E and links through
node+0x4A. Their direct callers are listed below; no direct caller of
`0x43AD50` was found in the disassembly.

- `0x43AC60`: `0x43BA1E`, `0x43BA30`.
- `0x43ACB0`: `0x401C99`, `0x401CAC`, `0x401F59`, `0x401F6E`, `0x4032F7`, `0x40330A`, `0x40529A`, `0x4052B2`, `0x405C06`, `0x405D18`, `0x405D33`, `0x406108`, `0x40611B`, `0x406532`, `0x40667A`, `0x406695`, `0x40F92B`, `0x40F943`, `0x40FEC3`, `0x410109`, `0x410124`, `0x4105D0`, `0x410A2A`, `0x410B0D`, `0x41101A`, `0x411EE1`, `0x411EF9`, `0x412692`, `0x412C14`, `0x413365`, `0x413380`, `0x413B5A`, `0x415453`, `0x4155EA`, `0x41578B`, `0x4157A6`, `0x489E97`, `0x489EA6`, `0x48A044`, `0x48A051`.
- `0x43AD10`: `0x410384`, `0x411FBE`, `0x411FD6`, `0x41203E`, `0x412056`, `0x412786`, `0x4127E2`, `0x4127FA`, `0x412DAE`, `0x412DC6`, `0x412E28`, `0x412E40`, `0x4134D9`, `0x4134F1`, `0x413551`, `0x413569`.
- `0x43AD50`: no direct caller.
- `0x43A1F0`: `0x4384E4`, `0x439EF3`, `0x439F5F`, `0x439FC9`, `0x43AE47`, `0x43AEB8`, `0x43B075`, `0x43B1C9`, `0x43B93B`, `0x43BA5B`, `0x43BBE6`, `0x43BC49`.

At the existing normal dispatcher observation sites `0x43B878` and
`0x43BB1D`, ESI names the live order node; both read node+0x0E and call
its handler. Destruction has a separate site, `0x43A216`, which is excluded
from execution evidence. The destructor `0x43A1F0` receives the node in ECX
and reaches any event-2 handler only after `0x43A1F5`. Forgetting a sample
at entry establishes its lifetime before an allocation can reuse its address.
The observer dereferences only the current dispatcher node, never a saved
sample pointer, and bounds its owner against the level's unit allocation.

DISASSEMBLED: the secondary controller `0x43BAD0`, called at `0x48AF9E`, reads
the unit's `+0x60` list at `0x43BADA`. A node with nonzero `+6` waits until
GameTime reaches its `+0x0A` deadline (`0x43BAE8..0x43BAFD`). Dispatch clears
`+6` at `0x43BB09`, pushes event 0 and the current node at `0x43BB15..0x43BB16`,
then reads its owner and calls the handler at `0x43BB1D..0x43BB21`. Removal
calls the destructor at `0x43BBE6` or `0x43BC49` before `0x4B4F20` frees the
node. The controller is inside the unit tick's owner-type-1/2 block, beside
the primary controller at `0x48AF98`.

MEASURED on the reference setup, 2026-10-04, normal coverage build: native
selection of the isolated ARMSOLAR at screen `(512,384)`, then Ctrl+D, removed
that unit while the two remote units stayed alive. The coverage bitmap reached
`ev_nano_h2`. This covers the secondary dispatch callback; a human self-destruct
does not count as a planner-originated AI order.

Observer stolen prefixes: `0x408C40` five bytes `53 56 57 8B F9`;
`0x43A0C0` seven bytes
`83 EC 0C 8A 44 24 10`; `0x43A1F0` five bytes `53 56 8B F1 57`.
All are complete instructions without relative operands. The normal-dispatch
observers already exist for the recorder's nano context.
`ai_hooks_init` checks all three current prefixes with `tagpu_detour_bytes_ok`
before installing any observer. `tagpu_detour_observe` alone compares an
already registered Impure stub; its first-install path does not match the
engine's bytes. A mismatch therefore leaves all three new observers uninstalled,
and the harness refuses AI activity observation.

#### The AI object, its lifecycle and its ten planners [DISASSEMBLED 2026-11-11]

**The object** at record `+0x74`, `0x3D` bytes, constructed by `0x408CB0` (`thiscall(obj, rec)`,
`ret 4`): `+0` the record, `+4` the record's `+0x146` byte, `+5` the group assignment's countdown
(30), `+9` and `+0xD` zeroed (`+0xD` is a GameTime the builder planner's commander waits for,
`0x40817D..0x40818B`; its one writer is the on-damage handler `0x406F80`, below), `+0x11..+0x38` ten planner slots, `+0x39` the weapon scan's cursor
(`0x408CBB..0x408CDD`). Each planner is a small object: `+0` its vtable {tick, destructor}, `+4`
the AI object, `+8` its group, `[rec+0x78] + 0x20·g` (the group table `0x480190` allocates, ten
`0x20`-byte groups), `+0xC` the GameTime it is next due, `+0x10` the record's `+0x146` byte.
`0x408C40` calls a slot's tick when `[slot+0xC]` ≤ GameTime `main+0x38A47`
(`0x408C75..0x408C94`); the AI object's destructor `0x408F10` calls each slot's destructor.

**Lifecycle.** The player init `0x464700` makes the object for every record but an active type 3
— **human seats get one too**, for the weapon scan — and the teardown `0x464A00` frees it (both:
[memory](memory.html), [recorder](recorder.html) *The takeover*). In between it is never freed: the
seat activation `0x464290` zeroes `+0x74` without a free, and the `AI` chat command `0x416280`
flips `+0x73` between 1 and 2 without touching it. `0x4648E0` gives each active type-2 record its
profile: it loads `ai\<aiprofile>.txt`, the map's OTA key `aiprofile` (`0x504B18`, read at
`0x4366D5..0x436729` into the map struct's `+0x804`, default `default`), falling back to
`ai\default.txt` (`0x507308`, `0x46490C..0x464916`), then applies `0x409F80(i)` and `0x40A040(i)`
(`0x46494E..0x464982`). The tick `0x408C40` is reached from one instruction only, `0x465031`
(`call`, `E8 0A 3C FA FF`): no other call, no stored pointer to it, and no branch into the rel32
bytes `0x465032..0x465035`. At that call `ECX` is the AI object (`0x46502A`), `EDI` the record and
`ESI` the record's slot index, below 10. A puppet seat (active type 3) holds no object, so nothing
reached from `0x465031` runs for a puppet.

**The ten slots** (the constructor `0x408CB0..0x408F00`; vtables read from `.rdata`):

| Slot | Group | Object, vtable | Tick, period | What the tick does |
|---|---|---|---|---|
| `+0x11` | 0 | none (NULL) | — | — |
| `+0x15` | 1, structures | `0x14` B, `0x4FC9B0` | `0x4086D0`, every 30 | each complete structure (`+0x110` bits 29 and 28, not 14): a metal maker (`+0x22D`) is switched **off** through `0x48B090(unit, 1, 0)` when twice the stored metal `rec+0x98` is at least the stored energy `rec+0x8C`, left alone when the net energy `0x464AD0(rec)` is not above 0, and otherwise switched **on** when `0x4B6C30(5)` is not 0 (`0x40871E..0x40877F`); a factory (a build list, `+0x152`) with no order queues one unit of the type `0x40BDB0(seat, unit)` picks, through `0x419B00(name, unit, 1)` (`0x408781..0x4087C4`). `0x408620` and `0x408670` are uncalled copies of these two |
| `+0x19` | 2, attack force | `0x28` B, `0x4FC988`; `+0x14` 3, `+0x18` 6, `+0x1C` 20000, `+0x20` 3 | `0x4077E0`, every 300 | `0x407560(3, 20000)` pulls units from group 3 into group 2; at three units or fewer it moves (mode 2) to the centroid of group 5, 1 or 4, the base; at six or more, or while attacking, it attacks (mode 3) the unit the target picker names, all through the group order `0x480460` |
| `+0x1D` | 3, armed mobile | `0x18` B, `0x4FC990`; `+0x14` 2 | `0x4079F0`, every 150 | moves group 3 (mode 2) to group 2's centroid |
| `+0x21` | 4, builders | `0x14` B, `0x4FC9A8` | `0x408100`, every 90 | the builder planner, below |
| `+0x25` | 5, armed structures | `0x14` B, `0x4FC980` | `0x407380` = `ret` | nothing; its `+0xC` stays 0, so it is due every tick |
| `+0x29` | 6, second force | `0x28` B, `0x4FC988`; `+0x1C` 50000 | `0x4077E0`, every 300 | as group 2, pulling from group 7 |
| `+0x2D` | 7, naval | `0x18` B, `0x4FC990`; `+0x14` 6 | `0x4079F0`, every 150 | follows group 6 |
| `+0x31` | 8, aircraft | `0x14` B, `0x4FC998` (constructor `0x407350`) | `0x407AE0`, every 30 + rand(900) | below five units, patrols (mode 9) over the target picker's unit, else random points appended with Shift; otherwise moves and patrols to random map edges |
| `+0x35` | 9, roamers | `0x3C` B, `0x4FC9A0` (constructor `0x407D40`) | `0x407E90`, every 30 + rand(150) | wanders to a point drawn from the sim RNG; whether the point is seen is the record's own sight counters `+0x7C` when `main+0x14281` bit 1 is set, else `0x408090`, the **viewed** player's explored bit (`0x407F5C..0x407FC2`); each CanAttack unit gets mode 3 at the point, issued only when the resolver answered (`0x40804C..0x408064`) |

Each tick first stores GameTime plus its period into its `+0xC` (`0x4086E2`, `0x4077F7`,
`0x407A07`, `0x40811B`; the aircraft and roamer periods draw `0x4B6C30(900)` and `0x4B6C30(150)`
at `0x407AF2` and `0x407E9C`). The work of the ticks other than the builder's is as read from
their bodies, not each branch traced. **The builder planner `0x408100`** takes the base point
from `0x40BA80(seat, &base)`; for each group-4 unit with a CANBUILD list (`+0x152`), a CanCapture
unit (the stock commander) waits while `0x40C230(seat)` ≥ 5 or GameTime is below the AI object's
`+0xD`; a unit whose head order has `+0x42` bit `0x8` is skipped; `0x40BDB0(seat, unit)` picks the
type and `0x40BFE0(seat, &pos, def, &site)` the site, which a CanCapture unit refuses beyond
(W+H)/3; then `0x43F0E0(&a, 14, unit, 0, &site)` and `0x43ADC0(a, 0, unit, 0, &site, type, 1)`
(`0x408292..0x4082B0`) — **without testing the resolver's byte**. An idle unit (no head order, or
a head with `+0x42` bit `0x4000`) patrols around the base: a CanCapture unit first moves to a random
point within 640 world units of it. [INFERRED: the meanings of `0x40BA80`, `0x40BDB0`, `0x40BFE0`
and `0x40C230`, read only at their call sites.]

**The target picker `0x4071F0`** (`thiscall` on the AI object, the point by value, `ret 0xC`)
returns the unit nearest the point over every record that is active, of type 1–3, `+0x146` ≠ 10
and not allied (the `+0x108` row): alive (`+0x110` bit 28), `+0x110 & 3` ≠ 2, not bit 15, not
`+0x10E` bit 2 (`0x407207..0x407330`). **It tests no sight, radar or explored map** — its body reads
neither `main+0x14273` nor a record's `+0x7C` and calls only arithmetic — so the stock AI's attack
targets come from every enemy unit on the map. Its callers are the force planner (`0x407901`) and
the aircraft planner (`0x407B74`).

**The stock planners draw the simulation RNG** `0x4B6C30` (`0x407AF2`, `0x407E9C`, `0x4083A9`,
`0x4086A5` among others), so a seat whose planners do not run draws a different stream from one
whose do.

#### The stock AI's helpers: the base, the build choice, the build site and the scan [DISASSEMBLED 2026-11-11]

The planners' `seat` is the AI object's `+4`, the record's `+0x146`; the helpers use it both as the
index of the acquisition record `acq` = `0x5119C0[seat]` and as the record's index
(`main+0x1B63 + seat·0x14B`). Float constants were read from `.rdata`.

**The acquisition record** (`0x10D` bytes, constructor `0x409160`; made by `0x40B320`, freed by
`0x40B390`, [recorder](recorder.html) *The takeover*) is the stock AI's world model:

| Offset | What | Written by | Read by |
|---|---|---|---|
| `+0` / `+4` | the record / the seat | the constructor | |
| `+0x05` vector | **enemy units the seat sees** | the scan `0x40AA40` | `0x40AD80`, the roamer's threat sum `0x40B1C0` |
| `+0x15` vector | enemy units with unit `+0x110` bit 8 — the **viewed** player's radar contact, not this seat's | the scan | `0x40AD80` |
| `+0x25` vector | own complete air pads (`builder` and `isairbase`, activated) | the scan | `0x40B530` |
| `+0x35..+0x3D` | **the base point**, 16.16 | the scan (`0x40AD02..0x40AD0B`) | `0x40BA80`, `0x40BFE0` |
| `+0x4D` vector | **the metal spots**, `{u16 x, u16 z, f32 metal}` | `0x40A7B0` | the spot search `0x40A260` |
| `+0x69 + 3·type` | three signed bytes per type: t0 general, t1 metal, t2 energy | the rating `0x409730` | the score `0x40BB00` |
| `+0x75` | **own complete units with a build list** (`+0x156` ≠ 0) | the scan | `0x40C230` |
| `+0x79` | 1 = owns an activated `istargetingupgrade` unit | the scan | `0x40AD80` |
| `+0x81` | per type: own complete count | the scan | the rating, the score |
| `+0x91` | per type: the value score v | the rating | the rating, `0x40B1C0` |
| `+0xA1` | per type: base weight, 40 for `bmcode` 0 plus 20 for a build list | `0x409470` | the scan |
| `+0xB1` | per type: the profile's weight percent, default 100 | `0x409470`, `weight` `0x409DC0` | the score |
| `+0xC1` / `+0xE1` | per type: weight / limit locked by a `weight` / `limit` naming the exact unit | `0x409DC0` / `0x409E90` | |
| `+0xD1` | per type: the build limit, −1 for none | `0x409470`, `limit` `0x409E90` | `0x409F20` |
| `+0xED` | GameTime of the last scan | the tick | the tick `0x40B2C0` |
| `+0xF1..+0x105` | the build lattice: eight draws of the simulation RNG at creation (`0x40A150`) | | the general search `0x40A5D0` |
| `+0x109` | the site-search radius | `0x40BFE0` | `0x40BFE0` |

**The tick `0x40B2C0(i)`** (one caller, `0x465037` in the players phase, for every record that has
an acquisition record, humans included) runs the scan once 30 ticks have passed, then draws
`0x4B6C30(30)` (`0x40B301`) and on 0 re-runs the rating `0x409730`, about every 900 ticks. **The
scan `0x40AA40`** walks every live, not dying unit slot: a **non-allied** unit goes into `+0x05`
when `UnitInPlayerLOS 0x465AC0(rec, unit)` passes and it is not `+0x110` bit 15, and into `+0x15`
when it has `+0x110` bit 8; an **own** complete unit counts into `+0x81`, `+0x75`, `+0x25`, `+0x79`
and the base. Allies' units are ignored. **The base point** is the centroid of the own complete
units weighted by `+0xA1`, so of structures and builders only; with none it is (0, 0, 0), the map's
corner. `0x40BA80(seat, out)` copies it (callers: the builder and aircraft planners).

**The build choice `0x40BDB0(seat, builder)`** (stdcall, `ret 8`, a type or 0) walks the builder's
build list; for each type with a score w = `0x40BB00(seat, type)` above 0 it adds w to a running
total and takes the type when `0x4B6C30(total)` < w (`0x40BDE7..0x40BE06`): **a weighted random
pick, each type in proportion to its score**, one draw of the simulation RNG per scored entry. A
pick of another side than the builder's returns 0 (`0x40BE5A..0x40BE8B`). Callers: the builder
planner (`0x4081A7`), the structure planner (`0x408797`) and the uncalled `0x408620`.

**The score `0x40BB00(seat, type)`** (stdcall, `ret 8`) is 0 while the stored energy `rec+0x8C` is
below 50 or the stored metal `rec+0x98` below 25 (`0x40BB2A`, `0x40BB51`), for a `downloadable` type
in a campaign, or past the type's limit (`0x409F20`: the type below `UNITINFOCount`, and the own
count below `+0xD1` unless that is −1). Otherwise an energy need E = (min(energy storage, 1000) −
stored) × 0.125, plus 20 when the net energy is below 1, plus 100 when the income is below 50 or 10
below 200; a metal need M = (min(metal storage, 500) − stored) × 0.25, plus 20 when the net metal is
below 1, plus 100 when the income is below 3 or 20 below 5. Metal is served first: Mw = clamp(M, 0,
100), Ew = clamp(E − Mw, 0, 100), Ow = max(0, 100 − Mw − Ew), and the score is (Ow·t0 + Mw·t1 +
Ew·t2) × `+0xB1` ÷ 10000 (`0x40BBB6..0x40BDA5`).

**The rating `0x409730`** (no RNG) sets per type: v (1, or 11 for an extractor, +10 for a metal
maker, +10 for an energy producer, plus cost terms; plus 1, or 11 for `canattack`, and per weapon
with an ID range ÷ 100 + damage ÷ 40 + 5; clamped to ±100); t0 (1, or 21 for `canattack`; +30 for a
builder while fewer than 3 are owned, +50 an energy producer, +50 an extractor, +25 a metal maker,
+40 `canfly`, +15 sonar, +5 radar, plus `energymake` up to 30; × 4 when none is owned, × 2 when one;
× 3 when def `+0x1C0` ≥ 0; + v/2 when the record's live count is above half the per-player limit; 0
for a transport, a feature, or a wind generator on a map whose maximum wind is below half the
nominal; at most 100); t2 = clamp(−0.0025 × energy cost − 5 × `0x488F30(def)`, 0, 100); t1 =
clamp(100 for an extractor + 25 for a metal maker − 0.02 × metal cost, 0, 100). [The signs of v's
cost terms rest on objdump's `fsubp` operand order.] The profile (`ai\*.txt` and the FBI's
`ai_weight`) only scales `+0xB1` and sets `+0xD1`; `weight` applies to every seat with an AI object,
`limit` to type-2 seats only.

**The build site `0x40BFE0(seat, pos, def, site)`** (stdcall, `ret 0x10`, 1 on success): the radius
`+0x109` grows by 160 a call, up to the map's larger dimension, while searches fail, and is reset to
0 on success (`0x40BFF8..0x40C01E`, `0x40C1E3`). The centre is the base point, or the point that far
from the builder towards the base. An extractor draws `0x4B6C30(255)` (`0x40C155`) and, when the
map's `SurfaceMetal` is below the draw, takes the **spot search** `0x40A260` (the metal spots within
4·r cells, nearest first, the richest accepted one; no RNG); otherwise the **general search**
`0x40A5D0`: up to 30 tries (`0x40A77A`), each a random distance and angle (`0x40A673`, `0x40A682`)
snapped to the seat's random lattice and jittered (`0x40A705`, `0x40A736`), four draws a try,
accepted by the site test and refused when the site's metal is above twice the background
[INFERRED intent: spots are kept for extractors]. The site is the footprint centre `(2·cell + foot)
<< 19`; its y is not written. **The site tests pass player 0, so they read no sight and no explored
map: the stock AI builds on ground it has never seen**, and its spot list (`0x40A7B0`, built once a
level from `0x4649ED`) is every metal feature cell on the map.

**The builder count `0x40C230(seat)`** returns `+0x75`, the seat's complete units with a build list
— commander, constructors and factories alike. The commander (the CanCapture unit) builds while it
is below 5 and roams once it reaches 5 (`0x408169`, `0x40830E`). **A hit commander stops
building**: the on-damage handler `0x406F80` (caller `0x489DA2`) sets the AI object's `+0xD` to
GameTime + 30 + `0x4B6C30(300)` and purges the commander's orders through `0x439EB0(unit, 0)`
(`0x406FC9..0x406FEE`).

**The force planners' rebalance `0x407560(src, cap)`** (thiscall, `ret 8`; one caller `0x407804`)
takes the first unit of group `src` into an empty group, sends a unit back to `src` while it is
farther than √(cap·n) from the group's centroid, then takes in every `src` unit within that
distance (`0x407560..0x4077CF`): √(20000·n) for group 2, √(50000·n) for group 6. No RNG, no sight.
The attack rule (three units or fewer: regroup at the base; six or more, or more than three while
already attacking: attack the target picker's unit) runs every 300 ticks.

**RNG and sight, in one place.** The helpers that draw the simulation RNG: the build choice (one
per scored entry), the build site (one for an extractor, four a try), the acquisition record (eight
at creation, one every 30 ticks for every seat that has one, humans included), the metal maker, the
builder's idle roam (`0x4083A9`, `0x4084CA`), the hit commander (`0x406FCE`) and `0x40B7B0`'s weapon
re-find (`0x40B8A2`, `0x40B9D8`). The base point, the score, the rating, the builder count, the
rebalance, the scan and the spot search draw none. Only the scan reads other players' units through
sight (`0x465AC0`), plus the viewed player's radar bit; the target picker reads every enemy with no
test, and the site tests no sight at all.

#### The group order `0x480460`, and the standing states

**`0x480460(rec, group, mode, shift, target, pos*, p1, p2)`**, stdcall, `ret 0x20`: walks the
record's block `[+0x67, +0x6B]` and, for each unit with `+0xA6` ≠ 0 and `+0xAC` = `group` (no alive
test), calls `0x43F0E0(&p2_slot, mode, unit, target, pos)` and then `0x43ADC0` with the byte it
wrote (`0x48047E..0x4804BB`). **It never tests the resolver's answer**: a refused unit gets action
0, `Ready` ([render](render.html), *`0x512344`*), and with `shift` 0 that order wipes its queue
first. Its callers are the stock planners only: `0x4078C8`, `0x40791F`, `0x407A83`, `0x407C50`,
`0x407D2C`.

**The standing states are orders too.** `Standing_MoveOrder` and `Standing_FireOrder` (records
`0x4FC526` and `0x4FC53F`, status `Acknowledged`, flags `0x10060`: bit `0x40` exempts them from
`0x43ADC0`'s wipe, bit `0x20` links them at the head) have the handlers `0x4030D0`, which writes the
node's `+0x36 & 3` into unit `+0x110` bits 18–19, and `0x403100`, which writes it into bits 20–21
and, for states 0 and 1, clears every weapon slot's target (`0x403129..0x40313A`); both return 5.
The player's buttons issue them through `0x48CF30`, which requires def `+0x245` bit 0
(`mobilestandorders`) for the move order and bit 1 (`firestandorders`) for the fire order
(`0x48D0B7..0x48D10B`). The stock AI does not issue them; its group assignment writes the bits
directly (above). **A new unit's states come from its def**: the spawn initialiser `0x485A40`
writes bits 18–19 from def `+0x241` bits 0–1 (`standingmoveorder`, `0x485C8A..0x485C9D`) and bits
20–21 from bits 2–3 (`standingfireorder`, `0x485CA3..0x485CB6`), both 2 by default; its three
callers (the create `0x485F50`, `CreateFromNetwork 0x4861D0` and `0x485E90`) write neither
before. A unit being built then takes its builder's states (BuildingBuild `0x40294D..0x40298C`,
GetBuilt `0x402E82..0x402EC2`) [which unit is the builder in each, from [units](units.html)].

#### Calling the order roots from the AI's tick [DISASSEMBLED 2026-11-11]

What makes an order-root call from inside `0x408C40` (or a replacement reached from `0x465031`)
correct by construction, and which roots fit an AI:

- **Ordering.** `0x408C40` runs in the players phase `0x464F80` (step call `0x49555F`), which each
  step reaches only after the unit tick `0x48AD30` (`0x4954ED`) has returned ([sim](sim.html), *Each
  iteration, in order*). The order controllers `0x43B7C0` and `0x43BAD0` run inside the unit tick,
  so no order list is being walked while a root links or wipes nodes. The stock planners call
  `0x43F0E0`, `0x43ADC0` and `0x43B0B0` (through `0x419B00`) from this same frame.
- **Lifetime.** `0x43ADC0` without Shift destroys nodes through `0x43A1F0`, which can run handlers
  (*Contextual cancellation*, below): no order-node pointer survives a root call, and a unit
  pointer is re-derived from its ID after each call.
- **Bound.** No root validates its unit: the resolver dereferences `unit+0x92` and `+0x96` untested
  (`0x43F112`, `0x43F131`) and `0x43ADC0` validates nothing. The unit must be
  `array + ID·0x118` with the ID below `u16 main+0x14351`, inside `[main+0x14357, main+0x1435B]`
  (*Input bounds and layout*, below), and live (`+0x110` bit 28). A target, once linked, is held
  by the engine's own reference chain (`0x4895C0` at `0x43A0E3`; the death walk `0x489740` tells
  its holders, [units](units.html)).
- **Thread.** The players phase runs on the game thread every step and **once on the loader
  thread** at the end of a level load (`0x497C10`), before the loader's done bit
  `main+0x38D75` bit 1 is set (`0x497C62`), which the game thread waits for before its first tick
  ([sim](sim.html)): single-writer by that ordering. The bit is a one-frame flag: the in-play entry clears the whole word at `0x498480` after the level's first in-game frame ([units](units.html), the load flags), so it marks the order of that first frame and cannot gate "loaded" during play.

**Which root for which order:**

| Order | Root | Note |
|---|---|---|
| any action by name | `0x438760` | the table never changes in a process, so once is enough |
| move, attack a unit or the ground, guard, repair or assist, patrol, stop, build at a site | `0x43F0E0` with mode 2, 3 (target NULL for the ground), 7, 8, 9, 10, 14, then `0x43ADC0` | test the resolver's byte and issue nothing on 0. Mode 8 has no alliance test (*What each resolver mode answers*); a build's `pos` is the footprint centre `(2·origin + size) << 19`, as the manual build `0x4196D8..0x419714`, with `p1` the type and `p2` 1, as the builder planner |
| reclaim a feature | `0x438760("RECLAIM" / "VTOL_RECLAIM")` and `0x43ADC0` | not mode 12, whose map test is the viewed player's (*The order resolver's reclaim case*); a unit target's reclaim is mode 2 against an enemy |
| a factory's queue | `0x43B0B0(BUILDINGBUILD, unit, type, count)` | the type bounded below the def count; not `0x419B00`, which plays the build sound for the viewed player and finds the type by a linear scan (`0x488B10`) |
| the standing states | `0x43ADC0` with `Standing_MoveOrder` / `Standing_FireOrder`, or the stock group assignment | fire state 2 is what the weapon scan needs |
| not for an AI | `0x480460`, `0x43AFC0`, `0x48CF30` | issues `Ready` on a refusal; toggles a queued node with Shift; walks the selection of `main+0x2A42` |

`0x43AFC0` and `0x48CF30` are the player's roots: `0x43AFC0`'s two callers are `0x4197A9` and
`0x48D1F4`, and no AI code calls either. The LOS fix (`tagpu_patches.c`) patches tests inside
`0x43F0E0`; a call by address goes through it.

#### What each resolver mode answers [DISASSEMBLED 2026-11-11]

`0x43F0E0`'s mode is `(byte)mode − 1` into the jump table `0x4401EC` (`0x43F12D..0x43F14D`);
anything outside 1–14 is refused. Before it, a non-NULL target must be alive (`+0x110` bit 28,
`0x43F0F4`, else refused), and the alliance byte of the unit's record for the target's owner
(`[unit+0x96] + 0x108 + [target+0x96]+0x146`) sets `eax` = 1 (enemy) when it is 0 and `ebx` = 1
(ally) otherwise (`0x43F104..0x43F128`). "VTOL" answers are chosen by def `+0x241` bit 11; "no
mover" is a unit whose first dword is 0.

| Mode | Case | Needs (def `+0x245` bit) | Answers |
|---|---|---|---|
| 1 | `0x43F9E9` | — | the contextual order (the Interface Type arms, *The order resolver's reclaim case*) |
| 2 move | `0x43F845` | 7 `canmove` | no mover → `QMOVE`; no target → `VTOL_MOVE`/`MOVE_GROUND`; with a target, by its kind: `CAPTURE`, `VTOL_RECLAIMUNIT`/`RECLAIMUNIT` (an enemy), `HELPBUILD`/`REPAIRUNIT` (an ally `0x4899B0` accepts), the pickups (`0x489A90`), `VTOL_FOLLOW`/`FOLLOW_GROUND` (an ally), else the move (`0x43F893..0x43F9E4`) [the order of these tests as read, not each branch traced] |
| 3 attack | `0x43F154` | 4 `canattack` | an enemy target: by both units' air and water state `AIRSTRIKE`, `AIRTOAIR`, `AIRTOGROUND`, `AIRTOGROUNDHOVER`; a mover → `ATTACK_CHASE`; a structure → `ATTACK_NOMOVE`; `ATTACK_KAMIKAZE` for `+0x241` bit 28. **No target, or an allied one: the ground attack**, refused when weapon 1's `+0x111` bit 17 (`toairweapon`) is set (`0x43F17E..0x43F189`), else `SUPPRESS` for a non-flyer and `AIRSTRIKE`/`AIRTOGROUND` for a flyer |
| 4 | `0x43F7E8` | 14 `candgun` | `ATTACKSPECIAL` |
| 5 unload | `0x43F735` | 8 `canload` | `VTOL_UNLOAD`/`GROUND_UNLOAD` |
| 6 load | `0x43F701` | a target `0x489A90` accepts | `VTOL_PICKUP`/`GROUND_PICKUP` |
| 7 guard | `0x43F4C7` | 5 `canguard`, **and an allied target** (`ebx`, `0x43F4D4`) | `VTOL_FOLLOW`/`FOLLOW_GROUND` |
| 8 repair | `0x43F46C` | `0x4899B0(target)`: a target, the builder's bit 9 (`0x4899CC`), the target's health `+0x108` below def `+0x1FA`, and further tests | target `+0x104` ≠ 0.0 (unfinished) → `VTOL_HELPBUILD`/`HELPBUILD`, else `VTOL_REPAIRUNIT`/`REPAIRUNIT`. **No alliance test**: an enemy target is accepted |
| 9 patrol | `0x43F3B9` | 6 `canpatrol` | no mover → `QPATROL`; bit 9 → `VTOL_REPAIRPATROL`/`REPAIRPATROL`; else `VTOL_PATROL`/`PATROL` |
| 10 stop | `0x43F82C` | **none** (bit 3, `canstop`, is not tested) | `STOP` (`0x502714`) |
| 11 | `0x43F813` | none | `TELEPORT` |
| 12 reclaim | `0x43F4F7` | 10 `canreclamate` | *The order resolver's reclaim case*, above |
| 13 capture | `0x43F6D1` | 12 `cancapture`, a target of another record | `CAPTURE` |
| 14 build | `0x43F7A0` | def `+0x156` (a CANBUILD list) and a mover | `VTOL_MOBILEBUILD`/`MOBILEBUILD` |

Bit 9 is **not a key**: the FBI loader writes it as a copy of bit 10, `canreclamate`
(`0x42CA3B..0x42CA4F`), so every reclaimer can repair and nothing else can. The rows other than
2, 7, 8, 10 and 12 are as read from each case's first tests; their tails were not each traced.

### Resource placement and contextual order contracts [DISASSEMBLED + MEASURED 2026-10-04]

`tagpu_resource.c` implements manual mex/geo snapping and contextual resource building. All
queries and dispatches run on the game/input thread; the renderer gets only packet values.
The following contracts were disassembled from `pristine/TotalA.exe.pristine`. Caller counts
are an instruction-level `call rel32` scan of `.text`, not a scan of address-shaped bytes.

| Entry | Contract and callers |
|---|---|
| `0x4197D0` | Manual build cursor, `ret 0`, prologue `83 EC 14 8B 15 E8 1D 51 00`; called at `0x491CDB`, `0x499241`. Reads type `main+0x2CC4` and point `+0x2CAA`; writes six build-corner ints `+0x2C92..+0x2CA6` and the `+0x2CC6` valid/region bits. Snapping observes its entry; original code owns the rectangle and validation |
| `0x498DA0` | Mouse-to-world update, called only at `0x499221`. The existing viewport wrapper repairs its result before resource input captures the raw point. In `0x499200`, cursor update `0x499241`, hovered unit query `0x499278` and context resolution `0x499297` precede button dispatch |
| `0x498F70` | Left button handler, `ret 4`, prologue `51 A1 E8 1D 51 00`, one caller `0x4995B3`. The resource gate's saved entry stack holds the six-dword message pointer at `[esp+4]` |
| `0x499100` | Right button handler, `ret 4`, prologue `51 8B 0D E8 1D 51 00`, one caller `0x4993AC`. Contextual orders use right when Interface Type `main+0x37EFA` is nonzero; zero uses left. Explicit commands retain native button/cancel behavior |
| `0x47D2E0` | Site test, `stdcall(def, packed_cell, ignore_unit_id, player)`, `ret 16`; callers `0x40A4CD`, `0x4198C6`, `0x47DBE5`. Requires origin at least 1 and footprint end strictly below W/H. Its return's bit 0 reports acceptance |
| `0x47C780` | Build-site altitude result, `ret 0`, only caller `0x4198F5`; low byte is the site's altitude. Reads the scratch produced by the preceding site test. Resource preview/click queries save and restore `0x51E684`/`0x51E688`; the native manual cursor retains its normal scratch ownership |
| `0x47D820` | Blocked-site altitude [DISASSEMBLED 2026-10-09], `stdcall(def, x \| z<<16)`, `ret 8`; callers `0x4198FE` (the build cursor, on a refused site) and `0x47DE4A`. Answers 0 for an origin below 1 or a footprint end at or past W/H (`0x47D834..0x47D888`); otherwise walks the yardmap `def+0x14E` over the footprint `def+0x14A`/`+0x14C` (grid `main+0x14287`, 13-byte cells, width `+0x14233`) and, over the cells whose yard byte has `0x08`, takes the lowest cell byte `+6` and the highest `+5`: it returns the lowest `+6` as a byte, or sea level `main+0x1427F` less `def+0x22C` when that highest `+5` is below it, which includes a footprint with no such cell (`0x47D92A..0x47D94A`). It writes nothing outside its own frame. Quick Build gives it to the ghost of a queued plan whose ground the site test refuses |
| `0x43AFC0` | Ordinary mouse order dispatch, `stdcall(action, shift, unit, target, pos, param1, param2)`, `ret 28`. Called at `0x4197A9` (manual build) and `0x48D1F4` (contextual command). A build's param1 is the definition ID; an upgrade uses the resolved reclaim action then an appended build |
| `0x438760` | Action-name lookup, thiscall `(output_byte*, name)`; 109 direct callers, including manual dispatch `0x4197A4`. Resource dispatch asks for `MOBILEBUILD` (`0x502654`) or `VTOL_MOBILEBUILD` (`0x502640`) according to the builder's fly bit |
| `0x43F0E0` | Action resolver, `stdcall(index*, mode, unit, target, pos)`; returns the caller's output pointer; refusal writes action byte zero (`0x4401E3`), which native dispatch rejects at `0x48D0AF`. Guard is mode 7, reclaim mode 12; every mode's answer is *What each resolver mode answers*, above. It refuses a dead target (`+0x110` bit 28, `0x43F0F4`) but never tests its unit, whose `+0x92` and `+0x96` it dereferences (`0x43F112`, `0x43F131`). There are 28 direct callers; context dispatch uses `0x48D0A0`, script orders `0x4804A3`, and constructor/assist paths `0x487D5B`..`0x487F8C`. Resource input asks the resolver instead of assuming every selected builder can reclaim or Guard. **The move answers** [DISASSEMBLED 2026-11-09], the mode `(byte)mode − 1` into the table `0x4401EC`: mode 2, Move (entry 1, `0x43F845`), needs def `+0x245` bit `0x80` (else refused, `0x43F84E`); a unit whose first dword is 0 [INFERRED: it has no mover object] gets `QMOVE` (`0x50531C`, `0x43F854..0x43F870`) whatever the target and `pos`; else with no target `VTOL_MOVE` (`0x5016CC`) when def `+0x241` bit 11 is set, otherwise `MOVE_GROUND` (`0x5052E4`, through `0x4401C2`). Mode 1 at Interface Type 1 (entry 0, `0x43F9E9`; its Type 0 arm is `0x43FE35`) with no target and no reclaimable feature at `pos` ends in the move fallback `0x43FDFF..0x43FE30`: def `+0x245` bit `0x80` and a non-zero first dword (the unit, `edi` from `0x43FCD1`), then `VTOL_MOVE` / `MOVE_GROUND` by the same bit 11 — **the same answer as mode 2 for a mover**, and 0 for a unit without one (`0x43FE13` → `0x4401DC`), which `0x48CF30` then skips. The formation move asks it once per selected unit at a press, with the press's point, and starts a gesture only on these answers |
| `0x41ACE0`'s calls | **Quick Build's candidates are each selected constructor's build menu** (owner decision 23), read by `tagpu_resource.c` `build_menu` as the page loader reads it, on the GAME thread inside `quick_choice`'s callers (the gates above and the packet fill), never inside an engine file call. Per page from `tagpu_buildmenu_pages(def+0x241 bit 31, def+0x22E)`: the probe `0x4290F0(buf[1024], 0x502820, "<NAME><page>.GUI", 0x502828)` + `0x4BBC40`, else `<prefix>DL` with the watched player's side (bounded below min(`main+0x37F39`, 5), prefix `strnlen < 4`); GUI_Load's path from `main+0xECF` (`strnlen < 0x100`); the file through `0x4BB5B0`/`0x4BBD00`/`0x4BB710`/`0x4BB7C0`/`0x4BB5D0`, 1 byte to 1 MiB, the read exactly the size; the builder's download entries from records below `[main+0x391C7]`, five a record; `0x488B10`'s answer masked to its word and bounded below the def count. The rule is `tagpu_buildmenu.c`; the table holds type numbers per def table (base `main+0x1439B` and count `main+0x1438F`, which the teardown `0x42DB90` frees (`0x42DCCB`) and zeroes (`0x42DCE6`) and the menu-time loader `0x42A8D0` allocates anew, `0x4D83B0` at `0x42AA8A` stored at `0x42AA98`), level generation and side. CANBUILD `+0x152`/`+0x156` is not read |
| `0x48CD80`, `0x48D220` | Hovered-unit query and context resolution; callers respectively `0x491D18`/`0x499278` and `0x491D36`/`0x499297`. Hovered unit ID is stored at `main+0x2CBA`; feature ID is `+0x2CBC` |
| `0x481930` | Explored-map setter, six callers: `0x4818E6`, `0x48269C`, `0x48279B`, `0x482901`, `0x482A94`, `0x482C08`. Uses the player's map bit, not its seat number; allocation `0x483CF6`..`0x483D25` is W×H/4 u16 entries for even W/H |
| `0x422040` | Applies indestructible metal-spot feature values to grid byte `+7`; only caller `0x483D7D`. The cell's feature ID must be below `0xFFFB`, its definition's metal float `+0xF0` nonzero and flags `+0xFF & 2` set. Uses definition footprint `+0x94/+0x96`, bounds helper `0x481550`, and float conversion `0x4E43A0`. This is how the private synthetic fixture supplies metal values |

The full resolver caller list is `0x401F2F`, `0x402E3D`, `0x4031A7`, `0x405265`, `0x405ABC`,
`0x4064F5`, `0x40804C`, `0x408299`, `0x408403`, `0x408435`, `0x408589`, `0x40FE86`,
`0x410AD9`, `0x43B242`, `0x43B285`, `0x43B416`, `0x43B451`, `0x43B583`, `0x43FE5A`,
`0x43FE83`, `0x43FECC`, `0x4804A3`, `0x487D5B`, `0x487DD3`, `0x487E2E`, `0x487EFD`,
`0x487F8C`, `0x48D0A0`.

**Candidate-cell distance and actual placement centers.** SOURCE: TADR v2026.8.6
`tahook.cpp:SnapToNear` ranks equal-coverage candidates by squared distance from
`(cx + 0.5, cz + 0.5)` to the raw mouse in cell units; its geo caller uses the same selector.
Impure preserves this metric. It is not a guarantee of the nearest actual building center.
DISASSEMBLED: manual dispatch `0x419670` (sole direct caller `0x498F98`) reads type
`main+0x2CC4`, point `+0x2CAA` and definition footprint
`+0x14A/+0x14C`. Its `0x4196D8..0x419714` conversion reconstructs each order coordinate
as `(2 * footprint_origin + footprint_size) << 19`, passed through `0x43AFC0` at `0x4197A9`.
`tagpu_resource.c:position` uses that same center: even footprint axes place at `cx * 16`
world units, odd axes at `(cx + 0.5) * 16`. Thus an even axis's actual center differs by
eight world units from the candidate-cell center used for the tie. No metric change is implied.

#### Native MobileBuild placement retries [DISASSEMBLED 2026-10-05]

An accepted contextual upgrade queues native reclaim followed by `MOBILEBUILD`; queue
creation does not establish that the replacement will start. `MobileBuild` at `0x403A20`
dispatches state 1 to `0x403B45`. It obtains the replacement definition from `node+0x36`,
recomputes its packed footprint origin from the stored position, and calls
`0x47DB70(def, 0, packed_cell, 1)` at `0x403CC7`. The zero is the ignored unit number:
this test does not exempt the constructor. For a stationary definition (`def+0x22F == 0`),
`0x47DB70` delegates to the yard test `0x47D2E0` at `0x47DBE5`, also with ignored unit
number and player zero. The other branch tests occupied cells against the supplied
ignored number at `0x47DD00..0x47DD0A`.

On placement refusal, `node+0x3E` counts retries. The first refusal emits its message at
`0x403CDF`; `0x403CE4..0x403CF4` increments the counter, arms a 30-tick wait through
`0x439E80`, and returns 2. Subsequent refusals reach `0x403D03`: a counter greater than
10 emits the final message at `0x403D10` and returns 8 at `0x403D15`, abandoning the
command. Acceptance calls `0x47DDC0` at `0x403D2F`, creates a nanoframe through
`0x485F50` at `0x403D5B`, and links it into `node+0x16` through `0x489690` at
`0x403D64`. Failure to obtain a unit retries after 300 ticks (`0x403D7D..0x403D89`),
which is distinct from placement refusal. These contracts are disassembly, not proof
that a particular reported upgrade failed for constructor overlap.

Installed stock definitions read for the Comet Catcher stress test: `ARMMEX`/`CORMEX`
have 3x3 footprints, `ARMMOHO`/`CORMOHO` 5x5, and `ARMACK`/`CORACK` 2x2 with
build distance 60. `tools/resource_upgrade_stress.py` measures actual mouse dispatch,
reclamation, frame creation and completion; its native-order snapshots require the
pause bit at `main+0x38A51`, so the unit tick and its order destructors cannot run
between the game-thread peeks.

#### CORE reclaim-to-Moho handoff [DISASSEMBLED + MEASURED 2026-10-05]

On installed Comet Catcher, stock `CORACK` at `(2892,1772)` upgrading `CORMEX`
at `(2824,1704)` through a contextual click repeatedly reclaimed the old mex,
created `CORMOHO`, then abandoned it without reducing `unit+0x104` from 1.
The native MobileBuild node reached state 2, flags `0x14`. At `0x403DE1`,
`0x438590` starts StartBuilding; state 2 calls `0x438700(unit,node,0xA)` at
`0x403DFD`. That helper waits for `unit+0x10F` bit 0 (`INBUILDSTANCE`), marking
the node's flags with bit 2 while it is clear. State 3 builds through `0x41BA60`.

The COB trace identified the missing transition. At tick 2901 all eight native
slots were occupied: MotionControl and walklegs, SmokeUnit, RequestState,
Stop and close, StartBuilding and StopMoving. StartBuilding's START of another
RequestState was refused at `0x4B08C0` (`cmp eax,8` at `0x4B08E3`), so the
script never requested its new build state. The installed CORACK script has one
serialized RequestState coordinator, which CALLs Go/open or Stop/close; a new
RequestState updates its desired state and returns while that coordinator is busy.
The handoff therefore requires a ninth record to deliver that update. Those
chains have no recursion. This establishes the demand of the reproduced handoff,
not a bound on every possible mod script or arbitrary additional callbacks.

The nanoframe's GetBuilt handler (`0x402DA0`) remains incomplete when stance
never arrives. Its wait arms 300 ticks through `0x439E80` from state 0 at
`0x402F8C`; state 2's elapsed-event branch at `0x402F5B` calls the native
unbuild/decay path `0x41BCD0(unit,11)` at `0x402F6B`. A ghost disappearing here
does not establish a placement failure. In one paused reproduction, the bot's
2×2 footprint at cells `(178,109)` and the accepted Moho's 5×5 footprint at
`(174,104)` shared no cell. Explicit native build from idle at the same site and
bot position entered state 3, stance 1 and reached remaining fraction `0.961694`.

On the sixteen-slot diagnostic prototype, the trace accepts
the formerly refused RequestState in slot 8. The focused native assertion reached
stance 1 and remaining `0.947848` after 679 ticks. An all-deposit CORE sweep
completed all 61 accepted upgrades by 7541 elapsed ticks; 20 initial site refusals
are separate from accepted upgrades. The constructors, mexes and Mohos retained
stock definitions; only supply was synthetic. The original mixed sweep completed
all 37 accepted ARM upgrades and abandoned all 24 accepted CORE upgrades. These
measurements identify a real CORE COB failure; they do not identify the faction
or exact cause of an unrecorded earlier play report.

The chosen ten-record runtime repeated the same CORE phase-0 sweep: **61 of 61
accepted upgrades completed**, by 7611 elapsed ticks, 20 initial refusals and no
unfinished build. Its trace accepts RequestState in slot 8 (including units 169
and 265 at tick 2651). Native ten-record save/load preserved all ten active records
on human and local-AI probes; native eight-record saves imported without loss.
Full-pool START and CALL probes passed 16 and 3 assertions respectively.
The automated native save/load preflight separately preserves all ten active
records on both owners; its coverage bitmap reaches the replacement `cob_save`
and `cob_load` functions at the native `0x4B1EC0`/`0x4B2040` methods.
The ARM phase-1 sweep on ten records completed all 67 accepted upgrades by 8303
elapsed ticks, with 14 initial refusals. Keeping generated starts 128 world units
inside the border lets the previously motionless lower-border bot reclaim and
complete; no native pathfinder change is made.

The long replay fixture checks completed stock Moho at tick 7800 and requires its
last comparison tick at or after that checkpoint. The diagnostic run's unit/player,
effect-count and three restore comparisons passed, while paused viewer input first
computed CORMOHO's ghost-mask row (`s_ghost` type 219): piece-count/valid/any and its
hide bits changed at frozen tick 9219. `tagpu_datakeys_ghost_mask` now keeps an
uncached paused preview in local storage; `main+0x38A51` bit 0 gates cache writes,
so a first hover cannot write SIM_STATE while the simulation tick is frozen.
Puppet cache misses use local storage regardless of native pause, leaving the
recording the only writer even while a replay is paused. No declaration or recorded-state comparison is removed by this correction.

#### Input bounds and layout

The root is `0x511DE8`. The controlled player byte `main+0x2A42` is bounded below 10, and resource
input refuses puppet seats. Global unit begin/end `+0x14357/+0x1435B` are inclusive, stride
`0x118`; their aligned distance plus one must equal the engine count `u16 +0x14351`. The
controlled player's record at `+0x1B63 + seat*0x14B` has first/last pointers `+0x67/+0x6B`, also
inclusive and aligned within the global pool. Own-unit and order scans use that smaller partition.

Unit flags `+0x110`: alive `0x10000000`, selected `0x10`, excluded `0x4000`, structural footprint
`0x20000000`, contested footprint bits `0x0C000000`. Owner is byte `+0xFF`; definition pointer
`+0x92` must be aligned in the `0x249`-stride definition array at `main+0x1439B`, bounded by
`+0x1438F`. Unit number is `u16 +0xA8`, world position is three 16.16 ints `+0x6A/+0x6E/+0x72`;
`+0x104 == 0` identifies completed construction in the resource path. Grid origin is
`u16 +0x76/+0x78`, footprint size `+0x7E/+0x80`.

Definitions: footprint shorts `+0x14A/+0x14C`, yard pointer `+0x14E` (allocated as FootX×FootZ by
`0x42CF30`). Quick Build's candidates come from the builder's build menu (the `0x41ACE0` row
above), not from CANBUILD `+0x152`/`+0x156`, so the resource hooks do not depend on the
whole-build-list patch. The menu reads the page count byte `+0x22E`, the orders-page bit
`+0x241` bit 31, the type word `+0x21E` and the name `+0x20`. Building movement code is byte `+0x22F == 0`, builder
bit `+0x241 & 0x40`, fly bit `+0x241 & 0x800`; worker time is u16 `+0x1FE`, repair capability
`+0x245 & 0x200` (no key of its own: the FBI loader copies `canreclamate`, bit `0x400`, into it,
`0x42CA3B..0x42CA4F`). Metal extraction and energy output floats `+0x1CE/+0x1C2` must be finite and
positive. Geo recognition scans the complete bounded yard for byte `0x8F`, at most 64×64 cells.

The feature grid `main+0x14287` has dimensions `+0x14233/+0x14237`, stride 13. Dimension products
are checked before indexing. Metal is byte `cell+7`. Background `SurfaceMetal` is loaded through
`[main+0x391E9]+0xD30` (`0x4366B3`..`0x4366DB`) and seeds ordinary grid metal
(`0x4839B3`..`0x4839ED`). Explored entries at `main+0x14273` use the player record's byte `+0x146`
as a bit index, bounded below 16. Every metal read first checks that cell's explored 2×2 block;
unknown neighbors prevent a Quick deposit from being proved closed. The engine's full-map
reveal fills this explored map; resource scoring has no alternate unseen-data path.

Order nodes belong to the live unit's game-thread-owned heap chain at `unit+0x5C`; no node is
retained across iterations. Walks stop at 16384 nodes and check owner pointer `node+0x0E`.
Action `byte +4` indexes the level's descriptor vector `0x512344..0x512348` at stride `0x19`;
capability `+0x0C & 1` distinguishes a placed build from factory counts. Target is pointer
`+0x16`, position `+0x22/+0x26/+0x2A`, build type u16 `+0x36`, flags `+0x42`, next pointer `+0x4A`.
Plain order clearing (`0x43ADC0`) preserves flags bit 2 (`4`); queued collision tests reproduce
that distinction for participating builders/helpers rather than discarding every old plan.

#### Contextual cancellation through the native order roots [DISASSEMBLED 2026-10-05]

`0x43AFC0` tests Shift at `0x43AFD4` and scans only the main chain `unit+0x5C` at
`0x43AFE0`. The first action match (`0x43AFF2`) whose optional target matches (`0x43AFF7`..`0x43AFFE`)
and whose X/Z positions are within inclusive ±16 world units (`0x43B004`..`0x43B029`) wins.
The position arithmetic is unsigned wrapping 32-bit subtraction plus `0x100000`, compared against
`0x200000`. **It never compares the build type at `node+0x36`.** A NULL target argument skips
the target comparison. With no match, `0x43B08A`..`0x43B09D` appends through `0x43ADC0`.

`0x43B034` chooses the unlink head: `+0x60` for node flags `+0x42 & 0x40000`, otherwise
`+0x5C`. `0x43B05B`..`0x43B05E` unlink the matched node. The original head is saved at
`[esp+0x24]`; a non-head removal sets `+0x42 & 0x10000` at `0x43B068`. The calls at
`0x43B075` and `0x43B07B` destruct (`0x43A1F0`) and free (`0x4B4F20`) it, then return
without creating an order. The root is used by manual placement `0x4197A9`, contextual orders
`0x48D1F4` and the resource input path. A match flagged `0x40000` removes nothing and adds
nothing: the node was found on the main chain, the unlink searches the `+0x60` chain for it, and
the call returns at `0x43B054..0x43B058` (`ret 0x1C`; or `0x43B044` → `0x43B0A2` when that chain is empty)
[INFERRED: a node is on one chain only]. **The consequence for repeated Shift-clicks**: the match
is the first node of the action within ±16 inclusive, whatever its build type, so two 1×1 sites
16 world units apart toggle each other, and a run of stock Shift-clicks along a line of 1×1
buildings alternates add and remove [DISASSEMBLED 2026-10-25, `0x43AFC0..0x43B0A6`].

The destructor calls the action handler with flags 2 at `0x43A21A` when node byte `+6 & 2`
is set. Flags `+0x42 & 0x400000` run the native `StopBuilding` script (`0x505104`) lookup/start at
`0x43A227`..`0x43A258` (`0x4B07C0`, `0x4B0B00`, `0x456190`) and clear the bit (`0x43A25D`). Its
`+0x52` object cleanup at `0x43A264`..`0x43A2A4` can invoke virtual destructors. A head removal
also calls `0x489800(unit,3)` at `0x43A2B9`, which resets the unit's slots 0..2 and queues a COB
start of `TargetCleared` (`0x508D58`, `push` at `0x489893`, through `0x4B0A70`) for each slot it
resets; the `0x10000` flag skips that call for non-heads [DISASSEMBLED 2026-10-25]. **Flag
`0x40000` is never on a `+0x5C` node:** the constructor is its one writer (from the descriptor's
`+0x11`, `0x43A18F..0x43A196`; every immediate OR into `+0x42` in `.text` is `0x10000`,
`0x400000`, `0x2000` or `0x8000`), and both linkers put a node carrying it on `+0x60` —
`0x43A020` (`0x43A07D..0x43A09D`) and `0x43ADC0` (`0x43AEF0..0x43AF2B`) [DISASSEMBLED
2026-10-25]. So `0x43AFC0`'s `0x40000` case, a match on the `+0x5C` scan, is never reached.
`0x489650` at `0x43A2C1` releases the target reference at `node+0x12`. Thus cancellation
can re-enter engine handlers, and retaining an order-node pointer across the root is unsafe.

**Exact removal `0x439F80(unit,node)`** is stdcall, `ret 8`. Its two direct callers are
`0x43BA7C` (order controller) and `0x49646F` (Ctrl+D cancellation). `0x439F89` reads the main
head, `0x439F8F` selects the sub-list for flags `0x40000`, and `0x439FA1`..`0x439FAD`
search for the exact pointer. A missing node returns without appending or freeing. A found node
is unlinked at `0x439FB3`..`0x439FB8`, sets `0x10000` when not the main head at `0x439FBC`,
and is destructed/freed by `0x439FC9`/`0x439FCF`. Resource cancellation finds the actual
selected constructor's main-list node in the bounded owned chain and uses this root, so build
variant changes and neighboring positions cannot redirect deletion.

**The paired-reclaim lifetime:** the immediately preceding node must be the resolved reclaim
on the old target at exactly the build position. The build must be unstarted (`byte +6 & 2 == 0`),
without `+0x42 & 0x400000` or an owned object (`+0x52 == NULL`). Being preceded proves it is
non-head; exact removal sets `0x10000`. These conditions disable every callback branch of
`0x43A1F0`. Its remaining `0x489650` call has no calls or virtual dispatch: `0x48965D`..`0x489683`
only unlink this node's target reference from the intrusive unit-reference chain. Therefore the
preceding reclaim stays allocated across build removal and can be passed to the second exact
removal root. This is the only order pointer retained across a root; it has an explicit lifetime
proof. After reclaim removal no node pointer is used. Helpers' Guards remain intact.

**New orders `0x43ADC0(action,shift,unit,target,pos,p1,p2)`** use the same seven-argument
stdcall layout and `ret 28` as `0x43AFC0`. **It validates nothing** [DISASSEMBLED 2026-11-11]: not
the unit, not the target, and the action only as a byte (the constructor reads it with
`mov al` at `0x43A0C3` and masks it `0xFF` before indexing `0x512344`, `0x43A17C..0x43A182`), so
junk above the low byte is harmless and an action byte of 0 issues `Ready`. When the allocation
fails it stores NULL and, without Shift, reads the NULL node's `+0x42` at `0x43AE0E` (the engine's
own crash). It constructs a `0x56`-byte node through
`0x4B4F10`/`0x43A0C0` at `0x43ADC4`..`0x43ADF4`, preserves existing non-temporary work with
Shift, and replaces removable work without Shift (`0x43AE02`..`0x43AE59`). The native temporary
head cleanup and linking retain their normal descriptor flags (`0x43AE5B`..`0x43AFB1`). In
detail [DISASSEMBLED 2026-10-25]: without Shift, and when the new node lacks `+0x42` bit `0x40`,
every main-chain node lacking bit 4 is destroyed; then, on every call unless the new node has
`0x40000`, the chain's head is destroyed while it carries `0x4000` (`0x43AE72..0x43AECE`). The new
node gets `+0x42 |= 1`, and `|= 0x2000` without Shift (`0x43AEE7`). A node with `0x40020` is linked
at the head of its chain (`+0x60` for `0x40000`, else `+0x5C`) and takes the old head's `0x4000`.
Any other node gets `0x1000` and is linked **after the node carrying `0x1000`**, whose bit is
cleared (`0x43AF84`), or at the tail when none does, or as the head of an empty chain
(`0x43AFA4`). So consecutive appends land in call order, each after the one before. It does
not run the mouse root's proximity toggle. New Quick reclaim/build orders use this root, ensuring
that Shift can append a distinct adjacent site. Its 27 direct callers are `0x4029A5`, `0x402E5E`,
`0x402EFA`, `0x403DC5`, `0x408064`, `0x4082B0`, `0x408421`, `0x408453`, `0x4085A1`, `0x4140F3`,
`0x43B09D`, `0x43B120`, `0x4804BB`, `0x487D70`, `0x487DE8`, `0x487E3F`, `0x487E8A`, `0x487F25`,
`0x487FA5`, `0x487FF6`, `0x4880AB`, `0x4880D2`, `0x488110`, `0x488175`, `0x4881D1`, `0x4881F8`,
`0x48821E` (instruction-level call scan). It calls only the allocator `0x4B4F10`, the constructor `0x43A0C0`, the destructor
`0x43A1F0` and the free `0x4B4F20` (`0x43ADC6`, `0x43ADEF`, `0x43AE47`/`0x43AEB8`,
`0x43AE4D`/`0x43AEBE`) [DISASSEMBLED 2026-11-09, direct calls only: the destructor `0x43A1F0`'s
calls through the order's table (`0x43A21A`, `0x43A285`, `0x43A293`) and the constructor's
`0x4895C0`/`0x489690` are not traced]. None of those direct calls sends anything; an order the DLL
makes through it with a click's arguments is a stock click's order by parity — `0x43AFC0` forwards
a no-Shift click to it unchanged — whatever the untraced calls do. The formation move (`tagpu_formation.c`) gives each unit its slot
through it with the release's Shift: an append, or without Shift the replace above, as `0x43AFC0`
forwards a no-Shift click; it does not go through `0x43AFC0`, whose Shift scan would let one
unit's slot cancel its own queued move within 16 units.

**Geo identity:** `0x47D684` tests candidate yard bit `0x80`. `0x47D69D` reads the feature ID
at grid-cell `+8`; an ID below `0xFFFB` is checked against the feature count. `0xFFFE` resolves
through cell `+10` (dz) and `+11` (dx), subtracting `(dz*W+dx)*13` at `0x47D6C7`..`0x47D6E4`.
`0x47D708` tests feature-definition byte `+0xFE & 0x20`; no spacing rule separates geo sites.
Resource identity bounds the alias offsets before subtracting, checks explored anchor cells and
feature IDs against the allocation, and requires one unique canonical geothermal anchor among
all G-yard cells. Candidate and queued footprints must name the same anchor; touching multiple
anchors is ambiguous and does not permit cancellation. The generated adjacent-one-cell fixture
exercises the distinction that a +/-16-world-unit comparison cannot establish.

#### The ground Guard's node, and what the order dispatch does with a handler's result [DISASSEMBLED 2026-10-09]

`tools/resource_regression.py` compares a constructor's queue before a plan and after its
cancellation, with a Guard as the preceding order because a Guard does not finish on its own.
What that rests on:

- **`0x406300` Follow_Ground**, the handler of order type `0x12` ("Guarding"; record `0x4FC733`;
  `tacli order guard` on a ground unit), `stdcall(unit, order, events)`, `ret 0xC`. Its exits
  before the state switch: target `+0x16` NULL → 5 (`0x406312`); the guarder's `+0x86` nonzero →
  7 (`0x40632F`); the target definition's `+0x241` bit 11 → 8 (`0x406352`). The state byte `+0x5`
  selects state 0 (`0x406705`) or 1 (`0x406381`); any other state returns 7.
- **State 0** (`0x406705..0x40677D`) writes `+0x36` = (guarder `+0x7E` + target `+0x7E` + 2) × 16
  [INFERRED: the two units' radii] and, from `0x4B6C30(0x10000)` [INFERRED: a random angle] through
  `0x4B70EF`/`0x4B7123` [INFERRED: sine and cosine], `+0x22/+0x26/+0x2A` as an offset of that
  length around the target, then returns 1. So the node's position stops being the issued one at
  its first step. MEASURED 2026-10-09 on Comet Catcher: ARMCK's Guard of ARMMSTOR, read paused
  before that step, held (1300, 55, 1550) and `+0x36` 0; after it, (−115.3, 0, −54.7) and 128.
- **State 1** (`0x406381..0x406702`) returns 3, with an order put in front of itself, in three
  branches: `0x43B1F0` succeeding at `0x4063F5`, reached when the target's `+0xF0` is set among
  other tests [INFERRED: it is attacked]; the resolver's mode 8 (repair, `0x4064F5`) when the
  target's `+0x108` is below its definition's `+0x1FA` [INFERRED: health against maximum], the
  node made at `0x40652B` and put first by `0x43ACB0` at `0x406532`; and a target whose head
  order's flags `+0x42` carry `0x100000` (`0x40658A`) [INFERRED: an assist of its build],
  through `0x43ACB0` at `0x40667A`; that branch is skipped when the target's head order targets
  the guarder itself (`0x406597`), which keeps two units guarding each other out of it. Otherwise it moves to the target position plus the offset (`0x438930` at
  `0x4066E0`), waits 30 ticks (`0x439E80`), and returns 2. **No state-1 path writes
  `+0x22..+0x2D` or `+0x36`.**
- **The dispatch `0x43B87C`** maps a handler's result through the jump table `0x43BAA4`
  (10 entries; above 9 → `0x43BA90`): 0 → state 0 (`0x43B89F`); 1 → state + 1 (`0x43B897`);
  2 and 4 → nothing (`0x43B99F`); 3 → a random wait (`0x43B890` → `0x43B951`); 5 and 8 → unlink and
  free (`0x43B8A8`); 6 → `0x43B975`; 7 → `0x43BA3D`; 9 → `0x43B8E5`, which also returns a node with
  no successor (`+0x4A` null, `0x43B8F8` → `0x43B94B`) to state 0. Follow_Ground returns only 1, 2,
  3, 5, 7 and 8, never 0 or 9, so once its first step has run, nothing returns its node to state 0.

A Guard of an owned unit that is neither damaged, building nor attacked therefore keeps one node
whose compared fields (action, target, position, `+0x36`) are fixed from its first step on.

#### An upgrade preview without grid mutation

`0x47CC30` stamps a footprint; callers `0x43DA41`, `0x48610F`, `0x48630C`, `0x48AA9A`, `0x48B6B9`.
An old yard byte's bit 0 stamps cell flag bit 1 (`2`) at `0x47CE32`; yard occupancy bit 2 (`4`,
or bit 1 when unit byte `+0x10F & 4`) stamps its unit number at `0x47CE23`.
The mask selection at `0x47CD88..0x47CD9D` reads that byte, ANDs with `4`, and chooses yard
mask `2` when set, otherwise `4`.
`0x47D0E0` clears the footprint; callers `0x43DA0F`, `0x47CBD6`, `0x48AA6B`, `0x48B685`.
It clears a matching unit number at `0x47D172`, and clears cell flag `2` for every old yard bit 0
at `0x47D17C`. The site's only read of the candidate yard's bit 0 is `0x47D530`; its occupancy
check at `0x47D554` ignores the supplied unit ID exactly.

Therefore, for a validated uncontested old footprint, clearing candidate yard bit 0 where it
would overlap an old yard bit 0 gives the site's post-clear yard-bit test without touching the
grid. A stack copy of the definition points at this bounded stack yard. Both old and new
footprints and the old live footprint dimensions are validated first. Other yard bits and the
old-ID occupancy exemption retain the engine's tests. Contested footprints are refused because
`0x47D2B9`..`0x47D2C4` calls `0x47E5C0` to restamp neighbors: the simple projection is insufficient
there. No timing or pointer probe supplies this proof.

**MEASURED on retail Comet Catcher:** the patch spans metal cells 88–90 by 81–83, metal byte 223
against SurfaceMetal 3. Manual ARMMEX snaps to footprint origin (1408,1296), and a stationary Alt
returns raw origin x = 1440. The resulting order center is (1432,1320). Advanced ARMACK
queues reclaim of a completed own ARMMEX followed by ARMMOHO, while ARMCK/CORCK receive persistent
Guards and ARMPW's orders stay unchanged. The generated RS maps exercise background 5 with metal
5, background 5 with metal 20 everywhere, and background 20 with an isolated metal-223 patch.
These measurements establish the input cases; their final replay/compatibility gates are tracked
in the landing's notes, not assumed from these values.

**MEASURED on retail Comet Catcher, 2026-10-05:** at eye (1000,950), pointer (588,377),
manual ARMMEX's rectangle at `main+0x2C92` and Quick Build's native order at `unit+0x5C`
both center at (1432,1320), with a 3×3 footprint. At pointer (620,375), the raw x is 1492;
manual placement retains origin x=1472 and queues center x=1496. Before Quick Build shared
the manual complete-target check, it instead queued x=1448: the radius-3 search captured only
part of the deposit. A footprint-radius search from that candidate finds the full-coverage
origin (88,81), so the candidate is not stable. The centered site is accepted by `0x47D2E0`
at the nearer pointer, and both modes use definition 78; the difference is the selection check,
not a footprint conversion or engine refusal. The shared check retains the original radius
and refuses that partial Quick target, leaving the native contextual action available.

**MEASURED native interface selection:** Tab → `PREFS` → `SPEEDS`, set `LEFTCLICK` stage 0,
then `PREV` → `OK` sets `main+0x37EFA` to 0 after launch. `tacli` forces this field's registry
setting to 1 at every launch, so a prelaunch fixture edit cannot establish left-button coverage.
The classic resource regression uses that native menu path and checks the live field before its
clicks; the other mex fixtures require value 1.

#### GUI-only writes found by the resource replay fixtures

The graphics globals base is `0x51F320`. Its event-ring write/read indices `+0x18E/+0x192` are
`0x51F4AE`/`0x51F4B2`, capacity `+0x186 = 20`; `0x4C2E30` and `0x4C2D60` advance them through
mouse-button messages even when a modal GUI consumes the click. The GUI is `main+0x519`:
its latest event occupies `main+0x555..+0x56C` (`GUI+0x3C..+0x53`): six dwords x, y,
wParam, timestamp, message and double-click flag. Thus `main+0x559` is y, `main+0x561..+0x564`
its timestamp, `main+0x565` its message and `main+0x569` its double-click flag, and `main+0x56D`
is `GUI+0x54`, the wParam of the last event the pump popped, while `main+0x533` is inside
its active-screen pointer `+0x18`, and `main+0x585` is the previous hovered/help gadget index at `GUI+0x6C`: `0x4AA596`
and `0x4A00B5` copy `GUI+0x68` into it before selecting that gadget's text at record `+0x33`. The existing event-ring and GUI pump sections establish those owners. A two-image null
twin whose arrows cancel and whose keyboard menu opens without clicks cannot observe every
changed ring/eye bit. A native injected click returns the pointer to screen centre, so the
harness follows each sampled click with a separate move and requires the GUI x/y/message echo
before dumping. It samples GUI-consumed clicks, every valid coordinate bit and several eyes at
one paused GameTime, and requires its carried simulation records to remain equal. A click is two
ring entries, its press and its release, so the twin's whole clicks move the read index by even
steps of the ring of 20 and never change its low bit: the images gate cannot count that bit as the
viewer's. MEASURED 2026-10-09, resource-geo-upgrade at speed 20: a dump at T1 451 taken as a click
of the script reached the engine held a read index of 9 live against 6 played and sought, and
failed the gate on that bit.
The recorder's 60 recorded global slots / 78 dwords are unchanged. Owner-approved `leave`
rows keep the active GUI pointer (`main+0x531`, four bytes), GUI mouse-event clock
(`main+0x561`, four bytes) and bitmap-font pointer (`0x51F524`, four bytes) local.
The three additional UI fields below and the two keyboard indices above also have exact
owner-approved rows, and so, since 2026-10-09, do the mouse-button ring's indices and the GUI's
copy of the popped event (*The mouse-button ring is viewer input*, below). Coordinates and text colours retain their existing classification;
orders and carried simulation records retain their existing comparisons.

The GUI event timestamp is a wall-clock sample. `0x4B5E70`, `0x4B5ED1` and `0x4B5F1D`
call GetTickCount through IAT `0x4FC0DC`, multiply the low 32 bits by graphics `+0xE8`, and
divide by 1000, as `0x4B6340` does. Moves copy the event through `0x4C2360`; buttons preserve
it in the ring. `0x4AB66C` copies the six dwords into `GUI+0x3C`; with an empty ring,
`0x4AB67B` calls `0x4C2340`, whose copy is at `0x4C2356`. The pump copies the event into
its local dispatch data at `0x4AA151`. The original timestamp at `0x51F4C2` and its input
copy at `main+0x2C82` already remain local under the existing mouse/input rows.
Ring flush `0x4C2BB0` resets only the indices, and GUI init `0x4AA850` does not clear
`GUI+0x48`. The window procedure stores the stamp into the event it builds at `0x4B5E9C` (a
move), `0x4B5EF0` (a button) and `0x4B5F3C` (a double click). The record's three readers each
copy all six dwords to their stack (`0x4A44B7..0x4A44D1`, `0x4A6B57..0x4A6B63`,
`0x4A737D..0x4A7389`) and go on with the copy's x and y (`[copy+0]`, `[copy+4]`, read at once);
`0x49FD20(gui, gadget, out)`, `ret 0xC`, which copies the six dwords out whole and makes x and y
gadget-relative, has no direct caller and no stored address in the image. The `leave` row
`guistamp` (`main+0x561`) rests on this. Ordinary native moves and clicks produce another clock sample. A short paused
probe cannot establish a change of every clock bit by waiting for arithmetic rollover.

**The mouse-button ring is viewer input [DISASSEMBLED 2026-10-09].** The ring lives in the
graphics object `0x51F320` (`0x4B6220` returns it): capacity `+0x186`, entries at `[+0x18A]`
(24 bytes each, the six-dword mouse record), write index `+0x18E` (`0x51F4AE`) and read index
`+0x192` (`0x51F4B2`). Its writers: the init `0x4C2BD0`, whose one call `0x4B5A67` passes a
capacity of 20, allocates the entries (`MEM_AllocNamed 0x4D83B0`, `0x4C2BEF`) and zeroes both
indices (`0x4C2C0B`, `0x4C2C11`); the flush `0x4C2BB0` (`0x4C2BB7`, `0x4C2BBD`; one call,
`0x426636`); the push `0x4C2E30`, which advances and wraps the write index (`0x4C2E78`,
`0x4C2E82`) and whose one caller `0x4B5F51` is the window procedure's `0x201..0x206` arms (the
switch at `0x4B5E3B`); and the pop `0x4C2D60`, which advances and wraps the read index (`0x4C2DB5`,
`0x4C2DBF`). The peek `0x4C2DE0` reads both and writes neither. The pop is called only from the
input pass `0x499890` (`0x4999C4`) and the GUI pump's refresh `0x4AB5D0` (`0x4AB654`), the peek from
the same two (`0x499982`, `0x49999C`, `0x4AB60F`). No ring function's address, and neither index's,
is stored in the image (a search of the file for each as a little-endian dword). The 25 other
instructions with a `+0x18E` or `+0x192` displacement (`0x413060..0x48A68F`) form their base with no
fetch of the graphics object in the 25 instructions before them [INFERRED: other objects' fields].
A recursive descent over direct calls and jumps from the step `0x495490` (946 functions reached,
173 of them with an indirect call or jump it does not follow) reaches the pop and the peek only
through `0x4AB5D0` ← `0x4A9FD0` ← `0x4ABD90` ← the players phase `0x464F80`: the watch-mode notice
("You are placed in watch mode because you…", `0x507338`), a dialog box opened at `0x465816` whose
pump runs at `0x4AC035`, which computes no simulation state. It does not reach the push, the init
or the flush.

The GUI's copy of the event: `GUI+0x4C` (the message, `main+0x565`) and `GUI+0x50` (the
double-click flag, `main+0x569`) are written only by the six-dword copies `0x4AB66C` (from the
popped entry) and `0x4C2356` (`0x4C2340`, from the current record when the ring is empty, called at
`0x4AB67B`). `GUI+0x54` (`main+0x56D`) takes the popped entry's wParam at `0x4AB664` and is zeroed
at `0x4AB502` by `0x4AB4E0`, which the process init `0x491200` calls once with `main+0x519`
(`0x491537`). Their readers are the GUI's: the button tests `0x4AB510` (a press or a double click)
and `0x4AB570` (a double click) read `GUI+0x4C`, and `0x4AB5B0` reads `GUI+0x54`, all from the gadget
handlers (call sites above and below); a displacement search over `0x4A0000..0x4AFFFF` finds no
reader of `GUI+0x50`. All five are `leave` rows of `replay-globals.tsv`, the owner's (2026-10-09):
`mousehead`, `mousetail`, `guimsg`, `guidblclick` and `guipopwparam`.

`main+0x531..+0x534` is the full active GUI pointer (`GUI+0x18`), not a unit type or a
simulation pointer. Init zeros it at `0x4AA85E`; `GUI_Load` links its allocated screen to
the old top at screen `+0`, then installs it at `0x4AAC5B`. Both `GUI_Pop` (`0x4A96A2`)
and the pump's own pop (`0x4AA7D5`) replace it with that previous link before freeing
the removed screen. MEASURED resource-refusals: live T301 held `0x0F72F320`, T1891 and
the paused base held `0x0DC035B0`, menu views held `0x0B0B8018`, and the native RSNRACK
selection returned to `0x0DC035B0`. Playback held `0x0B0A0BD0`; fresh seek held
`0x0B0A7AF8`. Different screen allocations do not guarantee that every differing address bit
changes during the null twin.

`0x51F524..0x51F527` is the full current bitmap-font pointer, graphics `+0x204` (the text
globals above). `SetFont 0x4C1420` ignores NULL at `0x4C1425..0x4C1427`, then stores the
pointer at `0x4C142E`. `GetFont 0x4C1440` reads it at `0x4C1445`; `0x4C1450` reads its
first byte as font height. Text drawing `0x4C14F0` passes this font to the glyph rasterizer
`0x4CCF60` at `0x4C16D4`/`0x4C1744`. The adjacent `+0x208/+0x20C/+0x210` fields are text
colour latches, not part of this pointer. MEASURED resource-classic: live T301 held
`0x0DC48C20`, T1441 and all thirteen paused views held `0x0DA31580`; playback, second playing
and seek held `0x0B0EBF90`, `0x0B0EB178` and `0x0B0EB128`. The two reported bytes at
`0x51F525` were the middle of that four-byte pointer. A NULL SetFont call cannot clear it.
Font loader `0x42A320` reads the two native faces through `0x4BBE50` at
`0x42A347`/`0x42A387` and stores the returned allocations at `main+0x391F9/+0x391FD`
(`0x42A375`/`0x42A3A2`). The file loader allocates bytes through `MEM_AllocNamed 0x4D83B0`
at `0x4BBF3C` and reads them at `0x4BBF49`; teardown frees them through `MEM_Free 0x4D85A0`
at `0x42A3BC`/`0x42A3E0`. The font latch holds a process allocation address, not a font ID.
All three focused fixtures' carried simulation, events, round trips and seeks compare equal,
including 39 additional paused views and three final endpoints. The former strict image
failures identify the exact fields above; their owner-approved rows now classify that metadata
explicitly. The full landing tier remains the runtime gate for the resulting list.

`main+0x37E9E` is the command panel's displayed unit type: word stores at `0x41B0D2` and
`0x41B1BC` copy unit `+0xA6`; the no-unit branch zeros it at `0x41B1E1`. The adjacent word
`main+0x37E9C` receives unit `+0xA8` at `0x41B0BE`/`0x41B1A8`. MEASURED: native selection of
ARMACK changes the type to 4 at paused GameTime 1722, which remains 1722 after selection.
The resource-refusals null twin selects ARMACK through native input and requires its displayed
type before a thirteenth dump; that dump's carried simulation must still equal the paused base.

**Additional strict-image failures [DISASSEMBLED + MEASURED 2026-10-05].** The
disk-interrupted combined run also completed three failing image comparisons:
`resource-nozoom` at T1291 names `main+0x37E9E` (live `0xA5`, playback/seek zero),
`resource-plain` at T481 names `main+0x11E7` (live 1, playback/seek zero), and
`resource-upgrade` at T481 names `main+0x55D` (live 4, playback/seek zero).
Each reports one differing byte. The owner subsequently approved exactly the
full four-byte wParam, four-byte gadget latch and two-byte displayed type as
viewer-local fields. Their leave rows add ten bytes without changing any recorded
global slot or comparison tolerance. A new full run remains the validation gate.

`main+0x55D` is the full four-byte native event wParam, `GUI+0x44`, within the
six-dword copies at `0x4AB66C`/`0x4C2356` described above. Bit 2 is `MK_SHIFT`;
the right mouse button is bit 1. An isolated native paused probe at GameTime 172
changes the flags from 0 to 4 using held Shift and a mouse move. The same frozen
probe selects ARMCK, ARMACK and CORCK and observes displayed types 32, 4 and 165
respectively. Thus the old no-zoom value `0xA5` is CORCK's displayed type, not
ARMCK's. The current refusal-only selection samples do not exercise that type
in every resource fixture.

`main+0x11E7` is `GUI+0xCCE`, a four-byte saved signed gadget status. The native
handler `0x4A6AE0`, called at `0x4AA24C`, resolves the active screen at `GUI+0x18`
and its gadget array at `screen+4`, with stride `0x15B`. At `0x4A6BC0` and
`0x4A6BF1` it sign-extends gadget word `+0x138`; the stores at `0x4A6BC7` and
`0x4A6BF8` save that value in the GUI latch. The branches follow left/right
press tests through `0x4AB510` at `0x4A6BA1`/`0x4A6BD2`. That helper tests
`GUI+0x4C` for native messages `0x201`/`0x203` or `0x204`/`0x206` and returns
with `ret 8`. Its other direct callers are `0x4A3A13`, `0x4A3A58`, `0x4A430F`,
`0x4A43B8`, `0x4A44EC`, `0x4A451B`, `0x4A4BCF`, `0x4A4C02`, `0x4A6DD2`,
`0x4A73D7` and `0x4A744F`.

The latch's word reads at `0x4A6C56`/`0x4A6CE3` restore gadget `+0x138` when
the pointer is outside its rectangle. Its dword read at `0x4A6CB2` supplies the
zero/nonzero toggle stored at `0x4A6CBF`. The latter branch tests the held-button
mask through `0x4AB5B0`: that helper reads `GUI+0x54`, ANDs its requested mask
and returns a boolean with `ret 8`; it does not decode a mouse-up message.
Direct callers are `0x4A3AB6`, `0x4A4204`, `0x4A4553`, `0x4A4C3E`, `0x4A6C17`,
`0x4A6C82`, `0x4A6D66` and `0x4A6E62`. The handler calls `0x4A0340` and
`0x4A5F40` at `0x4A6CC8`/`0x4A6CCF` after updating gadget state, and returns
with `ret 12`. A displacement search finds those two stores and three reads of
`+0xCCE`; it does not prove the absence of an aliased or block-copy access.

Related reset `0x4A69D0` clears nonzero type-1 gadget status words over the
native signed count at panel `+0xB6`, starting at gadget 1; it requests a redraw
through `GUI+0xCCA` at `0x4A6A14`. No direct CALL to this entry was found.
`0x4A6A40` applies the same reset only to the supplied gadget's byte-1 group,
and sets the redraw flag at `0x4A6AAF`. Its direct callers are `0x4958AD`,
`0x49591F`, `0x495F82`, `0x49900C`, `0x4990F1`, `0x499159` and `0x49933D`.
Neither reset writes `GUI+0xCCE`.

Three isolated diagnostic attempts did not reproduce the latch changing to 1
with the native Interface `LEFTCLICK` control. Their stage assertions failed
before the paired dump comparisons ran. The frozen-tick flags/type observations
above are established; a paired carried-state proof for this particular gadget
status is not. The native writers establish its GUI owner, not a successful
diagnostic regression. Owner approval of those exact extents is separate from
the native ownership evidence and from the unsuccessful paired-state probe.

`main+0x142CB..+0x142DA` holds four signed dwords for the minimap camera rectangle.
`0x41C3C0` clamps eye `+0x1431F/+0x14323` against native scroll extents `+0x1422B/+0x1422F`
minus viewport dimensions `+0x37E37/+0x37E3B`, then passes `main+0x142CB` to
`0x466B70` at `0x41C426`/`0x41C442`. The indirect stores are `0x466B99` (left),
`0x466BBF` (top), `0x466BE6` (right) and `0x466C0F` (bottom). Left/top use signed
minimap origin `+0x142E7/+0x142E9` plus minimap dimensions `+0x142EB/+0x142ED`
times the eye divided by those extents. Right/bottom add those minimap dimensions
times view cell dimensions `+0x1423B/+0x1423F` times 16 divided by those extents,
then subtract one. The rectangle is passed to `0x4BF8C0` at `0x466B5E` after
the minimap image copy through `0x4C6B70` at `0x466B44`. DISASSEMBLED.

MEASURED combined ten-slot resource-refusals: T691 rectangle `(53,22,66,32)` and
displayed type 4; T1711 and paused base T1759 rectangle `(32,20,45,30)` and type 0.
P/R/Q at T691 held `(12,0,25,10)` and type 0. The small paused camera sweep reached
right coordinates only 25..50, never setting the differing bit 6 at `main+0x142D3`;
paused RSNRACK selection displayed 280 (`0x118`), never setting ARMACK's bit 2.
The null twin now spans the map's native scroll extents and samples ARMACK and RSNRACK selection,
while preserving the exact paused-simulation check and existing comparison list.


The native Stop-button callback plays a UI sound at `0x41A0F1`, returning to `0x41A0F6`,
through `0x47F1A0("immediateorders", 0)` (name pointer `0x50271C`). The Stop branch
compares `"STOP"` (`0x502714`), sets mode 1, clears region bit 3 and dispatches through
`0x48CF30` (`0x419CBC`..`0x419CF2`), then jumps to this shared UI sound tail. The Load and
other unarmed command paths share the tail; it is not a
simulation reply produced by `0x47F780`/`0x47F850`. The resource-priority fixture observed three
such calls in its live [481,1681) count window and none on the puppet. Its unit/player/order,
round-trip and fresh-seek comparisons still agree. The owner approved its exact call-site expected-absent row on 2026-10-05; the command
orders and their effects retain the ordinary recorder checks.

The resource profile-label refresh writes only a label (`gadget+0`, ID 5) on a screen whose
OnCommand dispatcher is one of Impure's own menu handlers. `GUI_Load 0x4AA8F0` allocates
`0x10F57` bytes at `0x4AAA68` and advances `ControlsAry` by `0x3F` at `0x4AAA95`: the remaining
`0x10F18` bytes hold exactly 200 records of stride `0x15B`, including panel record zero.
The signed count at panel `+0xB6` must therefore be 1..199 before a refresh walks records
1..count. The profile label's text is `char[128]` at gadget `+0xB6`, ending before its status
word at `+0x137`. This is the allocation bound, not a readable-page probe.

Resource raw coordinates are captured after the `0x499221` conversion independently of zoom.
With zoom disabled, the installed wrapper calls pristine `0x498DA0` directly before capture;
no zoom conversion or viewport rect write runs. Resource hooks require that producer
installation to be live.

The resolver's mode-7 branch at `0x43F4C7` requires definition `+0x245` bit `0x20`
(CanGuard) and a nonzero relationship at `0x43F4D4`. Mode 12 starts at `0x43F4F7` and
requires `+0x245` bit `0x400` (CanReclamate) at `0x43F4FD`. Refusal reaches `0x4401DC`,
loads the caller's output pointer into EAX, writes byte zero at `0x4401E3` and returns
`ret 0x14` at `0x4401E7`. Native dispatch at `0x48D0A5`..`0x48D0B1` tests that byte
and skips zero. A nonnull pointer alone is not eligibility. The resource wrapper returns
`-1` for byte zero before any reclaim/build/Guard plan is accepted.

Visuals Undo snapshots the two raw per-mod resource preferences, not their capped plates.
Restoration uses the settings store's global value bounds rather than the active profile
cap; menu refresh derives the effective values again. Raising a cap can therefore recover
a saved higher radius even after Undo.

Profile polling runs inside the existing `0x4C63A0` flip observer, after its game-thread guard
and before census/publisher early returns. `DrawGameScreen` returns from its flip call at
`0x46A3E0`; that game-flip identity freezes policy even without frame packets. Main-menu
selection releases the freeze. Polling is independent of mailbox capacity and of a live render
consumer; `fill_shell` only fills presentation data. No additional byte patch is installed.

DISASSEMBLED GUI stack removal: `GUI_Pop 0x4A9660` installs the previous active link
`[gi+0x18] = [removed]` at `0x4A96A2` before freeing the removed GUIMEMSTRUCT through
`MEM_Free 0x4D85A0` at `0x4A96AF`. Profile refresh uses that current link on the same
GUI/input thread and retains no GUI pointer beyond the refresh call.
