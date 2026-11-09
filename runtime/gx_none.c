/*
 * A build without GX (soa-ue5's U4.2; recompile.py --no-gx, which defines
 * SOA_NO_GX and leaves gx.c, gxr.c, gxr_tev.c and gxv.c out of the link).
 *
 * The host answers GX natively -- its no-FIFO mode: the display lists' bytes
 * and lengths in RAM, CP status and the PI FIFO pointers, the draw-sync token
 * and draw-done with their interrupts, the frame's end at the display copy,
 * each register access's clock charge -- and draws from its own scene list, so
 * nothing of the command processor's model or the renderer is needed. What
 * the rest of the runtime still asks of them is answered here exactly as the
 * full build answers it with SOA_GX=off, in which nothing is parsed or drawn:
 * a store to the gather pipe is counted and dropped, an access to a CP, PE or
 * PI FIFO register counted and answered with zero (a read) or dropped (a
 * write), the pixel engine's interrupts and the bytes drawn are the host's
 * (gx_frame.c's switch, always on here), the register shadows read as zero,
 * no list is recording, no picture is presented, and every renderer hook does
 * nothing. The frame, its end and SOA_FRAMES's stop, the display-list hooks'
 * registration and the pipe's watchers are gx_frame.c's, which every build
 * has. A save state's "gx" section has gx.c's layout, with its model empty;
 * there are no "gxr" and "tex" sections (snapshot.c), and a snapshot taken
 * with GX drawing is refused (its model is not empty).
 *
 * Without SOA_NO_GX this file is empty: the full build has gx.c.
 */
#ifdef SOA_NO_GX
#define _CRT_SECURE_NO_WARNINGS
#include "cpu.h"
#include "gx_state.h"
#include "gxr.h"
#include "gxr_export.h"
#include "snapshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GP_PIPE 0xCC008000u
#define CP_BASE 0xCC000000u
#define PE_BASE 0xCC001000u
#define PI_FIFO_BASE 0xCC00300Cu
#define PI_FIFO_WPTR 0xCC003014u

/* gx_frame.c's */
extern int g_gx_off;
extern int g_gx_pipe_watch;
int gx_is_off(void);
void gx_off_access(CpuState* s, const char* dir, uint32_t ea, unsigned size, uint64_t v);
void gx_off_pipe(CpuState* s, unsigned size, uint64_t v);
unsigned gx_off_pe_pending(void);
uint64_t gx_off_drawn(void);
void gx_off_report(void);
void gx_pipe_watch(CpuState* s, unsigned size, uint64_t v);
void gx_writers_report(void);
void gx_frame_snap(Snap* sn);

/* gx.c's model as SOA_GX=off leaves it: never written, so zero (a save state's "gx" section has them). */
static uint32_t g_cp[0x100], g_xf[0x1100], g_bp[0x100];
static uint16_t g_cp_mmio[0x40], g_pe_mmio[8];
static uint32_t g_pi_fifo[3];
static uint32_t g_cap_cp[0x100], g_cap_xf[0x1100], g_cap_bp[0x100];
static uint8_t g_pipe[1];
/* The pipe's bytes: every one counted and dropped (the report's, and the quiet rule's with the host's). */
static uint64_t g_bytes;

/* ---- the gather pipe and the registers ------------------------------------ */

void gx_pipe_write(CpuState* s, unsigned size, uint64_t v)
{
    if (g_gx_pipe_watch) gx_pipe_watch(s, size, v);
    gx_is_off();
    gx_off_pipe(s, size, v);
    g_bytes += size;
}

uint64_t gx_pipe_bytes(void) { return g_bytes; }
/* What the quiet rule's wait finder takes for drawing (irq.c): the pipe's bytes and the host's. */
uint64_t gx_drawn_bytes(void) { return g_gx_off > 0 ? g_bytes + gx_off_drawn() : g_bytes; }

static int gx_register(uint32_t ea)
{
    return (ea >= CP_BASE && ea < CP_BASE + 0x80) || (ea >= PE_BASE && ea < PE_BASE + 0x10) ||
           (ea >= PI_FIFO_BASE && ea <= PI_FIFO_WPTR + 3);
}

int gx_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out)
{
    if (gx_register(ea) && gx_is_off()) {
        *out = 0;
        gx_off_access(s, "read", ea, size, 0);
        return 1;
    }
    return 0;
}

