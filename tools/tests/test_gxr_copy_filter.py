"""The EFB copy's vertical filter (PLAN C3).

The console runs a filter over the EFB as it copies, and its seven six-bit
weights are the thing this test exists to pin down, because they are not seven
rows and the plan said they were for two days. They are vertical sub-samples:
two belong to the row above, three to the row itself and two to the row below.
So the game's 8,8,10,12,10,8,8 is 16/64, 32/64, 16/64 across three rows, and
*nothing* lands two or three rows away.

The zeros at distance two and three are the half of the expectation that
discriminates. A test asserting only "the neighbours get 16/64" passes under
the refuted reading too, and would have blessed it.

The second case is the one that can fail for the right reason. The SDK's own
filter-off coefficients, {0,0,21,22,21,0,0}, put all 64 units of weight on taps
2, 3 and 4 -- so under the correct grouping they are the exact identity and the
copy must come out bit for bit as it did before C3. A wrong collapse that
happens to yield 16/32/16 for the game's set will still fail this one.

Everything here writes the EFB directly and copies it out, so the rasterizer is
not in the picture: what is being checked is the copy, not a rendering. The
build follows ``tools/citest/render_check.py`` -- gx.c, gxr.c, gxr_tev.c and
png.c reach only two symbols outside themselves. No disc, no recompiled code,
no window.
"""

import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from soa import toolchain  # noqa: E402

RUNTIME = ROOT / "runtime"
SOURCES = ["gx.c", "gx_frame.c", "gxr.c", "gxr_tev.c", "png.c"]

# The two coefficient sets that matter, packed as the hardware takes them:
# w0..w3 in BP 0x53 at bits 0, 6, 12, 18 and w4..w6 in BP 0x54 at 0, 6, 12.
GAME_F0, GAME_F1 = 0x30A208, 0x00820A  # 8,8,10,12,10,8,8 -> 16/32/16
OFF_F0, OFF_F1 = 0x595000, 0x000015  # 0,0,21,22,21,0,0 -> the identity

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
static void gp32(CpuState* s, uint32_t v) { gx_pipe_write(s, 4, v); }
static void bp_w(CpuState* s, unsigned reg, uint32_t v) { gp8(s, 0x61); gp32(s, ((uint32_t)reg << 24) | (v & 0xFFFFFFu)); }

#define COPY_W 640
#define COPY_H 480
#define PROBE_X 100

/* A copy of the whole 640x480 rectangle to the screen, clamping both edges,
 * with no clear: the EFB has to survive for the next case. 0x4003 is the
 * game's own 0x4803 without the clear bit. */
static void copy_out(CpuState* s)
{
    bp_w(s, 0x49, 0);
    bp_w(s, 0x4A, ((uint32_t)(COPY_H - 1) << 10) | (COPY_W - 1));
    bp_w(s, 0x4D, 0x28);
    bp_w(s, 0x4B, (0x81100000u & 0x1FFFFFFFu) >> 5);
    bp_w(s, 0x52, 0x4003);
    gxr_flush();
}

static void efb_fill(unsigned char v)
{
    int x, y;
    for (y = 0; y < EFB_H; y++)
        for (x = 0; x < EFB_W; x++) {
            g_efb[y][x][0] = v; g_efb[y][x][1] = v; g_efb[y][x][2] = v; g_efb[y][x][3] = 255;
        }
}

static void efb_white_row(int row)
{
    int x;
    for (x = 0; x < EFB_W; x++) {
        g_efb[row][x][0] = 255; g_efb[row][x][1] = 255; g_efb[row][x][2] = 255; g_efb[row][x][3] = 255;
    }
}

/* One column of the copied frame, as "name: a,b,c,..." over the rows around
 * `centre`. Printed rather than asserted in C so the test names the case. */
static void profile(const char* name, int centre, int span)
{
    int w, h, i;
    const uint8_t* fb = gxr_screen(&w, &h);
    printf("[case] %s:", name);
    for (i = centre - span; i <= centre + span; i++) {
        int v = (i >= 0 && i < h) ? fb[(size_t)i * EFB_W * 4 + PROBE_X * 4] : -1;
        printf(" %d", v);
    }
    printf("\n");
}

static void line_case(CpuState* s, const char* name, uint32_t f0, uint32_t f1, int row, int span)
{
    bp_w(s, 0x53, f0); bp_w(s, 0x54, f1);
    efb_fill(0);
    efb_white_row(row);
    copy_out(s);
    profile(name, row, span);
}

/* A texture copy of the same rectangle, RGBA8 to memory, and an FNV-1a of
 * what it wrote (P5b: SOA_DEFLICKER=0 leaves texture copies filtered). */
