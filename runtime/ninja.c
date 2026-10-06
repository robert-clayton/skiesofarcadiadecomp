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
/* Ninja's draw state (fn_8029D0F4 reads it for a texture chunk): +68 is the
 * current NJS_TEXLIST, whose entries are {name, attr, memlist}; a memlist's
 * +12 is the texture's info, with the GX format at +4, the size at +16 and
 * +20 and the image at +32. */
#define NJ_TEXLIST 0x803457ECu

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
                 "\"r\":%.9g,\"depth\":%u,\"top\":%u,\"texlist\":%u,\"mtx\":%s,\"lr\":%u,\"stack\":[",
                 drawer, gx_frame_count(), model, mem_r32(s, model), mem_r32(s, model + 4), f32(s, model + 8), f32(s, model + 12),
                 f32(s, model + 16), f32(s, model + 20), depth, top, mem_r32(s, NJ_TEXLIST), cur, s->lr);
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
    snprintf(line, sizeof line, "{\"kind\":\"model_end\",\"ret\":%d,\"texlist\":%u}", (int)s->gpr[3], mem_r32(s, NJ_TEXLIST));
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

/* The texture chunk's handler (fn_8029D0F4): r4's low 13 bits are the texture
 * id, looked up in the texture list current right now. That list is often
 * built on the stack for the object being drawn, so it has to be read here,
 * not from memory afterwards: the entry's name, and from its info the GX
 * format, size and image, which live on the heap and stay. */
static int in_ram(uint32_t a)
{
    return a >= 0x80000000u && a < 0x81800000u - 64u;
}

void hook_8029D0F4(CpuState* s)
{
    char line[256], name[33] = "";
    uint32_t id = s->gpr[4] & 0x1FFFu, list = mem_r32(s, NJ_TEXLIST), arr = 0, count = 0, entry = 0, info = 0;
    if (!gxr_export_wants_notes()) return;
    if (in_ram(list)) { arr = mem_r32(s, list); count = mem_r32(s, list + 4); }
    if (in_ram(arr) && id < count) entry = arr + 12u * id;
    if (entry) {
        uint32_t str = mem_r32(s, entry), mem = mem_r32(s, entry + 8), k;
        if (in_ram(mem)) info = mem_r32(s, mem + 12);
        if (in_ram(str))
            for (k = 0; k < 32; k++) {
                char c = (char)mem_r8(s, str + k);
                if (!c || c == '"' || c == '\\' || (unsigned char)c < 32) break;
                name[k] = c;
                name[k + 1] = 0;
            }
    }
    if (in_ram(info))
        snprintf(line, sizeof line,
                 "{\"kind\":\"tex\",\"id\":%u,\"texlist\":%u,\"name\":\"%s\",\"fmt\":%u,\"w\":%u,\"h\":%u,\"img\":%u}", id,
                 list, name, mem_r32(s, info + 4), mem_r32(s, info + 16), mem_r32(s, info + 20), mem_r32(s, info + 32));
    else
        snprintf(line, sizeof line, "{\"kind\":\"tex\",\"id\":%u,\"texlist\":%u}", id, list);
    gxr_export_note(line);
}
