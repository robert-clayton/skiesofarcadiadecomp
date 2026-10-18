# Swap any function

The translated game is a scaffold that native code replaces one system at a
time, so a host (soa-ue5's Unreal plugin) or the runtime must be able to
answer **any** of the game's functions at run time, not just a list wired
in by hand. Native code must also be able to call back into any translated
function, including the original of the one it replaced. When nothing is
replaced, the cost has to be negligible. This is how it works, what it
costs, and what changed with it (host ABI 23, game table ABI 2).

## The mechanism

**A guard at every translated function's entry.** With `swap=True` (what
`tools/recompile.py` passes), `soa.recomp.Emitter` starts every translated
function with

```c
void fn_8025CB24(CpuState* s)
{
    SOA_SWAP(6209);          /* its index: its place in address order */
L_8025CB24:;
    ...
```

`SOA_SWAP(i)` (in `<out>/functions.h`) reads one byte, `soa_swap_flags[i]`,
as a volatile load, and branches to `soa_swap_enter(s, i)` only when that
byte is not 0. gcc emits `cmpb $0, soa_swap_flags+i(%rip); jne cold`, a
compare against memory and a branch that is never taken. The guard comes
before the entry's own label, so a loop back to the entry is not counted as
a second call. Call sites do not change. Direct calls stay direct, gcc can
still inline across them, and every way into a function reaches the guard:
direct calls, tail calls, `dispatch` (indirect calls, mods, the host's
`game`), and the runtime calling `fn_X` itself.

**The table** (`seam.swap_table_c`, written into `<out>/game_table.c`) holds,
for each of the 7,144 functions, the guard's byte (hidden, so a split game
library still exports only `soa_game`), its address, and its kind. Kind 1
means a function the runtime answers natively (`config/hle.txt`: 25 in a
decomp build, 13 in a split or host build). Those have no guard and cannot
be swapped. `SoaGame` (ABI 2) carries the four of them to the runtime.

**`runtime/swap.c`** is the only code that writes the bytes. Bit 0 means a
replacement is set; bit 1 means the call is counted. Its slow path,
`soa_swap_enter`, does the following:

- **skip**: if `g_skip` is this function's index plus one, it clears the
  skip and lets the translation run. This is how anything runs "the
  original": `swap_original` (in place, on the live registers) and
  `swap_call_original` (isolated, registers put back, as `mod_call_guest`
  does) set the skip and dispatch. The skip is used up at the next entry, so
  the original's own calls, recursion included, meet the replacements the
  way the game's calls would.
- **count** the call if counting is on.
- **hook**: it copies `{handler, fn, user}` out under a lock, sets `s->pc` to
  the function's address (so a profiler sample is attributed to the
  function), and calls the handler. The handler returns one of three
  answers:
  - `SWAP_GAMES`: the translation runs.
  - `SWAP_ANSWERED`: the translation is skipped.
  - `SWAP_BOTH`: the translation runs through the skip, then the handler is
    called again with `after` set to 1.

**The 10 hooks from `replace.c` now use this mechanism.** The field camera,
the leader's walk and scan, and the ship's flight were the only functions
`soa_host_replace` could answer. Each was bound in `hle.txt` to a hand-written
`fn_X` in `runtime/replace.c` that called `host_answer`. Their `hle.txt`
lines, their 10 `seam.txt` twins and `replace.c` are gone. They are now
ordinary translated functions with guards, and `soa_host_replace` sets them
like any other function. The host's semantics are unchanged:

| host returns | swap.c does |
|---|---|
| `SOA_HOST_GAMES` (0) | the translation runs |
| `SOA_HOST_ANSWERED` (1) | r3 is set from `gpr[0]` and f1 from `fpr[0]`; the translation is skipped |
| `SOA_HOST_BOTH` (2) | the translation runs, then the host is called with `after` = 1 |
| `SOA_HOST_ANSWERED_LIVE` (3, new) | the translation is skipped; the host has left the results in the live registers itself |

**A delivery point** (bit 3, `SWAP_DUE`): under the fixed clock's quiet
rule (docs/CLOCK.md) every byte gets this bit whenever something has come
due -- the clock moved, the guest wrote the hardware -- and the next guard to
run delivers it before anything else, then the bits are cleared. A
replaced function's entry is its translation's entry, so an interrupt that
falls due just before a call runs at the same place whether the replacement
or the translation answers. It costs one pass over the bytes each way, a
few hundred times a frame, and only under the fixed clock.

