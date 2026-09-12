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
     before  = reserved for the commands (landing 2); a no-op today
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
#include "tagpu_gui.h"

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
    if (!ta) return need;                                  /* in_game stays 0: fail closed */

    p->in_game   = 1;
    p->tick      = RDU32(ta, OFF_GAMETIME);
    if (!s_haveTick || p->tick != s_lastTick) {
        QueryPerformanceCounter(&s_tickStart);
        s_lastTick = p->tick; s_haveTick = 1;
    }
    p->tick_start_lo = s_tickStart.LowPart;
    p->tick_start_hi = (uint32_t)s_tickStart.HighPart;
    p->level_gen   = tagpu_reclaim_level_gen();
    p->cmd_ack_seq = 0;
    p->gui_flips   = tagpu_gui_flips();
    p->draw_seq    = s_cDraws;
    p->eye[0]       = RD32(ta, OFF_EYE_X);      p->eye[1]       = RD32(ta, OFF_EYE_Y);
    p->scroll_to[0] = RD32(ta, OFF_SCROLLTO_X); p->scroll_to[1] = RD32(ta, OFF_SCROLLTO_Y);
    tagpu_vpwide_true_rect(ta, &L, &T, &W, &H);
    p->vp[0] = L; p->vp[1] = T; p->vp[2] = W; p->vp[3] = H;
    p->screen[0] = RD32(ta, OFF_SCREEN_W);  p->screen[1] = RD32(ta, OFF_SCREEN_H);
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

/* the level end: header only, in_game = 0, the new generation */
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
    return sizeof(TAGPU_PACKET);
}

void tagpu_packet_pub_level_end(unsigned level_gen)
{
    char b[200];
    unsigned flags = load_flags();
    if (!on_game_thread()) {
        s_cForeign++;
        _snprintf(b, sizeof b, "packet: level end on thread %u, not the game thread %u — NOT published (foreign=%u)",
                  (unsigned)GetCurrentThreadId(), (unsigned)s_gameTid, s_cForeign);
        b[sizeof b - 1] = 0; plog(b);
        return;
    }
    if (s_installed && !s_countOnly)
        tagpu_packet_publish(fill_level_end, &level_gen, 1 /* past the FRESH gate */);
    _snprintf(b, sizeof b, "packet: level end -> gen %u: in_game=0 published%s; %u in-play draw(s) this level; load flags 0x%04X",
              level_gen, (s_installed && !s_countOnly) ? "" : " (NOT: module off)", s_levelDraws, flags);
    b[sizeof b - 1] = 0; plog(b);
    s_levelOpen = 0; s_levelDraws = 0; s_shellSeen = 0; s_shellFlags = 0xFFFFu;
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
    /* landing 2: apply the commands here — post-tick, pre-draw */
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
                  tagpu_reclaim_level_gen(), s_cDraws, s_lastTick, load_flags(), s_shellFlags,
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
              (unsigned)s_loaderTid, s_loaderEntries, (unsigned)s_gameTid, load_flags(), tagpu_reclaim_level_gen(), s_cDraws);
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
              (unsigned)GetCurrentThreadId(), load_flags(), tagpu_reclaim_level_gen(), s_cDraws,
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
    unsigned all = s_cDrawsAll, in = s_cDraws;
    _snprintf(buf, cap, " | draws=%u inplay=%u draws/s=%.0f inplay/s=%.0f foreign=%u deep=%u fontcopies=%u/%u levelend=%s",
              all, in,
              secs > 0.0 ? (double)(all - lastAll) / secs : 0.0,
              secs > 0.0 ? (double)(in - lastIn) / secs : 0.0,
              s_cForeign, s_cDeep, s_cFontCopies, s_cFontRefused,
              s_levelEndBy == 1 ? "reclaim" : s_levelEndBy == 2 ? "own" : "none");
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
                 "teardown 0x491B60 could not be observed, so a level's last packet would outlive the level");
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
              s_countOnly ? " — nothing is published or taken: every string through tagpu_text_place draws nothing (the group digits, the ShowRanges labels, the FPS readout)" : "");
    b[sizeof b - 1] = 0;
    plog(b);
}
