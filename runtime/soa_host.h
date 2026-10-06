/*
 * The runtime inside another program: the C API of a host build (SOA_HOST,
 * tools/recompile.py --split --host). The program that loads
 * libsoa_runtime.so owns the window, the input and the sound -- a game
 * engine's editor, say -- and the runtime owns the game, on threads of its
 * own. A host build is a split build with this file's backend, host.c, in
 * place of SDL's window and sound; everything else, the self test among it,
 * is the split build's.
 *
 * Nothing here calls back into the host. The host pulls what the run makes
 * (the newest frame, the sound, the motor) and pushes what the player does
 * (the pads, pause, mute), each from whichever of its threads suits it, so
 * the guest's thread never runs host code and the host never runs guest code.
 *
 * Every function is safe before soa_host_start and from any thread.
 */
#ifndef SOA_HOST_H
#define SOA_HOST_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#define SOA_HOST_API
#else
#define SOA_HOST_API __attribute__((visibility("default")))
#endif

/* Bumped when anything below changes meaning; the host checks soa_host_abi()
 * against the number it was built with before calling anything else. */
#define SOA_HOST_ABI 4u

/* One controller in the game's own terms, as si.c reads it.
 *
 * buttons: A 0x0100, B 0x0200, X 0x0400, Y 0x0800, START 0x1000, Z 0x0010,
 *          R 0x0020, L 0x0040, D-pad up 0x0008, down 0x0004, left 0x0001,
 *          right 0x0002. L and R are the triggers' digital clicks.
 * stick, cstick: x then y, 0 to 255 with 128 the centre; y 255 is up.
 * trig: L then R, 0 to 255. */
typedef struct {
    uint16_t buttons;
    uint8_t stick[2];
    uint8_t cstick[2];
    uint8_t trig[2];
} SoaHostPad;

typedef struct {
    uint32_t abi;     /* SOA_HOST_ABI */
    const char* game; /* the game library, libsoa_game.so, from the same build */
    const char* disc; /* the extracted directory or the disc image; NULL: soa.ini's, else "extracted" */
    const char* root; /* the port root: soa.ini, the memory card, logs; NULL: settings.c's own rule */
} SoaHostStart;

enum { SOA_HOST_IDLE, SOA_HOST_RUNNING, SOA_HOST_ENDED };

SOA_HOST_API unsigned soa_host_abi(void);

/* Loads and checks the game library on the calling thread, then starts the
 * run on a thread of its own and returns 0; or returns nonzero with why in
 * `why`, the run not started. One run per process: the runtime's state is
 * static, so a second start is refused. `root`, when given, also places the
 * memory card (<root>/build/cards/slotA.raw) unless SOA_CARD names one.
 *
 * Most of the ways a run ends leave the process, as they do in the launcher:
 * a fatal error in the guest exits. The ones that return are reported by
 * soa_host_state. */
SOA_HOST_API int soa_host_start(const SoaHostStart* start, char* why, size_t cap);

/* SOA_HOST_IDLE, _RUNNING, or _ENDED with the run's return code in *rc. */
SOA_HOST_API int soa_host_state(int* rc);

/* The number of frames the renderer has copied out so far; a new number is a
 * new frame. *rgba is the newest: RGBA, *w by *h pixels, rows *stride bytes
 * apart. Its alpha is the frame buffer's, not opacity, so treat it as opaque.
 * The renderer copies the next frame into the same memory on the guest's
 * thread, so read it promptly; a read that overlaps a copy tears, as the
 * launcher's window does. */
SOA_HOST_API long soa_host_frame(const uint8_t** rgba, int* w, int* h, int* stride);

/* What controller port 1 or 2 holds now; NULL unplugs it. Port 1 is the
 * game's, merged with SOA_PAD's script as the launcher's keyboard is; port 2
 * is for mods (P10a). Read at each of the game's polls. */
SOA_HOST_API void soa_host_set_pad(unsigned port, const SoaHostPad* pad);

/* Up to `frames` stereo frames of the game's sound, interleaved left/right
 * signed 16-bit in host byte order, taken from the queue; returns how many.
 * *rate is the sample rate (the game's, 32000 or 48000), or 0 before the game
 * has made any sound. The queue holds what the launcher's device would, about
 * 0.77 s at 32 kHz, and drops blocks when it is full. */
SOA_HOST_API unsigned soa_host_audio(int16_t* lr, unsigned frames, unsigned* rate);

/* The rumble the game asks of port 1 now: 0 off, up to 65535. */
SOA_HOST_API unsigned soa_host_motor(void);

/* ---- the game's models (ABI 2; the lists in each record since 3, GX's state and the lights since 4) ----------
 * The chunk models Sega's Ninja library draws (runtime/ninja.h), so a host
 * can rebuild the scene with its own renderer: per frame, each model the
 * game handed a drawer -- culled or not -- with its matrices and the
 * textures its chunks selected. The host reads the model data and the
 * images themselves with soa_host_read: the layouts are Ninja's, described
 * in soa-ue5's docs/ninja-models.md. Off until soa_host_watch_models(1); it
 * costs a copy of two matrices per model drawn. */