static void tex_case(CpuState* s, const char* name, uint32_t f0, uint32_t f1)
{
    const uint32_t dest = 0x80400000u;
    const uint8_t* p;
    uint32_t h = 2166136261u;
    size_t i, n = (size_t)COPY_W * COPY_H * 4;
    bp_w(s, 0x53, f0); bp_w(s, 0x54, f1);
    efb_fill(0);
    efb_white_row(240);
    bp_w(s, 0x49, 0);
    bp_w(s, 0x4A, ((uint32_t)(COPY_H - 1) << 10) | (COPY_W - 1));
    bp_w(s, 0x4D, 0x28);
    bp_w(s, 0x4B, (dest & 0x1FFFFFFFu) >> 5);
    bp_w(s, 0x52, 0x000063u); /* RGBA8 to memory, no clear */
    gxr_flush();
    p = mem_ptr(s, dest);
    for (i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    printf("[tex] %s: %08X\n", name, h);
}

int main(void)
{
    static CpuState s;
    s.mem = (uint8_t*)calloc(1, MEM_IMAGE_SIZE);
    if (!s.mem) return 2;
    if (getenv("DRIVER_DEFLICKER")) {
        /* SOA_DEFLICKER=0 set by the test: the screen and texture copies
         * under the game's weights and under the identity. */
        _putenv("SOA_RENDER=1");
        _putenv("SOA_SNAP=");
        if (!gxr_enabled()) return 3;
        gxr_reset_efb();
        line_case(&s, "screen-game", 0x30A208u, 0x00820Au, 240, 3);
        line_case(&s, "screen-off", 0x595000u, 0x000015u, 240, 3);
        tex_case(&s, "tex-game", 0x30A208u, 0x00820Au);
        tex_case(&s, "tex-off", 0x595000u, 0x000015u);
        gxr_flush();
        fprintf(stderr, "[driver] done\n");
        return 0;
    }
    _putenv("SOA_RENDER=1");
    _putenv("SOA_SNAP=");
    _putenv("SOA_GXR_DRAWS=");
    if (!gxr_enabled()) return 3;
    gxr_reset_efb();

    /* Before anything writes the filter registers they read zero, which is a
     * synthetic stream rather than the game and must copy unfiltered. Taking
     * seven zero weights for a kernel multiplies the frame by nothing. */
    line_case(&s, "unwritten", 0u, 0u, 240, 3);

    /* The game's filter, a single white row in open field. Three rows move and
     * the fourth and fifth must not. */
    line_case(&s, "game-mid", 0x30A208u, 0x00820Au, 240, 3);

    /* The SDK's filter-off set: bit-for-bit what an unfiltered copy gives. */
    line_case(&s, "off-mid", 0x595000u, 0x000015u, 240, 3);

    /* Flat field: 16+32+16 is 64, so a constant has to survive exactly. */
    bp_w(&s, 0x53, 0x30A208u); bp_w(&s, 0x54, 0x00820Au);
    efb_fill(200);
    copy_out(&s);
    profile("flat", 240, 2);

    /* Both edges of the copy rectangle, where a tap would fall outside it.
     * 480 is the rectangle's height, not EFB_H, which is 528: clamping to the
     * EFB instead pulls rows from below the rectangle into the bottom row. */
    line_case(&s, "top", 0x30A208u, 0x00820Au, 0, 2);
    line_case(&s, "bottom", 0x30A208u, 0x00820Au, COPY_H - 1, 2);

    /* A set that does not total 64 scales the whole frame's brightness, and a
     * "are the neighbours zero" test would wave it through. 8,8,10,12,10,8,7
     * sums to 63. */
    bp_w(&s, 0x53, 0x30A208u); bp_w(&s, 0x54, 0x00720Au);
    efb_fill(0);
    efb_white_row(240);
    copy_out(&s);
    profile("sum63", 240, 1);

    gxr_flush();
    fprintf(stderr, "[driver] done\n");
    return 0;
}
"""


needs_msvc = pytest.mark.skipif(
    toolchain.cl_path() is None, reason="no MSVC: the renderer cannot be built here"
)

# The thread counts the replay sweep uses. The filter reads one row either side
# of each output row, and my_row hands neighbouring rows to *different* workers,
# so a frame that changes with the thread count is the row-ownership rule
# broken -- which is the whole reason the copy drains the queue first.
THREADS = [1, 2, 3, 8]


def build(out):
    (out / "driver.c").write_text(DRIVER)
    exe = out / "copyfilter.exe"
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
    return exe


def run(exe, out, threads):
    env = dict(os.environ)
    for name in list(env):
        if name.startswith("SOA_"):
            env.pop(name)
    env["SOA_THREADS"] = str(threads)
    proc = subprocess.run(
        [str(exe)], capture_output=True, text=True, env=env, cwd=out, timeout=300, check=False
    )
    text = proc.stdout + proc.stderr
    assert proc.returncode == 0, text
    assert "[driver] done" in text, text
    return text


@pytest.fixture(scope="module")
def runs(tmp_path_factory):
    """The same stream at every thread count, so one build serves both the
    pixel expectations and the row-ownership check."""
    if toolchain.cl_path() is None:
        pytest.skip("no MSVC")
    out = tmp_path_factory.mktemp("copyfilter")
    exe = build(out)
    return {n: run(exe, out, n) for n in THREADS}


def case(text, name):
    for line in text.splitlines():
        if line.startswith(f"[case] {name}:"):
            return [int(v) for v in line.split(":", 1)[1].split()]
    raise AssertionError(f"no [case] {name} line in:\n{text}")


def pixels(text):
    """Only the [case] lines: the rest of the output names the thread count,
    which is exactly what the comparison below varies."""
    return [ln for ln in text.splitlines() if ln.startswith("[case] ")]


@needs_msvc
def test_the_game_filter_spreads_one_row_over_three(runs):
    """16/64, 32/64, 16/64 -- and nothing at all at distance two or three.

    255*32/64 is 127.5 and 255*16/64 is 63.75; the copy truncates, so 127 and
    63. The two zeros on each end are what separate this from the reading the
    plan used to carry, which put 8/64 out at three rows.
    """
    assert case(runs[1], "game-mid") == [0, 0, 63, 127, 63, 0, 0]


@needs_msvc
def test_the_sdk_filter_off_set_is_the_exact_identity(runs):
    """{0,0,21,22,21,0,0} is 0/64/0 once collapsed, so the copy must be what it
    was before the filter existed. This is the case that fails if taps 2 and 4
    are put anywhere but the current row -- including the five-row grouping the
    filter-off set alone cannot rule out."""
    assert case(runs[1], "off-mid") == [0, 0, 0, 255, 0, 0, 0]


@needs_msvc
def test_unwritten_filter_registers_are_not_a_zero_kernel(runs):
    """Zero is the register never having been written, not a request to
    multiply the frame by nothing. Reading it the other way copies black, which
    is what the renderer's own selftest reported the first time this ran:
    "render full-screen quad: 0 of 307200 red"."""
    assert case(runs[1], "unwritten") == [0, 0, 0, 255, 0, 0, 0]


@needs_msvc
def test_a_flat_field_survives_exactly(runs):
    """16+32+16 is 64, so a constant copies to itself. Catches a divisor or a
    rounding rule that the white-line cases would let through."""
    assert case(runs[1], "flat") == [200] * 5


@needs_msvc
def test_the_taps_clamp_to_the_copy_rectangle_not_the_efb(runs):
    """At the rectangle's first row the missing tap above is the row itself, so
    it keeps 16/64 + 32/64 = 48/64 of 255, which is 191. EFB_H is 528 and the
    rectangle is 480, so clamping to the EFB passes every other case here and
    fails only this one and its mirror."""
    assert case(runs[1], "top") == [-1, -1, 191, 63, 0]
    assert case(runs[1], "bottom") == [0, 63, 191, -1, -1]


@needs_msvc
def test_weights_that_do_not_sum_to_64_are_named(runs):
    """A set totalling 63 dims every pixel it copies, and no neighbours-are-zero
    test can see it. The copy applies it as given and says so."""
    assert "sum to 63, not 64" in runs[1], runs[1]


@needs_msvc
@pytest.mark.parametrize("threads", THREADS[1:])
def test_the_filtered_copy_does_not_depend_on_the_thread_count(runs, threads):
    """The filter reads the rows above and below each output row, and my_row
    gives those to other workers. If the copy did not drain the queue first,
    these would differ."""
    assert pixels(runs[threads]) == pixels(runs[1]), f"SOA_THREADS={threads} differs from 1"


def tex(text, name):
    for line in text.splitlines():
        if line.startswith(f"[tex] {name}:"):
            return line.split(":", 1)[1].strip()
    raise AssertionError(f"no [tex] {name} line in:\n{text}")


@needs_msvc
def test_deflicker_off_leaves_the_screen_copy_unfiltered_and_texture_copies_alone(tmp_path):
    """P5b: with SOA_DEFLICKER=0 the display copy under the game's weights is
    the identity copy, byte for byte, and a texture copy under the game's
    weights still differs from the identity -- the game's own effects keep
    their filter. Applying the switch to texture copies fails the second."""
    exe = build(tmp_path)
    env = {k: v for k, v in os.environ.items() if not k.startswith("SOA_")}
    env.update(SOA_THREADS="2", SOA_DEFLICKER="0", DRIVER_DEFLICKER="1")
    proc = subprocess.run(
        [str(exe)], capture_output=True, text=True, env=env, cwd=tmp_path, timeout=300, check=False
    )
    text = proc.stdout + proc.stderr
    assert proc.returncode == 0 and "[driver] done" in text, text
    assert "SOA_DEFLICKER=0: the screen copy is not deflickered" in text, text
    assert case(text, "screen-game") == case(text, "screen-off"), text
    assert case(text, "screen-game") == [0, 0, 0, 255, 0, 0, 0], text
    assert tex(text, "tex-game") != tex(text, "tex-off"), text
