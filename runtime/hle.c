/*
 * Minimal runtime: logging MMIO stubs, traps and a real timebase.
 *
 * Enough to boot the recompiled program until it touches hardware we have
 * not modelled -- and to say exactly what it touched, which is how the
 * device models get prioritised. Reads return zero; accesses are counted per
 * register for the end-of-run report, and SOA_MMIO=1 additionally prints the
 * first few of each as they happen.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "cpu.h"
#include "clock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "plat.h"

#define MMIO_BASE 0xCC000000u
#define MMIO_SLOTS 0x2000u /* 32 KB of registers, one slot per word */
#define LOG_PER_REG 6      /* per-register budget for the SOA_MMIO log */
#define LOG_SYSCALLS 6     /* its own budget, so changing the one above says nothing about syscalls */

static uint32_t g_hits[MMIO_SLOTS];

static uint64_t g_syscalls;

/* Several hundred lines in the first second of a run, all of it a diagnostic
 * the end-of-run report already summarises: worth having, not worth reading
 * unless you asked. SOA_MMIO=1 asks. */
static int mmio_verbose(void)
{
    static int v = -1;
    if (v < 0) {
        const char* env = getenv("SOA_MMIO");
        v = env && atoi(env) ? 1 : 0;
    }
    return v;
}

void hle_report(void);
uint64_t gx_pipe_bytes(void);

/* A poll loop against a register we answer with zero never ends. Instead of
 * timing out, notice it: a long run of reads with no write in between, or
 * the same register read over and over, is a spin -- report which one. */
#define SPIN_SAME 200000u
#define SPIN_READS 2000000u
static uint32_t g_spin_ea, g_spin_same, g_reads_since_write;

void guest_backtrace(CpuState* s, uint32_t sp);

static void spin_check(CpuState* s, uint32_t ea, int is_write)
{
    if (is_write) {
        g_spin_same = 0;
        g_reads_since_write = 0;
        return;
    }
    if (ea == g_spin_ea) g_spin_same++;
    else { g_spin_ea = ea; g_spin_same = 0; }
    g_reads_since_write++;
    if (g_spin_same > SPIN_SAME || g_reads_since_write > SPIN_READS) {
        fprintf(stderr, "[spin] polling %08X: %u consecutive reads, %u reads since the last write\n",
                ea, g_spin_same, g_reads_since_write);
        fprintf(stderr, "[spin] pc %08X lr %08X; backtrace:", s->pc, s->lr);
        guest_backtrace(s, s->gpr[1]);
        hle_report();
        exit(4);
    }
}

static const char* peripheral(uint32_t ea)
{
    uint32_t off = ea - MMIO_BASE;
    if (off < 0x1000) return "CP";
    if (off < 0x2000) return "PE";
    if (off < 0x3000) return "VI";
    if (off < 0x4000) return "PI";
    if (off < 0x5000) return "MI";
    if (off < 0x6000) return "DSP";
    if (off < 0x6400) return "DI";
    if (off < 0x6800) return "SI";
    if (off < 0x6C00) return "EXI";
    if (off < 0x7000) return "AI";
    if (off < 0x8100) return "GP";
    return "??";
}

