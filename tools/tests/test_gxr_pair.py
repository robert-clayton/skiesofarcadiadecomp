"""Tests for the in-between image of two frames (PLAN-60FPS-MODS H10).

``soa.exe --replay F F+1`` renders F recording every draw's key and clip-space
positions, renders F+1 as it is, then renders F+1 again with each draw matched
in F moved to ``(1-t)*F + t*F+1`` (runtime/gxr.c, "frame pairs"). Whether that
is right cannot be judged from a game capture alone -- nobody knows where the
in-between frame's pixels belong -- so this builds the renderer on its own,
captures synthetic frames through the real capture path (gx.c's frame_end,
into a temporary SOA_FIFO_DIR), and replays them through the real
``gx_replay_pair``.

Every scene below is drawn three ways: as F, as F+1 with its geometry moved by
2d, and as C with it moved by d. The midpoint of (F, F+1) must then be C, pixel
for pixel. Every colour is a pure primary, flat across its triangle, and every
edge stays at least 0.25 px in x from every pixel centre (centres sit at .5,
vertical edges at .25, the hypotenuses on x+y = n+.25), so rounding in the
interpolation cannot flip a pixel's coverage. Nothing is pinned: every
comparison is between two renders made in the same test, so there is no
baseline to bless.

Built the way ``test_gxr_queue.py`` builds it: the renderer on its own, a
synthetic command stream, no disc and no game data.
"""

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import fifopair  # noqa: E402
from soa import toolchain  # noqa: E402

RUNTIME = ROOT / "runtime"
SOURCES = ["gx.c", "gx_frame.c", "gxr.c", "gxr_tev.c", "png.c"]
GXR = (RUNTIME / "gxr.c").read_text(encoding="utf-8")

