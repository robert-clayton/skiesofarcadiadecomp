"""Build the render queue's two drivers on their own and hold every thread
count and stall to the one-worker run (portability.md L8).

    python tools/citest/queue_check.py [--cc PROFILE] [--threads 1,2,3,4]
                                       [--cflag FLAG]... [--out DIR]

The drivers are the ones tools/tests/test_gxr_queue.py and test_gxr_overlap.py
build under MSVC x64 on every push. They live here, once, so that this script
can build them under any toolchain profile -- gcc and clang, and under
ThreadSanitizer with --cflag -fsanitize=thread -- and on any machine, the
ARM64 runner among them, where a load the queue forgot to order is a wrong
pixel rather than a lucky one. What it repeats of the tests is the point:
the other compilers and the other hardware.

The queue driver draws ten frames at each thread count, and every count must
hash them alike. The overlap driver (H14's copies) runs each thread count
unstalled and with each stall kind -- one worker held back before its draws,
its copies or its clears -- and every run must leave the one-worker run's
copied memory, screen, EFB and decoded textures. Synthetic streams, no disc
and no game data. Exits non-zero on any mismatch, a build failure, a driver
that exits non-zero (ThreadSanitizer's report does), or a missing compiler.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from soa import toolchain  # noqa: E402

RUNTIME = ROOT / "runtime"
STALLS = ("2:0:300", "2:1:20000", "2:2:20000")  # worker 2 held before draws, copies, clears

# ---- test_gxr_overlap.py's driver: the ordering around EFB copies (H14) ----

OVERLAP_SOURCES = ["gx.c", "gx_frame.c", "gxr.c", "png.c"]  # gxr_tev.c comes in through the driver

OVERLAP_DRIVER = r"""
#define _CRT_SECURE_NO_WARNINGS
#include "gxr_tev.c" /* the texture cache's statics, to hash its decodes */
#ifdef OVERLAP_GPU
#include "gxv.h"
#endif

uint32_t mmio_read32(CpuState* s, uint32_t ea) { (void)s; (void)ea; return 0; }
void hle_report(void) {}

static void gp8(CpuState* s, unsigned v) { gx_pipe_write(s, 1, v); }
static void gp16(CpuState* s, unsigned v) { gx_pipe_write(s, 2, v); }
static void gp32(CpuState* s, uint32_t v) { gx_pipe_write(s, 4, v); }
static void gpf(CpuState* s, float f) { uint32_t u; memcpy(&u, &f, 4); gp32(s, u); }
static void bp_w(CpuState* s, unsigned reg, uint32_t v) { gp8(s, 0x61); gp32(s, ((uint32_t)reg << 24) | (v & 0xFFFFFFu)); }
static void cp_w(CpuState* s, unsigned reg, uint32_t v) { gp8(s, 0x08); gp8(s, reg); gp32(s, v); }
static void xf_w(CpuState* s, unsigned addr, unsigned n, const uint32_t* w)
{
    unsigned i;
    gp8(s, 0x10); gp16(s, n - 1); gp16(s, addr);
    for (i = 0; i < n; i++) gp32(s, w[i]);
}
static void xf_f(CpuState* s, unsigned addr, unsigned n, const float* f)
{
    unsigned i;
    gp8(s, 0x10); gp16(s, n - 1); gp16(s, addr);
    for (i = 0; i < n; i++) gpf(s, f[i]);
}

#define W 128
#define H 96
#define FRAMES 6
#define A_BASE 0x00400000u /* frame f's first copy at A_BASE + f * SLOT */
#define B_BASE 0x00600000u
#define C_BASE 0x00700000u
#define D_BASE 0x00800000u
#define E_BASE 0x00900000u /* frame f's mipmapped texture at E_BASE + f * 0x40000 */
#define E_LEVEL1 196608u  /* 256 x 192 RGBA8: its level 1, 128 x 96, starts here */
#define SLOT 0x10000u      /* 128 x 96 RGBA8 is 48 KB */
#define G_BASE 0x00B00000u /* frame f's copy image overwritten in part, at G_BASE + f * SLOT */
#define K_BASE 0x00C00000u /* frame f's unfiltered copy, at K_BASE + f * SLOT */
#define M_BASE 0x00D00000u /* frame f's copy the CPU rewrites after GXDrawDone */
#define N_BASE 0x00E00000u /* frame f's copy a token separates from its draw */
#define Q_BASE 0x00F00000u /* frame f's copy a hook pokes */
#define C4_TEX 0x00300000u
#define XFB 0x00100000u

