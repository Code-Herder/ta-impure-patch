#ifndef TAGPU_PACKET_H
#define TAGPU_PACKET_H
/* tagpu_packet — the frame packet exchange, consumer side (landings 1 and 2).

   Design: research/notes/frame-packet-exchange.html. The game thread publishes
   a COPY of the per-frame engine state it owns, once per presented frame at
   most, from the DrawGameScreen observer's `after` (post-flip, in-play frames
   only); the render thread takes the latest copy once at the top of its frame
   and reads nothing but that copy for the rest of it. The exchange is one
   aligned word and two `xchg`s — no lock, no wait on either side, four slots
   that are always a permutation of {W, cell, READ, PREV} (tagpu_packet.c).

   THE OTHER DIRECTION (landing 2) is the same primitive with the threads
   swapped: the render thread posts a COMMAND record — the zoom level in
   force, the cursor anchor's eye delta, the camera hold, the follow release —
   and the game thread takes the latest one at the top of every in-play draw
   (the observer's `before`, post-tick, pre-draw) and writes the engine's
   words there, on the thread that owns them. No render-thread store into
   engine memory remains. The packet echoes what was applied (`cmd_ack_*`), and
   the render thread draws from the packet's eye PLUS the deltas not yet
   acknowledged — prediction reconciled by the next packet, so a wheel notch
   is drawn on the frame it happens and never wobbles.

   WHAT THIS HEADER IS. The two records' layouts and the consumer's calls. No
   engine address appears here: the fields say what they MEAN, and the one
   file that knows where each comes from is the publisher (tagpu_packet_pub.c,
   through inc/tagpu_engine.h). A module on the render thread receives the
   packet pointer from the driver, through TAGPU_FRAME / TAGPU_FXVIEW, and may
   not acquire on its own; the pointer is valid for THIS frame only.

   Landing 1 carried the header, the marker text's font as glyph bytes, and
   the out-of-game packet. Landing 2 added the view (the addressable rect, the
   palette and gamma, the command acknowledgement) and the command record.
   LANDING 3 adds the four world tables — units, pieces, wrecks and feature
   anchors — with the header fields they need (the GUI colours, the mouse and
   the build cursor, the per-map array bases the fenced passes index, the
   engine's shade table), and makes the acquire tick-aware so that the two
   packets the consumer holds always span two distinct sim ticks. The
   projectile, explosion, particle and fog tables arrive with landing 4 and
   every table then has its `off`/`n` here. */
#include <stdint.h>

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


/* ---- THE WORLD TABLES (landing 3) ---------------------------------------
   One contiguous block per slot: this header, then the tables, 4-aligned, each
   named by an `off_`/`n_` pair below. Every field says where the publisher
   reads it (game thread, in-play frames only) and the bound it applies. A
   `_key` is an engine address that crosses as an OPAQUE IDENTITY — a cache key
   the render thread compares and never dereferences — with one stated
   exception, PK_PIECE.node, whose paragraph says why.

   NOTHING PER-UNIT IS DEREFERENCED ON THE RENDER THREAD ANY MORE. The unit
   array and every Object3do field the passes used to read are copied here by
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
                                what the glTF replacement pass looks a mesh up by
                                and what the roster line prints. 16 is the FBI
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
   teardown fence — the same argument tagpu_posebake.c and tagpu_r3dcache.c
   already stand on, and the one landing 3 deliberately does not change (the
   plan's row 3: "the template ones stay, they are the fence's"). What the
   packet removes here is the per-UNIT read: the PrimitiveStruct lives inside
   the Object3do, which FreeObjectState 0x45AAA0 frees while the render thread
   may be mid-frame, and it is that read — not the template one — that the
   game thread now makes on our behalf. */
typedef struct TAGPU_PK_PIECE {
    int32_t  pos[3];         /* prim+0x04 P_POS, the COB MOVE delta, 16.16      */
    uint16_t turn[3];        /* prim+0x10 P_TURN, 65536 = 360 degrees           */
    uint8_t  flags;          /* prim+0x28 P_FLAGS: bit0 visible                 */
    uint8_t  pad;
    uint32_t node;           /* prim+0x00 P_NODE, the type's template node      */
} TAGPU_PK_PIECE;

/* 36 B, one per live wreck record the anchor rect names. Posed exactly as a
   unit is — the engine draws a husk through a scratch fake unit — so it
   carries the same pose fields. */
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
                                the tile, as the grid walk it replaced did, and
                                only then culls by the projected anchor          */
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

