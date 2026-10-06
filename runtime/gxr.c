/*
 * Software GX: vertex decode, transform unit (matrices, lighting, texgen),
 * clipping, rasterization, depth/blend into the embedded framebuffer, and
 * EFB copies (to textures in memory, or to the "screen" as a PNG).
 *
 * Correctness first, speed later: every pixel runs the full TEV. The point
 * is a frame we can look at, produced from the exact command stream the
 * game emits, so every deviation from the real console is ours to find.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "gxr.h"
#include "gxr_cmd.h"
#include "gxr_export.h"
#include "crmath.h"
#include "plat.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

/* A backend that draws the commands instead of the worker pool (V2), or
 * NULL. Set before the first command; with one, no worker starts. */
static const GxrBackend* g_backend;

uint64_t g_gxr_ticks[T_COUNT];
uint64_t g_gxr_phase_last;
int g_gxr_phase = T_HOST;
int g_gxr_tsc = -1;

static double qpc_hz(void)
{
    static double freq;
    if (freq == 0.0) freq = plat_mono_hz();
    return freq;
}

uint64_t gxr_qpc(void)
{
    return plat_mono_raw();
}

double gxr_clock(void)
{
    double hz = qpc_hz();
    return hz > 0.0 ? (double)gxr_qpc() / hz : 0.0;
}

/* Both clocks are read together at the first phase boundary and again at the
 * report, and the tick rate is the ratio of the two spans -- this run's own
 * rate over this run's own duration, rather than a nominal frequency that a
 * power state can make a lie. Four extra clock reads in a whole run. */
static uint64_t g_cal_tick0, g_cal_qpc0, g_cal_span;
static double g_ticks_hz, g_cal_seconds;

void gxr_timing_init(void)
{
    if (g_gxr_tsc >= 0) return;
    {
        const char* env = getenv("SOA_TSC");
        int ok = plat_cycles_invariant(); /* an invariant TSC, or the ARM64 generic timer */
        if (env && !atoi(env)) ok = 0;    /* SOA_TSC=0: measure with gxr_qpc */
        g_gxr_tsc = ok;
    }
    g_cal_qpc0 = gxr_qpc();
    g_cal_tick0 = gxr_ticks();
    g_gxr_phase_last = g_cal_tick0;
}

void gxr_timing_finish(void)
{
    uint64_t qpc1, tick1;
    double hz = qpc_hz();
    if (g_ticks_hz > 0.0 || g_gxr_tsc < 0) return; /* already fixed, or nothing was ever timed */
    qpc1 = gxr_qpc();
    tick1 = gxr_ticks();
    g_cal_span = tick1 - g_cal_tick0;
    g_cal_seconds = hz > 0.0 ? (double)(qpc1 - g_cal_qpc0) / hz : 0.0;
    if (!g_gxr_tsc) g_ticks_hz = hz;
    else if (g_cal_seconds > 0.001 && g_cal_span) g_ticks_hz = (double)g_cal_span / g_cal_seconds;
}

double gxr_seconds(uint64_t ticks) { return g_ticks_hz > 0.0 ? (double)ticks / g_ticks_hz : 0.0; }

/* The span the buckets partition: the producer's wall time from the first
 * phase boundary to the report, which is not the process's -- nothing is
 * timed until the first draw. */
double gxr_producer_span(void) { return g_cal_seconds; }

uint8_t g_efb[EFB_H][EFB_W][4];
uint32_t g_efb_z[EFB_H][EFB_W];

static int g_enabled = -1;
static unsigned g_snap_every; /* SOA_SNAP=n; frames are numbered by gx_frame_count() so the PNG names agree with SOA_FRAMES and SOA_PAD */
static unsigned g_snap_from, g_snap_to = ~0u; /* SOA_SNAP=n@A-B: only frames A to B */

/* Whether SOA_SNAP writes this frame: every nth, inside its range. */
static int snap_frame(unsigned frame)
{
    return g_snap_every && frame >= g_snap_from && frame <= g_snap_to && (frame % g_snap_every) == 0;
}
static int g_draw_every;      /* a window is open: draw every frame, snapshots or not */
static char g_png_path[512];
static uint64_t g_tris, g_lines, g_points, g_clipped, g_verts_bad;
static uint64_t g_copies_tex, g_copies_xfb, g_rej_bary;

/* Rasterization is row-parallel: worker threads each take every Nth row of
 * every triangle in a draw, and the main thread is participant 0. Counters
 * touched per pixel are per thread. */
#define MAX_THREADS 16
static int g_nthreads = 1;
static PLAT_THREAD_LOCAL int t_tid;
/* Indexed by t_tid, which is 1..MAX_THREADS for a worker and 0 for the thread
 * that produces, so there are MAX_THREADS + 1 of them: at SOA_THREADS=16 the
 * last worker used to write one element past these.
 *
 * A cache line each. The three counters are touched once per shaded pixel and
 * the two timers twice per queued command, so packed -- which is how the
 * counters used to sit, three arrays of eight-byte elements, all eight
 * workers inside two lines -- every increment is a line handed between cores.
 * Measured on this machine with eight threads: 4.8 ns an increment packed
 * against 0.8 ns a line apart, in a path that runs billions of times a run.
 * The padding is not tidiness, and the timers had to be padded anyway. */
typedef struct {
    uint64_t pixels, rej_depth, rej_alpha;
    uint64_t busy; /* ticks inside draw_command */
    uint64_t idle; /* ticks spinning for the next command */
    uint64_t last; /* when this thread's current stretch began */
    uint64_t fence_ticks; /* the part of idle spent at fences (H14) */
    uint64_t fences;      /* fenced commands this worker waited at */
} ThreadState;
static PLAT_ALIGN(64) ThreadState g_ts[MAX_THREADS + 1];
/* The alignment above only puts the array on a line; what puts each element
 * on its own is the size, and a field added without shrinking the padding
 * would quietly undo the whole point of it. */
_Static_assert(sizeof(ThreadState) == 64, "one worker's counters must be one cache line");
static uint64_t g_pool_t0; /* when the worker pool came up: what busy + idle is measured against */
#define g_pixels g_ts[t_tid].pixels
#define g_rej_depth g_ts[t_tid].rej_depth
#define g_rej_alpha g_ts[t_tid].rej_alpha

/* Close this thread's open stretch and charge it. The clamp is the same one
 * gxr_phase makes: a thread that moved to a core whose counter is behind
 * reads backwards, and charging nothing beats charging a wrap. */
static void charge(ThreadState* W, uint64_t* acc)
{
    uint64_t n = gxr_ticks();
    /* The owner's own read of *acc; the store is atomic because gxr_report
     * reads the idle timer while a parked worker is still charging it, with
     * nothing to order the two (ThreadSanitizer found it, L8). A plain mov. */
    if (n > W->last) plat_store64_relaxed((plat_a64*)acc, (int64_t)(*acc + (n - W->last)));
    W->last = n;
}
static int g_cull_flip, g_debug;
static int g_dbg_x = -1, g_dbg_y = -1; /* SOA_GXR_PIXEL=x,y: narrate every fragment landing on one pixel */
static int g_debug_lights;
static unsigned g_draw_limit, g_draw_no;
static int g_hash; /* SOA_HASH set to anything: hash every frame the port presents */

/* A test knob (PLAN-60FPS-MODS H14): SOA_GXR_STALL=<worker>:<kind>:<us>[,...]
 * makes that worker wait that many microseconds before every command of that
 * kind -- 0 a draw, 1 a copy, 2 a clear -- which turns an ordering race
 * between the workers into a certain failure. Read once, before any worker
 * exists, and off it costs a worker one untaken branch a command. */
typedef struct {
    int worker, kind, us;
} Stall;
static Stall g_stall[16];
static int g_stall_n;

static void read_stalls(const char* p)
{
    while (p && *p && g_stall_n < 16) {
        Stall st;
        int used = 0;
        if (sscanf(p, "%d:%d:%d%n", &st.worker, &st.kind, &st.us, &used) != 3 || used <= 0) break;
        g_stall[g_stall_n++] = st;
        p += used;
        if (*p != ',') break;
        p++;
    }
    if (g_stall_n) fprintf(stderr, "[gxr] SOA_GXR_STALL: %d stall%s in force; this run is a test\n", g_stall_n, g_stall_n == 1 ? "" : "s");
}

static void stall(int id, int kind)
{
    int i;
    for (i = 0; i < g_stall_n; i++)
        if (g_stall[i].worker == id && g_stall[i].kind == kind) {
            double end = gxr_clock() + g_stall[i].us * 1e-6;
            while (gxr_clock() < end) plat_relax();
        }
}

/* SOA_GXR_DRAIN=1: order EFB copies the way the renderer did before H14 --
 * a full drain before and after every copy that reads rows other workers own,
 * and a full drain for every texture read from a queued copy's destination.
 * The oracle for the fences, and the fallback. */
static int g_legacy;
static int g_token_wait; /* SOA_GXR_TOKENWAIT=1: a draw token waits for the copies before it (see gxr_bp_written) */
/* SOA_DEFLICKER=0 (P5b): the display copy without the game's deflicker, for
 * a sharper picture on a progressive display. Texture copies keep the
 * game's weights: they are the game's own effects, not the picture. */
static int g_deflicker = 1;

int gxr_enabled(void)
{
    if (g_enabled < 0) {
        read_stalls(getenv("SOA_GXR_STALL"));
        g_legacy = getenv("SOA_GXR_DRAIN") && atoi(getenv("SOA_GXR_DRAIN")) ? 1 : 0;
        g_token_wait = getenv("SOA_GXR_TOKENWAIT") && atoi(getenv("SOA_GXR_TOKENWAIT")) ? 1 : 0;
        g_deflicker = !(getenv("SOA_DEFLICKER") && !strcmp(getenv("SOA_DEFLICKER"), "0"));
        if (!g_deflicker) fprintf(stderr, "[gxr] SOA_DEFLICKER=0: the screen copy is not deflickered; texture copies are\n");
        if (g_legacy) fprintf(stderr, "[gxr] SOA_GXR_DRAIN: copies are drained around, as before H14\n");
        const char* env = getenv("SOA_RENDER");
        const char* snap = getenv("SOA_SNAP");
        g_enabled = env && atoi(env) ? 1 : 0;
        g_snap_every = snap ? (unsigned)atoi(snap) : 0;
        if (snap && strchr(snap, '@') && sscanf(strchr(snap, '@') + 1, "%u-%u", &g_snap_from, &g_snap_to) != 2) {
            fprintf(stderr, "[gxr] SOA_SNAP=%s: the range is not A-B; every frame\n", snap);
            g_snap_from = 0;
            g_snap_to = ~0u;
        }
        g_cull_flip = getenv("SOA_CULLFLIP") ? 1 : 0;
        if (getenv("SOA_GXR_PIXEL")) sscanf(getenv("SOA_GXR_PIXEL"), "%d,%d", &g_dbg_x, &g_dbg_y);
        g_debug = getenv("SOA_GXR_DEBUG") ? atoi(getenv("SOA_GXR_DEBUG")) : 0;
        g_debug_lights = getenv("SOA_GXR_LIGHTS") ? atoi(getenv("SOA_GXR_LIGHTS")) : 0;
        g_draw_limit = getenv("SOA_GXR_DRAWS") ? (unsigned)atoi(getenv("SOA_GXR_DRAWS")) : 0;
        g_hash = getenv("SOA_HASH") ? 1 : 0;
    }
    return g_enabled;
}

/* ---- tripwires -----------------------------------------------------------
 *
 * This rasterizer does not implement everything the hardware does, and where
 * it falls short it draws something plausible and says nothing -- which makes
 * a missing feature look exactly like a bug in a feature we do have. Each
 * condition below speaks the first time the game asks for what we do not
 * model, naming the register and value it asked with and what we do instead.
 *
 * Once per condition, not once overall: every call site keeps its own flag,
 * so one going off leaves the rest armed. A line that repeats every frame is
 * a line nobody reads.
 *
 * Every tripwire here runs on the guest thread that parses the command stream
 * -- BP writes, draw setup and copies all do, and the rasterizer's workers
 * reach none of it -- so the flags need no lock.
 *
 * Decoding all 23 captures config/fifo_manifest.tsv pins and running these
 * conditions over their streams sets none of them off, and none is expected
 * to fire in normal play; a line here is news. The EFB copy's vertical filter
 * used to be the one thing the game really asked for that we really did not
 * do, and it was deliberately kept out of this channel rather than put in it:
 * the game programs it before the first frame of every run and leaves it
 * there, so a tripwire for it would have been in every log, and a channel
 * with a line in it every time stops meaning anything. gxr_report stated it
 * once at the end instead. PLAN C3 implemented the filter, so both the
 * counter and that line are gone; the reasoning is kept because the next
 * unmodelled-but-always-on feature will pose the same question.
 *
 * Two trigger shapes, and the difference matters when reading a replay: the
 * BP tripwires fire on a *write*, the draw and copy ones on the *state* a
 * draw or copy reads. gx_replay loads a capture's frame-start register
 * snapshot straight into the shadow without passing it through
 * gxr_bp_written, so a feature a capture merely inherited -- zfreeze, ZTEX2,
 * a non-RGB8 EFB -- is rendered wrong and says nothing. A silent corpus is
 * evidence that nothing was *asked for* in those windows, not that nothing
 * was in force. SOA_SNAP narrows the draw tripwires and not the BP ones for
 * the same reason the other way round: gxr_draw_inner returns before
 * draw_tripwire on a frame it is skipping, so a line, a point or a
 * destination-alpha blend on such a frame says nothing -- correctly, since
 * nothing was drawn and there is no picture to be wrong. What the game asked
 * for is still caught on every frame that is written.
 */
/* Two workers can reach the same tripwire at once, so the flag is taken with
 * a compare-and-swap (L8): exactly one of them prints, and a plain flag's
 * race, which ThreadSanitizer reports, is gone. The load first keeps the
 * common case, already said, to one plain read. */
#define WARN_ONCE(...)                                                  \
    do {                                                                \
        static plat_a32 said;                                           \
        if (!plat_load32(&said) && plat_cas32(&said, 0, 1) == 0)        \
            fprintf(stderr, __VA_ARGS__);                               \
    } while (0)

/* BP_MASK (BP 0xFE) says the next BP write changes only the bits it names,
 * and we apply all 24 of them. That is only wrong when the write really
 * carries a bit outside the mask that differs from what the register already
 * holds -- and the SDK's one use of it, GXSetCoPlanar (mask 080000, then
 * GEN_MODE), writes the whole shadowed GEN_MODE back, so every bit outside
 * the mask is already what it says. The game does that from its first frame,
 * so warning on the mask write itself is a line in every log of a port that
 * renders correctly. Our own copy of the previous value is what tells the two
 * apart; g_bp_seen keeps a register whose first write is masked from being
 * compared against a zero we never saw written. */
static uint32_t g_bp_prev[256];
static uint8_t g_bp_seen[256];
static uint32_t g_bp_mask = 0xFFFFFFu;

/* The BP registers whose value alone says the game wants something we do not
 * have. Called for every BP write, after the shadow has taken it. */
static void bp_tripwire(const uint32_t* bp, uint32_t reg, uint32_t v)
{
    uint32_t mask = g_bp_mask, prev = g_bp_prev[reg & 0xFFu];
    int seen = g_bp_seen[reg & 0xFFu];
    g_bp_prev[reg & 0xFFu] = v;
    g_bp_seen[reg & 0xFFu] = 1;
    g_bp_mask = 0xFFFFFFu; /* the mask covers one write, then lapses */
    if (reg == 0xFE) {
        g_bp_mask = v & 0xFFFFFFu;
        return;
    }
    if (mask != 0xFFFFFFu && seen && ((v ^ prev) & ~mask & 0xFFFFFFu))
        WARN_ONCE("[gxr] BP_MASK %06X was in force for BP %02X %06X, which also changes %06X outside the mask; we apply all 24 bits, so those changed too and the register now differs from the hardware's by that much\n",
                  mask, reg, v, (v ^ prev) & ~mask & 0xFFFFFFu);
    switch (reg) {
    case 0x00: /* GEN_MODE */
        if ((v >> 16) & 7)
            WARN_ONCE("[gxr] GEN_MODE %06X asks for %u indirect texture stage(s); indirect textures are not modelled, so the stages are dropped and each direct coordinate is sampled unperturbed\n",
                      v, (v >> 16) & 7);
        if ((v >> 19) & 1)
            WARN_ONCE("[gxr] GEN_MODE %06X turns zfreeze on; the frozen depth plane is not modelled and depth stays per-triangle\n", v);
        break;
    case 0x43: /* PE_CONTROL */
        if (v & 7)
            WARN_ONCE("[gxr] PE_CONTROL %06X selects EFB pixel format %u; only RGB8 (0) is modelled, so the EFB keeps eight bits a channel whatever the game asked for\n",
                      v, v & 7);
        if ((v >> 3) & 7)
            WARN_ONCE("[gxr] PE_CONTROL %06X selects EFB depth format %u; only linear 24-bit Z (0) is modelled, so compressed depth is stored and compared linear\n",
                      v, (v >> 3) & 7);
        break;
    case 0x63: /* PRELOAD_MODE: writing it runs the preload, and the SDK writes zero to arm nothing */
        if (v)
            WARN_ONCE("[gxr] TMEM preload (BP 63 %06X from BP 60 %06X into BP 61 %06X / BP 62 %06X) is not modelled; every texture is decoded from main memory when a draw samples it, so one the game only preloads reads whatever is left at its address\n",
                      v, bp[0x60], bp[0x61], bp[0x62]);
        break;
    case 0xE8: /* FOGRANGE */
        if ((v >> 10) & 1)
            WARN_ONCE("[gxr] FOGRANGE (BP E8 %06X) enables fog range adjustment about x=%u; the adjustment is not modelled and fog uses eye depth alone, so the edges of the screen fog too little\n",
                      v, v & 0x3FF);
        break;
    case 0xF5: /* ZTEX2 */
        if ((v >> 2) & 3)
            WARN_ONCE("[gxr] ZTEX2 (BP F5 %06X) turns Z textures on (op %u, format %u); they are not modelled and a fragment's depth stays the interpolated one\n",
                      v, (v >> 2) & 3, v & 3);
        break;
    default: break;
    }
}

/* What a draw asks for that the pixel and primitive paths do not do. Called
 * once per draw, from the setup that reads the same registers. */
static void draw_tripwire(const uint32_t* bp, unsigned prim)
{
    uint32_t lp = bp[0x22], cmode = bp[0x41];
    /* GXSetLineWidth and GXSetPointSize count in sixths of a pixel, so 6 is
     * the one pixel raster_line and raster_point actually draw. The threshold
     * is two pixels, not "anything but exactly one": the game programs
     * linesize 7 -- 1.17 px -- in three of the captures, and drawing that one
     * pixel wide is a rounding, not a missing feature. Twice the width it
     * asked for is where the picture is visibly wrong. */
    if ((prim == 0xA8 || prim == 0xB0) && (lp & 0xFF) >= 12)
        WARN_ONCE("[gxr] LINEPTWIDTH (BP 22 %06X) asks for lines %.2f pixels wide; lines are drawn one pixel wide\n",
                  lp, (double)(lp & 0xFF) / 6.0);
    if (prim == 0xB8 && ((lp >> 8) & 0xFF) >= 12)
        WARN_ONCE("[gxr] LINEPTWIDTH (BP 22 %06X) asks for points %.2f pixels across; points are drawn as single pixels\n",
                  lp, (double)((lp >> 8) & 0xFF) / 6.0);
    /* GX_BL_DSTALPHA and its inverse read the EFB alpha plane, which RGB8
     * does not have: the console reads 1.0 there. We keep an alpha byte per
     * EFB pixel and blend against that, which is a different picture. */
    if ((cmode & 1) && (bp[0x43] & 7) == 0) {
        unsigned sfac = (cmode >> 8) & 7, dfac = (cmode >> 5) & 7;
        if (sfac >= 6 || dfac >= 6)
            WARN_ONCE("[gxr] PE_CMODE0 %06X blends with a destination-alpha factor (src %u, dst %u) at EFB format RGB8, where the console has no alpha plane and reads 1.0; we blend against the alpha we kept\n",
                      cmode, sfac, dfac);
    }
}

/* The EFB persists across frames on the console; a replay starts from the
 * state the previous frame's clear left: the clear color and z from the
 * captured registers. */
void gxr_reset_efb(void)
{
    gxr_flush();
    const uint32_t* bp = gx_bp_regs();
    uint32_t ar = bp[0x4F], gb = bp[0x50], z = bp[0x51] & 0xFFFFFFu;
    uint8_t col[4] = {(uint8_t)(ar & 0xFF), (uint8_t)((gb >> 8) & 0xFF), (uint8_t)(gb & 0xFF), (uint8_t)((ar >> 8) & 0xFF)};
    int x, y;
    for (y = 0; y < EFB_H; y++)
        for (x = 0; x < EFB_W; x++) { memcpy(g_efb[y][x], col, 4); g_efb_z[y][x] = z ? z : 0xFFFFFFu; }
    if (g_backend && g_backend->reset_efb) g_backend->reset_efb(bp);
}

void gxr_enable(int on)
{
    gxr_enabled();
    g_enabled = on;
}

void gxr_set_output(const char* png_path)
{
    snprintf(g_png_path, sizeof g_png_path, "%s", png_path);
}

/* SOA_SNAP skips the frames it is not writing, which is what makes a
 * snapshot run fast -- but the XFB copy still clears and presents every
 * frame, so with a window that skip shows the clear colour all but one frame
 * in N. main calls this when it opens a window anyway (SOA_WINDOW=1 with
 * SOA_SNAP set). */
void gxr_draw_every_frame(void)
{
    g_draw_every = 1;
}

static float xff(const uint32_t* xf, unsigned i)
{
    float f;
    uint32_t v = xf[i];
    memcpy(&f, &v, 4);
    return f;
}

