/*
 * The window and the sound of a host build (soa_host.h; tools/recompile.py
 * --split --host): the program that loaded the runtime is the window. It
 * reads the frames, sets the pads and pulls the sound through soa_host.h,
 * and this file gives si.c, main.c and audio_out.c what window_sdl.c and
 * audio_sdl.c give them in the launcher. Built only when SOA_HOST is defined,
 * which is never alongside SOA_SDL.
 *
 * Everything the host and the guest share is behind one of two locks, held
 * for a copy and no longer: the pads (the host writes, si.c reads at each
 * poll) and the sound queue (audio_out.c writes on the guest's thread, the
 * host's mixer reads on its own).
 */
#ifdef SOA_HOST
#ifndef SOA_SPLIT
#error "a host build is a split build: tools/recompile.py --split --host"
#endif
#define _CRT_SECURE_NO_WARNINGS
#include "soa_host.h"
#include "gxr.h"
#include "gxr_export.h"
#include "ninja.h"
#include "plat.h"
#include <fenv.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int soa_load_game(const char* path, char* why, size_t cap); /* game.c */
int soa_main(int argc, char** argv);                        /* main.c's main, under SOA_SPLIT */
long gxr_presented(void);
void watchdog_fallback(void);
void si_set_motor_sink(void (*fn)(unsigned speed));
void si_set_motor_window(int open);
void clock_pause(int on);
void audio_set_muted(int on);

static plat_a32 g_state = SOA_HOST_IDLE;
static volatile int g_rc;
static char g_disc[1024];

/* ---- the run ---------------------------------------------------------------- */

/* A thread the host made may carry the host's floating-point modes -- an
 * engine can turn on flush-to-zero for its own threads -- and every thread
 * the run makes inherits them from this one. The translated code is checked
 * against the defaults, which a launched process starts with. */
static void run_main(void* arg)
{
    static char name[] = "soa";
    char* argv[3];
    (void)arg;
    argv[0] = name;
    argv[1] = g_disc;
    argv[2] = NULL;
    fesetenv(FE_DFL_ENV);
    g_rc = soa_main(g_disc[0] ? 2 : 1, argv);
    fprintf(stderr, "[host] the run returned %d\n", g_rc);
    plat_cas32(&g_state, SOA_HOST_RUNNING, SOA_HOST_ENDED);
}

unsigned soa_host_abi(void)
{
    return SOA_HOST_ABI;
}

int soa_host_start(const SoaHostStart* start, char* why, size_t cap)
{
    PlatThread t;
    if (!start || start->abi != SOA_HOST_ABI) {
        snprintf(why, cap, "the host was built for host ABI %u and this runtime is %u", start ? start->abi : 0u,
                 SOA_HOST_ABI);
        return 1;
    }
    if (!start->game || !*start->game) {
        snprintf(why, cap, "no game library named (libsoa_game.so from the same build)");
        return 1;
    }
    if (plat_cas32(&g_state, SOA_HOST_IDLE, SOA_HOST_RUNNING) != SOA_HOST_IDLE) {
        snprintf(why, cap, "a run has already started in this process; the runtime has one run per process");
        return 1;
    }
    if (start->disc && strlen(start->disc) >= sizeof g_disc) {
        snprintf(why, cap, "the disc path is longer than %zu bytes", sizeof g_disc - 1);
        goto refused;
    }
    if (soa_load_game(start->game, why, cap) != 0) goto refused;
    /* The switches the run reads, set as soa.ini's are: only where the
     * environment has not set them already. */
    if (start->root && *start->root) {
        char card[1200];
        plat_setenv("SOA_ROOT", start->root);
        snprintf(card, sizeof card, "%s" PLAT_SEP "build" PLAT_SEP "cards" PLAT_SEP "slotA.raw", start->root);
        if (!getenv("SOA_CARD")) plat_setenv("SOA_CARD", card);
    }
    if (!getenv("SOA_RENDER")) plat_setenv("SOA_RENDER", "1"); /* a host wants the picture */
    snprintf(g_disc, sizeof g_disc, "%s", start->disc ? start->disc : "");
    if (!plat_thread_start(&t, run_main, NULL, 0)) {
        snprintf(why, cap, "cannot start the run's thread");
        goto refused;
    }
    plat_thread_detach(&t);
    return 0;
refused:
    plat_cas32(&g_state, SOA_HOST_RUNNING, SOA_HOST_IDLE);
    return 1;
}

int soa_host_state(int* rc)
{
    int s = plat_load32(&g_state);
    if (s == SOA_HOST_ENDED && rc) *rc = g_rc;
    return s;
}

/* ---- the picture -------------------------------------------------------------- */

long soa_host_frame(const uint8_t** rgba, int* w, int* h, int* stride)
{
    int fw, fh;
    const uint8_t* p = gxr_screen(&fw, &fh);
    if (rgba) *rgba = p;
    if (w) *w = fw;
    if (h) *h = fh;
    if (stride) *stride = EFB_W * 4;
    return gxr_presented();
}

/* ---- the pads ------------------------------------------------------------------- */

typedef struct {
    int present;
    SoaHostPad pad;
} HostPort;

static PlatLock g_pad_lock;
static HostPort g_ports[2];

void soa_host_set_pad(unsigned port, const SoaHostPad* pad)
{
    if (port < 1 || port > 2) return;
    plat_lock(&g_pad_lock);
    g_ports[port - 1].present = pad != NULL;
    if (pad) g_ports[port - 1].pad = *pad;
    plat_unlock(&g_pad_lock);
}

static int read_port(unsigned i, uint16_t* buttons, uint8_t stick[2], uint8_t cstick[2], uint8_t trig[2])
{
    HostPort p;
    plat_lock(&g_pad_lock);
    p = g_ports[i];
    plat_unlock(&g_pad_lock);
    if (!p.present) return 0;
    *buttons = p.pad.buttons;
    memcpy(stick, p.pad.stick, 2);
    memcpy(cstick, p.pad.cstick, 2);
    memcpy(trig, p.pad.trig, 2);
    return 1;
}

/* ---- the motor -------------------------------------------------------------------- */

static plat_a64 g_motor;

static void motor(unsigned speed)
{
    plat_xchg64(&g_motor, (int64_t)speed);
}

unsigned soa_host_motor(void)
{
    return (unsigned)plat_load64(&g_motor);
}

void soa_host_pause(int on)
{
    clock_pause(on);
}

void skip_request(unsigned frame, unsigned speed); /* main.c */
void soa_host_skip_to(unsigned frame, unsigned speed)
{
    skip_request(frame, speed);
}

void soa_host_mute(int on)
{
    audio_set_muted(on);
}

/* ---- what main.c and si.c ask of a window ------------------------------------------- */

/* main.c calls this when the run wants a window, which in a host build means
 * the host's: a run that SOA_PAD or SOA_SNAP leave headless never gets here.
 * The launcher of a host build has no host, so it is headless after all, and
 * the watchdog main.c stood down for the window comes back. */
void window_start(void)
{
    if (plat_load32(&g_state) == SOA_HOST_IDLE) {
        fprintf(stderr, "[host] no host has started this run; it continues headless\n");
        watchdog_fallback();
        return;
    }
    si_set_motor_sink(motor);
    si_set_motor_window(1);
    fprintf(stderr, "[host] the host shows the frames and gives the input\n");
}

int window_open(void)
{
    return plat_load32(&g_state) != SOA_HOST_IDLE;
}

int window_toggle_fullscreen(void)
{
    return 0; /* the host's window is the host's to size */
}

int window_pad(uint16_t* buttons, uint8_t stick[2], uint8_t cstick[2], uint8_t trig[2])
{
    return read_port(0, buttons, stick, cstick, trig);
}

int window_pad2(uint16_t* buttons, uint8_t stick[2], uint8_t cstick[2], uint8_t trig[2])
{
    return read_port(1, buttons, stick, cstick, trig);
}

int window_host(uint16_t* host)
{
    (void)host;
    return 0; /* no chords (CH1): the host has its own menus */
}

/* ---- the sound ------------------------------------------------------------------------
 * audio_out.c's device: a queue the host empties, holding what the
 * launcher's device may fall behind by -- 24 blocks of 4096 bytes -- so a
 * block is dropped where it would be dropped there. */
#define QUEUE_BYTES (24u * 4096u)

static PlatLock g_audio_lock;
static uint8_t g_queue[QUEUE_BYTES];
static unsigned g_head, g_used; /* bytes: where the oldest starts, and how many */
static plat_a64 g_rate;

int audio_host_open(unsigned rate)
{
    plat_xchg64(&g_rate, (int64_t)rate);
    fprintf(stderr, "[audio] output open at %u Hz, for the host to pull\n", rate);
    return 1;
}

/* Little-endian left/right pairs into the queue; 0 when they do not fit. */
int audio_host_put(const int16_t* lr, unsigned bytes)
{
    const uint8_t* src = (const uint8_t*)lr;
    unsigned tail, first;
    plat_lock(&g_audio_lock);
    if (g_used + bytes > QUEUE_BYTES) {
        plat_unlock(&g_audio_lock);
        return 0;
    }
    tail = (g_head + g_used) % QUEUE_BYTES;
    first = bytes < QUEUE_BYTES - tail ? bytes : QUEUE_BYTES - tail;
    memcpy(g_queue + tail, src, first);
    memcpy(g_queue, src + first, bytes - first);
    g_used += bytes;
    plat_unlock(&g_audio_lock);
    return 1;
}

