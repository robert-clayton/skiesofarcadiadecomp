# Architecture

How this port is put together, and where to put a breakpoint.

[SPEC.md](SPEC.md) says what was decided and why; [ROADMAP.md](ROADMAP.md)
and [PLAN.md](PLAN.md) say what is done and what is next. This page says
what the code actually does today, so that reading one frame's path here
saves reading twenty thousand lines. Everything below was checked against
the tree; where the built thing differs from the plan, the last section
says so.

---

## The three kinds of code in one process

**Translated guest code** — `gen/`, a build artifact, never committed.
`tools/recompile.py` turns every one of the 7,144 functions in the game's
executable into a C function taking one argument: `void fn_800A1234(CpuState*
s)`. Guest registers live in that `CpuState` (`runtime/cpu.h`); guest memory
is one byte image kept in the console's big-endian order, and every load and
store swaps as it goes (`mem_r32`, `mem_w32`). A call the recompiler can
resolve is a direct C call; anything else goes through the generated
`dispatch(s, addr)` switch (`tools/soa/recomp/emit.py`, `dispatch_c`).

**The runtime** — `runtime/`, 28 C files compiled with MSVC at `/std:c17
/fp:strict` (and on CI under clang-cl, gcc and clang as well). This is the console: the memory window, the device models the
guest programs through memory-mapped registers, the software graphics
pipeline, the AX mixer, the window, and the diagnostics that make a wrong
run legible.

**Hand-decompiled C** — `src/`, compiled twice. Once with the original
Metrowerks compiler, so `tools/matchcheck.py` can compare it word for word
with the executable; once with MSVC, renamed `dc_*`, so it can actually run
in the port. `config/hle.txt` decides which functions the running port uses
(25 entries today: 12 decompiled routines; `VIGetRetraceCount`, which
`tick.c` answers so the main loop has a safe point; and the five data-cache
range calls, answered as the no-ops they are with no cache to keep).

**Threads in the process.** One guest CPU thread, which owns every device
model and parses the graphics command stream; that thread's guest threads
live on host fibers underneath it. Then the rasterizer's worker pool
(`SOA_THREADS`, default three quarters of the logical CPUs, capped at 16), the UI thread when a
window is open, the watchdog, and the profiler's sampler.

---

## One video frame, end to end

### 1. The guest writes to the graphics pipe

Translated code has no idea graphics exist. A `stw` becomes
`mem_w32(s, ea, v)`, a `stfs` becomes `mem_wf32` (which is `mem_w32` with a
`memcpy` in front of it), and a quantized `psq_st` goes through
`psq_store` into the same `mem_w8`/`mem_w16`/`mem_wf32`. There is no special
case anywhere in the emitter for the gather pipe — grep
`tools/soa/recomp/emit.py` for `0xCC008000` and nothing comes back.

`runtime/cpu.h` · `mem_w32` tests `is_mmio(ea)` (top byte `0xCC` or `0xE0`)
and, nested inside that, `is_gather_pipe(ea)`
(`(ea & 0xFFFFFF00) == 0xCC008000`). A pipe store calls `gx_pipe_write`; any
other hardware store goes to `mmio_write32`; everything else — nearly every
store the guest makes — pays one compare and writes to the image.

### 2. Bytes become a command stream

`runtime/gx.c` · `gx_pipe_write` appends the bytes big-endian to `g_pipe`,
offers them to the frame capture (`cap_append`), and calls `pipe_flush`,
which calls `parse`. The exception is a display list being recorded: between
`GXBeginDisplayList` and `GXEndDisplayList` the bytes go into guest memory
at the PI write pointer instead (`dl_record`), as the console's gather pipe
puts them, and are parsed only when the game calls the list (C5b).

`runtime/gx.c` · `parse` consumes whole commands and leaves a partial one
for the next store:

| Opcode | Meaning | What `parse` does |
|---|---|---|
| `0x08` | CP register | shadows it in `g_cp` |
| `0x10` | XF registers | shadows a run into `g_xf` |
| `0x20`/`0x28`/`0x30`/`0x38` | indexed XF load | reads the matrix out of guest memory through the CP array registers |
| `0x40` | display list | recurses into guest memory with `in_display_list = 1`; a live call also puts the list into the capture inline, as a `0x41` record only a replay reads |
| `0x61` | BP register | `load_bp` |
| `0x80`–`0xBF` | draw | sizes the vertices and calls the renderer |

Unknown bytes are reported once and resynchronised one byte at a time — the
`unknown FIFO bytes` counter `tools/scenario.py check` fails a run on.

### 3. Registers reach the renderer

`runtime/gx.c` · `load_bp` shadows the BP register and forwards every write
to `runtime/gxr.c` · `gxr_bp_written`, which routes the handful that need
more than shadowing: `0xE0`–`0xE7` are TEV colour registers
(`runtime/gxr_tev.c` · `tev_register_written`), `0x65` is a palette load
(`tmem_load_tlut`), `0x52` is an EFB copy (step 9 below), `0x66` is the
texture-cache invalidate, which with every copy moves the texture epoch
(step 6), and `0x45` with bit 1 is `GXDrawDone`, which flushes the queue
because the CPU is about to read what was drawn.

### 4. A draw becomes vertices

`runtime/gx.c` · `vertex_size` works out how many bytes one vertex occupies
from the current vertex descriptor and format, and `gxr_draw` →
`gxr_draw_inner` (`runtime/gxr.c`) takes it from there.

`runtime/gxr.c` · `decode_vertex` reads each attribute according to the VCD
(CP `0x50`/`0x60`) and the VAT (CP `0x70`/`0x80`/`0x90` + format index),
following data inlined in the stream or indexing the arrays at CP
`0xA0`/`0xB0` through guest memory (`attr_data`). An index that would leave
MEM1 is counted as a bad vertex reference and dropped — the other counter
the scenario checker fails on.

### 5. Transform, lighting, texture coordinates

`runtime/gxr.c` · `transform` is the XF block:

- position through the matrix at `4 * posidx` (`mat_mul_3x4`);
- normals through the matrices at XF `0x400`, renormalised;
- projection from XF `0x1020`–`0x1026`, perspective or orthographic;
- two colour channels through `light_channel` (XF `0x100E`+: ambient,
  material, per-light diffuse and attenuation);
- texture coordinate generation from XF `0x103F`/`0x1040`+, including the
  dual-transform post matrices at XF `0x500` when XF `0x1012` asks for them.

