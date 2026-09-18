#ifndef TAGPU_INPUT_H
#define TAGPU_INPUT_H
#include "tagpu.h"
/* In-process input injection: tagpu_keys.txt (one-shot key tokens posted to
   the game window) + tagpu_eye.txt (camera eye hold). Session-proof — no X.

   THE TWO HALVES RUN ON DIFFERENT THREADS SINCE LANDING 10c-2, and the split is
   along the packet: the token half needs none and drives the game, the hold
   half dereferences f->packet and feeds the render thread's command record. */

/* The token half: keys, clicks, the shield's expiries. GAME THREAD, from
   tagpu_gui_hook.c's before_flip via tagpu_triggers_frame — which is what lets
   `tacli keys` and `tacli click` reach renderer=gdi, where tagpu_overlay_draw
   is never called. Reads no packet; posts, never sends. */
void tagpu_input_frame(const TAGPU_FRAME* f);

/* The hold half: tagpu_eye.txt. RENDER THREAD, from tagpu_overlay_draw. Needs
   f->packet, and its output is read on that same thread (tagpu_input_cmd
   below). Not reached on renderer=gdi — nor could it do anything there, since
   the command record it feeds is posted from the overlay frame. */
void tagpu_input_eye_frame(const TAGPU_FRAME* f);

/* 1 while tagpu_eye.txt is holding the camera. tagpu_zoom asks before it steps
   the eye for the cursor anchor — a hold means the camera does not move, and
   it wins. Render thread. */
int  tagpu_input_eye_held(void);

/* The hold's half of this frame's command record (tagpu_packet.h): on, and
   where. Render thread, from tagpu_zoom_frame_end(); the game thread clamps
   the point into the camera's range and writes the eye and its scroll target
   at the top of every in-play draw for as long as the file stands. This
   module writes no engine memory itself since the frame packet's landing 2. */
struct TAGPU_CMD;
void tagpu_input_cmd(struct TAGPU_CMD* rec);
#endif
