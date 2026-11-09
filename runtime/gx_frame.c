/*
 * What the runtime keeps of the graphics processor whether or not GX is built
 * (soa-ue5's U4.2): the frame -- its count, its end, the frame hook and
 * SOA_FRAMES's stop --, the display-list hooks' registration, the gather
 * pipe's watchers, and the no-FIFO switch (SOA_GX=off) with what its host
 * answers through soa_host.h's GX block.
 *
 * gx.c ends a frame at a copy to the external frame buffer, with its captures
 * around the end (gx_frame_hook_run, gx_frame_next, gx_frame_stop). With
 * SOA_GX=off, or in a build without GX (--no-gx: gx.c, gxr.c, gxr_tev.c and
 * gxv.c left out, gx_none.c in their place), nothing parses the stream, and
 * the host that answers GX natively ends each frame at its display copy
 * (soa_host_gx_frame_end, gx_off_frame_end below): the frame hook, the count,
 * and SOA_FRAMES's stop, at the same byte gx.c ended it.
 *
 * Every function here was gx.c's, and does what it did there.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hle_report(void);

/* ---- the frame ---------------------------------------------------------- */

/* Frames end at a copy to the XFB: what SOA_FRAMES, SOA_PAD and every
 * frame-keyed switch count. */
static unsigned g_frame;

/* SOA_FRAMES=n stops the run at the frame's end rather than in the renderer,
 * so it counts the frames the game presents whether or not anything is being
 * drawn, and counts the same ones SOA_PAD scripts against. main sets it once
 * the guest is about to run, which keeps --replay and the selftest out of it. */
static unsigned g_frame_limit;

/* Something to do at each frame boundary, if anyone has asked. main.c hangs
 * SOA_POKE off this rather than the frame's end calling into main.c, because
 * the renderer links on its own -- render_check.py and the four gxr tests
 * build gx.c, gx_frame.c, gxr.c, gxr_tev.c and png.c with two stubs and no main.c, and
 * PLAN A1 counts those two stubs as a property worth keeping. A direct call
 * compiled fine and broke 29 tests at the link step, which is the failure the
 * compile-only CI job cannot see. */
static void (*g_frame_hook)(CpuState*, unsigned);

unsigned gx_frame_count(void) { return g_frame; }

void gx_set_frame_hook(void (*fn)(CpuState*, unsigned))
{
    g_frame_hook = fn;
}

void gx_set_frame_limit(unsigned frames)
{
    g_frame_limit = frames;
}

/* The frame's end, in three steps a capture fits between (gx.c): the hook,
 * with the frame that is ending; the count, with whether SOA_FRAMES is
 * reached; and the stop. */
void gx_frame_hook_run(CpuState* s)
{
    if (g_frame_hook) g_frame_hook(s, g_frame);
}

int gx_frame_next(void)
{
    g_frame++;
    return g_frame_limit && g_frame >= g_frame_limit;
}

/* `finish`, when given, first finishes what is still being drawn (gx.c's
 * gxr_flush: any snapshot is written and the counters are real before the
 * report). Leave with _Exit (C99's _exit) for the reason window.c does: the
 * rasterizer's workers may be spinning, and CRT teardown around them can
 * hang. */
void gx_frame_stop(void (*finish)(void))
{
    fprintf(stderr, "[boot] %u frames done (SOA_FRAMES)\n", g_frame);
    if (finish) finish();
    hle_report();
    fflush(NULL);
    _Exit(0);
}

/* The frame's end with nothing captured and nothing drawn: the runtime's own. */
void gx_frame_end(CpuState* s)
{
    gx_frame_hook_run(s);
    if (gx_frame_next()) gx_frame_stop(NULL);
}

/* A snapshot's frame count, in gx.c's "gx" section where it always was (and
 * gx_none.c's). */
#include "snapshot.h"
void gx_frame_snap(Snap* sn)
{
    SNAP(sn, g_frame);
}

/* ---- the display-list hooks --------------------------------------------- */

/* Something that follows the game's display lists: runtime/ninja.c, for the
 * models whose strips the game records into a list instead of drawing them
 * (ninja.c's draw modes). `recording` is told when GXBeginDisplayList starts
 * a list at `addr`, whatever that buffer held before. `called` is asked
 * before the list at `addr`, `size` bytes long, is parsed, and again each
 * time the parse reaches the point it named: `done` bytes are parsed, GX's
 * registers are as the list's commands have left them so far, and it answers
 * how many more bytes to parse before the next ask -- 0, or more than are
 * left, for the rest. Pointers for the same reason as the frame hook above:
 * the renderer links without ninja.c. Only gx.c's parse calls them: with GX
 * off, or not built, no list is parsed here (the host records its own). */
