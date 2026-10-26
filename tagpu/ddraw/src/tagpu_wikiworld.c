/* tagpu_wikiworld.c -- the producer of the in-game wiki's 3D stage, and the inert answers the
   wiki's instances of the world passes take for the game-only modules. Contract:
   tagpu_wikiworld.h; the instancing itself is tagpu_scene.h.

   THE PROTOTYPES ARE THE INSTANCES' OWN. TAGPU_SCENE_WIKI is defined here before the headers, so
   each header below declares its function under the name tagpu_scene.h maps it to -- exactly the
   declaration the wiki's instance calls through. A definition here that disagreed with it in a
   parameter or a return type would not compile, so the two cannot drift apart.

   EVERY ANSWER IS "NOTHING", and each is one the instance already takes from the game's own module
   on a frame with nothing to give: no hand-over (0), no fog grid, no clip; no shadow map and no
   scaffold (a null view, which the passes replace with their own 1x1 dummy; an UNDEFINED shadow
   format, on which the passes give the dummy their own depth format and keep a NEAREST compare
   sampler they never sample); no effects models; Classic++ off and the restorer down, so no job is
   ever made; the A/B and growth levers unarmed. Nothing here holds state. */

#define TAGPU_SCENE_WIKI
#include "tagpu_scene.h"

#include <windows.h>
#include "tagpu_terr.h"
#include "tagpu_feat.h"
#include "tagpu_native.h"
#include "tagpu_vk_shadow.h"
#include "tagpu_vk_scaffold.h"
#include "tagpu_vk_fx.h"
#include "tagpu_vk_restore.h"
#include "tagpu_restore_guard.h"
#include "tagpu_classicpp.h"
#include "tagpu_vk.h"
#include "tagpu_packet.h"

/* ---- the hand-over sources ---------------------------------------------------------------- */

int tagpu_wikiworld_terr_handover(TAGPU_TERRHAND* out, unsigned now)
{
    (void)out; (void)now;
    return 0;
}

int tagpu_wikiworld_feat_handover(TAGPU_FEATHAND* out, unsigned now)
{
    (void)out; (void)now;
    return 0;
}

int tagpu_wikiworld_feat_atlas_hand(TAGPU_FEATHAND* h)
{
    (void)h;
    return 0;
}

int tagpu_wikiworld_feat_atlas_owed(void)
{
    return 0;
}

void tagpu_wikiworld_feat_atlas_ack(unsigned serial, int keep)
{
    (void)serial; (void)keep;
}

int tagpu_wikiworld_worldtgt(TAGPU_WORLDTGT* out)
{
    (void)out;
    return 0;
}

const unsigned short* tagpu_wikiworld_foggrid(int* cols, int* rows, int* cells)
{
    if (cols)  *cols = 0;
    if (rows)  *rows = 0;
    if (cells) *cells = 0;
    return NULL;
}

int tagpu_wikiworld_scissor_on(void)
{
    return 0;
}

/* ---- the game-only modules ---------------------------------------------------------------- */

VkFormat tagpu_wikiworld_shadow_format(const TAGPU_VKPASS* d, int* linearOk)
{
    (void)d;
    if (linearOk) *linearOk = 0;
    return VK_FORMAT_UNDEFINED;
}

int tagpu_wikiworld_shadow_ready(unsigned frame)
{
    (void)frame;
    return 0;
}

VkImageView tagpu_wikiworld_shadow_view(unsigned frame, uint32_t slot)
{
    (void)frame; (void)slot;
    return VK_NULL_HANDLE;
}

VkImageView tagpu_wikiworld_scaffold_view(unsigned frame, uint32_t slot)
{
    (void)frame; (void)slot;
    return VK_NULL_HANDLE;
}

int tagpu_wikiworld_fx_models_ok(unsigned frame)
{
    (void)frame;
    return 0;
}

int tagpu_wikiworld_classicpp_assets(void)
{
    return 0;
}

/* Only `aniso` is read, to build the restored twin's sampler, which the stage never samples:
   1 is the knob's "off". */
static const TAGPU_LIGHT s_classicLight = { .aniso = 1.0f };

