/* tagpu_hudstate.c -- the HUD's state, from the game thread to the render thread.
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
#include "tagpu_hudart.h"
#include "tagpu_replay.h"
#include "tagpu_recfile.h"
#include "tagpu_recreader.h"
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
        tagpu_logf("hud: the HUD cannot draw (%s) - a replay has no on-screen controls and a live "
                   "game no rewind button this session", why);
}

int tagpu_hudstate_mode(void)
{
    TAGPU_REPLAY_RWSTATE rw;
    unsigned first, last;
    if (!tagpu_hudstate_on()) return HUD_MODE_NONE;
    tagpu_replay_rewind_state(&rw);
    if (rw.state != TAGPU_REPLAY_RWS_IDLE || rw.available) return HUD_MODE_LIVE;
    return tagpu_replay_live() && tagpu_replay_span(&first, &last) ? HUD_MODE_REPLAY : HUD_MODE_NONE;
}

/* THE LIVE HUD'S RECORD: the clock, the rewind's state and span, and the HUD's own state; no
   seats, no markers, no stats. tagpu_hudmarks_get is never called here: during a rewind into the
   past tagpu_replay_live is true, and the markers' walker would start on the live recording. */
static int fill_live(TAGPU_PK_HUD* h, unsigned now)
{
    TAGPU_REPLAY_RWSTATE rw;
    tagpu_replay_rewind_state(&rw);
    memset(h, 0, sizeof *h);
    h->mode = TAGPU_PK_HUD_LIVE;
    h->now = now;
    h->paused = (uint8_t)(tagpu_replay_paused() != 0);
    h->scale = (uint16_t)tagpu_hudinput_scale();
    h->first = rw.first;
    h->last = rw.last;
    h->rw_state = (uint8_t)rw.state;
    h->rw_G = rw.G;
    tagpu_hudinput_fill(h);
    return 1;
}

/* THE FORKS, from the replay's timelines cache (inc/tagpu_replay.h, THE TIMELINES): the file's
   live forks by branch point, oldest first, as the mockup lists them (index.html `forkList`), each
   with the seat it was played as and whether it is the watched one or on the chain that plays; the
   original's end; and where the watched chain leaves the original, which the track fills to (the
   chain's segment whose parent is the original). The recorder keeps at most REC_FORK_LIMIT live
   forks (recr_append_point refuses a fifth), so only a foreign file holds more, and the packet
   carries the oldest. */
static char s_cutFile[MAX_PATH];        /* the file whose excess forks were said, once */

static void fill_forks(TAGPU_PK_HUD* h, unsigned tl)
{
    TAGPU_REPLAY_TIMELINE row[RECR_TIMELINES];
    int order[RECR_TIMELINES], n = tagpu_replay_timelines(row, RECR_TIMELINES), live = 0, i, j;
    const char* path = tagpu_replay_path();

    if (n > RECR_TIMELINES) n = RECR_TIMELINES;
    h->orig_last = h->last;
    h->fill_to = 0;
    for (i = 0; i < n; i++) {
        const TAGPU_REPLAY_TIMELINE* r = &row[i];
        if (r->id == 0) h->orig_last = r->last;
        if (r->id && r->on_chain && r->parent == 0) h->fill_to = r->fork_at;
        if (!r->id || !r->live) continue;
        for (j = live; j > 0 && (row[order[j - 1]].fork_at > r->fork_at ||
                                 (row[order[j - 1]].fork_at == r->fork_at && row[order[j - 1]].id > r->id)); j--)
            order[j] = order[j - 1];
        order[j] = i;
        live++;
    }
    if (live > (int)TAGPU_PK_HUD_FORKS && path && strcmp(path, s_cutFile)) {
        lstrcpynA(s_cutFile, path, sizeof s_cutFile);
        tagpu_logf("hud: %s holds %d live forks; the HUD shows the %u oldest", path, live, TAGPU_PK_HUD_FORKS);
    }
    for (i = 0; i < live && i < (int)TAGPU_PK_HUD_FORKS; i++) {
        const TAGPU_REPLAY_TIMELINE* r = &row[order[i]];
        TAGPU_PK_HUDFORK* f = &h->fork[i];
        f->id = r->id;
        f->from = r->fork_at;
        f->end = r->last;
        f->parent = r->parent;
        f->seat = (uint8_t)(r->controlled < 0xFF ? r->controlled : 0xFF);
        f->flags = (uint8_t)((r->id == tl ? TAGPU_PK_FORK_WATCHED : 0) | (r->on_chain ? TAGPU_PK_FORK_CHAIN : 0));
    }
    h->nfork = (uint8_t)i;
    h->fork_max = REC_FORK_LIMIT;
    if (tl == 0 || !h->fill_to) h->fill_to = h->orig_last;
}

