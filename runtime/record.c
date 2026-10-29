/*
 * Real calls of chosen functions recorded during a run (SOA_RECORD): what
 * and why in record.h and docs/RECORD.md, the replay in tools/replay/.
 *
 * A recorded call runs with MEM1 made unreadable. The first touch of a page
 * faults here: the page is copied as it is and opened for reading; the first
 * store to it faults again and opens it for writing. So the pages a call and
 * everything it calls touch are known, each one's bytes before the call are
 * kept, and which of them it may have written. The guard is the swap's: a
 * SWAP_BOTH replacement on each function asked for, which the guest's
 * thread meets at the function's entry, before (after 0) and after (after
 * 1) its translation runs. The fault handler is chained in front of
 * plat.c's guard on the memory above the RAM and hands it everything else.
 */
#include "record.h"
#include "gx_state.h"
#include "plat.h"
#include "soa_game.h"
#include "swap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

extern volatile int g_record_armed;                           /* hle.c */
extern unsigned g_record_polls;                               /* hle.c, for irq.c */
extern void (*g_record_note)(CpuState* s, int kind, uint32_t ea, uint64_t v); /* hle.c */
uint64_t irq_delivered_count(unsigned number);                /* irq.c */
uint64_t threads_switch_count(void);                          /* threads.c */
unsigned gx_frame_count(void);                                /* gx.c */
void hle_on_report(void (*fn)(void));                         /* hle.c */
extern void (*g_record_pipe)(CpuState* s, unsigned size, uint64_t v); /* gx.c */
void gx_pipe_watch_changed(void);                             /* gx.c */

#define MAX_FUNCTIONS 64

typedef struct {
    uint32_t address;
    uint64_t seen, recorded, flagged, dropped;
    uint64_t drew, pipe_bytes; /* recorded calls that wrote the gather pipe, and the bytes */
    uint32_t drop_ea, drop_pc; /* the first hardware access that dropped a call, and the block it was in */
} Fn;

static Fn g_fn[MAX_FUNCTIONS];
static unsigned g_nfn, g_left; /* functions asked for; those still short of the limit */
static uint64_t g_limit = 1000, g_every = 1;
static FILE* g_out;
static const char* g_path;
static uint64_t g_bytes;

static uint8_t* g_mem;
static uint32_t g_page, g_npages;
static uint8_t* g_state;     /* a page's: 0 untouched, 1 read, 2 written (this call) */
static uint32_t* g_touched;  /* the pages touched, in the order they were */
static uint8_t* g_pool;      /* their bytes before the call, in the same order */
static uint32_t g_ntouched, g_max_pages = 4096;
static uint8_t* g_shadow;    /* each page's last image in the file */
static uint8_t* g_shadowed;  /* 1: g_shadow has it */
static uint8_t* g_zero;
static uint8_t* g_named;     /* 1: SOA_RECORD_WITH names the page */
static uint32_t g_nnamed;
static int g_all;            /* SOA_RECORD_WITH=all */

static Fn* volatile g_active; /* the call being recorded */
static volatile int g_protected, g_dropped;
static volatile uint32_t g_flags;
static pthread_t g_owner;
static RecCallHead g_head;
static RecInput g_in[REC_MAX_INPUTS];
static RecCallee g_callee[REC_MAX_CALLEES];
static uint32_t g_nin, g_ncallee;
static uint64_t g_irq0, g_switch0;
static struct sigaction g_prev;
static double g_t0, g_spent, g_most; /* seconds a recorded call took, from its entry to its record written */
static uint8_t* g_buf; /* a call's encoded pages, before they are written */

/* The GX part (record.h): the pipe's bytes the call wrote, and GX's state as
 * it found and left it. */
#define PIPE_MAX (64u << 20) /* the bytes one call may write to the pipe and be kept */
static GxImage g_gx_pre, g_gx_post, g_gx_shadow; /* the shadow: the last image in the file */
static uint8_t* g_pipe_log;
static size_t g_pipe_n, g_pipe_cap;
static uint8_t g_wait[GX_PIPE_CAP]; /* the bytes of a command not yet whole, at the call's entry */
static uint32_t g_nwait;
static int g_gx_part, g_gx_access;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}
static size_t g_buf_cap;

