/*
 * The external interface: three channels of chip-selected serial devices.
 *
 *   channel 0, device 0  memory card in slot A (a file, formatted by the game)
 *   channel 0, device 1  the RTC/SRAM (settings and the clock), the IPL ROM (not provided)
 *   channel 1, device 0  slot B: empty
 *   channel 2            nothing
 *
 * Transfers are immediate (1-4 bytes through EXI_DATA) or DMA (EXI_MAR /
 * EXI_LENGTH); each completes at once with TCINT, and the TC interrupt is
 * raised when the SDK enables it. Devices see a byte stream framed by the
 * chip select, which is how the real ones work.
 *
 *   0xCC006800 + 0x14*ch: CSR, MAR, LENGTH, CR, DATA
 *   CSR: EXIINTMSK 0, EXIINT 1, TCINTMSK 2, TCINT 3, CLK 4-6, CS0B 7, CS1B 8, CS2B 9,
 *        EXTINTMSK 10, EXTINT 11, EXT 12 (device present), ROMDIS 13
 *   CR:  TSTART 0, DMA 1, RW 2-3 (0 read, 1 write, 2 read/write), TLEN 4-5 (bytes-1)
 *
 * A card has two interrupts, not one. TCINT says the bus finished moving
 * bytes; EXIINT is the card itself, pulling its interrupt line when a program
 * or erase it was left to do has finished. The CARD library waits on the
 * second one for every write, so a device that raises only the first makes
 * every save sit out a 100 ms timeout and fail.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "clock.h"
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

uint64_t irq_retrace_count(void);

#define EXI_BASE 0xCC006800u
#define CSR_EXIINTMSK 0x0001u
#define CSR_EXIINT 0x0002u
#define CSR_TCINTMSK 0x0004u
#define CSR_TCINT 0x0008u
#define CSR_EXT 0x1000u
#define CSR_W1C 0x080Au /* EXIINT, TCINT, EXTINT */
#define CSR_STORED 0x07F5u /* masks, clock, chip selects -- the mask the SDK itself uses */

typedef struct {
    uint32_t csr, mar, len, cr, data;
    int selected; /* device index or -1 */
    unsigned pos; /* byte position within the current transaction */
    uint32_t cmd; /* the command word being assembled */
} Channel;

/* Nothing is selected at power-on, and the first thing the boot code does is
 * select something: starting at device 0 would swallow that first framing. */
static Channel g_ch[3] = {{0, 0, 0, 0, 0, -1, 0, 0}, {0, 0, 0, 0, 0, -1, 0, 0}, {0, 0, 0, 0, 0, -1, 0, 0}};
static uint64_t g_imm, g_dma, g_card_reads, g_card_writes, g_card_ints;

/* One line per transaction under SOA_CARD_VERBOSE. Nothing else in a run
 * reports EXI traffic, which is why no saved log says which frame the game
 * probes the card on. */
static int card_verbose(void)
{
    static int v = -1;
    if (v < 0) v = getenv("SOA_CARD_VERBOSE") ? 1 : 0;
    return v;
}

/* ---- memory card ---------------------------------------------------------- */

#define CARD_BYTES (4u << 17) /* 4 Mbit: the 59-block card */
#define CARD_SECTOR 0x2000u
/* The EXI device ID the card answers command 0x00 with, and the only value
 * this device model is consistent with. CARDIsCard (0x8024863C) reads it as
 * three fields: bits 0-7 the size in Mbit, so 4 for our 512 KB; bits 11-13 an
 * index into the sector-size table at 0x802FB560, so 0 for the 0x2000 the
 * erase below uses; bits 8-10 an index into the latency table at 0x802FB580,
 * so 0 for the four dummy bytes __CARDReadSegment sends before the data. */
#define CARD_ID 0x00000004u
static uint8_t* g_card;
static uint32_t g_card_addr;
static uint8_t g_card_cmd;
static uint8_t g_card_status = 0x41; /* ready, unlocked */
static uint8_t g_card_int_en; /* command 0x81: the card may drive its interrupt line */
static uint8_t g_card_int_req; /* the line itself, until clear-status acknowledges it */
static int g_card_dirty;
static uint32_t g_card_dirty_lo, g_card_dirty_hi;
static FILE* g_card_file;
static const char* g_card_path = "build/cards/slotA.raw";