static uint16_t be16(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

/* ---- vertex attributes ------------------------------------------------- */

/* VertexIn, one vertex before the transform, is in gxr_export.h, which
 * writes it out under SOA_GXR_EXPORT. */

static unsigned comp_bytes(unsigned fmt) { return fmt == 4 ? 4 : (fmt >= 2 ? 2 : 1); }

static float read_comp(const uint8_t* p, unsigned fmt, unsigned frac)
{
    float scale = 1.0f / (float)(1u << frac);
    switch (fmt) {
    case 0: return (float)p[0] * scale;
    case 1: return (float)(int8_t)p[0] * scale;
    case 2: return (float)be16(p) * scale;
    case 3: return (float)(int16_t)be16(p) * scale;
    default: { float f; uint32_t v = be32(p); memcpy(&f, &v, 4); return f; }
    }
}

static unsigned color_bytes(unsigned fmt)
{
    static const unsigned t[8] = {2, 3, 4, 2, 3, 4, 4, 4};
    return t[fmt & 7];
}

static void read_color(const uint8_t* p, unsigned fmt, Color4* c)
{
    unsigned v;
    switch (fmt) {
    case 0: /* RGB565 */
        v = be16(p);
        c->r = (float)((v >> 11) & 31) / 31.0f; c->g = (float)((v >> 5) & 63) / 63.0f; c->b = (float)(v & 31) / 31.0f; c->a = 1.0f;
        break;
    case 1: case 2: /* RGB888, RGBX8888 */
        c->r = p[0] / 255.0f; c->g = p[1] / 255.0f; c->b = p[2] / 255.0f; c->a = 1.0f;
        break;
    case 3: /* RGBA4444 */
        v = be16(p);
        c->r = (float)((v >> 12) & 15) / 15.0f; c->g = (float)((v >> 8) & 15) / 15.0f; c->b = (float)((v >> 4) & 15) / 15.0f; c->a = (float)(v & 15) / 15.0f;
        break;
    case 4: /* RGBA6666 */
        v = ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | p[2];
        c->r = (float)((v >> 18) & 63) / 63.0f; c->g = (float)((v >> 12) & 63) / 63.0f; c->b = (float)((v >> 6) & 63) / 63.0f; c->a = (float)(v & 63) / 63.0f;
        break;
    default: /* RGBA8888 */
        c->r = p[0] / 255.0f; c->g = p[1] / 255.0f; c->b = p[2] / 255.0f; c->a = p[3] / 255.0f;
        break;
    }
}

/* Where an attribute's data lives: inline in the stream, or in the array
 * the index selects (CP ARRAY_BASE/ARRAY_STRIDE, GXSetArray). */
static int g_pending_n; /* copies to memory since the last drain (with the copies below) */
void gxr_source_hazard(uint32_t addr, uint32_t bytes);

static const uint8_t* attr_data(CpuState* s, const uint32_t* cp, unsigned mode, const uint8_t** p, unsigned array, unsigned direct_size)
{
    const uint8_t* d;
    uint32_t idx, base, stride, addr;
    switch (mode) {
    case 0: return NULL;
    case 1: d = *p; *p += direct_size; return d;
    case 2: idx = **p; *p += 1; break;
    default: idx = be16(*p); *p += 2; break;
    }
    base = cp[0xA0 + array] & 0x1FFFFFFFu;
    stride = cp[0xB0 + array] & 0xFFu;
    addr = base + idx * stride;
    if ((addr & MEM_MASK) + direct_size > MEM1_SIZE) { g_verts_bad++; return NULL; }
    if (g_pending_n) gxr_source_hazard(addr, direct_size); /* an array a queued copy may be writing (H14) */
    return mem_ptr(s, addr | 0x80000000u);
}

static const uint8_t* decode_vertex(CpuState* s, const uint8_t* p, unsigned vat, VertexIn* v)
{
    const uint32_t* cp = gx_cp_regs();
    const uint32_t* xf = gx_xf_regs();
    uint32_t lo = cp[0x50], hi = cp[0x60], a = cp[0x70 + vat], b = cp[0x80 + vat], c = cp[0x90 + vat];
    unsigned i;
    const uint8_t* d;
    unsigned tc[8][3] = {
        {(a >> 21) & 1, (a >> 22) & 7, (a >> 25) & 31}, {(b >> 0) & 1, (b >> 1) & 7, (b >> 4) & 31},
        {(b >> 9) & 1, (b >> 10) & 7, (b >> 13) & 31},  {(b >> 18) & 1, (b >> 19) & 7, (b >> 22) & 31},
        {(b >> 27) & 1, (b >> 28) & 7, (c >> 0) & 31},  {(c >> 5) & 1, (c >> 6) & 7, (c >> 9) & 31},
        {(c >> 14) & 1, (c >> 15) & 7, (c >> 18) & 31}, {(c >> 23) & 1, (c >> 24) & 7, (c >> 27) & 31},
    };

    memset(v, 0, sizeof *v);
    v->posidx = (lo & 1) ? *p++ : (xf[0x1018] & 0x3F);
    for (i = 0; i < 8; i++) {
        unsigned dflt = i < 4 ? (xf[0x1018] >> (6 + 6 * i)) & 0x3F : (xf[0x1019] >> (6 * (i - 4))) & 0x3F;
        v->texidx[i] = ((lo >> (1 + i)) & 1) ? *p++ : dflt;
    }
    /* position */
    {
        unsigned cnt = (a & 1) ? 3 : 2, fmt = (a >> 1) & 7, frac = (a >> 4) & 31, nb = comp_bytes(fmt);
        d = attr_data(s, cp, (lo >> 9) & 3, &p, 0, cnt * nb);
        if (d) for (i = 0; i < cnt; i++) v->pos[i] = read_comp(d + i * nb, fmt, fmt == 4 ? 0 : frac);
    }
    /* normal */
    {
        unsigned mode = (lo >> 11) & 3, elems = (a >> 9) & 1, fmt = (a >> 10) & 7, nb = comp_bytes(fmt);
        unsigned frac = fmt == 1 ? 6 : (fmt == 3 ? 14 : 0);
        if (mode >= 2 && elems && ((a >> 31) & 1)) {
            /* NBT with three indices: read the normal, skip the other two */
            d = attr_data(s, cp, mode, &p, 1, 3 * nb);
            if (d) for (i = 0; i < 3; i++) v->nrm[i] = read_comp(d + i * nb, fmt, frac);
            attr_data(s, cp, mode, &p, 1, 3 * nb);
            attr_data(s, cp, mode, &p, 1, 3 * nb);
            v->has_nrm = 1;
        } else if (mode) {
            d = attr_data(s, cp, mode, &p, 1, (elems ? 9 : 3) * nb);
            if (d) for (i = 0; i < 3; i++) v->nrm[i] = read_comp(d + i * nb, fmt, frac);
            v->has_nrm = 1;
        }
    }
    /* colors */
    for (i = 0; i < 2; i++) {
        unsigned mode = (lo >> (13 + 2 * i)) & 3, fmt = i == 0 ? (a >> 14) & 7 : (a >> 18) & 7;
        d = attr_data(s, cp, mode, &p, 2 + i, color_bytes(fmt));
        if (d) { read_color(d, fmt, &v->col[i]); v->has_col[i] = 1; }
    }
    /* texture coordinates */
    for (i = 0; i < 8; i++) {
        unsigned mode = (hi >> (2 * i)) & 3, cnt = tc[i][0] ? 2 : 1, fmt = tc[i][1], frac = tc[i][2], nb = comp_bytes(fmt);
        unsigned k;
        d = attr_data(s, cp, mode, &p, 4 + i, cnt * nb);
        if (d) {
            for (k = 0; k < cnt; k++) v->tex[i][k] = read_comp(d + k * nb, fmt, fmt == 4 ? 0 : frac);
            v->has_tex[i] = 1;
        }
    }
    return p;
}

/* ---- transform unit ----------------------------------------------------- */

static void mat_mul_3x4(const uint32_t* xf, unsigned row0, const float in[4], float out[3])
{
    unsigned r;
    for (r = 0; r < 3; r++)
        out[r] = xff(xf, row0 + 4 * r) * in[0] + xff(xf, row0 + 4 * r + 1) * in[1] +
                 xff(xf, row0 + 4 * r + 2) * in[2] + xff(xf, row0 + 4 * r + 3) * in[3];
}

static void normalize3(float v[3])
{
    float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 1e-12f) { v[0] /= len; v[1] /= len; v[2] /= len; }
}

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

/* GX lighting for one channel (color: chan, alpha: chan). Register layout
 * per GXSetChanCtrl; light data per GXInitLight*. */
static void light_channel(const uint32_t* xf, unsigned chan, int alpha, const VertexIn* in, const float pos[3], const float nrm[3], float out[4])
{
    uint32_t ctl = xf[(alpha ? 0x1010 : 0x100E) + chan];
    uint32_t amb_reg = xf[0x100A + chan], mat_reg = xf[0x100C + chan];
    int matsrc = ctl & 1, enable = (ctl >> 1) & 1, ambsrc = (ctl >> 6) & 1;
    unsigned diffuse = (ctl >> 7) & 3, attnfn = (ctl >> 9) & 3;
    unsigned mask = ((ctl >> 2) & 15) | (((ctl >> 11) & 15) << 4);
    float mat[4], amb[4], acc[4];
    unsigned li, k;
    const Color4* vc = &in->col[chan];

    mat[0] = matsrc ? vc->r : ((mat_reg >> 24) & 255) / 255.0f;
    mat[1] = matsrc ? vc->g : ((mat_reg >> 16) & 255) / 255.0f;
    mat[2] = matsrc ? vc->b : ((mat_reg >> 8) & 255) / 255.0f;
    mat[3] = matsrc ? vc->a : (mat_reg & 255) / 255.0f;
    if (matsrc && !in->has_col[chan]) { mat[0] = mat[1] = mat[2] = mat[3] = 1.0f; }
    if (!enable) { memcpy(out, mat, sizeof mat); return; }

    if (g_debug_lights > 0) {
        unsigned li2;
        g_debug_lights--;
        fprintf(stderr, "[gxr] chan%u %s ctl %08X mat %08X amb %08X mask %02X diffuse %u attn %u\n", chan, alpha ? "alpha" : "color", ctl, mat_reg, amb_reg, mask, diffuse, attnfn);
        for (li2 = 0; li2 < 8; li2++) {
            const uint32_t* L2 = xf + 0x600 + 16 * li2;
            if (!((mask >> li2) & 1)) continue;
            fprintf(stderr, "[gxr]   light %u color %08X att %g %g %g / %g %g %g pos %g %g %g dir %g %g %g\n", li2, L2[3],
                    xff(xf, 0x600 + 16 * li2 + 4), xff(xf, 0x600 + 16 * li2 + 5), xff(xf, 0x600 + 16 * li2 + 6),
                    xff(xf, 0x600 + 16 * li2 + 7), xff(xf, 0x600 + 16 * li2 + 8), xff(xf, 0x600 + 16 * li2 + 9),
                    xff(xf, 0x600 + 16 * li2 + 10), xff(xf, 0x600 + 16 * li2 + 11), xff(xf, 0x600 + 16 * li2 + 12),
                    xff(xf, 0x600 + 16 * li2 + 13), xff(xf, 0x600 + 16 * li2 + 14), xff(xf, 0x600 + 16 * li2 + 15));
        }
    }
    amb[0] = ambsrc ? vc->r : ((amb_reg >> 24) & 255) / 255.0f;
    amb[1] = ambsrc ? vc->g : ((amb_reg >> 16) & 255) / 255.0f;
    amb[2] = ambsrc ? vc->b : ((amb_reg >> 8) & 255) / 255.0f;
    amb[3] = ambsrc ? vc->a : (amb_reg & 255) / 255.0f;
    memcpy(acc, amb, sizeof acc);

    for (li = 0; li < 8; li++) {
        const uint32_t* L;
        uint32_t color;
        float lcol[4], lpos[3], ldir[3], dir[3], attn = 1.0f, diff = 1.0f;
        if (!((mask >> li) & 1)) continue;
        L = xf + 0x600 + 16 * li;
        color = L[3];
        lcol[0] = ((color >> 24) & 255) / 255.0f; lcol[1] = ((color >> 16) & 255) / 255.0f;
        lcol[2] = ((color >> 8) & 255) / 255.0f; lcol[3] = (color & 255) / 255.0f;
        lpos[0] = xff(xf, 0x600 + 16 * li + 10); lpos[1] = xff(xf, 0x600 + 16 * li + 11); lpos[2] = xff(xf, 0x600 + 16 * li + 12);
        dir[0] = xff(xf, 0x600 + 16 * li + 13); dir[1] = xff(xf, 0x600 + 16 * li + 14); dir[2] = xff(xf, 0x600 + 16 * li + 15);

        if (attnfn == 1) { /* specular: light "position" is a direction, half-angle in dir */
            float nl;
            ldir[0] = lpos[0]; ldir[1] = lpos[1]; ldir[2] = lpos[2];
            normalize3(ldir);
            nl = nrm[0] * ldir[0] + nrm[1] * ldir[1] + nrm[2] * ldir[2];
            if (nl >= 0.0f) {
                float hd[3] = {dir[0], dir[1], dir[2]}, nh, ca, da;
                normalize3(hd);
                nh = nrm[0] * hd[0] + nrm[1] * hd[1] + nrm[2] * hd[2];
                if (nh < 0.0f) nh = 0.0f;
                ca = xff(xf, 0x600 + 16 * li + 4) + xff(xf, 0x600 + 16 * li + 5) * nh + xff(xf, 0x600 + 16 * li + 6) * nh * nh;
                da = xff(xf, 0x600 + 16 * li + 7) + xff(xf, 0x600 + 16 * li + 8) * nh + xff(xf, 0x600 + 16 * li + 9) * nh * nh;
                attn = (da != 0.0f) ? (ca < 0.0f ? 0.0f : ca) / da : 0.0f;
            } else attn = 0.0f;
        } else {
            ldir[0] = lpos[0] - pos[0]; ldir[1] = lpos[1] - pos[1]; ldir[2] = lpos[2] - pos[2];
            if (attnfn == 3) { /* spot */
                float dist2 = ldir[0] * ldir[0] + ldir[1] * ldir[1] + ldir[2] * ldir[2];
                float dist = sqrtf(dist2), cosv, ca, da;
                if (dist > 1e-12f) { ldir[0] /= dist; ldir[1] /= dist; ldir[2] /= dist; }
                cosv = ldir[0] * dir[0] + ldir[1] * dir[1] + ldir[2] * dir[2];
                if (cosv < 0.0f) cosv = 0.0f;
                ca = xff(xf, 0x600 + 16 * li + 4) + xff(xf, 0x600 + 16 * li + 5) * cosv + xff(xf, 0x600 + 16 * li + 6) * cosv * cosv;
                da = xff(xf, 0x600 + 16 * li + 7) + xff(xf, 0x600 + 16 * li + 8) * dist + xff(xf, 0x600 + 16 * li + 9) * dist2;
                attn = (da != 0.0f) ? (ca < 0.0f ? 0.0f : ca) / da : 0.0f;
            } else {
                normalize3(ldir);
            }
        }
        if (diffuse != 0) {
            diff = nrm[0] * ldir[0] + nrm[1] * ldir[1] + nrm[2] * ldir[2];
            if (diffuse == 2 && diff < 0.0f) diff = 0.0f;
        }
        for (k = 0; k < 4; k++) acc[k] += lcol[k] * attn * diff;
    }
    for (k = 0; k < 4; k++) out[k] = mat[k] * clamp01(acc[k]);
}

/* A mod's say in the projection (PLAN-60FPS-MODS M3c): the six parameters of
 * GXSetProjection (XF 0x1020-0x1025) and whether it is orthographic (0x1026),
 * handed over once each time the game sets a new one and cached against the
 * raw register words, never per vertex. Registered through a setter so the
 * renderer still links alone; with none, transform() is the code it was. It
 * runs on the thread that parses the command stream, the guest's. */
static void (*g_proj_filter)(float p[6], int orthographic);
static uint32_t g_proj_raw[7];
static float g_proj_out[6];
static int g_proj_valid;

void gxr_set_projection_filter(void (*fn)(float p[6], int orthographic))
{
    g_proj_filter = fn;
    g_proj_valid = 0;
}

static void filtered_projection(const uint32_t* xf, float p[6])
{
    int i;
    if (!g_proj_valid || memcmp(g_proj_raw, xf + 0x1020, sizeof g_proj_raw) != 0) {
        memcpy(g_proj_raw, xf + 0x1020, sizeof g_proj_raw);
        for (i = 0; i < 6; i++) g_proj_out[i] = xff(xf, 0x1020 + (unsigned)i);
        g_proj_filter(g_proj_out, (int)(xf[0x1026] & 1));
        g_proj_valid = 1;
    }
    memcpy(p, g_proj_out, sizeof g_proj_out);
}

static void transform(CpuState* s, const VertexIn* in, Vertex* out)
{
    const uint32_t* xf = gx_xf_regs();
    float pos4[4] = {in->pos[0], in->pos[1], in->pos[2], 1.0f};
    float view[3], nrm[3] = {0, 0, 1};
    unsigned i;
    (void)s;

    mat_mul_3x4(xf, 4 * (in->posidx & 0x3F), pos4, view);
    if (in->has_nrm) {
        unsigned nb = 0x400 + 3 * (in->posidx & 0x3F);
        unsigned r;
        for (r = 0; r < 3; r++)
            nrm[r] = xff(xf, nb + 3 * r) * in->nrm[0] + xff(xf, nb + 3 * r + 1) * in->nrm[1] + xff(xf, nb + 3 * r + 2) * in->nrm[2];
        normalize3(nrm);
    }

    /* projection (XF 0x1020-0x1026, GXSetProjection) */
    {
        float p0 = xff(xf, 0x1020), p1 = xff(xf, 0x1021), p2 = xff(xf, 0x1022), p3 = xff(xf, 0x1023), p4 = xff(xf, 0x1024), p5 = xff(xf, 0x1025);
        if (g_proj_filter) {
            float p[6];
            filtered_projection(xf, p);
            p0 = p[0]; p1 = p[1]; p2 = p[2]; p3 = p[3]; p4 = p[4]; p5 = p[5];
        }
        if (xf[0x1026] & 1) { /* orthographic */
            out->x = p0 * view[0] + p1;
            out->y = p2 * view[1] + p3;
            out->z = p4 * view[2] + p5;
            out->w = 1.0f;
        } else {
            out->x = p0 * view[0] + p1 * view[2];
            out->y = p2 * view[1] + p3 * view[2];
            out->z = p4 * view[2] + p5;
            out->w = -view[2];
        }
    }

    /* color channels (XF 0x1009 numColorChans, 0x100E.. controls) */
    for (i = 0; i < 2; i++) {
        float c[4], a[4];
        light_channel(xf, i, 0, in, view, nrm, c);
        light_channel(xf, i, 1, in, view, nrm, a);
        out->col[i].r = c[0]; out->col[i].g = c[1]; out->col[i].b = c[2]; out->col[i].a = a[3];
    }

    /* texture coordinate generation (XF 0x103F count, 0x1040+ GXSetTexCoordGen) */
    {
        unsigned n = xf[0x103F] & 15;
        for (i = 0; i < 8; i++) {
            uint32_t info = xf[0x1040 + i];
            unsigned proj = (info >> 1) & 1, form = (info >> 2) & 1, type = (info >> 4) & 7, src = (info >> 7) & 31;
            float in4[4] = {0, 0, 1, 1}, o[3] = {0, 0, 1};
            if (i >= n) { out->tex[i][0] = out->tex[i][1] = 0; out->tex[i][2] = 1; continue; }
            if (src == 0) { in4[0] = in->pos[0]; in4[1] = in->pos[1]; in4[2] = form ? in->pos[2] : 1.0f; }
            else if (src == 1) { in4[0] = in->nrm[0]; in4[1] = in->nrm[1]; in4[2] = form ? in->nrm[2] : 1.0f; }
            else if (src >= 5 && src < 13) { in4[0] = in->tex[src - 5][0]; in4[1] = in->tex[src - 5][1]; in4[2] = 1.0f; }
            if (type == 2 || type == 3) {
                const Color4* cc = &out->col[type - 2];
                o[0] = cc->r; o[1] = cc->g; o[2] = 1.0f;
            } else if (type == 0) {
                unsigned row0 = 4 * (in->texidx[i] & 0x3F);
                if (proj) mat_mul_3x4(xf, row0, in4, o);
                else {
                    float t[3];
                    mat_mul_3x4(xf, row0, in4, t);
                    o[0] = t[0]; o[1] = t[1]; o[2] = 1.0f;
                }
            } else { /* emboss: pass the source through */
                o[0] = in4[0]; o[1] = in4[1]; o[2] = 1.0f;
            }
            if (xf[0x1012] & 1) { /* dual transform (GXSetTexCoordGen2 post matrix) */
                uint32_t post = xf[0x1050 + i];
                unsigned pidx = post & 0x3F;
                float t4[4] = {o[0], o[1], o[2], 1.0f}, r[3];
                if ((post >> 8) & 1) { float v3[3] = {o[0], o[1], o[2]}; normalize3(v3); t4[0] = v3[0]; t4[1] = v3[1]; t4[2] = v3[2]; }
                mat_mul_3x4(xf, 0x500 + 4 * pidx, t4, r);
                o[0] = r[0]; o[1] = r[1]; o[2] = r[2];
            }
            out->tex[i][0] = o[0]; out->tex[i][1] = o[1]; out->tex[i][2] = proj ? o[2] : 1.0f;
        }
    }
}

/* ---- draw commands ------------------------------------------------------
 * A draw is parsed, transformed and queued by the main thread; worker
 * threads rasterize queued draws in order, each taking every Nth row, so
 * per-pixel ordering is preserved and the game keeps running meanwhile.
 * EFB copies and clears wait for the queue to drain (gxr_flush). */

#define QUEUE_CAP 4096
#define QMASK (QUEUE_CAP - 1) /* commands are numbered, not indexed, so the capacity is a power of two */
#define ARENA_BYTES (48u << 20)

/* The 20-bit floats in the fog registers: sign, 8-bit exponent, 11-bit mantissa. */
static float fog_float(uint32_t v)
{
    uint32_t bits = ((v >> 19) & 1) << 31 | ((v >> 11) & 0xFF) << 23 | (v & 0x7FF) << 12;
    float f;
    memcpy(&f, &bits, 4);
    return f;
}


static void scissor_rect(const uint32_t* bp, Rect* r)
{
    uint32_t tl = bp[0x20], br = bp[0x21], off = bp[0x59];
    int xoff = (int)((off & 0x3FF) * 2), yoff = (int)(((off >> 10) & 0x3FF) * 2);
    r->x0 = (int)((tl >> 12) & 0x7FF) - xoff;
    r->y0 = (int)(tl & 0x7FF) - yoff;
    r->x1 = (int)((br >> 12) & 0x7FF) - xoff;
    r->y1 = (int)(br & 0x7FF) - yoff;
    if (r->x0 < 0) r->x0 = 0;
    if (r->y0 < 0) r->y0 = 0;
    if (r->x1 > EFB_W - 1) r->x1 = EFB_W - 1;
    if (r->y1 > EFB_H - 1) r->y1 = EFB_H - 1;
}

static void raster_prepare(const uint32_t* xf, const uint32_t* bp, RasterCfg* rc)
{
    uint32_t off = bp[0x59];
    scissor_rect(bp, &rc->scissor);
    rc->cull = (bp[0] >> 14) & 3;
    rc->wd = xff(xf, 0x101A); rc->ht = xff(xf, 0x101B); rc->zrange = xff(xf, 0x101C);
    rc->xorig = xff(xf, 0x101D) - (float)((off & 0x3FF) * 2);
    rc->yorig = xff(xf, 0x101E) - (float)(((off >> 10) & 0x3FF) * 2);
    rc->farz = xff(xf, 0x101F);
}