static uint64_t irq_total(void)
{
    uint64_t n = 0;
    unsigned k;
    for (k = 0; k < 32; k++) n += irq_delivered_count(k);
    return n;
}

static void open_all(void)
{
    mprotect(g_mem, MEM1_SIZE, PROT_READ | PROT_WRITE);
    g_protected = 0;
}

static void chain(int sig, siginfo_t* si, void* uc)
{
    if ((g_prev.sa_flags & SA_SIGINFO) && g_prev.sa_sigaction) {
        g_prev.sa_sigaction(sig, si, uc);
        return;
    }
    if (!(g_prev.sa_flags & SA_SIGINFO) && g_prev.sa_handler != SIG_DFL && g_prev.sa_handler != SIG_IGN) {
        g_prev.sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL); /* the access runs again and ends the process as it would have */
}

static void on_fault(int sig, siginfo_t* si, void* uc)
{
    uintptr_t off = (uintptr_t)si->si_addr - (uintptr_t)g_mem;
    if (g_protected && off < MEM1_SIZE) {
        uint32_t p = (uint32_t)(off / g_page);
        uint8_t* at = g_mem + (size_t)p * g_page;
        if (!pthread_equal(pthread_self(), g_owner)) g_flags |= REC_FOREIGN;
        if (g_state[p] == 0) {
            if (g_ntouched >= g_max_pages) { /* more than the pool holds: the call is not kept */
                g_dropped = 2;
                open_all();
                return;
            }
            mprotect(at, g_page, PROT_READ);
            memcpy(g_pool + (size_t)g_ntouched * g_page, at, g_page);
            g_touched[g_ntouched++] = p;
            g_state[p] = 1;
            return;
        }
        if (g_state[p] == 1) {
            mprotect(at, g_page, PROT_READ | PROT_WRITE);
            g_state[p] = 2;
            return;
        }
    }
    chain(sig, si, uc);
}

static void on_note(CpuState* s, int kind, uint32_t ea, uint64_t v)
{
    if (!g_active || g_dropped) return;
    if ((REC_KIND(kind) == REC_MMIO_STORE || REC_KIND(kind) == REC_IN_MMIO) &&
        ((ea >= 0xCC000000u && ea < 0xCC000080u) ||
         (ea >= 0xCC001000u && ea < 0xCC001010u) ||
         (ea >= 0xCC00300Cu && ea <= 0xCC003014u))) g_gx_access = 1;
    if (REC_KIND(kind) == REC_MMIO_STORE && (ea & 0xFFFFFC00u) == 0xCC006000u) {
        /* the disc's interface: a read it starts goes through the kernel into memory. Open it all, and the
         * call is not kept */
        if (!g_active->drop_ea) g_active->drop_ea = ea, g_active->drop_pc = s->pc;
        g_dropped = 1;
        g_record_armed = 0;
        open_all();
        return;
    }
    if (g_nin < REC_MAX_INPUTS) {
        g_in[g_nin].kind = (uint32_t)kind;
        g_in[g_nin].ea = ea;
        g_in[g_nin].value = v;
        g_nin++;
    } else
        g_flags |= REC_MANY_INPUTS;
}

static void on_entry(CpuState* s, uint32_t address)
{
    if (!g_active) return;
    if (g_ncallee < REC_MAX_CALLEES) {
        g_callee[g_ncallee].address = address;
        g_callee[g_ncallee].sp = s->gpr[1];
        g_ncallee++;
    } else
        g_flags |= REC_MANY_CALLEES;
}

static void on_pipe(CpuState* s, unsigned size, uint64_t v)
{
    unsigned i;
    (void)s;
    if (!g_active || g_dropped) return;
    if (g_pipe_n + size > g_pipe_cap) {
        size_t cap = g_pipe_cap ? g_pipe_cap * 2 : 1u << 16;
        uint8_t* b;
        while (cap < g_pipe_n + size) cap *= 2;
        b = g_pipe_n + size <= PIPE_MAX ? (uint8_t*)realloc(g_pipe_log, cap) : NULL;
        if (!b) { /* more than is kept: the call is not */
            g_dropped = 3;
            return;
        }
        g_pipe_log = b;
        g_pipe_cap = cap;
    }
    for (i = 0; i < size; i++) g_pipe_log[g_pipe_n++] = (uint8_t)(v >> (8 * (size - 1 - i)));
}