# The stream helpers and setup() are test_gxr_queue.py's, whole: viewport
# 320,-240 at origin 662,582 less the scissor offset of 342, a view matrix of
# (x-320, 240-y, z-100) and an orthographic projection, so a vertex at (x, y)
# lands on pixel (x, y) to within about 0.02 px. Added here: a grey clear, so
# gxr_reset_efb starts every replay from grey, and for the perspective scenes a
# projection that keeps that mapping at view depth 64.
DRIVER = r"""
#define _CRT_SECURE_NO_WARNINGS
#include "cpu.h"
#include "gxr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t mmio_read32(CpuState* s, uint32_t ea) { (void)s; (void)ea; return 0; }
void hle_report(void) {}
int gx_replay(CpuState* s, const char* base);
int gx_replay_pair(CpuState* s, const char* a, const char* b);

#define RED 0xFF0000FFu
#define GREEN 0x00FF00FFu
#define BLUE 0x0000FFFFu

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

static void setup(CpuState* s, int persp)
{
    static const float viewport[6] = {320.0f, -240.0f, 16777215.0f, 662.0f, 582.0f, 16777215.0f};
    static const float ortho[6] = {0.003125f, -0.0f, 0.004167f, -0.0f, -0.01f, -1.0f};
    /* At view depth 64 this maps as the orthographic one does: x/w = 0.2x/64 = x/320. */
    static const float perspective[6] = {0.2f, 0.0f, 0.26666667f, 0.0f, 0.5f, 0.0f};
    static const float view[12] = {1, 0, 0, -320, 0, -1, 0, 240, 0, 0, 1, -100};
    static const uint32_t one = 1, matidx = 0x3CF3CF00u, chan = 0x441u, zero = 0;
    bp_w(s, 0x59, 0x02ACABu); bp_w(s, 0x20, 0x156156u); bp_w(s, 0x21, 0x3D5335u);
    xf_f(s, 0x101A, 6, viewport);
    xf_f(s, 0x1020, 6, persp ? perspective : ortho); xf_w(s, 0x1026, 1, persp ? &zero : &one);
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
    bp_w(s, 0x4F, 0x00FF80u); bp_w(s, 0x50, 0x008080u); bp_w(s, 0x51, 0xFFFFFFu); /* clear: grey, far */
}

/* Stage 0 samples map 0 and passes the rasterized colour through: the texture
 * address is in the draw's key, and no texel reaches a pixel. */
static void use_texture(CpuState* s, uint32_t addr, unsigned fmt, unsigned w, unsigned h)
{
    bp_w(s, 0x80, 0); bp_w(s, 0x84, 0);
    bp_w(s, 0x88, ((fmt & 15) << 20) | ((h - 1) << 10) | (w - 1));
    bp_w(s, 0x94, (addr >> 5) & 0x1FFFFFu);
    bp_w(s, 0x98, 0);
    bp_w(s, 0x30, w - 1); bp_w(s, 0x31, h - 1);
    bp_w(s, 0x28, 0x40);
}

static void tri3(CpuState* s, float x0, float y0, float x1, float y1, float x2, float y2, float z, uint32_t rgba)
{
    gp8(s, 0x90); gp16(s, 3);
    vertex(s, x0, y0, z, rgba); vertex(s, x1, y1, z, rgba); vertex(s, x2, y2, z, rgba);
}

static void tri(CpuState* s, float x, float y, float z, uint32_t rgba) { tri3(s, x, y, x + 64.0f, y, x, y + 64.0f, z, rgba); }

static void quad(CpuState* s, float x0, float y0, float x1, float y1, uint32_t rgba)
{
    gp8(s, 0x80); gp16(s, 4);
    vertex(s, x0, y0, 50.0f, rgba); vertex(s, x1, y0, 50.0f, rgba);
    vertex(s, x1, y1, 50.0f, rgba); vertex(s, x0, y1, 50.0f, rgba);
}

/* A copy of the EFB rectangle at (0,0) to memory; 0x800 in v clears what it copied. */
static void copy_efb(CpuState* s, uint32_t dest, unsigned w, unsigned h, uint32_t v)
{
    bp_w(s, 0x49, 0);
    bp_w(s, 0x4A, ((h - 1) << 10) | (w - 1));
    bp_w(s, 0x4D, 0x28);
    bp_w(s, 0x4B, (dest >> 5) & 0x1FFFFFu);
    bp_w(s, 0x52, v);
}

static void present(CpuState* s)
{
    bp_w(s, 0x49, 0); bp_w(s, 0x4A, 0x077E7F); bp_w(s, 0x4D, 0x28);
    bp_w(s, 0x4B, (0x81100000u & 0x1FFFFFFFu) >> 5);
    bp_w(s, 0x52, 0x4003);
    gxr_flush();
}

/* Frame k of a scene: 1 is F, 2 is F+1 with the geometry moved by 2d, 3 is C
 * with it moved by d, and the rest are the scene's own. */
static void draw_frame(CpuState* s, const char* scene, int k)
{
    static const float dx[4] = {0, 0, 32, 16}, dy[4] = {0, 0, 16, 8};
    bp_w(s, 0x28, 0); /* no texture: the frame before may have left one on */
    if (!strcmp(scene, "move")) {
        tri(s, 100.25f + dx[k], 100.0f + dy[k], 50.0f, RED);
    } else if (!strcmp(scene, "persp")) {
        tri(s, 100.25f + dx[k], 100.0f + dy[k], 36.0f, RED); /* view z -64 */
    } else if (!strcmp(scene, "depth")) {
        /* view x 32..96, y -32..32, from view z -64 (screen x 352..416) to -192 (330.7..352) */
        float z = k == 1 ? 36.0f : -92.0f;
        tri3(s, 352.0f, 208.0f, 416.0f, 208.0f, 352.0f, 272.0f, z, RED);
    } else if (!strcmp(scene, "tex")) {
        /* A textured quad moving by 2e; 4 is D, the triangle at d and the quad where F+1 has it. */
        static const float ex[4] = {0, 0, -40, -20}, ey[4] = {0, 0, 20, 10};
        int t = k == 4 ? 3 : k, q = k == 4 ? 2 : k;
        tri(s, 100.25f + dx[t], 100.0f + dy[t], 50.0f, RED);
        use_texture(s, 0x00400000u, 4, 16, 16);
        quad(s, 300.25f + ex[q], 200.0f + ey[q], 340.25f + ex[q], 240.0f + ey[q], BLUE);
    } else if (!strcmp(scene, "copies")) {
        /* A copy to texture of the red triangle, then a clearing one of the
         * whole screen that takes the triangle and the green quad with it. */
        tri(s, 100.25f + dx[k], 100.0f + dy[k], 50.0f, RED);
        copy_efb(s, 0x00400000u, 256, 256, 0x43);
        quad(s, 300.25f + dx[k], 300.0f + dy[k], 340.25f + dx[k], 340.0f + dy[k], GREEN);
        copy_efb(s, 0x00600000u, 640, 480, 0x843);
        quad(s, 400.25f + dx[k], 100.0f + dy[k], 440.25f + dx[k], 140.0f + dy[k], BLUE);
    } else if (!strcmp(scene, "twins")) {
        /* Two triangles with one key (colour is a vertex attribute, not part of
         * it). 4 is F+1 with the two in the other order; 5 is where that order
         * puts both: F's first with F+1's first is (a + b + 2d) / 2. */
        if (k == 4) {
            tri(s, 292.25f + 32.0f, 164.0f + 16.0f, 50.0f, BLUE);
            tri(s, 100.25f + 32.0f, 100.0f + 16.0f, 50.0f, RED);
        } else if (k == 5) {
            tri(s, 212.25f, 140.0f, 50.0f, BLUE);
            tri(s, 212.25f, 140.0f, 50.0f, RED);
        } else {
            tri(s, 100.25f + dx[k], 100.0f + dy[k], 50.0f, RED);
            tri(s, 292.25f + dx[k], 164.0f + dy[k], 50.0f, BLUE);
        }
    }
}

static const struct { const char* name; int persp, frames; } SCENES[] = {
    {"move", 0, 3}, {"persp", 1, 3}, {"depth", 1, 2}, {"tex", 0, 4}, {"copies", 0, 3}, {"twins", 0, 5},
};

/* build/fifo is the pinned corpus (CLAUDE.md): a capture must go anywhere else. */
static int is_corpus(const char* d)
{
    size_t n = strlen(d);
    while (n && (d[n - 1] == '/' || d[n - 1] == '\\')) n--;
    return n >= 10 && (_strnicmp(d + n - 10, "build/fifo", 10) == 0 || _strnicmp(d + n - 10, "build\\fifo", 10) == 0);
}

static int capture(CpuState* s, const char* scene)
{
    const char* dir = getenv("SOA_FIFO_DIR");
    char dump[64] = "SOA_FIFO_DUMP=";
    int i, k, n = -1, persp = 0;
    for (i = 0; i < (int)(sizeof SCENES / sizeof SCENES[0]); i++)
        if (!strcmp(SCENES[i].name, scene)) { n = SCENES[i].frames; persp = SCENES[i].persp; }
    if (n < 0) { fprintf(stderr, "[pairtest] no scene %s\n", scene); return 2; }
    if (!dir || !*dir || is_corpus(dir)) { fprintf(stderr, "[pairtest] SOA_FIFO_DIR must be set, and not to build/fifo\n"); return 4; }
    for (k = 1; k <= n; k++) sprintf(dump + strlen(dump), k > 1 ? ",%d" : "%d", k);
    _putenv(dump);
    _putenv("SOA_RENDER=");
    /* Frame 0 is never captured (its stream starts before the capture list is
     * read), so it holds the setup, and its end is frame 1's .regs. */
    setup(s, persp);
    present(s);
    for (k = 1; k <= n; k++) { draw_frame(s, scene, k); present(s); }
    return 0;
}

static uint64_t fnv(const uint8_t* p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    while (n--) { h ^= *p++; h *= 1099511628211ull; }
    return h;
}

/* What the final screen holds: the box and count of each pure primary. */
static void report(CpuState* s)
{
    static const struct { const char* name; uint8_t r, g, b; } C[] = {{"red", 255, 0, 0}, {"green", 0, 255, 0}, {"blue", 0, 0, 255}};
    int w, h, x, y, c;
    const uint8_t* px = gxr_screen(&w, &h);
    for (c = 0; c < 3; c++) {
        int x0 = w, y0 = h, x1 = -1, y1 = -1, n = 0;
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                const uint8_t* p = px + ((size_t)y * EFB_W + x) * 4;
                if (p[0] != C[c].r || p[1] != C[c].g || p[2] != C[c].b) continue;
                n++;
                if (x < x0) x0 = x;
                if (y < y0) y0 = y;
                if (x > x1) x1 = x;
                if (y > y1) y1 = y;
            }
        printf("[pairtest] box %s %d %d %d %d %d\n", C[c].name, x0, y0, x1, y1, n);
    }
    printf("[pairtest] copy %016llx\n", (unsigned long long)fnv(s->mem + 0x00400000u, 0x40000));
}

int main(int argc, char** argv)
{
    static CpuState s;
    int rc;
    s.mem = (uint8_t*)calloc(1, MEM_IMAGE_SIZE);
    if (!s.mem || argc < 3) return 2;
    if (!strcmp(argv[1], "capture")) return capture(&s, argv[2]);
    _putenv("SOA_RENDER=1");
    _putenv("SOA_SNAP=");
    _putenv("SOA_FIFO_DUMP=");
    if (!gxr_enabled()) return 3;
    if (!strcmp(argv[1], "one")) rc = gx_replay(&s, argv[2]);
    else if (!strcmp(argv[1], "pair") && argc > 3) rc = gx_replay_pair(&s, argv[2], argv[3]);
    else return 2;
    if (rc) return rc;
    report(&s);
    fflush(stdout);
    return 0;
}
"""

