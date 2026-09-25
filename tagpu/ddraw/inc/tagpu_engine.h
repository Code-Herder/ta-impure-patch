#ifndef TAGPU_ENGINE_H
#define TAGPU_ENGINE_H
/* tagpu_engine.h — the engine's addresses, for PUBLISHER files only.

   THE RULE (frame-packet-exchange.md §10). Every engine address and every
   `main+` offset the frame packet's publisher reads lives here, and only a
   publisher — game-thread code inside an engine call, at a site that owns
   the memory it reads — may include this header. `tools/thread-split-check.sh`
   fails the build for any file outside `tagpu/ddraw/thread-split.allow` that
   includes it, names a virtual address, probes with IsBad*Ptr or adds an
   offset to the main pointer. The list only shrinks: each change that
   converts a render-thread reader to the packet removes its line.

   Not every engine address is here yet. The other modules still carry their
   own `#define`s; they move here as they are converted, not before, so a
   conversion's diff shows exactly what left the render thread. Every value
   here is established in research/notes/exe-reverse-engineering.md, by
   disassembly of the pristine 3.1 exe or by a live measurement that the note
   quotes. */

/* ---- the root ------------------------------------------------------------ */
#define TA_MAIN_PP         0x00511DE8u  /* TAdynmemStruct**: `mov eax,ds:0x511de8` */
#define TA_GFX_PP          0x0051FBD0u  /* the graphics globals: 0x4B6220 returns it */

/* ---- the publish point ---------------------------------------------------- */
#define VA_DRAWGAMESCREEN  0x00468CF0u  /* DrawGameScreen(drawUnits, blitScreen)   */
#define VA_DRAW_RET_INPLAY 0x004969D2u  /* the return address of the in-play call: */
                                        /* 0x4969CD `call 0x468cf0` inside the     */
                                        /* frame callback 0x496790, both args 1    */
/* THE SHELL'S PUBLISH POINT. The shell never calls DrawGameScreen, so the
   frame packet's gate has nothing to select on: what the shell does have is
   the flip 0x4C63A0, which every present takes and which draws the cursor
   itself. The flip cannot be observed a second time — tagpu_gui_hook's
   observer on it hijacks the return, and tagpu_detour_observe refuses to
   chain onto a hijacker (THE CHAIN RULE) — so the shell's channel hangs off
   the cursor draw the flip makes: the one thing inside it that writes the
   drawn position, and the only per-present site there that is unowned.

   `stdcall(mouseObj, surface)`, `ret 8`, prologue `56 8B 74 24 08 8B 86 CE 01
   00 00` (11 bytes, resuming at 0x4C67CB). It early-outs unless the globals'
   +0x1CE, +0x1D2 and +0x1B2 are ALL non-zero, and only then computes
   +0x1B6/+0x1BA from the mouse record minus the hotspot. Two call sites,
   0x4C641B (the GDI arm, `[+0xF0] & 2` clear) and 0x4C6544 (the DirectDraw
   arm) — mutually exclusive arms of the flip, so it runs AT MOST once per
   present, in play and in the shell alike. "At most", not "exactly": the
   DirectDraw arm leaves before reaching its call on three exits — 0x4C666A
   (no primary, `[+0xDC] == 0`), 0x4C67B0 (the back buffer's size disagrees
   with the screen) and 0x4C65A0 (`Lock` failed / DDERR_SURFACELOST) — and on
   such a present the engine draws no cursor at all. Nothing then publishes
   and the consumer keeps the previous state, which is one frame of a stale
   rect on paths that mean the primary is gone anyway; named here because an
   observer on this site cannot see them. [VERIFIED 2026-09-13 by disassembly
   of the pristine exe; the live shell runs with all three words non-zero and
   +0x1B6/+0x1BA == +0x196/+0x19A, and `gui off` presents the cursor the layer
   was covering.] */
#define VA_CURSOR_DRAW     0x004C67C0u

