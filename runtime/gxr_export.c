/*
 * The draw exporter: see gxr_export.h. Runs on the producer, which is the
 * only thread that builds commands, so nothing here is locked.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "gxr_export.h"
#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_on = -1;
static char g_dir[900];
static FILE *g_json, *g_verts;
static unsigned long long g_nverts;
static float* g_buf;
static unsigned g_cap;
static unsigned long long g_draws, g_copies, g_textures, g_notes;
static unsigned g_first, g_last = ~0u, g_step = 1; /* SOA_GXR_EXPORT_FRAMES */

#define SEEN_CAP 8192
static uint64_t g_seen[SEEN_CAP];

static float xff(const uint32_t* xf, unsigned i)
{
    float f;
    memcpy(&f, &xf[i], 4);
    return f;
}

static void report(void)
{
    if (g_json) fflush(g_json);
    if (g_verts) fflush(g_verts);
    fprintf(stderr, "[export] %llu draws, %llu copies, %llu notes, %llu vertices and %llu textures into %s\n", g_draws,
            g_copies, g_notes, g_nverts, g_textures, g_dir);
}

static void export_vertex(unsigned i, const VertexIn* in, const Vertex* out);
static void export_draw(const DrawCmd* D, unsigned count);
static void export_copy(uint32_t v, int x0, int y0, int w, int h, uint32_t dest, uint32_t bytes);
static const GxrDrawExport g_hooks = {export_vertex, export_draw, export_copy};

void gxr_export_install(void)
{
    if (g_on < 0) {
        const char* d = getenv("SOA_GXR_EXPORT");
        char path[1000];
        g_on = 0;
        if (!d || !*d) return;
        snprintf(g_dir, sizeof g_dir, "%s", d);
        {
            const char* fr = getenv("SOA_GXR_EXPORT_FRAMES");
            if (fr && *fr && sscanf(fr, "%u-%u/%u", &g_first, &g_last, &g_step) < 1)
                fprintf(stderr, "[export] SOA_GXR_EXPORT_FRAMES=%s is not first-last[/step]; every frame is exported\n", fr);
            if (!g_step) g_step = 1;
        }
        plat_mkdir(g_dir);
        snprintf(path, sizeof path, "%s/tex", g_dir);
        plat_mkdir(path);
        snprintf(path, sizeof path, "%s/draws.jsonl", g_dir);
        g_json = fopen(path, "w");
        snprintf(path, sizeof path, "%s/verts.bin", g_dir);
        g_verts = fopen(path, "wb");
        if (!g_json || !g_verts) {
            fprintf(stderr, "[export] cannot write into %s; nothing is exported\n", g_dir);
            return;
        }
        atexit(report);
        g_on = 1;
        gxr_set_draw_export(&g_hooks);
    }
}

static void export_vertex(unsigned i, const VertexIn* in, const Vertex* out)
{
    const uint32_t* xf = gx_xf_regs();
    unsigned m = 4 * (in->posidx & 0x3F), nb = 0x400 + 3 * (in->posidx & 0x3F), r, k;
    float* f;
    if (i >= g_cap) {
        unsigned cap = g_cap ? g_cap * 2 : 4096;
        while (cap <= i) cap *= 2;
        g_buf = (float*)realloc(g_buf, (size_t)cap * GXR_EXPORT_FLOATS * sizeof(float));
        g_cap = cap;
    }
    f = g_buf + (size_t)i * GXR_EXPORT_FLOATS;
    for (r = 0; r < 3; r++) /* the view-space position and normal, as transform() makes them */
        f[r] = xff(xf, m + 4 * r) * in->pos[0] + xff(xf, m + 4 * r + 1) * in->pos[1] + xff(xf, m + 4 * r + 2) * in->pos[2] +
               xff(xf, m + 4 * r + 3);
    for (r = 0; r < 3; r++)
        f[3 + r] = in->has_nrm ? xff(xf, nb + 3 * r) * in->nrm[0] + xff(xf, nb + 3 * r + 1) * in->nrm[1] +
                                     xff(xf, nb + 3 * r + 2) * in->nrm[2]
                               : 0.0f;
    memcpy(f + 6, in->pos, 3 * sizeof(float));
    f[9] = (float)(in->posidx & 0x3F);
    f[10] = out->x; f[11] = out->y; f[12] = out->z; f[13] = out->w;
    for (k = 0; k < 2; k++) {
        f[14 + 4 * k] = out->col[k].r; f[15 + 4 * k] = out->col[k].g;
        f[16 + 4 * k] = out->col[k].b; f[17 + 4 * k] = out->col[k].a;
    }
    for (k = 0; k < 8; k++) memcpy(f + 22 + 3 * k, out->tex[k], 3 * sizeof(float));
}

