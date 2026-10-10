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

#endif