static void note(CpuState* s, const char* dir, uint32_t ea, unsigned size, uint64_t v)
{
    uint32_t slot;
    /* (write-gather pipe stores never come here: cpu.h sends them straight to gx_pipe_write) */
    spin_check(s, ea, dir[0] == 'w');
    if (ea < MMIO_BASE || ea >= MMIO_BASE + MMIO_SLOTS * 4u) {
        static int strict = -1, shown;
        if (strict < 0) strict = getenv("SOA_STRICT") ? 1 : 0;
        /* Its own tag: this one is a bug signal, not the log SOA_MMIO turns
         * on, and it stays on so that grepping away [mmio] cannot hide it. */
        if (shown++ < 20 || strict)
            fprintf(stderr, "[mmio!] %s %08X/%u = %llx (outside modelled range)\n", dir, ea, size,
                    (unsigned long long)v);
        if (strict) { /* almost always a garbage pointer: stop at the first one, with the stack */
            fprintf(stderr, "[strict] pc %08X lr %08X; backtrace:", s->pc, s->lr);
            guest_backtrace(s, s->gpr[1]);
            hle_report();
            exit(8);
        }
        return;
    }
    slot = (ea - MMIO_BASE) >> 2;
    g_hits[slot]++; /* counted whether or not it is printed: the report is built from these */
    if (mmio_verbose() && g_hits[slot] <= LOG_PER_REG)
        fprintf(stderr, "[mmio] %-3s %s %08X/%u = %llx\n", peripheral(ea), dir, ea, size,
                (unsigned long long)v);
}

/* Modelled devices (irq.c) get first refusal; everything else reads as zero. */
int device_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out);
void device_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v);

/* runtime/record.c (SOA_RECORD, docs/RECORD.md): while a recorded call is
 * under way, what it asks of the hardware and the clock is told to it --
 * an MMIO read with the value it read, an MMIO store before the device
 * model runs (a device may move memory the recorder has made read-only, and
 * the disc's reads do it through the kernel), each timebase read with its
 * value. Kept here rather than in record.c so this file links on its own, as
 * tests link it. The kinds are record.h's. */
volatile int g_record_armed;
unsigned g_record_polls; /* irq.c's deliveries held while a recorded call runs */
void (*g_record_note)(CpuState* s, int kind, uint32_t ea, uint64_t v);
#define RECORD_NOTE(s, kind, ea, v) do { if (g_record_armed && g_record_note) g_record_note((s), (kind), (ea), (v)); } while (0)

/* SOA_CLOCK=fixed (docs/CLOCK.md): a hardware read moves the fixed-step
 * clock as a timebase read does, after the answer, so a loop polling a
 * device's status with interrupts off sees its deadline come. */
static int g_clock_fixed;

#define MMIO_READ(T, N)                                        \
    T mmio_read##N(CpuState* s, uint32_t ea)                   \
    {                                                          \
        uint64_t v = 0;                                        \
        int handled = device_read(s, ea, sizeof(T), &v);       \
        note(s, handled ? "RD" : "rd", ea, sizeof(T), v);         \
        RECORD_NOTE(s, 4 | (int)sizeof(T) << 8, ea, (T)v);     \
        if (g_clock_fixed) clock_fixed_after_read();           \
        return (T)v;                                           \
    }
#define MMIO_WRITE(T, N)                                       \
    void mmio_write##N(CpuState* s, uint32_t ea, T v)          \
    {                                                          \
        RECORD_NOTE(s, 1 | (int)sizeof(T) << 8, ea, v);        \
        note(s, "wr", ea, sizeof(T), v);                          \
        device_write(s, ea, sizeof(T), v);                     \
        if (g_clock_fixed) clock_fixed_mark();                 \
    }

MMIO_READ(uint8_t, 8)
MMIO_READ(uint16_t, 16)
MMIO_READ(uint32_t, 32)
MMIO_READ(uint64_t, 64)
MMIO_WRITE(uint8_t, 8)
MMIO_WRITE(uint16_t, 16)
MMIO_WRITE(uint32_t, 32)
MMIO_WRITE(uint64_t, 64)

void irq_report(void);
void threads_report(void);
unsigned gx_frame_count(void);
uint64_t irq_retrace_count(void);
/* Defined at the bottom of this file, with the clocks they read. */
double hle_guest_seconds(void);
double hle_wall_seconds(void);
static void frametime_report(void);
unsigned hle_speed(void);
/* Defined in main.c, where the sampler lives. Called from here rather than
 * from the watchdog so that every stop path prints a profile: the old one was
 * reachable only from a stall, which is why no healthy run ever produced
 * one. */
void profile_report(void);

