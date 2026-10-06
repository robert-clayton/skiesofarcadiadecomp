/*
 * Software GX: the graphics pipeline in C, driven by the command stream the
 * front end (gx.c) parses. Register numbers follow the hardware; comments
 * name the SDK call that writes each one.
 */
#pragma once
#include "cpu.h"
#include "plat.h"

#define EFB_W 640
#define EFB_H 528

typedef struct {
    float r, g, b, a;
} Color4;

/* One vertex after the transform unit. */
typedef struct {
    float x, y, z, w;   /* clip space */
    float sx, sy;       /* EFB position */
    float depth;        /* 0..1 -> 24-bit z */
    Color4 col[2];      /* lit channel colors, 0..1 */
    float tex[8][3];    /* s, t, q */
} Vertex;

/* Front end (gx.c) */
const uint32_t* gx_cp_regs(void);
const uint32_t* gx_xf_regs(void);
const uint32_t* gx_bp_regs(void);
int gx_draw_list(uint32_t* addr); /* the draw came through a display list, and which */
unsigned long long gx_draw_count(void); /* draws parsed so far */
int gx_list_recording(uint32_t* at);     /* a display list is being recorded, and where its next byte goes */
/* follow the game's display lists: one being recorded, and one parsed in the pieces `called` asks for (gx.c) */
void gx_set_list_hooks(void (*recording)(CpuState* s, uint32_t addr),
                       uint32_t (*called)(CpuState* s, uint32_t addr, uint32_t size, uint32_t done));
/* Frames presented so far, counted from 0: what SOA_FRAMES, SOA_SNAP and
 * SOA_PAD all count. SOA_FRAMES=N therefore runs the frames numbered 0..N-1
 * and stops before presenting frame N -- SOA_SNAP=N SOA_FRAMES=N writes
 * 0000.png and not NNNN.png, and SOA_PAD="N:a" needs SOA_FRAMES>N to fire. */
unsigned gx_frame_count(void);
void gx_set_frame_limit(unsigned frames);
void gx_set_frame_hook(void (*fn)(CpuState*, unsigned)); /* SOA_POKE; see gx.c */

/* Pipeline (gxr.c) */
void gxr_draw(CpuState* s, unsigned op, unsigned count, const uint8_t* verts, unsigned vsize);
void gxr_bp_written(CpuState* s, uint32_t reg, uint32_t value);
int gxr_enabled(void);
void gxr_enable(int on);
void gxr_set_output(const char* png_path);
void gxr_draw_every_frame(void); /* a window is open: do not skip the frames SOA_SNAP is not writing */
void gxr_report(void);
void gxr_reset_efb(void);
void gxr_flush(void);
/* Wait for any queued copy that writes guest memory in [addr, addr + bytes)
 * (PLAN-60FPS-MODS H14): before the producer reads what a copy may be writing.
 * One copy's wait, not a drain, and nothing is recycled. Texture lookups, the
 * front end's sources (vertex arrays, display lists, indexed XF) and the
 * frame-end hooks (SOA_PEEK, SOA_POKE, mods) each have their own reason. */
void gxr_texture_hazard(uint32_t addr, uint32_t bytes);
void gxr_source_hazard(uint32_t addr, uint32_t bytes);
void gxr_hook_hazard(uint32_t addr, uint32_t bytes);
/* Frame pairs (PLAN-60FPS-MODS H10): RECORD keeps each draw's key and
 * positions as frame F's and makes them the frame before at the screen copy;
 * LERP moves each draw matched in the frame before to (1-t)*F + t*F+1 and
 * skips copies to texture. 0 turns it off. Returns 0 when refused. */
#define GXR_PAIR_RECORD 1u
#define GXR_PAIR_LERP 2u
int gxr_pair_mode(unsigned flags, float t);
unsigned long long gxr_pair_rotations(void);
void gxr_pair_list(void* file); /* a FILE*: "F-index F+1-index" per match, fifopair's numbering */
void gxr_pair_report(void);
/* A mod's filter on GXSetProjection's six parameters (mod.c, M3c); NULL removes it. */
void gxr_set_projection_filter(void (*fn)(float p[6], int orthographic));
/* A mod's texture provider (mod.c, M3c): return 1 with a w x h RGBA8 image to
 * replace the texture with that source hash; the image is copied at once. */
void gxr_set_texture_provider(int (*fn)(uint64_t hash, uint32_t fmt, uint32_t w, uint32_t h, const uint8_t* rgba,
                                        const uint8_t** out, uint32_t* out_w, uint32_t* out_h));
