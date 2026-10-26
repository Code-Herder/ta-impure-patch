#ifndef TAGPU_SCENE_H
#define TAGPU_SCENE_H
/* tagpu_scene.h -- the second scene: the world passes compiled again for the in-game wiki's stage.

   Seven files draw one scene: the world target (tagpu_vk_world.c), the terrain, feature and unit
   passes (tagpu_vk_terr.c, tagpu_vk_feat.c, tagpu_vk_unit.c), the posed program
   (tagpu_posedraw.c), the geometry bake (tagpu_posebake.c) and the unit material layer
   (tagpu_render3do.c). Their state -- per-slot buffers, atlases and their single acked serial,
   bake caches bounded by one scene's records, latches -- is file-scope statics, and a second scene
   drawn through the same statics would be corrupted, not merely wrong
   (research/notes/ingame-wiki-3d-stage-plan.md, "The world passes"). So the Makefile compiles each
   of the seven twice: `src/<name>.o` is the game's world, unchanged, and `src/<name>.wiki.o`,
   compiled with -DTAGPU_SCENE_WIKI, is the wiki's stage. Each of the seven includes this header
   first. Without TAGPU_SCENE_WIKI it does nothing; with it, it does four things:

     1. It renames every external symbol the seven define (tagpu_vk_* -> tagpu_vkw_*,
        tagpu_r3d_* -> tagpu_r3dw_*, tagpu_posedraw_* -> tagpu_pdw_*, tagpu_posebake_* ->
        tagpu_pbw_*), so every call between the wiki's instances stays among them.
     2. It points the instances' hand-over sources at the wiki's producer, tagpu_wikiworld.c.
     3. It points every call into a module only the game's world has -- the cast-shadow map, the
        scaffold, the effects pass, the Classic++ restorer and its switches, the A/B and growth
        levers -- at tagpu_wikiworld.c's inert answer. The wiki's instances therefore bind no image
        a game pass writes and queue no restorer job.
     4. It sizes the bake's caches, the unit pass's vertex-buffer table and their lookup hashes for
        the stage (WIKIWORLD_FRAMEMAX), and makes the bake's scratch the game instance's
        (PB_SCRATCH).

   THE INVARIANTS, AND WHAT ENFORCES EACH:

     NO STATIC OF THE WIKI'S STAGE IS A STATIC OF THE GAME'S WORLD. Two translation units share no
       file-scope state: C's linkage guarantees it for every static these files hold today and for
       every one a later edit adds, without anyone having to remember. The one deliberate
       exception is the bake's scratch below, which has external linkage for that reason.
     EVERY EXTERNAL SYMBOL IS RENAMED. The link proves it: a definition left off list 1 is defined
       by both objects and the link fails naming it.
     NO WIKI INSTANCE REACHES A GAME-ONLY MODULE. tools/scene-check.sh, a prerequisite of the link,
       lists each wiki object's undefined symbols and fails on any that another object of ours
       defines and that is not on its allow list of stateless or read-only helpers, naming the
       symbol. A call this header does not redirect is therefore a build failure, not a wiki
       stage drawn from the game's shadow map.

   THE BAKE'S SCRATCH IS SHARED, AND WHY THAT IS SAFE. The scratch holds one model while one call of
   tagpu_posebake_unit walks it, and is dead when the call returns. Both instances run on the render
   thread only, and neither calls the other (list 1 keeps every call inside its own instance), so
   no two bakes are ever inside a walk at once. The 5.3 MB it costs is paid once.

   IT DECLARES NOTHING FOR A CALLER OUTSIDE THE SEVEN: nothing outside them calls the wiki's
   instances, and a caller's declarations belong with the caller. */

#ifdef TAGPU_SCENE_WIKI

/* ---- 1. the instances' own names --------------------------------------------------------- */
#define tagpu_vk_world_begin            tagpu_vkw_world_begin
#define tagpu_vk_world_down             tagpu_vkw_world_down
#define tagpu_vk_world_down_owed        tagpu_vkw_world_down_owed
#define tagpu_vk_world_down_paid        tagpu_vkw_world_down_paid
#define tagpu_vk_world_end              tagpu_vkw_world_end
#define tagpu_vk_world_prepare          tagpu_vkw_world_prepare
#define tagpu_vk_world_record           tagpu_vkw_world_record
#define tagpu_vk_world_record_direct    tagpu_vkw_world_record_direct
#define tagpu_vk_world_shot             tagpu_vkw_world_shot

#define tagpu_vk_terr_ab_frame          tagpu_vkw_terr_ab_frame
#define tagpu_vk_terr_down              tagpu_vkw_terr_down
#define tagpu_vk_terr_down_owed         tagpu_vkw_terr_down_owed
#define tagpu_vk_terr_down_paid         tagpu_vkw_terr_down_paid
#define tagpu_vk_terr_prepare           tagpu_vkw_terr_prepare
#define tagpu_vk_terr_record            tagpu_vkw_terr_record

