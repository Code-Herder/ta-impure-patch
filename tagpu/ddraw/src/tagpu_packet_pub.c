/* tagpu_packet_pub.c — the frame packet's PUBLISHER.
   Contract: tagpu_packet_pub.h. Design: research/notes/frame-packet-exchange.html
   §3 (the publish point), §8 (the loader thread and the out-of-game packet),
   §9 (the marker font as glyph bytes).

   THE ONE FILE THAT READS ENGINE MEMORY FOR THE PACKET, and it reads it on
   the game thread, inside an engine call, at a site that owns what it reads.
   Every address comes from inc/tagpu_engine.h; nothing here is dereferenced
   on the render thread.

   THE PUBLISH POINT is one observer (tagpu_detour_observe) on DrawGameScreen
   0x468CF0 — the site tagpu_menu.c already observes with the same six stolen
   bytes, so the chaining is proven, and ours is installed AFTER the menu's
   because its `before` never hijacks the return and ours must, for `after`.
   The observer acts only when the return address is 0x4969D2, the in-play
   frame callback's call (0x4969CD, `DrawGameScreen(1, 1)`): that gate
   excludes the shell, the screenshot function 0x495A30 (which calls
   DrawGameScreen in a loop while driving the eye, 0x495C76 with no flip and
   0x495E66 at the end), the movie recorder (0x4962C2), and — the reason it
   is a correctness gate and not a filter — every draw of the LOADING SCREEN,
   during which the loader thread owns the per-map arrays (below).
     before  = THE COMMANDS: post-tick, pre-draw, the latest
               record the render thread posted is taken and applied on
               this thread — the camera range (centred while the ground
               is ours), the zoom level (the addressable rect, the minimap
               box, the scroll step), the cursor anchor's eye
               delta, the camera hold, the follow release — so the frame
               the engine is about to draw, its fog rebuild and its minimap
               box all see the commanded camera (tagpu_zoom_apply,
               tagpu_vpwide_apply). The render thread stores nothing into
               engine memory.
     after   = the publish, post-flip, only when the cell holds no FRESH
               packet: the engine draws 330..4900 times a second against
               about 60 presents, so most draws cost one relaxed load
   The scenario applier's tick stub lands its jmp AT 0x4969D2 while a
   scenario is being applied; that changes the bytes there, not the return
   address the call pushed, so the gate holds with it installed.

   THE LOADER THREAD [VERIFIED by disassembly, 2026-09-12]. The game-screen
   enter callback 0x497F40 creates it at 0x4982CA (`push 0x497C70; call
   0x4B6B20`, the CRT's _beginthread over CreateThread + ResumeThread) and
   sets bit 0 of main+0x38D75 at 0x49832A. The thread's entry 0x497C70 is a
   SEH wrapper around 0x497180, the loader body (one function, one `ret` at
   0x497C6C), which calls LoadGameData_Main 0x4917D0 at 0x497581, sets bit 2
   at 0x4975C7 and waits for bit 3 in a SleepBatch100ms loop (0x4975DE..
   0x4975F1; the game thread clears bit 2 and sets bit 3 at 0x49855D/0x498576),
   and as its LAST act sets bit 1 at 0x497C5F/0x497C62. The game thread's
   callback tests bit 1 at 0x498342 before installing the in-play handler. So
   "the game thread owns all of main" is false during a load and true for
   every in-play draw — exactly the set the return-address gate selects, and
   what closes the audit's begin/end hazard by ORDERING rather than by speed.
   The in-play entry zeroes the flag word with bulk stores at 0x49847E and
   0x498480, after the catch-up frame and the state-6 store. That frame's
   first draw can therefore see flags 3; later draws see 0 with no live loader.
   The observer on 0x497C70 (stolen `55 8B EC 6A FF`, position-independent)
   logs the loader's thread id at its entry — a direct measurement rather than
   a call-chain inference.

   THE FONT, AS BYTES. The marker block's font (`[globals+0x204]`) and text
   colour (`+0x208`) are latched at hook 8 (0x469BD7, markown's stub), the
   instant the block that draws the group digit and the ShowRanges labels
   begins — nothing between there and the digit's own draw at 0x469CF9 calls
   SetFont. No note establishes a UI font's lifetime, and a probe is not a
   lifetime argument (CLAUDE.md), so the render thread never dereferences the
   font: the game thread copies the
   font's header and its 95 printable glyphs into a game-side buffer whenever
   the pointer or the header signature changes, each glyph as a one-glyph
   font object the engine's own blitter accepts, and every packet carries the
   buffer (~1.5 KB for a stock font). THE BOUND ON THE COPY IS THE FORMAT: the
   object at `[globals+0x204]` is the `.fnt` file image — `u16 height; u16
   yoff; u16 offset[256]; glyphs` (tools/guifont.py, decoded 2026-09-09 over
   the twenty stock faces) — so the offset table has 256 entries whatever the
   byte at +3 says (it is the high byte of the y-offset word, 0 for every
   such font), every index `c - first` for `c <= 0x7E` is inside it, and an
   entry is a file offset into the block the loader read the file into. A
   font whose byte at +3 is not 0 is not that format and is REFUSED, not
   probed: the copy draws nothing rather than index a table it cannot bound.
   What a corrupt file could do to the engine's own blitter
   it can do here too; that is the same trust, on the same thread. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>         /* free: the loader's hand-over */
#include <string.h>
#include "tagpu_modload.h"
#include "dd.h"           /* g_ddraw: the game's own screen, for the roster's on-screen test */
#include "render_vk.h"     /* vk_render_main: whether the session draws from packets */
#include "tagpu_engine.h"
#include "tagpu_addr.h"   /* tagpu_user_top: the range filters' upper end */
#include "tagpu_packet.h"
#include "tagpu_packet_pub.h"
#include "tagpu_detour.h"
#include "tagpu_reclaim.h"
#include "tagpu_vpwide.h"
#include "tagpu_resource.h"
#include "tagpu_place.h"
#include "tagpu_sweep.h"
#include "tagpu_rangeown.h"
#include "tagpu_formation.h"  /* the formation move's dots, copied in THIS draw */
#include "tagpu_profiles.h"
#include "tagpu_zoom.h"
#include "tagpu_gui.h"
#include "tagpu_native.h"   /* tagpu_native_owns_unit: the ownership answer, taken here */
#include "tagpu_patches.h"   /* tagpu_wire_format: the B3 wire section, DLL counters only */
#include "tagpu_zoom.h"      /* tagpu_zoom_pub_window: the rect the tables cover */
#include "tagpu_model3do.h"  /* TAGPU_PBMAXPIECE, to assert the packet's copy of it */
#include "tagpu_gaf.h"       /* the GAF resolvers: pure reads, taken on THIS thread */
#include "tagpu_fxown.h"     /* whether the render thread wants the effect tables */
#include "tagpu_fogwide.h"   /* the fog source, packed in THIS draw on this thread */
#include "tagpu_terrown.h"   /* whether the fog site ran in this draw */
#include "tagpu_gui.h"       /* whether the render half wants the minimap surfaces */
#include "tagpu_log.h"
#include "tagpu_order.h"     /* tagpu_order_copy_builds: the build-ghost table,
                                out of the snapshot the squares draw from; and
                                tagpu_order_art_ensure, the art the dots draw    */
#include "tagpu_surf.h"      /* the golden source: TA's composed frame, copied on
                                THIS thread at the one point it is finished      */
#include "tagpu_feat.h"      /* whether the mirror wants the map's own features */
#include "tagpu_hudstate.h"   /* the replay HUD's record, through the replay's own calls */
#include "tagpu_wiki.h"       /* the in-game wiki's record: Impure's own state, and the two engine
                                values it follows, read here (wiki_side, wiki_iface) */
#include "tagpu_hudinput.h"   /* ...and the controls it drew, handed back */
#include "tagpu_datakeys.h"  /* the build ghost's piece masks, computed on THIS thread */
#include "tagpu_replay.h"    /* the replay's overlays, built at its frame visit on THIS thread */
#include "tagpu_icontype.h"  /* the icons' type table, built here at the in-play fill */
#include "tagpu_iconset.h"   /* a mod's icon set: its files' parsers, pure */

/* DrawGameScreen's prologue, `sub esp,0x214` — the same six bytes
   tagpu_menu.c observes */
static const unsigned char DRAW_STOLEN[6]   = { 0x81, 0xEC, 0x14, 0x02, 0x00, 0x00 };
/* the loader thread's entry: `push ebp; mov ebp,esp; push -1` */
static const unsigned char LOADER_STOLEN[5] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF };

#define RET_DEPTH 8

static void plog(const char* s)
{
    tagpu_log(s);
}

static int ptr_ok(const void* p) { return (size_t)p > 0x10000u && (size_t)p <= tagpu_user_top(); }

static DWORD    s_gameTid;                 /* DllMain's thread: the game loop's */
static int      s_installed, s_countOnly, s_stress;
static void*    s_retStack[RET_DEPTH];     /* hijacked returns, LIFO            */
static int      s_retDepth;
/* counters: written on the game thread, read by the heartbeat */
static volatile unsigned s_cDrawsAll, s_cDraws, s_cForeign, s_cDeep;
/* the shell's cursor channel — declared here, with the rest of
   the counters, because the heartbeat reads them and is defined above the
   channel's own section */
static volatile unsigned s_cCursorDraws, s_cCursorOwned, s_cCursorHidden,
                         s_cCursorPub, s_cCursorSkip, s_cCursorForeign;
static volatile unsigned s_cCursorStuck;    /* hijack slots that were never given back */
/* the apply's cost: 2 us buckets to 512 us, plus an overflow bucket, written
   on the game thread and read by the heartbeat on the render thread */
#define APPLY_HIST_N 256
static volatile unsigned s_applyHist[APPLY_HIST_N + 1];
static unsigned          s_applyPrev[APPLY_HIST_N + 1];   /* render thread only */
static LARGE_INTEGER     s_freq;
/* the tick's start */
static unsigned       s_lastTick;
static int            s_haveTick;
static LARGE_INTEGER  s_tickStart;
/* the level log */
static int      s_levelOpen;               /* an in-play frame has been published this level */
static unsigned s_levelDraws;
static unsigned s_shellFlags = 0xFFFFu;    /* main+0x38D75 at the first non-in-play draw after a teardown; 0xFFFF = none seen */
static int      s_shellSeen;
static DWORD    s_loaderTid;
static unsigned s_loaderEntries;
/* the font copy: game thread only */
static const unsigned char* s_fontPtr;
static unsigned char s_fontSig[3];
static unsigned      s_fontGen;
static unsigned char s_fontRows, s_fontFirst;
static signed char   s_fontYoff;
static TAGPU_PK_GLYPH s_glyph[TAGPU_PK_NGLYPH];
static unsigned char s_fontArea[TAGPU_PK_FONT_MAX];
static unsigned      s_fontLen;
static int           s_fontTrunc;
static int           s_textFg = -1;
static volatile unsigned s_cFontCopies, s_cFontRefused;
static int      s_levelEndBy;              /* the level-end packet's provider: 1 reclaim's post hook, 2 our observer, 0 none */
/* THE LEVEL GENERATION IS THE PACKET'S OWN, and it has to be: tagpu_reclaim's
   counter is bumped only in reclaim's teardown post hook, so with reclaim
   unarmed it never moves, and every consumer keyed on it would silently stop
   invalidating. `frame_pair` withholds a PREV across a level on it, the pose
   blend refuses a pair on it, and the model-template caches drop on it. This
   one advances at every level end, whoever published it. Reclaim's own counter
   still exists for reclaim's own ring. */
static unsigned s_levelGen;

static int on_game_thread(void)
{
    return s_gameTid != 0 && GetCurrentThreadId() == s_gameTid;
}

static const char* ta_main(void)
{
    const char* ta = *(const char* const*)TA_MAIN_PP;
    return ptr_ok(ta) ? ta : NULL;
}

#define RD32(ta, off)  (*(const int*)((ta) + (off)))
#define RDU32(ta, off) (*(const unsigned*)((ta) + (off)))
#define RDU16(ta, off) (*(const unsigned short*)((ta) + (off)))
#define RDI16(ta, off) (*(const short*)((ta) + (off)))
#define RDU8(ta, off)  (*(const unsigned char*)((ta) + (off)))

static unsigned load_flags(void)
{
    const char* ta = ta_main();
    return ta ? RDU16(ta, OFF_LOADFLAGS) : 0xFFFFu;
}

/* ---- the font copy (hook 8) --------------------------------------------- */

void tagpu_packet_pub_font_snapshot(void)
{
    const char* g;
    const unsigned char* f;
    const unsigned short* tab;
    unsigned char sig[3];
    unsigned c, pos = 0, n = 0, dropped = 0;
    char b[200];

    if (!on_game_thread()) return;
    g = *(const char* const*)TA_GFX_PP;
    if (!ptr_ok(g)) return;
    s_textFg = *(const int*)(g + GFX_TEXTFG);
    f = *(const unsigned char* const*)(g + GFX_FONT);
    if (!ptr_ok(f)) return;
    sig[0] = f[0]; sig[1] = f[2]; sig[2] = f[3];
    if (f == s_fontPtr && !memcmp(sig, s_fontSig, 3)) return;   /* the common case: nothing changed */

    /* A DIFFERENT FONT, by pointer or by header: the pointer alone is what an
       allocator recycles. Copy it. The bounds are the engine's own layout:
       first <= 0x7E (else none of our 95 codes exists), a glyph is rows x w
       bits packed MSB-first across row boundaries, w == 0 is refused because
       the blitter's do-while would write 256 columns for it. */
    s_fontPtr = f; memcpy(s_fontSig, sig, 3);
    s_fontLen = 0; s_fontTrunc = 0;
    memset(s_glyph, 0, sizeof s_glyph);
    s_fontRows = sig[0]; s_fontYoff = (signed char)sig[1]; s_fontFirst = sig[2];
    /* the format bound (file comment): a `.fnt` image has first == 0 and a
       256-entry table; anything else is not a font this copy can bound */
    if (s_fontRows == 0 || s_fontFirst != 0) {
        s_fontGen++;
        s_cFontRefused++;
        if (s_cFontRefused <= 4) {
            _snprintf(b, sizeof b, "packet: font %p REFUSED (rows=%u first=0x%02X: not a .fnt image): no glyphs carried, gen %u",
                      (const void*)f, (unsigned)s_fontRows, (unsigned)s_fontFirst, s_fontGen);
            b[sizeof b - 1] = 0; plog(b);
        }
        return;
    }
    tab = (const unsigned short*)(f + 4);
    for (c = TAGPU_PK_GLYPH_LO; c <= TAGPU_PK_GLYPH_HI; c++) {
        unsigned off, w, len, i = c - TAGPU_PK_GLYPH_LO;
        unsigned char* o;
        if (c < s_fontFirst) continue;                    /* 0x4CCFAA skips it  */
        off = tab[c - s_fontFirst];
        if (!off) continue;                               /* 0x4CCFB9 skips it  */
        w = f[off];
        if (w == 0) continue;                             /* would smear 256 columns */
        len = ((unsigned)s_fontRows * w + 7u) >> 3;
        if (pos + TAGPU_PK_GLYPH_HDR + len > TAGPU_PK_FONT_MAX) { dropped++; s_fontTrunc = 1; continue; }
        o = s_fontArea + pos;
        o[0] = s_fontRows; o[1] = 0; o[2] = (unsigned char)s_fontYoff; o[3] = (unsigned char)c;
        o[4] = (unsigned char)TAGPU_PK_GLYPH_HDR - 1u; o[5] = 0;   /* the one table entry: 6 */
        o[6] = (unsigned char)w;
        memcpy(o + TAGPU_PK_GLYPH_HDR, f + off + 1, len);
        s_glyph[i].off = (unsigned short)pos;
        s_glyph[i].w   = (unsigned char)w;
        pos += (TAGPU_PK_GLYPH_HDR + len + 3u) & ~3u;
        n++;
    }
    s_fontLen = pos;
    s_fontGen++;
    s_cFontCopies++;
    /* one line per copy for the first few, then every 64th: two fonts
       alternating at hook 8 would otherwise write a line per engine draw */
    if (s_cFontCopies <= 4 || (s_cFontCopies & 63u) == 0) {
        _snprintf(b, sizeof b, "packet: font %p copied: rows=%u yoff=%d first=0x%02X glyphs=%u bytes=%u dropped=%u gen=%u fg=%d (copy #%u)",
                  (const void*)f, (unsigned)s_fontRows, (int)s_fontYoff, (unsigned)s_fontFirst, n, pos, dropped, s_fontGen, s_textFg, s_cFontCopies);
        b[sizeof b - 1] = 0; plog(b);
    }
}


/* ---- the fills ----------------------------------------------------------- */

/* WHAT THE TABLES THAT DID NOT FIT WOULD HAVE TAKEN, in this fill. A table
   that does not fit leaves the cursor where it was, so the tables after it
   pack into the room it would have used, and the largest end any one of them
   asked for is not the size of the fill: with several tables cut, a slot grown
   to that end would cut one again. Every refusal below adds its table's
   4-aligned length here. In the layout where everything fits, each table
   after a refused one moves by exactly that length, and a move by a multiple
   of 4 keeps every alignment, so `ALIGN4(cursor) + s_fillShort` is at least
   the size of the whole fill (fill_frame returns it). That is what lets
   pkx_publish grow the slot once and fill it again instead of publishing the
   cut frame. Zeroed at the top of fill_frame; every appender runs inside it,
   on the game thread. */
static unsigned s_fillShort;
/* 1 while fill_frame redoes the fill it made for this same publish (same
   head_seq): pkx_publish grew the slot under it. Set at the top of
   fill_frame; the caches' reuse counters skip a fill that is done again, and
   PK_FILL_COUNTS / PK_FC_UNDO (above fill_frame) take back what the
   discarded fill counted. */
static int      s_fillAgain;

/* the replay HUD's record, built here and copied into the area: 3.4 KB, off the stack */
static TAGPU_PK_HUD s_hud;
static TAGPU_PK_HUDSTAT s_hudstat;
static TAGPU_PK_WIKI s_wiki;

static unsigned append_area(TAGPU_PACKET* p, unsigned* cursor, const void* src, unsigned len,
                            unsigned* off_out, unsigned* len_out, unsigned trunc_bit)
{
    unsigned at = (*cursor + 3u) & ~3u;
    unsigned end = at + ((len + 3u) & ~3u);
    if (!len) { *off_out = 0; *len_out = 0; return *cursor; }
    if (end > p->cap_bytes) {
        p->truncated |= trunc_bit; *off_out = 0; *len_out = 0;
        s_fillShort += (len + 3u) & ~3u;
        return end;
    }
    if (src) tagpu_pk_copy((unsigned char*)p + at, src, len);
    else tagpu_pk_fill((unsigned char*)p + at, 0x5A, len);
    *off_out = at; *len_out = len;
    *cursor = end;
    return end;
}

/* ======================= THE WORLD TABLES =================================
   Everything below runs on the GAME THREAD, inside DrawGameScreen, on the
   in-play gate — the one set of frames during which the loader thread has
   finished and this thread owns every array it touches (the file comment's
   "THE LOADER THREAD"). That ordering is what closes the audit's hazard: no
   read-side gate could make a render-thread read of the unit array's
   begin/end pair safe, because `end` is never nulled.

   THE BOUNDS ARE THE ENGINE'S OWN COUNTS, APPLIED HERE AND NOWHERE ELSE.
   The walk runs to `unit_slots` (u16 main+0x14351, stored at 0x4854EF BEFORE
   `begin`), never to the `end` pointer — so it needs no pair at all and cannot
   be skewed by one. A model id is dropped unless it is inside UNITINFOCount, a
   feature def unless it is inside NumFeatureDefs, a piece count unless it is
   inside TAGPU_PK_MAXPIECE, a cargo link unless it resolves to a slot this
   packet carries. Past this file every index is a packet index into a table
   whose length the consumer's own bounds check has already verified.

   THE LAYOUT, and why it is this order: header | pieces | units | unit types |
   wrecks | anchors | font | shd. A unit's `piece_off` has to be an absolute byte offset
   when the entry is written, so the piece arena has to start at a known place
   — which means first. The unit and wreck entries are therefore built in
   file-static scratch and copied in afterwards; the anchors likewise, because
   the wrecks come out of the same walk. The scratch is ~2.7 MB of BSS, touched
   only as far as a level actually fills it. */

#define PKT_ALIGN4(x)   (((x) + 3u) & ~3u)

/* THE LAYOUT IS PART OF THE CONTRACT, so it is asserted rather than described:
   the consumer indexes these tables by stride, and a compiler that padded one
   differently would read every entry but the first at the wrong offset. The
   packet's copy of the piece bound has to be the model header's number too. */
typedef char pk_maxpiece_agrees[(TAGPU_PK_MAXPIECE == TAGPU_PBMAXPIECE) ? 1 : -1];
/* the units table holds every slot of the largest game this is built for, and
   that game is at least the one the installed unit limit allows */
typedef char pk_units_design[(TAGPU_PK_MAX_UNITS >= TAGPU_PK_DESIGN_SLOTS) ? 1 : -1];
typedef char pk_limit_design[(10u * TAGPU_LIM_UNITS + 1u <= TAGPU_PK_DESIGN_SLOTS) ? 1 : -1];
typedef char pk_unit_size  [(sizeof(TAGPU_PK_UNIT)   == 104) ? 1 : -1];
typedef char pk_unit_rsel  [(offsetof(TAGPU_PK_UNIT, pad0) == 85 &&
                             offsetof(TAGPU_PK_UNIT, rsel) == 86) ? 1 : -1];
typedef char pk_overlay_size[(sizeof(TAGPU_PK_OVERLAY) == 16) ? 1 : -1];
/* the place table is tagpu_place.h's site byte for byte: fill_place hands the
   module's own array to append_table as the packet's rows */
typedef char pk_place_size [(sizeof(TAGPU_PK_PLACE) == 28 &&
                             sizeof(TagpuPlaceSite) == sizeof(TAGPU_PK_PLACE)) ? 1 : -1];
typedef char pk_place_layout[(offsetof(TagpuPlaceSite, rect) == offsetof(TAGPU_PK_PLACE, rect) &&
                              offsetof(TagpuPlaceSite, type) == offsetof(TAGPU_PK_PLACE, type) &&
                              offsetof(TagpuPlaceSite, colour) == offsetof(TAGPU_PK_PLACE, colour) &&
                              offsetof(TagpuPlaceSite, pad) == offsetof(TAGPU_PK_PLACE, pad)) ? 1 : -1];
/* the range rings' two records: tagpu_rangeown.c fills them by name and tagpu_range.c reads them
   by name, so only the sizes the acquire's strides assume are pinned */
typedef char pk_range_size[(sizeof(TAGPU_PK_RANGE) == 24 && sizeof(TAGPU_PK_RING) == 32) ? 1 : -1];

/* the sweep's two records, field by field: tagpu_sweep.c fills them by name and the marker pass
   reads them by name, so a padding the compiler chose differently would only move the bytes --
   but the layout is the one the header's comment states, and the acquire's bounds name it */
typedef char pk_sweep_size  [(sizeof(TAGPU_PK_SWEEP) == 52 &&
                              sizeof(TAGPU_PK_SWEEPITEM) == 20) ? 1 : -1];
typedef char pk_sweep_layout[(offsetof(TAGPU_PK_SWEEP, flags) == 0 &&
                              offsetof(TAGPU_PK_SWEEP, kind) == 1 &&
                              offsetof(TAGPU_PK_SWEEP, hot) == 2 &&
                              offsetof(TAGPU_PK_SWEEP, style) == 3 &&
                              offsetof(TAGPU_PK_SWEEP, pop) == 4 &&
                              offsetof(TAGPU_PK_SWEEP, tongue) == 12 &&
                              offsetof(TAGPU_PK_SWEEP, c) == 20 &&
                              offsetof(TAGPU_PK_SWEEP, r) == 32 &&
                              offsetof(TAGPU_PK_SWEEP, nwreck) == 36 &&
                              offsetof(TAGPU_PK_SWEEP, ntree) == 38 &&
                              offsetof(TAGPU_PK_SWEEP, metal) == 40 &&
                              offsetof(TAGPU_PK_SWEEP, energy) == 44 &&
                              offsetof(TAGPU_PK_SWEEP, col) == 48) ? 1 : -1];
typedef char pk_sweepitem_layout[(offsetof(TAGPU_PK_SWEEPITEM, kind) == 0 &&
                                  offsetof(TAGPU_PK_SWEEPITEM, colour) == 1 &&
                                  offsetof(TAGPU_PK_SWEEPITEM, pad) == 2 &&
                                  offsetof(TAGPU_PK_SWEEPITEM, x) == 4 &&
                                  offsetof(TAGPU_PK_SWEEPITEM, h) == 8 &&
                                  offsetof(TAGPU_PK_SWEEPITEM, z) == 12 &&
                                  offsetof(TAGPU_PK_SWEEPITEM, w) == 16 &&
                                  offsetof(TAGPU_PK_SWEEPITEM, d) == 18) ? 1 : -1];
/* the formation table is tagpu_formation.h's dot byte for byte: fill_form hands the module's
   own array to append_table as the packet's rows */
typedef char pk_form_size  [(sizeof(TAGPU_PK_FORMDOT) == 12 &&
                             sizeof(TagpuFormDot) == sizeof(TAGPU_PK_FORMDOT)) ? 1 : -1];
typedef char pk_form_layout[(offsetof(TagpuFormDot, x) == offsetof(TAGPU_PK_FORMDOT, x) &&
                             offsetof(TagpuFormDot, alt) == offsetof(TAGPU_PK_FORMDOT, alt) &&
                             offsetof(TagpuFormDot, z) == offsetof(TAGPU_PK_FORMDOT, z)) ? 1 : -1];
typedef char pk_piece_size [(sizeof(TAGPU_PK_PIECE)  ==  24) ? 1 : -1];
typedef char pk_wreck_size [(sizeof(TAGPU_PK_WRECK)  ==  44) ? 1 : -1];
typedef char pk_anchor_size[(sizeof(TAGPU_PK_ANCHOR) ==  16) ? 1 : -1];
typedef char pk_mapfeat_size[(sizeof(TAGPU_PK_MAPFEAT) == 8) ? 1 : -1];
typedef char pk_utype_size [(sizeof(TAGPU_PK_UTYPE) == 8) ? 1 : -1];

static TAGPU_PK_UNIT   s_uScratch[TAGPU_PK_MAX_UNITS];
/* what fill_frame last wrote. Read back by roster_log immediately after the
   publish, on the producer's own thread.

   THE INVARIANT IS THE CONSUMER'S ROTATION, NOT "CONSUMERS ONLY READ".
   "Consumers only read" is false under the
   `tagpu_packet.poison` lever, where the consumer memsets the header of the
   slot it hands back. What actually holds: for the slot just published to
   reach the consumer's spare -- the one poison touches -- the consumer needs
   two more acquires, each needing FRESH in the cell, and only a PUBLISH sets
   FRESH. There is exactly one producer thread (prodByRole 0, any other
   refused as `foreign`), and it is the thread sitting inside roster_log. So
   the rotation cannot advance while we read. Move roster_log anywhere the
   producer is not holding the thread, or add a second publisher, and this is
   gone. */
static const TAGPU_PACKET* s_lastFilled;
static TAGPU_PK_WRECK  s_wScratch[TAGPU_PK_MAX_WRECKS];
static TAGPU_PK_ANCHOR s_aScratch[TAGPU_PK_MAX_ANCHORS];
/* the anchor scan's own order (rows outward from the view's centre row) before
   it is put back in row order into s_aScratch, and where each row landed */
#define PK_ANCH_MAXROWS 4096
static TAGPU_PK_ANCHOR s_aScan[TAGPU_PK_MAX_ANCHORS];
static unsigned        s_aRowAt[PK_ANCH_MAXROWS], s_aRowN[PK_ANCH_MAXROWS];
/* slot -> index in the units table, for the cargo links; 0xFFFF = not carried
   in this packet. Sized like the scratch and rewritten for the slots the walk
   actually visits, so no memset of 32 KB per frame. */
static unsigned short  s_slotIdx[TAGPU_PK_MAX_UNITS];
static unsigned        s_slotWalk;      /* the slots this frame's walk wrote: 0 = none */
/* the stable-id collision oracle: one bit per id, cleared only for the ids
   this packet used (the sweep is over n_units, not over 65536) */
static unsigned char   s_idSeen[8192];
static unsigned short  s_idUsed[TAGPU_PK_MAX_UNITS];

/* the shade table, latched like the font: a pointer plus its first bytes, so a
   reallocation or a rebuild is noticed. PALETTE.SHD is built at init and the
   note records no rebuild, but "no note establishes it" is exactly the font's
   lesson, so it is re-copied whenever either changes. */
static unsigned char s_shd[TAGPU_PK_SHD_BYTES];
static const unsigned char* s_shdPtr;
static int s_shdOk;
static volatile unsigned s_cShdCopies;

/* THE ANCHOR SCAN IS PER TICK, SO IT IS TAKEN PER TICK. The widest zoom rect
   is 465 x 273 cells on a 1080p viewport over Two Continents — 126 945 u16
   loads, which MEASURED 168 us of the publish's first 200 (2026-09-12). The
   grid it reads is sim state: the def index, the flags nibble and the wreck
   index are written by the tick and by nothing else, so two publishes of one
   tick over one rect must produce the same table, and the second may reuse the
   first. At the 256 published frames a second the reference setup reaches
   against a 60 Hz sim that is four publishes out of five; at or below the sim rate it
   costs one comparison and changes nothing. The WRECKS are re-derived from the
   cached anchors either way, because their piece runs go into THIS packet's
   arena. The key is everything the scan's result depends on: the tick, the
   rect and the centre row it scans outward from. */
static unsigned s_aTick;
static int      s_aHave, s_aTrunc, s_aRect[4], s_aMid;
static unsigned s_aN;
static volatile unsigned s_cAnchScan, s_cAnchReuse;

/* THE MAP'S OWN FEATURES (TAGPU_PK_MAPFEAT), the game thread's copy: adopted
   from the loader's hand-over at the level's first in-play draw
   (`mapfeat_adopt`), and level `s_mfLevel`'s while `s_mfHeld`. Keyed on the
   level rather than reset to a sentinel, so no level_gen reads as "none". */
static TAGPU_PK_MAPFEAT s_mfScratch[TAGPU_PK_MAX_MAPFEAT];
static unsigned s_mfN, s_mfLevel;
static int      s_mfHeld, s_mfTrunc;
static volatile unsigned s_cMfCarried;
/* ...and the LOADER'S HAND-OVER, the one thing both threads touch. The loader
   thread builds the list on the heap and parks it here (`mapfeat_at_load`,
   through `tagpu_packet_pub_mapfeat_loaded`); the game thread takes it over. Every
   access to the slot is under `s_mfLock`, an interlocked lock: the loader
   waits for it -- the game thread holds it for one copy of at most 512 KB --
   and the game thread only tries it, and on a miss adopts at the next draw.
   So the list is read only after it is written, by an ordering and not by the
   load having happened to finish. Every join empties the slot before it
   builds (`mapfeat_unpark`), so a second load before the first list is
   adopted replaces it -- with its own list, or with nothing if its build
   fails -- rather than racing it.
   THE LOCK'S CONTRACT, TO THE COMPILER AS WELL AS THE CPU: taken with
   InterlockedCompareExchange (a `lock cmpxchg`, and a full barrier as the
   __sync builtin it expands to), released with a RELEASE store, so every
   store to the slot made under the lock is visible before the lock reads
   free and none of them sinks past it -- nor past the free() of the list it
   just unhooked. Not InterlockedExchange: mingw spells that
   __sync_lock_test_and_set, whose documented contract is acquire-only
   (tagpu_packet.c, "ORDERING", has the same gap and the same answer). */
static volatile LONG     s_mfLock;
static TAGPU_PK_MAPFEAT* s_mfLoadE;       /* malloc'd by the loader; freed by whoever
                                             replaces or adopts it                     */
static unsigned          s_mfLoadN, s_mfLoadLevel;
static int               s_mfLoadHave, s_mfLoadTrunc;

/* counters the heartbeat prints; game thread writes, render thread reads */
static volatile unsigned s_cUnitTrunc, s_cWreckTrunc, s_cAnchTrunc, s_cPieceTrunc;
static volatile unsigned s_cUnitDup, s_cRelBad, s_cAnchCells, s_cWreckOob;
static volatile unsigned s_cLastUnits, s_cLastPieces, s_cLastWrecks, s_cLastAnchors;

static void shd_snapshot(void)
{
    const char* g = *(const char* const*)TA_GFX_PP;
    const unsigned char* t;
    if (!ptr_ok(g)) return;
    t = *(const unsigned char* const*)(g + GFX_SHD);
    if (!ptr_ok(t)) return;
    if (t == s_shdPtr && s_shdOk) return;
    /* THE BOUND IS THE FORMAT: 0x459C70's Gouraud path indexes [row][idx] with
       a 5-bit row and a byte, so the table is exactly 32 x 256 and a copy of
       that size reads what the rasteriser reads and nothing more. */
    memcpy(s_shd, t, sizeof s_shd);
    s_shdPtr = t; s_shdOk = 1; s_cShdCopies++;
}

/* THE LOGO COLOUR OF PLAYER `id`: PlayerInfo (`main+0x1B63+0x27+id*0x14B`)
   `+0x96`, the frame index 0x45861C hands 0x4B7F30 for a team-coloured face of
   a unit. The id is bounded by the ten PlayerStructs the engine keeps, and the
   PlayerInfo is the game's own allocation, which 0x45861C reads for every such
   face on every frame. The byte is DATA: the consumer bounds it against the
   sequence's frame count. A byte past the ten team colours (logos.gaf carries
   ten frames per entry) is not a colour and reads as -1, which keeps the
   bake's material key to eleven values however the byte is corrupted. -1
   also when the id or the pointer does not validate. DISASSEMBLED
   (0x459B34..0x459B58). */