/* The flash chip's twelve-byte serial, used when the image on disk cannot
 * supply one. Any twelve bytes do -- nothing reads them for their value --
 * but they must be the same twelve every boot, or a card this port formatted
 * stops verifying against the SRAM the next run presents. */
static const uint8_t CARD_FLASH_ID[12] = {0x53, 0x4F, 0x41, 0x50, 0x4F, 0x52, 0x54, 0x00, 0x00, 0x00, 0x00, 0x01};

static void make_parents(const char* path)
{
    char dir[512];
    size_t i;
    for (i = 0; path[i] && i + 1 < sizeof dir; i++) {
        dir[i] = path[i];
        if (path[i] != '/' && path[i] != '\\') continue;
        dir[i] = 0;
        if (i > 0) {
#ifdef _WIN32
            _mkdir(dir);
#else
            mkdir(dir, 0777);
#endif
        }
        dir[i] = path[i];
    }
}

static void card_load(void)
{
    FILE* f;
    const char* env;
    if (g_card) return;
    env = getenv("SOA_CARD");
    if (env && *env) g_card_path = env;
    g_card = (uint8_t*)malloc(CARD_BYTES);
    memset(g_card, 0xFF, CARD_BYTES); /* blank flash */
    f = fopen(g_card_path, "rb");
    if (f) {
        long size;
        size_t n = fread(g_card, 1, CARD_BYTES, f);
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fclose(f);
        fprintf(stderr, "[exi] memory card %s (%zu bytes)\n", g_card_path, n);
        /* An image of another size is not this card. A bigger one is read and
         * written back through its first 512 KB and keeps its tail, which is
         * neither card; and the size the EXI ID advertises is what CARDIsCard
         * checks, so the game would call it BROKEN without saying why. */
        if (size >= 0 && (uint32_t)size != CARD_BYTES)
            fprintf(stderr, "[exi] %s is %ld bytes, not the %u of a %u Mbit card: only the first %u are used\n",
                    g_card_path, size, CARD_BYTES, CARD_BYTES >> 17, CARD_BYTES);
    } else {
        fprintf(stderr, "[exi] memory card %s: new blank card\n", g_card_path);
    }
}

/* Whether anything has opened the card yet. The selftest points SOA_CARD at an
 * image of its own and erases it, which is safe only while no card is open: if
 * something ever reads SRAM before the selftest runs, the path is already
 * cached and the erase would land on whatever card the run was using. */
int exi_card_loaded(void)
{
    return g_card != NULL;
}

static void card_dirty(uint32_t off, uint32_t len)
{
    if (!g_card_dirty) {
        g_card_dirty = 1;
        g_card_dirty_lo = off;
        g_card_dirty_hi = off + len;
        return;
    }
    if (off < g_card_dirty_lo) g_card_dirty_lo = off;
    if (off + len > g_card_dirty_hi) g_card_dirty_hi = off + len;
}

/* A save is not one write but a sector erase and then dozens of page programs
 * spread over several frames, so waiting for the run to end loses whatever a
 * kill interrupts. Each framed program writes its own bytes through and
 * flushes; the file stays open so that costs a seek, not a 512 KB rewrite. */
static void card_flush(void)
{
    if (!g_card || !g_card_dirty) return;
    if (!g_card_file) {
        make_parents(g_card_path);
        g_card_file = fopen(g_card_path, "r+b");
        if (!g_card_file) g_card_file = fopen(g_card_path, "w+b");
        if (!g_card_file) {
            fprintf(stderr, "[exi] cannot write %s\n", g_card_path);
            g_card_dirty = 0;
            return;
        }
        /* A file that did not exist, or one from a smaller card, has to reach
         * full size before a page in the middle can be written in place. */
        fseek(g_card_file, 0, SEEK_END);
        if (ftell(g_card_file) != (long)CARD_BYTES) {
            g_card_dirty_lo = 0;
            g_card_dirty_hi = CARD_BYTES;
        }
    }
    fseek(g_card_file, (long)g_card_dirty_lo, SEEK_SET);
    fwrite(g_card + g_card_dirty_lo, 1, g_card_dirty_hi - g_card_dirty_lo, g_card_file);
    fflush(g_card_file);
    g_card_dirty = 0;
}

