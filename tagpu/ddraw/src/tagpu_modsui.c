#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dd.h"
#include "config.h"
#include "mouse.h"
#include "tagpu_modsui.h"
#include "tagpu_modcat.h"
#include "tagpu_modinst.h"
#include "tagpu_modload.h"
#include "tagpu_profiles.h"
#include "tagpu_pak.h"
#include "tagpu_log.h"
#include "tagpu_settings.h"
#include "tagpu_shield.h"
#include "tagpu_restore_guard.h"
#include "tagpu_wiki.h"
#include "tagpu_wikistate.h"
#include "tagpu_modfit.h"
#include "tagpu_hudstate.h"
#include "tagpu_hudart.h"

#define MAX_MOD      (MODS_MAXROW - 1)    /* the first row of Manage is the folder */
#define FOLDER       (-1)                 /* a row's mod: this folder's own */
#define NONE         (-2)
#define STALE_MAX    8
#define BASE_LEVER   "tagpu_modbase.on"
#define WHEEL_UNITS  24.0f
#define PAGE_UNITS   120.0f
#define DUMP_ASK     "tagpu_modsui.dump"
#define DUMP_OUT     "tagpu_modsui.json"

typedef struct {
    char     id[32];
    MODABOUT about;
    char     installed[32];     /* what mods\<id>\ holds, "" nothing */
    uint8_t  ready;             /* an install finished this session: at the next start it is the live folder */
    char     failed[200];       /* its last install's reason */
    int        has_mut;         /* a mutator whose entry reads: `mut` below */
    MODMUTATOR mut;
    int        base;            /* a mutator's base: FOLDER, a mod's index, NONE no such mod */
    uint8_t    on;              /* mutator.<id> */
    uint8_t*   off;             /* file.<path> has it off: its files, then its hides */
    int        nstale;          /* -1 until the fit has answered */
    char       stale[STALE_MAX][MODCAT_PATH];
} ENTRY;

/* ---- what crosses --------------------------------------------------------------------------- */
static CRITICAL_SECTION s_lock;
static volatile LONG    s_open;
static MODSPAGE         s_pub;
static unsigned         s_pubSeq;
static TAGPU_CMD_WIKI   s_map;

/* ---- the game thread's ------------------------------------------------------------------------ */
static MODSPAGE s_page;
static TAGPU_CMD_WIKI s_hit;
static ENTRY    s_mod[MAX_MOD];
static int      s_nmod;
static const char* s_catalog;
static size_t   s_ncatalog;
static char     s_folder[64];
static int      s_tab, s_sel[MODS_NTAB];        /* the mod selected on each tab: FOLDER, an index, or NONE */
static int      s_chosen;                       /* Manage's radio */
static int      s_running;                      /* the mod this process runs */
static int      s_rowMod[MODS_MAXROW];
static int      s_hideChanged;                  /* the running mod's archives differ from what it mounted */
static char     s_held[MODS_KEY];
static unsigned s_heldGen;                      /* the page's generation at the press of s_held */
static unsigned s_shapeHash, s_shapeGen = 1;
static unsigned s_owed;
static uint8_t  s_keyOwed[256];
static int      s_drag, s_dragY0;
static float    s_dragS0;
static int      s_active = NONE;                /* the mod the worker has */
static int      s_queue[MAX_MOD], s_nqueue;
static MODINST_STATUS s_status;
static char     s_note[200];
static int      s_paneStale;

/* the archives of this folder a mod can mount: written by the scan's thread, read after s_scanDone */
#define SCAN_MAX MODS_MAXSW
static char s_arc[SCAN_MAX][32];
static int  s_narc;
static volatile LONG s_scanStarted, s_scanDone;

/* ---- the archives ------------------------------------------------------------------------------- */

/* HASHING OFF THE GAME THREAD: only an original archive mounts under a mod, and the retail archives are a few hundred
   megabytes. The array is the scan's until s_scanDone says otherwise, and never written again. */
