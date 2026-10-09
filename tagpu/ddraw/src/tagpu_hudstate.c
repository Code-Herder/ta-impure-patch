/* tagpu_hudstate.c -- the replay HUD's state, from the game thread to the render thread.
   Contract: tagpu_hudstate.h.

   NOTHING HERE NAMES ENGINE MEMORY. The game-thread half reads through the replay's calls
   (inc/tagpu_replay.h), the dock's markers and the cards' colour rule, all of them on the thread
   that owns what they read; the render-thread half reads the packet's copy, the presented
   palette and the HUD scale's one published word. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu.h"
#include "tagpu_hudstate.h"
#include "tagpu_replay.h"
#include "tagpu_dock.h"
#include "tagpu_cards.h"
#include "tagpu_settings.h"
#include "tagpu_opt.h"
#include "tagpu_pal.h"
#include "tagpu_hud.h"
#include "tagpu_log.h"
#include "dd.h"
#include "render_vk.h"

#define LEVER "tagpu_replayhud.on"
#define TICKS 30.0f                     /* GameTime runs at 30 a second */

/* ---- the game thread ---------------------------------------------------------------------- */

static volatile LONG s_on = -1;         /* -1 unread, 0 off, else the scale's lever value or 1 */

int tagpu_hudstate_on(void)
{
    LONG v = InterlockedCompareExchange(&s_on, 0, 0);
    if (v < 0) {
        char b[128];
        const char* k;
        int pct;
        v = 0;
        if (tagpu_opt_read(LEVER, b, sizeof b) >= 0) {
            /* `scale=N`, as tagpu_hud.on carries the game's HUD scale */
            pct = (k = strstr(b, "scale=")) ? atoi(k + 6) : 0;
            v = pct == 75 || pct == 100 || pct == 125 || pct == 150 ? pct : 1;
        }
        /* the game thread's fill and the dock's gate both ask: the first answer stands */
        if (InterlockedCompareExchange(&s_on, v, -1) == -1 && v)
            tagpu_logf("hud: the replay HUD is on (%s)%s", LEVER, v > 1 ? ", scale from the lever" : "");
        v = InterlockedCompareExchange(&s_on, 0, 0);
    }
    /* drawn only on the Vulkan lane, the packet's only reader: on GDI, or after the lane falls
       back to it (render_vk.c), the HUD is off and the dock and the cards stand where they were */
    return v != 0 && g_ddraw.renderer == vk_render_main;
}

static unsigned scale_pct(void)
{
    LONG v = InterlockedCompareExchange(&s_on, 0, 0);
    int s;
    if (v > 1) return (unsigned)v;
    if (tagpu_settings_get(TS_REPLAYHUD, &s) && (s == 75 || s == 100 || s == 125 || s == 150))
        return (unsigned)s;
    return 100;
}

int tagpu_hudstate_fill(TAGPU_PK_HUD* h, unsigned now)
{
    TAGPU_REPLAY_PLAYER pl[TAGPU_PK_HUD_SEATS];
    unsigned T[TAGPU_PK_HUD_MARKS], first, last;
    unsigned char kind[TAGPU_PK_HUD_MARKS], seat[TAGPU_PK_HUD_MARKS];
    int n, i;

    if (!tagpu_hudstate_on() || !tagpu_replay_live() || !tagpu_replay_span(&first, &last)) return 0;
    memset(h, 0, sizeof *h);
    h->now = now;
    h->first = first;
    h->last = last;
    h->speed = (uint16_t)tagpu_replay_speed();
    h->paused = (uint8_t)(tagpu_replay_paused() != 0);
    h->scale = (uint16_t)scale_pct();

    n = tagpu_replay_players(pl, (int)TAGPU_PK_HUD_SEATS);
    for (i = 0; i < n && i < (int)TAGPU_PK_HUD_SEATS; i++) {
        TAGPU_PK_HUDSEAT* s = &h->seat[h->nseat];
        if (pl[i].seat >= TAGPU_PK_HUD_SEATS) continue;
        memcpy(s->name, pl[i].name, sizeof s->name);
        s->name[sizeof s->name - 1] = 0;
        s->m = pl[i].metal;   s->mmax = pl[i].metal_max;  s->mi = pl[i].metal_in;  s->mo = pl[i].metal_out;
        s->e = pl[i].energy;  s->emax = pl[i].energy_max; s->ei = pl[i].energy_in; s->eo = pl[i].energy_out;
        s->units = pl[i].units;
        s->allies = (uint16_t)pl[i].allies;
        s->seat = (uint8_t)pl[i].seat;
        s->pal = (uint8_t)tagpu_cards_colour(pl[i].colour);
        s->lost = (uint8_t)(pl[i].commander_losses > 0);
        h->nseat++;
    }

    n = tagpu_dock_marks(T, kind, seat, (int)TAGPU_PK_HUD_MARKS);
    for (i = 0; i < n && i < (int)TAGPU_PK_HUD_MARKS; i++) {
        h->mark[i].t = T[i];
        h->mark[i].kind = kind[i];
        h->mark[i].seat = seat[i];
    }
    h->nmark = (uint16_t)i;
    return 1;
}