static void pixel_prepare(const uint32_t* bp, PixelCfg* px)
{
    uint32_t cmode = bp[0x41], cmode1 = bp[0x42], zmode = bp[0x40];
    px->blend_en = cmode & 1; px->logic_en = (cmode >> 1) & 1;
    px->col_upd = (cmode >> 3) & 1; px->alpha_upd = (cmode >> 4) & 1;
    px->dfac = (cmode >> 5) & 7; px->sfac = (cmode >> 8) & 7;
    px->subtract = (cmode >> 11) & 1; px->lop = (cmode >> 12) & 15;
    px->const_alpha = (cmode1 & 0x100) ? (int)(cmode1 & 0xFF) : -1;
    px->z_en = zmode & 1; px->z_func = (zmode >> 1) & 7; px->z_upd = (zmode >> 4) & 1;
    px->ztop = (bp[0x43] >> 6) & 1;
    px->fog_type = (bp[0xF1] >> 21) & 7;
    px->fog_proj = (bp[0xF1] >> 20) & 1;
    px->fog_a = fog_float(bp[0xEE]);
    px->fog_c = fog_float(bp[0xF1]);
    px->fog_b_mag = bp[0xEF] & 0xFFFFFFu;
    px->fog_b_shift = bp[0xF0] & 0x1F;
    px->fog_color[0] = (uint8_t)((bp[0xF2] >> 16) & 0xFF);
    px->fog_color[1] = (uint8_t)((bp[0xF2] >> 8) & 0xFF);
    px->fog_color[2] = (uint8_t)(bp[0xF2] & 0xFF);
    px->blend_kind = !px->blend_en && !px->logic_en ? 1u
                   : (px->blend_en && px->sfac == 4 && px->dfac == 5 && !px->subtract ? 2u : 0u);
}

/* Fog blends the TEV output toward the fog colour by a function of eye
 * distance recovered from the 24-bit screen z, as the pixel engine does. */
/* The per-fragment helpers, inlined whether MSVC would or not: the worker
 * profile found shade, blend_pixel and depth_test as calls of their own, a
 * call each per fragment (H15c). */
#ifdef _MSC_VER
#define PIXEL_INLINE static __forceinline
#else
#define PIXEL_INLINE static inline
#endif

PIXEL_INLINE void fog_apply(const PixelCfg* px, uint8_t out[4], float depth)
{
    float ze, f;
    int fi, i;
    /* Unsigned, and bounded by the clamp except for a NaN, which converts to 0
     * on every target: MSVC and clang on x86-64 go through a 64-bit cvttss2si
     * and keep the low half, and ARM64's fcvtzu gives 0. So not plat_f2i;
     * render_check's driver checks the expression (L6). depth_test's is the
     * same. */
    uint32_t zs = (uint32_t)(depth < 0.0f ? 0.0f : (depth > 1.0f ? 16777215.0f : depth * 16777215.0f));
    if (px->fog_type == 0) return;
    if (!px->fog_proj) {
        int32_t denom = (int32_t)px->fog_b_mag - (int32_t)(zs >> px->fog_b_shift);
        if (denom == 0) return;
        ze = (px->fog_a * 16777215.0f) / (float)denom;
    } else {
        ze = px->fog_a * ((float)zs / 16777215.0f);
    }
    f = ze - px->fog_c;
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    /* soa_exp2f, not the C library's: correctly rounded, so the same bits on
     * every platform (crmath.h, L6). */
    switch (px->fog_type) {
    case 2: break;                                     /* linear */
    case 4: f = 1.0f - soa_exp2f(-8.0f * f); break;    /* exp */
    case 5: f = 1.0f - soa_exp2f(-8.0f * f * f); break; /* exp2 */
    case 6: f = soa_exp2f(-8.0f * (1.0f - f)); break;  /* backward exp */
    case 7: f = soa_exp2f(-8.0f * (1.0f - f) * (1.0f - f)); break;
    default: return;
    }
    fi = plat_f2i(f * 256.0f); /* a NaN passes both clamps above */
    if (fi > 256) fi = 256;
    for (i = 0; i < 3; i++) out[i] = (uint8_t)((out[i] * (256 - fi) + px->fog_color[i] * fi) >> 8);
}

static void to_screen(const RasterCfg* rc, Vertex* v)
{
    float iw = v->w != 0.0f ? 1.0f / v->w : 0.0f;
    v->sx = rc->xorig + v->x * iw * rc->wd;
    v->sy = rc->yorig + v->y * iw * rc->ht;
    v->depth = (rc->farz + v->z * iw * rc->zrange) / 16777216.0f;
}

/* ---- pixels --------------------------------------------------------------- */

PIXEL_INLINE void blend_pixel(const PixelCfg* px, int x, int y, const uint8_t src[4])
{
    uint8_t* dst = g_efb[y][x];
    int out[4], i;
    int sa = px->const_alpha >= 0 ? px->const_alpha : src[3], da = dst[3];

    /* The two cases most pixels take, as the general code below computes
     * them with the factors fixed (H15c): the source as it is, and
     * src * a + dst * (255 - a), rounded, which cannot leave 0..255. */
    if (px->blend_kind == 1) {
        out[0] = src[0]; out[1] = src[1]; out[2] = src[2];
    } else if (px->blend_kind == 2) {
        int sf = src[3], df = 255 - src[3];
        for (i = 0; i < 3; i++) out[i] = (src[i] * sf + dst[i] * df + 127) / 255;
    } else if (px->blend_en) {
        int sf, df;
        switch (px->sfac) {
        case 0: sf = 0; break; case 1: sf = 255; break; case 2: sf = -1; break; case 3: sf = -2; break;
        case 4: sf = src[3]; break; case 5: sf = 255 - src[3]; break; case 6: sf = da; break; default: sf = 255 - da; break;
        }
        switch (px->dfac) {
        case 0: df = 0; break; case 1: df = 255; break; case 2: df = -1; break; case 3: df = -2; break;
        case 4: df = src[3]; break; case 5: df = 255 - src[3]; break; case 6: df = da; break; default: df = 255 - da; break;
        }
        for (i = 0; i < 3; i++) {
            int s_f = sf == -1 ? dst[i] : (sf == -2 ? 255 - dst[i] : sf);
            int d_f = df == -1 ? src[i] : (df == -2 ? 255 - src[i] : df);
            int r = px->subtract ? dst[i] - src[i] : (src[i] * s_f + dst[i] * d_f + 127) / 255;
            out[i] = r < 0 ? 0 : (r > 255 ? 255 : r);
        }
    } else if (px->logic_en) {
        for (i = 0; i < 3; i++) {
            int sv = src[i], dv = dst[i], r;
            switch (px->lop) {
            case 0: r = 0; break; case 1: r = sv & dv; break; case 2: r = sv & ~dv; break; case 3: r = sv; break;
            case 4: r = ~sv & dv; break; case 5: r = dv; break; case 6: r = sv ^ dv; break; case 7: r = sv | dv; break;
            case 8: r = ~(sv | dv); break; case 9: r = ~(sv ^ dv); break; case 10: r = ~dv; break; case 11: r = sv | ~dv; break;
            case 12: r = ~sv; break; case 13: r = ~sv | dv; break; case 14: r = ~(sv & dv); break; default: r = 255; break;
            }
            out[i] = r & 255;
        }
    } else {
        out[0] = src[0]; out[1] = src[1]; out[2] = src[2];
    }
    if (px->col_upd) { dst[0] = (uint8_t)out[0]; dst[1] = (uint8_t)out[1]; dst[2] = (uint8_t)out[2]; }
    if (px->alpha_upd) dst[3] = (uint8_t)sa;
}

PIXEL_INLINE int depth_test(const PixelCfg* px, int x, int y, float depth)
{
    uint32_t z = (uint32_t)(depth < 0.0f ? 0.0f : (depth > 1.0f ? 16777215.0f : depth * 16777215.0f)); /* see fog_apply */
    uint32_t cur = g_efb_z[y][x];
    int pass;
    if (!px->z_en) return 1;
    switch (px->z_func) {
    case 0: pass = 0; break; case 1: pass = z < cur; break; case 2: pass = z == cur; break; case 3: pass = z <= cur; break;
    case 4: pass = z > cur; break; case 5: pass = z != cur; break; case 6: pass = z >= cur; break; default: pass = 1; break;
    }
    if (pass && px->z_upd) g_efb_z[y][x] = z;
    return pass;
}

/* What shade() did with a pixel. Its callers count them, a triangle in
 * locals added once at its end: a thread-local counter bumped a pixel at a
 * time was 3% of the workers' time (FINDINGS "H15d's starting point"). */
#define SHADE_DRAWN 0
#define SHADE_ALPHA 1
#define SHADE_DEPTH 2

PIXEL_INLINE int shade(const DrawCmd* D, int x, int y, const int col[2][4], const float tex[8][4], float depth)
{
    uint8_t out[4];
    int alpha_ok = 1;
    if (x == g_dbg_x && y == g_dbg_y) {
        uint8_t o[4]; int ok = 1;
        extern int g_tev_narrate;
        g_tev_narrate = 1;
        tev_pixel(&D->tev, col, tex, o, &ok);
        g_tev_narrate = 0;
        fprintf(stderr, "[gxr] pixel %d,%d: col0 %d,%d,%d,%d tex0 %.3f,%.3f lod %.2f depth %.6f z-buf %.6f -> tev %d,%d,%d,%d alpha_ok %d blend %d z_en %d z_func %u\n",
                x, y, col[0][0], col[0][1], col[0][2], col[0][3], tex[0][0], tex[0][1], tex[0][3], depth,
                (float)g_efb_z[y][x] / 16777215.0f, o[0], o[1], o[2], o[3], ok, D->px.blend_en, D->px.z_en, D->px.z_func);
    }
    /* Z before texturing (PE_CONTROL ztop) or after: order matters only for
     * alpha-tested pixels, so test late unless ztop is set -- or unless the
     * draw's alpha test passes every alpha, when the order cannot matter and
     * testing first spares the TEV every fragment depth would discard (H15a). */
    if ((D->px.ztop || D->tev.alpha_always) && !depth_test(&D->px, x, y, depth)) return SHADE_DEPTH;
    tev_pixel(&D->tev, col, tex, out, &alpha_ok);
    if (!alpha_ok) return SHADE_ALPHA;
    if (!(D->px.ztop || D->tev.alpha_always) && !depth_test(&D->px, x, y, depth)) return SHADE_DEPTH;
    fog_apply(&D->px, out, depth);
    blend_pixel(&D->px, x, y, out);
    return SHADE_DRAWN;
}

/* One pixel's outcome into this thread's counts, for lines and points. */
static void count_shaded(int r)
{
    if (r == SHADE_DRAWN) g_pixels++;
    else if (r == SHADE_ALPHA) g_rej_alpha++;
    else g_rej_depth++;
}

/* Plane equation of a value linear in screen space: v = a*x + b*y + c. */
typedef struct { float a, b, c; } Plane;

static Plane plane_of(const Vertex* v0, const Vertex* v1, const Vertex* v2, float p0, float p1, float p2, float inv_area)
{
    Plane P;
    float dx1 = v1->sx - v0->sx, dy1 = v1->sy - v0->sy, dx2 = v2->sx - v0->sx, dy2 = v2->sy - v0->sy;
    float dp1 = p1 - p0, dp2 = p2 - p0;
    P.a = (dp1 * dy2 - dp2 * dy1) * inv_area;
    P.b = (dp2 * dx1 - dp1 * dx2) * inv_area;
    P.c = p0 - P.a * v0->sx - P.b * v0->sy;
    return P;
}

#define MAX_ATTR (2 + 8 + 8 * 3) /* depth, 1/w, two colours, eight texcoords */

/* log2 of the texel footprint of one pixel for a texcoord slot, from the
 * screen-space derivatives of s = (S/w)/(1/w) and t. */
static float span_lod(const Plane* attr, int wi, int ti, float px, float py, float scale_s, float scale_t)
{
    float W = attr[wi].a * px + attr[wi].b * py + attr[wi].c;
    float S = attr[ti].a * px + attr[ti].b * py + attr[ti].c;
    float T = attr[ti + 1].a * px + attr[ti + 1].b * py + attr[ti + 1].c;
    float Q = attr[ti + 2].a * px + attr[ti + 2].b * py + attr[ti + 2].c;
    float iw2, dsdx, dsdy, dtdx, dtdy, q, fx, fy, f;
    if (W == 0.0f) return 0.0f;
    iw2 = 1.0f / (W * W);
    q = Q / W;
    if (q == 0.0f) q = 1.0f;
    /* d(S/W)/dx = (S_a W - S W_a) / W^2, likewise for y and for T; divide by q */
    dsdx = (attr[ti].a * W - S * attr[wi].a) * iw2 / q * scale_s;
    dsdy = (attr[ti].b * W - S * attr[wi].b) * iw2 / q * scale_s;
    dtdx = (attr[ti + 1].a * W - T * attr[wi].a) * iw2 / q * scale_t;
    dtdy = (attr[ti + 1].b * W - T * attr[wi].b) * iw2 / q * scale_t;
    fx = dsdx * dsdx + dtdx * dtdx;
    fy = dsdy * dsdy + dtdy * dtdy;
    f = fx > fy ? fx : fy;
    if (f <= 1e-12f) return -16.0f;
    return 0.5f * soa_log2f(f); /* correctly rounded (crmath.h, L6) */
}

float gxr_span_lod(const float planes[4][3], float px, float py, float scale_s, float scale_t)
{
    Plane attr[4];
    int i;
    for (i = 0; i < 4; i++) {
        attr[i].a = planes[i][0];
        attr[i].b = planes[i][1];
        attr[i].c = planes[i][2];
    }
    return span_lod(attr, 0, 1, px, py, scale_s, scale_t);
}