/* truncation bits, one per table (TAGPU_PK_TRUNC_FONT/STRESS are above) */
#define TAGPU_PK_TRUNC_UNITS   0x4u
#define TAGPU_PK_TRUNC_PIECES  0x8u
#define TAGPU_PK_TRUNC_WRECKS  0x10u
#define TAGPU_PK_TRUNC_ANCHORS 0x20u
#define TAGPU_PK_TRUNC_SHD     0x40u

#define TAGPU_PK_SHD_ROWS   32u      /* the engine's PALETTE.SHD shade table:  */
#define TAGPU_PK_SHD_BYTES  (TAGPU_PK_SHD_ROWS * 256u)   /* 32 x 256 bytes     */

/* The piece-count bound, the same number tagpu_model3do.h's TAGPU_PBMAXPIECE
   states as a fact about a model (the largest stock model has 36 pieces).
   Spelled again here so the packet's own bounds check needs no model header;
   tagpu_packet_pub.c compiles a static assertion that the two agree. */
#define TAGPU_PK_MAXPIECE   256u

/* The units table's own ceiling. The engine's slot count is 10 x MaxUnits + 1
   and MaxUnits tops out at 1500, so 15001 is the largest the engine can ask
   for; 16384 is that rounded up, and a slot count past it truncates the table
   (and says so) rather than walking off the publisher's scratch. */
#define TAGPU_PK_MAX_UNITS    16384u
#define TAGPU_PK_MAX_WRECKS   4096u
#define TAGPU_PK_MAX_ANCHORS  65536u

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
    uint32_t gui_flips;         /* the GL UI publisher's flip counter, so the
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

    /* ---- landing 3: the world tables and what reads them ---- */
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
    /* THE PER-MAP ARRAY BASES ARE NOT HERE, AND THAT IS DELIBERATE (the landing
       review found them here and the disassembly agreed). FeatureDef
       `main+0x1426F`, the wreck records `+0x1420B` and `MODEL_PTRS` `+0x14377`
       are POINTERS the level teardown frees AND THEN NULLS — `0x4221F8` then
       `0x422214`, `0x42227D` then `0x42228B`, `0x42DCCB` then `0x42DCD8`, all
       inside the cascade `0x491B60`. That null is the only invalidation the
       fenced passes have ever had, and a copy in a packet that outlives the
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
       take the smaller of the two (landing review, 2026-09-12). */
    int32_t  feat_defcount;
    int32_t  sweep_cols, sweep_rows;  /* the engine's own feature sweep rect size */
    int32_t  mouse[2];                /* the dispatched mouse point, screen px    */
    int32_t  build_rect[6];           /* the build cursor's two corners as
                                         x, altitude, z (0x2C92..0x2CA6)          */
    uint8_t  gui_col[64];             /* GetGuiPaletteColor's byte array          */
    uint8_t  cursor_mode;             /* 0x2CC3: 0x0E = build placement           */
    uint8_t  region_flags;            /* 0x2CC6: bit3 band box, bit6 site OK      */
    uint8_t  game_opt;                /* 0x37F06 low byte: bit0 damagebars,
                                         bit2 Shadow, bit3 TShadow, bit4 FShadow  */
    uint8_t  pad3;
    uint32_t shd_off, shd_len;        /* the engine's 32x256 shade table
                                         (PALETTE.SHD at graphics+0xC4), copied
                                         whole: the Gouraud LUT the unit pass
                                         builds its shading from                  */
    uint32_t truncated;         /* TAGPU_PK_TRUNC_* bits: what did not fit      */
    uint32_t crc;               /* CRC-32 of the slot with these two fields as
                                   zero, under `tagpu_packet.check`; else 0     */
    uint32_t tail_seq;          /* stored LAST, after the fill: equals head_seq
                                   or the consumer's frame_end counts a violation */
} TAGPU_PACKET;

/* THE COMMAND RECORD — render thread to game thread (landing 2). One record
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

/* ---- lifetime ---- */
/* DLL attach, before either thread exists — never lazily: reserves the slots
   of both exchanges and puts each into its initial permutation. A zeroed cell
   would put both threads on slot 0. Disarms itself if the reservation fails. */
void tagpu_packet_init(void);
int  tagpu_packet_armed(void);

/* ---- render thread ---- */
/* Exactly once per frame, at the top of the overlay frame, by the driver
   (render_ogl.c) and nobody else. Takes the fresh packet if there is one,
   else keeps the one it holds. Returns NULL when no packet has ever arrived,
   when the module is off, or when the packet fails its structural bounds
   (counted as a violation).

   `*prev` is the previously taken packet when it is from the same level, in
   game, and of a DIFFERENT sim tick — so the pair the interpolation blends
   over always spans two ticks, never one (landing 3). The engine publishes
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
