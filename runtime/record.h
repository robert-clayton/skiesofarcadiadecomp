/*
 * Real calls of chosen functions, recorded during a run so a native
 * replacement can be held to them outside the game (docs/RECORD.md).
 *
 * SOA_RECORD=0xADDR[,0xADDR...][;limit[;every]] puts a SWAP_BOTH replacement
 * (runtime/swap.c) on each address. Of the calls the game makes, every
 * `every`-th is recorded until `limit` of that function have been (1000 and 1
 * by default). A recorded call is run by its translation with the game's
 * memory read-only and then not even that: each page the call (and every
 * call it makes) first reads is copied as it was and opened for reading,
 * each it first writes opened for writing. What the file keeps of a call:
 *
 *   the registers before and after;
 *   each page it touched, as it was before (its input), and the bytes of it
 *   that are not after as they were (its output);
 *   the timebase and hardware reads it made, in order, with the values
 *   they read;
 *   the entry of every function it called, in order, with r1 (SWAP_TRACE).
 *
 * A call that met what a replay cannot give back is kept with a flag that
 * says so: an interrupt delivered inside it, another guest thread run inside
 * it, a page touched by another host thread. A call that stored to the
 * disc's interface is dropped: from that store on its pages were opened
 * again, since the disc's reads go through the kernel, which a protected
 * page would fail, and what it touched after is not known. What any other
 * device does to memory at a store is done in the runtime's own code and is
 * seen as the call's.
 *
 * SOA_RECORD_WITH names more memory to keep at each recorded call, touched
 * or not: lo-hi or lo+bytes ranges, comma-separated, or `all` (every page of
 * the RAM). A candidate that reads more than the game's function does (a
 * whole list of bodies where the function stops at the first) needs it, or
 * the replay has nothing to give it there. A page that has not changed since
 * the file last had it costs a page record and no bytes.
 *
 * Nothing costs anything with SOA_RECORD unset: the guards are the swap's,
 * and the one other test is in hle.c's MMIO and timebase entry points.
 *
 * A recording is the game's memory: it stays on the machine that made it,
 * like a memory card (CONTRIBUTING.md), never in the repository.
 *
 * The file, in the byte order of the machine that made it (a recording is
 * replayed where it was made):
 *
 *   RecFileHead
 *   RecCallHead, then its `pages` RecPage, each followed by its pre bytes and
 *   then its post bytes; then `inputs` RecInput, then `callees` RecCallee;
 *   and so on to the end of the file.
 *
 * A page's bytes are encoded against a base: the pre-image against the
 * page's last image in the file (all zero before its first), the post image
 * against the pre-image. The encoding is runs: a u16 count of bytes that are
 * the base's, a u16 count of bytes that follow literally (their XOR with the
 * base), repeated until the page is whole; pre_bytes 0 means "the base, as
 * it is". The last image of a page is the post image of the last call that
 * had it (its pre-image when that call left it as it was).
 */
#ifndef SOA_RECORD_H
#define SOA_RECORD_H
#include "cpu.h"
#include <stdint.h>

#define REC_MAGIC "SOAREC\0\1"
#define REC_VERSION 1u
#define REC_CALL_MAGIC 0x31435253u /* "SRC1" */

/* A call's flags. */
#define REC_IRQ 1u         /* an interrupt was delivered inside it: its handler's work is in the output */
#define REC_SLEPT 2u       /* another guest thread ran inside it */
#define REC_FOREIGN 4u     /* another host thread touched a page while it ran */
#define REC_MANY_INPUTS 8u /* more timebase reads than are kept */
#define REC_MANY_CALLEES 16u /* more calls than are kept: the list is the first ones */

/* A page's flags. */
#define REC_PAGE_READ 1u
#define REC_PAGE_WRITTEN 2u

/* What hle.c's RECORD_NOTE tells, and a call's record keeps in order (its
 * "inputs", though a store is an output): a store to the hardware, the
 * timebase's two halves read, a read of the hardware. An access's size in
 * bytes is in bits 8 on. A store to the disc's interface drops the call. */
#define REC_MMIO_STORE 1
#define REC_IN_TB_LO 2
#define REC_IN_TB_HI 3
#define REC_IN_MMIO 4
#define REC_KIND(k) ((k) & 0xFFu)

#define REC_MAX_INPUTS 256u
#define REC_MAX_CALLEES 65536u

typedef struct {
    char magic[8];       /* REC_MAGIC */
    uint32_t version;    /* REC_VERSION */
    uint32_t page_size;  /* the bytes a page has */
    uint32_t mem_bytes;  /* the RAM the pages are of: MEM1_SIZE */
    uint32_t functions;  /* the game's table's count, which a replay checks against its own */
    char game[192];      /* the game's table's record string ("abi=1 mode=..."), cut to fit */
} RecFileHead;

