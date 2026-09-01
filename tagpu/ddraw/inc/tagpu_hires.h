#ifndef TAGPU_HIRES_H
#define TAGPU_HIRES_H
/* G12d replacement-mesh slot: gamedir/hires/<defname>.obj, hot-reloaded.
   Returns NULL when no replacement exists for the type. */
const void*  tagpu_hires_mesh(const char* defname);
int          tagpu_hires_ntri(const void* mesh);
const float* tagpu_hires_tri(const void* mesh, int i);   /* 9 floats */
int          tagpu_hires_col(const void* mesh, int i);   /* palette idx */
#endif
