/*
 * The stock AX microcode, in C: what the DSP does with each command list
 * the game's audio driver mails it every 5 ms. Voices are parameter blocks
 * (AXPB, 0xC0 bytes, linked through their first word) whose samples live
 * in ARAM as DSP-ADPCM, PCM16 or PCM8; each frame every running voice is
 * decoded, rate-converted to 32 kHz, shaped by its volume envelope and
 * mixed by its mixer gains into the main and two auxiliary buses. The aux
 * buses can be handed to the CPU for effects and mixed back; the main bus
 * is written as 160 stereo samples where the AI DMA reads them.
 *
 * Layouts follow the AX headers of the SDK this game links (2002); the
 * command numbers are those Dolphin's AX HLE documents for this microcode.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_SAMPLES 160 /* 5 ms at 32 kHz */
#define MS_SAMPLES 32

uint8_t* aram_memory(void);
const char* aram_source_name(uint32_t byte_addr, uint32_t* a0, uint32_t* a1);

/* ---- parameter block: word offsets (AXPB, 96 words) ----------------------- */

enum {
    PB_NEXT_HI = 0, PB_NEXT_LO, PB_THIS_HI, PB_THIS_LO,
    PB_SRC_TYPE = 4, PB_COEF_SELECT, PB_MIXER_CTRL, PB_RUNNING, PB_IS_STREAM,
    PB_MIXER = 9,        /* 18: L dL R dR AL dAL AR dAR BL dBL BR dBR S dS AS dAS BS dBS */
    PB_ITD = 27,         /* 7 */
    PB_UPDATES = 34,     /* 7: num_updates[5], data hi, data lo */
    PB_DPOP = 41,        /* 9 */
    PB_VOL_ENV = 50,     /* 2: cur_volume, delta */
    PB_UNK3 = 52,        /* 3 */
    PB_AUDIO_ADDR = 55,  /* 8: looping, format, loop hi/lo, end hi/lo, cur hi/lo */
    PB_ADPCM = 63,       /* 20: coefs[16], gain, pred_scale, yn1, yn2 */
    PB_SRC = 83,         /* 7: ratio hi/lo, cur_addr_frac, last_samples[4] */
    PB_ADPCM_LOOP = 90,  /* 3: pred_scale, yn1, yn2 */
    PB_WORDS = 96
};

enum { MX_L = 0, MX_DL, MX_R, MX_DR, MX_AL, MX_DAL, MX_AR, MX_DAR, MX_BL, MX_DBL, MX_BR, MX_DBR, MX_S, MX_DS, MX_AS, MX_DAS, MX_BS, MX_DBS };

/* mixer_control bits of this microcode */
#define MIX_L 0x0001
#define MIX_R 0x0002
#define MIX_S 0x0004
#define MIX_RAMP 0x0008
#define MIX_AL 0x0010
#define MIX_AR 0x0020
#define MIX_AS 0x0040
#define MIX_A_RAMP 0x0080
#define MIX_BL 0x0100
#define MIX_BR 0x0200
#define MIX_BS 0x0400
#define MIX_B_RAMP 0x0800

/* The bits the driver actually writes into mixer_control, which are not the
 * MIX_* above: those are this mixer's own decoded sends. A per-studio mode
 * word (studio+84) chooses, for every voice and for the whole frame at once,
 * which of two encodings auxiliary B arrives under -- bit 1 alongside command
 * 0x05, or bit 4 alongside command 0x10 (fn_8027CDF4 tests that word at
 * 0x8027D92C for the bit and at 0x8027E848 for the command). No voice can
 * carry both, so the order the two are decoded in cannot matter. */
#define RAW_AUXA 0x0001
#define RAW_AUXB 0x0002
#define RAW_SURROUND 0x0004
#define RAW_RAMP 0x0008
#define RAW_AUXB_LR 0x0010
#define RAW_AUXB_ANY (RAW_AUXB | RAW_AUXB_LR)

typedef struct {
    uint16_t w[PB_WORDS];
} PB;

static uint32_t rd32(CpuState* s, uint32_t a) { return mem_r32(s, a | 0x80000000u); }
static uint16_t rd16(CpuState* s, uint32_t a) { return mem_r16(s, a | 0x80000000u); }
static void wr16(CpuState* s, uint32_t a, uint16_t v) { mem_w16(s, a | 0x80000000u, v); }

static void pb_read(CpuState* s, uint32_t addr, PB* pb)
{
    int i;
    for (i = 0; i < PB_WORDS; i++) pb->w[i] = rd16(s, addr + 2u * (uint32_t)i);
}

static void pb_write(CpuState* s, uint32_t addr, const PB* pb)
{
    int i;
    for (i = 0; i < PB_WORDS; i++) wr16(s, addr + 2u * (uint32_t)i, pb->w[i]);
}

/* ---- buses ------------------------------------------------------------ */

static int32_t g_main[3][FRAME_SAMPLES];  /* L R S */
static int32_t g_auxa[3][FRAME_SAMPLES];
static int32_t g_auxb[3][FRAME_SAMPLES];
static uint64_t g_frames, g_voices, g_samples_out;
static int g_verbose = -1;

/* ---- census (PLAN E1) -----------------------------------------------------
 *
 * Six opcodes here are parsed for their length only, two are ignored, and ten
 * of the parameter block's named fields are never read, so every argument
 * about what to implement next rests on a guess about what the driver sends.
 * This counts it instead: opcodes, the fields a block is ever given a value
 * in, whether anything reaches the auxiliary buses or comes back from the
 * CPU's effects pass, and what a starting voice's samples were uploaded from.
 *
 * Cost, since the mixer runs every 5 ms: the opcode count is one increment per
 * command, the field census about thirty compares on a block already in a
 * local, and the start test one hash probe -- all of it lost in the noise of
 * decoding 160 samples a voice. The two that are not free are paid for only
 * when they can say something: an auxiliary bus is scanned for its peak only
 * if a voice mixed into it this frame (the peak of a bus nothing touched is
 * zero, and this game touches neither), and a source name is resolved only at
 * a start, then cached on the block for the frames that follow.
 */
#define CENSUS_OPS 20

/* Four distinct values is enough to answer "is this field ever anything but
 * its default", and keeps the report to one line per field. */
