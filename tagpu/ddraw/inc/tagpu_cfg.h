#ifndef TAGPU_CFG_H
#define TAGPU_CFG_H
/* tagpu_cfg -- the fork settings this DLL owns, so a player never edits an ini
   to get them. The sibling of tagpu_opt.h: that one arms our render passes with
   no arm file, this one configures cnc-ddraw itself.

   THE RULE. Our value applies only when the player did NOT write the key. The
   presence test uses the same section rule cfg_get_string does (the game
   section first, then `ddraw`), so a key the player typed wins. Display mode,
   the frame cap and the windowed frame are the settings store's
   (tagpu_settings.h): an absent key takes the store's value, and cnc-ddraw's
   own save-on-exit is off, so nothing but the player (or tacli) types them.

   THE ONE EXCEPTION IS A BOUND, NOT A PREFERENCE. `max_resolutions` is clamped
   to TA's own buffer whatever the ini says; see tagpu_cfg.c. */

/* Called from the END of cfg_load(), immediately before ini_free() -- the ini
   is still parsed there, which is what makes the presence test possible. */
void tagpu_cfg_defaults(void);

/* Called from EnumDisplayModes once the desktop mode is known, before the
   `inject_resolution` string is parsed. Deliberately NOT done at
   DLL_PROCESS_ATTACH: cfg_load runs under the loader lock and this needs the
   display. Does nothing if the player wrote `inject_resolution` themselves. */
void tagpu_cfg_inject_native(unsigned int w, unsigned int h);

/* 1 when ddraw.ini carries the key that holds the menu row -- Display mode,
   Frame cap, the windowed frame. The row is then greyed (tagpu_settings.h). */
int tagpu_cfg_display_held(void);
int tagpu_cfg_maxfps_held(void);
int tagpu_cfg_window_held(void);

#endif