int tagpu_hudstate_fill(TAGPU_PK_HUD* h, unsigned now)
{
    TAGPU_REPLAY_PLAYER pl[TAGPU_PK_HUD_SEATS];
    unsigned T[TAGPU_PK_HUD_MARKS], first, last, a0, a1, tl;
    unsigned char kind[TAGPU_PK_HUD_MARKS], seat[TAGPU_PK_HUD_MARKS];
    int n, i, mode = tagpu_hudstate_mode();

    if (mode == HUD_MODE_LIVE) return fill_live(h, now);
    if (mode != HUD_MODE_REPLAY || !tagpu_replay_span(&first, &last)) return 0;
    memset(h, 0, sizeof *h);
    h->mode = TAGPU_PK_HUD_REPLAY;
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
    tl = tagpu_replay_timeline();
    fill_forks(h, tl);
    tagpu_hudinput_fill(h);
    /* THE AXIS takes in the forks only while their lanes are open or one is watched, as the
       mockup's `axis()` does; closed, the track is the original's. After the input's fill, which
       keeps whether the lanes are open. */
    if (!tagpu_replay_axis(&a0, &a1) || a1 < h->last) a1 = h->last;
    h->axis_last = h->lanes || tl ? a1 : h->orig_last > h->last ? h->orig_last : h->last;
    return 1;
}

int tagpu_hudstate_fill_stats(TAGPU_PK_HUDSTAT* s, const TAGPU_PK_HUD* h)
{
    unsigned char seat[TAGPU_PK_HUD_SEATS];
    char key[TAGPU_PK_HUD_MEASURE + 1];
    int i, n = h->nseat < TAGPU_PK_HUD_SEATS ? h->nseat : (int)TAGPU_PK_HUD_SEATS;
    if (!h->stat_open) return 0;
    memcpy(key, h->stat_measure, TAGPU_PK_HUD_MEASURE);
    key[TAGPU_PK_HUD_MEASURE] = 0;
    for (i = 0; i < n; i++) seat[i] = h->seat[i].seat;
    s->npt = (uint16_t)tagpu_hudmarks_stats(key, seat, n, &s->v[0][0], (int)TAGPU_PK_HUD_POINTS, &s->period, &s->base);
    return 1;
}

/* ---- the render thread -------------------------------------------------------------------- */

static HUDSTATE s_st;
/* every marker the packet carries -- the walker's deaths and defeats, then the bookmarks -- fits */
typedef char hud_marks_fit[(TAGPU_PK_HUD_MARKS <= HUD_MAXMARK) ? 1 : -1];
typedef char hud_stats_fit[(TAGPU_PK_HUD_POINTS == HUD_MAXPT && TAGPU_PK_HUD_MEASURE == HUD_MEASURE
                            && TAGPU_PK_HUD_SEATS <= HUD_MAXSEAT) ? 1 : -1];
/* every control a loaded layout can make fits the hit map (tagpu_hudlay_max_hot), and every live
   fork a file keeps fits the packet and the layout */