static void setup(CpuState* s)
{
    static const float viewport[6] = {320.0f, -240.0f, 16777215.0f, 662.0f, 582.0f, 16777215.0f};
    static const float ortho[6] = {0.003125f, -0.0f, 0.004167f, -0.0f, -0.01f, -1.0f};
    static const float view[12] = {1, 0, 0, -320, 0, -1, 0, 240, 0, 0, 1, -100};
    static const uint32_t one = 1, matidx = 0x3CF3CF00u, chan = 0x441u, zero = 0, texgen = 0;
    bp_w(s, 0x59, 0x02ACABu); bp_w(s, 0x20, 0x156156u); bp_w(s, 0x21, 0x3D5335u);
    xf_f(s, 0x101A, 6, viewport);
    xf_f(s, 0x1020, 6, ortho); xf_w(s, 0x1026, 1, &one);
    xf_f(s, 0x0000, 12, view);
    cp_w(s, 0x30, matidx); xf_w(s, 0x1018, 1, &matidx);
    xf_w(s, 0x1009, 1, &one); xf_w(s, 0x100E, 1, &chan); xf_w(s, 0x1010, 1, &chan);
    xf_w(s, 0x103F, 1, &one); xf_w(s, 0x1040, 1, &texgen); xf_w(s, 0x1008, 1, &one);
    (void)zero;
    bp_w(s, 0x28, 0); bp_w(s, 0xC0, 0x08AFFFu); bp_w(s, 0xC1, 0x08BFF0u); bp_w(s, 0x00, 0x000011u);
    bp_w(s, 0xF6, 0x018064u); bp_w(s, 0xF7, 0x01806Eu); bp_w(s, 0xF8, 0x018060u); bp_w(s, 0xF9, 0x01806Cu);
    bp_w(s, 0xFA, 0x018065u); bp_w(s, 0xFB, 0x01806Du); bp_w(s, 0xFC, 0x01806Au); bp_w(s, 0xFD, 0x01806Eu);
    bp_w(s, 0x40, 0x17); bp_w(s, 0x41, 0x18); bp_w(s, 0xF3, 0x3F0000u); bp_w(s, 0x43, 0x40);
    cp_w(s, 0x50, 0x2200); cp_w(s, 0x60, 0); cp_w(s, 0x70, 0x41377009u); cp_w(s, 0x80, 0xC8241209u); cp_w(s, 0x90, 0x04824120u);
    bp_w(s, 0x4E, 0x000100u);
    bp_w(s, 0x53, 0x30A208u); bp_w(s, 0x54, 0x00820Au); /* the game's deflicker: every copy reads three rows */
    bp_w(s, 0x4F, 0x00FF20u); bp_w(s, 0x50, 0x002040u); bp_w(s, 0x51, 0xFFFFFFu);
}

static uint32_t colour(int i, int f)
{
    return ((uint32_t)((i * 37 + f * 53) & 0xFF) << 24) | ((uint32_t)((i * 91 + f * 17) & 0xFF) << 16) |
           ((uint32_t)((i * 13 + f * 101) & 0xFF) << 8) | 0xFFu;
}

static void vertex(CpuState* s, float x, float y, uint32_t rgba) { gpf(s, x); gpf(s, y); gpf(s, 50.0f); gp32(s, rgba); }

static void quad(CpuState* s, float x0, float y0, float x1, float y1, uint32_t rgba)
{
    gp8(s, 0x80); gp16(s, 4);
    vertex(s, x0, y0, rgba); vertex(s, x1, y0, rgba); vertex(s, x1, y1, rgba); vertex(s, x0, y1, rgba);
}

static void untextured(CpuState* s) { bp_w(s, 0x28, 0); bp_w(s, 0xC0, 0x08AFFFu); }

/* Stage 0 outputs the texel, through a texture matrix that spreads the texture
 * over the quad, so what a draw sampled reaches the EFB. */
static void textured(CpuState* s, uint32_t addr, unsigned fmt, unsigned w, unsigned h, float x0, float y0, float qw, float qh)
{
    const float m[12] = {1.0f / qw, 0, 0, -x0 / qw, 0, 1.0f / qh, 0, -y0 / qh, 0, 0, 1, 0};
    xf_f(s, 240, 12, m); /* texture matrix 60, which MATIDX gives texcoord 0 */
    bp_w(s, 0x80, 0); bp_w(s, 0x84, 0);
    bp_w(s, 0x88, ((fmt & 15) << 20) | ((h - 1) << 10) | (w - 1));
    bp_w(s, 0x94, (addr >> 5) & 0x1FFFFFu);
    bp_w(s, 0x98, 0); /* TLUT at TMEM 0, IA8 */
    bp_w(s, 0x30, w - 1); bp_w(s, 0x31, h - 1);
    bp_w(s, 0x28, 0x40);
    bp_w(s, 0xC0, 0x088FFFu); /* the texel, not the vertex colour */
}

/* BP 0x4D is each copy's own row of tiles, in 32-byte units, as
 * GXSetTexCopyDst would set it: a copy into a texture its own width. */
static void copy_tex(CpuState* s, uint32_t dest)
{
    bp_w(s, 0x49, 0); bp_w(s, 0x4A, ((H - 1) << 10) | (W - 1)); bp_w(s, 0x4D, W / 4 * 64 / 32);
    bp_w(s, 0x4B, (dest >> 5) & 0x1FFFFFu);
    bp_w(s, 0x52, 0x000063u); /* RGBA8 to memory, no clear */
}

/* The first rows only, as RGB565: another size and format from copy_tex's,
 * over the first tile row of an RGBA8 copy at the same address. */
static void copy_rows(CpuState* s, uint32_t dest, unsigned rows)
{
    bp_w(s, 0x49, 0); bp_w(s, 0x4A, ((rows - 1) << 10) | (W - 1)); bp_w(s, 0x4D, W / 4 * 32 / 32);
    bp_w(s, 0x4B, (dest >> 5) & 0x1FFFFFu);
    bp_w(s, 0x52, 0x000043u); /* RGB565 to memory, no clear */
}