static uint64_t fnv(uint64_t h, const uint8_t* p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}

/* The texture's level 0 as tex/<hash>.png, once; its hash. */
static uint64_t texture_file(const TexCfg* T)
{
    const uint8_t* px = T->level[0];
    int w = T->lw[0], h = T->lh[0];
    uint8_t dim[8];
    uint64_t hash;
    unsigned slot;
    memcpy(dim, &w, 4);
    memcpy(dim + 4, &h, 4);
    hash = fnv(fnv(0xCBF29CE484222325ull, dim, 8), px, (size_t)w * h * 4);
    if (!hash) hash = 1;
    for (slot = (unsigned)(hash % SEEN_CAP); g_seen[slot]; slot = (slot + 1) % SEEN_CAP)
        if (g_seen[slot] == hash) return hash;
    g_seen[slot] = hash;
    {
        char path[1100];
        snprintf(path, sizeof path, "%s/tex/%016llx.png", g_dir, (unsigned long long)hash);
        if (png_write_rgba(path, px, w, h, w * 4)) g_textures++;
        else fprintf(stderr, "[export] cannot write %s\n", path);
    }
    return hash;
}

static int in_range(void)
{
    unsigned f = gx_frame_count();
    return f >= g_first && f <= g_last && (f - g_first) % g_step == 0;
}

int gxr_export_wants_notes(void)
{
    return g_on > 0 && in_range();
}

void gxr_export_note(const char* json)
{
    if (!gxr_export_wants_notes()) return;
    fprintf(g_json, "%s\n", json);
    g_notes++;
}

/* Where texture map m's image is in guest memory: BP's texture image
 * register 3 (0x94 for maps 0-3, 0xB4 for 4-7), in 32-byte units. */
static uint32_t image_addr(unsigned m)
{
    const uint32_t* bp = gx_bp_regs();
    uint32_t reg = (m < 4 ? 0x94u + m : 0xB4u + (m - 4)) & 0xFFu;
    return 0x80000000u | ((bp[reg] & 0x00FFFFFFu) << 5);
}

