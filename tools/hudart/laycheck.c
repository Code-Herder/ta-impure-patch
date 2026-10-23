/* laycheck.c -- tagpu_hudlay's rectangles for a state the mockup dumped, as JSON on stdout.

     laycheck <art/hud dir> <state.json>

   The state is the mockup's own (index.html `state()`), so the two sides lay out the same
   game; tools/hudart/laycheck.py compares this output with the mockup's element boxes.
   Compiled natively by laycheck.py: tagpu_json.c and tagpu_hudlay.c are pure C99 for this. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_json.h"
#include "tagpu_hudlay.h"

static char* slurp(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    char* b;
    long len;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (char*)malloc((size_t)len + 1);
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    fclose(f);
    if (b) { b[len] = 0; *n = (size_t)len; }
    return b;
}

/* a PNG's size from its IHDR, which is always the first chunk */
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

static void json_str(const char* s, int n)
{
    int i;
    putchar('"');
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

int main(int argc, char** argv)
{
    char path[1024], err[256];
    size_t nl, nk, ns;
    char *lt, *kt, *stt;
    TJDOC* sd;
    const TJV *S, *v, *e;
    HUDLAY* l;
    HUDSTATE* st = (HUDSTATE*)calloc(1, sizeof(HUDSTATE));
    HUDOPS* o = (HUDOPS*)malloc(sizeof(HUDOPS));
    int i, j;

    if (argc != 3 && argc != 4) { fprintf(stderr, "usage: laycheck <art/hud dir> <state.json> [<px a unit>]\n"); return 2; }
    snprintf(path, sizeof path, "%s/layout.json", argv[1]);
    if (!(lt = slurp(path, &nl))) { fprintf(stderr, "laycheck: cannot read %s\n", path); return 2; }
    snprintf(path, sizeof path, "%s/kit.json", argv[1]);
    if (!(kt = slurp(path, &nk))) { fprintf(stderr, "laycheck: cannot read %s\n", path); return 2; }
    if (!(l = tagpu_hudlay_load(lt, nl, kt, nk, png_size, argv[1], err, sizeof err))) { fprintf(stderr, "laycheck: %s\n", err); return 1; }
    if (!(stt = slurp(argv[2], &ns)) || !(sd = tagpu_json_parse(stt, ns, err, sizeof err))) { fprintf(stderr, "laycheck: state: %s\n", err); return 2; }
    /* the fork limit is the mockup's, the layout's proposed.forks.max; the game's is the recorder's */
    {
        TJDOC* ld = tagpu_json_parse(lt, nl, err, sizeof err);
        st->fork_max = ld ? (int)tj_num(tj_get(tj_get(tj_get(tagpu_json_root(ld), "proposed"), "forks"), "max"), 0) : 0;
        tagpu_json_free(ld);
    }

    S = tagpu_json_root(sd);
    st->W = (float)tj_num(tj_get(S, "W"), 0);
    st->H = (float)tj_num(tj_get(S, "H"), 0);
    v = tj_get(S, "frame");
    st->panel = (float)tj_num(tj_get(v, "panel"), 0);
    st->top = (float)tj_num(tj_get(v, "top"), 0);
    st->bottom = (float)tj_num(tj_get(v, "bottom"), 0);
    st->t = (float)tj_num(tj_get(S, "t"), 0);
    st->T = (float)tj_num(tj_get(S, "T"), 0);
    st->playing = tj_is(tj_get(S, "playing"), TJ_TRUE);
    st->speed = (float)tj_num(tj_get(S, "speed"), 1);
    st->hud = (float)tj_num(tj_get(S, "hud"), 1);
    v = tj_get(S, "seats");
    for (e = v ? v->first : NULL; e && st->nseat < HUD_MAXSEAT; e = e->next) {
        HUDSEAT* q = &st->seat[st->nseat++];
        const TJV* r = tj_get(e, "res");
        snprintf(q->name, sizeof q->name, "%s", tj_str(tj_get(e, "name"), ""));
        q->colour = (uint32_t)strtoul(tj_str(tj_get(e, "colour"), "#000000") + 1, NULL, 16);
        q->units = (float)tj_num(tj_get(e, "units"), 0);
        q->lost = tj_is(tj_get(e, "lost"), TJ_TRUE);
        q->out = tj_is(tj_get(e, "out"), TJ_TRUE);
        q->m = (float)tj_num(tj_get(r, "m"), 0); q->mmax = (float)tj_num(tj_get(r, "mmax"), 0);
        q->mi = (float)tj_num(tj_get(r, "mi"), 0); q->mo = (float)tj_num(tj_get(r, "mo"), 0);
        q->e = (float)tj_num(tj_get(r, "e"), 0); q->emax = (float)tj_num(tj_get(r, "emax"), 0);
        q->ei = (float)tj_num(tj_get(r, "ei"), 0); q->eo = (float)tj_num(tj_get(r, "eo"), 0);
    }
    v = tj_get(S, "teams");
    for (e = v ? v->first : NULL; e && st->nteam < HUD_MAXSEAT; e = e->next, st->nteam++)
        for (j = 0; j < e->n && j < HUD_MAXSEAT; j++)
            st->team_seat[st->nteam][st->team_n[st->nteam]++] = (uint8_t)tj_num(tj_at(e, j), 0);
    v = tj_get(S, "folded");
    for (e = v ? v->first : NULL; e; e = e->next) st->folded |= 1u << (int)tj_num(e, 0);
    v = tj_get(S, "toggles");
    for (e = v ? v->first : NULL; e; e = e->next)
        if (tj_is(e, TJ_TRUE)) st->toggles |= tagpu_hudlay_toggle(e->key);
    v = tj_get(S, "markers");
    for (e = v ? v->first : NULL; e && st->nmark < HUD_MAXMARK; e = e->next) {
        HUDMARK* m = &st->mark[st->nmark++];
        const char* k = tj_str(tj_get(e, "kind"), "");
        m->t = (float)tj_num(tj_get(e, "t"), 0);
        m->kind = !strcmp(k, "defeat") ? HUD_MK_DEFEAT : !strcmp(k, "bookmark") ? HUD_MK_BOOKMARK : HUD_MK_DEATH;
        m->seat = (int)tj_num(tj_get(e, "seat"), -1);
    }
    st->mx = (float)tj_num(tj_get(S, "mx"), -1);
    st->my = (float)tj_num(tj_get(S, "my"), -1);
    snprintf(st->held, sizeof st->held, "%s", tj_str(tj_get(S, "held"), ""));
    st->sight = (int)tj_num(tj_get(S, "sight"), -1);
    st->follow = (int)tj_num(tj_get(S, "follow"), -1);
    st->scroll = (int)tj_num(tj_get(S, "scroll"), 0);
    snprintf(st->toast, sizeof st->toast, "%s", tj_str(tj_get(S, "toast"), ""));
    /* proposed, variation B: the mockup's names (index.html `state()`); a state without them is the
       original's alone */
    st->ends = (float)tj_num(tj_get(S, "ends"), st->T);
    st->len = (float)tj_num(tj_get(S, "len"), st->T);
    st->fill_to = (float)tj_num(tj_get(S, "fillTo"), st->T);
    st->timeline = (int)tj_num(tj_get(S, "timeline"), -1);
    st->lanes_open = tj_is(tj_get(S, "lanesOpen"), TJ_TRUE);
    st->picker = tj_is(tj_get(S, "picker"), TJ_TRUE);
    st->armed = (int)tj_num(tj_get(S, "armed"), -1);
    st->replace = (int)tj_num(tj_get(S, "replace"), -1);
    /* the Take over button is down while the picker is open, and the picker's title is the
       playhead's clock: the mockup's st.t */
    st->pick_down = st->picker;
    st->pick_t = st->t;
    v = tj_get(S, "forks");
    for (e = v ? v->first : NULL; e && st->nfork < HUD_MAXFORK; e = e->next) {
        HUDFORK* f = &st->fork[st->nfork++];
        f->id = (int)tj_num(tj_get(e, "id"), 0);
        snprintf(f->name, sizeof f->name, "%s", tj_str(tj_get(e, "name"), ""));
        f->from = (float)tj_num(tj_get(e, "from"), 0);
        f->end = (float)tj_num(tj_get(e, "end"), 0);
        f->parent = (int)tj_num(tj_get(e, "parent"), 0);
        /* the packet's flags (TAGPU_PK_FORK_*): the mockup's `chain`, and the watched one by its place */
        f->flags = (tj_is(tj_get(e, "chain"), TJ_TRUE) ? 2 : 0) | (st->nfork - 1 == st->timeline ? 1 : 0);
    }
    v = tj_get(S, "stats");
    if (v) {
        HUDSTATS* x = &st->stats;
        x->open = tj_is(tj_get(v, "open"), TJ_TRUE);
        x->x = (float)tj_num(tj_get(v, "x"), 0);
        x->y = (float)tj_num(tj_get(v, "y"), 0);
        snprintf(x->measure, sizeof x->measure, "%s", tj_str(tj_get(v, "measure"), ""));
        x->team = !strcmp(tj_str(tj_get(v, "split"), ""), "team");
        x->dt = (float)tj_num(tj_get(v, "dt"), 0);
        x->t0 = (float)tj_num(tj_get(v, "t0"), 0);
        e = tj_get(v, "groups");
        for (e = e ? e->first : NULL; e && x->ngroup < HUD_MAXSEAT; e = e->next) {
            HUDGROUP* g = &x->group[x->ngroup++];
            const TJV* d = tj_get(e, "death");
            const TJV* vs = tj_get(e, "v");
            snprintf(g->name, sizeof g->name, "%s", tj_str(tj_get(e, "name"), ""));
            g->colour = (uint32_t)strtoul(tj_str(tj_get(e, "col"), "#000000") + 1, NULL, 16);
            g->death = tj_is(d, TJ_NUM) ? (float)d->num : -1.0f;
            for (j = 0; vs && j < vs->n && j < HUD_MAXPT; j++) g->v[j] = (float)tj_num(tj_at(vs, j), 0);
            x->npt = j;
        }
    }

    if (!tagpu_hudlay_build(l, st, o, err, sizeof err)) { fprintf(stderr, "laycheck: %s\n", err); return 1; }
    printf("{\"overflow\":%d,\"ops\":[\n", o->overflow);
    for (i = 0; i < o->n; i++) {
        const HUDOP* p = &o->op[i];
        static const char* kinds[] = { "part", "solid", "dot", "text", "hot", "line", "chart" };
        const char* kind = p->kind == HOP_SOLID && (p->flags & HOPF_DROP) ? "shadow"
                         : p->kind == HOP_TEXT && (p->flags & HOPF_BASE) ? "svgtext" : kinds[p->kind];
        printf("%s{\"kind\":\"%s\",\"x\":%.4f,\"y\":%.4f,\"w\":%.4f,\"h\":%.4f,\"dim\":%d,\"edge\":%d", i ? ",\n" : "",
               kind, p->r.x, p->r.y, p->r.w, p->r.h, (p->flags & HOPF_DIM) != 0, (p->flags & HOPF_EDGE) != 0);
        if (p->flags & HOPF_DROP) printf(",\"dy\":%.4f,\"blur\":%.4f", p->size, p->shade);
        if (p->kind == HOP_LINE || p->kind == HOP_CHART) printf(",\"rgba\":\"%08x\",\"width\":%.4f", p->rgba, p->size);
        if (p->kind == HOP_CHART) printf(",\"clip\":[%.4f,%.4f,%.4f,%.4f]", p->clip.x, p->clip.y, p->clip.w, p->clip.h);
        if (p->kind == HOP_PART) {
            printf(",\"part\":"); json_str(tagpu_hudlay_part(l, p->part)->name, (int)strlen(tagpu_hudlay_part(l, p->part)->name));
            printf(",\"alpha\":%u", p->rgba & 0xFF);
        }
        if (p->kind == HOP_SOLID || p->kind == HOP_DOT) printf(",\"rgba\":\"%08x\"", p->rgba);
        if (p->clip.w >= 0 && p->kind != HOP_TEXT && p->kind != HOP_CHART) printf(",\"clip\":[%.4f,%.4f,%.4f,%.4f]", p->clip.x, p->clip.y, p->clip.w, p->clip.h);
        if (p->kind == HOP_TEXT) {
            const char* sn = tagpu_hudlay_globals(l)->style[p->style].name;
            printf(",\"style\":\"%s\",\"size\":%.4f,\"centre\":%d,\"alpha\":%u", sn, p->size, p->centre,
                   p->rgba & 0xFF);
            if (p->nrun) printf(",\"rgb\":\"%06x\"", o->run[p->run].rgb);
            /* the baseline at `px` a unit, from the face metrics the state carries per style */
            if (argc == 4) {
                const TJV* fm = tj_get(tj_get(tagpu_json_root(sd), "faces"), sn);
                float px = (float)atof(argv[3]);
                if (fm)
                    printf(",\"base\":%.4f", tagpu_hudlay_baseline(p->r.y * px, p->r.h * px, p->size * px,
                                                                  (float)tj_num(tj_at(fm, 0), 0), (float)tj_num(tj_at(fm, 1), 0)) / px);
            }
            printf(",\"text\":");
            putchar('"');
            for (j = 0; j < p->nrun; j++) {
                const HUDRUN* r = &o->run[p->run + j];
                int k;
                for (k = 0; k < r->len; k++) {
                    unsigned char ch = (unsigned char)o->text[r->off + k];
                    if (ch == '"' || ch == '\\') printf("\\%c", ch); else putchar(ch);
                }
            }
            putchar('"');
        }
        if (p->kind == HOP_HOT) { printf(",\"key\":"); json_str(o->text + p->run, (int)strlen(o->text + p->run)); }
        putchar('}');
    }
    printf("\n],\"chart\":{\"n\":%d,\"npt\":%d,\"w\":%.4f,\"halo_w\":%.4f,\"groups\":[", o->chart.n, o->chart.npt,
           o->chart.w, o->chart.halo_w);
    for (i = 0; i < o->chart.n; i++) {
        printf("%s{\"rgb\":\"%06x\",\"dark\":%d,\"cross\":", i ? "," : "", o->chart.g[i].rgb, o->chart.g[i].dark);
        if (o->chart.g[i].cross) printf("[%.4f,%.4f]", o->chart.g[i].cx, o->chart.g[i].cy);
        else printf("null");
        printf(",\"pts\":[");
        for (j = 0; j < o->chart.npt; j++) printf("%s[%.4f,%.4f]", j ? "," : "", o->chart.pt[i][j][0], o->chart.pt[i][j][1]);
        printf("]}");
    }
    printf("]}}\n");
    return 0;
}