static void screen_copy(CpuState* s)
{
    bp_w(s, 0x49, 0); bp_w(s, 0x4A, ((H - 1) << 10) | (W - 1)); bp_w(s, 0x4D, 0x28);
    bp_w(s, 0x4B, (XFB >> 5) & 0x1FFFFFu);
    bp_w(s, 0x52, 0x004803u); /* to the screen, and clear */
}

static uint64_t fnv(uint64_t h, const uint8_t* p, size_t n)
{
    while (n--) { h ^= *p++; h *= 1099511628211ull; }
    return h;
}

/* Every level the cache holds for one texture a draw samples, found by its
 * key. By key and not by walking the cache: a copy image (FINDINGS "Copy
 * images") gives every copy's destination an entry whether or not a draw
 * samples it, which SOA_GXR_DRAIN does not, so what is compared is what the
 * draws sampled -- a copy's image under the default protocol, memory's decode
 * under the drains, the same pixels. */
static uint64_t decoded(uint64_t h, uint32_t addr, uint32_t fmt, uint32_t w, uint32_t ht)
{
    TexEntry key;
    const TexEntry* e;
    int l;
    memset(&key, 0, sizeof key);
    key.addr = addr & MEM_MASK; key.fmt = fmt; key.w = w; key.h = ht;
    e = tex_find(&key);
    if (!e || !e->rgba) return fnv(h, (const uint8_t*)"none", 4);
    for (l = 0; l < e->nlevels; l++) h = fnv(h, e->level[l], (size_t)e->lw[l] * e->lh[l] * 4);
    return h;
}

/* What the CPU reads of the fourth copy: after GXDrawDone, which a game
 * waits on before it reads what was drawn and which must find it finished;
 * and after a draw token, which finds it finished only with
 * SOA_GXR_TOKENWAIT=1 (H14 answers tokens as they are parsed). */
static uint64_t g_done = 1469598103934665603ull, g_cpu = 1469598103934665603ull;

