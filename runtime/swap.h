/*
 * Any function of the game's, swapped for native code at run time
 * (docs/SWAP.md). Every translated function begins with SOA_SWAP(index), a
 * byte of the game's table read and a branch never taken until this file
 * sets the byte; then soa_swap_enter is called, counts the call if counting
 * is on, and hands it to the replacement set for that function, if any.
 *
 * A replacement is a handler and two words for it (the host's own function
 * and user, say), called on the guest's thread with the guest's state:
 *
 *   SWAP_GAMES     the translation runs, as if nothing had asked
 *   SWAP_ANSWERED  the handler has done the function's work; it does not run
 *   SWAP_BOTH      the translation runs, then the handler once more (after 1)
 *
 * Functions the runtime answers itself (config/hle.txt, kind 1 in the table)
 * have no guard and are refused. Every function here is safe from any thread
 * and before the game library is loaded (a request then waits for it).
 */
#ifndef SOA_SWAP_H
#define SOA_SWAP_H
#include "cpu.h"
#include <stddef.h>
#include <stdint.h>

#define SWAP_GAMES 0
#define SWAP_ANSWERED 1
#define SWAP_BOTH 2

/* The guard's byte: a replacement is set; the call is counted. */
#define SWAP_HOOK 1u
#define SWAP_COUNT 2u
#define SWAP_TRACE 4u /* the entry is told to the trace function (swap_trace) */
#define SWAP_DUE 8u   /* the fixed clock's quiet rule: something has come due, and the next entry delivers it */

typedef void (*SwapFn)(void); /* the handler's own function, whatever its type: cast back by the handler */
typedef int (*SwapHandler)(CpuState* s, uint32_t address, SwapFn fn, void* user, int after);

/* The function's index in the table, -1 when address starts no function this
 * program has, -2 when the runtime answers it itself, -3 with no table. */
int swap_index(uint32_t address);

/* Sets (handler not NULL) or clears the replacement at address: 1 done, 0
 * refused (not a function, or the runtime's own). Before the game library is
 * loaded a request is kept (64 at most) and 1 means only that. */
int swap_set(uint32_t address, SwapHandler handler, SwapFn fn, void* user);

/* Counting on (resets every count) or off (keeps them for reading). */
void swap_count(int on);

typedef struct {
    uint32_t address;
    uint32_t replaced;    /* 1: a replacement is set now */
    uint64_t calls;       /* the game called it (not the runs of its own translation a replacement asked for) */
    uint64_t answered;    /* a replacement answered the call (SWAP_ANSWERED) */
    uint64_t translated;  /* its translation ran, for the game or for a replacement */
} SwapCount;

/* Every function with a count above 0, in address order, into out while it
 * fits; returns how many there are. */
unsigned swap_counts(SwapCount* out, unsigned max);

/* How many functions the table has; in *native, how many of them the runtime
 * answers itself. */
unsigned swap_functions(unsigned* native);

/* Every translated function's entry told to fn, with its address, until
 * swap_trace(NULL): for runtime/record.c, which wants the calls a recorded
 * call makes. An entry that runs a translation for swap_original or for
 * SWAP_BOTH is not told (it is the function already being run). Setting it
 * sets a bit in every guard's byte, so every call pays the slow path while
 * it is on. */
typedef void (*SwapTraceFn)(CpuState* s, uint32_t address);
void swap_trace(SwapTraceFn fn);

/* Runs the translation at address on the live state, in place, bypassing its
 * replacement for this one call (its own calls, and any recursion, meet the
 * replacements as the game's would). 0 when address starts no function. */
int swap_original(CpuState* s, uint32_t address);

/* Calls a function of the game's from native code as mod_call_guest does
 * (every register put back, r3 and f1 taken), but its translation even if it
 * is replaced: for a replacement that wants what the original would answer. */
int swap_call_original(CpuState* s, uint32_t address, const uint32_t* ints, uint32_t n_ints, const double* floats,
                       uint32_t n_floats, uint32_t* r3, double* f1, char* why, size_t cap);

/* After a host build has loaded the game library (host.c): the requests
 * kept until then are applied. */
void swap_attach(void);

/* SOA_COUNT_CALLS (main.c, at the start of a run): counting on, and a report
 * at the run's end. */
void swap_env(void);

/* SOA_PIPE_CENSUS (swap_env reads it): on, and the note gx.c is to call
 * with each gather-pipe store's block address and size (main.c passes it to
 * gx_set_pipe_census). */
int swap_pipe_census_on(void);
void swap_pipe_census_note(uint32_t pc, unsigned size);

/* The fixed clock's quiet rule (docs/CLOCK.md): on, every function's byte
 * gets SWAP_DUE, so its next entry calls `take` (irq.c's irq_quiet_entry) to
 * deliver what has come due; off, they lose it. hle.c installs swap_due as
 * clock.c's g_clock_due_fn when the rule is on. */
void swap_due(int on);
void swap_due_handler(void (*take)(CpuState* s));
/* Every call a replacement answered (SWAP_ANSWERED, or SWAP_BOTH's), counted
 * on the guest's thread whether or not counting is on: the quiet rule (irq.c,
 * through quiet.c) reads it, so a loop around an answer that did work in
 * memory is not taken for a wait (docs/CLOCK.md). */
extern volatile uint64_t g_swap_answers;

int soa_swap_enter(CpuState* s, uint32_t index); /* the guard's slow path */

#endif