/* ---- the render thread -------------------------------------------------------------------- */

static HUDSTATE s_st;
static int      s_have;
static float    s_px;
static int      s_ox, s_oy;

/* THE NAMES IN WHAT THE FACES CARRY. A recording's names are the engine's 8-bit text, read as
   Latin-1, and the faces are cut to ASCII and Latin-1 (tools/hudart/fonts.py): a control code
   -- below 0x20, or 0x80..0x9F, which Latin-1 leaves to them -- is '?', and the soft hyphen,
   which Oswald lacks, is '-'. Nothing reaches the rasteriser that it would draw as a missing glyph. */
static void latin1_utf8(char* dst, size_t cap, const char* src, size_t n)
{
    size_t i, k = 0;
    for (i = 0; i < n && src[i] && k + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c < 0x20 || (c >= 0x80 && c < 0xA0)) c = '?';
        else if (c == 0xAD) c = '-';
        if (c < 0x80) dst[k++] = (char)c;
        else {
            if (k + 2 >= cap) break;
            dst[k++] = (char)(0xC0 | (c >> 6));
            dst[k++] = (char)(0x80 | (c & 0x3F));
        }
    }
    dst[k] = 0;
}

static int root(int* par, int i)
{
    while (par[i] != i) i = par[i] = par[par[i]];
    return i;
}

/* THE TEAMS ARE THE MUTUAL ALLIANCES, joined transitively: the rows a player and those it allies
   with, who ally with it back. A game with no alliance is one team, drawn without headers, as the
   mockup draws one; otherwise every group is a team, a lone player too, in the players' order. */
static void teams(const TAGPU_PK_HUD* h, HUDSTATE* st)
{
    int par[TAGPU_PK_HUD_SEATS], tof[TAGPU_PK_HUD_SEATS], i, j, n = st->nseat, big = 0;
    for (i = 0; i < n; i++) { par[i] = i; tof[i] = -1; }
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            if ((h->seat[i].allies >> h->seat[j].seat & 1) && (h->seat[j].allies >> h->seat[i].seat & 1))
                par[root(par, j)] = root(par, i);
    st->nteam = 0;
    for (i = 0; i < n; i++) {
        int r = root(par, i);
        if (tof[r] < 0) { tof[r] = st->nteam; st->team_n[st->nteam++] = 0; }
        st->team_seat[tof[r]][st->team_n[tof[r]]++] = (uint8_t)i;
        if (st->team_n[tof[r]] > big) big = st->team_n[tof[r]];
    }
    if (big <= 1) {
        st->nteam = 1;
        st->team_n[0] = n;
        for (i = 0; i < n; i++) st->team_seat[0][i] = (uint8_t)i;
    }
}

/* ---- the dump the mockup draws from (tools/hudart/mockup/index.html, `state=`) ---------------
   `tagpu_replayhud.dump` in the game folder: the next HUD frame writes tagpu_replayhud.json -- the
   state in the mockup's own shape, in HUD units, with the device size, the unit and the engine
   frame's scale -- and deletes the trigger, so the mockup can draw the frame the game drew. */
