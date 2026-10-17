/*
 * Call recompiled library functions directly, outside the game's control
 * flow, and check what they produce. SOA_SELFTEST=1 runs these instead of
 * the game: a wrong answer here is a translation bug with a tiny repro.
 *
 * The C library is position-independent enough for this -- sprintf needs
 * nothing but the small-data bases the startup code sets in r2 and r13.
 */
#include "cpu.h"
#include "gxr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint8_t* aram_memory(void);
int device_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out);
void device_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v);
unsigned exi_exi_irq_pending(void);
int exi_card_loaded(void);
void exi_card_reset(void);
void exi_card_counters(uint64_t* reads, uint64_t* writes, uint64_t* ints);
/* irq.c: the slot the delivery path reads a handler out of, how many
 * deliveries each interrupt number has had, and the idle-loop hook that is
 * the port's one delivery point. */
uint32_t irq_handler_slot(CpuState* s, unsigned irq);
uint64_t irq_delivered_count(unsigned number);
void hook_80237BA8(CpuState* s);

#define SCRATCH 0x81000000u /* above the arena; nothing else is there yet */
#define SDA2_BASE 0x80350000u
#define SDA_BASE 0x8034E720u
#define STACK_TOP 0x8035D4C8u

static void put_string(CpuState* s, uint32_t addr, const char* text)
{
    size_t i;
    for (i = 0; i <= strlen(text); i++) mem_w8(s, addr + (uint32_t)i, (uint8_t)text[i]);
}

static void get_string(CpuState* s, uint32_t addr, char* out, size_t cap)
{
    size_t i;
    for (i = 0; i + 1 < cap; i++) {
        out[i] = (char)mem_r8(s, addr + (uint32_t)i);
        if (!out[i]) return;
    }
    out[i] = 0;
}

static void call(CpuState* s, uint32_t fn)
{
    s->gpr[1] = STACK_TOP - 0x400;
    s->gpr[2] = SDA2_BASE;
    s->gpr[13] = SDA_BASE;
    s->lr = 0;
    s->cr = 0; /* CR bit 6 clear: no floating-point varargs */
    s->msr = 0x00002030u;
    dispatch(s, fn);
}

/* What the render checks need from the environment, set outside the block
 * tools/citest/render_driver.c copies, because the two set the thread count
 * differently: here it is always one, so the port's self test never changes.
 * SOA_SNAP is removed: an interval left in the environment would skip the one
 * frame the checks draw. */
static void render_env(void)
{
    plat_setenv("SOA_RENDER", "1");
    plat_setenv("SOA_THREADS", "1");
    plat_setenv("SOA_SNAP", NULL);
}

static int check(const char* what, const char* got, const char* want)
{
    int ok = strcmp(got, want) == 0;
    fprintf(stderr, "[selftest] %-28s %s  got \"%s\"%s%s%s\n", what, ok ? "ok  " : "FAIL", got,
            ok ? "" : " want \"", ok ? "" : want, ok ? "" : "\"");
    return !ok;
}

/* ---- the software renderer, through the real GX pipe --------------------
 * The register recipe is the one the game uses for its full-screen fades
 * (orthographic, 640x480, one colour channel from the vertex), so this also
 * pins down the command encodings the front end parses. */
static void gp8(CpuState* s, unsigned v) { gx_pipe_write(s, 1, v); }
static void gp16(CpuState* s, unsigned v) { gx_pipe_write(s, 2, v); }
static void gp32(CpuState* s, uint32_t v) { gx_pipe_write(s, 4, v); }
static void gpf(CpuState* s, float f) { uint32_t u; memcpy(&u, &f, 4); gp32(s, u); }
static void bp_w(CpuState* s, unsigned reg, uint32_t v) { gp8(s, 0x61); gp32(s, ((uint32_t)reg << 24) | (v & 0xFFFFFFu)); }
static void cp_w(CpuState* s, unsigned reg, uint32_t v) { gp8(s, 0x08); gp8(s, reg); gp32(s, v); }
static void xf_w(CpuState* s, unsigned addr, unsigned n, const uint32_t* w)
{
    unsigned i;
    gp8(s, 0x10); gp16(s, n - 1); gp16(s, addr);
    for (i = 0; i < n; i++) gp32(s, w[i]);
}
static void xf_f(CpuState* s, unsigned addr, unsigned n, const float* f)
{
    unsigned i;
    gp8(s, 0x10); gp16(s, n - 1); gp16(s, addr);
    for (i = 0; i < n; i++) gpf(s, f[i]);
}
static void vertex(CpuState* s, float x, float y, float z, uint32_t rgba) { gpf(s, x); gpf(s, y); gpf(s, z); gp32(s, rgba); }

static void present(CpuState* s)
{
    bp_w(s, 0x49, 0); bp_w(s, 0x4A, 0x077E7F); bp_w(s, 0x4D, 0x28); bp_w(s, 0x4B, (0x81100000u & 0x1FFFFFFFu) >> 5);
    bp_w(s, 0x52, 0x4003); /* copy to the XFB: the frame is done */
    gxr_flush();
}

static unsigned count_color(const uint8_t* screen, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    unsigned n = 0;
    int x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            const uint8_t* p = screen + ((size_t)y * EFB_W + x) * 4;
            if (p[0] == r && p[1] == g && p[2] == b) n++;
        }
    return n;
}

static int render_selftest(CpuState* s, char* got, size_t cap)
{
    static const float viewport[6] = {320.0f, -240.0f, 16777215.0f, 662.0f, 582.0f, 16777215.0f};
    static const float ortho[6] = {0.003125f, -0.0f, 0.004167f, -0.0f, -0.01f, -1.0f};
    static const float view[12] = {1, 0, 0, -320, 0, -1, 0, 240, 0, 0, 1, -100};
    static const uint32_t one = 1, matidx = 0x3CF3CF00u, chan = 0x441u, zero = 0;
    const uint8_t* screen;
    int w = 0, h = 0, failures = 0;

    render_env();
    if (!gxr_enabled()) { fprintf(stderr, "[selftest] renderer disabled; skipping render checks\n"); return 0; }
    gxr_reset_efb();

    bp_w(s, 0x59, 0x02ACABu); bp_w(s, 0x20, 0x156156u); bp_w(s, 0x21, 0x3D5335u); /* scissor: the full 640x480 */
    xf_f(s, 0x101A, 6, viewport);
    xf_f(s, 0x1020, 6, ortho); xf_w(s, 0x1026, 1, &one);
    xf_f(s, 0x0000, 12, view);
    cp_w(s, 0x30, matidx); xf_w(s, 0x1018, 1, &matidx);
    xf_w(s, 0x1009, 1, &one); xf_w(s, 0x100E, 1, &chan); xf_w(s, 0x1010, 1, &chan); xf_w(s, 0x103F, 1, &zero); xf_w(s, 0x1008, 1, &one);
    bp_w(s, 0x28, 0); bp_w(s, 0xC0, 0x08AFFFu); bp_w(s, 0xC1, 0x08BFF0u); bp_w(s, 0x00, 0x10);
    /* swap tables (GXSetTevSwapModeTable): identity for the rasterized and texture colours */
    bp_w(s, 0xF6, 0x018064u); bp_w(s, 0xF7, 0x01806Eu); bp_w(s, 0xF8, 0x018060u); bp_w(s, 0xF9, 0x01806Cu);
    bp_w(s, 0xFA, 0x018065u); bp_w(s, 0xFB, 0x01806Du); bp_w(s, 0xFC, 0x01806Au); bp_w(s, 0xFD, 0x01806Eu);
    bp_w(s, 0x40, 0x17); bp_w(s, 0x41, 0x18); bp_w(s, 0xF3, 0x3F0000u); bp_w(s, 0x43, 0x40);
    cp_w(s, 0x50, 0x2200); cp_w(s, 0x60, 0); cp_w(s, 0x70, 0x41377009u); cp_w(s, 0x80, 0xC8241209u); cp_w(s, 0x90, 0x04824120u);

    /* A full-screen red quad covers every pixel. */
    gp8(s, 0x80); gp16(s, 4);
    vertex(s, 0, 0, 50, 0xFF0000FFu); vertex(s, 640, 0, 50, 0xFF0000FFu);
    vertex(s, 640, 480, 50, 0xFF0000FFu); vertex(s, 0, 480, 50, 0xFF0000FFu);
    present(s);
    screen = gxr_screen(&w, &h);
    snprintf(got, cap, "%u of %d red", screen ? count_color(screen, w, h, 255, 0, 0) : 0u, w * h);
    failures += check("render full-screen quad", got, "307200 of 307200 red");

    /* The title screen's cloud triangle: (320.5,-22.1) (789.1,-22.1) (789.1,530.2).
     * Its top edge is off horizontal by a sub-pixel amount (as the perspective
     * divide leaves it): its x coefficient is a crumb, and the span code
     * once divided by it, overflowed the int conversion and dropped rows. The part
     * inside the screen is right of the diagonal, about 53,300 pixels. */
    gxr_reset_efb();
    gp8(s, 0x90); gp16(s, 3);
    vertex(s, 320.5f, -22.1f, 50, 0x00FF00FFu); vertex(s, 789.1f, -22.09999f, 50, 0x00FF00FFu); vertex(s, 789.1f, 530.2f, 50, 0x00FF00FFu);
    present(s);
    screen = gxr_screen(&w, &h);
    {
        unsigned green = screen ? count_color(screen, w, h, 0, 255, 0) : 0u;
#define G(x, y) (screen && screen[((size_t)(y) * EFB_W + (x)) * 4 + 1] == 255)
        int ok = green > 52000 && green < 54500 && G(600, 300) && G(630, 10) && G(500, 100) && !G(360, 60) && !G(600, 340) && !G(100, 200);
#undef G
        snprintf(got, cap, "%s, %u px", ok ? "complete" : "holes", green);
        failures += check("render triangle rows", got, ok ? got : "complete, ~53300 px");
    }
    return failures;
}

/* ---- the memory card, through the EXI registers -------------------------
 * Shaped like the ARAM round-trip further down: drive the device model the
 * way the CARD library drives it and check what comes back. Every command the
 * library sends is here -- device ID, read status, set interrupt, clear
 * status, sector erase, page program and read array -- together with the
 * interrupt each write ends with and the SRAM the mount checksums, because a
 * card that answers most of those and gets one wrong is a game that quietly
 * never saves, which is what every log so far has shown.
 *
 * It runs first, on an image of its own: the flash ID the runtime presents in
 * SRAM is recovered from the card image the first time anything reads SRAM,
 * so the image has to be shaped before that happens. It is written below the
 * render checks rather than above them because tools/citest/render_driver.c
 * copies check() and the render section as one block, which is the only way
 * CI can run them. */
#define EXI0_CSR 0xCC006800u
#define EXI0_MAR 0xCC006804u
#define EXI0_LEN 0xCC006808u
#define EXI0_CR 0xCC00680Cu
#define EXI0_DATA 0xCC006810u
#define EXIINTMSK 0x0001u
#define EXIINT 0x0002u
#define CARD_IMAGE "build/cards/selftest.raw"
/* EXIIntrruptHandler: the handler EXIInit registers for interrupts 9, 12 and
 * 15. It recovers its channel as (number - 9) / 3, clears the channel's EXIINT
 * and calls the channel's EXI callback if one is set -- none is here, so it is
 * the bit-clearing half alone. */
#define EXI_HANDLER 0x802643B8u
/* The traffic the checks below add up to: six 512-byte read-array transfers
 * and one 32-byte read that starts inside the latency, ten 128-byte page
 * programs, and one interrupt for each program or erase the card was allowed
 * to raise one for. */
#define CARD_READS 3100u
#define CARD_WRITES 1280u
#define CARD_INTS 6u

/* EXISelect keeps the three mask bits and sets one chip select; EXIDeselect
 * clears the selects and leaves the masks. Neither writes a one to a
 * write-one-to-clear bit, which is why a latched interrupt survives both. */
static void exi_select(CpuState* s, int dev)
{
    device_write(s, EXI0_CSR, 4, EXIINTMSK | (uint64_t)(0x80u << dev));
}

static void exi_deselect(CpuState* s)
{
    device_write(s, EXI0_CSR, 4, EXIINTMSK);
}

static uint32_t exi_csr(CpuState* s)
{
    uint64_t v = 0;
    device_read(s, EXI0_CSR, 4, &v);
    return (uint32_t)v;
}

static uint32_t exi_imm(CpuState* s, uint32_t data, unsigned len, unsigned type)
{
    uint64_t v = 0;
    device_write(s, EXI0_DATA, 4, data);
    device_write(s, EXI0_CR, 4, 1u | (type << 2) | ((len - 1) << 4));
    device_read(s, EXI0_DATA, 4, &v);
    return (uint32_t)v;
}

static void exi_dma(CpuState* s, uint32_t mar, uint32_t len, unsigned type)
{
    device_write(s, EXI0_MAR, 4, mar);
    device_write(s, EXI0_LEN, 4, len);
    device_write(s, EXI0_CR, 4, 1u | 2u | (type << 2));
}

/* The five-byte address commands, split four and one the way EXIImmEx splits
 * them: the byte position has to carry across the two transfers. */
static void card_addr_cmd(CpuState* s, uint8_t op, uint32_t addr)
{
    exi_imm(s,
            ((uint32_t)op << 24) | (((addr >> 17) & 0x7F) << 16) | (((addr >> 9) & 0xFF) << 8) | ((addr >> 7) & 3), 4,
            1);
    exi_imm(s, (addr & 0x7F) << 24, 1, 1);
}

static void card_erase(CpuState* s, uint32_t addr)
{
    exi_select(s, 0);
    exi_imm(s, 0xF1000000u | (((addr >> 17) & 0x7F) << 16) | (((addr >> 9) & 0xFF) << 8), 3, 1);
    exi_deselect(s);
}

static void card_program(CpuState* s, uint32_t addr, uint32_t src)
{
    exi_select(s, 0);
    card_addr_cmd(s, 0xF2, addr);
    exi_dma(s, src, 128, 1);
    exi_deselect(s);
}

static void card_read(CpuState* s, uint32_t addr, uint32_t dst, uint32_t len)
{
    exi_select(s, 0);
    card_addr_cmd(s, 0x52, addr);
    exi_imm(s, 0, 4, 1); /* the four dummy bytes the ID's latency field asks for */
    exi_dma(s, dst, len, 0);
    exi_deselect(s);
}

/* What __CARDExiHandler does with the interrupt, in its order: clear the CSR
 * bit, then re-select the card to read status and acknowledge it. The card is
 * still asserting for the whole of that -- it only lets go at the
 * acknowledge -- so `midway', read between the two, is the moment that tells
 * a latched interrupt from a level-sensitive one. It must be 0: a runtime
 * that re-derives the bit from the card's line, or re-arms on the deselect
 * that ends the status read, sends the handler round again for ever. */
static uint8_t card_handle_interrupt(CpuState* s, unsigned* midway)
{
    uint8_t status;
    device_write(s, EXI0_CSR, 4, (exi_csr(s) & 0x07F5u) | EXIINT);
    exi_select(s, 0);
    exi_imm(s, 0x83000000u, 2, 1);
    status = (uint8_t)(exi_imm(s, 0, 1, 0) >> 24);
    exi_deselect(s);
    *midway = ((exi_csr(s) & EXIINT) ? 1u : 0u) | ((exi_exi_irq_pending() & 1u) << 1);
    exi_select(s, 0);
    exi_imm(s, 0x89000000u, 1, 1);
    exi_deselect(s);
    return status;
}

/* __CARDCheckSum: a u16 sum and a u16 sum of complements, 0xFFFF folded to 0.
 * Written out again here rather than shared with the runtime, so the two have
 * to agree rather than being wrong together. */
static void card_sum(const uint8_t* p, unsigned bytes, unsigned* sum, unsigned* inv)
{
    unsigned i, s = 0, v = 0;
    for (i = 0; i < bytes; i += 2) {
        unsigned w = ((unsigned)p[i] << 8) | p[i + 1];
        s += w;
        v += (unsigned)(uint16_t)~w;
    }
    *sum = s & 0xFFFFu;
    *inv = v & 0xFFFFu;
    if (*sum == 0xFFFFu) *sum = 0;
    if (*inv == 0xFFFFu) *inv = 0;
}

static int64_t card_rand(int64_t r)
{
    return ((int64_t)((uint64_t)r * 0x41C64E6Du + 12345u)) >> 16;
}