static void begin(CpuState* s, Fn* f, uint64_t seen)
{
    g_t0 = now_s();
    memset(&g_head, 0, sizeof g_head);
    g_head.magic = REC_CALL_MAGIC;
    g_head.address = f->address;
    g_head.seen = seen;
    g_head.frame = gx_frame_count();
    rec_regs_from(&g_head.in, s);
    g_irq0 = irq_total();
    g_switch0 = threads_switch_count();
    g_nin = g_ncallee = g_ntouched = 0;
    g_gx_access = 0;
    g_flags = 0;
    g_dropped = 0;
    g_owner = pthread_self();
    g_active = f;
    {
        const uint8_t* wait = gx_pipe_left(&g_nwait);
        gx_image(&g_gx_pre);
        if (g_nwait > sizeof g_wait) g_nwait = sizeof g_wait;
        memcpy(g_wait, wait, g_nwait);
        g_pipe_n = 0;
        g_record_pipe = on_pipe;
        gx_pipe_watch_changed();
    }
    swap_trace(on_entry);
    g_record_note = on_note;
    g_record_polls = 0;
    g_record_armed = 1;
    g_protected = 1;
    if (g_all) { /* every page kept now, and opened for reading: only a store faults */
        uint32_t p;
        mprotect(g_mem, MEM1_SIZE, PROT_READ);
        memcpy(g_pool, g_mem, MEM1_SIZE);
        for (p = 0; p < g_npages; p++) g_touched[p] = p, g_state[p] = 1;
        g_ntouched = g_npages;
        g_head.named = g_npages;
        return;
    }
    mprotect(g_mem, MEM1_SIZE, PROT_NONE);
    if (g_nnamed) { /* the pages SOA_RECORD_WITH names, kept as a first touch would keep them */
        uint32_t p;
        for (p = 0; p < g_npages; p++) {
            uint8_t* at = g_mem + (size_t)p * g_page;
            if (!g_named[p]) continue;
            mprotect(at, g_page, PROT_READ);
            memcpy(g_pool + (size_t)g_ntouched * g_page, at, g_page);
            g_touched[g_ntouched++] = p;
            g_state[p] = 1;
        }
        g_head.named = g_ntouched;
    }
}

/* SOA_RECORD_WITH: lo-hi and lo+bytes ranges, or all. */
static int read_with(const char* e)
{
    const char* p = e;
    if (!strcmp(e, "all")) {
        g_all = 1;
        return 1;
    }
    while (*p) {
        char* end;
        unsigned long lo = strtoul(p, &end, 0), hi;
        uint32_t a, b;
        if (end == p) return 0;
        p = end;
        if (*p == '-') hi = strtoul(p + 1, &end, 0);
        else if (*p == '+') hi = lo + strtoul(p + 1, &end, 0);
        else return 0;
        if (end == p + 1) return 0;
        p = end;
        if (hi < lo) return 0;
        a = (uint32_t)lo & MEM_MASK;
        b = hi - lo > MEM1_SIZE ? MEM1_SIZE : a + (uint32_t)(hi - lo);
        if (b > MEM1_SIZE) b = MEM1_SIZE;
        for (; a < b; a = (a / g_page + 1) * g_page)
            if (!g_named[a / g_page]) g_named[a / g_page] = 1, g_nnamed++;
        while (*p == ',' || *p == ' ') p++;
    }
    return 1;
}

static int grow(size_t need)
{
    uint8_t* b;
    size_t cap = g_buf_cap ? g_buf_cap : 1u << 20;
    if (need <= g_buf_cap) return 1;
    while (cap < need) cap *= 2;
    b = (uint8_t*)realloc(g_buf, cap);
    if (!b) return 0;
    g_buf = b;
    g_buf_cap = cap;
    return 1;
}

static int by_page(const void* a, const void* b)
{
    uint32_t x = g_touched[*(const uint32_t*)a], y = g_touched[*(const uint32_t*)b];
    return x < y ? -1 : x > y;
}

