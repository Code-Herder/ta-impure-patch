#ifndef TAGPU_INPUT_H
#define TAGPU_INPUT_H
#include "tagpu.h"
/* In-process input injection: tagpu_keys.txt (one-shot key tokens posted to
   the game window) + tagpu_eye.txt (camera eye hold). Session-proof — no X. */
void tagpu_input_frame(const TAGPU_FRAME* f);

/* 1 while tagpu_eye.txt is holding the camera. tagpu_zoom asks before it steps
   the eye for the cursor anchor — a hold means the camera does not move, and
   it wins. */
int  tagpu_input_eye_held(void);
#endif
