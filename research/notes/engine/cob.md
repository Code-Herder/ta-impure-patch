# The COB engine

The COB object, the loaded script, the runner and its opcodes, piece animation, get/set and the guarded runtime.

*Part of the [engine map](../exe-reverse-engineering.md). A function's entry is the section whose heading names it — the [address index](address-index.html) lists them: a new fact about a function extends its entry, in place; a new section is for an area nobody has mapped yet.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### COB on a puppet: getter 75, the writers COB reaches, fresh COB [DISASSEMBLED + SOURCE 2026-09-29]

What decision 2 of the [completeness note](../tadr-port/demo-recorder-completeness.md#decisions-taken)
rests on. A puppet's units run their COB as a live remote copy's do: the unit tick admits type-3
records and runs `DoScriptsNow(1)` for every unit with `+0x9A` (`0x48ADDF..0x48ADEB`), while
AutoAim and the local block (orders, movement, regeneration) run only for types 1 and 2
(`0x48ADC9..0x48ADDA`, `0x48AEB5..0x48AECD`). What the supported mods do with getter 75 is in the
[state design evidence](../tadr-port/demo-recorder-state-evidence.md).

- **Getter 75 is the only machine-dependent getter.** Impure's `cob_get` (the E9 at `0x480770`,
  `tagpu_patches.c`) answers 1 exactly when the queried unit's owner record has `+0x73` of 1 or 2,
  the engine's own "simulated here" test (AutoAim `0x48ADCE`, the local block `0x48AEC4`, the
  `0x11` sender `0x48B1C0`); unlike those gates it does not test that the record is active. It
  bounds the id by `u16 main+0x14351` and requires the live bit, `+0xA8` = id and the owner byte
  `+0xFF` below 10. `+0xFF` is stored only by the level init `0x4854A0` (`0x485880`, `0x48590E`):
  no other byte store reaches it, and the word stores at `+0xFE` are to player records, feature
  defs and a parser, never a unit. No getter, stock or Impure's, reads the controlled or viewed
  player, the network bit, the game type, GameTime or the sim RNG; the only player-record bytes
  any reads are `+0x73` (75) and the alliance row `+0x108` (74). On a puppet every seat is type 3,
  so 75 answers 0 for every unit, as every live peer does for a unit it does not simulate
  (MEASURED on two Wine peers in all five supported mods, [cob.md](../tadr-port/cob.md)).
- **Where getters run.** Inside the runner `0x4B0DA0`: `GET_UNIT_VALUE` calls `vt+0x44` at
  `0x4B160F`, `GET` at `0x4B1678`. The runner is reached from `DoScriptsNow 0x4B0D60` (the unit
  tick, `0x48ADEB`), from a start with run-now set (`0x4B09D9` in `0x4B0940`, `0x4B0B97` in
  `0x4B0B00`) and from the synchronous query `0x4B0C40` (`0x4B0CE1`). Every create runs Create
  with run-now set (`0x485D40`: `push 1` at `0x485DDD`, the call at `0x485DE6`; called from
  `0x4860A6` in the create `0x485F50` and `0x4862BE` in CreateFromNetwork `0x4861D0`), so Create's
  first slice, and any getter in it, runs inside the create: on the game thread in play, and on
  the loader thread while the level load creates units (`0x497963`, `0x497ACD`, `0x4977BB`,
  `0x497B29`, `0x497B40` in the loader body `0x497180`).
- **The COB-to-engine writers.** SET, `vt+0x40` = `0x480B20`, maps `id − 1` through the byte
  table `0x480C18`: ACTIVATION → `0x48B090(unit, 1, v)` (`0x480B52`); INBUILDSTANCE, BUSY and
  BUGGER_OFF → `+0x10F` bits 0, 1 and 3; YARD_OPEN → `0x47DAC0(unit, v)` (`0x480BAE`); ARMORED →
  `0x48B090(unit, 2, v)` (`0x480BEC`); every id, known or not, sets `+0xBA` bit 2. ATTACH,
  `vt+0x38` = `0x481340`, and DROP, `vt+0x3C` = `0x4813B0`, call `0x48AAC0` (`0x4813A6`,
  `0x481422`), which has no owner test: it sends a `0x0A` from the DirectPlay id `0x44FDB0`
  returns (the first active type-1/2 record) and applies it locally through `0x48AB70`
  (`0x48AB58`, `0x48AB62`). EXPLODE, `vt+0x34` = `0x481140`, without flag `0x20` draws the sim RNG
  six times and spawns debris `0x421620` (`0x48116F..0x4811B9`, `0x48123C`); its flag bits 8..13
  add explosions through `0x420A30`. EMIT_SFX, `vt+0x30` = `0x480EB0`, emits particles only, and
  only when the unit is in the viewed player's sight (`0x465AC0`, `0x480EC5..0x480EEC`).
  PLAY_SOUND `0x10072000` kills its own thread (`0x4B1B60`).
- **`0x48B090(mask, value)`** (thiscall on the unit, `ret 8`; 45 call sites) computes the new
  byte as `value ? old | mask : old & ~mask` of `+0x10E`, both arguments taken as bytes
  (`0x48B09A..0x48B0D4`), stores it (`0x48B0D8`) and returns at once when the byte did not change
  (`0x48B0DE`). Its effects, DISASSEMBLED `0x48B0D8..0x48B1FC` (2026-10-06), start at `0x48B0E4`
  and end with the last reply call at `0x48B1AA`: with the bits newly
  set in `bl` and the bits newly cleared at `[esp+0x14]`, bit 0 set starts `Activate` (the name at
  `0x501280`) through `0x4B0940` on the unit's script `[+0x9A]` with run-now clear (`0x48B106`) and
  queues reply kind 3 through `0x47F780(unit, 3, 0)` (`0x48B110`); bit 0 cleared starts
  `Deactivate` (`0x501274`, `0x48B12B`) and queues kind 4 (`0x48B135`); bit 3 set starts
  `StartBuilding` (`0x5050F4`, `0x48B14E`) and bit 3 cleared `StopBuilding` (`0x505104`,
  `0x48B169`), with no reply; bit 2 set queues kind 14 (`0x48B178`) and hands event `0x10000` to
  every reference on the unit's `+0xA2` chain (`0x48B17D..0x48B19C`, the owner's `vtable[0]`, which
  ORs it into an order node's `+0x4E`), and bit 2 cleared queues kind 15 (`0x48B1AA`). Then
  `0x41C110(unit)` (`0x48B1B0`), which sets `main+0x37EBE` bit 4 for a unit of the controlled
  player with `+0x110` bit 4, and the `0x11` (`0x11`, the slot `+0xA8`, the new byte; 4 bytes)
  through `0x451DF0` only when the owner record `[+0x96]` is active and of type 1 or 2
  (`0x48B1B5..0x48B1F3`). It has no result: EAX holds whatever its last load or call left. `0x47F780(unit,
  kind, 0)` acts only for a live unit of the viewed player and queues a status line through
  `0x47FAD0` (table `0x5086E8`). It never reaches the sound entry `0x47F0C0`, whose callers are
  `0x45562D`, `0x47F1FA`, `0x47F274` and `0x47F31A`: the queue's consumer `0x47F680`, called once
  per main-loop pass (`0x4999A6`), plays a reply through `0x47FD70` and `0x4D0640` by file name and
  sends nothing. Bit 0 queues reply kind 3 or 4 (`0x48B110`, `0x48B135`), bit 2 kind 14 or 15
  (`0x48B178`, `0x48B1AA`).
- **The build finish `0x41B8D0(builder, built)`** (stdcall, `ret 8`), DISASSEMBLED
  `0x41B8D0..0x41BA51` (2026-10-06). It returns at once unless the builder is non-NULL and live
  (`+0x110` bit 28), its def's `+0x156` is nonzero, and the built unit is non-NULL and live
  (`0x41B8D6..0x41B917`). Then on the built unit: the model instance's composite frame pointer
  `[+0x9E]+0x10` = 0 (`0x41B923`) -- the cached 1x bake, not a nanoframe flag: cleared, the next
  draw bakes the unit again as a finished one (*The composite and the build fraction*, below) --
  `+0x104` = 0 (`0x41B933`) and `+0x110` bit 13 set (`0x41B930`, `0x41B93D`). For an owner of type 1 or 2 (`0x41B945..0x41B959`): with `+0x110` bit 29, when
  `BUILDER.GUI` (`0x502844`) is on top (`0x4AB060`) its refresh flag through `0x49FA90`
  (`0x41B95B..0x41B989`), otherwise, with `+0x86` set, `0x48AAC0(built, 0, −1, 1)` (`0x41B990..
  0x41B9A1`). With def `+0x241` bit 18, `0x48B090(1, 1)` on the built unit (`0x41B9A6..0x41B9BF`)
  -- the activation that starts `Activate` -- and the code after it (`0x41B9C4`) does not read
  EAX. When `main+0x37E9C` names the builder (its `+0xA8`), `0x4199B0(main+0x519, builder)`
  (`0x41B9C4..0x41B9E0`), which relabels the top GUI window's unit buttons: for each gadget
  (stride `0x15B`, from `[[gui+0x18]+4]+0x15B`, count at `+0xB6`) of type byte 1 with flag 4 it
  looks the button's type up by name (`0x488B10`) and writes `+%d` (`0x502660`) of the builder's
  queued count of it, `0x439D80(builder, type)`, into the gadget's label, or empties it; flag 8
  writes `%d`/` +%d` from the builder's `+0x1E` and its count; then `0x49FA90(gui)`
  (`0x4199B0..0x419AB1`, the label writes `0x4199E9..0x419A6F`); with def `+0x241` bit 24, `+0xF5` = 7 and `+0x110` bit 14
  (`0x41B9E5..0x41BA08`); for an owner of type 1 or 2 the `0x12` through `0x4560C0(builder,
  built)` (`0x41BA0E..0x41BA26`); and when either unit has `+0x110` bit 4, `main+0x37EBE` bit 4
  (`0x41BA2B..0x41BA48`).
- **The composite and the build fraction** [DISASSEMBLED 2026-10-06, the pristine exe]. The model
  instance's `Object3do+0x10` is its composite frame, the cached 1x bake: the builder `0x4586A0`
  stores its allocation through `lea ebx,[edi+0x10]` (`0x4586FA..0x45871E`), the `O3_COMPOSITE` of
  `tagpu_engine.h` ([composite-buffer](../composite-buffer.html)).
    - **It depends on `+0x104` only through whether it is 0.0.** The allocation is colour + depth
      (`0x437BE0`) for a nonzero second argument, `+0x114` bit 0 or `+0x104 != 0`
      (`0x4586D2..0x4586F0`), else colour only (`0x437B50`). The piece filter skips a piece whose
      cached bit (prim `+0x28` bit 1) differs from the requested mode only when `+0x104 == 0` (the
      plain bake `0x459963..0x459985`, the Gouraud bake `0x459DB8..0x459DCC`). So an unfinished
      unit's bake holds every visible piece and a depth plane, a finished one's only the cached
      pieces. The only `+0x104` reads in `0x458000..0x45B000` are `0x4586DF`, `0x45889E`,
      `0x458DF5`, `0x458E92` (the blit-time scaffold), `0x45961C`, `0x459974` and `0x459DBB`.
    - **"Finished" is C3.** Every one of those reads but `0x458E92` (which scales the scaffold by the
      fraction, `fmul [0x4FD4C4]`) is a zero test of the form `fld; fcomp [0x4FD4C0]` (0.0);
      `fnstsw; test ah,0x40`: true for 0.0, -0.0 and a NaN alike.
    - **When the draw `0x458810` rebakes.** When `Object3do+4` is 0 (`0x458870`); for a structure
      (`+0x110` bit 29) when the composite is NULL (`0x45889C`) or when it is a nanoframe with
      `+0x110` bit 13 and no depth plane (`0x45889E..0x4588BF`); with `+0x114` bit 0 when NULL
      (`0x4588DD`). A NULL composite also clears the shadow cache `+0x14` (`0x458884`). A
      non-structure with a NULL composite draws its pieces directly (`0x45895A..0x4589AA`). **A
      finished structure with a composite is never rebaked by the draw.**
    - **What a stale bake shows.** With a depth plane the blit `0x459200` copies the composite and
      runs the scaffold (`0x4589C0` -> `0x458DD0`, which returns at `+0x104 == 0`), then, for a
      unit that is not an active nanoframe, redraws the dont-cache pieces live (`0x459830(...,
      mode 0)`, `0x45960D..0x459641`); the waterline erase/tint and the digger clip are on this path
      only (the split at `0x45927E`). So a finished unit that keeps its nanoframe bake draws its
      dont-cache pieces twice on GDI (the bake's frozen copy under the live ones) and is clipped as
      a depth-plane unit in both renderers ([INFERRED from these paths; not seen on screen]).
    - **The round robin clears it on every `0x2C` record that names a type.** `0x48B3F0` zeroes
      `ebp` (`0x48B40E`) and stores it through `[unit+0x9E]+0x10` (`0x48B4A6`) whether or not the
      fraction changed; it writes the fraction and bit 13 only when it differs
      (`0x48B4C6..0x48B4EC`).
    - **Who stores 0 there** (a pattern scan of the whole `.text`: every load of `+0x9E` followed
      within four instructions by a store to `+0x10`; complete for that pattern only): `0x41B923`
      (the finish), `0x485DF2` and `0x485E1F` (the create `0x485D40`), `0x48B4A6` (the round robin).
      The COB `cache`/`shade` handlers clear it through the COB's model pointer (`[cob+0x540]+0x10`,
      `0x480DE3`, `0x480E24`). **Negative:** the build step `0x41BA60` writes `+0x104` (`0x41BC16`,
      `0x41BC96`) and never the composite; it calls the finish at `0x41BCBF` when the fraction
      compares equal to 0.0.
    - **The model's lifetime.** The create stores the model and clears its composite at `0x485DF2` /
      `0x485E1F`; the destructor frees it
      ([`FreeObjectState`](memory.md#0x45aaa0-freeobjectstate-the-object3do-destructor)).
    - **A puppet runs no build step.** The unit tick runs a unit's orders only for an owner of
      `+0x73` 1 or 2; any other goes to the reaper (`0x48AEB5..0x48AECD` -> `0x48AFB9`).
- **A remote copy takes `+0x10E` only through `0x48B090`**, called twice (set the byte's bits,
  clear the rest) by the `0x11` receiver (`0x4555A1`, `0x4555B0`) and the round robin
  (`0x48B4F6..0x48B519`). Neither carries `+0x10F`, so a remote copy's `+0x10F` comes only from
  its own COB.
- **COB never reaches the script-start broadcasters.** `0x456190`, `0x456200` and `0x456290` send
  a `0x10` and start nothing locally. Their only callers are five engine sites, each right after
  the matching local start: `0x4385D7` (StartBuilding, in `0x438590`), `0x438632` (StopBuilding,
  in `0x4385F0`), `0x43A258` (StopBuilding, in the order destructor), `0x411403` (BeginTransport)
  and `0x49E3A6` (the Aim scripts, in AutoAim). START (`0x4B18BB`) and CALL (`0x4B1928`) call only
  the allocator `0x4B08C0`, which calls nothing, and so does Impure's guarded step that replaces
  them: a start a script issues is never broadcast, and runs again on each copy from its parent.
  Since those five sites are the broadcasters' only callers, a start the engine makes anywhere else
  is shown only by the machine that runs the code making it: `HitByWeapon` (`0x489F43`) and
  `TakeDamage` (`0x489F8E`) where the hit `0x489CE0` is applied, and `TransportPickup` (`0x406834`)
  and `TransportDrop` (`0x4069BF`) through `0x4B0A70` and `EndTransport` (`0x40F433`, `0x41148D`,
  `0x411794`, `0x411DA1`, `0x411E2B`) through `0x4B0940`, in the transport orders.
- **Stock runs COB queries on remote units** in three places, with no owner gate: the `0x0D`
  receiver `0x49D270` on the branch where the weapon def's `+0x111` bit 5 is clear (the shooter
  from `msg+0x21`, through `0x49C740` to `Query<w>` at `0x43E227`, beside the `Fire*` and
  `RockUnit` starts); CreateFromNetwork `0x4861D0` → `0x49E070` (`Query<w>`, `AimFrom<w>` and the
  start of `SetMaxReloadTime`), reached when a `0x2C` finds a slot of another type (`0x48BA00`),
  from the round robin (`0x48B497`) and from the dispatcher (`0x4553E4`); and the Killed query
  `0x4865C3` in `Send_UnitDeath 0x4864B0`, reached by the reaper `0x48AFD1`, CreateFromNetwork's
  replacement `0x486244` and kill-all `0x486FB4`. Every other query site is local-only.
- **Only a create gives fresh COB.** `0x485D40` has three callers: `0x4860A6` (the create
  `0x485F50`), `0x4862BE` (CreateFromNetwork) and `0x485EFE` (in the uncalled `0x485E90`). The
  destructor frees the old object through `[vt+0x50](../1)` = `0x485E30` (`0x486D86..0x486D8D`), and
  the mover install `0x485E50` touches only the mover and the heading. A seat flipped in place
  keeps its units' COB threads and statics, and a thread keeps the branch it took on a getter-75
  answer until it tests again. A create reads `+0x73` twice: Create's first slice (getter 75) and
  `0x43DC00`'s mover choice (`0x43DC48..0x43DC53`: proxy movers `0x490940`/`0x44F570`, local
  movers `0x4907E0`/`0x44F010`).
