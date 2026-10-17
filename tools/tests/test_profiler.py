"""The sampler in runtime/main.c, built and run, with no game and no disc.

PLAN A4 wants a profile that a healthy run produces, that names what it found
and that says the same thing twice. Two of those three are properties of the C
rather than of the report's wording, so they are checked by building the boot
path out of runtime/main.c plus stubs and giving it a guest that spends a
known amount of time in known places: a block inside SelectThread, the entry
of strlen, the command-stream parse marker, and one renderer phase. The report
then has to find them, in roughly those proportions, and find the same ten in
a second run.

That is also the check on the two things nothing else can see: that a block
address folds onto the function containing it through config/functions.tsv
(SelectThread's row is 0x80237A5C + 552, and the address sampled here is
0x80237BA8, which is 332 bytes inside it), and that a sample taken while the
guest thread is in the runtime is charged to the runtime rather than to the
guest block that last ran.

Needs a compiler; skips where there is none. The fake disc below is three
files of zeroes -- no game data.
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import profile as prof  # noqa: E402

from soa import toolchain  # noqa: E402

# The guest, and everything else runtime/main.c calls. The entry point spends
# a known time in each of four places; the proportions below are what the
# report has to find. Sleep is the point: this is a test of where the sampler
# says the time went, not of how fast anything is.
STUBS = r"""
#define _CRT_SECURE_NO_WARNINGS
#include "gxr.h"
#include <windows.h>
#include <stdio.h>

void profile_report(void);

/* The renderer's phase words, as gxr.c would define them: the guest below
 * writes them directly, which is what the real producer does through TIMED. */
uint64_t g_gxr_ticks[T_COUNT];
uint64_t g_gxr_phase_last;
int g_gxr_phase;
int g_gxr_tsc = 1;
int g_gx_parsing;
uint64_t gxr_qpc(void) { return 0; }
void gxr_timing_init(void) {}
void gxr_timing_finish(void) {}
/* Enough for the sampler-against-timers line to be printed and read. */
double gxr_seconds(uint64_t t) { return (double)t / 1000.0; }
double gxr_producer_span(void) { return 3.0; }

/* 0x80237BA8 is 332 bytes inside SelectThread and must print as SelectThread;
 * 0x8025F1D8 is the entry of strlen, which is what runtime/decomp_swap.c's
 * adapter now stores. */
#define DWELL_MS 700
void fn_80003140(CpuState* s)
{
    s->pc = 0x80237BA8u;
    Sleep(DWELL_MS * 2);
    s->pc = 0x8025F1D8u;
    Sleep(DWELL_MS);
    g_gx_parsing = 1;
    Sleep(DWELL_MS);
    g_gx_parsing = 0;
    g_gxr_phase = T_DECODE;
    g_gxr_ticks[T_DECODE] = (uint64_t)(DWELL_MS);
    Sleep(DWELL_MS);
    g_gxr_phase = T_HOST;
}

void hle_report(void) { profile_report(); }
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
void hle_dump(CpuState* s, uint32_t pc) { (void)s; (void)pc; }
void threads_init(CpuState* s) { (void)s; }
void dvd_init(const char* p) { (void)p; }
/* disc.c (disc-layer I1): an image that opens, and three buffers of zeroes --
 * enough shape for the boot path to reach the guest, and nothing of the game. */
