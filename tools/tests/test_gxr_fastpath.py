"""Differential tests for the pixel path's specialised cases (PLAN-60FPS-MODS H15c).

tev_prepare recognises the one-stage shapes most pixels use (the vertex colour
alone; texture times vertex colour) and tev_pixel runs them directly;
pixel_prepare does the same for the blend most pixels use. Each is meant to be
the general formula with its constants put in, so it must give the general
path's answer for every input -- and the plan's warning about specialising is
that a wrong body neither crashes nor moves a replay hash when no capture
happens to use it. So these compare the two paths on random inputs:

- random BP register sets, weighted towards the recognised shapes but with
  every plain-looking field sometimes not plain (a swap, a bias, a shift, a
  second stage, the other colour channel), go through the real tev_prepare,
  and every pixel is run through tev_pixel twice -- as prepared, and with the
  shape switched off -- and must agree, colour and alpha test;
- random source and destination colours go through blend_pixel with the
  specialised case and with the general one;
- random bilinear samples -- texture sizes powers of two and not, all three
  wrap modes, coordinates across and outside the texture, exact texels and
  half-texel points among them -- go through sample_level's integer SIMD
  (H15d) and its scalar loop, and must give the same bytes.

The first driver includes gxr_tev.c and the second gxr.c, to reach their
statics. No disc and no game data.
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

TEV_DRIVER = r"""
#define _CRT_SECURE_NO_WARNINGS
#include "gxr_tev.c"

uint32_t mmio_read32(CpuState* s, uint32_t ea) { (void)s; (void)ea; return 0; }
void hle_report(void) {}

static uint32_t g_r = 0x2545F491u;
static uint32_t rnd(void) { g_r ^= g_r << 13; g_r ^= g_r >> 17; g_r ^= g_r << 5; return g_r; }
static uint32_t pick(const uint32_t* v, int n) { return v[rnd() % (uint32_t)n]; }

#define TEXADDR 0x00200000u

