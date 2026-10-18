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
 *
 * Native code adds two more sources (docs/CLOCK.md): events scheduled at a
 * guest tick, run at the first delivery point at or after it as a handler
 * would be (irq_event_add; soa_host_schedule), and interrupt lines it marks
 * pending, which call the game's own handler for that interrupt number at the
 * next delivery point (irq_raise; soa_host_raise). Under the fixed-step clock
 * (SOA_CLOCK=fixed) a delivery point at a loop's back edge moves guest time a
 * step, and the idle loop with nothing to deliver moves it to the next
 * deadline: the retrace, the decrementer, a device's, an event's.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "cpu.h"
#include "clock.h"
#include "irq.h"
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
int di_next_due(uint64_t* due);
int aram_next_due(uint64_t* due);
int dsp_next_due(uint64_t* due);
void dvd_report(void);
int gx_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out);
int gx_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v);
unsigned gx_pe_irq_pending(void);
void gx_report(void);
uint64_t gx_pipe_bytes(void); /* the bytes sent down the gather pipe so far */
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

uint64_t guest_time_device(CpuState* s); /* hle.c: the timebase as a device reads it */
static uint64_t tb_now(CpuState* s)
{
    return guest_time_device(s);
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
    case 8: g_clock_wait_reads |= CLOCK_READ_AISCNT; return (uint32_t)((tb_now(s) - g_ai_scnt_base) * 16u / 13500u);
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
static uint64_t g_dec_repeats; /* decrementer exceptions moved on 8 ticks: a second at one instant (quiet rule) */
static uint64_t g_dec_last = ~0ull; /* the tick the last decrementer exception ran at (quiet rule) */
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
    g_clock_wait_reads |= CLOCK_READ_TB;
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

/* ---- native sources: events at a guest tick, and interrupt lines -------
 *
 * Both are set from any thread and run on the guest's, inside a delivery,
 * as a handler is: every register put back after, MSR[EE] clear while it
 * runs, no delivery nested in it, and the scheduler run after it if it woke a
 * thread (irq_poll). An event is due at a tick of guest time and runs at the
 * first delivery point at or after it; a periodic one is then due `period`
 * ticks after the tick it was due at, not after when it ran, so a 5 ms
 * sound tick does not drift. Lateness (the tick it ran at less the tick it
 * was due at) is kept for the report. A line is a number in the OS's
 * interrupt table (0-31): at the next delivery point the game's handler in
 * that slot is called with it, as the device's own interrupt would. */
typedef struct {
    int id; /* 0: a free slot */
    uint64_t due, period;
    IrqEventFn fn;
    void* user;
    void (*extra)(void);
} IrqEvent;
static IrqEvent g_events[IRQ_EVENTS];
static PlatLock g_event_lock;
static int g_event_next_id;
static volatile int g_nevents; /* pending, so a delivery with none costs one test */
static volatile uint32_t g_lines; /* raised and not yet delivered, a bit a number */
static uint64_t g_event_runs, g_event_late_max, g_event_late_sum, g_line_runs, g_line_unhandled;

int irq_event_add(uint64_t due, uint64_t period, IrqEventFn fn, void* user, void (*extra)(void))
{
    int k, id = 0;
    if (!fn) return 0;
    plat_lock(&g_event_lock);
    for (k = 0; k < IRQ_EVENTS; k++)
        if (!g_events[k].id) {
            if (++g_event_next_id <= 0) g_event_next_id = 1;
            id = g_event_next_id;
            g_events[k].id = id;
            g_events[k].due = due;
            g_events[k].period = period;
            g_events[k].fn = fn;
            g_events[k].user = user;
            g_events[k].extra = extra;
            g_nevents++;
            break;
        }
    plat_unlock(&g_event_lock);
    if (id) clock_fixed_mark(); /* the quiet rule's next delivery point looks */
    return id;
}

int irq_event_cancel(int id)
{
    int k, found = 0;
    if (id <= 0) return 0;
    plat_lock(&g_event_lock);
    for (k = 0; k < IRQ_EVENTS; k++)
        if (g_events[k].id == id) g_events[k].id = 0, found = 1, g_nevents--;
    plat_unlock(&g_event_lock);
    return found;
}

int irq_raise(unsigned number)
{
    uint32_t was;
    if (number >= 32) return 0;
    plat_lock(&g_event_lock);
    was = g_lines;
    g_lines = was | (1u << number);
    plat_unlock(&g_event_lock);
    clock_fixed_mark();
    return 1;
}

uint64_t irq_guest_ticks(CpuState* s)
{
    return tb_now(s);
}

/* The earliest event's tick in *due; 0 when there is none. */
static int event_next_due(uint64_t* due)
{
    int k, any = 0;
    if (!g_nevents) return 0;
    plat_lock(&g_event_lock);
    for (k = 0; k < IRQ_EVENTS; k++)
        if (g_events[k].id && (!any || g_events[k].due < *due)) *due = g_events[k].due, any = 1;
    plat_unlock(&g_event_lock);
    return any;
}

/* A native event, run as call_guest_handler runs the game's. */
static void call_native(CpuState* s, const IrqEvent* e, uint64_t now)
{
    RegisterFile saved;
    regs_save(s, &saved);
    s->msr &= ~0x8000u;
    g_in_handler++;
    g_delivered++;
    e->fn(s, e->user, e->extra, e->due, now);
    g_in_handler--;
    regs_restore(s, &saved);
}

/* Every event due by `now`, earliest first; a periodic one that is several
 * periods behind runs once a period, up to 64 times a pass. */
static void run_events(CpuState* s, uint64_t now)
{
    unsigned runs;
    if (!g_nevents) return;
    for (runs = 0; runs < 64; runs++) {
        int k, best = -1;
        IrqEvent e;
        plat_lock(&g_event_lock);
        for (k = 0; k < IRQ_EVENTS; k++)
            if (g_events[k].id && g_events[k].due <= now && (best < 0 || g_events[k].due < g_events[best].due)) best = k;
        if (best >= 0) {
            e = g_events[best];
            if (e.period) g_events[best].due += e.period;
            else g_events[best].id = 0, g_nevents--;
        }
        plat_unlock(&g_event_lock);
        if (best < 0) return;
        g_event_runs++;
        if (now - e.due > g_event_late_max) g_event_late_max = now - e.due;
        g_event_late_sum += now - e.due;
        call_native(s, &e, now);
    }
}

/* runtime/record.c (SOA_RECORD): while a recorded call runs, interrupts wait
 * for it to return -- its recording slows it a thousandfold, and with the
 * guest's clock the host's, one would land inside nearly every call -- unless
 * it polls 4096 times, which is a call waiting on one. hle.c's. */
extern volatile int g_record_armed;
extern unsigned g_record_polls;

/* A line native code raised: its handler, once (deliver_pending). */
static void line_run(CpuState* s, unsigned n)
{
    uint32_t handler = interrupt_handler(s, n);
    if (handler) { g_line_runs++; call_guest_handler(s, handler, n); }
    else g_line_unhandled++;
}
#define LINE(n) (1u << (n))
#define LINES_AT_DEVICES (LINE(IRQ_PI_PE_FINISH) | LINE(IRQ_PI_PE_TOKEN) | LINE(IRQ_DSP_DSP) | LINE(IRQ_DSP_AI) | \
                          LINE(IRQ_DSP_ARAM) | LINE(IRQ_PI_DI) | LINE(IRQ_PI_VI) | LINE(IRQ_EXI_0_EXI) | LINE(IRQ_EXI_0_TC) | \
                          LINE(IRQ_EXI_1_EXI) | LINE(IRQ_EXI_1_TC) | LINE(IRQ_EXI_2_EXI) | LINE(IRQ_EXI_2_TC) | LINE(IRQ_PI_SI))