static const uint8_t zero_dol[0x100], zero_boot[0x440], zero_fst[0x40];
int disc_open(const char* w, char* why, size_t cap) { (void)w; (void)why; (void)cap; return 0; }
int disc_builtin(char* why, size_t cap) { (void)why; (void)cap; return 0; } /* I3: built without */
void disc_set_stop_hook(void (*fn)(void)) { (void)fn; } /* I5 */
int disc_check_store(const char* w) { (void)w; return 0; }
int disc_system(const uint8_t** dol, size_t* dol_n, const uint8_t** boot, const uint8_t** fst,
                size_t* fst_n, char* why, size_t cap)
{
    (void)why; (void)cap;
    *dol = zero_dol; *dol_n = sizeof zero_dol; *boot = zero_boot; *fst = zero_fst;
    *fst_n = sizeof zero_fst;
    return 0;
}
int selftest(CpuState* s) { (void)s; return 0; }
int gx_replay(CpuState* s, const char* b) { (void)s; (void)b; return 0; }
int gx_replay_pair(CpuState* s, const char* a, const char* b) { (void)s; (void)a; (void)b; return 0; }
void gxr_hook_hazard(uint32_t a, uint32_t b) { (void)a; (void)b; }
int gxr_enabled(void) { return 0; } /* main.c's gpu_start asks; no renderer here */
void gxr_enable(int on) { (void)on; }
void gxr_set_output(const char* p) { (void)p; }
void watch_init(void) {}
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
int hle_clock_fixed(void) { return 0; }                  /* SOA_CLOCK=fixed is off: */
void irq_fixed_wait(CpuState* s) { (void)s; }            /* the frame end's wait is never set */
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

needs_msvc = pytest.mark.skipif(
    toolchain.cl_path() is None, reason="no MSVC: the boot path cannot be built here"
)


def build(tmp_path: Path) -> Path:
    (tmp_path / "stubs.c").write_text(STUBS, encoding="utf-8")
    exe = tmp_path / "prof.exe"
    proc = toolchain.cl(
        [
            *toolchain.CFLAGS,
            "/I",
            str(ROOT / "runtime"),
            str(ROOT / "runtime" / "main.c"),
            str(ROOT / "runtime" / "mod.c"),
            str(ROOT / "runtime" / "sha1.c"),  # disc-layer I1
            str(ROOT / "runtime" / "tick.c"),
            str(ROOT / "runtime" / "picture.c"),
            str(ROOT / "runtime" / "gxv.c"),  # the stub: no SOA_GXV
            *map(str, toolchain.runtime_support_sources()),
            str(tmp_path / "stubs.c"),
            "/Fo" + str(tmp_path) + os.sep,
            "/Fe" + str(exe),
        ],
        cwd=tmp_path,
    )
    assert proc.returncode == 0, (proc.stdout or "") + (proc.stderr or "")
    # The disc is the stubs' (disc_open and disc_system above); the
    # directory is only what the command line names.
    (tmp_path / "disc").mkdir(exist_ok=True)
    return exe


def run(exe: Path) -> str:
    env = dict(os.environ)
    env["SOA_WATCHDOG"] = "0"  # the guest below is asleep on purpose
    env.pop("SOA_RENDER", None)
    proc = subprocess.run(
        [str(exe), str(exe.parent / "disc")],
        env=env,
        capture_output=True,
        text=True,
        cwd=str(ROOT),  # so that config/functions.tsv is where the namer looks
        timeout=180,
    )
    return proc.stdout + proc.stderr


@pytest.fixture(scope="module")
def two_runs(tmp_path_factory):
    if toolchain.cl_path() is None:
        pytest.skip("no MSVC")
    tmp = tmp_path_factory.mktemp("profiler")
    exe = build(tmp)
    return run(exe), run(exe)


@needs_msvc
def test_a_run_that_did_not_stall_still_produces_a_profile(two_runs):
    """The defect A4 names: the old sampler zeroed itself on every presented
    frame and printed only from the watchdog, so this run -- which ends by
    walking off the end of the guest -- would have produced nothing."""
    out = two_runs[0]
    assert "[profile]" in out, out
    p = prof.parse(out)
    assert p.samples and p.samples > 200, out
    assert p.rate_hz and p.rate_hz > 100, f"sampled at {p.rate_hz} Hz, which is Sleep(1) territory"


@needs_msvc
def test_the_table_sums_to_the_whole_run(two_runs):
    got = {c.name: c for c in prof.check(prof.parse(two_runs[0]))}
    assert got["the table sums to 100%"].status == prof.PASS, two_runs[0]


