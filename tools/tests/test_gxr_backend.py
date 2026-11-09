"""The renderer's backend seam (specs/gpu-backend.md V2), on a renderer-only
build: gx.c, gxr.c, gxr_tev.c and png.c, with a driver that sets a counting
backend in place of the worker pool.

What a GPU backend will rely on is checked here before one exists: commands
arrive in order and of the right kind, a copy's clear always as its own
command; a texture's generation moves when its bytes change and not
otherwise; the frame count and the presented count agree; and each command
carries the EFB it was built for. Then the 23 corpus captures, replayed
through the passthrough backend, must keep their manifest hashes.
"""

import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import scenario  # noqa: E402
from soa import toolchain  # noqa: E402

RUNTIME = ROOT / "runtime"
PROFILE = toolchain.profile(os.environ.get("SOA_CC"))

DRIVER = r"""
#define _CRT_SECURE_NO_WARNINGS
#include "cpu.h"
#include "gxr.h"
#include "gxr_cmd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t mmio_read32(CpuState* s, uint32_t ea) { (void)s; (void)ea; return 0; }
void hle_report(void) {}
long gxr_presented(void);
int gx_replay(CpuState* s, const char* base);

/* ---- a backend that only counts ----------------------------------------- */
static char log_kinds[512], log_efb[512], log_gens[512];
static size_t nk, ne, ng;
static void note(char* buf, size_t* n, const char* fmt, unsigned v)
{
    *n += (size_t)snprintf(buf + *n, 500 - *n, fmt, v);
}
static int cb_draw(const DrawCmd* D)
{
    note(log_kinds, &nk, "%u ", 0);
    note(log_efb, &ne, "%u ", D->efb);
    if (D->tev.tex[0].level[0]) note(log_gens, &ng, "%u ", D->tev.tex[0].tex_gen);
    return 1;
}
static int cb_copy(const DrawCmd* D) { note(log_kinds, &nk, "%u ", 1); note(log_efb, &ne, "%u ", D->efb); return 1; }
static int cb_clear(const DrawCmd* D) { note(log_kinds, &nk, "%u ", 2); note(log_efb, &ne, "%u ", D->efb); return 1; }
static const GxrBackend counting = {"counting", cb_draw, cb_copy, cb_clear, NULL, NULL};

/* ---- the GX pipe, as runtime/selftest.c drives it ---------------------- */
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

static void recipe(CpuState* s, int textured)
{
    static const float viewport[6] = {320.0f, -240.0f, 16777215.0f, 662.0f, 582.0f, 16777215.0f};
    static const float ortho[6] = {0.003125f, -0.0f, 0.004167f, -0.0f, -0.01f, -1.0f};
    static const float view[12] = {1, 0, 0, -320, 0, -1, 0, 240, 0, 0, 1, -100};
    static const float ident[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    static const uint32_t one = 1, matidx = 0x3CF3CF00u, chan = 0x441u, zero = 0, texgen = 5u << 7;
    bp_w(s, 0x59, 0x02ACABu); bp_w(s, 0x20, 0x156156u); bp_w(s, 0x21, 0x3D5335u);
    xf_f(s, 0x101A, 6, viewport);
    xf_f(s, 0x1020, 6, ortho); xf_w(s, 0x1026, 1, &one);
    xf_f(s, 0x0000, 12, view);
    cp_w(s, 0x30, matidx); xf_w(s, 0x1018, 1, &matidx);
    xf_w(s, 0x1009, 1, &one); xf_w(s, 0x100E, 1, &chan); xf_w(s, 0x1010, 1, &chan);
    xf_w(s, 0x1008, 1, &one);
    bp_w(s, 0xC1, 0x08BFF0u); bp_w(s, 0x00, textured ? 0x11 : 0x10);
    bp_w(s, 0xF6, 0x018064u); bp_w(s, 0xF7, 0x01806Eu); bp_w(s, 0xF8, 0x018060u); bp_w(s, 0xF9, 0x01806Cu);
    bp_w(s, 0xFA, 0x018065u); bp_w(s, 0xFB, 0x01806Du); bp_w(s, 0xFC, 0x01806Au); bp_w(s, 0xFD, 0x01806Eu);
    bp_w(s, 0x40, 0x17); bp_w(s, 0x41, 0x18); bp_w(s, 0xF3, 0x3F0000u); bp_w(s, 0x43, 0x40);
    cp_w(s, 0x50, 0x2200); cp_w(s, 0x70, 0x41377009u); cp_w(s, 0x80, 0xC8241209u); cp_w(s, 0x90, 0x04824120u);
    if (textured) {
        cp_w(s, 0x60, 1);
        xf_w(s, 0x103F, 1, &one); xf_w(s, 0x1040, 1, &texgen); xf_f(s, 4 * 60, 12, ident);
        bp_w(s, 0x28, 0x40); bp_w(s, 0xC0, 0x08F8AFu);
        bp_w(s, 0x30, 7); bp_w(s, 0x31, 3); bp_w(s, 0x80, 0); bp_w(s, 0x84, 0);
        bp_w(s, 0x88, 0x100C07u); bp_w(s, 0x94, 0x00100000u >> 5);
    } else {
        cp_w(s, 0x60, 0); xf_w(s, 0x103F, 1, &zero);
        bp_w(s, 0x28, 0); bp_w(s, 0xC0, 0x08AFFFu);
    }
}

static void quad(CpuState* s, int textured)
{
    static const float xy[4][2] = {{0, 0}, {640, 0}, {640, 480}, {0, 480}};
    int i;
    gp8(s, 0x80); gp16(s, 4);
    for (i = 0; i < 4; i++) {
        gpf(s, xy[i][0]); gpf(s, xy[i][1]); gpf(s, 50); gp32(s, 0xFF0000FFu);
        if (textured) { gpf(s, xy[i][0] / 640.0f); gpf(s, xy[i][1] / 480.0f); }
    }
}

/* filter: 1 the SDK's deflicker weights, 0 its filter-off set (identity) */
static void copy(CpuState* s, int to_screen, int filtered, uint32_t dest)
{
    bp_w(s, 0x53, filtered ? 0x30A208u : 0x595000u);
    bp_w(s, 0x54, filtered ? 0x00820Au : 0x000015u);
    bp_w(s, 0x49, 0); bp_w(s, 0x4A, 0x077E7Fu);
    bp_w(s, 0x4D, to_screen ? 0x28 : 640 / 4);
    bp_w(s, 0x4B, (dest & 0x1FFFFFu) >> 5);
    bp_w(s, 0x52, to_screen ? 0x4803u : 0x0840u); /* the clear bit on both; RGB565 to memory */
}

int main(int argc, char** argv)
{
    static CpuState s;
    const char* mode = argc > 1 ? argv[1] : "";
    s.mem = (uint8_t*)calloc(1, MEM1_SIZE);
    if (!s.mem) return 2;
    if (!strcmp(mode, "replay")) {
        int r = gx_replay(&s, argv[2]);
        gxr_report();
        return r;
    }
    gxr_set_backend(&counting);
    gxr_enable(1);
    gxr_reset_efb();
    if (!strcmp(mode, "synthetic")) {
        recipe(&s, 0);
        quad(&s, 0); quad(&s, 0);
        copy(&s, 0, 1, 0x00300000u); /* filtered, to a texture, with the clear bit */
        copy(&s, 0, 0, 0x00400000u); /* unfiltered and full scale, with the clear bit */
        quad(&s, 0);
        copy(&s, 1, 1, 0x01100000u); /* the screen, with the clear bit */
        gxr_flush();
        printf("frames: %u presented %ld\n", gx_frame_count(), gxr_presented());
    } else if (!strcmp(mode, "texgen")) {
        int i;
        for (i = 0; i < 32; i++) s.mem[0x00100000u + i] = (uint8_t)(i * 8);
        recipe(&s, 1);
        quad(&s, 1); quad(&s, 1);
        bp_w(&s, 0x66, 0); /* the game's texture invalidate: the epoch moves, the bytes do not */
        quad(&s, 1);
        s.mem[0x00100000u] ^= 0xFF;
        bp_w(&s, 0x66, 0);
        quad(&s, 1);
        gxr_flush();
    } else if (!strcmp(mode, "target")) {
        recipe(&s, 0);
        quad(&s, 0);
        gxr_set_target(1);
        quad(&s, 0);
        gxr_set_target(0);
        quad(&s, 0);
        gxr_flush();
    } else {
        fprintf(stderr, "modes: synthetic, texgen, target, replay <base>\n");
        return 2;
    }
    printf("kinds: %s\nefb: %s\ngens: %s\n", log_kinds, log_efb, log_gens);
    return 0;
}
"""