static void frame(CpuState* s, int f)
{
    uint32_t a = A_BASE + (uint32_t)f * SLOT, b = B_BASE + (uint32_t)f * SLOT, c = C_BASE + (uint32_t)f * SLOT;
    uint32_t d = D_BASE + (uint32_t)f * SLOT, e = E_BASE + (uint32_t)f * 0x40000u, g = G_BASE + (uint32_t)f * SLOT;
    uint32_t k = K_BASE + (uint32_t)f * SLOT, m = M_BASE + (uint32_t)f * SLOT, n = N_BASE + (uint32_t)f * SLOT;
    uint32_t q = Q_BASE + (uint32_t)f * SLOT;
    int i;
    untextured(s);
    for (i = 0; i < 24; i++) {
        float x = (float)((i * 13 + f * 7) % (W - 20)), y = (float)((i * 29 + f * 11) % (H - 16));
        quad(s, x, y, x + 20, y + 14, colour(i, f));
    }
    for (i = 0; i < H; i += 24) quad(s, 0, (float)i, W, (float)i + 1, colour(i + 50, f)); /* worker 1's rows, heavier */
    copy_tex(s, a);                                   /* reads rows every worker owns */
    quad(s, 0, 0, W, H, colour(99, f));               /* writes every row the copy just read */
    for (i = 0; i < 8; i++) quad(s, (float)(i * 16), (float)(i * 11), (float)(i * 16 + 30), (float)(i * 11 + 20), colour(i + 200, f));
    copy_tex(s, b);
    quad(s, 8, 8, W - 8, H - 8, colour(77, f));
    copy_tex(s, c); /* a third, so each kind of read below has a copy of its own to wait for */
    /* Each read below waits for the newest unfinished copy it overlaps, so
     * each comes while its own copy is that one: the palette the second, the
     * vertex colours the third, the texture the first destination's second
     * copy -- a read that waited for its older copy would see the old bytes. */
    bp_w(s, 0x66, 0x001000u); bp_w(s, 0x66, 0x001100u); /* GXInvalidateTexAll */
    bp_w(s, 0x64, (b >> 5) & 0x1FFFFFu); bp_w(s, 0x65, 0x000400u); /* a palette from inside the second copy */
    textured(s, C4_TEX, 8, 8, 8, W / 2, 0, W / 2, H / 2);
    quad(s, W / 2, 0, W, H / 2, 0xFFFFFFFFu);
    untextured(s);                                   /* colours indexed from inside the third copy */
    cp_w(s, 0x50, 0x4200); cp_w(s, 0xA2, (c + 256) & 0x1FFFFFFFu); cp_w(s, 0xB2, 4);
    gp8(s, 0x80); gp16(s, 4);
    gpf(s, 0); gpf(s, H / 2); gpf(s, 50); gp8(s, 0);
    gpf(s, W); gpf(s, H / 2); gpf(s, 50); gp8(s, 5);
    gpf(s, W); gpf(s, H); gpf(s, 50); gp8(s, 9);
    gpf(s, 0); gpf(s, H); gpf(s, 50); gp8(s, 14);
    cp_w(s, 0x50, 0x2200);
    quad(s, 16, 16, 48, 48, colour(55, f));
    copy_tex(s, a); /* the first destination again */
    bp_w(s, 0x66, 0x001000u); bp_w(s, 0x66, 0x001100u);
    textured(s, a, 6, W, H, 0, 0, W / 2, H / 2);     /* samples it: must be this copy, not the first */
    quad(s, 0, 0, W / 2, H / 2, 0xFFFFFFFFu);
    untextured(s);
    /* A copy, GXDrawDone, then the CPU rewriting the copy's bytes, which
     * GXDrawDone told it it may: the draw after must read what the CPU wrote,
     * not the copy's image, which the drain retired. */
    quad(s, 70, 20, 120, 60, colour(22, f));
    copy_tex(s, m);
    bp_w(s, 0x45, 0x000002u);
    for (i = 0; i < 64; i++) s->mem[m + (uint32_t)i * 97u] ^= (uint8_t)(0x5A + f + i);
    bp_w(s, 0x66, 0x001000u); bp_w(s, 0x66, 0x001100u);
    textured(s, m, 6, W, H, W / 2, 0, W / 2, H / 2);
    quad(s, W / 2, 0, W, H / 2, 0xFFFFFFFFu);
    untextured(s);
    /* A hook writing a copy's bytes in the middle of a frame, as SOA_POKE or a
     * mod does: it waits for the copy through gxr_hook_hazard, then writes,
     * and the draw after must read what it wrote. */
    quad(s, 10, 50, 60, 80, colour(5, f));
    copy_tex(s, q);
    gxr_hook_hazard(q, 256);
    for (i = 0; i < 64; i++) s->mem[q + (uint32_t)i * 4u] = (uint8_t)(0xC3 ^ (f * 7 + i));
    bp_w(s, 0x66, 0x001000u); bp_w(s, 0x66, 0x001100u);
    textured(s, q, 6, W, H, 0, 0, W / 2, H / 2);
    quad(s, 0, 0, W / 2, H / 2, 0xFFFFFFFFu);
    untextured(s);
    quad(s, 40, 30, 90, 70, colour(66, f));
    copy_tex(s, d); /* read by nothing but the CPU, after the token */
    /* A copy into mip level 1 of a texture, then a draw that minifies it so
     * the sampler picks level 1: the texture's read must wait for this copy
     * although it does not touch the base level (the review of H14). */
    quad(s, 24, 40, 100, 90, colour(88, f));
    copy_tex(s, e + E_LEVEL1);
    textured(s, e, 6, 256, 192, 0, 48, 64, 48);
    bp_w(s, 0x80, 2u << 5);        /* point, with mipmaps */
    bp_w(s, 0x84, 16u << 8);       /* max LOD 1.0: two levels */
    quad(s, 0, 48, 64, 96, 0xFFFFFFFFu);
    untextured(s);
    /* A copy's image (FINDINGS "Copy images") overwritten in part by a newer
     * copy of another size and format: the draw after both must read memory,
     * the two copies' bytes together, and not the first copy's image. */
    quad(s, 60, 10, 110, 50, colour(44, f));
    copy_tex(s, g);
    copy_rows(s, g, 8);
    bp_w(s, 0x66, 0x001000u); bp_w(s, 0x66, 0x001100u);
    textured(s, g, 6, W, H, W / 2, H / 2, W / 2, H / 2);
    quad(s, W / 2, H / 2, W, H, 0xFFFFFFFFu);
    untextured(s);
    /* An unfiltered copy reads only rows its own worker drew, so it carries no
     * fence, and the draw that samples its image is the one that has to wait
     * for every worker to have finished it. */
    bp_w(s, 0x53, 0x556000u); bp_w(s, 0x54, 0x000015u); /* the identity: 22 + 21 + 21 on the centre row */
    quad(s, 20, 60, 70, 90, colour(33, f));
    copy_tex(s, k);
    bp_w(s, 0x53, 0x30A208u); bp_w(s, 0x54, 0x00820Au); /* the deflicker again */
    bp_w(s, 0x66, 0x001000u); bp_w(s, 0x66, 0x001100u);
    textured(s, k, 6, W, H, 0, H / 2, W / 2, H / 2);
    quad(s, 0, H / 2, W / 2, H, 0xFFFFFFFFu);
    untextured(s);
    /* A token between a copy and the draw sampling it: once the token has
     * found every copy finished -- always under SOA_GXR_TOKENWAIT, which waits
     * -- the game may write the copy's bytes, so the image retires and the
     * draw reads memory. Nothing writes them here: the pixels are the same
     * either way, and the counts say which way it went. */
    quad(s, 30, 70, 80, 95, colour(11, f));
    copy_tex(s, n);
    bp_w(s, 0x48, 0x100u + (uint32_t)f);
    bp_w(s, 0x66, 0x001000u); bp_w(s, 0x66, 0x001100u);
    textured(s, n, 6, W, H, W / 2, H / 2, W / 2, H / 2);
    quad(s, W / 2, H / 2, W, H, 0xFFFFFFFFu);
    untextured(s);
    screen_copy(s);
    bp_w(s, 0x48, (uint32_t)f); /* a draw token */
    g_cpu = fnv(g_cpu, s->mem + d, W * H * 4);
    if (f == 3) {
        bp_w(s, 0x45, 0x000002u); /* GXDrawDone */
        g_done = fnv(g_done, s->mem + d, W * H * 4);
    }
}