typedef struct {
    uint16_t v[4];
    uint64_t n[4];
    uint64_t other;
} Field;

static uint64_t g_cmd_n[CENSUS_OPS + 1], g_cmd_bad, g_lists_full;
static uint16_t g_cmd_bad_op;
static unsigned g_cmds_hi, g_chain_hi;
static uint16_t g_dl_aux_a, g_dl_aux_b; /* the two send levels command 0x01 carries and we drop */
static uint64_t g_dl_aux_n;

static Field g_f_src_type, g_f_coef_sel, g_f_is_stream, g_f_ctrl, g_f_format, g_f_gain, g_f_upd;
static uint64_t g_f_itd, g_f_itd_sh, g_f_dpop, g_f_unk3, g_f_this, g_f_noloop, g_f_noupd;
static uint16_t g_ctrl_or;
static uint64_t g_ctrl_aux, g_ctrl_b10;

static int g_auxa_live, g_auxb_live;
static int32_t g_auxa_peak, g_auxb_peak, g_ret_a, g_ret_b, g_ret_nw, g_ret_lr;
static uint64_t g_auxa_frames, g_auxb_frames;

static uint64_t g_starts, g_repoints, g_start_ms[5], g_stop_ms[5];

#define VSLOTS 1024
static struct {
    uint32_t addr, tag;
    int16_t srci;
    uint8_t used, running;
} g_vs[VSLOTS];

#define SRCSLOTS 64
static struct {
    const char* name;
    uint32_t a0, a1;
    uint64_t starts, frames;
} g_srcs[SRCSLOTS];
static int g_nsrcs;
static uint64_t g_srcs_lost;

static void field_note(Field* f, uint16_t v)
{
    int i;
    for (i = 0; i < 4; i++) {
        if (!f->n[i]) {
            f->v[i] = v;
            f->n[i] = 1;
            return;
        }
        if (f->v[i] == v) {
            f->n[i]++;
            return;
        }
    }
    f->other++;
}

/* Which of the driver's per-millisecond writes land where, and when a note-on
 * arrives after millisecond 0 -- which this mixer's loop cannot act on. */
static void census_update(uint16_t off, uint16_t val, uint16_t was, int ms)
{
    field_note(&g_f_upd, off);
    if (off != PB_RUNNING) return;
    if (val && !was) g_start_ms[ms]++;
    else if (!val && was) g_stop_ms[ms]++;
}

static uint32_t voice_byte_addr(const PB* pb)
{
    uint32_t cur = ((uint32_t)pb->w[PB_AUDIO_ADDR + 6] << 16) | pb->w[PB_AUDIO_ADDR + 7];
    uint16_t fmt = pb->w[PB_AUDIO_ADDR + 1];
    return fmt == 0x00 ? cur >> 1 : (fmt == 0x0A ? cur * 2 : cur);
}

static int census_source(const PB* pb)
{
    uint32_t a0 = 0, a1 = 0;
    const char* name = aram_source_name(voice_byte_addr(pb), &a0, &a1);
    int i;
    for (i = 0; i < g_nsrcs; i++)
        if (g_srcs[i].name == name && g_srcs[i].a0 == a0) return i;
    if (g_nsrcs == SRCSLOTS) {
        g_srcs_lost++;
        return -1;
    }
    g_srcs[g_nsrcs].name = name;
    g_srcs[g_nsrcs].a0 = a0;
    g_srcs[g_nsrcs].a1 = a1;
    return g_nsrcs++;
}

/* A start is a block that was not running and now is, or one that stayed
 * running but was pointed at different samples: the driver sets RUNNING
 * directly as well as through the update list, so the update list alone
 * cannot see every note-on. */
static void census_voice(uint32_t addr, const PB* pb)
{
    unsigned h = (addr >> 5) % VSLOTS, n;
    uint32_t loop = ((uint32_t)pb->w[PB_AUDIO_ADDR + 2] << 16) | pb->w[PB_AUDIO_ADDR + 3];
    uint32_t end = ((uint32_t)pb->w[PB_AUDIO_ADDR + 4] << 16) | pb->w[PB_AUDIO_ADDR + 5];
    uint32_t tag = loop * 2654435761u + end * 40503u + pb->w[PB_AUDIO_ADDR + 1] + 1u;
    int i;

    for (n = 0; n < VSLOTS; n++) {
        if (!g_vs[h].used || g_vs[h].addr == addr) break;
        h = (h + 1) % VSLOTS;
    }
    if (n == VSLOTS) return; /* more live blocks than slots: count nothing rather than count wrong */
    if (!g_vs[h].used) {
        g_vs[h].used = 1;
        g_vs[h].addr = addr;
        g_vs[h].srci = -1;
    }
    if (!pb->w[PB_RUNNING]) {
        g_vs[h].running = 0;
        g_vs[h].tag = tag;
        return;
    }
    if (!g_vs[h].running || g_vs[h].tag != tag) {
        if (g_vs[h].running) g_repoints++;
        else g_starts++;
        g_vs[h].srci = (int16_t)census_source(pb);
        if (g_vs[h].srci >= 0) g_srcs[g_vs[h].srci].starts++;
    }
    g_vs[h].running = 1;
    g_vs[h].tag = tag;
    if (g_vs[h].srci >= 0) g_srcs[g_vs[h].srci].frames++;

    field_note(&g_f_src_type, pb->w[PB_SRC_TYPE]);
    field_note(&g_f_coef_sel, pb->w[PB_COEF_SELECT]);
    field_note(&g_f_is_stream, pb->w[PB_IS_STREAM]);
    field_note(&g_f_ctrl, pb->w[PB_MIXER_CTRL]);
    field_note(&g_f_format, pb->w[PB_AUDIO_ADDR + 1]);
    field_note(&g_f_gain, pb->w[PB_ADPCM + 16]);
    g_ctrl_or |= pb->w[PB_MIXER_CTRL];
    if (pb->w[PB_MIXER_CTRL] & (RAW_AUXA | RAW_AUXB_ANY)) g_ctrl_aux++;
    if (pb->w[PB_MIXER_CTRL] & RAW_AUXB_LR) g_ctrl_b10++;
    if (pb->w[PB_ITD]) g_f_itd++; /* the enable; the two words after it are the allocator's buffer */
    for (i = 3; i < 7; i++)
        if (pb->w[PB_ITD + i]) { g_f_itd_sh++; break; }
    for (i = 0; i < 9; i++)
        if (pb->w[PB_DPOP + i]) { g_f_dpop++; break; }
    for (i = 0; i < 3; i++)
        if (pb->w[PB_UNK3 + i]) { g_f_unk3++; break; }
    if (((((uint32_t)pb->w[PB_THIS_HI] << 16) | pb->w[PB_THIS_LO]) & 0x7FFFFFFFu) !=
        (addr & 0x7FFFFFFFu))
        g_f_this++;
    if (!pb->w[PB_AUDIO_ADDR]) g_f_noloop++;
    if (!(((uint32_t)pb->w[PB_UPDATES + 5] << 16) | pb->w[PB_UPDATES + 6])) g_f_noupd++;
}