void (*g_gx_list_recording)(CpuState*, uint32_t);
uint32_t (*g_gx_list_called)(CpuState*, uint32_t, uint32_t, uint32_t);

void gx_set_list_hooks(void (*recording)(CpuState* s, uint32_t addr),
                       uint32_t (*called)(CpuState* s, uint32_t addr, uint32_t size, uint32_t done))
{
    g_gx_list_recording = recording;
    g_gx_list_called = called;
}

/* ---- the pipe's watchers ------------------------------------------------ */

/* SOA_GX_WRITERS=1: which guest code feeds the gather pipe, by the block
 * doing the store and the link register at the time (its caller, for a leaf
 * function), with the bytes each pair wrote; the busiest at exit. Finds the
 * routines that emit vertices, which the address alone of a draw command
 * cannot: the stream carries no return addresses. */
#define WRITERS_CAP 4096
typedef struct { uint32_t pc, lr; uint64_t bytes; } Writer;
static Writer g_writers[WRITERS_CAP];
static int g_writers_on = -1;

static int writer_cmp(const void* a, const void* b)
{
    uint64_t x = ((const Writer*)a)->bytes, y = ((const Writer*)b)->bytes;
    return x < y ? 1 : x > y ? -1 : 0;
}

void gx_writers_report(void)
{
    unsigned i, n = 0;
    uint64_t total = 0;
    if (g_writers_on <= 0) return; /* SOA_GX_WRITERS */
    for (i = 0; i < WRITERS_CAP; i++)
        if (g_writers[i].bytes) { g_writers[n++] = g_writers[i]; total += g_writers[i].bytes; }
    qsort(g_writers, n, sizeof g_writers[0], writer_cmp);
    fprintf(stderr, "[writers] %u block/caller pairs wrote %llu gather-pipe bytes; the busiest:\n", n,
            (unsigned long long)total);
    for (i = 0; i < n && i < 40; i++)
        fprintf(stderr, "[writers]   block %08X lr %08X  %llu bytes (%.1f%%)\n", g_writers[i].pc, g_writers[i].lr,
                (unsigned long long)g_writers[i].bytes, total ? 100.0 * g_writers[i].bytes / total : 0.0);
}

static void writer_note(const CpuState* s, unsigned size)
{
    unsigned h = ((s->pc * 2654435761u) ^ (s->lr * 40503u)) & (WRITERS_CAP - 1), k;
    for (k = 0; k < WRITERS_CAP; k++, h = (h + 1) & (WRITERS_CAP - 1)) {
        Writer* w = &g_writers[h];
        if (w->bytes && (w->pc != s->pc || w->lr != s->lr)) continue;
        w->pc = s->pc;
        w->lr = s->lr;
        w->bytes += size;
        return;
    }
}

/* Who else watches the pipe's bytes: the census (SOA_PIPE_CENSUS, swap.c's:
 * the bytes each translated function writes itself; main.c hands it over
 * with gx_set_pipe_census) and the recorder (record.c sets g_record_pipe for
 * the length of a recorded call, so the bytes the call writes are kept with
 * it). One test of g_gx_pipe_watch on the way in (gx_pipe_write) when none of
 * them is on. */
void (*g_record_pipe)(CpuState* s, unsigned size, uint64_t v);
static void (*g_census_note)(uint32_t pc, unsigned size);
int g_gx_pipe_watch = -1; /* writers, census or the recorder: -1 until first asked */

void gx_pipe_watch_changed(void) { g_gx_pipe_watch = -1; } /* record.c, as it sets or clears g_record_pipe */

void gx_set_pipe_census(void (*note)(uint32_t pc, unsigned size))
{
    g_census_note = note;
    g_gx_pipe_watch = -1;
}

void gx_pipe_watch(CpuState* s, unsigned size, uint64_t v)
{
    void (*rec)(CpuState*, unsigned, uint64_t);
    if (g_gx_pipe_watch < 0) {
        if (g_writers_on < 0) {
            const char* e = getenv("SOA_GX_WRITERS");
            g_writers_on = e && atoi(e);
        }
        g_gx_pipe_watch = g_writers_on || g_census_note || g_record_pipe;
        if (!g_gx_pipe_watch) return;
    }
    if (g_writers_on) writer_note(s, size);
    if (g_census_note) g_census_note(s->pc, size);
    rec = g_record_pipe;
    if (rec) rec(s, size, v);
}

/* ---- the no-FIFO switch ------------------------------------------------- */

