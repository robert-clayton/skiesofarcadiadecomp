/*
 * Functions of the game's that a host may answer in the game's place
 * (soa_host.h, soa_host_replace).
 *
 * Each is bound in config/hle.txt, so every caller reaches the function here
 * and the translation survives as recomp_fn_XXXXXXXX (a twin in
 * config/seam.txt, for a split build). With no host, or a host that has not
 * asked for the address, the function here only runs the translation: the
 * game is the game. A host that has asked is called first, on the guest's
 * thread, with the game's memory and the arguments, and says whether it has
 * answered the call, or the game is to answer it, or the game is to answer
 * it and the host be called once more afterwards (to hold its own answer to
 * the game's).
 *
 * The one so far is the field camera's task (0x80096DA4): it reads the
 * camera's request, the party, the pad and the ground, and writes the camera
 * the next frame is drawn with.
 */
#include "cpu.h"

void recomp_fn_80096DA4(CpuState* s);
#ifdef SOA_HOST
int host_answer(CpuState* s, uint32_t address, int after); /* host.c */
#endif

static void replaceable(CpuState* s, uint32_t address, void (*game)(CpuState*))
{
#ifdef SOA_HOST
    {
        int how = host_answer(s, address, 0);
        if (how == 1) return;
        game(s);
        if (how == 2) host_answer(s, address, 1);
    }
#else
    game(s);
#endif
}

/* (each stores its address first, as the translation's block head does: a
 * sample in here names it. test_hle_pc holds every binding to that.) */
void fn_80096DA4(CpuState* s)
{
    s->pc = 0x80096DA4u;
    replaceable(s, 0x80096DA4u, recomp_fn_80096DA4);
}
