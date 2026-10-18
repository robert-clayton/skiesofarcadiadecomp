/*
 * Any function of the game's, swapped for native code at run time: swap.h
 * says what, docs/SWAP.md why and at what cost.
 *
 * The game's table (soa_game.h, ABI 2) holds a byte a function, which the
 * guard at the head of each translated function reads; this file is the only
 * writer, under g_lock, from whichever thread asks. The guest's thread reads
 * the byte with no lock, and when it is set takes g_lock only to copy the
 * replacement out, so a replacement is set or cleared whole: a call meets
 * the old one or the new one, never half of each. A call already inside a
 * replacement when it is cleared finishes in it; a host that needs a frame
 * boundary clears it while the guest is held.
 *
 * The counts are the guest's to write and are read with no lock: a count read
 * while the guest runs may be one call behind.
 */
#include "swap.h"
#include "mod.h"
#include "plat.h"
#include "soa_game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int dispatch_known(uint32_t addr); /* <out>/dispatch.c, or the split build's forwarder */

typedef struct {
    SwapHandler handler;
    SwapFn fn;
    void* user;
} Swap;

typedef struct {
    uint32_t address;
    SwapHandler handler;
    SwapFn fn;
    void* user;
} Pending;

static PlatLock g_lock;
static const SoaGame* g_game;  /* set once the table is known and the slots made */
static Swap* g_swap;           /* one a function */
static uint64_t *g_calls, *g_answered, *g_translated;
static int g_counting;
/* Requests made before the game library is loaded, kept until it is: as many
 * as are made (a host may set hundreds of answers at start), grown as asked. */
static Pending* g_pending;
static unsigned g_npending, g_cap_pending;
/* The one function whose next entry runs its translation, its index plus one
 * (swap_original): the guest is one thread at a time, and the entry that
 * reads it is the very next thing the call does. */
static uint32_t g_skip;
static SwapTraceFn g_trace; /* swap_trace's, with SWAP_TRACE in every byte while it is set */
static void (*g_due_take)(CpuState* s); /* swap_due_handler's */
static int g_due;                       /* SWAP_DUE is in every byte */
volatile uint64_t g_swap_answers; /* swap.h */

static const SoaGame* table(void)
{
#ifdef SOA_SPLIT
    return soa_game_table;
#else
    return soa_game();
#endif
}

/* The table, with the slots made for it; NULL while there is none (a split
 * build before its game library is loaded) or it has no swap table. Under
 * g_lock. */
static const SoaGame* ready_locked(void)
{
    const SoaGame* g;
    if (g_game) return g_game;
    g = table();
    if (!g || g->abi < 2u || !g->swap || !g->count) return NULL;
    g_swap = (Swap*)calloc(g->count, sizeof *g_swap);
    g_calls = (uint64_t*)calloc(g->count, sizeof *g_calls);
    g_answered = (uint64_t*)calloc(g->count, sizeof *g_answered);
    g_translated = (uint64_t*)calloc(g->count, sizeof *g_translated);
    if (!g_swap || !g_calls || !g_answered || !g_translated) {
        fprintf(stderr, "[swap] out of memory for %u functions' slots; nothing can be swapped\n", g->count);
        free(g_swap), free(g_calls), free(g_answered), free(g_translated);
        g_swap = NULL, g_calls = g_answered = g_translated = NULL;
        return NULL;
    }
    g_game = g;
    return g;
}

static int index_in(const SoaGame* g, uint32_t address)
{
    uint32_t lo = 0, hi = g->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (g->address[mid] < address) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= g->count || g->address[lo] != address) return -1;
    return g->kind[lo] ? -2 : (int)lo;
}

int swap_index(uint32_t address)
{
    const SoaGame* g;
    int i;
    plat_lock(&g_lock);
    g = ready_locked();
    i = g ? index_in(g, address) : -3;
    plat_unlock(&g_lock);
    return i;
}

