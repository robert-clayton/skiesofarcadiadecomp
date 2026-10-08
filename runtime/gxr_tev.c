/*
 * Texture environment: texture decode and sampling, TLUTs in TMEM, and the
 * TEV combiner stages. Register layouts follow the hardware (BP register
 * numbers in comments); the arithmetic follows the documented fixed-point
 * combiner: lerp in 8.8, bias, shift, clamp to 8 or 11 bits.
 *
 * Everything that can be decided per draw is decided once, in
 * tev_prepare: stage selectors, konst values, swap tables, and the decoded
 * textures with their scale and wrap modes. The per-pixel path then only
 * indexes.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "gxr.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
/* Every x86-64 build gets the SIMD path (L2a): MSVC, clang-cl, and gcc or
 * clang for Linux or windows-gnu. The blend is a PLAT_TARGET_SSE41 function,
 * so the file itself stays baseline x86-64 and simd_decide chooses at run
 * time. Before L2a the test was _MSC_VER && _M_X64, which clang-cl passed and
 * then could not compile, and a windows-gnu build silently failed. */
#if PLAT_X86_64
#include <smmintrin.h>
#define TEX_SIMD 1
#endif

static CpuState* g_s;
static uint8_t g_tmem[1u << 20];

/* SSE4.1 for the pixel path's SIMD (H15d): 1 when this CPU has it and
 * SOA_GXR_NOSIMD is not set, 0 otherwise. Decided on the producer, in
 * tev_prepare, before any worker samples; the scalar path is the fallback and
 * the reference, and the two agree bit for bit (test_gxr_fastpath.py). */
static int g_gxr_simd = -1;
/* SOA_GXR_NOTEX: every texel a flat grey, for telling texture faults from
 * the rest. Decided on the producer in tev_prepare, beside simd_decide,
 * before any draw that samples is published: sample() runs on the workers,
 * and deciding it there was a write several of them could race to (L8). */
static int g_notex = -1;

static void simd_decide(void)
{
    g_gxr_simd = 0;
#ifdef TEX_SIMD
    {
        const char* off = getenv("SOA_GXR_NOSIMD");
        g_gxr_simd = plat_cpu_has_sse41() && !(off && atoi(off));
    }
#endif
}

/* Tripwires, as in gxr.c: what this file does not model says so once and then
 * keeps quiet. Neither of the two below can use that file's plain flag -- one
 * wants a line per texture format, the other a line when a count crosses --
 * but both run inside tev_prepare, on the guest thread that parses the
 * command stream, so neither needs a lock. */

void tex_set_memory(CpuState* s)
{
    g_s = s;
}

/* ---- texture cache ------------------------------------------------------ */

typedef struct {
    uint32_t addr, fmt, w, h, tlut_off, tlut_fmt; /* the key, kept when the decode is dropped */
    uint8_t* rgba; /* every level, consecutively; level 0 first; NULL once dropped */
    const uint8_t* level[MAX_MIPS];
    int lw[MAX_MIPS], lh[MAX_MIPS], nlevels;
    uint64_t stamp;
    uint64_t hash;
    uint64_t hashed; /* the epoch the hash was taken in */
    int replaced; /* a mod's image stands in for the decode: one level, any size */
    int from_copy; /* the decode is a copy's image, made in the pool by command copy_cmd */
    long long copy_cmd;
    uint32_t gen; /* moves on every decode, replacement, copy image and eviction (TexCfg.tex_gen) */
} TexEntry;

/* The texture epoch (PLAN-60FPS-MODS H12). A texture's source bytes are
 * hashed at most once an epoch, and the epoch moves on everything that can
 * change texture memory under the renderer: the game's own texture-cache
 * invalidate (BP 0x66, which GXInvalidateTexAll and GXInvalidateTexRegion
 * write, and which the hardware needs before it will sample texels the CPU
 * rewrote), every EFB copy, to a texture or to the screen, and so every frame
 * end. Hashing on every lookup read 16-39 MB a frame in the heavy captures,
 * one texture thirty times over; once an epoch it is the 0.8-2.0 MB of
 * distinct textures, a few times a frame. SOA_TEXVERIFY=1 hashes on every
 * lookup as before and counts the rewrites the epochs would have missed. */
static uint64_t g_tex_epoch = 1;
static int g_tex_verify = -1;
static unsigned long long g_lookups, g_hashes, g_hash_bytes, g_decodes, g_missed;
/* Why each decode happened, for the report: a key first seen, its bytes or
 * palette rewritten, more mip levels wanted, or a decode dropped and made
 * again -- and how many of those came out as they were. */
static unsigned long long g_dec_new, g_dec_rewritten, g_dec_levels, g_dec_dropped, g_dec_same;
static unsigned long long g_tlut_loads, g_tlut_marked;
/* Copy images: made by copies to memory; draws that sampled one while its copy
 * was queued; lookups that found one retired by the drain, or overwritten by a
 * newer copy, and read the texture from memory instead. */
static unsigned long long g_img_made, g_img_used, g_img_retired, g_img_overwritten;
/* The fence the draw being set up needs: every worker past the copies whose
 * images it samples. Producer only; tev_prepare starts it at 0. */
static long long g_tex_fence;

void tex_epoch_advance(void)
{
    g_tex_epoch++;
}

/* A mod's texture provider (PLAN-60FPS-MODS M3c), asked once per decode with
 * the texture's source hash -- its bytes, and its palette for the indexed
 * formats, the same key the cache uses and stable from run to run -- and the
 * decoded base level. Returning 1 with a w x h RGBA8 image (any size: the
 * sampler scales by lw/w) replaces the texture; the image is copied at once.
 * Registered through a setter, so the renderer still links alone. */
static int (*g_tex_provider)(uint64_t hash, uint32_t fmt, uint32_t w, uint32_t h, const uint8_t* rgba,
                             const uint8_t** out, uint32_t* out_w, uint32_t* out_h);

void gxr_set_texture_provider(int (*fn)(uint64_t hash, uint32_t fmt, uint32_t w, uint32_t h, const uint8_t* rgba,
                                        const uint8_t** out, uint32_t* out_w, uint32_t* out_h))
{
    g_tex_provider = fn;
}

/* 1,024 entries (H12). With 256, H1's Part L run threw out 64 in one frame
 * (frame 2071, a menu's 24x24 glyphs) and the heaviest captured frame wants
 * 266. Found through an index and not by comparing every entry, which 256
 * could afford and 1,024 cannot: at 4,400 lookups a frame that scan would be
 * 4.5 million compares. An entry keeps its key when its decode is dropped, so
 * the index forgets a key only at an eviction. */
#define TEX_CACHE 1024
#define TEX_INDEX 2048 /* a power of two, twice the entries */
static TexEntry g_cache[TEX_CACHE];
static int g_cache_used;           /* entries ever given a key; the rest are fresh */
static int16_t g_index[TEX_INDEX]; /* entry number + 1, by key, linear probing; 0 is empty */
static uint64_t g_stamp;
/* Decoded textures thrown out to make room: the run's total, and how many in
 * the frame g_evict_frame names. The tripwire below is about the second. */
static unsigned g_evicted, g_evicted_in_frame, g_evict_frame;

/* Two things hold pointers into the cache: every draw still in the queue, and
 * the draw the producer is building right now. A decoded texture is therefore
 * never handed straight back to the allocator -- it waits here until a flush
 * has proved that neither can reach it -- and there is no number of waiting
 * textures that it is safe to exceed, because exceeding it means freeing a
 * buffer a worker thread is about to sample. So this list grows instead of
 * overflowing. GRAVE_SOFT below is a hint to the producer that the waiting
 * textures are piling up and it is a good moment to flush; nothing goes wrong
 * if a caller sails past it. */
#define GRAVE_SOFT 448
static uint8_t** g_grave;
static int g_grave_n, g_grave_cap, g_grave_peak;
static unsigned g_grave_lost;

/* The setup tev_prepare is filling, or NULL between draws. It is not in the
 * queue yet, so draining the queue says nothing about it: a flush that happens
 * while it is being built -- which is what a texture read from a destination a
 * queued copy has not written yet does -- has to leave its textures alone and
 * let the next flush take them. */
static const TevSetup* g_building;

static void tex_free_later(uint8_t* p)
{
    if (!p) return;
    if (g_grave_n == g_grave_cap) {
        int cap = g_grave_cap ? g_grave_cap * 2 : 512;
        uint8_t** grown = (uint8_t**)realloc(g_grave, (size_t)cap * sizeof *grown);
        if (!grown) {
            /* Out of memory for a pointer. Losing the texture costs its bytes;
             * freeing it here would cost a worker reading freed memory, which
             * is the whole reason this list exists. */
            if (!g_grave_lost++)
                fprintf(stderr, "[gxr] cannot grow the texture graveyard past %d entries; decoded textures are being leaked rather than freed while a queued draw may still sample them\n",
                        g_grave_cap);
            return;
        }
        g_grave = grown;
        g_grave_cap = cap;
    }
    g_grave[g_grave_n++] = p;
    if (g_grave_n > g_grave_peak) g_grave_peak = g_grave_n;
}

int tex_graveyard_full(void) { return g_grave_n >= GRAVE_SOFT; }

/* The high-water mark, for gxr_report: a run that passed 512 here is a run the
 * old fixed-size list would have overflowed, and every texture past that one
 * was freed while queued draws still pointed at it. */
int tex_graveyard_peak(void) { return g_grave_peak; }

