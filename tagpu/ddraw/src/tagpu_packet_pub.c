/* tagpu_packet_pub.c — the frame packet's PUBLISHER (landing 1).
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
     before  = THE COMMANDS (landing 2): post-tick, pre-draw, the latest
               record the render thread posted is taken and applied on
               this thread — the zoom level (the camera range, the
               addressable rect, ScrollSpeed), the cursor anchor's eye
               delta, the camera hold, the follow release — so the frame
               the engine is about to draw, its fog rebuild and its minimap
               box all see the commanded camera (tagpu_zoom_apply,
               tagpu_vpwide_apply). No render-thread store into engine
               memory remains.
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
   No instruction clears bit 0 or bit 1 (every reference to the word is
   listed in the exe note), so whether they are reset between two games in
   one process is settled by the log lines here, not by the disassembly.
   The observer on 0x497C70 (stolen `55 8B EC 6A FF`, position-independent)
   logs the loader's thread id at its entry — the direct measurement the plan
   asked for, in place of the call-chain inference.

   THE FONT, AS BYTES. The marker block's font (`[globals+0x204]`) and text
   colour (`+0x208`) are latched at hook 8 (0x469BD7, markown's stub), the
   instant the block that draws the group digit and the ShowRanges labels
   begins — nothing between there and the digit's own draw at 0x469CF9 calls
   SetFont. Until this landing the render thread dereferenced that font
   behind IsBadReadPtr; no note establishes a UI font's lifetime, and a probe
   is not a lifetime argument (CLAUDE.md). Now the game thread copies the
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
   probed: the copy draws nothing rather than index a table it cannot bound
   (landing review). What a corrupt file could do to the engine's own blitter
   it can do here too; that is the same trust, on the same thread. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_engine.h"
#include "tagpu_packet.h"
#include "tagpu_packet_pub.h"
#include "tagpu_detour.h"
#include "tagpu_reclaim.h"
#include "tagpu_vpwide.h"
#include "tagpu_zoom.h"
#include "tagpu_gui.h"
#include "tagpu_native.h"   /* tagpu_native_owns_unit: the ownership answer, taken here */
#include "tagpu_zoom.h"      /* TAGPU_ZOOM_MIN: the widest rect the tables cover */
#include "tagpu_model3do.h"  /* TAGPU_PBMAXPIECE, to assert the packet's copy of it */
#include "tagpu_gaf.h"       /* the GAF resolvers: pure reads, taken on THIS thread */
#include "tagpu_fxown.h"     /* whether the render thread wants the effect tables */
#include "tagpu_fogwide.h"   /* the wide fog grid, built in THIS draw on this thread */
#include "tagpu_terrown.h"   /* the eye the engine's own fog grid is anchored at */
#include "tagpu_gui.h"       /* whether the render half wants the minimap surfaces */

/* DrawGameScreen's prologue, `sub esp,0x214` — the same six bytes
   tagpu_menu.c observes */
static const unsigned char DRAW_STOLEN[6]   = { 0x81, 0xEC, 0x14, 0x02, 0x00, 0x00 };
/* the loader thread's entry: `push ebp; mov ebp,esp; push -1` */
static const unsigned char LOADER_STOLEN[5] = { 0x55, 0x8B, 0xEC, 0x6A, 0xFF };

#define RET_DEPTH 8

static void plog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

static int ptr_ok(const void* p) { return (size_t)p > 0x10000u && (size_t)p < 0x7FFF0000u; }

static DWORD    s_gameTid;                 /* DllMain's thread: the game loop's */
static int      s_installed, s_countOnly, s_stress;
static void*    s_retStack[RET_DEPTH];     /* hijacked returns, LIFO            */
static int      s_retDepth;
/* counters: written on the game thread, read by the heartbeat */
static volatile unsigned s_cDrawsAll, s_cDraws, s_cForeign, s_cDeep;
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
/* THE LEVEL GENERATION IS THE PACKET'S OWN, and it has to be: it was
   tagpu_reclaim's counter, which is bumped only in reclaim's teardown post
   hook — so with reclaim unarmed it never moved, and every consumer keyed on
   it silently stopped invalidating. `frame_pair` withholds a PREV across a
   level on it, the pose blend refuses a pair on it, and the model-template
   caches drop on it; all three were inert in that configuration (landing
   review, 2026-09-12). It advances here instead, at every level end, whoever
   published it. Reclaim's own counter still exists for reclaim's own ring. */
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

static unsigned append_area(TAGPU_PACKET* p, unsigned* cursor, const void* src, unsigned len,
                            unsigned* off_out, unsigned* len_out, unsigned trunc_bit)
{
    unsigned at = (*cursor + 3u) & ~3u;
    unsigned end = at + ((len + 3u) & ~3u);
    if (!len) { *off_out = 0; *len_out = 0; return *cursor; }
    if (end > p->cap_bytes) { p->truncated |= trunc_bit; *off_out = 0; *len_out = 0; return end; }
    if (src) tagpu_pk_copy((unsigned char*)p + at, src, len);
    else tagpu_pk_fill((unsigned char*)p + at, 0x5A, len);
    *off_out = at; *len_out = len;
    *cursor = end;
    return end;
}