static int set_locked(const SoaGame* g, uint32_t address, SwapHandler handler, SwapFn fn, void* user)
{
    int i = index_in(g, address);
    if (i < 0) return 0;
    if (handler) {
        g_swap[i].handler = handler;
        g_swap[i].fn = fn;
        g_swap[i].user = user;
        g->swap[i] = (uint8_t)(g->swap[i] | SWAP_HOOK);
    } else {
        g->swap[i] = (uint8_t)(g->swap[i] & ~SWAP_HOOK);
        g_swap[i].handler = NULL;
        g_swap[i].fn = NULL;
        g_swap[i].user = NULL;
    }
    return 1;
}

int swap_set(uint32_t address, SwapHandler handler, SwapFn fn, void* user)
{
    const SoaGame* g;
    int ok = 0;
    unsigned k;
    plat_lock(&g_lock);
    g = ready_locked();
    if (g) {
        ok = set_locked(g, address, handler, fn, user);
    } else if (!table()) { /* not loaded yet: kept, the last request an address wins */
        for (k = 0; k < g_npending && g_pending[k].address != address; k++) {}
        if (k == g_cap_pending) {
            unsigned cap = g_cap_pending ? g_cap_pending * 2u : 256u;
            Pending* grown = (Pending*)realloc(g_pending, cap * sizeof *grown);
            if (grown) g_pending = grown, g_cap_pending = cap;
        }
        if (k < g_cap_pending) {
            g_pending[k].address = address;
            g_pending[k].handler = handler;
            g_pending[k].fn = fn;
            g_pending[k].user = user;
            if (k == g_npending) g_npending++;
            ok = 1;
        }
    }
    plat_unlock(&g_lock);
    return ok;
}

void swap_attach(void)
{
    const SoaGame* g;
    unsigned k;
    plat_lock(&g_lock);
    g = ready_locked();
    for (k = 0; k < g_npending; k++)
        if (!g || !set_locked(g, g_pending[k].address, g_pending[k].handler, g_pending[k].fn, g_pending[k].user))
            if (g_pending[k].handler)
                fprintf(stderr, "[swap] %08X: not a function of the game's the runtime does not answer itself; not swapped\n",
                        g_pending[k].address);
    g_npending = 0;
    free(g_pending);
    g_pending = NULL, g_cap_pending = 0;
    plat_unlock(&g_lock);
}

void swap_count(int on)
{
    const SoaGame* g;
    uint32_t i;
    plat_lock(&g_lock);
    g = ready_locked();
    if (g) {
        if (on) {
            memset(g_calls, 0, g->count * sizeof *g_calls);
            memset(g_answered, 0, g->count * sizeof *g_answered);
            memset(g_translated, 0, g->count * sizeof *g_translated);
        }
        for (i = 0; i < g->count; i++)
            if (!g->kind[i]) g->swap[i] = (uint8_t)(on ? g->swap[i] | SWAP_COUNT : g->swap[i] & ~SWAP_COUNT);
        g_counting = on != 0;
    }
    plat_unlock(&g_lock);
}

void swap_trace(SwapTraceFn fn)
{
    const SoaGame* g;
    uint32_t i;
    plat_lock(&g_lock);
    g = ready_locked();
    if (g) {
        g_trace = fn;
        for (i = 0; i < g->count; i++)
            if (!g->kind[i]) g->swap[i] = (uint8_t)(fn ? g->swap[i] | SWAP_TRACE : g->swap[i] & ~SWAP_TRACE);
    }
    plat_unlock(&g_lock);
}

void swap_due_handler(void (*take)(CpuState* s))
{
    g_due_take = take;
}

/* Every byte at once, as the guard reads it: the native functions' bytes
 * too (no guard reads them, and one loop is the cheapest). Called on each
 * change of the clock's flag, a few hundred times a frame. */
void swap_due(int on)
{
    const SoaGame* g;
    uint32_t i, n;
    uint8_t* b;
    plat_lock(&g_lock);
    g = ready_locked();
    if (g && on != g_due) {
        b = g->swap, n = g->count;
        if (on)
            for (i = 0; i < n; i++) b[i] = (uint8_t)(b[i] | SWAP_DUE);
        else
            for (i = 0; i < n; i++) b[i] = (uint8_t)(b[i] & ~SWAP_DUE);
        g_due = on;
    }
    plat_unlock(&g_lock);
}

