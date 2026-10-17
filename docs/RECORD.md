# Record and compare

A native replacement for one of the game's functions (docs/SWAP.md) is
checked here against thousands of the game's own calls of that function, in
seconds and outside Unreal. A normal headless run records real calls of
chosen functions (`SOA_RECORD`). Then `soa-compare` replays each call on its
own: it restores the memory and registers the call began with, runs the
replacement (a *candidate*), and holds what the candidate left to what the
game's call left. The same replay of the game's own translation is a self
check, and it must agree on every call.

A recording is the game's memory, and so it is game data. It stays on the
machine that made it, like a memory card, and never goes into this
repository. The default path, `build/record.soarec`, is ignored.

## Recording

```
SOA_RECORD=0x80096DA4,0x8011660C;3000;2   functions; at most 3000 calls of each; one call in 2
SOA_RECORD_FILE=path                      where (default build/record.soarec)
SOA_RECORD_WITH=all | lo-hi,lo+bytes,...  more memory kept at every recorded call (below)
SOA_RECORD_PAGES=4096                     the most pages one call may touch (default 4096)
```

It works in any build with the swap table (game table ABI 2) on a POSIX
system: the single-file build or the split build. On Windows the variable is
refused with a line in the log.

**How a call is caught.** `runtime/record.c` sets a `SWAP_BOTH` replacement
(docs/SWAP.md) on each function. The guard calls it at the function's entry,
the translation runs, and then the guard calls it again. Nothing costs
anything when `SOA_RECORD` is unset, because the guards are the swap's own.
The only other cost is one test of `g_record_armed` in `hle.c`'s MMIO and
timebase entry points and in `irq.c`'s delivery. When the limit is reached
for every function, the replacements are cleared, so the rest of the run
costs nothing either.

**What the call touched.** While a recorded call runs, the 24 MB of RAM
are made inaccessible (`mprotect`). The call's first touch of a page faults.
The fault handler copies the page as it is (its *pre-image*) and makes it
readable. The first store to that page faults again, and the handler makes
it writable. So the pages the call touched are known exactly, including
those of every function it calls and anything the runtime's native code
does to memory for it (a device model's copy, an HLE `memcpy`). Each page's
bytes from before the call are kept, and so is which pages the call may
have written. When the call returns, every page is opened again, and the
bytes of each written page that differ from its pre-image are the call's
output. The handler sits in front of `plat.c`'s guard on the memory past
the RAM and hands that guard every other fault.

**The rest of the call.** These are also kept:

- the registers before and after: r0–r31, f0–f31 (both halves), CR, XER,
  LR, CTR, FPSCR, MSR, the GQRs;
- every timebase read (`mftb`), every hardware read (MMIO) with the value
  it read, and every store to the hardware, in order;
- every function the call entered, in order, with r1. The swap's new
  `SWAP_TRACE` bit (`swap_trace`) sends every guard to the recorder while
  the call runs. This list is for a person reading the call and for the
  self check's strictest test;
- every byte the call wrote to GX's write-gather pipe, in order, and GX's
  state as `gx.c` keeps it, as the call found it and as it left it (below,
  "The GX pipe"). Only for a call that wrote the pipe or changed that state.

**What a recording run changes.** Interrupts are held until the recorded
call returns, unless it polls 4096 times, which means it is waiting for one.
The reason: recording makes a call take 0.2 to 10 ms, and the guest's clock
is the host's, so an interrupt would otherwise land inside nearly every
recorded call. With them held, none of the 31,760 calls recorded for the
measurements below had one. A call that still had an interrupt inside, or
let another guest thread run, or had a page touched by another host thread,
is kept with a flag, and the replay skips it unless told otherwise. A call that stored to the disc's
interface is dropped. A disc read goes into guest memory through the
kernel's `read`, and a protected page would make that read fail. So from
that store on, the recorder opens every page and does not keep the call.

**The file** (`runtime/record.h` has the layout; version 2 since the GX
part, and the replay still reads version 1). Each page's pre-image is
encoded against that page's last image in the file, and its post image
against its pre-image. The encoding is runs of bytes the same as the base
and runs of XOR'd literals. A page that did not change since the file last
had it costs a 16-byte record and no bytes.

**`SOA_RECORD_WITH`.** A candidate may read memory the game's function did
not read. For example, the plugin's camera task reads the whole list of
bodies and the place's number every call, and the game's task does not. The
replay cannot give back a page the recording does not have; it reports any
touch of such a page. `SOA_RECORD_WITH` keeps more memory at every recorded
call, touched or not. It takes ranges, or `all` for every page of the RAM.
With `all`, the whole RAM is copied at the call's entry and made read-only,
and only stores fault.

## Comparing

```
python tools/replay.py [--out DIR] [-I DIR] [CANDIDATE.c|.cpp ...]   build DIR/soa-compare (default build/replay)
DIR/soa-compare REC --game gen/linux-split/libsoa_game.so            the self check, then every candidate
DIR/soa-compare REC --original                                       the self check alone
DIR/soa-compare REC --candidate NAME [--candidate NAME ...]
DIR/soa-compare REC --info                                           what the recording holds
DIR/soa-compare REC --calls N --only 0xADDR                          the calls the game's call N made
DIR/soa-compare --list                                               the candidates built in
    --show N         mismatching calls told in full (default 5)
    --stack BYTES    stack under r1 a candidate may use as scratch (default 65536)
    --with-flagged   replay the calls the recording flagged too
    --partial        a byte the game's call wrote and the candidate left as it was is
                     "left undone": counted and listed by stretch, not held
    --no-hardware    the candidate is a native replacement of the platform: it must
                     touch no hardware, even where the game's call did (below)
    --no-pipe        the candidate is a native drawer: it must write nothing to GX's pipe,
                     and what the game's calls drew is listed as retired (below)
```

The exit code is 0 when every replayed call agreed, 1 when one did not, and
2 for a usage or read error.

**The replay is the game library and a stand-in runtime.**
`tools/replay/replay.c` is built as a `libsoa_runtime.so` of its own.
`soa-compare` links against it, and then `dlopen`s the game library of the
build that recorded (`libsoa_game.so` from a split build). That library
needs a library of that name, and this one has it. It answers the runtime's
side of the seam (`config/seam.txt`) as a replay needs:

| the call meets | the replay gives |
|---|---|
| a swap guard | the candidate at its function's entry; every entry is traced |
| a timebase read, a hardware read | the recording's value, in order; anything else ends the call (*not replayable*) |
| a store to the hardware | held to the recording's stores, in order |
| `VIGetRetraceCount` | the word in memory the runtime's own reads (the frame's start ends the call) |
| the GX pipe | every byte kept, and walked as `gx.c` walks it from the GX state the call began with; the bytes and the state they leave are held (below) |
| the cache calls, `OSReport`, the game's debug print, trace and watch points, the Ninja model notes | nothing, since none of them changes memory |
| the decrementer, a trap, a thread's context saved or switched, the scheduler's idle, the audio system's start | the call ends: not replayable |