/* THE IN-GAME CURSOR DRAW, AND NOT THE FLIP'S. MEASURED 2026-09-13: in play the
   flip's draw above is ENTERED ~5900 times a second and never once draws (its
   siblings' gates leave it nothing to do), while THIS one is what writes
   +0x1B6/+0x1BA. It writes the RECORD +0x196/+0x19A from its own poll's
   answer (0x4C2624), then subtracts the hotspot at 0x4C2638/0x4C2645 and
   stores the pair at 0x4C284C/0x4C2852 — pos minus hotspot, as the flip's
   draw computes it too (GFX_CUR_X below has the measurement).

   `stdcall(mouseObj)`, `ret 4` (epilogue 0x4C2864), prologue `83 EC 58 56 8B 74
   24 60` (8 bytes, resuming 0x4C25E8). Two early-outs, both to 0x4C2860:
   `[+0x1D2] == 0`, and the context acquire 0x4C5FF0 answering 0. Past them it
   polls GetCursorPos (IAT 0x4FC2E4), writes the RECORD +0x196/+0x19A from that
   answer, fills the saved-background descriptors at +0x1C2/+0x1C6 from the
   sprite record, blits through 0x4B7F90 with the hotspot, and stores
   +0x1B6/+0x1BA. ONE caller, 0x4C2A0D inside 0x4C2990 — itself reached
   INDIRECTLY (no `call 0x4C2990` in the image), the mouse object's per-frame
   update.

   NOTHING PATCHES OR OBSERVES THIS FUNCTION, nor the other two draw paths
   (blit calls at 0x4C297E and 0x4C258C); the only cursor site hooked is
   VA_CURSOR_DRAW above (tagpu_packet_pub.c). 0x4C2870 is inert while the
   mouse thread is up (its first gate is `cmp [+0x1CE], 1` — 0x4C287D loads
   edi = 1 — and +0x1CE is 1 for exactly as long as that thread lives), and
   0x4C24B0 has no call site at all.
   */
#define VA_CURSOR_POLL     0x004C25E0u

/* the loader thread: created at 0x4982CA (`push 0x497C70; call 0x4B6B20`,
   the CRT's _beginthread over CreateThread + ResumeThread), its entry the SEH
   wrapper 0x497C70 -> 0x497180, whose last act sets bit 1 of TA_LOADFLAGS */
#define VA_LOADER_ENTRY    0x00497C70u
/* LoadMap 0x483610 (loader thread; its one caller 0x4918C0): every path through
   the TNT's features joins here -- a fresh game's placement loops, a saved
   game's skip of them (0x483A44 / 0x483B0A on OFF_SAVEDGAME), and the v2 path's
   schema features 0x423160 -- before the TNT buffer is freed at 0x483BA6. The
   seven bytes `mov edx,[esp+0x40]; shl edx,0xa` are position-independent, the
   flags are dead (the shl sets them), and every branch to the join lands on
   0x483B53 itself. LoadMap's frame there, from its esp: */
#define VA_LOADMAP_JOIN    0x00483B53u
#define LM_VERSION         0x18         /* i32 the TNT's version: 0x1020 or 0x2000   */
#define LM_V1RECS          0x48         /* v1: 8-byte cell records, feature u8 at +2 */
#define LM_V2RECS          0x4C         /* v2: 4-byte cell records, height u8 at +0, */
                                        /* feature u16 at +1 (0xFFFC = void); the   */
                                        /* other version's pointer is 0             */
#define LM_THRESH          0x50         /* a feature below this is placed: 0xFFFB   */
                                        /* for v2, 0xFC for v1                      */
#define OFF_SAVEDGAME      0x38D6B      /* the saved game being loaded, 0 for a new */
                                        /* one: set 0x492655, cleared 0x4915F4      */
#define VA_TEARDOWN        0x00491B60u  /* the level teardown cascade: no stack args, */
                                        /* two exits (ret 0x491C59, tail-jump 0x491C54 */
                                        /* to 0x450DD0 which rets); reclaim wraps it, */
                                        /* the packet observes it when reclaim is off */

/* ---- the header's fields, main + offset ---------------------------------- */
#define OFF_GAMETIME       0x38A47      /* i32 GameTime, the sim tick                */
#define OFF_GAMESPEED_LIVE 0x38A4D      /* i16 the LIVE speed, 3 x it = ticks/s      */
#define OFF_GAMEPAUSED     0x38A51      /* u8, bit 0                                 */
#define OFF_LOADFLAGS      0x38D75      /* u16: bit0 load started (0x49832A), bit1   */
                                        /* loader done (0x497C5F), bit2/bit3 the     */
                                        /* loader<->game handshake (0x4975C7,        */
                                        /* 0x498576); nothing clears bit0/bit1       */