/* The device's own interrupt, or the line raised for it, or both: the
 * handler runs once, as the OS's dispatcher runs it once for a cause. */
#define TAKE(n) ((lines & LINE(n)) ? (lines &= ~LINE(n), 1) : 0)

static void deliver_pending(CpuState* s)
{
    uint64_t now;
    uint32_t handler, lines = 0;

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
            /* Reads cost nothing (the quiet rule's default), so an alarm the
             * OS sets for the instant it fired -- a periodic alarm started at
             * OSGetTime(), whose fire time is its start until time has moved
             * past it -- would fire again at the same tick for ever. A second
             * decrementer exception at one instant first moves the clock as
             * far as one read used to (8 ticks), which is all the OS needs to
             * see time pass. */
            if (g_clock_quiet_off && clock_fixed_now() == g_dec_last) {
                clock_fixed_skip_to(g_dec_last + 8);
                g_dec_repeats++;
            }
            call_guest_handler(s, handler, EXC_DECREMENTER);
            if (g_clock_quiet_off) g_dec_last = clock_fixed_now();
        } else if (handler) {
            static int warned;
            if (!warned++) fprintf(stderr, "[irq] decrementer expired before its handler was installed\n");
        }
    }

    /* Native events due by now, then the lines native code raised. */
    run_events(s, now);
    /* A raised line is delivered where its device's interrupt is, in the
     * same pass and the same order (PE, DSP, AI, ARAM, DI, VI, EXI, SI), so
     * that a native device whose interrupt falls on the tick of another one
     * -- a 5 ms AI block on a retrace, which the fixed clock makes common --
     * runs its handler before or after the other exactly as the device model
     * would. A number no device has (none the OS uses) goes first. */
    if (g_lines) {
        unsigned n;
        plat_lock(&g_event_lock);
        lines = g_lines;
        g_lines = 0;
        plat_unlock(&g_event_lock);
        for (n = 0; n < 32; n++)
            if (((lines & ~LINES_AT_DEVICES) >> n) & 1u) line_run(s, n);
        lines &= LINES_AT_DEVICES;
    }

    /* Pixel engine: a draw-done or token the command stream produced. */
    {
        unsigned due = gx_pe_irq_pending();
        int ln = TAKE(IRQ_PI_PE_FINISH);
        if (due & 2) {
            handler = interrupt_handler(s, IRQ_PI_PE_FINISH);
            if (handler) { g_pe_count++; call_guest_handler(s, handler, IRQ_PI_PE_FINISH); }
        } else if (ln) line_run(s, IRQ_PI_PE_FINISH);
        ln = TAKE(IRQ_PI_PE_TOKEN);
        if (due & 1) {
            handler = interrupt_handler(s, IRQ_PI_PE_TOKEN);
            if (handler) { g_pe_count++; call_guest_handler(s, handler, IRQ_PI_PE_TOKEN); }
        } else if (ln) line_run(s, IRQ_PI_PE_TOKEN);
    }

    /* Devices with commands in flight: finish the ones whose time is up. */
    di_poll(s);
    aram_poll(s);

    /* DSP: a mail from the microcode; AI: a DMA block finished playing. */
    dsp_poll(s);
    if (dsp_irq_pending()) {
        TAKE(IRQ_DSP_DSP);
        handler = interrupt_handler(s, IRQ_DSP_DSP);
        if (handler) { g_dsp_count++; call_guest_handler(s, handler, IRQ_DSP_DSP); }
    } else if (TAKE(IRQ_DSP_DSP)) line_run(s, IRQ_DSP_DSP);
    if (ai_dma_irq_pending()) {
        TAKE(IRQ_DSP_AI);
        handler = interrupt_handler(s, IRQ_DSP_AI);
        if (handler) { g_aid_count++; call_guest_handler(s, handler, IRQ_DSP_AI); }
    } else if (TAKE(IRQ_DSP_AI)) line_run(s, IRQ_DSP_AI);

    /* ARAM: a finished DMA the ARQ handler has not acknowledged. */
    if (aram_irq_pending()) {
        TAKE(IRQ_DSP_ARAM);
        handler = interrupt_handler(s, IRQ_DSP_ARAM);
        if (handler) { g_ar_count++; call_guest_handler(s, handler, IRQ_DSP_ARAM); }
    } else if (TAKE(IRQ_DSP_ARAM)) line_run(s, IRQ_DSP_ARAM);

    /* DVD: a finished transfer whose interrupt the handler has not acknowledged. */
    if (di_irq_pending()) {
        TAKE(IRQ_PI_DI);
        handler = interrupt_handler(s, IRQ_PI_DI);
        if (handler) {
            g_di_count++;
            call_guest_handler(s, handler, IRQ_PI_DI);
        }
    } else if (TAKE(IRQ_PI_DI)) line_run(s, IRQ_PI_DI);

    /* VI retrace at 60 Hz of guest time. */
    if (now - g_vi_last >= VI_PERIOD_TICKS && (handler = interrupt_handler(s, IRQ_PI_VI)) != 0) {
        TAKE(IRQ_PI_VI);
        g_vi_last = now;
        g_vi_count++;
        g_vi_pending = 1;
        si_poll(); /* the controllers are polled once per field */
        call_guest_handler(s, handler, IRQ_PI_VI);
    } else if (TAKE(IRQ_PI_VI)) line_run(s, IRQ_PI_VI);

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
                    TAKE(exi[ch]);
                    handler = interrupt_handler(s, exi[ch]);
                    if (handler) { g_exid_count++; call_guest_handler(s, handler, exi[ch]); ran = 1; }
                } else if (TAKE(exi[ch])) line_run(s, exi[ch]), ran = 1;
                if ((exi_irq_pending() >> ch) & 1) {
                    TAKE(tc[ch]);
                    handler = interrupt_handler(s, tc[ch]);
                    if (handler) { g_exi_count++; call_guest_handler(s, handler, tc[ch]); ran = 1; }
                } else if (TAKE(tc[ch])) line_run(s, tc[ch]), ran = 1;
            }
            if (!ran) break;
        }
    }

    /* SI: a direct transfer completed, or polled data is waiting. */
    if (si_irq_pending()) {
        TAKE(IRQ_PI_SI);
        handler = interrupt_handler(s, IRQ_PI_SI);
        if (handler) { g_si_count++; call_guest_handler(s, handler, IRQ_PI_SI); }
    } else if (TAKE(IRQ_PI_SI)) line_run(s, IRQ_PI_SI);
    if (g_pace) plat_yield();
}

