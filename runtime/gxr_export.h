/*
 * The draw exporter (SOA_GXR_EXPORT=<dir>): every draw and copy the renderer
 * builds, written out with what it knew before the transform -- the vertices
 * in model space, the matrices, the TEV, blend and depth state and the
 * decoded textures -- so a frame can be rebuilt elsewhere as 3D geometry.
 * For replays of captured frames (soa --replay), with SOA_GXR_INLINE=1 so
 * every copy a draw samples has run before the draw is written.
 *
 *   <dir>/draws.jsonl  one object per draw or copy, in order
 *   <dir>/verts.bin    float32 vertices, GXR_EXPORT_FLOATS each; a draw's
 *                      "vofs" is its first vertex's index
 *   <dir>/tex/<hash>.png  each texture a draw samples, level 0, once
 *
 * A vertex is: view-space position (3), view-space normal (3), model-space
 * position (3), matrix index (1), clip-space position (4), the two lit
 * colours (4 + 4) and the eight generated texture coordinates (8 x 3).
 */
#ifndef SOA_GXR_EXPORT_H
#define SOA_GXR_EXPORT_H

#include "gxr_cmd.h"

#define GXR_EXPORT_FLOATS 46

/* One vertex as the vertex loader reads it, before the transform. */
typedef struct {
    float pos[3];
    float nrm[3];
    Color4 col[2];
    float tex[8][2];
    unsigned posidx, texidx[8];
    int has_nrm, has_col[2], has_tex[8];
} VertexIn;

/* What the renderer calls on the producer while an exporter is set. */
typedef struct {
    /* vertex i of the draw being built, before and after the transform */
    void (*vertex)(unsigned i, const VertexIn* in, const Vertex* out);
    /* the draw whose count vertices were just given */
    void (*draw)(const DrawCmd* D, unsigned count);
    void (*copy)(uint32_t v, int x0, int y0, int w, int h, uint32_t dest, uint32_t bytes);
} GxrDrawExport;

void gxr_set_draw_export(const GxrDrawExport* e); /* gxr.c; NULL: none */
/* gxr_export.c: sets the exporter when SOA_GXR_EXPORT names a directory. */
void gxr_export_install(void);

#endif
