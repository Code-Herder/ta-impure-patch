# Own the draw — skipping ONLY the composite rasterisation (Gate G7)

*Instruction-level map of the per-unit draw path
`DrawUnit 0x45AC20 → 0x458810 → builder 0x4586A0 → blit 0x459200`, produced to
answer one question precisely: **how do we make TA keep doing pose-sync, AABB,
allocation, hotspot bookkeeping and the blit, but skip only the software
rasterisation that fills the composite's colour + depth planes** (our GPU thread
fills them instead)? All VAs are for the pristine build (ImageBase `0x400000`,
md5 `8e74a1dffa1f5988624c52048f5b20cd`). Disassembly via
`objdump -d -M intel <TotalA.exe>` (PE-direct); `.text` VA→file = `VA − 0x400C00`.*

Evidence tags: **[BINARY-VERIFIED]** = instruction/bytes read for this build this
session (VA + bytes quoted). **[CORPUS]** = a name/layout from the other notes /
TADR, cross-checked against our bytes. **[INFERRED]** = my reading, not yet
runtime-confirmed.

---

## Summary — the one thing to know

The two rasterisers that fill the composite planes — **`0x459830`** (opaque) and
**`0x459C70`** (build/nanoframe) — are both **thiscall, 4 stack args, callee-clean
`ret 0x10`**, and both open with a `mov eax,imm32 ; call __chkstk` pair whose first
instruction is exactly 5 bytes (`B8 xx xx xx xx`). That gives a **clean 5-byte
detour boundary at each callee entry**, identical in spirit to the existing G5
`DrawUnit` suppressor. **Nothing in the builder consumes the rasteriser's return
value** — the builder overwrites `eax` with `1` on the very next instruction after
each call. So a per-unit "classify → `ret 0x10`" no-op stub on each callee leaves
every piece of surrounding bookkeeping (AABB, alloc, the `Object3do+0x10` store,
the hotspot writes) fully intact and simply produces a blank (ColorKey-filled)
composite for our GPU thread to overwrite.

**Recommended: detour the two callees** (`0x459830`, `0x459C70`), not the call
sites — 2 patches cover all 3 rasterise-into-composite sites, and it matches
`tagpu_suppress.c` byte-for-byte in style.

---

## The arming rule — every code-patching pass, not just this one

`tagpu_owndraw.c` was the first of what are now four passes that install engine-code
detours through the shared `tagpu_detour.c`: **`owndraw`** (unit rasterisers),
**`fxown`** (effects), **`featown`** (the feature leaf `0x46A610`) and, planned for G13b,
**`terrown`** (the terrain pass `0x483FA0`). They all obey one rule, and it has cost time
in three separate gates:

> **A code-patching pass installs its detours ONCE at DLL attach, and only if its trigger
> file exists at that moment. There is no per-frame re-arm.** Patching live engine bytes
> off the frame loop is racy, so it is deliberately not attempted.

Consequences worth stating plainly:

- `tacli arm <inst> owndraw.on` **after** launch does nothing at all. The GL/behaviour
  triggers (`native.on`, `fx.on`, `feat.on`, `sfx.on`, and the `.off` toggles) *are*
  re-read every frame and can be flipped live — that asymmetry is the trap, because the
  same `arm` verb behaves differently depending on which trigger you name.
- **A stale own-draw trigger is worse than none.** With `owndraw.on` present but
  `native.on` cleared, the engine's rasterisers are skipped and *nothing* draws the units
  — only health bars. An engine-side A/B then silently measures nothing. The shape of it
  in the log is `OWND … repaint=0 miss=<everything>` with no `native:` lines. `tacli
  launch` now drops a stale `owndraw.on` when native is off and says so.
- Because of both of the above, `tacli launch` / `scenario load` **auto-arm the patching
  companion to match its GL pass** — `owndraw` from `native.on`, `fxown` from `fx.on` or
  `sfx.on`, `featown` from `feat.on` — and print `auto-armed …`. **Any new pass must be
  added to that list** (`tools/tacli`, near `_ensure_featown_for_feat`), or it will appear
  to work for whoever wrote it and fail for everyone else.