**A trace** (`swap_trace`, bit 2, `SWAP_TRACE`): every guard calls a
function with the entry's address, until it is cleared. The recorder
(docs/RECORD.md) uses it to list the calls a recorded call makes. Like
counting, it sets every byte, so every call pays the slow path while it is
on.

**Thread safety.** The host can set or clear a replacement from any thread
at any time. Writes to the bytes and slots are serialised by a lock. A
slot's handler and user word are written together, before the byte is set,
and are cleared after the byte is cleared. The guest copies them under the
same lock, so a call sees either the old pair or the new one, never half of
each. A call already inside a replacement when it is cleared finishes in
that replacement. A host that needs a frame boundary changes replacements
while the guest is held (`soa_host_hold`). A request made before
`soa_host_start` is kept (as many as are made: the list grows; it was 64 at
most before 2026-10-17) and applied once the game library is loaded. An address that starts no function is logged and refused at that
point. `g_skip` is a single global, which is safe because only one guest
context runs at a time (guest threads are fibers that switch
cooperatively).

**Limits.**
- The runtime's own native functions (`hle.txt`) are refused. They are
  already native.
- A function reached by a `goto` inside a different function whose body
  overlaps its entry skips the guard. The recompiler only emits such a
  `goto` when another function's blocks include that address, which is rare.
- A handler must keep the caller's non-volatile registers (r1, r2, r13–r31,
  f14–f31) as it found them.

## What the host gets (`soa_host.h`, ABI 23; 24 adds the clock and events)

`SoaHostCall` keeps its ABI 22 layout. These fields are appended at the end:

- `uint32_t* r` and `SoaHostPair* f`: the game's live r0–r31 and f0–f31 (a
  pair of doubles each). Writes go straight to the guest.
- `void (*original)(SoaHostCall*)`: runs this function's own translation now,
  in place on the live registers, bypassing the host for this one call. It
  then copies r3 and f1 into `gpr[0]` and `fpr[0]`.
- `int (*game_original)(call, address, args, n, r3)`: like `game`, but calls
  the translation of `address` even when a replacement is set for it.

The rest is unchanged: `mem`, `sp`, copies of r3–r10 and f1–f8, `frame`,
`game`. `soa_host_replace` returns 1 for any translated function's entry and
0 for anything else.

**The progress counter.** `soa_host_functions(&native)`,
`soa_host_count_calls(on)` (on also zeroes the counts) and
`soa_host_call_counts(out, max)` fill one `SoaHostCallCount` per function
that has any count:

- `calls`: how many times the game called the function.
- `answered`: how many of those calls a replacement answered.
- `translated`: how many times the function's translation ran, whether for
  the game or for a replacement through `original`.

A function whose `translated` count stays 0 over a recorded play is no
longer needed in its translated form. Counting works by setting every byte,
so each call pays one extra call into the runtime.

Outside a host, `SOA_COUNT_CALLS=1` (or `=file.tsv`) turns counting on from
the start of a run. The end-of-run report then gets a `[swap]` line, and a
TSV with one line per function if a path was given. Combined with
`SOA_PAD_FILE`, this counts over a recorded play.

`SOA_PIPE_CENSUS=file.tsv` counts, beside the calls, the bytes and stores
each translated function writes to GX's write-gather pipe itself, from its
own blocks and not its callees': every function that talks to GX directly,
the list a native draw (soa-ue5's Layer 2) has to cover. docs/RECORD.md, "Who
writes the pipe", has the format and a measured census.

## A swap on against off

The check for any swap that does not change timing semantics: the same pad
script and card copy under `SOA_CLOCK=fixed`, once with the replacement set
and once without, must give the same frame hashes and the same call counts
but for what the answer replaces (its `translated` count, and the calls its
translation made). The fixed clock's quiet rule makes that hold: ordinary
code costs no guest time, and since its free reads neither does a read of
the clock or the hardware, so an answer moves nothing unless the
translation it stands for waits on a device -- then it says how long, with
`soa_host_charge`. docs/CLOCK.md, "Native answers and the clock" and "Reads
cost nothing", has the design and the runs: the 10 former `replace.c`
functions and the six disc lookups answered over three scenes, 12,000
frames, no frame different (with 8 ticks a read; not re-run since); and
the whole native platform (disc, sound,
card, pad: 122 replacements) over 21,000 frames, no frame or sample
different.

## Measurements (2026-10-17, this machine, gcc 16, `--optimize`)

The run was the opening as in `speed.scn` (its pad script), headless, muted,
at `SOA_SPEED=30` so the host CPU is the limit (12 VI retraces per frame).
Each run was 6,000 frames, using a copy of a blank card. The order was five
interleaved A/B pairs (A B, B A, ...) on a machine with other load.

| build | wall seconds for 6,000 frames (5 runs) | median |
|---|---|---|
| base (`bea764c`, no guard) | 42.60 41.14 41.08 40.58 40.64 | 41.08 |
| swap, nothing replaced | 42.70 40.93 41.33 40.83 41.57 | 41.33 (+0.6%) |
| swap, `SOA_COUNT_CALLS=1` | 44.99 44.58 | 44.8 (+9%) |

- The difference with nothing replaced is +0.6% at the median and within
  the run-to-run spread (40.6–42.7 s).
- Over those 6,000 frames the game made 984 million translated calls (about
  164,000 a frame) into 1,857 of the 7,119 guarded functions. At one compare
  and one predicted branch each, that is about 0.25 s of 41 s, which agrees
  with the measured +0.6%.
- Code size: the single-file `.text` grew by 641 KB (+2.2%, about 90 bytes a
  function: the guard and its cold call). The split game library grew from
  25.40 to 26.04 MB.

The checks:

- `SOA_SELFTEST=1` in `gen/linux` and `gen/linux-split` passes ("any function
  swapped": sprintf answered in place, after, and around its original;
  counted; refused inside a function and for VIGetRetraceCount).
- `tools/tests/test_swap.py`: the guard and table as text, plus a two-function
  program translated, compiled against the real `swap.c` and run.
- A host run of `gen/linux-host-abi23` through `soa_host.h`: one hot function
  replaced, alternately `BOTH` and `original` + `ANSWERED_LIVE`, over 606
  frames. The counts agreed with the host's own (7,456 calls, 3,728 answered,
  7,456 translated).