int main(int argc, char** argv)
{
    static CpuState s;
    int cap, f, i;
    uint64_t h;
    (void)argc; (void)argv;
    s.mem = (uint8_t*)calloc(1, MEM_IMAGE_SIZE);
    if (!s.mem) return 2;
    for (i = 0; i < 32; i++) s.mem[C4_TEX + i] = (uint8_t)(i * 37 + 11);
    plat_setenv("SOA_RENDER", "1");
    plat_setenv("SOA_SNAP", NULL);
    if (!gxr_enabled()) return 3;
#ifdef OVERLAP_GPU
    {
        /* GPU spec V6b (tools/gpuspike.py overlap): the same stream with the
         * GPU backend as the queue's one consumer; a mutation of gxv's from
         * SOA_GPU_MUTATE. Never built by this file. */
        char why[256];
        const char* m = getenv("SOA_GPU_MUTATE");
        if (m && *m && !gxv_set_mutation(m)) { printf("no mutation %s\n", m); return 2; }
        if (!gxv_init(why, sizeof why)) { printf("skip: %s\n", why); return 3; }
        gxr_set_backend(gxv_backend());
    }
#endif
    for (cap = 0; cap < 2; cap++) {
        setup(&s); /* first: the reset clears to the clear colour it sets */
        gxr_reset_efb();
        for (f = 0; f < FRAMES; f++) frame(&s, f);
    }
    gxr_flush();
    h = 1469598103934665603ull;
    for (f = 0; f < FRAMES; f++) h = fnv(h, s.mem + A_BASE + (uint32_t)f * SLOT, W * H * 4);
    printf("[overlap] first copies %016llx\n", (unsigned long long)h);
    h = 1469598103934665603ull;
    for (f = 0; f < FRAMES; f++) h = fnv(h, s.mem + B_BASE + (uint32_t)f * SLOT, W * H * 4);
    printf("[overlap] second copies %016llx\n", (unsigned long long)h);
    h = 1469598103934665603ull;
    for (f = 0; f < FRAMES; f++) h = fnv(h, s.mem + C_BASE + (uint32_t)f * SLOT, W * H * 4);
    printf("[overlap] third copies %016llx\n", (unsigned long long)h);
    printf("[overlap] token reads %016llx\n", (unsigned long long)g_cpu);
    printf("[overlap] drawdone reads %016llx\n", (unsigned long long)g_done);
    printf("[overlap] screen %016llx\n", (unsigned long long)gxr_screen_hash());
    printf("[overlap] efb %016llx\n", (unsigned long long)fnv(1469598103934665603ull, &g_efb[0][0][0], sizeof g_efb));
    h = decoded(1469598103934665603ull, C4_TEX, 8, 8, 8);
    for (f = 0; f < FRAMES; f++) {
        h = decoded(h, A_BASE + (uint32_t)f * SLOT, 6, W, H);
        h = decoded(h, E_BASE + (uint32_t)f * 0x40000u, 6, 256, 192);
        h = decoded(h, G_BASE + (uint32_t)f * SLOT, 6, W, H);
        h = decoded(h, K_BASE + (uint32_t)f * SLOT, 6, W, H);
        h = decoded(h, M_BASE + (uint32_t)f * SLOT, 6, W, H);
        h = decoded(h, N_BASE + (uint32_t)f * SLOT, 6, W, H);
        h = decoded(h, Q_BASE + (uint32_t)f * SLOT, 6, W, H);
    }
    printf("[overlap] decodes %016llx\n", (unsigned long long)h);
    if (getenv("OVERLAP_DUMP")) {
        /* the copy destinations, A_BASE to Q_BASE's last frame, then the
         * screen: for telling two renderers' differences apart by size */
        FILE* d = fopen(getenv("OVERLAP_DUMP"), "wb");
        int w = 0, hh = 0;
        const uint8_t* scr = gxr_screen(&w, &hh);
        if (d) {
            fwrite(s.mem + A_BASE, 1, Q_BASE + FRAMES * SLOT - A_BASE, d);
            if (scr) fwrite(scr, 1, (size_t)EFB_W * 4 * hh, d);
            fclose(d);
        }
    }
    gxr_report();
    fflush(stdout);
    return 0;
}
"""

# (threads, SOA_GXR_STALL, SOA_HASH, other environment): the one-worker run
# first, as the oracle -- its SOA_HASH drains at every screen copy, so its
# reads after the token are of finished copies.
OVERLAP_RUNS = [
    ("1", "", "1"),
    ("1", "", ""),
    ("2", "", ""),
    ("3", "", ""),
    ("8", "", ""),
    ("8", "", "1"),
    ("2", "2:0:300", ""),
    ("3", "2:0:300", ""),
    ("8", "2:0:300", ""),
    ("2", "2:1:20000", ""),
    ("3", "2:1:20000", ""),
    ("8", "2:1:20000", ""),
    ("3", "2:2:20000", ""),
    ("8", "2:1:20000", "1"),
    ("3", "", "", "SOA_GXR_TOKENWAIT=1"),
    ("8", "2:1:20000", "", "SOA_GXR_TOKENWAIT=1"),
    ("8", "2:1:20000", "", "SOA_GXR_DRAIN=1"),
    ("3", "2:0:300", "", "SOA_GXR_DRAIN=1"),
]
OVERLAP_RUNS = [r if len(r) == 4 else (*r, "") for r in OVERLAP_RUNS]
OVERLAP_FINAL = (
    "first copies",
    "second copies",
    "third copies",
    "drawdone reads",
    "screen",
    "efb",
    "decodes",
)


# ---- test_gxr_queue.py's driver: the handshake around gxr_flush ----

QUEUE_SOURCES = ["gx.c", "gx_frame.c", "gxr.c", "gxr_tev.c", "png.c"]

# The stream drives the flush in every shape the queue has: a copy to memory
# followed by a draw sampling that destination (the flush that fires from
# inside tev_prepare with a draw half built), GXDrawDone, the screen copy, and
# gxr_reset_efb between captures. Positions and colours move with the frame so
# a frame drawn out of order, twice, or not at all does not hash the same as
# the one before it.
QUEUE_DRIVER = r"""
#define _CRT_SECURE_NO_WARNINGS
#include "cpu.h"
#include "gxr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t mmio_read32(CpuState* s, uint32_t ea) { (void)s; (void)ea; return 0; }
void hle_report(void) {}