SCENES = ("move", "persp", "depth", "tex", "copies", "twins")

# name: (mode, captures..., SOA_PAIR_T). A capture is "<scene>/<frame>";
# "tex/mut" and "move/strip" are made by the fixture from real ones.
RUNS = {
    "move.F": ("one", "move/0001"),
    "move.F1": ("one", "move/0002"),
    "move.C": ("one", "move/0003"),
    "move.pair": ("pair", "move/0001", "move/0002"),
    "move.t0": ("pair", "move/0001", "move/0002", "0"),
    "move.t1": ("pair", "move/0001", "move/0002", "1"),
    "move.self": ("pair", "move/0001", "move/0001"),
    "persp.C": ("one", "persp/0003"),
    "persp.pair": ("pair", "persp/0001", "persp/0002"),
    "depth.F": ("one", "depth/0001"),
    "depth.F1": ("one", "depth/0002"),
    "depth.pair": ("pair", "depth/0001", "depth/0002"),
    "tex.C": ("one", "tex/0003"),
    "tex.D": ("one", "tex/0004"),
    "tex.pair": ("pair", "tex/0001", "tex/0002"),
    "tex.mut": ("pair", "tex/0001", "tex/mut"),
    "copies.F1": ("one", "copies/0002"),
    "copies.C": ("one", "copies/0003"),
    "copies.pair": ("pair", "copies/0001", "copies/0002"),
    "twins.C": ("one", "twins/0003"),
    "twins.C2": ("one", "twins/0005"),
    "twins.pair": ("pair", "twins/0001", "twins/0002"),
    "twins.swap": ("pair", "twins/0001", "twins/0004"),
}