static int player_logo(const char* ta, unsigned id)
{
    const char* pi;
    unsigned b;
    if (id >= PLAYER_COUNT) return -1;
    pi = *(const char* const*)(ta + OFF_PLAYERS + id * PLAYER_STRIDE + PL_INFO);
    if (!ptr_ok(pi)) return -1;
    b = *(const unsigned char*)(pi + PI_LOGO);
    return b < PLAYER_COUNT ? (int)b : -1;
}

/* ---- THE IN-GAME WIKI'S CONTEXT: the controlled player's side and the Interface Type, read
   here at the fill, on the game thread, in game, and handed to tagpu_wiki_fill by value
   (tagpu_wiki.c reads no engine memory). ---- */
#define WIKI_PLAYER_RECORDS 11      /* main's player records: the ten seats and the viewer's, which
                                       a replay controls (0x2A42 = 10, MEASURED 2026-10-28); all
                                       eleven built by 0x463BE0 from 0x41D98B (tagpu_recorder.c) */
#define PI_SIDE             0x95    /* u8 PlayerInfo's side: sidedata's [SIDEn] index            */
#define OFF_IFACE           0x37EFA /* i32 the Interface Type                                    */

/* THE CONTROLLED PLAYER'S SIDE, as the engine's sidebar router reads it: the record at
   `main+0x1B63 + 0x14B * byte[main+0x2A42]`, its PlayerInfo at +0x27, the side byte at +0x95
   (DISASSEMBLED 0x41AD1C..0x41AD66). In a replay 0x2A42 names the viewer's record, whose side the
   sidebar shows -- the wiki follows what the sidebar shows.
   LIFETIME: main's eleven PlayerInfo blocks are allocated by the record constructor and nothing
   frees one (tagpu_recorder.c, the player records), and the router dereferences this one on this
   thread in game (0x41AD61); this runs on the same thread, in game, between engine calls, so the
   block is the engine's own live allocation while it is read. ptr_ok is a sanity filter on the
   pointer's value, not that argument.
   BOUND: the record index is held below the eleven records; the byte returned is DATA -- the wiki
   maps it through the published WIKISIDE's side_of_engine, below WIKI_SIDE_SIDES and mapped, else
   no casting and the fixture -- so a wrong value shows another side's tutorial and never reads
   memory the engine does not own. -1 when the index or the pointer does not validate. */
static int wiki_side(const char* ta)
{
    const unsigned p = RDU8(ta, OFF_WATCHED);
    const char* pi;
    if (p >= WIKI_PLAYER_RECORDS) return -1;
    pi = *(const char* const*)(ta + OFF_PLAYERS + p * PLAYER_STRIDE + PL_INFO);
    if (!ptr_ok(pi)) return -1;
    return RDU8(pi, PI_SIDE);
}

/* THE INTERFACE TYPE, main+0x37EFA, as the four readers that decide the left button's rows test
   it, `== 1` (0x43E505, 0x43F9EF, 0x499046, 0x499567, DISASSEMBLED): 1 right-click, anything
   else left-click. The typed command IFace (0x416D20) stores any integer, unclamped, and the
   registry load's clamp is signed (0x42F9CD), so a value outside {0, 1} is the engine's mixed
   interface -- its two `!= 0` readers (0x499162, the right press; 0x499352, a release) take the
   type-1 branch -- which the wiki shows as the left-click one (research/notes/ingame-wiki.md,
   Gaps). */
static int wiki_iface(const char* ta)
{
    return RD32(ta, OFF_IFACE) == 1;
}

/* One live unit's entry. `ta` and `u` are the game thread's own; every value
   that leaves here is either a copy of a field or an index this function has
   already bounded. */
static void fill_unit(TAGPU_PK_UNIT* e, const char* ta, const char* u,
                      unsigned slot, unsigned udefCount, const char* udefs)
{
    const char* def;
    const char* o3;
    unsigned i;

    memset(e, 0, sizeof *e);
    e->slot        = (unsigned short)slot;
    e->state       = RDU32(u, U_STATE);
    e->pos[0]      = RD32(u, U_XFIX);
    e->pos[1]      = RD32(u, U_ZFIX);
    e->pos[2]      = RD32(u, U_YFIX);
    for (i = 0; i < 3; i++) e->rot[i] = RDU16(u, U_ROT + i * 2);
    e->id          = RDU16(u, U_INDEX);
    e->model_id    = RDU16(u, U_MODELID);
    e->owner       = RDU8(u, U_OWNER);
    {
        const int lg = player_logo(ta, e->owner);
        e->logo = lg < 0 ? TAGPU_PK_LOGO_NONE : (unsigned char)lg;
    }
    e->cloak       = RDU8(u, U_CLOAKF);
    e->health      = RDI16(u, U_HEALTH);
    e->squad       = RDU32(u, U_SQUAD);
    e->nano        = *(const float*)(u + U_NANO);
    e->o3_key      = (unsigned)(size_t)*(const void* const*)(u + U_OBJ3DO);
    e->cargo_first = -1;
    e->cargo_next  = -1;
    e->type_row    = 0xFFFFu;
    e->base_piece  = 0xFFFFu;
    /* the model id is DROPPED, not clamped, when it is outside the table the
       engine writes (1 .. UNITINFOCount-1): slot 0 is its own "no model" and a
       recycled unit slot is the only way a larger value gets here */
    if (!(e->model_id && udefCount && e->model_id < udefCount)) e->model_id = 0;

    /* THE DEF, BY ROW. `unit+0x92` is a pointer into the UnitDef array; the row
       is what crosses, so the consumer can key a cache on it without ever
       holding an engine address, and the three fields the passes actually read
       come over with it. A pointer that is not a row of that array resolves to
       0xFFFF and the entry carries no def at all. */
    def = *(const char* const*)(u + U_TYPE);
    if (ptr_ok(def) && ptr_ok(udefs) && udefCount && def >= udefs) {
        size_t d = (size_t)(def - udefs);
        if (d % UDEF_STRIDE == 0 && d / UDEF_STRIDE < udefCount) {
            e->type_row   = (unsigned short)(d / UDEF_STRIDE);
            e->def_mask   = RDU32(def, UD_TYPEMASK);
            e->max_health = RD32(def, UD_MAXHP);
            for (i = 0; i < sizeof e->name - 1u; i++) {
                char c = def[UD_NAME + i];
                if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
                e->name[i] = c;
                if (!c) break;
            }
        }
    }

    /* The feature cell under the anchor: the shadow's ground height. The grid
       and its dimensions are read by the caller once per frame. */
    /* (filled by the caller, which holds the grid) */

    o3 = *(const char* const*)(u + U_OBJ3DO);
    if (ptr_ok(o3)) {
        unsigned np = RDU16(o3, O3_NUMPARTS);
        const char* fr;
        if (np && np <= TAGPU_PK_MAXPIECE) e->nparts = (unsigned short)np;
        for (i = 0; i < 3; i++) e->bturn[i] = RDU16(o3, O3_BTURN + i * 2);
        {
            const char* bp = *(const char* const*)(o3 + O3_BASEPRIM);
            const char* p0 = o3 + O3_PRIM0;
            if (bp >= p0 && e->nparts) {
                size_t d = (size_t)(bp - p0);
                if (d % PRIM_STRIDE == 0 && d / PRIM_STRIDE < e->nparts)
                    e->base_piece = (unsigned short)(d / PRIM_STRIDE);
            }
        }
        fr = *(const char* const*)(o3 + O3_COMPOSITE);
        if (ptr_ok(fr)) {
            e->comp_w  = (short)RDU16(fr, GF_WIDTH);
            e->comp_h  = (short)RDU16(fr, GF_HEIGHT);
            e->comp_hx = RDI16(fr, GF_HOTX);
            e->comp_hy = RDI16(fr, GF_HOTY);
            if (RDU32(fr, GF_PTRDEPTH)) e->flags |= TAGPU_PK_U_DEPTHPLANE;
        }
    }
    /* THE OWNERSHIP ANSWER IS TAKEN HERE, ON THIS THREAD, WITH THE DEF IN
       HAND. `tagpu_native_owns_unit` reads the def's three name fields and the
       build fraction, so the unit pass takes the answer from the packet rather
       than calling it from the render thread. It is the same predicate the
       marker pass, the composite wipe and the owndraw classifier ask on the
       game thread, so all four agree by construction rather than by two
       threads reading the same bytes. */
    if (tagpu_native_owns_unit(u)) e->flags |= TAGPU_PK_U_NATIVE;
    (void)ta;
}

/* A unit pointer as a SLOT, or -1: the only arithmetic that ever turns an
   engine address into an index, done once, here. A cargo link that is not a
   stride-aligned member of the array inside the slot count is dropped. */
static short slot_of(const char* beg, const char* u, unsigned slots)
{
    size_t d;
    if (!ptr_ok(u) || u <= beg) return -1;
    d = (size_t)(u - beg);
    if (d % UNIT_STRIDE) return -1;
    d /= UNIT_STRIDE;
    return (d && d < slots && d < 0x7FFFu) ? (short)d : (short)-1;
}

/* one table into the record: 4-aligned, bounded by the committed capacity,
   and the count published only when the bytes actually landed */
static unsigned append_table(TAGPU_PACKET* p, unsigned* cursor, const void* src,
                             unsigned n, unsigned stride,
                             unsigned* off_out, unsigned* n_out, unsigned trunc_bit)
{
    unsigned at = PKT_ALIGN4(*cursor);
    unsigned end = at + n * stride;
    *off_out = 0; *n_out = 0;
    if (!n) return *cursor;
    if (end > p->cap_bytes) { p->truncated |= trunc_bit; s_fillShort += PKT_ALIGN4(n * stride); return end; }
    tagpu_pk_copy((unsigned char*)p + at, src, n * stride);
    *off_out = at; *n_out = n; *cursor = end;
    return end;
}

/* one unit's or wreck's pieces into the arena; returns the pieces written */
static unsigned fill_pieces(TAGPU_PACKET* p, const char* o3, unsigned nparts,
                            unsigned at, unsigned* truncated)
{
    unsigned i;
    unsigned end = at + nparts * (unsigned)sizeof(TAGPU_PK_PIECE);
    if (!nparts) return 0;
    if (end > p->cap_bytes) { *truncated = 1; return 0; }
    for (i = 0; i < nparts; i++) {
        const char* pr = o3 + O3_PRIM0 + i * PRIM_STRIDE;
        TAGPU_PK_PIECE e;
        e.pos[0]  = RD32(pr, P_POS + 0);
        e.pos[1]  = RD32(pr, P_POS + 4);
        e.pos[2]  = RD32(pr, P_POS + 8);
        e.turn[0] = RDU16(pr, P_TURN + 0);
        e.turn[1] = RDU16(pr, P_TURN + 2);
        e.turn[2] = RDU16(pr, P_TURN + 4);
        e.flags   = RDU8(pr, P_FLAGS);
        e.pad     = 0;
        e.node    = RDU32(pr, P_NODE);
        tagpu_pk_copy((unsigned char*)p + at + i * sizeof e, &e, sizeof e);
    }
    return nparts;
}

/* THE ANCHOR RECT: the tables' reach (table_reach: every world point the reach
   bound lets a frame drawn from this packet gather in), in 16-px cells, plus a
   margin, and clamped to the map. Never the zoom in force — the render thread
   may still be drawing from this packet a frame later, and at a level the
   game thread has not seen. The feature pass asks for the engine's sweep
   about the eye it draws from: 10 cells left of its effective rect, 16 rows
   above it, and the sweep's own count on from there (tagpu_feat.c), which
   ends about 2 cells right of that rect and 16 rows below it — inside the
   slab, TAGPU_GATHER_MARGIN (16 cells) about the rect, but for the sweep's
   own rounding of the eye to a cell. The margin and the 16 columns and 40
   rows past the far edges are the slack for that rounding and for a sweep
   longer than the engine's own. A consumer at any zoom asks for a sub-rect of
   this one and counts what falls outside (`outside=`). */
#define PK_ANCH_MARGIN 16           /* 16-px cells on every side               */

static void anchor_rect(const TAGPU_PACKET* p, int loX, int hiX, int loY, int hiY,
                        int* c0, int* r0, int* cols, int* rows)
{
    int mapW = p->map_w16, mapH = p->map_h16;
    /* `>> 4` floors on either sign (arithmetic shift), as the cells do */
    *c0   = (loX >> 4) - PK_ANCH_MARGIN;
    *r0   = (loY >> 4) - PK_ANCH_MARGIN;
    *cols = (hiX >> 4) - (loX >> 4) + 1 + 16 + 2 * PK_ANCH_MARGIN;
    *rows = (hiY >> 4) - (loY >> 4) + 1 + 40 + 2 * PK_ANCH_MARGIN;
    if (*c0 < 0) { *cols += *c0; *c0 = 0; }
    if (*r0 < 0) { *rows += *r0; *r0 = 0; }
    if (*c0 + *cols > mapW) *cols = mapW - *c0;
    if (*r0 + *rows > mapH) *rows = mapH - *r0;
    if (*cols < 0) *cols = 0;
    if (*rows < 0) *rows = 0;
}

/* THE LOADER'S HAND-OVER (tagpu_packet_pub.h). Runs on the LOADER THREAD,
   inside LoadMap. The level it is stamped with is `s_levelGen` as the loader
   reads it: the level end bumps it on the game thread before the next load's
   thread is created, and nothing writes it again until that level ends, so
   the stamp is the level the list's packets will carry. The stamp is also the
   safety, not only the label: `mapfeat_adopt` takes a list only for the level
   it is stamped with, so a stamp that were ever stale would leave that
   level's edge black -- never mirror another level's features. */
void tagpu_packet_pub_mapfeat_loaded(TAGPU_PK_MAPFEAT* e, unsigned n, int trunc)
{
    TAGPU_PK_MAPFEAT* old;
    while (InterlockedCompareExchange(&s_mfLock, 1, 0) != 0) SwitchToThread();
    old = s_mfLoadE;
    s_mfLoadE = e; s_mfLoadN = n; s_mfLoadTrunc = trunc;
    s_mfLoadLevel = s_levelGen; s_mfLoadHave = 1;
    __atomic_store_n(&s_mfLock, 0, __ATOMIC_RELEASE);
    free(old);
}

/* ...and emptied at every join, before this load builds anything: the slot
   holds the LATEST LoadMap's result or nothing. A load abandoned on the
   loading screen tears nothing down, so the level number does not move
   before the next load, and a list it parked would be adopted as the next
   map's if that map's build then failed. Nothing parked is not an empty
   list -- an empty list is a map with no features, and mirrors bare ground
   -- so a failed build leaves the level with no list, and the edge black
   (tagpu_feat_mapfeat_sync). The game thread's own copy cannot stand in
   either: it is adopted only at an in-play draw, and a level that drew in
   play bumps the generation at its end, before the next load's thread
   starts. */
static void mapfeat_unpark(void)
{
    TAGPU_PK_MAPFEAT* old;
    while (InterlockedCompareExchange(&s_mfLock, 1, 0) != 0) SwitchToThread();
    old = s_mfLoadE;
    s_mfLoadE = NULL; s_mfLoadN = 0; s_mfLoadTrunc = 0; s_mfLoadHave = 0;
    __atomic_store_n(&s_mfLock, 0, __ATOMIC_RELEASE);
    free(old);
}

/* ...and its adoption, on the GAME THREAD, at an in-play draw of a level whose
   list is not held yet. Bounded by this side's own capacity whatever the
   loader said. */
static void mapfeat_adopt(void)
{
    TAGPU_PK_MAPFEAT* e = NULL;
    if (s_mfHeld && s_mfLevel == s_levelGen) return;
    if (InterlockedCompareExchange(&s_mfLock, 1, 0) != 0) return;
    if (s_mfLoadHave && s_mfLoadLevel == s_levelGen) {
        unsigned n = s_mfLoadN < TAGPU_PK_MAX_MAPFEAT ? s_mfLoadN : TAGPU_PK_MAX_MAPFEAT;
        if (n && s_mfLoadE) tagpu_pk_copy(s_mfScratch, s_mfLoadE, n * (unsigned)sizeof *s_mfScratch);
        else n = 0;
        s_mfN = n;
        s_mfTrunc = s_mfLoadTrunc || n < s_mfLoadN;
        s_mfLevel = s_levelGen; s_mfHeld = 1;
        e = s_mfLoadE; s_mfLoadE = NULL; s_mfLoadHave = 0;
    }
    __atomic_store_n(&s_mfLock, 0, __ATOMIC_RELEASE);
    free(e);
    if (s_mfHeld && s_mfLevel == s_levelGen) {
        char b[160];
        _snprintf(b, sizeof b, "packet: level gen %u: the map's own features adopted from the load: %u%s",
                  s_levelGen, s_mfN, s_mfTrunc ? " (TRUNCATED at the table's cap)" : "");
        b[sizeof b - 1] = 0; plog(b);
    }
}

/* ...into this packet, WHOLE -- `mapfeat_ok` set only when every entry landed,
   none at all included -- while the consumer wants them and does not yet hold
   this level's. */
static unsigned fill_mapfeat(TAGPU_PACKET* p, unsigned* cursor)
{
    unsigned e;
    if (!s_mfHeld || s_mfLevel != p->level_gen) return *cursor;
    if (!tagpu_feat_mapfeat_want() || tagpu_feat_mapfeat_holds(p->level_gen)) return *cursor;
    e = append_table(p, cursor, s_mfScratch, s_mfN, (unsigned)sizeof(TAGPU_PK_MAPFEAT),
                     &p->off_mapfeat, &p->n_mapfeat, TAGPU_PK_TRUNC_MAPFEAT);
    if (p->n_mapfeat != s_mfN) return e;                 /* cut: the next packet */
    p->mapfeat_ok = 1;
    if (s_mfTrunc) p->truncated |= TAGPU_PK_TRUNC_MAPFEAT;
    s_cMfCarried++;
    return e;
}

/* ================= THE STRATEGIC ICONS' TYPE TABLE ========================
   One TAGPU_PK_UTYPE per UnitDef row (tagpu_icontype.h): the class BAR's icon
   rule gives the type, its draw layer and its model's radius. Built once a
   level, at the level's first in-play fill, and carried whole in every
   in-play packet right after the units table.

   GAME THREAD ONLY. Every def field is read on the thread that owns the def
   array, inside the in-play publish, where the loader thread has finished
   (the file comment's "THE LOADER THREAD"); every row is below
   n = UNITINFOCount, the count the level teardown's 0x42DB90 loops to; a side
   is below both the count the loader stored and the five records the engine
   keeps; a weapon slot counts only when it is a record of the weapon array.

   THE CLASS RULE, first match wins:
     1. COMMANDER -- the type's UnitName is a side's commander name, or the
        type has both showplayername and hidedamage (TADR's rule), so a decoy
        commander is a commander to everyone;
     2. immobile (bmcode 0): a builder that is no air base is a FACTORY, any
        other a BUILDING;
     3. canfly: a builder is an AIRCONS, an armed type an AIRCOMBAT, else AIR;
     4. a builder that is no air base is a CONS, an armed type a COMBAT, else
        UNKNOWN.
   "Armed" is TADR's: one of the def's three weapon slots holds a weapon. */
#define OFF_SIDES          0x37F39   /* u32: the sides this game loaded       */
#define OFF_SIDE_CMD       0x37F5F   /* each side's commander name, 0x20 bytes:
                                        +0x22 into its record, which the loader
                                        0x431A60 lays out from main+0x37F3D    */
#define SIDE_STRIDE        0x232
#define SIDE_ROOM          5u        /* the engine's side records: its free loop
                                        stops at five (0x431A20)               */
#define UD_WEAPONS         0x1EE     /* three weapon pointers, +0x1EE/1F2/1F6  */
#define UD_BMCODE          0x22F     /* u8: 0 = the type cannot move           */
#define UD_TYPEMASK2       0x245     /* u32, the second FBI word               */
#define UDM_BUILDER        0x40u     /* UD_TYPEMASK bits                       */
#define UDM_AIRBASE        0x200u
#define UDM_CANFLY         0x800u
#define UDM_HIDEDAMAGE     0x4000u
#define UDM2_SHOWNAME      0x20000u  /* UD_TYPEMASK2: showplayername           */

static TAGPU_PK_UTYPE s_utype[TAGPU_LIM_TYPES];
static unsigned       s_utypeN;
/* THE LEVEL KEY is the publisher's own level counter, not a sentinel reset at
   load (the map features' rule): `s_utypeHave` with `s_utypeLevel ==
   s_levelGen` is "built for the level in play". tagpu_packet_pub_level_end
   clears it on the game thread as well, so neither the fill nor the hit guard
   can use one level's table in the next. */
static unsigned       s_utypeLevel;
static int            s_utypeHave;

static int utype_armed(const char* def)
{
    int k;
    for (k = 0; k < 3; k++) {
        const void* w = *(const void* const*)(def + UD_WEAPONS + k * 4);
        if (w && tagpu_limits_weapon_index(w) >= 1) return 1;
    }
    return 0;
}

static int utype_commander(const char* ta, const char* def, unsigned nsides)
{
    unsigned s;
    if (!def[UD_NAME]) return 0;
    for (s = 0; s < nsides; s++)
        if (!_strnicmp(def + UD_NAME, ta + OFF_SIDE_CMD + (size_t)s * SIDE_STRIDE, 0x20)) return 1;
    return (RDU32(def, UD_TYPEMASK2) & UDM2_SHOWNAME) && (RDU32(def, UD_TYPEMASK) & UDM_HIDEDAMAGE);
}

static int utype_class(const char* ta, const char* def, unsigned nsides)
{
    const unsigned m = RDU32(def, UD_TYPEMASK);
    const int builder = (m & UDM_BUILDER) != 0, airbase = (m & UDM_AIRBASE) != 0;
    if (utype_commander(ta, def, nsides)) return TAGPU_ICLS_COMMANDER;
    if (RDU8(def, UD_BMCODE) == 0) return builder && !airbase ? TAGPU_ICLS_FACTORY : TAGPU_ICLS_BUILDING;
    if (m & UDM_CANFLY)
        return builder ? TAGPU_ICLS_AIRCONS : utype_armed(def) ? TAGPU_ICLS_AIRCOMBAT : TAGPU_ICLS_AIR;
    if (builder && !airbase) return TAGPU_ICLS_CONS;
    return utype_armed(def) ? TAGPU_ICLS_COMBAT : TAGPU_ICLS_UNKNOWN;
}

/* BAR's drawOrder per class (icontypes.lua): structures under ground units
   under aircraft under commanders */
static const unsigned char UTYPE_LAYER[TAGPU_ICLS_N] = {
    /* UNKNOWN */ 1, /* COMMANDER */ 3, /* FACTORY */ 0, /* BUILDING */ 0,
    /* AIRCONS */ 2, /* AIRCOMBAT */ 2, /* AIR */ 2, /* CONS */ 1, /* COMBAT */ 1 };

/* ---- the icon art (tagpu_icontype.h) ----
   THE RADAR LOGO, `radlogo` (main+0x147DF, the sequence the minimap's dots
   are drawn from by 0x466DC0: frame PlayerInfo+0x96 through 0x4B7F30). It is
   SESSION art: 0x429870 loads it, whose one caller is start-up's 0x49134D,
   and the only writer of the pointer is 0x42991E in that load; 0x429935
   beside it stores `radlogohigh` at main+0x147E3 [DISASSEMBLED]. So the first in-play fill reads the session's frames, and
   they never change after. The team colour of a logo is its frame's texel at
   the hotspot -- MEASURED 2026-11-03 on the retail fx.GAF: ten 4x4 frames,
   hotspot (1,1), a 2x2 centre of the colour in a ring -- read from the frame,
   never from a table, because mods recolour it. */
#define OFF_RADLOGO        0x147DF   /* GAF sequence*: radlogo                */
#define ICONART_FRMAX      16        /* the largest logo frame kept, a side    */
/* THE LAYOUT is tagpu_iconart_pack's: the logo frames, then the mod set's
   pictures, left to right a texel apart -- the gap keeps a neighbour out of
   a filtered edge -- in rows from texel (1, 1). It refuses a layout that
   would leave the atlas, but the worst case cannot: every picture is at most
   ICONART_PICMAX square, so a picture advances a row by at most a cell of
   ICONART_PICMAX + 1 and a row holds at least ICONART_ROWCELLS of them (the
   next fits while 1 + cells x ICONART_CELL <= the width), and a row is at
   most a cell tall. The assert holds the rows the most pictures the build
   reads -- TAGPU_ICONART_LOGOS frames and TAGPU_ICONART_SETS pictures -- take
   against the atlas's height, so the logos with a full set always fit, and
   so do the logos alone the build falls back to. */
#define ICONART_PICMAX     (ICONART_FRMAX > TAGPU_PCX_MAXW ? ICONART_FRMAX : TAGPU_PCX_MAXW)
#define ICONART_CELL       (ICONART_PICMAX + 1)
#define ICONART_ROWCELLS   ((TAGPU_ICONART_W - 1) / ICONART_CELL)
#define ICONART_PICS       (TAGPU_ICONART_LOGOS + TAGPU_ICONART_SETS)
_Static_assert(TAGPU_PCX_MAXH <= TAGPU_PCX_MAXW && ICONART_FRMAX <= ICONART_PICMAX &&
               1 + ((ICONART_PICS + ICONART_ROWCELLS - 1) / ICONART_ROWCELLS) * ICONART_CELL
               <= TAGPU_ICONART_H, "every picture the build reads has a place in the atlas");

static unsigned char  s_iconPix[TAGPU_ICONART_W * TAGPU_ICONART_H];
static TAGPU_ICONART  s_iconArt;
static volatile LONG  s_iconArtReady;
/* the logo frames as decoded, before the packer places them: this thread's */
static unsigned char  s_logoPix[TAGPU_ICONART_LOGOS][ICONART_FRMAX * ICONART_FRMAX];
static unsigned char  s_logoCls[TAGPU_ICONART_LOGOS][ICONART_FRMAX * ICONART_FRMAX];

/* ---- a mod's own icon set (inc/tagpu_iconset.h) ----
   WHERE IT IS. TADR reads `[Preferences] MegaMapConfig` from its own ini,
   whose name is the string at 0x5098A3 in the running exe -- `totala.ini` as
   shipped, rewritten by the Community Patch Loader at start-up to the mod's
   (`mayhem.ini`, `ProTA.ini`, `taesc.ini`, `tazero.ini`), so by the first
   in-play fill it is the mod's. The value is a path relative to the game's
   folder, the iconcfg.ini; its folder holds the pictures. TADR resolves it
   against the current directory; the exe's folder is used here [INFERRED
   equal under every launcher in use: tacli and the loaders start TA in its
   folder], and an absolute path is refused, so only the install's own files
   are read.

   ONCE A PROCESS, on the game thread, at the first in-play fill, before the
   art's one release store: the files are read from the player's install and
   parsed by tagpu_iconset.c, the pictures packed into the art beside the
   logo frames, and nothing here is written again. Every file is bounded
   before it is read (an ini at ISET_TEXT_MAX, a picture at ISET_PCX_MAX), and
   everything in one is DATA to the parser. A set that is absent or does not
   load changes nothing: the art is the logo's alone and every type keeps its
   generated shape. TADR's code never runs. */
#define VA_TADR_INI        0x005098A3u  /* char[]: TADR's ini's file name     */
#define TADR_INI_CAP       33           /* the name's room read: 32 and a NUL */
#define OFF_SIDE_NAME      0x37F3D      /* each side's name, its record's +0  */
#define SIDE_NAME_CAP      0x1E
#define ISET_TEXT_MAX      0x10000u     /* an ini's bytes                     */
#define ISET_PCX_MAX       0x1000u      /* a picture's bytes                  */
#define ISET_PIX           (TAGPU_PCX_MAXW * TAGPU_PCX_MAXH)

/* the set as loaded, index = its picture's place in s_iconArt.set: this
   thread's, written before the art's release store and read after it only
   on this thread (the per-level walk) */
static TAGPU_ISENTRY  s_isEnt[TAGPU_ICONART_SETS];
static int            s_isN, s_isNone = -1, s_isUnknown = -1, s_isSelected;
static unsigned char  s_isPix[TAGPU_ICONART_SETS][ISET_PIX];
static unsigned char  s_isCls[TAGPU_ICONART_SETS][ISET_PIX];
static int            s_isW[TAGPU_ICONART_SETS], s_isH[TAGPU_ICONART_SETS];

/* The whole file at `path` into buf when it is 1..cap bytes; 0 otherwise. */
static int iconset_read(const char* path, void* buf, unsigned cap, unsigned* got)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER size;
    DWORD n = 0;
    int ok;
    *got = 0;
    if (f == INVALID_HANDLE_VALUE) return 0;
    ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart <= (LONGLONG)cap &&
         ReadFile(f, buf, (DWORD)size.QuadPart, &n, NULL) && n == (DWORD)size.QuadPart;
    CloseHandle(f);
    if (ok) *got = n;
    return ok;
}

/* `a\b` into out (MAX_PATH), or 0 when it does not fit */
static int iconset_join(char* out, const char* a, const char* b)
{
    const int k = _snprintf(out, MAX_PATH, "%s\\%s", a, b);
    if (k < 0 || k >= MAX_PATH) { out[0] = 0; return 0; }
    return 1;
}

/* the folder of `path`: cut at its last separator, either spelling */
static void iconset_folder(char* path)
{
    char* cut = NULL;
    char* p;
    for (p = path; *p; p++) if (*p == '\\' || *p == '/') cut = p;
    if (cut) *cut = 0;
}

static void iconset_load(const char* ta)
{
    static char           text[ISET_TEXT_MAX];
    static TAGPU_ICONCFG  cfg;
    static TAGPU_ISENTRY  plan[TAGPU_ICONSET_PLAN];
    static unsigned char  pcx[ISET_PCX_MAX];
    char dir[MAX_PATH], path[MAX_PATH], folder[MAX_PATH], val[TAGPU_INI_VALMAX], ini[TADR_INI_CAP];
    char skipped[256], b[512], side[TAGPU_ICONSET_SIDES][SIDE_NAME_CAP];
    const char* sides[TAGPU_ICONSET_SIDES];
    unsigned got, nsides = RDU32(ta, OFF_SIDES), s;
    int np, i, nskip = 0, sk = 0;
    DWORD dn;

    s_isN = 0; s_isNone = s_isUnknown = -1;
    skipped[0] = 0;
    if (!tagpu_iconset_ini_name_ok((const char*)VA_TADR_INI, TADR_INI_CAP)) {
        plog("icons: no mod set - the exe names no TADR ini");
        return;
    }
    memcpy(ini, (const char*)VA_TADR_INI, sizeof ini);
    ini[sizeof ini - 1] = 0;
    dn = GetModuleFileNameA(NULL, dir, sizeof dir);
    if (dn == 0 || dn >= sizeof dir) return;
    iconset_folder(dir);
    if (!iconset_join(path, dir, ini) || !iconset_read(path, text, sizeof text, &got) ||
        tagpu_ini_get(text, got, "Preferences", "MegaMapConfig", val, sizeof val) != 1 ||
        !tagpu_iconset_relpath_ok(val)) {
        _snprintf(b, sizeof b, "icons: no mod set - %s names no MegaMapConfig of the install's own", ini);
        b[sizeof b - 1] = 0; plog(b);
        return;
    }
    /* a managed mod's icon folder is in its package: the ini beside the exe names it relative to the mod */
    if (tagpu_modload_id()) {
        strncat(dir, "\\mods\\", sizeof dir - strlen(dir) - 1);
        strncat(dir, tagpu_modload_id(), sizeof dir - strlen(dir) - 1);
    }
    if (!iconset_join(path, dir, val) || !iconset_read(path, text, sizeof text, &got)) {
        _snprintf(b, sizeof b, "icons: no mod set - %s's MegaMapConfig %s does not read", ini, val);
        b[sizeof b - 1] = 0; plog(b);
        return;
    }
    tagpu_iconcfg_parse(text, got, &cfg);
    memcpy(folder, path, sizeof folder);
    iconset_folder(folder);
    /* the sides' names, for the per-side commanders' pictures: below both the
       count the loader stored and the five records the engine keeps, each
       copied only when tagpu_iconset_side_ok finds it a file name ending
       within its record's 0x1E bytes */
    if (nsides > TAGPU_ICONSET_SIDES) nsides = TAGPU_ICONSET_SIDES;
    for (s = 0; s < TAGPU_ICONSET_SIDES; s++) {
        const char* nm = ta + OFF_SIDE_NAME + (size_t)s * SIDE_STRIDE;
        sides[s] = NULL;
        if (s < nsides && tagpu_iconset_side_ok(nm, SIDE_NAME_CAP)) {
            memcpy(side[s], nm, SIDE_NAME_CAP);
            sides[s] = side[s];
        }
    }
    np = tagpu_iconset_plan(&cfg, sides, (int)nsides, plan, TAGPU_ICONSET_PLAN);
    for (i = 0; i < np; i++) {
        int w, h;
        const TAGPU_ISENTRY* e = &plan[i];
        if (s_isN < TAGPU_ICONART_SETS && iconset_join(path, folder, e->file) &&
            iconset_read(path, pcx, sizeof pcx, &got) &&
            tagpu_pcx_decode(pcx, got, s_isPix[s_isN], ISET_PIX, &w, &h)) {
            tagpu_iconset_classify(s_isPix[s_isN], w * h, cfg.transparent, cfg.fill, cfg.selected,
                                   s_isCls[s_isN]);
            s_isW[s_isN] = w; s_isH[s_isN] = h;
            s_isEnt[s_isN] = *e;
            if (e->kind == TAGPU_ISE_NONE) s_isNone = s_isN;           /* the last named, as TADR */
            if (e->kind == TAGPU_ISE_UNKNOWN) s_isUnknown = s_isN;
            s_isN++;
            continue;
        }
        nskip++;
        if (sk < (int)sizeof skipped - 2) {
            const int k = _snprintf(skipped + sk, sizeof skipped - (size_t)sk, " %s", e->file);
            sk = k < 0 ? (int)sizeof skipped : sk + k;
            skipped[sizeof skipped - 1] = 0;
        }
    }
    s_isSelected = cfg.selected;
    _snprintf(b, sizeof b, "icons: mod set %s from %s (%s), %s mode, %d picture(s) loaded of %d, "
              "%d [Icon] line(s) refused, skipped:%s", folder, ini, val, cfg.custom ? "custom" : "default",
              s_isN, np, cfg.refused, nskip ? skipped : " none");
    b[sizeof b - 1] = 0; plog(b);
}

