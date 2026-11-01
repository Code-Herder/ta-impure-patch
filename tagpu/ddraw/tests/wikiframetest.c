/* wikiframetest.c -- the C half of the game frame's check, for tools/wiki/framecheck.mjs: the real
   tagpu_wikiframe.c, tagpu_wikireplay.c, tagpu_wikisim.c, tagpu_hudlay.c and tagpu_huddraw.c,
   compiled natively, answer what frame.js answers, and framecheck.mjs compares the two.

   `wikiframetest frames PAK ENTRY LAYOUT CASTING RIVAL IFACE HUDDIR [TRY]` plays every demo of the
   game entry the file ENTRY holds through the replay layer, its viewer's script played, in the
   context the arguments give (simparity.mjs's: the layout LAYOUT of PAK or "-", the castings in the
   files CASTING and RIVAL or "-", the Interface Type), and with TRY Try it yourself on its first
   demo with the pointer's events the file TRY holds (below). One JSON line a chosen demo tick (THE
   TICKS below): {"e", "d", "t", "rec": the frame record from tagpu_wikiframe_record, before the
   bound, in frame.js record()'s names; "st": the HUD state tagpu_wikiframe_hudstate makes of the
   bounded record (`dump_state`); "px", "rest", "no", "game": the stage HUD's unit and the rest
   point, the question's No and seat 0's cursor (tagpu_wikiframe_game_point, null when the record
   is not live) in units, for the frame fitted into FIT_W x FIT_H device pixels at FIT_U a unit;
   "ends": the track's inset; "boxes": the controls the cursor is resolved among, laid out by
   tagpu_hudlay_build from that state or, for a return glide, from tagpu_wikiframe_pressed's, [key,
   x, y, w, h] in units; "pt": tagpu_wikiframe_point's point; "chart": 1 when a build emitted
   HOP_CHART; "unresolved": a cursor key no box holds, or ""}. A demo the replay
   refuses is one line {"e", "d", "refused"}.
   THE TICKS: tick 0, every 15th, the last, and each tick on which the viewer's clock stands at a
   trip's start, at its press (HUD_DOWN on) or at its release; in Try it, every tick.
   TRY is {"end": N, "events": [[tick, op, src, key, right, T], ...]}: after a tick's line, each of
   its events goes to the press machine (op "down", "drag" or "up"; src 1 the HUD, 2 the
   question), and each press it gives to tagpu_wikireplay_press; the record's held control is the
   machine's.

   `wikiframetest readstates HUDDIR FILE`: each line of FILE a {"st": a frame.js hudState, "px"},
   read by tools/hudart/hudstatejson.c as laycheck.c reads a state (fork_max the layout's, as
   laycheck.c takes it), and dumped as `frames` dumps C's own.

   `wikiframetest idle LAYOUT...`: each file a sidebar layout; for each of its sides one line
   [screen, h, state...] of tagpu_wikiframe_idle_side, or null.

   `wikiframetest glide FILE`: each line "a b i n"; tagpu_wikiframe_glide's answer, %.17g.

   `wikiframetest press FILE`: FILE a JSON list of sequences, each a list of [op, src, key, right,
   T]; one line a sequence, the press machine's answer to each event: [control, value] or null.

   `wikiframetest colours DIR`: a side pack's art (wikisideart.h); tagpu_wikiframe_seat_colour of
   every colour index from -1 to two past its badge frames, one line, as "#rrggbb".

   `wikiframetest selftest`: tagpu_wikiframe_bound on hostile records, the stage HUD built from each,
   and tagpu_huddraw_partition on synthetic batch lists; one line a case, "ok" or "FAILED", and
   exit 1 on a failure.

   Exit 0 when everything loaded and ran; 2 when an input cannot be read. The native build runs
   under AddressSanitizer, so a read past an array fails the run as well. */
#define STB_TRUETYPE_IMPLEMENTATION
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_json.h"
#include "tagpu_huddraw.h"
#include "tagpu_hudlay.h"
#include "tagpu_wikidata.h"
#include "tagpu_wikiframe.h"
#include "tagpu_wikireplay.h"
#include "tagpu_wikisim.h"
#include "tagpu_wikistage.h"
#include "hudstatejson.h"
#include "wikisideart.h"

/* the stage the frame is fitted into: stagecheck.mjs's 1920 x 1080 stage */
#define FIT_W 1045.0
#define FIT_H 532.0
#define FIT_U 1.5