unsigned soa_host_audio(int16_t* lr, unsigned frames, unsigned* rate)
{
    uint8_t* dst = (uint8_t*)lr;
    unsigned bytes, first;
    if (rate) *rate = (unsigned)plat_load64(&g_rate);
    if (!lr) return 0;
    plat_lock(&g_audio_lock);
    bytes = g_used / 4u < frames ? g_used / 4u * 4u : frames * 4u;
    first = bytes < QUEUE_BYTES - g_head ? bytes : QUEUE_BYTES - g_head;
    memcpy(dst, g_queue + g_head, first);
    memcpy(dst + first, g_queue, bytes - first);
    g_head = (g_head + bytes) % QUEUE_BYTES;
    g_used -= bytes;
    plat_unlock(&g_audio_lock);
    return bytes / 4u;
}
/* ---- the game's models (ninja.h's feed) -------------------------------------
 * Two frames' worth of records: the guest fills one while the host may copy
 * the other. A frame is published when the game ends it (main.c tells us),
 * so what the host reads is always whole, and is the frame whose picture
 * the renderer is presenting. */
#define MAX_MODELS 8192
#define MAX_TEXTURES 16384
#define MAX_LIGHTS 1024
#define MAX_OPEN 8 /* drawers inside drawers; none are seen, but a begin and its end must still pair */
#define MAX_PASSES 16
#define MAX_KEPT (512u * 1024u) /* vertex lists copied as their models are drawn: a few, of a few KB each */
#define MAX_TINTS 65536u
#define MAX_ALPHA 16384u
#define MAX_ALPHA_VERTS 393216u
#define MAX_ALPHA_STAGES 32768u
#define MAX_FLAT 2048u
#define MAX_FLAT_VERTS 16384u
#define FLAT_TEXTURES 1024u     /* the renderer's texture cache has this many slots (TexCfg.tex_id) */

typedef struct {
    long frame;
    unsigned n, nt, nl;
    unsigned open[MAX_OPEN], n_open; /* the models whose drawers have not returned yet */
    unsigned sent;                   /* how many of them have reached the GPU: the next one's place in that order */
    unsigned np, np_skipped;         /* the screen passes drawn after them, and the ones left out */
    SoaHostScreenPass pass[MAX_PASSES];
    float proj[7];
    uint32_t visit[MAX_MODELS];      /* ninja.c's number for each, which climbs with them */
    SoaHostModel m[MAX_MODELS];
    SoaHostTexture t[MAX_TEXTURES];
    SoaHostLight l[MAX_LIGHTS];
    unsigned nb;
    uint8_t bytes[MAX_KEPT];
    unsigned ntint;
    uint32_t tint[MAX_TINTS];
    unsigned drawing;                /* the model whose strips are reaching the GPU now, or ~0u */
    unsigned na, nav, na_skipped;    /* the see-through phase: draws, their vertices, and the draws left out */
    SoaHostAlphaDraw alpha[MAX_ALPHA];
    SoaHostAlphaVertex av[MAX_ALPHA_VERTS];
    unsigned nas;
    SoaHostAlphaStage as[MAX_ALPHA_STAGES];
    unsigned nf, nfv, nf_skipped;    /* the 2D layer: draws, their vertices, and the draws left out */
    unsigned nf_under;               /* how many of the draws came before the first model: the backdrop */
    uint32_t soft_proj[8][2];        /* the projections (XF 0x1024, 0x1025) a soft see-through draw has written */
    unsigned n_soft;                 /* depth under this frame: solid strips after it there are drawn in order */
    SoaHostFlatDraw flat[MAX_FLAT];
    SoaHostFlatVertex fv[MAX_FLAT_VERTS];
} ModelFrame;

static ModelFrame g_mf[2];
static int g_build;                  /* the guest's; the other is the host's */
static int g_ready = -1;             /* the published one, under g_mf_lock */
static PlatLock g_mf_lock;
static uint8_t* volatile g_mem;      /* MEM1, as the hooks see it */
static unsigned long long g_mf_dropped;

/* The frame being built, begun afresh when the game has moved on to another.
 * A frame is worth publishing when it has a model, or -- a menu, the title
 * -- only draws on the screen. */
static ModelFrame* building_frame(void)
{
    long f = (long)gx_frame_count();
    ModelFrame* b = &g_mf[g_build];
    if (b->frame != f) {
        if (b->n || b->nf || b->na) {
            plat_lock(&g_mf_lock);
            g_ready = g_build;
            plat_unlock(&g_mf_lock);
            g_build ^= 1;
            b = &g_mf[g_build];
        }
        memset(b->proj, 0, sizeof b->proj);
        b->proj[6] = 1.0f; /* none seen yet */
        b->frame = f;
        b->n = b->nt = b->nl = b->n_open = b->sent = b->np = b->np_skipped = b->nb = 0;
        b->nf = b->nfv = b->nf_skipped = b->ntint = b->na = b->nav = b->na_skipped = b->nas = b->nf_under = b->n_soft = 0;
        b->drawing = ~0u;
    }
    return b;
}

static ModelFrame* building(CpuState* s)
{
    g_mem = s->mem;
    return building_frame();
}

void host_frame_end(unsigned frame)
{
    ModelFrame* b = &g_mf[g_build];
    if ((!b->n && !b->nf && !b->na) || b->frame != (long)frame) return; /* a frame with nothing in it leaves the last one standing */
    plat_lock(&g_mf_lock);
    g_ready = g_build;
    plat_unlock(&g_mf_lock);
    g_build ^= 1;
    g_mf[g_build].n = 0; /* building() starts it afresh at the next model */
    g_mf[g_build].frame = -1;
}

static void feed_model(CpuState* s, const NinjaVisit* v)
{
    ModelFrame* b = building(s);
    SoaHostModel* m;
    const uint32_t* xf = gx_xf_regs();
    /* The 3D projection: GX's, whenever it is a perspective one. A frame's
     * first model can arrive while the last frame's 2D one is still loaded,
     * since Ninja sets its own as it draws. */
    if (!(xf[0x1026] & 1)) {
        int i;
        for (i = 0; i < 6; i++) memcpy(&b->proj[i], &xf[0x1020 + i], 4);
        b->proj[6] = 0.0f;
    }
    /* What was drawn on the screen before the frame's first model lies
     * under the scene, and is marked so: a host's 2D layer goes over. (A
     * frame with no model at all has no under: there it is the picture.) */
    if (!b->n) {
        unsigned k;
        for (k = 0; k < b->nf; k++) b->flat[k].under = 1;
        b->nf_under = b->nf;
    }
    /* a model past the array still opens, so its end pairs with it */
    if (b->n_open < MAX_OPEN) b->open[b->n_open] = b->n < MAX_MODELS ? b->n : ~0u;
    b->n_open++;
    if (b->n >= MAX_MODELS) { g_mf_dropped++; return; }
    b->visit[b->n] = v->visit;
    b->drawing = b->n; /* until its drawer returns; what it records into a list is drawn later (feed_sending) */
    m = &b->m[b->n++];
    memset(m, 0, sizeof *m);
    m->model = v->model;
    m->drawer = v->drawer;
    m->mode = v->mode;
    m->strip_and = v->strip_and;
    m->strip_or = v->strip_or;
    m->order = SOA_HOST_NOT_SENT;
    m->vlist = v->vlist;
    m->plist = v->plist;
    m->first_texture = b->nt;
    m->textures = 0;
    memcpy(m->modelview, v->modelview, sizeof m->modelview);
    memcpy(m->camera, v->camera, sizeof m->camera);
    /* a vertex list the game is about to refill for the next model */
    if (v->vlist_bytes) {
        uint32_t off = v->vlist & 0x01FFFFFFu;
        if (b->nb + v->vlist_bytes <= MAX_KEPT && off + (uint64_t)v->vlist_bytes <= MEM1_SIZE) {
            memcpy(b->bytes + b->nb, s->mem + off, v->vlist_bytes);
            m->vertices = b->nb;
            m->vertices_bytes = v->vlist_bytes;
            b->nb += v->vlist_bytes;
        } else {
            g_mf_dropped++;
        }
    }
}

static void feed_texture(CpuState* s, uint32_t id, uint32_t image, uint32_t format, uint32_t palette, uint32_t width,
                         uint32_t height, const char* name)
{
    ModelFrame* b = building(s);
    SoaHostTexture* t;
    if (!b->n) return; /* outside any model */
    if (b->nt >= MAX_TEXTURES) { g_mf_dropped++; return; }
    t = &b->t[b->nt++];
    t->id = id;
    t->image = image;
    t->format = format;
    t->palette = palette;
    t->width = (uint16_t)width;
    t->height = (uint16_t)height;
    t->levels = 1;
    t->lod_bias = 0;
    t->min_lod = t->max_lod = 0;
    snprintf(t->name, sizeof t->name, "%s", name);
    b->m[b->n - 1].textures++;
}

/* A light's sixteen XF words from its fourth on: the colour, then twelve
 * floats. */
static void read_light(const uint32_t* xf, unsigned index, SoaHostLight* out)
{
    const uint32_t* L = xf + 0x600 + 16 * index;
    out->index = index;
    out->colour = L[3];
    memcpy(out->a, L + 4, sizeof out->a);
    memcpy(out->k, L + 7, sizeof out->k);
    memcpy(out->position, L + 10, sizeof out->position);
    memcpy(out->direction, L + 13, sizeof out->direction);
}

