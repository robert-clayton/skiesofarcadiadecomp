/*
 * The Ninja model layer, seen from the runtime (soa-ue5's graphics work).
 * Sega's Ninja library draws a chunk model with one of four functions of
 * one shape -- 0x802873BC, 0x80287C14, 0x802890A4 and 0x8028AB48, each
 * with its own polygon walker; Ninja's njCnkDrawModel and its easy, simple
 * and direct siblings, by the look of them. The argument is the
 * NJS_CNK_MODEL -- vlist, plist, centre, radius -- and the matrix on top of
 * Ninja's stack is that model's modelview, the camera and every parent's
 * transform already folded in; stack level 1 is the camera itself. The
 * library then moves the vertices into view space itself, so the GPU only
 * ever sees the result (docs/graphics-spike.md in soa-ue5). Hooks
 * (config/hooks.txt) catch each drawer on the way in and out, and the
 * texture chunk's handler as a model selects its textures.
 *
 * What they see goes two ways, each off unless asked for: the draw
 * exporter's stream (gxr_export.h), as "model", "model_end" and "tex" lines
 * between the draws; and a feed (ninja.h), which host.c gives an embedding
 * app.
 *
 * Not every strip is drawn while its drawer runs. A drawer draws the strips
 * its mode names (NJ_DRAW_MODE below), and the game hands most models over
 * twice: for the opaque strips, which go straight to the GPU, and for the
 * ones that use alpha, which it has GX record into a display list
 * (GXBeginDisplayList; gx.c models the recording) and calls later in an
 * order of its own. So each visit that recorded anything is remembered
 * with the list it wrote into and the bytes it wrote, and when that list is
 * called the parser is made to stop at each one's first and last byte
 * (gx.c's list hooks): the draws in between are that visit's, and GX's
 * state at the end is how it was shaded.
 * The stream gets "list", "model_sent" and "model_sent_end" lines for it.
 */
#include "cpu.h"
#include "gxr.h"
#include "gxr_export.h"
#include "ninja.h"
#include <stdio.h>
#include <string.h>

/* r13 is the small-data base, 0x8034E720 in this executable
 * (__init_registers); Ninja's matrix stack state is r13-relative. */
#define NJ_DEPTH   0x80347EA8u /* r13-26744: matrices pushed */
#define NJ_CURRENT 0x80347EB4u /* r13-26732: the current 3x4 matrix, 48 bytes */
/* Ninja's draw state (fn_8029D0F4 reads it for a texture chunk): +68 is the
 * current NJS_TEXLIST, whose entries are {name, attr, memlist}; a memlist's
 * +12 is the texture's info, with the GX format at +4, a palette's colour
 * format at +12, the size at +16 and +20 and the image at +32. A palettized
 * image (C4, C8) starts with its palette -- 16 or 256 two-byte entries --
 * and its texels follow, which is where GX is pointed. */
#define NJ_TEXLIST 0x803457ECu
/* Which of a model's strips a drawer draws (r13-31952; ninja.h's
 * NINJA_MODE_*). Each drawer turns it into the pass or two it runs its
 * polygon walker for (r13-26880: 0 every strip, 1 the opaque ones, 2 to 4
 * the ones that use alpha), and a strip emitter picks its cull mode by the
 * pass. Any other value draws nothing. */
#define NJ_DRAW_MODE 0x80346A50u
/* Ninja's 3D control flags (r13-26676): with 0x800 set, the polygon walkers
 * and a strip chunk's header (flags << 8 | type) with the word at r13-26680
 * and or it with the one at r13-26684 before they read its flags -- Ninja's
 * constant attribute. */
#define NJ_CONTROL 0x80347EECu
#define NJ_ATTR_AND 0x80347EE8u
#define NJ_ATTR_OR 0x80347EE4u

/* The visits of the frame being drawn whose strips went into a display
 * list: which list, and the bytes each wrote, until that list is called or
 * recorded over. */
typedef struct {
    uint32_t visit, list, begin, end;
    int sent;
} Recorded;
#define MAX_RECORDED 8192
static Recorded g_rec[MAX_RECORDED];
static unsigned g_nrec;
static unsigned long long g_rec_dropped;
static unsigned g_frame = ~0u, g_visit; /* the frame the visits are numbered in, and the next number */
static uint32_t g_list;                 /* the list being recorded, or the last that was */

/* The drawers that have not returned: none nests in what has been seen,
 * but an end must still find its own beginning. */
typedef struct {
    uint32_t visit, at;
    int recording;
    unsigned long long draws;
} Open;
#define MAX_OPEN 8
static Open g_open[MAX_OPEN];
static unsigned g_nopen;

static const NinjaFeed* g_feed;

void ninja_set_feed(const NinjaFeed* feed)
{
    g_feed = feed;
}