static void census_field(char* out, size_t cap, const char* name, const Field* f, int dec)
{
    size_t at = (size_t)snprintf(out, cap, "%s", name);
    int i;
    for (i = 0; i < 4 && f->n[i] && at + 40 < cap; i++)
        at += (size_t)snprintf(out + at, cap - at, dec ? " %u:%llu" : " %X:%llu", f->v[i],
                               (unsigned long long)f->n[i]);
    if (f->other && at + 40 < cap)
        snprintf(out + at, cap - at, " +%llu more", (unsigned long long)f->other);
}

static void census_report(void)
{
    char sent[320] = "", never[96] = "", a[96], b[96], c[96], d[96], e[96], g[96], line[1024];
    size_t at = 0, nt = 0;
    int i, shown;

    for (i = 0; i < CENSUS_OPS; i++) {
        if (g_cmd_n[i] && at + 32 < sizeof sent)
            at += (size_t)snprintf(sent + at, sizeof sent - at, "%s%02X:%llu", at ? " " : "", i,
                                   (unsigned long long)g_cmd_n[i]);
        else if (!g_cmd_n[i] && nt + 8 < sizeof never)
            nt += (size_t)snprintf(never + nt, sizeof never - nt, "%s%02X", nt ? " " : "", i);
    }
    if (g_cmd_n[CENSUS_OPS] && at + 32 < sizeof sent)
        snprintf(sent + at, sizeof sent - at, " >13:%llu", (unsigned long long)g_cmd_n[CENSUS_OPS]);
    fprintf(stderr,
            "[ax] census opcodes sent %s; never sent %s; %llu lists abandoned (first bad op %u), "
            "%llu hit the 64-command cap; longest list %u commands, longest voice chain %u\n",
            sent, never, (unsigned long long)g_cmd_bad, g_cmd_bad_op,
            (unsigned long long)g_lists_full, g_cmds_hi, g_chain_hi);

    census_field(a, sizeof a, "src_type", &g_f_src_type, 1);
    census_field(b, sizeof b, "coef_select", &g_f_coef_sel, 1);
    census_field(c, sizeof c, "is_stream", &g_f_is_stream, 1);
    census_field(d, sizeof d, "mixer_ctrl", &g_f_ctrl, 0);
    census_field(e, sizeof e, "format", &g_f_format, 0);
    census_field(g, sizeof g, "adpcm_gain", &g_f_gain, 0);
    fprintf(stderr, "[ax] census PB values over %llu voice-frames: %s | %s | %s | %s | %s | %s\n",
            (unsigned long long)g_voices, a, b, c, d, e, g);
    fprintf(stderr,
            "[ax] census PB set: itd on %llu (shifts %llu), dpop %llu, unk3 %llu, this!=own %llu, "
            "one-shot %llu, no update list %llu; mixer_ctrl bits seen %04X (aux send %llu, bit "
            "0x10 %llu); cmd 01 dropped sends max %u/%u in %llu\n",
            (unsigned long long)g_f_itd, (unsigned long long)g_f_itd_sh,
            (unsigned long long)g_f_dpop, (unsigned long long)g_f_unk3,
            (unsigned long long)g_f_this, (unsigned long long)g_f_noloop,
            (unsigned long long)g_f_noupd, g_ctrl_or, (unsigned long long)g_ctrl_aux,
            (unsigned long long)g_ctrl_b10, g_dl_aux_a, g_dl_aux_b,
            (unsigned long long)g_dl_aux_n);
    fprintf(stderr,
            "[ax] census aux: A reached by a voice in %llu frames (peak %d), B in %llu (peak %d); "
            "back from the CPU: auxA %d auxB %d nowrite %d set-LR %d\n",
            (unsigned long long)g_auxa_frames, g_auxa_peak, (unsigned long long)g_auxb_frames,
            g_auxb_peak, g_ret_a, g_ret_b, g_ret_nw, g_ret_lr);
    census_field(a, sizeof a, "", &g_f_upd, 1);
    fprintf(stderr,
            "[ax] census starts: %llu (+%llu re-points); RUNNING through the update list by ms "
            "on %llu/%llu/%llu/%llu/%llu off %llu/%llu/%llu/%llu/%llu; update writes by PB word%s\n",
            (unsigned long long)g_starts, (unsigned long long)g_repoints,
            (unsigned long long)g_start_ms[0], (unsigned long long)g_start_ms[1],
            (unsigned long long)g_start_ms[2], (unsigned long long)g_start_ms[3],
            (unsigned long long)g_start_ms[4], (unsigned long long)g_stop_ms[0],
            (unsigned long long)g_stop_ms[1], (unsigned long long)g_stop_ms[2],
            (unsigned long long)g_stop_ms[3], (unsigned long long)g_stop_ms[4], a);

    at = 0;
    for (shown = 0; shown < 10; shown++) {
        int best = -1;
        for (i = 0; i < g_nsrcs; i++)
            if (g_srcs[i].starts && (best < 0 || g_srcs[i].starts > g_srcs[best].starts)) best = i;
        if (best < 0) break;
        if (at + 72 < sizeof line)
            at += (size_t)snprintf(line + at, sizeof line - at, "%s%s %llu starts %llu frames %06X-%06X",
                                   at ? " | " : "", g_srcs[best].name,
                                   (unsigned long long)g_srcs[best].starts,
                                   (unsigned long long)g_srcs[best].frames, g_srcs[best].a0,
                                   g_srcs[best].a1);
        g_srcs[best].starts = 0; /* printing the table is the last thing it is for */
    }
    if (!at) snprintf(line, sizeof line, "none");
    fprintf(stderr, "[ax] census starts by source (%d distinct runs, %llu unslotted): %s\n", g_nsrcs,
            (unsigned long long)g_srcs_lost, line);
}