/* A model's strips have reached the GPU: GX's registers are as the last of
 * them left them, which is how the model was shaded, and it takes the next
 * place in the order the frame's models got there. */
static void feed_strips(ModelFrame* b, SoaHostModel* m, unsigned strips)
{
    const uint32_t* xf = gx_xf_regs();
    const uint32_t* bp = gx_bp_regs();
    SoaHostLight found[8];
    unsigned mask, i, n = 0;
    m->strips = strips;
    m->order = b->sent++;
    m->channels = xf[0x1009] & 3;
    m->chan_colour = xf[0x100E];
    m->chan_alpha = xf[0x1010];
    m->ambient = xf[0x100A];
    m->material = xf[0x100C];
    m->tev_stages = ((bp[0x00] >> 10) & 15) + 1;
    m->tev_colour = bp[0xC0] & 0x00FFFFFFu;
    m->tev_alpha = bp[0xC1] & 0x00FFFFFFu;
    for (i = 0; i < 5; i++) m->fog[i] = bp[0xEE + i] & 0x00FFFFFFu;
    {
        float p4, p5;
        memcpy(&p4, &xf[0x1024], 4);
        memcpy(&p5, &xf[0x1025], 4);
        m->near_clip = (xf[0x1026] & 1) || p4 == 1.0f ? 0.0f : p5 / (p4 - 1.0f);
        m->far_clip = (xf[0x1026] & 1) || p4 == 0.0f ? 0.0f : p5 / p4;
    }
    m->depth = bp[0x40] & 0x1Fu;
    if (!m->channels || !(m->chan_colour & 2)) return;
    mask = ((m->chan_colour >> 2) & 15) | (((m->chan_colour >> 11) & 15) << 4);
    for (i = 0; i < 8; i++)
        if ((mask >> i) & 1) read_light(xf, i, &found[n++]);
    /* a frame has a few sets of lights and most models share one: a set
     * already in the frame's table is used again, so the same lights are the
     * same place in it */
    for (i = 0; n && i + n <= b->nl; i++)
        if (memcmp(&b->l[i], found, n * sizeof *found) == 0) break;
    if (n && i + n <= b->nl) {
        m->first_light = i;
        m->lights = n;
    } else if (n && b->nl + n <= MAX_LIGHTS) {
        memcpy(&b->l[b->nl], found, n * sizeof *found);
        m->first_light = b->nl;
        m->lights = n;
        b->nl += n;
    } else if (n) {
        g_mf_dropped++;
    }
}

/* The drawer returns. What it drew at once is at the GPU now; what GX
 * recorded into a display list gets there when feed_sent says so. */
static void feed_end(CpuState* s, unsigned visit, int drawn, int recorded, unsigned strips)
{
    ModelFrame* b = &g_mf[g_build];
    SoaHostModel* m;
    unsigned index;
    (void)s;
    (void)visit;
    b->drawing = ~0u;
    if (!b->n_open) return; /* watching began inside a drawer */
    b->n_open--;
    if (b->n_open >= MAX_OPEN || (index = b->open[b->n_open]) >= b->n) return;
    m = &b->m[index];
    m->drawn = drawn ? 1u : 0u;
    if (!recorded && strips) feed_strips(b, m, strips);
}

/* Where a visit is among the frame's records, or ~0u. */
static unsigned visit_index(const ModelFrame* b, unsigned visit)
{
    unsigned lo = 0, hi = b->n;
    while (lo < hi) { /* the visits' numbers climb */
        unsigned mid = (lo + hi) / 2;
        if (b->visit[mid] < visit) lo = mid + 1;
        else hi = mid;
    }
    return lo < b->n && b->visit[lo] == visit ? lo : ~0u;
}

/* The parser is about to run the strips a visit recorded. */
static void feed_sending(CpuState* s, unsigned visit)
{
    ModelFrame* b = &g_mf[g_build];
    (void)s;
    b->drawing = b->frame == (long)gx_frame_count() ? visit_index(b, visit) : ~0u;
}

/* The display list a visit's strips were recorded into is called, and the
 * parser is past the last of them. */
static void feed_sent(CpuState* s, unsigned visit, unsigned strips)
{
    ModelFrame* b = &g_mf[g_build];
    unsigned lo = 0, hi = b->n;
    (void)s;
    b->drawing = ~0u;
    if (b->frame != (long)gx_frame_count() || !strips) return;
    while (lo < hi) { /* the visits' numbers climb */
        unsigned mid = (lo + hi) / 2;
        if (b->visit[mid] < visit) lo = mid + 1;
        else hi = mid;
    }
    if (lo < b->n && b->visit[lo] == visit) feed_strips(b, &b->m[lo], strips);
}

static const NinjaFeed g_ninja_feed = {feed_model, feed_texture, feed_end, feed_sending, feed_sent};

/* ---- what the game draws over the whole screen ------------------------------------------------
 * The renderer shows every draw to an exporter (gxr_export.h); this one
 * keeps the quads that cover the screen, drawn after the frame's models. */

/* One of a one-stage combiner's inputs, 0 to 1: GX_CC_* for colour (each of
 * three channels), GX_CA_* for alpha. */
static float tev_input(const TevSetup* T, const Stage* S, unsigned sel, int alpha, unsigned ch, const float ras[4],
                       const float tex[4])
{
    static const uint8_t alpha_sel[8] = {1, 3, 5, 7, 9, 11, 14, 15}; /* GX_CA_* as the GX_CC_* that reads the same alpha */
    if (alpha) sel = alpha_sel[sel & 7], ch = 3;
    switch (sel) {
    case 0: case 2: case 4: case 6: return (float)T->reg_init[sel / 2][ch] / 255.0f;
    case 1: case 3: case 5: case 7: return (float)T->reg_init[sel / 2][3] / 255.0f;
    case 8: return tex[S->tswap[ch] & 3];
    case 9: return tex[S->tswap[3] & 3];
    case 10: return ras[S->rswap[ch] & 3];
    case 11: return ras[S->rswap[3] & 3];
    case 12: return 1.0f;
    case 13: return 0.5f;
    case 14: return (float)S->konst[ch] / 255.0f;
    default: return 0.0f;
    }
}

/* What a one-stage combiner makes of a vertex colour and a texel, RGBA
 * packed; *ok cleared for a shape not worked out here. A stage in compare
 * mode (bias 3) gives d + c where a > b and d elsewhere; of those only the
 * comparison of the red channels is done, and when what a is compared with
 * is the texture, *step is a: the texel value the outcome turns at. */
static uint32_t tev_one_stage(const TevSetup* T, const float ras[4], const float tex[4], int* ok, uint32_t* step)
{
    static const float bias[3] = {0.0f, 0.5f, -0.5f}, scale[4] = {1.0f, 2.0f, 4.0f, 0.5f};
    const Stage* S = &T->st[0];
    uint32_t out = 0;
    unsigned ch;
    int compare = S->cbias == 3;
    if (S->abias > 2 || (compare && (S->cshift != 0 || S->cop != 0))) { *ok = 0; return 0; }
    for (ch = 0; ch < 4; ch++) {
        int al = ch == 3;
        float a = tev_input(T, S, al ? S->aa : S->ca, al, ch, ras, tex), b = tev_input(T, S, al ? S->ab : S->cb, al, ch, ras, tex);
        float c = tev_input(T, S, al ? S->ac : S->cc, al, ch, ras, tex), d = tev_input(T, S, al ? S->ad : S->cd, al, ch, ras, tex);
        float v;
        if (compare && !al) {
            float ar = tev_input(T, S, S->ca, 0, 0, ras, tex), br = tev_input(T, S, S->cb, 0, 0, ras, tex);
            v = d + (ar > br ? c : 0.0f);
            if (step && S->cb == 8 && S->ca != 8) *step = (uint32_t)(ar * 255.0f + 0.5f);
        } else {
            float mix = a * (1.0f - c) + b * c;
            v = (d + ((al ? S->aop : S->cop) ? -mix : mix) + bias[al ? S->abias : S->cbias]) * scale[(al ? S->ashift : S->cshift) & 3];
        }
        v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
        out |= (uint32_t)(v * 255.0f + 0.5f) << (24 - 8 * ch);
    }
    return out;
}

static float g_view[4096][3];

/* Each vertex of the draw being built, in view space as the transform unit
 * makes it: for model_draw. */
static void screen_vertex(unsigned i, const VertexIn* in, const Vertex* out)
{
    const uint32_t* xf = gx_xf_regs();
    unsigned row = 4 * (in->posidx & 0x3F), r;
    (void)out;
    if (i >= 4096u) return;
    for (r = 0; r < 3; r++) {
        float mtx[4];
        memcpy(mtx, &xf[row + 4 * r], sizeof mtx);
        g_view[i][r] = mtx[0] * in->pos[0] + mtx[1] * in->pos[1] + mtx[2] * in->pos[2] + mtx[3];
    }
}

/* The textures of the 2D layer and the see-through phase as the renderer
 * decoded them, kept here so the host's thread never reads the renderer's
 * cache: copied when a draw uses one whose image has changed, under the
 * frames' lock.
 *
 * A texture is a slot of the renderer's cache and a generation that moves
 * when the slot's image does. The host asks for one after the frame that
 * named it is published, by which time the game may have put another image
 * in the slot: so a slot's last few images are kept, each under its
 * generation, the one unused longest making room. */