/* The fixed-step clock with nothing runnable and nothing delivered: guest
 * time goes to the next deadline ahead of it -- the retrace, the
 * decrementer, a disc, ARAM or AI DMA completion, a native event -- which
 * the next pass of the idle loop delivers. With none ahead (the VI handler
 * not yet installed, its deadline behind) a field's worth. */
static uint64_t next_deadline(uint64_t now)
{
    uint64_t best = 0, t;
    int any = 0;
#define DUE(t) do { if ((t) > now && (!any || (t) < best)) best = (t), any = 1; } while (0)
    DUE(g_vi_last + VI_PERIOD_TICKS);
    if (g_dec_armed) DUE(g_dec_set + g_dec_value);
    if (di_next_due(&t)) DUE(t);
    if (aram_next_due(&t)) DUE(t);
    if (dsp_next_due(&t)) DUE(t);
    if (event_next_due(&t)) DUE(t);
#undef DUE
    return any ? best : now + VI_PERIOD_TICKS;
}

static void fixed_idle_skip(CpuState* s)
{
    clock_fixed_skip_to(next_deadline(tb_now(s)));
}

/* A wait found by its state (below): to the next deadline, as the idle loop,
 * but no further than the loop can see a change in what it read
 * (clock.h's CLOCK_READ_*): AI's sample counter's next step; or, for the
 * timebase, a step that starts at 8 ticks and doubles each time the same
 * loop is found waiting again, so an elapsed-time loop ends late by less
 * than it waited. */