void exi_card_save(void)
{
    card_flush();
}

/* The card's own 32-bit checksum pair, as __CARDCheckSum (0x80247728)
 * computes it: a u16 sum and a u16 sum of complements, each 0xFFFF folded
 * to 0. Every system block on the card carries one. */
static void card_checksum(const uint8_t* p, unsigned bytes, uint16_t* sum, uint16_t* inv)
{
    uint32_t s = 0, v = 0;
    unsigned i;
    for (i = 0; i < bytes; i += 2) {
        uint16_t w = (uint16_t)(((uint16_t)p[i] << 8) | p[i + 1]);
        s += w;
        v += (uint16_t)~w;
    }
    *sum = (uint16_t)s;
    *inv = (uint16_t)v;
    if (*sum == 0xFFFFu) *sum = 0;
    if (*inv == 0xFFFFu) *inv = 0;
}

/* The scrambling sequence the card library runs over the flash ID:
 * rand = (rand * 0x41C64E6D + 12345) >> 16, arithmetic, on 64 bits.
 * __CARDFormatRegionAsync adds successive bytes of it to the flash ID to make
 * the ID block's serial; CARDVerifyID subtracts them again and compares. */
static void card_scramble_step(int64_t* r)
{
    *r = ((int64_t)((uint64_t)*r * 0x41C64E6Du + 12345u)) >> 16;
}

/* Recover the flash ID a formatted image was made with. CARDVerifyID checks
 * the ID block's serial against SRAM, so an image formatted anywhere else
 * verifies only if we present the flash ID it carries. Deriving it makes the
 * arrangement a fixed point: the game formats a blank card with the constant
 * above, and the next boot reads that same constant back out of the header
 * the game wrote. Returns 0 for a blank or damaged image. */
static int card_flash_id(uint8_t out[12])
{
    const uint8_t* h = g_card;
    uint16_t sum, inv;
    int64_t rand;
    int i;
    card_checksum(h, 508, &sum, &inv);
    if ((((uint16_t)h[508] << 8) | h[509]) != sum) return 0;
    if ((((uint16_t)h[510] << 8) | h[511]) != inv) return 0;
    rand = (int64_t)(((uint64_t)h[12] << 56) | ((uint64_t)h[13] << 48) | ((uint64_t)h[14] << 40) |
                     ((uint64_t)h[15] << 32) | ((uint64_t)h[16] << 24) | ((uint64_t)h[17] << 16) |
                     ((uint64_t)h[18] << 8) | h[19]);
    for (i = 0; i < 12; i++) {
        card_scramble_step(&rand);
        out[i] = (uint8_t)(h[i] - (uint8_t)rand);
        card_scramble_step(&rand);
        rand &= 0x7FFF;
    }
    return 1;
}