#define KEPT_IMAGES 4u
typedef struct {
    uint32_t gen;    /* the number the host knows it by: the slot's own count of images, from 1 */
    uint32_t seen;   /* the renderer's generation it was last seen under */
    uint32_t hash;
    unsigned frame;  /* the last frame a draw used it in */
    int w, h;
    int levels;      /* how many levels of detail rgba holds, the image first */
    int soft;        /* some texel is neither clear nor solid: a layer drawn with it is part see-through */
    size_t bytes;
    uint8_t* rgba;
} FlatTexture;
static FlatTexture g_flat_tex[FLAT_TEXTURES][KEPT_IMAGES];
static uint32_t g_flat_count[FLAT_TEXTURES];

/* Whether a texture can be kept, and keeping it: its number for the host
 * and its generation. */
static int keepable(const TexCfg* C)
{
    return C->level[0] && C->tex_id >= 0 && (unsigned)C->tex_id < FLAT_TEXTURES && C->lw[0] > 0 && C->lh[0] > 0 && C->lw[0] <= 1024 &&
           C->lh[0] <= 1024;
}

/* The generation handed out is this file's, not the renderer's, which
 * moves whenever a slot is decoded again and so nearly every frame for
 * some textures whose texels never change: a host would make a new texture
 * of its own each time. Here an image decoded again is known by its texels
 * and keeps its number. */
/* The levels of detail a draw samples: the image alone unless its filter
 * for a texture seen small is a mipmapped one, and then no more than its
 * own limit lets it reach. The renderer's cache may hold more levels than
 * that, decoded for another draw of the same image or from whatever lies
 * after an image that has none: a plant's leaves drawn from those were a
 * burst of yellow. */
static int sampled_levels(const TexCfg* C)
{
    int l, n = C->mip && C->nlevels > 1 ? C->nlevels : 1, most = C->max_lod > 0.0f ? (int)(C->max_lod + 0.999f) + 1 : 1;
    if (n > most) n = most;
    for (l = 1; l < n; l++)
        if (!C->level[l] || C->lw[l] <= 0 || C->lh[l] <= 0) return l;
    return n;
}

static int keep_texture(const TexCfg* C, uint32_t* texture, uint32_t* gen)
{
    FlatTexture* slot = g_flat_tex[C->tex_id];
    FlatTexture* ft = &slot[0];
    unsigned k, now = gx_frame_count();
    int l, levels = sampled_levels(C);
    size_t bytes = (size_t)C->lw[0] * (size_t)C->lh[0] * 4u, at, all = 0;
    uint32_t hash = 2166136261u, word;
    *texture = (uint32_t)C->tex_id + 1u;
    for (l = 0; l < levels; l++) all += (size_t)C->lw[l] * (size_t)C->lh[l] * 4u;
    for (k = 0; k < KEPT_IMAGES; k++)
        if (slot[k].rgba && slot[k].seen == C->tex_gen && slot[k].w == C->lw[0] && slot[k].h == C->lh[0] && slot[k].levels == levels) {
            slot[k].frame = now; /* only this thread writes these; the host's reads the texels, under the lock */
            *gen = slot[k].gen;
            return slot[k].soft;
        }
    for (at = 0; at < bytes; at += 4) {
        memcpy(&word, C->level[0] + at, 4);
        hash = (hash ^ word) * 16777619u;
    }
    for (k = 0; k < KEPT_IMAGES; k++) {
        if (slot[k].rgba && slot[k].hash == hash && slot[k].w == C->lw[0] && slot[k].h == C->lh[0] && slot[k].levels == levels &&
            memcmp(slot[k].rgba, C->level[0], bytes) == 0) {
            slot[k].seen = C->tex_gen;
            slot[k].frame = now;
            *gen = slot[k].gen;
            return slot[k].soft;
        }
        if (!slot[k].rgba || (ft->rgba && (int)(slot[k].frame - ft->frame) < 0)) ft = &slot[k];
    }
    plat_lock(&g_mf_lock);
    if (!ft->rgba || ft->bytes != all) {
        free(ft->rgba);
        ft->rgba = (uint8_t*)malloc(all);
    }
    if (ft->rgba) {
        for (l = 0, at = 0; l < levels; l++) {
            memcpy(ft->rgba + at, C->level[l], (size_t)C->lw[l] * (size_t)C->lh[l] * 4u);
            at += (size_t)C->lw[l] * (size_t)C->lh[l] * 4u;
        }
        ft->levels = levels;
        ft->bytes = all;
        ft->gen = ++g_flat_count[C->tex_id];
        ft->seen = C->tex_gen;
        ft->hash = hash;
        ft->frame = now;
        ft->w = C->lw[0];
        ft->h = C->lh[0];
        ft->soft = 0;
        for (at = 3; at < bytes && !ft->soft; at += 4) ft->soft = C->level[0][at] > 8 && C->level[0][at] < 247;
    }
    plat_unlock(&g_mf_lock);
    *gen = ft->gen;
    return ft->soft;
}

/* ---- a model's draws: tints, and the see-through phase ------------------------------------------
 * What the whole combiner makes of each vertex of a draw with the model's
 * own texture (GX's map 0, where Ninja puts it) white, and with it black:
 * the renderer's own combiner works both out, other textures looked up at
 * the vertex and all. */
#define DRAW_VERTS 4096u
static uint32_t g_white[DRAW_VERTS], g_black[DRAW_VERTS];

static void combine_vertices(const DrawCmd* D, unsigned count)
{
    static const uint8_t white[4] = {255, 255, 255, 255}, black[4] = {0, 0, 0, 0};
    static TevSetup T; /* the guest's thread only; too big for its stack */
    TexCfg* own;
    unsigned i, k;
    T = D->tev;
    own = &T.tex[0];
    memset(own, 0, sizeof *own);
    own->lw[0] = own->lh[0] = own->w = own->h = own->nlevels = 1;
    own->scale_s = own->scale_t = own->su0 = own->sv0 = 1.0f;
    for (i = 0; i < count; i++) {
        const Vertex* p = &D->v[i];
        int ras[2][4];
        float tex[8][4];
        uint8_t out[4];
        int pass = 1;
        for (k = 0; k < 2; k++) {
            ras[k][0] = (int)(p->col[k].r * 255.0f + 0.5f);
            ras[k][1] = (int)(p->col[k].g * 255.0f + 0.5f);
            ras[k][2] = (int)(p->col[k].b * 255.0f + 0.5f);
            ras[k][3] = (int)(p->col[k].a * 255.0f + 0.5f);
        }
        for (k = 0; k < 8; k++) {
            tex[k][0] = p->tex[k][0];
            tex[k][1] = p->tex[k][1];
            tex[k][2] = p->tex[k][2];
            tex[k][3] = 0.0f;
        }
        own->level[0] = white;
        tev_pixel(&T, ras, tex, out, &pass);
        g_white[i] = (uint32_t)out[0] << 24 | (uint32_t)out[1] << 16 | (uint32_t)out[2] << 8 | out[3];
        own->level[0] = black;
        tev_pixel(&T, ras, tex, out, &pass);
        g_black[i] = (uint32_t)out[0] << 24 | (uint32_t)out[1] << 16 | (uint32_t)out[2] << 8 | out[3];
    }
}

/* A draw under a perspective projection: a model's strip, or one of the few
 * no model makes.
 *
 * A strip drawn with more than one combiner stage gives its model tints.
 *
 * And a draw that isn't solid belongs to the see-through phase, which a
 * host can't leave to opaque meshes of its own: the game blends these in
 * the order it draws them and they test and write depth, so one hides what
 * comes after it and behind it. They are handed over as they are drawn:
 * the triangles the game's culling leaves, in view space, a colour a
 * vertex, with the blend, the alpha test, the depth mode and the fog. Solid
 * is a constant alpha of one, no alpha test, and a blend that then replaces
 * what is there. */
/* How a model's draw samples its texture when it is seen small goes on
 * the model's record of that texture, found by the image GX was given: a
 * palettized one's record starts at its palette. Of two strips that
 * differ, the one that reads more levels is kept. */
static void texture_lod(ModelFrame* b, const SoaHostModel* m, const TexCfg* C)
{
    uint32_t image = 0x80000000u | ((gx_bp_regs()[0x94] & 0x001FFFFFu) << 5);
    int levels = sampled_levels(C);
    unsigned k;
    for (k = 0; k < m->textures && m->first_texture + k < b->nt; k++) {
        SoaHostTexture* t = &b->t[m->first_texture + k];
        uint32_t palette = t->format == 8 ? 32u : t->format == 9 ? 512u : 0u;
        if ((t->image | 0x80000000u) + palette != image || levels <= t->levels) continue;
        t->levels = (uint8_t)levels;
        t->lod_bias = (int8_t)(C->lod_bias * 32.0f);
        t->min_lod = (uint8_t)(C->min_lod * 16.0f);
        t->max_lod = (uint8_t)(C->max_lod * 16.0f);
    }
}

