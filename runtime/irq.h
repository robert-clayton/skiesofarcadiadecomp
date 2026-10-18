/*
 * Native sources of interrupts (irq.c; docs/CLOCK.md): work native code wants
 * done at a precise guest time, and interrupt lines it raises.
 *
 * Guest time here is the timebase, 40.5 MHz ticks (irq_guest_ticks). An
 * event runs on the guest's thread at the first delivery point at or after
 * its tick -- the scheduler's idle loop, or a loop's back edge with MSR[EE]
 * set, every 16th -- inside a delivery as an interrupt handler would: every
 * register is put back after it, MSR[EE] is clear while it runs, nothing is
 * delivered inside it, and if it woke a guest thread the scheduler runs after.
 * Under the fixed-step clock the idle loop goes straight to the earliest
 * event's tick, so an event that falls while the guest idles runs on it
 * exactly; one that falls while the guest is busy runs at the next delivery
 * point, and the report gives the worst lateness.
 *
 * Everything here may be called from any thread. Only events added from the
 * guest's own thread (at boot, from a replacement, from another event) are
 * part of a run that repeats; one added from another thread lands where the
 * guest happens to be.
 */
#ifndef SOA_IRQ_H
#define SOA_IRQ_H
#include <stdint.h>

struct CpuState;

/* An event's work: `user` and `extra` as irq_event_add was given them, `due`
 * the tick it was due at, `now` the tick it runs at. */
typedef void (*IrqEventFn)(struct CpuState* s, void* user, void (*extra)(void), uint64_t due, uint64_t now);

#define IRQ_EVENTS 32 /* events pending at once */

/* Run fn at guest tick `due`, and if period is not 0 every `period` ticks
 * after (counted from the due tick, so it does not drift); `extra` is a
 * function of the caller's carried along (host.c's: the host's own). Returns the
 * event's id, or 0 when the table is full or fn is NULL. */
int irq_event_add(uint64_t due, uint64_t period, IrqEventFn fn, void* user, void (*extra)(void));
/* Cancel an event; 1 when it was pending. From inside its own run it stops
 * the next. */
int irq_event_cancel(int id);
/* Mark interrupt `number` (the OS interrupt table's, 0-31: 5 AI, 7 DSP, ...)
 * pending: at the next delivery point the game's handler in that slot is
 * called with it, once however many times it was raised. 0 for a bad number. */
int irq_raise(unsigned number);
int irq_charge(uint64_t ticks); /* soa_host_charge: the fixed clock on `ticks` (the guest's thread) */
/* Guest ticks now, as the devices read them, on the guest's thread (it moves
 * nothing under the fixed-step clock). From another thread, hle_guest_ticks. */
uint64_t irq_guest_ticks(struct CpuState* s);

/* A wait in the game's own code the runtime recognises (tick.c's frame-end
 * spin): under the fixed-step clock, deliver what is due or go to the next
 * deadline, as the idle loop does; nothing under the real clock. */
void irq_fixed_wait(struct CpuState* s);
/* The fixed clock's quiet rule (docs/CLOCK.md): a translated function's entry
 * delivers what has come due, as a back edge does (swap.c calls it). */
void irq_quiet_entry(struct CpuState* s);

/* hle.c: guest ticks now from any thread, moving and writing nothing; 0
 * before the guest's first read of the timebase. */
uint64_t hle_guest_ticks(void);

#endif