static int card_selftest(CpuState* s, char* got, size_t cap)
{
    /* Any twelve bytes: what matters is that the runtime gives these back. */
    static const uint8_t flash[12] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC};
    const uint32_t SRC = SCRATCH + 0x2000, DST = SCRATCH + 0x3000, SRB = SCRATCH + 0x4000;
    const uint32_t sector = 0x2000, page = sector + 0x100;
    uint8_t id_block[512], sram[64], when[8];
    uint64_t reads, writes, ints, before;
    char want[128];
    FILE* stale;
    int failures = 0, i, bad;
    unsigned sum, inv, midway;
    int64_t rand;
    uint32_t id, stamp;

    /* Redirecting SOA_CARD works only while no card has been opened, because
     * card_load caches the path on first use. Nothing before this point in
     * main() touches EXI today; the day something does, this says so instead
     * of quietly erasing and reformatting whatever card the run was using. */
    if (exi_card_loaded()) {
        failures += check("card untouched at entry", "a card is already open", "nothing opened yet");
        return failures;
    }
    plat_setenv("SOA_CARD", CARD_IMAGE);

    /* The checks below describe a card nobody has written, so the image from
     * the last run has to be gone. If it is not -- a stale handle, a
     * read-only file -- every check still passes against those old bytes and
     * the run proves nothing, so fail here instead. */
    remove(CARD_IMAGE);
    stale = fopen(CARD_IMAGE, "rb");
    if (stale) fclose(stale);
    snprintf(got, cap, "%s", stale ? "still there" : "gone");
    failures += check("card image starts absent", got, "gone");

    /* And the image this run writes is not the image the last one wrote: the
     * format time seeds the scrambled serial, so every byte of the ID block
     * downstream of it differs run to run. A check that reads back what a
     * previous run left behind then fails rather than passing. */
    stamp = (uint32_t)time(NULL) ^ (uint32_t)clock();
    memset(when, 0, sizeof when);
    when[4] = (uint8_t)(stamp >> 24); when[5] = (uint8_t)(stamp >> 16);
    when[6] = (uint8_t)(stamp >> 8); when[7] = (uint8_t)stamp;

    /* EXIGetID writes two command bytes and then reads four, so the ID has to
     * land at transaction positions 2 to 5. CARDIsCard reads 0x00000004 as
     * 4 Mbit, 8 KB sectors and four dummy bytes before read data -- the only
     * value the rest of this device model is consistent with. */
    exi_select(s, 0);
    exi_imm(s, 0, 2, 1);
    id = exi_imm(s, 0, 4, 0);
    exi_deselect(s);
    snprintf(got, cap, "%08X", id);
    failures += check("card device ID", got, "00000004");

    exi_select(s, 0);
    exi_imm(s, 0x83000000u, 2, 1);
    snprintf(got, cap, "%02X", (unsigned)(exi_imm(s, 0, 1, 0) >> 24));
    exi_deselect(s);
    failures += check("card read status", got, "41"); /* ready, and unlocked */

    /* An ID block as __CARDFormatRegionAsync writes one: the flash ID with
     * successive values of the scrambling sequence added to it, seeded from
     * the format time, and the block's own checksum pair at 508 and 510. */
    memset(id_block, 0xFF, sizeof id_block);
    memcpy(id_block + 12, when, sizeof when);
    rand = 0;
    for (i = 0; i < 8; i++) rand = (rand << 8) | when[i];
    for (i = 0; i < 12; i++) {
        rand = card_rand(rand);
        id_block[i] = (uint8_t)(flash[i] + (uint8_t)rand);
        rand = card_rand(rand) & 0x7FFF;
    }
    id_block[32] = id_block[33] = 0; /* deviceID */
    id_block[34] = 0; id_block[35] = 4; /* size, in Mbit */
    /* encode: CARDVerifyID compares it against the font encoding CARDInit
     * cached, and returns ENCODING otherwise. OSGetFontEncode answers 0 for a
     * US NTSC console, so the block this builds is one the mount would take
     * rather than one shaped only well enough for the device model. */
    id_block[36] = id_block[37] = 0;
    card_sum(id_block, 508, &sum, &inv);
    id_block[508] = (uint8_t)(sum >> 8); id_block[509] = (uint8_t)sum;
    id_block[510] = (uint8_t)(inv >> 8); id_block[511] = (uint8_t)inv;

    card_erase(s, 0);
    for (i = 0; i < 4; i++) {
        int j;
        for (j = 0; j < 128; j++) mem_w8(s, SRC + (uint32_t)j, id_block[i * 128 + j]);
        card_program(s, (uint32_t)i * 128, SRC);
    }
    card_read(s, 0, DST, 512);
    for (bad = -1, i = 0; i < 512 && bad < 0; i++)
        if (mem_r8(s, DST + (uint32_t)i) != id_block[i]) bad = i;
    if (bad < 0) snprintf(got, cap, "512 bytes match");
    else snprintf(got, cap, "byte %d differs", bad);
    failures += check("card program and read back", got, "512 bytes match");

    /* A DMA is not a second way into the card but the same byte stream the
     * immediate bytes that framed it belong to, and the device ID's latency
     * field says how far into that stream the read data starts: four bytes,
     * which is what __CARDReadSegment sends before its EXIDma. So a DMA that
     * arrives without them must see the latency first and the array after,
     * which is the only place the two halves of the model can be seen to
     * agree -- the game always sends the four, so they agree by accident
     * everywhere else. */
    exi_select(s, 0);
    card_addr_cmd(s, 0x52, 0);
    exi_dma(s, DST, 32, 0);
    exi_deselect(s);
    for (bad = -1, i = 0; i < 32 && bad < 0; i++)
        if (mem_r8(s, DST + (uint32_t)i) != (i < 4 ? 0xFF : id_block[i - 4])) bad = i;
    snprintf(got, cap, "%s", bad < 0 ? "four dummy bytes, then the array" : "wrong");
    failures += check("read latency on the DMA path", got, "four dummy bytes, then the array");

    /* The first read of SRAM, and so the first chance the runtime has to
     * recover the flash ID from the image just written. An image formatted
     * anywhere else verifies only if it does. */
    exi_select(s, 1);
    exi_imm(s, 0x20000100u, 4, 1);
    exi_dma(s, SRB, 64, 0);
    exi_deselect(s);
    for (i = 0; i < 64; i++) sram[i] = mem_r8(s, SRB + (uint32_t)i);
    snprintf(got, cap, "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X", sram[20], sram[21], sram[22], sram[23],
             sram[24], sram[25], sram[26], sram[27], sram[28], sram[29], sram[30], sram[31]);
    failures += check("SRAM flash ID from image", got, "112233445566778899AABBCC");

    /* CARDDoMount sums those twelve bytes and compares the complement against
     * SRAM 58; a mismatch is IOERROR and the mount ends there. */
    for (sum = 0, i = 0; i < 12; i++) sum += sram[20 + i];
    snprintf(got, cap, "%02X", sram[58]);
    snprintf(want, sizeof want, "%02X", (unsigned)(uint8_t)~sum);
    failures += check("SRAM flash ID checksum", got, want);

    /* The OSSram checksum covers the four u16 at 12, 14, 16 and 18 and stops
     * short of the extended half; byte 19 is the flags OSGetSoundMode reads,
     * whose bit 2 is stereo. Both pin the layout the mount depends on. */
    for (sum = inv = 0, i = 12; i < 20; i += 2) {
        unsigned w = ((unsigned)sram[i] << 8) | sram[i + 1];
        sum += w;
        inv += (unsigned)(uint16_t)~w;
    }
    snprintf(got, cap, "%04X %04X %02X", ((unsigned)sram[0] << 8) | sram[1], ((unsigned)sram[2] << 8) | sram[3],
             sram[19]);
    snprintf(want, sizeof want, "%04X %04X 04", sum & 0xFFFFu, inv & 0xFFFFu);
    failures += check("SRAM checksum and flags", got, want);

    /* The command's offset field names where the payload starts: every write
     * the game makes to the extended half goes through __OSUnlockSramEx,
     * which commits from offset 20. SRAM 56 is the DVD error code, which
     * nothing here reads back. */
    exi_select(s, 1);
    exi_imm(s, 0xA0000100u + (56u << 6), 4, 1); /* UnlockSram adds, and must: the */
    exi_imm(s, 0x5A000000u, 1, 1);              /* shifted offset overlaps the base */
    exi_deselect(s);
    exi_select(s, 1);
    exi_imm(s, 0x20000100u + (20u << 6), 4, 1);
    id = exi_imm(s, 0, 4, 0);
    exi_deselect(s);
    exi_select(s, 1);
    exi_imm(s, 0x20000100u, 4, 1);
    exi_dma(s, SRB, 64, 0);
    exi_deselect(s);
    for (bad = -1, i = 0; i < 64 && bad < 0; i++)
        if (i != 56 && mem_r8(s, SRB + (uint32_t)i) != sram[i]) bad = i;
    snprintf(got, cap, "%02X %08X %s", mem_r8(s, SRB + 56), id, bad < 0 ? "in place" : "moved");
    failures += check("SRAM write and read at offset", got, "5A 11223344 in place");

    /* The same device by DMA, both ways. __OSInitSram reads the 64 bytes by
     * DMA and UnlockSram commits them by immediate, so the write direction has
     * no user in the game today -- but a DMA the device dropped would still
     * clear TSTART and set TCINT, so EXISync and the transfer-complete handler
     * would both report success and the guest would never learn. That is the
     * path CARDDoMount writes the flash ID back through, and a silent drop
     * there looks exactly like the IOERROR the mount has never got past. The
     * bytes are slot B's flash ID and the wireless IDs, which nothing below
     * reads: SRAM is rebuilt from the image at the reload check. */
    for (i = 0; i < 32; i++) mem_w8(s, SRC + (uint32_t)i, (uint8_t)(0xE0 + i));
    exi_select(s, 1);
    exi_imm(s, 0xA0000100u + (32u << 6), 4, 1);
    exi_dma(s, SRC, 32, 1);
    exi_deselect(s);
    exi_select(s, 1);
    exi_imm(s, 0x20000100u + (32u << 6), 4, 1);
    exi_dma(s, DST, 32, 0);
    exi_deselect(s);
    for (bad = -1, i = 0; i < 32 && bad < 0; i++)
        if (mem_r8(s, DST + (uint32_t)i) != (uint8_t)(0xE0 + i)) bad = i;
    snprintf(got, cap, "%s", bad < 0 ? "32 bytes in and back" : "wrong");
    failures += check("SRAM DMA both ways", got, "32 bytes in and back");

    /* The card's own interrupt. __CARDStart arms a 100 ms alarm, sends the
     * command and waits for the card to say it has finished; nothing raising
     * it means every write times out, which is why no log has a byte written. */
    exi_select(s, 0);
    exi_imm(s, 0x81010000u, 2, 1); /* __CARDEnableInterrupt(chan, TRUE) */
    exi_deselect(s);

    for (i = 0; i < 128; i++) mem_w8(s, SRC + (uint32_t)i, (uint8_t)(0xA0 + i));
    card_program(s, page, SRC);
    snprintf(got, cap, "csr %u pending %u", (exi_csr(s) & EXIINT) ? 1u : 0u, exi_exi_irq_pending() & 1u);
    failures += check("program raises the interrupt", got, "csr 1 pending 1");

    /* EXILock clears the enable for as long as the handler holds the channel:
     * a request already latched has to wait for the unlock, not vanish. */
    device_write(s, EXI0_CSR, 4, 0);
    snprintf(got, cap, "csr %u pending %u", (exi_csr(s) & EXIINT) ? 1u : 0u, exi_exi_irq_pending() & 1u);
    failures += check("masked interrupt waits", got, "csr 1 pending 0");

    /* Everything so far reads exi_exi_irq_pending(), which is the predicate
     * under test: it says the device model thinks an interrupt is due, not
     * that one reached the guest. irq.c is the other half of B2 and nothing
     * exercised it -- its whole EXI block could be deleted and every check
     * above would still pass. So install the handler EXIInit installs, in the
     * slots it installs it in, and run the idle-loop hook that is the port's
     * one delivery point. EXIIntrruptHandler is a real handler and clears the
     * CSR bit itself, so this also pins the write-one-to-clear arithmetic
     * against the guest's own code rather than against a second copy of it. */
    s->gpr[1] = STACK_TOP - 0x400; /* the handler has a frame; give it a stack */
    s->gpr[2] = SDA2_BASE;
    s->gpr[13] = SDA_BASE;
    for (i = 0; i < 32; i++) mem_w32(s, irq_handler_slot(s, (unsigned)i), 0);
    mem_w32(s, irq_handler_slot(s, 9), EXI_HANDLER);  /* channel 0 */
    mem_w32(s, irq_handler_slot(s, 12), EXI_HANDLER); /* channel 1 */
    mem_w32(s, irq_handler_slot(s, 15), EXI_HANDLER); /* channel 2 */
    before = irq_delivered_count(9);
    hook_80237BA8(s);
    snprintf(got, cap, "%llu", (unsigned long long)(irq_delivered_count(9) - before));
    failures += check("masked, nothing delivered", got, "0");

    device_write(s, EXI0_CSR, 4, EXIINTMSK);
    hook_80237BA8(s);
    snprintf(got, cap, "9:%llu 12:%llu 15:%llu csr %u",
             (unsigned long long)(irq_delivered_count(9) - before),
             (unsigned long long)irq_delivered_count(12), (unsigned long long)irq_delivered_count(15),
             (exi_csr(s) & EXIINT) ? 1u : 0u);
    failures += check("delivered to interrupt 9", got, "9:1 12:0 15:0 csr 0");

    /* The card is still holding its line -- only clear-status lets go -- so a
     * second pass must not find a second interrupt. A runtime that re-derived
     * the CSR bit from the line instead of latching it would loop here. */
    before = irq_delivered_count(9);
    hook_80237BA8(s);
    snprintf(got, cap, "%llu", (unsigned long long)(irq_delivered_count(9) - before));
    failures += check("cleared, nothing delivered", got, "0");

    sum = card_handle_interrupt(s, &midway);
    snprintf(got, cap, "%02X midway %u", sum, midway);
    failures += check("handler reads status", got, "41 midway 0");
    snprintf(got, cap, "csr %u pending %u", (exi_csr(s) & EXIINT) ? 1u : 0u, exi_exi_irq_pending() & 1u);
    failures += check("handler leaves it quiet", got, "csr 0 pending 0");

    card_read(s, sector, DST, 512);
    for (bad = -1, i = 0; i < 512 && bad < 0; i++) {
        uint8_t b = mem_r8(s, DST + (uint32_t)i);
        if (b != (i >= 0x100 && i < 0x180 ? (uint8_t)(0xA0 + i - 0x100) : 0xFF)) bad = i;
    }
    snprintf(got, cap, "%s", bad < 0 ? "one page in an erased sector" : "wrong");
    failures += check("programmed page reads back", got, "one page in an erased sector");

    /* Sector erase clears its own 8 KB and nothing else: the address names a
     * byte inside the sector, not the sector's start. */
    card_erase(s, page);
    snprintf(got, cap, "pending %u", exi_exi_irq_pending() & 1u);
    failures += check("erase raises the interrupt", got, "pending 1");
    card_handle_interrupt(s, &midway);
    snprintf(got, cap, "midway %u", midway);
    failures += check("erase handler sees a clear bit", got, "midway 0");
    card_read(s, sector, DST, 512);
    for (bad = -1, i = 0; i < 512 && bad < 0; i++)
        if (mem_r8(s, DST + (uint32_t)i) != 0xFF) bad = i;
    card_read(s, 0, DST + 0x200, 512);
    for (i = 0; i < 512 && bad < 0; i++)
        if (mem_r8(s, DST + 0x200 + (uint32_t)i) != id_block[i]) bad = 0x1000 + i;
    snprintf(got, cap, "%s", bad < 0 ? "sector blank, ID block intact" : "wrong");
    failures += check("sector erase", got, "sector blank, ID block intact");

    /* A save is not one program but dozens: __CARDWrite walks a sector as 64
     * page programs, each one waiting on the interrupt the last one raised.
     * The card raises only while its previous request is unacknowledged, so
     * a round of the handler has to fall between them -- it does, because
     * __CARDClearStatus always precedes the next __CARDStart, but that is an
     * argument and this is the check. Four in a row, each with its own
     * interrupt and its own 128 bytes. */
    bad = -1;
    for (i = 0; i < 4; i++) {
        int j;
        for (j = 0; j < 128; j++) mem_w8(s, SRC + (uint32_t)j, (uint8_t)(0x10 * (i + 1) + j));
        card_program(s, sector + 0x800 + (uint32_t)i * 128, SRC);
        if (bad < 0 && !(exi_exi_irq_pending() & 1)) bad = i;
        card_handle_interrupt(s, &midway);
        if (bad < 0 && midway) bad = 0x10 + i;
    }
    card_read(s, sector + 0x800, DST, 512);
    for (i = 0; i < 512 && bad < 0; i++)
        if (mem_r8(s, DST + (uint32_t)i) != (uint8_t)(0x10 * (i / 128 + 1) + i % 128)) bad = 0x100 + i;
    snprintf(got, cap, "%s", bad < 0 ? "four pages, four interrupts" : "wrong");
    failures += check("consecutive page programs", got, "four pages, four interrupts");

    /* With the interrupt turned off the card must not drive the line at all:
     * the mount only turns it on once, at its last step. */
    exi_select(s, 0);
    exi_imm(s, 0x81000000u, 2, 1); /* __CARDEnableInterrupt(chan, FALSE) */
    exi_deselect(s);
    card_program(s, sector + 0x400, SRC);
    snprintf(got, cap, "csr %u pending %u", (exi_csr(s) & EXIINT) ? 1u : 0u, exi_exi_irq_pending() & 1u);
    failures += check("disabled card stays quiet", got, "csr 0 pending 0");

    /* And the bytes are on disk already, not waiting for the run to end: a
     * save is dozens of page programs and a kill in the middle keeps what
     * landed. This also makes the directory, which nothing else creates. */
    {
        FILE* f = fopen(CARD_IMAGE, "rb");
        long size = 0;
        uint8_t head[2] = {0, 0};
        if (f) {
            fseek(f, 0, SEEK_END);
            size = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (fread(head, 1, sizeof head, f) != sizeof head) size = -1;
            fclose(f);
        }
        snprintf(got, cap, "%ld bytes, %02X%02X", size, head[0], head[1]);
        snprintf(want, sizeof want, "524288 bytes, %02X%02X", id_block[0], id_block[1]);
        failures += check("card image flushed to disk", got, want);
    }

    /* Everything above ran against the buffer this run filled. An image the
     * game did not just write -- a Dolphin-formatted blank, which is how B4
     * means to get its first mount -- arrives through card_load's read path,
     * and nothing had ever taken it. Forget the card and read it back the way
     * the next boot would, SRAM included: the flash ID has to survive a
     * restart or the arrangement is no use. */
    exi_card_reset();
    card_read(s, 0, DST, 512);
    for (bad = -1, i = 0; i < 512 && bad < 0; i++)
        if (mem_r8(s, DST + (uint32_t)i) != id_block[i]) bad = i;
    exi_select(s, 1);
    exi_imm(s, 0x20000100u, 4, 1);
    exi_dma(s, SRB, 64, 0);
    exi_deselect(s);
    for (sum = 0, i = 0; i < 12; i++) sum += mem_r8(s, SRB + 20 + (uint32_t)i);
    snprintf(got, cap, "%s %02X%02X..%02X%02X %02X", bad < 0 ? "ID block" : "wrong",
             mem_r8(s, SRB + 20), mem_r8(s, SRB + 21), mem_r8(s, SRB + 30), mem_r8(s, SRB + 31),
             mem_r8(s, SRB + 58));
    snprintf(want, sizeof want, "ID block 1122..BBCC %02X", (unsigned)(uint8_t)~sum);
    failures += check("reloaded from disk", got, want);

    /* What the [exi] line at the end of a run prints. B2's own acceptance test
     * is a non-zero read count there, and until this nothing checked the
     * counters at all: a model that answered reads out of its buffer without
     * counting them would leave every log saying 0 bytes read. The numbers are
     * the bytes the checks above moved, so adding a check moves them. */
    exi_card_counters(&reads, &writes, &ints);
    snprintf(got, cap, "%llu read, %llu written, %llu interrupts", (unsigned long long)reads,
             (unsigned long long)writes, (unsigned long long)ints);
    snprintf(want, sizeof want, "%u read, %u written, %u interrupts", CARD_READS, CARD_WRITES, CARD_INTS);
    failures += check("card counters", got, want);
    return failures;
}