static void model_draw(ModelFrame* b, const DrawCmd* D, unsigned count)
{
    const Stage* S = &D->tev.st[0];
    const TexCfg* C = NULL;
    const Vertex* v = D->v;
    SoaHostModel* m = b->drawing < b->n ? &b->m[b->drawing] : NULL;
    SoaHostAlphaDraw* a;
    unsigned i, t, tris, coord = 0, src = D->px.sfac, dst = D->px.dfac, order[3], kept = 0, offsets = 0;
    unsigned place[3] = {0, 0, 0}, place_map[3] = {0, 0, 0}, places = 0;
    const TexCfg* C1 = NULL;
    int plain, constant, tex_alpha = 0, program, late = 0, solid = 0, soft = 0;
    uint32_t proj[2];
    unsigned strip = m ? m->solid_strips + m->alpha_strips : 0u; /* which of the visit's strips this is */
    if (D->efb || !D->px.col_upd) return;
    /* A see-through layer that is part clear and writes depth hides what is
     * drawn later behind it, solid or not, and is blended with what was
     * there before: a night's black smoke is drawn before the sky behind
     * it, and the sky never shows through. A host that draws solid things
     * first has that the wrong way round, so the solid strips drawn after
     * such a layer, under the projection it wrote depth under, are handed
     * over with the see-through draws, in order. */
    proj[0] = gx_xf_regs()[0x1024]; proj[1] = gx_xf_regs()[0x1025];
    for (i = 0; i < b->n_soft && !late; i++) late = b->soft_proj[i][0] == proj[0] && b->soft_proj[i][1] == proj[1];
    /* the strip of eight zeroes the game sends between real ones */
    for (i = 0; count == 8 && i < 8 && v[i].x == 0.0f && v[i].y == 0.0f && v[i].w == 0.0f; i++) {}
    if (count == 8 && i == 8) return;
    switch (D->prim) {
    case 0x80: tris = (count / 4) * 2; break;
    case 0x90: tris = count / 3; break;
    case 0x98: case 0xA0: tris = count >= 3 ? count - 2 : 0; break;
    default: return; /* lines and points */
    }
    for (i = 0; i < D->tev.stages; i++)
        if (D->tev.st[i].texen && D->tev.st[i].texmap == 0) { C = &D->tev.tex[0]; coord = D->tev.st[i].texcoord & 7; break; }
    if (!m && !C) return; /* no model's and no texture: a shadow volume, which the screen passes account for */
    if (m && C) texture_lod(b, m, C);
    /* a frame with no model yet has had no projection noted: the first of its own */
    if (b->proj[6] != 0.0f) {
        for (i = 0; i < 6; i++) memcpy(&b->proj[i], &gx_xf_regs()[0x1020 + i], 4);
        b->proj[6] = 0.0f;
    }
    if (!D->px.blend_en) src = 1, dst = 0;
    plain = (src == 4 && dst == 5) || (src == 1 && dst == 0);
    /* the usual solid strip, told without working its combiner out */
    constant = D->tev.alpha_always && D->tev.stages == 1 && S->aa == 6 && S->ab == 7 && S->ac == 7 && S->ad == 7 && S->konst[3] == 255;
    if (m && constant && plain && !D->px.logic_en) {
        if (!late) { m->solid_strips++; return; }
        solid = 1; /* solid, and drawn after something it may lie behind: in order, with the see-through draws */
    }
    if (!tris || count > DRAW_VERTS) { g_mf_dropped++; return; }
    combine_vertices(D, count);
    if (m && D->tev.stages > 1) {
        if (!m->tint_vertices) m->tints = b->ntint;
        if (m->tints + m->tint_vertices == b->ntint && b->ntint + count <= MAX_TINTS) {
            for (i = 0; i < count; i++) {
                b->tint[b->ntint + i] = g_white[i];
                if (g_black[i] >> 8) m->tint_offsets++;
            }
            b->ntint += count;
            m->tint_vertices += count;
        } else {
            g_mf_dropped++;
        }
    }
    for (i = 0, constant = D->tev.alpha_always; constant && i < count; i++) constant = (g_black[i] & 255u) == 255u;
    if (m && constant && plain && !D->px.logic_en) {
        if (!late) { m->solid_strips++; return; }
        solid = 1; /* solid, and drawn after something it may lie behind: in order, with the see-through draws */
    }
    if (m) m->alpha_strips++;
    /* what a host's one blend can't do, and a texture it can't be handed */
    if (D->px.logic_en || src == 2 || src == 3 || src > 5 || dst > 5 || (C && !keepable(C)) || b->na >= MAX_ALPHA ||
        b->nav + tris * 3 > MAX_ALPHA_VERTS) {
        b->na_skipped++;
        return;
    }
    a = &b->alpha[b->na];
    memset(a, 0, sizeof *a);
    a->first_vertex = b->nav;
    /* Whether the combiner can be handed over whole: its stages read at
     * most GX's first two textures, at three places a vertex between them,
     * and none compares. (A sky is one texture times a table of colours looked up at
     * two places: worked out a vertex and spread across the triangle, as
     * what can't be handed over is, its clouds came out blue.) */
    program = D->tev.stages <= 8 && b->nas + D->tev.stages <= MAX_ALPHA_STAGES;
    for (i = 0; program && i < D->tev.stages; i++) {
        const Stage* T = &D->tev.st[i];
        if (T->texen) {
            /* one of two textures (GX's maps 0 and 1), at one of three places a vertex */
            unsigned map = T->texmap & 7, k = T->texcoord & 7, p;
            if (map > 1 || !keepable(&D->tev.tex[map])) { program = 0; break; }
            for (p = 0; p < places && !(place[p] == k && place_map[p] == map); p++) {}
            if (p == places) {
                if (places == 3) { program = 0; break; }
                place[places] = k;
                place_map[places++] = map;
            }
            if (map == 1) C1 = &D->tev.tex[1];
        }
        if (T->cbias == 3 || T->abias == 3) program = 0;
    }
    if (!program) C1 = NULL;
    if (program) {
        a->first_stage = b->nas;
        a->stages = D->tev.stages;
        for (i = 0; i < D->tev.stages; i++) {
            const Stage* T = &D->tev.st[i];
            SoaHostAlphaStage* o = &b->as[b->nas++];
            o->colour = (uint32_t)T->cd | (uint32_t)T->cc << 4 | (uint32_t)T->cb << 8 | (uint32_t)T->ca << 12 | (uint32_t)T->cbias << 16 |
                        (uint32_t)(T->cop & 1) << 18 | (uint32_t)(T->cclamp & 1) << 19 | (uint32_t)(T->cshift & 3) << 20 | (uint32_t)(T->cdest & 3) << 22;
            o->alpha = (uint32_t)T->ad | (uint32_t)T->ac << 4 | (uint32_t)T->ab << 8 | (uint32_t)T->aa << 12 | (uint32_t)T->abias << 16 |
                       (uint32_t)(T->aop & 1) << 18 | (uint32_t)(T->aclamp & 1) << 19 | (uint32_t)(T->ashift & 3) << 20 | (uint32_t)(T->adest & 3) << 22;
            o->konst = (uint32_t)(T->konst[0] & 255) << 24 | (uint32_t)(T->konst[1] & 255) << 16 | (uint32_t)(T->konst[2] & 255) << 8 | (uint32_t)(T->konst[3] & 255);
            o->flags = (T->texen ? 1u : 0u) | (T->chan == 0 ? 2u : 0u) | (T->chan == 1 ? 4u : 0u);
            if (T->texen) {
                unsigned p;
                for (p = 0; p + 1 < places && !(place[p] == (unsigned)(T->texcoord & 7) && place_map[p] == (unsigned)(T->texmap & 7)); p++) {}
                o->flags |= ((T->texmap & 7) == 1 ? 8u : 0u) | p << 4;
                o->flags |= (uint32_t)(T->tswap[0] & 3) << 8 | (uint32_t)(T->tswap[1] & 3) << 10 | (uint32_t)(T->tswap[2] & 3) << 12 | (uint32_t)(T->tswap[3] & 3) << 14;
            }
            if (T->chan < 2)
                o->flags |= (uint32_t)(T->rswap[0] & 3) << 16 | (uint32_t)(T->rswap[1] & 3) << 18 | (uint32_t)(T->rswap[2] & 3) << 20 | (uint32_t)(T->rswap[3] & 3) << 22;
        }
        for (i = 0; i < 4; i++)
            for (t = 0; t < 4; t++) a->registers[i][t] = (int16_t)D->tev.reg_init[i][t];
    }
    for (t = 0; t < tris; t++) {
        const Vertex *p0, *p1, *p2;
        float det;
        if (D->prim == 0x80) order[0] = 4 * (t / 2), order[1] = order[0] + 1 + (t & 1), order[2] = order[0] + 2 + (t & 1);
        else if (D->prim == 0x90) order[0] = 3 * t, order[1] = order[0] + 1, order[2] = order[0] + 2;
        else if (D->prim == 0x98) order[0] = t + (t & 1), order[1] = t + 1 - (t & 1), order[2] = t + 2; /* as the rasteriser alternates a strip */
        else order[0] = 0, order[1] = t + 1, order[2] = t + 2;
        /* the rasteriser's cull, on the triangle's area on the screen: its
         * sign is this determinant's, whichever side of the eye a corner is */
        p0 = &v[order[0]]; p1 = &v[order[1]]; p2 = &v[order[2]];
        det = (p0->x * (p1->y * p2->w - p2->y * p1->w) - p0->y * (p1->x * p2->w - p2->x * p1->w) + p0->w * (p1->x * p2->y - p2->x * p1->y)) *
              D->rc.wd * D->rc.ht;
        if (det == 0.0f || (D->rc.cull == 1 && det < 0.0f) || (D->rc.cull == 2 && det > 0.0f) || D->rc.cull == 3) continue;
        for (i = 0; i < 3; i++) {
            const Vertex* p = &v[order[i]];
            SoaHostAlphaVertex* o = &b->av[b->nav + 3 * kept + i];
            memcpy(&o->x, g_view[order[i]], sizeof g_view[0]);
            o->u = o->v = o->u1 = o->v1 = o->u2 = o->v2 = 0.0f;
            if (program) {
                /* each place over the texture a stage samples there */
                float* uv[3] = {&o->u, &o->u1, &o->u2};
                unsigned q;
                for (q = 0; q < places; q++) {
                    const TexCfg* M = &D->tev.tex[place_map[q]];
                    uv[q][0] = p->tex[place[q]][0] * M->scale_s / (float)(M->w > 0 ? M->w : 1);
                    uv[q][1] = p->tex[place[q]][1] * M->scale_t / (float)(M->h > 0 ? M->h : 1);
                }
            } else if (C) {
                o->u = p->tex[coord][0] * C->scale_s / (float)(C->w > 0 ? C->w : 1);
                o->v = p->tex[coord][1] * C->scale_t / (float)(C->h > 0 ? C->h : 1);
            }
            o->colour = g_white[order[i]];
            o->colour1 = 0;
            if (program) { /* the vertex's two lit colours, for the stages to work on */
                o->colour = (uint32_t)(p->col[0].r * 255.0f + 0.5f) << 24 | (uint32_t)(p->col[0].g * 255.0f + 0.5f) << 16 |
                            (uint32_t)(p->col[0].b * 255.0f + 0.5f) << 8 | (uint32_t)(p->col[0].a * 255.0f + 0.5f);
                o->colour1 = (uint32_t)(p->col[1].r * 255.0f + 0.5f) << 24 | (uint32_t)(p->col[1].g * 255.0f + 0.5f) << 16 |
                             (uint32_t)(p->col[1].b * 255.0f + 0.5f) << 8 | (uint32_t)(p->col[1].a * 255.0f + 0.5f);
            }
            if (g_black[order[i]] >> 8) offsets++;
            if ((g_black[order[i]] & 255u) != (g_white[order[i]] & 255u)) tex_alpha = 1;
        }
        kept++;
    }
    if (!kept) {
        if (program) b->nas -= D->tev.stages;
        return;
    }
    a->vertices = kept * 3;
    a->model = m ? b->drawing : ~0u;
    a->src_factor = src;
    a->dst_factor = dst;
    a->subtract = D->px.blend_en && D->px.subtract ? 1u : 0u;
    a->texture_alpha = (uint8_t)tex_alpha;
    a->depth = (uint8_t)((D->px.z_en ? 1u : 0u) | (D->px.z_func & 7u) << 1 | (D->px.z_en && D->px.z_upd ? 16u : 0u));
    a->alpha_comp0 = (uint8_t)D->tev.acomp0;
    a->alpha_comp1 = (uint8_t)D->tev.acomp1;
    a->alpha_logic = (uint8_t)D->tev.alogic;
    a->alpha_ref0 = (uint8_t)D->tev.aref0;
    a->alpha_ref1 = (uint8_t)D->tev.aref1;
    a->offsets = offsets;
    for (i = 0; i < 5; i++) a->fog[i] = gx_bp_regs()[0xEE + i] & 0x00FFFFFFu;
    {
        /* the planes GX clips this draw to: its projection's own, which a
         * frame has several of (the sky's reaches twenty times as far) */
        float p4, p5;
        memcpy(&p4, &gx_xf_regs()[0x1024], 4);
        memcpy(&p5, &gx_xf_regs()[0x1025], 4);
        a->near_clip = p4 != 1.0f ? p5 / (p4 - 1.0f) : 0.0f;
        a->far_clip = p4 != 0.0f ? p5 / p4 : 0.0f;
    }
    for (i = 0; i < count && !soft; i++) soft = (g_white[i] & 255u) > 8u && (g_white[i] & 255u) < 247u;
    if (dst == 1) soft = 1; /* (added on: what is behind shows through all of it) */
    a->passes_before = (uint8_t)(b->np < 255 ? b->np : 255);
    a->strip = (uint16_t)(strip < 65535u ? strip : 65535u);
    a->strip_vertices = (uint16_t)count;
    a->cull = (uint8_t)D->rc.cull;
    a->chan_colour = gx_xf_regs()[0x100E];
    a->chan_alpha = gx_xf_regs()[0x1010];
    a->ambient = gx_xf_regs()[0x100A];
    a->material = gx_xf_regs()[0x100C];
    {
        /* The strip's own coordinates and nothing else: the one place its
         * stages sample at is GX's regular generation from the first
         * texture coordinate, through a matrix that changes nothing, with no
         * second one either (the game leaves GX's second transform on, with
         * a matrix of the same kind). And its stages read the first lit
         * colour only. (A table of colours looked up by the light is
         * generated; a scrolled texture is not: the game rewrites the
         * strip's coordinates for that.) */
        const uint32_t* xf = gx_xf_regs();
        int own = m != NULL && D->prim == 0x98 && program && places <= 1 && strip < 65535u;
        for (i = 0; own && i < D->tev.stages; i++) own = D->tev.st[i].chan != 1;
        if (own && places == 1) {
            uint32_t info = xf[0x1040 + place[0]], post = xf[0x1050 + place[0]];
            unsigned row[2], n = 1, q, r, c;
            static const float same[2][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}};
            row[0] = 4u * ((place[0] < 4 ? xf[0x1018] >> (6 + 6 * place[0]) : xf[0x1019] >> (6 * (place[0] - 4))) & 0x3Fu);
            own = place_map[0] == 0 && ((info >> 4) & 7u) == 0 && ((info >> 7) & 31u) == 5u;
            if (xf[0x1012] & 1u) {
                row[n++] = 0x500u + 4u * (post & 0x3Fu);
                own = own && !((post >> 8) & 1u);
            }
            for (q = 0; own && q < n; q++)
                for (r = 0; own && r < 2; r++)
                    for (c = 0; own && c < 4; c++) {
                        float f;
                        memcpy(&f, &xf[row[q] + 4 * r + c], 4);
                        own = f == same[r][c];
                    }
        }
        a->plain = (uint8_t)own;
    }
    if (C) {
        if (keep_texture(C, &a->texture, &a->texture_gen) && tex_alpha) soft = 1;
        a->width = (uint16_t)C->lw[0];
        a->height = (uint16_t)C->lh[0];
        a->wrap_s = (uint8_t)C->wrap_s;
        a->wrap_t = (uint8_t)C->wrap_t;
        a->linear = (uint8_t)(C->linear != 0);
        a->levels = (uint8_t)sampled_levels(C);
        a->lod_bias = (int8_t)(C->lod_bias * 32.0f);
        a->min_lod = (uint8_t)(C->min_lod * 16.0f);
        a->max_lod = (uint8_t)(C->max_lod * 16.0f);
        a->texels = C->copy_image || C->replaced ? 0u : 0x80000000u | C->addr;
        a->format = C->fmt;
        a->palette = C->tlut_fmt;
    }
    if (C1) {
        keep_texture(C1, &a->texture1, &a->texture1_gen);
        a->width1 = (uint16_t)C1->lw[0];
        a->height1 = (uint16_t)C1->lh[0];
        a->wrap1_s = (uint8_t)C1->wrap_s;
        a->wrap1_t = (uint8_t)C1->wrap_t;
        a->linear1 = (uint8_t)(C1->linear != 0);
        a->levels1 = (uint8_t)sampled_levels(C1);
        a->lod_bias1 = (int8_t)(C1->lod_bias * 32.0f);
        a->texels1 = C1->copy_image || C1->replaced ? 0u : 0x80000000u | C1->addr;
        a->format1 = C1->fmt;
        a->palette1 = C1->tlut_fmt;
    }
    if (!solid && !late && soft && (a->depth & 16u) && b->n_soft < 8) {
        b->soft_proj[b->n_soft][0] = proj[0];
        b->soft_proj[b->n_soft++][1] = proj[1];
    }
    b->nav += kept * 3;
    b->na++;
}