static void gp8(CpuState* s, unsigned v) { gx_pipe_write(s, 1, v); }
static void gp16(CpuState* s, unsigned v) { gx_pipe_write(s, 2, v); }
static void gp32(CpuState* s, uint32_t v) { gx_pipe_write(s, 4, v); }
static void gpf(CpuState* s, float f) { uint32_t u; memcpy(&u, &f, 4); gp32(s, u); }
static void bp_w(CpuState* s, unsigned reg, uint32_t v) { gp8(s, 0x61); gp32(s, ((uint32_t)reg << 24) | (v & 0xFFFFFFu)); }
static void cp_w(CpuState* s, unsigned reg, uint32_t v) { gp8(s, 0x08); gp8(s, reg); gp32(s, v); }
static void xf_w(CpuState* s, unsigned addr, unsigned n, const uint32_t* w)
{
    unsigned i;
    gp8(s, 0x10); gp16(s, n - 1); gp16(s, addr);
    for (i = 0; i < n; i++) gp32(s, w[i]);
}
static void xf_f(CpuState* s, unsigned addr, unsigned n, const float* f)
{
    unsigned i;
    gp8(s, 0x10); gp16(s, n - 1); gp16(s, addr);
    for (i = 0; i < n; i++) gpf(s, f[i]);
}
static void vertex(CpuState* s, float x, float y, float z, uint32_t rgba) { gpf(s, x); gpf(s, y); gpf(s, z); gp32(s, rgba); }

static void setup(CpuState* s)
{
    static const float viewport[6] = {320.0f, -240.0f, 16777215.0f, 662.0f, 582.0f, 16777215.0f};
    static const float ortho[6] = {0.003125f, -0.0f, 0.004167f, -0.0f, -0.01f, -1.0f};
    static const float view[12] = {1, 0, 0, -320, 0, -1, 0, 240, 0, 0, 1, -100};
    static const uint32_t one = 1, matidx = 0x3CF3CF00u, chan = 0x441u, zero = 0;
    bp_w(s, 0x59, 0x02ACABu); bp_w(s, 0x20, 0x156156u); bp_w(s, 0x21, 0x3D5335u);
    xf_f(s, 0x101A, 6, viewport);
    xf_f(s, 0x1020, 6, ortho); xf_w(s, 0x1026, 1, &one);
    xf_f(s, 0x0000, 12, view);
    cp_w(s, 0x30, matidx); xf_w(s, 0x1018, 1, &matidx);
    xf_w(s, 0x1009, 1, &one); xf_w(s, 0x100E, 1, &chan); xf_w(s, 0x1010, 1, &chan);
    xf_w(s, 0x103F, 1, &zero); xf_w(s, 0x1008, 1, &one);
    bp_w(s, 0x28, 0); bp_w(s, 0xC0, 0x08AFFFu); bp_w(s, 0xC1, 0x08BFF0u); bp_w(s, 0x00, 0x000010u);
    bp_w(s, 0xF6, 0x018064u); bp_w(s, 0xF7, 0x01806Eu); bp_w(s, 0xF8, 0x018060u); bp_w(s, 0xF9, 0x01806Cu);
    bp_w(s, 0xFA, 0x018065u); bp_w(s, 0xFB, 0x01806Du); bp_w(s, 0xFC, 0x01806Au); bp_w(s, 0xFD, 0x01806Eu);
    bp_w(s, 0x40, 0x17); bp_w(s, 0x41, 0x18); bp_w(s, 0xF3, 0x3F0000u); bp_w(s, 0x43, 0x40);
    cp_w(s, 0x50, 0x2200); cp_w(s, 0x60, 0); cp_w(s, 0x70, 0x41377009u); cp_w(s, 0x80, 0xC8241209u); cp_w(s, 0x90, 0x04824120u);
    bp_w(s, 0x4E, 0x000100u);
}

/* Stage 0 samples map 0 and passes the rasterized colour through, so every
 * draw decodes a texture the next flush frees without the texels reaching a
 * pixel: the picture says which draws ran, not what they sampled. */
static void use_texture(CpuState* s, uint32_t addr, unsigned fmt, unsigned w, unsigned h)
{
    bp_w(s, 0x80, 0); bp_w(s, 0x84, 0);
    bp_w(s, 0x88, ((fmt & 15) << 20) | ((h - 1) << 10) | (w - 1));
    bp_w(s, 0x94, (addr >> 5) & 0x1FFFFFu);
    bp_w(s, 0x98, 0);
    bp_w(s, 0x30, w - 1); bp_w(s, 0x31, h - 1);
    bp_w(s, 0x28, 0x40);
}

