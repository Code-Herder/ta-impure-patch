#ifndef TAGPU_PACKET_H
#define TAGPU_PACKET_H
/* tagpu_packet — the frame packet exchange, consumer side.

   Design: research/notes/frame-packet-exchange.html. The game thread publishes
   a COPY of the per-frame engine state it owns, once per presented frame at
   most, from the DrawGameScreen observer's `after` (post-flip, in-play frames
   only); the render thread takes the latest copy once at the top of its frame
   and reads nothing but that copy for the rest of it. The exchange is one
   aligned word and two `xchg`s — no lock, no wait on either side, four slots
   that are always a permutation of {W, cell, READ, PREV} (tagpu_packet.c).

   THE OTHER DIRECTION is the same primitive with the threads
   swapped: the render thread posts a COMMAND record — the zoom level in
   force, the cursor anchor's eye delta, the camera hold, the follow release —
   and the game thread takes the latest one at the top of every in-play draw
   (the observer's `before`, post-tick, pre-draw) and writes the engine's
   words there, on the thread that owns them. The render thread stores into
   no engine memory. The packet echoes what was applied (`cmd_ack_*`), and
   the render thread draws from the packet's eye PLUS the deltas not yet
   acknowledged — prediction reconciled by the next packet, so a wheel notch
   is drawn on the frame it happens and never wobbles.

   WHAT THIS HEADER IS. The two records' layouts and the consumer's calls. No
   engine address appears here: the fields say what they MEAN, and the one
   file that knows where each comes from is the publisher (tagpu_packet_pub.c,
   through inc/tagpu_engine.h). A module on the render thread receives the
   packet pointer from the driver, through TAGPU_FRAME / TAGPU_FXVIEW, and may
   not acquire on its own; the pointer is valid for THIS frame only.

   WHAT THE PACKET CARRIES. The header, the marker text's font as glyph
   bytes, and the view (the addressable rect, the palette and gamma, the
   command acknowledgement); the four world tables — units, pieces, wrecks and
   feature anchors — with the header fields they need (the GUI colours, the
   mouse and the build cursor, the engine's shade table); the four effect
   tables — projectiles, explosions, flying debris and the ten particle
   layers' sub-particles — with the LHT ramp, the ALP/LHT capability bits and
   the whole 256-byte GUI colour LUT they need; the two fog grids; and the UI
   layer's render half. Every table has its `off`/`n` here. The acquire is
   tick-aware, so the two packets the consumer holds always span two distinct
   sim ticks. The effect tables' gather is CACHED PER SIM TICK in the
   publisher: the engine's two effect passes read their arrays and write
   nothing, so two publishes of one tick must produce the same table. */
#include <stdint.h>
#include "tagpu_limits.h"   /* the effect pools' sizes: the tables hold every record */

/* One glyph of the marker font, as a self-contained one-glyph FONT OBJECT the
   engine's own blitter 0x4CCF60 accepts: [rows][0][yoff][code][u16 6][w][bits]
   — the same four header fields the blitter reads, a one-entry offset table,
   and the glyph's width byte and packed bit rows copied verbatim from the
   engine's font on the game thread. `off` is the object's byte offset inside
   the packet's font area; `w` the width, duplicated here so a measure never
   touches the bits. w == 0: the font has no such glyph (or it did not fit). */
typedef struct TAGPU_PK_GLYPH {
    uint16_t off;
    uint8_t  w;
    uint8_t  pad;
} TAGPU_PK_GLYPH;

#define TAGPU_PK_GLYPH_LO   0x20u
#define TAGPU_PK_GLYPH_HI   0x7Eu
#define TAGPU_PK_NGLYPH     (TAGPU_PK_GLYPH_HI - TAGPU_PK_GLYPH_LO + 1)   /* 95 */
#define TAGPU_PK_FONT_MAX   (16u * 1024u)   /* the font area's cap, bytes: a stock
                                               font is 1..2 KB; past this, glyphs
                                               are dropped and `truncated` says so */
#define TAGPU_PK_GLYPH_HDR  7u              /* header 4 + table entry 2 + width 1 */

/* truncated bits */
#define TAGPU_PK_TRUNC_FONT   0x1u
#define TAGPU_PK_TRUNC_STRESS 0x2u          /* the stress lever's dummy table   */


/* ---- THE WORLD TABLES ---------------------------------------------------
   One contiguous block per slot: this header, then the tables, 4-aligned, each
   named by an `off_`/`n_` pair below. Every field says where the publisher
   reads it (game thread, in-play frames only) and the bound it applies. A
   `_key` is an engine address that crosses as an OPAQUE IDENTITY — a cache key
   the render thread compares and never dereferences — with one stated
   exception, PK_PIECE.node, whose paragraph says why.

   NOTHING PER-UNIT IS DEREFERENCED ON THE RENDER THREAD. The unit array and
   every Object3do field the passes read are copied here by
   the thread that owns them, which is what closes the audit's open hazard
   (cross-thread-engine-reads.md §5 row 2: the unsynchronised begin/end pair). */

/* 100 B, one per LIVE unit, in engine slot order. The table's CAPACITY is the
   engine's own slot count (`unit_slots` above), so a unit is never cut: the
   engine allocates slots in per-player blocks and a walk stopped short would
   drop the highest-numbered players entirely (the plan's §5 card). */