/* A flat draw that isn't a screen pass: one of the 2D layer's. */
static void flat_draw(ModelFrame* b, const DrawCmd* D, unsigned count)
{
    const Stage* S = &D->tev.st[0];
    const Vertex* v = D->v;
    const TexCfg* C = S->texen ? &D->tev.tex[S->texmap & 7] : NULL;
    SoaHostFlatDraw* f;
    float lo[4] = {0, 0, 0, 0}, hi[4] = {1, 1, 1, 1};
    unsigned i, tris, src = D->px.sfac, dst = D->px.dfac, order[3], t;
    uint32_t colour0 = 0;
    int ok = 1;
    if (!D->px.col_upd) return; /* it writes no colour */
    switch (D->prim) {
    case 0x80: tris = (count / 4) * 2; break;
    case 0x90: tris = count / 3; break;
    case 0x98: case 0xA0: tris = count >= 3 ? count - 2 : 0; break;
    default: return; /* lines and points */
    }
    if (!tris) return;
    if (!D->px.blend_en && !D->px.logic_en) src = 1, dst = 0; /* drawn over what is there */
    else if (D->px.logic_en || src == 2 || src == 3 || src > 5 || dst > 5) ok = 0;
    if (D->tev.stages != 1 || S->cbias == 3) ok = 0;
    if (C && !keepable(C)) ok = 0;
    if (!ok || b->nf >= MAX_FLAT || b->nfv + tris * 3 > MAX_FLAT_VERTS) { b->nf_skipped++; return; }
    f = &b->flat[b->nf];
    memset(f, 0, sizeof *f);
    f->first_vertex = b->nfv;
    for (t = 0; t < tris; t++) {
        if (D->prim == 0x80) order[0] = 4 * (t / 2), order[1] = order[0] + 1 + (t & 1), order[2] = order[0] + 2 + (t & 1);
        else if (D->prim == 0x90) order[0] = 3 * t, order[1] = order[0] + 1, order[2] = order[0] + 2;
        else if (D->prim == 0x98) order[0] = t, order[1] = t + 1, order[2] = t + 2;
        else order[0] = 0, order[1] = t + 1, order[2] = t + 2;
        for (i = 0; i < 3; i++) {
            const Vertex* p = &v[order[i]];
            SoaHostFlatVertex* o = &b->fv[b->nfv + 3 * t + i];
            float w = p->w != 0.0f ? p->w : 1.0f, ras[4] = {0, 0, 0, 0};
            uint32_t black;
            if (S->chan < 2) ras[0] = p->col[S->chan].r, ras[1] = p->col[S->chan].g, ras[2] = p->col[S->chan].b, ras[3] = p->col[S->chan].a;
            o->x = (D->rc.xorig + p->x / w * D->rc.wd) / 640.0f;
            o->y = (D->rc.yorig + p->y / w * D->rc.ht) / 480.0f;
            o->u = o->v = 0.0f;
            black = tev_one_stage(&D->tev, ras, lo, &ok, NULL);
            o->colour = black;
            if (C) {
                o->u = p->tex[S->texcoord & 7][0] * C->scale_s / (float)(C->w > 0 ? C->w : 1);
                o->v = p->tex[S->texcoord & 7][1] * C->scale_t / (float)(C->h > 0 ? C->h : 1);
                o->colour = tev_one_stage(&D->tev, ras, hi, &ok, NULL);
                if (t == 0 && i == 0) colour0 = black;
                else if (black != colour0) ok = 0; /* shaded where the texture is black too */
            }
        }
    }
    if (!ok) { b->nf_skipped++; return; }
    f->vertices = tris * 3;
    f->src_factor = src;
    f->dst_factor = dst;
    f->subtract = D->px.blend_en && D->px.subtract ? 1u : 0u;
    f->colour0 = colour0;
    f->passes_before = (uint8_t)(b->np < 255 ? b->np : 255);
    /* as the rasteriser will work it out (to_screen), which it hasn't yet */
    f->depth = (D->rc.farz + v[0].z / (v[0].w != 0.0f ? v[0].w : 1.0f) * D->rc.zrange) / 16777216.0f;
    f->depth_test = (uint8_t)(D->px.z_en && (D->px.z_func == 1 || D->px.z_func == 3));
    f->depth_write = (uint8_t)(D->px.z_en && D->px.z_upd);
    f->scissor[0] = (float)D->rc.scissor.x0 / 640.0f;
    f->scissor[1] = (float)D->rc.scissor.y0 / 480.0f;
    f->scissor[2] = (float)(D->rc.scissor.x1 + 1) / 640.0f;
    f->scissor[3] = (float)(D->rc.scissor.y1 + 1) / 480.0f;
    if (C) {
        keep_texture(C, &f->texture, &f->texture_gen);
        f->width = (uint16_t)C->lw[0];
        f->height = (uint16_t)C->lh[0];
        f->wrap_s = (uint8_t)C->wrap_s;
        f->wrap_t = (uint8_t)C->wrap_t;
        f->linear = (uint8_t)(C->linear != 0);
    }
    b->nfv += tris * 3;
    b->nf++;
}