/* ---- the AX mixer, driven directly (PLAN E2) ----------------------------
 *
 * runtime/ax.c decodes, rate-converts, shapes and mixes 160 samples a frame
 * for every voice the game's audio driver hands it, and until this nothing
 * had ever checked one of those samples. These build the parameter blocks
 * and the command list by hand, put known bytes in ARAM, call
 * ax_command_list() outside the game, and read the result back out of guest
 * memory. No disc, no driver, no clock.
 *
 * Every expected value here is arithmetic, never a recording of what the
 * mixer produces today: the DSP-ADPCM predictor's own formula, the Q15
 * shift, and the address units each sample format counts in. In particular
 * nothing may depend on the *shape* of the rate converter, because nothing
 * establishes which shape is right -- the census says the driver asks for
 * src_type 0 in all 351,290 voice-frames, Dolphin's AX enum calls that
 * polyphase, and ax.c interpolates two taps linearly. So every value check
 * drives a constant, which any interpolator with unity DC gain passes
 * through unchanged, or reads the decoder's state back out of the block,
 * and rate conversion is checked by how many input samples it consumes,
 * which is the address accumulator's arithmetic and true of any resampler.
 *
 * Where a check needs a source it can predict sample by sample it uses
 * PCM16, which the game itself never asks for -- the census reports format 0
 * in every one of those voice-frames. That is deliberate: PCM16 is the way
 * to put a known constant on the bus with no decoder in between, and the
 * format the game does use has its own cases above. The same goes for the
 * auxiliary-B and surround sends, which this game never sets a bit for.
 *
 * The block below reaches nothing in the port but ax_command_list(),
 * aram_memory() and guest memory, so it can be lifted into a driver of its
 * own the way tools/citest/render_driver.c lifts the render checks.
 */
void ax_command_list(CpuState* s, uint32_t addr);

/* AXPB word offsets -- the layout runtime/ax.c enumerates, named again here
 * because that enum is private to it. */
enum {
    AXPB_NEXT = 0,        /* 2 words: the chain pointer */
    AXPB_SRC_TYPE = 4,
    AXPB_MIXER_CTRL = 6,
    AXPB_RUNNING = 7,
    AXPB_MIXER = 9,       /* 18 words, see AXMX_* */
    AXPB_UPDATES = 34,    /* per-millisecond counts[5], then data hi, lo */
    AXPB_VOL_ENV = 50,    /* current volume, delta */
    AXPB_AUDIO = 55,      /* looping, format, loop hi/lo, end hi/lo, cur hi/lo */
    AXPB_ADPCM = 63,      /* coefs[16], gain, pred_scale, yn1, yn2 */
    AXPB_SRC = 83,        /* ratio hi/lo, address fraction, last_samples[4] */
    AXPB_ADPCM_LOOP = 90, /* pred_scale, yn1, yn2 */
    AXPB_WORDS = 96
};
enum { AXMX_L = 0, AXMX_DL, AXMX_R, AXMX_DR, AXMX_AL, AXMX_DAL, AXMX_AR, AXMX_DAR,
       AXMX_BL, AXMX_DBL, AXMX_BR, AXMX_DBR, AXMX_S, AXMX_DS, AXMX_AS, AXMX_DAS, AXMX_BS };

/* Guest scratch, clear of everything else the selftest uses. */
#define AX_PB   (SCRATCH + 0x20000u) /* up to 300 blocks, 0xC0 apart */
#define AX_CMD  (SCRATCH + 0x50000u)
#define AX_OUT  (SCRATCH + 0x50400u) /* 160 stereo 16-bit samples */
#define AX_SUR  (SCRATCH + 0x50800u) /* 160 surround samples */
#define AX_BUF  (SCRATCH + 0x51000u) /* a CPU-side bus: 3 x 160 32-bit samples */
#define AX_UPD  (SCRATCH + 0x52000u) /* one update-list entry */

/* ARAM, in bytes. The addresses a block carries are in its format's own
 * units: nibbles for DSP-ADPCM, samples for PCM16. */
#define AX_AR_ADPCM 0x24000u /* one frame, then a loud one after it */
#define AX_AR_C4000 0x28000u /* 4096 PCM16 samples of 0x4000 */
#define AX_AR_C5000 0x2A000u /* 1024 of +0x5000 */
#define AX_AR_M5000 0x2C000u /* 1024 of -0x5000 */
#define AX_AR_LOOP  0x2E000u /* 100 of 0x4000, then 0x7000 past the end */
#define AX_NIB(b) ((b) * 2u)
#define AX_SMP(b) ((b) / 2u)

static void ax_w(CpuState* s, uint32_t pb, unsigned word, uint16_t v) { mem_w16(s, pb + word * 2u, v); }
static uint16_t ax_r(CpuState* s, uint32_t pb, unsigned word) { return mem_r16(s, pb + word * 2u); }
static void ax_w32(CpuState* s, uint32_t pb, unsigned word, uint32_t v)
{
    ax_w(s, pb, word, (uint16_t)(v >> 16));
    ax_w(s, pb, word + 1, (uint16_t)v);
}
static uint32_t ax_r32(CpuState* s, uint32_t pb, unsigned word)
{
    return ((uint32_t)ax_r(s, pb, word) << 16) | ax_r(s, pb, word + 1);
}
static void ax_pb_clear(CpuState* s, uint32_t pb)
{
    unsigned i;
    for (i = 0; i < AXPB_WORDS; i++) ax_w(s, pb, i, 0);
}

static void ax_aram_pcm16(uint32_t byte_addr, uint32_t samples, int16_t value)
{
    uint8_t* ar = aram_memory();
    uint32_t i;
    for (i = 0; i < samples; i++) {
        ar[byte_addr + i * 2u] = (uint8_t)((uint16_t)value >> 8);
        ar[byte_addr + i * 2u + 1u] = (uint8_t)value;
    }
}

/* One DSP-ADPCM frame: the header byte the format calls pred_scale (low
 * nibble the scale exponent, next three the coefficient pair), then seven
 * bytes holding fourteen sample nibbles. */
static void ax_aram_adpcm(uint32_t byte_addr, uint8_t header, const uint8_t* seven)
{
    uint8_t* ar = aram_memory();
    int i;
    ar[byte_addr] = header;
    for (i = 0; i < 7; i++) ar[byte_addr + 1u + (uint32_t)i] = seven[i];
}

/* A voice reading PCM16 at unity rate through a unit envelope. The four
 * history samples are preloaded with the constant the samples hold, so the
 * output is that constant from the first sample on, whatever kernel the
 * rate converter turns out to want. */
static void ax_pcm16_voice(CpuState* s, uint32_t pb, uint32_t byte_addr, uint32_t samples, int16_t value)
{
    unsigned i;
    ax_pb_clear(s, pb);
    ax_w(s, pb, AXPB_RUNNING, 1);
    ax_w(s, pb, AXPB_AUDIO + 1, 0x0A); /* PCM16 */
    ax_w32(s, pb, AXPB_AUDIO + 4, AX_SMP(byte_addr) + samples);
    ax_w32(s, pb, AXPB_AUDIO + 6, AX_SMP(byte_addr));
    ax_w32(s, pb, AXPB_SRC, 0x10000u); /* one input sample per output sample */
    ax_w(s, pb, AXPB_VOL_ENV, 0x8000); /* Q15 unity: (x * 0x8000) >> 15 == x */
    for (i = 0; i < 4; i++) ax_w(s, pb, AXPB_SRC + 3 + i, (uint16_t)value);
}

static void ax_adpcm_voice(CpuState* s, uint32_t pb, uint32_t nib_start, uint32_t nib_end, int16_t c0, int16_t c1)
{
    ax_pb_clear(s, pb);
    ax_w(s, pb, AXPB_RUNNING, 1);
    ax_w(s, pb, AXPB_AUDIO + 1, 0x00); /* DSP-ADPCM */
    ax_w32(s, pb, AXPB_AUDIO + 4, nib_end);
    ax_w32(s, pb, AXPB_AUDIO + 6, nib_start);
    ax_w(s, pb, AXPB_ADPCM, (uint16_t)c0); /* coefficient pair 0 */
    ax_w(s, pb, AXPB_ADPCM + 1, (uint16_t)c1);
    ax_w32(s, pb, AXPB_SRC, 0x10000u);
    ax_w(s, pb, AXPB_VOL_ENV, 0x8000);
    ax_w(s, pb, AXPB_MIXER + AXMX_L, 0x8000);
}

static uint32_t ax_put16(CpuState* s, uint32_t p, uint16_t v) { mem_w16(s, p, v); return p + 2u; }
static uint32_t ax_put32(CpuState* s, uint32_t p, uint32_t v)
{
    mem_w16(s, p, (uint16_t)(v >> 16));
    mem_w16(s, p + 2u, (uint16_t)v);
    return p + 4u;
}

/* The command list the driver sends every 5 ms, cut down to what a check
 * needs: point at the voice chain and run it, optionally hand a bus to the
 * CPU and take one back, write the frame's output, end. */
static void ax_fill_out(CpuState* s, uint8_t byte)
{
    int i;
    for (i = 0; i < 640; i++) mem_w8(s, AX_OUT + (uint32_t)i, byte);
}

static void ax_run_aux(CpuState* s, uint32_t pb, uint16_t aux, uint32_t up, uint32_t down)
{
    uint32_t p = AX_CMD;
    ax_fill_out(s, 0xAA); /* a list that never reaches OUTPUT must not pass on the last frame's */
    if (pb) {
        p = ax_put16(s, p, 0x02); /* PB_ADDR */
        p = ax_put32(s, p, pb);
        p = ax_put16(s, p, 0x03); /* PROCESS_PB */
    }
    if (aux == 0x09) { /* MIX_AUXB_NOWRITE takes the download address alone */
        p = ax_put16(s, p, aux);
        p = ax_put32(s, p, down);
    } else if (aux) {
        p = ax_put16(s, p, aux);
        p = ax_put32(s, p, up);
        p = ax_put32(s, p, down);
    }
    p = ax_put16(s, p, 0x0E); /* OUTPUT: the surround address, then L/R */
    p = ax_put32(s, p, AX_SUR);
    p = ax_put32(s, p, AX_OUT);
    ax_put16(s, p, 0x0F); /* END */
    ax_command_list(s, AX_CMD);
}

static void ax_run(CpuState* s, uint32_t pb) { ax_run_aux(s, pb, 0, 0, 0); }

/* The output as the AI DMA buffer holds it: 160 stereo pairs, the right
 * sample first, each a big-endian 16-bit word. Read a byte at a time, so
 * that neither a swapped byte order nor a swapped channel can cancel out
 * against a matching mistake in the check itself. */
static int ax_out_r(CpuState* s, int i)
{
    uint32_t a = AX_OUT + (uint32_t)i * 4u;
    return (int16_t)(((uint16_t)mem_r8(s, a) << 8) | mem_r8(s, a + 1u));
}
static int ax_out_l(CpuState* s, int i)
{
    uint32_t a = AX_OUT + (uint32_t)i * 4u + 2u;
    return (int16_t)(((uint16_t)mem_r8(s, a) << 8) | mem_r8(s, a + 1u));
}
static int ax_out_s(CpuState* s, int i) { return (int16_t)mem_r16(s, AX_SUR + (uint32_t)i * 2u); }

/* The first sample in [from, to) that is not `want`, or -1. */
static int ax_bad_l(CpuState* s, int from, int to, int want)
{
    int i;
    for (i = from; i < to; i++)
        if (ax_out_l(s, i) != want) return i;
    return -1;
}
static int ax_bad_r(CpuState* s, int from, int to, int want)
{
    int i;
    for (i = from; i < to; i++)
        if (ax_out_r(s, i) != want) return i;
    return -1;
}

/* The buses exchanged with the CPU are 32-bit samples, three channels of
 * 160, channel-major (FINDINGS section 10). */
static int32_t ax_bus(CpuState* s, int c, int i)
{
    return (int32_t)mem_r32(s, AX_BUF + (uint32_t)(c * 160 + i) * 4u);
}
static int ax_bad_bus(CpuState* s, int c, int32_t want)
{
    int i;
    for (i = 0; i < 160; i++)
        if (ax_bus(s, c, i) != want) return i;
    return -1;
}
/* Pre-fill the buffer the CPU shares with the mixer: `ramp` makes it
 * 0, 1, 2, ... for the download checks; otherwise every word gets a marker
 * the mixer has to overwrite, so a channel it never writes at all is caught
 * as surely as one it writes wrongly. */
static void ax_fill_bus(CpuState* s, int ramp)
{
    int i;
    for (i = 0; i < 480; i++) mem_w32(s, AX_BUF + (uint32_t)i * 4u, ramp ? (uint32_t)i : 0x0BADF00Du);
}

/* yn2 and yn1 -- the decoder's own history, where the block keeps it. */
static void ax_history(CpuState* s, uint32_t pb, char* out, size_t cap)
{
    snprintf(out, cap, "%d %d", (int16_t)ax_r(s, pb, AXPB_ADPCM + 19), (int16_t)ax_r(s, pb, AXPB_ADPCM + 18));
}