typedef struct TAGPU_PK_UNIT {
    uint32_t state;          /* unit+0x110: bit28 alive, bit14 excluded,
                                bit4 selected, bit17 cargo-chain skip,
                                bit29 structure (cached-shadow blit), bit9 sonar */
    int32_t  pos[3];         /* unit+0x6A / +0x6E / +0x72, 16.16: x, altitude, depth */
    uint32_t squad;          /* unit+0xAC, read as a DWORD because 0x469C55 does */
    uint32_t o3_key;         /* unit+0x9E, the Object3do address, KEY: the pose
                                caches key on it and nothing dereferences it   */
    uint32_t piece_off;      /* byte offset of this unit's PK_PIECE run, 0 = none */
    uint32_t def_mask;       /* UnitDef+0x241, the FBI booleans (bit12 canhover,
                                bit19 floater, bit25 noshadow, bit30 digger)    */
    float    nano;           /* unit+0x104, the fraction of the build REMAINING */
    int32_t  max_health;     /* UnitDef+0x1FA, read as the engine's divisor does */
    uint16_t rot[3];         /* unit+0x64: bank, heading (+0x66), pitch         */
    uint16_t bturn[3];       /* Object3do+0x18 / +0x1A / +0x1C, the cached body
                                turn the compose folds into the base piece      */
    uint16_t slot;           /* index in the engine's unit array (stride 0x118) */
    uint16_t id;             /* unit+0xA8, the stable in-game id: the lerp's key
                                and the only thing two packets are matched on   */
    uint16_t model_id;       /* unit+0xA6; the entry is DROPPED unless
                                model_id < udef_count, so an index taken from a
                                recycled slot can never address the model table
                                out of bounds                                   */
    uint16_t type_row;       /* the UnitDef row of unit+0x92, bounded by
                                udef_count; 0xFFFF = the def did not resolve     */
    uint16_t nparts;         /* Object3do+0x00, <= TAGPU_PBMAXPIECE             */
    uint16_t piece_n;        /* nparts when the unit is inside the WIDEST zoom
                                rect plus a margin, else 0 — never the current
                                zoom level, which a held packet outlives        */
    uint16_t base_piece;     /* (Object3do+0x1E - prim0) / 0x36, the piece the
                                body turn folds into; 0xFFFF = none             */
    int16_t  health;         /* unit+0x108                                      */
    int16_t  cargo_first;    /* unit+0x8A / +0x8E resolved to PACKET indices,   */
    int16_t  cargo_next;     /* -1 = none                                        */
    int16_t  comp_w, comp_h; /* the unit composite's GAFFrame rect and hotspot  */
    int16_t  comp_hx, comp_hy;
    uint8_t  owner;          /* unit+0xFF, player id                            */
    uint8_t  cloak;          /* unit+0x10E, bit2 = actively cloaked             */
    uint8_t  flags;          /* TAGPU_PK_U_* below                              */
    uint8_t  ground_h;       /* the feature cell's height byte under the anchor */
    char     name[16];       /* UnitDef+0x20 UnitName, LOWERCASED and NUL-padded:
                                what the roster and scaffold log lines print.
                                16 is the FBI
                                field's own width; a longer name is truncated    */
} TAGPU_PK_UNIT;

#define TAGPU_PK_U_DEPTHPLANE 0x01u  /* the composite has a depth plane (path B) */
#define TAGPU_PK_U_NATIVE     0x02u  /* tagpu_native_owns_unit() said yes, on the
                                        GAME thread, with the def in hand        */
#define TAGPU_PK_U_INRECT     0x04u  /* inside the widest zoom rect: piece_n set  */
#define TAGPU_PK_U_GROUND     0x08u  /* ground_h is a real cell: the unit's anchor
                                        tile was inside the map                   */

/* 24 B, piece_n per unit, in the model's own order (parents first).
   `node` IS DEREFERENCED, and that is deliberate: it is the per-TYPE
   Model3DONode template, which the level teardown cascade frees (0x42DB90)
   and no unit's destructor touches, so its lifetime is tagpu_reclaim's
   teardown fence — the same argument tagpu_posebake.c stands on (the plan's
   row 3: "the template ones stay, they are the fence's"). What the packet
   carries instead is the per-UNIT read: the PrimitiveStruct lives inside the
   Object3do, which FreeObjectState 0x45AAA0 frees while the render thread may
   be mid-frame, and it is that read — not the template one — that the game
   thread makes on our behalf. */
typedef struct TAGPU_PK_PIECE {
    int32_t  pos[3];         /* prim+0x04 P_POS, the COB MOVE delta, 16.16      */
    uint16_t turn[3];        /* prim+0x10 P_TURN, 65536 = 360 degrees           */
    uint8_t  flags;          /* prim+0x28 P_FLAGS: bit0 visible                 */
    uint8_t  pad;
    uint32_t node;           /* prim+0x00 P_NODE, the type's template node      */
} TAGPU_PK_PIECE;

/* 44 B (asserted in tagpu_packet_pub.c), one per live wreck record the anchor
   rect names. Posed exactly as a unit is — the engine draws a husk through a
   scratch fake unit — so it carries the same pose fields. */
typedef struct TAGPU_PK_WRECK {
    int32_t  pos[3];         /* record+0x08 / +0x0C / +0x10, 16.16              */
    uint32_t o3_key;         /* record+0x04, KEY                                */
    uint32_t piece_off;
    uint16_t bturn[3];
    uint16_t rec;            /* the record index (stride 0x30)                  */
    uint16_t def;            /* the FeatureDef row of the anchor that named it  */
    uint16_t nparts, piece_n;
    uint16_t base_piece;
    uint16_t col, row;       /* THE ANCHOR TILE, not the husk's own position: the
                                unit pass decides which wrecks are in its rect by
                                the tile, and only then culls by the projected
                                anchor                                           */
    uint16_t pad;
} TAGPU_PK_WRECK;

/* 16 B, one per feature-grid cell in the anchor rect that anchors a feature,
   row-major over that rect. PER FRAME, not per level: the def index, the flags
   and the wreck index are sim state the game thread rewrites every tick. The
   six height bytes are the cell's own and the five neighbours the two height
   rules need — the 2x2 corner average of the engine's projection (h, hr, hd,
   hrd) and the left/right/up/down gradient the Classic++ ground light uses
   (hl, h, hr and hu, h, hd) — so a consumer needs no second grid read. */
typedef struct TAGPU_PK_ANCHOR {
    uint16_t col, row;       /* the cell, in 16-px tiles                        */
    uint16_t def;            /* cell+0x08 FT_DEFIDX, live when < 0xFFFB and
                                inside the FeatureDef count; bounded here       */
    uint16_t wreck;          /* cell+0x0A FT_WIDX, valid when flags bit0        */
    uint8_t  flags;          /* cell+0x0C FT_FLAGS: bit0 wreckage present,
                                bits 3.. the seen-by-player nibble              */
    uint8_t  h;              /* cell+0x04, this cell's height byte              */
    uint8_t  hr, hd, hrd;    /* (col+1,row), (col,row+1), (col+1,row+1)         */
    uint8_t  hl, hu;         /* (col-1,row), (col,row-1)                        */
    uint8_t  pad;
} TAGPU_PK_ANCHOR;

/* 16 B, one per QUEUED build the order-marker driver would show a site rect
   for — the build-ghost pass draws these as translucent models. `type` is the
   UnitDef index the engine hands MODEL_PTRS, the same index space as
   PK_UNIT.model_id, so the same udef_count bound applies. The publisher copies
   it out of tagpu_order.c's game-thread snapshot under the same lever gate the
   squares are drawn with, so this table and the green squares it mirrors can
   differ only by one presented frame — the copy's age. */