- Verify from the log, never from the trigger file: each pass prints an ARMED line naming
  the sites it patched, e.g. `featown: ARMED feature@0x46A610=1`, plus a per-window skip
  counter. No ARMED line means no patch, whatever the filesystem says.

---

## 1. Builder `0x4586A0` — the two rasteriser call sites

Builder convention (entry): **thiscall**, `ecx = this` = the model-layer object
`*(main+0x1437B)`; 3 stack args `[esp+4]=Object3do*`, `[esp+8]=flag`,
`[esp+0xC]=mode`; **callee-clean `ret 0xC`** (epilogues `c2 0c 00` at `0x458776`,
`0x45879C`, `0x4587A8`). [BINARY-VERIFIED]

Live-register state established in the prologue and held across both call sites:
- `esi` = `this` (`mov esi,ecx` @ `0x4586B3`).
- `edi` = `Object3do*` (`mov edi,[esp+0x20]` @ `0x4586AF`, = arg0).
- `ebp` = `UnitStruct*` (`mov ebp,[edi+0xc]` @ `0x4586B8`, = `Object3do+0x0C`
  ThisUnit).
- `eax` = the freshly-allocated composite `GAFFrame*` = `*(Object3do+0x10)`
  (`mov eax,[ebx]` @ `0x45871E`, `ebx = edi+0x10`).

**Order of operations inside the builder (all BEFORE either rasteriser call):**
1. `0x4586C9` `call 0x4581E0` — screen AABB → W/H/HotX/HotY into stack locals.
2. `0x458702` `call 0x437B50` **or** `0x458719` `call 0x437BE0` — allocate the
   composite; the allocator **stores the buffer pointer into `Object3do+0x10`**
   (the `+0x10` store) and fills the colour plane `0x01` / depth plane `0x00`.
3. `0x458729` `mov [eax+0x4],dx` (**HotspotX**) and `0x458732` `mov [eax+0x6],cx`
   (**HotspotY**) — the hotspot writes.
4. state test `test [ebp+0x110],0x20000000` (`0x45873C`) + `test
   [main+0x37F06],0x20` (`0x45874A`) selects nanoframe vs opaque path.

### Call site A — nanoframe/build rasteriser `0x459C70`
```
458753  8b 4c 24 28          mov  ecx,[esp+0x28]   ; arg2 mode
458757  33 d2                xor  edx,edx
458759  8a 95 ff 00 00 00    mov  dl,[ebp+0xff]    ; UnitStruct+0xFF flag byte
45875f  51                   push ecx              ; [esp+0x10] mode
458760  52                   push edx              ; [esp+0x0C] flagByte
458761  57                   push edi              ; [esp+0x08] Object3do*
458762  50                   push eax              ; [esp+0x04] composite GAFFrame*
458763  8b ce                mov  ecx,esi          ; this
458765  e8 06 15 00 00       call 0x459c70         ; <-- RASTERISE (nanoframe)
45876a  b8 01 00 00 00       mov  eax,0x1          ; return value = 1  (rasteriser ret DISCARDED)
45876f  5f 5e 5d 5b          pop  edi/esi/ebp/ebx
458773  83 c4 0c             add  esp,0xc
458776  c2 0c 00             ret  0xc
```
Call VA **`0x458765`**, bytes **`E8 06 15 00 00`**.