_Static_assert(TAGPU_CMD_HUDHITS == HUD_MAXHOT, "the hit map holds the layout's bound");
_Static_assert(TAGPU_PK_HUD_FORKS == REC_FORK_LIMIT && TAGPU_PK_HUD_FORKS == HUD_MAXFORK,
               "the packet and the layout carry every live fork");
_Static_assert(TAGPU_PK_FORK_CHAIN == 2u, "tagpu_hudlay.c's LANE_CHAIN is the packet's flag");
static int      s_have;
static unsigned s_frame;                /* the render-thread frame `s_st` is for */
static float    s_px;
static int      s_ox, s_oy;
static uint8_t  s_seatOf[TAGPU_PK_HUD_SEATS];   /* row i's recorded seat, this frame's   */
static unsigned s_tFirst, s_tLast;      /* the GameTime span the track is drawn with, this frame's */
static unsigned s_pick, s_pickR, s_pickEvict;   /* the picker's state as the input keeps it, for
                                                   the dump                                  */
/* the live rewind as this frame's record carries it, for the dump */
static struct { unsigned state, arm, R, evict, G, first; } s_rw;
/* THE HIT MAP OF THE LAST FRAME DRAWN: written by the Vulkan pass once it has built a frame it will
   draw (tagpu_hudstate_drawn), read by the dump and then by the next command record
   (tagpu_hudstate_cmd), which empties it, all on this thread. So each drawn frame's map is posted
   once, and a frame that draws no HUD -- whether the pass ran and drew none or tagpu_vk_frame
   returned before it ran -- posts none. `pass` tells the two apart: the pass's prepare sets it
   (tagpu_hudstate_drawn, NULL first, on every frame it runs) and the record clears it, so a
   record posted with no prepare since the last one carries 0. */
static TAGPU_CMD_HUD s_map;
static int      s_mapCut;               /* a frame had more controls than the map: said once */

/* THE NAMES IN WHAT THE FACES CARRY. A recording's names are the engine's 8-bit text, read as
   Latin-1, and the faces are cut to ASCII and Latin-1 (tools/hudart/fonts.py): a control code
   -- below 0x20, or 0x80..0x9F, which Latin-1 leaves to them -- is '?', and the soft hyphen,
   which Oswald lacks, is '-'. Nothing reaches the rasteriser that it would draw as a missing glyph. */