typedef struct TAGPU_PK_BUILD {
    uint16_t type;            /* node+0x36, the build target's unit-type id     */
    uint16_t started;         /* node+0x16 (N_TARGET) resolved to a live unit:
                                 the nanoframe EXISTS and the builder is working
                                 on it, so the site is no longer empty ground.
                                 The square still draws over the frame -- that is
                                 the engine's own behaviour and the marker pass
                                 reproduces it -- but the GHOST must not, or the
                                 player sees a second, solid copy of the building
                                 standing inside the one being built. An exact
                                 link, not a position match: the engine hangs the
                                 created unit on the order node itself, so there
                                 is no tolerance to tune and nothing to race. */
    int32_t  pos[3];          /* node+0x22.., 16.16 x, altitude, z              */
} TAGPU_PK_BUILD;

/* ---- THE EFFECTS AND THE PARTICLE LAYERS --------------------------------
   The engine's four per-frame effect arrays, copied by the thread that owns
   them. All four are SIM STATE — the tick moves a projectile, advances an
   explosion's anim frame and walks every particle's update leaf, and the two
   engine draw passes (0x49BE60 projectiles, 0x420B00 explosions) read and
   write nothing — so the gather is taken ONCE PER TICK and every packet of
   that tick carries the same table, exactly as the anchor scan is.

   EVERY ENGINE POINTER IS RESOLVED HERE, ON THE GAME THREAD. A sprite's GAF
   frame is looked up through its sequence and its frame number at copy time
   (the sequence is the one the particle names, live while the thread that
   wrote it is the thread reading it); a weapon's colour number goes through
   the GUI colour LUT here; the model roots are the per-type templates the
   native pass already walks under `tagpu_reclaim`'s fence, and no consumer of
   these tables dereferences one — `tagpu_fx.c` hands the address straight to
   `emit_fx_model` in the (fenced) native pass and reads no byte of it. */

/* 56 B, one per live projectile, in the engine's own array order. The array
   is exactly TAGPU_LIM_PROJ slots (0x499A30 allocates TAGPU_LIM_PROJ x 0x6B)
   and all ten append sites refuse past it (0x49B6EE, 0x49B809, ... `cmp
   ...,imm32 / jge`; tagpu_limits.h raises the allocation and the ten caps
   together), so the walk's bound is the allocation itself rather than a
   sanity cap. */
typedef struct TAGPU_PK_PROJ {
    int32_t  pos[3];         /* proj+0x04/+0x08/+0x0C, 16.16: x, altitude, y */
    int32_t  start[3];       /* proj+0x10/+0x14/+0x18, the tail              */
    uint32_t node;           /* the weapon's Model3DONode root (rendertypes
                                1, 3 and 6), 0 = none. A TEMPLATE ADDRESS the
                                native pass resolves under the fence          */
    uint32_t child;          /* node+0x30, the thrust flame, only while the
                                projectile is alive (rendertype 1), else 0    */
    uint32_t frame;          /* the resolved sprite (rt 4) or flare (rt 5)
                                GAF frame, already indexed by this tick, 0 =
                                none: an address in the GAF banks, opaque to
                                every consumer but tagpu_gaf.c                */
    int32_t  shadow_y;       /* the ground-shadow blob's screen y in WORLD
                                terms: hi(y) - groundh/2 (the blob is drawn
                                at the terrain point under the projectile)    */
    int16_t  turn[3];        /* the rotation triple, ALREADY adjusted for the
                                rendertype (rt 1 subtracts 0x8000 from two)   */
    int16_t  cturn0;         /* the child's first turn word (rt 1)            */
    uint8_t  rt;             /* WeaponStruct+0x10C RenderType, 0..7           */
    uint8_t  col, col2;      /* PALETTE INDICES: the weapon's colour numbers
                                are taken through main+0xDCB here             */
    uint8_t  owner;          /* the attacker's player id, 0 when it is gone    */
    uint8_t  flags;          /* TAGPU_PK_FX_* below                            */
    uint8_t  pad[2];
} TAGPU_PK_PROJ;

#define TAGPU_PK_FX_SHADOW  0x01u   /* draw the ground-shadow blob            */
#define TAGPU_PK_FX_COL2    0x02u   /* the weapon's SECOND colour number is
                                       non-zero, so the laser is two lines.
                                       The test is on the NUMBER, before the
                                       LUT: `col2` below is already an index,
                                       and index 0 is a real colour           */

/* 32 B, one per live explosion. The records are at tagpu_limits_expl_pool()+4
   (stock: inline at main+0x1491F), stride 0x54, and the engine's own add site
   refuses past TAGPU_LIM_EXPL (0x420A42). */
typedef struct TAGPU_PK_EXPL {
    int32_t  pos[3];         /* expl+0x1C/+0x20/+0x24, 16.16                  */
    uint32_t node;           /* the debris node (+0x00), a template, or 0     */
    uint32_t frame;          /* anim state 1's current frame, the opaque
                                sprite, resolved here; 0 = none               */
    uint32_t flash;          /* anim state 2's, the LHT flash; 0 = none       */
    int16_t  turn[3];        /* expl+0x4C                                     */
    uint16_t pad;
} TAGPU_PK_EXPL;

/* 24 B, one per occupied debris particle slot. The slots are the
   TAGPU_LIM_PSYS dwords tagpu_limits_psys_begin() names (stock: 100 at
   0x511DF0..0x511F80); each names a system whose +0x2C is the piece. */
typedef struct TAGPU_PK_DEBRIS {
    int32_t  pos[3];         /* piece+0x16/+0x1A/+0x1E, 16.16                 */
    uint32_t node;           /* piece+0x00, a template, or 0                  */
    int16_t  turn[3];        /* piece+0x12                                    */
    uint16_t pad;
} TAGPU_PK_DEBRIS;

/* 16 B, one per drawable SUB-PARTICLE, in layer order (the layer IS the draw
   depth: the engine calls 0x471F90(ctx, n) for n = 0..9 at ten fixed points
   of its frame). The projection is done here because it is all the consumer
   needs: `x` is hi(world x) and `zp` is hi(world y) - hi(altitude)/2, the
   space both the screen transform and the fog grid are in. */
typedef struct TAGPU_PK_PART {
    int32_t  x, zp;
    uint32_t frame;          /* the resolved GAF frame; 0 = a 2x2 dot in
                                `col`, which is what the wake and nanolathe
                                classes draw (DrawBar 0x4BF6F0)               */
    uint8_t  layer;          /* 0..9                                          */
    uint8_t  kind;           /* TAGPU_PK_PK_* below: which lever mutes it and
                                whether the engine's LOS gate applies         */
    uint8_t  col;            /* the dot's palette index (frame == 0)          */
    uint8_t  pad;
} TAGPU_PK_PART;

