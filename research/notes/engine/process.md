# Process, threads, files and registry

Start-up, the crash filter, the threads and the load flags, DirectDraw, the archives, the registry, sound and the movie player.

*Part of the [engine map](../exe-reverse-engineering.md). A function's entry is the section whose heading names it — the [address index](address-index.html) lists them: a new fact about a function extends its entry, in place; a new section is for an area nobody has mapped yet.*

Addresses are VAs for our pristine build (ImageBase `0x400000`, md5 `8e74a1dffa1f5988624c52048f5b20cd`). `main` = `*(void**)0x511DE8`, the `TAdynmemStruct`.

### TA's crash filter — `0x4DA1D0`, `0x4DA2A0` [DISASSEMBLED + MEASURED 2026-09-27]

**`0x4DA1D0(flags)`**, cdecl, sets up crash reporting once (the latch `0x5289CC`). It reads the
command line's `-debughelper` (`0x4D9F60` with the text at `0x50D1A8`, then `=%d`), and then, by
bit:

- bit 2 clear: `SetUnhandledExceptionFilter(0x4DA2A0)` at `0x4DA247` (the IAT slot `0x4FC1AC`).
  The previous filter it returns is **discarded**.
- bit 4 clear: `0x4DF160`.
- bit 8 clear: more setup from `0x4DA266` on, not traced.

Its one live caller is WinMain, `0x49E83C`, with flags 8, just before it installs the
out-of-memory handler above (`0x49E849`). The other call site, `0x4DA3E0` (flags 0), has no
callers: no call, no jump and no pointer to it anywhere in the image.

**`0x4DA2A0`**, the top-level filter, stdcall on the `EXCEPTION_POINTERS*`: it returns
`0x4D8E60(e, "Global Exception Handler")` (the text at `0x50D2D0`) and calls no earlier filter,
because none was kept. `0x4D8E60` is the crash report: an `0x143F0`-byte frame, and a re-entry
latch at `0x5289C0`, so a fault inside the report answers 0 (`EXCEPTION_CONTINUE_SEARCH`) at
once. Its other callers are `0x49E6BF` (inside `0x49E680`, the out-of-memory handler's register
dump), `0x4CBABD` and `0x4D9C07`.

**The C runtime's pair.** `0x4F2240` installs `0x4F21E0` and keeps the previous filter at
`0x52A428`; `0x4F2260` puts that one back. Neither has a direct caller: the only references are
the pointers at `0x50105C` and `0x50107C`, in two null-bounded tables of function pointers in
`.data` — the runtime's start-up and exit lists [INFERRED]. So the runtime's filter is in place
before WinMain and is replaced, unchained, by `0x4DA2A0`.

**What this means for us** ([gpu-status](../gpu-status.html) §2.102): a filter installed after
WinMain sees an exception first and must pass on what is not its own; `tagpu_restore_guard.c`'s
is installed at the restorer's first bring-up and logs `004DA2A0` as the filter it passes to
(MEASURED on the reference setup).

### TA's single-instance test — `0x49E877..0x49E8AB` [DISASSEMBLED + MEASURED 2026-09-27]

In WinMain, right after the crash-reporting setup above: `OpenSemaphoreA(0x1F0003
SEMAPHORE_ALL_ACCESS, FALSE, name)` at `0x49E885` (the IAT slot `0x4FC0AC`, KERNEL32's seventh
thunk from `0x4FC094`), the name through the pointer at `0x50971C`, which holds `0x507A58`,
`"Total Annihilation"`. A non-NULL handle means another TotalA.exe runs, and WinMain returns −1
at once (`0x49E88F`), with no message. Otherwise `CreateSemaphoreA(NULL, 1, 1, name)` at
`0x49E8AB` (the slot `0x4FC0B0`) creates it, and the process holds it until it ends.

**What this means for us**: a process that starts TotalA.exe again, as the restorer's crash
relaunch does ([compute-restorer](../compute-restorer.html)), must not let the new process reach
`0x49E885` while the old one still holds the semaphore, or the relaunch silently exits.
`tagpu_rguard_attach` waits for the old process at `DLL_PROCESS_ATTACH`, which runs before WinMain:
a terminating process's handles are closed before its process object is signalled, so once the
wait returns the semaphore is gone. MEASURED: on the reference setup the relaunch has logged
both that it waited and, on another run, that the old process had already ended, and came up
each time; on the Windows test setup it came up.

Under Wine the name lives in the prefix's wineserver, which can hold it after the process's
leader thread is gone, so `tacli launch` ends that server (`wineserver -k`, then `-w`) before
starting a game ([tacli-design](../tacli-design.html), *A launch ends the prefix's wineserver
first*). MEASURED 2026-10-09: with another process holding the semaphore, `wine TotalA.exe`
exits with code 255, the −1 above.

### The engine's mouse thread `0x4C2990`, and TotalA's other threads during play [DISASSEMBLED + MEASURED 2026-10-01]

