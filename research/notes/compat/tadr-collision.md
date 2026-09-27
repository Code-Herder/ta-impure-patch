# TADR beside Impure: a refusal and a crash

## Summary

What happens when TADR starts beside Impure on the
[Patch Loader route](overview.md#the-load-routes), where Impure has installed first. A 2026 TADR
validates the bytes it is about to patch and refuses to start (**"TADR engine-limit error"**). An
older TADR, the "Install Limit Crack" that Total Mayhem 11.3.0 and ProTA 4.8 ship, writes
without reading, rewrites 17 of Impure's sites, and **crashes the game on the first skirmish
load**. Since [the takeover](takeover.md)'s first landing TADR does not start on this route, and
if one got through by another way, the safety net stops the game at the first DirectDraw call
instead; this page is what both of them exist for, and what Impure v0.2.3 players met. The engine facts are in the
[engine map](../exe-reverse-engineering.md#where-other-patchers-meet-ours-the-patch-loaders-hand-off-and-tadrs-limit-crack-disassembled-measured-2026-09-26).

## 2026 TADR: EngineLimits refuses

**From 586d71a, 2026-08-23.** At its `DirectDrawCreate` it validates
the stock bytes of every site it is about to raise and, on the first mismatch,
`EngineLimits::AbortIfInstallFailed` (`vendor/TADR/src/DDraw/EngineLimits.cpp:572`) shows
"TADR engine-limit error" and calls `ExitProcess(0xC1)`. Beside Impure the first mismatch is
the projectile allocation size at `0x499A32`, which Impure has already raised. The player's
report is exactly this line: *address validation failed for projectile allocation size at
0x00499A32*. It is the mirror of Impure's own refusal: whoever checks second refuses.

## Pre-2026 TADR: the limit crack

`LimitCrack.cpp` writes its raised limits through `ModifyHook` and `SingleHook` without reading what is there. A `SingleHook` writes an
immediate, and where both raise the same count to the same value it is harmless (`0x488CD3`
reads `B9 00 02 00 00` afterwards: `0x200` dwords, the same from both). A `ModifyHook` is not:
it copies the bytes at its site as if they were stock, re-assembles them into a stub behind its
own replacement, and jumps back past them (`hook/ModifyHook.cpp`). Twelve of its sites are
exactly Impure's unit-type relocations — `0x406DB5`, `0x406DC9`, `0x406DFD`, `0x406E3A`
(the AI plan's `Weight`), `0x406E45`, `0x406E5D`, `0x406E64`, `0x406EB2`, `0x406ED6` (its
`Limit`), `0x488CC2` (the category-mask allocation), `0x48BE08`, `0x48BF1E` (Ctrl-Z) — in the
current source. Total Mayhem's 2024 build, measured by the safety net with the takeover
switched off (the suite's `mayhem-11.3.0-net`), rewrites 17 of Impure's 265 sites: ten of those
(not Ctrl-Z's two), and seven of the fixes Impure ported from TADR, whose hooks sit at the same
places — the whole build lists `0x42DAC7`, stacked aircraft `0x4954ED`, the stale-hit sites
`0x486036`, `0x486DC1`, `0x4854A0`, the wind `0x490C5A` and the yardmap parse `0x42CF5E`.

## The crash, read from the running process

Total Mayhem 11.3.0 with the DLL at `main`, on Wine, 3 runs of 3; ProTA 4.8 crashes with the
same `ErrorLog.txt`. Impure puts a 7-byte `jmp` to its own stub (`push MB; call 0x4B4F10;
jmp 0x488CC9`) at `0x488CC2`. TADR's `ModifyHook(0x488CC2, 7 bytes, "push New", redirect from +2)`
then decodes Impure's jump displacement as instructions, relocates them, and NOPs through
`0x488CCB`, erasing the stock `mov esi,eax` and the first byte of `add esp,4`. The bytes at
`0x488CC2` afterwards are `E9 <TADR stub> 90 90 90 90 90`, and the stub is:

```
push 0x800                   ; TADR's mask size
jae  0x488D0D                ; Impure's displacement bytes 73 47, relocated
add  [eax+0x83F08B90], edx   ; the rest of Impure's bytes and the stock mov esi,eax
jmp  0x488CCC
```

The carry flag is clear on every path into `0x488CC2`, so the `jae` is taken: the allocation
never runs, and the function `0x488C50` (the unit-category name map) reaches its epilogue with
the `push 0x800` still on the stack. Its string destructor at `0x4C9390` then treats its own
return address as a string: `ErrorLog.txt` reads *Access Violation … at 0023:004c9396, Illegal
write, data address 0x0042C029*, in the Load Thread, with `0x800` on top of the stack and
`0x0042C02D`, the return address of the call at `0x42C028`, where the string should be. The
function allocates only for a name its map does not hold yet, which the Load Thread reaches
while it loads the skirmish and not at the menu. The exact bytes TADR
ends up with depend on where Impure's stub is allocated, so another build or another machine
can fail differently at the same place; the collision itself does not depend on it.

Impure v0.2.3 is the first release with these sites (the 16 384-type limit); v0.2.2 has none
of them, so this collision cannot happen with it. v0.2.2 beside Mayhem was not run to a battle.