**Memory** has the runtime's shape: the RAM, then the mask's slack. Every
page is inaccessible except those of the call being replayed. A touch of any
other page faults. The page is handed over zeroed and noted, unless it is in
the stack under the call (scratch). A call's pages are made read-only, so
the replay knows which of them were written. Pages stay mapped from one call
to the next. A page that neither the last replay nor the game changed is not
written again and not compared again, which is what makes `SOA_RECORD_WITH=all`
recordings fast to replay.

**A call is held** to its recording on:

- the registers the calling convention keeps: r1, r2, r13–r31, f14–f31
  (ps0 only, because the game saves those with `stfd` and its paired-single
  code changes ps1 as it pleases), and CR fields 2–4;
- the results the candidate declares (`SOA_REPLAY_R3`, `_R4`, `_F1`). A
  function that returns nothing leaves r3 and f1 as garbage, and the
  candidate is not held to them;
- every byte of every page the game's call touched, except the stack under
  r1 (`--stack`, the frames of the call and its callees) and the word at
  r1+4 (where a callee keeps its return address, in the caller's frame);
- nothing written to a page the game's call never touched, and no load from
  one;
- the stores to the hardware the game's call made, in order;
- the bytes the game's call wrote to the GX pipe, byte for byte, and GX's
  state after them; no pipe byte at all where the game's call wrote none.

The self check holds every register, both halves of every FPR, every byte
including the stack, and the list of functions called with their r1. Any
difference is one line: the address, what the replay left, and the game's
value.

**`--partial`** is for a rule that answers part of a function. The plugin's
rules write the record they model and leave alone what nothing reads
afterwards, such as a body test's own record. Under `--partial`, a byte
the game's call wrote and the candidate left as it found it counts as *left
undone*. It is not held, and the undone bytes are listed by stretch with how
many calls left them. Everything else is still held: every byte the
candidate wrote, every result and kept register, nothing written or read
outside the recording. A wrong value is still a difference. A call that
agrees except for undone bytes is counted apart from one that agrees in
full.

**`--no-hardware`** is for a native replacement of an SDK function (soa-ue5's
Layer 1: disc, pad, card, sound, timing). The game's recorded call read and
stored the console's registers; the native one talks to the native device
instead, and must not touch MMIO at all. Under it, for a candidate (the self
check is held as ever):

- a hardware read by the candidate, or by any function of the game's it
  calls, ends the call, which counts as not agreeing ("a read of AI
  CC006C00 (the call ends there)"): there is no value to give it;
- a store to the hardware is counted, dropped, and is a difference ("1
  store(s); the first AI CC006C08 = 5");
- the recording's own hardware accesses are not held: the game's stores need
  not be made, and its timebase reads are given in their own order, the
  hardware's between them skipped;
- everything else is held as before: memory, results, the kept registers.
  What the hardware did to memory -- a DMA's bytes, a device model's copy --
  is in the recorded pages, so the candidate must still have put it there.

After the run it lists the hardware the game's replayed calls touched,
register by register, as *retired*: what the candidate is expected to
reproduce some other way, a read's value known natively, a store's effect
made natively. For example:

```
  --no-hardware: the hardware the game's calls touched, retired; the candidate is expected to reproduce
  each effect some other way (a read's value known natively, a store's effect made natively):
    AI      CC006C00 read32: 6 times in 6 of 6 calls; values 40, 41, 42, 43, ...
    AI      CC006C04 store32: 6 times in 6 of 6 calls; values 41, 42, 43, 44, ...
```

(`test_replay_nohw.py`: a native candidate fails the ordinary compare on
the missing store and passes `--no-hardware`; one that leaves the call to
the translation fails on the read, one that stores through a game function
fails on the store.)

**Recording under the fixed-step clock** (`SOA_CLOCK=fixed`,
docs/CLOCK.md) gives a recorded run that repeats, recording and all.
Interrupts are still held while a recorded call runs, so it is not the
unrecorded run frame for frame: letting them land inside recorded calls
(where a fixed clock would put them) was tried, with `SOA_RENDER=1`: 1,521
frames identical to the unrecorded run, and then a rasterizer worker's copy
into guest memory (`run_copy_pixels`) met a page the recorder had protected
and the run died (SIGSEGV). Holding them avoids that; letting them in needs
the recorder and the renderer's copies to agree first.

**What a candidate's calls into the game do.** They run the game's own
translation against the restored memory. A candidate that calls a maker of
the game's (the camera's bodies are blocks of the game's heap) gets the
game's own maker and the game's own heap. As long as it calls what the game's
call called, everything those calls read is in the recording. If it calls
something else, a touch outside the recording says so. Replaying the
callees' recorded effects instead was rejected. Their writes are mixed into
the caller's in the page diff and cannot be separated from them. Replaying
them would also hold a candidate to calling exactly the same callees with
exactly the same arguments, which is the thing a rewrite is free to change.

## The GX pipe

A call that draws writes GX's commands to the write-gather pipe at
`0xCC008000`. Before version 2 of the recording such a call was not
replayable; now the pipe is part of what a call does, as memory is.

**What is kept.** `gx.c` hands the recorder every store to the pipe while a
recorded call runs (`g_record_pipe`, one more pointer test on the pipe's
path, and only while a call is being recorded). The call's record keeps the
bytes in order, and two images of GX's state (`runtime/gx_state.h`'s
`GxImage`): the CP registers, XF's registers and matrix memory, the BP
registers, the PI FIFO's three registers, whether the CPU FIFO is the
display-list one, and how many bytes of a command not yet whole the pipe was
holding -- as the call found them and as it left them. The waiting bytes
themselves are kept too, since the call's first bytes may finish a command
its caller began. The images are encoded as the pages are, against the last
image in the file, so a call that changed one matrix costs a few dozen
bytes.

**How the replay holds it.** `replay.c`'s `gx_pipe_write` keeps every byte
the replayed call writes, and walks them as `gx.c`'s parse does, from the
image the call began with: CP, XF (direct and indexed, reading memory as
the game's would) and BP loads, display-list calls followed into memory,
draws stepped over by the vertex size the CP registers give
(`gx_vertex_size`, shared with `gx.c`). While the CPU FIFO is the
display-list one, the bytes go into memory at the PI write pointer instead,
as `gx.c` puts them, and the PI FIFO's stores move it. So a call that records
a display list leaves the list in memory and is held there like any other
store. After the call:

- the bytes are held to the game's, byte for byte;
- the state the walk left is held to the image `gx.c` left, register by
  register.

The self check holds the replay's walk to `gx.c` on every recorded call that
drew, which is what keeps the two the same. A difference names the first
byte, the command it falls in, and the registers left wrong:

```
    GX pipe: 11 bytes, the game's 11; the first difference at byte 4, in BP 41 = 000222, the command at byte 0: 23085000001002, the game's 22085000001002
    GX BP 41: 00000223, the game's 00000222
```

What the walk does not do is draw: there is no renderer in the replay, so
an EFB copy into memory (a copy to a texture) is not made again. Record
drawing functions with `SOA_RENDER=0`, as measured below: no renderer, no
copies. With the renderer on, its threads' touches of memory while a call is
recorded are flagged (another thread's touch), and such a call is skipped
unless `--with-flagged`. The PE's interrupts a
draw-done or token raises are not modelled: they are held until the call
returns, as every interrupt is.

**`--no-pipe`** is for a native drawer (soa-ue5's Layer 2): a replacement
that hands its draws on some other way and writes nothing to the pipe. For a
candidate under it, a byte to the pipe is a difference ("wrote 11 bytes to
GX's pipe, where a native drawer must write none"), the game's bytes and GX
state are not held, and what the game's calls drew is listed as retired:

```
  --no-pipe: what the game's 12 drawing calls sent down the pipe, retired; the candidate is expected to hand it
  on some other way: 132 bytes, 0 draws (0 vertices), 12 BP, 12 CP, 0 XF and 0 indexed XF loads,
  0 display-list calls, 0 other commands
```

Memory, results and kept registers are held as ever.

**Measured** (2026-10-17, `gen/linux-split`'s game library, a runtime from
this tree, the opening's pad script, `SOA_CLOCK=fixed SOA_RENDER=0`):

| functions | calls | self check | pipe bytes held |
|---|---:|---|---:|
| the strip emitters `802A49F4`, `802A2F84`; GX's `8024E568`, `8024F838`, `8025030C`, `8024DA00` (one call in 37 over 4,000 frames) | 1,800 | 1,800 agree, GX state included | 1,348,713 |
| GXBeginDisplayList `80251D80`, GXEndDisplayList `80251E48`, the text drawer `801E203C`, the strip emitter `802A52C4` (one in 3) | 800 | 800 agree | 434,509 |

A recorded drawing call took 0.25 to 0.28 ms; the self check ran 60,000 to
76,000 calls a second. `test_record_pipe.py` holds a synthetic drawer and
display-list recorder to their recordings, catches a candidate one BP value
off on one call, and fails the self check of a recording with one pipe byte
changed.

## Who writes the pipe: `SOA_PIPE_CENSUS`

```
SOA_PIPE_CENSUS=census.tsv   every translated function's own stores to the pipe, and its calls
```

Each store to the pipe is told to `swap.c` with the block that made it
(`s->pc`), which names the function whose entry is the nearest at or below
it: the bytes are the function's own, not its callees'. Calls are counted
from the start, as `SOA_COUNT_CALLS` counts them. At the end, the file has a
line a function that wrote anything, busiest first:

```
address	pipe_bytes	pipe_stores	calls	translated	bytes_per_call
```

and stderr the twelve busiest (`[census]`). The total is the `[gx]` line's
pipe bytes to the byte (821,038,156 of 821,038,156 over the first 4,000
frames of the opening): no store is missed or counted twice.

**Measured** (2026-10-17; fixed clock, `SOA_RENDER=0`; three runs on card
copies: the opening to the first battle, blank card, 14,000 frames; walking,
the `final-boss` save, 12,000 frames; flying, the `secrets` save, 12,000
frames): 86 functions write the pipe themselves, 84, 81 and 83 in the three
runs, 16.2 GB between them.

| where | functions | share of the bytes |
|---|---:|---:|
| the GX library (`8024B000`-`80256000`) | 70 | 40.7% |
| Ninja (`80290000`-`802B8000`): the strip emitters `802A49F4` (37.0%), `802A52C4` (19.2%), `802A2F84` (1.0%), the shadow `802AC5E8`, and `80290ED4`, `80291064`, `80299758`, `802A4FC8`, `802AA2F0` | 9 | 58.4% |
| elsewhere: the text drawer `801E203C`, `80231360`, `80235574`, `8025A1A4`, `8028A0F4`, `8028A418`, `8028A8C0` | 7 | 0.9% |

The busiest of GX's are `8024E568` (6.8%, 218 bytes a call), `8024DA00`
(4.0%, 12 a call), `80252100`, `8025030C`, `802521D8` and `80252150` (2.3 to
3.4% each). Three writers ran only in the opening (`8028A0F4`, `8028A418`,
`802AA2F0`) and two only in flight (`80231360`, `802A4FC8`). The ranges are
by address, not by library: which of the seven "elsewhere" are SDK code is
for the reader of each.

## The candidate interface

A candidate is the host's own answer type, `SoaHostAnswer` from
`runtime/soa_host.h` (ABI 23). `replay.c` calls it the way `runtime/host.c`
calls a host's replacement. So the code tested here is the code a host
passes to `soa_host_replace`, and `SOA_HOST_GAMES`, `_ANSWERED`, `_BOTH`
and `_ANSWERED_LIVE` mean what they mean there. `game`, `game_original`
and `original` work as there. The candidate reads and writes `mem` and
`r`/`f`. `tools/replay/replay.h`:

```c
static int answer(void* user, SoaHostCall* call) { ... return SOA_HOST_ANSWERED; }
SOA_REPLAY_CANDIDATE(my_rule, "my-rule", 0x80096DA4u, answer, NULL, SOA_REPLAY_NONE)
```

A file of candidates can be C or C++ and is linked into `soa-compare` by
`tools/replay.py`. `soa_replay_seen()` and `soa_replay_frame()` say which
of the game's calls is being replayed. The report counts how many calls the
candidate answered and how many it left to the game. A candidate that
returns `SOA_HOST_GAMES` agrees by construction, so the coverage line is
part of the result.

## Measured (2026-10-17, this machine, gcc 16)

The proof is the 10 functions soa-ue5's plugin answers natively. Their rules
(`SoACameraRule.cpp`, `SoAPartyRule.cpp`, `SoAPartyRestRule.cpp`,
`SoAWalkRule.cpp`, `SoAShipRule.cpp`) compile without Unreal headers and
were built straight into `soa-compare`. The adapters are the plugin's
"instead" paths (`SoACameraTask.cpp`, `SoAPartyTask.cpp`,
`SoAShipTask.cpp`, mode 2) rewritten in plain C++. They are a scratch file
and not in this repository. Runs were muted, headless, `SOA_SPEED=30`, on
the split build (`gen/linux-split`'s game library and a runtime built with
the recorder), with `SOA_RECORD_WITH=all`.

### The ten functions

Four recordings, all with `SOA_RECORD_WITH=all`:

- **opening**: a blank card, the opening's pad script (`speed.scn`'s),
  12,000 frames, every second call.
- **walk**: soa-ue5's `final-boss` save loaded, then the stick walking
  about the rooms below the save room with A pressed now and then; 9,600
  frames, every second call.
- **ship** and **ship2**: the `secrets` save on the world map, flying. The
  first is a pad file with turns, both triggers and the C stick, until a
  random battle at frame 4,290 ends the flight. The second flies ahead with
  A pressed every 60 frames, so the battles are fought and the flight goes
  on. Every call is recorded.

| function | calls | self check | candidate, held in full | `--partial`: agree, all else left undone | wrong |
|---|---|---|---|---|---|
| camera task `80096DA4` (opening) | 3,000 | 3,000 | 2,999 | — | 1 |
| camera task (walk) | 3,000 | 3,000 | 1,869 | 1,130 | 1 |
| leader's frame start `801165C4` | 2,348 | 2,348 | 2,348 | — | 0 |
| leader's pad step `8011660C` (f1 held) | 2,049 | 2,049 | 2,049 | — | 0 |
| leader's floor and walls `8011753C` | 2,348 | 2,348 | 1 | 2,347 | 0 |
| scan `80117BE4` (r3 held) | 2,230 | 2,230 | 2,228 | 2 | 0 |
| ship's pad step `800CFDF0` | 2,757 | 2,757 | 1,815 | 942 | 0 |
| ship, land and bodies `80115EAC` (r3) | 2,757 | 2,757 | 0 | 2,757 | 0 |
| ship, out of the bodies `80117290` | 2,757 | 2,757 | 2,653 | 104 | 0 |
| ship, may it be there `80114950` (r3) | 2,757 | 2,757 | 2,653 | 104 | 0 |
| ship, floor under it `800D07F8` | 2,757 | 2,757 | 0 | 2,757 | 0 |

- **The self check agreed on all 28,760 calls** of the four recordings,
  with every register, both halves of every FPR, every byte including the
  stack, and every call made. Every recorded call was replayable, and none
  had a flag. The candidates answered every call themselves; none was left
  to the game.
- **The one wrong byte is real.** In the camera task's call 1,228 (frame
  1,615, the title's intro, in both runs that pass it), the task ends
  itself, and the rule writes 0 to the task's stage byte, 25 bytes into
  the task's record. The game leaves `FF` there. Found with
  `soa-compare --calls 1228 --only 0x80096DA4` (it frees the capsule with
  `80222930`, then the ball with `801E17E0`, as the rule does) and the
  record's address.
- **What the rules leave undone**, every byte of it the plugin's own choice
  or something its beside mode never looked at:
  - the body test's own record, `8030E7E0`–`8030E81F`, which `SoAPartyTask`
    already excludes;
  - the 352 bytes after the camera's state, `803030E0`–`8030323F`, which
    `SoACameraTask` excludes ("with the floor records after it");
  - for floor and walls, the camera, and the ship's pad step and land:
    short stretches in `80344DE0`–`80344E7F` and the same ones 0x5A0
    further on (`80345380`–`803453FF`), and the heap at
    `80D3B560`–`80D3B65F` (the camera also at `80ED25C0`–`80ED25FF`);
  - for the ship's land and floor under it, the heap at
    `80EB6080`–`80EB609F` and `80F1E6A0`–`80F1E6BF`, on every call of
    the floor; and for the land, a word or so in `8030EA00`–`8030EA1F`.

  The game's functions write all of these in most calls, and the rules
  never do. Whether anything reads them afterwards is the next question for
  those rules.
- **The broken candidates are caught.** The camera task with its state's
  first byte flipped one call in 7 fails on 429 of 3,000 calls in the
  opening (428 planted, plus the real one). The ship's answer turned one
  call in 5 fails on 216 of 1,082 and 335 of 1,675 calls: r3 against the
  game's, at the call. A test of the tool itself (`test_record.py`)
  changes one register of a recording, and the self check fails on that
  call alone.

### Speeds

| | |
|---|---|
| recording, the pages a call touches | 0.20–0.24 ms a call. 3,000 camera calls over 6,000 frames of the opening: 39.8 and 40.1 s wall against 39.0 and 39.2 s without (+2–3%), 1.8 KB a call |
| recording, `SOA_RECORD_WITH=all` | 8.7–9.9 ms a call; 242 KB a call (opening, a call every second frame), 199 KB (walk, five functions), 131–133 KB (ship, five functions every frame) |
| `SOA_RECORD` unset | the swap's guards and nothing else: one more load in interrupt delivery and the MMIO and timebase entry points |
| self check, touched pages | 207,000 calls a second (3,000 calls in 0.014 s) |
| self check, `all` | 6,100–13,100 calls a second; the 2.4 GB walk recording (11,975 calls) in 2.6 s wall |
| candidates, `all` | 2,900–10,200 calls a second, the rule's own time included |

The candidates' adapters were built with the plugin's rule files and the
replay into one `soa-compare` (g++ -O2) without one Unreal header or shim.

## Limits

- **POSIX only** (`mprotect` and `SIGSEGV`). Windows refuses `SOA_RECORD`.
  The page size is the host's. A recording replays on a machine with the
  same page size and the same game library.
- **Interrupts are held** while a recorded call runs, under the real clock.
  The recorded run's timing is therefore not the unrecorded run's, so record
  a run for its calls, not for its frames.
- **A store to the disc's interface drops the call.** Calls that start disc
  reads are not recorded.
- **The decrementer, thread switches** end a replayed call as not
  replayable. A function that sleeps cannot be replayed yet. A function that
  draws can (above), but an EFB copy it makes is not made again, and the
  PE's interrupts are not modelled.
- **The heap in the replay is the recorded call's.** A candidate that
  allocates through the game's own allocator gets the same blocks only if
  it allocates what the game's call allocated.
- **A candidate's reads beyond the game's** need `SOA_RECORD_WITH`. With
  `all`, a call costs about 10 ms to record and its record about 250 KB,
  most of it the pages that changed since the last recorded call.
- **Stores with the same value as before** cannot be seen in a page diff.
  A candidate that skips such a store agrees, which is what the game's
  memory would show too.
