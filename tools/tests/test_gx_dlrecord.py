"""Display lists recorded into guest memory and drawn at their call (gpu-backend.md C5b).

GXBeginDisplayList points the CPU FIFO at the SDK's DisplayListFifo and
GXEndDisplayList points it back; until C5b the port parsed every byte the
game pushed at once, so each list was drawn while it was being recorded and
then called empty (FINDINGS "Recorded display lists (C5a)"). runtime/gx.c now
records while GXSetCPUFifo's current-FIFO global holds DisplayListFifo: the
bytes go into guest memory at the PI write pointer, GXEndDisplayList sizes the
list from it, and the list is parsed at its call. A capture holds each call
with the list's bytes inline (CAP_LIST), because its RAM is the frame's end.

The driver below builds the renderer on its own and plays the SDK's side by
hand -- the global, the PI FIFO writes, GXFlush's 32 NOPs, the write pointer
read back -- then reports which pure primaries the screen holds. Every scene
is drawn live with a capture taken, and the capture replayed: the two must
agree. No disc and no game data; nothing pinned.
"""

import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import fifo  # noqa: E402
import fifopair  # noqa: E402
from soa import toolchain  # noqa: E402

RUNTIME = ROOT / "runtime"
SOURCES = ["gx.c", "gx_frame.c", "gxr.c", "gxr_tev.c", "png.c"]

# The stream helpers and setup() are test_gxr_pair.py's orthographic ones: a
# vertex at (x, y) lands on pixel (x, y) to within about 0.02 px.
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
int gx_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out);
int gx_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v);
void gx_report(void);

#define RED 0xFF0000FFu
#define GREEN 0x00FF00FFu
#define BLUE 0x0000FFFFu

/* What GXSetCPUFifo leaves for runtime/gx.c to read: r13, the small-data
 * global at r13-27520 naming the current CPU FIFO object, and the SDK's
 * DisplayListFifo at 0x80318B18. */
#define R13 0x80500000u
#define CUR_FIFO (R13 - 27520u)
#define DL_FIFO 0x80318B18u
#define MAIN_OBJ 0x80330000u  /* the main FIFO's object: anything but DL_FIFO */
#define LOGO_OBJ 0x80340000u
#define MAIN_FIFO 0x804A74E0u /* where C5a saw the CP's FIFO */
#define LOGO_FIFO 0x804024E0u /* the logo screen's redirect, never called (C5a) */
#define LIST_A 0x80600000u
#define LIST_B 0x80610000u

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
    bp_w(s, 0x4F, 0x00FF80u); bp_w(s, 0x50, 0x008080u); bp_w(s, 0x51, 0xFFFFFFu); /* clear: grey, far */
}

/* 67 bytes: the opcode, the count and four 16-byte vertices. The depth test is
 * LEQUAL, so a later quad at the same depth lands on top. */
static void quad(CpuState* s, float x0, float y0, float x1, float y1, uint32_t rgba)
{
    gp8(s, 0x80); gp16(s, 4);
    vertex(s, x0, y0, 50.0f, rgba); vertex(s, x1, y0, 50.0f, rgba);
    vertex(s, x1, y1, 50.0f, rgba); vertex(s, x0, y1, 50.0f, rgba);
}

/* The screen copy that ends a frame; with `clear`, it also clears the EFB to
 * setup()'s grey and far depth, as a replay's gxr_reset_efb starts it. */
static void present(CpuState* s, int clear)
{
    bp_w(s, 0x49, 0); bp_w(s, 0x4A, 0x077E7F); bp_w(s, 0x4D, 0x28);
    bp_w(s, 0x4B, (0x81100000u & 0x1FFFFFFFu) >> 5);
    bp_w(s, 0x52, clear ? 0x4803 : 0x4003);
    gxr_flush();
}

/* GXSetCPUFifo: the object's address first (0x8024C620), then base, top
 * (the last word) and write pointer, physical. */
