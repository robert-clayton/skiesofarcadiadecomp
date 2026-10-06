"""Tests for the bound on the guest memory image (PLAN A3).

``mem_ptr`` masks an effective address with ``MEM_MASK`` and does nothing else
with it, so the size of the image is the only thing between a guest address in
the top eighth of the window -- 0x81800000 up, where the console has no RAM --
and the host heap behind it. Two things are checked here.

The first is arithmetic over the constants in ``runtime/cpu.h``: that the image
covers every offset the mask can produce, plus the widest access that can start
at the last of them. That is text, and runs on both legs of the matrix.

The second builds the real boot path out of ``runtime/main.c`` plus stubs
synthesised here and runs it, to see that an address up there is reported once
and that the run carries on afterwards. It needs a compiler, so it skips where
there is none, and it needs no disc: ``SOA_MEMPOKE`` is handled before the
disc is opened, and with none the boot stops (exit 1) naming tools/extract.py
(disc-layer I1), which every run here asserts. ``SOA_CC`` picks the toolchain profile: CI's Linux leg
runs this module with ``SOA_CC=gcc`` (portability L7), where the guard is
``runtime/plat.c``'s SIGSEGV handler rather than a vectored one.
"""

import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from soa import embed, toolchain  # noqa: E402

# Everything runtime/main.c calls out to. Stubbing them is what lets the boot
# path link on its own; none of them is reached before SOA_MEMPOKE is.
STUBS = """
#include "gxr.h"
void fn_80003140(CpuState* s) { (void)s; }
void hle_report(void) {}
void hle_clock_start(void) {}
const char* settings_load(void) { return 0; }
const char* settings_recorded(char* out, size_t cap) { (void)cap; out[0] = 0; return out; }
int seed_init(char* e, size_t cap) { if (cap) e[0] = 0; return 0; }
void settings_record_as(const char* k, const char* v) { (void)k; (void)v; }
void settings_check_mods(int (*loaded)(const char* id)) { (void)loaded; }
void si_set_motor_strength(int percent) { (void)percent; }
void si_motor_stop(void) {}
void si_set_chord_handler(void (*fn)(int chord, unsigned frame)) { (void)fn; }
uint32_t si_host_buttons(void) { return 0; }
const char* si_chord_name(int chord, int what) { (void)chord; (void)what; return ""; }
void seed_report(void) {}
void hle_frame_mark(void) {}
void hle_frametime_restart(unsigned frame) { (void)frame; }
void hle_on_report(void (*fn)(void)) { (void)fn; }
void si_set_config_extra(const char* x) { (void)x; }
void si_set_pad_filter(void (*fn)(unsigned, void*)) { (void)fn; }
void dispatch(CpuState* s, uint32_t a) { (void)s; (void)a; }
int dispatch_known(uint32_t a) { (void)a; return 0; }
void gxr_set_projection_filter(void (*fn)(float p[6], int o)) { (void)fn; }
void gxr_set_texture_provider(int (*fn)(uint64_t, uint32_t, uint32_t, uint32_t, const uint8_t*, const uint8_t**, uint32_t*, uint32_t*)) { (void)fn; }
/* The renderer's phase words and its clock. main.c reads them for the profile
 * it prints at the end of a run; this boot never gets that far, but it still
 * has to link. */
uint64_t g_gxr_ticks[T_COUNT];
uint64_t g_gxr_phase_last;
int g_gxr_phase;
int g_gxr_tsc = 0;
int g_gx_parsing;
uint64_t gxr_qpc(void) { return 0; }
void gxr_timing_init(void) {}
void gxr_timing_finish(void) {}
double gxr_seconds(uint64_t t) { (void)t; return 0.0; }
double gxr_producer_span(void) { return 0.0; }
void hle_dump(CpuState* s, uint32_t pc) { (void)s; (void)pc; }
void threads_init(CpuState* s) { (void)s; }
void dvd_init(const char* p) { (void)p; }
int selftest(CpuState* s) { (void)s; return 0; }
int gx_replay(CpuState* s, const char* b) { (void)s; (void)b; return 0; }
int gx_replay_pair(CpuState* s, const char* a, const char* b) { (void)s; (void)a; (void)b; return 0; }
void gxr_hook_hazard(uint32_t a, uint32_t b) { (void)a; (void)b; }
int gxr_enabled(void) { return 0; } /* main.c's gpu_start asks; no renderer here */
void gxr_enable(int on) { (void)on; }
void gxr_set_output(const char* p) { (void)p; }
void watch_init(void) {}
void gxr_export_install(void) {}
void window_start(void) {}
int window_toggle_fullscreen(void) { return 0; }
const char* settings_root(void) { return "."; }
int settings_console_to_log(char* path, size_t cap) { (void)path; (void)cap; return 0; }
void si_set_path_root(const char* root) { (void)root; }
void aram_set_data_dir(const char* dir) { (void)dir; }
void aram_census_prepare(void) {}
const uint8_t* gxr_screen(int* w, int* h) { *w = 0; *h = 0; return NULL; }
int png_write_rgba(const char* path, const uint8_t* rgba, int w, int h, int stride) { (void)path; (void)rgba; (void)w; (void)h; (void)stride; return 0; }
int clock_pause_requested(void) { return 0; }
void si_set_pad2_source(int (*fn)(uint16_t*, uint8_t*, uint8_t*, uint8_t*)) { (void)fn; }
int si_read_pad(unsigned port, void* out) { (void)port; (void)out; return 0; }
int window_pad2(uint16_t* b, uint8_t* s, uint8_t* c, uint8_t* t) { (void)b; (void)s; (void)c; (void)t; return 0; }
void gx_set_frame_limit(unsigned f) { (void)f; }
void gx_set_frame_hook(void (*fn)(CpuState*, unsigned)) { (void)fn; }
unsigned gx_frame_count(void) { return 0; }
void gxr_draw_every_frame(void) {}
int irq_in_handler(void) { return 0; }
void guest_backtrace(CpuState* s, uint32_t sp) { (void)s; (void)sp; }
uint32_t g_watch_addr, g_watch_len;
void watch_hit(CpuState* s, uint32_t e, unsigned n, uint64_t v)
{ (void)s; (void)e; (void)n; (void)v; }
void gx_pipe_write(CpuState* s, unsigned n, uint64_t v) { (void)s; (void)n; (void)v; }
uint8_t mmio_read8(CpuState* s, uint32_t e) { (void)s; (void)e; return 0; }
uint16_t mmio_read16(CpuState* s, uint32_t e) { (void)s; (void)e; return 0; }
uint32_t mmio_read32(CpuState* s, uint32_t e) { (void)s; (void)e; return 0; }
uint64_t mmio_read64(CpuState* s, uint32_t e) { (void)s; (void)e; return 0; }
void mmio_write8(CpuState* s, uint32_t e, uint8_t v) { (void)s; (void)e; (void)v; }
void mmio_write16(CpuState* s, uint32_t e, uint16_t v) { (void)s; (void)e; (void)v; }
void mmio_write32(CpuState* s, uint32_t e, uint32_t v) { (void)s; (void)e; (void)v; }
void mmio_write64(CpuState* s, uint32_t e, uint64_t v) { (void)s; (void)e; (void)v; }
"""