int main(void)
{
    static CpuState S;
    static uint32_t bp[0x100];
    /* (ca, cb, cc, cd) and (aa, ab, ac, ad): the recognised shapes first, then
     * near misses the recogniser must refuse. */
    static const uint32_t cols[][4] = {{10, 15, 15, 15}, {15, 15, 15, 10}, {15, 8, 10, 15}, {15, 10, 8, 15}, {8, 15, 15, 15}, {15, 8, 10, 2}};
    static const uint32_t alps[][4] = {{6, 7, 7, 7}, {5, 7, 7, 7}, {7, 7, 7, 5}, {7, 4, 5, 7}, {4, 7, 7, 7}, {7, 5, 4, 7}};
    long long trials = 0, fast = 0, per[3][4] = {{0}}, pixels = 0, bad = 0;
    int t, p, i;
    S.mem = (uint8_t*)calloc(1, MEM_IMAGE_SIZE);
    if (!S.mem) return 2;
    for (i = 0; i < 16 * 16 * 4; i++) S.mem[TEXADDR + i] = (uint8_t)rnd();
    tex_set_memory(&S);
    for (t = 0; t < 4000; t++) {
        TevSetup T, G;
        const uint32_t* c = cols[rnd() % 6];
        const uint32_t* a = alps[rnd() % 6];
        uint32_t odd = rnd() % 8; /* 0: one field made not plain */
        uint32_t cenv = (c[0] << 12) | (c[1] << 8) | (c[2] << 4) | c[3], aenv = (a[0] << 13) | (a[1] << 10) | (a[2] << 7) | (a[3] << 4);
        uint32_t texen = rnd() % 4 != 0;
        memset(bp, 0, sizeof bp);
        cenv |= 1u << 19;                        /* clamp */
        cenv |= (rnd() % 2) << 20;               /* shift 0 or 1 */
        aenv |= 1u << 19;
        if (odd == 0) {
            switch (rnd() % 7) {
            case 0: cenv |= 1u << 16; break;        /* a bias */
            case 1: cenv |= 1u << 18; break;        /* subtract */
            case 2: cenv &= ~(1u << 19); break;     /* no clamp */
            case 3: cenv |= 1u << 22; break;        /* into another register */
            case 4: aenv |= 1u << 20; break;        /* an alpha shift */
            case 5: aenv |= 1u; break;              /* a raster swap */
            default: cenv |= 2u << 20; break;       /* shift 2 */
            }
        }
        bp[0x00] = (rnd() % 10 == 0 ? 1u : 0u) << 10; /* sometimes a second stage */
        bp[0x28] = (texen << 6) | ((rnd() % 10 == 0 ? 1u : 0u) << 7); /* map 0, coord 0; sometimes channel 1 */
        bp[0x29] = bp[0x28];
        bp[0xC0] = cenv; bp[0xC1] = aenv; bp[0xC2] = cenv; bp[0xC3] = aenv;
        bp[0xF6] = 0x018064u | ((rnd() % 32) << 4) | ((rnd() % 32) << 9); /* identity swap table 0, random konst selectors */
        bp[0xF7] = 0x01806Eu; bp[0xF8] = 0x018060u; bp[0xF9] = 0x01806Cu;
        bp[0xFA] = 0x018065u; bp[0xFB] = 0x01806Du; bp[0xFC] = 0x01806Au; bp[0xFD] = 0x01806Eu;
        bp[0xF3] = (rnd() % 256) | ((rnd() % 256) << 8) | ((rnd() % 8) << 16) | ((rnd() % 8) << 19) | ((rnd() % 4) << 22);
        bp[0x80] = (rnd() % 3) | ((rnd() % 3) << 2) | ((rnd() % 2) << 4);
        bp[0x88] = (6u << 20) | (15u << 10) | 15u; /* RGBA8 16x16 */
        bp[0x94] = TEXADDR >> 5;
        bp[0x30] = 15; bp[0x31] = 15;
        tev_register_written(0xE0, (1u << 23) | (rnd() % 256) | ((rnd() % 256) << 12));
        tev_register_written(0xE1, (1u << 23) | (rnd() % 256) | ((rnd() % 256) << 12));
        tev_prepare(bp, &T);
        trials++;
        if (T.fast_c) { fast++; per[T.fast_c][T.fast_a]++; }
        G = T;
        G.fast_c = G.fast_a = 0;
        for (p = 0; p < 64; p++) {
            int ras[2][4];
            float tex[8][4];
            uint8_t o1[4], o2[4];
            int a1 = 0, a2 = 0, k;
            for (k = 0; k < 4; k++) { ras[0][k] = (int)(rnd() % 256); ras[1][k] = (int)(rnd() % 256); }
            memset(tex, 0, sizeof tex);
            tex[0][0] = (float)(rnd() % 4000) / 1000.0f - 1.0f;
            tex[0][1] = (float)(rnd() % 4000) / 1000.0f - 1.0f;
            tex[0][2] = rnd() % 4 ? 1.0f : (float)(rnd() % 3000 + 1) / 1000.0f;
            tev_pixel(&T, (const int (*)[4])ras, (const float (*)[4])tex, o1, &a1);
            tev_pixel(&G, (const int (*)[4])ras, (const float (*)[4])tex, o2, &a2);
            pixels++;
            if (memcmp(o1, o2, 4) || a1 != a2) {
                if (++bad <= 5)
                    printf("MISMATCH trial %d fast %u/%u: %d,%d,%d,%d/%d vs %d,%d,%d,%d/%d\n", t, T.fast_c, T.fast_a,
                           o1[0], o1[1], o1[2], o1[3], a1, o2[0], o2[1], o2[2], o2[3], a2);
            }
        }
    }
    printf("[fastpath] tev %lld setups, %lld recognised (%lld %lld %lld / %lld %lld %lld), %lld pixels, %lld mismatches\n",
           trials, fast, per[1][1], per[1][2], per[1][3], per[2][1], per[2][2], per[2][3], pixels, bad);
    return 0;
}
"""

BILINEAR_DRIVER = r"""
#define _CRT_SECURE_NO_WARNINGS
#include "gxr_tev.c"

