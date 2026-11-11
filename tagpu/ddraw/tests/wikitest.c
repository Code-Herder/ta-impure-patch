/* wikitest.c -- the C half of the wiki's parity check, for tools/wiki/simparity.mjs: the real
   tagpu_wikidata.c and tagpu_wikisim.c over the core's tasim_*.c, compiled natively (or for
   Windows, run under Wine), write what they see as JSON lines, and simparity.mjs compares them
   with sim.js's, field by field.

   `wikitest demos PAK [LAYOUT [CASTING IFACE [COB]]]` loads PAK, an archive of art/wiki/
   (tools/hudpak.py --set wiki), as the DLL loads WIKIPAK, plays every demo of every entry to its
   end with the sidebar layout LAYOUT (the archive's side/LAYOUT.json; none when absent or "-"),
   in the context of side 0, the casting the file CASTING holds (one side's record; none when
   absent or "-"), Interface Type IFACE (1 when absent) and the units' scripts the file COB
   names (none when absent or "-"; see cob_load), and prints one line per tick from 0:
   {"entry", "demo", "t", "h": tagpu_wikisim_hash as 16 hex digits, "v": the view, "o": every
   unit's orders by id}. The view carries the map on the first and the last tick only: nothing
   writes it after the scene is laid out. A demo the sim refuses in that context is one line,
   {"entry", "demo", "refused": why}.

   `wikitest game PAK ENTRY LAYOUT CASTING RIVAL IFACE COB SCRIPTED` plays every demo of the game
   entry the file ENTRY holds as a game (SCRIPTED 2, WIKI_PLAY_GAME, or 3, its stall mode) with the
   arguments demos takes ("-" for none) and the opponent's casting the file RIVAL holds, and prints
   as demos does; each view carries the game's part, "game", as sim.js's view does.

   `wikitest sweep PAK ENTRY LAYOUT CASTING RIVAL IFACE COB` plays every demo's game of the game
   entry the file ENTRY holds once for each tick r from 0 to its script's last input, with Back to
   now's release (tagpu_wikisim_release) between r and r + 1, in the context game's arguments give,
   and prints one line per r: {"entry", "demo", "r", "t", "stopped", "h"} (see sweep).

   `wikitest replay PAK ENTRY LAYOUT CASTING RIVAL IFACE COB` plays every demo of the game entry the
   file ENTRY holds through the replay layer (tagpu_wikireplay, its viewer's script played) in the
   context game's arguments give, and prints one line per demo tick from 0: {"entry", "demo", "t",
   "h": tagpu_wikireplay_hash, "r": the replay's view in replay.js's shape, "gh": the game's
   tagpu_wikisim_hash, "v": the game's view (its map on the first and the last tick), "o": its units'
   orders}; a demo the replay refuses is one line, {"entry", "demo", "refused": why}.

   `wikitest replaytrace PAK ENTRY LAYOUT CASTING RIVAL IFACE COB TRACE` plays Try it yourself on the
   entry's first demo (tagpu_wikireplay, create) with the presses the file TRACE holds,
   {"end": N, "presses": [[tick, control's name, value], ...]}: each tick from 0 its frame is printed
   as replay prints it, then the presses of that tick are made in order, then it steps, up to tick N
   or the replay's end. In a live entry's trace a press may be a world input instead, given through
   tagpu_wikireplay_input: [tick, "move", x, z], [tick, "down", key] or [tick, "up", key], a key by
   sim.js's name.

   `wikitest serve` is the simulation host (tests/wikihost.c states its protocol): JSON requests on
   stdin, one answer line each on stdout. `wikitest trace FILE` is the same loop reading FILE, so a
   recorded session replays through any build; simparity.mjs records the rule cases so.

   `wikitest refuse FILE` reads cases two lines each, {"name", "scripted": 0..3 (WIKI_PLAY_*),
   "layout": the layout or null, "side", "iface", "casting": a side's casting record or null,
   "rival": the opponent's or null, "replay": true for a replay} and then the
   entry, and makes a sim of the entry's first demo with that layout in that context: through the
   casting's reader, the entry's loader, the layout's and tagpu_wikisim_init, or with `replay`
   tagpu_wikireplay_init playing the viewer's script. It prints
   "accepted <name>" or "refused <name>: why" per case; simparity.mjs asks text.js's checkEntry
   and sim.js the same.

   `wikitest data FILE` reads items two lines each, {"kind": "casting"|"layout", "name"} and then
   the JSON, reads each with tagpu_wikidata_casting or tagpu_wikidata_layout_json, and prints
   what it read as one JSON line: {"ok": true, ...the struct in sim.js's names} or {"ok": false,
   "why"}. simparity.mjs computes the same from sim.js's checkCasting and checkLayout.

   `wikitest constants` prints the build trip's constants and the replay layer's as one JSON line, in
   sim.js's and replay.js's names.

   `wikitest rules FILE` plays the recorder's rules, tagpu_recrules.h's own functions, on the cases
   FILE holds, one a line: `refresh N TICK PREV INTERVAL`, reclossy_is_refresh's answer, 0 or 1;
   `walk INTERVAL T1 T2 ...`, the refreshes of a segment whose iterations are at GameTimes T1, T2,
   ... in that order, as a JSON array; `evict SELECTED LIMIT ID:LIVE:USED ...`, rec_fork_evict on the
   rows in file order, {"ok": 0|1, "evict": the id or null}. It prints the constants first,
   {"REFRESH_TICKS", "FORK_LIMIT", "TIMELINES"}, then one line per case; simparity.mjs asks
   replay.js's twins the same.

   `wikitest sightcost PAK N` times tagpu_wikisim_sight_lit and tagpu_wikisim_contacts on the costs
   spec's worst scene, N fills each: a 96 x 48 map (a 48 x 24 grid), 128 units of one seat of a type
   of sight 290 (the archive's fixture table, TABLE8's 212 offsets) spread over the map, each one
   also a 400-radar, activated, finished commander, 128 units of another seat, and 20 deaths of
   the first seat within the death vision; prints {"fill_us", "contacts_us", "lit"}, the mean
   microseconds of one call and the blocks the fill lit. The contacts are timed over an unlit grid,
   so every target takes the distance tests to all 128 radars, the pass's worst. Build it optimised for a figure that means
   anything.

   `wikitest malformed` loads data the loader must survive -- each case an archive or an entry
   built here -- and prints one line per case, "ok <case>" or "FAILED <case>: why". The native
   build runs under AddressSanitizer, so a write past an allocation fails the run as well.

   Exit 0 when everything loaded and ran; the comparison is simparity.mjs's. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "tagpu_json.h"
#include "tagpu_wikidata.h"
#include "tagpu_wikisim.h"
#include "tagpu_wikireplay.h"
#include "tagpu_recrules.h"
#include "wikihost.h"
#include "wikicobfile.h"

#ifdef _WIN32
#include <windows.h>
/* tagpu_pak.c's resource lookup links against the DLL's module handle; this test reads its archive
   from a file instead and never calls it */
HMODULE g_ddraw_module;
#include <fcntl.h>
#include <io.h>
#endif

/* the units' scripts and sounds the file modes' COB argument names (wikicobfile.h) */
static COBFILES g_cob;

/* one tick's line; with `sight` (a players game's, `t` its table or NULL) also its sight_out */
static void frame_sight(WIKISIM* s, const char* entry, const char* demo, int map, int sight, const WIKISIGHT* t)
{
    const WIKIVIEW* v;
    int i;
    putchar('{');
    wh_key("entry", 1); wh_str(entry);
    wh_key("demo", 0); wh_str(demo);
    wh_key("t", 0); printf("%d", s->tick);
    wh_key("h", 0); wh_hash(s);
    wh_key("v", 0); wh_view_as(s, map, 0);
    wh_key("o", 0);
    putchar('{');
    v = tagpu_wikisim_view(s);
    for (i = 0; i < v->nunit; i++) {
        wh_key(v->unit[i].id, i == 0);
        wh_orders(s, v->unit[i].id);
    }
    putchar('}');
    if (sight) {
        wh_key("s", 0);
        wh_sight_out(s, t);
    }
    fputs("}\n", stdout);
}

static void frame(WIKISIM* s, const char* entry, const char* demo, int map) { frame_sight(s, entry, demo, map, 0, NULL); }

/* the casting a file holds, one side's record, into `k`; 1, else 0 with a message on stderr */
static int casting_file(const char* path, WIKICASTING* k)
{
    char err[256];
    size_t n;
    char* text = slurp(path, &n);
    TJDOC* doc = text ? tagpu_json_parse(text, n, err, sizeof err) : NULL;
    int ok = doc && tagpu_wikidata_casting(tagpu_json_root(doc), k, err, sizeof err);
    if (!ok) fprintf(stderr, "wikitest: %s: %s\n", path, text ? err : "cannot read it");
    tagpu_json_free(doc);
    free(text);
    return ok;
}