void tex_graveyard_empty(void)
{
    int i, keep = 0;
    for (i = 0; i < g_grave_n; i++) {
        uint8_t* p = g_grave[i];
        int live = 0, j;
        /* level[0] is the start of the single block a decode allocates for all
         * of a texture's levels, which is the pointer that was queued here, so
         * comparing against it finds every way the setup can still reach p. */
        if (g_building)
            for (j = 0; j < 8; j++)
                if (g_building->tex[j].level[0] == p) { live = 1; break; }
        if (live) g_grave[keep++] = p;
        else free(p);
    }
    g_grave_n = keep;
}

void tex_invalidate_all(void)
{
    int i;
    for (i = 0; i < g_cache_used; i++) {
        tex_free_later(g_cache[i].rgba);
        g_cache[i].rgba = NULL;
        g_cache[i].from_copy = 0;
        g_cache[i].gen++;
    }
}

/* Bytes a texture occupies in memory, from its tiled layout. */
static uint32_t texture_bytes(uint32_t fmt, uint32_t w, uint32_t h)
{
    unsigned tw, th, bpt;
    switch (fmt) {
    case 0: case 8: tw = 8; th = 8; bpt = 32; break;
    case 1: case 2: case 9: tw = 8; th = 4; bpt = 32; break;
    case 6: tw = 4; th = 4; bpt = 64; break;
    case 14: tw = 8; th = 8; bpt = 32; break;
    default: tw = 4; th = 4; bpt = 32; break;
    }
    return ((w + tw - 1) / tw) * ((h + th - 1) / th) * bpt;
}

#define HASH_P1 0x9E3779B185EBCA87ull
#define HASH_P2 0xC2B2AE3D27D4EB4Full

static uint64_t rotl64(uint64_t v, unsigned r) { return (v << r) | (v >> (64 - r)); }

/* Fold n bytes into h, reading every one of them.
 *
 * Four independent lanes, because the cost that matters here is throughput,
 * not the strength of the mixing: one 64-bit multiply has three cycles of
 * latency and four of them in flight keep the loop at memory speed rather
 * than at multiplier speed. Measured with the port's own flags it sustains
 * 35-40 GB/s, so every caller below is bounded by how many bytes it reads.
 * The rotates in the fold are rotates and not shifts so that no lane loses
 * the bits that a shift would push out of the word. */
static uint64_t hash_range(uint64_t h, const uint8_t* p, uint32_t n)
{
    uint64_t h0 = h ^ HASH_P1, h1 = h ^ HASH_P2, h2 = h + n, h3 = h ^ 0x165667B19E3779F9ull;
    uint32_t left = n;
    while (left >= 32) {
        uint64_t a, b, c, d;
        memcpy(&a, p, 8); memcpy(&b, p + 8, 8); memcpy(&c, p + 16, 8); memcpy(&d, p + 24, 8);
        h0 = (h0 ^ a) * HASH_P1;
        h1 = (h1 ^ b) * HASH_P1;
        h2 = (h2 ^ c) * HASH_P1;
        h3 = (h3 ^ d) * HASH_P1;
        p += 32;
        left -= 32;
    }
    /* Every tiled texture size and every palette size is a multiple of 32, so
     * the two tails below are for a caller that is not one of those. */
    while (left >= 8) {
        uint64_t a;
        memcpy(&a, p, 8);
        h0 = (h0 ^ a) * HASH_P1;
        p += 8;
        left -= 8;
    }
    if (left) {
        uint64_t a = 0;
        memcpy(&a, p, left);
        h1 = (h1 ^ a) * HASH_P1;
    }
    h = rotl64(h0, 1) + rotl64(h1, 7) + rotl64(h2, 12) + rotl64(h3, 18) + n;
    h ^= h >> 33;
    h *= HASH_P2;
    h ^= h >> 29;
    h *= HASH_P1;
    h ^= h >> 32;
    return h;
}

/* A fingerprint of a texture's source bytes and its palette, over all of
 * them. It decides whether a cached decode is still the picture the guest
 * has in memory, so a byte it does not read is a byte the game can rewrite
 * in place while the renderer keeps drawing the old texture. The 64 spread
 * words this replaced read the same 256 bytes whether the image was 32 bytes
 * or a megabyte, so the larger the texture the smaller the fraction of it
 * that was ever looked at -- one in four thousand for a 1 MB sky.
 *
 * This ran on every texture lookup, once per map per draw on the guest
 * thread, until H12 made it once an epoch (above); what it costs was
 * measured rather than assumed. Against the old
 * sample, per lookup: a 24x24 I4 glyph (288 bytes) 13 ns instead of 47,
 * because 288 contiguous bytes are five cache lines where 64 spread words
 * were up to 64 of them; a 64x64 CMPR texture (8 KB, seven of every eight
 * lookups in the captured corpus) 220 ns instead of 58; a 1 MB RGBA8 sky
 * 27 us instead of 250 ns. Over a whole frame, across the twenty-three
 * captures, that is between +0.02 and +0.84 ms on the guest thread, the
 * worst of them 2.5% of a 33 ms frame -- against the 47% of a run that A4
 * measured inside the OS idle loop. */
static uint64_t source_hash(uint32_t addr, uint32_t fmt, uint32_t w, uint32_t h, uint32_t tlut_off)
{
    uint32_t bytes = texture_bytes(fmt, w, h);
    uint64_t hsh;
    if (!g_s || (addr & MEM_MASK) + bytes > MEM1_SIZE) return 0;
    hsh = hash_range(0x243F6A8885A308D3ull, mem_ptr(g_s, addr), bytes);
    if (fmt == 8 || fmt == 9 || fmt == 10) {
        /* The palette decides the pixels as much as the indices do, and
         * this is what notices a new one: tmem_load_tlut only sends the
         * entries a load overlaps to be hashed again. 32 bytes for C4, 512
         * for C8, the whole 32 KB table for C14X2. */
        uint32_t n = fmt == 8 ? 32 : (fmt == 9 ? 512 : 32768);
        uint32_t off = tlut_off & ((1u << 20) - 1);
        if (off + n > sizeof g_tmem) n = (uint32_t)(sizeof g_tmem - off);
        hsh = hash_range(hsh, g_tmem + off, n);
    }
    return hsh;
}

void tmem_load_tlut(CpuState* s, uint32_t src, uint32_t tmem_off, uint32_t bytes)
{
    int i;
    if (tmem_off + bytes > sizeof g_tmem || (src & MEM_MASK) + bytes > MEM1_SIZE) return;
    memcpy(g_tmem + tmem_off, mem_ptr(s, src), bytes);
    g_tlut_loads++;
    /* Palettised textures decoded through this range may be stale now, so
     * their next lookup hashes them again (epochs start at 1), and the hash
     * covers the palette bytes the decode reads: only a texture whose palette
     * did change is decoded again. The game loads the same palettes over and
     * over. This used to throw the decodes out, and in H1's Part L run every
     * one of the 35,667 made again that way came out as it was -- 93% of the
     * decode time (FINDINGS "Palette loads"). The range is still the widest
     * a palette can be, since a mark costs a hash and not a decode. */
    for (i = 0; i < g_cache_used; i++) {
        TexEntry* e = &g_cache[i];
        if (e->rgba && (e->fmt == 8 || e->fmt == 9 || e->fmt == 10) && e->tlut_off < tmem_off + bytes && e->tlut_off + 32768 > tmem_off) {
            e->hashed = 0;
            g_tlut_marked++;
        }
    }
}

/* ---- texture decode --------------------------------------------------- */

#ifdef _MSC_VER
#define TEX_INLINE static __forceinline
#else
#define TEX_INLINE static inline
#endif

static void tlut_color(uint32_t tlut_off, uint32_t tlut_fmt, unsigned index, uint8_t* out)
{
    const uint8_t* p = g_tmem + ((tlut_off + index * 2) & ((1u << 20) - 1));
    unsigned v = ((unsigned)p[0] << 8) | p[1];
    switch (tlut_fmt) {
    case 0: /* IA8 */
        out[0] = out[1] = out[2] = p[1];
        out[3] = p[0];
        break;
    case 1: /* RGB565 */
        out[0] = (uint8_t)(((v >> 11) & 31) * 255 / 31);
        out[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63);
        out[2] = (uint8_t)((v & 31) * 255 / 31);
        out[3] = 255;
        break;
    default: /* RGB5A3 */
        if (v & 0x8000) {
            out[0] = (uint8_t)(((v >> 10) & 31) * 255 / 31);
            out[1] = (uint8_t)(((v >> 5) & 31) * 255 / 31);
            out[2] = (uint8_t)((v & 31) * 255 / 31);
            out[3] = 255;
        } else {
            out[0] = (uint8_t)(((v >> 8) & 15) * 17);
            out[1] = (uint8_t)(((v >> 4) & 15) * 17);
            out[2] = (uint8_t)((v & 15) * 17);
            out[3] = (uint8_t)(((v >> 12) & 7) * 255 / 7);
        }
        break;
    }
}

static void rgb565(unsigned v, uint8_t* out)
{
    out[0] = (uint8_t)(((v >> 11) & 31) * 255 / 31);
    out[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63);
    out[2] = (uint8_t)((v & 31) * 255 / 31);
    out[3] = 255;
}

