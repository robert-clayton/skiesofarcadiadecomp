/*
 * Boot the recompiled program.
 *
 * Builds the MEM1 image the console's boot code and apploader would have left
 * behind -- the DOL's sections at their virtual addresses, BSS zeroed, the
 * FST parked below the arena, the low-memory globals the OS reads at start
 * -- then jumps to the DOL entry point exactly as the apploader would.
 *
 *     soa.exe [extracted-dir]
 */
#define _CRT_SECURE_NO_WARNINGS
#include "cpu.h"
#include "disc.h"
#include "sha1.h"
#include "gxr.h"
#include "gxv.h"
#include "mod.h"
#include "picture.h"
#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ENTRY_FN
#define ENTRY_FN fn_80003140 /* __start */
#endif
#define STR2(x) #x
#define STR(x) STR2(x)
void ENTRY_FN(CpuState* s);

/* The guest's call depth becomes the host's, so it runs on a stack as large
 * as the Windows link's /STACK (tools/recompile.py): there the main thread,
 * elsewhere a thread of that size, which then owns the MEM1 guard (L7). */
#define GUEST_STACK_BYTES ((size_t)32 << 20)
static int run_guest(void* s)
{
    plat_guard_owner();
    ENTRY_FN((CpuState*)s);
    return 0;
}
void hle_report(void);
void hle_dump(CpuState* s, uint32_t pc);
void threads_init(CpuState* s);
int selftest(CpuState* s);
int gx_replay(CpuState* s, const char* base);
int gx_replay_pair(CpuState* s, const char* a, const char* b); /* H10: A, B and B.mid.png */
void gxr_enable(int on);
void gxr_set_output(const char* png_path);
void watch_init(void);
void gxr_export_install(void);
void window_start(void);
void gx_set_frame_limit(unsigned frames);
void gx_set_frame_hook(void (*fn)(CpuState*, unsigned)); /* SOA_POKE; see gx.c */
unsigned gx_frame_count(void);
void gxr_draw_every_frame(void);
int irq_in_handler(void);
void hle_clock_start(void);
const char* settings_load(void); /* settings.c */
const char* settings_recorded(char* out, size_t cap);
int seed_init(char* in_effect, size_t cap); /* seed.c */
void settings_record_as(const char* key, const char* value);
void settings_check_mods(int (*loaded)(const char* id));
void si_set_motor_strength(int percent); /* si.c, M18 */
void si_motor_stop(void);
void si_set_chord_handler(void (*fn)(int chord, unsigned frame)); /* si.c, CH1 */
uint32_t si_host_buttons(void);
const char* si_chord_name(int chord, int what);
void mod_set_host_buttons(uint32_t (*fn)(void));

/* A gamepad chord (CH1): View with LB, RS or LS. One arm per chord, each
 * replaced by the slice that builds its action -- H19a fullscreen, M11a
 * turbo, M8 the menu -- so CH1 depends on none of them. */
int window_toggle_fullscreen(void); /* window.c, H19a */
int tick_turbo_toggle(void);        /* tick.c, M11a */
const char* settings_root(void); /* settings.c, M5b */
int settings_console_to_log(char* path, size_t cap);
void si_set_path_root(const char* root);
void aram_census_prepare(void); /* aram.c: its disc reads before the clock starts (M19) */
void tick_set_hold(int (*held)(void)); /* tick.c; M19's pause */
int clock_pause_requested(void);
void si_set_pad2_source(int (*fn)(uint16_t* buttons, uint8_t stick[2], uint8_t cstick[2], uint8_t trig[2]));
int si_read_pad(unsigned port, void* out);
int window_pad2(uint16_t* buttons, uint8_t stick[2], uint8_t cstick[2], uint8_t trig[2]);

static void on_chord(int chord, unsigned frame)
{
    switch (chord) {
    case 0: /* view+lb: fullscreen on the UI thread, which owns the window (H19a) */
        fprintf(stderr, "[chord] frame %u: %s -> %s%s\n", frame, si_chord_name(chord, 0), si_chord_name(chord, 1),
                window_toggle_fullscreen() ? "" : " (no window: logged only)");
        break;
    case 1: /* view+rs: turbo, from the next safe point (M11a) */
        fprintf(stderr, "[chord] frame %u: %s -> %s %s\n", frame, si_chord_name(chord, 0), si_chord_name(chord, 1),
                tick_turbo_toggle() ? "on" : "off");
        break;
    default:
        fprintf(stderr, "[chord] frame %u: %s -> %s (not built yet)\n", frame, si_chord_name(chord, 0),
                si_chord_name(chord, 1));
        break;
    }
}
int mod_loaded(const char* id); /* mod.c */
void seed_report(void);
/* Set while gx.c is inside the command-stream parse. The sampler reads it
 * because a clock pair there would cost more than the parse. */
extern int g_gx_parsing;

/* A loop that never touches hardware never trips the MMIO spin detector, so
 * a second thread waits SOA_WATCHDOG seconds without a video frame and then
 * reports the block the guest is in. It defaults to 20 seconds headless and
 * to off when a window is open, since a window means a person is driving and
 * a timeout would cut them off mid-play; an explicit SOA_WATCHDOG wins
 * either way. It times a stall, not the run: a run that is still presenting
 * frames is working, and killing it is what made a long SOA_FRAMES run
 * impossible headless. */
#ifdef _WIN32
#include <process.h>
#include <windows.h>
#endif
static CpuState* g_state;

/* ---- naming a block address ---------------------------------------------
 *
 * A sample is the address of a basic block, not of a function: cpu.h says so,
 * and the recompiler stores it at every label. config/functions.tsv is the
 * inventory tools/ builds -- sorted by address, with each function's size --
 * so a block resolves to the function containing it by binary search on
 * containment. Equality would miss every block but the first of each
 * function, which is what made the old profile a list of addresses.
 *
 * Read once, at the report, on the reporting thread: 320 KB and a sort of
 * 7,144 rows at exit, and nothing in any hot path touches it. It is the first
 * file the binary opens out of config/, so a run started from anywhere but
 * the repository root will not find it -- the header below says which file it
 * used and how many rows it got, so a table of bare hex reads as "no
 * inventory here" rather than as "these functions have no names". Most rows
 * in that file are still fn_XXXXXXXX, which the header also says, for the
 * same reason.
 */
typedef struct {
    uint32_t addr, size;
    const char* name;
} Sym;
static Sym* g_syms;
static unsigned g_nsyms;
static char* g_symtext;
static char g_sympath[512];
static int g_sym_tried;

static int sym_cmp(const void* a, const void* b)
{
    uint32_t x = ((const Sym*)a)->addr, y = ((const Sym*)b)->addr;
    return x < y ? -1 : x > y ? 1 : 0;
}

static void sym_load(void)
{
    const char* env = getenv("SOA_SYMBOLS");
    FILE* f;
    long len;
    size_t got, lines = 0, i;
    char* line;
    int first = 1;
    if (g_sym_tried) return;
    g_sym_tried = 1;
    snprintf(g_sympath, sizeof g_sympath, "%s", env && *env ? env : "config/functions.tsv");
    f = fopen(g_sympath, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return; }
    g_symtext = (char*)malloc((size_t)len + 1);
    if (!g_symtext) { fclose(f); return; }
    got = fread(g_symtext, 1, (size_t)len, f);
    fclose(f);
    g_symtext[got] = 0;
    for (i = 0; i < got; i++)
        if (g_symtext[i] == '\n') lines++;
    g_syms = (Sym*)malloc(sizeof(Sym) * (lines + 1));
    if (!g_syms) { free(g_symtext); g_symtext = NULL; return; }
    /* address, size, name, then six columns this does not need. The file is
     * written with CRLF; the name ends at the tab after it either way. */
    for (line = g_symtext; line && *line;) {
        char* eol = strchr(line, '\n');
        if (eol) *eol = 0;
        if (first) first = 0; /* the header row */
        else {
            char* t1 = strchr(line, '\t');
            char* t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
            char* t3 = t2 ? strchr(t2 + 1, '\t') : NULL;
            if (t3) {
                Sym* e = &g_syms[g_nsyms];
                *t1 = *t2 = *t3 = 0;
                e->addr = (uint32_t)strtoul(line, NULL, 0);
                e->size = (uint32_t)strtoul(t1 + 1, NULL, 10);
                e->name = t2 + 1;
                if (e->addr && e->size) g_nsyms++;
            }
        }
        line = eol ? eol + 1 : NULL;
    }
    qsort(g_syms, g_nsyms, sizeof g_syms[0], sym_cmp);
}

/* The function containing pc, and where it starts. NULL when no row contains
 * it -- the inventory has ten small gaps, and a run without config/ has none
 * of it at all. */