static int demos(const char* path, const char* layout, const char* casting, int iface, int cob)
{
    char err[256], file[96];
    size_t n;
    char* pak;
    WIKIDATA* d;
    WIKILAYOUT* lay = NULL;
    WIKICASTING k;
    WIKISIMCTX ctx;
    int i, j, rc = 0;
    ctx.side = 0;
    ctx.cast = casting && strcmp(casting, "-") ? &k : NULL;
    ctx.iface = iface;
    ctx.cob = cob ? &g_cob.cob : NULL;
    ctx.snd = cob ? &g_cob.snd : NULL;
    ctx.rival = NULL;
    if (ctx.cast && !casting_file(casting, &k)) return 2;
    if (!(pak = slurp(path, &n))) { fprintf(stderr, "wikitest: cannot read %s\n", path); return 2; }
    if (!(d = tagpu_wikidata_load((const unsigned char*)pak, n, err, sizeof err))) {
        fprintf(stderr, "wikitest: %s\n", err);
        free(pak);
        return 2;
    }
    snprintf(file, sizeof file, "side/%s.json", layout ? layout : "-");
    if (layout && strcmp(layout, "-") && !(lay = tagpu_wikidata_layout_pak((const unsigned char*)pak, n, file, err, sizeof err))) {
        fprintf(stderr, "wikitest: %s\n", err);
        tagpu_wikidata_free(d);
        free(pak);
        return 2;
    }
    for (i = 0; i < d->nentry; i++)
        for (j = 0; j < d->entry[i].ndemo; j++) {
            const WIKIENTRY* e = &d->entry[i];
            WIKISIM s;
            if (!tagpu_wikisim_init(&s, e, &e->demo[j], 1, lay, &ctx, err, sizeof err)) {
                putchar('{');
                wh_key("entry", 1); wh_str(e->id);
                wh_key("demo", 0); wh_str(e->demo[j].id);
                wh_key("refused", 0); wh_str(err);
                fputs("}\n", stdout);
                continue;
            }
            frame(&s, e->id, e->demo[j].id, 1);
            while (!s.done) {
                tagpu_wikisim_step(&s);
                frame(&s, e->id, e->demo[j].id, s.done);
            }
            if (s.oom) {
                fprintf(stderr, "wikitest: %s/%s ran out of memory\n", e->id, e->demo[j].id);
                rc = 2;
            }
            tagpu_wikisim_free(&s);
            if (rc) goto done;
        }
done:
    tagpu_wikidata_free(d);
    free(lay);
    free(pak);
    return rc;
}

/* A GAME ENTRY'S RUN: the file ENTRY's entry, the archive PAK, its layout LAYOUT, the castings in
   the files CASTING and RIVAL ("-" for none) and the context they make with IFACE and the scripts,
   demos' way; the castings live in it, so the sims made in its context must not outlive it */
typedef struct {
    char *pak, *text;
    size_t npak;
    WIKILAYOUT* lay;
    WIKICASTING k, rk;
    WIKISIMCTX ctx;
    WIKIENTRY e;
    int loaded;
} GAMERUN;

static void run_close(GAMERUN* g)
{
    if (g->loaded) tagpu_wikidata_entry_free(&g->e);
    free(g->text);
    free(g->lay);
    free(g->pak);
}

/* 1, else 0 with a message on stderr and nothing to close */
static int run_open(GAMERUN* g, const char* path, const char* entry, const char* layout, const char* casting, const char* rival,
                    int iface, int cob)
{
    char err[256], file[96];
    size_t ne;
    memset(g, 0, sizeof *g);
    g->ctx.side = 0;
    g->ctx.cast = casting && strcmp(casting, "-") ? &g->k : NULL;
    g->ctx.rival = rival && strcmp(rival, "-") ? &g->rk : NULL;
    g->ctx.iface = iface;
    g->ctx.cob = cob ? &g_cob.cob : NULL;
    g->ctx.snd = cob ? &g_cob.snd : NULL;
    if ((g->ctx.cast && !casting_file(casting, &g->k)) || (g->ctx.rival && !casting_file(rival, &g->rk))) return 0;
    if (!(g->text = slurp(entry, &ne))) { fprintf(stderr, "wikitest: cannot read %s\n", entry); return 0; }
    if (!(g->loaded = tagpu_wikidata_entry(&g->e, g->text, ne, err, sizeof err))) {
        fprintf(stderr, "wikitest: %s\n", err);
        run_close(g);
        return 0;
    }
    if (!(g->pak = slurp(path, &g->npak))) { fprintf(stderr, "wikitest: cannot read %s\n", path); run_close(g); return 0; }
    snprintf(file, sizeof file, "side/%s.json", layout ? layout : "-");
    if (layout && strcmp(layout, "-") && !(g->lay = tagpu_wikidata_layout_pak((const unsigned char*)g->pak, g->npak, file, err, sizeof err))) {
        fprintf(stderr, "wikitest: %s\n", err);
        run_close(g);
        return 0;
    }
    return 1;
}

static void refused_line(const char* entry, const char* demo, const char* why)
{
    putchar('{');
    wh_key("entry", 1); wh_str(entry);
    wh_key("demo", 0); wh_str(demo);
    wh_key("refused", 0); wh_str(why);
    fputs("}\n", stdout);
}

/* every demo of the game entry the file ENTRY holds, its game script played as a game (`scripted`
   WIKI_PLAY_GAME or _STALL) to its end in the context the arguments give, demos' way, plus the
   rival casting the file RIVAL holds; one line per tick, as demos prints */
static int game(const char* path, const char* entry, const char* layout, const char* casting, const char* rival, int iface,
                int cob, int scripted)
{
    char err[256];
    GAMERUN g;
    int j, rc = 0;
    if (!run_open(&g, path, entry, layout, casting, rival, iface, cob)) return 2;
    for (j = 0; j < g.e.ndemo; j++) {
        WIKISIM s;
        if (!tagpu_wikisim_init(&s, &g.e, &g.e.demo[j], scripted, g.lay, &g.ctx, err, sizeof err)) {
            refused_line(g.e.id, g.e.demo[j].id, err);
            continue;
        }
        frame_sight(&s, g.e.id, g.e.demo[j].id, 1, g.e.players, g.lay ? &g.lay->sight : NULL);
        while (!s.done) {
            tagpu_wikisim_step(&s);
            frame_sight(&s, g.e.id, g.e.demo[j].id, s.done, g.e.players, g.lay ? &g.lay->sight : NULL);
        }
        if (s.oom) {
            fprintf(stderr, "wikitest: %s/%s ran out of memory\n", g.e.id, g.e.demo[j].id);
            rc = 2;
        }
        tagpu_wikisim_free(&s);
        if (rc) break;
    }
    run_close(&g);
    return rc;
}

/* THE RELEASE SWEEP (simcheck.mjs releaseSweep): every demo of the game entry the file ENTRY holds,
   its game played in refuse mode with tagpu_wikisim_release between tick r and r + 1 for every r
   from 0 to its game script's last input, then on to its end, in the context game's arguments give;
   one line per r: {"entry", "demo", "r", "t": the tick it ended on, "stopped": a lock, a starved
   pass, a kill that found no unit or a stalled wait, "h": tagpu_wikisim_hash as 16 hex digits} */
static int sweep(const char* path, const char* entry, const char* layout, const char* casting, const char* rival, int iface,
                 int cob)
{
    char err[256];
    GAMERUN g;
    int j, k, r, last, rc = 0;
    if (!run_open(&g, path, entry, layout, casting, rival, iface, cob)) return 2;
    for (j = 0; j < g.e.ndemo && !rc; j++) {
        const WIKIDEMO* d = &g.e.demo[j];
        for (last = 0, k = 0; k < d->game.ninput; k++)
            if (d->game.input[k].at > last) last = (int)d->game.input[k].at;
        for (r = 0; r <= last && !rc; r++) {
            WIKISIM s;
            if (!tagpu_wikisim_init(&s, &g.e, d, WIKI_PLAY_GAME, g.lay, &g.ctx, err, sizeof err)) {
                refused_line(g.e.id, d->id, err);
                break;
            }
            while (s.tick < r && !s.done) tagpu_wikisim_step(&s);
            tagpu_wikisim_release(&s, 0);
            while (!s.done) tagpu_wikisim_step(&s);
            putchar('{');
            wh_key("entry", 1); wh_str(g.e.id);
            wh_key("demo", 0); wh_str(d->id);
            wh_key("r", 0); printf("%d", r);
            wh_key("t", 0); printf("%d", s.tick);
            wh_key("stopped", 0); fputs(s.locked != -1 || s.starved || s.bad_kill || s.stalled ? "true" : "false", stdout);
            wh_key("h", 0); wh_hash(&s);
            fputs("}\n", stdout);
            if (s.oom) {
                fprintf(stderr, "wikitest: %s/%s ran out of memory\n", g.e.id, d->id);
                rc = 2;
            }
            tagpu_wikisim_free(&s);
        }
    }
    run_close(&g);
    return rc;
}

