/* tagpu_hudstate.c -- the replay HUD's state, from the game thread to the render thread.
   Contract: tagpu_hudstate.h.

   NOTHING HERE NAMES ENGINE MEMORY. The game-thread half reads through the replay's calls
   (inc/tagpu_replay.h), the markers' walker and the HUD's input, all of them on the thread that
   owns what they read; the render-thread half reads the packet's copy, the presented palette and
   the HUD scale's one published word, and keeps the hit map of the frame it drew. */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu.h"
#include "tagpu_hudstate.h"
#include "tagpu_hudinput.h"
#include "tagpu_hudmarks.h"
#include "tagpu_replay.h"
#include "tagpu_pal.h"
#include "tagpu_hud.h"
#include "tagpu_log.h"
#include "dd.h"
#include "render_vk.h"

#define TICKS 30.0f                     /* GameTime runs at 30 a second */

/* ---- the game thread ---------------------------------------------------------------------- */

static volatile LONG s_refused;         /* the render thread: the HUD cannot draw this session  */

int tagpu_hudstate_on(void)
{
    /* drawn only on the Vulkan lane, the packet's only reader, and only while the pass has not
       refused: on GDI, after the lane falls back to it (render_vk.c), or once the HUD cannot draw,
       a replay has no on-screen controls */
    return g_ddraw.renderer == vk_render_main && !InterlockedCompareExchange(&s_refused, 0, 0);
}

void tagpu_hudstate_refuse(const char* why)
{
    if (InterlockedExchange(&s_refused, 1) == 0)
        tagpu_logf("hud: the replay HUD cannot draw (%s) - a replay has no on-screen controls this "
                   "session", why);
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
    h->scale = (uint16_t)tagpu_hudinput_scale();

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
        s->pal = (uint8_t)pl[i].colour;
        s->lost = (uint8_t)(pl[i].commander_losses > 0);
        s->pov = (uint8_t)(pl[i].pov != 0);
        h->nseat++;
    }

    n = tagpu_hudmarks_get(T, kind, seat, (int)TAGPU_PK_HUD_MARKS);
    for (i = 0; i < n && i < (int)TAGPU_PK_HUD_MARKS; i++) {
        h->mark[i].t = T[i];
        h->mark[i].kind = kind[i];
        h->mark[i].seat = seat[i];
    }
    h->nmark = (uint16_t)i;
    tagpu_hudinput_fill(h);
    return 1;
}

/* ---- the render thread -------------------------------------------------------------------- */

static HUDSTATE s_st;
/* every marker the packet carries -- the walker's deaths and defeats, then the bookmarks -- fits */
typedef char hud_marks_fit[(TAGPU_PK_HUD_MARKS <= HUD_MAXMARK) ? 1 : -1];
static int      s_have;
static unsigned s_frame;                /* the render-thread frame `s_st` is for */
static float    s_px;
static int      s_ox, s_oy;
static uint8_t  s_seatOf[TAGPU_PK_HUD_SEATS];   /* row i's recorded seat, this frame's   */
/* THE HIT MAP OF THE LAST FRAME DRAWN: written by the Vulkan pass once it has built a frame it will
   draw (tagpu_hudstate_drawn), read by the next command record (tagpu_hudstate_cmd) and the dump,
   all on this thread. A frame that draws no HUD empties it. */
static TAGPU_CMD_HUD s_map;
static int      s_mapCut;               /* a frame had more controls than the map: said once */

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
   with, who ally with it back. Every group is a team, a lone player too, in the players' order;
   whether they get headers is the layout's rule (tagpu_hudlay.c, `headed`): none in a
   free-for-all. */