/* Reports that belong to something optional (mod.c's, seed.c's), registered
 * rather than called, so that this file still links without them. Room for
 * sixteen, and a seventeenth says so instead of vanishing, which is what the
 * fifth used to do when the table held four. */
#define REPORT_HOOKS 16
static void (*g_report_hooks[REPORT_HOOKS])(void);

void hle_on_report(void (*fn)(void))
{
    int i;
    for (i = 0; i < REPORT_HOOKS; i++)
        if (!g_report_hooks[i]) { g_report_hooks[i] = fn; return; }
    fprintf(stderr, "[hle] report hook table full (%d); a report will be missing\n", REPORT_HOOKS);
}

void hle_report(void)
{
    uint32_t i, distinct = 0;
    uint64_t total = 0;
    /* Every stop path ends here and then leaves, and printing the busiest
     * registers empties the table it prints from -- so a second report (the
     * frame limit and a window close landing together) would be a lie.
     *
     * The loser waits rather than returning: every caller _exit()s the moment
     * this returns, and doing that while the winner is still inside the chain
     * would cut the report off partway and leave the WAV header unfinalised
     * (audio_report, at the end of irq_report, is what writes the real data
     * size). The wait is bounded so a wedged reporter cannot hang the exit. */
    static plat_a32 reported, done;
    if (plat_cas32(&reported, 0, 1) != 0) {
        int waited = 0;
        while (!plat_load32(&done) && waited < 5000) { plat_sleep_ms(1); waited++; }
        return;
    }
    {
        unsigned frames = gx_frame_count();
        unsigned long long retraces = irq_retrace_count();
        double guest = hle_guest_seconds(), wall = hle_wall_seconds();
        fprintf(stderr, "[run] %u game frames, %llu VI retraces", frames, retraces);
        if (frames) fprintf(stderr, " (%.2f per frame; the game's 30 fps cap wants 2.00)", (double)retraces / frames);
        fprintf(stderr, "; %.1f guest seconds at SOA_SPEED=%u, %.1f wall seconds; %llu clock gap(s), %.1f s excluded\n",
                guest, hle_speed(), wall, clock_gaps(), clock_excluded_seconds());
    }
    frametime_report();
    for (i = 0; i < REPORT_HOOKS && g_report_hooks[i]; i++) g_report_hooks[i]();
    irq_report();
    threads_report();
    for (i = 0; i < MMIO_SLOTS; i++) {
        if (g_hits[i]) { distinct++; total += g_hits[i]; }
    }
    fprintf(stderr, "[hle] %llu MMIO accesses over %u registers; %llu bytes to the gather pipe; "
            "%llu syscalls\n", (unsigned long long)total, distinct,
            (unsigned long long)gx_pipe_bytes(), (unsigned long long)g_syscalls);
    /* The busiest registers say what the guest is waiting on. */
    {
        uint32_t shown;
        for (shown = 0; shown < 12; shown++) {
            uint32_t best = 0, best_i = 0;
            for (i = 0; i < MMIO_SLOTS; i++)
                if (g_hits[i] > best) { best = g_hits[i]; best_i = i; }
            if (!best) break;
            fprintf(stderr, "  %10u  %s %08X\n", best, peripheral(MMIO_BASE + best_i * 4),
                    MMIO_BASE + best_i * 4);
            g_hits[best_i] = 0;
        }
    }
    profile_report();
    fflush(stderr);
    plat_cas32(&done, 0, 1); /* release any other stop path waiting above */
}

void hle_dump(CpuState* s, uint32_t pc)
{
    int i;
    fprintf(stderr, "  pc=%08X lr=%08X ctr=%08X cr=%08X xer=%08X msr=%08X\n", pc, s->lr, s->ctr,
            s->cr, s->xer, s->msr);
    for (i = 0; i < 32; i += 4)
        fprintf(stderr, "  r%-2d=%08X r%-2d=%08X r%-2d=%08X r%-2d=%08X\n", i, s->gpr[i], i + 1,
                s->gpr[i + 1], i + 2, s->gpr[i + 2], i + 3, s->gpr[i + 3]);
}

