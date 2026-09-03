#ifndef TAGPU_HIRES_H
#define TAGPU_HIRES_H
/* Replacement-mesh slot, glTF 2.0: gamedir/hires/<defname>.glb (or .gltf),
   hot-reloaded. tagpu_hires.c loads; tagpu_hires_draw.c renders. */

/* one glTF material = one contiguous draw out of the mesh's vertex buffer */
typedef struct {
    unsigned int albedo;      /* GL texture; a 1x1 white when the material had none */
    unsigned int normal;      /* GL texture, 0 = no normal map                      */
    float base[4];            /* baseColorFactor, LINEAR as glTF stores it          */
    float metal, rough;
    float cutoff;             /* alphaMode: < 0 OPAQUE, else the alpha cutoff       */
    int   doubleSided;
    int   first, count;       /* first vertex and vertex count, for glDrawArrays    */
} TAGPU_HGROUP;

/* NULL when no replacement exists for the type. Cheap to call per frame: it
   only stats the file, and reloads when the write time moves. */
const void*  tagpu_hires_mesh(const char* defname);
int          tagpu_hires_ngroup(const void* mesh);
/* 0 if the group is empty or the mesh is not on the GPU yet */
int          tagpu_hires_group(const void* mesh, int i, TAGPU_HGROUP* out);
/* the mesh's VAO, uploading vertex buffer and textures on the first call —
   CALL ONLY WITH A CURRENT GL CONTEXT. 0 if there is nothing to draw. */
unsigned int tagpu_hires_vao(const void* mesh);
void         tagpu_hires_glreset(void);
#endif
