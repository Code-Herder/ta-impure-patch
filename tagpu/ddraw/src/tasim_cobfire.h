#ifndef TASIM_COBFIRE_H
#define TASIM_COBFIRE_H
/* tasim_cobfire -- THE SCRIPTED RELOAD: the ticks between a weapon slot's shots as its unit's own COB script times
   them, for ImpAI's content (tagpu_worldq.c: the classifier's input and the fight predictor's types).

   A weapon's reloadtime (+0xE4) is a floor, not its rate: AutoAim 0x49E1A0 starts the slot's aim script when the
   slot's state has bit 0 clear, fires at reload 0 only once that script has returned non-zero, and the fire clears
   the result and bit 0 (0x49D78E..0x49D797 for a turret, 0x49DD36..0x49DD38 for a vertical launch), so the next
   shot waits for the next aim's return as well as for the reload (engine/weapons.md, *AutoAim's slot loop*). A
   script that holds its aim while a flag its fire script set is up -- Escalation's `reloadtime=0.125; //script
   timed` weapons, whose FirePrimary sleeps 150 + 2 250 ms before it lowers the flag -- fires at its script's rate:
   ESC's ARMSAM at 76 ticks against a declared 3, as the shots of its games measured (2.53 s).

   THE PROBE runs the type's validated program in tagpu_cobvm (the engine's runner, opcode for opcode) under that
   protocol, the target still and dead ahead: Create (0x485DE6, run-now), SetMaxReloadTime with the slots' largest
   reload in ms (0x49E186), then a tick at a time the reload countdown run down (0x49E1C8), the aim started with
   heading and pitch 0 when none is pending (0x49E31C), a shot at countdown 0 once the aim returned non-zero -- the
   fire script started (0x49CB94), the countdown set to the reload, the aim cleared -- and the unit's scripts run
   (DoScriptsNow 0x4B0D60, after AutoAim in the unit's turn). The interval is the one between the second and the
   third shots, past the first aim's slew.

   SAFE BY CONSTRUCTION: the program is tagpu_cob_program's, validated; the VM refuses a fault; every run is bounded
   by TASIM_FIRE_SPAN ticks of the VM's own bounded steps. Anything the probe cannot rate -- no aim script, a fault,
   fewer than three shots in the span -- keeps the declared reload, and an answer is never below it: the countdown
   holds every shot to it.

   Pure: no engine memory, no Win32, no static, no allocation; the VM's blocks are the caller's (tasim_cobfire_work).
   RESIDUAL: a target that moves, a slot whose scripts share a flag with another slot's (each slot is probed alone,
   as against a target only it engages), a script that reads the unit's state through `get` (answered 0) or times
   itself by its health. */
#include <stddef.h>
#include <stdint.h>
#include "tagpu_cob_safe.h"

#define TASIM_FIRE_SLOTS 3u     /* the slots the engine names an aim script for: Primary, Secondary, Tertiary
                                   (0x509688; 0x509678 the fire scripts) */
#define TASIM_FIRE_SPAN  900u   /* the probe's ticks, 30 s: three shots of a scripted interval up to 15 s */

/* the bytes of the blocks tasim_cobfire_interval runs `program` in */
size_t   tasim_cobfire_work(const TagpuCobProgram* program);

/* slot `slot`'s interval between shots in ticks, at least `reload` (its weapon's +0xE4), `reload` when the probe
   cannot rate it; `max_reload` is the largest of the unit's three slots' reloads (SetMaxReloadTime's); `work` holds
   tasim_cobfire_work(program) bytes, the caller's */
uint32_t tasim_cobfire_interval(const TagpuCobProgram* program, unsigned slot, uint32_t reload, uint32_t max_reload,
                                void* work, size_t bytes);

#endif