/* ======================= THE WORLD TABLES (landing 3) =====================
   Everything below runs on the GAME THREAD, inside DrawGameScreen, on the
   in-play gate — the one set of frames during which the loader thread has
   finished and this thread owns every array it touches (the file comment's
   "THE LOADER THREAD"). That ordering is what closes the audit's open hazard:
   the unit array's begin/end pair was read unsynchronised by six render-thread
   files, and no read-side gate could close it because `end` is never nulled.

   THE BOUNDS ARE THE ENGINE'S OWN COUNTS, APPLIED HERE AND NOWHERE ELSE.
   The walk runs to `unit_slots` (u16 main+0x14351, stored at 0x4854EF BEFORE
   `begin`), never to the `end` pointer — so it needs no pair at all and cannot
   be skewed by one. A model id is dropped unless it is inside UNITINFOCount, a
   feature def unless it is inside NumFeatureDefs, a piece count unless it is
   inside TAGPU_PK_MAXPIECE, a cargo link unless it resolves to a slot this
   packet carries. Past this file every index is a packet index into a table
   whose length the consumer's own bounds check has already verified.

   THE LAYOUT, and why it is this order: header | pieces | units | wrecks |
   anchors | font | shd. A unit's `piece_off` has to be an absolute byte offset
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
typedef char pk_unit_size  [(sizeof(TAGPU_PK_UNIT)   == 100) ? 1 : -1];
typedef char pk_piece_size [(sizeof(TAGPU_PK_PIECE)  ==  24) ? 1 : -1];
typedef char pk_wreck_size [(sizeof(TAGPU_PK_WRECK)  ==  44) ? 1 : -1];
typedef char pk_anchor_size[(sizeof(TAGPU_PK_ANCHOR) ==  16) ? 1 : -1];

static TAGPU_PK_UNIT   s_uScratch[TAGPU_PK_MAX_UNITS];
static TAGPU_PK_WRECK  s_wScratch[TAGPU_PK_MAX_WRECKS];
static TAGPU_PK_ANCHOR s_aScratch[TAGPU_PK_MAX_ANCHORS];
/* slot -> index in the units table, for the cargo links; 0xFFFF = not carried
   in this packet. Sized like the scratch and rewritten for the slots the walk
   actually visits, so no memset of 32 KB per frame. */
static unsigned short  s_slotIdx[TAGPU_PK_MAX_UNITS];
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
   first. At the 256 published frames a second this machine reaches against a
   60 Hz sim that is four publishes out of five; at or below the sim rate it
   costs one comparison and changes nothing. The WRECKS are re-derived from the
   cached anchors either way, because their piece runs go into THIS packet's
   arena. */
static unsigned s_aTick;
static int      s_aHave, s_aTrunc, s_aRect[4];
static unsigned s_aN;
static volatile unsigned s_cAnchScan, s_cAnchReuse;

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
       build fraction; the unit pass used to call it per unit per frame from the
       render thread, which is one of the reads this landing removes. It is the
       same predicate the marker pass, the composite wipe and the owndraw
       classifier already ask on the game thread, so all four now agree by
       construction rather than by two threads reading the same bytes. */
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
    if (end > p->cap_bytes) { p->truncated |= trunc_bit; return end; }
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

/* THE ANCHOR RECT: the engine's own feature sweep grown to the WIDEST zoom the
   lever allows, plus a margin, and clamped to the map. Never the zoom in force
   — the render thread may still be drawing from this packet a frame later, and
   at a level the game thread has not seen; the margin also absorbs the cursor
   anchor's unacknowledged eye delta, which is how far ahead of `eye` the frame
   is actually drawn. A consumer at any zoom asks for a sub-rect of this one and
   counts what falls outside. */
#define PK_ANCH_MARGIN 32           /* 16-px cells on every side               */

static void anchor_rect(const TAGPU_PACKET* p, int* c0, int* r0, int* cols, int* rows)
{
    int vw = p->vp[2], vh = p->vp[3];
    int ew = (int)((float)vw / TAGPU_ZOOM_MIN) + 64;
    int eh = (int)((float)vh / TAGPU_ZOOM_MIN) + 64;
    int mapW = p->map_w16, mapH = p->map_h16;
    *c0   = ((p->eye[0] + (vw - ew) / 2) >> 4) - PK_ANCH_MARGIN;
    *r0   = ((p->eye[1] + (vh - eh) / 2) >> 4) - PK_ANCH_MARGIN;
    *cols = (ew >> 4) + 16 + 2 * PK_ANCH_MARGIN;
    *rows = (eh >> 4) + 40 + 2 * PK_ANCH_MARGIN;
    if (*c0 < 0) { *cols += *c0; *c0 = 0; }
    if (*r0 < 0) { *rows += *r0; *r0 = 0; }
    if (*c0 + *cols > mapW) *cols = mapW - *c0;
    if (*r0 + *rows > mapH) *rows = mapH - *r0;
    if (*cols < 0) *cols = 0;
    if (*rows < 0) *rows = 0;
}