- **The sim RNG is per machine.** COB RAND pushes `lo + 0x4B6C30(hi − lo + 1)`
  (`0x4B15BD..0x4B15F2`). `0x4B6C30` steps Park–Miller on `0x51FC88`, which the loader body seeds
  once per level from `QueryPerformanceCounter` (`0x49718C..0x49719D`, stored as
  `(seed ^ 0x66E29572) | 1` at `0x4B6CAB`); `0x51FC88`'s only stores are `0x4B6C87` and
  `0x4B6CAB`. A type-3 seat has no `0x5119C0` object (the player init skips it,
  `0x46489D..0x4648A5`), so a puppet also draws less often than the live game did.
- **A playing's level-start units keep the recording machine's COB clock.** The recording machine's
  loader made them before its first unit tick, which gave each one slice (`0x48ADEB`). A playing
  skips the loader's seat setup and makes them at the first iteration's capture point, the first
  call of B2's stub, before the stub calls `0x48AD30` (`tagpu_replay.c`, THE FIRST STEP'S UNITS),
  so the host's first unit tick gives each the same slice at the same GameTime. The capture
  point's write of the recorded globals follows the creates there, so that slice's COB RAND
  (`0x4B15C9..0x4B15EE` → `0x4B6C30`) draws from the recording's `0x51FC88`, not from the seed
  the host's loader took from `QueryPerformanceCounter` (`0x49719D`). Made after the
  unit tick, every COB thread of a level-start unit would run one tick behind for the whole level,
  and a walker's `MotionControl`, a loop that never ends, would sample its moving and aiming inputs
  a tick late and choose `walk`, `walklegs` or `stand` differently from the first input change
  inside the lag on. MEASURED 2026-10-12 with `tools/tacob`'s VM fed each
  tenseat commander's recorded inputs: made after the unit tick it gives walk/walklegs/stand
  103/3/394 over [481, 1411) and 109/0/445 over [301, 1291), the puppets' measured counts; made
  before it, 102/6/393 and 108/3/442, the live machines' measured counts, call for call.
  Residuals of the same class:
  - **A unit created during play** in a later slot of a block not yet finished (a factory's
    product, a builder's nanoframe) gets its slice in the creating tick on the recording machine
    (the slot loop's re-read, the unit tick's row above). Closed by the slice marker: its create's
    Create start (`0x485DE6`) is an engine start on its COB since the capture point, so the
    recorder writes `RK_EV_SLICE` just before its slice, after the create's own events, and a
    playing gives the slice there once it has made the unit (*The COB module's entries*, below;
    `tagpu_replay.c`, THE SCRIPT SLICES).
  - **AutoAim's starts (A5)** -- TargetCleared (`0x48A1E0` at `0x49E1E1`, the start at `0x48A2E0`),
    the aim script (`0x49E31C`, `0x49E386`) with its broadcast (`0x49E3A6`), FirePrimary and
    RockUnit from the fire function (`0x49CB94`, `0x49CBEB`) -- run in the unit's own turn, before
    its slice. Closed the same way: each is an engine start on the unit's COB, so its slice is
    marked, and a playing replays the recorded start, broadcast and shot before the marker and
    gives the slice at it.
  - **The walk class's run-now pass against starts made after the unit tick.** The AI changes
    orders in the players phase, after the unit tick; a StopBuilding made there goes in with step
    T's events, before image T+1's walk class, whose run-now start then runs every record with
    `dt = 0`, that start's first slice included, a tick before the recording machine ran it. Not
    seen in the three games simulated: their live sequences matched exactly.

### The COB module's entries: every start outside a slice, and the script slice [DISASSEMBLED 2026-10-12]

What the demo recorder's slice markers rest on (`tagpu_recorder.c`, THE SLICE MARKERS;
`tagpu_replay.c`, THE SCRIPT SLICES): outside a slice, a COB object's threads are made only by two
functions, both reached by every start the engine, Impure or a playing makes. Whole-file scans of
`pristine/TotalA.exe.pristine` with `objdump -d`, every `call`/`jmp`/`jcc` by target, and a search of
the file for each entry's address as a dword.

**The thread allocator `0x4B08C0(cob, idx)`** (`thiscall`) has exactly eight callers:

| caller | in | what it is |
|---|---|---|
| `0x4B0896`, `0x4B08AA` | `0x4B0830` | no caller anywhere |
| `0x4B099F` | the start by name `0x4B0940` | `(name, cb, now)`, `ret 0xC` |
| `0x4B0A18` | `0x4B0A10` | no caller anywhere |
| `0x4B0B08` | the start by index `0x4B0B00` | `(idx, cb, now, argc, a0..a3)`, `ret 0x20` |
| `0x4B0C48` | the query `0x4B0C40` | runs the thread at once through the runner at `dt` 0 (`0x4B0CE1`); its one caller is the query by name `0x4B0BC0` (`0x4B0C2F`) |
| `0x4B18BB` | the VM's START | inside a slice (the caller key `0x4B18C0`) |
| `0x4B1928` | the VM's CALL | inside a slice (the caller key `0x4B192D`) |

The start by name with arguments `0x4B0A70` calls `0x4B0B00` (`0x4B0AEE`). A start by name of a
script the COB lacks still reaches the allocator, with -1 (`0x4B0995..0x4B099F`), which refuses it
(`0x4B08C5..0x4B08D0`). No dword in the file holds `0x4B0B00`, `0x4B0940`, `0x4B0A70`, `0x4B08C0`,
`0x4B0BC0`, `0x4B0C40` or `0x4B0D60`.

**Every call into the COB module from outside it** -- targets in `0x4B0610..0x4B2BFF` from
sources outside that range; the constructor `0x4B0610` is the module's first function, and the
functions at `0x4B0000..0x4B060F` are graphics helpers called from `0x4A1BE7..0x4AFF60` that call
no COB entry:

| target | callers | what it is |
|---|---|---|
| `0x4B0610` | `0x485D88` | the constructor |
| `0x4B06F0` | `0x485E33` | the destructor body, in the deleting destructor `0x485E30` |
| `0x4B0720` | `0x485DAC` | the script load onto the object |
| `0x4B07C0` | 9 (`0x4385A2`, `0x43860F`, `0x43A235`, `0x456123`, `0x456213`, `0x48987B`, `0x48992B`, `0x48A12C`, `0x48A2C3`) | name to index |
| `0x4B0940` | 15 (`0x40F433`, `0x41148D`, `0x411794`, `0x411DA1`, `0x411E2B`, `0x43DAF2`, `0x43DB27`, `0x485DE6`, `0x48B106`, `0x48B12B`, `0x48B14E`, `0x48B169`, `0x49CB94`, `0x49CD4F`, `0x49CF73`) | start by name |
| `0x4B0A70` | 21 (`0x406834`, `0x4069BF`, `0x4113EF`, `0x437902`, `0x43795E`, `0x43798A`, `0x43DBE8`, `0x486877`, `0x489898`, `0x489948`, `0x489F43`, `0x489F8E`, `0x48A149`, `0x48A2E0`, `0x499C5C`, `0x49CBEB`, `0x49CDA6`, `0x49CFCA`, `0x49E186`, `0x49E31C`, `0x49E386`) | start by name with arguments |
| `0x4B0B00` | 4 (`0x4385C7`, `0x43862B`, `0x43A251`, `0x455551`) | start by index |
| `0x4B0BC0` | 14 (`0x4027FB`, `0x4113B1`, `0x41189C`, `0x411A35`, `0x411AA6`, `0x411BF5`, `0x411CF1`, `0x43E227`, `0x43E291`, `0x43E32C`, `0x43E370`, `0x43E3E4`, `0x43E427`, `0x4865C3`) | queries |
| `0x4B0D60` | 1 (`0x48ADEB`) | DoScriptsNow |
| `0x4B1EC0` | `0x48773D` | the saved game's save |
| `0x4B2040` | `0x487600` | the saved game's load |
| `0x4B2450` | `0x42D294`, `0x42D8EF` | the script file's load, in the unit-def loader |
| `0x4B2540` | `0x42D275`, `0x42DC3C` | the script file's release, in the unit-def loader |

So, outside a slice, a COB object's threads change only through a start at `0x4B0940` or
`0x4B0B00`, a query (which runs to completion at `dt` 0; the two an order handler makes are
recorded and made again by a playing, *The owner's COB queries* below, and AutoAim's aim queries
are owner-only on a puppet: the `0x4B0C4D` rows of `replay-absent.tsv`), its setup, its free, and
the saved game. Impure's own
starts (`tagpu_weapons.c` through `0x4B0A70`) and a playing's (`tagpu_replay.c`, `E_StartIdx`,
`E_StartName`, `E_StartName0`) enter the same two functions.

**The two start entries' first bytes**, which Impure's observers take (`tagpu_recorder.c`,
`OBSERVE_SITES`): `0x4B0B00` `8B 44 24 04 57` (`mov eax,[esp+4]; push edi`), `0x4B0940`
`51 53 55 56 57` (`push ecx; push ebx; push ebp; push esi; push edi`). Neither span holds a relative
instruction, and nothing branches into bytes 1-4 of either. The observer reads `ecx`, the COB
object the start is made on, as a key, and never dereferences it.

**DoScriptsNow `0x4B0D60(cob, dt)`** (`thiscall`, `ret 4`) runs the thread runner `0x4B0DA0(i, dt)`
over the records when `[cob+0x53C]` is set (the count byte at `0x4B0D81` is the records' bound,
which the raised build sets to `TAGPU_COB_THREADS`), then the piece stepper `0x4B1C00(dt)`. Its
one caller is the unit tick's `0x48ADEB`, with `dt` 1, and `dt` is the only amount a sleep counts
down by (`0x4B0DEF..0x4B0E01`): a unit's COB clock is the number of slices it has had.

**The object's life.** The setup `0x485D40` allocates `0x544` bytes (`0x4B4F10` at `0x485D78`),
constructs (`0x4B0610` at `0x485D88`), writes the vtable `0x4FD698` (`0x485D8D`, the vtable's one
reference in the file), stores the object at `+0x9A` (`0x485D9F`), loads the script (`0x4B0720`
at `0x485DAC`), makes the model (`0x45A950` at `0x485DC0`), binds it (`0x480D40` at `0x485DD2`),
and then starts Create through `0x4B0940("Create" 0x508BE0, 0, run-now 1)` at `0x485DE6`
unconditionally: every new COB object has a start. A type with no script takes `0x485DFE`, which
stores NULL at `+0x9A` (`0x485DFF`). The vtable holds the VM's engine callbacks (`0x480770` ..
`0x481470`) and, at `+0x50`, the deleting destructor `0x485E30`. The unit destructor `0x4866D0`
deletes the object through `call [eax+0x50]` with flag 1 (`0x486D8A`) and stores NULL at `+0x9A`
(`0x486D8D`), both on the game thread in play. `+0x9A` has two more stores in `.text`, `0x4A9FFE`
and `0x4AA88D`, in GUI code beside `+0x96`, of elapsed times from `0x4B6340` [INFERRED: not a unit
record]. So a COB object lives from its unit's setup to its unit's destructor.

**AutoAim's starts**, all in the unit's own turn before its script call (*AutoAim's slot loop*,
below): the target read `0x48A1E0` (`0x49E1E1`) starts TargetCleared through `0x4B0A70` at
`0x48A2E0`; the aim script is started through `0x4B0A70` at `0x49E31C` and `0x49E386` with
callback `&slot+8`, run-now 0 and argc 2, its name from the table at `0x509688` (AimPrimary,
AimSecondary, AimTertiary), and broadcast at `0x456200` (`0x49E3A6`), which the recorder takes at
its hook `0x456218`; the fire function `call [ebx+0x60]` (`0x49E43C`) is one of `0x49C9C0`,
`0x49CDE0` and `0x49CC20`, which start FirePrimary through `0x4B0940` (`0x49CB94`, run-now 0, the
names at `0x509678`: FirePrimary, FireSecondary, FireTertiary) and RockUnit through `0x4B0A70`
(`0x49CBEB`, the name at `0x5096F8`), and record a shot. On a puppet the shot receiver `0x49D270`
calls the same fire functions (`0x49D44E` among others). The wind starts SetDirection and SetSpeed
(`0x437910`, the names at `0x50500C` and `0x505000`, `0x43795E`, `0x43798A`) come earlier in the
same turn, on a puppet too.

**A death's Killed: the owner's query, a peer's run-now start.** `Send_UnitDeath 0x4864B0`, the
death of a unit the machine itself destroys, runs Killed as a query, `0x4B0BC0(cob, "Killed"
0x508BE8, &severity, &corpse, 0, 0)` at `0x4865C3` (`thiscall`, `ret 0x14`; it finds the index by
name and calls the query `0x4B0C40` at `0x4B0C2F`), which allocates one thread (`0x4B0C48`) and
runs that thread alone at `dt` 0 (`0x4B0CE1`); it skips the query for modes 4, 5, 7 and 9 and a
unit whose `+0x108` is above 0 (`0x486525..0x48655C`), and writes the query's severity as the
`0x0C`'s `+9` (`0x4865E9..0x4865F4`): the value after the call, which the query wrote back
(`0x4B0CE6..0x4B0D11`), so it is the value that went in unless the script assigns its first
parameter [INFERRED: no stock Killed seen to]. The corpse slot it passes is a local written only on the
mode-7 path, which skips the query (`0x48652C`, `0x486528`). The destructor `0x4866D0(rec, mode)`
starts Killed itself only in mode 0 and when the record's severity `+9` is above 0
(`0x48684A..0x48685A`): `0x4B0A70("Killed", 0, run-now 1, argc 1, severity, 0, 0, 0)` at
`0x486877`, after which nothing relies on what the call leaves in `eax`, `ecx` or `edx`: the paths
that skip it (`0x486853`, `0x48685A`) join it at `0x48687C` with unrelated values there (the stock
call returns with `ecx` the COB object, its `pop ecx`). A run-now
start runs every record of the object at `dt` 0, so a waiting thread of the dying unit -- an aim
script whose turn the last slice finished -- runs on and starts what it starts before the object
is deleted, which the owner's query does not. A playing applies a recorded death as a peer's,
mode 0, so Impure takes the call at `0x486877` (`tagpu_patches.c`, THE KILLED SCRIPT OF A
REPLAYED DEATH): inside a replayed death the recording machine made itself, the query with the
record's severity, the recording machine's answer -- so a severity of 0 or below there makes no
Killed on the playing (`0x48685A`'s `jle`); everywhere else the stock start.

**The script slice in Impure.** The unit tick's script call (`0x48ADDF..0x48ADF0`, the unit tick's
row) jumps to `pushad; push esi; call unit_slice; add esp,4; popad; jmp 0x48ADF0`, in the
fail-closed table of both builds (`tagpu_patches.c`, THE SCRIPT SLICE). `unit_slice` reads `+0x9A`
as `0x48ADDF` does, asks the recorder for a marker (`tagpu_rec_slice`: on a recording level,
`RK_EV_SLICE` when the COB object had a start at `0x4B0B00` or `0x4B0940` since the capture
point), asks a playing whether it gives this slice at the recording's marker instead
(`tagpu_replay_slice_defer`), and otherwise calls `0x4B0D60(cob, 1)` itself, counted at
`0x48ADEB` with aux 0 (in the unit tick), 1 (at its recorded place) or 2 (at a playing's step's
end).

### The owner's COB queries [DISASSEMBLED 2026-10-12]