unsigned swap_counts(SwapCount* out, unsigned max)
{
    const SoaGame* g;
    unsigned n = 0;
    uint32_t i;
    plat_lock(&g_lock);
    g = ready_locked();
    for (i = 0; g && i < g->count; i++) {
        uint64_t c = g_calls[i], a = g_answered[i], t = g_translated[i];
        if (!c && !a && !t) continue;
        if (out && n < max) {
            out[n].address = g->address[i];
            out[n].replaced = (g->swap[i] & SWAP_HOOK) ? 1u : 0u;
            out[n].calls = c;
            out[n].answered = a;
            out[n].translated = t;
        }
        n++;
    }
    plat_unlock(&g_lock);
    return n;
}

unsigned swap_functions(unsigned* native)
{
    const SoaGame* g;
    unsigned n = 0, k = 0;
    uint32_t i;
    plat_lock(&g_lock);
    g = ready_locked();
    if (g) {
        n = g->count;
        for (i = 0; i < g->count; i++) k += g->kind[i] != 0;
    }
    plat_unlock(&g_lock);
    if (native) *native = k;
    return n;
}

/* The translation of function i, now, on the live state: the guard at its
 * entry takes the skip and lets it run. A guard that has been cleared since
 * the caller looked does not take it, and the skip is dropped after. */
static void run_translation(CpuState* s, const SoaGame* g, uint32_t i)
{
    g_skip = i + 1u;
    dispatch(s, g->address[i]);
    g_skip = 0;
}

int swap_original(CpuState* s, uint32_t address)
{
    const SoaGame* g = g_game ? g_game : table();
    int i;
    if (!g || !dispatch_known(address)) return 0;
    i = g_game ? index_in(g, address) : -3;
    if (i >= 0 && ((volatile uint8_t*)g->swap)[i]) run_translation(s, g, (uint32_t)i);
    else dispatch(s, address); /* nothing in the way, or the runtime's own */
    return 1;
}

int swap_call_original(CpuState* s, uint32_t address, const uint32_t* ints, uint32_t n_ints, const double* floats,
                       uint32_t n_floats, uint32_t* r3, double* f1, char* why, size_t cap)
{
    const SoaGame* g = g_game;
    int i = g ? index_in(g, address) : -3, ok;
    if (i < 0 || !((volatile uint8_t*)g->swap)[i])
        return mod_call_guest(s, address, ints, n_ints, floats, n_floats, r3, f1, why, cap);
    g_skip = (uint32_t)i + 1u;
    ok = mod_call_guest(s, address, ints, n_ints, floats, n_floats, r3, f1, why, cap);
    g_skip = 0;
    return ok;
}

int soa_swap_enter(CpuState* s, uint32_t i)
{
    const SoaGame* g = g_game;
    uint8_t f;
    Swap w;
    int how;
    if (!g) { /* a byte set with no slots cannot happen; be the translation */
        g_skip = 0;
        return 0;
    }
    f = ((volatile uint8_t*)g->swap)[i];
    if ((f & SWAP_DUE) && g_due_take) {
        /* an entry is a delivery point under the quiet rule: before the
         * skip, the count and the replacement, so it is the same place
         * whether a replacement or the translation answers */
        g_due_take(s);
        f = ((volatile uint8_t*)g->swap)[i];
    }
    if (g_skip == i + 1u) {
        g_skip = 0;
        if (f & SWAP_COUNT) g_translated[i]++;
        return 0;
    }
    if (f & SWAP_COUNT) g_calls[i]++;
    if (f & SWAP_TRACE) {
        SwapTraceFn t = g_trace;
        if (t) t(s, g->address[i]);
    }
    if (f & SWAP_HOOK) {
        plat_lock(&g_lock);
        w = g_swap[i];
        plat_unlock(&g_lock);
        if (w.handler) {
            uint32_t address = g->address[i];
            s->pc = address; /* a sample in the replacement names the function, as its own block head would */
            how = w.handler(s, address, w.fn, w.user, 0);
            if (how == SWAP_ANSWERED || how == SWAP_BOTH) g_swap_answers++; /* native work: not a wait's */
            if (how == SWAP_ANSWERED) {
                if (f & SWAP_COUNT) g_answered[i]++;
                return 1;
            }
            if (how == SWAP_BOTH) {
                run_translation(s, g, i);
                w.handler(s, address, w.fn, w.user, 1);
                return 1;
            }
        }
    }
    if (f & SWAP_COUNT) g_translated[i]++;
    return 0;
}