### Call site B — opaque rasteriser `0x459830`
```
458779  8b 4c 24 28          mov  ecx,[esp+0x28]   ; arg2 mode
45877d  33 d2                xor  edx,edx
45877f  8a 95 ff 00 00 00    mov  dl,[ebp+0xff]    ; UnitStruct+0xFF flag byte
458785  51                   push ecx              ; [esp+0x10] mode
458786  52                   push edx              ; [esp+0x0C] flagByte
458787  57                   push edi              ; [esp+0x08] Object3do*
458788  50                   push eax              ; [esp+0x04] composite GAFFrame*
458789  8b ce                mov  ecx,esi          ; this
45878b  e8 a0 10 00 00       call 0x459830         ; <-- RASTERISE (opaque)
458790  b8 01 00 00 00       mov  eax,0x1          ; return value = 1  (rasteriser ret DISCARDED)
458795  5f 5e 5d 5b          pop  edi/esi/ebp/ebx
458799  83 c4 0c             add  esp,0xc
45879c  c2 0c 00             ret  0xc
```
Call VA **`0x45878B`**, bytes **`E8 A0 10 00 00`**.

**Convention of BOTH callees** (identical): thiscall, `ecx = this`
(`*(main+0x1437B)`); 4 stack args pushed by the caller —
`[esp+4]=composite GAFFrame*`, `[esp+8]=Object3do*`, `[esp+0xC]=UnitStruct+0xFF
byte`, `[esp+0x10]=mode`; **callee cleans** (see §4, `ret 0x10`). The caller does
**no** `add esp` after the call. [BINARY-VERIFIED]

**Return value is dead — CONFIRMED.** The instruction immediately after each call
is `mov eax,0x1` (`0x45876A`, `0x458790`), so whatever the rasteriser left in `eax`
is overwritten before anything reads it. Skipping the call and returning with a
balanced stack is therefore observationally identical, and the hotspot writes
(step 3, done earlier) and the `+0x10` store (step 2) survive untouched. [BINARY-VERIFIED]

### The third rasterise site — inside the blit `0x459200`
`0x459830` has a **second caller at `0x459641`** (inside `0x459200`, the build-state
self-rasterise-into-scratch path). `0x459C70` has **only** the builder caller.
```
45962f  33 c0                xor eax,eax
459631  6a 00                push 0x0             ; mode 0
459633  8a 81 ff 00 00 00    mov al,[ecx+0xff]    ; UnitStruct+0xFF flag (ecx=ebp+0xC unit)
459639  8b 4f 10             mov ecx,[edi+0x10]    ; ecx = this+0x10 = SHARED scratch composite
45963c  50                   push eax             ; flagByte
45963d  55                   push ebp             ; Object3do*
45963e  51                   push ecx             ; composite
45963f  8b cf                mov ecx,edi          ; this
459641  e8 ea 01 00 00       call 0x459830        ; <-- RASTERISE (opaque, build-state)
459646  8b 55 0c             mov edx,[ebp+0xc]     ; continues to cargo z-merge (ret NOT consumed)
```
Call VA **`0x459641`**, bytes **`E8 EA 01 00 00`**. Same 4-arg thiscall shape.
Reached only when `unit+0x110 & 0x20000000` (build/nanoframe) and speed==0
(`0x45960D..0x45962D`). For "own the draw" we want this skipped too (it also fills a
composite plane) — a **callee detour on `0x459830` covers it for free**; a
call-site patch would have to patch it separately.

### Recommendation for making the rasterise a per-unit no-op
**Detour the two callees** (`0x459830`, `0x459C70`) — cleanest, fewest patches,
matches the existing G5 suppressor exactly:

- **Why callee, not call-site:** one 5-byte detour on `0x459830` neutralises *both*
  its call sites (`0x45878B` builder + `0x459641` blit); patching call sites would
  need 3 separate thunks (`0x458765`, `0x45878B`, `0x459641`). Both callees have a
  clean 5-byte entry boundary (§4); the builder call sites are `E8` too, so
  call-site patching is *possible* (replace `call rasteriser` with `call thunk`;
  thunk does `ret 0x10` to skip or `jmp rasteriser` to pass) but strictly more work.
- **Classification handle at callee entry:** `[esp+8] = Object3do*`, and
  `UnitStruct* = *(Object3do+0x0C)` — feed that to the same read-only name-match
  classifier as `tagpu_suppress.c` (`UnitStruct+0x92 → UnitDefStruct`, match
  Name@0x00 / UnitName@0x20 / ObjectName@0x80).
