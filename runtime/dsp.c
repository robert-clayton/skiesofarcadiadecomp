/*
 * The DSP's mailboxes, the microcode's mail protocol, and the AI DMA clock.
 *
 * The game runs Nintendo's stock AX microcode (FINDINGS section 3), whose
 * mail behaviour is documented and simple. This models that behaviour --
 * nothing is mixed yet -- so the SDK's DSP task system and Sega's AX driver
 * see the replies they wait for:
 *
 *   ROM boot   CPU sends control/parameter mail pairs (0x80F3xxxx, value);
 *              after the 0x80F3D001 pair the microcode starts and mails
 *              DSP_INIT (0xDCD10000) with the DSP interrupt.
 *   command    CPU sends 0xBABExxxx then the command-list address; the
 *              microcode processes it and mails DSP_YIELD (0xDCD10002).
 *   resume     CPU sends 0xCDD10001; the microcode acknowledges DSP_RESUME.
 *
 * The AI DMA engine streams the mixed output to the audio interface and
 * raises AIDINT when a block has played -- 640 bytes at 32 kHz is 5 ms, the
 * AX frame cadence -- which is what drives the driver's frame callback.
 *
 *   0xCC005000/02  MBOX_IN  hi/lo  (CPU -> DSP; the low half completes a mail)
 *   0xCC005004/06  MBOX_OUT hi/lo  (DSP -> CPU; bit 15 of hi = mail waiting; reading lo pops)
 *   0xCC005030/32  AI DMA start address hi/lo
 *   0xCC005036     AI DMA control: bit 15 enable, bits 0-14 length in 32-byte blocks
 *   0xCC00503A     AI DMA blocks left
 */
#define _CRT_SECURE_NO_WARNINGS
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>

#define DSP_BASE 0xCC005000u
#define TB_HZ 40500000ull

#define DSP_INIT 0xDCD10000u
#define DSP_RESUME 0xDCD10001u
#define DSP_YIELD 0xDCD10002u
#define MAIL_RESUME 0xCDD10001u
#define MAIL_NEW_UCODE 0xCDD10002u
#define MAIL_RESET 0xCDD10003u
#define MAIL_CONTINUE 0xCDD10004u

void dsp_csr_raise(uint16_t bits);
uint32_t guest_timebase_lo(CpuState* s);
uint32_t guest_timebase_hi(CpuState* s);

#define CSR_AIDINT 0x0008u
#define CSR_DSPINT 0x0080u

static uint32_t g_in_hi;
static uint32_t g_out[16];
static unsigned g_out_head, g_out_tail;
static int g_expect; /* 0 none; 1 boot parameter; 2 command-list address */
static uint32_t g_last_ctrl;
static int g_booted;
static int g_rom_after_stub;
static uint64_t g_mails_in, g_mails_out, g_cmdlists, g_unknown;

/* AI DMA */
static uint32_t g_dma_start;
static uint16_t g_dma_ctrl;
static uint64_t g_dma_due, g_dma_period, g_dma_blocks_done;
static uint64_t g_ctl_starts, g_ctl_running, g_ctl_stops; /* the control writes, by kind */
static uint32_t g_ctl_running_lr; /* LR at the first running write: 80241704, in fn_802416E4 */

uint64_t guest_time_device(CpuState* s); /* hle.c: the timebase as a device reads it */
static uint64_t tb_now(CpuState* s)
{
    return guest_time_device(s);
}

static void push_out(uint32_t mail, int irq)
{
    unsigned next = (g_out_tail + 1) % 16;
    if (next == g_out_head) {
        fprintf(stderr, "[dsp] outgoing mailbox overflow; dropping %08X\n", mail);
        return;
    }
    g_out[g_out_tail] = mail;
    g_out_tail = next;
    g_mails_out++;
    if (irq) dsp_csr_raise(CSR_DSPINT);
}

/* A DSP reset (CSR RES) restarts the ROM, which announces itself with
 * 0x8071FEED; the SDK's boot task waits for exactly that before uploading. */
void dsp_reset(void)
{
    g_out_head = g_out_tail = 0;
    g_expect = 0;
    g_booted = 0;
    g_rom_after_stub = 0;
    push_out(0x8071FEEDu, 0);
}

/* Clearing CSR DSPINIT boots the 128-byte stub __OSInitAudioSystem parked in
 * ARAM. It mails 0x80544348 and, once that is collected, falls through to
 * the ROM, which announces itself again. */
void dsp_stub_boot(void)
{
    g_out_head = g_out_tail = 0;
    g_expect = 0;
    g_booted = 0;
    push_out(0x80544348u, 0);
    g_rom_after_stub = 1;
}