Out comes a `Vertex` (`runtime/gxr.h`) in clip space, with its screen
position still unset.

### 6. The draw is queued

Still on the guest thread, `gxr_draw_inner`:

- `runtime/gxr_tev.c` · `tev_prepare` resolves the combiner — stages,
  input selectors, konst, swap tables, alpha compare — and decodes and
  caches every texture the stages sample (`texture`, `decode_texture`).
  The cache holds 1,024 decodes, found through an index, and hashes a
  texture's source bytes at most once a *texture epoch*, which moves on the
  game's texture-cache invalidate (BP 0x66), on every EFB copy and on a
  replay's RAM load (PLAN-60FPS-MODS H12). Each bound texture also carries
  its cache slot and a generation that moves on every decode, replacement,
  copy image and eviction (`TexCfg.tex_id`, `tex_gen`), so a backend can
  tell it changed without hashing (V2);
- `pixel_prepare` and `raster_prepare` snapshot blend, logic op, depth,
  fog, the viewport and the scissor, so a later register write cannot
  change a queued draw;
- the vertices go into a shared arena and the `DrawCmd` into slot
  `g_published & QMASK` of a 4,096-entry ring, claimed in `claim_slot`,
  which waits (without draining) for every worker to be past the command
  that slot last held, and stamps the command's fence (section 9);
- `publish` increments `g_published` (`plat_inc64`) and, if any worker has
  gone to sleep on it, wakes them with `plat_wake_all64` (`WakeByAddressAll`
  on Windows).

In a pair replay (section 12) two more calls sit on this path, both on the
producer and both before `publish`: `pair_claim` keys the draw before its
vertices are transformed, and `pair_positions` records them -- or moves
them to the point between two frames -- after. With pair mode off each is
one untaken branch.

Commands are *numbered*, never re-indexed. `g_published` has one writer (the
producer) and `g_ran[i]` one writer (worker `i`), and none of them is ever
reset, so a stale read is always too small and can only make a thread wait.
Every read of another thread's count goes through a seq_cst `plat_load*`
from `plat.h` (portability.md 3.4: a plain `mov` on x64, `ldar` on ARM64),
and `tools/tests/test_gxr_atomics.py` checks that over the source.
With no workers the producer runs the command itself on the spot
(`run_here`): with `SOA_GXR_INLINE=1`, or whenever a backend is set
(`gxr_set_backend`, or `SOA_GXR_BACKEND=passthrough`), no worker starts,
and every command goes through the backend if there is one and
`draw_command` if not. The command types live in `runtime/gxr_cmd.h`, and
each command carries `efb`, the EFB it was built for -- 0, or 1 for H17's
in-between image -- which `claim_slot` stamps from the producer's target,
so a consumer never reads it from global state (specs/gpu-backend.md 3.1,
V2).

### 7. Workers rasterize it

`runtime/gxr.c` · `worker` spins on one shared word (`mine >= g_published`)
and after 4,000 spins parks in `plat_wait64` on it (`WaitOnAddress` on
Windows, a private futex on Linux and Android), for at most 50 ms at a time (PLAN-60FPS-MODS H11), then runs `draw_command`.

`draw_command` expands quads, triangles, strips and fans into
`emit_triangle`, which clips against the near, `w` and far planes
(`clip_polygon` → `clip_against` → `clip_dist`), projects the survivors
(`to_screen`) and calls `raster_triangle` on the resulting fan. Lines and
points are drawn on one worker only.

`runtime/gxr.c` · `raster_triangle` computes the signed area and culls,
clamps the bounding box to the scissor, builds edge functions
(`plane_of`), builds perspective-correct planes for depth, `1/w`, the two
colours and the eight texture coordinates, then walks rows. **Row ownership
is the parallelism**: worker *i* takes the rows where
`y % nthreads == i - 1`, so per-pixel ordering within a row is preserved and
no two workers touch the same pixel. Per texcoord, the level of detail is
evaluated at both ends of the span (`span_lod`) and interpolated across it.

### 8. Texturing, the combiner, depth and blend

`runtime/gxr.c` · `shade` is one fragment:

1. if PE_CONTROL `ztop` is set, the depth test runs *first*
   (`depth_test`) -- and so it does when the draw's alpha compare passes
   every alpha (`TevSetup.alpha_always`, decided once a draw by trying all
   256), since then the order cannot change a pixel and the TEV is spared
   every fragment depth would discard (PLAN-60FPS-MODS H15a);
2. `runtime/gxr_tev.c` · `tev_pixel` runs the stages. Each texture
   coordinate a stage uses is divided by `q` once, and each enabled stage
   samples
   (`sample` → `sample_level`: wrap or clamp or mirror, bilinear, mip level
   from the interpolated LOD), applies the swap tables, and evaluates the
   colour and alpha ops into a bank of four registers. Then the two alpha
   comparisons and their logic op decide `alpha_pass`;
3. a failed alpha test returns here, counted per worker. It is not a rare
   path: `build/boot_window.log` reports 792,513,505 fragments shaded
   against 69,749,462 that failed alpha and 269,041 that failed depth;
4. if it did not run first, the depth test runs now;
5. `fog_apply` blends toward the fog colour by eye distance recovered from
   the screen z;
6. `blend_pixel` does the blend or the logic op or neither, and `col_upd` /
   `alpha_upd` decide what actually lands in `g_efb`.

The EFB is a plain `uint8_t g_efb[528][640][4]` with a separate 24-bit depth
buffer, both in `runtime/gxr.c`.

### 9. The copy out

BP `0x52` reaches `runtime/gxr.c` · `enqueue_copy`, which does **not** copy
on the spot: it queues a `DrawCmd` with `kind = 1` so that the copy runs in
order behind the draws it is copying. `run_copy` then calls:

- `copy_to_screen` when bit `0x4000` says the destination is the display
  buffer — fills `g_screen` and bumps `g_frames_presented`;
- `copy_to_texture` otherwise — writes the EFB rectangle back into guest
  memory tiled in the format the sampler decodes, which is how the game's
  own render-to-texture effects work;

and then `efb_clear` if the copy asked for a clear. With a backend set the
clear is always its own kind-2 command, so a backend's copy never clears,
and the hook in `run_here`, not the backend, counts a screen copy in
`g_frames_presented` (V2).