/* a WIKI_HUD_* by its name, a live trip's start at the game by replay.js's AT_GAME, or JSON null for
   none */
static void hud_name(int c)
{
    if (c == WIKI_REPLAY_AT_GAME) fputs("\"@game\"", stdout);
    else if (c < 0) fputs("null", stdout);
    else wh_str(tagpu_wikidata_hud_name(c));
}

/* a float as a JSON number: the double replay.js's Math.fround holds, exactly (17 digits round-trip
   a double) */
static void f32(float f)
{
    printf("%.17g", (double)f);
}

/* a live replay's `live` block in replay.js's shape */
static void live_view(const WIKIREPLAYVIEW* v)
{
    static const char* const RW[] = { "idle", "open", "past" }, * const ARM[] = { "none", "seeking", "armed" };
    int i;
    printf(",\"live\":{\"rw\":\"%s\",\"G\":%d,\"first\":%d,\"last\":%d,\"now\":%d,\"arm\":\"%s\",\"armR\":%d,"
           "\"armEvict\":%d,\"dragT\":%d,\"clash\":%s,\"nlogged\":%d,\"selected\":%d,\"human\":%d,\"timelines\":[",
           RW[v->rw], v->G, v->first, v->last, v->now, ARM[v->arm], v->arm_R, v->arm_evict, v->drag_t,
           v->clash ? "true" : "false", v->nlogged, v->selected, v->human);
    for (i = 0; i < v->ntl; i++)
        printf("%s{\"id\":%d,\"parent\":%d,\"R\":%d,\"last\":%d,\"live\":%s,\"used\":%d,\"mode\":\"%s\",\"nlog\":%d}",
               i ? "," : "", v->tl[i].id, v->tl[i].parent, v->tl[i].R, v->tl[i].last, v->tl[i].live ? "true" : "false",
               v->tl[i].used, v->tl[i].mode == WIKI_ECON_STALL ? "stall" : "refuse", v->tl[i].nlog);
    fputs("],\"past\":", stdout);
    if (v->has_past) printf("{\"tick\":%d,\"target\":%d,\"gen\":%d}", v->past_tick, v->past_target, v->past_gen);
    else fputs("null", stdout);
    fputs(",\"bar\":{\"be\":", stdout);
    f32(v->be);
    fputs(",\"bm\":", stdout);
    f32(v->bm);
    fputs(",\"rates\":[", stdout);
    for (i = 0; i < 4; i++) {
        if (i) putchar(',');
        f32(v->rates[i]);
    }
    printf("],\"dt\":%d},\"back\":%d,\"full\":%s}", v->dt, v->back, v->full ? "true" : "false");
}

/* a takeover replay's `tk` block in replay.js's shape */
static void tk_view(const WIKIREPLAYVIEW* v)
{
    static const char* const PICK[] = { "closed", "asked", "seeking", "open" };
    int i;
    printf(",\"tk\":{\"mode\":\"%s\",\"human\":%d,\"pick\":\"%s\",\"pickR\":%d,\"pickEvict\":%d,\"pickParent\":%d,"
           "\"pickTl\":%d,\"pickSerial\":%d,\"lanes\":%d,\"armed\":%d,\"selected\":%d,\"chainLast\":%d,\"fillTo\":%d,"
           "\"serial\":%d,\"saved\":[",
           v->tk_mode == WIKI_TK_LIVE ? "live" : "watch", v->human, PICK[v->pick], v->pick_R, v->pick_evict, v->pick_parent,
           v->pick_tl, v->pick_serial, v->lanes, v->armed, v->selected, v->chain_last, v->fill_to, v->serial);
    for (i = 0; i < v->nsaved; i++) printf("%s%d", i ? "," : "", v->saved[i]);
    printf("],\"bg\":%s,\"walker\":%d,\"tkFull\":%s,\"timelines\":[", v->bg ? "true" : "false", v->walker_tick,
           v->tk_full ? "true" : "false");
    for (i = 0; i < v->ntl; i++)
        printf("%s{\"id\":%d,\"parent\":%d,\"R\":%d,\"last\":%d,\"live\":%s,\"used\":%d,\"mode\":\"%s\",\"seat\":%d}",
               i ? "," : "", v->tl[i].id, v->tl[i].parent, v->tl[i].R, v->tl[i].last, v->tl[i].live ? "true" : "false",
               v->tl[i].used, v->tl[i].mode == WIKI_ECON_STALL ? "stall" : "refuse", v->tl[i].seat);
    fputs("]}", stdout);
}

/* a players replay's `players` block in replay.js's shape */
static void players_view(const WIKIREPLAYVIEW* v)
{
    int i;
    printf(",\"players\":{\"cam\":{\"x\":%d,\"z\":%d},\"follow\":%d,\"jumpReq\":%d,\"sight\":%d,\"sightReq\":%d,\"tog\":%d,"
           "\"folded\":%d,\"stat\":{\"open\":%s,\"measure\":%d,\"team\":%s},\"clickFrom\":%d,\"shown\":%d,\"pend\":%s,"
           "\"bar\":{\"be\":", v->cam_x, v->cam_z, v->follow, v->jump_req, v->sight, v->sight_req, v->tog, v->folded,
           v->stat_open ? "true" : "false", v->stat_measure, v->stat_team ? "true" : "false", v->click_from, v->shown,
           v->pend ? "true" : "false");
    f32(v->pbe);
    fputs(",\"bm\":", stdout);
    f32(v->pbm);
    fputs(",\"rates\":[", stdout);
    for (i = 0; i < 4; i++) {
        if (i) putchar(',');
        f32(v->prates[i]);
    }
    fputs("],\"dt\":[", stdout);
    for (i = 0; i <= WIKI_SEATS; i++) printf("%s%d", i ? "," : "", v->pdt[i]);
    printf("]},\"pass\":{\"at\":%d,\"blocks\":%d,\"fog\":\"", v->pass_at, v->pass_blocks);
    for (i = 0; i < (v->pass_blocks + 7) / 8; i++) printf("%02x", v->pass_fog[i]);
    fputs("\",\"contacts\":[", stdout);
    for (i = 0; i < v->npass_contact; i++) {
        if (i) putchar(',');
        wh_str(v->pass_contact[i]);
    }
    printf("]},\"npt\":%d,\"nseat\":%d,\"base\":%d,\"period\":%d}", v->npt, v->nseat, v->base, v->period);
}

