/*
 * The graphics processor's front end: write-gather pipe, command stream,
 * register shadows, and the interrupts the pixel engine raises.
 *
 * Vertex submission is inlined into game code (SPEC section 7), so the only
 * faithful place to see it is the byte stream the CPU pushes through the
 * write-gather pipe at 0xCC008000. This module reassembles that stream and
 * walks it as the command processor would: CP/XF/BP register loads are
 * shadowed, display lists are followed, and draw commands are stepped over
 * using the vertex size the VCD/VAT registers imply. Nothing is rendered yet;
 * what matters first is that GXDrawDone and GXSetDrawSync produce the
 * PE_FINISH and PE_TOKEN interrupts the game sleeps on.
 *
 * The CPU FIFO in main memory is modelled only for display lists: between
 * GXBeginDisplayList and GXEndDisplayList the bytes go into guest memory at
 * the PI write pointer, and the list is parsed when the game calls it (C5b,
 * below). Everything else is consumed as it is written, so the FIFO always
 * reads as empty and the GP as idle, which is exactly what the SDK's flow
 * control and the game's GP-hang detector want.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GP_PIPE 0xCC008000u
#define CP_BASE 0xCC000000u
#define PE_BASE 0xCC001000u
#define PI_FIFO_BASE 0xCC00300Cu
#define PI_FIFO_TOP 0xCC003010u
#define PI_FIFO_WPTR 0xCC003014u

/* CP register shadow (indices are the CP register numbers). */
static uint32_t g_cp[0x100];
static uint32_t g_xf[0x1100]; /* 0x000-0xFFF matrix memory, 0x1000-0x10FF registers */
static uint32_t g_bp[0x100];
/* Set while the parser is inside a display list: the list's address as the
 * call gave it. Part of a draw's identity across frames (H10, and
 * tools/fifopair.py's "dl" key field). */
static uint32_t g_list_addr;
static int g_in_list;
static uint16_t g_cp_mmio[0x40]; /* the CP's own MMIO registers, by half-word index */
static uint16_t g_pe_mmio[8];
static uint32_t g_pi_fifo[3];

/* The write-gather pipe's bytes not yet parsed (a command not yet whole). */
#define PIPE_CAP 65536
static uint8_t g_pipe[PIPE_CAP];
static size_t g_pipe_len;

/* Display-list recording (gpu-backend.md C5b). GXBeginDisplayList
 * (fn_80251D80) points the CPU FIFO at DisplayListFifo, the SDK's one FIFO
 * object for lists, through GXSetCPUFifo (fn_8024C5FC), which stores the
 * object's address in the small-data global at r13-27520 (0x8024C620) before
 * it writes the PI FIFO registers; GXEndDisplayList (fn_80251E48) points it
 * back the same way. So the port records exactly while that global holds
 * DisplayListFifo: each gather-pipe byte goes into guest memory at the PI write
 * pointer, which GXEndDisplayList reads (through fn_8024C8A4, after GXFlush's
 * 32 NOPs) to size the list, and the list is parsed when the game calls it.
 * Any other CPU FIFO -- the one the logo screen draws through from frame 1 to
 * 385 and never calls, for one -- is parsed at once, as before (FINDINGS
 * "Recorded display lists (C5a)"). Before this, every list was drawn when it
 * was recorded and called empty. */
#define GX_CPU_FIFO_SDA (-27520) /* r13-relative: GXSetCPUFifo's current CPU FIFO object */
#define GX_DL_FIFO 0x80318B18u   /* DisplayListFifo (GXBeginDisplayList, 0x80251D94) */
#define PI_WRAP 0x04000000u      /* PI_FIFO_WPTR's wrap bit; GXEndDisplayList reads it as overflow */
/* A capture-only record: a display-list call with the list's bytes inline,
 * as they were at the call (opcode, address, size, then size bytes). A
 * capture's RAM is the frame's end, and a list recorded again after its call
 * would replay wrong from it. No GX command uses 0x41, and only a replay
 * reads it. */
#define CAP_LIST 0x41
static int g_dl_rec;    /* the CPU FIFO is DisplayListFifo: record, do not parse */
static int g_replaying; /* parse() is reading a capture: CAP_LIST is honoured */
static uint64_t g_dl_recorded, g_dl_rec_bytes, g_dl_nonempty, g_dl_call_bytes, g_dl_overflows;

/* PE_ISR at 0xCC00100A: bit0 token enable, bit1 finish enable, bit2 token, bit3 finish. */
#define PE_ISR 5
#define PE_TOKEN 7
#define ISR_TOKEN_EN 0x1u
#define ISR_FINISH_EN 0x2u
#define ISR_TOKEN 0x4u
#define ISR_FINISH 0x8u

static uint64_t g_bytes, g_cmds, g_draws, g_verts, g_dl_calls, g_bp_loads, g_xf_loads, g_cp_loads;
static uint64_t g_finishes, g_tokens, g_efb_copies, g_xfb_copies, g_unknown;
static uint32_t g_last_unknown;

/* SOA_GX_DLLOG=1 (PLAN C5a, from the GPU spec's research): whether the game
 * records display lists by pointing the CPU FIFO at a buffer of its own --
 * GXBeginDisplayList through GXSetCPUFifo, which writes the PI FIFO base, top
 * and write pointer -- while the port, which parses every gather-pipe store as
 * it comes, runs those commands at once and leaves the list empty. It logs
 * each move of the PI FIFO base away from the command processor's FIFO base
 * and back, with the draws and bytes parsed while it was away, and each
 * display-list call with its size; the report sums them. Diagnostic only:
 * nothing here changes what is parsed. */
