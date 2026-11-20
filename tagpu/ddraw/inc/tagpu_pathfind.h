#ifndef TAGPU_PATHFIND_H
#define TAGPU_PATHFIND_H
/* tagpu_pathfind -- the pathfinder's search, ours, held to the engine's (research/notes/pathfinder-parallel.md).

   At DllMain: with `tagpu_pathverify.on` in the game's folder, four observers inside the engine's pathfinder --
   the scheduler 0x40EB70, the grid reset 0x40D900, the extraction 0x40E050 and the class's cleanup 0x440BE0 --
   byte-matched and all or none. They run our search beside the engine's on every search, compare the two results
   and count the units the 60-tick gate holds; they write no engine memory. With no lever nothing is installed. */
void tagpu_pathfind_init(void);
#endif