#define tagpu_vk_feat_ab_frame          tagpu_vkw_feat_ab_frame
#define tagpu_vk_feat_down              tagpu_vkw_feat_down
#define tagpu_vk_feat_down_owed         tagpu_vkw_feat_down_owed
#define tagpu_vk_feat_down_paid         tagpu_vkw_feat_down_paid
#define tagpu_vk_feat_prepare           tagpu_vkw_feat_prepare
#define tagpu_vk_feat_record            tagpu_vkw_feat_record

#define tagpu_vk_unit_ab_frame          tagpu_vkw_unit_ab_frame
#define tagpu_vk_unit_cast              tagpu_vkw_unit_cast
#define tagpu_vk_unit_casters           tagpu_vkw_unit_casters
#define tagpu_vk_unit_down              tagpu_vkw_unit_down
#define tagpu_vk_unit_down_owed         tagpu_vkw_unit_down_owed
#define tagpu_vk_unit_down_paid         tagpu_vkw_unit_down_paid
#define tagpu_vk_unit_fx_count          tagpu_vkw_unit_fx_count
#define tagpu_vk_unit_fx_ready          tagpu_vkw_unit_fx_ready
#define tagpu_vk_unit_prepare           tagpu_vkw_unit_prepare
#define tagpu_vk_unit_record            tagpu_vkw_unit_record
#define tagpu_vk_unit_record_ghosts     tagpu_vkw_unit_record_ghosts
#define tagpu_vk_unit_upload            tagpu_vkw_unit_upload

#define tagpu_posedraw_atlas_ack        tagpu_pdw_atlas_ack
#define tagpu_posedraw_atlas_hand       tagpu_pdw_atlas_hand
#define tagpu_posedraw_atlas_owed       tagpu_pdw_atlas_owed
#define tagpu_posedraw_begin            tagpu_pdw_begin
#define tagpu_posedraw_begin_ghost      tagpu_pdw_begin_ghost
#define tagpu_posedraw_end              tagpu_pdw_end
#define tagpu_posedraw_fog_carried      tagpu_pdw_fog_carried
#define tagpu_posedraw_frame            tagpu_pdw_frame
#define tagpu_posedraw_fx               tagpu_pdw_fx
#define tagpu_posedraw_handover         tagpu_pdw_handover
#define tagpu_posedraw_mirrors          tagpu_pdw_mirrors
#define tagpu_posedraw_other_forecast   tagpu_pdw_other_forecast
#define tagpu_posedraw_ready            tagpu_pdw_ready
#define tagpu_posedraw_refused          tagpu_pdw_refused
#define tagpu_posedraw_shadow_on        tagpu_pdw_shadow_on
#define tagpu_posedraw_slant_drew       tagpu_pdw_slant_drew
#define tagpu_posedraw_slant_take       tagpu_pdw_slant_take
#define tagpu_posedraw_uncarried        tagpu_pdw_uncarried
#define tagpu_posedraw_unit             tagpu_pdw_unit

#define tagpu_posebake_frame            tagpu_pbw_frame
#define tagpu_posebake_geom_mirror      tagpu_pbw_geom_mirror
#define tagpu_posebake_mat_mirror       tagpu_pbw_mat_mirror
#define tagpu_posebake_unit             tagpu_pbw_unit

#define tagpu_r3d_atlas_ack             tagpu_r3dw_atlas_ack
#define tagpu_r3d_atlas_frame           tagpu_r3dw_atlas_frame
#define tagpu_r3d_atlas_gen             tagpu_r3dw_atlas_gen
#define tagpu_r3d_atlas_level           tagpu_r3dw_atlas_level
#define tagpu_r3d_atlas_mirror_want     tagpu_r3dw_atlas_mirror_want
#define tagpu_r3d_atlas_owed            tagpu_r3dw_atlas_owed
#define tagpu_r3d_atlas_recycle         tagpu_r3dw_atlas_recycle
#define tagpu_r3d_atlas_restore_armed   tagpu_r3dw_atlas_restore_armed
#define tagpu_r3d_atlas_restore_list    tagpu_r3dw_atlas_restore_list
#define tagpu_r3d_atlas_restore_want    tagpu_r3dw_atlas_restore_want
#define tagpu_r3d_atlas_texels          tagpu_r3dw_atlas_texels
#define tagpu_r3d_atlas_uv              tagpu_r3dw_atlas_uv
#define tagpu_r3d_atlas_view            tagpu_r3dw_atlas_view
#define tagpu_r3d_ensure                tagpu_r3dw_ensure
#define tagpu_r3d_face_colour           tagpu_r3dw_face_colour
#define tagpu_r3d_face_texframe         tagpu_r3dw_face_texframe
#define tagpu_r3d_nano_state            tagpu_r3dw_nano_state
#define tagpu_r3d_ready                 tagpu_r3dw_ready
#define tagpu_r3d_shade_k               tagpu_r3dw_shade_k
#define tagpu_r3d_shade_neutral         tagpu_r3dw_shade_neutral
#define tagpu_r3d_shade_want            tagpu_r3dw_shade_want

