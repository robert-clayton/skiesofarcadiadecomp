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
} ModelFrame;

static ModelFrame g_mf[2];
static int g_build;                  /* the guest's; the other is the host's */
static int g_ready = -1;             /* the published one, under g_mf_lock */
static PlatLock g_mf_lock;
static uint8_t* volatile g_mem;      /* MEM1, as the hooks see it */
static unsigned long long g_mf_dropped;

static ModelFrame* building(CpuState* s)
{
    long f = (long)gx_frame_count();
    ModelFrame* b = &g_mf[g_build];
    g_mem = s->mem;
    if (b->frame != f) {
        if (b->n) {
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
    }
    return b;
}

void host_frame_end(unsigned frame)
{
    ModelFrame* b = &g_mf[g_build];
    if (!b->n || b->frame != (long)frame) return; /* a frame without a model leaves the last one standing */
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
    /* a model past the array still opens, so its end pairs with it */
    if (b->n_open < MAX_OPEN) b->open[b->n_open] = b->n < MAX_MODELS ? b->n : ~0u;
    b->n_open++;
    if (b->n >= MAX_MODELS) { g_mf_dropped++; return; }
    b->visit[b->n] = v->visit;
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
    if (!b->n_open) return; /* watching began inside a drawer */
    b->n_open--;
    if (b->n_open >= MAX_OPEN || (index = b->open[b->n_open]) >= b->n) return;
    m = &b->m[index];
    m->drawn = drawn ? 1u : 0u;
    if (!recorded && strips) feed_strips(b, m, strips);
}

/* The display list a visit's strips were recorded into is called, and the
 * parser is past the last of them. */
static void feed_sent(CpuState* s, unsigned visit, unsigned strips)
{
    ModelFrame* b = &g_mf[g_build];
    unsigned lo = 0, hi = b->n;
    (void)s;
    if (b->frame != (long)gx_frame_count() || !strips) return;
    while (lo < hi) { /* the visits' numbers climb */
        unsigned mid = (lo + hi) / 2;
        if (b->visit[mid] < visit) lo = mid + 1;
        else hi = mid;
    }
    if (lo < b->n && b->visit[lo] == visit) feed_strips(b, &b->m[lo], strips);
}

static const NinjaFeed g_ninja_feed = {feed_model, feed_texture, feed_end, feed_sent};

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

static void screen_vertex(unsigned i, const VertexIn* in, const Vertex* out)
{
    (void)i;
    (void)in;
    (void)out;
}

static void screen_draw(const DrawCmd* D, unsigned count)
{
    ModelFrame* b = &g_mf[g_build];
    const uint32_t* bp = gx_bp_regs();
    const Stage* S = &D->tev.st[0];
    const Vertex* v = D->v;
    SoaHostScreenPass pass;
    float lo[4] = {0, 0, 0, 0}, hi[4] = {1, 1, 1, 1};
    float x0 = 2, x1 = -2, y0 = 2, y1 = -2;
    unsigned i, corners = 0, src = D->px.sfac, dst = D->px.dfac;
    int ok = 1, flat = 1;
    /* a quad, flat on the screen, over all of it, after the frame's first model */
    if (count != 4 || D->efb || !(gx_xf_regs()[0x1026] & 1) || !b->n || b->frame != (long)gx_frame_count()) return;
    for (i = 0; i < 4; i++) {
        float w = v[i].w != 0.0f ? v[i].w : 1.0f, x = v[i].x / w, y = v[i].y / w;
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
        if (memcmp(&v[i].col[0], &v[0].col[0], sizeof v[0].col[0]) != 0) flat = 0;
    }
    if (x0 > -0.99f || x1 < 0.99f || y0 > -0.99f || y1 < 0.99f) return;
    if (!D->px.col_upd) return; /* it writes no colour */
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
        pass.colour[corner] = tev_one_stage(&D->tev, ras, lo, &ok, NULL);
        corners |= 1u << corner;
        if (S->texen && i == 0) pass.white = tev_one_stage(&D->tev, ras, hi, &ok, &pass.step);
    }
    if (corners != 15u) ok = 0;
    if (ok && S->texen) {
        unsigned m = S->texmap & 7;
        uint32_t image0 = bp[(m < 4 ? 0x88u + m : 0xA8u + (m - 4)) & 0xFFu], image3 = bp[(m < 4 ? 0x94u + m : 0xB4u + (m - 4)) & 0xFFu];
        pass.image = 0x80000000u | ((image3 & 0x00FFFFFFu) << 5);
        pass.format = (image0 >> 20) & 15u;
        pass.width = (uint16_t)((image0 & 0x3FFu) + 1u);
        pass.height = (uint16_t)(((image0 >> 10) & 0x3FFu) + 1u);
    }
    if (ok && b->np < MAX_PASSES) b->pass[b->np++] = pass;
    else b->np_skipped++;
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