A copy that reads rows other workers own -- a filtered one, which is every
copy the game makes, or a half-scale one -- needs every earlier command
finished on every row it reads, and nothing later written to those rows
until every worker has read them (PLAN C3's race). Until H14 two full
drains around each such copy did that, and the producer waited for the
whole frame at every copy. Now the workers order themselves with
**fences**: the copy carries an entry fence (no worker starts it until
every other worker has finished everything before it), and the command
after it an exit fence (no worker starts that until every worker has
finished the copy). `fence_wait` reads only the other workers' `g_ran`
counts, and a fence is never above its own command's number, so the
worker furthest behind can always run: no fence can deadlock. Every screen
copy is entry-fenced too, so `gxr_presented` still counts whole frames.
A copy that is foreign for its filter alone -- full scale, its rows the
workers' own -- reads, for each row it writes, only the row either side,
which the writer's two neighbours own (worker k's rows are `y % n == k-1`),
so its entry and exit fences ask those two and no one else (`fence_near`,
FINDINGS "Neighbour fences"). A screen copy's entry fence stays whole.

What the producer reads of guest memory a queued copy may be writing --
a texture (`gxr_texture_hazard`), a palette (BP 0x65), vertex arrays, a
display list or an indexed XF load (`gxr_source_hazard`), a hook's peek,
poke or mod access (`gxr_hook_hazard`) -- waits for the newest copy that
overlaps it, found in the list of copy destinations with their exact
extents and command numbers, and for nothing else. A texture read covers
every mip level its decode will read, not the base alone. A draw token (BP
0x47/0x48) is answered as it is parsed and does not wait: this game writes
its token at the top of each frame, before the frame's logic, and waiting
there took all of H14's gain (FINDINGS "H14"), so a copy may still be
running when a token is answered, and the report counts how often.
`SOA_GXR_TOKENWAIT=1` makes a token wait for the newest copy, and
`GXDrawDone` still drains. `SOA_GXR_DRAIN=1` restores the drains.

A draw that samples a copy's own texture does not wait at all (**copy
images**, FINDINGS "Copy images"). At the copy, `tex_copy_image` points the
texture cache's entry for exactly what the copy makes -- its address,
format and size, no palette -- at a fresh RGBA image, and each worker,
having written its rows of the copy to memory, decodes those rows into it
(`tex_decode_row`, through the same `decode_texel` a decode from memory
uses; a row's texels are all its owner's). A lookup of that key while the
copy is still the newest queued write to those bytes takes the image,
unhashed and undecoded, and raises the draw's fence to the copy, so no
worker rasterizes the draw until every worker has finished the copy.
Anything else retires the image and the texture is decoded from memory as
before: a newer copy over any of its bytes, a drain, a token that finds
every copy finished (the game may write the destination then), a hook's
wait on those bytes, a lookup wanting mip levels, `SOA_TEXVERIFY`. No
images are made under `SOA_GXR_DRAIN=1`, with no workers, or while a mod's
texture provider is registered, which is asked by a hash an image does not
have.

### 10. Out to the window

`runtime/window.c` · `ui_thread` polls `gxr_presented()` and, when it moves,
calls `present`, which converts RGBA to BGRA into `g_bgra`. `dxgi_present`
scales that by whole pixels into a flip-model swap chain's back buffer, at
`SOA_SCALE` (default 2), and presents it held for `g_interval` refreshes:
two at 60 Hz, four at 120, one where the display's rate is not a multiple
of 30 (H8). With `SOA_PRESENTER=gdi`, or if DXGI cannot start, `present`
invalidates the client area instead and `wndproc`'s `WM_PAINT` puts the
frame on screen with `StretchDIBits`. With `SOA_GPU=vulkan` (V8) the
window's thread calls `gxv_present` instead: the GPU's own screen copy
through a Vulkan swap chain, `present.frag` doing `picture_scale`'s
layout and nearest neighbour, each frame presented `g_interval` times.

### 11. End of frame

Back in `runtime/gx.c` · `load_bp`, a copy to the display buffer also calls
`frame_end`, which is the frame counter everything else is keyed to:
`SOA_FRAMES`, `SOA_SNAP`, `SOA_PAD` scripts, and the frame numbers in a
`SOA_PAD_RECORD` recording. `frame_end` is also where a requested FIFO
capture is written (`.fifo`, `.regs` and a full `.ram` image), and where
`SOA_FRAMES` stops the run after flushing the queue. Since H14 the frame
end runs while the workers may still be drawing the frame: its screen
copy is published but not waited for, and the next frame's first command
drains (the *frame gate*, the one full drain a frame). A frame being
captured is drained before its hook runs, so its `.ram` holds every copy.

`SOA_HASH=1` prints one FNV-1a hash per presented frame from `enqueue_copy`,
after a `gxr_flush` so the hash describes the finished frame. That is the
value `config/fifo_manifest.tsv` pins for all 23 captures, and that
`python tools/scenario.py replay` re-checks at `SOA_THREADS` 1, 2, 3 and 8.

### 12. The in-between image and the vertex history (H10)

`soa.exe --replay F F+1` (`runtime/gx.c` · `gx_replay_pair`) renders three
passes: F with `GXR_PAIR_RECORD`, F+1 as it is, and F+1 again with
`GXR_PAIR_LERP`, to `F.png`, `F+1.png` and `F+1.mid.png`. On the draw path
(section 6) `pair_claim` keys each draw as `tools/fifopair.py` does -- its
display list, the arrays its vertex layout indexes, the textures its stages
sample, its primitive, vertex count and TEV setup, but not the TEV colour
registers, so a fade stays one draw -- and numbers it in stream order.
RECORD files the draw under its key; LERP takes the oldest unmatched draw of
F with the same key. After the transform, `pair_positions` keeps the
clip-space x, y, z and w of RECORD's vertices, or replaces LERP's with
(1-t)·F + t·F+1. Colours and texture coordinates stay F+1's. A draw with no
match, or whose projection kind changed between the frames, is drawn as F+1
draws it. In the LERP pass a copy to texture is skipped, since it would
write what F+1's own frame reads next, but a clear the copy carries is
kept, because the draws after it expect the EFB it leaves.

What is kept, and the rules the live path (H16, H17a) inherits:

- **per frame**, a 32-byte record per keyed draw (`PairRec`), an
  open-addressed key table (`PairSlot`, 32,768 slots) and 16 bytes of
  position per vertex, for at most 16,384 draws and 262,144 vertices. The
  busiest frame measured, the cutscene's, has 4,280 and 27,676. A frame past
  either limit is counted in the report, not fatal. Two frames (current and
  before) come to about 10.5 MB, allocated only when pair mode is first
  switched on;
- **outside the vertex arena, the queue and the texture graveyard.** A drain
  recycles those -- at the frame gate, and in the middle of a frame whenever
  the arena or the graveyard fills -- so nothing a drain touches holds pair
  state (a test pins that `drain()` names none of it);
- **owned by the producer.** It claims, records and lerps before `publish`,
  so no worker ever sees a command whose positions are half replaced;
- **rotated only at the screen copy** (`enqueue_copy` with `to_screen`),
  which is what makes "the frame before" mean the frame last presented.

Two things the live version needs that the offline one does not. The
in-between command has to be built next to the real one, and so needs a
second EFB: `g_efb` and `g_efb_z` are single globals, which blending, clears
and copies all write. And both frames' commands have to be built before
either is rasterized. Built that way, no texture has to be kept alive for a
second frame, because the in-between image samples what F+1 samples.

---

## One sound frame, more briefly

A frame of audio is 5 ms: 160 samples at 32 kHz, three channels
(`FRAME_SAMPLES` in `runtime/ax.c`).

1. **The driver hands over a command list.** It writes the mailbox at
   `0xCC005002`: `mem_w16` → `runtime/hle.c` · `mmio_write16` →
   `runtime/irq.c` · `device_write` → `runtime/dsp.c` · `dsp_write`.
   A `0xBABExxxx` mail arms the next mail to be a command-list address.
2. **The microcode's frame of work.** `runtime/dsp.c` · `handle_mail` calls
   `runtime/ax.c` · `ax_command_list` with that address, then mails
   `DSP_YIELD` back with the DSP interrupt. There is no DSP interpreter and
   there does not need to be one: the game runs Nintendo's stock AX
   microcode, so the mixer is reimplemented rather than emulated.
3. **The mixer.** `ax_command_list` walks the opcodes. `0x02` sets the
   parameter-block list address, `0x03` processes it
   (`process_pb_list` → `process_voice` per voice): each voice reads its
   samples out of ARAM (`runtime/aram.c` · `aram_memory`), decodes
   DSP-ADPCM or PCM in `fetch_sample`, resamples, applies its volume ramp,
   and `mix_add`s into the main and auxiliary buses. `0x04`/`0x05`/`0x10`
   exchange an auxiliary bus with the CPU's own effects pass; `0x0E`
   (`output_samples`) writes the finished frame back into guest memory as
   interleaved 16-bit, right channel first, as the audio interface wants it.
4. **The DMA engine plays it.** The guest arms AI DMA at `0xCC005036`
   (`dsp_write`), which sets a period from the block count on the guest
   timebase. `runtime/dsp.c` · `dsp_poll`, called from `deliver_pending`,
   notices the period elapse, hands the block to `runtime/audio_out.c` ·
   `audio_push_block`, and raises AIDINT.
5. **Out to the speakers.** `audio_push_block` meters the peak, appends to
   the `SOA_WAV` file if one was asked for, and queues the block on
   `waveOut` — **dropping it outright if no header is free**, which is the
   one place an audible defect can appear that no correlation test would
   catch.
6. AIDINT is delivered as interrupt 5 on the next pass, and that is what
   makes the driver build the next frame.

---

## One interrupt, more briefly

Nothing in a recompiled program is asynchronous, so interrupts are delivered
synchronously, at exactly two kinds of place.

1. **The delivery points.** The scheduler's idle spin, hooked at
   `0x80237BA8` (`config/hooks.txt` → `runtime/irq.c` · `hook_80237BA8`);
   and every backward branch in translated code, where
   `tools/soa/recomp/emit.py` · `_jump` emits `irq_poll(s)` — so a thread
   spinning on a flag with interrupts enabled still gets them.
2. **The gate.** `runtime/irq.c` · `irq_poll` returns at once if MSR[EE] is
   clear or a handler is already running, and otherwise runs
   `deliver_pending` on every 16th call, because the devices are clocked by
   a timebase that is not worth reading more often.
3. **The order.** `deliver_pending` reads the guest timebase
   (`runtime/hle.c` · `guest_timebase_hi`/`_lo` — the host clock, multiplied
   by `SOA_SPEED`) and works down a fixed list: decrementer, pixel-engine
   finish and token, DVD and ARAM polls, DSP and AI, ARAM, DI, VI, EXI (up
   to four rounds, because each EXI handler can leave the other pending),
   SI.
4. **The VI retrace**, at 60 Hz of guest time, is the game's heartbeat.
   Before the handler runs, `runtime/si.c` · `si_poll` reads the
   controllers once per field — which is where `runtime/window.c` ·
   `window_pad`, the `SOA_PAD` script and a `SOA_PAD_FILE` replay all enter
   the guest.
5. **Calling the handler.** `runtime/irq.c` · `call_guest_handler` saves the
   whole register file, puts the interrupt number in r3 and the current
   `OSContext` in r4, clears MSR[EE], `setjmp`s, and `dispatch`es into the
   guest's own handler — which is ordinary translated code.
6. **Returning from it.** A handler may simply return, or it may "rfi" by
   calling `OSLoadContext` on the context it interrupted.
   `runtime/threads.c` · `fn_802335C4` recognises that case and calls
   `irq_return_from_handler`, which `longjmp`s back to that `setjmp`
   however many translated frames deep it had gone.
7. **Afterwards.** If anything was delivered, `irq_poll` runs the guest
   scheduler at `0x80237C84` with the registers saved around it, because a
   handler may have woken a thread and the real hardware path would have
   rescheduled too.

---

## Every file under `runtime/`

**Device model** means the guest reaches it by storing to a memory-mapped
register and it behaves like the chip. **Renderer** is the software graphics
pipeline. **Host plumbing** is what the host provides that the console did
not. **Diagnostic** is there to explain a run, not to run it.

