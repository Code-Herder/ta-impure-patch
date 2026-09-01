#ifndef TAGPU_SCAFFOLD_H
#define TAGPU_SCAFFOLD_H
#include "tagpu.h"
/* G12a: per-frame scene-depth scaffold (painter's row key as a depth buffer)
   + colour debug overlay + per-unit occlusion prediction log. Armed by the
   tagpu_scaffold.on trigger file; safe no-op otherwise. Leaves program/VAO
   bindings at 0. */
void tagpu_scaffold_frame(const TAGPU_FRAME* f);
unsigned int tagpu_scaffold_texref(void);
int tagpu_scaffold_frameinfo(unsigned frame_counter, int* r0, int* nrows);
#endif