typedef struct {
    uint32_t pc;   /* the loop the step is for; 0: none */
    uint64_t step; /* the next step for a wait on the timebase there */
} WaitStep;
/* One for each table of waits (below): a handler's wait with interrupts off
 * does not move the step of the loop it interrupted, and a stretch with
 * interrupts off that ends (an elapsed-time loop left) starts the next one's
 * at 8 ticks again. */
static WaitStep g_step_on, g_step_off;
static unsigned long long g_wait_tb_steps, g_wait_ai_steps, g_quiet_off_waits, g_quiet_passes_differ;
static void fixed_wait_skip(CpuState* s, unsigned reads, WaitStep* w)
{
    uint64_t now = tb_now(s), best = next_deadline(now);
    if (reads & CLOCK_READ_AISCNT) {
        uint64_t c = (now - g_ai_scnt_base) * 16u / 13500u, next = g_ai_scnt_base + ((c + 1) * 13500u + 15u) / 16u;
        if (next > now && next < best) best = next, g_wait_ai_steps++;
    }
    if (reads & CLOCK_READ_TB) {
        w->step = s->pc == w->pc && w->step && w->step < (1ull << 40) ? w->step * 2 : 8;
        w->pc = s->pc;
        if (now + w->step < best) best = now + w->step, g_wait_tb_steps++;
    } else {
        w->pc = 0;
    }
    clock_fixed_skip_to(best);
}

/* SelectThread's idle loop, 0x80237BA8: `lwz RunQueueBits; beq`. Nothing is
 * runnable, interrupts are enabled, IdleContext is current. Deliver. */
void hook_80237BA8(CpuState* s)
{
    uint64_t before = g_delivered;
    deliver_pending(s);
    if (clock_fixed() && g_delivered == before && !g_in_handler) fixed_idle_skip(s);
}

/* Every backward branch in recompiled code lands here (emit.py). A thread
 * spinning on a flag with interrupts enabled gets its interrupts, and if a
 * handler woke a thread the scheduler runs, as __OSDispatchInterrupt would
 * have made it after the handler returned. Rate-limited: the devices are
 * clocked by the timebase, which is not worth reading on every iteration. */
#define OS_RESCHEDULE 0x80237C84u
/* Deliver, and run the scheduler if a handler woke a thread; 1 if anything
 * was delivered. */
static int deliver_and_reschedule(CpuState* s)
{
    uint64_t before = g_delivered;
    deliver_pending(s);
    if (g_delivered == before) return 0;
    {
        /* The scheduler is guest code too: it clobbers volatile registers
         * the interrupted loop is still using. */
        RegisterFile saved;
        regs_save(s, &saved);
        s->msr &= ~0x8000u; /* SelectThread expects interrupts disabled */
        s->lr = 0;
        dispatch(s, OS_RESCHEDULE);
        regs_restore(s, &saved);
    }
    return 1;
}

/* The fixed clock's quiet rule (clock.h; docs/CLOCK.md "Native answers and
 * the clock"): no back edge costs guest time, and none is a delivery point by
 * count. The first back edge with interrupts enabled after something may have
 * come due delivers -- the same program point whether a native answer or a
 * translation ran before it, since an answer that reads nothing moves
 * nothing.
 *
 * A loop that waits on what only an interrupt can change has to be found, or
 * it spins forever with the clock stopped. It is found by its state: each
 * back edge notes the integer registers, CR, LR, CTR and XER under its block's
 * address (a small table, emptied whenever something comes due), and a back
 * edge that finds them as its block's last one left them, with nothing sent
 * down the gather pipe since, has gone round a loop that changed none of
 * them; once that happens twice running with the floating registers the
 * same too, the loop is a wait. (The pipe counts because a function called
 * twice with the same registers to draw the same thing twice would look
 * like a loop that waits.) Work does not repeat
 * its registers (a pointer, a count, a value read moves), so it is never cut,
 * however long it runs. And where a wait is found depends only on the loop
 * and on what came due before it -- not on how much other code ran since,
 * so a native answer before the wait does not move it. A wait that keeps a
 * count in a register is found by length alone, at `spin` back edges with
 * nothing due (SOA_CLOCK_SPIN). Either way what is pending is delivered, and
 * with nothing pending guest time goes to the next deadline, as the idle
 * loop's does. */
#define QUIET_SLOTS 256u

typedef struct {
    uint32_t pc, seen; /* seen: 0 empty, 1 noted, 2 noted twice the same, with the floats */
    uint64_t stores;   /* gx_pipe_bytes() when noted: a loop that drew something is not a wait */
    uint64_t mark;     /* the table's running sum just after this back edge last came by */
    uint64_t pass;     /* the sum over the other back edges of the last pass (seen == 2) */
    uint64_t answers;  /* swap.c's count of native answers when this back edge last came by */
    uint64_t memory;   /* the guest's memory, hashed, when it last came by after a pass with an answer in it */
    uint32_t memory_ok; /* 1: `memory` is that pass's */
    uint32_t gpr[32], cr, lr, ctr, xer;
    Fpr fpr[32];
} QuietSlot;