typedef struct {
    uint32_t model;         /* the NJS_CNK_MODEL's guest address: vlist, plist, centre, radius */
    uint32_t drawer;        /* which of Ninja's four drawers took it (0-3) */
    uint32_t vlist, plist;  /* its vertex and polygon chunk lists when it was drawn: what identifies
                               the model, since one struct can be refilled for piece after piece */
    uint32_t first_texture; /* its textures: [first_texture, first_texture + textures) of the frame's */
    uint32_t textures;
    float modelview[12];    /* 3x4, rows: the model's space to the game's view space */
    float camera[12];       /* 3x4, rows: the view matrix, world to view space (Ninja's stack level 1) */
    /* How GX was set to shade it, as its last strip left the registers when
     * the drawer returned. Not set for a model the drawer culled (drawn 0):
     * it sent nothing, so these are whatever the model before it left. */
    uint32_t drawn;         /* 0: the drawer's clip test dropped it */
    uint32_t channels;      /* GXSetNumChans: colour channels in use (0: no colour reaches the combiner) */
    uint32_t chan_colour;   /* GXSetChanCtrl for COLOR0 (XF 0x100E): bit 0 material from the vertex colour
                               (else the register), bit 1 lighting on, bits 2-5 and 11-14 the lights
                               (0-3, 4-7), bit 6 ambient from the vertex colour, bits 7-8 the diffuse
                               function (0 none, 1 signed, 2 clamped), bits 9-10 attenuation (1 specular,
                               3 spot and distance, else none) */
    uint32_t chan_alpha;    /* the same for ALPHA0 (XF 0x1010) */
    uint32_t ambient;       /* GXSetChanAmbColor, RGBA */
    uint32_t material;      /* GXSetChanMatColor, RGBA */
    uint32_t tev_stages;    /* GXSetNumTevStages */
    uint32_t tev_colour;    /* stage 0's colour combiner (BP 0xC0): d in bits 0-3, c 4-7, b 8-11, a 12-15
                               (GX_CC_*: 8 the texture, 10 the rasterised colour, 15 zero), bias 16-17,
                               subtract 18, clamp 19, scale 20-21 (x1, x2, x4, x1/2), destination 22-23 */
    uint32_t tev_alpha;     /* stage 0's alpha combiner (BP 0xC1) */
    uint32_t first_light;   /* the lights chan_colour names: [first_light, first_light + lights) of the */
    uint32_t lights;        /* frame's, in the order of their GX indices */
} SoaHostModel;

/* One GX light as a model's draws had it (GXInitLight*), in the game's view
 * space. A directional light is a position far away: the direction to it
 * is `position`, normalised. */
typedef struct {
    uint32_t index;         /* which of GX's eight */
    uint32_t colour;        /* RGBA */
    float a[3];             /* angle attenuation (spot): a0 + a1 cos + a2 cos^2 */
    float k[3];             /* distance attenuation: 1 / (k0 + k1 d + k2 d^2) */
    float position[3];
    float direction[3];
} SoaHostLight;

typedef struct {
    uint32_t id;            /* the texture id the model's chunks name */
    uint32_t image;         /* the image's guest address; a palettized one (C4, C8) starts with its
                               palette, 16 or 256 two-byte entries, and its texels follow */
    uint32_t format;        /* the GX format: 14 CMPR, 5 RGB5A3, 6 RGBA8, 4 RGB565, 8 C4, 9 C8 ... */
    uint32_t palette;       /* a palette's colour format: 0 IA8, 1 RGB565, 2 RGB5A3 */
    uint16_t width, height;
    char name[24];          /* the texture list's name for it */
} SoaHostTexture;

SOA_HOST_API void soa_host_watch_models(int on);

/* The newest whole frame's models: up to max_models of them, max_textures of
 * their textures and max_lights of their lights into the arrays given, how
 * many in *n_models, *n_textures and *n_lights, and the GX projection they
 * were drawn with in projection[7] (GXSetProjection's six parameters, then
 * 0; or 1 when the frame drew under no perspective projection). Returns that
 * frame's number, or -1 before the first. */
SOA_HOST_API long soa_host_models(SoaHostModel* models, unsigned max_models, unsigned* n_models, SoaHostTexture* textures,
                                  unsigned max_textures, unsigned* n_textures, SoaHostLight* lights, unsigned max_lights,
                                  unsigned* n_lights, float projection[7]);

/* bytes of the game's memory at a guest address (0x80000000 up), as the
 * game sees them -- big-endian -- into out; 0 for a range outside MEM1, or
 * before the game has drawn a model. The game may be writing there as it
 * is read: models and images stay put while they are in use. */
SOA_HOST_API int soa_host_read(uint32_t address, void* out, unsigned bytes);

/* Pause holds the guest at the end of a frame, and its clock with it (M19);
 * mute silences what soa_host_audio returns. */
SOA_HOST_API void soa_host_pause(int on);
SOA_HOST_API void soa_host_mute(int on);

#ifdef __cplusplus
}
#endif
#endif