static void raster_triangle(const DrawCmd* D, const Vertex* a, const Vertex* b, const Vertex* c)
{
    const Rect* sc = &D->rc.scissor;
    float area = (b->sx - a->sx) * (c->sy - a->sy) - (c->sx - a->sx) * (b->sy - a->sy);
    unsigned cull = D->rc.cull;
    int minx, miny, maxx, maxy, x, y;
    float inv_area;
    Plane e0, e1, e2;                 /* barycentric weights, positive inside */
    Plane attr[MAX_ATTR];             /* perspective-corrected attributes (value/w) */
    int nattr = 0, ci[2], ti[8], di, wi;
    unsigned i, k;
    const Vertex* v[3];
    float lod[8] = {0, 0, 0, 0, 0, 0, 0, 0}, dlod[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    /* One slot per texcoord: s, t, q and the level of detail. A TEV stage may
     * name any of the eight whether this draw supplies it or not, and what it
     * reads then is defined -- the console keeps eight coordinates and only
     * refreshes the ones the XF generates. This renderer's transform unit
     * writes (0, 0, 1) into every ungenerated slot, so hold the unsupplied
     * ones at that value here as well: a stage naming one samples texel (0, 0)
     * of its map, the same result the line and point paths already give,
     * rather than whatever the stack held, which made the frame depend on how
     * many worker threads were rasterizing. */
    float tex[8][4];
    int col[2][4], chn[2], act[8], nch, nact;
    uint64_t shaded[3] = {0, 0, 0};

    if (area == 0.0f) return;
    if (g_cull_flip) area = -area;
    if ((cull == 1 && area < 0.0f) || (cull == 2 && area > 0.0f) || cull == 3) return; /* back = negative here */
    if (g_cull_flip) area = -area;

    if (g_debug && t_tid <= 1 && (g_tris <= 8 || (g_debug > 1 && g_draw_no >= (unsigned)g_debug))) /* SOA_GXR_DEBUG=N: also every triangle from draw N on */
        fprintf(stderr, "[gxr] tri (%.1f,%.1f,%.3f) (%.1f,%.1f,%.3f) (%.1f,%.1f,%.3f) area %.1f scissor %d,%d-%d,%d\n",
                a->sx, a->sy, a->depth, b->sx, b->sy, b->depth, c->sx, c->sy, c->depth, area, sc->x0, sc->y0, sc->x1, sc->y1);
    /* Clamp in float first: a vertex just past the near plane can sit millions of
     * pixels off screen, and a huge value converted to int becomes INT_MIN.
     * fminf and fmaxf drop a NaN, so the casts see only [-1e6, 1e6] (L6). */
    minx = (int)fmaxf(-1e6f, floorf(fminf(a->sx, fminf(b->sx, c->sx))));
    maxx = (int)fminf(1e6f, ceilf(fmaxf(a->sx, fmaxf(b->sx, c->sx))));
    miny = (int)fmaxf(-1e6f, floorf(fminf(a->sy, fminf(b->sy, c->sy))));
    maxy = (int)fminf(1e6f, ceilf(fmaxf(a->sy, fmaxf(b->sy, c->sy))));
    if (minx < sc->x0) minx = sc->x0;
    if (miny < sc->y0) miny = sc->y0;
    if (maxx > sc->x1) maxx = sc->x1;
    if (maxy > sc->y1) maxy = sc->y1;
    if (minx > maxx || miny > maxy) return;

    /* Orient so the weights are positive inside. */
    v[0] = a; v[1] = b; v[2] = c;
    if (area < 0.0f) { v[1] = c; v[2] = b; area = -area; }
    inv_area = 1.0f / area;
    e0 = plane_of(v[0], v[1], v[2], 1.0f, 0.0f, 0.0f, inv_area);
    e1 = plane_of(v[0], v[1], v[2], 0.0f, 1.0f, 0.0f, inv_area);
    e2 = plane_of(v[0], v[1], v[2], 0.0f, 0.0f, 1.0f, inv_area);

    {
        float iw0 = v[0]->w != 0.0f ? 1.0f / v[0]->w : 1.0f;
        float iw1 = v[1]->w != 0.0f ? 1.0f / v[1]->w : 1.0f;
        float iw2 = v[2]->w != 0.0f ? 1.0f / v[2]->w : 1.0f;
        di = nattr; attr[nattr++] = plane_of(v[0], v[1], v[2], v[0]->depth, v[1]->depth, v[2]->depth, inv_area);
        wi = nattr; attr[nattr++] = plane_of(v[0], v[1], v[2], iw0, iw1, iw2, inv_area);
        for (i = 0; i < 2; i++) {
            const float* c0 = &v[0]->col[i].r; const float* c1 = &v[1]->col[i].r; const float* c2 = &v[2]->col[i].r;
            ci[i] = nattr;
            if (!((D->nchan >> i) & 1)) continue;
            for (k = 0; k < 4; k++)
                attr[nattr++] = plane_of(v[0], v[1], v[2], c0[k] * iw0, c1[k] * iw1, c2[k] * iw2, inv_area);
        }
        for (i = 0; i < 8; i++) {
            tex[i][0] = 0.0f; tex[i][1] = 0.0f; tex[i][2] = 1.0f; tex[i][3] = 0.0f;
            ti[i] = -1;
            if (!((D->ntex >> i) & 1)) continue;
            ti[i] = nattr;
            for (k = 0; k < 3; k++)
                attr[nattr++] = plane_of(v[0], v[1], v[2], v[0]->tex[i][k] * iw0, v[1]->tex[i][k] * iw1, v[2]->tex[i][k] * iw2, inv_area);
        }
    }

    /* The colour channels and texture coordinates the draw interpolates,
     * listed once so the pixel loop below visits only those. */
    {
        int j;
        nch = nact = 0;
        for (j = 0; j < 2; j++)
            if ((D->nchan >> j) & 1) chn[nch++] = j;
        for (j = 0; j < 8; j++)
            if (ti[j] >= 0) act[nact++] = j;
        memset(col, 0, sizeof col);
    }

    for (y = miny; y <= maxy; y++) {
        float py = (float)y + 0.5f;
        float av[MAX_ATTR];
        float px0;
        int n, xs = minx, xe = maxx;
        if (g_nthreads > 1 && (unsigned)y % (unsigned)g_nthreads != (unsigned)(t_tid - 1)) continue;
        /* The row's span: each weight w = a*x + b*y + c must be >= 0. */
        {
            const Plane* e[3] = {&e0, &e1, &e2};
            int j;
            for (j = 0; j < 3; j++) {
                float base = e[j]->b * py + e[j]->c; /* w at x = 0 */
                /* Compare in float before converting: a nearly horizontal edge has an x
                 * coefficient that is a rounding crumb, and -base/a runs to billions,
                 * which an int conversion turns into INT_MIN and an empty row. The
                 * casts see only (xs, 1e8] and [-1e8, xe): a NaN fails the compare. */
                if (e[j]->a > 0.0f) { float lim = ceilf(-base / e[j]->a - 0.5f); if (lim > (float)xs) xs = lim > 1e8f ? xe + 1 : (int)lim; }
                else if (e[j]->a < 0.0f) { float lim = floorf(-base / e[j]->a - 0.5f); if (lim < (float)xe) xe = lim < -1e8f ? xs - 1 : (int)lim; }
                else if (base < 0.0f) { xs = xe + 1; break; }
            }
        }
        if (g_dbg_x >= 0 && y == g_dbg_y)
            fprintf(stderr, "[gxr] row %d of tri (%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f): span %d..%d; e0 %g,%g,%g e1 %g,%g,%g e2 %g,%g,%g box %d..%d\n",
                    y, v[0]->sx, v[0]->sy, v[1]->sx, v[1]->sy, v[2]->sx, v[2]->sy, xs, xe, e0.a, e0.b, e0.c, e1.a, e1.b, e1.c, e2.a, e2.b, e2.c, minx, maxx);
        if (xs > xe) continue;
        px0 = (float)xs + 0.5f;
        for (n = 0; n < nattr; n++) av[n] = attr[n].a * px0 + attr[n].b * py + attr[n].c;
        /* Level of detail per texcoord slot: log2 of the texel footprint of
         * one pixel, evaluated at both ends of the span and interpolated. */
        for (i = 0; i < 8; i++) {
            float l0, l1;
            if (ti[i] < 0 || !((D->miptex >> i) & 1)) continue;
            l0 = span_lod(attr, wi, ti[i], px0, py, D->tev.tex[D->texmap_of[i]].scale_s, D->tev.tex[D->texmap_of[i]].scale_t);
            l1 = span_lod(attr, wi, ti[i], (float)xe + 0.5f, py, D->tev.tex[D->texmap_of[i]].scale_s, D->tev.tex[D->texmap_of[i]].scale_t);
            lod[i] = l0;
            dlod[i] = xe > xs ? (l1 - l0) / (float)(xe - xs) : 0.0f;
        }
        for (x = xs; x <= xe; x++) {
            float w = av[wi] != 0.0f ? 1.0f / av[wi] : 0.0f;
            float w255 = w * 255.0f;
            int j;
            /* Only the channels and coordinates this draw uses, found once a
             * triangle below (H15c); the rest of col[] was zeroed there and
             * stays zero, as the per-pixel test here used to leave it. */
            for (j = 0; j < nch; j++) {
                i = chn[j];
                for (k = 0; k < 4; k++) {
                    int cv = plat_f2i(av[ci[i] + k] * w255 + 0.5f);
                    col[i][k] = cv < 0 ? 0 : (cv > 255 ? 255 : cv);
                }
            }
            for (j = 0; j < nact; j++) {
                i = act[j];
                tex[i][0] = av[ti[i]] * w; tex[i][1] = av[ti[i] + 1] * w; tex[i][2] = av[ti[i] + 2] * w;
                tex[i][3] = lod[i];
                lod[i] += dlod[i];
            }
            shaded[shade(D, x, y, (const int (*)[4])col, (const float (*)[4])tex, av[di])]++;
            for (n = 0; n < nattr; n++) av[n] += attr[n].a;
        }
    }
    g_pixels += shaded[SHADE_DRAWN];
    g_rej_alpha += shaded[SHADE_ALPHA];
    g_rej_depth += shaded[SHADE_DEPTH];
}

static void raster_line(const DrawCmd* D, const Vertex* a, const Vertex* b)
{
    const Rect* sc = &D->rc.scissor;
    float dx = b->sx - a->sx, dy = b->sy - a->sy;
    float len = fmaxf(fabsf(dx), fabsf(dy));
    int n = plat_f2i(ceilf(len)), i;
    unsigned t, k;
    g_lines++;
    if (n < 1) n = 1;
    for (i = 0; i <= n; i++) {
        float f = (float)i / (float)n;
        int x = plat_f2i(floorf(a->sx + dx * f)), y = plat_f2i(floorf(a->sy + dy * f));
        int col[2][4];
        float tex[8][4];
        if (x < sc->x0 || x > sc->x1 || y < sc->y0 || y > sc->y1) continue;
        for (t = 0; t < 2; t++) {
            const float* ca = &a->col[t].r; const float* cb = &b->col[t].r;
            for (k = 0; k < 4; k++) {
                int cv = plat_f2i((ca[k] + (cb[k] - ca[k]) * f) * 255.0f + 0.5f);
                col[t][k] = cv < 0 ? 0 : (cv > 255 ? 255 : cv);
            }
        }
        for (t = 0; t < 8; t++) {
            for (k = 0; k < 3; k++) tex[t][k] = a->tex[t][k] + (b->tex[t][k] - a->tex[t][k]) * f;
            tex[t][3] = 0.0f;
        }
        count_shaded(shade(D, x, y, col, tex, a->depth + (b->depth - a->depth) * f));
    }
}

static void raster_point(const DrawCmd* D, const Vertex* a)
{
    const Rect* sc = &D->rc.scissor;
    int x = plat_f2i(floorf(a->sx)), y = plat_f2i(floorf(a->sy));
    int col[2][4];
    unsigned t, k;
    g_points++;
    if (x < sc->x0 || x > sc->x1 || y < sc->y0 || y > sc->y1) return;
    float tex[8][4];
    for (t = 0; t < 2; t++) {
        const float* ca = &a->col[t].r;
        for (k = 0; k < 4; k++) { int cv = plat_f2i(ca[k] * 255.0f + 0.5f); col[t][k] = cv < 0 ? 0 : (cv > 255 ? 255 : cv); }
    }
    for (t = 0; t < 8; t++) { tex[t][0] = a->tex[t][0]; tex[t][1] = a->tex[t][1]; tex[t][2] = a->tex[t][2]; tex[t][3] = 0.0f; }
    count_shaded(shade(D, x, y, col, tex, a->depth));
}

/* ---- clipping ----------------------------------------------------------- */

static void lerp_vertex(const Vertex* a, const Vertex* b, float t, Vertex* o)
{
    unsigned i, k;
    o->x = a->x + (b->x - a->x) * t;
    o->y = a->y + (b->y - a->y) * t;
    o->z = a->z + (b->z - a->z) * t;
    o->w = a->w + (b->w - a->w) * t;
    for (i = 0; i < 2; i++) {
        o->col[i].r = a->col[i].r + (b->col[i].r - a->col[i].r) * t;
        o->col[i].g = a->col[i].g + (b->col[i].g - a->col[i].g) * t;
        o->col[i].b = a->col[i].b + (b->col[i].b - a->col[i].b) * t;
        o->col[i].a = a->col[i].a + (b->col[i].a - a->col[i].a) * t;
    }
    for (i = 0; i < 8; i++) for (k = 0; k < 3; k++) o->tex[i][k] = a->tex[i][k] + (b->tex[i][k] - a->tex[i][k]) * t;
}

/* Clip space on this hardware: -w <= z <= 0 is visible, the volume Dolphin's
 * software clipper uses. A polygon is clipped against the near plane
 * (z + w >= 0), against w > 0, and against the far plane (z <= 0); the guard
 * band in x and y is still left to the scissor.
 *
 * The far plane used to be left to the scissor as well, which does not cover
 * it: a vertex past the far plane comes out of to_screen with a depth above
 * 1.0, depth_test clamps that to 0xFFFFFF, and LEQUAL against a buffer
 * cleared to 0xFFFFFF passes -- so geometry behind the far plane painted
 * over geometry in front of it.
 *
 * The slack below is load-bearing, and it is why the far test is not a bare
 * z <= 0. The game draws its whole 2D layer -- HUD, dialogue, menus, the
 * title -- as orthographic quads sitting exactly on the far plane: its ortho
 * projection leaves XF 0x1024 = -0.00999999978 and 0x1025 = -1.0 (near 0,
 * far 100) and the quads are at view z = -100, so z is fl(-0.00999999978 *
 * -100) - 1.0 and that product rounds to exactly 1.0f. Ten of the
 * twenty-three captured frames hold between 18 and 2652 such vertices, all of
 * them landing on z == 0.0f, which an inclusive test keeps. But the two terms
 * are a reciprocal and that reciprocal times a distance, computed separately
 * by the guest, so a scene whose numbers round the other way puts the 2D
 * layer one ulp past the plane -- and a bare z <= 0 would erase it. One ulp
 * at that scale is two units of the 24-bit depth buffer, so everything the
 * slack admits already has the deepest depth the buffer can hold and cannot
 * draw over anything the clip exists to protect. */
#define Z_FAR_SLACK (1.0f / 8388608.0f) /* 2^-23: one float32 ulp where the two projection terms cancel */

typedef enum { CLIP_NEAR, CLIP_W, CLIP_FAR } ClipPlane;

/* Signed distance to a clip plane, positive inside. Each one is linear in the
 * homogeneous coordinates, which is what makes the edge parameter below an
 * exact split of the edge rather than an approximation of one. */
static float clip_dist(const Vertex* v, ClipPlane plane)
{
    switch (plane) {
    case CLIP_NEAR: return v->z + v->w;
    case CLIP_W: return v->w - 1e-5f;
    default: return v->w * Z_FAR_SLACK - v->z;
    }
}

static unsigned clip_against(const Vertex* in, unsigned n, Vertex* out, ClipPlane plane)
{
    unsigned m = 0, i;
    for (i = 0; i < n; i++) {
        const Vertex* a = &in[i];
        const Vertex* b = &in[(i + 1) % n];
        float da = clip_dist(a, plane), db = clip_dist(b, plane);
        if (da >= 0.0f) out[m++] = *a;
        if ((da >= 0.0f) != (db >= 0.0f)) {
            float t = da / (da - db);
            lerp_vertex(a, b, t, &out[m++]);
        }
        if (m >= 14) break;
    }
    return m;
}

/* Whether the fast path may rasterize a vertex without running the clipper.
 * The w test is deliberately looser than the clipper's: this one asks only
 * that the vertex is in front of the eye, while the clipper's 1e-5 is there
 * to keep to_screen's reciprocal finite. Tightening it here would start
 * clipping triangles the renderer draws today, which is a separate question
 * from the far plane. */
static int vertex_unclipped(const Vertex* v)
{
    return clip_dist(v, CLIP_NEAR) >= 0.0f && v->w > 0.0f && clip_dist(v, CLIP_FAR) >= 0.0f;
}

static unsigned clip_polygon(Vertex* in, unsigned n, Vertex* out)
{
    Vertex tmp[16];
    n = clip_against(in, n, out, CLIP_NEAR);
    n = clip_against(out, n, tmp, CLIP_W);
    return clip_against(tmp, n, out, CLIP_FAR);
}

int gxr_vertex_unclipped(const Vertex* v) { return vertex_unclipped(v); }
unsigned gxr_clip_polygon(Vertex* in, unsigned n, Vertex* out) { return clip_polygon(in, n, out); }

static void emit_triangle(const DrawCmd* D, const Vertex* a, const Vertex* b, const Vertex* c)
{
    Vertex in[3], out[16];
    unsigned n, i;
    int inside = vertex_unclipped(a) && vertex_unclipped(b) && vertex_unclipped(c);
    if (g_debug > 1 && t_tid <= 1 && g_draw_no >= (unsigned)g_debug)
        fprintf(stderr, "[gxr] draw %u clip-space (%.3f,%.3f,%.3f,%.3f) (%.3f,%.3f,%.3f,%.3f) (%.3f,%.3f,%.3f,%.3f) tex0 (%.3f,%.3f) (%.3f,%.3f) (%.3f,%.3f)\n",
                g_draw_no, a->x, a->y, a->z, a->w, b->x, b->y, b->z, b->w, c->x, c->y, c->z, c->w,
                a->tex[0][0], a->tex[0][1], b->tex[0][0], b->tex[0][1], c->tex[0][0], c->tex[0][1]);
    if (inside) {
        in[0] = *a; in[1] = *b; in[2] = *c;
        for (i = 0; i < 3; i++) to_screen(&D->rc, &in[i]);
        raster_triangle(D, &in[0], &in[1], &in[2]);
        return;
    }
    in[0] = *a; in[1] = *b; in[2] = *c;
    n = clip_polygon(in, 3, out);
    if (n < 3) {
        if (t_tid <= 1) g_clipped++;
        if (g_debug && t_tid <= 1 && g_clipped <= 6)
            fprintf(stderr, "[gxr] clipped: (%.2f,%.2f,%.2f,%.2f) (%.2f,%.2f,%.2f,%.2f) (%.2f,%.2f,%.2f,%.2f)\n",
                    a->x, a->y, a->z, a->w, b->x, b->y, b->z, b->w, c->x, c->y, c->z, c->w);
        return;
    }
    for (i = 0; i < n; i++) to_screen(&D->rc, &out[i]);
    for (i = 1; i + 1 < n; i++) raster_triangle(D, &out[0], &out[i], &out[i + 1]);
}

static void run_copy(const DrawCmd* D);
static void run_copy_clear(const DrawCmd* D);
static void run_here(DrawCmd* D);
static void run_backend(const DrawCmd* D);
static void filter_sample(const DrawCmd* D, int sx, int sy, int ytop, int ybot, uint8_t* o);
static void workers_start(void);
void gxr_flush(void);

static void draw_command(const DrawCmd* D)
{
    const Vertex* v = D->v;
    unsigned count = D->count, i;
    if (D->kind == 1) { run_copy(D); return; }
    if (D->kind == 2) { run_copy_clear(D); return; }
    switch (D->prim) {
    case 0x80: /* quads */
        for (i = 0; i + 3 < count; i += 4) {
            emit_triangle(D, &v[i], &v[i + 1], &v[i + 2]);
            emit_triangle(D, &v[i], &v[i + 2], &v[i + 3]);
        }
        break;
    case 0x90: /* triangles */
        for (i = 0; i + 2 < count; i += 3) emit_triangle(D, &v[i], &v[i + 1], &v[i + 2]);
        break;
    case 0x98: /* strip */
        for (i = 2; i < count; i++) {
            if (i & 1) emit_triangle(D, &v[i - 1], &v[i - 2], &v[i]);
            else emit_triangle(D, &v[i - 2], &v[i - 1], &v[i]);
        }
        break;
    case 0xA0: /* fan */
        for (i = 2; i < count; i++) emit_triangle(D, &v[0], &v[i - 1], &v[i]);
        break;
    case 0xA8: /* lines: one thread only */
        if (t_tid > 1) break;
        for (i = 0; i + 1 < count; i += 2) {
            Vertex a = v[i], b = v[i + 1];
            if (a.w <= 0.0f || b.w <= 0.0f) continue;
            to_screen(&D->rc, &a); to_screen(&D->rc, &b);
            raster_line(D, &a, &b);
        }
        break;
    case 0xB0: /* line strip */
        if (t_tid > 1) break;
        for (i = 1; i < count; i++) {
            Vertex a = v[i - 1], b = v[i];
            if (a.w <= 0.0f || b.w <= 0.0f) continue;
            to_screen(&D->rc, &a); to_screen(&D->rc, &b);
            raster_line(D, &a, &b);
        }
        break;
    case 0xB8: /* points */
        if (t_tid > 1) break;
        for (i = 0; i < count; i++) {
            Vertex a = v[i];
            if (a.w <= 0.0f) continue;
            to_screen(&D->rc, &a);
            raster_point(D, &a);
        }
        break;
    default: break;
    }
}

/* ---- the queue and its workers ----------------------------------------- */

/* Commands are numbered rather than indexed, and the numbering is never reset.
 * Command n lives in slot n & QMASK. Three rules follow, and they are one rule
 * seen three ways:
 *
 *  - every count here has exactly one writer and only ever goes up.
 *    g_published is the producer's; g_ran[id] is worker id's. Nothing resets
 *    either, so a stale read is always too small, and too small can only make
 *    a thread wait longer than it had to.
 *  - a worker's decision to run a command reads exactly one word another
 *    thread writes -- g_published -- against a count it keeps to itself. There
 *    is no second shared load for a compiler to order against the first, which
 *    is the pair gxr_flush used to be racing when it rewound both of them.
 *  - gxr_flush leaves the numbering alone. It waits for every g_ran[] to reach
 *    g_published, which puts every worker back in its spin with nothing left
 *    to run, and only then recycles the vertex arena, the copy hazard list and
 *    the texture graveyard -- none of which any worker can still reach. So
 *    what the flush writes and what a running worker reads are disjoint sets,
 *    and that is checkable by finding the writes rather than by reasoning
 *    about where each thread is at the time.
 *
 * Two live commands would share a slot only if they were QUEUE_CAP apart, and
 * claim_slot waits for every worker to be past command n - QUEUE_CAP before it
 * builds command n over it -- a wait for that one command, which recycles
 * nothing. DrawCmd::seq is the check on that arithmetic rather than a comment
 * about it.
 *
 * Since H14 the workers also wait on each other, at fences. A command's fence
 * F holds each worker until every other worker's count has reached F, which
 * reads only those counts: the same one-writer, only-rising words, so checking
 * them one after another proves they all hold. F is never above the command's
 * own number, and a worker at command c has finished everything below c, so
 * the worker furthest behind is never held: no fence can deadlock. The
 * producer, for its part, only ever waits for commands already published.
 * What it waits for and why is in drain() and wait_ran(); only drain()
 * recycles, and it runs once a frame, at the next frame's first command (the
 * frame gate), unless something fills up first.
 *
 * Counted in 64 bits because nothing resets them and a single run is long:
 * build/boot_field.log records 138,619,966 draws in one process, and
 * build/boot_perf.log 3,176,974 over 4,210 presented frames -- 755 a frame,
 * which reaches the end of a signed 32-bit count in about a day of play.
 *
 * Every gxr_flush caller is on the thread that produces (the one parsing the
 * guest's command stream); the watchdog thread only calls gxr_report, which
 * does not flush. That is what lets the flush read g_published once and treat
 * it as fixed: it is the only writer. */
/* Why the producer waited (PLAN-60FPS-MODS H14), counted and timed under its
 * reason so the report can say which of them the wait is. The first group are
 * full drains, which recycle the arena, the copy list and the texture
 * graveyard; the second are waits for one command, which recycle nothing. */
enum {
    W_EXTERNAL,    /* drain: gxr_flush from outside -- a reset, a replay's end, SOA_FRAMES, a driver */
    W_ARENA,       /* drain: the vertex arena is full */
    W_GRAVE,       /* drain: the texture graveyard is full */
    W_PENDING,     /* drain: the list of copy destinations is full */
    W_GATE,        /* drain: the next frame's first command, the one full drain a frame */
    W_COPY_FIRST,  /* drain, SOA_GXR_DRAIN only: before a foreign copy, the frame's first */
    W_COPY_BEFORE, /* drain, SOA_GXR_DRAIN only: before a later one */
    W_COPY_AFTER,  /* drain, SOA_GXR_DRAIN only: after one */
    W_DRAWDONE,    /* drain: GXDrawDone, the CPU may read what was drawn */
    W_HASHPNG,     /* drain: SOA_HASH or a PNG wants the finished frame */
    W_RING,        /* wait: the ring would lap a command still queued */
    W_HAZARD,      /* wait: a texture read from a queued copy's destination (a drain under SOA_GXR_DRAIN) */
    W_TLUT,        /* wait: a palette loaded from one */
    W_SRC,         /* wait: vertex arrays, a display list or an indexed XF load read from one */
    W_HOOK,        /* wait: SOA_PEEK, SOA_POKE or a mod touching one */
    W_TOKEN,       /* wait: a draw token, which says the copies before it are done */
    W_LANDED,      /* wait: any of the above, for a copy run but not yet in guest RAM (lands_late, V7) */
    W_COUNT
};
static const char* const g_wait_name[W_COUNT] = {
    "external", "arena", "graveyard", "copy list", "gate", "copy-first", "copy-before", "copy-after",
    "drawdone", "hash/png", "ring", "hazard", "tlut", "source", "hook", "token", "landed"};
static uint64_t g_wait_n[W_COUNT], g_wait_ticks[W_COUNT]; /* producer only; read by the report */

static DrawCmd* g_queue;
static plat_a64 g_published;            /* commands published, ever */
static plat_a64 g_ran[MAX_THREADS + 1]; /* per worker: commands finished, ever */
static plat_a64 g_landed;               /* lands_late: every command below it has its RAM writes in place (V7) */

/* The queue's four ordering rules (docs/specs/portability.md 3.4; c8274db
 * applied the first two, L2 the rest). Every helper named is seq_cst, from
 * plat.h; on x64 each load is the plain mov it replaced, compared in the /FA
 * listing, and on ARM64 an LDAR, where a plain load would let a worker read a
 * command slot or a neighbour's rows stale (docs/specs/now.md N4).
 *  1. Publication: the producer fills a slot with plain stores, then
 *     plat_inc64(&g_published) publishes them; a worker reads the slot only
 *     after plat_load64(&g_published) is past it.
 *  2. Completion: a worker writes its rows, images and guest memory, then
 *     plat_xchg64(&g_ran[id], mine); anyone reads those only after
 *     plat_load64(&g_ran[i]) reaches the command. That covers wait_ran (the
 *     producer's texture, TLUT and vertex reads), fence_wait (the neighbours'
 *     rows and copy images), drain (the arena, graveyard, hash and PNG) and
 *     gxr_presented (the UI thread's g_screen).
 *  3. Sleepers, a Dekker pair: the waiter (an idle worker, a fence waiter)
 *     does plat_inc32(&sleepers) and then plat_load64(&count), sleeping only
 *     if the count is unchanged; the waker (publish, a worker finishing a
 *     command) raises the count and then plat_load32(&sleepers), waking only
 *     if there are any. All four seq_cst, so at least one side sees the
 *     other's write; the 50 ms bound on a wait is the backstop.
 *  4. Every cross-thread read goes through plat_load*. The producer's reads
 *     of its own g_published stay plain -- it is the only writer -- and each
 *     carries the marker own count; test_gxr_atomics.py checks every use.
 *  5. Landing (V7), for a backend that lands copies late: it writes a copy's
 *     guest memory, then plat_xchg64(&g_landed, ...); the producer reads that
 *     memory only after plat_load64(&g_landed) is past the copy (wait_landed,
 *     drain). g_ran then says only that a command has run. */
static uint64_t g_flushes;                       /* producer only: drains, so a nested one can be seen */
static uint8_t* g_arena;
static size_t g_arena_used;
static int g_workers; /* worker threads; the main thread (tid 0) only produces */
/* Where a draw's TEV setup is built, before the slot it will be queued in has
 * been chosen. Only the thread that parses the command stream touches it. */
static TevSetup g_prep;
static uint64_t g_prepare_hazards; /* draws whose setup read a texture a queued copy writes */

/* H14's producer-side state, all producer only. g_fence_after is the fence
 * the next command published must carry: a copy that read rows other workers
 * own is not over until every worker has finished it. g_frame_gate says a
 * screen copy has been published and the next frame's first command must
 * drain first -- the one full drain a frame. g_ran_floor is a lower bound on
 * ran_min(), so claiming a slot does not read every worker's count every time.
 * g_last_copy is the newest copy of any kind, which a token waits for. */
static long long g_fence_after, g_fence_after_near, g_ran_floor, g_last_copy = -1;
static int g_frame_gate;
static uint64_t g_hazard_hits, g_tokens_waited;

/* Workers asleep in plat_wait64 on g_published (PLAN-60FPS-MODS H11). The
 * producer wakes them only when there are any, so a burst of draws with the
 * pool awake costs one more load per command, and no call. */
static plat_a32 g_sleepers;

/* Publish one command to the pool, waking it if it has gone to sleep. */
static void publish(void)
{
    plat_inc64(&g_published);
    if (plat_load32(&g_sleepers)) plat_wake_all64(&g_published);
}

/* The oldest command some worker has not finished: every worker has finished
 * every command below it. With no workers every command ran as it was
 * published. A stale count is too small, so this can only be too small. */
static long long ran_min(void)
{
    long long m = g_published; /* own count */
    int i;
    for (i = 1; i <= g_workers; i++) {
        long long r = plat_load64(&g_ran[i]);
        if (r < m) m = r;
    }
    return m;
}

/* The producer waits until every worker has finished command c. Nothing is
 * recycled: this is a wait for one command, not a drain, and what it lets the
 * producer do next -- reuse that command's slot, or read memory that command
 * wrote -- names nothing a drain hands out again. */
static void wait_ran(long long c, int why)
{
    unsigned spins = 0;
    int i, prev;
    uint64_t t0, t1;
    if (g_backend && g_backend->finish && !g_workers) g_backend->finish(); /* the producer's backend only: V6a's thread finishes its own */
    if (c >= g_published) { /* own count */
        WARN_ONCE("[gxr] a wait for command %lld, which is not published yet (%lld are): nothing to wait for, so no wait\n", c, (long long)g_published); /* own count */
        return;
    }
    if (ran_min() > c) return;
    prev = gxr_phase(T_WAIT);
    t0 = gxr_ticks();
    for (i = 1; i <= g_workers; i++)
        while (plat_load64(&g_ran[i]) <= c) { if (++spins > 4000) { plat_yield(); spins = 0; } else plat_relax(); }
    t1 = gxr_ticks();
    g_wait_n[why]++;
    if (t1 > t0) g_wait_ticks[why] += t1 - t0;
    gxr_phase(prev);
    plat_compiler_barrier(); /* what the command wrote is read after the wait, not hoisted above it */
}

/* For a backend that lands copies late (V7, 3.6): after command c has run,
 * wait until what it wrote is in guest RAM as well. The consumer lands what it
 * holds when it runs out of commands, which it does while the producer waits
 * here, publishing nothing. Counted under W_LANDED, whatever the wait was
 * for, so the report says how often a landing was waited on. */
static int lands_late(void)
{
    return g_backend && g_backend->lands_late;
}

static void wait_landed_only(long long c)
{
    unsigned spins = 0;
    int prev;
    uint64_t t0, t1;
    if (!lands_late() || plat_load64(&g_landed) > c) return;
    prev = gxr_phase(T_WAIT);
    t0 = gxr_ticks();
    while (plat_load64(&g_landed) <= c) { if (++spins > 4000) { plat_yield(); spins = 0; } else plat_relax(); }
    t1 = gxr_ticks();
    g_wait_n[W_LANDED]++;
    if (t1 > t0) g_wait_ticks[W_LANDED] += t1 - t0;
    gxr_phase(prev);
    plat_compiler_barrier(); /* what the copy wrote is read after the wait, not hoisted above it */
}

static void wait_landed(long long c, int why)
{
    wait_ran(c, why);
    if (c < g_published) wait_landed_only(c); /* own count */
}

/* The oldest command whose RAM writes may not all be in place: ran_min, or
 * below it for a backend that lands late. */
static long long done_min(void)
{
    long long m = ran_min();
    if (lands_late()) {
        long long l = plat_load64(&g_landed);
        if (l < m) m = l;
    }
    return m;
}

void gxr_backend_landed(long long through)
{
    plat_xchg64(&g_landed, through);
}

/* The one place a slot is taken. Command n goes in slot n & QMASK, which held
 * command n - QUEUE_CAP: wait until every worker is past that one, and no
 * further. The fence is the larger of what the caller asks and what the last
 * copy left for the command after it, and never above the command itself, so
 * the worker furthest behind can always run the command it is on; the same
 * for the neighbours' fence, kept only where it asks more than the other. */
/* Which EFB the commands built from now on are for: 0 the real frame, 1
 * H17's in-between image. The producer's alone; claim_slot copies it into
 * each command, which is all any consumer reads (3.1). */
static int g_target;

void gxr_set_target(int efb)
{
    g_target = efb ? 1 : 0;
}

static DrawCmd* claim_slot(int kind, long long want, long long want_near)
{
    long long n = g_published, fence, nbr; /* own count */
    DrawCmd* D;
    if (n - g_ran_floor >= QUEUE_CAP) {
        g_ran_floor = ran_min();
        if (n - g_ran_floor >= QUEUE_CAP) {
            wait_ran(n - QUEUE_CAP, W_RING);
            g_ran_floor = ran_min();
        }
    }
    D = &g_queue[n & QMASK];
    D->seq = g_published; /* own count */
    D->kind = kind;
    D->efb = (uint8_t)g_target;
    fence = want > g_fence_after ? want : g_fence_after;
    if (fence > n) {
        WARN_ONCE("[gxr] command %lld asked for a fence at %lld, past itself; held to its own number\n", n, fence);
        fence = n;
    }
    D->fence = fence;
    nbr = want_near > g_fence_after_near ? want_near : g_fence_after_near;
    if (nbr > n) nbr = n;
    D->fence_near = nbr > fence ? nbr : 0;
    g_fence_after = g_fence_after_near = 0;
    return D;
}

/* Workers parked at a fence in plat_wait64 on another worker's count. A
 * worker raising its count wakes them only when there are any, the pattern
 * H11 gave the idle spin. */
static plat_a32 g_fence_sleepers;

/* Hold this worker until every other worker has finished every command below
 * f. Each check reads one count that only its owner writes and that only
 * rises, so checking them one after another proves they all hold at the end
 * -- C0's rule: a stale read is too small and only makes a thread wait
 * longer. A worker waiting here has finished everything below its own
 * command, and every fence is at most its command's number, so the worker
 * furthest behind never waits: no fence can deadlock. The time is idle, and
 * counted apart as fence time.
 *
 * A short spin, then a sleep on the count being waited for: Sleep(0) returns
 * at once when no other thread is ready, so a fence that spun on it cost a
 * core per waiting worker, 12-13% more CPU over H1's Part L run (FINDINGS
 * "H14"). The sleeper count goes up before the count is read again and the
 * worker that raises it reads the sleepers after, all four seq_cst (rule 3
 * above), so a rise in between is either seen here or wakes the wait; the
 * 50 ms bound is the backstop. */
static void fence_wait(ThreadState* W, int self, long long all, long long nbr)
{
    int j, prev = self > 1 ? self - 1 : g_workers, next = self < g_workers ? self + 1 : 1;
    unsigned spins = 0;
    uint64_t idle0;
    charge(W, &W->busy);
    idle0 = W->idle;
    for (j = 1; j <= g_workers; j++) {
        long long f = (j == prev || j == next) && nbr > all ? nbr : all;
        if (j == self) continue;
        while (plat_load64(&g_ran[j]) < f) {
            if (++spins > 4000) {
                int64_t seen = plat_load64(&g_ran[j]);
                plat_inc32(&g_fence_sleepers);
                if (plat_load64(&g_ran[j]) == seen && seen < f) plat_wait64(&g_ran[j], seen, 50);
                plat_dec32(&g_fence_sleepers);
                spins = 0;
            } else {
                plat_relax();
            }
        }
    }
    charge(W, &W->idle);
    W->fence_ticks += W->idle - idle0;
    W->fences++;
    plat_compiler_barrier(); /* the compiler's half: the load above is the machine's */
}

static void worker(void* arg)
{
    int id = (int)(intptr_t)arg; /* 1..workers */
    /* This worker's own count, kept here rather than read back out of g_ran so
     * the spin below has one shared word in it. Zero because the pool is
     * created before the first command is published. */
    long long mine = 0;
    ThreadState* W = &g_ts[id];
    t_tid = id;
    /* Every worker's clock starts when the pool did, so busy + idle can be
     * compared against one span for the whole pool. */
    W->last = g_pool_t0;
    for (;;) {
        const DrawCmd* D;
        unsigned spins = 0;
        while (mine >= plat_load64(&g_published)) {
            /* A short spin, for the next command of a burst, then sleep
             * until the producer publishes (H11). This used to be
             * Sleep(0), which returns at once when no other thread is
             * ready, so an idle pool cost a core per worker at any load:
             * 8.4-8.9 cores with 8 workers whether a frame was drawn or
             * not (FINDINGS "H3"). The sleeper count goes up before
             * g_published is read again and the producer reads it after
             * publishing, all four seq_cst (rule 3 above), so a publish in
             * between is either seen here or wakes the wait. Charging the idle clock
             * on every pass, and the wait's 50 ms bound, are what let a
             * pool parked for twenty seconds under the watchdog still add
             * up to the span the report divides by. */
            if (++spins > 4000) {
                int64_t seen = mine;
                /* A backend that lands late lands here (V7): a producer
                 * waiting for a landing publishes nothing, so this is where
                 * the wait is answered. */
                if (g_backend && g_backend->idle) {
                    charge(W, &W->idle);
                    g_backend->idle();
                    charge(W, &W->busy);
                    if (mine < plat_load64(&g_published)) { spins = 0; continue; }
                }
                charge(W, &W->idle);
                plat_inc32(&g_sleepers);
                if (plat_load64(&g_published) == seen) plat_wait64(&g_published, seen, 50);
                plat_dec32(&g_sleepers);
                spins = 0;
            } else {
                plat_relax();
            }
        }
        charge(W, &W->idle);
        /* The producer fills a slot before it publishes the count, and the
         * count was read with plat_load64 (rule 1 above), so the command is
         * there on any machine -- this one does not reorder two loads, and an
         * ARM64 would without it. The barrier is against the compiler alone,
         * stopping it from reading the command's fields before the spin
         * ends; it emits nothing. */
        plat_compiler_barrier();
        D = &g_queue[mine & QMASK];
        if (D->seq != mine)
            WARN_ONCE("[gxr] queue slot %lld holds command %lld, not command %lld, which is the one this worker is on: the producer got %d commands ahead of it without draining and built over it, so the command is skipped and this frame is wrong\n",
                      mine & QMASK, D->seq, mine, QUEUE_CAP);
        else {
#ifdef GXR_MUTATE_COUNT_EARLY
            /* test_gxv_queue.py's mutation, built only into a variant of the GPU
             * spike: the command counted before it runs, so the producer may
             * recycle its vertices while a stalled consumer has yet to read them
             * -- and, for a backend that lands late (V7), its landing too, which
             * the drains now also wait for. */
            plat_xchg64(&g_ran[id], mine + 1);
            if (g_backend && g_backend->lands_late) plat_xchg64(&g_landed, mine + 1);
#endif
            if ((D->fence > 0 || D->fence_near > 0) && g_workers > 1) fence_wait(W, id, D->fence, D->fence_near);
            if (g_stall_n) stall(id, D->kind);
            /* With a backend of its own thread this is that thread, the
             * ring's one consumer (V6a, 3.7): the command is done, its
             * vertices and textures uploaded and a copy's bytes in guest
             * RAM, before the count below says so. */
            if (g_backend) run_backend(D);
            else draw_command(D);
        }
        /* The two readings bracketing the command are inside what they
         * measure, so a command this worker owns no rows of is charged their
         * cost -- about 14 ns against a command that does nothing. The report
         * says so rather than hide it. */
        charge(W, &W->busy);
        mine++;
        plat_xchg64(&g_ran[id], mine);
        if (plat_load32(&g_fence_sleepers)) plat_wake_all64(&g_ran[id]); /* a worker parked at a fence on this count */
    }
}

/* ---- SOA_HOSTPROF: where the host's time goes, by function and line -------
 *
 * The profile main.c prints samples the guest thread, and reads what it was
 * running from words that thread writes; a worker writes none, so the pixel
 * path was measurable only as a total, and the guest thread's own table stops
 * at the guest function -- a psq_load's ldexp, an irq_poll or a memory
 * accessor inside it is charged to the function that called it. With
 * SOA_HOSTPROF=1 a thread suspends each worker and the guest thread about once
 * a millisecond, reads its instruction pointer and lets it go, and the report
 * resolves the samples to functions and source lines through gen/soa.pdb
 * (dbghelp): the workers in one table, the guest thread in another, where a
 * translated function is its fn_XXXXXXXX. Suspending a thread costs it a few
 * microseconds a millisecond, well under the drift this machine has between
 * runs; a profiled run is for reading, not for timing. A sample in the kernel
 * or a wait counts under the function the thread was waiting in. */
#ifdef _WIN32
#include <dbghelp.h>
#include <process.h>
#pragma comment(lib, "dbghelp.lib")
#define HP_SLOTS 65536u
typedef struct {
    uint64_t rip[HP_SLOTS];
    uint32_t count[HP_SLOTS];
    uint64_t samples, lost;
} HpTable;
static HANDLE g_worker_handle[MAX_THREADS + 1];
static HANDLE g_guest_handle; /* the thread that started the pool: the guest's, which parses and sets up draws */
static HpTable g_hp_workers, g_hp_guest;
static volatile LONG g_hp_on;

static void hp_add(HpTable* t, uint64_t rip)
{
    uint32_t h = (uint32_t)((rip * 0x9E3779B97F4A7C15ull) >> 48) & (HP_SLOTS - 1), i;
    for (i = 0; i < 64; i++) {
        uint32_t k = (h + i) & (HP_SLOTS - 1);
        if (t->rip[k] == rip) { t->count[k]++; return; }
        if (!t->rip[k]) { t->rip[k] = rip; t->count[k] = 1; return; }
    }
    t->lost++;
}

static void hp_sample(HpTable* t, HANDLE h)
{
    CONTEXT ctx;
    if (!h || SuspendThread(h) == (DWORD)-1) return;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext(h, &ctx)) {
        hp_add(t, ctx.Rip);
        t->samples++;
    }
    ResumeThread(h);
}