needs_cc = pytest.mark.skipif(
    toolchain.compiler_path(PROFILE) is None,
    reason=f"no {PROFILE.name}: the renderer cannot be built",
)


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    if toolchain.compiler_path(PROFILE) is None:
        pytest.skip(f"no {PROFILE.name}")
    tmp = tmp_path_factory.mktemp("backend")
    (tmp / "backend.c").write_text(DRIVER, encoding="utf-8")
    sources = [RUNTIME / f for f in ("gx.c", "gx_frame.c", "gxr.c", "gxr_tev.c", "png.c")] + [tmp / "backend.c"]
    p = toolchain.cc(
        [*PROFILE.cflags, "/c", "/I", str(RUNTIME), *map(str, sources), "/Fo" + str(tmp) + os.sep],
        tmp,
        PROFILE,
    )
    assert p.returncode == 0, (p.stdout or "") + (p.stderr or "")
    exe = tmp / ("backend" + PROFILE.exeext)
    objs = [str(tmp / (s.stem + PROFILE.objext)) for s in sources]
    p = toolchain.cc([*PROFILE.cflags, *objs, "/Fe" + str(exe), *PROFILE.linker], tmp, PROFILE)
    assert p.returncode == 0, (p.stdout or "") + (p.stderr or "")
    return exe


