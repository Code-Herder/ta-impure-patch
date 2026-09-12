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
#define OFF_VP_L           0x37E27      /* i32 x4: the viewport rect L, T, R, B the   */
#define OFF_VP_T           0x37E2B      /* engine can NAME (0x497F40 builds it at    */
#define OFF_VP_R           0x37E2F      /* game entry; tagpu_vpwide widens L/T/R/B   */
#define OFF_VP_B           0x37E33      /* on the game thread at zoom < 1)           */
#define OFF_PALETTE        0x143A7      /* 256 x {R,G,B,pad}: the engine's own table, */
                                        /* never gamma-scaled (tagpu_pal.h)          */
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

/* ---- the world tables (landing 3) ---------------------------------------
   Every one of these was read on the RENDER thread until landing 3, from nine
   files; the publisher makes them here, on the thread that stores them, once
   per presented frame. The derivations are in the exe note ("The unit array at
   level load", "The 3DO model tree", "The feature grid and its defs", "The
   wreck records"). */
#define OFF_UNIT_BEGIN     0x14357     /* UnitStruct* the array starts at, the  */
#define OFF_UNIT_END       0x1435B     /* pair whose unsynchronised read was    */
                                       /* the audit's open hazard: begin is     */
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
#define FD_MASK            0xFE        /* u8, bit0 = animated GAF wreck         */
#define OFF_FEATCOUNT      0x14253     /* i32 NumFeatureDefs                    */
#define OFF_WRECKS         0x1420B     /* wreck records, stride 0x30            */
#define WR_STRIDE          0x30
/* THE POOL IS FIXED AND ITS SIZE IS THE BOUND. `0x421F29` allocates 0x18000
   bytes for this array once per level and zeroes them (`mov ecx,0x6000; rep
   stos`), then threads a free list through them stepping 0x30 from 0 to
   0x18000 — so 0x18000/0x30 = 2048 records, indices 0..2047, and `0x421F94`
   terminates the list by writing -1 into record 2047's next word at +0x17FD0
   (2047*0x30). The allocator `0x4232A0` returns 2048 itself when the free list
   is empty, so 2048 is also the engine's own "no record" value. The engine's
   draw path `0x46A6C4` takes the cell's u16 unbounded; we do not, because the
   publisher forms this address for cells the engine never draws. */
#define WR_COUNT           2048
#define WR_OBJ3DO          0x04
#define WR_XPOS            0x08        /* i32 16.16 triple, as a unit's         */
#define WR_ZPOS            0x0C
#define WR_YPOS            0x10
#define OFF_SWEEP_C        0x1424B     /* i32 sweep cols = viewTilesX + 0x0C    */
#define OFF_SWEEP_R        0x1424F     /* i32 sweep rows = viewTilesY + 0x20    */

#define OFF_GUICOL         0x0DCB      /* GetGuiPaletteColor's byte array       */
#define OFF_MOUSE_X        0x2C76      /* the dispatched mouse point            */
#define OFF_MOUSE_Y        0x2C7A
#define OFF_BUILDRECT      0x2C92      /* i32[6]: x, altitude, z of two corners */
#define OFF_CURMODE        0x2CC3      /* u8, 0x0E = build placement            */
#define OFF_REGIONFL       0x2CC6      /* u8, bit3 band box, bit6 site OK       */

/* ---- the graphics globals ------------------------------------------------ */
#define GFX_FONT           0x204        /* the current font object: SetFont 0x4C1420 */
#define GFX_TEXTFG         0x208        /* its foreground index: SetTextColors 0x4C13A0 */
#define GFX_SHD            0x0C4        /* u8[32][256] PALETTE.SHD: the shade   */
                                        /* table the Gouraud rasteriser 0x459C70 */
                                        /* uses; built at init, never rebuilt    */
#define GFX_GAMMA          0x614        /* float: the factor 0x4BA200 scales every     */
                                        /* palette entry by (SetGamma 0x4BA590)        */

#endif
