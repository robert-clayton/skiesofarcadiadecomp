# Testing — how to check this thing works

Nothing here needs you to read the rest of the documentation. The sections go
cheapest first, and each one ends with what it does *not* cover, so you can
stop as soon as you have the answer you came for.

Every command below was run on the machine this was written on and the output
is quoted from that run, with one exception that says so where it occurs:
section 2's build transcript, whose line shapes come from
`tools/recompile.py` itself and whose counts come from the tree. That machine:
Windows 11, 16 logical CPUs, Python 3.14.0, VS 2022 Build Tools (MSVC
14.44.35207), the Metrowerks compilers in `vendor/`, and an extracted disc in
`extracted/`. Timings will differ; orders of magnitude should not.

| Question | Section | Wall time here |
|---|---|---|
| Did I break the tooling? | [1. No disc needed](#1-the-checks-that-need-no-disc) | 1 min |
| Does it build? | [2. The build](#2-the-build) | minutes |
| Is the runtime still sane? | [3. The self test](#3-the-self-test-75-cases) | 0.1 s |
| Does the game still run? | [4. The scenarios](#4-the-scenario-library) | 71 s to 26 min |
| Does it still draw the same pixels? | [5. The frame hashes](#5-the-frame-hash-corpus) | 18 s |
| Do the decompiled units still match? | [6. The match check](#6-the-decompilation-check) | 4 s |
| What can CI do for me? | [7. What needs what](#7-what-needs-what) | — |
| I am about to push | [8. Before you push](#8-before-you-push) | ~1 min |

---

## 1. The checks that need no disc

These are the whole of what CI can run, and between them they take about a
minute. Nothing here reads `extracted/`; every fixture is synthesised in
memory.

### `python -m pytest tools/tests -q`

```
1347 passed, 13 skipped in 697.13s
```

1360 tests in 86 files, none of which reads the disc. The two FMA probes of
`test_toolchain_fp.py` skip wherever no clang is found (set `SOA_CLANG_CL`), as in
a default run here, `test_mingw.py`'s archive test where no symbolic link can be made
(Windows without developer mode), `test_window_sdl.py` wherever there is no X display or
SDL build, and `test_seam.py`'s nine wherever there is no ELF system, as on every Windows run;
the counts below include those 13 skips, and this machine has the Android NDK that
`test_android_build.py` wants. CI's Windows runner ships LLVM, so the FMA probes run there, and
CI's Linux legs run the SDL window (gcc, under Xvfb), the seam and the Android build. They cover the Python
that builds the port and, through the tests that compile one `runtime/*.c` on
its own and run it, some of the C as well:

| File | Tests | What a failure means |
|---|---|---|
| `test_mods.py` | 132 | `runtime/mod.c`'s data patches, built alone against a fake DOL: every refusal (address spelling, hardware window, outside RAM, alignment, the DOL's code, values, triggers, conditions, `mod.ini` keys, the API and the DOL's SHA-1) refuses the whole mod with its file and line; manifest 2's id and version name a mod in the recording -- past the line's end too, by id and not folder -- a bad or duplicate id refuses it, an `x_` key is passed over; `every_frame`, `once` and `on_map_load` apply when they say; a `mod.dll` built here loads on `SoaModApi`, whose memory calls refuse what a patch would, its callbacks fire where they say, a DLL that refuses itself takes its callbacks with it, and the example in `examples/mods` builds and loads; `call_guest` runs at a safe point with every register put back and is refused anywhere else; and the shipped `mods/encounter-rate`, built with `--link`'s line: nothing written unset, each preset's byte from the base the game works out (no accessory, 210, 211 on a later character), only in the field, hold-B's zeros and its one restore of the game's value, hold-B off leaving the controller alone, an unknown preset refused, and the spec's two mutations (halving the byte read back, no restore) failing; and the shipped `mods/autotext`: one press 45 frames into a complete page, two frames down and two up, again only on the next page; none after a choice, on an auto-scroll page, in a choice box, in state 8, with no window, over the person's own A, or when off -- with its two mutations (no flags guard, the press-in-choice switch), and hold-to-skip (LB) in states 3 and 4 and nowhere else, with the live check's own rules each broken; `host_buttons` as si.c gives it, and a mod built against the header before it still loads; `read_pad` giving port 2 as si.c does and nothing for 1 or 3, and a mod built before it still loading; and the shipped `mods/coop` (P10b): pad 2 plays the slots given, only in a battle's party input, a handover neutral -- buttons, sticks and triggers -- until the incoming pad lets go, pad 2 forwarded whole, one line per press and per phase edge, port 1 alone when pad 2 is absent, off and refused values filtering nothing, each rule with a mutation; and the live check's own test (`python tools/tests/test_mods.py p10b <log> <recording>`) with each rule broken |
| `test_cardformat.py` | 109 | the memory-card formatter: does the image it writes say what the mount reads? And `.gci` import and export (P3): into the older slot with the next check code, the newer untouched, every refusal, disjoint chains, a round trip |
| `test_scenario.py` | 102 | the scenario files, the pad grammar and the invariant checker, against report lines copied from the `fprintf`s that produce them; a sweep refuses a replay the GPU drew, and a replay cannot see `SOA_GPU` or a `soa.ini` (V5); and `--wrap` (L1): its words go before the exe for `run` and `replay`, and a wrapped replay may not bless; and `--threads inline` (GPU spec V2), which runs with no worker pool and fails a run that does not say it used none |
| `test_guard.py` | 83 | the game-data guard (and, distribution R3, `--tree` over a player's package: a clean package passes with its third-party binaries over the size limit; `src/` and `include/` are refused by name, and a DOL by its header -- named innocently, inside a file on a 32-byte step, and in a third party's folder; size, folder names, suffixes and mod-folder text still hold outside those folders; random bytes and text hold no DOL): its suffix and size limits against CI's copy, CI's grep refusing the same names as the guard, a suffix anywhere in a name (`slotA.raw.bak`), the tree check on names git would quote, and `--history` -- a file deleted later, a file renamed through a forbidden name, an exemption keyed by content, and the content check over deleted blobs; and T0: what mods, packs and saves would carry, the pack, load, dump, blob, photo and out folders, a binary file that begins as game data whatever it is named (a card image by its directory, an untagged MP3 by its first frame), and in a mod folder a file that is not text, NULs included |
| `test_cfg.py` | 40 | control-flow recovery over synthetic DOLs: function boundaries and switch tables |
| `test_soak.py` | 41 | `tools/soak.py`: the generated play repeats by seed and fits `si.c`; `check` turns a soak log into pass, FAIL or "did not test what it says" and each injected fault fails through its own check; the warp and the encounter accelerator never poke the forced-battle flag, a field that comes back black after a battle is a question, and (GPU spec V7) gxv's pipelines are counted by where they were made, the longest of each and how many came after the landing map loaded |
| `test_padrec.py` | 33 | recording controller input and replaying it byte for byte, and the `SOA_PAD` items `si.c` refuses rather than pressing nothing; the host buttons (`lb`, `view`, `ls`, `rs`) and their chords, which never reach the report, fire once a frame, and are comments in a recording; and a card under the port root named relative to it; `SOA_PAD2`, port 2 for mods, in the same grammar, apart from port 1, and never answering the game; 600 bytes of recorded settings and mods reach the `# config` line whole, and 800 are cut with a line that says so |
| `test_decode.py` | 32 | the Gekko decoder, on encodings hand-derived from the 750CL manual |
| `test_settings.py` | 28 | `runtime/settings.c`, built alone: each `soa.ini` key sets its switch and `disc` names the directory, the environment wins and says so, an unknown key is named with its line, `SOA_SETTINGS=0` turns the file off, every scripted check sets it, and a setting that changes the game is recorded only when set -- as its owner says it is in effect, never half a value when the line is full; a setting a mod reads says it does nothing without that mod and is then not recorded, and one of a fixed set of values is recorded only as one of them; and M5b's root: relative paths in soa.ini under the port root, its defaults only with a file, the root found beside `runtime` from `gen` and `gen\clang`, and the console handed to a log only when it is the run's own; `coop` recorded only as party slots mods/coop takes; and README's list of soa.ini keys holding every key settings.c reads |
| `test_emit.py` | 22 | the emitter; the last cases compile the emitted C with MSVC and run it |
| `test_gxr_overlap.py` | 23 | the ordering around EFB copies (H14): with `SOA_GXR_STALL` holding one worker back before its draws, copies or clears, every thread count leaves the copied memory, screen, EFB, decoded textures and what the CPU reads after `GXDrawDone` that the one-worker run leaves, over frames that differ; the copies were fenced and not drained, each producer read (texture, palette, vertex array) waited for its own copy, the frame gate drained once a frame, and `SOA_GXR_DRAIN=1` and `SOA_GXR_TOKENWAIT=1` hold too. Nine deliberate breakages of the fences, waits and gate each turn it red. Copy images: a draw sampling a copy's own texture takes the image the workers decoded, held to the drains' decode from memory, through an overwritten image, an unfiltered copy, a drain then a CPU write, a hook's write and a token between copy and draw, with the producer's image counts pinned per protocol; seven more breakages each turn it red; and (GPU spec V6b) the same stream with the GPU as the consumer, unstalled, stalled and token-waited, leaves every hash its synchronous run leaves, the untextured copies the CPU's, and late-readback fails it (MSVC, `vendor/`, a Vulkan device) |
| `test_dump.py` | 20 | whether the tree notices a dump that is not the build `config/` describes |
| `test_crossval_capstone.py` | 19 | our decoder against capstone's PowerPC backend — **needs `capstone`, which CI does not install** |
| `test_profile.py` | 19 | `tools/profile.py` against the report the port prints, and the wording of those lines as an interface to `runtime/` |
| `test_fifo_summary.py` | 5 | `tools/fifo.py --summary` (specs/gpu-backend.md V1) on a synthetic stream: one draw under a logic OR, a display list of 0x40 bytes, and an R8 and an RGB565 copy, each with its destination and size; a blend overrides the logic op, as GX and the renderer have it; and intensity copies are named as such |
| `test_gxv_live.py` | 10 | a run with `SOA_GPU=vulkan` judged on its own log (GPU spec V5): `scenario.gpu_problems` passes a run the GPU drew whole, and fails a fallback, each count off by one against what the renderer sent or counted, a run that drew nothing, two start lines, and a backend that is not vulkan; it reads the last report, not the watchdog's; and the fifth invariant is there only with the GPU. A real log is checked with `scenario.py check <log>`, which applies it to any log the backend wrote in |
| `test_gxv_copyimage.py` | 3 | a copy sampled in its own frame (GPU spec V7), through `gpuspike.py copyimage`: sixteen cells copied to an RGBA8 texture and sampled in the same frame give the CPU's hash and all sixteen cells on the GPU, one sampler served from the copy's image in the GPU's pool and no readback wait of its own; `--mutate cimg-cpu` (the producer's image sampled before it lands) and `--mutate land-at-copy` (V6's wait at every copy) each fail it. They skip without MSVC, `vendor/` or a Vulkan device |
| `test_gxv_logicop.py` | 1 | logic ops without `logicOp` (GPU spec V10), through `gpuspike.py logictest`: 0x55 ORed into 0xAA gives 0xFF from the CPU, native, a snapshot, the interlock and `SOA_GPU_FEATURES=core`'s route, and 198 from the forced blend; two overlapping triangles XORed onto black come back black in the overlap from the CPU and native, and with `SOA_GPU_FEATURES=nologicop` the draw is routed to the interlock and drawn as native draws it, while `core` routes it to a snapshot, named in a line, never to a blend. It skips without MSVC, `vendor/` or a Vulkan device |
| `test_package.py` | 4 | `tools/package.py` (distribution R3): the licence texts it copies come from the folders `guard.py --tree` allows to hold third parties, it never copies `src/`, `include/` or build output, its downloads are pinned by exact version and hash, and a stage refuses a folder that already holds something |
| `test_setup.py` | 11 | the setup window (distribution R4), `Setup.exe --check` as `package.py` builds it: a whole package in a plain folder passes and keeps nothing; Setup.exe opened alone (from inside the zip) and each package file missing, a path with a non-ASCII letter, a root one character too long for the deepest file, both Program Files folders (the short name too, not a name that only starts the same) and a folder denied writes are each refused in words; `package.deepest` measures a tree; formats but ISO, GCM and RVZ name Dolphin's Convert File; the exit codes are `setup.c`'s; one source builds one file, holding its manifest; python and the game are started with `CREATE_NO_WINDOW` (skipped off Windows and without `vendor/llvm-mingw`) |
| `test_player_build.py` | 13 | the player's build (distribution R2): the 12 bindings `decomp_swap.c` answers are the ones `hle.txt` notes as decompiled, an adapter added without its note is seen, `--no-decomp`'s link builds no native unit and defines `SOA_NO_DECOMP`, and the runtime keeps that branch; `player_build.py` refuses a disc of another game by name, an executable whose SHA-1 differs, an image that is not a disc and a config it cannot read, each exit 2 and a `[build] refused:` line; a changed input retranslates and an unchanged one relinks, and without the check a changed `cpu.h` would only relink; Setup's Rebuild (`--rebuild`, R4) translates again when nothing moved; a synthetic RVZ reads back as its disc, and under the embedded CPython, which fails it with `_zstd.pyd` taken out (skipped without `vendor/`'s zip) |
| `test_mingw.py` | 7 | `soa.exe` with no Microsoft compiler (distribution R1): `fetch_mingw.py` keeps the x86-64 target alone, its `--verify` finds a byte changed and a file missing, and the Linux archive unpacks files and links and skips the rest (where the OS makes links); the `mingw` profile's flags, libraries and stack are the spec's, a `SOA_MINGW` naming no compiler finds none, and its plan writes only under `gen/mingw`, each `mod.dll` under `gen/mingw/mods`; and, with `vendor/llvm-mingw` and the disc's executable, the whole build runs with `msvc_env` made to raise and the exe imports no `fma`, `exp2f` or `log2f` |
| `test_gxv_present.py` | 5 | the window's picture from the GPU (GPU spec V8), through `gpuspike.py present`: two synthetic screen copies through the presenter's pass into eight target sizes at both layouts give `picture_scale`'s picture in every pixel's colour, 32 of 32, and `--mutate present` (one column over) gives 0 of 32; and (V8b) `present --filters`, P5a's filters on the GPU, at scale 1 and 3: eight filter sets and a two-frame flash-limiter blend give `picture_filter`'s, `picture_blend`'s and the scaler's bytes in 24 of 24 cases, and one colour coefficient changed (`--mutate filter`) 12 of 24. They skip without MSVC, `vendor/` or a Vulkan device |
| `test_gxv_scale.py` | 6 | the EFB at three times the console's size (GPU spec V9a): `copydiff --scale 3`, on an EFB whose samples are alike within each pixel, gives the CPU's bytes, image and screen, with the pool's image and the full screen the native ones replicated, and `--mutate taps` (the copy filter's taps one sample apart) fails it; `copyimage --scale 3` gives the CPU's frame from a copy sampled at scale, and `--mutate copy-scale` (the scaled image sampled as if native) fails it; `present --scale 3` gives `picture_scale_area`'s picture, 32 of 32, and `--mutate present` 0 of 32. The oracle at scale is `gpuspike.py oracle --scale 3`, too long for here. They skip without MSVC, `vendor/` or a Vulkan device |
| `test_gxv_queue.py` | 6 | the GPU backend on its own thread (GPU spec V6a), and (V7) the pipeline cache on disk: a second run of the queue frame finds every pipeline the first made, and a file of junk none; the compiler thread accounts for every pipeline specialised on the TEV's shape, with every compile stalled 200 ms the frame is unchanged and its consumer under the stall, `--mutate compile-wait` fails that, and `SOA_GPU_SPECIALIZE` `wait` and `0` report as they should; through `gpuspike.py queue`: a synthetic frame of 64 quads each sampling one texture slot at a new generation and 656,000 vertices that fill the vertex arena twice gives the CPU's frame hash, every quad its own texture and two arena drains, three times on the thread, inline and with the consumer stalled; and the pool-in-place and count-early mutations each fail it. MSVC, `vendor/` and a Vulkan device, skipped saying which without |
| `test_gxr_backend.py` | 5 | the renderer's backend seam (GPU spec V2), on a renderer-only build with a counting backend: the commands arrive as 0 0 1 2 1 2 0 1 2 for two draws, a filtered and an unfiltered copy to texture and a screen copy, each copy's clear its own command; a texture's generation moves when its bytes change and not otherwise; the frame and presented counts agree; each command carries the EFB it was built for; and the 23 corpus captures through the passthrough keep their manifest hashes (skipped, with its reason, without `build/fifo`). Each of the spec's five mutations turns it red |
| `test_gpuspike.py` | 36 | the GPU spike (specs/gpu-backend.md V3a, V3b, V4a, V4b, V5, V7): the judge passes a pixel moved at an edge and fails a hole or a one-step colour inside, takes in colour edges, holds `cull3` to nothing, lines to one pixel and points to none; copydiff's comparison counts each kind of difference and refuses a refused copy that wrote, runs whose cases differ, a short run and an empty one; a replay sees no `SOA_*` but `SOA_SETTINGS=0`, and one pixel of a frame does not count as a mutation applying; the shader list tools/soa/shaders.py builds is exactly what runtime/gxv.c includes and each stub declares its array, `fetch_gpu.py --headers` records the headers alone and wants no glslang where a full fetch refuses, gxv.c compiles as the backend against vendor/'s headers and fails against an empty vulkan_core.h (MSVC), `live`'s comparison passes identical snapshots, fails a frame painted over, judges only the frames two CPU runs reproduced, and calls a missing frame or an empty folder a problem, the Python copy_texfmt is gxr.c's, a copy's runs and texels skip a stride's gaps, poison covers only what a copy writes, and efb_at cuts the stream before the copy; the driver's recipe is still `runtime/selftest.c`'s and every scene it writes is judged; `vendor/` matches its record, and a copy with one header byte changed and `LICENSE.md` gone fails it; and on this machine's GPU the 17 scenes pass (the invariance strip and the logic ops among them) and `--mutate unclipped` fails, tevdiff's 100,000 cases and copydiff's 25,600 copies have no mismatch, the clamp, rounding, intensity and unseeded mutations each fail as they should, loddiff holds the level of detail (100,000 cases bit for bit, and the formula within 1/1024) with its lod and lodmin mutations red, the 35 captures of the corpus and the benchmark set pass V0 (two by design, listed) against references equal to the manifest's and V0's, the copy captures poisoned, the mask effect's 14 and V1's two that draw logic ops give byte-identical frames under every logic-op path (native, blend, snapshot and, V10, the interlock where the device has it), and (V7) the 35 give byte-identical frames with every pipeline specialised on the TEV's shape and with the interpreter alone, `--mutate spec-stages` failing that. The vendor tests skip, saying why, without `vendor/`; the GPU ones also without MSVC or a Vulkan device, and the capture ones without the captures |
| `test_imgdiff.py` | 17 | the frame oracle (specs/gpu-backend.md V0) on synthetic frames: identity passes; +-1 noise and 0.3% scattered pixels pass; a black block, +4 brightness and a channel swap fail; a frame drawn a pixel over fails the shift test and a second vertical filter the blur test, while noise explains nothing; a block is a blob and a one-pixel line is not; the screen hash is `gxr_screen_hash`'s, worked by hand, and one pixel moves it; and the replay that makes a reference runs on a scratch copy and sees no `SOA_*` but the tool's own, even with `SOA_GPU` set; and (V9a) `--sample`: the centre sample of each 3x3 block is its middle, the box of a 2x2 its mean rounded as the copy shader rounds it, an even scale has no centre, and the largest step between two pictures ignores alpha |
| `test_midpoint.py` | 18 | `tools/midpoint.py` on canned output: the `[pair]` and hash lines parse, each of the seven verdicts fails when its one thing breaks, a mutation that costs no pair is not a pass, and a capture that drifted from the manifest is refused before anything runs |
| `test_fifopair.py` | 17 | the H4 pair analyser, on captures built byte by byte: an identical pair matches all its area, a changed texture unmatches its draw, a moved draw lands in the displacement histogram, list and direct draws are counted apart, and the area estimate clips and culls as the renderer does |
| `test_disasm.py` | 17 | `tools/disasm.py`'s address notes: an update form moves its base, `ori` reads rD and writes rA, and rA=0 is the number zero |
| `test_uncap.py` | 17 | `SOA_UNCAP=N` and `SOA_FRAMETIME_FROM=N` are read at startup and refuse a value that is not a frame; the `[frametime]` percentiles tell a hitch from a steady run, and an uncap restarts the record at its frame |
| `test_portability.py` | 4 | where `runtime/` may say `_WIN32` (portability L9): every `#if`/`#ifdef`/`#ifndef`/`#elif` naming it is found and named by its function, comments and strings blanked first, and held to a written list -- the window, waveOut, the fibers, the profilers, the GPU's Win32 surface and loader name, the Windows console, the mods' export macro and the renderer-only build's mkdir pairs; a listed site that has gone fails too; an `#ifdef _WIN32` added to another function of exi.c is caught |
| `test_mod_library.py` | 6 | a mod's native library on every system (portability L9), built with the profile `SOA_CC` names (CI's Linux legs: gcc and clang): examples/mods/map-log as `mod.dll` or `mod.so` loads through the real loader and the recording names it `map-log@1.0`; one exporting no `soa_mod_init` is refused in the loader's words (dlerror's); and (L10) each of the three shipped mods builds as this system's library, loads, and reads its switch from the environment, which every shipped mod must have listed. On Linux, autotext as it was before L10 (`windows.h`) fails to build, and with its `getenv` finding nothing fails its switch |
| `test_audio_wav.py` | 1 | `SOA_WAV` on every system (portability L9), with `SOA_CC`'s profile: a driver pushes seven blocks to `audio_out.c` with no device, and the WAV's header holds 7 x 640 bytes, the samples left then right, and the report says so; with the writer back inside `#ifdef _WIN32` it fails on Linux |
| `test_fetch_sdl.py` | 8 | SDL3 for the window and sound off Windows (portability L10): an archive that is not the pinned one is refused and nothing unpacked, and the pinned one is unpacked once and its headers found; `--verify` finds a changed byte and a missing file of this host's build, and with none recorded says how to make one; the build config's drivers are read without their features; what a static `libSDL3.a` needs comes from `sdl3.pc`, either way SDL writes it; with a build in `vendor/sdl3` the Linux link compiles every runtime file with `SOA_SDL` and links the library after the objects and before the profile's libraries, and without one the link is as it was; on Windows the build is refused (D2) |
| `test_seam.py` | 9 | the seam between the runtime and a split game library (specs/android.md L12a), with `SOA_CC`'s profile (CI's Linux legs: gcc, clang, ARM64): a game of two functions, built from the real `game_table.c` template, loads through the real launcher, `runtime/game.c` and `runtime/elfcheck.c` -- its entry calls the runtime, the table's record is the one written into it -- and exports `soa_game` alone; a library is refused before dlopen, in the player's words, for an import the runtime lacks, another build's record, the decompiled code's record, no table, 4 KB pages and no record; with a real split build in `gen/linux-split`, its libraries cross exactly `config/seam.txt` (CI leaves that case out). With `elfcheck.c`'s import check or its record comparison removed, the matching cases fail. Skips without an ELF system, as on Windows |
| `test_android_build.py` | 9 | the game library from the PC for Android (specs/android.md L12b), with the NDK `tools/soa/toolchain.py` finds: a mistyped `SOA_ANDROID_NDK` is no NDK, never the next one, and the places looked are named; a game of two functions, built as `recompile.py --cc android-arm64` builds the real one, is what the phone loads by `tools/soa/elfcheck.py` (AArch64, `libsoa_game.so`, 16 KB pages, needing `libsoa_runtime.so`, `soa_game` alone, the record written into it), and two builds are the same bytes; a library with 4 KB pages, its functions not hidden, an import the runtime lacks, another build's record, or built for x86-64 draws its refusal; and `runtime/elfcheck.c`, built for this machine with `SOA_CC`'s profile, gives every one the Python's verdict in the same words. With `elfcheck.py`'s page limit at 4 KB, or `elfcheck.c`'s wording changed, it fails. Skips without an NDK; the C half without a compiler for this machine |
| `test_window_sdl.py` | 1 | the SDL3 window and sound run for real (L10), built with `SOA_CC`'s profile against `vendor/sdl3` with a driver standing in for the renderer, on an X display (CI's gcc leg: Xvfb): the frame read back with xwd is the driver's, whole pixels and its colours in order; a key typed with xdotool is port 1's A, and let go nothing; `SOA_WINDOW_TEST`'s size gives a 1000x700 client with the frame at 1x, centred, in window.c's words; the presents are counted, none failed; 200 blocks of sound at once keep waveOut's 24 and drop the rest. Red and blue swapped, the key sent as B, the queue a hundred times deeper and the resize not asked each fail it. Skips without gcc or clang, SDL, a display, xwd or xdotool, as everywhere on Windows |
| `test_disc_check.py` | 5 | `tools/citest/disc_check.py` (disc-layer I1): its `--log` half on a synthetic image, no compiler -- each file's own reads pass, and a read naming the neighbouring file, one naming no file, a wrong count past a file's end, and a log with no reads are each refused; its fixture half builds `runtime/disc.c` (with the fixture's system files built in, and without, I3, and reading a store, I5) and passes, and the mutation that moves the image's file table four bytes fails it (that one skipped without MSVC) |
| `test_recompile_inputs.py` | 6 | what `recompile.py` builds into `soa.exe` (disc-layer I3), no compiler and no DOL: the build-input record passes the same executable and refuses one a digit apart, and a folder with none passes once with a note; `tools/soa/embed.py`'s words read back to the fixture's very bytes, little-endian and padded, under the marker; `--no-embed`'s file has the same symbols and nothing in them; the system files come from the disc and are refused when its executable is not `config/`'s or `--dol`'s, unless forced; and both links compile `disc_sys.c` |
| `test_disc_const.py` | 4 | `runtime/disc.c`'s `DISC_DOL_SHA1` is `config/GEAE8P/config.yml`'s hash and its `DISC_GAME_ID` the config directory's name; one hex digit changed, or another id, is refused by the same checker; both sit behind `#ifndef` for the fixture build |
| `test_store.py` | 17 | the disc store (disc-layer I4), `tools/soa/store.py` and `extract.py --store`, on a synthetic image whose tail runs past the store's mebibyte: an ISO and an RVZ import and read back as the image below the covered end and zeros above, every block digest hashlib's; a flipped payload byte names its file and fails exactly one block and one extent; a truncated store, an unfinished `.part` (and a stale one the importer deletes), a changed header, another game id and an executable that is not `config/`'s are refused; files not the pinned ones are refused unless forced, and a padding-only difference is accepted with a warning; `--compare` finds 0 bytes and exactly the flipped one; `--sys-only` writes the four system files; GCZ, WIA, CISO, WBFS and NKit are named; a table whose files overlap or run past the image is refused; the committed `disc.yml` reads in its form and a mistyped pin is refused; a folder holding a store is read through it |
| `test_extract.py` | 17 | I2, on `tools/soa/discfixture.py`'s synthetic image (a test game id, no game bytes): the fixture holds every file where its FST says, each AKLZ file decodes, and each layout feature is there; `extract.py` writes `disc.iso` and the four `sys/` files, each equal to its slice, and no loose file, `--files` writes every file equal to its slice, `--iso` is accepted; `--prune-loose` deletes exactly the loose files equal to the image and keeps and names the ones altered (a byte flipped, a byte short), `--dry-run` deletes nothing; `sct.py`, `validate_assets.py` and `audio_check.py` each read a file present only in the image, and a container broken there fails. The `audio_check.py` case needs numpy, which CI does not install. (Count to be regenerated.) |
| `test_poke.py` | 16 | SOA_POKE: a malformed switch is refused out loud rather than driving a run that looks like it ignored you |
| `test_decomp.py` | 15 | the `dc_*` rename scanner, on declarations that look like functions and are not; and the one `units.txt` reader, which refuses a row it cannot read |
| `test_bindings.py` | 15 | the binding lists (`hle.txt`, `hooks.txt`, `savepoints.txt`, `trace.txt`): a line that is not an entry, or a repeated address, is an error naming its file and line |
| `test_gxr_pair.py` | 15 | H10's in-between image, on the renderer built alone: synthetic frames captured through the real capture path and replayed as pairs. A triangle moved by 2d lands at d pixel for pixel; perspective and depth motion give the view-space midpoint; t=0 and t=1 give the two frames; an unmatched draw comes from F+1; copies to texture are skipped with their clears kept; equal keys pair in stream order; the pairs written are fifopair's; and `gxr_flush` never touches the pair state |
| `test_snapshot.py` | 11 | save states (docs/SNAPSHOT.md): every data and bss symbol of every runtime object, a host build's, has a line in `config/snapshot_state.txt` saying what a snapshot does with it, and each `saved` one is in its file's `*_snap` (a line removed, a name misspelt and a variable dropped from its `*_snap` each fail it); `runtime/snapshot.c` with stand-in sections, a snapshot written by one process and resumed by another: every byte, the registers, a heap buffer, a host's own section and a function and its data found again by module and offset, main entered at the resume point; a flipped byte, a cut file, another magic and another format refused by name, a section longer or shorter than this build's or of another configuration refused, an event with heap data refused before the start, and nothing left under the name when the file cannot be written; the emitter's second entry at a resume point, before the swap guard, run with gcc from that block with the registers it has; the game record's `resume=`; and the runtime and `config/resumepoints.txt` agreeing on the one point |
| `test_seed.py` | 15 | `runtime/seed.c`, built alone: the race seed's values are MurmurHash3's finaliser over the seed, the site and the site's call count (against an independent Python one), exactly the three OSGetTick sites are pinned, each counts its own calls, a seed that is not 32 bits is refused and the seed in effect is handed back in decimal; each pin prints its line, and the module's own log check (`python tools/tests/test_seed.py <log>`) fails an edited value, a missing pin, a wrong count, a wrong seed and a run that never left a battle; and each site is in the translated code once, followed by srand |
| `test_aklz.py` | 14 | the AKLZ container decoder, on hand-built streams |
| `test_formats.py` | 14 | the disc's format parsers, on synthesised fixtures |
| `test_gxr_tripwires.py` | 14 | each unmodelled renderer feature warns exactly once, and what the game really programs stays silent |
| `test_matchcheck_relocs.py` | 14 | what a relocated word is allowed to hide — every case is a thing the old oracle called a MATCH |
| `test_peek.py` | 14 | `SOA_PEEK` refuses a malformed item out loud and keeps its own list; a watch aimed with `SOA_WATCH_FROM` prints only from that frame, and every watch line carries its frame (against the real `trace.c`) |
| `test_gx_dlrecord.py` | 13 | display lists recorded into guest memory and drawn at their call (C5b), on the renderer built alone with the SDK's Begin/End/Call played by hand: a list recorded, drawn over, then called lands on top; a CPU FIFO that is not DisplayListFifo (the logo screen's) is drawn at once; only what a list holds at its call is drawn, and an uncalled one never; an overflowing list is empty; each capture holds its lists inline, once, and replays to the live picture; `fifo.py` and `fifopair` read the inline list. Five hand mutations each turn it red (FINDINGS "C5b") |
| `test_regs.py` | 12 | which GPR an instruction actually writes |
| `test_fifo_verts.py` | 11 | vertex-attribute dumping, on streams built byte by byte |
| `test_profiler.py` | 11 | the sampler in `runtime/main.c`, built and run with no game and no disc |
| `test_toolchain_inputs.py` | 11 | what a fresh checkout can check and with which compiler; every `src/**/*.c` is in `units.txt` |
| `test_gxr_copy_filter.py` | 10 | what the EFB copy's vertical filter does to a pixel, including that the SDK's filter-off weights are the exact identity, and that `SOA_DEFLICKER=0` (P5b) makes the screen copy the identity and leaves texture copies filtered |
| `test_matchcheck.py` | 9 | how an object's symbol is matched to a function in the executable |
| `test_symbols.py` | 9 | the symbol database |
| `test_picture.py` | 11 | `runtime/picture.c`, built alone (H19a): `picture_layout` gives the spec's rectangles at 1920x1080, 1280x800 and 2560x1600 in both modes and agrees with a Python twin over a grid of clients, always inside the client; `present_interval` holds 60, 85, 120 and 144 Hz to 2, 1, 4 and 1, and at turbo's 60 images a second (M11a) to 1, 1, 2 and 1; both mutations (width and height swapped, a plain round) fail, the plain round at 60 fps on 144 Hz alone; the twin's log check passes a good windowed run and fails each rule broken; and P5a's filters: the matrices equal to 3.13's text to six places in picture.c and the twin, each colour-blind model within a step of a Python twin of the spec's formulas over 91 colours, grey to grey, gamma 1.0 no change and 2.2 its table, a bad key refused by name with the others left on; the flash limiter, fed whole frames and every pixel counted independently, keeping the spec's 5 Hz, faster flashes, a stepped flash, swapping halves, a flash over a gradient, an overlay pulse over a busy picture, a band sweep and a small flash under a quarter of the picture, and leaving 2 Hz, a fifth of the frame, a small step and bright changes untouched; `picture_scale` where the layout says; the replay identity check's PNG decoder under all five row filters; and eight mutations each failing; and (V9a) `picture_scale_area`, the GPU presenter's averaging of a picture drawn at scale, held to a twin of its definition by exact fractions -- shrinking on both sides, on one and on neither -- with the mean truncated and a column left out each failing it |
| `test_decomp_native.py` | 8 | what it takes for a unit to run natively, checked by building it |
| `test_card.py` | 7 | the parts of Track B that are text: `exi.c`, `selftest.c`, `irq.c`, `names.txt` and the README agreeing |
| `test_gxr_texcache.py` | 7 | the texture cache (H12), with `gxr_tev.c` included whole to reach its statics: the index stays whole through 30,000 lookups over three times its keys, the least recently used texture goes first, a texture is hashed once an epoch and again after BP 0x66 or a copy moves it, `SOA_TEXVERIFY` catches a rewrite inside one, a dropped decode is rebuilt in place, and a palette load keeps a decode whose palette came back the same and makes it again when it did not |
| `test_toolchain_profiles.py` | 20 | the toolchain profiles (portability L3a), no compiler run: every profile's flags, strict set, linker flags and directory are 3.9's table, copied; the msvc profile's --compile, decompiled-unit, link and mod command lines, and the whole --link plan with the objects it links, equal a golden copy of what recompile.py ran before profiles; a clang-cl build's --compile and --link write and read only under gen/clang, with clang-cl's flags on every line and `-fuse-ld=link` after the objects, and build no mod; `--cc clang-cl --out gen` is refused, in any spelling, and main() takes its directory from that rule; --compile's level is /Od or -O0; a `SOA_CLANG_CL` naming no file finds no compiler; the gnu grammar's translation, a POSIX path that begins like a flag passing through; compile_runtime.py's strict set. The review's 22 mutations each fail it (FINDINGS "L3a's review"); and cl's `/Fo<dir>/` over several sources becomes one gcc command per source, each object named for its source (L4b); and `runtime_support_sources()` is `runtime/plat.c` (L7); and --link adds the GPU backend's define and include paths only when asked (V5) |
| `test_libm_check.py` | 7 | `tools/citest/libm_check.py`'s arithmetic (portability L6), no compiler run: the 64 slices cover each domain once, in order; the domains are 2.5's counts; exact values round to themselves; rounding to a float goes to the nearer one and a tie to the even mantissa, decided against the decimal, not through a double; at `x = -0.029743773862719536` the double-precision `exp2` rounds the wrong way and decimal does not; the pinned file names both functions over their domains; and FNV-1a is the driver's |
| `test_ax_census.py` | 6 | the audio census lines a run prints, and the invariants between them |
| `test_gxr_queue.py` | 6 | the handshake between `gxr_flush` and the rasterizer threads |
| `test_tick.py` | 6 | `runtime/tick.c`'s native `VIGetRetraceCount`, built alone: the original everywhere but the main loop's two call sites; the top of the loop runs the safe-point callbacks in order, and the frame end's spin answers start + 1 from the unlock frame on |
| `test_gxr_lifetimes.py` | 6 | the lifetime rules the renderer's queue lives by — the texture use-after-free of 2026-09-17 — under H14's fences and again under `SOA_GXR_DRAIN=1`, where a draw's setup still drains for a queued copy |
| `test_clock.py` | 6 | `runtime/clock.c`, built alone and fed synthetic host times (M19): steady steps are guest time, a 10 s gap counts as none and bumps the epoch (10 s with the rule off), a host step backwards moves nothing, a speed change is continuous and bumps the epoch, a pause is excluded, and a peek writes nothing |
| `test_citest.py` | 6 | the CI scripts' own claims: nothing fell out of coverage, the render driver has not drifted from `selftest.c`, the import graph is stdlib-only, and so is `player_build.py`'s, which a package runs on the embeddable CPython (distribution 3.5) |
| `test_gxr_atomics.py` | 5 | the render queue's rule 4 (portability 3.4, L2), text only: every use of a shared counter in `gxr.c` is an argument of a `plat_*` helper, its declaration, or the producer's plain read of its own `g_published` on a line marked `own count`; a bare `g_ran[1]` read added to `worker()` (the spec's mutation), a marked worker count, a cast and a condition each fail it, and the code before L2 fails it 38 times |
| `test_sct.py` | 5 | `tools/sct.py`, the field-script disassembler, on bytecode built word by word: a flag test, a backward jump, a warp name, a switch, and an entry that runs off its end |
| `test_inventory.py` | 5 | regenerating the inventory leaves both symbol files saying the same thing |
| `test_dspadpcm.py` | 4 | DSP-ADPCM decoding against hand-computed frames |
| `test_hle_pc.py` | 4 | every native adapter says which guest function it is, so the profile does not charge it to its caller |
| `test_memguard.py` | 4 | the bound on the guest memory image, and the boot path built and poked past the RAM; profile-aware through `SOA_CC`, which CI's Linux legs set, where the guard is `runtime/plat.c`'s SIGSEGV handler (L7) |
| `test_rvz_junk.py` | 4 | the junk generator behind RVZ junk runs |
| `test_perfbench.py` | 4 | the renderer benchmark: its figure is busy thread-time over every fragment processed, a capture that drifted from the pinned manifest is caught, and `--exe` (portability L2a) benchmarks a saved build, a relative path taken from where it was typed and a missing one refused before any replay |
| `test_gxr_fastpath.py` | 3 | the pixel path's specialised cases (H15c) against the general path: 4,000 random register sets through the real `tev_prepare`, near misses included, 64 random pixels each through both TEV paths, and 400,000 random blends through both blend cases -- colour and alpha test identical; and 600,000 random bilinear samples through the SSE4.1 blend and the scalar loop, byte for byte (H15d). `SOA_CC=clang-cl` builds it with that profile, which passes with the NDK's clang-cl since L2a |
| `test_gxr_alpha.py` | 2 | the early depth test's premise (H15a): whether a draw's alpha compare passes every alpha, on sixteen combinations worked out by hand -- the XOR of two always-true compares among them -- and the answer's cache between draws |
| `test_turbo.py` | 2 | the check a turbo run is held to (M11a), `python tools/tests/test_turbo.py <log>`: over the battle the game's frame counter advances one a retrace, over the field before it one per two, the battle ends inside the run, and the audio reached the device at 128,000 bytes a second within 3%; a synthetic log with each rule broken fails its own line |
| `test_toolchain_fp.py` | 2 | no fused multiply-add from a clang profile (portability 2.8, L3a): a*b+c built with -mfma disassembles as vmulss and vaddss, and without -ffp-contract=off (the mutation) as vfmadd213ss; skips, saying so, where no clang and llvm-objdump are found, as in a default run here; CI's Windows runner ships LLVM, so the Tests job there runs both |

Anything that needs a C compiler or an optional package skips itself rather
than failing, so the number you see depends on what is installed. Measured on
this machine on 2026-10-05 by hiding one at a time, with a pytest plugin that
makes `toolchain.msvc_env` answer None or `import capstone` fail (FINDINGS
"L3a's review"); there is no clang here, so every row has the two FMA skips:

| Installed | Result |
|---|---|
| everything (MSVC + capstone) | `1347 passed, 13 skipped` |
| no capstone | `1328 passed, 14 skipped` |
| no MSVC | `939 passed, 421 skipped` |
| neither | `920 passed, 422 skipped` |

No row is a CI leg. CI installs no capstone, its Windows runner ships LLVM,
and a few tests are Windows-only, so read CI's counts from CI: at 4441a80
the Windows Tests job printed `1102 passed, 4 skipped` and the Ubuntu one
`723 passed, 383 skipped` (`gh run view <id> --log | grep passed`).

Two things follow. The 408 compiler-gated tests are the ones that build runtime
files, or the GPU spike, and run them — the renderer's queue and lifetimes, the tripwires, the memory
guard, the pad recorder, the profiler, the native-twin build — so on Linux the
Python is checked and the C is not. And CI's install line is `pytest` and
`ruff` only, **not** `pip install -e .[dev]`, so `capstone` is absent and the 19
cross-validation tests skip in CI on both legs: our decoder is checked against
an independent disassembler only on a developer machine.

One test — `test_image_every_word_agrees` in `test_crossval_capstone.py` — also
skips without `extracted/sys/main.dol`. It is the only test in the tree that
wants the disc.

### `python -m ruff check tools` and `python -m ruff format --check tools`

```
All checks passed!
```
```
70 files already formatted
```

Two separate commands. **Run both.** The format check has broken CI twice, and
it is the one people forget, because `ruff check` passing feels like it covered
formatting. It did not. A failure looks like this — the tool prints the diff it
would apply and exits 1:

```
unformatted: File would be reformatted
   --> tools\soa\dump.py:162:8
    |
161 | def describe_disc(root: Path) -> str:
    -     """"GEAE8P rev 0" from the extracted disc header, or "" if it is not
162 +     """ "GEAE8P rev 0" from the extracted disc header, or "" if it is not
...
1 file would be reformatted, 65 files already formatted
```

`python -m ruff format tools` fixes it. CI pins `ruff==0.16.7`; a different
local version can disagree about a line nobody touched.

### `python tools/guard.py`

```
guard: 161 tracked files, no game data
```

Refuses game data in the tree: 49 forbidden extensions, wherever they sit in a
name (`.rvz`, `.iso`, `.dol`, `.tpl`, `.dsp`, `.bin`, `.map`, `.gci`, … — so
`slotA.raw.bak` too; `.bin` is deliberate, the only `.bin` files
in this project's world are `boot.bin`, `bi2.bin` and `fst.bin`), eighteen
directory names that must never be tracked (`extracted/`, `gen/`, `build/`,
`vendor/`, `scratch/`, `packs/`, `photos/`, …), any tracked file over 2 MiB, a binary
file that begins as game data (AKLZ, GVR, a `.gci` or disc header, a GCS or SAV
card save, PNG, DDS, Ogg, FLAC, MP3 with or without a tag, WAV, or a memory card
image whose directory holds this game's save) whatever it is named, and, in a
folder holding a `mod.ini`, any file that is not text (T0: a mod here is its
edits, never the game's data). A capture's `.fifo` and `.regs` have no header,
so renamed they pass: only their names keep them out. It lists `git
ls-files`, so it sees what would actually be committed rather than what is
lying around. CI runs the same script, then scans **every blob in the whole
history** for the same suffixes, because a file deleted in a later commit is
still in the pack, and `guard.py --history` applies the directory names and
the content check to every path and blob any commit added — in a folder that
held a `mod.ini` at any point, the text rule too.

### The three MSVC checks — no disc, no `gen/`

Each takes `--cc clang-cl` (portability L3a) to build with the clang-cl profile
instead, into its own `build/citest/<check>-clang-cl`; `SOA_CLANG_CL` names the
compiler, or it is looked for on PATH, in LLVM's folder and in Visual Studio's.
Since L2a all three pass under the NDK's clang-cl 19.0.1: `compile_runtime.py
--cc clang-cl` compiles every file, 37 of 37 on 2026-10-05 (29 of 29 at L7's `plat.c`; until then `gxr_tev.c` failed with clang's
SSE4.1 always_inline error, portability.md 2.2's). CI runs all three that way
on every push, in the clang-cl job (L4a, section 7), which is what reports the
day a runtime change compiles under MSVC and not under clang.

```
python tools/citest/compile_runtime.py
python tools/citest/dc_check.py
python tools/citest/render_check.py
```

**`compile_runtime.py`** — 3.2 s:

```
ok   aram.c
...
ok   window.c
ok   window_sdl.c
ok   gxv.c with SOA_GXV=1 (the backend)

compiled 37/37 runtime translation units
gxv.c: compiled as the backend too
the SDL window and sound: not compiled with SOA_SDL: Windows keeps window.c (D2)
not compiled here: nothing, every runtime/*.c is covered
```

`gxv.c` is compiled twice (V5): as every file is, which is the stub a build
without the GPU backend links, and with `SOA_GXV=1`, the backend, against
`vendor/`'s Vulkan-Headers (`python tools/fetch_gpu.py --headers` fetches them
alone) and one-word stand-ins for the SPIR-V, so no glslang is needed. Without
the headers it says so; `--require-gxv`, which every CI compile job passes,
makes that a failure.

`window_sdl.c` and `audio_sdl.c`, the window and sound off Windows (L10),
compile to nothing without `SOA_SDL`. Under `--cc gcc` or `clang` they are
compiled again with `SOA_SDL=1`, with the three files whose code that changes
(window.c, audio_out.c, main.c), against SDL3's headers (`python
tools/fetch_sdl.py --headers` fetches the pinned source alone). Without the
headers it says so; `--require-sdl`, which CI's Linux legs pass, makes that a
failure.

Each file compiled on its own with `/c` and the flags in
`tools/soa/toolchain.py`, plus nine warnings promoted to errors. Nothing links,
so C4013 — implicit declaration — is first among them: it is the only sign that
a rename left a caller behind. Three C4996 deprecation warnings (`getenv`,
`fopen`) print and are not errors. If a runtime file ever genuinely needs
generated code, it goes in the `UNCOVERED` table at the top of that script with
a reason and the job's log names it; the table is empty today, and
`test_citest.py` holds it that way.

**`dc_check.py`** — 2.8 s. Byte-matching a decompiled routine says nothing
about how it is *called*. This builds the units `units.txt` marks `native` with
the `/Ddc_*` renames `recompile.py` derives, links them to a driver, and
compares each routine with the host C library over 3,000 generated cases:

```
decompiled MSL routines vs the host C library, 3000 cases each
ok   strlen      3000 cases
ok   strchr      3000 cases
ok   memchr      3000 cases
ok   __memrchr   3000 cases
ok   strncmp     3000 cases
ok   strcat      3000 cases
ok   strncpy     3000 cases
ok   memcpy      3000 cases
ok   memset      3000 cases

all 12 routines agree with the host C library
```

One gap worth knowing: `memset` here is the wrapper only. Its fill is
`__fill_mem`, which `runtime/decomp_shims.c` answers with the host `memset`, so
the decompiled fill is checked by `tools/decomp.py` (it matches) and by the
in-port self test, not here.

**`render_check.py`** — 6.4 s. The only check in this section that looks at a
pixel; everything above it would pass for a renderer that drew nothing. It
links `gx.c`, `gxr.c`, `gxr_tev.c`, `png.c` and a driver with two stubs, and
runs the same two checks the in-port self test runs:

```
[gxr] rasterizing on 1 worker thread
[selftest] render full-screen quad      ok    got "307200 of 307200 red"
[selftest] render triangle rows         ok    got "complete, 53301 px"
[render] second frame hash 63a57c77609efd77 (not asserted)
[render] plat_f2i: 14 table cases, the unsigned depth cast, and all 4294967296 floats against cvttss2si: ok
[render] copy stride: rows of tiles 512 bytes apart, the gap untouched: ok
[render] out-of-range frame: 6 of 6 blocks as worked out by hand; hash aa535458106bed0e, pinned aa535458106bed0e: ok
[render] every render check passes
```

The register recipe in the driver is a verbatim copy of `selftest.c`'s, because
that one is `static` in a file that cannot link outside the port;
`tools/tests/test_citest.py` compares the two texts so the copy cannot drift.
The frame hash is printed and *not* asserted: `ceilf`/`floorf` at a span
boundary is where two MSVC versions could legitimately differ, and nobody has
run two.

Since L6 it also holds `plat_f2i`, x86's `cvttss2si` everywhere, to a table of
14 cases on every leg and to the instruction itself over all 2^32 floats on
x86, and checks that the two unsigned depth casts give 0 for a NaN. And it
draws a frame from out-of-range conversions: six blocks of constant texture
coordinates, some beyond int range or NaN, each one value worked out by hand
in the driver's comment and checked pixel by pixel, with the frame's hash
pinned. `--cflag=/DPLAT_F2I_SATURATE`, ARM64's conversion on x86, fails both.
It also copies a block to memory with its rows of tiles further apart than the
block's own width (BP 0x4D), as the battle transition does, and checks where
the rows landed and that the gap between them was left alone.

**`threads_check.py`** — 2.7 s (L7). Builds `runtime/threads.c` and
`runtime/plat.c` with `tools/citest/threads_driver.c`, and parks and resumes a
guest thread twice the way the recompiler wraps SelectThread's `OSSaveContext`,
on the stack `plat_run_on_big_stack` gives the guest. On Linux it also reads
that thread's stack size back, which must be `main.c`'s `GUEST_STACK_BYTES`;
the script holds that to `recompile.py`'s `/STACK` before it builds. Under gcc:

```
[threads_check] the guest's stack: 33554432 bytes, asked for 33554432
[threads_check] ok: two same-stack OSSaveContext/OSLoadContext round trips
[threads] 1 guest threads seen; 2 context saves, 2 resumes, 0 fiber switches
```

On Windows the stack line says the main thread's is sized by the link. CI runs
it on the three Linux legs, which are where the resume used to exit 6.

**`disc_check.py`** — 6 s (disc-layer I1, I3, I5). Builds `runtime/disc.c` and
`runtime/sha1.c` with `tools/citest/disc_driver.c` for a synthetic image
(`tools/soa/discfixture.py`: a test game id and its own executable's SHA-1
compiled in place of the real ones), and checks what disc.c hands back: the
executable, boot.bin and file table are the image's exact slices, from a folder
holding `disc.iso` and from the image named directly; every file read as the
game reads it (its length rounded up to 32) is the image's bytes; a read past
the end is the last bytes then zeros, and counted; each file is named at its
first and last byte, and padding, junk and the system area by none. Then ten
images refused, each in words of its own. That build has no system files of
its own (`--no-embed`'s `disc_sys.c`); a second has the fixture's built in
(I3), hands them back with no image open, opens their image saying the table
matches, and refuses an image whose table differs by one byte as not the
build's; a third, whose built-in table misses its own SHA-1, is refused before
anything is handed out. Then a store `tools/soa/store.py` writes from a
long-tailed fixture (I5): the image's bytes at every offset below its covered
end and zeros above, preferred in a folder; every touched block hashed;
`SOA_DISC_VERIFY=all` finding nothing; a flipped byte caught twice -- the ISO
comparison names its offset, and the hash check stops the driver with exit 9
naming the file; a header that misses its SHA-1, a truncated payload and
format 2 refused at open; a hash check asked of an ISO refused; and
`--check-disc`'s check passing the store and failing the flip:

```
disc check: 108 passed, 0 failed (12 files, 1212416 bytes)
  refused bad boot magic: broken0.iso is not a GameCube disc image (boot magic 00000000, not C2339F3D)
  refused another game id: broken1.iso is GTSP01, not GTSE01: this port is the North American GameCube release only (European and Japanes
  ...
  refused no image: no disc image at empty/disc.iso; make one from your own disc with: python tools/extract.py <your disc dump> (w
```

`--mutate fst-offset` moves the image's file table by four bytes and must fail
it. `--log <log> --data extracted` holds a live run's `SOA_DISC_LOG=1` lines to
the file table as `tools/soa/disc.py` parses it: every read names the file that
holds its offset, and how far past that file's end it reaches (title, 226 of
226 on 2026-10-05). CI runs the first form on Windows under MSVC and clang-cl
and on the three Linux legs, where it is the POSIX half's `stat` and `fseeko`.

**`libm_check.py`** — 18 s (L6). The renderer's `exp2f` (fog) and `log2f`
(texture LOD) are CORE-MATH's correctly rounded ones, `runtime/crmath.h`. This
builds `tools/citest/libm_driver.c` and runs every input the renderer can give
them, 1,090,519,041 for `exp2f` and 2,139,095,039 for `log2f`, in 64 fixed
slices a function at idle priority. Each output is compared with the host's
double-precision function rounded to float; where that lies within 2^-40 of a
rounding boundary, Python's `decimal` at 50 digits decides instead. The
outputs' hashes must match `config/libm.tsv`, and `--bless` rewrites that only
while every output is correctly rounded:

```
[libm] 128 slices in 6.6 s
[libm] exp2f over [-8, 0]: 1090519041 inputs; 6996 near a rounding boundary, decided by decimal at 50 digits, 6996 of them right; 0 not correctly rounded; hash d26a9f42e670180c (pinned d26a9f42e670180c): ok
[libm] log2f over (0, inf): 2139095039 inputs; 49987 near a rounding boundary, decided by decimal at 50 digits, 49987 of them right; 0 not correctly rounded; hash 68eb594274ef7407 (pinned 68eb594274ef7407): ok
```

`--cflag=/DLIBM_HOST` (or `-DLIBM_HOST`) checks the C library's own functions
instead: 74,153 and 313,550 outputs not correctly rounded, the same bits under
UCRT and glibc. CI runs it on the MSVC, clang-cl, Linux and ARM64 legs, which
print the same two hashes.

### One check on the built binary that still needs no disc

```
$env:SOA_MEMPOKE='0x81800000'
gen\soa.exe nodisc
```

```
[mem] a store from block 00000000 reached 81800000, past the console's 24 MB of RAM; the port keeps zeroed scratch up there so that it does not reach the host heap. An address up there means the port is not modelling something. Reported once.
  backtrace from r1:
[mem] SOA_MEMPOKE: 81800000 <- DEADBEEF, reads back DEADBEEF
```

That is the MEM1 out-of-range tripwire firing on purpose, before anything opens
the disc. It proves the `PAGE_NOACCESS` reservation and the vectored exception
handler are both in place in this binary (both `runtime/plat.c`'s since L7;
off Windows they are an `mmap(PROT_NONE)` reservation and a SIGSEGV handler,
which CI's Linux legs fire through `test_memguard.py`). The run then exits 1
with

```
[boot] no disc image at nodisc; make one from your own disc with: python tools/extract.py <your disc dump> (which writes extracted/disc.iso), or name an .iso or .gcm file
```

which is expected: `nodisc` is not a disc.

### What section 1 does not cover

Linking `soa.exe`, anything under `gen/`, the device models, the guest half of
`runtime/selftest.c`, the mwcc byte-for-byte match, the replay corpus, and
every frame the game itself draws. A green run here means the C compiles, nine
MSL routines behave like libc, and the rasterizer still fills a triangle. It
says nothing about the game running.

---

## 2. The build

```
python tools/recompile.py --compile --link
```

Needs the extracted disc (it reads `extracted/sys/main.dol`) and MSVC. After
changing anything under `runtime/` only, `--link` on its own is enough. Add
`--optimize` for a build that runs the game at full speed; the default `/Od`
build is for checking that the translated C is well-formed.

A successful run prints seven lines, in this order — the shape is
`tools/recompile.py`'s and the counts are this project's, with only the elapsed
times left as `<t>`:

```
7,144 functions, <n> switch tables (<t>s)
20 functions bound to HLE, 1 runtime hooks, 1 savepoints
emitted 7,144 functions into 18 files, 55.7 MB of C (<t>s)

instruction coverage: 100.000%  (696,171 translated, 0 not)

compiling 19 translation units with MSVC (/Od) ...
compiled 19/19 units in <t>s
linked gen\soa.exe (<t>s)
```

Every count there is checkable without building: 7,144 functions and 696,171
instructions are [ROADMAP.md](ROADMAP.md) slice 3.6's; `config/hle.txt`,
`hooks.txt` and `savepoints.txt` hold 19, 1 and 1 entries; and a built `gen/`
in this checkout holds 18 `chunk_*.c` totalling 55.7 MB, which with
`dispatch.c` is the 19 translation units.

The three lines worth reading every time:

- **`instruction coverage: 100.000%`** with `0 not`. Anything less prints an
  `unsupported, by mnemonic:` table underneath it naming what was dropped, and
  the port will run past those instructions doing nothing.
- **`compiled 19/19 units`**. A `FAIL chunk_0NN.c: ...` line above it is emitted
  C that does not compile.
- **`linked gen\soa.exe`**. Without it there is no binary, and sections 3, 4
  and 5 all need one.

Recorded timings (ROADMAP 3.6, 16 cores): the `/Od` validation compile is 3 s;
the `--optimize` release compile of the translated code is 103 s. The link is
where `runtime/*.c` and the natively compiled `src/` twins are built, so a
runtime-only change costs the link and nothing else.

**The clang-cl build (L3b).** With `SOA_CLANG_CL` naming a clang-cl (the
Android NDK's is enough), `python tools/recompile.py --cc clang-cl --compile
--optimize --link` writes its translation, objects and `gen\clang\soa.exe`
under `gen\clang` and touches nothing in `gen`: about a minute and a half on
this machine, 19 units compiled in 58 s. Its checks are the reference build's,
pointed at it:

```
$env:SOA_SELFTEST='1'; gen\clang\soa.exe extracted
python tools/scenario.py replay --exe gen/clang/soa.exe --threads 1,2,3,8
python tools/scenario.py run title --check --exe gen/clang/soa.exe
```

The replay is held to the MSVC manifest: the 23 frames are the same under both
compilers, so a hash that moves under one only is a difference to find, never
a manifest to re-bless (`replay --bless` refuses any exe but `gen/soa.exe`).
Either build says which compiler made it right after its `[boot] DOL` line:
`[boot] built with ...`.

**Under Wine (L1).** What the Steam Deck's Proton runs, smoke-tested in a
Linux container through Docker Desktop (D-13). The image is the distribution,
CI's Python and the distribution's Wine, and nothing else:

```
FROM python:3.14-slim
RUN apt-get update && apt-get install -y --no-install-recommends wine wine64 && rm -rf /var/lib/apt/lists/*
ENV WINEDEBUG=-all
WORKDIR /soa
```

Built as `docker build -t soa-wine:l1 .`. The checkout is mounted read-only,
`extracted/` with it, and a fresh named volume goes over `build/`, so no run
can write anywhere else; the captures and a card are mounted read-only apart
and copied onto the volume, so no original can be touched. No image layer
holds game data, and the volume is deleted afterwards:

```
docker volume create soa-l1-build
docker run --rm -v "C:/Users/<you>/.../SOA:/soa:ro" -v soa-l1-build:/soa/build \
  -v "C:/Users/<you>/.../SOA/build/fifo:/src/fifo:ro" -v "C:/Users/<you>/.../SOA/build/cards:/src/cards:ro" \
  soa-wine:l1 sh -c '
    wineboot -i
    cp gen/soa.exe /tmp/soa.exe
    mkdir -p build/fifo-copy && cp /src/fifo/*.fifo /src/fifo/*.regs /src/fifo/*.ram build/fifo-copy/
    cp /src/cards/slotA.raw build/card-copy.raw
    SOA_SELFTEST=1 wine /tmp/soa.exe extracted
    python3 tools/scenario.py replay --exe /tmp/soa.exe --wrap wine --fifo build/fifo-copy --threads 1,2,3,8
    python3 tools/scenario.py run title --check --exe /tmp/soa.exe --wrap wine --env SOA_CARD=build/card-copy.raw'
docker volume rm soa-l1-build
```

**Natively on Linux (L10).** gcc in a `python:3.14-slim` container through
Docker Desktop (D-13). The checkout is mounted read-only at `/src` and copied
to `/work`, so nothing is written back; `extracted/` is linked in read-only
and the captures are copied into the container's own layer, gone with it:

```
docker run -d --name soa-linux -v "C:/Users/<you>/.../SOA:/src:ro" python:3.14-slim sleep infinity
docker exec soa-linux sh -c '
  apt-get update -qq && apt-get install -y -qq gcc libc6-dev && pip install -q pytest
  mkdir -p /work/build && cd /src && cp -r runtime tools config src include mods examples pyproject.toml /work/
  cp -r build/fifo /work/build/fifo && cd /work && ln -s /src/extracted extracted
  python3 tools/recompile.py --cc gcc --compile --optimize --link
  SOA_SELFTEST=1 gen/linux/soa extracted
  python3 tools/scenario.py replay --exe gen/linux/soa --threads 1,2,3,8
  python3 tools/scenario.py run title --check --exe gen/linux/soa'
docker rm -f soa-linux
```

The build takes about three minutes on this PC, the replay 23 s.
`--split` builds Android's arrangement instead (specs/android.md L12a), into
`gen/linux-split`: `libsoa_runtime.so`, `libsoa_game.so` and the launcher
`soa`, which loads the game library from beside it after
`runtime/elfcheck.c` has passed it. The same three checks run on it with
`--exe gen/linux-split/soa`, and `SOA_CC=gcc python3 -m pytest
tools/tests/test_seam.py` then holds its libraries to `config/seam.txt`. For the
window and sound, install the packages `python3 tools/fetch_sdl.py` names
(Debian's), and `xvfb x11-apps xdotool` for a display with no screen, run
it (about a minute), `--link` again, and give the run a display:
`Xvfb :99 -screen 0 1920x1080x24 &`, then `DISPLAY=:99 SOA_RENDER=1
SOA_WINDOW=1 gen/linux/soa extracted`, or `DISPLAY=:99 SOA_CC=gcc python3 -m
pytest tools/tests/test_window_sdl.py`. `SDL_AUDIO_DRIVER=disk` sends the
sound to a file at the device's pace, which is how L10's audio rate was
taken; `openbox &` before the run gives fullscreen a window manager to ask.

**Run the exe from a copy inside the container.** Started straight off the
Docker Desktop share from Windows, Wine 10.0 faults at the entry point on a
page that reads as zeros, though Python's own `mmap` of the same file reads it
correctly; the copy in `/tmp` is the container's own layer, gone with `--rm`.
`--wrap` puts its words before the exe for `run` and `replay`, and a wrapped
replay may not `--bless`: Wine's frames are held to the manifest, never
written into it.

If MSVC cannot be found, every step says so on stderr — `MSVC not found;
skipping compile` — and returns 1 rather than pretending it did the work.
`tools/soa/toolchain.py` finds it through `vswhere` and runs `vcvars64.bat`, so
`cl.exe` does not need to be on `PATH`.

---

## 3. The self test (86 cases)

```
$env:SOA_SELFTEST='1'
gen\soa.exe extracted
```

**0.11 s.** This is the cheapest real check in the project and the one to run
after every `--link`. It calls the recompiled library functions, the device
models, the AX mixer and the software renderer directly, outside the game's
control flow, so a wrong answer is a bug with a two-line repro. The last line
is the verdict:

```
[selftest] 0 failure(s)
```

Every case prints `ok` or `FAIL`, what it got and — on failure — what it
wanted. A failure exits **7**. It does not need a window, a sound device or a
card, but it does need the disc: `main.c` loads the DOL before it runs
anything, because the checks `dispatch()` into recompiled code by address.
Without one you get the boot message and exit 1, not a self-test result:

```
> $env:SOA_SELFTEST='1'; gen\soa.exe nodisc
[boot] no disc image at nodisc; make one from your own disc with: python tools/extract.py <your disc dump> (which writes extracted/disc.iso), or name an .iso or .gcm file
```

That is the reason none of section 3 runs in CI.

The 86 cases, in the order they print:

| # | Group | Cases |
|---|---|---|
| 1–26 | The memory card, through the EXI registers | 26 |
| 27–35 | The MSL C library, through the recompiled code | 9 |
| 36–37 | ARAM DMA, both directions | 2 |
| 38–70 | The AX mixer | 33 |
| 71–72 | The software renderer, through the real GX pipe | 2 |
| 73 | Decompiled against recompiled | 1 |
| 74 | `VIGetRetraceCount`, native against its twin | 1 |
| 75 | The data-cache range calls, native against their twins | 1 |
| 76 | Paired-single loads and stores against the general formula | 1 |
| 77 | The race seed at OSGetTick | 1 |
| 78 | The encounter multiplier as the game works it out | 1 |
| 79 | The rumble motor, from the OUTBUF writes PADControlMotor makes | 1 |
| 80 | `unfocused = mute`: the device gets zeros, then the block again | 1 |
| 81 | The audio DMA across a clock epoch: the owed blocks still come | 1 |
| 82 | The audio DMA's pace through the game's running writes | 1 |
| 83 | A mod's call into the game: every register as it was | 1 |
| 84–85 | A fused multiply-add rounds once: the guest's `fmadds`, and the runtime's software `fma` over 200,000 inputs (distribution R1) | 2 |
| 86 | The game's table against what the build calls directly: the entry, `dispatch`, the system files, the record, all 21 twins (specs/android.md L12a; in a split build, that a table is loaded) | 1 |

### The memory card (26)

Every command the CARD library sends, driven the way the library drives it:

```
[selftest] card image starts absent     ok    got "gone"
[selftest] card device ID               ok    got "00000004"
[selftest] card read status             ok    got "41"
[selftest] card program and read back   ok    got "512 bytes match"
[selftest] read latency on the DMA path ok    got "four dummy bytes, then the array"
[selftest] SRAM flash ID from image     ok    got "112233445566778899AABBCC"
[selftest] SRAM flash ID checksum       ok    got "D1"
[selftest] SRAM checksum and flags      ok    got "0004 FFF8 04"
[selftest] SRAM write and read at offset ok    got "5A 11223344 in place"
[selftest] SRAM DMA both ways           ok    got "32 bytes in and back"
[selftest] program raises the interrupt ok    got "csr 1 pending 1"
[selftest] masked interrupt waits       ok    got "csr 1 pending 0"
[selftest] masked, nothing delivered    ok    got "0"
[selftest] delivered to interrupt 9     ok    got "9:1 12:0 15:0 csr 0"
[selftest] cleared, nothing delivered   ok    got "0"
[selftest] handler reads status         ok    got "41 midway 0"
[selftest] handler leaves it quiet      ok    got "csr 0 pending 0"
[selftest] programmed page reads back   ok    got "one page in an erased sector"
[selftest] erase raises the interrupt   ok    got "pending 1"
[selftest] erase handler sees a clear bit ok    got "midway 0"
[selftest] sector erase                 ok    got "sector blank, ID block intact"
[selftest] consecutive page programs    ok    got "four pages, four interrupts"
[selftest] disabled card stays quiet    ok    got "csr 0 pending 0"
[selftest] card image flushed to disk   ok    got "524288 bytes, 97BF"
[selftest] reloaded from disk           ok    got "ID block 1122..BBCC D1"
[selftest] card counters                ok    got "3100 read, 1280 written, 6 interrupts"
```

The command set is device ID (0x00), read status, set interrupt (0x81), clear
status, sector erase (0xF1), page program (0xF2), read array, and the read
latency on the DMA path — plus the interrupt each write ends with and the two
SRAM checksums the mount recomputes. It runs first and on an image of its own
(`build/cards/selftest.raw`), which it deletes and rebuilds each run: the flash
ID the runtime presents in SRAM is recovered from the card image the first time
anything reads SRAM, so the image has to be shaped before that happens.

Two things that look like flakiness and are not:

- **The two hex digits in `card image flushed to disk` change every run.** The
  format time seeds the scrambled serial, so every byte of the ID block
  downstream of it differs run to run — `001C`, `6CA8` and `97BF` on three
  consecutive runs here. The expectation is computed from the block this run
  wrote, not blessed. That is the point: a check that read back what a previous
  run left behind would pass while proving nothing.
- **`card image starts absent` failing** means the last run's image could not be
  deleted (a stale handle, a read-only file). Everything after it would pass
  against old bytes, so it fails there instead.

Reverting any one of the three EXI mount fixes fails a check that names it —
measured, one revert at a time. So does deleting `irq.c`'s EXI delivery block
or renumbering it: the interrupt checks install the guest's own
`EXIIntrruptHandler` in the slot `deliver_pending` reads and count what arrives
there, rather than reading back the predicate under test.

### The C library and ARAM (11)

`sprintf` four ways (strings, integer conversions and widths, more than four
arguments, and floats through the FPU with the CR bit-6 varargs convention),
then `strcpy`+`strcat`, `strlen`, `strcmp`, `memset` and an unaligned `memcpy`,
all executed as recompiled PowerPC. Then ARAM DMA in both directions, driven
through the AI registers the way `ARStartDMA` drives them — the direction has to
survive the read-back-and-merge the SDK does in the middle.

### The AX mixer (33)

The ADPCM arithmetic (scale, shift flooring, predictor, coefficient pairs,
clamping), one-shot and looped playback with the state a loop restores, pan and
word order, the Q15 envelope, bus width and clamping, gain ramps, rate
consumption, both auxiliary buses in both directions including the mode this
game never selects, and the command-list walker's limits (the end command, the
64-command cap, a self-linked block, the chain guard). Every expectation is
derived from the sample format's own arithmetic rather than blessed from what
the mixer produces — which matters, because this project has already pinned a
bug in place by blessing output once. The first run of these found a real
defect: the resampler formed `(next - prev) * fraction` in `int`, wrapping a
silent sample to full-scale negative.

### The renderer's pixel checks (2)

```
[selftest] render full-screen quad      ok    got "307200 of 307200 red"
[selftest] render triangle rows         ok    got "complete, 53301 px"
```

Both go through the real GX pipe — bytes written to the gather pipe, parsed by
the front end, rasterized — using the register recipe the game uses for its
full-screen fades, so they pin down the command encodings as well as the
rasterizer.

1. **The full-screen quad** must cover *every* pixel: 307,200 of 640×480, exact.
   Off by one pixel and it fails.
2. **The title screen's cloud triangle**, at the real coordinates
   (320.5, −22.1), (789.1, −22.1), (789.1, 530.2). Its top edge is off
   horizontal by a sub-pixel amount, as the perspective divide leaves it; the
   span code once divided by that crumb, overflowed the int conversion and
   dropped whole rows. The check is a count *and* six probe pixels: three that
   must be green (600,300), (630,10), (500,100) and three that must not be
   (360,60), (600,340), (100,200), so a triangle that filled its bounding box
   fails as loudly as one full of holes.

These two also run in CI through `render_check.py` (section 1), which is the
only pixel CI ever looks at.

### Decompiled against recompiled (1)

```
[selftest] decompiled vs recompiled     ok    got "12 functions agree over 200 rounds"
```

The twelve functions that `config/hle.txt` swaps in — `strlen`, `strchr`,
`memchr`, `__memrchr`, `strncmp`, `strcat`, `strncpy`, `memcpy`, `memset`,
`strcpy`, `strcmp`, `strstr` — each run against the recompiled translation of
the same address, on the same guest memory, over 200 rounds of random strings
drawn from a six-letter alphabet so repeats and near-misses actually occur.
This is what catches a match that is somehow still wrong, and a native routine
that is right on its own but wrong at the boundary:

- The writing routines (`strcat`, `strncpy`, `strcpy`, `memcpy`, `memset`) are
  compared over the whole 128-byte destination buffer, not by comparing the
  resulting string. `strcpy`'s word path reads up to three bytes past the
  terminator inside an aligned word; a twin that wrote one byte too many would
  pass a `strcmp` of the result.
- `strcmp` is compared on the *sign* only. The twin's word path is big-endian
  and the magnitudes are allowed to differ; the guest only ever tests the sign.
  This is the one twin whose C needs a host-order guard, and getting it
  backwards is exactly the defect that was found by running it.

### `VIGetRetraceCount` against its twin (1)

```
[selftest] VIGetRetraceCount native vs twin ok    got "the count at 0x80347A64 over 200 rounds and four call sites"
```

`runtime/tick.c` answers `VIGetRetraceCount` (PLAN-60FPS-MODS M2) so the main
loop has a safe point and a tick the port can let go. It must be the original
-- the retrace count at 0x80347A64 -- everywhere but the frame end's spin once
unlocked. This runs it against the recompiled translation over 200 random
counts and four call sites, the top of the loop among them, where it adds only
a pass over the callbacks. The spin, and the callbacks, are `test_tick.py`'s.
Answering the count plus one at one call site fails it at round 2.

### The data-cache calls, paired singles, the race seed (3)

```
[selftest] DC range calls native vs twin ok    got "five calls over 100 rounds, a quarter of them empty"
[selftest] psq_load/psq_store vs generic ok    got "8192 loads and stores over 8 types and 64 scales agree with the ldexp formula"
[selftest] race seed at OSGetTick       ok    got "three sites pinned twice each and srand stored each pin; 8000A1DC left to the clock"
```

The five data-cache range calls (H13) are no-ops natively; their twins must
leave memory and the syscall count as the natives do. `cpu.h`'s paired-single
fast path is held to the `ldexp` formula over every type and scale. The race
seed (P6) runs the game's own OSGetTick and srand from each reseed site twice,
and from `0x8000A1DC` must get the clock (FINDINGS "H13, first steps", "P6").

### The encounter multiplier (1)

```
[selftest] encounter multiplier as the game has it ok    got "FF with none or without effect 84, 64 for 210, 05 for 211 on a later character"
```

`mods/encounter-rate` works out the multiplier the game would hold from the
party's accessories, and writes it back after a hold of B; this runs the
game's own `fn_801EF7E0` on five parties set up in memory and needs the byte
the mod's reckoning gives (FINDINGS "P1a"). Claiming the first character's
accessory wins fails it: the game gives 05.

### The rumble motor (1)

```
[selftest] rumble motor from OUTBUF     ok    got "on, off, off hard, full and half strength; nothing for channels 1-3, strength 0, no window or a script"
```

`si.c` hands a change of channel 0's motor bits to the sink `window.c` sets
(M18); a counting sink stands in for XInput, and the writes go through
`si_write` as the game's PADControlMotor makes them. A gate that ignores
the window fails it (FINDINGS "M18").

### Silence with another window in front (1)

```
[selftest] unfocused mute zeroes the device ok    got "muted 8 of 8 zero, unmuted 8 of 8 the block's, 4 frames"
```

`audio_set_muted(1)` and a nonzero block pushed: the samples the device
would get, through audio_out.c's test sink, are all zero; unmuted, they are
the block's, left then right. A mute that does not reach those samples
fails it (FINDINGS "M5b").

### The audio DMA across a clock epoch (1)

```
[selftest] audio DMA across a clock epoch ok    got "same epoch 4 of 4 polls, new epoch 1 then 1"
```

A DMA deadline left eight blocks behind is caught up a block a poll, and
an epoch change (M19) does not throw the owed blocks away: the clock counts
no gap, so an epoch brings no backlog of its own, and restarting the
deadline there only dropped blocks (FINDINGS "M19, followed up"). Restarting
it (tried) gives `new epoch 1 then 0`.

### The audio DMA's pace (1)

```
[selftest] audio DMA keeps its pace through AIInitDMA ok    got "3 of 3 owed blocks played after a running write"
```

The game writes the DMA's length once a block while the engine runs, keeping
the enable bit (`fn_802416E4`, AIInitDMA's shape). That write sets the next
block and nothing else: three blocks owed before it are all played after
it. Taking it as a restart, as the port did until 2026-09-25, plays none of
them -- `0 of 3` (tried) -- and every block came a delivery late, the audio
5% slow (FINDINGS "The AI DMA's pace").

---

## 4. The scenario library

Thirteen scripted runs in `config/scenarios/`, each a pad script, a frame
budget, an environment, what it is supposed to show, and an `evidence:` line
naming the log in `build/` or the section of FINDINGS it was recovered from.

```
python tools/scenario.py list
```

```
audio        11500 frames   19 events  The opening, headless, with every mixed sample written to a WAV file.
battle       12000 frames   19 events  The opening through to the first battle and out the other side.
capture       4500 frames   18 events  Run the opening headless and dump three command streams for --replay.
cardwrite     6000 frames    5 events  Blank card in slot A, New Game, then Yes to "Proceed with formatting?" -- the only CARDFormatAsync in the executable.
encounter    30000 frames   23 events  Walk a101b for twenty minutes of game time until a fight starts.
hold         16500 frames   21 events  Past the battle into the ship's hold, then walk Vyse with the stick.
monkey      126000 frames   24 events  The hold and its menus mashed with five buttons on five different periods.
newgame       3400 frames   18 events  Nine START/A pairs from 1600 to 3240: the title, New Game, the first dialogue.
opening       7500 frames   19 events  The Valuan bridge and the boarding, advanced by one A press every 150 frames.
partl         3000 frames    9 events  Continue a Part L save from the title into its field (126a), every frame drawn, headless.
speed        10000 frames   19 events  The opening headless at ten times guest speed, rendering nothing.
title         2000 frames    2 events  Boot through the logos to the title screen, press START, then New Game.
voice        12600 frames   19 events  The opening through the end of the first battle, headless: the first recording this port has made that contains speech.
window        7500 frames   19 events  The opening in a window at 2x, so the frame rate is the one a person sees.
```

### Running one

```
python tools/scenario.py run title --check
```

The tail of that run:

```
[check] the run exits 0                     pass  exit 0, 2000 frames
[check] no MMIO outside the modelled range  pass  no [mmio!] lines
[check] no unknown FIFO bytes               pass  0 unknown bytes of 354674256
[check] no bad vertex references            pass  0 bad vertex refs over 7874314 vertices, 112314 triangles
[check] the pad script was understood       pass  2 events
title: 4 of 4 invariants hold
```

The log goes to `build/scenario-<name>.log` (gitignored), and `--quiet` stops
the run echoing to the terminal. `python tools/scenario.py show title` prints
the scenario with its evidence and the exact command line;
`show <name> --command --shell powershell|cmd|sh` prints just the command, so
you can run it by hand:

```
$env:SOA_PAD='1600:start,1640:a'
$env:SOA_FRAMES='2000'
$env:SOA_RENDER='1'
$env:SOA_SNAP='50'
gen\soa.exe extracted
```

**With the GPU drawing** (V5), `--env SOA_GPU=vulkan` adds a fifth invariant,
`the GPU drew the run`: one `[gxv] Vulkan ... on ...` start line, no `[gxv]
fallback:`, and a `[gxv]` report whose draws, copies and clears are what the
renderer sent the backend (`[gxr] vulkan backend: ...`) and whose screen and
texture copies are `[gxr]`'s:

```
python tools/scenario.py run title --check --env SOA_GPU=vulkan
[check] the GPU drew the run                pass  26336 draws, 2000 screen copies, 152 copies to a texture, as sent; Vulkan 1.4.344 on AMD Radeon Graphics ...
title: 5 of 5 invariants hold
```

With `--env SOA_GPU_LOADER=nonexistent.dll` the run falls back to the CPU and
that invariant fails while the other four pass. `scenario.py check <log>` applies it
to any log with a `[gxv]` line, and `tools/tests/test_gxv_live.py` holds it to canned logs.

`python tools/scenario.py check build/scenario-hold.log --name hold` applies the
same checks to a log written earlier — useful for a run you drove by hand, and
free.

**Run one at a time.** Two runs of the port at once share `build/frames/`,
`build/cards/slotA.raw` and, if you do not pass `--log`, the same log path, and
in this session three consecutive scenarios died part-way through — silently,
exit `-1`, with no report line — while a second `soa.exe` was running, and
passed when run alone. Nothing in the tool stops you; the collisions are in the
filesystem.

### What the invariants mean

| Check | Reads | Fails when |
|---|---|---|
| the run exits 0 | the process status | the run did not reach its frame budget. The runtime documents its codes: 1 the disc did not load, 2 something unimplemented, 3 a guest trap, 4 a spin on a register answered with zero, 5 the watchdog, 6 the thread model gave up, 7 the self test, 8 `SOA_STRICT` |
| no MMIO outside the modelled range | `[mmio!]` lines | the guest touched hardware the runtime does not model — almost always a garbage pointer |
| no unknown FIFO bytes | the `[gx]` report line | the graphics front end met a command byte it could not parse. **Also fails at zero with nothing behind it**: a run that pushed nothing to the GP has checked nothing |
| no bad vertex references | the `[gxr]` report line | a draw referenced a vertex outside its array. Also fails at a zero with no triangles behind it |
| the pad script was understood | `[si] N scripted controller events` | the port read a different script from the one the file says. Without it the other four can all be true of a run that drove nothing |

`--check` sets `SOA_STRICT=1`, which stops the run at the *first* unmodelled
access with a guest backtrace and exits 8, rather than logging the first twenty
and carrying on. Knowing where beats knowing how many. The exit-status check
then reports itself as skipped and points at the MMIO check, so one broken thing
is named once. `--no-strict` runs to the frame limit and counts the lines
instead, which is what you want when a bad access is already known.

A check reports **skip** when the run could not have produced the evidence. A
headless scenario prints no `[gxr]` line at all, so:

```
[check] no bad vertex references            skip  nothing was rendered (no SOA_RENDER), so gxr_report prints nothing
scenario-capture.log: 3 of 4 invariants hold (1 skipped: no bad vertex references)
```

A scenario that *does* set `SOA_RENDER` and still prints no `[gxr]` line fails
that check rather than skipping it.

### How long each one takes

Measured here, one at a time, `python tools/scenario.py run <name> --check
--quiet`, on 16 cores:

| Scenario | Frames | What sets the pace | Wall time |
|---|---|---|---|
| `title` | 2,000 | renders, a PNG every 50 frames | **71 s** |
| `newgame` | 3,400 | renders, a PNG every 100 | **118 s** |
| `capture` | 4,500 | headless; writes three 24 MB captures | **154 s** |
| `cardwrite` | 6,000 | headless, tracing on | **204 s** |
| `opening` | 7,500 | renders, a PNG every 100 | **256 s** |
| `window` | 7,500 | opens a real window at 2x | *not run here — needs a desktop* |
| `speed` | 10,000 | `SOA_SPEED=10`, nothing drawn | **130 s** |
| `audio` | 11,500 | headless, every sample to a WAV | **389 s** |
| `battle` | 12,000 | renders, a PNG every 100 | **408 s** |
| `voice` | 12,600 | headless, WAV and tracing | **425 s** |
| `hold` | 16,500 | `SOA_SPEED=3`, a PNG every 300, three captures | **262 s** |
| `encounter` | 30,000 | `SOA_SPEED=3`, a PNG every 500 | **425 s** (7 min) |
| `monkey` | 126,000 | `SOA_SPEED=3`, a PNG every 1,000 | **1,577 s** (26 min) |

All twelve headless ones back to back are about 74 minutes, and `monkey` is
more than a third of that. For a change to `runtime/si.c` or a scenario file,
`title` at 71 seconds is the one to run; for a change that could affect the
game's own logic, `opening` at four minutes reaches the boarding.

**The frame count does not predict the time**, and the reason is worth knowing.
At `SOA_SPEED=1` every scenario runs at 28–30 game frames per wall second —
`title` 28, `newgame` 29, `opening` 29, `battle` 29, `audio` 30, `voice` 30 —
whether it renders or not, which is the game's own 30 fps cap and not the host:
the port spends the difference idling, as PLAN A4 measured directly (47% of a
run is `SelectThread`). `SOA_SPEED=3` lifts that to 63–80 (`hold` 63,
`encounter` 71, `monkey` 80), short of the 90 the cap would then allow, so the
host is starting to be the limit. `speed`, at `SOA_SPEED=10`, manages 77
against a cap of 300: that scenario measures the host, the others measure the
guest.

### The invariants are not the point of every scenario

`cardwrite` is the example, and it caught this page out. It is judged by one
number in the `[exi]` line — `40960 written`, which had been 0 in every log
this repository ever kept — and `--check` does not read that number. Its own
file says **delete `build/cards/blank.raw` before every run**: a blank card is
the absence of the file, a successful format creates it, and the second run
then finds a valid card, never draws the prompt and writes nothing. Run here
without deleting it, `cardwrite` reported

```
cardwrite: 3 of 4 invariants hold (1 skipped: no bad vertex references)
```

with `[exi] ... card 81920 bytes read, 0 written, 0 interrupts` in the log. All
the checks it can make passed, and the thing the scenario exists to show did
not happen.

Pointed at a card image that really is absent, the same scenario — 3 m 24 s —
gives the same three ticks and a completely different log:

```
> python tools/scenario.py run cardwrite --check --quiet --env SOA_CARD=build/cards/fresh.raw
[exi] memory card build/cards/fresh.raw: new blank card
...
[trace] CardScreenStatus@r31   ... r3 00000002     <- status 2: the format prompt
[trace] CardPromptYes          ... pc 8022B47C     <- Yes, confirmed
[trace] CardFormatAsyncCall    ... pc 801A2758     <- the one CARDFormatAsync in the executable
[trace] CardFormatResult       ... r3 00000000
[exi] 2403 immediate, 609 DMA transfers; card 122880 bytes read, 40960 written, 325 interrupts
```

and `python tools/cardformat.py verify build/cards/fresh.raw` then ends
`verdict: CARDDoMount would return 0 (READY)` — the game formatted a card the
game's own mount accepts. None of that is in the exit status. Read the `shows:`
and `note:` lines of a scenario before trusting it — `python
tools/scenario.py show <name>` prints them.

### What the scenarios do not cover

They are the cheap invariants. None of them looks at a pixel, a sample or a
counter's value — a renderer that painted every frame black passes all four.
What a frame actually contains is section 5. `--check` also counts only
positives: a stray `[gxr]` or `[mem]` tripwire line is in the log but does not
fail the exit status, so read the log when something changed in the renderer.

---

## 5. The frame-hash corpus

```
python tools/scenario.py replay
```

**18.3 s.** This is the only check in the project that compares what the game
actually drew with what it drew before.

```
[replay] 23 captures x 4 thread counts x 2 passes = 184 replays, each loading a 24 MB memory image
[replay] pass 1, SOA_THREADS=1
[replay] pass 1, SOA_THREADS=2
[replay] pass 1, SOA_THREADS=3
[replay] pass 1, SOA_THREADS=8
[replay] pass 2, SOA_THREADS=1
[replay] pass 2, SOA_THREADS=2
[replay] pass 2, SOA_THREADS=3
[replay] pass 2, SOA_THREADS=8
  ok    0100: 929dde33f20748af
  ok    0300: 0f45ed5ffa309d19
  ok    0500: 895a38755a9ed879
  ok    0700: 1af7b540bace66b0
  ok    11900: ee1d9d70189d1ae6
  ok    12000: fd09804a9ef0c71d
  ok    12100: 20926164c40594f0
  ok    1500: 398cc5b72790e18e
  ok    15200: 3279b1ae0d8ec929
  ok    1550: 8d5525a97371f560
  ok    15800: 48922aa95fed6683
  ok    16300: bc911bca56c5dfa1
  ok    2000: baf1dae6d503defd
  ok    2050: 0d46fbdcb1053ea6
  ok    2100: b4eba6e04b2cf520
  ok    3600: e4ea1dae37a91aa5
  ok    3900: b768e25aeac6b91f
  ok    4200: aafb63c62b2b3b13
  ok    4500: 7ec6eb11bd244d52
  ok    4800: 61ea704e28906f81
  ok    6000: b64d79603a729baa
  ok    6300: 68e3d078cec5fcc0
  ok    8000: bd6e7ee0bfb37f19
[replay] 23 captures match config/fifo_manifest.tsv at SOA_THREADS 1,2,3,8
```

`--threads inline` sweeps with no worker pool at all (`SOA_GXR_INLINE=1`),
the path a GPU backend takes: every command runs on the producer as it is
built, and the run must say `rasterizing on 0 worker threads`. `python
tools/scenario.py replay --threads inline,1,8` is V2's check that the two
paths draw the same 23 frames.

Each capture is three files written by `SOA_FIFO_DUMP`: the frame's command
stream, the CP/XF/BP register shadows it began with, and a 24 MB image of MEM1.
`gen/soa.exe --replay <base>` renders one with no game running at all, and
`SOA_HASH=1` makes it print `[gxr] frame N WxH hash <16 hex>` — FNV-1a over the
finished rows after a `gxr_flush()`.

### What 184 replays prove, and what they do not

- **The same bytes in give the same pixels out.** Two passes over the same
  capture with the same thread count must agree; a disagreement is
  non-determinism in the renderer itself.
- **The answer does not depend on how many threads rasterize it.** That is the
  whole reason for sweeping 1, 2, 3 and 8. A hash that moves with the thread
  count is a race in the rasterizer, not a rendering — this is what pinned the
  worker hand-off across a flush, and how the `float tex[8][4]` slots that read
  stack were found.
- **A change to the renderer is visible and attributable.** It has already
  caught two real things: the test suite quietly overwriting its own corpus,
  and the texture use-after-free of 2026-09-17, whose fix moved 8 of the 23
  frames. Those 8 old hashes were the manifest pinning broken output — washed
  green and blue, with gold, maroon and skin tones missing — so a hash moving is
  a question, not a verdict. Look at the PNG before re-blessing.
- **It does not prove the frames are right.** They are pinned to what this port
  rendered, not to a console. The corpus also contains no line and no point
  draw, so the row-ownership rule in `raster_line` and `raster_point` is
  unpinned: both draw on every worker with no `my_row` test and nothing here
  would catch it.

### The manifest, and why it can live in the repository

`config/fifo_manifest.tsv` has three columns: the capture's name, **sha256 over
its own `.fifo`, `.regs` and `.ram`**, and the frame hash. The middle column is
what makes a row mean something on a machine that cannot have the capture —
without it a row could not be told from a typo. A mismatch there is reported
differently from a pixel change:

```
input <name>: this capture hashes ..., the manifest was blessed from ... -- a different capture, so the frame hash means nothing
```

### The warning: the corpus lives in a gitignored directory

`build/fifo/` is the corpus. It is game data — 24 MB of the game's own memory
per capture — so it is gitignored, never committed, and **exists on exactly one
machine**. There is nothing to restore it from.

- **A capturing run must be pointed somewhere else.** Set
  `SOA_FIFO_DIR=build/fifo-new` (or anything but `build/fifo`) alongside
  `SOA_FIFO_DUMP`. The two scenarios that capture — `capture` and `hold` — both
  do, and `scenario.py run` creates the directory for you. A run that dumps into
  `build/fifo` overwrites a capture whose blessed hash then means nothing, with
  nothing to restore from.
- **`--replay` overwrites `<base>.png`.** The reference PNGs are the only
  rendering of those frames anyone has, so the first sweep copies them to
  `build/fifo/ref-before` before it starts and says so.
- **Re-blessing is deliberate.** `python tools/scenario.py replay --bless`
  rewrites the manifest, and refuses to do it if the sweep did not agree with
  itself. Blessing a frame you have not looked at is how the texture
  use-after-free stayed pinned for a day.

Adding a capture to the corpus is therefore a deliberate two-step: capture into
`build/fifo-new`, sweep it on its own with `python tools/scenario.py replay
--fifo build/fifo-new` to see that it renders the same way at every thread
count, then move the three files into `build/fifo` and `--bless`. A capture
that is in `build/fifo` but not in the manifest is reported as `new`, and one
in the manifest with no files behind it as `gone`; both fail the sweep rather
than being quietly ignored.

`SOA_HASH=1` is for replays and nothing else. In a live run it forces a
`gxr_flush()` on every frame the port presents, which spoils any frame-rate
measurement, and `SOA_SNAP` skips rasterizing the frames it is not writing, so
most of the hashes would be of a frame nothing drew.

### The in-between image (H10)

```
python tools/midpoint.py            # the five pairs in build/perfset, 8 threads
```

About three minutes. Each pair's captures are checked against
`config/perfset_manifest.tsv`, copied to a scratch directory and replayed one
`soa.exe` at a time with `--replay F F+1`. Every pair must pass seven checks:
pass 2 is F+1 exactly as a single replay draws it; the pairs the renderer
wrote are `fifopair.match`'s, as many as H4 counted; the images are the same
at one thread; F paired with itself is F; t=1 is F+1; and a copy of F+1 with
one texture address changed loses the same pairs in both implementations.
The last line reads `midpoint: 5 of 5 pairs pass`, and anything else exits 1.

It pins no hash. The in-between image has no reference but the eye, so the
images land in `build/midpoint/<scene>/` -- `F.png`, `mid.png`, `F1.png`,
`t0.png` and `diff.png`, magenta where the in-between image differs from F+1 --
and a change to the lerp means opening them. The synthetic side, where the
right answer is known exactly, is `tools/tests/test_gxr_pair.py` in section 1.

---

### The frame oracle (V0)

A GPU renderer cannot reproduce these hashes: edges, interpolation and
blending round differently. `tools/imgdiff.py` is the tolerance that judges it
instead (specs/gpu-backend.md 3.12), frozen before any GPU frame existed:

```
python tools/imgdiff.py REF.png CAND.png [--heat out.png]   # one pair: metrics and a verdict
python tools/imgdiff.py refs [--set corpus|perfset|gpuset]  # the CPU references, 6 s for the corpus
python tools/imgdiff.py mutate                              # the thresholds against ten mutations, 3 min
```

Per pixel, d is the largest of |R-C| over red, green and blue: exact at 0, near
at 1-16, far above. A frame passes with far pixels under 1%, at most 64 blob
pixels (far pixels whose eight neighbours are far too) and no blob over 32,
MAE at most 1.5, |bias| at most 0.75 per channel, and the filter test: the
error regressed on the reference's first difference (S) and second difference
(L) per axis, |S| at most 0.25 and |L| at most 0.10. The filter test catches
what the rest pass: a frame drawn a pixel over (S = -1) and a second vertical
1:2:1 filter (L = 0.25), whose errors are small and everywhere.

`refs` replays each capture from a scratch copy, with no `SOA_*` in the
environment but `SOA_SETTINGS=0`, `SOA_HASH=1` and `SOA_THREADS=4`, and keeps
the frames in `build/gpu-oracle/ref/<set>/`; every corpus frame must hash to
the manifest. `mutate` runs ten mutations over the corpus references: identity,
+-1 noise, 0.3% scattered pixels and a one-pixel diagonal line must pass on
all 23; a black 24x24 block on the most detailed region, +4 brightness, a
red-blue swap, a washed-out transform, a one-pixel shift and a second vertical
blur must fail on all 23 but the blind spots `BLIND_SPOTS` lists with their
reasons (two near-white boot frames for brightness, two grey frames for the
swap). The verdict and the thresholds are FINDINGS "V0"'s.

`--set gpuset` is V1's captures in `build/gpuset`, pinned by input in
`config/gpuset_manifest.tsv`: a random battle's first frame, whose screen is
copied to a texture, the 30 frames that sample it, and a field frame with the
mask effect. They were captured with `SOA_RENDER=1`: a capture's RAM holds a
copy's output only when the live run drew, and the frames after a copy read
it. `python tools/fifo.py BASE --summary` lists what a frame does besides
draw: every copy with its format, destination and size, the draws under each
logic op, and its display lists.

### The GPU spike (V3a, V3b, V4a, V4b)

The renderer drawing through Vulkan, headless and outside the port, on this
machine's GPU (specs/gpu-backend.md V3a, V3b, V4a, V4b). It needs MSVC, a Vulkan driver and
`vendor/`, which one command fills and which is never committed:

```
python tools/fetch_gpu.py            # Vulkan-Headers and glslang, pinned, into vendor/
python tools/fetch_gpu.py --headers  # Vulkan-Headers alone: what compiling gxv.c needs
python tools/fetch_gpu.py --verify   # every fetched file against vendor/GPU.sha256
python tools/gpuspike.py selftest    # build if stale, draw 17 scenes on the CPU and the GPU, compare; ~5 s
python tools/gpuspike.py tevdiff     # tev.glsl against tev_pixel, 100,000 random setups; 1 s
python tools/gpuspike.py copydiff    # copy.comp against gxr.c's copies, 25,600 random copies; 22 s
python tools/gpuspike.py loddiff     # lod.glsl against the sampler and span_lod, 100,000 cases each; 1 s
python tools/gpuspike.py contrast --set corpus,perfset,gpuset   # the spike and soa.exe --replay on the GPU, the same pixels; 5 min
python tools/gpuspike.py live title --range 1290-1400   # a running game on CPU (twice) and GPU, seeded, V0 per frame; 4 min
python tools/gpuspike.py queue       # V6a's queue frame: the CPU's hash on the thread, inline and stalled; 10 s
python tools/gpuspike.py overlap     # V6b: the copy hazards with the GPU as consumer, held to its synchronous run; 15 s
python tools/gpuspike.py specdiff --set corpus,perfset,gpuset   # V7: specialised and interpreted, the same bytes; 40 s
python tools/gpuspike.py copyimage   # V7: a copy sampled in its own frame, from the GPU's pool; 2 s
python tools/gpuspike.py present     # V8: the GPU's presenter against picture_scale, 32 layouts; 2 s
python tools/gpuspike.py oracle      # corpus and benchmark set, CPU against GPU, V0's verdict; 40 s
python tools/gpuspike.py oracle --set corpus,perfset,gpuset --mutations   # all 67 and eight mutations; 16 min
python tools/gpuspike.py logicop     # the mask effect's 14 and V1's 2 captures, every logic-op path; 40 s
python tools/gpuspike.py logictest   # V10: an OR quad and an overlapping XOR through every route; 5 s
python tools/gpuspike.py ramdiff --set corpus,perfset,gpuset   # every copy's RAM, CPU against GPU; 20 s
python tools/gpuspike.py chain battle_4421 ... battle_4451     # V1's battle start, chained; 30 s
python tools/gpuspike.py time        # GPU and consumer ms a frame, perfbench before and after; 30 s
```

`selftest` runs `build/gpuspike/msvc/gpuspike.exe` twice, `--backend cpu`
and `--backend gpu`, into `build/gpuspike/msvc/cpu` and `gpu`, and judges each
scene's PNG. Away from edges, coverage must match exactly. An edge pixel is
one whose 3x3 neighbourhood in the CPU's image is not one colour (or, for
the gradient scenes, not all covered or all uncovered): Vulkan's top-left fill
rule and the CPU's inclusive one may disagree there. Colours must be exact,
or within one step where they are interpolated. Lines must be within one
pixel of each other, and points exact. The two render recipes of
`runtime/selftest.c` run in the driver on both, and the clip scenes check
the vertices the consumer uploaded against `gxr_clip_polygon`. Exit 0 is a
pass, 1 a failure and 3 a skip (no compiler, no `vendor/`, no Vulkan
device), with the reason printed. `--mutate unclipped` must fail, and the
test runs it. `--cc clang-cl` builds into `build/gpuspike/clang-cl/`.

`tevdiff` and `copydiff` are exact: zero mismatches is the only pass. `tevdiff`
builds each case with `tev_prepare` from random registers, runs it through
`tev_pixel` as prepared and with its fast shape off, and through `tev.glsl`,
and fails a path counted fewer than 100 times. `copydiff` makes the same
random copies -- every command format, intensity, half scale and filter, 200
rectangles each -- through the CPU renderer and through gxv in two processes
at once, and compares the bytes each left in RAM, the decoded image and a
screen copy, and that every refused copy left RAM alone. `--mutate clamp`
(tevdiff) and `rounding`, `intensity` and `unseeded` (copydiff) must each
fail, and the test runs them. What the scenes and differentials showed, and
every mutation turned red, is FINDINGS "V3". `loddiff` (V5) holds lod.glsl,
the fragment stage's level of detail, to the sampler's own level choice bit
for bit and to `span_lod`'s formula within 1/1024, the derivatives given to
both; `--mutate lod` and `lodmin` each fail their half (FINDINGS "V5, first").
`contrast` (V5) replays every capture through the spike and through `soa.exe --replay`
with `SOA_GPU=vulkan` and wants the same pixels. `live` (after V5) runs a scenario on the
CPU twice and the GPU once, all with `SOA_SEED`, and holds each GPU snapshot to the CPU's by
V0 -- only the frames the two CPU runs make byte for byte alike, since guest time follows the
host and an undrawn stretch is not reproducible; use `--range A-B`, which draws every frame,
and reproduced 81 of 81 and 111 of 111 (FINDINGS "V5, after"). `--mutate nofilter` fails it.

`oracle` needs the captures too (`build/fifo`, `build/perfset`). Each
reference is the spike's own CPU replay, made in the same session: a corpus
frame must hash to the manifest's FNV, a benchmark frame must be V0's
inspected reference byte for byte. The GPU frame is then judged by V0
(`tools/imgdiff.py`), its heat map beside it in `build/gpuspike/msvc/oracle/`.
With `--mutations`, each GPU mutation must fail V0 wherever it changes 0.5% of
a frame or more, on five captures or more; the exceptions are listed in
`GPU_BLIND_SPOTS` and `SHORT_OF_FIVE` with their reasons, and an unlisted one
fails. A capture whose frame copies to a texture is replayed from a scratch copy with 0xA5 over
every row of tiles it copies into (3.12's poison), so its samplers can only see this replay's
copies; a failure must be by design and bisected (`BY_DESIGN`, with `SOA_GXR_DRAWS=N`,
`SOA_GPU_DRAW=N` and `--dump-depth`). `logicop` holds native, blend and snapshot logic ops to
byte-identical frames; `ramdiff` traces every byte a copy writes differently to the EFB the copy
read; `chain` carries a frame's copies into the next. What they showed is FINDINGS "V4".

## 6. The decompilation check

```
python tools/decomp.py
```

**4.0 s.** Builds every unit in `config/GEAE8P/units.txt` with the Metrowerks
compiler it names, then compares every function and every data object in the
object file against the executable's bytes. Needs `vendor/mwcc/` (see
`python tools/fetch_toolchain.py`) and the extracted disc.

Today: **21 units, 83 functions and 17 data objects MATCH, nothing differs**,
and the run exits 0. Four more symbols are named and skipped rather than
counted — static helpers mwcc inlined, which have no counterpart in the
executable to compare with (`aramCacheIntact`, `aramCacheSum`,
`aramCacheSlotValid`, `pool_order_seed`).

```
== src\sdk\msl\string.c (mwcc 1.3.2)
strlen                   MATCH  7/7 words  (object 28 bytes, executable 28)
strchr                   MATCH  12/12 words  (object 48 bytes, executable 48)
...
== src\sdk\ar\arq.c (mwcc 1.2.5n)
fn_80243A34              MATCH  46/64 words, 18 need a link  (object 256 bytes, executable 256)
...
__ARQVersion             MATCH  0/4 bytes, 4 need a link  (data at 0x80346918 via fn_80243C04)
@1                       MATCH  69/69 bytes  (data at 0x802FB198 via __ARQVersion)
66 word(s) and 4 data byte(s) only a link can decide, across 7 otherwise-matching symbol(s)
...
7 unit(s) fully verified; 14 with words only a link can decide
```

One unit on its own:

```
python tools/matchcheck.py build/src/strcpy.o
```
```
strcpy                   MATCH  46/46 words  (object 184 bytes, executable 184)
```

### What MATCH means now

It means every word was compared and every word was decided. That is a stronger
claim than it used to be, and the difference matters.

The old oracle compared any word carrying a relocation on its **six-bit primary
opcode alone**, on the theory that the linker fills the rest in. Under that
rule a `bl` to the wrong function reported MATCH, and so did an address
materialised into the wrong register — in exactly the code where matching mwcc
is hardest. Now the linker fills in only the field the relocation names; every
other bit is the compiler's, and the field itself has a right answer whenever
the relocation's symbol has an address this project knows. So each relocated
word is one of three things:

- **verified** — the bits outside the field are identical *and* the field in the
  executable points where the relocation says it should. For a REL24 that means
  the opcode plus AA and LK plus the actual branch target; for a 16-bit
  relocation the whole first halfword; for an SDA21 the register too.
- **differs** — one of those is wrong. Reported, and the unit fails.
- **unverified** — "needs a link".

Data objects are compared as well, not just `STT_FUNC`. A unit's own globals
have no address until something places them, so their addresses are recovered
from the fields the executable already has: that is how `__ARVersion` leads to
the SDK banner it points at, and the banner gets compared like anything else.
Two references to one symbol must agree, and two symbols in one input section
must keep their distance, because the linker places an input section as one run.

`tools/tests/test_matchcheck_relocs.py` builds its own objects and its own
executable — no disc — and each of its 14 cases is a thing the old rule called a
match.

### What "needs a link" means

The bits outside the relocated field are identical, but **nothing here knows the
target's address**, so only a link could settle the field. It is never counted
as a match, and it is never counted as a failure either. The count is printed
per unit:

```
5 word(s) only a link can decide, across 3 otherwise-matching symbol(s)
```

In practice these are references to globals this project has no address for
yet. They cluster where you would expect: `src/sdk/ar/arq.c` has 66 of them
across 7 symbols because the AR queue is nothing but calls and global state,
while `src/sdk/msl/strcpy.c` and `strstr.c` have none at all — a leaf routine
that touches no global has nothing a link could decide.

A unit with any of them is reported as **unverified**, not **match**:

```
7 unit(s) fully verified; 14 with words only a link can decide
```

`--strict` makes those a failure — `python tools/decomp.py --strict` exits 1
here where the plain run exits 0 — and a checkout with a complete symbol
database should reach it. It is not the default, because today it would fail on
14 units that are almost certainly right.

### Exit codes

`tools/decomp.py`: non-zero if a unit fails to compile, any unit differs, or a
compiler named in `units.txt` is missing (**2**, and every unit that needs it is
named — the run was incomplete, not clean, which is why a checkout missing
1.2.5n does not silently report a short run). `tools/matchcheck.py` on one
object: **0** everything compared and verified, **1** something differs or a
symbol makes a claim about the executable that is wrong, **3** nothing differs
but something was left undecided. Both read the executable and never write it.

### A match is not a behaviour test

Matching bytes proves understanding of what the compiler did; it proves nothing
about whether the routine is called correctly, or whether the natively compiled
twin behaves the same on x86. Those are `tools/citest/dc_check.py` (section 1)
and the self test's case 73 (section 3). All three are needed: the string
comparison that returned positive where the answer was negative byte-matched
perfectly the whole time.

---

## 7. What needs what

| Check | Disc | MSVC | Metrowerks | Built `soa.exe` | Capture corpus | CI runs it |
|---|---|---|---|---|---|---|
| `pytest tools/tests` | no¹ | partly² | no | no | no | **yes**, Ubuntu + Windows |
| `ruff check` / `ruff format --check` | no | no | no | no | no | **yes** |
| `tools/guard.py` | no | no | no | no | no | **yes**, plus a full-history scan |
| `citest/compile_runtime.py` | no | yes | no | no | no | **yes**, Windows |
| `citest/dc_check.py` | no | yes | no | no | no | **yes**, Windows |
| `citest/render_check.py` | no | yes | no | no | no | **yes**, Windows |
| `citest/threads_check.py` | no | yes, or `--cc` | no | no | no | **yes**, Linux and Linux ARM64 |
| `citest/disc_check.py` | no | yes, or `--cc` | no | no | no | **yes**, Windows, Linux and Linux ARM64 |
| `citest/libm_check.py` | no | yes, or `--cc` | no | no | no | **yes**, Windows, Linux and Linux ARM64 |
| `SOA_MEMPOKE` tripwire | no | — | no | **yes** | no | no |
| `recompile.py --compile --link` | **yes** | yes | no | — | no | no |
| `SOA_SELFTEST=1` | **yes** | — | no | **yes** | no | no |
| `scenario.py run … --check` | **yes** | — | no | **yes** | no | no |
| `scenario.py replay` | **yes**³ | — | no | **yes** | **yes** | no |
| `imgdiff.py refs` and `mutate` | **yes**³ | — | no | **yes** | **yes** | no |
| `gpuspike.py selftest`, `tevdiff`, `copydiff`, `loddiff` | no | yes | no | no | no | no⁴ |
| `gpuspike.py oracle`, `time` | no | yes | no | no | **yes** | no⁴ |
| `decomp.py` / `matchcheck.py` | **yes** | no | **yes** | no | no | no |
| `audio_check.py` | **yes** | — | no | **yes** | no | no |
| `validate_assets.py` | **yes** | no | no | no | no | no |

¹ One test (`test_image_every_word_agrees`) skips without `extracted/sys/main.dol`.
² 421 of the 1360 skip here without a C compiler: 408 build runtime files or the GPU spike with MSVC and run them (one, `runtime/elfcheck.c`, to hold it to the Python), the two FMA probes want a clang, `test_mingw.py`'s archive test wants symbolic links, `test_window_sdl.py` wants an X display and SDL, and `test_seam.py`'s nine an ELF system, which no Windows run has.
³ `--replay` takes the capture as its argument, but `main.c` still opens the
disc directory.
⁴ It also needs `vendor/` (`tools/fetch_gpu.py`) and a Vulkan driver, which CI's runners lack;
CI runs the eleven tests of `test_gpuspike.py` that need neither.

### What CI can and cannot run

Nine job runs on every push and pull request:

| Job | Runner | Does |
|---|---|---|
| **Game data guard** | ubuntu | `tools/guard.py`, then every blob in the whole history against the same suffix list, then `tools/guard.py --history` over every path any commit touched and the bytes it held, then a 2 MiB blob-size ceiling |
| **Tests** | ubuntu | `pytest`, `ruff check`, `ruff format --check` — 723 passed, 383 skipped at 4441a80 |
| **Tests** | windows | the same three — 1102 passed, 4 skipped at 4441a80; the runner ships LLVM, so the FMA probes run |
| **Runtime compiles (MSVC)** | windows | `compile_runtime.py`, `dc_check.py`, `render_check.py`, and since L6 `libm_check.py` |
| **Runtime compiles (Linux, gcc)** and **(Linux, clang)** | ubuntu | the same three with `--cc gcc` or `--cc clang`, and `render_check.py` twice, at `--threads 1` and `--threads 4`, each asserting the renderer started that many workers: the first runs of `plat.h`'s POSIX half and of the twins where `long` is 64 bits (L4b); then `test_memguard.py` under `noskip` with `SOA_CC` set, and `threads_check.py`: `plat.c`'s SIGSEGV guard and the guest's 32 MB stack (L7); and `libm_check.py` (L6) |
| **ThreadSanitizer (render queue)** | ubuntu | clang `-fsanitize=thread`: `render_check.py --threads 4` and `queue_check.py --threads 1,2,3,4`, every race reported and any failing the run, no suppression (L8) |
| **Runtime compiles (Linux ARM64, gcc)** | ubuntu-24.04-arm | `compile_runtime.py`, `dc_check.py`, `render_check.py --threads 4` and `queue_check.py --threads 1,2,3,4` on ARM64, where a load the queue forgot to order can show (L8); and L7's two steps, where the guard learns a fault was a store from the ESR record in the signal frame; and `libm_check.py`, which prints the x86 legs' hashes (L6) |
| **Runtime compiles (clang-cl)** | windows | the same three and `libm_check.py` with `--cc clang-cl`, then `test_toolchain_fp.py` and `test_gxr_fastpath.py` with `SOA_CC=clang-cl`, where a skip fails the run — 5 passed on 2026-09-30, under LLVM's clang-cl 20.1.8 (the job prints its version) |

The Windows runner already ships VS 2022, and `tools/soa/toolchain.py` finds it
through `vswhere` and runs `vcvars64.bat` exactly as it does on a developer
machine, so CI and your build use the same compiler and the same flags. All
three scripts **fail loudly rather than skipping** when there is no `cl.exe`: a
check that quietly does nothing is worse than no check. The `native` job also
installs nothing with pip — `dc_check.py` imports `tools/recompile.py` for one
helper and that import graph is stdlib-only, which `test_citest.py` asserts
rather than leaving it to be discovered in CI.

The clang-cl job (portability L4a) is the same three scripts under the second
compiler, on MSVC's headers, libraries and linker. Its first step prints which
clang-cl and which version, and fails if there is none. The two pytest modules
it adds skip, rightly, on a machine with no clang-cl, no `llvm-objdump` or no
SSE4.1, so the job loads `tools/citest/noskip.py`, a pytest plugin under which
each skip is named and fails the run. `SOA_CC=clang-cl` is what makes
`test_gxr_fastpath.py` build with clang-cl; without it the module builds with
MSVC and passes either way. Every check step runs even after one fails. To
try the pytest step locally, point `SOA_CLANG_CL` at a clang-cl and put its
`bin` on `PATH`, because the second FMA probe looks for `clang` there:

```
$env:SOA_CC = 'clang-cl'; $env:PYTHONPATH = 'tools/citest'
python -m pytest -p noskip tools/tests/test_toolchain_fp.py tools/tests/test_gxr_fastpath.py
```

**CI cannot run:** linking `soa.exe`, anything under `gen/`, the device models,
the guest half of `runtime/selftest.c`, the mwcc byte-for-byte match, the replay
corpus, any scenario, and anything that reads `extracted/`. All of those need
the disc, the vendored compilers, or captures that are game data. A green tick
means the C compiles, those MSL routines behave like libc, and the rasterizer
still fills a triangle — nothing about the game running. **Sections 3, 4 and 5
are yours to run; nobody else can.**

### Other tools that check something

- `python tools/profile.py build/scenario-title.log` — reads the timing a run
  printed back and asserts the four properties a truthful profile has (busy plus
  idle comes to the pool's span within 5%, the table sums to 100%, and frames,
  guest seconds and wall seconds are three separate numbers). Two logs of the
  same scenario also compares their top tens. On the title run here: `8 threads
  x 70.72s = 565.76s against busy 10.90s + idle 554.86s = 565.76s, 0.0% apart`.
- `python tools/cardformat.py verify <image>` — re-runs the mount's own checks
  over a card image without launching the port, and ends `verdict: CARDDoMount
  would return 0 (READY)`. This is how the card the game wrote was confirmed to
  be one its own mount would accept.
- `python tools/audio_check.py build/opening.wav extracted/sound/<stream>.dsp` —
  cross-correlates a `SOA_WAV` recording against the decoded stream on the disc.
  Needs numpy, which `pyproject.toml` does not list. A stream path that is not
  there loose is read through `extracted/disc.iso`.
- `python tools/validate_assets.py` — decodes every AKLZ container on the disc,
  read through `extracted/disc.iso`, and asserts each yields exactly its
  declared size and contains no PowerPC code.

---

## 8. Before you push

In this order, because each one is cheaper than the next and the first failure
is the one worth reading:

```
python tools/guard.py
python -m ruff check tools
python -m ruff format --check tools
python -m pytest tools/tests -q
```

About a minute all told (`ruff ...` and `python -m ruff ...` are the same
thing; the second works whether or not the shim is on `PATH`).

**Run the format check as its own command.** `ruff check` passing feels like it
covered formatting; it does not, and that is what broke CI both times.
[CONTRIBUTING.md](../CONTRIBUTING.md) asks for the guard and the tests — the two
ruff commands are what CI adds on top, and the format one is the omission that
costs a red tick. If you would rather not think about it, run `python -m ruff
format tools` before the check: it is idempotent, and its diff is the fix.

If you touched anything under `runtime/`, add:

```
python tools/citest/compile_runtime.py
python tools/recompile.py --link
$env:SOA_SELFTEST='1'; gen\soa.exe extracted
```

If you touched `runtime/gxr*.c`, `runtime/gx.c` or anything a draw passes
through, add `python tools/scenario.py replay` — 18 seconds, and it is the only
thing that will tell you a pixel moved. If it reports `DIFFS`, look at the PNG
before you re-bless.

If you touched `src/`, `include/` or `config/GEAE8P/units.txt`, add
`python tools/decomp.py`; if you touched `config/hle.txt`, rebuild with
`--compile --optimize --link` and run the self test, because case 73 is what
checks the swap.

If you touched `config/scenarios/`, `tools/scenario.py` or `runtime/si.c`, run
one scenario end to end — `python tools/scenario.py run title --check`, about
70 seconds — because the pad grammar lives in two places and the tests only
check that they agree in the abstract.

---

Every figure on this page was measured in one sitting on one machine, so treat
it as a starting point and not as a contract: the checks are the contract, and
they are all in the tree. [PLAN.md](PLAN.md) says what is being worked on next,
[ROADMAP.md](ROADMAP.md) what is done, [FINDINGS.md](FINDINGS.md) what the
evidence behind a claim is, and [CONTRIBUTING.md](../CONTRIBUTING.md) how to get
from a clean machine to a running game.