void guest_trap(CpuState* s, uint32_t pc)
{
    fprintf(stderr, "[trap] at %08X\n", pc);
    hle_dump(s, pc);
    fprintf(stderr, "  backtrace from r1:");
    guest_backtrace(s, s->gpr[1]);
    hle_report();
    exit(3);
}

void guest_unimplemented(CpuState* s, uint32_t pc, const char* what)
{
    fprintf(stderr, "[unimplemented] %s at %08X\n", what, pc);
    hle_dump(s, pc);
    hle_report();
    exit(2);
}

/* For the self test, which holds the native cache calls to their twins. */
uint64_t hle_syscall_count(void)
{
    return g_syscalls;
}

void guest_syscall(CpuState* s, uint32_t pc)
{
    (void)s;
    g_syscalls++; /* the count is in the report either way; the lines are hardware tracing like the MMIO log */
    if (mmio_verbose() && g_syscalls <= LOG_SYSCALLS) fprintf(stderr, "[sc] at %08X\n", pc);
}

/* The Gekko timebase ticks at a quarter of the 162 MHz bus clock. */
#define TB_HZ 40500000ull

/* SOA_SPEED=n: guest time runs n times faster than the wall clock. Read here
 * rather than kept, so the report can print it whether or not the guest ever
 * got as far as reading the timebase. */
unsigned hle_speed(void)
{
    const char* env = getenv("SOA_SPEED");
    return env && atoi(env) > 0 ? (unsigned)atoi(env) : 1u;
}

static uint64_t g_tb_origin;
static int g_tb_started; /* the guest has read the timebase, so the origin is fixed */
static int g_clock_utc;  /* SOA_CLOCK=utc: the old wall-clock source, for bisecting a timing report */
double hle_wall_seconds(void);

/* A host gap the clock did not count (clock.c), with the frame it fell in. */
static void clock_gap(double gap_s)
{
    fprintf(stderr, "[clock] frame %u: a gap of %.1f s wall counted as 0 s of guest time\n", gx_frame_count(), gap_s);
}

/* SOA_CLOCK=fixed (docs/CLOCK.md): the fixed-step clock's three numbers,
 * each a non-negative whole number or the default. */
static unsigned long long env_ull(const char* name, unsigned long long dflt)
{
    const char* v = getenv(name);
    char* end;
    unsigned long long n;
    if (!v || !*v) return dflt;
    n = strtoull(v, &end, 0);
    if (*end || v[0] == '-') {
        fprintf(stderr, "[clock] %s=%s is not a whole number; %llu\n", name, v, dflt);
        return dflt;
    }
    return n;
}

/* SOA_CLOCK=fixed, set up once: from main() before the guest runs
 * (hle_clock_start), so the RTC is fixed from the first read too, or at the
 * first timebase read of a program that has no main of ours. */
static int fixed_setup(void)
{
    static int done;
    const char* src = getenv("SOA_CLOCK");
    if (done || !src || strcmp(src, "fixed")) return g_clock_fixed;
    done = 1;
    {
        unsigned long long poll = env_ull("SOA_CLOCK_POLL_TICKS", 0);
        unsigned long long rd = env_ull("SOA_CLOCK_READ_TICKS", poll ? 8 : 0);
        unsigned long long rtc = env_ull("SOA_CLOCK_RTC", 31622400ull), spin = env_ull("SOA_CLOCK_SPIN", 4194304);
        clock_fixed_configure(rd, poll, (uint32_t)rtc);
        clock_fixed_set_spin(spin);
        g_clock_fixed = 1;
        if (poll)
            fprintf(stderr, "[clock] SOA_CLOCK=fixed: guest time moves %llu ticks a timebase or hardware read, %llu a delivery "
                            "point, and to the next deadline when idle; SOA_SPEED and the host's clock are not read; "
                            "the RTC starts at %llu s past 2000\n", rd, poll, rtc);
        else
            fprintf(stderr, "[clock] SOA_CLOCK=fixed: guest time moves %llu ticks a timebase or hardware read and to the "
                            "next deadline when the guest waits (idle, the frame end, a loop whose state repeats, %llu back edges "
                            "with nothing due); other code costs none, "
                            "so a native answer moves nothing; SOA_SPEED and the host's clock are not read; "
                            "the RTC starts at %llu s past 2000\n", rd, spin, rtc);
    }
    return 1;
}

