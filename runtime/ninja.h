/*
 * The Ninja model layer's feed (runtime/ninja.c): for a consumer that wants
 * each chunk model as the game draws it -- host.c's, for a program that
 * rebuilds the scene itself. Set, it is called on the guest's thread for
 * every model a drawer is handed and every texture a model's chunks select,
 * in the order they happen; NULL turns it off.
 *
 * A visit is one model handed to a drawer once. The game hands most models
 * over twice a frame, once for their opaque strips and once for the ones
 * that use alpha (`mode`), and the second kind it does not draw then: it
 * records them into a GX display list and calls the list later, in an order
 * of its own. So when a visit's strips reach the GPU is told apart from
 * when the drawer ran: `end` for the ones drawn at once, `sent` for the
 * recorded ones, when their list is called.
 */
#ifndef SOA_NINJA_H
#define SOA_NINJA_H
#include "cpu.h"

/* One model handed to a drawer. */
typedef struct {
    unsigned visit;  /* its number in the frame, from 0 */
    unsigned drawer; /* which of the four (0-3) */
    unsigned mode;   /* which of the model's strips the drawer is set to draw (NINJA_MODE_*) */
    /* Ninja's constant attribute: a strip chunk's flags are and-ed with the
     * first and or-ed with the second before the drawer looks at them, so
     * 0x08 in strip_or makes every strip one that uses alpha (how the game
     * fades a whole model). 0xFF and 0 when it is off. */
    unsigned strip_and, strip_or;
    uint32_t model;        /* the NJS_CNK_MODEL */
    uint32_t vlist, plist; /* its lists as the struct names them right now: a drawer can be handed one
                              struct refilled for each piece */
    uint32_t vlist_bytes;  /* not 0: the game refills this vertex list for one model after another (a
                              shape it works out afresh each time), so only now is it this model's.
                              Its length, end chunk included, for whoever wants to keep a copy */
    float modelview[12];   /* the matrix on top of Ninja's stack; 3x4, rows */
    float camera[12];      /* stack level 1, the view matrix; 3x4, rows */
} NinjaVisit;

typedef struct {
    void (*model)(CpuState* s, const NinjaVisit* v);
    /* the texture a model just selected, resolved in the list current now;
     * palette is the colour format of a palettized image's entries (GX_TL_*:
     * 0 IA8, 1 RGB565, 2 RGB5A3), which come first at `image`, the texels
     * after them */
    void (*texture)(CpuState* s, uint32_t id, uint32_t image, uint32_t format, uint32_t palette, uint32_t width,
                    uint32_t height, const char* name);
    /* the drawer the last `model` entered is returning: drawn says whether
     * its clip test let the model through (its return value is not -1, "out
     * of view"). Then its strips went one of two ways. Recorded: into the
     * display list the game is recording, and `sent` follows when that list
     * is called. Else to the GPU, `strips` of them, and when that is not 0
     * GX's state is now as the last of them left it. May be NULL. */
    void (*end)(CpuState* s, unsigned visit, int drawn, int recorded, unsigned strips);
    /* a recorded visit's strips have just reached the GPU, `strips` of them,
     * inside the list the game is calling; GX's state is as the last of
     * them left it. May be NULL. */
    void (*sent)(CpuState* s, unsigned visit, unsigned strips);
} NinjaFeed;

/* Which of a model's strips a drawer draws: Ninja's draw mode, a global the
 * game sets before it hands models over. A strip chunk whose flags have
 * 0x08 (NJD_FST_UA, use alpha) is a see-through one. */
enum {
    NINJA_MODE_ALL = 1,    /* every strip */
    NINJA_MODE_OPAQUE = 2, /* the ones without the flag */
    NINJA_MODE_ALPHA = 3,  /* the ones with it */
    NINJA_MODE_ALPHA_BACK_FRONT = 4, /* those, twice: culled the other way round, then the usual way */
    NINJA_MODE_ALPHA_BACK = 5,       /* the first of those two passes alone */
    NINJA_MODE_ALPHA_FRONT = 6       /* the second alone */
};

void ninja_set_feed(const NinjaFeed* feed);

#endif