uint32_t mmio_read32(CpuState* s, uint32_t ea) { (void)s; (void)ea; return 0; }
void hle_report(void) {}

static uint32_t g_r = 0x9E3779B9u;
static uint32_t rnd(void) { g_r ^= g_r << 13; g_r ^= g_r >> 17; g_r ^= g_r << 5; return g_r; }

int main(void)
{
    static uint8_t img[64 * 64 * 4];
    long long n = 0, bad = 0;
    int t, p, i;
    simd_decide();
    printf("[fastpath] simd %d\n", g_gxr_simd);
    for (i = 0; i < (int)sizeof img; i++) img[i] = (uint8_t)rnd();
    for (t = 0; t < 3000 && g_gxr_simd > 0; t++) {
        TexCfg C;
        memset(&C, 0, sizeof C);
        if (rnd() % 2) { C.lw[0] = 1 << (rnd() % 7); C.lh[0] = 1 << (rnd() % 7); }
        else { C.lw[0] = 1 + (int)(rnd() % 64); C.lh[0] = 1 + (int)(rnd() % 64); }
        C.level[0] = img; C.nlevels = 1; C.w = C.lw[0]; C.h = C.lh[0];
        C.wrap_s = rnd() % 3; C.wrap_t = rnd() % 3; C.linear = 1;
        for (p = 0; p < 200; p++) {
            float u = ((float)(int)(rnd() % 20001) - 10000.0f) / 100.0f, v = ((float)(int)(rnd() % 20001) - 10000.0f) / 100.0f;
            uint8_t o1[4], o2[4];
            if (p % 7 == 0) { u = (float)(int)(rnd() % 201) - 100.0f; v = (float)(int)(rnd() % 201) - 100.5f; }
            g_gxr_simd = 1; sample_level(&C, 0, u, v, o1);
            g_gxr_simd = 0; sample_level(&C, 0, u, v, o2);
            g_gxr_simd = 1;
            n++;
            if (memcmp(o1, o2, 4) && ++bad <= 5)
                printf("MISMATCH %dx%d wrap %u/%u at %g,%g: %d,%d,%d,%d vs %d,%d,%d,%d\n", C.lw[0], C.lh[0], C.wrap_s, C.wrap_t,
                       u, v, o1[0], o1[1], o1[2], o1[3], o2[0], o2[1], o2[2], o2[3]);
        }
    }
    printf("[fastpath] bilinear %lld samples, %lld mismatches\n", n, bad);
    return 0;
}
"""

BLEND_DRIVER = r"""
#define _CRT_SECURE_NO_WARNINGS
#include "gxr.c"

uint32_t mmio_read32(CpuState* s, uint32_t ea) { (void)s; (void)ea; return 0; }
void hle_report(void) {}

