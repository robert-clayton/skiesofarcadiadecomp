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
 * the other. A frame is published when the guest starts drawing the next,
 * so what the host reads is always whole. */
#define MAX_MODELS 8192
#define MAX_TEXTURES 16384
#define MAX_LIGHTS 1024
#define MAX_OPEN 8 /* drawers inside drawers; none are seen, but a begin and its end must still pair */

typedef struct {
    long frame;
    unsigned n, nt, nl;
    unsigned open[MAX_OPEN], n_open; /* the models whose drawers have not returned yet */
    unsigned sent;                   /* how many of them have reached the GPU: the next one's place in that order */
    float proj[7];
    uint32_t visit[MAX_MODELS];      /* ninja.c's number for each, which climbs with them */
    SoaHostModel m[MAX_MODELS];
    SoaHostTexture t[MAX_TEXTURES];
    SoaHostLight l[MAX_LIGHTS];
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
        b->n = b->nt = b->nl = b->n_open = b->sent = 0;
    }
    return b;
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

void soa_host_watch_models(int on)
{
    ninja_set_feed(on ? &g_ninja_feed : NULL);
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
