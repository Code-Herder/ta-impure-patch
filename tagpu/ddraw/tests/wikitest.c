/* wikitest.c -- the wiki's simulation host and the C half of the replay layer's parity check, for
   tools/wiki/simparity.mjs and the mockup (tools/wiki/mockup/simhost.js): the real tagpu_wikidata.c,
   tagpu_wikisim.c over the core's tasim_*.c and tagpu_wikireplay.c, compiled natively (or for
   Windows, run under Wine; tools/wiki/wikibuild.mjs is the recipe of the three builds).

   The modes that take a context take PAK, an archive of art/wiki/ (tools/hudpak.py --set wiki),
   loaded as the DLL loads WIKIPAK; ENTRY, a file holding a game entry; LAYOUT, a sidebar layout of
   the archive (side/LAYOUT.json; none when "-"); CASTING and RIVAL, files holding one side's
   casting record, the player's and the opponent's (none when "-"); IFACE, the Interface Type; and
   COB, the units' scripts and sounds the file names (none when "-"; see cob_load).

   `wikitest replay PAK ENTRY LAYOUT CASTING RIVAL IFACE COB` plays every demo of the game entry the
   file ENTRY holds through the replay layer (tagpu_wikireplay, its viewer's script played) in the
   context the arguments give, and prints one line per demo tick from 0: {"entry", "demo", "t",
   "h": tagpu_wikireplay_hash, "r": the replay's view in replay.js's shape, "gh": the game's
   tagpu_wikisim_hash, "v": the game's view (its map on the first and the last tick), "o": its units'
   orders}; a demo the replay refuses is one line, {"entry", "demo", "refused": why}.

   `wikitest replaytrace PAK ENTRY LAYOUT CASTING RIVAL IFACE COB TRACE` plays Try it yourself on the
   entry's first demo (tagpu_wikireplay, create) with the presses the file TRACE holds,
   {"end": N, "presses": [[tick, control's name, value], ...]}: each tick from 0 its frame is printed
   as replay prints it, then the presses of that tick are made in order, then it steps, up to tick N
   or the replay's end. In a live entry's trace a press may be a world input instead, given through
   tagpu_wikireplay_input: [tick, "move", x, z], [tick, "down", key] or [tick, "up", key], a key by
   its WIKI_KEY_* name (key_of).

   `wikitest serve [--instance ID] [--packs-root DIR]` is the simulation host (tests/wikihost.c
   states its protocol, THE INSTANCE and THE PACKS' ROOT among it): JSON requests on stdin, one
   answer line each on stdout. `wikitest trace FILE` is the same loop reading FILE, with neither,
   so a recorded session replays through any build.

   `wikitest constants` prints the build trip's constants and the replay layer's as one JSON line, in
   the host's and replay.js's names.

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

   `wikitest worldq PAK ENTRY` opens the shared core's WORLDQ (tasim_worldq.h) for seat 0 on the sim of the
   entry's first demo as it is made, before its first tick, and prints every query's answer over the stage, in
   text: the map, the explored and metal grids, each type's definition, yard and build options, the coverage
   and the site test of each resource building at every first cell, the features, the units, their orders,
   the resolver's answers between every two units of the seat, the actions and the economy.
   tests/worldq/quickbuild.txt holds the answers hand-checked against art/wiki/entries/quickbuild.json's first
   scene; `wikitest worldq wiki.bin quickbuild | diff - tests/worldq/quickbuild.txt` holds the core to them.

   `wikitest impai PAK ENTRY LAYOUT` plays ImpAI's knowledge tick for every seat over the core's WORLDQ through
   each demo of a game entry and prints its digests every 30 ticks (impai_test says what); the three builds
   must print the same lines, and it exits 1 when the threat map differs from its cell-by-cell definition.

   `wikitest malformed` loads data the loader must survive -- each case an archive or an entry
   built here -- and prints one line per case, "ok <case>" or "FAILED <case>: why". The native
   build runs under AddressSanitizer, so a write past an allocation fails the run as well.

   Exit 0 when everything loaded and ran; the comparisons are simparity.mjs's. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "tagpu_json.h"
#include "tagpu_wikidata.h"
#include "tagpu_wikisim.h"
#include "tagpu_wikireplay.h"
#include "tagpu_recrules.h"
#include "tasim_worldq.h"
#include "impai.h"
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

/* A GAME ENTRY'S RUN: the file ENTRY's entry, the archive PAK, its layout LAYOUT, the castings in
   the files CASTING and RIVAL ("-" for none) and the context they make with IFACE and the scripts;
   the castings live in it, so the sims made in its context must not outlive it */
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