void ax_command_list(CpuState* s, uint32_t addr);
void ax_report(void);
void audio_push_block(const uint8_t* be_rl, unsigned bytes, unsigned rate);
void audio_report(void);
static CpuState* g_cpu;

static void handle_mail(uint32_t mail)
{
    g_mails_in++;
    if (g_expect == 1) { /* parameter of a ROM boot control mail */
        g_expect = 0;
        if (g_last_ctrl == 0x80F3D001u && !g_booted) {
            g_booted = 1;
            fprintf(stderr, "[dsp] microcode booted; start vector %04X\n", mail & 0xFFFFu);
            push_out(DSP_INIT, 1);
        }
        return;
    }
    if (g_expect == 2) { /* command-list address: the microcode's frame of work */
        g_expect = 0;
        g_cmdlists++;
        {
            static int noax = -1;
            if (noax < 0) noax = getenv("SOA_NOAX") ? 1 : 0; /* experiment: no mixing, no RAM writes */
            if (g_cpu && !noax) ax_command_list(g_cpu, mail);
        }
        push_out(DSP_YIELD, 1);
        return;
    }
    if ((mail & 0xFFFF0000u) == 0x80F30000u) {
        g_last_ctrl = mail;
        g_expect = 1;
        return;
    }
    if ((mail & 0xFFFF0000u) == 0xBABE0000u) {
        g_expect = 2;
        return;
    }
    switch (mail) {
    case MAIL_RESUME: push_out(DSP_RESUME, 1); break;
    case MAIL_NEW_UCODE: g_booted = 0; break;
    case MAIL_RESET: g_booted = 0; break;
    case MAIL_CONTINUE: break;
    default:
        if (g_unknown++ < 8) fprintf(stderr, "[dsp] unknown mail from CPU: %08X\n", mail);
        break;
    }
}

int dsp_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out)
{
    (void)s;
    if (ea < DSP_BASE || ea >= DSP_BASE + 0x40 || size != 2) return 0;
    switch (ea - DSP_BASE) {
    case 0x00: *out = 0; return 1; /* the DSP has always taken the last mail */
    case 0x02: *out = 0; return 1;
    case 0x04:
        *out = (g_out_head != g_out_tail) ? (0x8000u | (g_out[g_out_head] >> 16)) : 0u;
        return 1;
    case 0x06:
        if (g_out_head != g_out_tail) {
            *out = g_out[g_out_head] & 0xFFFFu;
            g_out_head = (g_out_head + 1) % 16;
            if (g_out_head == g_out_tail && g_rom_after_stub) {
                g_rom_after_stub = 0;
                push_out(0x8071FEEDu, 0);
            }
        } else {
            *out = 0;
        }
        return 1;
    case 0x30: *out = g_dma_start >> 16; return 1;
    case 0x32: *out = g_dma_start & 0xFFFFu; return 1;
    case 0x36: *out = g_dma_ctrl; return 1;
    case 0x3A: { /* blocks left in the block being played */
        uint64_t now = tb_now(s);
        uint32_t blocks = g_dma_ctrl & 0x7FFFu;
        if (!(g_dma_ctrl & 0x8000u) || !g_dma_period || now >= g_dma_due) { *out = 0; return 1; }
        *out = (uint32_t)((g_dma_due - now) * blocks / g_dma_period);
        return 1;
    }
    default: return 0;
    }
}

int dsp_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v)
{
    if (ea < DSP_BASE || ea >= DSP_BASE + 0x40 || size != 2) return 0;
    switch (ea - DSP_BASE) {
    case 0x00: g_in_hi = (uint32_t)v & 0xFFFFu; return 1;
    case 0x02: g_cpu = s; handle_mail((g_in_hi << 16) | ((uint32_t)v & 0xFFFFu)); return 1;
    case 0x30: g_dma_start = (g_dma_start & 0xFFFFu) | (((uint32_t)v & 0xFFFFu) << 16); return 1;
    case 0x32: g_dma_start = (g_dma_start & 0xFFFF0000u) | ((uint32_t)v & 0xFFFFu); return 1;
    case 0x36: {
        uint32_t blocks = (uint32_t)v & 0x7FFFu;
        int was_on = (g_dma_ctrl & 0x8000u) != 0;
        g_dma_ctrl = (uint16_t)v;
        if (v & 0x8000u) {
            /* 32 bytes per block; 128 bytes per millisecond of 16-bit stereo at 32 kHz. */
            g_dma_period = (uint64_t)blocks * 32u * TB_HZ / 128000u;
            if (!g_dma_period) g_dma_period = 1;
            /* Starting, the first block plays from now. Running, a write
             * only sets the next block: the one playing ends when it ends.
             * The game makes one every block, from fn_802416E4 (AIInitDMA's
             * shape: it keeps bit 15 and sets the length), so taking each as
             * a restart put every block a delivery late and the audio 5%
             * slow -- 121,475 bytes a second where 128,000 are due, the
             * device starved (FINDINGS "The AI DMA's pace"). */
            if (!was_on) {
                g_dma_due = tb_now(s) + g_dma_period;
                g_ctl_starts++;
            } else if (!g_ctl_running++) {
                g_ctl_running_lr = s->lr;
            }
        } else if (was_on) {
            g_ctl_stops++;
        }
        return 1;
    }
    default: return 0;
    }
}

