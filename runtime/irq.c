/*
 * Interrupt delivery and the devices that raise them.
 *
 * Nothing in a recompiled program is asynchronous, so interrupts are
 * delivered synchronously at the one place the guest waits for them: the
 * scheduler's idle loop (hooked via config/hooks.txt). Each delivery calls
 * the handler the OS registered in its low-memory tables, with the same
 * arguments the exception path would have passed.
 *
 * Devices modelled so far: the VI retrace (the game's heartbeat) and the
 * decrementer (which drives OSAlarm).
 */
#define _CRT_SECURE_NO_WARNINGS
#include "cpu.h"
#include "plat.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OS_EXCEPTION_TABLE 0x80003000u /* 16 entries, one per PowerPC exception */
#define OS_INTERRUPT_TABLE 0x80003040u /* 32 entries, one per PI/DSP/EXI/... source */
#define EXCEPTION_TABLE_PTR 0x803478F4u /* the OS's own pointers to those tables, */
#define INTERRUPT_TABLE_PTR 0x80347930u /* read by __OSSet{Exception,Interrupt}Handler */
#define OS_CURRENT_CONTEXT 0x800000D4u
#define EXC_DECREMENTER 8 /* vector 0x900 is index 8: the SDK counts from 0x100 */
#define IRQ_PI_DI 21
#define IRQ_PI_VI 24

int di_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out);
int di_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v);
int di_irq_pending(void);
void di_poll(CpuState* s);
void aram_poll(CpuState* s);
void dvd_report(void);
int gx_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out);
int gx_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v);
unsigned gx_pe_irq_pending(void);
void gx_report(void);
#define IRQ_PI_PE_TOKEN 18
#define IRQ_PI_PE_FINISH 19
int aram_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out);
int aram_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v);
int aram_irq_pending(void);
void aram_report(void);
#define IRQ_DSP_ARAM 6
int dsp_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out);
int dsp_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v);
int dsp_irq_pending(void);
int ai_dma_irq_pending(void);
void dsp_poll(CpuState* s);
void dsp_report(void);
#define IRQ_DSP_AI 5
#define IRQ_DSP_DSP 7
int si_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out);
int si_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v);
int si_irq_pending(void);
void si_poll(void);
void si_report(void);
#define IRQ_PI_SI 20
int exi_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out);
int exi_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v);
unsigned exi_irq_pending(void);
unsigned exi_exi_irq_pending(void);
void exi_report(void);
/* Two interrupts per channel, three apart: the device's own line, then the
 * transfer-complete of the bus. __OSDispatchInterrupt tells them apart by
 * number alone -- EXIIntrruptHandler recovers its channel as (irq - 9)/3 and
 * TCIntrruptHandler as (irq - 10)/3 -- so both have to be delivered. */
#define IRQ_EXI_0_EXI 9
#define IRQ_EXI_0_TC 10
#define IRQ_EXI_1_EXI 12
#define IRQ_EXI_1_TC 13
#define IRQ_EXI_2_EXI 15
#define IRQ_EXI_2_TC 16

#define TB_HZ 40500000ull
#define VI_PERIOD_TICKS (TB_HZ / 60)

uint32_t guest_timebase_lo(CpuState* s);
uint32_t guest_timebase_hi(CpuState* s);

static uint64_t tb_now(CpuState* s)
{
    return ((uint64_t)guest_timebase_hi(s) << 32) | guest_timebase_lo(s);
}

/* ---- VI --------------------------------------------------------------- */

static uint16_t g_vi_di[4]; /* DI0..DI3 as written, without the status bit */
/* PI: the interrupt mask the OS writes, echoed back; and the cause register,
 * whose bit 16 is the reset switch, active-low -- reading 0 there tells
 * OSGetResetButtonState the button is being held. */
static uint32_t g_pi_mask, g_pi_cause;
#define PI_RSWST_RELEASED 0x10000u

/* AI: the audio interface. Its sample counter runs at 48 kHz off the timebase
 * (40.5 MHz / 843.75 = 16/13500); the SDK spins on it advancing to sync to
 * the audio clock. AICR: PSTAT, AFR, AIINTMSK, AIINT (w1c), AIINTVLD, SCRESET (pulse), DSPFR. */
