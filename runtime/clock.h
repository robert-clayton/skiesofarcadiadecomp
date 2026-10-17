/*
 * The guest's clock (M19): monotonic, blind to host sleeps and stalls longer
 * than a threshold, pausable, and continuous across speed changes. See
 * clock.c. Times are nanoseconds of guest time; hle.c turns them into
 * timebase ticks.
 */
#ifndef SOA_CLOCK_H
#define SOA_CLOCK_H

#include <stdint.h>

/* The speed (SOA_SPEED), the gap threshold in ms (0 off, -1 keep the
 * default 2000), and a function told of each gap, for the log. */
void clock_configure(unsigned speed, long gap_ms, void (*gap_note)(double gap_s));

uint64_t clock_host_ns(void);                  /* the host's monotonic time */
uint64_t clock_advance_at(uint64_t host_ns);  /* guest ns, advancing to host_ns (the guest thread) */
uint64_t clock_peek_at(uint64_t host_ns);     /* guest ns at host_ns, writing nothing (the report) */
void clock_set_speed_at(unsigned speed, uint64_t host_ns);
void clock_exclude_at(uint64_t from_host_ns, uint64_t to_host_ns); /* a wait of the guest thread's own: no guest time */
void clock_pause(int on);                     /* any thread; takes effect at the next read */
int clock_pause_requested(void);
unsigned clock_epoch(void);                   /* bumped by each gap, speed change and resume */
unsigned clock_speed(void);
int clock_started(void);
double clock_origin_seconds(uint64_t since_host_ns); /* the first read, after a host time */
unsigned long long clock_gaps(void);
double clock_excluded_seconds(void);
void clock_reset(void);                       /* tests */

/* The fixed-step clock (SOA_CLOCK=fixed; docs/CLOCK.md), in timebase ticks:
 * moved by the guest's work alone, so a run repeats exactly. */
void clock_fixed_configure(uint64_t read_ticks, uint64_t poll_ticks, uint32_t rtc_seconds);
int clock_fixed(void);                        /* 1: the fixed-step clock is the guest's */
uint64_t clock_fixed_now(void);               /* guest ticks now, moving nothing */
void clock_fixed_after_read(void);            /* a guest mftb or hardware read answered: `read` ticks on */
void clock_fixed_poll(void);                  /* a delivery point at a loop's back edge: `poll` ticks on */
int clock_fixed_skip_to(uint64_t tick);       /* the idle loop: forward to a deadline; 0 if it is not ahead */
uint32_t clock_fixed_rtc(void);               /* the RTC: the fixed start date plus guest seconds */
void clock_fixed_stats(unsigned long long* reads, unsigned long long* polls, unsigned long long* skips,
                       uint64_t* skipped_ticks);

/* The quiet rule (poll ticks 0: the default of SOA_CLOCK=fixed; docs/CLOCK.md
 * "Native answers and the clock"): no instruction costs guest time, and no
 * count of back edges is a delivery point. g_clock_due is set whenever
 * something may have come due -- the clock moved (a read, a skip), the guest
 * wrote the hardware, a native event or line was added -- and the next back
 * edge or translated function's entry with interrupts enabled delivers and
 * clears it. A loop found waiting (irq.c: its state repeated, or `spin` back
 * edges with nothing due) has what is pending delivered, and with nothing,
 * guest time goes to the next deadline, as in the idle loop. */
extern int g_clock_quiet;        /* 1: the quiet rule is on */
extern volatile int g_clock_due; /* something may be due: the next delivery point delivers */
void clock_fixed_set_spin(unsigned long long back_edges); /* SOA_CLOCK_SPIN */
unsigned long long clock_fixed_spin(void);
void clock_fixed_mark(void);     /* something may be due (the quiet rule only), from any thread */
int clock_fixed_take_due(void);  /* the guest's thread: 1 if something was, and no longer */
extern void (*g_clock_due_fn)(int on); /* told when g_clock_due changes (quiet.c: swap.c's entries look too) */
void clock_fixed_note_spin(unsigned long long run, int how); /* irq.c: a run of back edges ended (clock.c says how) */
void clock_fixed_spin_stats(unsigned long long waits[5], unsigned long long* longest); /* by clock_fixed_note_spin's how */

#endif