/* a frame's team colour: its texel at the hotspot, or its commonest drawn
   texel when the hotspot draws nothing; -1 for a frame that draws nothing */
static int iconart_team(const unsigned char* pix, const unsigned char* cov, int w, int h, int hx, int hy)
{
    int count[256], k, best = -1;
    if (hx >= 0 && hy >= 0 && hx < w && hy < h && cov[hy * w + hx]) return pix[hy * w + hx];
    memset(count, 0, sizeof count);
    for (k = 0; k < w * h; k++) if (cov[k]) count[pix[k]]++;
    for (k = 0; k < 256; k++) if (count[k] && (best < 0 || count[k] > count[best])) best = k;
    return best;
}

static void iconart_build(const char* ta)
{
    static unsigned char cov[ICONART_FRMAX * ICONART_FRMAX];
    static TAGPU_ICONSRC src[ICONART_PICS];
    static TAGPU_ICONRECT rect[ICONART_PICS];
    const char* seq = *(const char* const*)(ta + OFF_RADLOGO);
    int marks[3] = { 0, 1, 2 }, i = 0, k, px, nfr = 0, nset, packed;
    char b[200];

    memset(&s_iconArt, 0, sizeof s_iconArt);
    if (ptr_ok(seq)) {
        nfr = tagpu_gaf_seq_nframes(seq);
        for (i = 0; i < nfr && i < TAGPU_ICONART_LOGOS; i++) {
            const unsigned char* g = tagpu_gaf_seq_frame(seq, i);
            unsigned char* pix = s_logoPix[i];
            TAGPU_GAFGEOM gm;
            int t;
            if (!g || !tagpu_gaf_frame_geom(g, &gm)) break;
            if (gm.w <= 0 || gm.h <= 0 || gm.w > ICONART_FRMAX || gm.h > ICONART_FRMAX) break;
            if (!tagpu_gaf_decode_cov(g, gm.w, gm.h, pix, cov)) break;
            /* a raw frame "covers" every texel (tagpu_gaf.h): its key is the
               blitter's test, so it is applied here */
            if (!*(const unsigned char*)(g + TAGPU_GF_COMP))
                for (px = 0; px < gm.w * gm.h; px++) if (pix[px] == gm.ck) cov[px] = 0;
            t = iconart_team(pix, cov, gm.w, gm.h, gm.hotx, gm.hoty);
            if (t < 0) break;
            for (px = 0; px < gm.w * gm.h; px++)
                s_logoCls[i][px] = cov[px] ? TAGPU_ITX_DRAWN : TAGPU_ITX_KEY;
            src[i].pix = pix; src[i].cls = s_logoCls[i]; src[i].w = gm.w; src[i].h = gm.h;
            s_iconArt.logo[i].hx = (int16_t)gm.hotx; s_iconArt.logo[i].hy = (int16_t)gm.hoty;
            s_iconArt.team[i] = (unsigned char)t;
        }
    }
    iconset_load(ta);
    for (k = 0; k < s_isN; k++) {
        src[i + k].pix = s_isPix[k]; src[i + k].cls = s_isCls[k];
        src[i + k].w = s_isW[k]; src[i + k].h = s_isH[k];
    }
    /* THE MARKERS are chosen over every picture the atlas holds
       (tagpu_iconart_pack): the smallest indices no drawn texel holds. A set
       that leaves fewer than three free is dropped and the logos are packed
       alone (the assert above rules out a set that does not fit). The retail
       art, ten 4x4 frames, draws at most 160 texels, so three are free for
       it; a mod's logo art can draw every index, and then there is no art. */
    nset = s_isN;
    packed = tagpu_iconart_pack(src, i + nset, s_iconPix, TAGPU_ICONART_W, TAGPU_ICONART_H, rect, marks);
    if (!packed && nset) {
        plog("icons: the mod set leaves no three free palette indices beside the radar logo - not drawn");
        nset = s_isN = 0; s_isNone = s_isUnknown = -1;
        packed = tagpu_iconart_pack(src, i, s_iconPix, TAGPU_ICONART_W, TAGPU_ICONART_H, rect, marks);
    }
    if (!packed) {
        memset(s_iconArt.logo, 0, sizeof s_iconArt.logo);
        memset(s_iconPix, 0, sizeof s_iconPix);
        i = 0; marks[0] = 0; marks[1] = 1; marks[2] = 2;
    }
    for (k = 0; k < i; k++) {
        s_iconArt.logo[k].x = rect[k].x; s_iconArt.logo[k].y = rect[k].y;
        s_iconArt.logo[k].w = rect[k].w; s_iconArt.logo[k].h = rect[k].h;
    }
    for (k = 0; k < nset; k++) s_iconArt.set[k] = rect[i + k];
    s_iconArt.pix = s_iconPix;
    s_iconArt.w = TAGPU_ICONART_W; s_iconArt.h = TAGPU_ICONART_H;
    s_iconArt.key = marks[0]; s_iconArt.fill = marks[1]; s_iconArt.sel = marks[2];
    s_iconArt.nlogo = i;
    s_iconArt.nset = nset;
    s_iconArt.none = nset ? s_isNone : -1;
    s_iconArt.unknown = nset ? s_isUnknown : -1;
    s_iconArt.selcol = nset ? s_isSelected : 0;
    /* THE PUBLICATION. Every byte of the atlas and of s_iconArt is stored above,
       on this thread, before this release store; nothing writes or frees either
       after it, for the life of the process. So a render-thread reader that
       sees the flag through tagpu_icontype_art's acquire load reads finished
       bytes, and a pointer it took stays valid and unchanging -- the order
       art's argument (tagpu_order.c). Not InterlockedExchange: mingw spells it
       __sync_lock_test_and_set, whose contract is acquire-only. */
    __atomic_store_n(&s_iconArtReady, 1, __ATOMIC_RELEASE);
    _snprintf(b, sizeof b, "icons: art %d radar logo frame(s) of %d, %d mod set picture(s), "
              "key %d fill %d sel %d", i, nfr, nset, s_iconArt.key, s_iconArt.fill, s_iconArt.sel);
    b[sizeof b - 1] = 0; plog(b);
}

const TAGPU_ICONART* tagpu_icontype_art(void)
{
    return __atomic_load_n(&s_iconArtReady, __ATOMIC_ACQUIRE) == 1 ? &s_iconArt : NULL;
}

/* ---- each type's picture of the mod's set, once a level ----
   THE ENGINE'S CATEGORIES. The map [*(u8**)0x51E6B4, *(u8**)0x51E6B8) holds
   8-byte entries {char* name, u32* mask}, kept sorted by _stricmp (0x4F8A70,
   an ASCII A-Z fold); a type is bit `row` of a mask -- 0x42D61A..0x42D653
   renumber the def's ID to its row after the name sort [DISASSEMBLED] -- and
   a mask is TAGPU_LIM_TYPES bits under Impure's raise, 512 stock
   (tagpu_patches.c's category mask sites, installed all or nothing: the
   process exits on a mismatch). A bit is read only for a row below the
   level's UNITINFOCount, which utype_level has already bounded by
   TAGPU_LIM_TYPES. The walk is linear and compares names itself, so it holds
   whatever order the map is in.

   WHY IT IS SAFE TO READ: the map and its masks are engine heap objects that
   only two things free: the teardown (0x488BF0, whose one caller is 0x491C3A
   in the teardown 0x491B60, game thread) and the process-exit destructor
   0x488A00, which the map's static initialiser registers at 0x4889D5 through
   0x4E4AC0 [INFERRED: the CRT's atexit] and which runs only once the process
   is leaving, after its last in-play frame; and, during play, only game-thread code
   inserts into (0x488C50 through 0x488FB0, which can move every entry: the
   Ctrl keys 0x41C338, 0x48BF38, 0x48D9AC and 0x48D9E5 (0x48DC39 has no
   caller), the SHARE give 0x49342D, and the console -- Reload 0x417490 ->
   0x42D1F0 -> 0x42BF40, and the AI plan's weight/limit 0x488D30 (0x488E34),
   which outside the load run only through it); this walk runs on the game
   thread inside the in-play publish, so neither can happen during it -- a
   lifetime by thread ownership. It keeps no entry pointer past this call:
   an insert moves the entries. The level's load filled the map on the loader thread (0x42BF40
   through 0x42CC40 and 0x488E70) before its last act sets main+0x38D75 bit 1
   (0x497C5F), which the game thread read before its first in-play frame
   (0x498342). The map's bounds and every entry's two pointers pass ptr_ok,
   a filter on values, not the argument. We NEVER call 0x488C50: on a miss it
   inserts, so a name the map does not hold is simply absent here.

   WHICH PICTURE: the first set entry, in load order, whose types hold the
   row (TADR's UnitPicture), else the set's UNKNOWN, else none -- the type
   keeps its generated shape (TADR would index its picture list at -1).
   What each kind holds is TADR's ExternQuickKey rule: a side's picture its
   commanders -- showplayername and hidedamage, decoys included -- whose
   Side is the side's name; COMMANDER every such commander; MOBILECOMBAT the
   types of CTRL_W that cannot fly; CONS those of CTRL_B when it names any,
   else the mobile builders that are no air base and no commander; FACTORY
   CTRL_F likewise, else the immobile builders; BUILDING a yard map and no
   commander; AIRCRAFTCOMBAT a flier with a weapon that is no stockpile, no
   builder, no yard map and no commander; AIRCONS a flying builder with no
   yard map and no commander; a category of the custom list, its types. A
   category with no type is the same as an absent one. */
#define VA_CATMAP_BEGIN    0x0051E6B4u  /* {char* name, u32* mask}*: the map  */
#define VA_CATMAP_END      0x0051E6B8u
#define CATMAP_MAX         65536u       /* entries the walk accepts           */
#define CAT_NAME_MAX       64           /* bytes of a name it compares        */
#define UD_SIDE            0xA0         /* char Side[], the FBI's `side`      */
#define UD_SIDE_CAP        0x1E         /* its room: the FBI reader stores it
                                           with a cap of 0x1E (`push 0x1e` at
                                           0x42ACB0, `lea edx,[ebp+0xa0]` at
                                           0x42ACB6) [DISASSEMBLED]           */
#define UD_YARDMAP         0x14E        /* yard map pointer; 0 = none         */
#define WTM_STOCKPILE      0x10000000u  /* W_MASK bit 28: a stockpile weapon  */

enum { ISC_CTRL_W, ISC_CTRL_B, ISC_CTRL_F, ISC_N };
static const char* const ISC_NAME[ISC_N] = { "CTRL_W", "CTRL_B", "CTRL_F" };

static int ascii_fold(int c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }

/* `name` (the engine's) equals `want` (ours, under CAT_NAME_MAX), folded as
   _stricmp folds. The engine's string is read only up to its NUL, or to the
   first byte that differs from ours. */
static int cat_name_eq(const char* name, const char* want, int cap)
{
    int k;
    for (k = 0; k < cap; k++) {
        const int a = ascii_fold((unsigned char)name[k]), b = ascii_fold((unsigned char)want[k]);
        if (a != b) return 0;
        if (!a) return 1;
    }
    return 0;
}

static int mask_bit(const unsigned* m, unsigned row)
{
    return m && ((m[row >> 5] >> (row & 31u)) & 1u);
}

/* TADR's commander: showplayername and hidedamage, decoys included */
static int iset_commander(const char* def)
{
    return (RDU32(def, UD_TYPEMASK2) & UDM2_SHOWNAME) && (RDU32(def, UD_TYPEMASK) & UDM_HIDEDAMAGE);
}

/* a weapon slot holding a record of the weapon array that is no stockpile */
static int iset_direct_weapon(const char* def)
{
    int k;
    for (k = 0; k < 3; k++) {
        const char* w = *(const char* const*)(def + UD_WEAPONS + k * 4);
        if (w && tagpu_limits_weapon_index(w) >= 1 && !(RDU32(w, W_MASK) & WTM_STOCKPILE)) return 1;
    }
    return 0;
}

static void iset_level(const char* udefs, unsigned n)
{
    static const unsigned* catMask[TAGPU_ICONART_SETS];
    static unsigned        catCount[TAGPU_ICONART_SETS];
    const unsigned* isc[ISC_N] = { NULL, NULL, NULL };
    unsigned iscCount[ISC_N] = { 0, 0, 0 };
    const char* begin = *(const char* const*)VA_CATMAP_BEGIN;
    const char* end = *(const char* const*)VA_CATMAP_END;
    unsigned entries = 0, row, picked = 0, unknown = 0;
    int e, k, len;
    char b[1024];

    if (!s_isN) return;
    memset(catMask, 0, sizeof catMask);
    memset(catCount, 0, sizeof catCount);
    /* the map's bounds: an empty map is two equal pointers, NULL before any
       insert; anything else that is not a whole number of entries between two
       plausible pointers is treated as no categories */
    if (begin != end) {
        if (ptr_ok(begin) && ptr_ok(end) && end > begin && (size_t)(end - begin) % 8 == 0 &&
            (size_t)(end - begin) / 8 <= CATMAP_MAX)
            entries = (unsigned)((size_t)(end - begin) / 8);
        else
            plog("icons: the category map's bounds are not a map - no categories this level");
    }
    for (k = 0; k < (int)entries; k++) {
        const char* name = *(const char* const*)(begin + (size_t)k * 8);
        const unsigned* mask = *(const unsigned* const*)(begin + (size_t)k * 8 + 4);
        if (!ptr_ok(name) || !ptr_ok(mask)) continue;
        for (e = 0; e < s_isN; e++)
            if (s_isEnt[e].kind == TAGPU_ISE_CATEGORY && !catMask[e] &&
                cat_name_eq(name, s_isEnt[e].name, CAT_NAME_MAX))
                catMask[e] = mask;
        for (e = 0; e < ISC_N; e++)
            if (!isc[e] && cat_name_eq(name, ISC_NAME[e], CAT_NAME_MAX)) isc[e] = mask;
    }
    for (row = 0; row < n; row++) {
        for (e = 0; e < s_isN; e++) catCount[e] += (unsigned)mask_bit(catMask[e], row);
        for (e = 0; e < ISC_N; e++) iscCount[e] += (unsigned)mask_bit(isc[e], row);
    }
    for (row = 1; row < n; row++) {
        const char* def = udefs + (size_t)row * UDEF_STRIDE;
        const unsigned m = RDU32(def, UD_TYPEMASK);
        const int builder = (m & UDM_BUILDER) != 0, airbase = (m & UDM_AIRBASE) != 0;
        const int canfly = (m & UDM_CANFLY) != 0, mobile = RDU8(def, UD_BMCODE) != 0;
        const int yard = *(const void* const*)(def + UD_YARDMAP) != NULL;
        const int cmd = iset_commander(def);
        int pick = -1;
        for (e = 0; e < s_isN && pick < 0; e++) {
            const TAGPU_ISENTRY* en = &s_isEnt[e];
            int holds = 0;
            switch (en->kind) {
            case TAGPU_ISE_SIDE:
                holds = cmd && cat_name_eq(def + UD_SIDE, en->name, UD_SIDE_CAP);
                break;
            case TAGPU_ISE_CATEGORY:
                holds = mask_bit(catMask[e], row);
                break;
            case TAGPU_ISE_DEFAULT:
                switch (en->arg) {
                case TAGPU_ISD_COMMANDER:    holds = cmd; break;
                case TAGPU_ISD_AIRWEAPON:    holds = canfly && !cmd && !builder && !yard &&
                                                     iset_direct_weapon(def); break;
                case TAGPU_ISD_AIRCON:       holds = canfly && builder && !yard && !cmd; break;
                case TAGPU_ISD_CONS:         holds = iscCount[ISC_CTRL_B] ? mask_bit(isc[ISC_CTRL_B], row)
                                                   : builder && !airbase && mobile && !cmd; break;
                case TAGPU_ISD_MOBILEWEAPON: holds = !canfly && mask_bit(isc[ISC_CTRL_W], row); break;
                case TAGPU_ISD_FACTORY:      holds = iscCount[ISC_CTRL_F] ? mask_bit(isc[ISC_CTRL_F], row)
                                                   : builder && !airbase && !mobile && !cmd; break;
                case TAGPU_ISD_BUILDING:     holds = yard && !cmd; break;
                default: break;
                }
                break;
            default:                     /* NONE, UNKNOWN, NUKEICON hold no type */
                break;
            }
            if (holds) pick = e;
        }
        if (pick < 0 && s_isUnknown >= 0) { pick = s_isUnknown; unknown++; }
        else if (pick >= 0) picked++;
        if (pick >= 0) s_utype[row].icon = (unsigned char)pick;
    }
    len = _snprintf(b, sizeof b, "icons: mod set by type - %u map entr(ies), %u of %u type(s) by a picture, "
                    "%u UNKNOWN; CTRL_W=%u CTRL_B=%u CTRL_F=%u;", entries, picked, n - 1, unknown,
                    iscCount[ISC_CTRL_W], iscCount[ISC_CTRL_B], iscCount[ISC_CTRL_F]);
    for (e = 0; e < s_isN && len > 0 && len < (int)sizeof b; e++) {
        int k2;
        if (s_isEnt[e].kind != TAGPU_ISE_CATEGORY) continue;
        k2 = catMask[e] ? _snprintf(b + len, sizeof b - (size_t)len, " %s=%u", s_isEnt[e].name, catCount[e])
                        : _snprintf(b + len, sizeof b - (size_t)len, " %s=absent", s_isEnt[e].name);
        len = k2 < 0 ? (int)sizeof b : len + k2;
    }
    b[sizeof b - 1] = 0; plog(b);
}

/* The table for the level in play, built at its first in-play fill; a no-op
   once built for it. A level with more types than the table holds has none,
   and every unit of it is drawn as its model. */
static void utype_level(const char* ta)
{
    const char* udefs = *(const char* const*)(ta + OFF_UNITDEFS);
    unsigned n = RDU32(ta, OFF_UDEFCOUNT), nsides = RDU32(ta, OFF_SIDES), row;
    /* the art first, once per process: it is the session's, not the level's */
    if (!s_iconArtReady) iconart_build(ta);
    if (s_utypeHave && s_utypeLevel == s_levelGen) return;
    if (!ptr_ok(udefs)) return;                     /* no defs yet: the next fill */
    s_utypeLevel = s_levelGen; s_utypeHave = 1; s_utypeN = 0;
    if (n < 1 || n > (unsigned)TAGPU_LIM_TYPES) {
        char b[160];
        _snprintf(b, sizeof b, "icons: %u unit types, past the table's %u - no icons this level",
                  n, (unsigned)TAGPU_LIM_TYPES);
        b[sizeof b - 1] = 0; plog(b);
        return;
    }
    if (nsides > SIDE_ROOM) nsides = SIDE_ROOM;
    memset(s_utype, 0, (size_t)n * sizeof s_utype[0]);
    for (row = 0; row < n; row++) {
        const char* def = udefs + (size_t)row * UDEF_STRIDE;
        TAGPU_PK_UTYPE* t = &s_utype[row];
        unsigned r;
        /* row 0 is the engine's "no model": it stays UNKNOWN with no radius */
        t->cls   = (unsigned char)(row ? utype_class(ta, def, nsides) : TAGPU_ICLS_UNKNOWN);
        t->layer = UTYPE_LAYER[t->cls];
        t->icon  = TAGPU_ICON_NOSET;       /* iset_level fills it, with a set */
        /* unit+0xA6 is both the model id and the type row, so the hit guard's
           radius per model id is this row's model radius */
        r = row ? tagpu_zoom_hit_radius(ta, row) : 0u;
        t->r_px = (unsigned short)(r > 0xFFFFu ? 0xFFFFu : r);
    }
    /* each type's picture of the mod's set, when one is loaded */
    iset_level(udefs, n);
    s_utypeN = n;
}

static void utype_level_end(void)
{
    s_utypeHave = 0; s_utypeN = 0;
}

const TAGPU_PK_UTYPE* tagpu_icontype_table(unsigned* n)
{
    const int built = s_utypeHave && s_utypeLevel == s_levelGen && s_utypeN;
    *n = built ? s_utypeN : 0u;
    return built ? s_utype : NULL;
}

/* The four tables, into the slot the producer holds. Returns the byte count
   the fill NEEDED — past cap_bytes means something did not fit, and the
   primitive grows the write slot and fills it again before publishing
   (pkx_publish; fill_frame turns this into the whole fill's size). */
static unsigned fill_world(TAGPU_PACKET* p, const char* ta, unsigned* cursor)
{
    const char* beg   = *(const char* const*)(ta + OFF_UNIT_BEGIN);
    const char* uend  = *(const char* const*)(ta + OFF_UNIT_END);
    const char* udefs = *(const char* const*)(ta + OFF_UNITDEFS);
    const char* fmap  = *(const char* const*)(ta + OFF_FEATMAP);
    const char* fdefs = *(const char* const*)(ta + OFF_FEATDEF);
    const char* recs  = *(const char* const*)(ta + OFF_WRECKS);
    unsigned udefCount = p->udef_count;
    unsigned slots = p->unit_slots;
    int mapW = p->map_w16, mapH = p->map_h16;
    unsigned nu = 0, nw = 0, na = 0, pk = 0, i, dup = 0, walk;
    /* what the arena WOULD have taken if everything fitted. `pk` stops at the
       first run that did not, so growing on it would add one unit's worth per
       publish and take as many publishes as there are units to converge; this
       counts the whole demand, so one growth is enough. */
    unsigned pkWant = 0;
    unsigned pieces_base = PKT_ALIGN4(*cursor);   /* after the fog source and the shade table */
    unsigned need = pieces_base, e;
    unsigned pTrunc = 0;
    int c0, r0, cols, rows, row, col;
    int inL, inT, inR, inB;
    int midRow;

    /* the icons' type table for this level, before the tables it rides after */
    utype_level(ta);
    p->feat_defcount = RD32(ta, OFF_FEATCOUNT);
    p->sweep_cols  = RD32(ta, OFF_SWEEP_C);
    p->sweep_rows  = RD32(ta, OFF_SWEEP_R);
    if (p->feat_defcount < 0 || p->feat_defcount > 4096) p->feat_defcount = 0;

    /* THE TABLES COVER THE REACH: every world point a frame drawn from this
       packet can gather in, the header's own `reach_lo/hi` (table_reach) --
       the very numbers the render thread's reach bound clamps the drawn eye by.
       So a slab the bound accepts cannot hold a unit carried outside the reach
       nor ask for an anchor row the table was not scanned over. */
    inL = p->reach_lo[0]; inR = p->reach_hi[0];
    inT = p->reach_lo[1]; inB = p->reach_hi[1];
    anchor_rect(p, inL, inR, inT, inB, &c0, &r0, &cols, &rows);
    /* the anchor scan's starting row: the centre of the view about the
       packet's eye, which a frame is drawn from plus the steps the game
       thread has not applied yet (clamped into the rect below) */
    midRow = (p->eye[1] + p->vp[3] / 2) >> 4;
    p->anch_c0 = c0; p->anch_r0 = r0; p->anch_cols = cols; p->anch_rows = rows;

    /* ---- the units, and their pieces ---- */
    walk = slots;
    s_slotWalk = 0;
    if (ptr_ok(beg) && slots > 1) {
        if (walk > TAGPU_PK_MAX_UNITS) { walk = TAGPU_PK_MAX_UNITS; s_cUnitTrunc++; p->truncated |= TAGPU_PK_TRUNC_UNITS; }
        /* THE RELATION, ASSERTED RATHER THAN RELIED ON. `end` is stored once in
           the binary, as begin + (count-1)*stride (0x4855D6); the walk below
           uses the COUNT and never the pointer, so a disagreement cannot
           mis-bound anything — it is recorded because it would mean one of the
           three fields is not what the note says it is. */
        if (uend != beg + (size_t)(slots - 1u) * UNIT_STRIDE) s_cRelBad++;
        for (i = 1; i < walk; i++) {
            const char* u = beg + (size_t)i * UNIT_STRIDE;
            unsigned st = RDU32(u, U_STATE);
            TAGPU_PK_UNIT* ue;
            const char* o3;
            s_slotIdx[i] = 0xFFFFu;
            if (!(st & ST_ALIVE) || (st & ST_EXCLUDED)) continue;
            ue = &s_uScratch[nu];
            fill_unit(ue, ta, u, i, udefCount, udefs);
            /* the recorded selections this slot is in, as the replay's frame visit left them
               before this draw (fill_overlay below states the ordering) */
            ue->rsel = (uint16_t)tagpu_replay_rsel(i);
            /* the ground height under the anchor: the shadow's height, and the
               one feature-grid read a unit needs.

               IT IS THE ENGINE'S BILINEAR BLEND, NOT THE NEAREST CELL, and the
               difference is the shadow's whole vertical accuracy on a slope.
               The engine takes this height from `GetPosHeight 0x485070`, which
               `0x459252` calls immediately before it blits the shadow; that
               function splits the position into tile (`sar 4`) and sub-tile
               fraction (`and 0xF`) on both axes and blends the 2x2
               neighbourhood, with each of its three steps dividing by 16
               TOWARD ZERO (`cdq / and edx,0xf / add / sar 4`, which is what C's
               int `/` does). Publishing the nearest cell instead would put the
               silhouette and the slant up to half a tile's height out on any
               sloped ground, and make `gy == ay` -- the claim the unit pass
               rests its zero shift on -- true only on the flat.

               THE BOUND IS THE ENGINE'S OWN, one cell stricter than a plain
               index check because the blend reads `tx+1` and `ty+1`. Outside
               it the engine returns -1 and its caller draws the shadow at a
               nonsense height; we fall back to the nearest cell, which is a
               height rather than a sentinel. */
            {
                int wx = (int)(short)(ue->pos[0] >> 16);
                int wy = (int)(short)(ue->pos[2] >> 16);
                int tx = wx >> 4, ty = wy >> 4;
                if (ptr_ok(fmap) && tx >= 0 && ty >= 0 && tx < mapW && ty < mapH) {
                    const unsigned char* h00 =
                        (const unsigned char*)(fmap + ((size_t)ty * mapW + tx) * FT_STRIDE + FT_HEIGHT);
                    if (tx + 1 < mapW && ty + 1 < mapH) {
                        int fx = wx & 0xF, fz = wy & 0xF;
                        int a  = h00[0],            b = h00[FT_STRIDE];
                        int c  = h00[(size_t)mapW * FT_STRIDE],
                            d  = h00[(size_t)mapW * FT_STRIDE + FT_STRIDE];
                        int r0 = a + (b - a) * fx / 16;
                        int r1 = c + (d - c) * fx / 16;
                        int hv = r0 + (r1 - r0) * fz / 16;
                        ue->ground_h = (unsigned char)(hv < 0 ? 0 : hv > 255 ? 255 : hv);
                    } else {
                        ue->ground_h = h00[0];
                    }
                    ue->flags |= TAGPU_PK_U_GROUND;
                }
                /* in the reach, tested on the pass's own projection
                   (tagpu_native.c: `wy - wz / 2` with the altitude as a
                   short), so the two agree exactly and no bound on a unit's
                   altitude is needed */
                {
                    int wz = (int)(short)(ue->pos[1] >> 16);
                    int py = wy - wz / 2;
                    if (wx >= inL && wx <= inR && py >= inT && py <= inB)
                        ue->flags |= TAGPU_PK_U_INRECT;
                }
            }
            /* the cargo links, as SLOTS for now: the packet indices they become
               are not all known until the walk ends */
            {
                const char* c = *(const char* const*)(u + U_CARGO);
                const char* n = *(const char* const*)(u + U_CARGONEXT);
                ue->cargo_first = slot_of(beg, c, slots);
                ue->cargo_next  = slot_of(beg, n, slots);
            }
            o3 = *(const char* const*)(u + U_OBJ3DO);
            if ((ue->flags & TAGPU_PK_U_INRECT) && ue->nparts && ptr_ok(o3)) {
                unsigned at = pieces_base + pk * (unsigned)sizeof(TAGPU_PK_PIECE);
                unsigned got = fill_pieces(p, o3, ue->nparts, at, &pTrunc);
                if (got) { ue->piece_off = at; ue->piece_n = (unsigned short)got; pk += got; }
                pkWant += ue->nparts;
            }
            s_slotIdx[i] = (unsigned short)nu;
            /* THE STABLE-ID COLLISION ORACLE, over THIS packet. Two live units
               with one id would make the pose blend match the wrong pair across
               two packets, silently; the gate is that this reads 0. The bitmap
               is cleared only over the ids this packet used, so it costs the
               unit count and not 8 KB a frame. */
            {
                unsigned id = ue->id;
                if (s_idSeen[id >> 3] & (unsigned char)(1u << (id & 7u))) dup++;
                else s_idSeen[id >> 3] |= (unsigned char)(1u << (id & 7u));
                s_idUsed[nu] = (unsigned short)id;
            }
            if (++nu >= TAGPU_PK_MAX_UNITS) { s_cUnitTrunc++; p->truncated |= TAGPU_PK_TRUNC_UNITS; break; }
        }
        /* a walk cut by the table's cap wrote slots 1..i, a whole one 1..walk-1 */
        s_slotWalk = i < walk ? i + 1u : walk;
        /* slots -> packet indices, now that every slot the walk visited has one */
        for (i = 0; i < nu; i++) {
            int s1 = s_uScratch[i].cargo_first, s2 = s_uScratch[i].cargo_next;
            s_uScratch[i].cargo_first = (s1 >= 0 && (unsigned)s1 < walk && s_slotIdx[s1] != 0xFFFFu)
                                        ? (short)s_slotIdx[s1] : (short)-1;
            s_uScratch[i].cargo_next  = (s2 >= 0 && (unsigned)s2 < walk && s_slotIdx[s2] != 0xFFFFu)
                                        ? (short)s_slotIdx[s2] : (short)-1;
        }
        /* the oracle's bitmap, cleared only where it was set */
        for (i = 0; i < nu; i++) s_idSeen[s_idUsed[i] >> 3] = 0;
    }

    /* ---- the feature anchors: scanned once per tick per rect ---- */
    if (ptr_ok(fmap) && mapW > 0 && mapH > 0 && cols > 0 && rows > 0) {
        int rr = rows < PK_ANCH_MAXROWS ? rows : PK_ANCH_MAXROWS;
        int mid = midRow < r0 ? r0 : midRow >= r0 + rr ? r0 + rr - 1 : midRow;
        if (s_aHave && s_aTick == p->tick &&
            s_aRect[0] == c0 && s_aRect[1] == r0 &&
            s_aRect[2] == cols && s_aRect[3] == rows && s_aMid == mid) {
            na = s_aN;
            if (s_aTrunc) p->truncated |= TAGPU_PK_TRUNC_ANCHORS;
            if (!s_fillAgain) s_cAnchReuse++;
        } else {
            /* ROWS OUTWARD FROM THE VIEW'S CENTRE ROW, WHOLE ROWS ONLY. The
               rect is the tables' reach plus a margin, and the table holds
               TAGPU_PK_MAX_ANCHORS; scanned from its top, an overflow would
               drop the rows at the bottom of the screen. The rows are taken
               in the order `mid, mid+1, mid-1, mid+2, mid-2, ...`, and the
               scan stops at the first row that does not fit in the room the
               table has left: that row is rolled back, and it and every row
               after it in that order are dropped — the row at the same
               distance on the other side among them. What a truncation keeps
               is therefore a band `[mid - a, mid + b]`, b - a in {0, 1}.
               `mid` is the centre row of the view about the packet's eye,
               clamped into the rect. A frame is drawn from that eye plus the
               steps the game thread has not applied yet, as far as the reach
               bound lets them through — at most the lead (vh/4, 33 rows at
               4K), since the reach is published about the packet's eye. A
               frame loses
               nothing while the band kept holds its own slab, placed at that
               offset from `mid` — 560 rows at 0.25 at 4K, the whole map at a
               floor that fits it; the densest stock map puts 12 633 anchors
               in the WHOLE rect. Put back in row
               order afterwards: the depth keys come from each anchor's own
               row and column, and the order is what the table has always
               promised. A truncation sets TAGPU_PK_TRUNC_ANCHORS, once per
               scan, which the feature pass counts per frame (`trunc=` on its
               line). */
            int k, cut = rr < rows;
            unsigned out = 0;
            for (k = 0; k < rr; k++) s_aRowN[k] = 0;
            for (k = 0; ; k++) {
                int off = (k & 1) ? (k + 1) / 2 : -(k / 2);
                unsigned start = na;
                const char* trow;
                row = mid + off;
                if (k > 2 * rr) break;
                if (row < r0 || row >= r0 + rr) continue;
                trow = fmap + ((size_t)row * mapW) * FT_STRIDE;
                for (col = c0; col < c0 + cols; col++) {
                    const char* t = trow + (size_t)col * FT_STRIDE;
                    unsigned d = RDU16(t, FT_DEFIDX);
                    TAGPU_PK_ANCHOR* a;
                    if (d >= 0xFFFBu) continue;
                    if (na >= TAGPU_PK_MAX_ANCHORS) break;
                    a = &s_aScan[na++];
                    a->col = (unsigned short)col; a->row = (unsigned short)row;
                    a->def = (unsigned short)d;
                    a->wreck = RDU16(t, FT_WIDX);
                    a->flags = RDU8(t, FT_FLAGS);
                    a->h   = RDU8(t, FT_HEIGHT);
                    a->hr  = (col + 1 < mapW) ? RDU8(t + FT_STRIDE, FT_HEIGHT) : a->h;
                    a->hl  = (col > 0)        ? RDU8(t - FT_STRIDE, FT_HEIGHT) : a->h;
                    a->hu  = (row > 0)        ? RDU8(t - (size_t)mapW * FT_STRIDE, FT_HEIGHT) : a->h;
                    if (row + 1 < mapH) {
                        const char* t2 = t + (size_t)mapW * FT_STRIDE;
                        a->hd  = RDU8(t2, FT_HEIGHT);
                        a->hrd = (col + 1 < mapW) ? RDU8(t2 + FT_STRIDE, FT_HEIGHT) : a->hd;
                    } else { a->hd = a->h; a->hrd = a->hr; }
                    a->pad = 0;
                }
                if (col < c0 + cols) {      /* this row did not fit: drop it whole */
                    na = start;
                    cut = 1;
                    break;
                }
                s_aRowAt[row - r0] = start; s_aRowN[row - r0] = na - start;
            }
            if (cut) { s_cAnchTrunc++; p->truncated |= TAGPU_PK_TRUNC_ANCHORS; }
            for (k = 0; k < rr; k++) {
                if (!s_aRowN[k]) continue;
                memcpy(&s_aScratch[out], &s_aScan[s_aRowAt[k]], s_aRowN[k] * sizeof(TAGPU_PK_ANCHOR));
                out += s_aRowN[k];
            }
            s_aHave = 1; s_aTick = p->tick; s_aN = na;
            /* A TRUNCATED SCAN IS CACHED AS TRUNCATED. Without this every reuse
               publish of the same tick would report a complete anchor table,
               and the feature pass's `trunc=`, which must read 0, is the one
               number that says the rect overflowed the table: it would be
               under-reported by exactly the reuse rate, which is 6 in 7. */
            s_aTrunc = (p->truncated & TAGPU_PK_TRUNC_ANCHORS) ? 1 : 0;
            s_aRect[0] = c0; s_aRect[1] = r0; s_aRect[2] = cols; s_aRect[3] = rows;
            s_aMid = mid;
            s_cAnchScan++;
        }
        /* ---- the 3D husks the anchors name, from THIS packet's arena ----
           The engine draws a husk through a scratch fake unit, so it is posed
           exactly as a unit and carries the same fields. A GAF wreck
           (FeatureMask bit0) is the feature pass's, not this table's. */
        for (i = 0; i < na && ptr_ok(recs) && ptr_ok(fdefs); i++) {
            const TAGPU_PK_ANCHOR* a = &s_aScratch[i];
            unsigned d = a->def;
            const char* rec;
            const char* o3;
            if (!(a->flags & 1u)) continue;
            if (!p->feat_defcount || (int)d >= p->feat_defcount) continue;
            if (RDU8(fdefs + (size_t)d * FD_STRIDE, FD_MASK) & 1u) continue;
            /* THE BOUND ON THE RECORD INDEX, and it has to be here. The cell's
               u16 is engine DATA until the engine's own count says otherwise:
               unbounded it addresses up to 65535*0x30 ~ 3 MB past the pool, and
               a garbage "Object3do" that survives ptr_ok would put up to 256
               rows of nonsense `node` pointers into the packet — dereferenced
               later on the RENDER thread. The engine's own draw path does not
               bound it, but it only forms the address for the cells it is
               drawing; this walk covers the tables' reach plus a margin,
               which is most of a screen of cells the engine never touches.
               WR_COUNT is the pool the level allocates (tagpu_engine.h), and
               also the value its allocators hand back for "none". */
            if (a->wreck >= WR_COUNT) { s_cWreckOob++; continue; }
            rec = recs + (size_t)a->wreck * WR_STRIDE;
            o3 = *(const char* const*)(rec + WR_OBJ3DO);
            if (nw >= TAGPU_PK_MAX_WRECKS) { s_cWreckTrunc++; p->truncated |= TAGPU_PK_TRUNC_WRECKS; break; }
            if (!ptr_ok(o3)) continue;
            {
                TAGPU_PK_WRECK* we = &s_wScratch[nw++];
                unsigned np = RDU16(o3, O3_NUMPARTS), k;
                memset(we, 0, sizeof *we);
                we->pos[0] = RD32(rec, WR_XPOS);
                we->pos[1] = RD32(rec, WR_ZPOS);
                we->pos[2] = RD32(rec, WR_YPOS);
                we->o3_key = (unsigned)(size_t)o3;
                we->rec = a->wreck; we->def = (unsigned short)d;
                we->col = a->col; we->row = a->row;
                we->base_piece = 0xFFFFu;
                if (np && np <= TAGPU_PK_MAXPIECE) we->nparts = (unsigned short)np;
                for (k = 0; k < 3; k++) we->bturn[k] = RDU16(o3, O3_BTURN + k * 2);
                {
                    const char* bp = *(const char* const*)(o3 + O3_BASEPRIM);
                    const char* p0 = o3 + O3_PRIM0;
                    if (bp >= p0 && we->nparts) {
                        size_t dd = (size_t)(bp - p0);
                        if (dd % PRIM_STRIDE == 0 && dd / PRIM_STRIDE < we->nparts)
                            we->base_piece = (unsigned short)(dd / PRIM_STRIDE);
                    }
                }
                if (we->nparts) {
                    unsigned at = pieces_base + pk * (unsigned)sizeof(TAGPU_PK_PIECE);
                    unsigned got = fill_pieces(p, o3, we->nparts, at, &pTrunc);
                    if (got) { we->piece_off = at; we->piece_n = (unsigned short)got; pk += got; }
                    pkWant += we->nparts;
                }
            }
        }
    }
    s_cAnchCells = (unsigned)(cols > 0 && rows > 0 ? cols * rows : 0);
    if (pTrunc) { s_cPieceTrunc++; p->truncated |= TAGPU_PK_TRUNC_PIECES; }

    /* the arena is done: name it, then the three tables after it */
    p->off_pieces = pk ? pieces_base : 0;
    p->n_pieces   = pk;
    *cursor = pieces_base + pk * (unsigned)sizeof(TAGPU_PK_PIECE);
    if (*cursor > p->cap_bytes) *cursor = pieces_base;      /* nothing fitted   */
    if (need < *cursor) need = *cursor;
    {
        unsigned want = pieces_base + pkWant * (unsigned)sizeof(TAGPU_PK_PIECE);
        if (want > need) need = want;
        s_fillShort += PKT_ALIGN4((pkWant - pk) * (unsigned)sizeof(TAGPU_PK_PIECE));
    }

    e = append_table(p, cursor, s_uScratch, nu, (unsigned)sizeof(TAGPU_PK_UNIT),
                     &p->off_units, &p->n_units, TAGPU_PK_TRUNC_UNITS);
    if (e > need) need = e;
    e = append_table(p, cursor, s_utype, s_utypeN, (unsigned)sizeof(TAGPU_PK_UTYPE),
                     &p->utype_off, &p->utype_n, TAGPU_PK_TRUNC_UTYPE);
    if (e > need) need = e;
    e = append_table(p, cursor, s_wScratch, nw, (unsigned)sizeof(TAGPU_PK_WRECK),
                     &p->off_wrecks, &p->n_wrecks, TAGPU_PK_TRUNC_WRECKS);
    if (e > need) need = e;
    e = append_table(p, cursor, s_aScratch, na, (unsigned)sizeof(TAGPU_PK_ANCHOR),
                     &p->off_anchors, &p->n_anchors, TAGPU_PK_TRUNC_ANCHORS);
    if (e > need) need = e;
    /* a table that did not fit leaves its runs dangling: drop the pieces with
       the units rather than leave an offset nothing indexes */
    if (!p->n_units && !p->n_wrecks) { p->n_pieces = 0; p->off_pieces = 0; }
    p->unit_dup = dup;
    s_cUnitDup += dup;
    s_cLastUnits = nu; s_cLastPieces = pk; s_cLastWrecks = nw; s_cLastAnchors = na;
    return need;
}