## Alternatives considered

- **A function-pointer table at every call site** (`g_fn[i](s)`). This was
  rejected because every direct call (about 100,000 call sites) becomes an
  indirect load and call that gcc can no longer inline, and the call-site
  text changes everywhere. The entry guard leaves call sites alone and costs
  one byte compare per call.
- **Hot-patching the compiled code** (`-fpatchable-function-entry`, writing a
  `jmp` into the function). This costs nothing at all, but it needs writable
  code pages (W^X), a different patch for each architecture (x86-64 and
  Android's arm64), and something else again under MSVC. Patching a
  function while another thread runs it is not safe without stopping the
  guest. The byte guard is portable C.

## What changed (and what to rebuild)

- **Within host ABI 24** (2026-10-17): strict mode, functions and one struct
  added after the rest of the header (`soa_host_set_strict`,
  `soa_host_held`, `soa_host_set_pad_at`, `soa_host_strict_stats`;
  docs/CLOCK.md, "Deterministic runs under a host"). Nothing before them
  changed, so the ABI number did not: a plugin built against 24 runs on this
  runtime unchanged and looks the new functions up by name to use them.
  `soa_host_go` and `soa_host_hold(0)` wake a strict wait at once (a
  futex), where the old hold polls every millisecond. The recording format went to version 2
  (`runtime/record.h`, the GX part); `soa-compare` reads 1 and 2. A
  runtime-only change: `--link`.

- **Host ABI 23 → 24** (2026-10-17, `runtime/soa_host.h`): new entry points
  only, at the end of the header: `soa_host_set_clock`,
  `soa_host_guest_ticks`, `soa_host_schedule`, `soa_host_cancel`,
  `soa_host_raise` (docs/CLOCK.md). No struct moved, `SoaHostCall` included:
  an event's function is handed one, with `address` 0. A plugin built
  against 23 refuses a 24 runtime as before; rebuilding it with the new
  header is enough, with no source change.

- **Host ABI 22 → 23** (`runtime/soa_host.h`). `SoaHostCall` grew at its end,
  and there are new entry points: `soa_host_functions`,
  `soa_host_count_calls`, `soa_host_call_counts`, and
  `SOA_HOST_ANSWERED_LIVE`. A plugin built against 22 refuses a 23 runtime
  ("rebuild the one that is behind"). Rebuilding the plugin with the new
  header is enough: the existing camera, party and ship tasks need no source
  change.
- **Game table ABI 1 → 2** (`runtime/soa_game.h`): the swap table's four
  fields. The new seam import is `soa_swap_enter` (`config/seam.txt`).
- `config/hle.txt` loses the 10 `replace.c` lines, which takes it back to 25
  bindings. `config/seam.txt` loses their 10 twins. `runtime/replace.c` is
  deleted. `runtime/swap.c` and `runtime/swap.h` are new. `host.c`'s
  `soa_host_replace` now goes through `swap_set`.
- The translated C changed, so a full `--compile --optimize --link` is
  needed for every variant.
