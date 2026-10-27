#ifndef TAGPU_SCENE_WIKI_H
#define TAGPU_SCENE_WIKI_H
/* tagpu_scene_wiki.h -- the entry points of the wiki's instances of the world passes
   (tagpu_scene.h), for the seam that calls them (tagpu_vk.c): the world target's, the terrain's
   and the features' -- the passes landing 6g draws the stage's ground with.

   WRITTEN OUT BY HAND, AND CHECKED. The instances' own prototypes are their game headers' under
   the names tagpu_scene.h maps them to, which a file can only see by compiling with
   TAGPU_SCENE_WIKI -- and then it no longer sees the game's: the headers' include guards keep a
   second inclusion out. So these are declared here under their mapped names, and
   tagpu_wikiworld.c, which is compiled with TAGPU_SCENE_WIKI, includes this header after the game
   headers: each function is declared twice in that file, once by its game header through the
   mapping and once here, and C refuses two declarations of one function that disagree in a
   parameter or the return. A name here the mapping does not make is a link error. Either way a
   drift does not build. */
#include "tagpu_vk_pass.h"

int  tagpu_vkw_world_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                             uint32_t* tw, uint32_t* th);
void tagpu_vkw_world_begin(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot);
void tagpu_vkw_world_end(const TAGPU_VKPASS* d, VkCommandBuffer cb);
void tagpu_vkw_world_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                            uint32_t w, uint32_t h);
int  tagpu_vkw_world_shot(uint32_t slot, VkImage* img, uint32_t* w, uint32_t* h,
                          VkFormat* fmt);
void tagpu_vkw_world_down(const TAGPU_VKPASS* d);
int  tagpu_vkw_world_down_owed(void);
void tagpu_vkw_world_down_paid(const TAGPU_VKPASS* d);

int  tagpu_vkw_terr_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot);
void tagpu_vkw_terr_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                           uint32_t w, uint32_t h);
void tagpu_vkw_terr_down(const TAGPU_VKPASS* d);
int  tagpu_vkw_terr_down_owed(void);
void tagpu_vkw_terr_down_paid(const TAGPU_VKPASS* d);

int  tagpu_vkw_feat_prepare(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot);
void tagpu_vkw_feat_record(const TAGPU_VKPASS* d, VkCommandBuffer cb, uint32_t slot,
                           uint32_t w, uint32_t h);
void tagpu_vkw_feat_down(const TAGPU_VKPASS* d);
int  tagpu_vkw_feat_down_owed(void);
void tagpu_vkw_feat_down_paid(const TAGPU_VKPASS* d);

#endif
