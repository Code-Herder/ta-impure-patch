#ifndef TAGPU_WIKISOUND_H
#define TAGPU_WIKISOUND_H
/* tagpu_wikisound -- the in-game wiki's own mixer: the sounds its simulation makes (tagpu_wikisim.h,
   THE SOUND) and its page's buttons, played through a DirectSound object of its own
   (research/notes/ingame-wiki-cob-plan.md, decision 12, and *Sound*).

   NEVER THE ENGINE'S: no engine sound entry is called (0x47F0C0, 0x47F1A0, 0x47F300, the notice
   queue 0x47F780), so the game's 24 per-kind cooldowns, its notice queue and its buffers are the
   game's alone, and the game's own sounds are what they are with the wiki open.

   THE PLAYER'S SETTINGS (tagpu_menu_sound, read on the game thread at each play): nothing plays
   while NoDirectSound says the engine has no DirectSound, or while fxvol is 0 or Sound Mode is off
   (0x47FEA8..0x47FEC2, 0x47F10C..0x47F11F); a unit's reply needs speechfx and a priority above
   10 - unitchat (0x47FDEC, 0x47FE18). UseWindowsSound does NOT silence the engine, which then
   plays its sounds through PlaySoundA (0x47F0D9 -> 0x49F6A0 / 0x49F680; a reply 0x47FE75..0x47FE83
   -> 0x49F6C0); the wiki has no PlaySoundA path and is silent under it, its own choice (the plan's
   *Sound*). Each play is at the engine's own -585 (0x47FED0, 0x47F17A), and fxvol is not applied
   here: the Options slider sets it as the process's wave volume (waveOutSetVolume(fxvol << 10),
   0x45BCC0 -> 0x4D0070), which reaches this object's buffers as it reaches the engine's
   [INFERRED: not measured on Windows or under Wine]. Neither the reply nor the named sound is
   positional (0x4D0640 takes no point; these callers pass 0x4CF570 none), so Sound Mode's 3D and
   off-screen rules do not apply.

   NOT ON THE GAME THREAD: the object and one buffer a clip (DSBCAPS_STATIC | DSBCAPS_CTRLVOLUME,
   filled from the published samples) are made off it, by tagpu_wikisound_make -- on the 3D load's
   thread, before the WIKI3D is published, unless a muting setting was on when the load started;
   else on a thread of its own that tagpu_wikisound_arm starts at an open once the settings no
   longer mute. The object is DirectSoundCreate's from the dsound.dll the game imports, at PRIORITY
   on the game's window as the engine's (DSSCL_PRIORITY pushed at 0x4CEFBB), without global focus,
   so it is silent while the game is not in front, as the engine's is. It is published by one
   interlocked store once every buffer is made, and kept for the process: never released, not at
   the wiki's close, not at DLL detach. A create that fails leaves the wiki silent for the process
   and says so once; the engine's object is never borrowed. A clip DirectSound refuses a buffer
   for is silent for the object's life, said once when it is made.

   ON THE GAME THREAD only GetStatus, GetCurrentPosition, DuplicateSoundBuffer, SetCurrentPosition,
   SetVolume, Play and Stop, by 0x4CF570's rule for WIKISOUND_VOICES copies of a clip's buffer:
   the first copy whose status is 0 plays; a GetStatus that fails plays nothing (0x4CF5F8); with
   every copy busy, a copy not made yet is duplicated, and a duplicate that fails plays nothing
   (0x4CF66D); with all four made, the one with the largest play cursor restarts (0x4CF615). The
   wiki's close stops every voice (tagpu_wikisound_stop) -- the level's teardown closes it first
   (tagpu_wiki_level_end, from the teardown wrap's entry to 0x491B60).

   The clips are the published WIKI3D's (tagpu_wikiasset.h), immutable and never freed, so a
   buffer made from one never outlives its samples. */
#include <stdint.h>
#include "tagpu_wikiasset.h"

#define WIKISOUND_VOICES 4

/* The object and the clips' buffers for `s` on window `hwnd`, published for the process; does
   nothing once a make has run or is running. Any thread but the game's; it blocks for as long as
   DirectSound takes. */
void tagpu_wikisound_make(const WIKI3DSOUND* s, uintptr_t hwnd);

/* Game thread, at an open: when no make has run, `s` is published and the player's settings no
   longer mute, a thread of its own makes the object (tagpu_wikisound_make). */
void tagpu_wikisound_arm(const WIKI3DSOUND* s, uintptr_t hwnd);

/* 1 when the player's settings silence every sound the wiki plays (above); game thread */
int  tagpu_wikisound_muted(void);

/* Game thread. A unit's reply: category `cat`'s variant `variant` of `kind`, at `priority` (the
   kind table's); a named sound: WIKI_SNDNAME_* `name`. 1 when a Play call succeeded. */
int  tagpu_wikisound_reply(const WIKI3DSOUND* s, int cat, int kind, int variant, int priority);
int  tagpu_wikisound_named(const WIKI3DSOUND* s, int name);

/* what was played since the last call -- Play calls that succeeded, that failed, and sounds the
   settings, no object or a silent clip left unplayed -- and zeroes it; game thread */
void tagpu_wikisound_tally(int* played, int* failed, int* silent);

/* every voice stopped; the object and the buffers stay; game thread */
void tagpu_wikisound_stop(void);

#endif