THREAD_COUNTS = ("1", "3")

REPORT = re.compile(
    r"\[pair\] midpoint at t=[\d.]+: (\d+) of (\d+) draws matched \(\d+ vertices interpolated\), "
    r"(\d+) drawn from F\+1, (\d+) demoted .*?, (\d+) copies to texture skipped \((\d+) clears kept\)"
)

needs_msvc = pytest.mark.skipif(
    toolchain.cl_path() is None, reason="no MSVC: the renderer cannot be built here"
)


def clean_env() -> dict:
    return {k: v for k, v in os.environ.items() if not k.startswith("SOA_")}


def parse(text: str) -> dict:
    out = {
        "hashes": re.findall(r"\[gxr\] frame \d+ \d+x\d+ hash (\w+)", text),
        "boxes": {
            m[0]: tuple(int(v) for v in m[1:])
            for m in re.findall(
                r"\[pairtest\] box (\w+) (-?\d+) (-?\d+) (-?\d+) (-?\d+) (\d+)", text
            )
        },
        "copy": re.search(r"\[pairtest\] copy (\w+)", text),
        "report": REPORT.search(text),
        "text": text,
    }
    out["copy"] = out["copy"].group(1) if out["copy"] else None
    if out["report"]:
        keys = ("matched", "draws", "from_f1", "demoted", "skipped", "kept")
        out["report"] = dict(zip(keys, (int(v) for v in out["report"].groups()), strict=True))
    return out


