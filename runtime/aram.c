/*
 * Auxiliary RAM (ARAM) and the DSP-side registers that drive it.
 *
 * 16 MB of memory only the DSP and its DMA engine can reach. The game keeps
 * audio samples there (SPEC: the stock AX ucode reads voices out of ARAM) and
 * the SDK's ARQ queue moves data in and out with the DMA engine below. DMA
 * completes instantly and raises the ARAM interrupt in the DSP status
 * register.
 *
 *   0xCC00500A  DSP_CSR     RES, PIINT, HALT, AIDINT/MSK, ARINT/MSK, DSPINT/MSK, DMAINT, DSPINIT
 *   0xCC005012  AR_SIZE
 *   0xCC005016  AR_MODE     bit 0 reads back set: the controller is ready
 *   0xCC00501A  AR_REFRESH
 *   0xCC005020  AR_DMA_MMADDR  main-memory address (u32)
 *   0xCC005024  AR_DMA_ARADDR  ARAM address (u32)
 *   0xCC005028  AR_DMA_CNT     bit 31 direction (1 = ARAM to main memory), length below
 */
#define _CRT_SECURE_NO_WARNINGS
#include "cpu.h"
#include "disc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARAM_SIZE (16u << 20)
#define ARAM_MASK (ARAM_SIZE - 1)

#define CSR_RES 0x0001u
#define CSR_PIINT 0x0002u
#define CSR_HALT 0x0004u
#define CSR_AIDINT 0x0008u
#define CSR_AIDINTMSK 0x0010u
#define CSR_ARINT 0x0020u
#define CSR_ARINTMSK 0x0040u
#define CSR_DSPINT 0x0080u
#define CSR_DSPINTMSK 0x0100u
#define CSR_W1C (CSR_PIINT | CSR_AIDINT | CSR_ARINT | CSR_DSPINT)
#define CSR_DSPINITCODE 0x0400u
#define CSR_DSPINIT 0x0800u
#define CSR_STORED (CSR_HALT | CSR_AIDINTMSK | CSR_ARINTMSK | CSR_DSPINTMSK | CSR_DSPINITCODE | CSR_DSPINIT)

void dsp_reset(void);
void dsp_stub_boot(void);

static uint8_t* g_aram;
static int g_ar_busy;
static uint64_t g_ar_due;
uint32_t guest_timebase_lo(CpuState* s);
uint32_t guest_timebase_hi(CpuState* s);
#define TB_HZ 40500000ull
uint64_t guest_time_device(CpuState* s); /* hle.c: the timebase as a device reads it */
static uint64_t tb_now(CpuState* s) { return guest_time_device(s); }
static uint16_t g_csr = CSR_HALT | CSR_DSPINIT, g_ar_size, g_ar_mode, g_ar_refresh;
static uint32_t g_mmaddr, g_araddr;
static uint16_t g_cnt_hi; /* the register bank is 16-bit; the low half of CNT starts the DMA */
static uint64_t g_dmas, g_dma_bytes;

/* ---- where a voice's samples came from (PLAN E1) -------------------------
 *
 * The mixer can only see an ARAM address, and a census of voice starts is
 * worth little without a name beside each one. Three hops join the two, and
 * only the first needs anything the runtime does not already have: a disc
 * file's first 32 bytes identify the buffer a DVD read filled, that buffer is
 * the source of the main-to-ARAM transfers that follow it, and those name the
 * ARAM run a voice reads out of. The index is built once from the FST and one
 * 32-byte read of each of the disc's 5,552 files; after that only a transfer
 * that begins a new upload pays for a hash, and a voice start pays for one
 * scan of the interval list.
 *
 * Every file is indexed, not only the audio, so that an upload this cannot
 * name is a real gap rather than a file the index skipped. Anchoring on the
 * first bytes works because a bank is read whole into one buffer and uploaded
 * from its start, and a stream's refills land in the same buffer and inherit
 * its name. Where it cannot work at all -- tone.samp and eight music banks
 * share their entire first waveform -- the length of the finished upload
 * decides, since a bank occupies exactly its own size in ARAM. Anything left
 * over is reported as unidentified rather than guessed at.
 */