/* The call's record, written: its pages in address order. */
static int write_call(void)
{
    static uint32_t* order;
    static uint32_t order_cap;
    size_t n = 0;
    uint32_t k;
    if (order_cap < g_ntouched) {
        free(order);
        order = (uint32_t*)malloc(g_max_pages * sizeof *order);
        if (!order) return 0;
        order_cap = g_max_pages;
    }
    for (k = 0; k < g_ntouched; k++) order[k] = k;
    qsort(order, g_ntouched, sizeof *order, by_page);
    for (k = 0; k < g_ntouched; k++) {
        uint32_t i = order[k], p = g_touched[i];
        const uint8_t* pre = g_pool + (size_t)i * g_page;
        const uint8_t* post = g_mem + (size_t)p * g_page;
        uint8_t* shadow = g_shadow + (size_t)p * g_page;
        RecPage pg;
        if (!grow(n + sizeof pg + 4u * g_page + 16u)) return 0;
        pg.page = p;
        pg.flags = REC_PAGE_READ | (g_state[p] == 2 ? REC_PAGE_WRITTEN : 0u);
        pg.pre_bytes = rec_encode(pre, g_shadowed[p] ? shadow : g_zero, g_page, g_buf + n + sizeof pg);
        pg.post_bytes = g_state[p] == 2 ? rec_encode(post, pre, g_page, g_buf + n + sizeof pg + pg.pre_bytes) : 0u;
        memcpy(g_buf + n, &pg, sizeof pg);
        n += sizeof pg + pg.pre_bytes + pg.post_bytes;
        memcpy(shadow, post, g_page);
        g_shadowed[p] = 1;
    }
    if (!grow(n + g_nin * sizeof(RecInput) + g_ncallee * sizeof(RecCallee))) return 0;
    memcpy(g_buf + n, g_in, g_nin * sizeof(RecInput));
    n += g_nin * sizeof(RecInput);
    memcpy(g_buf + n, g_callee, g_ncallee * sizeof(RecCallee));
    n += g_ncallee * sizeof(RecCallee);
    g_head.gx = (uint32_t)g_gx_part;
    if (g_gx_part) {
        const uint32_t img = (uint32_t)sizeof(GxImage);
        if (!grow(n + g_pipe_n + g_nwait + 4u * img + 16u)) return 0;
        memcpy(g_buf + n, g_pipe_log, g_pipe_n);
        n += g_pipe_n;
        memcpy(g_buf + n, g_wait, g_nwait);
        n += g_nwait;
        g_head.pipe_bytes = (uint32_t)g_pipe_n;
        g_head.pipe_left = g_nwait;
        g_head.gx_pre_bytes = rec_encode((const uint8_t*)&g_gx_pre, (const uint8_t*)&g_gx_shadow, img, g_buf + n);
        n += g_head.gx_pre_bytes;
        g_head.gx_post_bytes = rec_encode((const uint8_t*)&g_gx_post, (const uint8_t*)&g_gx_pre, img, g_buf + n);
        n += g_head.gx_post_bytes;
        g_gx_shadow = g_gx_post;
    }
    g_head.pages = g_ntouched;
    g_head.inputs = g_nin;
    g_head.callees = g_ncallee;
    g_head.bytes = (uint32_t)(sizeof g_head + n);
    if (fwrite(&g_head, sizeof g_head, 1, g_out) != 1 || (n && fwrite(g_buf, n, 1, g_out) != 1)) return 0;
    fflush(g_out); /* a run killed part-way keeps every call written before */
    g_bytes += g_head.bytes;
    return 1;
}

static void end(CpuState* s, Fn* f)
{
    uint32_t k;
    open_all();
    g_record_armed = 0;
    g_active = NULL;
    g_record_pipe = NULL;
    gx_pipe_watch_changed();
    gx_image(&g_gx_post);
    g_gx_part = g_gx_access || g_pipe_n || memcmp(&g_gx_pre, &g_gx_post, sizeof g_gx_post) != 0;
    swap_trace(NULL);
    rec_regs_from(&g_head.out, s);
    if (irq_total() != g_irq0) g_flags |= REC_IRQ;
    if (threads_switch_count() != g_switch0) g_flags |= REC_SLEPT;
    g_head.flags = g_flags;
    if (g_dropped) f->dropped++;
    else if (!write_call()) {
        fprintf(stderr, "[record] cannot write %s; recording stops\n", g_path);
        g_left = 0;
    } else {
        f->recorded++;
        f->flagged += g_flags != 0;
        f->drew += g_pipe_n != 0;
        f->pipe_bytes += g_pipe_n;
    }
    for (k = 0; k < g_ntouched; k++) g_state[g_touched[k]] = 0;
    {
        double took = now_s() - g_t0;
        g_spent += took;
        if (took > g_most) g_most = took;
    }
    if (f->recorded >= g_limit && g_left) {
        swap_set(f->address, NULL, NULL, NULL);
        g_left--;
    }
    if (!g_left)
        for (k = 0; k < g_nfn; k++) swap_set(g_fn[k].address, NULL, NULL, NULL);
}