/* ====================== THE EFFECTS TABLES ===============================
   The four per-frame effect arrays, on the thread that owns them. On the
   RENDER thread the particle heap is a client the level fence cannot cover:
   the layer table is per game, but each layer's {begin,end} pair and every
   object's sub-particle vector are std::vectors the game thread GROWS
   mid-play, freeing the old array (0x4732E0), so the pair could be read skewed
   and a consistent pair could name memory just freed
   (cross-thread-engine-reads.md §5). Reading them here is not a narrower
   window; it is the same thread doing both.

   THE GATHER IS TAKEN ONCE PER SIM TICK, AND THE ARGUMENT FOR THAT IS NOT THE
   ONE THE ANCHOR SCAN USES. The anchor grid really is constant within a tick,
   because only the tick writes it. These four are not: **the engine's own
   explosion DRAW emits particles** — 0x420B00's debris loop calls 0x421550
   (0x420B18), which calls the grey-smoke emitter 0x472810 (0x421583) and the
   fire emitter 0x472AB0 (0x4215AA), and both append to a layer. So a second
   draw of one tick can find a layer the first did not
   produce.

   What actually licenses the cache is weaker and is enough:

     * THE TABLES ARE COPIES. An append after the gather cannot dangle anything
       in a packet, because nothing in a packet points into engine memory except
       the asset addresses whose lifetimes are stated separately below. That is
       the safety half, and it does not depend on the arrays being constant.
     * THE POSITIONS ARE THE TICK'S. The sim moves every projectile, particle
       and debris piece once per tick, so what a reused table gets WRONG is
       confined to objects created between two publishes of one tick — never a
       stale position for an object that was already there.
     * WHAT THAT COSTS is that the newest smoke or fire of a tick can land one
       publish late: under 4 ms of wall time at the publish rates the reference
       setup reaches, on a sprite that is one frame old. It is a quality trade, taken
       deliberately, and the alternative is the whole gather at the DRAW rate —
       measured at 6 to 7 % of the game thread.

   At the 250-odd publishes a second the reference setup reaches against a
   60 Hz sim, four publishes in five reuse.

   EVERY ENGINE POINTER IS RESOLVED HERE. A sprite's GAF frame is looked up
   through the sequence the particle or the anim state names, while the object
   that names it is live on this thread; a weapon's colour NUMBER goes through
   main+0xDCB here. Every 3DO model an effect draws is posed and copied here
   (`fx_model`), so no model crosses at all.

   WHAT STILL CROSSES AS AN ADDRESS, and on whose lifetime:

     * a model FACE's GAF frame -- a 3DO texture, which the render thread
       paints into the unit atlas (tagpu_render3do.c) exactly as it paints the
       ones the unit bake resolves out of the templates, on the same fence.
     * a PROJECTILE's or a PARTICLE's GAF frame — from the "fx" bank, which
       0x429870 loads once per process (its only caller chain is 0x49134D <-
       0x491200 <- WinMain's 0x49EA62), so a SESSION asset.
     * an EXPLOSION's GAF frame, `frame` and `flash` — **per LEVEL**, and this
       is not the same answer.
       The add site takes the sequence from main+0x1AB8F[idx] (0x420AA2), a
       table 0x420620 builds from the level load (0x4919D2) and 0x420960 frees
       and nulls from the teardown (0x491B9F). So these two stand on
       tagpu_reclaim's fence exactly as the templates do, and a consumer cache
       keyed on such a frame's address must drop at a level boundary —
       tagpu_fx.c's atlas does, on the packet's level generation.

   Only tagpu_gaf.c ever dereferences any of the GAF frames. */

static TAGPU_PK_PROJ   s_pScratch[TAGPU_PK_MAX_PROJ];
static TAGPU_PK_EXPL   s_eScratch[TAGPU_PK_MAX_EXPL];
static TAGPU_PK_DEBRIS s_dScratch[TAGPU_PK_MAX_DEBRIS];
static TAGPU_PK_PART   s_partScratch[TAGPU_PK_MAX_PART];
static unsigned s_nProj, s_nExpl, s_nDebris, s_nPart;
static unsigned s_partN[TAGPU_PK_NLAYER], s_partObj[TAGPU_PK_NLAYER];
/* THE WORLD RECT THE MODELS' CULL KEEPS: the tables' reach (table_reach) of the
   packet being filled -- every world point a frame drawn from it can gather
   in, x across and `y - altitude / 2` down, the space the effects pass
   projects into (fx_in_reach) */
typedef struct { int l, r, t, b; } FXREACH;
static unsigned s_fxTick, s_fxGen, s_fxLevel, s_fxWant;
static FXREACH  s_fxReach;
static int      s_fxHave, s_fxPartTrunc;
static volatile unsigned s_cFxScan, s_cFxReuse, s_cPartTrunc, s_cPartMax, s_cLayerBad, s_cSubBad;
#define PART_SUBCAP 4096u   /* sub-particles per object: a containment filter */
static volatile unsigned s_cLastProj, s_cLastExpl, s_cLastDebris, s_cLastPart;

/* the LHT lighten table, latched exactly like the shade table */
static unsigned char s_lht[TAGPU_PK_LHT_BYTES];
static const unsigned char* s_lhtPtr;
static int s_lhtOk;
static volatile unsigned s_cLhtCopies;

typedef char pk_proj_size  [(sizeof(TAGPU_PK_PROJ)   == 44) ? 1 : -1];
typedef char pk_expl_size  [(sizeof(TAGPU_PK_EXPL)   == 24) ? 1 : -1];
typedef char pk_debris_size[(sizeof(TAGPU_PK_DEBRIS) == 16) ? 1 : -1];
typedef char pk_fxvert_size [(sizeof(TAGPU_PK_FXVERT)  == 12) ? 1 : -1];
typedef char pk_fxface_size [(sizeof(TAGPU_PK_FXFACE)  == 12) ? 1 : -1];
typedef char pk_fxshape_size[(sizeof(TAGPU_PK_FXSHAPE) ==  8) ? 1 : -1];
typedef char pk_fxmodel_size[(sizeof(TAGPU_PK_FXMODEL) ==  8) ? 1 : -1];
typedef char pk_part_size  [(sizeof(TAGPU_PK_PART)   == 16) ? 1 : -1];

static void lht_snapshot(void)
{
    const char* g = *(const char* const*)TA_GFX_PP;
    const unsigned char* t;
    if (!ptr_ok(g)) return;
    if (!(RDU16(g, PROG_CAPS) & 0x80u)) return;        /* not built yet */
    t = *(const unsigned char* const*)(g + PROG_LHT);
    if (!ptr_ok(t)) return;
    if (t == s_lhtPtr && s_lhtOk) return;
    /* THE BOUND IS THE FORMAT, as the shade table's is: the flash blit indexes
       [row][idx] with a 5-bit row and a byte, so the table is exactly 32 x 256
       and a copy of that size reads what the blit reads and nothing more. */
    tagpu_pk_copy(s_lht, t, sizeof s_lht);
    s_lhtPtr = t; s_lhtOk = 1; s_cLhtCopies++;
}

/* the particle class, by vtable — the engine's own dispatch, as a row number */
static int part_kind(unsigned vt)
{
    switch (vt) {
    case VT_SMOKE1: return (int)TAGPU_PK_PK_SMOKE1;
    case VT_SMOKE2: return (int)TAGPU_PK_PK_SMOKE2;
    case VT_FIRE:   return (int)TAGPU_PK_PK_FIRE;
    case VT_FLARE:  return (int)TAGPU_PK_PK_FLARE;
    case VT_WAKE:   return (int)TAGPU_PK_PK_WAKE;
    case VT_NANO:   return (int)TAGPU_PK_PK_NANO;
    default:        return -1;                 /* VT_BASE and anything else */
    }
}

/* each class's sub-particle record, from the engine's own draw leaves:
   stride, the 16.16 triple's offset, the frame index's (sprites) or the
   colour byte's (dots), and whether the engine LOS-gates the class */
static const struct { short stride, pos, frame, col; char sprite; }
PART_FMT[TAGPU_PK_NPARTKIND] = {
    { 0x20, 4, 0x14,    0, 1 },   /* smoke 1 */
    { 0x20, 4, 0x14,    0, 1 },   /* smoke 2 */
    { 0x3C, 4, 0x2C,    0, 1 },   /* fire    */
    { 0x34, 4, 0x2C,    0, 1 },   /* flare   */
    { 0x44, 4,    0, 0x30, 0 },   /* wake    */
    { 0x30, 0,    0, 0x28, 0 }    /* nano    */
};

/* One layer's object vector, [*b, *b + n): 0 when the layer is empty, and
   `*bad` when it is larger than the engine's own rule allows. */
static unsigned layer_objs(const char* layers, int L, const char* const** b, int* bad)
{
    const char* lay = layers + (size_t)L * LAYER_STRIDE;
    const char* const* e = *(const char* const* const*)(lay + LAYER_END);
    unsigned n;
    *b = *(const char* const* const*)(lay + LAYER_BEGIN);
    *bad = 0;
    if (!ptr_ok(*b) || !ptr_ok(e) || e <= *b) return 0;
    /* THE BOUND IS THE ENGINE'S OWN RULE, not a probe, and the rule is the cap
       plus ONE. Every emitter reads the layer's size and `cmp eax,cap / jbe
       append` (0x472071 and nineteen more across 0x471183..0x472CD9; 0x472BF2
       compares ecx), the cap being TAGPU_LIM_SFX, which the limits write into
       all twenty: at the cap or fewer it appends, and past that it destroys the
       FRONT object, shifts the vector down by one and appends anyway. So a
       layer at cap + 1 is the engine's steady state, and a walk that stopped at
       the cap would drop the whole layer every time it filled — measured
       2026-09-12 at stock's 400: that bound refused 86 layers in one fx-mix
       run. The pair itself is sound because this thread is the one that runs
       those emitters; a count past cap + 1 is a fact worth counting, not a walk
       worth attempting. */
    n = (unsigned)(e - *b);
    if (n > (unsigned)TAGPU_LIM_SFX + 1u) { *bad = 1; return 0; }
    return n;
}

/* One object's drawable sub-particles: their count, 0 when the object draws
   nothing, with its class and vector. `*bad` when the count fails the filter. */
static unsigned obj_subs(const char* o, int* kind, const char** sb, int* bad)
{
    const char* se;
    unsigned ns;
    *bad = 0;
    if (!ptr_ok(o)) return 0;
    *kind = part_kind(*(const unsigned*)o);
    if (*kind < 0) return 0;                   /* the base class draws nothing */
    *sb = *(const char* const*)(o + PO_SUB0);
    se = *(const char* const*)(o + PO_SUB1);
    if (!ptr_ok(*sb) || !ptr_ok(se) || se <= *sb) return 0;
    ns = (unsigned)(se - *sb) / (unsigned)PART_FMT[*kind].stride;
    /* A SANITY FILTER ON A VALUE, not the safety argument — the argument is
       that this thread is the one that grows these vectors. What it buys is
       containment: without it one object with a wild `end` fills the whole
       table and truncates every layer after it, and with it that object is
       skipped and the frame is otherwise complete. */
    if (ns > PART_SUBCAP) { *bad = 1; return 0; }
    return ns;
}

/* Every layer's drawable sub-particles, counted by the walk's own rules. */
static unsigned count_subs(const char* layers)
{
    unsigned total = 0, n, i;
    const char* const* b;
    const char* sb;
    int L, k, bad;
    for (L = 0; L < (int)TAGPU_PK_NLAYER; L++) {
        n = layer_objs(layers, L, &b, &bad);
        for (i = 0; i < n; i++) total += obj_subs(b[i], &k, &sb, &bad);
    }
    return total;
}

/* THINNED, NOT TRUNCATED. The layers are walked bottom to top, so a table
   that simply filled would lose the TOP layers whole — the trails and smoke
   the engine draws last, layer 9 among them. When the frame holds more than
   the table, every layer keeps the same share instead: the sub-particles are
   counted first, and a 16.16 accumulator keeps `s_partKeep` of each run,
   which by construction totals at most TAGPU_PK_MAX_PART. The table's own
   check stays as the bound; the share is only what makes it rare. */
static unsigned s_partKeep, s_partAcc;
static volatile unsigned s_cPartThin;

/* one layer's objects into the particle table */
static void gather_layer(const char* layers, int L)
{
    const char* const* b;
    unsigned n, i;
    int bad;
    n = layer_objs(layers, L, &b, &bad);
    if (bad) { s_cLayerBad++; return; }
    s_partObj[L] = n;
    for (i = 0; i < n; i++) {
        int k, sub;
        unsigned ns, j;
        const char* sb;
        ns = obj_subs(b[i], &k, &sb, &bad);
        if (bad) s_cSubBad++;
        if (!ns) continue;
        sub = PART_FMT[k].sprite;
        for (j = 0; j < ns; j++) {
            const char* q = sb + (size_t)j * (size_t)PART_FMT[k].stride;
            TAGPU_PK_PART* pe;
            int X, A, Y;
            if (s_partKeep < 0x10000u) {
                s_partAcc += s_partKeep;
                if (s_partAcc < 0x10000u) continue;
                s_partAcc -= 0x10000u;
            }
            if (s_nPart >= TAGPU_PK_MAX_PART) { s_fxPartTrunc = 1; return; }
            X = *(const int*)(q + PART_FMT[k].pos);
            A = *(const int*)(q + PART_FMT[k].pos + 4);
            Y = *(const int*)(q + PART_FMT[k].pos + 8);
            pe = &s_partScratch[s_nPart];
            pe->x  = X >> 16;
            pe->zp = (Y >> 16) - ((A >> 16) >> 1);
            pe->layer = (unsigned char)L;
            pe->kind  = (unsigned char)k;
            pe->col   = 0;
            pe->pad   = 0;
            if (sub) {
                /* the sequence the sub-particle names, indexed by its own
                   frame number: resolved HERE, where the object is live */
                pe->frame = (unsigned)(size_t)tagpu_gaf_seq_frame(
                    *(const char* const*)q, *(const int*)(q + PART_FMT[k].frame));
                if (!pe->frame) continue;          /* nothing to draw: drop it */
            } else {
                pe->frame = 0;
                pe->col = *(const unsigned char*)(q + PART_FMT[k].col);
            }
            s_nPart++; s_partN[L]++;
        }
    }
}

/* THE TEAM COLOUR OF A DEBRIS PIECE, as 0x4211D0 takes it: the piece's unit
   (`sys+0`), that unit's owning player (`+0x96`), the player's PlayerInfo
   (`+0x27`) and its logo colour (`+0x96`) -- `0x42132F..0x421346`,
   DISASSEMBLED. The engine walks the chain unchecked; here each link is
   validated as DATA against the table it must belong to before the next one
   is read. The unit must be a stride-aligned member of the unit array inside
   the slot count (`slot_of`), and the player pointer must BE one of the ten
   PlayerStructs -- an address checked against the engine's own table. The
   PlayerInfo is the game's own allocation, which 0x45861C reads for every
   team-coloured face of every unit on every frame; the byte taken out of it
   is bounded where it is used, against the sequence's frame count (0x4B7F30's
   own test). The unit is usually DEAD by now -- a piece flies off a destroyed
   unit -- and its slot may already hold another: the engine reads whatever
   the slot holds at this moment, and so does this. -1 when a link does not
   validate, and then a team-coloured face draws nothing. */
static int debris_logo(const char* ta, const char* u)
{
    const char* beg = *(const char* const*)(ta + OFF_UNIT_BEGIN);
    const char* players = ta + OFF_PLAYERS;
    const char* pl;
    const char* pi;
    size_t d;
    if (!ptr_ok(beg) || slot_of(beg, u, RDU16(ta, OFF_UNITSLOTS)) < 0) return -1;
    pl = *(const char* const*)(u + U_PLAYER);
    if (pl < players) return -1;
    d = (size_t)(pl - players);
    if (d % PLAYER_STRIDE || d / PLAYER_STRIDE >= PLAYER_COUNT) return -1;
    pi = *(const char* const*)(pl + PL_INFO);
    if (!ptr_ok(pi)) return -1;
    return *(const unsigned char*)(pi + PI_LOGO);
}

/* ---- THE EFFECTS MODELS (tagpu_packet.h) ---------------------------------
   Five tables, grown on this thread and reset with the rest of the gather.
   A model is taken from the very record the engine is about to draw, on the
   thread that draws it, so what it copies is what the engine would draw:
   there is no lifetime to argue, only values to bound. */
static TAGPU_PK_FXMODEL* s_fmScratch; static unsigned s_fmCap, s_nFm;
static TAGPU_PK_FXSHAPE* s_fsScratch; static unsigned s_fsCap, s_nFs;
static TAGPU_PK_FXFACE*  s_ffScratch; static unsigned s_ffCap, s_nFf;
static uint16_t*         s_fiScratch; static unsigned s_fiCap, s_nFi;
static TAGPU_PK_FXVERT*  s_fvScratch; static unsigned s_fvCap, s_nFv;
static volatile unsigned s_cLastFm, s_cLastFv, s_cFxLost;
/* a model the engine draws and the packet cannot carry: any index past
   `n_fxmodel` says that to the consumer (tagpu_packet.h) */
#define FX_LOST 0xFFFFFFFEu

/* THE FIVE TABLES' BOUND, checked before a table grows: each at the acquire's
   own TAGPU_PK_TABLE_MAX rows, and their bytes together at one slot's
   reserve. A model that would take a table past either is not carried -- FX_LOST, and its
   record is taken back whole -- so no table this file lands is one the
   acquire refuses. What fits is appended last of the fill (fill_fxmodels),
   where a slot too small for the five cuts them together and nothing else. */
static int fx_fits(unsigned dv, unsigned di, unsigned df, unsigned ds, unsigned dm)
{
    const unsigned long long bytes =
        (unsigned long long)(s_nFv + dv) * sizeof(TAGPU_PK_FXVERT) +
        (unsigned long long)(s_nFi + di) * sizeof(uint16_t) +
        (unsigned long long)(s_nFf + df) * sizeof(TAGPU_PK_FXFACE) +
        (unsigned long long)(s_nFs + ds) * sizeof(TAGPU_PK_FXSHAPE) +
        (unsigned long long)(s_nFm + dm) * sizeof(TAGPU_PK_FXMODEL);
    return s_nFv + dv <= TAGPU_PK_TABLE_MAX && s_nFi + di <= TAGPU_PK_TABLE_MAX &&
           s_nFf + df <= TAGPU_PK_TABLE_MAX && s_nFs + ds <= TAGPU_PK_TABLE_MAX &&
           s_nFm + dm <= TAGPU_PK_TABLE_MAX && bytes <= TAGPU_PK_RESERVE;
}

/* grows one table's scratch to `need` rows, which fx_fits has bounded */
static int fx_room(void** p, unsigned* cap, unsigned need, size_t elem)
{
    unsigned n;
    void* q;
    if (need <= *cap) return 1;
    n = *cap ? *cap : 256u;
    while (n < need) n *= 2u;
    q = realloc(*p, (size_t)n * elem);
    if (!q) return 0;
    *p = q; *cap = n;
    return 1;
}

/* THE SHAPES OF THIS GATHER, by (node, logo): a weapon's model is one node
   for every projectile that fires it, so two hundred rockets are one shape.
   Probed linearly and stamped with the gather's generation rather than
   cleared; a key that finds no slot within the probe gets a shape of its own,
   which costs bytes and never a wrong face. */
#define FX_SHAPE_HASH  4096u
#define FX_SHAPE_PROBE 16u
static struct { const char* node; int logo; unsigned shape, gen; } s_shHash[FX_SHAPE_HASH];
static unsigned s_shGen;

/* A textured face's frame, BY THE DRAW'S OWN RULE (DISASSEMBLED):

     0x46BAE0 (a weapon's body and flame, an explosion's body)
        bit 1 clear   the frame pointer at +0x10
        bit 1 set     0x4B7EE0(face+0x10): the anim state {u16 frame @0,
                      sequence @8}; 0 for a NULL sequence (0x4B7EE6..
                      0x4B7EEB), else the sequence's +0x28 + 8 * frame --
                      UNBOUNDED in the engine
     0x4211D0 (a debris piece) the same, except
        bits 1 and 2  0x4B7F30(*(face+0x18), logo): the sequence indexed by
                      the logo colour of the unit the piece flew off, which
                      answers 0 for an index at or past the sequence's count

   THREE ANSWERS, because a model is carried whole or not at all. A frame
   pointer: the face draws it. FXF_NONE: the engine draws NOTHING for the
   face -- 0x4C7580 returns at once on a NULL frame (0x4C7597), which is what
   a NULL pointer, 0x4B7EE0's NULL sequence and 0x4B7F30's out-of-range answer
   hand it -- so leaving the face out is the engine's picture. FXF_REFUSE: the
   engine draws something no copy can reproduce -- 0x4B7EE0 reading past its
   table, a logo reached through a chain that did not validate, a pointer
   that is not a sane frame header, or an RLE-compressed frame, whose pixel
   plane the span 0x4C7310 copies as raw rows -- and the whole model is
   refused (fx_shape). A NULL sequence on a team face is a refusal too:
   0x4B7F30 reads its count before its NULL test (0x4B7F41, 0x4B7F48), so
   there the engine faults rather than draws nothing. */
#define FXF_NONE   0u
#define FXF_REFUSE 1u
static unsigned fx_face_frame(const char* fa, int debris, int logo)
{
    const unsigned flags = RDU32(fa, F_FLAGS);
    const char* seq = *(const char* const*)(fa + F_TEXSEQ);
    const void* raw;
    const unsigned char* f;
    if (!(flags & FF_ANIM)) {
        raw = *(const void* const*)(fa + F_TEXFRAME);
    } else if (debris && (flags & FF_TEAM)) {
        int r;
        if (logo < 0) return FXF_REFUSE;
        r = tagpu_gaf_seq_entry(seq, logo, &raw);
        if (r < 0) return FXF_REFUSE;
        if (r == 0) return FXF_NONE;                  /* 0x4B7F30's out-of-range 0 */
    } else {
        if (!seq) return FXF_NONE;                    /* 0x4B7EE0's NULL test */
        if (tagpu_gaf_seq_entry(seq, (int)RDU16(fa, F_TEXFRAME), &raw) != 1) return FXF_REFUSE;
    }
    if (!raw) return FXF_NONE;
    f = tagpu_gaf_frame_sane(raw);
    if (!f || f[TAGPU_GF_COMP] != 0) return FXF_REFUSE;
    return (unsigned)(size_t)f;
}

/* A node's faces as the draw walks them (0x46BB76..0x46BC4E, 0x4212A3..
   0x42138D, DISASSEMBLED): face 0 is skipped when the node carries a
   selection primitive; a face with flag bit 0 is a flat fill (0x4C0310 ->
   0x4C0330) in the colour word's low byte, whatever its vertex count; any
   other face is drawn only with exactly four vertices, textured by
   GAF_DrawTransformed 0x4C7580.

   A SHAPE IS WHOLE OR REFUSED. A face the engine draws nothing for is left
   out, and that is its picture: a flat fill of fewer than three vertices has
   no span, a textured face of another count is never handed to the
   rasteriser, and a face whose frame is FXF_NONE is returned on. A face the
   engine DOES draw and this cannot carry refuses the whole shape rather than
   leaving a hole in it: a flat face past TAGPU_PK_FXMAXFV, a frame that is
   FXF_REFUSE, an index pointer that is not one, or an index at or past the
   node's vertex count (the engine takes that vertex out of its projected
   scratch unchecked -- another model's, or a stale one). A node with no
   vertex is walked the same way: 0x46BAE0 skips the rotation on a count of
   0 or less (`jle` at 0x46BB09) and still walks the faces, so every face it
   paints reads stale points, and refuses.

   Returns the shape; FXS_EMPTY for a node the engine paints no face of -- no
   face, or none its walk draws -- whose record's other parts draw as they
   would; FXS_REFUSED for a node that did not validate or a shape refused
   above. Both answers are remembered for the gather, as a shape is. */
#define FXS_EMPTY   (-2)
#define FXS_REFUSED (-1)
#define FXS_HASH_EMPTY   0xFFFFFFFEu
#define FXS_HASH_REFUSED 0xFFFFFFFFu
static void fx_shape_remember(unsigned slot, const char* node, int key, unsigned shape)
{
    if (slot >= FX_SHAPE_HASH) return;
    s_shHash[slot].node = node; s_shHash[slot].logo = key;
    s_shHash[slot].shape = shape; s_shHash[slot].gen = s_shGen;
}

static int fx_shape(const char* node, int debris, int logo)
{
    const int key = debris ? logo : -2;
    const unsigned h0 = ((unsigned)(size_t)node * 2654435761u ^ (unsigned)key) & (FX_SHAPE_HASH - 1u);
    unsigned k, slot = FX_SHAPE_HASH, f0, i0;
    int nvert, nface, j, j0, t;
    const char* faces;
    TAGPU_PK_FXSHAPE* sh;
    for (k = 0; k < FX_SHAPE_PROBE; k++) {
        const unsigned i = (h0 + k) & (FX_SHAPE_HASH - 1u);
        if (s_shHash[i].gen != s_shGen) { slot = i; break; }
        if (s_shHash[i].node == node && s_shHash[i].logo == key) {
            const unsigned sv = s_shHash[i].shape;
            return sv == FXS_HASH_EMPTY ? FXS_EMPTY : sv == FXS_HASH_REFUSED ? FXS_REFUSED : (int)sv;
        }
    }
    nvert = RD32(node, N_VCOUNT);
    nface = RD32(node, N_FCOUNT);
    faces = *(const char* const*)(node + N_FACES);
    if (nface < 1) { fx_shape_remember(slot, node, key, FXS_HASH_EMPTY); return FXS_EMPTY; }
    if (nvert < 0) nvert = 0;          /* the rotation's `jle`: no vertex at all */
    if (nvert > (int)TAGPU_PK_FXMAXNV || nface > (int)TAGPU_PK_FXMAXNF ||
        !ptr_ok(faces) ||
        !fx_fits(0, (unsigned)nface * TAGPU_PK_FXMAXFV, (unsigned)nface, 1u, 0) ||
        !fx_room((void**)&s_fsScratch, &s_fsCap, s_nFs + 1u, sizeof *s_fsScratch) ||
        !fx_room((void**)&s_ffScratch, &s_ffCap, s_nFf + (unsigned)nface, sizeof *s_ffScratch) ||
        !fx_room((void**)&s_fiScratch, &s_fiCap, s_nFi + (unsigned)nface * TAGPU_PK_FXMAXFV,
                 sizeof *s_fiScratch)) {
        fx_shape_remember(slot, node, key, FXS_HASH_REFUSED);
        return FXS_REFUSED;
    }
    sh = &s_fsScratch[s_nFs];
    sh->face = s_nFf; sh->nface = 0; sh->nvert = (uint16_t)nvert;
    f0 = s_nFf; i0 = s_nFi;
    j0 = RD32(node, N_SELPRIM) != -1 ? 1 : 0;
    for (j = j0; j < nface; j++) {
        const char* fa = faces + (size_t)j * FACE_STRIDE;
        const int fvc = RD32(fa, F_VCOUNT);
        const uint16_t* ip = *(const uint16_t* const*)(fa + F_INDICES);
        TAGPU_PK_FXFACE* f = &s_ffScratch[s_nFf];
        int ok = 1;
        memset(f, 0, sizeof *f);
        if (RDU32(fa, F_FLAGS) & FF_FLAT) {
            if (fvc < 3) continue;                         /* no span: nothing drawn */
            if (fvc > (int)TAGPU_PK_FXMAXFV) goto refuse;
            f->flat = 1;
            f->colour = (uint8_t)RDU32(fa, F_COLORTAB);
        } else {
            if (fvc != 4) continue;                        /* never rasterised */
            f->frame = fx_face_frame(fa, debris, logo);
            if (f->frame == FXF_NONE) continue;
            if (f->frame == FXF_REFUSE) goto refuse;
        }
        if (!ptr_ok(ip)) goto refuse;
        for (t = 0; t < fvc; t++) {
            const uint16_t v = ip[t];
            if ((int)v >= nvert) { ok = 0; break; }
            s_fiScratch[s_nFi + (unsigned)t] = v;
        }
        if (!ok) goto refuse;
        f->idx = s_nFi; f->n = (uint8_t)fvc;
        s_nFi += (unsigned)fvc;
        s_nFf++;
        sh->nface++;
    }
    if (sh->nface == 0) { fx_shape_remember(slot, node, key, FXS_HASH_EMPTY); return FXS_EMPTY; }
    fx_shape_remember(slot, node, key, s_nFs);
    return (int)s_nFs++;

refuse:
    s_nFf = f0; s_nFi = i0;
    fx_shape_remember(slot, node, key, FXS_HASH_REFUSED);
    return FXS_REFUSED;
}