/* the replay's view in replay.js's R.view() shape */
static void replay_view(const WIKIREPLAYVIEW* v)
{
    int i;
    putchar('{');
    wh_key("tick", 1); printf("%d", v->tick);
    wh_key("clock", 0); printf("%d", v->clock);
    wh_key("done", 0); fputs(v->done ? "true" : "false", stdout);
    wh_key("waiting", 0); fputs(v->waiting == WIKI_UNTIL_SEEKED ? "\"seeked\"" : "null", stdout);
    wh_key("stalled", 0); fputs(v->stalled ? "true" : "false", stdout);
    wh_key("caption", 0); if (v->caption) wh_str(v->caption); else fputs("null", stdout);
    wh_key("failed", 0); fputs(v->failed ? "true" : "false", stdout);
    wh_key("ignored", 0); printf("%d", v->ignored);
    wh_key("clickFrom", 0); printf("%d", v->click_from);
    wh_key("game", 0);
    printf("{\"tick\":%d,\"end\":%d,\"playhead\":%d,\"paused\":%s,\"speed\":%d,\"acc\":%d,\"target\":%d,\"gen\":%d,\"quiet\":%d}",
           v->game_tick, v->end, v->playhead, v->paused ? "true" : "false", v->speed, v->acc, v->target, v->gen, v->quiet);
    wh_key("cursor", 0);
    if (v->has_cursor) {
        putchar('{');
        wh_key("from", 1); hud_name(v->from);
        wh_key("fromValue", 0); printf("%d", v->from_value);
        wh_key("fromWatched", 0); printf("%d", v->from_watched);
        wh_key("to", 0); hud_name(v->to);
        wh_key("value", 0); printf("%d", v->value);
        wh_key("watched", 0); printf("%d", v->watched);
        wh_key("at", 0); printf("%d", v->at);
        wh_key("drag", 0);
        if (v->drag >= 0) printf("{\"drag\":%d,\"over\":%d}", v->drag, v->over);
        else fputs("null", stdout);
        putchar('}');
    } else fputs("null", stdout);
    wh_key("held", 0); hud_name(v->held);
    wh_key("marks", 0);
    putchar('[');
    for (i = 0; i < v->nmark; i++) printf("%s[%d,%d,%d]", i ? "," : "", v->mark[i][0], v->mark[i][1], v->mark[i][2]);
    putchar(']');
    wh_key("walked", 0); printf("%d", v->walked);
    wh_key("books", 0);
    putchar('[');
    for (i = 0; i < v->nbook; i++) printf("%s%d", i ? "," : "", v->book[i]);
    putchar(']');
    wh_key("toast", 0);
    if (v->toast_kind >= 0) {
        static const char* const TOASTS[] = { "added", "removed", "playing_on", "cannot", "armed", "playing_from", "refused",
                                              "taking_over", "cannot_take", "saved", "cannot_save", "click_delete", "deleted",
                                              "camera" };
        printf("{\"kind\":\"%s\",\"at\":%d,\"value\":%d", TOASTS[v->toast_kind], v->toast_at, v->toast_value);
        if (v->live || v->tk) printf(",\"evict\":%d", v->toast_evict);
        if (v->toast_seat >= 0) printf(",\"seat\":%d", v->toast_seat);
        putchar('}');
    } else fputs("null", stdout);
    wh_key("question", 0);
    if (v->question) printf("{\"wasPaused\":%s}", v->was_paused ? "true" : "false");
    else fputs("null", stdout);
    wh_key("left", 0); fputs(v->left ? "true" : "false", stdout);
    if (v->live) live_view(v);
    if (v->tk) tk_view(v);
    if (v->players) players_view(v);
    putchar('}');
}

/* one demo tick of a replay: its hash and view, then the game's hash, view and orders; a players
   replay's also the game's sight_out over `t`, its layout's table (NULL none) */
static void replay_frame(WIKIREPLAY* r, const char* entry, const char* demo, int map, const WIKISIGHT* t)
{
    WIKIREPLAYVIEW rv;
    WIKISIM* s = tagpu_wikireplay_game(r);
    const uint64_t h = tagpu_wikireplay_hash(r);
    int i;
    tagpu_wikireplay_view(r, &rv);
    putchar('{');
    wh_key("entry", 1); wh_str(entry);
    wh_key("demo", 0); wh_str(demo);
    wh_key("t", 0); printf("%d", rv.tick);
    wh_key("h", 0); printf("\"%08lx%08lx\"", (unsigned long)(h >> 32), (unsigned long)(h & 0xFFFFFFFFu));
    wh_key("r", 0); replay_view(&rv);
    if (s) {
        const WIKIVIEW* v;
        wh_key("gh", 0); wh_hash(s);
        wh_key("v", 0); wh_view_as(s, map, 0);
        wh_key("o", 0);
        putchar('{');
        v = tagpu_wikisim_view(s);
        for (i = 0; i < v->nunit; i++) {
            wh_key(v->unit[i].id, i == 0);
            wh_orders(s, v->unit[i].id);
        }
        putchar('}');
        if (rv.players) {
            wh_key("s", 0);
            wh_sight_out(s, t);
        }
    } else fputs(",\"gh\":null,\"v\":null,\"o\":null", stdout);
    fputs("}\n", stdout);
}

/* every demo of the game entry through the replay layer, its viewer's script played, one line a demo
   tick */
static int replay(const char* path, const char* entry, const char* layout, const char* casting, const char* rival, int iface,
                  int cob)
{
    char err[256];
    GAMERUN g;
    int j;
    if (!run_open(&g, path, entry, layout, casting, rival, iface, cob)) return 2;
    for (j = 0; j < g.e.ndemo; j++) {
        WIKIREPLAY r;
        if (!tagpu_wikireplay_init(&r, &g.e, &g.e.demo[j], WIKI_PLAY_DEMO, g.lay, &g.ctx, err, sizeof err)) {
            refused_line(g.e.id, g.e.demo[j].id, err);
            continue;
        }
        replay_frame(&r, g.e.id, g.e.demo[j].id, 1, g.lay ? &g.lay->sight : NULL);
        while (!r.done) {
            tagpu_wikireplay_step(&r);
            replay_frame(&r, g.e.id, g.e.demo[j].id, r.done, g.lay ? &g.lay->sight : NULL);
        }
        tagpu_wikireplay_free(&r);
    }
    run_close(&g);
    return 0;
}

/* a press of Try it's by replay.js's name: a control, or the question's Yes or a team row's fold,
   which no script names */
static int press_of(const char* name)
{
    return name && !strcmp(name, "leave:yes") ? WIKI_HUD_LEAVE_YES : name && !strcmp(name, "fold") ? WIKI_HUD_FOLD
         : tagpu_wikidata_hud(name);
}

/* a WIKI_KEY_* by sim.js's name, or -1 */
static int key_of(const char* name)
{
    int k;
    for (k = 0; name && k < WIKI_NKEYS; k++)
        if (!strcmp(name, tagpu_wikidata_key_name(k))) return k;
    return -1;
}

/* Try it yourself on the entry's first demo with the file TRACE's presses, one line a tick */
static int replaytrace(const char* path, const char* entry, const char* layout, const char* casting, const char* rival,
                       int iface, int cob, const char* tracefile)
{
    char err[256];
    size_t n;
    char* text = slurp(tracefile, &n);
    TJDOC* doc = text ? tagpu_json_parse(text, n, err, sizeof err) : NULL;
    const TJV *root = doc ? tagpu_json_root(doc) : NULL, *p;
    const int last = (int)tj_num(tj_get(root, "end"), 0);
    GAMERUN g;
    WIKIREPLAY r;
    int rc = 0;
    if (!doc) {
        fprintf(stderr, "wikitest: %s: %s\n", tracefile, text ? err : "cannot read it");
        free(text);
        return 2;
    }
    if (!run_open(&g, path, entry, layout, casting, rival, iface, cob)) rc = 2;
    else if (!g.e.ndemo || !tagpu_wikireplay_init(&r, &g.e, &g.e.demo[0], WIKI_PLAY_TRY, g.lay, &g.ctx, err, sizeof err)) {
        fprintf(stderr, "wikitest: the replay is refused: %s\n", g.e.ndemo ? err : "no demo");
        run_close(&g);
        rc = 2;
    }
    if (rc) {
        tagpu_json_free(doc);
        free(text);
        return rc;
    }
    for (;;) {
        replay_frame(&r, g.e.id, g.e.demo[0].id, r.tick == 0 || r.done || r.tick >= last, g.lay ? &g.lay->sight : NULL);
        if (r.done || r.tick >= last) break;
        for (p = tj_is(tj_get(root, "presses"), TJ_ARR) ? tj_get(root, "presses")->first : NULL; p; p = p->next) {
            const char* what = tj_str(tj_at(p, 1), NULL);
            if ((int)tj_num(tj_at(p, 0), -1) != r.tick) continue;
            if (what && (!strcmp(what, "move") || !strcmp(what, "down") || !strcmp(what, "up"))) {
                /* a world input, as tagpu_wiki.c try_send builds one */
                WIKIINPUT in;
                memset(&in, 0, sizeof in);
                in.down = in.up = in.build = -1;
                if (!strcmp(what, "move")) {
                    in.has = WIKI_IN_MOVE;
                    in.move.x = tj_num(tj_at(p, 2), 0);
                    in.move.z = tj_num(tj_at(p, 3), 0);
                } else if (!strcmp(what, "down")) in.down = key_of(tj_str(tj_at(p, 2), NULL));
                else in.up = key_of(tj_str(tj_at(p, 2), NULL));
                tagpu_wikireplay_input(&r, &in);
            } else tagpu_wikireplay_press(&r, press_of(what), (int)tj_num(tj_at(p, 2), 0));
        }
        tagpu_wikireplay_step(&r);
    }
    tagpu_wikireplay_free(&r);
    run_close(&g);
    tagpu_json_free(doc);
    free(text);
    return 0;
}

/* a message on stderr; 2, the exit of a run that could not go on */
static int cannot(const char* what)
{
    fprintf(stderr, "wikitest: %s\n", what);
    return 2;
}

/* ---- refusals, case by case ---- */