/* ---- sample fetch ------------------------------------------------------- */

typedef struct {
    PB* pb;
    uint32_t cur;        /* current address in the format's units (nibbles / samples / bytes) */
    uint32_t end, loop;
    int fmt, looping;
    int16_t yn1, yn2;
    int pred_scale;
    const int16_t* coefs;
    uint8_t* aram;
    int stopped;
} Voice;

static int16_t clamp16(int32_t v) { return (int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v)); }

#define ARAM_MASK ((16u << 20) - 1)

/* One sample at the voice's current address, advancing it and following the loop. */
static int16_t fetch_sample(Voice* v)
{
    int16_t out = 0;
    if (v->stopped) return 0;
    if (v->cur >= v->end) {
        if (v->looping) {
            v->cur = v->loop;
            if (v->fmt == 0) {
                v->pred_scale = v->pb->w[PB_ADPCM_LOOP];
                v->yn1 = (int16_t)v->pb->w[PB_ADPCM_LOOP + 1];
                v->yn2 = (int16_t)v->pb->w[PB_ADPCM_LOOP + 2];
            }
        } else {
            v->stopped = 1;
            return 0;
        }
    }
    switch (v->fmt) {
    case 0x00: { /* DSP-ADPCM: nibble addresses, 16 nibbles per frame, the first two the header */
        uint32_t nib = v->cur;
        if ((nib & 15) == 0) { v->pred_scale = v->aram[(nib / 2) & ARAM_MASK]; nib += 2; }
        {
            uint8_t byte = v->aram[(nib / 2) & ARAM_MASK];
            int n = (nib & 1) ? (byte & 15) : (byte >> 4);
            int scale = 1 << (v->pred_scale & 15);
            int ci = (v->pred_scale >> 4) & 7;
            int32_t c1 = v->coefs[ci * 2], c2 = v->coefs[ci * 2 + 1];
            int32_t nibble = n >= 8 ? n - 16 : n;
            int32_t val = (scale * nibble * 2048 + c1 * v->yn1 + c2 * v->yn2 + 1024) >> 11;
            out = clamp16(val);
            v->yn2 = v->yn1;
            v->yn1 = out;
        }
        v->cur = nib + 1;
        break;
    }
    case 0x0A: { /* PCM16, sample addresses */
        uint32_t b = (v->cur * 2) & ARAM_MASK;
        out = (int16_t)(((uint16_t)v->aram[b] << 8) | v->aram[b + 1]);
        v->cur++;
        break;
    }
    case 0x19: /* PCM8, byte addresses */
        out = (int16_t)((int8_t)v->aram[v->cur & ARAM_MASK] << 8);
        v->cur++;
        break;
    default:
        v->cur++;
        break;
    }
    return out;
}

/* ---- voice processing ---------------------------------------------------- */

static void mix_add(int32_t* bus, const int32_t* in, int n, uint16_t* gain, int ramp, int16_t delta)
{
    int i;
    int32_t vol = *gain;
    for (i = 0; i < n; i++) {
        bus[i] += (in[i] * vol) >> 15;
        if (ramp) vol += delta;
    }
    if (ramp) *gain = (uint16_t)vol;
}

/* The driver's per-millisecond parameter changes: (offset, value) pairs. */
static void apply_updates(CpuState* s, PB* pb, int ms)
{
    uint32_t data = ((uint32_t)pb->w[PB_UPDATES + 5] << 16) | pb->w[PB_UPDATES + 6];
    unsigned start = 0, n = pb->w[PB_UPDATES + ms], i;
    int k;
    if (!data || !n) return;
    for (k = 0; k < ms; k++) start += pb->w[PB_UPDATES + k];
    data &= 0x7FFFFFFFu;
    for (i = start; i < start + n && i < 4096; i++) {
        uint16_t off = rd16(s, data + i * 4u), val = rd16(s, data + i * 4u + 2u);
        if (off < PB_WORDS) {
            census_update(off, val, pb->w[off], ms);
            pb->w[off] = val;
        }
    }
}

static int g_starts_at_tick = -1; /* SOA_AX_STARTS=tick: a voice starts only at a tick's beginning, as before */