/* THE NODE'S REACH FROM ITS ORIGIN, in whole pixels, by node for the gather
   (the shapes' generation): every rest vertex's |x| + |y| + |z|, the L1 norm,
   is at least its distance from the origin, and a rotation keeps that
   distance, so no posed vertex lies farther out whatever the triple. -1 when
   the node has no vertex or they cannot be read: no bound, so nothing is
   culled by it. */
static struct { const char* node; int rad; unsigned gen; } s_rdHash[FX_SHAPE_HASH];
static int fx_rad(const char* node)
{
    const unsigned h0 = ((unsigned)(size_t)node * 2654435761u) & (FX_SHAPE_HASH - 1u);
    unsigned k, slot = FX_SHAPE_HASH;
    int nvert, t, rad = -1;
    const int* vb;
    for (k = 0; k < FX_SHAPE_PROBE; k++) {
        const unsigned i = (h0 + k) & (FX_SHAPE_HASH - 1u);
        if (s_rdHash[i].gen != s_shGen) { slot = i; break; }
        if (s_rdHash[i].node == node) return s_rdHash[i].rad;
    }
    nvert = RD32(node, N_VCOUNT);
    vb = *(const int* const*)(node + N_VERTS);
    if (nvert >= 1 && nvert <= (int)TAGPU_PK_FXMAXNV && ptr_ok(vb)) {
        long long m = 0;
        for (t = 0; t < nvert; t++) {
            const long long d = llabs((long long)vb[t * 3]) + llabs((long long)vb[t * 3 + 1]) +
                                llabs((long long)vb[t * 3 + 2]);
            if (d > m) m = d;
        }
        rad = (int)(m >> 16) + 1;
    }
    if (slot < FX_SHAPE_HASH) {
        s_rdHash[slot].node = node; s_rdHash[slot].rad = rad; s_rdHash[slot].gen = s_shGen;
    }
    return rad;
}

/* WHETHER ANY PIXEL OF A MODEL CAN BE ON A FRAME DRAWN FROM THIS PACKET. The
   model's anchor projects to (X, Y - ALT / 2) as the effects pass projects
   it; a posed vertex is at most `rad` from the origin, which puts its pixel
   at most rad + 1 across and 1.5 rad + 3 down from the anchor's (its y and
   half its altitude, each floored), and a span reaches one pixel past its
   last vertex. `2 rad + 4` holds all of it on both axes, so a model this
   refuses has no pixel inside the reach, and no frame drawn from the packet
   could have shown one. The engine culls explosions and debris by the anchor
   before it poses them (0x4B6720 at 0x420C4A and 0x42123F) and poses every
   model projectile it draws wherever it is (0x49BE60 calls 0x46BAE0 with no
   rect test, DISASSEMBLED), so it is the projectiles off the reach whose
   rotation this saves. */
static int fx_in_reach(const int32_t pos[3], int rad, const FXREACH* rc)
{
    const int hx = pos[0] >> 16, halt = pos[1] >> 16;
    const int pz = (pos[2] >> 16) - (halt >> 1);
    const int m = 2 * rad + 4;
    return hx + m >= rc->l && hx - m <= rc->r && pz + m >= rc->t && pz - m <= rc->b;
}

/* ONE POSED MODEL: every vertex of the node through the engine's own
   rotation (ROTATE3_VA, called here as 0x46BB1A and 0x4215EE call it), then
   offset by the effect's 16.16 position the way the draw offsets it before
   its shifts: rotated x + X, rotated y + altitude, Y - rotated z. This is the
   engine's arithmetic on the engine's thread, so it is the engine's answer
   bit for bit -- the x87 precision it runs at is this thread's own. The eye
   is NOT taken off here: the consumer projects with the eye it draws with,
   and `(v + X - (eye << 16)) >> 16` is `((v + X) >> 16) - eye` exactly.

   THE REACH IS TESTED FIRST, because it needs only the node's rest
   vertices: a model no frame can show is TAGPU_PK_NOMODEL whatever its faces
   would answer, so its record's other parts draw, and a face it could not
   have shown never takes them back with it.

   `debris` selects 0x4211D0's frame rule and `logo` is its team index (-1
   when the chain did not validate). Returns the model; TAGPU_PK_NOMODEL for a
   node the engine draws nothing for, or one with no pixel inside the reach;
   FX_LOST for one the packet cannot carry. */
static unsigned fx_model(const char* node, const short turn[3], const int32_t pos[3],
                         int debris, int logo, const FXREACH* rc)
{
    typedef void (__stdcall *ROTATE3)(const int* src, int* dst, const short* turn);
    const ROTATE3 rot = (ROTATE3)(size_t)ROTATE3_VA;
    const int* vb;
    int shape, nvert, rad, i;
    if (!ptr_ok(node)) { s_cFxLost++; return FX_LOST; }
    rad = fx_rad(node);
    if (rad >= 0 && !fx_in_reach(pos, rad, rc)) return TAGPU_PK_NOMODEL;
    shape = fx_shape(node, debris, logo);
    if (shape == FXS_EMPTY) return TAGPU_PK_NOMODEL;
    if (shape < 0) { s_cFxLost++; return FX_LOST; }
    vb = *(const int* const*)(node + N_VERTS);
    nvert = s_fsScratch[shape].nvert;
    if (!ptr_ok(vb) || !fx_fits((unsigned)nvert, 0, 0, 0, 1u) ||
        !fx_room((void**)&s_fmScratch, &s_fmCap, s_nFm + 1u, sizeof *s_fmScratch) ||
        !fx_room((void**)&s_fvScratch, &s_fvCap, s_nFv + (unsigned)nvert, sizeof *s_fvScratch))
        { s_cFxLost++; return FX_LOST; }
    for (i = 0; i < nvert; i++) {
        int r[3];
        TAGPU_PK_FXVERT* o = &s_fvScratch[s_nFv + (unsigned)i];
        rot(vb + i * 3, r, turn);
        o->x   = (int32_t)((uint32_t)r[0] + (uint32_t)pos[0]);
        o->alt = (int32_t)((uint32_t)r[1] + (uint32_t)pos[1]);
        o->y   = (int32_t)((uint32_t)pos[2] - (uint32_t)r[2]);
    }
    s_fmScratch[s_nFm].vert = s_nFv;
    s_fmScratch[s_nFm].shape = (uint32_t)shape;
    s_nFv += (unsigned)nvert;
    return s_nFm++;
}

static void fx_models_reset(void)
{
    s_nFm = s_nFs = s_nFf = s_nFi = s_nFv = 0;
    if (++s_shGen == 0) {
        memset(s_shHash, 0, sizeof s_shHash);
        memset(s_rdHash, 0, sizeof s_rdHash);
        s_shGen = 1;
    }
}

/* the projectiles, the debris slots and the explosions */
static void gather_effects(const char* ta, int tick, const unsigned char* coltab,
                           const FXREACH* rc)
{
    const char* pbase = *(const char* const*)(ta + OFF_PROJ);
    const char* xpool = tagpu_limits_expl_pool(ta);
    const void* const* ps;
    int np = RD32(ta, OFF_NPROJ);
    int ne = RD32(xpool, EXPL_NCOUNT);
    int i;

    /* ---- projectiles (0x49BE60) ---------------------------------------
       The array is per game: 0x499A30 allocates exactly PROJ_COUNT slots
       (tagpu_limits.h raises the allocation and the ten caps together, at
       attach, before any game) and 0x499A80 frees AND NULLS the base inside
       the teardown cascade, so a NULL base is the refusal and the walk's
       bound is the allocation. */
    if (ptr_ok(pbase) && np > 0) {
        if (np > PROJ_COUNT) np = PROJ_COUNT;
        for (i = 0; i < np; i++) {
            const char* q = pbase + (size_t)i * PROJ_STRIDE;
            const char* w;
            TAGPU_PK_PROJ* pe;
            int rt, color, color2;
            if (*(const short*)(q + PJ_HIDDEN) != 0) continue;
            w = *(const char* const*)(q + PJ_WEAPON);
            if (!ptr_ok(w)) continue;
            rt = *(const signed char*)(w + W_RT);
            if (rt < 0 || rt > 7) continue;
            color  = *(const unsigned char*)(w + W_COLOR);
            color2 = *(const unsigned char*)(w + W_COLOR2);
            if (s_nProj >= TAGPU_PK_MAX_PROJ) break;      /* np <= PROJ_COUNT already */
            pe = &s_pScratch[s_nProj++];
            tagpu_pk_fill(pe, 0, (unsigned)sizeof *pe);
            pe->pos[0]   = *(const int*)(q + PJ_X);
            pe->pos[1]   = *(const int*)(q + PJ_ALT);
            pe->pos[2]   = *(const int*)(q + PJ_Y);
            pe->start[0] = *(const int*)(q + PJ_XS);
            pe->start[1] = *(const int*)(q + PJ_ALTS);
            pe->start[2] = *(const int*)(q + PJ_YS);
            pe->rt   = (unsigned char)rt;
            /* THE COLOUR LUT IS 256 BYTES (0x4AC7D0), so a colour number is
               inside it whatever the FBI said; the engine indexes it with the
               same byte and no check at all. */
            pe->col  = coltab[color];
            pe->col2 = coltab[color2];
            if (color2) pe->flags |= TAGPU_PK_FX_COL2;
            pe->model = pe->cmodel = TAGPU_PK_NOMODEL;
            if (rt == 1 || rt == 3 || rt == 6) {
                /* THE RENDERTYPE'S OWN TRIPLE, as 0x49BE60 hands it to
                   0x46BAE0: rt 1 the projectile's words with 0x8000 added to
                   the second and third (0x49C0FA..0x49C11A), rt 6 the words
                   themselves (0x49C477), rt 3 a local of 0x49BE60's own
                   ([esp+0x38], 0x49C252) that nothing in the function writes
                   -- whatever the stack held, taken here as zero, the
                   unrotated body effects.md §1 records. */
                const short* tr = (const short*)(q + PJ_TURN);
                const char* node = *(const char* const*)(w + W_MODEL);
                short t[3] = { 0, 0, 0 };
                if (rt == 1) {
                    t[0] = tr[0];
                    t[1] = (short)(tr[1] - 0x8000);
                    t[2] = (short)(tr[2] - 0x8000);
                } else if (rt == 6) {
                    t[0] = tr[0]; t[1] = tr[1]; t[2] = tr[2];
                }
                if (ptr_ok(node)) {
                    pe->model = fx_model(node, t, pe->pos, 0, -1, rc);
                    /* THE THRUST FLAME, the node's first child, while the
                       projectile is alive (0x49C12C..0x49C185): its first
                       word is the spin when the weapon's mask has bit 21 */
                    if (rt == 1 && tick < *(const int*)(q + PJ_DEATH)) {
                        const char* child = *(const char* const*)(node + N_CHILD);
                        if (ptr_ok(child)) {
                            const unsigned mask = *(const unsigned*)(w + W_MASK);
                            short ct[3];
                            ct[0] = (mask & (1u << 21)) ? *(const short*)(q + PJ_SPIN) : t[0];
                            ct[1] = t[1]; ct[2] = t[2];
                            pe->cmodel = fx_model(child, ct, pe->pos, 0, -1, rc);
                        }
                    }
                }
            }
            /* the ground-shadow blob: rendertypes 1, 3, 6 always, and 4 unless
               its colour number is 0xFF — the engine's own `case 4` breaks out
               BEFORE the blob for that one and after it for colours past 4 */
            if (rt == 1 || rt == 3 || rt == 6 || (rt == 4 && color != 0xFF)) {
                pe->flags |= TAGPU_PK_FX_SHADOW;
                pe->shadow_y = (pe->pos[2] >> 16)
                             - ((int)*(const unsigned short*)(q + PJ_GROUNDH) >> 1);
            }
            if (rt == 4 && color < 5) {
                const char* seq = *(const char* const*)(ta + OFF_SPRSEQ0 + color * 4);
                int nf = tagpu_gaf_seq_nframes(seq);
                if (nf > 0) {
                    int idx = (tick - *(const int*)(q + PJ_SPAWN)) % nf;
                    if (idx < 0) idx += nf;
                    pe->frame = (unsigned)(size_t)tagpu_gaf_seq_frame(seq, idx);
                }
            } else if (rt == 5) {
                const char* seq = *(const char* const*)(ta + OFF_FLARESEQ);
                int nf = tagpu_gaf_seq_nframes(seq);
                int life = *(const unsigned short*)(w + W_LIFE);
                if (nf > 0 && life > 0) {
                    int idx = nf - ((*(const int*)(q + PJ_DEATH) - tick) * nf) / life;
                    if (idx >= 0 && idx < nf)
                        pe->frame = (unsigned)(size_t)tagpu_gaf_seq_frame(seq, idx);
                }
            }
        }
    }

    /* ---- the flying-debris particle slots (drawn by 0x4211D0) ----------
       Stock's .data array or ours, whichever the engine writes into. */
    for (ps = tagpu_limits_psys_begin(); ps < tagpu_limits_psys_end(); ps++) {
        const char* sys = (const char*)*ps;
        const char* pc;
        TAGPU_PK_DEBRIS* de;
        if (!ptr_ok(sys)) continue;
        pc = *(const char* const*)(sys + PSYS_PIECE);
        if (!ptr_ok(pc)) continue;
        if (s_nDebris >= TAGPU_PK_MAX_DEBRIS) break;      /* one row per slot */
        de = &s_dScratch[s_nDebris++];
        de->pos[0] = *(const int*)(pc + DB_X);
        de->pos[1] = *(const int*)(pc + DB_ALT);
        de->pos[2] = *(const int*)(pc + DB_Y);
        de->model = TAGPU_PK_NOMODEL;
        {
            /* the triple 0x421550 builds and poses the piece by, right before
               it calls 0x4211D0 (tagpu_engine.h DB_TURN_T0..T2) */
            const char* node = *(const char* const*)(pc + DB_NODE);
            short t[3];
            t[0] = *(const short*)(pc + DB_TURN_T0);
            t[1] = *(const short*)(pc + DB_TURN_T1);
            t[2] = *(const short*)(pc + DB_TURN_T2);
            if (ptr_ok(node))
                de->model = fx_model(node, t, de->pos, 1,
                                     debris_logo(ta, *(const char* const*)(sys + PSYS_UNIT)), rc);
        }
    }

    /* ---- explosions (0x420B00) ---------------------------------------
       The pool is a fixed array -- inline in the block, or our static -- so
       there is no base to be NULL and no allocation to outlive: the count is
       the whole bound, and the engine's own add site keeps it under
       EXPL_COUNT (0x420A42). */
    if (ne > 0) {
        if (ne > EXPL_COUNT) ne = EXPL_COUNT;
        for (i = 0; i < ne; i++) {
            const char* q = xpool + EXPL_RECS + (size_t)i * EXPL_STRIDE;
            TAGPU_PK_EXPL* xe;
            if (s_nExpl >= TAGPU_PK_MAX_EXPL) break;      /* ne <= EXPL_COUNT already */
            xe = &s_eScratch[s_nExpl++];
            const short* tr = (const short*)(q + EX_TURN);
            const char* node = *(const char* const*)(q + EX_NODE);
            xe->pos[0] = *(const int*)(q + EX_X);
            xe->pos[1] = *(const int*)(q + EX_ALT);
            xe->pos[2] = *(const int*)(q + EX_Y);
            /* the body (0x420C53..0x420C68): 0 for an explosion that has
               none, else a piece-explosion record (tagpu_packet.h) */
            xe->model  = ptr_ok(node) ? fx_model(node, tr, xe->pos, 0, -1, rc) : TAGPU_PK_NOMODEL;
            xe->frame  = *(const unsigned*)(q + EX_ST1 + TAGPU_AS_SEQ)
                       ? (unsigned)(size_t)tagpu_gaf_state_frame(q + EX_ST1) : 0u;
            xe->flash  = *(const unsigned*)(q + EX_ST2 + TAGPU_AS_SEQ)
                       ? (unsigned)(size_t)tagpu_gaf_state_frame(q + EX_ST2) : 0u;
        }
    }
}

/* the whole gather, once per tick and reach; `tick` is the packet's own
   GameTime */
#define FX_REACH_SLACK 256
static void fx_gather(const char* ta, unsigned tick, unsigned level, const FXREACH* rc)
{
    FXREACH grown;
    const char* layers;
    unsigned want = (tagpu_fxown_want_fx() ? 1u : 0u) | (tagpu_fxown_want_sfx() ? 2u : 0u);
    int L;
    /* THE KEY IS (LEVEL, TICK), not the tick alone. A new level restarts
       GameTime, and some of the GAF frames in the tables belong to the level
       that produced them (an explosion's sprite and flash, below): a cache
       hit across a level boundary would hand the render thread a frame the
       teardown has freed. The level end clears
       the cache as well (tagpu_packet_pub_level_end), but that is the second
       line — this is the one that holds whichever provider fired. */
    /* AND THE REACH, because the models are culled against it (fx_in_reach):
       a gather is kept only for a reach INSIDE the rect it culled against,
       where every model the smaller rect could show was kept. That rect is
       the reach grown by FX_REACH_SLACK on every side, so a camera that moves
       inside one tick -- a scroll publishes about twice a tick at 60 frames a
       second, and more at a higher rate -- finds its reach still inside and
       the models are not posed again. */
    if (s_fxHave && tick == s_fxTick && level == s_fxLevel && want == s_fxWant &&
        rc->l >= s_fxReach.l && rc->r <= s_fxReach.r &&
        rc->t >= s_fxReach.t && rc->b <= s_fxReach.b)
        { if (!s_fillAgain) s_cFxReuse++; return; }
    grown.l = rc->l - FX_REACH_SLACK; grown.r = rc->r + FX_REACH_SLACK;
    grown.t = rc->t - FX_REACH_SLACK; grown.b = rc->b + FX_REACH_SLACK;
    s_nProj = s_nExpl = s_nDebris = s_nPart = 0;
    fx_models_reset();
    s_fxPartTrunc = 0;
    tagpu_pk_fill(s_partN, 0, (unsigned)sizeof s_partN);
    tagpu_pk_fill(s_partObj, 0, (unsigned)sizeof s_partObj);
    if (want & 2u) {
        /* the layer TABLE is per game (0x471D90 allocates it, 0x471DE0 frees
           and NULLS it in the teardown), which is the refusal after a level
           ends; the vectors inside it are this thread's own */
        layers = *(const char* const*)(ta + OFF_LAYERS);
        if (ptr_ok(layers)) {
            unsigned total = count_subs(layers);
            s_partKeep = total > TAGPU_PK_MAX_PART
                ? (unsigned)(((unsigned long long)TAGPU_PK_MAX_PART << 16) / total) : 0x10000u;
            s_partAcc = 0;
            if (s_partKeep < 0x10000u) s_cPartThin++;
            for (L = 0; L < (int)TAGPU_PK_NLAYER; L++) gather_layer(layers, L);
        }
    }
    if (want & 1u) {
        const unsigned char* coltab = (const unsigned char*)(ta + OFF_GUICOL);
        gather_effects(ta, (int)tick, coltab, &grown);
    }
    if (s_fxPartTrunc) s_cPartTrunc++;
    if (s_nPart > s_cPartMax) s_cPartMax = s_nPart;
    s_fxTick = tick; s_fxLevel = level; s_fxWant = want; s_fxReach = grown;
    s_fxHave = 1; s_fxGen++;
    s_cFxScan++;
}

/* the four tables into the record, after the world's; the models' five go
   last of the fill (fill_fxmodels) */
static unsigned fill_fx(TAGPU_PACKET* p, const char* ta, unsigned* cursor)
{
    unsigned need = *cursor, e;
    const char* g = *(const char* const*)TA_GFX_PP;
    FXREACH rc;
    p->fx_caps = ptr_ok(g) ? RDU16(g, PROG_CAPS) : 0u;
    rc.l = p->reach_lo[0]; rc.r = p->reach_hi[0];
    rc.t = p->reach_lo[1]; rc.b = p->reach_hi[1];
    fx_gather(ta, p->tick, p->level_gen, &rc);
    p->fx_gen  = s_fxGen;
    p->fx_want = s_fxWant;
    /* frame 0 of the ground-shadow sequence: one session asset, resolved once
       a frame rather than once a projectile */
    p->shadow_frame = (unsigned)(size_t)tagpu_gaf_seq_frame(
        *(const char* const*)(ta + OFF_SHADOWSEQ), 0);
    if (s_fxPartTrunc) p->truncated |= TAGPU_PK_TRUNC_PART;
    e = append_table(p, cursor, s_pScratch, s_nProj, (unsigned)sizeof(TAGPU_PK_PROJ),
                     &p->off_proj, &p->n_proj, TAGPU_PK_TRUNC_PROJ);
    if (e > need) need = e;
    e = append_table(p, cursor, s_eScratch, s_nExpl, (unsigned)sizeof(TAGPU_PK_EXPL),
                     &p->off_expl, &p->n_expl, TAGPU_PK_TRUNC_EXPL);
    if (e > need) need = e;
    e = append_table(p, cursor, s_dScratch, s_nDebris, (unsigned)sizeof(TAGPU_PK_DEBRIS),
                     &p->off_debris, &p->n_debris, TAGPU_PK_TRUNC_DEBRIS);
    if (e > need) need = e;
    e = append_table(p, cursor, s_partScratch, s_nPart, (unsigned)sizeof(TAGPU_PK_PART),
                     &p->off_part, &p->n_part, TAGPU_PK_TRUNC_PART);
    if (e > need) need = e;
    /* the per-layer counts describe the table that LANDED: a truncated
       particle table carries none of them, because the consumer walks the
       layers by these counts and a stale set would name another layer's rows */
    if (p->n_part) {
        tagpu_pk_copy(p->part_n, s_partN, (unsigned)sizeof p->part_n);
        tagpu_pk_copy(p->part_obj, s_partObj, (unsigned)sizeof p->part_obj);
    }
    lht_snapshot();
    if (s_lhtOk) {
        e = append_area(p, cursor, s_lht, (unsigned)sizeof s_lht, &p->lht_off, &p->lht_len,
                        TAGPU_PK_TRUNC_LHT);
        if (e > need) need = e;
    }
    s_cLastProj = s_nProj; s_cLastExpl = s_nExpl;
    s_cLastFm = s_nFm; s_cLastFv = s_nFv;
    s_cLastDebris = s_nDebris; s_cLastPart = s_nPart;
    return need;
}

/* THE FIVE MODEL TABLES, LAST OF THE FILL AND TOGETHER OR NOT AT ALL. A model
   whose shape or vertices did not land would name bytes the packet does not
   carry, so a slot too small for all five carries none of them: every
   record's model then lies past `n_fxmodel` and the consumer takes each such
   record back whole, for this slot's one frame, while the shortfall grows the
   slot. They go last because they are the one layer an engine count can
   drive past the reserve's design point (their bound is fx_fits), so a cut
   costs them and nothing else -- and the cursor goes back to where they began,
   so what the tables that did fit took is not lost to the packet either. */
static unsigned fill_fxmodels(TAGPU_PACKET* p, unsigned* cursor)
{
    const unsigned mark = *cursor;
    unsigned need = *cursor, e;
    e = append_table(p, cursor, s_fvScratch, s_nFv, (unsigned)sizeof(TAGPU_PK_FXVERT),
                     &p->off_fxvert, &p->n_fxvert, TAGPU_PK_TRUNC_FXMODEL);
    if (e > need) need = e;
    e = append_table(p, cursor, s_fiScratch, s_nFi, (unsigned)sizeof(uint16_t),
                     &p->off_fxidx, &p->n_fxidx, TAGPU_PK_TRUNC_FXMODEL);
    if (e > need) need = e;
    e = append_table(p, cursor, s_ffScratch, s_nFf, (unsigned)sizeof(TAGPU_PK_FXFACE),
                     &p->off_fxface, &p->n_fxface, TAGPU_PK_TRUNC_FXMODEL);
    if (e > need) need = e;
    e = append_table(p, cursor, s_fsScratch, s_nFs, (unsigned)sizeof(TAGPU_PK_FXSHAPE),
                     &p->off_fxshape, &p->n_fxshape, TAGPU_PK_TRUNC_FXMODEL);
    if (e > need) need = e;
    e = append_table(p, cursor, s_fmScratch, s_nFm, (unsigned)sizeof(TAGPU_PK_FXMODEL),
                     &p->off_fxmodel, &p->n_fxmodel, TAGPU_PK_TRUNC_FXMODEL);
    if (e > need) need = e;
    if (p->truncated & TAGPU_PK_TRUNC_FXMODEL) {
        p->n_fxvert = p->n_fxidx = p->n_fxface = p->n_fxshape = p->n_fxmodel = 0;
        p->off_fxvert = p->off_fxidx = p->off_fxface = p->off_fxshape = p->off_fxmodel = 0;
        /* the tables that fitted are owed again with the rest: the slot has
           to grow by all five. They began at `ALIGN4(mark)` (append_table),
           so this is their 4-aligned length exactly. */
        s_fillShort += PKT_ALIGN4(*cursor) - PKT_ALIGN4(mark);
        *cursor = mark;
    }
    return need;
}


/* ======================== THE FOG SOURCE ==================================
   The fog of war's source bits for the whole map (tagpu_fogwide.h), packed on
   THIS thread, in THIS draw -- in `terr_fogtick` when the fog site is ours, in
   `wide_tick` below when it is the engine's -- and copied into the packet here.
   The packet carries nothing of the engine's own screen grid: every reader
   expands the bits as its builder 0x4843C0 does (tagpu_fogsrc.h). */

static volatile unsigned s_cFogSeen, s_cFogRecOdd;
static volatile int      s_lastFogW, s_lastFogH;

/* THE ENGINE GRID'S BUILD RECORD: whether its builder `0x4843C0` last ran in a
   draw this observer tracked, at the eye that draw was drawn from. GAME THREAD
   ONLY. Nothing here copies the grid; the record keeps two things true: the
   wide tick's `rebuilt` (s_fogStale, below), and the oracle's premise
   (tagpu_fogwide.c `fogw_check`) that the engine's grid was built at the eye
   the draw reads.

   IT IS TAKEN FROM LosType BIT 3, WHICH ONLY A BUILD SETS [DISASSEMBLED
   2026-09-24]. The one engine setter is `0x484904`, right after the lazy
   test `0x4848F2` and the build `0x4848FA` inside the fog function
   `0x4848E0`, which DrawGameScreen calls once (`0x469D8E`); ours is
   `terr_fogtick`, right after its own call of the builder. Every other store
   to the word either clears the bit or writes it back as it read it (the
   exe map's row "who sets `main+0x14281` bit 3" lists all fifty). None of
   the fifteen functions holding a clear is reached from DrawGameScreen by
   direct calls, branches or jump tables, and nothing in it stores the eye
   (the publish point's own note, `before_draw`). So:
     bit 3 clear after this draw's apply and set after the draw — the fog
       function rebuilt the grid IN this draw, from the eye the draw was drawn
       at;
     set before — nothing rebuilt it, and the record stands, however the eye
       has moved since without clearing the bit (the camera shake `0x41C6F0`
       stores it at `0x41C784`/`0x41C797` and does not);
     set before and clear after — not expected, counted (`recodd=`), and the
       record dropped.
   A DrawGameScreen this observer does not track — the screenshot sweep, the
   movie recorder, the depth guard — may rebuild at another eye, so it drops
   the record, as a new level does. WHERE THERE IS NO RECORD the apply clears
   the bit, exactly as every engine eye writer does, so the draw it precedes
   rebuilds and makes one. */
static int      s_engRecOk;
static unsigned s_engRecGen;
static int      s_fogBitBefore = -1;      /* bit 3 after this draw's apply; -1 untracked */
/* the LOS state moved in this draw: bit 3 was clear after the apply (the
   engine grid was not current as the draw began), or it is clear at the draw's
   end (something cleared it during the draw -- a `recodd` draw), or the draw
   was untracked. What the wide tick is told as `rebuilt` when the fog site is
   the engine's (wide_tick). */
static int      s_fogStale = 1;

static void fog_rec_before(char* ta)
{
    unsigned short* los = (unsigned short*)(ta + OFF_LOSTYPE);
    if (!s_engRecOk || s_engRecGen != s_levelGen) *los &= (unsigned short)~8u;
    s_fogBitBefore = (*los & 8) != 0;
}

/* a draw this observer does not bracket: no record survives it, and no
   "before" reading taken by a tracked draw it is nested in */
static void fog_rec_drop(void)
{
    s_engRecOk = 0;
    s_fogBitBefore = -1;
}

static void fog_rec_after(const char* ta)
{
    int after;
    if (!ta || s_fogBitBefore < 0) { s_fogStale = 1; fog_rec_drop(); return; }
    after = (RDU16(ta, OFF_LOSTYPE) & 8) != 0;
    s_fogStale = s_fogBitBefore != 1 || !after;
    if (!s_fogBitBefore && after) {
        s_engRecGen = s_levelGen; s_engRecOk = 1;
    } else if (s_fogBitBefore && !after) {
        s_cFogRecOdd++; s_engRecOk = 0;
    }
    s_fogBitBefore = -1;
}

/* THE SOURCE ON EVERY TRACKED DRAW, not only while the fog site is ours.
   `terr_fogtick` ticks tagpu_fogwide at the engine's fog call `0x469D8E`,
   but only on a draw latched to our terrain (`g_terrown_own`), and that latch
   follows the terrain pass's request: at a level's start, until our pass
   has gathered a frame and the game thread has seen its request, and under
   `terr.on=off` or `passive`, the ENGINE's `0x4848E0` runs there instead and
   our site does not. So on the draws where our site did not run, this `after`
   ticks the source itself, before the fill copies it.

   ONLY WHILE THE SESSION'S RENDERER IS VULKAN. The source's one reader is the
   Vulkan lane; on the GDI lane nothing reads it, and a pack per sim tick there
   is paid for nothing. The renderer is a property of the session, not of a
   frame: `dd_CreateEx` stores `g_ddraw.renderer` on the game thread before the
   first draw, and its one later store is render_vk.c's hand-over to GDI, after
   which no frame is drawn from a packet at all.

   WHAT IT READS IS SAFE HERE FOR THE SAME REASON AS AT THE FOG SITE: every
   in-play draw falls inside the lifetimes of both maps, and each index the
   pack forms is bounded by the maps' own dimensions (tagpu_fogwide.c,
   fogw_source). The local player's LOS counters are allocated by `0x464700`,
   which `0x464990` calls for each active player at `0x4919C8`, inside the
   level load `LoadGameData_Main 0x4917D0` on the loader thread, and freed in
   the teardown cascade `0x491B60` by `0x464A00` (`0x491BA9`). MAPPED is
   allocated at `0x483CF6` in the map load and freed at `0x483E70`, by the
   map-free routine `0x483DD0` later in that teardown (`0x491BB3`)
   [DISASSEMBLED 2026-09-28; exe-reverse-engineering.md, "The two routines
   that bracket a level"]. This `after` runs on the in-play gate, after the
   load and before the teardown, on the thread that runs the teardown -- the
   argument every table this publisher copies stands on.

   `rebuilt` IS "the LOS state moved in this draw" (s_fogStale). The engine's
   `0x4848E0` runs on every in-play draw (its call at `0x469D8E` is gated on
   `drawUnits`, which the in-play call passes as 1) and rebuilds whenever bit
   3 is clear, and every LOS stamp and every eye writer clears it; so a clear
   bit at the apply is the same signal `terr_fogtick` hands over when it
   rebuilds, a bit set at the apply and clear at the draw's end is a stamp
   inside the draw (`recodd`), and an untracked draw counts as one. The tick
   packs on it or on changed maps, and otherwise keeps the bits, which are then
   still exactly what a pack would produce. */
static unsigned s_wideTickDraw;
static int      s_wideTickSeen;

static void wide_tick(void)
{
    char* ta;
    if (g_ddraw.renderer != vk_render_main) return;  /* no reader on the GDI lane */
    if (tagpu_terrown_fog_site_live()) return;       /* ticked at our fog site */
    ta = (char*)ta_main();
    if (!ta) return;
    tagpu_fogwide_tick(ta, s_fogStale);
    s_wideTickDraw = s_cDraws; s_wideTickSeen = 1;
}