@pytest.fixture(scope="module")
def world(tmp_path_factory):
    if toolchain.cl_path() is None:
        pytest.skip("no MSVC")
    out = tmp_path_factory.mktemp("pair")
    (out / "driver.c").write_text(DRIVER, encoding="utf-8")
    exe = out / "pair.exe"
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
    env = clean_env()

    def run(*args, extra=None):
        return subprocess.run(
            [str(exe), *args],
            capture_output=True,
            text=True,
            env={**env, **(extra or {})},
            cwd=out,
            timeout=300,
            check=False,
        )

    for scene in SCENES:
        d = out / scene
        d.mkdir()
        cap = run("capture", scene, extra={"SOA_FIFO_DIR": str(d)})
        assert cap.returncode == 0, cap.stdout + cap.stderr
    # F+1 of the texture scene with its one texture address changed: the
    # quad's key differs, so it has no match and must be drawn as F+1 draws it.
    fifo = bytearray((out / "tex" / "0002.fifo").read_bytes())
    at = [m.start() for m in re.finditer(rb"\x61\x94", bytes(fifo))]
    assert len(at) == 1, f"expected the quad's one texture address in the stream, found {len(at)}"
    fifo[at[0] + 2 : at[0] + 5] = ((0x00500000 >> 5) & 0x1FFFFF).to_bytes(3, "big")
    (out / "tex" / "mut.fifo").write_bytes(bytes(fifo))
    for ext in (".regs", ".ram"):
        shutil.copyfile(out / "tex" / ("0002" + ext), out / "tex" / ("mut" + ext))
    # F of the moving triangle without its screen copy, which ends the stream.
    fifo = (out / "move" / "0001.fifo").read_bytes()
    assert fifo[-5:-3] == b"\x61\x52", "the capture no longer ends with its screen copy"
    (out / "move" / "strip.fifo").write_bytes(fifo[:-5])
    for ext in (".regs", ".ram"):
        shutil.copyfile(out / "move" / ("0001" + ext), out / "move" / ("strip" + ext))

    results = {}
    for threads in THREAD_COUNTS:
        for name, (mode, *rest) in RUNS.items():
            caps = [str(out / c) for c in rest if "/" in c]
            t = [c for c in rest if "/" not in c]
            extra = {"SOA_THREADS": threads, "SOA_HASH": "1"}
            if t:
                extra["SOA_PAIR_T"] = t[0]
            if mode == "pair":
                extra["SOA_PAIR_LIST"] = str(out / f"{name}.{threads}.pairs")
            r = run(mode, *caps, extra=extra)
            text = r.stdout + r.stderr
            assert r.returncode == 0, (
                f"{name} at SOA_THREADS={threads}: exit {r.returncode}\n{text}"
            )
            res = parse(text)
            if mode == "pair":
                lines = (out / f"{name}.{threads}.pairs").read_text().split()
                res["pairs"] = [
                    (int(a), int(b)) for a, b in zip(lines[::2], lines[1::2], strict=True)
                ]
            results[(name, threads)] = res
    strip = run(
        "pair", str(out / "move" / "strip"), str(out / "move" / "0002"), extra={"SOA_HASH": "1"}
    )
    yield {"out": out, "runs": results, "strip": strip}
    # Twenty-odd 24 MB images: pytest keeps the last three temporary trees.
    for ram in out.rglob("*.ram"):
        ram.unlink()


def one(world, name, threads="1") -> str:
    hashes = world["runs"][(name, threads)]["hashes"]
    assert len(hashes) == 1, f"{name}: {hashes}"
    return hashes[0]


def mid(world, name, threads="1") -> str:
    hashes = world["runs"][(name, threads)]["hashes"]
    assert len(hashes) == 3, f"{name}: a pair presents three frames, got {hashes}"
    return hashes[2]


def box(world, name, colour, threads="1") -> tuple:
    return world["runs"][(name, threads)]["boxes"][colour]


@needs_msvc
def test_every_thread_count_draws_the_same_frames(world):
    """The pair is recorded and lerped on the producer before a command is
    published, so which worker rasterizes it must not matter."""
    for name in RUNS:
        a, b = world["runs"][(name, "1")], world["runs"][(name, "3")]
        assert a["hashes"] == b["hashes"], f"{name} differs between 1 and 3 threads"
        assert a["boxes"] == b["boxes"], name