/* SOA_COUNT_CALLS=1, or a path: counting from the start of the run, and at
 * its end one line of how much of the game ran translated -- the port's
 * progress counter over a recorded play (docs/SWAP.md) -- and, given a path,
 * every function counted there, a line each: address, calls, answered,
 * translated, tab-separated. */
static const char* g_count_path;

static void swap_report(void)
{
    const SoaGame* g = g_game;
    unsigned n, k, native = 0, total = swap_functions(&native), ran = 0, answered = 0, translated = 0;
    SwapCount* c;
    FILE* f = NULL;
    if (!g) return;
    n = swap_counts(NULL, 0);
    c = (SwapCount*)malloc((n ? n : 1) * sizeof *c);
    if (!c) return;
    n = swap_counts(c, n);
    if (g_count_path && strcmp(g_count_path, "1") != 0) {
        f = fopen(g_count_path, "w");
        if (!f) fprintf(stderr, "[swap] cannot write %s\n", g_count_path);
        else fprintf(f, "address\tcalls\tanswered\ttranslated\n");
    }
    for (k = 0; k < n; k++) {
        ran += c[k].calls > 0;
        answered += c[k].answered > 0;
        translated += c[k].translated > 0;
        if (f)
            fprintf(f, "%08X\t%llu\t%llu\t%llu\n", c[k].address, (unsigned long long)c[k].calls,
                    (unsigned long long)c[k].answered, (unsigned long long)c[k].translated);
    }
    if (f) fclose(f);
    fprintf(stderr, "[swap] %u of %u functions called (%u more the runtime answers natively); %u ran translated, "
                    "%u answered by a replacement%s%s\n",
            ran, total - native, native, translated, answered, f ? "; every count in " : "", f ? g_count_path : "");
    free(c);
}

void hle_on_report(void (*fn)(void)); /* hle.c */

/* SOA_PIPE_CENSUS=<path>: every translated function that stores to the
 * write-gather pipe itself, with the bytes and stores it made there (its own
 * blocks', not its callees'), and its calls; at the run's end, into the file,
 * a line each, busiest first, and the busiest on stderr. gx.c tells each
 * store with the block's address (s->pc), which names the function whose
 * entry is the nearest at or below it. Counting is on from the start, as
 * SOA_COUNT_CALLS's. */
static uint64_t *g_pipe_bytes, *g_pipe_stores;
static uint64_t g_pipe_outside; /* bytes from a block below the first function */
static const char* g_census_path;
static uint32_t g_census_lo, g_census_hi, g_census_at; /* the last function found: [lo, hi) is index at */

int swap_pipe_census_on(void)
{
    return g_pipe_bytes != NULL;
}

void swap_pipe_census_note(uint32_t pc, unsigned size)
{
    const SoaGame* g = g_game;
    uint32_t lo, hi;
    if (!g || !g_pipe_bytes) return;
    if (!(pc >= g_census_lo && pc < g_census_hi)) {
        lo = 0, hi = g->count;
        while (lo < hi) { /* the last entry at or below pc */
            uint32_t mid = lo + (hi - lo) / 2;
            if (g->address[mid] <= pc) lo = mid + 1;
            else hi = mid;
        }
        if (!lo) {
            g_pipe_outside += size;
            return;
        }
        g_census_at = lo - 1;
        g_census_lo = g->address[g_census_at];
        g_census_hi = lo < g->count ? g->address[lo] : 0xFFFFFFFFu;
    }
    g_pipe_bytes[g_census_at] += size;
    g_pipe_stores[g_census_at]++;
}

typedef struct {
    uint32_t i;
    uint64_t bytes;
} CensusRow;