static uint32_t g_ai_cr, g_ai_vr, g_ai_it;
static uint64_t g_ai_scnt_base;
static uint64_t tb_now(CpuState* s);
static uint32_t ai_read(CpuState* s, uint32_t ea)
{
    switch (ea - 0xCC006C00u) {
    case 0: return g_ai_cr;
    case 4: return g_ai_vr;
    case 8: return (uint32_t)((tb_now(s) - g_ai_scnt_base) * 16u / 13500u);
    case 12: return g_ai_it;
    default: return 0;
    }
}
static void ai_write(CpuState* s, uint32_t ea, uint32_t v)
{
    switch (ea - 0xCC006C00u) {
    case 0:
        if (v & 0x20u) g_ai_scnt_base = tb_now(s);
        g_ai_cr = (g_ai_cr & 0x8u & ~(v & 0x8u)) | (v & ~0x28u);
        break;
    case 4: g_ai_vr = v; break;
    case 12: g_ai_it = v; break;
    default: break;
    }
}
static int g_vi_pending;
static uint64_t g_vi_last;
static uint64_t g_vi_count;

int device_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out)
{
    if (size == 2 && ea >= 0xCC002030u && ea < 0xCC002040u && !(ea & 2)) {
        unsigned n = (ea - 0xCC002030u) >> 2;
        *out = g_vi_di[n] | ((n == 0 && g_vi_pending) ? 0x8000u : 0u);
        return 1;
    }
    if (size == 4 && ea == 0xCC003000u) { *out = g_pi_cause | PI_RSWST_RELEASED; return 1; }
    if (size == 4 && ea == 0xCC003004u) { *out = g_pi_mask; return 1; }
    if (size == 4 && ea >= 0xCC006C00u && ea < 0xCC006C10u) { *out = ai_read(s, ea); return 1; }
    if (exi_read(s, ea, size, out)) return 1;
    if (si_read(s, ea, size, out)) return 1;
    if (dsp_read(s, ea, size, out)) return 1;
    if (aram_read(s, ea, size, out)) return 1;
    if (gx_read(s, ea, size, out)) return 1;
    return di_read(s, ea, size, out);
}

void device_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v)
{
    if (size == 2 && ea >= 0xCC002030u && ea < 0xCC002040u && !(ea & 2)) {
        unsigned n = (ea - 0xCC002030u) >> 2;
        g_vi_di[n] = (uint16_t)(v & 0x7FFFu);
        if (n == 0 && !(v & 0x8000u)) g_vi_pending = 0; /* status bit written clear */
        return;
    }
    if (size == 4 && ea == 0xCC003000u) { g_pi_cause &= ~(uint32_t)v; return; } /* write-one-to-clear */
    if (size == 4 && ea == 0xCC003004u) { g_pi_mask = (uint32_t)v; return; }
    if (size == 4 && ea >= 0xCC006C00u && ea < 0xCC006C10u) { ai_write(s, ea, (uint32_t)v); return; }
    if (exi_write(s, ea, size, v)) return;
    if (si_write(s, ea, size, v)) return;
    if (dsp_write(s, ea, size, v)) return;
    if (aram_write(s, ea, size, v)) return;
    if (gx_write(s, ea, size, v)) return;
    di_write(s, ea, size, v);
}

/* ---- decrementer ------------------------------------------------------ */

static uint32_t g_dec_value;
static uint64_t g_dec_set;
static int g_dec_armed;
static uint64_t g_dec_count, g_dec_arms, g_di_count, g_pe_count, g_ar_count, g_dsp_count, g_aid_count, g_si_count,
    g_exi_count, g_exid_count;

void dec_write(CpuState* s, uint32_t v)
{
    g_dec_value = v;
    g_dec_set = tb_now(s);
    g_dec_armed = 1;
    g_dec_arms++;
}

uint32_t dec_read(CpuState* s)
{
    uint64_t elapsed = tb_now(s) - g_dec_set;
    return (uint32_t)((int64_t)g_dec_value - (int64_t)elapsed);
}

/* ---- delivery --------------------------------------------------------- */

/* A handler "returns from the exception" by loading the interrupted context.
 * Delivered synchronously, that means unwinding back to here -- so the
 * OSLoadContext replacement longjmps when its target is the context being
 * delivered into, whatever recompiled frames sit in between. */
