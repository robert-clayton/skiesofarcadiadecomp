"""The fixed-step clock and native events (docs/CLOCK.md).

runtime/clock.c and runtime/irq.c are built with stand-ins for the devices
and the guest (the guest's handlers are numbers that log when dispatched)
and driven through the runtime's own entry points: the scheduler's idle
hook and the back-edge poll. Under SOA_CLOCK=fixed guest time moves only
with those calls and the timebase reads, so every tick below is exact:

- the idle loop goes straight to the next deadline, so retraces land on
  every 675,000 ticks and an event that falls while idle runs on its tick;
- a periodic event keeps its period from the tick it was due, not from when
  it ran, and is late while the guest is busy by less than the poll's step;
- a raised line calls the game's handler for that number once, and an event
  puts every register back and runs with interrupts off;
- a cancelled event does not run again; the table refuses a 33rd;
- two drives with the same calls give the same log byte for byte.

The compile wants gcc or clang (SOA_CC), as test_record.py's does.
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
    reason="needs gcc or clang (SOA_CC)",
)

VI = 675_000
TICK5MS = 202_500

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "clock.h"
#include "irq.h"

void hook_80237BA8(CpuState* s); /* irq.c's: the scheduler's idle loop */
void irq_report(void);

/* the devices: none busy, nothing pending */
int di_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out) { (void)s, (void)ea, (void)size, (void)out; return 0; }
int di_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v) { (void)s, (void)ea, (void)size, (void)v; return 0; }
int di_irq_pending(void) { return 0; }
void di_poll(CpuState* s) { (void)s; }
void aram_poll(CpuState* s) { (void)s; }
int di_next_due(uint64_t* due) { (void)due; return 0; }
int aram_next_due(uint64_t* due) { (void)due; return 0; }
int dsp_next_due(uint64_t* due) { (void)due; return 0; }
void dvd_report(void) {}
int gx_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out) { (void)s, (void)ea, (void)size, (void)out; return 0; }
int gx_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v) { (void)s, (void)ea, (void)size, (void)v; return 0; }
unsigned gx_pe_irq_pending(void) { return 0; }
void gx_report(void) {}
int aram_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out) { (void)s, (void)ea, (void)size, (void)out; return 0; }
int aram_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v) { (void)s, (void)ea, (void)size, (void)v; return 0; }
int aram_irq_pending(void) { return 0; }
void aram_report(void) {}
int dsp_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out) { (void)s, (void)ea, (void)size, (void)out; return 0; }
int dsp_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v) { (void)s, (void)ea, (void)size, (void)v; return 0; }
int dsp_irq_pending(void) { return 0; }
int ai_dma_irq_pending(void) { return 0; }
void dsp_poll(CpuState* s) { (void)s; }
void dsp_report(void) {}
int si_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out) { (void)s, (void)ea, (void)size, (void)out; return 0; }
int si_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v) { (void)s, (void)ea, (void)size, (void)v; return 0; }
int si_irq_pending(void) { return 0; }
void si_poll(void) {}
void si_report(void) {}
int exi_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out) { (void)s, (void)ea, (void)size, (void)out; return 0; }
int exi_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v) { (void)s, (void)ea, (void)size, (void)v; return 0; }
unsigned exi_irq_pending(void) { return 0; }
unsigned exi_exi_irq_pending(void) { return 0; }
void exi_report(void) {}
volatile int g_record_armed;
unsigned g_record_polls;
uint32_t mmio_read32(CpuState* s, uint32_t ea) { (void)s, (void)ea; return 0; } /* the tables are RAM; unused */

/* the timebase: hle.c's, under the fixed-step clock */
uint64_t guest_time_device(CpuState* s) { (void)s; return clock_fixed_now(); }
uint32_t guest_timebase_lo(CpuState* s) { uint32_t v = (uint32_t)clock_fixed_now(); (void)s; clock_fixed_after_read(); return v; }
uint32_t guest_timebase_hi(CpuState* s) { (void)s; return (uint32_t)(clock_fixed_now() >> 32); }

/* the guest: a handler is its number, logged with the tick */
void dispatch(CpuState* s, uint32_t addr)
{
    printf("handler %X r3 %u at %llu\n", addr, s->gpr[3], (unsigned long long)clock_fixed_now());
    s->gpr[3] = 0xDEAD; /* clobbered, as a handler may */
}
static void event(CpuState* s, void* user, void (*extra)(void), uint64_t due, uint64_t now)
{
    (void)extra;
    printf("event %s due %llu at %llu ee %u\n", (const char*)user, (unsigned long long)due, (unsigned long long)now,
           (s->msr >> 15) & 1u);
    s->gpr[3] = 0xBEEF;
    s->gpr[31] = 0xBEEF;
}
static void wr32(CpuState* s, uint32_t a, uint32_t v)
{
    uint8_t* p = s->mem + (a & MEM_MASK);
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

int main(void)
{
    static CpuState s;
    char cmd[16];
    unsigned long long a, b, ids[64];
    unsigned n = 0;
    s.mem = (uint8_t*)calloc(1, MEM_IMAGE_SIZE);
    clock_fixed_configure(8, 1000, 0);
    wr32(&s, 0x80003040u + 4 * 24, 0x24u); /* the OS's VI handler: "24" */
    wr32(&s, 0x80003040u + 4 * 5, 0x5u);   /* AI's: "5" */
    s.gpr[3] = 77, s.gpr[31] = 31;
    while (scanf("%15s", cmd) == 1) {
        if (!strcmp(cmd, "idle") && scanf("%llu", &a) == 1) {
            while (a--) hook_80237BA8(&s);
        } else if (!strcmp(cmd, "busy") && scanf("%llu", &a) == 1) {
            s.msr |= 0x8000u;
            while (a--) irq_poll(&s);
            s.msr &= ~0x8000u;
        } else if (!strcmp(cmd, "every") && scanf("%llu %llu", &a, &b) == 2) {
            ids[n] = (unsigned long long)irq_event_add(a, b, event, (void*)(b ? "tick" : "once"), NULL);
            printf("id %llu\n", ids[n]), n++;
        } else if (!strcmp(cmd, "cancel") && scanf("%llu", &a) == 1) {
            printf("cancel %d\n", irq_event_cancel((int)a));
        } else if (!strcmp(cmd, "raise") && scanf("%llu", &a) == 1) {
            printf("raise %d\n", irq_raise((unsigned)a));
        } else if (!strcmp(cmd, "fill")) {
            unsigned k, got = 0;
            for (k = 0; k < 40; k++) got += irq_event_add(~0ull, 0, event, (void*)"far", NULL) != 0;
            printf("filled %u\n", got);
        } else if (!strcmp(cmd, "regs")) {
            printf("r3 %u r31 %u now %llu\n", s.gpr[3], s.gpr[31], (unsigned long long)clock_fixed_now());
        }
    }
    irq_report();
    return 0;
}
"""


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    out = tmp_path_factory.mktemp("clockfixed")
    (out / "driver.c").write_text(DRIVER, encoding="utf-8")
    exe = out / "driver"
    proc = toolchain.cc(
        [
            *PROFILE.cflags,
            f"/I{RUNTIME}",
            f"/Fe{exe}",
            str(out / "driver.c"),
            str(RUNTIME / "irq.c"),
            str(RUNTIME / "clock.c"),
            *map(str, toolchain.runtime_support_sources()),
            *PROFILE.linker,
        ],
        out,
        PROFILE,
    )
    assert proc.returncode == 0, (proc.stdout or "") + (proc.stderr or "")
    return exe


