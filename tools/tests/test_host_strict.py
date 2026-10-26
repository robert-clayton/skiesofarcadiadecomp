"""Strict mode (runtime/soa_host.h, docs/CLOCK.md "Deterministic runs under a
host"): the real runtime/host.c, with a stand-in guest in place of the game.

The stand-in's thread is the guest's: each frame it reads both pads three
times, a little apart, notes a word of memory, and ends the frame through
host_frame_end, as main.c's frame hook does. The host program drives it as
an editor would: from its own thread, waiting for soa_host_held, sleeping a
random while, setting the next frame's pad and now and then writing memory,
sleeping again, and letting the frame go. Checked: every read in frame F+1
sees the pad given while F was held; a write made while F was held is there
from F+1; a pad named for a frame with soa_host_set_pad_at comes in on that
frame; a pad given and a write tried before the start are counted late and
refused; and two runs with different delays are the same run. Wants gcc or
clang on an ELF system (SOA_CC).
"""

import os
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from soa import toolchain  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / "runtime"
PROFILE = toolchain.profile(os.environ.get("SOA_CC"))
gnu = pytest.mark.skipif(
    os.name == "nt"
    or PROFILE.name not in ("gcc", "clang")
    or toolchain.compiler_path(PROFILE) is None,
    reason="needs gcc or clang (SOA_CC) on an ELF system",
)

FRAMES = 60