- **Suppress path = `ret 0x10`.** Pops the return address and the 4 arg dwords —
  exactly what the real callee's `ret 0x10` would do — returning to the builder,
  which runs `mov eax,1 ; … ; ret 0xc` (return dead), or to the blit at `0x459646`
  (continues to the cargo merge). Every bookkeeping write is already done.
- **Result:** the composite is allocated, sized, hotspotted, its `+0x10` pointer
  stored, but its colour plane stays `0x01` (ColorKey) and depth `0x00` — a blank
  transparent sprite the GPU thread then paints (the write-back channel proven in
  `composite-buffer.md §G6`). Pose-sync, AABB, alloc, hotspot and the blit all still
  run. **This is exactly "own the draw".**

---

## 2. Pose-sync `0x45AB10` — how our detour invokes it directly

Prologue bytes **`53 55 56 8B 74 24 10 33 ED 57`**:
```
45ab10  53                   push ebx
45ab11  55                   push ebp
45ab12  56                   push esi
45ab13  8b 74 24 10          mov  esi,[esp+0x10]   ; esi = arg0 = UnitStruct*   (NOT ecx)
45ab17  33 ed                xor  ebp,ebp
45ab19  57                   push edi
45ab1a  8b 8e 9e 00 00 00    mov  ecx,[esi+0x9e]   ; Object3do* = unit+0x9E
...
45ac11  c2 04 00             ret  0x4              ; callee-clean, ONE stack arg
```
**Calling convention: effectively `stdcall(UnitStruct* unit)` — one stack arg at
`[esp+4]`, callee-cleans-4 (`ret 4`). `ecx` is IGNORED on entry** (immediately
overwritten from `[esi+0x9e]`). Although the vtable `0x4FD698` slot 12 *entry*
points here (so C++ dispatches it thiscall with `ecx = COB ctx`), the function does
not read `ecx` — the COB ctx is irrelevant to it. [BINARY-VERIFIED]

Caller (the direct call, `0x480EFC`) proves the arg derivation: [BINARY-VERIFIED]
```
480ef2  8b 96 40 05 00 00    mov edx,[esi+0x540]   ; esi=COB ctx; Object3do* = ctx+0x540
480ef8  8b 42 0c             mov eax,[edx+0xc]      ; UnitStruct* = Object3do+0x0C
480efb  50                   push eax               ; the ONLY arg
480efc  e8 0f 9c fd ff       call 0x45ab10
480f01  8b 5c 24 30          mov ebx,[esp+0x30]     ; NO add esp -> callee cleaned (ret 4)
```

What it does [BINARY-VERIFIED]: reads `Object3do = unit+0x9E`; if the cached body
turn `Object3do+0x18/0x1A/0x1C` differs from live `unit+0x64/0x66/0x68` by ≥8 on any
axis, copies the live turn in and sets **`Object3do+0x08 = 1`** (pose-dirty); then
**if `Object3do+0x08 != 0`** (`0x45AB94`) it reposes — refreshes each piece's vertex
buffer (`rep movs`), walks Child `+0x2E` / Sibling `+0x2A` via `0x45B030`, composes
via `0x45B0A0(ecx=Object3do, edx=Object3do+0x1E BaseObject, push 0)`, and clears
`Object3do+0x08 = 0`.

**To invoke directly from our detour, given a `UnitStruct* unit`:**
```
push unit
call 0x45AB10          ; ecx = don't-care; it cleans its own arg (ret 4)
```
No `this` needed, no ctx needed. This is the call to force a fresh pose after we
suppress the engine's own lazy repose (the G5 caveat in `frame-composition.md`:
suppressing `DrawUnit` stops `vbuf`/`org` refreshing) — feed it the `UnitStruct*`
and it repopulates `prim+0x22` (posed verts) / `prim+0x16` (origin) exactly as TA
does before drawing.