/* A back edge's state, mixed to 64 bits. Each table keeps a running sum of
 * these over every back edge it sees; the sum since a block's last pass is
 * what the other back edges did in between. A loop that waits does the same
 * every pass, inner loops and all, so that sum repeats too. The same block
 * reached again with the same registers by a loop *around* it whose count is
 * in memory -- a function called five times running by a caller that keeps
 * its counter in a register the callee saves on the stack -- is not a wait:
 * the caller's back edge, between the two, is a different state each time.
 * (The sound library's frame, 0x80283478, does exactly that in an AI
 * interrupt; taken for a wait, it moved the clock on in every AI block.) */
static uint64_t quiet_state_hash(const CpuState* s)
{
    uint64_t h = (uint64_t)s->pc * 0x9E3779B97F4A7C15ull + s->cr + ((uint64_t)s->lr << 32) + s->ctr * 0xC2B2AE3D27D4EB4Full +
                 ((uint64_t)s->xer << 17);
    unsigned i;
    for (i = 0; i < 32; i++) h += ((uint64_t)s->gpr[i] + 1u) * (0xD6E8FEB86659FD93ull + 2u * i);
    h ^= h >> 33, h *= 0xFF51AFD7ED558CCDull, h ^= h >> 33, h *= 0xC4CEB9FE1A85EC53ull, h ^= h >> 33;
    return h;
}

/* Native answers (docs/SWAP.md) are not loops of the guest's, and leave the
 * registers cleaner than their translations do: an answer puts back what the
 * calling convention lets a function clobber (r0, r4-r12, CTR, CR...), where
 * the translation leaves cursors, masks and operands that change with what it
 * did. So a loop around an answered call -- the script runner's
 * `while (...) dispatch(&pc, 1)` at 0x80212974 -- can come back with the
 * registers it left last pass although the answer did work: the work is in
 * memory, where the state above does not look. A pass with an answer in it
 * is therefore a repeat only if the guest's memory is as the last pass left
 * it too (hashed, MEM1, at the back edge), and only then can it make a wait:
 * a poll around an answer that only reads (a status, a count) is still a
 * wait, on the same pass as its translation's would be; a loop whose answer
 * wrote anything is not. Passes with no answer in them are unchanged, so a
 * run with no answers set hashes nothing and finds every wait as before.
 * swap.c counts the answers; quiet.c hands irq.c the count. */
static const volatile uint64_t* g_answers;
static unsigned long long g_quiet_answer_passes, g_quiet_answer_work;

void irq_quiet_answers(const volatile uint64_t* count)
{
    g_answers = count;
}

static uint64_t quiet_memory_hash(const CpuState* s)
{
    const uint8_t* p = s->mem;
    uint64_t a = 0x243F6A8885A308D3ull, b = 0x13198A2E03707344ull, c = 0xA4093822299F31D0ull, d = 0x082EFA98EC4E6C89ull;
    size_t i;
    for (i = 0; i + 32u <= MEM1_SIZE; i += 32u) {
        uint64_t w[4];
        memcpy(w, p + i, sizeof w);
        a = (a ^ w[0]) * 0x9E3779B97F4A7C15ull;
        b = (b ^ w[1]) * 0xC2B2AE3D27D4EB4Full;
        c = (c ^ w[2]) * 0x165667B19E3779F9ull;
        d = (d ^ w[3]) * 0xD6E8FEB86659FD93ull;
        a ^= a >> 29, b ^= b >> 31, c ^= c >> 27, d ^= d >> 33;
    }
    return a ^ (b * 3u) ^ (c * 5u) ^ (d * 7u);
}

/* One back edge against its slot: 0 noted (the state is new), 1 the same
 * state again, 2 a wait (the same state a third time running, with the same
 * floats and the same pass in between as the last time, and, if an answer
 * ran in that pass, the same memory). */
static int quiet_match(QuietSlot* q, const CpuState* s, uint64_t* sum, int fresh)
{
    uint64_t f = quiet_state_hash(s), before = *sum, answers = g_answers ? *g_answers : 0;
    *sum += f;
    if (!fresh && q->seen && q->pc == s->pc && q->stores == gx_pipe_bytes() && q->cr == s->cr && q->lr == s->lr &&
        q->ctr == s->ctr && q->xer == s->xer && !memcmp(q->gpr, s->gpr, sizeof q->gpr)) {
        uint64_t pass = before - q->mark;
        int changed = 0;
        q->mark = *sum;
        if (answers != q->answers) { /* an answer ran in the pass: did it leave memory as it found it? */
            uint64_t m = quiet_memory_hash(s);
            g_quiet_answer_passes++;
            changed = !q->memory_ok || m != q->memory;
            q->answers = answers, q->memory = m, q->memory_ok = 1;
        } else {
            q->memory_ok = 0;
        }
        if (q->seen == 2 && pass == q->pass && !memcmp(q->fpr, s->fpr, sizeof q->fpr)) {
            if (!changed) return 2;
            g_quiet_answer_work++;
        }
        if (q->seen == 2 && pass != q->pass) g_quiet_passes_differ++;
        q->seen = 2;
        q->pass = pass;
        memcpy(q->fpr, s->fpr, sizeof q->fpr);
        return 1;
    }
    q->pc = s->pc, q->seen = 1, q->stores = gx_pipe_bytes(), q->mark = *sum;
    q->answers = answers, q->memory_ok = 0;
    q->cr = s->cr, q->lr = s->lr, q->ctr = s->ctr, q->xer = s->xer;
    memcpy(q->gpr, s->gpr, sizeof q->gpr);
    return 0;
}