/* the particle classes, by vtable, as tagpu_sfx.c's own enum had them */
#define TAGPU_PK_PK_SMOKE1  0u   /* grey smoke, NOT LOS-gated by the engine   */
#define TAGPU_PK_PK_SMOKE2  1u   /* dark smoke                                */
#define TAGPU_PK_PK_FIRE    2u
#define TAGPU_PK_PK_FLARE   3u
#define TAGPU_PK_PK_WAKE    4u   /* wake / bubbles: a dot                     */
#define TAGPU_PK_PK_NANO    5u   /* nanolathe spray: a dot                    */
#define TAGPU_PK_NPARTKIND  6u

#define TAGPU_PK_FXWANT_FX   0x1u    /* projectiles, explosions, debris        */
#define TAGPU_PK_FXWANT_SFX  0x2u    /* the ten particle layers                */

#define TAGPU_PK_NLAYER     10u

/* the engine's own array sizes, so a table holds every record the engine can */
#define TAGPU_PK_MAX_PROJ    ((unsigned)TAGPU_LIM_PROJ)
#define TAGPU_PK_MAX_EXPL    ((unsigned)TAGPU_LIM_EXPL)
#define TAGPU_PK_MAX_DEBRIS  ((unsigned)TAGPU_LIM_PSYS)
/* The particle table's ceiling. The engine allows 10 layers x 400 objects and
   every object carries a sub-particle vector it grows as it burns, so no
   engine count bounds this one: it is OUR cap, with a truncation bit and a
   counter, and the heartbeat's `partmax` says how close a 200v200 fight came.
   16 384 entries is 256 KB per slot. */
#define TAGPU_PK_MAX_PART    16384u

/* The engine's LHT "lighten" table, TAProgram+0xC8: 32 rows of 256 bytes, the
   explosion flash's colour ramp. Copied whole, like the shade table. */
#define TAGPU_PK_LHT_ROWS    32u
#define TAGPU_PK_LHT_BYTES   (TAGPU_PK_LHT_ROWS * 256u)

/* ---- THE FOG GRIDS ------------------------------------------------------
   Two lattices of 32-px cells, each cell two bytes of 4-bit corner masks — low
   byte the unexplored corners, high byte the out-of-LOS ones, laid out exactly
   as an RG8 texture so the bytes upload with no conversion.

   THE ENGINE'S OWN, `*(main+0x1421F)`, spans the 1x viewport and about two
   cells more. It is built once per map by LoadMap and only REWRITTEN by the
   builder 0x4843C0, which the terrain owner calls from the engine's own fog
   site — so a render-thread read of the buffer, or of the descriptor beside
   it, races the game thread rewriting them (`tagpu_fog_at`'s guard answers a
   hard fault off a base of -9 whose root cause was never found). Both cross
   as ONE record whose
   bounds the acquire checks: `fog_len` must be exactly `fog_cols * fog_rows *
   2` and must lie inside the packet, so the largest index a consumer can form
   is inside the bytes it was given, by construction rather than by a probe.

   THE WIDE ONE is tagpu_fogwide's replication of the same builder over a
   window the whole zoom range fits in — the same lattice, the same rule, more
   cells — used by a frame drawn at zoom < 1, where the engine's stops partway
   across the screen. The grid is built on the game thread inside the draw and
   copied into the packet after it, so there is one buffer, no lock, and
   nothing to retire.

   THE ORIGIN IS THE PUBLISHER'S. `fog_org` is the world point of cell (0,0) —
   `32*col0 + 16` — derived from the eye the grid was actually built at. The
   render thread's PREDICTED eye — the packet's eye plus a cursor-anchor step
   the game thread has not applied yet — would put the lattice off its own
   bytes whenever something is unacknowledged. (The pass also takes the wide
   grid whenever anything is unacknowledged.) */
/* ---- THE UI LAYER'S RENDER HALF -----------------------------------------
   The four engine reads tagpu_gui_surf.c's render half needs, every
   present: the cursor's sprite record through the graphics globals, the
   minimap's box, its three 8bpp surfaces and the view box drawn over them. The
   UI layer's op QUEUE is untouched and stays a queue — it carries an op stream
   into retained twins and a latest-wins snapshot cannot do that (the plan's
   §9). What crosses here is the per-frame STATE the render half reads beside it.

   THE CURSOR'S SPRITE IS A KEY, not a copy. `cur_rec` is the record at
   `graphics+0x1B2`, which IS a GAF frame header — size, hotspot, colour key and
   a pixel pointer — and it comes out of the cursor TABLE, a session asset
   loaded once and never rewritten (tagpu_gaf.c's class). Only tagpu_gaf.c
   dereferences it, which is where that argument lives.

   THE MINIMAP SURFACES ARE A COPY, and they are gated. The engine repaints the
   three at every draw; the publisher interleaves them and the packet carries
   the result — but only while the consumer asks for it (tagpu_gui_want_minimap),
   because at k = 1 the sharp minimap is deliberately the engine's own and the
   whole copy would be paid for nothing.

   THE PICTURE ARRIVES IN THE LEVEL'S FIRST IN-PLAY PACKET and in no other, so
   the consumer keeps its own copy keyed on the level generation. */
#define TAGPU_PK_MM_DIMCAP  512    /* A SANITY CEILING OF OURS on a minimap
                                      surface's dimension, and on the picture's.
                                      It is NOT an engine bound — the engine
                                      bounds only the BOX it fits the picture
                                      into (0x7E), and the surfaces' own
                                      dimensions are not bounded anywhere we
                                      have found. What actually bounds a consumer
                                      is `len == w * h * 3` (and `w * h` for the
                                      picture), checked once at acquire; this
                                      only keeps the product inside 32 bits and
                                      the scratch inside its array. The picture
                                      is a GAF frame, so TAGPU_GAF_DECMAX (640)
                                      is its own decoder's ceiling as well      */

#define TAGPU_PK_FOG_DIMCAP 4096   /* a sanity ceiling on a dimension; the real
                                      bound is `len == cols*rows*2` inside the
                                      record, checked once at acquire          */
#define TAGPU_PK_FOGSHADE_BYTES 256u  /* the grey band's palette remap,
                                         *(TAProgram+0xCC): 0x4BFE10 rewrites
                                         every pixel p as shade[p]             */

/* truncation bits, one per table (TAGPU_PK_TRUNC_FONT/STRESS are above) */
#define TAGPU_PK_TRUNC_UNITS   0x4u
#define TAGPU_PK_TRUNC_PIECES  0x8u
#define TAGPU_PK_TRUNC_WRECKS  0x10u
#define TAGPU_PK_TRUNC_ANCHORS 0x20u
#define TAGPU_PK_TRUNC_SHD     0x40u
#define TAGPU_PK_TRUNC_PROJ    0x80u
#define TAGPU_PK_TRUNC_EXPL    0x100u
#define TAGPU_PK_TRUNC_DEBRIS  0x200u
#define TAGPU_PK_TRUNC_PART    0x400u
#define TAGPU_PK_TRUNC_LHT     0x800u
#define TAGPU_PK_TRUNC_FOG     0x1000u
#define TAGPU_PK_TRUNC_FOGW    0x2000u
#define TAGPU_PK_TRUNC_FOGSH   0x4000u
#define TAGPU_PK_TRUNC_MM      0x8000u
#define TAGPU_PK_TRUNC_MMPIC   0x10000u
#define TAGPU_PK_TRUNC_BUILDS  0x20000u