/* a WIKI_KEY_* by its name (the host's KEYS, in WIKI_KEY_* order), or -1 */
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
        /* the G rows hold 16 cells a side: a vent type wider than 16 with no G list is refused (tagpu_wikidata.h) */
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

/* ---- worldq: the core's WORLDQ answers over one entry's first scene ---- */

static void wq_grid(const WORLDQ* q, const WQ_MAP* m, const char* what, int (*cell)(const WORLDQ*, int, int, int), int arg)
{
    int x, z;
    for (z = 0; z < m->h; z++) {
        printf("%s %2d ", what, z);
        for (x = 0; x < m->w; x++) putchar(cell(q, x, z, arg) ? '1' : '0');
        putchar('\n');
    }
}

static int wq_explored_at(const WORLDQ* q, int x, int z, int arg) { (void)arg; return q->ops->explored(q, x, z); }
static int wq_site_at(const WORLDQ* q, int x, int z, int type) { return q->ops->site_ok(q, type, x, z, 0, NULL); }

static void wq_order_out(const WQ_ORDER* o)
{
    printf("  order %u action=%u build=%u started=%u owned=%u type=%u target=%d pos=%d,%d,%d flags=0x%x\n", o->place,
           o->action, o->build, o->started, o->owned, o->type, o->target == WQ_NONE ? -1 : (int)o->target, o->pos[0], o->pos[1],
           o->pos[2], o->flags);
}