static int ax_selftest(CpuState* s, char* got, size_t cap)
{
    /* nibbles 1,7,-1,-8, eight zeros, then 3,-5. The first four are the
     * vectors tools/tests/test_dspadpcm.py hand-computed for the Python
     * decoder, so the mixer and the analysis tool now agree with a third,
     * derived source rather than with each other. */
    static const uint8_t frame_a[7] = {0x17, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x3B};
    static const uint8_t frame_pred[7] = {0x11, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00};  /* 1,1,1,1,0.. */
    static const uint8_t frame_clip[7] = {0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};  /* 7,-8,0.. */
    static const uint8_t frame_loud[7] = {0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77};
    static const uint8_t frame_zero[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t frame_three[7] = {0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    const uint32_t nib = AX_NIB(AX_AR_ADPCM);
    char want[128];
    int failures = 0, bad, i;

    /* ---- the DSP-ADPCM decoder ------------------------------------------
     * s[n] = clamp16((nibble * 2^scale * 2048 + c0*s[n-1] + c1*s[n-2] + 1024) >> 11),
     * the nibble sign-extended from four bits and the coefficients in 4.11
     * fixed point. The rate converter must not stand between the check and
     * that formula, so each case reads the predictor's history back out of
     * the block instead of listening to the bus: a one-shot voice whose end
     * address is start + 2 + N nibbles decodes exactly N samples and stops,
     * leaving yn2 = s[N-2] and yn1 = s[N-1]. */
    ax_aram_adpcm(AX_AR_ADPCM, 0x04, frame_a);         /* scale 4, coefficient pair 0 */
    ax_aram_adpcm(AX_AR_ADPCM + 8u, 0x0C, frame_loud); /* the next frame along, loud */

    /* 1 and 7 at scale 4 are 16 and 112, from (n * 2^4 * 2048 + 1024) >> 11 */
    ax_adpcm_voice(s, AX_PB, nib, nib + 4u, 0, 0);
    ax_run(s, AX_PB);
    ax_history(s, AX_PB, got, cap);
    failures += check("ax adpcm scale", got, "16 112");

    /* -1 and -8 give -16 and -128, not -15 and -127: that >> 11 is an
     * arithmetic shift, so it floors rather than truncating toward zero. */
    ax_adpcm_voice(s, AX_PB, nib, nib + 6u, 0, 0);
    ax_run(s, AX_PB);
    ax_history(s, AX_PB, got, cap);
    failures += check("ax adpcm shift floors", got, "-16 -128");

    /* c0 = 2048 is 1.0, so at scale 0 the decoder integrates: 1, 2, 3, 4. */
    ax_aram_adpcm(AX_AR_ADPCM, 0x00, frame_pred);
    ax_adpcm_voice(s, AX_PB, nib, nib + 6u, 2048, 0);
    ax_run(s, AX_PB);
    ax_history(s, AX_PB, got, cap);
    failures += check("ax adpcm predictor", got, "3 4");

    /* The +1024 rounds to nearest, and the header's upper nibbles pick which
     * of the eight coefficient pairs to predict with. Here pair 1 holds 0.5
     * and pair 0 is zero, and the nibbles are the four above, so
     *   1, then 7 + 0.5*1, then -1 + 0.5*8, then -8 + 0.5*3
     * is 1, 8, 3, -6 -- a sequence a decoder that ignores the pair, or that
     * truncates instead of rounding, cannot produce. */
    ax_aram_adpcm(AX_AR_ADPCM, 0x10, frame_a); /* scale 0, coefficient pair 1 */
    ax_adpcm_voice(s, AX_PB, nib, nib + 6u, 0, 0);
    ax_w(s, AX_PB, AXPB_ADPCM + 2, 1024); /* pair 1: 0.5 in 4.11 fixed point */
    ax_run(s, AX_PB);
    ax_history(s, AX_PB, got, cap);
    failures += check("ax adpcm coefficient pair", got, "3 -6");

    /* scale 15: 7 * 32768 * 2048 >> 11 is 229376 and -8 * 32768 * 2048 >> 11
     * is -262144, so one frame exercises both ends of the 16-bit clamp. */
    ax_aram_adpcm(AX_AR_ADPCM, 0x0F, frame_clip);
    ax_adpcm_voice(s, AX_PB, nib, nib + 4u, 0, 0);
    ax_run(s, AX_PB);
    ax_history(s, AX_PB, got, cap);
    failures += check("ax adpcm clamps", got, "32767 -32768");

    /* ---- a one-shot voice ends on its own last sample -------------------
     * A frame holds fourteen samples, so a block whose end address is
     * start + 16 nibbles decodes s[0..13] and stops: the sample *at* the end
     * address is not played. The frame after this one decodes to 28672, and
     * the history would hold that instead of s[13] = -80 if the end test let
     * one more sample through. Nothing is mixed once the voice stops and the
     * bus starts every frame silent, so the rest of the frame is exactly
     * zero however the converter is shaped. */
    ax_aram_adpcm(AX_AR_ADPCM, 0x04, frame_a);
    ax_adpcm_voice(s, AX_PB, nib, nib + 16u, 0, 0);
    ax_run(s, AX_PB);
    snprintf(got, cap, "running %u cur %X yn %d %d %s", ax_r(s, AX_PB, AXPB_RUNNING),
             ax_r32(s, AX_PB, AXPB_AUDIO + 6), (int16_t)ax_r(s, AX_PB, AXPB_ADPCM + 19),
             (int16_t)ax_r(s, AX_PB, AXPB_ADPCM + 18),
             ax_bad_l(s, 32, 160, 0) < 0 ? "then silent" : "then noise");
    snprintf(want, sizeof want, "running 0 cur %X yn 48 -80 then silent", nib + 16u);
    failures += check("ax one-shot stops at end", got, want);

    /* ---- the loop, and the decoder state it restores --------------------
     * A block carries a second copy of pred_scale, yn1 and yn2 for its loop
     * point, because a loop address that is not frame-aligned lands where
     * there is no header byte to re-read -- and the disc's own streams are
     * like that: m01_L.dsp and 141_L.dsp both loop at nibble 2. These make
     * the loop one nibble long at nibble 2 of the frame, so every fetch
     * after the first wraps and restores, and the whole rest of the frame is
     * one constant that any rate converter passes through.
     *
     * The frame's own header selects coefficient pair 1, whose coefficients
     * are zero; the restored pred_scale selects pair 0. So a mixer that
     * keeps the header's pred_scale, or that fails to restore either history
     * word, emits 0 here instead of the constant. */
    ax_aram_adpcm(AX_AR_ADPCM, 0x13, frame_zero); /* scale 3, coefficient pair 1 */
    ax_adpcm_voice(s, AX_PB, nib, nib + 3u, 2048, 0);
    ax_w(s, AX_PB, AXPB_AUDIO, 1); /* looping */
    ax_w32(s, AX_PB, AXPB_AUDIO + 2, nib + 2u);
    ax_w(s, AX_PB, AXPB_ADPCM_LOOP, 0x00); /* scale 0, coefficient pair 0 */
    ax_w(s, AX_PB, AXPB_ADPCM_LOOP + 1, 1000);
    ax_run(s, AX_PB);
    bad = ax_bad_l(s, 8, 160, 1000); /* from 8: clear of the wrap by any short kernel */
    if (bad < 0) snprintf(got, cap, "1000 constant");
    else snprintf(got, cap, "%d at %d", ax_out_l(s, bad), bad);
    failures += check("ax loop restores yn1", got, "1000 constant");

    ax_adpcm_voice(s, AX_PB, nib, nib + 3u, 0, 2048);
    ax_w(s, AX_PB, AXPB_AUDIO, 1);
    ax_w32(s, AX_PB, AXPB_AUDIO + 2, nib + 2u);
    ax_w(s, AX_PB, AXPB_ADPCM_LOOP, 0x00);
    ax_w(s, AX_PB, AXPB_ADPCM_LOOP + 2, 2000);
    ax_run(s, AX_PB);
    bad = ax_bad_l(s, 8, 160, 2000);
    if (bad < 0) snprintf(got, cap, "2000 constant");
    else snprintf(got, cap, "%d at %d", ax_out_l(s, bad), bad);
    failures += check("ax loop restores yn2", got, "2000 constant");

    /* Same shape with the coefficients zero and the nibble 3, so the sample
     * is 3 * 2^scale: the header's scale 3 would give 24, the loop's 5
     * gives 96. */
    ax_aram_adpcm(AX_AR_ADPCM, 0x13, frame_three);
    ax_adpcm_voice(s, AX_PB, nib, nib + 3u, 0, 0);
    ax_w(s, AX_PB, AXPB_AUDIO, 1);
    ax_w32(s, AX_PB, AXPB_AUDIO + 2, nib + 2u);
    ax_w(s, AX_PB, AXPB_ADPCM_LOOP, 0x05); /* scale 5, coefficient pair 0 */
    ax_run(s, AX_PB);
    bad = ax_bad_l(s, 8, 160, 96);
    if (bad < 0) snprintf(got, cap, "96 constant, ps %04X", ax_r(s, AX_PB, AXPB_ADPCM + 17));
    else snprintf(got, cap, "%d at %d", ax_out_l(s, bad), bad);
    failures += check("ax loop restores scale", got, "96 constant, ps 0005");

    /* ---- PCM16, the pan, and the order the samples are written ---------- */
    ax_aram_pcm16(AX_AR_C4000, 4096, 0x4000);
    ax_pcm16_voice(s, AX_PB, AX_AR_C4000, 4096, 0x4000);
    ax_w(s, AX_PB, AXPB_MIXER + AXMX_L, 0x8000);
    ax_w(s, AX_PB, AXPB_MIXER + AXMX_R, 0);
    ax_run(s, AX_PB);
    bad = ax_bad_l(s, 0, 160, 16384);
    if (bad < 0) bad = ax_bad_r(s, 0, 160, 0);
    snprintf(got, cap, "%02X %02X %02X %02X %s", mem_r8(s, AX_OUT), mem_r8(s, AX_OUT + 1u),
             mem_r8(s, AX_OUT + 2u), mem_r8(s, AX_OUT + 3u), bad < 0 ? "hard left" : "wrong");
    failures += check("ax pan and word order", got, "00 00 40 00 hard left");

    /* Q15 all the way through: the envelope and the mixer gain each multiply
     * and shift right 15, so 0x8000 is transparent and 0x4000 halves. A
     * shift of 16 anywhere would give 4096, 4096, 1024 instead. */
    {
        int v[3];
        static const uint16_t ve[3] = {0x4000, 0x8000, 0x4000};
        static const uint16_t lg[3] = {0x8000, 0x4000, 0x4000};
        for (i = 0; i < 3; i++) {
            ax_pcm16_voice(s, AX_PB, AX_AR_C4000, 4096, 0x4000);
            ax_w(s, AX_PB, AXPB_VOL_ENV, ve[i]);
            ax_w(s, AX_PB, AXPB_MIXER + AXMX_L, lg[i]);
            ax_run(s, AX_PB);
            v[i] = ax_out_l(s, 0);
        }
        snprintf(got, cap, "%d %d %d", v[0], v[1], v[2]);
        failures += check("ax Q15 envelope and gain", got, "8192 8192 4096");
    }

    /* ---- two voices sum on a 32-bit bus, and the output clamps ----------
     * 20480 + 20480 is 40960, which a 16-bit accumulator would have wrapped
     * to -24576 long before the output clamp ever saw it. */
    ax_aram_pcm16(AX_AR_C5000, 1024, 0x5000);
    ax_aram_pcm16(AX_AR_M5000, 1024, -0x5000);
    {
        int peak[2];
        static const uint32_t src[2] = {AX_AR_C5000, AX_AR_M5000};
        static const int16_t val[2] = {0x5000, -0x5000};
        for (i = 0; i < 2; i++) {
            int expect = val[i] > 0 ? 32767 : -32768;
            ax_pcm16_voice(s, AX_PB, src[i], 1024, val[i]);
            ax_pcm16_voice(s, AX_PB + 0xC0u, src[i], 1024, val[i]);
            ax_w(s, AX_PB, AXPB_MIXER + AXMX_L, 0x8000);
            ax_w(s, AX_PB, AXPB_MIXER + AXMX_R, 0x8000);
            ax_w(s, AX_PB + 0xC0u, AXPB_MIXER + AXMX_L, 0x8000);
            ax_w(s, AX_PB + 0xC0u, AXPB_MIXER + AXMX_R, 0x8000);
            ax_w32(s, AX_PB, AXPB_NEXT, AX_PB + 0xC0u);
            ax_run(s, AX_PB);
            bad = ax_bad_l(s, 0, 160, expect);
            if (bad < 0) bad = ax_bad_r(s, 0, 160, expect);
            peak[i] = bad < 0 ? expect : ax_out_l(s, bad);
        }
        snprintf(got, cap, "%d %d", peak[0], peak[1]);
        failures += check("ax bus width and clamp", got, "32767 -32768");
    }

    /* ---- the gain ramp --------------------------------------------------
     * mixer_ctrl bit 3 turns the per-sample ramps on. Sample i is mixed at
     * the gain in force *before* its own step, so with the left gain
     * starting at 0 and stepping by 0x100 the left sample is
     * (16384 * 256i) >> 15 = 128i, and 160 steps leave the block's own gain
     * word at 0xA000. */
    ax_pcm16_voice(s, AX_PB, AX_AR_C4000, 4096, 0x4000);
    ax_w(s, AX_PB, AXPB_MIXER_CTRL, 8);
    ax_w(s, AX_PB, AXPB_MIXER + AXMX_L, 0);
    ax_w(s, AX_PB, AXPB_MIXER + AXMX_DL, 0x0100);
    ax_run(s, AX_PB);
    for (bad = -1, i = 0; i < 160; i++)
        if (ax_out_l(s, i) != 128 * i) { bad = i; break; }
    if (bad < 0) snprintf(got, cap, "0..20352 by 128, gain %04X", ax_r(s, AX_PB, AXPB_MIXER + AXMX_L));
    else snprintf(got, cap, "%d at %d", ax_out_l(s, bad), bad);
    failures += check("ax gain ramp", got, "0..20352 by 128, gain A000");

    /* ---- rate conversion, counted rather than heard ---------------------
     * The address accumulator is 16.16, and 160 output samples advance it by
     * 160 * ratio whatever kernel reads the samples out. 160 * 0x18000 is
     * 240 whole samples exactly; 160 * 0x5555 is 3,495,200, which is 53
     * samples and 21,792/65536 left over. */
    {
        uint32_t adv[2], fr[2];
        static const uint32_t ratio[2] = {0x18000u, 0x5555u};
        for (i = 0; i < 2; i++) {
            ax_pcm16_voice(s, AX_PB, AX_AR_C4000, 4096, 0x4000);
            ax_w32(s, AX_PB, AXPB_SRC, ratio[i]);
            ax_run(s, AX_PB);
            adv[i] = ax_r32(s, AX_PB, AXPB_AUDIO + 6) - AX_SMP(AX_AR_C4000);
            fr[i] = ax_r(s, AX_PB, AXPB_SRC + 2);
        }
        snprintf(got, cap, "%u %04X %u %04X", adv[0], fr[0], adv[1], fr[1]);
        failures += check("ax rate consumption", got, "240 0000 53 5520");
    }

    /* ---- the loop, in sample addresses ----------------------------------
     * 100 samples with the loop 20 in: 160 outputs at unity rate read all
     * 100 and then 60 more from the loop point, leaving the address 80 in.
     * The samples past the end hold a different value, so a mixer that runs
     * past `end` instead of wrapping is heard as well as counted. */
    ax_aram_pcm16(AX_AR_LOOP, 100, 0x4000);
    ax_aram_pcm16(AX_AR_LOOP + 200u, 200, 0x7000);
    ax_pcm16_voice(s, AX_PB, AX_AR_LOOP, 100, 0x4000);
    ax_w(s, AX_PB, AXPB_AUDIO, 1);
    ax_w32(s, AX_PB, AXPB_AUDIO + 2, AX_SMP(AX_AR_LOOP) + 20u);
    ax_w(s, AX_PB, AXPB_MIXER + AXMX_L, 0x8000);
    ax_run(s, AX_PB);
    snprintf(got, cap, "+%u running %u %s", ax_r32(s, AX_PB, AXPB_AUDIO + 6) - AX_SMP(AX_AR_LOOP),
             ax_r(s, AX_PB, AXPB_RUNNING),
             ax_bad_l(s, 0, 160, 16384) < 0 ? "in the loop" : "past the end");
    failures += check("ax pcm16 loop", got, "+80 running 1 in the loop");

    /* A block whose RUNNING word is clear is not a voice: nothing it carries
     * reaches the bus, and its address does not move. */
    ax_pcm16_voice(s, AX_PB, AX_AR_C4000, 4096, 0x4000);
    ax_w(s, AX_PB, AXPB_MIXER + AXMX_L, 0x8000);
    ax_w(s, AX_PB, AXPB_MIXER + AXMX_R, 0x8000);
    ax_w(s, AX_PB, AXPB_RUNNING, 0);
    ax_run(s, AX_PB);
    snprintf(got, cap, "%s +%u", ax_bad_l(s, 0, 160, 0) < 0 && ax_bad_r(s, 0, 160, 0) < 0 ? "silent" : "audible",
             ax_r32(s, AX_PB, AXPB_AUDIO + 6) - AX_SMP(AX_AR_C4000));
    failures += check("ax stopped block is silent", got, "silent +0");

    /* ---- the auxiliary sends --------------------------------------------
     * This microcode build (hash 0x4E8A8B21) reads the mixer control word
     * differently from the later SDK headers: bit 0 adds auxiliary A, bit 1
     * auxiliary B, bit 2 surround, bit 3 the ramps. That is measurement, not
     * a published encoding -- under the later one every voice mixed into
     * nothing and the game was silent while reporting thousands of running
     * voices (FINDINGS section 10) -- so what these pin is that a send lands
     * on the bus its bit names and on no other, and that the buffer handed
     * to the CPU is channel-major 32-bit. Every send gain is set in both
     * runs and only the control word changes, so a bit that reaches one bus
     * too many shows up as a bus that should have been silent. */
    for (i = 0; i < 2; i++) {
        static const uint16_t ctrl[2] = {1, 2};
        static const uint16_t cmd[2] = {0x04, 0x05};
        int wrong;
        ax_pcm16_voice(s, AX_PB, AX_AR_C4000, 4096, 0x4000);
        ax_w(s, AX_PB, AXPB_MIXER_CTRL, ctrl[i]);
        ax_w(s, AX_PB, AXPB_MIXER + (i ? AXMX_BL : AXMX_AL), 0x8000);
        ax_w(s, AX_PB, AXPB_MIXER + (i ? AXMX_BR : AXMX_AR), 0x4000);
        ax_w(s, AX_PB, AXPB_MIXER + (i ? AXMX_AL : AXMX_BL), 0x8000);
        ax_w(s, AX_PB, AXPB_MIXER + (i ? AXMX_AR : AXMX_BR), 0x4000);
        ax_w(s, AX_PB, AXPB_MIXER + AXMX_AS, 0x8000); /* neither surround send may */
        ax_w(s, AX_PB, AXPB_MIXER + AXMX_BS, 0x8000); /* fire without bit 2 */
        ax_fill_bus(s, 0);
        ax_run_aux(s, AX_PB, cmd[i], AX_BUF, 0);
        wrong = ax_bad_bus(s, 0, 16384);
        if (wrong < 0) wrong = ax_bad_bus(s, 1, 8192);
        if (wrong < 0) wrong = ax_bad_bus(s, 2, 0);
        snprintf(got, cap, "%02X %02X %02X %02X %d %d %d %s", mem_r8(s, AX_BUF), mem_r8(s, AX_BUF + 1u),
                 mem_r8(s, AX_BUF + 2u), mem_r8(s, AX_BUF + 3u), ax_bus(s, 0, 0), ax_bus(s, 1, 0),
                 ax_bus(s, 2, 0), wrong < 0 ? "ok" : "varies");
        failures += check(i ? "ax aux B send" : "ax aux A send", got, "00 00 40 00 16384 8192 0 ok");

        /* and that same voice leaves the other bus alone */
        ax_fill_bus(s, 0);
        ax_run_aux(s, AX_PB, cmd[i ^ 1], AX_BUF, 0);
        wrong = ax_bad_bus(s, 0, 0);
        if (wrong < 0) wrong = ax_bad_bus(s, 1, 0);
        if (wrong < 0) wrong = ax_bad_bus(s, 2, 0);
        snprintf(got, cap, "%s", wrong < 0 ? "silent" : "leaked");
        failures += check(i ? "ax aux A not reached" : "ax aux B not reached", got, "silent");
    }

    /* Surround is bit 2 and nothing else: the same block with the same
     * surround gain is silent on the third channel without it. */
    {
        int sur[2];
        for (i = 0; i < 2; i++) {
            ax_pcm16_voice(s, AX_PB, AX_AR_C4000, 4096, 0x4000);
            ax_w(s, AX_PB, AXPB_MIXER_CTRL, i ? 4 : 0);
            ax_w(s, AX_PB, AXPB_MIXER + AXMX_S, 0x8000);
            ax_run(s, AX_PB);
            sur[i] = ax_out_s(s, 0);
            if (ax_out_s(s, 159) != sur[i]) sur[i] = -1; /* -1: not constant across the frame */
        }
        snprintf(got, cap, "%d %d", sur[0], sur[1]);
        failures += check("ax surround needs bit 2", got, "0 16384");
    }

    /* ---- auxiliary B under the other studio mode ------------------------
     * The driver chooses its auxiliary-B encoding once per studio and uses
     * it for every voice and every frame: bit 1 of the control word with
     * command 0x05, or bit 4 with command 0x10 (fn_8027CDF4 branches on the
     * studio's mode word at 0x8027D92C for the bit and 0x8027E848 for the
     * command). The port implemented neither half of the second mode, so
     * every send under it was dropped and nothing came back -- and no
     * scenario can catch that, because this title's mode word is 0 on every
     * reachable path, which is what makes these cases the only cover the
     * path has.
     *
     * Command 0x10's payload is the same five halfwords in the same order as
     * 0x05's, over buffers of the same shape (emitters 0x8027E900 and
     * 0x8027EA14), so these drive it exactly as the bit-1 cases above drive
     * 0x05. The one place the two encodings disagree is asserted rather than
     * assumed: bit 1 maintains and ramps three auxiliary-B gains, bit 4 two
     * (three calls to fn_8027C758 at 0x8027DCCC against two at 0x8027DD60),
     * so under bit 4 the bus's third channel has no gain and must stay
     * silent however the surround bit is set. */
    ax_pcm16_voice(s, AX_PB, AX_AR_C4000, 4096, 0x4000);
    ax_w(s, AX_PB, AXPB_MIXER_CTRL, 0x10 | 4);
    ax_w(s, AX_PB, AXPB_MIXER + AXMX_BL, 0x8000); /* 16384 at unity */
    ax_w(s, AX_PB, AXPB_MIXER + AXMX_BR, 0x4000); /* and half of it */
    ax_w(s, AX_PB, AXPB_MIXER + AXMX_AL, 0x8000); /* auxiliary A is bit 0 and is clear */
    ax_w(s, AX_PB, AXPB_MIXER + AXMX_AR, 0x8000);
    ax_w(s, AX_PB, AXPB_MIXER + AXMX_S, 0x8000);  /* whichever of the three surround */
    ax_w(s, AX_PB, AXPB_MIXER + AXMX_AS, 0x8000); /* gains is auxiliary B's, bit 4 */
    ax_w(s, AX_PB, AXPB_MIXER + AXMX_BS, 0x8000); /* must not reach the bus with it */
    ax_fill_bus(s, 0);
    ax_run_aux(s, AX_PB, 0x10, AX_BUF, 0);
    bad = ax_bad_bus(s, 0, 16384);
    if (bad < 0) bad = ax_bad_bus(s, 1, 8192);
    if (bad < 0) bad = ax_bad_bus(s, 2, 0);
    snprintf(got, cap, "%02X %02X %02X %02X %d %d %d %s", mem_r8(s, AX_BUF), mem_r8(s, AX_BUF + 1u),
             mem_r8(s, AX_BUF + 2u), mem_r8(s, AX_BUF + 3u), ax_bus(s, 0, 0), ax_bus(s, 1, 0),
             ax_bus(s, 2, 0), bad < 0 ? "ok" : "varies");
    failures += check("ax bit 4 aux B send", got, "00 00 40 00 16384 8192 0 ok");

    /* and the same voice reaches no other bus: command 0x04 uploads aux A,
     * which nothing has mixed into */
    ax_fill_bus(s, 0);
    ax_run_aux(s, AX_PB, 0x04, AX_BUF, 0);
    bad = ax_bad_bus(s, 0, 0);
    if (bad < 0) bad = ax_bad_bus(s, 1, 0);
    if (bad < 0) bad = ax_bad_bus(s, 2, 0);
    snprintf(got, cap, "%s", bad < 0 ? "silent" : "leaked");
    failures += check("ax bit 4 leaves aux A", got, "silent");

    /* Bit 3 is the ramp bit for both encodings, so it arms this one's two
     * gains too: from zero, stepping 0x0100 a sample, 16384 through a gain
     * of 256*i is (16384 * 256 * i) >> 15 = 128*i, and the gain the block
     * keeps after 160 samples is 160 * 256 = 0xA000. */
    ax_pcm16_voice(s, AX_PB, AX_AR_C4000, 4096, 0x4000);
    ax_w(s, AX_PB, AXPB_MIXER_CTRL, 0x10 | 8);
    ax_w(s, AX_PB, AXPB_MIXER + AXMX_BL, 0);
    ax_w(s, AX_PB, AXPB_MIXER + AXMX_DBL, 0x0100);
    ax_fill_bus(s, 0);
    ax_run_aux(s, AX_PB, 0x10, AX_BUF, 0);
    bad = -1;
    for (i = 0; i < 160; i++)
        if (ax_bus(s, 0, i) != 128 * i) { bad = i; break; }
    if (bad < 0)
        snprintf(got, cap, "0..20352 by 128, gain %04X", ax_r(s, AX_PB, AXPB_MIXER + AXMX_BL));
    else snprintf(got, cap, "%d at %d", ax_bus(s, 0, bad), bad);
    failures += check("ax bit 4 aux B ramp", got, "0..20352 by 128, gain A000");

    /* The whole path in one command, which is what the defect cost: the send
     * reaches the bus, the bus is handed out, and what comes back is mixed
     * into main. One buffer serves as both addresses, so the effects stage is
     * the identity and every output below is the two gains added --
     * 16384 * 0x4000 >> 15 = 8192 of main L plus 16384 * 0x8000 >> 15 = 16384
     * of auxiliary B on the left, nothing on main R plus
     * 16384 * 0x2000 >> 15 = 4096 on the right, and silence on surround with
     * bit 2 clear. Reaching OUTPUT at all also pins the command's length:
     * a 0x10 that consumed the wrong number of halfwords would leave the
     * 0xAA fill behind instead. */
    ax_pcm16_voice(s, AX_PB, AX_AR_C4000, 4096, 0x4000);
    ax_w(s, AX_PB, AXPB_MIXER_CTRL, 0x10);
    ax_w(s, AX_PB, AXPB_MIXER + AXMX_L, 0x4000);
    ax_w(s, AX_PB, AXPB_MIXER + AXMX_R, 0);
    ax_w(s, AX_PB, AXPB_MIXER + AXMX_BL, 0x8000);
    ax_w(s, AX_PB, AXPB_MIXER + AXMX_BR, 0x2000);
    ax_fill_bus(s, 0);
    ax_run_aux(s, AX_PB, 0x10, AX_BUF, AX_BUF);
    bad = ax_bad_l(s, 0, 160, 24576);
    if (bad < 0) bad = ax_bad_r(s, 0, 160, 4096);
    if (bad < 0)
        for (i = 0; i < 160; i++)
            if (ax_out_s(s, i) != 0) { bad = i; break; }
    snprintf(got, cap, "%d %d %d %s", ax_out_l(s, 0), ax_out_r(s, 0), ax_out_s(s, 0),
             bad < 0 ? "mixed back" : "wrong");
    failures += check("ax bit 4 round trip", got, "24576 4096 0 mixed back");

    /* ---- what the CPU's effects pass hands back -------------------------
     * The download adds to the main bus rather than replacing it, and covers
     * all three channels -- the surround one is the half a two-channel loop
     * would drop in silence. The pattern is small enough that nothing
     * clamps, so every output below is derived addition. */
    for (i = 0; i < 2; i++) {
        static const uint16_t cmd[2] = {0x04, 0x09};
        int k, wrong = -1;
        ax_pcm16_voice(s, AX_PB, AX_AR_C4000, 4096, 0x4000);
        ax_w(s, AX_PB, AXPB_MIXER_CTRL, 4); /* surround as well as L and R */
        ax_w(s, AX_PB, AXPB_MIXER + AXMX_L, 0x8000);
        ax_w(s, AX_PB, AXPB_MIXER + AXMX_R, 0x8000);
        ax_w(s, AX_PB, AXPB_MIXER + AXMX_S, 0x8000);
        ax_fill_bus(s, 1);
        ax_run_aux(s, AX_PB, cmd[i], 0, AX_BUF);
        for (k = 0; k < 160 && wrong < 0; k++)
            if (ax_out_l(s, k) != 16384 + k || ax_out_r(s, k) != 16544 + k ||
                ax_out_s(s, k) != 16704 + k)
                wrong = k;
        snprintf(got, cap, "%d %d %d %s", ax_out_l(s, 0), ax_out_r(s, 0), ax_out_s(s, 0),
                 wrong < 0 ? "added" : "wrong");
        failures += check(i ? "ax aux B nowrite adds" : "ax download adds", got, "16384 16544 16704 added");
    }

    /* ---- SET_OPPOSITE_LR replaces the main bus --------------------------
     * The command is named SET_, not MIX_, so what it writes must land
     * instead of what the voices put there rather than on top of it. Which
     * channel gets the negated copy is this port's reading of the driver and
     * not something a selftest can settle, so this checks the magnitude and
     * that the two channels are opposite, and leaves the sign to the
     * capture. */
    ax_pcm16_voice(s, AX_PB, AX_AR_C4000, 4096, 0x4000);
    ax_w(s, AX_PB, AXPB_MIXER + AXMX_L, 0x8000);
    ax_w(s, AX_PB, AXPB_MIXER + AXMX_R, 0x8000);
    for (i = 0; i < 480; i++) mem_w32(s, AX_BUF + (uint32_t)i * 4u, 1000u);
    ax_fill_out(s, 0xAA);
    {
        uint32_t p = AX_CMD;
        p = ax_put16(s, p, 0x02);
        p = ax_put32(s, p, AX_PB);
        p = ax_put16(s, p, 0x03);
        p = ax_put16(s, p, 0x11);
        p = ax_put32(s, p, AX_BUF);
        p = ax_put16(s, p, 0x0E);
        p = ax_put32(s, p, AX_SUR);
        p = ax_put32(s, p, AX_OUT);
        ax_put16(s, p, 0x0F);
        ax_command_list(s, AX_CMD);
    }
    snprintf(got, cap, "%d %d %d", ax_out_l(s, 0) < 0 ? -ax_out_l(s, 0) : ax_out_l(s, 0),
             ax_out_l(s, 0) + ax_out_r(s, 0), ax_out_s(s, 0));
    failures += check("ax set-opposite replaces", got, "1000 0 0");

    /* ---- the interpolator must not overflow -----------------------------
     * Nothing is fetched for the first output when the ratio is under one
     * and the fraction starts at zero, so this is the interpolator alone: at
     * 65535/65536 of the way from -32768 to +1 the answer is
     *   -32768 + (32769 * 65535) / 65536 = -32768 + 32768 = 0,
     * silence. ax.c forms (last[3] - last[2]) * frac in `int`, and
     * 32769 * 65535 = 2,147,516,415 is 32,768 past INT32_MAX, so the product
     * wraps and the frame opens at full-scale negative instead: an audible
     * click, reachable on any loud sample that crosses near full scale
     * between adjacent source samples. Command 0x01 already does its
     * multiply in int64_t, so the file disagrees with itself.
     *
     * This case therefore FAILS as ax.c stands, and it is meant to: the
     * expected value is the interpolation's own arithmetic, not what the
     * mixer emits today. Widening that product makes it pass. */
    ax_pcm16_voice(s, AX_PB, AX_AR_C4000, 4096, 0x4000);
    ax_w32(s, AX_PB, AXPB_SRC, 0xFFFFu);
    ax_w(s, AX_PB, AXPB_SRC + 5, (uint16_t)-32768); /* last[2] */
    ax_w(s, AX_PB, AXPB_SRC + 6, 1);                /* last[3] */
    ax_w(s, AX_PB, AXPB_MIXER + AXMX_L, 0x8000);
    ax_run(s, AX_PB);
    snprintf(got, cap, "%d", ax_out_l(s, 0));
    failures += check("ax interpolate full scale", got, "0");

    /* ---- the walkers terminate ------------------------------------------
     * Each of these is a malformed list or chain a stale guest pointer could
     * produce. The limits are this port's own rather than the microcode's,
     * so what they pin is that a bad list cannot run forever and cannot run
     * on past where the walk stopped. */
    ax_fill_out(s, 0xAA);
    {
        uint32_t p = AX_CMD;
        p = ax_put16(s, p, 0x0F); /* END, then an OUTPUT that must not run */
        p = ax_put16(s, p, 0x0E);
        p = ax_put32(s, p, AX_SUR);
        p = ax_put32(s, p, AX_OUT);
        ax_command_list(s, AX_CMD);
    }
    snprintf(got, cap, "%02X", mem_r8(s, AX_OUT));
    failures += check("ax end stops the list", got, "AA");

    /* The walk gives up after 64 commands, so an OUTPUT as the 64th runs and
     * the same list one command longer does not. */
    {
        unsigned first[2];
        for (i = 0; i < 2; i++) {
            uint32_t p = AX_CMD;
            int k;
            for (k = 0; k < 63 + i; k++) p = ax_put16(s, p, 0x0B); /* no operands */
            p = ax_put16(s, p, 0x0E);
            p = ax_put32(s, p, AX_SUR);
            p = ax_put32(s, p, AX_OUT);
            ax_put16(s, p, 0x0F);
            ax_fill_out(s, 0xAA);
            ax_command_list(s, AX_CMD);
            first[i] = mem_r8(s, AX_OUT);
        }
        snprintf(got, cap, "%02X %02X", first[0], first[1]);
        failures += check("ax 64-command cap", got, "00 AA");
    }

    /* A block whose chain pointer is itself is processed once, not forever:
     * 160 samples consumed, not 320. */
    ax_pcm16_voice(s, AX_PB, AX_AR_C4000, 4096, 0x4000);
    ax_w32(s, AX_PB, AXPB_NEXT, AX_PB);
    ax_run(s, AX_PB);
    snprintf(got, cap, "+%u", ax_r32(s, AX_PB, AXPB_AUDIO + 6) - AX_SMP(AX_AR_C4000));
    failures += check("ax self-linked block", got, "+160");

    /* And a 300-deep chain stops at the 256th block. Each one is silent but
     * carries a single update-list entry, which process_voice applies before
     * it looks at RUNNING, so the blocks the walk reached are exactly those
     * that now hold the marker. */
    mem_w16(s, AX_UPD, AXPB_SRC_TYPE);
    mem_w16(s, AX_UPD + 2u, 0xBEEF);
    for (i = 0; i < 300; i++) {
        uint32_t pb = AX_PB + (uint32_t)i * 0xC0u;
        ax_pb_clear(s, pb);
        ax_w32(s, pb, AXPB_NEXT, i == 299 ? 0u : pb + 0xC0u);
        ax_w(s, pb, AXPB_UPDATES, 1);
        ax_w32(s, pb, AXPB_UPDATES + 5, AX_UPD);
    }
    ax_run(s, AX_PB);
    snprintf(got, cap, "%04X %04X", ax_r(s, AX_PB + 255u * 0xC0u, AXPB_SRC_TYPE),
             ax_r(s, AX_PB + 256u * 0xC0u, AXPB_SRC_TYPE));
    failures += check("ax chain guard", got, "BEEF 0000");

    return failures;
}

/* ---- decompiled functions against their recompiled twins ----------------
 * src/ is also compiled natively (every function renamed dc_<name>, see
 * tools/recompile.py). Each pair runs on the same bytes in guest memory: the
 * twin through dispatch() with guest addresses, the decompiled C on host
 * pointers into the same memory. Same answers on random inputs, or fail. */
size_t dc_strlen(const char* str);
char* dc_strchr(const char* str, int chr);
void* dc_memchr(const void* src, int val, size_t n);
void* dc___memrchr(const void* src, int val, size_t n);
int dc_strncmp(const char* a, const char* b, size_t n);
char* dc_strcat(char* dst, const char* src);
char* dc_strncpy(char* dst, const char* src, size_t n);
void* dc_memcpy(void* dst, const void* src, size_t n);
void* dc_memset(void* dst, int val, size_t n);

/* The recompiled twins, by name: dispatch() now reaches the adapters in
 * runtime/decomp_swap.c for these addresses (config/hle.txt). */
#ifdef SOA_NO_DECOMP
/* A build without src/ (distribution 3.1) binds none of these: the translation
 * is fn_ itself, and the twins the checks below run are the game's own. */
#define recomp_fn_8025F1D8 fn_8025F1D8
#define recomp_fn_8025EF18 fn_8025EF18
#define recomp_fn_8025C73C fn_8025C73C
#define recomp_fn_8025C710 fn_8025C710
#define recomp_fn_8025EF48 fn_8025EF48
#define recomp_fn_8025F0B0 fn_8025F0B0
#define recomp_fn_8025F0DC fn_8025F0DC
#define recomp_fn_80005520 fn_80005520
#define recomp_fn_80005434 fn_80005434
#define recomp_fn_8025EF88 fn_8025EF88
#define recomp_fn_8025ED74 fn_8025ED74
#define recomp_fn_8025F120 fn_8025F120
#endif
void recomp_fn_8025F1D8(CpuState* s); void recomp_fn_8025EF18(CpuState* s); void recomp_fn_8025C73C(CpuState* s);
void recomp_fn_8025C710(CpuState* s); void recomp_fn_8025EF48(CpuState* s); void recomp_fn_8025F0B0(CpuState* s);
void recomp_fn_8025F0DC(CpuState* s); void recomp_fn_80005520(CpuState* s); void recomp_fn_80005434(CpuState* s);
void recomp_fn_8025EF88(CpuState* s); void recomp_fn_8025ED74(CpuState* s); void recomp_fn_8025F120(CpuState* s);
int dc_fn_8025EF88(const char* a, const char* b);
char* dc_strstr(const char* hay, const char* needle);
char* dc_strcpy(char* dst, const char* src);

static void run_twin(CpuState* s, void (*fn)(CpuState*))
{
    s->gpr[1] = STACK_TOP - 0x400;
    s->gpr[2] = SDA2_BASE;
    s->gpr[13] = SDA_BASE;
    s->lr = 0;
    s->cr = 0;
    s->msr = 0x00002030u;
    fn(s);
}

static uint32_t g_rng = 0x2545F491u;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

static void random_string(CpuState* s, uint32_t addr, unsigned len)
{
    unsigned i;
    for (i = 0; i < len; i++) mem_w8(s, addr + i, (uint8_t)('a' + rnd() % 6)); /* a small alphabet: repeats matter */
    mem_w8(s, addr + len, 0);
}

static int decomp_selftest(CpuState* s, char* got, size_t cap)
{
#ifdef SOA_NO_DECOMP
    (void)s;
    (void)got;
    (void)cap;
    fprintf(stderr, "[selftest] %-28s skip  a build without src/: the translated MSL runs (distribution 3.1)\n",
            "decompiled vs recompiled");
    return 0;
#else
    const uint32_t A = SCRATCH + 0x1000, B = SCRATCH + 0x1200, DST = SCRATCH + 0x1400;
    int failures = 0, round, bad = 0;
    for (round = 0; round < 200 && !bad; round++) {
        unsigned la = rnd() % 40, lb = rnd() % 40, n = rnd() % 48;
        int chr = 'a' + (int)(rnd() % 8), r1, r2;
        uint32_t p1, p2;
        random_string(s, A, la); random_string(s, B, lb);

        s->gpr[3] = A; run_twin(s, recomp_fn_8025F1D8); /* strlen */
        if (s->gpr[3] != (uint32_t)dc_strlen((const char*)mem_ptr(s, A))) { bad = 1; snprintf(got, cap, "strlen round %d", round); }

        s->gpr[3] = A; s->gpr[4] = (uint32_t)chr; run_twin(s, recomp_fn_8025EF18); /* strchr */
        p1 = s->gpr[3];
        { char* r = dc_strchr((const char*)mem_ptr(s, A), chr); p2 = r ? (uint32_t)(A + (r - (char*)mem_ptr(s, A))) : 0; }
        if (p1 != p2) { bad = 1; snprintf(got, cap, "strchr round %d: twin %08X, C %08X", round, p1, p2); }

        s->gpr[3] = A; s->gpr[4] = (uint32_t)chr; s->gpr[5] = n; run_twin(s, recomp_fn_8025C73C); /* memchr */
        p1 = s->gpr[3];
        { char* r = (char*)dc_memchr(mem_ptr(s, A), chr, n); p2 = r ? (uint32_t)(A + (r - (char*)mem_ptr(s, A))) : 0; }
        if (p1 != p2) { bad = 1; snprintf(got, cap, "memchr round %d: twin %08X, C %08X", round, p1, p2); }

        s->gpr[3] = A; s->gpr[4] = (uint32_t)chr; s->gpr[5] = n; run_twin(s, recomp_fn_8025C710); /* __memrchr */
        p1 = s->gpr[3];
        { char* r = (char*)dc___memrchr(mem_ptr(s, A), chr, n); p2 = r ? (uint32_t)(A + (r - (char*)mem_ptr(s, A))) : 0; }
        if (p1 != p2) { bad = 1; snprintf(got, cap, "memrchr round %d: twin %08X, C %08X", round, p1, p2); }

        s->gpr[3] = A; s->gpr[4] = B; s->gpr[5] = n; run_twin(s, recomp_fn_8025EF48); /* strncmp */
        r1 = (int)s->gpr[3]; r2 = dc_strncmp((const char*)mem_ptr(s, A), (const char*)mem_ptr(s, B), n);
        if (r1 != r2) { bad = 1; snprintf(got, cap, "strncmp round %d: twin %d, C %d", round, r1, r2); }

        /* strcmp returns the sign of a comparison, and the twin's word path is
         * big-endian, so this is the one twin whose C needs a host-order guard.
         * Compare the sign rather than the value: the two are allowed to differ
         * in magnitude and the guest only ever tests the sign. */
        s->gpr[3] = A; s->gpr[4] = B; run_twin(s, recomp_fn_8025EF88); /* strcmp */
        r1 = (int)s->gpr[3]; r2 = dc_fn_8025EF88((const char*)mem_ptr(s, A), (const char*)mem_ptr(s, B));
        if ((r1 > 0) - (r1 < 0) != (r2 > 0) - (r2 < 0))
            { bad = 1; snprintf(got, cap, "strcmp round %d: twin %d, C %d", round, r1, r2); }

        s->gpr[3] = A; s->gpr[4] = B; run_twin(s, recomp_fn_8025ED74); /* strstr */
        p1 = s->gpr[3];
        { char* r = (char*)dc_strstr((const char*)mem_ptr(s, A), (const char*)mem_ptr(s, B));
          p2 = r ? (uint32_t)(A + (r - (char*)mem_ptr(s, A))) : 0; }
        if (p1 != p2) { bad = 1; snprintf(got, cap, "strstr round %d: twin %08X, C %08X", round, p1, p2); }

        /* strcat and strncpy write: run the twin, snapshot, run the C on a fresh copy, compare */
        {
            uint8_t twin[128], native[128];
            memset(mem_ptr(s, DST), 'z', 96); random_string(s, DST, rnd() % 24);
            memcpy(native, mem_ptr(s, DST), 128);
            s->gpr[3] = DST; s->gpr[4] = A; run_twin(s, recomp_fn_8025F0B0); /* strcat */
            memcpy(twin, mem_ptr(s, DST), 128);
            dc_strcat((char*)native, (const char*)mem_ptr(s, A));
            if (memcmp(twin, native, 128) != 0) { bad = 1; snprintf(got, cap, "strcat round %d", round); }

            memset(mem_ptr(s, DST), 'z', 96); mem_w8(s, DST + 96, 0);
            memcpy(native, mem_ptr(s, DST), 128);
            s->gpr[3] = DST; s->gpr[4] = B; s->gpr[5] = n; run_twin(s, recomp_fn_8025F0DC); /* strncpy */
            memcpy(twin, mem_ptr(s, DST), 128);
            dc_strncpy((char*)native, (const char*)mem_ptr(s, B), n);
            if (memcmp(twin, native, 128) != 0) { bad = 1; snprintf(got, cap, "strncpy round %d", round); }

            /* strcpy's word path reads up to three bytes past the terminator
             * inside an aligned word, so compare the whole buffer: a twin that
             * wrote one byte too many would pass a strcmp of the result. */
            memset(mem_ptr(s, DST), 'z', 96); mem_w8(s, DST + 96, 0);
            memcpy(native, mem_ptr(s, DST), 128);
            s->gpr[3] = DST; s->gpr[4] = A; run_twin(s, recomp_fn_8025F120); /* strcpy */
            memcpy(twin, mem_ptr(s, DST), 128);
            dc_strcpy((char*)native, (const char*)mem_ptr(s, A));
            if (memcmp(twin, native, 128) != 0) { bad = 1; snprintf(got, cap, "strcpy round %d", round); }

            /* memcpy in both directions over an overlapping region, and memset */
            {
                unsigned off = rnd() % 8, len = rnd() % 40;
                uint32_t from = DST + off, to = DST + (rnd() % 16);
                random_string(s, DST, 60);
                memcpy(native, mem_ptr(s, DST), 128);
                s->gpr[3] = to; s->gpr[4] = from; s->gpr[5] = len; run_twin(s, recomp_fn_80005520); /* memcpy */
                memcpy(twin, mem_ptr(s, DST), 128);
                dc_memcpy(native + (to - DST), native + off, len);
                if (memcmp(twin, native, 128) != 0) { bad = 1; snprintf(got, cap, "memcpy round %d", round); }
                s->gpr[3] = DST + off; s->gpr[4] = (uint32_t)chr; s->gpr[5] = len; run_twin(s, recomp_fn_80005434); /* memset */
                memcpy(twin, mem_ptr(s, DST), 128);
                dc_memset(native + off, chr, len);
                if (memcmp(twin, native, 128) != 0) { bad = 1; snprintf(got, cap, "memset round %d", round); }
            }
        }
    }
    if (!bad) snprintf(got, cap, "12 functions agree over %d rounds", round);
    failures += check("decompiled vs recompiled", got, bad ? "agreement" : got);
    return failures;
#endif
}

/* VIGetRetraceCount is answered by runtime/tick.c (PLAN M2), which must be the
 * original -- `lwz r3,-27836(r13)` -- wherever it is not the main loop's safe
 * point or an unlocked spin. Those two call sites are held by test_tick.py;
 * this holds the rest to the recompiled twin, over random counts and call
 * sites, including the top of the loop, where it only adds a callback pass. */
void recomp_fn_8023F704(CpuState* s);
void fn_8023F704(CpuState* s);

static int tick_selftest(CpuState* s, char* got, size_t cap)
{
    static const uint32_t sites[] = {0, 0x801DCB88u, 0x80001000u, 0x8023F708u};
    int round, bad = 0;
    for (round = 0; round < 200 && !bad; round++) {
        uint32_t count = rnd(), lr = sites[round % 4], twin, native;
        mem_w32(s, 0x80347A64u, count);
        run_twin(s, recomp_fn_8023F704);
        twin = s->gpr[3];
        s->gpr[13] = SDA_BASE;
        s->lr = lr;
        fn_8023F704(s);
        native = s->gpr[3];
        if (twin != native || native != count) {
            bad = 1;
            snprintf(got, cap, "round %d, lr %08X: twin %08X, native %08X, count %08X", round, lr, twin, native, count);
        }
    }
    if (!bad) snprintf(got, cap, "the count at 0x80347A64 over %d rounds and four call sites", round);
    return check("VIGetRetraceCount native vs twin", got, bad ? "agreement" : got);
}

/* The data-cache range calls answered natively (H13b) against their
 * recompiled twins, over random ranges and empty ones: neither side moves the
 * stack, and the two that end in `sc` make it exactly when the twin does. */
void recomp_fn_80232E38(CpuState* s); void recomp_fn_80232E64(CpuState* s); void recomp_fn_80232E94(CpuState* s);
void recomp_fn_80232EC4(CpuState* s); void recomp_fn_80232EF0(CpuState* s);
void fn_80232E38(CpuState* s); void fn_80232E64(CpuState* s); void fn_80232E94(CpuState* s);
void fn_80232EC4(CpuState* s); void fn_80232EF0(CpuState* s);
uint64_t hle_syscall_count(void);

static int dcache_selftest(CpuState* s, char* got, size_t cap)
{
    static void (*const twin[5])(CpuState*) = {recomp_fn_80232E38, recomp_fn_80232E64, recomp_fn_80232E94,
                                               recomp_fn_80232EC4, recomp_fn_80232EF0};
    static void (*const native[5])(CpuState*) = {fn_80232E38, fn_80232E64, fn_80232E94, fn_80232EC4, fn_80232EF0};
    int round, k, bad = 0;
    for (round = 0; round < 100 && !bad; round++) {
        for (k = 0; k < 5 && !bad; k++) {
            uint32_t start = SCRATCH + rnd() % 4096u, len = round % 4 == 0 ? 0u : rnd() % 16384u;
            uint64_t c0, c1, c2;
            uint32_t sp;
            c0 = hle_syscall_count();
            s->gpr[3] = start; s->gpr[4] = len;
            run_twin(s, twin[k]);
            c1 = hle_syscall_count();
            sp = s->gpr[1];
            s->gpr[3] = start; s->gpr[4] = len;
            native[k](s);
            c2 = hle_syscall_count();
            if (c2 - c1 != c1 - c0 || s->gpr[1] != sp) {
                bad = 1;
                snprintf(got, cap, "call %d, %08X+%u: twin %llu syscalls, native %llu, sp %08X then %08X", k, start, len,
                         (unsigned long long)(c1 - c0), (unsigned long long)(c2 - c1), sp, s->gpr[1]);
            }
        }
    }
    if (!bad) snprintf(got, cap, "five calls over %d rounds, a quarter of them empty", round);
    return check("DC range calls native vs twin", got, bad ? "agreement" : got);
}

/* psq_load and psq_store (PLAN-60FPS-MODS H13a) against the generic formula
 * they replaced -- the scale from ldexp on every access, whatever the type --
 * over all eight types and all 64 scales, paired and single, with random bit
 * patterns in (NaNs and infinities among them) and random floats out. The two
 * must agree bit for bit, loads and stores both. */
static int psq_selftest(CpuState* s, char* got, size_t cap)
{
    const uint32_t A = SCRATCH + 0x2000, B = SCRATCH + 0x2100, C = SCRATCH + 0x2200;
    int type, sc, i, bad = 0, n = 0;
    for (type = 0; type < 8 && !bad; type++) {
        for (sc = 0; sc < 64 && !bad; sc++) {
            uint32_t g = ((uint32_t)type << 16) | ((uint32_t)sc << 24) | (uint32_t)type | ((uint32_t)sc << 8);
            for (i = 0; i < 16 && !bad; i++, n++) {
                int w = i & 1;
                uint32_t size, k, u0 = rnd(), u1 = rnd();
                float f0, f1;
                double deq = ldexp(1.0, -psq_sscale(g >> 24)), quant = ldexp(1.0, psq_sscale(g >> 8)), r0, r1;
                uint64_t got0, got1, want0, want1;
                mem_w32(s, A, rnd());
                mem_w32(s, A + 4, rnd());
                s->gqr[2] = g;
                psq_load(s, 5, A, 2, w);
                r0 = psq_load1(s, A, (uint32_t)type, deq, &size);
                r1 = w ? 1.0 : psq_load1(s, A + size, (uint32_t)type, deq, &size);
                r0 = (double)(float)r0;
                r1 = (double)(float)r1;
                memcpy(&got0, &s->fpr[5].ps0, 8); memcpy(&got1, &s->fpr[5].ps1, 8);
                memcpy(&want0, &r0, 8); memcpy(&want1, &r1, 8);
                if (got0 != want0 || got1 != want1) {
                    bad = 1;
                    snprintf(got, cap, "load type %d scale %d w %d: %016llX %016llX, generic %016llX %016llX", type, sc, w,
                             (unsigned long long)got0, (unsigned long long)got1, (unsigned long long)want0, (unsigned long long)want1);
                    break;
                }
                memcpy(&f0, &u0, 4); memcpy(&f1, &u1, 4);
                if (i & 2) { f0 = (float)((int32_t)u0 >> (u0 & 15)); f1 = (float)((int32_t)u1 >> (u1 & 15)); }
                s->fpr[6].ps0 = f0; s->fpr[6].ps1 = f1;
                mem_w32(s, B, 0); mem_w32(s, B + 4, 0); mem_w32(s, C, 0); mem_w32(s, C + 4, 0);
                psq_store(s, 6, B, 2, w);
                psq_store1(s, C, (uint32_t)type, quant, s->fpr[6].ps0, &size);
                if (!w) psq_store1(s, C + size, (uint32_t)type, quant, s->fpr[6].ps1, &size);
                for (k = 0; k < 8; k++)
                    if (mem_r8(s, B + k) != mem_r8(s, C + k)) {
                        bad = 1;
                        snprintf(got, cap, "store type %d scale %d w %d: byte %u is %02X, generic %02X", type, sc, w, k,
                                 mem_r8(s, B + k), mem_r8(s, C + k));
                        break;
                    }
            }
        }
    }
    if (!bad) snprintf(got, cap, "%d loads and stores over 8 types and 64 scales agree with the generic path", n);
    return check("psq_load/psq_store vs generic", got, bad ? "agreement" : got);
}

/* The race seed (P6, seed.c): with a seed set, the game's own OSGetTick
 * called from each of the three reseed sites returns that site's pin, and
 * the game's srand stores it at the seed word 0x803469A8; from a return
 * address one instruction past the last site (0x8000A1DC, the plan's
 * mutation) nothing is pinned. */
void fn_8023851C(CpuState* s); /* OSGetTick */
void fn_8025ECBC(CpuState* s); /* srand */
void seed_set(int on, uint32_t seed);
uint32_t seed_value(uint32_t s, int site, uint32_t n);

static int seed_selftest(CpuState* s, char* got, size_t cap)
{
    static const uint32_t sites[3] = {0x801012B0u, 0x8000A1D0u, 0x8000A1D8u};
    const uint32_t seed = 12345u;
    int i, n, bad = 0;
    uint32_t v;
    seed_set(1, seed);
    for (n = 0; n < 2 && !bad; n++)
        for (i = 0; i < 3 && !bad; i++) {
            uint32_t want = seed_value(seed, i, (uint32_t)n);
            s->gpr[1] = STACK_TOP - 0x400; s->gpr[2] = SDA2_BASE; s->gpr[13] = SDA_BASE;
            s->lr = sites[i];
            fn_8023851C(s);
            v = s->gpr[3];
            s->lr = sites[i] + 8;
            fn_8025ECBC(s);
            if (v != want || mem_r32(s, 0x803469A8u) != want) {
                bad = 1;
                snprintf(got, cap, "call %d from %08X: OSGetTick %08X, seed word %08X, the pin %08X", n, sites[i], v,
                         mem_r32(s, 0x803469A8u), want);
            }
        }
    s->lr = 0x8000A1DCu;
    fn_8023851C(s);
    v = s->gpr[3];
    for (n = 0; n < 4 && !bad; n++)
        for (i = 0; i < 3 && !bad; i++)
            if (v == seed_value(seed, i, (uint32_t)n)) {
                bad = 1;
                snprintf(got, cap, "OSGetTick from 8000A1DC returned a pin, %08X", v);
            }
    seed_set(0, 0);
    if (!bad) snprintf(got, cap, "three sites pinned twice each and srand stored each pin; 8000A1DC left to the clock");
    return check("race seed at OSGetTick", got, bad ? "agreement" : got);
}

/* The encounter multiplier as the game itself works it out (PLAN-GAMEPLAY-MODS
 * P1a): mods/encounter-rate computes base and the game's own byte from the
 * party's accessories every frame and writes the byte back at normal after a
 * hold of B, so its reckoning has to be the game's. The game's is
 * fn_801EF7E0, which Continue and the equipment menus call (a warp does not:
 * FINDINGS "P1a"). Here it runs on parties set up in memory -- no accessory
 * with effect 84, accessory 210 on character 0, and 211 on character 1 as
 * well -- and must leave the byte at 0x8030B7AD as the mod's base_now
 * would: FF, 64, and 05, the last character's. The party block and the
 * bytes the function writes are put back afterwards. */
void fn_801EF7E0(CpuState* s);

static int encounter_selftest(CpuState* s, char* got, size_t cap)
{
    static uint8_t saved[0x8030BC00u - 0x8030B4D4u];
    static const struct {
        uint16_t acc[6];
        uint8_t want;
        const char* what;
    } k_case[] = {
        {{0, 0, 0, 0, 0, 0}, 0xFF, "no accessory"},
        {{204, 201, 0, 0, 0, 0}, 0xFF, "204 and 201, no effect 84"},
        {{210, 0, 0, 0, 0, 0}, 0x64, "210 on character 0"},
        {{210, 211, 0, 0, 0, 0}, 0x05, "210 on 0, 211 on 1"},
        {{0, 0, 0, 0, 0, 211}, 0x05, "211 on character 5"},
    };
    unsigned i, c;
    int bad = 0;
    memcpy(saved, mem_ptr(s, 0x8030B4D4u), sizeof saved);
    for (i = 0; i < sizeof k_case / sizeof k_case[0] && !bad; i++) {
        uint8_t v;
        for (c = 0; c < 6; c++) mem_w16(s, 0x8030B7F4u + 92u * c + 20u, k_case[i].acc[c]);
        mem_w8(s, 0x8030B7ADu, 0x5A);
        s->gpr[1] = STACK_TOP - 0x400; s->gpr[2] = SDA2_BASE; s->gpr[13] = SDA_BASE;
        s->lr = 0x801A4538u; /* Continue's call */
        fn_801EF7E0(s);
        v = mem_r8(s, 0x8030B7ADu);
        if (v != k_case[i].want) {
            bad = 1;
            snprintf(got, cap, "%s: the byte %02X, not %02X", k_case[i].what, v, k_case[i].want);
        }
    }
    memcpy(mem_ptr(s, 0x8030B4D4u), saved, sizeof saved);
    if (!bad) snprintf(got, cap, "FF with none or without effect 84, 64 for 210, 05 for 211 on a later character");
    return check("encounter multiplier as the game has it", got, bad ? "agreement" : got);
}

/* The rumble motor (PLAN-GAMEPLAY-MODS M18): what si.c hands the sink for
 * each OUTBUF write PADControlMotor would make, through si_write as the game
 * makes it. A counting sink stands in for XInput. */
int si_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v);
void si_set_motor_sink(void (*fn)(unsigned speed));
void si_set_motor_window(int open);
void si_set_motor_strength(int percent);
void si_motor_stop(void);

static unsigned g_sink_calls, g_sink_speed;
static void count_sink(unsigned speed)
{
    g_sink_calls++;
    g_sink_speed = speed;
}

/* One OUTBUF write; the calls it made, and the last speed. */
static unsigned motor_write(CpuState* s, unsigned chan, unsigned cmd, unsigned* speed)
{
    unsigned before = g_sink_calls;
    g_sink_speed = 0xDEAD;
    si_write(s, 0xCC006400u + 12u * chan, 4, 0x00400300u | cmd);
    *speed = g_sink_speed;
    return g_sink_calls - before;
}

static int motor_selftest(CpuState* s, char* got, size_t cap)
{
    char pad_was[256] = "";
    const char* was = getenv("SOA_PAD");
    unsigned n, v, c, bad = 0;
#define WANT(cond, what) do { if (!bad && !(cond)) { bad = 1; snprintf(got, cap, "%s (calls %u, speed %u)", what, n, v); } } while (0)
    if (was) snprintf(pad_was, sizeof pad_was, "%s", was);
    plat_setenv("SOA_PAD", "");
    si_set_motor_sink(count_sink);
    si_set_motor_window(1);
    si_set_motor_strength(100);
    n = motor_write(s, 0, 0, &v);             /* the state it starts in: no call */
    WANT(n == 0, "a stop while stopped called the sink");
    n = motor_write(s, 0, 1, &v);
    WANT(n == 1 && v == 65535, "rumble did not turn the motor at full strength");
    n = motor_write(s, 0, 1, &v);
    WANT(n == 0, "the same bits again called the sink");
    n = motor_write(s, 0, 0, &v);
    WANT(n == 1 && v == 0, "stop did not send speed 0");
    n = motor_write(s, 0, 1, &v);
    n = motor_write(s, 0, 2, &v);
    WANT(n == 1 && v == 0, "stop hard did not send speed 0");
    for (c = 1; c < 4; c++) {
        n = motor_write(s, c, 1, &v) + motor_write(s, c, 0, &v);
        WANT(n == 0, "a channel other than 0 called the sink");
    }
    n = motor_write(s, 0, 1, &v);
    {
        unsigned before = g_sink_calls;
        si_motor_stop();
        n = g_sink_calls - before;
        v = g_sink_speed;
        WANT(n == 1 && v == 0, "si_motor_stop while on did not send one speed 0");
        before = g_sink_calls;
        si_motor_stop();
        n = g_sink_calls - before;
        WANT(n == 0, "si_motor_stop while off called the sink");
    }
    motor_write(s, 0, 0, &v);
    si_set_motor_strength(50);
    n = motor_write(s, 0, 1, &v);
    WANT(n == 1 && v == 32767, "strength 50 did not give half speed");
    motor_write(s, 0, 0, &v);
    si_set_motor_strength(0);
    n = motor_write(s, 0, 1, &v);
    WANT(n == 1 && v == 0, "strength 0 turned the motor");
    motor_write(s, 0, 0, &v);
    si_set_motor_strength(100);
    si_set_motor_window(0);
    n = motor_write(s, 0, 1, &v);
    WANT(n == 1 && v == 0, "with no window the motor turned");
    motor_write(s, 0, 0, &v);
    si_set_motor_window(1);
    plat_setenv("SOA_PAD", "100:a");
    n = motor_write(s, 0, 1, &v);
    WANT(n == 1 && v == 0, "with a pad script driving the input the motor turned");
    motor_write(s, 0, 0, &v);
    plat_setenv("SOA_PAD", pad_was);
#undef WANT
    si_set_motor_window(0);
    si_set_motor_sink(NULL);
    if (!bad) snprintf(got, cap, "on, off, off hard, full and half strength; nothing for channels 1-3, strength 0, no window or a script");
    return check("rumble motor from OUTBUF", got, bad ? "agreement" : got);
}

/* Silence with another window in front (M5b, `unfocused = mute`): what
 * audio_push_block hands the device, through the test sink, is zeros while
 * muted and the block's samples -- left then right, little-endian -- once
 * not. */
void audio_push_block(const uint8_t* be_rl, unsigned bytes, unsigned rate);
void audio_set_muted(int on);
void audio_set_test_sink(void (*fn)(const int16_t* lr, unsigned frames));

static int16_t g_heard[16];
static unsigned g_heard_n;
static void hear(const int16_t* lr, unsigned frames)
{
    unsigned i;
    g_heard_n = frames;
    for (i = 0; i < 2 * frames && i < 16; i++) g_heard[i] = lr[i];
}

static int mute_selftest(CpuState* s, char* got, size_t cap)
{
    /* four frames of right, left: 0x1234,0x0567 / -2,3 / 0x7FFF,-0x8000 / 1,-1 */
    static const uint8_t block[16] = {0x12, 0x34, 0x05, 0x67, 0xFF, 0xFE, 0x00, 0x03,
                                      0x7F, 0xFF, 0x80, 0x00, 0x00, 0x01, 0xFF, 0xFF};
    static const int16_t want[8] = {0x0567, 0x1234, 3, -2, -0x8000, 0x7FFF, -1, 1};
    unsigned i, zeros = 0, same = 0;
    (void)s;
    audio_set_test_sink(hear);
    audio_set_muted(1);
    audio_push_block(block, sizeof block, 32000);
    for (i = 0; i < 8; i++) zeros += g_heard[i] == 0;
    audio_set_muted(0);
    audio_push_block(block, sizeof block, 32000);
    for (i = 0; i < 8; i++) same += g_heard[i] == want[i];
    audio_set_test_sink(NULL);
    snprintf(got, cap, "muted %u of 8 zero, unmuted %u of 8 the block's, %u frames", zeros, same, g_heard_n);
    return check("unfocused mute zeroes the device", got,
                 "muted 8 of 8 zero, unmuted 8 of 8 the block's, 4 frames");
}

/* The audio DMA across a clock epoch (M19): a deadline left eight blocks
 * behind is caught up one block a poll, and an epoch change -- which brings
 * no backlog of its own, since the clock does not count a gap -- does not
 * throw the owed blocks away: the polls after it keep delivering. */
int dsp_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v);
void dsp_poll(CpuState* s);
uint64_t dsp_dma_blocks(void);
void dsp_dma_backdate(unsigned periods);
void clock_set_speed_at(unsigned speed, uint64_t host_ns);
uint64_t clock_host_ns(void);
uint64_t clock_advance_at(uint64_t host_ns);
unsigned clock_speed(void);