#define TAGPU_PK_SHD_ROWS   32u      /* the engine's PALETTE.SHD shade table:  */
#define TAGPU_PK_SHD_BYTES  (TAGPU_PK_SHD_ROWS * 256u)   /* 32 x 256 bytes     */

/* The piece-count bound, the same number tagpu_model3do.h's TAGPU_PBMAXPIECE
   states as a fact about a model (the largest stock model has 36 pieces).
   Spelled again here so the packet's own bounds check needs no model header;
   tagpu_packet_pub.c compiles a static assertion that the two agree. */
#define TAGPU_PK_MAXPIECE   256u

/* THE DESIGN POINT: ten players of 1500 units each. The engine's unit array
   has 10 x MaxUnits + 1 slots (the u16 at main+0x14351), and tagpu_limits.h
   raises MaxUnits' ceiling to 1500 (stock clamps it to 500 at 0x491658), so
   15 001. Every cap in the exchange and in the passes it feeds that scales
   with the unit count is asserted against this where it is declared, so a cap
   below it fails the build rather than a large game; tagpu_packet_pub.c
   asserts that the installed limit fits it. A network game takes the host's
   limit unclamped (0x449D9B): past the design point the tables below truncate
   and say so, they never overrun. */
#define TAGPU_PK_DESIGN_SLOTS (10u * 1500u + 1u)

/* The units table's own ceiling: the design point rounded up to a power of
   two. A slot count past it truncates the table (and says so, through
   TAGPU_PK_TRUNC_UNITS) rather than walking off the publisher's scratch. */
#define TAGPU_PK_MAX_UNITS    16384u
#define TAGPU_PK_MAX_WRECKS   4096u
#define TAGPU_PK_MAX_ANCHORS  65536u
#define TAGPU_PK_MAX_BUILDS   6144u    /* the order snapshot's own arena cap:
                                          one record per queued marker, and a
                                          build is a subset of those             */

/* THE PRIMITIVE'S PREFIX AND SUFFIX. Every record the exchange carries — the
   frame packet below and the command record after it — starts with these
   three dwords and ends with `crc, tail_seq`; tagpu_packet.c stores the head
   before the fill and the tail after it, and validates every record against
   the slot's committed size, whatever the record's own layout is. */