const uint8_t* gxr_screen(int* w, int* h);           /* the last frame copied out (RGBA, EFB_W stride) */
/* FNV-1a over that frame's pixels, the value SOA_HASH prints as
 * "[gxr] frame <n> <w>x<h> hash <16 hex digits>" once per presented frame.
 * Call it after gxr_flush(): a frame is only complete once the workers are. */
uint64_t gxr_screen_hash(void);

/* TEV / textures (gxr_tev.c) */

/* Per-pixel input bank: 0-15 the four colour registers (r,g,b,a each),
 * 16-19 texture colour, 20-23 rasterized colour, 24-27 konst, 28 one,
 * 29 half, 30 zero. Selectors resolve to bank indices once per draw. */
#define BANK_TEX 16
#define BANK_RAS 20
#define BANK_KONST 24
#define BANK_ONE 28
#define BANK_HALF 29
#define BANK_ZERO 30
#define BANK_SIZE 32

typedef struct {
    uint8_t texmap, texcoord, texen, chan;
    uint8_t rswap[4], tswap[4];
    uint8_t ca, cb, cc, cd, aa, ab, ac, ad;
    uint8_t cbias, cop, cclamp, cshift, cdest;
    uint8_t abias, aop, aclamp, ashift, adest;
    int konst[4];
    uint8_t ia[3], ib[3], ic[3], id[3]; /* colour input bank indices per channel */
    uint8_t ja, jb, jc, jd;             /* alpha input bank indices */
} Stage;

#define MAX_MIPS 11

typedef struct {
    const uint8_t* level[MAX_MIPS]; /* decoded RGBA per mip level; level[0] NULL = unused */
    int lw[MAX_MIPS], lh[MAX_MIPS];
    int nlevels;
    int w, h;
    unsigned wrap_s, wrap_t;
    int linear, mip;        /* bilinear within a level; pick a level by lod */
    float lod_bias, min_lod, max_lod;
    float scale_s, scale_t;
    float su0, sv0; /* scale_s * lw[0] / w and scale_t * lh[0] / h: sample()'s factors at level 0, once a draw (H15b) */
    /* For a backend (V2): the cache slot (0-1023), a generation that moves
     * on every decode, replacement, copy image and eviction of that slot,
     * and whether the decode is a copy's image, which a GPU makes itself.
     * They tell a backend a texture changed without hashing it. */
    int tex_id;
    uint32_t tex_gen;
    uint8_t copy_image;
} TexCfg;

typedef struct {
    unsigned stages;
    Stage st[16];
    TexCfg tex[8];
    unsigned used_tex;   /* bit per texcoord slot read by an enabled stage */
    unsigned used_chan;  /* bit per rasterized channel read */
    int aref0, aref1;
    unsigned acomp0, acomp1, alogic;
    int alpha_always; /* the alpha compare passes every alpha 0-255, so depth can be tested first (H15a) */
    /* A one-stage shape tev_pixel runs directly (H15c), 0 when none: colour 1
     * the vertex colour, 2 texture x vertex colour (cshift 0 or 1); alpha 1
     * konst, 2 vertex alpha, 3 texture x vertex alpha. */
    unsigned fast_c, fast_a;
    int reg_init[4][4];
} TevSetup;

void tev_prepare(const uint32_t* bp, TevSetup* out);
void tev_pixel(const TevSetup* T, const int ras[2][4], const float tex[8][4], uint8_t out[4], int* alpha_pass);
void tev_register_written(uint32_t reg, uint32_t v);
/* For tools (loddiff): the level a sample at lod reads, and s, t scaled to its
 * texels, exactly as the sampler finds them; and span_lod's level of detail
 * at (px, py), from the planes of W, S, T and Q in that order. */
int tex_level(const TexCfg* C, float s, float t, float lod, float* u, float* v);
float gxr_span_lod(const float planes[4][3], float px, float py, float scale_s, float scale_t);
void tex_invalidate_all(void);
void tex_graveyard_empty(void); /* frees textures no queued draw can reference any more */
int tex_graveyard_full(void);
void tex_set_memory(CpuState* s);
void tex_epoch_advance(void); /* texture memory may have changed: hash each texture again at its next use */
void tmem_load_tlut(CpuState* s, uint32_t src, uint32_t tmem_off, uint32_t bytes);
/* Copy images (FINDINGS "Copy images"): the producer gives a copy's texture a
 * fresh image at the copy, the workers decode each row into it as they copy
 * it, and a draw sampling it while the copy is queued is fenced on the copy in
 * the pool. gxr_pending_newest is the newest queued copy writing into a range,
 * or -1; tex_draw_fence is the fence the draw just set up needs. */