static int g_dllog = -1;
static int g_dl_away;                          /* the PI FIFO base is not the CP's */
static uint64_t g_dl_moves, g_dl_away_draws, g_dl_away_bytes, g_dl_zero, g_dl_sized;
static uint64_t g_dl_draws0, g_dl_bytes0;       /* at the move away */
static unsigned g_dl_lines;
static uint32_t g_dl_cp_seen;                   /* the CP's FIFO base, last logged */
#define DLLOG_LINES 400

/* Per buffer the CPU FIFO was pointed at, or list called: how often each,
 * and the draws parsed while it was the CPU FIFO. */
typedef struct {
    uint32_t addr;
    uint64_t moves, calls, draws, bytes;
} DlBuf;
#define DL_BUFS 32
static DlBuf g_dl_buf[DL_BUFS];
static unsigned g_dl_nbuf, g_dl_cur = DL_BUFS;
static uint64_t g_dl_spill;

static unsigned dl_buf(uint32_t addr)
{
    unsigned i;
    addr &= 0x1FFFFFFFu;
    for (i = 0; i < g_dl_nbuf; i++)
        if (g_dl_buf[i].addr == addr) return i;
    if (g_dl_nbuf == DL_BUFS) { g_dl_spill++; return DL_BUFS; }
    g_dl_buf[g_dl_nbuf].addr = addr;
    return g_dl_nbuf++;
}

static uint32_t cp_fifo_base(void)
{
    return ((uint32_t)g_cp_mmio[0x22 >> 1] << 16 | g_cp_mmio[0x20 >> 1]) & 0x1FFFFFFFu;
}

unsigned gx_frame_count(void);

static int dllog_on(void)
{
    if (g_dllog < 0) g_dllog = getenv("SOA_GX_DLLOG") && atoi(getenv("SOA_GX_DLLOG")) ? 1 : 0;
    return g_dllog > 0;
}

static void dllog_line(const char* fmt, uint32_t a, uint32_t b, unsigned long long c, unsigned long long d)
{
    if (g_dl_lines++ < DLLOG_LINES) {
        fprintf(stderr, "[dl] frame %u: ", gx_frame_count());
        fprintf(stderr, fmt, a, b, c, d);
        fputc('\n', stderr);
    }
}

/* After every PI FIFO register write: has the base moved away, or back? */
static void dllog_pi(void)
{
    uint32_t pi = g_pi_fifo[0], cp = cp_fifo_base();
    int away = pi && cp && pi != cp;
    if (away && !g_dl_away) {
        g_dl_away = 1;
        g_dl_moves++;
        g_dl_draws0 = g_draws;
        g_dl_bytes0 = g_bytes;
        g_dl_cur = dl_buf(pi);
        if (g_dl_cur < DL_BUFS) g_dl_buf[g_dl_cur].moves++;
        dllog_line("CPU FIFO moved to %08X (the CP's is at %08X)%.0llu%.0llu", pi, cp, 0ull, 0ull);
    } else if (!away && g_dl_away) {
        g_dl_away = 0;
        g_dl_away_draws += g_draws - g_dl_draws0;
        g_dl_away_bytes += g_bytes - g_dl_bytes0;
        if (g_dl_cur < DL_BUFS) {
            g_dl_buf[g_dl_cur].draws += g_draws - g_dl_draws0;
            g_dl_buf[g_dl_cur].bytes += g_bytes - g_dl_bytes0;
        }
        dllog_line("CPU FIFO back at %08X; write pointer %08X; %llu draw(s) and %llu byte(s) parsed while it was away", pi,
                   g_pi_fifo[2], (unsigned long long)(g_draws - g_dl_draws0), (unsigned long long)(g_bytes - g_dl_bytes0));
    }
}

void gx_dllog_report(void)
{
    if (g_dllog <= 0) return;
    fprintf(stderr, "[dl] %llu move(s) of the CPU FIFO away from the CP's, %llu draw(s) and %llu byte(s) parsed while away%s; "
                    "%llu display-list call(s), %llu of size 0 and %llu with a size\n",
            (unsigned long long)g_dl_moves, (unsigned long long)g_dl_away_draws, (unsigned long long)g_dl_away_bytes,
            g_dl_away ? " (and away at the end)" : "", (unsigned long long)(g_dl_zero + g_dl_sized),
            (unsigned long long)g_dl_zero, (unsigned long long)g_dl_sized);
    {
        unsigned i;
        for (i = 0; i < g_dl_nbuf; i++)
            fprintf(stderr, "[dl]   %08X: the CPU FIFO %llu time(s), %llu draw(s) and %llu byte(s) parsed there; called %llu time(s)\n",
                    g_dl_buf[i].addr, (unsigned long long)g_dl_buf[i].moves, (unsigned long long)g_dl_buf[i].draws,
                    (unsigned long long)g_dl_buf[i].bytes, (unsigned long long)g_dl_buf[i].calls);
        if (g_dl_spill) fprintf(stderr, "[dl]   and %llu more past the table's %d\n", (unsigned long long)g_dl_spill, DL_BUFS);
    }
}

/* After a CP MMIO write: has the FIFO the command processor reads moved? */
static void dllog_cp(void)
{
    uint32_t cp = cp_fifo_base();
    if (cp != g_dl_cp_seen) {
        g_dl_cp_seen = cp;
        dllog_line("the CP's FIFO base is now %08X (the CPU FIFO's is %08X)%.0llu%.0llu", cp, g_pi_fifo[0], 0ull, 0ull);
    }
}