# What host.c needs of the rest of the runtime, and a guest of three reads a
# frame. Loose types where host.c's are the renderer's: nothing here calls them.
STUBS = r"""
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void audio_set_muted(int on) { (void)on; }
void ax_tap(void* fn) { (void)fn; }
void clock_exclude_at(uint64_t a, uint64_t b) { (void)a, (void)b; }
uint64_t clock_host_ns(void) { return 0; }
void clock_pause(int on) { (void)on; }
static uint32_t g_regs[0x1100];
const uint32_t* gx_bp_regs(void) { return g_regs; }
const uint32_t* gx_xf_regs(void) { return g_regs; }
static volatile unsigned g_frame;
unsigned gx_frame_count(void) { return g_frame; }
void gxr_hook_hazard(uint32_t a, uint32_t b) { (void)a, (void)b; }
long gxr_presented(void) { return 0; }
const uint8_t* gxr_screen(int* w, int* h) { *w = *h = 0; return NULL; }
void gxr_set_draw_export(const void* e) { (void)e; }
uint64_t hle_guest_ticks(void) { return 0; }
int irq_event_add(uint64_t due, uint64_t period, void* fn, void* user, void* extra) { (void)due, (void)period, (void)fn, (void)user, (void)extra; return 1; }
int irq_event_cancel(int id) { (void)id; return 1; }
int irq_raise(unsigned n) { (void)n; return 1; }
int irq_charge(uint64_t ticks) { (void)ticks; return 0; }
int mod_call_guest(void) { return 0; }
void ninja_set_feed(const void* f) { (void)f; }
void si_set_motor_sink(void* fn) { (void)fn; }
void si_set_motor_window(int open) { (void)open; }
void skip_request(unsigned f, unsigned s) { (void)f, (void)s; }
int soa_load_game(const char* path, char* why, size_t cap) { (void)path, (void)why, (void)cap; return 0; }
void swap_attach(void) {}
int swap_call_exact(void) { return 0; }
int swap_call_original(void) { return 0; }
void swap_count(int on) { (void)on; }
unsigned swap_counts(void* out, unsigned max) { (void)out, (void)max; return 0; }
unsigned swap_functions(unsigned* native) { *native = 0; return 0; }
int swap_original(void) { return 0; }
int swap_set(void) { return 0; }
void tev_pixel(void) {}
void watchdog_fallback(void) {}
/* snapshot.c's, which a run here never takes or resumes */
int snapshot_resuming(void) { return 0; }
int snapshot_precheck(char* why, size_t cap) { (void)why, (void)cap; return 0; }
int snapshot_request(const char* path) { (void)path; return 0; }
int snapshot_status(long* frame, char* why, size_t cap) { (void)frame, (void)why, (void)cap; return 0; }
int snapshot_arm_resume(const char* path, char* why, size_t cap) { (void)path, (void)why, (void)cap; return 1; }
unsigned snapshot_resumed_frame(void) { return 0; }
unsigned snapshot_armed_frame(void) { return 0; }
int snapshot_host_put(const char* name, const void* data, size_t bytes) { (void)name, (void)data, (void)bytes; return 0; }
long snapshot_host_get(const char* name, void* out, size_t max) { (void)name, (void)out, (void)max; return -1; }
void snapshot_set_host_hook(void (*fn)(void* user), void* user) { (void)fn, (void)user; }
int irq_event_rebind(int id, void* fn, void* user, void* extra) { (void)id, (void)fn, (void)user, (void)extra; return 0; }
int snapshot_foreign(void) { return 0; }
/* the delivery points soa_host_back_edge hands a native answer's loop to: counted, with the pc they were given
 * (test_host_back_edge.py reads them; nothing here calls them otherwise) */
int g_clock_quiet;
unsigned g_polls, g_quiet_entries;
uint32_t g_poll_pc;
void irq_poll(CpuState* s) { g_polls++, g_poll_pc = s->pc; }
void irq_quiet_entry(CpuState* s) { g_quiet_entries++, g_poll_pc = s->pc; }

int window_pad(uint16_t* buttons, uint8_t stick[2], uint8_t cstick[2], uint8_t trig[2]);
int window_pad2(uint16_t* buttons, uint8_t stick[2], uint8_t cstick[2], uint8_t trig[2]);
void host_frame_end(CpuState* s, unsigned frame);

/* the guest: three reads of each pad a frame, a word of memory, the frame's end */
int soa_main(int argc, char** argv)
{
    static CpuState s;
    unsigned f, k;
    struct timespec ts = {0, 200000};
    (void)argc, (void)argv;
    s.mem = (uint8_t*)calloc(1, 0x02000000u);
    for (f = 0; f < FRAMES; f++) {
        g_frame = f;
        printf("frame %u:", f);
        for (k = 0; k < 3; k++) {
            uint16_t b = 0, b2 = 0;
            uint8_t st[2], cs[2], tr[2];
            int p1 = window_pad(&b, st, cs, tr), p2 = window_pad2(&b2, st, cs, tr);
            printf(" %d:%04X/%d:%04X", p1, b, p2, b2);
            nanosleep(&ts, NULL);
        }
        printf(" mem %08X\n", mem_r32(&s, 0x80001000u));
        fflush(stdout);
        host_frame_end(&s, f);
    }
    exit(0);
}
"""

