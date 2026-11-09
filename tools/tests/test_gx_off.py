"""SOA_GX=off in runtime/gx.c (soa-ue5's U4.1, the no-FIFO mode): the command processor's model and the renderer off.

gx.c built on its own with gcc, the renderer's entries and the rest of the runtime stood in for by counters. The same
driver runs twice, without the switch and with it: a BP write, a draw and a display copy down the gather pipe, a CP,
PE and PI FIFO register written and read back, the pixel engine's token raised by its register, and the frame's end a
host makes.

Checked: without the switch the stream is parsed, the renderer is called, the copy ends the frame and the registers
hold what was written (the model as before). With it, the pipe's bytes are counted and nothing else -- no renderer
call, no frame ended by the stream, no draw -- each register access is counted, read back 0 and written nowhere, the
pending interrupts are the host's (its callback's two low bits), the wait finder's bytes are the pipe's and the host's,
the host's frame end runs the frame hook and counts, and the report says so. No disc and no game data.
"""

import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / "runtime"

DRIVER = r"""
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>

unsigned g_renderer, g_flushes, g_hooks;
void gxr_bp_written(CpuState* s, uint32_t reg, uint32_t value) { (void)s, (void)reg, (void)value; g_renderer++; }
void gxr_draw(CpuState* s, unsigned op, unsigned n, const uint8_t* v, unsigned vs) { (void)s, (void)op, (void)n, (void)v, (void)vs; g_renderer++; }
void gxr_flush(void) { g_flushes++; }
void gxr_report(void) {}
void gxr_reset_efb(void) { g_renderer++; }
void gxr_set_output(int a, int b) { (void)a, (void)b; g_renderer++; }
void gxr_source_hazard(uint32_t a, uint32_t b) { (void)a, (void)b; }
int gxr_pair_mode(unsigned f, float t) { (void)f, (void)t; return 0; }
unsigned long long gxr_pair_rotations(void) { return 0; }
void gxr_pair_list(void* f) { (void)f; }
void gxr_pair_report(void) {}
void tex_epoch_advance(void) {}
void hle_report(void) {}
uint32_t mmio_read32(CpuState* s, uint32_t ea) { (void)s, (void)ea; return 0; }

void gx_pipe_write(CpuState* s, unsigned size, uint64_t v);
int gx_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out);
int gx_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v);
unsigned gx_pe_irq_pending(void);
void gx_report(void);
unsigned gx_frame_count(void);
unsigned long long gx_draw_count(void);
uint64_t gx_pipe_bytes(void);
uint64_t gx_drawn_bytes(void);
void gx_set_frame_hook(void (*fn)(CpuState*, unsigned));
int gx_is_off(void);
int gx_set_native(unsigned (*pe_pending)(void* user), void* user, const volatile uint64_t* drawn);
int gx_off_frame_end(CpuState* s);

static unsigned host_pe(void* user) { return *(unsigned*)user; }
static void hook(CpuState* s, unsigned frame) { (void)s, (void)frame; g_hooks++; }
static void p8(CpuState* s, unsigned v) { gx_pipe_write(s, 1, v); }
static void p16(CpuState* s, unsigned v) { gx_pipe_write(s, 2, v); }
static void p32(CpuState* s, uint32_t v) { gx_pipe_write(s, 4, v); }
static void bp(CpuState* s, unsigned reg, uint32_t v) { p8(s, 0x61); p32(s, (uint32_t)reg << 24 | (v & 0xFFFFFFu)); }

int main(void)
{
    static CpuState s;
    static unsigned pending = 0x7; /* the host's: a token and a finish, and a bit it must not pass on */
    static volatile uint64_t drawn = 1000;
    uint64_t cp = 0, pe = 0, pi = 0, status = 0;
    int i;
    s.mem = (uint8_t*)calloc(1, MEM_IMAGE_SIZE);
    if (!s.mem) return 2;
    gx_set_frame_hook(hook);
    printf("off %d native %d\n", gx_is_off(), gx_set_native(host_pe, &pending, &drawn));
    bp(&s, 0x00, 0x000010u);                   /* a generator mode: a register the renderer is told of */
    p8(&s, 0x08); p8(&s, 0x50); p32(&s, 0x2200); /* a CP load: the position direct */
    p8(&s, 0x08); p8(&s, 0x70); p32(&s, 0x41377009u);
    p8(&s, 0x80); p16(&s, 1);                  /* a quad's opcode with one vertex: a position and a colour */
    for (i = 0; i < 4; i++) p32(&s, 0);
    bp(&s, 0x52, 0x4003);                      /* a display copy: the frame's end */
    gx_write(&s, 0xCC000020u, 2, 0x1234);      /* the CP's FIFO base, low */
    gx_read(&s, 0xCC000020u, 2, &cp);
    gx_read(&s, 0xCC000000u, 2, &status);
    gx_write(&s, 0xCC00100Au, 2, 0x5);         /* PE interrupt status: the token's enable, and the token cleared */
    gx_write(&s, 0xCC00100Au, 2, 0x5);
    gx_read(&s, 0xCC00100Au, 2, &pe);
    gx_write(&s, 0xCC003014u, 4, 0x00ABCDE0u); /* the PI FIFO's write pointer */
    gx_read(&s, 0xCC003014u, 4, &pi);
    printf("pipe %llu drawn %llu frames %u hooks %u draws %llu renderer %u\n", (unsigned long long)gx_pipe_bytes(),
           (unsigned long long)gx_drawn_bytes(), gx_frame_count(), g_hooks, gx_draw_count(), g_renderer ? 1u : 0u);
    printf("cp %llx status %llx pe %llx pi %llx pending %u\n", (unsigned long long)cp, (unsigned long long)status,
           (unsigned long long)pe, (unsigned long long)pi, gx_pe_irq_pending());
    i = gx_off_frame_end(&s); /* apart: an argument list's order is unspecified */
    printf("host frame end %d frames %u hooks %u\n", i, gx_frame_count(), g_hooks);
    fflush(stdout);
    gx_report();
    return 0;
}
"""