int gx_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v)
{
    if (gx_register(ea) && gx_is_off()) {
        gx_off_access(s, "write", ea, size, v);
        return 1;
    }
    if (ea >= GP_PIPE && ea < GP_PIPE + 0x100) {
        gx_pipe_write(s, size, v);
        return 1;
    }
    return 0;
}

/* Bit 0: a token interrupt is due; bit 1: a finish interrupt is due -- the host's pixel engine's. */
unsigned gx_pe_irq_pending(void)
{
    return g_gx_off > 0 ? gx_off_pe_pending() : 0u;
}

void gx_report(void)
{
    gx_off_report();
    fprintf(stderr,
            "[gx] %llu pipe bytes, %llu commands: %llu BP, %llu XF, %llu CP loads, %llu display "
            "lists, %llu draws (%llu vertices); %llu draw-dones, %llu tokens, %llu EFB copies, "
            "%llu unknown bytes\n",
            (unsigned long long)g_bytes, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull);
    fprintf(stderr, "[gx] display lists: %llu calls, %llu nonempty, %llu bytes; %llu recorded, %llu bytes recorded\n", 0ull,
            0ull, 0ull, 0ull, 0ull);
    fprintf(stderr, "[gxr] none: this runtime is built without GX (--no-gx)\n");
    gx_writers_report(); /* SOA_GX_WRITERS */
}

/* ---- the model's state, as read by the recorder, ninja.c and the exporter ----- */

const uint32_t* gx_cp_regs(void) { return g_cp; }
const uint32_t* gx_xf_regs(void) { return g_xf; }
const uint32_t* gx_bp_regs(void) { return g_bp; }
unsigned long long gx_draw_count(void) { return 0; }

int gx_list_recording(uint32_t* at)
{
    *at = 0x80000000u | (g_pi_fifo[2] & 0x03FFFFFFu);
    return 0;
}

int gx_draw_list(uint32_t* addr)
{
    *addr = 0;
    return 0;
}

void gx_image(GxImage* out)
{
    unsigned i;
    memcpy(out->cp, g_cp, sizeof out->cp);
    memcpy(out->xf, g_xf, sizeof out->xf);
    memcpy(out->bp, g_bp, sizeof out->bp);
    memcpy(out->pi, g_pi_fifo, sizeof out->pi);
    out->dl_rec = 0;
    out->pipe_len = 0;
    memset(out->reserved, 0, sizeof out->reserved);
    for (i = 0; i < 0x40; ++i) out->cp_mmio[i] = g_cp_mmio[i];
    for (i = 0; i < 8; ++i) out->pe_mmio[i] = g_pe_mmio[i];
}

const uint8_t* gx_pipe_left(uint32_t* len)
{
    *len = 0;
    return g_pipe;
}

int gx_replay(CpuState* s, const char* base)
{
    (void)s;
    fprintf(stderr, "[gx] cannot replay %s: this runtime is built without GX (--no-gx)\n", base);
    return 1;
}

int gx_replay_pair(CpuState* s, const char* a, const char* b)
{
    (void)s, (void)b;
    fprintf(stderr, "[pair] cannot replay %s: this runtime is built without GX (--no-gx)\n", a);
    return 1;
}

/* ---- a snapshot: gx.c's "gx" section, its model empty ---------------------- */

static int all_zero(const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    while (n--)
        if (*b++) return 0;
    return 1;
}

