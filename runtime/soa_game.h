/*
 * The game library's one export (specs/android.md 3.2): what the runtime
 * needs from the translated code, in one table. tools/recompile.py writes
 * <out>/game_table.c, which fills it, into every build.
 *
 * A single-file build (soa.exe, gen/linux/soa) links the table and calls the
 * translated code directly; the self test holds the one to the other. A
 * split build (recompile.py --split: libsoa_runtime.so and libsoa_game.so,
 * Android's arrangement) reaches the game only through the table, which
 * runtime/game.c loads: <out>/runtime_seam.c gives the runtime forwarders
 * under the names it calls, and disc.c reads the system files from here.
 */
#ifndef SOA_GAME_H
#define SOA_GAME_H
#include "cpu.h"
#include <stddef.h>
#include <stdint.h>

#define SOA_GAME_ABI 2u /* 2: the swap table (runtime/swap.c, docs/SWAP.md) */

typedef void (*SoaFn)(CpuState* s);

/* The swap guard's bytes are the game's own, hidden in a split build's game
 * library (<out>/functions.h says the same). */
#ifndef SOA_SWAP_HIDDEN
#if defined(_MSC_VER)
#define SOA_SWAP_HIDDEN
#else
#define SOA_SWAP_HIDDEN __attribute__((visibility("hidden")))
#endif
#endif

typedef struct {
    uint32_t abi;  /* SOA_GAME_ABI */
    uint32_t size; /* sizeof(SoaGame) */
    const char* record; /* "abi=1 mode=... baked=... dol=... profile=...", as in the ELF note */
    SoaFn entry;        /* __start */
    void (*dispatch)(CpuState* s, uint32_t addr);
    int (*dispatch_known)(uint32_t addr);
    /* the player's system files (disc-layer I3) */
    const uint32_t *dol, *boot, *fst;
    const size_t *dol_size, *boot_size, *fst_size;
    const char *dol_sha1, *boot_sha1, *fst_sha1;
    /* the translated body at a guest address the self test runs, or NULL */
    SoaFn (*twin)(uint32_t addr);
    /* ABI 2, the swap table (runtime/swap.c, docs/SWAP.md): every function the
     * recompiler knows, in address order, with its index the one its entry's
     * SOA_SWAP(index) guard names. swap is the guard's byte an index (0: the
     * translation runs and nothing else is asked; swap.c's own to write),
     * address the function's guest address, kind 1 where the runtime answers
     * the function itself (config/hle.txt: no guard, never swapped). All NULL
     * and count 0 in a table built without them (a test's stand-in game). */
    uint8_t* swap;
    const uint32_t* address;
    const uint8_t* kind;
    uint32_t count;
} SoaGame;

#if defined(_MSC_VER)
#define SOA_GAME_EXPORT
#else
#define SOA_GAME_EXPORT __attribute__((visibility("default")))
#endif

/* <out>/game_table.c */
SOA_GAME_EXPORT const SoaGame* soa_game(void);

#ifdef SOA_SPLIT
/* runtime/game.c: the loaded library's table */
extern const SoaGame* soa_game_table;
#endif

#endif