#define OFF_EYE_X          0x1431F      /* i32 pair: the camera's eye                */
#define OFF_EYE_Y          0x14323
#define OFF_SCROLLTO_X     0x14327      /* i32 pair: MapXScrollingTo, the target     */
#define OFF_SCROLLTO_Y     0x1432B
#define OFF_SCREEN_W       0x37E1F      /* i32: the screen, never written by us      */
#define OFF_SCREEN_H       0x37E23
#define OFF_VP_L           0x37E27      /* i32 x4: the viewport rect L, T, R, B the   */
#define OFF_VP_T           0x37E2B      /* engine can NAME (0x497F40 builds it at    */
#define OFF_VP_R           0x37E2F      /* game entry; tagpu_vpwide widens L/T/R/B   */
#define OFF_VP_B           0x37E33      /* on the game thread at zoom < 1)           */
#define OFF_PALETTE        0x143A7      /* 256 x {R,G,B,pad}: the engine's own table, */
                                        /* never gamma-scaled (tagpu_pal.h)          */
#define OFF_MAP_PXW        0x1422B      /* i32: the scroll extent, map px less 32 and */
#define OFF_MAP_PXH        0x1422F      /* less 128 (0x4833C4/0x4833E0); the map's    */
                                        /* own size is main+0x14223/0x14227          */
#define OFF_MAP_W16        0x14233      /* i32: the PLOT grid, 16-px cells           */
#define OFF_MAP_H16        0x14237
#define OFF_VIEWCELLS_W    0x1423B      /* i32: the view in map cells (minimap box)  */
#define OFF_VIEWCELLS_H    0x1423F
#define OFF_UDEFCOUNT      0x1438F      /* u32 UNITINFOCount: the bound on ModelId   */
#define OFF_UNITSLOTS      0x14351      /* u16: 10 x MaxUnits + 1, stored at 0x4854EF */
#define OFF_LOSTYPE        0x14281      /* u16: bit1 true LOS, bit3 screen grid current */
#define OFF_GFXOPT         0x37F06      /* u8: bit2 Shadow, bit3 TShadow             */
#define OFF_UIGATES        0x37F2F      /* u8: bit2 SelBoxes                         */
#define OFF_LOCALPLAYER    0x2A43       /* u8: the id the marker block compares against */
#define OFF_WATCHED        0x2A42       /* u8: the order-marker driver's player      */
#define OFF_SEALEVEL       0x1427F      /* u8: water level, elevation units          */

/* ---- the world tables -----------------------------------------------------
   The publisher reads these on the thread that stores them, once per
   presented frame. The derivations are in the exe note ("The unit array at
   level load", "The 3DO model tree", "The feature grid and its defs", "The
   wreck records"). */
#define OFF_UNIT_BEGIN     0x14357     /* UnitStruct* the array starts at, the  */
#define OFF_UNIT_END       0x1435B     /* pair that is unsafe to read           */
                                       /* unsynchronised: begin is              */
                                       /* stored at 0x485525 and end only at    */
                                       /* 0x4855D6, with a memset between       */
#define UNIT_STRIDE        0x118
#define U_ROT              0x64        /* u16[3] bank, heading (+0x66), pitch   */
#define U_XFIX             0x6A        /* i32 16.16 world x                     */
#define U_ZFIX             0x6E        /* i32 16.16 altitude                    */
#define U_YFIX             0x72        /* i32 16.16 map depth                   */
#define U_CARGO            0x8A        /* UnitStruct* first unit carried/built  */
#define U_CARGONEXT        0x8E        /* UnitStruct* next in that chain        */
#define U_TYPE             0x92        /* UnitDef*                              */
#define U_OBJ3DO           0x9E        /* Object3do*                            */
#define U_MODELID          0xA6        /* u16 index into MODEL_PTRS             */
#define U_INDEX            0xA8        /* u16 UnitInGameIndex: the stable id    */
#define U_SQUAD            0xAC        /* the group tag, tested as a DWORD      */
#define U_OWNER            0xFF        /* u8 player id                          */
#define U_PLAYER           0x96        /* PlayerStruct* owner: the chain 0x4211D0
                                          walks to a debris face's team colour  */
