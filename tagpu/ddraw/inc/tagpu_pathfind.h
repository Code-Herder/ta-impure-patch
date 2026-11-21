#ifndef TAGPU_PATHFIND_H
#define TAGPU_PATHFIND_H
/* tagpu_pathfind -- the ground units' pathfinder: our scheduler and our search in the engine's place
   (research/notes/pathfinder-parallel.md, research/notes/modules/pathfinder.md).

   At DllMain: the players phase's call of the engine's scheduler (0x464F92) is sent to ours, the step's call
   0x4955E5 to the join that applies a tick's results, and the ground course's set goal 0x44F2A0 is watched: all
   three, each site and each engine function ours calls byte-matched first, or the launch is refused at the first
   DirectDraw call (tagpu_pathfind_report). With `tagpu_pathverify.on` in the game's folder the engine's scheduler is left in place
   instead, and four observers inside it -- 0x40EB70, 0x40D900, 0x40E050, 0x440BE0 -- run our search beside the
   engine's on every search and compare the two; they write no engine memory. */
void tagpu_pathfind_init(void);
/* At the first DirectDraw call: the launch ends (tagpu_refuse) unless the pathfinder is installed and its sites
   still hold what Impure wrote -- a game never runs on the engine's scheduler, but under tagpu_pathverify.on */
void tagpu_pathfind_report(void);
/* a level's load: nothing of the last game's is carried */
void tagpu_pathfind_level_init(void);
#endif
