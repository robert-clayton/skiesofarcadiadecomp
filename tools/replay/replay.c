/*
 * The replay (replay.h, docs/RECORD.md): a recording's calls run again,
 * outside the game, against the game library of the build that made it.
 *
 * Built as a libsoa_runtime.so of its own (tools/replay.py): the game
 * library needs a library of that name, and gets this one, which answers
 * the runtime's side of the seam (config/seam.txt) as a replay needs and no
 * more. What a call may meet of the runtime and how it is met:
 *
 *   the swap guard (soa_swap_enter)  the candidate, at its function's entry;
 *                                    every other entry told to the trace
 *   the timebase, a hardware read    the recording's, in order
 *   a store to the hardware          held to the recording's, in order
 *   VIGetRetraceCount                the word in memory the runtime's reads
 *   the cache calls, OSReport, the game's debug print, the trace and watch
 *   points, the Ninja model notes    nothing, as they do nothing to memory
 *   the GX pipe, the decrementer, a trap, a thread's context saved or
 *   switched, the scheduler's idle, the audio system's start
 *                                    the call is ended there: unreplayable
 *
 * Memory is the runtime's shape (the RAM, then the mask's slack), all of it
 * inaccessible but for the pages of the call being replayed: a touch of
 * any other faults here, and the page is given zeroed and noted. The stack
 * under the call is scratch: its pages are given without a note.
 */
#define _GNU_SOURCE
#include "replay.h"
#include "cpu.h"
#include "record.h"
#include "soa_game.h"
#include "swap.h"
#include <dlfcn.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))
#define RESERVE ((size_t)MEM_MASK + 1u + 0x10000u)

/* ---- the candidates ---- */

#define MAX_CANDIDATES 256
static const SoaReplayCandidate* g_cands[MAX_CANDIDATES];
static unsigned g_ncands;

EXPORT void soa_replay_register(const SoaReplayCandidate* c)
{
    if (g_ncands < MAX_CANDIDATES) g_cands[g_ncands++] = c;
}

EXPORT const SoaReplayCandidate* const* soa_replay_candidates(unsigned* n)
{
    *n = g_ncands;
    return g_cands;
}

/* ---- the state of a replay ---- */

static const SoaGame* g_game;
static CpuState g_s;
static uint8_t* g_mem;
static uint32_t g_page;
/* A page's state. Pages stay given from one call to the next where both
 * have them, so a page neither call changed costs nothing to put back. */
static uint8_t* g_incall; /* 1: the call's (recorded); 2: given zeroed on a touch; 0: not this call's */
static uint8_t* g_prot;   /* 0 none, 1 read, 2 read and write */
static uint8_t* g_valid;  /* 1: the page as it is here is its last image in the file */
static uint8_t* g_dirty;  /* 1: written by this replay of the call */
static uint32_t* g_extra; /* the pages given on a touch, this call */
static uint32_t g_nextra;
static uint32_t* g_held;  /* the pages given now: the last call's and its touches */
static uint32_t g_nheld;
static uint32_t* g_runs;  /* scratch for protect() */
static uint32_t g_outside_at, g_outside_n, g_outside_storing; /* the first such touch outside the stack */
static uint32_t g_scratch_lo, g_scratch_hi; /* the stack under the call: RAM offsets */

static sigjmp_buf g_jmp;
static volatile int g_in_call;
static const char* g_why; /* why the call was ended */
static uint32_t g_why_pc;

static const RecInput* g_inputs;
static uint32_t g_ninputs, g_input_at;
static int g_input_bad;
static uint32_t g_stores_off, g_store_ea; /* stores to the hardware not the game's call's, and the first */
static uint64_t g_store_v;

static int g_trace;           /* the entries told, into g_called */
static RecCallee* g_called;
static uint32_t g_ncalled, g_called_cap;
static int g_root;            /* the next entry is the call's own */
static uint32_t g_skip;       /* as swap.c's: the next entry of index+1 runs its translation */

static const SoaReplayCandidate* g_cand;
static int g_cand_index = -1;
static uint64_t g_answered, g_games, g_both, g_live;
static uint64_t g_seen, g_frame;

EXPORT uint64_t soa_replay_seen(void) { return g_seen; }
EXPORT uint64_t soa_replay_frame(void) { return g_frame; }

static void stop(const char* why, uint32_t pc)
{
    if (!g_in_call) {
        fprintf(stderr, "[replay] %s at %08X outside a call\n", why, pc);
        abort();
    }
    g_why = why;
    g_why_pc = pc;
    siglongjmp(g_jmp, 1);
}

/* ---- the runtime's side of the seam ---- */

EXPORT uint32_t g_watch_addr, g_watch_len;
EXPORT void watch_hit(CpuState* s, uint32_t ea, unsigned size, uint64_t v) { (void)s, (void)ea, (void)size, (void)v; }
EXPORT void trace_hit(CpuState* s, uint32_t pc, const char* name) { (void)s, (void)pc, (void)name; }
EXPORT void irq_poll(CpuState* s) { (void)s; }
EXPORT void guest_syscall(CpuState* s, uint32_t pc) { (void)s, (void)pc; }
EXPORT void guest_trap(CpuState* s, uint32_t pc) { (void)s; stop("a trap", pc); }
EXPORT void guest_unimplemented(CpuState* s, uint32_t pc, const char* what) { (void)s, (void)what; stop("an instruction the recompiler left out", pc); }

static jmp_buf g_savepoint;
EXPORT jmp_buf* guest_savepoint(CpuState* s) { (void)s; return &g_savepoint; }
EXPORT void guest_resumed(CpuState* s) { stop("a thread resumed", s->pc); }