static jmp_buf g_irq_jmp;
static uint32_t g_irq_ctx;
static int g_in_handler;
static uint64_t g_delivered; /* handlers run, for the poll to notice */
/* The same total, split by interrupt number, which is the only way a test can
 * tell "the card's interrupt reached slot 9" from "something ran". The
 * decrementer is delivered through the same call with exception number 8,
 * which no interrupt source in this file uses. */
static uint64_t g_delivered_by_number[32];

uint64_t irq_delivered_count(unsigned number)
{
    return number < 32 ? g_delivered_by_number[number] : 0;
}

uint32_t irq_interrupted_context(void)
{
    return g_irq_ctx;
}

int irq_in_handler(void)
{
    return g_in_handler;
}

void irq_return_from_handler(void)
{
    longjmp(g_irq_jmp, 1);
}

/* A thread switch out of a handler discards the delivery in progress. */
void irq_abandon_delivery(void)
{
    g_in_handler = 0;
    g_irq_ctx = 0;
}

static uint32_t exception_handler(CpuState* s, uint32_t exc)
{
    uint32_t table = mem_r32(s, EXCEPTION_TABLE_PTR);
    if (!table) table = OS_EXCEPTION_TABLE;
    return mem_r32(s, table + 4 * exc);
}

/* Run a handler as the exception path would: MSR[EE] clear on entry and the
 * interrupted MSR back on return, whether the handler returns or "rfi"s by
 * loading the interrupted context. Callbacks rely on that -- the audio
 * driver's frame callback enables interrupts for its work and disables them
 * again before returning to the handler. */
typedef struct {
    uint32_t gpr[32];
    Fpr fpr[32];
    uint32_t cr, xer, lr, ctr, fpscr, msr, pc;
    uint32_t gqr[8];
} RegisterFile;

static void regs_save(const CpuState* s, RegisterFile* r)
{
    memcpy(r->gpr, s->gpr, sizeof r->gpr);
    memcpy(r->fpr, s->fpr, sizeof r->fpr);
    memcpy(r->gqr, s->gqr, sizeof r->gqr);
    r->cr = s->cr; r->xer = s->xer; r->lr = s->lr; r->ctr = s->ctr;
    r->fpscr = s->fpscr; r->msr = s->msr; r->pc = s->pc;
}

static void regs_restore(CpuState* s, const RegisterFile* r)
{
    memcpy(s->gpr, r->gpr, sizeof r->gpr);
    memcpy(s->fpr, r->fpr, sizeof r->fpr);
    memcpy(s->gqr, r->gqr, sizeof r->gqr);
    s->cr = r->cr; s->xer = r->xer; s->lr = r->lr; s->ctr = r->ctr;
    s->fpscr = r->fpscr; s->msr = r->msr; s->pc = r->pc;
}

/* Run a handler as the exception path would: every register comes back as
 * it was -- the interrupted code is mid-flight with live values in volatile
 * registers, which the handler is free to use -- and MSR[EE] is clear while
 * it runs, whether it returns or "rfi"s by loading the interrupted context.
 * Callbacks rely on that too: the audio driver's frame callback enables
 * interrupts for its work and disables them again before returning. */
static void call_guest_handler(CpuState* s, uint32_t handler, uint32_t number)
{
    RegisterFile saved;
    regs_save(s, &saved);
    s->gpr[3] = number;
    s->gpr[4] = mem_r32(s, OS_CURRENT_CONTEXT);
    s->msr &= ~0x8000u;
    g_irq_ctx = s->gpr[4];
    g_in_handler++;
    g_delivered++;
    if (number < 32) g_delivered_by_number[number]++;
    if (setjmp(g_irq_jmp) == 0) dispatch(s, handler);
    g_in_handler--;
    g_irq_ctx = 0;
    regs_restore(s, &saved);
}

/* Where the OS keeps the handler for one interrupt. Exported so that a test
 * can install a handler in the very slot the delivery path reads, rather than
 * in the table it assumes is in use. */
uint32_t irq_handler_slot(CpuState* s, unsigned irq)
{
    uint32_t table = mem_r32(s, INTERRUPT_TABLE_PTR);
    if (!table) table = OS_INTERRUPT_TABLE;
    return table + 4 * irq;
}