DRIVER = r"""
#define _GNU_SOURCE
#include "soa_host.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static unsigned g_seed;
static void wait_a_while(unsigned most_us)
{
    g_seed = g_seed * 1103515245u + 12345u;
    usleep((g_seed >> 8) % (most_us + 1));
}

static int write_word(uint32_t v) /* big-endian, as the game's memory is */
{
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    return soa_host_write(0x80001000u, b, 4);
}

static void report(void)
{
    SoaHostStrictStats st;
    soa_host_strict_stats(&st);
    fprintf(stderr, "stats: %llu held, %llu pads, %llu late, %llu refused\n", (unsigned long long)st.held,
            (unsigned long long)st.pads, (unsigned long long)st.late, (unsigned long long)st.refused);
}

int main(int argc, char** argv)
{
    SoaHostStart start = {SOA_HOST_ABI, "stand-in", NULL, NULL};
    SoaHostPad pad = {0, {128, 128}, {128, 128}, {0, 0}}, two = {0x0100, {128, 128}, {128, 128}, {0, 0}};
    char why[200];
    long last = -1;
    unsigned most = (unsigned)atoi(argv[2]);
    g_seed = (unsigned)atoi(argv[1]);
    if (!soa_host_set_strict(SOA_HOST_STRICT_ON)) return 3;
    pad.buttons = 0x0010;
    soa_host_set_pad(1, &pad);                /* before the start: late, at the first frame's end */
    soa_host_set_pad_at(10, 2, &two);         /* port 2 from frame 10 */
    if (write_word(0xDEADBEEFu)) return 4;    /* not held: refused */
    if (soa_host_start(&start, why, sizeof why)) return 5;
    if (soa_host_set_strict(0)) return 6;     /* not once the run has started */
    atexit(report);
    for (;;) {
        long f = soa_host_held();
        if (f < 0 || f == last) {
            usleep(20);
            continue;
        }
        wait_a_while(most);
        pad.buttons = (uint16_t)(((f + 1) * 37) & 0x1F7F);
        soa_host_set_pad(1, &pad);
        if (f % 5 == 0) {
            if (!write_word(0x1000u + (uint32_t)f)) return 7;
        }
        wait_a_while(most);
        last = f;
        soa_host_go(f);
    }
}
"""


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("host_strict")
    (tmp / "stubs.c").write_text(STUBS.replace("FRAMES", str(FRAMES)), encoding="utf-8")
    (tmp / "driver.c").write_text(DRIVER, encoding="utf-8")
    exe = tmp / "strict"
    proc = toolchain.cc(
        [
            *PROFILE.cflags,
            "/DSOA_SPLIT=1",
            "/DSOA_HOST=1",
            f"/I{RUNTIME}",
            f"/Fe{exe}",
            str(RUNTIME / "host.c"),
            "stubs.c",
            "driver.c",
            *map(str, toolchain.runtime_support_sources()),
            *PROFILE.linker,
        ],
        tmp,
        PROFILE,
    )
    assert proc.returncode == 0, (proc.stdout or "") + (proc.stderr or "")
    return exe


def run(exe: Path, seed: int, most_us: int):
    env = {k: v for k, v in os.environ.items() if not k.startswith("SOA_")}
    out = subprocess.run([str(exe), str(seed), str(most_us)], capture_output=True, text=True, timeout=120, env=env)
    assert out.returncode == 0, out.stdout + out.stderr
    return out


def frames(out) -> dict:
    rows = {}
    for line in out.stdout.splitlines():
        if line.startswith("frame "):
            head, rest = line.split(":", 1)
            rows[int(head.split()[1])] = rest.split()
    return rows


@gnu
def test_the_pad_and_writes_land_at_frame_boundaries(built):
    out = run(built, 1, 300)
    rows = frames(out)
    assert sorted(rows) == list(range(FRAMES))
    # frame 0: the neutral port from the start; nothing on port 2
    assert rows[0][:3] == ["1:0000/0:0000"] * 3, rows[0]
    # frame 1: the pad given before the start (late), and then the one given while 0 was held
    for f in range(1, FRAMES):
        want = f"1:{(f * 37) & 0x1F7F:04X}"
        two = "1:0100" if f >= 10 else "0:0000"
        assert rows[f][:3] == [f"{want}/{two}"] * 3, (f, rows[f])
        # a write made while frame g was held is there from g + 1 (g a multiple of 5)
        g = (f - 1) - (f - 1) % 5
        assert rows[f][4] == f"{0x1000 + g:08X}", (f, rows[f])
    assert rows[0][4] == "00000000"  # the write before the start was refused
    assert f"stats: {FRAMES} held, {FRAMES + 2} pads, 1 late, 1 refused" in out.stderr, out.stderr


@gnu
def test_two_runs_with_different_host_timing_are_one_run(built):
    a = run(built, 1, 50)
    b = run(built, 7, 2000)
    assert a.stdout == b.stdout
    assert a.stderr.splitlines()[-1] == b.stderr.splitlines()[-1]