@needs_msvc
def test_a_block_is_named_by_the_function_containing_it(two_runs):
    """0x80237BA8 is not a row in config/functions.tsv -- it is 332 bytes into
    SelectThread's -- so a lookup by equality would print it as an address and
    a lookup by containment prints the name. That is the whole difference
    between the old table and a named one."""
    labels = prof.parse(two_runs[0]).top
    assert "SelectThread" in labels, two_runs[0]
    assert "strlen" in labels, two_runs[0]


@needs_msvc
def test_time_in_the_runtime_is_charged_to_the_runtime(two_runs):
    """While the guest thread is inside the parse or inside a renderer phase,
    its last block address is whatever ran before it -- which is how the parse
    and the renderer have always been invisible. The marker is what stops
    those samples being charged to an innocent guest function."""
    labels = prof.parse(two_runs[0]).top
    assert "[gx] command-stream parse" in labels, two_runs[0]
    assert "[gxr] texture decode" in labels, two_runs[0]


@needs_msvc
def test_the_shares_are_the_time_actually_spent(two_runs):
    """The guest above spends twice as long in SelectThread as in any of the
    other three, which each get one dwell. Loose bounds: the run is a few
    seconds and there is a boot before it, so this asserts the ordering and
    the rough size, not the numbers."""
    rows = {r.label: r.share for r in prof.parse(two_runs[0]).rows}
    assert rows["SelectThread"] > rows["strlen"], rows
    assert rows["SelectThread"] > 30.0, rows
    for label in ("strlen", "[gx] command-stream parse", "[gxr] texture decode"):
        assert 10.0 < rows[label] < 30.0, (label, rows)


@needs_msvc
def test_the_same_top_ten_twice(two_runs):
    """The fourth thing A4 asks for, and the one the old profile could not
    do -- an unknown sample rate over an unknown window cannot repeat."""
    a, b = (prof.parse(t, n) for t, n in zip(two_runs, ("first", "second"), strict=True))
    c = prof.compare(a, b)
    assert c.status == prof.PASS, c.detail + "\n" + two_runs[0] + two_runs[1]


@needs_msvc
def test_the_sampler_says_which_symbol_file_it_used(two_runs):
    """A table of bare hex has two causes -- an inventory that has not named
    those functions yet, and a run started somewhere that cannot see the
    inventory at all -- and they need different fixes."""
    assert "config/functions.tsv" in two_runs[0]


@needs_msvc
def test_a_run_with_no_inventory_degrades_to_addresses(tmp_path):
    """The runtime has to keep running from a directory with no config/."""
    exe = build(tmp_path)
    env = dict(os.environ, SOA_WATCHDOG="0", SOA_SYMBOLS=str(tmp_path / "nothing.tsv"))
    proc = subprocess.run(
        [str(exe), str(exe.parent / "disc")],
        env=env,
        capture_output=True,
        text=True,
        cwd=str(tmp_path),
        timeout=180,
    )
    out = proc.stdout + proc.stderr
    assert "no symbol file" in out, out
    assert "80237BA8" in out, out


@needs_msvc
def test_the_profile_can_be_turned_off(tmp_path):
    """It is a thread and about 700 wakeups a second; a run measuring anything
    else has to be able to say no."""
    exe = build(tmp_path)
    env = dict(os.environ, SOA_WATCHDOG="0", SOA_PROFILE="0")
    proc = subprocess.run(
        [str(exe), str(exe.parent / "disc")],
        env=env,
        capture_output=True,
        text=True,
        cwd=str(ROOT),
        timeout=180,
    )
    assert "[profile]" not in proc.stdout + proc.stderr


# --------------------------------------------------------------------------
# the [run] line: three clocks that are three different things
# --------------------------------------------------------------------------