static int worldq(const char* path, const char* entry)
{
    static WQ_ORDER list[PLACE_LIST_CAP];
    char err[256];
    size_t n;
    char* pak;
    WIKIDATA* d;
    WIKISIM s;
    WIKISIMCTX ctx;
    WQ_CORE core;
    WORLDQ q;
    WQ_MAP m;
    WQ_DEF def;
    WQ_FEAT f;
    WQ_UNIT u, v;
    WQ_ECON econ;
    uint16_t opts[WIKI_MAX_DEFS];
    uint8_t yard[WIKI_FP_MAX * WIKI_FP_MAX];
    const WIKIENTRY* e = NULL;
    int i, k, x, z, t, first, last, rc = 0;
    memset(&ctx, 0, sizeof ctx);
    ctx.iface = 1;
    if (!(pak = slurp(path, &n))) { fprintf(stderr, "wikitest: cannot read %s\n", path); return 2; }
    if (!(d = tagpu_wikidata_load((const unsigned char*)pak, n, err, sizeof err))) {
        fprintf(stderr, "wikitest: %s\n", err);
        free(pak);
        return 2;
    }
    for (i = 0; i < d->nentry && !e; i++)
        if (!strcmp(d->entry[i].id, entry)) e = &d->entry[i];
    if (!e || !e->ndemo || !tagpu_wikisim_init(&s, e, &e->demo[0], 1, NULL, &ctx, err, sizeof err)) {
        fprintf(stderr, "wikitest: worldq %s: %s\n", entry, !e ? "no such entry" : !e->ndemo ? "no demo" : err);
        rc = 2;
        goto done;
    }
    tasim_worldq_open(&core, &q, &s.sim, &s.defs, 0);
    q.ops->map(&q, &m);
    printf("map w=%d h=%d threshold=%d sea=%u lostype=%u ndefs=%d nslots=%d gametime=%d\n", m.w, m.h, m.threshold, m.sea,
           m.lostype, q.ops->ndefs(&q), q.ops->nslots(&q), q.ops->gametime(&q));
    wq_grid(&q, &m, "explored", wq_explored_at, 0);
    for (z = 0; z < m.h; z++)
        for (x = 0; x < m.w; x++)
            if (q.ops->metal(&q, x, z) > 0) printf("metal %d,%d %d\n", x, z, q.ops->metal(&q, x, z));
    for (t = 1; t < q.ops->ndefs(&q); t++) {
        if (!q.ops->def(&q, t, &def)) { printf("def %d refused\n", t); continue; }
        printf("def %d %s fx=%d fz=%d kind=%u flags=0x%x strength=%g workertime=%u reach=%u speed=%d", t, def.name, def.fx,
               def.fz, def.kind, def.flags, def.strength, def.workertime, def.reach, def.speed);
        k = q.ops->yard(&q, t, yard, (int)sizeof yard);
        printf(" yard=");
        for (i = 0; i < k; i++) printf("%02X", yard[i]);
        k = q.ops->build_options(&q, t, opts, WIKI_MAX_DEFS);
        printf(" options=");
        for (i = 0; i < k; i++) printf("%s%u", i ? "," : "", opts[i]);
        putchar('\n');
        if (!def.kind) continue;
        for (z = 0; z < m.h; z++)
            for (x = 0; x < m.w; x++)
                if (q.ops->coverage(&q, x, z, def.fx, def.fz)) printf("coverage %d %d,%d %d\n", t, x, z, q.ops->coverage(&q, x, z, def.fx, def.fz));
        wq_grid(&q, &m, def.name, wq_site_at, t);
    }
    for (z = 0; z < m.h; z++)
        for (x = 0; x < m.w; x++)
            if (q.ops->feature(&q, x, z, &f))
                printf("feature %d,%d anchor=%d,%d def=%d fx=%d fz=%d flags=0x%02x metal=%g energy=%g\n", x, z, f.ax, f.az,
                       f.def == WQ_NONE ? -1 : (int)f.def, f.fx, f.fz, f.flags, f.metal, f.energy);
    q.ops->seat_units(&q, &first, &last);
    printf("seat units %d..%d\n", first, last);
    for (i = 1; i < q.ops->nslots(&q); i++) {
        if (!q.ops->unit(&q, i, &u)) { printf("unit %d -\n", i); continue; }
        printf("unit %d type=%u index=%u owner=%u finished=%u pos=%d,%d,%d state=0x%x\n", i, u.type, u.index, u.owner,
               u.finished, u.pos[0], u.pos[1], u.pos[2], u.state);
        k = q.ops->orders(&q, i, list, PLACE_LIST_CAP);
        for (x = 0; x < k; x++) wq_order_out(&list[x]);
        for (k = 1; k < q.ops->nslots(&q); k++)
            if (q.ops->unit(&q, k, &v))
                printf("  can %d: reclaim=%d guard=%d\n", k, q.ops->can(&q, i, WQ_CAN_RECLAIM, k, u.pos),
                       q.ops->can(&q, i, WQ_CAN_GUARD, k, u.pos));
    }
    printf("actions");
    for (i = 0; i < WQ_ACTS; i++) printf(" %d", q.ops->action(&q, i));
    printf("\neconomy %d\n", q.ops->economy(&q, &econ));
    tagpu_wikisim_free(&s);
done:
    tagpu_wikidata_free(d);
    free(pak);
    return rc;
}

/* `serve [--instance ID] [--packs-root DIR]`: an instance of 1..WH_INSTANCE_MAX letters, digits,
   '-' or '_' (it is printed into JSON as it is); the root needs realpath, which the Windows build
   has not, so that build refuses one rather than check nothing */
static int serve(int argc, char** argv)
{
    const char *instance = NULL, *root = NULL;
    int i;
    for (i = 2; i + 1 < argc; i += 2)
        if (!strcmp(argv[i], "--instance") && !instance) instance = argv[i + 1];
        else if (!strcmp(argv[i], "--packs-root") && !root) root = argv[i + 1];
        else break;
    if (i != argc || (instance && (!instance[0] || strlen(instance) > WH_INSTANCE_MAX
                                   || strspn(instance, "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_") != strlen(instance)))) {
        fprintf(stderr, "usage: wikitest serve [--instance ID] [--packs-root DIR], ID 1..%d of [0-9A-Za-z_-]\n", WH_INSTANCE_MAX);
        return 2;
    }
#ifdef _WIN32
    if (root) { fputs("wikitest: --packs-root needs realpath, which the Windows build has not\n", stderr); return 2; }
#endif
    return wikihost_serve(stdin, instance, root);
}