/* GX's fog on a colour at a depth of the screen, as the rasteriser's
 * fog_apply has it: a quad drawn flat on the screen is fogged like anything
 * else, by the one depth it lies at. (At night the game adds a glow to the
 * whole picture with a quad at the near plane, where that scene's fog has
 * already taken a quarter of it.) */
static uint32_t fogged(const PixelCfg* px, uint32_t rgba, float depth)
{
    uint32_t zs = (uint32_t)(depth < 0.0f ? 0.0f : (depth > 1.0f ? 16777215.0f : depth * 16777215.0f)), out = rgba & 255u;
    float ze, f;
    int fi, i;
    if (px->fog_type == 0) return rgba;
    if (!px->fog_proj) {
        int32_t denom = (int32_t)px->fog_b_mag - (int32_t)(zs >> px->fog_b_shift);
        if (denom == 0) return rgba;
        ze = (px->fog_a * 16777215.0f) / (float)denom;
    } else {
        ze = px->fog_a * ((float)zs / 16777215.0f);
    }
    f = ze - px->fog_c;
    if (!(f > 0.0f)) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    switch (px->fog_type) {
    case 2: break;
    case 4: f = 1.0f - powf(2.0f, -8.0f * f); break;
    case 5: f = 1.0f - powf(2.0f, -8.0f * f * f); break;
    case 6: f = powf(2.0f, -8.0f * (1.0f - f)); break;
    case 7: f = powf(2.0f, -8.0f * (1.0f - f) * (1.0f - f)); break;
    default: return rgba;
    }
    fi = (int)(f * 256.0f);
    if (fi > 256) fi = 256;
    for (i = 0; i < 3; i++) {
        uint32_t c = (rgba >> (24 - 8 * i)) & 255u;
        out |= ((c * (uint32_t)(256 - fi) + px->fog_color[i] * (uint32_t)fi) >> 8) << (24 - 8 * i);
    }
    return out;
}

static void screen_draw(const DrawCmd* D, unsigned count)
{
    ModelFrame* b = building_frame();
    const uint32_t* bp = gx_bp_regs();
    const Stage* S = &D->tev.st[0];
    const Vertex* v = D->v;
    SoaHostScreenPass pass;
    float lo[4] = {0, 0, 0, 0}, hi[4] = {1, 1, 1, 1};
    float x0 = 2, x1 = -2, y0 = 2, y1 = -2;
    unsigned i, corners = 0, src = D->px.sfac, dst = D->px.dfac, cut[4] = {0, 0, 0, 0};
    int ok = 1, flat = 1;
    if (!(gx_xf_regs()[0x1026] & 1)) { model_draw(b, D, count); return; }
    /* flat on the screen */
    if (D->efb) return;
    /* the strip the game sends between real ones, every vertex at one place: nothing */
    for (i = 1; i < count && v[i].x == v[0].x && v[i].y == v[0].y && v[i].w == v[0].w; i++) {}
    if (i >= count) return;
    /* Before the frame has a model there is no scene for a quad to be a
     * pass over: all of it is 2D layer, kept if the frame turns out to have
     * no model and let go at its first. */
    if (!b->n) { flat_draw(b, D, count); return; }
    /* a quad over all of the screen is a screen pass, until the 2D layer
     * has begun: then whatever covers the screen covers that too */
    if (count != 4 || b->nf > b->nf_under) { flat_draw(b, D, count); return; }
    for (i = 0; i < 4; i++) {
        float w = v[i].w != 0.0f ? v[i].w : 1.0f, x = v[i].x / w, y = v[i].y / w;
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
        if (memcmp(&v[i].col[0], &v[0].col[0], sizeof v[0].col[0]) != 0) flat = 0;
    }
    if (x0 > -0.99f || x1 < 0.99f || y0 > -0.99f || y1 < 0.99f) { flat_draw(b, D, count); return; }
    if (!D->px.col_upd) return; /* it writes no colour */
    /* A picture drawn over the whole screen (the opening's page of words) is
     * a 2D draw like any other. The one textured quad that is a pass is the
     * shadows': it multiplies what is there by one less a colour its mask
     * picks. */
    if (S->texen && !D->px.logic_en && !(D->px.blend_en && src == 0 && dst == 3)) { flat_draw(b, D, count); return; }
    /* What is left out: a logic operation (the shadows' own bookkeeping in
     * the red channel, which comes out even), a blend that reads the frame
     * buffer, more than one combiner stage, a textured quad that is shaded. */
    if (!D->px.blend_en && !D->px.logic_en) src = 1, dst = 0; /* drawn over what is there */
    else if (D->px.logic_en || src == 2 || src == 3 || src > 5 || dst > 5) ok = 0;
    if (D->tev.stages != 1 || (S->texen && !flat)) ok = 0;
    memset(&pass, 0, sizeof pass);
    pass.src_factor = src;
    pass.dst_factor = dst;
    pass.subtract = D->px.blend_en && D->px.subtract ? 1u : 0u;
    for (i = 0; ok && i < 4; i++) {
        /* which corner the vertex is: clip space's y runs up */
        float w = v[i].w != 0.0f ? v[i].w : 1.0f, ras[4];
        unsigned corner = (v[i].x / w > 0.0f ? 1u : 0u) | (v[i].y / w < 0.0f ? 2u : 0u);
        ras[0] = v[i].col[0].r; ras[1] = v[i].col[0].g; ras[2] = v[i].col[0].b; ras[3] = v[i].col[0].a;
        float depth = (D->rc.farz + v[i].z / w * D->rc.zrange) / 16777216.0f;
        pass.colour[corner] = fogged(&D->px, tev_one_stage(&D->tev, ras, lo, &ok, NULL), depth);
        corners |= 1u << corner;
        cut[i] = corner;
        if (S->texen && i == 0) pass.white = fogged(&D->px, tev_one_stage(&D->tev, ras, hi, &ok, &pass.step), depth);
    }
    if (corners != 15u) ok = 0;
    /* Where the quad is, which may be past the screen's edges (the night's
     * glow is a tenth wider), and which way it is cut in two: GX shades
     * each triangle between its own three corners, so a quad bright at one
     * corner is brighter along the cut than a blend of all four. A quad or
     * a fan is cut from its first vertex to its third, a strip from its
     * second to its third. */
    pass.rect[0] = (x0 + 1.0f) * 0.5f; pass.rect[1] = (1.0f - y1) * 0.5f;
    pass.rect[2] = (x1 + 1.0f) * 0.5f; pass.rect[3] = (1.0f - y0) * 0.5f;
    {
        unsigned ends = (1u << cut[D->prim == 0x98 ? 1 : 0]) | (1u << cut[2]);
        pass.split = ends == 6u ? 1u : ends == 9u ? 2u : 0u; /* bottom left to top right; top left to bottom right */
    }
    if (ok && S->texen) {
        unsigned m = S->texmap & 7;
        uint32_t image0 = bp[(m < 4 ? 0x88u + m : 0xA8u + (m - 4)) & 0xFFu], image3 = bp[(m < 4 ? 0x94u + m : 0xB4u + (m - 4)) & 0xFFu];
        pass.image = 0x80000000u | ((image3 & 0x00FFFFFFu) << 5);
        pass.format = (image0 >> 20) & 15u;
        pass.width = (uint16_t)((image0 & 0x3FFu) + 1u);
        pass.height = (uint16_t)(((image0 >> 10) & 0x3FFu) + 1u);
    }
    if (ok && b->np < MAX_PASSES) b->pass[b->np++] = pass;
    else if (!D->px.logic_en) b->np_skipped++; /* (a logic operation is the shadows' bookkeeping, which comes out even) */
}

