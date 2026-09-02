#ifndef TAGPU_SCENARIO_H
#define TAGPU_SCENARIO_H

#include "tagpu.h"

/* tagpu_scenario — the applier: a compiled wire file becomes a live situation.

   Phase C of `tacli scenario` (design: research/notes/scenario-format.md). The
   CLI compiles a JSON scenario into a private, versioned, line-oriented wire
   file; this module scans it with a fixed-vocabulary parser and calls the
   engine's OWN creation functions. The DLL never parses JSON in — one testable
   component decides what reaches the engine — but it does write JSON out.

       tagpu_scenario.trigger  -> tagpu_scenario.json    (apply a situation)
       tagpu_switches.trigger  -> tagpu_switches.json    (SoftwareDebugMode bits)

   Two paths, deliberately split:

     * DETECT and REPORT run in the present path (tagpu_overlay_draw), where the
       fork already does its file I/O. Parsing a stale or truncated file costs a
       result line, never a sim tick.
     * APPLY runs from a detour inside Game_MainLoopTick, never mid-render: the
       render path walks the sort grid, and creating a unit relinks it. Every
       entity lands in ONE visit, so the situation is reproducible.

   The hook site is 0x004969D2 (`mov eax, ds:0x511DE8`), five position-
   independent bytes an E9 fits exactly, in the straight-line block that
   0x004969CB — TADR's GameTickHook address — falls into. TADR's own note says
   that site fires 9-15x per simulation tick, so this module gates itself on a
   one-way state machine instead of on the call count.

   Everything the engine is asked to do is validated twice: once by the CLI, and
   again here, because the file is on disk and can be stale. A name that does
   not resolve, a coordinate outside the live map or an ordinal out of range
   means NOTHING is created (`onerror=abort`), and the result file says which
   line was wrong. */

void tagpu_scenario_frame(const TAGPU_FRAME* f);

#endif