static void quiet(const int16_t* lr, unsigned frames) { (void)lr; (void)frames; }

static int epoch_selftest(CpuState* s, char* got, size_t cap)
{
    uint64_t b0, burst, resync, after;
    int i;
    audio_set_test_sink(quiet);
    /* eight 1 ms blocks behind needs 8 ms of guest time behind it */
    while (clock_advance_at(clock_host_ns()) < 20000000ull) {}
    dsp_write(s, 0xCC005030u, 2, 0x0000);             /* the block's address: RAM low */
    dsp_write(s, 0xCC005032u, 2, 0x4000);
    dsp_write(s, 0xCC005036u, 2, 0x8000u | 0x04u);   /* on: 4 blocks of 32 bytes, 1 ms */
    dsp_dma_backdate(8);
    b0 = dsp_dma_blocks();
    for (i = 0; i < 4; i++) dsp_poll(s);              /* the same epoch: one block a poll */
    burst = dsp_dma_blocks() - b0;
    clock_set_speed_at(clock_speed(), clock_host_ns()); /* an epoch change, as a gap would make */
    b0 = dsp_dma_blocks();
    dsp_poll(s);
    resync = dsp_dma_blocks() - b0;
    b0 = dsp_dma_blocks();
    dsp_poll(s);
    after = dsp_dma_blocks() - b0;
    dsp_write(s, 0xCC005036u, 2, 0x0000);             /* off again */
    audio_set_test_sink(NULL);
    snprintf(got, cap, "same epoch %llu of 4 polls, new epoch %llu then %llu", (unsigned long long)burst,
             (unsigned long long)resync, (unsigned long long)after);
    return check("audio DMA across a clock epoch", got, "same epoch 4 of 4 polls, new epoch 1 then 1");
}