/* One byte in each direction on the card's serial line. */
static uint8_t card_byte(Channel* c, uint8_t in)
{
    uint8_t out = 0xFF;
    card_load();
    if (c->pos == 0) {
        g_card_cmd = in;
        /* Clear status is a one-byte command, so its whole effect belongs
         * here. It is the acknowledge __CARDExiHandler sends after reading
         * status, and it is where the card lets go of its interrupt line. */
        if (in == 0x89) {
            g_card_status &= (uint8_t)~0x18u; /* the erase and program error bits */
            g_card_int_req = 0;
        }
        c->pos++;
        return 0xFF;
    }
    switch (g_card_cmd) {
    case 0x00: /* EXI device ID: two command bytes, then the ID most significant first */
        out = c->pos >= 2 ? (uint8_t)(CARD_ID >> (8 * (3 - ((c->pos - 2) & 3)))) : 0xFF;
        break;
    case 0x85: /* read ID */
        out = c->pos == 1 ? 0x80 : (c->pos == 2 ? 0xC2 : (c->pos == 3 ? 0x21 : 0x00));
        break;
    case 0x83: /* read status */
        out = g_card_status;
        break;
    case 0x52: /* read array: 4 address bytes, then data by DMA (or serially) */
        if (c->pos == 1) g_card_addr = (uint32_t)in << 17;
        else if (c->pos == 2) g_card_addr |= (uint32_t)in << 9;
        else if (c->pos == 3) g_card_addr |= (uint32_t)(in & 3) << 7;
        else if (c->pos == 4) g_card_addr |= in & 0x7F;
        else if (c->pos >= 9) { out = g_card[g_card_addr % CARD_BYTES]; g_card_addr++; g_card_reads++; }
        break;
    case 0xF2: /* page program: 4 address bytes, then 128 bytes */
        if (c->pos == 1) g_card_addr = (uint32_t)in << 17;
        else if (c->pos == 2) g_card_addr |= (uint32_t)in << 9;
        else if (c->pos == 3) g_card_addr |= (uint32_t)(in & 3) << 7;
        else if (c->pos == 4) g_card_addr |= in & 0x7F;
        else {
            uint32_t a = g_card_addr % CARD_BYTES;
            g_card[a] &= in;
            card_dirty(a, 1);
            g_card_addr++;
            g_card_writes++;
        }
        break;
    case 0xF1: /* sector erase: 2 address bytes (8 KB sectors) */
        if (c->pos == 1) g_card_addr = (uint32_t)in << 17;
        else if (c->pos == 2) {
            /* The command addresses a sector but names a byte inside it, and
             * erasing 8 KB from wherever that byte fell ran off the end of the
             * allocation for any address in the last sector. */
            uint32_t base;
            g_card_addr |= (uint32_t)in << 9;
            base = (g_card_addr % CARD_BYTES) & ~(CARD_SECTOR - 1u);
            memset(g_card + base, 0xFF, CARD_SECTOR);
            card_dirty(base, CARD_SECTOR);
        }
        break;
    case 0xF4: /* chip erase */
        if (c->pos == 2) { memset(g_card, 0xFF, CARD_BYTES); card_dirty(0, CARD_BYTES); }
        break;
    case 0x81: /* set interrupt: whether the card drives EXIINT when a write finishes */
        if (c->pos == 1) g_card_int_en = (uint8_t)(in & 1);
        break;
    case 0x87: /* wake up */
    case 0x88: /* sleep */
    case 0x86: /* read error buffer */
    default:
        break;
    }
    c->pos++;
    return out;
}

/* The chip select going away frames the transaction, and that is the first
 * instant at which a program or erase has all of its bytes and can start.
 * Two things follow from it: the bytes are worth writing through to disk, and
 * the card starts driving its interrupt line.
 *
 * The line is latched into CSR on its rising edge and never re-derived. That
 * matters because __CARDExiHandler clears the CSR bit first and only then
 * reads status and sends clear-status; a level-sensitive bit would still be
 * set when it returned and the handler would re-enter forever. The two
 * commands the handler sends are framed by their own deselects, but neither
 * 0x83 nor 0x89 starts anything, so neither can re-arm the latch. */
static void card_deselected(Channel* c)
{
    if (g_card_cmd != 0xF1 && g_card_cmd != 0xF2 && g_card_cmd != 0xF4) return;
    card_flush();
    if (g_card_int_en && !g_card_int_req) {
        g_card_int_req = 1;
        c->csr |= CSR_EXIINT;
        g_card_ints++;
    }
}

/* DMA against the card. A DMA is not a second way into the device but the same
 * byte stream the immediate path carries: the command that framed the
 * transaction is still in force and the byte position carries on from the
 * address and latency bytes that came before it. Driving both halves through
 * card_byte is what makes that true rather than nearly true -- a DMA that went
 * straight to the array would answer a status or erase command with flash
 * contents, and would return read data without the four latency bytes the
 * device ID asks for. */