What `RK_EV_QUERY` rests on (`tagpu_recorder.c` and `tagpu_replay.c`, THE OWNER'S QUERIES). Of the
fourteen calls of the query `0x4B0BC0` (*The COB module's entries*, above), seven are made only
by an order handler, which a playing's puppets never run, and an eighth is dead (the transport and
pad calls DISASSEMBLED 2026-10-13):

- **`QueryBuildInfo`** (`0x501330`), whose one call is `0x4027FB`, in BuildingBuild `0x402640`'s
  state dispatch (the case at `0x4027D9`, through the table `0x402B5C`): `mov edi,[esp+0x44]`
  (`0x4027D9`) takes the handler's unit, `mov ecx,[edi+0x9A]` (`0x4027E5`) its COB object, `push
  0x501330` (`0x4027EE`) the name, and the piece local is set to -1 (`0x4027F3`) before its
  address is passed as the first argument, the other three 0. The answer feeds the build position
  `0x43E060` at `0x40280B`.
- **`QueryNanoPiece`** (`0x5052D4`), whose one call is `0x43E427` inside `0x43E400(unit, out)`,
  with the piece local set to 0 (`0x43E41F`) and the other three arguments 0 (*The nano spray*,
  above); `0x43E400` has fifteen callers, all order handlers, each passing the spray's source: the
  handler's unit (the order's `+0xE`, or the register holding it), except SelfRepair `0x402430`,
  which passes the repairing pad, the order's target `+0x16` (`mov eax,[edi+0x16]` at `0x402525`,
  the call at `0x40252E`), in the aircraft's turn; the pad can be another player's.
- **`QueryTransport`** (`0x501BB4`), whose one call is `0x4113B1`, in VTOL_Pickup `0x4111B0`'s
  state 2 (`0x41138A`, through the table `0x41153C`): esi is the transport, the handler's first
  argument (`mov esi,[esp+0x28]`, `0x41121A`), `mov ecx,[esi+0x9A]` (`0x4113A5`) its COB object,
  `push 0x501BB4` (`0x4113AC`) the name; the order's `+0x36` is set to -1 (`0x41139F`) and its
  address passed as the first argument, the other three 0. The script writes the link piece into
  the order, which state 4 attaches the passenger at (`0x48AAC0`, `0x4114A6`).
- **`QueryLandingPad`** (`0x501C14`), four calls in VTOL_Landing `0x4118E0` (table `0x411F28`), on
  the pad, the order's target (`mov ebx,[esi+0x16]` at `0x4118F2`, again at `0x411A62`; ebx is not
  written between each `mov ecx,[ebx+0x9A]` and its call), each with four argument pointers to
  values set to -1 first: state 1 `0x411A35` (`0x411A24`, `0x411A30`), and when none of the four
  answers is a free piece (`0x47E570`) again at `0x411AA6` (`0x411A8B`, `0x411A91`); state 3
  `0x411BF5` (`0x411BE9`, `0x411BF0`); state 5 `0x411CF1` (`0x411CE0`, `0x411CEC`), unless the
  order's stored piece `+0x36` is still free. The pad can be an ally's (the default command below
  takes an own or allied target).
- **`0x41189C` is dead.** It is in `0x411840(pad, piece)`, `ret 8`, the same answer-then-test as
  VTOL_Landing's state 1, which has no `call` or `jmp` in the listing and no absolute dword in the
  file.

A player's order reaches VTOL_Landing through the default command: the command-to-action mapper
`0x43F0E0` (`ScriptAction_Type2Index`), command 1 (`0x43F9E9`, the first entry of the table
`0x4401EC`), names `VTOL_LANDING` (`push 0x501B94` at `0x43FB1F`, looked up by name through
`0x438760`) for a flying type (`def+0x241` bit 11) on an own or allied target whose def has bit 9,
an air base, once the reclaim, help-build and repair tests before it have not matched. MEASURED
2026-10-13 by the `transport-pad` and `stock-transport-pad` test groups, two runs each: an Atlas
selected with a left click and a right click on an ARMASP at Interface Type 1 lands on it
through VTOL_Landing, which queries the pad once at `0x411A35` (state 1) and once at `0x411BF5`
(state 3), the Atlas ending at the pad's position; a first answer with a free piece and a piece
still free in state 5 leave `0x411AA6` and `0x411CF1` unreached there.

**Teleport, order type 11** [DISASSEMBLED]. The mapper's table `0x4401EC` sends type 11 to
`0x43F813`, which names `TELEPORT` (`push 0x505324`, `0x43F817`) through `0x438760` with no test of
the unit, its def or a target; type 10 goes to `0x43F82C`, `STOP` (`0x502714`). `TELEPORT` is entry
12 of the 22-entry action table `0x4FC6E8` (stride 25; `0x406BF0` passes it and its count to
`0x43BC90`, which registers it [INFERRED]): name `Teleport`, gerund `Teleporting`, handler
`0x406AA0`, second handler `0x439740`. The handler,
`ret 0xC`, its unit the first argument and the order the second, forms the box `def+0x15E..+0x172`
about the unit's position `+0x6A/+0x6E/+0x72` (x from `+0x15E` to `+0x16A`, altitude `+0x162` to
`+0x16E`, z `+0x166` to `+0x172`), walks the unit array `main+0x14357..+0x1435B` inclusive with no
alive test, and for each other unit inside the box calls the flare `0x471FD0(&pos, &target, 0x1E,
5)` (`0x406B9F`) and set-position `0x48A9F0(unit, x, alt, z, 1)` (`0x406BC0`), the target being the
order's position `+0x22/+0x26/+0x2A` plus the unit's offset from the teleporter; it returns 5.
The handler table is its only reference and no stock button issues type 11
(`deep-total-mayhem.md`); a scenario's `teleport` order does (`scenario-format.md`, *Order
vocabulary*). MEASURED 2026-10-23 by the `teleport` test group: an
ARMLAB ordered `teleport` three times moved the four ARMPW 16 wu either side of its centre by the
order's position minus its own in the next step, and the counts named the flare live 8, puppet 0
over [301, 751) (`sink 0x471FD0 from 0x406BA4`).

`0x4B0BC0` reads the object at once (`mov eax,[ecx+8]`, `0x4B0BC1`), so a unit with no script
(`+0x9A` NULL, `0x485DFF`) never reaches either call in a running game. It finds the script by
name (`0x4B0BDE..0x4B0C11`; a missing one gives -1) and calls the query `0x4B0C40` (`0x4B0C2F`),
which allocates a thread (`0x4B0C48`, the count key `0x4B08C0` from `0x4B0C4D`), reads each
non-NULL argument pointer in (`0x4B0C5C..0x4B0CCA`), runs that thread alone at `dt` 0
(`0x4B0DA0` at `0x4B0CE1`) and writes the four values back to the non-NULL pointers
(`0x4B0CE6..0x4B0D11`): the script can change the object's statics and pieces.

**Where they run.** The handlers are entered only through the three dispatch calls `0x43A21A`,
`0x43B87C` and `0x43BB21` (*The nano spray*). `0x43B87C` is in the order run `0x43B7C0` and
`0x43BB21` in `0x43BAD0`, whose only callers are the unit tick's `0x48AF98` and `0x48AF9E`, in the
unit's turn after its script slice (`0x48ADEB`) and behind the gate that its owner be a local
seat of type 1 or 2 (`0x48AEB5..0x48AECD`); no dword in the file holds either address.
`0x43A21A` is the destructor `0x43A1F0`'s call, with event 2, made only when the order's `+6` has
bit `0x02`: BuildingBuild returns on it before its state dispatch (`0x402647..0x40264D`);
VTOL_Pickup sets the bit only at the end of state 3 (`0x411464`), so event 2 finds it in state 4,
where it starts `EndTransport` (`0x41148D`) and never reaches its query; VTOL_Landing never sets
it (its stores of `+6` are `= 0xE8` at `0x411B75` and `0x411C80`, `|= 0xE0` at `0x4119E2` and
`|= 0xE8` at `0x411DBE`, none with bit `0x02`). So each query is made in the handler's unit's
own turn, after that unit's slice, inside no death and no feature call, on the machine of that
unit's owner. The queried unit is that unit, except a pad — SelfRepair's `QueryNanoPiece` and
VTOL_Landing's `QueryLandingPad`: their query can come before the pad's own slice in the same
tick, and on a machine that does not own the pad. The slice markers' start set notes every
query's COB, which is what puts the pad's later slice after the query on a playing.

**In Impure.** A recording carries each as `RK_EV_QUERY {ID, the query}` where the engine makes
it: a stub on each of the six calls `0x4027FB`, `0x4113B1`, `0x411A35`, `0x411AA6`, `0x411BF5` and
`0x411CF1` (`QUERY_SITES`), built like the owner-only starts' (the `call`, the `mov` and the
name's `push` compared first; the unit's register read under the stub's `pushad`, its `+0x9A`
checked against `ecx`), and the existing observer at `0x43E400`'s entry, whose unit is its first
argument. The query's COB object is noted in the slice markers' start set as a start's is. A
playing makes the same `0x4B0BC0` there, on the `+0x9A` of the unit live in the ID's slot, with
the values the engine's call puts there first — -1, 0 or -1 in the first argument and NULL for the
rest, or four -1 for `QueryLandingPad`; what the caller does with the answers is not made, since
it writes only the handler's buffer or the order, and what follows on the recording machine (the
spray, the attach, the landing) is recorded. The other seven calls are not recorded: the Killed
query `0x4865C3` (*A death's Killed*, above); AutoAim's aim and `SweetSpot` queries
`0x43E227`..`0x43E3E4`, owner-only on a puppet and accepted as such by the owner
(`replay-absent.tsv`); and the dead `0x41189C`.

## The COB engine — mapped by us (tacob landing 2, 2026-09-07)

*Everything here is from `i686-w64-mingw32-objdump -d -M intel` of `pristine/TotalA.exe.pristine`
unless marked **[LIVE]** or explicitly attributed to a mod executable. The fork's oracle
`tagpu_cobtrace.c` observes the five event sites plus both destructor entries and writes no
engine data; its line contract is [tacob-design](../tacob-design.html) §"The
trace contract", and `extra-weapons.md` snag 10 is the story that led here. Names marked
`[INFERRED]` are ours; `COBEngine_*` names come from the community symbol file.*

### The object — `unit+0x9A`, 0x544 bytes, vtable `0x4FD698`

Built by `UNITS_CreateModelScripts 0x485D40` (`stdcall(unit)`, one caller each from the two
create paths): when the def's script pointer `def+0x18E` is non-null it `MEM_Alloc(0x544)`s
(`0x4B4F10`), runs the base constructor `0x4B0610` (vtable `0x4FDB00`; zeroes `+8`, `+0x10`,
`+0x14`, the eight record status words and `+0x53C`; **`+4 = 0x4B6330()` = `[[0x51FBD0]+0xE8]`**,
the sim rate the `sleep` conversion below divides by — 30 `[INFERRED from that use]`), sets the
vtable to `0x4FD698`, stores the object at `unit+0x9A`, then `0x4B0720(cob, scriptfile)`
attaches the loaded `.cob` at `+8` and allocates `+0x14` (`npieces × 19` dwords, tag string
`0x509C84` — the per-piece animation state MOVE/TURN/SPIN drive) and `+0x10` (`nstatics × 4`,
tag `0x509C74` — the static variables), `0x45A950(model, scriptfile, unit)` builds the posed
model (`unit+0x9E`), `0x480D40(cob, o3)` stores that at **`cob+0x540`**, and finally
`0x4B0940(cob, "Create" @0x508BE0, callback 0, run-now 1)`. A unit without a script (`0x485DFE`)
gets `unit+0x9A = 0`, its model from `0x45A8D0`, and `o3+0xC = unit` written directly at
`0x485E14` — so **the unit behind a COB object is `*(*(cob+0x540)+0xC)`**, the path every
vtable handler takes (`0x480770`, `0x480C30`, `0x480EB0` all open with it).

| Offset | Field | Established |
|---|---|---|
| `+0x00` | vtable (`0x4FDB00` base, `0x4FD698` the unit script class) | ctor `0x4B061A`, `0x485D8D` |
| `+0x04` | sim rate (30) — `sleep` ticks = `ms × rate / 1000`, MOVE/TURN speeds `/ rate` | `0x4B0641`; `0x4B1363..0x4B1370`; `0x4B0EDE`, `0x4B0F7B` |
| `+0x08` | the loaded `.cob`: `+4` script count, `+8` piece count, `+0x10` static count, `+0x18` entry table (word indices), `+0x1C` name pointers, `+0x24` code words — the on-disk header (`file-formats.md` §2.1) with the offsets relocated to pointers | `0x4B072A`, `0x4B08CA`, `0x4B0900`, `0x4B07D0`, `0x4B0E5D`, `0x4B073B`, `0x4B0759` |
| `+0x0C` | `0x4B26F0(scriptfile)` result `[unknown]` | `0x4B0735` |
| `+0x10` | static variables, `count × 4`, zero-filled | `0x4B076E`; read by `PUSH_STATIC` `0x4B13AA` |
| `+0x14` | piece animation array, `19` dwords per piece, zero-filled | `0x4B0756`, `0x4B0777..0x4B0791` |
| `+0x18` | "something is animating" — set by MOVE/TURN, cleared by the stepper | `0x4B0F1A`, `0x4B1C21` |
| `+0x1C` | **eight thread records × `0xA4`** (below) | `0x4B08D4..0x4B08E6` |
| `+0x53C` | running-thread count | `0x4B0921`, `0x4B19F9`, `0x4B1AAB` |
| `+0x540` | the posed model `Object3do` (`unit+0x9E`); `+0xC` of it is the unit | `0x480D44`, `0x480EBB..0x480EC1` |

**Vtable `0x4FD698`** (22 slots; the base `0x4FDB00` has slots 0–6 = `0x4E6110` (pure), 7–13 =
`0x4B1E50..0x4B1EB0`, 14–20 = `0x4B0650..0x4B06B0`, the no-op bases, and `0x4B06B0` is the base
destructor: frees `+0x14`, `+0x10`, then the object when the flag argument has bit 0). The
slots the VM calls, with the handler each opcode reaches — this closes the "unidentified opcode
reaching the effect handlers" gap: **`EMIT_SFX` → `vt+0x30` = `0x480EB0`, `EXPLODE` → `vt+0x34`
= `0x481140`**, `ATTACH` → `vt+0x38` = `0x481340`, `DROP` → `vt+0x3C` = `0x4813B0`, `SET` →
`vt+0x40` = `0x480B20`, `GET_UNIT_VALUE` → `vt+0x44` = `0x480770` (a 20-entry jump table at
`0x480AC4` on `id-1`, so value ids run 1..20), `vt+0x14` = `0x480C30` = a piece's current
position, `o3 + 0x22 + piece × 0x36 + axis × 4 + 4` (the `PrimitiveStruct` stride), read by
MOVE/TURN for the sign of the travel; `vt+0x50` = `0x485E30` `FreeUnitScriptData`, the deleting
destructor path (`call [vt+0x50]` at `0x486D8A`). Only the handlers named with an offset were
read; the other slot addresses are from the table dump.

**The "COB thread handle" the weapon slots preset to `0x4FD6F0` is a vtable pointer, not a
thread.** `0x4FD6F0` holds two slots: `0x481490` `thiscall(this = &slot->thread, value)` —
`if (value) *(this+4) = 1`, i.e. **the slot's `+0x08` aim result** — and `0x4814B0` (returns 0).
A thread record's `+0x20` points at that `+0x04` field; when the thread's `RETURN` runs, the
engine calls `(*cb)->slot0(cb, value)` (`0x4B19E2..0x4B19E5`), and when a start is *refused* on
a full pool it calls the same with `0` at once (`0x4B0B11..0x4B0B1D`) — so a refused
`AimPrimary` reports "not aimed" immediately, which is why the stock loop retries it.

### The loaded script — `0x4B2450` `[DISASSEMBLED 2026-09-25]`

`stdcall(path)`, `ret 4`, called at `0x42D8EF` (the level's load) and `0x42D294` (the one-type
reload). It reads the file whole (`0x4BBE50`), checksums it (`0x4B6BA0` over the size
`0x4BBC40` gives), enters it in a map at `0x51FBC0` through `0x4B2850` with the checksum at the
entry's `+0x10` `[role INFERRED]`, and then **relocates the header in place**
(`0x4B24A7..0x4B2527`): each offset becomes a pointer, and so does every entry of the two name
tables:

| offset | field | relocated |
|---|---|---|
| `+0x00` | version, 4 | — |
| `+0x04` / `+0x08` | script count / piece count | — |
| `+0x0C` | code length, in dwords | — |
| `+0x10` | static-variable count | — |
| `+0x14` | the count of the `+0x28` table's 8-byte entries | — |
| `+0x18` | the entry-point table: one dword a script, **a code offset in dwords** | the table pointer only |
| `+0x1C` | script names, `char*` a script | the pointer **and every entry** |
| `+0x20` | piece names, `char*` a piece | the pointer **and every entry** |
| `+0x24` | the code, dwords | the pointer |
| `+0x28` | 8-byte entries, the second dword a name `[INFERRED: the sound table]` | the pointer and each entry's second dword |

**Nothing checks a count or an offset**, so every one of them is file data: a reader that
follows them bounds each first (`tagpu_datakeys.c`'s `create_hides`). The script is freed by
`0x4B2540` and lives from the level's load to its teardown (`0x42DC3C`, zeroed at `0x42DC41`),
or to a console `Reload` of its type.

**The block's length is the checksum's.** The loaded script does not record its size; the
allocator does ([`MEM_Size 0x4D8360`](memory.md#0x4d8360-mem_size-the-allocator-knows-a-blocks-size)),
but that is the heap block's size, which this reading does not use. But
`0x4BBE50(path, 0)` sizes the block from the file's archive entry (`[[h+8]+4]`) or its
`filelength` (`0x4E79D0`), and `0x4BBC40(path)` answers the same lookup the same way
(`0x4BB2E0` with the same mode `0x505F10`, `0x4BBC5A..0x4BBC7E` against `0x4BBE9A..0x4BBEBC`);
`0x4B6BA0(buf, size)` — a four-byte checksum, `stdcall`, `ret 8` (`0x4B6C1E`), that reads exactly
`size` bytes (`0x4B6BC2..0x4B6BF0`) — is called with that pair at `0x4B2475..0x4B2477` and
returns to `0x4B247C`. So the arguments of that one call are the block and its length, and the
engine itself reads every byte of it there. They are two opens of one file, so they agree while
the game's files hold still: a loose script rewritten by another program between `0x4BBE50` and
`0x4BBC40` would make the engine's own checksum read past its block first. `0x4B6BA0` has eleven callers; `tagpu_datakeys.c`
observes its entry and keeps the pair only for the call that returns to `0x4B247C`, which is how
the ghost mask bounds every read of a script by its own block.

**The model template has no such length.** `0x4CB560(path)` reads the `.3do` whole through the
same `0x4BBE50` but passes no size out (`push 0`, `0x4CB565`), relocates it in place
(`0x4CB4C0`: the name `+0x1C`, `+0x20`, the vertices `+0x24`, the faces `+0x28`, the sibling
`+0x2C` and the child `+0x30`, recursively, and each face's pointers), and returns the block,
which is the root node itself: `MODEL_PTRS[type]` is the file's first byte. So a reader of the
tree (the pose bake, the native pass, the ghost's walk and its name compares) has the type bound
and the level lifetime, and no length to bound a node's pointers against.

### The eight records — `cob+0x1C + slot × 0xA4`

| Offset | Field | Established |
|---|---|---|
| `+0x00` | status: `0` free; `0x01000000` running; `0x02100000` wait-for-turn; `0x02200000` wait-for-move; `0x02400000` sleeping; `0x02800000` blocked in a `call-script`. The runner keys on the top byte, then bits 20–23 | alloc `0x4B08F9`; `0x4B12F1`, `0x4B132E`, `0x4B134D`, `0x4B196E`; dispatch `0x4B0DBF..0x4B0DED` |
| `+0x04` | pc, a word index into the code; kept current in memory (`0x4B1BD9`), the handlers read it back (`0x4B0E60`) | |
| `+0x08` | stack top index, `-1` empty | `0x4B090C`; every push/pop |
| `+0x0C` | sleep ticks left, `-= dt` per run, wakes at `<= 0` | `0x4B1363..`, `0x4B0DEF..0x4B0DFF` |
| `+0x10` / `+0x14` | piece and axis a wait blocks on | `0x4B12F7..0x4B12FA`, `0x4B0E1E..` |
| `+0x18` | the child slot a `call-script` waits on — **`-1` when the child was refused** | `0x4B1965` |
| `+0x1C` | signal mask; `1` at alloc, inherited from the parent by START/CALL | `0x4B091A`, `0x4B18F4`, `0x4B1961`, `0x4B1B14` |
| `+0x20` | completion callback object pointer (above), `0` at alloc | `0x4B0913`, `0x4B0B37`, `0x4B0C69` |
| `+0x24` | the stack, 32 words to the end of the record. **`PUSH` has no bound**: `0x4B13CF..0x4B13D9` increments `+0x08` and stores, so a thread that holds more than 32 words writes into the next record, and past the eighth into the COB object. Stock content peaks at 11 words; three Escalation scripts need 54 to 85 ([merge exploration §F](../tadr-merge-exploration.md#f-the-cob-getters)) [DISASSEMBLED 2026-09-25] | every push |

**`COBEngine_AllocThread 0x4B08C0`** — `thiscall(cob, scriptIndex)` → slot 0..7 or `-1`: rejects an
index outside `0..nscripts-1` (`0x4B08C5..0x4B08D0`) — so **the engine asking for a script the
unit does not define reaches here as `-1` and is indistinguishable from a full pool by return
value**; scans the eight status words for `0`, and on the first free one writes status
`0x1000000`, pc = `entry[idx]`, sp `-1`, callback `0`, mask `1`, and `+0x53C++`. It is the **single
funnel every thread start takes**: callers `0x4B0896`/`0x4B08AA` (in `0x4B0830`, a by-name
allocate-only entry — **no callers**), `0x4B099F` (`0x4B0940`), `0x4B0A18` (`0x4B0A10`, by-index
no-args — **no callers**), `0x4B0B08` (`0x4B0B00`), `0x4B0C48` (`0x4B0C40`), `0x4B18BB` (the
START opcode), `0x4B1928` (the CALL opcode). **Every caller pushes the arguments onto the new
record only after it returns**, which is why the oracle latches the start here and writes the
line at the next hook event.

**The engine's entries** (the by-name ones inline the same two-byte-at-a-time `strcmp` walk over
the name table; `0x4B07C0 Name2Index` is the standalone copy):

| VA | Convention | What | Callers (`E8` scan) |
|---|---|---|---|
| `0x4B0A70` `COBEngine_StartScript` | `thiscall(cob, name, cb, runNow, argc, a0, a1, a2, a3)`, `ret 0x20` | name → index → `0x4B0B00` | 21: `0x406834` `0x4069BF` `0x4113EF` `0x437902` `0x43795E` `0x43798A` `0x43DBE8` `0x486877` `0x489898` `0x489948` `0x489F43` `0x489F8E` `0x48A149` `0x48A2E0` `0x499C5C` `0x49CBEB` `0x49CDA6` `0x49CFCA` `0x49E186` (`UNITS_StartWeaponsScripts`) `0x49E31C` `0x49E386` (`AutoAim`) |
| `0x4B0B00` | `thiscall(cob, idx, cb, runNow, argc, a0..a3)`, `ret 0x20` | alloc; refused → `cb->slot0(0)` and return 0; else `+0x20 = cb`, `a0..a3` into `stack[0..3]`, **sp = argc−1** (`0x4B0B76..0x4B0B7B`); `runNow` → run all eight records with `dt = 0` then the stepper `0x4B1C00(cob, 0)`; returns 1 | `0x4385C7` `0x43862B` `0x43A251` (`ORDERS_CancelOrder+0x61`) `0x455551` (the `0x10` message's case `0x4554EE..0x455556`, dispatch entry `0x455FBC`, which passes the message's script index) `0x4B0AEE` |
| `0x4B0940` | `thiscall(cob, name, cb, runNow)`, `ret 0xC` | no-argument start, same shape; refused → returns 0 *without* calling the callback | 15: `0x40F433` `0x41148D` `0x411794` `0x411DA1` `0x411E2B` `0x43DAF2` `0x43DB27` `0x485DE6` (`Create`) `0x48B106` `0x48B12B` `0x48B14E` `0x48B169` (`UNITS_SetStateMask`) `0x49CB94` `0x49CD4F` `0x49CF73` (the fire paths) |
| `0x4B0BC0` `COBEngine_QueryScript` | `thiscall(cob, name, p0, p1, p2, p3)`, `ret 0x14` | → `0x4B0C40` | 14: `0x4027FB` `0x4113B1` `0x41189C` `0x411A35` `0x411AA6` `0x411BF5` `0x411CF1` `0x43E227` `0x43E291` (`UNITS_QueryWeaponPosition`) `0x43E32C` `0x43E370` `0x43E3E4` `0x43E427` (`0x43E400`, `QueryNanoPiece`) `0x4865C3` |
| `0x4B0C40` | `thiscall(cob, idx, p0..p3)` | alloc; **refused → returns 0 leaving `*p0..*p3` untouched** (the silent failure); else callback `0`, pushes `*p0..*p3` (`0` for a null pointer), **sp = 3**, runs that thread now (`0x4B0DA0(cob, slot, 0)`), then copies `stack[0..3]` back through the non-null pointers — a `Query*` script answers by assigning its parameter | `0x4B0C2F` |
| `0x4B0D60` `COBEngine_DoScriptsNow` | `thiscall(cob, dt)` | runs the eight records, then `0x4B1C00(cob, dt)` | **one**: `0x48ADEB`, in the per-unit tick function, immediately after `AutoAim 0x49E1A0` (called when the block owner's player record, `edi` from `0x48AD73`, is active with `+0x73 ∈ {1, 2}`, `0x48ADC9..0x48ADD7`), **with `dt = 1`** — so `+0x0C` counts ticks |
| `0x4B0D20` | `thiscall(cob, cb)` | clears a matching callback pointer in every busy record | **no callers** |
| `0x4B1C00` | `thiscall(cob, dt)` | the animation stepper: returns at once when `dt == 0` or `cob+0x18 == 0`; else walks `+0x14` and advances every MOVE/TURN/SPIN | `0x4B09E8` `0x4B0A5F` `0x4B0BA6` `0x4B0D88` |

### The runner `0x4B0DA0` and the opcode handlers

`thiscall(cob, slot, dt)`, `ret 8`. Frame: `sub esp,0x20` + four pushes, so `[esp+0x34]` is
`slot` and `[esp+0x38]` the "keep running" flag; `edi` = cob, `esi` = the record, `ebp` = slot
(reloaded at every loop head `0x4B0E59`), `ecx` = pc. Status dispatch at `0x4B0DBF..0x4B0E3D`:
sleeping subtracts `dt` and wakes at `<= 0`; the two waits test the piece animation array; a
thread blocked in a call is not touched here — **only the child's `RETURN` (or a `signal` that
kills the child) wakes it**; a running thread then executes opcodes back to back until one
blocks it or ends it. The dispatch is a compare chain on `op & 0x100FF000` (a `cmp edx, …` per
value; the handler follows each compare):

| Opcode | `cmp` at | What the handler does (the facts the VM needs) |
|---|---|---|
| MOVE `0x10001000` / TURN `0x10002000` | `0x4B0E83` / `0x4B0E71` | piece and axis inline; pops target then speed; speed `/ (cob+4)` per tick; `vt+0x14` for the current value (sign of travel); sets `cob+0x18` |
| SPIN `…3000` / STOP_SPIN `…4000` | `0x4B0FFE` / `0x4B0FEC` | |
| SHOW/HIDE/CACHE/DONT_CACHE/MOVE_NOW/TURN_NOW/SHADE/DONT_SHADE | `0x4B10DD` `0x4B10D3` `0x4B1112` `0x4B1108` `0x4B11AA` `0x4B119C` `0x4B127C` `0x4B1272` | |
| EMIT_SFX `0x1000F000` | `0x4B12B1` | pops type; piece inline; `vt+0x30(piece, type)` → `0x480EB0`; pc += 2 |
| WAIT_TURN `0x10011000` / WAIT_MOVE `…12000` | `0x4B12A7` / `0x4B1317` | piece, axis inline into `+0x10`/`+0x14`; status `0x2100000` / `0x2200000`; stop |
| SLEEP `0x10013000` | `0x4B130D` | pops ms; **`+0x0C = ms × (cob+4) / 1000`** (the `0x10624DD3` magic, truncating: `sleep 150` = 4 ticks); status `0x2400000`; stop |
| PUSH_* `0x10021xxx` | `0x4B1390` | `op & 7`: 1 constant inline, 2 `stack[inline]` (a local), 4 `statics[inline]` |
| CREATE_LOCAL_VAR `0x10022000` | `0x4B1386` | **`sp++` and nothing written** — a local the caller did not pass reads whatever the record last held there; nothing zeroes a record between uses |
| POP_* `0x10023xxx` / POP_STACK | `0x4B1402` / `0x4B13F8` | |
| ADD SUB MUL DIV/MOD AND OR XOR NOT | `0x4B1483` `0x4B1479` `0x4B14D3` `0x4B14C9` `0x4B1525` `0x4B151B` `0x4B1575` `0x4B156B` | |
| RAND `0x10041000` | `0x4B15BD` | pops hi, lo; **`call 0x4B6C30(hi − lo + 1)` at `0x4B15E0`** — the sim RNG (stdcall; returns 0 for `n < 2`; state at `0x51FC88`; 129 call sites across the sim), pushes `lo + result` |
| GET_UNIT_VALUE `…42000` / GET `…43000` / `…44000` / `…45000` | `0x4B15B3` `0x4B1630` `0x4B1622` `0x4B16B8` | `vt+0x44(id, 0, 0, 0, 0)` for the first |
| SET_LESS … LOGICAL_NOT | `0x4B16AE` `0x4B1707` `0x4B16FD` `0x4B1761` `0x4B1757` `0x4B17BB` `0x4B17B1` `0x4B181D` `0x4B1813` `0x4B188A` | |
| START `0x10061000` | `0x4B1880` (body `0x4B18B0`) | inline `[pc+1]` script index, `[pc+2]` argc; alloc; **refused (`0x4B18C2`) → the argc words stay on the parent's stack** and pc += 3; else pops them into `child.stack[0..argc−1]` in push order **without setting the child's sp** (it stays `−1`; the child's `CREATE_LOCAL_VAR`s climb over them), `child.mask = parent.mask`; pc += 3 |
| CALL `0x10062000` | `0x4B1911` (body `0x4B191D`) | as START, then `parent+0x18 = child slot`, parent status `0x2800000`, stop — **the child slot is written even when it is `−1`, and nothing ever wakes a thread waiting on `−1`: a `call-script` on a full pool leaves the caller dormant until it is killed** |
| `0x10063000` | `0x4B1903` (body `0x4B1984`) | pops `[pc+2]` words into the runner's own frame `[esp+0x20..]`, pc += 3 `[unknown use]` |
| JUMP `0x10064000` | `0x4B19BB` | pc = inline |
| RETURN `0x10065000` | `0x4B19B1` (body `0x4B19D0`) | if `+0x20`: pop → `cb->slot0(value)`; else the value stays on the stack; status `0`, `+0x53C--`; every record with status `0x2800000` and `+0x18 == this slot` → running; stop |
| JUMP_NOT_EQUAL `0x10066000` | `0x4B1A40` | pops; jumps to the inline target when the value is **zero** |
| SIGNAL `0x10067000` | `0x4B1A32` (body `0x4B1A75`) | pops mask; for each busy record with `+0x1C & mask`: status `0` (`0x4B1A99`), count--, its blocked callers woken, and if it is the running thread, stop — **no callback is called for a killed thread**, so an aim script killed by the next `AimPrimary`'s signal never reports |
| SET_SIGNAL_MASK `0x10068000` | `0x4B1B00` | `+0x1C = pop` |
| EXPLODE `0x10071000` | `0x4B1AF6` (body `0x4B1B1F`) | pops flags; piece inline; `vt+0x34(piece, flags)` → `0x481140` |
| SET `0x10082000` / ATTACH `…83000` / DROP `…84000` | `0x4B1B48` `0x4B1B50` `0x4B1B58` | `vt+0x40` (2 pops) / `vt+0x38` (3 pops) / `vt+0x3C` (1 pop) |
| anything else | `0x4B1B60` | **the thread is killed silently** (status `0`, count--) |

The tail: `0x4B1BD5` pc++, `0x4B1BD9` writes pc back, `0x4B1BDC` loops while the flag is set,
`0x4B1BE8` unwinds.

### Exhausted CALL is a dormant thread, not an invalid address (O15)

[DISASSEMBLED 2026-10-03] `0x4B08C0` returns `-1` for an invalid script index
(`0x4B08C5..0x4B08D0`, `0x4B092C`) or eight occupied records
(`0x4B08D7..0x4B08ED`). The guarded opcode validates its script index first,
so a refused VM CALL is the latter. `0x4B192F` skips argument copying on refusal:
the parent's stack and SP stay intact. `0x4B1965..0x4B197F` writes child `-1`,
advances PC by three, sets status `0x02800000` and stops this thread's execution.

The status dispatch `0x4B0DBF..0x4B0E49` leaves a CALL wait dormant. RETURN's
wake loop `0x4B19F4..0x4B1A23` visits exactly eight records and compares each
waiting record's child value with the returning slot at `0x4B1A12`; it never
uses the child as an address or index. SIGNAL can kill the waiting record by
its signal mask. No returning slot equals `-1`, so the refused call cannot
resume by accidentally borrowing another child's completion. The native
per-unit runner `0x4B0D60` continues visiting the other seven records.

Impure preserves that state. `tagpu_cob_check_record` accepts child `-1` or
a slot below the runtime capacity; save readers also bound it by the saved capacity.
Other negative values and out-of-range slots remain faults. A refused START
still consumes arguments in the guarded adapter to bound repeated failed starts.
The sentinel is safe independently of capacity; the CORE constructor handoff below
establishes a separate need for more than the native eight slots.

[MEASURED 2026-10-03] The full-pool fixture held eight busy records on each of
human unit 2 and local-AI unit 1002. Their refused CALLs retained 128 arguments
(all 123), SP 127, PC 286, status `0x02800000`, child `-1`; the game advanced
48 more ticks. The synthetic portable check accepts this sentinel through
expanded save/load and native 32-word import, and rejects child `-2`, `8` and
`2147483647`. This does not make a refused child execute or preserve COB history
across a replay seek. See [COB compatibility](../tadr-port/cob.html#o15-exhausted-call-and-failed-worker-cleanup).

### The piece animation array `cob+0x14` and the stepper `0x4B1C00` (tacob landing 3)

`0x4B0756` allocates `npieces × 19` dwords, zero-filled, tag `0x509C84`. **Nineteen dwords
per piece, six three-axis groups and a flag**, and the axis operand of MOVE/TURN indexes
each group directly (`slot = piece × 19 + axis`):

| Dword | Field | Written by | Read by |
|---|---|---|---|
| `+0` | "this piece is animating" | MOVE/TURN/SPIN (`0x4B0F13`, `0x4B106D`), the stepper re-sets it while any axis is still travelling | the stepper's per-piece skip (`0x4B1C4F`) |
| `+1..3` | MOVE target, 16.16 | MOVE (`0x4B0EC3`), MOVE_NOW (`0x4B11E2`) | the stepper's arrival test |
| `+4..6` | MOVE speed **per tick** = `speed / (cob+4)`, signed | MOVE (`0x4B0EE4`), zeroed by MOVE_NOW and on arrival | **`wait-for-move` releases when this is 0** (`0x4B0E03..0x4B0E1A`) |
| `+7..9` | TURN target, `& 0xFFFF`; **`-1` means "no target"** — what SPIN writes (`0x4B101D`) so the stepper never arrives | TURN (`0x4B0F5A`), TURN_NOW (`0x4B123F`), SPIN | the stepper |
| `+10..12` | TURN speed **per tick**, signed | TURN (`0x4B0F85`), SPIN when the acceleration is 0 (`0x4B1060`), the stepper's acceleration step | **`wait-for-turn` releases when this is 0** (`0x4B0E1E..0x4B0E35`) |
| `+13..15` | SPIN target speed per tick | SPIN (`0x4B1037`), STOP_SPIN writes 0 (`0x4B109B`) | the stepper's acceleration step |
| `+16..18` | SPIN acceleration per tick | SPIN (`0x4B104D`), STOP_SPIN writes **`-decel/rate`** (`0x4B10B3`), TURN and TURN_NOW clear it | the stepper |

**The four movement opcodes, exactly** (all divide by `cob+4` with `idiv`, so the per-tick step
truncates toward zero — `<90>` = 16384 becomes 546, `<50>` = 9102 becomes 303):

- **MOVE `0x4B0E8F`** pops the target then the speed, stores both, calls `vt+0x14(piece, axis)`
  for the piece's current position and **negates the speed when the target is below it**
  (`0x4B0EFB`). It does not test for "already there".
- **TURN `0x4B0F2C`** masks the target to 16 bits, clears the axis's spin acceleration, pops the
  speed, then calls `vt+0x18(piece, axis)` for the current angle. `delta == 0` → the speed is
  written as 0 (the wait releases on the next tick anyway); otherwise the speed is negated when
  `(|delta| > 0x8000) XOR (delta < 0)` (`0x4B0FBA..0x4B0FE7`) — **`turn` always takes the short
  way round**, and that is the whole rule; there is no "shortest arc" flag anywhere.
- **SPIN `0x4B100A`** pops the speed then the acceleration (BOS pushes them the other way), and
  with a zero acceleration puts the piece at its target speed at once.
- **STOP_SPIN `0x4B1086`** pops the deceleration, negates it into the acceleration field, and
  with a zero deceleration stops the axis dead. It is the one movement opcode that sets neither
  the piece's flag nor `cob+0x18` (`0x4B10BE` jumps to `0x4B18F8`, the bare `pc += 3` tail) —
  harmless only because the axis it stops was already spinning and so already flagged.
- **MOVE_NOW `0x4B11B6` / TURN_NOW `0x4B1210`** write the value through `vt+0x00` / `vt+0x04`,
  zero the axis's speed, and flag nothing.

**The stepper `0x4B1C00`** `thiscall(cob, dt)`, called at the end of `DoScriptsNow` with the
tick's `dt` and by every run-now start with `dt = 0`. It returns immediately on `dt == 0` or
`cob+0x18 == 0`; otherwise it clears `cob+0x18`, and for each piece whose flag is set: clears
the flag, walks the three axes, and re-sets the flag (and `cob+0x18`) if any axis is still
travelling. Per axis, in this order:

1. **Move**: `new = vt+0x14(piece, axis) + dt × speed`; arrived when `new >= target` for a
   positive speed or `new <= target` for a negative one, and arriving snaps to the target and
   zeroes the speed (`0x4B1CA1`, `0x4B1CB3`). The new value goes through `vt+0x00` either way.
2. **Spin acceleration**: `turnspeed += accel`, clamped to the spin target speed, and the
   acceleration is zeroed on arrival (`0x4B1D08`).
3. **Turn**: with the *post-acceleration* speed, `remaining = (target − cur + 0x10000) & 0xFFFF`
   for a positive speed and `(cur − target + 0x10000) & 0xFFFF` for a negative one; the axis
   arrives when `remaining <= |dt × speed|`, snapping to the target and zeroing the speed
   (`0x4B1D78`, `0x4B1DB1`). A target of `-1` (a spin) never arrives. The angle goes through
   `vt+0x04` masked to 16 bits.

So **the wait opcodes release on the tick *after* the stepper zeroes the speed**: the runner
runs the eight records first and the stepper last, so a thread blocked on an axis that arrives
during tick *N* resumes at tick *N+1*. Measured end to end on ARMSTUMP's `AimPrimary(33, 1066)`:
started at tick 118, turret (speed 546, 33 to go) arrives in the tick-118 stepper, barrel
(speed −303, 1066 to go) takes four, and the script's `start-script RestoreAfterDelay` and
`return (1)` land at tick 122 — which is what the fixture logged.

### The by-name starts: every call site, its entry and its `runNow` (tacob landing 3)

Read off each caller listed above; the name is the string it pushes, and the argument order for
`0x4B0A70` is `name, cb, runNow, argc, a0..a3` (pushed in reverse). **`0x4B0B00` writes
`a0..a3` into `stack[0..3]` whatever `argc` says and only then sets `sp = argc−1`**
(`0x4B0B37..0x4B0B7B`), so an engine start does *not* leave the record's stale words under the
arguments it did not pass — a script start (the START opcode `0x4B18EB` reads the parent's mask
and copies only `argc` words) does. Its run-now tail is `0x4B0B86`: **only when
`cob+0x53C != 0`** does it run the eight records with `dt = 0`, and the stepper `0x4B1C00(cob, 0)`
runs either way (and returns at once on `dt == 0`).

| Script | Site | Entry | `runNow` | `argc` |
|---|---|---|---|---|
| `Create` | `0x485DE6` | `0x4B0940` | **1** | – |
| `StartMoving`, `StopMoving` | `0x43DAF2` | `0x4B0940` | **1** | – |
| `MoveRate1/2/3` | `0x43DB27` | `0x4B0940` | **1** | – |
| `setSFXoccupy` | `0x43DBE8` | `0x4B0A70` | **1** | 1 |
| `Killed` (the second death path) | `0x486877` | `0x4B0A70` | **1** | 1 |
| `Activate`, `Deactivate`, `StartBuilding`, `StopBuilding` | `0x48B106`, `0x48B12B`, `0x48B14E`, `0x48B169` | `0x4B0940` | 0 | – |
| `FirePrimary/Secondary/Tertiary` (table `0x509678`) | `0x49CB94`, `0x49CD4F`, `0x49CF73` | `0x4B0940` | 0 | – |
| `AimPrimary/Secondary/Tertiary` (table `0x509688`) | `0x49E31C` (heading, pitch, with the aim callback), `0x49E386` (`AutoAim`, all four words 0) | `0x4B0A70` | 0 | 2 |
| `SetMaxReloadTime` | `0x49E186` | `0x4B0A70` | 0 | 1 |
| `SetSpeed`, `SetDirection` | `0x437902`, `0x43795E`, `0x43798A` | `0x4B0A70` | 0 | 1 |
| `RockUnit` | `0x499C5C`, `0x49CBEB`, `0x49CDA6`, `0x49CFCA` | `0x4B0A70` | 0 | 2 |
| `HitByWeapon` | `0x489F43` | `0x4B0A70` | 0 | 2 |
| `TakeDamage` | `0x489F8E` | `0x4B0A70` | 0 | 1 |
| `TargetCleared` | `0x489898`, `0x489948`, `0x48A149`, `0x48A2E0` | `0x4B0A70` | 0 | 1 |
| `EndTransport` | `0x40F433`, `0x411DA1` | `0x4B0940` | **1** | – |
| `EndTransport` | `0x41148D`, `0x411794`, `0x411E2B` | `0x4B0940` | 0 | – |
| `TransportPickup`, `TransportDrop`, `BeginTransport` | `0x406834`, `0x4069BF`, `0x4113EF` | `0x4B0A70` | **1** | 1 |
| `Query*`/`AimFrom*` (tables), `QueryNanoPiece`, `SweetSpot`, `QueryBuildInfo`, `QueryTransport`, `QueryLandingPad`, `Killed` (`Send_UnitDeath`) | `0x43E227`, `0x43E291`, `0x43E32C`, `0x43E370`, `0x43E3E4`, `0x43E427`, `0x4027FB`, `0x4113B1`, `0x41189C`+4, `0x4865C3` | `0x4B0BC0` | query | 4 in, 4 out |

`0x509678` = `{FirePrimary, FireSecondary, FireTertiary, NULL, AimPrimary, AimSecondary,
AimTertiary, NULL}`; `0x509688` = `{AimPrimary, AimSecondary, AimTertiary, NULL, …}` — both
indexed by `(weaponbits >> 2) & 3`, which is why the stock engine stops at three weapons.

**Where each of those sits in the frame** matters as much as `runNow`, because a run-later start
issued before the unit's own `DoScriptsNow` still takes its first step in the same tick. The
per-unit tick function (it ends at `0x48B080`) runs, in order: `0x437910` (`0x48ADC4`),
`AutoAim 0x49E1A0` (`0x48ADDA`, only when the block owner's player record — `edi`, from
`0x48AD73` — is active with `+0x73 ∈ {1,2}`, `0x48ADC9..0x48ADD7`), **`DoScriptsNow(1)`
(`0x48ADEB`)**, then, for a unit whose owner `+0x96` is of type 1 or 2 (`0x48AEB5..0x48AECD`),
`0x489BB0`, `0x41BD10`, `0x43B7C0`, `0x43BAD0`, **the movement pass `0x43DD20` (`0x48AFAA`)** and
`0x48A870`; then `0x4864B0` for a unit pending death, and after the block's units `0x48B710`
(*Replay state design* has the gates). `0x43DD20` calls `0x43DA70`, which holds the
`StartMoving`/`StopMoving` (`0x43DAF2`) and `MoveRate1..3` (`0x43DB27`) starts, and `0x43DB50`
(`setSFXoccupy`, `0x43DBE8`) — so **those land after the unit's own script tick**, while the weapon
and aim traffic lands before it.
`UNITS_SetStateMask` (`Activate`/`Deactivate`, `0x48B090` — landing 3 wrote `0x48B0A0`, which is
0x10 past its entry; `tools/ta_symbols.txt:284`) is *not* in that function; measured,
its starts land after `DoScriptsNow` too (the fighter, gunship and bomber fixtures each start
`Activate` one tick after `Create` and it first steps the tick after that). `SweetSpot` and
`Killed` come from the *attacker's* tick and so land after this unit's as well, when the
attacker's index is the higher one — which it is in all nine fixtures.

### Opcodes this engine does not implement (tacob landing 3)

The dispatch's last compare chain (`0x4B1B48`) tests only `SET 0x10082000`, `ATTACH 0x10083000`
and `DROP 0x10084000` above `EXPLODE`, so **`PLAY_SOUND 0x10072000` and `MAP_COMMAND
0x10073000` fall into `0x4B1B60` and kill the thread silently.** `play-sound` is in the BOS
dialect the community writes and in our compiler, and in retail TA it ends the script that uses
it — a lint, not an opcode. Two more shapes worth recording: **`MOD 0x10034001` reaches the same
handler as `DIV`** (the dispatch masks `op & 0x100FF000`, which erases the `1`), so `%` *is*
integer division here; and `BITWISE_NOT 0x10038000` and the logical `NOT 0x1005A000` neither pop
nor push — they rewrite the top of the stack in place (`0x4B159E`, `0x4B1896`). `0x10009000`
(two pops → `vt+0x28`), `0x1000A000` (`vt+0x2C`), `0x10044000` (one pop → `vt+0x48`),
`0x10045000` (no pops → `vt+0x4C`) and `0x10063000` (pops `[pc+2]` words into the runner's own
frame) are handled but unused by the stock corpus and their vtable slots are unread.

**Handler bodies the VM had to match** (the table above lists each opcode's `cmp`; these are the
bodies whose stack effect is not obvious from it): the two-operand arithmetic and comparison
words run `0x4B1479..0x4B18AB`, popping `b` then reading `a` in place and writing the result over
`a`; the logical `OR 0x4B1829` pushes `a` when both sides are zero (so its result is 0/1 anyway)
and `XOR 0x4B1875` is the plain bitwise word; the logical `NOT` body ends at `0x4B18AB`. Waking a
blocked caller is the same eight-record scan in two places — `0x4B19FF` after a `RETURN` and
`0x4B1AB1` after a `signal` kill — each testing `status & 0xFFF00000 == 0x2800000` and
`+0x18 == the freed slot`.

### The posed model the opcodes write into — `0x45AEC0`, and the piece order

The scripted builder [`0x45A950`](memory.md#0x45a8d0-0x45a950-the-object3do-builders) builds the
`Object3do` at `unit+0x9E`. It binds the model's
nodes to the COB's **piece-name table** (the name compare at `0x45A9FD` → `0x4F8A70`), so the
`PrimitiveStruct` array is in the COB's piece order, not the 3DO's tree order — ARMPW's array
slot 0 is `torso`, its COB piece 0, while the 3DO's root node is `ground`. `0x45AEC0` then walks
the tree and initialises each `PrimitiveStruct` (stride `0x36`, first at `o3+0x22`):

- `+0x22` a private copy of the node's vertices, `count × 12` bytes (tag `0x506624`)
- `+0x28` flags: **bit 1 set for every piece** (`0x45AED4` — the `cached` bit `DONT_CACHE`
  clears), bit 2 set for every piece (`0x45AF31`, unread), and **bit 0 — `Visible` — set only
  when the node has three or more vertices** (`0x45AF1B`: `cmp [node+4], 3` / `jl` →
  `and [prim+0x28], 0xFFFE`).

That last line is why a unit's flares, wakes, thrust anchors and torpedo tubes are hidden
without any `hide` in its script: they are one- and two-vertex marker nodes. Measured against
all eight fixtures that dumped a pose of their own unit — every `HIDDEN` piece is either such a
node or one the unit's `Create` hides, with no exceptions and no false positives.

**How COB piece *i* comes to name primitive *i*, exactly** `[DISASSEMBLED 2026-09-25]`. The count
`0x45AE80` is `1 + count(child) + count(sibling)`, so it includes the root's own siblings.
`0x45AEC0(o3, node, parent)` lays the primitives out in **pre-order**: the node at the index
the count at `o3+0x00` holds (read `0x45AECA`, incremented `0x45AF35`), its parent primitive
stored at `+0x32` (`0x45AF6B` / `0x45AF77`), then its child's whole subtree with itself as the
parent (`[ebp+0x30]`, `0x45AF41..0x45AF44`), then its sibling's with its own parent
(`[ebp+0x2C]`, `0x45AF60..0x45AF63`); it returns the primitive, stored as the child link `+0x2E`
and the sibling link `+0x2A`. Then the builder's loop runs over the COB's pieces: for piece *i* it
takes the first primitive **from index *i* up** whose node name matches and swaps it into place
([the loop](memory.md#0x45a8d0-0x45a950-the-object3do-builders)). So a COB piece whose name no node carries drives
whatever primitive sits at its index, and two nodes of one name go to the first one found at or
after the index. `tagpu_datakeys.c` reproduces this to find the node a `Create()` `HIDE`
reaches; on the stock structures it equals a by-name match.

### The repose, and the window it leaves open — `0x45AC20`, `0x45AB10`, `0x45B030`, `0x45B0A0`

The posed vertex buffers (`prim+0x22`) are not built once. **They are rewritten in place, on the
game thread, every time the pose is dirty — and the rewrite has two stages, with the buffer
holding the model's REST vertices in between.** Read out of the binary 2026-09-08; this is the
mechanism behind the one-frame pose pop the native pass showed on a walking commander
(`gpu-status.md`, the pose-race row).

**`Object3do+0x08` is the dirty flag, and it brackets the rewrite exactly.** Written 1 at:

| site | when |
|---|---|
| `0x45AC89` (inside `DrawUnit 0x45AC20`) | the cached body turn `o3+0x18/+0x1A/+0x1C` differs from the live `unit+0x64/+0x66/+0x68` by ≥ 8 on any axis |
| `0x45AB6C` (`0x45AB10`, the same test, called from the COB at `0x480EFC`) | as above |
| `0x45ADA5` | the same test again, for each attached unit in `DrawUnit`'s cargo chain |
| `0x480C90` | a COB `move` writes a piece position (`0x480C60`, the `MOVE` setter) |
| `0x480D22` | a COB `turn` writes a piece angle (`0x480CE0`, the `TURN` setter) |

and written 0 at `0x45AD28`, `0x45AC0A` and `0x45AE47` — the last thing each repose does. **There
are THREE reposes, not two:** besides the one inlined in `DrawUnit` and the standalone
`0x45AB10`, `DrawUnit`'s attached-unit chain carries a full third copy for each unit of the
cargo — flag set `0x45ADA5`, entry gate `0x45ADCC`, the base piece's `rep movs` `0x45ADF3`, the
tree reset `0x45AE1D`/`0x45AE2D` (both into `0x45B030`), the compose `0x45AE3C`, the clear
`0x45AE47`. `[MEASURED 2026-09-08 against the pristine build]`; the set-site table above already
listed `0x45ADA5`, and this note previously said the flag was cleared at two sites only, which
left it with three sets and two clears. Nothing about the guard changes — the flag still brackets
that repose exactly — but a transported unit's flag lifetime is the third site's, not the first's.
The repose is **entered only when the flag is non-zero** (`0x45ACB1` / `0x45AB94` / `0x45ADCC`),
so the flag is 1 for the whole of it. It is also 1 while the buffers are merely *stale* — a COB write the next `DrawUnit`
has not composed yet — which is the common case and is perfectly consistent to read.

⚠ **A NEGATIVE RESULT WORTH THE SPACE `[MEASURED 2026-09-08]`: some units are never composed at
all, and their posed buffers therefore hold rest vertices for the whole session.** Over a 69-unit
screen inventory with the native pass armed, three to six units per frame read **byte-equal to
their own `node+0x24` arrays** for as long as they were on screen, with `Object3do+0x08` set on
both sides of every read — measured by `tagpu_native.c`'s rest-equality detector, whose whole job
is to catch exactly that, and which is supposed to read 0 in play. They are almost all
**structures** — identified through `Object3do+0x0C → unit+0x92 →` the def's name as "Gaat Gun",
"Sentinel", "Solar Collector" (both sides), "Vulcan", "Scorpion" and "Wind Generator", plus one
aircraft, "Hurricane". So `prim+0x22` is **not** reliably "the pose one tick behind": for these it
is the rest pose indefinitely, and anything reading it draws them unrotated unless it reconstructs
from the fields.

**Which of the three repose call sites is not reaching them is NOT established.** The candidates
are that the composite is cached and its builder therefore never re-runs, or that an idle COB
script keeps setting the flag without anything clearing it; neither was tested. What is measured
is the state of the buffer and of the flag, not the reason.
[GPU posing §0b](../gpu-posing.html) has the run.

**Stage 1, the reset.** `0x45ACC1..0x45ACF3` (and the identical `0x45ABA4..0x45ABD5` in
`0x45AB10`) `rep movs` — the instruction itself is at **`0x45ACDD`**, `0x45ABBF` in the other
copy — the node's own vertex array `node+0x24` back over the base piece's
`prim+0x22`, `[node+0x04] × 12` bytes, and zero `prim+0x16/+0x1A/+0x1E` (the piece origin) and
`prim+0x26`. **`0x45B030`** then does the same for the whole tree, recursing on `prim+0x2E`
(child) and looping on `prim+0x2A` (sibling); a piece with `prim+0x26 != 0` is skipped unless it
is the top-level call. At the end of this walk **every piece's posed buffer holds its rest
vertices** — unrotated, unposed, no body turn.

**Stage 2, the compose.** `0x45B0A0(ecx = Object3do, edx = base piece, [esp+4] = isChild)`:

- on the **top-level call only** (`isChild == 0`, `0x45B0DB`) it adds the cached body turn into
  the piece's own angle triple before using it — `o3+0x18` onto `prim+0x14` (the Z word),
  `o3+0x1A` (the heading) onto `prim+0x12` (Y), `o3+0x1C` onto `prim+0x10` (X). **The body turn
  is folded into the base piece's turn, not applied as an outer rotation**, which is only the
  same thing when the base piece's own turn is zero — it is on every stock unit measured
  (ARMCOM's base piece is `ground`, turn `(0,0,0)`).
- it builds the origin triple as `prim+0x04/+0x08/+0x0C` (the COB `MOVE` delta) plus the node's
  `+0x10/+0x14/+0x18` (the rest offset) — the same pair `0x43DEF0` uses — and calls
  **`0x45B150`**, which for each piece rotates `prim+0x16` and then **every vertex of
  `prim+0x22`, in place, one at a time, last to first** (`0x45B18C..0x45B1AA`) through
  `0x4B6CC0`, and then adds the parent origin to the piece origin and to every vertex, again in
  place (`0x45B1AC..0x45B204`). A piece with `prim+0x26 != 0` is left alone entirely.
- `0x45B150` recurses on the child (`prim+0x2E`) and loops on the sibling (`prim+0x2A`), so the
  tree is composed piece by piece.

**The window.** Between the last `rep movs` of stage 1 and the moment stage 2 reaches a given
piece, that piece's `prim+0x22` holds rest vertices; between the rotate loop and the translate
loop it holds rotated-but-unmoved ones. Anything reading `prim+0x22` from another thread can
see either. The whole rewrite is only microseconds for a 15-piece unit, which is why the pop is
rare on an idle machine and clusters into bursts when the game thread is preempted inside it —
and why `Object3do+0x08` is the only usable interlock: it is the one field that is set before
the first write and cleared after the last.

**The rebuild that leads to it is a different question.** `0x458810`'s local rebuild flag
(`[esp+0x10]`, built at `0x458870..0x4588F2`) is set when `o3+4 == 0`; when a structure
(`unit+0x110` bit 29) lacks its composite; when `unit+0x114` bit 0 is set and the composite is
absent; or when a structure has a nonzero construction fraction, state bit 13 set and an
existing composite with **no depth plane** (`0x45889A..0x4588C1`). That last case does not
require a null composite. The test is not simply "the pose changed": the repose runs in
`DrawUnit` before `0x458810`, and cache invalidation can separately force rebuilding.
The allocation choice is mapped in *Replay snapshots and draw-created caches* above
[DISASSEMBLED 2026-09-28].

### `get` and `set` — the twenty value ids (tacob landing 4, 2026-09-07)

`GET_UNIT_VALUE` and `GET` reach `vt+0x44` = **`0x480770`**, `SET` reaches `vt+0x40` =
**`0x480B20`**. Both are `thiscall(cob, id, a, b, c, d)` / `thiscall(cob, id, value)` and both
open with `lea eax,[ecx-1]; cmp eax,0x13; ja` — so **the value ids really are 1..20** and
anything else returns 0 / does nothing. `esi` is the unit, taken as `[[cob+0x540]+0x0C]`.
Impure's `cob_get` takes over `get` with a jump at `0x480770`, installed through the fail-closed
limits table, and answers ids 32 and 69–75 as well, as TADR's recorder does
([merge exploration §F](../tadr-merge-exploration.md#f-the-cob-getters)); `set` is stock. What 75
answers on a puppet is in *COB on a puppet*, in the replay section.
Read out of the binary at the addresses below; the arithmetic is what `tools/tacob`'s
`EditorWorld` reproduces, and `tools/test_tacob.py` §`ValueIds` pins it.

**Live control of the stock handler, 2026-09-28:** the generated getter probe in the
complete Escalation 10.2.0 setup, before Impure's jump existed and with no TADR getter hooks,
executed ids 32 and 69–75 on a human-owned unit and a local-AI-owned unit: all 16 returned
zero through this handler.
Two fresh Wine skirmishes on the reference setup read `u16 main+0x14351` as **10001**
via `main = *(u32*)0x511DE8`; the probe units occupied slots 2 and 1002. A raised
capacity does not make every match allocate 15001 slots. Getter 70 (`MAX_ID`) returns
this match's last valid index, the count − 1 (10000 here), not the capacity maximum.
The test, its failing port assertions and remaining gaps are in
[group F](../tadr-port/cob.md#all-eight-getters-called-in-real-escalation).
The repeated baseline additionally read the fixed unit array at `main+0x14357`,
stride `0x118`, after bounding each ordinal by that count: `+0xA8` matched the unit
ID, `+0xFF` read owners 0 and 1, `u16 +0xB8` read zero kills, `float +0x104` read
zero construction remaining, and `u32 +0x110` had bit 28 set on both. These are
probe precondition checks, not evidence for a non-zero getter result.

**GET — the jump table is `0x480AC4`, indexed by `id − 1`** (20 dwords; out of range →
`0x480ABB`, `xor eax,eax`).

| id | Name | Handler | What it computes |
|---|---|---|---|
| 1 | `ACTIVATION` | `0x480794` | `unit+0x10E` bit 0 |
| 2 | `STANDINGMOVEORDERS` | `0x4807A4` | `(unit+0x110 >> 18) & 3` |
| 3 | `STANDINGFIREORDERS` | `0x4807B7` | `(unit+0x110 >> 20) & 3` |
| 4 | `HEALTH` | `0x4807CA` | `(i16)(unit+0x108) × 100 / (def+0x1FA)`, unsigned `div` — a **percent**, 0..100 |
| 5 | `INBUILDSTANCE` | `0x4807EF` | `unit+0x10F` bit 0 |
| 6 | `BUSY` | `0x4807FF` | `unit+0x10F` bit 1 |
| 7 | `PIECE_XZ` | `0x480811` | `0x43E060(&v, unit, a)`, then `(v.x & 0xFFFF0000) + (v.z >> 16)` |
| 8 | `PIECE_Y` | `0x48083F` | the same call; `v.y` **raw 16.16**, not an integer |
| 9 | `UNIT_XZ` | `0x480868` | that unit's `+0x6A`/`+0x72` packed the same way |
| 10 | `UNIT_Y` | `0x4808C2` | that unit's `+0x6E`, raw 16.16 |
| 11 | `UNIT_HEIGHT` | `0x48090F` | `[[unit+0x92]+0x16E]` — a unit-**definition** field |
| 12 | `XZ_ATAN` | `0x480965` | `0x4B715A(x, z)` **minus the unit's own heading `+0x66`**, `& 0xFFFF` |
| 13 | `XZ_HYPOT` | `0x480994` | `_hypot` of the unpacked 16.16 pair → `_ftol` → 16.16 |
| 14 | `ATAN` | `0x4809C7` | `0x4B715A(a, b)`, `& 0xFFFF` — **no** heading subtraction |
| 15 | `HYPOT` | `0x4809E5` | `_hypot(a, b)` on the raw arguments, `_ftol` |
| 16 | `GROUND_HEIGHT` | `0x480A0D` | `0x485070(&v)` on the unpacked pair, result `<< 16` |
| 17 | `BUILD_PERCENT_LEFT` | `0x480A44` | `0` when `unit+0x104 == 0.0f` (`ds:0x4FD668`), else `1 − (int)(unit+0x104 × −99.0f)` (`ds:0x4FD66C`) — the field is `Nanoframe`, and that it holds the fraction *still to go* is read off the arithmetic, not the writer `[INFERRED]` |
| 18 | `YARD_OPEN` | `0x480A83` | `unit+0x10F` bit 2 |
| 19 | `BUGGER_OFF` | `0x480A96` | `unit+0x10F` bit 3 |
| 20 | `ARMORED` | `0x480AA9` | `unit+0x10E` bit 1 |

Four things in that table are not folklore and cost scripts real bugs:

- **The XZ packing *adds* rather than or-s.** `0x480821` builds `(x & 0xFFFF0000) + (z >> 16)`
  from two 16.16 world coordinates, so a negative z **borrows from x** — and every handler
  that unpacks one (`0x480965`, `0x480994`, `0x480A0D`) undoes it with the same three
  instructions: `x = v & 0xFFFF0000`, `z = v << 16`, and `if (z < 0) x += 0x10000`.
- **`XZ_ATAN` is relative, `ATAN` is absolute.** Only the first subtracts `unit+0x66`, so
  `get XZ_ATAN(…)` answers "how far round from where I am pointing" and `get ATAN(dx, dz)`
  answers a world bearing. Both come back as `uint16` TAang.
- **The distances are 16.16, not world units.** `PIECE_Y`, `UNIT_Y`, both hypots and
  `GROUND_HEIGHT` are all `world × 65536`; only `PIECE_XZ`/`UNIT_XZ` hold integers, and only
  because the packing shifts them.
- **`BUILD_PERCENT_LEFT` is never 0 while a unit is building.** Read the constants back and
  `unit+0x104` has to be the fraction still to go: `1 − trunc(frac × −99)` gives 100 at the
  start and 1 just before the end, and
  the id returns a true 0 only on the `frac == 0.0f` early out — so `while( get
  BUILD_PERCENT_LEFT )` terminates exactly at completion.

**SET — a byte table at `0x480C18` (20 bytes, `id − 1`) selects one of seven cases from the
jump table at `0x480BFC`.** The bytes are `00 06 06 06 01 02 06 06 06 06 06 06 06 06 06 06 06
03 04 05`, and **case 6 is the shared default `0x480BF1`, which only ORs `unit+0xBA |= 4` and
returns.** So:

| Case | ids | Handler | What it writes |
|---|---|---|---|
| 0 | `ACTIVATION` | `0x480B49` | `UNITS_SetStateMask 0x48B090(unit, 1, value)` — the same call `Activate`/`Deactivate` take |
| 1 | `INBUILDSTANCE` | `0x480B62` | `unit+0x10F` bit 0 |
| 2 | `BUSY` | `0x480B84` | `unit+0x10F` bit 1 |
| 3 | `YARD_OPEN` | `0x480BA8` | `0x47DAC0(unit, value)` |
| 4 | `BUGGER_OFF` | `0x480BBE` | `unit+0x10F` bit 3 |
| 5 | `ARMORED` | `0x480BE3` | `0x48B090(unit, 2, value)` — which is how TA:ESC's COB "shields" work |
| 6 | the other **fourteen** | `0x480BF1` | nothing but the dirty bit |

**`set HEALTH to 50` is a measured no-op on retail TA**, and so is every other write to ids
2, 3, 4 and 7..17. The editor lints it (`set-ignored`) rather than modelling a write that the
engine does not make.

Helpers the ids reach, all read this session:

- **`0x4B715A`** `cdecl(a, b)` — `fild a; fild b; fpatan` (so `atan2(a, b)`, +x measured from
  +z), `fmul qword ds:0x509EF0` = **65536 / 2π = 10430.37835047**, then a **bare `fistp`**, so
  this one rounds to nearest. `ds:0x509EF8` is its inverse, 2π/65536.
- **`0x4E43A0`** is MSVC's `_ftol`: `fstcw`, `or ah,0x0C`, `fldcw` — the mode is forced to
  chop, so every other float→int here **truncates toward zero**. `0x4FB440` is the two-double
  entry of the C runtime's mode-dispatched math routine (`push 0x18; call 0x4FB480`); the mode
  table is unread, so "hypot" is from the id it serves and the shape of the call, not from the
  callee `[INFERRED]`.
- **`0x485070(vec3*)`** reads the *high halves* of the struct's `+0x00` and `+0x08` dwords
  (`movsx eax, word [ecx+2]` / `[ecx+0xA]`), i.e. the integer world x and z, then `>> 4` for
  the heightmap cell and `& 0xF` for the sub-cell — 16 world units to a cell.
- **`0x4B6C30(n)`** — the sim RNG, and it is **Park–Miller by Schrage's trick**: state at
  `ds:0x51FC88`, `q = s / 127773` computed with the magic multiply `0x69C16BD` at `0x4B6C47`,
  then `s = 16807·s − q·0x7FFFFFFF` (which is `16807·(s mod 127773) − 2836·q`), `s += 0x7FFFFFFF`
  when the result is `<= 0`, and the draw is `s % n`. `n < 2` returns 0 **without touching the
  state** (`0x4B6C38`). `tools/tacob`'s `SimRandom` is that recurrence; what the game seeds it
  with at match start is **per peer** [DISASSEMBLED 2026-09-25]: `0x497180` calls
  `QueryPerformanceCounter` (IAT `0x4FC0BC`) and passes `low + high` to `0x4B6CA0` (`0x49719D`),
  which stores `(seed ^ 0x66E29572) | 1` at `0x51FC88`; `srand(time(0))` follows (`0x4971A5`). No
  peer's sim RNG agrees with another's, which fits state-and-event replication (the per-peer wind
  of [the B evidence](../tadr-port/sim-fixes-evidence.md), Part 4 §6a).
- **The C runtime's `rand` is a second, separate stream** [DISASSEMBLED 2026-09-23]: `rand`
  `0x4E4870` is MSVC's LCG (`·0x343FD + 0x269EC3`, bits 16..30), its state at `+0x14` of the
  per-thread data `0x4EB0F0` returns, and `srand` is `0x4E4860`, which writes only the calling
  thread's state. Two calls seed it from the clock (`0x4E6480` is `time`): the match's
  `srand(time(0))` at `0x4971AE` seeds the loader thread's stream, and WinMain's at `0x49E8BB` the
  game thread's. 61 sites draw from it — the particle
  emitters among them. The three at `0x49C619`, `0x49C648` and `0x49C677` add `rand() % 11 − 5` to
  screen coordinates taken with the eye (`main+0x1431F`/`+0x14323`): the lightning bolt's jitter,
  a draw, not a rule.

### The piece transform — `0x43DEF0`, `0x43E060` and `0x4B6CC0` (tacob landing 4)

`PIECE_XZ` and `PIECE_Y` are the only place a script can see the composed pose, so their
helpers settle the rules the renderer needs as well.

**`0x43E060(out, unit, piece)`**, `stdcall`, `ret 0xC`: calls `0x43DEF0` for the piece's offset
in the unit's own frame and adds the unit's world position — `+0x6A` x, `+0x6E` altitude,
`+0x72` map depth, all 16.16 — writing `(x, y, z)` to `out`.

**`0x43DEF0(out, unit, piece)`**, `ret 0xC`, is the composition:

- `o3 = unit+0x9E`; a piece `< 0` or `>= [o3]` returns `(0,0,0)`, and so does a null model.
- The accumulator starts at the piece's own **node offset plus its `PrimitiveStruct` position**:
  `0x43DF2A..0x43DF55` pairs `prim+0x04/+0x08/+0x0C` with the node's `+0x10/+0x14/+0x18` in
  that order. **So the COB's three axis operands are plain X, Y and Z** — the `XPos/ZPos/YPos`
  naming in `tamem.h` is TA's screen convention, not a transposition — and **`MOVE` is a delta
  in the parent's frame, added to the rest offset before any rotation.**
- Then, for each ancestor up the `prim+0x32` parent chain: rotate what is under it by that
  ancestor's three angle words through `0x4B6CC0`, **then** add that ancestor's own
  offset+position. The requested piece's own angles are never applied — turning a piece does
  not move its origin.
- At the **root** (`prim+0x32 == 0`) the unit's body turn is added to the angle triple first:
  `+0x64` to the z-axis word, `+0x66` (the heading) to the y-axis word, `+0x68` to the x-axis
  word. Because Y is the outermost factor, that is the same thing as applying the yaw last.
- The returned z is **negated** (`neg ecx` at `0x43E00A`), which is what puts it on the world's
  map-depth axis.
- **All three of the unit's words are live on a ground unit, not just the heading.**
  `[MEASURED 2026-09-08]` Three ARMSTUMPs parked on Two Continents grass read `+0x64` (bank) /
  `+0x68` (pitch) as `0x0C57`/`0xF047` = **+17.4° / −22.1°**, `0`/`0xEA2B` = **0 / −30.7°** and
  `0`/`0x0146` = **0 / +1.8°** — the terrain's tilt, not the flat 0 a "heading" reading of
  `+0x66` would suggest. Anything that rotates a unit-space point by the heading alone (the
  selection rect did until 2026-09-08, `ui-markers.md` §1) is therefore right on level ground
  and a few pixels out on a hillside. *[CORRECTED 2026-09-08: this bullet used to end "`+0x66`
  alone stays correct for the body geometry only because the engine bakes just the yaw into
  `vbuf` (`pose_dump`, err 0.00)". It does not. `vbuf` carries all three — the compose folds
  the whole cached triple at `0x45B0DB` — and `pose_dump` read err 0.00 only on fixtures whose
  bank and pitch happened to be zero. See "Checked against the engine's own vertex buffer"
  below.]*
- **`0x467A50(ctx, &pos, pts4, &angles)` — rotate four points, project them, draw the loop.**
  `[BINARY-VERIFIED 2026-09-08]` `ret 0x10`; one caller, `0x46A5FB` inside
  `DrawUnitSelectBoxRect`. Per point it calls `0x4B6CC0(pts[i], scratch, angles)` and then
  projects with the **standard rule, one term at a time**:
  `sx = ((rot.x + pos.x) >> 16) + 0x80` and
  `sy = ((pos.z − rot.z) >> 16) − (((rot.y + pos.y) >> 16) >> 1) + 0x20` (`0x467A8C`..`0x467AC7`).
  Both `>>` are arithmetic — floors — and the `sar 1` at `0x467AB5` halves the **already
  truncated** height, so it is `floor(a) − floor(floor(b)/2)` and not `floor(a − b/2)`; the two
  differ by a pixel on some edges. Each component is also narrowed through `movswl` before use.
  The colour byte is read once from `main+0xDD5` and the four lines go out through
  `DrawLine 0x4BE950` as `p0→p1→p2→p3→p0`. **Both scratch buffers are globals and therefore
  peekable**: `*(main+0x14383)` holds the four rotated vectors (three 16.16 dwords each) and
  `*(main+0x14387)` the four screen points (two ints each), for the **last box drawn** — which
  is how our own redraw was held to the engine's own corner integers rather than to its pixels.
- **`0x4CB650(model, &min, &max, flag)` is not a whole-tree AABB unless you ask for one.**
  `[BINARY-VERIFIED 2026-09-08]` It seeds both vectors with `{0,0,0}` (`0x4CB65D`..`0x4CB675`)
  and calls `0x4CB6A0(node, offset, min, max, flag)`, which accumulates the node's own vertices
  (`node+0x24`, count `node+0x04`, offset `node+0x10/14/18` added and passed down to the child)
  — but **skips a node with fewer than three vertices** (`0x4CB6D9` `cmp $2,eax; jle`, the
  `0x45AF1B` threshold again) and **recurses into the child (`+0x30`) and the sibling (`+0x2C`)
  only when `flag` is non-zero** (`0x4CB780` `test ebp,ebp; je`). The sibling is walked with the
  *caller's* offset, the child with the accumulated one. `DrawUnitSelectBoxRect` passes
  `flag = 0` (`push $0` @`0x46A55A`), so a selection box is the ROOT PIECE's vertices unioned
  with the model origin — which is what makes it smaller than the model.

**`0x4B6CC0(in, out, angles)`** rotates one vector by the three words, and **fixes the order**.
*[CORRECTED 2026-09-08: the argument order is `(in, out, angles)` — `0x4B6CC4` takes arg1 as the
source and `0x4B6D09` writes arg2 — which is what `effects.md` says.]* The rotation reads its
angle words at `angles+0x00` for the `(x,y)` pair (`0x4B6CD6`), `angles+0x04` for `(y,z)`
(`0x4B6CF6`) and `angles+0x02` for `(x,z)` (`0x4B6D1B`) — so for a caller passing a bare triple,
word[0] drives the FIRST rotation and word[2] the second. Against a `PrimitiveStruct`, whose
angle words sit at `+0x10/12/14`, that is:

| Order | Pair rotated | `PrimitiveStruct` word | COB axis operand |
|---|---|---|---|
| first | `(x, y)` | `+0x14` | `z-axis` (2) |
| second | `(y, z)` | `+0x10` | `x-axis` (0) |
| third | `(x, z)` | `+0x12` | `y-axis` (1) |

so the local matrix is **`Ry · Rx · Rz`**. Each step is `0x4B7173(angle, pair)`: it returns
untouched on a zero angle word (`cmp word [ebp+8],0`), else `p0' = p0·cos − p1·sin`,
`p1' = p1·cos + p0·sin` with the angle read as a **signed** 16-bit through `fild word`, scaled
by `ds:0x509EF8`, and stored back with a bare `fistp` (round to nearest).

**The 3DO loader negates X and Z.** Every offset and every vertex of the `Model3DONode` the
engine holds is `(−x, y, −z)` of the same field in the `.3do` — a half turn about Y, baked in
at load. `[MEASURED 2026-09-07]` against all eight `posedump.txt` fixtures, which print the
engine's own `N_OFF` and vertex array beside the file's: every piece of all eight units, x and
z flipped, y kept, no exceptions. The site that does it is **not located**; the fact is read
off the two arrays, not off code.

**Checked against the engine's own vertex buffer.** `tools/tacob pose-check --all` rebuilds
each fixture's posed vertices from the rules above and diffs them against `P_VBUF`, which
`tagpu_native.c`'s posedump prints beside the model-space vertex it came from. The residual is
**exactly 0 on every class** once the body turn is supplied in full. That also settles the two
questions `model-import.md` left open: the composition order, and `MOVE` being a delta added
before the rotation.

*[CORRECTED 2026-09-08.] This paragraph used to record 4.1 / 7.0 / 48.4 / 77.2 world units on
the tank, fighter, gunship and bomber and conclude that **the vertex buffer is a frame or two
behind the pose the dump sampled**. That was wrong, and the way it was wrong is worth keeping:*

- *`pose-check` passed `body=(0, yaw, 0)` and `tagpu_native.c`'s `pose_dump` rotated model space
  by `unit+0x66` alone. **Both omitted the bank and the pitch**, which the compose folds at
  `0x45B0DB` along with the heading. The two agreed to within 1.4 units on every fixture, and
  that agreement was read as corroboration when it was a shared omission.*
- *`[MEASURED 2026-09-08]` The missing words are recoverable from the tracked fixtures
  themselves: each records its base piece's rest vertices (`node=`) beside the engine's posed
  ones (`vbuf=`), and Kabsch on those pairs gives the rotation exactly. Supplying the recovered
  triple takes **all eight classes to exactly 0** — and on five of the eight (kbot, tank,
  building, ship, sub) the recovered heading reproduces the recorded `yaw` to within 2 units,
  which is what says the recovery is sound rather than a fit: it never sees `yaw`. The tank is
  the telling one — its residual was 4.11, so it is not one of the four that already read 0, yet
  its heading comes back exact while its pitch comes back as the −12.34° that was being dropped.*
- *`[MEASURED 2026-09-08]` Confirmed live: `pose_dump` now prints `body=` (the cached triple it
  folds) and `live=` (`unit+0x68/+0x66/+0x64`), and a fresh capture of the tank and the bomber
  reads **`err=0.00` on every piece**, against 5.45 and 77.19 before. The tank's recorded
  `body=(63290,49152,0)` is exactly the triple recovered offline from its own fixture.*
- ***The cached triple is not always the live one.*** *The tank read `body=` and `live=`
  identical; the bomber read `body=(0,16128,3)` against `live=(0,44767,65508)` — a heading
  28639 units (157°) apart, and it is `body=` that the drawn geometry follows. Which of the two
  moves, and why, is **not established**. It matters because anything reconstructing a unit's
  pose must fold `Object3do+0x18/+0x1A/+0x1C` and not `unit+0x64/+0x66/+0x68`; the fork's
  `recon_begin` already does.*
- *The fixtures in `research/notes/evidence/cobtrace/` were captured before `body=` existed, so
  `pose-check` still reports the old residuals for them and labels them `legacy: yaw only`.
  Regenerating them is `tools/cobtrace_fixtures.py`, and it also regenerates `cobtrace.log` and
  needs `tacob fit-world` re-run for the tank's `replay.json` — a separate gate
  (`tacob run --all`, nine byte-identical replays), not done here.*

### [REPLAY] The nine fixtures, re-run offline (tacob landing 3, 2026-09-07)

`tools/tacob run --all` runs the model above against landing 2's nine logs. Each replay's
`tacob run <class> --trace-out …` output is **byte-identical to the file the game wrote** —
4272 lines across the nine, including every slot number, every `K`, every `D` and the tick of
every `R` — and every piece of the eight posedumps of the traced unit matches on `move=`,
`turn=` and `HIDDEN`. What the replay is *given* is listed in `tacob-design.md` §"What the
replay supplies"; the rules above are what it had to get right to produce the rest.

### The oracle's five sites (`tagpu_cobtrace.c`, armed by `tagpu_cobtrace.on`)

| VA | Bytes stolen | Captures |
|---|---|---|
| `0x4B08C0` | `56 8B 74 24 08` | a wrapper, the stolen prologue resuming at `0x4B08C5`: calls the original through the stolen prologue, then latches (cob, slot, index, tick, source); source from the return address — `0x4B18C0` = START, `0x4B192D` = CALL, anything else = the engine — and for the two opcodes the parent record (`esi`) and slot (`ebp`), whose `code[pc+2]` is the argument count |
| `0x4B0DA0` | `83 EC 20 53 55` | the runner's entry, resuming at `0x4B0DA5`: the latched start is written here, before its thread's first step (a `Query*` overwrites its own first argument on that step) |
| `0x4B19D0` | `8B 4E 20 85 C9` | RETURN: `esi` record, `ebp` slot, `ecx` pc, the value at `stack[sp]`; the stolen `test` still sets the flags the `je` at `0x4B19D5` reads |
| `0x4B1A99` | `C7 01 00 00 00 00` | SIGNAL's kill, resuming at `0x4B1A9F` (the sixth stolen byte is NOPped): `ecx` the record, `ebx` its slot, `[esp+0x34]` the signaller |
| `0x4B15E0` | `E8 4B 56 00 00` | the RNG call, redirected: the stub calls `0x4B6C30` itself and logs `lo + result` (`ebx` = lo, `ebp` = slot) |

Reads only: the tick `main+0x38A47`, the in-game index `unit+0xA8` (`u16`), the def name
`def+0x20`, and the record/file fields above. Loader queries and in-play events use separate
TLS latches. The trace lock orders deferred reads against the two native COB destructor
entries: the destructor invalidates every matching latch before freeing the object.
Not traced: the engine's
asks for a script the unit lacks (only `-1` reaches the allocator, the name is gone by then),
and which engine function issued an `E` start (its return address is two frames up).

The allocator entry is also the recorder's counted COB sink. Both observers require its
pristine five bytes; with trace installed first, the recorder reports that the sink holds
other bytes and leaves its counts out. MEASURED on the stock resource fixture: `images-gate`
and carried-state comparisons run, but `compare-counts` refuses the missing allocator
counter. Trace coverage belongs in an unrecorded preflight, with its lever restored before
the ordinary recording/playback lanes. No counter is excluded to accommodate tracing.

**Three call sites read for the trace's sake** (the strings are the engine's own, in `.data`):
`0x4865C3` (`Send_UnitDeath+0x113`) is **`QueryScript(cob, "Killed" @0x508BE8, &severity,
&corpsetype, ebp, ebp)`** with both out-slots the caller's locals (`lea eax,[esp+0x14]`,
`lea ecx,[esp+0x2C]`) — so `Killed` runs synchronously and its second argument is whatever the
stack held (the death fixture shows `46379093`, an address inside `TAdynmem`), and the script's
`corpsetype = …` is read back from the record; `0x486877` also starts `"Killed"` through
`StartScript` with `argc = 1` (`severity` from `[ebx+9]`, `runNow = 1`) on another death path;
`0x489F43` is `StartScript("HitByWeapon" @0x508D74, 0, runNow 0, argc 2, ebx, eax)` and
`0x489F8E` `StartScript("TakeDamage" @0x508D68, 0, 0, argc 1, eax)`, both in the damage path
(`takeDamageAddr+0x349`/`+0x394`); `"TargetCleared"` is `0x508D58`.