const TAGPU_LIGHT* tagpu_wikiworld_classicpp_light(void)
{
    return &s_classicLight;
}

int tagpu_wikiworld_rguard_fault(const char* token)
{
    (void)token;
    return 0;
}

int tagpu_wikiworld_ab_arm(const char* tag)
{
    (void)tag;
    return 0;
}

int tagpu_wikiworld_grow_stress(void)
{
    return 0;
}

/* The restorer is down: `up` and `fits` refuse, so no instance makes a job. The job calls answer
   for a job that does not exist -- idle, failed, nothing painted -- and touch no argument. */

int tagpu_wikiworld_restore_up(const TAGPU_VKPASS* d)
{
    (void)d;
    return 0;
}

unsigned tagpu_wikiworld_restore_epoch(void)
{
    return 0;
}

int tagpu_wikiworld_restore_fits(const TAGPU_VKPASS* d, unsigned long long bytes, char* why,
                                 int whyLen)
{
    (void)d; (void)bytes;
    if (why && whyLen > 0) lstrcpynA(why, "the wiki's stage has no restorer", whyLen);
    return 0;
}

TAGPU_VKRJOB* tagpu_wikiworld_restore_job_new(const TAGPU_VKPASS* d, const char* tag,
                                              int prio, int oneshot, int model, int repaint,
                                              VkImage srcImg, VkImageView srcView,
                                              int srcW, int srcH, int srcBase,
                                              const unsigned char* pal,
                                              VkImage dstImg, VkImageView dstView,
                                              int dstW, int dstH)
{
    (void)d; (void)tag; (void)prio; (void)oneshot; (void)model; (void)repaint;
    (void)srcImg; (void)srcView; (void)srcW; (void)srcH; (void)srcBase; (void)pal;
    (void)dstImg; (void)dstView; (void)dstW; (void)dstH;
    return NULL;
}

int tagpu_wikiworld_restore_job_chain(const TAGPU_VKPASS* d, TAGPU_VKRJOB* j, int mips, int dim,
                                      const VkImageView* attach, const VkImageView* sample)
{
    (void)d; (void)j; (void)mips; (void)dim; (void)attach; (void)sample;
    return 0;
}

int tagpu_wikiworld_restore_job_add(TAGPU_VKRJOB* j, const TAGPU_RGLSL_FRAME* frames, int count)
{
    (void)j; (void)frames; (void)count;
    return 0;
}

int tagpu_wikiworld_restore_job_add_nbhd(TAGPU_VKRJOB* j, const TAGPU_RNBFRAME* frames, int count)
{
    (void)j; (void)frames; (void)count;
    return 0;
}

void tagpu_wikiworld_restore_job_budget(TAGPU_VKRJOB* j, int prio, double capMs)
{
    (void)j; (void)prio; (void)capMs;
}

int tagpu_wikiworld_restore_job_queued(const TAGPU_VKRJOB* j)
{
    (void)j;
    return 0;
}

void tagpu_wikiworld_restore_job_feeding(TAGPU_VKRJOB* j, int more)
{
    (void)j; (void)more;
}

int tagpu_wikiworld_restore_job_idle(const TAGPU_VKRJOB* j)
{
    (void)j;
    return 1;
}

int tagpu_wikiworld_restore_job_failed(const TAGPU_VKRJOB* j)
{
    (void)j;
    return 1;
}

int tagpu_wikiworld_restore_job_painted(const TAGPU_VKRJOB* j)
{
    (void)j;
    return 0;
}

int tagpu_wikiworld_restore_job_remap(TAGPU_VKRJOB* j, int (*map)(void* ctx, TAGPU_RGLSL_FRAME* f),
                                      void* ctx, int* kept, int* requeued, int* dropped)
{
    (void)j; (void)map; (void)ctx;
    if (kept)     *kept = 0;
    if (requeued) *requeued = 0;
    if (dropped)  *dropped = 0;
    return 0;
}

int tagpu_wikiworld_restore_job_dst_live(const TAGPU_VKRJOB* j)
{
    (void)j;
    return 0;
}

void tagpu_wikiworld_restore_job_free(const TAGPU_VKPASS* d, TAGPU_VKRJOB* j)
{
    (void)d; (void)j;
}