static void teams(const TAGPU_PK_HUD* h, HUDSTATE* st)
{
    int par[TAGPU_PK_HUD_SEATS], tof[TAGPU_PK_HUD_SEATS], i, j, n = st->nseat;
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
    fprintf(f, "],\"toggles\":{");
    for (i = j = 0; i < 32; i++) {
        const char* n = tagpu_hudlay_toggle_name(1u << i);
        if (n) fprintf(f, "%s\"%s\":%s", j++ ? "," : "", n, st->toggles >> i & 1 ? "true" : "false");
    }
    fprintf(f, "},\"sight\":%d,\"follow\":%d,\"scroll\":%d,\"toast\":", st->sight, st->follow, st->scroll);
    jstr(f, st->toast);
    /* the controls of the frame on screen, in client pixels, topmost first: what `tacli hud`
       clicks */
    fprintf(f, ",\"held\":");
    jstr(f, st->held);
    fprintf(f, "},\"hits\":{\"track\":[%d,%d],\"first\":%u,\"last\":%u,\"at\":[",
            s_map.track_x0, s_map.track_x1, (unsigned)s_map.first, (unsigned)s_map.last);
    for (i = 0; i < (int)s_map.n; i++) {
        const TAGPU_CMD_HUDHIT* t = &s_map.hit[i];
        fprintf(f, "%s{\"key\":", i ? "," : "");
        jstr(f, t->key);
        fprintf(f, ",\"r\":[%d,%d,%d,%d]}", t->x0, t->y0, t->x1, t->y1);
    }
    fprintf(f, "]}}\n");
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

    /* THE HUD'S OWN STATE, as its input keeps it (tagpu_hudinput.c): the pointer in units, and
       Sight and Follow from recorded seats to rows */
    st->mx = st->my = -1;
    if (h->px >= 0) {
        st->mx = (float)(h->px - s_ox) / u;
        st->my = (float)(h->py - s_oy) / u;
    }
    st->folded = h->folded;
    st->toggles = h->toggles;
    st->scroll = h->scroll;
    st->sight = st->follow = -1;
    for (i = 0; i < st->nseat; i++) {
        s_seatOf[i] = h->seat[i].seat;
        if (h->sight >= 0 && h->seat[i].seat == (uint8_t)h->sight) st->sight = i;
        if (h->follow >= 0 && h->seat[i].seat == (uint8_t)h->follow) st->follow = i;
    }
    memcpy(st->held, h->held, sizeof st->held);
    st->held[sizeof st->held - 1] = 0;
    memcpy(st->toast, h->toast, sizeof st->toast);
    st->toast[sizeof st->toast - 1] = 0;
    s_px = u;
    s_frame = f->frame_counter;
    s_have = 1;
    if ((f->frame_counter % 30u) == 0u && GetFileAttributesA(DUMP_ASK) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA(DUMP_ASK);
        dump(st, vw, vh, u, q8);
    }
}

static int16_t dev(float v)
{
    float r = floorf(v + 0.5f);
    return (int16_t)(r < -32768.0f ? -32768 : r > 32767.0f ? 32767 : r);
}

void tagpu_hudstate_drawn(const HUDOPS* ops)
{
    int i, cut = 0;
    s_map.n = 0;
    if (!ops || !s_have) return;
    /* topmost first: the build paints in order, so the last control painted is on top */
    for (i = ops->n - 1; i >= 0; i--) {
        const HUDOP* p = &ops->op[i];
        TAGPU_CMD_HUDHIT* t;
        if (p->kind != HOP_HOT) continue;
        if (s_map.n >= TAGPU_CMD_HUDHITS) { cut++; continue; }
        t = &s_map.hit[s_map.n++];
        t->x0 = dev((float)s_ox + p->r.x * s_px);
        t->y0 = dev((float)s_oy + p->r.y * s_px);
        t->x1 = dev((float)s_ox + (p->r.x + p->r.w) * s_px);
        t->y1 = dev((float)s_oy + (p->r.y + p->r.h) * s_px);
        lstrcpynA(t->key, ops->text + p->run, sizeof t->key);
    }
    if (cut && !s_mapCut) {
        s_mapCut = 1;
        tagpu_logf("hud: %d controls past the hit map's %u are not clickable (said once)", cut,
                   TAGPU_CMD_HUDHITS);
    }
    s_map.track_x0 = dev((float)s_ox + ops->track_x0 * s_px);
    s_map.track_x1 = dev((float)s_ox + ops->track_x1 * s_px);
    s_map.first = (uint8_t)(ops->first < 0 ? 0 : ops->first > 255 ? 255 : ops->first);
    s_map.last = (uint8_t)(ops->last < 0 ? 0 : ops->last > 255 ? 255 : ops->last);
    s_map.nseat = (uint8_t)(s_st.nseat < (int)TAGPU_PK_HUD_SEATS ? s_st.nseat : (int)TAGPU_PK_HUD_SEATS);
    memcpy(s_map.seat, s_seatOf, sizeof s_map.seat);
}

void tagpu_hudstate_cmd(TAGPU_CMD_HUD* out)
{
    *out = s_map;
}

int tagpu_hudstate_frame(unsigned frame, const HUDSTATE** st, float* px, int* ox, int* oy)
{
    /* this frame's state or none: a frame whose overlay returned before `present` (a teardown,
       tagpu_overlay.off) would otherwise draw the last one it saw (tagpu_vk_pass.h, `frame`) */
    if (!s_have || s_frame != frame) return 0;
    *st = &s_st;
    *px = s_px;
    *ox = s_ox;
    *oy = s_oy;
    return 1;
}