/* The audio DMA's pace: AIInitDMA writes the length with the enable bit kept,
 * once a block, while the engine runs. That write sets the next block and
 * nothing else; taking it as a restart put every block a delivery late and
 * the audio 5% slow (FINDINGS "The AI DMA's pace"). Three blocks owed before
 * the write are all still owed after it. */
static int rearm_selftest(CpuState* s, char* got, size_t cap)
{
    uint64_t b0, owed;
    int i;
    audio_set_test_sink(quiet);
    while (clock_advance_at(clock_host_ns()) < 20000000ull) {}
    dsp_write(s, 0xCC005030u, 2, 0x0000);
    dsp_write(s, 0xCC005032u, 2, 0x4000);
    dsp_write(s, 0xCC005036u, 2, 0x8000u | 0x04u);   /* on: 1 ms blocks, the first due in 1 ms */
    dsp_dma_backdate(3);                              /* two blocks late and a third due */
    dsp_write(s, 0xCC005036u, 2, 0x8000u | 0x04u);   /* AIInitDMA's write, the engine running */
    b0 = dsp_dma_blocks();
    for (i = 0; i < 3; i++) dsp_poll(s);
    owed = dsp_dma_blocks() - b0;
    dsp_write(s, 0xCC005036u, 2, 0x0000);
    audio_set_test_sink(NULL);
    snprintf(got, cap, "%llu of 3 owed blocks played after a running write", (unsigned long long)owed);
    return check("audio DMA keeps its pace through AIInitDMA", got, "3 of 3 owed blocks played after a running write");
}