static QuietSlot g_quiet[QUIET_SLOTS];
static unsigned long long g_quiet_run;
static uint64_t g_quiet_sum;

static void quiet_forget(void)
{
    unsigned k;
    g_clock_wait_reads = 0;
    for (k = 0; k < QUIET_SLOTS; k++) g_quiet[k].seen = 0;
    g_quiet_run = 0;
}

static void quiet_wait(CpuState* s, int by_length)
{
    unsigned reads = g_clock_wait_reads;
    quiet_forget();
    if (deliver_and_reschedule(s)) {
        clock_fixed_note_spin(0, by_length ? 3 : 1);
        return;
    }
    clock_fixed_note_spin(0, by_length ? 4 : 2);
    fixed_wait_skip(s, reads, &g_step_on);
    clock_fixed_take_due();
    deliver_and_reschedule(s);
}

/* Something came due: deliver it here. A back edge with interrupts enabled
 * calls it, and so does the entry of every translated function (swap.c,
 * through irq_quiet_entry), so that the first of those after a read or a
 * write is where the handler runs: a native answer's entry is its
 * translation's entry, and a delivery that falls before a call lands in the
 * same place whichever of the two runs. */
static void quiet_due(CpuState* s)
{
    clock_fixed_take_due();
    clock_fixed_note_spin(g_quiet_run, 0);
    quiet_forget();
    deliver_and_reschedule(s);
}

void irq_quiet_entry(CpuState* s)
{
    if (!(s->msr & 0x8000u) || g_in_handler || !g_clock_due) return; /* interrupts off, or nested: later */
    quiet_due(s);
}

static void quiet_poll(CpuState* s)
{
    QuietSlot* q;
    if (g_clock_due) {
        quiet_due(s);
        return;
    }
    q = &g_quiet[(s->pc >> 2) & (QUIET_SLOTS - 1u)];
    if (quiet_match(q, s, &g_quiet_sum, 0) == 2) {
        quiet_wait(s, 0);
        return;
    }
    if (++g_quiet_run >= clock_fixed_spin()) quiet_wait(s, 1);
}

/* The same, with interrupts off (MSR[EE] clear, or inside a handler or an
 * event): nothing can be delivered here, so a loop whose state repeats is
 * waiting on a device's status or on the timebase -- ARAM's busy bit at boot,
 * an mftb loop -- and only time can end it. Reads cost no time (the quiet
 * rule's default, SOA_CLOCK_READ_TICKS=0), so the clock goes to the next
 * deadline, where the device's status changes, and the loop reads it. What
 * came due waits for the first delivery point with interrupts on. A table of
 * its own, good for one stretch with interrupts off: the next back edge with
 * them on starts a new one (an epoch, so nothing is cleared). */
static QuietSlot g_quiet_off[QUIET_SLOTS];
static uint32_t g_quiet_off_epoch[QUIET_SLOTS], g_quiet_epoch = 1;
static int g_quiet_off_used;
static unsigned long long g_quiet_off_run;
static uint64_t g_quiet_off_sum;

static void quiet_off_forget(void)
{
    if (!g_quiet_off_used) return;
    g_quiet_off_used = 0;
    g_quiet_off_run = 0;
    g_quiet_epoch++;
}

static void quiet_off_wait(CpuState* s, int by_length)
{
    unsigned reads = g_clock_wait_reads;
    g_clock_wait_reads = 0;
    g_quiet_off_waits++;
    quiet_off_forget();
    clock_fixed_note_spin(0, by_length ? 4 : 2);
    fixed_wait_skip(s, reads, &g_step_off);
}

static void quiet_poll_off(CpuState* s)
{
    unsigned k = (s->pc >> 2) & (QUIET_SLOTS - 1u);
    QuietSlot* q = &g_quiet_off[k];
    g_quiet_off_used = 1;
    {
        int fresh = g_quiet_off_epoch[k] != g_quiet_epoch;
        g_quiet_off_epoch[k] = g_quiet_epoch;
        if (quiet_match(q, s, &g_quiet_off_sum, fresh) == 2) {
            quiet_off_wait(s, 0);
            return;
        }
    }
    if (++g_quiet_off_run >= clock_fixed_spin()) quiet_off_wait(s, 1);
}

static unsigned g_poll_n; /* the old rule's count of back edges with interrupts on */

void irq_poll(CpuState* s)
{
    if (!(s->msr & 0x8000u) || g_in_handler) { /* MSR[EE] clear, or nested */
        if (g_clock_quiet && g_clock_quiet_off) quiet_poll_off(s);
        return;
    }
    if (g_clock_quiet) {
        quiet_off_forget();
        g_step_off.pc = 0; /* a stretch with interrupts off is over: its loops were left */
        quiet_poll(s);
        return;
    }
    if (++g_poll_n & 0xFu) return;
    if (clock_fixed()) clock_fixed_poll(); /* the guest's work is its clock's */
    deliver_and_reschedule(s);
}

