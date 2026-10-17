# The guest's clock, and work at a guest time

Every timer, interrupt and device deadline in the port counts in one clock:
the Gekko timebase the game reads with `mftb`, 40.5 MHz, 675,000 ticks a
60 Hz field. By default that clock follows the host (`runtime/clock.c`,
M19): host monotonic time times `SOA_SPEED`, with long gaps and pauses left
out. A run under it does not repeat. Retraces per game frame ranged from 2
to over 40 in a 12,000-frame bench, the random seeds come from the clock at
every field load and battle start, and the title's music waits on it. This
file is the clock's other mode, which makes a run repeat exactly, and the
way native code gets work done at a precise guest time.

## The two clocks

| `SOA_CLOCK` | guest time is | for |
|---|---|---|
| unset (or `real`) | host monotonic time times `SOA_SPEED`; gaps over `SOA_CLOCK_GAP_MS` and pauses excluded | play |
| `fixed` | moved by the guest's own work only (below); the host's clock and `SOA_SPEED` are never read | checks, recordings, comparing a native system against the scaffold |
| `utc` | the pre-M19 wall clock | bisecting an old timing report |

A host sets the same with `soa_host_set_clock(SOA_HOST_CLOCK_FIXED)` before
`soa_host_start` (it sets `SOA_CLOCK` for the run).

### What moves the fixed clock

The unit is the timebase tick, and the clock is a counter of them
(`clock_fixed_now`). Three things move it, and nothing else does:

1. **A read the guest makes.** Each `mftb` (the timebase's low word) and
   each hardware register read moves it `SOA_CLOCK_READ_TICKS` (default 8)
   after it has answered. After, so `mftbu; mftb; mftbu` (OSGetTime) agrees
   on the high word; and hardware reads count because the SDK polls a
   device's status with interrupts off (ARAM's DMA busy bit at boot) and
   must see its deadline come. `mftbu` alone moves nothing. The device
   models' own reads of the timebase (`guest_time_device`) move nothing
   either, so their polling costs no guest time.
2. **A delivery point.** Every 16th back edge of a loop running with
   interrupts enabled (`irq_poll`) moves it `SOA_CLOCK_POLL_TICKS` (default
   32). This is an instruction count of a coarse kind: a loop that waits on
   a flag an interrupt sets makes time pass by running. 32 ticks for 16
   back edges is 2 ticks, about 24 of the Gekko's cycles, an iteration: a
   plausible size for a small loop [inferred, not measured on hardware]. At
   1, the boot's disc-load loop at `0x8021A024` (it calls `0x80219B7C` until
   the read is in, pumping `0x801D150C`) went round for hundreds of millions
   of iterations to wait out `dvd.c`'s transfer time (3 MB/s: 14 million
   ticks a megabyte), 22 s of host time for one frame [measured: the
   frametime maximum, and a sample of the polls' return addresses]. At 32 the 600 frames after
   boot take 3.2 s, and retraces a frame are the same (2.27 against 2.26).
3. **A wait.** When the scheduler has nothing to run (`hook_80237BA8`), or
   the frame end's spin on the retrace count is about to go round again
   (`tick.c`, `0x801DC49C`), and the delivery there found nothing due, the
   clock jumps to the earliest deadline ahead: the next retrace, the
   decrementer, a disc, ARAM or AI DMA completion, a native event. The next
   pass delivers it. This is where most guest time goes, and why a fixed run
   is faster than the game: a frame takes the time its work takes, then
   skips to its retrace. (Without the spin's case the frame end spun a
   field's worth of delivery points every frame: 122 ms of host time a
   frame.)

The retrace is still raised every 675,000 ticks of guest time
(`irq.c`, as before), and the game still waits two of them a frame. So the
"fixed amount per frame" is not imposed: it falls out, two fields a frame
whenever a frame's work fits in its fields, and a measured number of fields
when it does not -- the same number on every run.

**The calendar.** The RTC reads `SOA_CLOCK_RTC` (seconds since 2000-01-01,
default 31,622,400: 2001-01-01) plus the guest seconds so far, so save
stamps and the OS's calendar repeat too.

**What still has to be the same** for two runs to agree: the game library
and the runtime, the disc, the pad input (`SOA_PAD` scripts are keyed by
frame, as are recordings), and the memory card image at the start. The seed
does not have to be pinned: the reseeds read the fixed clock, so they repeat
on their own; `SOA_SEED` still pins them to chosen values.

**What it does not cover.** Anything a host does from its own thread at its
own time: pads set by `soa_host_set_pad` from a UI thread, a write with
`soa_host_write`, an event scheduled from another thread. Those land where
the guest happens to be. A host that wants repeatable runs does them on the
guest's thread: at the held frame end (`soa_host_hold`), in a replacement,
or in an event.

## Work at a guest time

Native sound has to call the game's sound driver every 5 ms of sound, in
what was interrupt context, in the middle of a frame (soa-ue5's
`docs/layer1-platform.md`, section 3). Two primitives give it that, in
`runtime/irq.h` for runtime code and `soa_host.h` (ABI 24) for a host:

| runtime (`irq.h`) | host (`soa_host.h`) | does |
|---|---|---|
| `irq_event_add(due, period, fn, user, extra)` | `soa_host_schedule(at, period, fn, user)` | run `fn` at guest tick `due`, then every `period` ticks |
| `irq_event_cancel(id)` | `soa_host_cancel(id)` | cancel it |
| `irq_raise(number)` | `soa_host_raise(number)` | call the game's handler for interrupt `number` at the next delivery point |
| `irq_guest_ticks(s)`, `hle_guest_ticks()` | `soa_host_guest_ticks()` | guest ticks now |

**An event** runs on the guest's thread, inside a delivery, the way the
game's own interrupt handlers are run (`call_guest_handler`): every
register of the interrupted code is put back afterwards, MSR[EE] is clear
while it runs, nothing is delivered inside it, and if it woke a guest
thread the scheduler runs after it. It runs at the first delivery point at
or after its tick: the idle loop, or a back edge with interrupts enabled.
A periodic event is next due `period` ticks after the tick it was *due*,
not the tick it ran, so a 5 ms tick (202,500 ticks) does not drift; one
several periods behind runs once a period, up to 64 times a pass. The host's
`fn` gets a `SoaHostCall` with the game's memory and `game` and
`game_original` to call the game's functions (the driver's AI callback, say).
32 events can be pending.