/* A mod's call into the game (PLAN M4): mod_call_guest on strlen's entry,
 * dispatched like any translated call, with every register set to a pattern
 * first. The length comes back in r3, and every register -- r1, r2, r13, the
 * non-volatile ones, LR, CTR, CR and the GQRs -- is as it was. */
int mod_call_guest(CpuState* s, uint32_t addr, const uint32_t* ints, uint32_t n_ints, const double* floats,
                   uint32_t n_floats, uint32_t* r3, double* f1, char* why, size_t cap);

/* The guest's fused multiply-add rounds once (specs/distribution.md R1, 3.4).
 * The translated 80120A50 is a vector lerp, out = a + t (b - a), each
 * component an fmadds of t, b - a and a. With t = 1 + 2^-30 in f1, a.z =
 * -(1 + 2^-23) and b.z = 0, the exact z is 2^-30 (1 + 2^-23), a float:
 * 0x30800001. Round the product first and the 2^-53 it carries is lost:
 * 0x30800000. Then soa_fma_soft, the software fma a MinGW build falls back to
 * on a CPU without FMA3 (soafma.c), held to this build's own fma over
 * 200,000 triples where one rounding decides: most of a product cancelled,
 * subnormal results, and random bit patterns, infinities and NaNs among them. */
double soa_fma_soft(double x, double y, double z);

static uint64_t fused_rng(uint64_t* x)
{
    *x ^= *x << 13;
    *x ^= *x >> 7;
    *x ^= *x << 17;
    return *x;
}

static double fused_bits(uint64_t v)
{
    double d;
    memcpy(&d, &v, 8);
    return d;
}

static int fused_selftest(CpuState* s, char* got, size_t cap)
{
    const uint32_t OUT = SCRATCH + 0x1900, A = SCRATCH + 0x1910, B = SCRATCH + 0x1920;
    uint64_t x = 0x9E3779B97F4A7C15ull;
    unsigned n, bad = 0;
    int i, fails = 0;
    for (i = 0; i < 3; i++) {
        mem_wf32(s, A + 4 * i, i == 2 ? -(1.0f + 0x1p-23f) : 1.0f);
        mem_wf32(s, B + 4 * i, i == 2 ? 0.0f : 2.0f);
        mem_w32(s, OUT + 4 * i, 0);
    }
    s->gpr[3] = OUT;
    s->gpr[4] = A;
    s->gpr[5] = B;
    s->fpr[1].ps0 = s->fpr[1].ps1 = 1.0 + 0x1p-30;
    call(s, 0x80120A50u);
    snprintf(got, cap, "%08X", mem_r32(s, OUT + 8));
    fails += check("guest fmadds rounds once", got, "30800001");
    for (n = 0; n < 200000; n++) {
        double a, b, c, want, soft;
        uint64_t r = fused_rng(&x);
        if (n % 8 == 0) {
            /* anything at all: subnormals, infinities and NaNs too */
            a = fused_bits(fused_rng(&x));
            b = fused_bits(fused_rng(&x));
            c = fused_bits(fused_rng(&x));
        } else {
            /* moderate magnitudes, the sum mostly cancelling the product,
             * now and then pushed to the subnormal range */
            a = ldexp(1.0 + (double)(r >> 11) * 0x1p-53, (int)(r % 64) - 32);
            b = ldexp(1.0 + (double)(fused_rng(&x) >> 11) * 0x1p-53, (int)(fused_rng(&x) % 64) - 32);
            if (r & 1) a = -a;
            c = -(a * b) * (1.0 + (double)(int)(fused_rng(&x) % 9 - 4) * 0x1p-52);
            if (n % 8 == 1) {
                a = ldexp(a, -520);
                b = ldexp(b, -520);
                c = ldexp(c, -1040);
            }
        }
        want = fma(a, b, c);
        soft = soa_fma_soft(a, b, c);
        if (memcmp(&want, &soft, 8) && !(want != want && soft != soft)) bad++;
    }
    snprintf(got, cap, "%u of %u differ", bad, n);
    fails += check("software fma rounds once", got, "0 of 200000 differ");
    return fails;
}