static void card_dma(CpuState* s, Channel* c, uint32_t mar, uint32_t len, int send)
{
    uint32_t i;
    card_load();
    if ((mar & MEM_MASK) + len > MEM1_SIZE) {
        /* Dropped, and TCINT is set below either way, so say so: a save that
         * silently moved no bytes reads as a working one from the outside. */
        static int warned;
        if (!warned++) fprintf(stderr, "[exi] card DMA to %08X+%u is outside MEM1; dropped\n", mar, len);
        return;
    }
    for (i = 0; i < len; i++) {
        uint32_t ea = (mar | 0x80000000u) + i;
        if (send) card_byte(c, mem_r8(s, ea));
        else mem_w8(s, ea, card_byte(c, 0));
    }
}

/* ---- RTC / SRAM ----------------------------------------------------------- */

/* One device answers three ranges, addressed by a 32-bit command word: bit 31
 * is the write flag and bits 30-6 the address. The RTC counter sits at
 * 0x800000, the 64 bytes of SRAM at 0x800004, and the IPL ROM below both.
 * The payload streams on from the address the command named, which is why the
 * command's offset field has to be honoured -- __OSUnlockSramEx commits from
 * offset 20, so every write the game makes to the extended half of SRAM goes
 * there and nowhere else.
 *
 * SRAM's layout, from __OSInitSram (0x80236CB8), UnlockSram (0x80236EA4) and
 * __OSLockSramEx (0x80236E48), which returns SRAM + 20:
 *
 *    0..1  checkSum          12..15 counterBias      32..43 flashID[1]
 *    2..3  checkSumInv       16     displayOffsetH   44..55 wireless IDs
 *    4..7  ead0              17     ntd              56     dvdErrorCode
 *    8..11 ead1              18     language         58     flashIDCheckSum[0]
 *                            19     flags            59     flashIDCheckSum[1]
 *                            20..31 flashID[0]
 *
 * The checksum pair covers the four u16 at 12, 14, 16 and 18 and nothing
 * else. flashIDCheckSum is the complement of the byte sum of that slot's
 * flash ID: CARDDoMount recomputes it from SRAM alone and returns IOERROR if
 * the two disagree, which is the gate the mount has never got past. */
#define RTC_COUNTER_ADDR 0x800000u
#define SRAM_ADDR 0x800004u

static uint8_t g_sram[64];
static int g_sram_init;
static uint32_t g_rtc_cmd, g_rtc_latch;

static void sram_init(void)
{
    uint32_t sum = 0, inv = 0, idsum = 0;
    unsigned i;
    if (g_sram_init) return;
    g_sram_init = 1;
    memset(g_sram, 0, sizeof g_sram);
    /* ead0/ead1 0, counterBias 0, displayOffsetH 0, ntd 0, language English.
     * OSGetSoundMode reads bit 2 of byte 19: the console default is stereo,
     * and a zero there configures the game's audio driver for mono. */
    g_sram[19] = 0x04;
    card_load();
    if (!card_flash_id(g_sram + 20)) memcpy(g_sram + 20, CARD_FLASH_ID, sizeof CARD_FLASH_ID);
    for (i = 0; i < 12; i++) idsum += g_sram[20 + i];
    g_sram[58] = (uint8_t)~idsum;
    g_sram[59] = 0xFF; /* slot B has no card, and an all-zero flash ID sums to zero */
    for (i = 12; i < 20; i += 2) {
        uint16_t w = (uint16_t)(((uint16_t)g_sram[i] << 8) | g_sram[i + 1]);
        sum += w;
        inv += (uint16_t)~w;
    }
    g_sram[0] = (uint8_t)(sum >> 8); g_sram[1] = (uint8_t)sum;
    g_sram[2] = (uint8_t)(inv >> 8); g_sram[3] = (uint8_t)inv;
}