# runtime/hle.c and nothing else. The guest here reads the timebase 300 ms
# after the process starts, as OSInit does, so that the two clocks cannot
# accidentally agree by starting together.
CLOCK_DRIVER = r"""
#define _CRT_SECURE_NO_WARNINGS
#include "cpu.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

void hle_report(void);
void hle_clock_start(void);
uint32_t guest_timebase_lo(CpuState* s);

void irq_report(void) {}
void threads_report(void) {}
unsigned gx_frame_count(void) { return 4210; }
uint64_t irq_retrace_count(void) { return 8848; }
uint64_t gx_pipe_bytes(void) { return 0; }
void guest_backtrace(CpuState* s, uint32_t sp) { (void)s; (void)sp; }
int device_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out)
{ (void)s; (void)ea; (void)size; (void)out; return 0; }
void device_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v)
{ (void)s; (void)ea; (void)size; (void)v; }
void profile_report(void) {}

int main(void)
{
    CpuState s;
    ULONGLONG until;
    memset(&s, 0, sizeof s);
    hle_clock_start();
    Sleep(300);
    guest_timebase_lo(&s);
    /* A guest reads the timebase constantly; one that read nothing for 1.2 s
     * would be a host stall, which the clock does not count (M19). */
    until = GetTickCount64() + 1200;
    while (GetTickCount64() < until) {
        Sleep(10);
        guest_timebase_lo(&s);
    }
    hle_report();
    return 0;
}
"""


def clock_run(tmp_path: Path, speed: str) -> str:
    (tmp_path / "clocks.c").write_text(CLOCK_DRIVER, encoding="utf-8")
    exe = tmp_path / "clocks.exe"
    proc = toolchain.cl(
        [
            *toolchain.CFLAGS,
            "/I",
            str(ROOT / "runtime"),
            str(tmp_path / "clocks.c"),
            str(ROOT / "runtime" / "hle.c"),
            str(ROOT / "runtime" / "seed.c"),
            str(ROOT / "runtime" / "clock.c"),
            *map(str, toolchain.runtime_support_sources()),  # plat.c (portability L9)
            "/Fo" + str(tmp_path) + os.sep,
            "/Fe" + str(exe),
        ],
        cwd=tmp_path,
    )
    assert proc.returncode == 0, (proc.stdout or "") + (proc.stderr or "")
    done = subprocess.run(
        [str(exe)],
        env=dict(os.environ, SOA_SPEED=speed),
        capture_output=True,
        text=True,
        timeout=120,
    )
    return done.stdout + done.stderr


@needs_msvc
def test_the_run_line_gives_three_numbers_that_are_three_things(tmp_path):
    """Frames, guest seconds and wall seconds, separately -- the report used to
    print none of the three. Frames against retraces is the pair that is
    genuinely independent evidence, so it shares the line."""
    p = prof.parse(clock_run(tmp_path, "1"))
    assert p.frames == 4210 and p.retraces == 8848
    assert p.speed == 1
    # 1.2 s of guest time inside a 1.5 s run: the guest's clock starts when the
    # guest first reads it, which is what makes it a different number from the
    # wall clock even at SOA_SPEED=1.
    assert 1.0 <= p.guest_s <= 1.5, p
    assert p.wall_s > p.guest_s, p


@needs_msvc
def test_guest_seconds_are_wall_seconds_times_the_speed(tmp_path):
    """hle.c multiplies a real clock, so this is true by construction -- which
    is exactly why the report has to say SOA_SPEED on the same line instead of
    offering the two as independent evidence. No saved log in build/ records
    it, which is why no rate in docs/PLAN.md converts to real time."""
    p = prof.parse(clock_run(tmp_path, "10"))
    assert p.speed == 10
    assert 10.0 <= p.guest_s <= 15.0, p
    assert p.wall_s < 3.0, p
    got = {c.name: c for c in prof.check(p)}
    assert got["frames, guest and wall are separate"].status == prof.PASS
