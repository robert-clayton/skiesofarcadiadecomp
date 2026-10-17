/*
 * VIGetRetraceCount (0x8023F704), answered natively, so that the game's main
 * loop has a safe point and a tick the port can let go early
 * (docs/PLAN-60FPS-MODS.md M2).
 *
 * The original is two instructions, `lwz r3,-27836(r13); blr`: the retrace
 * count at 0x80347A64. Two of its callers are the main loop's:
 *
 *   801DCB84 bl VIGetRetraceCount   the top of the loop, which stores the
 *   801DCB88 stw r3,-28820(r13)     count as the frame's start (0x8034768C)
 *
 *   801DC498 bl VIGetRetraceCount   the frame end's spin, after the XFB copy:
 *   801DC49C lwz r0,-28820(r13)     it goes round while count - start < 1,
 *   801DC4A0 subf r0,r0,r3          each pass behind fn_801C6248(0)'s wait
 *   801DC4A4 cmpli cr0,r0,1         for a retrace -- the immediate that makes
 *   801DC4A8 blt 801DC490           the game two fields a frame
 *
 * Every translated call sets lr first, so the call site is s->lr here:
 *
 * - At 0x801DCB88, before the count is read, the safe-point callbacks run.
 *   Nothing of the frame has started: no GX, no guest thread mid-update --
 *   the one point a mod may call guest code (M4) or change what the frame
 *   does.
 * - At 0x801DC49C, once the tick is unlocked (SOA_UNCAP=N, from frame N), the
 *   answer is the frame's start plus one, so the spin leaves on its first
 *   pass and a frame needs the one field fn_801C6248 waits for, not two. The
 *   game's logic advances once a frame (FINDINGS "H2"), so this is the whole
 *   game at up to twice the speed -- a battle speed-up (M11) or a
 *   measurement, never 60 fps. H3 did the same by zeroing the stored start;
 *   answering here leaves that word as the game wrote it.
 * - At 0x801DC49C under the fixed-step clock (SOA_CLOCK=fixed), when the
 *   spin is about to go round again, irq.c (tick_set_spin_wait) delivers what is due or moves the
 *   clock to the next deadline, as its idle loop does: the spin is a wait,
 *   and going round it a million times to add up a field of delivery points
 *   would only cost the host time (docs/CLOCK.md).
 * - Anywhere else, and at both with nothing registered and the tick locked,
 *   it is the original, and selftest's twin case holds it to the recompiled
 *   one.
 */
#include "cpu.h"
#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RETRACE_COUNT 0x80347A64u      /* r13 - 27836 */
#define FRAME_START_FIELDS 0x8034768Cu /* r13 - 28820 */
#define LR_FRAME_START 0x801DCB88u
#define LR_FRAME_END_SPIN 0x801DC49Cu
#define SAFE_POINT_MAX 8

static void (*g_safe[SAFE_POINT_MAX])(CpuState*);
static int g_safe_n, g_reported;
static unsigned g_unlock_from; /* 0: locked */
static unsigned long long g_safe_points, g_released;

/* ---- turbo (M11a): up to 2x in battles or in the sky --------------------
 * SOA_TURBO=battle|sky|both arms it (`turbo`, recorded); the View+RS chord
 * toggles it, from the next safe point. Evaluated at the safe point and kept
 * for the frame: when on, the frame end's spin goes after one field, as
 * SOA_UNCAP's does, so the game runs up to twice as fast. battle is scene 7,
 * and scene 6 on a map of 500 or more (the ship battles); sky is the sky-mode
 * word, 1 on the world map and the ship-piloted maps. */
#define SCENE_ID 0x803475CCu
#define MAP_NUMBER 0x80311AC0u
#define SKY_MODE 0x80347464u
enum { TURBO_BATTLE = 1, TURBO_SKY = 2 };
unsigned gx_frame_count(void);
static int g_turbo_mode = -1; /* the SOA_TURBO bits, -1 before it is read */
static int g_turbo_armed, g_turbo_now;
static volatile int g_turbo_toggle;
static unsigned long long g_turbo_frames;

static void turbo_read(void)
{
    const char* v = getenv("SOA_TURBO");
    g_turbo_mode = 0;
    if (!v || !*v) return;
    if (!strcmp(v, "battle")) g_turbo_mode = TURBO_BATTLE;
    else if (!strcmp(v, "sky")) g_turbo_mode = TURBO_SKY;
    else if (!strcmp(v, "both")) g_turbo_mode = TURBO_BATTLE | TURBO_SKY;
    else fprintf(stderr, "[turbo] SOA_TURBO=%s is not battle, sky or both; off\n", v);
    g_turbo_armed = g_turbo_mode != 0;
    if (g_turbo_armed) fprintf(stderr, "[turbo] armed for %s: up to 2x there\n", v);
}