**[LIVE] 2026-09-07, `scenarios/cob-kbot.json`, ARMPW walking 300 units:** a run-later start
(`SetMaxReloadTime(400)` from `UNITS_StartWeaponsScripts`, `runNow = 0`) is logged at tick 117
and returns at 118 — the thread takes its first step at the next tick's `DoScriptsNow`; a
run-now start (`StartMoving`, `0x4B0940` with `runNow = 1`) returns inside its own tick; the
walk cycle (`MotionControl` `call-script`ing `walk`) is 19 ticks; the skirmish's own
commanders run `Create` at tick 0. The posedump header now carries `tick=` and `idx=` so the
two logs join.

**[LIVE] the other class fixtures (`research/notes/evidence/cobtrace/`)**, each read back
against the disassembly above: the tank's `RestoreAfterDelay` is killed by the next
`AimPrimary`'s `signal` (`K` lines, six in one run — the SIGNAL path `0x4B1A99` fires on stock
scripts every few seconds, not only in extra-weapons content); `SmokeUnit` below 66 % health
draws `rand` every few hundred ms (`D` lines, 29 in one run — every one a call through
`0x4B15E0`); a script without a trailing return runs into the next script's words exactly as
§2.8 predicted — ARMSTUMP's `HitByWeapon` (started at `0x489F43` with two arguments) ends
under `SweetSpot`'s name (its `R` line names the script whose body holds the `RETURN`);
`Send_UnitDeath`'s `Killed` query carries the caller's uninitialised local as its second
argument (`46379093` = `0x02C3A0D5`, inside `TAdynmem`, on that run); the `X` line has not
been observed — no stock fixture fills eight records. A death does not break the trace:
the ARMPW's `Killed` returns at tick 126 and the sim ticks on (593 → 803 measured after).
**Two things that are not the oracle's:** `0x43A164` (`ORDERS_CreateObject+0xA4` by the
symbol file) faults with `eax = 0x6A` — a target unit pointer of NULL plus the position
offset — when the fighter fixture gave a Hawk an `attack` order on a Vamp that then died,
with the oracle armed *and* in the control run without it (patrol orders avoid it; not
chased further); and an `E8` whose rel32 is computed against the wrong base is not a crash
but a *freeze* — the engine's own handler reports `Access Violation … at 014b227d` once per
attempt and every thread then waits on the wineserver, which is what the first tank runs
looked like before the site's displacement was fixed.