static uint32_t interrupt_handler(CpuState* s, uint32_t irq)
{
    return mem_r32(s, irq_handler_slot(s, irq));
}

static int g_pace = -1;

/* runtime/record.c (SOA_RECORD): while a recorded call runs, interrupts wait
 * for it to return -- its recording slows it a thousandfold, and with the
 * guest's clock the host's, one would land inside nearly every call -- unless
 * it polls 4096 times, which is a call waiting on one. hle.c's. */
extern volatile int g_record_armed;
extern unsigned g_record_polls;

static void deliver_pending(CpuState* s)
{
    uint64_t now;
    uint32_t handler;

    if (g_in_handler) return; /* a handler that idles must not nest deliveries */
    if (g_record_armed && g_record_polls++ < 4096u) return;
    now = tb_now(s);
    if (g_pace < 0) {
        const char* env = getenv("SOA_PACE");
        g_pace = env && atoi(env) ? 1 : 0;
    }

    /* Decrementer: fires once each time it is armed and runs out. */
    if (g_dec_armed && now - g_dec_set >= g_dec_value) {
        g_dec_armed = 0;
        handler = exception_handler(s, EXC_DECREMENTER);
        /* Until the OS installs its alarm handler, slot 9 holds the catch-all
         * OSExceptionInit put in every slot; the hardware would have taken
         * that as an unhandled exception, which no shipping game relies on. */
        if (handler && handler != exception_handler(s, 1)) {
            g_dec_count++;
            call_guest_handler(s, handler, EXC_DECREMENTER);
        } else if (handler) {
            static int warned;
            if (!warned++) fprintf(stderr, "[irq] decrementer expired before its handler was installed\n");
        }
    }

    /* Pixel engine: a draw-done or token the command stream produced. */
    {
        unsigned due = gx_pe_irq_pending();
        if (due & 2) {
            handler = interrupt_handler(s, IRQ_PI_PE_FINISH);
            if (handler) { g_pe_count++; call_guest_handler(s, handler, IRQ_PI_PE_FINISH); }
        }
        if (due & 1) {
            handler = interrupt_handler(s, IRQ_PI_PE_TOKEN);
            if (handler) { g_pe_count++; call_guest_handler(s, handler, IRQ_PI_PE_TOKEN); }
        }
    }

    /* Devices with commands in flight: finish the ones whose time is up. */
    di_poll(s);
    aram_poll(s);

    /* DSP: a mail from the microcode; AI: a DMA block finished playing. */
    dsp_poll(s);
    if (dsp_irq_pending()) {
        handler = interrupt_handler(s, IRQ_DSP_DSP);
        if (handler) { g_dsp_count++; call_guest_handler(s, handler, IRQ_DSP_DSP); }
    }
    if (ai_dma_irq_pending()) {
        handler = interrupt_handler(s, IRQ_DSP_AI);
        if (handler) { g_aid_count++; call_guest_handler(s, handler, IRQ_DSP_AI); }
    }

    /* ARAM: a finished DMA the ARQ handler has not acknowledged. */
    if (aram_irq_pending()) {
        handler = interrupt_handler(s, IRQ_DSP_ARAM);
        if (handler) { g_ar_count++; call_guest_handler(s, handler, IRQ_DSP_ARAM); }
    }

    /* DVD: a finished transfer whose interrupt the handler has not acknowledged. */
    if (di_irq_pending()) {
        handler = interrupt_handler(s, IRQ_PI_DI);
        if (handler) {
            g_di_count++;
            call_guest_handler(s, handler, IRQ_PI_DI);
        }
    }

    /* VI retrace at 60 Hz of guest time. */
    if (now - g_vi_last >= VI_PERIOD_TICKS) {
        handler = interrupt_handler(s, IRQ_PI_VI);
        if (handler) {
            g_vi_last = now;
            g_vi_count++;
            g_vi_pending = 1;
            si_poll(); /* the controllers are polled once per field */
            call_guest_handler(s, handler, IRQ_PI_VI);
        }
    }

    /* EXI: the device's own interrupt -- a memory card saying the program or
     * erase it was left with has finished -- and a completed transfer. The
     * device's is the higher priority of the two on the console, and taking it
     * first means the handler that starts the next page finds a clean channel.
     *
     * Each handler can leave the other pending, so both are re-read after
     * every one that runs rather than once per pass. A page program goes
     * transfer-complete -> __CARDTxHandler -> EXIDeselect, and that deselect
     * is what makes the card raise its own interrupt: visiting the channel
     * once would leave the request for the next delivery, against a 100 ms
     * alarm that was armed before the program started. The round count bounds
     * the chain -- a handler that never clears its own CSR bit is a bug, not a
     * reason to sit here -- and what is left over waits for the next pass. */
    {
        static const unsigned exi[3] = {IRQ_EXI_0_EXI, IRQ_EXI_1_EXI, IRQ_EXI_2_EXI};
        static const unsigned tc[3] = {IRQ_EXI_0_TC, IRQ_EXI_1_TC, IRQ_EXI_2_TC};
        unsigned ch, round;
        for (round = 0; round < 4; round++) {
            unsigned ran = 0;
            for (ch = 0; ch < 3; ch++) {
                if ((exi_exi_irq_pending() >> ch) & 1) {
                    handler = interrupt_handler(s, exi[ch]);
                    if (handler) { g_exid_count++; call_guest_handler(s, handler, exi[ch]); ran = 1; }
                }
                if ((exi_irq_pending() >> ch) & 1) {
                    handler = interrupt_handler(s, tc[ch]);
                    if (handler) { g_exi_count++; call_guest_handler(s, handler, tc[ch]); ran = 1; }
                }
            }
            if (!ran) break;
        }
    }

    /* SI: a direct transfer completed, or polled data is waiting. */
    if (si_irq_pending()) {
        handler = interrupt_handler(s, IRQ_PI_SI);
        if (handler) { g_si_count++; call_guest_handler(s, handler, IRQ_PI_SI); }
    }
    if (g_pace) plat_yield();
}