static int call_guest_selftest(CpuState* s, char* got, size_t cap)
{
    static CpuState before;
    const uint32_t STR = SCRATCH + 0x1800;
    uint32_t arg = STR, r3 = 0;
    double f1 = 0.0;
    char why[160];
    int i, ok, same;
    put_string(s, STR, "skies of arcadia");
    for (i = 0; i < 32; i++) s->gpr[i] = 0xA5000000u + (uint32_t)i;
    s->gpr[1] = STACK_TOP - 0x400;
    s->gpr[2] = SDA2_BASE;
    s->gpr[13] = SDA_BASE;
    for (i = 0; i < 32; i++) { s->fpr[i].ps0 = i + 0.5; s->fpr[i].ps1 = -(double)i; }
    for (i = 0; i < 8; i++) s->gqr[i] = 0x00070007u * (uint32_t)(i + 1);
    s->lr = 0x801DCB88u; s->ctr = 77; s->cr = 0x24000000u; s->xer = 0x20000000u;
    memcpy(&before, s, sizeof before);
    ok = mod_call_guest(s, 0x8025F1D8u, &arg, 1, NULL, 0, &r3, &f1, why, cap < sizeof why ? cap : sizeof why);
    same = !memcmp(s->gpr, before.gpr, sizeof s->gpr) && !memcmp(s->fpr, before.fpr, sizeof s->fpr) &&
           !memcmp(s->gqr, before.gqr, sizeof s->gqr) && s->lr == before.lr && s->ctr == before.ctr &&
           s->cr == before.cr && s->xer == before.xer && s->msr == before.msr && s->fpscr == before.fpscr;
    if (!ok) snprintf(got, cap, "refused: %s", why);
    else if (r3 != 16) snprintf(got, cap, "strlen gave %u", r3);
    else if (!same) snprintf(got, cap, "a register came back changed");
    else snprintf(got, cap, "strlen 16, every register as it was");
    return check("a mod's call into the game", got, "strlen 16, every register as it was");
}

/* ---- the game's table (specs/android.md 3.2) ------------------------------
 * A split build reaches the translated code only through soa_game's table
 * (runtime/game.c); a single-file build calls it directly. Here the table is
 * held to what this build calls: the entry, dispatch and its question, the
 * system files, the record's abi and mode, and every twin the checks above
 * run (config/seam.txt's). In a split build those names are the table's own
 * forwarders, so it asks only that a table is loaded and is this runtime's. */
#include "soa_game.h"
void fn_80003140(CpuState* s);
int dispatch_known(uint32_t addr);

static int game_table_selftest(CpuState* s, char* got, size_t cap)
{
#ifdef SOA_SPLIT
    const SoaGame* g = soa_game_table;
    (void)s;
    snprintf(got, cap, "%s", g && g->abi == SOA_GAME_ABI && g->size >= sizeof(SoaGame) ? "loaded, this abi" : "none");
#else
    extern const uint32_t disc_sys_dol[];
    static const struct {
        uint32_t addr;
        SoaFn fn;
    } twins[] = {
        {0x80005434u, recomp_fn_80005434}, {0x80005520u, recomp_fn_80005520}, {0x801EF7E0u, fn_801EF7E0},
        {0x80232E38u, recomp_fn_80232E38}, {0x80232E64u, recomp_fn_80232E64}, {0x80232E94u, recomp_fn_80232E94},
        {0x80232EC4u, recomp_fn_80232EC4}, {0x80232EF0u, recomp_fn_80232EF0}, {0x8023851Cu, fn_8023851C},
        {0x8023F704u, recomp_fn_8023F704}, {0x8025C710u, recomp_fn_8025C710}, {0x8025C73Cu, recomp_fn_8025C73C},
        {0x8025ECBCu, fn_8025ECBC},        {0x8025ED74u, recomp_fn_8025ED74}, {0x8025EF18u, recomp_fn_8025EF18},
        {0x8025EF48u, recomp_fn_8025EF48}, {0x8025EF88u, recomp_fn_8025EF88}, {0x8025F0B0u, recomp_fn_8025F0B0},
        {0x8025F0DCu, recomp_fn_8025F0DC}, {0x8025F120u, recomp_fn_8025F120}, {0x8025F1D8u, recomp_fn_8025F1D8},
    };
#ifdef SOA_NO_DECOMP
    const char* mode = "mode=no-decomp";
#else
    const char* mode = "mode=decomp";
#endif
    const SoaGame* g = soa_game();
    unsigned i, n = (unsigned)(sizeof twins / sizeof twins[0]);
    (void)s;
    snprintf(got, cap, "%u twins, entry, dispatch, files, record", n);
    if (g->abi != SOA_GAME_ABI || g->size != sizeof(SoaGame)) snprintf(got, cap, "abi %u, size %u", g->abi, g->size);
    else if (g->entry != fn_80003140) snprintf(got, cap, "the entry is not fn_80003140");
    else if (g->dispatch != dispatch || g->dispatch_known != dispatch_known) snprintf(got, cap, "dispatch differs");
    else if (g->dol != disc_sys_dol) snprintf(got, cap, "the system files differ");
    else if (!strstr(g->record, "abi=1 ") || !strstr(g->record, mode)) snprintf(got, cap, "record \"%s\"", g->record);
    else
        for (i = 0; i < n; i++)
            if (g->twin(twins[i].addr) != twins[i].fn) {
                snprintf(got, cap, "twin %08X differs", twins[i].addr);
                break;
            }
#endif
    return check("the game's table", got,
#ifdef SOA_SPLIT
                 "loaded, this abi"
#else
                 "21 twins, entry, dispatch, files, record"
#endif
    );
}

/* ---- any function swapped (runtime/swap.c, docs/SWAP.md) -------------------
 * sprintf (0x8025CB24), translated in every build, answered by a replacement
 * in each of the ways one can: not at all, in its place, after it, and around
 * its original run in place; counted; and two addresses refused, one inside a
 * function and one the runtime answers itself (VIGetRetraceCount). */
#include "swap.h"
static int g_swap_mode;
static uint32_t g_swap_buf;

static int swap_test_handler(CpuState* s, uint32_t address, SwapFn fn, void* user, int after)
{
    (void)fn;
    (void)user;
    if (!after) g_swap_buf = s->gpr[3];
    switch (g_swap_mode) {
    case 1:
        put_string(s, s->gpr[3], "native");
        s->gpr[3] = 6;
        return SWAP_ANSWERED;
    case 2:
        if (after) mem_w8(s, g_swap_buf, (uint8_t)(mem_r8(s, g_swap_buf) - 32));
        return SWAP_BOTH;
    case 3:
        swap_original(s, address);
        mem_w8(s, g_swap_buf + 1, (uint8_t)(mem_r8(s, g_swap_buf + 1) - 32));
        return SWAP_ANSWERED;
    default:
        return SWAP_GAMES;
    }
}

static void swap_sprintf(CpuState* s, int mode, char* out, size_t cap)
{
    g_swap_mode = mode;
    put_string(s, SCRATCH, "");
    put_string(s, SCRATCH + 0x100, "%s.%s");
    put_string(s, SCRATCH + 0x200, "abc");
    put_string(s, SCRATCH + 0x300, "def");
    s->gpr[3] = SCRATCH;
    s->gpr[4] = SCRATCH + 0x100;
    s->gpr[5] = SCRATCH + 0x200;
    s->gpr[6] = SCRATCH + 0x300;
    call(s, 0x8025CB24u);
    get_string(s, SCRATCH, out, cap);
}

static int swap_selftest(CpuState* s, char* got, size_t cap)
{
    const uint32_t SPRINTF = 0x8025CB24u;
    char a[32], g[32], b[32], o[32];
    uint32_t ra, rb;
    SwapCount c[256];
    unsigned n, k, refused = 0;
    uint64_t calls = 0, answered = 0, translated = 0;
    if (swap_index(SPRINTF) < 0) {
        snprintf(got, cap, "no swap table (%d)", swap_index(SPRINTF));
        return check("any function swapped", got, "answered native/6, games abc.def, both Abc.def/7, original aBc.def, "
                                                  "counted 2 1 1, refused 2");
    }
    swap_set(SPRINTF, swap_test_handler, NULL, NULL);
    swap_count(1);
    swap_sprintf(s, 1, a, sizeof a);
    ra = s->gpr[3];
    swap_sprintf(s, 0, g, sizeof g);
    swap_count(0);
    swap_sprintf(s, 2, b, sizeof b);
    rb = s->gpr[3];
    swap_sprintf(s, 3, o, sizeof o);
    swap_set(SPRINTF, NULL, NULL, NULL);
    n = swap_counts(c, 256);
    for (k = 0; k < n && k < 256; k++)
        if (c[k].address == SPRINTF) calls = c[k].calls, answered = c[k].answered, translated = c[k].translated;
    refused += !swap_set(SPRINTF + 4, swap_test_handler, NULL, NULL);
    refused += !swap_set(0x8023F704u, swap_test_handler, NULL, NULL);
    snprintf(got, cap, "answered %s/%u, games %s, both %s/%u, original %s, counted %llu %llu %llu, refused %u", a, ra, g,
             b, rb, o, (unsigned long long)calls, (unsigned long long)answered, (unsigned long long)translated, refused);
    return check("any function swapped", got,
                 "answered native/6, games abc.def, both Abc.def/7, original aBc.def, counted 2 1 1, refused 2");
}

int selftest(CpuState* s)
{
    char got[256];
    int failures = 0;

    /* First, because it shapes a card image before anything reads SRAM. */
    failures += card_selftest(s, got, sizeof got);

    /* sprintf(buf, "%s.%s", "abc", "def") -- 0x8025CB24 */
    put_string(s, SCRATCH + 0x100, "%s.%s");
    put_string(s, SCRATCH + 0x200, "abc");
    put_string(s, SCRATCH + 0x300, "def");
    s->gpr[3] = SCRATCH;
    s->gpr[4] = SCRATCH + 0x100;
    s->gpr[5] = SCRATCH + 0x200;
    s->gpr[6] = SCRATCH + 0x300;
    call(s, 0x8025CB24u);
    get_string(s, SCRATCH, got, sizeof got);
    failures += check("sprintf %s.%s", got, "abc.def");

    /* sprintf(buf, "%d|%5u|%-4x|%c|%08X", -42, 7, 255, 'Q', 0xBEEF) */
    put_string(s, SCRATCH + 0x100, "%d|%5u|%-4x|%c|%08X");
    s->gpr[3] = SCRATCH;
    s->gpr[4] = SCRATCH + 0x100;
    s->gpr[5] = (uint32_t)-42;
    s->gpr[6] = 7;
    s->gpr[7] = 255;
    s->gpr[8] = 'Q';
    s->gpr[9] = 0xBEEF;
    call(s, 0x8025CB24u);
    get_string(s, SCRATCH, got, sizeof got);
    failures += check("sprintf integers", got, "-42|    7|ff  |Q|0000BEEF");

    /* sprintf(buf, "%s/%s%03d.%s", "title", "ts", 26, "mld") -- more than 4 args */
    put_string(s, SCRATCH + 0x100, "%s/%s%03d.%s");
    put_string(s, SCRATCH + 0x200, "title");
    put_string(s, SCRATCH + 0x300, "ts");
    put_string(s, SCRATCH + 0x400, "mld");
    s->gpr[3] = SCRATCH;
    s->gpr[4] = SCRATCH + 0x100;
    s->gpr[5] = SCRATCH + 0x200;
    s->gpr[6] = SCRATCH + 0x300;
    s->gpr[7] = 26;
    s->gpr[8] = SCRATCH + 0x400;
    call(s, 0x8025CB24u);
    get_string(s, SCRATCH, got, sizeof got);
    failures += check("sprintf mixed", got, "title/ts026.mld");

    /* sprintf(buf, "%.2f|%g|%5.1f", 3.14159, 2.5, -0.75): the FPU and float varargs */
    put_string(s, SCRATCH + 0x100, "%.2f|%g|%5.1f");
    s->gpr[3] = SCRATCH;
    s->gpr[4] = SCRATCH + 0x100;
    s->fpr[1].ps0 = 3.14159; s->fpr[2].ps0 = 2.5; s->fpr[3].ps0 = -0.75;
    s->gpr[1] = STACK_TOP - 0x400; s->gpr[2] = SDA2_BASE; s->gpr[13] = SDA_BASE; s->lr = 0; s->msr = 0x00002030u;
    s->cr = 0x02000000u; /* CR bit 6: floating-point arguments present */
    dispatch(s, 0x8025CB24u);
    get_string(s, SCRATCH, got, sizeof got);
    failures += check("sprintf floats", got, "3.14|2.5| -0.8");

    /* strcpy / strcat / strcmp / strlen / memset / memcpy */
    put_string(s, SCRATCH + 0x200, "alpha");
    put_string(s, SCRATCH + 0x300, "beta");
    s->gpr[3] = SCRATCH; s->gpr[4] = SCRATCH + 0x200;
    call(s, 0x8025F120u); /* strcpy */
    s->gpr[3] = SCRATCH; s->gpr[4] = SCRATCH + 0x300;
    run_twin(s, recomp_fn_8025F0B0); /* strcat */
    get_string(s, SCRATCH, got, sizeof got);
    failures += check("strcpy+strcat", got, "alphabeta");
    s->gpr[3] = SCRATCH;
    run_twin(s, recomp_fn_8025F1D8); /* strlen */
    snprintf(got, sizeof got, "%u", s->gpr[3]);
    failures += check("strlen", got, "9");
    s->gpr[3] = SCRATCH; s->gpr[4] = SCRATCH + 0x200;
    call(s, 0x8025EF88u); /* strcmp("alphabeta", "alpha") > 0 */
    snprintf(got, sizeof got, "%s", (int32_t)s->gpr[3] > 0 ? "positive" : "other");
    failures += check("strcmp", got, "positive");
    s->gpr[3] = SCRATCH + 0x400; s->gpr[4] = 0x41; s->gpr[5] = 37;
    run_twin(s, recomp_fn_80005434); /* memset 37 bytes of 'A' */
    mem_w8(s, SCRATCH + 0x400 + 37, 0);
    get_string(s, SCRATCH + 0x400, got, sizeof got);
    failures += check("memset", got, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    s->gpr[3] = SCRATCH + 0x500 + 3; s->gpr[4] = SCRATCH; s->gpr[5] = 10; /* unaligned memcpy incl. the NUL */
    run_twin(s, recomp_fn_80005520);
    get_string(s, SCRATCH + 0x503, got, sizeof got);
    failures += check("memcpy unaligned", got, "alphabeta");

    /* ARAM DMA: the SDK's ARStartDMA writes the direction into CNT_H, reads CNT_H back to
     * merge the length's high bits, then writes CNT_L. The direction must survive that. */
    {
        uint64_t v = 0;
        uint8_t* ar = aram_memory();
        memset(ar + 0x10000, 0x5A, 64);
        mem_w32(s, SCRATCH + 0x600, 0);
        device_write(s, 0xCC005020u, 2, (SCRATCH + 0x600) >> 16);
        device_write(s, 0xCC005022u, 2, (SCRATCH + 0x600) & 0xFFFFu);
        device_write(s, 0xCC005024u, 2, 0x10000u >> 16);
        device_write(s, 0xCC005026u, 2, 0);
        device_write(s, 0xCC005028u, 2, 0x8000u); /* ARAM to main memory */
        device_read(s, 0xCC005028u, 2, &v);
        device_write(s, 0xCC005028u, 2, (v & ~0x3FFull) | 0u);
        device_write(s, 0xCC00502Au, 2, 64);
        snprintf(got, sizeof got, "%08X", mem_r32(s, SCRATCH + 0x600));
        failures += check("ARAM DMA to main memory", got, "5A5A5A5A");
        mem_w32(s, SCRATCH + 0x600, 0xC0FFEE11u);
        device_write(s, 0xCC005024u, 2, 0x10040u >> 16);
        device_write(s, 0xCC005026u, 2, 0x10040u & 0xFFFFu);
        device_write(s, 0xCC005028u, 2, 0); /* main memory to ARAM */
        device_read(s, 0xCC005028u, 2, &v);
        device_write(s, 0xCC005028u, 2, (v & ~0x3FFull) | 0u);
        device_write(s, 0xCC00502Au, 2, 64);
        snprintf(got, sizeof got, "%02X%02X%02X%02X", ar[0x10040], ar[0x10041], ar[0x10042], ar[0x10043]);
        failures += check("ARAM DMA from main memory", got, "C0FFEE11");
    }

    failures += ax_selftest(s, got, sizeof got);
    failures += render_selftest(s, got, sizeof got);
    failures += decomp_selftest(s, got, sizeof got);
    failures += tick_selftest(s, got, sizeof got);
    failures += dcache_selftest(s, got, sizeof got);
    failures += psq_selftest(s, got, sizeof got);
    failures += seed_selftest(s, got, sizeof got);
    failures += encounter_selftest(s, got, sizeof got);
    failures += motor_selftest(s, got, sizeof got);
    failures += mute_selftest(s, got, sizeof got);
    failures += epoch_selftest(s, got, sizeof got);
    failures += rearm_selftest(s, got, sizeof got);
    failures += call_guest_selftest(s, got, sizeof got);
    failures += fused_selftest(s, got, sizeof got);
    failures += game_table_selftest(s, got, sizeof got);
    failures += swap_selftest(s, got, sizeof got);

    fprintf(stderr, "[selftest] %d failure(s)\n", failures);
    return failures;
}