### COB guarded-runtime integration [DISASSEMBLED + MEASURED 2026-09-28]

The stock layouts above describe the executable. The F port's raised layout and
its incomplete verification gates are in [the COB note](../tadr-port/cob.md).
`cob_runtime_sites` in `tagpu_patches.c` is the checked patch inventory: allocation
operand `0x485D74`; ten record-count operands; 33 model-field operands (the getter's 34th reference lives in
its trampoline); 12 busy-count operands; six record-iteration strides; and the
record-index expressions at `0x4B08F0`, `0x4B09B2`, `0x4B0A27`, `0x4B0B25`, and
`0x4B0C57`. The original `slot*41*4` becomes `slot*137*4`. The runner address
calculation at `0x4B0DAD` is replaced by a bound and record validation **before**
the wait-state dispatcher reads piece/axis fields. The first instruction fetch
and all later fetches pass `0x4B0E5D`; `0x4B0E59` alone misses the first fetch.

The operand inventory below was checked against retail disassembly. Addresses name the
four-byte operand, not necessarily the beginning of its instruction. Every operand is
byte-checked before replacement; the owning functions retain their native call sites.

| Field or loop | Operand addresses | Native operation and raised value |
|---|---|---|
| model pointer, reads | `0x480B22`, `0x480C36`, `0x480C56`, `0x480C7F`, `0x480C8C`, `0x480C99`, `0x480CBA`, `0x480CF3`, `0x480D0C`, `0x480D1E`, `0x480D2B`, `0x480D5E`, `0x480D8B`, `0x480D96`, `0x480DB6`, `0x480DDF`, `0x480DF6`, `0x480E20`, `0x480E36`, `0x480E56`, `0x480E76`, `0x480EBD`, `0x480EF4`, `0x480F0C`, `0x480F5B`, `0x48115B`, `0x48124C`, `0x481388`, `0x481397`, `0x4813EE`, `0x481432`, `0x481472` | `mov register,[ecx/esi+0x540]` in the unit COB's model/piece/query/attachment methods; offset becomes `0x1588` |
| model pointer, initialization | `0x480D46` | `mov [ecx+0x540],eax`; offset becomes `0x1588` |
| active-record count, reads | `0x4B09CA`, `0x4B0A3F`, `0x4B0B88`, `0x4B0D22`, `0x4B0D6A`, `0x4B19EF`, `0x4B1AA1`, `0x4B1B68` | `mov eax,[edi/ecx+0x53C]` in callback/call/runner/return/signal/termination paths; offset becomes `0x1584` |
| active-record count, writes | `0x4B0638`, `0x4B19FB`, `0x4B1AAD`, `0x4B1B77` | constructor stores EDX; return/signal/termination store EAX into `+0x53C`; offset becomes `0x1584` |
| record iteration | `0x4B062F`, `0x4B08DF`, `0x4B0D4C`, `0x4B1A1E`, `0x4B1AD0`, `0x4B1AE8` | constructor/free-slot search/runner cleanup/return/signal advance a record pointer by `0xA4`; stride becomes `0x224` |
| record-count dwords | `0x4B0616`, `0x4B0D33`, `0x4B19F5`, `0x4B1AA7` | constructor initialization, callback clearing, RETURN parent wakeup and SIGNAL parent wakeup: `mov ecx/edx,8` becomes 10 |
| record-count bytes | `0x4B08E5`, `0x4B09E1`, `0x4B0A57`, `0x4B0B9F`, `0x4B0D81`, `0x4B1AEE` | allocator, synchronous named/indexed/native starts, DoScriptsNow and SIGNAL: `cmp eax/esi/ebx,8` becomes 10; capacity remains below the signed immediate's 128 boundary |