static uint32_t rtc_seconds(void)
{
    /* seconds since 2000-01-01 00:00:00 local */
    time_t now = time(NULL);
    struct tm t2000;
    if (clock_fixed()) return clock_fixed_rtc(); /* SOA_CLOCK=fixed: a fixed date, plus guest time (docs/CLOCK.md) */
    memset(&t2000, 0, sizeof t2000);
    t2000.tm_year = 100; t2000.tm_mon = 0; t2000.tm_mday = 1;
    return (uint32_t)(now - mktime(&t2000));
}

static uint8_t rtc_byte(Channel* c, uint8_t in)
{
    uint32_t addr, off;
    uint8_t out = 0;
    sram_init();
    if (c->pos < 4) {
        g_rtc_cmd = (g_rtc_cmd << 8) | in;
        if (c->pos == 3) g_rtc_latch = rtc_seconds(); /* one reading per transaction */
        c->pos++;
        return 0;
    }
    off = c->pos - 4;
    addr = (g_rtc_cmd >> 6) & 0x01FFFFFFu;
    c->pos++;
    if (addr >= SRAM_ADDR && addr < SRAM_ADDR + sizeof g_sram) {
        off += addr - SRAM_ADDR;
        if (off >= sizeof g_sram) return 0;
        if (g_rtc_cmd >> 31) g_sram[off] = in;
        else out = g_sram[off];
    } else if (addr >= RTC_COUNTER_ADDR && addr < SRAM_ADDR && !(g_rtc_cmd >> 31)) {
        off += addr - RTC_COUNTER_ADDR;
        if (off < 4) out = (uint8_t)(g_rtc_latch >> (8 * (3 - off)));
    }
    /* Below 0x800000 is the IPL ROM, which holds the fonts: not provided. */
    return out;
}

/* ---- channels ---------------------------------------------------------------- */

static uint8_t dev_byte(unsigned ch, Channel* c, uint8_t in)
{
    if (ch == 0 && c->selected == 0) return card_byte(c, in);
    if (ch == 0 && c->selected == 1) return rtc_byte(c, in);
    return 0xFF; /* nothing there */
}

static void exi_log(unsigned ch, const Channel* c, unsigned rw, unsigned pos, uint32_t in, uint32_t addr)
{
    unsigned long long frame = (unsigned long long)irq_retrace_count();
    char at[32];
    at[0] = 0;
    if (ch == 0 && c->selected == 0) snprintf(at, sizeof at, " card %06X cmd %02X", addr % CARD_BYTES, g_card_cmd);
    if (c->cr & 2)
        fprintf(stderr, "[card] f%-6llu ch%u dev%d dma %c%u mar %08X%s\n", frame, ch, c->selected,
                rw == 1 ? 'w' : 'r', c->len & ~31u, c->mar | 0x80000000u, at);
    else
        fprintf(stderr, "[card] f%-6llu ch%u dev%d imm %c%u %08X pos %u%s\n", frame, ch, c->selected,
                rw == 1 ? 'w' : 'r', ((c->cr >> 4) & 3) + 1, rw == 1 ? in : c->data, pos, at);
}

static void transfer(CpuState* s, unsigned ch, Channel* c)
{
    unsigned rw = (c->cr >> 2) & 3, pos = c->pos;
    uint32_t in0 = c->data, addr0 = g_card_addr;
    if (c->cr & 2) { /* DMA */
        uint32_t len = c->len & ~31u;
        g_dma++;
        if (ch == 0 && c->selected == 0) card_dma(s, c, c->mar, len, rw == 1);
        else if (ch == 0 && c->selected == 1) {
            /* Both directions: a DMA write the device dropped would still
             * clear TSTART and set TCINT below, so the guest would be told
             * its SRAM had been committed when nothing had moved. */
            uint32_t i;
            for (i = 0; i < len && (c->mar & MEM_MASK) + i < MEM1_SIZE; i++) {
                uint32_t ea = (c->mar | 0x80000000u) + i;
                if (rw == 1) rtc_byte(c, mem_r8(s, ea));
                else mem_w8(s, ea, rtc_byte(c, 0));
            }
        }
    } else { /* immediate: TLEN+1 bytes through DATA, most significant first */
        unsigned n = ((c->cr >> 4) & 3) + 1, i;
        uint32_t in = c->data, out = 0;
        g_imm++;
        for (i = 0; i < n; i++) {
            uint8_t b = (uint8_t)(in >> (8 * (3 - i)));
            uint8_t r = dev_byte(ch, c, rw == 1 ? b : (rw == 0 ? 0 : b));
            out |= (uint32_t)r << (8 * (3 - i));
        }
        if (rw != 1) c->data = out;
    }
    c->cr &= ~1u;          /* TSTART clears: done */
    c->csr |= CSR_TCINT;   /* transfer complete */
    /* A DMA reports where it started; an immediate reports where the address
     * bytes it just carried left the card, which is the page a program or
     * read is about to work on. */
    if (card_verbose()) exi_log(ch, c, rw, pos, in0, (c->cr & 2) ? addr0 : g_card_addr);
}

