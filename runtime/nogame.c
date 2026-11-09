/*
 * A run without the game library (soa_host_without_game; docs/SWAP.md, "A run without the game library"): for a host
 * that answers every function the game calls natively (soa-ue5's U5a). Nothing translated is loaded, so nothing
 * translated can run: the runtime's table is a stand-in made here at the start, from the addresses the host asked to
 * replace before it (swap.c's kept requests) and the functions the runtime answers itself (<out>/runtime_seam.c's
 * soa_runtime_answers). Its dispatch:
 *
 *   - a function the runtime answers: its own answer, as the game library's dispatch calls it;
 *   - a function the host replaced: the guard's slow path (soa_swap_enter), so the quiet rule's delivery, the counts,
 *     the trap and the replacement are as they are with the library;
 *   - anything else -- an address no answer covers (an indirect call through a pointer, say), an answer that hands its
 *     call back (SWAP_GAMES) or asks for its translation (swap_original, SWAP_BOTH) -- stops the run (exit 3) after the
 *     runtime's report, with the address, why and the guest's call chain: there the translation would have run.
 *
 * dispatch_known says yes to every address, so an exact or game call by address always reaches dispatch and its
 * verdict. The system files (I3) are not the library's: disc_builtin finds none and main.c reads the image's, with
 * SOA_DVD=off too (the drive's model stays off). A split build only.
 */
#ifdef SOA_SPLIT
#include "soa_game.h"
#include "swap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hle_report(void);                          /* hle.c */
void guest_backtrace(CpuState* s, uint32_t sp); /* hle.c */

static int g_on;           /* soa_nogame_set */
static SoaGame g_table;    /* the stand-in */
static uint32_t* g_addr;   /* sorted */
static uint8_t* g_kind;    /* 1: the runtime's own answer */
static uint8_t* g_bytes;   /* the guard bytes swap.c writes */
static SoaFn* g_own;       /* the runtime's answer where g_kind is 1 */
static uint32_t g_n;
static const size_t g_zero_size = 0;

int soa_nogame(void)
{
    return g_on;
}

void soa_nogame_set(int on)
{
    g_on = on != 0;
}

static int find(uint32_t address)
{
    uint32_t lo = 0, hi = g_n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (g_addr[mid] < address) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_n && g_addr[lo] == address ? (int)lo : -1;
}

static void unanswered(CpuState* s, uint32_t address, const char* why)
{
    fflush(stdout);
    fprintf(stderr, "[nogame] %08X: %s; without the game library its translation cannot run (lr %08X). The guest's "
                    "call chain:", address, why, s->lr);
    guest_backtrace(s, s->gpr[1]);
    hle_report();
    fflush(stderr);
    exit(3);
}

static void nogame_dispatch(CpuState* s, uint32_t address)
{
    int i = find(address);
    if (i < 0) {
        unanswered(s, address, "no answer covers this address (a call through a pointer, or one never bound)");
        return;
    }
    if (g_kind[i]) {
        g_own[i](s);
        return;
    }
    if (soa_swap_enter(s, (uint32_t)i)) return;
    unanswered(s, address, g_bytes[i] & SWAP_HOOK ? "its answer handed the call back or asked for its translation"
                                                  : "no answer is set for it");
}

static int nogame_known(uint32_t address)
{
    (void)address;
    return 1;
}

static SoaFn nogame_twin(uint32_t address)
{
    (void)address;
    return NULL; /* runtime_seam.c's forwarders then dispatch */
}

static void nogame_entry(CpuState* s)
{
    nogame_dispatch(s, 0x80003140u); /* __start */
}

static int cmp_u32(const void* a, const void* b)
{
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}

/* swap.c: the addresses asked for before the table */
unsigned swap_pending_addresses(uint32_t* out, unsigned max);

int soa_nogame_build(char* why, size_t cap)
{
    unsigned asked = swap_pending_addresses(NULL, 0), n = asked + soa_runtime_answer_count, k, m = 0;
    uint32_t* all;
    if (!g_on) {
        snprintf(why, cap, "not a run without the game library");
        return 1;
    }
    all = (uint32_t*)malloc((n ? n : 1) * sizeof *all);
    if (!all) {
        snprintf(why, cap, "out of memory for the stand-in table");
        return 1;
    }
    asked = swap_pending_addresses(all, asked);
    for (k = 0; k < soa_runtime_answer_count; k++) all[asked + k] = soa_runtime_answers[k].address;
    n = asked + soa_runtime_answer_count;
    qsort(all, n, sizeof *all, cmp_u32);
    for (k = 0; k < n; k++)
        if (!m || all[m - 1] != all[k]) all[m++] = all[k];
    g_addr = all, g_n = m;
    g_kind = (uint8_t*)calloc(m ? m : 1, 1);
    g_bytes = (uint8_t*)calloc(m ? m : 1, 1);
    g_own = (SoaFn*)calloc(m ? m : 1, sizeof *g_own);
    if (!g_kind || !g_bytes || !g_own) {
        snprintf(why, cap, "out of memory for the stand-in table");
        return 1;
    }
    for (k = 0; k < soa_runtime_answer_count; k++) {
        int i = find(soa_runtime_answers[k].address);
        g_kind[i] = 1;
        g_own[i] = soa_runtime_answers[k].fn;
    }
    g_table.abi = SOA_GAME_ABI;
    g_table.size = (uint32_t)sizeof g_table;
    g_table.record = "nogame resume=801DCB84"; /* the resume is main's answer's (snapshot.c) */
    g_table.entry = nogame_entry;
    g_table.dispatch = nogame_dispatch;
    g_table.dispatch_known = nogame_known;
    g_table.dol = g_table.boot = g_table.fst = NULL;
    g_table.dol_size = g_table.boot_size = g_table.fst_size = &g_zero_size;
    g_table.dol_sha1 = g_table.boot_sha1 = g_table.fst_sha1 = "";
    g_table.twin = nogame_twin;
    g_table.swap = g_bytes;
    g_table.address = g_addr;
    g_table.kind = g_kind;
    g_table.count = m;
    soa_game_table = &g_table;
    fprintf(stderr, "[game] none loaded (soa_host_without_game): %u functions answered, %u of them by the host, %u by "
                    "the runtime; any other call stops the run\n", m, m - soa_runtime_answer_count,
            soa_runtime_answer_count);
    return 0;
}
#else
int soa_nogame(void)
{
    return 0;
}
#endif