def defines(path, names, known=None):
    """The named object-like macros, evaluated, on top of any already known.
    They are small integer expressions over each other, which Python spells the
    same way once the C integer suffixes and the one cast are gone."""
    text = (ROOT / path).read_text()
    out = dict(known or {})
    for name in names:
        match = re.search(rf"^#define\s+{name}\s+([^\n]*)", text, re.M)
        assert match, f"{path} no longer defines {name}"
        expr = re.sub(r"\((?:SIZE_T|size_t)\)", "", match.group(1).split("/*")[0]).strip()
        expr = re.sub(r"(?<=[0-9A-Fa-f])[uU]\b", "", expr)
        out[name] = eval(expr, {"__builtins__": {}}, dict(out))  # noqa: S307
    return out


def test_the_image_covers_every_offset_the_mask_can_produce():
    """The bug this file exists for: the mask spanned 32 MB and the image was
    24, so 8 MB of guest addresses pointed into the host heap. The mask cannot
    be narrowed to the RAM -- 24 MB is not a power of two -- so the image has
    to be as wide as the mask instead."""
    d = defines("runtime/cpu.h", ("MEM1_SIZE", "MEM_MASK", "MEM_IMAGE_SIZE"))
    assert d["MEM_IMAGE_SIZE"] >= d["MEM_MASK"] + 1, "the mask reaches past the end of the image"
    # ... and an access that starts at the last offset the mask can produce
    # finishes inside the image too: mem_r64 and mem_w64 take 8 bytes from any
    # address, and mem_zero32 takes a 32-byte line from an aligned one.
    assert d["MEM_IMAGE_SIZE"] >= d["MEM_MASK"] + 1 + 8
    assert d["MEM_IMAGE_SIZE"] >= (d["MEM_MASK"] + 1) // 32 * 32 + 32
    # The premise of the tripwire: there is a gap, and it is the top eighth.
    assert d["MEM1_SIZE"] < d["MEM_MASK"] + 1


