"""Tests for the renderer's tripwires (PLAN A3).

The tripwires exist because the rasterizer draws something plausible for the
hardware features it does not implement and says nothing, so a missing feature
looks exactly like a bug in a feature we do have. That only works if two things
hold, and neither can be read off the source: each condition speaks once when
the stream asks for what we do not model, and none of them speaks on a stream
the port renders correctly. A tripwire that cries wolf is worse than none --
the channel stops being read -- and one that fires every frame is the same
thing at a different rate.

So this builds the renderer on its own, the way ``tools/citest/render_check.py``
does (gx.c, gxr.c, gxr_tev.c and png.c reach only two symbols outside
themselves), and feeds it a synthetic command stream: every condition asked for
twice, and beside each one the nearby value the game really does program, which
must stay silent. No disc, no recompiled code, no window.
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

# The register recipe down to "---- the stream ----" is the short form of the
# one in tools/citest/render_driver.c: enough state for a draw to reach the
# rasterizer, since what is checked here is what the renderer says, not what it
# draws. Nothing below reads a pixel.
DRIVER = r"""
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
    bp_w(s, 0x4E, 0x000100u); /* copy Y-scale: the identity, which is all the game programs */
    bp_w(s, 0x53, 0x30A208u); bp_w(s, 0x54, 0x00820Au); /* the deflicker filter, as the game programs it */
}

/* Two vertices for a line, three for anything else; off-screen, because no
 * pixel here is looked at. */
static void draw(CpuState* s, unsigned op, unsigned n)
{
    unsigned i;
    gp8(s, op); gp16(s, n);
    for (i = 0; i < n; i++) vertex(s, 10.0f + 10.0f * i, 20.0f, 50.0f, 0xFF0000FFu);
}

static void copy(CpuState* s)
{
    bp_w(s, 0x49, 0); bp_w(s, 0x4A, 0x077E7F); bp_w(s, 0x4D, 0x28);
    bp_w(s, 0x4B, (0x81100000u & 0x1FFFFFFFu) >> 5);
    bp_w(s, 0x52, 0x4003);
    gxr_flush();
}