---

## 3. Dispatch `0x458810` — the rebuild-vs-blit branch, and the blit convention

Entry: **thiscall** `ecx = this` (`*(main+0x1437B)`), `[esp+4]=Object3do*`,
`[esp+8]=OFFSCREEN* ctx`; `ret 0x8` (`c2 08 00` @ `0x458957`). Loads
`esi=eyeX (main+0x1431F)`, `ebp=eyeY (main+0x14323)`, `edi=Object3do*`,
`ecx=[edi+0xc]=UnitStruct*`. [BINARY-VERIFIED]

### The rebuild decision — field tested and the branch VA
The function builds a **rebuild flag** in stack local `[esp+0x10]` (init 0) and then
branches on it:
```
458870  39 57 04             cmp [edi+0x4],edx       ; edx=0  -> test Object3do+0x04 (TimeVisible)
458873  75 08                jne 0x45887d
458875  c7 44 24 10 01000000  mov [esp+0x10],1        ; TimeVisible==0  => REBUILD
...
4588f2  8b 44 24 10          mov eax,[esp+0x10]        ; the rebuild flag
4588f6  85 c0                test eax,eax
4588f8  74 19                je  0x458913              ; ==0 -> STRAIGHT TO BLIT
4588fa  ...(rebuild)...      ; !=0 -> call builder 0x4586A0 then fall through to blit
45890c  e8 8f fd ff ff       call 0x4586a0             ; builder(this,Object3do,0,1)
```
**The decisive branch is `0x4588F2` (`test eax,eax ; je 0x458913`, bytes
`85 C0 74 19`).** The field that primarily drives it is **`Object3do+0x04`
(TimeVisible)**, tested at **`0x458870`** (`cmp [edi+0x4],edx ; jne` — bytes
`39 57 04 75 08`): **TimeVisible == 0 ⇒ rebuild.** [BINARY-VERIFIED]

Secondary contributors to the same flag (all set `[esp+0x10]=1`): composite ptr
`Object3do+0x10` null combined with the nanoframe state bit `unit+0x110 &
0x20000000` (`0x4588C9`), the cloak/speed nanoframe test on `unit+0x104`
vs `ds:0x4FD4C0` (`0x45889E..0x4588B4`), and `unit+0x114 & 1` with composite null
(`0x4588DD`). **`Object3do+0x08` is NOT tested here** — the pose-dirty `+0x08` check
lives in `0x45AB10`/`DrawUnit`, not in `0x458810`. So the answer to "which field
decides rebuild-vs-blit" is **`Object3do+0x04`** (feeding `0x4588F2`), not `+0x08`.

Whether or not it rebuilt, control converges at `0x458917`, reloads
`ecx = Object3do+0x10` (the composite), and if non-null takes the blit; after the
blit it does **`inc [edi+0x4]`** (`0x45894D`) — i.e. `Object3do+0x04` is the draw
stamp incremented every frame and zeroed on invalidation (the field `0x458870`
tests). [BINARY-VERIFIED]