void gx_snap(Snap* sn)
{
    /* gx.c's counts, in its order: bytes (the pipe's), commands, draws, vertices, list calls, BP, XF and CP loads,
     * finishes, tokens, EFB and XFB copies, unknown bytes; then the lists recorded, their bytes, the nonempty calls,
     * their bytes and the overflows. All but the first are the model's, zero here. */
    uint64_t counts[13] = {0}, lists[5] = {0}, pipe_len = 0, cap_len = 0;
    uint32_t last_unknown = 0;
    int dl_rec = 0;
    unsigned i;
    counts[0] = g_bytes;
    SNAP(sn, g_cp);
    SNAP(sn, g_xf);
    SNAP(sn, g_bp);
    SNAP(sn, g_cp_mmio);
    SNAP(sn, g_pe_mmio);
    SNAP(sn, g_pi_fifo);
    SNAP(sn, dl_rec);
    gx_frame_snap(sn);
    SNAP(sn, pipe_len);
    if (pipe_len > GX_PIPE_CAP) {
        snap_fail(sn, "the gather pipe holds %llu bytes, more than its %u", (unsigned long long)pipe_len, (unsigned)GX_PIPE_CAP);
        return;
    }
    if (pipe_len) {
        snap_fail(sn, "the snapshot's gather pipe holds %llu bytes of a command: taken with GX on, and this runtime has none "
                      "(--no-gx)", (unsigned long long)pipe_len);
        return;
    }
    for (i = 0; i < 13; i++) SNAP(sn, counts[i]);
    SNAP(sn, last_unknown);
    for (i = 0; i < 5; i++) SNAP(sn, lists[i]);
    SNAP(sn, g_cap_cp);
    SNAP(sn, g_cap_xf);
    SNAP(sn, g_cap_bp);
    SNAP(sn, cap_len);
    if (!snap_loading(sn) || snap_failed(sn)) return;
    if (dl_rec || cap_len || last_unknown || !all_zero(counts + 1, sizeof counts - sizeof counts[0]) || !all_zero(lists, sizeof lists) ||
        !all_zero(g_cp, sizeof g_cp) || !all_zero(g_xf, sizeof g_xf) || !all_zero(g_bp, sizeof g_bp) ||
        !all_zero(g_cp_mmio, sizeof g_cp_mmio) || !all_zero(g_pe_mmio, sizeof g_pe_mmio) || !all_zero(g_pi_fifo, sizeof g_pi_fifo) ||
        !all_zero(g_cap_cp, sizeof g_cap_cp) || !all_zero(g_cap_xf, sizeof g_cap_xf) || !all_zero(g_cap_bp, sizeof g_cap_bp)) {
        memset(g_cp, 0, sizeof g_cp), memset(g_xf, 0, sizeof g_xf), memset(g_bp, 0, sizeof g_bp);
        memset(g_cp_mmio, 0, sizeof g_cp_mmio), memset(g_pe_mmio, 0, sizeof g_pe_mmio), memset(g_pi_fifo, 0, sizeof g_pi_fifo);
        memset(g_cap_cp, 0, sizeof g_cap_cp), memset(g_cap_xf, 0, sizeof g_cap_xf), memset(g_cap_bp, 0, sizeof g_cap_bp);
        snap_fail(sn, "the snapshot's GX model is not empty: taken with GX on, and this runtime has none (--no-gx)");
        return;
    }
    g_bytes = counts[0];
}

/* ---- the renderer: nothing drawn, nothing presented ------------------------ */

static uint8_t g_screen[EFB_H][EFB_W][4]; /* never written: gxr.c's, with nothing copied out */

int gxr_enabled(void) { return 0; }
void gxr_enable(int on) { (void)on; }
void gxr_flush(void) {}
void gxr_reset_efb(void) {}
void gxr_set_output(const char* png_path) { (void)png_path; }
void gxr_draw_every_frame(void) {}
void gxr_skip_until(unsigned frame) { (void)frame; }
void gxr_hook_hazard(uint32_t addr, uint32_t bytes) { (void)addr, (void)bytes; }
void gxr_memory_hazard(void) {}
long gxr_presented(void) { return 0; }

const uint8_t* gxr_screen(int* w, int* h)
{
    *w = EFB_W;
    *h = 480;
    return &g_screen[0][0][0];
}

void gxr_set_draw_export(const GxrDrawExport* e) { (void)e; } /* no draw to export */
void gxr_set_projection_filter(void (*fn)(float p[6], int orthographic)) { (void)fn; }
void gxr_set_texture_provider(int (*fn)(uint64_t hash, uint32_t fmt, uint32_t w, uint32_t h, const uint8_t* rgba,
                                        const uint8_t** out, uint32_t* out_w, uint32_t* out_h))
{
    (void)fn;
}

/* The combiner, for host.c's exporter of the renderer's draws: never called, since nothing is drawn or exported. */
void tev_pixel(const TevSetup* T, const int ras[2][4], const float tex[8][4], uint8_t out[4], int* alpha_pass)
{
    (void)T, (void)ras, (void)tex;
    memset(out, 0, 4);
    if (alpha_pass) *alpha_pass = 0;
}

/* The GPU backend: none. */
void gxv_set_root(const char* root) { (void)root; }
int gxv_start(char* why, size_t cap)
{
    snprintf(why, cap, "this runtime is built without GX (--no-gx)");
    return 0;
}
#endif