/* One case's verdict: 1 accepted, else 0 with why in `err`. The steps are sim.js's make() in its
   order, each one the C side has: the casting's reader (checkCasting, which sim.js's binder
   calls first), the entry's loader (text.js's checkEntry among its refusals), the layout's, and
   tagpu_wikisim_init in the case's context. A `loader` case is the entry's loader alone, as wiki.js
   loads an entry: a text entry has no demo to make a sim of. */
static int refuse_case(const TJV* v, const char* entry, char* err, size_t errn)
{
    const TJV *layout = tj_get(v, "layout"), *casting = tj_get(v, "casting"), *rival = tj_get(v, "rival");
    const int scripted = (int)tj_num(tj_get(v, "scripted"), 0);
    WIKICASTING k, rk;
    WIKISIMCTX ctx;
    WIKIENTRY e;
    WIKILAYOUT* lay = NULL;
    WIKISIM s;
    int ok;
    ctx.side = (int)tj_num(tj_get(v, "side"), 0);
    ctx.iface = (int)tj_num(tj_get(v, "iface"), 1);
    ctx.cast = casting && !tj_is(casting, TJ_NULL) ? &k : NULL;
    ctx.cob = NULL;
    ctx.snd = NULL;
    ctx.rival = rival && !tj_is(rival, TJ_NULL) ? &rk : NULL;
    if (tj_is(tj_get(v, "loader"), TJ_TRUE)) {
        if (!tagpu_wikidata_entry(&e, entry, strlen(entry), err, errn)) return 0;
        tagpu_wikidata_entry_free(&e);
        return 1;
    }
    if (ctx.cast && !tagpu_wikidata_casting(casting, &k, err, errn)) return 0;
    if (ctx.rival && !tagpu_wikidata_casting(rival, &rk, err, errn)) return 0;
    if (!tagpu_wikidata_entry(&e, entry, strlen(entry), err, errn)) return 0;
    ok = !(layout && !tj_is(layout, TJ_NULL) && !(lay = tagpu_wikidata_layout_json(layout, err, errn)));
    if (ok && tj_is(tj_get(v, "replay"), TJ_TRUE)) {
        WIKIREPLAY r;
        if ((ok = tagpu_wikireplay_init(&r, &e, e.ndemo ? &e.demo[0] : NULL, WIKI_PLAY_DEMO, lay, &ctx, err, errn)))
            tagpu_wikireplay_free(&r);
    } else if (ok && (ok = tagpu_wikisim_init(&s, &e, e.ndemo ? &e.demo[0] : NULL, scripted, lay, &ctx, err, errn)))
        tagpu_wikisim_free(&s);
    tagpu_wikidata_entry_free(&e);
    free(lay);
    return ok;
}

static int refuse(const char* path)
{
    char err[256];
    size_t n;
    char* text = slurp(path, &n);
    char *line, *end;
    int rc = 0;
    if (!text) { fprintf(stderr, "wikitest: cannot read %s\n", path); return 2; }
    for (line = text; *line; line = end) {
        TJDOC* doc;
        const TJV* v;
        char name[128], *entry;
        for (end = line; *end && *end != '\n'; end++) {}
        if (*end) *end++ = 0;
        if (!*line) continue;
        for (entry = end; *end && *end != '\n'; end++) {}
        if (*end) *end++ = 0;
        if (!(doc = tagpu_json_parse(line, strlen(line), err, sizeof err))) { rc = cannot(err); break; }
        v = tagpu_json_root(doc);
        snprintf(name, sizeof name, "%s", tj_str(tj_get(v, "name"), "?"));
        err[0] = 0;
        if (refuse_case(v, entry, err, sizeof err)) printf("accepted %s\n", name);
        else printf("refused %s: %s\n", name, err);
        tagpu_json_free(doc);
    }
    free(text);
    return rc;
}

/* ---- castings and layouts, read and written back ---- */

static void casting_out(const WIKICASTING* k)
{
    static const char* const FIG[12] = { "buildTime", "workerTime", "buildDistance", "maxVelocity", "extracts", "energyMake",
                                         "buildCostMetal", "buildCostEnergy", "metalMake", "metalStorage", "energyStorage",
                                         "energyUse" };
    int c, i;
    fputs("{\"ok\":true,\"casts\":[", stdout);
    for (c = 0; c < WIKI_NCAST; c++) {
        const WIKICASTDEF* d = &k->cast[c];
        const double fig[12] = { d->build_time, d->worker_time, d->build_distance, d->max_velocity, d->extracts, d->energy_make,
                                 d->build_cost_metal, d->build_cost_energy, d->metal_make, d->metal_storage, d->energy_storage,
                                 d->energy_use };
        if (c) putchar(',');
        if (!d->bound) { fputs("null", stdout); continue; }
        putchar('{');
        wh_key("type", 1); wh_str(d->type);
        wh_key("title", 0); wh_str(d->title);
        wh_key("fx", 0); printf("%d", d->fx);
        wh_key("fz", 0); printf("%d", d->fz);
        for (i = 0; i < 12; i++) { wh_key(FIG[i], 0); wh_num(fig[i]); }
        wh_key("canstop", 0); printf("%d", d->canstop);
        wh_key("activatewhenbuilt", 0); printf("%d", d->activate_when_built);
        wh_key("canreclamate", 0); printf("%d", d->canreclamate);
        wh_key("sightDistance", 0); printf("%d", d->sight_distance);
        wh_key("radarDistance", 0); printf("%d", d->radar_distance);
        wh_key("sonarDistance", 0); printf("%d", d->sonar_distance);
        wh_key("modelHeight", 0); printf("%d", d->model_height);
        wh_key("vent", 0); printf("%d", d->vent);
        wh_key("g", 0);
        for (i = 0; i < WIKI_FP_MAX; i++) printf("%c%u", i ? ',' : '[', (unsigned)d->g[i]);
        putchar(']');
        wh_key("builds", 0); printf("%u", (unsigned)d->builds);
        wh_key("corpse", 0);
        if (!d->has_corpse) fputs("null", stdout);
        else {
            const WIKIFEATDEF* f = &d->corpse;
            putchar('{');
            wh_key("name", 1); wh_str(f->name);
            wh_key("fx", 0); printf("%d", f->fx);
            wh_key("fz", 0); printf("%d", f->fz);
            wh_key("metal", 0); wh_num(f->metal);
            wh_key("energy", 0); wh_num(f->energy);
            wh_key("height", 0); printf("%d", f->height);
            wh_key("blocking", 0); printf("%d", f->blocking);
            wh_key("reclaimable", 0); printf("%d", f->reclaimable);
            wh_key("object", 0); wh_str(f->object);
            putchar('}');
        }
        putchar('}');
    }
    puts("]}");
}

static void layout_out(const WIKILAYOUT* L)
{
    int i, j, k, n;
    printf("{\"ok\":true,\"height\":%d,\"screens\":[", L->height);
    for (i = 0; i < L->nscreen; i++) { if (i) putchar(','); wh_str(L->screen[i].name); }
    fputs("],\"sides\":[", stdout);
    for (i = 0; i < L->nside; i++) {
        const WIKISIDEDEF* d = &L->side[i];
        if (i) putchar(',');
        putchar('{');
        wh_key("name", 1); wh_str(d->name);
        wh_key("prefix", 0); wh_str(d->prefix);
        wh_key("main2", 0); wh_str(L->screen[d->main2].name);
        wh_key("gen", 0); wh_str(L->screen[d->gen].name);
        wh_key("types", 0);
        putchar('{');
        for (j = 0, n = 0; j < L->ntype; j++) {
            const WIKIPAGES* t = &L->type[j];
            if (t->side != i) continue;
            wh_key(t->name, n++ == 0);
            putchar('{');
            wh_key("pages", 1);
            putchar('[');
            for (k = 0; k < t->npage; k++) { if (k) putchar(','); wh_str(L->screen[t->page[k]].name); }
            putchar(']');
            wh_key("orders", 0); wh_str(t->orders >= 0 ? L->screen[t->orders].name : NULL);
            putchar('}');
        }
        fputs("}}", stdout);
    }
    fputs("],\"sight\":", stdout);
    if (!L->sight.numtables) fputs("null", stdout);
    else {
        printf("{\"numtables\":%d,\"tables\":[", L->sight.numtables);
        for (i = 0; i < L->sight.nidx; i++) {
            printf("%s{\"idx\":%d,\"cells\":[", i ? "," : "", L->sight.idx[i]);
            for (k = L->sight.first[i]; k < L->sight.first[i + 1]; k++)
                printf("%s[%d,%d,%d]", k > L->sight.first[i] ? "," : "", L->sight.cell[k].dc, L->sight.cell[k].dr,
                       L->sight.cell[k].n);
            fputs("]}", stdout);
        }
        fputs("]}", stdout);
    }
    puts("}");
}