/* A wait the runtime recognises in the game's own code (tick.c: the frame
 * end's spin on the retrace count). Under the fixed-step clock it is the
 * idle loop's case: deliver what is due, and with nothing due go to the next
 * deadline, rather than spin the guest until delivery points add up to a
 * field. Under the real clock it does nothing; the spin is the game's. */
void irq_fixed_wait(CpuState* s)
{
    if (!clock_fixed() || !(s->msr & 0x8000u) || g_in_handler) return;
    if (!deliver_and_reschedule(s)) fixed_idle_skip(s);
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
    if (g_event_runs || g_line_runs || g_line_unhandled)
        fprintf(stderr, "[irq] native: %llu events run, late by at most %llu ticks (%.1f us), %.1f ticks on average; "
                        "%llu raised lines delivered, %llu with no handler installed\n",
                (unsigned long long)g_event_runs, (unsigned long long)g_event_late_max,
                (double)g_event_late_max * 1e6 / (double)TB_HZ,
                g_event_runs ? (double)g_event_late_sum / (double)g_event_runs : 0.0, (unsigned long long)g_line_runs,
                (unsigned long long)g_line_unhandled);
    if (clock_fixed()) {
        unsigned long long reads, polls, skips;
        uint64_t skipped;
        clock_fixed_stats(&reads, &polls, &skips, &skipped);
        fprintf(stderr, "[clock] fixed: %llu ticks of guest time (%.3f s): %llu timebase and hardware reads, %llu delivery points, "
                        "%llu idle skips for %llu ticks\n",
                (unsigned long long)clock_fixed_now(), (double)clock_fixed_now() / (double)TB_HZ, reads, polls, skips,
                (unsigned long long)skipped);
        if (g_clock_quiet) {
            unsigned long long w[5] = {0}, longest = 0;
            clock_fixed_spin_stats(w, &longest);
            fprintf(stderr, "[clock] quiet: back edges cost no time; %llu waits found by a loop's repeated state "
                            "(%llu went to the next deadline), %llu by %llu back edges with nothing due (%llu); the longest "
                            "run of back edges that ended at something due was %llu\n",
                    w[1] + w[2], w[2], w[3] + w[4], clock_fixed_spin(), w[4], longest);
            if (g_clock_quiet_off)
                fprintf(stderr, "[clock] free reads: reads cost no time; %llu of those waits with interrupts off, %llu stepped "
                                "for a loop on the timebase, %llu to AI's next sample; %llu decrementer exceptions a second "
                                "time at one instant, 8 ticks on; %llu repeats not a wait, the back edges between them "
                                "different\n",
                        g_quiet_off_waits, g_wait_tb_steps, g_wait_ai_steps, (unsigned long long)g_dec_repeats,
                        g_quiet_passes_differ);
            if (g_answers)
                fprintf(stderr, "[clock] answers: %llu repeated passes had a native answer in them (memory hashed); "
                                "%llu of them would have been a wait but the answer had changed memory\n",
                        g_quiet_answer_passes, g_quiet_answer_work);
        }
    }
}

/* soa_host_charge: an answer that stands for a device wait moves the fixed
 * clock that far (docs/CLOCK.md); the real clock's own time is the guest's. */
int irq_charge(uint64_t ticks)
{
    if (!clock_fixed() || !ticks) return 0;
    return clock_fixed_skip_to(clock_fixed_now() + ticks);
}

uint64_t irq_retrace_count(void)
{
    return g_vi_count;
}

/* ---- a snapshot (snapshot.h; docs/SNAPSHOT.md) ------------------------------
 * Taken at the main loop's top, never inside a delivery: g_in_handler and
 * g_irq_ctx are 0 there and the jmp_buf is dead. Everything else here is the
 * future -- the devices' registers, the decrementer, the retrace's phase, the
 * quiet rule's tables of loop states, which decide where the next waits are
 * found -- and the report's counters, so a resumed run's report is the
 * whole run's. */
#include "snapshot.h"

/* A host's own function and data for an event of the snapshot's, given
 * before the run starts (soa_host_resume_event): for an event whose user
 * data is on the heap, which no snapshot can find again. */
typedef struct {
    int id;
    IrqEventFn fn;
    void* user;
    void (*extra)(void);
} Rebind;
static Rebind g_rebind[IRQ_EVENTS];
static int g_nrebind;

int irq_event_rebind(int id, IrqEventFn fn, void* user, void (*extra)(void))
{
    int k;
    if (id <= 0 || !fn) return 0;
    plat_lock(&g_event_lock);
    for (k = 0; k < g_nrebind && g_rebind[k].id != id; k++) {}
    if (k == IRQ_EVENTS) {
        plat_unlock(&g_event_lock);
        return 0;
    }
    g_rebind[k].id = id, g_rebind[k].fn = fn, g_rebind[k].user = user, g_rebind[k].extra = extra;
    if (k == g_nrebind) g_nrebind++;
    plat_unlock(&g_event_lock);
    return 1;
}