static void process_voice(CpuState* s, uint32_t addr, uint8_t* aram)
{
    PB pb;
    Voice v;
    int32_t samples[MS_SAMPLES];
    int ms, i, counted = 0;
    uint32_t ratio, frac;
    int16_t last[4];

    /* A block is gone through a millisecond at a time whether or not it is
     * running: the driver starts a voice either by setting "running" before
     * the tick or by listing "running = 1" among the changes for one of the
     * four milliseconds after the first, and stops one the same two ways.
     * Until 2026-10-10 this returned when the block was not running after the
     * first millisecond's changes, and left the loop when a voice stopped, so
     * a voice started inside a tick never ran at all: about five of every six
     * the game starts (2,990 of 3,536 over six runs; soa-ue5's
     * docs/audio-voices.md has the count). SOA_AX_STARTS=tick is the old way,
     * to hear the difference by. */
    if (g_starts_at_tick < 0) {
        const char* e = getenv("SOA_AX_STARTS");
        g_starts_at_tick = e && !strcmp(e, "tick") ? 1 : 0;
    }
    pb_read(s, addr, &pb);
    apply_updates(s, &pb, 0);
    census_voice(addr, &pb);
    if (g_starts_at_tick && !pb.w[PB_RUNNING]) { pb_write(s, addr, &pb); return; }

    v.pb = &pb;
    v.aram = aram;
    v.stopped = 0;

    for (ms = 0; ms < 5; ms++) {
        uint16_t ctrl, vol;
        int16_t dvol;
        uint16_t* mx;
        if (ms) apply_updates(s, &pb, ms);
        if (!pb.w[PB_RUNNING]) {
            if (g_starts_at_tick) break;
            continue;
        }
        if (!counted) {
            counted = 1;
            g_voices++;
            if (g_verbose && g_voices <= 6)
                fprintf(stderr, "[ax] voice pb %08X fmt %04X loop %u addr %04X%04X..%04X%04X cur %04X%04X ratio %04X%04X ctrl %04X vol %04X/%d gains L %04X R %04X AL %04X AR %04X updates %u %u %u %u %u\n",
                        addr, pb.w[PB_AUDIO_ADDR + 1], pb.w[PB_AUDIO_ADDR], pb.w[PB_AUDIO_ADDR + 2], pb.w[PB_AUDIO_ADDR + 3],
                        pb.w[PB_AUDIO_ADDR + 4], pb.w[PB_AUDIO_ADDR + 5], pb.w[PB_AUDIO_ADDR + 6], pb.w[PB_AUDIO_ADDR + 7],
                        pb.w[PB_SRC], pb.w[PB_SRC + 1], pb.w[PB_MIXER_CTRL], pb.w[PB_VOL_ENV], (int16_t)pb.w[PB_VOL_ENV + 1],
                        pb.w[PB_MIXER + MX_L], pb.w[PB_MIXER + MX_R], pb.w[PB_MIXER + MX_AL], pb.w[PB_MIXER + MX_AR],
                        pb.w[PB_UPDATES], pb.w[PB_UPDATES + 1], pb.w[PB_UPDATES + 2], pb.w[PB_UPDATES + 3], pb.w[PB_UPDATES + 4]);
        }

        /* (re)load the voice's position and decoder state: an update may have changed them */
        v.fmt = pb.w[PB_AUDIO_ADDR + 1];
        v.looping = pb.w[PB_AUDIO_ADDR] != 0;
        v.loop = ((uint32_t)pb.w[PB_AUDIO_ADDR + 2] << 16) | pb.w[PB_AUDIO_ADDR + 3];
        v.end = ((uint32_t)pb.w[PB_AUDIO_ADDR + 4] << 16) | pb.w[PB_AUDIO_ADDR + 5];
        v.cur = ((uint32_t)pb.w[PB_AUDIO_ADDR + 6] << 16) | pb.w[PB_AUDIO_ADDR + 7];
        v.coefs = (const int16_t*)&pb.w[PB_ADPCM];
        v.pred_scale = pb.w[PB_ADPCM + 17];
        v.yn1 = (int16_t)pb.w[PB_ADPCM + 18];
        v.yn2 = (int16_t)pb.w[PB_ADPCM + 19];
        ratio = ((uint32_t)pb.w[PB_SRC] << 16) | pb.w[PB_SRC + 1];
        frac = pb.w[PB_SRC + 2];
        for (i = 0; i < 4; i++) last[i] = (int16_t)pb.w[PB_SRC + 3 + i];
        if (ratio == 0) ratio = 0x10000;

        /* 32 output samples through the rate converter */
        for (i = 0; i < MS_SAMPLES; i++) {
            uint32_t step = frac + ratio, whole = step >> 16, k;
            frac = step & 0xFFFF;
            for (k = 0; k < whole; k++) {
                last[0] = last[1]; last[1] = last[2]; last[2] = last[3];
                last[3] = fetch_sample(&v);
            }
            /* In int, a difference near full scale times a fraction near one
             * overflows: 32769 * 65535 is 32,768 past the maximum and wraps
             * to full-scale negative, turning a silent sample into a click.
             * Command 0x01's mix already does its multiply wide; this is the
             * same arithmetic and needs the same width. */
            samples[i] = last[2] + (int32_t)(((int64_t)(last[3] - last[2]) * (int64_t)frac) >> 16);
        }

        /* volume envelope */
        vol = pb.w[PB_VOL_ENV];
        dvol = (int16_t)pb.w[PB_VOL_ENV + 1];
        for (i = 0; i < MS_SAMPLES; i++) {
            samples[i] = (samples[i] * (int32_t)vol) >> 15;
            vol = (uint16_t)(vol + dvol);
        }
        pb.w[PB_VOL_ENV] = vol;

        /* mixer: this microcode build (0x4E8A8B21) always mixes L/R; bit 0
         * adds aux A, bit 1 or bit 4 aux B, bit 2 surround, bit 3 the ramps.
         *
         * The two auxiliary-B bits differ in exactly one thing, the surround
         * send. Under bit 1 the driver keeps three aux-B gains and ramps all
         * three (0x8027DCCC, three calls to fn_8027C758); under bit 4 it keeps
         * and ramps two (0x8027DD60, two calls) and never writes the third, so
         * that channel has no gain to be mixed by and stays silent however the
         * surround bit is set. Everything else -- which gains, which bus, the
         * ramp bit -- is shared, so it is decoded once here. */
        {
            uint16_t raw = pb.w[PB_MIXER_CTRL];
            ctrl = MIX_L | MIX_R;
            if (raw & RAW_AUXA) ctrl |= MIX_AL | MIX_AR;
            if (raw & RAW_AUXB_ANY) ctrl |= MIX_BL | MIX_BR;
            if (raw & RAW_SURROUND) {
                ctrl |= MIX_S;
                if (raw & RAW_AUXA) ctrl |= MIX_AS;
                if (raw & RAW_AUXB) ctrl |= MIX_BS;
            }
            if (raw & RAW_RAMP)
                ctrl |= MIX_RAMP | ((raw & RAW_AUXA) ? MIX_A_RAMP : 0) |
                        ((raw & RAW_AUXB_ANY) ? MIX_B_RAMP : 0);
        }
        mx = &pb.w[PB_MIXER];
        {
            int o = ms * MS_SAMPLES;
            int r0 = (ctrl & MIX_RAMP) != 0, ra = (ctrl & MIX_A_RAMP) != 0, rb = (ctrl & MIX_B_RAMP) != 0;
            if (ctrl & MIX_L) mix_add(g_main[0] + o, samples, MS_SAMPLES, &mx[MX_L], r0, (int16_t)mx[MX_DL]);
            if (ctrl & MIX_R) mix_add(g_main[1] + o, samples, MS_SAMPLES, &mx[MX_R], r0, (int16_t)mx[MX_DR]);
            if (ctrl & MIX_S) mix_add(g_main[2] + o, samples, MS_SAMPLES, &mx[MX_S], r0, (int16_t)mx[MX_DS]);
            if (ctrl & MIX_AL) mix_add(g_auxa[0] + o, samples, MS_SAMPLES, &mx[MX_AL], ra, (int16_t)mx[MX_DAL]);
            if (ctrl & MIX_AR) mix_add(g_auxa[1] + o, samples, MS_SAMPLES, &mx[MX_AR], ra, (int16_t)mx[MX_DAR]);
            if (ctrl & MIX_AS) mix_add(g_auxa[2] + o, samples, MS_SAMPLES, &mx[MX_AS], ra, (int16_t)mx[MX_DAS]);
            if (ctrl & MIX_BL) mix_add(g_auxb[0] + o, samples, MS_SAMPLES, &mx[MX_BL], rb, (int16_t)mx[MX_DBL]);
            if (ctrl & MIX_BR) mix_add(g_auxb[1] + o, samples, MS_SAMPLES, &mx[MX_BR], rb, (int16_t)mx[MX_DBR]);
            if (ctrl & MIX_BS) mix_add(g_auxb[2] + o, samples, MS_SAMPLES, &mx[MX_BS], rb, (int16_t)mx[MX_DBS]);
            if (ctrl & (MIX_AL | MIX_AR | MIX_AS)) g_auxa_live = 1;
            if (ctrl & (MIX_BL | MIX_BR | MIX_BS)) g_auxb_live = 1;
        }

        /* state back into the block */
        pb.w[PB_AUDIO_ADDR + 6] = (uint16_t)(v.cur >> 16);
        pb.w[PB_AUDIO_ADDR + 7] = (uint16_t)v.cur;
        pb.w[PB_ADPCM + 17] = (uint16_t)v.pred_scale;
        pb.w[PB_ADPCM + 18] = (uint16_t)v.yn1;
        pb.w[PB_ADPCM + 19] = (uint16_t)v.yn2;
        pb.w[PB_SRC + 2] = (uint16_t)frac;
        for (i = 0; i < 4; i++) pb.w[PB_SRC + 3 + i] = (uint16_t)last[i];
        if (v.stopped) {
            pb.w[PB_RUNNING] = 0;
            if (g_starts_at_tick) break;
            v.stopped = 0; /* (the changes of the milliseconds left are still the block's) */
        }
    }
    pb_write(s, addr, &pb);
}

