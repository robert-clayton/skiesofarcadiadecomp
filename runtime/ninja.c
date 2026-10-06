/*
 * The Ninja model layer, seen from the runtime (soa-ue5's graphics work).
 * Sega's Ninja library draws a chunk model with one of four functions of
 * one shape -- 0x802873BC, 0x80287C14, 0x802890A4 and 0x8028AB48, each
 * with its own polygon walker; Ninja's njCnkDrawModel and its easy, simple
 * and direct siblings, by the look of them. The argument is the
 * NJS_CNK_MODEL -- vlist, plist, centre, radius -- and
 * the matrix on top of Ninja's stack is that model's modelview, the camera
 * and every parent's transform already folded in. The library then moves
 * the vertices into view space itself, so the GPU only ever sees the
 * result (docs/graphics-spike.md in soa-ue5). Two hooks (config/hooks.txt)
 * catch the call: on the way in, the model and the matrices; on the way
 * out, its end, so every GX draw in between is known to be that model's.
 *
 * What they record goes to the draw exporter's stream (gxr_export.h), as
 * "model" and "model_end" lines between the draws.
 */
#include "cpu.h"
#include "gxr_export.h"
#include <stdio.h>
#include <string.h>

/* r13 is the small-data base, 0x8034E720 in this executable
 * (__init_registers); Ninja's matrix stack state is r13-relative. */
#define NJ_DEPTH   0x80347EA8u /* r13-26744: matrices pushed */
#define NJ_CURRENT 0x80347EB4u /* r13-26732: the current 3x4 matrix, 48 bytes */

static float f32(CpuState* s, uint32_t ea)
{
    uint32_t v = mem_r32(s, ea);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static void matrix(CpuState* s, uint32_t ea, char* out, size_t cap)
{
    int i, n = 0;
    n += snprintf(out + n, cap - (size_t)n, "[");
    for (i = 0; i < 12 && n > 0 && (size_t)n < cap; i++)
        n += snprintf(out + n, cap - (size_t)n, "%s%.9g", i ? "," : "", f32(s, ea + 4u * (unsigned)i));
    if (n > 0 && (size_t)n < cap) snprintf(out + n, cap - (size_t)n, "]");
}

/* A drawer's entry: r3 is the model; `drawer` says which of the four. */
static void model_begin(CpuState* s, unsigned drawer)
{
    /* Every level of the stack under the top, so where the camera sits can
     * be read off the data rather than assumed: a model's own transforms are
     * pushed above it. */
    char line[32768], cur[400];
    uint32_t model = s->gpr[3], top = mem_r32(s, NJ_CURRENT), depth = mem_r32(s, NJ_DEPTH), k;
    int n;
    if (!gxr_export_wants_notes()) return;
    matrix(s, top, cur, sizeof cur);
    n = snprintf(line, sizeof line,
                 "{\"kind\":\"model\",\"drawer\":%u,\"frame\":%u,\"model\":%u,\"vlist\":%u,\"plist\":%u,\"centre\":[%.9g,%.9g,%.9g],"
                 "\"r\":%.9g,\"depth\":%u,\"top\":%u,\"mtx\":%s,\"lr\":%u,\"stack\":[",
                 drawer, gx_frame_count(), model, mem_r32(s, model), mem_r32(s, model + 4), f32(s, model + 8), f32(s, model + 12),
                 f32(s, model + 16), f32(s, model + 20), depth, top, cur, s->lr);
    for (k = 1; k <= depth && k <= 64 && n > 0 && (size_t)n < sizeof line - 500; k++) {
        char m[400];
        matrix(s, top - 48u * k, m, sizeof m);
        n += snprintf(line + n, sizeof line - (size_t)n, "%s%s", k > 1 ? "," : "", m);
    }
    if (n > 0 && (size_t)n < sizeof line - 4) snprintf(line + n, sizeof line - (size_t)n, "]}");
    gxr_export_note(line);
}

/* The one epilogue each drawer's returns pass through. */
static void model_end(CpuState* s)
{
    char line[64];
    if (!gxr_export_wants_notes()) return;
    snprintf(line, sizeof line, "{\"kind\":\"model_end\",\"ret\":%d}", (int)s->gpr[3]);
    gxr_export_note(line);
}

void hook_802873BC(CpuState* s) { model_begin(s, 0); }
void hook_80287C14(CpuState* s) { model_begin(s, 1); }
void hook_802890A4(CpuState* s) { model_begin(s, 2); }
void hook_8028AB48(CpuState* s) { model_begin(s, 3); }
void hook_802875FC(CpuState* s) { model_end(s); }
void hook_80287D80(CpuState* s) { model_end(s); }
void hook_80289210(CpuState* s) { model_end(s); }
void hook_8028AD88(CpuState* s) { model_end(s); }