/* ---- 2. the hand-over sources: the wiki's producer, not the game's gathers ---------------- */
#define tagpu_terr_handover             tagpu_wikiworld_terr_handover
#define tagpu_feat_handover             tagpu_wikiworld_feat_handover
#define tagpu_feat_atlas_hand           tagpu_wikiworld_feat_atlas_hand
#define tagpu_feat_atlas_owed           tagpu_wikiworld_feat_atlas_owed
#define tagpu_feat_atlas_ack            tagpu_wikiworld_feat_atlas_ack
#define tagpu_native_worldtgt           tagpu_wikiworld_worldtgt
#define tagpu_native_foggrid            tagpu_wikiworld_foggrid
#define tagpu_native_scissor_on         tagpu_wikiworld_scissor_on

/* ---- 3. the game-only modules: an inert answer each --------------------------------------- */
#define tagpu_vk_shadow_format          tagpu_wikiworld_shadow_format
#define tagpu_vk_shadow_ready           tagpu_wikiworld_shadow_ready
#define tagpu_vk_shadow_view            tagpu_wikiworld_shadow_view
#define tagpu_vk_scaffold_view          tagpu_wikiworld_scaffold_view
#define tagpu_vk_fx_models_ok           tagpu_wikiworld_fx_models_ok
#define tagpu_classicpp_assets          tagpu_wikiworld_classicpp_assets
#define tagpu_classicpp_light           tagpu_wikiworld_classicpp_light
#define tagpu_rguard_fault              tagpu_wikiworld_rguard_fault
#define tagpu_vk_ab_arm                 tagpu_wikiworld_ab_arm
#define tagpu_grow_stress               tagpu_wikiworld_grow_stress
#define tagpu_vk_restore_up             tagpu_wikiworld_restore_up
#define tagpu_vk_restore_epoch          tagpu_wikiworld_restore_epoch
#define tagpu_vk_restore_fits           tagpu_wikiworld_restore_fits
#define tagpu_vk_restore_job_new        tagpu_wikiworld_restore_job_new
#define tagpu_vk_restore_job_chain      tagpu_wikiworld_restore_job_chain
#define tagpu_vk_restore_job_add        tagpu_wikiworld_restore_job_add
#define tagpu_vk_restore_job_add_nbhd   tagpu_wikiworld_restore_job_add_nbhd
#define tagpu_vk_restore_job_budget     tagpu_wikiworld_restore_job_budget
#define tagpu_vk_restore_job_queued     tagpu_wikiworld_restore_job_queued
#define tagpu_vk_restore_job_feeding    tagpu_wikiworld_restore_job_feeding
#define tagpu_vk_restore_job_idle       tagpu_wikiworld_restore_job_idle
#define tagpu_vk_restore_job_failed     tagpu_wikiworld_restore_job_failed
#define tagpu_vk_restore_job_painted    tagpu_wikiworld_restore_job_painted
#define tagpu_vk_restore_job_remap      tagpu_wikiworld_restore_job_remap
#define tagpu_vk_restore_job_dst_live   tagpu_wikiworld_restore_job_dst_live
#define tagpu_vk_restore_job_free       tagpu_wikiworld_restore_job_free

/* ---- 4. the stage's sizes, and the shared scratch ----------------------------------------- */
#include "tagpu_wikiworld.h"            /* WIKIWORLD_FRAMEMAX */

/* tagpu_posebake.h's frame bound and its two keeps. The keep is where recycling starts, so a keep
   at the bound recycles only a full table, which a frame of at most the bound always leaves an
   entry to evict from (tagpu_posebake.c `geom_slot`). */
#define TAGPU_PB_FRAMEMAX               WIKIWORLD_FRAMEMAX
#define TAGPU_PB_KEEPGEOM               ((int)WIKIWORLD_FRAMEMAX)
#define TAGPU_PB_KEEPMAT                ((int)WIKIWORLD_FRAMEMAX)

/* the two lookup hashes over those tables, tagpu_posebake.c's and tagpu_vk_unit.c's: 2 048 chains
   for at most 2 x WIKIWORLD_FRAMEMAX entries. The game's 65 536 chains, sized for its own tables
   of 24 577 to 61 442 entries, would cost the stage 384 KB of .bss. */
#define PB_HBITS                        11
#define VB_HBITS                        11

/* the bake's scratch is the game instance's definition: see "THE BAKE'S SCRATCH IS SHARED" */
#define PB_SCRATCH                      extern

#endif
#endif