static int on_call(CpuState* s, uint32_t address, SwapFn fn, void* user, int after)
{
    Fn* f = (Fn*)user;
    uint64_t n;
    (void)address;
    (void)fn;
    if (after) {
        if (g_active == f) end(s, f);
        return SWAP_GAMES;
    }
    n = f->seen++;
    if (g_active || !g_left) return SWAP_GAMES; /* inside a call being recorded: part of it */
    if (f->recorded >= g_limit || n % g_every) return SWAP_GAMES;
    begin(s, f, n);
    return SWAP_BOTH;
}

static void report(void)
{
    unsigned k;
    if (g_out) fflush(g_out);
    for (k = 0; k < g_nfn; k++)
        fprintf(stderr, "[record] %08X: %llu calls seen, %llu recorded (%llu with a flag), %llu dropped (a store to the disc's interface, more than %u pages, or more than 64 MB to the pipe)\n",
                g_fn[k].address, (unsigned long long)g_fn[k].seen, (unsigned long long)g_fn[k].recorded,
                (unsigned long long)g_fn[k].flagged, (unsigned long long)g_fn[k].dropped, g_max_pages);
    for (k = 0; k < g_nfn; k++)
        if (g_fn[k].drew)
            fprintf(stderr, "[record] %08X: %llu of the recorded calls wrote the gather pipe, %llu bytes in all, kept with them\n",
                    g_fn[k].address, (unsigned long long)g_fn[k].drew, (unsigned long long)g_fn[k].pipe_bytes);
    for (k = 0; k < g_nfn; k++)
        if (g_fn[k].drop_ea)
            fprintf(stderr, "[record] %08X: the first call dropped stored to the disc's interface at %08X, from block %08X\n",
                    g_fn[k].address, g_fn[k].drop_ea, g_fn[k].drop_pc);
    {
        uint64_t calls = 0;
        for (k = 0; k < g_nfn; k++) calls += g_fn[k].recorded + g_fn[k].dropped;
        fprintf(stderr, "[record] %llu bytes in %s; a recorded call took %.1f us on average, %.1f us at most (%.3f s in all)\n",
                (unsigned long long)g_bytes, g_path, calls ? g_spent / (double)calls * 1e6 : 0.0, g_most * 1e6, g_spent);
    }
}

static const SoaGame* game_table(void)
{
#ifdef SOA_SPLIT
    return soa_game_table;
#else
    return soa_game();
#endif
}