/* ---- frame capture ---------------------------------------------------
 * SOA_FIFO_DUMP=a,b,c names frame numbers (frames end at a copy to the
 * XFB). For each, NNNN.regs holds the CP/XF/BP shadows as the frame began,
 * NNNN.fifo the bytes the CPU pushed during it, NNNN.ram all of MEM1 as it
 * ended: everything a replay needs to render it offline. They go to
 * SOA_FIFO_DIR, which defaults to build/fifo. */
static uint8_t* g_cap;
static size_t g_cap_len, g_cap_cap;
static uint32_t g_cap_cp[0x100], g_cap_xf[0x1100], g_cap_bp[0x100];
static unsigned g_frame;
static const char* g_dump_list = NULL;
static int g_dump_checked;

/* SOA_FRAMES=n stops the run here rather than in the renderer, so it counts
 * the frames the game presents whether or not anything is being drawn, and
 * counts the same ones SOA_PAD scripts against. main sets it once the guest
 * is about to run, which keeps --replay and the selftest out of it. */
static unsigned g_frame_limit;
void hle_report(void);
void gxr_flush(void);

/* Something to do at each frame boundary, if anyone has asked. main.c hangs
 * SOA_POKE off this rather than gx.c calling into main.c, because the renderer
 * links on its own -- render_check.py and the four gxr tests build gx.c, gxr.c,
 * gxr_tev.c and png.c with two stubs and no main.c, and PLAN A1 counts those
 * two stubs as a property worth keeping. A direct call compiled fine and broke
 * 29 tests at the link step, which is the failure the compile-only CI job
 * cannot see. */
static void (*g_frame_hook)(CpuState*, unsigned);

void gx_set_frame_hook(void (*fn)(CpuState*, unsigned));

void gx_set_frame_hook(void (*fn)(CpuState*, unsigned))
{
    g_frame_hook = fn;
}

void gx_set_frame_limit(unsigned frames)
{
    g_frame_limit = frames;
}

static int frame_wanted(unsigned frame)
{
    const char* p;
    if (!g_dump_checked) { g_dump_checked = 1; g_dump_list = getenv("SOA_FIFO_DUMP"); }
    for (p = g_dump_list; p && *p;) {
        char* end;
        unsigned long n = strtoul(p, &end, 10);
        if (end == p) break;
        if (n == frame) return 1;
        p = *end == ',' ? end + 1 : end;
    }
    return 0;
}

static void cap_append(const uint8_t* p, size_t n)
{
    if (!g_dump_list) return;
    if (g_cap_len + n > g_cap_cap) {
        size_t want = g_cap_cap ? g_cap_cap * 2 : (1u << 20);
        while (want < g_cap_len + n) want *= 2;
        g_cap = (uint8_t*)realloc(g_cap, want);
        g_cap_cap = want;
    }
    memcpy(g_cap + g_cap_len, p, n);
    g_cap_len += n;
}

/* The 9-byte list call `back` bytes before the capture's end becomes a
 * CAP_LIST record holding the list as it is now. The bytes after the call
 * (`back` - 9 of them, the rest of the store it arrived in) follow it again. */
static void cap_inline_list(size_t back, const uint8_t* body, uint32_t size)
{
    uint8_t head[9], tail[64];
    size_t pos, n;
    if (!g_dump_list || back < 9 || back > g_cap_len || back - 9 > sizeof tail) return;
    pos = g_cap_len - back;
    if (g_cap[pos] != 0x40) return;
    n = back - 9;
    memcpy(tail, g_cap + pos + 9, n);
    memcpy(head, g_cap + pos, 9);
    head[0] = CAP_LIST;
    g_cap_len = pos;
    cap_append(head, 9);
    cap_append(body, size);
    cap_append(tail, n);
}

static void write_file(const char* path, const void* data, size_t len)
{
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[gx] cannot write %s\n", path); return; }
    fwrite(data, 1, len, f);
    fclose(f);
}

/* Where captures land. build/fifo is the corpus config/fifo_manifest.tsv
 * pins frame hashes against, so a capturing run must be able to write
 * somewhere else: overwriting a capture silently invalidates the hash that
 * was blessed from it, and the streams are not in the repository to restore
 * from. SOA_FIFO_DIR is how a scenario says "not the corpus". */
static const char* dump_dir(void)
{
    static const char* dir;
    if (!dir) {
        dir = getenv("SOA_FIFO_DIR");
        if (!dir || !*dir) dir = "build/fifo";
    }
    return dir;
}

