/*
 * The fixed clock's quiet rule, wired (docs/CLOCK.md, "Native answers and the
 * clock"): under it every translated function's entry is a delivery point as
 * well as every back edge, so clock.c tells swap.c each time something comes
 * due, and swap.c's guard hands the entry to irq.c. A file of its own so that
 * clock.c, irq.c and swap.c each still build alone, as their tests build them.
 */
#include "clock.h"
#include "irq.h"
#include "swap.h"

void clock_quiet_start(void)
{
    if (!clock_fixed() || !g_clock_quiet) return;
    swap_due_handler(irq_quiet_entry);
    g_clock_due_fn = swap_due;
    if (g_clock_due) swap_due(1);
}