typedef struct TAGPU_PACKET {
    uint32_t head_seq;          /* the publish counter, stored BEFORE the fill;
                                   0 = this slot never held a packet            */
    uint32_t cap_bytes;         /* this slot's committed capacity; a canary sits
                                   at exactly this offset                       */
    uint32_t used_bytes;        /* header + areas, 4-aligned, <= cap_bytes      */
    uint32_t in_game;           /* 1 = an in-play frame; 0 = published by the
                                   level teardown: draw no world                */
    uint32_t tick;              /* the sim tick (GameTime)                      */
    uint32_t tick_start_lo;     /* QueryPerformanceCounter when the publisher   */
    uint32_t tick_start_hi;     /* first saw this tick (the lerp's clock, L3)   */
    uint32_t level_gen;         /* tagpu_reclaim's level generation: caches key
                                   on it, and `prev` is withheld across it      */
    uint32_t cmd_ack_seq;       /* the last command record applied before this
                                   draw (its cmd_seq); 0 = none yet             */
    int32_t  cmd_ack_dx;        /* the cursor anchor's CUMULATIVE eye delta the */
    int32_t  cmd_ack_dy;        /* game thread had applied by this draw: the
                                   render thread's own cumulative minus these
                                   is what it draws ahead of `eye` (prediction) */
    uint32_t gui_flips;         /* the UI layer's count of engine flips, so the
                                   twin-to-packet skew can be counted           */
    uint32_t draw_seq;          /* the observer's in-play draw count            */
    int32_t  eye[2];            /* the camera's eye, world px, as this frame
                                   was drawn with it                            */
    int32_t  scroll_to[2];      /* where the camera is heading                  */
    int32_t  vp[4];             /* the TRUE 1x viewport: L, T, W, H (never the
                                   widened rect)                                */
    int32_t  vp_addr[4];        /* the rect the engine can NAME right now, as
                                   its field holds it: L, T, R, B inclusive —
                                   the true rect, or the widened one at zoom<1 */
    int32_t  screen[2];         /* screen W, H                                  */
    int32_t  map_pxw, map_pxh;  /* the map in world px                          */
    int32_t  map_w16, map_h16;  /* ...and in 16-px cells                        */
    int32_t  view_cells[2];     /* the view in map cells (the minimap box)      */
    uint32_t udef_count;        /* UNITINFOCount: the bound on every model id   */
    uint32_t unit_slots;        /* the unit array's slot count, 10 x MaxUnits+1 */
    uint16_t los_type;          /* bit1 true LOS, bit3 the screen fog grid is current */
    uint16_t gfx_opt;           /* bit2 Shadow, bit3 TShadow                    */
    uint16_t ui_gates;          /* bit2 SelBoxes                                */
    uint16_t load_flags;        /* the loader's flag word (bit0 started, bit1
                                   done, bit2/3 the handshake): for the level log */
    uint8_t  local_player;      /* the id the marker block compares owners to   */
    uint8_t  watched;           /* the order-marker driver's player             */
    uint8_t  sea_level;
    uint8_t  paused;            /* bit 0 of the engine's pause byte             */
    int32_t  game_speed;        /* the LIVE speed: ticks/s = 3 x this           */
    int32_t  text_fg;           /* the marker block's text colour, a palette
                                   index; -1 = never seen                       */
    float    zoom_applied;      /* the level the game thread applied before this
                                   draw (1.0 = none): a diagnostic, not a source */
    uint32_t cmd_epoch;         /* the command apply's epoch: bumped at every
                                   level end, with the applied cumulative delta
                                   reset to zero. A record from an older epoch
                                   carries no delta; the render thread resets
                                   its own sum when it sees a new one, so a
                                   notch in a level's last frames can never be
                                   applied to the next level's camera         */
    uint32_t pal_ok;            /* 1 = pal[] and gamma below were copied        */
    float    gamma;             /* the engine's gamma factor, bounded 0.05..8.0 */
    uint8_t  pal[1024];         /* the engine's own palette table, 256 x RGBA,
                                   NOT what the screen shows (tagpu_pal.h)     */
    uint32_t font_gen;          /* bumped when the glyph copy below changed;
                                   0 = no font yet                              */
    uint8_t  font_rows;         /* the font's header, as the blitter reads it   */
    int8_t   font_yoff;
    uint8_t  font_first;
    uint8_t  font_pad;
    TAGPU_PK_GLYPH font_glyph[TAGPU_PK_NGLYPH];   /* index = code - 0x20     */
    uint32_t font_off;          /* the font area: [font_off, font_off+font_len) */
    uint32_t font_len;          /* inside the slot; 0 = no font in this packet  */
    uint32_t stress_off;        /* the stress lever's dummy table (grows the   */
    uint32_t stress_len;        /* slot on purpose); 0 otherwise                */

    /* ---- the world tables and what reads them ---- */
    uint32_t n_units,   off_units;    /* PK_UNIT,   live units in slot order    */
    uint32_t n_pieces,  off_pieces;   /* PK_PIECE,  the arena the runs index    */
    uint32_t n_wrecks,  off_wrecks;   /* PK_WRECK                               */
    uint32_t n_anchors, off_anchors;  /* PK_ANCHOR, row-major over the rect     */
    int32_t  anch_c0, anch_r0;        /* the anchor rect, in 16-px cells, already */
    int32_t  anch_cols, anch_rows;    /* clamped to the map: the WIDEST zoom rect
                                         plus a margin, so a consumer at any zoom
                                         asks for a sub-rect of it               */
    uint32_t unit_dup;                /* live units sharing a stable id in THIS
                                         packet: the collision oracle, and a gate */
    /* THE PER-MAP ARRAY BASES ARE NOT HERE, AND THAT IS DELIBERATE. FeatureDef
       `main+0x1426F`, the wreck records `+0x1420B` and `MODEL_PTRS` `+0x14377`
       are POINTERS the level teardown frees AND THEN NULLS — `0x4221F8` then
       `0x422214`, `0x42227D` then `0x42228B`, `0x42DCCB` then `0x42DCD8`, all
       inside the cascade `0x491B60`. That null is the only invalidation the
       fenced passes have, and a copy in a packet that outlives the
       frame it was made in reads past it: with `tagpu_reclaim` unarmed —
       which is a supported configuration, the publisher has a second level-end
       provider for exactly that case — the render thread would walk a freed
       FeatureDef or a freed model template for the whole cascade. So those
       three stay a LIVE read in the `fenced` files that index them, where the
       engine's own null still refuses them. The packet carries values, and a
       pointer whose lifetime it cannot state is not one. */
    /* NumFeatureDefs at publish time. It is the bound on a def row only
       TOGETHER with the live count: `main+0x1426F`'s array is grown one record
       at a time and `main+0x14253` is incremented LAST (0x422543 reallocs,
       0x422558 stores the base, 0x422DAC counts), so the live count never
       over-describes the live base — where this one belongs to the packet's
       level and would over-describe the next map's smaller array. Consumers
       take the smaller of the two. */
    int32_t  feat_defcount;
    int32_t  sweep_cols, sweep_rows;  /* the engine's own feature sweep rect size */
    int32_t  mouse[2];                /* the dispatched mouse point, screen px    */
    int32_t  build_rect[6];           /* the build cursor's two corners as
                                         x, altitude, z (0x2C92..0x2CA6)          */
    /* main+0xDCB, the whole 256-byte LUT the engine rebuilds from `guipal` at
       startup (0x4AC7D0 writes exactly 0x100 bytes). All of it: a weapon's
       colour NUMBER indexes this table and nothing bounds it below 256. */
    uint8_t  gui_col[256];
    uint8_t  cursor_mode;             /* 0x2CC3: 0x0E = build placement           */
    uint8_t  region_flags;            /* 0x2CC6: bit3 band box, bit6 site OK      */
    uint16_t build_unit_id;           /* 0x2CC4: what the build cursor is placing,
                                         a UnitDef index; 0 = none. VERIFIED
                                         2026-09-12 (tagpu_engine.h): 78 =
                                         ARMMEX after a build-menu click       */
    uint8_t  game_opt;                /* 0x37F06 low byte: bit0 damagebars,
                                         bit2 Shadow, bit3 TShadow, bit4 FShadow  */
    uint8_t  pad3;
    uint32_t shd_off, shd_len;        /* the engine's 32x256 shade table
                                         (PALETTE.SHD at graphics+0xC4), copied
                                         whole: the Gouraud LUT the unit pass
                                         builds its shading from                  */

    /* ---- the build-orders table (the ghost pass) ---- */
    uint32_t n_builds, off_builds;    /* PK_BUILD: the queued builds whose site
                                         rect the order pass is showing          */

    /* ---- the effects and the particle layers ---- */
    uint32_t n_proj,   off_proj;      /* PK_PROJ,   the live projectiles        */
    uint32_t n_expl,   off_expl;      /* PK_EXPL,   the live explosions         */
    uint32_t n_debris, off_debris;    /* PK_DEBRIS, the occupied debris slots   */
    uint32_t n_part,   off_part;      /* PK_PART,   every drawable sub-particle,
                                         IN LAYER ORDER                          */
    uint32_t part_n[TAGPU_PK_NLAYER]; /* how many of them are in each layer, so
                                         a consumer walks 0..6, then the two
                                         effect passes, then 7..9 as the engine
                                         does — the counts sum to n_part        */
    uint32_t part_obj[TAGPU_PK_NLAYER];   /* the OBJECTS each layer held, a
                                         diagnostic: the sub-particle table can
                                         be truncated, this count never is       */
    uint32_t fx_caps;                 /* TAProgram+0xF0: bit5 the ALP alpha
                                         table is built, bit7 the LHT one        */
    uint32_t fx_want;                 /* what the gather actually ran with: bit0
                                         the three effect tables, bit1 the
                                         particle one. A consumer must not claim
                                         the engine's draw until this says the
                                         publisher was filling for it — the
                                         request crosses on the render thread's
                                         clock and the fill on the game
                                         thread's, so the first armed frames see
                                         empty tables                            */
    uint32_t fx_gen;                  /* bumped whenever any of the four tables
                                         was re-gathered (once per sim tick):
                                         the consumer's own "is this the same
                                         gather" test, and the heartbeat's       */
    uint32_t shadow_frame;            /* the projectile ground-shadow blob, frame
                                         0 of main+0x1480F's sequence, resolved
                                         here; 0 = the sequence is not there     */
    uint32_t lht_off, lht_len;        /* the engine's 32x256 LHT lighten table
                                         (TAProgram+0xC8), the explosion flash's
                                         colour ramp                             */

    /* ---- the two fog grids ---- */
    int32_t  fog_cols, fog_rows;      /* the ENGINE's screen grid; 0 = none this
                                         frame (the descriptor did not hold up) */
    int32_t  fog_org[2];              /* world x, projected z of its cell (0,0) */
    uint32_t fog_off, fog_len;        /* fog_cols * fog_rows * 2 bytes          */
    int32_t  fogw_cols, fogw_rows;    /* the WIDE grid; 0 = the module is off,
                                         not building, or has published nothing */
    int32_t  fogw_org[2];
    uint32_t fogw_off, fogw_len;
    uint32_t fogsh_off, fogsh_len;    /* the grey band's 256-byte palette remap */

    /* ---- the UI layer's render half ---- */
    uint32_t cur_rec;                 /* graphics+0x1B2, the sprite record — a
                                         GAF frame header in the SESSION cursor
                                         table. A KEY: only tagpu_gaf.c reads it.
                                         0 = no cursor this frame, in play and in
                                         the shell alike (tagpu_packet_pub.c) */
    int32_t  mm_box[4];               /* main+0x142E7/E9/EB/ED, the box the
                                         engine fitted the minimap into, ITS px */
    int32_t  mm_view[4];              /* main+0x142CB, the view box, screen px,
                                         edges inclusive                         */
    int32_t  mm_w, mm_h;              /* the three surfaces' size               */
    uint32_t mm_off, mm_len;          /* mm_w * mm_h * 3 bytes: the fog base,
                                         the unshaded base and the composite,
                                         interleaved as an RGB texture           */
    uint8_t  mm_live;                 /* the engine's minimap exists this frame
                                         (its composite pointer is non-NULL) —
                                         the render half's "in a game" gate      */
    uint8_t  mm_viewcol;              /* main+0xDD9, the view box's palette index */
    uint8_t  mm_pad[2];
    int32_t  mmpic_w, mmpic_h;        /* the decoded minimap picture, in the
                                         LEVEL'S FIRST in-play packet only       */
    uint32_t mmpic_off, mmpic_len;    /* mmpic_w * mmpic_h palette indices       */
    uint32_t truncated;         /* TAGPU_PK_TRUNC_* bits: what did not fit      */
    uint32_t crc;               /* CRC-32 of the slot with these two fields as
                                   zero, under `tagpu_packet.check`; else 0     */
    uint32_t tail_seq;          /* stored LAST, after the fill: equals head_seq
                                   or the consumer's frame_end counts a violation */
} TAGPU_PACKET;