/* ---- the tap (host.c's, for soa_host_voices) ---------------------------------
 * A host that mixes the voices itself is handed, for each tick: every block on
 * the list as the driver left it, before this mixer has made a change in it or
 * moved a word; the tick's changes; for a stream the sixteen bytes of sound
 * memory under its cursor; what the list adds to left and right that is not
 * the tick's voices; and what was written out. A block's record is 216 bytes,
 * soa_host.h's SoaHostVoiceBlock: its address, its 192 bytes as memory has
 * them, the sixteen bytes, how many changes are its own, whether a stream. */
#define TAP_BLOCKS 64u
#define TAP_BLOCK_BYTES 216u
#define TAP_CHANGES 4096u
typedef void (*AxTap)(uint32_t tick, const uint8_t* blocks, unsigned nblocks, const uint8_t* changes, unsigned nchanges,
                      const int32_t* back, const int16_t* mixed);
static AxTap g_tap;
static uint8_t g_tap_blocks[TAP_BLOCKS * TAP_BLOCK_BYTES];
static uint8_t g_tap_changes[TAP_CHANGES * 4u];
static unsigned g_tap_nblocks, g_tap_nchanges;
static int32_t g_tap_voices[2][FRAME_SAMPLES]; /* what the tick's voices added to left and right */
static int32_t g_tap_back[2 * FRAME_SAMPLES];  /* the rest of what was written out: left's, then right's */
static int16_t g_tap_mixed[2 * FRAME_SAMPLES]; /* what was written out: left, right, left, right */
static int g_tap_out;

void ax_tap(AxTap fn) { g_tap = fn; }

static void tap_block(CpuState* s, uint32_t addr, const uint8_t* aram)
{
    uint8_t* b;
    uint16_t w[PB_WORDS], n16, stream;
    uint32_t data;
    unsigned i, n = 0;
    if (g_tap_nblocks >= TAP_BLOCKS) return;
    b = g_tap_blocks + g_tap_nblocks * TAP_BLOCK_BYTES;
    memset(b, 0, TAP_BLOCK_BYTES);
    memcpy(b, &addr, 4);
    for (i = 0; i < PB_WORDS; i++) {
        w[i] = rd16(s, addr + 2u * i);
        b[4u + 2u * i] = (uint8_t)(w[i] >> 8);
        b[5u + 2u * i] = (uint8_t)w[i];
    }
    stream = w[PB_IS_STREAM] ? 1 : 0;
    if (stream) {
        uint32_t cur = ((uint32_t)w[PB_AUDIO_ADDR + 6] << 16) | w[PB_AUDIO_ADDR + 7];
        for (i = 0; i < 16u; i++) b[196u + i] = aram[((cur >> 4) * 8u + i) & ARAM_MASK];
    }
    data = (((uint32_t)w[PB_UPDATES + 5] << 16) | w[PB_UPDATES + 6]) & 0x7FFFFFFFu;
    for (i = 0; i < 5u; i++) n += w[PB_UPDATES + i];
    if (!data) n = 0;
    if (n > 4096u) n = 4096u; /* as apply_updates reads no further */
    if (n > TAP_CHANGES - g_tap_nchanges) n = TAP_CHANGES - g_tap_nchanges;
    for (i = 0; i < n; i++) {
        uint16_t off = rd16(s, data + i * 4u), val = rd16(s, data + i * 4u + 2u);
        uint8_t* c = g_tap_changes + (g_tap_nchanges + i) * 4u;
        c[0] = (uint8_t)(off >> 8);
        c[1] = (uint8_t)off;
        c[2] = (uint8_t)(val >> 8);
        c[3] = (uint8_t)val;
    }
    g_tap_nchanges += n;
    n16 = (uint16_t)n;
    memcpy(b + 212, &n16, 2);
    memcpy(b + 214, &stream, 2);
    g_tap_nblocks++;
}