@needs_msvc
def test_a_triangle_moved_2d_lands_at_d(world):
    """The plan's test: the midpoint of a move by 2d is the frame moved by d,
    pixel for pixel -- and neither frame it came from."""
    for threads in THREAD_COUNTS:
        m = mid(world, "move.pair", threads)
        assert m == one(world, "move.C", threads)
        assert m != one(world, "move.F", threads) and m != one(world, "move.F1", threads)
    x0, y0, x1, y1, n = box(world, "move.F", "red")
    assert n > 1000, "the triangle is not on the screen, so this proves nothing"
    assert box(world, "move.pair", "red") == (x0 + 16, y0 + 8, x1 + 16, y1 + 8, n)


@needs_msvc
def test_perspective_at_constant_depth(world):
    """Under a perspective projection the lerp is of clip-space x, y, z and w;
    with w the same in both frames that is the screen-space midpoint."""
    for threads in THREAD_COUNTS:
        hashes = world["runs"][("persp.pair", threads)]["hashes"]
        assert hashes[2] == one(world, "persp.C", threads)
        assert hashes[2] not in hashes[:2]
    assert world["runs"][("persp.pair", "1")]["report"]["matched"] == 1


@needs_msvc
def test_depth_motion_gives_the_view_space_midpoint(world):
    """A vertex at view x 96 moves from view z -64 (screen x 416) to -192
    (352). Lerping clip space puts it at view z -128, screen 368; lerping the
    screen would put it at 384. This pins which of the two the design gives:
    the one a real frame at the in-between time would draw."""

    def right(name):
        return box(world, name, "red")[2] + 1  # the first column past the triangle

    assert abs(right("depth.F") - 416) <= 1
    assert abs(right("depth.F1") - 352) <= 1
    assert abs(right("depth.pair") - 368) <= 1
    assert abs(right("depth.pair") - 384) >= 12


@needs_msvc
def test_t_zero_and_one_give_the_two_frames(world):
    """SOA_PAIR_T=0 is F and =1 is F+1: true only of (1-t)*a + t*b, and not
    of forms like a + t*(b-a) that round differently at the ends."""
    for threads in THREAD_COUNTS:
        assert mid(world, "move.t0", threads) == one(world, "move.F", threads)
        assert mid(world, "move.t1", threads) == one(world, "move.F1", threads)


@needs_msvc
def test_a_frame_paired_with_itself_is_itself(world):
    for threads in THREAD_COUNTS:
        assert mid(world, "move.self", threads) == one(world, "move.F", threads)
        assert world["runs"][("move.self", threads)]["report"]["matched"] == 1


@needs_msvc
def test_the_real_passes_are_plain_replays(world):
    """Passes 1 and 2 of a pair are F and F+1 as a single replay draws them:
    recording changes nothing, and neither does the pass before."""
    for threads in THREAD_COUNTS:
        hashes = world["runs"][("move.pair", threads)]["hashes"]
        assert hashes[:2] == [one(world, "move.F", threads), one(world, "move.F1", threads)]
        hashes = world["runs"][("depth.pair", threads)]["hashes"]
        assert hashes[:2] == [one(world, "depth.F", threads), one(world, "depth.F1", threads)]


@needs_msvc
def test_an_unmatched_draw_comes_from_f_plus_1(world):
    """A quad that samples another texture in F+1 has another key, so it has
    no match: it is drawn where F+1 draws it while the triangle goes to d."""
    for threads in THREAD_COUNTS:
        assert mid(world, "tex.pair", threads) == one(world, "tex.C", threads)
        assert mid(world, "tex.mut", threads) == one(world, "tex.D", threads)
    rep = world["runs"][("tex.pair", "1")]["report"]
    assert (rep["matched"], rep["draws"]) == (2, 2)
    rep = world["runs"][("tex.mut", "1")]["report"]
    assert (rep["matched"], rep["draws"], rep["from_f1"]) == (1, 2, 1)
    assert box(world, "tex.mut", "blue") == box(world, "tex.D", "blue")
    assert box(world, "tex.mut", "blue") != box(world, "tex.C", "blue")