/* A store to the hardware: the next of the recording's accesses, or a difference. */
static void store(CpuState* s, uint32_t kind, uint32_t ea, uint64_t v)
{
    const RecInput* in = g_input_at < g_ninputs ? &g_inputs[g_input_at] : NULL;
    (void)s;
    if (in && in->kind == kind && in->ea == ea) {
        g_input_at++;
        if (in->value == v) return;
    }
    if (!g_stores_off++) g_store_ea = ea, g_store_v = v;
}

static uint64_t input(CpuState* s, uint32_t kind, uint32_t ea)
{
    if (g_input_at < g_ninputs && g_inputs[g_input_at].kind == kind && g_inputs[g_input_at].ea == ea)
        return g_inputs[g_input_at++].value;
    g_input_bad = 1;
    stop(kind >= REC_IN_MMIO ? "a hardware read the recording does not have (not the next one it has)"
                             : "a timebase read the recording does not have", s->pc);
    return 0;
}
EXPORT uint32_t guest_timebase_lo(CpuState* s) { return (uint32_t)input(s, REC_IN_TB_LO, 0); }
EXPORT uint32_t guest_timebase_hi(CpuState* s) { return (uint32_t)input(s, REC_IN_TB_HI, 0); }
EXPORT uint32_t dec_read(CpuState* s) { stop("the decrementer read", s->pc); return 0; }
EXPORT void dec_write(CpuState* s, uint32_t v) { (void)v; stop("the decrementer set", s->pc); }

#define MMIO(N, T)                                                                                     \
    EXPORT T mmio_read##N(CpuState* s, uint32_t ea) { return (T)input(s, REC_IN_MMIO | (uint32_t)sizeof(T) << 8, ea); } \
    EXPORT void mmio_write##N(CpuState* s, uint32_t ea, T v) { store(s, REC_MMIO_STORE | (uint32_t)sizeof(T) << 8, ea, v); }
MMIO(8, uint8_t)
MMIO(16, uint16_t)
MMIO(32, uint32_t)
MMIO(64, uint64_t)
EXPORT void gx_pipe_write(CpuState* s, unsigned size, uint64_t v) { (void)size, (void)v; stop("the GX pipe written", s->pc); }

/* config/hle.txt's bindings in a split build */
EXPORT void fn_80232B90(CpuState* s) { stop("__OSInitAudioSystem", s->pc); }
EXPORT void fn_80232D4C(CpuState* s) { stop("__OSStopAudioSystem", s->pc); }
EXPORT void fn_80232E38(CpuState* s) { s->pc = 0x80232E38u; }
EXPORT void fn_80232E64(CpuState* s) { s->pc = 0x80232E64u; }
EXPORT void fn_80232E94(CpuState* s) { s->pc = 0x80232E94u; }
EXPORT void fn_80232EC4(CpuState* s) { s->pc = 0x80232EC4u; }
EXPORT void fn_80232EF0(CpuState* s) { s->pc = 0x80232EF0u; }
EXPORT void fn_80233544(CpuState* s) { stop("OSSaveContext", s->pc); }
EXPORT void fn_802335C4(CpuState* s) { stop("OSLoadContext", s->pc); }
EXPORT void fn_802336A4(CpuState* s) { stop("OSSwitchFiber", s->pc); }
EXPORT void fn_802AC75C(CpuState* s) { s->pc = 0x802AC75Cu; }
EXPORT void fn_801DBE1C(CpuState* s) { s->pc = 0x801DBE1Cu; }
EXPORT void fn_8023F704(CpuState* s) /* VIGetRetraceCount, as runtime/tick.c answers it but at the frame's start */
{
    s->pc = 0x8023F704u;
    if (s->lr == 0x801DCB88u) stop("VIGetRetraceCount at the frame's start (the runtime's safe point)", s->pc);
    s->gpr[3] = mem_r32(s, 0x80347A64u);
}
EXPORT void hook_80237BA8(CpuState* s) { stop("the scheduler's idle", s->pc); }
EXPORT void hook_802873BC(CpuState* s) { (void)s; }
EXPORT void hook_802875FC(CpuState* s) { (void)s; }
EXPORT void hook_80287C14(CpuState* s) { (void)s; }
EXPORT void hook_80287D80(CpuState* s) { (void)s; }
EXPORT void hook_802890A4(CpuState* s) { (void)s; }
EXPORT void hook_80289210(CpuState* s) { (void)s; }
EXPORT void hook_8028AB48(CpuState* s) { (void)s; }
EXPORT void hook_8028AD88(CpuState* s) { (void)s; }
EXPORT void hook_8029D0F4(CpuState* s) { (void)s; }

static int index_of(uint32_t address)
{
    uint32_t lo = 0, hi = g_game->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (g_game->address[mid] < address) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_game->count && g_game->address[lo] == address && !g_game->kind[lo] ? (int)lo : -1;
}

static void run_translation(CpuState* s, uint32_t address)
{
    int i = index_of(address);
    if (i >= 0) g_skip = (uint32_t)i + 1u;
    g_game->dispatch(s, address);
    g_skip = 0;
}

typedef struct {
    uint32_t gpr[32];
    Fpr fpr[32];
    uint32_t gqr[8];
    uint32_t cr, xer, lr, ctr, fpscr, msr, pc;
} Saved;

