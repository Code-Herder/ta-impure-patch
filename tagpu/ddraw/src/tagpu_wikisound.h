#ifndef TAGPU_WIKISOUND_H
#define TAGPU_WIKISOUND_H
/* tagpu_wikisound -- the in-game wiki's own mixer: the sounds its simulation makes (tagpu_wikisim.h,
   THE SOUND) and its page's buttons, played through a DirectSound object of its own
   (research/notes/ingame-wiki-cob-plan.md, decision 12).

   NEVER THE ENGINE'S: no engine sound entry is called (0x47F0C0, 0x47F1A0, 0x47F300, the notice
   queue 0x47F780), so the game's 24 per-kind cooldowns, its notice queue and its buffers are the
   game's alone, and the game's own sounds are what they are with the wiki open.

   THE PLAYER'S SETTINGS, as the engine's entries apply them (tagpu_menu_sound, read at each play):
   nothing plays while UseWindowsSound or NoDirectSound says the engine has no DirectSound, while
   fxvol is 0 or Sound Mode is off (0x47FEA8..0x47FEC2, 0x47F10C..0x47F11F); a unit's reply needs
   speechfx and a priority above 10 - unitchat (0x47FDEC, 0x47FE18). Each play is at the engine's
   own -585 (0x47FED0, 0x47F17A); fxvol is never applied here: the Options slider sets it as the
   process's wave volume (waveOutSetVolume(fxvol << 10), 0x45BCC0 -> 0x4D0070), which reaches this
   object's buffers as it reaches the engine's. Neither the reply nor the named sound is positional
   (0x4D0640 takes none; 0x4CF570 takes one, and these callers pass 0), so Sound Mode's 3D and
   off-screen rules do not apply.

   OUR OWN LIFETIME: the object is created by DirectSoundCreate out of the dsound.dll the game
   imports, at the first sound after an open, on the game thread, at PRIORITY on the game's window
   as the engine's (DSSCL_PRIORITY pushed at 0x4CEFBB) and without global focus, so it is silent
   while the game is not in front, as the engine's is. A buffer is made a WAV at its first play and
   up to WIKISOUND_VOICES copies of it (DuplicateSoundBuffer), 0x4CF570's rule: a stopped copy is
   reused, else a new copy is made, else the one furthest through its samples (the largest play
   cursor, GetCurrentPosition at 0x4CF615) restarts. tagpu_wikisound_release lets
   everything go, on the game thread, when the wiki closes -- the level's teardown closes it first
   (tagpu_wiki_level_end, from the teardown wrap's entry to 0x491B60) -- and never at DLL detach.
   A create that fails leaves the wiki silent for the process and says so once; the engine's
   object is never borrowed.

   Game thread only. The clips are the published WIKI3D's (tagpu_wikiasset.h), immutable and never
   freed, so a buffer made from one never outlives its samples. */
#include "tagpu_wikiasset.h"

#define WIKISOUND_VOICES 4

/* a unit's reply: category `cat`'s variant `variant` of `kind`, at `priority` (the kind table's);
   a named sound: WIKI_SNDNAME_* `name`. 1 when a Play call succeeded. */
int  tagpu_wikisound_reply(const WIKI3DSOUND* s, int cat, int kind, int variant, int priority);
int  tagpu_wikisound_named(const WIKI3DSOUND* s, int name);

/* what was played since the last call -- Play calls that succeeded, that failed, and sounds the
   settings or a silent clip left unplayed -- and zeroes it */
void tagpu_wikisound_tally(int* played, int* failed, int* silent);

/* every buffer and the object let go; the next sound creates them again */
void tagpu_wikisound_release(void);

#endif