@needs_msvc
def test_copies_to_texture_are_skipped_and_their_clear_kept(world):
    """The in-between pass must not write a texture F+1's own frame reads, so
    its copies to texture are skipped -- but the clear a copy carries is kept,
    since the draws after it expect the EFB it leaves."""
    for threads in THREAD_COUNTS:
        run = world["runs"][("copies.pair", threads)]
        # The copy at X holds F+1's triangle, from pass 2: pass 3 did not
        # overwrite it with its own, which would be C's.
        assert run["copy"] == world["runs"][("copies.F1", threads)]["copy"]
        assert run["copy"] != world["runs"][("copies.C", threads)]["copy"]
        # The kept clear took the red triangle and the green quad; the blue
        # quad after it went to d like any other matched draw.
        assert run["boxes"]["green"][4] == 0 and run["boxes"]["red"][4] == 0
        assert run["hashes"][2] == one(world, "copies.C", threads)
        assert (run["report"]["skipped"], run["report"]["kept"]) == (2, 1)
    assert box(world, "copies.C", "blue")[4] > 1000


@needs_msvc
def test_equal_key_draws_pair_in_stream_order(world):
    """Within a key, the oldest unmatched draw of F: two triangles of one key
    each land at d. Swapped in F+1, they pair first with first, and both land
    where the crossed lerp puts them -- the known limit fifopair.match has too."""
    for threads in THREAD_COUNTS:
        assert mid(world, "twins.pair", threads) == one(world, "twins.C", threads)
        assert mid(world, "twins.swap", threads) == one(world, "twins.C2", threads)
    assert world["runs"][("twins.swap", "1")]["pairs"] == [(0, 0), (1, 1)]


@needs_msvc
def test_the_c_pairs_are_fifopair_pairs(world):
    """The renderer's match is fifopair's, which H4 measured the game with, so
    the list it writes must be fifopair.match's on the same captures -- and a
    changed texture address must cost exactly that one pair in both."""
    out = world["out"]
    for name, (mode, *rest) in RUNS.items():
        if mode != "pair":
            continue
        fa, fb = (fifopair.load_frame(out / c) for c in rest[:2])
        want = fifopair.match(fa.draws, fb.draws, lambda d: d.key())
        for threads in THREAD_COUNTS:
            assert world["runs"][(name, threads)]["pairs"] == want, f"{name} at {threads}"
    lost = set(world["runs"][("tex.pair", "1")]["pairs"]) - set(
        world["runs"][("tex.mut", "1")]["pairs"]
    )
    assert lost == {(1, 1)}, lost


@needs_msvc
def test_a_capture_without_one_screen_copy_is_refused(world):
    """A pair is two captures of one frame each; F without its screen copy
    never becomes the frame before, so pass 3 would lerp against nothing."""
    strip = world["strip"]
    assert strip.returncode == 1, strip.stdout + strip.stderr
    assert "holds 0 screen copies, not one" in strip.stderr


def _body(name: str) -> str:
    start = GXR.index(name)
    return re.sub(r"/\*.*?\*/", "", GXR[start : GXR.index("\n}\n", start)], flags=re.S)


def test_the_drain_never_touches_the_pair_state():
    """The frames' records live outside the arena, the queue and the graveyard
    because a drain recycles those mid-frame (ARCHITECTURE section 12). A
    drain that reset or read them would lose a frame's records to any flush --
    and the game flushes from inside draws. gxr_flush is one call to drain(),
    so drain's body is the one read."""
    assert "drain(W_EXTERNAL);" in _body("void gxr_flush(void)\n{")
    body = _body("static void drain(int why)\n{")
    assert not re.search(r"\bg_pair|\bpair_", body), "the drain reaches the pair state"


def test_draws_are_claimed_in_one_place():
    """The draw index is fifopair's numbering only if every keyed draw is
    claimed once, in stream order, from the one path every draw takes."""
    calls = re.findall(r"\bpair_claim\(", re.sub(r"/\*.*?\*/", "", GXR, flags=re.S))
    assert len(calls) == 2, "pair_claim is defined once and called once"
    assert "pair_claim(" in _body(
        "static void gxr_draw_inner(CpuState* s, unsigned op, unsigned count, const uint8_t* verts, unsigned vsize)\n{"
    )


def test_positions_are_lerped_before_the_command_is_published():
    """Owned by H16, which moves the transform to the workers and will rewrite
    this: today the producer lerps before publish(), so no worker can see a
    command with F+1's positions in it."""
    body = _body(
        "static void gxr_draw_inner(CpuState* s, unsigned op, unsigned count, const uint8_t* verts, unsigned vsize)\n{"
    )
    assert body.index("pair_positions(") < body.index("publish();")
