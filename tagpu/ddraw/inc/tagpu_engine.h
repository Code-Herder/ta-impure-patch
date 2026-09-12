#ifndef TAGPU_ENGINE_H
#define TAGPU_ENGINE_H
/* tagpu_engine.h — the engine's addresses, for PUBLISHER files only.

   THE RULE (frame-packet-exchange.md §10). Every engine address and every
   `main+` offset the frame packet's publisher reads lives here, and only a
   publisher — game-thread code inside an engine call, at a site that owns
   the memory it reads — may include this header. `tools/thread-split-check.sh`
   fails the build for any file outside `tagpu/ddraw/thread-split.allow` that
   includes it, names a virtual address, probes with IsBad*Ptr or adds an
   offset to the main pointer. The list only shrinks: each landing that
   converts a render-thread reader to the packet removes its line.

   Started with landing 1 (the frame packet's header). The other modules
   still carry their own `#define`s; they move here as they are converted,
   not before, so a conversion's diff shows exactly what left the render
   thread. Every value here is established in
   research/notes/exe-reverse-engineering.md, by disassembly of the pristine
   3.1 exe or by a live measurement that the note quotes. */

/* ---- the root ------------------------------------------------------------ */
#define TA_MAIN_PP         0x00511DE8u  /* TAdynmemStruct**: `mov eax,ds:0x511de8` */
#define TA_GFX_PP          0x0051FBD0u  /* the graphics globals: 0x4B6220 returns it */

/* ---- the publish point ---------------------------------------------------- */
#define VA_DRAWGAMESCREEN  0x00468CF0u  /* DrawGameScreen(drawUnits, blitScreen)   */
#define VA_DRAW_RET_INPLAY 0x004969D2u  /* the return address of the in-play call: */
                                        /* 0x4969CD `call 0x468cf0` inside the     */
                                        /* frame callback 0x496790, both args 1    */
/* the loader thread: created at 0x4982CA (`push 0x497C70; call 0x4B6B20`,
   the CRT's _beginthread over CreateThread + ResumeThread), its entry the SEH
   wrapper 0x497C70 -> 0x497180, whose last act sets bit 1 of TA_LOADFLAGS */
#define VA_LOADER_ENTRY    0x00497C70u
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
#define OFF_MAP_PXW        0x1422B      /* i32: map size in world px                 */
#define OFF_MAP_PXH        0x1422F
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

/* ---- the graphics globals ------------------------------------------------ */
#define GFX_FONT           0x204        /* the current font object: SetFont 0x4C1420 */
#define GFX_TEXTFG         0x208        /* its foreground index: SetTextColors 0x4C13A0 */

#endif