static int data(const char* path)
{
    char err[256];
    size_t n;
    char* text = slurp(path, &n);
    char *line, *end;
    int rc = 0;
    if (!text) { fprintf(stderr, "wikitest: cannot read %s\n", path); return 2; }
    for (line = text; *line && !rc; line = end) {
        TJDOC *head, *doc;
        char* item;
        for (end = line; *end && *end != '\n'; end++) {}
        if (*end) *end++ = 0;
        if (!*line) continue;
        for (item = end; *end && *end != '\n'; end++) {}
        if (*end) *end++ = 0;
        if (!(head = tagpu_json_parse(line, strlen(line), err, sizeof err))) { rc = cannot(err); break; }
        if (!(doc = tagpu_json_parse(item, strlen(item), err, sizeof err))) {
            tagpu_json_free(head);
            rc = cannot(err);
            break;
        }
        err[0] = 0;
        if (!strcmp(tj_str(tj_get(tagpu_json_root(head), "kind"), ""), "casting")) {
            WIKICASTING k;
            if (tagpu_wikidata_casting(tagpu_json_root(doc), &k, err, sizeof err)) casting_out(&k);
            else { fputs("{\"ok\":false,\"why\":", stdout); wh_str(err); puts("}"); }
        } else {
            WIKILAYOUT* L = tagpu_wikidata_layout_json(tagpu_json_root(doc), err, sizeof err);
            if (L) layout_out(L);
            else { fputs("{\"ok\":false,\"why\":", stdout); wh_str(err); puts("}"); }
            free(L);
        }
        tagpu_json_free(doc);
        tagpu_json_free(head);
    }
    free(text);
    return rc;
}

static int constants(void)
{
    static const WIKITRIPROW rows[WIKI_TRIP_NROWS] = WIKI_TRIP_ROWS;
    static const int speeds[WIKI_REPLAY_NSPEED] = WIKI_REPLAY_SPEEDS;
    int k, n;
    printf("{\"TRIP\":%d,\"TRIP_DOWN\":%d,\"TRIP_UP\":%d,\"PICK\":%d,\"PICK_DOWN\":%d,\"PICK_UP\":%d,\"TRIP_ROWS\":[",
           WIKI_TRIP, WIKI_TRIP_DOWN, WIKI_TRIP_UP, WIKI_PICK, WIKI_PICK_DOWN, WIKI_PICK_UP);
    for (k = 0; k < WIKI_TRIP_NROWS; k++)
        printf("%s{\"glide\":%d,\"hold\":%d,\"gap\":%d,\"back\":%d,\"hover\":%d}", k ? "," : "", rows[k].glide,
               rows[k].hold, rows[k].gap, rows[k].back, rows[k].hover);
    fputs("],\"REPLAY\":{\"CONTROLS\":[", stdout);
    for (k = 0; k < WIKI_NHUD; k++) { if (k) putchar(','); wh_str(tagpu_wikidata_hud_name(k)); }
    fputs("],\"SPEEDS\":[", stdout);
    for (k = 0; k < WIKI_REPLAY_NSPEED; k++) printf("%s%d", k ? "," : "", speeds[k]);
    printf("],\"SPEED0\":%d,\"SEEK\":%d,\"WALK\":%d,\"STEP\":%d,\"NEAR\":%d,\"BOOKS\":%d,\"MARKS\":%d,\"FILL\":%d,"
           "\"HUD_DOWN\":%d,\"HUD_UP\":%d,\"DEATH\":%d,\"DEFEAT\":%d,\"GAME_END_MAX\":%d}}\n",
           WIKI_REPLAY_SPEED0, WIKI_REPLAY_SEEK, WIKI_REPLAY_WALK, WIKI_REPLAY_STEP, WIKI_REPLAY_NEAR, WIKI_REPLAY_BOOKS,
           WIKI_REPLAY_MARKS, WIKI_REPLAY_FILL, WIKI_REPLAY_HUD_DOWN, WIKI_REPLAY_HUD_UP, HUD_MK_DEATH, HUD_MK_DEFEAT,
           WIKI_GAME_END_MAX);
    fputs("{\"CHIPS\":[", stdout);
    for (k = 0; k < WIKI_NCHIP; k++) { if (k) putchar(','); wh_str(tagpu_wikidata_chip_name(k)); }
    printf("],\"PLAYERS_CONTROLS\":[");
    for (k = 0, n = 0; k < WIKI_NHUD; k++)
        if (tagpu_wikidata_hud_players(k)) { if (n++) putchar(','); wh_str(tagpu_wikidata_hud_name(k)); }
    printf("],\"PLAYERS_DEATHS\":%d,\"PLAYERS_DEATH_SPAN\":%d,\"SIGHT_MIN\":%d,\"SIGHT_NUMTABLES\":%d,\"SIGHT_IDX\":%d,"
           "\"SIGHT_REACH\":%d,\"SIGHT_CELLS\":%d,\"STAT_MEASURES\":%d,\"STAT_MEASURE0\":%d,\"STAT_PTS\":%d,\"TOG_ALL\":%d,"
           "\"NOT_ASKED\":%d,\"PASS\":%d}\n", WIKI_PLAYERS_DEATHS, WIKI_PLAYERS_DEATH_SPAN, WIKI_SIGHT_MIN,
           WIKI_SIGHT_NUMTABLES, WIKI_SIGHT_IDX, WIKI_SIGHT_REACH, WIKI_SIGHT_CELLS, WIKI_STAT_MEASURES, WIKI_STAT_MEASURE0,
           WIKI_STAT_PTS, (int)WIKI_REPLAY_TOG_ALL, WIKI_REPLAY_NOT_ASKED, WIKI_REPLAY_PASS);
    return 0;
}

/* ---- the recorder's rules ---- */

static int rules(const char* path)
{
    size_t n;
    char* text = slurp(path, &n);
    char *line, *end;
    int rc = 0;
    if (!text) { fprintf(stderr, "wikitest: cannot read %s\n", path); return 2; }
    printf("{\"REFRESH_TICKS\":%d,\"FORK_LIMIT\":%d,\"TIMELINES\":%d}\n", REC_REFRESH_TICKS, REC_FORK_LIMIT,
           RECR_TIMELINES);
    for (line = text; *line && !rc; line = end) {
        unsigned a, b, c, d;
        int len;
        for (end = line; *end && *end != '\n'; end++) {}
        if (*end) *end++ = 0;
        if (!*line) continue;
        if (sscanf(line, "refresh %u %u %u %u", &a, &b, &c, &d) == 4)
            printf("%d\n", reclossy_is_refresh(a, b, c, d));
        else if (sscanf(line, "walk %u%n", &a, &len) == 1) {
            unsigned t, prev = 0, k = 0;
            const char* p = line + len;
            const char* sep = "";
            putchar('[');
            while (sscanf(p, " %u%n", &t, &len) == 1) {
                p += len;
                if (reclossy_is_refresh(++k, t, prev, a)) {
                    printf("%s%u", sep, t);
                    sep = ",";
                    prev = t;
                }
            }
            puts("]");
            p += strspn(p, " ");
            if (*p) { rc = cannot("a walk case's iterations are GameTimes"); break; }
        } else if (sscanf(line, "evict %u %u%n", &a, &b, &len) == 2) {
            RecForkRow rows[RECR_TIMELINES];
            unsigned k = 0, id, live;
            unsigned long long stamp;
            uint32_t evict;
            const char* p = line + len;
            int ok;
            while (k < RECR_TIMELINES && sscanf(p, " %u:%u:%llu%n", &id, &live, &stamp, &len) == 3) {
                rows[k++] = (RecForkRow){id, (uint8_t)live, stamp};
                p += len;
            }
            p += strspn(p, " ");
            if (*p) { rc = cannot("an evict case's rows are at most RECR_TIMELINES of ID:LIVE:USED"); break; }
            ok = rec_fork_evict(rows, k, a, b, &evict);
            if (evict == UINT32_MAX) printf("{\"ok\":%d,\"evict\":null}\n", ok);
            else printf("{\"ok\":%d,\"evict\":%u}\n", ok, (unsigned)evict);
        } else rc = cannot("a rules case is not refresh, walk or evict");
    }
    free(text);
    return rc;
}

/* ---- malformed data ---- */

