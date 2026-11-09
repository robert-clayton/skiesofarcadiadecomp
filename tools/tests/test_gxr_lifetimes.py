"""Tests for the lifetime rules the renderer's queue lives by.

A queued draw holds raw pointers into the decoded-texture cache -- TexCfg
carries ``const uint8_t* level[MAX_MIPS]`` by value inside the DrawCmd -- and a
worker thread dereferences them for as long as it is rasterizing that command.
Two rules follow, and neither can be read off the source:

* a draw is published in the slot it was built in, so what a worker executes is
  the draw the producer just wrote and not a command that has already run;
* no decoded texture is handed back to the allocator while a queued draw, or
  the draw being built right now, can still reach it.

Both used to be broken by the same thing: ``tev_prepare`` flushes when a draw
samples a texture a queued EFB copy has not written yet, and a flush recycles
the queue, the vertex arena and the texture graveyard. The first test below
drives exactly that shape; before the fix it drew the wrong command. The second
piles up more freed textures between two drains than any fixed-size list would
hold, which is what a mass invalidation of the cache does with a full queue --
a TLUT load did that until 2026-09-25, when it began sending textures to be
hashed again instead, and ``tex_invalidate_all`` still does.

Built the way ``test_gxr_tripwires.py`` builds it: the renderer on its own, a
synthetic command stream, no disc and no game data.
"""

import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from soa import toolchain  # noqa: E402

RUNTIME = ROOT / "runtime"
SOURCES = ["gx.c", "gx_frame.c", "gxr.c", "gxr_tev.c", "png.c"]

# The register recipe in setup() is the short form of the one in
# tools/citest/render_driver.c: a vertex at (x, y) lands on EFB pixel (x, y),
# and the TEV stage passes the rasterized colour through, so a quad's colour is
# its vertex colour whether or not the stage samples a texture.
DRIVER = r"""
#define _CRT_SECURE_NO_WARNINGS
#include "cpu.h"
#include "gxr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t mmio_read32(CpuState* s, uint32_t ea) { (void)s; (void)ea; return 0; }
void hle_report(void) {}

int tex_graveyard_peak(void); /* gxr_tev.c, as gxr_report declares it */

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

/* Stage 0 samples texture map 0, which is what makes tev_prepare look a
 * texture up; the combiner still passes the rasterized colour through, so the
 * texels themselves never reach the picture. */
static void use_texture(CpuState* s, uint32_t addr, unsigned fmt, unsigned w, unsigned h)
{
    bp_w(s, 0x80, 0); /* clamp both ways, point sampling, no mipmaps */
    bp_w(s, 0x84, 0);
    bp_w(s, 0x88, ((fmt & 15) << 20) | ((h - 1) << 10) | (w - 1));
    bp_w(s, 0x94, (addr >> 5) & 0x1FFFFFu);
    bp_w(s, 0x98, 0); /* TLUT at TMEM 0, format IA8 */
    bp_w(s, 0x30, w - 1); bp_w(s, 0x31, h - 1);
    bp_w(s, 0x28, 0x40); /* stage 0: map 0 through coord 0, texture enabled */
}

static void quad(CpuState* s, float x0, float y0, float x1, float y1, uint32_t rgba)
{
    gp8(s, 0x80); gp16(s, 4);
    vertex(s, x0, y0, 50.0f, rgba); vertex(s, x1, y0, 50.0f, rgba);
    vertex(s, x1, y1, 50.0f, rgba); vertex(s, x0, y1, 50.0f, rgba);
}

/* An EFB copy to a texture in memory, which stays in the queue and leaves its
 * destination in the hazard list until something drains it. */
static void copy_to_memory(CpuState* s, uint32_t dest, unsigned w, unsigned h)
{
    bp_w(s, 0x49, 0);
    bp_w(s, 0x4A, ((h - 1) << 10) | (w - 1));
    bp_w(s, 0x4D, 0x28);
    bp_w(s, 0x4B, (dest >> 5) & 0x1FFFFFu);
    bp_w(s, 0x52, 0x000043u); /* copy format 4 (RGB565) into memory, no clear */
}

static unsigned px(int x, int y)
{
    return ((unsigned)g_efb[y][x][0] << 16) | ((unsigned)g_efb[y][x][1] << 8) | g_efb[y][x][2];
}

#define RED 0xFF0000FFu
#define GREEN 0x00FF00FFu

int main(void)
{
    static CpuState s;
    int i;
    s.mem = (uint8_t*)calloc(1, MEM_IMAGE_SIZE);
    if (!s.mem) return 2;
    _putenv("SOA_RENDER=1");
    _putenv("SOA_THREADS=1");
    _putenv("SOA_SNAP=");
    _putenv("SOA_GXR_DRAWS=");
    if (!gxr_enabled()) return 3;
    gxr_reset_efb();
    setup(&s);

    /* ---- a draw whose texture a queued copy has not written yet ----
     * The red quad is queued, then a copy to the address the green quad's
     * texture is read from, then the green quad. Resolving that texture makes
     * tev_prepare flush, which recycles the queue under a draw that has
     * already chosen a slot in it: the green quad used to be published at the
     * index the red one had just been drawn from, so the red quad ran twice
     * and the green quad never ran at all. */
    quad(&s, 100.0f, 100.0f, 200.0f, 200.0f, RED);
    copy_to_memory(&s, 0x00300000u, 32, 32);
    use_texture(&s, 0x00300000u, 4, 16, 16); /* inside the copy, not its own texture, which would be its image */
    quad(&s, 300.0f, 300.0f, 400.0f, 400.0f, GREEN);
    gxr_flush();
    printf("[lifetime] hazard red=%06X green=%06X\n", px(150, 150), px(350, 350));

    /* ---- more freed textures between two drains than any fixed list holds ----
     * Seven hundred queued draws, each sampling a texture the cache has never
     * seen, fill 700 of its 1,024 entries. Then every decode is dropped at once
     * -- from outside any draw, so nothing on the path asks whether this is a
     * good moment: the draw path drains when the graveyard reaches GRAVE_SOFT,
     * a mass invalidation does not. A TLUT load was that path until it began
     * sending textures to be hashed again instead; tex_invalidate_all still is.
     * Every one of those buffers is still named by a draw in the queue that a
     * worker has not finished. */
    for (i = 0; i < 700; i++) {
        use_texture(&s, 0x00400000u + (uint32_t)i * 64u, 9, 8, 8); /* C8, a new one every draw */
        quad(&s, 8.0f, 8.0f, 10.0f, 10.0f, RED);
    }
    tex_invalidate_all();
    use_texture(&s, 0x00300000u, 4, 32, 32);
    quad(&s, 500.0f, 300.0f, 560.0f, 360.0f, GREEN);
    gxr_flush();
    printf("[lifetime] graveyard peak=%d green=%06X\n", tex_graveyard_peak(), px(530, 330));

    gxr_report();
    fprintf(stderr, "[lifetime] done\n");
    return 0;
}
"""