/* THE TABLES' REACH, written into the header before the tables are filled: the
   window tagpu_zoom_pub_window publishes about the packet's eye on each axis --
   the native pass's gather slab at the reach level (the rung the render thread
   asked for, clamped to the floor by this draw's apply), plus
   TAGPU_GATHER_MARGIN, grown by the lead the render thread's unapplied eye
   steps are sized to fit in. The anchor table and the models' cull cover it;
   the render thread draws at no level below `reach_level` (tagpu_zoom.c, "the
   reach ladder") and its reach bound holds the eye it draws from until its
   slab lies inside the rect, so a frame never gathers past what this packet
   carries. The floor rides beside it: the render thread's floor for the
   frames drawn from this packet. */
static void table_reach(TAGPU_PACKET* p)
{
    int axis, l, span;
    p->reach_level = tagpu_zoom_reach_level();
    p->zoom_floor = tagpu_zoom_game_floor();
    for (axis = 0; axis < 2; axis++) {
        tagpu_zoom_pub_window(p->eye[axis], p->vp[2 + axis], p->reach_level, &l, &span);
        p->reach_lo[axis] = l;
        p->reach_hi[axis] = l + span;
    }
}

/* The source into the packet. ONLY ONE PACKED OR KEPT IN THIS DRAW, at our fog
   site or by wide_tick: the producer's own "valid" flag cannot say so, since it
   is only ever cleared from inside a tick, so a tick that stopped running would
   leave the packet carrying the LAST bits ever packed against an LOS state that
   keeps moving. Both tick sites stamp the draw. */
static unsigned fill_fog(TAGPU_PACKET* p, unsigned* cursor)
{
    unsigned need = *cursor, e, len = 0;
    TAGPU_FOGSRC f;
    if ((tagpu_terrown_fog_site_live() || (s_wideTickSeen && s_wideTickDraw == s_cDraws)) &&
        tagpu_fogwide_source(&f) && tagpu_fog_src_ok(&f)) {
        unsigned off = 0;
        e = append_area(p, cursor, f.words, (unsigned)f.stride * (unsigned)f.h * 4u,
                        &off, &len, TAGPU_PK_TRUNC_FOGSRC);
        if (e > need) need = e;
        if (len) {
            p->fogsrc_w = f.w; p->fogsrc_h = f.h; p->fogsrc_stride = f.stride;
            p->fogsrc_off = off; p->fogsrc_serial = f.serial;
            s_cFogSeen++;
        }
    }
    s_lastFogW = p->fogsrc_w; s_lastFogH = p->fogsrc_h;
    return need;
}


/* ==================== THE UI'S RENDER HALF ================================
   The four engine reads tagpu_gui_surf.c needs every present. The queue
   itself is untouched: it carries an op STREAM into retained twins and stays
   a queue (the plan's §9). What is taken here is the per-frame state the
   render half reads beside it.

   THE MINIMAP SURFACES ARE THE ONLY EXPENSIVE ONE and they are gated on the
   consumer asking (tagpu_gui_want_minimap), because at k = 1 the sharp minimap
   is deliberately the engine's own and the copy would be paid for nothing.
   Their DESCRIPTORS are what make a render-thread read dangerous rather than
   merely stale: each carries a base and a pitch, so a torn one is a wild read
   and not a wrong picture. Their lifetime is the minimap build's — 0x4669B0,
   from the level load at 0x4919C3, stores the three pointers once, and
   0x466AA0 frees and NULLS them inside the teardown cascade — and every field
   is cross-checked here, on the thread that owns them.

   THE PICTURE IS DECODED HERE TOO, on the level's first in-play publish, and
   not on the LOADER thread, the one publisher this plan's rule forbids
   outright. The engine's own ordering is what makes decoding it here correct —
   the in-play handler is installed only after the loader has set bit 1 of
   main+0x38D75, so on the first in-play draw of a level every per-map pointer
   is final. */

#define MM_OFFX      0x142E7      /* i16 the box the engine fitted it into    */
#define MM_OFFY      0x142E9
#define MM_W         0x142EB
#define MM_H         0x142ED
#define MM_VIEWRECT  0x142CB      /* i32[4] the view box, screen px, inclusive */
#define MM_VIEWCOL   0x0DD9       /* u8 its palette index (0x466B50 reads it) */
#define MM_COMPOSITE 0x142DB      /* the fog+dots composite; non-NULL = built */
#define MM_FOGBASE   0x142DF      /* the base WITH the engine's fog shading   */
#define MM_SCALEDMAP 0x142E3      /* the same base WITHOUT it                 */
#define MM_PICFRAME  0x1426B      /* the level's minimap picture, a GAF frame */
/* GFX_CUR_REC / GFX_CUR_ON / GFX_CUR_OK are in tagpu_engine.h with the rest
   of the graphics globals, because the shell's cursor channel and this file's
   in-play fill read the same words and a second copy is how they would come to
   disagree. */

/* the interleave, per frame, per row */
static unsigned char s_mmRg[TAGPU_PK_MM_DIMCAP * TAGPU_PK_MM_DIMCAP * 3];
/* the level's picture, decoded once per level (see fill_gui) */
static unsigned char s_mmPic[TAGPU_PK_MM_DIMCAP * TAGPU_PK_MM_DIMCAP];
static int s_mmPicW, s_mmPicH, s_mmPicGen = -1;
static volatile unsigned s_cMmCopies, s_cMmRefused, s_cMmPic;
static volatile int s_lastMmW, s_lastMmH;

/* The cursor's sprite record, as a KEY (tagpu_gaf.c is its only reader).
   0 when the graphics globals or the record cannot be read; the caller zeroed
   the header, so every path that does not set it leaves it 0, which the
   consumer reads as "no cursor this frame". */
static void fill_cursor(TAGPU_PACKET* p)
{
    const char* g = *(const char* const*)TA_GFX_PP;
    const unsigned short* rec;
    if (!ptr_ok(g)) return;
    rec = *(const unsigned short* const*)(g + GFX_CUR_REC);
    if (!ptr_ok(rec)) return;
    p->cur_rec = (unsigned)(size_t)rec;
}

/* THE PLACEMENT'S SITES, COPIED BY VALUE. tagpu_place.c answers on this game
   thread from its own game-thread state, checked against the cursor mode and
   type the engine holds at this fill (tagpu_place.h); the rows carry no
   pointer. At most TAGPU_PK_MAX_PLACE, the cap the acquire checks again with
   every site's colour, coordinates and type. A table that does not fit sets
   its bit, and the render thread then draws neither the sites nor the square
   they replace (tagpu_pk_place_replaces). */
static TagpuPlaceSite s_plScratch[TAGPU_PK_MAX_PLACE];

static unsigned fill_place(TAGPU_PACKET* p, unsigned* cursor, unsigned* nsites)
{
    unsigned n = tagpu_place_sites(s_plScratch, TAGPU_PK_MAX_PLACE);
    if (n > TAGPU_PK_MAX_PLACE) n = TAGPU_PK_MAX_PLACE;
    *nsites = n;
    return append_table(p, cursor, s_plScratch, n, (unsigned)sizeof(TAGPU_PK_PLACE),
                        &p->off_place, &p->n_place, TAGPU_PK_TRUNC_PLACE);
}

/* THE RECLAIM SWEEP, COPIED BY VALUE: tagpu_sweep.c answers on this game thread from its own
   game-thread state and the engine's feature grid, read inside this draw (tagpu_sweep.h). The
   header and at most TAGPU_PK_MAX_SWEEP items, the caps the acquire checks again with every
   field; items cut by the cap or by the packet's room set the table's bit, and the marker pass
   then draws what arrived. */
static TAGPU_PK_SWEEP     s_swHead;
static TAGPU_PK_SWEEPITEM s_swItems[TAGPU_PK_MAX_SWEEP];

static unsigned fill_sweep(TAGPU_PACKET* p, unsigned* cursor)
{
    unsigned n = 0, need = *cursor, e;
    int cut = 0;
    if (!tagpu_sweep_fill(&s_swHead, s_swItems, TAGPU_PK_MAX_SWEEP, &n, &cut)) return need;
    if (n > TAGPU_PK_MAX_SWEEP) n = TAGPU_PK_MAX_SWEEP;
    if (cut) p->truncated |= TAGPU_PK_TRUNC_SWEEP;
    e = append_table(p, cursor, &s_swHead, 1u, (unsigned)sizeof(TAGPU_PK_SWEEP),
                     &p->off_sweep, &p->n_sweep, TAGPU_PK_TRUNC_SWEEP);
    if (e > need) need = e;
    if (!p->n_sweep) return need;      /* no items without their header */
    e = append_table(p, cursor, s_swItems, n, (unsigned)sizeof(TAGPU_PK_SWEEPITEM),
                     &p->off_sweepitem, &p->n_sweepitem, TAGPU_PK_TRUNC_SWEEP);
    if (e > need) need = e;
    return need;
}

/* THE FORMATION MOVE'S DOTS, COPIED BY VALUE: tagpu_formation.c answers on this game thread from
   its own game-thread gesture state (tagpu_formation.h). At most TAGPU_PK_MAX_FORM, the cap the
   acquire checks again with every coordinate; dots cut by the cap or by the packet's room set the
   table's bit, and the marker pass then draws what arrived. The order art is built here when a
   dot rides (tagpu_order_art_ensure), so the dots do not wait for a Shift-held order snapshot. */
static TagpuFormDot s_formScratch[TAGPU_PK_MAX_FORM];

static unsigned fill_form(TAGPU_PACKET* p, unsigned* cursor)
{
    int cut = 0;
    unsigned n = tagpu_formation_dots(s_formScratch, TAGPU_PK_MAX_FORM, &cut);
    if (n > TAGPU_PK_MAX_FORM) n = TAGPU_PK_MAX_FORM;
    if (cut) p->truncated |= TAGPU_PK_TRUNC_FORM;
    if (n) tagpu_order_art_ensure();
    return append_table(p, cursor, s_formScratch, n, (unsigned)sizeof(TAGPU_PK_FORMDOT),
                        &p->off_form, &p->n_form, TAGPU_PK_TRUNC_FORM);
}

/* THE WEAPON RANGE RINGS, COPIED BY VALUE: tagpu_rangeown.c answers on this game thread from the
   engine's units, types and weapons, read inside this draw (tagpu_rangeown.h). The header and at
   most TAGPU_PK_MAX_RING rings, the caps the acquire checks again with every field. Each ring's
   unit is mapped onto this packet's unit table through the slot map fill_world wrote this frame,
   so the render thread can take the unit's interpolated position; a slot the walk did not reach
   maps to none, and the ring then stands at the position the record carries. */
static TAGPU_PK_RANGE s_rgHead;
static TAGPU_PK_RING  s_rgRings[TAGPU_PK_MAX_RING];

static unsigned fill_range(TAGPU_PACKET* p, unsigned* cursor)
{
    unsigned n = 0, need = *cursor, e, k;
    int cut = 0;
    if (!tagpu_rangeown_fill(&s_rgHead, s_rgRings, TAGPU_PK_MAX_RING, &n, &cut)) return need;
    if (n > TAGPU_PK_MAX_RING) n = TAGPU_PK_MAX_RING;
    if (cut) p->truncated |= TAGPU_PK_TRUNC_RING;
    for (k = 0; k < n; k++) {
        unsigned slot = s_rgRings[k].slot;
        s_rgRings[k].uidx = slot && slot < s_slotWalk && slot < TAGPU_PK_MAX_UNITS
                            ? s_slotIdx[slot] : 0xFFFFu;
    }
    e = append_table(p, cursor, &s_rgHead, 1u, (unsigned)sizeof(TAGPU_PK_RANGE),
                     &p->off_range, &p->n_range, TAGPU_PK_TRUNC_RING);
    if (e > need) need = e;
    if (!p->n_range) return need;      /* no rings without their header */
    e = append_table(p, cursor, s_rgRings, n, (unsigned)sizeof(TAGPU_PK_RING),
                     &p->off_ring, &p->n_ring, TAGPU_PK_TRUNC_RING);
    if (e > need) need = e;
    return need;
}

/* THE BUILD-ORDERS TABLE — the queued builds the order pass is showing site
   rects for, copied out of that pass's own game-thread snapshot (the one the
   squares are drawn from), so the ghost pass and the squares can never drift.
   Both caps are counted: the arena's in the snapshot's `dropped`, this one's
   in the packet's `truncated` bit. */
static TAGPU_PK_BUILD s_builds[TAGPU_PK_MAX_BUILDS];

/* THE GHOSTS' PIECE MASKS: one row per type a ghost may be drawn for this
   frame that hides a piece — the build cursor's type, every placement site's
   and every type in the table above whose site is not started yet — sorted by
   type, because the consumer binary-searches it per ghost. tagpu_datakeys
   computes each type once a level, so a frame costs a pass over the sites and
   the builds and a cache hit per distinct type. A type is taken ONCE through `s_gmSeen`, which is cleared
   again from the collected list rather than wholesale. */
static TAGPU_PK_GHOSTMASK s_gmask[TAGPU_PK_MAX_GHOSTMASK];
static unsigned           s_gmSeen[TAGPU_LIM_TYPES / 32];

static int gmask_type_cmp(const void* a, const void* b)
{
    return (int)((const TAGPU_PK_GHOSTMASK*)a)->type - (int)((const TAGPU_PK_GHOSTMASK*)b)->type;
}

/* The cursor's type is the packet's own `build_unit_id`, under the render
   thread's own test for drawing it (cursor mode 0x0E), so every packet the
   cursor ghost is drawn from carries that type's row, or the table's
   TAGPU_PK_TRUNC_GHOSTMASK bit, on which the ghost pass draws nothing. */
static unsigned fill_ghostmask(TAGPU_PACKET* p, unsigned* cursor, int nb, unsigned nplace)
{
    static unsigned short types[TAGPU_PK_MAX_BUILDS + 1 + TAGPU_PK_MAX_PLACE];
    unsigned nt = 0, nm = 0, k, over = 0;
    unsigned cur = p->cursor_mode == 0x0E ? p->build_unit_id : 0u;
    if (cur && cur < TAGPU_LIM_TYPES) {
        s_gmSeen[cur >> 5] |= 1u << (cur & 31);
        types[nt++] = (unsigned short)cur;
    }
    for (k = 0; k < nplace; k++) {
        unsigned t = s_plScratch[k].type;
        if (!t || t >= TAGPU_LIM_TYPES) continue;
        if (s_gmSeen[t >> 5] >> (t & 31) & 1) continue;
        s_gmSeen[t >> 5] |= 1u << (t & 31);
        types[nt++] = (unsigned short)t;
    }
    for (k = 0; k < (unsigned)nb; k++) {
        unsigned t = s_builds[k].type;
        if (s_builds[k].started || !t || t >= TAGPU_LIM_TYPES) continue;
        if (s_gmSeen[t >> 5] >> (t & 31) & 1) continue;
        s_gmSeen[t >> 5] |= 1u << (t & 31);
        types[nt++] = (unsigned short)t;
    }
    for (k = 0; k < nt; k++) {
        TAGPU_PK_GHOSTMASK row;
        s_gmSeen[types[k] >> 5] &= ~(1u << (types[k] & 31));
        if (!tagpu_datakeys_ghost_mask(types[k], &row)) continue;
        if (nm >= TAGPU_PK_MAX_GHOSTMASK) { over = 1; continue; }
        s_gmask[nm++] = row;
    }
    if (over) p->truncated |= TAGPU_PK_TRUNC_GHOSTMASK;
    if (!nm) return *cursor;
    qsort(s_gmask, nm, sizeof s_gmask[0], gmask_type_cmp);
    return append_table(p, cursor, s_gmask, nm, (unsigned)sizeof(TAGPU_PK_GHOSTMASK),
                        &p->off_ghostmask, &p->n_ghostmask, TAGPU_PK_TRUNC_GHOSTMASK);
}

static unsigned fill_builds(TAGPU_PACKET* p, unsigned* cursor, unsigned nplace)
{
    unsigned e, need = *cursor;
    int n = 0;
    /* THE BUILDS TABLE IS GATED ON THE ONLY PASS THAT READS IT, like the
       effect tables above: the walk and the copy are pure cost to a session
       with no build ghost. `tagpu_native_want_builds()` is the ghost's own
       30-frame poll, published from the render thread; being a frame late
       either way costs one frame of an unused or an empty table. */
    if (tagpu_native_want_builds()) {
        n = tagpu_order_copy_builds(s_builds, TAGPU_PK_MAX_BUILDS);
        if (n > 0) {
            e = append_table(p, cursor, s_builds, (unsigned)n, (unsigned)sizeof(TAGPU_PK_BUILD),
                             &p->off_builds, &p->n_builds, TAGPU_PK_TRUNC_BUILDS);
            if (e > need) need = e;
        }
        if (n < 0) n = 0;
    }
    /* THE MASKS ARE NOT GATED: the cursor ghost draws whenever its lever is
       on, whatever the poll has published yet, so its row rides every packet
       that carries its type (a cut table sets its bit, and the ghost pass
       then draws nothing). What that costs a session with no ghost is one
       cached lookup a frame while a build is on the cursor. */
    e = fill_ghostmask(p, cursor, n, nplace);
    if (e > need) need = e;
    return need;
}

/* THE REPLAY'S OVERLAYS, COPIED BY VALUE. The replay builds them at its frame visit, before
   the draw call 0x4969CD, from the recording alone; this fill runs in the `after` of that same
   draw, at 0x4969D2, on the same game thread -- so a packet carries the overlays of the frame it
   describes, and nothing in it points into the replay's state. The items are at most
   TAGPU_PK_MAX_OVERLAY, the bound the acquire checks again with every item's kind and player. */
static TAGPU_PK_OVERLAY s_ovScratch[TAGPU_PK_MAX_OVERLAY];

static unsigned fill_overlay(TAGPU_PACKET* p, unsigned* cursor)
{
    unsigned n = tagpu_replay_overlays(s_ovScratch, TAGPU_PK_MAX_OVERLAY, p->ov_col);
    if (n > TAGPU_PK_MAX_OVERLAY) n = TAGPU_PK_MAX_OVERLAY;
    return append_table(p, cursor, s_ovScratch, n, (unsigned)sizeof(TAGPU_PK_OVERLAY),
                        &p->off_overlay, &p->n_overlay, TAGPU_PK_TRUNC_OVERLAY);
}

static unsigned fill_gui(TAGPU_PACKET* p, const char* ta, unsigned* cursor)
{
    unsigned need = *cursor, e;
    const int* co;
    fill_cursor(p);
    p->mm_box[0] = RDI16(ta, MM_OFFX); p->mm_box[1] = RDI16(ta, MM_OFFY);
    p->mm_box[2] = RDI16(ta, MM_W);    p->mm_box[3] = RDI16(ta, MM_H);
    { int k; for (k = 0; k < 4; k++) p->mm_view[k] = RD32(ta, MM_VIEWRECT + k * 4); }
    p->mm_viewcol = RDU8(ta, MM_VIEWCOL);
    co = *(const int* const*)(ta + MM_COMPOSITE);
    p->mm_live = ptr_ok(co) ? 1u : 0u;

    /* THE LEVEL'S PICTURE, DECODED HERE, INTO ITS FIRST IN-PLAY PACKET AND NO
       OTHER. `main+0x1426B` is the TNT's own minimap picture, a GAF frame, and
       it is ALIVE FOR THE WHOLE LEVEL: LoadMap stores it at 0x483900 (0x483936
       stores NULL when the file is absent), `BuildMinimapSurface 0x466780`
       reads it at 0x46684F and does not null it, and the only thing that frees
       it is 0x483DFE inside 0x483DD0, whose only caller is 0x491BB3 — the level
       TEARDOWN cascade [VERIFIED 2026-09-12, objdump of the pristine build].

       So the picture is not alive only inside that call, and nothing needs to
       decode it on the LOADER thread. The first in-play draw is the earliest
       moment the engine's own ordering makes every per-map pointer final (the
       in-play handler is installed only after the loader sets bit 1 of
       main+0x38D75), and this is that draw. */
    if (s_mmPicGen != (int)p->level_gen) {
        const unsigned char* fr = tagpu_gaf_frame_sane(*(const void* const*)(ta + MM_PICFRAME));
        s_mmPicGen = (int)p->level_gen;              /* tried: not once a draw */
        s_mmPicW = s_mmPicH = 0;
        if (fr) {
            int w = *(const unsigned short*)(fr + TAGPU_GF_W);
            int h = *(const unsigned short*)(fr + TAGPU_GF_H);
            if (w > 0 && h > 0 && w <= TAGPU_PK_MM_DIMCAP && h <= TAGPU_PK_MM_DIMCAP &&
                tagpu_gaf_decode(fr, w, h, s_mmPic)) {
                s_mmPicW = w; s_mmPicH = h;
            }
        }
    }
    /* IT RIDES UNTIL THE CONSUMER SAYS IT HAS IT, not until one packet has
       carried it. The mailbox is latest-wins and a dropped packet is a counted
       statistic — and the likeliest packet to be dropped is a level's first,
       when the render thread is busiest. It is also gated on the minimap being
       WANTED at all, so a session that never turns the sharp minimap on never
       pays the 63 KB. */
    if (s_mmPicW > 0 && s_mmPicGen == (int)p->level_gen &&
        tagpu_gui_want_minimap() &&
        tagpu_gui_minimap_have() != p->level_gen + 1u) {
        p->mmpic_w = s_mmPicW; p->mmpic_h = s_mmPicH;
        e = append_area(p, cursor, s_mmPic, (unsigned)s_mmPicW * (unsigned)s_mmPicH,
                        &p->mmpic_off, &p->mmpic_len, TAGPU_PK_TRUNC_MMPIC);
        if (e > need) need = e;
        if (p->mmpic_len) s_cMmPic++;
        else { p->mmpic_w = p->mmpic_h = 0; }        /* did not fit: the next packet tries */
    }

    if (p->mm_live && tagpu_gui_want_minimap()) {
        const int* fo = *(const int* const*)(ta + MM_FOGBASE);
        const int* so = *(const int* const*)(ta + MM_SCALEDMAP);
        const unsigned char *fb, *sb, *cb;
        int ew, eh, fp, sp, cp, yy, xx;
        if (!ptr_ok(fo) || !ptr_ok(so)) { s_cMmRefused++; return need; }
        ew = fo[0]; eh = fo[1];
        fb = (const unsigned char*)(size_t)fo[3];
        sb = (const unsigned char*)(size_t)so[3];
        cb = (const unsigned char*)(size_t)co[3];
        fp = fo[2]; sp = so[2]; cp = co[2];
        /* THE PITCHES ARE VALIDATED LIKE THE DIMENSIONS. The walk is
           `base + yy * pitch` for yy up to eh: a wild or negative pitch out of
           a half-freed surface reads eh x pitch bytes of engine memory. A row
           cannot be shorter than the surface is wide, and these are 8bpp
           offscreens of at most 512 px. */
        if (ew <= 0 || eh <= 0 || ew > TAGPU_PK_MM_DIMCAP || eh > TAGPU_PK_MM_DIMCAP ||
            so[0] != ew || so[1] != eh || co[0] != ew || co[1] != eh ||
            !ptr_ok(fb) || !ptr_ok(sb) || !ptr_ok(cb) ||
            fp < ew || sp < ew || cp < ew || fp > 4096 || sp > 4096 || cp > 4096) {
            s_cMmRefused++; return need;
        }
        for (yy = 0; yy < eh; yy++) {
            const unsigned char* fr = fb + (size_t)yy * fp;
            const unsigned char* sr = sb + (size_t)yy * sp;
            const unsigned char* cr = cb + (size_t)yy * cp;
            unsigned char* d = s_mmRg + (size_t)yy * ew * 3;
            for (xx = 0; xx < ew; xx++) {
                d[3 * xx] = fr[xx]; d[3 * xx + 1] = sr[xx]; d[3 * xx + 2] = cr[xx];
            }
        }
        p->mm_w = ew; p->mm_h = eh;
        e = append_area(p, cursor, s_mmRg, (unsigned)ew * (unsigned)eh * 3u,
                        &p->mm_off, &p->mm_len, TAGPU_PK_TRUNC_MM);
        if (e > need) need = e;
        if (!p->mm_len) { p->mm_w = p->mm_h = 0; }
        else s_cMmCopies++;
        s_lastMmW = p->mm_w; s_lastMmH = p->mm_h;
    }
    return need;
}


/* The engine's palette table and gamma factor, into the packet — both kinds
   of packet carry them (the level-end one from the teardown, where `main` is
   still valid), so the render thread's palette module never reads either
   field itself (tagpu_pal.c). The gamma is BOUNDED
   here to the band a slider or the chat command can produce; anything else,
   NaN included, ships as 1.0, the identity. */
static void fill_pal(TAGPU_PACKET* p, const char* ta)
{
    const char* g = *(const char* const*)TA_GFX_PP;
    float v = 1.0f;
    if (!ta) return;
    tagpu_pk_copy(p->pal, ta + OFF_PALETTE, sizeof p->pal);
    if (ptr_ok(g)) {
        v = *(const float*)(g + GFX_GAMMA);
        if (!(v >= 0.05f && v <= 8.0f)) v = 1.0f;
    }
    p->gamma  = v;
    p->pal_ok = 1;
}

/* THE COUNTS OF A FILL THAT IS DONE AGAIN ARE TAKEN BACK. pkx_publish fills
   a slot a second time when the first fill did not fit, and publishes only
   the second, so everything these count -- a cut table, a grid or minimap
   that landed, a refusal, a duplicate id -- counts the fill that was
   published, once per publish. The mark is taken before a publish's first
   fill and restored before each fill that redoes it. Not in the list: the
   anchor and effect SCANS, which count work done (a refill reuses their
   caches, and its reuse is not counted either), and the snapshot copies,
   which a refill does not repeat. */
#define PK_FILL_COUNTS(X) \
    X(s_cUnitTrunc) X(s_cWreckTrunc) X(s_cPieceTrunc) X(s_cUnitDup) X(s_cRelBad) \
    X(s_cWreckOob) X(s_cFogSeen) \
    X(s_cMmPic) X(s_cMmCopies) X(s_cMmRefused) X(s_cMfCarried)
#define PK_FC_FIELD(c) unsigned c;
#define PK_FC_MARK(c)  s_fcMark.c = c;
#define PK_FC_UNDO(c)  c = s_fcMark.c;
static struct { PK_FILL_COUNTS(PK_FC_FIELD) } s_fcMark;
static uint32_t s_fillHead;

/* the in-play frame: every field of the header, from the thread that owns it */
static unsigned fill_frame(TAGPU_PACKET* p, void* ctx)
{
    const char* ta = ta_main();
    unsigned cursor = sizeof(TAGPU_PACKET), need = sizeof(TAGPU_PACKET), e, nplace = 0;
    uint32_t wlen = 0;
    int L, T, W, H;
    (void)ctx;

    /* zero everything the header carries, then fill; the tail area is bounded
       by cap_bytes, which the primitive set before calling us */
    tagpu_pk_fill((unsigned char*)p + offsetof(TAGPU_PACKET, used_bytes), 0,
                  sizeof(TAGPU_PACKET) - offsetof(TAGPU_PACKET, used_bytes));
    /* STAMPED HERE, NOT AT THE RETURN: the `!ta` exit below
       returns early, and stamping only at the end would leave s_lastFilled
       naming an EARLIER packet while the publish still reported success --
       roster_log would then print a live `units: alive=` block from the
       previous packet's world, which is the very line `tacli scenario load`
       treats as proof the map is live. Stamped here the header is already
       zeroed, so in_game is 0 and roster_log returns at its guard. */
    s_lastFilled = p;
    s_fillShort = 0;
    s_fillAgain = p->head_seq == s_fillHead;
    if (s_fillAgain) { PK_FILL_COUNTS(PK_FC_UNDO) }
    else { s_fillHead = p->head_seq; PK_FILL_COUNTS(PK_FC_MARK) }
    p->used_bytes = sizeof(TAGPU_PACKET);
    p->text_fg = -1;
    p->gamma = 1.0f;
    if (!ta) return need;                                  /* in_game stays 0: fail closed */

    p->in_game   = 1;
    p->tick      = RDU32(ta, OFF_GAMETIME);
    if (!s_haveTick || p->tick != s_lastTick) {
        QueryPerformanceCounter(&s_tickStart);
        s_lastTick = p->tick; s_haveTick = 1;
    }
    p->tick_start_lo = s_tickStart.LowPart;
    p->tick_start_hi = (uint32_t)s_tickStart.HighPart;
    p->level_gen   = s_levelGen;
    /* what the command apply in `before` had done by this draw: the render
       thread reconciles its prediction against these */
    tagpu_zoom_applied(&p->cmd_ack_seq, &p->cmd_ack_dx, &p->cmd_ack_dy, &p->zoom_applied, &p->cmd_epoch,
                       &p->cam_centre);
    /* the replay's Follow the same apply wrote the eye for, read below */
    tagpu_zoom_follow_pub(&p->follow_epoch, &p->follow_level, &p->follow_last);
    p->gui_flips   = tagpu_gui_flips();
    p->draw_seq    = s_cDraws;
    p->eye[0]       = RD32(ta, OFF_EYE_X);      p->eye[1]       = RD32(ta, OFF_EYE_Y);
    p->scroll_to[0] = RD32(ta, OFF_SCROLLTO_X); p->scroll_to[1] = RD32(ta, OFF_SCROLLTO_Y);
    tagpu_vpwide_true_rect(ta, &L, &T, &W, &H);
    p->vp[0] = L; p->vp[1] = T; p->vp[2] = W; p->vp[3] = H;
    /* the rect the engine can NAME, as its field stands after `before` wrote
       it: the true rect, or the widened one at zoom < 1 */
    p->vp_addr[0] = RD32(ta, OFF_VP_L); p->vp_addr[1] = RD32(ta, OFF_VP_T);
    p->vp_addr[2] = RD32(ta, OFF_VP_R); p->vp_addr[3] = RD32(ta, OFF_VP_B);
    p->screen[0] = RD32(ta, OFF_SCREEN_W);  p->screen[1] = RD32(ta, OFF_SCREEN_H);
    fill_pal(p, ta);
    p->map_pxw = RD32(ta, OFF_MAP_PXW);     p->map_pxh = RD32(ta, OFF_MAP_PXH);
    p->map_w16 = RD32(ta, OFF_MAP_W16);     p->map_h16 = RD32(ta, OFF_MAP_H16);
    p->view_cells[0] = RD32(ta, OFF_VIEWCELLS_W); p->view_cells[1] = RD32(ta, OFF_VIEWCELLS_H);
    p->udef_count = RDU32(ta, OFF_UDEFCOUNT);
    p->unit_slots = RDU16(ta, OFF_UNITSLOTS);
    p->los_type   = RDU16(ta, OFF_LOSTYPE);
    p->gfx_opt    = RDU8(ta, OFF_GFXOPT);
    p->ui_gates   = RDU8(ta, OFF_UIGATES);
    p->load_flags = RDU16(ta, OFF_LOADFLAGS);
    p->local_player = RDU8(ta, OFF_LOCALPLAYER);
    {
        const int lg = player_logo(ta, p->local_player);
        p->local_logo = lg < 0 ? TAGPU_PK_LOGO_NONE : (unsigned char)lg;
    }
    p->watched      = RDU8(ta, OFF_WATCHED);
    p->sea_level    = RDU8(ta, OFF_SEALEVEL);
    p->paused       = RDU8(ta, OFF_GAMEPAUSED) & 1u;
    p->game_speed   = RDI16(ta, OFF_GAMESPEED_LIVE);
    p->text_fg      = s_textFg;
    /* what the marker pass reads, so that it takes nothing from engine memory
       itself: the GUI colour bytes, the dispatched mouse point, the build
       cursor's two corners and the two mode bytes that gate them */
    tagpu_pk_copy(p->gui_col, ta + OFF_GUICOL, sizeof p->gui_col);
    p->mouse[0]     = RD32(ta, OFF_MOUSE_X);
    p->mouse[1]     = RD32(ta, OFF_MOUSE_Y);
    {
        int k;
        for (k = 0; k < 6; k++) p->build_rect[k] = RD32(ta, OFF_BUILDRECT + k * 4);
    }
    p->cursor_mode  = RDU8(ta, OFF_CURMODE);
    p->region_flags = RDU8(ta, OFF_REGIONFL);
    p->build_unit_id = RDU16(ta, OFF_BUILDUNITID);
    p->game_opt     = RDU8(ta, OFF_GFXOPT);
    p->resource_queued = 0;
    tagpu_profiles_begin_match();
    tagpu_resource_packet(p);

    /* THE ORDER IS THE CUT ORDER. A fill that does not fit the reserve is
       published cut (tagpu_packet.c, pkx_publish), and what is cut is what
       comes last. So the fog source and the shade table go down first, the
       world tables straight after them, and then the map's own features while
       the mirror is taking them -- the reserve holds all of those at the
       design point (tagpu_packet.c, PK_RESERVE) -- and what an overflow can
       reach is what follows: the placement's sites, the build orders, the
       replay's overlays, the effects, the UI's render half, the font, and last
       the effects models' five tables, the one layer
       an engine count can drive past the design point (fill_fxmodels). A
       frame without its fog source draws no unit but the watched player's own
       and no wreck, one without its
       unit tables drops its whole unit hand-over, and one without its
       map-feature table leaves the mirror off another packet; one without an
       effect layer, the minimap or the models is missing that alone. */
    /* ---- the tables' reach, then the fog source ---- */
    table_reach(p);
    e = fill_fog(p, &cursor);
    if (e > need) need = e;
    /* ---- the shade table the unit pass shades by ---- */
    shd_snapshot();
    if (s_shdOk) {
        e = append_area(p, &cursor, s_shd, (unsigned)sizeof s_shd, &p->shd_off, &p->shd_len,
                        TAGPU_PK_TRUNC_SHD);
        if (e > need) need = e;
    }
    /* ---- the world tables ---- */
    e = fill_world(p, ta, &cursor);
    if (e > need) need = e;
    /* ---- the map's own features, until the mirror holds them: inside the
       reserve's design point with the world tables (tagpu_packet.c), so a cut
       reaches them only past that point -- unit or wreck models over the 36
       and 19 pieces it assumes. A cut costs the mirror
       time, never trees: the table lands whole or `mapfeat_ok` stays 0, the
       feature pass holds no copy, the sync answers 0 and the edge stays black
       until a packet carries the table whole. It rides only a level's first
       packets ---- */
    e = fill_mapfeat(p, &cursor);
    if (e > need) need = e;
    /* ---- the placement's sites: 7 KB at most, before the builds table
       because the ghost masks fill_builds writes cover the sites' types ---- */
    e = fill_place(p, &cursor, &nplace);
    if (e > need) need = e;
    /* ---- the reclaim sweep: 26 KB at most (the header and 1281 items of 20 B) ---- */
    e = fill_sweep(p, &cursor);
    if (e > need) need = e;
    /* ---- the formation move's dots: 18 KB at most (1500 of 12 B) ---- */
    e = fill_form(p, &cursor);
    if (e > need) need = e;
    /* ---- the weapon range rings: 128 KB at most (4096 of 32 B) ---- */
    e = fill_range(p, &cursor);
    if (e > need) need = e;
    /* ---- the build-orders table (the ghost pass) ---- */
    e = fill_builds(p, &cursor, nplace);
    if (e > need) need = e;
    /* ---- the replay's overlays: 2 KB at most ---- */
    e = fill_overlay(p, &cursor);
    if (e > need) need = e;
    /* ---- the effects and the particle layers ---- */
    e = fill_fx(p, ta, &cursor);
    if (e > need) need = e;
    /* ---- the UI's render half ---- */
    e = fill_gui(p, ta, &cursor);
    if (e > need) need = e;
    /* ---- the replay HUD's record, while a recording plays (tagpu_hudstate.c) ---- */
    if (tagpu_hudstate_fill(&s_hud, p->tick)) {
        e = append_area(p, &cursor, &s_hud, (unsigned)sizeof s_hud, &p->hud_off, &p->hud_len,
                        TAGPU_PK_TRUNC_HUD);
        if (e > need) need = e;
        if (tagpu_hudstate_fill_stats(&s_hudstat, &s_hud)) {
            e = append_area(p, &cursor, &s_hudstat, (unsigned)sizeof s_hudstat, &p->hstat_off,
                            &p->hstat_len, TAGPU_PK_TRUNC_HUD);
            if (e > need) need = e;
        }
    }

    /* the font: header + glyph table in the header, the objects in the area */
    p->font_gen   = s_fontGen;
    p->font_rows  = s_fontRows; p->font_yoff = s_fontYoff; p->font_first = s_fontFirst;
    tagpu_pk_copy(p->font_glyph, s_glyph, sizeof s_glyph);
    if (s_fontTrunc) p->truncated |= TAGPU_PK_TRUNC_FONT;
    e = append_area(p, &cursor, s_fontArea, s_fontLen, &p->font_off, &p->font_len, TAGPU_PK_TRUNC_FONT);
    if (e > need) need = e;
    if (!p->font_len) {
        tagpu_pk_fill(p->font_glyph, 0, sizeof p->font_glyph);            /* no area: no glyphs */
        /* the area did NOT FIT this packet (a copy exists on our side): carry
           generation 0 so the consumer keeps the copy it has and takes the
           next packet that fits, rather than latching this generation as
           "no font" for as long as the engine keeps the same font. A REFUSED
           or empty font (s_fontLen == 0) keeps its generation: the consumer
           must blank, and stay blank. */
        if (s_fontLen) p->font_gen = 0;
    }

    /* stress: a dummy table past a one-page commit, so the growth path runs */
    if (s_stress) {
        e = append_area(p, &cursor, NULL, 6144u, &p->stress_off, &p->stress_len, TAGPU_PK_TRUNC_STRESS);
        if (e > need) need = e;
    }
    /* ---- the effects models ---- */
    e = fill_fxmodels(p, &cursor);
    if (e > need) need = e;
    /* ---- the in-game wiki's record, LAST: it can grow without moving or cutting a table
       before it (tagpu_wikipk.h) ---- */
    if (tagpu_wiki_fill(&s_wiki, &wlen, s_fillAgain, wiki_side(ta), wiki_iface(ta))) {
        e = append_area(p, &cursor, &s_wiki, wlen, &p->wiki_off, &p->wiki_len, TAGPU_PK_TRUNC_WIKI);
        if (e > need) need = e;
        /* THE STAGE IS CUT BEFORE THE PAGE: a whole record that does not fit is placed as its head,
           so the window is still drawn and can be closed. The shortfall stays the whole record's,
           so the growth aims at the stage; the head's own refusal adds nothing to it. A head that
           does not fit either carries no record, and the wiki closes the window at the next
           publish's fill (tagpu_wiki.h). */
        if (!p->wiki_len && wlen > TAGPU_PK_WIKI_HEAD) {
            const unsigned short_was = s_fillShort;
            append_area(p, &cursor, &s_wiki, tagpu_wiki_head_only(&s_wiki), &p->wiki_off, &p->wiki_len,
                        TAGPU_PK_TRUNC_WIKI);
            s_fillShort = short_was;
        }
        tagpu_wiki_carried(&s_wiki, p->wiki_len);
    }
    p->used_bytes = cursor;
    /* the whole fill's size when a table did not fit (s_fillShort) */
    if (s_fillShort && PKT_ALIGN4(cursor) + s_fillShort > need) need = PKT_ALIGN4(cursor) + s_fillShort;
    return need;
}