static unsigned __stdcall hostprof_thread(void* arg)
{
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* high resolution */, TIMER_ALL_ACCESS);
    int n = (int)(intptr_t)arg, i;
    for (;;) {
        LARGE_INTEGER due;
        due.QuadPart = -10000; /* 1 ms */
        if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, 100);
        else Sleep(1);
        for (i = 1; i <= n; i++) hp_sample(&g_hp_workers, g_worker_handle[i]);
        hp_sample(&g_hp_guest, g_guest_handle);
    }
}

static void hostprof_start(int n)
{
    const char* env = getenv("SOA_HOSTPROF");
    if (!env || !atoi(env) || n < 1) return;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_guest_handle,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        g_guest_handle = NULL;
    g_hp_on = 1;
    _beginthreadex(NULL, 0, hostprof_thread, (void*)(intptr_t)n, 0, NULL);
    fprintf(stderr, "[hostprof] sampling %d worker thread%s%s every millisecond\n", n, n == 1 ? "" : "s",
            g_guest_handle ? " and the guest thread" : "");
}

typedef struct {
    char name[160];
    uint64_t count;
} HpRow;

static int hp_row_cmp(const void* a, const void* b)
{
    uint64_t x = ((const HpRow*)a)->count, y = ((const HpRow*)b)->count;
    return x < y ? 1 : (x > y ? -1 : 0);
}

static int hp_name_cmp(const void* a, const void* b)
{
    return strcmp(((const HpRow*)a)->name, ((const HpRow*)b)->name);
}

/* Fold the samples by key and print the top rows: mode 0 the function the
 * address is in, 1 the innermost function inlined there, 2 that and its line.
 * One row per sampled address, sorted by key and merged, because the guest
 * thread's translated code has far more distinct lines than a table searched
 * row by row could afford; an inlined helper sampled at several addresses
 * still folds into its one source line. */
static void hp_print(const HpTable* t, int mode, int rows)
{
    HpRow* table = (HpRow*)malloc(sizeof(HpRow) * HP_SLOTS);
    int nrows = 0, merged = 0, i;
    uint32_t k;
    HANDLE proc = GetCurrentProcess();
    if (!table) return;
    for (k = 0; k < HP_SLOTS; k++) {
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* sym = (SYMBOL_INFO*)buf;
        char* key = table[nrows].name;
        size_t cap = sizeof table[nrows].name;
        DWORD64 disp = 0;
        if (!t->rip[k]) continue;
        memset(buf, 0, sizeof buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        if (!SymFromAddr(proc, t->rip[k], &disp, sym)) snprintf(key, cap, "?? %llx", (unsigned long long)t->rip[k]);
        else if (mode) {
            /* The innermost inlined frame when there is one: the pixel path's
             * helpers are __forceinline, and without this every sample in them
             * is charged to the line that calls them. */
            IMAGEHLP_LINE64 line;
            DWORD ldisp = 0, ictx = 0, iframe = 0;
            BOOL got;
            memset(&line, 0, sizeof line);
            line.SizeOfStruct = sizeof line;
#ifndef __MINGW32__
            if (SymAddrIncludeInlineTrace(proc, t->rip[k]) &&
                SymQueryInlineTrace(proc, t->rip[k], 0, t->rip[k], t->rip[k], &ictx, &iframe)) {
                DWORD64 idisp = 0;
                if (SymFromInlineContext(proc, t->rip[k], ictx, &idisp, sym)) disp = idisp;
                got = SymGetLineFromInlineContext(proc, t->rip[k], ictx, 0, &ldisp, &line);
            } else
#else
            /* mingw-w64's dbghelp.h has no inline-frame API (distribution R1):
             * there a sample is charged to the line that calls the inline. */
            (void)ictx;
            (void)iframe;
#endif
            {
                got = SymGetLineFromAddr64(proc, t->rip[k], &ldisp, &line);
            }
            if (mode == 1) {
                snprintf(key, cap, "%s", sym->Name);
            } else if (got) {
                const char* file = strrchr(line.FileName, '\\');
                snprintf(key, cap, "%s %s:%lu", sym->Name, file ? file + 1 : line.FileName, (unsigned long)line.LineNumber);
            } else snprintf(key, cap, "%s +%llx", sym->Name, (unsigned long long)disp);
        } else snprintf(key, cap, "%s", sym->Name);
        table[nrows++].count = t->count[k];
    }
    qsort(table, (size_t)nrows, sizeof table[0], hp_name_cmp);
    for (i = 0; i < nrows; i++) {
        if (merged && !strcmp(table[merged - 1].name, table[i].name)) table[merged - 1].count += table[i].count;
        else table[merged++] = table[i];
    }
    qsort(table, (size_t)merged, sizeof table[0], hp_row_cmp);
    for (i = 0; i < merged && i < rows; i++)
        fprintf(stderr, "[hostprof] %5.1f%%  %s\n", 100.0 * (double)table[i].count / (double)t->samples, table[i].name);
    free(table);
}

static void hostprof_report(void)
{
    if (!g_hp_on || !(g_hp_workers.samples + g_hp_guest.samples)) return;
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (!SymInitialize(GetCurrentProcess(), NULL, TRUE)) {
        fprintf(stderr, "[hostprof] %llu samples, but dbghelp would not start, so no names\n",
                (unsigned long long)(g_hp_workers.samples + g_hp_guest.samples));
        return;
    }
    fprintf(stderr, "[hostprof] %llu samples of the workers (%llu past a full table); by function:\n",
            (unsigned long long)g_hp_workers.samples, (unsigned long long)g_hp_workers.lost);
    hp_print(&g_hp_workers, 0, 20);
    fprintf(stderr, "[hostprof] by the function inlined there:\n");
    hp_print(&g_hp_workers, 1, 30);
    fprintf(stderr, "[hostprof] by line:\n");
    hp_print(&g_hp_workers, 2, 40);
    if (g_hp_guest.samples) {
        fprintf(stderr, "[hostprof] %llu samples of the guest thread (%llu past a full table); by function:\n",
                (unsigned long long)g_hp_guest.samples, (unsigned long long)g_hp_guest.lost);
        hp_print(&g_hp_guest, 0, 40);
        fprintf(stderr, "[hostprof] by line:\n");
        hp_print(&g_hp_guest, 2, 40);
    }
    SymCleanup(GetCurrentProcess());
}
#else
static void hostprof_start(int n)
{
    const char* env = getenv("SOA_HOSTPROF");
    (void)n;
    if (env && atoi(env)) fprintf(stderr, "[hostprof] SOA_HOSTPROF is Windows-only here (it suspends threads and reads gen/soa.pdb); not sampling\n");
}
static void hostprof_report(void) {}
#endif

static const GxrBackend g_passthrough;

static void workers_start(void)
{
    const char* env = getenv("SOA_THREADS");
    const char* inl = getenv("SOA_GXR_INLINE");
    const char* be = getenv("SOA_GXR_BACKEND");
    int n = env ? atoi(env) : 0, i, inline_only;
    /* SOA_GXR_BACKEND names a backend built in, when none was set by
     * code; the passthrough is the only one in V2. */
    if (!g_backend && be && *be) {
        if (!strcmp(be, "passthrough")) g_backend = &g_passthrough;
        else fprintf(stderr, "[gxr] SOA_GXR_BACKEND=%s is not a backend this build has; the worker pool draws\n", be);
    }
    /* Zero workers: every command runs on the producer as it is built --
     * through a backend without a thread of its own, when there is one. The
     * path V2 made reachable and SOA_GXR_INLINE=1 proves, and which
     * SOA_GXR_INLINE=1 forces on a backend that has one too (V5's way). */
    inline_only = (g_backend && !g_backend->own_thread) || (inl && atoi(inl));
    /* Before the threads, so every one of them starts its busy/idle clock at
     * the same reading the report measures the pool's span from. */
    if (g_gxr_tsc < 0) gxr_timing_init();
    g_pool_t0 = gxr_ticks();
    g_queue = (DrawCmd*)malloc(sizeof(DrawCmd) * QUEUE_CAP);
    g_arena = (uint8_t*)malloc(ARENA_BYTES);
    if (inline_only) n = 0;
    else if (g_backend) n = 1; /* the backend's own thread: the ring's one consumer (V6a, 3.7) */
    else if (n <= 0) {
        /* Three quarters of the logical CPUs (FINDINGS "H15c"). On the
         * 16-thread machine this was measured on, 12 workers ran the Dangral
         * base at 27 fps against 25 at 10, 24 at 14 and 19 at the old half;
         * the rest of the machine is the guest thread, the audio and the
         * window, and fifteen workers took the guest thread's core. */
        n = plat_cpu_count() * 3 / 4;
        if (n < 1) n = 1;
    }
    if (n > MAX_THREADS) n = MAX_THREADS;
    for (i = 1; i <= n && !inline_only; i++) {
        PlatThread t;
        if (!plat_thread_start(&t, worker, (void*)(intptr_t)i, 0)) { n = i - 1; break; }
#ifdef _WIN32
        g_worker_handle[i] = (HANDLE)t.os; /* kept for SOA_HOSTPROF, which samples through it */
#endif
    }
    hostprof_start(n);
    g_workers = n;
    g_nthreads = n > 0 ? n : 1;
    fprintf(stderr, "[gxr] rasterizing on %d worker thread%s\n", n, n == 1 ? "" : "s");
    if (g_backend)
        fprintf(stderr, "[gxr] the %s backend draws every command, %s\n", g_backend->name,
                n ? "on a thread of its own" : "on the producer");
}

static int g_pending_n; /* queued copy destinations (defined with the copies below) */
static int g_started;   /* worker pool created */

static void drain(int why);

/* Wait for every queued draw to finish, then recycle the queue. */
void gxr_flush(void)
{
    drain(W_EXTERNAL);
}

/* Nothing below the wait is written that a worker reads: the numbering is left
 * where it is and only producer-private storage is handed out again. A worker
 * that has reached g_published cannot run anything else, because the thread
 * inside this function is the only one that publishes. */
static void drain(int why)
{
    long long target;
    unsigned spins = 0;
    int i, prev;
    uint64_t t0;
    if (!g_queue) return;
    if (g_backend && g_backend->finish && !g_workers) g_backend->finish(); /* the producer's backend only, as in wait_ran */
    /* Read once: this thread is the only writer, so the target cannot move. */
    target = g_published; /* own count */
    /* The wait is the producer's idle, and it used to be charged to whichever
     * of draw, prepare and copies happened to enclose the call -- and to
     * nothing at all from GXDrawDone. It is its own bucket now, and it is the
     * number that says whether the guest thread or the rasterizer is the one
     * holding the run up. */
    prev = gxr_phase(T_WAIT);
    t0 = gxr_ticks();
    /* Backing off matters here for the reason it does in the worker's own
     * spin, which this copies: at SOA_THREADS near the core count the producer
     * and the workers compete for the same cores, and a bare plat_relax()
     * takes one away from the very threads being waited on. With four of these
     * processes sharing sixteen cores, 400 flushes of a full-screen draw at
     * SOA_THREADS=16 cost 3.1-3.5s and 12-15s of CPU each without it, and
     * 1.5-1.6s and 5.6-7.4s with it. */
    for (i = 1; i <= g_workers; i++)
        while (plat_load64(&g_ran[i]) < target) { if (++spins > 4000) { plat_yield(); spins = 0; } else plat_relax(); }
    {
        uint64_t t1 = gxr_ticks();
        g_wait_n[why]++;
        if (t1 > t0) g_wait_ticks[why] += t1 - t0;
    }
    gxr_phase(prev);
    /* Every copy's bytes in guest RAM too, for a backend that lands late:
     * what a drain hands out again, and what the CPU may read after one. */
    if (g_last_copy >= 0) wait_landed_only(g_last_copy);
    g_flushes++;
    g_frame_gate = 0;
    g_ran_floor = target;
    g_arena_used = 0;
    g_pending_n = 0;
    tex_graveyard_empty();
}

/* ---- frame pairs (PLAN-60FPS-MODS H10) ----------------------------------
 *
 * The in-between image of two consecutive frames, offline: `--replay F F+1`
 * renders F recording each draw's key and its vertices' clip-space positions,
 * renders F+1 as it is, then renders F+1 again with every draw matched to one
 * of F moved to the point between -- (1-t)*F + t*F+1 of each x, y, z, w -- and
 * everything else drawn as F+1 draws it. The match is tools/fifopair.py's
 * exactly (FINDINGS "H4"): the key is a draw's display list, the arrays its
 * vertex layout indexes, the textures its stages sample, its primitive, its
 * vertex count and its TEV setup (not the TEV colour and konst registers, so a
 * fade stays one draw); within a key, the oldest unmatched draw of F.
 *
 * What is kept is ARCHITECTURE section 12, the layout the live path (H16,
 * H17a) inherits: per frame, a 32-byte record per keyed draw, an open-addressed
 * key table, and 16 bytes of position per vertex -- outside the vertex arena,
 * the queue and the texture graveyard, owned by the producer thread, rotated
 * only at the screen copy, and never touched by a drain. Nothing is allocated
 * unless pair mode is switched on, and with it off the draw path adds one
 * untaken branch. Colours and texture coordinates are F+1's: positions only. */
#define PAIR_DRAWS 16384u
#define PAIR_SLOTS 32768u /* a power of two */
#define PAIR_VERTS 262144u
#define PAIR_NONE 0xFFFFFFFFu

typedef struct {
    uint64_t key;
    uint32_t count, off, next, idx; /* idx: its number among the frame's draws, as fifopair numbers them */
    uint8_t prim, ortho;
} PairRec;

typedef struct {
    uint64_t key;
    uint32_t count, head, tail; /* count 0: an empty slot; head: the oldest record not yet matched */
} PairSlot;

typedef struct {
    PairRec* rec;
    uint32_t nrec;
    PairSlot* slot;
    float (*pos)[4];
    uint32_t npos;
} PairFrame;

typedef struct {
    uint32_t cur_off, prev_off;
} PairClaim;

static PairFrame g_pair[2];
static int g_pair_cur;
static unsigned g_pair_flags;
static float g_pair_t = 0.5f;
static FILE* g_pair_list;
static uint32_t g_pair_ndraw; /* keyed draws so far in this frame */
static unsigned long long g_pair_rotations, g_pair_matched, g_pair_unmatched, g_pair_demoted, g_pair_over;
static unsigned long long g_pair_verts, g_pair_skipped, g_pair_kept, g_pair_over32;
static double g_pair_maxdisp;
static uint32_t g_pair_peak_draws, g_pair_peak_verts;
static uint64_t g_pair_list_hash = 1469598103934665603ull;

static uint64_t pair_mix(uint64_t h, uint32_t w)
{
    h ^= w;
    h *= 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 29);
}

