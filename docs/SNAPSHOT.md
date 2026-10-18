# Save states

Every check of the port boots the game from power-on and plays to the scene
it looks at: 4,500 to 14,000 frames, minutes a run, twice a check, headless
and in Unreal. A **snapshot** is the runtime's whole state at a frame
boundary, written to a file; a fresh process **resumed** from it runs on as
the run it was taken from did, frame for frame, so a check starts at its
scene. This is how it works, what it holds, how to use it, and how it was
proved.

## Where a snapshot is taken

The game is translated C (docs/ARCHITECTURE.md): a guest call is a host
call, so wherever the guest is, the host's C stack holds every translated
function between `__start` and there, each part-way through its body. A file
can hold the guest's memory and registers, but not a host stack. So a
snapshot is taken where that stack is as short as it gets and the same every
time: **the top of the game's main loop**, the call of VIGetRetraceCount at
`0x801DCB84` in main (`0x801DCB28`) -- tick.c's safe point. There the last
frame has been presented and the next has not begun; no interrupt handler,
native event or recorded call is running (each is checked); the guest is
its one thread, not parked in SelectThread (a second thread on a fiber of its
own, Windows only, is refused); and the host's stack is `__start` and main.

**The way back in.** A translated function can only be entered at its head,
so the recompiler gives the function holding each address of
`config/resumepoints.txt` a second entry (emit.py, `RESUME_SPR`):

```c
void fn_801DCB28(CpuState* s)
{
    if (s->spr[1000] != 0u) { /* a snapshot resumes here */
        switch (s->spr[1000]) {
        case 0x801DCB84u: s->spr[1000] = 0u; goto L_801DCB84; /* MainLoop.frame_top */
        default: break;
        }
    }
    SOA_SWAP(4413);
    ...
```

