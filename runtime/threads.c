/*
 * Guest threads on host fibers (SPEC section 4.3b).
 *
 * The SDK switches threads with OSSaveContext -- setjmp-like, returning 0
 * when saving and 1 when resumed -- and OSLoadContext, which restores a
 * saved context and rfi's into the middle of whatever function saved it.
 * That resume is the one thing static recompilation cannot express, so the
 * two primitives are replaced.
 *
 * Every guest thread runs on its own host fiber. OSSaveContext has exactly
 * one caller, SelectThread, and the recompiler wraps that call in a setjmp
 * on SelectThread's own frame: a parked thread therefore has a live frame to
 * return into. OSSaveContext snapshots the registers; OSLoadContext switches
 * to the target's fiber if it is not the current one and longjmps to the
 * target's savepoint, where guest_resumed restores the snapshot and hands
 * back r3 = 1 exactly as the hardware path would.
 *
 * Everything else about scheduling -- run queues, priorities, the idle loop,
 * sleep and wakeup -- stays recompiled and authentic. The runtime's one other
 * intervention is delivering interrupts at the idle loop (irq.c).
 */
#define _CRT_SECURE_NO_WARNINGS
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* OSContext layout, confirmed from OSSaveContext at 0x80233544:
 * stmw r13 lands at 0x34 (= 13*4) and GQR1 at 0x1A8 (= 0x1A4 + 4). */
enum {
    CTX_GPR = 0x000,
    CTX_CR = 0x080,
    CTX_LR = 0x084,
    CTX_CTR = 0x088,
    CTX_XER = 0x08C,
    CTX_FPR = 0x090,
    CTX_FPSCR = 0x194,
    CTX_SRR0 = 0x198,
    CTX_SRR1 = 0x19C,
    CTX_GQR = 0x1A4,
    CTX_PSF = 0x1C8,
    CTX_SIZE = 0x2C8
};

#define MAX_THREADS 64
#define FIBER_STACK (4u << 20)

typedef struct {
    uint32_t ctx; /* guest OSContext address; 0 = unused */
    void* fiber;
    int started;
    jmp_buf jmp;    /* the setjmp at this thread's OSSaveContext call site */
    CpuState saved; /* registers as of that save */
} GuestThread;

static GuestThread g_threads[MAX_THREADS];
static GuestThread* g_resuming; /* set by the loader for guest_resumed */
static void* g_main_fiber;
static CpuState* g_s;
static uint64_t g_saves, g_resumes, g_switches;

/* How many fiber switches there have been: runtime/record.c tells a recorded
 * call that slept (another thread ran inside it) by this moving. */
uint64_t threads_switch_count(void)
{
    return g_switches;
}

/* Return addresses up a guest stack: EABI keeps the back chain at 0(r1)
 * and each frame's saved LR at 4(caller r1). Prints on one line. */
void guest_backtrace(CpuState* s, uint32_t sp)
{
    uint32_t depth;
    for (depth = 0; depth < 14 && sp >= 0x80000000u && sp < 0x81800000u; depth++) {
        uint32_t prev = mem_r32(s, sp);
        uint32_t ret;
        if (prev <= sp || prev >= 0x81800000u) break;
        ret = mem_r32(s, prev + 4);
        fprintf(stderr, " %08X", ret);
        sp = prev;
    }
    fprintf(stderr, "\n");
}

void threads_report(void)
{
    int i, n = 0;
    for (i = 0; i < MAX_THREADS; i++) n += g_threads[i].ctx != 0;
    fprintf(stderr, "[threads] %d guest threads seen; %llu context saves, %llu resumes, %llu fiber switches\n",
            n, (unsigned long long)g_saves, (unsigned long long)g_resumes,
            (unsigned long long)g_switches);

    /* A parked thread's guest stack says what it is waiting in: EABI keeps
     * the back chain at 0(r1) and the saved LR at 4(caller r1). */
    for (i = 0; i < MAX_THREADS; i++) {
        GuestThread* t = &g_threads[i];
        uint32_t sp;
        if (!t->ctx || !g_s) continue;
        sp = t->saved.gpr[1];
        fprintf(stderr, "  thread ctx %08X parked at lr %08X, r1 %08X; backtrace:", t->ctx,
                t->saved.lr, sp);
        guest_backtrace(g_s, sp);
    }
}

uint32_t irq_interrupted_context(void);
void irq_return_from_handler(void);
void irq_abandon_delivery(void);

void threads_init(CpuState* s)
{
    g_s = s;
#ifdef _WIN32
    g_main_fiber = ConvertThreadToFiber(NULL);
#endif
}

static GuestThread* by_ctx(uint32_t ctx)
{
    int i;
    for (i = 0; i < MAX_THREADS; i++)
        if (g_threads[i].ctx == ctx) return &g_threads[i];
    return NULL;
}