/* THE COMMAND RECORD — render thread to game thread. One record
   per render frame, latest wins: every quantity in it is either a LEVEL (the
   game thread re-applies it before every in-play draw for as long as it
   stands) or a CUMULATIVE sum (the game thread applies the difference from
   what it last applied, so a record overwritten before it was taken loses
   nothing and a delta is consumed exactly once). Nothing in it is an
   absolute camera position except the hold, which is what a hold means. */
typedef struct TAGPU_CMD {
    uint32_t head_seq;          /* the primitive's prefix, as above            */
    uint32_t cap_bytes;
    uint32_t used_bytes;
    uint32_t cmd_seq;           /* the render thread's post counter, monotone  */
    uint32_t epoch;             /* the cmd_epoch of the last packet the render
                                   thread saw: the game thread applies the delta
                                   below only when it matches its own epoch    */
    int32_t  cum_dx, cum_dy;    /* the cursor anchor's cumulative eye delta,
                                   world px, since the epoch began — the game
                                   thread applies `cum - applied` and clamps   */
    float    zoom;              /* the zoom level in force this frame          */
    uint32_t live;              /* a zoomed world is on screen: the level above
                                   is what the picture is drawn at; 0 = the
                                   game thread applies 1.0 (the engine's own
                                   range, rect and scroll rate)                */
    uint32_t eyeoff;            /* tagpu_zoomedge.off: the camera range is the
                                   engine's own whatever the level             */
    uint32_t hold_on;           /* tagpu_eye.txt holds the camera at hold_x/y  */
    int32_t  hold_x, hold_y;
    uint32_t drop_follow;       /* the delta above came from a gesture that
                                   wants the camera: release the follow when
                                   applying it (the engine's own rule for a
                                   manual camera move)                         */
    uint32_t crc;               /* the primitive's suffix, as above            */
    uint32_t tail_seq;
} TAGPU_CMD;

/* ---- the tables, by name ------------------------------------------------
   Every offset in the header was checked against `used_bytes` at acquire
   (tagpu_packet.c's frame_valid), so these are plain adds; a consumer bounds
   its INDEX by the matching `n_` and nothing else. */
static __inline const TAGPU_PK_UNIT* tagpu_pk_units(const TAGPU_PACKET* p)
{ return p->n_units ? (const TAGPU_PK_UNIT*)(const void*)((const unsigned char*)p + p->off_units) : (const TAGPU_PK_UNIT*)0; }
static __inline const TAGPU_PK_WRECK* tagpu_pk_wrecks(const TAGPU_PACKET* p)
{ return p->n_wrecks ? (const TAGPU_PK_WRECK*)(const void*)((const unsigned char*)p + p->off_wrecks) : (const TAGPU_PK_WRECK*)0; }
static __inline const TAGPU_PK_ANCHOR* tagpu_pk_anchors(const TAGPU_PACKET* p)
{ return p->n_anchors ? (const TAGPU_PK_ANCHOR*)(const void*)((const unsigned char*)p + p->off_anchors) : (const TAGPU_PK_ANCHOR*)0; }
/* One unit's or wreck's piece run, or NULL when the entry carries none (it sat
   outside the widest zoom rect, or its model has no pieces). `piece_off` was
   checked to lie inside the pieces area with room for `piece_n` entries. */
static __inline const TAGPU_PK_PIECE* tagpu_pk_pieces(const TAGPU_PACKET* p, unsigned off, unsigned n)
{ return (n && off) ? (const TAGPU_PK_PIECE*)(const void*)((const unsigned char*)p + off) : (const TAGPU_PK_PIECE*)0; }
/* The engine's shade table, 32 rows of 256 bytes, or NULL if it did not fit. */
static __inline const unsigned char* tagpu_pk_shd(const TAGPU_PACKET* p)
{ return p->shd_len == TAGPU_PK_SHD_BYTES ? (const unsigned char*)p + p->shd_off : (const unsigned char*)0; }
/* The LHT lighten table, same shape, or NULL. */
static __inline const unsigned char* tagpu_pk_lht(const TAGPU_PACKET* p)
{ return p->lht_len == TAGPU_PK_LHT_BYTES ? (const unsigned char*)p + p->lht_off : (const unsigned char*)0; }
/* ---- the fog grids. Each is NULL, or `cols * rows` u16 corner
   masks — the acquire proved the length is exactly that and that it lies
   inside the record, so `grid[cy * cols + cx]` for cx < cols, cy < rows is
   inside the bytes by construction. ---- */