SPR 1000 is no register of the Gekko's and no instruction of the game names
it (checked over the whole translation). A translated function keeps the
guest's state in `CpuState` and memory, never in C locals across a block, so
entering at the block with the registers restored is the same as having run
to it. The entry is before the swap guard: a resume is not a call, and
nothing is counted or delivered on the way in. The list is a baked input
(player_build.py's `BAKED`; a new point is a retranslation), and the game
library's build record names the points (`resume=801DCB84`, seam.record):
the runtime refuses to resume with a library that has none, rather than run
main from its head over a restored memory. recompile.py refuses a point no
translated function holds.

So a resumed run is: the process starts as any run does (the disc opened, the
switches read, the game library loaded, the host's replacements set), the
snapshot's sections are loaded into the runtime's statics, and main is
entered at `0x801DCB84` with every register as it was. Its first act is the
safe point again (not snapshotted a second time), then frame N's work.

**Frame numbers.** A snapshot "at frame N" is taken at the safe point where
`gx_frame_count()` is N: frames 0 to N-1 have been presented, and the
resumed run presents frame N first. There is one safe point a frame from
frame 1 on (`[tick] 3899 safe points over 3900 presented frames`).

## What a snapshot holds

One file per snapshot, written beside its name and renamed into place:

| part | bytes |
|---|---|
| header | `SOASNAP\0`, format version (1), header size, payload size, the payload's SHA-1, the frame, the guest tick, the resume address: 64 |
| sections | each a 16-byte name, an 8-byte size and its bytes, in the order of snapshot.c's `SECTIONS` |

| section | what |
|---|---|
| `build` | the game library's build record (executable, baked inputs, resume points) and the layouts: checked, not loaded |
| `swaps` | the replacements set (address, and the module and offset of what answers it): checked against the resumed run's |
| `events` | native events (soa_host_schedule's): id, due tick, period, and the function, data and host function as module and offset |
| `cpu`, `mem` | every register of `CpuState`; MEM1, 24 MB |
| `threads` | each guest thread's context, whether started, and the registers of its last save |
| `clock` | the fixed clock's tick count and statistics, the quiet rule's "something is due"; its configuration (SOA_CLOCK and the SOA_CLOCK_* numbers) checked; the real clock's guest time |
| `hle` | the MMIO spin detector's run, the register and syscall counts |
| `seed` | each reseed site's count (SOA_SEED checked) |
| `irq` | VI, PI, AI registers and the AI sample counter's base; the decrementer; the retrace's phase; pending lines; the quiet rule's two tables of loop states, their sums, epochs and timebase steps; the old rule's back-edge count; every delivery count |
| `tick` | turbo's state; the report's counts |
| `swap` | the count of native answers the quiet rule compares; while counting, every function's calls, answered and translated |
| `main` | the frame (SOA_POKE, SOA_PEEK, SOA_STALL and SOA_SKIP_TO are read again; what they named for a frame before N is not done again) |
| `gx` | CP, XF and BP register shadows; CP, PE and PI FIFO registers; the bytes of a command not yet whole; whether a display list is being recorded; the frame count; the pipe's byte count (the quiet rule compares it); a capture's registers as the frame began |
| `gxr` | the EFB's colour and depth (a copy clears only its rectangle; rows 480 up and the alpha byte are read again), the last screen copied out, the frames presented, the draw count SOA_GXR_DRAWS counts, the renderer's counts |
| `tex` | TMEM; the combiner's colour and constant registers (built up a channel pair at a time, so not to be had from the BP shadow); **the texture cache whole**, every decode with its hash and epoch: an empty one would decode afresh what this one keeps by design (mip levels the hash does not cover, a texture rewritten within an epoch) |
| `dvd` | DI registers and a command in flight: its data is in memory already (a read moves it at once), its interrupt comes at the saved tick |
| `dsp`, `ax` | mailboxes, the microcode protocol's place, AI DMA and the tick its block ends; the mixer keeps nothing of the game's between lists (every voice is in its parameter block) |
| `aram` | ARAM, 16 MB; its registers and a DMA in flight |
| `si` | SI registers and buffers; a SOA_PAD_FILE replay's cursor (a line a read, not a function of the frame); the host buttons' last state |
| `exi` | the three channels, the card's command state, SRAM, the RTC latch, and the card image as the game has written it |
| `record`, `mod` | nothing: a run with SOA_RECORD or SOA_MODS is refused (a recording's counts and a mod's library are state no file holds) |
| `host` (host builds) | strict mode's latched pads (frame N's, latched at N-1's end), the ports, pads named for later frames, writes not yet made, the strict counts; strict mode checked |
| `host:<name>` | a host's own (below) |

A snapshot of the opening is 75 to 92 MB (MEM1 and ARAM are most of it, then
the EFB, the texture cache and TMEM) and takes 0.3 s to write.

**Nothing is forgotten.** `config/snapshot_state.txt` lists every variable
the runtime keeps between calls -- every data and bss symbol of every object
of a host build, each function's statics included, 902 of them -- with what a
snapshot does with it: `saved` (298), `cpu`, or why not (`config`, 188: read
again at the start of every run; `report`, 160; `host`, 56: what a host or
window reads, made again from the frames; `scratch`, 93: empty or unused at
the safe point; `refused`, 106: a mode no snapshot is taken in). `test_snapshot.py` compiles runtime/, lists the symbols with nm
and holds the file to them both ways, and every `saved` name to its file's
`*_snap` function. A variable added to the runtime without that decision
fails the suite. The list is a Linux host build's (the plugin's, and the
launcher of one): what only other builds compile -- the Vulkan backend
(refused), SDL's window and sound and Win32's window (devices, live input:
nothing that repeats), the Windows sampler (the profile) -- is not on it.

**What a resumed run must share with the run it continues**, since a resume
continues one run: the build (game library and runtime; the `build` section
and the section sizes refuse another's), the disc, the switches read at the
start (the clock's are checked; SOA_SEED is checked; SOA_THREADS, SOA_RENDER,
SOA_DEFLICKER and the rest are not: give the same), the pad input from frame
N on (SOA_PAD scripts are keyed by frame and mean the same), and a host's
replacements (checked). The card is not needed as it was: the snapshot's
image is written to the resumed run's card (SOA_CARD) as it starts, and the
game's saves go on into it.

**What does not continue**: the report's census lines (ARAM's sources, AX's
opcode and voice census) and its timers start again; a SOA_WAV file and a
SOA_PAD_RECORD recording are the resumed run's own, from its first block and
line; a host's published models and texture generations start again from the
first frame; the renderer's host-facing counters count on. The rest of the
report -- every delivery, wait and clock statistic, the GX, texture, DSP, SI
and EXI counts, the MMIO table -- is the whole run's.

**Refused**: a snapshot inside an interrupt handler or event (never the case
at the safe point), with mods loaded, with SOA_RECORD on, with a GPU backend
(SOA_GPU: its frame buffer is on the GPU), or with a guest thread on a fiber
of its own. A resume is refused for a damaged, cut or foreign file (by
name), another build's sections, a different clock or seed, another set of
replacements, or an event no snapshot can find again.

## Using it

**Headless** (the launcher, `gen/<profile>/soa`):

```
SOA_SNAPSHOT_AT=4000:/scratch/opening-4000.snap[,5000:/scratch/b.snap...]   # take, at those frames
SOA_RESUME=/scratch/opening-4000.snap                                       # start from one
```

with everything else as the run that took it (SOA_CLOCK=fixed, SOA_PAD, the
pokes, SOA_FRAMES counting from power-on as before). A resumed run may take
snapshots of its own further on; one of SOA_SNAPSHOT_AT's at or before the
frame it resumed at is taken already. Snapshots hold the game's memory: they
go to scratch, never into a repository.

A snapshot is for the game library that wrote it (its record is checked) and
outlives a rebuild of the runtime as long as every section is the size it
was; a runtime change that gives a variable a new meaning without changing
its section's size is the one thing no check sees, and wants new snapshots.

**A host** (soa_host.h, "Save states"; within ABI 24, functions only,
resolved by name):

| call | when | does |
|---|---|---|
| `soa_host_snapshot(path)` | while held (strict mode or soa_host_hold), or any time outside strict mode | a snapshot at the next frame's start: held at F, it is of frame F+1, which a resumed run presents first. 1 asked |
| `soa_host_snapshot_status(&frame, why, cap)` | any time | 1 written (its frame), -1 failed (why), 0 not yet |
| `soa_host_resume(path, why, cap)` | before soa_host_start | the snapshot the run starts from: 0, or why not (checked now: header, checksum, version) |
| `soa_host_resumed_frame()` | any time | the snapshot's frame, or -1 |
| `soa_host_resume_event(id, fn, user)` | before soa_host_start | an event of the snapshot's given its function and data again |
| `soa_host_set_snapshot_hook(fn, user)` | any time | fn is called on the guest's thread as each snapshot is taken |
| `soa_host_snapshot_put(name, data, bytes)` | from the hook | a section of the host's own in the snapshot (a name of up to ten characters) |
| `soa_host_snapshot_get(name, out, max)` | after soa_host_resume | it back: its size, or -1 |

`soa_host_start` refuses a resume whose replacements are not the
snapshot's (the host sets them first, as it does for any run), and one with
an event whose data was on the heap that the host has not given back. An
event whose function and data lie in a module (the host's program, a
library) comes back by itself: a pointer is kept as its module's file name
and offset, which a fresh process finds again with its modules elsewhere.

The host's own state -- a native replacement's, an event's -- is the host's
to keep: what the hook puts in comes back to the resumed host before it
starts. `tools/strict_host.c` keeps its event count and hash that way, so a
resumed run's last line is the whole run's (`STRICT_SNAPSHOT=F:path`,
`STRICT_RESUME=path`).

## Proof

Run A from power-on to N+M; run B1 from power-on taking a snapshot at N
(stopping just after); run B2, a fresh process, resumed from it to N+M. B2
must equal A from N on in: every frame's hash, the call counts (totals, and
less the snapshot's own counts, the calls after N), the PCM after N (A's WAV
from the snapshot's byte offset against B2's whole WAV), the guest ticks at
the end, and the card at the end. B1 must equal A before N (taking a
snapshot changes nothing). This tree, its host build's launcher (`soa`),
`SOA_CLOCK=fixed SOA_THREADS=1 SOA_RENDER=1 SOA_HASH=1`, muted, card copies,
`SOA_SETTINGS=0`, SOA_COUNT_CALLS and SOA_WAV:

| scene (card; pad) | N | to | frames from N | calls after N | PCM after N | snapshot | wall: A / B2 |
|---|---:|---:|---|---:|---|---|---|
| opening (blank; the bench script) | 4,000 | 5,000 | 1,000 identical (991 distinct) | 131,049,570 in 920 functions, identical | 4,262,400 bytes, identical | 92.5 MB | 263.8 / 69.0 s |
| opening, a second snapshot | 4,500 | 5,000 | 500 identical | 66,637,798, identical | identical | 92.5 MB | 263.8 / 37.2 s |
| town (final-boss; Continue, the warp to 013e at 3,400) | 4,000 | 5,000 | 1,000 identical (1,000 distinct) | 119,117,225 in 791, identical | identical | 106.6 MB | 290.0 / 78.6 s |
| the same load, a disc read in flight (done 1,337,472 ticks later) | 3,408 | 5,000 | 1,592 identical | 183,371,872, identical | identical | 89.0 MB | 290.0 / 121.9 s |
| the same load, between reads | 3,404 | 5,000 | 1,596 identical | 183,400,661, identical | identical | 89.0 MB | 290.0 / 122.4 s |
| battle (secrets; Continue, stick up: a random battle, scene 7 from frame 4,990 to 5,990) | 5,000 | 6,000 | 1,000 identical (1,000 distinct) | 106,979,856 in 678, identical | identical | 93.2 MB | 337.9 / 74.9 s |
| world map flight (secrets; Continue, stick up; sky mode 1) | 3,800 | 4,800 | 1,000 identical (950 distinct) | 95,547,873 in 1,534, identical | 4,316,160 bytes, identical | 82.7 MB | 246.5 / 66.8 s |
| card format (an unformatted card; New Game, Yes to formatting: 40,960 bytes written) | 1,740 | 2,300 | 560 identical | identical | identical | 75.4 MB | 97.2 / 18.7 s |
| the same, the card open | 1,770 and 1,800 | 2,300 | 530 and 500 identical | identical | identical | 80.6 MB | 97.2 / 17.5, 17.0 s |

In every row the guest ticks at the end are A's, the card at the end is A's
(the card-format rows: written during the run, after the snapshot), and B1 is
A frame for frame before N. More:

- **Taking a snapshot changes nothing**: the opening's B1, taking both of its
  snapshots and running on to 5,000, is A in every one of its 5,000 frames,
  its counts and its PCM.
- **The state itself repeats**: B2, resumed at 4,000, took a snapshot of its
  own at 4,500; against B1's at 4,500 all 25 sections are byte for byte the
  same (MEM1, ARAM, every device, the clock and the quiet rule's tables, the
  texture cache), and B3, resumed from B2's, is A again to 5,000.
- A snapshot is written in 0.26 to 0.39 s and read in 13 to 22 ms.

**Time saved**: a check that looks at frames N to N+M runs only those, about
0.065 s a frame here (one rasterizer thread, hashing every frame, a load
average of 10 to 20), where from power-on it ran all N+M: 3.5 to 5.7 times
faster for the 1,000-frame checks above, 195 to 263 s a run saved; for a
shot at a single frame (M of 50), minutes become seconds.

Strict host mode (`tools/strict_host.c`, the opening's 4,000 frames): S1
(seed 1, delays up to 0.3 ms) took a snapshot while frame 2,999 was held --
the snapshot of frame 3,000 -- and ran on to 4,000; S2, a fresh process with
seed 2 and delays up to 3 ms, resumed from it to 4,000. Frames 3,000 to 3,999
have the same hashes; every held frame's globals and MEM1 hashes, and every
run of the host's 5 ms event after the snapshot's tick (event 6,738 on, due
and run ticks), are the same lines; the last line is the same --
`end: 1823 functions called, counts a64910c2ef97de21; event ran 13397 times,
ticks 86735e55fb77939b; strict: 4000 held, 3999 pads, 0 late, 0 refused` --
as are the PCM after the snapshot (4,262,400 bytes), the guest ticks
(5,527,575,000) and the card. The events came back by module and offset
(tick_event in the program, user NULL); the host's own count and hash came
back through its section. S1 took 188 s, S2 58 s.

## What soa-ue5's plugin needs to use it

All in `Plugins/SoAHost`; nothing there has changed yet.

1. **A game library and runtime built from this tree** (the resume point is
   baked in: a retranslation, and `resume=801DCB84` in the library's record).
2. **Resolve the eight functions by name**, as optional (`Export(...)`), as
   strict mode's are; ABI 24 unchanged.
3. **Taking one**: a console command (`soa.Snapshot <path>`) calls
   `soa_host_snapshot` from the held tick, before `soa_host_go`; the
   snapshot is of the next frame. Poll `soa_host_snapshot_status` on later
   ticks. The path is under the scratch folder, never the repo.
4. **Resuming**: `-SoAResume=<path>` (or `soa.Resume`, read before the
   start): `soa_host_resume` before `StartFn`, after the replacements are set
   and with the same strict mode and clock as the run that took it.
   `soa_host_resumed_frame` tells the mirror and the shot scheduler which
   frame comes first (`soa.ShotAtFrame` past it, no SOA_SKIP_TO).
5. **Native state**: every native replacement or event that keeps state of
   its own across frames (the sound driver's native mix and its AI tick, the
   native pad's latch, the card's, the disc's caches) puts it in from
   `soa_host_set_snapshot_hook` and reads it back with
   `soa_host_snapshot_get` before the start; one that keeps none needs
   nothing. An event scheduled with a heap `user` (an object) is given back
   with `soa_host_resume_event(id, fn, user)`: keep the ids soa_host_schedule
   returned in the host's own section.
6. **The check** is the one above: a scene's shot from power-on against the
   same shot resumed, the frame hashes and soa_host_call_counts equal from N.