static GuestThread* by_fiber(void* fiber)
{
    int i;
    for (i = 0; i < MAX_THREADS; i++)
        if (g_threads[i].ctx && g_threads[i].fiber == fiber) return &g_threads[i];
    return NULL;
}

static GuestThread* alloc(uint32_t ctx)
{
    int i;
    for (i = 0; i < MAX_THREADS; i++) {
        if (!g_threads[i].ctx) {
            memset(&g_threads[i], 0, sizeof g_threads[i]);
            g_threads[i].ctx = ctx;
            return &g_threads[i];
        }
    }
    fprintf(stderr, "[threads] more than %d guest threads\n", MAX_THREADS);
    exit(6);
}

static void* current_fiber(void)
{
#ifdef _WIN32
    return GetCurrentFiber();
#else
    return NULL;
#endif
}

/* Keep the guest's own OSContext coherent: the SDK reads these back. */
static void ctx_write(CpuState* s, uint32_t ctx)
{
    int i;
    for (i = 0; i < 32; i++) mem_w32(s, ctx + CTX_GPR + 4 * i, s->gpr[i]);
    mem_w32(s, ctx + CTX_CR, s->cr);
    mem_w32(s, ctx + CTX_LR, s->lr);
    mem_w32(s, ctx + CTX_CTR, s->ctr);
    mem_w32(s, ctx + CTX_XER, s->xer);
    for (i = 0; i < 32; i++) {
        mem_wf64(s, ctx + CTX_FPR + 8 * i, s->fpr[i].ps0);
        mem_wf64(s, ctx + CTX_PSF + 8 * i, s->fpr[i].ps1);
    }
    mem_w32(s, ctx + CTX_FPSCR, s->fpscr);
    mem_w32(s, ctx + CTX_SRR0, s->lr); /* resume point: the return from OSSaveContext */
    mem_w32(s, ctx + CTX_SRR1, s->msr);
    for (i = 0; i < 8; i++) mem_w32(s, ctx + CTX_GQR + 4 * i, s->gqr[i]);
}

static void ctx_read(CpuState* s, uint32_t ctx)
{
    int i;
    for (i = 0; i < 32; i++) s->gpr[i] = mem_r32(s, ctx + CTX_GPR + 4 * i);
    s->cr = mem_r32(s, ctx + CTX_CR);
    s->lr = mem_r32(s, ctx + CTX_LR);
    s->ctr = mem_r32(s, ctx + CTX_CTR);
    s->xer = mem_r32(s, ctx + CTX_XER);
    for (i = 0; i < 32; i++) {
        s->fpr[i].ps0 = mem_rf64(s, ctx + CTX_FPR + 8 * i);
        s->fpr[i].ps1 = mem_rf64(s, ctx + CTX_PSF + 8 * i);
    }
    s->fpscr = mem_r32(s, ctx + CTX_FPSCR);
    s->msr = mem_r32(s, ctx + CTX_SRR1);
    for (i = 0; i < 8; i++) s->gqr[i] = mem_r32(s, ctx + CTX_GQR + 4 * i);
}

static void restore(GuestThread* t, CpuState* s)
{
    uint8_t* mem = s->mem;
    void* user = s->user;
    memcpy(s, &t->saved, sizeof *s);
    s->mem = mem;
    s->user = user;
}

/* Called at the OSSaveContext call site, before the call: r3 is the context
 * about to be saved, i.e. the current thread. Hands back its jmp_buf. */
jmp_buf* guest_savepoint(CpuState* s)
{
    uint32_t ctx = s->gpr[3];
    GuestThread* t = by_ctx(ctx);
    if (!t) t = alloc(ctx);
    t->fiber = current_fiber();
    t->started = 1;
    return &t->jmp;
}

/* Called when a longjmp lands on that setjmp: this thread has been loaded. */
void guest_resumed(CpuState* s)
{
    GuestThread* t = g_resuming;
    g_resuming = NULL;
    if (!t) {
        fprintf(stderr, "[threads] resumed with no target\n");
        exit(6);
    }
    restore(t, s);
    s->gpr[3] = 1; /* OSSaveContext "returns" 1 the second time */
    irq_abandon_delivery(); /* any delivery on the way here is over */
    g_resumes++;
}

/* OSSaveContext(ctx) -- 0x80233544. */
void fn_80233544(CpuState* s)
{
    uint32_t ctx = s->gpr[3];
    GuestThread* t = by_ctx(ctx);
    if (!t) { /* only if the recompiler did not wrap the call */
        t = alloc(ctx);
        t->fiber = current_fiber();
        t->started = 1;
    }
    ctx_write(s, ctx);
    mem_w32(s, ctx + CTX_GPR + 4 * 3, 1); /* the guest's own setjmp trick */
    memcpy(&t->saved, s, sizeof *s);
    s->gpr[3] = 0;
    g_saves++;
}

#ifdef _WIN32
/* First run of a thread OSCreateThread set up: registers come from the guest
 * context it initialised (stack in r1, argument in r3, PC in srr0, and LR
 * pointing at OSExitThread). */