typedef struct {
    uint32_t gpr[32];
    double fpr[32][2]; /* ps0, ps1 */
    uint32_t cr, xer, lr, ctr, fpscr, msr;
    uint32_t gqr[8];
} RecRegs;

typedef struct {
    uint32_t magic;   /* REC_CALL_MAGIC */
    uint32_t bytes;   /* the whole record's, this head included */
    uint32_t address; /* the function */
    uint32_t flags;   /* REC_IRQ ... */
    uint64_t seen;    /* which of the game's calls of it this was, from 0 */
    uint64_t frame;   /* the frame it was made in */
    uint32_t pages, inputs, callees;
    uint32_t named; /* of the pages, how many SOA_RECORD_WITH named (kept whether touched or not) */
    RecRegs in, out;
} RecCallHead;

typedef struct {
    uint32_t page;      /* its number: the offset in RAM over page_size */
    uint32_t flags;     /* REC_PAGE_READ, REC_PAGE_WRITTEN */
    uint32_t pre_bytes, post_bytes;
} RecPage;

typedef struct {
    uint32_t kind, ea; /* ea: an MMIO read's address */
    uint64_t value;
} RecInput;

typedef struct {
    uint32_t address, sp;
} RecCallee;

/* The registers a CpuState has that a call's record keeps. */
static inline void rec_regs_from(RecRegs* r, const CpuState* s)
{
    int i;
    for (i = 0; i < 32; i++) {
        r->gpr[i] = s->gpr[i];
        r->fpr[i][0] = s->fpr[i].ps0;
        r->fpr[i][1] = s->fpr[i].ps1;
    }
    r->cr = s->cr; r->xer = s->xer; r->lr = s->lr; r->ctr = s->ctr;
    r->fpscr = s->fpscr; r->msr = s->msr;
    for (i = 0; i < 8; i++) r->gqr[i] = s->gqr[i];
}

static inline void rec_regs_to(CpuState* s, const RecRegs* r)
{
    int i;
    for (i = 0; i < 32; i++) {
        s->gpr[i] = r->gpr[i];
        s->fpr[i].ps0 = r->fpr[i][0];
        s->fpr[i].ps1 = r->fpr[i][1];
    }
    s->cr = r->cr; s->xer = r->xer; s->lr = r->lr; s->ctr = r->ctr;
    s->fpscr = r->fpscr; s->msr = r->msr;
    for (i = 0; i < 8; i++) s->gqr[i] = r->gqr[i];
}

/* The encoding: page against base into out (room for twice the page and 8
 * bytes is always enough), its length returned; 0 when they are the same. */
static inline uint32_t rec_encode(const uint8_t* page, const uint8_t* base, uint32_t size, uint8_t* out)
{
    uint32_t at = 0, n = 0;
    int differs = 0;
    while (at < size) {
        uint32_t same = 0, lit = 0, k;
        while (at + same < size && same < 0xFFFFu && page[at + same] == base[at + same]) same++;
        /* a literal run ends at four bytes the same in a row (or the page's end) */
        while (at + same + lit < size && lit < 0xFFFFu) {
            uint32_t p = at + same + lit, q = 0;
            while (q < 4 && p + q < size && page[p + q] == base[p + q]) q++;
            if (q == 4 || (q > 0 && p + q == size)) break;
            lit += q ? q : 1;
            if (lit > 0xFFFFu) lit = 0xFFFFu;
        }
        out[n++] = (uint8_t)same; out[n++] = (uint8_t)(same >> 8);
        out[n++] = (uint8_t)lit; out[n++] = (uint8_t)(lit >> 8);
        for (k = 0; k < lit; k++) out[n++] = (uint8_t)(page[at + same + k] ^ base[at + same + k]);
        differs |= lit != 0;
        at += same + lit;
    }
    return differs ? n : 0;
}

/* Back again: base and the runs into page; 0 for runs that do not make a
 * whole page of size. */
static inline int rec_decode(uint8_t* page, const uint8_t* base, uint32_t size, const uint8_t* in, uint32_t n)
{
    uint32_t at = 0, i = 0;
    if (page != base) memcpy(page, base, size);
    if (!n) return 1;
    while (at < size) {
        uint32_t same, lit, k;
        if (i + 4 > n) return 0;
        same = in[i] | (uint32_t)in[i + 1] << 8;
        lit = in[i + 2] | (uint32_t)in[i + 3] << 8;
        i += 4;
        if (at + same + lit > size || i + lit > n) return 0;
        at += same;
        for (k = 0; k < lit; k++) page[at + k] ^= in[i + k];
        at += lit;
        i += lit;
    }
    return i == n;
}

/* main.c, once the game's table is there: SOA_RECORD read and armed. */
void record_env(CpuState* s);

#endif