/* ===================== THE LIVE-STATE LOG tacli READS =====================
   Three lines, and with `peek:` they are the whole of what `tacli` greps out
   of tagpu.log:

     `units: alive=N onscreen=N eye=(x,y) me=N`  -- `tacli roster`'s header,
        and the signal `tacli scenario load` waits on to decide a map is live;
     `  uNNN <name> own=N idx=N world=(...) screen=(...) nano=...` -- the
        roster itself, which is how a headless harness steers to a unit;
     `mouse: screen=(x,y)` -- TA's own pointer, read without touching the
        user's.

   They are emitted here, on the game thread, and not by a renderer: only
   render_vk.c reaches the overlay, so on the GDI backend a log
   there would never appear, `tacli roster` would answer nothing and `tacli
   scenario load` would TIME OUT -- with the game behind it running perfectly
   well. That was measured rather than assumed: `tacli scenario apply` against
   an instance `load` had declared dead applied 5 of 5. Only the detector was
   missing, which is why this is one function and not a subsystem.

   GATED IN MILLISECONDS, NOT IN FRAMES, and that is the whole of the care
   this needs. This runs from the in-play draw, measured at 13 361 draws/s on
   the reference setup, so a frame modulo sized for ~60 render frames a second
   would raise the file-write rate about two hundredfold on the game thread
   inside an engine call. The cadences below are 30, 300 and 15 render frames
   at 60/s, restated as time. */
#define ROSTER_HDR_MS    500u   /* `units:`                              */
#define ROSTER_DUMP_MS  5000u   /* the dump                              */
#define ROSTER_MOUSE_MS  250u   /* `mouse:`                              */

static LARGE_INTEGER s_rosFreq, s_rosNow, s_rosFill, s_rosHdr, s_rosDump, s_rosMouse;
/* QPF is asked ONCE. Without this a refusal would re-probe on every in-play
   draw for the life of the process, and say nothing. */
static int s_rosFreqTried;

static int ros_due(const LARGE_INTEGER* last, unsigned ms)
{
    if (!last->QuadPart) return 1;
    return (s_rosNow.QuadPart - last->QuadPart) * 1000 >=
           (LONGLONG)ms * s_rosFreq.QuadPart;
}

/* WHY THE PUBLISH BELOW IS SOMETIMES FORCED. `fill_frame` runs only when a
   publish is not skipped, and the FRESH gate skips whenever the renderer has
   not taken the last packet. `tagpu_packet_acquire` has exactly one call
   site -- render_vk.c -- so on the GDI backend NOTHING
   takes, every unforced publish is skipped, and `fill_frame` runs about once
   per level. Hanging the roster off the packet without this would produce
   nothing on the GDI backend, and produce it silently.

   IT ASKS WHEN THE LAST FILL WAS, not when a particular line is next due.
   Against the last FILL it is self-limiting for the right reason: on a lane
   whose renderer takes packets the fills are ~16 ms apart, so this is never
   due and never forces.

   AND IT ASKS THE HEADER'S CADENCE, WHICH IS THE SLOWEST OF THE TWO THAT
   MATTER. The MOUSE's 250 ms -- the shortest gate any line has -- would double
   the forced-publish rate to serve `mouse:`, and NOTHING PARSES `mouse:`: `tacli`'s
   structured readers are the peek line, the roster line and `units:`, and
   the only other reference in the tree is a sentence of prose in
   input-firewall.md. It is a human diagnostic. So the force runs at
   ROSTER_HDR_MS, `units:` and the roster keep their exact cadences on every
   lane, and `mouse:` runs at 500 ms rather than 250 ms on a lane with no
   consumer. That asymmetry is deliberate and it costs a diagnostic nothing.

   It stamps the shared clock, so the whole draw costs one
   QueryPerformanceCounter. */
static int roster_wants_fill(void)
{
    if (s_rosFreqTried) {
        if (!s_rosFreq.QuadPart) return 0;        /* fail closed, and stop asking */
    } else {
        s_rosFreqTried = 1;
        QueryPerformanceFrequency(&s_rosFreq);
        if (!s_rosFreq.QuadPart) {
            plog("roster: QueryPerformanceFrequency refused -- `tacli roster` and "
                 "`tacli scenario load` cannot work in this process");
            return 0;
        }
    }
    QueryPerformanceCounter(&s_rosNow);
    return ros_due(&s_rosFill, ROSTER_HDR_MS);
}

static void roster_log(const TAGPU_PACKET* pk)
{
    const TAGPU_PK_UNIT* uu;
    unsigned i;
    int alive = 0, onscreen = 0, eyeX, eyeY, gw, gh, me, wantHdr, wantDump;
    TLOG_BLOCK* dump;

    if (!s_rosFreq.QuadPart || !s_rosNow.QuadPart) return;
    /* STAMPED BEFORE THE in_game GUARD, AND THAT ORDER IS LOAD-BEARING. It
       records that a FILL happened, which is the only
       question roster_wants_fill asks. Move it below the guard and a persistent
       !in_game fill would leave s_rosFill frozen, so the force would be due on
       EVERY in-play draw -- a forced publish at the full draw rate, on the game
       thread inside an engine call. */
    s_rosFill = s_rosNow;
    if (!pk || !pk->in_game) return;

    /* the eye the world was drawn with: the roster's screen= is the 1x
       projection about it */
    eyeX = pk->eye[0]; eyeY = pk->eye[1];
    /* `me=` is main+0x2A42, the order driver's player -- the byte this log has
       always printed, NOT the bar loop's main+0x2A43. The packet carries both
       and they are written independently (the exe note's marker-block
       section), so the wrong one would change what tacli reads. */
    me = pk->watched;
    gw = g_ddraw.width  > 0 ? (int)g_ddraw.width  : 640;
    gh = g_ddraw.height > 0 ? (int)g_ddraw.height : 480;

    if (ros_due(&s_rosMouse, ROSTER_MOUSE_MS)) {
        int mx = pk->mouse[0], my = pk->mouse[1];
        s_rosMouse = s_rosNow;
        if (mx >= -50 && mx <= 4000 && my >= -50 && my <= 4000) {
            char b[96]; _snprintf(b, sizeof b, "mouse: screen=(%d,%d)", mx, my);
            b[sizeof b - 1] = 0; plog(b);
        }
    }

    wantHdr  = ros_due(&s_rosHdr,  ROSTER_HDR_MS);
    wantDump = ros_due(&s_rosDump, ROSTER_DUMP_MS);
    if (!wantHdr && !wantDump) return;   /* the walk is only for these two */
    if (wantHdr)  s_rosHdr  = s_rosNow;

    /* ONE BLOCK FOR THE WHOLE DUMP: one write on the GAME thread, where a stall
       costs sim time, and never split across a rotation, so `tacli roster`
       finds the whole dump in one file. The loop is bounded by
       TAGPU_PK_MAX_UNITS (16384), far inside a block's cap. */
    dump = wantDump ? tagpu_log_block_begin(TLOG_MAIN) : NULL;
    /* THE CLOCK IS STAMPED BY THE BLOCK, NOT BY THE INTENT. A block that could
       not start (no memory) leaves the gate due and the next fill retries,
       instead of putting the next attempt 5 s away while the `units:` line
       below still prints -- which `tacli roster` would read as a header with
       no roster lines, "no roster lines yet (needs a running game)", for a
       game that is running. It says once that it happened. */
    if (wantDump) {
        if (dump) s_rosDump = s_rosNow;
        else {
            static int moaned;
            if (!moaned) { moaned = 1; plog("roster: no block for the unit dump"); }
        }
    }
    uu = tagpu_pk_units(pk);
    for (i = 0; i < pk->n_units; i++) {
        const TAGPU_PK_UNIT* u = &uu[i];
        int wx = (int)(short)(u->pos[0] >> 16);
        int wz = (int)(short)(u->pos[1] >> 16);
        int wy = (int)(short)(u->pos[2] >> 16);
        int sx = wx - eyeX + 128;
        int sy = wy - (wz / 2) - eyeY + 32;
        alive++;
        /* the full dump: index, type, owner, position. idx = UnitInGameIndex
           (+0xA8), the engine's own slot. It is RECYCLED on death, so it is
           never a public identity -- but it is what `tacli scenario` reports
           per spawned entity, so the roster has to speak the same number for
           the two to be comparable. */
        if (dump)
            tagpu_log_blockf(dump,
                "  u%03d %-12.12s own=%d idx=%d world=(%d,%d,%d) screen=(%d,%d) nano=%.2f",
                alive, u->name[0] ? u->name : "?", (int)u->owner, (int)u->id,
                wx, wy, wz, sx, sy, u->nano);
        if (sx >= -gw / 40 && sx <= gw + gw / 40 && sy >= -gh / 40 && sy <= gh + gh / 40)
            onscreen++;
    }
    tagpu_log_block_end(dump);
    if (wantHdr) {
        char b[160]; _snprintf(b, sizeof b, "units: alive=%d onscreen=%d eye=(%d,%d) me=%d",
                               alive, onscreen, eyeX, eyeY, (int)me);
        b[sizeof b - 1] = 0; plog(b);
    }
}

/* the level end: header only, in_game = 0, the new generation (and the
   palette, which the shell's passes may still resolve through) */
static unsigned fill_level_end(TAGPU_PACKET* p, void* ctx)
{
    unsigned gen = *(const unsigned*)ctx;
    tagpu_pk_fill((unsigned char*)p + offsetof(TAGPU_PACKET, used_bytes), 0,
                  sizeof(TAGPU_PACKET) - offsetof(TAGPU_PACKET, used_bytes));
    p->used_bytes = sizeof(TAGPU_PACKET);
    p->in_game    = 0;
    p->level_gen  = gen;
    p->tick       = s_lastTick;
    p->load_flags = (unsigned short)load_flags();
    p->text_fg    = -1;
    p->gamma      = 1.0f;
    tagpu_zoom_applied(&p->cmd_ack_seq, &p->cmd_ack_dx, &p->cmd_ack_dy, &p->zoom_applied, &p->cmd_epoch,
                       &p->cam_centre);
    fill_pal(p, ta_main());
    return sizeof(TAGPU_PACKET);
}

void tagpu_packet_pub_level_end(unsigned level_gen)
{
    char b[200];
    unsigned flags = load_flags();
    /* the argument is reclaim's counter when reclaim is the provider, kept for
       the log; ours is what the packet carries and what every consumer keys on */
    s_levelGen++;
    /* THE GOLDEN SOURCE'S DROP IS ABOVE THE THREAD CHECK, and deliberately.
       Below it, a teardown seen on a foreign thread -- which this function
       counts and returns from -- would leave the previous level's reference
       live with no line saying so. The level ended
       whoever noticed; the drop is one atomic increment and needs no thread
       identity, and the render thread is what acts on it.

       NO FOREIGN TEARDOWN HAS EVER BEEN OBSERVED and this line is therefore
       belt-and-braces -- said so that nobody reads it as evidence that one
       happens. The survey is in research/notes/engine/memory.md, the level teardown
       0x491B60, "Who enters it": the teardown's address is never taken, so its six call sites
       are the complete set; but all six are dispatched through the engine's
       state-handler pointer, so the call graph cannot settle the thread and
       the answer is the measurement -- four teardowns over two UI paths, all
       on the game thread, plus the `tdforeign=` instrument that re-checks it
       every run. The other provider of this function, `after_teardown`
       below, is game-thread-only by construction (its `before` returns 0 off
       the game thread, and the stub skips the `after` when it does). One
       atomic increment is worth not having to re-derive that. */
    tagpu_surf_level_end();
    if (!on_game_thread()) {
        s_cForeign++;
        _snprintf(b, sizeof b, "packet: level end on thread %u, not the game thread %u — NOT published (foreign=%u)",
                  (unsigned)GetCurrentThreadId(), (unsigned)s_gameTid, s_cForeign);
        b[sizeof b - 1] = 0; plog(b);
        return;
    }
    /* the level's camera state is handed back here, on the game thread, before
       the shell draws: the engine's own range flag, viewport rect and scroll
       rate — no in-play draw will apply a command until the next level's
       first one, and the shell must not inherit a widened rect */
    if (s_installed && !s_countOnly) {
        char* ta = (char*)ta_main();
        tagpu_resource_level_end();
        tagpu_place_level_end();
        tagpu_sweep_level_end();
        tagpu_formation_level_end();
        tagpu_rangeown_level_end();
        if (ta) { tagpu_zoom_level_end(ta); tagpu_vpwide_level_end(ta); }
        /* and the terrain latch with it: the shell, the next level's loading
           draws and any screenshot draw before its first in-play one never
           latch, so without this they would run our key fill and fog tick on
           this level's decision. Only a rect `restore` could not put back
           keeps it up. */
        tagpu_terrown_latch(tagpu_vpwide_wide());
        tagpu_packet_publish(fill_level_end, &s_levelGen, 1 /* past the FRESH gate */);
    }
    _snprintf(b, sizeof b, "packet: level end -> gen %u (reclaim's %u): in_game=0 published%s; %u in-play draw(s) this level; load flags 0x%04X",
              s_levelGen, level_gen, (s_installed && !s_countOnly) ? "" : " (NOT: module off)", s_levelDraws, flags);
    b[sizeof b - 1] = 0; plog(b);
    s_levelOpen = 0; s_levelDraws = 0; s_shellSeen = 0; s_shellFlags = 0xFFFFu;
    /* the anchor cache names cells of the level that is going away, and the
       effect cache names its model templates */
    s_aHave = 0; s_aN = 0; s_aTrunc = 0;
    s_fxHave = 0; s_nProj = s_nExpl = s_nDebris = s_nPart = 0;
    /* the next level's picture is a different picture, and it has not been sent */
    s_mmPicGen = -1; s_mmPicW = s_mmPicH = 0;
    /* and its features are a different map's (keyed on the level either way) */
    s_mfHeld = 0; s_mfN = 0; s_mfTrunc = 0;
    /* and the icons' type table its types */
    utype_level_end();
}

/* ---- the observers ------------------------------------------------------- */

static int __cdecl before_draw(void* entry_esp)
{
    unsigned ret = ((unsigned*)entry_esp)[0];
    if (!on_game_thread()) { s_cForeign++; return 0; }
    s_cDrawsAll++;
    if (ret != VA_DRAW_RET_INPLAY) {
        /* the shell, the loading screen, the screenshot sweep: never publish,
           never apply. Record the loader's flag word once per level for the
           log — the first such draw after a teardown is the loading screen. */
        if (!s_levelOpen && !s_shellSeen) { s_shellFlags = load_flags(); s_shellSeen = 1; }
        /* a draw we do not track may rebuild the engine's fog grid at an eye
           of its own (the screenshot sweep drives the eye): the record goes,
           and so does a "before" reading a tracked draw around it took */
        fog_rec_drop();
        return 0;
    }
    s_cDraws++;
    s_levelDraws++;
    if (s_countOnly) { fog_rec_drop(); return 0; }
    if (s_retDepth >= RET_DEPTH) { s_cDeep++; fog_rec_drop(); return 0; }
    s_retStack[s_retDepth++] = (void*)(size_t)ret;
    /* THE COMMANDS, on the thread that owns every word they write. This
       runs after whichever of the frame callback's own camera writers ran
       this frame — the stepper 0x41CA10 (0x495599 inside 0x495490, called at
       0x49680C/0x49693E) and the scroll poll 0x41CE90 (0x496976) both precede
       the draw call at 0x4969CD, and both can be skipped: the stepper when the
       sim is paused, both under an in-game GUI screen — and before the draw's
       first read of the eye at 0x468DD9; no ENGINE store to the eye exists
       inside DrawGameScreen (whose extent is 0x468CF0..0x46A3FD; the tail past
       0x46A200 is status icons, the clock, the GUI blit, the profiler bars,
       the options tab, the cursor and the flip, and none of them stores the
       eye), and none of ours either: the scenario camera is handed to this
       apply (tagpu_zoom_place_eye) — so a delta applied here composes
       with the engine's own camera move and the draw that follows reads the
       commanded eye; its fog rebuild and its minimap box see it too. The
       latest record is taken and every part of it applied by the module that
       owns the field; a record already applied re-applies only its levels.
       `done` on every path, like the frame packet's frame_end. Timed with the
       performance counter like the publish: the heartbeat's `applyus`. */
    {
        char* ta = (char*)ta_main();
        const TAGPU_CMD* c;
        LARGE_INTEGER t0, t1;
        unsigned us;
        QueryPerformanceCounter(&t0);
        c = tagpu_cmd_take();
        /* the replay HUD's controls as the player last saw them, for a click before the next take */
        tagpu_hudinput_take(c ? &c->hud : NULL);
        /* the reclaim popup as last drawn, for a click before the next take */
        tagpu_sweep_take(c ? &c->sweep : NULL);
        tagpu_wiki_take(c ? &c->wiki : NULL);
        /* THE CAMERA AND THE RECT, THEN THE TERRAIN LATCH: the eye may only
           be off the engine's range, and the rect only wide, on a draw whose
           ground is ours. The render thread's request is read ONCE, the camera
           range centres and the rect widens only on it, and the stubs are
           latched on it OR on a rect that is still wide (a `restore` that
           could not run) -- after both are final, on the one thread that
           writes any of them, before this draw reaches `0x468DB0`. See
           `g_terrown_own` and tagpu_zoom.c's "the camera's range". */
        {
            int terr = tagpu_terrown_request();
            if (ta) {
                const TAGPU_CMD* view = tagpu_zoom_cut_gate(c);
                tagpu_zoom_apply(ta, view, terr);
                tagpu_vpwide_apply(ta, view, terr);
            }
            tagpu_terrown_latch(terr || tagpu_vpwide_wide());
            /* LAST, after every write of the eye and of LosType this draw
               makes before it draws: the engine fog grid's build record */
            if (ta) fog_rec_before(ta);
        }
        tagpu_cmd_done();
        QueryPerformanceCounter(&t1);
        us = (s_freq.QuadPart && t1.QuadPart >= t0.QuadPart)
             ? (unsigned)(((t1.QuadPart - t0.QuadPart) * 1000000LL) / s_freq.QuadPart) / 2u : 0u;
        s_applyHist[us < APPLY_HIST_N ? us : APPLY_HIST_N]++;
    }
    return 1;
}

static void* __cdecl after_draw(unsigned int* regs)
{
    void* ret = s_retDepth > 0 ? s_retStack[--s_retDepth] : NULL;
    (void)regs;
    /* the draw is done: did its fog call rebuild the engine's grid -- which
       is what the wide tick is told as `rebuilt` */
    fog_rec_after(ta_main());
    /* the fog source, when our fog site did not pack it in this draw */
    wide_tick();
    /* post-flip: the packet. The FRESH gate inside makes most of these a load.
       FORCED WHILE THE LEVEL HAS NO PACKET YET: the shell's cursor
       channel publishes through the load, so the FRESH gate can be set at the
       instant this draw runs — and a first packet the gate dropped would leave
       the renderer on an in_game = 0 packet, with no world, until the render
       thread happened to take one in between. One forced publish per level for
       THAT reason, counted as an overrun exactly like the level-end packet's.

       IT IS NOT THE ONLY FORCED ONE. roster_wants_fill()
       forces a fill whenever none has happened for ROSTER_HDR_MS, so on a lane
       with no consumer -- the GDI backend, where nothing calls
       tagpu_packet_acquire -- this forces about twice a second and `overrun`
       counts every one; gpu-status.md's exchange health rule says so. */
    /* THE MAP'S OWN FEATURES, BEFORE THE PUBLISH AND OUTSIDE ITS GATE: a
       publish can be skipped, the adoption cannot, and the list it adopts was
       taken during the load, before anything played on the map. */
    mapfeat_adopt();
    {
        const int pub = tagpu_packet_publish(fill_frame, NULL,
                                             !s_levelOpen || roster_wants_fill());
        if (pub && !s_levelOpen) {
            char b[260];
            s_levelOpen = 1;
            _snprintf(b, sizeof b,
                      "packet: level gen %u: first in-play packet at draw #%u (tick %u, load flags now 0x%04X, at the first non-in-play draw after the teardown 0x%04X, loader thread %u entered %u time(s), game thread %u)",
                      s_levelGen, s_cDraws, s_lastTick, load_flags(), s_shellFlags,
                      (unsigned)s_loaderTid, s_loaderEntries, (unsigned)s_gameTid);
            b[sizeof b - 1] = 0; plog(b);
        }
        /* the live-state log tacli reads, from the packet just filled */
        if (pub) roster_log(s_lastFilled);
    }

    /* THE GOLDEN SOURCE, ON THE THREAD THAT DREW IT. TA's composed
       frame is copied here and nowhere else. This is the one point in the
       process where it is COMPLETE BY CONSTRUCTION rather than by timing: the
       flip `0x4C63A0` is called from inside this very function at `0x46A3DB`,
       34 bytes before its `ret` at `0x46A3FD`, so by the time the observer's
       `after` runs the engine has finished writing the primary -- and the
       thread that writes it is this one, which is not running anywhere else.
       A copy on the RENDER thread has nothing sequencing it against the
       engine, and can publish a frame torn between the engine's terrain rows
       and its side-panel rows (measured at such a site, in
       `tagpu_overlay_draw`: 223 of 16 500 reads came back torn).

       THAT CALL IS CONDITIONAL, AND WHAT MAKES IT CERTAIN HERE IS THE
       RETURN-ADDRESS FILTER RATHER THAN THE CALL'S PRESENCE [re-disassembled
       from the pristine exe 2026-09-20]. Two guards
       stand over it -- `0x46A3CC test ebx,ebx / je 0x46A3E0` on argument 1
       (`drawUnits`; the note explains why that argument lives in `ebx`) and
       `0x46A3D0 mov 0x22c(%esp),%eax / test eax,eax / je 0x46A3E0` on
       argument 2 (`blitScreen`) -- so a `DrawGameScreen` that draws without
       presenting falls straight through to the `ret`. What satisfies both on
       every draw we admit is `before_draw`'s `ret != VA_DRAW_RET_INPLAY`
       test: that return address, `0x4969D2`, belongs to the single call site
       `0x4969CD` (`push ebx; push ebx` at `0x4969CB`, `ebx` set to 1 at
       `0x4967CF` and never rewritten in between), i.e. `DrawGameScreen(1, 1)`.
       The other three call sites are the movie recorder and the screenshot
       function, and the filter excludes them -- exe-reverse-engineering.md,
       "its arguments, and the branch that skips hook 9". **So a new in-play
       call site passing `blitScreen = 0` would reach this `after` with no
       flip having run, and the capture would silently take the previous
       frame.** Stated here because the filter is what carries it.

       IT IS NOT A GUARANTEE THAT THE REFERENCE AND THE PACKET ARE THE SAME
       FRAME. The publish above has its own FRESH gate and the
       capture below has the render thread's request gate, and they are
       independent: a draw can publish without capturing and capture without
       publishing. The render thread also acquires the packet at the TOP of its
       loop and syncs the snapshot after, so the frame it draws can render
       packet D-1 against reference D. `stamp` carries the draw number the
       snapshot was taken at, and it is how a comparison CHECKS the pairing --
       nothing gates on it.

       AFTER the publish, deliberately: the packet is what the render thread
       waits on, and the copy is most of a megabyte. The call returns at once
       unless the render thread has asked for a new one, which is what bounds
       this to one copy per presented frame. */
    tagpu_surf_capture(s_cDraws);
    return ret;
}

/* the loader thread's entry and exit: its identity, measured rather than
   inferred, and the ORDER of its end against the level's first in-play packet
   (the log is one append per line, so the file order is the time order).
   The thread entry returns into the CRT's thread-start wrapper; its return
   is hijacked exactly as DrawGameScreen's, depth one — loads never overlap. */
static void* volatile s_loaderRet;
static int __cdecl before_loader(void* entry_esp)
{
    char b[200];
    unsigned ret = ((unsigned*)entry_esp)[0];
    s_loaderTid = GetCurrentThreadId();
    s_loaderEntries++;
    _snprintf(b, sizeof b, "packet: loader thread %u entered 0x497C70 (entry #%u; game thread %u; load flags 0x%04X; level gen %u; %u in-play draw(s) so far)",
              (unsigned)s_loaderTid, s_loaderEntries, (unsigned)s_gameTid, load_flags(), s_levelGen, s_cDraws);
    b[sizeof b - 1] = 0; plog(b);
    /* one live loader at a time (0x497F40 creates it only on its first-entry
       path); the slot is claimed atomically all the same, so a second one
       could never be hijacked into the first one's return */
    if (InterlockedCompareExchangePointer((void* volatile*)&s_loaderRet, (void*)(size_t)ret, NULL) != NULL) return 0;
    return 1;
}

static void* __cdecl after_loader(unsigned int* regs)
{
    char b[200];
    void* ret = (void*)InterlockedExchangePointer((void* volatile*)&s_loaderRet, NULL);
    (void)regs;
    _snprintf(b, sizeof b, "packet: loader thread %u leaving 0x497C70 (load flags 0x%04X; level gen %u; %u in-play draw(s) so far; first in-play packet of this level %s)",
              (unsigned)GetCurrentThreadId(), load_flags(), s_levelGen, s_cDraws,
              s_levelOpen ? "ALREADY PUBLISHED" : "not yet");
    b[sizeof b - 1] = 0; plog(b);
    return ret;
}

/* ---- the map's own features, from the TNT (TAGPU_PK_MAPFEAT) -------------
   THE MIRROR IS THE MAP AS ITS TNT LAYS IT OUT, and the one place that list
   exists is LoadMap 0x483610, on the loader thread, while it holds the TNT.
   A fresh game places the TNT's features into the grid there; a SAVED game
   does not (the two `jne 0x483B53` on OFF_SAVEDGAME) -- the save's own records
   are restored into the grid later, by 0x424C00 from 0x432610 -- so no moment
   of a saved game's load has the TNT's features in the grid, and a grid read
   would mirror the saved game, burnt trees and all. So the list is BUILT FROM
   THE TNT, by LoadMap's own rules, at the join every path reaches
   (VA_LOADMAP_JOIN), on both paths alike: a fresh game and its save mirror
   the same features by construction.

   THE RULES ARE SpawnFeatureOnMap 0x423C50(cell, def, 0, 0, 0xA) and the
   FEATURES_Destroy 0x4246B0(cell, 0) it calls [DISASSEMBLED]:
   - a v2 map first marks every cell whose TNT feature is 0xFFFC as void;
     then, cell by cell in row order, every feature below LM_THRESH is spawned
     with the TNT's index as the FeatureDef index;
   - a spawn refuses a footprint (FD_FOOTX x FD_FOOTZ, from the anchor right
     and down) that runs past the map; then, footprint cell by footprint cell,
     anything that is not empty is destroyed -- a footprint cell (0xFFFE)
     resolves to its anchor through its dz/dx bytes -- and a destroy that is
     refused (a void or other marker at 0xFFFB and up, or a def with FD_MASKHI
     bit1) abandons the spawn, keeping what it destroyed so far;
   - a 3DO def (FD_MASK bit0 clear) then takes a wreck-pool record, and none
     left abandons it too; a destroy of one gives its record back;
   - the anchor takes the def, every other footprint cell 0xFFFE and its
     dz/dx, and a destroy clears the anchor and every 0xFFFE cell of the
     destroyed def's rectangle.
   What is kept is every GAF anchor that survives, row-major; the 3D ones are
   the wreck pool's and are not mirrored. The schema's own features, which
   0x423160 adds to a fresh v2 game after the TNT's, are not the TNT's and are
   not in it either.

   EVERY BOUND IS THE ENGINE'S OWN: the cell records are the W x H the engine
   has just read on this path (the height loop runs on both), a def is read
   only under the FeatureDef count 0x421F20 has just set -- the engine spawns a
   TNT index past it unchecked; we skip it and count it -- and every footprint
   index is inside the map by the spawn's own refusal. The working grid is
   ours, W x H words on the heap, freed before the join returns; W and H are
   the ones the engine allocated its own grid with (main+0x14233/0x14237). */
typedef struct {
    const char* defs;
    unsigned*   g;             /* per cell: the def, or 0xFFFF / 0xFFFC / 0xFFFE;
                                  a 0xFFFE cell's dz in bits 16..23, dx 24..31   */
    int         W, H, count;
    unsigned    pool, poolFull, refused, beyond, placed3d;
} MF_TNT;

#define MFT_EMPTY 0xFFFFu
#define MFT_VOID  0xFFFCu
#define MFT_FOOT  0xFFFEu