/* mod_call_guest's call (runtime/mod.c): every register put back, r3 taken. */
static int call_game(CpuState* s, uint32_t address, const uint32_t* args, unsigned n, uint32_t* r3, int original)
{
    Saved v;
    unsigned k;
    if (n > 8 || !g_game->dispatch_known(address)) return 0;
    if (s->gpr[2] != 0x80350000u || s->gpr[13] != 0x8034E720u) return 0;
    memcpy(v.gpr, s->gpr, sizeof v.gpr);
    memcpy(v.fpr, s->fpr, sizeof v.fpr);
    memcpy(v.gqr, s->gqr, sizeof v.gqr);
    v.cr = s->cr, v.xer = s->xer, v.lr = s->lr, v.ctr = s->ctr, v.fpscr = s->fpscr, v.msr = s->msr, v.pc = s->pc;
    for (k = 0; k < n; k++) s->gpr[3 + k] = args[k];
    if (original) run_translation(s, address);
    else g_game->dispatch(s, address);
    if (r3) *r3 = s->gpr[3];
    memcpy(s->gpr, v.gpr, sizeof v.gpr);
    memcpy(s->fpr, v.fpr, sizeof v.fpr);
    memcpy(s->gqr, v.gqr, sizeof v.gqr);
    s->cr = v.cr, s->xer = v.xer, s->lr = v.lr, s->ctr = v.ctr, s->fpscr = v.fpscr, s->msr = v.msr, s->pc = v.pc;
    return 1;
}

static int host_game(SoaHostCall* c, uint32_t address, const uint32_t* args, unsigned n, uint32_t* r3)
{
    return call_game((CpuState*)c->runtime, address, args, n, r3, 0);
}
static int host_game_original(SoaHostCall* c, uint32_t address, const uint32_t* args, unsigned n, uint32_t* r3)
{
    return call_game((CpuState*)c->runtime, address, args, n, r3, 1);
}
static void host_original(SoaHostCall* c)
{
    CpuState* s = (CpuState*)c->runtime;
    run_translation(s, c->address);
    c->gpr[0] = s->gpr[3];
    c->fpr[0] = s->fpr[1].ps0;
}

/* runtime/host.c's host_swap, with the candidate for the host. */
static int ask(CpuState* s, uint32_t address, int after)
{
    SoaHostCall c;
    unsigned r;
    int how;
    memset(&c, 0, sizeof c);
    c.address = address;
    c.after = after ? 1u : 0u;
    c.mem = s->mem;
    c.mem_bytes = MEM1_SIZE;
    c.sp = s->gpr[1];
    for (r = 0; r < 8; r++) c.gpr[r] = s->gpr[3 + r];
    c.frame = (long)g_frame;
    c.game = host_game;
    c.runtime = s;
    for (r = 0; r < 8; r++) c.fpr[r] = s->fpr[1 + r].ps0;
    c.r = s->gpr;
    c.f = (SoaHostPair*)s->fpr;
    c.original = host_original;
    c.game_original = host_game_original;
    how = g_cand->answer(g_cand->user, &c);
    if (after) return SWAP_GAMES;
    if (how == SOA_HOST_ANSWERED) {
        s->gpr[3] = c.gpr[0];
        s->fpr[1].ps0 = s->fpr[1].ps1 = c.fpr[0];
        g_answered++;
        return SWAP_ANSWERED;
    }
    if (how == SOA_HOST_ANSWERED_LIVE) {
        g_live++;
        return SWAP_ANSWERED;
    }
    if (how == SOA_HOST_BOTH) {
        g_both++;
        return SWAP_BOTH;
    }
    g_games++;
    return SWAP_GAMES;
}

EXPORT int soa_swap_enter(CpuState* s, uint32_t i)
{
    uint32_t address = g_game->address[i];
    if (g_skip == i + 1u) {
        g_skip = 0;
        return 0;
    }
    if (g_root) g_root = 0;
    else if (g_trace) {
        if (g_ncalled == g_called_cap) {
            g_called_cap = g_called_cap ? g_called_cap * 2 : 1024;
            g_called = (RecCallee*)realloc(g_called, g_called_cap * sizeof *g_called);
        }
        g_called[g_ncalled].address = address;
        g_called[g_ncalled].sp = s->gpr[1];
        g_ncalled++;
    }
    if (g_cand && (int)i == g_cand_index) {
        int how;
        s->pc = address;
        how = ask(s, address, 0);
        if (how == SWAP_ANSWERED) return 1;
        if (how == SWAP_BOTH) {
            run_translation(s, address);
            ask(s, address, 1);
            return 1;
        }
    }
    return 0;
}

/* ---- memory ---- */

/* The pages given (in ascending order, or any order: runs are found where
 * they are) set to prot, a run of neighbours in one call. */
static void protect(const uint32_t* pages, uint32_t n, uint8_t prot)
{
    static const int how[3] = {PROT_NONE, PROT_READ, PROT_READ | PROT_WRITE};
    uint32_t k = 0;
    while (k < n) {
        uint32_t lo = pages[k], hi = lo + 1;
        if (g_prot[lo] == prot) {
            k++;
            continue;
        }
        g_prot[lo] = prot;
        for (k++; k < n && pages[k] == hi && g_prot[hi] != prot; k++, hi++) g_prot[hi] = prot;
        mprotect(g_mem + (size_t)lo * g_page, (size_t)(hi - lo) * g_page, how[prot]);
    }
}

static int storing(void* ucp)
{
#if defined(__x86_64__) && defined(REG_ERR)
    return (((ucontext_t*)ucp)->uc_mcontext.gregs[REG_ERR] & 2) != 0;
#else
    (void)ucp;
    return -1;
#endif
}