int exi_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out)
{
    unsigned ch, reg;
    (void)s;
    if (ea < EXI_BASE || ea >= EXI_BASE + 0x40 || size != 4) return 0;
    ch = (ea - EXI_BASE) / 0x14;
    reg = (ea - EXI_BASE) % 0x14;
    if (ch > 2) return 0;
    switch (reg) {
    case 0x00: *out = g_ch[ch].csr | (ch == 0 ? CSR_EXT : 0); return 1;
    case 0x04: *out = g_ch[ch].mar; return 1;
    case 0x08: *out = g_ch[ch].len; return 1;
    case 0x0C: *out = g_ch[ch].cr; return 1;
    case 0x10: *out = g_ch[ch].data; return 1;
    default: return 0;
    }
}

int exi_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v)
{
    unsigned ch, reg;
    uint32_t w = (uint32_t)v;
    Channel* c;
    if (ea < EXI_BASE || ea >= EXI_BASE + 0x40 || size != 4) return 0;
    ch = (ea - EXI_BASE) / 0x14;
    reg = (ea - EXI_BASE) % 0x14;
    if (ch > 2) return 0;
    c = &g_ch[ch];
    switch (reg) {
    case 0x00: {
        int sel = -1;
        c->csr = (c->csr & CSR_W1C & ~(w & CSR_W1C)) | (w & CSR_STORED);
        if (w & 0x0080) sel = 0; else if (w & 0x0100) sel = 1; else if (w & 0x0200) sel = 2;
        if (sel != c->selected) {
            if (ch == 0 && c->selected == 0) card_deselected(c);
            c->selected = sel;
            c->pos = 0;
            g_rtc_cmd = 0;
        }
        return 1;
    }
    case 0x04: c->mar = w & 0x03FFFFE0u; return 1;
    case 0x08: c->len = w & 0x03FFFFE0u; return 1;
    case 0x0C:
        c->cr = w & 0x3Fu;
        if (w & 1) transfer(s, ch, c);
        return 1;
    case 0x10: c->data = w; return 1;
    default: return 1;
    }
}

/* Bit per channel: a completed transfer with its interrupt enabled. */
unsigned exi_irq_pending(void)
{
    unsigned bits = 0, ch;
    for (ch = 0; ch < 3; ch++)
        if ((g_ch[ch].csr & CSR_TCINT) && (g_ch[ch].csr & CSR_TCINTMSK)) bits |= 1u << ch;
    return bits;
}

/* Bit per channel: a device pulling its interrupt line, with that interrupt
 * enabled. The enable is the whole of the guest's gate -- SetInterruptMask
 * (0x802357A0) mirrors the OS mask for interrupt 9 into CSR bit 0, so EXILock
 * clearing it is what keeps __CARDExiHandler's own transactions from
 * delivering a second interrupt on top of the one it is handling. */
unsigned exi_exi_irq_pending(void)
{
    unsigned bits = 0, ch;
    for (ch = 0; ch < 3; ch++)
        if ((g_ch[ch].csr & CSR_EXIINT) && (g_ch[ch].csr & CSR_EXIINTMSK)) bits |= 1u << ch;
    return bits;
}