static uint64_t pair_key(const uint32_t* cp, const uint32_t* bp, unsigned prim, unsigned count)
{
    uint64_t h = 0x243F6A8885A308D3ull;
    uint32_t list_addr, n = 0;
    unsigned i, st, stages = ((bp[0x00] >> 10) & 15) + 1;
    int in_list = gx_draw_list(&list_addr);
    h = pair_mix(h, (uint32_t)in_list);
    h = pair_mix(h, in_list ? list_addr : 0);
    for (i = 0; i < 12; i++) { /* arrays: pos, nrm, clr0, clr1, tex0-7, as fifo.vertex_layout orders them */
        unsigned mode = i < 4 ? (cp[0x50] >> (9 + 2 * i)) & 3 : (cp[0x60] >> (2 * (i - 4))) & 3;
        if (mode < 2) continue;
        h = pair_mix(pair_mix(h, i), cp[0xA0 + i] & 0x1FFFFFFFu);
        n++;
    }
    h = pair_mix(h, n);
    for (n = 0, st = 0; st < stages; st++) { /* textures: each enabled stage's image address */
        uint32_t tref = (bp[0x28 + st / 2] >> (12 * (st & 1))) & 0xFFF, m, rb;
        if (!((tref >> 6) & 1)) continue;
        m = tref & 7;
        rb = m < 4 ? m : 0x20 + (m - 4);
        h = pair_mix(h, (bp[0x94 + rb] & 0x1FFFFFu) << 5);
        n++;
    }
    h = pair_mix(h, n);
    h = pair_mix(pair_mix(h, prim), count);
    h = pair_mix(h, bp[0x00] & 0x73C7Fu); /* tev: as fifopair.tev_hash */
    for (st = 0; st < stages; st++) {
        h = pair_mix(h, (bp[0x28 + st / 2] >> (12 * (st & 1))) & 0xFFF);
        h = pair_mix(h, bp[0xC0 + 2 * st] & 0xFFFFFFu);
        h = pair_mix(h, bp[0xC1 + 2 * st] & 0xFFFFFFu);
    }
    for (i = 0xF6; i < 0xFE; i++) h = pair_mix(h, bp[i] & 0xFFFFFFu);
    return pair_mix(h, bp[0xF3] & 0xFFFFFFu);
}

/* The slot for (key, count), or where it would go; NULL when the table is full. */
static PairSlot* pair_slot(PairFrame* F, uint64_t key, uint32_t count)
{
    uint32_t i, n;
    for (i = (uint32_t)(key ^ (key >> 32)) & (PAIR_SLOTS - 1), n = 0; n < PAIR_SLOTS; i = (i + 1) & (PAIR_SLOTS - 1), n++) {
        PairSlot* sl = &F->slot[i];
        if (sl->count == 0 || (sl->key == key && sl->count == count)) return sl;
    }
    return NULL;
}

static void pair_reset(PairFrame* F)
{
    F->nrec = 0;
    F->npos = 0;
    memset(F->slot, 0, sizeof(PairSlot) * PAIR_SLOTS);
}

/* RECORD: note this draw as frame F's. LERP: find its match in the frame
 * before, oldest first within the key, and say where that frame's positions
 * are. A match whose projection kind differs (a 2D draw that was 3D, or the
 * other way) is written to the list, consumed and demoted: F+1 draws it. */
static PairClaim pair_claim(uint64_t key, unsigned prim, unsigned count, int ortho)
{
    PairClaim c = {PAIR_NONE, PAIR_NONE};
    uint32_t idx = g_pair_ndraw++;
    if (g_pair_flags & GXR_PAIR_LERP) {
        PairFrame* P = &g_pair[g_pair_cur ^ 1];
        PairSlot* sl = pair_slot(P, key, count);
        if (sl && sl->count && sl->head != PAIR_NONE) {
            PairRec* r = &P->rec[sl->head];
            sl->head = r->next;
            if (g_pair_list) fprintf(g_pair_list, "%u %u\n", r->idx, idx);
            g_pair_list_hash = pair_mix(pair_mix(g_pair_list_hash, r->idx), idx);
            if (r->ortho != (uint8_t)ortho) {
                g_pair_demoted++;
            } else {
                c.prev_off = r->off;
                g_pair_matched++;
            }
        } else {
            g_pair_unmatched++;
        }
    }
    if (g_pair_flags & GXR_PAIR_RECORD) {
        PairFrame* C = &g_pair[g_pair_cur];
        PairSlot* sl = C->nrec < PAIR_DRAWS && C->npos + count <= PAIR_VERTS ? pair_slot(C, key, count) : NULL;
        if (!sl) {
            g_pair_over++;
        } else {
            uint32_t r = C->nrec++;
            C->rec[r].key = key;
            C->rec[r].count = count;
            C->rec[r].off = C->npos;
            C->rec[r].next = PAIR_NONE;
            C->rec[r].idx = idx;
            C->rec[r].prim = (uint8_t)prim;
            C->rec[r].ortho = (uint8_t)ortho;
            if (!sl->count) {
                sl->key = key;
                sl->count = count;
                sl->head = sl->tail = r;
            } else {
                C->rec[sl->tail].next = r;
                sl->tail = r;
            }
            c.cur_off = C->npos;
            C->npos += count;
        }
    }
    return c;
}

/* After a draw's vertices are transformed and before it is published: keep
 * them as this frame's, or move them to the point between the frame before
 * and this one. Only x, y, z, w -- colours and texture coordinates stay F+1's. */
static void pair_positions(const RasterCfg* rc, Vertex* v, unsigned count, PairClaim c)
{
    unsigned i;
    if (c.cur_off != PAIR_NONE) {
        float (*d)[4] = g_pair[g_pair_cur].pos + c.cur_off;
        for (i = 0; i < count; i++) {
            d[i][0] = v[i].x;
            d[i][1] = v[i].y;
            d[i][2] = v[i].z;
            d[i][3] = v[i].w;
        }
    }
    if (c.prev_off != PAIR_NONE) {
        float (*p)[4] = g_pair[g_pair_cur ^ 1].pos + c.prev_off;
        float t = g_pair_t, u = 1.0f - g_pair_t;
        double worst = 0.0;
        for (i = 0; i < count; i++) {
            if (p[i][3] > 0.0f && v[i].w > 0.0f) {
                double dx = ((double)v[i].x / v[i].w - (double)p[i][0] / p[i][3]) * rc->wd;
                double dy = ((double)v[i].y / v[i].w - (double)p[i][1] / p[i][3]) * rc->ht;
                double d = sqrt(dx * dx + dy * dy);
                if (d > worst) worst = d;
            }
            v[i].x = u * p[i][0] + t * v[i].x;
            v[i].y = u * p[i][1] + t * v[i].y;
            v[i].z = u * p[i][2] + t * v[i].z;
            v[i].w = u * p[i][3] + t * v[i].w;
        }
        g_pair_verts += count;
        if (worst > g_pair_maxdisp) g_pair_maxdisp = worst;
        if (worst > 32.0) g_pair_over32++;
    }
}

/* The screen copy ends a frame: what was recorded becomes the frame before. */
static void pair_rotate(void)
{
    PairFrame* C = &g_pair[g_pair_cur];
    if (C->nrec > g_pair_peak_draws) g_pair_peak_draws = C->nrec;
    if (C->npos > g_pair_peak_verts) g_pair_peak_verts = C->npos;
    g_pair_cur ^= 1;
    pair_reset(&g_pair[g_pair_cur]);
    g_pair_ndraw = 0;
    g_pair_rotations++;
}

int gxr_pair_mode(unsigned flags, float t)
{
    int i;
    if ((flags & GXR_PAIR_RECORD) && (flags & GXR_PAIR_LERP)) return 0; /* H17a's second span makes this legal */
    if (flags && !g_pair[0].rec) {
        for (i = 0; i < 2; i++) {
            g_pair[i].rec = (PairRec*)malloc(sizeof(PairRec) * PAIR_DRAWS);
            g_pair[i].slot = (PairSlot*)malloc(sizeof(PairSlot) * PAIR_SLOTS);
            g_pair[i].pos = (float (*)[4])malloc(sizeof(float) * 4 * PAIR_VERTS);
            if (!g_pair[i].rec || !g_pair[i].slot || !g_pair[i].pos) return 0;
            pair_reset(&g_pair[i]);
        }
    }
    g_pair_flags = flags;
    g_pair_t = t;
    g_pair_ndraw = 0;
    return 1;
}

unsigned long long gxr_pair_rotations(void) { return g_pair_rotations; }

void gxr_pair_list(void* file) { g_pair_list = (FILE*)file; }

void gxr_pair_report(void)
{
    fprintf(stderr,
            "[pair] midpoint at t=%.2f: %llu of %llu draws matched (%llu vertices interpolated), %llu drawn from F+1, "
            "%llu demoted (projection type differs), %llu copies to texture skipped (%llu clears kept), %llu over "
            "capacity; largest displacement %.1f px, %llu matched draws with a vertex over 32 px; peak %u draws / "
            "%u vertices a frame; pairs %016llx\n",
            (double)g_pair_t, g_pair_matched, g_pair_matched + g_pair_unmatched + g_pair_demoted, g_pair_verts,
            g_pair_unmatched, g_pair_demoted, g_pair_skipped, g_pair_kept, g_pair_over, g_pair_maxdisp,
            g_pair_over32, g_pair_peak_draws, g_pair_peak_verts, (unsigned long long)g_pair_list_hash);
}

static void gxr_draw_inner(CpuState* s, unsigned op, unsigned count, const uint8_t* verts, unsigned vsize);
/* The draw exporter (gxr_export.h), or NULL: set through a setter so the
 * renderer still links alone (test_gxr_backend.py). */
static const GxrDrawExport* g_export;
void gxr_set_draw_export(const GxrDrawExport* e) { g_export = e; }

void gxr_draw(CpuState* s, unsigned op, unsigned count, const uint8_t* verts, unsigned vsize)
{
    TIMED(T_SETUP, gxr_draw_inner(s, op, count, verts, vsize));
}

static void gxr_draw_inner(CpuState* s, unsigned op, unsigned count, const uint8_t* verts, unsigned vsize)
{
    const uint32_t* xf = gx_xf_regs();
    const uint32_t* bp = gx_bp_regs();
    unsigned prim = op & 0xF8, vat = op & 7, i;
    const uint8_t* p = verts;
    Vertex* v;
    DrawCmd* D;
    PairClaim pc = {PAIR_NONE, PAIR_NONE};
    (void)vsize;
    if (!gxr_enabled() || count == 0) return;
    ++g_draw_no;
    if (g_draw_limit && g_draw_no > g_draw_limit) return; /* SOA_GXR_DRAWS=N: stop after N draws */
    /* Headless snapshots (SOA_SNAP=N): only the frames being written are
     * worth rasterizing; the game then runs at full speed between them. A
     * window (g_draw_every) or a replay (g_png_path) wants every frame it
     * shows, whatever the interval says. */
    if (g_snap_every && !g_draw_every && !g_png_path[0] && !snap_frame(gx_frame_count())) return;
    if (!g_started) { g_started = 1; workers_start(); }
    tex_set_memory(s);

    /* Every drain on this path comes before tev_prepare: the gate (a screen
     * copy was the last frame's end), then the arena and the graveyard. */
    if (g_frame_gate) drain(W_GATE);
    if (g_arena_used + sizeof(Vertex) * count > ARENA_BYTES) drain(W_ARENA);
    if (tex_graveyard_full()) drain(W_GRAVE);
    draw_tripwire(bp, prim);
    /* Nothing of the queue's is claimed until tev_prepare has returned.
     * Under SOA_GXR_DRAIN, resolving a texture read from a destination a
     * queued copy has not written yet makes it drain, and a drain takes the
     * vertex arena back to the start: a vertex pointer taken before the call
     * would name storage about to be handed out again. Since H14 a texture
     * read waits for that one copy and recycles nothing, but the order is kept
     * for the drain that is still there to be switched on. */
    {
        uint64_t hits = g_hazard_hits;
        TIMED(T_PREPARE, tev_prepare(bp, &g_prep));
        if (g_hazard_hits != hits) g_prepare_hazards++;
    }
    v = (Vertex*)(g_arena + g_arena_used);
    g_arena_used += (sizeof(Vertex) * count + 15) & ~(size_t)15;
    D = claim_slot(0, tex_draw_fence(), 0); /* every worker past the copies whose images it samples */
    D->tev = g_prep;
    pixel_prepare(bp, &D->px);
    raster_prepare(xf, bp, &D->rc);
    D->ntex = D->tev.used_tex & ((1u << (xf[0x103F] & 15)) - 1u);
    D->nchan = D->tev.used_chan;
    D->miptex = 0;
    memset(D->texmap_of, 0, sizeof D->texmap_of);
    {
        unsigned st;
        for (st = 0; st < D->tev.stages; st++) {
            const Stage* S = &D->tev.st[st];
            if (!S->texen) continue;
            D->texmap_of[S->texcoord] = S->texmap;
            if (D->tev.tex[S->texmap].mip && D->tev.tex[S->texmap].nlevels > 1) D->miptex |= 1u << S->texcoord;
        }
    }
    D->prim = prim; D->count = count; D->v = v;
    if (g_pair_flags) pc = pair_claim(pair_key(gx_cp_regs(), bp, prim, count), prim, count, (int)(xf[0x1026] & 1));
    for (i = 0; i < count; i++) {
        VertexIn in;
        p = decode_vertex(s, p, vat, &in);
        transform(s, &in, &v[i]);
        if (g_export) g_export->vertex(i, &in, &v[i]);
    }
    if (g_export) g_export->draw(D, count);
    if (g_pair_flags) pair_positions(&D->rc, v, count, pc);
    if (prim <= 0xA0) g_tris += prim == 0x80 ? (count / 4) * 2 : (prim == 0x90 ? count / 3 : (count >= 2 ? count - 2 : 0));
    if (g_workers > 0) {
        publish(); /* the workers pick it up */
    } else {
        run_here(D);
    }
}

/* ---- EFB copy and clear ---------------------------------------------------- */

/* Rows this thread owns: every Nth when workers exist, all otherwise. */
static inline int my_row(int y)
{
    return g_nthreads <= 1 || (unsigned)y % (unsigned)g_nthreads == (unsigned)(t_tid - 1);
}

static void efb_clear(uint32_t ar, uint32_t gb, uint32_t zreg, int x0, int y0, int w, int h)
{
    uint32_t z = zreg & 0xFFFFFFu;
    uint8_t col[4] = {(uint8_t)(ar & 0xFF), (uint8_t)((gb >> 8) & 0xFF), (uint8_t)(gb & 0xFF), (uint8_t)((ar >> 8) & 0xFF)};
    int x, y;
    for (y = y0; y < y0 + h && y < EFB_H; y++) {
        if (y < 0 || !my_row(y)) continue;
        for (x = x0; x < x0 + w && x < EFB_W; x++) {
            if (x < 0) continue;
            memcpy(g_efb[y][x], col, 4);
            g_efb_z[y][x] = z;
        }
    }
}

/* Write an EFB rectangle into memory as a texture (GXCopyTex). Tiled like
 * the formats the sampler decodes. */
/* The bytes from one row of tiles to the next where a copy writes. BP 0x4D
 * holds them in 32-byte units, and GXSetTexCopyDst sets it from the
 * texture the copy makes, which can be wider than the copy: the battle
 * transition copies the 640-wide screen into a 1024-wide RGB5A3 texture,
 * and packed rows drew it as streaks over whatever the texture held
 * before (FINDINGS "V1"). A stride narrower than the copy's own rows packs
 * them instead: its rows would overlap, which the console resolves by
 * writing them in order and the pool, writing rows in parallel, could not
 * -- the result would depend on the thread count. No game sets one; the
 * synthetic streams in tools/citest leave 0x28 there from the screen copy. */
static uint32_t copy_row_stride(uint32_t stride_reg, uint32_t natural)
{
    uint32_t s = (stride_reg & 0x3FFu) * 32u;
    return s > natural ? s : natural;
}

/* From a copy's first tile to the end of its last, rows row_bytes apart. */
static uint32_t copy_extent(uint32_t ow, uint32_t oh, unsigned tw, unsigned th, unsigned bpt, uint32_t row_bytes)
{
    uint32_t rows = (oh + th - 1) / th, cols = (ow + tw - 1) / tw;
    return rows && cols ? (rows - 1) * row_bytes + cols * bpt : 0;
}