/* ---- impai: ImpAI's knowledge and maps on the core's WORLDQ, over a game entry's demos ---- */

#define IMPAI_TEST_SLOTS 1024

static int impai_test_pass(const void* ctx, int mclass, int cx, int cz)
{
    const WORLDQ* q = (const WORLDQ*)ctx;
    return q->ops->passable(q, mclass, cx, cz);
}

/* The stage's casts carry no weapons, so the test's threat table is fixed by type: dps 16 (t + 1) on the
   ground, 8 (t + 1) in the air and 4 (t + 1) on water, range 96 + 32 t px, and the value metal + energy / R
   of the cast's costs. */
static void impai_test_threat(const WIKIENTRY* e, IMPAI_THREAT* t, int n)
{
    int i;
    memset(t, 0, (size_t)n * sizeof *t);
    for (i = 1; i < n && i - 1 < e->ndef; i++) {
        const WIKIDEF* d = &e->def[i - 1];
        const double v = d->build_cost_metal + d->build_cost_energy / TASIM_ROLES_R;
        t[i].dps_ground = (uint16_t)(16 * i);
        t[i].dps_air = (uint16_t)(8 * i);
        t[i].dps_water = (uint16_t)(4 * i);
        t[i].range = (uint16_t)(96 + 32 * (i - 1));
        t[i].value = v > 0 ? (uint32_t)(v + 0.5) : 0;
    }
}

/* the threat map recomputed cell by cell from its definition; the count of cells that differ */
static int impai_test_brute(const IMPAI_VIEW* v, const IMPAI_MAPS* m)
{
    int bad = 0, layer, gx, gz, i;
    for (layer = 0; layer < IMPAI_THREATS; layer++)
        for (gz = 0; gz < m->gh; gz++)
            for (gx = 0; gx < m->gw; gx++) {
                uint64_t sum = 0;
                for (i = 1; i < v->nrows; i++) {
                    const IMPAI_SEEN* r = &v->rows[i].seat[v->seat];
                    const IMPAI_THREAT* t;
                    long long dx, dz;
                    if (!r->how || r->owner == 0xFF || r->owner == v->seat || (v->allies >> r->owner & 1u) ||
                        r->type == WQ_NONE || r->type >= v->ntypes)
                        continue;
                    t = &v->threat[r->type];
                    dx = (long long)gx * IMPAI_CELL_PX + IMPAI_CELL_PX / 2 - (r->bx * 32 + 16);
                    dz = (long long)gz * IMPAI_CELL_PX + IMPAI_CELL_PX / 2 - (r->bz * 32 + 16);
                    if (dx * dx + dz * dz <= (long long)t->range * t->range)
                        sum += layer == IMPAI_THREAT_GROUND ? t->dps_ground : layer == IMPAI_THREAT_AIR ? t->dps_air : t->dps_water;
                }
                bad += (sum > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)sum) != m->threat[layer][gz * m->gw + gx];
            }
    return bad;
}

/* `wikitest impai PAK ENTRY LAYOUT`: every demo of the game entry the file ENTRY holds played through the
   replay layer, and on each game tick every seat's impai_know_tick over the core's WORLDQ with the seat's lit
   grid and contacts (the layout's sight table); every 30 ticks one line a seat, {"demo", "t", "seat", "known":
   impai_known_digest, "maps": impai_maps_digest, "rows": its non-empty rows, "clusters", "work": the period's
   largest slots, visits, cells and flood, "brute": threat cells that differ from impai_test_brute}, then
   the class-0 reach labels' digest of the stage. Exit 1 when any line's brute is not 0. */
