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
    /* drawer: which of the four (0-3); modelview: the matrix on top of
     * Ninja's stack; camera: stack level 1, the view matrix. Both 3x4, rows. */
    void (*model)(CpuState* s, unsigned drawer, uint32_t model, const float modelview[12], const float camera[12]);
    /* the texture a model just selected, resolved in the list current now */
    void (*texture)(CpuState* s, uint32_t id, uint32_t image, uint32_t format, uint32_t width, uint32_t height,
                    const char* name);
} NinjaFeed;

void ninja_set_feed(const NinjaFeed* feed);

#endif
