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
#define SOA_HOST_ABI 18u

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

/* ---- the game's models (ABI 2; the lists in each record since 3, GX's state and the lights since 4,
 * ---- the draw mode and the order since 5) ----------------------------------------------------------
 * The chunk models Sega's Ninja library draws (runtime/ninja.h), so a host
 * can rebuild the scene with its own renderer: per frame, each time the
 * game handed a model to a drawer -- culled or not -- with its matrices and
 * the textures its chunks selected. The host reads the model data and the
 * images themselves with soa_host_read: the layouts are Ninja's, described
 * in soa-ue5's docs/ninja-models.md. Off until soa_host_watch_models(1); it
 * costs a copy of two matrices per model drawn.
 *
 * A model is usually handed over twice a frame, at the same place: once
 * for its opaque strips and once for the ones that use alpha (`mode`). The
 * second kind the game does not draw then but records and draws later, in
 * its own order, so the records come in the order the drawers ran and
 * `order` says when each one's strips were really drawn. */
#define SOA_HOST_NOT_SENT 0xFFFFFFFFu

typedef struct {
    uint32_t model;         /* the NJS_CNK_MODEL's guest address: vlist, plist, centre, radius */
    uint32_t drawer;        /* which of Ninja's four drawers took it (0-3) */
    uint32_t vlist, plist;  /* its vertex and polygon chunk lists when it was drawn: what identifies
                               the model, since one struct can be refilled for piece after piece */
    uint32_t first_texture; /* its textures: [first_texture, first_texture + textures) of the frame's */
    uint32_t textures;
    float modelview[12];    /* 3x4, rows: the model's space to the game's view space */
    float camera[12];       /* 3x4, rows: the view matrix, world to view space (Ninja's stack level 1) */
    uint32_t mode;          /* which of its strips the drawer was set to draw: 1 all, 2 the opaque ones, 3 the
                               ones that use alpha (a strip chunk's flag 0x08), 4 those twice (culled the
                               other way round, then the usual way), 5 and 6 the first and the second of
                               those passes alone; anything else, none */
    uint32_t strip_and;     /* Ninja's constant attribute: each strip chunk's flags are and-ed with strip_and */
    uint32_t strip_or;      /* and or-ed with strip_or before the mode is applied, so 0x08 in strip_or makes
                               every strip one that uses alpha (a model being faded). 0xFF and 0 when off */
    uint32_t drawn;         /* 0: the drawer's clip test dropped it */
    uint32_t strips;        /* how many strips it sent the GPU this frame: 0 for a model that was culled, had
                               none of the mode's kind, or was recorded into a list the game never called */
    uint32_t order;         /* its place among the frame's records in the order their strips reached the GPU,
                               from 0: the order translucent ones were laid over each other.
                               SOA_HOST_NOT_SENT when strips is 0 */
    /* How GX was set to shade it, as its last strip left the registers.
     * Set only when strips is not 0. */
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
    uint32_t fog[5];        /* GXSetFog, as BP 0xEE to 0xF2 hold it: A; B's mantissa; B's shift; C in the low
                               20 bits, with bit 20 an orthographic projection and bits 21-23 the type (0
                               off, 2 linear, 4 exponential, 5 its square, 6 and 7 those backwards); and the
                               colour, RGB. A and C are 20-bit floats: sign, 8 bits of exponent, 11 of
                               mantissa. Under a perspective projection a pixel's fog is the type's curve
                               of A / (B - its screen depth) - C */
    uint32_t first_light;   /* the lights chan_colour names: [first_light, first_light + lights) of the */
    uint32_t lights;        /* frame's, in the order of their GX indices */
    uint32_t depth;         /* GX's depth mode as its last strip left it (BP 0x40): bit 0 the test is on, bits
                               1-3 its function (3 less or equal), bit 4 the strip writes its depth. A
                               see-through strip that does hides what is drawn later behind it. Set only
                               when strips is not 0 (ABI 9) */
    uint32_t tints;         /* for a model drawn with more than one combiner stage (ABI 10): what the whole */
    uint32_t tint_vertices; /* combiner makes of each vertex with the model's own texture white, RGBA:
                               [tints, tints + tint_vertices) of the frame's (soa_host_model_tints), a vertex
                               for a vertex in the order its strips reached the GPU, both passes of a mode
                               that has two. The model's texture times that is the strip's colour, whatever
                               the stages were: other textures looked up at the vertex, a second light, a
                               doubling. 0 for a model drawn with one stage */
    uint32_t tint_offsets;  /* how many of those vertices have a colour with the model's texture black too:
                               there the texture is more than a multiplier and the tint alone is short */
    uint32_t solid_strips;  /* how many of its strips were solid, and how many belonged to the see-through */
    uint32_t alpha_strips;  /* phase and were handed over there (soa_host_alpha_draws, ABI 11). A strip's flag
                               says which it should be; GX's state when it is drawn says which it is. ABI 13:
                               a solid strip drawn after a see-through layer that is part clear and wrote
                               depth, under the same projection, counts with the second kind and is handed
                               over in order: it may lie behind that layer, which then hides it */
    uint32_t vertices;      /* its vertex list as it was when the drawer took it, for a list the game refills */
    uint32_t vertices_bytes; /* for one model after another (a shape it works out afresh): [vertices,
                               vertices + vertices_bytes) of the frame's kept bytes (soa_host_model_bytes),
                               the bytes guest memory held at vlist then. 0 for every other model, whose
                               list stays as it is and is read at vlist */
    float near_clip, far_clip; /* ABI 12. How far along the view the near and far planes of the projection
                               its strips were drawn under are; 0 for one that can't be worked out. A frame
                               has several projections (a sky's reaches far past the scene's), and GX's fog
                               goes by the depth the strip's own gives */
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
    uint8_t levels;         /* ABI 12. How many levels of detail the model's draws sample: 1 for the image
                               alone; more when GX's filter for a texture seen small is a mipmapped one.
                               The smaller levels follow the image in memory, each half the last a side
                               (never under one texel) and laid out as an image of its own size is */
    int8_t lod_bias;        /* added to the level GX works out, in 32nds of a level */
    uint8_t min_lod, max_lod; /* the levels it keeps between, in 16ths */
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

/* The vertex lists the newest whole frame's models name in `vertices` (ABI
 * 7): up to max bytes into out, how many in *n. Returns that frame's number
 * as soa_host_models does, or -1; when the two differ a frame was published
 * between the calls, and both want asking again. */
SOA_HOST_API long soa_host_model_bytes(uint8_t* out, unsigned max, unsigned* n);

/* The tints the newest whole frame's models name: up to max of them into
 * out, how many in *n; the frame's number as soa_host_models gives it. */
SOA_HOST_API long soa_host_model_tints(uint32_t* out, unsigned max, unsigned* n);

/* ---- what the game draws over the whole screen (ABI 6) ----------------------------------------
 * After the models the game draws on the frame itself, with quads that
 * cover the screen: a glow added to every pixel, a fade, and the last step
 * of its shadows, which multiplies the picture down where a mask says so. A
 * host that draws the scene itself needs them to end with the game's
 * picture. Each is one blend of a source colour over what is there:
 *
 *     new = source x src_factor + old x dst_factor     (old - source when subtract)
 *
 * with GX's factors: 0 zero, 1 one, 4 the source's alpha, 5 one less it, and
 * for dst_factor also 2 the source's colour and 3 one less it. The source is
 * the quad's own colour through its combiner, given at the screen's four
 * corners and shaded evenly between them. A quad with a texture has one
 * colour all over where the texture is black (the corners) and another where
 * it is white. The shadow mask is such a texture: an 8-bit copy of the frame
 * buffer, in guest memory at `image`, which the combiner compares with a
 * number instead of mixing by it.
 *
 * Only quads drawn after the frame's first model are reported, in the order
 * they were drawn, and only ones whose blend doesn't read the frame buffer
 * and whose combiner is one stage: `skipped` counts the rest. */
typedef struct {
    uint32_t src_factor, dst_factor, subtract;
    uint32_t colour[4];     /* the source at the top left, top right, bottom left and bottom right, RGBA: with
                               the texture black, or with none */
    uint32_t white;         /* with a texture: the source where it is white */
    uint32_t step;          /* with a texture: 0 when the source runs evenly from black's to white's; else the
                               texel value, 1 to 255, under which it is black's and from which it is white's */
    uint32_t image;         /* the texture's guest address; 0 for a quad without one */
    uint32_t format;        /* its GX format: 1 I8, ... */
    uint16_t width, height;
    float rect[4];          /* ABI 12. Where the quad's corners are: left, top, right, bottom, 0 to 1 across
                               and down the screen. It covers the screen and may reach past it, and its
                               corners' colours are out where they are */
    uint32_t split;         /* ABI 12. How GX cut it into two triangles, each shaded between its own three
                               corners: 1 from the bottom left to the top right, 2 from the top left to the
                               bottom right; 0 when it can't be told (shade it evenly between the four) */
} SoaHostScreenPass;

/* The screen passes of the newest whole frame: up to max of them into the
 * array, how many in *n, and in *skipped how many screen-filling quads were
 * left out. Returns that frame's number, the one soa_host_models gives for
 * the same frame, or -1 before the first. Watched with the models. */
SOA_HOST_API long soa_host_screen_passes(SoaHostScreenPass* passes, unsigned max, unsigned* n, unsigned* skipped);

/* ---- the see-through phase (ABI 11) -----------------------------------------------------------
 * The draws that aren't solid, in the order they reached the GPU: strips
 * that use alpha, glows, a sky that fades at its rim, and the few 3D draws
 * no model makes (a sea of cloud). The game blends them in that order and
 * they test and write depth as they go, so one drawn early hides what is
 * drawn later behind it, and what was drawn before it shows through. A
 * host that draws them some other way gets a different picture, so they
 * are handed over ready to draw: triangles, the ones the game's culling
 * left, in the game's view space (x right, y up, looking down -z), each
 * vertex with its lit colour. A pixel is what the draw's combiner makes of
 * that colour, shaded across the triangle, and the draw's texture:
 * `stages` of them from first_stage on, each GX's
 *
 *     result = (d + ((1 - c) x a + c x b, taken away when `subtract`) + bias) x scale
 *
 * for the colour and again for the alpha, into one of four registers
 * (0 the result so far, which the last stage leaves the pixel in). It is
 * dropped unless it passes the alpha test, fogged as `fog` says, blended
 * as a screen pass is (src_factor, dst_factor, subtract), and tested
 * against and written to the depth buffer as `depth` says.
 *
 * A combiner that reads more than the draw's own texture and one vertex
 * colour (a second texture looked up somewhere of its own, GX's second
 * colour, a stage that compares) can't be handed over that way. Then
 * `stages` is 0 and each vertex has what the whole combiner made of it with
 * the texture white instead:
 *
 *     colour = texel x vertex colour        alpha = texel's alpha x vertex alpha
 *                                           (the vertex alpha alone when texture_alpha is 0) */
typedef struct {
    uint32_t colour;        /* the colour's sum: d in bits 0-3, c 4-7, b 8-11, a 12-15 (GX_CC_*: 0 the result so
                               far, 2 4 6 registers 1 to 3, and 1 3 5 7 their alphas; 8 the texel, 9 its alpha;
                               10 the vertex colour, 11 its alpha; 12 one, 13 a half, 14 the stage's constant,
                               15 zero); bias 16-17 (1 add a half, 2 take it away), subtract 18, clamp to 0..1
                               19 (else to -4..4), scale 20-21 (x1, x2, x4, x1/2), the register written 22-23 */
    uint32_t alpha;         /* the alpha's, laid out the same (GX_CA_*: 0 the result so far, 1 2 3 the
                               registers, 4 the texel's alpha, 5 the vertex's, 6 the constant's, 7 zero) */
    uint32_t konst;         /* the stage's constant, RGBA */
    uint32_t flags;         /* bit 0 the stage samples a texture, bit 1 it takes the vertex colour, bit 2
                               (ABI 12) the vertex's second colour: a stage that does none sees what the
                               last one that did saw. ABI 12: bit 3 the texture is the draw's second
                               (texture1), bits 4-5 which of the vertex's places it samples at (0 u v,
                               1 u1 v1, 2 u2 v2); bits 8-15 which of the texel's channels the stage sees
                               as its red, green, blue and alpha, two bits each (0 red ... 3 alpha: GX's
                               swap table; 0 1 2 3 is the texel as it is), bits 16-23 the same for the
                               vertex colour it takes */
} SoaHostAlphaStage;

typedef struct {
    float x, y, z;          /* in the game's view space */
    float u, v;             /* over the texture: 0 to 1 */
    uint32_t colour;        /* RGBA */
    uint32_t colour1;       /* ABI 12. GX's second lit colour of the vertex, for a stage that takes it */
    float u1, v1, u2, v2;   /* ABI 12. Two more places on a texture, for a combiner whose stages sample at
                               more than one: a stage says which of the three it reads */
} SoaHostAlphaVertex;

/* How GX makes a coordinate a texture is sampled at (GXSetTexCoordGen). */
typedef struct {
    uint32_t info;          /* XF's word for it: bit 2, three numbers go in (else two and a one); bits 4-6 the
                               kind (0 a matrix's); bits 7-11 what it starts from: 0 the vertex's place, 1 its
                               normal, 5 the first coordinate the game sent for it, 6 the second, 7 the third */
    float rows[2][4];       /* the matrix: each coordinate is a row times (x, y, z or 1, 1) */
    uint32_t second;        /* bit 0: GX's second transform follows; bit 8: with what the first made, and a 1
                               after it, brought to length one before it */
    float second_rows[2][4]; /* the second's matrix: a row times (what the first made, 1, 1) */
    float scale[2];         /* and then times this, which puts it in the texture's own 0 to 1 */
} SoaHostTexGen;

typedef struct {
    uint32_t first_vertex;  /* its triangles: [first_vertex, first_vertex + vertices) of the frame's, three each */
    uint32_t vertices;
    uint32_t model;         /* which of the frame's models it is a strip of, or 0xFFFFFFFF for none */
    uint32_t src_factor, dst_factor, subtract;
    uint32_t texture;       /* 0 for none; else as a 2D draw's, for soa_host_flat_texture */
    uint32_t texture_gen;
    uint16_t width, height;
    uint8_t wrap_s, wrap_t; /* past its edge: 0 clamp, 1 repeat, 2 mirror */
    uint8_t linear;
    uint8_t texture_alpha;  /* 1: the texture's alpha multiplies the vertex's */
    uint8_t depth;          /* bit 0 the depth test is on, bits 1-3 its function (GX_COMPARE: 3 less or equal),
                               bit 4 a pixel that is drawn writes its depth */
    uint8_t alpha_comp0, alpha_comp1; /* the alpha test: alpha comp0 ref0, joined by alpha_logic (0 and, 1 or, */
    uint8_t alpha_logic;              /* 2 xor, 3 xnor) to alpha comp1 ref1; GX_COMPARE: 0 never, 1 less, 2 equal, */
    uint8_t alpha_ref0, alpha_ref1;   /* 3 less or equal, 4 greater, 5 not equal, 6 greater or equal, 7 always */
    uint8_t levels;         /* as SoaHostTexture's four (ABI 12): the texture's levels of detail this draw */
    int8_t lod_bias;        /* samples, which soa_host_flat_texture hands over together */
    uint8_t min_lod, max_lod;
    uint8_t passes_before;  /* ABI 13. How many of the frame's screen passes were drawn before it: the shadows'
                               darkening comes after the solid scene and before the see-through phase, and a
                               host that draws these in order darkens between the same two draws */
    uint8_t pad;
    uint32_t fog[5];        /* as SoaHostModel's */
    uint32_t offsets;       /* stages 0: how many of its vertices have a colour with the texture black too */
    uint32_t first_stage;   /* its combiner: [first_stage, first_stage + stages) of the frame's stages */
    uint32_t stages;        /* (soa_host_alpha_stages); 0 when it can't be given, see above */
    int16_t registers[4][4]; /* what the four registers hold going in, RGBA, 255 to one */
    float near_clip, far_clip; /* how far along the view its projection's near and far planes are: GX draws
                               nothing of it nearer or farther. A frame's draws don't all share a projection
                               (0 for a plane that can't be worked out) */
    uint16_t strip;         /* ABI 14. Which of its model's strips it is: the nth strip of the visit to reach
                               the GPU, from 0, solid ones counted too. With the visit's draw mode that
                               names a strip of the model's polygon list, so a host that has the model
                               can draw the strip from it */
    uint16_t strip_vertices; /* how many vertices that strip has */
    uint8_t cull;           /* GX's cull mode for it: 0 none, 1 front faces, 2 back faces, 3 all */
    uint8_t plain;          /* 1: a host that has the model can make its vertices: a strip, each place on a
                               texture made as `gen` says from one of the first three coordinates the game
                               sent for the vertex, its place or its normal, its stages reading the first
                               lit colour only. (The game's second and third coordinates are the bytes of
                               the vertex's colour in its file: blue and green, red and alpha, as 256ths.
                               A table of colours is looked up with them.) */
    uint8_t not_plain;      /* ABI 16. What goes into a vertex beyond the strip's own, a bit each. These make
                               it not plain: 1 not a model's, 2 not a strip, 4 a combiner that can't be handed
                               over, 16 its stages read the second lit colour, 32 a place generated from a lit
                               colour, 64 a place projected, or from a coordinate past the third. These
                               don't: 8 more than one place, 128 a place not the strip's own coordinate as
                               it is (from another, the vertex's place or normal, or through a matrix or a
                               second transform that changes it) */
    uint8_t places;         /* ABI 16. How many places its stages sample at (u v, u1 v1, u2 v2 of a vertex, in
                               that order), each made as gen[] says */
    uint32_t chan_colour;   /* GX's lighting when it was drawn, as SoaHostModel's four (which are as the
                               model's last strip left them): the lights are the model's */
    uint32_t chan_alpha;
    uint32_t ambient;
    uint32_t material;
    uint32_t texture1;      /* ABI 12. A second texture its stages sample (a table of colours looked up by
                               the light, say), as `texture` and the fields after it; 0 for none */
    uint32_t texture1_gen;
    uint16_t width1, height1;
    uint8_t wrap1_s, wrap1_t, linear1, levels1;
    int8_t lod_bias1;
    uint8_t pad1[3];
    uint32_t texels;        /* ABI 15. Where the texels `texture` was decoded from are in guest memory (the
                               first level's; a palettized image's palette is the bytes just before, as
                               the game keeps them), so a host that has the image from elsewhere, the
                               disc say, can know it. 0 when the image is not memory's: a copy of the
                               frame buffer, or one a mod put in its place */
    uint32_t texels1;       /* the same for `texture1` */
    uint8_t format;         /* GX's format for them: 14 CMPR, 8 C4, 5 RGB5A3 ... */
    uint8_t palette;        /* a palette's colour format: 0 IA8, 1 RGB565, 2 RGB5A3 */
    uint8_t format1, palette1;
    SoaHostTexGen gen[3];   /* ABI 16 */
} SoaHostAlphaDraw;

/* The see-through draws of the newest whole frame and their vertices, as
 * soa_host_flat_draws gives the 2D layer's; `skipped` counts the draws left
 * out (a blend that reads the frame buffer's alpha, a logic operation, a
 * texture too large to hand over). */
SOA_HOST_API long soa_host_alpha_draws(SoaHostAlphaDraw* draws, unsigned max_draws, unsigned* n_draws, SoaHostAlphaVertex* vertices,
                                       unsigned max_vertices, unsigned* n_vertices, unsigned* skipped);
SOA_HOST_API long soa_host_alpha_stages(SoaHostAlphaStage* stages, unsigned max, unsigned* n);

/* ---- the game's 2D layer (ABI 8) -----------------------------------------------------------------
 * Text, the HUD, a minimap, a dialogue box: flat draws on the screen, made
 * after the scene. A host that draws the scene itself lays them over its
 * picture in the order given. Each is triangles with one texture or none,
 * blended like a screen pass (above) with the source
 *
 *     colour0 x (1 - texel) + the vertex's colour x texel      per channel
 *
 * which is what the game's one-stage combiners come to: a texture times the
 * vertex colour (colour0 zero), or text, a register times the glyph plus
 * another. Without a texture the source is the vertex's colour.
 *
 * Reported from a frame's first such draw after its first model, until the
 * frame ends; a screen-filling quad among them (a fade over everything) is
 * one of them and not a screen pass. A frame with no model at all (a menu,
 * the title) is published too, with zero models, and then every draw on
 * the screen is one of these, from the frame's first. Left out and counted in `skipped`:
 * more than one combiner stage, a blend that reads the frame buffer's
 * alpha, a logic operation, and a textured draw whose colour0 differs from
 * vertex to vertex. The alpha test and the depth test are not applied: the
 * depth test is the host's to honour (depth_test, depth_write, depth). */
typedef struct {
    float x, y;             /* on the game's screen: 0 to 1 across and down its 640 by 480 */
    float u, v;             /* over the texture: 0 to 1 */
    uint32_t colour;        /* RGBA */
} SoaHostFlatVertex;

typedef struct {
    uint32_t first_vertex;  /* its triangles: [first_vertex, first_vertex + vertices) of the frame's, three each */
    uint32_t vertices;
    uint32_t src_factor, dst_factor, subtract;
    uint32_t colour0;       /* RGBA */
    uint32_t texture;       /* 0 for none; else which of the game's textures, for soa_host_flat_texture */
    uint32_t texture_gen;   /* moves whenever that texture's image changes */
    uint16_t width, height; /* the image's */
    uint8_t wrap_s, wrap_t; /* past its edge: 0 clamp, 1 repeat, 2 mirror */
    uint8_t linear;         /* filtered between texels */
    uint8_t passes_before;  /* how many of the frame's screen passes were drawn before it */
    uint8_t depth_test;     /* 1: drawn only where nothing nearer has written its depth (GX's less, or less
                               or equal): the 2D layer is laid out in depth, and a piece drawn early can
                               stay on top of one drawn later */
    uint8_t depth_write;    /* 1: it writes its depth, where its alpha is over 0 */
    uint8_t under;          /* ABI 12. 1: drawn before the frame's first model, so it lies under the scene
                               and shows only where the scene leaves the screen bare: a backdrop */
    uint8_t pad;
    float depth;            /* GX's screen depth of its first vertex, 0 (near) to 1 */
    float scissor[4];       /* left, top, right, bottom, as x and y above: nothing is drawn outside */
    uint32_t fog_colour;    /* ABI 17. GX's fog on it, as it is on anything drawn: after its texture, each
                               pixel's colour goes fog_amount of the way to this one (RGB; its alpha stays).
                               A minimap in a foggy place is hazed by it */
    float fog_amount;       /* 0 (none) to 1, at the depth of its first vertex */
} SoaHostFlatDraw;

/* The 2D draws of the newest whole frame and their vertices, as
 * soa_host_models gives its arrays; the frame's number, or -1. */
SOA_HOST_API long soa_host_flat_draws(SoaHostFlatDraw* draws, unsigned max_draws, unsigned* n_draws, SoaHostFlatVertex* vertices,
                                      unsigned max_vertices, unsigned* n_vertices, unsigned* skipped);

/* A draw's texture, decoded: width x height RGBA bytes, rows from the top,
 * into out. `texture` and `gen` are the draw's: the runtime keeps the last
 * few images each texture has held, since the game may have put another
 * there by the time a published frame's draws are looked at. Returns 0 when
 * that image is no longer kept, or max_bytes is too few. After the image
 * come its smaller levels of detail (ABI 12), when the draw that named it
 * samples them: *levels in all, each half the last a side and never under
 * one texel, so a third as many bytes again at most. */
SOA_HOST_API int soa_host_flat_texture(uint32_t texture, uint32_t gen, uint8_t* out, unsigned max_bytes, unsigned* width, unsigned* height,
                                       unsigned* levels);

/* bytes of the game's memory at a guest address (0x80000000 up), as the
 * game sees them -- big-endian -- into out; 0 for a range outside MEM1, or
 * before the game has drawn a model. The game may be writing there as it
 * is read: models and images stay put while they are in use. */
SOA_HOST_API int soa_host_read(uint32_t address, void* out, unsigned bytes);

/* ---- the game's memory as a frame ends, and writes into it (ABI 18) ----------------
 * soa_host_read copies memory as it stands while the game runs on, so what it
 * returns may be part one frame and part the next. A host that holds the
 * game's state to a frame names the ranges it wants, and the runtime copies
 * them as each frame ends, with the frame's models: soa_host_memory then
 * gives them as of the frame soa_host_models last returned.
 *
 * soa_host_watch_memory replaces the list (64 ranges and 128 KB in all at
 * most; a range outside MEM1, or past the limit, is kept in its place in the
 * list with nothing copied). soa_host_memory copies range `index` of the
 * list into out and returns the frame it is of, or -1 if no frame has been
 * copied with this list yet, or the range was not kept; *n is how many bytes.
 *
 * soa_host_write queues bytes for a guest address (1,024 at a time, 32 writes
 * a frame at most; 0 if it is not queued). They are written at the end of the
 * frame the game is in, after that frame's ranges are copied, so the next
 * frame begins with them.
 *
 * soa_host_hold(1) has the game wait at each frame's end that has something
 * to show, after it is published and before the queued writes are written,
 * until soa_host_go names that frame or a later one, or half a second has
 * passed: a host that works something out from one frame's memory and writes
 * it back before the next frame begins. The wait is no time of the game's:
 * its clock leaves the span out, at whatever speed the run is. soa_host_hold(0)
 * lets it run on. */
typedef struct SoaHostRange {
    uint32_t address; /* guest, 0x80000000 up */
    uint32_t bytes;
} SoaHostRange;
SOA_HOST_API void soa_host_watch_memory(const SoaHostRange* ranges, unsigned n);
SOA_HOST_API long soa_host_memory(unsigned index, void* out, unsigned max, unsigned* n);
SOA_HOST_API int soa_host_write(uint32_t address, const void* data, unsigned bytes);
SOA_HOST_API void soa_host_hold(int on);
SOA_HOST_API void soa_host_go(long frame);

/* Pause holds the guest at the end of a frame, and its clock with it (M19);
 * mute silences what soa_host_audio returns. */
SOA_HOST_API void soa_host_pause(int on);
SOA_HOST_API void soa_host_mute(int on);

/* Get to a frame fast (ABI 12): from the end of the frame the game is in
 * until it presents `frame`, its clock runs `speed` times the wall clock's
 * (0 for ten) and nothing is drawn, as SOA_SKIP_TO does from the start of a
 * run; then it is as it was. For a host that looks at one moment after
 * another of one run: the frames between, and what a pad script presses at
 * each, are the same ones. A frame already past is not a request. */
SOA_HOST_API void soa_host_skip_to(unsigned frame, unsigned speed);

#ifdef __cplusplus
}
#endif
#endif