static int impai_test(const char* path, const char* entry, const char* layout)
{
    static IMPAI_ROW rows[IMPAI_TEST_SLOTS];
    static uint8_t bits[WIKI_SEATS][WQ_RADAR_BYTES(IMPAI_TEST_SLOTS)];
    static WQ_RADAR_HIT hits[WIKI_SEATS][IMPAI_TEST_SLOTS];
    static SNAP_PATCH clusters[WIKI_SEATS][IMPAI_CLUSTERS];
    static uint8_t lit[WIKI_SIGHT_GRID];
    static uint32_t threat[IMPAI_THREATS][IMPAI_GRID_MAX * IMPAI_GRID_MAX];
    static int32_t influence[IMPAI_GRID_MAX * IMPAI_GRID_MAX], diff[(IMPAI_GRID_MAX + 1) * IMPAI_GRID_MAX];
    static uint16_t presence[IMPAI_GRID_MAX * IMPAI_GRID_MAX];
    static const IMPAI_BUDGET budget = { 64, 256, 4, 2 * SNAP_PATCH_CAP };
    char err[256];
    GAMERUN g;
    SNAP_SCRATCH* scr = (SNAP_SCRATCH*)calloc(1, sizeof *scr);
    IMPAI_THREAT* tt = NULL;
    int j, k, rc = 0;
    if (!scr) return 2;
    if (!run_open(&g, path, entry, layout, "-", "-", 1, 0)) { free(scr); return 2; }
    tt = (IMPAI_THREAT*)calloc((size_t)g.e.ndef + 1, sizeof *tt);
    if (!tt) { run_close(&g); free(scr); return 2; }
    impai_test_threat(&g.e, tt, g.e.ndef + 1);
    for (j = 0; j < g.e.ndemo; j++) {
        WIKIREPLAY r;
        IMPAI_SEAT st[WIKI_SEATS];
        IMPAI_WORK most[WIKI_SEATS];
        int last = -1;
        if (!tagpu_wikireplay_init(&r, &g.e, &g.e.demo[j], WIKI_PLAY_DEMO, g.lay, &g.ctx, err, sizeof err)) {
            refused_line(g.e.id, g.e.demo[j].id, err);
            continue;
        }
        memset(rows, 0, sizeof rows);
        memset(bits, 0, sizeof bits);
        memset(clusters, 0, sizeof clusters);
        memset(st, 0, sizeof st);
        memset(most, 0, sizeof most);
        while (!r.done) {
            WIKISIM* s = tagpu_wikireplay_game(&r);
            const WIKIVIEW* v;
            uint8_t* con;
            if (!s || s->sim.tick == last) { tagpu_wikireplay_step(&r); continue; }
            last = s->sim.tick;
            v = tagpu_wikisim_view(s);
            if (v->nunit != s->sim.nunit || s->sim.nunit + 1 > IMPAI_TEST_SLOTS || !(con = (uint8_t*)malloc((size_t)v->nunit + 1))) {
                fprintf(stderr, "wikitest: impai %s: %d units past the test's slots or its view's\n", g.e.demo[j].id, s->sim.nunit);
                rc = 2;
                break;
            }
            for (k = 0; k < v->nseat && k < WIKI_SEATS; k++) {
                const IMPAI_ARRAYS a = { rows, IMPAI_TEST_SLOTS, bits[k], (int)sizeof bits[k],
                                         { hits[k], IMPAI_TEST_SLOTS, (unsigned)sizeof hits[k][0], 0 }, clusters[k] };
                WQ_CORE core;
                WORLDQ q;
                IMPAI_WORK w;
                tagpu_wikisim_sight_lit(v, g.lay ? &g.lay->sight : NULL, k, s->sim.tick, lit);
                tagpu_wikisim_contacts(v, lit, k, con);
                if (!tasim_worldq_open(&core, &q, &s->sim, &s->defs, k)) continue;
                core.lit = lit;
                core.con = con;
                if (impai_know_tick(&q, &st[k], &a, &budget, scr, &w) < 0) {
                    printf("{\"demo\":\"%s\",\"t\":%d,\"seat\":%d,\"refused\":true}\n", g.e.demo[j].id, s->sim.tick, k);
                    continue;
                }
                if (w.slots > most[k].slots) most[k].slots = w.slots;
                if (w.visits > most[k].visits) most[k].visits = w.visits;
                if (w.cells > most[k].cells) most[k].cells = w.cells;
                if (w.flood > most[k].flood) most[k].flood = w.flood;
                if (s->sim.tick % 30 == 0) {
                    IMPAI_MAPS m = { 0, 0, 0, { threat[0], threat[1], threat[2] }, influence, presence, diff };
                    IMPAI_VIEW iv = { k, rows, s->sim.nunit + 1, clusters[k], st[k].nclusters, 0, tt, g.e.ndef + 1, s->sim.W, s->sim.H };
                    int i, used = 0, bad;
                    impai_maps_build(&iv, &m);
                    bad = impai_test_brute(&iv, &m);
                    for (i = 1; i < s->sim.nunit + 1; i++) used += rows[i].seat[k].how != 0;
                    printf("{\"demo\":\"%s\",\"t\":%d,\"seat\":%d,\"known\":\"%016llx\",\"maps\":\"%016llx\",\"rows\":%d,"
                           "\"clusters\":%u,\"work\":[%u,%u,%u,%u],\"brute\":%d}\n",
                           g.e.demo[j].id, s->sim.tick, k, (unsigned long long)impai_known_digest(&iv),
                           (unsigned long long)impai_maps_digest(&m), used, st[k].nclusters, most[k].slots,
                           most[k].visits, most[k].cells, most[k].flood, bad);
                    if (bad) rc = 1;
                    memset(&most[k], 0, sizeof most[k]);
                }
            }
            free(con);
            if (rc == 2) break;
            tagpu_wikireplay_step(&r);
        }
        if (rc != 2) {
            WIKISIM* s = tagpu_wikireplay_game(&r);
            WQ_CORE core;
            WORLDQ q;
            if (s && tasim_worldq_open(&core, &q, &s->sim, &s->defs, 0)) {
                static uint16_t labels[(WIKI_MAP_MAX / 2) * (WIKI_MAP_MAX / 2)];
                const size_t nl = (size_t)(s->sim.W / 2) * (size_t)(s->sim.H / 2);
                void* work = malloc(impai_reach_work(s->sim.W, s->sim.H));
                uint64_t h = 0xCBF29CE484222325ull;
                size_t i;
                int comps = work ? impai_reach_label(impai_test_pass, &q, 0, s->sim.W, s->sim.H, labels, work) : -1;
                for (i = 0; i < nl; i++) h = (h ^ (labels[i] & 0xFF)) * 0x100000001B3ull, h = (h ^ (labels[i] >> 8)) * 0x100000001B3ull;
                printf("{\"demo\":\"%s\",\"reach\":\"%016llx\",\"components\":%d}\n", g.e.demo[j].id, (unsigned long long)h, comps);
                free(work);
            }
        }
        tagpu_wikireplay_free(&r);
        if (rc == 2) break;
    }
    free(tt);
    free(scr);
    run_close(&g);
    return rc;
}

