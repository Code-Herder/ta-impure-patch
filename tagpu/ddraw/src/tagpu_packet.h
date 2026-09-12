#ifndef TAGPU_PACKET_H
#define TAGPU_PACKET_H
/* tagpu_packet — the frame packet exchange, consumer side (landing 1).

   Design: research/notes/frame-packet-exchange.html. The game thread publishes
   a COPY of the per-frame engine state it owns, once per presented frame at
   most, from the DrawGameScreen observer's `after` (post-flip, in-play frames
   only); the render thread takes the latest copy once at the top of its frame
   and reads nothing but that copy for the rest of it. The exchange is one
   aligned word and two `xchg`s — no lock, no wait on either side, four slots
   that are always a permutation of {W, cell, READ, PREV} (tagpu_packet.c).

   WHAT THIS HEADER IS. The packet's layout and the consumer's three calls.
   No engine address appears here: the fields say what they MEAN, and the one
   file that knows where each comes from is the publisher (tagpu_packet_pub.c,
   through inc/tagpu_engine.h). A module on the render thread receives the
   packet pointer from the driver, through TAGPU_FRAME / TAGPU_FXVIEW, and may
   not acquire on its own; the pointer is valid for THIS frame only.

   LANDING 1 carries the header, the marker text's font as glyph bytes, and
   the out-of-game packet. No tables yet: the units, pieces, fog and the rest
   arrive with landings 2..4 and every table then has its `off`/`n` here. */
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
    uint32_t cmd_ack_seq;       /* the last command applied before this draw: 0
                                   until landing 2                              */
    uint32_t gui_flips;         /* the GL UI publisher's flip counter, so the
                                   twin-to-packet skew can be counted           */
    uint32_t draw_seq;          /* the observer's in-play draw count            */
    int32_t  eye[2];            /* the camera's eye, world px                   */
    int32_t  scroll_to[2];      /* where the camera is heading                  */
    int32_t  vp[4];             /* the TRUE 1x viewport: L, T, W, H (never the
                                   widened rect)                                */
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
    uint32_t truncated;         /* TAGPU_PK_TRUNC_* bits: what did not fit      */
    uint32_t crc;               /* CRC-32 of the slot with these two fields as
                                   zero, under `tagpu_packet.check`; else 0     */
    uint32_t tail_seq;          /* stored LAST, after the fill: equals head_seq
                                   or the consumer's frame_end counts a violation */
} TAGPU_PACKET;

/* ---- lifetime ---- */
/* DLL attach, before either thread exists — never lazily: reserves the four
   slots and puts the exchange into its initial permutation. A zeroed cell
   would put both threads on slot 0. Disarms itself if the reservation fails. */
void tagpu_packet_init(void);
int  tagpu_packet_armed(void);

/* ---- render thread ---- */
/* Exactly once per frame, at the top of the overlay frame, by the driver
   (render_ogl.c) and nobody else. Takes the fresh packet if there is one,
   else keeps the one it holds. Returns NULL when no packet has ever arrived,
   when the module is off, or when the packet fails its structural bounds
   (counted as a violation). `*prev` is the previously taken packet when it is
   from the same level and in-game, else NULL. Both pointers are valid until
   tagpu_packet_frame_end(); caching either across frames is the bug the
   poison lever exists to expose. */
const TAGPU_PACKET* tagpu_packet_acquire(const TAGPU_PACKET** prev);
/* After the frame's LAST read of either packet, unconditionally — every path
   out of the overlay. Verifies the held slot was not rewritten during the
   frame (tail == the head latched at acquire; the CRC under `check`) and
   writes the heartbeat every 300 frames. */
void tagpu_packet_frame_end(unsigned frame_counter);

/* The per-frame counters the heartbeat needs from the publisher's side, so
   that one log line carries both halves. Registered once by the publisher. */
typedef void (*tagpu_packet_extra_fn)(char* buf, unsigned cap, double seconds);
void tagpu_packet_set_extra(tagpu_packet_extra_fn fn);
#endif
