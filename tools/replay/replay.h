/*
 * A native replacement for a function of the game's, held to the game's own
 * calls of it outside the game (docs/RECORD.md).
 *
 * runtime/record.c records real calls during a run (SOA_RECORD). soa-compare
 * (tools/replay/compare.c, built by tools/replay.py) loads a recording and
 * the game library of the build that made it, and for each call puts the
 * pages the call touched back into a fresh memory, sets the registers the
 * call began with, and runs either the translation (a self check, which must
 * agree on every call) or a candidate. Then it holds what the call left to
 * what the recording says the game's call left: the registers that matter,
 * every byte of every page the call touched, and nothing written anywhere
 * else.
 *
 * A candidate is the host's own kind of answer (runtime/soa_host.h,
 * SoaHostAnswer, ABI 23), called as runtime/host.c's host_swap calls it, so
 * the code that is tested here is the code a host sets with
 * soa_host_replace: what it returns (SOA_HOST_GAMES, _ANSWERED, _BOTH,
 * _ANSWERED_LIVE) means what it means there, `game` and `game_original`
 * call the game's functions, and `original` runs the translation of the one
 * being answered. The call's `mem` is the replay's memory, which holds the
 * pages the recorded call touched and nothing else: a candidate that reads
 * a page the game's call did not is told so.
 *
 * Calls a candidate makes into the game run the game's translation against
 * the restored memory (the replay has the whole game library), so they act
 * as they did in the recorded run as long as they read what the game's own
 * call read. Their effects are not replayed from the recording.
 *
 * A file of candidates registers each at load time:
 *
 *     static int my_answer(void* user, SoaHostCall* call) { ... }
 *     SOA_REPLAY_CANDIDATE(my_rule, "my rule", 0x80096DA4u, my_answer, NULL, SOA_REPLAY_NONE)
 */
#ifndef SOA_REPLAY_H
#define SOA_REPLAY_H
#include <stdint.h>
#include "soa_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Which of the function's results a candidate is held to, beside memory and
 * the registers the calling convention keeps (r1, r2, r13-r31, f14-f31's
 * ps0, CR fields 2-4), which every candidate is held to. (ps1 of f14-f31 is
 * not: the game saves and restores those with stfd and lfd, which keep ps0
 * alone, and its paired-single code changes ps1 as it pleases.) A function that returns
 * nothing leaves r3 and f1 as whatever they were last, and holding a
 * candidate to that would be holding it to garbage. */
#define SOA_REPLAY_NONE 0u
#define SOA_REPLAY_R3 1u
#define SOA_REPLAY_R4 2u
#define SOA_REPLAY_F1 4u

typedef struct {
    const char* name;
    uint32_t address;     /* the function it answers */
    SoaHostAnswer answer; /* soa_host.h's */
    void* user;
    uint32_t results;     /* SOA_REPLAY_R3 ... */
} SoaReplayCandidate;

/* Adds a candidate (the struct is the caller's, kept); a file of candidates
 * calls it from SOA_REPLAY_CANDIDATE's constructor. */
void soa_replay_register(const SoaReplayCandidate* c);

#define SOA_REPLAY_CANDIDATE(id, name, address, answer, user, results)                                   \
    static const SoaReplayCandidate soa_replay_c_##id = {name, address, answer, user, results};           \
    __attribute__((constructor)) static void soa_replay_r_##id(void) { soa_replay_register(&soa_replay_c_##id); }

/* What a candidate may want from inside its answer while it is replayed:
 * which recorded call this is (the game's nth call of the function, from 0)
 * and the frame the recording made it in. */
uint64_t soa_replay_seen(void);
uint64_t soa_replay_frame(void);

/* Big-endian guest memory as a host call has it (mem[a & 0x01FFFFFF]), for
 * candidates written in C. */
static inline uint32_t soa_replay_r32(const SoaHostCall* c, uint32_t a)
{
    const uint8_t* p = c->mem + (a & 0x01FFFFFFu);
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline void soa_replay_w32(SoaHostCall* c, uint32_t a, uint32_t v)
{
    uint8_t* p = c->mem + (a & 0x01FFFFFFu);
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

/* ---- the library soa-compare is (replay.c) ---- */

typedef struct {
    const char* recording;
    const char* game;          /* the game library: libsoa_game.so of the build that recorded */
    uint32_t only;             /* 0, or the one function to replay */
    int original;              /* 1: the translation's self check */
    const SoaReplayCandidate* candidate; /* or NULL */
    uint32_t stack_scratch;    /* bytes under the call's r1 not held (its frames and its callees'); 0: 64K */
    int with_flagged;          /* 1: calls with a flag (an interrupt inside, ...) are replayed too */
    int partial;               /* 1: a byte the game's call wrote and the candidate left as it was is counted as
                                  left undone, not held: for a rule that answers part of a function */
    int no_hardware;           /* 1: the candidate is a native replacement, held to touching no hardware at all,
                                  even where the game's call did (its reads end the call, its stores are
                                  counted; either is a difference); the hardware the game's calls touched is
                                  listed as retired, for the candidate to have reproduced some other way */
    int show;                  /* mismatching calls told in full; the rest counted */
    int quiet;
} SoaReplayRun;

typedef struct {
    uint64_t calls, matched, mismatched, unreplayable, flagged_skipped;
    uint64_t partial, undone_bytes; /* with run->partial: calls that agree but for what they left undone */
    uint64_t hw_touched;            /* with run->no_hardware: calls in which the candidate touched the hardware */
    uint64_t answered, games, both, live; /* a candidate's answers */
    uint64_t bytes_held, regs_held;
    double seconds;
} SoaReplayTotals;

/* Replays and holds; the totals of every call it replayed; 0 when the
 * recording or the game library could not be read (why on stderr). */
int soa_replay(const SoaReplayRun* run, SoaReplayTotals* totals);

/* The registered candidates, n of them. */
const SoaReplayCandidate* const* soa_replay_candidates(unsigned* n);

/* A summary of a recording: each function's calls, flags, pages and the
 * functions they called; with seen not -1, the calls the game's call of
 * `only` with that number made, in order (their address and r1). */
int soa_replay_info(const char* recording, uint32_t only, int64_t seen);

#ifdef __cplusplus
}
#endif
#endif