def run(exe, *args, **env) -> str:
    e = {k: v for k, v in os.environ.items() if not k.startswith("SOA_")}
    e.update(SOA_SETTINGS="0", SOA_RENDER="1", **env)
    p = subprocess.run(
        [str(exe), *args], env=e, capture_output=True, text=True, timeout=300, check=False
    )
    assert p.returncode == 0, p.stdout + p.stderr
    return p.stdout + p.stderr


def field(text: str, name: str) -> list[str]:
    line = next(ln for ln in text.splitlines() if ln.startswith(name + ":"))
    return line.split(":", 1)[1].split()


@needs_cc
def test_every_copy_with_the_clear_bit_is_followed_by_its_own_clear(driver):
    """Two draws, a filtered copy to texture with the clear bit, an unfiltered
    full-scale copy with the clear bit, a draw, a screen copy with the clear
    bit. Without a backend the unfiltered copy carries its clear inside it;
    with one, every clear is its own command, so a backend that replaced the
    copy can never lose the EFB clear between frames."""
    out = run(driver, "synthetic")
    assert field(out, "kinds") == ["0", "0", "1", "2", "1", "2", "0", "1", "2"], out
    assert "rasterizing on 0 worker threads" in out, out


@needs_cc
def test_the_frame_count_and_the_presented_count_agree(driver):
    """The backend cannot reach the presented counter; the hook counts a screen
    copy the backend returned 1 for."""
    out = run(driver, "synthetic")
    frames, _, presented = field(out, "frames")
    assert frames == presented == "1", out


@needs_cc
def test_a_textures_generation_moves_only_when_its_bytes_do(driver):
    """Four draws of one texture: two in an epoch, one after an invalidate that
    changed nothing, one after its bytes changed."""
    gens = [int(g) for g in field(run(driver, "texgen"), "gens")]
    assert len(gens) == 4, gens
    assert gens[0] == gens[1] == gens[2] < gens[3], gens


@needs_cc
def test_each_command_carries_the_efb_it_was_built_for(driver):
    """H17's routing: the target set for one draw reaches that command alone,
    so a consumer reads it from the command and never from global state."""
    assert field(run(driver, "synthetic"), "efb") == ["0"] * 9
    assert field(run(driver, "target"), "efb") == ["0", "1", "0"]


CORPUS = (
    [b for b in scenario.find_captures(scenario.FIFO_DIR)[0]] if scenario.FIFO_DIR.exists() else []
)


@needs_cc
@pytest.mark.skipif(not CORPUS, reason="no build/fifo: the corpus is on the owner's machine only")
def test_the_corpus_through_the_passthrough_keeps_its_hashes(driver):
    """The CPU path's own code, called through the backend hook: every corpus
    frame must hash as the manifest pins it, and the passthrough must have
    drawn something, so the hook is not a frame skipped by mistake."""
    pinned = scenario.read_manifest(scenario.MANIFEST)
    bad = []
    for base in CORPUS:
        out = run(driver, "replay", str(base), SOA_HASH="1", SOA_GXR_BACKEND="passthrough")
        hashes = scenario.frame_hashes(out)
        draws = next((ln for ln in out.splitlines() if "passthrough backend:" in ln), "")
        n = int(draws.split(":")[1].split()[0]) if draws else 0
        if hashes != [pinned[base.name][1]] or n == 0:
            bad.append(f"{base.name}: {hashes} against {pinned[base.name][1]}, {n} draws")
    assert not bad and len(CORPUS) == len(pinned), f"{len(bad)} of {len(CORPUS)} differ: {bad}"