#define SRC_FILES 8192
#define SRC_HASH 32768
#define SRC_EXTENTS 64
#define SRC_INTERVALS 512

static struct {
    char name[28];
    uint32_t size;
    int stream; /* a .dsp: read a piece at a time, so its buffer is one piece */
} g_src_file[SRC_FILES];
static int g_src_files;
static struct { uint64_t key; int file; } g_src_hash[SRC_HASH];
static struct { uint32_t m0, m1, ar_next; int file; uint32_t serial; } g_src_ext[SRC_EXTENTS];
static int g_src_exts;
static struct { uint32_t a0, a1, mm_next; int file; uint32_t serial; } g_src_ivl[SRC_INTERVALS];
static int g_src_ivls, g_src_last = -1;
static uint32_t g_src_serial;
static int g_src_ready; /* 0 not tried, 1 built, -1 no disc image or no FST */
static uint64_t g_src_named, g_src_unnamed;

static uint64_t src_hash32(const uint8_t* p)
{
    uint64_t h = 1469598103934665603ull;
    int i;
    for (i = 0; i < 32; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h ? h : 1; /* zero means "empty slot" below */
}

static void src_hash_put(uint64_t key, int file)
{
    unsigned i = (unsigned)(key % SRC_HASH), n;
    for (n = 0; n < SRC_HASH; n++) {
        if (!g_src_hash[i].key) {
            g_src_hash[i].key = key;
            g_src_hash[i].file = file;
            return;
        }
        /* two files that begin alike cannot be told apart this way, and naming
         * one of them would be worse than admitting it */
        if (g_src_hash[i].key == key) {
            if (g_src_hash[i].file != file) g_src_hash[i].file = -2;
            return;
        }
        i = (i + 1) % SRC_HASH;
    }
}

static int src_hash_get(uint64_t key)
{
    unsigned i = (unsigned)(key % SRC_HASH), n;
    for (n = 0; n < SRC_HASH; n++) {
        if (!g_src_hash[i].key) return -1;
        if (g_src_hash[i].key == key) return g_src_hash[i].file;
        i = (i + 1) % SRC_HASH;
    }
    return -1;
}

/* The fallback for an upload whose first bytes named nothing: tone.samp shares
 * its whole first waveform with eight music banks, so no prefix can separate
 * them, but a bank uploaded whole occupies exactly its own size rounded to 32.
 * Only worth believing when one file on the disc is that long. */
static int src_by_span(uint32_t span)
{
    int i, found = -1;
    if (span < 4096) return -1;
    for (i = 0; i < g_src_files; i++)
        if (((g_src_file[i].size + 31u) & ~31u) == span) {
            if (found >= 0) return -2;
            found = i;
        }
    return found;
}

static uint32_t src_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Build the census now rather than at the first ARAM DMA: it reads the
 * head of every file on the disc, 0.3-1.1 s under load, and main.c calls
 * this before the game starts, where no clock is running (M19). */
static void src_build(void);

void aram_census_prepare(void)
{
    if (!g_src_ready) src_build();
}

static int src_suffix(const char* name, const char* suf)
{
    size_t n = strlen(name), m = strlen(suf);
    size_t i;
    if (n < m) return 0;
    for (i = 0; i < m; i++) {
        char a = name[n - m + i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return 0;
    }
    return 1;
}

/* The disc's file table and each file's first 32 bytes, from disc.c: the
 * image main.c opened, wherever the command line or soa.ini put it
 * (disc-layer I1). */
static void src_build(void)
{
    size_t size = 0;
    const uint8_t* tbl = disc_fst(&size);
    uint32_t n, i, strtab;

    g_src_ready = -1;
    if (!tbl || size <= 12) return;
    n = src_be32(tbl + 8);
    if (n == 0 || n > size / 12u) return;
    strtab = n * 12u;
    if (strtab >= size) return;
    for (i = 1; i < n && g_src_files < SRC_FILES; i++) {
        const uint8_t* e = tbl + i * 12u;
        uint32_t noff = ((uint32_t)e[1] << 16) | ((uint32_t)e[2] << 8) | e[3];
        uint32_t off = src_be32(e + 4), len = src_be32(e + 8);
        const char* name;
        uint8_t head[32];
        int dsp;
        if (e[0]) continue; /* a directory */
        if ((size_t)strtab + noff >= size) continue;
        name = (const char*)tbl + strtab + noff;
        if (memchr(name, 0, size - (strtab + noff)) == NULL) continue;
        dsp = src_suffix(name, ".dsp");
        if (len < sizeof head || disc_peek(off, head, sizeof head) != sizeof head) continue;
        snprintf(g_src_file[g_src_files].name, sizeof g_src_file[0].name, "%s", name);
        g_src_file[g_src_files].size = len;
        g_src_file[g_src_files].stream = dsp;
        src_hash_put(src_hash32(head), g_src_files);
        /* a stream's first refill may begin past the .dsp header */
        if (dsp && len >= 128 && disc_peek((uint64_t)off + 96, head, sizeof head) == sizeof head)
            src_hash_put(src_hash32(head), g_src_files);
        g_src_files++;
    }
    g_src_ready = g_src_files ? 1 : -1;
}

/* The main-memory buffer a file was read into. A later read over the same
 * memory retires what was there: the heap reuses a bank's buffer as soon as
 * the upload is done. */
static void src_ext_put(uint32_t m0, uint32_t len, uint32_t ar, int file)
{
    uint32_t m1 = m0 + (g_src_file[file].stream ? len : g_src_file[file].size);
    int i, slot = -1;
    for (i = 0; i < g_src_exts; i++) {
        if (g_src_ext[i].m1 <= m0 || g_src_ext[i].m0 >= m1) continue;
        if (g_src_ext[i].m0 >= m0 && g_src_ext[i].m1 <= m1) {
            g_src_ext[i].m1 = g_src_ext[i].m0; /* covered: retire it */
            slot = i;
        } else if (g_src_ext[i].m0 < m0) {
            g_src_ext[i].m1 = m0;
        } else {
            g_src_ext[i].m0 = m1;
        }
    }
    if (slot < 0) {
        if (g_src_exts < SRC_EXTENTS) {
            slot = g_src_exts++;
        } else {
            int o = 0;
            for (i = 1; i < SRC_EXTENTS; i++)
                if (g_src_ext[i].serial < g_src_ext[o].serial) o = i;
            slot = o;
        }
    }
    g_src_ext[slot].m0 = m0;
    g_src_ext[slot].m1 = m1;
    g_src_ext[slot].ar_next = ar + len;
    g_src_ext[slot].file = file;
    g_src_ext[slot].serial = ++g_src_serial;
}

/* One upload walks up ARAM without a break -- the bank heap is a bump cursor
 * and ARQ hands the request over in order. A transfer out of a buffer we have
 * a name for but landing somewhere else in ARAM is a different subsystem that
 * happens to have been given the same heap memory, so the name dies with it
 * rather than being lent out. */
static int src_ext_get(uint32_t mm, uint32_t ar, uint32_t len)
{
    int i, best = -1;
    for (i = 0; i < g_src_exts; i++)
        if (mm >= g_src_ext[i].m0 && mm < g_src_ext[i].m1 &&
            (best < 0 || g_src_ext[i].serial > g_src_ext[best].serial))
            best = i;
    if (best < 0) return -1;
    if (g_src_ext[best].ar_next && ar != g_src_ext[best].ar_next) {
        g_src_ext[best].m1 = g_src_ext[best].m0;
        return -1;
    }
    g_src_ext[best].ar_next = ar + len;
    return g_src_ext[best].file;
}

static int src_ivl_get(uint32_t ar)
{
    int i, best = -1;
    for (i = 0; i < g_src_ivls; i++)
        if (ar >= g_src_ivl[i].a0 && ar < g_src_ivl[i].a1 &&
            (best < 0 || g_src_ivl[i].serial > g_src_ivl[best].serial))
            best = i;
    return best < 0 ? -1 : g_src_ivl[best].file;
}

/* Chunks arrive 4096 bytes at a time (ARQ cuts a low-priority request up), so
 * an upload is only a run of them: coalescing is what keeps a 2.4 MB bank to
 * one interval instead of six hundred. The bank heap is one bump cursor and
 * every bank in a run of them is ARAM-contiguous with the last, so the break
 * between two banks is in main memory, not in ARAM: the source buffer has to
 * be continuous too, or the run is a different file's. */
static void src_ivl_add(uint32_t mm, uint32_t a0, uint32_t len, int file)
{
    uint32_t a1 = a0 + len;
    int i;
    if (g_src_last >= 0 && g_src_ivl[g_src_last].file == file && g_src_ivl[g_src_last].a1 == a0 &&
        g_src_ivl[g_src_last].mm_next == mm) {
        g_src_ivl[g_src_last].a1 = a1;
        g_src_ivl[g_src_last].mm_next = mm + len;
        g_src_ivl[g_src_last].serial = ++g_src_serial;
        return;
    }
    for (i = 0; i < g_src_ivls; i++)
        if (g_src_ivl[i].a0 == a0 && g_src_ivl[i].file == file) { /* a ring, refilled again */
            if (a1 > g_src_ivl[i].a1) g_src_ivl[i].a1 = a1;
            g_src_ivl[i].mm_next = mm + len;
            g_src_ivl[i].serial = ++g_src_serial;
            g_src_last = i;
            return;
        }
    if (g_src_ivls < SRC_INTERVALS) {
        i = g_src_ivls++;
    } else {
        int o = 0, j;
        for (j = 1; j < SRC_INTERVALS; j++)
            if (g_src_ivl[j].serial < g_src_ivl[o].serial) o = j;
        i = o;
    }
    g_src_ivl[i].a0 = a0;
    g_src_ivl[i].a1 = a1;
    g_src_ivl[i].mm_next = mm + len;
    g_src_ivl[i].file = file;
    g_src_ivl[i].serial = ++g_src_serial;
    g_src_last = i;
}

static const char* src_ivl_name(int i)
{
    int f = g_src_ivl[i].file;
    if (f < 0) f = src_by_span(g_src_ivl[i].a1 - g_src_ivl[i].a0);
    if (f >= 0) return g_src_file[f].name;
    return f == -2 ? "(several files that long)" : "(unidentified)";
}

/* A named run has a known length, and two banks read into adjacent buffers
 * upload back to back with no break in either address: without this the second
 * one is swallowed by the first and wears its name. */
static int src_run_full(int i)
{
    int f = g_src_ivl[i].file;
    if (f < 0 || g_src_file[f].stream) return 0; /* unknown, or a ring refilled forever */
    return g_src_ivl[i].a1 - g_src_ivl[i].a0 >= ((g_src_file[f].size + 31u) & ~31u);
}

static void src_note_dma(CpuState* s, uint32_t mm, uint32_t ar, uint32_t len)
{
    int file = -1;
    if (!g_src_ready) src_build();
    /* A transfer that carries on where the last one stopped, in ARAM and in
     * main memory both, is the same upload: its bytes are somewhere in the
     * middle of a file and must not be looked up as if they were a file's
     * first, which is how a bank's interior chunk comes to be mistaken for
     * some other file and cuts the run in two. */
    if (g_src_last >= 0 && g_src_ivl[g_src_last].a1 == ar && g_src_ivl[g_src_last].mm_next == mm &&
        !src_run_full(g_src_last)) {
        int carried = g_src_ivl[g_src_last].file;
        src_ivl_add(mm, ar, len, carried);
        if (carried < 0) g_src_unnamed++;
        else g_src_named++;
        return;
    }
    if (g_src_ready > 0 && len >= 32) {
        int hit = src_hash_get(src_hash32(mem_ptr(s, mm)));
        if (hit >= 0) {
            file = hit;
            src_ext_put(mm, len, ar, file);
        }
    }
    if (file < 0) file = src_ext_get(mm, ar, len);
    if (file < 0) file = src_ivl_get(ar); /* a refill of a ring already named */
    if (file < 0) g_src_unnamed++; else g_src_named++;
    src_ivl_add(mm, ar, len, file);
}

/* The name of the file whose bytes were uploaded over an ARAM address, and the
 * run they were uploaded as. */
const char* aram_source_name(uint32_t byte_addr, uint32_t* a0, uint32_t* a1)
{
    int i, best = -1;
    byte_addr &= ARAM_MASK;
    for (i = 0; i < g_src_ivls; i++)
        if (byte_addr >= g_src_ivl[i].a0 && byte_addr < g_src_ivl[i].a1 &&
            (best < 0 || g_src_ivl[i].serial > g_src_ivl[best].serial))
            best = i;
    if (a0) *a0 = best < 0 ? 0 : g_src_ivl[best].a0;
    if (a1) *a1 = best < 0 ? 0 : g_src_ivl[best].a1;
    if (best < 0) return "(nothing uploaded there)";
    return src_ivl_name(best);
}

static void dma(CpuState* s, uint32_t cnt)
{
    uint32_t len = cnt & 0x7FFFFFFFu;
    int to_main = (cnt >> 31) & 1;
    if (!g_aram) g_aram = (uint8_t*)calloc(1, ARAM_SIZE);
    if ((g_araddr & ARAM_MASK) + len > ARAM_SIZE || (g_mmaddr & MEM_MASK) + len > MEM1_SIZE) {
        fprintf(stderr, "[aram] DMA out of range: mm %08X ar %08X len %u\n", g_mmaddr, g_araddr, len);
    } else if (to_main) {
        memcpy(mem_ptr(s, g_mmaddr), g_aram + (g_araddr & ARAM_MASK), len);
    } else {
        memcpy(g_aram + (g_araddr & ARAM_MASK), mem_ptr(s, g_mmaddr), len);
        src_note_dma(s, g_mmaddr & MEM_MASK, g_araddr & ARAM_MASK, len);
    }
    g_dmas++;
    g_dma_bytes += len;
    {
        static int verbose = -1;
        if (verbose < 0) verbose = getenv("SOA_ARAM_VERBOSE") ? 1 : 0;
        if (verbose && g_dmas <= 4000 && (g_araddr & ARAM_MASK) + len <= ARAM_SIZE && (g_mmaddr & MEM_MASK) + len <= MEM1_SIZE) {
            const uint8_t* p = to_main ? (const uint8_t*)mem_ptr(s, g_mmaddr) : g_aram + (g_araddr & ARAM_MASK);
            fprintf(stderr, "[aram] %s mm %08X ar %08X len %u: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
                    to_main ? "ARAM->main" : "main->ARAM", g_mmaddr, g_araddr, len, p[0], p[1], p[2], p[3], p[4], p[5],
                    p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
        }
    }
    /* the engine takes time; the interrupt (and the busy bit clearing) wait for it */
    g_ar_busy = 1;
    g_ar_due = tb_now(s) + TB_HZ / 20000 + (uint64_t)len * TB_HZ / 20000000u;
}

/* From the interrupt code: a DMA whose time is up completes with ARINT. */
void aram_poll(CpuState* s)
{
    if (g_ar_busy && tb_now(s) >= g_ar_due) { g_ar_busy = 0; g_csr |= CSR_ARINT; }
}

/* The fixed-step clock's idle skip (irq.c): when the DMA in flight is due. */
int aram_next_due(uint64_t* due)
{
    if (!g_ar_busy) return 0;
    *due = g_ar_due;
    return 1;
}

void aram_poll(CpuState* s);

int aram_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out)
{
    if (ea < 0xCC005000u || ea >= 0xCC005040u) return 0;
    aram_poll(s); /* a poll loop with interrupts off still sees the DMA finish */
    switch (ea - 0xCC005000u) {
    case 0x0A: if (size != 2) return 0; *out = g_csr | (g_ar_busy ? 0x0200u : 0u); return 1; /* DSPDMA: in progress */
    case 0x12: if (size != 2) return 0; *out = g_ar_size; return 1;
    case 0x16: if (size != 2) return 0; *out = g_ar_mode | 1u; return 1; /* ready */
    case 0x1A: if (size != 2) return 0; *out = g_ar_refresh; return 1;
    case 0x20: *out = size == 4 ? g_mmaddr : (g_mmaddr >> 16); return 1;
    case 0x22: if (size != 2) return 0; *out = g_mmaddr & 0xFFFFu; return 1;
    case 0x24: *out = size == 4 ? g_araddr : (g_araddr >> 16); return 1;
    case 0x26: if (size != 2) return 0; *out = g_araddr & 0xFFFFu; return 1;
    /* ARStartDMA writes the direction bit into CNT_H, then reads CNT_H back to merge the
     * length's high bits into it: the read must return what was written, or the direction
     * is lost and every ARAM-to-main-memory fetch turns into a store over the cached data. */
    case 0x28: *out = size == 4 ? ((uint32_t)g_cnt_hi << 16) : g_cnt_hi; return 1;
    case 0x2A: *out = 0; return 1; /* the low half reads as zero once the DMA is done */
    default: return 0;
    }
}

int aram_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v)
{
    uint16_t was;
    if (ea < 0xCC005000u || ea >= 0xCC005040u) return 0;
    switch (ea - 0xCC005000u) {
    case 0x0A:
        if (size != 2) return 0;
        /* RES self-clears; interrupt bits are write-one-to-clear; the rest is stored. */
        was = g_csr;
        g_csr = (uint16_t)((g_csr & CSR_W1C & ~(v & CSR_W1C)) | (v & CSR_STORED));
        if (v & CSR_RES) dsp_reset();
        if ((was & CSR_DSPINIT) && !(v & CSR_DSPINIT)) { /* boot from ARAM: the stub runs */
            g_csr |= CSR_DSPINITCODE;
            dsp_stub_boot();
        }
        return 1;
    case 0x12: if (size != 2) return 0; g_ar_size = (uint16_t)v; return 1;
    case 0x16: if (size != 2) return 0; g_ar_mode = (uint16_t)v; return 1;
    case 0x1A: if (size != 2) return 0; g_ar_refresh = (uint16_t)v; return 1;
    case 0x20:
        g_mmaddr = size == 4 ? (uint32_t)v : ((g_mmaddr & 0xFFFFu) | ((uint32_t)v << 16));
        return 1;
    case 0x22: if (size != 2) return 0; g_mmaddr = (g_mmaddr & 0xFFFF0000u) | ((uint32_t)v & 0xFFFFu); return 1;
    case 0x24:
        g_araddr = size == 4 ? (uint32_t)v : ((g_araddr & 0xFFFFu) | ((uint32_t)v << 16));
        return 1;
    case 0x26: if (size != 2) return 0; g_araddr = (g_araddr & 0xFFFF0000u) | ((uint32_t)v & 0xFFFFu); return 1;
    case 0x28: /* a full write starts the transfer; a half write waits for the low half */
        if (size == 4) { dma(s, (uint32_t)v); return 1; }
        g_cnt_hi = (uint16_t)v;
        return 1;
    case 0x2A: if (size != 2) return 0; dma(s, ((uint32_t)g_cnt_hi << 16) | ((uint32_t)v & 0xFFFFu)); return 1;
    default: return 0;
    }
}

uint8_t* aram_memory(void)
{
    if (!g_aram) g_aram = (uint8_t*)calloc(1, ARAM_SIZE);
    return g_aram;
}

/* True when a finished DMA has an enabled, unacknowledged interrupt. */
int aram_irq_pending(void)
{
    return (g_csr & CSR_ARINT) && (g_csr & CSR_ARINTMSK);
}

/* The mailbox and AI DMA models (dsp.c) raise their interrupt bits here. */
void dsp_csr_raise(uint16_t bits)
{
    g_csr |= bits;
}

int dsp_irq_pending(void)
{
    return (g_csr & CSR_DSPINT) && (g_csr & CSR_DSPINTMSK);
}

int ai_dma_irq_pending(void)
{
    return (g_csr & CSR_AIDINT) && (g_csr & CSR_AIDINTMSK);
}

void aram_report(void)
{
    fprintf(stderr, "[aram] %llu DMAs, %llu bytes\n", (unsigned long long)g_dmas,
            (unsigned long long)g_dma_bytes);
    {
        int i, named = 0;
        for (i = 0; i < g_src_ivls; i++)
            if (src_ivl_name(i)[0] != '(') named++;
        fprintf(stderr,
                "[aram] census sources: %d disc files indexed; %llu transfers named at the "
                "transfer, %llu not, in %d ARAM upload runs of which %d are named\n",
                g_src_files, (unsigned long long)g_src_named, (unsigned long long)g_src_unnamed,
                g_src_ivls, named);
    }
    {
        /* Which banks and streams reached ARAM at all: a bank that is here and
         * never starts a voice is the case PLAN E1 is looking for. */
        char line[1024];
        size_t at = 0;
        int shown, picked[16], np = 0, i, j;
        for (shown = 0; shown < 16; shown++) {
            int best = -1;
            for (i = 0; i < g_src_ivls; i++) {
                if (src_ivl_name(i)[0] == '(') continue; /* nothing names this run */
                for (j = 0; j < np; j++)
                    if (picked[j] == i) break;
                if (j < np) continue;
                if (best < 0 || g_src_ivl[i].a1 - g_src_ivl[i].a0 > g_src_ivl[best].a1 - g_src_ivl[best].a0)
                    best = i;
            }
            if (best < 0) break;
            picked[np++] = best;
            if (at + 48 < sizeof line)
                at += (size_t)snprintf(line + at, sizeof line - at, "%s%s %06X-%06X", at ? " | " : "",
                                       src_ivl_name(best), g_src_ivl[best].a0, g_src_ivl[best].a1);
        }
        fprintf(stderr, "[aram] census largest named uploads: %s\n", at ? line : "none");
    }
}

/* ---- a snapshot (snapshot.h; docs/SNAPSHOT.md) ------------------------------
 * ARAM's 16 MB (the samples the sound driver streams from), its registers,
 * and a DMA in flight with its due tick. The census of where samples came
 * from is the report's, and starts again. */
#include "snapshot.h"
void aram_snap(Snap* sn)
{
    void* p = g_aram;
    size_t bytes = g_aram ? (size_t)ARAM_SIZE : 0u;
    snap_buffer(sn, &p, &bytes);
    if (snap_loading(sn) && !snap_failed(sn)) {
        g_aram = (uint8_t*)p; /* snap_buffer freed the old one if it made another; NULL: never touched */
        if (bytes && bytes != (size_t)ARAM_SIZE) {
            free(g_aram);
            g_aram = NULL;
            snap_fail(sn, "ARAM is %zu bytes, not %u", bytes, (unsigned)ARAM_SIZE);
            return;
        }
    }
    SNAP(sn, g_ar_busy);
    SNAP(sn, g_ar_due);
    SNAP(sn, g_csr);
    SNAP(sn, g_ar_size);
    SNAP(sn, g_ar_mode);
    SNAP(sn, g_ar_refresh);
    SNAP(sn, g_mmaddr);
    SNAP(sn, g_araddr);
    SNAP(sn, g_cnt_hi);
    SNAP(sn, g_dmas);
    SNAP(sn, g_dma_bytes);
}