void record_env(CpuState* s)
{
    const char* e = getenv("SOA_RECORD");
    const char* p;
    const SoaGame* g = game_table();
    RecFileHead h;
    unsigned k;
    long ps;
    if (!e || !*e) return;
    for (p = e; *p && *p != ';' && g_nfn < MAX_FUNCTIONS;) {
        char* end;
        unsigned long a = strtoul(p, &end, 0);
        if (end == p) break;
        g_fn[g_nfn++].address = (uint32_t)a;
        p = end;
        while (*p == ',' || *p == ' ') p++;
    }
    if (*p == ';') {
        char* end;
        g_limit = strtoull(p + 1, &end, 0);
        if (*end == ';') g_every = strtoull(end + 1, NULL, 0);
    }
    if (!g_limit) g_limit = 1000;
    if (!g_every) g_every = 1;
    if (getenv("SOA_RECORD_PAGES")) g_max_pages = (uint32_t)strtoul(getenv("SOA_RECORD_PAGES"), NULL, 0);
    ps = sysconf(_SC_PAGESIZE);
    g_mem = s->mem;
    if (!g_nfn || ps <= 0 || MEM1_SIZE % (uint32_t)ps || ((uintptr_t)g_mem % (uintptr_t)ps) || !g || !g->count) {
        fprintf(stderr, "[record] SOA_RECORD: %s; nothing is recorded\n",
                !g_nfn ? "no address (0xADDR[,0xADDR...][;limit[;every]])"
                : !g || !g->count ? "this build has no swap table"
                : "the memory is not whole pages");
        return;
    }
    g_page = (uint32_t)ps;
    g_npages = MEM1_SIZE / g_page;
    g_named = (uint8_t*)calloc(g_npages, 1);
    if (!g_named || (getenv("SOA_RECORD_WITH") && *getenv("SOA_RECORD_WITH") && !read_with(getenv("SOA_RECORD_WITH")))) {
        fprintf(stderr, "[record] SOA_RECORD_WITH: ranges are lo-hi or lo+bytes, comma-separated, or all; nothing is recorded\n");
        return;
    }
    g_max_pages += g_nnamed;
    if (g_all || g_max_pages > g_npages) g_max_pages = g_npages;
    g_state = (uint8_t*)calloc(g_npages, 1);
    g_touched = (uint32_t*)malloc(g_max_pages * sizeof *g_touched);
    g_pool = (uint8_t*)malloc((size_t)g_max_pages * g_page);
    g_shadow = (uint8_t*)malloc(MEM1_SIZE);
    g_shadowed = (uint8_t*)calloc(g_npages, 1);
    g_zero = (uint8_t*)calloc(1, g_page);
    g_path = getenv("SOA_RECORD_FILE");
    if (!g_path || !*g_path) g_path = "build/record.soarec";
    g_out = fopen(g_path, "wb");
    if (!g_state || !g_touched || !g_pool || !g_shadow || !g_shadowed || !g_zero || !g_out) {
        fprintf(stderr, "[record] %s; nothing is recorded\n", g_out ? "out of memory" : "cannot write the file");
        return;
    }
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_fault;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
        sigemptyset(&sa.sa_mask);
        if (sigaction(SIGSEGV, &sa, &g_prev) != 0) {
            fprintf(stderr, "[record] no fault handler; nothing is recorded\n");
            return;
        }
    }
    memset(&h, 0, sizeof h);
    memcpy(h.magic, REC_MAGIC, 8);
    h.version = REC_VERSION;
    h.page_size = g_page;
    h.mem_bytes = MEM1_SIZE;
    h.functions = g->count;
    if (g->record) snprintf(h.game, sizeof h.game, "%s", g->record);
    fwrite(&h, sizeof h, 1, g_out);
    g_bytes = sizeof h;
    for (k = 0; k < g_nfn; k++) {
        if (!swap_set(g_fn[k].address, on_call, NULL, &g_fn[k])) {
            fprintf(stderr, "[record] %08X: not a translated function's entry; not recorded\n", g_fn[k].address);
            continue;
        }
        g_left++;
    }
    hle_on_report(report);
    fprintf(stderr, "[record] recording %u functions, %llu calls of each at most, one call in %llu, into %s (pages of %u bytes; %s)\n",
            g_left, (unsigned long long)g_limit, (unsigned long long)g_every, g_path, g_page,
            g_all ? "every page kept at each" : g_nnamed ? "more pages named by SOA_RECORD_WITH kept" : "the pages each touches");
}

#else

void record_env(CpuState* s)
{
    (void)s;
    if (getenv("SOA_RECORD") && *getenv("SOA_RECORD"))
        fprintf(stderr, "[record] SOA_RECORD: recording needs a POSIX system's page protection; nothing is recorded\n");
}

#endif

/* A snapshot (snapshot.h; docs/SNAPSHOT.md): a recording's counts decide
 * which calls it records, and a recorded call holds interrupts back, so a
 * run being recorded is not one a snapshot can be taken of or resumed into. */
#include "snapshot.h"
int record_busy(void)
{
    return g_nfn != 0;
}

void record_snap(Snap* sn)
{
    if (g_nfn) snap_fail(sn, "SOA_RECORD is on: a recording's counts are part of the run, and it is not snapshotted");
}
