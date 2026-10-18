/*
 * strict_host: a host of the runtime that drives it as a game engine's
 * editor would, from threads of its own at times of its own, to show that
 * strict mode (runtime/soa_host.h, docs/CLOCK.md "Deterministic runs under a
 * host") makes the run repeat whatever those times are.
 *
 *   strict_host GAME DISC ROOT SEED MAXDELAY_US strict|loose OUT
 *
 * STRICT_PAD="frame:start|a,..." replaces the script below with those
 * presses alone (docs/CLOCK.md: the crescent check).
 *
 * GAME is libsoa_game.so of the build whose host runtime (recompile.py
 * --split --host) this is linked against; the run stops at SOA_FRAMES.
 *
 * Two threads besides the runtime's, as an engine has: a "tick" thread (the
 * engine's game thread) that waits for the guest's held frame F, sleeps a
 * random while, hashes the game's memory as the frame left it, and asks an
 * "input" thread for the pad of frame F+1; the input thread sleeps a random
 * while and sets it with soa_host_set_pad; the tick thread sleeps again and
 * lets F go. At frame 2000 the tick thread also schedules a 5 ms event while
 * the guest is held, which notes the tick each run lands on. Delays are
 * drawn from SEED, up to MAXDELAY_US each: two runs with different seeds and
 * delays differ only in host-side timing.
 *
 * "loose" is the same host without strict mode: the fixed clock, the old
 * hold (half a second at most), the pad set whenever the input thread gets
 * to it for whichever frame the tick thread last saw. It shows what strict
 * mode is for.
 *
 * OUT gets one line a frame: the frame, a hash of a megabyte of the game's
 * globals as the frame ended, every 100th frame a hash of all of MEM1, and at
 * the end the strict stats, the event's ticks and a hash of every call count.
 * Each run of the event adds a line too: its due and its tick.
 *
 * Save states (docs/SNAPSHOT.md): STRICT_SNAPSHOT=F:path asks for one while
 * frame F is held (it is of frame F+1); STRICT_RESUME=path starts the run
 * from one. The event's count and running hash are this host's own state,
 * so they go into the snapshot as a section of its own (the snapshot hook,
 * soa_host_snapshot_put) and come back with soa_host_snapshot_get: a resumed
 * run's last line is the whole run's.
 */
#define _GNU_SOURCE
#include "soa_host.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MEM1 0x01800000u

static unsigned g_seed;
static unsigned g_maxdelay;
static int g_strict;
static FILE* g_out;

static unsigned rnd(void)
{
    g_seed = g_seed * 1103515245u + 12345u;
    return (g_seed >> 8) & 0xFFFFFFu;
}

static void delay(void)
{
    unsigned us = g_maxdelay ? rnd() % (g_maxdelay + 1) : 0;
    if (us) usleep(us);
}