| File | Kind | What it is | If it is wrong |
|---|---|---|---|
| `aram.c` | device model | 16 MB of ARAM, its DMA engine, the DSP status register, and a census that names ARAM uploads against files on disc | Every sample the mixer reads is wrong or silent; `aram_irq_pending` not firing stalls the SDK's ARQ queue, and asset streaming into ARAM stops dead |
| `dsp.c` | device model | The DSP mailboxes and the stock AX mail protocol, plus the AI DMA clock that paces playback | Boot hangs: the SDK's DSP task system waits for a reply that never comes. A wrong DMA period makes the driver's frame callback fire at the wrong rate, so audio runs fast, slow or not at all |
| `dvd.c` | device model | The seven DI registers; each read is `disc.c`'s, straight into guest memory, its transfer-complete interrupt raised when the drive's modelled time (6 ms plus 3 MB/s) is up | Assets read as zeros (which looks like a content bug, not an I/O bug), or the game waits forever for a completion that never arrives |
| `disc.c` | host plumbing | The player's disc image (disc-layer I1): a folder holding `GEAE8P.soadisc` (the checked store, I5, each 64 KiB block hashed at first touch, exit 9 naming the file if one is damaged) or `disc.iso`, or either file itself; refuses an image that is not GEAE8P revision 0 or whose executable is not the one the code was translated from (`sha1.c`); hands `main.c` the executable, boot.bin and the file table -- the build's own copies when `recompile.py` built them in (`gen/disc_sys.c`, I3), each checked against its SHA-1 at boot, and then an image whose table differs is refused and the self test and replays open none -- `dvd.c` its reads (zeros past the end, counted), and the census names and heads; `SOA_DISC_LOG=1` names the file each read lands in | The port boots another disc's data under this one's code, or refuses the player's own; a read from the wrong offset loads the wrong asset, which looks like a content bug. `tools/citest/disc_check.py` holds it to a synthetic image |
| `exi.c` | device model | Three EXI channels: the memory card in slot A as a file, the RTC and SRAM. Two interrupts per card, since the CARD library waits on the device's own line for every write | The card does not mount, or every save sits out a 100 ms timeout and fails. A wrong SRAM checksum makes a card formatted elsewhere unverifiable |
| `si.c` | device model | The four controller ports, direct transfers and per-field polling; plus `SOA_PAD` scripting, `SOA_PAD_RECORD` and `SOA_PAD_FILE` replay | The game reads garbage input, or a recording and its replay disagree — which is the one failure that silently invalidates every scripted scenario |
| `gx.c` | device model | The graphics front end: the write-gather pipe buffer, the GX command parser, the CP/XF/BP shadow registers, pixel-engine interrupt state, the frame counter, FIFO capture and `--replay` | The command stream desynchronises and *everything* downstream is garbage. It also owns the frame number `SOA_FRAMES`, `SOA_SNAP`, `SOA_PAD` and every recording are keyed to |
| `irq.c` | device model | Interrupt delivery, plus the VI, PI and AI registers and the decrementer. The one place a handler is ever entered | No interrupts means no frames, no input, no audio and no DVD completions — the run simply stops making progress. A wrong delivery *order* shows up as rare, timing-dependent hangs |
| `gxr.c` | renderer | Transform, lighting, texgen, clipping, the rasterizer, depth, fog and blend, the EFB, the command queue and its worker threads, EFB copies and clears, frame hashing | Wrong pixels. The 23 pinned captures in `config/fifo_manifest.tsv` are the check: a change that moves a frame moves a hash |
| `gxr_tev.c` | renderer | Texture decode and cache, palettes, sampling, the TEV combiner and the alpha compare | Wrong colours. A cache bug is worse than a decode bug, because it shows up as *stale* textures in some frames and not others |
| `gxr.h` | renderer | Shared types (`Vertex`, `TevSetup`, `TexCfg`), the EFB dimensions, and the producer's phase-accounting clock | A phase mistake makes the profile lie about where the time goes; the pipeline itself is unaffected |
| `gxr_cmd.h` | renderer | The commands the producer builds and a consumer runs -- `DrawCmd` with its `PixelCfg`, `RasterCfg` and `Rect`, and since V2 the `efb` it was built for -- and the backend interface: `GxrBackend`, `gxr_set_backend`, `gxr_backend_screen`, the exported clip helpers and `gxr_set_target` (specs/gpu-backend.md 3.1). What a GPU backend is written against | A backend that reads a field the producer stopped filling, or global state the producer has already moved on from, draws the wrong command |
| `gxv.c`, `gxv/` | renderer | The Vulkan backend (specs/gpu-backend.md 3.2-3.10), a `GxrBackend` that draws the renderer's commands on the GPU: the vertex stage with the CPU's own clipping, the TEV and sampling in `gxv/raster.frag` (`tev.glsl`, `lod.glsl`), every EFB copy in `gxv/copy.comp`, logic ops three ways, and the screen copy read back through `gxr_backend_screen`. It opens Vulkan at run time through `plat_dl_open`, so nothing links against it. Built two ways: with `SOA_GXV=1` it is the backend, compiled against Vulkan-Headers and the SPIR-V `tools/soa/shaders.py` makes from `gxv/` -- `tools/gpuspike.py` always builds it so, and `--link` does when `tools/fetch_gpu.py` has filled `vendor/` -- and without it, a stub whose `gxv_built` says the build has none. `tevdiff.comp` and `loddiff.comp` are the differentials' shaders, and the `GXV_MUTATE_*` variants the mutations'. In `soa.exe`, `SOA_GPU=vulkan` starts it (`main.c`'s `gpu_start`, `gxv_start`) before the renderer's first command, and anything short of a device is one `[gxv] fallback:` line and the CPU (V5). Since V6a it is the render queue's one consumer on a thread of its own (`GxrBackend.own_thread`); the producer only publishes. Since V7 its pipelines are specialised on the TEV's shape, made on a compiler thread while the interpreter draws, and kept on disk; and a copy to a texture is counted when recorded and lands in guest RAM when its submission is done (`GxrBackend.lands_late`, gxr's `g_landed`), a draw in the same submission sampling the copy's image from the pool | A GPU picture that differs from the CPU's: the spike's oracle and differentials (FINDINGS "V3"-"V5") are how to tell which is right |
| `plat.h` | host plumbing | The platform layer (portability.md 3.2), header-only: `PLAT_X86_64`, `PLAT_ARM64`, `PLAT_MSVC` and `PLAT_GNU`, each 0 or 1, so a file tests a meaning rather than a compiler; and, since L2a, `PLAT_TARGET_SSE41` and `plat_cpu_has_sse41`, which let `gxr_tev.c`'s SIMD blend build under any x86-64 compiler while choosing it at run time. Since L2, everything the renderer asks of a host: the queue's atomics (`plat_load*`, `plat_inc*`, `plat_dec32`, `plat_xchg64`, `plat_cas32`, `plat_compiler_barrier`, all seq_cst, and `plat_store64_relaxed` for the timers a worker charges while the report reads them), spinning and sleeping, waits (`WaitOnAddress`, or a futex on Linux and Android), clocks (`plat_mono_raw`, `plat_mono_ns` since L7, and `plat_cycles`: the TSC or ARM64's generic timer), threads and the CPU count, `plat_fseek64` and `plat_setenv`. Since L7 it also declares `plat.c`'s cold half. It declares the six Win32 calls it makes as the SDK does rather than include `<windows.h>`, whose macros `hle.c`'s names collide with; `gxr.h` includes it. It never includes `cpu.h`, so an edit is a `--link` | A wrong test drops the SIMD path silently -- a windows-gnu build did before L2a -- or lets a compiler reach intrinsics it cannot build, as clang-cl did from b377b1f to L2a |
| `plat.c` | host plumbing | The platform layer's cold half (portability.md 3.3, L7). It reserves, commits and releases address space. It puts the fault guard on a reserved range: a vectored handler on Windows; elsewhere `sigaction(SIGSEGV)` on a `sigaltstack`, chaining to the previous handler, with the store bit taken from the page fault's error code or AArch64's ESR record. It also runs the guest on a stack of the size it asks for, and since V5 opens shared libraries (`plat_dl_open`, `plat_dl_sym`, `plat_dl_close`: `LoadLibraryExA` or `dlopen`), which `gxv.c` loads Vulkan with. Only `main.c` and `gxv.c` call it, and every test that links either adds it through `toolchain.runtime_support_sources()` | The MEM1 tripwire goes quiet, or an address past the RAM kills the run instead of being reported once. Off Windows the guest overflows an 8 MB default stack somewhere deep, or a fault that is not the guard's never reaches the handler before it |
| `crmath.h` | renderer | CORE-MATH's correctly rounded binary32 `exp2f` and `log2f` (portability.md 3.8, L6), as `soa_exp2f` for fog and `soa_log2f` for texture LOD: one answer on every platform, where a C library's own answer is that library's. MIT; the changes from upstream are listed at its top, and `config/libm.tsv` pins its outputs over every input the renderer can give it | Fog or mip choice drifts by platform, by a bit at a time, which only `tools/citest/libm_check.py` would see |
| `png.c` | diagnostic | A minimal PNG writer (stored deflate) for `SOA_SNAP` and `--replay` output | Snapshots are unreadable. Nothing the game sees changes |
| `cpu.h` | host plumbing | `CpuState`, the memory window and its mask, byte-swapping loads and stores, MMIO routing, the gather-pipe test, the fiber savepoint declarations | Everything, at the level of a single instruction — so it shows up as arbitrary corruption anywhere. This file is also where the cost of the hot path is decided |
| `main.c` | host plumbing | Boot: allocate and guard the memory image, load the DOL, park the FST, fill low memory, pick the run mode, start the window, the watchdog and the sampling profiler, then jump to `__start` | The image is not what the apploader would have left, and the game misbehaves before any of its own code is suspect. The guarded tail is what turns an out-of-range guest store into one `[mem]` line instead of silent host-heap corruption |
| `threads.c` | host plumbing | Guest threads on host fibers; the `OSSaveContext` / `OSLoadContext` replacements and the guest backtrace walker | A thread resumes with the wrong registers or on the wrong stack — which presents as the game corrupting itself minutes later, nowhere near the switch |
| `hle.c` | host plumbing | MMIO routing to the device models, per-register access counting for unmodelled hardware, the guest timebase and `SOA_SPEED`, guest traps, and the end-of-run report | A register that should be modelled reads zero and the guest quietly takes a wrong branch; `SOA_STRICT=1` is what turns that into a stop with a backtrace |
| `hle_os.c` | host plumbing | Native replacements for `__OSInitAudioSystem` and `__OSStopAudioSystem`, which exist only to leave the right register state behind | The DSP never announces itself and audio initialisation hangs |
| `hle_stdio.c` | host plumbing | The guest's own `printf`, formatted from the EABI argument layout, so the game's messages reach the log | You lose the game's own diagnostics — which is how "memory reallocate error" was ever seen |
| `decomp_swap.c` | host plumbing | The EABI adapters that let a natively compiled decompiled function stand in for its translation, and the program counter each one stores for the profiler | `memcpy`, `memset` and `strcpy` corrupt guest memory. The selftest's twin comparison is what catches it |
| `decomp_shims.c` | host plumbing | Native stand-ins for functions a decompiled unit calls but nobody has decompiled yet. Empty today | A swapped-in function computes the wrong thing while byte-matching perfectly, because the error is in its callee |
| `audio_out.c` | host plumbing | `waveOut` playback, or SDL3's through `audio_sdl.c` off Windows (L10), and the `SOA_WAV` writer | Nothing is audible, or blocks are dropped. The mix itself is unaffected: `ax.c` writes into guest memory whether or not a device exists |
| `ax.c` | device model | The AX mixer: the command list, parameter blocks, voices, resampling, the buses, and the census the report prints. Reached through `dsp.c`'s mailbox rather than through registers of its own, because that is how the console reaches it too | Wrong or missing sound, and the game never notices — it writes a command list and reads buses back, so an error here is silent outside the report |
| `soa_game.h`, `game.c`, `elfcheck.c` | host plumbing | The game's table (specs/android.md 3.2), which every build links from `<out>/game_table.c` and the self test holds to the direct calls; and, in a split build (`--split`), the loader that finds `libsoa_game.so`, checks it before `dlopen` and hands its table to the runtime | A split build refuses a library for another machine, page size or build, in words; a single-file build is unchanged |
| `window_sdl.c`, `audio_sdl.c` | host plumbing | Off Windows (L10, built when `vendor/sdl3` holds SDL3): window.c's window, pads, rumble and report on SDL3, every SDL call on its own thread; and the sound's SDL audio stream | As window.c's and audio_out.c's, on Linux |
| `window.c` | host plumbing | The Win32 window on its own thread, presented through a DXGI flip-model swap chain paced to the display's refresh (GDI with `SOA_PRESENTER=gdi`; with `SOA_GPU=vulkan`, gxv's own swap chain, V8), and live keyboard and XInput input for port 1 | No picture, or input the guest never sees. Closing the window is also how a recording session ends cleanly — the `WM_QUIT` path is what flushes the last of what the player did |
| `mod.c` | host plumbing | `SOA_MODS`: data-patch mods checked against the DOL's SHA-1 and applied from the frame hook after the pokes, and native `mod.dll` mods on `soa_mod.h`'s `SoaModApi`, whose callbacks run from the frame hook and the main loop's safe point; a mod with any fault is refused whole, with its file and line | With mods unset nothing: it is not reached. With them, a patch lands at the wrong frame or not at all, and the end-of-run lines say how often each applied |
| `tick.c` | host plumbing | `VIGetRetraceCount`, native (M2): the original everywhere but the main loop's two call sites, told apart by `lr` -- the top of the loop runs the safe-point callbacks, and the frame end's spin is let go after one field once `SOA_UNCAP` unlocks it | The game's frame pacing: a wrong answer at the spin is a game at the wrong speed, which self-test case 74 and `test_tick.py` hold |
| `settings.c` | host plumbing | `soa.ini` beside `soa.exe` (M5): the switches a player would set, and the disc, applied where the environment is silent, and the ones that change the game named in a pad recording; off for every check (`SOA_SETTINGS=0`) | A player's file could move a check if a script forgot `SOA_SETTINGS=0`; `test_settings.py` holds that each one sets it |
| `clock.c` | host plumbing | The guest's clock (M19): the timebase every timer and device deadline counts in, accumulated from the host's monotonic time between reads, with a gap past `SOA_CLOCK_GAP_MS` (250) counted as none, a pause excluded, a speed change continuous, and an epoch dsp.c watches; `hle.c`'s `timebase()` reads it; with `SOA_CLOCK=fixed` a fixed-step clock instead, moved only by the guest's own work, so a run repeats exactly (docs/CLOCK.md) | A host sleep counted as guest time makes every deadline in the gap fire at once and the game's play-time clock jump; `test_clock.py` drives it with synthetic host times |
| `picture.c` | host plumbing | Where the picture goes in the window -- `picture_layout`, the largest whole multiple of the frame (`integer`) or the largest 4:3 that fits (`fit`), centred with black bars -- and `present_interval`, how many refreshes each frame is held (H19a); pure, built alone by `test_picture.py`, whose Python twin checks a windowed run's `[window]` lines | A layout that disagreed between the two presenters, or with the log, would show as a moved picture only by eye; the twin is what catches it |
| `seed.c` | host plumbing | The race seed (P6): with `SOA_SEED`, OSGetTick answers its three reseed sites -- a field load, two at a battle start -- with values from the seed, the site and the call count, through `guest_timebase_lo` in `hle.c`, gated on OSGetTick's pc | A pin that leaked to another timebase read would move a device's clock; the pc gate is what stops it, and `test_seed.py` and the self test hold the sites |
| `trace.c` | diagnostic | Tracepoints from `config/trace.txt`: registers, string arguments and a backtrace at chosen addresses | Nothing about the run changes; you lose the answer to "how far did this get?" |
| `selftest.c` | diagnostic | `SOA_SELFTEST=1`: the library routines, the card and EXI model, the AX mixer, a synthetic frame through the real GX pipe, all 12 decompiled functions against their recompiled twins over 200 random rounds, `VIGetRetraceCount` and the five data-cache calls against theirs, and the paired-single loads and stores against the generic formula they replaced | A false pass here is the worst failure in the tree: it is the check that is supposed to catch the others |

---

## Four things that will otherwise puzzle you

### Guest threads are host fibers, because a context switch cannot be translated

The SDK switches threads with `OSSaveContext` (setjmp-like: returns 0 when
saving, 1 when resumed) and `OSLoadContext`, which restores a saved context
and `rfi`s into the middle of whatever function saved it. **That resume is
the one thing static recompilation cannot express** — there is no C
construct for "continue in the middle of a function whose frame is gone."

So the two primitives are replaced (`config/hle.txt` →
`runtime/threads.c`). Every guest thread gets its own host fiber.
`OSSaveContext` has exactly one caller, `SelectThread`, and the recompiler
wraps that call site as

```c
if (setjmp(*guest_savepoint(s)) == 0) fn_OSSaveContext(s); else guest_resumed(s);
```

so a parked thread always has a live C frame to return into.
`OSLoadContext` switches to the target's fiber if it is not the current one
and `longjmp`s to that savepoint, where `guest_resumed` restores the
register snapshot and hands back r3 = 1, exactly as the hardware path would.

Everything else about scheduling — run queues, priorities, the idle loop,
sleep and wakeup — stays recompiled and authentic. The runtime's only other
intervention in the scheduler is delivering interrupts at the idle loop.

### The write-gather pipe is modelled at the hardware level, not by intercepting GX

The obvious design is to intercept `GXBegin`, `GXSetVtxDesc` and the rest,
and reconstruct draws from the API. This port does not do that.
`config/hle.txt` has 25 entries and **none of them is a GX function**: every
graphics call in the game runs as translated PowerPC, right down to the
individual `stfs` of a vertex component into `0xCC008000`.

What makes that workable is that the pipe is a *byte stream with a
self-describing format*. `gx_pipe_write` appends bytes; `parse` reads
opcodes; the vertex format comes out of the CP registers the stream itself
loaded, not out of any state the runtime tracked at an API boundary. There
is no address check at the store sites and no per-call bookkeeping — just
one compare in `mem_w32` that nearly every store pays anyway, and a
byte-append.

The cost of that decision is visible: `pipe_flush` runs once per guest store
to the pipe, and one saved run pushed 3,018,001,651 bytes through it. The
benefit is that the graphics path is *the console's*, not an approximation
of the SDK's — which is why a captured FIFO stream replays byte-identically
outside the game (`gen/soa.exe --replay`) and why 23 pinned frames are a
meaningful regression test.

Two consequences worth knowing. Display lists are recorded by tracking the
one CPU FIFO that is a list: at each write of the PI FIFO base, the port
reads the global where `GXSetCPUFifo` keeps the current FIFO object
(`r13-27520`), and records while it is the SDK's `DisplayListFifo`
(0x80318B18). Any other redirect of the CPU FIFO -- the logo screen's, for
one -- is parsed at once. Until C5b every list was parsed as it was recorded
and called empty (FINDINGS "Recorded display lists (C5a)"). `HID2` and
`WPAR` are carried in `CpuState`, but nothing reads them. And
because the renderer sees registers rather than API calls, a capture that
merely *inherits* an unmodelled state — rather than writing it inside the
captured window — renders wrong and says nothing.

### A decompiled function can be swapped into the running port and checked against its twin

Every function has one identity: its address. That is what lets the three
implementations coexist.

When `config/hle.txt` names an address, the recompiler emits that function's
translation as `recomp_fn_XXXXXXXX` instead of `fn_XXXXXXXX`, and the
runtime's own `fn_XXXXXXXX` wins the link. For a decompiled routine that
`fn_` is a five-line adapter in `runtime/decomp_swap.c` that marshals the
EABI call — arguments in r3..r5, result in r3 — onto the natively compiled
`dc_*` body from `src/`.

**Both implementations are still in the binary**, so they can be run against
each other. `runtime/selftest.c` · `decomp_selftest` does exactly that: 200
rounds of random strings, each of the 12 swapped-in routines called both
ways on the same guest memory, pointer results and written buffers compared
byte for byte.

That catches a class of bug the mwcc match structurally cannot. `strcmp` is
the standing example: its word-at-a-time path is correct in the mwcc build
and byte-matches the executable, and is *inverted* when the same source is
compiled for a little-endian host. The unit carries a host-order guard for
exactly that, and the twin comparison is written to compare the sign rather
than the value, since the guest only ever tests the sign.

What may be swapped in is restricted, and `config/GEAE8P/units.txt` marks it
`native`: the unit must compile under MSVC, read nothing whose meaning
depends on byte order, touch no hardware register, and call nothing
undecompiled. A routine that reads a field of one of the game's structures
does not qualify and waits for byte-order-aware accessors.

### The renderer is software, and it runs on worker threads

By default there is no GPU backend, no SDL and no shader compiler: the
Vulkan backend (`gxv.c`, `SOA_GPU=vulkan`, GPU spec V5 onward) is optional. The
GameCube's pipeline is implemented in C in `gxr.c` and `gxr_tev.c`, and the
final image reaches the screen through a DXGI flip-model swap chain
(section 10).

The parallelism is one rule: **worker *i* owns rows where
`y % nthreads == i - 1`**, in `raster_triangle`'s row test and in `my_row`
for clears and copies. Because a row belongs to exactly one worker,
per-pixel ordering within a row is preserved without any locking, and the
output does not depend on the thread count — which is checkable, and is
checked, by replaying all 23 captures at 1, 2, 3 and 8 threads and comparing
hashes.

The producer (the guest thread) and the workers meet at one word, and
since H14 wait for each other in three ways, each named in the report's
`[gxr] waits:` line:

- **a drain** (`drain(why)`, which `gxr_flush` calls) waits for every
  `g_ran[i]` to reach `g_published` and only then recycles
  producer-private state — the vertex arena, the copy list, the texture
  graveyard — so what a drain writes and what a running worker reads are
  disjoint sets. It runs once a frame, at the next frame's first command
  (the gate); for a `GXDrawDone`; before a frame is hashed or written to
  PNG; and when the arena, the graveyard or the copy list fills;
- **a wait for one command** (`wait_ran`) holds the producer until every
  worker has finished that command and recycles nothing: to reuse a ring
  slot, to read memory a queued copy writes, and at a draw token under
  `SOA_GXR_TOKENWAIT=1`;
- **a fence** holds a worker at a command until every other worker has
  finished the commands before it (section 9). Only copies that read rows
  other workers own, the command after each, screen copies, a copy that
  writes memory an unfinished copy is still writing, and a draw that
  samples a copy's image carry one.

`SOA_GXR_DRAIN=1` puts back the drains around every such copy, as the
oracle and the fallback, and `SOA_GXR_STALL` holds a worker back to turn a
race into a certain failure (`tools/tests/test_gxr_overlap.py`).

One measured surprise, which changes what "the renderer is slow" would even
mean: over a 3,000-frame profiled run (PLAN A4), the eight rasterizer
workers were busy 8.8 seconds out of 830 seconds of thread time, and 47.4%
of the run was `SelectThread`, the guest's own OS idle loop. This port is
not rasterizer-bound: it runs at the game's own 30 fps cap and idles about
half the time. Read any performance argument against those numbers first.

---

## Where this differs from SPEC.md

`SPEC.md` is the design record, and §10 in particular describes a build that
was planned and not taken. For a reader navigating the tree, the differences
that matter:

- There is no `gfx/` or `audio/` directory. The graphics pipe, the GX
  parser, the TEV and the AX mixer are all in `runtime/`.
- The runtime is C, not C++20, and there is no CMake, Ninja, SDL3 or Vulkan
  anywhere in the tree. `tools/recompile.py` drives the compiler directly with
  the flags of a profile in `tools/soa/toolchain.py`: MSVC by default,
  clang-cl into `gen/clang` with `--cc clang-cl`.
- The binding table is `config/hle.txt`, not `config/symbols.toml`.
- §7 proposed a hybrid: HLE the 104 out-of-line GX entry points and decode
  vertices from HLE-tracked format state. What was built is the
  command-stream parser alone — see *The write-gather pipe is modelled at
  the hardware level* above.
- §8's reference interpreter and lockstep differ do not exist. The
  correctness oracles that do exist are the byte-for-byte mwcc match
  (`tools/matchcheck.py`), the twin comparison in the selftest, the pinned
  frame hashes, and the scenario checker.

Correcting `SPEC.md` itself is PLAN item G2 and belongs in that file.

---

## Where to look next

- `tools/tests/` — 1360 tests, none of which needs a disc (anything that
  would synthesises its fixtures or skips), and `runtime/selftest.c` under
  `SOA_SELFTEST=1`, which does. `docs/TESTING.md` says how to run all of
  it.
- `config/scenarios/` — 13 scripted runs, each with its pad script, frame
  budget, environment and the evidence it was recovered from;
  `python tools/scenario.py list` prints them.
- `config/fifo_manifest.tsv` — the 23 pinned frame hashes, and
  `python tools/scenario.py replay` to check them.
- `README.md` — the environment switches, which are the fastest way to make
  the port explain itself.

---

## Placeholder: licensing

**There is no `LICENSE`, `COPYING` or `NOTICE` file in this tree.** Choosing
one is the repository owner's decision (PLAN item G1), so this page does not
name a licence. When one lands, link it from here and from `README.md`.