static void quad(CpuState* s, float x0, float y0, float x1, float y1, uint32_t rgba)
{
    gp8(s, 0x80); gp16(s, 4);
    vertex(s, x0, y0, 50.0f, rgba); vertex(s, x1, y0, 50.0f, rgba);
    vertex(s, x1, y1, 50.0f, rgba); vertex(s, x0, y1, 50.0f, rgba);
}

static void copy_to_memory(CpuState* s, uint32_t dest, unsigned w, unsigned h)
{
    bp_w(s, 0x49, 0);
    bp_w(s, 0x4A, ((h - 1) << 10) | (w - 1));
    bp_w(s, 0x4D, (w + 3) / 4); /* RGB565: a row of 4x4 tiles, 32 bytes each */
    bp_w(s, 0x4B, (dest >> 5) & 0x1FFFFFu);
    bp_w(s, 0x52, 0x000043u);
}

static void present(CpuState* s)
{
    bp_w(s, 0x49, 0); bp_w(s, 0x4A, 0x077E7F); bp_w(s, 0x4D, 0x28);
    bp_w(s, 0x4B, (0x81100000u & 0x1FFFFFFFu) >> 5);
    bp_w(s, 0x52, 0x4003);
    gxr_flush();
}

#define CAPTURES 2
#define FRAMES 5

int main(void)
{
    static CpuState s;
    int cap, f, i;
    s.mem = (uint8_t*)calloc(1, MEM_IMAGE_SIZE);
    if (!s.mem) return 2;
    plat_setenv("SOA_RENDER", "1");
    plat_setenv("SOA_SNAP", NULL);
    plat_setenv("SOA_GXR_DRAWS", NULL);
    if (!gxr_enabled()) return 3;

    for (cap = 0; cap < CAPTURES; cap++) {
        gxr_reset_efb(); /* a replay renders several captures in one process */
        setup(&s);
        for (f = 0; f < FRAMES; f++) {
            for (i = 0; i < 40; i++) {
                float x = (float)(8 + (i * 13 + f * 7) % 560), y = (float)(8 + (i * 29 + f * 11) % 400);
                uint32_t col = ((uint32_t)((i * 7 + f * 5) & 0xFF) << 24) | ((uint32_t)((i * 31 + f * 3) & 0xFF) << 16) |
                               ((uint32_t)((f * 9 + i) & 0xFF) << 8) | 0xFFu;
                use_texture(&s, 0x00400000u + (uint32_t)((cap * 997 + f * 61 + i) * 512), 9, 16, 16);
                quad(&s, x, y, x + 60.0f, y + 40.0f, col);
                if ((i & 3) == 3) {
                    /* The draw after this copy samples what the copy has not
                     * written yet, so tev_prepare flushes with the draw half
                     * built. A 16x16 texture inside the 32x32 copy: the copy's
                     * own texture would be its image, which a draw samples in
                     * the pool without waiting (FINDINGS "Copy images"). */
                    copy_to_memory(&s, 0x00300000u + (uint32_t)(i * 4096), 32, 32);
                    use_texture(&s, 0x00300000u + (uint32_t)(i * 4096), 4, 16, 16);
                    quad(&s, (float)(100 + i + f), (float)(60 + i), (float)(140 + i + f), (float)(100 + i), 0x00FF00FFu);
                }
                if ((i % 17) == 16) bp_w(&s, 0x45, 2); /* GXDrawDone: flushes */
            }
            present(&s);
            printf("[queue] cap %d frame %d hash %016llx\n", cap, f, (unsigned long long)gxr_screen_hash());
        }
    }
    gxr_report();
    fprintf(stderr, "[queue] done\n");
    return 0;
}
"""

QUEUE_THREAD_COUNTS = ("1", "2", "3", "8")


class BuildError(Exception):
    pass


def build(name: str, driver: str, sources: list[str], out: Path, prof, cflags=()) -> Path:
    """Compile the driver and the runtime files it needs, then link them: two
    steps, because cl's compile-and-link with /Fo<dir> has no gcc spelling."""
    objdir = out / f"{name}-obj"
    objdir.mkdir(parents=True, exist_ok=True)
    src = out / f"{name}.c"
    src.write_bytes(driver.encode())
    paths = [str(RUNTIME / s) for s in sources] + [str(src)]
    compiled = toolchain.cc(
        [*prof.cflags, *cflags, "/c", f"/I{RUNTIME}", *paths, f"/Fo{objdir}{os.sep}"], ROOT, prof
    )
    if compiled.returncode:
        raise BuildError(f"compiling {name}:\n{compiled.stdout}{compiled.stderr}")
    exe = out / f"{name}{prof.exeext}"
    objs = [str(objdir / (Path(p).stem + prof.objext)) for p in paths]
    linked = toolchain.cc([*prof.cflags, *cflags, *objs, f"/Fe:{exe}", *prof.linker], ROOT, prof)
    if linked.returncode:
        raise BuildError(f"linking {name}:\n{linked.stdout}{linked.stderr}")
    return exe


def run(exe: Path, extra: dict, cwd: Path) -> tuple[int, str]:
    """One run, with no SOA_* from the caller's environment but `extra`."""
    env = {k: v for k, v in os.environ.items() if not k.startswith("SOA_")}
    p = subprocess.run(
        [str(exe)],
        capture_output=True,
        text=True,
        env={**env, **extra},
        cwd=cwd,
        timeout=900,
        check=False,
    )
    return p.returncode, p.stdout + p.stderr