static const char* sym_name(uint32_t pc, uint32_t* start)
{
    unsigned lo = 0, hi;
    /* Loaded before the bound is read, not after: reading g_nsyms first made
     * the first lookup of every run search an empty table and answer NULL,
     * which showed up as exactly one function in the report going unnamed. */
    sym_load();
    hi = g_nsyms;
    if (!hi) return NULL;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (g_syms[mid].addr <= pc) lo = mid + 1;
        else hi = mid;
    }
    if (!lo) return NULL;
    lo--;
    if (pc - g_syms[lo].addr >= g_syms[lo].size) return NULL;
    if (start) *start = g_syms[lo].addr;
    return g_syms[lo].name;
}

/* " (name+0xNN)" for the bare addresses this file prints, or "" when there is
 * no inventory to name them from. One static buffer: every caller is on a
 * stop path, one at a time. */
static const char* block_name(uint32_t pc)
{
    static char buf[128];
    uint32_t start = 0;
    const char* n = sym_name(pc, &start);
    if (!n) return "";
    if (pc == start) snprintf(buf, sizeof buf, " (%s)", n);
    else snprintf(buf, sizeof buf, " (%s+0x%X)", n, pc - start);
    return buf;
}

#ifdef _WIN32
/* ---- the sampler ---------------------------------------------------------
 *
 * Its own thread, started before the guest and never stopped until the
 * report. The old one was the watchdog's second job, zeroed its samples on
 * every presented frame and only ever printed from the stall path, so a
 * healthy run produced no profile at all and every saved one covered a stall.
 * Splitting the two jobs is the whole fix: the watchdog still watches for a
 * stall, this samples the run.
 *
 * Each tick reads three words the guest thread is writing -- its block
 * address, the renderer phase, and the parse marker -- and charges the wall
 * interval since the previous tick to whichever of them applies, phase first,
 * then the parse, then the block. That ordering is what makes the buckets
 * disjoint: a sample belongs to a renderer phase or to a guest function,
 * never to both. The reads are plain aligned loads with no coherence between
 * them; at a millisecond and a half apart the window in which they can
 * disagree is nanoseconds, which is worth one sentence rather than a lock
 * that would change what is being measured.
 *
 * Weighted by that interval rather than counted, because the pacer is jittery
 * by design: counting would over-represent whatever happens to be running
 * when the timer fires early. The weights are seconds, which is also what
 * makes this table comparable with the renderer's timers.
 *
 * What it cannot tell you: anything shorter than its period. It answers "what
 * share of the thread went where" over a run, never "how long did one frame's
 * prepare take" -- the timers in gxr.c answer that. And it samples the guest
 * thread only, so the workers are absent from it by construction and measured
 * by the busy/idle pair in gxr.c instead.
 */
#define PROF_SLOTS 65536          /* a count per distinct block; a run of any length fits */
#define PROF_PERIOD_100NS 14000   /* 1.4 ms, which is what a high-resolution timer delivers */
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static uint32_t g_prof_key[PROF_SLOTS]; /* block address, bit 0 set inside a handler */
static uint64_t g_prof_tk[PROF_SLOTS];  /* performance-counter ticks charged to it */
static uint64_t g_prof_phase[T_COUNT];
static uint64_t g_prof_parse;   /* inside gx.c's command-stream parse */
static uint64_t g_prof_noblock; /* before the guest ran a block at all */
static uint64_t g_prof_lost;    /* the table filled: more distinct blocks than slots */
static uint64_t g_prof_samples, g_prof_span;
static int g_prof_on;
/* Enough rows for the plan's top ten and a little context. The report is
 * printed at the end of every run and is already long, so the rest go into
 * one line that says how much they came to; SOA_PROFILE=N asks for N. */
static int g_prof_rows = 12;
static volatile long g_prof_stop, g_prof_stopped;

static void prof_add(uint32_t key, uint64_t dt)
{
    unsigned h = (unsigned)((key * 2654435761u) >> 16) & (PROF_SLOTS - 1), i;
    /* The probe is bounded rather than allowed to walk the whole table: a run
     * with more distinct blocks than this has room for would otherwise turn
     * every sample into a scan of 65,536 slots, on a thread that wakes 700
     * times a second, and the profiler would start showing up in the program
     * it is measuring. Sixty-four is far past what a half-full table needs,
     * and what falls off the end is counted and named in the report rather
     * than dropped. */
    for (i = 0; i < 64; i++) {
        unsigned k = (h + i) & (PROF_SLOTS - 1);
        if (g_prof_key[k] == key) { g_prof_tk[k] += dt; return; }
        if (!g_prof_key[k]) { g_prof_key[k] = key; g_prof_tk[k] = dt; return; }
    }
    g_prof_lost += dt;
}

static unsigned __stdcall sampler(void* arg)
{
    HANDLE timer = (HANDLE)arg;
    LARGE_INTEGER prev, now;
    uint32_t rnd = 0x9E3779B9u;
    QueryPerformanceCounter(&prev);
    while (!g_prof_stop) {
        LARGE_INTEGER due;
        uint64_t dt;
        uint32_t pc;
        int ph;
        /* Twenty per cent of jitter on purpose. A fixed period against a
         * 60 Hz retrace and a 30 Hz present can lock onto one phase of the
         * frame and systematically over-count whatever runs there -- and two
         * runs would then agree with each other and with nothing else, which
         * is the one failure the "same top ten twice" check cannot see. */
        rnd ^= rnd << 13;
        rnd ^= rnd >> 17;
        rnd ^= rnd << 5;
        if (timer) {
            due.QuadPart = -(LONGLONG)(PROF_PERIOD_100NS * 4 / 5 + rnd % (PROF_PERIOD_100NS * 2 / 5 + 1));
            if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, 1000);
            else Sleep(1);
        } else Sleep(1); /* ~15.6 ms, so about a twelfth of the samples */
        QueryPerformanceCounter(&now);
        dt = (uint64_t)(now.QuadPart - prev.QuadPart);
        prev = now;
        g_prof_samples++;
        g_prof_span += dt;
        ph = g_gxr_phase;
        if (ph > T_HOST && ph < T_COUNT) { g_prof_phase[ph] += dt; continue; }
        if (g_gx_parsing) { g_prof_parse += dt; continue; }
        pc = g_state ? g_state->pc : 0u;
        if (!pc) { g_prof_noblock += dt; continue; }
        /* Block addresses are 4-aligned; bit 0 tags samples taken in a handler. */
        prof_add((pc & ~1u) | (irq_in_handler() ? 1u : 0u), dt);
    }
    g_prof_stopped = 1;
    return 0;
}

static void profile_start(CpuState* s)
{
    const char* env = getenv("SOA_PROFILE");
    HANDLE timer;
    uintptr_t h;
    g_state = s;
    if (env) {
        int n = atoi(env);
        if (!n) return; /* SOA_PROFILE=0: one fewer thread on the machine */
        if (n > 1) g_prof_rows = n;
    }
    /* A high-resolution waitable timer delivers about 1.4 ms here where
     * Sleep(1) delivers 15.9, which is eleven times the samples -- and unlike
     * timeBeginPeriod it does not raise the timer resolution for the whole
     * process. That would change the scheduling quantum the renderer's
     * Sleep(0) backoff rides on, and that backoff is one of the things being
     * measured. Older Windows refuses the flag; then a plain timer, and
     * failing that Sleep(1) and a twelfth of the resolution. */
    timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    h = _beginthreadex(NULL, 0, sampler, timer, 0, NULL);
    if (!h) {
        fprintf(stderr, "[profile] cannot start the sampler thread; this run has no profile\n");
        if (timer) CloseHandle(timer);
        return;
    }
    CloseHandle((HANDLE)h);
    g_prof_on = 1;
}

typedef struct {
    uint32_t key; /* function start, bit 0 set inside a handler; 0 for a named bucket */
    const char* name;
    uint64_t tk;
} Row;

