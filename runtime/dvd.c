/*
 * The DVD interface (DI), modelled at the register level.
 *
 * The SDK drives the drive through seven registers and waits for a
 * transfer-complete interrupt. A read is served by disc.c (the player's
 * image) straight into guest memory, and the completion interrupt is raised
 * once the drive's modelled time is up.
 *
 *   0xCC006000  DISR      status: BRK, DEINTMASK, DEINT, TCINTMASK, TCINT, BRKINTMASK, BRKINT
 *   0xCC006004  DICVR     cover: CVR, CVRINTMASK, CVRINT
 *   0xCC006008  DICMDBUF0 command in the top byte
 *   0xCC00600C  DICMDBUF1 usually the disc offset >> 2
 *   0xCC006010  DICMDBUF2 usually the length
 *   0xCC006014  DIMAR     DMA address in main memory
 *   0xCC006018  DILENGTH  DMA length
 *   0xCC00601C  DICR      TSTART, DMA, RW
 *   0xCC006020  DIIMMBUF  result of an immediate (non-DMA) command
 *   0xCC006024  DICFG
 */
#define _CRT_SECURE_NO_WARNINGS
#include "cpu.h"
#include "disc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DI_BASE 0xCC006000u

#define DISR_BRK 0x01u
#define DISR_DEINTMASK 0x02u
#define DISR_DEINT 0x04u
#define DISR_TCINTMASK 0x08u
#define DISR_TCINT 0x10u
#define DISR_BRKINTMASK 0x20u
#define DISR_BRKINT 0x40u
#define DISR_MASKS (DISR_DEINTMASK | DISR_TCINTMASK | DISR_BRKINTMASK)
#define DISR_INTS (DISR_DEINT | DISR_TCINT | DISR_BRKINT)

#define DICVR_CVRINTMASK 0x02u
#define DICVR_CVRINT 0x04u

#define DICR_TSTART 0x01u
#define DICR_DMA 0x02u

static uint32_t g_disr, g_dicvr, g_cmd[3], g_mar, g_len, g_imm, g_cfg;

/* A command takes time on the real drive, and the SDK's callbacks run
 * from the completion interrupt; completing at once let callbacks run
 * before their requesters had finished. Data moves immediately (nothing
 * may touch the buffer before completion anyway); the interrupt waits. */
static int g_busy;
static uint64_t g_due;
uint32_t guest_timebase_lo(CpuState* s);
uint32_t guest_timebase_hi(CpuState* s);
#define TB_HZ 40500000ull
uint64_t guest_time_device(CpuState* s); /* hle.c: the timebase as a device reads it */
static uint64_t tb_now(CpuState* s) { return guest_time_device(s); }

static void complete(void)
{
    uint32_t cmd = g_cmd[0] >> 24;
    if (cmd == 0x12 || cmd == 0xA8) {
        /* The DMA engine counts DILENGTH down as it moves data and leaves
         * DIMAR at the end of the transfer; the SDK reads DILENGTH back to
         * learn how much arrived, and retries if it is not zero. */
        g_mar += g_len;
        g_len = 0;
    }
    g_busy = 0;
    g_disr |= DISR_TCINT; /* transfer complete; raised on the next delivery */
}

/* From the interrupt code: finish the command in flight once its time is up. */
void di_poll(CpuState* s)
{
    if (g_busy && tb_now(s) >= g_due) complete();
}

/* The fixed-step clock's idle skip (irq.c): when the command in flight is due. */
int di_next_due(uint64_t* due)
{
    if (!g_busy) return 0;
    *due = g_due;
    return 1;
}
static uint64_t g_reads, g_bytes;

/* The image is disc.c's (disc-layer I1): main.c opened it, or stopped, before
 * the game ran, so every read here has a disc behind it. */
static void disc_read(CpuState* s, uint64_t offset, uint32_t addr, uint32_t length)
{
    /* The length comes from a guest store to the DI registers, so the bound
     * has to hold for every value of it: offset + length overflows 32 bits
     * and wraps back under MEM1_SIZE for a large enough length, which would
     * pass a read of most of 4 GB into the image. Subtract instead of add. */
    if (length > MEM1_SIZE || (addr & MEM_MASK) > MEM1_SIZE - length) {
        fprintf(stderr, "[dvd] read of %u bytes to %08X leaves MEM1\n", length, addr);
        return;
    }
    disc_serve(offset, mem_ptr(s, addr), length);
    g_reads++;
    g_bytes += length;
}