static void copy_to_texture(const DrawCmd* D, CpuState* s, uint32_t dest_reg, uint32_t v, int x0, int y0, int w, int h)
{
    uint32_t dest = (dest_reg & 0x1FFFFFu) << 5;
    unsigned tpf = (v >> 3) & 15;
    unsigned fmt = tpf / 2 + (tpf & 1) * 8; /* EFBCopyFormat */
    int intensity = (v >> 15) & 1, half = (v >> 9) & 1;
    int ow = half ? w / 2 : w, oh = half ? h / 2 : h;
    /* Texture copies are filtered too. BP 0x53/0x54 are global PE state with
     * no per-copy enable: GXCopyDisp and GXCopyTex each read-modify-write only
     * their own shadow of the 0x52 command word and touch neither filter
     * register, and GXSetCopyClamp writes the filter's edge control into the
     * texture-copy shadow as well, which it would have no reason to do if a
     * texture copy were unfiltered. */
    int filtered = !(D->cp_f_up == 0 && D->cp_f_dn == 0 && D->cp_f_mid == 64);
    int ytop = y0 < 0 ? 0 : y0;
    int ybot = y0 + h - 1 > EFB_H - 1 ? EFB_H - 1 : y0 + h - 1;
    int x, y;
    unsigned tw, th, bpt;
    uint32_t row_bytes;
    uint8_t* base;

    /* map copy formats onto texture formats; anything else is left alone
     * rather than written at a guessed size */
    unsigned texfmt, chan_a = 0, chan_b = 3; /* source channels for single/dual-channel copies */
    if (intensity) texfmt = fmt == 0 ? 0 : fmt == 1 ? 1 : fmt == 2 ? 2 : fmt == 3 ? 3 : 99;
    else switch (fmt) {
    case 0: texfmt = 0; break;                       /* R4 */
    case 1: case 8: texfmt = 1; break;               /* R8 */
    case 9: texfmt = 1; chan_a = 1; break;           /* G8 */
    case 10: texfmt = 1; chan_a = 2; break;          /* B8 */
    case 7: texfmt = 1; chan_a = 3; break;           /* A8 */
    case 2: texfmt = 2; break;                       /* RA4 */
    case 3: texfmt = 3; break;                       /* RA8 */
    case 11: texfmt = 3; chan_a = 0; chan_b = 1; break; /* RG8: like IA8 with (g, r)? keep (b=g, a=r) */
    case 12: texfmt = 3; chan_a = 1; chan_b = 2; break; /* GB8 */
    case 4: texfmt = 4; break;
    case 5: texfmt = 5; break;
    case 6: texfmt = 6; break;
    default: texfmt = 99; break;
    }
    if (g_debug && g_copies_tex < 16)
        fprintf(stderr, "[gxr] copy to texture: dest %08X %dx%d from (%d,%d) fmt %u intensity %d half %d -> texfmt %u" "\n",
                dest, w, h, x0, y0, fmt, intensity, half, texfmt);
    if (texfmt == 99) return;

    switch (texfmt) {
    case 0: tw = 8; th = 8; bpt = 32; break;
    case 1: case 2: tw = 8; th = 4; bpt = 32; break;
    case 3: case 4: case 5: tw = 4; th = 4; bpt = 32; break;
    default: tw = 4; th = 4; bpt = 64; texfmt = 6; break;
    }
    /* A half-scale copy already averages two rows, and no source in this tree
     * settles whether the console filters before or after that box, so it is
     * left unfiltered and says so. No capture sets half scale: bit 9 is clear
     * on all 39 copies in the corpus. */
    if (half && filtered)
        WARN_ONCE("[gxr] half-scale EFB copy with the vertical filter programmed; the box filter is applied and the vertical filter is not, because their order is not established\n");
    row_bytes = copy_row_stride(D->cp_stride, ((uint32_t)ow + tw - 1) / tw * bpt);
    if ((dest & MEM_MASK) + (size_t)copy_extent((uint32_t)ow, (uint32_t)oh, tw, th, bpt, row_bytes) > MEM1_SIZE) return;
    base = mem_ptr(s, dest | 0x80000000u);
    for (y = 0; y < oh; y++) {
        if (!my_row(half ? 2 * y : y) && !(half && my_row(2 * y + 1))) continue;
        if (half && !my_row(2 * y)) continue; /* half-scale rows are flushed before queueing */
        for (x = 0; x < ow; x++) {
            int sx = x0 + (half ? 2 * x : x), sy = y0 + (half ? 2 * y : y);
            uint8_t px[4] = {0, 0, 0, 255};
            uint8_t* tile = base + (size_t)(y / th) * row_bytes + (size_t)(x / tw) * bpt;
            unsigned ix = x % tw, iy = y % th;
            unsigned I;
            if (sx >= 0 && sy >= 0 && sx < EFB_W && sy < EFB_H) {
                if (half && sx + 1 < EFB_W && sy + 1 < EFB_H) {
                    int k;
                    for (k = 0; k < 4; k++)
                        px[k] = (uint8_t)((g_efb[sy][sx][k] + g_efb[sy][sx + 1][k] + g_efb[sy + 1][sx][k] + g_efb[sy + 1][sx + 1][k]) / 4);
                } else {
                    /* Filter before the format conversion below: for RGB565,
                     * RGB5A3 and R4 , blending the quantised values gives a
                     * different answer from quantising the blend. Alpha keeps
                     * the centre row's -- the console has no alpha plane to
                     * filter here, PE_CONTROL being RGB8_Z24 in every capture. */
                    memcpy(px, g_efb[sy][sx], 4);
                    if (filtered) filter_sample(D, sx, sy, ytop, ybot, px);
                }
            }
            if (intensity) {
                /* Bounded: three bytes weighted to at most 235.7, never NaN (L6). */
                I = (unsigned)(0.257f * px[0] + 0.504f * px[1] + 0.098f * px[2] + 16.0f);
                if (I > 255) I = 255;
            } else {
                I = px[chan_a]; /* single-channel copies take that channel; dual ones pair it with chan_b */
                if (texfmt == 2 || texfmt == 3) px[3] = px[chan_b];
            }
            switch (texfmt) {
            case 0: { uint8_t* b = &tile[iy * 4 + ix / 2]; unsigned n = I >> 4; if (ix & 1) *b = (uint8_t)((*b & 0xF0) | n); else *b = (uint8_t)((*b & 0x0F) | (n << 4)); break; }
            case 1: tile[iy * 8 + ix] = (uint8_t)I; break;
            case 2: tile[iy * 8 + ix] = (uint8_t)((px[3] & 0xF0) | (I >> 4)); break;
            case 3: tile[(iy * 4 + ix) * 2] = px[3]; tile[(iy * 4 + ix) * 2 + 1] = (uint8_t)I; break;
            case 4: { unsigned c = ((px[0] >> 3) << 11) | ((px[1] >> 2) << 5) | (px[2] >> 3); tile[(iy * 4 + ix) * 2] = (uint8_t)(c >> 8); tile[(iy * 4 + ix) * 2 + 1] = (uint8_t)c; break; }
            case 5: { unsigned c;
                if (px[3] >= 224) c = 0x8000 | ((px[0] >> 3) << 10) | ((px[1] >> 3) << 5) | (px[2] >> 3);
                else c = ((px[3] >> 5) << 12) | ((px[0] >> 4) << 8) | ((px[1] >> 4) << 4) | (px[2] >> 4);
                tile[(iy * 4 + ix) * 2] = (uint8_t)(c >> 8); tile[(iy * 4 + ix) * 2 + 1] = (uint8_t)c; break; }
            default:
                tile[(iy * 4 + ix) * 2] = px[3]; tile[(iy * 4 + ix) * 2 + 1] = px[0];
                tile[32 + (iy * 4 + ix) * 2] = px[1]; tile[32 + (iy * 4 + ix) * 2 + 1] = px[2];
                break;
            }
        }
        /* The row's texels are this worker's alone, so its image row can be
         * decoded from what was just written (FINDINGS "Copy images"). */
        if (D->cp_image) tex_decode_row(D->cp_image, base, texfmt, (uint32_t)ow, (uint32_t)y); /* packed only */
    }
}

static uint8_t g_screen[EFB_H][EFB_W][4]; /* the last frame copied out, RGBA */
static int g_screen_w = EFB_W, g_screen_h = 480;
static plat_a32 g_frames_presented; /* copies to the screen completed by all rows */

/* A backend's screen copy (V2): its pixels into the screen buffer, which the
 * window, the PNG and the frame hash all read. Exactly the size the producer
 * set for this copy, or the frame would be part one copy and part another. */
void gxr_backend_screen(const uint8_t* rgba, int w, int h)
{
    int y;
    if (w != g_screen_w || h != g_screen_h) {
        fprintf(stderr, "[gxr] a backend's screen copy is %dx%d where the copy is %dx%d; stopping the run\n", w, h,
                g_screen_w, g_screen_h);
        exit(8);
    }
    for (y = 0; y < h; y++) memcpy(g_screen[y][0], rgba + (size_t)y * w * 4, (size_t)w * 4);
}

/* One filtered EFB sample: the three rows the copy filter reads, weighted and
 * divided by 64.
 *
 * The taps are clamped to the COPY RECTANGLE, not to the EFB. EFB_H is 528 and
 * every copy this game makes is 480 rows, so clamping to EFB_H-1 would pull
 * rows from below the rectangle -- whatever the last clear and scissor left
 * there -- into the bottom row of every frame, and only an edge test would
 * ever catch it. BP 0x52's bits 0 and 1 are the hardware's own clamp_top and
 * clamp_bottom and are set on every copy in the corpus; when one is clear we
 * clamp anyway and say so, because nothing in this tree establishes what the
 * hardware reads instead and a guess should be visible rather than silent.
 *
 * Truncating (>> 6) rather than rounding is a decision, not an accident: it is
 * what Dolphin does, and whichever rule is compiled is what the frame manifest
 * pins, so a later tidy-up to round-half-up would move all 23 hashes with no
 * behavioural reason to. */
static void filter_sample(const DrawCmd* D, int sx, int sy, int ytop, int ybot, uint8_t* o)
{
    int ya = sy - 1 < ytop ? ytop : sy - 1;
    int yb = sy + 1 > ybot ? ybot : sy + 1;
    unsigned up = D->cp_f_up, mid = D->cp_f_mid, dn = D->cp_f_dn;
    int k;
    for (k = 0; k < 3; k++) {
        unsigned v = up * g_efb[ya][sx][k] + mid * g_efb[sy][sx][k] + dn * g_efb[yb][sx][k];
        v >>= 6;
        o[k] = (uint8_t)(v > 255u ? 255u : v);
    }
}

static void copy_to_screen(const DrawCmd* D, int x0, int y0, int w, int h)
{
    int x, y;
    int filtered = !(D->cp_f_up == 0 && D->cp_f_dn == 0 && D->cp_f_mid == 64);
    int ytop = y0 < 0 ? 0 : y0;
    int ybot = y0 + h - 1 > EFB_H - 1 ? EFB_H - 1 : y0 + h - 1;
    if (filtered && (D->cp_v & 3u) != 3u)
        WARN_ONCE("[gxr] EFB copy asks for the vertical filter with clamp_top/clamp_bottom (BP 52 %06X) not both set; the taps are clamped to the copy rectangle anyway\n",
                  D->cp_v & 0xFFFFFFu);
    for (y = 0; y < h && y < EFB_H; y++) {
        int sy = y0 + y;
        if (!my_row(y)) continue;
        for (x = 0; x < w && x < EFB_W; x++) {
            int sx = x0 + x;
            uint8_t* o = g_screen[y][x];
            if (sx >= 0 && sy >= 0 && sx < EFB_W && sy < EFB_H) {
                if (filtered) filter_sample(D, sx, sy, ytop, ybot, o);
                else memcpy(o, g_efb[sy][sx], 3);
                o[3] = 255;
            } else { o[0] = o[1] = o[2] = 0; o[3] = 255; }
        }
    }
}

/* Whether this copy reads EFB rows the worker running it did not write: a
 * vertical filter reaches one row either side, and a half-scale copy pairs
 * rows. Both make the fused clear unsafe, so enqueue_copy publishes the clear
 * separately and this says so from the command alone. */
static int copy_is_foreign(int half, int filtered, int y0)
{
    /* The third case: the copy picks its rows by destination y and reads
     * source row y0 + y, while the rasterizer owns rows by absolute y, so the
     * two coincide only when y0 is a multiple of the worker count. Found by
     * reading the code, not by a sweep, which is the reason it can hide. */
    return half || filtered || (g_nthreads > 1 && (unsigned)y0 % (unsigned)g_nthreads != 0);
}

static int copy_reads_foreign_rows(const DrawCmd* D)
{
    int half = (D->cp_v >> 9) & 1;
    int filtered = !(D->cp_f_up == 0 && D->cp_f_dn == 0 && D->cp_f_mid == 64);
    return copy_is_foreign(half, filtered, (int)((D->cp_tl >> 10) & 0x3FF));
}

/* A copy's pixels, and its clear where it can ride inside the copy: never
 * with a backend, whose copy never clears (3.1), and never for a copy that
 * reads rows other workers own. */
static void run_copy_pixels(const DrawCmd* D)
{
    int x0 = (int)(D->cp_tl & 0x3FF), y0 = (int)((D->cp_tl >> 10) & 0x3FF);
    int w = (int)(D->cp_wh & 0x3FF) + 1, h = (int)((D->cp_wh >> 10) & 0x3FF) + 1;
    if (D->cp_v & 0x4000u) copy_to_screen(D, x0, y0, w, h);
    else copy_to_texture(D, D->s, D->cp_dest, D->cp_v, x0, y0, w, h);
    if ((D->cp_v & 0x800u) && !g_backend && !copy_reads_foreign_rows(D))
        efb_clear(D->cp_ar, D->cp_gb, D->cp_z, x0, y0, w, h);
}

static void run_copy(const DrawCmd* D)
{
    run_copy_pixels(D);
    if (D->cp_v & 0x4000u) plat_inc32(&g_frames_presented);
}

/* The deferred half of the command above, published after a drain. */
static void run_copy_clear(const DrawCmd* D)
{
    int x0 = (int)(D->cp_tl & 0x3FF), y0 = (int)((D->cp_tl >> 10) & 0x3FF);
    int w = (int)(D->cp_wh & 0x3FF) + 1, h = (int)((D->cp_wh >> 10) & 0x3FF) + 1;
    efb_clear(D->cp_ar, D->cp_gb, D->cp_z, x0, y0, w, h);
}

/* A command through the backend, on the producer (run_here) or on the
 * backend's own thread (worker, V6a): a failure stops the run, and the
 * counts the backend cannot keep are kept here -- what it was handed, for
 * gxr_report, and the screen copies it presented, which it cannot reach.
 * Only one thread ever runs it, so g_be_sent has one writer. */
static unsigned long long g_be_sent[3];
static void run_backend(const DrawCmd* D)
{
    int ok = D->kind == 0 ? g_backend->draw(D) : D->kind == 1 ? g_backend->copy(D) : g_backend->clear(D);
    if (!ok) {
        fprintf(stderr, "[gxr] the %s backend failed command %lld (kind %d); stopping the run\n", g_backend->name, D->seq, D->kind);
        exit(8);
    }
    g_be_sent[D->kind < 3 ? D->kind : 2]++;
    if (D->kind == 1 && (D->cp_v & 0x4000u)) plat_inc32(&g_frames_presented);
}

/* A command run on the producer, as it is built: through the backend when
 * one is set that has no thread of its own (V2), or by the CPU path, which
 * then needs a time bucket of its own or would be booked as vertex setup.
 * Run here and finished here, so the numbering still advances and the arena
 * is free again. */
static void run_here(DrawCmd* D)
{
    t_tid = 1;
    if (g_backend) {
        TIMED(T_RASTER, run_backend(D));
    } else {
        TIMED(T_RASTER, draw_command(D));
    }
    plat_inc64(&g_published);
    g_arena_used = 0;
}

/* The test-only passthrough backend (SOA_GXR_BACKEND=passthrough, V2): the
 * CPU path's own code called through the hook, so that the hook, the
 * separate clear and the presented count are proved before a GPU backend
 * relies on them. run_here counts what it ran, as it does for any backend. */
static int pt_draw(const DrawCmd* D) { draw_command(D); return 1; }
static int pt_copy(const DrawCmd* D) { run_copy_pixels(D); return 1; }
static int pt_clear(const DrawCmd* D) { run_copy_clear(D); return 1; }
static const GxrBackend g_passthrough = {"passthrough", pt_draw, pt_copy, pt_clear, NULL, NULL, NULL};

void gxr_set_backend(const GxrBackend* b)
{
    if (g_started) {
        WARN_ONCE("[gxr] a backend set after the first command is ignored: the pool is already running\n");
        return;
    }
    g_backend = b;
}

/* Copies to memory since the last drain: where each wrote, how much, and
 * which command it is. Anything the producer reads of guest memory -- a
 * texture, a palette, vertex arrays, a display list, what a hook peeks --
 * that overlaps one must wait for it; cleared only by a drain. */
typedef struct {
    uint32_t addr, bytes;
    long long cmd;
} Pending;
static Pending g_pending[QUEUE_CAP];
static int g_pending_n;

/* The texture format copy_to_texture maps a copy command word to, and 99 for
 * one it does not write. */
static unsigned copy_texfmt(uint32_t v)
{
    unsigned tpf = (v >> 3) & 15, fmt = tpf / 2 + (tpf & 1) * 8;
    if ((v >> 15) & 1) return fmt <= 3 ? fmt : 99;
    switch (fmt) {
    case 0: return 0;
    case 1: case 7: case 8: case 9: case 10: return 1;
    case 2: return 2;
    case 3: case 11: case 12: return 3;
    case 4: return 4;
    case 5: return 5;
    case 6: return 6;
    default: return 99;
    }
}

/* What a copy to memory spans, in bytes, from its first tile to the end of
 * its last: the rows of tiles row_bytes apart (copy_row_stride), so a copy
 * into a wider texture spans more than it writes. 0 for a format it does
 * not write. *packed says whether the rows lie end to end, which is the
 * only layout a copy image (tex_copy_image) describes. */
static uint32_t copy_bytes(uint32_t v, int w, int h, uint32_t stride_reg, int* packed)
{
    unsigned texfmt = copy_texfmt(v), tw, th, bpt;
    int half = (v >> 9) & 1;
    uint32_t ow = (uint32_t)(half ? w / 2 : w), oh = (uint32_t)(half ? h / 2 : h), natural, row;
    if (packed) *packed = 1;
    if (texfmt == 99) return 0;
    switch (texfmt) {
    case 0: tw = 8; th = 8; bpt = 32; break;
    case 1: case 2: tw = 8; th = 4; bpt = 32; break;
    case 3: case 4: case 5: tw = 4; th = 4; bpt = 32; break;
    default: tw = 4; th = 4; bpt = 64; break;
    }
    natural = (ow + tw - 1) / tw * bpt;
    row = copy_row_stride(stride_reg, natural);
    if (packed) *packed = row == natural;
    return copy_extent(ow, oh, tw, th, bpt, row);
}

/* The newest copy that overlaps [addr, addr + bytes) and some worker has not
 * finished, or -1. The list is in publish order, so the last overlap is the
 * newest, and every older one is finished once it is. */
static long long pending_overlap(uint32_t addr, uint32_t bytes)
{
    int i;
    addr &= MEM_MASK;
    for (i = g_pending_n - 1; i >= 0; i--)
        if (addr < g_pending[i].addr + g_pending[i].bytes && addr + bytes > g_pending[i].addr) return g_pending[i].cmd;
    return -1;
}

long long gxr_pending_newest(uint32_t addr, uint32_t bytes)
{
    return g_pending_n ? pending_overlap(addr, bytes) : -1;
}

/* A read of guest memory on the producer that a queued copy may be writing:
 * wait for that copy, and for nothing else. */
void gxr_ram_hazard(uint32_t addr, uint32_t bytes, int why)
{
    long long c;
    if (!g_pending_n) return;
    c = pending_overlap(addr, bytes);
    if (c < 0) return;
    if (why == W_HAZARD) g_hazard_hits++;
    if (g_legacy && why == W_HAZARD) { drain(W_HAZARD); return; }
    wait_landed(c, why);
}

void gxr_texture_hazard(uint32_t addr, uint32_t bytes)
{
    gxr_ram_hazard(addr, bytes, W_HAZARD);
}

/* For the front end and the hooks (gx.c, main.c, mod.c), which name waits by
 * what they read rather than by this file's reasons. */
void gxr_source_hazard(uint32_t addr, uint32_t bytes)
{
    gxr_ram_hazard(addr, bytes, W_SRC);
}

void gxr_hook_hazard(uint32_t addr, uint32_t bytes)
{
    int i, n = 0;
    gxr_ram_hazard(addr, bytes, W_HOOK);
    /* A hook may write here (SOA_POKE, a mod's patch or API call), and every
     * copy it overlaps is over now -- it waited for the newest, and a worker
     * runs its commands in order. Forget those, so a copy's image of these
     * bytes retires at its next lookup and the draw reads what the hook wrote
     * (FINDINGS "Copy images"). */
    addr &= MEM_MASK;
    for (i = 0; i < g_pending_n; i++)
        if (!(addr < g_pending[i].addr + g_pending[i].bytes && addr + bytes > g_pending[i].addr)) g_pending[n++] = g_pending[i];
    g_pending_n = n;
}

long gxr_presented(void)
{
    long n = plat_load32(&g_frames_presented);
    return g_workers > 0 ? n / g_workers : n;
}

const uint8_t* gxr_screen(int* w, int* h)
{
    *w = g_screen_w; *h = g_screen_h;
    return &g_screen[0][0][0];
}

static uint64_t fnv1a(uint64_t h, const uint8_t* p, size_t n)
{
    while (n--) { h ^= *p++; h *= 1099511628211ULL; }
    return h;
}

/* FNV-1a over the pixels the port would present, so two runs can be compared
 * without keeping a PNG of either. The size goes in first -- a frame that
 * changes shape is a different frame -- then the rows in screen order, only
 * the part of each row this frame covers. Nothing here depends on which
 * worker produced a row, so the value is the same at any SOA_THREADS, and
 * every caller hashes after gxr_flush() so the frame is finished. */
uint64_t gxr_screen_hash(void)
{
    uint8_t dim[4] = {(uint8_t)(g_screen_w >> 8), (uint8_t)g_screen_w,
                      (uint8_t)(g_screen_h >> 8), (uint8_t)g_screen_h};
    uint64_t h = fnv1a(14695981039346656037ULL, dim, sizeof dim);
    int y;
    for (y = 0; y < g_screen_h; y++) h = fnv1a(h, g_screen[y][0], (size_t)g_screen_w * 4);
    return h;
}

/* Where SOA_SNAP writes: build/frames, or SOA_FRAMES_DIR. A job of its own
 * directory is what lets a soak's snapshots be judged afterwards: every run
 * shares build/frames, so what is there is whichever run wrote each number
 * last (PLAN-60FPS-MODS S3, S6). */
static const char* frames_dir(void)
{
    static char dir[400];
    if (!dir[0]) {
        const char* e = getenv("SOA_FRAMES_DIR");
        snprintf(dir, sizeof dir, "%s", e && *e ? e : "build/frames");
    }
    return dir;
}

/* Nothing else creates the directory, so the first SOA_SNAP run on a clean
 * tree used to write nothing and say only that it could not. Every level is
 * made, as mkdir -p would. */
static void ensure_frames_dir(void)
{
    static int done;
    char path[400];
    size_t i;
    if (done) return;
    done = 1;
    snprintf(path, sizeof path, "%s", frames_dir());
    for (i = 1; path[i - 1]; i++) {
        char c = path[i];
        if (c != '/' && c != '\\' && c != '\0') continue;
        path[i] = '\0';
#ifdef _WIN32
        _mkdir(path);
#else
        mkdir(path, 0777);
#endif
        path[i] = c;
    }
}

static void write_frame_png(const char* path, int w, int h)
{
    if (!png_write_rgba(path, &g_screen[0][0][0], w, h, EFB_W * 4)) fprintf(stderr, "[gxr] cannot write %s\n", path);
    else fprintf(stderr, "[gxr] wrote %s (%dx%d)\n", path, w, h);
}

/* The EFB copy's vertical filter (BP 0x53/0x54), collapsed onto the rows it
 * actually reads.
 *
 * The seven six-bit weights are NOT seven rows. They are vertical sub-samples:
 * two belong to the row above, three to the row itself and two to the row
 * below, which is why seven taps span three pixels. So the game's
 * 8,8,10,12,10,8,8 is 16/64 above, 32/64 centre, 16/64 below -- a 1:2:1
 * deflicker blur -- and nothing lands two or three rows away.
 *
 * The source for that grouping is patent US6999100B1, the hardware's own
 * description ("seven samples from three vertically arranged pixels ... Three
 * samples are taken from the current pixel, two samples ... immediately above
 * ... two ... immediately below"), corroborated by libogc, whose vfilter
 * tables are labelled "line n-1 through n+1". It is deliberately not derived
 * from the SDK's filter-off set {0,0,21,22,21,0,0} -- GXSetCopyFilter is
 * fn_8024EF50 and its vf == 0 arm at 0x8024F138 loads exactly that. That set
 * proves taps 2,3,4 land on the current row, since nothing else makes it an
 * identity, and so kills any reading of seven distinct rows; but it is equally
 * an identity under a FIVE-row window, which would give 8/8/32/8/8 instead.
 * The register layout cannot settle this. Reading it and assuming the rest is
 * how PLAN C3's acceptance criterion came to be wrong for two days. */