static __inline const unsigned short* tagpu_pk_fog(const TAGPU_PACKET* p)
{ return p->fog_len ? (const unsigned short*)(const void*)((const unsigned char*)p + p->fog_off) : (const unsigned short*)0; }
static __inline const unsigned short* tagpu_pk_fogw(const TAGPU_PACKET* p)
{ return p->fogw_len ? (const unsigned short*)(const void*)((const unsigned char*)p + p->fogw_off) : (const unsigned short*)0; }
/* the grey band's palette remap, 256 bytes, or NULL */
static __inline const unsigned char* tagpu_pk_fogshade(const TAGPU_PACKET* p)
{ return p->fogsh_len == TAGPU_PK_FOGSHADE_BYTES ? (const unsigned char*)p + p->fogsh_off : (const unsigned char*)0; }
/* ---- the UI layer's render half ---- */
/* the three minimap surfaces interleaved, mm_w * mm_h RGB triples, or NULL */
static __inline const unsigned char* tagpu_pk_minimap(const TAGPU_PACKET* p)
{ return p->mm_len ? (const unsigned char*)p + p->mm_off : (const unsigned char*)0; }
/* the level's decoded minimap picture, mmpic_w * mmpic_h palette indices —
   present in the level's FIRST in-play packet and in no other, so a consumer
   that needs it across frames copies it and keys the copy on `level_gen`. */
static __inline const unsigned char* tagpu_pk_minimap_pic(const TAGPU_PACKET* p)
{ return p->mmpic_len ? (const unsigned char*)p + p->mmpic_off : (const unsigned char*)0; }
/* ---- the effects tables ---- */
static __inline const TAGPU_PK_PROJ* tagpu_pk_proj(const TAGPU_PACKET* p)
{ return p->n_proj ? (const TAGPU_PK_PROJ*)(const void*)((const unsigned char*)p + p->off_proj) : (const TAGPU_PK_PROJ*)0; }
static __inline const TAGPU_PK_EXPL* tagpu_pk_expl(const TAGPU_PACKET* p)
{ return p->n_expl ? (const TAGPU_PK_EXPL*)(const void*)((const unsigned char*)p + p->off_expl) : (const TAGPU_PK_EXPL*)0; }
static __inline const TAGPU_PK_DEBRIS* tagpu_pk_debris(const TAGPU_PACKET* p)
{ return p->n_debris ? (const TAGPU_PK_DEBRIS*)(const void*)((const unsigned char*)p + p->off_debris) : (const TAGPU_PK_DEBRIS*)0; }
static __inline const TAGPU_PK_PART* tagpu_pk_part(const TAGPU_PACKET* p)
{ return p->n_part ? (const TAGPU_PK_PART*)(const void*)((const unsigned char*)p + p->off_part) : (const TAGPU_PK_PART*)0; }
/* the queued builds the order pass is showing site rects for, or NULL */
static __inline const TAGPU_PK_BUILD* tagpu_pk_builds(const TAGPU_PACKET* p)
{ return p->n_builds ? (const TAGPU_PK_BUILD*)(const void*)((const unsigned char*)p + p->off_builds) : (const TAGPU_PK_BUILD*)0; }

/* ---- lifetime ---- */
/* DLL attach, before either thread exists — never lazily: reserves the slots
   of both exchanges and puts each into its initial permutation. A zeroed cell
   would put both threads on slot 0. Disarms itself if the reservation fails. */
void tagpu_packet_init(void);
int  tagpu_packet_armed(void);

/* THE GROWTH-STRESS LEVER, `tagpu_grow.stress` in the gamedir, read once at
   attach like the packet's own. The arrays whose caps this sizes for the
   design point move on every frame under it: the native gather's and the
   marker pass's are freed and allocated again at exactly the size asked for,
   the posed hand-over's arenas grow to the exact size on every append, and
   the Vulkan unit pass rebuilds its two slot buffers. Other growable arrays
   (the unit pass's draw list and staging, the marker hand-over) keep their
   ordinary doubling. A pointer that outlives a move then reads freed memory at the
   unit counts a stock game reaches, rather than only in a game large enough to
   grow the arrays for real. A measurement lever: it costs an allocation per
   array per frame. */
int  tagpu_grow_stress(void);

/* ---- render thread ---- */
/* Exactly once per frame, at the top of the overlay frame, by the driver
   (render_vk.c) and nobody else. Takes the fresh packet if there is one,
   else keeps the one it holds. Returns NULL when no packet has ever arrived,
   when the module is off, or when the packet fails its structural bounds
   (counted as a violation).

   `*prev` is the previously taken packet when it is from the same level, in
   game, and of a DIFFERENT sim tick — so the pair the interpolation blends
   over always spans two ticks, never one. The engine publishes
   several packets per tick, so a consumer that handed PREV back on every take
   would soon hold two of the same tick and the blend would refuse: stepped
   motion, always. The give-back is therefore tick-aware, and the way it is
   made so is the frame instance holding THREE slots rather than two — READ,
   PREV and a SPARE — so the choice is made AFTER the exchange, out of records
   the consumer already owns, instead of peeking at a slot the producer may be
   refilling (tagpu_packet.c, "the rotation"). Both pointers are valid until
   tagpu_packet_frame_end(); caching either across frames is the bug the poison
   lever exists to expose. */
const TAGPU_PACKET* tagpu_packet_acquire(const TAGPU_PACKET** prev);
/* After the frame's LAST read of either packet, unconditionally — every path
   out of the overlay. Verifies the held slot was not rewritten during the
   frame (tail == the head latched at acquire; the CRC under `check`) and
   writes the heartbeat every 300 frames. */
void tagpu_packet_frame_end(unsigned frame_counter);

/* The command record, posted once per render frame from the end of the
   overlay frame (tagpu_zoom_frame_end, every exit path) — the primitive fills
   the prefix and suffix, the caller fills everything between. Latest wins: a
   record the game thread has not taken yet is replaced, not queued, which the
   cumulative fields make lossless. Returns 1 when posted; 0 when the exchange
   is off (nothing is then applied on the game thread either: the engine keeps
   its own camera range, rect and scroll rate). */
int  tagpu_cmd_post(const TAGPU_CMD* c);

/* The per-frame counters the heartbeat needs from the publisher's side, so
   that one log line carries both halves. Registered once by the publisher. */
typedef void (*tagpu_packet_extra_fn)(char* buf, unsigned cap, double seconds);
void tagpu_packet_set_extra(tagpu_packet_extra_fn fn);
#endif