static void CALLBACK thread_entry(void* arg)
{
    GuestThread* t = (GuestThread*)arg;
    CpuState* s = g_s;
    uint32_t pc, lr;
    ctx_read(s, t->ctx);
    pc = mem_r32(s, t->ctx + CTX_SRR0);
    lr = s->lr;
    irq_abandon_delivery();
    dispatch(s, pc);
    dispatch(s, lr); /* OSExitThread: reschedules away and never returns */
    fprintf(stderr, "[threads] thread %08X ran past its exit\n", t->ctx);
    for (;;) SwitchToFiber(g_main_fiber);
}

static void resume_self(void)
{
    GuestThread* me = by_fiber(current_fiber());
    if (!me) {
        fprintf(stderr, "[threads] switched to a fiber with no savepoint\n");
        exit(6);
    }
    g_resuming = me;
    longjmp(me->jmp, 1);
}
#endif

/* OSLoadContext(ctx) -- 0x802335C4. Never returns. */
void fn_802335C4(CpuState* s)
{
    uint32_t ctx = s->gpr[3];
    GuestThread* t = by_ctx(ctx);

    if (ctx && ctx == irq_interrupted_context()) {
        irq_return_from_handler(); /* a handler returning to what it interrupted */
    }
    /* Same fiber: unwind to its savepoint. Plain setjmp and longjmp on one
     * stack, so it runs everywhere; off Windows every thread's fiber is
     * NULL, and the guest's one thread always takes this path (L7). */
    if (t && t->started && t->fiber == current_fiber()) {
        g_resuming = t;
        longjmp(t->jmp, 1);
    }
#ifdef _WIN32
    if (t && t->started) {
        g_switches++;
        SwitchToFiber(t->fiber);
        resume_self(); /* we were switched back: go to our own savepoint */
    }
    if (!ctx || mem_r32(s, ctx + CTX_SRR0) == 0) {
        fprintf(stderr, "[threads] OSLoadContext(%08X) from lr %08X with no entry; backtrace:", ctx, s->lr);
        guest_backtrace(s, s->gpr[1]);
        exit(6);
    }
    if (!t) t = alloc(ctx);
    t->started = 1;
    fprintf(stderr, "[threads] new guest thread: context %08X, entry %08X\n", ctx,
            mem_r32(s, ctx + CTX_SRR0));
    t->fiber = CreateFiber(FIBER_STACK, thread_entry, t);
    if (!t->fiber) {
        fprintf(stderr, "[threads] CreateFiber failed\n");
        exit(6);
    }
    SwitchToFiber(t->fiber);
    resume_self();
#else
    (void)t;
    fprintf(stderr, "[threads] OSLoadContext(%08X): a second guest thread is Windows-only\n", ctx);
    exit(6);
#endif
}

/* A snapshot (snapshot.h) at the main loop's top: no thread is parked in a
 * SelectThread then -- the guest is running its one thread, and its savepoint
 * jmp_buf is dead -- so each thread is its context, whether it has started
 * and the registers of its last save (OSSaveContext's copy, which the next
 * resume of it would put back). A thread on a fiber of its own (a second
 * guest thread, Windows only) has a host stack no file can hold: refused. */
#include "snapshot.h"
void threads_snap(Snap* sn)
{
    int i;
    uint32_t n = MAX_THREADS;
    SNAP_CHECK(sn, n, "the table of guest threads");
    if (!snap_loading(sn))
        for (i = 0; i < MAX_THREADS; i++)
            if (g_threads[i].ctx && g_threads[i].started && g_threads[i].fiber != current_fiber()) {
                snap_fail(sn, "guest thread %08X runs on a fiber of its own; a snapshot holds one thread", g_threads[i].ctx);
                return;
            }
    for (i = 0; i < MAX_THREADS; i++) {
        GuestThread* t = &g_threads[i];
        SNAP(sn, t->ctx);
        SNAP(sn, t->started);
        snap_bytes(sn, &t->saved, offsetof(CpuState, mem));
        if (snap_loading(sn)) {
            t->fiber = current_fiber();
            memset(&t->jmp, 0, sizeof t->jmp);
            t->saved.mem = g_s ? g_s->mem : NULL;
            t->saved.user = g_s ? g_s->user : NULL;
        }
    }
    SNAP(sn, g_saves);
    SNAP(sn, g_resumes);
    SNAP(sn, g_switches);
}

/* OSSwitchFiber(pc, sp) -- 0x802336A4. Call pc on a different stack. */
void fn_802336A4(CpuState* s)
{
    uint32_t old_sp = s->gpr[1];
    uint32_t pc = s->gpr[3];
    s->gpr[1] = s->gpr[4] - 8;
    mem_w32(s, s->gpr[1], old_sp);
    dispatch(s, pc);
    s->gpr[1] = old_sp;
}