### The blit `0x459200` — calling convention
```
45892e  8b 4c 24 2c          mov ecx,[esp+0x2c]     ; cloak/alpha byte
458932  51                   push ecx               ; [esp+0x18] cloakByte     (arg6, highest)
458933  8b cb                mov ecx,ebx            ; ecx = this
458935  83 ec 0c             sub esp,0xc            ; reserve 3 dwords
458938  8b d4                mov edx,esp
45893a  57                   push edi               ; [esp+0x08] Object3do*
45893b  89 32                mov [edx],esi          ; [esp+0x0C] eyeX<<16
45893d  89 42 04             mov [edx+4],eax        ; [esp+0x10] ? (unused 5th)
458940  8b 44 24 44          mov eax,[esp+0x44]     ; OFFSCREEN* ctx
458944  50                   push eax               ; [esp+0x04] ctx            (arg1, lowest)
458945  89 6a 08             mov [edx+8],ebp        ; [esp+0x14] eyeY<<16
458948  e8 b3 08 00 00       call 0x459200
45894d  ff 47 04             inc [edi+0x4]          ; NO add esp -> callee cleaned
```
**`0x459200`: thiscall**, `ecx = this` (`*(main+0x1437B)`); 6 stack args
`[esp+4]=OFFSCREEN* ctx`, `[esp+8]=Object3do*`, `[esp+0xC]=eyeX<<16`,
`[esp+0x10]=? (unused 5th, an fp/coord slot)`, `[esp+0x14]=eyeY<<16`,
`[esp+0x18]=cloak/alpha byte`; **callee-clean `ret 0x18`** (`c2 18 00` @ `0x45949A`).
Prologue `sub esp,0x20 ; push ebx ; push ebp ; mov ebp,[esp+0x30]` confirms
`arg1=Object3do*`, `mov edi,ecx` confirms `this`, `mov esi,[ebp+0x10]` loads the
composite. [BINARY-VERIFIED] Matches the notes' order
`(this, OFFSCREEN* ctx, Object3do*, eyeX<<16, ?, eyeY<<16 [, cloakByte])`.

---

## 4. Rasterisers `0x459830` / `0x459C70` — prologue & detour boundary

Both open with a large-frame stack probe: `mov eax,imm32 ; call __chkstk 0x4E4B20`
(`0x4E4B20` verified to be the standard `_chkstk`: takes probe size in `eax`,
page-probes and subtracts from `esp`). [BINARY-VERIFIED]

**`0x459830` (opaque):**
```
459830  b8 04 5f 00 00       mov eax,0x5f04         ; <-- 5 bytes: DETOUR HERE
459835  e8 e6 b2 08 00       call 0x4e4b20          ; __chkstk (resume point)
45983a  a1 e8 1d 51 00       mov eax,ds:0x511de8
45983f  53 55 56 57          push ebx/ebp/esi/edi
...
459c67  c2 10 00             ret 0x10               ; thiscall, 4 args, callee-clean
```

**`0x459C70` (nanoframe):**
```
459c70  b8 d4 59 01 00       mov eax,0x159d4        ; <-- 5 bytes: DETOUR HERE
459c75  e8 a6 ae 08 00       call 0x4e4b20          ; __chkstk (resume point)
459c7a  a1 e8 1d 51 00       mov eax,ds:0x511de8
459c7f  53 55 56 57          push ebx/ebp/esi/edi
...
45a46c  81 c4 d4 59 01 00 /  add esp,0x159d4
        c2 10 00             ret 0x10               ; thiscall, 4 args, callee-clean
```

Both: **thiscall (`ecx = this`), 4 stack args `[esp+4]=composite`,
`[esp+8]=Object3do*`, `[esp+0xC]=flagByte`, `[esp+0x10]=mode`, callee-clean
`ret 0x10`.** [BINARY-VERIFIED]

**Detour boundary is clean at 5 bytes.** The first instruction is exactly 5 bytes
(`B8 imm32`), so a `E9 rel32` overwrites it whole with no straddle. The passthrough
stub replays that one instruction and jumps to entry+5 (the `call __chkstk`):
- `0x459830`: stolen `B8 04 5F 00 00`, resume `0x459835`.
- `0x459C70`: stolen `B8 D4 59 01 00`, resume `0x459C75`.
The suppress path never runs `__chkstk` at all (it just `ret 0x10`s), so no frame is
allocated — correct, since we do no work.

---

## Recommended patch plan

**Goal:** keep pose-sync + AABB + alloc + hotspot + `+0x10` store + blit; skip only
the plane-filling rasterisation, per targeted unit type. Read-only classification,
no sim writes — the `tagpu_suppress.c` model.

### Patch — detour both rasterisers (mirror `tagpu_suppress.c`)
Two 5-byte `E9` detours to a generated RWX stub, one per callee. Byte-guard each
entry before patching (bail on mismatch):

