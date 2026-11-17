/* tasim_cobfire -- THE SCRIPTED RELOAD (tasim_cobfire.h): a weapon slot's interval between shots as its unit's own
   COB script times it, under AutoAim's aim and fire protocol. */
#include "tasim_cobfire.h"
#include "tagpu_cobvm.h"
#include <string.h>

#define AIM_CB 1u               /* the aim start's completion token, the slot's +0x8 (the vtable 0x4FD6F0) */
#define LOG_CAP 8u              /* effects the host is not handed: none, every write is committed and dropped */

typedef struct PROBE {
    int aimed;                  /* the slot's +0x8: the aim script returned non-zero */
} PROBE;

/* the slot's +0x8 through the start's callback (0x481490: `if (value) *(this + 4) = 1`), a refused start's 0 none */
static void probe_callback(void* ctx, uint32_t cb, int32_t value)
{
    if (cb == AIM_CB && value) ((PROBE*)ctx)->aimed = 1;
}

/* a write outside the unit -- an effect, an explosion, a value set -- changes nothing the interval reads */
static void probe_commit(void* ctx, const TAGPU_COBVM_EFFECT* e)
{
    (void)ctx;
    (void)e;
}

static uint32_t pieces_of(const TagpuCobProgram* p)
{
    const uint32_t n = p->pieces > p->model_pieces ? p->pieces : p->model_pieces;
    return n ? n : 1u;
}

static size_t up4(size_t n) { return (n + 3u) & ~(size_t)3u; }

size_t tasim_cobfire_work(const TagpuCobProgram* program)
{
    const uint32_t n = program ? pieces_of(program) : 1u;
    if (!program) return 0;
    return up4(TAGPU_COB_OBJECT_BYTES) + up4((size_t)(program->statics ? program->statics : 1u) * 4u) +
           up4((size_t)n * TAGPU_COBVM_ANIM * 4u) + up4(TAGPU_COBVM_MODEL_BYTES(n)) + LOG_CAP * sizeof(TAGPU_COBVM_EFFECT);
}

uint32_t tasim_cobfire_interval(const TagpuCobProgram* program, unsigned slot, uint32_t reload, uint32_t max_reload,
                                void* work, size_t bytes)
{
    static const char* const AIM[TASIM_FIRE_SLOTS] = { "AimPrimary", "AimSecondary", "AimTertiary" };
    static const char* const FIRE[TASIM_FIRE_SLOTS] = { "FirePrimary", "FireSecondary", "FireTertiary" };
    const int32_t none[4] = { 0, 0, 0, 0 };
    unsigned char* w = (unsigned char*)work;
    TAGPU_COBVM_HOST host;
    TAGPU_COBVM_RUN run;
    TAGPU_COBVM vm;
    PROBE probe;
    int32_t args[4] = { 0, 0, 0, 0 };
    uint32_t n, t, count = 0, shots = 0, last = 0, gap = 0;
    int aim, fire, create, setmax, pending = 0;
    char why[64];
    size_t at = 0;
    if (!program || slot >= TASIM_FIRE_SLOTS || !work || bytes < tasim_cobfire_work(program)) return reload;
    if ((aim = tagpu_cobvm_index(program, AIM[slot])) < 0) return reload;
    fire = tagpu_cobvm_index(program, FIRE[slot]);
    create = tagpu_cobvm_index(program, "Create");
    setmax = tagpu_cobvm_index(program, "SetMaxReloadTime");
    n = pieces_of(program);
    memset(work, 0, tasim_cobfire_work(program));
    memset(&host, 0, sizeof host);
    memset(&run, 0, sizeof run);
    memset(&probe, 0, sizeof probe);
    host.ctx = &probe;
    host.callback = probe_callback;
    host.commit = probe_commit;
    run.host = &host;
    {
        unsigned char* object = w + at;
        int32_t* statics = (int32_t*)(void*)(w + (at += up4(TAGPU_COB_OBJECT_BYTES)));
        int32_t* anim = (int32_t*)(void*)(w + (at += up4((size_t)(program->statics ? program->statics : 1u) * 4u)));
        unsigned char* model = w + (at += up4((size_t)n * TAGPU_COBVM_ANIM * 4u));
        at += up4(TAGPU_COBVM_MODEL_BYTES(n));
        run.log = (TAGPU_COBVM_EFFECT*)(void*)(w + at);
        run.cap = LOG_CAP;
        if (!tagpu_cobvm_init(&vm, program, TAGPU_COBVM_RETAIL, object, statics, anim, model, why, sizeof why))
            return reload;
    }
    if (create >= 0 && tagpu_cobvm_start(&vm, &run, create, 0, 1, 0, 0, none) < 0) return reload;
    if (setmax >= 0) {
        args[0] = (int32_t)((max_reload * 1000u) / 30u);
        if (tagpu_cobvm_start(&vm, &run, setmax, 0, 0, 1, 1, args) < 0) return reload;
        args[0] = 0;
    }
    for (t = 1; t <= TASIM_FIRE_SPAN; t++) {
        run.tick = t;
        if (count) count--;
        if (!pending) {
            probe.aimed = 0;
            if (tagpu_cobvm_start(&vm, &run, aim, AIM_CB, 0, 1, 2, args) < 0) return reload;
            pending = 1;
        }
        if (!count && probe.aimed) {
            if (fire >= 0 && tagpu_cobvm_start(&vm, &run, fire, 0, 0, 0, 0, none) < 0) return reload;
            count = reload;
            pending = 0;
            probe.aimed = 0;
            if (++shots == 3) {
                gap = t - last;
                break;
            }
            last = t;
        }
        if (tagpu_cobvm_tick(&vm, &run, 1) < 0) return reload;
    }
    return shots == 3 && gap > reload ? gap : reload;
}