static void put32(unsigned char* p, size_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* an archive in tools/hudpak.py's format of `n` files, names[i] holding texts[i]; `*len` its size */
static unsigned char* pack(const char* const* names, const char* const* texts, int n, size_t* len)
{
    size_t head = 12 + 16 * (size_t)n, at = head, noff = head, i;
    unsigned char* p;
    for (i = 0; i < (size_t)n; i++) at += strlen(names[i]);
    for (i = 0; i < (size_t)n; i++) at = ((at + 3) & ~(size_t)3) + strlen(texts[i]);
    if (!(p = (unsigned char*)calloc(1, at))) return NULL;
    memcpy(p, "HPAK", 4);
    put32(p + 4, 1);
    put32(p + 8, (size_t)n);
    at = head;
    for (i = 0; i < (size_t)n; i++) at += strlen(names[i]);
    for (i = 0; i < (size_t)n; i++) {
        size_t nl = strlen(names[i]), tl = strlen(texts[i]);
        at = (at + 3) & ~(size_t)3;
        put32(p + 12 + 16 * i, noff);
        put32(p + 16 + 16 * i, nl);
        put32(p + 20 + 16 * i, at);
        put32(p + 24 + 16 * i, tl);
        memcpy(p + noff, names[i], nl);
        memcpy(p + at, texts[i], tl);
        noff += nl;
        at += tl;
    }
    *len = at;
    return p;
}

static int said(int ok, const char* name, const char* why)
{
    if (ok) printf("ok %s\n", name);
    else printf("FAILED %s: %s\n", name, why);
    return ok;
}

/* a tree whose `tree.json` is `tree`: it loads, as `nodes` nodes, the folders without an entry */
static int tree_case(const char* name, const char* tree, int nodes)
{
    const char* names[1] = { "tree.json" };
    const char* texts[1];
    char err[256], why[320];
    size_t n;
    unsigned char* pak;
    WIKIDATA* d;
    int ok;
    texts[0] = tree;
    if (!(pak = pack(names, texts, 1, &n))) return said(0, name, "out of memory");
    err[0] = 0;
    d = tagpu_wikidata_load(pak, n, err, sizeof err);
    snprintf(why, sizeof why, "loaded %d nodes and %d entries (%s), expected %d nodes", d ? d->nnode : -1,
             d ? d->nentry : -1, d ? "" : err, nodes);
    ok = d && d->nnode == nodes && d->nentry == 0;
    tagpu_wikidata_free(d);
    free(pak);
    return said(ok, name, why);
}

/* every case, its verdict in its line: the count is simparity.mjs's, as the comparison is */
/* lists given as objects: each reads as empty, never as as many unread elements as members. The
   one list given as a list is `demos`: given as an object it refuses the entry (a text entry's
   rule, tagpu_wikidata_entry), and with none the entry is a text entry, which carries no scenes */
static int entry_case(void)
{
    static const char TEXT[] =
        "{\"format\":2,\"id\":\"m\",\"text\":{\"a\":\"x\",\"b\":\"y\"},"
        "\"units\":{\"ARMMEX\":{\"footprint\":[2,2],\"buildTime\":10}},"
        "\"scenes\":{\"g\":{\"map\":{\"w\":8,\"h\":8},\"metal\":{\"r\":1},\"vents\":{\"v\":[1,1]},"
        "\"units\":{\"u\":{\"type\":\"ARMMEX\",\"at\":[1,1]}},\"selected\":{\"s\":\"u\"}}},"
        "\"demos\":[{}]}";
    WIKIENTRY e;
    char err[256], why[320];
    int ok;
    err[0] = 0;
    ok = tagpu_wikidata_entry(&e, TEXT, sizeof TEXT - 1, err, sizeof err);
    snprintf(why, sizeof why, "%s: text %d, metal %d, vents %d, units %d, selected %d, demos %d",
             ok ? "loaded" : err, ok ? e.ntext : -1, ok ? e.scenes[0].nmetal : -1, ok ? e.scenes[0].nvent : -1,
             ok ? e.scenes[0].nunit : -1, ok ? e.scenes[0].nselected : -1, ok ? e.ndemo : -1);
    ok = ok && !e.ntext && !e.scenes[0].nmetal && !e.scenes[0].nvent && !e.scenes[0].nunit && !e.scenes[0].nselected
         && e.ndemo == 1;
    tagpu_wikidata_entry_free(&e);
    return said(ok, "entry-lists-as-objects", why);
}

/* a unit whose "facing" is the JSON `facing` (NULL: none): refused when `refused`, else loaded
   as the unit vector (hx, hz), compared exactly. The entry has one demo, so it is no text entry */
static int facing_case(const char* name, const char* facing, int refused, double hx, double hz)
{
    char text[512], err[256], why[512];
    WIKIENTRY e;
    int loaded, ok;
    snprintf(text, sizeof text,
             "{\"format\":2,\"id\":\"f\",\"units\":{\"ARMCK\":{\"footprint\":[2,2],\"buildTime\":10,"
             "\"maxVelocity\":1}},\"scenes\":{\"g\":{\"map\":{\"w\":8,\"h\":8},\"units\":[{\"type\":\"ARMCK\","
             "\"at\":[40,40]%s%s}]}},\"demos\":[{}]}", facing ? ",\"facing\":" : "", facing ? facing : "");
    err[0] = 0;
    loaded = tagpu_wikidata_entry(&e, text, strlen(text), err, sizeof err);
    if (loaded && e.scenes[0].nunit != 1) snprintf(err, sizeof err, "loaded %d units", e.scenes[0].nunit);
    ok = loaded && e.scenes[0].nunit == 1;
    snprintf(why, sizeof why, "%s: (%.17g, %.17g), expected %s(%.17g, %.17g)", ok ? "loaded" : err,
             ok ? e.scenes[0].unit[0].hx : 0.0, ok ? e.scenes[0].unit[0].hz : 0.0, refused ? "a refusal, not " : "", hx, hz);
    ok = refused ? !loaded : ok && e.scenes[0].unit[0].hx == hx && e.scenes[0].unit[0].hz == hz;
    if (loaded) tagpu_wikidata_entry_free(&e);
    return said(ok, name, why);
}

/* an entry of one type, ARMX, whose record is `unit`, and one demo: refused when `refused`, else
   loaded with cast `cast` and G rows `g` (its first four), compared exactly */
static int unit_case(const char* name, const char* unit, int refused, int cast, const unsigned* g)
{
    char text[512], err[256], why[512];
    WIKIENTRY e;
    int loaded, ok, i;
    snprintf(text, sizeof text, "{\"format\":2,\"id\":\"u\",\"units\":{\"ARMX\":%s},\"scenes\":{\"g\":{\"map\":{\"w\":8,\"h\":8}}},\"demos\":[{}]}", unit);
    err[0] = 0;
    loaded = tagpu_wikidata_entry(&e, text, strlen(text), err, sizeof err);
    snprintf(why, sizeof why, "%s: cast %d, g %u %u %u %u; expected %scast %d, g %u %u %u %u", loaded ? "loaded" : err,
             loaded ? e.def[0].cast : -2, loaded ? e.def[0].g[0] : 0, loaded ? e.def[0].g[1] : 0, loaded ? e.def[0].g[2] : 0,
             loaded ? e.def[0].g[3] : 0, refused ? "a refusal, not " : "", cast, g[0], g[1], g[2], g[3]);
    ok = refused ? !loaded : loaded && e.def[0].cast == cast;
    for (i = 0; !refused && ok && i < WIKI_FP_MAX; i++) ok = e.def[0].g[i] == (i < 4 ? g[i] : 0);
    if (loaded) tagpu_wikidata_entry_free(&e);
    return said(ok, name, why);
}

static int malformed(void)
{
    int ok = 1;
    /* an `entry` that is not a string is a folder, counted and read as one: its children too */
    ok &= tree_case("tree-entry-number",
                    "{\"format\":1,\"tree\":[{\"entry\":5,\"children\":[{\"title\":\"a\"},"
                    "{\"title\":\"b\"},{\"title\":\"c\"}]}]}", 4);
    ok &= tree_case("tree-entry-object",
                    "{\"format\":1,\"tree\":[{\"entry\":{\"id\":\"x\"},\"children\":[{\"title\":\"a\","
                    "\"children\":[{\"title\":\"b\"}]}]}]}", 3);
    ok &= entry_case();
    ok &= facing_case("facing-absent", NULL, 0, 0, 1);
    ok &= facing_case("facing-null", "null", 0, 0, 1);
    ok &= facing_case("facing-normalised", "[3,4]", 0, 3.0 / 5.0, 4.0 / 5.0);
    ok &= facing_case("facing-zero", "[0,0]", 1, 0, 0);
    ok &= facing_case("facing-one-number", "[1]", 1, 0, 0);
    ok &= facing_case("facing-three-numbers", "[1,2,3]", 1, 0, 0);
    ok &= facing_case("facing-not-a-number", "[1,\"a\"]", 1, 0, 0);
    ok &= facing_case("facing-an-object", "{\"x\":1,\"z\":0}", 1, 0, 0);
    ok &= facing_case("facing-square-underflows", "[1e-200,0]", 1, 0, 0);
    ok &= facing_case("facing-square-overflows", "[1e200,0]", 1, 0, 0);
    {
        static const unsigned none[4] = { 0, 0, 0, 0 }, listed[4] = { 2, 0, 8, 0 }, all[4] = { 7, 7, 0, 0 },
                              row16[4] = { 0xFFFF, 0, 0, 0 };
        ok &= unit_case("cast-named", "{\"cast\":\"geo\",\"footprint\":[2,2],\"buildTime\":1}", 0, WIKI_CAST_GEO, none);
        ok &= unit_case("cast-absent", "{\"footprint\":[2,2],\"buildTime\":1}", 0, -1, none);
        ok &= unit_case("cast-unknown", "{\"cast\":\"Geo\",\"footprint\":[2,2],\"buildTime\":1}", 0, -1, none);
        ok &= unit_case("g-listed", "{\"footprint\":[4,4],\"buildTime\":1,\"vent\":true,\"g\":[[1,0],[3,2]]}", 0, -1, listed);
        ok &= unit_case("g-vent-all", "{\"footprint\":[3,2],\"buildTime\":1,\"vent\":true}", 0, -1, all);
        ok &= unit_case("g-vent-all-16", "{\"footprint\":[16,1],\"buildTime\":1,\"vent\":true}", 0, -1, row16);
        /* the G rows hold 16 cells a side: sim.js would play it, the C refuses it (tagpu_wikidata.h) */
        ok &= unit_case("g-footprint-past-16", "{\"footprint\":[17,2],\"buildTime\":1,\"vent\":true}", 1, -1, none);
        ok &= unit_case("no-g-footprint-past-16", "{\"footprint\":[17,2],\"buildTime\":1,\"g\":[]}", 0, -1, none);
    }
    (void)ok;
    return 0;
}

/* THE SIGHT'S COST (the usage above): the worst scene of the costs spec, fills timed with clock() */
static int sightcost(const char* path, int n)
{
    static WIKIVUNIT unit[256];
    static WIKIDEATH death[20];
    static uint8_t lit[WIKI_SIGHT_GRID], dark[WIKI_SIGHT_GRID], con[256];
    static char ids[256][16];   /* "u" and any int, which -Wformat-truncation asks room for */
    WIKIVTYPE type[1];
    WIKIVIEW v;
    WIKILAYOUT* L;
    size_t npak;
    char err[256], *pak = slurp(path, &npak);
    clock_t t0;
    double fill, cont;
    int i, on = 0;
    if (!pak) { fprintf(stderr, "wikitest: cannot read %s\n", path); return 2; }
    if (!(L = tagpu_wikidata_layout_pak((const unsigned char*)pak, npak, "side/fixture.json", err, sizeof err))) {
        fprintf(stderr, "wikitest: %s\n", err);
        free(pak);
        return 2;
    }
    memset(&v, 0, sizeof v);
    memset(type, 0, sizeof type);
    type[0].sight_distance = 290;
    type[0].radar_distance = 400;
    type[0].activates = 1;
    type[0].model_height = 39;
    for (i = 0; i < 256; i++) {
        snprintf(ids[i], sizeof ids[i], "u%d", i);
        unit[i].id = ids[i];
        unit[i].owner = i < 128 ? 0 : 1;
        unit[i].x = 16 + (i % 16) * 96 + (i / 128) * 40;
        unit[i].z = 64 + ((i / 16) % 8) * 80;
        unit[i].built = 1;
    }
    for (i = 0; i < 20; i++) {
        death[i].tick = 0;
        death[i].x = 100 + i * 70;
        death[i].z = 400;
    }
    v.w = 96;
    v.h = 48;
    v.game = 1;
    v.nunit = 256;
    v.unit = unit;
    v.ndeath = 20;
    v.death = death;
    v.ntype = 1;
    v.type = type;
    if (n < 1) n = 1;
    t0 = clock();
    for (i = 0; i < n; i++) tagpu_wikisim_sight_lit(&v, &L->sight, 0, 30, lit);
    fill = (double)(clock() - t0) * 1e6 / CLOCKS_PER_SEC / n;
    t0 = clock();
    for (i = 0; i < n; i++) tagpu_wikisim_contacts(&v, dark, 0, con);
    cont = (double)(clock() - t0) * 1e6 / CLOCKS_PER_SEC / n;
    for (i = 0; i < 48 * 24; i++) on += lit[i] != 0;
    printf("{\"fill_us\":%.2f,\"contacts_us\":%.2f,\"lit\":%d}\n", fill, cont, on);
    free(L);
    free(pak);
    return 0;
}

int main(int argc, char** argv)
{
    int rc;
    cob_init(&g_cob);
    if ((argc == 3 || argc == 4 || argc == 6 || argc == 7) && !strcmp(argv[1], "demos")) {
        const int cob = argc == 7 && strcmp(argv[6], "-");
        if (cob && !cob_load(&g_cob, argv[6])) return 2;
        rc = demos(argv[2], argc >= 4 ? argv[3] : NULL, argc >= 6 ? argv[4] : NULL, argc >= 6 ? atoi(argv[5]) : 1, cob);
        cob_free(&g_cob);
        return rc;
    }
    if (argc == 10 && !strcmp(argv[1], "game")) {
        const int cob = strcmp(argv[8], "-") != 0;
        if (cob && !cob_load(&g_cob, argv[8])) return 2;
        rc = game(argv[2], argv[3], argv[4], argv[5], argv[6], atoi(argv[7]), cob, atoi(argv[9]));
        cob_free(&g_cob);
        return rc;
    }
    if (argc == 9 && !strcmp(argv[1], "sweep")) {
        const int cob = strcmp(argv[8], "-") != 0;
        if (cob && !cob_load(&g_cob, argv[8])) return 2;
        rc = sweep(argv[2], argv[3], argv[4], argv[5], argv[6], atoi(argv[7]), cob);
        cob_free(&g_cob);
        return rc;
    }
    if ((argc == 9 && !strcmp(argv[1], "replay")) || (argc == 10 && !strcmp(argv[1], "replaytrace"))) {
        const int cob = strcmp(argv[8], "-") != 0;
        if (cob && !cob_load(&g_cob, argv[8])) return 2;
        rc = argc == 9 ? replay(argv[2], argv[3], argv[4], argv[5], argv[6], atoi(argv[7]), cob)
                       : replaytrace(argv[2], argv[3], argv[4], argv[5], argv[6], atoi(argv[7]), cob, argv[9]);
        cob_free(&g_cob);
        return rc;
    }
#ifdef _WIN32
    /* the host's bytes as written, in and out: msvcrt's text mode would add a CR to each answer */
    if (argc >= 2 && (!strcmp(argv[1], "serve") || !strcmp(argv[1], "trace"))) {
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
    }
#endif
    if (argc == 2 && !strcmp(argv[1], "serve")) return wikihost_serve(stdin);
    if (argc == 3 && !strcmp(argv[1], "trace")) {
        FILE* f = fopen(argv[2], "rb");
        if (!f) { fprintf(stderr, "wikitest: cannot read %s\n", argv[2]); return 2; }
        rc = wikihost_serve(f);
        fclose(f);
        return rc;
    }
    if (argc == 3 && !strcmp(argv[1], "refuse")) return refuse(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "data")) return data(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "constants")) return constants();
    if (argc == 3 && !strcmp(argv[1], "rules")) return rules(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "malformed")) return malformed();
    if (argc == 4 && !strcmp(argv[1], "sightcost")) return sightcost(argv[2], atoi(argv[3]));
    fprintf(stderr, "usage: wikitest demos PAK [LAYOUT [CASTING IFACE [COB]]]"
            " | wikitest game PAK ENTRY LAYOUT CASTING RIVAL IFACE COB SCRIPTED"
            " | wikitest sweep PAK ENTRY LAYOUT CASTING RIVAL IFACE COB"
            " | wikitest replay PAK ENTRY LAYOUT CASTING RIVAL IFACE COB"
            " | wikitest replaytrace PAK ENTRY LAYOUT CASTING RIVAL IFACE COB TRACE"
            " | wikitest serve | wikitest trace FILE | wikitest refuse FILE | wikitest data FILE"
            " | wikitest constants | wikitest rules FILE"
            " | wikitest malformed | wikitest sightcost PAK N\n");
    return 2;
}