static void set_cpu_fifo(CpuState* s, uint32_t obj, uint32_t base, uint32_t size)
{
    uint8_t* g = mem_ptr(s, CUR_FIFO); /* big-endian, as the guest stores it */
    g[0] = (uint8_t)(obj >> 24); g[1] = (uint8_t)(obj >> 16); g[2] = (uint8_t)(obj >> 8); g[3] = (uint8_t)obj;
    gx_write(s, 0xCC00300Cu, 4, base & 0x3FFFFFFFu);
    gx_write(s, 0xCC003010u, 4, (base + size - 4) & 0x3FFFFFFFu);
    gx_write(s, 0xCC003014u, 4, base & 0x3FFFFFFFu);
}

static void begin_list(CpuState* s, uint32_t list, uint32_t size) { set_cpu_fifo(s, DL_FIFO, list, size); }

/* GXEndDisplayList: the wrap bit read, GXFlush's 32 NOPs, the write pointer
 * read back, the main FIFO restored; 0 when the list overflowed. */
static uint32_t end_list(CpuState* s, uint32_t list)
{
    uint64_t w = 0;
    int i, ov;
    gx_read(s, 0xCC003014u, 4, &w);
    ov = (int)((w >> 26) & 1);
    for (i = 0; i < 8; i++) gp32(s, 0);
    gx_read(s, 0xCC003014u, 4, &w);
    set_cpu_fifo(s, MAIN_OBJ, MAIN_FIFO, 0x10000);
    return ov || (w & 0x04000000u) ? 0 : (uint32_t)((w & 0x03FFFFFFu) - (list & 0x03FFFFFFu));
}

static void call_list(CpuState* s, uint32_t list, uint32_t size) { gp8(s, 0x40); gp32(s, list); gp32(s, size); }

#define R1 100.25f, 100.0f, 164.25f, 164.0f
#define R2 300.25f, 200.0f, 364.25f, 264.0f

static int scene(CpuState* s, const char* name)
{
    uint32_t n;
    if (!strcmp(name, "order")) { /* recorded, drawn over directly, then called */
        begin_list(s, LIST_A, 0x1000); quad(s, R1, RED); n = end_list(s, LIST_A);
        quad(s, R1, GREEN);
        call_list(s, LIST_A, n);
    } else if (!strcmp(name, "logo")) { /* a CPU FIFO that is not a list, the CP's elsewhere */
        gx_write(s, 0xCC000020u, 2, MAIN_FIFO & 0xFFFFu);
        gx_write(s, 0xCC000022u, 2, (MAIN_FIFO >> 16) & 0x1FFFu);
        set_cpu_fifo(s, LOGO_OBJ, LOGO_FIFO, 0x10000);
        quad(s, R1, BLUE);
        set_cpu_fifo(s, MAIN_OBJ, MAIN_FIFO, 0x10000);
    } else if (!strcmp(name, "twice")) { /* recorded twice before one call; another never called */
        begin_list(s, LIST_A, 0x1000); quad(s, R1, RED); end_list(s, LIST_A);
        begin_list(s, LIST_A, 0x1000); quad(s, R2, BLUE); n = end_list(s, LIST_A);
        begin_list(s, LIST_B, 0x1000); quad(s, R1, GREEN); end_list(s, LIST_B);
        call_list(s, LIST_A, n);
    } else if (!strcmp(name, "rerecord")) { /* called, then recorded again in the same frame */
        begin_list(s, LIST_A, 0x1000); quad(s, R1, RED); n = end_list(s, LIST_A);
        call_list(s, LIST_A, n);
        begin_list(s, LIST_A, 0x1000); quad(s, R2, BLUE); end_list(s, LIST_A);
    } else if (!strcmp(name, "overflow")) { /* 67 bytes and the NOPs into a 64-byte buffer */
        begin_list(s, LIST_A, 64); quad(s, R1, RED); n = end_list(s, LIST_A);
        call_list(s, LIST_A, n);
        printf("[dltest] overflow size %u\n", n);
    } else {
        return 2;
    }
    return 0;
}

