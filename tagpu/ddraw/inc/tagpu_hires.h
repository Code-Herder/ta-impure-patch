#ifndef TAGPU_HIRES_H
#define TAGPU_HIRES_H
/* Replacement-mesh slot, glTF 2.0: gamedir/hires/<defname>.glb (or .gltf),
   hot-reloaded. tagpu_hires.c loads; tagpu_hires_draw.c renders. */

/* posable pieces per model. The renderer spends 3 vec4 of vertex-shader
   uniform on each, so this is a GPU budget as much as a modelling one. */
#define TAGPU_HMAXPIECE 48

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
/* The pieces the model can be posed by: one per glTF node carrying geometry,
   in the order the vertices' piece attribute tags them. The NAME is the whole
   binding to the engine — the caller matches it, case-insensitively, against
   the unit's own 3DO piece names and hands back one matrix per piece. */
int          tagpu_hires_npiece(const void* mesh);
const char*  tagpu_hires_piece(const void* mesh, int i);
/* changes whenever the file is reloaded — the mesh handle does not, so this is
   what tells a cache built from the piece list that the list is a new one */
unsigned     tagpu_hires_gen(const void* mesh);
/* 0 if the group is empty or the mesh is not on the GPU yet */
int          tagpu_hires_group(const void* mesh, int i, TAGPU_HGROUP* out);
/* the mesh's VAO, uploading vertex buffer and textures on the first call —
   CALL ONLY WITH A CURRENT GL CONTEXT. 0 if there is nothing to draw. */
unsigned int tagpu_hires_vao(const void* mesh);
void         tagpu_hires_glreset(void);
#endif