static void frame_end(CpuState* s)
{
    char path[256];
    /* A frame being captured is finished first, so its hook and its .ram see
     * every copy it made, as they did before H14 let a frame end with the
     * workers still drawing it. */
    if (frame_wanted(g_frame)) gxr_flush();
    /* Before the capture below, so a frame that is both poked and dumped is
     * dumped with the poke already in it. */
    if (g_frame_hook) g_frame_hook(s, g_frame);
    if (frame_wanted(g_frame)) {
        const char* dir = dump_dir();
        snprintf(path, sizeof path, "%s/%04u.regs", dir, g_frame);
        {
            FILE* f = fopen(path, "wb");
            if (f) {
                fwrite(g_cap_cp, 4, 0x100, f);
                fwrite(g_cap_xf, 4, 0x1100, f);
                fwrite(g_cap_bp, 4, 0x100, f);
                fclose(f);
            } else fprintf(stderr, "[gx] cannot write %s (mkdir %s)\n", path, dir);
        }
        snprintf(path, sizeof path, "%s/%04u.fifo", dir, g_frame);
        write_file(path, g_cap, g_cap_len);
        snprintf(path, sizeof path, "%s/%04u.ram", dir, g_frame);
        write_file(path, s->mem, MEM1_SIZE);
        fprintf(stderr, "[gx] captured frame %u into %s: %zu command bytes\n", g_frame, dir,
                g_cap_len);
    }
    g_frame++;
    if (g_frame_limit && g_frame >= g_frame_limit) {
        /* The copy for this frame is already queued (load_bp calls the
         * renderer before us), so flushing here finishes it -- any snapshot
         * is written and the counters are real before the report. Leave with
         * _Exit (C99's _exit) for the reason window.c does: the rasterizer's workers are
         * spinning, and CRT teardown around them can hang. */
        fprintf(stderr, "[boot] %u frames done (SOA_FRAMES)\n", g_frame);
        gxr_flush();
        hle_report();
        fflush(NULL);
        _Exit(0);
    }
    g_cap_len = 0;
    memcpy(g_cap_cp, g_cp, sizeof g_cp);
    memcpy(g_cap_xf, g_xf, sizeof g_xf);
    memcpy(g_cap_bp, g_bp, sizeof g_bp);
}

/* ---- vertex size from the current VCD/VAT --------------------------- */

static unsigned comp_size(unsigned fmt)
{
    return fmt == 4 ? 4 : (fmt >= 2 ? 2 : 1); /* u8 s8 u16 s16 f32 */
}

static unsigned color_size(unsigned comp)
{
    static const unsigned t[8] = {2, 3, 4, 2, 3, 4, 4, 4}; /* 565 888 888x 4444 6666 8888 */
    return t[comp & 7];
}

static unsigned attr_size(unsigned vcd, unsigned direct_size)
{
    switch (vcd & 3) {
    case 0: return 0;            /* not present */
    case 1: return direct_size;  /* inline */
    case 2: return 1;            /* 8-bit index */
    default: return 2;           /* 16-bit index */
    }
}

static unsigned vertex_size(unsigned vat)
{
    uint32_t lo = g_cp[0x50], hi = g_cp[0x60];
    uint32_t a = g_cp[0x70 + vat], b = g_cp[0x80 + vat], c = g_cp[0x90 + vat];
    unsigned size = 0, i;
    unsigned tc[8][2] = {
        {(a >> 21) & 1, (a >> 22) & 7}, {(b >> 0) & 1, (b >> 1) & 7},
        {(b >> 9) & 1, (b >> 10) & 7},  {(b >> 18) & 1, (b >> 19) & 7},
        {(b >> 27) & 1, (b >> 28) & 7}, {(c >> 5) & 1, (c >> 6) & 7},
        {(c >> 14) & 1, (c >> 15) & 7}, {(c >> 23) & 1, (c >> 24) & 7},
    };

    size += lo & 1; /* position matrix index */
    for (i = 0; i < 8; i++) size += (lo >> (1 + i)) & 1; /* texture matrix indices */

    size += attr_size((lo >> 9) & 3, ((a & 1) ? 3 : 2) * comp_size((a >> 1) & 7));

    {
        unsigned vcd = (lo >> 11) & 3, elems = (a >> 9) & 1, fmt = (a >> 10) & 7;
        if (vcd >= 2 && elems && ((a >> 31) & 1))
            size += 3 * (vcd == 2 ? 1 : 2); /* NBT with three separate indices */
        else
            size += attr_size(vcd, (elems ? 9 : 3) * comp_size(fmt));
    }

    size += attr_size((lo >> 13) & 3, color_size((a >> 14) & 7));
    size += attr_size((lo >> 15) & 3, color_size((a >> 18) & 7));

    for (i = 0; i < 8; i++)
        size += attr_size((hi >> (2 * i)) & 3, (tc[i][0] ? 2 : 1) * comp_size(tc[i][1]));
    return size;
}

/* ---- command stream --------------------------------------------------- */

void gxr_bp_written(CpuState* s, uint32_t reg, uint32_t value);
void gxr_draw(CpuState* s, unsigned op, unsigned count, const uint8_t* verts, unsigned vsize);
void gxr_report(void);
void gxr_reset_efb(void);
void gxr_flush(void);
void tex_epoch_advance(void);
void gxr_source_hazard(uint32_t addr, uint32_t bytes); /* gxr.c: wait for a queued copy writing there */

static void load_bp(CpuState* s, uint32_t v)
{
    uint32_t reg = v >> 24;
    g_bp[reg] = v & 0xFFFFFFu;
    g_bp_loads++;
    switch (reg) {
    case 0x45: /* PE_DONE: draw done, optionally with the finish interrupt */
        if (v & 2) {
            g_pe_mmio[PE_ISR] |= ISR_FINISH;
            g_finishes++;
        }
        break;
    case 0x47: /* PE_TOKEN: token without interrupt */
        g_pe_mmio[PE_TOKEN] = (uint16_t)v;
        break;
    case 0x48: /* PE_TOKEN_INT */
        g_pe_mmio[PE_TOKEN] = (uint16_t)v;
        g_pe_mmio[PE_ISR] |= ISR_TOKEN;
        g_tokens++;
        break;
    case 0x52: /* EFB copy: to a texture or to the XFB -- a frame, when the latter */
        g_efb_copies++;
        gxr_bp_written(s, reg, v & 0xFFFFFFu);
        if (v & 0x4000u) { g_xfb_copies++; frame_end(s); }
        return;
    default: break;
    }
    gxr_bp_written(s, reg, v & 0xFFFFFFu);
}