static void process_pb_list(CpuState* s, uint32_t addr)
{
    uint8_t* aram = aram_memory();
    int guard = 0;
    while (addr && guard++ < 256) {
        uint32_t next;
        if (g_tap) tap_block(s, addr, aram);
        process_voice(s, addr, aram);
        next = rd32(s, addr) & 0x7FFFFFFFu;
        if (next == addr) break;
        addr = next;
    }
    if ((unsigned)guard > g_chain_hi) g_chain_hi = (unsigned)guard;
}

/* ---- aux buses and output ----------------------------------------------- */

static void wr32(CpuState* s, uint32_t a, uint32_t v) { mem_w32(s, a | 0x80000000u, v); }

/* Buffers exchanged with the CPU are 32-bit samples, three channels of 160. */
static void upload_bus(CpuState* s, uint32_t addr, int32_t bus[3][FRAME_SAMPLES])
{
    int c, i;
    if (!addr) return;
    for (c = 0; c < 3; c++)
        for (i = 0; i < FRAME_SAMPLES; i++)
            wr32(s, addr + (uint32_t)(c * FRAME_SAMPLES + i) * 4u, (uint32_t)bus[c][i]);
}

/* Returns the peak of what came back, which is the only direct evidence that
 * the driver's CPU-side effects pass produced anything at all. */
static int32_t download_into_main(CpuState* s, uint32_t addr)
{
    int c, i;
    int32_t peak = 0;
    if (!addr) return 0;
    for (c = 0; c < 3; c++)
        for (i = 0; i < FRAME_SAMPLES; i++) {
            int32_t v = (int32_t)rd32(s, addr + (uint32_t)(c * FRAME_SAMPLES + i) * 4u);
            int32_t m = v < 0 ? -v : v;
            if (m > peak) peak = m;
            g_main[c][i] += v;
        }
    return peak;
}

static void output_samples(CpuState* s, uint32_t lr_addr, uint32_t s_addr)
{
    int i;
    for (i = 0; i < FRAME_SAMPLES; i++) {
        int16_t l = clamp16(g_main[0][i]), r = clamp16(g_main[1][i]);
        wr16(s, lr_addr + (uint32_t)i * 4u, (uint16_t)r);      /* right first, as the AI wants it */
        wr16(s, lr_addr + (uint32_t)i * 4u + 2u, (uint16_t)l);
    }
    if (s_addr)
        for (i = 0; i < FRAME_SAMPLES; i++) wr16(s, s_addr + (uint32_t)i * 2u, (uint16_t)clamp16(g_main[2][i]));
    g_samples_out += FRAME_SAMPLES;
}

/* ---- the command list ------------------------------------------------------- */

static int32_t bus_peak(int32_t bus[3][FRAME_SAMPLES])
{
    int32_t peak = 0;
    int c, i;
    for (c = 0; c < 3; c++)
        for (i = 0; i < FRAME_SAMPLES; i++) { int32_t v = bus[c][i] < 0 ? -bus[c][i] : bus[c][i]; if (v > peak) peak = v; }
    return peak;
}