#define DUMP_ASK "tagpu_replayhud.dump"
#define DUMP_OUT "tagpu_replayhud.json"

static void jstr(FILE* f, const char* s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static void dump(const HUDSTATE* st, int vw, int vh, float u, int q8)
{
    static const char* const KIND[3] = { "death", "defeat", "bookmark" };
    FILE* f = fopen(DUMP_OUT ".tmp", "wb");
    int i, j;
    if (!f) { tagpu_logf("hud: the state dump could not open %s", DUMP_OUT ".tmp"); return; }
    fprintf(f, "{\"device\":{\"W\":%d,\"H\":%d},\"px\":%.9g,\"q8\":%d,\"state\":{", vw, vh, u, q8);
    fprintf(f, "\"W\":%.9g,\"H\":%.9g,\"frame\":{\"panel\":%.9g,\"top\":%.9g,\"bottom\":%.9g},",
            st->W, st->H, st->panel, st->top, st->bottom);
    fprintf(f, "\"t\":%.9g,\"T\":%.9g,\"playing\":%s,\"speed\":%.9g,\"hud\":%.9g,\"seats\":[",
            st->t, st->T, st->playing ? "true" : "false", st->speed, st->hud);
    for (i = 0; i < st->nseat; i++) {
        const HUDSEAT* q = &st->seat[i];
        fprintf(f, "%s{\"i\":%d,\"name\":", i ? "," : "", i);
        jstr(f, q->name);
        fprintf(f, ",\"colour\":\"#%06x\",\"units\":%.9g,\"lost\":%s,\"out\":%s,\"death\":null,"
                   "\"res\":{\"m\":%.9g,\"mmax\":%.9g,\"mi\":%.9g,\"mo\":%.9g,\"e\":%.9g,\"emax\":%.9g,"
                   "\"ei\":%.9g,\"eo\":%.9g}}",
                (unsigned)q->colour, q->units, q->lost ? "true" : "false", q->out ? "true" : "false",
                q->m, q->mmax, q->mi, q->mo, q->e, q->emax, q->ei, q->eo);
    }
    fprintf(f, "],\"teams\":[");
    for (i = 0; i < st->nteam; i++) {
        fprintf(f, "%s[", i ? "," : "");
        for (j = 0; j < st->team_n[i]; j++) fprintf(f, "%s%d", j ? "," : "", st->team_seat[i][j]);
        fprintf(f, "]");
    }
    fprintf(f, "],\"folded\":[");
    for (i = j = 0; i < HUD_MAXSEAT; i++)
        if (st->folded >> i & 1) fprintf(f, "%s%d", j++ ? "," : "", i);
    fprintf(f, "],\"markers\":[");
    for (i = 0; i < st->nmark; i++)
        fprintf(f, "%s{\"t\":%.9g,\"kind\":\"%s\",\"seat\":%d}", i ? "," : "", st->mark[i].t,
                KIND[st->mark[i].kind % 3], st->mark[i].seat);
    fprintf(f, "],\"toggles\":{\"stats\":%s,\"map\":%s}}}\n",
            st->toggles & HUD_TOG_STATS ? "true" : "false", st->toggles & HUD_TOG_MAP ? "true" : "false");
    if (fclose(f) != 0 || !MoveFileExA(DUMP_OUT ".tmp", DUMP_OUT, MOVEFILE_REPLACE_EXISTING)) {
        tagpu_logf("hud: the state dump could not be written (%lu)", (unsigned long)GetLastError());
        return;
    }
    tagpu_logf("hud: state dumped to %s: %d seats, %d teams, %d markers, t %.1f of %.1f s, %dx%d at %.4f px a unit",
               DUMP_OUT, st->nseat, st->nteam, st->nmark, st->t, st->T, vw, vh, u);
}

void tagpu_hudstate_present(const TAGPU_FRAME* f)
{
    const struct TAGPU_PACKET* pk = f->packet;
    const TAGPU_PK_HUD* h;
    const unsigned char* pal = tagpu_pal_live();
    HUDSTATE* st = &s_st;
    int vw = f->vp_w, vh = f->vp_h, pw = 128, bh = 32, q8 = 256, i, k;
    unsigned end;
    float u;

    s_have = 0;
    if (!pk || !pk->in_game || !(h = tagpu_pk_hud(pk))) return;
    if (vw > 0 && vh > 0) { s_ox = f->vp_x; s_oy = f->win_height - f->vp_y - vh; }
    else { vw = f->win_width; vh = f->win_height; s_ox = s_oy = 0; }
    if (f->game_width <= 0 || f->game_height <= 0 || vw <= 0 || vh <= 0 || s_oy < 0) return;

    /* THE UNIT IS THE HUD'S OWN: the screen's height / 720 times its scale, whatever the game's
       HUD scale. The engine's frame, at the game's HUD scale, only says where the space it leaves
       begins (hud-layout.md, *Units and the screen*). */
    u = (float)vh / 720.0f * (float)h->scale / 100.0f;
    tagpu_hud_live(&pw, &bh, &q8);
    memset(st, 0, sizeof *st);
    st->W = (float)vw / u;
    st->H = (float)vh / u;
    st->panel = (float)pw * (float)vw / (float)f->game_width / u;
    st->top = st->bottom = (float)bh * (float)vh / (float)f->game_height / u;

    /* the playhead stops at the recording's end; GameTime runs on past it */
    end = h->last > h->first ? h->last : h->first;
    st->T = (float)(end - h->first) / TICKS;
    st->t = (float)((h->now < h->first ? h->first : h->now > end ? end : h->now) - h->first) / TICKS;
    st->playing = !h->paused;
    st->speed = (float)h->speed / 10.0f;
    st->hud = (float)h->scale / 100.0f;

    for (i = 0; i < h->nseat && i < HUD_MAXSEAT; i++) {
        const TAGPU_PK_HUDSEAT* s = &h->seat[i];
        HUDSEAT* q = &st->seat[i];
        latin1_utf8(q->name, sizeof q->name, s->name, sizeof s->name);
        q->colour = pal ? (uint32_t)pal[s->pal * 4] << 16 | (uint32_t)pal[s->pal * 4 + 1] << 8 | pal[s->pal * 4 + 2]
                        : 0x808080;
        q->units = (float)s->units;
        q->lost = s->lost;
        q->out = s->units == 0;
        q->m = s->m; q->mmax = s->mmax; q->mi = s->mi; q->mo = s->mo;
        q->e = s->e; q->emax = s->emax; q->ei = s->ei; q->eo = s->eo;
    }
    st->nseat = i;
    teams(h, st);

    for (i = k = 0; i < h->nmark && k < HUD_MAXMARK; i++) {
        const TAGPU_PK_HUDMARK* m = &h->mark[i];
        HUDMARK* o = &st->mark[k];
        int j;
        if (m->t < h->first || m->t > end) continue;
        o->t = (float)(m->t - h->first) / TICKS;
        if (m->kind == TAGPU_PK_MK_BOOKMARK) { o->kind = HUD_MK_BOOKMARK; o->seat = -1; k++; continue; }
        for (j = 0; j < st->nseat && h->seat[j].seat != m->seat; j++) {}
        if (j == st->nseat) continue;
        o->kind = m->kind == TAGPU_PK_MK_DEATH ? HUD_MK_DEATH : HUD_MK_DEFEAT;
        o->seat = j;
        k++;
    }
    st->nmark = k;
    st->mx = st->my = -1;
    s_px = u;
    s_have = 1;
    if ((f->frame_counter % 30u) == 0u && GetFileAttributesA(DUMP_ASK) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA(DUMP_ASK);
        dump(st, vw, vh, u, q8);
    }
}

int tagpu_hudstate_frame(const HUDSTATE** st, float* px, int* ox, int* oy)
{
    if (!s_have) return 0;
    *st = &s_st;
    *px = s_px;
    *ox = s_ox;
    *oy = s_oy;
    return 1;
}