static void report(void)
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
        printf("[dltest] box %s %d %d %d %d %d\n", C[c].name, x0, y0, x1, y1, n);
    }
}

int main(int argc, char** argv)
{
    static CpuState s;
    int rc;
    s.mem = (uint8_t*)calloc(1, MEM_IMAGE_SIZE);
    if (!s.mem || argc < 3) return 2;
    s.gpr[13] = R13;
    if (!gxr_enabled()) return 3;
    if (!strcmp(argv[1], "live")) {
        /* Frame 0 is the setup and the clear, and is never captured; frame 1
         * is the scene. */
        setup(&s);
        present(&s, 1);
        rc = scene(&s, argv[2]);
        if (rc) return rc;
        present(&s, 0);
        gx_report();
    } else if (!strcmp(argv[1], "one")) {
        rc = gx_replay(&s, argv[2]);
        if (rc) return rc;
    } else {
        return 2;
    }
    report();
    fflush(stdout);
    return 0;
}
"""

SCENES = ("order", "logo", "twice", "rerecord", "overflow")

# The quads' pixel boxes: an edge at x .25 takes the pixel whose centre is
# past it, so x 100.25..164.25 covers columns 100..163, and y 100..164 rows
# 100..163.
BOX_R1 = (100, 100, 163, 163, 64 * 64)
BOX_R2 = (300, 200, 363, 263, 64 * 64)
NONE = (640, 480, -1, -1, 0)

LISTS = re.compile(
    r"\[gx\] display lists: (\d+) calls, (\d+) nonempty, (\d+) bytes; "
    r"(\d+) recorded, (\d+) bytes recorded(, some overflowed their buffers)?"
)

needs_msvc = pytest.mark.skipif(
    toolchain.cl_path() is None, reason="no MSVC: the renderer cannot be built here"
)


def clean_env() -> dict:
    return {k: v for k, v in os.environ.items() if not k.startswith("SOA_")}


def parse(text: str) -> dict:
    boxes = {
        m[0]: tuple(int(v) for v in m[1:])
        for m in re.findall(r"\[dltest\] box (\w+) (-?\d+) (-?\d+) (-?\d+) (-?\d+) (\d+)", text)
    }
    lists = LISTS.search(text)
    return {
        "boxes": boxes,
        "lists": tuple(int(v) for v in lists.groups()[:5]) if lists else None,
        "overflowed": bool(lists and lists.group(6)),
        "text": text,
    }


@pytest.fixture(scope="module")
def runs(tmp_path_factory):
    if toolchain.cl_path() is None:
        pytest.skip("no MSVC")
    out = tmp_path_factory.mktemp("dlrecord")
    (out / "driver.c").write_text(DRIVER, encoding="utf-8")
    exe = out / "dlrecord.exe"
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

    def run(*args, extra):
        r = subprocess.run(
            [str(exe), *args],
            capture_output=True,
            text=True,
            env={**env, "SOA_RENDER": "1", **extra},
            cwd=out,
            timeout=300,
            check=False,
        )
        text = r.stdout + r.stderr
        assert r.returncode == 0, f"{args}: exit {r.returncode}\n{text}"
        return parse(text)

    results = {}
    for scene in SCENES:
        d = out / scene
        d.mkdir()
        live = run("live", scene, extra={"SOA_FIFO_DUMP": "1", "SOA_FIFO_DIR": str(d)})
        base = d / "0001"
        results[scene] = {
            "live": live,
            "replay": run("one", str(base), extra={}),
            "fifo": (base.with_suffix(".fifo")).read_bytes(),
            "base": base,
        }
    yield results
    for ram in out.rglob("*.ram"):
        ram.unlink()


def boxes(runs, scene, which="live") -> dict:
    return runs[scene][which]["boxes"]


@needs_msvc
def test_a_list_is_drawn_at_its_call_not_while_it_is_recorded(runs):
    """Recorded red, then green drawn directly over it, then the list called:
    the red lands on top, as on the console. Drawn while recording, the green
    would cover it."""
    b = boxes(runs, "order")
    assert b["red"] == BOX_R1, runs["order"]["live"]["text"]
    assert b["green"] == NONE, runs["order"]["live"]["text"]


@needs_msvc
def test_a_cpu_fifo_that_is_not_a_list_is_drawn_at_once(runs):
    """The logo screen's redirect: the CPU FIFO moved somewhere the command
    processor is not reading, by a FIFO object that is not DisplayListFifo,
    and never called. A rule of 'the bases differ' would draw nothing."""
    b = boxes(runs, "logo")
    assert b["blue"] == BOX_R1, runs["logo"]["live"]["text"]
    calls, nonempty, _, recorded, _ = runs["logo"]["live"]["lists"]
    assert (calls, nonempty, recorded) == (0, 0, 0)


@needs_msvc
def test_only_what_a_list_holds_at_its_call_is_drawn(runs):
    """A list recorded twice before one call draws only the second recording,
    and a list recorded and never called draws nothing."""
    b = boxes(runs, "twice")
    assert b["blue"] == BOX_R2, runs["twice"]["live"]["text"]
    assert b["red"] == NONE and b["green"] == NONE, runs["twice"]["live"]["text"]


@needs_msvc
def test_a_list_recorded_again_after_its_call_keeps_what_was_called(runs):
    b = boxes(runs, "rerecord")
    assert b["red"] == BOX_R1 and b["blue"] == NONE, runs["rerecord"]["live"]["text"]


@needs_msvc
def test_a_list_that_overflows_its_buffer_is_empty(runs):
    """67 bytes and GXFlush's 32 NOPs into 64: the write pointer wraps and sets
    the bit GXEndDisplayList reads as overflow, so the list's size is 0."""
    live = runs["overflow"]["live"]
    assert "[dltest] overflow size 0" in live["text"]
    assert live["overflowed"], live["text"]
    assert boxes(runs, "overflow")["red"] == NONE, live["text"]