def test_the_boot_path_allocates_the_image_and_not_the_ram():
    """MEM1_SIZE is the size of the console's memory and MEM_IMAGE_SIZE the
    size of the buffer; allocating by the first is exactly the bug."""
    main = (ROOT / "runtime" / "main.c").read_text()
    assert re.search(r"^\s*s\.mem = mem_alloc\(&s\);", main, re.M), (
        "s.mem no longer comes from mem_alloc"
    )
    alloc = main.split("static uint8_t* mem_alloc", 1)[1].split("\n}", 1)[0]
    assert "calloc(1, MEM_IMAGE_SIZE)" in alloc, "the fallback allocation is not the whole image"
    assert "calloc(1, MEM1_SIZE)" not in main, (
        "something allocates the image at the size of the RAM"
    )
    # The reservation the guarded path makes has to hold the image as well:
    # what it does not reserve, the tripwire cannot catch.
    d = defines("runtime/cpu.h", ("MEM1_SIZE", "MEM_MASK", "MEM_IMAGE_SIZE"))
    if "#define MEM_RESERVE_BYTES" in main:
        d = defines("runtime/main.c", ("MEM_RESERVE_BYTES",), d)
        assert d["MEM_RESERVE_BYTES"] >= d["MEM_IMAGE_SIZE"]


# Nine other modules skip on this, for builds of their own that only MSVC
# makes; test_peek, test_poke and test_uncap also use build() below, which with
# SOA_CC unset is MSVC.
needs_msvc = pytest.mark.skipif(
    toolchain.cl_path() is None, reason="no MSVC: the boot path cannot be built here"
)
PROFILE = toolchain.profile(os.environ.get("SOA_CC"))
needs_cc = pytest.mark.skipif(
    toolchain.compiler_path(PROFILE) is None,
    reason=f"no {PROFILE.name}: the boot path cannot be built here",
)


@needs_cc
def test_an_address_past_the_ram_is_reported_once_and_survived(tmp_path):
    """Four stores past the end of the RAM, one of them through the uncached
    alias and one straddling the boundary, produce one message between them --
    a guest loop must not be able to turn the tripwire into a stream -- and
    every one of them reads back what it wrote, so the run went on."""
    exe = build(tmp_path)
    out = run(exe, tmp_path, "0x81800000,0x81C00000,0xC1900000,0x817FFFFE")
    assert out.count("past the console's 24 MB") == 1, out
    assert re.search(r"\[mem\] a store from block [0-9A-F]{8} reached 81800000, past ", out), out
    assert out.count("reads back DEADBEEF") == 4, out


@needs_cc
def test_an_address_in_the_ram_says_nothing(tmp_path):
    """The tripwire is only worth having if it is quiet in a working run."""
    exe = build(tmp_path)
    assert "past the console's 24 MB" not in run(exe, tmp_path, "0x80100000")
    assert "[mem]" not in run(exe, tmp_path, None)


def build(tmp_path):
    """Compile, then link: two steps, because cl's compile-and-link with
    /Fo<dir> has no gcc spelling."""
    (tmp_path / "stubs.c").write_text(STUBS)
    # a --no-embed build's disc_sys.c (disc-layer I3): disc.c reads the image,
    # and there is none, so the boot stops as these tests need it to
    (tmp_path / "disc_sys.c").write_text(embed.disc_sys_c(None), encoding="utf-8")
    runtime = ROOT / "runtime"
    sources = [
        *(runtime / f for f in ("main.c", "mod.c", "tick.c", "picture.c", "gxv.c")),
        *(runtime / f for f in ("disc.c", "sha1.c")),  # disc-layer I1
        *toolchain.runtime_support_sources(),
        tmp_path / "stubs.c",
        tmp_path / "disc_sys.c",
    ]
    proc = toolchain.cc(
        [
            *PROFILE.cflags,
            "/c",
            "/I",
            str(runtime),
            *map(str, sources),
            "/Fo" + str(tmp_path) + os.sep,
        ],
        tmp_path,
        PROFILE,
    )
    assert proc.returncode == 0, (proc.stdout or "") + (proc.stderr or "")
    exe = tmp_path / ("boot" + PROFILE.exeext)
    objs = [str(tmp_path / (s.stem + PROFILE.objext)) for s in sources]
    proc = toolchain.cc(
        [*PROFILE.cflags, *objs, "/Fe" + str(exe), *PROFILE.linker], tmp_path, PROFILE
    )
    assert proc.returncode == 0, (proc.stdout or "") + (proc.stderr or "")
    return exe


def run(exe, tmp_path, poke):
    """One boot, given a directory with no disc in it: main() pokes, then finds
    no disc image and stops, naming the fix, which is all this needs it to do."""
    env = dict(os.environ)
    env.pop("SOA_MEMPOKE", None)
    if poke:
        env["SOA_MEMPOKE"] = poke
    proc = subprocess.run(
        [str(exe), str(tmp_path / "no-disc-here")],
        capture_output=True,
        text=True,
        env=env,
        timeout=120,
        check=False,
    )
    out = proc.stdout + proc.stderr
    # disc.c's stop: no image, so exit 1 and the command that makes one
    assert proc.returncode == 1 and "python tools/extract.py" in out, out
    return out
