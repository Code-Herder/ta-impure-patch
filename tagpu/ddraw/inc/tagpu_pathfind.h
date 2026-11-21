#ifndef TAGPU_PATHFIND_H
#define TAGPU_PATHFIND_H
/* tagpu_pathfind -- the ground units' pathfinder: our scheduler and our search in the engine's place
   (research/notes/pathfinder-parallel.md, research/notes/modules/pathfinder.md).

   At DllMain: the players phase's call of the engine's scheduler (0x464F92) is sent to ours, the step's call
   0x4955E5 to the join that applies a tick's results, and the ground course's set goal 0x44F2A0 is watched: all
   three or the engine's scheduler stays, each site and each engine function ours calls byte-matched first. With `tagpu_pathverify.on` in the game's folder the engine's scheduler is left in place
   instead, and four observers inside it -- 0x40EB70, 0x40D900, 0x40E050, 0x440BE0 -- run our search beside the
   engine's on every search and compare the two; they write no engine memory. */
void tagpu_pathfind_init(void);
/* a level's load: nothing of the last game's is carried */
void tagpu_pathfind_level_init(void);
#endif