static void screen_copy(uint32_t v, int x0, int y0, int w, int h, uint32_t dest, uint32_t bytes)
{
    (void)v; (void)x0; (void)y0; (void)w; (void)h; (void)dest; (void)bytes;
}

static const GxrDrawExport g_screen_export = {screen_vertex, screen_draw, screen_copy};

void soa_host_watch_models(int on)
{
    const char* exporting = getenv("SOA_GXR_EXPORT");
    ninja_set_feed(on ? &g_ninja_feed : NULL);
    /* the renderer has one exporter: a run that writes its draws out keeps it */
    if (!exporting || !*exporting) gxr_set_draw_export(on ? &g_screen_export : NULL);
}

long soa_host_screen_passes(SoaHostScreenPass* passes, unsigned max, unsigned* n, unsigned* skipped)
{
    long frame = -1;
    unsigned count = 0, left = 0;
    plat_lock(&g_mf_lock);
    if (g_ready >= 0) {
        const ModelFrame* r = &g_mf[g_ready];
        count = r->np < max ? r->np : max;
        if (passes) memcpy(passes, r->pass, count * sizeof *passes);
        left = r->np_skipped;
        frame = r->frame;
    }
    plat_unlock(&g_mf_lock);
    if (n) *n = count;
    if (skipped) *skipped = left;
    return frame;
}

long soa_host_flat_draws(SoaHostFlatDraw* draws, unsigned max_draws, unsigned* n_draws, SoaHostFlatVertex* vertices,
                         unsigned max_vertices, unsigned* n_vertices, unsigned* skipped)
{
    long frame = -1;
    unsigned n = 0, nv = 0, left = 0;
    plat_lock(&g_mf_lock);
    if (g_ready >= 0) {
        const ModelFrame* r = &g_mf[g_ready];
        n = r->nf < max_draws ? r->nf : max_draws;
        nv = r->nfv < max_vertices ? r->nfv : max_vertices;
        if (draws) memcpy(draws, r->flat, n * sizeof *draws);
        if (vertices) memcpy(vertices, r->fv, nv * sizeof *vertices);
        left = r->nf_skipped;
        frame = r->frame;
    }
    plat_unlock(&g_mf_lock);
    if (n_draws) *n_draws = n;
    if (n_vertices) *n_vertices = nv;
    if (skipped) *skipped = left;
    return frame;
}

int soa_host_flat_texture(uint32_t texture, uint32_t gen, uint8_t* out, unsigned max_bytes, unsigned* width, unsigned* height,
                          unsigned* levels)
{
    int found = 0;
    unsigned k;
    if (!texture || texture > FLAT_TEXTURES) return 0;
    plat_lock(&g_mf_lock);
    for (k = 0; k < KEPT_IMAGES && !found; k++) {
        const FlatTexture* ft = &g_flat_tex[texture - 1u][k];
        if (ft->rgba && ft->gen == gen && out && ft->bytes <= max_bytes) {
            memcpy(out, ft->rgba, ft->bytes);
            if (width) *width = (unsigned)ft->w;
            if (height) *height = (unsigned)ft->h;
            if (levels) *levels = (unsigned)ft->levels;
            found = 1;
        }
    }
    plat_unlock(&g_mf_lock);
    return found;
}

long soa_host_alpha_draws(SoaHostAlphaDraw* draws, unsigned max_draws, unsigned* n_draws, SoaHostAlphaVertex* vertices,
                          unsigned max_vertices, unsigned* n_vertices, unsigned* skipped)
{
    long frame = -1;
    unsigned n = 0, nv = 0, left = 0;
    plat_lock(&g_mf_lock);
    if (g_ready >= 0) {
        const ModelFrame* r = &g_mf[g_ready];
        n = r->na < max_draws ? r->na : max_draws;
        nv = r->nav < max_vertices ? r->nav : max_vertices;
        if (draws) memcpy(draws, r->alpha, n * sizeof *draws);
        if (vertices) memcpy(vertices, r->av, nv * sizeof *vertices);
        left = r->na_skipped;
        frame = r->frame;
    }
    plat_unlock(&g_mf_lock);
    if (n_draws) *n_draws = n;
    if (n_vertices) *n_vertices = nv;
    if (skipped) *skipped = left;
    return frame;
}

long soa_host_alpha_stages(SoaHostAlphaStage* stages, unsigned max, unsigned* n)
{
    long frame = -1;
    unsigned count = 0;
    plat_lock(&g_mf_lock);
    if (g_ready >= 0) {
        const ModelFrame* r = &g_mf[g_ready];
        count = r->nas < max ? r->nas : max;
        if (stages) memcpy(stages, r->as, count * sizeof *stages);
        frame = r->frame;
    }
    plat_unlock(&g_mf_lock);
    if (n) *n = count;
    return frame;
}

long soa_host_model_tints(uint32_t* out, unsigned max, unsigned* n)
{
    long frame = -1;
    unsigned count = 0;
    plat_lock(&g_mf_lock);
    if (g_ready >= 0) {
        const ModelFrame* r = &g_mf[g_ready];
        count = r->ntint < max ? r->ntint : max;
        if (out) memcpy(out, r->tint, count * sizeof *out);
        frame = r->frame;
    }
    plat_unlock(&g_mf_lock);
    if (n) *n = count;
    return frame;
}

long soa_host_model_bytes(uint8_t* out, unsigned max, unsigned* n)
{
    long frame = -1;
    unsigned count = 0;
    plat_lock(&g_mf_lock);
    if (g_ready >= 0) {
        const ModelFrame* r = &g_mf[g_ready];
        count = r->nb < max ? r->nb : max;
        if (out) memcpy(out, r->bytes, count);
        frame = r->frame;
    }
    plat_unlock(&g_mf_lock);
    if (n) *n = count;
    return frame;
}

long soa_host_models(SoaHostModel* models, unsigned max_models, unsigned* n_models, SoaHostTexture* textures,
                     unsigned max_textures, unsigned* n_textures, SoaHostLight* lights, unsigned max_lights,
                     unsigned* n_lights, float projection[7])
{
    long frame = -1;
    unsigned n = 0, nt = 0, nl = 0;
    plat_lock(&g_mf_lock);
    if (g_ready >= 0) {
        const ModelFrame* r = &g_mf[g_ready];
        n = r->n < max_models ? r->n : max_models;
        nt = r->nt < max_textures ? r->nt : max_textures;
        nl = r->nl < max_lights ? r->nl : max_lights;
        if (models) memcpy(models, r->m, n * sizeof *models);
        if (textures) memcpy(textures, r->t, nt * sizeof *textures);
        if (lights) memcpy(lights, r->l, nl * sizeof *lights);
        if (projection) memcpy(projection, r->proj, sizeof r->proj);
        frame = r->frame;
    }
    plat_unlock(&g_mf_lock);
    if (n_models) *n_models = n;
    if (n_textures) *n_textures = nt;
    if (n_lights) *n_lights = nl;
    return frame;
}

int soa_host_read(uint32_t address, void* out, unsigned bytes)
{
    uint8_t* mem = g_mem;
    uint32_t off = address & 0x01FFFFFFu;
    if (!mem || !out || address < 0x80000000u || address >= 0x81800000u || off + (uint64_t)bytes > MEM1_SIZE) return 0;
    memcpy(out, mem + off, bytes);
    return 1;
}
#endif