/* Forget the open image, so the next access loads it from disk again. That is
 * what the next boot does, and it is the only way to exercise the path that
 * reads an image this run did not write -- the one a Dolphin-formatted card
 * arrives by. The flash ID in SRAM is recovered from the image, so SRAM has to
 * be rebuilt with it. */
void exi_card_reset(void)
{
    card_flush();
    if (g_card_file) { fclose(g_card_file); g_card_file = NULL; }
    free(g_card);
    g_card = NULL;
    g_card_addr = 0;
    g_card_cmd = 0;
    g_card_int_en = 0;
    g_card_int_req = 0;
    g_sram_init = 0;
}

/* The three numbers exi_report prints, for a caller that wants to assert them
 * rather than read them: a device model that answers reads out of a buffer
 * without counting them would leave the report saying 0 bytes read forever,
 * which is what every log so far has said. */
void exi_card_counters(uint64_t* reads, uint64_t* writes, uint64_t* ints)
{
    if (reads) *reads = g_card_reads;
    if (writes) *writes = g_card_writes;
    if (ints) *ints = g_card_ints;
}

void exi_report(void)
{
    exi_card_save();
    fprintf(stderr, "[exi] %llu immediate, %llu DMA transfers; card %llu bytes read, %llu written, %llu interrupts\n",
            (unsigned long long)g_imm, (unsigned long long)g_dma, (unsigned long long)g_card_reads,
            (unsigned long long)g_card_writes, (unsigned long long)g_card_ints);
}

/* ---- a snapshot (snapshot.h; docs/SNAPSHOT.md) ------------------------------
 * The three channels, the card's command state, SRAM and the RTC's latch, and
 * the card's whole image as the game has written it so far. The card file is
 * written through as the game saves, so the run that took the snapshot has
 * moved its file on since: a resumed run writes the snapshot's image to its
 * own card (SOA_CARD) at once, and goes on from there. */
#include "snapshot.h"
void exi_snap(Snap* sn)
{
    void* card = g_card;
    size_t bytes = g_card ? (size_t)CARD_BYTES : 0u;
    int dirty;
    uint32_t lo, hi;
    dirty = g_card_dirty, lo = g_card_dirty_lo, hi = g_card_dirty_hi;
    SNAP(sn, g_ch);
    SNAP(sn, g_card_addr);
    SNAP(sn, g_card_cmd);
    SNAP(sn, g_card_status);
    SNAP(sn, g_card_int_en);
    SNAP(sn, g_card_int_req);
    SNAP(sn, dirty);
    SNAP(sn, lo);
    SNAP(sn, hi);
    SNAP(sn, g_sram);
    SNAP(sn, g_sram_init);
    SNAP(sn, g_rtc_cmd);
    SNAP(sn, g_rtc_latch);
    SNAP(sn, g_imm);
    SNAP(sn, g_dma);
    SNAP(sn, g_card_reads);
    SNAP(sn, g_card_writes);
    SNAP(sn, g_card_ints);
    if (snap_loading(sn)) card = NULL;
    snap_buffer(sn, &card, &bytes);
    if (!snap_loading(sn) || snap_failed(sn)) return;
    if (bytes && bytes != (size_t)CARD_BYTES) {
        free(card);
        snap_fail(sn, "the card image is %zu bytes, not %u", bytes, (unsigned)CARD_BYTES);
        return;
    }
    if (card) {
        const char* env = getenv("SOA_CARD");
        if (env && *env) g_card_path = env; /* as card_load names it */
        free(g_card);
        g_card = (uint8_t*)card;
        if (g_card_file) {
            fclose(g_card_file);
            g_card_file = NULL;
        }
        g_card_dirty = 1, g_card_dirty_lo = 0, g_card_dirty_hi = CARD_BYTES;
        card_flush();
        fprintf(stderr, "[exi] memory card %s: the snapshot's image written to it\n", g_card_path);
    }
    g_card_dirty = dirty, g_card_dirty_lo = lo, g_card_dirty_hi = hi;
}