static int census_cmp(const void* a, const void* b)
{
    uint64_t x = ((const CensusRow*)a)->bytes, y = ((const CensusRow*)b)->bytes;
    if (x != y) return x < y ? 1 : -1;
    return ((const CensusRow*)a)->i < ((const CensusRow*)b)->i ? -1 : 1;
}

static void census_report(void)
{
    const SoaGame* g = g_game;
    CensusRow* rows;
    unsigned n = 0, k, i;
    uint64_t total = 0, stores = 0;
    FILE* f;
    if (!g || !g_pipe_bytes) return;
    rows = (CensusRow*)malloc(g->count * sizeof *rows);
    if (!rows) return;
    for (i = 0; i < g->count; i++)
        if (g_pipe_bytes[i]) {
            rows[n].i = i, rows[n].bytes = g_pipe_bytes[i], n++;
            total += g_pipe_bytes[i], stores += g_pipe_stores[i];
        }
    qsort(rows, n, sizeof *rows, census_cmp);
    f = fopen(g_census_path, "w");
    if (!f) fprintf(stderr, "[census] cannot write %s\n", g_census_path);
    else {
        fprintf(f, "address\tpipe_bytes\tpipe_stores\tcalls\ttranslated\tbytes_per_call\n");
        for (k = 0; k < n; k++) {
            i = rows[k].i;
            fprintf(f, "%08X\t%llu\t%llu\t%llu\t%llu\t%.1f\n", g->address[i], (unsigned long long)g_pipe_bytes[i],
                    (unsigned long long)g_pipe_stores[i], (unsigned long long)g_calls[i],
                    (unsigned long long)g_translated[i], g_calls[i] ? (double)g_pipe_bytes[i] / (double)g_calls[i] : 0.0);
        }
        fclose(f);
    }
    fprintf(stderr, "[census] %u functions wrote %llu bytes in %llu stores to the gather pipe themselves%s%s; the busiest:\n", n,
            (unsigned long long)total, (unsigned long long)stores, f ? "; every one in " : "", f ? g_census_path : "");
    for (k = 0; k < n && k < 12; k++) {
        i = rows[k].i;
        fprintf(stderr, "[census]   %08X  %llu bytes (%.1f%%) in %llu stores, %llu calls\n", g->address[i],
                (unsigned long long)g_pipe_bytes[i], total ? 100.0 * (double)g_pipe_bytes[i] / (double)total : 0.0,
                (unsigned long long)g_pipe_stores[i], (unsigned long long)g_calls[i]);
    }
    if (g_pipe_outside)
        fprintf(stderr, "[census] and %llu bytes from code below the first function\n", (unsigned long long)g_pipe_outside);
    free(rows);
}

void swap_env(void)
{
    const char* e = getenv("SOA_COUNT_CALLS");
    const char* census = getenv("SOA_PIPE_CENSUS");
    if (census && *census && strcmp(census, "0") != 0) {
        const SoaGame* g;
        swap_count(1);
        plat_lock(&g_lock);
        g = ready_locked();
        plat_unlock(&g_lock);
        if (!g) fprintf(stderr, "[census] SOA_PIPE_CENSUS: this build has no swap table; nothing is counted\n");
        else {
            g_pipe_stores = (uint64_t*)calloc(g->count, sizeof *g_pipe_stores);
            g_pipe_bytes = g_pipe_stores ? (uint64_t*)calloc(g->count, sizeof *g_pipe_bytes) : NULL;
            if (g_pipe_bytes) {
                g_census_path = census;
                hle_on_report(census_report);
                fprintf(stderr, "[census] the gather pipe's writers, by function, into %s (SOA_PIPE_CENSUS)\n", census);
            }
        }
    }
    if (!e || !*e || !strcmp(e, "0")) return;
    g_count_path = e;
    swap_count(1);
    if (!g_game) {
        fprintf(stderr, "[swap] SOA_COUNT_CALLS: this build has no swap table; nothing is counted\n");
        return;
    }
    hle_on_report(swap_report);
    {
        unsigned native = 0, total = swap_functions(&native);
        fprintf(stderr, "[swap] counting every call of the %u translated functions (SOA_COUNT_CALLS)\n", total - native);
    }
}