/* the ten players: PlayerStruct[10] inline in the main block (0x49BEDF's
   `lea [edx+eax*2+0x1B63]`, eax = id*0xA5), and the logo colour the unit
   and debris rasterisers pick a team-coloured frame by -- PlayerInfo+0x96,
   reached through the player's +0x27 (0x45861C, 0x421340). DISASSEMBLED. */
#define OFF_PLAYERS        0x1B63
#define PLAYER_STRIDE      0x14B
#define PLAYER_COUNT       10
#define PL_INFO            0x27        /* PlayerInfo*                          */
#define PI_LOGO            0x96        /* u8 the logo colour                   */
#define U_NANO             0x104       /* float, the build fraction REMAINING   */
#define U_HEALTH           0x108       /* s16                                   */
#define U_CLOAKF           0x10E       /* u8, bit2 = actively cloaked           */
#define U_STATE            0x110       /* u32 UnitStateMask                     */
#define ST_ALIVE           0x10000000u
#define ST_EXCLUDED        0x4000u
#define ST_NOCARGO         0x20000u    /* the blit's own skip on a chain member */

#define OFF_UNITDEFS       0x1439B     /* UnitDef[], stride 0x249, length =     */
                                       /* UNITINFOCount (OFF_UDEFCOUNT above)   */
#define UDEF_STRIDE        0x249
#define UD_NAME            0x20        /* char UnitName[], inline               */
#define UD_MAXHP           0x1FA       /* read as the engine's own divisor does */
#define UD_TYPEMASK        0x241       /* u32 FBI booleans                      */
#define UD_COB             0x18E       /* the type's COB, relocated in place by */
                                       /* 0x4B2450; stored at 0x42D8F4 by the   */
                                       /* level's unit-data load, freed and     */
                                       /* zeroed by the teardown's 0x42DB90     */
#define OFF_MODELPTRS      0x14377     /* Model3DONode*[UNITINFOCount]          */

#define O3_NUMPARTS        0x00        /* u16                                   */
#define O3_COMPOSITE       0x10        /* GAFFrame* the unit's sprite           */
#define O3_BTURN           0x18        /* u16[3] the cached body turn           */
#define O3_BASEPRIM        0x1E        /* PrimitiveStruct* the compose starts at */
#define O3_PRIM0           0x22        /* the inline PrimitiveStruct[]          */
#define PRIM_STRIDE        0x36
#define P_NODE             0x00        /* Model3DONode*, the TYPE's template    */
#define P_POS              0x04        /* i32[3] 16.16 COB MOVE delta           */
#define P_TURN             0x10        /* u16[3] COB TURN                       */
#define P_FLAGS            0x28        /* bit0 = visible                        */
#define GF_WIDTH           0x00        /* GAFFrame: u16 W, u16 H, s16 hot x/y   */
#define GF_HEIGHT          0x02
#define GF_HOTX            0x04
#define GF_HOTY            0x06
#define GF_PTRDEPTH        0x14        /* u8* depth plane; NULL = blit path A   */

#define OFF_FEATMAP        0x14287     /* the feature grid, 0x0D per 16-px cell */
#define FT_STRIDE          0x0D
#define FT_HEIGHT          0x04        /* u8 the cell's terrain height          */
#define FT_DEFIDX          0x08        /* u16, < 0xFFFB = a live anchor         */
#define FT_WIDX            0x0A        /* u16 wreck record index                */
#define FT_FLAGS           0x0C        /* u8, bit0 wreckage, bits3.. seen-by    */
#define OFF_FEATDEF        0x1426F     /* FeatureDef[], stride 0x100            */
#define FD_STRIDE          0x100
#define FD_FOOTX           0x94        /* i16 footprint, 16-px cells            */
#define FD_FOOTZ           0x96
#define FD_MASK            0xFE        /* u8, bit0 = GAF (else a 3DO: a record  */
                                       /* of the wreck pool below)              */
#define FD_MASKHI          0xFF        /* u8, bit1 = a spawn over it is refused */
#define OFF_FEATCOUNT      0x14253     /* i32 NumFeatureDefs                    */
#define OFF_WRECKS         0x1420B     /* wreck records, stride 0x30            */
#define WR_STRIDE          0x30
/* THE POOL IS FIXED FOR A LEVEL AND ITS SIZE IS THE BOUND. `0x421F20` allocates
   TAGPU_LIM_WRECKS records of 0x30 bytes once per level, zeroes them and
   threads a free list through them; the limits table writes that count into
   the allocation, the loop and every allocator's "no record" value together
   (tagpu_patches.c), and stock's is 2048 (0x18000 / 0x30). The engine's draw
   path `0x46A6C4` takes the cell's u16 unbounded; we do not, because the
   publisher forms this address for cells the engine never draws. */