static uint32_t be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint16_t be16(const uint8_t* p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* Parse as many whole commands as `len` bytes hold; return how many bytes
 * were consumed. A command cut off by the end of the buffer is left for the
 * next call. */
static size_t parse(CpuState* s, const uint8_t* p, size_t len, int in_display_list)
{
    size_t off = 0;
    while (off < len) {
        uint8_t op = p[off];
        size_t need;
        if (op == 0x00) { off += 1; g_cmds++; continue; }
        if (op == 0x48) { off += 1; g_cmds++; continue; } /* invalidate vertex cache */
        if (op == 0x08) { /* CP register */
            need = 6;
            if (off + need > len) break;
            g_cp[p[off + 1]] = be32(p + off + 2);
            g_cp_loads++;
        } else if (op == 0x10) { /* XF registers */
            uint32_t count, addr;
            if (off + 5 > len) break;
            count = be16(p + off + 1) + 1u;
            addr = be16(p + off + 3);
            need = 5 + 4 * (size_t)count;
            if (off + need > len) break;
            {
                uint32_t i;
                for (i = 0; i < count; i++)
                    if (addr + i < 0x1100) g_xf[addr + i] = be32(p + off + 5 + 4 * i);
            }
            g_xf_loads++;
        } else if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) { /* indexed XF (GXLoadPosMtxIndx etc.) */
            uint32_t index, v, count, addr, array, base, stride, src, i;
            need = 5;
            if (off + need > len) break;
            index = be16(p + off + 1);
            v = be16(p + off + 3);
            count = (v >> 12) + 1;
            addr = v & 0xFFF;
            array = 12 + ((op - 0x20) >> 3);
            base = g_cp[0xA0 + array] & 0x1FFFFFFFu;
            stride = g_cp[0xB0 + array] & 0xFFu;
            src = base + index * stride;
            /* count comes out of the command stream, so 4 * count can
             * overflow and wrap the sum back under the limit. Subtract. */
            if (count <= MEM1_SIZE / 4 && (src & MEM_MASK) <= MEM1_SIZE - 4 * count) {
                gxr_source_hazard(src, 4 * count); /* a queued copy may be writing it (H14) */
                for (i = 0; i < count; i++)
                    if (addr + i < 0x1100) g_xf[addr + i] = mem_r32(s, (src | 0x80000000u) + 4 * i);
            }
            g_xf_loads++;
        } else if (op == 0x40 || (op == CAP_LIST && g_replaying && !in_display_list)) { /* display list */
            uint32_t addr, size;
            const uint8_t* body = NULL;
            need = 9;
            if (off + need > len) break;
            addr = be32(p + off + 1);
            size = be32(p + off + 5);
            if (op == CAP_LIST) { /* a capture's call, the list inline after it */
                if (size > len - off - 9) break;
                need += size;
                body = p + off + 9;
            }
            g_dl_calls++;
            if (g_dllog > 0) {
                unsigned b = dl_buf(addr);
                if (b < DL_BUFS) g_dl_buf[b].calls++;
                if (size) g_dl_sized++;
                else g_dl_zero++;
                dllog_line("display list %08X called, %u byte(s)%.0llu%.0llu", addr, size, 0ull, 0ull);
            }
            /* size is the guest's, and the same wrap applies to it. */
            if (!body && !in_display_list && size <= MEM1_SIZE && (addr & MEM_MASK) <= MEM1_SIZE - size) {
                gxr_source_hazard(addr, size); /* a queued copy may be writing it (H14) */
                body = mem_ptr(s, addr);
                /* A live call (the pipe's own buffer): the capture keeps the
                 * list as it is now, not as the frame's end leaves it. */
                if (p == g_pipe && size) cap_inline_list(len - off, body, size);
            }
            if (body && !in_display_list) {
                size_t done;
                g_list_addr = addr; /* the draws inside say which list they came through (gx_draw_list) */
                g_in_list = 1;
                done = parse(s, body, size, 1);
                g_in_list = 0;
                if (size) { g_dl_nonempty++; g_dl_call_bytes += size; }
                if (done != size) {
                    static int warned;
                    if (!warned++) fprintf(stderr, "[gx] display list at %08X: %zu of %u bytes parsed\n", addr, done, size);
                }
            }
        } else if (op == 0x61) { /* BP register */
            need = 5;
            if (off + need > len) break;
            load_bp(s, be32(p + off + 1));
        } else if (op >= 0x80 && op < 0xC0) { /* draw */
            unsigned vat = op & 7, count, vsize;
            if (off + 3 > len) break;
            count = be16(p + off + 1);
            vsize = vertex_size(vat);
            need = 3 + (size_t)count * vsize;
            if (off + need > len) break;
            g_draws++;
            g_verts += count;
            gxr_draw(s, op, count, p + off + 3, vsize);
        } else {
            if (g_unknown++ == 0 || g_last_unknown != op)
                fprintf(stderr, "[gx] unknown command byte %02X (%s)\n", op, in_display_list ? "display list" : "pipe");
            g_last_unknown = op;
            need = 1; /* resync one byte at a time */
        }
        off += need;
        g_cmds++;
    }
    return off;
}