| Detour at | Guard bytes (first 5) | Resume (entry+5) | Covers |
|---|---|---|---|
| **`0x459830`** | `B8 04 5F 00 00` | `0x459835` | builder site `0x45878B` **and** blit site `0x459641` |
| **`0x459C70`** | `B8 D4 59 01 00` | `0x459C75` | builder site `0x458765` |

**Stub behaviour (each, identical shape to `tagpu_suppress.c`):**
```
60                   pushad
FF 74 24 28          push [esp+0x28]        ; Object3do*  ([esp+8] arg, +0x20 for pushad)
E8 <rel>             call classify_obj3do   ; __cdecl; reads UnitStruct=*(Object3do+0x0C),
                                             ;   name-matches +0x92 def (Name/UnitName/ObjectName)
83 C4 04             add esp,4
85 C0                test eax,eax
61                   popad
75 08                jnz suppress           ; skip = length of passthrough block (8)
B8 <imm32>           mov eax,<0x5f04 | 0x159d4>   ; replay stolen instruction
E9 <rel>             jmp <0x459835 | 0x459c75>    ; resume real rasteriser
suppress:
C2 10 00             ret 0x10               ; skip: clean 4 args, return to caller
```
`classify_obj3do(Object3do*)` = the existing `should_suppress` with one extra
deref: `unit = *(UnitStruct**)(obj3do + 0x0C)` then the current name match. Passthrough
`jnz` displacement is the fixed 8-byte block (`B8 imm32` + `E9 rel32`).

**Why this is correct (all [BINARY-VERIFIED] above):**
- Rasteriser return value is dead — builder does `mov eax,1` right after
  (`0x45876A`, `0x458790`); blit continues to the cargo merge (`0x459646`). Our
  `ret 0x10` matches the real callee-clean unwind exactly.
- Everything the builder must keep runs *before* the call: AABB `0x4581E0`, alloc +
  `+0x10` store `0x437BE0`/`0x437B50`, hotspot writes `0x458729`/`0x458732`.
- The composite ends up allocated/sized/hotspotted with ColorKey-blank planes — the
  exact buffer the GPU thread paints (proven channel, `composite-buffer.md §G6`).