#define WR_COUNT           TAGPU_LIM_WRECKS
#define WR_OBJ3DO          0x04
#define WR_XPOS            0x08        /* i32 16.16 triple, as a unit's         */
#define WR_ZPOS            0x0C
#define WR_YPOS            0x10
#define OFF_SWEEP_C        0x1424B     /* i32 sweep cols = viewTilesX + 0x0C    */
#define OFF_SWEEP_R        0x1424F     /* i32 sweep rows = viewTilesY + 0x20    */

#define OFF_GUICOL         0x0DCB      /* GetGuiPaletteColor's byte array: 256  */
                                       /* bytes, rebuilt from `guipal` once at  */
                                       /* startup (0x4AC7D0 writes exactly 0x100 */
                                       /* of them, 0x4AC7FF..0x4AC88F)          */
#define OFF_MOUSE_X        0x2C76      /* the dispatched mouse point            */
#define OFF_MOUSE_Y        0x2C7A
#define OFF_BUILDRECT      0x2C92      /* i32[6]: x, altitude, z of two corners */
#define OFF_CURMODE        0x2CC3      /* u8, 0x0E = build placement            */
#define OFF_BUILDUNITID    0x2CC4      /* u16, the unit-type id (UnitDef index)
                                          of the building being placed.
                                          [VERIFIED 2026-09-12, tacli peek:
                                          after a build-menu click on ARMMEX
                                          the word read 78, the mex's in-game
                                          UnitDef index; the footprint in
                                          OFF_BUILDRECT matched its def.]    */
#define OFF_REGIONFL       0x2CC6      /* u8, bit3 band box, bit6 site OK       */

/* ---- the effects: the four per-frame arrays -------------------------------
   All four are SIM state: the tick moves them, the two engine draw passes
   (0x49BE60 projectiles, 0x420B00 explosions, 0x471F90 particle layers) read
   and write nothing. research/notes/effects.md has the decompiled rules.
   THEIR SIZES ARE tagpu_limits.h's: raised at attach, all four or none, and
   the explosion records and flying-piece slots MOVED into our statics. */
#include "tagpu_limits.h"
#define OFF_NPROJ          0x141F3     /* i32 live projectiles; ten append     */
                                       /* sites refuse past PROJ_COUNT         */
                                       /* (`cmp ...,imm32 / jge`, 0x49B6EE ..) */
#define OFF_PROJ           0x141F7     /* ProjectileStruct*: 0x499A30 allocates */
                                       /* PROJ_COUNT x 0x6B and 0x499A80 frees */
                                       /* AND NULLS it in the teardown         */
#define PROJ_STRIDE        0x6B
#define PROJ_COUNT         TAGPU_LIM_PROJ
#define PJ_WEAPON          0x00        /* WeaponStruct*                        */
#define PJ_X               0x04        /* i32 16.16 world x                    */
#define PJ_ALT             0x08
#define PJ_Y               0x0C
#define PJ_XS              0x10        /* the start (tail) point, same layout  */
#define PJ_ALTS            0x14
#define PJ_YS              0x18
#define PJ_TURN            0x34        /* i16[3] rotation triple               */
#define PJ_SPAWN           0x42        /* i32 tick                             */
#define PJ_DEATH           0x46        /* i32 tick                             */
#define PJ_GROUNDH         0x5E        /* u16 terrain height under it          */
#define PJ_HIDDEN          0x60        /* i16; drawn only when 0               */
#define PJ_SPIN            0x64        /* i16                                  */
#define W_MODEL            0x74        /* Model3DONode*, the weapon's root     */
#define W_LIFE             0xE6        /* u16                                  */
#define W_RT               0x10C       /* i8 RenderType 0..7                   */
#define W_COLOR            0x10D       /* u8 colour NUMBER, through OFF_GUICOL */
#define W_COLOR2           0x10E
#define W_MASK             0x111       /* u32 WeaponTypeMask; bit21 spins the  */
                                       /* thrust flame by PJ_SPIN              */