def drive(exe, script):
    run = subprocess.run([str(exe)], input=script, capture_output=True, text=True, timeout=60)
    assert run.returncode == 0, run.stdout + run.stderr
    return run.stdout, run.stderr


@gnu
def test_idle_goes_to_each_deadline_and_events_run_on_their_tick(driver):
    out, err = drive(driver, f"every {VI + 1000} {TICK5MS}\nidle 40\nregs\n")
    lines = out.splitlines()
    vi = [int(x.split(" at ")[1]) for x in lines if x.startswith("handler 24")]
    assert vi[:4] == [VI, 2 * VI, 3 * VI, 4 * VI], out
    ticks = [x for x in lines if x.startswith("event tick")]
    # due and run on the same tick, period counted from the due tick, interrupts off inside
    for k, line in enumerate(ticks[:6]):
        due = VI + 1000 + k * TICK5MS
        assert line == f"event tick due {due} at {due} ee 0", out
    # every register of the interrupted code put back
    assert lines[-1].startswith("r3 77 r31 31 now "), out
    assert "late by at most 0 ticks" in err, err


@gnu
def test_a_busy_guest_runs_an_event_at_the_next_delivery_point(driver):
    # 16 back-edge polls are one delivery point, 1000 ticks of the fixed clock
    out, err = drive(driver, "every 5500 0\nbusy 160\nregs\n")
    assert "event once due 5500 at 6000 ee 0" in out, out
    assert "late by at most 500 ticks" in err, err
    assert out.count("event once") == 1


@gnu
def test_a_raised_line_calls_the_game_s_handler_once(driver):
    out, _ = drive(driver, "raise 5\nraise 5\nraise 40\nidle 1\nidle 1\nregs\n")
    assert "raise 1\nraise 1\nraise 0\n" in out
    assert out.count("handler 5 r3 5 at 0") == 1, out
    assert "r3 77 r31 31" in out


@gnu
def test_a_cancelled_event_stops_and_the_table_is_bounded(driver):
    out, _ = drive(driver, f"every 100 {TICK5MS}\nidle 3\ncancel 1\ncancel 1\nidle 6\nfill\n")
    assert out.count("event tick") == 1, out
    assert "cancel 1\ncancel 0\n" in out
    assert "filled 32" in out  # of 40 asked: 32 slots, the cancelled one free again


@gnu
def test_the_same_drive_gives_the_same_run(driver):
    script = f"every 1234 {TICK5MS}\nidle 25\nbusy 3000\nraise 5\nidle 25\nregs\n"
    a, ea = drive(driver, script)
    b, eb = drive(driver, script)
    assert a == b and ea == eb
    assert "[clock] fixed:" in ea