/* The four tables, into the slot the producer holds. Returns the byte count
   the fill NEEDED — past cap_bytes means something truncated this frame and
   the primitive grows the write slot before the next fill. */
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
    unsigned pieces_base = PKT_ALIGN4((unsigned)sizeof(TAGPU_PACKET));
    unsigned need = pieces_base, e;
    unsigned pTrunc = 0;
    int c0, r0, cols, rows, row, col;
    int inL, inT, inR, inB;

    p->feat_defcount = RD32(ta, OFF_FEATCOUNT);
    p->sweep_cols  = RD32(ta, OFF_SWEEP_C);
    p->sweep_rows  = RD32(ta, OFF_SWEEP_R);
    if (p->feat_defcount < 0 || p->feat_defcount > 4096) p->feat_defcount = 0;

    anchor_rect(p, &c0, &r0, &cols, &rows);
    p->anch_c0 = c0; p->anch_r0 = r0; p->anch_cols = cols; p->anch_rows = rows;

    /* the piece-cull rect, in world px: the widest viewport about the eye plus
       the 256-px slack the unit gather already allows itself */
    {
        int vw = p->vp[2], vh = p->vp[3];
        int ew = (int)((float)vw / TAGPU_ZOOM_MIN) + 64 + 512;
        int eh = (int)((float)vh / TAGPU_ZOOM_MIN) + 64 + 512;
        inL = p->eye[0] - (ew - vw) / 2;
        inT = p->eye[1] - (eh - vh) / 2;
        inR = inL + ew;
        inB = inT + eh;
    }

    /* ---- the units, and their pieces ---- */
    walk = slots;
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
            /* the ground cell under the anchor: the shadow's height, and the
               one feature-grid read a unit needs */
            {
                int wx = (int)(short)(ue->pos[0] >> 16);
                int wy = (int)(short)(ue->pos[2] >> 16);
                int tx = wx >> 4, ty = wy >> 4;
                if (ptr_ok(fmap) && tx >= 0 && ty >= 0 && tx < mapW && ty < mapH) {
                    ue->ground_h = *(const unsigned char*)(fmap + ((size_t)ty * mapW + tx) * FT_STRIDE + FT_HEIGHT);
                    ue->flags |= TAGPU_PK_U_GROUND;
                }
                if (wx >= inL && wx <= inR && wy >= inT && wy <= inB)
                    ue->flags |= TAGPU_PK_U_INRECT;
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
        if (s_aHave && s_aTick == p->tick &&
            s_aRect[0] == c0 && s_aRect[1] == r0 &&
            s_aRect[2] == cols && s_aRect[3] == rows) {
            na = s_aN;
            if (s_aTrunc) p->truncated |= TAGPU_PK_TRUNC_ANCHORS;
            s_cAnchReuse++;
        } else {
            for (row = r0; row < r0 + rows; row++) {
                const char* trow = fmap + ((size_t)row * mapW) * FT_STRIDE;
                for (col = c0; col < c0 + cols; col++) {
                    const char* t = trow + (size_t)col * FT_STRIDE;
                    unsigned d = RDU16(t, FT_DEFIDX);
                    TAGPU_PK_ANCHOR* a;
                    if (d >= 0xFFFBu) continue;
                    if (na >= TAGPU_PK_MAX_ANCHORS) { s_cAnchTrunc++; p->truncated |= TAGPU_PK_TRUNC_ANCHORS; row = r0 + rows; break; }
                    a = &s_aScratch[na++];
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
            }
            s_aHave = 1; s_aTick = p->tick; s_aN = na;
            /* A TRUNCATED SCAN IS CACHED AS TRUNCATED. Without this every reuse
               publish of the same tick reported a complete anchor table, and
               the gate we read is "trunc is 0 after the first fill" — so the
               one number that would have said the rect overflowed the table was
               under-reported by exactly the reuse rate, which is 6 in 7
               (landing review, 2026-09-12). */
            s_aTrunc = (p->truncated & TAGPU_PK_TRUNC_ANCHORS) ? 1 : 0;
            s_aRect[0] = c0; s_aRect[1] = r0; s_aRect[2] = cols; s_aRect[3] = rows;
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
               drawing; this walk covers the zoom-floor rect plus a margin,
               which is most of a screen of cells the engine never touches.
               WR_COUNT is the pool the level allocates (tagpu_engine.h), and
               2048 is also the value its allocator hands back for "none".
               (landing review, 2026-09-12) */
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
    }

    e = append_table(p, cursor, s_uScratch, nu, (unsigned)sizeof(TAGPU_PK_UNIT),
                     &p->off_units, &p->n_units, TAGPU_PK_TRUNC_UNITS);
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


/* ====================== THE EFFECTS TABLES (landing 4a) ===================
   The four per-frame effect arrays, on the thread that owns them. Until this
   landing `tagpu_fx.c` and `tagpu_sfx.c` walked all four on the RENDER thread
   behind `IsBadReadPtr`, and the particle heap was the one client the level
   fence could not cover: the layer table is per game, but each layer's
   {begin,end} pair and every object's sub-particle vector are std::vectors the
   game thread GROWS mid-play, freeing the old array (0x4732E0), so the pair
   could be read skewed and a consistent pair could name memory just freed
   (cross-thread-engine-reads.md §5). Reading them here is not a narrower
   window; it is the same thread doing both.

   THE GATHER IS TAKEN ONCE PER SIM TICK. All four arrays are sim state — the
   tick moves a projectile, advances an explosion's anim frame and runs every
   particle's update leaf — and the engine's own draw passes (0x49BE60,
   0x420B00, 0x471F90) read them and write nothing, so two publishes of one
   tick must produce the same table and the second reuses the first. That is
   the anchor scan's argument (§ THE ANCHOR SCAN above) applied to a second
   gather, and it matters more here: at the 250-odd publishes a second this
   machine reaches against a 60 Hz sim, four publishes in five reuse.

   EVERY ENGINE POINTER IS RESOLVED HERE. A sprite's GAF frame is looked up
   through the sequence the particle or the anim state names, while the object
   that names it is live on this thread; a weapon's colour NUMBER goes through
   main+0xDCB here; the attacker's owner byte is read here. What still crosses
   as an address is a model TEMPLATE root — `tagpu_fx.c` passes it straight to
   the native pass's `emit_fx_model` and reads no byte of it — and a GAF frame,
   which only `tagpu_gaf.c` (session-reader) ever dereferences. */

static TAGPU_PK_PROJ   s_pScratch[TAGPU_PK_MAX_PROJ];
static TAGPU_PK_EXPL   s_eScratch[TAGPU_PK_MAX_EXPL];
static TAGPU_PK_DEBRIS s_dScratch[TAGPU_PK_MAX_DEBRIS];
static TAGPU_PK_PART   s_partScratch[TAGPU_PK_MAX_PART];
static unsigned s_nProj, s_nExpl, s_nDebris, s_nPart;
static unsigned s_partN[TAGPU_PK_NLAYER], s_partObj[TAGPU_PK_NLAYER];
static unsigned s_fxTick, s_fxGen, s_fxLevel, s_fxWant;
static int      s_fxHave, s_fxPartTrunc;
static volatile unsigned s_cFxScan, s_cFxReuse, s_cPartTrunc, s_cPartMax, s_cLayerBad, s_cSubBad;
#define PART_SUBCAP 4096u   /* sub-particles per object: a containment filter */
static volatile unsigned s_cLastProj, s_cLastExpl, s_cLastDebris, s_cLastPart;

/* the LHT lighten table, latched exactly like the shade table */
static unsigned char s_lht[TAGPU_PK_LHT_BYTES];
static const unsigned char* s_lhtPtr;
static int s_lhtOk;
static volatile unsigned s_cLhtCopies;

typedef char pk_proj_size  [(sizeof(TAGPU_PK_PROJ)   == 56) ? 1 : -1];
typedef char pk_expl_size  [(sizeof(TAGPU_PK_EXPL)   == 32) ? 1 : -1];
typedef char pk_debris_size[(sizeof(TAGPU_PK_DEBRIS) == 24) ? 1 : -1];
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

/* one layer's objects into the particle table */
static void gather_layer(const char* layers, int L)
{
    const char* lay = layers + (size_t)L * LAYER_STRIDE;
    const char* const* b = *(const char* const* const*)(lay + LAYER_BEGIN);
    const char* const* e = *(const char* const* const*)(lay + LAYER_END);
    unsigned n, i;
    if (!ptr_ok(b) || !ptr_ok(e) || e <= b) return;
    /* THE BOUND IS THE ENGINE'S OWN RULE, not a probe, and the rule is 401 —
       one MORE than the number in the compare. Every emitter reads the layer's
       size and `cmp eax,0x190 / jbe append` (0x472071 and twelve more): at 400
       or fewer it appends, and past that it destroys the FRONT object, shifts
       the vector down by one and appends anyway. So a layer at 401 is the
       engine's steady state, and a walk that stopped at 400 would drop the
       whole layer every time it filled — measured 2026-09-12, 86 layers
       refused in one fx-mix run before this line said 401. The pair itself is
       sound because this thread is the one that runs those emitters; a count
       past 401 is a fact worth counting, not a walk worth attempting. */
    n = (unsigned)(e - b);
    if (n > (unsigned)LAYER_OBJCAP + 1u) { s_cLayerBad++; return; }
    s_partObj[L] = n;
    for (i = 0; i < n; i++) {
        const char* o = b[i];
        int k, sub;
        unsigned ns, j;
        const char* sb; const char* se;
        if (!ptr_ok(o)) continue;
        k = part_kind(*(const unsigned*)o);
        if (k < 0) continue;                       /* the base class draws nothing */
        sub = PART_FMT[k].sprite;
        sb = *(const char* const*)(o + PO_SUB0);
        se = *(const char* const*)(o + PO_SUB1);
        if (!ptr_ok(sb) || !ptr_ok(se) || se <= sb) continue;
        ns = (unsigned)(se - sb) / (unsigned)PART_FMT[k].stride;
        /* A SANITY FILTER ON A VALUE, not the safety argument — the argument is
           that this thread is the one that grows these vectors. What it buys is
           containment: without it one object with a wild `end` fills the whole
           table and truncates every layer after it, and with it that object is
           skipped and the frame is otherwise complete. 4096 is the number the
           render-thread pass used. */
        if (ns > PART_SUBCAP) { s_cSubBad++; continue; }
        for (j = 0; j < ns; j++) {
            const char* q = sb + (size_t)j * (size_t)PART_FMT[k].stride;
            TAGPU_PK_PART* pe;
            int X = *(const int*)(q + PART_FMT[k].pos);
            int A = *(const int*)(q + PART_FMT[k].pos + 4);
            int Y = *(const int*)(q + PART_FMT[k].pos + 8);
            if (s_nPart >= TAGPU_PK_MAX_PART) { s_fxPartTrunc = 1; return; }
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

/* the projectiles, the debris slots and the explosions */
static void gather_effects(const char* ta, int tick, const unsigned char* coltab)
{
    const char* pbase = *(const char* const*)(ta + OFF_PROJ);
    int np = RD32(ta, OFF_NPROJ);
    int ne = RD32(ta, OFF_NEXPL);
    unsigned a;
    int i;

    /* ---- projectiles (0x49BE60) ---------------------------------------
       The array is per game: 0x499A30 allocates exactly 300 slots and
       0x499A80 frees AND NULLS the base inside the teardown cascade, so a
       NULL base is the refusal and the walk's bound is the allocation. The
       8192-slot sanity cap the render-thread pass used is gone with it. */
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
            if (s_nProj >= TAGPU_PK_MAX_PROJ) break;      /* np <= 300 already */
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
            {
                const char* au = *(const char* const*)(q + PJ_ATTACKER);
                pe->owner = ptr_ok(au) ? *(const unsigned char*)(au + U_OWNER) : 0;
            }
            {
                const short* tr = (const short*)(q + PJ_TURN);
                /* the rendertype's own adjustment, applied once, here */
                if (rt == 1) {
                    pe->turn[0] = tr[0];
                    pe->turn[1] = (short)(tr[1] - 0x8000);
                    pe->turn[2] = (short)(tr[2] - 0x8000);
                } else if (rt == 6) {
                    pe->turn[0] = tr[0]; pe->turn[1] = tr[1]; pe->turn[2] = tr[2];
                }
                if (rt == 1 || rt == 3 || rt == 6) {
                    const char* node = *(const char* const*)(w + W_MODEL);
                    if (ptr_ok(node)) {
                        pe->node = (unsigned)(size_t)node;
                        if (rt == 1 && tick < *(const int*)(q + PJ_DEATH)) {
                            const char* child = *(const char* const*)(node + 0x30);
                            if (ptr_ok(child)) {
                                unsigned mask = *(const unsigned*)(w + W_MASK);
                                pe->child  = (unsigned)(size_t)child;
                                pe->cturn0 = (mask & (1u << 21))
                                           ? *(const short*)(q + PJ_SPIN) : pe->turn[0];
                            }
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

    /* ---- the flying-debris particle slots (drawn by 0x4211D0) ---------- */
    for (a = VA_PSYS_BEGIN; a < VA_PSYS_END; a += 4) {
        const char* sys = *(const char* const*)(size_t)a;
        const char* pc;
        TAGPU_PK_DEBRIS* de;
        const short* tr;
        if (!ptr_ok(sys)) continue;
        pc = *(const char* const*)(sys + PSYS_PIECE);
        if (!ptr_ok(pc)) continue;
        if (s_nDebris >= TAGPU_PK_MAX_DEBRIS) break;      /* 100 slots, 100 rows */
        de = &s_dScratch[s_nDebris++];
        de->pos[0] = *(const int*)(pc + DB_X);
        de->pos[1] = *(const int*)(pc + DB_ALT);
        de->pos[2] = *(const int*)(pc + DB_Y);
        {
            const char* node = *(const char* const*)(pc + DB_NODE);
            de->node = ptr_ok(node) ? (unsigned)(size_t)node : 0u;
        }
        tr = (const short*)(pc + DB_TURN);
        de->turn[0] = tr[0]; de->turn[1] = tr[1]; de->turn[2] = tr[2];
        de->pad = 0;
    }

    /* ---- explosions (0x420B00) ---------------------------------------
       The records are INLINE in the block, so there is no base to be NULL and
       no allocation to outlive: the count is the whole bound, and the engine's
       own add site keeps it under 300 (0x420A42). */
    if (ne > 0) {
        if (ne > EXPL_COUNT) ne = EXPL_COUNT;
        for (i = 0; i < ne; i++) {
            const char* q = ta + OFF_EXPL + (size_t)i * EXPL_STRIDE;
            TAGPU_PK_EXPL* xe;
            if (s_nExpl >= TAGPU_PK_MAX_EXPL) break;      /* ne <= 300 already */
            xe = &s_eScratch[s_nExpl++];
            const short* tr = (const short*)(q + EX_TURN);
            const char* node = *(const char* const*)(q + EX_NODE);
            xe->pos[0] = *(const int*)(q + EX_X);
            xe->pos[1] = *(const int*)(q + EX_ALT);
            xe->pos[2] = *(const int*)(q + EX_Y);
            xe->node   = ptr_ok(node) ? (unsigned)(size_t)node : 0u;
            xe->frame  = *(const unsigned*)(q + EX_ST1 + TAGPU_AS_SEQ)
                       ? (unsigned)(size_t)tagpu_gaf_state_frame(q + EX_ST1) : 0u;
            xe->flash  = *(const unsigned*)(q + EX_ST2 + TAGPU_AS_SEQ)
                       ? (unsigned)(size_t)tagpu_gaf_state_frame(q + EX_ST2) : 0u;
            xe->turn[0] = tr[0]; xe->turn[1] = tr[1]; xe->turn[2] = tr[2];
            xe->pad = 0;
        }
    }
}

/* the whole gather, once per tick; `tick` is the packet's own GameTime */
static void fx_gather(const char* ta, unsigned tick, unsigned level)
{
    const char* layers;
    unsigned want = (tagpu_fxown_want_fx() ? 1u : 0u) | (tagpu_fxown_want_sfx() ? 2u : 0u);
    int L;
    /* THE KEY IS (LEVEL, TICK), not the tick alone. A new level restarts
       GameTime, and every template address in the tables belongs to the level
       that produced them: a cache hit across a level boundary would hand the
       native pass a model root the teardown has freed. The level end clears
       the cache as well (tagpu_packet_pub_level_end), but that is the second
       line — this is the one that holds whichever provider fired. */
    if (s_fxHave && tick == s_fxTick && level == s_fxLevel && want == s_fxWant)
        { s_cFxReuse++; return; }
    s_nProj = s_nExpl = s_nDebris = s_nPart = 0;
    s_fxPartTrunc = 0;
    tagpu_pk_fill(s_partN, 0, (unsigned)sizeof s_partN);
    tagpu_pk_fill(s_partObj, 0, (unsigned)sizeof s_partObj);
    if (want & 2u) {
        /* the layer TABLE is per game (0x471D90 allocates it, 0x471DE0 frees
           and NULLS it in the teardown), which is the refusal after a level
           ends; the vectors inside it are this thread's own */
        layers = *(const char* const*)(ta + OFF_LAYERS);
        if (ptr_ok(layers))
            for (L = 0; L < (int)TAGPU_PK_NLAYER; L++) gather_layer(layers, L);
    }
    if (want & 1u) {
        const unsigned char* coltab = (const unsigned char*)(ta + OFF_GUICOL);
        gather_effects(ta, (int)tick, coltab);
    }
    if (s_fxPartTrunc) s_cPartTrunc++;
    if (s_nPart > s_cPartMax) s_cPartMax = s_nPart;
    s_fxTick = tick; s_fxLevel = level; s_fxWant = want; s_fxHave = 1; s_fxGen++;
    s_cFxScan++;
}

/* the four tables into the record, after the world's */
static unsigned fill_fx(TAGPU_PACKET* p, const char* ta, unsigned* cursor)
{
    unsigned need = *cursor, e;
    const char* g = *(const char* const*)TA_GFX_PP;
    p->fx_caps = ptr_ok(g) ? RDU16(g, PROG_CAPS) : 0u;
    fx_gather(ta, p->tick, p->level_gen);
    p->fx_gen  = s_fxGen;
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
    s_cLastDebris = s_nDebris; s_cLastPart = s_nPart;
    return need;
}


/* ======================== THE FOG GRIDS (landing 4b) ======================
   Two lattices, both read on the thread that writes them.

   THE ENGINE'S is `*(main+0x1421F)` — a descriptor {u16* buf, cols, rows,
   cells} built once per map by LoadMap (0x483C03 / 0x483C96) and REWRITTEN in
   place by the builder 0x4843C0, which this fork's terrain owner calls from the
   engine's own fog site inside the draw. So the render thread used to read the
   descriptor, and the buffer behind it, while this thread was rewriting both:
   the read `tagpu_fog_at`'s guard was added for after a hard fault off a base
   of -9 (2026-09-03; the root cause was never found and still is not — what
   changes here is that the class is gone, not that it was diagnosed).

   THE RELATION IS THE CHECK, and it is the engine's own: `cells` is the
   allocation, which the builder rounds up to a multiple of 8 before allocating
   (0x483C84: add 7, and ~7), so `cells == ((cols*rows + 7) & ~7)` says the
   three numbers describe one block. Demanding `cells == cols*rows` refused
   every viewport whose cell count is not already a multiple of 8 — 1920x1080 is
   58x34 = 1972 against an allocated 1976 — and a refusal here is not degraded
   fog but NO fog at all. What lands in the packet is exactly `cols*rows`
   entries, so the consumer's bound is its own length and the round-up is this
   file's business alone.

   THE ORIGIN IS TAKEN HERE, from the eye this packet carries, because that is
   the eye the grid was built at. The render thread used to derive it from its
   PREDICTED eye — the packet's plus a cursor-anchor step not yet applied — and
   covered the disagreement by taking the wide grid whenever anything was
   unacknowledged. It still takes the wide grid there; the origin is now right
   either way.

   THE WIDE ONE is tagpu_fogwide's, built in `terr_fogtick` during THIS draw,
   on this thread. Copying it here is what retired that module's three buffers,
   its critical section and its retire ring. */

#define FOG_DESC     0x1421F      /* -> {u16* buf, i32 cols, i32 rows, i32 cells} */
#define PROG_FOGSH   0x0CC        /* u8[256]: the grey band's palette remap,
                                     applied by 0x4BFE10 as p -> shade[p]      */

static unsigned char s_fogsh[TAGPU_PK_FOGSHADE_BYTES];
static const unsigned char* s_fogshPtr;
static int s_fogshOk;
static volatile unsigned s_cFogshCopies, s_cFogRefused, s_cFogwSeen;
static volatile int      s_lastFogC, s_lastFogR, s_lastFogwC, s_lastFogwR;

static void fogshade_snapshot(void)
{
    const char* g = *(const char* const*)TA_GFX_PP;
    const unsigned char* t;
    if (!ptr_ok(g)) return;
    t = *(const unsigned char* const*)(g + PROG_FOGSH);
    if (!ptr_ok(t)) return;
    if (t == s_fogshPtr && s_fogshOk) return;
    /* THE BOUND IS THE FORMAT: 0x4BFE10 indexes it with a palette byte, so it
       is exactly 256 entries and a copy of that size reads what the remap
       reads and nothing more. */
    tagpu_pk_copy(s_fogsh, t, sizeof s_fogsh);
    s_fogshPtr = t; s_fogshOk = 1; s_cFogshCopies++;
}

/* the eye rounded to the lattice the overlay anchors on: cell (0,0)'s world
   point is 32*col0 + 16, col0 being the builder's half-cell-rounded eye>>5 */
static int fog_org(int eye) { int r = eye % 32; return eye + (r > 15 ? 16 : -16) - r; }

static unsigned fill_fog(TAGPU_PACKET* p, const char* ta, unsigned* cursor)
{
    unsigned need = *cursor, e;
    const int* fg = *(const int* const*)(ta + FOG_DESC);
    if (ptr_ok(fg)) {
        const unsigned short* buf = (const unsigned short*)(size_t)fg[0];
        int cols = fg[1], rows = fg[2], cells = fg[3];
        if (ptr_ok(buf) && cols > 0 && rows > 0 &&
            cols <= FOGW_ENGINE_DIMCAP && rows <= FOGW_ENGINE_DIMCAP &&
            cells == (((cols * rows) + 7) & ~7)) {
            int ex = p->eye[0], ey = p->eye[1];
            /* the eye the grid was ANCHORED at, latched inside the fog site
               when the engine's builder ran, not the one this packet carries:
               between the two the engine's own camera stepper may have moved
               the camera, and the grid does not follow until the next draw */
            tagpu_terrown_fog_eye(&ex, &ey);
            p->fog_cols = cols; p->fog_rows = rows;
            p->fog_org[0] = fog_org(ex);
            p->fog_org[1] = fog_org(ey);
            e = append_area(p, cursor, buf, (unsigned)cols * (unsigned)rows * 2u,
                            &p->fog_off, &p->fog_len, TAGPU_PK_TRUNC_FOG);
            if (e > need) need = e;
            if (!p->fog_len) { p->fog_cols = 0; p->fog_rows = 0; }
        } else {
            s_cFogRefused++;
        }
    } else {
        s_cFogRefused++;
    }
    {
        const unsigned short* wb; int wc, wr, wox, woy;
        if (tagpu_fogwide_current(&wb, &wc, &wr, &wox, &woy) &&
            wc > 0 && wr > 0 && wc <= TAGPU_PK_FOG_DIMCAP && wr <= TAGPU_PK_FOG_DIMCAP) {
            p->fogw_cols = wc; p->fogw_rows = wr;
            p->fogw_org[0] = wox; p->fogw_org[1] = woy;
            e = append_area(p, cursor, wb, (unsigned)wc * (unsigned)wr * 2u,
                            &p->fogw_off, &p->fogw_len, TAGPU_PK_TRUNC_FOGW);
            if (e > need) need = e;
            if (!p->fogw_len) { p->fogw_cols = 0; p->fogw_rows = 0; }
            else s_cFogwSeen++;
        }
    }
    s_lastFogC = p->fog_cols;  s_lastFogR = p->fog_rows;
    s_lastFogwC = p->fogw_cols; s_lastFogwR = p->fogw_rows;
    fogshade_snapshot();
    if (s_fogshOk) {
        e = append_area(p, cursor, s_fogsh, (unsigned)sizeof s_fogsh,
                        &p->fogsh_off, &p->fogsh_len, TAGPU_PK_TRUNC_FOGSH);
        if (e > need) need = e;
    }
    return need;
}


/* ==================== THE GL UI'S RENDER HALF (landing 4c) =================
   The four engine reads tagpu_gui_surf.c made on the render thread, every
   present. The queue itself is untouched: it carries an op STREAM into
   retained twins and stays a queue (the plan's §9). What moves is the
   per-frame state the render half read beside it.

   THE MINIMAP SURFACES ARE THE ONLY EXPENSIVE ONE and they are gated on the
   consumer asking (tagpu_gui_want_minimap), because at k = 1 the sharp minimap
   is deliberately the engine's own and the copy would be paid for nothing.
   Their DESCRIPTORS are what made the old read dangerous rather than merely
   stale: each carries a base and a pitch, so a torn one is a wild read and not
   a wrong picture. Their lifetime is the minimap build's — 0x4669B0, from the
   level load at 0x4919C3, stores the three pointers once, and 0x466AA0 frees
   and NULLS them inside the teardown cascade — and every field is cross-checked
   here, on the thread that owns them, exactly as the render half checked them.

   THE PICTURE IS DECODED HERE TOO, on the level's first in-play publish. It
   used to be decoded by an observer on the LOADER thread (before_minimap), the
   one publisher this plan's rule forbids outright; the engine's own ordering is
   what makes decoding it here correct instead — the in-play handler is
   installed only after the loader has set bit 1 of main+0x38D75, so on the
   first in-play draw of a level every per-map pointer is final. */

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
#define GFX_CUR_REC  0x1B2        /* the cursor sprite: a GAF frame header    */
#define GFX_CUR_X    0x1B6        /* where the engine last drew it            */
#define GFX_CUR_Y    0x1BA

/* the interleave the render half used to do per frame, per row */
static unsigned char s_mmRg[TAGPU_PK_MM_DIMCAP * TAGPU_PK_MM_DIMCAP * 3];
/* the level's picture: what was sent, for the heartbeat. The bytes are the GL
   UI observer's — see fill_gui — and are copied straight out of it. */
static int s_mmPicW, s_mmPicH, s_mmPicGen = -1;
static unsigned s_mmPicSrcGen;       /* the decoder's generation we last sent */
/* the picture rides in ONE packet per level; this says it already has */
static int s_mmPicSent;
static volatile unsigned s_cMmCopies, s_cMmRefused, s_cMmPic;
static volatile int s_lastMmW, s_lastMmH;

/* The three values the render half's `cursor_rect` used to take live out of the
   graphics globals, in exactly its shape: an unreadable globals block is
   (-1, -1, 0, 0) — no rect, erase nothing — and a readable one with an
   unreadable sprite record is the position with the 64x64 fallback size. The
   out-of-game packet zeroes the header and never reaches here, so the consumer
   also treats a zero size as "no cursor state this frame". */
static void fill_cursor(TAGPU_PACKET* p)
{
    const char* g = *(const char* const*)TA_GFX_PP;
    const unsigned short* rec;
    p->cur_pos[0] = -1; p->cur_pos[1] = -1;
    p->cur_w = 0; p->cur_h = 0;
    if (!ptr_ok(g)) return;
    p->cur_pos[0] = RD32(g, GFX_CUR_X);
    p->cur_pos[1] = RD32(g, GFX_CUR_Y);
    rec = *(const unsigned short* const*)(g + GFX_CUR_REC);
    if (!ptr_ok(rec)) { p->cur_w = 64; p->cur_h = 64; return; }
    /* the record IS a GAF frame header, so its first two u16 are the size the
       render half read out of it; the frame itself crosses as a key */
    p->cur_rec = (unsigned)(size_t)rec;
    p->cur_w = rec[0]; p->cur_h = rec[1];
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

    /* THE LEVEL'S PICTURE, INTO ITS FIRST IN-PLAY PACKET AND NO OTHER.
       It cannot be decoded here: `main+0x1426B` is alive only inside
       BuildMinimapSurface 0x466780, which consumes the frame at 0x46684F, and
       the loader frees the picture at 0x483DF3/0x483E0B — by the first in-play
       draw there is nothing left to decode. So the GL UI's observer at that
       entry still decodes it, on the LOADER thread, into a buffer of ours; what
       landing 4c removes is the render thread ever reading that buffer.

       THE ORDERING IS THE ENGINE'S OWN, not a barrier of ours: the loader's
       last act sets bit 1 of main+0x38D75 and the game-screen handler installs
       the in-play frame handler only after testing it, so the decode is
       complete before any in-play publish can run. That is the same ordering
       the whole exchange's level story rests on. */
    if (!s_mmPicSent && s_mmPicGen != (int)p->level_gen) {
        const unsigned char* pic; int w, h; unsigned gen = 0;
        /* THE DECODER'S OWN GENERATION IS THE FRESHNESS TEST, not the level's.
           `tagpu_gui_minimap_pic_game` hands back the last picture it decoded
           SUCCESSFULLY, whatever level that was, so a level whose decode failed
           would otherwise put the PREVIOUS map's minimap in this level's first
           packet — a picture of the wrong world, which is worse than none. An
           unchanged generation means this level produced no picture, and the
           consumer then keeps drawing the engine's own minimap. */
        if (tagpu_gui_minimap_pic_game(&pic, &w, &h, &gen) && gen != s_mmPicSrcGen &&
            w > 0 && h > 0 && w <= TAGPU_PK_MM_DIMCAP && h <= TAGPU_PK_MM_DIMCAP) {
            p->mmpic_w = w; p->mmpic_h = h;
            e = append_area(p, cursor, pic, (unsigned)w * (unsigned)h,
                            &p->mmpic_off, &p->mmpic_len, TAGPU_PK_TRUNC_MMPIC);
            if (e > need) need = e;
            if (p->mmpic_len) {                  /* it landed: no later packet carries it */
                s_mmPicSent = 1; s_mmPicGen = (int)p->level_gen;
                s_mmPicSrcGen = gen;
                s_mmPicW = w; s_mmPicH = h; s_cMmPic++;
            } else {
                p->mmpic_w = p->mmpic_h = 0;     /* it did not fit: the next packet tries */
            }
        }
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
   field itself (tagpu_pal.c, converted by landing 2). The gamma is BOUNDED
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

/* the in-play frame: every field of the header, from the thread that owns it */
static unsigned fill_frame(TAGPU_PACKET* p, void* ctx)
{
    const char* ta = ta_main();
    unsigned cursor = sizeof(TAGPU_PACKET), need = sizeof(TAGPU_PACKET), e;
    int L, T, W, H;
    (void)ctx;

    /* zero everything the header carries, then fill; the tail area is bounded
       by cap_bytes, which the primitive set before calling us */
    tagpu_pk_fill((unsigned char*)p + offsetof(TAGPU_PACKET, used_bytes), 0,
                  sizeof(TAGPU_PACKET) - offsetof(TAGPU_PACKET, used_bytes));
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
    tagpu_zoom_applied(&p->cmd_ack_seq, &p->cmd_ack_dx, &p->cmd_ack_dy, &p->zoom_applied, &p->cmd_epoch);
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
    p->watched      = RDU8(ta, OFF_WATCHED);
    p->sea_level    = RDU8(ta, OFF_SEALEVEL);
    p->paused       = RDU8(ta, OFF_GAMEPAUSED) & 1u;
    p->game_speed   = RDI16(ta, OFF_GAMESPEED_LIVE);
    p->text_fg      = s_textFg;
    /* what the marker pass reads and used to take from engine memory itself:
       the GUI colour bytes, the dispatched mouse point, the build cursor's two
       corners and the two mode bytes that gate them */
    tagpu_pk_copy(p->gui_col, ta + OFF_GUICOL, sizeof p->gui_col);
    p->mouse[0]     = RD32(ta, OFF_MOUSE_X);
    p->mouse[1]     = RD32(ta, OFF_MOUSE_Y);
    {
        int k;
        for (k = 0; k < 6; k++) p->build_rect[k] = RD32(ta, OFF_BUILDRECT + k * 4);
    }
    p->cursor_mode  = RDU8(ta, OFF_CURMODE);
    p->region_flags = RDU8(ta, OFF_REGIONFL);
    p->game_opt     = RDU8(ta, OFF_GFXOPT);

    /* ---- the world tables ---- */
    e = fill_world(p, ta, &cursor);
    if (e > need) need = e;
    /* ---- the effects and the particle layers (landing 4a) ---- */
    e = fill_fx(p, ta, &cursor);
    if (e > need) need = e;
    /* ---- the two fog grids (landing 4b) ---- */
    e = fill_fog(p, ta, &cursor);
    if (e > need) need = e;
    /* ---- the GL UI's render half (landing 4c) ---- */
    e = fill_gui(p, ta, &cursor);
    if (e > need) need = e;
    shd_snapshot();
    if (s_shdOk) {
        e = append_area(p, &cursor, s_shd, (unsigned)sizeof s_shd, &p->shd_off, &p->shd_len,
                        TAGPU_PK_TRUNC_SHD);
        if (e > need) need = e;
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
           "no font" for as long as the engine keeps the same font (landing
           review). A REFUSED or empty font (s_fontLen == 0) keeps its
           generation: the consumer must blank, and stay blank. */
        if (s_fontLen) p->font_gen = 0;
    }

    /* stress: a dummy table past a one-page commit, so the growth path runs */
    if (s_stress) {
        e = append_area(p, &cursor, NULL, 6144u, &p->stress_off, &p->stress_len, TAGPU_PK_TRUNC_STRESS);
        if (e > need) need = e;
    }
    p->used_bytes = cursor;
    return need;
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
    tagpu_zoom_applied(&p->cmd_ack_seq, &p->cmd_ack_dx, &p->cmd_ack_dy, &p->zoom_applied, &p->cmd_epoch);
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
       first one, and the shell must not inherit a widened rect or a scaled
       ScrollSpeed (its options screen shows that byte) */
    if (s_installed && !s_countOnly) {
        char* ta = (char*)ta_main();
        if (ta) { tagpu_zoom_level_end(ta); tagpu_vpwide_level_end(ta); }
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
    s_mmPicGen = -1; s_mmPicW = s_mmPicH = 0; s_mmPicSent = 0;
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
        return 0;
    }
    s_cDraws++;
    s_levelDraws++;
    if (s_countOnly) return 0;
    if (s_retDepth >= RET_DEPTH) { s_cDeep++; return 0; }
    s_retStack[s_retDepth++] = (void*)(size_t)ret;
    /* THE COMMANDS, on the thread that owns every word they write. This
       runs after whichever of the frame callback's own camera writers ran
       this frame — the stepper 0x41CA10 (0x495599 inside 0x495490, called at
       0x49680C/0x49693E) and the scroll poll 0x41CE90 (0x496976) both precede
       the draw call at 0x4969CD, and both can be skipped: the stepper when the
       sim is paused, both under an in-game GUI screen — and before the draw's
       first read of the eye at 0x468DD9; no store to the eye exists inside
       DrawGameScreen (0x468CF0..0x46A200), so a delta applied here composes
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
        if (ta) {
            tagpu_zoom_apply(ta, c);
            tagpu_vpwide_apply(ta, c);
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
    /* post-flip: the packet. The FRESH gate inside makes most of these a load. */
    if (tagpu_packet_publish(fill_frame, NULL, 0) && !s_levelOpen) {
        char b[260];
        s_levelOpen = 1;
        _snprintf(b, sizeof b,
                  "packet: level gen %u: first in-play packet at draw #%u (tick %u, load flags now 0x%04X, at the first non-in-play draw after the teardown 0x%04X, loader thread %u entered %u time(s), game thread %u)",
                  s_levelGen, s_cDraws, s_lastTick, load_flags(), s_shellFlags,
                  (unsigned)s_loaderTid, s_loaderEntries, (unsigned)s_gameTid);
        b[sizeof b - 1] = 0; plog(b);
    }
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
                  " | fx: proj=%u expl=%u deb=%u part=%u/%u scan=%u/%u trunc=%u layerbad=%u subbad=%u lht=%u want=%d/%d"
                  " | fog: %dx%d wide=%dx%d/%u refused=%u shade=%u"
                  " | gui: mm=%dx%d/%u refused=%u pic=%dx%d/%u",
                  s_cLastProj, s_cLastExpl, s_cLastDebris, s_cLastPart, s_cPartMax,
                  s_cFxScan, s_cFxReuse, s_cPartTrunc, s_cLayerBad, s_cSubBad, s_cLhtCopies,
                  tagpu_fxown_want_fx(), tagpu_fxown_want_sfx(),
                  s_lastFogC, s_lastFogR, s_lastFogwC, s_lastFogwR, s_cFogwSeen,
                  s_cFogRefused, s_cFogshCopies,
                  s_lastMmW, s_lastMmH, s_cMmCopies, s_cMmRefused,
                  s_mmPicW, s_mmPicH, s_cMmPic);
    }
    if (cap) buf[cap - 1] = 0;
    lastAll = all; lastIn = in;
}

/* ---- the level-end packet's second provider -------------------------------
   THE OUT-OF-GAME PACKET MUST NOT DEPEND ON ANOTHER MODULE BEING ARMED
   (landing review): tagpu_reclaim's teardown wrap publishes it when reclaim
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
   The level generation is reclaim's counter, which does not move when
   reclaim is off — one counter, as the plan requires, and 0 for the session
   in that case. */
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

/* ---- install ------------------------------------------------------------- */

void tagpu_packet_pub_init(void)
{
    char b[400];
    int drawOk, loaderOk;
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
    tagpu_packet_set_extra(extra);
    _snprintf(b, sizeof b,
              "packet: publisher %s on DrawGameScreen 0x468CF0 (in-play gate: return address 0x4969D2; "
              "chained after tagpu_menu's observer), level-end packet by %s, loader-thread observer at 0x497C70=%d, game thread %u%s",
              s_countOnly ? "COUNT-ONLY" : "ARMED",
              s_levelEndBy == 1 ? "tagpu_reclaim's teardown post hook" : s_levelEndBy == 2 ? "our own observer on the teardown 0x491B60 (reclaim is not armed; the level generation stays 0)" : "nobody",
              loaderOk, (unsigned)s_gameTid,
              s_countOnly ? " — nothing is published, taken or applied: no world pass draws, no command is applied (the engine's own camera range, rect and scroll rate), every string through tagpu_text_place draws nothing" : "");
    b[sizeof b - 1] = 0;
    plog(b);
}