/* THE EXPLOSION POOL is {i32 count; records}: stock keeps it inline at
   main+0x1491B, the raised build in a static -- tagpu_limits_expl_pool()
   says which. The add site refuses past EXPL_COUNT (0x420A42). */
#define EXPL_NCOUNT        0x0         /* i32 live explosions, pool-relative   */
#define EXPL_RECS          0x4         /* the records, pool-relative           */
#define EXPL_STRIDE        0x54
#define EXPL_COUNT         TAGPU_LIM_EXPL
#define EX_NODE            0x00        /* Model3DONode* debris piece           */
#define EX_ST1             0x04        /* anim state: u16 frame @0, seq* @8    */
#define EX_ST2             0x10        /* the LHT flash's anim state           */
#define EX_X               0x1C
#define EX_ALT             0x20
#define EX_Y               0x24
#define EX_TURN            0x4C        /* i16[3]                               */
/* the flying-debris particle slots: stock's 100 dwords at 0x511DF0..0x511F80,
   or TAGPU_LIM_PSYS of ours -- tagpu_limits_psys_begin()/_end() */
#define PSYS_UNIT          0x00        /* UnitStruct* the piece flew off: the
                                          creator copies it in at 0x4216AA and
                                          0x421713 reads +0x9E off it         */
#define PSYS_PIECE         0x2C        /* the system's piece record            */
#define DB_NODE            0x00
/* THE TURN WORDS ARE +0x10/+0x12/+0x14 AND 0x421550 HANDS THEM OVER REVERSED:
   `0x4215AF..0x4215C5` builds the triple as {w14, w12, w10} before calling
   0x4B6CC0, and the tick 0x4213B0 spins exactly those three words
   (`0x421509..0x421519`). DISASSEMBLED. */
#define DB_TURN_T0         0x14        /* i16: the triple's t0 (Rz)            */
#define DB_TURN_T1         0x12        /* i16: t1 (Ry)                          */
#define DB_TURN_T2         0x10        /* i16: t2 (Rx)                          */
#define DB_X               0x16
#define DB_ALT             0x1A
#define DB_Y               0x1E
/* THE ENGINE'S ROTATION BY A TRIPLE, which poses every effects model:
   void __stdcall (const int32 src[3], int32 dst[3], const int16 turn[3]),
   `ret 0xc`. Pure: it reads the two arguments and the double at 0x509EF8
   (2 pi / 65536) and writes `dst` alone. Three calls of the pair rotator
   0x4B7173, which skips a zero word -- turn[0] on (x, y), then turn[2] on
   (y, z), then turn[1] on (x, z) -- each a' = a cos - b sin, b' = a sin +
   b cos in x87 at the thread's precision, stored back with `fistp`.
   DISASSEMBLED. */
#define ROTATE3_VA         0x004B6CC0u
/* the effect GAF sequences, resolved ONCE PER PROCESS: 0x429870 loads the
   "fx" bank and stores every one of them, and its only caller is 0x49134D
   inside 0x491200, whose only caller is 0x49EA62 in WinMain (0x49E830). So
   these are SESSION assets, not per-level ones [VERIFIED 2026-09-12]. */
#define OFF_SHADOWSEQ      0x1480F     /* the projectile ground-shadow blob     */
#define OFF_SPRSEQ0        0x147BB     /* 5 sprite-weapon sequences (rt 4)      */
#define OFF_FLARESEQ       0x147F3     /* rt 5                                  */
/* the ten particle layers: {u8 flag, void** begin @4, end @8, cap @0xC} x 10,
   allocated per game by 0x471D90 and freed AND NULLED by 0x471DE0 in the
   teardown cascade. Every emitter caps a layer at TAGPU_LIM_SFX + 1 objects
   (tagpu_limits.h): it takes the size, `cmp eax,0x190 / jbe append` (0x472071,
   0x47219F, ... twenty sites, the operand raised by the limits), and past the
   cap destroys the FRONT object, shifts the vector down by one and appends
   anyway — so cap + 1 is the steady state and the cap is not the bound. */