/* Called from the delivery point: a block that has finished playing raises
 * AIDINT, and the engine keeps looping the block until it is disabled. A
 * deadline behind is caught up a block a poll, across a clock epoch too: the
 * clock does not count a gap (M19), so an epoch brings no backlog of its own,
 * and restarting the deadline there only threw away blocks owed from before
 * it (FINDINGS "M19, followed up"). */
void dsp_poll(CpuState* s)
{
    if ((g_dma_ctrl & 0x8000u) && g_dma_period && tb_now(s) >= g_dma_due) {
        uint32_t bytes = (g_dma_ctrl & 0x7FFFu) * 32u;
        g_dma_due += g_dma_period;
        g_dma_blocks_done++;
        /* the block the DAC just played */
        if (bytes && (g_dma_start & MEM_MASK) + bytes <= MEM1_SIZE)
            audio_push_block(mem_ptr(s, g_dma_start | 0x80000000u), bytes, 32000);
        dsp_csr_raise(CSR_AIDINT);
    }
}

/* The fixed-step clock's idle skip (irq.c): when the AI DMA's block ends. */
int dsp_next_due(uint64_t* due)
{
    if (!(g_dma_ctrl & 0x8000u) || !g_dma_period) return 0;
    *due = g_dma_due;
    return 1;
}

/* The self test's view of the DMA: blocks played, and a deadline pushed
 * `periods` into the past, as a host stall would leave it. */
uint64_t dsp_dma_blocks(void)
{
    return g_dma_blocks_done;
}

void dsp_dma_backdate(unsigned periods)
{
    g_dma_due -= (uint64_t)periods * g_dma_period;
}

void dsp_report(void)
{
    ax_report();
    audio_report();
    fprintf(stderr,
            "[dsp] booted=%d; %llu mails in, %llu out, %llu command lists; %llu AI DMA blocks played\n",
            g_booted, (unsigned long long)g_mails_in, (unsigned long long)g_mails_out,
            (unsigned long long)g_cmdlists, (unsigned long long)g_dma_blocks_done);
    fprintf(stderr, "[dsp] AI DMA control writes: %llu start(s), %llu while running (the first with LR %08X), %llu stop(s)\n",
            (unsigned long long)g_ctl_starts, (unsigned long long)g_ctl_running, g_ctl_running_lr,
            (unsigned long long)g_ctl_stops);
}

/* ---- a snapshot (snapshot.h; docs/SNAPSHOT.md) ------------------------------
 * The mailboxes and the microcode protocol's place, and AI's DMA: the block
 * playing and the tick it is done at, which the fixed clock's idle skip goes
 * to. The sound itself is in guest memory and ARAM. */
#include "snapshot.h"
void dsp_snap(Snap* sn)
{
    SNAP(sn, g_in_hi);
    SNAP(sn, g_out);
    SNAP(sn, g_out_head);
    SNAP(sn, g_out_tail);
    SNAP(sn, g_expect);
    SNAP(sn, g_last_ctrl);
    SNAP(sn, g_booted);
    SNAP(sn, g_rom_after_stub);
    SNAP(sn, g_mails_in);
    SNAP(sn, g_mails_out);
    SNAP(sn, g_cmdlists);
    SNAP(sn, g_unknown);
    SNAP(sn, g_dma_start);
    SNAP(sn, g_dma_ctrl);
    SNAP(sn, g_dma_due);
    SNAP(sn, g_dma_period);
    SNAP(sn, g_dma_blocks_done);
    SNAP(sn, g_ctl_starts);
    SNAP(sn, g_ctl_running);
    SNAP(sn, g_ctl_stops);
    SNAP(sn, g_ctl_running_lr);
}
