#ifndef TAGPU_INPUT_H
#define TAGPU_INPUT_H
#include "tagpu.h"
/* In-process input injection: tagpu_keys.txt (one-shot key tokens posted to
   the game window) + tagpu_eye.txt (camera eye hold). Session-proof — no X. */
void tagpu_input_frame(const TAGPU_FRAME* f);

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