#define OFF_LAYERS         0x38D77
#define LAYER_STRIDE       0x10
#define LAYER_BEGIN        0x04
#define LAYER_END          0x08
#define PO_END             0x04        /* the object: end tick                 */
#define PO_TICK            0x08
#define PO_LAYER           0x0C        /* u8, the layer it was emitted into    */
#define PO_SUB0            0x10        /* its sub-particle vector {begin,end}  */
#define PO_SUB1            0x14
#define VT_SMOKE1          0x004FD638u /* grey smoke, NOT LOS-gated            */
#define VT_SMOKE2          0x004FD618u /* dark smoke                           */
#define VT_FIRE            0x004FD5D8u
#define VT_FLARE           0x004FD588u
#define VT_WAKE            0x004FD5F8u /* wake / bubbles: a 2x2 dot            */
#define VT_NANO            0x004FD5B8u /* nanolathe spray: a 2x2 dot           */
#define VT_BASE            0x004FD5A8u /* destroyed: draws nothing             */
/* TAProgram (the graphics globals' owner, TA_GFX_PP) */
#define PROG_LHT           0x0C8       /* u8[32][256] lighten table            */
#define PROG_CAPS          0x0F0       /* u16: bit5 ALP built, bit6 SHD built, */
                                       /* bit7 LHT built. Established by the    */
                                       /* three in-place setters 0x4BAAD0 /     */
                                       /* 0x4BAB00 / 0x4BAB30, each of which    */
                                       /* tests its own bit and refuses to      */
                                       /* write when it is clear. The bit is a   */
                                       /* SEPARATE fact from the pointer being  */
                                       /* non-NULL: 0x4BA660 allocates the LHT  */
                                       /* and returns without touching this     */
                                       /* word. Test it before reading a table. */

/* ---- the graphics globals ------------------------------------------------ */
#define GFX_FONT           0x204        /* the current font object: SetFont 0x4C1420 */
#define GFX_TEXTFG         0x208        /* its foreground index: SetTextColors 0x4C13A0 */
#define GFX_SHD            0x0C4        /* u8[32][256] PALETTE.SHD: the shade   */
                                        /* table the Gouraud rasteriser 0x459C70 */
                                        /* uses. 0x4BAB00 rewrites it IN PLACE   */
                                        /* behind caps bit 6, from               */
                                        /* 0x42E21B — the same shape as the LHT's */
                                        /* 0x4BAB30. How often that path runs is */
                                        /* NOT established; what is established  */
                                        /* is that the pointer is no identity.   */
#define GFX_GAMMA          0x614        /* float: the factor 0x4BA200 scales every     */
                                        /* palette entry by (SetGamma 0x4BA590)        */

/* ---- the cursor, in the same object -------------------------------------- */
#define GFX_MOUSE_X        0x196        /* i32: the mouse record's x, and +0x19A its   */
#define GFX_MOUSE_Y        0x19A        /* y — what 0x4C67C0 draws the cursor FROM     */
#define GFX_CUR_REC        0x1B2        /* the sprite record: a GAF frame header, out  */
                                        /* of the cursor table. 0 = none               */
#define GFX_CUR_X          0x1B6        /* i32: where the engine last DREW it —        */
#define GFX_CUR_Y          0x1BA        /* written ONLY by the draw paths. 0x4C67C0    */
                                        /* writes it as the mouse record minus the     */
                                        /* hotspot; the three polling paths          */
                                        /* (0x4C2870, 0x4C24B0, 0x4C25E0) write the   */
                                        /* same pair, ALSO pos-hotspot, from their own */
                                        /* poll's answer. In 0x4C25E0 the code at      */
                                        /* 0x4C2638 and 0x4C2645 subtracts the movsx'd */
                                        /* hotspot into edi/ebx, which are what        */
                                        /* 0x4C284C/0x4C2852 store. Measured live as   */
                                        /* well: the pair sits 13..17 px off the       */
                                        /* record, which is exactly the pulsing move   */
                                        /* cursor's hotspot.                           */
#define GFX_CUR_ON         0x1CE        /* u32: THE MOUSE THREAD IS RUNNING. Set to 1  */
                                        /* at 0x4C2AAE, immediately after the          */
                                        /* _beginthread at 0x4C2A9A succeeds, and 0 at */
                                        /* 0x4C2C72 when it is torn down; the thread   */
                                        /* handle lands beside it at +0x1CA. It is not */
                                        /* a hide counter or a display-mode word.      */
                                        /* 0x4C67C0 draws only when it is non-zero,    */
                                        /* i.e. only while that thread exists.         */
#define GFX_CUR_OK         0x1D2        /* u32: and this one                           */

#endif