def overlap_sweep(threads: list[str]) -> list[tuple[str, str, str, str]]:
    """The oracle, then each thread count unstalled and, from two workers up,
    with each stall kind."""
    runs = [("1", "", "1", "")]
    for t in threads:
        runs.append((t, "", "", ""))
        if int(t) >= 2:
            runs += [(t, stall, "", "") for stall in STALLS]
    return runs


def overlap_env(key: tuple[str, str, str, str]) -> dict:
    threads, stall, hashed, other = key
    extra = {"SOA_THREADS": threads}
    if other:
        k, v = other.split("=")
        extra[k] = v
    if stall:
        extra["SOA_GXR_STALL"] = stall
    if hashed:
        extra["SOA_HASH"] = hashed
    return extra


def overlap_finals(text: str) -> dict:
    return {
        k: (m[1] if (m := re.search(rf"\[overlap\] {k} (\w+)", text)) else None)
        for k in OVERLAP_FINAL
    }


def check_queue(exe: Path, out: Path, threads: list[str]) -> list[str]:
    bad, hashes, failed = [], {}, set()
    for t in threads:
        code, text = run(exe, {"SOA_THREADS": t}, out)
        hashes[t] = re.findall(r"\[queue\] cap \d+ frame \d+ hash (\w+)", text)
        why = (
            f"exit {code}"
            if code
            else "no [queue] done"
            if "[queue] done" not in text
            else "a worker found a command built over its own"
            if "queue slot" in text
            else f"{len(hashes[t])} frames, not 10"
            if len(hashes[t]) != 10
            else ""
        )
        if why:
            failed.add(t)
            bad.append(f"queue SOA_THREADS={t}: {why}\n{text[-3000:]}")
    one = hashes.get("1") or hashes[threads[0]]
    if len(one) == 10 and (len(set(one[:5])) != 5 or one[:5] != one[5:]):
        bad.append(f"queue: the oracle's frames prove nothing (alike, or not repeated): {one}")
    for t, h in hashes.items():
        if len(h) == 10 and h != one:
            bad.append(f"queue SOA_THREADS={t} drew a different picture:\n{h}\n{one}")
        print(f"{'FAIL' if h != one or t in failed else 'ok  '} queue    SOA_THREADS={t}")
    return bad


def check_overlap(exe: Path, out: Path, threads: list[str]) -> list[str]:
    bad, oracle = [], None
    for key in overlap_sweep(threads):
        code, text = run(exe, overlap_env(key), out)
        label = f"overlap  SOA_THREADS={key[0]}" + (f" SOA_GXR_STALL={key[1]}" if key[1] else "")
        label += " SOA_HASH=1 (the oracle)" if key[2] else ""
        finals = overlap_finals(text)
        why = (
            f"exit {code}"
            if code
            else f"missing {[k for k, v in finals.items() if v is None]}"
            if None in finals.values()
            else "the stall was not in force"
            if key[1] and "SOA_GXR_STALL: 1 stall in force" not in text
            else ""
        )
        if oracle is None and not why:
            oracle = finals
            frames = re.findall(r"\[gxr\] frame \d+ \d+x\d+ hash (\w+)", text)
            if len(frames) != 12 or len(set(frames[:6])) != 6 or frames[:6] != frames[6:]:
                why = f"the oracle's frames prove nothing: {frames}"
        elif not why and oracle is not None:
            differ = [k for k in OVERLAP_FINAL if finals[k] != oracle[k]]
            if differ:
                why = f"{', '.join(differ)} differ from the one-worker run"
        if why:
            bad.append(f"{label}: {why}\n{text[-3000:]}")
        print(f"{'FAIL' if why else 'ok  '} {label}")
    return bad


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cc", choices=tuple(toolchain.PROFILES), default="msvc")
    ap.add_argument("--threads", default="1,2,3,4", help="the worker counts to run, one included")
    ap.add_argument("--cflag", action="append", default=[], help="added to every compile and link")
    ap.add_argument("--out", type=Path, default=None)
    args = ap.parse_args()
    prof = toolchain.profile(args.cc)
    threads = [t.strip() for t in args.threads.split(",") if t.strip()]
    if "1" not in threads:
        threads.insert(0, "1")
    out = (args.out or ROOT / "build" / "citest" / f"queue-{prof.name}").resolve()
    cc = toolchain.compiler_path(prof)
    if cc is None:
        print(f"::error::no {prof.name} compiler (tools/soa/toolchain.py compiler_path)")
        return 1
    print(f"compiler: {cc}")
    print(f"threads:  {', '.join(threads)}; cflags: {' '.join(args.cflag) or 'none'}\n")
    try:
        queue = build("queue", QUEUE_DRIVER, QUEUE_SOURCES, out, prof, args.cflag)
        overlap = build("overlap", OVERLAP_DRIVER, OVERLAP_SOURCES, out, prof, args.cflag)
    except BuildError as e:
        print(f"::error::{e}")
        return 1
    bad = check_queue(queue, out, threads) + check_overlap(overlap, out, threads)
    for b in bad:
        print(f"\n::error::{b}")
    print(f"\n{len(bad)} problem(s)" if bad else "\nevery run matches the one-worker run")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