int main(void)
{
    static CpuState s;
    int pass;
    s.mem = (uint8_t*)calloc(1, MEM_IMAGE_SIZE);
    if (!s.mem) return 2;
    _putenv("SOA_RENDER=1");
    _putenv("SOA_THREADS=1");
    _putenv("SOA_SNAP=");
    _putenv("SOA_GXR_DRAWS=");
    if (!gxr_enabled()) return 3;
    gxr_reset_efb();
    setup(&s);

    /* ---- the stream ---- Everything twice: a tripwire that speaks twice is
     * a tripwire that will speak every frame. */
    for (pass = 0; pass < 2; pass++) {
        /* What the game really programs, beside what it does not. Line size 7
         * is 1.17 pixels, which we draw one pixel wide, and three captures
         * have it; the filter and the identity Y-scale are in every capture. */
        bp_w(&s, 0x22, 0x000607u);
        draw(&s, 0xA8, 2);
        draw(&s, 0xB8, 1);
        copy(&s);

        bp_w(&s, 0x22, 0x001818u); /* four pixels wide, four across */
        draw(&s, 0xA8, 2);
        draw(&s, 0xB8, 1);
        bp_w(&s, 0x22, 0x000606u);

        /* GX_BL_DSTALPHA at EFB format RGB8: blend on, source factor 4,
         * destination factor 6 (the EFB's alpha, which RGB8 does not have). */
        bp_w(&s, 0x41, 0x0004C1u);
        draw(&s, 0x80, 4);
        bp_w(&s, 0x41, 0x18);

        bp_w(&s, 0x00, 0x010010u); /* GEN_MODE: one indirect stage */
        bp_w(&s, 0x00, 0x080010u); /* GEN_MODE: zfreeze */
        bp_w(&s, 0x00, 0x000010u);
        bp_w(&s, 0x63, 0x000001u); /* TMEM preload */
        bp_w(&s, 0xE8, 0x000556u); /* FOGRANGE: adjustment on, centre 342 */
        bp_w(&s, 0xE8, 0x000156u); /* ... and off again, as the SDK leaves it */
        bp_w(&s, 0xF5, 0x000004u); /* ZTEX2: Z textures on */
        bp_w(&s, 0xF5, 0x000000u);

        /* BP_MASK. GXSetCoPlanar writes the mask and then the whole shadowed
         * GEN_MODE, so every bit outside the mask is already what it says:
         * that must stay silent. A masked write that really does change a bit
         * outside the mask must not. */
        bp_w(&s, 0xFE, 0x080000u);
        bp_w(&s, 0x00, 0x000010u);
        bp_w(&s, 0xFE, 0x080000u);
        bp_w(&s, 0x00, 0x000011u);
        bp_w(&s, 0x00, 0x000010u);

        bp_w(&s, 0x43, 0x000041u); /* PE_CONTROL: EFB pixel format 1 */
        bp_w(&s, 0x43, 0x000048u); /* ... and depth format 1 */
        bp_w(&s, 0x43, 0x000040u);

        bp_w(&s, 0x4E, 0x000200u); /* copy Y-scale 2.0 */
        copy(&s);
        bp_w(&s, 0x4E, 0x000100u);
    }
    gxr_flush();
    gxr_report();
    fprintf(stderr, "[driver] done\n");
    return 0;
}
"""


needs_msvc = pytest.mark.skipif(
    toolchain.cl_path() is None, reason="no MSVC: the renderer cannot be built here"
)


@pytest.fixture(scope="module")
def output(tmp_path_factory):
    """Build the renderer and run the stream once; every test reads the same
    output, because what is being checked is how often each line appears."""
    if toolchain.cl_path() is None:
        pytest.skip("no MSVC")
    out = tmp_path_factory.mktemp("tripwires")
    (out / "driver.c").write_text(DRIVER)
    exe = out / "tripwires.exe"
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
    run = subprocess.run(
        [str(exe)], capture_output=True, text=True, env=env, cwd=out, timeout=300, check=False
    )
    text = run.stdout + run.stderr
    assert run.returncode == 0, text
    assert "[driver] done" in text, text
    return text


# What each condition says, and the value that has to produce it.
CONDITIONS = [
    ("indirect texture stage", "GEN_MODE 010010 asks for 1 indirect texture stage"),
    ("zfreeze", "GEN_MODE 080010 turns zfreeze on"),
    ("EFB pixel format", "PE_CONTROL 000041 selects EFB pixel format 1"),
    ("EFB depth format", "PE_CONTROL 000048 selects EFB depth format 1"),
    ("TMEM preload", "TMEM preload (BP 63 000001"),
    ("fog range", "FOGRANGE (BP E8 000556) enables fog range adjustment about x=342"),
    ("Z textures", "ZTEX2 (BP F5 000004) turns Z textures on"),
    ("BP_MASK", "BP_MASK 080000 was in force for BP 00 000011"),
    ("wide lines", "asks for lines 4.00 pixels wide"),
    ("wide points", "asks for points 4.00 pixels across"),
    ("destination alpha", "blends with a destination-alpha factor"),
    ("copy Y-scale", "EFB copy Y-scale (BP 4E 000200) is 2.000"),
]


@needs_msvc
@pytest.mark.parametrize("what,line", CONDITIONS, ids=[c[0] for c in CONDITIONS])
def test_each_condition_warns_exactly_once(output, what, line):
    """Asked for twice, said once: the flags are per condition, so one firing
    leaves the others armed, and none of them repeats."""
    assert output.count(line) == 1, f"{what}: {output.count(line)} lines\n{output}"


@needs_msvc
def test_what_the_game_really_programs_stays_silent(output):
    """The false positives that would put a line in every log of a port that
    renders the game correctly. Line size 7 is in three captures, the copy
    filter and the identity Y-scale in all 23, and the masked GEN_MODE write
    that changes nothing outside the mask is GXSetCoPlanar, which the game
    calls from its first frame. The copy filter is applied now rather than
    merely unmentioned, but it is still not news, so it still says nothing."""
    assert "1.17 pixels" not in output, output
    assert "EFB copy filter" not in output, output
    assert "BP 4E 000100" not in output, output
    assert output.count("BP_MASK") == 1, output  # the real one, not the GXSetCoPlanar shape
    # Every [gxr] line that is not one of the twelve conditions, the report, or
    # the driver's own. A new tripwire firing on this stream shows up here.
    strays = [
        ln
        for ln in output.splitlines()
        if ln.startswith("[gxr]")
        and not any(line in ln for _, line in CONDITIONS)
        and not re.match(
            r"\[gxr\] (time|producer|workers|rasterizing|\d+ triangles|\d+ copies asked|waits:|fences:|\d+ draw tokens came)",
            ln,
        )
    ]
    assert not strays, "\n".join(strays)


@needs_msvc
def test_the_copy_filter_is_no_longer_a_standing_limitation(output):
    """It used to be reported at the end of every run as something the port was
    asked for and did not do. PLAN C3 implemented it, so the line is gone
    rather than merely quiet, and what the filter does to a pixel is pinned in
    test_gxr_copy_filter.py instead. The tripwire channel stays silent about it
    either way, which is what the test above checks."""
    assert "seven-tap vertical filter" not in output, output
    assert "got the centre row alone" not in output, output