static void decode_cmpr_block(const uint8_t* p, uint8_t* out, unsigned ox, unsigned oy, unsigned w, unsigned h)
{
    unsigned c0 = ((unsigned)p[0] << 8) | p[1], c1 = ((unsigned)p[2] << 8) | p[3];
    uint8_t pal[4][4];
    unsigned y, x;
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    if (c0 > c1) {
        int i;
        for (i = 0; i < 3; i++) {
            pal[2][i] = (uint8_t)((2 * pal[0][i] + pal[1][i]) / 3);
            pal[3][i] = (uint8_t)((pal[0][i] + 2 * pal[1][i]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        int i;
        for (i = 0; i < 3; i++) {
            pal[2][i] = (uint8_t)((pal[0][i] + pal[1][i]) / 2);
            pal[3][i] = 0;
        }
        pal[2][3] = 255;
        pal[3][3] = 0;
    }
    for (y = 0; y < 4; y++) {
        unsigned row = p[4 + y];
        for (x = 0; x < 4; x++) {
            unsigned idx = (row >> (6 - 2 * x)) & 3;
            unsigned px = ox + x, py = oy + y;
            if (px < w && py < h) memcpy(out + (py * w + px) * 4, pal[idx], 4);
        }
    }
}

/* One texel: column ix, row iy of its tile, in format fmt, to RGBA. */
TEX_INLINE void decode_texel(const uint8_t* tile, unsigned ix, unsigned iy, uint32_t fmt, uint32_t tlut_off,
                             uint32_t tlut_fmt, uint8_t* o)
{
    unsigned v;
    switch (fmt) {
    case 0: /* I4 */
        v = tile[iy * 4 + ix / 2];
        v = (ix & 1) ? (v & 15) : (v >> 4);
        o[0] = o[1] = o[2] = o[3] = (uint8_t)(v * 17);
        break;
    case 1: /* I8 */
        v = tile[iy * 8 + ix];
        o[0] = o[1] = o[2] = o[3] = (uint8_t)v;
        break;
    case 2: /* IA4 */
        v = tile[iy * 8 + ix];
        o[0] = o[1] = o[2] = (uint8_t)((v & 15) * 17);
        o[3] = (uint8_t)((v >> 4) * 17);
        break;
    case 3: /* IA8 */
        o[3] = tile[(iy * 4 + ix) * 2];
        o[0] = o[1] = o[2] = tile[(iy * 4 + ix) * 2 + 1];
        break;
    case 4: /* RGB565 */
        v = ((unsigned)tile[(iy * 4 + ix) * 2] << 8) | tile[(iy * 4 + ix) * 2 + 1];
        rgb565(v, o);
        break;
    case 5: /* RGB5A3 */
        v = ((unsigned)tile[(iy * 4 + ix) * 2] << 8) | tile[(iy * 4 + ix) * 2 + 1];
        if (v & 0x8000) {
            o[0] = (uint8_t)(((v >> 10) & 31) * 255 / 31);
            o[1] = (uint8_t)(((v >> 5) & 31) * 255 / 31);
            o[2] = (uint8_t)((v & 31) * 255 / 31);
            o[3] = 255;
        } else {
            o[0] = (uint8_t)(((v >> 8) & 15) * 17);
            o[1] = (uint8_t)(((v >> 4) & 15) * 17);
            o[2] = (uint8_t)((v & 15) * 17);
            o[3] = (uint8_t)(((v >> 12) & 7) * 255 / 7);
        }
        break;
    case 6: /* RGBA8: 16 AR pairs then 16 GB pairs */
        o[3] = tile[(iy * 4 + ix) * 2];
        o[0] = tile[(iy * 4 + ix) * 2 + 1];
        o[1] = tile[32 + (iy * 4 + ix) * 2];
        o[2] = tile[32 + (iy * 4 + ix) * 2 + 1];
        break;
    case 8: /* C4 */
        v = tile[iy * 4 + ix / 2];
        v = (ix & 1) ? (v & 15) : (v >> 4);
        tlut_color(tlut_off, tlut_fmt, v, o);
        break;
    case 9: /* C8 */
        tlut_color(tlut_off, tlut_fmt, tile[iy * 8 + ix], o);
        break;
    case 10: /* C14X2 */
        v = ((unsigned)tile[(iy * 4 + ix) * 2] << 8) | tile[(iy * 4 + ix) * 2 + 1];
        tlut_color(tlut_off, tlut_fmt, v & 0x3FFF, o);
        break;
    default:
        o[0] = 255; o[1] = 0; o[2] = 255; o[3] = 255; /* unsupported: magenta */
        break;
    }
}

static void decode_level(uint8_t* out, uint32_t addr, uint32_t fmt, uint32_t w, uint32_t h, uint32_t tlut_off, uint32_t tlut_fmt)
{
    const uint8_t* base;
    unsigned tw, th, bytes_per_tile, tiles_w;
    unsigned x, y, tx, ty;
    if (!out || !g_s) return;
    if ((addr & MEM_MASK) >= MEM1_SIZE) return;
    base = mem_ptr(g_s, addr);

    /* A format with no case below paints magenta, which is indistinguishable
     * from a texture the game meant to be magenta. One line per format rather
     * than one overall: a second unhandled format is a second thing to build,
     * and fmt is four bits, so the mask of what has been said is 16 flags. */
    if (fmt == 7 || (fmt >= 11 && fmt != 14)) {
        static unsigned said;
        if (!(said & (1u << (fmt & 15)))) {
            said |= 1u << (fmt & 15);
            fprintf(stderr, "[gxr] texture format %u is not decoded (%ux%u at %08X); it draws magenta\n", fmt, w, h, addr);
        }
    }

    switch (fmt) {
    case 0: case 8: tw = 8; th = 8; bytes_per_tile = 32; break;       /* I4, C4 */
    case 1: case 2: case 9: tw = 8; th = 4; bytes_per_tile = 32; break; /* I8, IA4, C8 */
    case 3: case 4: case 5: case 10: tw = 4; th = 4; bytes_per_tile = 32; break; /* IA8 565 5A3 C14X2 */
    case 6: tw = 4; th = 4; bytes_per_tile = 64; break;                /* RGBA8 */
    case 14: tw = 8; th = 8; bytes_per_tile = 32; break;               /* CMPR */
    default: tw = 4; th = 4; bytes_per_tile = 32; break;
    }
    tiles_w = (w + tw - 1) / tw;

    if (fmt == 14) {
        unsigned by, bx;
        for (by = 0; by < (h + 7) / 8; by++)
            for (bx = 0; bx < tiles_w; bx++) {
                const uint8_t* blk = base + (by * tiles_w + bx) * 32;
                if ((size_t)(blk - g_s->mem) + 32 > MEM1_SIZE) continue;
                decode_cmpr_block(blk, out, bx * 8, by * 8, w, h);
                decode_cmpr_block(blk + 8, out, bx * 8 + 4, by * 8, w, h);
                decode_cmpr_block(blk + 16, out, bx * 8, by * 8 + 4, w, h);
                decode_cmpr_block(blk + 24, out, bx * 8 + 4, by * 8 + 4, w, h);
            }
        return;
    }

    /* Tile by tile, texel by texel within the tile: each texel comes from the
     * same tile, row and column as a walk in raster order would give it,
     * without the four divisions a texel that walk took to find them, by
     * divisors the compiler could not know (FINDINGS, "Texture decode by
     * tile"). The bounds check is the tile's, once. */
    for (ty = 0; ty < (h + th - 1) / th; ty++) {
        for (tx = 0; tx < tiles_w; tx++) {
            const uint8_t* tile = base + (ty * tiles_w + tx) * bytes_per_tile;
            unsigned ix, iy;
            if ((size_t)(tile - g_s->mem) + bytes_per_tile > MEM1_SIZE) continue;
            for (iy = 0; iy < th; iy++) {
                y = ty * th + iy;
                if (y >= h) break;
                for (ix = 0; ix < tw; ix++) {
                    uint8_t* o;
                    x = tx * tw + ix;
                    if (x >= w) break;
                    o = out + (y * w + x) * 4;
                    decode_texel(tile, ix, iy, fmt, tlut_off, tlut_fmt, o);
                }
            }
        }
    }
}

/* Decode a texture and up to `nlevels` of its mipmaps, which follow the
 * base level in memory, each tiled at its own size. */
static void decode_texture(TexEntry* e, int nlevels)
{
    uint32_t w = e->w, h = e->h, addr = e->addr;
    size_t total = 0;
    int l;
    uint8_t* out;
    if (nlevels < 1) nlevels = 1;
    if (nlevels > MAX_MIPS) nlevels = MAX_MIPS;
    e->nlevels = 0;
    for (l = 0; l < nlevels; l++) {
        e->lw[l] = (int)w; e->lh[l] = (int)h;
        total += (size_t)w * h * 4;
        e->nlevels++;
        if (w == 1 && h == 1) break;
        w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1;
    }
    out = (uint8_t*)calloc(total, 1);
    e->rgba = out;
    e->gen++;
    if (!out) {
        /* The levels still name the buffer the caller has just put in the
         * graveyard, and this entry is about to be handed to a draw. Say there
         * is no texture instead, which the sampler draws as transparent black. */
        for (l = 0; l < MAX_MIPS; l++) e->level[l] = NULL;
        e->nlevels = 0;
        return;
    }
    for (l = 0; l < e->nlevels; l++) {
        e->level[l] = out;
        decode_level(out, addr, e->fmt, (uint32_t)e->lw[l], (uint32_t)e->lh[l], e->tlut_off, e->tlut_fmt);
        addr += texture_bytes(e->fmt, (uint32_t)e->lw[l], (uint32_t)e->lh[l]);
        out += (size_t)e->lw[l] * e->lh[l] * 4;
    }
    for (; l < MAX_MIPS; l++) e->level[l] = NULL;
}

/* Row y of a w-wide texture in format fmt, tiled at base, into row y of the
 * RGBA image out -- the texels decode_level would give that row, from the same
 * bytes through the same decode_texel. A worker runs it on each row of a copy
 * it has just written (copy images, FINDINGS "Copy images"); every texel of a
 * row is written by the one worker that owns the row, so it reads only bytes
 * it wrote. The formats a copy makes, which need no palette. */
void tex_decode_row(uint8_t* out, const uint8_t* base, uint32_t fmt, uint32_t w, uint32_t y)
{
    unsigned tw, th, bytes_per_tile, tiles_w, tx, ix, iy, x;
    const uint8_t* row;
    switch (fmt) {
    case 0: tw = 8; th = 8; bytes_per_tile = 32; break;
    case 1: case 2: tw = 8; th = 4; bytes_per_tile = 32; break;
    case 3: case 4: case 5: tw = 4; th = 4; bytes_per_tile = 32; break;
    case 6: tw = 4; th = 4; bytes_per_tile = 64; break;
    default: return;
    }
    tiles_w = (w + tw - 1) / tw;
    row = base + (y / th) * tiles_w * bytes_per_tile;
    iy = y % th;
    for (tx = 0; tx < tiles_w; tx++)
        for (ix = 0; ix < tw; ix++) {
            x = tx * tw + ix;
            if (x >= w) break;
            decode_texel(row + tx * bytes_per_tile, ix, iy, fmt, 0, 0, out + ((size_t)y * w + x) * 4);
        }
}

/* After a decode, with the entry's hash set: a mod's image replaces it, as one
 * level of its own size. The decode it replaces was never handed to a draw,
 * but it goes through the graveyard like any other buffer. */
static void maybe_replace(TexEntry* e)
{
    const uint8_t* img = NULL;
    uint32_t w = 0, h = 0;
    uint8_t* copy;
    int l;
    e->replaced = 0;
    if (!g_tex_provider || !e->rgba || !e->level[0]) return;
    if (!g_tex_provider(e->hash, e->fmt, (uint32_t)e->lw[0], (uint32_t)e->lh[0], e->level[0], &img, &w, &h)) return;
    if (!img || !w || !h || w > 4096 || h > 4096) return;
    copy = (uint8_t*)malloc((size_t)w * h * 4);
    if (!copy) return;
    memcpy(copy, img, (size_t)w * h * 4);
    tex_free_later(e->rgba);
    e->rgba = copy;
    e->gen++;
    e->level[0] = copy;
    e->lw[0] = (int)w;
    e->lh[0] = (int)h;
    e->nlevels = 1;
    for (l = 1; l < MAX_MIPS; l++) e->level[l] = NULL;
    e->replaced = 1;
}

/* ---- the index ----------------------------------------------------------- */

static uint32_t tex_home(const TexEntry* e)
{
    uint64_t k = ((uint64_t)e->addr << 32) ^ ((uint64_t)e->fmt << 28) ^ ((uint64_t)e->w << 16) ^ e->h ^
                 ((uint64_t)e->tlut_off << 7) ^ ((uint64_t)e->tlut_fmt << 60);
    k ^= k >> 33;
    k *= HASH_P2;
    k ^= k >> 29;
    return (uint32_t)k & (TEX_INDEX - 1);
}

static int same_key(const TexEntry* a, const TexEntry* b)
{
    return a->addr == b->addr && a->fmt == b->fmt && a->w == b->w && a->h == b->h && a->tlut_off == b->tlut_off &&
           a->tlut_fmt == b->tlut_fmt;
}

static TexEntry* tex_find(const TexEntry* key)
{
    uint32_t i;
    for (i = tex_home(key); g_index[i]; i = (i + 1) & (TEX_INDEX - 1))
        if (same_key(&g_cache[g_index[i] - 1], key)) return &g_cache[g_index[i] - 1];
    return NULL;
}

static void tex_index_add(int n)
{
    uint32_t i = tex_home(&g_cache[n]);
    while (g_index[i]) i = (i + 1) & (TEX_INDEX - 1);
    g_index[i] = (int16_t)(n + 1);
}

/* Entry n's key out of the index, closing the gap behind it (Knuth's
 * algorithm R) so every other key is still found by walking forward from its
 * home: a key moves back into the hole unless its home lies cyclically in
 * (hole, where it is]. */
static void tex_index_remove(int n)
{
    uint32_t i = tex_home(&g_cache[n]), j, home;
    while (g_index[i] != n + 1) i = (i + 1) & (TEX_INDEX - 1);
    for (j = i;;) {
        g_index[i] = 0;
        for (;;) {
            j = (j + 1) & (TEX_INDEX - 1);
            if (!g_index[j]) return;
            home = tex_home(&g_cache[g_index[j] - 1]);
            if (!(i <= j ? (i < home && home <= j) : (i < home || home <= j))) break;
        }
        g_index[i] = g_index[j];
        i = j;
    }
}

/* ---- lookup --------------------------------------------------------------- */

static uint64_t counted_hash(const TexEntry* e)
{
    g_hashes++;
    g_hash_bytes += texture_bytes(e->fmt, e->w, e->h);
    return source_hash(e->addr, e->fmt, e->w, e->h, e->tlut_off);
}

/* SOA_TEXVERIFY found a texture whose bytes changed inside one epoch: without
 * it the renderer would have gone on drawing the old decode. */
static void missed_rewrite(const TexEntry* e)
{
    if (++g_missed <= 8)
        fprintf(stderr, "[gxr] SOA_TEXVERIFY: texture %08X (%ux%u fmt %u) changed in frame %u with no texture-cache invalidate, copy or frame end since it was hashed; without SOA_TEXVERIFY the old decode would have been drawn\n",
                e->addr, e->w, e->h, e->fmt, gx_frame_count());
}

/* Where a texture not in the cache goes: a fresh entry, else one whose decode
 * was dropped, else the least recently used -- with its key out of the index. */
static TexEntry* tex_take(const TexEntry* key)
{
    int i, victim = 0;
    uint64_t oldest = ~0ull;
    TexEntry* e;
    if (g_cache_used < TEX_CACHE) return &g_cache[g_cache_used++];
    for (i = 0; i < TEX_CACHE; i++) {
        if (!g_cache[i].rgba) { victim = i; break; }
        if (g_cache[i].stamp < oldest) { oldest = g_cache[i].stamp; victim = i; }
    }
    e = &g_cache[victim];
    {
        /* Taking an entry that still holds a decode means the working set no
         * longer fits, and that texture is decoded again the next time a draw
         * wants it. What says the cache is too small is the rate, not the
         * total: the heaviest captured frame wants 266 textures, and at a few
         * evictions a frame a running total climbs in ordinary play, so a
         * total would report a scene the cache holds as thrash. Count per
         * frame instead and speak at a quarter of the cache in one frame. */
        if (e->rgba) {
            unsigned frame = gx_frame_count();
            if (frame != g_evict_frame) { g_evict_frame = frame; g_evicted_in_frame = 0; }
            g_evicted++;
            if (++g_evicted_in_frame == TEX_CACHE / 4) {
                static int said;
                if (!said) {
                    said = 1;
                    fprintf(stderr, "[gxr] texture cache: %u decoded textures thrown out of %d slots in frame %u (%u this run), the last %08X (%ux%u fmt %u) to make room for %08X (%ux%u fmt %u); the working set does not fit and textures are being decoded repeatedly\n",
                            g_evicted_in_frame, TEX_CACHE, frame, g_evicted, e->addr, e->w, e->h,
                            e->fmt, key->addr, key->w, key->h, key->fmt);
                }
            }
        }
    }
    tex_free_later(e->rgba);
    e->rgba = NULL;
    e->gen++; /* evicted, or taken for another key */
    e->from_copy = 0;
    tex_index_remove(victim);
    return e;
}

/* The bytes a decode of n levels reads, walked exactly as decode_texture walks
 * them: the base, then each mip at half the size, stopping at 1x1. */
static uint32_t chain_bytes(uint32_t fmt, uint32_t w, uint32_t h, int n)
{
    uint32_t total = 0;
    int l;
    if (n < 1) n = 1;
    if (n > MAX_MIPS) n = MAX_MIPS;
    for (l = 0; l < n; l++) {
        total += texture_bytes(fmt, w, h);
        if (w == 1 && h == 1) break;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return total;
}

static const TexEntry* texture(uint32_t addr, uint32_t fmt, uint32_t w, uint32_t h, uint32_t tlut_off, uint32_t tlut_fmt, int nlevels)
{
    TexEntry key, *e;
    uint64_t hsh;
    int levels, retired = 0;
    if (g_tex_verify < 0) g_tex_verify = getenv("SOA_TEXVERIFY") != NULL;
    g_lookups++;
    /* One key per picture: the address as memory sees it, and no palette for
     * a format that has none -- the TLUT register a draw leaves set is
     * whatever the last palettised draw wanted, and it neither changes these
     * pixels nor goes into their hash. A copy's image is keyed the same way. */
    addr &= MEM_MASK;
    if (fmt != 8 && fmt != 9 && fmt != 10) tlut_off = tlut_fmt = 0;
    key.addr = addr; key.fmt = fmt; key.w = w; key.h = h; key.tlut_off = tlut_off; key.tlut_fmt = tlut_fmt;
    e = tex_find(&key);
    /* A copy's own image (FINDINGS "Copy images"). While the copy that makes
     * it is the newest queued write to these bytes, the draw samples what the
     * copy is decoding in the pool and is fenced there on it: no wait here, no
     * hash, no decode. Once a newer copy writes here, or the drain that ends a
     * frame has retired this one, the texture is read from memory as any other
     * is. A draw that wants mip levels, or SOA_TEXVERIFY, reads memory too. */
    if (e && e->from_copy) {
        long long c = gxr_pending_newest(addr, texture_bytes(fmt, w, h));
        if (c == e->copy_cmd && e->rgba && nlevels <= 1 && !g_tex_verify) {
            g_img_used++;
            if (c + 1 > g_tex_fence) g_tex_fence = c + 1;
            e->stamp = ++g_stamp;
            return e;
        }
        if (c < 0) g_img_retired++;
        else g_img_overwritten++;
        e->from_copy = 0;
        retired = 1;
    }
    /* Wait for any queued copy writing what a decode here could read: every
     * level, not the base alone -- a copy into mip level 1 was waited for only
     * by the drain after every copy until H14 took that away (found by the
     * review of H14). Before anything is freed, so SOA_GXR_DRAIN's drain here
     * cannot land between a free and a decode. */
    levels = e && e->rgba && e->nlevels > nlevels ? e->nlevels : nlevels;
    gxr_texture_hazard(addr, chain_bytes(fmt, w, h, levels));
    if (e && e->rgba) {
        e->stamp = ++g_stamp;
        if (e->hashed != g_tex_epoch || g_tex_verify) {
            hsh = counted_hash(e);
            if (e->hashed == g_tex_epoch && hsh != e->hash) missed_rewrite(e);
            e->hashed = g_tex_epoch;
        } else {
            hsh = e->hash;
        }
        /* rewritten in place, or more levels wanted -- which a replaced
         * texture, one level by design, never is, or it would be decoded
         * again on every draw */
        if (retired || e->hash != hsh || (!e->replaced && e->nlevels < nlevels)) {
            tex_free_later(e->rgba);
            g_decodes++;
            if (retired || e->hash != hsh) g_dec_rewritten++;
            else g_dec_levels++;
            TIMED(T_DECODE, decode_texture(e, nlevels > e->nlevels ? nlevels : e->nlevels));
            e->hash = hsh;
            maybe_replace(e);
        }
        return e;
    }
    /* Not cached, or cached under this key with its decode dropped
     * (tex_invalidate_all, or a decode that could not allocate), in which
     * case the entry and its place in the index are reused. */
    if (!e) {
        e = tex_take(&key);
        e->addr = addr; e->fmt = fmt; e->w = w; e->h = h; e->tlut_off = tlut_off; e->tlut_fmt = tlut_fmt;
        tex_index_add((int)(e - g_cache));
        hsh = counted_hash(e);
        g_dec_new++;
    } else {
        hsh = counted_hash(e);
        g_dec_dropped++;
        if (hsh == e->hash) g_dec_same++;
    }
    g_decodes++;
    TIMED(T_DECODE, decode_texture(e, nlevels));
    e->hash = hsh;
    e->hashed = g_tex_epoch;
    maybe_replace(e);
    e->stamp = ++g_stamp;
    return e;
}

/* A copy to memory is about to make texture (addr, fmt, w, h): give its cache
 * entry a fresh image for the workers to decode their rows into as they copy
 * them, and remember which command makes it (FINDINGS "Copy images"). The
 * decode it replaces goes to the graveyard as any other. Returns the image,
 * or NULL when there is none. */
uint8_t* tex_copy_image(uint32_t addr, uint32_t fmt, uint32_t w, uint32_t h, long long cmd)
{
    TexEntry key, *e;
    uint8_t* img;
    int l;
    /* With a mod's texture provider, none: it is asked with the texture's
     * hash, which a copy's image does not have, so a replacement that matched
     * a copy's bytes would come and go (found by the review). */
    if (fmt > 6 || !w || !h || g_tex_provider) return NULL;
    img = (uint8_t*)malloc((size_t)w * h * 4);
    if (!img) return NULL;
    key.addr = addr & MEM_MASK; key.fmt = fmt; key.w = w; key.h = h; key.tlut_off = 0; key.tlut_fmt = 0;
    e = tex_find(&key);
    if (!e) {
        e = tex_take(&key);
        e->addr = key.addr; e->fmt = fmt; e->w = w; e->h = h; e->tlut_off = 0; e->tlut_fmt = 0;
        tex_index_add((int)(e - g_cache));
    } else {
        tex_free_later(e->rgba);
    }
    e->rgba = img;
    e->gen++;
    e->level[0] = img;
    for (l = 1; l < MAX_MIPS; l++) e->level[l] = NULL;
    e->lw[0] = (int)w; e->lh[0] = (int)h;
    e->nlevels = 1;
    e->replaced = 0;
    e->hashed = 0; /* no hash: once the copy is retired the texture is read from memory again */
    e->from_copy = 1;
    e->copy_cmd = cmd;
    e->stamp = ++g_stamp;
    g_img_made++;
    return img;
}

/* The fence the draw just set up needs, for its command (gxr_draw_inner). */
long long tex_draw_fence(void)
{
    return g_tex_fence;
}

/* For gxr_report (declared there): what the cache did, and what
 * SOA_TEXVERIFY caught. */
void tex_report(void)
{
    if (g_gxr_simd == 0) fprintf(stderr, "[gxr] the pixel path ran without SIMD (SOA_GXR_NOSIMD, or a CPU without SSE4.1)\n");
    if (!g_lookups) return;
    fprintf(stderr, "[gxr] textures: %llu lookups, %llu hashed (%.1f MB), %llu decoded (%llu new, %llu rewritten, %llu for more levels, %llu dropped and made again, %llu of those unchanged), %u evicted from %d entries (%d used); %llu palette loads touched %llu decodes",
            g_lookups, g_hashes, (double)g_hash_bytes / 1e6, g_decodes, g_dec_new, g_dec_rewritten, g_dec_levels,
            g_dec_dropped, g_dec_same, g_evicted, TEX_CACHE, g_cache_used, g_tlut_loads, g_tlut_marked);
    if (g_img_made)
        fprintf(stderr, "; %llu copy images made, sampled by %llu lookups; %llu retired and %llu overwritten before a lookup, which read memory",
                g_img_made, g_img_used, g_img_retired, g_img_overwritten);
    if (g_tex_verify > 0)
        fprintf(stderr, "; SOA_TEXVERIFY: %llu changed inside an epoch\n", g_missed);
    else
        fprintf(stderr, "\n");
}

/* ---- per-draw setup ------------------------------------------------------ */

static uint8_t color_index(unsigned sel, int i)
{
    switch (sel) {
    case 0: return (uint8_t)i;       case 1: return 3;
    case 2: return (uint8_t)(4 + i); case 3: return 7;
    case 4: return (uint8_t)(8 + i); case 5: return 11;
    case 6: return (uint8_t)(12 + i); case 7: return 15;
    case 8: return (uint8_t)(BANK_TEX + i); case 9: return BANK_TEX + 3;
    case 10: return (uint8_t)(BANK_RAS + i); case 11: return BANK_RAS + 3;
    case 12: return BANK_ONE; case 13: return BANK_HALF;
    case 14: return (uint8_t)(BANK_KONST + i); default: return BANK_ZERO;
    }
}

static uint8_t alpha_index(unsigned sel)
{
    switch (sel) {
    case 0: return 3; case 1: return 7; case 2: return 11; case 3: return 15;
    case 4: return BANK_TEX + 3; case 5: return BANK_RAS + 3; case 6: return BANK_KONST + 3; default: return BANK_ZERO;
    }
}

/* Color and konst registers are written through BP 0xE0-0xE7; bit 23 of the
 * RA half says which set. Kept here, latched as the writes arrive. */
static int16_t g_tev_reg[4][4];  /* r g b a, s11 */
static uint8_t g_tev_konst[4][4];

void tev_register_written(uint32_t reg, uint32_t v)
{
    unsigned i = (reg - 0xE0) >> 1;
    int is_konst = (v >> 23) & 1;
    if (reg & 1) { /* BG */
        if (is_konst) { g_tev_konst[i][2] = (uint8_t)(v & 0xFF); g_tev_konst[i][1] = (uint8_t)((v >> 12) & 0xFF); }
        else {
            g_tev_reg[i][2] = (int16_t)((int32_t)((v & 0x7FF) << 21) >> 21);
            g_tev_reg[i][1] = (int16_t)((int32_t)(((v >> 12) & 0x7FF) << 21) >> 21);
        }
    } else { /* RA */
        if (is_konst) { g_tev_konst[i][0] = (uint8_t)(v & 0xFF); g_tev_konst[i][3] = (uint8_t)((v >> 12) & 0xFF); }
        else {
            g_tev_reg[i][0] = (int16_t)((int32_t)((v & 0x7FF) << 21) >> 21);
            g_tev_reg[i][3] = (int16_t)((int32_t)(((v >> 12) & 0x7FF) << 21) >> 21);
        }
    }
}

static int konst_value(unsigned sel, int channel)
{
    if (sel < 8) { int v = (8 - (int)sel) * 32; return v > 255 ? 255 : v; }
    if (sel >= 12 && sel < 16) return channel < 3 ? g_tev_konst[sel - 12][channel] : g_tev_konst[sel - 12][3];
    if (sel >= 16 && sel < 32) return g_tev_konst[(sel - 16) & 3][(sel - 16) >> 2];
    return 0;
}

/* The per-fragment helpers, inlined whether MSVC would or not: the worker
 * profile found sample_level, wrap and the alpha compare as calls of their
 * own (H15c). TEX_INLINE itself is defined above the decoder. */

TEX_INLINE int compare(unsigned mode, int a, int b);
TEX_INLINE int alpha_passes(const TevSetup* T, int alpha);

/* Whether the alpha compare passes every alpha a fragment can have (H15a),
 * which is what lets depth be tested before the TEV: a fragment that fails
 * depth is then discarded whichever order the two run in. Decided by trying
 * all 256, through the same function the pixels use, rather than by reasoning
 * about the modes -- the XOR of two always-true compares is always false, and
 * that is the kind of case a table of modes gets wrong. The answer depends on
 * the compare register alone, and draws share it, so the last one is kept. */
static int alpha_always(const TevSetup* T, uint32_t ac)
{
    static uint32_t last_ac = 0xFFFFFFFFu;
    static int last;
    int a;
    if ((ac & 0xFFFFFFu) == last_ac) return last;
    last_ac = ac & 0xFFFFFFu;
    for (last = 1, a = 0; a < 256 && last; a++) last = alpha_passes(T, a);
    return last;
}

void tev_prepare(const uint32_t* bp, TevSetup* T)
{
    unsigned st, i, j;
    uint32_t ac = bp[0xF3];

    T->stages = ((bp[0] >> 10) & 15) + 1;
    T->used_tex = 0;
    T->used_chan = 0;
    for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) T->reg_init[i][j] = g_tev_reg[i][j];
    T->aref0 = ac & 0xFF; T->aref1 = (ac >> 8) & 0xFF;
    T->acomp0 = (ac >> 16) & 7; T->acomp1 = (ac >> 19) & 7; T->alogic = (ac >> 22) & 3;
    T->alpha_always = alpha_always(T, ac);

    for (st = 0; st < T->stages; st++) {
        Stage* S = &T->st[st];
        uint32_t tref = bp[0x28 + st / 2] >> ((st & 1) * 12);
        uint32_t cenv = bp[0xC0 + 2 * st], aenv = bp[0xC1 + 2 * st];
        uint32_t ksel = bp[0xF6 + st / 2];
        unsigned kc = (st & 1) ? (ksel >> 14) & 31 : (ksel >> 4) & 31;
        unsigned ka = (st & 1) ? (ksel >> 19) & 31 : (ksel >> 9) & 31;
        unsigned rs = aenv & 3, ts = (aenv >> 2) & 3;
        uint32_t k0, k1;
        S->texmap = tref & 7; S->texcoord = (tref >> 3) & 7; S->texen = (tref >> 6) & 1; S->chan = (tref >> 7) & 7;
        S->cd = cenv & 15; S->cc = (cenv >> 4) & 15; S->cb = (cenv >> 8) & 15; S->ca = (cenv >> 12) & 15;
        S->cbias = (cenv >> 16) & 3; S->cop = (cenv >> 18) & 1; S->cclamp = (cenv >> 19) & 1; S->cshift = (cenv >> 20) & 3; S->cdest = (cenv >> 22) & 3;
        S->ad = (aenv >> 4) & 7; S->ac = (aenv >> 7) & 7; S->ab = (aenv >> 10) & 7; S->aa = (aenv >> 13) & 7;
        S->abias = (aenv >> 16) & 3; S->aop = (aenv >> 18) & 1; S->aclamp = (aenv >> 19) & 1; S->ashift = (aenv >> 20) & 3; S->adest = (aenv >> 22) & 3;
        k0 = bp[0xF6 + 2 * rs]; k1 = bp[0xF7 + 2 * rs];
        S->rswap[0] = k0 & 3; S->rswap[1] = (k0 >> 2) & 3; S->rswap[2] = k1 & 3; S->rswap[3] = (k1 >> 2) & 3;
        k0 = bp[0xF6 + 2 * ts]; k1 = bp[0xF7 + 2 * ts];
        S->tswap[0] = k0 & 3; S->tswap[1] = (k0 >> 2) & 3; S->tswap[2] = k1 & 3; S->tswap[3] = (k1 >> 2) & 3;
        for (i = 0; i < 3; i++) S->konst[i] = konst_value(kc, (int)i);
        S->konst[3] = konst_value(ka, 3);
        for (i = 0; i < 3; i++) {
            S->ia[i] = color_index(S->ca, (int)i); S->ib[i] = color_index(S->cb, (int)i);
            S->ic[i] = color_index(S->cc, (int)i); S->id[i] = color_index(S->cd, (int)i);
        }
        S->ja = alpha_index(S->aa); S->jb = alpha_index(S->ab); S->jc = alpha_index(S->ac); S->jd = alpha_index(S->ad);
        if (S->texen) T->used_tex |= 1u << S->texcoord;
        if (S->chan < 2) T->used_chan |= 1u << S->chan;
    }

    /* The one-stage shapes that carry most of the pixels (H15c; a census of
     * the H6 set: the vertex colour alone, about 35% of the area, and texture
     * times vertex colour, about 45%). Only where the general path's every
     * other choice is the plain one -- identity swaps, colour channel 0, no
     * bias, adding, clamped, into the register the output is read from -- so
     * that tev_pixel's direct arithmetic is the general formula with those
     * choices put in, and so the same result. */
    T->fast_c = T->fast_a = 0;
    if (T->stages == 1) {
        const Stage* S = &T->st[0];
        int plain = S->chan == 0 && S->rswap[0] == 0 && S->rswap[1] == 1 && S->rswap[2] == 2 && S->rswap[3] == 3 &&
                    (!S->texen || (S->tswap[0] == 0 && S->tswap[1] == 1 && S->tswap[2] == 2 && S->tswap[3] == 3)) &&
                    S->cbias == 0 && S->cop == 0 && S->cclamp && S->cdest == 0 && S->abias == 0 && S->aop == 0 &&
                    S->aclamp && S->adest == 0 && S->ashift == 0;
        /* GX_CC: 8 TEXC, 10 RASC, 15 ZERO. GX_CA: 4 TEXA, 5 RASA, 6 KONST, 7 ZERO. */
        if (plain) {
            if (S->cshift == 0 && ((S->ca == 10 && S->cb == 15 && S->cc == 15 && S->cd == 15) ||
                                   (S->ca == 15 && S->cb == 15 && S->cc == 15 && S->cd == 10)))
                T->fast_c = 1;
            else if (S->texen && S->cshift <= 1 && S->ca == 15 && S->cb == 8 && S->cc == 10 && S->cd == 15)
                T->fast_c = 2;
            if (S->aa == 6 && S->ab == 7 && S->ac == 7 && S->ad == 7) T->fast_a = 1;
            else if ((S->aa == 5 && S->ab == 7 && S->ac == 7 && S->ad == 7) || (S->aa == 7 && S->ab == 7 && S->ac == 7 && S->ad == 5))
                T->fast_a = 2;
            else if (S->texen && S->aa == 7 && S->ab == 4 && S->ac == 5 && S->ad == 7)
                T->fast_a = 3;
            if (!T->fast_c || !T->fast_a) T->fast_c = T->fast_a = 0;
        }
    }

    /* textures: decode (cached) and resolve sampling state per map used.
     * From here until the loop ends the setup names decoded textures, and a
     * lookup below can flush; g_building is what keeps that flush from freeing
     * the maps this draw has already resolved. */
    for (i = 0; i < 8; i++) T->tex[i].level[0] = NULL;
    g_tex_fence = 0;
    if (g_gxr_simd < 0) simd_decide();
    if (g_notex < 0) g_notex = getenv("SOA_GXR_NOTEX") ? 1 : 0;
    g_building = T;
    for (st = 0; st < T->stages; st++) {
        Stage* S = &T->st[st];
        unsigned map = S->texmap, rb;
        TexCfg* C;
        uint32_t mode0, mode1, image0, image3, tlut, w, h, fmt, addr, tlut_off, tlut_fmt;
        unsigned minf;
        int nlevels, l;
        const TexEntry* te;
        if (!S->texen) continue;
        C = &T->tex[map];
        if (C->level[0]) continue;
        rb = map < 4 ? map : 0x20 + (map - 4);
        mode0 = bp[0x80 + rb]; mode1 = bp[0x84 + rb]; image0 = bp[0x88 + rb]; image3 = bp[0x94 + rb]; tlut = bp[0x98 + rb];
        w = (image0 & 0x3FF) + 1; h = ((image0 >> 10) & 0x3FF) + 1; fmt = (image0 >> 20) & 15;
        addr = (image3 & 0x1FFFFF) << 5;
        tlut_off = (tlut & 0x3FF) << 9; tlut_fmt = (tlut >> 10) & 3;
        minf = (mode0 >> 5) & 7;
        C->mip = (minf == 1 || minf == 2 || minf == 5 || minf == 6);
        C->min_lod = (float)(mode1 & 0xFF) / 16.0f;
        C->max_lod = (float)((mode1 >> 8) & 0xFF) / 16.0f;
        C->lod_bias = (float)(int8_t)((mode0 >> 9) & 0xFF) / 32.0f;
        nlevels = C->mip ? (int)(C->max_lod + 0.999f) + 1 : 1; /* bounded: a byte over 16 (L6) */
        te = texture(addr, fmt, w, h, tlut_off, tlut_fmt, nlevels);
        C->nlevels = te->nlevels;
        C->tex_id = (int)(te - g_cache);
        C->tex_gen = te->gen;
        C->copy_image = (uint8_t)(te->from_copy != 0);
        C->addr = addr;
        C->fmt = (uint8_t)fmt;
        C->tlut_fmt = (uint8_t)tlut_fmt;
        C->replaced = (uint8_t)(te->replaced != 0);
        for (l = 0; l < MAX_MIPS; l++) { C->level[l] = te->level[l]; C->lw[l] = te->lw[l]; C->lh[l] = te->lh[l]; }
        C->w = (int)w; C->h = (int)h;
        C->wrap_s = mode0 & 3; C->wrap_t = (mode0 >> 2) & 3;
        C->linear = (mode0 >> 4) & 1;
        /* SU_SSIZE/TSIZE are indexed by texture *coordinate*, not by map: the SDK
         * writes the dimensions of the map a stage samples into the registers of
         * the coordinate that stage uses (a glyph on map 7 read through coord 0
         * scales by SU0). */
        C->scale_s = (float)((bp[0x30 + 2 * S->texcoord] & 0xFFFF) + 1);
        C->scale_t = (float)((bp[0x31 + 2 * S->texcoord] & 0xFFFF) + 1);
        /* sample()'s factors at level 0, once a draw rather than a division a
         * sample (H15b): the same expression, so the same float. */
        C->su0 = C->scale_s * (float)C->lw[0] / (float)C->w;
        C->sv0 = C->scale_t * (float)C->lh[0] / (float)C->h;
    }
    /* The caller owns the setup from here: it queues the draw without letting
     * anything flush in between, and once queued the queue's own rule covers
     * it. */
    g_building = NULL;
}

/* ---- sampling ----------------------------------------------------------- */

TEX_INLINE int fast_floor(float f)
{
    int i = plat_f2i(f);
    return f < (float)i ? i - 1 : i;
}

TEX_INLINE int wrap(int i, int size, int mask, unsigned mode)
{
    switch (mode) {
    case 0: return i < 0 ? 0 : (i >= size ? size - 1 : i);
    case 1:
        if (mask >= 0) return i & mask;
        i %= size; return i < 0 ? i + size : i;
    default: {
        int period = 2 * size;
        if (mask >= 0) { i &= period - 1; return i < size ? i : period - 1 - i; }
        i %= period;
        if (i < 0) i += period;
        return i < size ? i : period - 1 - i;
    }
    }
}

#ifdef TEX_SIMD
/* sample_level's bilinear loop, four channels at a time (H15d): a lane is a
 * channel. The horizontal pass is one multiply-add of the texel pair by
 * (256 - ax, ax) -- 255 * 256 fits a signed 16-bit lane -- the vertical one
 * 32-bit multiplies; the same integers, so the same bytes. The q are four
 * texels as they sit in memory, the result the blended one.
 *
 * Its own function because _mm_mullo_epi32 is SSE4.1 (L2a): under gcc and
 * clang it carries the target attribute and is a call from the baseline
 * caller; under MSVC it is forced inline, so MSVC's code is what it was. */
#if PLAT_MSVC
#define BILINEAR_SSE41 static __forceinline
#else
#define BILINEAR_SSE41 static PLAT_TARGET_SSE41
#endif
BILINEAR_SSE41 uint32_t bilinear_sse41(uint32_t q00, uint32_t q10, uint32_t q01, uint32_t q11, int ax, int ay)
{
    __m128i z = _mm_setzero_si128(), wx, top, bot, r;
    wx = _mm_set_epi16((short)ax, (short)(256 - ax), (short)ax, (short)(256 - ax), (short)ax, (short)(256 - ax),
                       (short)ax, (short)(256 - ax));
    top = _mm_madd_epi16(_mm_unpacklo_epi8(_mm_unpacklo_epi8(_mm_cvtsi32_si128((int)q00), _mm_cvtsi32_si128((int)q10)), z), wx);
    bot = _mm_madd_epi16(_mm_unpacklo_epi8(_mm_unpacklo_epi8(_mm_cvtsi32_si128((int)q01), _mm_cvtsi32_si128((int)q11)), z), wx);
    r = _mm_add_epi32(_mm_add_epi32(_mm_mullo_epi32(top, _mm_set1_epi32(256 - ay)), _mm_mullo_epi32(bot, _mm_set1_epi32(ay))),
                      _mm_set1_epi32(32768));
    r = _mm_srli_epi32(r, 16);
    r = _mm_packus_epi16(_mm_packus_epi32(r, r), z);
    return (uint32_t)_mm_cvtsi128_si32(r);
}
#endif

TEX_INLINE void sample_level(const TexCfg* C, int l, float u, float v, uint8_t out[4])
{
    const uint8_t* img = C->level[l];
    int w = C->lw[l], h = C->lh[l];
    int mask_s = (w & (w - 1)) == 0 ? w - 1 : -1, mask_t = (h & (h - 1)) == 0 ? h - 1 : -1;
    if (!C->linear) {
        int x = wrap(fast_floor(u), w, mask_s, C->wrap_s), y = wrap(fast_floor(v), h, mask_t, C->wrap_t);
        memcpy(out, img + ((size_t)y * w + x) * 4, 4);
    } else {
        float fu = u - 0.5f, fv = v - 0.5f;
        int x0 = fast_floor(fu), y0 = fast_floor(fv);
        int ax = plat_f2i((fu - (float)x0) * 256.0f), ay = plat_f2i((fv - (float)y0) * 256.0f);
        int xa, xb, ya, yb;
        const uint8_t *p00, *p10, *p01, *p11;
        int i;
        /* A coordinate beyond int range has no fraction: fast_floor gives
         * INT32_MIN, and so does the weight. The SIMD path's 16-bit lanes
         * and the scalar path's int products wrapped that differently, so
         * the same draw came out differently with and without SSE4.1 --
         * and on ARM64, which has only the scalar path. Weight 0 is what
         * the SIMD path already made of it along s; along t it made a
         * value that depended on the texels' parity (L6). */
        if ((unsigned)ax > 255u) ax = 0;
        if ((unsigned)ay > 255u) ay = 0;
        xa = wrap(x0, w, mask_s, C->wrap_s); xb = wrap(x0 + 1, w, mask_s, C->wrap_s);
        ya = wrap(y0, h, mask_t, C->wrap_t); yb = wrap(y0 + 1, h, mask_t, C->wrap_t);
        p00 = img + ((size_t)ya * w + xa) * 4;
        p10 = img + ((size_t)ya * w + xb) * 4;
        p01 = img + ((size_t)yb * w + xa) * 4;
        p11 = img + ((size_t)yb * w + xb) * 4;
#ifdef TEX_SIMD
        if (g_gxr_simd > 0) {
            uint32_t q00, q10, q01, q11;
            memcpy(&q00, p00, 4); memcpy(&q10, p10, 4); memcpy(&q01, p01, 4); memcpy(&q11, p11, 4);
            q00 = bilinear_sse41(q00, q10, q01, q11, ax, ay);
            memcpy(out, &q00, 4);
            return;
        }
#endif
        for (i = 0; i < 4; i++) {
            int top = p00[i] * (256 - ax) + p10[i] * ax;
            int bot = p01[i] * (256 - ax) + p11[i] * ax;
            out[i] = (uint8_t)((top * (256 - ay) + bot * ay + 32768) >> 16);
        }
    }
}

/* The level sample() reads at lod (l, which starts at 0), with s and t scaled
 * to its texels (u, v): the part of sampling a GPU must reproduce bit for bit
 * (loddiff). A macro, because as a function, even forced inline, it moved
 * tev_pixel's register allocation. */
#define SAMPLE_AT(C, s, t, lod, l, u, v)                                       \
    do {                                                                       \
        if ((C)->mip && (C)->nlevels > 1) {                                    \
            float L = (lod) + (C)->lod_bias;                                   \
            if (L < (C)->min_lod) L = (C)->min_lod;                            \
            if (L > (C)->max_lod) L = (C)->max_lod;                            \
            (l) = plat_f2i(L + 0.5f); /* a NaN passes both clamps above */     \
            if ((l) >= (C)->nlevels) (l) = (C)->nlevels - 1;                   \
            if ((l) < 0) (l) = 0;                                              \
        }                                                                      \
        if ((l) == 0) {                                                        \
            (u) = (s) * (C)->su0;                                              \
            (v) = (t) * (C)->sv0;                                              \
        } else {                                                               \
            (u) = (s) * ((C)->scale_s * (float)(C)->lw[l] / (float)(C)->w);    \
            (v) = (t) * ((C)->scale_t * (float)(C)->lh[l] / (float)(C)->h);    \
        }                                                                      \
    } while (0)

int tex_level(const TexCfg* C, float s, float t, float lod, float* u, float* v)
{
    int l = 0;
    float uu, vv;
    SAMPLE_AT(C, s, t, lod, l, uu, vv);
    *u = uu;
    *v = vv;
    return l;
}

/* lod: log2 of texels per pixel at this pixel, from the rasterizer. */
TEX_INLINE void sample(const TexCfg* C, float s, float t, float lod, uint8_t out[4])
{
    int l = 0;
    float u, v;
    if (g_notex > 0) { out[0] = out[1] = out[2] = out[3] = 200; return; } /* read only: tev_prepare decides */
    if (!C->level[0] || C->w <= 0 || C->h <= 0) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    SAMPLE_AT(C, s, t, lod, l, u, v);
    sample_level(C, l, u, v, out);
}

/* ---- TEV ---------------------------------------------------------------- */

static inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
static inline int clamp_s11(int v) { return v < -1024 ? -1024 : (v > 1023 ? 1023 : v); }

TEX_INLINE int compare(unsigned mode, int a, int b)
{
    switch (mode) {
    case 0: return 0;
    case 1: return a < b;
    case 2: return a == b;
    case 3: return a <= b;
    case 4: return a > b;
    case 5: return a != b;
    case 6: return a >= b;
    default: return 1;
    }
}

int g_tev_narrate; /* set by the renderer's SOA_GXR_PIXEL hook: print every stage of one pixel */

/* Runs the stages for one pixel. ras[]: rasterized channel colors 0..255;
 * tex[]: texture coordinates per texcoord slot (s, t, q). */
/* Alpha compare (PE_ALPHA_COMPARE, GXSetAlphaCompare): the pixel path and
 * alpha_always both ask here, so they cannot disagree. */
TEX_INLINE int alpha_passes(const TevSetup* T, int alpha)
{
    int p0 = compare(T->acomp0, alpha, T->aref0), p1 = compare(T->acomp1, alpha, T->aref1);
    switch (T->alogic) {
    case 0: return p0 && p1;
    case 1: return p0 || p1;
    case 2: return p0 != p1;
    default: return p0 == p1;
    }
}

void tev_pixel(const TevSetup* T, const int ras[2][4], const float tex[8][4], uint8_t out[4], int* alpha_pass)
{
    unsigned st, m;
    int bank[BANK_SIZE];
    int i;
    /* The perspective divide, once a texture coordinate rather than twice a
     * stage (H15a): stages that share a coordinate share its s and t, and the
     * division is the same one, so the result is the same float. */
    float sd[8], td[8];
    if (T->fast_c && !g_tev_narrate) { /* SOA_GXR_PIXEL narrates the general path */
        /* A one-stage shape tev_prepare recognised (H15c): the stage's formula
         * -- (a*(256-c') + b*c' + 128) >> 8, plus d, clamped, c' = c + (c >> 7)
         * -- with its constant operands put in. The vertex colour passes
         * through it unchanged (a = RASC with c = 0 gives (a*256 + 128) >> 8,
         * which is a; d = RASC with a = b = c = 0 gives d), and texture times
         * vertex colour is (t*c' + 128) >> 8, doubled when cshift is 1. */
        const Stage* S = &T->st[0];
        const int* r = ras[0];
        uint8_t t[4] = {0, 0, 0, 0};
        if (S->texen) {
            const float* tc = tex[S->texcoord];
            float q = tc[2];
            sample(&T->tex[S->texmap], q != 0.0f ? tc[0] / q : tc[0], q != 0.0f ? tc[1] / q : tc[1], tc[3], t);
        }
        if (T->fast_c == 1) {
            out[0] = (uint8_t)r[0]; out[1] = (uint8_t)r[1]; out[2] = (uint8_t)r[2];
        } else {
            for (i = 0; i < 3; i++) {
                int cc = r[i] + (r[i] >> 7), v = (t[i] * cc + 128) >> 8;
                out[i] = (uint8_t)clamp255(S->cshift == 1 ? v << 1 : v);
            }
        }
        if (T->fast_a == 1) out[3] = (uint8_t)S->konst[3];
        else if (T->fast_a == 2) out[3] = (uint8_t)r[3];
        else {
            int cc = r[3] + (r[3] >> 7);
            out[3] = (uint8_t)clamp255((t[3] * cc + 128) >> 8);
        }
        *alpha_pass = alpha_passes(T, out[3]);
        return;
    }
    for (m = T->used_tex, i = 0; m; m >>= 1, i++) {
        if (m & 1) {
            const float* tc = tex[i];
            float q = tc[2];
            sd[i] = q != 0.0f ? tc[0] / q : tc[0];
            td[i] = q != 0.0f ? tc[1] / q : tc[1];
        }
    }
    memcpy(bank, T->reg_init, sizeof(int) * 16);
    bank[BANK_ONE] = 255; bank[BANK_HALF] = 128; bank[BANK_ZERO] = 0;
    bank[BANK_TEX] = bank[BANK_TEX + 1] = bank[BANK_TEX + 2] = bank[BANK_TEX + 3] = 0;
    bank[BANK_RAS] = bank[BANK_RAS + 1] = bank[BANK_RAS + 2] = bank[BANK_RAS + 3] = 0;

    for (st = 0; st < T->stages; st++) {
        const Stage* S = &T->st[st];
        uint8_t tmp[4];
        int* dc = &bank[S->cdest * 4];

        if (S->texen) {
            sample(&T->tex[S->texmap], sd[S->texcoord], td[S->texcoord], tex[S->texcoord][3], tmp);
            for (i = 0; i < 4; i++) bank[BANK_TEX + i] = tmp[S->tswap[i]];
        }
        if (S->chan < 2) {
            const int* r = ras[S->chan];
            for (i = 0; i < 4; i++) bank[BANK_RAS + i] = r[S->rswap[i]];
        }
        bank[BANK_KONST] = S->konst[0]; bank[BANK_KONST + 1] = S->konst[1];
        bank[BANK_KONST + 2] = S->konst[2]; bank[BANK_KONST + 3] = S->konst[3];

        /* Colour */
        if (S->cbias != 3) {
            int bias = S->cbias == 1 ? 128 : S->cbias == 2 ? -128 : 0;
            int res[3];
            for (i = 0; i < 3; i++) {
                int a = bank[S->ia[i]] & 0xFF, b = bank[S->ib[i]] & 0xFF, c = bank[S->ic[i]] & 0xFF, d = bank[S->id[i]];
                int cc = c + (c >> 7);
                int v = (a * (256 - cc) + b * cc + 128) >> 8;
                int r;
                if (S->cop) v = -v;
                r = d + v + bias;
                if (S->cshift == 1) r <<= 1; else if (S->cshift == 2) r <<= 2; else if (S->cshift == 3) r >>= 1;
                res[i] = S->cclamp ? clamp255(r) : clamp_s11(r);
            }
            dc[0] = res[0]; dc[1] = res[1]; dc[2] = res[2];
        } else {
            unsigned cmp = (S->cshift << 1) | S->cop;
            int a[3], b[3], c[3], d[3], res;
            for (i = 0; i < 3; i++) {
                a[i] = bank[S->ia[i]] & 0xFF; b[i] = bank[S->ib[i]] & 0xFF;
                c[i] = bank[S->ic[i]]; d[i] = bank[S->id[i]];
            }
            switch (cmp >> 1) {
            case 0: res = cmp & 1 ? a[0] == b[0] : a[0] > b[0]; break;
            case 1: { int av = (a[1] << 8) | a[0], bv = (b[1] << 8) | b[0]; res = cmp & 1 ? av == bv : av > bv; break; }
            case 2: { int av = (a[2] << 16) | (a[1] << 8) | a[0], bv = (b[2] << 16) | (b[1] << 8) | b[0]; res = cmp & 1 ? av == bv : av > bv; break; }
            default: res = -1; break;
            }
            for (i = 0; i < 3; i++) {
                int r = res == -1 ? ((cmp & 1 ? a[i] == b[i] : a[i] > b[i]) ? c[i] : 0) : (res ? c[i] : 0);
                r += d[i];
                dc[i] = S->cclamp ? clamp255(r) : clamp_s11(r);
            }
        }
        if (g_tev_narrate)
            fprintf(stderr, "[tev] stage %u: map %u coord %u texel %d,%d,%d,%d ras %d,%d,%d,%d konst %d,%d,%d,%d; color sel %u,%u,%u,%u -> %d,%d,%d (dest %u); alpha sel %u,%u,%u,%u bias %u op %u shift %u (dest %u)\n",
                    st, S->texmap, S->texcoord, bank[BANK_TEX], bank[BANK_TEX + 1], bank[BANK_TEX + 2], bank[BANK_TEX + 3],
                    bank[BANK_RAS], bank[BANK_RAS + 1], bank[BANK_RAS + 2], bank[BANK_RAS + 3], S->konst[0], S->konst[1], S->konst[2], S->konst[3],
                    S->ca, S->cb, S->cc, S->cd, dc[0], dc[1], dc[2], S->cdest, S->aa, S->ab, S->ac, S->ad, S->abias, S->aop, S->ashift, S->adest);
        /* Alpha */
        {
            int* da = &bank[S->adest * 4 + 3];
            if (S->abias != 3) {
                int a = bank[S->ja] & 0xFF, b = bank[S->jb] & 0xFF, c = bank[S->jc] & 0xFF, d = bank[S->jd];
                int cc = c + (c >> 7);
                int v = (a * (256 - cc) + b * cc + 128) >> 8, r;
                if (S->aop) v = -v;
                r = d + v + (S->abias == 1 ? 128 : S->abias == 2 ? -128 : 0);
                if (S->ashift == 1) r <<= 1; else if (S->ashift == 2) r <<= 2; else if (S->ashift == 3) r >>= 1;
                *da = S->aclamp ? clamp255(r) : clamp_s11(r);
            } else {
                unsigned cmp = (S->ashift << 1) | S->aop;
                int a = bank[S->ja] & 0xFF, b = bank[S->jb] & 0xFF, c = bank[S->jc], d = bank[S->jd];
                int res = cmp & 1 ? a == b : a > b;
                *da = S->aclamp ? clamp255(d + (res ? c : 0)) : clamp_s11(d + (res ? c : 0));
            }
        }
    }

    for (i = 0; i < 4; i++) out[i] = (uint8_t)clamp255(bank[i]);

    *alpha_pass = alpha_passes(T, out[3]);
}