### Companion — drive our own pose (fixes the G5 lazy-repose caveat)
Because we now let `DrawUnit`/`0x458810`/`0x4586A0` run (unlike the G5 `DrawUnit`
suppressor), TA still reposes visible units — so `vbuf`/`org` stay fresh and no extra
call is strictly required for the *engine* path. If we later also suppress higher up
(e.g. detour `DrawUnit 0x45AC20`) and need a fresh pose for our GPU read, call
**`0x45AB10`** directly: `push unit ; call 0x45AB10` (ecx don't-care, `ret 4`). It
reposes iff `Object3do+0x08` is dirty and repopulates `prim+0x22`/`prim+0x16`.

### Coexistence
The rasteriser detours are on *different* bytes from the G5 `DrawUnit` detour
(`0x45AC20`) and the tracer's `0x459200` detour, so all three can install together.
If both this and the G5 `DrawUnit` suppressor target the same unit type, the
`DrawUnit` suppressor removes the unit entirely (no builder, no blit) and wins;
"own the draw" is the alternative that *keeps* the blit — do not arm both for the
same type.

---

## Appendix — addresses, bytes, conventions

| VA | Bytes / epilogue | Role | Convention |
|---|---|---|---|
| `0x4586A0` | `83 EC 0C …` / `c2 0c 00` | composite **builder** | thiscall, 3 args, ret 0xC |
| `0x458765` | `E8 06 15 00 00` | builder → **nanoframe rasterise** call | — |
| `0x45878B` | `E8 A0 10 00 00` | builder → **opaque rasterise** call | — |
| `0x459641` | `E8 EA 01 00 00` | blit (build-state) → **opaque rasterise** call | — |
| `0x459830` | `B8 04 5F 00 00 …` / `c2 10 00` @`0x459C67` | **opaque rasteriser** | thiscall, 4 args, ret 0x10 |
| `0x459C70` | `B8 D4 59 01 00 …` / `c2 10 00` @`0x45A46C` | **nanoframe rasteriser** | thiscall, 4 args, ret 0x10 |
| `0x4E4B20` | `_chkstk` | large-frame stack probe (size in eax) | — |
| `0x458810` | `83 EC 18 …` / `c2 08 00` @`0x458957` | dispatch (rebuild-vs-blit) | thiscall, 2 args, ret 0x8 |
| `0x458870` | `39 57 04 75 08` | **`Object3do+0x04`==0 ⇒ rebuild** test | — |
| `0x4588F2` | `85 C0 74 19` | **rebuild-vs-blit branch** (`je 0x458913`) | — |
| `0x45890C` | `E8 8F FD FF FF` | dispatch → builder call (this,obj,0,1) | — |
| `0x458948` | `E8 B3 08 00 00` | dispatch → **blit** call (ret `0x45894D`) | — |
| `0x459200` | `83 EC 20 …` / `c2 18 00` @`0x45949A` | composite → offscreen **blit** + cargo z-merge | thiscall, 6 args, ret 0x18 |
| `0x45AB10` | `53 55 56 8B 74 24 10 …` / `c2 04 00` @`0x45AC11` | **pose-sync + tree walk** | stdcall(UnitStruct*), 1 arg, ret 4, ecx ignored |
| `0x480EFC` | `E8 0F 9C FD FF` | direct caller of `0x45AB10` (arg = `*(Object3do+0x0C)`) | — |
| `Object3do+0x04` | — | TimeVisible / draw stamp — 0 ⇒ rebuild; `inc` after blit | — |
| `Object3do+0x08` | — | pose-dirty (tested in `0x45AB10`, not `0x458810`) | — |
| `Object3do+0x0C` | — | ThisUnit (`UnitStruct*`) — classify handle at rasteriser entry | — |
| `Object3do+0x10` | — | composite `GAFFrame*` (the `+0x10` store; GPU write target) | — |

## Implemented & proven live — 2026-08-31 (same session)

`tagpu/ddraw/src/tagpu_owndraw.c` implements exactly the recommended plan: two
classify-then-`ret 0x10` stubs (the `tagpu_suppress.c` shape — pushad → cdecl classifier →
popad → skip or stolen-bytes+resume) detouring `0x459830` and `0x459C70` at their
`mov eax,imm32` prologue boundaries, byte-match guarded, armed by `tagpu_owndraw.on`
(first token = type, `all` = every unit). Classification: `[esp+8]` = `Object3do*` →
`+0x0C` ThisUnit → `+0x92` def → the usual three-name match. Wired in `dllmain.c` after
the tracer; per-frame counter flush in the overlay.

**Live result (skirmish, armcom, writeback+owndraw both armed):** `owndraw: ARMED …
opaque@0x459830=OK nano@0x459C70=OK`; counters run `skipped≈1700–4200 per 60-frame window,
passed=0` — every rasterise call for the commander is intercepted, none for other units
fire (only the commander is in LOS). The on-screen commander remains our full textured GPU
render, stable, at speed. **Negative proof:** firing `tagpu_diff.trigger` (forces
TimeVisible=0 + pose-dirty → full rebuild) yields an engine colour plane that is **100%
ColorKey — all 1548 bytes = 0x01** — the engine allocated, sized and hotspotted the
composite but wrote not a single pixel; our writeback repaints it and TA blits our
content. The engine now does bookkeeping + blit only; the GPU path owns the pixels.

Observation for later: the skip counter ticks continuously (~30–70/frame, plausibly
per-face calls during idle-anim rebuilds) yet the sprite never blanks — the builder
appears to reuse the same-size allocation without re-clearing, so our pixels persist
across rebuilds; only the trigger's full invalidation produced a cleared plane. Worth
pinning down when we characterise rebuild cadence for moving units.