/* ---- write-gather pipe ------------------------------------------------ */

/* Set while this thread is inside the parse, and read by the sampler in
 * main.c. A clock pair here is not affordable and that is measured, not
 * argued: pipe_flush runs once per guest store to the gather pipe -- one
 * saved run pushed 3,018,001,651 bytes through it -- and a benchmark shaped
 * like the loop below puts the region at 4.8 ns, against 9.3 ns for the
 * cheapest clock pair and 28.6 ns for QueryPerformanceCounter. A timer here
 * would report mostly itself and would add seconds to the run it was timing.
 * Two plain stores instead, which measure at nothing, and the sampler turns
 * the share of its samples that land here into seconds. What that costs is
 * resolution: the parse is one number for the whole run and never a duration
 * for one flush. */
int g_gx_parsing;

static void pipe_flush(CpuState* s)
{
    int prev = g_gx_parsing;
    size_t done;
    g_gx_parsing = 1;
    done = parse(s, g_pipe, g_pipe_len, 0);
    g_gx_parsing = prev;
    if (done) {
        memmove(g_pipe, g_pipe + done, g_pipe_len - done);
        g_pipe_len -= done;
    }
}

/* Inside GXBeginDisplayList/GXEndDisplayList: the bytes go into guest memory
 * at the PI write pointer, as the console's gather pipe puts them, and are
 * neither parsed nor captured here (a capture gets the list at its call). Past
 * the list's buffer the pointer wraps to its base and sets the wrap bit, which
 * GXEndDisplayList reads as overflow and answers with a list of size 0. */
static void dl_record(CpuState* s, unsigned size, uint64_t v)
{
    uint32_t base = g_pi_fifo[0] & 0x03FFFFFFu, end = (g_pi_fifo[1] & 0x03FFFFFFu) + 4u; /* top is the last word */
    unsigned i;
    for (i = 0; i < size; i++) {
        uint32_t w = g_pi_fifo[2] & 0x03FFFFFFu;
        if (w >= end) {
            if (!(g_pi_fifo[2] & PI_WRAP) && g_dl_overflows++ == 0)
                fprintf(stderr, "[gx] a display list overflowed its buffer at %08X\n", base);
            g_pi_fifo[2] |= PI_WRAP;
            w = base;
        }
        if (w < MEM1_SIZE) *mem_ptr(s, 0x80000000u | w) = (uint8_t)(v >> (8 * (size - 1 - i)));
        g_pi_fifo[2] = (g_pi_fifo[2] & PI_WRAP) | (w + 1u);
    }
    g_dl_rec_bytes += size;
    g_bytes += size;
}

/* After a write of the PI FIFO base: is the CPU FIFO now DisplayListFifo? */
static void dl_bracket(CpuState* s)
{
    int rec = s->gpr[13] && mem_r32(s, s->gpr[13] + (uint32_t)GX_CPU_FIFO_SDA) == GX_DL_FIFO;
    if (rec && !g_dl_rec) {
        g_dl_recorded++;
        if (g_pipe_len) {
            static int warned;
            if (!warned++) fprintf(stderr, "[gx] a display list began with %zu byte(s) of a command unparsed\n", g_pipe_len);
        }
    }
    g_dl_rec = rec;
}

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

static void writers_report(void)
{
    unsigned i, n = 0;
    uint64_t total = 0;
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

void gx_pipe_write(CpuState* s, unsigned size, uint64_t v)
{
    unsigned i;
    if (g_writers_on < 0) {
        const char* e = getenv("SOA_GX_WRITERS");
        g_writers_on = e && atoi(e);
    }
    if (g_writers_on) writer_note(s, size);
    if (g_dl_rec) {
        dl_record(s, size, v);
        return;
    }
    if (g_pipe_len + size > PIPE_CAP) {
        static int warned;
        if (!warned++) fprintf(stderr, "[gx] pipe buffer full at %zu bytes; a command is not parsing\n", g_pipe_len);
        g_pipe_len = 0;
    }
    for (i = 0; i < size; i++) g_pipe[g_pipe_len++] = (uint8_t)(v >> (8 * (size - 1 - i)));
    cap_append(g_pipe + g_pipe_len - size, size);
    g_bytes += size;
    pipe_flush(s);
}

uint64_t gx_pipe_bytes(void) { return g_bytes; }

/* ---- MMIO ------------------------------------------------------------- */

int gx_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out)
{
    (void)s;
    if (ea >= CP_BASE && ea < CP_BASE + 0x80 && size == 2) {
        unsigned idx = (ea - CP_BASE) >> 1;
        switch (ea - CP_BASE) {
        case 0x00: *out = 0x000Cu; return 1; /* status: read idle, command idle */
        case 0x30: case 0x32: *out = 0; return 1; /* read/write distance: FIFO empty */
        case 0x38: *out = g_cp_mmio[0x34 >> 1]; return 1; /* read ptr == write ptr */
        case 0x3A: *out = g_cp_mmio[0x36 >> 1]; return 1;
        default: *out = g_cp_mmio[idx]; return 1;
        }
    }
    if (ea >= PE_BASE && ea < PE_BASE + 0x10 && size == 2) {
        *out = g_pe_mmio[(ea - PE_BASE) >> 1];
        return 1;
    }
    if (size == 4 && ea >= PI_FIFO_BASE && ea <= PI_FIFO_WPTR) {
        *out = g_pi_fifo[(ea - PI_FIFO_BASE) >> 2];
        return 1;
    }
    return 0;
}