static int mft_destroy(MF_TNT* m, int c)
{
    unsigned v = m->g[c], d;
    const char* def;
    int fx, fz, dz, dx;
    if ((v & 0xFFFFu) == MFT_FOOT) {
        c -= (int)((v >> 16) & 0xFFu) * m->W + (int)(v >> 24);
        if (c < 0) return 0;             /* a mark is only ever made from its anchor */
        v = m->g[c];
    }
    d = v & 0xFFFFu;
    if (d >= 0xFFFBu) return 0;
    def = m->defs + (size_t)d * FD_STRIDE;     /* only a def under the count is placed */
    if (RDU8(def, FD_MASKHI) & 2u) return 0;
    if (!(RDU8(def, FD_MASK) & 1u) && m->pool) m->pool--;
    m->g[c] = MFT_EMPTY;
    fx = RDI16(def, FD_FOOTX); fz = RDI16(def, FD_FOOTZ);
    for (dz = 0; dz < fz; dz++)
        for (dx = 0; dx < fx; dx++) {
            unsigned* q = &m->g[c + dz * m->W + dx];
            if ((*q & 0xFFFFu) == MFT_FOOT) *q = MFT_EMPTY;
        }
    return 1;
}

static void mft_spawn(MF_TNT* m, int idx, unsigned d)
{
    const char* def;
    int col = idx % m->W, row = idx / m->W, fx, fz, dz, dx;
    if (d >= (unsigned)m->count) { m->beyond++; return; }
    def = m->defs + (size_t)d * FD_STRIDE;
    fx = RDI16(def, FD_FOOTX); fz = RDI16(def, FD_FOOTZ);
    if (col + fx > m->W || row + fz > m->H) { m->refused++; return; }
    for (dz = 0; dz < fz; dz++)
        for (dx = 0; dx < fx; dx++) {
            int c = idx + dz * m->W + dx;
            if ((m->g[c] & 0xFFFFu) != MFT_EMPTY && !mft_destroy(m, c)) { m->refused++; return; }
        }
    if (!(RDU8(def, FD_MASK) & 1u)) {
        if (m->pool >= (unsigned)WR_COUNT) { m->poolFull++; return; }
        m->pool++; m->placed3d++;
    }
    m->g[idx] = d;
    for (dz = 0; dz < fz; dz++)
        for (dx = 0; dx < fx; dx++)
            if (dz || dx)
                m->g[idx + dz * m->W + dx] = MFT_FOOT | ((unsigned)(dz & 0xFF) << 16) |
                                             ((unsigned)(dx & 0xFF) << 24);
}

/* LOADER THREAD, at VA_LOADMAP_JOIN. `regs` is the stub's pushad block and
   LoadMap's own frame follows it. Returns nothing the engine sees: the stub
   restores every register and runs the stolen bytes. */
static void __cdecl mapfeat_at_load(const unsigned int* regs)
{
    const char* f = (const char*)(regs + 8);
    const char* ta = ta_main();
    const unsigned char* v1 = *(const unsigned char* const*)(f + LM_V1RECS);
    const unsigned char* v2 = *(const unsigned char* const*)(f + LM_V2RECS);
    unsigned thresh = *(const unsigned*)(f + LM_THRESH);
    int version = *(const int*)(f + LM_VERSION);
    const char* grid;
    MF_TNT m;
    TAGPU_PK_MAPFEAT* e = NULL;
    unsigned n = 0, want = 0, idx, cells;
    int trunc = 0;
    LARGE_INTEGER t0, t1;
    char b[320];

    mapfeat_unpark();
    /* the mirror is a Vulkan-lane picture: under any other renderer nothing
       would take the list, so nothing is built */
    if (g_ddraw.renderer != vk_render_main || !ta) return;
    QueryPerformanceCounter(&t0);
    memset(&m, 0, sizeof m);
    m.W = RD32(ta, OFF_MAP_W16); m.H = RD32(ta, OFF_MAP_H16);
    m.count = RD32(ta, OFF_FEATCOUNT);
    m.defs = *(const char* const*)(ta + OFF_FEATDEF);
    grid = *(const char* const*)(ta + OFF_FEATMAP);
    if (m.W <= 0 || m.H <= 0 || m.W > 4096 || m.H > 4096 || !ptr_ok(grid) ||
        m.count < 0 || m.count > 4096 || (m.count && !ptr_ok(m.defs)) ||
        (version == 0x2000 ? !ptr_ok(v2) : version == 0x1020 ? (v1 && !ptr_ok(v1)) : 1)) {
        _snprintf(b, sizeof b, "packet: LoadMap: the map's own features NOT taken: map %dx%d, "
                  "%d defs, TNT version 0x%X (the mirror stays off this level)", m.W, m.H, m.count, version);
        b[sizeof b - 1] = 0; plog(b);
        return;
    }
    if (version == 0x2000) { v1 = NULL; if (thresh > 0xFFFBu) thresh = 0xFFFBu; }
    else { v2 = NULL; if (thresh > 0xFCu) thresh = 0xFCu; }
    cells = (unsigned)m.W * (unsigned)m.H;
    m.g = (unsigned*)malloc((size_t)cells * sizeof *m.g);
    if (!m.g) {
        _snprintf(b, sizeof b, "packet: LoadMap: the map's own features NOT taken: no %u KB for the "
                  "working grid (the mirror stays off this level)", (unsigned)(cells * sizeof *m.g >> 10));
        b[sizeof b - 1] = 0; plog(b);
        return;
    }
    for (idx = 0; idx < cells; idx++) m.g[idx] = MFT_EMPTY;
    if (v2) {
        for (idx = 0; idx < cells; idx++)
            if (*(const unsigned short*)(v2 + (size_t)idx * 4 + 1) == MFT_VOID) m.g[idx] = MFT_VOID;
        for (idx = 0; idx < cells; idx++) {
            unsigned d = *(const unsigned short*)(v2 + (size_t)idx * 4 + 1);
            if (d < thresh) mft_spawn(&m, (int)idx, d);
        }
    } else if (v1) {
        for (idx = 0; idx < cells; idx++) {
            unsigned d = v1[(size_t)idx * 8 + 2];
            if (d < thresh) mft_spawn(&m, (int)idx, d);
        }
    }
    for (idx = 0; idx < cells; idx++) {
        unsigned d = m.g[idx] & 0xFFFFu;
        if (d < (unsigned)m.count && (RDU8(m.defs + (size_t)d * FD_STRIDE, FD_MASK) & 1u)) want++;
    }
    if (want > TAGPU_PK_MAX_MAPFEAT) { want = TAGPU_PK_MAX_MAPFEAT; trunc = 1; }
    if (want) e = (TAGPU_PK_MAPFEAT*)malloc((size_t)want * sizeof *e);
    if (want && !e) {
        free(m.g);
        plog("packet: LoadMap: the map's own features NOT taken: no memory for the list "
             "(the mirror stays off this level)");
        return;
    }
    for (idx = 0; idx < cells && n < want; idx++) {
        unsigned d = m.g[idx] & 0xFFFFu;
        int col = (int)(idx % (unsigned)m.W), row = (int)(idx / (unsigned)m.W);
        const char* t;
        unsigned h, hr, hd, hrd;
        if (d >= (unsigned)m.count || !(RDU8(m.defs + (size_t)d * FD_STRIDE, FD_MASK) & 1u)) continue;
        /* the lift from the grid's heights, which LoadMap wrote from the same
           records on both paths before the join; the anchor table's edge
           clamps, so it is the projection's own height term */
        t = grid + (size_t)idx * FT_STRIDE;
        h  = RDU8(t, FT_HEIGHT);
        hr = (col + 1 < m.W) ? RDU8(t + FT_STRIDE, FT_HEIGHT) : h;
        if (row + 1 < m.H) {
            const char* t2 = t + (size_t)m.W * FT_STRIDE;
            hd  = RDU8(t2, FT_HEIGHT);
            hrd = (col + 1 < m.W) ? RDU8(t2 + FT_STRIDE, FT_HEIGHT) : hd;
        } else { hd = h; hrd = hr; }
        e[n].col = (unsigned short)col; e[n].row = (unsigned short)row;
        e[n].def = (unsigned short)d;
        e[n].lift = (unsigned char)((h + hr + hd + hrd) >> 3);
        e[n].pad = 0;
        n++;
    }
    free(m.g);
    tagpu_packet_pub_mapfeat_loaded(e, n, trunc);
    QueryPerformanceCounter(&t1);
    _snprintf(b, sizeof b, "packet: LoadMap (loader thread %u, level gen %u): the map's own features "
              "from its TNT: %u%s over %dx%d (v%d, %s game; %u spawn(s) refused, %u past the %d defs, "
              "%u 3D, %u refused a pool record) in %.2f ms",
              (unsigned)GetCurrentThreadId(), s_levelGen, n, trunc ? " (TRUNCATED at the table's cap)" : "",
              m.W, m.H, v2 ? 2 : 1, RD32(ta, OFF_SAVEDGAME) ? "a saved" : "a new",
              m.refused, m.beyond, m.count, m.placed3d, m.poolFull,
              (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)s_freq.QuadPart);
    b[sizeof b - 1] = 0; plog(b);
}

static const unsigned char LOADMAP_STOLEN[7] = { 0x8B, 0x54, 0x24, 0x40,   /* mov edx,[esp+0x40] */
                                                 0xC1, 0xE2, 0x0A };       /* shl edx,0xa        */
static int mapfeat_install(void)
{
    unsigned char* s;
    unsigned char* p;
    if (!tagpu_detour_bytes_ok(VA_LOADMAP_JOIN, LOADMAP_STOLEN, sizeof LOADMAP_STOLEN)) return 0;
    s = p = tagpu_detour_stub();
    if (!s) return 0;
    *p++ = 0x60;                                            /* pushad           */
    *p++ = 0x54;                                            /* push esp         */
    *p++ = 0xE8;                                            /* call             */
    tagpu_detour_rel(p, (unsigned int)(size_t)mapfeat_at_load); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;                  /* add esp,4        */
    *p++ = 0x61;                                            /* popad            */
    memcpy(p, LOADMAP_STOLEN, sizeof LOADMAP_STOLEN); p += sizeof LOADMAP_STOLEN;
    *p++ = 0xE9; tagpu_detour_rel(p, VA_LOADMAP_JOIN + (unsigned)sizeof LOADMAP_STOLEN); p += 4;
    if (!tagpu_detour_land(VA_LOADMAP_JOIN, s, (int)sizeof LOADMAP_STOLEN)) {
        VirtualFree(s, 0, MEM_RELEASE);
        return 0;
    }
    return 1;
}

unsigned tagpu_packet_pub_draw_seq(void)  { return s_cDraws; }
unsigned tagpu_packet_pub_level_gen(void) { return s_levelGen; }
int      tagpu_packet_pub_level_open(void) { return s_levelOpen; }

/* ---- the heartbeat's producer half --------------------------------------- */

/* RUNS ON THE RENDER THREAD (from the consumer's frame_end), so it reads only
   this file's own counters — never engine memory: the flag word the level
   log wants is the packet's own `load_flags` field, printed by the consumer. */
static void extra(char* buf, unsigned cap, double secs)
{
    static unsigned lastAll, lastIn;
    unsigned all = s_cDrawsAll, in = s_cDraws, vpApplies = 0, vpWh = 0;
    unsigned i, total = 0, acc = 0, p50 = 0, p99 = 0;
    tagpu_vpwide_counters(&vpApplies, &vpWh);     /* two game-thread dwords, not engine memory */
    /* the apply-time histogram over the interval, 2 us per bucket, like the publish's */
    for (i = 0; i <= APPLY_HIST_N; i++) { unsigned h = s_applyHist[i]; total += h - s_applyPrev[i]; }
    for (i = 0; i <= APPLY_HIST_N; i++) {
        unsigned h = s_applyHist[i], d = h - s_applyPrev[i];
        s_applyPrev[i] = h;
        acc += d;
        if (!p50 && total && acc * 2u >= total) p50 = i * 2u;
        if (!p99 && total && acc * 100u >= total * 99u) p99 = i * 2u;
    }
    _snprintf(buf, cap, " | draws=%u inplay=%u draws/s=%.0f inplay/s=%.0f foreign=%u deep=%u fontcopies=%u/%u levelend=%s vpapply=%u vpwh=%u applyus p50=%u p99=%s%u"
              " | world: u=%u p=%u w=%u a=%u/%u cells scan=%u/%u dup=%u trunc=%u/%u/%u/%u relbad=%u woob=%u shd=%u",
              all, in,
              secs > 0.0 ? (double)(all - lastAll) / secs : 0.0,
              secs > 0.0 ? (double)(in - lastIn) / secs : 0.0,
              s_cForeign, s_cDeep, s_cFontCopies, s_cFontRefused,
              s_levelEndBy == 1 ? "reclaim" : s_levelEndBy == 2 ? "own" : "none",
              vpApplies, vpWh, p50, p99 >= APPLY_HIST_N * 2u ? ">" : "", p99,
              s_cLastUnits, s_cLastPieces, s_cLastWrecks, s_cLastAnchors, s_cAnchCells,
              s_cAnchScan, s_cAnchReuse,
              s_cUnitDup, s_cUnitTrunc, s_cPieceTrunc, s_cWreckTrunc, s_cAnchTrunc,
              s_cRelBad, s_cWreckOob, s_cShdCopies);
    {
        unsigned n = 0;
        while (n < cap && buf[n]) n++;
        _snprintf(buf + n, cap > n ? cap - n : 0,
                  " | fx: proj=%u expl=%u deb=%u models=%u/%uv lost=%u part=%u/%u scan=%u/%u trunc=%u thin=%u layerbad=%u subbad=%u lht=%u want=%d/%d"
                  " | fog: %dx%d/%u recodd=%u"
                  " | gui: mm=%dx%d/%u refused=%u pic=%dx%d/%u"
                  " | mapfeat=%u%s carried=%u",
                  s_cLastProj, s_cLastExpl, s_cLastDebris, s_cLastFm, s_cLastFv, s_cFxLost,
                  s_cLastPart, s_cPartMax,
                  s_cFxScan, s_cFxReuse, s_cPartTrunc, s_cPartThin, s_cLayerBad, s_cSubBad, s_cLhtCopies,
                  tagpu_fxown_want_fx(), tagpu_fxown_want_sfx(),
                  s_lastFogW, s_lastFogH, s_cFogSeen, s_cFogRecOdd,
                  s_lastMmW, s_lastMmH, s_cMmCopies, s_cMmRefused,
                  s_mmPicW, s_mmPicH, s_cMmPic,
                  s_mfN, s_mfTrunc ? "(trunc)" : "", s_cMfCarried);
        n = 0;
        while (n < cap && buf[n]) n++;
        /* THE SHELL'S CURSOR CHANNEL. `draws` is every entry to
           the cursor draw, in play and in the shell alike, so 0 over a session
           in the shell means the 0x4C67C0 observer never fired and the channel
           is not what the layer is missing. `owned` + `hidden` are the frames
           it acted on; `skip` is the FRESH gate dropping a publish the render
           thread had not taken yet — expected throughout on a shell that flips
           thousands of times a second, since the gate is what keeps this
           channel off the in-play publisher's slot. The engine's cursor BLIT
           is not skipped: the engine's cursor goes into its own surface,
           where the reference wants it (tagpu_gui.h `tagpu_gui_cursor_frame`),
           so `draws` counts every draw that runs. */
        _snprintf(buf + n, cap > n ? cap - n : 0,
                  " | cursor: draws=%u owned=%u hidden=%u pub=%u skip=%u foreign=%u stuck=%u",
                  s_cCursorDraws, s_cCursorOwned, s_cCursorHidden,
                  s_cCursorPub, s_cCursorSkip, s_cCursorForeign, s_cCursorStuck);
        n = 0;
        while (n < cap && buf[n]) n++;
        /* B3's wire section (sim-fixes.md B3): DLL counters, no engine read */
        if (cap > n) tagpu_wire_format(buf + n, cap - n);
        n = 0;
        while (n < cap && buf[n]) n++;
        /* B4's stale-hits section (sim-fixes.md B4): DLL counters, no engine read */
        if (cap > n) tagpu_hits_format(buf + n, cap - n);
        n = 0;
        while (n < cap && buf[n]) n++;
        /* B5's ghost-commander section (sim-fixes.md B5): DLL counters, no engine read */
        if (cap > n) tagpu_ghost_format(buf + n, cap - n);
    }
    if (cap) buf[cap - 1] = 0;
    lastAll = all; lastIn = in;
}

/* B8's kill counts (sim-fixes.md B8), the heartbeat's second line: DLL counters, no engine
   read. A line of their own, so that neither they nor the packet: line's last sections are
   cut; the typedef ties the kills line's proven bound to the line's buffer. */
typedef char kills_line_in_line2[(TAGPU_KILLS_LINE <= TAGPU_PACKET_LINE2) ? 1 : -1];

static void extra_line(char* buf, unsigned cap, double secs)
{
    (void)secs;
    tagpu_kills_format(buf, cap);
}

/* ---- the level-end packet's second provider -------------------------------
   THE OUT-OF-GAME PACKET MUST NOT DEPEND ON ANOTHER MODULE BEING ARMED:
   tagpu_reclaim's teardown wrap publishes it when reclaim
   is armed, but reclaim has five ways not to arm (its lever, a byte
   mismatch, a stub or a land failure) and then no level-end packet would
   ever exist — the renderer would hold a dead level's `in_game = 1` packet
   through the shell and the next load. So when reclaim is not armed, this
   module observes the teardown 0x491B60 itself (the same five stolen bytes
   reclaim's wrap takes, `mov eax,[0x511DE8]`; the function has no stack
   argument and two exits, a `ret` and a tail-jump to 0x450DD0 whose own
   `ret` returns through the same slot, so the hijacked return reaches
   `after` either way), and publishes from `after`. With neither provider the
   publisher stays COUNT-ONLY: no packet at all is better than a stale one.

   THE LEVEL GENERATION IS THIS MODULE'S OWN (`s_levelGen`), not reclaim's.
   `tagpu_packet_pub_level_end` increments
   `s_levelGen` before anything else, whichever provider called it, so the
   generation moves with reclaim armed or not. Reclaim's own counter is passed
   in for the log line and for nothing else. */
static const unsigned char TEARDOWN_STOLEN[5] = { 0xA1, 0xE8, 0x1D, 0x51, 0x00 };
static void* volatile s_teardownRet;

static int __cdecl before_teardown(void* entry_esp)
{
    unsigned ret = ((unsigned*)entry_esp)[0];
    if (!on_game_thread()) { s_cForeign++; return 0; }
    if (InterlockedCompareExchangePointer((void* volatile*)&s_teardownRet, (void*)(size_t)ret, NULL) != NULL) return 0;
    return 1;
}

static void* __cdecl after_teardown(unsigned int* regs)
{
    void* ret = (void*)InterlockedExchangePointer((void* volatile*)&s_teardownRet, NULL);
    (void)regs;
    tagpu_packet_pub_level_end(tagpu_reclaim_level_gen());
    return ret;
}

/* ---- the shell's cursor channel ------------------------------------------

   THE PROBLEM IT SOLVES. Nothing of the engine's frame is underneath the UI
   layer, so the only cursor on screen is the one the layer draws itself, in
   its sharp layer -- and it draws one only once `tagpu_gui_cursor_frame`
   (tagpu_gui_surf.c) has found the cursor's record in the packet.
   The in-play fill publishes that record on the in-play gate alone — so
   without this channel a shell frame carries no cursor record, the layer owns
   no cursor, and the shell shows none. MEASURED on the
   reference setup 2026-09-13, main menu, pointer over the window: `gui off`
   presents the cursor at (320,240) and `gui on` does not, and the two frames
   differ by 288 px whose box contains it.

   WHY NOT THE FLIP. The shell never calls DrawGameScreen, so the in-play gate
   has nothing to select on; what the shell has is the flip 0x4C63A0, which
   every present takes. But tagpu_gui_hook already observes the flip WITH an
   `after`, so THE CHAIN RULE refuses a second observer there, and it cannot be
   installed first either (tagpu_gui_init runs before tagpu_packet_pub_init).
   What the flip does have is the cursor draw it makes — 0x4C67C0, the only
   per-present site inside it that nothing owns and the only code that WRITES
   the drawn position (+0x1B6/+0x1BA) out of the mouse record. It is called
   from two mutually exclusive arms of the flip (0x4C641B, 0x4C6544), so
   AT MOST one call per present, in play and in the shell alike — the
   DirectDraw arm leaves before its call on three exits (no primary, the back
   buffer's size disagreeing with the screen, and a failed Lock), and on those
   the engine draws no cursor; nothing publishes and the consumer keeps the
   previous state. tagpu_engine.h names the three, and it is the one gap a
   hook here cannot close.
   tagpu_engine.h VA_CURSOR_DRAW carries the disassembly and the live reading.

   THE GATE IS TWO TESTS, AND NEITHER IS A DELAY. `s_retDepth == 0` and
   `!s_levelOpen`.
     - **`s_retDepth == 0`** — the cursor draw runs INSIDE DrawGameScreen, so
       on every in-play frame `before_draw` has already pushed the hijacked
       return. Not tidiness: this channel and the in-play publish share ONE
       mailbox, whose FRESH gate drops the second publisher of a frame, so two
       publishes in one draw would be a coin flip over which one the render
       thread ends up holding.
     - **`!s_levelOpen`** — because `s_retDepth == 0` is NOT the complement of
       the in-play gate, as the disassembly confirms: the screenshot sweep
       `0x495A30` ends with its own `DrawGameScreen(1, 1)` at `0x495E66` (both
       arguments 1, so the flip's own gate at `0x46A3CC`/`0x46A3D7` passes and
       the cursor draw runs), and the address that call pushes is `0x495E6B`
       rather than the in-play `0x4969D2`, so nothing is on the stack. Without
       this test a Ctrl+F9 screenshot would publish an `in_game = 0` packet
       from the middle of a level — and every world pass reads that field to
       decide whether to draw at all, so the world would blink out for a
       present or two. The movie recorder 0x4962C2 is excluded by the flip's
       own gate (it passes drawUnits = 0), which is why only the screenshot
       path needed this.
   Both are decided by the same observer, and neither depends on timing.

   FOR THE SAME REASON the in-play publisher FORCES its first packet of a
   level (see after_draw). This channel publishes all through a load, at the
   shell's own rate, so the FRESH gate can well be set at the instant the
   level's first in-play draw runs — and a dropped first packet would hold the
   renderer on an in_game = 0 packet, with no world, until the render thread
   happened to take one in between. Forcing makes the level's first packet land
   whatever this channel is doing; it costs one counted overrun per level,
   exactly like the level-end packet's own forced publish.

   WHAT CROSSES is what the in-play fill already crosses, on the same trust:
   the record pointer as a KEY (tagpu_gaf.c is its only reader). The shell's
   packet stays in_game = 0 — every world pass reads that field to decide
   whether to draw at all — and carries the cursor through `cur_rec` alone.

   THE HIDDEN CASE IS PUBLISHED TOO, and on every such frame rather than on the
   edge: 0x4C67C0 early-outs unless +0x1CE, +0x1D2 and +0x1B2 are all
   non-zero, and on those frames it writes nothing at all — so `before`
   publishes a packet with `cur_rec` 0 instead of leaving the last cursor
   standing, which would have the layer go on drawing a cursor the engine has
   hidden.
   An edge-triggered publish cannot be used here: a publish the FRESH
   gate drops is not an edge that comes again, and the render thread would hold
   the stale state until the cursor happened to change once more. */
static const unsigned char CURSOR_STOLEN[11] = { 0x56, 0x8B, 0x74, 0x24, 0x08,
                                                 0x8B, 0x86, 0xCE, 0x01, 0x00, 0x00 };
static void* volatile s_cursorRet;         /* the hijacked return, depth 1     */

/* the shell packet: the header alone. used_bytes == the header is what makes
   every area refused by area_ok — a packet with no world in it must not leave
   the previous level's areas readable through offsets a slot still holds. */
static unsigned fill_shell(TAGPU_PACKET* p, void* ctx)
{
    int live = *(const int*)ctx;
    unsigned lf = load_flags();
    tagpu_pk_fill((unsigned char*)p + offsetof(TAGPU_PACKET, used_bytes), 0,
                  sizeof(TAGPU_PACKET) - offsetof(TAGPU_PACKET, used_bytes));
    p->used_bytes = sizeof(TAGPU_PACKET);
    p->in_game    = 0;                       /* must stay 0: no world draws  */
    p->level_gen  = s_levelGen;
    p->tick       = s_lastTick;
    p->load_flags = (unsigned short)lf;
    p->text_fg    = -1;
    p->gamma      = 1.0f;
    if (live) fill_cursor(p);
    /* NO PALETTE ON THIS CHANNEL: `fill_pal` is not called, so `pal_ok` stays
       0 from the fill above and nothing here reads `main+0x143A7`; a shell
       frame draws no world. */
    return sizeof(TAGPU_PACKET);
}

static void shell_cursor_publish(int live)
{
    if (tagpu_packet_publish(fill_shell, &live, 0)) s_cCursorPub++;
    else s_cCursorSkip++;
}

static int __cdecl before_cursor(void* entry_esp)
{
    const char* g;
    if (s_countOnly) return 0;
    if (!on_game_thread()) { s_cCursorForeign++; return 0; }
    s_cCursorDraws++;
    /* in play the frame packet carries the cursor, and this channel must not
       race the publish that follows it in the same draw */
    if (s_retDepth != 0) return 0;
    /* AND NO LEVEL MAY BE IN PLAY, which is not the same test.
       The screenshot sweep 0x495A30 ends with its own `DrawGameScreen(1, 1)` at
       0x495E66 — drawUnits and blitScreen both 1, so the flip's gate at
       0x46A3CC/0x46A3D7 passes and 0x4C67C0 runs — but the address that call
       pushes is 0x495E6B, not the in-play 0x4969D2, so `before_draw` pushed
       nothing and `s_retDepth` is 0. Mid-level, this channel would then publish
       an `in_game = 0` packet from the middle of the level, and every world
       pass reads that field to decide whether to draw at all: a Ctrl+F9
       screenshot would blank the world for a present or two. `s_levelOpen` is
       this module's own "a level is being played" — set at the level's first
       in-play publish, cleared by `tagpu_packet_pub_level_end` — and it is 1
       for the whole of a level, screenshots included. */
    if (s_levelOpen) return 0;
    g = *(const char* const*)TA_GFX_PP;
    if (!ptr_ok(g)) return 0;
    if (RDU32(g, GFX_CUR_ON) && RDU32(g, GFX_CUR_OK) && RDU32(g, GFX_CUR_REC)) {
        /* it WILL draw: hijack the return so the publish follows the draw it
           reports. The packet carries the record alone (`cur_rec`), so
           `after` reads no word this draw writes.

           THE DRAWN POSITION +0x1B6/+0x1BA IS NOT READ HERE, and a hook that
           wants it needs the engine's lock: those words have a SECOND writer —
           0x4C284C and 0x4C2852, inside 0x4C25E0, the body of the engine's
           mouse thread, which loops about every 33 ms (GetTickCount() + 0x21
           at 0x4C29C5, then Sleep to it) for the whole process, play
           included: nothing calls its stop 0x4C2AC0. The engine serialises the
           two: the flip takes its tagged mutex at 0x52A4E8 before anything else
           (0x4C63CC `mov edi,'MAIN'`, `push 0x52A4E8`, `call ebp` at 0x4C63D7)
           and holds it across 0x4C67C0 and this hijack, releasing only at
           0x4C6641; the mouse
           thread takes the SAME lock around its own draw (0x4C29C8
           `mov edi,'MOUS'`, `push 0x52A4E8`, `call ebx` at 0x4C29D3).
           Verified by disassembly 2026-09-14. A read made inside this hijack
           inherits that ordering; one made from outside the flip does not. */
        if (InterlockedCompareExchangePointer((void* volatile*)&s_cursorRet,
                                              (void*)(size_t)((unsigned*)entry_esp)[0], NULL) != NULL) {
            /* THE SLOT WAS ALREADY TAKEN, which should be impossible: this
               site does not nest. It means a hijacked return never reached the
               trampoline — an exception unwinding through the draw — so the
               slot is stuck and every later frame fails here. The channel is
               then DEAD for the session and the shell cursor goes back to
               being the engine's under our layer, which is the original bug.
               COUNTED rather than recovered from: the stale value may be a
               live frame's return address, and there is nothing here that can
               tell. `stuck` climbing in the heartbeat is the only signal that
               distinction can be made from, and without it the channel dies
               silently — `draws` climbs and `pub` freezes, which is exactly
               what the draw's own early-out looks like. */
            s_cCursorStuck++;
            return 0;
        }
        s_cCursorOwned++;
        return 1;
    }
    s_cCursorHidden++;
    shell_cursor_publish(0);
    return 0;
}

static void* __cdecl after_cursor(unsigned int* regs)
{
    void* ret = (void*)InterlockedExchangePointer((void* volatile*)&s_cursorRet, NULL);
    (void)regs;
    shell_cursor_publish(1);
    return ret;
}


/* ---- install ------------------------------------------------------------- */

void tagpu_packet_pub_init(void)
{
    char b[400];
    int drawOk, loaderOk, cursorOk, mapfeatOk;
    s_gameTid = GetCurrentThreadId();       /* DllMain runs on the game loop's thread */
    QueryPerformanceFrequency(&s_freq);
    s_countOnly = !tagpu_packet_armed();
    s_stress = GetFileAttributesA("tagpu_packet.stress") != INVALID_FILE_ATTRIBUTES;
    if (!tagpu_detour_bytes_ok(VA_DRAWGAMESCREEN, DRAW_STOLEN, sizeof DRAW_STOLEN)) {
        plog("packet: NOT armed — engine bytes differ at DrawGameScreen 0x468CF0");
        return;
    }
    /* the level-end packet's provider, decided BEFORE the draw observer so a
       count-only decision is made once */
    if (!s_countOnly) {
        if (tagpu_reclaim_armed()) s_levelEndBy = 1;
        else if (tagpu_detour_bytes_ok(VA_TEARDOWN, TEARDOWN_STOLEN, sizeof TEARDOWN_STOLEN) &&
                 tagpu_detour_observe(VA_TEARDOWN, TEARDOWN_STOLEN, sizeof TEARDOWN_STOLEN, before_teardown, after_teardown))
            s_levelEndBy = 2;
        else {
            s_countOnly = 1;
            plog("packet: publisher COUNT-ONLY — no level-end provider: tagpu_reclaim is not armed and the "
                 "teardown 0x491B60 could not be observed, so a level's last packet would outlive the level. "
                 "Nothing is published or applied: NO WORLD PASS DRAWS (every pass reads the packet's view), "
                 "no command is applied (the engine keeps its own camera range, viewport rect and scroll rate; "
                 "the wheel and tagpu_eye.txt do nothing), and every string through tagpu_text_place is blank");
        }
    }
    drawOk = tagpu_detour_observe(VA_DRAWGAMESCREEN, DRAW_STOLEN, sizeof DRAW_STOLEN,
                                  before_draw, s_countOnly ? NULL : after_draw);
    if (!drawOk) {
        plog("packet: NOT armed — could not observe DrawGameScreen 0x468CF0");
        return;
    }
    s_installed = 1;
    tagpu_packet_producer(s_gameTid);
    loaderOk = tagpu_detour_bytes_ok(VA_LOADER_ENTRY, LOADER_STOLEN, sizeof LOADER_STOLEN) &&
               tagpu_detour_observe(VA_LOADER_ENTRY, LOADER_STOLEN, sizeof LOADER_STOLEN, before_loader, after_loader);
    /* the map's own features, taken inside LoadMap: only when something
       publishes, since the list rides the packet and nothing else reads it.
       A site that does not take leaves the mirror off -- the consumer never
       holds a list, so neither ground nor trees are mirrored -- and says so. */
    mapfeatOk = !s_countOnly && mapfeat_install();
    /* the shell's cursor channel — the cursor draw inside the flip. Only when
       something publishes: in count-only mode it would cost a hijack per
       present to reach a publish that returns 0 at the door. Armed, the site
       is byte-matched like every other, and an install that does not take
       leaves the shell exactly as it was (a missing cursor, not a broken
       frame): a failed hijack here is logged and nothing else changes. */
    cursorOk = !s_countOnly &&
               tagpu_detour_bytes_ok(VA_CURSOR_DRAW, CURSOR_STOLEN, sizeof CURSOR_STOLEN) &&
               tagpu_detour_observe(VA_CURSOR_DRAW, CURSOR_STOLEN, sizeof CURSOR_STOLEN,
                                    before_cursor, after_cursor);
    tagpu_packet_set_extra(extra);
    tagpu_packet_set_extra_line(extra_line);
    _snprintf(b, sizeof b,
              "packet: publisher %s on DrawGameScreen 0x468CF0 (in-play gate: return address 0x4969D2; "
              "chained after tagpu_menu's observer), level-end packet by %s, loader-thread observer at 0x497C70=%d, game thread %u%s",
              s_countOnly ? "COUNT-ONLY" : "ARMED",
              s_levelEndBy == 1 ? "tagpu_reclaim's teardown post hook" : s_levelEndBy == 2 ? "our own observer on the teardown 0x491B60 (reclaim is not armed; the level generation is this module's own either way)" : "nobody",
              loaderOk, (unsigned)s_gameTid,
              s_countOnly ? " — nothing is published, taken or applied: no world pass draws, no command is applied (the engine's own camera range, rect and scroll rate), every string through tagpu_text_place draws nothing" : "");
    b[sizeof b - 1] = 0;
    plog(b);
    _snprintf(b, sizeof b, "packet: the map's own features from LoadMap's join 0x483B53=%d%s", mapfeatOk,
              mapfeatOk ? " (loader thread, Vulkan lane only)"
                        : s_countOnly ? " (count-only: nothing is published at all)"
                                      : " -- NOT installed: the engine bytes differ, and the map edge stays black");
    b[sizeof b - 1] = 0;
    plog(b);
    /* its own line, because a missing shell cursor is otherwise a silent
       symptom: the layer covers the engine's and nothing says why */
    _snprintf(b, sizeof b,
              "packet: shell cursor channel observer on the flip's cursor draw 0x4C67C0=%d%s",
              cursorOk,
              cursorOk ? " (the shell's only publish point: the flip 0x4C63A0 cannot be observed a second time, tagpu_gui_hook's hijacks it)"
                       : s_countOnly ? " (count-only: nothing is published at all)"
                                     : " — NOT installed: the shell's cursor stays the engine's, and the UI layer paints over it (tagpu_engine.h VA_CURSOR_DRAW)");
    b[sizeof b - 1] = 0;
    plog(b);
}
