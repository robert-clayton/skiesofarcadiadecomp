/*
 * exact_host: a host of the runtime that holds soa_host_game_exact
 * (runtime/soa_host.h, docs/SWAP.md "Exact calls") to the translation itself
 * on the game's own calls of a function that answers in f1.
 *
 *   exact_host GAME DISC ROOT [ADDRESS...]
 *
 * GAME is libsoa_game.so of the build whose host runtime (recompile.py
 * --split --host) this is linked against; the run stops at SOA_FRAMES. Each
 * ADDRESS (hex; MSL's cosf, 0x80262A34, when none is given) is replaced by an
 * answer that, at every call the game makes:
 *
 *   - keeps the game's registers as the call found them, r0-r31, f0-f31 (both
 *     halves), CR, XER, LR, CTR and FPSCR;
 *   - runs the function's own translation on them (`original`) and keeps the
 *     registers it leaves: what the game would have had;
 *   - puts the registers back as they were and calls the function exactly
 *     (soa_host_game_exact, SOA_HOST_EXACT_ORIGINAL, LR as the call found
 *     it), and holds every register it leaves to the translation's, bit for
 *     bit;
 *   - puts them back again and calls it through `game`, to show what that
 *     call gives: r3, and every register as it was (f1 not the result);
 *   - answers with what the exact call left (SOA_HOST_ANSWERED_LIVE).
 *
 * Every second of the run, and at the end if the run ends by itself
 * (SOA_FRAMES leaves the process at once, so the last second's line is the
 * run's, less what the game called in that second), one line an address:
 * the calls so far, how many exact calls differed from the translation in
 * any register (and in f1 alone), and how many `game` calls left f1 other
 * than the translation's. The function runs three times a
 * call; for one that does work in memory (a turn by a step, a counter), name
 * it with EXACT_MEMORY=1: the game's RAM is then copied before the first run,
 * put back before the second and third, and the exact call's held to the
 * translation's too (a "memory differs" count), at 24 MB a copy.
 *
 * Measured (2026-10-19; the fork's docs/SWAP.md, "Exact calls"): MSL's cosf,
 * cos and the kernel under them over the opening and towns routes, and the
 * look-at helper 0x80091040 in towns with EXACT_MEMORY=1: no call differed.
 *
 *   gcc -O2 -Iruntime tools/exact_host.c -L<out> -l:libsoa_runtime.so -Wl,-rpath,<out> -o exact_host
 */
#define _DEFAULT_SOURCE /* usleep */
#include "soa_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    uint32_t r[32];
    SoaHostPair f[32];
    SoaHostSpecial sp;
} Regs;

typedef struct {
    uint32_t address;
    unsigned long long calls, differ, f1_differ, game_f1_differ, refused, mem_differ;
} Watch;

static int g_memory;            /* EXACT_MEMORY=1 */
static uint8_t *g_before, *g_after;

#define MAX_WATCH 16
static Watch g_watch[MAX_WATCH];
static unsigned g_nwatch;

static void report(long second)
{
    unsigned k;
    for (k = 0; k < g_nwatch; k++)
        printf("[exact] second %ld %08X calls %llu exact differs %llu (f1 %llu) refused %llu; game's f1 not the "
               "translation's %llu; memory differs %llu\n", second, g_watch[k].address, g_watch[k].calls, g_watch[k].differ,
               g_watch[k].f1_differ, g_watch[k].refused, g_watch[k].game_f1_differ, g_watch[k].mem_differ);
    fflush(stdout);
}

static void take(SoaHostCall* c, Regs* o)
{
    memset(o, 0, sizeof *o); /* the struct's padding too: it is compared whole */
    memcpy(o->r, c->r, sizeof o->r);
    memcpy(o->f, c->f, sizeof o->f);
    soa_host_special(c, &o->sp, NULL);
}

static void give(SoaHostCall* c, const Regs* o)
{
    memcpy(c->r, o->r, sizeof o->r);
    memcpy(c->f, o->f, sizeof o->f);
    soa_host_special(c, NULL, &o->sp);
}