**A raised line** is a number of the OS's interrupt table (5 AI, 6 ARAM, 7
DSP, 9-16 EXI, 18-19 PE, 20 SI, 21 DI, 24 VI). At the next delivery point
the game's handler in that slot is called with it, as `irq.c` calls it for
the device models: once, however many times it was raised before then.
This is option 1 of the sound design: native code says "AI is pending" and
the game's handler -- or the native replacement of it -- runs where the
device's would have.

**How precise.** Under the fixed clock an event that falls while the game
is idle runs on its tick exactly: the idle loop goes straight to the
earliest deadline, events included. One that falls while the game is busy
runs at the next delivery point; the report's `[irq] native:` line gives
the worst lateness and the mean. Under the real clock the same delivery
points apply, at host time.

**Repeatable** only when scheduled from the guest's thread (at boot from a
replacement, from inside another event, at the held frame end); an event
scheduled from another thread lands where the guest happens to be.

The report adds, when used:

```
[irq] native: N events run, late by at most T ticks (U us), M ticks on average; L raised lines delivered, K with no handler installed
[clock] fixed: T ticks of guest time (S s): R timebase and hardware reads, P delivery points, I idle skips for J ticks
```

## Checking a native replacement of the platform

`soa-compare --no-hardware` (docs/RECORD.md) holds a candidate to touching
no hardware at all, even though the game's recorded call did: a native
replacement of an SDK function talks to the native device, not to MMIO.
The hardware the recorded calls touched is listed as retired, register by
register with the values seen, for the candidate to have reproduced some
other way.

## Measured (2026-10-17, this machine)

Runs of the opening: the split build's game library (`gen/linux-split`)
with a runtime built from this tree, headless, muted, the bench pad script
(`1600:start,1640:a,...,3600:a@150`), each on a fresh copy of one card image,
`SOA_RENDER=1 SOA_HASH=1` (a hash of every presented frame) and
`SOA_COUNT_CALLS` (every call of every translated function, counted).

**Fixed runs repeat.** Each pair below is two runs with nothing in common but
the inputs:

| pair | frames | runs | frame hashes | call counts | card after |
|---|---:|---|---|---|---|
| A, B | 3,000 | A: `SOA_SPEED=30`; B: `SOA_SPEED=1` with 16 busy loops beside it (load average 14.5) | all 3,000 the same (1,894 distinct pictures) | the same in every one of 1,796 functions (595,504,707 calls) | byte for byte |
| C1, C2 | 6,000 | C1: `SOA_SPEED=7 SOA_PACE=1`; C2: `SOA_SPEED=1` with 16 busy loops (load average 34.8) | all 6,000 the same; C1's first 3,000 are A's | the same in all 1,868 functions (898,798,914 calls) | byte for byte |

A third run of the same 3,000 frames with the runtime as finally built (F)
gave A's hashes and counts again. Each fixed run: 2.07 retraces a frame over 3,000 frames (2.03 over 6,000),
103.3 guest seconds; 23.5 s of wall time for A, 39.4 s for B under load.
Without rendering a fixed 3,000 frames take 6.6 s.

**The real clock is as it was**, and does not repeat. The same opening at
`SOA_SPEED=30`, twice with the runtime before this change and twice with it:

| | retraces a frame | guest s | wall s | frametime p50 / p99 ms | functions called |
|---|---:|---:|---:|---|---:|
| before, 1 | 8.12 | 710.9 | 23.7 | 7.5 / 21.6 | 1,744 |
| before, 2 | 7.74 | 698.2 | 23.3 | 7.4 / 21.4 | 1,744 |
| after, 1 | 7.91 | 705.3 | 23.5 | 7.4 / 20.4 | 1,743 |
| after, 2 | 7.93 | 716.0 | 23.9 | 7.2 / 21.2 | 1,796 |

Frame hashes part at frame 876 in every pair of real runs: 388 of 3,000
differ between the two "before" runs, 1,325 between the two "after" runs,
455 between a "before" and an "after"; 277 functions have different call
counts between the two "before" runs. Under the real clock the change adds
one untaken test to each guest timebase and hardware read and to each
delivery (no events pending, no lines raised), and nothing else on the
guest's path; the frame end's wait and the idle skip are not set.

**A 5 ms event in the game.** A scratch runtime (not this tree's) scheduled
one periodic event of 202,500 ticks from boot under the fixed clock, over
3,000 frames: 20,666 ticks ran; 19,369 of them under 16 ticks late, 1,294
under 64, 2 under 256, and one 24,272 ticks (0.6 ms) late, mean 3.0 ticks.
Two runs (`SOA_SPEED=30` and `1`) gave the same report to the tick. The run's
call counts are not run A's: an event is a delivery, and a delivery runs the
scheduler after it.

**Recording** under the fixed clock: see docs/RECORD.md (interrupts are
still held inside recorded calls).