/* The Gekko timebase: clock.c's guest time (M19), which is monotonic and
 * does not count a host sleep or stall past SOA_CLOCK_GAP_MS; or, with
 * SOA_CLOCK=utc, the old wall time since the first read, times SOA_SPEED;
 * or, with SOA_CLOCK=fixed, clock.c's fixed-step ticks, which only the
 * guest's own work moves. */
static uint64_t timebase(void)
{
    uint64_t ns;
    if (g_clock_fixed) {
        g_tb_started = 1;
        return clock_fixed_now();
    }
    if (!g_tb_started) {
        const char* src = getenv("SOA_CLOCK");
        const char* gap = getenv("SOA_CLOCK_GAP_MS");
        g_clock_utc = src && !strcmp(src, "utc");
        if (fixed_setup()) {
            g_tb_started = 1;
            return clock_fixed_now();
        }
        if (src && *src && strcmp(src, "utc") && strcmp(src, "real"))
            fprintf(stderr, "[clock] SOA_CLOCK=%s is not fixed, utc or real; the real clock\n", src);
        clock_configure(hle_speed(), gap && *gap ? atol(gap) : -1, clock_gap);
        if (g_clock_utc) {
            struct timespec ts;
            timespec_get(&ts, TIME_UTC);
            g_tb_origin = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
            fprintf(stderr, "[clock] SOA_CLOCK=utc: guest time is wall time since the first read, as before M19\n");
        } else {
            clock_advance_at(clock_host_ns());
            fprintf(stderr, "[clock] origin at %.3f s of wall time since start\n", hle_wall_seconds());
        }
        g_tb_started = 1;
    }
    if (g_clock_utc) {
        struct timespec ts;
        timespec_get(&ts, TIME_UTC);
        ns = ((uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec - g_tb_origin) * hle_speed();
    } else {
        ns = clock_advance_at(clock_host_ns());
    }
    return ns / 1000000000ull * TB_HZ + ns % 1000000000ull * TB_HZ / 1000000000ull;
}

/* ---- the three clocks the report keeps apart ---------------------------
 *
 * Game frames are the guest's own output: the copies that present a picture.
 * Guest seconds are the clock the game reads, and they are wall seconds times
 * SOA_SPEED by construction, since the timebase above multiplies a real clock
 * -- so the two are not independent evidence and the report does not offer
 * them as though they were. The pair that is independent is frames against VI
 * retraces, which is why they share the line: at the game's 30 fps cap that
 * ratio wants to be 2.00, and how far above it a run sits is how many of its
 * own deadlines the run missed.
 *
 * Wall seconds come from QueryPerformanceCounter and not from the timebase's
 * own timespec_get(TIME_UTC), which is not monotonic: a system clock step
 * would move it, and the whole point of the number is to be the one thing in
 * the report that is not the guest's opinion.
 */
static uint64_t g_wall_origin;
static int g_wall_started;

/* The monotonic clock, so a change to the wall clock's time of day never
 * reaches a run's seconds (off Windows it was TIME_UTC until L9). */
static uint64_t wall_now(void) { return plat_mono_raw(); }

static double wall_hz(void)
{
    static double hz;
    if (hz == 0.0) hz = plat_mono_hz();
    return hz;
}

/* main() calls this once, before the guest starts, so that wall seconds mean
 * the run and not whatever part of it happened to ask first. */
void hle_clock_start(void)
{
    fixed_setup();
    if (g_wall_started) return;
    g_wall_started = 1;
    g_wall_origin = wall_now();
}

/* Per-frame wall time, marked once a frame by the frame hook (main.c). The
 * [run] line gives totals only, so a run that averages 29 fps and a run that
 * holds 33 ms and hitches to 200 ms every few seconds read the same; the
 * percentiles below tell them apart (PLAN-60FPS-MODS H3). */
static double* g_ft;
static size_t g_ft_n, g_ft_cap;
static double g_ft_last = -1.0;
static unsigned g_ft_from; /* 0: every frame of the run */
/* The guest thread marks frames while a window's close can run the report on
 * the UI thread, so the buffer is locked: the report copies it under the lock
 * and works from the copy (the review of 2026-09-25 found the report reading
 * g_ft_n twice across a malloc while a mark could append or realloc). */
static PlatLock g_ft_lock; /* held for a copy or an append: short (off Windows a no-op until L9) */
#define FT_LOCK() plat_lock(&g_ft_lock)
#define FT_UNLOCK() plat_unlock(&g_ft_lock)

/* Forget the frames so far, and count from frame `frame` on: SOA_UNCAP=N
 * calls this at N, so an uncapped run's percentiles are the uncapped
 * stretch's and not diluted by the capped frames a pad script needed to get
 * there. */
void hle_frametime_restart(unsigned frame)
{
    FT_LOCK();
    g_ft_n = 0;
    g_ft_from = frame;
    FT_UNLOCK();
}

void hle_frame_mark(void)
{
    double now = hle_wall_seconds();
    FT_LOCK();
    if (g_ft_last >= 0.0 && g_ft_n < (1u << 22)) {
        if (g_ft_n == g_ft_cap) {
            size_t cap = g_ft_cap ? g_ft_cap * 2 : 4096;
            double* p = (double*)realloc(g_ft, cap * sizeof *g_ft);
            if (!p) {
                FT_UNLOCK();
                return;
            }
            g_ft = p;
            g_ft_cap = cap;
        }
        g_ft[g_ft_n++] = now - g_ft_last;
    }
    g_ft_last = now;
    FT_UNLOCK();
}

static int cmp_double(const void* a, const void* b)
{
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

static double process_cpu_seconds(double* user, double* kernel) { return plat_process_cpu(user, kernel); }

static void frametime_report(void)
{
    double user, kernel, cpu = process_cpu_seconds(&user, &kernel);
    double* s = NULL;
    size_t n;
    unsigned ft_from;
    FT_LOCK();
    n = g_ft_n;
    ft_from = g_ft_from;
    if (n && (s = (double*)malloc(n * sizeof *s)) != NULL) memcpy(s, g_ft, n * sizeof *s);
    FT_UNLOCK();
    if (n) {
        if (s) {
            size_t i;
            double total = 0.0;
            char from[48] = "";
            for (i = 0; i < n; i++) total += s[i];
            qsort(s, n, sizeof *s, cmp_double);
            if (ft_from) snprintf(from, sizeof from, " from frame %u", ft_from);
            fprintf(stderr, "[frametime] %zu frames%s, %.1f a second; wall ms per frame: p50 %.1f, p95 %.1f, "
                            "p99 %.1f, max %.1f", n, from, total > 0.0 ? (double)n / total : 0.0, s[n / 2] * 1e3,
                    s[(n * 95) / 100] * 1e3, s[(n * 99) / 100] * 1e3, s[n - 1] * 1e3);
            free(s);
        }
    } else {
        fprintf(stderr, "[frametime] no frames marked");
    }
    fprintf(stderr, "; process CPU %.1f s (%.1f user + %.1f kernel)\n", cpu, user, kernel);
}

double hle_wall_seconds(void)
{
    if (!g_wall_started) return 0.0;
    return (double)(wall_now() - g_wall_origin) / wall_hz();
}

/* Zero until the guest has read the timebase at all, which OSInit does within
 * the first milliseconds. A run that stopped before that has no guest clock,
 * which is a different thing from a guest clock reading zero, and the report
 * says which. */
double hle_guest_seconds(void)
{
    if (!g_tb_started) return 0.0;
    if (g_clock_fixed) return (double)clock_fixed_now() / (double)TB_HZ;
    /* the report may run on the UI thread: read the clock, do not advance it */
    return g_clock_utc ? (double)timebase() / (double)TB_HZ : (double)clock_peek_at(clock_host_ns()) / 1e9;
}

/* Guest ticks now from any thread (irq.h): the clock read, not advanced. */
uint64_t hle_guest_ticks(void)
{
    uint64_t ns;
    if (!g_tb_started) return 0;
    if (g_clock_fixed) return clock_fixed_now();
    if (g_clock_utc) return timebase();
    ns = clock_peek_at(clock_host_ns());
    return ns / 1000000000ull * TB_HZ + ns % 1000000000ull * TB_HZ / 1000000000ull;
}

/* The race seed (seed.c, PLAN-GAMEPLAY-MODS P6): with SOA_SEED set, a read
 * from inside OSGetTick -- pc 0x8023851C, the only place the pc decides
 * anything -- goes to seed_pin, which pins the three reseed sites by their
 * return address and hands every other read its timebase. */
extern int g_seed_on;
uint32_t seed_pin(uint32_t lr, uint32_t tb);

/* 1 while a device model reads the timebase (guest_time_device): under the
 * fixed-step clock only the guest's own mftb moves time. */
static int g_tb_device;

uint32_t guest_timebase_lo(CpuState* s)
{
    uint32_t v = (uint32_t)timebase();
    v = g_seed_on && s->pc == 0x8023851Cu ? seed_pin(s->lr, v) : v;
    RECORD_NOTE(s, 2, 0, v);
    /* after the answer, so mftbu, mftb, mftbu (OSGetTime) agree on the high word */
    if (g_clock_fixed && !g_tb_device) {
        clock_fixed_after_read();
        g_clock_wait_reads |= CLOCK_READ_TB;
    }
    return v;
}
uint32_t guest_timebase_hi(CpuState* s)
{
    uint32_t v = (uint32_t)(timebase() >> 32);
    RECORD_NOTE(s, 3, 0, v);
    return v;
}

/* The timebase as the device models read it (irq.c, dvd.c, aram.c, dsp.c):
 * the same two reads as ever, high word first, so the real clock and a
 * recording see what they saw; under the fixed-step clock they move nothing. */
uint64_t guest_time_device(CpuState* s)
{
    uint64_t v;
    g_tb_device = 1;
    v = (uint64_t)guest_timebase_hi(s) << 32;
    v |= guest_timebase_lo(s);
    g_tb_device = 0;
    return v;
}

/* 1: the fixed-step clock is the guest's (SOA_CLOCK=fixed, read at the first
 * timebase read). */
int hle_clock_fixed(void)
{
    return g_clock_fixed;
}

/* ---- a snapshot (snapshot.h) ----------------------------------------------
 * The spin detector's run of reads is the future (a run that ends in [spin]
 * ends there whether resumed or not); the register counts are the report's. */
#include "snapshot.h"
void hle_snap(Snap* sn)
{
    int started = g_tb_started;
    SNAP(sn, g_hits);
    SNAP(sn, g_syscalls);
    SNAP(sn, g_spin_ea);
    SNAP(sn, g_spin_same);
    SNAP(sn, g_reads_since_write);
    SNAP(sn, started);
    if (snap_loading(sn) && started && !g_tb_started) {
        timebase(); /* the clock set up as the first read sets it (the real clock's origin) */
        g_tb_started = 1;
    }
}