static int answer(void* user, SoaHostCall* c)
{
    Watch* w = (Watch*)user;
    Regs in, translated, exact, game;
    uint32_t r3 = 0;
    w->calls++;
    take(c, &in);
    if (g_memory) memcpy(g_before, c->mem, c->mem_bytes);
    c->original(c);
    take(c, &translated);
    if (g_memory) {
        memcpy(g_after, c->mem, c->mem_bytes);
        memcpy(c->mem, g_before, c->mem_bytes);
    }
    give(c, &in);
    if (!soa_host_game_exact(c, c->address, in.sp.lr, SOA_HOST_EXACT_ORIGINAL)) {
        w->refused++;
        give(c, &translated);
        return SOA_HOST_ANSWERED_LIVE;
    }
    take(c, &exact);
    if (memcmp(&exact, &translated, sizeof exact) && !w->differ++) {
        unsigned k;
        fprintf(stderr, "[exact] %08X call %llu: the exact call left other registers than the translation:",
                w->address, w->calls);
        for (k = 0; k < 32; k++)
            if (exact.r[k] != translated.r[k]) fprintf(stderr, " r%u %08X/%08X", k, exact.r[k], translated.r[k]);
        for (k = 0; k < 32; k++)
            if (memcmp(&exact.f[k], &translated.f[k], sizeof exact.f[k]))
                fprintf(stderr, " f%u %a %a/%a %a", k, exact.f[k].ps0, exact.f[k].ps1, translated.f[k].ps0,
                        translated.f[k].ps1);
        if (memcmp(&exact.sp, &translated.sp, sizeof exact.sp))
            fprintf(stderr, " cr %08X/%08X xer %08X/%08X lr %08X/%08X ctr %08X/%08X fpscr %08X/%08X", exact.sp.cr,
                    translated.sp.cr, exact.sp.xer, translated.sp.xer, exact.sp.lr, translated.sp.lr, exact.sp.ctr,
                    translated.sp.ctr, exact.sp.fpscr, translated.sp.fpscr);
        fprintf(stderr, "\n");
    }
    if (memcmp(&exact.f[1], &translated.f[1], sizeof exact.f[1])) w->f1_differ++;
    if (g_memory) {
        if (memcmp(c->mem, g_after, c->mem_bytes)) w->mem_differ++;
        memcpy(g_after, c->mem, c->mem_bytes); /* the exact call's, kept for the game */
        memcpy(c->mem, g_before, c->mem_bytes);
    }
    give(c, &in);
    c->game_original(c, c->address, in.r + 3, 8, &r3);
    take(c, &game);
    if (memcmp(&game.f[1], &translated.f[1], sizeof game.f[1])) w->game_f1_differ++;
    if (g_memory) memcpy(c->mem, g_after, c->mem_bytes);
    give(c, &exact);
    return SOA_HOST_ANSWERED_LIVE;
}

int main(int argc, char** argv)
{
    SoaHostStart start;
    char why[256];
    int rc = 0, i;
    unsigned k;
    if (argc < 4) {
        fprintf(stderr, "usage: exact_host GAME DISC ROOT [ADDRESS...]\n");
        return 2;
    }
    if (soa_host_abi() != SOA_HOST_ABI) return 2;
    soa_host_set_clock(SOA_HOST_CLOCK_FIXED);
    if (getenv("EXACT_MEMORY") && atoi(getenv("EXACT_MEMORY"))) {
        g_memory = 1;
        g_before = malloc(0x01800000u);
        g_after = malloc(0x01800000u);
        if (!g_before || !g_after) return 2;
    }
    for (i = 4; i < argc && g_nwatch < MAX_WATCH; i++) g_watch[g_nwatch++].address = (uint32_t)strtoul(argv[i], NULL, 16);
    if (!g_nwatch) g_watch[g_nwatch++].address = 0x80262A34u; /* MSL's cosf */
    for (k = 0; k < g_nwatch; k++)
        if (!soa_host_replace(g_watch[k].address, answer, &g_watch[k])) {
            fprintf(stderr, "%08X: refused\n", g_watch[k].address);
            return 2;
        }
    start.abi = SOA_HOST_ABI;
    start.game = argv[1];
    start.disc = argv[2];
    start.root = argv[3];
    if (soa_host_start(&start, why, sizeof why)) {
        fprintf(stderr, "%s\n", why);
        return 1;
    }
    for (i = 1; soa_host_state(&rc) != SOA_HOST_ENDED; i++) {
        usleep(20000);
        if (i % 50 == 0) report(i / 50); /* the counts are the game thread's, read here a call behind at most */
    }
    report(-1);
    return rc;
}