void tagpu_hudstate_utf8(char* dst, size_t cap, const char* src, size_t n)
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
    static const char* const RWS[5] = { "idle", "flush", "open", "past", "ending" };
    FILE* f = fopen(DUMP_OUT ".tmp", "wb");
    int i, j;
    if (!f) { tagpu_logf("hud: the state dump could not open %s", DUMP_OUT ".tmp"); return; }
    fprintf(f, "{\"device\":{\"W\":%d,\"H\":%d},\"px\":%.9g,\"q8\":%d,\"state\":{", vw, vh, u, q8);
    /* the live HUD under the mockup's own field (`live`), and the rewind's state for the tests */
    fprintf(f, "\"mode\":\"%s\",", st->live ? "live" : "replay");
    if (st->live) {
        fprintf(f, "\"live\":{\"open\":%s},\"rw\":{\"state\":\"%s\",\"arm\":%u,\"R\":%u,\"evict\":",
                st->live_open ? "true" : "false", RWS[s_rw.state < 5 ? s_rw.state : 0], s_rw.arm, s_rw.R);
        if (s_rw.evict == UINT32_MAX) fprintf(f, "null");
        else fprintf(f, "%u", s_rw.evict);
        fprintf(f, ",\"G\":%u,\"first\":%u},", s_rw.G, s_rw.first);
    }
    fprintf(f, "\"W\":%.9g,\"H\":%.9g,\"frame\":{\"panel\":%.9g,\"top\":%.9g,\"bottom\":%.9g},",
            st->W, st->H, st->panel, st->top, st->bottom);
    /* `hud` is the mockup's field: the replay HUD's own scale (its scale button, 0.75 to 1.5),
       which the live HUD keeps too; the game's HUD scale is the engine frame's, `q8` above */
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
    /* proposed, variation B, under the mockup's names */
    fprintf(f, ",\"ends\":%.9g,\"len\":%.9g,\"fillTo\":%.9g,\"timeline\":%d,\"lanesOpen\":%s,\"picker\":%s,"
               "\"armed\":%d,\"replace\":%d,\"forks\":[", st->ends, st->len, st->fill_to, st->timeline,
            st->lanes_open ? "true" : "false", st->picker ? "true" : "false", st->armed, st->replace);
    for (i = 0; i < st->nfork; i++) {
        fprintf(f, "%s{\"id\":%d,\"name\":", i ? "," : "", st->fork[i].id);
        jstr(f, st->fork[i].name);
        fprintf(f, ",\"from\":%.9g,\"end\":%.9g,\"parent\":%d,\"flags\":%d}", st->fork[i].from, st->fork[i].end,
                st->fork[i].parent, st->fork[i].flags);
    }
    /* the picker for the tests: 0 closed, 1 asked or seeking, 2 open; R and the eviction as
       GameTimes and a timeline id */
    fprintf(f, "],\"pick\":{\"state\":%u,\"R\":%u,\"evict\":", s_pick, s_pickR);
    if (s_pickEvict == UINT32_MAX) fprintf(f, "null}");
    else fprintf(f, "%u}", s_pickEvict);
    /* the stats window whole, as the mockup's state carries it */
    fprintf(f, ",\"stats\":{\"open\":%s,\"x\":%.9g,\"y\":%.9g,\"measure\":", st->stats.open ? "true" : "false",
            st->stats.x, st->stats.y);
    jstr(f, st->stats.measure);
    fprintf(f, ",\"split\":\"%s\",\"dt\":%.9g,\"t0\":%.9g,\"groups\":[", st->stats.team ? "team" : "player",
            st->stats.dt, st->stats.t0);
    for (i = 0; i < st->stats.ngroup; i++) {
        const HUDGROUP* g = &st->stats.group[i];
        fprintf(f, "%s{\"name\":", i ? "," : "");
        jstr(f, g->name);
        fprintf(f, ",\"col\":\"#%06x\",\"death\":", (unsigned)g->colour);
        if (g->death < 0) fprintf(f, "null");
        else fprintf(f, "%.9g", g->death);
        fprintf(f, ",\"v\":[");
        for (j = 0; j < st->stats.npt; j++) fprintf(f, "%s%.9g", j ? "," : "", g->v[j]);
        fprintf(f, "]}");
    }
    fprintf(f, "]}},\"hits\":{\"track\":[%d,%d],\"first\":%u,\"last\":%u,\"at\":[",
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

/* a sample is data from the recording: one that is not a finite number is drawn as 0 */
static float sample(float v)
{
    return v == v && v < 1e30f && v > -1e30f ? v : 0.0f;
}

/* THE STATS WINDOW: its state as the input keeps it, and the groups the mockup's host makes of the
   samples (index.html `state()`): a player each, with the cross of its first commander death, or,
   when the split asks for teams and the teams are told apart (tagpu_hudlay_headed, the test the
   teams chip is enabled by), a team each, summed, in the chart's team colours. */
static void stats(const struct TAGPU_PACKET* pk, const TAGPU_PK_HUD* h, HUDSTATE* st)
{
    const TAGPU_PK_HUDSTAT* hs = tagpu_pk_hudstat(pk);
    const HUDART* art = tagpu_hudart_get();
    const HUDGLOBALS* gl = art ? tagpu_hudlay_globals(art->lay) : NULL;
    HUDSTATS* S = &st->stats;
    int npt = hs ? (hs->npt < HUD_MAXPT ? hs->npt : HUD_MAXPT) : 0, i, j, k;

    S->open = h->stat_open;
    S->x = h->stat_x;
    S->y = h->stat_y;
    memcpy(S->measure, h->stat_measure, sizeof S->measure);
    S->measure[sizeof S->measure - 1] = 0;
    S->team = h->stat_team && tagpu_hudlay_headed(st);
    S->dt = hs && hs->period ? (float)hs->period / TICKS : 1.0f;
    S->t0 = hs && hs->base > h->first ? (float)(hs->base - h->first) / TICKS : 0.0f;
    S->npt = npt;
    if (!S->team) {
        S->ngroup = st->nseat;
        for (i = 0; i < st->nseat; i++) {
            HUDGROUP* g = &S->group[i];
            memcpy(g->name, st->seat[i].name, sizeof g->name);
            g->colour = st->seat[i].colour;
            g->death = -1.0f;
            for (j = 0; j < st->nmark; j++)
                if (st->mark[j].kind == HUD_MK_DEATH && st->mark[j].seat == i && (g->death < 0 || st->mark[j].t < g->death))
                    g->death = st->mark[j].t;
            for (k = 0; k < npt; k++) g->v[k] = sample(hs->v[i][k]);
        }
        return;
    }
    S->ngroup = st->nteam;
    for (i = 0; i < st->nteam; i++) {
        HUDGROUP* g = &S->group[i];
        snprintf(g->name, sizeof g->name, "Team %d", i + 1);
        g->colour = gl ? gl->team_rgb[i % gl->nteam_rgb] : 0x808080;
        g->death = -1.0f;
        for (k = 0; k < npt; k++) {
            float v = 0;
            for (j = 0; j < st->team_n[i]; j++) v += sample(hs->v[st->team_seat[i][j] % TAGPU_PK_HUD_SEATS][k]);
            g->v[k] = v;
        }
    }
}

/* a GameTime in seconds from the track's start, held to [first, axis] */
static float on_axis(unsigned T, unsigned first, unsigned axis)
{
    return (float)((T < first ? first : T > axis ? axis : T) - first) / TICKS;
}

/* THE FORKS AND THE PICKER, as the fill and the input keep them: each fork named by the seat it was
   played as, as the packet's seats carry it, and the watched one, the one a takeover would
   replace, the one whose Delete is armed and the picker's start by their places in the list */
static void forks(const TAGPU_PK_HUD* h, HUDSTATE* st, unsigned axis)
{
    int i, j;
    st->nfork = h->nfork < HUD_MAXFORK ? h->nfork : HUD_MAXFORK;
    st->fork_max = h->fork_max;
    st->timeline = st->replace = st->armed = -1;
    for (i = 0; i < st->nfork; i++) {
        const TAGPU_PK_HUDFORK* k = &h->fork[i];
        HUDFORK* f = &st->fork[i];
        f->id = (int)k->id;
        f->parent = (int)k->parent;
        f->flags = k->flags;
        f->from = on_axis(k->from, h->first, axis);
        f->end = on_axis(k->end, h->first, axis);
        for (j = 0; j < h->nseat && j < (int)TAGPU_PK_HUD_SEATS && h->seat[j].seat != k->seat; j++) {}
        if (j < h->nseat && j < (int)TAGPU_PK_HUD_SEATS)
            tagpu_hudstate_utf8(f->name, sizeof f->name, h->seat[j].name, sizeof h->seat[j].name);
        else if (k->seat < TAGPU_PK_HUD_SEATS)
            snprintf(f->name, sizeof f->name, "Player %u", (unsigned)k->seat + 1);
        else    /* no recorded seat, 0xFF among them: no name is known */
            snprintf(f->name, sizeof f->name, "Unknown seat");
        if (k->flags & TAGPU_PK_FORK_WATCHED) st->timeline = i;
        /* the input marks the armed Delete on its fork, so the arm follows it when the list reorders */
        if (k->flags & TAGPU_PK_FORK_ARMED) st->armed = i;
    }
    st->picker = h->pick == 2;
    st->pick_down = h->pick != 0;
    st->lanes_open = h->lanes != 0;
    if (st->picker)
        for (i = 0; i < st->nfork; i++)
            if (h->pick_evict != UINT32_MAX && st->fork[i].id == (int)h->pick_evict) st->replace = i;
}

/* THE LIVE HUD (hud.js `live`): its button down while a rewind owns the level, its strip while
   the rewind is open or in the past, and the strip's track from the rewind's first GameTime to
   now, where it opened -- `t` the playhead and `T` the strip's end, in seconds from `first`. Over
   what the replay's half made of the same record, which carries no seats, markers or stats. */
static void present_live(const TAGPU_PK_HUD* h, HUDSTATE* st)
{
    unsigned G = h->rw_G > h->first ? h->rw_G : h->first;
    st->live = 1;
    st->live_open = h->rw_state == TAGPU_REPLAY_RWS_OPEN || h->rw_state == TAGPU_REPLAY_RWS_PAST;
    st->rw_down = h->rw_state != TAGPU_REPLAY_RWS_IDLE;
    st->rw_arm = h->rw_arm;
    st->T = (float)(G - h->first) / TICKS;
    st->t = (float)((h->now < h->first ? h->first : h->now > G ? G : h->now) - h->first) / TICKS;
    st->playing = 0;
    s_tFirst = h->first;
    s_tLast = G;
    s_rw.state = h->rw_state;
    s_rw.arm = h->rw_arm;
    s_rw.R = h->rw_R;
    s_rw.evict = h->rw_evict;
    s_rw.G = h->rw_G;
    s_rw.first = h->first;
}

void tagpu_hudstate_present(const TAGPU_FRAME* f)
{
    const struct TAGPU_PACKET* pk = f->packet;
    const TAGPU_PK_HUD* h;
    const unsigned char* pal = tagpu_pal_live();
    HUDSTATE* st = &s_st;
    int vw = f->vp_w, vh = f->vp_h, pw = 128, bh = 32, q8 = 256, i, k;
    unsigned end, axis;
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

    /* THE TRACK'S AXIS is the fill's (axis_last): the original's span, or the longest live chain's
       while the lanes are open or a fork is watched. The playhead stops at the end of the chain
       that plays; GameTime runs on past it. Every time below is held to the axis, so a value the
       fill got wrong draws in the wrong place on the track, never off it. */
    end = h->last > h->first ? h->last : h->first;
    axis = h->axis_last > end ? h->axis_last : end;
    st->T = (float)(axis - h->first) / TICKS;
    st->len = (float)(end - h->first) / TICKS;
    st->ends = on_axis(h->orig_last, h->first, axis);
    st->fill_to = on_axis(h->fill_to, h->first, axis);
    st->t = (float)((h->now < h->first ? h->first : h->now > end ? end : h->now) - h->first) / TICKS;
    /* at the recording's end the button shows play, as the mockup's does: the playhead has
       stopped, and a click there starts the recording over (tagpu_hudinput.c) */
    st->playing = !h->paused && h->now < end;
    st->speed = (float)h->speed / 10.0f;
    st->hud = (float)h->scale / 100.0f;

    for (i = 0; i < h->nseat && i < HUD_MAXSEAT; i++) {
        const TAGPU_PK_HUDSEAT* s = &h->seat[i];
        HUDSEAT* q = &st->seat[i];
        tagpu_hudstate_utf8(q->name, sizeof q->name, s->name, sizeof s->name);
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

    stats(pk, h, st);

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
    forks(h, st, axis);
    s_tFirst = h->first;
    s_tLast = axis;
    s_pick = h->pick;
    s_pickR = h->pick_R;
    s_pickEvict = h->pick_evict;
    if (h->mode == TAGPU_PK_HUD_LIVE) present_live(h, st);
    s_px = u;
    s_frame = f->frame_counter;
    s_have = 1;
    if ((f->frame_counter % 30u) == 0u && GetFileAttributesA(DUMP_ASK) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA(DUMP_ASK);
        dump(st, vw, vh, u, q8);
    }
}

/* a hit edge: the device pixel the draw puts the same edge on (tagpu_hud_snap, the draw's own
   float products), from the game screen's corner at `o`; the track's ends are the playhead's
   centre run, not a drawn edge, and agree with the draw to within a pixel, which seek_at's
   interpolation absorbs */
static int16_t dev(int o, double snapped)
{
    double r = o + snapped;
    return (int16_t)(r < -32768.0 ? -32768 : r > 32767.0 ? 32767 : r);
}

void tagpu_hudstate_drawn(const HUDOPS* ops)
{
    int i, top, cut = 0;
    s_map.n = 0;
    s_map.pass = 1;
    if (!ops || !s_have) return;
    /* topmost first: the top layer's controls (HOPF_TOP), which are drawn after everything else,
       then the rest -- the build paints in order, so the last control painted is on top */
    for (top = 1; top >= 0; top--)
        for (i = ops->n - 1; i >= 0; i--) {
            const HUDOP* p = &ops->op[i];
            TAGPU_CMD_HUDHIT* t;
            if (p->kind != HOP_HOT || ((p->flags & HOPF_TOP) != 0) != top) continue;
            if (s_map.n >= TAGPU_CMD_HUDHITS) { cut++; continue; }
            t = &s_map.hit[s_map.n++];
            t->x0 = dev(s_ox, tagpu_hud_snap(p->r.x * s_px));
            t->y0 = dev(s_oy, tagpu_hud_snap(p->r.y * s_px));
            t->x1 = dev(s_ox, tagpu_hud_edge(p->r.x, p->r.w, s_px));
            t->y1 = dev(s_oy, tagpu_hud_edge(p->r.y, p->r.h, s_px));
            lstrcpynA(t->key, ops->text + p->run, sizeof t->key);
        }
    if (cut && !s_mapCut) {
        s_mapCut = 1;
        tagpu_logf("hud: %d controls past the hit map's %u are not clickable (said once)", cut,
                   TAGPU_CMD_HUDHITS);
    }
    s_map.u = s_px;
    s_map.t_first = s_tFirst;
    s_map.t_last = s_tLast;
    s_map.W = s_st.W;
    s_map.H = s_st.H;
    s_map.track_x0 = dev(s_ox, tagpu_hud_snap(ops->track_x0 * s_px));
    s_map.track_x1 = dev(s_ox, tagpu_hud_snap(ops->track_x1 * s_px));
    s_map.first = (uint8_t)(ops->first < 0 ? 0 : ops->first > 255 ? 255 : ops->first);
    s_map.last = (uint8_t)(ops->last < 0 ? 0 : ops->last > 255 ? 255 : ops->last);
    s_map.nseat = (uint8_t)(s_st.nseat < (int)TAGPU_PK_HUD_SEATS ? s_st.nseat : (int)TAGPU_PK_HUD_SEATS);
    memcpy(s_map.seat, s_seatOf, sizeof s_map.seat);
    /* the lanes this frame drew, in the order their keys index them (tagpu_hudlay.c, lanes) */
    s_map.nlane = 0;
    for (i = 0; s_st.lanes_open && i < s_st.nfork && i < (int)TAGPU_PK_HUD_FORKS; i++)
        s_map.lane_id[s_map.nlane++] = (uint32_t)s_st.fork[i].id;
}

void tagpu_hudstate_cmd(TAGPU_CMD_HUD* out)
{
    *out = s_map;
    s_map.n = 0;
    s_map.pass = 0;
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