static void on_fault(int sig, siginfo_t* si, void* uc)
{
    uintptr_t off = (uintptr_t)si->si_addr - (uintptr_t)g_mem;
    if (g_in_call && off < RESERVE) {
        uint32_t page = (uint32_t)(off / g_page);
        if (g_incall[page] == 1 && g_prot[page] == 1) { /* a store to a page of the call's */
            g_prot[page] = 2;
            g_dirty[page] = 1;
            mprotect(g_mem + (size_t)page * g_page, g_page, PROT_READ | PROT_WRITE);
            return;
        }
        if (!g_incall[page]) {
            g_prot[page] = 2;
            mprotect(g_mem + (size_t)page * g_page, g_page, PROT_READ | PROT_WRITE);
            memset(g_mem + (size_t)page * g_page, 0, g_page);
            g_incall[page] = 2;
            g_valid[page] = 0;
            g_extra[g_nextra++] = page;
            if (!(off >= g_scratch_lo && off < g_scratch_hi)) {
                if (!g_outside_n++) g_outside_at = (uint32_t)off, g_outside_storing = (uint32_t)storing(uc);
            }
            return;
        }
    }
    signal(sig, SIG_DFL);
}

static void on_timeout(int sig)
{
    (void)sig;
    if (g_in_call) stop("more than the time a call is given (a loop that does not end?)", g_s.pc);
}

/* ---- the recording ---- */

typedef struct {
    uint8_t* data;
    size_t size;
    RecFileHead head;
    uint8_t* shadow; /* each page's last image, as the file's decoding needs it */
    uint8_t* zero;
} Rec;