needs_msvc = pytest.mark.skipif(
    toolchain.cl_path() is None, reason="no MSVC: the renderer cannot be built here"
)


# Both protocols (H14): by default the green quad's setup waits for the one
# copy it samples; under SOA_GXR_DRAIN=1 it drains inside tev_prepare, the
# path whose use-after-free this file was written for, which keeps its test
# for as long as the switch exists.
@pytest.fixture(scope="module", params=["", "1"], ids=["fenced", "drained"])
def output(tmp_path_factory, request):
    if toolchain.cl_path() is None:
        pytest.skip("no MSVC")
    out = tmp_path_factory.mktemp("lifetimes")
    (out / "driver.c").write_text(DRIVER, encoding="utf-8")
    exe = out / "lifetimes.exe"
    proc = toolchain.cl(
        [
            *toolchain.CFLAGS,
            "/I",
            str(RUNTIME),
            *[str(RUNTIME / name) for name in SOURCES],
            str(out / "driver.c"),
            "/Fo" + str(out) + os.sep,
            "/Fe" + str(exe),
        ],
        cwd=out,
    )
    assert proc.returncode == 0, (proc.stdout or "") + (proc.stderr or "")
    env = dict(os.environ)
    for name in list(env):
        if name.startswith("SOA_"):
            env.pop(name)
    if request.param:
        env["SOA_GXR_DRAIN"] = request.param
    run = subprocess.run(
        [str(exe)], capture_output=True, text=True, env=env, cwd=out, timeout=300, check=False
    )
    text = run.stdout + run.stderr
    assert run.returncode == 0, f"exit {run.returncode}\n{text}"
    assert "[lifetime] done" in text, text
    return text


@needs_msvc
def test_the_draw_that_waited_for_a_copy_is_the_draw_that_runs(output):
    """The green quad samples a texture a queued copy is about to write, so its
    setup flushes half way through. What the workers run afterwards has to be
    the green quad. Before the fix they ran the red one a second time and the
    green quad was dropped, which is the same defect that let a worker sample a
    texture the flush had just freed."""
    m = re.search(r"\[lifetime\] hazard red=([0-9A-F]{6}) green=([0-9A-F]{6})", output)
    assert m, output
    assert m.group(2) == "00FF00", f"the draw built across the flush did not run: {output}"
    assert m.group(1) == "FF0000", f"the draw before the flush did not run: {output}"


@needs_msvc
def test_freed_textures_outlast_any_fixed_size_list(output):
    """Seven hundred queued draws and one mass invalidation put far more
    decoded textures in the graveyard than the 512 the old fixed array held,
    and the ones past 512 were freed on the spot while those draws still
    pointed at them. Nothing may be freed early now, so the count is free to go
    as high as the stream drives it."""
    m = re.search(r"\[lifetime\] graveyard peak=(\d+) green=([0-9A-F]{6})", output)
    assert m, output
    assert int(m.group(1)) > 512, f"the stream no longer overruns a 512-entry list: {output}"
    assert m.group(2) == "00FF00", output


@needs_msvc
def test_the_report_states_both_lifetime_facts(output):
    """gxr_report is where a run says whether it took these paths at all: a
    frame whose hash moves between two builds of the renderer is a frame where
    one of these two lines is non-zero, and a run that prints neither took
    neither path."""
    assert output.count("in the middle of their setup") == 1, output
    assert re.search(r"\[gxr\] \d+ decoded textures waited to be freed at once", output), output