/* The chord (main.c): flip turbo from the next safe point. The answer is
 * what it will be. */
int tick_turbo_toggle(void)
{
    if (g_turbo_mode < 0) turbo_read();
    g_turbo_toggle = 1;
    return !g_turbo_armed;
}

/* Whether this frame runs at turbo: for the presenter's sync interval. */
int tick_turbo_now(void)
{
    return g_turbo_now;
}

static void turbo_evaluate(CpuState* s)
{
    uint32_t scene;
    int mode;
    if (g_turbo_mode < 0) turbo_read();
    if (g_turbo_toggle) {
        g_turbo_toggle = 0;
        g_turbo_armed = !g_turbo_armed;
        fprintf(stderr, "[turbo] frame %u: %s\n", gx_frame_count(), g_turbo_armed ? "on" : "off");
    }
    mode = g_turbo_mode ? g_turbo_mode : TURBO_BATTLE | TURBO_SKY; /* armed by the chord alone: both */
    scene = mem_r32(s, SCENE_ID);
    g_turbo_now = g_turbo_armed && (((mode & TURBO_BATTLE) && (scene == 7 || (scene == 6 && mem_r32(s, MAP_NUMBER) >= 500))) ||
                                    ((mode & TURBO_SKY) && mem_r32(s, SKY_MODE) == 1));
    if (g_turbo_now) g_turbo_frames++;
}

void hle_on_report(void (*fn)(void));
unsigned gx_frame_count(void);

static void tick_report(void)
{
    fprintf(stderr, "[tick] %llu safe points over %u presented frames", g_safe_points, gx_frame_count());
    if (g_unlock_from)
        fprintf(stderr, "; from frame %u the frame end's spin was let go after one field %llu times\n",
                g_unlock_from, g_released);
    else
        fprintf(stderr, "; the tick is locked, two fields a frame\n");
    if (g_turbo_mode > 0 || g_turbo_frames)
        fprintf(stderr, "[turbo] %llu frame(s) at turbo\n", g_turbo_frames);
}

static void tick_register_report(void)
{
    if (g_reported) return;
    g_reported = 1;
    hle_on_report(tick_report);
}

/* A callback for the top of every frame; 0 when the table is full. */
int tick_on_safe_point(void (*fn)(CpuState*))
{
    if (g_safe_n == SAFE_POINT_MAX) return 0;
    g_safe[g_safe_n++] = fn;
    return 1;
}

/* While `held` says so, the top of the main loop waits (M19's pause: the
 * clock does not count the time). A setter, so this file still links alone. */
static int (*g_held)(void);

void tick_set_hold(int (*held)(void))
{
    g_held = held;
}

static void hold_while_paused(void)
{
    while (g_held && g_held()) plat_sleep_ms(20);
}

/* What the frame end's spin does when it is about to go round again: set
 * (to irq.c's irq_fixed_wait) only under the fixed-step clock. A setter, as
 * the hold's is, so this file still links alone. */
static void (*g_spin_wait)(CpuState*);

void tick_set_spin_wait(void (*fn)(CpuState*))
{
    g_spin_wait = fn;
}

/* Let the frame end's spin go after one field, from frame `frame` on; 0
 * locks it again. */
void tick_unlock_from(unsigned frame)
{
    g_unlock_from = frame;
}

void fn_8023F704(CpuState* s)
{
    s->pc = 0x8023F704u; /* first, as the translation's block head does: a sample in here names it (test_hle_pc) */
    tick_register_report();
    if (s->lr == LR_FRAME_START) {
        int i;
        g_safe_points++;
        hold_while_paused();
        turbo_evaluate(s);
        for (i = 0; i < g_safe_n; i++) g_safe[i](s);
    } else if (s->lr == LR_FRAME_END_SPIN && ((g_unlock_from && gx_frame_count() >= g_unlock_from) || g_turbo_now)) {
        g_released++;
        s->gpr[3] = mem_r32(s, FRAME_START_FIELDS) + 1;
        return;
    } else if (s->lr == LR_FRAME_END_SPIN && g_spin_wait &&
               mem_r32(s, RETRACE_COUNT) - mem_r32(s, FRAME_START_FIELDS) < 1u) {
        g_spin_wait(s); /* the spin goes round again: under the fixed clock, a wait (docs/CLOCK.md) */
    }
    s->gpr[3] = mem_r32(s, RETRACE_COUNT);
}