/* SOA_GX=off (soa-ue5's U4.1, "the no-FIFO mode"): the command processor's model and the renderer are off. A host
 * answers natively what the game reads back of them -- CP status, the PI FIFO pointers, the pixel engine's token and
 * finish, the display lists' bytes in RAM and their lengths -- and keeps feeding its own draw machine, but nothing
 * reaches gx.c: a store to the gather pipe is counted and dropped, an access to a CP, PE or PI FIFO register
 * counted, its first few named, and answered with zero (a read) or dropped (a write), and nothing is parsed, drawn,
 * copied or captured. What the stream did for the rest of the runtime the host does through soa_host.h's GX block:
 * the frame's end at its display copy (soa_host_gx_frame_end: gx_frame_end above, without the renderer), a register
 * access's clock charge (soa_host_gx_charge, the mmio wrappers' own), the pixel engine's interrupts
 * (soa_host_gx_native's pending callback, asked where gx_pe_irq_pending was) and the bytes it drew (its counter,
 * which the quiet rule's wait finder reads where it read the pipe's bytes). The report gives the counts, which a
 * check run holds to 0 (soa-ue5's U4.2 gate). Without the switch the runtime is as before. A build without GX
 * (--no-gx) is always off. */
int g_gx_off = -1;
static uint64_t g_off_reads, g_off_writes, g_off_pipe, g_off_frames;
static unsigned (*g_host_pe)(void* user);
static void* g_host_pe_user;
static const volatile uint64_t* g_host_drawn;

int gx_is_off(void)
{
    if (g_gx_off < 0) {
#ifdef SOA_NO_GX
        g_gx_off = 1;
        fprintf(stderr, "[gx] off: this runtime is built without GX (--no-gx): no command stream parsed, nothing drawn; the host "
                        "answers the CP, PE and PI FIFO\n");
#else
        const char* v = getenv("SOA_GX");
        g_gx_off = v && strcmp(v, "off") == 0;
        if (g_gx_off) fprintf(stderr, "[gx] off (SOA_GX=off): no command stream parsed, nothing drawn; the host answers the CP, PE and PI FIFO\n");
#endif
    }
    return g_gx_off;
}

/* An access to a CP, PE or PI FIFO register with GX off: counted, its first few named. */
void gx_off_access(CpuState* s, const char* dir, uint32_t ea, unsigned size, uint64_t v)
{
    if (g_off_reads + g_off_writes < 8)
        fprintf(stderr, "[gx] off: %s %08X/%u = %llx at pc %08X lr %08X\n", dir, ea, size, (unsigned long long)v,
                s ? s->pc : 0u, s ? s->lr : 0u);
    if (dir[0] == 'r') g_off_reads++;
    else g_off_writes++;
}

/* A store to the gather pipe with GX off: counted, its first few named, and dropped. */
void gx_off_pipe(CpuState* s, unsigned size, uint64_t v)
{
    if (g_off_pipe < 8)
        fprintf(stderr, "[gx] off: %u pipe byte(s) %llx at pc %08X lr %08X\n", size, (unsigned long long)v, s ? s->pc : 0u,
                s ? s->lr : 0u);
    g_off_pipe += size;
}

/* The host's native GX (soa_host_gx_native): its pending pixel-engine interrupts, and the bytes it drew. */
int gx_set_native(unsigned (*pe_pending)(void* user), void* user, const volatile uint64_t* drawn)
{
    if (!gx_is_off()) return 0;
    g_host_pe = pe_pending;
    g_host_pe_user = user;
    g_host_drawn = drawn;
    return 1;
}

/* Bit 0: a token interrupt is due; bit 1: a finish interrupt is due -- the host's pixel engine's. */
unsigned gx_off_pe_pending(void)
{
    return g_host_pe ? g_host_pe(g_host_pe_user) & 3u : 0u;
}

/* The bytes the host drew (0 before it says where it counts them). */
uint64_t gx_off_drawn(void)
{
    return g_host_drawn ? *g_host_drawn : 0u;
}

/* The frame's end the display copy made, for a host that sees that copy itself (soa_host_gx_frame_end). */
int gx_off_frame_end(CpuState* s)
{
    if (!gx_is_off() || !s) return 0;
    g_off_frames++;
    gx_frame_end(s);
    return 1;
}

void gx_off_report(void)
{
    if (g_gx_off > 0)
        fprintf(stderr, "[gx] off: %llu register reads, %llu register writes, %llu pipe bytes; %llu frames ended by the host, "
                        "%llu bytes the host drew\n",
                (unsigned long long)g_off_reads, (unsigned long long)g_off_writes, (unsigned long long)g_off_pipe,
                (unsigned long long)g_off_frames, (unsigned long long)gx_off_drawn());
}