gnu = pytest.mark.skipif(
    shutil.which("gcc") is None or os.name == "nt", reason="wants gcc on an ELF system"
)


@pytest.fixture(scope="module")
def exe(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("gx_off")
    (tmp / "driver.c").write_text(DRIVER, encoding="utf-8")
    out = tmp / "gxoff"
    proc = subprocess.run(
        ["gcc", "-std=c17", "-O1", "-D_GNU_SOURCE", "-fno-strict-aliasing", "-fwrapv", f"-I{RUNTIME}",
         str(RUNTIME / "gx.c"), str(RUNTIME / "gx_frame.c"), str(tmp / "driver.c"), "-lm", "-o", str(out)],
        capture_output=True, text=True, timeout=120, check=False,
    )  # fmt: skip
    assert proc.returncode == 0, proc.stdout + proc.stderr
    return out


def run(exe, **env):
    clean = {k: v for k, v in os.environ.items() if not k.startswith("SOA_")}
    r = subprocess.run(
        [str(exe)], capture_output=True, text=True, timeout=60, env={**clean, **env}, check=False
    )
    assert r.returncode == 0, r.stdout + r.stderr
    return r.stdout.splitlines(), r.stderr


@gnu
def test_without_the_switch_the_model_is_as_before(exe):
    out, err = run(exe)
    assert out[0] == "off 0 native 0"
    assert out[1] == "pipe 41 drawn 41 frames 1 hooks 1 draws 1 renderer 1", err
    assert out[2] == "cp 1234 status c pe 1 pi abcde0 pending 0", err
    assert out[3] == "host frame end 0 frames 1 hooks 1"
    assert "1 EFB copies, 0 unknown bytes" in err
    assert "[gx] off" not in err


@gnu
def test_with_the_switch_nothing_reaches_the_model_and_the_host_answers(exe):
    out, err = run(exe, SOA_GX="off")
    assert out[0] == "off 1 native 1"
    assert out[1] == "pipe 41 drawn 1041 frames 0 hooks 0 draws 0 renderer 0", err
    assert out[2] == "cp 0 status 0 pe 0 pi 0 pending 3", err
    assert out[3] == "host frame end 1 frames 1 hooks 1"
    m = re.search(r"\[gx\] off: (\d+) register reads, (\d+) register writes, (\d+) pipe bytes; (\d+) frames ended "
                  r"by the host, (\d+) bytes the host drew", err)  # fmt: skip
    assert m and tuple(map(int, m.groups())) == (4, 4, 41, 1, 1000), err
    assert "[gx] off (SOA_GX=off)" in err