int gx_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v)
{
    if (ea >= GP_PIPE && ea < GP_PIPE + 0x100) {
        gx_pipe_write(s, size, v);
        return 1;
    }
    if (ea >= CP_BASE && ea < CP_BASE + 0x80 && size == 2) {
        g_cp_mmio[(ea - CP_BASE) >> 1] = (uint16_t)v;
        if ((ea - CP_BASE == 0x20 || ea - CP_BASE == 0x22) && dllog_on()) dllog_cp();
        return 1;
    }
    if (ea >= PE_BASE && ea < PE_BASE + 0x10 && size == 2) {
        unsigned idx = (ea - PE_BASE) >> 1;
        if (idx == PE_ISR) /* enables are stored; status bits are write-one-to-clear */
            g_pe_mmio[PE_ISR] = (uint16_t)((g_pe_mmio[PE_ISR] & (ISR_TOKEN | ISR_FINISH) & ~(v & (ISR_TOKEN | ISR_FINISH))) | (v & (ISR_TOKEN_EN | ISR_FINISH_EN)));
        else
            g_pe_mmio[idx] = (uint16_t)v;
        return 1;
    }
    if (size == 4 && ea >= PI_FIFO_BASE && ea <= PI_FIFO_WPTR) {
        g_pi_fifo[(ea - PI_FIFO_BASE) >> 2] = (uint32_t)v & 0x1FFFFFFFu;
        if (ea == PI_FIFO_BASE) dl_bracket(s);
        if (dllog_on()) dllog_pi();
        return 1;
    }
    return 0;
}

/* Bit 0: a token interrupt is due; bit 1: a finish interrupt is due. */
unsigned gx_pe_irq_pending(void)
{
    unsigned isr = g_pe_mmio[PE_ISR], due = 0;
    if ((isr & ISR_TOKEN) && (isr & ISR_TOKEN_EN)) due |= 1;
    if ((isr & ISR_FINISH) && (isr & ISR_FINISH_EN)) due |= 2;
    return due;
}

void gx_report(void)
{
    fprintf(stderr,
            "[gx] %llu pipe bytes, %llu commands: %llu BP, %llu XF, %llu CP loads, %llu display "
            "lists, %llu draws (%llu vertices); %llu draw-dones, %llu tokens, %llu EFB copies, "
            "%llu unknown bytes\n",
            (unsigned long long)g_bytes, (unsigned long long)g_cmds, (unsigned long long)g_bp_loads,
            (unsigned long long)g_xf_loads, (unsigned long long)g_cp_loads,
            (unsigned long long)g_dl_calls, (unsigned long long)g_draws,
            (unsigned long long)g_verts, (unsigned long long)g_finishes,
            (unsigned long long)g_tokens, (unsigned long long)g_efb_copies,
            (unsigned long long)g_unknown);
    fprintf(stderr, "[gx] display lists: %llu calls, %llu nonempty, %llu bytes; %llu recorded, %llu bytes recorded%s\n",
            (unsigned long long)g_dl_calls, (unsigned long long)g_dl_nonempty, (unsigned long long)g_dl_call_bytes,
            (unsigned long long)g_dl_recorded, (unsigned long long)g_dl_rec_bytes,
            g_dl_overflows ? ", some overflowed their buffers" : "");
    gx_dllog_report();
    gxr_report();
    if (g_writers_on > 0) writers_report(); /* SOA_GX_WRITERS */
}

/* ---- replay ------------------------------------------------------------
 * Feed a captured frame through the same parser with MEM1 restored, so the
 * renderer sees exactly what it saw in the game. */
unsigned gx_frame_count(void) { return g_frame; }
const uint32_t* gx_cp_regs(void) { return g_cp; }
const uint32_t* gx_xf_regs(void) { return g_xf; }
const uint32_t* gx_bp_regs(void) { return g_bp; }

/* 1, with the list's address, when the draw being parsed came through a
 * display list; 0 for a draw in the stream itself. */
int gx_draw_list(uint32_t* addr)
{
    *addr = g_in_list ? g_list_addr : 0;
    return g_in_list;
}

/* A capture's register images, its end-of-frame RAM (unless with_ram is 0)
 * and its command stream, loaded as gx_replay has always loaded them. */
static int load_capture(CpuState* s, const char* base, int with_ram, uint8_t** fifo_out, size_t* len_out)
{
    char path[512];
    FILE* f;
    uint8_t* fifo;
    size_t len;

    snprintf(path, sizeof path, "%s.regs", base);
    f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[gx] cannot open %s\n", path); return 1; }
    if (fread(g_cp, 4, 0x100, f) != 0x100 || fread(g_xf, 4, 0x1100, f) != 0x1100 || fread(g_bp, 4, 0x100, f) != 0x100) {
        fprintf(stderr, "[gx] short register file %s\n", path); fclose(f); return 1;
    }
    fclose(f);

    if (with_ram) {
        snprintf(path, sizeof path, "%s.ram", base);
        f = fopen(path, "rb");
        if (!f) { fprintf(stderr, "[gx] cannot open %s\n", path); return 1; }
        if (fread(s->mem, 1, MEM1_SIZE, f) != MEM1_SIZE) { fprintf(stderr, "[gx] short RAM file\n"); fclose(f); return 1; }
        fclose(f);
        /* Every byte of memory replaced, with no BP write to say so: hash each
         * texture again at its next use (gxr_tev.c, the texture epoch). */
        tex_epoch_advance();
    }

    snprintf(path, sizeof path, "%s.fifo", base);
    f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[gx] cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    fifo = (uint8_t*)malloc(len ? len : 1);
    if (fread(fifo, 1, len, f) != len) { fprintf(stderr, "[gx] short FIFO file\n"); fclose(f); free(fifo); return 1; }
    fclose(f);
    *fifo_out = fifo;
    *len_out = len;
    return 0;
}