static int row_key_cmp(const void* a, const void* b)
{
    uint32_t x = ((const Row*)a)->key, y = ((const Row*)b)->key;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* Heaviest first, and a total order rather than only a ranking: qsort is not
 * stable, so two rows the sort called equal could come out either way round,
 * and "the same top ten twice" would then fail on a tie that was never a
 * difference. */
static int row_tk_cmp(const void* a, const void* b)
{
    const Row *p = (const Row*)a, *q = (const Row*)b;
    if (p->tk != q->tk) return p->tk < q->tk ? 1 : -1;
    if (p->key != q->key) return p->key < q->key ? -1 : 1;
    if (!p->name || !q->name) return (p->name ? 0 : 1) - (q->name ? 0 : 1);
    return strcmp(p->name, q->name);
}

/* Every phase gets a row whether or not it fired, so the vocabulary the
 * report uses for the renderer is the same in the table and in the [gxr]
 * lines above it. */
static const char* const PHASE_NAME[T_COUNT] = {
    "[host]", "[gxr] vertex setup", "[gxr] TEV setup", "[gxr] texture decode",
    "[gxr] EFB copy", "[gxr] PNG write", "[gxr] waiting for the workers", "[gxr] rasterizing inline",
};


void profile_report(void)
{
    static Row rows[PROF_SLOTS + T_COUNT + 4];
    unsigned n = 0, i, j, shown;
    uint64_t total = 0, named = 0, printed = 0;
    double hz, span;
    LARGE_INTEGER f;
    if (!g_prof_on) return;
    /* Stop the one writer before reading its table, and bound the wait: a
     * sampler that has wedged must not hold up an exit path. */
    g_prof_stop = 1;
    for (i = 0; i < 200 && !g_prof_stopped; i++) Sleep(1);
    QueryPerformanceFrequency(&f);
    hz = (double)f.QuadPart;
    span = hz > 0.0 ? (double)g_prof_span / hz : 0.0;
    if (!g_prof_samples || span <= 0.0) {
        fprintf(stderr, "[profile] no samples\n");
        return;
    }
    /* Fold each block onto the function containing it. Two blocks of one
     * function ranked separately is what made the old table jitter between
     * runs, and folding is also what lets a row carry a name at all. */
    for (i = 0; i < PROF_SLOTS; i++) {
        uint32_t pc, start;
        const char* name;
        if (!g_prof_key[i] || !g_prof_tk[i]) continue;
        pc = g_prof_key[i] & ~1u;
        start = pc;
        name = sym_name(pc, &start);
        rows[n].key = start | (g_prof_key[i] & 1u);
        rows[n].name = name;
        rows[n].tk = g_prof_tk[i];
        if (name) named += g_prof_tk[i];
        n++;
    }
    qsort(rows, n, sizeof rows[0], row_key_cmp);
    for (i = 0, j = 0; i < n; i++) {
        if (j && rows[j - 1].key == rows[i].key) rows[j - 1].tk += rows[i].tk;
        else rows[j++] = rows[i];
    }
    n = j;
    for (i = T_HOST + 1; i < T_COUNT; i++) {
        if (!g_prof_phase[i]) continue;
        rows[n].key = 0;
        rows[n].name = PHASE_NAME[i];
        rows[n].tk = g_prof_phase[i];
        n++;
    }
    if (g_prof_parse) {
        rows[n].key = 0;
        rows[n].name = "[gx] command-stream parse";
        rows[n++].tk = g_prof_parse;
    }
    if (g_prof_noblock) {
        rows[n].key = 0;
        rows[n].name = "[boot] before the guest's first block";
        rows[n++].tk = g_prof_noblock;
    }
    if (g_prof_lost) {
        rows[n].key = 0;
        rows[n].name = "[profile] past the sample table's reach";
        rows[n++].tk = g_prof_lost;
    }
    for (i = 0; i < n; i++) total += rows[i].tk;
    qsort(rows, n, sizeof rows[0], row_tk_cmp);
    fprintf(stderr,
            "[profile] %llu samples at %.0f Hz over %.1fs of wall clock, each weighted by the "
            "interval it covers; %u entries, %.0f%% of the time named from %s (%u rows, most of "
            "them still fn_XXXXXXXX). H = in an interrupt handler.\n",
            (unsigned long long)g_prof_samples, (double)g_prof_samples / span, span, n,
            total ? 100.0 * named / total : 0.0, g_nsyms ? g_sympath : "no symbol file (run from the repository root, or set SOA_SYMBOLS)",
            g_nsyms);
    shown = n < (unsigned)g_prof_rows ? n : (unsigned)g_prof_rows;
    for (i = 0; i < shown; i++) {
        double pct = total ? 100.0 * rows[i].tk / total : 0.0;
        printed += rows[i].tk;
        if (!rows[i].key) fprintf(stderr, "  %5.1f%%           %s\n", pct, rows[i].name);
        else if (rows[i].name)
            fprintf(stderr, "  %5.1f%%  %08X %s%s\n", pct, rows[i].key & ~1u, rows[i].name,
                    (rows[i].key & 1u) ? " H" : "");
        else
            fprintf(stderr, "  %5.1f%%  %08X (no row in the inventory covers it)%s\n", pct,
                    rows[i].key & ~1u, (rows[i].key & 1u) ? " H" : "");
    }
    if (n > shown)
        fprintf(stderr, "  %5.1f%%           the other %u entries\n",
                total ? 100.0 * (total - printed) / total : 0.0, n - shown);
    /* Two independent measurements of the same seconds, printed side by side
     * rather than reconciled in private. They disagree by sampling error and
     * by anything this does not know about -- a phase entered on a path with
     * no boundary, a clock miscalibrated -- so a gap much wider than the
     * sampling error is a defect in one of them, and the only way to see it
     * is to print it. */
    gxr_timing_finish();
    if (gxr_producer_span() > 0.0) {
        int worst = 0;
        double gap = -1.0;
        for (i = T_HOST + 1; i < T_COUNT; i++) {
            double d = gxr_seconds(g_gxr_ticks[i]) - (double)g_prof_phase[i] / hz;
            if (d < 0.0) d = -d;
            if (d > gap) { gap = d; worst = (int)i; }
        }
        fprintf(stderr, "[profile] sampler against timers, widest of the %d renderer phases: %s "
                        "%.2fs timed, %.2fs sampled, %.1f%% of the run apart\n",
                T_COUNT - 1, PHASE_NAME[worst], gxr_seconds(g_gxr_ticks[worst]),
                (double)g_prof_phase[worst] / hz, 100.0 * gap / span);
    }
}

#else
/* No sampler off Windows: there is no worker pool there either, and the
 * report says nothing rather than print an empty table. */
static void profile_start(CpuState* s) { (void)s; }
void profile_report(void) {}
#endif

/* ---- the watchdog --------------------------------------------------------
 * On every platform since L7: a plain thread, a monotonic clock and a 1 ms
 * sleep, through plat.h. */
void guest_backtrace(CpuState* s, uint32_t sp);
static void watchdog(void* arg)
{
    unsigned secs = (unsigned)(uintptr_t)arg;
    unsigned frames = gx_frame_count();
    uint64_t t0 = plat_mono_ns();
    /* Watching for a stall is the whole of this thread's job now. It used to
     * sample as well, and zero its samples on every presented frame, which is
     * why the only profile it could ever print was of a stall. The sampler
     * above runs the whole time instead.
     *
     * A 1 ms sleep is really ~15 ms at Windows' default timer resolution,
     * so pace the wait by the clock rather than by counting sleeps. */
    for (;;) {
        unsigned now;
        plat_sleep_ms(1);
        /* Every frame presented restarts the clock, so the timeout means what
         * the message says -- nothing happened for this long. A boot that
         * never reaches its first frame still reports, on time. */
        now = gx_frame_count();
        if (now != frames) { frames = now; t0 = plat_mono_ns(); continue; }
        if (plat_mono_ns() - t0 >= (uint64_t)secs * 1000000000u) break;
    }
    fprintf(stderr, "[watchdog] no video frame for %us (SOA_WATCHDOG=0 disables it, SOA_WATCHDOG=s "
                    "changes the timeout); %u frames so far, last block %08X%s\n", secs, frames,
            g_state->pc, block_name(g_state->pc));
    hle_dump(g_state, g_state->pc);
    fprintf(stderr, "  backtrace from r1:");
    guest_backtrace(g_state, g_state->gpr[1]);
    hle_report(); /* which prints the profile, on this path and on every other */
    _Exit(5);
}
static int g_watchdog_on;

/* Returns the timeout it armed, so the startup line can say what will end
 * the run rather than guess. */
static unsigned start_watchdog(CpuState* s, int windowed)
{
    const char* env = getenv("SOA_WATCHDOG");
    unsigned secs = env ? (unsigned)atoi(env) : (windowed ? 0u : 20u);
    g_state = s;
    if (secs) {
        /* Only report a timeout there is really a thread behind: the startup
         * line says what will end the run, and a thread that never started
         * would make that a lie. */
        PlatThread t;
        if (!plat_thread_start(&t, watchdog, (void*)(uintptr_t)secs, 0)) {
            fprintf(stderr, "[watchdog] cannot start the watchdog thread; nothing will time this run out\n");
            return 0;
        }
        plat_thread_detach(&t);
        g_watchdog_on = 1;
    }
    return secs;
}

/* The window standing down the watchdog is only safe while the window turns
 * up; if it fails to open, the run would be headless with nothing watching
 * it at all. window.c calls this on that path. */
void watchdog_fallback(void)
{
    const char* env = getenv("SOA_WATCHDOG");
    if (g_watchdog_on || !g_state || (env && !atoi(env))) return;
    fprintf(stderr, "[watchdog] no window after all; arming the headless default\n");
    start_watchdog(g_state, 0);
}

/* ---- the MEM1 image, and the 8 MB of it that is not RAM ------------------
 *
 * mem_ptr masks an effective address with MEM_MASK and returns a pointer.
 * That is the whole of the guest's address translation and it runs on every
 * load and store in 55 MB of generated C, so it cannot afford to check
 * anything. The mask covers 32 MB and the console has 24, which leaves the
 * top eighth of the window -- 0x81800000 up, and its uncached and real-mode
 * aliases -- pointing past the end of the RAM. Narrowing the mask is not on:
 * 24 MB is not a power of two, so folding the range back would put a test on
 * every guest memory access to pay for an address the game should never form.
 *
 * Instead the image is the mask's whole range, and the part of it above the
 * RAM is reserved rather than committed. mem_ptr is untouched and the common
 * case costs exactly what it cost before; a stray access lands in reserved
 * address space and faults, and the handler below reports it, commits the
 * range and lets the run carry on against zeroed pages rather than against
 * the host heap. Committing all of it at the first fault is also what holds
 * this to a single message: afterwards there is nothing left up there to
 * fault on, so a guest loop cannot turn the tripwire into a stream.
 */
static int g_mem_guarded;

/* Reserved past the mask's range as well, so an 8-byte load that starts in
 * its last bytes has somewhere to land. 64K: the granularity VirtualAlloc
 * reserves in, and a whole number of pages on every other platform. */
#define MEM_RESERVE_BYTES ((size_t)MEM_MASK + 1u + 0x10000u)
static uint8_t* g_mem_base;
static CpuState* g_mem_state;

/* plat.c's guard calls this for a fault in the part above the RAM. The
 * thread that runs the guest owns the guard (plat_guard_owner), so a fault on
 * any other is the runtime's own code and s->pc belongs to neither it nor the
 * moment: say which kind of fault it was rather than print a block address
 * that had nothing to do with it. */
static int mem_fault(size_t off, int storing, int guest)
{
    static plat_a32 reported;
    int committed;
    /* Commit first: the report walks the guest stack, and that walk must not
     * fault its way back in here. A commit that fails leaves the access
     * violation standing and the process dies of it -- so say so first,
     * because the message is the entire point of the mechanism and a bare
     * access violation explains nothing. */
    committed = plat_commit(g_mem_base + MEM1_SIZE, MEM_RESERVE_BYTES - MEM1_SIZE);
    if (plat_cas32(&reported, 0, 1) == 0) {
        CpuState* s = g_mem_state;
        char who[192];
        if (guest) snprintf(who, sizeof who, "from block %08X%s", s ? s->pc : 0u,
                            block_name(s ? s->pc : 0u));
        else snprintf(who, sizeof who, "on a runtime thread, not the guest's");
        /* Reported in the cached window, because the mask has already thrown
         * away which of the three windows the guest used, and as the address
         * the access reached rather than the one it started from: an access
         * straddling the end of the RAM stops at the first byte past it. */
        fprintf(stderr,
                "[mem] %s %s reached %08X, past the console's 24 MB of RAM; the port "
                "keeps zeroed scratch up there so that it does not reach the host heap. An address "
                "up there means the port is not modelling something. Reported once.%s\n",
                storing > 0 ? "a store" : storing == 0 ? "a load" : "an access", who,
                0x80000000u + (uint32_t)off,
                committed ? "" : " The scratch could not be committed, so this access violation "
                                 "stands and the process is about to die of it.");
        /* Only for the thread the registers belong to, and only once the
         * scratch is there to walk through. */
        if (s && guest && committed) {
            fprintf(stderr, "  backtrace from r1:");
            guest_backtrace(s, s->gpr[1]);
        }
    }
    return committed;
}

/* The image every window folds onto. If the reservation or the guard will
 * not take, the whole range is ordinary zeroed memory: nothing the guest can
 * do reaches the host heap either way, there is just nothing to say that it
 * tried. */
static uint8_t* mem_alloc(CpuState* s)
{
    uint8_t* p = (uint8_t*)plat_reserve(MEM_RESERVE_BYTES);
    if (p) {
        g_mem_base = p;
        g_mem_state = s;
        if (plat_commit(p, MEM1_SIZE) && plat_guard_install(p, MEM1_SIZE, MEM_RESERVE_BYTES, mem_fault)) {
            g_mem_guarded = 1;
            return p;
        }
        g_mem_base = NULL;
        plat_release(p, MEM_RESERVE_BYTES);
    }
    fprintf(stderr, "[mem] cannot reserve the guarded MEM1 window (error %lu); running without the "
                    "out-of-range tripwire\n",
            plat_last_error());
    return (uint8_t*)calloc(1, MEM_IMAGE_SIZE);
}

/* SOA_MEMPOKE=addr[,addr...] stores a word at each guest address and reads it
 * back, before the game runs. Nothing in a working run goes near the range the
 * tripwire covers, which would leave the tripwire itself untested until the
 * day it mattered; this is how to fire it on purpose, and a list of addresses
 * past the RAM is how to see that it still only says so once. It happens
 * before the disc is read, so it needs no disc. */
static void mem_poke(CpuState* s)
{
    const char* p = getenv("SOA_MEMPOKE");
    if (!p || !*p) return;
    if (!g_mem_guarded)
        fprintf(stderr, "[mem] SOA_MEMPOKE: no tripwire is armed, so an out-of-range address will be "
                        "silent\n");
    while (*p) {
        char* end;
        uint32_t ea = (uint32_t)strtoul(p, &end, 0);
        if (end == p) break; /* not a number: stop rather than spin on it */
        /* The hardware window is not memory and this runs before dvd_init,
         * threads_init and the first GX state exist: a store to 0xCC008000
         * would enter the write-gather pipe, and one to 0xCC006000 a device
         * model that has not been set up. The switch is here to fire the
         * tripwire, which is about RAM. */
        if (is_mmio(ea))
            fprintf(stderr, "[mem] SOA_MEMPOKE: %08X is in the hardware window (MMIO or the "
                            "write-gather pipe), which is not set up yet; skipped\n", ea);
        else {
            mem_w32(s, ea, 0xDEADBEEFu);
            fprintf(stderr, "[mem] SOA_MEMPOKE: %08X <- DEADBEEF, reads back %08X\n", ea,
                    mem_r32(s, ea));
        }
        p = end + (*end == ',' ? 1 : 0);
    }
}

/* SOA_POKE=frame:addr=value[,frame:addr=value...] stores one 32-bit word into
 * guest memory at the end of the named frame, once, and says what was there
 * before. PLAN D2 asked for this and gave the reason: nothing in runtime/ could
 * write guest memory while the game ran, so every question of the form "what
 * does the game do if this variable says that" needed a recompile to answer.
 *
 * SOA_MEMPOKE above is a different thing and stays: it fires before the disc is
 * read, to test the out-of-range tripwire. This one fires inside the run.
 *
 * Fired on the first frame at or after the target rather than on equality: a
 * frame number can be skipped -- SOA_SNAP skips rasterizing, the game can
 * present nothing across a long load -- and a poke that silently never
 * happened would be read as the game ignoring it, which is the worst possible
 * failure for a switch whose whole purpose is answering that question.
 *
 * Addresses are the game's, so the useful ones are worth naming here. The
 * field's own map identity, verified against the three MEM1 images in
 * build/fifo (all of which say a101b, which is what the trace says loaded):
 *   0x80311AC4  map number, a word          -- 101
 *   0x80311AC8  map letter, top byte        -- 0x62000000 is 'b'
 *   0x80311AEC  field state, a word         -- 8 is the steady per-frame update
 * `/field/a%03d%c.mld` is sprintf'd from the first two (0x801017A8). */
/* Three words warp the field, so the limit is really "how many maps can one
 * run visit": 256 items is 85 of them. */
#define POKE_MAX 256

typedef struct {
    unsigned frame;
    uint32_t ea, value;
    int done;
} Poke;

static Poke g_pokes[POKE_MAX];
static int g_poke_n = -1; /* -1 until SOA_POKE has been read */

/* One number of an item, or 0 if it is not one. strtoul alone read three
 * things nobody typed and armed them without a word: MSVC's unsigned long is
 * 32 bits, so 0x100000000 saturated to FFFFFFFF (and a frame past 2^32 never
 * fired); base 0 read 0101 as octal 65; and it skips spaces and takes a sign,
 * so -1 was FFFFFFFF. */
static int poke_number(const char** p, int base, uint32_t* out)
{
    const char* s = *p;
    char* end;
    unsigned long long v;
    if (*s < '0' || *s > '9') return 0;
    if (base == 0 && s[0] == '0' && s[1] >= '0' && s[1] <= '9') return 0;
    v = strtoull(s, &end, base);
    if (end == s || v > 0xFFFFFFFFull) return 0;
    *out = (uint32_t)v;
    *p = end;
    return 1;
}

static void poke_parse(void)
{
    const char* p = getenv("SOA_POKE");
    g_poke_n = 0;
    if (!p || !*p) return;
    while (*p && g_poke_n < POKE_MAX) {
        const char* item = p; /* a refusal quotes the whole item, not the half of it that failed */
        uint32_t frame, ea, value;
        if (!poke_number(&p, 10, &frame) || *p != ':') { p = item; break; }
        p++;
        if (!poke_number(&p, 0, &ea) || *p != '=') { p = item; break; }
        p++;
        if (!poke_number(&p, 0, &value) || (*p && *p != ',')) { p = item; break; }
        g_pokes[g_poke_n].frame = frame;
        g_pokes[g_poke_n].ea = ea;
        g_pokes[g_poke_n].value = value;
        g_poke_n++;
        p += *p == ',' ? 1 : 0;
    }
    /* Refusing quietly is what a switch must never do: a run driven by a
     * mistyped poke looks exactly like a run whose poke did nothing. */
    if (*p)
        fprintf(stderr, "[poke] SOA_POKE: stopped at %.32s -- each item is frame:addr=value, "
                        "decimal frame, 0x addresses and values accepted, up to %d items; "
                        "%d parsed\n", p, POKE_MAX, g_poke_n);
    else if (g_poke_n)
        fprintf(stderr, "[poke] %d poke(s) armed\n", g_poke_n);
}

/* SOA_PEEK=addr@N[-M][,...] reads a word at the end of frame N, or of every
 * frame from N to M, and prints it with the game's retrace count -- the read
 * half of SOA_POKE without having to write the value back. SOA_WATCH cannot
 * serve here: it stops after 201 hits and says which store, not which frame, so
 * "how many fields does this fade take" had no answer without a tracepoint and
 * a retranslation. Peeks fire before pokes in the same hook, so a peek and a
 * poke of one word at one frame read what the game wrote. Its own list, not a
 * share of SOA_POKE's 256. */
#define PEEK_MAX 256
#define VI_RETRACE_COUNT 0x80347A64u /* the VI library's retraceCount */

typedef struct {
    uint32_t ea;
    unsigned first, last, done;
} Peek;

static Peek g_peeks[PEEK_MAX];
static int g_peek_n = -1;

static void peek_parse(void)
{
    const char* p = getenv("SOA_PEEK");
    g_peek_n = 0;
    if (!p || !*p) return;
    while (*p && g_peek_n < PEEK_MAX) {
        const char* item = p;
        uint32_t ea, first, last;
        if (!poke_number(&p, 0, &ea) || *p != '@') { p = item; break; }
        p++;
        if (!poke_number(&p, 10, &first)) { p = item; break; }
        last = first;
        if (*p == '-') {
            p++;
            if (!poke_number(&p, 10, &last) || last < first) { p = item; break; }
        }
        if (*p && *p != ',') { p = item; break; }
        g_peeks[g_peek_n].ea = ea;
        g_peeks[g_peek_n].first = first;
        g_peeks[g_peek_n].last = last;
        g_peeks[g_peek_n].done = 0;
        g_peek_n++;
        p += *p == ',' ? 1 : 0;
    }
    if (*p)
        fprintf(stderr, "[peek] SOA_PEEK: stopped at %.32s -- each item is addr@frame or addr@first-last, "
                        "up to %d items; %d parsed\n", p, PEEK_MAX, g_peek_n);
    else if (g_peek_n)
        fprintf(stderr, "[peek] %d peek(s) armed\n", g_peek_n);
}

static void peek_at_frame(CpuState* s, unsigned frame)
{
    int i;
    if (g_peek_n < 0) peek_parse();
    for (i = 0; i < g_peek_n; i++) {
        Peek* k = &g_peeks[i];
        /* A single frame fires once on the first frame at or after it, as a
         * poke does; a range fires on every frame inside it that is presented. */
        if (k->done || frame < k->first) continue;
        if (k->first == k->last) k->done = 1;
        else if (frame > k->last) { k->done = 1; continue; }
        if (is_mmio(k->ea)) {
            fprintf(stderr, "[peek] frame %u: %08X is in the hardware window, not memory; skipped\n", frame, k->ea);
            k->done = 1;
            continue;
        }
        gxr_hook_hazard(k->ea, 4); /* the frame may still be copying there (H14) */
        fprintf(stderr, "[peek] frame %u: %08X = %08X (retrace %u)\n", frame, k->ea, mem_r32(s, k->ea),
                mem_r32(s, VI_RETRACE_COUNT));
    }
}

/* SOA_UNCAP=N lets the frame end's spin go after one field instead of two:
 * tick.c's VIGetRetraceCount answers the frame's start plus one at the spin's
 * call site (the cap is the immediate `cmpli r0,1` at 0x801DC4A4). The logic
 * runs once per frame, so this runs the whole game up to twice as fast
 * (FINDINGS "H2") -- a measurement, or a battle speed-up, never 60 fps. H3
 * first did it by zeroing the stored start (0x8034768C) every frame; M2's
 * answer at the call site leaves that word as the game wrote it.
 *
 * SOA_UNCAP=N starts at frame N, and 1 is from the start. A start frame is
 * what makes it usable at all: a disc load runs on the wall clock, so uncapped
 * from boot, a pad script's START at 1600 reaches the title at some other
 * frame -- H2's fade arrived 41 frames late. 0 or unset is off.
 *
 * The [frametime] record restarts at the same frame, so the percentiles are
 * the uncapped stretch's. SOA_FRAMETIME_FROM=N restarts it at N without
 * uncapping, which is how a capped run measures the same stretch. */
void tick_unlock_from(unsigned frame); /* tick.c */
static uint32_t g_uncap_from;     /* 0 = off */
static uint32_t g_frametime_from; /* 0 = the uncap's frame, or the whole run */
static int g_uncap_read;

static uint32_t frame_switch(const char* name, const char* consequence)
{
    const char* u = getenv(name);
    const char* p = u;
    uint32_t v;
    if (!u || !*u) return 0;
    if (!poke_number(&p, 10, &v) || *p) {
        fprintf(stderr, "[uncap] %s=%s is not a frame number; %s\n", name, u, consequence);
        return 0;
    }
    return v;
}

static void uncap_parse(void)
{
    g_uncap_read = 1;
    g_uncap_from = frame_switch("SOA_UNCAP", "the cap stays on");
    g_frametime_from = frame_switch("SOA_FRAMETIME_FROM", "[frametime] covers the whole run");
    if (g_uncap_from)
        fprintf(stderr, "[uncap] from frame %u, the frame end's spin is let go after one field, not two -- "
                        "the game runs up to twice as fast; not 60 fps\n", g_uncap_from);
    if (!g_frametime_from) g_frametime_from = g_uncap_from;
    tick_unlock_from(g_uncap_from);
}

void hle_frame_mark(void);
void hle_frametime_restart(unsigned frame);
void hle_on_report(void (*fn)(void));
void si_set_config_extra(const char* extra);

void poke_at_frame(CpuState* s, unsigned frame);
#ifdef SOA_HOST
void host_frame_end(unsigned frame);
#endif

/* SOA_STALL=<frame>:<seconds> (M19's check): the guest thread sleeps once, at
 * that frame's end, the way a host that went to sleep would stop it. The
 * clock is to count none of it. */
static void stall_at_frame(unsigned frame)
{
    static int parsed, done;
    static unsigned at, secs;
    if (!parsed) {
        const char* v = getenv("SOA_STALL");
        parsed = 1;
        if (v && *v && sscanf(v, "%u:%u", &at, &secs) != 2) {
            fprintf(stderr, "[boot] SOA_STALL=%s is not frame:seconds; ignored\n", v);
            at = 0;
        }
    }
    if (done || !at || frame < at) return;
    done = 1;
    fprintf(stderr, "[boot] frame %u: stalling the guest thread %u s (SOA_STALL)\n", frame, secs);
    plat_sleep_ms(secs * 1000u);
}

/* SOA_SKIP_TO=frame[@speed]: get to a frame fast and then carry on as
 * usual. Until the game presents that frame its clock runs `speed` times
 * the wall clock's (10 unless given) and nothing is rasterized; from there
 * the speed is SOA_SPEED's again and every frame is drawn. For a scene a
 * few minutes into a scripted run, under a window or a host that would
 * otherwise have to sit through them: the frames, and what SOA_PAD presses
 * at each, are the same ones. */
static unsigned g_skip_to, g_skip_back = 1;
static char g_skip_speed_was[16];
void clock_set_speed_at(unsigned speed, uint64_t host_ns);
uint64_t clock_host_ns(void);
unsigned hle_speed(void);

static void skip_parse(void)
{
    const char* p = getenv("SOA_SKIP_TO");
    const char* was = getenv("SOA_SPEED");
    char speed[16];
    char* end;
    unsigned long frame, fast = 10;
    if (!p || !*p) return;
    frame = strtoul(p, &end, 10);
    if (end != p && *end == '@') {
        const char* q = end + 1;
        fast = strtoul(q, &end, 10);
        if (end == q) fast = 0;
    }
    if (end == p || *end || !frame || !fast) {
        fprintf(stderr, "[skip] SOA_SKIP_TO=%s is not frame or frame@speed; nothing is skipped\n", p);
        return;
    }
    g_skip_to = (unsigned)frame;
    g_skip_back = hle_speed();
    snprintf(g_skip_speed_was, sizeof g_skip_speed_was, "%s", was ? was : "");
    snprintf(speed, sizeof speed, "%lu", fast);
    plat_setenv("SOA_SPEED", speed); /* the clock takes its speed from here when the guest first reads it */
    gxr_skip_until(g_skip_to);
    fprintf(stderr, "[skip] to frame %u at %lu times real speed, drawing nothing on the way\n", g_skip_to, fast);
}

static void skip_at_frame(unsigned frame)
{
    if (!g_skip_to || frame < g_skip_to) return;
    plat_setenv("SOA_SPEED", g_skip_speed_was);
    clock_set_speed_at(g_skip_back, clock_host_ns());
    fprintf(stderr, "[skip] frame %u: at %u times real speed from here, and drawing\n", frame, g_skip_back);
    g_skip_to = 0;
}

void poke_at_frame(CpuState* s, unsigned frame)
{
    int i;
    hle_frame_mark();
#ifdef SOA_HOST
    host_frame_end(frame); /* host.c: the frame's models are whole, and its to publish */
#endif
    skip_at_frame(frame);
    stall_at_frame(frame);
    peek_at_frame(s, frame);
    if (g_poke_n < 0) poke_parse();
    for (i = 0; i < g_poke_n; i++) {
        if (g_pokes[i].done || frame < g_pokes[i].frame) continue;
        g_pokes[i].done = 1;
        if (is_mmio(g_pokes[i].ea)) {
            fprintf(stderr, "[poke] frame %u: %08X is in the hardware window, not memory; skipped\n",
                    frame, g_pokes[i].ea);
            continue;
        }
        gxr_hook_hazard(g_pokes[i].ea, 4); /* the frame may still be copying there (H14) */
        fprintf(stderr, "[poke] frame %u: %08X <- %08X (was %08X)\n", frame, g_pokes[i].ea,
                g_pokes[i].value, mem_r32(s, g_pokes[i].ea));
        mem_w32(s, g_pokes[i].ea, g_pokes[i].value);
    }
    /* The mods after the pokes and the peeks before both, so a peek reads
     * what the game wrote and a mod has the last word (PLAN M1). */
    mod_frame(s, frame);
    if (!g_uncap_read) uncap_parse();
    if (g_frametime_from && frame >= g_frametime_from) {
        static int started;
        if (!started) {
            started = 1;
            hle_frametime_restart(frame);
        }
    }
}

#define ARENA_HI 0x81700000u
#define GEKKO_PVR 0x00083214u

static void w32(uint8_t* mem, uint32_t ea, uint32_t v)
{
    v = BSWAP32(v);
    memcpy(mem + (ea & MEM_MASK), &v, 4);
}
static uint32_t be32(const uint8_t* p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return BSWAP32(v);
}

/* DOL header: 7 text + 11 data sections, then BSS and the entry point. */
static int load_dol(uint8_t* mem, const uint8_t* dol, size_t size)
{
    int i;
    if (size < 0x100) { /* the header itself is read below */
        fprintf(stderr, "main.dol is %zu bytes, too short to hold a DOL header\n", size);
        return 0;
    }
    for (i = 0; i < 18; i++) {
        uint32_t off = be32(dol + i * 4);
        uint32_t addr = be32(dol + 0x48 + i * 4);
        uint32_t len = be32(dol + 0x90 + i * 4);
        uint32_t dest = addr & MEM_MASK;
        if (!len) continue;
        /* Offset, address and length all come out of the file and all three
         * are 32-bit, so every bound here is written as a difference: as a
         * sum, a corrupt or crafted header wraps it and the memcpy below
         * copies gigabytes out of a small buffer into the image. */
        if (off > size || len > size - off || dest >= MEM1_SIZE || len > MEM1_SIZE - dest) {
            fprintf(stderr, "DOL section %d out of range\n", i);
            return 0;
        }
        memcpy(mem + dest, dol + off, len);
    }
    /* BSS is already zero: the image comes back zeroed, however it was got. */
    return 1;
}

/* What the boot ROM and apploader leave in the OS low-memory area. Values
 * follow the GameCube's documented layout; the OS reads several of them
 * during OSInit. */
static void setup_low_memory(uint8_t* mem, const uint8_t* boot, uint32_t fst_addr, uint32_t fst_max)
{
    memcpy(mem, boot, 0x20); /* game ID, maker, disc, version, streaming, magic */
    w32(mem, 0x80000020u, 0x0D15EA5Eu); /* standard boot code magic */
    w32(mem, 0x80000024u, 0x00000001u); /* version */
    w32(mem, 0x80000028u, MEM1_SIZE);   /* physical memory size */
    w32(mem, 0x8000002Cu, 0x00000003u); /* console type: retail production board */
    w32(mem, 0x80000030u, 0x00000000u); /* arena lo: let the OS derive it */
    w32(mem, 0x80000034u, fst_addr);    /* arena hi: the FST sits just above it */
    w32(mem, 0x80000038u, fst_addr);    /* FST location */
    w32(mem, 0x8000003Cu, fst_max);     /* FST max size */
    w32(mem, 0x800000CCu, 0x00000000u); /* video mode: NTSC */
    w32(mem, 0x800000D0u, MEM1_SIZE);   /* simulated memory size (mirror) */
    w32(mem, 0x800000F0u, MEM1_SIZE);   /* simulated memory size */
    w32(mem, 0x800000F8u, 0x09A7EC80u); /* bus clock, 162 MHz */
    w32(mem, 0x800000FCu, 0x1CF7C580u); /* CPU clock, 486 MHz */
}

/* snprintf returns the length it wanted, not the length it wrote, so clamp
 * before using the total as an offset again. */
#define STOP_ADD(buf, n, ...)                                            \
    do {                                                                 \
        (n) += snprintf((buf) + (n), sizeof(buf) - (size_t)(n), __VA_ARGS__); \
        if ((n) > (int)sizeof(buf) - 1) (n) = (int)sizeof(buf) - 1;      \
    } while (0)

/* One line before the guest starts. Which mode the port is in, what will end
 * the run and how to drive it are exactly the three things the switch names
 * used to answer wrongly, so say them outright. The keys are window.c's
 * mapping; keep the two in step. */
static void print_mode(int windowed, int rendering, int scripted, unsigned frames, unsigned snap,
                       unsigned watchdog_secs)
{
    char stop[256], snaps[512];
    const char* fdir = getenv("SOA_FRAMES_DIR"); /* gxr.c reads the same switch */
    int n = 0;
    stop[0] = '\0';
    snaps[0] = '\0';
    /* A snapshot needs something to snapshot: without SOA_RENDER the EFB copy
     * hook never reaches the renderer, so no PNG is ever written. Say that
     * rather than promise files that will not appear. */
    if (snap && rendering)
        snprintf(snaps, sizeof snaps, ", a snapshot to %s every %u frames", fdir && *fdir ? fdir : "build/frames",
                 snap);
    if (frames) STOP_ADD(stop, n, "stopping after %u frames", frames);
#ifdef SOA_HOST
    if (windowed) STOP_ADD(stop, n, "%sthe host ends it", n ? ", " : "");
#else
    if (windowed) STOP_ADD(stop, n, "%sEscape or closing the window quits", n ? ", " : "");
#endif
    if (watchdog_secs)
        STOP_ADD(stop, n, "%swatchdog if no frame for %us (SOA_WATCHDOG=0 disables it)", n ? ", " : "",
                 watchdog_secs);
    if (!n) snprintf(stop, sizeof stop, "nothing will stop it -- Ctrl-C to quit");
#ifdef SOA_HOST
    if (windowed) /* host.c: the program that loaded the runtime shows the frames and gives the input */
        fprintf(stderr, "[run] hosted%s%s; %s\n",
                rendering ? "" : " (blank until SOA_RENDER=1: nothing is drawn without it)", snaps, stop);
#else
    if (windowed)
        fprintf(stderr, "[run] window%s%s; %s; keys X=A Z=B C=X V=Y, Enter or Space=START, Q=L E=R R=Z, "
                        "T/F/G/H=D-pad up/left/down/right, arrows or WASD=stick, IJKL=C-stick\n",
                rendering ? "" : " (blank until SOA_RENDER=1: nothing is drawn without it)", snaps, stop);
#endif
    else if (snap && rendering)
        fprintf(stderr, "[run] headless%s; %s\n", snaps, stop);
    else if (snap)
        fprintf(stderr, "[run] headless; SOA_SNAP is set but SOA_RENDER is not, so nothing is drawn and "
                        "no snapshot is written; %s\n", stop);
    else if (rendering)
        fprintf(stderr, "[run] headless, rendering with nowhere to put it -- SOA_WINDOW=1 for a window, "
                        "SOA_SNAP=n for PNGs in build/frames; %s\n", stop);
    else
        fprintf(stderr, "[run] headless; %s\n", stop);
    if (scripted)
        fprintf(stderr, "[run] SOA_PAD drives the controller%s\n",
                windowed ? "; the keyboard and gamepad add to it" : "");
}

static void usage(void)
{
    fprintf(stderr,
            "soa.exe [extracted-dir]             run the game (default directory: extracted)\n"
            "soa.exe --check-disc [store]        hash every block of a disc store and say if it is whole\n"
            "soa.exe --replay build/fifo/0000    render one captured frame to <base>.png\n"
            "soa.exe --replay A B                render consecutive captures A and B, and the image\n"
            "                                    between them to B.mid.png (SOA_PAIR_T, SOA_PAIR_LIST)\n"
            "\n"
            "Environment (PowerShell: $env:SOA_RENDER='1'):\n"
            "  SOA_RENDER=1     draw the game; a window opens unless SOA_SNAP or SOA_PAD is set\n"
            "  SOA_WINDOW=0|1   force the window off or on\n"
            "  SOA_FRAMES=n     run n video frames (numbered 0..n-1), then stop and print the report\n"
            "  SOA_SNAP=n       write build/frames/NNNN.png every n frames; needs SOA_RENDER=1\n"
            "  SOA_SKIP_TO=n    get to frame n fast, drawing nothing on the way, then carry on (n@speed; 10)\n"
            "  SOA_WATCHDOG=s   report and stop after s seconds with no frame (default 20 headless,\n"
            "                   off when a window is open; 0 disables it)\n"
            "  SOA_MMIO=1       log the first few accesses of every hardware register\n"
            "  SOA_PROFILE=n    0 turns off the end-of-run sampling profile; n>1 shows n rows\n"
            "  SOA_PAD=f:btns   scripted controller, e.g. 1700:start (implies no window)\n"
            "  SOA_MEMPOKE=a,b  store a word at each guest address before boot; an address past the\n"
            "                   console's 24 MB, e.g. 0x81800000, fires the MEM1 tripwire\n"
            "The rest of the switches, and the keyboard mapping, are in README.md.\n");
}

/* P5a: with any picture key set, `--replay <base>` also writes
 * <base>.picture.png -- the frame through the filters and the scaler, as a
 * window would show it, at SOA_PICTURE_SIZE (default 1920x1080) -- so every
 * filter can be opened without a window. Never <base>.png, which is the
 * frame the hashes pin. */
static int replay_picture(const char* base)
{
    PicFilters pf;
    PicFilterState* st;
    char why[160], path[1024];
    const char* size = getenv("SOA_PICTURE_SIZE");
    const char* sc = getenv("SOA_SCALER");
    const char* keys[4] = {getenv("SOA_GAMMA"), getenv("SOA_COLORBLIND"), getenv("SOA_COLORBLIND_MODE"), getenv("SOA_FLASH_LIMIT")};
    int w, h, x, y, k, used = 0, n = 0, dw = 1920, dh = 1080, rc = 1;
    const uint8_t* rgba = gxr_screen(&w, &h);
    uint8_t *frame, *out;
    size_t i;
    /* any key set, at any value: identity values make the picture the
     * frame, which is how the picture path itself is checked */
    for (k = 0; k < 4; k++) used |= keys[k] && *keys[k];
    if (!used) return 0;
    if (!picture_filters_parse(&pf, keys[0], keys[1], keys[2], keys[3], why, sizeof why)) fprintf(stderr, "[picture] %s\n", why);
    if (!rgba || w < 1 || h < 1) return 1;
    if (size && *size && (sscanf(size, "%dx%d%n", &dw, &dh, &n) != 2 || size[n] || dw < 1 || dh < 1 || dw > 16384 || dh > 16384)) {
        fprintf(stderr, "[picture] SOA_PICTURE_SIZE=%s is not WxH; no picture written\n", size);
        return 1;
    }
    if (sc && *sc && strcmp(sc, "fit") && strcmp(sc, "integer"))
        fprintf(stderr, "[picture] SOA_SCALER=%s is not integer or fit; integer\n", sc);
    frame = (uint8_t*)malloc((size_t)w * h * 4);
    out = (uint8_t*)malloc((size_t)dw * dh * 4);
    st = picture_filters_new(&pf);
    if (frame && out && st) {
        for (y = 0; y < h; y++) /* RGBA at the EFB's stride to BGRA, as the window presents it */
            for (x = 0; x < w; x++) {
                const uint8_t* s = rgba + ((size_t)y * EFB_W + x) * 4;
                uint8_t* d = frame + ((size_t)y * w + x) * 4;
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255;
            }
        picture_filter(st, frame, w, h, 0.0);
        picture_scale(frame, w, h, out, dw, dh, sc && !strcmp(sc, "fit") ? PICTURE_FIT : PICTURE_INTEGER);
        for (i = 0; i < (size_t)dw * dh; i++) { /* and back, with the bars opaque */
            uint8_t t = out[4 * i];
            out[4 * i] = out[4 * i + 2];
            out[4 * i + 2] = t;
            out[4 * i + 3] = 255;
        }
        picture_filters_name(&pf, why, sizeof why);
        snprintf(path, sizeof path, "%s.picture.png", base);
        rc = !png_write_rgba(path, out, dw, dh, dw * 4);
        if (!rc) fprintf(stderr, "[picture] wrote %s (%dx%d; %s)\n", path, dw, dh, why);
        else fprintf(stderr, "[picture] cannot write %s\n", path);
    } else {
        fprintf(stderr, "[picture] out of memory for a %dx%d picture; none written\n", dw, dh);
    }
    picture_filters_free(st);
    free(frame);
    free(out);
    return rc;
}

/* SOA_GPU=vulkan (soa.ini gpu): every command the renderer builds is drawn by
 * gxv, the Vulkan backend, on the producer as it is built (specs/gpu-backend.md
 * V5), and the screen copy read back is the picture. Anything short of a
 * device is one "[gxv] fallback:" line and the CPU renderer, which is also
 * the default. Before the renderer's first command. */
static void gpu_start(void)
{
    const char* g = getenv("SOA_GPU");
    char why[512];
    if (!g || !*g || !strcmp(g, "off")) return;
    if (strcmp(g, "vulkan") != 0) {
        fprintf(stderr, "[gxv] fallback: SOA_GPU=%s is neither off nor vulkan; the CPU draws\n", g);
        return;
    }
    if (!gxr_enabled()) {
        fprintf(stderr, "[gxv] SOA_GPU=vulkan but nothing is drawn (SOA_RENDER is not set); the GPU is not started\n");
        return;
    }
    gxv_set_root(settings_root()); /* the pipeline cache under the root, not the working directory */
    if (!gxv_start(why, sizeof why)) fprintf(stderr, "[gxv] fallback: %s; the CPU draws\n", why);
}

#ifdef SOA_SPLIT
/* A split build's launcher loads the game library first, then calls this
 * (runtime/game.c's soa_run; specs/android.md 3.2). */
#define main soa_main
#endif
int main(int argc, char** argv)
{
    const char* dir = argc > 1 && argv[1][0] != '-' ? argv[1] : "extracted";
    const uint8_t *dol, *boot, *fst;
    size_t dol_size, fst_size;
    uint32_t fst_addr, fst_max;
    int built_in;
    static CpuState s;

    /* An option we do not know is a typo, not a directory: saying so beats
     * booting the game as though nothing had been asked for. */
    if (argc > 1 && argv[1][0] == '-') {
        int replay = strcmp(argv[1], "--replay") == 0;
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "-?") == 0) {
            usage();
            return 0;
        }
        if (strcmp(argv[1], "--check-disc") == 0) {
            /* a store just copied to a Deck or a phone, checked whole before
             * a run depends on it (disc-layer I5): exit 0, or 9 */
            const char* disc = argc > 2 ? argv[2] : NULL;
            if (!disc && !getenv("SOA_SELFTEST")) disc = settings_load();
            return disc_check_store(disc ? disc : "extracted");
        }
        if (replay && argc > 4) {
            fprintf(stderr, "--replay takes one capture, or two consecutive ones\n");
            usage();
            return 1;
        }
        if (!replay || argc < 3) {
            fprintf(stderr, "%s: %s\n", argv[1],
                    replay ? "--replay needs the base path of a captured frame" : "unknown option");
            usage();
            return 1;
        }
    }

    /* Before anything else this process does, so that "wall seconds" in the
     * report means the run and not the part of it that asked first. */
    hle_clock_start();
    /* The player's soa.ini beside the exe (settings.c, PLAN M5), before any
     * switch is read: it fills in only what the environment has not set, and
     * names the disc when the command line does not. Never for the self test;
     * the scripted checks turn it off with SOA_SETTINGS=0. */
    if (!getenv("SOA_SELFTEST")) {
        const char* disc;
        char log[1200];
        /* Started from Explorer or a front end, the console is its own: send
         * the log to a file under the root and let the console go (M5b). */
        if (settings_console_to_log(log, sizeof log))
            fprintf(stderr, "[boot] this run's log is %s (started without a terminal)\n", log);
        disc = settings_load();
        if (disc && !(argc > 1 && argv[1][0] != '-')) dir = disc;
        si_set_path_root(settings_root());
    }
    s.mem = mem_alloc(&s);
    if (!s.mem) { fprintf(stderr, "cannot allocate MEM1\n"); return 1; }
    mem_poke(&s);
    /* Read SOA_POKE here rather than at the first frame that needs it, so a
     * mistyped item is refused while the person who typed it is still looking,
     * instead of sixteen thousand frames later in a run that appears to have
     * ignored them. The pokes themselves still fire at their own frames. */
    poke_parse();
    peek_parse();
    uncap_parse();
    skip_parse();
    watch_init(); /* here with the others, so SOA_WATCH is read before the disc is */
    gxr_export_install(); /* SOA_GXR_EXPORT (gxr_export.h) */
    gx_set_frame_hook(poke_at_frame);

    {
        /* The player's image, a directory holding disc.iso or the .iso
         * itself, checked to be this port's disc before anything of it is
         * believed (disc-layer I1). Here, after the switches above, so their
         * diagnostics print even with no disc; without one, nothing would
         * load but zeros, so the run stops and says how to make one. A build
         * carrying its own executable, boot.bin and file table (I3) opens
         * none for the self test or a replay, which read nothing else of
         * the disc; for a game run it opens one and refuses an image whose
         * file table is not the build's. */
        char why[1024];
        int no_disc = getenv("SOA_SELFTEST") || (argc > 2 && strcmp(argv[1], "--replay") == 0);
        disc_set_stop_hook(hle_report); /* a damaged store stops the run with its report (I5) */
        built_in = disc_builtin(why, sizeof why);
        if (built_in < 0 || (!(built_in && no_disc) && disc_open(dir, why, sizeof why) != 0) ||
            disc_system(&dol, &dol_size, &boot, &fst, &fst_size, why, sizeof why) != 0) {
            fprintf(stderr, "[boot] %s\n", why);
            return 1;
        }
    }

    if (!load_dol(s.mem, dol, dol_size)) return 1;

    /* The apploader parks the FST at the top of memory, 32-byte aligned, and
     * ends the arena where it starts. Both files are bounded before they are
     * believed: disc.c hands back the whole 0x440-byte header, and an FST
     * larger than the arena would make the subtraction below underflow into
     * an arbitrary destination offset -- this is the one write into the image
     * here that is not a device model's, so it carries its own bound. */
    if (fst_size == 0 || fst_size > ARENA_HI - 0x80000000u) {
        fprintf(stderr, "[boot] the disc's file table is %zu bytes, which does not fit under the arena at "
                        "%08X\n", fst_size, ARENA_HI);
        return 1;
    }
    fst_max = be32(boot + 0x42C);
    fst_addr = (ARENA_HI - (uint32_t)fst_size) & ~31u;
    memcpy(s.mem + (fst_addr & MEM_MASK), fst, fst_size);
    setup_low_memory(s.mem, boot, fst_addr, fst_max);

    /* Mods check themselves against the DOL, and load once memory holds what
     * the game starts from -- after the low-memory block, which would
     * otherwise overwrite what a mod's init wrote and hand its reads zeros
     * (the review of 2026-09-25). A recording then names them, and the report
     * says what each applied. */
    mod_note_dol(dol, dol_size); /* for call_guest's checks, the self test's among them */
    {
        /* What else a recording depends on: the settings that change the
         * game (seed.c's, and each later one marked recorded in settings.c),
         * then the mods. A run with neither keeps the line it always had. */
        static char extra[640];
        const char* mods = getenv("SOA_MODS");
        int modded = mods && *mods && mod_load(&s, mods, dol, dol_size);
        size_t n;
        char seed[16];
        const char* rumble = getenv("SOA_RUMBLE");
        /* The rumble motor (M18): how hard, and stopped on every way out --
         * the report covers each path through hle_report, atexit the exit()s
         * that skip it; window.c stops it on focus loss and close. */
        si_set_motor_strength(rumble && *rumble ? atoi(rumble) : 100);
        si_set_chord_handler(on_chord);
        tick_set_hold(clock_pause_requested);
        si_set_pad2_source(window_pad2); /* P10a: port 2 for mods */
        mod_set_pad_reader(si_read_pad);
        mod_set_host_buttons(si_host_buttons);
        hle_on_report(si_motor_stop);
        atexit(si_motor_stop);
        if (seed_init(seed, sizeof seed)) hle_on_report(seed_report);
        settings_record_as("seed", seed);
        settings_check_mods(mod_loaded);
        settings_recorded(extra, sizeof extra);
        n = strlen(extra);
        if (modded) {
            int k = snprintf(extra + n, sizeof extra - n, "%s%s", n ? " " : "", mod_describe());
            if (k < 0 || (size_t)k >= sizeof extra - n)
                fprintf(stderr, "[pad] the config line was cut at %zu bytes, in the mod list\n", sizeof extra - 1);
            hle_on_report(mod_report);
        }
        if (extra[0]) si_set_config_extra(extra);
    }

    s.spr[287] = GEKKO_PVR;
    s.msr = 0x00002030u; /* FP | IR | DR -- __init_hardware rewrites it anyway */

    /* One of the two always, so a build that lost its embed cannot pass
     * for one that has it (I3). */
    if (built_in) {
        char sha[41];
        sha1_hex(dol, dol_size, sha);
        fprintf(stderr, "[boot] system files built in (DOL sha1 %s)\n", sha);
    } else {
        fprintf(stderr, "[boot] system files from the image (built without them)\n");
    }
    fprintf(stderr, "[boot] DOL %zu bytes, FST %zu bytes at %08X; entering %s\n", dol_size,
            fst_size, fst_addr, STR(ENTRY_FN));
    /* Which compiler built this binary (L3b): a replay or a run from another
     * toolchain profile says so in its first lines, so a log can never be
     * mistaken for the reference build's. */
