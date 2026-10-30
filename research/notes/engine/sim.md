# The simulation step and its clock

The sim tick, its rate, the step's phases and counts, and how a level starts and ends.

*Part of the [engine map](../exe-reverse-engineering.md). A function's entry is the section whose heading names it — the [address index](address-index.html) lists them: a new fact about a function extends its entry, in place; a new section is for an area nobody has mapped yet.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### Seating a level: the SKIRMISH Start, the seat fill, the start commanders and the two end tests [DISASSEMBLED + MEASURED 2026-09-29]

How a skirmish fills its player records before the loader runs, what the loader creates for each
seat, and the two tests that end a level. Read and measured for the demo recorder's puppet host;
the measured runs and what they mean for the design are in the
[state design evidence](../tadr-port/demo-recorder-state-evidence.md#the-puppet-host-measured).

- **The SKIRMISH screen's Start branch** (`0x47AED2..0x47B0BC`, game thread; `ebx` = 1 from
  `0x47AEEB`) sets the player count, the word `main+0x2A3C`, to the AI entries plus one
  (`0x47AF39..0x47AF58`); reads the map's `.ota` through `0x435A20` with the name at
  `[main+0x29A0]+0x11C` (`0x47AF77`; a failure shows the string at `0x50842C`); refuses unless one
  of the first `main+0x38D81` settings entries is an AI (controller 2, `0x47AFB7`) and one a human
  (controller 1, `0x47AFD6`), and unless the count is within the map's player maximum `0x437300`
  (`0x47AFDE..0x47AFFA`, string `0x5083FC`); calls `0x479760` (`0x47B035`, string `0x5083C8` on a
  nonzero answer) [role not traced]; recounts `+0x2A3C` as the AI plus human entries
  (`0x47B04C..0x47B08B`); then calls the seat fill `0x47A760` (`0x47B092`), `0x41DA30`, `0x430F00`,
  stores 2 at `main+0x2BC0` and changes state through `0x491C80(0x14)` (`0x47B097..0x47B0B0`). The
  loader thread comes after it.
- **The CD check `0x41D6A0(disc)`** (stdcall, `ret 4`; AL nonzero when the disc is found) **never
  refuses in this exe.** Its first test reads `0x50289C` and, when it is nonzero, returns `'.'`
  (`0x41D6A0..0x41D6BD`); the dword is 1 in `pristine/TotalA.exe.pristine` and no instruction
  writes it (its four references, `0x41D4C0`, `0x41D6A0`, `0x41D7B0` and `0x41D8A0`, all read).
  Its twenty callers are `0x41D7C4`, `0x41EF30`, `0x41F6A8`, `0x41FB7E`, `0x425F11`,
  `0x425F1C`, `0x426108`, `0x426113`, `0x449C73`, `0x44AF65`, `0x46038F`, `0x4603DB`,
  `0x4775C0`, `0x477612`, `0x477724`, `0x477B7D`, `0x478D09`, `0x47AEF1` (the Start's),
  `0x4924DB` and `0x49252E`; no pointer to it is stored [DISASSEMBLED 2026-10-02: every `call`
  and `jmp` to it in the exe, and its address in no data]. The replay's
  Watch calls it as the Start does, so its refusal notice (`watch_refuse_engine`,
  `tagpu_replay.c`) is reached by no game on this exe.
- **The skirmish settings** `[main+0x29A0]`: ten entries of `0x18` bytes, `+0` the controller
  (0 off, 1 human, 2 AI), `+4` the side, `+8` the alliance group, `+0xC` metal, `+0x10` energy,
  `+0x14` the colour (read by the fill and the seat setup below). Past them, the loader's type-2
  branch (`0x4973C1..0x497457`) copies `+0x108` into `ActiveCommanderDeath` `main+0x37EF6` and bit 0
  of `+0x10C`, `+0x110` and `+0x114` into bits 0, 1 and 2 of the LosType word `main+0x14281`, the
  bits the `Mapping`, `LOS` and `LOSType` console commands toggle; `+0x118` nonzero means fixed
  start positions (`0x49790E`); `+0x11C` is the map name.
- **The seat fill `0x47A760`** (no arguments, plain `ret`; callers `0x47B092` Start, `0x499825` in
  the restart path `0x4997A0`, which tears the level down and re-reads the map first, and
  `0x49757C` in the loader's saved-game path) walks settings entries 0 to `main+0x38D81` − 1, the
  SKIRMISH screen's seat count: 4 by default (`0x4307EA`; also stored at `0x4307CF` and
  `0x4307DD`, and raised by the saved-game path to the number of controller-1/2 entries,
  `0x497542..0x497576`). **That count is why a skirmish seats four players.** Controller 1 copies the
  entry's colour and side into PlayerInfo `+0x96`/`+0x95`, calls `0x464290(seat, 1)` and stores the
  seat in `main+0x2A43` and `main+0x2A42` (`0x47A798..0x47A7DE`); controller 2 does the same with
  type 2 and leaves those two bytes; anything else calls `0x464290(seat, 0)`. Then a human or AI
  record is marked allied (`+0x108 + j` = 1, `0x47A890`) to every active entry j with the same `+8`
  group other than group 5 (`0x47A83C..0x47A89E`). The demo recorder writes the two bytes into its
  header's RT_OPTIONS, `main+0x2A42` (controlled) as byte 18 and `+0x2A43` (viewed) as byte 19
  (`tagpu_recorder.c`, READ), which is where a takeover's fork names the seat it was played as:
  the takeover's `tk_live` stores that seat in `+0x2A42`, then `tk_end` arms the fork, whose
  header `header_build` writes at its first capture point (`tagpu_replay.c`,
  `tagpu_recorder.c`, READ). MEASURED 2026-10-23 by the forks lane's `hud state` check on
  `battle`: forks 2, 4 and 5 each name the player taken over as.
- **The seat activation `0x464290(seat, type)`** (stdcall, `ret 8`; callers `0x47A7C7` and
  `0x47A81E` in the fill, `0x450C29` in the join `0x450A10`, and `0x496DC2`/`0x496DCB` in `0x496DB0`,
  which seats 0 as type 1 and 1 as type 2 and sets `+0x2A3C` to 2 [role not traced]) zeroes the
  record's five 11-byte blocks at `+0x108`, `+0x113`, `+0x11E`, `+0x129` and `+0x134`
  (`0x4642BC..0x464329`); stores `+0x73` = type and, for any type but 3, PlayerInfo `+0x94`
  (`0x46432C..0x46433A`, as `0x463C60` does); sets its own bytes `+0x108 + seat` and
  `+0x113 + seat`, `+0x74` = 0 without a free, `+0x146`, `+0x147` and `+0x148` = seat, the first
  dword = 1, `+0x13F` = 5, `+4` = seat, `+0xC` = 0 and `+0x22` = 0, and clears PlayerInfo `+0x9B`
  bit `0x20` (`0x464340..0x46438C`); for type 1 or 2 it sets PlayerInfo `+0x99`
  (`0x46439D..0x4643BC`); it names the record by game type and type (`0x4643C3..0x464494`) and
  copies the name `+0x2B` to `+0x49` (`0x464497..0x4644B8`). It takes any type as given: type 3 for
  all ten records gives records that the level init, the unit tick and the players phase accept
  (MEASURED, below).
- **The players reset `0x4644D0`** zeroes all eleven records, the ten seats and index 10's, except
  their PlayerInfo pointer `+0x27` (`0x4644D7..0x46450F`), sets `main+0x2A42` and `+0x2A43` to 0,
  and gives every record its own alliance bytes and a default name (`0x464511..0x4645C3`); its
  callers are `0x42734F`, `0x427D73` and `0x4518AC`. A record the fill
  does not reach keeps a zero first dword, which every loop over the players skips.
- **The loader creates a commander for every seat of type 1, 2 or 3.** After the level init
  (`0x497581`), a new skirmish (`0x435100` = 2 and no saved game at `main+0x38D6B`,
  `0x4978DA..0x497902`) calls the seat setup `0x496EE0(seat, start)` for each active record with
  `+0x73` 1–3 and `+0x146` ≠ 10: at start position = seat when `[main+0x29A0]+0x118` is set
  (`0x497918..0x497972`), otherwise at a position from a list shuffled with `0x4E4870`
  [INFERRED: the CRT `rand`] (`0x497979..0x497ADC`). `0x465E30` follows (`0x497ADE`).
- **The seat setup `0x496EE0(seat, start)`** (stdcall, `ret 8`; its only calls `0x497963` and
  `0x497ACD`) copies the entry's side and colour into PlayerInfo `+0x95`/`+0x96`
  (`0x496F08..0x496F3C`), sets `+0x149` bit 0 (`0x496F5D`) and the base storage `+0xDC`/`+0xE0` to
  max(entry `+0x10`/`+0xC`, 200) (`0x496F64..0x496F97`), takes the map's start position through
  `0x437320(&pos, start)` (`0x496FA9`; a failure formats the string at `0x509534` and shows it
  through `0x4B6290`, `0x497009..0x497021`), the side's commander type through `0x488B10`
  (`0x496FDC`), and creates it with `0x485F50(seat, type, pos, 1, 1, 0)` (`0x497002`); for the
  controlled player `main+0x2A42` it moves the camera onto it (`0x41C4C0`, `0x497038..0x497060`).
  A type-3 owner's create takes a proxy mover (`0x43DC00`, *Replay state design* above).
- **The two end tests** are methods of the objective object `[main+0x391ED]`, run on the controlled
  player's 30-tick pass of the players phase (`0x4650C9` and `0x4650E1` in a campaign, `0x46516D`
  and `0x46586F` otherwise; `0x4576C5` calls victory too [network, not traced]). Both answer 0
  while the object's `+0x88` is 0 (`0x490239`, `0x49036B`). In a skirmish **victory `0x490230`**
  answers 1 when every other record that the controlled player's `+0x108 + i` does not mark allied
  has no live unit, the word `+0x144` (`0x4902A3..0x4902E3`), and **defeat `0x490360`** answers 1
  when the controlled player's `+0x144` is 0 (`0x4903F0..0x490414`, a network game alike), or,
  while `[0x511DE4]` is set, once GameTime passes a timer it draws at 9000 ticks or more
  (`0x49037C..0x4903C9`) [the flag's role not traced]; a campaign runs its objective list. Outside
  a campaign defeat is tested first (`0x46516D`) and victory only when defeat answers 0 or the
  controlled player carries the watcher bit (`0x465154`, `0x465174` → `0x465864`). **Neither
  reads a record's type**, so a type-3 controlled player loses and wins like a local one (MEASURED,
  below). A firing test counts `main+0x39239` down over the passes and then sets the game-over bits
  of `main+0x3923B` (*The Deathmatch respawn*, below).