static int rec_open(Rec* r, const char* path)
{
    FILE* f = fopen(path, "rb");
    long n;
    memset(r, 0, sizeof *r);
    if (!f) {
        fprintf(stderr, "[replay] cannot read %s\n", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    r->data = (uint8_t*)malloc(n > 0 ? (size_t)n : 1u);
    if (!r->data || fread(r->data, 1, (size_t)n, f) != (size_t)n || (size_t)n < sizeof r->head) {
        fprintf(stderr, "[replay] %s: cannot read it whole\n", path);
        fclose(f);
        return 0;
    }
    fclose(f);
    r->size = (size_t)n;
    memcpy(&r->head, r->data, sizeof r->head);
    if (memcmp(r->head.magic, REC_MAGIC, 8) || r->head.version != REC_VERSION || r->head.mem_bytes != MEM1_SIZE ||
        !r->head.page_size || MEM1_SIZE % r->head.page_size) {
        fprintf(stderr, "[replay] %s: not a recording this replay reads (version %u)\n", path, r->head.version);
        return 0;
    }
    r->shadow = (uint8_t*)calloc(1, MEM1_SIZE);
    r->zero = (uint8_t*)calloc(1, r->head.page_size);
    return r->shadow && r->zero;
}

/* A call, its pages decoded: pre[k] and post[k] are page k's before and
 * after the game's call (into the decoding's own images, or buf). Calls are
 * decoded in the file's order, whatever is replayed, since a page's bytes
 * are against its last image. here[k]: the page as it is in the replay's
 * memory is already pre[k]. */
typedef struct {
    RecCallHead head;
    uint32_t* page;
    uint32_t* flags;
    uint32_t* post_n; /* its post bytes: 0 when the game's call left it as it was */
    uint8_t* here;
    const uint8_t** pre;
    const uint8_t** post;
    uint8_t* buf;
    const RecInput* inputs;
    const RecCallee* callees;
    uint32_t cap;
} Call;

static int rec_next(Rec* r, size_t* at, Call* c)
{
    const uint8_t* p;
    uint32_t k, ps = r->head.page_size;
    size_t o;
    if (*at + sizeof c->head > r->size) return 0;
    memcpy(&c->head, r->data + *at, sizeof c->head);
    if (c->head.magic != REC_CALL_MAGIC || *at + c->head.bytes > r->size) return 0;
    if (c->cap < c->head.pages) {
        c->cap = c->head.pages + 64;
        c->page = (uint32_t*)realloc(c->page, c->cap * sizeof *c->page);
        c->flags = (uint32_t*)realloc(c->flags, c->cap * sizeof *c->flags);
        c->post_n = (uint32_t*)realloc(c->post_n, c->cap * sizeof *c->post_n);
        c->here = (uint8_t*)realloc(c->here, c->cap);
        c->pre = (const uint8_t**)realloc((void*)c->pre, c->cap * sizeof *c->pre);
        c->post = (const uint8_t**)realloc((void*)c->post, c->cap * sizeof *c->post);
        free(c->buf);
        c->buf = (uint8_t*)malloc((size_t)c->cap * ps);
        if (!c->page || !c->flags || !c->post_n || !c->here || !c->pre || !c->post || !c->buf) return 0;
    }
    o = *at + sizeof c->head;
    for (k = 0; k < c->head.pages; k++) {
        RecPage pg;
        uint8_t *buf = c->buf + (size_t)k * ps, *shadow;
        memcpy(&pg, r->data + o, sizeof pg);
        o += sizeof pg;
        if (pg.page >= MEM1_SIZE / ps) return 0;
        p = r->data + o;
        shadow = r->shadow + (size_t)pg.page * ps;
        /* the image the replay's memory has (when it is valid) is shadow as it was */
        c->here[k] = g_valid && g_valid[pg.page] && !pg.pre_bytes;
        if (!pg.post_bytes) { /* after as before: both are the new last image */
            if (!rec_decode(shadow, shadow, ps, p, pg.pre_bytes)) return 0;
            c->pre[k] = c->post[k] = shadow;
        } else {
            if (!rec_decode(buf, shadow, ps, p, pg.pre_bytes)) return 0;
            if (!rec_decode(shadow, buf, ps, p + pg.pre_bytes, pg.post_bytes)) return 0;
            c->pre[k] = buf;
            c->post[k] = shadow;
        }
        if (g_valid && (pg.pre_bytes || pg.post_bytes)) g_valid[pg.page] = 0;
        o += pg.pre_bytes + pg.post_bytes;
        c->page[k] = pg.page;
        c->flags[k] = pg.flags;
        c->post_n[k] = pg.post_bytes;
    }
    c->inputs = (const RecInput*)(r->data + o);
    o += c->head.inputs * sizeof(RecInput);
    c->callees = (const RecCallee*)(r->data + o);
    o += c->head.callees * sizeof(RecCallee);
    if (o != *at + c->head.bytes) return 0;
    *at = o;
    return 1;
}

static int load_game(const char* path)
{
    void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    const SoaGame* (*get)(void);
    if (!h) {
        fprintf(stderr, "[replay] %s: %s\n", path, dlerror());
        return 0;
    }
    get = (const SoaGame* (*)(void))dlsym(h, "soa_game");
    g_game = get ? get() : NULL;
    if (!g_game || g_game->abi < 2u || !g_game->swap || !g_game->count) {
        fprintf(stderr, "[replay] %s: no swap table (game table ABI 2) in it\n", path);
        return 0;
    }
    return 1;
}

static int setup(const char* game, uint32_t page)
{
    struct sigaction sa;
    long ps = sysconf(_SC_PAGESIZE);
    if (g_game) return page == g_page;
    if (ps <= 0 || page % (uint32_t)ps) {
        fprintf(stderr, "[replay] the recording's pages (%u bytes) are not whole pages here (%ld)\n", page, ps);
        return 0;
    }
    if (!load_game(game)) return 0;
    g_page = page;
    g_mem = (uint8_t*)mmap(NULL, RESERVE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    {
        size_t n = RESERVE / page + 1;
        g_incall = (uint8_t*)calloc(n, 1);
        g_prot = (uint8_t*)calloc(n, 1);
        g_valid = (uint8_t*)calloc(n, 1);
        g_dirty = (uint8_t*)calloc(n, 1);
        g_extra = (uint32_t*)malloc(n * sizeof *g_extra);
        g_held = (uint32_t*)malloc(n * sizeof *g_held);
        g_runs = (uint32_t*)malloc(n * sizeof *g_runs);
        if (g_mem == MAP_FAILED || !g_incall || !g_prot || !g_valid || !g_dirty || !g_extra || !g_held || !g_runs) return 0;
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    signal(SIGVTALRM, on_timeout);
    g_s.mem = g_mem;
    /* every entry comes to soa_swap_enter, which tells it to the trace and the candidate */
    {
        uint32_t i;
        for (i = 0; i < g_game->count; i++)
            if (!g_game->kind[i]) g_game->swap[i] = SWAP_TRACE;
    }
    return 1;
}

/* ---- holding a call to its recording ---- */

typedef struct {
    int show, shown, lines, partial, undone;
    uint64_t bytes, regs, undone_bytes;
} Hold;

/* Where what candidates left undone is, by 32-byte block: an open table. */
#define UNDONE_SLOTS 16384u
static struct {
    uint32_t block, calls, bytes, last;
} g_undone[UNDONE_SLOTS];
static uint32_t g_nundone, g_undone_call;

static void left_undone(uint32_t off)
{
    uint32_t block = off >> 5, i = (block * 2654435761u) % UNDONE_SLOTS, n;
    for (n = 0; n < UNDONE_SLOTS; n++, i = (i + 1) % UNDONE_SLOTS) {
        if (g_undone[i].calls && g_undone[i].block != block) continue;
        if (!g_undone[i].calls) {
            if (g_nundone >= UNDONE_SLOTS / 2) return;
            g_nundone++;
            g_undone[i].block = block;
        }
        g_undone[i].bytes++;
        if (!g_undone[i].calls || g_undone[i].last != g_undone_call) g_undone[i].calls++, g_undone[i].last = g_undone_call;
        return;
    }
}

static int by_block(const void* a, const void* b)
{
    const uint32_t x = ((const uint32_t*)a)[0], y = ((const uint32_t*)b)[0];
    return x < y ? -1 : x > y;
}

/* The stretches left undone, blocks within 64 bytes of each other taken as one. */
static void tell_undone(void)
{
    uint32_t (*list)[3] = (uint32_t(*)[3])malloc((g_nundone + 1) * sizeof *list);
    uint32_t i, n = 0, told = 0;
    if (!list) return;
    for (i = 0; i < UNDONE_SLOTS; i++)
        if (g_undone[i].calls) list[n][0] = g_undone[i].block, list[n][1] = g_undone[i].bytes, list[n][2] = g_undone[i].calls, n++;
    qsort(list, n, sizeof *list, by_block);
    if (n) printf("  left undone, in stretches (bytes; the most calls a block of it was left in):\n");
    for (i = 0; i < n && told < 24;) {
        uint32_t j = i, bytes = list[i][1], most = list[i][2];
        while (j + 1 < n && list[j + 1][0] <= list[j][0] + 2) j++, bytes += list[j][1], most = list[j][2] > most ? list[j][2] : most;
        printf("    %08X..%08X: %u bytes, in up to %u calls\n", 0x80000000u + (list[i][0] << 5),
               0x80000000u + ((list[j][0] + 1) << 5) - 1, bytes, most);
        i = j + 1;
        told++;
    }
    if (told == 24 && i < n) printf("    ... and more\n");
    free(list);
}

#define LINES_A_CALL 24

static int bits_eq(double a, double b)
{
    return memcmp(&a, &b, sizeof a) == 0;
}

static void tell(Hold* h, int* said, const Call* c, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
static void tell(Hold* h, int* said, const Call* c, const char* fmt, ...)
{
    va_list ap;
    if (h->shown >= h->show) return;
    if (!*said) {
        printf("  call %llu of %08X (frame %llu):\n", (unsigned long long)c->head.seen, c->head.address,
               (unsigned long long)c->head.frame);
        *said = 1;
        h->lines = 0;
    }
    if (++h->lines > LINES_A_CALL) {
        if (h->lines == LINES_A_CALL + 1) printf("    ... and more\n");
        return;
    }
    printf("    ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/* Every difference; 0 for none. */
static int held(Hold* h, const Call* c, int original, uint32_t results, uint32_t sp)
{
    RecRegs now;
    const RecRegs* want = &c->head.out;
    int off = 0, said = 0, r;
    /* the word 4 bytes over r1 is the caller's frame's, where a callee keeps its return address */
    uint32_t k, ps = g_page, lr_lo = original ? 0 : (sp & MEM_MASK) + 4u, lr_hi = original ? 0 : lr_lo + 4u;
    rec_regs_from(&now, &g_s);
    /* the registers */
    for (r = 0; r < 32; r++) {
        int keep = r == 1 || r == 2 || r >= 13;
        if (original || keep || (r == 3 && (results & SOA_REPLAY_R3)) || (r == 4 && (results & SOA_REPLAY_R4))) {
            h->regs++;
            if (now.gpr[r] != want->gpr[r]) {
                off++;
                tell(h, &said, c, "r%d %08X, the game's %08X", r, now.gpr[r], want->gpr[r]);
            }
        }
    }
    for (r = 0; r < 32; r++) {
        int keep = r >= 14;
        if (original || keep || (r == 1 && (results & SOA_REPLAY_F1))) {
            /* a result is ps0, and so is a register the caller's: the game's own save and restore of f14-f31
             * is stfd and lfd, which keep ps0 alone, and its paired-single code leaves ps1 as it may */
            int both = original;
            h->regs++;
            if (!bits_eq(now.fpr[r][0], want->fpr[r][0]) || (both && !bits_eq(now.fpr[r][1], want->fpr[r][1]))) {
                off++;
                tell(h, &said, c, "f%d %.9g/%.9g, the game's %.9g/%.9g", r, now.fpr[r][0], now.fpr[r][1],
                     want->fpr[r][0], want->fpr[r][1]);
            }
        }
    }
    {
        uint32_t mask = original ? 0xFFFFFFFFu : 0x00FFF000u; /* CR2-CR4 are the caller's */
        h->regs++;
        if ((now.cr & mask) != (want->cr & mask)) {
            off++;
            tell(h, &said, c, "cr %08X, the game's %08X", now.cr, want->cr);
        }
    }
    if (original) {
        const uint32_t* a = &now.cr;
        const uint32_t* b = &want->cr;
        static const char* names[] = {"cr", "xer", "lr", "ctr", "fpscr", "msr"};
        for (r = 1; r < 6; r++) {
            h->regs++;
            if (a[r] != b[r]) {
                off++;
                tell(h, &said, c, "%s %08X, the game's %08X", names[r], a[r], b[r]);
            }
        }
        for (r = 0; r < 8; r++) {
            h->regs++;
            if (now.gqr[r] != want->gqr[r]) {
                off++;
                tell(h, &said, c, "gqr%d %08X, the game's %08X", r, now.gqr[r], want->gqr[r]);
            }
        }
    }
    /* the pages the game's call touched: every byte as it left them, but the stack under it */
    for (k = 0; k < c->head.pages; k++) {
        uint32_t p = c->page[k];
        const uint8_t* got = g_mem + (size_t)p * ps;
        const uint8_t* exp = c->post[k];
        const uint8_t* pre = c->pre[k];
        uint32_t base = p * ps, i = 0;
        h->bytes += ps;
        /* a page neither the replay nor the game's call wrote is as the call found it, and as it left it */
        if (!g_dirty[p] && !c->post_n[k]) {
            g_valid[p] = 1;
            continue;
        }
        if (!memcmp(got, exp, ps)) {
            g_valid[p] = 1;
            continue;
        }
        g_valid[p] = 0;
        while (i < ps) {
            uint32_t o = base + i, j;
            if (got[i] == exp[i] || (o >= g_scratch_lo && o < g_scratch_hi) || (o >= lr_lo && o < lr_hi)) {
                i++;
                continue;
            }
            if (h->partial && got[i] == pre[i]) { /* the game wrote it; the candidate left it */
                h->undone = 1;
                h->undone_bytes++;
                left_undone(o);
                i++;
                continue;
            }
            for (j = i; j < ps && j < i + 16 && got[j] != exp[j] && !(h->partial && got[j] == pre[j]); j++) {}
            {
                char a[40], b[40];
                uint32_t q;
                for (q = 0; q < j - i; q++) sprintf(a + 2 * q, "%02X", got[i + q]), sprintf(b + 2 * q, "%02X", exp[i + q]);
                off++;
                tell(h, &said, c, "%08X+%u: %s, the game's %s", 0x80000000u + o, j - i, a, b);
            }
            i = j;
        }
    }
    /* and nothing written where the game's call did not touch */
    for (k = 0; k < g_nextra; k++) {
        const uint8_t* got = g_mem + (size_t)g_extra[k] * ps;
        uint32_t base = g_extra[k] * ps, i;
        for (i = 0; i < ps; i++) {
            uint32_t o = base + i;
            if (got[i] && !(o >= g_scratch_lo && o < g_scratch_hi)) {
                off++;
                tell(h, &said, c, "%08X: written, where the game's call touched nothing", 0x80000000u + o);
                break;
            }
        }
    }
    if (g_stores_off) {
        off++;
        tell(h, &said, c, "%u stores to the hardware not the game's call's: the first %08X = %llX", g_stores_off,
             g_store_ea, (unsigned long long)g_store_v);
    }
    for (k = g_input_at; k < c->head.inputs; k++)
        if (REC_KIND(c->inputs[k].kind) == REC_MMIO_STORE) {
            off++;
            tell(h, &said, c, "the game's call stored %llX to the hardware at %08X, and this did not",
                 (unsigned long long)c->inputs[k].value, c->inputs[k].ea);
            break;
        }
    if (g_outside_n) {
        off++;
        tell(h, &said, c, "%s %08X, a page the game's call never touched (%u such touches)",
             g_outside_storing == 1 ? "a store to" : g_outside_storing == 0 ? "a load from" : "a touch of",
             0x80000000u + g_outside_at, g_outside_n);
    }
    /* the translation calls what the game's did, in order */
    if (original) {
        int same = g_ncalled == c->head.callees;
        for (k = 0; same && k < g_ncalled; k++)
            same = g_called[k].address == c->callees[k].address && g_called[k].sp == c->callees[k].sp;
        if (!same && !(c->head.flags & REC_MANY_CALLEES)) {
            off++;
            tell(h, &said, c, "%u calls made, the game's call made %u (or not the same ones)", g_ncalled,
                 c->head.callees);
        }
    }
    if (said) h->shown++;
    return off;
}

/* ---- the replay ---- */

static int ascending(const void* a, const void* b)
{
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}

/* The call's pages as they were before it, and nothing else of memory to be
 * had: the pages the last call had that this one does not are taken away,
 * the ones it has that are not as they were are written, and all of its own
 * are left readable, so that a store to one is seen. */
static void put(const Call* c)
{
    uint32_t k, n = 0;
    for (k = 0; k < g_nheld; k++) g_incall[g_held[k]] = 0;
    for (k = 0; k < c->head.pages; k++) g_incall[c->page[k]] = 1;
    for (k = 0; k < g_nheld; k++)
        if (!g_incall[g_held[k]]) g_runs[n++] = g_held[k];
    qsort(g_runs, n, sizeof *g_runs, ascending);
    protect(g_runs, n, 0);
    for (k = 0, n = 0; k < c->head.pages; k++)
        if (!c->here[k]) g_runs[n++] = c->page[k];
    protect(g_runs, n, 2);
    for (k = 0; k < c->head.pages; k++) {
        uint32_t p = c->page[k];
        if (!c->here[k]) memcpy(g_mem + (size_t)p * g_page, c->pre[k], g_page);
        g_dirty[p] = 0;
        g_held[k] = p;
    }
    g_nheld = c->head.pages;
    protect(c->page, c->head.pages, 1);
    g_nextra = 0;
}

/* After the call: its touches outside its pages are held with the rest. */
static void keep_touches(void)
{
    uint32_t k;
    for (k = 0; k < g_nextra; k++) g_held[g_nheld++] = g_extra[k];
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* The call, on the state made for it, ended early by stop() or the timer. */
static void run_call(uint32_t address)
{
    struct itimerval tv;
    memset(&tv, 0, sizeof tv);
    tv.it_value.tv_sec = 5;
    setitimer(ITIMER_VIRTUAL, &tv, NULL);
    if (sigsetjmp(g_jmp, 1) == 0) {
        g_in_call = 1;
        g_s.pc = address;
        g_game->dispatch(&g_s, address);
    }
    g_in_call = 0;
    memset(&tv, 0, sizeof tv);
    setitimer(ITIMER_VIRTUAL, &tv, NULL);
}

EXPORT int soa_replay(const SoaReplayRun* run, SoaReplayTotals* t)
{
    Rec r;
    Call c;
    size_t at;
    Hold h;
    uint32_t scratch = run->stack_scratch ? run->stack_scratch : 0x10000u;
    double t0;
    const char* reasons[16];
    uint64_t reason_n[16];
    unsigned nreasons = 0, k;
    memset(t, 0, sizeof *t);
    memset(&c, 0, sizeof c);
    memset(&h, 0, sizeof h);
    h.show = run->show;
    h.partial = run->partial && !run->original;
    memset(g_undone, 0, sizeof g_undone);
    g_nundone = 0;
    if (!rec_open(&r, run->recording)) return 0;
    if (!setup(run->game, r.head.page_size)) return 0;
    memset(g_valid, 0, RESERVE / g_page + 1); /* a replay before this one left memory as its own last call did */
    if (r.head.functions != g_game->count || strncmp(r.head.game, g_game->record ? g_game->record : "", sizeof r.head.game - 1))
        fprintf(stderr, "[replay] the recording was made by another build (%u functions, \"%s\"); this game library has %u (\"%s\")\n",
                r.head.functions, r.head.game, g_game->count, g_game->record ? g_game->record : "");
    g_cand = run->candidate;
    g_cand_index = g_cand ? index_of(g_cand->address) : -1;
    if (g_cand && g_cand_index < 0) {
        fprintf(stderr, "[replay] %s: %08X is not a translated function of this game library\n", g_cand->name, g_cand->address);
        return 0;
    }
    g_answered = g_games = g_both = g_live = 0;
    at = sizeof r.head;
    t0 = now_s();
    while (rec_next(&r, &at, &c)) {
        uint32_t want = g_cand ? g_cand->address : run->only;
        uint32_t sp = c.head.in.gpr[1], spo = sp & MEM_MASK, i;
        int off;
        if (want && c.head.address != want) continue;
        if (c.head.flags & (REC_IRQ | REC_SLEPT | REC_FOREIGN | REC_MANY_INPUTS)) {
            if (!run->with_flagged) {
                t->flagged_skipped++;
                continue;
            }
        }
        t->calls++;
        put(&c);
        /* the stack under the call is the candidate's scratch; the translation is held to it too */
        g_scratch_lo = run->original ? 0 : spo > scratch ? spo - scratch : 0;
        g_scratch_hi = run->original ? 0 : spo;
        g_outside_n = 0;
        memset(&g_s, 0, offsetof(CpuState, mem));
        rec_regs_to(&g_s, &c.head.in);
        g_s.mem = g_mem;
        g_inputs = c.inputs;
        g_ninputs = c.head.inputs;
        g_input_at = 0;
        g_input_bad = 0;
        g_stores_off = 0;
        g_seen = c.head.seen;
        g_frame = c.head.frame;
        g_ncalled = 0;
        g_trace = 1;
        g_root = 1;
        g_skip = 0;
        g_why = NULL;
        run_call(c.head.address);
        g_trace = 0;
        if (g_why) {
            for (k = 0; k < nreasons && strcmp(reasons[k], g_why); k++) {}
            if (k == nreasons && nreasons < 16) reasons[nreasons] = g_why, reason_n[nreasons++] = 0;
            if (k < 16) reason_n[k]++;
            t->unreplayable++;
            if (!run->quiet && h.shown < h.show) {
                printf("  call %llu of %08X (frame %llu): not replayable: %s at %08X\n", (unsigned long long)c.head.seen,
                       c.head.address, (unsigned long long)c.head.frame, g_why, g_why_pc);
                h.shown++;
            }
            keep_touches();
            for (i = 0; i < c.head.pages; i++) g_valid[c.page[i]] = !g_dirty[c.page[i]] && !c.post_n[i];
            continue;
        }
        h.undone = 0;
        g_undone_call++;
        off = held(&h, &c, run->original, g_cand ? g_cand->results : 0u, sp);
        if (off) t->mismatched++;
        else if (h.undone) t->partial++;
        else t->matched++;
        keep_touches();
    }
    t->seconds = now_s() - t0;
    t->answered = g_answered, t->games = g_games, t->both = g_both, t->live = g_live;
    t->bytes_held = h.bytes, t->regs_held = h.regs;
    t->undone_bytes = h.undone_bytes;
    if (h.partial) tell_undone();
    for (k = 0; k < nreasons; k++)
        printf("  not replayable: %llu calls met %s\n", (unsigned long long)reason_n[k], reasons[k]);
    free(c.page), free(c.flags), free(c.post_n), free(c.here), free((void*)c.pre), free((void*)c.post), free(c.buf);
    free(r.data), free(r.shadow), free(r.zero);
    return 1;
}

EXPORT int soa_replay_info(const char* recording, uint32_t only, int64_t seen)
{
    Rec r;
    Call c;
    size_t at;
    struct {
        uint32_t address;
        uint64_t calls, flagged, pages, written, inputs, callees, bytes;
        uint64_t irq, slept, foreign, many;
    } fn[64];
    unsigned nfn = 0, k;
    memset(&c, 0, sizeof c);
    if (!rec_open(&r, recording)) return 0;
    printf("%s: %zu bytes, pages of %u bytes, made by \"%s\" (%u functions)\n", recording, r.size, r.head.page_size,
           r.head.game, r.head.functions);
    at = sizeof r.head;
    while (1) {
        size_t was = at;
        uint32_t w = 0, i;
        if (!rec_next(&r, &at, &c)) break;
        for (k = 0; k < nfn && fn[k].address != c.head.address; k++) {}
        if (k == nfn) {
            if (nfn == 64) continue;
            memset(&fn[nfn], 0, sizeof fn[nfn]);
            fn[nfn++].address = c.head.address;
        }
        for (i = 0; i < c.head.pages; i++) w += (c.flags[i] & REC_PAGE_WRITTEN) != 0;
        if (seen >= 0 && c.head.seen == (uint64_t)seen && (!only || c.head.address == only)) {
            printf("  call %llu of %08X (frame %llu) made %u calls:\n", (unsigned long long)c.head.seen, c.head.address,
                   (unsigned long long)c.head.frame, c.head.callees);
            for (i = 0; i < c.head.callees; i++) printf("    %08X  r1 %08X\n", c.callees[i].address, c.callees[i].sp);
        }
        fn[k].calls++;
        fn[k].flagged += c.head.flags != 0;
        fn[k].irq += (c.head.flags & REC_IRQ) != 0;
        fn[k].slept += (c.head.flags & REC_SLEPT) != 0;
        fn[k].foreign += (c.head.flags & REC_FOREIGN) != 0;
        fn[k].many += (c.head.flags & (REC_MANY_INPUTS | REC_MANY_CALLEES)) != 0;
        fn[k].pages += c.head.pages;
        fn[k].written += w;
        fn[k].inputs += c.head.inputs;
        fn[k].callees += c.head.callees;
        fn[k].bytes += at - was;
    }
    if (at != r.size) printf("  the file ends part-way into a call at byte %zu (a run cut short): the calls before it are read\n", at);
    for (k = 0; k < nfn; k++)
        printf("  %08X: %llu calls (%llu with a flag: %llu an interrupt inside, %llu slept, %llu another thread's touch, "
               "%llu too many accesses or calls to keep); a call touches %.1f pages (%.1f written), makes %.1f hardware "
               "and timebase accesses and %.1f calls; %.0f bytes a call\n",
               fn[k].address, (unsigned long long)fn[k].calls, (unsigned long long)fn[k].flagged,
               (unsigned long long)fn[k].irq, (unsigned long long)fn[k].slept, (unsigned long long)fn[k].foreign,
               (unsigned long long)fn[k].many,
               (double)fn[k].pages / fn[k].calls, (double)fn[k].written / fn[k].calls, (double)fn[k].inputs / fn[k].calls,
               (double)fn[k].callees / fn[k].calls, (double)fn[k].bytes / fn[k].calls);
    free(c.page), free(c.flags), free(c.post_n), free(c.here), free((void*)c.pre), free((void*)c.post), free(c.buf);
    free(r.data), free(r.shadow), free(r.zero);
    return 1;
}