#if defined(__clang__)
    fprintf(stderr, "[boot] built with clang %s\n", __clang_version__);
#elif defined(_MSC_VER)
    fprintf(stderr, "[boot] built with MSVC %d\n", _MSC_FULL_VER);
#elif defined(__GNUC__)
    fprintf(stderr, "[boot] built with gcc %s\n", __VERSION__);
#endif
    threads_init(&s);
    if (getenv("SOA_SELFTEST")) return selftest(&s) ? 7 : 0;
    if (argc > 2 && strcmp(argv[1], "--replay") == 0) {
        /* Render one captured frame (see gx.c frame capture) to <base>.png,
         * or a consecutive pair and the image between them (H10). */
        char png[1024];
        int rc;
        snprintf(png, sizeof png, "%s.png", argv[2]);
        gxr_enable(1);
        gxr_set_output(png);
        gpu_start();
        if (argc > 3) return gx_replay_pair(&s, argv[2], argv[3]);
        rc = gx_replay(&s, argv[2]);
        if (rc == 0 && replay_picture(argv[2])) rc = 1; /* no picture of a capture that did not load */
        return rc;
    }
    {
        /* A window when rendering for a person: SOA_RENDER is set and neither
         * of the two switches that mean nobody is watching -- SOA_SNAP, which
         * writes frames to disk, and SOA_PAD, which drives the controller from
         * a script the live keyboard would otherwise override. SOA_WINDOW
         * forces it either way. SOA_FRAMES has no bearing here -- it only says
         * when to stop. */
        const char* r = getenv("SOA_RENDER");
        const char* w = getenv("SOA_WINDOW");
        const char* f = getenv("SOA_FRAMES");
        const char* snapenv = getenv("SOA_SNAP");
        const char* pad = getenv("SOA_PAD");
        unsigned frames = f ? (unsigned)atoi(f) : 0u;
        unsigned snap = snapenv ? (unsigned)atoi(snapenv) : 0u;
        int rendering = r && atoi(r);
        int scripted = pad && *pad;
        int want = rendering && !snap && !scripted;
        unsigned secs;
        if (w) want = atoi(w) != 0;
#if !defined(_WIN32) && !defined(SOA_SDL) && !defined(SOA_HOST)
        want = 0; /* window.c is stubs off Windows without SDL; do not promise a window or a quit key */
#endif
        gx_set_frame_limit(frames);
        /* Skipping the frames between snapshots is a headless speed-up. A
         * window asked for alongside them wants every frame drawn, or it
         * shows the clear colour all but one frame in N. */
        if (want && snap) gxr_draw_every_frame();
        /* The watchdog has to know about the window, so decide the window
         * first; window_open() cannot answer yet, the UI thread has not run. */
        secs = start_watchdog(&s, want);
        profile_start(&s);
        print_mode(want, rendering, scripted, frames, snap, secs);
        /* The GPU first: with it running, the window presents from it (V8). */
        gpu_start();
        if (want) window_start(); /* after the line above: the UI thread prints from its own thread */
    }
    /* The ARAM census reads the head of every file on the disc, which took
     * 0.3-1.1 s at the first ARAM DMA under load; here, before the game's
     * first instruction, it is not time the game's clock sees. */
    aram_census_prepare();
    plat_run_on_big_stack(run_guest, &s, GUEST_STACK_BYTES);

    fprintf(stderr, "[boot] entry point returned\n");
    hle_report();
    return 0;
}