- **Arming and disarming the objective.** The `.ota` reader `0x435DA0` destroys the old object
  (`0x48DFB0`, then the free `0x4B4F20`, `0x435DBC..0x435DCE`), allocates a new `0x8C`-byte one
  (`0x435DD6..0x435DDB`), arms it through `0x48DF90` (`0x435DE9`; `+0x88` = 1, `+0x40` = `+0x84` =
  0) and stores it at `main+0x391ED` (`0x435DFB`); `0x48DF90` has no other caller. `0x4904B0`
  (thiscall, `mov [ecx+0x88],0; ret`) disarms it; its callers are the `Kill` console command after
  it has killed every unit (`0x4164CD`, and `0x4164EF` on the command's other arm) and `0x488547`,
  when `[main+0x391E9]+0xDB0` counts nothing [INFERRED: a campaign mission without objectives]. A
  skirmish reads the `.ota` in Start's branch (`0x47AF77`), before the seat fill, and its loader
  branch reads it no more; the network loader does (`0x4972D6`), as do the restart path (`0x499820`)
  and the saved-game loader (`0x4927A5`). **A disarm made in the seat fill therefore holds for the
  whole skirmish level.**
- **MEASURED 2026-09-29** (a temporary probe build, not landed; Wine, a private display, Core Prime
  Industrial Area, ten start positions): after the stock fill, `0x464290(s, 3)` for s = 0..9 with the
  entries 4–9 given a side and colour, `+0x2A3C` = 10 and `main+0x2A42` = `+0x2A43` = 0 loaded a
  level whose ten records read active, type 3, `+0x146` = s, blocks in seat order (slots
  `1 + 1500·s`, 1500 each), no `+0x74` and no `0x5119C0` object, and a sight grid each; the loader
  created ten commanders at ten start positions. With every unit dead, seat 0's included, the level
  went to ENDMSN reading **DEFEAT**, and with every unit of seats 1–9 dead and seat 0's alive,
  **VICTORY**, both with ten rows; with the
  objective disarmed through `0x4904B0`, in play or in the seat fill, neither came, and a level with
  no unit at all ran on (68 s measured) and took new units. With `ActiveCommanderDeath` set to 1, a
  type-3 commander's death cleared its `+0x149` bit 0 and killed nothing else: the kill-all is for a
  local owner only (*`Send_UnitDeath 0x4864B0`*, below, `0x48667E..0x4866B3`).

### The demo recorder's tick boundaries: the sim step `0x495490` and its callers [DISASSEMBLED 2026-10-01]

S3c-1 of the [skirmish slice](../tadr-port/demo-recorder-skirmish-slice.md#s3c-1-one-writer-for-the-tables-and-the-round-trip)
writes a puppet host's declared tables at every tick boundary and takes frame dumps and round trips
between two steps; this is the step those boundaries sit in. Read from `pristine/TotalA.exe.pristine`
(`objdump -d`, the PE's own section map: `.text` at `0x401000`), the stub against
`tagpu/ddraw/src/tagpu_patches.c`.

- **`0x495490(int pump)`**, `stdcall`, one argument (`ret 4` at `0x4956B0`). It reads the iteration
  count `main+0x38A3B` (`0x495497`) and, when it is 0, skips the loop (`je 0x495674` at `0x4954A2`) to
  the once-a-call tail; the argument goes to `edi` (`0x4954A8`), the count to `ebx` (`0x4954AD`), and
  the back edge is `dec ebx; jne 0x4954B2` (`0x49566C`, `0x49566D`).
- **Each iteration, in order.** GameTime `main+0x38A47` is read (`0x4954B7`), incremented (`0x4954BD`)
  and stored (`0x4954C0`) — its only write inside the step. `test edi,edi` (`0x4954BE`) and `je
  0x4954ED` (`0x4954C6`) skip, for a zero argument, both the network pump `0x453D40` (`0x4954C8`) and
  its timing accumulator (`0x4B6560` at `0x4954D8`, `main+0x38D85 +0x2C`). Then the unit tick's call
  `0x4954ED` (`E8 3E 58 FF FF`, returning to `0x4954F2`), and the rest: `0x49B720` (`0x495513`),
  `0x420F30` (`0x495539`), the players phase `0x464F80` (`0x49555F`), `0x424050` (`0x495585`),
  `0x415B30` (`0x49558A`), `0x490C40` (`0x49558F`), `0x437DE0` (`0x495594`), `0x41CA10` (`0x495599`),
  `0x471EB0` (`0x4955BF`), `0x466580` (`0x4955E5`), each timed through `0x4B6560`
  ([*The demo recorder's globals, projectiles and seek*](recorder.md#the-demo-recorders-globals-projectiles-and-seek-disassembled-measured-2026-10-01)
  names them). Last, the argument gates two calls again: `test edi,edi` at `0x49560B` (`je 0x49566C`)
  and a nonzero `[0x506DBC]` (`0x49560F`) before `0x457D30` (`0x49563A`, its argument the controlled
  player's block, `main+0x2A42` × 0x14A `+0x1B63` + the index) and `0x4618A0` (`0x495646`, `this` =
  `0x513000`, argument 0) [names not read], so a skirmish skips those too. Once a call, loop or no loop: `0x428BD0`, `0x428BE0`, `0x428BF0` (`0x495674`..`0x49567E`,
  not read), the chat expiry `0x463EF0` (`0x495683`) and the temporary sight records' expiry
  `0x482130` (`0x495688`).
- **B2's stub at `0x4954ED`** (`tagpu_patches.c`, the stacked-aircraft installer's `tick` block):
  `pushfd; pushad; call tagpu_replay_capture; call tagpu_rec_capture; call air_rebuild; popad; popfd;
  call 0x48AD30; pushfd; pushad; call tagpu_replay_step; popad; popfd; ret`. The unit tick takes no
  argument and returns with a plain `ret` (`0x48B080`), so the stub's call is the site's own call.
  `tagpu_replay_capture` is the **capture point**'s write on a puppet host (the globals, the pool,
  the explored map and the declared tables, through `recorded_put`), preceded at the level's
  first iteration by the first step's units (the checks, the level's own units, image T, the
  wind's seed), `tagpu_rec_capture` the
  recorder's capture (and the armed dumps), `air_rebuild` the airborne pool, and `tagpu_replay_step`
  the replay's step, which writes the next iteration's images after the unit tick — all but the
  features, which wait for the step's late phase (next bullet).
- **The step's phases, at `0x495513` and `0x495539`** (`tagpu_patches.c`, `fix_step_phases`, in the
  fail-closed table): the step's calls of the projectile tick `0x49B720` (`E8 08 62 00 00`) and of
  the explosion tick `0x420F30` (`E8 F2 B9 F8 FF`) each call a stub `pushfd; pushad; push phase;
  call tagpu_rec_phase; add esp,4; popad; popfd; call <callee>; pushfd; pushad; push phase; call
  tagpu_replay_phase; add esp,4; popad; popfd; ret`, phase 1 and 2. Both callees take no argument
  and end in a plain `ret` (`0x49BE5C`, `0x421147`), and the site pushes nothing before either call
  and reloads what it uses after it (`mov edx,[0x511DE8]` at `0x495518` and `0x49553E`), so the
  stub's call is the site's own. The recorder notes the phase a feature root is made in (0 from
  the capture to the projectile tick, 1 inside it, 2 from the explosion tick to the step's end,
  the feature tick `0x424050` included); a puppet host makes each recorded root at its own phase
  ([*the counts and the step's phases*](#the-demo-recorders-counts-and-the-steps-phases-sinks-impacts-emitters-and-the-images-gates-map-disassembled-measured-2026-10-01)).
- **The callers: two**, both in the in-play frame callback `0x496790` (a scan of `.text` for `E8`/`E9`
  rel32 to `0x495490` finds `0x49680C` and `0x49693E`; no jump, and the file holds the address as no
  stored dword). `0x49680C` passes 1 (`push ebx` at `0x49680B`, `ebx` = 1 from `0x4967CF`) on the
  network branch (`main+0x2A44` bit 0, `0x4967E7`), after the scheduler `0x495230` (`0x4967F3`) and a
  nonzero count (`0x496803`). `0x49693E` passes 0 (`push 0` at `0x49693C`) otherwise: after the test
  of `main+0x37EBE` bit 0 (`0x496918`, `jne 0x49697B`), the pause test `main+0x38A51` (`0x496920`,
  `jne 0x496969`), the scheduler (`0x496928`) and a nonzero count (`0x49693A`). So **a skirmish never
  runs the pump** inside the step.
- **The two tick boundaries.** The *capture point* is B2's stub, inside step k after GameTime became
  k and before the unit tick: the world holds iteration k. The *frame visit* is Impure's hook before
  the in-play draw call `0x4969CD`, after `0x495490` returns (and on paused frames, which skip it):
  GameTime is k and the simulation holds step k's result. Between the step and the hook run the
  step's once-a-call tail — after a call's *last* iteration only, so when one call runs several
  steps the earlier ones' frame visits never happen and their tail work lands after step k — and,
  in the frame callback, `0x495E90` and `0x41CE90` (`0x496971`, `0x496976`, skipped by the jump at
  `0x49696F`), `0x48BAE0` (`0x49697B`) and, on one branch, `0x41B2E0` (`0x4969B4`) [names not read].
  A frame dump there is not iteration k+1: between it and the next capture point the rest of
  `0x496790` runs — the draw `0x468CF0`, input on
  a live host, the next frame's scheduler `0x495230`, which stores `main+0x38A37`, `+0x38A3B`,
  `+0x38A3F`, `+0x38A4D`, `+0x38A4F` and the pause word `+0x38A51` — then the next step's GameTime
  increment (its pump never runs in a skirmish). A dump carries both images whole, so those bytes
  differ from the capture point's; two dumps taken at one frame visit (the round trip's A and B)
  share all of it. A frame dump's `RT_FRAME` is that GameTime, and two frame dumps of one `RT_FRAME` saw no
  step between them, since a step raises GameTime exactly once (`0x4954C0`); its other writers are
  the shell's network clock `0x44A696`, the level init, the loader and the load wait (the S3b table),
  outside play [INFERRED from where that table places them], and Impure's seek, which sets it to T−1
  between two steps. On a puppet host `tagpu_replay_step` has already put iteration
  k+1's images in step k, which is why a rebuild at the frame visit is in place there and would not
  be on the recording machine.

### The demo recorder's counts and the step's phases: sinks, impacts, emitters and the images gate's map [DISASSEMBLED + MEASURED 2026-10-01]

S3c-2 of the [skirmish slice](../tadr-port/demo-recorder-skirmish-slice.md#s3c-2-counted-on-puppets-and-the-images-gate)
counts the engine's roots and sinks in every dump, compares a live game's counts with a playing's,
gates the two images on a null twin and scans dumps for addresses recorded as values. This is what
that rests on and what the work read on the way. Read from `pristine/TotalA.exe.pristine`
(`objdump -d -M intel`); callers are direct `E8`/`E9` rel32 plus a scan of the image for the
address as a stored dword, so a "none" is a none for those two.

**The projectile's collision test `0x49B090(weapon, proj)`**, `stdcall`, `ret 8`, one caller
(`0x49BD88`, in the projectile tick `0x49B720`). It looks the projectile's position `proj+4` up as a
plot cell through `0x4815A0` (`0x49B09F`); with no cell it sets the record's done bit `+0x69` bit 1
and returns (`0x49B0F8`), first moving a camera that follows it to the remembered position
`main+0x1433F`. Then, in order, each a `call 0x499EB0(proj, victim)`, every one but the first
ending the test:

| Return address | What it hits | Test |
|---|---|---|
| `0x49B1AE` | an interceptor's quarry `proj+0x56` (victim 0) | a distance test against the weapon's `+0xD6` (`0x49B106..0x49B1A4`); the tests below run after it whatever it answered |
| `0x49B218` | the cell's unit slot A, `[cell+0]` (`0x49B1C7..0x49B213`) | a unit of another owner than `proj+0x66` whose `+0x6E` plus its def's `+0x16E` is above the projectile's altitude `proj+8` |
| `0x49B27A` | slot B, `[cell+2]` (`0x49B222..0x49B275`) | the same owner test, the altitude between `+0x6E` + def `+0x162` and `+0x6E` + def `+0x16E` |
| `0x49B3D2` | the cell's feature, the ground or the water (victim 0, `0x49B28A..0x49B3CD`) | skipped for a weapon with `+0x111` bit 14 (`0x49B294`). The feature: the cell's `+8` index below the def count `main+0x14253` (`0xFFFE` resolved to its anchor through `+0xA`/`+0xB`, the other values from `0xFFFB` none), its def `[main+0x1426F] + idx·0x100`, hit when the def's height `+0xFA` plus the cell's height byte `+6` is above the projectile's altitude word `+0xA`, once a cell (`+0x5A`/`+0x5C` hold the last one). Else the ground: the cell's `+6` above the altitude bounces a weapon with bit 15 (`+0x20 = −(v >> 2)`) and hits otherwise; else the water: no bit 16, the altitude below the sea level `main+0x1427F` and `[[main+0x391E9]+0xD48]` zero |

Slot A is the cell's `+0`, slot B its `+2`, each a unit index scaled by `0x118` into
`main+0x14357`; the unit's def is its `+0x92`. Slot A hits anything below the unit's top, `+0x6E`
plus def `+0x16E`; slot B only inside the band from `+0x6E` plus def `+0x162` to `+0x6E` plus def
`+0x16E` (`jl`, `jg`, `0x49B265`, `0x49B271`). Both read only the projectile's position after its
step, so a projectile that starts a few pixels elsewhere can meet another slot or none: a shot
replayed from a start the live build did not use moved battle-los's slot-B count
([*What a shot's packet says of its start*](recorder.md#what-a-shots-packet-says-of-its-start-0x49d9c0-0x49d270-disassembled-measured-2026-10-09)).

`0x499EB0`'s other callers are the area damage `0x49A120` (`0x49A764`), the `0x0E` receiver
`0x49AF90` (`0x49AFEB`), the destructor's explosion `0x49B000` (`0x49B074`, returning to
`0x49B079`) and the projectile tick (`0x49BA6F`, `0x49BACC`, `0x49BC70`): ten in all. Inside the
impact the sound `0x47F300` is called at `0x499FD8` and `0x499FF2` (returning to `0x499FDD` and
`0x499FF7`), then a weapon with `+0x111` bit 10 makes grey smoke `0x472810(pos, 9)` at `0x49A007`
instead of the explosion `0x420A30` at `0x49A016` (returning to `0x49A01B`).

**Why the feature roots need the step's phases** (MEASURED 2026-10-01, forest-circle, live game
against its playing, the window [661, 2941)). Before them, the playing made every recorded feature
root at the replay step, after the unit tick and so before the host's projectile tick; a root the
recording machine made later in its step — a tree an impact felled, the feature tick's burn — took
the tree away before the host's projectiles met it. `0x499EB0` from `0x49B3D2` counted 134 on the
live game and 111 on the playing, and the explosion `0x420A30` from `0x49A01B`, the sound `0x47F300`
from `0x499FF7` and the flare `0x472630` from `0x420AE6` were short by the same 23; every impact the
playing missed had the tree in the recording machine's cell and its successor in the host's, at a
projectile record equal on both entering `0x49B090` but for its unit pointers `+0x4E` and `+0x52` (`tagpu_replay.c`, *THE PHASES*). With
the phases (the stubs on `0x495513` and `0x495539`, [*tick boundaries*](#the-demo-recorders-tick-boundaries-the-sim-step-0x495490-and-its-callers-disassembled-2026-10-01))
and the cells' unit slots of the next image written before the host's projectile tick — slot A and
B are what `0x49B1C7..0x49B275` test, and the recording machine's unit tick has just moved them —
the same counts over the final nine-fixture run are live/playing 121/121 (forest-fire), 68/68
(forest-circle), 2491/2491 (battle) and 510/511 (battle-los).

**The sinks** (`tagpu_recorder.c`, `CNT_SINKS`): count-only observers, keyed by the return address
`[esp]` at entry, each byte-matched at attach. Prologues as matched:

| Entry | Stolen | What |
|---|---|---|
| `0x420A30` | `A1 E8 1D 51 00` | the explosion add (`ret 0x10`) |
| `0x4B08C0` | `56 8B 74 24 08` | the COB thread allocation, `thiscall(cob, idx)`: an index refused when below 0 or at `[[cob+8]+4]` or above (`0x4B08C5..0x4B08D0`), and the three wrappers then start nothing (`0x4B0B0F..0x4B0B22`, `0x4B09A6..0x4B09AF`, `0x4B0C4F..0x4B0C54`); Name2Index `0x4B07C0` reads the names at `[[cob+8]+0x1C]` (`0x4B07C0..0x4B07D3`), which the sink hashes (FNV-1a, at most 31 bytes) as its key's aux |
| `0x47F0C0` | `8B 4C 24 04 83 EC 14` | a sound entry; `0x47F1A0` calls it at `0x47F1FA` on every call, so the call returning to `0x47F1FF` is not counted again |
| `0x47F1A0`, `0x47F210` | `8B 0D E8 1D 51 00` | sound entries; `0x47F210` is one: the function before it ends `ret 8` at `0x47F202` and `0x47F205..0x47F20F` is padding |
| `0x47F300` | `A1 94 E6 51 00` | the sound the impact plays |
| `0x49C740` | `53 8B 5C 24 10` | the projectile init |
| `0x49CDE0`, `0x49CC20`, `0x49C9C0` | `8B 0D E8 1D 51 00` | the three shot paths |
| `0x499EB0` | `53 55 56 8B 74 24 10` | the impact |
| `0x471FD0`, `0x472330`, `0x472430`, `0x472530`, `0x472630`, `0x472810`, `0x4728F0`, `0x4729D0`, `0x472C50` | `A1 E8 1D 51 00` | particle emitters |
| `0x472AB0` | `83 EC 0C 8B 44 24 10` | the fire emitter |

**The particle emitters account for the pool's allocations call for call.** `0x470EB0` (the pool
`0x51E610`'s allocation) has exactly 20 call sites, one in each emitter, live or dead
([*The raised effect pools*](weapons.md#the-raised-effect-pools-every-site-tagpu_limitsh-rewrites-disassembled-2026-09-23-objdump-of-the-pristine-build-measured-2026-09-23) lists the twelve live ones). In every emitter the only
branch between its entry and that call is the test of the byte `0x51E608` (e.g. `0x47281E..0x472832`):
one allocation a call while it is 0, none while it is set, an empty pool included (the call returns
0). `0x51E608`'s one writer is `xor byte [0x51E608],1; ret 4` at `0x419550`, the console command
"SFX" (its pointer at `0x501F34` in the command table): a toggle of one machine. `0x472AB0` draws the
CRT `rand` four times (`0x472ACF..0x472B3A`) before that test. So the recorder counts the ten
emitters with a caller besides the two nano emitters (which are roots), keyed by their callers, and
not `0x470EB0`. Their callers, classified:

- **COB `EMIT_SFX`, `0x480EB0`** (the COB object's vtable slot `+0x30`, at `0x4FD6C8`; EXPLODE
  `0x481140` is beside it at `0x4FD6CC`) emits only when `UnitInPlayerLOS 0x465AC0` answers for the
  **viewed** player's record (`main+0x2A43`, `0x480EC5..0x480EEC`), so its count follows the view as
  well as the simulation. The type dispatches at `0x480FDE` (types 0–5 through the table at
  `0x481128`): 0 and 1 fire `0x472330` (returning to `0x481015`, `0x481034`), 2–5 the wake `0x472430`
  (`0x481051`, `0x48106E`, `0x48108B`, `0x4810A8`), `0x101` grey smoke `0x472810` (`0x4810BE`),
  `0x102` black smoke `0x4728F0` (`0x48111D`), `0x103` bubbles `0x472530` at sea level (`0x481107`).
- **The explosion draw.** `0x421550` is called from the explosion draw `0x420B00` (`0x420B18`; the
  draw from DrawGameScreen at `0x469B2C`) and from `0x4211A0` (`0x4211B3`), which has no caller; it
  emits a flying piece's smoke `0x472810` at `0x421583` and its fire `0x472AB0` at `0x4215AA`
  (`0x472AB0`'s only call). So those two follow the frame rate, not the tick.
- **The simulation.** `0x472630` (a flare) from the explosion add `0x420A30` (`0x420AE1`) and the
  corpse `0x486360` (`0x48644B`). The add emits it when its 4th argument is 0 and the point's y
  word `+6` is above the sea level byte `main+0x1427F` (`0x420ABE..0x420AD8`); the recorder keys it
  by the add's caller, read at `[esp+0x20]` of the flare's entry, because the add's frame there is
  fixed (push edi `0x420A35`, esi `0x420A55`, ebx `0x420A58`, the four arguments
  `0x420ADA..0x420AE0`, and between them only `0x4B8B30`, `ret 0xC` on both exits `0x4B8B6B` and
  `0x4B8B7E`) — DISASSEMBLED. Debris decides the argument: the flying-piece tick `0x4213B0`'s water
  path passes 1 (`push 1` `0x421424`, returning to `0x421445`) and its ground hit 0 (`0x4214AD`,
  returning to `0x4214D9`); the record tick `0x420F30` passes 1 in water (`0x421046`, `0x421053`)
  and 0 on ground (`0x421090`), both returning to `0x4210A1`. EXPLODE `0x481140` draws the
  simulation RNG six times a piece (`0x48116F..0x4811B9`: three spins, vx = (20 − rand(40)) << 14,
  vy = rand(10) << 16, vz) and gives it 900 ticks (`0x4811C3`); the shatter `0x421700` draws six a
  polygon (`0x421851..0x4218D2`) — DISASSEMBLED; `0x472810` from the feature tick's burning feature (`0x4243CF`),
  the impact (`0x49A007`) and the projectile tick (`0x49BC80`, `0x49BDC1`); the trail `0x4729D0` from
  the three shot paths (`0x49CC07`, `0x49CDC3`, `0x49CFE6`), so a received shot emits its trail too;
  `0x471FD0` from the Teleport order handler `0x406AA0` (`0x406B9F`); and `0x472C50` from the
  feature spawn `0x423C50` (`0x423FE3`), whose callers include the map and save loaders and the
  console's feature command.

**COB's engine writes, observed on a puppet** (`tagpu_puppet.c`, *THE COB WRITES ON A PUPPET*):
count-only observers on the state setter `0x48B090` (`51 8B 44 24 0C`), the yard `0x47DAC0`
(`53 8B 5C 24 0C`) and the carry link `0x48AAC0` (`8B 4C 24 04 83 EC 08`), counting a call whose
return address is one of COB's five write sites: SET's `0x48B090` calls at `0x480B52` (mask 1,
returning to `0x480B57`) and `0x480BEC` (mask 2, `0x480BF1`), its `0x47DAC0` call at `0x480BAE`
(`0x480BB3`), ATTACH's `0x48AAC0` at `0x4813A6` (`0x4813AB`) and DROP's at `0x481422` (`0x481427`).
`0x48B090` is `thiscall(unit; mask, on)`: it computes `on ? old|mask : old&~mask` of `+0x10E`
(`0x48B09A..0x48B0D4`), stores it, and returns early when it is unchanged (`0x48B0D6..0x48B0DE`) —
which is why the root count of `0x48B090` counts only a call that changes the byte.

**The roots counted** are the recorder's existing observers: the create's exit `0x4861B3`, the
destructor `0x4866D0` (only for a slot the destructor's own alive test, `+0x110` bit 28 at
`0x486706`, passes), the build done `0x41B8D0`, the state setter `0x48B090`, the replies `0x47F780`
and `0x47F850`, the nano emitters `0x4720D0` and `0x472200`, and the seven feature roots
(`0x424050`, `0x423C50`, `0x4246B0`, `0x423550`, `0x4233A0`, `0x4244B0`, `0x423710`).

**Two memos of the draw** the images gate met: the unit info panel's 60 bytes at `main+0x37E60`,
compared with what the panel would draw (`repz cmpsb` of `0x3C` bytes at `0x46ACDB`) and copied in
when they differ (`rep movs` at `0x46ACEE`), with `+0x37E90`/`+0x37E94` written by the reset
`0x4679A0` (`0x4679C4`, `0x4679CA`, `0x4679F6`); and DrawGameScreen's 33-byte memo of the resource
bar at `main+0x37E3F` (`0x468FC6..0x468FEC`). Neither is read by the simulation; both are `leave`
rows.

**The images gate's map** — what `replay-globals.tsv`'s `leave` rows name, each row with its writers
and evidence; the corrections first:

- **`0x511A40..0x511DCF` is the network statistics**, not GAF or animation state: zeroed by
  `0x415E90`, counted per message type by `0x415EF0` from the transport (`0x451DBB`, `0x451F10`,
  `0x453595`, `0x45361F`, `0x4615C8`), sampled by `0x415FA0`, whose last `GetTickCount` is
  `0x511DD8`.
- **`0x51E710` (120 bytes) is the music's combat ring**, not a profile: 30 slots cleared at level
  load (`0x491A5C`), +5 when the controlled player's unit dies (`0x4869EB`) and +1 per hit by or on
  it (`0x489DE6`) through `0x494FF0`, a slot a second of wall clock (`0x494E90`), which picks the
  battle or the peace music; its index `0x51F2DC`, its sampler `0x51F2F8`.
- **The network object `0x513000..0x51E53B`** (`0xB53C` bytes): static constructor `0x460E20` from
  the initialiser table (`0x50101C`), vtable `0x4FD514`, 11 channels of `0x1044` from `0x513008`
  (each a ring of `0x400` message pointers and a wall-clock resend deadline at `+0x24`, `0x4624ED`),
  the sub-object `0x51E300`, ten `0x34`-byte records `0x51E320..0x51E527`, fields to `0x51E538`. The
  size is proved by the dead `this`-relative constructor `0x4611E0`, whose last store is
  `[ebp+0xB538]`; `0x51E53C` is the next global.
- **`0x51FDB0..0x52B65B`**: the shadow encoder's output count `0x51FDB0`; the Translate table
  pointer `0x51FDB8`; the DirectDraw surface-lock depth and its stack `0x51FE00..0x51FE3A`; the
  polygon routines' scratch `0x51FE40..0x51FF03`; HAPINET `0x51FF04..0x51FF0F`; the CD player and the
  delayed sound `0x51FF10..0x51FFCF`; the SQSH compressor `0x51FFD0..0x526FFF`; zlib `0x527000..0x528993`;
  the Cavedog library `0x528994..0x529EC3` (its memory manager's statistics `0x5289D0..0x528A1F`);
  the C runtime from `0x529EC4` (its small-block heap's first region `0x50E9C8..0x5109E7` lies
  earlier, with its pointer `0x5109E8` and threshold `0x5109EC`). `0x52A4E8..0x52A4F3` is the lock
  the draw code and the mouse thread take; `0x52A4F4..0x52A4FF` is the compressor's own
  (`0x4D0F60..0x4D1655`). No engine code below the last static initialiser `0x4E4140` references
  `0x52A500..0x52B65B`.
- **The main block's GUI**: the desktop GUI is `main+0x519` (`0x4AA858`); its window state
  `main+0x539..+0x558` (GUI `+0x20..+0x3F`: window pointers and a count) has no displacement writer,
  since the GUI's code writes it through `this` (MEASURED forest-fire: the live game held another
  panel at T1, a fresh process other heap addresses, and the paused twin changed `+0x3C`). The GUI
  and input block `main+0x2AA7..+0x2CC6`: all 336 stores whose displacement falls there have the
  main pointer as their base; `+0x2C82` (the mouse record's stamp, copied by `0x4999E7..0x499A04`)
  and `+0x2CB6` (the click time, `0x4994C7`) are wall clock. The graphics object `0x51F320`'s
  `+0x1AE..+0x1B5` (`0x51F4CE`) has no instruction naming it (MEASURED: the live game and the
  playing differ there at T1) [INFERRED: cursor state].
- **`main+0x14353`, the unit tick's count of the units it ticked** (zeroed `0x48AD4A`, incremented
  `0x48ADC2`, read only by the debug line `0x46AAC6`), is a `write` row: the steps write it.

#### The active GUI pointer and the images inventory [DISASSEMBLED + MEASURED 2026-10-04]

The desktop `GUIInfo` is embedded at `main+0x519`; its `+0x18`, therefore `main+0x531`, is the current top `GUIMEMSTRUCT*`. `ApplySelectUnitMenu` reads it at `0x49588B`. In `GUI_Pop 0x4A9660`, `0x4A969B` reads the current top, `0x4A969E` reads that screen's underlying screen, and `0x4A96A2` stores the underlying screen at `GUIInfo+0x18`. It marks the new top active and frees the old screen through `0x4D85A0` at `0x4A96AF`. The GUI pump performs the same inline pop: `0x4AA7CE` reads the top, `0x4AA7D1` reads the underlying screen, `0x4AA7D5` replaces the top, and `0x4AA7E2` calls `0x4D85A0` to free the removed screen. These are heap screen identities owned by the current GUI stack, not stable addresses across processes. See [GUI gadgets](../gui-gadgets.md), §1.2, for the stack layout and other pop paths.

MEASURED on the reference setup in the mapping checkpoint's `secondary-orders` run: live tick 331 has `0x0B0AD4D8`, changing to `0x1168C340` at 451; two paused frame dumps at tick 496 change `0x1168C340` to `0x0B0CC500`. Puppet playback at ticks 331 and 451 has `0x0B0B0560`; repeated playback at 331 has `0x0B0AF920`; fresh-process seek at 331 has `0x0B0AF8B0`. World/radar/events/counts and all three poisoned round trips pass, but the bitwise images inventory reports two bytes at `main+0x531`. Individual address bits can coincide across two heaps and remain equal across a paused pair even while the whole pointer changes, so the gate's bitwise presentation/host tests need not classify every bit of an address alike.

The positive list names these four bytes as the `leave` row `guitop` (the owner's decision, 2026-10-06): the viewer's own screen stack, and an address no other process can use. MEASURED 2026-10-06: `secondary-orders --cover` passes all 25 checks with it, the images gate included. Leaving a raw heap address with the viewer would not provide recorded GUI/input history: that history still needs stable identities and events. This investigation reads existing addresses and dumps; it adds no engine patch or engine write.

**The keyboard ring [DISASSEMBLED + MEASURED 2026-10-05].** `0x4B6220` returns
the graphics object through `0x51FBD0`, set by its initializer `0x4B5990`; the
WinMain setup passes static object `0x51F320`. Its `+0xF2` is a ring capacity,
at most 30 (`0x4C1A60`), `+0xF6` its dword character/key entries, `+0x16E`
the enqueue index (`0x51F48E`) and `+0x172` the dequeue index (`0x51F492`).
`0x4C1A40` clears both; `0x4C1AB0` removes one key, advances/wraps the dequeue
index and returns zero when empty; `0x4C1B00` peeks; `0x4C1B20` appends unless
the next slot equals the dequeue index, then advances/wraps the enqueue index.
The WM_CHAR path calls the append at `0x4B5DEA`; `0x4C1FBE..0x4C21C2` also
translate special keys into that ring. Readers include gameplay keys at
`0x495E9A`, IdleTick's three special codes at `0x4998B6/0x4998D7/0x49991D`, and
GUI input at `0x4AA034` (when each pops: *Who pushes the two rings*, below). On the long CORE upgrade replay's T451/T9181 images, the recording
had both indices 9, playback and fresh seek both 0: equal enqueue/dequeue indices
mean drained input, not a construction counter. The replay exception is
exactly these eight bytes; the owner approved both four-byte leave rows on 2026-10-05.
Enqueue stores are `0x4C1A47`, `0x4C1A87`, `0x4C1B5C`, `0x4C1B66`;
dequeue stores are `0x4C1A4D`, `0x4C1A8D`, `0x4C1ADC`, `0x4C1AE4`.

**Who pushes the two rings, and who pops them [DISASSEMBLED 2026-10-09].** TA's window
procedure is the only pusher: `WM_KEYDOWN` reaches the keydown translator `0x4C1D50` at `0x4B5D8E`
and `WM_SYSKEYDOWN` at `0x4B5DC3`; `WM_CHAR` appends through `0x4C1B20` at `0x4B5DEA`; button
messages push the mouse ring through `0x4C2E30` at `0x4B5F51` (the mouse object, below). A move
and a key-up push nothing. A mouse record is six dwords: x, y, the message's wParam (the buttons
held), a time, the message (stored at `0x4B5F4C`) and a double-click flag. Each push drops its
entry silently when the ring is full, and full is one short of the capacity: the push refuses
when the next write slot equals the read index (`0x4C1D9D`, `0x4C1B3A`, `0x4C2E4A..0x4C2E50`). So
the mouse ring holds 19 records (`0x4C2BD0(20)` at `0x4B5A67`) and the key ring at most 29 (its
capacity clamped to 30 by `0x4C1A60`). The main loop `0x49EB80` dispatches every queued message
before it runs an IdleTick: `PeekMessageA` at `0x49EC47`, and on a message `GetMessageA` /
`TranslateMessage` / `DispatchMessageA` (`0x49ECA0..0x49ECBA`) and back to the top (`0x49ECC0`);
IdleTick `0x499890` (`0x49EC63`) runs only when the queue is empty. So a burst of messages is
pushed whole before anything pops it, and ten clicks (twenty button records) overflow the mouse
ring in one drain.

Every pop is conditional:

- **The mouse ring** has two pops of `0x4C2D60`. The GUI pump `0x4A9FD0` pops through `0x4AB5D0`
  (`0x4AB654`), and only the head record whose point lies inside the top screen's panel (the
  rect `0x4A1680(gadgets, 0, &rect)`, right and bottom inclusive; the test `0x4A1920`) or whose
  wParam is 0 (`0x4AB643..0x4AB64D`); any other it leaves at the head (`0x4AB680`). IdleTick peeks
  the head before the pump (`0x499982`) and after it (`0x49999C`) and pops at `0x4999C4` only when
  the two carry the same message (`0x4999AB..0x4999B5`), a record the pump left; when they differ
  it copies one of them to `main+0x2C76` without a pop (`0x4999CB..0x499A04`, the earlier one for
  `WM_LBUTTONUP`/`WM_RBUTTONUP`). An empty ring's peek and pop copy the current move record and
  move nothing (`0x4C2D73`, `0x4C2DF3`). So wherever IdleTick runs a record waits at most one
  IdleTick, and it runs in the shell as well as in game: a press at (20,460), outside
  `SELMAP.GUI`'s panel (84,12, 494×420), with the button and shift held, was popped and its
  batch done in 0.2 s (MEASURED 2026-10-09, `tacli keys … down:shift pclick:20,460`), and the
  pump would not pop it, so `0x4999C4` did. The one loop that runs the pump without that second
  pop is the message box's: `0x49F9C0(gi, name)` runs, while the top screen's panel keeps that
  name (`0x4FAB50` at `0x49F9EC`), one `PeekMessageA` / `TranslateMessage` / `DispatchMessageA`
  (`0x49F9F8..0x49FA14`), the pump (`0x49FA1B`), the blit `0x4AB0B0`, `0x4C2870` and the flip
  `0x4C63A0` (`0x49FA32`), and its one caller, `0x4437B0`, runs it for `MSGBOX.GUI` (`0x502BD4`)
  after `"Unable to initialize scores reporting."` (`0x505900`, `ReportError`). There a press
  outside the box's panel stays at the head, and every record behind it waits, until the box
  closes.
- **The key ring** (`0x4C1AB0`, 28 call sites). The in-game keys `0x495E90` pop one at `0x495E9A`;
  the frame callback calls them at `0x496971` and skips them while `main+0x37EBE` bit 0 is set
  (`0x496918` and `0x496969` both jump to `0x49697B`; the bit is set at `0x45D002` while an
  in-game GUI screen is up). The GUI pump pops one at `0x4AA034` only when its top screen's
  `+0x18` is non-zero (`0x4AA00F..0x4AA017`); otherwise it peeks (`0x4AA019`) and leaves the key.
  IdleTick peeks at `0x499898` and pops only three codes: `0x7E` at `0x4998B6` while
  `main+0x37F2F` bit 1 is set (then `0x4B5910`), `0xE3` at `0x4998D7` while `main+0x37EBE` bit 0
  is set (then `0x491D70(0)` and the bit cleared, the in-game screen closed *[INFERRED]*), and
  `0xD6` at `0x49991D`, the screenshot (`"%s\screenshots"` `0x5024FC`, `"SHOT"` `0x50966C`).
  The other pops are the shell's and the gadgets' own (`0x41FE73` to `0x427D1A`, `0x4A45FD`,
  `0x4A716A`, `0x4A82FF`, `0x4A9E8A`, `0x4AB765`, `0x4AB9CF`, `0x4C9F5D`), not traced here.

Each popper hands its entry to its handler before the frame draws and flips: in IdleTick the pump
and the pop at `0x4999C4` come before the handler `[eax+0x391F5]` (`0x499A1C`) and the frame
`0x4995B8` → `0x496790`, and the in-game keys pop and handle inside the frame callback before its
draw. That no flip falls between a pop and its handler is *[INFERRED]* from this order, here and
in the message-box loop. The modifier state a handler sees is polled, not carried: `GetAsyncKeyState`
(IAT `0x4FC350`) for `VK_CONTROL` at the head of the keydown translator (`0x4C1D56`) and for
`VK_MENU` in the hotkey query (`0x4C1BA3`). The mouse ring has a flush that handles nothing,
`0x4C2BB0` (zeroes `+0x18E`/`+0x192`), with one call, `0x426636`, in the in-game setup.

**Five message pumps.** Four read `PeekMessageA` out of its IAT slot into a register and loop on
it — `0x47C6D4`, `0x49EB74` (the main loop above), `0x49F9C4` (the message-box loop), `0x4C9E8E` — and
`0x4DA331` calls `GetMessageA`. USER32's IAT starts at `0x4FC2BC`, member *n* at `0x4FC2BC + 4n`:
`PostMessageA 0x4FC2D0`, `PeekMessageA 0x4FC2D4`, `GetMessageA 0x4FC2D8`, `DispatchMessageA
0x4FC310`, `GetAsyncKeyState 0x4FC350`, `TranslateMessage 0x4FC364` (the import table of the
pristine executable, `objdump -p`).

**What Impure builds on this.** The injection (`tagpu_input.c`) posts one tagged message at a
time and the next only when both rings' own indices read empty at a flip after the shield
delivered it (`tagpu_gui_input_rings`, in `tagpu_gui_hook.c`), so a burst never overflows a ring
and a modifier is down while every event between its down and its up is handled. Because every
pop is conditional, a ring can stay non-empty -- a press outside the message box's panel, a key
while the in-game key pass is skipped and the GUI pump does not take it -- and the batch then
waits until its waiter abandons it ([input firewall](../input-firewall.md), *The transport*).

**Every wall clock the engine stores.** The sources are `GetTickCount` (IAT `0x4FC0DC`; `0x4B6560`
is `jmp` to it, and `0x4B6340` is `GetTickCount · rate / 1000`), `QueryPerformanceCounter` (IAT
`0x4FC0BC`; `0x4E1730` in seconds), and the C runtime's `time` `0x4E6480`; there is no
`timeGetTime`. Their stores into the two images: the GUI's clock `main+0x5AF..+0x5B6`, the ping
deadline `main+0x1B5F`, the pacing gate's `main+0x38A37..+0x38A42`, `+0x38A43` and `+0x38A4F` (and
`+0x38A4D`, which its trim steps, with bit 2 of the pause word `+0x38A51` that follows it), the profiler `main+0x38D85..+0x38DD4`, the palette fade and
briefing flow `main+0x3905B..+0x3906E`, the local network name `main+0x14` (from `time()`); in
`.data` `0x5129A4`, `0x5129A8`, `0x5129C8`, `0x512C78`, `0x512C7C`, the network channels' deadlines,
`0x51E544`, `0x51E674..0x51E67B`, `0x51E680`, the loading fade `0x51E6C8` and `0x51F308`, the
scoreboard `0x51F2F4`, the music `0x51F2F8`, `0x51F300`, `0x51F304`, the mouse record's stamp
`0x51F4C2`, the FPS counter `0x51F4FA..0x51F509`, `0x51FB94`, `0x51FBAC..0x51FBB7`, the timers
`0x51FBE0..0x51FC87`, the Cavedog library's `0x5292E0` and `0x529548`, and `time()`'s cache
`0x529F10`. Each is a `leave` row. Outside the images: the player records' clocks (already
`replay-layout.tsv`'s timer rows) and the hover wave in the unit records.

**The two random streams and where they are seeded.** The simulation RNG `0x51FC88` is seeded once
a level by the loader `0x497180` from `QueryPerformanceCounter` (`0x49718C`, the two dwords summed,
then `0x4B6CA0` at `0x49719D`); the CRT's `rand` `0x4E4870` is seeded by `srand` `0x4E4860` at
`0x4971AE` (the loader thread's stream) and `0x49E8BB` (WinMain, the game thread's), both from
`time(0)`, and keeps its state per thread at `+0x14` of the block `0x4EB0F0` returns, in neither
image. `0x4E4870` has 61 call sites, no jump to it and no stored pointer. **None of the simulation's
consumers is recomputed on a puppet host**: the wind's draw `0x490C60` never runs with Impure
(`fix_wind` replaces it, and the wind's state and fields are recorded); the meteor shower
`0x437DE0` and the forced shower `0x438070` are gated on a puppet level and their state is recorded;
the objective's draw `0x490385` is never reached in this exe; the skirmish start positions
(`0x4979E7`, `0x497A32`) feed the seat setup a puppet level skips; and the network start shuffle and
the packet loss are network-only. Every site a puppet host does recompute is presentation (feature
smoke, particles, lightning, replies, elimination texts, music), the viewer's camera shake
`0x41C6F0`, or the front end. A new simulation use of `0x4E4870` would diverge silently: neither
image holds its state. A live host's meteor shower is one already: its strike points share the
game thread's stream with the draw path, so a takeover cannot reproduce them
([*The scheduler, the clock and the test pacing*](#the-scheduler-the-clock-and-the-test-pacing-disassembled-measured-2026-10-08)).

**Values in the records that look like addresses** (MEASURED on the first run's last live dumps of
four fixtures; the DLL loaded at `0x76CF0000`, `SizeOfImage` `0x4949000`): 261 of 169,140 plain
aligned unit dwords lay inside the DLL's range and none in a heap range — dwords straddling two
16-bit fields (unit `+0x18`, `+0x64`, `+0x68`, `+0x70`, `+0xA8`, `+0x140`; projectile `+0x38`,
`+0x64`). A u16 whose value lies in `0x76CF..0x7B63` makes one: 1.79 % of uniform values. The
simulation RNG's state is below 2^31 and lands in the DLL's range 3.6 % of the time. So the scan
tests the engine's records and the globals image against the engine's own ranges only.

### A level starts paused after the last one was left through the in-game menu — the pause bit `main+0x38A51`, cleared at `0x49842A` [DISASSEMBLED + MEASURED 2026-10-01]

**The bit.** `main+0x38A51` is a u16 of flags; bit 0 is the pause. While it is set the frame
function `0x496790` skips the scheduler `0x495230` and the tick `0x495490` (`0x4968AA`,
`0x496920`), and those two are called from nowhere else (`0x4967F3`, `0x49680C`, `0x496928`,
`0x49693E`). The scheduler tests it as well (`0x4953D9`), the pause icon `igpaused` shows it (`0x46A107`;
`0x46A2A8` beside it tests bit 1, not the pause), and the pause key flips it and puts the new bit into a message `0x19`
(`0x4960AA..0x4960E0`) [INFERRED: `0x451DF0` sends it to the peers]; the `0x19` receiver stores a
peer's bit (`0x455967..0x45597A`). Bits 1 and 2 are the scheduler's: bit 1 its lag flag
(`0x49535C`, `0x495367`), tested only by the corner icon `0x46A2A8`; bit 2 the live speed below the
chosen one, stored on every call (`0x49527A..0x495299`); **bit 2 has no reader**
[DISASSEMBLED 2026-10-12: every instruction carrying the displacement `0x38A51` but the
scheduler's three stores tests only bit 0 or bit 1, or keeps bits 1 and 2 as they are; the saved
game's `Players` block copies `+0x38A37..+0x38A52` out at `0x46633A` and back at `0x4660A9`]. The
frame function calls the scheduler only past its bit-0 tests (`0x4968AA`, `0x496920`), so while
the level is paused bits 1 and 2 keep the last call's value through the hold. Both are pacing
state, bit 2 following the wall clock through the trim and bit 1 the peers' lag, and the demo
recorder leaves them (`replay-globals.tsv`, `pacingbits`, the owner's decision 2026-10-12): MEASURED 2026-10-12 under a load average of 63–87, a live
recording held bit 2 set at GameTime 721 where its playings held it clear. Every other store of the word: set bit 0 at `0x44BE61`, `0x45D0A1` (in `0x45CFC0`,
outside mode 3), `0x460E00` (the in-game menu's open, outside mode 3), `0x49306D` and `0x493330`
(the LOADGAME screens); clear it at `0x44B6F0` (in the battle room's code [INFERRED from its
neighbours]), `0x460A33` (the in-game menu's close, outside mode 3), `0x49720A` (the loader's
network branch) and `0x49990E` (key `0xE3` in state 7's handler `0x499880`); `0x495B6D` clears it
for a step and `0x495DC2` puts back the bit `0x495B85` saved.

**The defect.** Leaving a level through the in-game menu (EXIT, MAINMENU, yes) opens the menu,
which sets the bit, and never closes it, so the bit outlives the level. The loader `0x497180`
clears it only in its network branch (`0x4971EC`, mode 3); the campaign branch `0x49745C` and the
skirmish branch `0x4973C1` leave it. So the next skirmish, campaign mission or saved game starts
paused until the pause key. MEASURED 2026-10-01 on retail with the stock build: a SINGLE →
Skirmish → Start after a level left through the menu read GameTime 0 and the bit 1. The same
holds for a load from the in-game menu (*The demo recorder's capture*, above: GameTime stayed
1846 until a pause key, 2026-09-29).

**Every level start passes `0x49842A`.** Every level is loaded by the load state `0x497F40`.
Only three front-end states install it: `0x496BB0` at `0x496C3D` for mode 2 (a skirmish, our
Watch, a `tacli` playback), `0x496DB0` at `0x496DFF` (mode 1, a campaign mission;
`0x496BB0` sends mode 1 there at `0x496BF6`), and the battle room's `0x496CE0` at `0x496D75` and
`0x496D9D` (a network game). A saved game, from the shell or the in-game menu, goes the same way:
the LOADGAME screen's load branch sets the mode from the save's `Gametype` (`0x49267F`, the
mode setter `0x434AB0`) and installs `0x496BB0` (`edi` from `0x49295B`, stored at `0x49297E` or
`0x492A7F`). Those two are the only stores of the state handler `main+0x391F5` from a register;
of the 22 with an immediate only those five and SetState `0x490B30`'s case 5 (`0x490BB3`; the jump
table `0x490C14`) store `0x497F40`, and SetState's eleven callers hand it 1, 2 or 7. The load state
moves on only once the loader thread `0x497C90` has set its done bit, `main+0x38D75` bit 1
(tested at `0x498342`, set at `0x497C62`, `0x497180`'s last store before it returns and the thread
ends). Both branches of `0x498353..0x498455` then meet at `0x49842A`, the call of the HUD chrome
`0x467D70` (`0x498386` is the only jump in), before the frame function's first run at `0x49842F`
and before the in-play state `0x499200` is installed at `0x498455` (SetState's case 6, never handed
6).

**Patched** (`tagpu_patches.c`, `fix_load_unpaused`, local). The call at `0x49842A` goes to a stub
that clears bit 0 with an `and byte` unless the mode `[[main+0x391E9]]` is 3, then jumps to
`0x467D70`, which returns to `0x49842F`. A network game keeps the loader's and the peers' bit. The
demo recorder does not carry the word (`replay-globals.tsv` has no row for it), and a recording's
iterations are the ticks that ran, so the fix changes no recording.
MEASURED 2026-10-01 on retail, `tacli` on Two Continents at 1024×768, each start after leaving
the previous level through the menu with the bit read 1 in the shell: a new skirmish ran (GameTime
32 → 158 → 294 at 4 s intervals, bit 0); a Watch of a recording made before the fix ran (18 → 290)
and played to its end, 551 iterations, 0 late, 0 refused; a saved game loaded from SINGLE →
LoadGame ran (568 → 835), and so did the same save loaded from the in-game menu of a running
skirmish (796 → 1053); campaign mission 1 (Arm) ran (35 → 294). The pause key inside a level
still pauses (GameTime held at 293, bit 1) and resumes (328 → 431, bit 0). A network game was
not run.

## The engine's rates — sim tick, render cadence, and what `gamespeed` multiplies

[MEASURED 2026-09-09, this project — a live skirmish on Two Continents, one walking ARMCOM,
camera pinned, 1920x1080. Every number below is a rate against the wall clock.]

**At TA's normal game speed the sim runs at 30 Hz and the tick counter is the sim tick.**
`main+0x38A47` advanced **29.81/s** while the unit's 16.16 position advanced **29.85/s** —
1.00 ticks per position update. That is TADR's `GameTime == 6 * 30` and the `÷30` the
`+clock` handler applies (§"the `+clock` cheat"), both confirmed.

**The world only changes 30 times a second, and so does the picture.** With the present cap
at 60 fps, **49.9 %** of consecutive presented frames (601 sampled, world viewport, stock
renderer) are **byte-identical** to their predecessor: the engine redraws the same state.
So the useful frame rate of the original renderer is the sim rate; presenting faster only
duplicates. The renderer itself has no internal cap — with `maxfps 0` the same scene
presented **915 fps**, almost all of it repeats.

**The sim is paced by the wall clock, not by the frame loop.** The commander walked the same
world units per second under a 30 fps cap and a 60 fps cap, and the tick counter held ~60/s
at present rates from 30 to 915 fps. It is a fixed timestep that *falls behind* when the loop
is starved: at a 15 fps cap (9.4 fps actual) the tick managed only 47/s.

**`gamespeed` (registry `HKCU\…\Total Annihilation\gamespeed`, and TA's own `+`/`-` keys)
multiplies the TICK rate, not the update rate.** Measured on the same walk:

| `gamespeed` | tick rate | unit position updates | ticks per update |
|---|---|---|---|
| 10 (TA normal) | 29.81 /s | 29.85 /s | 1.00 |
| 20 | 60.49 /s | 29.89 /s | 2.02 |

So at 20 the picture still changes 30 times a second; each change is a bigger step, and the
tick counter runs at double rate. **Two consequences.** The in-game clock is `tick ÷ 30`, so
at `gamespeed` 20 **Game Time runs at exactly 2× real time** — read off the screen:
`00:00:16` at tick 489, `00:01:17` at tick 2319, 30.6 real seconds apart. And any tool that
treats `main+0x38A47` as seconds×30, or as a sim-step count, is only right at `gamespeed` 10.

**So a single-player fight does not reproduce run to run, even from one file** [MEASURED
2026-09-23, `tagpu_cobtrace` on a 40-unit fight, two runs of one build]. With the sim RNG state and
the game thread's CRT seed written at the same point of the scenario apply, and the apply landing
on the same tick, the two runs' per-unit COB traces agreed event for event for the first 90 ticks
(1806 events over 40 units) and then parted at the first impact: at `gamespeed` 20 the step is
two ticks on average but follows the wall clock, so a shell's flight is cut into different steps.
With both speed fields held at 10 in memory, the apply tick matched exactly and the traces parted
earlier, at the first target choice, for a reason not identified. Two consequences: a COB-trace
comparison of two builds over a whole fight measures noise, not the change, and TA's multiplayer
does not need one, because it replicates state rather than running lockstep
([networking-lobbies](../networking-lobbies.md)). The comparison that answers "do two peers agree" is
a paused roster on both peers of one game.

**The engine's DRAW loop is neither of these rates, and it is scene-dependent.** `DrawGameScreen`
— and with it the fog overlay `0x4848E0`, which is where `tagpu_fogwide.c` ticks — turns over as
fast as the scene allows while the presenter caps only the flip.

**`0x4848E0` has exactly ONE call site: `0x469D8E`, inside the per-frame world draw**
[VERIFIED 2026-09-11 by `objdump -d -M intel` over the whole image of
`pristine/TotalA.exe.pristine`: one `call 0x4848e0` in `.text`, encoded `e8 4d ab 01 00`. The grid
builder it drives, `0x4843C0`, likewise has exactly one]. That is worth stating because the
function reads like a sim tick and is not one: **it is on the DRAW path and keeps running with the
sim paused**, so anything hung off it runs at the draw rate below and not at `gamespeed`'s. What
*does* follow the sim rate is the fog REBUILD, because the is-current bit it tests is cleared by
the LOS stamps and nothing stamps while the sim is stopped. Reported by the cursor_zoom session,
whose own note had it the other way round; verified here rather than taken on trust. Counted against the wall clock
[MEASURED 2026-09-10, 1920×1080, `--maxfps 60`, both instances presenting 58–60 fps]: **330 calls
a second** on `crowd-static` (256 units, Two Continents) and **3200–4900** on a sparse Town &
Country skirmish. So a per-draw counter is not a per-frame counter and not a per-tick one either:
anything reported "per N draws" is a ratio, and turning it into a rate needs the draw rate
measured in the same run.

**`gamespeed` is registry state that outlives a launch, like `Gamma`.** The game saves it with
its other options, so a session that presses `+` leaves every later launch that reads the same
registry at that speed. Under tacli that registry is the instance's own store
([tacli design](../tacli-design.html) §"The registry: a store per instance"), made at 10; it used
to be the one `user.reg` the template prefix and every instance hold as **one inode**
(`clone_prefix` is `cp -al`; measured 2026-09-09: 101 links), where it was found at **20** on
2026-09-09. **Read it before trusting any timing measurement** (`tacli registry <i>
gamespeed`), and note that a walking-unit artifact measured at 20 is twice the size a player at
normal speed would see.

### The scheduler, the clock and the test pacing [DISASSEMBLED + MEASURED 2026-10-08]

Read from `pristine/TotalA.exe.pristine` (`objdump -d`) for the replay harness's turbo
([*Test speed*](../tadr-port/demo-recorder-completeness.md#the-replay-test-harness)); the speed fields
and the setter are in *The speeds* above, the step's iteration in
[*tick boundaries*](#the-demo-recorders-tick-boundaries-the-sim-step-0x495490-and-its-callers-disassembled-2026-10-01).

- **The clock `0x4B6340`** is GetTickCount (IAT `0x4FC0DC`) × `[*(0x51FBD0)+0xE8]` / 1000, integer
  (`0x4B6340..0x4B6361`); `+0xE8` reads 30, so it steps in whole thirtieths of a second. Its callers
  outside the scheduler include the main menu's idle timer `0x426BCA`, the GUI animation
  `0x478992..0x478BEB`, the player records' join and receive stamps and the hover wave `0x48A66A`
  (*The unit and player records, byte by byte*).
- **The scheduler `0x495230`** (called at `0x496928` in a skirmish, `0x4967F3` in a network game):
  elapsed = clock − the base `main+0x38A37`, kept at `+0x38A3F`; due = elapsed × the live speed
  `+0x38A4D` × 0.1 (`0x4FDA28`), × the lag scale when it applies, + the fraction `+0x38A43`; the
  floor goes to **DeltaTime** `main+0x38A3B` and the rest back to the fraction
  (`0x495236..0x4953B9`). DeltaTime is 0 while the pause bit `+0x38A51` bit 0 is set
  (`0x4953D9..0x4953E2`). **The clamp `0x4953F5`**: a due above 5 becomes 5, the excess dropped,
  and the throttle counter `+0x38A4F` takes one (past 10 the live speed drops by one); an unclamped
  frame gives one back (past −100 the live speed rises by one, up to the ceiling `+0x38A4B`)
  (`0x4953EF..0x49547B`). The lag scale applies only when the oldest type-3 record lags 900 ticks
  or more, and sets `+0x38A51` bit 1 (`0x49531F..0x49535C`).
- **So the rate is the clock's, not the frame's.** A frame that sees no new clock step runs no
  tick; one that sees a step runs speed × 0.1 ticks of it, at most five. At speed 20 that is two
  ticks every thirtieth of a second however many frames run in between: MEASURED 60.1 ticks/s with
  frames at ~180/s in play. Above speed 50 the clamp, not the speed, is the bound: five ticks per
  clock step, 150 a second at any frame rate of 30 or more — the 150.5 MEASURED at speed 80 in
  *The speeds*.
- **DeltaTime's readers**: the step `0x495490` (`0x495497`, which caches it for the call), the
  frame function (`0x4967FD`, and the count test `0x496932` before the step call `0x49693E`), the
  scheduler and a debug `sprintf` `0x46AA09`; the load wait writes it (`0x498175`, `mov
  [edx+0x38A3B], ebp`). The speed fields' readers
  are the Options screen, the save `0x43136B`, the HUD `0x468C0B`, the setter `0x490E16`, the keys
  `0x496559` and the scheduler — no tick code (a displacement scan of `.text`). The registry's
  `gamespeed` (`0x504550`) is copied into the ceiling and the live speed **unclamped** at start-up
  (`0x430509`, `0x43052B`).
- **The batch is part of the result.** The step runs its iterations DeltaTime times and then its
  tail once a call — `0x428BD0`, `0x428BE0`, `0x428BF0` (each a bare `ret`), the chat expiry
  `0x463EF0`, the temporary sight records' expiry `0x482130` (`0x495674..0x495688`) — so a sight record outlives its deadline
  by up to one batch, and where the batches fall changes what units see. MEASURED (tenseat taken
  over at 931 as player 0, each takeover dumped at 2731, compared field by field): of two pairs of
  takeovers at speed 20, whose batches are twos with an odd one after a slow frame, one pair parted
  in 1,328 unit fields; two pairs at a fixed batch (k = 2, then k = 1) differed only in the meteor
  shower's two points (next bullet); speed 20 against k = 2 differed in 1,272 and 1,383 unit fields.
- **The meteor shower and the CRT `rand`.** The meteor tick `0x437DE0` draws the CRT's `rand`
  `0x4E4870` at six sites (`0x437E37..0x437FB5`) for the strike points `0x512334` and `0x512320`;
  the forced shower `0x438070` (one caller, `0x41778A`) draws it too (`0x4380AD`, `0x4380CE`) and
  writes `0x512334` (`0x4380FB`). On
  the game thread the draw path draws from the same per-thread stream every frame (static call
  graph: `0x49BE60`, `0x420B00 → 0x421550 → 0x472AB0`, `0x418310`), so on a live host the strike
  points depend on how many frames were drawn between ticks, and a takeover, which does not restore
  that stream (it is in neither image, *The two random streams*), cannot reproduce them. A
  puppet host never runs the meteor tick: the shower's state is recorded.
- **The test pacing: the observer at `0x49692D`** (tagpu_replay.c, `rt_pace_init`), between the
  scheduler's call `0x496928` and the count test `0x496932`, on the skirmish branch only — past the
  front-end test `0x496918` and the pause test `0x496920`, so never in a network game, a menu or a
  paused frame. In order: the turbo writes k into DeltaTime, or 0 while the recorder's ring holds
  more than half its 16 MiB (the count test then skips the step, tail included, so every batch
  stays k ticks), and leaves a frame with the lag bit set at the stock count; a puppet's armed
  round trip lowers the count to stop at T−1; an armed `pause at <T>` lowers it to stop at T; and
  **a test launch's hold** (`tagpu_hold.on`, THE LAUNCH'S HOLD) lowers the level's first batch to
  one tick. The pause itself is set at the frame visit `0x4969CD` after step T (the hold's: after
  that first step, or after a seek already pending there once it has rebuilt the level; a seek or
  takeover asked for later runs on the held frames, which keep the level held at T−1) and
  cleared by `resume`, as the Pause key's toggle of bit 0 (`0x4960AA..0x4960BC`) does without its
  network packet; the hold keeper at `0x496920` (above) keeps it set until then. MEASURED at
  k = 1: a light fixture's live lane ~1,500 ticks a second, a puppet's P and R 200–385, the battle's
  lanes 64 (twelve games at once) to 230 (one), against ~60 at speed 20. Without the ring's
  back-pressure the stock-limits battle ran 314 ticks a second and filled the ring by GameTime
  1003: it writes ~56 KB of records a tick.

## The simulation clock — `main+0x38A3B..0x38A52` — mapped by us (2026-09-09)

*Layout `[VERIFIED]` against the vendored TADR corpus (`tools/tamem_ghidra.h`, which carries these
offsets in its comments); the rates below are **ours**, measured live with `tacli peek` against the
wall clock on `scenarios/walk-lerp.json`.*

| VA | type | what |
|---|---|---|
| `main+0x38A3B` | u32 | `DeltaTime` — **sim ticks to execute this engine frame, 0..5**, computed by `ApplyDeltaTime` and consumed by `InGameAsynchronousThread`. This is why the tick can advance by more than one between two render frames |
| `main+0x38A3F` | u32 | `scrollLen_buf` — raw elapsed, current − previous `GameRunSec()` |
| `main+0x38A43` | u32 | the fractional-tick accumulator (used as a float) |
| `main+0x38A47` | i32 | **`GameTime`** — the sim tick. `tagpu_cobtrace` and `tagpu_posedump` both stamp it, which is how their logs join |
| `main+0x38A4B` | i16 | the speed **CEILING** — the corpus calls it `GameSpeed`, and it is what `minus`/`plus` set, but it is *not* what scales time into ticks. `0x49546A` only lets the throttle raise the live value back **while `0x38A4D < 0x38A4B`** (`cmp cx,[eax+0x38a4b]; jae skip`) |
| `main+0x38A4D` | i16 | the **LIVE effective speed** — the corpus calls it `GameSpeed_Init` and the name is misleading. `0x495260` loads `WORD [main+0x38A4D]`, `fild`s it and multiplies by the double `0.1` at `0x4FDA28`, and *that* product scales elapsed time into ticks. **This is the field to read.** `[VERIFIED 2026-09-09 by disassembly]` |
| `main+0x38A4F` | i16 | the **lag counter** the throttle runs on: `inc` at `0x49540A`, `dec` at `0x495448`. At **+10** (`0x495415`) it is zeroed and the live speed `0x38A4D` is **decremented**, floored at 1 (`cmp ax,1; jbe`); at **−100** (`0xff9c`, `0x495454`) it is zeroed and the live speed is **incremented**, capped at the ceiling above `[VERIFIED 2026-09-09 by disassembly]` |
| `main+0x38A51` | u16 | flags: bit 0 `IsGamePaused` (also read as `[main+0x38A51] & 1` by the HUD's pause icon), bit 1 the scheduler's lag flag (`0x49535C`), bit 2 the live speed below the ceiling (`0x495299`, read by nothing) `[DISASSEMBLED 2026-10-12]`; see *A level starts paused* |

### The tick rate is `3 × GameSpeed` a second, and a skirmish does not start at 30 `[MEASURED 2026-09-09]`

| GameSpeed | GameTime per wall-clock second | ratio |
|---|---|---|
| **20** — what `tacli scenario load` lands on | 59.88 | 2.994 |
| 15 | 44.78 | 2.985 |
| 10 | ≈ 30 | 3 |

`minus` / `plus` step `GameSpeed` by one per press. So the tick period is
`1000 / (3 × GameSpeed)` ms — **16.7 ms on a default skirmish, not 33.3.**

**This corrects a reading of the `+clock` cheat.** Its arithmetic (`÷108 000, ÷1 800, ÷30`, in the
HUD-extras table above) is right, but "30 ticks a second" is the **GameSpeed-10** rate, not a
property of the engine: at the speed a skirmish actually starts at, that clock runs at double wall
time. Anything converting sim ticks to wall-clock time must read the **live** speed at
`main+0x38A4D` — **not** `main+0x38A4B`, which the corpus labels `GameSpeed` but which is only the
ceiling — or measure the interval; a hard-coded 33.3 ms is wrong by 2× out of the box. **The two
fields agree until the machine falls behind**, which is exactly the case where the difference
matters: the throttle drops `0x38A4D` and leaves `0x38A4B` where the player set it, so code that
reads the ceiling then believes a rate the sim is no longer running at. *(Corrected 2026-09-09 by
the landing review, which disassembled `0x495260` and the throttle at `0x495415`–`0x49547B`; the
table above previously carried the corpus's names with no note that they do not describe which
field drives the rate.)* **Measuring the interval, as `tagpu_lerp.c` does, sidesteps the whole
question and is the reason that module is right at any speed and through a throttle event. `tagpu_lerp.c` measures it, and its
learned period read `p=33.2ms` against a predicted 33.3 at GameSpeed 10 (0.3%) and tracked a live
speed change down from 20.

**The COB `sleep` conversion does NOT scale with GameSpeed.** The divisor is the COB object's
`+4`, i.e. `0x4B6330() = [[0x51FBD0]+0xE8]`, and that reads **30** with `GameSpeed` at 20
`[MEASURED]` — so `sleep 100` is always 3 COB ticks and a raised game speed simply plays every
animation at `GameSpeed/10 ×` real time. That settles the `[INFERRED from that use]` on `cob+4` in
the next section: it is 30, and it is a constant rather than the live tick rate.
