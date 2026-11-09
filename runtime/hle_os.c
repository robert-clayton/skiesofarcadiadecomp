/*
 * Native replacements for the SDK functions listed in config/hle.txt.
 *
 * Each has the recompiled ABI (SPEC section 4.3): arguments in s->gpr[3..10],
 * the return value in s->gpr[3]. The recompiled original is still linked as
 * recomp_fn_XXXXXXXX so the two can be run against each other.
 */
#include "cpu.h"

void device_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v);

/* __OSInitAudioSystem (0x80232B90). Copies a 128-byte DSP program to
 * 0x81000000, DMAs it into ARAM, un-halts the DSP, waits for its mailbox
 * reply, then halts and resets it and restores the memory it borrowed. The
 * program only clears ARAM. The DSP is high-level emulated and ARAM starts
 * zeroed, so only the register state it leaves behind matters: AR_SIZE set,
 * the DSP halted with DSPINIT, and a reset issued -- after which the ROM
 * has announced itself in the outgoing mailbox, which DSPInit relies on. */
int audio_is_off(void); /* dsp.c: SOA_AUDIO=off */
void fn_80232B90(CpuState* s)
{
    /* With the audio's models off (SOA_AUDIO=off) the host answers the DSP and ARAM: there is nothing to leave set. */
    if (audio_is_off()) return;
    device_write(s, 0xCC005012u, 2, 0x43);
    device_write(s, 0xCC00500Au, 2, 0x8AC);
    device_write(s, 0xCC00500Au, 2, 0x8AD);
}

/* __OSStopAudioSystem (0x80232D4C). The shutdown counterpart. */
void fn_80232D4C(CpuState* s)
{
    (void)s;
}

/* The data-cache range calls (PLAN-60FPS-MODS H13b): DCInvalidateRange
 * (0x80232E38), DCFlushRange (0x80232E64), DCStoreRange (0x80232E94),
 * DCFlushRangeNoSync (0x80232EC4) and DCStoreRangeNoSync (0x80232EF0). There
 * is no data cache here -- dcbi, dcbf and dcbst translate to nothing -- so
 * each was a loop that walked its range 32 bytes at a time doing nothing but
 * count and poll for interrupts: DCInvalidateRange alone was 2.5% of the guest
 * thread in the Dangral base drawn every frame, 5.2% with the drawing out of
 * the way (FINDINGS "H13, first steps"). What the loop leaves in r3-r5, CTR
 * and CR0 is volatile across a call. The two that end in `sc` still make
 * it, for a non-empty range as the original does, so the report's syscall
 * count is unchanged; the self test holds all five to their twins. */
void fn_80232E38(CpuState* s) { s->pc = 0x80232E38u; }
void fn_80232E64(CpuState* s) { s->pc = 0x80232E64u; if (s->gpr[4]) guest_syscall(s, 0x80232E8Cu); }
void fn_80232E94(CpuState* s) { s->pc = 0x80232E94u; if (s->gpr[4]) guest_syscall(s, 0x80232EBCu); }
void fn_80232EC4(CpuState* s) { s->pc = 0x80232EC4u; }
void fn_80232EF0(CpuState* s) { s->pc = 0x80232EF0u; }