/* The events: each one's tick and period, and its function, data and the
 * caller's function as modules and offsets (snap_pointer). Loading with
 * snap_dry (soa_host_start's check) reads them into nothing and fails for an
 * event that would not run. */
void irq_events_snap(Snap* sn)
{
    int k, n = IRQ_EVENTS, dry = snap_dry(sn);
    SNAP_CHECK(sn, n, "the size of the event table");
    if (!snap_loading(sn)) plat_lock(&g_event_lock);
    for (k = 0; k < IRQ_EVENTS && !snap_failed(sn); k++) {
        IrqEvent e = g_events[k];
        void *fn = (void*)e.fn, *user = e.user, *extra = (void*)e.extra;
        int ok_fn = 1, ok_user = 1, ok_extra = 1, r;
        SNAP(sn, e.id);
        SNAP(sn, e.due);
        SNAP(sn, e.period);
        snap_pointer(sn, &fn, &ok_fn);
        snap_pointer(sn, &user, &ok_user);
        snap_pointer(sn, &extra, &ok_extra);
        if (!snap_loading(sn) || snap_failed(sn)) continue;
        for (r = 0; r < g_nrebind && g_rebind[r].id != e.id; r++) {}
        if (e.id && r < g_nrebind) { /* the host's word for it is the last */
            fn = (void*)g_rebind[r].fn, user = g_rebind[r].user, extra = (void*)g_rebind[r].extra;
            ok_fn = ok_user = ok_extra = 1;
        }
        if (e.id && (!ok_fn || !ok_user || !ok_extra || !fn)) {
            snap_fail(sn, "event %d (due at tick %llu%s) has %s no snapshot can find again; the host gives it back with "
                          "soa_host_resume_event before soa_host_start",
                      e.id, (unsigned long long)e.due, e.period ? ", periodic" : "",
                      !ok_fn || !fn ? "a function" : !ok_extra ? "a host function" : "user data on the heap,");
            continue;
        }
        if (dry) continue;
        e.fn = (IrqEventFn)fn;
        e.user = user;
        e.extra = (void (*)(void))extra;
        if (!e.id) memset(&e, 0, sizeof e);
        g_events[k] = e;
    }
    {
        int next = g_event_next_id;
        SNAP(sn, next);
        if (!dry) g_event_next_id = next;
    }
    if (!dry && snap_loading(sn) && !snap_failed(sn)) {
        int pending = 0;
        for (k = 0; k < IRQ_EVENTS; k++) pending += g_events[k].id != 0;
        g_nevents = pending;
    }
    if (!snap_loading(sn)) plat_unlock(&g_event_lock);
}

void irq_snap(Snap* sn)
{
    uint32_t lines = g_lines;
    if (!snap_loading(sn) && (g_in_handler || g_irq_ctx)) {
        snap_fail(sn, "an interrupt handler is running");
        return;
    }
    SNAP(sn, g_vi_di);
    SNAP(sn, g_pi_mask);
    SNAP(sn, g_pi_cause);
    SNAP(sn, g_ai_cr);
    SNAP(sn, g_ai_vr);
    SNAP(sn, g_ai_it);
    SNAP(sn, g_ai_scnt_base);
    SNAP(sn, g_vi_pending);
    SNAP(sn, g_vi_last);
    SNAP(sn, g_vi_count);
    SNAP(sn, g_dec_value);
    SNAP(sn, g_dec_set);
    SNAP(sn, g_dec_armed);
    SNAP(sn, g_dec_repeats);
    SNAP(sn, g_dec_last);
    SNAP(sn, g_dec_count);
    SNAP(sn, g_dec_arms);
    SNAP(sn, g_di_count);
    SNAP(sn, g_pe_count);
    SNAP(sn, g_ar_count);
    SNAP(sn, g_dsp_count);
    SNAP(sn, g_aid_count);
    SNAP(sn, g_si_count);
    SNAP(sn, g_exi_count);
    SNAP(sn, g_exid_count);
    SNAP(sn, g_delivered);
    SNAP(sn, g_delivered_by_number);
    SNAP(sn, lines);
    if (snap_loading(sn)) g_lines = lines;
    SNAP(sn, g_event_runs);
    SNAP(sn, g_event_late_max);
    SNAP(sn, g_event_late_sum);
    SNAP(sn, g_line_runs);
    SNAP(sn, g_line_unhandled);
    SNAP(sn, g_step_on);
    SNAP(sn, g_step_off);
    SNAP(sn, g_wait_tb_steps);
    SNAP(sn, g_wait_ai_steps);
    SNAP(sn, g_quiet_off_waits);
    SNAP(sn, g_quiet_passes_differ);
    SNAP(sn, g_quiet_answer_passes);
    SNAP(sn, g_quiet_answer_work);
    SNAP(sn, g_quiet);
    SNAP(sn, g_quiet_run);
    SNAP(sn, g_quiet_sum);
    SNAP(sn, g_quiet_off);
    SNAP(sn, g_quiet_off_epoch);
    SNAP(sn, g_quiet_epoch);
    SNAP(sn, g_quiet_off_used);
    SNAP(sn, g_quiet_off_run);
    SNAP(sn, g_quiet_off_sum);
    SNAP(sn, g_poll_n);
}