static char* slurp_path(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    char* p;
    long len;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || !(p = (char*)malloc((size_t)len + 1)) || fread(p, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    p[len] = 0;
    *n = (size_t)len;
    return p;
}

static void str(const char* s)
{
    putchar('"');
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

/* a float as JSON: a number, or null where it is none (JSON has no NaN) */
static void fnum(double v)
{
    if (v == v && v != HUGE_VAL && v != -HUGE_VAL) printf("%.9g", v);
    else fputs("null", stdout);
}

/* ---- the HUD's layout, from art/hud ---- */

static int png_size(void* ctx, const char* part, int* w, int* h)
{
    char path[1024];
    unsigned char b[24];
    FILE* f;
    snprintf(path, sizeof path, "%s/parts/%s.png", (const char*)ctx, part);
    if (!(f = fopen(path, "rb"))) return 0;
    if (fread(b, 1, 24, f) != 24 || memcmp(b + 12, "IHDR", 4)) { fclose(f); return 0; }
    fclose(f);
    *w = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19];
    *h = (b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23];
    return 1;
}

static int s_forkMax;

static HUDLAY* hud_load(const char* dir)
{
    char path[1024], err[256];
    size_t nl, nk;
    char *lt, *kt;
    HUDLAY* l;
    TJDOC* ld;
    snprintf(path, sizeof path, "%s/layout.json", dir);
    lt = slurp_path(path, &nl);
    snprintf(path, sizeof path, "%s/kit.json", dir);
    kt = slurp_path(path, &nk);
    if (!lt || !kt) { fprintf(stderr, "wikiframetest: cannot read %s's layout.json and kit.json\n", dir); free(lt); free(kt); return NULL; }
    /* no text measure: a text's box is 0 wide. Both points are taken from these same boxes, so
       the comparison does not depend on them; wikilaycheck.py pairs the measured ones with hud.js */
    if (!(l = tagpu_hudlay_load(lt, nl, kt, nk, png_size, (void*)dir, err, sizeof err)))
        fprintf(stderr, "wikiframetest: %s\n", err);
    /* the mockup's fork limit, as laycheck.c reads it */
    ld = tagpu_json_parse(lt, nl, err, sizeof err);
    s_forkMax = ld ? (int)tj_num(tj_get(tj_get(tj_get(tagpu_json_root(ld), "proposed"), "forks"), "max"), 0) : 0;
    tagpu_json_free(ld);
    free(lt);
    free(kt);
    return l;
}

/* ---- the dumps ---- */

static void dump_rec(const WIKIFRAMEREC* f)
{
    int k;
    printf("{\"on\":%d,\"playing\":%d,\"question\":%d,\"nseat\":%d,\"side\":%d,\"colour\":%d,\"speed\":%d,\"try_it\":%d,"
           "\"playhead\":%d,\"end\":%d,\"seat\":[", f->on, f->playing, f->question, f->nseat, f->side, f->colour, f->speed,
           f->try_it, (int)f->playhead, (int)f->end);
    for (k = 0; k < f->nseat && k < WIKI_SEATS; k++) {
        const WIKIFRAMESEAT* z = &f->seat[k];
        printf("%s{\"name\":", k ? "," : "");
        str(z->name);
        printf(",\"colour\":%d,\"lost\":%d,\"out\":%d,\"units\":", z->colour, z->lost, z->out);
        fnum(z->units);
#define F_(n_) fputs(",\"" #n_ "\":", stdout); fnum(z->n_)
        F_(m); F_(mmax); F_(mi); F_(mo); F_(e); F_(emax); F_(ei); F_(eo);
#undef F_
        putchar('}');
    }
    printf("],\"nmark\":%d,\"mark\":[", f->nmark);
    for (k = 0; k < f->nmark && k < WIKIFRAME_MARKS; k++)
        printf("%s{\"t\":%d,\"kind\":%d,\"seat\":%d}", k ? "," : "", (int)f->mark[k].t, f->mark[k].kind, f->mark[k].seat);
    fputs("],\"held\":", stdout);
    str(f->held);
    fputs(",\"toast\":", stdout);
    str(f->toast);
    printf(",\"cur\":{\"on\":%d,\"from\":", f->cur.on);
    str(f->cur.from);
    fputs(",\"to\":", stdout);
    str(f->cur.to);
    printf(",\"from_v\":%d,\"to_v\":%d,\"i\":%d,\"n\":%d}", (int)f->cur.from_v, (int)f->cur.to_v, f->cur.i, f->cur.n);
    printf(",\"live\":%d,\"rw\":%d,\"arm\":%d,\"first\":%d,\"G\":%d,\"now\":%d,\"last\":%d,\"armR\":%d,\"bar\":{", f->live, f->rw,
           f->arm, (int)f->first, (int)f->G, (int)f->now, (int)f->last, (int)f->arm_R);
#define B_(n_, c_) fputs(c_ "\"" #n_ "\":", stdout); fnum(f->bar.n_)
    B_(e, ""); B_(m, ","); B_(ei, ","); B_(eo, ","); B_(mi, ","); B_(mo, ","); B_(emax, ","); B_(mmax, ","); B_(raw_e, ",");
    B_(raw_m, ",");
#undef B_
    printf("},\"simcur\":%d,\"sim\":{\"x\":", f->simcur);
    fnum(f->sim.x);
    fputs(",\"z\":", stdout);
    fnum(f->sim.z);
    printf(",\"shape\":%d,\"age\":%d}}", f->sim.shape, (int)f->sim.age);
}

static void dump_state(const HUDSTATE* st)
{
    int k, j;
    fputs("{\"W\":", stdout); fnum(st->W);
#define F_(n_) fputs(",\"" #n_ "\":", stdout); fnum(st->n_)
#define I_(n_) printf(",\"" #n_ "\":%d", (int)st->n_)
    F_(H); F_(panel); F_(top); F_(bottom); F_(t); F_(T); I_(playing); F_(speed); F_(hud); I_(nseat);
    fputs(",\"seat\":[", stdout);
    for (k = 0; k < st->nseat && k < HUD_MAXSEAT; k++) {
        const HUDSEAT* q = &st->seat[k];
        printf("%s{\"name\":", k ? "," : "");
        str(q->name);
        printf(",\"colour\":\"%06x\",\"lost\":%d,\"out\":%d,\"units\":", (unsigned)q->colour, q->lost, q->out);
        fnum(q->units);
#define S_(n_) fputs(",\"" #n_ "\":", stdout); fnum(q->n_)
        S_(m); S_(mmax); S_(mi); S_(mo); S_(e); S_(emax); S_(ei); S_(eo);
#undef S_
        putchar('}');
    }
    fputs("]", stdout);
    I_(nteam);
    fputs(",\"teams\":[", stdout);
    for (k = 0; k < st->nteam && k < HUD_MAXSEAT; k++) {
        printf("%s[", k ? "," : "");
        for (j = 0; j < st->team_n[k] && j < HUD_MAXSEAT; j++) printf("%s%d", j ? "," : "", st->team_seat[k][j]);
        putchar(']');
    }
    fputs("]", stdout);
    printf(",\"folded\":%u,\"toggles\":%u", st->folded, st->toggles);
    I_(nmark);
    fputs(",\"mark\":[", stdout);
    for (k = 0; k < st->nmark && k < HUD_MAXMARK; k++) {
        printf("%s[", k ? "," : "");
        fnum(st->mark[k].t);
        printf(",%d,%d]", st->mark[k].kind, st->mark[k].seat);
    }
    fputs("]", stdout);
    F_(mx); F_(my); F_(px);
    fputs(",\"held\":", stdout); str(st->held);
    I_(sight); I_(follow); I_(scroll);
    fputs(",\"toast\":", stdout); str(st->toast);
    printf(",\"stats\":{\"open\":%d,\"measure\":", st->stats.open);
    str(st->stats.measure);
    printf(",\"team\":%d,\"npt\":%d,\"ngroup\":%d}", st->stats.team, st->stats.npt, st->stats.ngroup);
    F_(ends); F_(len); F_(fill_to); I_(nfork); I_(fork_max); I_(timeline); I_(lanes_open); I_(picker); I_(pick_down);
    I_(replace); I_(armed); I_(live); I_(live_open); I_(rw_down); I_(rw_arm);
#undef F_
#undef I_
    putchar('}');
}

/* ---- frames ---- */

/* A GAME ENTRY'S RUN: the file ENTRY's entry, the archive PAK, its layout LAYOUT, the castings in the
   files CASTING and RIVAL ("-" for none) and the context they make with IFACE (simparity.mjs's) */
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

static int casting_file(const char* path, WIKICASTING* k)
{
    char err[256];
    size_t n;
    char* text = slurp_path(path, &n);
    TJDOC* doc = text ? tagpu_json_parse(text, n, err, sizeof err) : NULL;
    int ok = doc && tagpu_wikidata_casting(tagpu_json_root(doc), k, err, sizeof err);
    if (!ok) fprintf(stderr, "wikiframetest: %s: %s\n", path, text ? err : "cannot read it");
    tagpu_json_free(doc);
    free(text);
    return ok;
}

static int run_open(GAMERUN* g, char** a)
{
    char err[256], file[96];
    size_t ne;
    memset(g, 0, sizeof *g);
    g->ctx.side = 0;
    g->ctx.cast = strcmp(a[3], "-") ? &g->k : NULL;
    g->ctx.rival = strcmp(a[4], "-") ? &g->rk : NULL;
    g->ctx.iface = atoi(a[5]);
    if ((g->ctx.cast && !casting_file(a[3], &g->k)) || (g->ctx.rival && !casting_file(a[4], &g->rk))) return 0;
    if (!(g->text = slurp_path(a[1], &ne))) { fprintf(stderr, "wikiframetest: cannot read %s\n", a[1]); return 0; }
    if (!(g->loaded = tagpu_wikidata_entry(&g->e, g->text, ne, err, sizeof err))) {
        fprintf(stderr, "wikiframetest: %s\n", err);
        run_close(g);
        return 0;
    }
    if (!(g->pak = slurp_path(a[0], &g->npak))) { fprintf(stderr, "wikiframetest: cannot read %s\n", a[0]); run_close(g); return 0; }
    snprintf(file, sizeof file, "side/%s.json", a[2]);
    if (strcmp(a[2], "-") && !(g->lay = tagpu_wikidata_layout_pak((const unsigned char*)g->pak, g->npak, file, err, sizeof err))) {
        fprintf(stderr, "wikiframetest: %s\n", err);
        run_close(g);
        return 0;
    }
    return 1;
}

static int s_bad;   /* a check of selftest failed */

/* the HOP_HOT box of key `k` in `o`, or NULL */
static const HUDOP* hot_of(const HUDOPS* o, const char* k)
{
    const HUDOP* b = NULL;
    int i;
    for (i = 0; i < o->n; i++)
        if (o->op[i].kind == HOP_HOT && !strcmp(o->text + o->op[i].run, k)) b = &o->op[i];
    return b;
}

/* one line: the record, its HUD state, the HUD's controls laid out from it and the cursor's point */
static void emit(const HUDLAY* hud, const WIKIREPLAY* r, const char* e, const char* d, int try_it, const char* held,
                 const WIKILAYOUT* lay, HUDOPS* ops)
{
    static HUDSTATE st, pst;
    char err[256];
    WIKIREPLAYVIEW rv;
    WIKIFRAMEREC rec, b, pr;
    WIKISTAGESIDE sd;
    WIKISTAGEFIT fit;
    WIKIFRAMEDLG dlg;
    const WIKIVIEW* gv = r->game ? tagpu_wikisim_view(r->game) : NULL;
    const double ends = tagpu_hudlay_globals(hud)->track_ends;
    double fx, fy, rx, ry, nx, ny, gx, gy, x, y;
    float px;
    int Wg, Hg, i, chart = 0, side_h, nbox = 0;
    const char* unresolved = "";
    tagpu_wikireplay_view(r, &rv);
    tagpu_wikiframe_record(&rv, gv, 0, try_it, held, &rec);
    printf("{\"e\":");
    str(e);
    fputs(",\"d\":", stdout);
    str(d);
    printf(",\"t\":%d,\"rec\":", rv.tick);
    dump_rec(&rec);
    b = rec;
    tagpu_wikiframe_bound(&b, lay ? lay->nside : 0, 0);
    side_h = tagpu_wikiframe_idle_side(lay, 0, &sd) ? sd.h : 0;
    tagpu_wikiframe_size(gv ? gv->w : 1, gv ? gv->h : 1, side_h, &Wg, &Hg);
    tagpu_wikistage_fit(gv ? gv->w : 1, gv ? gv->h : 1, side_h, 1, FIT_W, FIT_H, FIT_U, &fit);
    px = (float)(Hg * fit.s / WIKIFRAME_HUD_H);
    fx = floor(fit.ox + fit.x0 * fit.s + 0.5 + 1.0 / 64);
    fy = floor(fit.oz + fit.z0 * fit.s + 0.5 + 1.0 / 64);
    rx = (fit.ox + (gv ? gv->w : 1) * WIKI_CELL / 2.0 * fit.s - fx) / px;
    ry = (fit.oz + (gv ? gv->h : 1) * WIKI_CELL / 2.0 * fit.s - fy) / px;
    tagpu_wikiframe_dialog(&fit, Wg, Hg, &dlg);
    nx = (dlg.no.x + dlg.no.w / 2.0 - fx) / px;
    ny = (dlg.no.y + dlg.no.h / 2.0 - fy) / px;
    tagpu_wikiframe_hudstate(&b, Wg, Hg, px, NULL, &st);
    fputs(",\"st\":", stdout);
    dump_state(&st);
    if (!tagpu_hudlay_build(hud, &st, ops, err, sizeof err)) {
        fprintf(stderr, "wikiframetest: the HUD's build failed: %s\n", err);
        exit(2);
    }
    for (i = 0; i < ops->n; i++) chart |= ops->op[i].kind == HOP_CHART;
    /* the cursor's controls are the HUD's the trip pressed: a return glide's, the strip as it stood */
    if (tagpu_wikiframe_pressed(&b, &pr)) {
        tagpu_wikiframe_hudstate(&pr, Wg, Hg, px, NULL, &pst);
        if (!tagpu_hudlay_build(hud, &pst, ops, err, sizeof err)) {
            fprintf(stderr, "wikiframetest: the pressed HUD's build failed: %s\n", err);
            exit(2);
        }
        for (i = 0; i < ops->n; i++) chart |= ops->op[i].kind == HOP_CHART;
    }
    tagpu_wikiframe_game_point(&b, &fit, fx, fy, px, &gx, &gy);
    tagpu_wikiframe_point(&b, ops, px, ends, rx, ry, nx, ny, gx, gy, &x, &y);
    fputs(",\"px\":", stdout);
    fnum(px);
    printf(",\"rest\":[%.17g,%.17g],\"no\":[%.17g,%.17g]", rx, ry, nx, ny);
    if (gx == gx) printf(",\"game\":[%.17g,%.17g]", gx, gy);
    else fputs(",\"game\":null", stdout);
    printf(",\"ends\":%.9g,\"boxes\":[", ends);
    for (i = 0; i < ops->n; i++) {
        const HUDOP* p = &ops->op[i];
        if (p->kind != HOP_HOT) continue;
        printf("%s[", nbox++ ? "," : "");
        str(ops->text + p->run);
        printf(",%.9g,%.9g,%.9g,%.9g]", p->r.x, p->r.y, p->r.w, p->r.h);
    }
    if (b.cur.on) {
        if (b.cur.from[0] && strcmp(b.cur.from, "leave:no") && strcmp(b.cur.from, WIKIFRAME_AT_GAME) && !hot_of(ops, b.cur.from))
            unresolved = b.cur.from;
        if (b.cur.to[0] && strcmp(b.cur.to, "leave:no") && strcmp(b.cur.to, WIKIFRAME_AT_GAME) && !hot_of(ops, b.cur.to))
            unresolved = b.cur.to;
    }
    printf("],\"pt\":[%.17g,%.17g],\"chart\":%d,\"unresolved\":", x, y, chart);
    str(unresolved);
    fputs("}\n", stdout);
}

/* THE TICKS (the header) */
static int chosen(const WIKIREPLAYVIEW* v)
{
    int rel;
    if (v->tick == 0 || v->done || v->tick % 15 == 0) return 1;
    if (!v->has_cursor) return 0;
    rel = v->drag >= 0 ? v->at + WIKI_REPLAY_HUD_DOWN + v->over : v->at + WIKI_REPLAY_HUD_UP;
    return v->clock == v->at || v->clock == v->at + WIKI_REPLAY_HUD_DOWN || v->clock == rel;
}

static int frames(char** a, int ntry)
{
    static HUDOPS ops;
    char err[256];
    GAMERUN g;
    HUDLAY* hud;
    int j;
    if (!(hud = hud_load(a[6]))) return 2;
    if (!run_open(&g, a)) return 2;
    for (j = 0; j < g.e.ndemo; j++) {
        WIKIREPLAY r;
        WIKIREPLAYVIEW v;
        if (!tagpu_wikireplay_init(&r, &g.e, &g.e.demo[j], WIKI_PLAY_DEMO, g.lay, &g.ctx, err, sizeof err)) {
            printf("{\"e\":");
            str(g.e.id);
            fputs(",\"d\":", stdout);
            str(g.e.demo[j].id);
            fputs(",\"refused\":", stdout);
            str(err);
            fputs("}\n", stdout);
            continue;
        }
        for (;;) {
            tagpu_wikireplay_view(&r, &v);
            if (chosen(&v)) emit(hud, &r, g.e.id, g.e.demo[j].id, 0, NULL, g.lay, &ops);
            if (r.done) break;
            tagpu_wikireplay_step(&r);
        }
        tagpu_wikireplay_free(&r);
    }
    if (ntry && g.e.ndemo) {
        size_t n;
        char* text = slurp_path(a[7], &n);
        TJDOC* doc = text ? tagpu_json_parse(text, n, err, sizeof err) : NULL;
        const TJV* T = doc ? tagpu_json_root(doc) : NULL;
        const TJV* ev = tj_get(T, "events");
        const int end = (int)tj_num(tj_get(T, "end"), 0);
        WIKIREPLAY r;
        WIKIFRAMEPRESS m;
        char name[96];
        if (!doc) { fprintf(stderr, "wikiframetest: %s: %s\n", a[7], text ? err : "cannot read it"); return 2; }
        snprintf(name, sizeof name, "%s/try", g.e.demo[0].id);
        tagpu_wikiframe_press_init(&m);
        if (tagpu_wikireplay_init(&r, &g.e, &g.e.demo[0], WIKI_PLAY_TRY, g.lay, &g.ctx, err, sizeof err)) {
            for (;;) {
                const TJV* x;
                emit(hud, &r, g.e.id, name, 1, m.held, g.lay, &ops);
                if (r.done || r.tick >= end) break;
                for (x = ev ? ev->first : NULL; x; x = x->next) {
                    const char* op = tj_str(tj_at(x, 1), "");
                    const char* key = tj_str(tj_at(x, 3), "");
                    const int T_ = (int)tj_num(tj_at(x, 5), 0);
                    int c = -1, val = 0, got;
                    if ((int)tj_num(tj_at(x, 0), -1) != r.tick) continue;
                    got = !strcmp(op, "down") ? tagpu_wikiframe_press_down(&m, (int)tj_num(tj_at(x, 2), 1), key,
                                                                            tj_is(tj_at(x, 4), TJ_TRUE), T_, &c, &val)
                        : !strcmp(op, "drag") ? tagpu_wikiframe_press_drag(&m, T_, &c, &val)
                        : tagpu_wikiframe_press_up(&m, key, T_, &c, &val);
                    if (got) tagpu_wikireplay_press(&r, c, val);
                }
                tagpu_wikireplay_step(&r);
            }
            tagpu_wikireplay_free(&r);
        }
        tagpu_json_free(doc);
        free(text);
    }
    run_close(&g);
    tagpu_hudlay_free(hud);
    return 0;
}

/* ---- the tables ---- */

static int readstates(char** a)
{
    static HUDSTATE st;
    size_t n;
    HUDLAY* hud = hud_load(a[0]);
    char* text = slurp_path(a[1], &n);
    char *line, *next;
    if (!hud || !text) { fprintf(stderr, "wikiframetest: cannot read %s\n", a[1]); return 2; }
    for (line = text; *line; line = next) {
        char err[256];
        TJDOC* doc;
        for (next = line; *next && *next != '\n'; next++) {}
        if (*next) *next++ = 0;
        if (!*line) continue;
        if (!(doc = tagpu_json_parse(line, strlen(line), err, sizeof err))) { fprintf(stderr, "wikiframetest: %s\n", err); return 2; }
        hudstate_json(tj_get(tagpu_json_root(doc), "st"), &st);
        st.px = (float)tj_num(tj_get(tagpu_json_root(doc), "px"), 1);
        st.fork_max = s_forkMax;
        dump_state(&st);
        putchar('\n');
        tagpu_json_free(doc);
    }
    free(text);
    tagpu_hudlay_free(hud);
    return 0;
}

static int idle(int n, char** a)
{
    int i, k, j;
    for (i = 0; i < n; i++) {
        char err[256];
        size_t len;
        char* text = slurp_path(a[i], &len);
        WIKILAYOUT* L = text ? tagpu_wikidata_layout(text, len, err, sizeof err) : NULL;
        if (!L) { fprintf(stderr, "wikiframetest: %s: %s\n", a[i], text ? err : "cannot read it"); return 2; }
        putchar('[');
        for (k = 0; k <= L->nside; k++) {
            WIKISTAGESIDE s;
            printf("%s", k ? "," : "");
            if (!tagpu_wikiframe_idle_side(L, k, &s)) { fputs("null", stdout); continue; }
            printf("[%d,%d", s.screen, s.h);
            for (j = 0; j < s.nstate; j++) printf(",%d", s.state[j]);
            putchar(']');
        }
        puts("]");
        free(L);
        free(text);
    }
    return 0;
}

static int glide(const char* path)
{
    FILE* f = fopen(path, "r");
    double x, y;
    int i, n;
    if (!f) { fprintf(stderr, "wikiframetest: cannot read %s\n", path); return 2; }
    while (fscanf(f, "%lf %lf %d %d", &x, &y, &i, &n) == 4) printf("%.17g\n", tagpu_wikiframe_glide(x, y, i, n));
    fclose(f);
    return 0;
}

static int press(const char* path)
{
    char err[256];
    size_t n;
    char* text = slurp_path(path, &n);
    TJDOC* doc = text ? tagpu_json_parse(text, n, err, sizeof err) : NULL;
    const TJV *seq, *ev;
    if (!doc) { fprintf(stderr, "wikiframetest: %s: %s\n", path, text ? err : "cannot read it"); return 2; }
    for (seq = tagpu_json_root(doc)->first; seq; seq = seq->next) {
        WIKIFRAMEPRESS m;
        int first = 1;
        tagpu_wikiframe_press_init(&m);
        putchar('[');
        for (ev = seq->first; ev; ev = ev->next) {
            const char* op = tj_str(tj_at(ev, 0), "");
            const char* key = tj_is(tj_at(ev, 2), TJ_STR) ? tj_str(tj_at(ev, 2), "") : NULL;
            const int T = (int)tj_num(tj_at(ev, 4), 0);
            int c = -1, v = 0, got;
            got = !strcmp(op, "down") ? tagpu_wikiframe_press_down(&m, (int)tj_num(tj_at(ev, 1), 1), key, tj_is(tj_at(ev, 3), TJ_TRUE), T, &c, &v)
                : !strcmp(op, "drag") ? tagpu_wikiframe_press_drag(&m, T, &c, &v)
                : tagpu_wikiframe_press_up(&m, key, T, &c, &v);
            printf("%s", first ? "" : ",");
            first = 0;
            if (got) {
                putchar('[');
                str(tagpu_wikiframe_key(c));
                printf(",%d]", v);
            } else
                fputs("null", stdout);
        }
        puts("]");
    }
    tagpu_json_free(doc);
    free(text);
    return 0;
}

static int colours(const char* dir)
{
    unsigned char* sheet = NULL;
    WIKISIDE* sd = load(dir, &sheet);
    int k;
    if (!sd) return 2;
    putchar('[');
    for (k = -1; k <= sd->nlogo + 2; k++) printf("%s\"#%06x\"", k > -1 ? "," : "", (unsigned)tagpu_wikiframe_seat_colour(sd, k));
    puts("]");
    free(sd);
    free(sheet);
    return 0;
}

/* ---- selftest ---- */

static void said(int ok, const char* what)
{
    printf("%s %s\n", ok ? "ok" : "FAILED", what);
    if (!ok) s_bad = 1;
}

/* a hostile record bounded, then drawn: the HUD built from it must stay inside its arrays */
static void hostile(const HUDLAY* hud, HUDOPS* ops, WIKIFRAMEREC* f, const char* what, int nside, int nlogo, int (*ok)(const WIKIFRAMEREC*))
{
    static HUDSTATE st;
    char err[256];
    double x, y;
    tagpu_wikiframe_bound(f, nside, nlogo);
    tagpu_wikiframe_hudstate(f, 768, 544, 1.5f, NULL, &st);
    said(ok(f) && tagpu_hudlay_build(hud, &st, ops, err, sizeof err), what);
    tagpu_wikiframe_point(f, ops, 1.5f, 6, 10, 10, NAN, NAN, NAN, NAN, &x, &y);
    said(x == x && y == y, "  and its point is a number");
}

static int ok_counts(const WIKIFRAMEREC* f) { return f->nseat == WIKI_SEATS && f->nmark <= WIKIFRAME_MARKS; }
static int ok_strings(const WIKIFRAMEREC* f)
{
    int k;
    for (k = 0; k < WIKI_SEATS; k++)
        if (!memchr(f->seat[k].name, 0, sizeof f->seat[k].name)) return 0;
    return !f->held[0] && memchr(f->toast, 0, sizeof f->toast) && !f->cur.on && !f->cur.from[0] && !f->cur.to[0];
}
static int ok_keys(const WIKIFRAMEREC* f) { return !f->held[0] && !f->cur.on; }
static int ok_floats(const WIKIFRAMEREC* f)
{
    int k;
    for (k = 0; k < WIKI_SEATS; k++) {
        const float* v = &f->seat[k].units;
        int j;
        for (j = 0; j < 9; j++)
            if (!(v[j] == v[j]) || fabsf(v[j]) > (float)WIKI_ECON_MAX) return 0;
    }
    return 1;
}
static int ok_ticks(const WIKIFRAMEREC* f)
{
    int k;
    for (k = 0; k < f->nmark; k++)
        if (f->mark[k].t < 0 || f->mark[k].t > f->end || f->mark[k].kind > 2 || f->mark[k].seat < -1 || f->mark[k].seat >= f->nseat) return 0;
    return f->end == WIKI_GAME_END_MAX && f->playhead == f->end && f->nmark == 1 && f->mark[0].t == 0;
}
static int ok_indices(const WIKIFRAMEREC* f)
{
    return f->speed == WIKI_REPLAY_SPEED0 && f->side == -1 && f->colour == 0xFF && f->seat[0].colour == 0xFF &&
           f->seat[1].colour == 3 && !f->cur.on;
}
static int ok_glide(const WIKIFRAMEREC* f) { return !f->cur.on; }
static int ok_live(const WIKIFRAMEREC* f)
{
    const float* b = &f->bar.e;
    int k;
    for (k = 0; k < 10; k++)
        if (!(b[k] == b[k]) || fabsf(b[k]) > (float)WIKI_ECON_MAX) return 0;
    return f->live == 1 && f->rw == WIKI_RW_IDLE && f->arm == WIKI_ARM_NONE && f->first == 0 && f->G == WIKI_GAME_END_MAX &&
           f->now == 0 && f->last == WIKI_GAME_END_MAX && f->arm_R == 0 && f->sim.x == -WIKIFRAME_SIDE_W && f->sim.z == -WIKIFRAME_BAR &&
           f->sim.age == 0 && f->sim.shape == WIKI_CURSOR_ARROW && !f->simcur && f->cur.on && !strcmp(f->cur.from, WIKIFRAME_AT_GAME);
}
static int ok_not_live(const WIKIFRAMEREC* f) { return !f->live && !f->simcur && !f->cur.on; }

static void base(WIKIFRAMEREC* f)
{
    memset(f, 0, sizeof *f);
    f->on = 1;
    f->nseat = 2;
    f->speed = 10;
    f->end = 900;
    f->playhead = 100;
    f->cur.on = 1;
    f->cur.n = 20;
    f->cur.i = 5;
    snprintf(f->cur.from, sizeof f->cur.from, "play");
    snprintf(f->cur.to, sizeof f->cur.to, "track");
    f->cur.to_v = 450;
}

/* the partition: batch k's `first` is its place before, so the order after names every batch */
static void partition_cases(void)
{
    HUDBATCH b[64], keep[64];
    HUDLAYREG g[8];
    /* seven lists from batch 3 on (3 batches of the replay HUD's before them): 0 two main and two
       groups, 1 one main, 2 none, 3 one main and one group, 4 no main and one group, 5 two main, 6 one
       main and two groups */
    const int nmain[7] = { 2, 1, 0, 1, 0, 2, 1 }, nbatch[7] = { 4, 1, 0, 2, 1, 2, 3 };
    int at[7], i, k, n = 3, r, ok;
    static const int want[16] = { 0, 1, 2, 3, 4, 7, 8, 11, 12, 13, 5, 6, 9, 10, 14, 15 };
    static const int g0[7] = { 5, 6, 9, 10, 14, 15, 1 };
    for (k = 0; k < 7; k++) { at[k] = n; n += nbatch[k]; }
    for (i = 0; i < n; i++) { b[i].tex = i; b[i].first = i; b[i].count = 1; }
    /* a region a group: list 0's two, list 3's, list 4's, list 6's two, and one of the replay HUD's */
    for (i = 0; i < 7; i++) { g[i].b0 = g0[i]; g[i].nb = 1; }
    memcpy(keep, b, sizeof b);
    r = tagpu_huddraw_partition(b, n, at, nmain, nbatch, 7, g, 7);
    ok = r == 3 + 7 && n == 16;
    for (i = 0; i < n && ok; i++) ok = b[i].first == want[i];
    for (i = 0; i < 7 && ok; i++) ok = b[g[i].b0].first == g0[i];
    said(ok, "partition: the main batches first in list order, then the groups', every region following its batch");
    memcpy(b, keep, sizeof b);
    at[3] = at[3] + 1;
    r = tagpu_huddraw_partition(b, n, at, nmain, nbatch, 7, g, 7);
    said(r == -1 && !memcmp(b, keep, sizeof b), "partition: lists that do not lie one after another are refused, untouched");
    at[3] = at[3] - 1;
    r = tagpu_huddraw_partition(b, 12, at, nmain, nbatch, 7, g, 7);
    said(r == -1, "partition: lists past the batches are refused");
    {
        const int m2[1] = { 3 }, n2[1] = { 2 }, a2[1] = { 0 };
        said(tagpu_huddraw_partition(b, n, a2, m2, n2, 1, g, 0) == -1, "partition: more main batches than batches is refused");
    }
    said(tagpu_huddraw_partition(b, HUD_PARTITION_MAX + 1, at, nmain, nbatch, 7, g, 0) == -1, "partition: past its bound is refused");
}

static int selftest(const char* huddir)
{
    static HUDOPS ops;
    HUDLAY* hud = hud_load(huddir);
    WIKIFRAMEREC f;
    if (!hud) return 2;
    base(&f);
    f.nseat = 200;
    f.nmark = 60000;
    hostile(hud, &ops, &f, "bound: every count past its cap", 1, 0, ok_counts);
    base(&f);
    memset(f.seat, 'x', sizeof f.seat);
    memset(f.held, 'x', sizeof f.held);
    memset(f.toast, 'x', sizeof f.toast);
    memset(f.cur.from, 'x', sizeof f.cur.from);
    memset(f.cur.to, 'x', sizeof f.cur.to);
    hostile(hud, &ops, &f, "bound: every string unterminated", 1, 0, ok_strings);
    base(&f);
    snprintf(f.held, sizeof f.held, "uiscale");
    snprintf(f.cur.to, sizeof f.cur.to, "dock");
    hostile(hud, &ops, &f, "bound: keys that are none of the replay's controls", 1, 0, ok_keys);
    base(&f);
    f.seat[0].m = NAN; f.seat[0].e = HUGE_VALF; f.seat[1].mi = -HUGE_VALF; f.seat[1].units = 3e9f; f.seat[2].eo = -NAN;
    hostile(hud, &ops, &f, "bound: NaN and infinite floats", 1, 0, ok_floats);
    base(&f);
    f.end = 99999;
    f.playhead = 123456;
    f.nmark = 4;
    f.mark[0].t = -100; f.mark[0].kind = HUD_MK_DEATH; f.mark[0].seat = 0;
    f.mark[1].t = 10; f.mark[1].kind = 7; f.mark[1].seat = 0;
    f.mark[2].t = 10; f.mark[2].kind = HUD_MK_DEFEAT; f.mark[2].seat = 9;
    f.mark[3].t = 10; f.mark[3].kind = HUD_MK_DEATH; f.mark[3].seat = -2;
    hostile(hud, &ops, &f, "bound: the end, the playhead and the marks", 1, 0, ok_ticks);
    base(&f);
    f.speed = 13;
    f.side = 99;
    f.colour = 40;
    f.seat[0].colour = 50;
    f.seat[1].colour = 3;
    f.cur.i = 30;
    hostile(hud, &ops, &f, "bound: the speed, the side, the colours and a glide step past its n", 2, 32, ok_indices);
    base(&f);
    f.cur.n = 0;
    hostile(hud, &ops, &f, "bound: a glide of no steps", 1, 0, ok_glide);
    base(&f);
    f.live = 7;
    f.rw = 9;
    f.arm = 200;
    f.simcur = 3;
    f.first = -5;
    f.G = 1 << 30;
    f.now = -1;
    f.last = 99999;
    f.arm_R = -7;
    f.bar.e = NAN; f.bar.m = HUGE_VALF; f.bar.ei = -HUGE_VALF; f.bar.mo = 3e9f; f.bar.raw_m = -NAN;
    f.sim.x = -1e9f; f.sim.z = NAN; f.sim.age = -40; f.sim.shape = 77;
    snprintf(f.cur.from, sizeof f.cur.from, WIKIFRAME_AT_GAME);
    hostile(hud, &ops, &f, "bound: a live record's flags, enums, ticks, bar and cursor", 1, 0, ok_live);
    base(&f);
    f.simcur = 1;
    snprintf(f.cur.from, sizeof f.cur.from, WIKIFRAME_AT_GAME);
    hostile(hud, &ops, &f, "bound: @game and the simulation's cursor in a record that is not live", 1, 0, ok_not_live);
    partition_cases();
    tagpu_hudlay_free(hud);
    return s_bad;
}

int main(int argc, char** argv)
{
    if ((argc == 9 || argc == 10) && !strcmp(argv[1], "frames")) return frames(argv + 2, argc == 10);
    if (argc == 4 && !strcmp(argv[1], "readstates")) return readstates(argv + 2);
    if (argc >= 3 && !strcmp(argv[1], "idle")) return idle(argc - 2, argv + 2);
    if (argc == 3 && !strcmp(argv[1], "glide")) return glide(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "press")) return press(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "colours")) return colours(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "selftest")) return selftest(argv[2]);
    fprintf(stderr, "usage: wikiframetest frames PAK ENTRY LAYOUT CASTING RIVAL IFACE HUDDIR [TRY]"
                    " | readstates HUDDIR FILE | idle LAYOUT... | glide FILE | press FILE | colours DIR | selftest HUDDIR\n");
    return 2;
}