static void copy_filter(uint32_t f0, uint32_t f1, uint8_t* up, uint8_t* mid, uint8_t* dn)
{
    unsigned w0 = f0 & 0x3F, w1 = (f0 >> 6) & 0x3F, w2 = (f0 >> 12) & 0x3F, w3 = (f0 >> 18) & 0x3F;
    unsigned w4 = f1 & 0x3F, w5 = (f1 >> 6) & 0x3F, w6 = (f1 >> 12) & 0x3F;
    *up = (uint8_t)(w0 + w1);
    *mid = (uint8_t)(w2 + w3 + w4);
    *dn = (uint8_t)(w5 + w6);
    /* All seven zero is not a request for a filter that multiplies every pixel
     * by nothing: it is the register never having been written, which means a
     * synthetic stream rather than the game. Copy unfiltered, exactly as the
     * Y-scale check below reads a zero there. Getting this wrong copies a
     * black frame, and it is the renderer's own selftest that says so --
     * "render full-screen quad: 0 of 307200 red". */
    if ((*up | *mid | *dn) == 0) { *mid = 64; return; }
    /* A set that does not sum to 64 scales every copied pixel's brightness,
     * and it would sail past a "the neighbours are zero" test. Nothing the
     * game programs can trip this; a synthetic stream can, which is the point
     * -- an oracle that cannot fail is worse than none. */
    if ((unsigned)*up + *mid + *dn != 64u)
        WARN_ONCE("[gxr] EFB copy filter weights (BP 53 %06X, BP 54 %06X) sum to %u, not 64, so the copy would rescale every pixel's brightness; applied as given\n",
                  f0, f1, (unsigned)*up + *mid + *dn);
}

/* A clear of its own (kind 2), for a copy whose clear cannot ride inside it:
 * a foreign copy after its second drain, or H10's in-between pass, which
 * skips the copy and keeps the clear. */
static void publish_clear(CpuState* s, const uint32_t* bp, uint32_t v)
{
    DrawCmd* D;
    if (g_frame_gate) drain(W_GATE);
    D = claim_slot(2, 0, 0); /* after a foreign copy this carries its exit fence */
    D->s = s;
    D->cp_v = v; D->cp_tl = bp[0x49]; D->cp_wh = bp[0x4A];
    D->cp_ar = bp[0x4F]; D->cp_gb = bp[0x50]; D->cp_z = bp[0x51];
    if (g_workers > 0) {
        publish();
    } else {
        run_here(D);
    }
}

static void enqueue_copy(CpuState* s, const uint32_t* bp, uint32_t v)
{
    DrawCmd* D;
    int x0 = (int)(bp[0x49] & 0x3FF), y0 = (int)((bp[0x49] >> 10) & 0x3FF);
    int w = (int)(bp[0x4A] & 0x3FF) + 1, h = (int)((bp[0x4A] >> 10) & 0x3FF) + 1;
    int to_screen = (v & 0x4000u) != 0, half = (v >> 9) & 1;
    uint8_t f_up, f_mid, f_dn;
    int filtered, foreign, near_rows;
    uint32_t dest, bytes;
    int packed = 1;
    long long want, want_near;
    /* Filtered iff the collapsed kernel is not the exact identity. Asking the
     * weights rather than masking the registers is what makes this right: the
     * mask here was 0x03FFFF against both words, which takes w3 alone for the
     * centre row and so calls the SDK's own filter-off set filtered, since
     * that set is w2 = 21, w3 = 22, w4 = 21. Testing the sum as well means a
     * set that does not total 64 takes the filtered path and is scaled, rather
     * than being waved through as "near enough to the identity". */
    copy_filter(bp[0x53], bp[0x54], &f_up, &f_mid, &f_dn);
    if (to_screen && !g_deflicker) { /* P5b: the identity, so the unfiltered path and its fences */
        f_up = 0;
        f_mid = 64;
        f_dn = 0;
    }
    filtered = !(f_up == 0 && f_dn == 0 && f_mid == 64);
    /* Copy Y-scale (BP 4E) is 1.8 fixed point over the whole 24-bit field,
     * and 000100 is the identity -- all the game has ever programmed, and it
     * cannot move a pixel -- so this warns only when the copy is asked to
     * rescale and we ignore it. Comparing the whole field matters both ways:
     * a nine-bit compare would read 000300 as the identity and say nothing,
     * and print 000200 as 0.000 while warning. Zero is not a request: it is
     * the register never having been written, which is a synthetic stream
     * rather than the game, and a scale of zero would copy nothing. */
    if ((bp[0x4E] & 0xFFFFFFu) && (bp[0x4E] & 0xFFFFFFu) != 0x100u)
        WARN_ONCE("[gxr] EFB copy Y-scale (BP 4E %06X) is %.3f, not 1.0; the copy is not scaled vertically, so the destination keeps the source's height\n",
                  bp[0x4E], (double)(bp[0x4E] & 0xFFFFFFu) / 256.0);
    if (!g_started) { g_started = 1; workers_start(); }
    tex_set_memory(s);
    /* H10's in-between pass: a copy to a texture would write what F+1's own
     * frame reads next, so it is skipped -- but its clear is kept, since the
     * draws after it expect the EFB it leaves. The screen copy runs as ever. */
    if ((g_pair_flags & GXR_PAIR_LERP) && !to_screen) {
        g_pair_skipped++;
        if (v & 0x800u) {
            publish_clear(s, bp, v);
            g_pair_kept++;
        }
        return;
    }
    /* Both of these read EFB rows this worker does not own. my_row is strided
     * -- y % nthreads == tid-1 -- and the plain copy is safe only because the
     * row it reads is the row it wrote: the rasterizer partitions by absolute
     * EFB row and the copy by destination row, which coincide at y0 = 0. A
     * filtered output row reads y-1 and y+1, which belong to the two
     * neighbouring workers, and workers advance independently. Without this
     * the frame depends on SOA_THREADS. */
    foreign = copy_is_foreign(half, filtered, y0);
    if (g_frame_gate) drain(W_GATE);
    if (g_legacy && foreign) {
        static unsigned last_frame = ~0u;
        unsigned frame = gx_frame_count();
        drain(frame != last_frame ? W_COPY_FIRST : W_COPY_BEFORE);
        last_frame = frame;
    }
    dest = ((bp[0x4B] & 0x1FFFFFu) << 5) & MEM_MASK;
    bytes = to_screen ? 0 : copy_bytes(v, w, h, bp[0x4D], &packed);
    if (bytes && g_pending_n == QUEUE_CAP) drain(W_PENDING);
    /* The entry fence (H14). A copy that reads rows other workers own -- and
     * every screen copy, whatever its filter, so that gxr_presented stays a
     * count of whole frames -- waits in the pool until every worker has
     * finished every command before it: every row it reads is final. A copy
     * that reads only its own rows but writes memory an unfinished copy is
     * still writing waits for that one copy instead. */
    /* A filtered copy that is foreign for its filter alone -- full scale,
     * its rows the workers' own rows -- reads, for each row it writes, the
     * row either side, which the writer's two neighbours own: those two are
     * all its fences need to wait for (FINDINGS "Neighbour fences"). A screen
     * copy still waits for every worker on the way in, for gxr_presented. */
    near_rows = !g_legacy && filtered && !half && (g_nthreads <= 1 || (unsigned)y0 % (unsigned)g_nthreads == 0);
    want_near = 0;
    if (to_screen || (foreign && !near_rows)) {
        want = g_published; /* own count */
    } else {
        long long c = bytes ? pending_overlap(dest, bytes) : -1;
        want = c >= 0 ? c + 1 : 0;
        if (foreign) want_near = g_published; /* own count */
    }
    D = claim_slot(1, want, want_near);
    D->s = s;
    D->cp_image = NULL;
    D->cp_v = v; D->cp_tl = bp[0x49]; D->cp_wh = bp[0x4A]; D->cp_dest = bp[0x4B]; D->cp_stride = bp[0x4D];
    D->cp_ar = bp[0x4F]; D->cp_gb = bp[0x50]; D->cp_z = bp[0x51];
    D->cp_f_up = f_up; D->cp_f_mid = f_mid; D->cp_f_dn = f_dn;
    if (g_export) g_export->copy(v, x0, y0, w, h, dest, bytes);
    if (to_screen) { g_screen_w = w > EFB_W ? EFB_W : w; g_screen_h = h > EFB_H ? EFB_H : h; g_copies_xfb++; }
    else {
        if (bytes) {
            g_pending[g_pending_n].addr = dest;
            g_pending[g_pending_n].bytes = bytes;
            g_pending[g_pending_n].cmd = D->seq;
            g_pending_n++;
            /* The copy's image (FINDINGS "Copy images"), where copy_to_texture
             * will write: not past the end of memory, which it refuses; not
             * under SOA_GXR_DRAIN, which keeps the old protocol whole; and not
             * with no workers, where the copy has run by the time anything
             * could sample it, so an image would save nothing and would outlive
             * a CPU write to its bytes. */
            if (!g_legacy && g_workers > 0 && dest + bytes <= MEM1_SIZE && packed)
                D->cp_image = tex_copy_image(dest, copy_texfmt(v), (uint32_t)(half ? w / 2 : w), (uint32_t)(half ? h / 2 : h), D->seq);
        }
        g_copies_tex++;
    }
    g_last_copy = D->seq;
    if (g_workers > 0) {
        publish();
    } else {
        run_here(D);
    }
    /* A copy that samples rows it does not own has to be over before anything
     * after it writes those rows, and this is the half that is easy to miss:
     * workers advance through the queue independently, so the worker that
     * finishes its share of the copy first would otherwise start on the next
     * draw -- or on the clear -- and write EFB rows its neighbours are still
     * reading as filter taps.
     *
     * This was measured, not reasoned about (PLAN C3). With only the clear
     * split out, four captures disagreed with themselves at SOA_THREADS=8 on
     * one sweep in four -- 1550, 4500, 6000 and 16300, which are four of the
     * eight captures that copy to a texture. Screen copies hid it: they set the
     * clear bit, so the clear's own drain happened to serve. Three green sweeps
     * in a row had already run before the fourth caught it.
     *
     * Until H14 a full drain did it, and the producer waited for the whole
     * frame at every copy. Now the command after the copy carries the exit
     * fence: no worker starts it until every worker has finished the copy, and
     * every later command a worker runs comes after that one. The clear goes in
     * its own command for the same reason, rather than riding inside run_copy
     * where a worker would clear rows its neighbours were still sampling. An
     * unfiltered, unscaled copy reads only rows the worker wrote itself, so it
     * keeps the fused clear and needs neither fence. SOA_GXR_DRAIN=1 puts the
     * drain back. */
    if (foreign) {
        if (g_legacy) drain(W_COPY_AFTER);
        else if (near_rows) g_fence_after_near = D->seq + 1;
        else g_fence_after = D->seq + 1;
        if (v & 0x800u) publish_clear(s, bp, v);
    } else if (g_backend && (v & 0x800u)) {
        publish_clear(s, bp, v); /* a backend's copy never clears (3.1) */
    }
    /* The frame is published: the next frame's first command drains, once.
     * That recycles the arena, the copy list and the graveyard a frame at a
     * time, keeps one frame in flight, and lets the guest's frame end run while
     * the workers finish this one. */
    if (to_screen && !g_legacy) g_frame_gate = 1;

    if (to_screen) {
        char path[512];
        int want = 0;
        unsigned frame = gx_frame_count(); /* the front end increments it after this copy, so this is the frame being presented */
        if (g_png_path[0]) { snprintf(path, sizeof path, "%s", g_png_path); want = 1; }
        else if (snap_frame(frame)) { ensure_frames_dir(); snprintf(path, sizeof path, "%s/%04u.png", frames_dir(), frame); want = 1; }
        /* The PNG and the hash both describe the finished frame, so wait here
         * for the rows of this copy every other worker owns. The hash is taken
         * from the same buffer the PNG is written from and at the same point,
         * so it does not depend on whether a PNG is being written. */
        if (want || g_hash) drain(W_HASHPNG);
        if (want) TIMED(T_PNG, write_frame_png(path, g_screen_w, g_screen_h));
        /* One line per presented frame, for a tool to diff between runs:
         * "[gxr] frame <n> <w>x<h> hash <16 hex digits>". Note that SOA_SNAP
         * skips rasterizing the frames it is not writing, so in that mode only
         * the frames that get a PNG have a hash worth comparing. */
        if (g_hash)
            fprintf(stderr, "[gxr] frame %u %dx%d hash %016llx\n", frame, g_screen_w, g_screen_h,
                    (unsigned long long)gxr_screen_hash());
        if (g_pair_flags & GXR_PAIR_RECORD) pair_rotate(); /* H10: this frame becomes the one before */
    }
}

void gxr_bp_written(CpuState* s, uint32_t reg, uint32_t v)
{
    const uint32_t* bp = gx_bp_regs();
    /* Only when there is a picture to be wrong: with the renderer off nothing
     * is drawn, so nothing is drawn wrong. */
    if (gxr_enabled()) bp_tripwire(bp, reg, v);
    if (reg >= 0xE0 && reg <= 0xE7) { tev_register_written(reg, v); return; }
    /* The texture-cache invalidate, and every EFB copy: texture memory may
     * have changed, so a texture is hashed again at its next use (H12). */
    if (reg == 0x66 || reg == 0x52) tex_epoch_advance();
    if (reg == 0x65) { /* TLUT load (GXLoadTlut): source from 0x64, tmem address and size here */
        uint32_t src = (bp[0x64] & 0x1FFFFFu) << 5;
        uint32_t tmem = (v & 0x3FFu) << 9, bytes = ((v >> 10) & 0x7FFu) << 5;
        gxr_ram_hazard(src, bytes, W_TLUT); /* the palette may be a queued copy's destination */
        tmem_load_tlut(s, src | 0x80000000u, tmem, bytes);
        return;
    }
    /* A draw token (GXSetDrawSync) is answered as it is parsed, as it always
     * has been -- the port's pipe has no latency to report. Until H14 every
     * copy before a token was also finished by then, because each was drained
     * after; now it may not be, and a game that read a copy's memory straight
     * after its token would see it unfinished. SOA_GXR_TOKENWAIT=1 makes the
     * token wait for the newest copy, and it is off because of where this
     * game's tokens sit: at the top of each frame's stream, before the frame's
     * logic, so waiting there serialized the logic behind the last frame's
     * drawing and took all of H14's gain (FINDINGS "H14"). GXDrawDone, which is
     * how a game waits before reading what was drawn, still drains. These are
     * counted either way. */
    if ((reg == 0x47 || reg == 0x48) && gxr_enabled() && !g_legacy && g_last_copy >= 0) {
        if (done_min() <= g_last_copy) {
            g_tokens_waited++;
            if (g_token_wait) wait_landed(g_last_copy, W_TOKEN);
        }
        /* Every copy is over, and the game has been told so: it may write a
         * copy's destination now. Forget them as a drain would, recycling
         * nothing, so a copy's image retires at its next lookup and the draw
         * reads memory (FINDINGS "Copy images"; found by its review). Without
         * SOA_GXR_TOKENWAIT only when the copies happen to be done: a game that
         * wrote a destination while its copy was still running raced it
         * before, and still does. */
        if (g_token_wait || done_min() > g_last_copy) g_pending_n = 0;
    }
    if (reg == 0x52 && gxr_enabled()) { /* EFB copy (GXCopyTex / GXCopyDisp) */
        TIMED(T_COPY, enqueue_copy(s, bp, v));
        return;
    }
    if (reg == 0x45 && (v & 2) && gxr_enabled()) drain(W_DRAWDONE); /* GXDrawDone: the CPU may read results now */
}

/* Defined in gxr_tev.c. A diagnostic for the report below rather than part of
 * the renderer's interface, so it is declared here and not in gxr.h. */
int tex_graveyard_peak(void);
void tex_report(void);

void gxr_report(void)
{
    /* No flush: the watchdog thread reports while the main thread produces,
     * so every number below is read while its writer may still be moving it.
     * Each one has a single writer and 64-bit alignment, so a read is a value
     * and not a tear; what it is not is a consistent instant, and the
     * workers' unaccounted share below is where that shows. */
    if (!gxr_enabled()) return;
    gxr_timing_finish();
    if (g_backend)
        fprintf(stderr, "[gxr] %s backend: %llu draws, %llu copies, %llu clears\n", g_backend->name, g_be_sent[0], g_be_sent[1],
                g_be_sent[2]);
    {
        double span = gxr_producer_span(), rest;
        uint64_t sum = 0;
        int i;
        for (i = 1; i < T_COUNT; i++) sum += g_gxr_ticks[i];
        rest = span - gxr_seconds(sum);
        if (rest < 0.0) rest = 0.0;
        if (span <= 0.0)
            fprintf(stderr, "[gxr] time: %s\n",
                    g_gxr_tsc < 0 ? "nothing was drawn this run, so nothing was timed"
                                  : "this build has no clock, so every phase timer reads zero");
        else
            fprintf(stderr,
                    "[gxr] producer over %.2fs by the %s: setup %.2fs, prepare %.2fs, decode %.2fs, copy %.2fs, png %.2fs, wait %.2fs, raster %.2fs; the other %.2fs is guest code and the command-stream parse, which no clock can afford to separate and the profile below separates by sampling. These are disjoint, so they add up.\n",
                    span, g_gxr_tsc ? "time-stamp counter" : "performance counter",
                    gxr_seconds(g_gxr_ticks[T_SETUP]), gxr_seconds(g_gxr_ticks[T_PREPARE]),
                    gxr_seconds(g_gxr_ticks[T_DECODE]), gxr_seconds(g_gxr_ticks[T_COPY]),
                    gxr_seconds(g_gxr_ticks[T_PNG]), gxr_seconds(g_gxr_ticks[T_WAIT]),
                    gxr_seconds(g_gxr_ticks[T_RASTER]), rest);
    }
    /* The identity the plan asks for: a worker is either running a command or
     * spinning for one, so the two add up to the pool's span times the number
     * of threads. What is left over is each thread's open stretch when this
     * was printed, bounded by one command or one backoff -- so a run that
     * reports more than a few percent has a worker stuck inside a command. */
    if (g_workers > 0 && g_pool_t0 && gxr_producer_span() > 0.0) {
        double pool = gxr_seconds(gxr_ticks() - g_pool_t0), busy = 0.0, idle = 0.0, thread_time;
        int i;
        for (i = 1; i <= g_workers; i++) { /* another thread's timers: rule 4, and charge()'s store */
            busy += gxr_seconds((uint64_t)plat_load64((plat_a64*)&g_ts[i].busy));
            idle += gxr_seconds((uint64_t)plat_load64((plat_a64*)&g_ts[i].idle));
        }
        thread_time = pool * g_workers;
        fprintf(stderr, "[gxr] workers: %d threads, pool up %.2fs each = %.2fs of thread time; busy %.2fs + idle %.2fs = %.2fs, %.1f%% unaccounted\n",
                g_workers, pool, thread_time, busy, idle, busy + idle,
                thread_time > 0.0 ? 100.0 * (thread_time - busy - idle) / thread_time : 0.0);
    }
    {
        uint64_t px = 0, rd = 0, ra = 0;
        int i;
        for (i = 0; i <= MAX_THREADS; i++) { px += g_ts[i].pixels; rd += g_ts[i].rej_depth; ra += g_ts[i].rej_alpha; }
    fprintf(stderr, "[gxr] %llu triangles, %llu lines, %llu points; %llu pixels shaded (%llu outside, %llu failed alpha, %llu failed depth); %llu clipped away; %llu bad vertex refs; %llu texture copies, %llu screen copies\n",
            (unsigned long long)g_tris, (unsigned long long)g_lines, (unsigned long long)g_points,
            (unsigned long long)px, (unsigned long long)g_rej_bary, (unsigned long long)ra, (unsigned long long)rd, (unsigned long long)g_clipped, (unsigned long long)g_verts_bad,
            (unsigned long long)g_copies_tex, (unsigned long long)g_copies_xfb);
    }
    if (g_backend && g_backend->report) g_backend->report();
    /* Two facts about lifetimes, stated when they happened at all, because
     * between them they say whether a run took the paths the queue's rules are
     * there for. The first is the only moment the renderer's own state moves
     * under a draw that is being built; the second is how far past a fixed
     * graveyard of 512 this run went, and every texture past that one is one
     * that would have been handed back to the allocator with queued draws
     * still pointing at it. */
    if (g_prepare_hazards)
        fprintf(stderr, "[gxr] %llu draws sampled a texture a queued copy writes, and waited for that copy in the middle of their setup\n",
                (unsigned long long)g_prepare_hazards);
    if (g_tokens_waited)
        fprintf(stderr, "[gxr] %llu draw tokens came while a copy before them was still running%s\n",
                (unsigned long long)g_tokens_waited,
                g_token_wait ? ", and waited for it" : "; SOA_GXR_TOKENWAIT=1 makes them wait");
    /* The fences (H14): how many commands carried one, and how long the
     * workers stood at them -- time already inside the idle above. */
    if (g_workers > 1) {
        uint64_t n = 0, ticks = 0;
        int i;
        for (i = 1; i <= g_workers; i++) { n += g_ts[i].fences; ticks += g_ts[i].fence_ticks; }
        if (n)
            fprintf(stderr, "[gxr] fences: %llu fenced commands run; the workers waited %.2fs at them (inside the idle above)\n",
                    (unsigned long long)(n / (uint64_t)g_workers), gxr_seconds(ticks));
    }
    if (tex_graveyard_peak())
        fprintf(stderr, "[gxr] %d decoded textures waited to be freed at once, at the most\n", tex_graveyard_peak());
    /* Where the producer's wait went, by reason (H14): seconds and how many. */
    {
        int i, any = 0;
        uint64_t n = 0;
        for (i = 0; i < W_COUNT; i++) n += g_wait_n[i];
        if (n) {
            fprintf(stderr, "[gxr] waits:");
            for (i = 0; i < W_COUNT; i++)
                if (g_wait_n[i])
                    fprintf(stderr, "%s %s %.2fs (%llu)", any++ ? "," : "", g_wait_name[i], gxr_seconds(g_wait_ticks[i]),
                            (unsigned long long)g_wait_n[i]);
            fprintf(stderr, "; %llu drains\n", (unsigned long long)n);
        }
    }
    tex_report();
    hostprof_report();
}
