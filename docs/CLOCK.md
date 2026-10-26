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
| `fixed` | moved only by the guest's reads of the clock and the hardware, and by its waits (below); the host's clock and `SOA_SPEED` are never read | checks, recordings, comparing a native system against the scaffold, a swap on against off |
| `utc` | the pre-M19 wall clock | bisecting an old timing report |

A host sets the same with `soa_host_set_clock(SOA_HOST_CLOCK_FIXED)` before
`soa_host_start` (it sets `SOA_CLOCK` for the run).

### What moves the fixed clock

The unit is the timebase tick, and the clock is a counter of them
(`clock_fixed_now`). By default (the *quiet rule*, since 2026-10-17's
"Native answers and the clock" below, with *free reads* since the same
day's "Reads cost nothing") only the guest's waits move it: no instruction
the guest runs costs time by itself, and neither does a read of the clock or
the hardware.

1. **A read the guest makes** costs `SOA_CLOCK_READ_TICKS`: 0 by default,
   8 under the old rule (below). With 8 under the quiet rule (the rule of
   the morning of 17 Oct, kept as a switch) each `mftb` and hardware read
   moves the clock 8 ticks after it has answered, so a poll loop sees its
   deadline come by reading; with 0 a poll loop is a wait like any other
   (next item). `mftbu` alone never moves it, nor do the device models' own
   reads of the timebase (`guest_time_device`).
2. **A wait.** The clock jumps to the earliest deadline ahead -- the next
   retrace, the decrementer, a disc, ARAM or AI DMA completion, a native
   event -- when the guest waits and nothing is due:
   - the scheduler has nothing to run (`hook_80237BA8`);
   - the frame end's spin on the retrace count is about to go round again
     (`tick.c`, `0x801DC49C`);
   - **any other loop that waits**, found by its state: a back edge that
     comes back to its block with every integer register, CR, LR, CTR and
     XER as that block's last pass left them, with nothing sent down the
     gather pipe since, twice running and with the floating registers the
     same the second time, and the other back edges in between the same
     both times (an inner loop's, a callee's; "A loop around a call is not
     a wait" below), and -- if a native answer ran in the last pass -- the
     guest's memory as the pass before left it too ("A native answer is not
     a wait" below), has gone round a loop that changed nothing it holds.
     That is a loop waiting on what only an interrupt can change
     (the boot's disc-load loop at `0x8021A024`, the SDK's ARQ and DSP
     polls). Work moves a register every pass -- a pointer, a count, a value
     read -- so it is never cut however long it runs (the longest run in
     the opening is 864,321 back edges, uncut). The table of states is
     emptied whenever something comes due. A wait that keeps a count in a
     register is found by length, at `SOA_CLOCK_SPIN` back edges with
     nothing due (default 4,194,304; it never fired in the runs below).

   In each case what is pending is delivered first, and the clock moves
   only when nothing was.

   With free reads three more cases, since a poll no longer moves time by
   reading:

   - **a wait with interrupts off** (MSR[EE] clear, or inside a handler or
     an event): found by its state the same way, from a table of its own
     that lasts until the next back edge with interrupts on. Nothing can be
     delivered there, so the clock goes to the next deadline and what came
     due waits for the first delivery point with interrupts on. This is the
     ARAM size probe at boot (`__ARChecksize`, a DMA's busy bit, 15 waits of
     2,089 ticks), the DSP's boot polls and AI's sample-rate calibration.
   - **no further than the loop can see.** A wait that read AI's sample
     counter goes to the counter's next step if that is sooner (it counts
     every 843.75 ticks; `__AI_SRC_INIT` waits for its edges and times them
     with OSGetTime). A wait that read the timebase (`mftb`, the
     decrementer) has no deadline to go to -- an elapsed-time loop,
     `while (OSGetTime() < t)` -- so it is stepped: 8 ticks, then twice as
     far each time the same loop is found waiting again, up to the next
     deadline. Such a loop ends late by less than it waited (8 + 16 + ... ),
     the same on every run. Each of the two tables has a step of its own,
     and the one with interrupts off starts at 8 again once they are on
     (the loop was left; before, a step outlived its loop).
   - **a decrementer exception a second time at one instant** first moves
     the clock 8 ticks. The OS's periodic alarm started at `OSGetTime()` has
     its fire time at its start until time has passed it, so with reads free
     its handler would set it again for the instant it ran, for ever (the
     boot's first frame did, 39 million times, before this).

**Delivery points.** Under the quiet rule something *comes due* when the
clock moves (a read, a skip), the guest writes the hardware, or native code
adds an event or raises a line. The first point after that with interrupts
enabled delivers it: a loop's back edge, or the entry of any translated
function (the swap guard: `SWAP_DUE` in every guard's byte, `runtime/quiet.c`).
Nothing is delivered by a count of back edges. So a handler runs at a
program point fixed by the reads and writes before it, not by how many
instructions ran since -- which is what makes a native answer invisible
(next section). Within one delivery a line native code raised
(`soa_host_raise`) is taken where its device's interrupt is, in the
device's order (PE, DSP, AI, ARAM, DI, VI, EXI, SI; under both clocks), and
with the device's own pending it runs once: a native AI block that falls on
a retrace runs before or after the retrace's handler as the device model's
would. A native answer that does a translated loop's work has no back edge of
the translation's, so it calls one: `soa_host_back_edge` (docs/SWAP.md,
"Interrupt delivery for native answers") is `irq_poll` on the registers the
answer has made the translation's, with the block's address in `s->pc`, so
a loop that waits on an interrupt is found and moves the clock as its
translation does.

**The old rule** is kept: `SOA_CLOCK_POLL_TICKS=32` (any value above 0)
makes every 16th back edge with interrupts enabled a delivery point that
moves the clock that many ticks, as before, and finds no waits by state
(the idle loop and frame end still skip). It reproduces the runs measured
below frame for frame (run A's 3,000 hashes again, 2026-10-17). At 32
ticks for 16 back edges, 2 ticks (about 24 of the Gekko's cycles) an
iteration [inferred, not measured on hardware]; at 1, the boot's
disc-load loop went round for hundreds of millions of iterations to wait
out `dvd.c`'s transfer time (3 MB/s: 14 million ticks a megabyte), 22 s of
host time for one frame [measured].

The retrace is still raised every 675,000 ticks of guest time
(`irq.c`, as before), and the game still waits two of them a frame. So the
"fixed amount per frame" is not imposed: it falls out, two fields a frame
whenever a frame's work fits in its fields, and more when a load or a
device makes the frame wait -- the same number on every run. Under the
quiet rule a frame's own work takes almost no guest time (its reads: a few
thousand ticks of 1,350,000), so a frame is late only for what it waits on:
2.06 retraces a frame over the opening's 3,000 frames, against 2.07 under
the old rule.

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
the guest happens to be. A host that wants repeatable runs turns on strict
mode (below, "Deterministic runs under a host"), which ties each of them to
a frame end, or does them on the guest's thread: in a replacement or an
event.

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
[clock] quiet: back edges cost no time; W waits found by a loop's repeated state (X went to the next deadline), Y by N back edges with nothing due (Z); the longest run of back edges that ended at something due was L
[clock] free reads: reads cost no time; O of those waits with interrupts off, T stepped for a loop on the timebase, A to AI's next sample; D decrementer exceptions a second time at one instant, 8 ticks on
```

(`P delivery points` counts the old rule's; it is 0 under the quiet rule.)

## Checking a native replacement of the platform

`soa-compare --no-hardware` (docs/RECORD.md) holds a candidate to touching
no hardware at all, even though the game's recorded call did: a native
replacement of an SDK function talks to the native device, not to MMIO.
The hardware the recorded calls touched is listed as retired, register by
register with the values seen, for the candidate to have reproduced some
other way.

## Measured (2026-10-17, this machine, the old rule)

These runs predate the quiet rule and used what is now `SOA_CLOCK_POLL_TICKS=32`.

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

## Native answers and the clock (the quiet rule)

**The problem.** Under the old rule translated code charged guest time as it
ran: its loops hit delivery points (32 ticks every 16 back edges) and the
16-edge count was global. A native answer (docs/SWAP.md) charges none, and
moves the count's phase. So with a swap on, loads finished earlier, the
delivery points fell elsewhere, and a fixed run with the swap on could not
be compared frame for frame with one with it off -- the check every swap
wants. Measured with the old rule, the 16 answers below on against off over
the opening's 3,000 frames: 388 frames differ from frame 866, and 204
functions' call counts differ outside what the answers replace.

**The options weighed.**

- *The swap guard charges what the translation would have* (recorded per
  call, or modelled per function). Exact only if the charge reproduces the
  translation's reads and back edges call by call, including the global
  16-edge phase; a lookup's walks depend on the path, a camera's on the
  scene. Per-swap work for every swap, and wrong whenever the model is.
- *A native answer declares its cost* (`call->charge(ticks)`, with helpers
  to measure it from recordings). The same model, written by hand per
  swap.
- *What the clock counts changes, so ordinary code costs nothing* and only
  true time sources move it: the guest's reads of the clock and the
  hardware, and its waits. Then an answer that reads no clock and touches no
  hardware -- every answer that does not change timing semantics -- moves
  nothing, with no work per swap. The cost is finding the waits, since a
  loop that waits on an interrupt with the clock stopped would spin for
  ever: the idle loop and the frame end were already known; the rest are
  found by their repeated state (above). And the delivery points must not
  depend on instruction counts either, hence "the first back edge or entry
  after something came due".

The third is the one taken, as the default of `SOA_CLOCK=fixed`; the real
clock is unchanged (none of this runs unless the fixed clock is on). The
loops that had to keep terminating do: the ARAM boot spin (interrupts off,
hardware reads: unchanged), `mftb` waits (reads: unchanged), the boot's
disc-load loop and the SDK's polls (found as waits: 2,650 in the opening,
none by length).

**What a swap must still do to be invisible.** Under the morning's rule
(8 ticks a read): read no timebase and no hardware register, write no
hardware register. With free reads (below, "Reads cost nothing") only the
last part is left, and one more: where the translation it stands for
*waits on a device* -- polls a status bit or the timebase until the
hardware is done -- the answer says how long, with `soa_host_charge`.

**Measured** (2026-10-17, this tree, `gen/linux-host`'s game library with a
host runtime built from it and a scratch host program; headless, muted,
`SOA_RENDER=1 SOA_HASH=1`, `SOA_COUNT_CALLS`, fresh card copies). "On" is the
16 answers: the 10 former `replace.c` functions (camera task, walk start,
pad step, floor and walls, scan, and the ship's five; soa-ue5's rules as
plain C++) and the six disc lookups (soa-ue5's `SoADiscSwap`). Counts are
compared outside what the answers replace: each function's calls less the
calls made inside an answered call (the translation's, off; the answer's own
calls back into the game, on), measured by wrapping the 16.

| run | frames | frames differing, on against off | call counts differing outside the answers | answered |
|---|---:|---|---|---|
| opening (bench pad script, blank card) | 3,000 | 0 | 0 of 1,797 functions | 2,141 camera, 4+4 walk, 164 disc |
| world map flight (secrets card, load, stick up) | 4,400 | 0 | 0 of 2,501 | 680 of each ship function, 646 scan, 1,918 camera, 301 disc |
| walking (final-boss card, load, stick down) | 4,600 | 0 | 3 of 2,026 (below) | 319 start, 85 pad step, 319 floor, 160 scan, 2,625 camera, 166 disc |
| opening, old rule | 3,000 | 388, from frame 866 | 204 of 1,796 | as the first row |

Guest time and reads are identical on against off in every quiet pair
(e.g. the opening: 4,180,950,728 ticks, 703,141 reads). The answered
functions' `translated` counts go to 0, and DVDConvertPathToEntrynum's
calls drop (67 to 44: the translated Open and ChangeDir call it). The
host runtime and the launcher give the same 3,000 hashes for the opening.
Wall time under the quiet rule: 29 to 36 s for the opening at a load average
of 20 to 26, against 27 to 31 s for the old rule.

**The one known gap: polling loops around an answer.** Walking, 9 of 2,751
waits were found one pass later with the answers on (2,742), and three
functions those loops call (`0x801C6248`, OSDisableInterrupts,
OSRestoreInterrupts) differ by 9, 117 and 9 calls in 1.3 million; the
frames do not. A wait is found by every register, and a native answer leaves
the volatile registers (r0, r4-r12, CTR, f0, f2-f13, CR0-1 and 5-7) as they
were before the call while the translation leaves its own: a loop around a
call can repeat its state on a different pass. Leaving the volatile
registers out of the state was tried and is wrong: work in a leaf loop
keeps its count there, and the opening went to 932 retraces a frame.
The other side of the same gap -- an answer that did work but left the
registers as a wait's -- was closed on 2026-10-18 ("A native answer is not
a wait" below); a poll found a pass sooner or later is still possible.

## Reads cost nothing (2026-10-17, afternoon)

**The problem.** The quiet rule made ordinary code free but kept 8 ticks a
read, and the native platform sets read far less than the SDK they replace:
before frame 2 the translated sound SDK made 13,881 hardware and timebase
reads, the native set 976. So with sound native the boot's load ended a
retrace sooner, and the disc and the pad hid the same effect with per-swap
work (a charge per elided read; "clock parity", an OSGetTick per read the
native pad leaves out). soa-ue5's layer1-sound.md, layer1-pad.md and
layer1-disc.md, "Under the quiet clock".

**The rule.** Reads cost nothing (`SOA_CLOCK_READ_TICKS` is 0 under the
quiet rule; 8 is still there to ask for). A poll is then a wait like any
other, found by its state, with three additions ("What moves the fixed
clock" above): waits with interrupts off are found too and go to the next
deadline; a wait goes no further than what it read can show (AI's next
sample; a timebase loop is stepped 8, 16, 32 ... ticks); and a second
decrementer exception at one instant moves the clock 8 ticks. Raised lines
are taken at their device's place in a delivery. Every polling pattern in
the runs below ends: the boot's ARAM probe and DSP polls (waits with
interrupts off), `__AI_SRC_INIT` (AI's sample edges, then an elapsed-time
loop), the boot's disc-load loop and the SDK's polls (as before), and the
OS's periodic alarm started at `OSGetTime()` (the decrementer rule; without
it the first frame never ended).

**What is left to a swap.** Not its reads: what the translation it stands
for *waits for*. The native sound set's AIInit and ARInit skip the SDK's
two boot probes, which wait on the hardware: `__ARChecksize` (15 ARAM DMAs,
2,089 ticks each under the ARAM model, 31,335) and `__AI_SRC_INIT` (AI's
sample-rate calibration, 3,728 ticks a call under this rule, twice from
AIInit). Without them the native boot is 38,791 ticks shorter, a periodic
alarm set just after starts at another phase, and 471 of the opening's
first 1,000 frames differ (from 427). That is time semantics, not read
accounting, so the answer says it: `soa_host_charge(ticks)` (soa_host.h,
within ABI 24, resolved by name), from inside the answer. With 31,335 for
ARInit and 7,456 for AIInit the set is invisible (below). No other swap
needs one.

**Measured** (2026-10-17, this tree's host runtime and game library, built
with `recompile.py --cc gcc --split --host --no-decomp`; soa-ue5's combined
scratch host: the disc's 5 lookups and 9 reads, sound's 46, the card's,
the pad's 42, 122 replacements; headless, muted -- the PCM goes only to a
file -- `SOA_RENDER=1 SOA_HASH=1`, `SOA_COUNT_CALLS`, fresh card copies,
seed not pinned). "On" is everything native with **no disc charge and no
pad parity**, sound's two boot charges; "off" is everything translated.

| run | frames | frames differing | PCM samples | guest ticks, on and off | retraces a frame | wall s off / on |
|---|---:|---|---|---|---:|---|
| opening (bench pad, blank card) | 6,000 | 0 | 13,002,560, identical | 8,228,925,000 | 2.03 | 135 / 137 |
| warps (final-boss card, 8 pokes) | 9,000 | 0 (5,885 distinct) | 19,528,320, identical | 12,358,575,000 | 2.03 | 231 / 215 |
| battle (final-boss card, battle pokes) | 6,000 | 0 (3,931 distinct) | 13,149,760, identical | 8,322,075,000 | 2.05 | 115 / 116 |

Each frame's guest tick is the same on and off too. Hardware and timebase
reads in the opening: 1,529,658 off, 778,386 on. Call counts differ in 126
to 137 functions outside the replaced set: the callees of the replaced SDK
functions (calls made inside an answered call are not subtracted here).
Each swap alone against off over the opening's first 1,000 frames: disc,
card and pad 0 frames differing, the same PCM and ticks; sound uncharged
471 (from frame 427), charged 0 (also over 3,000 frames, PCM identical).
The off runs of two builds of this tree (before and after the line order)
gave the same 6,000 and 9,000 hashes. Under the morning's rule the same
everything-on run differed in 4,237 of the opening's 6,000 frames. The 16-answer runs of
"Native answers and the clock" (camera, walk, ship, scan, the disc's
lookups) were not re-run with free reads; their disc lookups are part of
the set above.

Without rendering, the launcher's opening, 6,000 frames: 2.03 retraces a
frame and 23.5 s with free reads, 2.03 and 22.8 s with 8 ticks a read;
guest time 203.150 and 203.167 s. 7,750 waits found (2,650 with 8-tick
reads), 0 by length.

**The real clock** is untouched by the rule: everything above runs only
under `SOA_CLOCK=fixed` with free reads, except the place of a raised line
in a delivery, which applies to both clocks and changes nothing for a run
that raises none. The launcher's opening at `SOA_SPEED=30`, 3,000 frames:
8.81 retraces a frame, 26.9 s, frametime p50 7.7 ms, p99 24.0 ms (load
average 21; compare the table above).

**For soa-ue5's plugin.** The pad's clock parity (`bClockParity`) and the
disc's `FGuestCost` charge are no longer needed under this runtime: on
without either is identical to off. The sound set needs the two charges
(ARInit 31,335 ticks, AIInit 7,456), through `soa_host_charge` resolved by
name; `FServices` has no hook for it yet. AISetDSPSampleRate also runs
`__AI_SRC_INIT` when the rate changes (3,728 ticks); the plays above never
change it after boot, so its charge is unmeasured.

### A loop around a call is not a wait (2026-10-17, evening)

**The hang.** With free reads the crescent check below stopped at frame
3,684 (`[watchdog]`, rc 5) after 890,972 retraces, 241 a frame, and
573,406 guest seconds: 1,534,587 waits with interrupts off, 828,712 of them
stepped for the timebase. Under the real clock it ran to the end. The
waits were all one back edge, `0x80269F40` in `0x80269CA8`, inside the AI
interrupt: the sound library's frame (`0x80283478`) calls that function
five times running and keeps its count in r31, which the callee saves on
its stack and uses for itself. So the callee's back edge came by with the
same registers on every call, and the third call was taken for a wait. The
table with interrupts off lasts for the whole handler, so this happened in
every AI block; and the timebase step outlived the loop (it doubled for as
long as waits were found at that address), so each block soon moved the
clock to the next deadline, which brought the next block. Nothing about the
DVD: the backtrace's `DVDGetDriveStatus` was the interrupted thread.

**The rule now.** Each table keeps a running sum of every back edge's state
(the integer registers, CR, LR, CTR, XER and the address, mixed to 64
bits; `quiet_state_hash`). The sum between two passes of a block is what
the other back edges did meanwhile. A wait needs that sum to be the same
for both of its repeated passes as well as the registers: a loop that
waits does the same every pass, inner loops included, so it is found on
the same pass as before; the caller's back edge in between is a different
state each call, so a function called again is not. And each table has its
own timebase step; the step for waits with interrupts off starts at 8 again
when interrupts are on. `[clock] free reads` counts the repeats turned
down ("repeats not a wait"). Tests: `test_clock_fixed.py`'s
`..._called_again_by_a_loop_around_it_is_not_a_wait` (the pattern, with
interrupts off and on) and `..._entered_again_is_stepped_from_8_again`.

**Measured** (this tree; the combined host and the series of "Reads cost
nothing", SOA_THREADS=4 unless said):

| run | before (bd9bc7f) | now |
|---|---|---|
| crescent (below) | rc 5 at frame 3,684, 241.85 retraces a frame | 3,900 frames, 2.09 a frame, 135.9 guest s, 12 s |
| opening, 6,000, off and on | waits with interrupts off: 5,095 | 37; the same 6,000 hashes, PCM and ticks as before; on = off |
| warps, 9,000, off and on | 5,087 | 37; on = off, 0 frames, PCM 19,510,080 samples, 12,347,100,000 ticks; against before, frames 4,173-4,801 and 17 retraces (the false waits had lost them) |
| battle, 6,000, off and on | 4,543 | 37; on = off in PCM (13,053,760 samples) and every frame's tick; frames at SOA_THREADS=1 on = off, 0 of 6,000 (4,407 distinct); at 4, 3 to 9 frames from 4,591 on differ from run to run, off against off too (below) |

So the false waits were in every run with free reads, about 5,000 of them,
moving time in the warps; the crescent's pad found the scene where they
fed on themselves.

**Not the clock: the battle's pictures at four rasterizer threads.** Two
battle runs with everything native at SOA_THREADS=4 differ from each other
in 12 frames, and the translated run differs from a one-thread run in 3
(4,614, 4,817, 5,125), while a traced pair agrees in every wait, every
repeat turned down, every frame's tick and the PCM. The one-thread runs
repeat and agree on and off. Something in `gxr.c`'s threaded path is not
a function of the guest in that scene (open); the proofs above compare at
one thread where the four disagree.

The strict-host proof (next section) holds: runs A and B agree in all
4,000 frame hashes (2,894 distinct), the globals, the call counts and the
13,397 runs of the event; 4,000 held, 3,999 pads, none late or refused.

**The crescent check** (manual; game data). The launcher (`gen/.../soa
extracted`), `SOA_CLOCK=fixed`, a fresh copy of the secrets card,
`SOA_PAD="1600:start,2050:start,2200:a,2500:a,2800:a,3100:a,3650:a,3800:a,3950:a"`,
`SOA_FRAMES=3900`, headless and muted: it must reach frame 3,900 at about 2
retraces a frame. Under strict mode, `STRICT_PAD` with the same presses
gives `tools/strict_host.c` that script in place of its own; two strict
runs (seed 1 at up to 0.3 ms, seed 2 at up to 3 ms with four rasterizer
threads) agree in all 3,900 frame hashes (2,433 distinct), every frame's
globals and the call counts (2,225 functions); 3,900 held, none late or
refused. (Strict, bd9bc7f's rule did not hang on this script -- the host's
5 ms event is a deadline that caps the step -- but made 3,610 false waits.)

### A native answer is not a wait (2026-10-18)

**The problem.** A native answer (docs/SWAP.md) leaves the registers cleaner
than its translation: it puts back what the calling convention lets a
function clobber, where the translation leaves cursors, masks and operands
that change with what it did. So a loop around an answered call can come
back to its back edge with the registers it left last pass although the
answer did work -- in memory, where the wait's state does not look. soa-ue5
found it twice; the second time (its docs/layer3-scripts.md, "Flight-scene
false waits") the script runner at `0x80212974`, `while (...) dispatch(&pc,
1);`, with the plugin's native dispatcher: three like instructions in a row
gave the same state three times, 29 false waits, six retraces, and the
battle's reseed moved the encounter. The plugin worked round it by leaving
r0 and r4 as the translation does; every other swap could meet the same.

**The rule now.** swap.c counts every call a replacement answers
(`g_swap_answers`: `SWAP_ANSWERED`, and `SWAP_BOTH`'s, whose handler runs
after the translation), and quiet.c hands irq.c the count. A back edge
whose registers repeat, in a pass with an answer in it, also hashes the
guest's memory (MEM1, 24 MB, at the back edge; since the evening, only
the parts written since the last hash are read again: "Hashing only what
was written" below), and the pass is a repeat
that can make a wait only if memory is as the last pass left it too. So:

- a loop whose answer wrote anything (the dispatcher's cursor) is not a
  wait, whatever the answer left in the registers; with the hash checked
  before, its next passes are judged afresh;
- a poll around an answer that only reads -- a status, a count, polled until
  an interrupt changes it -- is still a wait, on the same pass as its
  translation's (the last pass's memory was hashed when its registers first
  repeated);
- a pass with no answer in it is judged as before, so a run with no
  replacements set hashes nothing and finds every wait as it did, and a
  translated loop that rewrites memory with the same values is a wait as
  before.

No answer has anything to do, and none may leave registers for the clock's
sake (the plugin's r0/r4 workaround is no longer needed). The report adds
`[clock] answers: N repeated passes had a native answer in them (memory
hashed); M of them would have been a wait but the answer had changed
memory`. Tests: `test_clock_fixed.py`'s
`..._loop_around_a_native_answer_that_did_work_is_not_a_wait` (with
interrupts on and off: an answer that moves a cursor in memory is no wait;
one that only reads is; a run with no answers as before; it fails against
the runtime before this) and `test_swap.py`'s count of answers.

**Measured** (this tree; the combined host of "Reads cost nothing" rebuilt
against it -- `all_host` with the plugin's disc, sound, card and pad, 122
replacements -- and the same host on 08b4f4b's runtime; SOA_THREADS=1,
muted, `SOA_RENDER=1 SOA_HASH=1`, fresh card copies):

| run | frames | on against off, this tree | on, 08b4f4b against this tree | PCM samples, identical | guest ticks | repeated passes with an answer hashed / turned down |
|---|---:|---|---|---|---:|---|
| opening | 6,000 | 0 frames differ, every frame's tick the same | 0; 2,655 waits both | 13,002,560 | 8,228,925,000 | 22,433 / 0 |
| warps | 9,000 | 0 (5,886 distinct) | 0; 4,172 waits both | 19,510,080 | 12,347,100,000 | 21,015 / 2 |
| battle | 6,000 | 0 (4,407 distinct) | 0 | 13,053,760 | 8,261,325,000 | 17,530 / 0 |

Wall time with the answers on, this tree against 08b4f4b: 372 / 371 s,
731 / 705 s, 372 / 342 s (the hash is about a millisecond; the load
average was 12 to 20). The warps' two turned down were each found as a wait
a pass later, at the same guest time. The strict host (next section),
4,000 frames, seed 1 at up to 0.3 ms and seed 2 at up to 3 ms with four
rasterizer threads: the same 4,000 hashes (2,894 distinct), globals, call
counts and 13,397 runs of the event, both runs and as at 08b4f4b; 4,000 held,
3,999 pads, none late or refused. The crescent check: 3,900 frames, 2.09
retraces a frame, 135.9 guest s, as before.

**The flight scene without the workaround.** soa-ue5's script swap from
before it (its commit 255ada1's parent: `FSwap::Dispatch` leaving every
register as it found it), the flight scene of its `script_swap_run.py
--flight` (secrets card, the encounter counter forced every 600 frames),
13,100 frames, `SOA_THREADS=1`:

| runtime | frames agreeing, on against off | guest ticks, on (off: 18,053,550,000) | waits found, on (off: 11,036) |
|---|---|---:|---|
| 08b4f4b | 7,594 of 13,100, from frame 3,891 | 18,077,175,000 (35 retraces more) | 11,759 |
| this tree | 13,100 of 13,100 | 18,053,550,000, and the same reads and idle skips | 11,036; 228 repeats turned down |

The same as the plugin's own fix gave (13,100 of 13,100), with no help from
the plugin. The 279 hashes cost nothing measurable there.

### Hashing only what was written (2026-10-18, evening)

**The problem.** The rule above hashes all of MEM1 -- 24 MB, 1.02 ms here --
on every repeated pass with an answer in it, and with every engine-free
owner installed (soa-ue5's progress counter: 746 to 860 bindings, battle and
people answers by the million) that is most of a run. The counter's towns
segment (final-boss card, seven warps, 25,900 frames, `SOA_THREADS=1`,
`SOA_RENDER=0`, call counting on): 585,339 hashes, 778 s, against 156 s on
08b4f4b's runtime with the same host and the same game library -- 597 s of
the 622 s more is the hash (585,339 x 1.02 ms). Not the swap guard's
count, not the trampolines, not save states: those are in both runs, and
call counting costs 10 s of the 166 below.

**The fix.** The hash is now of 4 KB units, each unit's 64 bits mixed with
its index and the terms summed (`irq.c`, `quiet_memory_hash`), so only the
units written since the last hash have to be read again. Under Linux the
kernel keeps that list: MEM1 is registered with a userfaultfd
(`UFFD_USER_MODE_ONLY`, so no privilege) for *asynchronous* write
protection (`UFFD_FEATURE_WP_ASYNC`, Linux 6.7 on), where a write to a
protected page just clears its protection in the kernel, with no signal and
no handler; `PAGEMAP_SCAN` with `PM_SCAN_WP_MATCHING` returns the pages
written since and protects them again in one call. Every write is in the
list whoever makes it -- translated code, a native answer through
`SoaHostCall::mem`, a device model's copy, a `read()` into guest memory (the
kernel's own write), another thread -- so nothing has to report its stores,
and a unit written with the values it had hashes as before. The verdict is
the whole hash's: memory as the last pass left it, byte for byte, to 64
bits. Real waits are found on the same passes, and an answer that did work
is no wait, exactly as before.

Where the kernel cannot keep the list -- another OS, a kernel before 6.7, a
sandbox without userfaultfd, an image not in whole pages -- every unit is
hashed every time: the same value, the old cost. `SOA_CLOCK_MEMORY=whole`
asks for that; `SOA_CLOCK_MEMORY=check` does both at every hash and counts
any difference (the whole hash's value is then used). The report adds

```
[clock] memory: H hashes of MEM1, the 4 KB units written since the last hashed again (the kernel's write watch): U units, W whole hashes
```

or `every unit every time (why)`, and with the check `; checked against the
whole hash: N different`. The value is not the old hash's (a sum of unit
terms, not one chain), so a snapshot taken by a runtime before this holds
loop states whose memory hash this one would not give again -- the one kind
of change docs/SNAPSHOT.md says no check sees: take such snapshots again.
Tests: `test_clock_fixed.py`'s
`..._the_memory_hash_reads_only_what_was_written` (with interrupts on and
off; page-aligned memory, as main.c's: an answer that moves a cursor,
by a store or by the kernel's `read()`, is no wait; one that reads, or
writes the value back, is; the same under `whole` and `check`, which finds
no difference; and under Linux the watch is used and reads a unit or two a
pass, not 6,144).

**Measured** (the counter's towns segment as above: soa-ue5's
progress host, every engine-free owner through the registry, 158 functions
answered; this machine, load 3 to 10):

| runtime | wall s | hashes | units of 4 KB hashed again | call counts, clock line, peeks |
|---|---:|---:|---:|---|
| 08b4f4b (no memory check) | 155.7 | 0 | -- | the same as below in every function |
| 4edff76 (whole MEM1 each time) | 777.9 | 585,339 | 3.6 billion | |
| this tree | 166.3 (160 as finally built) | 585,339 | 10,461,987 (18 a hash), and 1 whole | the same as 4edff76's, byte for byte |
| this tree, call counting off | 155.8 | 585,339 | 10,461,988 | |

(The units differ by one between runs: another thread wrote a page in one
of them at a different moment; the hash of what is there does not.) The
kernel's part is 5.2 s of the 166.

**A writer on another thread.** With rendering on, a copy to texture
(GXCopyTex) is written into MEM1 by a rasterizer worker while the guest runs
on, so a hash could fall while one was landing and see memory change between
two passes of a loop that only polls: the warps (below) have a disc poll
around the native DVDGetDriveStatus at `0x801D168C`. With the whole hash
taking a millisecond the copy had nearly always landed by the next pass (2
passes turned down at 4edff76, run after run); with the cheap one the guest
went round up to 100 times while it landed, and the number moved from run to
run (100, 5, 3; the check mode's differences were all units the kernel
listed after its scan, in a 64 KB copy at `0x80361000`: written by the
worker after the scan, none missed). Frames, ticks and PCM never moved --
a turned-down pass is found a pass later at the same guest time -- but the
poll loop's call counts did. So every copy queued into MEM1 now lands before
the hash (`gxr_memory_hazard`: the producer waits for the newest pending copy,
the report's `clock` wait; none when nothing is queued, so a run without
rendering never waits), and what is hashed is what the guest's commands have
made. The warps then have 0 passes turned down, and their two at 4edff76 were
this race too: the poll loop is now found waiting on the pass its registers
first say so, 2 calls fewer in each of its three functions than at 4edff76
(so 137 functions' counts differ off against on outside the answers, where
4edff76 had 139: those two now agree with the translation's).

**Proved** (this tree; the combined host of "Reads cost nothing", 122
replacements, `SOA_THREADS=1`, `SOA_RENDER=1 SOA_HASH=1`, muted, card copies):

| run | frames, on against off | PCM, guest ticks, every frame's tick | on, against 4edff76's rule (5fc3c17) | hashes; units read again | wall s on (5fc3c17) |
|---|---|---|---|---|---:|
| opening | 0 of 6,000 differ | identical (13,002,560 samples) | 0 frames; counts identical | 22,433; 1,478,023 | 320 (372) |
| warps | 0 of 9,000 | identical (19,510,080) | 0 frames; counts but the poll loop's 3 x 2 | 21,013, 0 turned down; 1,546,770 | 620 (731) |
| battle | 0 of 6,000 | identical (13,053,760) | 0 frames; counts identical | 17,530; 933,268 | 330 (372) |

The warps again with `SOA_CLOCK_MEMORY=check`: 0 differences, every count
the same as the plain run. soa-ue5's flight scene with the script swap
without its r0/r4 workaround (`script_swap_run.py --flight`, 13,100 frames):
13,100 of 13,100 frames and the fixed-clock totals agree on against off and
against 5fc3c17's off run; 11,036 waits, 279 hashes, 228 turned down, as at
5fc3c17; 471 s against 715. A snapshot (the progress host with every owner
but Audio, DiscRead and Card -- they schedule events with heap data -- on
the towns route, taken at 4,000 and resumed in a fresh process to 5,000):
1,000 of 1,000 frames, the clock line, the waits, the answer and turned-down
counts and the card are the uninterrupted run's; the call counts are too but
for one VI function (`0x8023F91C`, 9,994 against 9,998), which 4edff76's
runtime gives the same way (its A and this tree's A are identical).

## Save states and the clock (2026-10-18)

A snapshot (docs/SNAPSHOT.md) is the run at the top of a frame, and a run
resumed from it is that run: under the fixed clock it repeats from there as
it would have from power-on. Everything the clock's rules decide with is in
it, since a wait found a pass later moves time and every frame after it:

- the fixed clock's tick count, its statistics, and "something is due"
  (`g_clock_due`, and with it every swap guard's `SWAP_DUE` byte, set again
  from it as the run resumes);
- the quiet rule's two tables of loop states (`g_quiet`, `g_quiet_off`: each
  slot's registers, floats, gather-pipe count, running sums, the answer
  count and memory hash), their epochs and sums, the run of back edges, the
  timebase steps of both tables, and the count of native answers they compare
  (swap.c's `g_swap_answers`); the old rule's count of back edges;
- the decrementer (its value, when it was set, armed) and the tick of its
  last exception (the "second at one instant" rule), the retrace's last tick,
  AI's sample-counter base, and every device's due tick: a disc read or an
  ARAM DMA in flight, the AI block playing, a native event's.

The clock's configuration -- SOA_CLOCK, SOA_CLOCK_POLL_TICKS,
SOA_CLOCK_READ_TICKS, SOA_CLOCK_RTC, SOA_CLOCK_SPIN -- is read at the start
of the resumed run as of any run, and must be the snapshot's: it is checked,
and a resume under another is refused. (So is SOA_SEED.) The calendar
follows: the RTC is SOA_CLOCK_RTC plus the restored ticks.

**Under a strict host.** The held point is a frame's end, inside the XFB
copy, with the frame-end spin and the next frame's top still to run; that
is no place to resume, so `soa_host_snapshot` while frame F is held asks for
the snapshot at the safe point that follows: of frame F+1, taken before the
host is held at F+1. What the host gave at F is in it -- the pad latched at
F's end for frame F+1 (`g_latched`), the writes made at F's end, pads named
for later frames -- and its native events come back with their due ticks
(an event's function and data as their module and offset). The time the
guest waited at F is no guest time, so it is not in the snapshot either.
A resumed strict run's first held frame is F+1, and from there it is the
run that took the snapshot: docs/SNAPSHOT.md, "Proof", has the strict pair
(frames 3,000 to 3,999 of the opening, the 5 ms event's 6,660 runs after the
snapshot, the counts and the strict stats all the same).

**Under the real clock** a snapshot resumes with its guest time, counted on
from the host's time at the resume; such a run never repeated before the
snapshot and does not after it.

## Deterministic runs under a host (strict mode)

Under `SOA_CLOCK=fixed` the runtime repeats; a run in the editor does not
(docs/layer1-disc.md in soa-ue5, "Stage 2 in Unreal": 0.13 to 0.52/255 off
against off). What the plugin does from its own threads at its own time
lands wherever the guest happens to be:

- **The pad.** `USoAHostSubsystem::Tick` calls `SendPad` every Unreal frame,
  which calls `soa_host_set_pad` (and the native pad's `SetLive`) from the
  game thread; the guest's SI poll reads whatever is there at the retrace.
  Which guest frame sees an Unreal frame's input depends on both frame
  rates. Even with no key pressed, the port goes from absent (the script
  alone) to present (the live pad merged with the script) at the first
  Unreal tick, a time of the editor's.
- **Hold and go.** `soa.HostHold` (`ASoAMirror::Update`) makes the guest
  wait at each frame end for `soa_host_go(frame)`, but host.c gives up after
  500 ms (`host_frame_end`), so a slow editor frame lets the game run ahead.
  Without it the mirror and the tasks read and write memory at no
  particular guest point.
- **Writes.** `soa_host_write` (the camera's `Game.Write(CameraStateAt)` in
  `SoACameraBeside.cpp`, and others) is applied at the next frame end, which
  is the guest frame that happens to end next.
- **Native answers that read Unreal state** (a camera mode that hands back
  Unreal's camera) depend on the editor's frame, not the game's.
- Pause and stall (`soa_host_pause` from the heartbeat, `HoldForAssets`)
  stop the guest at a safe point and cost no guest time under the fixed
  clock: harmless. The sound queue drops rather than blocks: harmless.
  `soa.CallCounts on` from `-ExecCmds` starts counting a few seconds in:
  harmless to the picture, but counts compare only from the start
  (`GCountCallsAtStart`).

**Strict mode** (`runtime/host.c`, `soa_host.h`'s last section; within host
ABI 24, functions only) is the runtime's half.
`soa_host_set_strict(SOA_HOST_STRICT_ON)` before `soa_host_start`:

1. **Lockstep at every frame end.** The clock is fixed, and the guest waits
   at the end of every frame from the first -- shown or not, which the old
   hold skipped -- until `soa_host_go` names that frame or a later one, with
   no timeout (a futex wait; `soa_host_go` wakes it). `soa_host_held()`
   gives the frame waited at, or -1 while the guest runs. `soa_host_hold(0)`
   ends the waiting and the guarantee with it.
2. **The pad at frame boundaries.** The pad the guest reads in frame F+1 is
   the one the host gave while F was held (`soa_host_set_pad` then means
   "from the next frame"), or what `soa_host_set_pad_at(frame, port, pad)`
   named for F+1 or earlier. It is latched at F's end, after the wait and
   before the queued writes, and si.c reads the latch, never the host's
   slot. Port 1 is present and neutral from the start (the same reads as a
   port absent under a script, so `SOA_PAD` scripts mean what they did).
   `soa_host_set_pad_at` also works without strict mode.
3. **Writes, events and lines only while held.** From a host thread,
   `soa_host_write`, `soa_host_schedule`, `soa_host_cancel` and
   `soa_host_raise` are taken only while the guest is held; otherwise they
   return 0 and count as refused. From the guest's own thread (a
   replacement, an event) they are always taken. A write is made at the held
   frame's end, as before.
4. **Counting from the start**: `SOA_HOST_STRICT_COUNT_CALLS` counts every
   call from the first (`SOA_COUNT_CALLS=1`), for `soa_host_call_counts` at
   a held frame.
5. **A host can tell it held up its end**: `soa_host_strict_stats` counts
   the frame ends held, the pads latched, the pads late (given while the
   guest ran, or for a frame already begun) and writes made a frame end
   late, and the acts refused. None late and none refused: the run was a
   function of what the host did at each held frame alone.

**Measured** (2026-10-17, `tools/strict_host.c` against a host runtime built
from this tree and `gen/linux-split`'s game library; muted, `SOA_RENDER=1
SOA_HASH=1`, a fresh copy of the blank card, 4,000 frames of the opening).
The proof host drives the runtime as the editor does, from threads of its
own: a tick thread waits for the held frame, sleeps a random while, hashes a
megabyte of the game's globals (and every 100th frame all of MEM1), and asks
an input thread for the next frame's pad -- the opening's script, with the
stick and a trigger swung besides -- which sleeps a random while and calls
`soa_host_set_pad`; the tick thread sleeps again and lets the frame go. At
frame 2,000 it schedules a 5 ms event while the guest is held.

| run | host delays | rasterizer threads | wall s |
|---|---|---|---:|
| A, strict | up to 0.3 ms each, seed 1 | default | 104 |
| B, strict | up to 3 ms each, seed 2 | 4 | 114 |
| C, the same host without strict mode (the old 500 ms hold; the pad set whenever the input thread gets to it) | up to 3 ms, seed 3 | 4 | 79 |

A and B agree on every one of the 4,000 frame hashes (2,888 distinct
pictures), every per-frame hash of the globals and all 40 of MEM1, every
call count (1,823 functions), the `[si]` log of every pad change the guest
read (540), and the 13,397 runs of the event, tick for tick. Both report
4,000 frame ends held, 3,999 pads latched, none late, none refused. C parts
from them at frame 2,937, and 1,060 of its 4,000 frames differ from A's.
The load average was 18 to 33 over the three runs.

To run it: build a host runtime (`recompile.py --cc gcc --split --host`),
then `gcc -Iruntime tools/strict_host.c -L<out> -l:libsoa_runtime.so
-Wl,-rpath,<out> -pthread`, and `SOA_FRAMES=4000 SOA_HASH=1 strict_host
<out>/libsoa_game.so <disc> <root> SEED MAXDELAY_US strict OUT`.
`test_host_strict.py` checks the same in the suite with the real `host.c`
and a stand-in guest: every read in frame F+1 sees the pad given while F
was held, a write made at F is there from F+1, a pad named with
`soa_host_set_pad_at` comes in on its frame, acts before the start are
counted late and refused, and two runs with different delays print the same.

### What the plugin must change to use it

All in soa-ue5's `Plugins/SoAHost`; nothing there has changed yet.

1. **`FSoAHostRuntime`**: resolve `soa_host_set_strict`, `soa_host_held`,
   `soa_host_set_pad_at` and `soa_host_strict_stats` by name
   (`Export(...)`), as optional: the ABI stays 24 and the check against
   `SOA_HOST_ABI` is unchanged. A runtime without them has no strict mode.
2. **A switch read before the start**: `soa.Deterministic` (and
   `-SoADeterministic`), in `FSoAHostRuntime::Start` before `StartFn`:
   `soa_host_set_strict(SOA_HOST_STRICT_ON | (GCountCallsAtStart ?
   SOA_HOST_STRICT_COUNT_CALLS : 0))`; then no `CountCallsFn(1)` after the
   start (the flag counts from the first call) and no `Hold` calls at all.
3. **`USoAHostSubsystem::Tick` in that mode**: `F = soa_host_held()`. While F
   is -1 the guest is between frame ends: show the newest picture and do
   nothing that reaches the game (no `SendPad`, no `Game.Write`, no
   schedule). When F >= 0: `Mirror->Update()` (its reads of frame F and its
   tasks' writes), then `SendPad()` (both `soa_host_set_pad` and the native
   pad's `SetLive`), then `soa_host_go(F)`, last. Today `SendPad` runs after
   `Mirror->Update` has let the frame go, which in strict mode would make
   every pad late. A tick may take more than one held frame (loop while
   `soa_host_held()` moves on, within a time budget) to keep the game at
   speed.
4. **`ASoAMirror::Update` in that mode**: leave `soa.HostHold` alone (no
   `Hold(bHold)`) and drop its `ON_SCOPE_EXIT` `Go(HeldFrame)`. The
   subsystem lets the frame go; the frame held may have nothing to show,
   and the mirror's `HeldFrame` (the last published frame) would never name
   it.
5. **The native pad** (`SoAPadHost`'s `SetLive`): called only from step 3,
   while the guest is held, so its `Read` on the guest's thread in frame F+1
   sees the pad given at F. No other change.
6. **Writes from tasks** (`SoACameraBeside`'s `Game.Write(CameraStateAt)`
   under `soa.CameraBeside 2`, and any other `soa_host_write`): only from
   inside the held tick. A write at any other time now returns 0 and is
   counted refused, where before it landed at whichever frame end came
   next.
7. **Native answers read guest memory only.** The camera, party, ship and
   scan rules already do; `soa.CameraLag` changes Unreal's camera, not the
   game's. Anything that would hand the game Unreal's camera or time stays
   off in this mode.
8. **The check**: at the run's end (or any held frame) log
   `soa_host_strict_stats`; `late` and `refused` must be 0. Frame hashes:
   `SOA_HASH=1` in the editor's environment, or the subsystem's own hash of
   `soa_host_frame`'s picture at each held frame. `SOA_PAD_RECORD` writes
   down the pad the guest read, by guest frame, so a run in the editor can
   be replayed headless with `SOA_PAD_FILE`, the same card and the same
   build. Pause, the asset stall and the heartbeat's `soa_host_pause` stay
   as they are: under the fixed clock they cost no guest time.
