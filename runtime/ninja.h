/*
 * The Ninja model layer's feed (runtime/ninja.c): for a consumer that wants
 * each chunk model as the game draws it -- host.c's, for a program that
 * rebuilds the scene itself. Set, it is called on the guest's thread for
 * every model a drawer is handed and every texture a model's chunks select,
 * in the order they happen; NULL turns it off.
 */
#ifndef SOA_NINJA_H
#define SOA_NINJA_H
#include "cpu.h"

typedef struct {
    /* drawer: which of the four (0-3); vlist, plist: the model's lists as
     * the struct names them right now (a drawer can be handed one struct
     * refilled for each piece); modelview: the matrix on top of Ninja's
     * stack; camera: stack level 1, the view matrix. Both 3x4, rows. */
    void (*model)(CpuState* s, unsigned drawer, uint32_t model, uint32_t vlist, uint32_t plist, const float modelview[12],
                  const float camera[12]);
    /* the texture a model just selected, resolved in the list current now;
     * palette is the colour format of a palettized image's entries (GX_TL_*:
     * 0 IA8, 1 RGB565, 2 RGB5A3), which come first at `image`, the texels
     * after them */
    void (*texture)(CpuState* s, uint32_t id, uint32_t image, uint32_t format, uint32_t palette, uint32_t width,
                    uint32_t height, const char* name);
    /* the drawer the last `model` entered is returning: drawn says whether
     * it drew (its return value is not -1, the clip test's "out of view").
     * GX's state is now as the model's last strip left it. May be NULL. */
    void (*end)(CpuState* s, int drawn);
} NinjaFeed;

void ninja_set_feed(const NinjaFeed* feed);

#endif
