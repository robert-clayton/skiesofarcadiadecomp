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
uint64_t gx_pipe_bytes(void) { return 0; }
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
uint32_t guest_timebase_lo(CpuState* s) { uint32_t v = (uint32_t)clock_fixed_now(); (void)s; clock_fixed_after_read(); g_clock_wait_reads |= CLOCK_READ_TB; return v; }
uint32_t guest_timebase_hi(CpuState* s) { (void)s; return (uint32_t)(clock_fixed_now() >> 32); }

/* the guest: a handler is its number, logged with the tick; the
 * decrementer's ("9") sets it again for the instant it ran while `rearm`
 * lasts, as the OS's does for a periodic alarm whose start is now */
void dec_write(CpuState* s, uint32_t v);
static unsigned rearm;
void dispatch(CpuState* s, uint32_t addr)
{
    printf("handler %X r3 %u at %llu\n", addr, s->gpr[3], (unsigned long long)clock_fixed_now());
    s->gpr[3] = 0xDEAD; /* clobbered, as a handler may */
    if (addr == 0x9u && rearm) rearm--, dec_write(s, 0);
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
    if (getenv("FREE")) { /* the quiet rule with free reads (the default), and a short spin limit */
        clock_fixed_configure(0, 0, 0);
        clock_fixed_set_spin(strtoull(getenv("FREE"), NULL, 10));
    } else if (getenv("QUIET")) { /* the quiet rule (poll ticks 0) with 8 ticks a read, and a short spin limit */
        clock_fixed_configure(8, 0, 0);
        clock_fixed_set_spin(strtoull(getenv("QUIET"), NULL, 10));
    } else {
        clock_fixed_configure(8, 1000, 0);
    }
    wr32(&s, 0x80003040u + 4 * 24, 0x24u); /* the OS's VI handler: "24" */
    wr32(&s, 0x80003040u + 4 * 5, 0x5u);   /* AI's: "5" */
    wr32(&s, 0x80003000u + 4 * 8, 0x9u);   /* the decrementer's: "9" */
    wr32(&s, 0x80003040u + 4 * 18, 0x12u); /* PE token's: "12" */
    s.gpr[3] = 77, s.gpr[31] = 31;
    while (scanf("%15s", cmd) == 1) {
        if (!strcmp(cmd, "idle") && scanf("%llu", &a) == 1) {
            while (a--) hook_80237BA8(&s);
        } else if (!strcmp(cmd, "busy") && scanf("%llu", &a) == 1) {
            s.msr |= 0x8000u;
            while (a--) irq_poll(&s);
            s.msr &= ~0x8000u;
        } else if (!strcmp(cmd, "wait") && scanf("%llu", &a) == 1) {
            /* a loop that changes no register: the same block, the same state */
            s.msr |= 0x8000u;
            s.pc = 0x80001000u;
            while (a--) irq_poll(&s);
            s.msr &= ~0x8000u;
        } else if (!strcmp(cmd, "work") && scanf("%llu", &a) == 1) {
            /* a loop that counts in a register: work, however long */
            s.msr |= 0x8000u;
            s.pc = 0x80002000u;
            while (a--) s.gpr[5]++, irq_poll(&s);
            s.msr &= ~0x8000u;
        } else if (!strcmp(cmd, "offwait") && scanf("%llu", &a) == 1) {
            /* a loop with interrupts off that changes no register: polling a status bit */
            s.pc = 0x80003000u;
            while (a--) irq_poll(&s);
        } else if (!strcmp(cmd, "until") && scanf("%llu", &a) == 1) {
            /* interrupts off, mftb until the timebase reaches a: the read is kept in r4 */
            unsigned passes = 0;
            s.pc = 0x80004000u;
            while ((s.gpr[4] = guest_timebase_lo(&s)) < a) passes++, irq_poll(&s);
            printf("until %llu: out at %u after %u passes\n", a, s.gpr[4], passes);
        } else if (!strcmp(cmd, "dec") && scanf("%llu %llu", &a, &b) == 2) {
            rearm = (unsigned)b;
            dec_write(&s, (uint32_t)a);
        } else if (!strcmp(cmd, "read")) {
            guest_timebase_lo(&s);
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


def drive(exe, script, env=None):
    run = subprocess.run(
        [str(exe)], input=script, capture_output=True, text=True, timeout=60, env=dict(os.environ, **(env or {}))
    )
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


# ---- the quiet rule (docs/CLOCK.md, "Native answers and the clock") ----


@gnu
def test_quiet_work_costs_no_time_and_delivers_nothing(driver):
    # a million back edges of work: the clock does not move, nothing is due
    out, err = drive(driver, "work 1000000\nregs\n", {"QUIET": "4194304"})
    assert out.splitlines()[-1].endswith(" now 0"), out
    assert "handler" not in out
    assert "0 waits found by a loop's repeated state" in err, err


@gnu
def test_quiet_a_loop_that_repeats_its_state_is_a_wait(driver):
    # the first back edge delivers what came due at the start; then the third
    # pass with the same registers is a wait: the clock goes to the retrace,
    # which is delivered there and then
    out, err = drive(driver, "wait 4\nregs\n", {"QUIET": "4194304"})
    assert f"handler 24 r3 24 at {VI}" in out, out
    assert out.splitlines()[-1] == f"r3 77 r31 31 now {VI}", out
    assert "1 waits found by a loop's repeated state (1 went to the next deadline)" in err, err
    # two passes are not enough
    out, _ = drive(driver, "wait 3\nregs\n", {"QUIET": "4194304"})
    assert out.splitlines()[-1].endswith(" now 0"), out


@gnu
def test_quiet_a_wait_that_counts_is_found_by_length(driver):
    out, err = drive(driver, "work 1000\nregs\nwork 1\nregs\n", {"QUIET": "1000"})
    lines = out.splitlines()
    assert lines[0].endswith(" now 0") and lines[-1].endswith(f" now {VI}"), out
    assert "1 by 1000 back edges with nothing due (1)" in err, err


@gnu
def test_quiet_an_event_is_delivered_at_the_first_back_edge_after_it_comes_due(driver):
    # 8 ticks a read; the event due at 20 runs at the first back edge after
    # the third read moved the clock to 24, not later and not at a count
    out, _ = drive(driver, "every 20 0\nread\nread\nwork 5\nread\nwork 1\nregs\n", {"QUIET": "4194304"})
    assert "event once due 20 at 24 ee 0" in out, out
    assert out.count("event once") == 1


@gnu
def test_quiet_the_same_drive_gives_the_same_run(driver):
    script = f"every 1234 {TICK5MS}\nidle 5\nwork 5000\nwait 7\nread\nwork 3\nraise 5\nwait 3\nidle 5\nregs\n"
    a, ea = drive(driver, script, {"QUIET": "4096"})
    b, eb = drive(driver, script, {"QUIET": "4096"})
    assert a == b and ea == eb
    assert "[clock] quiet:" in ea


# ---- reads cost nothing (the quiet rule's default since 2026-10-17) ----


@gnu
def test_free_reads_move_nothing(driver):
    out, err = drive(driver, "read\nread\nwork 100\nread\nregs\n", {"FREE": "4194304"})
    assert out.splitlines()[-1].endswith(" now 0"), out
    assert "0 waits found" in err, err


@gnu
def test_free_a_poll_with_interrupts_off_goes_to_the_deadline_and_delivers_later(driver):
    # nothing can be delivered with interrupts off: the third pass goes to the
    # retrace's tick, and the retrace is delivered at the first back edge with
    # them on, not before
    out, err = drive(driver, "offwait 4\nregs\nbusy 1\nregs\n", {"FREE": "4194304"})
    lines = out.splitlines()
    assert lines[0] == f"r3 77 r31 31 now {VI}", out
    assert lines[1] == f"handler 24 r3 24 at {VI}", out
    assert "1 waits found by a loop's repeated state (1 went to the next deadline)" in err, err


@gnu
def test_free_an_elapsed_time_loop_is_stepped_and_ends_late_by_less_than_it_waited(driver):
    # 8, 16, 32, ... ticks a wait, so a loop on mftb for 1000 ticks ends by 2000
    out, _ = drive(driver, "until 1000\nregs\n", {"FREE": "4194304"})
    line = next(x for x in out.splitlines() if x.startswith("until"))
    end = int(line.split(" out at ")[1].split()[0])
    assert 1000 <= end < 2000, out
    assert end == 8 * (2**7 - 1), out  # 8 + 16 + ... + 512 = 1016: seven waits, the last at 1016
    # with a deadline nearer than the step, it stops there
    out, _ = drive(driver, f"every 1010 0\nuntil 1000\n", {"FREE": "4194304"})
    assert "out at 1010" in out, out


@gnu
def test_free_a_decrementer_set_for_the_instant_it_fired_moves_time_on(driver):
    # the OS's periodic alarm started at now: fired, set again for now, three
    # times; each second exception at one instant moves the clock 8 ticks
    out, _ = drive(driver, "dec 0 3\nidle 1\nidle 1\nidle 1\nidle 1\nregs\n", {"FREE": "4194304"})
    fired = [int(x.split(" at ")[1]) for x in out.splitlines() if x.startswith("handler 9")]
    assert fired == [0, 8, 16, 24], out


@gnu
def test_a_raised_line_runs_in_its_device_s_place(driver):
    # the device order of a delivery: PE before AI, whatever the numbers
    out, _ = drive(driver, "raise 5\nraise 18\nidle 1\n")
    lines = [x for x in out.splitlines() if x.startswith("handler")]
    assert [x.split()[1] for x in lines[:2]] == ["12", "5"], out

