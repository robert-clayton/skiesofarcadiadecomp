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
"Native answers and the clock" below) two things move it, and nothing else
does: no instruction the guest runs costs time by itself.

1. **A read the guest makes.** Each `mftb` (the timebase's low word) and
   each hardware register read moves it `SOA_CLOCK_READ_TICKS` (default 8)
   after it has answered. After, so `mftbu; mftb; mftbu` (OSGetTime) agrees
   on the high word; and hardware reads count because the SDK polls a
   device's status with interrupts off (ARAM's DMA busy bit at boot) and
   must see its deadline come. `mftbu` alone moves nothing. The device
   models' own reads of the timebase (`guest_time_device`) move nothing
   either, so their polling costs no guest time.
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
     same the second time, has gone round a loop that changed nothing it
     holds. That is a loop waiting on what only an interrupt can change
     (the boot's disc-load loop at `0x8021A024`, the SDK's ARQ and DSP
     polls). Work moves a register every pass -- a pointer, a count, a value
     read -- so it is never cut however long it runs (the longest run in
     the opening is 864,321 back edges, uncut). The table of states is
     emptied whenever something comes due. A wait that keeps a count in a
     register is found by length, at `SOA_CLOCK_SPIN` back edges with
     nothing due (default 4,194,304; it never fired in the runs below).

   In each case what is pending is delivered first, and the clock moves
   only when nothing was.

**Delivery points.** Under the quiet rule something *comes due* when the
clock moves (a read, a skip), the guest writes the hardware, or native code
adds an event or raises a line. The first point after that with interrupts
enabled delivers it: a loop's back edge, or the entry of any translated
function (the swap guard: `SWAP_DUE` in every guard's byte, `runtime/quiet.c`).
Nothing is delivered by a count of back edges. So a handler runs at a
program point fixed by the reads and writes before it, not by how many
instructions ran since -- which is what makes a native answer invisible
(next section).

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

**What a swap must still do to be invisible.** Read no timebase and no
hardware register, write no hardware register (those are time and device
semantics, and a native device is expected to change them). Answers that do
can say so in a later ABI (`call->charge`), if one ever needs to.

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