Found while the demo recorder's dumps were checked: 2 of 16 stock-fixture dumps summed bytes that
differed from the file, because the dump wrote a buffer and then read it again for its CRC while
another thread wrote some of its bytes (the slice note's *S3b, verified and measured*). This is
that thread, and the inventory that shows it is the only one. Each "no caller" below is a byte scan
of the whole image for rel32 calls and jumps to the address and for the address as a stored dword.

**The mouse thread runs from the graphics init to the process's exit.** WinMain `0x49E830` builds the
graphics globals' `+0x202` word with `or ah,2` at `0x49E936`, unconditionally, and calls the graphics
init `0x4B5980` once (`0x49E977`) with the globals block `0x51F320`, a static in `.data` (`push
0x51F320` at `0x49E8ED`), which it stores in `0x51FBD0` (`0x4B5990`; MEASURED: the dumps' `.data`
holds `0x51F320` there). The init turns that bit into the mode argument of the cursor init
`0x4C2BD0` (`0x4B5A3A..0x4B5A67`), whose mode-1 branch starts `0x4C2990` through `0x4B6B20` at
`0x4C2C91` and sets `+0x1CE` = 1 (`0x4C2CA0`). Nothing stops it: the stop routine `0x4C2AC0` (it sets
`+0x1D6` and waits) and the restart `0x4C2A80` have no caller and no stored address.

**Its loop, `0x4C2990..0x4C2A74`**: `SetThreadPriority(GetCurrentThread(), 2)`, THREAD_PRIORITY_HIGHEST
(`0x4C2990..0x4C2999`); then each iteration takes the deadline `GetTickCount() + 0x21`
(`0x4C29C1..0x4C29C5`, IAT `0x4FC0DC`), acquires the tagged lock `0x52A4E8` with the tag `'MOUS'`
(`InterlockedExchange`, IAT `0x4FC174`, `0x4C29BB`, `0x4C29D3`; the protocol is *The tagged lock at
`0x52A4E8`* above) and stores the owner tag `0x52A4EC` (`0x4C29F2`), draws the cursor when `+0x1B2`
is non-zero (`0x4C2A02..0x4C2A0D`), releases unless it already held the lock (tag 0 at `0x4C2A1C`,
`InterlockedExchange(0x52A4E8, 0)` at `0x4C2A22`, `SetEvent([0x52A4F0])` through IAT `0x4FC170`), and
sleeps to the deadline, at least 1 ms (`0x4C2A31..0x4C2A4A`, `Sleep` at IAT `0x4FC0E0`) — **about
every 33 ms**, while `+0x1D6` is 0 (`0x4C2A50..0x4C2A5C`).

**Its draw `0x4C25E0(globals)`**, under the lock, returns early when `+0x1D2` is 0 (`0x4C25E8`) or
when the context acquire `0x4C5FF0` answers 0 (`0x4C25FB`; with GDI mode `+0x44` it copies `+0x50`,
otherwise it locks the primary surface `[+0x88]` through the DirectDraw vtable's `+0x64`). Past them
it stores four `.data` words of the globals block: the cursor's position from `GetCursorPos`,
`+0x196`/`+0x19A` = `0x51F4B6`/`0x51F4BA` (`0x4C2624`, `0x4C262A`), and its drawn position, the
position less the hotspot, `+0x1B6`/`+0x1BA` = `0x51F4D6`/`0x51F4DA` (`0x4C284C`, `0x4C2852`). The
game thread writes the same two pairs (WM_MOUSEMOVE; the flip's cursor draw `0x4C67C0`). MEASURED in
all 403 saved dumps that carry `.data`: `+0x1CE` = 1, `+0x1D2` = 1 and `+0x1B2` ≠ 0, so the draw runs
in play. Its closure over direct calls (17 functions) names no address of the main block, the explored
map or the sight counters, and its only absolute `.data` stores are the two tag writes; the
back-buffer lock's stores in `0x4C5E70`/`0x4C5FA0` are reached only for a NULL context, which the
mouse thread never passes.

**What it writes during play, then**: the lock word `0x52A4E8` and its owner tag `0x52A4EC`, each twice
an iteration (`'MOUS'` and 0), and the cursor's two positions when the pointer moves. MEASURED: each
of the two dumps whose sum differed from its file sums right with those 8 lock bytes in the lock's
other state and nothing else changed (one held `'MOUS','MOUS'`, the other `0,0`), and the lock was
held in the file image of 1 of 416 dumps. None of these bytes is recorded (`replay-globals.tsv` names
them under what it leaves out), so two dumps of one GameTime can differ there.

**`.data` around the locks.** `0x52A4E8..0x52A4FF` holds two tagged locks: the first (word
`0x52A4E8`, tag `0x52A4EC`, event `0x52A4F0`), which the mouse thread takes and so does game-thread
code, among it the flip `0x4C63A0`, `0x4B5510`, `0x4BA200`, the cursor-sprite setter `0x4C2B20` and
`0x4CBAB0`,
and a second (tag `0x52A4F4`, event `0x52A4F8`, word `0x52A4FC`) used only by `0x4D0F60` and
`0x4D1480`. The two events are the only `CreateEventA` sites (`0x4C22C3`, `0x4D0A03`). Around them,
`0x52A41C..0x52A4E0` and `0x52A508..` are read by C runtime functions, and `0x52A4E4` and
`0x52A500..0x52A505` are static-initialisation guards (`0x463BA0`, `0x4E4140`).

**TotalA's other threads.** `CreateThread` has three call sites (`0x4DA27A`, `0x4E616E`, `0x4E780C`;
*Thread creation* above):

- **The debug helper's message pump `0x4DA2C0` never starts.** WinMain calls its init `0x4DA1D0` with
  flag 8 (`push 0x8` at `0x49E83A`), which skips the `CreateThread` at `0x4DA27A`; the init's other
  caller, `0x4DA3E5` in `0x4DA3E0`, is itself never referenced.
- **`_threadstartex` `0x4E61A0` serves one thread**, the DirectPlay lobby join `0x451770`, started from
  `0x4516F5` (`_beginthreadex` `0x4E6120`) by `0x451640`, which waits for it up to 40 s. It writes the
  network object at `main+0x14` (`0x4C9A70`'s stores), during a lobby-launched join only, before any
  level [INFERRED: after the 40 s time-out it can still be inside DirectPlay].
- **The `_beginthread` wrapper `0x4E77D0`** (through `0x4B6B20`) starts the loader (`0x4982CA`) and
  the mouse thread (`0x4C2C91`; and the restart's `0x4C2A9A`, never run). The loader's body ends with
  `0x497C62` and its thread with `_endthread` `0x4E7900`, which frees the thread's CRT block under the
  heap lock and calls `ExitThread`: once per load, not during play. (*The in-play publish point*
  below measured the loader thread alive at four teardowns; the disassembly shows no path that keeps
  it alive past `0x497C6C`, so that was a later load's loader or one still between there and
  `ExitThread` [INFERRED; not re-measured].)
- **The C runtime's heap lock** is lock 9, the critical section `0x529FC8` through `_locktable`
  `0x50DF60`, taken by `malloc`/`free`/`calloc` (`0x4E8890`, `0x4E8820`, `0x4E8EA0`); in play no thread
  but the game thread calls them (the mouse thread's closure holds no CRT function, and Impure's
  threads use their own C runtime).
- **Callbacks** handed out: the DirectPlay enumerations (lobby only, synchronous), `EnumWindows`, the
  window procedure (the window's thread, the game thread), the crash filter `0x4DA2A0`, and two
  `SetTimer`s with a NULL TIMERPROC in the debug helper's never-made window; `signal()` `0x4E68C0`,
  which would install a console handler, has no caller. Nothing is handed to DirectSound or winmm.

**Impure code that runs on the mouse thread** (SRC): `fake_GetCursorPos` (`winapi_hooks.c`), the
primary surface's Lock and Unlock (`ddsurface.c`; their critical section is the render thread's too,
so the thread's hold can wait for the renderer [INFERRED]), and the two observers chained on the sprite
blit `0x4B7F90` — `tagpu_fxown.c`'s leaf detour, which reads a DLL byte, and the UI census, which returns
at once off the game thread (`on_game_thread()`). None stores into engine memory.

**So during play no thread but the game thread writes the main block, the explored map or the sight
counters** (DISASSEMBLED inventory; MEASURED by the two sums, in which nothing else had changed).
**Not established**: how long the mouse thread holds its lock per iteration; what Wine's own
DirectPlay and DirectSound threads write [INFERRED: nothing of TotalA's, which hands them no buffer of
its images].

### The in-play publish point, the loader thread and the load flags `main+0x38D75` [VERIFIED by disassembly 2026-09-12; the order MEASURED the same day]

[This project — `objdump -d -M intel` of the pristine build for every reference to
`main+0x38D75`, the thread creation at `0x4982CA`, the import thunks it reaches and the function
boundaries; the ORDER measured live by the log lines of the frame packet's publisher
(`tagpu_packet_pub.c`, landing 1 of the [frame packet exchange](../frame-packet-exchange.html)),
`200v200` at 1920×1080.]

**The image, for the build rule's address ranges** (`objdump -h` of the pristine exe, 2026-09-12):
`.text` at `0x401000` (size `0xFA92A`), `.rdata` at `0x4FC000`, `.data` at `0x501000` (file size
`0x10A00`, its bss extending to the `.tls` at `0x52C000` — which is where `0x511DE8` the main pointer,
`0x511DF0..0x511F80` stock's 100 flying-piece slots (unused under the raised limits, which move them into the DLL), `0x512344` the order descriptors, `0x51FBD0`
the graphics globals and `0x5289A4` the allocator's flag byte live), `.rsrc` at `0x52D000`. So an
engine virtual address is `0x4xxxxx` or `0x5[0-2]xxxx`, which is what `tools/thread-split-check.sh`
looks for.

**One call of `DrawGameScreen 0x468CF0` is the played frame, and its return address names it.**
The four call sites are in the sweep-order table above; only `0x4969CD` — inside the in-game
frame callback `0x496790`, `push ebx; push ebx` with `ebx = 1`, i.e. `DrawGameScreen(1, 1)` —
is the played path, and the address the call pushes, **`0x4969D2`**, is what the publisher gates
every action on. That gate excludes the shell (no call), the movie recorder (`0x4962C2`), the
screenshot function `0x495A30` and, the reason it is a correctness gate and not a filter, every
frame drawn while a level is loading (below). Two facts about `0x495A30` *[name INFERRED from
its loop]*: it is one function, `0x495A30..0x495E7E` (`ret 0x18`), with one caller, `0x417738`,
and it holds both `0x495C76` (`DrawGameScreen(1, 0)` in its loop, after `0x41C4C0` and the
HotUnits cull `0x48BAE0`) and `0x495E66` (`DrawGameScreen(1, 1)`, once, at the end); it never
calls the flip itself. The scenario applier's tick stub lands its `jmp` AT `0x4969D2` while a
scenario is being applied (`tagpu_scenario.c`, stolen `A1 E8 1D 51 00`): that changes the bytes
there, not the return address the call pushed, so the gate holds with it installed.

**A paused frame still reaches `0x4969D2`** [DISASSEMBLED 2026-09-28; MEASURED 2026-09-29: the
demo-recorder probe rebuilt 838 units there on a paused frame, *Driving and rebuilding puppets*,
above]. In `0x496790` the pause tests skip only the tick: the single-player test at
`0x496920..0x496926` (`IsGamePaused`, `main+0x38A51`) jumps over the tick calls `0x495230` and
`0x495490` to `0x496969`, and the network branch's test of the same byte at `0x4968AA..0x4968B0`
also lands at `0x496969`. Every path from there reaches `0x4969CB` and the draw call `0x4969CD`, so
the scenario applier's stub runs on paused frames too, and applies there: MEASURED 2026-10-09,
`resource-mex` launched under a test launch's hold applied its 8 units on frames held at
GameTime 1, which stayed 1. Its watchdog's message names the menus and the mission-end screen
only.

**THE SHELL HAS NO DRAWGAMESCREEN CALL, SO IT HAS NO RETURN ADDRESS TO GATE ON — and the flip
cannot stand in for one [VERIFIED 2026-09-13].** `tagpu_gui_hook` already observes `0x4C63A0`
with an `after` (it hijacks the return to close its census), and `tagpu_detour_observe` refuses
to chain onto a stub that hijacks (tagpu_detour.h, THE CHAIN RULE) — nor can the packet's
observer be installed first, because `tagpu_gui_init` runs before `tagpu_packet_pub_init` in
`dllmain.c`. The shell's publish point is therefore the cursor draw the flip makes, `0x4C67C0`
(above): unowned, once per present, and the only thing there that writes the rect the GL UI
layer needs. **The gate that makes it the shell's and not the game's is TWO tests —
`s_retDepth == 0` AND `!s_levelOpen`** (landing 6). The first is necessary because the cursor
draw runs inside `DrawGameScreen`, so on an in-play frame the observer's own hijack of that
call is on the stack. It is not sufficient, and this note asserted it alone until the
2026-09-14 review: **`s_retDepth == 0` is not the complement of the in-play gate**, because
`DrawGameScreen 0x468CF0` has **four** callers — `0x495C76`, `0x495E66`, `0x4962C2` and the
in-play `0x4969CD` — so three of them return somewhere other than `0x4969D2`. Taking `0x495E66`
(`6a 01 / 6a 01 / e8 85 2e fd ff`) as the example
whose return address is `0x495E6B`, not the in-play `0x4969D2` the observer counts — a
screenshot draw is in-play with `s_retDepth` at 0. `!s_levelOpen` — this module's own "a level
is being played", set at the level's first in-play publish and cleared at the teardown — is
what actually keeps the channel out of a level. `tagpu_packet_pub.c` carries the argument under the
comment *"THE GATE IS TWO TESTS, AND NEITHER IS A DELAY"*, immediately above
`before_cursor`; the tests themselves are that function's first two early returns,
`if (s_retDepth != 0) return 0;` and `if (s_levelOpen) return 0;`. (Cited by
anchor rather than by line: the line numbers this note carried were already
stale one commit after they were written, which is what citing a moving file by
line number always costs.)

**The commands, applied in `before` [landing 2, 2026-09-12].** The same observer's `before`, on
the same in-play gate, is where every engine word the zoom used to write from the render thread
is written now, on the game thread. **The ordering it rests on, verified by disassembly of the
frame callback `0x496790`:** the stepper `0x41CA10` is called at `0x495599` inside `0x495490`,
which the callback calls at `0x49680C` and `0x49693E`, and the scroll poll `0x41CE90` at
`0x496976` — every one of those call sites precedes the draw call at `0x4969CD`, and the
HotUnits cull `0x48BAE0` at `0x49697B` does too. None of them is guaranteed to RUN on a given
frame: `0x496918` (`test [main+0x37EBE],1`, set at `0x45D002` when an in-game GUI screen is
pushed) and `0x49696F` jump straight to `0x49697B`, skipping both the stepper's function and
the poll, and `0x4968B0`/`0x496926`/`0x49693A` skip `0x495490` when the sim is paused or
`[main+0x38A3B]` is 0. So the property the apply actually needs is the other one: **no
instruction in `DrawGameScreen` `0x468CF0..0x46A200` stores to the eye** — all nine references
to `main+0x1431F`/`+0x14323` inside it are loads, the first at `0x468DD9`, ~0xE9 bytes in, past
only `0x4C69A0`, `0x4C2470`, `0x4C6B10`, `0x483FA0` and `0x418310` — so nothing moves the camera
between the apply at the function's entry and the draw's reads, whichever of the engine's own
writers ran that frame, and the frame, its fog rebuild at `0x4848E0`/`0x469D8E` and its minimap
box all see the commanded camera. *[The first draft of this paragraph said the stepper and the
poll "have run for this frame"; the landing review found the skip paths.]* One consequence,
named: the HotUnits cull at `0x49697B` runs before the apply, so on the one draw that applies a
wheel delta the engine's HotUnits list was culled for the pre-delta eye. Everything that list
feeds — the engine's own unit, bar and selection draws — is replaced by passes that gather from
the unit array or the packet, so the skew has no reader of ours; it is the same one-draw skew an
edge scroll applied after the cull would have. The render thread posts
one record per present through a second instance of the packet's own mailbox (latest wins; the
anchor's delta is a cumulative sum, so an overwritten record loses nothing), and
`tagpu_zoom_apply` / `tagpu_vpwide_apply` take the latest one. What they write, and where:

| field | written by the apply | how |
|---|---|---|
| `main+0x1431F`/`+0x14323` (eye) and `+0x14327`/`+0x1432B` (scroll target) | the cursor anchor's delta (`cum − applied`, consumed once), the scenario camera handed over by `tagpu_zoom_place_eye`, the replay's Follow or Jump handed over by `tagpu_zoom_follow` from the replay's frame visit before the draw call `0x4969CD` (since 4c-2, 2026-10-06), the `tagpu_eye.txt` hold, and the camera range's walk-home after a zoom-out | always the pair together, then `clamp_pair` into the range at the commanded level (`zoom_eye_range`). A Follow ends when the target at the apply's entry is not where its last write left it: every player camera move writes the target and nothing else between two applies does (`0x41C3C0` clamps only the eye; the stepper writes the target only from a followed unit, object or remembered position, released on the epoch's first write). Where no apply runs (GDI) `tagpu_zoom_follow` writes the pair itself, at the frame visit, with the same comparison |
| `main+0x1434B`, `+0x142F7`, `+0x142F3` (the follow) | zeroed on the draw that applies a NON-ZERO delta from a gesture that wants the camera, and on the first write of a replay Follow or Jump epoch | the three stores of `0x41C390`, same order, same thread |
| `main+0x142CB` (the minimap view rect) | recomputed through `0x466B70` after any eye the apply moved | the engine's own wrapper, scaled by 1/z as the two redirected sites inside `0x41C3C0` are |
| `main+0x142F1` bit 1 (DrawMinimap's dirty flag) | **set** after any eye the apply moved | the `orb $2` every engine eye writer makes before its `0x41C3C0` call (`0x41C598`, `0x41D04D`, …); safe here because it is the game thread, the one `0x466B16` clears it on |
| `main+0x14281` bit 3 (the screen fog grid is current) | **cleared** after any eye the apply moved | the same clear `0x41CB6B` makes; safe here because it is the game thread (the row on this bit in the camera module) |
| `main+0x1434D` (`ScrollSpeed`) | every in-play draw: `base / z`, or the base when nothing is zoomed | the base re-read whenever the byte is not the one last written |
| `main+0x37E27..0x37E33` (the viewport rect L, T, R, B) | widened to the transform's range at zoom < 1, restored otherwise | `tagpu_vpwide_apply`; W/H at `+0x37E37`/`+0x37E3B` are never written, and a mismatch against the screen-derived size is COUNTED (`vpwh=`, 0 on the landing) instead of repaired, since `0x497F40`'s own stores and ours are on one thread now |

And what the `after` READS for the packet that landing 2 added: the viewport rect as it stands
(`+0x37E27..+0x37E33`, the rect the engine can name), the palette table `main+0x143A7` (1 KB,
copied) and the gamma factor `[0x51FBD0]+0x614` (bounded to 0.05..8.0), plus the apply's own
record of what it applied (`cmd_ack_seq`, the cumulative delta), which is how the render thread
reconciles the eye it predicted. At the level teardown the apply's state is handed back on the
game thread as well — the range flag cleared, `ScrollSpeed` restored to the base, the rect
restored — before the shell draws.

**The observer chain on `0x468CF0`.** `tagpu_menu.c` observes the site first (`before` only, the
tick of its trigger poll; it never hijacks the return) and `tagpu_packet_pub.c` chains after it
(`before` = the commands above, `after` = the publish, post-flip). `tagpu_detour_observe`
chains by rewriting the earlier stub's copy of the stolen bytes into a `jmp` to the new stub, so
the `before`s run in install order and each stub sees the engine's own stack. **An observer that
hijacks the return must be installed AFTER every observer that does not, and there can be only
one hijacker per site in practice**: a later observer reads `((void**)entry_esp)[0]` after an
earlier hijacker replaced it with a trampoline, so its return-address gate would never match. The
menu's observer never hijacks; the publisher's is the last installed (`dllmain.c`) — and since the
landing review `tagpu_detour_observe` REFUSES to chain onto a stub that has an `after`, so the
constraint holds by construction rather than by install order alone.

**The level load runs on a LOADER THREAD, and the in-play gate is what orders the first publish
after it.** The engine review of the plan found what the cross-thread audit had missed; the
disassembly confirms it:

| where | what |
|---|---|
| `0x497F40` | the game-screen enter callback (the one `0x49821D` writes the viewport rect from, §vpwide). Its first test is bit 0 of `main+0x38D75` (`mov cl,[eax+0x38d75]; test cl,1` at `0x497F4B`/`0x497F54`): set → `0x498340`, the POLL path; clear → the first-entry path |
| `0x4982C3`..`0x4982CA` | `push ebp; push ebp; push 0x497C70; call 0x4B6B20` — the thread is created here. `0x4B6B20(start, stack, arg)` is a three-argument wrapper (`ret 0xC`) over the CRT's `0x4E77D0`: a `0x74`-byte thread block, **`CreateThread`** (IAT `0x4FC1A8`) with flags `4` = suspended, then **`ResumeThread`** (IAT `0x4FC240`) — the `_beginthread` shape |
| `0x49832A` | the game thread sets **bit 0** (`or ecx,1`) right after creating the thread, calls `0x45B640`, and falls into `0x498342`: **bit 1** tested (`shr dl,1; test dl,1`), clear → `je 0x4984DD`, the loading-screen path; set → the in-play handler is installed |
| `0x497C70` | the thread's entry: an SEH frame (`push -1; push 0x4FDA48; push 0x4E6718`) around `call 0x497180(arg)` at `0x497CA1`. Stolen bytes `55 8B EC 6A FF` are position-independent, which is what lets an observer sit on it |
| `0x497180` | the loader body: one function, `0x497180..0x497C6C`, one `ret`. First act `QueryPerformanceCounter` (IAT `0x4FC0BC`, it times itself); calls **`LoadGameData_Main 0x4917D0`** at `0x497581` — the routine the *per-map arrays* section left as "entry not traced": its one caller is this thread, and everything that section lists as allocated by the load (layers, projectiles, `LoadMap`, the unit array, the minimap) is allocated **on the loader thread** |
| `0x4975C7` / `0x4975DE..0x4975F1` | the loader sets **bit 2** (`or edx,4`) and then waits for **bit 3** in a loop: `push 0x32; call SleepBatch100ms 0x4B6B50` per iteration *[name from `tools/ta_symbols.txt`]* |
| `0x498539..0x498580` | the game thread's half of that handshake, inside `0x497F40`'s poll path: bit 2 set and `0x4568C0` non-zero → clear bit 2 (`and ecx,0xFFFB` at `0x49855D`), set bit 3 (`or edx,8` at `0x498576`), `call HAPINET_guaranteepackets 0x4C9790`. The in-game frame callback `0x496790` clears bit 2 the same way at `0x496868` (`0x496846..0x49686E`) |
| `0x497C57..0x497C62` | the loader body's LAST act: **bit 1** set (`or ecx,2` at `0x497C5F`, the store at `0x497C62`), then `ret` at `0x497C6C` |
| bits 0/1, the readers | `0x45290E`/`0x452917`, `0x4550C2`/`0x4550CB`, `0x494E8B`/`0x494E94` (`test byte,1` / `test byte,2` pairs), `0x497D28`, `0x497F4B` (bit 0), `0x498342` (bit 1); bit 2: `0x45578A`, `0x498539`, `0x496846` |

The table lists the individual bit updates: `or 1`, `or 2`, `or 4`, `or 8`
and the two `and ~4`. The in-play entry also clears the word by a bulk store:
`0x498478` forms `main+0x38D6F`, `0x49847E` zeroes its first dword and
`0x498480` zeroes the next, covering `main+0x38D73..+0x38D76` and therefore
the complete flag word. These stores follow state 6 at `0x498445` and the
catch-up frame's return. This explains the measured `0x0003` at the first
in-play draw and subsequent `0x0000`; zero flags during play do not mean the
loader is still running.

**The order, measured** (the log of `tagpu_packet_pub.c`, one append per line, so the file's order
is the time order; game thread 736, render thread 744 then 760 after the mode switch):

```
packet: loader thread 756 entered 0x497C70 (entry #1; game thread 736; load flags 0x0001; level gen 0; 0 in-play draw(s) so far)
packet: loader thread 756 leaving 0x497C70 (load flags 0x0003; level gen 0; 0 in-play draw(s) so far; first in-play packet of this level not yet)
packet: font 06C3E3A8 copied: rows=11 yoff=1 first=0x00 glyphs=95 bytes=1612 dropped=0 gen=1 fg=255
packet: level gen 0: first in-play packet at draw #1 (tick 4, load flags now 0x0003, ...)
```

So "the game thread owns all of `main`" is false while the loader runs and true for every in-play
draw — exactly the set the `0x4969D2` gate selects — and the audit's open begin/end hazard closes
by the engine's own ORDERING once the readers take the packet (landing 3), not because any copy is
faster than the load. Nothing of ours may publish or apply outside the gate: the GL UI's minimap
observer, which fires inside `BuildMinimapSurface 0x466780` on *this* thread (the "own thread"
that note measured is the loader thread), goes with landing 4c.

### What the frame packet's publisher copies, per in-play draw [VERIFIED 2026-09-12, landing 3]

[This project — the offsets collected from the nine render-thread files landing 3 of the
[frame packet exchange](../frame-packet-exchange.html) converted, each already binary-verified where
it was first used; the costs MEASURED the same day on `200v200`, Two Continents, 1920×1080,
`--maxfps 0`, 298 published frames a second against a 60 Hz sim.]

The publisher reads exactly this set, on the game thread, inside the `0x4969D2` gate. It is
listed here because it is now **the whole census of what the world needs out of engine memory**:
anything a render-thread pass wants that is not below has to be added here, on this thread, with
its bound.

| Where | Offsets | Bound applied, and by what |
| --- | --- | --- |
| the unit array | walked as `begin + i·0x118`, `i = 1 .. count−1`, `count = u16 main+0x14351` | the COUNT, never the `end` pointer — so the pair that was the audit's open hazard is not read at all. `end == begin + (count−1)·0x118` is asserted for the record (`relbad=`, 0 over every run) |
| one unit record | `+0x64` rot triple, `+0x6A/+0x6E/+0x72` the 16.16 position, `+0x8A`/`+0x8E` the cargo chain, `+0x92` def, `+0x9E` Object3do, `+0xA6` ModelId, `+0xA8` the stable id, `+0xAC` the group tag (a DWORD, as `0x469C55` reads it), `+0xFF` owner, `+0x104` the build fraction, `+0x108` health, `+0x10E` cloak, `+0x110` state | the walk's own bound; `ModelId` is DROPPED unless `< UNITINFOCount`, the cargo links resolve to packet indices or −1 |
| its UnitDef | `+0x20` UnitName (inline, lowercased into 16 bytes), `+0x1FA` MaxHitPoints, `+0x241` the FBI booleans | the pointer is turned into a ROW: it must lie in `main+0x1439B`, be a multiple of `0x249`, and be inside `UNITINFOCount` — otherwise the entry carries no def at all |
| its Object3do | `+0x00` nparts, `+0x10` the composite GAFFrame (W/H/hot x/y at `+0x00..+0x06`, the depth plane at `+0x14` as a flag), `+0x18` the cached body turn, `+0x1E` the base PrimitiveStruct, `+0x22 + i·0x36` the pieces | `nparts` against `TAGPU_PBMAXPIECE`; the base prim becomes an INDEX and must be inside `nparts` |
| one piece | `+0x00` the template node (crosses as a value the fence covers), `+0x04` the COB move, `+0x10` the COB turn, `+0x28` the flags | copied whole, `nparts` of them |
| the feature grid | `main+0x14287`, `0x0D` per cell: `+0x04` height, `+0x08` def index, `+0x0A` wreck index, `+0x0C` flags | the rect is clamped to `main+0x14233`/`+0x14237`; a def index `≥ 0xFFFB` is not an anchor |
| a wreck record | `main+0x1420B + idx·0x30`: `+0x04` Object3do, `+0x08/+0x0C/+0x10` the 16.16 position | reached only from an anchor whose FeatureDef row is inside `main+0x14253` and whose `FeatureMask` bit 0 is clear — **and whose cell index is under the pool's count** (`WR_COUNT` = `TAGPU_LIM_WRECKS`: 2048 in stock, 8192 under the raised limits), the pool `0x421F29` allocates. **[ADDED 2026-09-12, a landing review]** the index had no bound at all before, and this walk is not the engine's: the engine's own read at `0x46A6C4` is equally unbounded but only ever forms the address for a cell it is drawing, where the publisher covers the zoom-floor rect plus a 32-cell margin. The allocator `0x4232A0` returns the count itself when the free list is empty, so the count is the engine's own "no record" value as well as the array's length |
| the frame's option bytes | `main+0x0DCB` the GUI colour array — **256 bytes, not 64** [CORRECTED 2026-09-12, landing 4a: `0x4AC7D0` rebuilds it from `guipal` and its loop at `0x4AC7FF..0x4AC88F` writes exactly `0x100` of them], `+0x2C76`/`+0x2C7A` the dispatched mouse point, `+0x2C92..+0x2CA6` the build cursor's two corners, `+0x2CC3` the cursor mode, `+0x2CC6` the region flags, `+0x37F06` the option byte (damagebars, Shadow, TShadow, FShadow), `+0x1424B`/`+0x1424F` the feature sweep | none needed: they are values, and every consumer of them already treated them as such |
| the map's own features, once a load | **not read by the publisher**: built on the LOADER thread at LoadMap's join `0x483B53` from the TNT's own records, and adopted by the publisher from a hand-over under a lock — see "LoadMap's feature placement, and a saved game" below | the TNT's version, records and threshold from LoadMap's frame; `main+0x14233`/`+0x14237` 1..4096; `NumFeatureDefs` 0..4096 and a def read only under it; the heights of the grid LoadMap has just written; 65 536 entries at most (`TAGPU_PK_TRUNC_MAPFEAT`) |
| the shade table | `[0x51FBD0]+0xC4`, 32 × 256 bytes | the FORMAT is the bound: `0x459C70`'s Gouraud path indexes it with a 5-bit row and a byte, so a copy of exactly that size reads what the rasteriser reads. The unit lane does not remap through it: `tagpu_render3do.c` fits one RGB multiplier per row from it (`shade_k_build`, [GPU status](../gpu-status.html) §2.88) |

### The sound object and its table of 32 playing sounds — `*(main+0x10)` [DISASSEMBLED 2026-09-23; MEASURED 2026-09-23]

`MixingBuffers` is not a site of the limits table: it is a registry value, and the settings store
owns it (`impure.cfg`'s `mixingbuffers`, renderers.md 2.10b). The engine side:

- **The fields.** `+0x2C` MixingBuffers, `+0x30` the sounds in use, `+0x34` a play sequence, and
  one table of **32** slots in three arrays: `+0x38` the buffer, `+0xB8` its sequence number,
  `+0x138` the looping flag. Every loop over it stops at `cmp 0x20`.
- **Who writes `+0x2C`.** The registry loader `0x42F9A0` reads `MixingBuffers` (default 8) at
  `0x42FE4F` and stores it through the setter `0x4CF210`, which takes any value. The getter
  `0x4CF220` has one reader, the registry save (`0x4310A5` / `0x4310AB`).
- **The play `0x4CF570`.** While `+0x30 >= +0x2C` it evicts (`0x4CF5B2..0x4CF5CB`, `0x4CF180`);
  after the play it takes the first empty slot of the 32 (`0x4CF792..0x4CF7A3`) and bumps `+0x30`,
  and **with none free it returns without tracking the sound**. A looping sound is refused while one
  is tracked (`0x4CF592..0x4CF5A3`), so at most one is.
- **The eviction `0x4CF180`** stops the oldest non-looping sound. With none it falls out of its loop
  at 32 and reads slot 32, which is `+0xB8`: past the table.
- **So the table is the bound.** Past 32 the eviction never fires once 32 are playing, and the 33rd
  sound plays untracked, out of reach of the stop-all `0x4CF150`; a looping one (`0x4CF540`, the
  flag at `0x51FF48`) plays on. Below 2, a tracked looping sound leaves the eviction no victim.
  TADR's 128 is past the table; the store offers 8, 16, 24 and 32.
- **MEASURED 2026-09-23**, tier 1 with sound on a null device: `+0x2C` read 32 and `+0x30` stood at
  32 through two minutes of the fight, never above, the sequence at 83 146; the same fight with the
  store at 8 held `+0x30` at 8. Effect sounds also need the registry's `fxvol` (`main+0x37F0C`)
  above zero, which `tacli` sets to 0 on every silent launch.

### Where the engine looks for archives [VERIFIED 2026-09-09]

`InitTAHPIAry 0x41D4C0` globs four patterns through `0x4BC4B0`, in this order, and every one
of them is a **bare relative pattern** — so the process's working directory is where an
archive must land:

| order | pattern | at |
|---|---|---|
| 1 | `rev31.GP3` (`"rev%s.GP3"` `0x5028CC` + `"31"` `0x5028D8`) | `0x41D4E0` |
| 2 | `*.CCX` (`0x5028C4`) | `0x41D537` |
| 3 | `*.UFO` (`0x5028BC`) | `0x41D571` |
| 4 | `*.HPI` (`0x5028B4`) | `0x41D5AB` |

`DDRAW.dll` is the **first static import** of `TotalA.exe`, so `DLL_PROCESS_ATTACH` runs
before the exe's entry point and therefore before this — an archive the DLL writes at attach
is on disk in time by the loader's rules, not by luck. Measured: written at attach, globbed
and read in the same launch.

**What actually opens one is `OpenHAPIFile 0x4BDD70`** — and the name is the engine's own, not
ours: the allocation tag it passes is the string `"OPENHAPIFILE structure"` at `0x50A5A8`.
`stdcall`, `ret 8`. It calls `0x4E4990(name, "rb")` (`0x505F10`) and returns 0 outright if that
fails; otherwise it allocates a `0x118`-byte handle with `0x4D83B0(tag, 0x118)` — the tag is the
FIRST argument — stores the `FILE*` at `+0x00` and `-1` at `+0x04`, and fills `+0x14` with
`GetFullPathNameA(name, 0x104, handle+0x14, …)`, i.e. the **resolved absolute path**, not a
copy of the name it was given. The
36-byte trailer test that decides whether the file counts as an archive at all — the last 36
bytes must be `Copyright 1997 Cavedog Entertainment`, and anything else is skipped silently —
is documented with the rest of the container in [file formats](../file-formats.html) §5
(measured 2026-09-02). `tagpu_ufo.c` writes that trailer for exactly this reason.

**The mount list** [DISASSEMBLED 2026-10-02, for the recorder's `RT_ARCHIVE`]:
- **Where it is.** The graphics globals `*(0x51FBD0)` (`0x4B6220` returns it) hold the list at
  `+0x618`, a `HANDLE**`, and its i32 count at `+0x61C`.
- **The mounter `0x4BE0B0(name, flag)`** (stdcall, `ret 8`):
  - It resolves the name with `GetFullPathNameA` (`0x4BE0D7`) and skips one already mounted,
    compared case-insensitively (`0x4BE0FB`).
  - Otherwise it opens the archive (`0x4BE11B`), reallocates the list to `count + 1` entries on
    every mount (`0x4BE140..0x4BE159`) and appends it.
- **The prune `0x4BE180`** re-opens each entry whose `FILE*` is NULL, and removes one that fails,
  shifting the rest down without reallocating.
- **Writers.** `0x4BE270`, an unmount by handle, has no caller and no stored pointer. The list is
  zeroed at start-up (`0x4B5980`) and freed at exit (`0x4B6110`).
- **Who mounts.** Only `InitTAHPIAry 0x41D4C0` calls the mounter and the prune. Its mounts:
  - `rev31.GP3` with flag 1, `*.CCX` with flag 1, `*.UFO` and `*.HPI` with flag 0;
  - `%c:\*.hpi` on each drive `0x4BB190` returns.
- **Bounds.** `*.HPI` stops after ten successful mounts per call (`0x41D5C0`, `0x41D5D9`); nothing
  bounds the count otherwise. An archive already mounted does not count, so each call mounts the
  next ten, and the list grows over the first calls of a process. **MEASURED 2026-10-02** on
  retail (13 `.hpi`): the main menu of a fresh start held 28 archives, without `totala3.hpi`,
  `totala4.hpi` and `worlds.hpi`; a level held all 31, mounted by the Start's call. So the
  archive fingerprint taken at the first main menu is not the one a level records. The Replays
  browser calls `0x41D4C0` until a call mounts nothing before it takes the fingerprint
  (`tagpu_replays.c`, `mount_all`): from the main menu's dispatch, as MULTI's click calls it
  (`0x425E0B`), and after Leave from `menu_built`, once the main menu's builder `0x4263B0` has
  returned at `0x4270A9`. The engine's own calls in play, at RESTART (`0x460428`) and in the
  load-game dialog (`0x49257A`), show it is not tied to a shell menu. Its stop test is the count,
  which a call can leave unchanged by pruning one archive (`0x4BE180`) and mounting another: it
  may then stop early. `MOUNT_CALLS_MAX` (64) bounds it.
- **The eleven callers** are all on the game thread:
  - WinMain's start-up `0x49E996`;
  - the shell menus `0x41EF71`, `0x425E0B`, `0x4775C9`, `0x47761B`, `0x47772D`, `0x477BC3` and
    `0x478D12`;
  - the skirmish menu's Start `0x47AF2E`, before its `.ota` read;
  - in play, RESTART in the in-game options (`0x460428`) and the load-game dialog (`0x49257A`).
  The loader thread reaches neither the mounter nor `0x41D4C0`. So the list is final by a
  skirmish's first iteration, and a RESTART or a load can change it before the teardown.

**The main menu's version text** is the string at `0x5031B4`, `"v3.1"`, which MAINMENU's
enter function `0x4263B0` puts into its `DebugString` gadget. Seven characters and a NUL fit
before the next string. Some mods patch it into the exe (Escalation `10.2`); others have the
Patch Loader write it at run time (ProTA `4.8`, Mayhem `11.3.0`). Twilight leaves it and shows
its own text in a gadget of its `mainmenu.gui`. [DISASSEMBLED, and read from the compatibility
fixtures, 2026-10-02.]

### DirectDraw recreation and the borderless handoff [DISASSEMBLED + MEASURED 2026-10-04]

All addresses below are from the pristine executable. Direct call sites are from a full-image
`objdump -d -M intel` scan; runtime return addresses are from temporary proxy boundary logs.
The full protocol, captures, controls and unresolved compositor timing are in
[fullscreen-handoff](../fullscreen-handoff.html).

| Address | Established behaviour and ownership |
|---|---|
| `0x4B4FF0` | **`DDrawReleaseObjects` [INFERRED name]**, stdcall(program*), `ret 4`. Tests `program+0x84` for nonzero, then releases and clears palette `+0x94`, attached backbuffer `+0x8C`, primary `+0x88`, clipper `+0x90`, and device `+0x84`, in that order. Does not destroy the game HWND at `+0x40`. Four direct callers: `0x4B5574`, `0x4B5C45`, `0x4B61BC`, `0x4CBB46`. |
| `0x4B5061` / `0x4B5064` | The final device's vtable `+8` Release call / return address inside `0x4B4FF0`. Measured: proxy global ref reaches zero at game entry here, so the renderer join and `g_ddraw` reset happen before the next engine resize. |
| `0x4B5510` | **`DDrawDeviceCreateAndCaps`**, corpus name, stdcall(fullscreen), `ret 4`. Calls `0x4B4FF0` at `0x4B5574`; then resizes the existing HWND before `DirectDrawCreate` at `0x4B55FB`. Direct callers: `0x4B5924`, `0x4B592C`, `0x4B5971`, `0x4B5C3B`, `0x4B5CF3`, `0x4B6250`. Owns recreation of `TAProgram+0x84/+0x88/+0x8C/+0x90/+0x94`; see [resolution](../resolution.html) §1.2. |
| `0x4B55D7` / `0x4B55DD` | `SetWindowPos` import call / return address; HWND `TAProgram+0x40`, size `+0xD4/+0xD8`, flags 6 (`SWP_NOMOVE | SWP_NOZORDER`). Measured after final Release: proxy ref=0, tracked HWND=NULL, actual HWND unchanged. Before the lifetime-property fix, the resize guard was skipped; the same top-level X client shrank 1280×1024 → 1024×768 → 1280×1024, with 13.37 ms between its two size events. |
| `0x4B5940` | **`NewTAScreen`**, corpus name, stdcall(w,h), `ret 8`. Writes `TAProgram+0xD4/+0xD8`, reads bit 1 of `+0xF0`, calls `0x4B5510` at `0x4B5971`. Three direct callers: `0x491B0B` (leave game), `0x4980B7` (loading screen), `0x4983EA` (desired game mode). |
| `0x4B5613`, `0x4B5639` / `0x4B563C` | The replacement device's SetCooperativeLevel (`0x53`) and SetDisplayMode / return address; mode takes `TAProgram+0xD4/+0xD8` and hard-coded bpp 8. Runtime: same HWND `00020058`, proxy borderless state `windowed=1, fullscreen=1`, device mode 1024×768, presentation target 1280×1024. These calls follow the escaping resize. |
| `0x4B6700`, `0x4B6710` | Current **logical** mode width/height getters [INFERRED names], no arguments, plain `ret`. Read `TAProgram*` at `0x51FBD0`, then `+0xD4` / `+0xD8`; no Win32 screen-size query. The loading comparisons therefore read shell 640×480, not the actual 1280×1024 borderless client. Width's 14 direct callers: `0x41CCE9`, `0x46BCA5`, `0x49121B`, `0x491A8F`, `0x494A47`, `0x498044`, `0x498362`, `0x4A823E`, `0x4A826C`, `0x4A82A0`, `0x4A82B3`, `0x4A833E`, `0x4ABF06`, `0x4C648F`. Height's 17: `0x41CCF6`, `0x467DE4`, `0x46A284`, `0x46A8E0`, `0x46AD69`, `0x49122C`, `0x491A9B`, `0x498050`, `0x498375`, `0x4A8252`, `0x4A828C`, `0x4A82C7`, `0x4A82DA`, `0x4A834F`, `0x4A84F9`, `0x4ABF1A`, `0x4C649C`. Full-image direct call/jump scan found no direct jumps to either. |
| `0x498342..0x4983EA` | The loading handler tests `main+0x38D75` bit 1; clear goes to the loading-frame path `0x4984DD`. Set compares current width/height through `0x4B6700` / `0x4B6710` against desired `main+0x37F1B/+0x37F1F` at `0x49836D` / `0x498380`. A mismatch frees `main+0x37E1B`, calls the surface restore helpers `0x4C61F0` / `0x4C62C0`, requests a resize at `0x4983D1`, then calls `NewTAScreen` at `0x4983EA`. Both resizes are now blocked through the actual window-lifetime property; the inner resize escaped the old ref/hwnd guard. Matching 640×480 skips recreation: measured no long end-of-load presentation gap. |

**The loading screen.** Game entry, `0x498109..0x498156`: `0x4290F0(&path, "palettes",
"guipal", …)`, `0x4BBE50(path)` loads the file, then **`0x4AC7D0(GUIInfo, main+0x143A7, data)`**
(`ret 0xC`) copies its 256 entries to `GUIInfo+0xB2` (`main+0x5CB`) and rebuilds the 256-byte
LUT at `GUIInfo+0x8B2` (**`main+0xDCB`**) by nearest RGB match of each `guipal` entry against the
live table (`0x4AC7FF..0x4AC88F`: `|ΔR|+|ΔG|+|ΔB|`, minimum seeded at `0x98967F`) — **so
`guipal` is the GUI's logical palette and touches the live table not at all**; `MEM_Free(data)`;
`0x4B6340()` → `main+0x38A37`. Then, the viewport rect written (`0x4981C9..0x498237`),
**`0x4288D0("loadgame2bg", 0, 0, 0)`** at `0x49823D` — `ret 0x10`, and not the palette init
[resolution](../resolution.html) §2.2 called it. Its second argument (`[esp+0x38]` at `0x4288D3`,
tested at `0x4288DC`) gates a blank-and-present: non-zero makes `main+0x37E1B` the back buffer
(`0x4C69A0`, `0x4288F5`), fills it (`0x4C6890`, `0x4288FC`) and flips (`0x4C63A0`, `0x428901`);
zero, as here, skips all three [DISASSEMBLED 2026-09-29; no flip returning to `0x428906` runs
inside a load, MEASURED the same day]. It looks the name up in a ten-deep most-recent-first
cache of decoded backgrounds (`0x428912..0x428954`; a hit moves the record to the front and
skips the load, `0x428956..0x4289BC`) and on a miss loads the PCX (`0x429290`, `0x4289DE`, with
its own first argument, the name pointer, re-read at `0x4289D3` from `[esp+0x4C]` and pushed
as is, so an observer on the loader's return `0x4289E3` can tell a caller's name by its address
[DISASSEMBLED 2026-10-02]) into
it, unless `main+0x391F1` is 6 (`0x4289F1`, no insertion) (records of 0x28 bytes from `0x5120B8`,
the surface at `+0`, the name at `+4`; the oldest is `SurfaceFree`d and its name `MEM_Free`d at
`0x428A04`), and with no GUI screen on top (`main+0x531` zero, tested at `0x428A7F`) and its fourth
argument zero (`0x428A70`) stores the surface in **`main+0x11EB`** (`0x428AB2`) and its name at
`main+0x11EF` (`0x428AB0`, when the name is non-null); with one on top it hands
the surface to `0x4AB290(GUIInfo, surface)` instead and stores nothing. 42 callers, the shell's
`FrontendX` background among them (`0x478F19`). The surface is a `0x4C69F0` object tagged
`"bitmaps\loadgame2bg.PCX"`, 640×480 [MEASURED 2026-09-29, `gui trace: alloc`]. How the picture
then reaches the frame is *The loading frame*, below.

**Background-cache helpers [DISASSEMBLED 2026-10-05].** `0x428730` walks the ten
`0x28`-byte records from `0x5120B8` to the exclusive end `0x512248`. A non-NULL
surface is freed through `0x4C6AC0` at `0x428741`, and its name through `0x4D85A0`
at `0x42874A`. It clears a matching `main+0x11EB` at `0x428763` and a matching
active GUI screen's background pointer `screen+0x24` at `0x428781`, then zeroes
the record. Direct callers are `0x4916F6` and `0x49835D`. `0x4287B0`, called at
`0x491635`, initializes that cache with 100 zero dwords. The name lookup
`0x4287D0` walks the cached names at `0x5120C0` with the same stride and returns
the associated name pointer at `0x5120BC`; no direct CALL to its entry was found.
That negative result does not exclude an indirect caller. None of these helpers
writes the adjacent `main+0x11E7` gadget-status latch.

## The movie player — mapped by us [DISASSEMBLED + MEASURED 2026-09-26]

The movies are Smacker streams, `Data/1.ZRB` … `5.zrb`, 640×240 at 30 fps (`ffprobe` on the
retail files: `1.zrb`, the intro, is 599 frames and opens with a fade from black; `2.zrb` is
4 058). The decoder's own header reports **480** rows for them (`smk+8`, logged by the DLL), and
the engine's frame shows every second row black: the 240 rows go to alternate lines of a
640×480 box. That look is the engine's own picture, not a renderer artefact.

| address | what | established |
|---|---|---|
| `0x430DEC` | the settings loader reads `PlayMovie` (`0x504398`) into `main+0x3923D`; an absent value stores 1 (`0x430E0E`) | DISASSEMBLED |
| `0x426F06` | the shell's start tests `main+0x3923D`; non-zero plays `"1.zrb"` (`0x50329C`) at `0x426F13`, sets the frontend state `main+0x2BBE` to 1 (`0x426F36`), clears the field (`0x426F5F`) and calls `REGISTRY_SaveSettings 0x430F00`, which writes it back at `0x431723`. From state 1 the frontend plays the cinematic `2.zrb` next: with `PlayMovie` 1 a launch shows `1.zrb` and then `2.zrb`, once | DISASSEMBLED; the cinematic MEASURED |
| `0x426F74` | with `PlayMovie` 0: unless the game was lobby-launched (`main+0x39245`, set by `-c`/`-n`, `cmdline-options.md`), `0x426F81` plays `"1.zrb"` alone and sets state 2 (`0x426FBD`). **So the intro plays at every plain launch that finds `1.zrb`**, and `PlayMovie` only adds the cinematic | DISASSEMBLED + MEASURED |
| `0x4271FE..0x4272AA` | the player's other callers, `2.zrb`–`5.zrb`, reached through the frontend's jump table at `0x4285A0` (entry 0 is `0x4271FE`, `"2.zrb"`). The main menu's INTRO button plays `2.zrb` alone [MEASURED]; which screens reach `3.zrb`–`5.zrb`: not traced | DISASSEMBLED |
| `0x426780(name)` | the player: makes the offscreen the back buffer and flips it once (`0x4267D4`, returning to `0x4267D9`) before anything is decoded, so a movie begins with an ordinary flip; builds the movie object, `operator new(0x5B8)` (`0x4267E0`) and ctor `0x47BDF0`; stores it at `main+0x38D7B` (`0x426817`) and runs the loop from `0x426829` (its only call). After it, the object is destroyed (`0x47BF20`, the only close of its Smack) and freed (`0x426845`); while the loop flag `main+0x39241` is set the player goes back to `0x4267E0` with no flip, so the field dangles between that free and the next store and no flip runs there; otherwise it clears the field (`0x42685C`) and clears and flips the offscreen once more (`0x426892`) | DISASSEMBLED |
| `0x47C6C0` | the loop, `this` = the movie object. While `[obj+8]` is 0: `PeekMessageA` (`0x4FC2D4`) with `PM_REMOVE`; a `WM_CHAR` ends the movie (`[obj+8] = 1` and the loop flag `main+0x39241` cleared); `WM_SYSKEYDOWN` `VK_F4` ends it the same way and calls `PostQuitMessage(0)` (`0x47C762`). With no message, `SmackWait` (`0x47C731`) and, when it answers 0, the frame routine at `0x47C741` (its only call) | DISASSEMBLED |
| `0x47C3A0(hwnd)` | the frame routine. Nothing happens unless `GetFocus()` (`0x47C3A4`) is the argument window and `[obj+8]` is 0. A new palette (`smk+0x68` set, 256 RGB at `smk+0x6C`) is copied into 256 RGBX entries at `obj+0x10` (`0x47C3C8..0x47C3E9`) and handed to the palette's `SetEntries` through `[obj+0x544]+0x10` (`0x47C404`), before the decode; `obj+0x10` keeps it until the next. `*(main+0x37E1B)` (the offscreen) becomes the back buffer (`0x4C69A0`, `0x47C413`); `SmackToBuffer(smk, 0, (480 − H) >> 1, pitch [surf+8], H, bits [surf+0xC], 0)` at `0x47C441` aims the decoder at it, with `H = smk+8` and the unsigned `0x1E0 − H` of `0x47C434..0x47C43B`; `SmackDoFrame(smk)` at `0x47C44A` writes the frame; the flip `0x4C63A0` at `0x47C450` shows it. That is the routine's only call of the flip, so **a flip returning to `0x47C455` is a movie frame and nothing else**. After it, `SmackToBufferRect(smk, 1)` (`0x47C45A`) and the dirty rect at `smk+0x380..` compared with `smk+4`, the width (`0x47C47A..0x47C490`) | DISASSEMBLED |

**No drawing routine touches a movie frame.** The decoder writes the offscreen and the ordinary
flip presents it, so an observer of the engine's leaves sees nothing but flips. The DLL records
the frame at the flip itself (`gui-renderer.md` §25).

**`smackw32.DLL` is imported by ordinal** (ordinal base 1 in its export table). The fifteen IAT
slots from `0x4FC3E0`, with each export's `@n` checked against the pushes at its call:

| slot | ord | export | | slot | ord | export |
|---|---|---|---|---|---|---|
| `0x4FC3E0` | 21 | `SmackNextFrame` | | `0x4FC3FC` | 15 | `SmackSimulate` |
| `0x4FC3E4` | 19 | `SmackDoFrame` | | `0x4FC400` | 38 | `SmackSoundUseDirectSound` |
| `0x4FC3E8` | 23 | `SmackToBuffer` | | `0x4FC404` | 14 | `SmackOpen` |
| `0x4FC3EC` | 20 | `SmackSummary` | | `0x4FC408` | 17 | `SmackSoundOnOff` |
| `0x4FC3F0` | 27 | `SmackGoto` | | `0x4FC40C` | 32 | `SmackWait` |
| `0x4FC3F4` | 28 | `SmackToBufferRect` | | `0x4FC410` | 2 | `SmackBufferOpen` |
| `0x4FC3F8` | 18 | `SmackClose` | | `0x4FC414` | 5 | `SmackBufferNewPalette` |
| | | | | `0x4FC418` | 25 | `SmackColorRemap` |

**`GetFocus` (IAT `0x4FC2E0`) has two callers, both gates**: the frame routine above
(`0x47C3A4`), and the scroll poll's off-screen clamp (`0x41CF1C`, *The camera module*). The
import thunk `0x49F7C4` (`jmp [0x4FC2E0]`) has no callers. An instance whose window does not
have focus therefore sits in the movie loop without ever flipping, which is why the DLL's
armed input shield answers `GetFocus` with the game window.

**Palettes, from the files' frame-type tables** (bit 0 = the frame carries one): every retail
movie sets its palette at frame 0 only, and `5.zrb` (640×304) at frames 0 and 1.

**Pacing is `SmackWait`'s, and it follows the movie's audio clock** [MEASURED 2026-09-26, Wine,
the reference setup]. Into a PulseAudio null sink, a clock that runs in real time, every flip
interval of the intro was 30–37 ms (449 of 449 and 899 of 899 in two runs), the same with the
DLL's movie code on and off. Into Wine's ALSA `null` device the intervals scatter, again with the
movie code on or off: that device's clock is not real time. Under `NoDirectSound` nothing holds
the loop back: the launch's `1.zrb` has flipped all 599 frames before the renderer is up, and
`2.zrb`'s 4 058 go by in seconds.

### `0x4B5330` — the display-mode callback, and why `max_resolutions` is bounded [DISASSEMBLED 2026-09-24]

`stdcall(DDSURFACEDESC* desc, list* ctx)`, `ret 8`, handed to `EnumDisplayModes` by `0x4B5370`
(`call [vt+0x20]` at `0x4B54EB`). Its **only filter** is `desc+0x54 == 8` (the pixel format's
bit count); a match writes `{+0x0C width, +0x08 height, +0x18 refresh}` at `modes + count*12`
(`lea ecx,[ecx+ecx*2]` / `lea ecx,[edx+ecx*4]`) and increments `*ctx` — **with no bounds check**
against the `"DISPLAY MODES"` allocation, `0x4B0` bytes = 100 entries. It always returns 1
(`DDENUMRET_OK`). So the fork's mode list must never offer more than 100 descriptors: this DLL
sets `max_resolutions` 90 in code (`tagpu_cfg.c`, a static assert against 100), since cnc-ddraw's
default of 0 means no cap. The list object and the windowed path: [resolution](../resolution.html) §6.

## The registry — every call site, key and value — mapped by us [DISASSEMBLED + MEASURED 2026-09-25]

What the game asks of the registry. It was mapped for the test-mode registry store,
`tagpu_regstore.c`, which answers these calls from a file beside `TotalA.exe` in every tacli
test launch: a remote test folder's ([tacli design](../tacli-design.html) §"The registry: a file in
the test folder") and a local instance's gamedir (§"The registry: a store per instance"). The disassembly is of
`pristine/TotalA.exe.pristine`. The values come from a logged run of the store under wine
(startup, the menus, a skirmish started through `tacli ui`): the DLL logs every key and value
the first time it is opened, read or written, with the answer.

### The imports, and every call through them

Nine ADVAPI32 imports, all by name, at the start of `.rdata`. Several sites load the slot into
a register once and call through it; the register is given with the load.

| IAT slot | function | call sites |
|---|---|---|
| `0x4FC000` | `RegOpenKeyExA` | `0x49EA8D`, `0x49ECF0`, `0x4B5151` |
| `0x4FC004` | `RegQueryValueExA` | `0x49EAB1`, `0x4B51B1`, `0x4B692D`, `0x4E2D2A`, `0x4E2DBA` |
| `0x4FC008` | `RegCreateKeyA` | `0x4E2C6A`, `0x4E2C7B`, `0x4E2C90` (`ebp`, loaded at `0x4E2C55`) |
| `0x4FC00C` | `RegOpenKeyA` | `0x4DA629`, `0x4DA69C` (`ebx`, `0x4DA610`); `0x4E2C17`, `0x4E2C28`, `0x4E2C3D` (`ebp`, `0x4E2C02`) |
| `0x4FC010` | `RegQueryValueA` | `0x4DA644`, `0x4DA6BA` |
| `0x4FC014` | `RegCreateKeyExA` | `0x4B68CB`, `0x4B68EE`, `0x4B690D` (`edi`, `0x4B689F`) |
| `0x4FC018` | `RegSetValueExA` | `0x49EACE`, `0x49ED17`, `0x4B6961`, `0x4E2D83`, `0x4E2E13` |
| `0x4FC01C` | `RegFlushKey` | `0x49EB63`, `0x49ED22` |
| `0x4FC020` | `RegCloseKey` | `0x49EB6E`, `0x49ED2D`, `0x4B51BE`, `0x4B6985`/`0x4B6990`/`0x4B699B` (`esi`, `0x4B697A`), `0x4DA65D`, `0x4DA6C9` (`ebp`, `0x4DA656`) |

**The import slots are the only way in** (negative results):

- the nine `jmp [slot]` thunks (`0x49F83C..0x49F854`, `0x4FB422..0x4FB434`) have no caller: no
  relative call or jump reaches them, and no absolute pointer to them is in the file;
- no registry function is looked up by name: the only strings of the exe that name one are the
  import table's;
- `RegFlushKey` is called only on the CD autoplay key (below). TA never flushes its own key.

### The six sites

1. **The CD autoplay verb** — in `0x49E830` [INFERRED: WinMain; its one caller is `0x49EDDD`,
   the call TADR hooks as the entry point]. At startup (`0x49EA67..0x49EB6E`, right after
   `UIPipelinesInit 0x491200` at `0x49EA62`) it opens `HKLM\SOFTWARE\Classes\AudioCD\shell`
   (`0x5097B0`) with `KEY_ALL_ACCESS` (`0xF003F`), and skips everything below if the open fails
   (`jne 0x49EB74`). If the key has a default value (read into 50 bytes), it sets the default to
   the empty string (`0x5119B8`, a zeroed `.data` string that other sites push as `""`), which
   turns CD autoplay off while the game runs. If TA's own `cdshell` (`0x5097A8`, read through
   `0x42F980`) is empty, it saves the old default there (`0x42F960`, REG_BINARY). Then it
   flushes and closes. At exit (`0x49ECDB..0x49ED6A`, after the message loop) it reopens the
   key the same way, writes the saved default back, flushes, closes, and saves an empty
   `cdshell` (`0x49ED65`).
2. **The DirectX version check `0x4B5070`** (one caller, `0x4266A0`; its warning is the one
   `tagpu_patches.c` patches out at `0x4266A7`): `RegOpenKeyExA(HKLM, "Software\Microsoft\DirectX",
   KEY_READ)` at `0x4B5151`, one read of `InstalledVersion` (`0x4FDB80`) or `Version` (`0x4FDB78`),
   chosen at `0x4B5169`, at `0x4B51B1`, and a close. Read-only.
3. **The settings accessor `0x4B6880(subkey, name, data, size*, type, read)`**, `ret 0x18`. It
   opens three levels with `RegCreateKeyExA` from `HKCU`: `Software` (`0x509ED0`), `Cavedog
   Entertainment` (`0x509EB8`), then `subkey`, asking for `KEY_READ` (`0x20019`) when reading
   and `KEY_WRITE` (`0x20006`) when writing, so **a read creates the keys it does not find**.
   Then it calls `RegQueryValueExA` with no type pointer (success and `ERROR_MORE_DATA`, 0xEA,
   both count as success) or `RegSetValueExA(type, data, *size)`, and closes all three. The
   callers pass the section `Total Annihilation` (`0x5032E8`), except the six values of each
   skirmish player, whose section is `Total Annihilation\Skirmish` (`0x503300`): six callers of
   the dword read from `0x430C40` on and six of the dword write from `0x43161B` on. Its
   wrappers:

   | wrapper | what | callers |
   |---|---|---|
   | `0x4B6860`, `0x4B69B0` | read raw bytes into `buf` (size in and out) | 1 (`0x42F980`), 6 |
   | `0x4B69D0` | read a dword | 61 |
   | `0x4B6A00` | write REG_BINARY | 1 (`0x42F960`) |
   | `0x4B6A20` | write a string (REG_SZ, `strlen + 1`) | 7 |
   | `0x4B6A50` | write a dword (`SaveSetting`) | 92 |

   **Escalation GOLD 10.2.0 variant [DISASSEMBLED 2026-09-28]:** the same accessor's
   `0x4B68B1` still pushes `Software` at `0x509ED0`, but `0x4B68E8` pushes `TA Esc` at
   `0x509EB8`. The skirmish caller `0x430C3B` still supplies `0x503300`, so the effective
   key is `HKCU\Software\TA Esc\Total Annihilation\Skirmish`, independent of TADR.
   `0x50DDF4` also contains the combined root string. The option accessor `0x49F5A0`
   formats the executable directory with `0x5098A0` at `0x49F5CE`; in this executable
   that format is `%s\taesc.ini`, not retail's `%s\totala.ini`. Its
   `GetPrivateProfileIntA` call at `0x49F5F9` reads the `Preferences` section
   (`0x509894`). A test which edits retail's settings is therefore not configuring ESC.
   The highest-ID fixture observed the consequence: retail-key/ini edits left the match
   at 1000 units per player with player nine absent. See [the COB gate](../tadr-port/cob.md).

   The dword wrappers carry the options: the loader `0x42F9A0` and the saver
   `REGISTRY_SaveSettings 0x430F00` ("The Visuals options the store owns" above). `0x42F980` and
   `0x42F960` are the raw read and the binary write with the section bound, used for `cdshell`,
   `language` (read at `0x49E9F7`), `CDLISTS` (read at `0x490F58` and `0x491412`, written at
   `0x490FD1` and `0x4916F1`) and the multiplayer connection values (`TCPADDR`, `SERBAUD`,
   `SERPORT`, `MODEMNUMBERS`, …).
4. **Open a document, `0x4DA5B0(hwnd, file)`**: `ShellExecuteA(hwnd, "open", file, NULL, ".",
   SW_SHOWNORMAL)` at `0x4DA5DD`. When that fails, it reads the file type's association from
   `HKCR` by hand: `RegOpenKeyA` on the extension (found with `strrchr(file, '.')`) and
   `RegQueryValueA` for its class, then `%s\shell\open\command` (`0x50D318`) and
   `RegQueryValueA` for the command, and runs that (`0x4DA730`). Read-only. Callers `0x4DF974`
   and `0x4E132A` [screens not identified]; the logged runs never reached it.
5. **A registry-key object, `0x4E2BE0(this, read, subkey, section)`**, `ret 0xC`. It opens
   (`RegOpenKeyA`, for reading) or creates (`RegCreateKeyA`, for writing)
   `HKCU\Software\Cavedog Entertainment` (`0x50DDF4`), then `section` (the string at
   `*0x529E80` when NULL), then `subkey`. Its four constructions name the engine library's own
   keys [INFERRED from the names]: `CavedogLibrary` (`0x50D74C`) with `PerformanceSettings`
   (`0x50DD84`) at `0x4E1B26` and with a computed subkey at `0x4E053A`, and `Cavedog library`
   (`0x50DE40`) with a computed subkey at `0x4E3140` (reading) and `0x4E3481` (writing). It keeps the last handle at `this+0` and the direction at `this+4`, and
   **never closes any of the three**: no `RegCloseKey` is reached from it or its methods, so
   every construction leaks three handles. Its value methods clamp: `0x4E2D00` (signed) and
   `0x4E2D90` (unsigned) read a `REG_DWORD` of exactly 4 bytes and clamp it to `[min, max]`, or
   return the default; `0x4E2D70` and `0x4E2E00` write a `REG_DWORD`. The exchangers `0x4E2E20`,
   `0x4E2E60`, `0x4E2EA0`, … pick the read or the write on `this+4`, so one routine both loads
   and saves a record. The logged runs never reached it: no key under either section was
   opened.
6. **The `-r` switch** reaches the registry through no import of its own: the handler
   `0x49F249` loads `dsetup.dll` (`0x4FDAC0`) and calls `DirectXRegisterApplicationA`
   (`0x4FDAA0`, found with `GetProcAddress`), which writes DirectPlay's application key through
   `dsetup.dll`'s own imports, then quits ([command-line options](../cmdline-options.html)). In
   test mode `tagpu_patches.c` points two entries of the switch's jump table `0x49F494` (indexed
   through the byte table `0x49F500`, `jmp [ecx*4+0x49F494]` at `0x49EF33`) (`0x49F4B8` for `R`,
   `0x49F4EC` for `r`, both `0x49F249`) at the loop tail `0x49F461`, where every unknown letter
   goes (entry 26, `0x49F4FC`). The parser is `CmdlineArgsNormalize 0x49EE30` (called at
   `0x49E8D2`, its token loop head `0x49EED3`); the same rule is what lets tacli's token
   `-xtacli-test` through untouched ([command-line options](../cmdline-options.html)).
   **MEASURED**: `TotalA.exe -r` in a test-mode instance logged `the -r switch (DirectPlay
   registration through dsetup.dll) is ignored` and stayed in its front end until stopped 45 s
   later; stock, the switch quits.

### What the game reads and writes [MEASURED 2026-09-25]

The logged run, all under `HKCU\Software\Cavedog Entertainment\Total Annihilation`:

- **Read and written** (52 values): `Interface Type`, `DisplaymodeWidth`,
  `DisplaymodeHeight`, `side`, `Difficulty`, `scrollspeed`, `Single`/`Multi`/`Skirmish` ×
  `CommanderDeath`, `Mapping`, `LineOfSight`, `LOSType`, `SkirmishDifficulty`,
  `SkirmishLocation`, `SkirmishMap`, `screenchat`, `damagebars`, `Sound Mode`, `MixingBuffers`,
  `RestoreVolume`, `Anti-Alias`, `Shadows`, `FeatureShadows`, `VehicleShadows`, `Shading`,
  `DitheredFog`, `Gamma`, `SwitchAlt`, `Password`, `Nickname`, `Game Name`, `textlines`,
  `textscroll`, `mousespeed`, `gamespeed`, `unitchat`, `unitchattext`, `musicmode`, `cdmode`,
  `ackfx`, `buildfx`, `speechfx`, `fxvol`, `musicvol`, `clock`, `PlayMovie`; and
  `FixedLocations`, written without being read.
- **Read only**: `CDLISTS`, and, absent in that key, `language`, `Image Output Directory`,
  `Movie Output Rate`, `NumSkirmishPlayers`, `DisplaymodeDepth`, `AllMissions`.
- **`Skirmish\`**: `Player0..3` × `Controller`, `Side`, `Color`, `AllyGroup`, `Metal`, `Energy`,
  read and written (24 values).
- Outside the key: the CD autoplay key (refused in test mode, so `cdshell` was never reached)
  and the DirectX version (passed through, read-only).

The store's counters at its last rewrite of the file in that run (a stopped game logs no exit
line): 424 opens, 164 reads (11 of them absent) and 48 writes. The accessor (site 3) opens the
keys again for every value, hence the opens.

**`MixingBuffers` is the settings store's value, written into the registry by the game.** The
loader reads it at `0x42FE4F` into the sound object through `0x4CF210`; `tagpu_menu.c`'s
`eng_push_mixing` then writes `impure.cfg`'s value into the same field (`+0x2C`) after every
load; and the saver reads that field back (`0x4CF220` at `0x4310A5`) and writes it at
`0x4310AB`. So every launch whose saver runs writes `impure.cfg`'s mixing buffers into the
registry: a player's into the player's key, a tacli instance's into its store (the logged run
wrote 32).

### Outside TotalA.exe's own code

- **`win32.dll`**, which the Steam/GOG build imports where the retail build imported `WINMM.dll`
  ([binary patches](../binary-patches.html)), imports `RegOpenKeyExA` and `RegQueryValueExA` and
  nothing else of the registry. It reads `musicvol` (`0x100011AE`, `0x100011D4`) and `cdmode`
  (`0x10001259`, `0x1000127F`) from `HKCU\SOFTWARE\Cavedog Entertainment\Total Annihilation\`
  with `KEY_READ`, and imports no `RegCloseKey`, so it never closes them [DISASSEMBLED, image
  base `0x10000000`]. The store hooks its two imports as well.
- **`online.dll` and what it loads and starts.** `TotalA.exe` loads `online.dll` itself: a
  path built into `0x512DD0` from `online.dll` (`0x4FD4D0`, at `0x45B30B`), then `LoadLibraryA`
  at `0x45B33F` (the same at `0x45B557` and `0x45B747`). `online.dll` imports no registry
  function, but it searches the game folder for `ta*.dll` and loads each as a **Total
  Annihilation Extension DLL**: `tamplayx.dll`, `takalix.dll`, `taheatx.dll`, `tawirepx.dll`,
  `tadwngox.dll` and `tatenx.dll` export exactly `TotalAExtVersion`, `TotalAExtGetButtonText`
  and `TotalAExtAction` (they are not DirectPlay service providers). The first four import
  `RegOpenKeyExA`, `RegQueryValueExA` and `RegCloseKey`, the other two nothing of the registry.
  `online.dll` starts each online service's client with `CreateProcessA` or `ShellExecuteA`
  (its strings: `online.exe returned from CreateProcess`, `exec'ing command`). A client started
  that way is another process, and that is what would load `mptaext.dll`: its exports are
  Mplayer's offer negotiation (`MPOpenOffer`, `MPPrelaunchOffer`, …), it imports
  `RegSetValueExA`, and no string of `TotalA.exe` or `online.dll` names it [INFERRED from the
  exports]. `audiere.dll` and `smackw32.dll` import no registry function. Neither
  `reporter.dll` nor `DebugHelper.dll`, which `TotalA.exe` also loads by name, is in the Steam
  install.
- **Our DLL**: the fork's `debug.c` reads `HKLM` version values, in `make DEBUG=1` builds only;
  `utils.c` reads a Voobly key only when `age.dll` is loaded; `indeo.c` writes four `vidc.iv*`
  values under `HKCU\…\Drivers32` at attach and deletes them at detach, which test mode skips
  (the game plays Smacker and loads no Video for Windows codec). `tagpu_zoom.c` saves
  `scrollspeed` through `SaveSetting 0x4B6A50`, so it reaches the registry through TA's own
  imports.
- **Not examined**: the system DLLs that may touch the registry for the game (DirectPlay,
  DirectSound); what `ShellExecuteA` starts, from `0x4DA5B0` or from `online.dll`; what the
  extension DLLs read, what `online.dll`'s child processes do, and `mptaext.dll` itself; and
  the other names `TotalA.exe` gives `LoadLibraryA` (`DebugHelper.dll`, `reporter.dll`,
  `IMAGEHLP.DLL`, `psapi.dll`). None of it is hooked, so none of it is covered by the test
  mode's guarantee ([tacli design](../tacli-design.html) §"The registry: a file in the test
  folder").