The allocation is `0x158C` bytes: header `0x1C`, ten records of `0x224`,
the active count and the model pointer. All ten count operands and the existing
allocation/stride/field operands belong to the same fail-closed installation.
Native serializer loop counts at `0x4B1EEE` and `0x4B20E2` are not executed: their
whole methods are replaced at `0x4B1EC0` and `0x4B2040`. The `and register,7`
instructions inside opcode handling decode operand modes, not thread indices.
The stock-limits comparison build retains eight native `0xA4` records.

**Cost of the capacity [DISASSEMBLED].** DoScriptsNow `0x4B0D60` reads the busy
count at `0x4B0D6A` and bypasses the record sweep when zero (`0x4B0D68..0x4B0D70`).
Otherwise its `0x4B0D81` bound visits every record; the guarded runner checks an
empty record's status and returns before execution or locking. RETURN/SIGNAL's
parent-wakeup scans also visit the whole bound. Animation stepping at the unit
tick's call `0x48ADEB` continues independently. Two additional `0x224` records
cost 1,096 bytes per scripted unit: 15.68 MiB at 15,000, excluding allocator
overhead. The loop visits increase from eight to ten; frame CPU cost is unmeasured.

The guarded wire header remains six dwords, version 1, with explicit word/record
counts. Current records occupy `0x1580` bytes including the header. Loading also
accepts the earlier guarded eight-record `0x1138` block and the native `0x528`
block. The latter carries eight `0xA4` records and a trailing active count.
Each format is bounded by its exact length, original stack capacity and original
child-slot capacity; additional runtime records are zeroed. Records, statics and
animation are published only after validation. No format reads ten records
from an eight-record input.