static void execute(CpuState* s)
{
    uint32_t cmd = g_cmd[0] >> 24;
    switch (cmd) {
    case 0x12: { /* inquiry: drive revision, device code, firmware date */
        uint8_t info[0x20] = {0};
        uint32_t date = 0x20020402u;
        memcpy(info + 4, &(uint32_t){BSWAP32(date)}, 4);
        if ((g_mar & MEM_MASK) + sizeof info <= MEM1_SIZE) memcpy(mem_ptr(s, g_mar), info, sizeof info);
        break;
    }
    case 0xA8: /* read (0xA8000000) and read disk ID (0xA8000040): offset is in words */
        disc_read(s, (uint64_t)g_cmd[1] << 2, g_mar, g_len);
        break;
    case 0xAB: /* seek */
    case 0xE1: /* audio stream control */
    case 0xE3: /* stop motor */
    case 0xE4: /* audio buffer config */
        g_imm = 0;
        break;
    case 0xE0: /* request error */
    case 0xE2: /* request audio status */
        g_imm = 0;
        break;
    default:
        fprintf(stderr, "[dvd] unknown command %02X (%08X %08X %08X)\n", cmd, g_cmd[0], g_cmd[1],
                g_cmd[2]);
        g_imm = 0;
        break;
    }
    {
        /* seek plus transfer at the drive's rate; other commands are quick */
        uint64_t ticks = cmd == 0xA8 ? TB_HZ * 6 / 1000 + (uint64_t)g_len * TB_HZ / 3000000u : TB_HZ / 1000;
        g_busy = 1;
        g_due = tb_now(s) + ticks;
    }
}

int di_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out)
{
    di_poll(s); /* the drive progresses whether or not interrupts are on */
    if (ea < DI_BASE || ea >= DI_BASE + 0x28 || size != 4) return 0;
    switch (ea - DI_BASE) {
    case 0x00: *out = g_disr; break;
    case 0x04: *out = g_dicvr; break; /* cover closed */
    case 0x08: *out = g_cmd[0]; break;
    case 0x0C: *out = g_cmd[1]; break;
    case 0x10: *out = g_cmd[2]; break;
    case 0x14: *out = g_mar; break;
    case 0x18: *out = g_len; break;
    case 0x1C: *out = g_busy ? 3u : 0u; break; /* TSTART and DMA while a command is in flight */
    case 0x20: *out = g_imm; break;
    case 0x24: *out = g_cfg; break;
    default: *out = 0; break;
    }
    return 1;
}

int di_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v)
{
    uint32_t w = (uint32_t)v;
    if (ea < DI_BASE || ea >= DI_BASE + 0x28 || size != 4) return 0;
    switch (ea - DI_BASE) {
    case 0x00: /* masks are stored; interrupt bits are write-one-to-clear */
        g_disr = (g_disr & DISR_INTS & ~(w & DISR_INTS)) | (w & DISR_MASKS);
        break;
    case 0x04:
        g_dicvr = (g_dicvr & DICVR_CVRINT & ~(w & DICVR_CVRINT)) | (w & DICVR_CVRINTMASK);
        break;
    case 0x08: g_cmd[0] = w; break;
    case 0x0C: g_cmd[1] = w; break;
    case 0x10: g_cmd[2] = w; break;
    case 0x14: g_mar = w; break;
    case 0x18: g_len = w; break;
    case 0x1C:
        if (w & DICR_TSTART) execute(s);
        break;
    case 0x20: g_imm = w; break;
    case 0x24: g_cfg = w; break;
    default: break;
    }
    return 1;
}

/* True when a completed transfer has an enabled, unacknowledged interrupt. */
int di_irq_pending(void)
{
    return ((g_disr & DISR_TCINT) && (g_disr & DISR_TCINTMASK)) ||
           ((g_disr & DISR_DEINT) && (g_disr & DISR_DEINTMASK)) ||
           ((g_dicvr & DICVR_CVRINT) && (g_dicvr & DICVR_CVRINTMASK));
}

void dvd_report(void)
{
    fprintf(stderr, "[dvd] %llu reads, %llu bytes\n", (unsigned long long)g_reads,
            (unsigned long long)g_bytes);
    disc_report();
}
