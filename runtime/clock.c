/*
 * The guest's clock (PLAN-60FPS-MODS M19; the comfort-pack spec 3.10): the
 * time the Gekko timebase reads, which every timer, interrupt and device
 * deadline in the port is counted in.
 *
 * It used to be wall time since the first read, times SOA_SPEED, from
 * timespec_get(TIME_UTC): a system clock step moved it, and a handheld put to
 * sleep woke to a clock a minute ahead -- the game's play time jumped, and
 * every deadline that had come due in the gap fired at once. Now guest time
 * accumulates speed x the host's monotonic time between reads, and a gap
 * between two reads longer than the threshold (SOA_CLOCK_GAP_MS, default
 * 2000; 0 turns the rule off) counts as no time at all: the host was asleep,
 * or stopped in a debugger, and the game should not see it. The default was
 * 250 ms and was measured too tight: the port's own work on the guest
 * thread -- a snapshot frame drawn after 99 undrawn ones, under a loaded
 * host -- stalled it 0.3-1.8 s, and those count as time, as they always did
 * (FINDINGS "M19, followed up"). Each gap, speed change and pause bumps an
 * epoch.
 *
 * Guest time is monotonic by construction: a host step backwards counts as
 * nothing. Pause (clock_pause) excludes its span too; tick.c's safe point
 * holds the guest thread while paused.
 *
 * clock_advance_at takes the host time as an argument, so
 * tools/tests/test_clock.py builds this file alone and drives it with
 * synthetic host times. Only the guest thread advances the clock; the report
 * reads it with clock_peek, which writes nothing.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "clock.h"
#include <stdio.h>
#include <stdlib.h>
#include "plat.h"

#define NS 1000000000ull

static int g_started;
static uint64_t g_last_host_ns, g_guest_ns, g_origin_host_ns;
static unsigned g_speed = 1;
static uint64_t g_gap_ns = 2000000000ull;
static volatile unsigned g_epoch;
static unsigned long long g_gaps;
static uint64_t g_excluded_ns;
static volatile int g_pause_req;
static int g_paused;
static void (*g_gap_note)(double gap_s);

void clock_configure(unsigned speed, long gap_ms, void (*gap_note)(double gap_s))
{
    g_speed = speed ? speed : 1;
    if (gap_ms >= 0) g_gap_ns = (uint64_t)gap_ms * 1000000ull;
    g_gap_note = gap_note;
}

/* plat.h's monotonic counter in nanoseconds, in whole numbers: a double
 * would round once the counter is large (the QPC rate on Windows, 1e9 off
 * it). */
uint64_t clock_host_ns(void)
{
    static uint64_t f;
    uint64_t c = plat_mono_raw();
    if (!f) f = (uint64_t)plat_mono_hz();
    return c / f * NS + c % f * NS / f;
}

uint64_t clock_advance_at(uint64_t host_ns)
{
    uint64_t d;
    if (!g_started) {
        g_started = 1;
        g_last_host_ns = g_origin_host_ns = host_ns;
        return g_guest_ns;
    }
    if (g_pause_req != g_paused) {
        if (g_pause_req) {
            /* up to the pause, then nothing until it ends */
            if (host_ns > g_last_host_ns) g_guest_ns += (host_ns - g_last_host_ns) * g_speed;
            g_last_host_ns = host_ns;
            g_paused = 1;
        } else {
            if (host_ns > g_last_host_ns) g_excluded_ns += host_ns - g_last_host_ns;
            g_last_host_ns = host_ns;
            g_paused = 0;
            g_epoch++;
        }
        return g_guest_ns;
    }
    if (g_paused) return g_guest_ns;
    if (host_ns <= g_last_host_ns) return g_guest_ns; /* a backward step is no time */
    d = host_ns - g_last_host_ns;
    g_last_host_ns = host_ns;
    if (g_gap_ns && d > g_gap_ns) {
        g_gaps++;
        g_excluded_ns += d;
        g_epoch++;
        if (g_gap_note) g_gap_note((double)d / (double)NS);
        return g_guest_ns;
    }
    g_guest_ns += d * g_speed;
    return g_guest_ns;
}

uint64_t clock_peek_at(uint64_t host_ns)
{
    uint64_t d;
    if (!g_started || g_paused || host_ns <= g_last_host_ns) return g_guest_ns;
    d = host_ns - g_last_host_ns;
    return g_gap_ns && d > g_gap_ns ? g_guest_ns : g_guest_ns + d * g_speed;
}

/* A span the guest thread itself waited out (a host holding the frame's
 * end, host.c): none of it is guest time. Up to the wait counts as ever. */
void clock_exclude_at(uint64_t from_host_ns, uint64_t to_host_ns)
{
    if (!g_started || to_host_ns <= from_host_ns) return;
    clock_advance_at(from_host_ns);
    if (g_paused || to_host_ns <= g_last_host_ns) return;
    g_excluded_ns += to_host_ns - g_last_host_ns;
    g_last_host_ns = to_host_ns;
    g_epoch++;
}

void clock_set_speed_at(unsigned speed, uint64_t host_ns)
{
    clock_advance_at(host_ns); /* everything so far at the old speed */
    g_speed = speed ? speed : 1;
    g_epoch++;
}

void clock_pause(int on)
{
    g_pause_req = on ? 1 : 0;
}

int clock_pause_requested(void)
{
    return g_pause_req;
}

unsigned clock_epoch(void)
{
    return g_epoch;
}

unsigned clock_speed(void)
{
    return g_speed;
}

int clock_started(void)
{
    return g_started;
}

double clock_origin_seconds(uint64_t since_host_ns)
{
    return g_started && g_origin_host_ns > since_host_ns ? (double)(g_origin_host_ns - since_host_ns) / (double)NS : 0.0;
}

unsigned long long clock_gaps(void)
{
    return g_gaps;
}

double clock_excluded_seconds(void)
{
    return (double)g_excluded_ns / (double)NS;
}

/* Test only: back to the start. */
void clock_reset(void)
{
    g_started = 0;
    g_last_host_ns = g_guest_ns = g_origin_host_ns = 0;
    g_speed = 1;
    g_gap_ns = 2000000000ull;
    g_epoch = 0;
    g_gaps = 0;
    g_excluded_ns = 0;
    g_pause_req = g_paused = 0;
    g_gap_note = NULL;
}