Getter 73's native fraction read at `0x480A62` is `fld [esi+0x104]`, reached by
the construction-query case at `0x480A44`. The guarded replacement reads that same
field only after resolving the original full-width ID against the live unit pool.

START/CALL's old address expressions at `0x4B18CA`, `0x4B18EE`, `0x4B1937`, and
`0x4B1958` are not executed by the guarded dispatcher. The adapter calls the
same allocator, copies the bounded arguments in push order, leaves the child's
SP at -1, inherits its signal mask and advances the parent by three words. CALL
waits on the child and exits at `0x4B1BE8`; START resumes at `0x4B0E59`. The
allocator's `0x4B0921` observer initializes its stack/unused wait fields and
increments the relocated count. Native RETURN and SIGNAL still wake callers.

**File allocation ownership.** `0x4BBE50(path, size_out)` obtains length from its
already-open archive/file (`0x4BBE9A..0x4BBEC2`), allocates at `0x4BBF3C`, reads
at `0x4BBF49`, and writes that allocation length to `size_out` at `0x4BBF6A`.
The read result is tested only for positive length, not exact length: the input
validator's bound is the allocation, not a claim that the stock reader detects
short reads. The COB loader's call at `0x4B245C` is intercepted to validate this
same allocation before relocation. Its subsequent checksum-size query
(`push path` at `0x4B246F`, call at `0x4B2470`) instead asks for the recorded
allocation size, avoiding a second-open size mismatch. Checksum call/return
`0x4B2477`/`0x4B247C` stays intact for the data-keys observer. The script
destructor frees the blob through `0x4D85A0` at `0x4B26D5`; metadata is unlinked
before this call. Reload's unit-before-script destruction is documented above.