/* SelectThread's idle loop, 0x80237BA8: `lwz RunQueueBits; beq`. Nothing is
 * runnable, interrupts are enabled, IdleContext is current. Deliver. */
void hook_80237BA8(CpuState* s)
{
    deliver_pending(s);
}

/* Every backward branch in recompiled code lands here (emit.py). A thread
 * spinning on a flag with interrupts enabled gets its interrupts, and if a
 * handler woke a thread the scheduler runs, as __OSDispatchInterrupt would
 * have made it after the handler returned. Rate-limited: the devices are
 * clocked by the timebase, which is not worth reading on every iteration. */
#define OS_RESCHEDULE 0x80237C84u
void irq_poll(CpuState* s)
{
    static unsigned n;
    uint64_t before;
    if (!(s->msr & 0x8000u) || g_in_handler) return; /* MSR[EE] clear, or nested */
    if (++n & 0xFu) return;
    before = g_delivered;
    deliver_pending(s);
    if (g_delivered != before) {
        /* The scheduler is guest code too: it clobbers volatile registers
         * the interrupted loop is still using. */
        RegisterFile saved;
        regs_save(s, &saved);
        s->msr &= ~0x8000u; /* SelectThread expects interrupts disabled */
        s->lr = 0;
        dispatch(s, OS_RESCHEDULE);
        regs_restore(s, &saved);
    }
}

void irq_report(void)
{
    fprintf(stderr,
            "[irq] %llu VI retraces, %llu DI completions, %llu PE interrupts delivered; "
            "decrementer armed %llu times, fired %llu\n",
            (unsigned long long)g_vi_count, (unsigned long long)g_di_count,
            (unsigned long long)g_pe_count, (unsigned long long)g_dec_arms,
            (unsigned long long)g_dec_count);
    dvd_report();
    gx_report();
    aram_report();
    dsp_report();
    si_report();
    exi_report();
    fprintf(stderr, "[irq] %llu SI, %llu EXI transfer-complete, %llu EXI device interrupts delivered\n",
            (unsigned long long)g_si_count, (unsigned long long)g_exi_count, (unsigned long long)g_exid_count);
    fprintf(stderr, "[irq] %llu DSP mails, %llu AI DMA blocks, %llu ARAM DMAs delivered\n",
            (unsigned long long)g_dsp_count, (unsigned long long)g_aid_count,
            (unsigned long long)g_ar_count);
}

uint64_t irq_retrace_count(void)
{
    return g_vi_count;
}