int main(int argc, char** argv)
{
    int rc;
    cob_init(&g_cob);
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
    if (argc >= 2 && !strcmp(argv[1], "serve")) return serve(argc, argv);
    if (argc == 3 && !strcmp(argv[1], "trace")) {
        FILE* f = fopen(argv[2], "rb");
        if (!f) { fprintf(stderr, "wikitest: cannot read %s\n", argv[2]); return 2; }
        rc = wikihost_serve(f, NULL, NULL);
        fclose(f);
        return rc;
    }
    if (argc == 2 && !strcmp(argv[1], "constants")) return constants();
    if (argc == 3 && !strcmp(argv[1], "rules")) return rules(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "malformed")) return malformed();
    if (argc == 4 && !strcmp(argv[1], "sightcost")) return sightcost(argv[2], atoi(argv[3]));
    if (argc == 4 && !strcmp(argv[1], "worldq")) return worldq(argv[2], argv[3]);
    if (argc == 5 && !strcmp(argv[1], "impai")) return impai_test(argv[2], argv[3], argv[4]);
    fprintf(stderr, "usage: wikitest replay PAK ENTRY LAYOUT CASTING RIVAL IFACE COB"
            " | wikitest replaytrace PAK ENTRY LAYOUT CASTING RIVAL IFACE COB TRACE"
            " | wikitest serve [--instance ID] [--packs-root DIR] | wikitest trace FILE"
            " | wikitest constants | wikitest rules FILE"
            " | wikitest malformed | wikitest sightcost PAK N | wikitest worldq PAK ENTRY | wikitest impai PAK ENTRY LAYOUT\n");
    return 2;
}