**Termination, not malformed fallthrough.** Stock `0x4B1B60` clears the current
record, decrements the count and stops this invocation, without a completion
callback. Five shipped death scripts branch to exactly code length, where the
raw following word (entry zero) is zero: ESC ARMGANT (3548), COREVP (4618),
CORULAB (5424), Twilight CORCRW-OLD (606) and CORCRW (611). Twilight CMGEO reaches
word 1 at PC 346. These are native unknown-opcode termination, not missing
return operands. The guarded path accepts the proven zero and reaches the native
termination handler without fetching beyond declared code.

**Required mission units and the held-create wrapper.** The placed-unit loader
`0x488310` resolves each mission name through `0x488A50` at `0x48839B`; this bounded
binary search compares names at `def+0x20` and does not test availability. Placement
calls `0x485F50` at `0x488462` and skips NULL results at `0x48846B`. The creator's
five pushes at `0x485F50..0x485F63` put its immediate caller at site-ESP+20 at the
availability test `0x485FA0`. That is not necessarily the mission's caller:
`fix_kill_counts` routes `0x488462` through its held-create wrapper, which copies
eight arguments and makes another call. `kill_hold_site` therefore checks the
original `0x488467` return address and its type argument (`regs[PR_RET+2]`) before
arming the hold. Rejection-table indexing remains bounded by `TAGPU_LIM_TYPES`;
capture and resurrection keep their existing behavior. A malformed ARMFAV in the
first Arm campaign initially let play begin with 31 instead of 34 units, omitting
all three Jeffys. With the wrapper check, loading refuses with the unit, script
and reason before any of those placed units can be silently dropped.

**Serialization.** `0x4B1EC0(cob, stream)` is the thiscall save method;
`0x4B2040` is the load method, each `ret 4`. Stream size is `0x4B4BF0`, seek
`0x4B4C10(stream, offset)`, read `0x4B4C80(stream, buf, n)`, write
`0x4B4CF0(stream, buf, n)`; all are thiscall, and read/write return byte counts.
Stock's first `0x528` bytes hold checksum, eight `0xA4` records and busy count.
They are followed by four bytes per static and 108 per piece. Each piece is
18 animation words (`animation+4`, excluding its dirty word), three positions
(`vt+0x14`), three angles (`vt+0x18`), and flags from `vt+0x1C/+0x20/+0x24`.
Load restores the flags through `vt+8/+0xC/+0x10`, positions/angles through
`vt+0/+4`, and marks every animation plus `cob+0x18` dirty. Callbacks are always
cleared on restore. **Negative finding:** the two stores shown as `[esp+0x80]`
at `0x4B1F8B` and `0x4B1F99` do not overwrite the same flag: the first has a
pushed argument still on the stack. All three flags have distinct saved words.
The port replaces both whole methods and does not execute their old frame-copy
sizes or old count offsets. It imports stock records and writes a versioned
128-word format. Live tests restored native 32-word commander records, expanded sleeping
records with local 100, and an Escalation fusion upgrade in progress; the upgrade then completed.

**Transport operands.** `0x481340` and `0x4813B0` are the unit-class ATTACH/DROP
methods; both originally narrow to a word before unbounded pool addressing.
ATTACH forwards `(cargo, carrier, piece, flags)` to `0x48AAC0`. Its seven-byte
`0x0A` message carries u16 unit references and a **byte** piece (`0x48AB36`);
receiver `0x48AB70` treats piece `0xFF` as hidden cargo (`0x48AC99`), not geometry
piece 255. The guarded opcode checks full-width IDs and admits piece -1 or a
validated piece below 255. No new unit/network ID format is introduced.

**The two piece counts are not interchangeable.** Disassembly of `0x42D6C2..0x42D8FA`
shows the definition loader publishing its model template in `main+0x14377[type]`
at `0x42D79C..0x42D7A2`, before reading its COB at `0x42D8EF`. The checked hook at
`0x42D8F4` bounds EBP's definition index against `main+0x1438F`, binds the validated
script to that template's node count, then performs the original `def+0x18E` store.
Native `+reload` uses a separate loader call at `0x42D294` and store at `0x42D299`
(ESI owns the definition, not EBP). That store has the same binding guard and resumes
at `0x42D29F`; otherwise valid unused excess declarations would lose their model bound.
`0x42D224` requires the availability bit before reload, so a quarantined type remains
unavailable until a fresh level load rather than being rehabilitated by this command.
The builder `0x45A950` sizes the object from the template's node count `0x45AE80` and skips
COB ordinals at or above that count (`0x45A9E8`), so it never enlarges the model
([its entry](memory.md#0x45a8d0-0x45a950-the-object3do-builders)).

Negative finding: native model getters/setters such as `0x480C30`, `0x480C50`
and `0x480C7B` do not independently bound the piece. Native save/load also iterate
the COB-declared count, including unused excess names. The port permits those
names but bounds reachable operands, waits, getters 7/8 and ATTACH by the actual
model. It retains the declared-size save layout, writes zero padding for excess
names, and restores only backed rows. The native animation constructor zeros all
declared rows at `0x4B0783..0x4B0791`; restore also zeroes excess animation rows,
so the native animation stepper's dirty-row gate `0x4B1C49..0x4B1C54` cannot
dereference a missing piece. COB destruction at `0x486D8A` precedes model destruction
at `0x486D9E`; interpreter/save callbacks hold that owning-thread lifetime.
Measured in the installed Escalation assets: CORTSAR declares 110 pieces over a
109-node model; its unused final `height` declaration must not exclude the unit.

**Query-result consumer.** The `SweetSpot` call at `0x43E3E4` receives local 0
through `0x4B0CEA`, then forwards it at `0x43E3F4` to `0x43E0B0(unit, out, piece)`
(stdcall, `ret 12`). Unlike `0x43DEF0`'s ordinary position query, `0x43E0D1..0x43E0E4`
indexes the posed model without a bound. A checked seven-byte entry detour now
validates the full signed result against model+0 before calling the original body.
Its trampoline replays `sub esp,8; mov eax,[esp+0x14]` and resumes at `0x43E0B7`.
A generated `SweetSpot` returning piece `0x100000` was driven through real targeting:
the script-specific diagnostic refused the game, with no crash report.

**Post-load rejection owns native metadata too.** `0x4B2499` registers the blob pointer
and checksum in the native tree rooted at `0x51FBC0` before relocation finishes.
Disassembly of `0x4B2850..0x4B2895` identifies its header pointer at tree+4, root at
header+4, child links at node+0/+8, key at node+0xC, and nil sentinel at `0x51FBBC`.
The script destructor `0x4B2540` removes the registration before its final free
at `0x4B26D5`. Model rejection occurs after registration, so it calls that whole
stdcall destructor, not the blob-free wrapper directly. Direct freeing retained a
stale pointer key/checksum and leaked the native tree node; no use-after-free through
that tree was established. Malformed input rejected before relocation has not yet
been registered and still uses the raw allocator's free.

**Nonterminal native operations and division.** `0x10009000` dispatches to
`0x4B1153`: two stack pops, one inline piece, vtable+0x28, then PC+=2.
`0x1000A000` dispatches to `0x4B1185`: one inline piece, no pops, vtable+0x2C,
then PC+=2. Neither is native unknown-op termination. `0x4B1510` executes signed
`idiv ebx`; both divisor zero and INT_MIN/-1 must be refused before that instruction.
The production-core tests cover those cases and the two nonterminal instruction shapes.

**Trace lifetime.** Both base destructors, `0x4B06B0` (deleting) and `0x4B06F0`
(non-deleting, called by `0x485E30`), free animations/statics. Mandatory trace
observers at their six-byte entries invalidate any deferred start before either
free. Thus the pending trace no longer relies on `IsBadReadPtr`, a freed block's
vtable, or a one-tick age heuristic. The new dispatcher supplies START/CALL origin
to the trace explicitly because those allocator return addresses now lie in the DLL.

**Measured:** the corrected extended Escalation suite passed 84 assertions, including
nonzero kills, construction and signed/out-of-pool IDs, with native 10001-slot bounds and
no TADR execution. Real ARMCRAWL and CORDECI scripts reached 85 words. All eight getters
passed on two peers of each supported mod, including remote-owner getter 75 returning zero.
Highest-live/recycled IDs and complete feature-specific multiplayer coverage remain open;
the COB note records the exact builds and tests, including full-pool START/CALL behavior.

### COB child allocation caller keys

DISASSEMBLED: opcode `0x10061000` branches at `0x4B1888` to START's
`0x4B18B0`; `0x4B18BB` calls allocator `0x4B08C0`, returning at `0x4B18C0`.
Opcode `0x10062000` is tested at `0x4B1911`; CALL's `0x4B1928` calls the
same allocator, returning at `0x4B192D`. The raised-limits VM uses these two
addresses as semantic diagnostic caller keys around its synchronous allocation.
They identify child starts only; the rewind count exception does not apply to
other allocator callers. See the S4 rewind note for the accepted scope.

The pristine allocator body `0x4B08C0..0x4B0933` has no calls: it bounds the
script index, scans eight fixed records, initializes a free record, increments
`cob+0x53C`, and returns the slot or −1 (`ret 4`). The raised-limits replacement
uses the same synchronous allocation role. Neither executes the child's script
inside the allocation, so the diagnostic context ends before VM execution resumes.

### COB explosion additions and their child flares

DISASSEMBLED: EXPLODE `0x481140` gets the piece's world position through
`0x43E060` at `0x48125A`. Flag `0x400` selects the call at `0x4812CE`, returning
to `0x4812D3`: explosion add `0x420A30(point, sequence, 2, 0)`. The sequence
comes from `main+0x147FF`. The add checks its pool capacity at `0x420A42`, then
its fourth argument at `0x420ABE..0x420AC4`; for zero, it compares the point's
signed y word with the sea-level byte `main+0x1427F` at `0x420ACC..0x420AD8`.
Only a point above water reaches its single flare call at `0x420AE1`, returning
to `0x420AE6`. Thus each admitted add can emit at most one such flare. The
raised-limits build relocates the explosion count and records to `s_expl`;
`main+0x1491B` is not that build's active pool count.

MEASURED 2026-10-03, retained naval recording, GameTime [661,2701): the
`0x4812D3` parent additions number 201 live and 197 in playback; their child
flares number 33 and 30. A repeated playback produces the same counts. A
probe at the parent call sees 197 points, 30 above sea level 75, matching the
30 child flares. A fresh live/playback pair instead has 198 additions and
41 qualifying points on each side, with 41 flares each. Its point triples
match except for one small above-water difference. The retained run therefore
has upstream addition-count variance as well as a child-count difference;
the measurements do not identify which missing additions supplied the three
missing flares. The existing parent count band accepts four, while the child
count band rejects three. The owner-approved comparator allowance bounds only
this child key by its already-accepted parent difference, with matching direction
and at most one child per parent on each side; both count pairs are reported.
This policy does not identify the individual missing parent calls.