uint8_t* tex_copy_image(uint32_t addr, uint32_t fmt, uint32_t w, uint32_t h, long long cmd);
void tex_decode_row(uint8_t* out, const uint8_t* base, uint32_t fmt, uint32_t w, uint32_t y);
long long tex_draw_fence(void);
long long gxr_pending_newest(uint32_t addr, uint32_t bytes);

extern uint8_t g_efb[EFB_H][EFB_W][4];
extern uint32_t g_efb_z[EFB_H][EFB_W];

int png_write_rgba(const char* path, const uint8_t* rgba, int w, int h, int stride);

/* ---- phase accounting ----------------------------------------------------
 *
 * One word says which phase the thread that parses the command stream is in,
 * and every boundary reads the clock once and charges the stretch since the
 * last reading to the phase being left. Two things follow from that shape
 * rather than from arithmetic afterwards. The buckets are disjoint, because
 * one word holds one value: entering decode stops charging prepare for the
 * duration, and leaving it resumes. And they sum to the span between the
 * first reading and the last, because the readings telescope -- each one
 * closes one bucket and opens the next. The timers this replaces nested,
 * decode inside prepare inside draw, and printed as siblings, which is why
 * they added up to more than the run.
 *
 * The state is plain rather than per-thread because every site that switches
 * it runs on the producer: gxr_flush is producer-only by the queue's rules,
 * tev_prepare and decode_texture are reached from it, and a worker uses the
 * busy/idle pair in gxr.c instead. T_HOST is the residual -- translated guest
 * code, the device models it calls, and the command-stream parse. The parse
 * is not split out here because a clock pair at a gather-pipe store costs
 * more than the store: the sampler in main.c splits it instead, off a marker
 * that is two plain stores.
 */
typedef enum {
    T_HOST,    /* not in the renderer: guest code, device models, the parse */
    T_SETUP,   /* gxr_draw: vertex decode and transform into a queued command */
    T_PREPARE, /* tev_prepare, less the texture decodes inside it */
    T_DECODE,  /* decode_texture */
    T_COPY,    /* enqueue_copy, less the PNG and the waits inside it */
    T_PNG,     /* write_frame_png */
    T_WAIT,    /* gxr_flush: the producer waiting for the workers to catch up */
    T_RASTER,  /* draw_command on the producer, when there are no workers */
    T_COUNT
} GxrPhase;

extern uint64_t g_gxr_ticks[T_COUNT]; /* charged by the switch below */
extern uint64_t g_gxr_phase_last;     /* when the producer entered its current phase */
extern int g_gxr_phase;               /* which phase that is; read by the sampler */
extern int g_gxr_tsc;                 /* -1 undecided, 1 the TSC is usable, 0 use QPC */

void gxr_timing_init(void);         /* decide the clock and fix the origin */
void gxr_timing_finish(void);       /* fix the tick rate; gxr_report calls it */
uint64_t gxr_qpc(void);             /* plat_mono_raw: QueryPerformanceCounter, or CLOCK_MONOTONIC ns */
double gxr_seconds(uint64_t ticks); /* meaningful once gxr_timing_finish has run */
double gxr_producer_span(void);     /* seconds the buckets above partition */
double gxr_clock(void);             /* gxr_qpc as seconds */

/* About 7 ns a read against QueryPerformanceCounter's 17, which matters
 * because the worker loop reads it twice per queued command and a long run
 * queues millions of them. Only where the counter is invariant (CPUID
 * 80000007 EDX bit 8 says it runs at a constant rate across cores and power
 * states); otherwise QPC, whose 100 ns step quantises one command's
 * busy/idle split but not their sum, since the endpoints telescope. Not a
 * serializing instruction, so an out-of-order core can move it by a few
 * instructions: noise over regions of 100 ns and up, and nothing here
 * measures anything shorter. plat_cycles is the TSC on x86-64 and the generic
 * timer on ARM64; gxr_timing_finish measures its rate against gxr_qpc. */
static inline uint64_t gxr_ticks(void)
{
    if (g_gxr_tsc > 0) return plat_cycles();
    return gxr_qpc();
}

/* Switch to phase p and return the phase left, so a caller can put it back. */
static inline int gxr_phase(int p)
{
    int old = g_gxr_phase;
    uint64_t n;
    if (g_gxr_tsc < 0) gxr_timing_init();
    n = gxr_ticks();
    /* A thread moved to a core whose counter is behind reads backwards.
     * Charge nothing rather than an interval the width of the wrap. */
    if (n > g_gxr_phase_last) g_gxr_ticks[old] += n - g_gxr_phase_last;
    g_gxr_phase_last = n;
    g_gxr_phase = p;
    return old;
}

#define TIMED(which, stmt) do { int _ph = gxr_phase(which); stmt; gxr_phase(_ph); } while (0)