static uint64_t fnv(uint64_t h, const uint8_t* p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

/* The pad of frame f: the opening's script (tools/scenario.py's speed and
 * battle preamble), the stick swung now and then besides. */
static void script(long f, SoaHostPad* p)
{
    static const long starts[] = {1600, 1800, 2000, 2200, 2400, 2600, 2800, 3000, 3200};
    const char* own = getenv("STRICT_PAD");
    unsigned k;
    memset(p, 0, sizeof *p);
    p->stick[0] = p->stick[1] = p->cstick[0] = p->cstick[1] = 128;
    if (own) { /* STRICT_PAD="1600:start,2200:a,...": these alone, each held 10 frames, as SOA_PAD's are */
        while (*own) {
            char* end;
            long at = strtol(own, &end, 10);
            unsigned b = 0;
            if (*end != ':') break;
            own = end + 1;
            if (!strncmp(own, "start", 5)) b = 0x1000, own += 5;
            else if (*own == 'a') b = 0x0100, own += 1;
            if (f >= at && f < at + 10) p->buttons |= (uint16_t)b;
            if (*own == ',') own++;
        }
        return;
    }
    for (k = 0; k < sizeof starts / sizeof starts[0]; k++) {
        if (f >= starts[k] && f < starts[k] + 10) p->buttons |= 0x1000;           /* START */
        if (f >= starts[k] + 40 && f < starts[k] + 50) p->buttons |= 0x0100;      /* A */
    }
    if (f >= 3600 && (f - 3600) % 150 < 10) p->buttons |= 0x0100;
    if (f % 97 < 7) p->stick[1] = (uint8_t)(200 + f % 50);
    if (f % 131 < 5) p->trig[0] = (uint8_t)(f % 200);
}

/* ---- the input thread ---- */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static long g_want = -1, g_done = -1; /* the frame whose pad is asked for, and the last one set */

static void* input_main(void* arg)
{
    (void)arg;
    for (;;) {
        long f;
        SoaHostPad p;
        pthread_mutex_lock(&g_mu);
        while (g_want == g_done) pthread_cond_wait(&g_cv, &g_mu);
        f = g_want;
        pthread_mutex_unlock(&g_mu);
        delay();
        script(f, &p);
        soa_host_set_pad(1, &p);
        pthread_mutex_lock(&g_mu);
        g_done = f;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}

static void ask_pad(long f, int wait)
{
    pthread_mutex_lock(&g_mu);
    g_want = f;
    pthread_cond_broadcast(&g_cv);
    while (wait && g_done != f) pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
}

/* ---- the event ---- */
static uint64_t g_ev_n, g_ev_hash = 1469598103934665603ull;

static void tick_event(void* user, SoaHostCall* call, uint64_t due, uint64_t now)
{
    (void)user, (void)call;
    g_ev_n++;
    g_ev_hash = fnv(g_ev_hash, (const uint8_t*)&due, sizeof due);
    g_ev_hash = fnv(g_ev_hash, (const uint8_t*)&now, sizeof now);
    fprintf(g_out, "event %llu due %llu now %llu\n", (unsigned long long)g_ev_n, (unsigned long long)due, (unsigned long long)now);
}

/* This host's state as a snapshot is taken: on the game's thread, at the
 * frame's start the snapshot is of. */
static void snapshot_hook(void* user)
{
    uint64_t mine[2];
    (void)user;
    mine[0] = g_ev_n;
    mine[1] = g_ev_hash;
    soa_host_snapshot_put("strict", mine, sizeof mine);
}

static uint8_t g_mem[MEM1];

static void note_frame(long f)
{
    uint64_t h = fnv(1469598103934665603ull, NULL, 0);
    if (!soa_host_read(0x80300000u, g_mem, 0x100000u)) return;
    h = fnv(h, g_mem, 0x100000u);
    fprintf(g_out, "frame %ld globals %016llx", f, (unsigned long long)h);
    if (f % 100 == 0 && soa_host_read(0x80000000u, g_mem, MEM1))
        fprintf(g_out, " mem1 %016llx", (unsigned long long)fnv(1469598103934665603ull, g_mem, MEM1));
    fputc('\n', g_out);
    fflush(g_out);
}

static void note_end(void)
{
    static SoaHostCallCount counts[16384];
    unsigned n = soa_host_call_counts(counts, 16384), k;
    uint64_t h = 1469598103934665603ull;
    SoaHostStrictStats st;
    for (k = 0; k < n && k < 16384; k++) {
        h = fnv(h, (const uint8_t*)&counts[k].address, sizeof counts[k].address);
        h = fnv(h, (const uint8_t*)&counts[k].calls, sizeof counts[k].calls);
    }
    soa_host_strict_stats(&st);
    fprintf(g_out, "end: %u functions called, counts %016llx; event ran %llu times, ticks %016llx; "
                   "strict: %llu held, %llu pads, %llu late, %llu refused\n",
            n, (unsigned long long)h, (unsigned long long)g_ev_n, (unsigned long long)g_ev_hash,
            (unsigned long long)st.held, (unsigned long long)st.pads, (unsigned long long)st.late,
            (unsigned long long)st.refused);
    fflush(g_out);
}

int main(int argc, char** argv)
{
    SoaHostStart start;
    char why[256];
    pthread_t input;
    long last = -1, frames, snap_at = -1;
    const char* snap_path = NULL;
    int snap_said = 0;
    const char* e = getenv("SOA_FRAMES");
    if (argc != 8) {
        fprintf(stderr, "usage: strict_host GAME DISC ROOT SEED MAXDELAY_US strict|loose OUT (SOA_FRAMES ends the run)\n");
        return 2;
    }
    frames = e ? atol(e) : 0;
    if (frames < 2) {
        fprintf(stderr, "strict_host: set SOA_FRAMES\n");
        return 2;
    }
    g_seed = (unsigned)atoi(argv[4]);
    g_maxdelay = (unsigned)atoi(argv[5]);
    g_strict = !strcmp(argv[6], "strict");
    g_out = fopen(argv[7], "w");
    if (!g_out) return 2;
    if (soa_host_abi() != SOA_HOST_ABI) return 2;
    if (g_strict) {
        if (!soa_host_set_strict(SOA_HOST_STRICT_ON | SOA_HOST_STRICT_COUNT_CALLS)) return 2;
    } else {
        soa_host_set_clock(SOA_HOST_CLOCK_FIXED);
        soa_host_hold(1);
        setenv("SOA_COUNT_CALLS", "1", 0);
    }
    {
        const char* resume = getenv("STRICT_RESUME");
        soa_host_set_snapshot_hook(snapshot_hook, NULL);
        if (resume && *resume) {
            uint64_t mine[2];
            if (soa_host_resume(resume, why, sizeof why)) {
                fprintf(stderr, "strict_host: %s\n", why);
                return 1;
            }
            if (soa_host_snapshot_get("strict", mine, sizeof mine) == (long)sizeof mine) g_ev_n = mine[0], g_ev_hash = mine[1];
            fprintf(g_out, "resumed at frame %ld\n", soa_host_resumed_frame());
        }
    }
    pthread_create(&input, NULL, input_main, NULL);
    start.abi = SOA_HOST_ABI;
    start.game = argv[1];
    start.disc = argv[2];
    start.root = argv[3];
    if (soa_host_start(&start, why, sizeof why)) {
        fprintf(stderr, "strict_host: %s\n", why);
        return 1;
    }
    {
        const char* snap = getenv("STRICT_SNAPSHOT");
        if (snap && *snap) {
            char* colon;
            snap_at = strtol(snap, &colon, 10);
            if (*colon == ':') snap_path = colon + 1;
        }
    }
    for (;;) {
        long f;
        int rc;
        if (soa_host_state(&rc) == SOA_HOST_ENDED) break;
        if (g_strict) {
            f = soa_host_held();
            if (f < 0 || f == last) {
                usleep(50);
                continue;
            }
            delay();
            note_frame(f);
            if (snap_path && f == snap_at && !soa_host_snapshot(snap_path)) fprintf(stderr, "strict_host: soa_host_snapshot refused\n");
            if (snap_path && f > snap_at && !snap_said) {
                long at = -1;
                int st = soa_host_snapshot_status(&at, why, sizeof why);
                if (st) {
                    fprintf(g_out, "snapshot %s at frame %ld%s%s\n", st > 0 ? "written" : "failed", at, st < 0 ? ": " : "", st < 0 ? why : "");
                    snap_said = 1;
                }
            }
            if (f == 2000) soa_host_schedule(soa_host_guest_ticks() + 1000, 202500, tick_event, NULL);
            ask_pad(f + 1, 1); /* the input thread sets it, at a time of its own, before this frame is let go */
            if (f >= frames - 1) note_end();
            delay();
            last = f;
            soa_host_go(f);
        } else {
            /* the engine's own frame: whatever the guest is at when it looks */
            const uint8_t* rgba;
            int w, h, stride;
            usleep(1000 + rnd() % (g_maxdelay + 1));
            f = soa_host_frame(&rgba, &w, &h, &stride);
            if (f == last) continue;
            note_frame(f);
            if (f >= 2000 && last < 2000) soa_host_schedule(soa_host_guest_ticks() + 1000, 202500, tick_event, NULL);
            ask_pad(f + 1, 0);
            if (f >= frames - 3) note_end();
            last = f;
            soa_host_go(f);
        }
    }
    return 0;
}