@needs_msvc
def test_the_report_counts_calls_and_recordings(runs):
    """One recording of a quad and the flush (67 + 32 bytes), one call of it:
    and every report line is there, so no check above passes vacuously."""
    assert runs["order"]["live"]["lists"] == (1, 1, 99, 1, 99)
    assert runs["twice"]["live"]["lists"] == (1, 1, 99, 3, 297)
    assert runs["rerecord"]["live"]["lists"] == (1, 1, 99, 2, 198)


@needs_msvc
@pytest.mark.parametrize("scene", SCENES)
def test_a_capture_replays_to_the_live_picture(runs, scene):
    assert boxes(runs, scene, "replay") == boxes(runs, scene), runs[scene]["replay"]["text"]


@needs_msvc
def test_the_capture_holds_each_list_inline_and_once(runs):
    """The call becomes a CAP_LIST record with the list's 99 bytes after it, as
    they were at the call; the bytes recorded are not in the stream besides,
    or a replay would draw them twice. The blue quad recorded after the call is
    in neither."""
    stream = runs["rerecord"]["fifo"]
    red = (0xFF0000FF).to_bytes(4, "big")
    blue = (0x0000FFFF).to_bytes(4, "big")
    assert stream.count(red) == 4 and stream.count(blue) == 0
    at = stream.index(red) - 15  # the quad's first vertex starts 12 bytes in, after 3 of header
    head = stream[at - 9 : at]
    assert head[0] == fifo.CAP_LIST, head.hex()
    assert int.from_bytes(head[1:5], "big") == 0x80600000
    assert int.from_bytes(head[5:9], "big") == 99
    assert b"\x40\x80\x60\x00\x00" not in stream


@needs_msvc
def test_fifopair_reads_the_inline_list(runs):
    """tools/fifo.py follows a CAP_LIST record inline, as the replay does, so
    H10's pair keys see the draw come through the list."""
    frame = fifopair.load_frame(runs["rerecord"]["base"])
    assert frame.list_calls == 1 and frame.empty_list_calls == 0
    assert [d.dl for d in frame.draws] == [0x80600000]