static float f32(CpuState* s, uint32_t ea)
{
    uint32_t v = mem_r32(s, ea);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static void read_matrix(CpuState* s, uint32_t ea, float out[12])
{
    int i;
    for (i = 0; i < 12; i++) out[i] = f32(s, ea + 4u * (unsigned)i);
}

static void matrix(CpuState* s, uint32_t ea, char* out, size_t cap)
{
    int i, n = 0;
    n += snprintf(out + n, cap - (size_t)n, "[");
    for (i = 0; i < 12 && n > 0 && (size_t)n < cap; i++)
        n += snprintf(out + n, cap - (size_t)n, "%s%.9g", i ? "," : "", f32(s, ea + 4u * (unsigned)i));
    if (n > 0 && (size_t)n < cap) snprintf(out + n, cap - (size_t)n, "]");
}

static int in_ram(uint32_t a)
{
    return a >= 0x80000000u && a < 0x81800000u - 64u;
}

/* GXBeginDisplayList: whatever the buffer at addr held is gone, the visits
 * recorded into it and never called among it. */
static void list_recording(CpuState* s, uint32_t addr)
{
    unsigned i;
    (void)s;
    g_list = addr;
    for (i = 0; i < g_nrec; i++)
        if (g_rec[i].list == addr) g_rec[i].sent = 1;
}

/* A display list is called (gx.c asks before parsing it, and again at each
 * point this names): stop the parse where each visit recorded in it begins
 * and ends. */
static uint32_t list_called(CpuState* s, uint32_t addr, uint32_t size, uint32_t done)
{
    static uint16_t in_list[MAX_RECORDED]; /* the visits in this list, by where they begin */
    static unsigned n, cur;
    static int inside;
    static unsigned long long draws;
    uint32_t at = addr + done, end = addr + size;
    int notes = gxr_export_wants_notes();
    char line[96];
    if (!done) {
        unsigned i, k;
        n = cur = 0;
        inside = 0;
        if (gx_frame_count() != g_frame) g_nrec = 0; /* last frame's: their list was never called */
        for (i = 0; i < g_nrec; i++) {
            const Recorded* r = &g_rec[i];
            if (r->sent || r->list != addr || r->begin < addr || r->end > end) continue;
            for (k = n++; k > 0 && g_rec[in_list[k - 1]].begin > r->begin; k--) in_list[k] = in_list[k - 1];
            in_list[k] = (uint16_t)i;
        }
        if (notes) {
            snprintf(line, sizeof line, "{\"kind\":\"list\",\"addr\":%u,\"size\":%u,\"visits\":%u}", addr, size, n);
            gxr_export_note(line);
        }
    }
    while (cur < n) {
        Recorded* r = &g_rec[in_list[cur]];
        if (!inside) {
            if (at < r->begin) return r->begin - at;
            inside = 1;
            draws = gx_draw_count();
            if (notes) {
                snprintf(line, sizeof line, "{\"kind\":\"model_sent\",\"visit\":%u}", r->visit);
                gxr_export_note(line);
            }
        }
        if (at < r->end) return r->end - at;
        if (g_feed && g_feed->sent) g_feed->sent(s, r->visit, (unsigned)(gx_draw_count() - draws));
        if (notes) gxr_export_note("{\"kind\":\"model_sent_end\"}");
        r->sent = 1;
        inside = 0;
        cur++;
    }
    return 0; /* the rest of the list in one piece */
}

/* A drawer's entry: r3 is the model; `drawer` says which of the four. */
static void model_begin(CpuState* s, unsigned drawer)
{
    static int hooked;
    uint32_t model = s->gpr[3], top = mem_r32(s, NJ_CURRENT), depth = mem_r32(s, NJ_DEPTH), k;
    NinjaVisit v;
    Open o;
    if (!hooked) {
        gx_set_list_hooks(list_recording, list_called);
        hooked = 1;
    }
    if (gx_frame_count() != g_frame) {
        g_frame = gx_frame_count();
        g_visit = g_nrec = 0;
    }
    o.visit = g_visit++;
    o.recording = gx_list_recording(&o.at);
    o.draws = gx_draw_count();
    if (g_nopen < MAX_OPEN) g_open[g_nopen] = o;
    g_nopen++;
    v.visit = o.visit;
    v.drawer = drawer;
    v.mode = mem_r32(s, NJ_DRAW_MODE);
    v.strip_and = 0xFFu;
    v.strip_or = 0;
    if (mem_r32(s, NJ_CONTROL) & 0x800u) {
        v.strip_and = (mem_r32(s, NJ_ATTR_AND) >> 8) & 0xFFu;
        v.strip_or = (mem_r32(s, NJ_ATTR_OR) >> 8) & 0xFFu;
    }
    v.model = model;
    v.vlist = mem_r32(s, model);
    v.plist = mem_r32(s, model + 4);
    if (g_feed && in_ram(top) && in_ram(top - 48u * (depth ? depth - 1u : 0u))) {
        read_matrix(s, top, v.modelview);
        if (depth >= 2) read_matrix(s, top - 48u * (depth - 1u), v.camera);
        else memcpy(v.camera, v.modelview, sizeof v.camera);
        g_feed->model(s, &v);
    }
    if (gxr_export_wants_notes()) {
        /* Every level of the stack under the top, so where the camera sits
         * can be read off the data rather than assumed: a model's own
         * transforms are pushed above it. */
        static char line[32768];
        char cur[400];
        int n;
        matrix(s, top, cur, sizeof cur);
        n = snprintf(line, sizeof line,
                     "{\"kind\":\"model\",\"drawer\":%u,\"frame\":%u,\"model\":%u,\"vlist\":%u,\"plist\":%u,\"centre\":[%.9g,%.9g,%.9g],"
                     "\"r\":%.9g,\"depth\":%u,\"top\":%u,\"texlist\":%u,\"mtx\":%s,\"lr\":%u,\"visit\":%u,\"mode\":%u,"
                     "\"strip_and\":%u,\"strip_or\":%u,\"stack\":[",
                     drawer, gx_frame_count(), model, v.vlist, v.plist, f32(s, model + 8), f32(s, model + 12),
                     f32(s, model + 16), f32(s, model + 20), depth, top, mem_r32(s, NJ_TEXLIST), cur, s->lr, v.visit, v.mode,
                     v.strip_and, v.strip_or);
        for (k = 1; k <= depth && k <= 64 && n > 0 && (size_t)n < sizeof line - 500; k++) {
            char m[400];
            matrix(s, top - 48u * k, m, sizeof m);
            n += snprintf(line + n, sizeof line - (size_t)n, "%s%s", k > 1 ? "," : "", m);
        }
        if (n > 0 && (size_t)n < sizeof line - 4) snprintf(line + n, sizeof line - (size_t)n, "]}");
        gxr_export_note(line);
    }
}

/* The one epilogue each drawer's returns pass through. */
static void model_end(CpuState* s)
{
    char line[96];
    Open o;
    uint32_t at;
    int recorded = 0;
    unsigned strips;
    if (!g_nopen) return; /* the hooks came on inside a drawer */
    if (--g_nopen >= MAX_OPEN) return;
    o = g_open[g_nopen];
    /* What it wrote while GX was recording is in the list, not drawn. */
    if (o.recording && gx_list_recording(&at) && at != o.at) {
        if (g_nrec < MAX_RECORDED) {
            Recorded* r = &g_rec[g_nrec++];
            r->visit = o.visit;
            r->list = g_list;
            r->begin = o.at;
            r->end = at;
            r->sent = 0;
            recorded = 1;
        } else if (g_rec_dropped++ == 0) {
            fprintf(stderr, "[ninja] more than %u models recorded into display lists in a frame; the rest are not followed\n",
                    MAX_RECORDED);
        }
    }
    strips = (unsigned)(gx_draw_count() - o.draws);
    if (g_feed && g_feed->end) g_feed->end(s, o.visit, (int32_t)s->gpr[3] != -1, recorded, strips);
    if (!gxr_export_wants_notes()) return;
    snprintf(line, sizeof line, "{\"kind\":\"model_end\",\"ret\":%d,\"texlist\":%u,\"recorded\":%d}", (int)s->gpr[3],
             mem_r32(s, NJ_TEXLIST), recorded);
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
void hook_8029D0F4(CpuState* s)
{
    char name[33] = "";
    uint32_t id = s->gpr[4] & 0x1FFFu, list = mem_r32(s, NJ_TEXLIST), arr = 0, count = 0, entry = 0, info = 0;
    int notes = gxr_export_wants_notes();
    if (!g_feed && !notes) return;
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
    if (!in_ram(info)) info = 0;
    if (g_feed && info)
        g_feed->texture(s, id, mem_r32(s, info + 32), mem_r32(s, info + 4), mem_r32(s, info + 12), mem_r32(s, info + 16),
                        mem_r32(s, info + 20), name);
    if (notes) {
        char line[256];
        if (info)
            snprintf(line, sizeof line,
                     "{\"kind\":\"tex\",\"id\":%u,\"texlist\":%u,\"name\":\"%s\",\"fmt\":%u,\"tlut\":%u,\"w\":%u,\"h\":%u,"
                     "\"img\":%u}",
                     id, list, name, mem_r32(s, info + 4), mem_r32(s, info + 12), mem_r32(s, info + 16), mem_r32(s, info + 20),
                     mem_r32(s, info + 32));
        else
            snprintf(line, sizeof line, "{\"kind\":\"tex\",\"id\":%u,\"texlist\":%u}", id, list);
        gxr_export_note(line);
    }
}