int main(void)
{
    uint32_t r = 0x9E3779B9u;
    long long n = 0, bad = 0;
    int t, k;
    for (t = 0; t < 400000; t++) {
        PixelCfg px;
        uint8_t src[4], dst[4], o1[4], o2[4];
        memset(&px, 0, sizeof px);
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        for (k = 0; k < 4; k++) { src[k] = (uint8_t)(r >> (8 * k)); }
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        for (k = 0; k < 4; k++) { dst[k] = (uint8_t)(r >> (8 * k)); }
        px.col_upd = 1;
        px.alpha_upd = (int)(r & 1);
        px.const_alpha = (r >> 1) & 1 ? (int)((r >> 2) & 0xFF) : -1;
        if (t & 1) { px.blend_en = 1; px.sfac = 4; px.dfac = 5; } /* the source-alpha blend */
        /* else neither blend nor logic op: the source as it is */
        px.blend_kind = t & 1 ? 2u : 1u;
        memcpy(g_efb[0][0], dst, 4);
        blend_pixel(&px, 0, 0, src);
        memcpy(o1, g_efb[0][0], 4);
        px.blend_kind = 0;
        memcpy(g_efb[0][0], dst, 4);
        blend_pixel(&px, 0, 0, src);
        memcpy(o2, g_efb[0][0], 4);
        n++;
        if (memcmp(o1, o2, 4) && ++bad <= 5)
            printf("MISMATCH kind %d: src %d,%d,%d,%d dst %d,%d,%d,%d -> %d,%d,%d,%d vs %d,%d,%d,%d\n", t & 1 ? 2 : 1,
                   src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2], dst[3], o1[0], o1[1], o1[2], o1[3], o2[0], o2[1], o2[2], o2[3]);
    }
    printf("[fastpath] blend %lld pixels, %lld mismatches\n", n, bad);
    return 0;
}
"""


# SOA_CC picks the toolchain profile (portability L3a): the SIMD path is the
# part of the renderer that differs by compiler, and L4a's clang-cl leg runs
# this module with SOA_CC=clang-cl.
PROFILE = toolchain.profile(os.environ.get("SOA_CC"))


def build(tmp, name, driver, sources):
    (tmp / f"{name}.c").write_text(driver, encoding="utf-8")
    exe = tmp / f"{name}.exe"
    proc = toolchain.cc(
        [
            *PROFILE.cflags,
            "/I",
            str(RUNTIME),
            *[str(RUNTIME / s) for s in sources],
            str(tmp / f"{name}.c"),
            "/Fo" + str(tmp) + os.sep,
            "/Fe" + str(exe),
            *PROFILE.linker,  # the NDK clang-cl has no lld-link
        ],
        tmp,
        PROFILE,
    )
    assert proc.returncode == 0, (proc.stdout or "") + (proc.stderr or "")
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300, check=False)
    assert run.returncode == 0, run.stdout + run.stderr
    return run.stdout


needs_msvc = pytest.mark.skipif(
    toolchain.compiler_path(PROFILE) is None,
    reason=f"no MSVC ({PROFILE.name}): the renderer cannot be built here",
)


@needs_msvc
def test_the_tev_shapes_give_the_general_answer(tmp_path):
    out = build(tmp_path, "tev", TEV_DRIVER, ["gx.c", "gx_frame.c", "gxr.c", "png.c"])
    m = re.search(
        r"\[fastpath\] tev (\d+) setups, (\d+) recognised \((\d+) (\d+) (\d+) / (\d+) (\d+) (\d+)\), (\d+) pixels, (\d+) mismatches",
        out,
    )
    assert m, out
    setups, fast, *shapes, pixels, bad = (int(v) for v in m.groups())
    assert bad == 0, out
    # The recogniser took the shapes and refused the near misses: every shape
    # was exercised, and not every setup was taken.
    assert all(n > 20 for n in shapes), out
    assert 0 < fast < setups, out


@needs_msvc
def test_the_simd_bilinear_gives_the_scalar_answer(tmp_path):
    out = build(tmp_path, "bilinear", BILINEAR_DRIVER, ["gx.c", "gx_frame.c", "gxr.c", "png.c"])
    if "[fastpath] simd 1" not in out:
        pytest.skip("this CPU has no SSE4.1, so there is no SIMD path to compare")
    m = re.search(r"\[fastpath\] bilinear (\d+) samples, (\d+) mismatches", out)
    assert m and int(m[1]) == 3000 * 200 and m[2] == "0", out


@needs_msvc
def test_the_blend_cases_give_the_general_answer(tmp_path):
    out = build(tmp_path, "blend", BLEND_DRIVER, ["gx.c", "gx_frame.c", "gxr_tev.c", "png.c"])
    m = re.search(r"\[fastpath\] blend (\d+) pixels, (\d+) mismatches", out)
    assert m and int(m[1]) > 0 and m[2] == "0", out