static void export_draw(const DrawCmd* D, unsigned count)
{
    const uint32_t* xf = gx_xf_regs();
    const TevSetup* T = &D->tev;
    unsigned i, k, maps = 0;
    uint64_t used_mtx = 0;
    if (!in_range()) return;
    fwrite(g_buf, sizeof(float) * GXR_EXPORT_FLOATS, count, g_verts);
    for (i = 0; i < count; i++) used_mtx |= 1ull << (unsigned)g_buf[(size_t)i * GXR_EXPORT_FLOATS + 9];
    fprintf(g_json, "{\"kind\":\"draw\",\"n\":%llu,\"frame\":%u,\"prim\":%u,\"count\":%u,\"vofs\":%llu,\"efb\":%u,",
            g_draws, gx_frame_count(), D->prim, count, g_nverts, D->efb);
    fprintf(g_json, "\"ortho\":%u,\"proj\":[%.9g,%.9g,%.9g,%.9g,%.9g,%.9g],", xf[0x1026] & 1, xff(xf, 0x1020), xff(xf, 0x1021),
            xff(xf, 0x1022), xff(xf, 0x1023), xff(xf, 0x1024), xff(xf, 0x1025));
    fprintf(g_json, "\"vp\":[%.9g,%.9g,%.9g,%.9g,%.9g,%.9g],\"scissor\":[%d,%d,%d,%d],\"cull\":%u,", D->rc.wd, D->rc.ht,
            D->rc.zrange, D->rc.xorig, D->rc.yorig, D->rc.farz, D->rc.scissor.x0, D->rc.scissor.y0, D->rc.scissor.x1,
            D->rc.scissor.y1, D->rc.cull);
    fprintf(g_json, "\"mtx\":{");
    for (i = 0, k = 0; i < 64; i++) {
        unsigned r;
        if (!((used_mtx >> i) & 1)) continue;
        fprintf(g_json, "%s\"%u\":[", k++ ? "," : "", i);
        for (r = 0; r < 12; r++) fprintf(g_json, "%s%.9g", r ? "," : "", xff(xf, 4 * i + r));
        fprintf(g_json, "]");
    }
    fprintf(g_json, "},");
    fprintf(g_json,
            "\"blend\":{\"en\":%d,\"logic\":%d,\"sfac\":%u,\"dfac\":%u,\"sub\":%d,\"lop\":%u,\"col_upd\":%d,\"alpha_upd\":%d,"
            "\"const_alpha\":%d},\"z\":{\"en\":%d,\"upd\":%d,\"func\":%u,\"early\":%d},\"fog\":%u,",
            D->px.blend_en, D->px.logic_en, D->px.sfac, D->px.dfac, D->px.subtract, D->px.lop, D->px.col_upd,
            D->px.alpha_upd, D->px.const_alpha, D->px.z_en, D->px.z_upd, D->px.z_func, D->px.ztop, D->px.fog_type);
    fprintf(g_json,
            "\"tev\":{\"stages\":%u,\"fast_c\":%u,\"fast_a\":%u,\"used_tex\":%u,\"used_chan\":%u,"
            "\"alpha\":[%d,%d,%u,%u,%u,%d],\"regs\":[",
            T->stages, T->fast_c, T->fast_a, T->used_tex, T->used_chan, T->aref0, T->aref1, T->acomp0, T->acomp1,
            T->alogic, T->alpha_always);
    for (i = 0; i < 4; i++)
        fprintf(g_json, "%s[%d,%d,%d,%d]", i ? "," : "", T->reg_init[i][0], T->reg_init[i][1], T->reg_init[i][2],
                T->reg_init[i][3]);
    fprintf(g_json, "],\"st\":[");
    for (i = 0; i < T->stages; i++) {
        const Stage* S = &T->st[i];
        if (S->texen) maps |= 1u << S->texmap;
        fprintf(g_json,
                "%s{\"texmap\":%u,\"texcoord\":%u,\"texen\":%u,\"chan\":%u,\"c\":[%u,%u,%u,%u],\"a\":[%u,%u,%u,%u],"
                "\"cop\":%u,\"aop\":%u,\"cbias\":%u,\"abias\":%u,\"cshift\":%u,\"ashift\":%u,\"cclamp\":%u,"
                "\"aclamp\":%u,\"cdest\":%u,\"adest\":%u,\"konst\":[%d,%d,%d,%d]}",
                i ? "," : "", S->texmap, S->texcoord, S->texen, S->chan, S->ca, S->cb, S->cc, S->cd, S->aa, S->ab,
                S->ac, S->ad, S->cop, S->aop, S->cbias, S->abias, S->cshift, S->ashift, S->cclamp, S->aclamp, S->cdest,
                S->adest, S->konst[0], S->konst[1], S->konst[2], S->konst[3]);
    }
    fprintf(g_json, "]},\"tex\":[");
    for (i = 0, k = 0; i < 8; i++) {
        const TexCfg* C = &T->tex[i];
        if (!((maps >> i) & 1) || !C->level[0]) continue;
        fprintf(g_json,
                "%s{\"map\":%u,\"file\":\"%016llx.png\",\"w\":%d,\"h\":%d,\"wrap_s\":%u,\"wrap_t\":%u,\"linear\":%d,"
                "\"mip\":%d,\"copy\":%u,\"scale\":[%.9g,%.9g],\"addr\":%u}",
                k++ ? "," : "", i, (unsigned long long)texture_file(C), C->lw[0], C->lh[0], C->wrap_s, C->wrap_t,
                C->linear, C->mip, C->copy_image, C->scale_s, C->scale_t, image_addr(i));
    }
    fprintf(g_json, "]}\n");
    g_nverts += count;
    g_draws++;
}

static void export_copy(uint32_t v, int x0, int y0, int w, int h, uint32_t dest, uint32_t bytes)
{
    if (!in_range()) return;
    fprintf(g_json,
            "{\"kind\":\"copy\",\"n\":%llu,\"frame\":%u,\"after_draw\":%llu,\"to_screen\":%u,\"clear\":%u,\"rect\":[%d,%d,%d,%d],"
            "\"dest\":%u,\"bytes\":%u,\"cmd\":%u}\n",
            g_copies, gx_frame_count(), g_draws, (v & 0x4000u) != 0, (v & 0x800u) != 0, x0, y0, w, h, dest, bytes, v);
    g_copies++;
}