int gx_replay(CpuState* s, const char* base)
{
    uint8_t* fifo = NULL;
    size_t len = 0, done = 0;
    /* SOA_REPLAY_REPEAT=N renders the capture N times, each from its own RAM
     * image, so that a profiler (SOA_HOSTPROF) has more than one frame's worth
     * of the pixel path to sample. The PNG is the last pass's, the same. */
    const char* rep = getenv("SOA_REPLAY_REPEAT");
    int n = rep && atoi(rep) > 1 ? atoi(rep) : 1, i;
    for (i = 0; i < n; i++) {
        if (fifo) free(fifo);
        if (load_capture(s, base, 1, &fifo, &len)) return 1;
        gxr_reset_efb();
        g_dl_rec = 0;
        g_replaying = 1;
        done = parse(s, fifo, len, 0);
        g_replaying = 0;
        gxr_flush();
    }
    fprintf(stderr, "[gx] replayed %zu of %zu bytes%s\n", done, len, n > 1 ? " (the last of the repeats)" : "");
    gx_report();
    free(fifo);
    return 0;
}

/* gxr.c's side of a pair replay, declared here as gx.c declares the rest of
 * what it calls there. */
#define GXR_PAIR_RECORD 1u
#define GXR_PAIR_LERP 2u
int gxr_pair_mode(unsigned flags, float t);
unsigned long long gxr_pair_rotations(void);
void gxr_pair_list(void* file);
void gxr_pair_report(void);
void gxr_set_output(const char* png_path);

/* The image between two consecutive captures (PLAN-60FPS-MODS H10), in three
 * passes: A recorded (to A.png), B as it is (to B.png, the check that the
 * pair machinery changes nothing), then B again with every draw matched in A
 * moved to (1-t)*A + t*B (to B.mid.png). SOA_PAIR_T is t, 0.5 unless set;
 * SOA_PAIR_LIST names a file for the matched pairs, fifopair's numbering.
 * Nothing is captured and no frame hook runs between the passes. */
int gx_replay_pair(CpuState* s, const char* a, const char* b)
{
    uint8_t* fifo;
    size_t len, done;
    char png[1024];
    const char* t_env = getenv("SOA_PAIR_T");
    const char* list = getenv("SOA_PAIR_LIST");
    const char* draws = getenv("SOA_GXR_DRAWS");
    float t = 0.5f;
    FILE* lf = NULL;
    unsigned long long rot;
    int pass;
    g_dump_checked = 1;
    g_dump_list = NULL;
    g_frame_hook = NULL;
    fprintf(stderr, "[pair] a pair replay: no capture is written and no frame hook runs between the passes\n");
    if (t_env && *t_env) {
        char* end;
        double d = strtod(t_env, &end);
        if (*end || d < 0.0 || d > 1.0) {
            fprintf(stderr, "[pair] SOA_PAIR_T=%s is not a number from 0 to 1\n", t_env);
            return 1;
        }
        t = (float)d;
    }
    if (draws && *draws) {
        fprintf(stderr, "[pair] SOA_GXR_DRAWS stops a pass part way, and a pair needs every draw of both\n");
        return 1;
    }
    if (list && *list) {
        lf = fopen(list, "w");
        if (!lf) { fprintf(stderr, "[pair] cannot write %s\n", list); return 1; }
        gxr_pair_list(lf);
    }
    for (pass = 1; pass <= 3; pass++) {
        const char* base = pass == 1 ? a : b;
        unsigned flags = pass == 1 ? GXR_PAIR_RECORD : (pass == 3 ? GXR_PAIR_LERP : 0u);
        fprintf(stderr, "[pair] pass %d: %s %s\n", pass, pass == 1 ? "record" : (pass == 2 ? "as it is" : "between"), base);
        if (!gxr_pair_mode(flags, t)) { fprintf(stderr, "[pair] cannot set pair mode\n"); return 1; }
        snprintf(png, sizeof png, pass == 3 ? "%s.mid.png" : "%s.png", base);
        gxr_set_output(png);
        /* pass 3 keeps pass 2's RAM: both start from B's end-of-frame image */
        if (load_capture(s, base, pass != 3, &fifo, &len)) return 1;
        rot = gxr_pair_rotations();
        gxr_reset_efb();
        g_dl_rec = 0;
        g_replaying = 1;
        done = parse(s, fifo, len, 0);
        g_replaying = 0;
        gxr_flush();
        free(fifo);
        fprintf(stderr, "[gx] replayed %zu of %zu bytes\n", done, len);
        if (pass == 1 && gxr_pair_rotations() != rot + 1) {
            fprintf(stderr, "[pair] %s holds %llu screen copies, not one: a pair is two captures of one frame each\n",
                    a, gxr_pair_rotations() - rot);
            return 1;
        }
    }
    gxr_pair_mode(0, t);
    gxr_pair_list(NULL);
    if (lf) fclose(lf);
    gxr_pair_report();
    gx_report();
    return 0;
}