void ax_command_list(CpuState* s, uint32_t addr)
{
    uint32_t pb_addr = 0;
    int end = 0, guard = 0;
    uint32_t p = addr & 0x7FFFFFFFu;
    uint64_t voices_before = g_voices;
    static int traced;
    if (g_verbose < 0) g_verbose = getenv("SOA_AX_VERBOSE") ? 1 : 0;
    memset(g_main, 0, sizeof g_main);
    memset(g_auxa, 0, sizeof g_auxa);
    memset(g_auxb, 0, sizeof g_auxb);
    g_auxa_live = g_auxb_live = 0;
    g_frames++;
    if (g_tap) {
        g_tap_nblocks = g_tap_nchanges = 0;
        g_tap_out = 0;
        memset(g_tap_voices, 0, sizeof g_tap_voices);
    }

    while (!end && guard++ < 64) {
        uint16_t cmd = rd16(s, p);
        p += 2;
        g_cmd_n[cmd < CENSUS_OPS ? cmd : CENSUS_OPS]++;
        if (g_verbose && (g_frames <= 4 || (g_voices > voices_before && traced < 40))) {
            fprintf(stderr, "[ax] frame %llu cmd %u main peak %d auxa %d auxb %d\n", (unsigned long long)g_frames, cmd, bus_peak(g_main), bus_peak(g_auxa), bus_peak(g_auxb));
            if (g_voices > voices_before) traced++;
        }
        switch (cmd) {
        case 0x00: /* SETUP: studio initial values and ramps (buses start silent) */
            p += 4;
            break;
        case 0x01: { /* DL_AND_VOL_MIX: samples from memory into main with volume */
            uint32_t a = (((uint32_t)rd16(s, p) << 16) | rd16(s, p + 2)) & 0x7FFFFFFFu;
            uint16_t vol_main = rd16(s, p + 4);
            uint16_t vol_a = rd16(s, p + 6), vol_b = rd16(s, p + 8);
            int c, i;
            p += 10;
            if (vol_a > g_dl_aux_a) g_dl_aux_a = vol_a;
            if (vol_b > g_dl_aux_b) g_dl_aux_b = vol_b;
            if (vol_a || vol_b) g_dl_aux_n++;
            for (c = 0; c < 3; c++)
                for (i = 0; i < FRAME_SAMPLES; i++)
                    g_main[c][i] += (int32_t)(((int64_t)(int32_t)rd32(s, a + (uint32_t)(c * FRAME_SAMPLES + i) * 4u) * vol_main) >> 15);
            break;
        }
        case 0x02: /* PB_ADDR */
            pb_addr = ((uint32_t)rd16(s, p) << 16) | rd16(s, p + 2);
            p += 4;
            break;
        case 0x03: /* PROCESS_PB */
            if (g_tap) {
                /* (what the blocks add to left and right: the two after, less the two before) */
                int c, i;
                for (c = 0; c < 2; c++)
                    for (i = 0; i < FRAME_SAMPLES; i++) g_tap_voices[c][i] -= g_main[c][i];
                process_pb_list(s, pb_addr & 0x7FFFFFFFu);
                for (c = 0; c < 2; c++)
                    for (i = 0; i < FRAME_SAMPLES; i++) g_tap_voices[c][i] += g_main[c][i];
                break;
            }
            process_pb_list(s, pb_addr & 0x7FFFFFFFu);
            break;
        /* MIX_AUXA / MIX_AUXB / MIX_AUXB_LR: upload the bus, read back the
         * processed one. 0x10 is the auxiliary-B command the driver emits
         * instead of 0x05 in the studio's other mode, and its payload is the
         * same five halfwords in the same order, over buffers of the same
         * 3 x 160 x int32 shape out of the same per-studio array (emitters at
         * 0x8027E900 and 0x8027EA14). What differs is entirely the driver's
         * side of it: 0x05 rotates through three buffers and 0x10 through two,
         * and the CPU's effects pass runs on 0x05's buffer but is skipped for
         * 0x10 (0x8027F2C4), so what 0x10 mixes back is the raw bus one frame
         * late. Both addresses arrive in the payload either way, which is why
         * none of that reaches this mixer and the three share one arm. */
        case 0x04: case 0x05: case 0x10: {
            uint32_t up = ((uint32_t)rd16(s, p) << 16) | rd16(s, p + 2);
            uint32_t down = ((uint32_t)rd16(s, p + 4) << 16) | rd16(s, p + 6);
            int32_t back;
            p += 8;
            /* the bus as the CPU is about to see it; a bus no voice reached is
             * zero everywhere, so only a live one is worth the scan */
            if (cmd == 0x04 ? g_auxa_live : g_auxb_live) {
                int32_t pk = bus_peak(cmd == 0x04 ? g_auxa : g_auxb);
                if (cmd == 0x04) {
                    g_auxa_frames++;
                    if (pk > g_auxa_peak) g_auxa_peak = pk;
                } else {
                    g_auxb_frames++;
                    if (pk > g_auxb_peak) g_auxb_peak = pk;
                }
            }
            upload_bus(s, up & 0x7FFFFFFFu, cmd == 0x04 ? g_auxa : g_auxb);
            back = download_into_main(s, down & 0x7FFFFFFFu);
            if (cmd == 0x04) {
                if (back > g_ret_a) g_ret_a = back;
            } else if (back > g_ret_b) {
                g_ret_b = back;
            }
            break;
        }
        case 0x06: /* UPLOAD_LRS: main bus to memory */
            upload_bus(s, (((uint32_t)rd16(s, p) << 16) | rd16(s, p + 2)) & 0x7FFFFFFFu, g_main);
            p += 4;
            break;
        case 0x07: { /* SET_LR: main L/R from memory */
            uint32_t a = (((uint32_t)rd16(s, p) << 16) | rd16(s, p + 2)) & 0x7FFFFFFFu;
            int c, i;
            p += 4;
            for (c = 0; c < 2; c++)
                for (i = 0; i < FRAME_SAMPLES; i++) g_main[c][i] = (int32_t)rd32(s, a + (uint32_t)(c * FRAME_SAMPLES + i) * 4u);
            break;
        }
        case 0x08: p += 20; break;
        case 0x09: { /* MIX_AUXB_NOWRITE */
            int32_t back =
                download_into_main(s, (((uint32_t)rd16(s, p) << 16) | rd16(s, p + 2)) & 0x7FFFFFFFu);
            if (back > g_ret_nw) g_ret_nw = back;
            p += 4;
            break;
        }
        case 0x0A: p += 4; break; /* compressor table */
        case 0x0B: case 0x0C: break;
        case 0x0D: /* MORE: continue with another list */
            p = (((uint32_t)rd16(s, p) << 16) | rd16(s, p + 2)) & 0x7FFFFFFFu;
            break;
        case 0x0E: { /* OUTPUT: surround address, then L/R address */
            uint32_t sa = ((uint32_t)rd16(s, p) << 16) | rd16(s, p + 2);
            uint32_t lr = ((uint32_t)rd16(s, p + 4) << 16) | rd16(s, p + 6);
            p += 8;
            output_samples(s, lr & 0x7FFFFFFFu, sa & 0x7FFFFFFFu);
            if (g_tap) {
                int c, i;
                for (c = 0; c < 2; c++)
                    for (i = 0; i < FRAME_SAMPLES; i++) g_tap_back[c * FRAME_SAMPLES + i] = g_main[c][i] - g_tap_voices[c][i];
                for (i = 0; i < FRAME_SAMPLES; i++) {
                    g_tap_mixed[2 * i] = clamp16(g_main[0][i]);
                    g_tap_mixed[2 * i + 1] = clamp16(g_main[1][i]);
                }
                g_tap_out = 1;
            }
            break;
        }
        case 0x0F: end = 1; break;
        case 0x11: { /* SET_OPPOSITE_LR: main L/R from 32-bit samples, right positive, left negated */
            uint32_t a = (((uint32_t)rd16(s, p) << 16) | rd16(s, p + 2)) & 0x7FFFFFFFu;
            int i;
            p += 4;
            for (i = 0; i < FRAME_SAMPLES; i++) {
                int32_t x = (int32_t)rd32(s, a + (uint32_t)i * 4u);
                int32_t m = x < 0 ? -x : x;
                if (m > g_ret_lr) g_ret_lr = m;
                g_main[0][i] = -x;
                g_main[1][i] = x;
                g_main[2][i] = 0;
            }
            break;
        }
        case 0x12: p += 12; break;
        case 0x13: p += 16; break; /* SEND_AUX_AND_MIX */
        default:
            if (g_verbose) fprintf(stderr, "[ax] unknown command %u\n", cmd);
            g_cmd_bad++;
            if (!g_cmd_bad_op) g_cmd_bad_op = cmd;
            end = 1;
            break;
        }
    }
    if ((unsigned)guard > g_cmds_hi) g_cmds_hi = (unsigned)guard;
    if (!end) g_lists_full++;
    if (g_tap && g_tap_out) g_tap((uint32_t)g_frames, g_tap_blocks, g_tap_nblocks, g_tap_changes, g_tap_nchanges, g_tap_back, g_tap_mixed);
}

void ax_report(void)
{
    fprintf(stderr, "[ax] %llu frames mixed, %llu voice-frames, %llu samples output\n",
            (unsigned long long)g_frames, (unsigned long long)g_voices, (unsigned long long)g_samples_out);
    census_report();
}
