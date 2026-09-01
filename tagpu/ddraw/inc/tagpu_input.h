#ifndef TAGPU_INPUT_H
#define TAGPU_INPUT_H
#include "tagpu.h"
/* In-process input injection: tagpu_keys.txt (one-shot key tokens posted to
   the game window) + tagpu_eye.txt (camera eye hold). Session-proof — no X. */
void tagpu_input_frame(const TAGPU_FRAME* f);
#endif