static DWORD WINAPI scan_thread(void* arg)
{
    static const char* const PATTERN[] = { "*.ccx", "*.ufo", "*.hpi" };
    unsigned k;
    (void)arg;
    for (k = 0; k < sizeof PATTERN / sizeof *PATTERN; k++) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(PATTERN[k], &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (s_narc >= SCAN_MAX) break;
            if (strlen(fd.cFileName) >= sizeof s_arc[0]) continue;
            if (tagpu_modload_archive_is_original(fd.cFileName) > 0)
                lstrcpynA(s_arc[s_narc++], fd.cFileName, sizeof s_arc[0]);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    InterlockedExchange(&s_scanDone, 1);
    return 0;
}

static void scan_start(void)
{
    HANDLE t;
    if (InterlockedExchange(&s_scanStarted, 1)) return;
    t = CreateThread(NULL, 0, scan_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else InterlockedExchange(&s_scanDone, 1);
}

static void take_map(void);

/* ---- the fit -------------------------------------------------------------------------------------

   THE FILES OF A BASE ARE READ AND HASHED OFF THE GAME THREAD. The thread starts only once the scan has ended, so s_arc holds still, and works on what it
   has by value: the entries' parsed `mut`, which load_mods sets once, and `want`, taken when it starts. It answers in s_fitOut, which the game thread
   reads once s_fitState says done. The answer is what the screen says; the mount checks again at the next start (mount_mutators). */
static volatile LONG s_fitState;        /* 0 idle, 1 running, 2 an answer waits */
static int           s_fitDirty;
static struct { uint8_t want; int nstale; char stale[STALE_MAX][MODCAT_PATH]; } s_fitOut[MAX_MOD];

static DWORD WINAPI fit_thread(void* arg)
{
    static char folder[MODFIT_ARCS][MODFIT_NAME], ours[MODFIT_ARCS][MODFIT_NAME];
    const int nfolder = tagpu_modfit_folder(folder, MODFIT_ARCS);
    int m, i;
    (void)arg;
    for (i = 0; i < s_narc && i < MODFIT_ARCS; i++) lstrcpynA(ours[i], s_arc[i], MODFIT_NAME);
    for (m = 0; m < MAX_MOD; m++) {
        const ENTRY* e = &s_mod[m];
        MODFIT* f;
        if (!s_fitOut[m].want) continue;
        f = !strcmp(e->mut.base, "none") ? tagpu_modfit_open(e->mut.base, (const char (*)[MODFIT_NAME])folder, nfolder)
                                         : tagpu_modfit_open(e->mut.base, (const char (*)[MODFIT_NAME])ours, i);
        s_fitOut[m].nstale = f ? tagpu_modcat_stale(&e->mut, tagpu_modfit_hash, f, s_fitOut[m].stale, STALE_MAX) : 0;
        tagpu_modfit_close(f);
    }
    InterlockedExchange(&s_fitState, 2);
    return 0;
}

static void fit_start(void)
{
    HANDLE t;
    int m, any = 0;
    s_fitDirty = 0;
    for (m = 0; m < s_nmod; m++) {
        const ENTRY* e = &s_mod[m];
        /* a base not installed has no file to compare; it is not listed either */
        const int ask = e->has_mut && (e->base == FOLDER || (e->base >= 0 && s_mod[e->base].installed[0]));
        s_fitOut[m].want = (uint8_t)ask;
        s_fitOut[m].nstale = 0;
        any |= ask;
    }
    if (!any) {
        for (m = 0; m < s_nmod; m++) s_mod[m].nstale = 0;
        return;
    }
    InterlockedExchange(&s_fitState, 1);
    t = CreateThread(NULL, 0, fit_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else InterlockedExchange(&s_fitState, 2);    /* no verdict to give: the mount still checks */
}

static void fit_take(void)
{
    int m;
    for (m = 0; m < s_nmod; m++) {
        s_mod[m].nstale = s_fitOut[m].nstale;
        memcpy(s_mod[m].stale, s_fitOut[m].stale, sizeof s_mod[m].stale);
    }
    InterlockedExchange(&s_fitState, 0);
    s_paneStale = 1;
}

/* ---- what the player has switched --------------------------------------------------------------- */

static const char* base_id_of(const ENTRY* e)
{
    return e->mut.base;
}

static void load_switches(void)
{
    int m, i;
    for (m = 0; m < s_nmod; m++) {
        ENTRY* e = &s_mod[m];
        if (!e->has_mut) continue;
        e->on = tagpu_profiles_mutator_of(base_id_of(e), e->id) == 1;
        for (i = 0; i < e->mut.nfiles; i++) e->off[i] = tagpu_profiles_file_of(base_id_of(e), e->mut.files[i].path) == 0;
        for (i = 0; i < e->mut.nhides; i++) e->off[e->mut.nfiles + i] = tagpu_profiles_file_of(base_id_of(e), e->mut.hides[i]) == 0;
    }
}

/* MODCAT_OFF over the entries: a key names a path of one base, so any mutator of that base that holds the path says. ctx: the base's index. */
static int path_off(void* ctx, const char* path)
{
    const int base = (int)(intptr_t)ctx;
    int m, i;
    for (m = 0; m < s_nmod; m++) {
        const ENTRY* e = &s_mod[m];
        if (!e->has_mut || e->base != base) continue;
        for (i = 0; i < e->mut.nfiles; i++) if (tagpu_modcat_same_path(e->mut.files[i].path, path)) return e->off[i];
        for (i = 0; i < e->mut.nhides; i++) if (tagpu_modcat_same_path(e->mut.hides[i], path)) return e->off[e->mut.nfiles + i];
    }
    return 0;
}

static const char* stale_path(int m)
{
    ENTRY* e = &s_mod[m];
    return e->nstale > 0 ? tagpu_modcat_stale_live(e->stale, e->nstale, STALE_MAX, path_off, (void*)(intptr_t)e->base) : NULL;
}

enum { FIT_UNKNOWN, FIT_OK, FIT_STALE, FIT_CLASH };

static const char* mod_name(int m);

/* What a mutator is now, as the mount will find it: an earlier switched-on one that fits keeps its files, a later one gives way, and
   one not on gives way to any. `why`, a sentence for the pane. */
static int verdict(int m, char* why, size_t cap)
{
    const ENTRY* e = &s_mod[m];
    const char* p;
    int k;
    if (e->nstale < 0) return FIT_UNKNOWN;
    if ((p = stale_path(m)) != NULL) {
        snprintf(why, cap, "Needs an update for %s: %s differs from the file it was built on.", mod_name(e->base), p);
        return FIT_STALE;
    }
    for (k = 0; k < s_nmod; k++) {
        const ENTRY* o = &s_mod[k];
        if (k == m || !o->has_mut || o->base != e->base || !o->on || !o->installed[0] || (e->on && k > m) || stale_path(k)) continue;
        if ((p = tagpu_modcat_clash(&e->mut, &o->mut, path_off, (void*)(intptr_t)e->base)) != NULL) {
            snprintf(why, cap, "%s is also %s's. Only one of them can be on.", p, o->about.name);
            return FIT_CLASH;
        }
    }
    return FIT_OK;
}

/* ---- the page -------------------------------------------------------------------------------- */

static void text(char* out, size_t cap, const char* fmt, const char* a, const char* b)
{
    snprintf(out, cap, fmt, a, b);
}

static const char* mod_name(int m)
{
    if (m == FOLDER) return "This folder";
    return m >= 0 && m < s_nmod ? s_mod[m].about.name : "";
}

static int is_mutator(int m)
{
    return m >= 0 && m < s_nmod && s_mod[m].about.is_mutator;
}

static int has_update(const ENTRY* e)
{
    return e->installed[0] && strcmp(e->installed, e->about.version) && !e->ready;
}

static int is_active_install(int m)
{
    return m == s_active && m != NONE;
}

static int busy(void)
{
    return s_active != NONE || s_nqueue > 0;
}

/* the bar of a running install, 0..1000, -1 when its phase has no total; the word for its phase */
static const char* install_words(int* progress)
{
    *progress = s_status.total ? (int)((double)s_status.done / s_status.total * 1000.0) : -1;
    if (*progress > 1000) *progress = 1000;
    switch (s_status.phase) {
    case MODINST_DOWNLOADING: return "Downloading";
    case MODINST_CHECKING:    return "Checking the download";
    case MODINST_UNPACKING:   return "Unpacking";
    case MODINST_RECIPE:      return "Building from your archives";
    default:                  return "Waiting";
    }
}

static void row_badge(MODSROW* r, int badge, const char* word)
{
    r->badge = (uint8_t)badge;
    lstrcpynA(r->badge_text, word, sizeof r->badge_text);
}

static MODSROW* add_row(int mod, const char* name, const char* detail)
{
    MODSROW* r;
    if (s_page.nrow >= MODS_MAXROW) return NULL;
    s_rowMod[s_page.nrow] = mod;
    r = &s_page.row[s_page.nrow++];
    memset(r, 0, sizeof *r);
    lstrcpynA(r->name, name, sizeof r->name);
    lstrcpynA(r->detail, detail, sizeof r->detail);
    r->progress = -1;
    return r;
}

/* a running install's bar and word over a row's detail */
static void row_progress(MODSROW* r, int m)
{
    if (r && is_active_install(m)) {
        int p;
        const char* w = install_words(&p);
        r->act = MODS_ACT_BUSY;
        r->progress = (int16_t)p;
        lstrcpynA(r->detail, w, sizeof r->detail);
    }
}

/* A mutator on Manage: its check, unless it cannot go on. One already on keeps its check, or a player could not turn it off. */
static void mutator_row(int m)
{
    const ENTRY* e = &s_mod[m];
    char detail[sizeof s_page.row[0].detail], why[200];
    MODSROW* r;
    int v;
    if (!e->has_mut || !(e->installed[0] || e->ready)) return;
    v = verdict(m, why, sizeof why);
    if (v == FIT_UNKNOWN) lstrcpynA(detail, "Checking its files", sizeof detail);
    else text(detail, sizeof detail, "Mutator, version %s", e->ready ? e->about.version : e->installed, "");
    if (!(r = add_row(m, e->about.name, detail))) return;
    r->act = e->on ? MODS_ACT_CHECK_ON : v == FIT_OK ? MODS_ACT_CHECK : MODS_ACT_NONE;
    if (v == FIT_STALE) row_badge(r, MODS_BADGE_GREY, "Stale");
    else if (v == FIT_CLASH) row_badge(r, MODS_BADGE_CONFLICT, "Conflict");
    row_progress(r, m);
}

static void mutator_rows_of(int base)
{
    int m;
    for (m = 0; m < s_nmod; m++) if (s_mod[m].has_mut && s_mod[m].base == base) mutator_row(m);
}

static void build_rows(void)
{
    int m;
    char detail[sizeof s_page.row[0].detail], why[200];
    s_page.nrow = 0;
    s_page.empty[0] = 0;
    if (s_tab == MODS_TAB_MANAGE) {
        MODSROW* r = add_row(FOLDER, "This folder", s_folder);
        if (r) {
            r->act = s_chosen == FOLDER ? MODS_ACT_RADIO_ON : MODS_ACT_RADIO;
            if (s_running == FOLDER) row_badge(r, MODS_BADGE_ACTIVE, "Active");
            if (s_chosen == FOLDER) mutator_rows_of(FOLDER);
        }
    }
    for (m = 0; m < s_nmod; m++) {
        const ENTRY* e = &s_mod[m];
        const int have = e->installed[0] || e->ready;
        MODSROW* r = NULL;
        if (e->about.is_mutator) continue;
        if (s_tab == MODS_TAB_MANAGE && have) {
            text(detail, sizeof detail, "Version %s", e->ready ? e->about.version : e->installed, "");
            if ((r = add_row(m, e->about.name, detail)) != NULL) {
                r->act = s_chosen == m ? MODS_ACT_RADIO_ON : MODS_ACT_RADIO;
                if (s_running == m) row_badge(r, MODS_BADGE_ACTIVE, "Active");
                else if (has_update(e)) row_badge(r, MODS_BADGE_UPDATE, "Update");
            }
            row_progress(r, m);
            if (r && s_chosen == m) mutator_rows_of(m);
            continue;
        } else if (s_tab == MODS_TAB_EXPLORE && !have) {
            text(detail, sizeof detail, "Version %s", e->about.version, "");
            if ((r = add_row(m, e->about.name, detail)) != NULL) r->act = MODS_ACT_INSTALL;
        } else if (s_tab == MODS_TAB_UPDATES && has_update(e)) {
            text(detail, sizeof detail, "%s, now %s", e->installed, e->about.version);
            if ((r = add_row(m, e->about.name, detail)) != NULL) {
                r->act = MODS_ACT_UPDATE;
                row_badge(r, MODS_BADGE_UPDATE, "Update");
            }
        }
        row_progress(r, m);
    }
    /* A mutator lists only under its base: on Explore, for the base chosen; on Updates, whichever base it has. */
    for (m = 0; m < s_nmod; m++) {
        const ENTRY* e = &s_mod[m];
        const int have = e->installed[0] || e->ready;
        MODSROW* r = NULL;
        if (!e->has_mut) continue;
        if (s_tab == MODS_TAB_EXPLORE && !have && e->base == s_chosen) {
            text(detail, sizeof detail, "For %s", mod_name(e->base), "");
            if ((r = add_row(m, e->about.name, detail)) != NULL) r->act = MODS_ACT_INSTALL;
        } else if (s_tab == MODS_TAB_UPDATES && has_update(e)) {
            text(detail, sizeof detail, "%s, now %s", e->installed, e->about.version);
            if ((r = add_row(m, e->about.name, detail)) != NULL) {
                r->act = MODS_ACT_UPDATE;
                row_badge(r, MODS_BADGE_UPDATE, "Update");
            }
        } else if (s_tab == MODS_TAB_UPDATES && have && verdict(m, why, sizeof why) == FIT_STALE) {
            text(detail, sizeof detail, "Needs an update for %s", mod_name(e->base), "");
            if ((r = add_row(m, e->about.name, detail)) != NULL) row_badge(r, MODS_BADGE_GREY, "Stale");
        }
        row_progress(r, m);
    }
    if (!s_page.nrow) {
        if (s_tab == MODS_TAB_EXPLORE) lstrcpynA(s_page.empty, "Every mod Impure knows is installed.", sizeof s_page.empty);
        if (s_tab == MODS_TAB_UPDATES) lstrcpynA(s_page.empty, "Every installed mod is up to date.", sizeof s_page.empty);
    }
}

static void add_opt(const char* label, int kind, int on, const char* value, int changed, const char* locked)
{
    MODSOPT* o;
    if (s_page.nopt >= MODS_MAXOPT) return;
    o = &s_page.opt[s_page.nopt++];
    memset(o, 0, sizeof *o);
    lstrcpynA(o->label, label, sizeof o->label);
    lstrcpynA(o->value, value, sizeof o->value);
    o->kind = (uint8_t)kind;
    o->on = (uint8_t)on;
    o->changed = (uint8_t)changed;
    if (locked) {
        o->locked = 1;
        lstrcpynA(o->reason, locked, sizeof o->reason);
    }
}

/* A mod's rules and the file its choices live in: the catalog's for a catalog mod, the identified mod's policy for what
   runs from this folder. 0 for the folder under a managed mod, which is not what the policy describes. */
static int rules_of(int mod, MODRULES* r, const char** id)
{
    memset(r, 0, sizeof *r);
    if (is_mutator(mod)) return 0;
    if (mod == FOLDER) {
        const TagpuResourcePolicy* p;
        if (s_running != FOLDER) return 0;
        p = tagpu_profiles_policy();
        r->manual = p->manual;
        r->radius = p->radius;
        r->maximum = p->maximum;
        r->quick = p->quick;
        r->quick_default = p->quick_default;
        lstrcpynA(r->manual_reason, "This mod does not allow it.", sizeof r->manual_reason);
        lstrcpynA(r->quick_reason, "This mod does not allow it.", sizeof r->quick_reason);
        *id = tagpu_profiles_name();
        return 1;
    }
    {
        char e[120];
        *id = s_mod[mod].id;
        return tagpu_modcat_rules(s_catalog, s_ncatalog, *id, r, e, sizeof e) == 1;
    }
}

static void build_options(int mod)
{
    MODRULES r;
    const char* id = NULL;
    s_page.nopt = 0;
    if (!rules_of(mod, &r, &id)) return;
    {
        const int have = tagpu_profiles_choice_of(id, TAGPU_CHOICE_RADIUS);
        int v = have >= 0 ? have : r.radius;
        char value[16];
        if (v > r.maximum) v = r.maximum;
        snprintf(value, sizeof value, "%d", r.manual ? v : 0);
        add_opt("Resource snapping radius", MODS_OPT_STEPPER, 0, value, r.manual && have >= 0 && have != r.radius,
                r.manual ? NULL : r.manual_reason);
    }
    {
        const int have = tagpu_profiles_choice_of(id, TAGPU_CHOICE_QUICK);
        const int on = r.quick && (have >= 0 ? have : r.quick_default);
        add_opt("Quick Build", MODS_OPT_SWITCH, on, on ? "On" : "Off", r.quick && have >= 0 && have != r.quick_default,
                r.quick ? NULL : r.quick_reason);
    }
}

static void build_switches(int mod)
{
    int i;
    s_page.nsw = 0;
    lstrcpynA(s_page.sw_head, "Game archives", sizeof s_page.sw_head);
    if (mod >= 0 && s_mod[mod].has_mut) {
        const ENTRY* e = &s_mod[mod];
        const int all = e->mut.nfiles + e->mut.nhides;
        lstrcpynA(s_page.sw_head, "Files", sizeof s_page.sw_head);
        for (i = 0; i < all && s_page.nsw < MODS_MAXSW; i++) {
            MODSSWITCH* w = &s_page.sw[s_page.nsw++];
            memset(w, 0, sizeof *w);
            if (i < e->mut.nfiles) lstrcpynA(w->name, e->mut.files[i].path, sizeof w->name);
            else snprintf(w->name, sizeof w->name, "Hides %s", e->mut.hides[i - e->mut.nfiles]);
            w->on = (uint8_t)!e->off[i];
            w->changed = e->off[i];
        }
        return;
    }
    if (mod < 0 || !s_scanDone) return;
    for (i = 0; i < s_narc && s_page.nsw < MODS_MAXSW; i++) {
        MODSSWITCH* w = &s_page.sw[s_page.nsw++];
        const int def = tagpu_modcat_hidden(s_catalog, s_ncatalog, s_mod[mod].id, s_arc[i]);
        const int key = tagpu_profiles_hide_of(s_mod[mod].id, s_arc[i]);
        const int locked = !lstrcmpiA(tagpu_modload_essential_archive(), s_arc[i]);
        memset(w, 0, sizeof *w);
        lstrcpynA(w->name, s_arc[i], sizeof w->name);
        w->locked = (uint8_t)locked;
        w->on = (uint8_t)(locked || !(key >= 0 ? key : def));
        w->changed = (uint8_t)(!locked && key >= 0 && key != def);
    }
}

static void build_pane(void)
{
    const int m = s_sel[s_tab];
    int i;
    s_page.selected = -1;
    for (i = 0; i < s_page.nrow; i++) if (s_rowMod[i] == m) s_page.selected = i;
    /* THE PANE READS THE PLAYER'S FILE AND PARSES THE CATALOG ONCE AN ARCHIVE, on the game thread, and an install's progress
       publishes many times a second: only what can change it rebuilds it. */
    if (!s_paneStale) return;
    s_paneStale = 0;
    s_page.title[0] = s_page.meta[0] = s_page.summary[0] = s_page.note[0] = 0;
    s_page.nopt = s_page.nsw = 0;
    if (s_page.selected < 0) {
        /* Confirm stands on every tab, so its refusal can have no row to stand under */
        lstrcpynA(s_page.note, s_note, sizeof s_page.note);
        return;
    }
    if (m == FOLDER) {
        lstrcpynA(s_page.title, "This folder", sizeof s_page.title);
        lstrcpynA(s_page.meta, s_folder, sizeof s_page.meta);
        lstrcpynA(s_page.summary, "The game as this folder holds it, with whatever you have installed in it.", sizeof s_page.summary);
    } else {
        const ENTRY* e = &s_mod[m];
        lstrcpynA(s_page.title, e->about.name, sizeof s_page.title);
        snprintf(s_page.meta, sizeof s_page.meta, "%s, version %s", e->about.author, e->about.version);
        lstrcpynA(s_page.summary, e->about.summary, sizeof s_page.summary);
        if (e->failed[0]) lstrcpynA(s_page.note, e->failed, sizeof s_page.note);
        else if (e->ready) lstrcpynA(s_page.note, "Installed. It takes effect when the game next starts.", sizeof s_page.note);
    }
    if (m >= 0 && s_mod[m].has_mut && !s_page.note[0]) {
        const ENTRY* e = &s_mod[m];
        char why[200];
        const int all = e->mut.nfiles + e->mut.nhides;
        if (verdict(m, why, sizeof why) >= FIT_STALE) lstrcpynA(s_page.note, why, sizeof s_page.note);
        else if (all > MODS_MAXSW) snprintf(s_page.note, sizeof s_page.note, "Lists the first %d of its %d files.", MODS_MAXSW, all);
    }
    if (s_note[0]) lstrcpynA(s_page.note, s_note, sizeof s_page.note);
    build_options(m);
    if (m >= 0 && (s_mod[m].installed[0] || s_mod[m].ready)) build_switches(m);
}

static void build_footer(void)
{
    s_page.confirm[0] = 0;
    s_page.update_all = 0;
    if (s_tab == MODS_TAB_UPDATES && s_page.nrow && !busy()) s_page.update_all = 1;
    if (busy()) return;
    if (s_chosen != s_running)
        snprintf(s_page.confirm, sizeof s_page.confirm, "Restart with %.32s", mod_name(s_chosen));
    else if (s_hideChanged || (s_running >= 0 && s_mod[s_running].ready))
        lstrcpynA(s_page.confirm, "Restart to apply", sizeof s_page.confirm);
}

/* THE PAGE'S SHAPE: what an index in a hit key means. A key names a row, an option or a switch by its place on the page, so it means
   what the player saw only while the page has the shape of the frame they saw. Each map carries the generation of the page it was
   drawn from; a click acts only if the page has that generation at both its press and its release. */
static void note_shape(void)
{
    unsigned h = 2166136261u;
    int i;
#define MIX(v) (h = (h ^ (unsigned)(v)) * 16777619u)
    MIX(s_tab); MIX(s_sel[s_tab]); MIX(s_page.nrow); MIX(s_page.nopt); MIX(s_page.nsw);
    for (i = 0; i < s_page.nrow; i++) MIX(s_rowMod[i]);
#undef MIX
    if (h != s_shapeHash) { s_shapeHash = h; s_shapeGen++; }
}

static void publish(void)
{
    int updates = 0, i;
    char why[200];
    for (i = 0; i < s_nmod; i++)
        if (has_update(&s_mod[i]) || (s_mod[i].has_mut && (s_mod[i].installed[0] || s_mod[i].ready) && verdict(i, why, sizeof why) == FIT_STALE)) updates++;
    s_page.tab = s_tab;
    s_page.updates = updates;
    build_rows();
    build_pane();
    build_footer();
    note_shape();
    s_page.gen = s_shapeGen;
    lstrcpynA(s_page.held, s_held, sizeof s_page.held);
    EnterCriticalSection(&s_lock);
    s_pub = s_page;
    s_pubSeq++;
    LeaveCriticalSection(&s_lock);
}

/* ---- the catalog -------------------------------------------------------------------------------- */

static void add_id(void* ctx, const char* id)
{
    MODABOUT a;
    (void)ctx;
    if (s_nmod >= MAX_MOD || !tagpu_modcat_about(s_catalog, s_ncatalog, id, &a)) return;
    memset(&s_mod[s_nmod], 0, sizeof s_mod[0]);
    lstrcpynA(s_mod[s_nmod].id, id, sizeof s_mod[0].id);
    s_mod[s_nmod].about = a;
    s_nmod++;
}

static int index_of(const char* id)
{
    int i;
    for (i = 0; id && i < s_nmod; i++) if (!strcmp(s_mod[i].id, id)) return i;
    return NONE;
}

static void load_mods(void)
{
    int i;
    if (!s_catalog) {
        s_catalog = tagpu_modinst_catalog(&s_ncatalog);
        if (s_catalog) tagpu_modcat_each_id(s_catalog, s_ncatalog, add_id, NULL);
        for (i = 0; i < s_nmod; i++) {
            ENTRY* e = &s_mod[i];
            if (!e->about.is_mutator) continue;
            e->nstale = -1;
            e->has_mut = tagpu_modcat_mutator(s_catalog, s_ncatalog, e->id, &e->mut);
            if (e->has_mut) e->off = (uint8_t*)calloc((size_t)e->mut.nfiles + (size_t)e->mut.nhides + 1, 1);
            if (e->has_mut && !e->off) { tagpu_modcat_mutator_free(&e->mut); e->has_mut = 0; }
            if (!e->has_mut) tagpu_logf("modsui: the catalog's mutator %s cannot be used", e->id);
            else e->base = !strcmp(e->mut.base, "none") ? FOLDER : index_of(e->mut.base);
        }
    }
    for (i = 0; i < s_nmod; i++)
        if (!tagpu_modinst_installed(s_mod[i].id, s_mod[i].installed, sizeof s_mod[i].installed))
            s_mod[i].installed[0] = 0;
}

/* ---- installing --------------------------------------------------------------------------------- */

/* A local server's address, for the harness: in the game's folder until the catalog names a place of its own. */
static const char* base_lever(char* out, DWORD cap)
{
    HANDLE f = CreateFileA(BASE_LEVER, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD got = 0;
    char* e;
    if (f == INVALID_HANDLE_VALUE) return NULL;
    if (!ReadFile(f, out, cap - 1, &got, NULL)) got = 0;
    CloseHandle(f);
    out[got] = 0;
    for (e = out; *e && *e != '\r' && *e != '\n' && *e != ' '; e++) {}
    *e = 0;
    return out[0] ? out : NULL;
}

static void start_next(void)
{
    while (s_active == NONE && s_nqueue > 0) {
        char base[512], why[200] = "";
        const int m = s_queue[0];
        memmove(s_queue, s_queue + 1, sizeof s_queue[0] * (size_t)--s_nqueue);
        s_mod[m].failed[0] = 0;
        if (tagpu_modinst_start(s_catalog, s_ncatalog, s_mod[m].id, base_lever(base, sizeof base), why, sizeof why)) {
            s_active = m;
            memset(&s_status, 0, sizeof s_status);
            tagpu_logf("modsui: installing %s", s_mod[m].id);
        } else {
            lstrcpynA(s_mod[m].failed, why, sizeof s_mod[m].failed);
            tagpu_logf("modsui: %s not installed: %s", s_mod[m].id, why);
        }
    }
}

static void queue_install(int m)
{
    int i;
    if (m < 0 || is_active_install(m)) return;
    for (i = 0; i < s_nqueue; i++) if (s_queue[i] == m) return;
    if (s_nqueue < MAX_MOD) s_queue[s_nqueue++] = m;
    start_next();
}

/* THE STATE, FOR THE TESTS (tools/mods/modsscreen.py): written when tagpu_modsui.dump appears, on the game thread, as the wiki's
   tagpu_wiki.dump is, with the hit map of the last frame drawn in client pixels. */
static void jstr(FILE* f, const char* s)
{
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        if ((unsigned char)*s >= 0x20) fputc(*s, f);
    }
    fputc('"', f);
}

static void dump(void)
{
    FILE* f = fopen(DUMP_OUT ".tmp", "wb");
    int i;
    unsigned h;
    if (!f) { tagpu_logf("modsui: the state dump could not open %s", DUMP_OUT ".tmp"); return; }
    if (tagpu_modsui_is_open()) take_map();
    else memset(&s_hit, 0, offsetof(TAGPU_CMD_WIKI, hit));
    fprintf(f, "{\"open\":%d,\"current\":%d,\"holds\":%ld,\"drawn\":%d,\"tab\":%d,\"updates\":%d,\"selected\":%d,\"note\":",
            tagpu_modsui_is_open(), !tagpu_modsui_is_open() || s_hit.rep_serial == s_shapeGen, (long)tagpu_wiki_holds_input(), s_hit.drawn, s_page.tab, s_page.updates, s_page.selected);
    jstr(f, s_page.note);
    fputs(",\"chosen\":", f);
    jstr(f, mod_name(s_chosen));
    fputs(",\"running\":", f);
    jstr(f, mod_name(s_running));
    fputs(",\"confirm\":", f);
    jstr(f, s_page.confirm);
    fprintf(f, ",\"mutators\":\"%s\",\"update_all\":%d,\"scan_done\":%ld,\"scroll\":[%.2f,%.2f],\"scroll_max\":[%.2f,%.2f],\"rows\":[",
            tagpu_modload_mutators(), s_page.update_all, (long)s_scanDone, s_hit.scroll[0], s_hit.scroll[1], s_hit.scroll_max[0], s_hit.scroll_max[1]);
    for (i = 0; i < s_page.nrow; i++) {
        const MODSROW* r = &s_page.row[i];
        fputs(i ? ",{\"name\":" : "{\"name\":", f);
        jstr(f, r->name);
        fputs(",\"detail\":", f);
        jstr(f, r->detail);
        fprintf(f, ",\"badge\":%d,\"act\":%d,\"progress\":%d}", r->badge, r->act, r->progress);
    }
    fputs("],\"opts\":[", f);
    for (i = 0; i < s_page.nopt; i++) {
        const MODSOPT* o = &s_page.opt[i];
        fputs(i ? ",{\"label\":" : "{\"label\":", f);
        jstr(f, o->label);
        fputs(",\"value\":", f);
        jstr(f, o->value);
        fputs(",\"reason\":", f);
        jstr(f, o->reason);
        fprintf(f, ",\"on\":%d,\"changed\":%d,\"locked\":%d}", o->on, o->changed, o->locked);
    }
    fputs("],\"sw\":[", f);
    for (i = 0; i < s_page.nsw; i++) {
        const MODSSWITCH* w = &s_page.sw[i];
        fputs(i ? ",{\"name\":" : "{\"name\":", f);
        jstr(f, w->name);
        fprintf(f, ",\"on\":%d,\"changed\":%d,\"locked\":%d}", w->on, w->changed, w->locked);
    }
    fputs("],\"hits\":[", f);
    for (h = 0; h < s_hit.n; h++) {
        const TAGPU_CMD_WIKIHIT* k = &s_hit.hit[h];
        fputs(h ? ",{\"key\":" : "{\"key\":", f);
        jstr(f, k->key);
        fprintf(f, ",\"r\":[%d,%d,%d,%d]}", k->x0, k->y0, k->x1, k->y1);
    }
    fputs("]}\n", f);
    fclose(f);
    MoveFileExA(DUMP_OUT ".tmp", DUMP_OUT, MOVEFILE_REPLACE_EXISTING);
}

static void close_screen(const char* how);

void tagpu_modsui_tick(int in_game)
{
    MODINST_STATUS st;
    static DWORD s_asked;
    const DWORD now = GetTickCount();
    if (now - s_asked >= 100) {
        s_asked = now;
        if (GetFileAttributesA(DUMP_ASK) != INVALID_FILE_ATTRIBUTES) {
            dump();
            DeleteFileA(DUMP_ASK);
        }
    }
    /* THE QUEUE GOES ON WITH THE SCREEN SHUT, in the shell: Update all then Back leaves it running. A start reads files and parses the
       catalog on this thread, so in a match the finished job is only collected and the next waits for the menu. */
    if (s_active != NONE) {
        tagpu_modinst_status(&st);
        if (st.phase == MODINST_READY || st.phase == MODINST_FAILED) {
            if (st.phase == MODINST_READY) {
                s_mod[s_active].ready = 1;
                tagpu_logf("modsui: %s is ready", s_mod[s_active].id);
            } else {
                lstrcpynA(s_mod[s_active].failed, st.why, sizeof s_mod[s_active].failed);
                tagpu_logf("modsui: %s failed: %s", s_mod[s_active].id, st.why);
            }
            s_active = NONE;
            s_paneStale = 1;
            s_fitDirty = 1;
            if (tagpu_modsui_is_open()) publish();
        } else if (st.phase != s_status.phase || st.done != s_status.done) {
            s_status = st;
            if (tagpu_modsui_is_open()) publish();
        }
    }
    if (!in_game && s_active == NONE && s_nqueue > 0) {
        start_next();
        if (tagpu_modsui_is_open()) publish();
    }
    if (!tagpu_modsui_is_open()) return;
    /* THE LAYER CAN REFUSE AFTER THE OPEN: its pass is built at its first use and the art may still have been loading, and with the
       input captured a screen nobody can see would hold the menu. It closes; the next click on MODS gets the box. */
    if (!tagpu_hudstate_on() || tagpu_wikistate_refused() ||
        (tagpu_wikiart_done() && (!tagpu_wikiart_get() || !tagpu_wikiart_get()->mods))) {
        close_screen("the layer cannot draw this session");
        return;
    }
    if (s_fitState == 2) {
        fit_take();
        publish();
    }
    if (s_fitDirty && s_fitState == 0 && s_scanDone == 1) {
        fit_start();
        if (s_fitState == 0) { s_paneStale = 1; publish(); }
    }
    if (s_scanDone == 1) {
        static int s_saw;
        if (!s_saw) { s_saw = 1; s_paneStale = 1; publish(); }
    }
}

/* ---- opening and closing --------------------------------------------------------------------------- */

void tagpu_modsui_init(void)
{
    InitializeCriticalSection(&s_lock);
}

int tagpu_modsui_is_open(void)
{
    return InterlockedCompareExchange(&s_open, 0, 0) != 0;
}

void tagpu_modsui_open(const char* folder)
{
    char mod[64] = "";
    if (tagpu_modsui_is_open()) return;
    load_mods();
    load_switches();
    s_fitDirty = 1;
    lstrcpynA(s_folder, folder, sizeof s_folder);
    s_running = index_of(tagpu_modload_id());
    if (s_running == NONE) s_running = FOLDER;
    if (!tagpu_settings_mod(mod, sizeof mod) || !mod[0]) s_chosen = FOLDER;
    else { s_chosen = index_of(mod); if (s_chosen == NONE) s_chosen = FOLDER; }
    s_tab = MODS_TAB_MANAGE;
    s_sel[MODS_TAB_MANAGE] = s_chosen;
    s_sel[MODS_TAB_EXPLORE] = s_sel[MODS_TAB_UPDATES] = NONE;
    s_held[0] = s_note[0] = 0;
    s_owed = 0;
    memset(s_keyOwed, 0, sizeof s_keyOwed);
    s_drag = -1;
    s_paneStale = 1;
    memset(&s_page, 0, sizeof s_page);
    memset(&s_hit, 0, sizeof s_hit);
    EnterCriticalSection(&s_lock);
    memset(&s_map, 0, offsetof(TAGPU_CMD_WIKI, hit));
    LeaveCriticalSection(&s_lock);
    scan_start();
    tagpu_wiki_capture(1);
    /* the page first: a frame between the two would draw the last session's */
    publish();
    InterlockedExchange(&s_open, 1);
    tagpu_logf("modsui: opened: %d mods, running %s, chosen %s", s_nmod, mod_name(s_running), mod_name(s_chosen));
}

static void close_screen(const char* how)
{
    InterlockedExchange(&s_open, 0);
    tagpu_wiki_capture(0);
    tagpu_logf("modsui: closed (%s)", how);
}

static void confirm(void)
{
    char id[32], was[32] = "";
    lstrcpynA(id, s_chosen >= 0 ? s_mod[s_chosen].id : "", sizeof id);
    /* THE PROFILE FIRST: a choice it could not save refuses the restart with impure.cfg as it was */
    tagpu_profiles_flush_now();
    if (!tagpu_profiles_saved()) {
        lstrcpynA(s_note, "Your options could not be saved, so the game stays as it is.", sizeof s_note);
        tagpu_logf("modsui: %s", s_note);
        s_paneStale = 1;
        publish();
        return;
    }
    cfg_save();
    tagpu_settings_mod(was, sizeof was);
    tagpu_settings_set_mod(id);
    tagpu_settings_flush();
    /* A RESTART MUST NOT COME UP ON THE OLD MOD WITH NO WORD SAID: it goes only once the file holds the choice. Refused, what the store
       held goes back, since its retry would otherwise write the refused one once the file frees. */
    if (!tagpu_settings_saved()) {
        tagpu_settings_set_mod(was);
        lstrcpynA(s_note, "Your choice could not be saved in impure.cfg, so the game stays as it is.", sizeof s_note);
        tagpu_logf("modsui: %s", s_note);
        s_paneStale = 1;
        publish();
        return;
    }
    tagpu_logf("modsui: restarting with mod=%s", id);
    if (!tagpu_rguard_restart_self()) {
        snprintf(s_note, sizeof s_note, "The game could not restart itself (error %lu). Your choice is saved, so start it again.",
                 (unsigned long)GetLastError());
        tagpu_logf("modsui: %s", s_note);
        s_paneStale = 1;
        publish();
        return;
    }
    /* TERMINATED, NOT EXITED, AND WHAT MATTERS IS ALREADY ON DISK. ExitProcess kills the render thread wherever it stands and then
       runs the detaches, the graphics driver's among them, and one left waiting on a lock the killed thread held never returns
       (MEASURED: Wine, NVIDIA, a game on the screen for 20 s: it stayed in its detach for minutes, and held the semaphore the new
       process waits on). The engine's own exit releases the device first, and nothing here can reach that from a click.
       Terminating runs no detach, so the kernel ends it whatever any thread holds; nothing is left to save at the main menu. */
    TerminateProcess(GetCurrentProcess(), 0);
}

/* ---- the hit map --------------------------------------------------------------------------------- */

static void take_map(void)
{
    EnterCriticalSection(&s_lock);
    memcpy(&s_hit, &s_map, offsetof(TAGPU_CMD_WIKI, hit) + sizeof s_hit.hit[0] * (s_map.n <= TAGPU_CMD_WIKIHITS ? s_map.n : 0));
    LeaveCriticalSection(&s_lock);
    if (s_hit.n > TAGPU_CMD_WIKIHITS) s_hit.n = 0;
}

static int hit_key(int cx, int cy, char* key)
{
    unsigned i;
    for (i = 0; i < s_hit.n; i++) {
        const TAGPU_CMD_WIKIHIT* h = &s_hit.hit[i];
        if (cx >= h->x0 && cx < h->x1 && cy >= h->y0 && cy < h->y1) {
            memcpy(key, h->key, TAGPU_WIKI_KEY);
            key[TAGPU_WIKI_KEY - 1] = 0;
            return 1;
        }
    }
    return 0;
}

static float scroll_limit(int which, float v)
{
    const float hi = s_hit.scroll_max[which];
    return v < 0 || !(hi > 0) ? 0 : v > hi ? hi : v;
}

static void scroll_to(int which, float v)
{
    s_page.scroll[which] = scroll_limit(which, v);
}

static void select_mod(int mod)
{
    s_sel[s_tab] = mod;
    s_note[0] = 0;
    s_page.scroll[MODS_SCROLL_DETAIL] = 0;
}

static const char* mod_id_of(int mod)
{
    if (mod >= 0) return s_mod[mod].id;
    return tagpu_profiles_name();
}

/* An option's change. BOUND: an index read out of a hit key is bounded against the page it is about. */
static void option_key(int i, char what)
{
    const int m = s_sel[s_tab];
    MODRULES r;
    const char* id = NULL;
    if (i < 0 || i >= s_page.nopt || s_page.opt[i].locked || !rules_of(m, &r, &id)) return;
    if (i == 0) {
        const int have = tagpu_profiles_choice_of(id, TAGPU_CHOICE_RADIUS);
        int v = have >= 0 ? have : r.radius;
        if (what == '-' && v > 0) v--;
        if (what == '+' && v < r.maximum) v++;
        tagpu_profiles_set_choice_of(id, TAGPU_CHOICE_RADIUS, v);
    } else {
        const int have = tagpu_profiles_choice_of(id, TAGPU_CHOICE_QUICK);
        tagpu_profiles_set_choice_of(id, TAGPU_CHOICE_QUICK, !(have >= 0 ? have : r.quick_default));
    }
}

static void option_reset(int i)
{
    const char* id = mod_id_of(s_sel[s_tab]);
    if (i >= 0 && i < TAGPU_CHOICES && i < s_page.nopt && id)
        tagpu_profiles_set_choice_of(id, i == 0 ? TAGPU_CHOICE_RADIUS : TAGPU_CHOICE_QUICK, -1);
}

static void switch_set(int i, int reset)
{
    const int m = s_sel[s_tab];
    const MODSSWITCH* w;
    if (m < 0 || i < 0 || i >= s_page.nsw || s_page.sw[i].locked) return;
    w = &s_page.sw[i];
    if (s_mod[m].has_mut) {
        ENTRY* e = &s_mod[m];
        const char* path = i < e->mut.nfiles ? e->mut.files[i].path : e->mut.hides[i - e->mut.nfiles];
        /* a file is on with no key: only off is written, and a reset is the same as on */
        if (tagpu_profiles_set_file_of(base_id_of(e), path, reset || !w->on ? -1 : 0) && e->base == s_running) s_hideChanged = 1;
        load_switches();
        return;
    }
    if (tagpu_profiles_set_hide_of(s_mod[m].id, w->name, reset ? -1 : w->on ? 1 : 0) && m == s_running)
        s_hideChanged = 1;
}

/* Acts on a control the release landed on. */
static void act(const char* key)
{
    int i = -1;
    char what = 0;
    s_paneStale = 1;
    if (!strncmp(key, "tab:", 4)) {
        i = atoi(key + 4);
        if (i >= 0 && i < MODS_NTAB) { s_tab = i; s_note[0] = 0; s_page.scroll[MODS_SCROLL_LIST] = 0; s_page.scroll[MODS_SCROLL_DETAIL] = 0; }
    } else if (!strncmp(key, "row:", 4)) {
        i = atoi(key + 4);
        if (i >= 0 && i < s_page.nrow) select_mod(s_rowMod[i]);
    } else if (!strncmp(key, "act:", 4)) {
        i = atoi(key + 4);
        if (i >= 0 && i < s_page.nrow) {
            const int m = s_rowMod[i];
            select_mod(m);
            if (s_tab != MODS_TAB_MANAGE) queue_install(m);
            else if (is_mutator(m)) {
                ENTRY* e = &s_mod[m];
                /* off is no key, like every mutator a player never touched */
                if (e->has_mut && tagpu_profiles_set_mutator_of(base_id_of(e), e->id, e->on ? -1 : 1) && e->base == s_running) s_hideChanged = 1;
                load_switches();
            } else s_chosen = m;
        }
    } else if (sscanf(key, "opt:%d:%c", &i, &what) == 2) {
        option_key(i, what);
    } else if (!strncmp(key, "reset:", 6)) {
        option_reset(atoi(key + 6));
    } else if (!strncmp(key, "swreset:", 8)) {
        switch_set(atoi(key + 8), 1);
    } else if (!strncmp(key, "sw:", 3)) {
        switch_set(atoi(key + 3), 0);
    } else if (!strcmp(key, "updateall")) {
        int m;
        for (m = 0; m < s_nmod; m++) if (has_update(&s_mod[m])) queue_install(m);
    } else if (!strcmp(key, "confirm")) {
        if (s_page.confirm[0]) confirm();
    } else if (!strcmp(key, "back") || !strcmp(key, "close")) {
        close_screen(key);
        return;
    }
    publish();
}

/* ---- the input ------------------------------------------------------------------------------------ */

/* The drawn cursor follows mouse_last_client, and no move reaches cnc-ddraw's store while the capture holds. */
static void note_client(int cx, int cy)
{
    if (cx < 0) return;
    if (mouse_client_inside(cx, cy)) mouse_note_client(cx, cy);
    else mouse_forget_client();
}

static int press(unsigned bit, int cx, int cy)
{
    char key[TAGPU_WIKI_KEY];
    int i;
    note_client(cx, cy);
    s_owed |= bit;
    if (bit != 1u) return 1;
    take_map();
    s_drag = -1;
    /* a scrollbar's thumb drags, its track pages */
    for (i = 0; i < MODS_NSCROLL; i++) {
        const TAGPU_CMD_WIKIBAR* b = &s_hit.bar[i];
        if (b->y1 > b->y0 && cx >= b->x0 && cx < b->x1 && cy >= b->y0 && cy < b->y1) {
            if (cy >= b->thumb_y && cy < b->thumb_y + b->thumb_h) {
                s_drag = i;
                s_dragY0 = cy;
                s_dragS0 = s_hit.scroll[i];
            } else {
                scroll_to(i, s_hit.scroll[i] + (cy < b->thumb_y ? -PAGE_UNITS : PAGE_UNITS));
                publish();
            }
            return 1;
        }
    }
    /* a frame not yet drawn with the page's shape: the player has not seen what is under the pointer */
    if (s_hit.rep_serial == s_shapeGen && hit_key(cx, cy, key)) {
        lstrcpynA(s_held, key, sizeof s_held);
        s_heldGen = s_shapeGen;
        publish();
    }
    return 1;
}

static int release(unsigned bit, int cx, int cy)
{
    if (!(s_owed & bit)) return 0;
    s_owed &= ~bit;
    if (bit == 1u) {
        char key[TAGPU_WIKI_KEY];
        char held[MODS_KEY];
        s_drag = -1;
        lstrcpynA(held, s_held, sizeof held);
        s_held[0] = 0;
        if (held[0]) {
            take_map();
            if (cx >= 0 && s_heldGen == s_shapeGen && s_hit.rep_serial == s_shapeGen && hit_key(cx, cy, key) && !strcmp(key, held))
                act(key);
            else publish();
        }
    }
    return 1;
}

static void pointer(int cx, int cy)
{
    note_client(cx, cy);
    if (s_drag >= 0 && s_drag < MODS_NSCROLL && s_hit.bar[s_drag].per_px > 0) {
        scroll_to(s_drag, s_dragS0 + (float)(cy - s_dragY0) * s_hit.bar[s_drag].per_px);
        publish();
    }
}

/* the panel the point lies over, by its scrollbar's column; the list when neither */
static void wheel(int cx, float notches)
{
    int which = MODS_SCROLL_LIST;
    take_map();
    if (s_hit.bar[MODS_SCROLL_DETAIL].y1 > s_hit.bar[MODS_SCROLL_DETAIL].y0 &&
        cx >= s_hit.bar[MODS_SCROLL_LIST].x1 && s_hit.bar[MODS_SCROLL_LIST].y1 > s_hit.bar[MODS_SCROLL_LIST].y0)
        which = MODS_SCROLL_DETAIL;
    else if (s_hit.bar[MODS_SCROLL_LIST].y1 <= s_hit.bar[MODS_SCROLL_LIST].y0)
        which = MODS_SCROLL_DETAIL;
    scroll_to(which, s_hit.scroll[which] + notches * WHEEL_UNITS);
    publish();
}

/* A PRESS TAKEN OWES ITS RELEASE, AND ONLY THAT ONE IS TAKEN: the release of the key that opened the screen, MODS' quick key,
   is the engine's. Esc closes on its release, so its press, repeats and character all arrive while the screen still takes them. */
int tagpu_modsui_key(unsigned vk, int up)
{
    const unsigned k = vk & 0xFF;
    if (!up) {
        s_keyOwed[k] = 1;
        return 1;
    }
    if (!s_keyOwed[k]) return 0;
    s_keyOwed[k] = 0;
    if (vk == VK_ESCAPE) close_screen("Esc");
    return 1;
}

int tagpu_modsui_message(UINT msg, WPARAM wp, LPARAM lp)
{
    const int cx = GET_X_LPARAM(lp), cy = GET_Y_LPARAM(lp);
    switch (msg) {
    /* NO ALT CHORD REACHES THE ENGINE BUT ALT+F4, WHICH wndproc.c GIVES THE SYSTEM. TA's window procedure pushes the letter of a
       WM_SYSKEYDOWN (0x4B5DC3, 0x4C1D50), and only its GUI dispatch's test of the Alt key stops a quick key firing (0x4AA219,
       DISASSEMBLED): under the capture that key reads up, so the chord would press a menu button behind the screen. */
    case WM_SYSKEYDOWN:
        return wp != VK_F4;
    case WM_SYSCHAR:
    case WM_SYSDEADCHAR:
        return 1;
    case WM_KEYDOWN:
        return tagpu_modsui_key((unsigned)wp, 0);
    case WM_KEYUP:
        return tagpu_modsui_key((unsigned)wp, 1);
    case WM_CHAR:
    case WM_DEADCHAR:
        return 1;
    case WM_MOUSEMOVE:
        pointer(cx, cy);
        return 1;
    case WM_MOUSEWHEEL: {
        POINT pt;
        /* a wheel message's point is the screen's */
        pt.x = cx; pt.y = cy;
        ScreenToClient(g_ddraw.hwnd, &pt);
        wheel(pt.x, -(float)(short)HIWORD(wp) / WHEEL_DELTA);
        return 1;
    }
    case WM_INPUT:
    case WM_MOUSEHOVER:
        return 1;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: return press(1u, cx, cy);
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: return press(2u, cx, cy);
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: return press(4u, cx, cy);
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: return press(8u, cx, cy);
    case WM_LBUTTONUP: return release(1u, cx, cy);
    case WM_RBUTTONUP: return release(2u, cx, cy);
    case WM_MBUTTONUP: return release(4u, cx, cy);
    case WM_XBUTTONUP: return release(8u, cx, cy);
    }
    return 0;
}

int tagpu_modsui_mouse(int code, int cx, int cy)
{
    switch (code) {
    case TAGPU_M_MOVE: pointer(cx, cy); return 1;
    case TAGPU_M_WHEELUP: wheel(cx, -1.0f); return 1;
    case TAGPU_M_WHEELDN: wheel(cx, 1.0f); return 1;
    case TAGPU_M_LDOWN: return press(1u, cx, cy);
    case TAGPU_M_RDOWN: return press(2u, cx, cy);
    case TAGPU_M_MDOWN: return press(4u, cx, cy);
    case TAGPU_M_LUP: return release(1u, cx, cy);
    case TAGPU_M_RUP: return release(2u, cx, cy);
    case TAGPU_M_MUP: return release(4u, cx, cy);
    }
    return 1;
}

void tagpu_modsui_cancel(void)
{
    if (!tagpu_modsui_is_open()) return;
    s_owed = 0;
    s_held[0] = 0;
    s_drag = -1;
    publish();
}

/* ---- the render thread's --------------------------------------------------------------------------- */

int tagpu_modsui_page(MODSPAGE* out, unsigned* seen)
{
    int r;
    if (!tagpu_modsui_is_open()) return -1;
    EnterCriticalSection(&s_lock);
    r = s_pubSeq != *seen;
    if (r) {
        *out = s_pub;
        *seen = s_pubSeq;
    }
    LeaveCriticalSection(&s_lock);
    return r;
}

void tagpu_modsui_drawn(const TAGPU_CMD_WIKI* map)
{
    const unsigned n = map->n <= TAGPU_CMD_WIKIHITS ? map->n : 0;
    EnterCriticalSection(&s_lock);
    memcpy(&s_map, map, offsetof(TAGPU_CMD_WIKI, hit) + sizeof map->hit[0] * n);
    s_map.n = n;
    LeaveCriticalSection(&s_lock);
}
