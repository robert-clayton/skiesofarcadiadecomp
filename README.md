# Skies of Arcadia Legends — Native PC Port

A native PC port of **Skies of Arcadia Legends** (GameCube, `GEAE8P`) built by
static recompilation, with progressive decompilation layered on top.

> **No game data lives in this repository, ever.** You supply your own dump of a
> disc you own. This repo contains only original tooling, runtime code, and
> analysis metadata.

## Why this game

Disc analysis (see [docs/FINDINGS.md](docs/FINDINGS.md)) turned up an unusually
favorable target:

| Signal | Value | Why it matters |
|---|---|---|
| REL modules | **none** | All code is in one DOL. Static recompilation sees the whole program. |
| Gekko-only instructions | **0.76%** | Dreamcast-lineage code barely touches paired singles or locked cache. |
| Function count | **7,144** | Smaller than *any* GameCube game yet fully decompiled. |
| Symbol map | absent | The one real gap — mitigated by SDK signature lifting (see spec). |

## Documents

- **[docs/SPEC.md](docs/SPEC.md)** — architecture and technical decisions
- **[docs/ROADMAP.md](docs/ROADMAP.md)** — work slices, milestones, current status
- **[docs/PLAN.md](docs/PLAN.md)** — what to pick up next, and how to know when it is done
- **[docs/PLAN-NEXT.md](docs/PLAN-NEXT.md)** — the order: what comes next across all the plans
- **[docs/FINDINGS.md](docs/FINDINGS.md)** — disc analysis evidence base
- **[docs/PROGRESS.md](docs/PROGRESS.md)** — what got built, in order
- **[CONTRIBUTING.md](CONTRIBUTING.md)** — clean machine to running game, and the rules

Licence: **[MIT](LICENSE)** over the original work here — the recompiler, the
runtime, the tooling and the prose. It does not and cannot cover `src/`, which
is written to reproduce the original program's compiled output, or `config/`,
which is analysis metadata derived from the game. [NOTICE](NOTICE) says which
is which and why. No game data is in this repository or its history; you
supply your own dump of a disc you own.

## Status

The game boots, plays its intro, reaches the title screen, starts a new
game and plays through the opening cutscenes, the first battle and into
the first fields (the Valuan ship's hold and the room the story carries it
to next: the stick, the minimap, menus and random encounters), with dialogue text, music and sound, rendered by a
software implementation of the GameCube's graphics pipeline in a window
with keyboard or gamepad input, on Windows and, since portability L10, natively
on Linux through SDL3 (checked in a container; nobody has played it there yet).
It mounts a memory card and the game
formats one itself, but no save has been written or loaded yet. See the
roadmap for what is done and what is not.

## Building and running

You need Windows, Python 3.14 (its standard library decodes zstd, which the RVZ
reader needs), the Visual Studio 2022 Build Tools with the C++ workload
(`cl.exe` need not be on `PATH` — the tools find it through `vswhere`), and
your own dump of the disc (`.rvz`, `.iso` or `.gcm`).

**PowerShell** — the default shell of Windows Terminal and of VS Code:

```powershell
pip install -e ".[dev]"
python tools/extract.py "path\to\Skies of Arcadia Legends (USA).rvz"
python tools/checkdump.py
python tools/recompile.py --compile --link
$env:SOA_RENDER = '1'
gen\soa.exe extracted
```

`--cc clang-cl` builds the whole game with clang-cl instead, into `gen\clang` and never `gen`:
`python tools/recompile.py --cc clang-cl --compile --optimize --link` makes `gen\clang\soa.exe` in
about a minute and a half, and it draws the 23 reference frames exactly as MSVC's build does (L3b).
`--cc mingw` needs nothing of Microsoft's at all: `python tools/fetch_mingw.py` fetches llvm-mingw
(pinned and verified, about 190 MB, into `vendor\`), then
`python tools/recompile.py --cc mingw --compile --optimize --link` makes `gen\mingw\soa.exe` in
about a minute and a quarter, with the mods' `mod.dll` under `gen\mingw\mods` (point `SOA_MODS`
there). It draws the 23 reference frames exactly as MSVC's build does too (distribution R1).
`python tools/player_build.py --disc <image> --root <folder>` is the player's build: it checks the
disc, extracts it, builds with llvm-mingw and no `src/`, and leaves `soa.exe`, the mods and `soa.ini`
in the folder; `python tools/package.py stage <folder>` lays out the package that carries it (R2),
and `python tools/package.py --out <dir>` zips it (R3), which `.github/workflows/release.yml` builds,
checks with `python tools/guard.py --tree` and leaves as a draft release for the owner to publish.
A player never types either: the package's `Setup.exe` (`tools/setup/`, R4) checks the folder,
builds from the disc image they pick with no console window, and starts the game.
`SOA_CLANG_CL` names the compiler; the Android NDK's
(`...\Android\Sdk\ndk\<version>\toolchains\llvm\prebuilt\windows-x86_64\bin\clang-cl.exe`)
is enough,
and either build's first lines say which compiler made it (`[boot] built with ...`).

**Linux** (portability L10) builds natively with gcc or clang, the window, pads and sound through
SDL3. `python3 tools/fetch_sdl.py` fetches SDL 3.4.18 (pinned) and builds it with CMake into
`vendor/sdl3`, in about a minute; without the development headers it needs it stops and names
Debian's packages (SDL's `docs/README-linux.md` has every distribution's). Then
`python3 tools/recompile.py --cc gcc --compile --optimize --link` makes `gen/linux/soa`, with the
mods' `mod.so` under `gen/linux/mods`, and `SOA_RENDER=1 gen/linux/soa extracted` plays in a
window, X11 or Wayland. Without `vendor/sdl3` the same build is headless: the self test, the replay
and `title --check` (all three pass, the replay 23 of 23 against the same manifest), and `SOA_WAV`.

**Android** is under way (specs/android.md). With the Android NDK installed (Android Studio's SDK
manager, or `SOA_ANDROID_NDK`), `python tools/recompile.py --cc android-arm64 --compile --optimize --link`
builds the game library a phone will load, `gen/android-arm64/libsoa_game.so`, in about a minute, and
checks it as the phone would; `--cc android-x86_64` builds it for the emulator. The app that loads it is
L12c's.

**cmd.exe** — the same commands, except for the line that sets the variable:

```bat
set SOA_RENDER=1
gen\soa.exe extracted
```

> **`set SOA_RENDER=1` is cmd.exe syntax and it does not work in PowerShell.**
> There `set` is an alias for `Set-Variable`, so the line quietly creates a
> *shell* variable whose name is the whole string `SOA_RENDER=1` and leaves the
> environment untouched. No error, no window: the port says `[run] headless;
> watchdog if no frame for 20s` and then goes quiet, playing to nobody, with
> the keyboard and gamepad unreachable because they are read through the window
> that never opened. It does not time out either — the watchdog fires only when
> no frame arrives for twenty seconds, and the frames keep arriving — so the run
> sits there until you Ctrl-C it.
>
> Use `$env:SOA_RENDER = '1'`; `gen\soa.exe --help` prints the same form. Every
> `SOA_*` variable below works the same way: `$env:NAME = 'value'` in
> PowerShell, `set NAME=value` in cmd.exe. To clear one again:
> `Remove-Item Env:SOA_RENDER`, or `set SOA_RENDER=` in cmd.

`extract.py` unpacks the disc into `extracted/` (gitignored, never committed)
and finishes by checking the executable it unpacked against the sha1 in
`config/GEAE8P/config.yml`. What it writes is `disc.iso`, the flat image the
port reads (1.46 GB), and `sys/`, four system files for the tools (3.3 MB);
the tools read any other file of the disc through the image. `--files` also
writes every file loose (another 1.42 GB, for browsing; nothing reads them),
and `--iso`, which older instructions give, is accepted and is now the
default. An `extracted/` from before this still holds 5,552 loose files:
`python tools/extract.py --prune-loose --dry-run` compares each one with its
slice of `disc.iso` and says what it would delete, and without `--dry-run` it
deletes those that match, keeps and names any that differ, and never touches
`sys/` or the image. `--store` writes `extracted/GEAE8P.soadisc` instead:
every byte of the disc to a mebibyte past its last file, with a SHA-1 of every
64 KiB block, judged against the whole disc's hashes pinned in
`config/GEAE8P/disc.yml` (a dump whose files differ is refused). It is an
integrity format for copying the game to another device, not a space saving
(2%). The port prefers it to `disc.iso` in the same folder, or takes it named directly, and
checks each 64 KiB block the first time it is read (`SOA_DISC_VERIFY`); `gen\soa.exe --check-disc
<store>` hashes all of it at once, exit 0 or 9 -- for a store just copied to a Deck or a phone.
`--check <store>` re-hashes one, `--compare <store> <iso>` holds it to the ISO
byte for byte, and `--sys-only <store>` writes `sys/` from it.
`checkdump.py` is that same check on its own, for
an `extracted/` that has been sitting around: it needs nothing but the files
already on disk, and it refuses — naming both hashes — if the dump is not the
build `config/` describes. The port checks it again every time it starts
(disc-layer I1): it opens only an image whose game id is `GEAE8P` and whose
executable has that same SHA-1, and refuses any other by name, so a patched or
other-revision image never runs under code translated from another.

`gen\soa.exe` takes the folder holding `disc.iso` or the image itself, so
`gen\soa.exe "D:\Dumps\Skies of Arcadia Legends (USA).iso"` runs straight from
your own ISO or GCM with no copy (an RVZ is converted by `extract.py`, which
says so). With no image it stops at once, exit 1, naming the command that makes
one; it used to boot and read zeros.

`recompile.py` translates the whole executable to C into `gen/` (also
gitignored), compiles it with MSVC and links the runtime into `gen/soa.exe`.
It builds your disc's executable, boot.bin and file table into it too (from
`--disc`, `extracted` by default), so the self test and replays need no image
and an image other than the one it was built from is refused at boot;
`--no-embed` builds without them, and the runtime then reads the image's.
**`gen\soa.exe` contains your copy of the game's executable. Never share it.**
The default compiles the translated code without optimisation (fast to
build, fine for testing); add `--optimize` for a release build of it, which
takes longer but runs the game faster. After changing anything under
`runtime/`, `--link` on its own is enough.
The function inventory it uses is in `config/` and is the only thing derived
from the game that the repository carries: addresses, sizes and names.

A quick check that a build is sane. It still loads the executable, so
`extracted/` has to be there, but it runs checks instead of the game and opens
no window:

```powershell
$env:SOA_SELFTEST = '1'
gen\soa.exe extracted
```

That runs 86 checks over the translated C library, the device models, the card
and SRAM, the AX mixer and the software renderer — including the 12
hand-decompiled functions the port runs natively, compared against their
recompiled twins on random inputs — and ends in `[selftest] 0 failure(s)`.

### Controls

Keyboard when the window has focus: arrows or WASD move the stick, IJKL the
C-stick; X = A, Z = B, C = X, V = Y, Enter or Space = START, R = Z, Q = L,
E = R, T/F/G/H = D-pad up/left/down/right, Escape quits (or, in fullscreen,
leaves it). F11 or Alt+Enter switches borderless fullscreen on and off, and so
does View+LB on a pad; the window can be resized and maximised, and the
picture stays whole-pixel sharp with black bars round it. An XInput gamepad
works as you would expect (triggers are L/R, the right shoulder is Z), in any
of the four slots. The
keyboard is read only while the window is in front, so switching to another
window releases every key; the gamepad keeps driving the game whatever has the
focus.

To record a session and play it back:

```powershell
$env:SOA_RENDER = '1'
$env:SOA_PAD_RECORD = 'build\play.pad'
gen\soa.exe extracted
```

Play, then close the window. The file is a line per change, small enough to
read and to cut by hand at the point you wanted to reach. Replay it with the
same switches set, adding `$env:SOA_PAD_FILE = 'build\play.pad'` and clearing
`SOA_PAD_RECORD` (`Remove-Item Env:SOA_PAD_RECORD`); the run ends by itself
120 frames — about four seconds of game time — after the last line. Two
caveats worth knowing before a long session: input during a frame that lasts
seconds (a load) reaches neither the game nor the file, and the replay only
keeps step with the recording while frames arrive at the same rate, which is
why the run prints how far it has drifted.

### A settings file, for starting it without a terminal

`soa.ini` in the repository's root folder (the one holding `gen\` and
`runtime\`; the file beside the executable, `gen\soa.ini`, is read if there
is none there) holds the switches a player would set, one `key = value` a
line (`#` starts a comment). With a `soa.ini`, double-clicking `gen\soa.exe`
is enough: relative paths in it mean the root folder, the card defaults to
`build\cards\slotA.raw` and the disc to `extracted` there, the game is drawn,
and the console's log goes to `build\logs\` instead of a window:

```ini
disc = C:\Games\Skies\extracted   # a folder holding disc.iso, or the .iso; used when none is given
render = 1
scale = 3
mods = C:\Games\Skies\mods
```

The keys are `disc`, `render`, `gpu`, `gpu_scale`, `window`, `scale`, `threads`, `mods`, `card`,
`record` (`SOA_PAD_RECORD`), `nosound`, `uncap`, `seed`, `encounters`,
`encounters_hold_b`, `autotext`, `coop`, `rumble`, `fullscreen`, `scaler`, `unfocused`, `deflicker`, `turbo`, `gamma`, `colorblind`, `colorblind_mode` and `flash_limit`, each standing for the switch below. A key
that changes what the game does (`seed`, the two `encounters` keys,
`autotext`, `coop` and `turbo`) is also written into a pad recording's `# config` line when it is
in effect. The `encounters` keys, `autotext` and `coop` are read by the mods
`encounter-rate`, `autotext` and `coop`, not by the port, so they need `mods =
...\mods` too; set without it, the port says the key does nothing. A variable set in the environment always wins over the file, and
the port says so; a key it does not know is reported with its line and ignored.
The checks -- `scenario.py`'s runs and replays, `perfbench.py` and the self
test -- run with the file off (`SOA_SETTINGS=0`), so a player's settings never
move one.

### Saves to and from Dolphin

A `.gci` is one save the way Dolphin's memory-card manager imports and exports
it. `tools/cardformat.py` moves one between a `.gci` and a card image:

```powershell
python tools/cardformat.py export build\cards\slotA.raw --name SA_LEGENDS.000 save.gci
python tools/cardformat.py import build\cards\slotA.raw save.gci --out build\cards\new.raw
python tools/cardformat.py import build\cards\slotA.raw save.gci --in-place --replace
```

`export` takes the file by `--name` or by `--index` (the entry number `show`
prints); with no output named it writes beside the card, named the way a
Dolphin GCI folder names it (recalled from Dolphin, not yet checked against its
source). `import` writes a new image by default (`--out`, else the card's name
with `-imported`). `--in-place` rewrites the card itself, first copying it to
`slotA.raw.bak-YYYYMMDD-HHMMSS`, and refuses while `soa.exe` is running,
because the port keeps the card open and writes through. It refuses, writing
nothing, a save of another game, a file that is not a `.gci`, a card without
the blocks or a free entry for it, a name already on the card (`--replace`
deletes that file first), and a card that does not verify; then it verifies
what it wrote and exits 1 unless the game would mount it. A `.gci` is game
data like the card: the guard refuses the suffix, so keep them outside the
repository or under `build\`.

### Useful environment variables

Set these the way your shell sets environment variables: `$env:NAME = 'value'`
in PowerShell, `set NAME=value` in cmd.exe. Quote the value in PowerShell —
`$env:SOA_PAD = '9000:sup#120'` — because an unquoted `#` starts a comment and
an unquoted path with a space in it is two arguments.

| Variable | Effect |
|---|---|
| `SOA_RENDER=1` | render (and open the window) |
| `SOA_GPU=vulkan` | the GPU draws the picture (specs/gpu-backend.md V5; `gpu` in `soa.ini`), where this build has the backend -- `python tools/fetch_gpu.py`, then `python tools/recompile.py --link`, which then says `GPU backend: built in` -- and there is a Vulkan driver. It says `[gxv] Vulkan ... on <device>` when it starts and `[gxv] fallback: <why>` when it cannot, and then the CPU draws. The GPU draws on a thread of its own (V6a); the picture is judged against the CPU's by a tolerance, not a hash, and the pinned frames are checked with it off. `SOA_GPU_SCALE=2|3` (`gpu_scale` in `soa.ini`) draws the picture at two or three times the console's 640x528 and shows it sharper (V9a; `SOA_GPU_SCALE_LOD=native` picks mip levels as at 1x, the check's switch), `SOA_GPU_DEVICE=n` picks a GPU, `SOA_GPU_LOGICOP=native|blend|snapshot|interlock` the logic-op path (by default native, or, where the device lacks `logicOp`, chosen for each draw: V10), `SOA_GPU_FEATURES=core` every optional GPU feature treated as absent, as an Android GPU may lack them, `SOA_GPU_EDS=0` cull, topology and depth built into each pipeline rather than set by draw, `SOA_GPU_LOADER=path` another Vulkan loader, `SOA_GPU_VALIDATE=1` the validation layer where installed (the Vulkan SDK), each of its warnings and errors a `[gxv] validation` line, `SOA_GPU_PIPELINES=path` where the pipeline cache is kept (`build/gxv-pipelines.bin` by default; `off` for none), `SOA_GPU_SPECIALIZE=1|wait|0` pipelines specialised on the TEV's shape made on a thread of their own while the interpreter draws (the default), made on the draw path, or never |
| `SOA_SCALE=n` | the window's starting size, n times 640x480; by default the largest whole multiple whose window fits the monitor's work area |
| `SOA_FULLSCREEN=1` | start in borderless fullscreen (`fullscreen = 1`); F11, Alt+Enter and View+LB toggle it |
| `SOA_DEFLICKER=0` | the picture without the game's deflicker: the copy to the screen blends each row with the rows above and below it, which suits an interlaced television and softens a progressive display; `0` copies it unfiltered (`deflicker = 0`, P5b). The game's own copies to textures keep their filter. A replay with it set hashes differently from the manifest, by design |
| `SOA_SCALER=integer\|fit` | how the picture fills the window (`scaler`): `integer`, the default, the largest whole multiple of 640x480, sharp, with black bars; `fit`, the largest 4:3 that fits, nearest neighbour, uneven pixels (H19a) |
| `SOA_WINDOW_TEST=fs@N,win@N,size:WxH@N` | a check's knob: at each presented frame named, fullscreen, windowed or a client size, then one `[window] frame F: client WxH, image WxH at +X+Y, ...` line; `python tools/tests/test_picture.py <log>` checks those lines (H19a) |
| `SOA_WINDOW=0` / `=1` | force the window off (render headless) or on |
| `SOA_PRESENTER=gdi` | show the window with GDI on an 8 ms poll instead of the DXGI flip-model presenter, which holds each frame for a whole number of the display's refreshes (2 at 60 Hz, 4 at 120; the next refresh at other rates). With `SOA_GPU=vulkan` the GPU presents its own picture through a Vulkan swap chain, paced the same way, unless `SOA_PRESENTER` is `dxgi` or `gdi`; P5a's picture filters run on the GPU at the size it draws, the flash limiter deciding its blend on the CPU (V8b). The report ends with a histogram of present intervals either way |
| `SOA_FRAMES=n` | run n video frames (numbered 0..n-1), then stop and print the report |
| `SOA_GAME=path` | a split build only (`recompile.py --cc gcc --split`, Android's arrangement on Linux; specs/android.md L12a): the game library to load, in place of `libsoa_game.so` beside the launcher. It is checked before it is loaded, and refused in words if it was built for another machine, another page size or other sources |
| `SOA_SNAP=n` / `SOA_SNAP=n@A-B` | write `build/frames/NNNN.png` every n frames, or every nth from frame A to B; needs `SOA_RENDER=1`. With no window open it also skips rasterizing the frames it is not writing, so the game runs at full speed between them |
| `SOA_FRAMES_DIR=path` | where `SOA_SNAP` writes instead of `build/frames`, made if missing. Every run shares `build/frames`, so a job whose snapshots will be judged afterwards (`tools/soak.py check --frames`) needs its own |
| `SOA_PAD2=frame:buttons,...` | a script for controller port 2 in `SOA_PAD`'s grammar, for checks: port 2 is read by mods (`read_pad` in `runtime/soa_mod.h`, for couch co-op) and never by the game, which still sees one controller. In play, port 2 is the next connected XInput pad after port 1's (P10a) |
| `SOA_PAD=frame:buttons,...` | scripted controller for headless runs, e.g. `1700:start,1800:a`; `+` combines (`1800:a+sup`), `3600:a@150` repeats A every 150 frames, `9000:sup#120` holds the stick up for 120 frames; `lb`, `view`, `ls` and `rs` are the host buttons, which never reach the game (`1700:view+lb` is the fullscreen chord) |
| `SOA_PAD_RECORD=path` | write down every controller input the port reads, keyed by frame, one line per change; play in the window, then replay it. An existing file is never overwritten — the run records to `path.1` instead |
| `SOA_PAD_FILE=path` | replay a recording instead of live or scripted input (it becomes the whole input; `SOA_PAD` is then ignored). **Replay in the configuration you recorded in**: the recording is keyed by frame, the game runs on guest time, and the two only keep step while frames arrive at the same rate, so `SOA_SPEED`, `SOA_RENDER`, the window, the thread count and the memory card all have to match. The run says what it was recorded with and how far it has drifted |
| `SOA_PAD_STOP=n` | frames to keep running after a replayed recording runs out, then stop (default 120; 0 keeps going) |
| `SOA_DISC_VERIFY=hash\|iso[:path]\|all[:path]\|0` | how a disc store is checked as it is read (disc-layer I5): `hash`, the store's default, hashes each 64 KiB block the first time a read touches it and stops the run (exit 9) naming the file if it is not what was imported; `iso` reads every read again from `disc.iso` (or `path`) and names each one that differs; `all` both; `0` neither. An ISO has no hashes, so asking it is refused |
| `SOA_DISC_FLIP=<path>\|0x<offset>` | a check's knob: one byte of a store, a file's first or the offset, read with its low bit inverted, as damage on the disk would be; both checks must catch it, and the report says if it was never read |
| `SOA_DISC_LOG=1` | one `[disc]` line per read of the drive: the frame, the offset, the length, the file the read starts in and how far past its end it reaches; `python tools/citest/disc_check.py --log <log> --data extracted` holds every line to the disc's file table (disc-layer I1) |
| `SOA_SPEED=n` | run guest time n times faster than the wall clock (headless exploration; sound will not keep up) |
| `SOA_UNCAP=N` | from frame `N` on (`1` is from the start), let the frame end's spin go after one field instead of two: `runtime/tick.c` answers `VIGetRetraceCount` with the frame's start plus one at the spin's call site. The game's logic advances once a frame, so this runs the **whole game** up to twice as fast — it is not 60 fps. Start it after a pad script has reached its scene: disc loads run on the wall clock, so uncapped from boot the script's presses land somewhere else. For measuring how fast the port can go (`docs/PLAN-60FPS-MODS.md` H3), not for play. Every run's report also ends with a `[frametime]` line: frames a second, the median, 95th and 99th percentile and worst wall milliseconds per frame, and the process's CPU seconds; with an uncap, over the frames from `N` on. The `[tick]` line counts the main loop's safe points (one per frame, less the frame shown before the loop starts) and how often the spin was let go |
| `SOA_GX_DLLOG=1` | a diagnostic: every move of the CPU FIFO away from the command processor's (the game recording a display list), the draws parsed while it was away, and each display-list call with its size; the report adds a table per buffer (PLAN C5a, FINDINGS "Recorded display lists"). Changes nothing drawn |
| host buttons and chords | the pad's LB, View (Back) and stick clicks never reach the game: they are the port's (PLAN-GAMEPLAY-MODS CH1). View+LB toggles fullscreen, View+RS turbo (`SOA_TURBO`, M11a) and View+LS the menu, which logs `[chord] frame F: ... (not built yet)` until its slice lands; LB alone is left to mods (`host_buttons` in `runtime/soa_mod.h`). The keyboard's Tab counts as LB. A pad script names them `lb`, `view`, `ls`, `rs`; a recording notes each chord as a `# chord` comment. Port 1 is the first connected XInput pad, in any slot |
| `SOA_TURBO=battle\|sky\|both` | up to twice as fast in battles, in the sky, or both (PLAN-GAMEPLAY-MODS M11a; `turbo` in `soa.ini`, recorded): battle is scene 7, and scene 6 on a map of 500 or more (the ship battles); sky is the sky-mode word `0x80347464` at 1, the world map and the ship-piloted maps. It is decided at the loop's safe point and kept for the frame; when on, the frame end's spin goes after one field, as `SOA_UNCAP`'s does. View+RS toggles it from the next safe point (`[turbo] frame F: on`), and with no `SOA_TURBO` the chord arms both. The music keeps real time. While it is on the window holds each image `present_interval(hz, 60)` refreshes, not `(hz, 30)` (`[window] frame F: turbo on, each frame held N refresh(es)`), and the report counts the frames at turbo. `python tools/tests/test_turbo.py <log>` holds a run to it. Drawn every frame, a battle at turbo makes about 43 images a second in a window on this machine, not 60: the full 2x in a window waits on M11a-skip (FINDINGS "M11a") |
| `SOA_ENCOUNTERS=off\|half\|normal\|double` | how often random battles come in the field, read by the `mods/encounter-rate` mod (`SOA_MODS=mods`, PLAN-GAMEPLAY-MODS P1): the game's encounter multiplier, the signed byte at `0x8030B7AD` (-1 the normal rate, else the odds times byte/50), is written every frame in the field from the base the game itself works out -- 50, or the effect-84 value of the party's accessory (210 doubles battles, 211 cuts them to a tenth) -- as base/2 at `half`, min(base x 2, 127) at `double`, 0 at `off`; `normal` or unset writes nothing. The byte lies below the saved party block, so nothing reaches a save. The report's `W write(s)` counts the mod's writes |
| `SOA_AUTOTEXT=on\|N` | dialogue that turns its own pages, read by the `mods/autotext` mod (`SOA_MODS=mods`, PLAN-GAMEPLAY-MODS P11): in the field, a complete page (the message window's state 4 at `0x80346E64`) that the game would otherwise wait on gets one A after 45 frames (`on` or `1`) or N frames, two frames down and two up; never in a choice box, never on a page the game turns itself, never over the person's own A or B. Holding LB (or Tab) skips: A two frames down and two up in states 3 and 4 while it is held (P11b; a replay does not skip, since host buttons are not recorded yet). `0` or unset is off. One `[mod] autotext: frame F page advanced after W frames` line per press, and `skip press in state S` per skip |
| `SOA_COOP=1\|1,3` | couch co-op in battle, read by the `mods/coop` mod (`SOA_MODS=mods`, PLAN-GAMEPLAY-MODS P10b): while a battle is taking the party's commands (scene 7, the phase word `0x8034733C` at 1) and the member choosing (the word at `0x80347330`) is one of the party slots given, 0 to 3, the game's port 1 reads pad 2 -- the next connected XInput pad, or `SOA_PAD2` in checks -- in place of player 1's pad. When the turn passes between the pads, the incoming pad is read as let go, sticks and triggers too, until it has no button down, so a thumb resting on A cannot confirm the next member's command. With pad 2 absent port 1 plays every member, said once. The mod logs each handover, each press forwarded from pad 2, and each member's command type when the party's input ends. Recorded only as slots the mod takes (`1, 3`, with its space, is refused and not recorded). Pad 2 is not in recordings yet, so a co-op session does not replay: port 1's recorded presses would take pad 2's turns. `python tools/tests/test_mods.py p10b <log> <recording>` checks a run (FINDINGS "P10b") |
| `SOA_ENCOUNTERS_HOLD_B=0` | turn off holding B for no random battles (on by default with the `encounter-rate` mod): while B is held in the field the multiplier reads 0, and at `normal` the first frame after B is let go puts the game's own value back, once |
| `SOA_SEED=n` | a 32-bit number, decimal or `0x` hex: the game's three reseeds of its random numbers -- every field load and the two at every battle start, OSGetTick called from `0x801012B0`, `0x8000A1D0` and `0x8000A1D8` -- take values made from it (`runtime/seed.c`), so a run with the same seed starts every battle from the same seeds; every other timebase read is untouched. The report counts the pins per site, each pin prints `[seed] pin: site S (lr X) n N -> 0xV` (`python tools/tests/test_seed.py <log>` checks a run by those lines), and a recording's `# config` line ends `seed=n` with the seed in decimal, or names none when the value was refused (PLAN-GAMEPLAY-MODS P6). Whether the same seeds give the same battle is K6's question |
| `SOA_FRAMETIME_FROM=N` | start the `[frametime]` record at frame `N` without uncapping: a capped run's figures for the same stretch an uncapped one reports |
| `SOA_FIFO_DUMP=n,n` | capture those frame numbers; each lands as `NNNN.fifo`/`.regs`/`.ram`, zero-padded to four digits, for `gen\soa.exe --replay build/fifo/NNNN` |
| `SOA_FIFO_DIR=path` | where those captures go (default `build/fifo`, the corpus `config/fifo_manifest.tsv` pins; capture somewhere else) |
| `SOA_GAMMA=g` | the picture lighter (above 1) or darker (below), 0.5 to 2.5: each channel through `255 (v/255)^(1/g)` (PLAN-GAMEPLAY-MODS P5a) |
| `SOA_COLORBLIND=off\|protan\|deutan\|tritan`, `SOA_COLORBLIND_MODE=correct\|simulate` | colours corrected for that colour blindness -- the daltonize redistribution of what Machado's model (2009, severity 1) says is lost -- or, with `simulate`, shown as it would see them; worked in linear light, then gamma (P5a) |
| `SOA_FLASH_LIMIT=1` | no more than a quarter of the picture flashing more than three times in any second, as WCAG 2.3.1 and Xbox guideline 118 count flashes, pixel by pixel: a move of relative luminance of 0.1 or more from where the pixel last turned, the darker below 0.8, then the opposite move. A pixel that ends a fourth flash in a second counts against the quarter for a second after, and a frame that would take the pixels over it to a quarter is blended toward the one shown before, as far as it can go without (P5a). The log names each frame held (the first 20); the report counts them and gives the largest share of the picture that was over. It damps the opening's fast flight through cloud (FINDINGS "P5a") |
| `SOA_PICTURE_SIZE=WxH` | the size of the `<base>.picture.png` that `--replay <base>` writes when any of the keys above is set, at any value: the frame through them and the scaler, as a window that size would show it (default 1920x1080). With every key at its identity value and `640x480` it is the frame itself, pixel for pixel -- `python tools/tests/test_picture.py --same <base>.png <base>.picture.png` says so. `<base>.png` stays the frame the hashes pin. In a window the `[picture]` line names the filters in effect, and the report gives the present's own work, the frame to the back buffer, at p50, p99 and max (`[present] the work of a present`) |
| `SOA_THREADS=n` | rasterizer worker threads, default three quarters of the logical CPUs (12 of 16), which was fastest in the heaviest field scene measured (FINDINGS "H15c") |
| `SOA_NOSOUND=1` | no audio device |
| `SOA_UNFOCUSED=run\|mute\|pause` | with another window in front: `run` (the default) carries on as before; `mute` silences the game and ignores the pad until the window is back in front (M5b); `pause` holds the game, and its clock with it, until then (M19) |
| `SOA_CLOCK_GAP_MS=n` | the guest's clock (M19, `runtime/clock.c`) counts a gap of more than n ms between two reads -- a host asleep, or stopped in a debugger -- as no time, logging `[clock] frame F: a gap of S s wall counted as 0 s of guest time`; default 2000, 0 turns the rule off. The `[run]` line ends with the gaps and the seconds excluded. The system's own sleep holds the game outright. 250 ms, the first default, was too tight: the port's own work on a loaded host -- a snapshot frame drawn after 99 undrawn ones -- held the guest thread up to 1.8 s |
| `SOA_CLOCK=utc` | the clock before M19, wall time since the first read times `SOA_SPEED`, for bisecting a timing report; for one release |
| `SOA_CLOCK=fixed` | the fixed-step clock ([docs/CLOCK.md](docs/CLOCK.md)): guest time moves only with the guest's own work -- `SOA_CLOCK_READ_TICKS` (default 8) a timebase or hardware read, `SOA_CLOCK_POLL_TICKS` (default 32) a delivery point at a loop's back edge, and to the next deadline when the scheduler is idle -- and never with the host's clock or `SOA_SPEED`, so the same pad script, card image and build give the same frame hashes and call counts on every run, at any speed and load. The RTC starts at `SOA_CLOCK_RTC` seconds past 2000 (default 31622400, 2001-01-01) and counts guest seconds. The report adds `[clock] fixed: ...` |
| `SOA_STALL=frame:seconds` | a check's knob: the guest thread sleeps once, at that frame's end, the way a host that went to sleep would stop it (M19) |
| `SOA_ROOT=dir` / `SOA_SETTINGS=file` | for tests: the port root (by default the exe's folder, or the parent of the `gen` folder beside `runtime\`), and a `soa.ini` to read in place of `<root>\soa.ini` |
| `SOA_RUMBLE=0..100` | how hard the pad rumbles when the game asks (PADControlMotor), as a share of full; default 100, 0 is off. The motor turns only with a window open and no `SOA_PAD` script or `SOA_PAD_FILE` replay driving the input, and stops when the window loses focus or closes and at the end of a run (PLAN-GAMEPLAY-MODS M18) |
| `SOA_WAV=file.wav` | also write everything the game plays to a WAV file (works headless and with `SOA_NOSOUND`). The report's `[audio]` lines count the blocks the device played and dropped -- drops come in runs, a burst after a host stall the queue cannot hold -- and the rate blocks arrived at in wall time: 128,000 bytes a second is the console's 32 kHz stereo, and `SOA_SPEED=n` makes it n times that. `[dsp] AI DMA control writes` should read one start and one write while running per block (FINDINGS "The AI DMA's pace") |
| `SOA_WATCHDOG=s` | stop after s seconds with no video frame and print a report (default 20 headless, off when a window is open; 0 disables) |
| `SOA_POKE=f:a=v[,f:a=v]` | store the 32-bit value `v` at guest address `a` at the end of frame `f`, once, printing what was there before. The one way to answer "what does the game do if this variable says that" without a recompile. Fires on the first frame at or after `f`, so a skipped frame number does not silently lose the poke. Up to 256 items, which is 85 field warps at three words each; a mistyped item stops parsing and says so rather than driving a run that looks like it ignored you. The field's own map identity is `0x80311AC4` (map number), `0x80311AC8` (map letter in the top byte) and `0x80311AEC` (field state, 8 is the steady update). A warp is the destination's script name, e.g. `ME103A.SCT`, as three words at `0x80305CF0`, then 15 in `0x80311AEC`: the game's own warp, no button; also set `0x8030E420` (`sys[15]`, where the party came from) to 0 so the map takes its default entrance, and do not poke the map words (`HANDOFF.md` has the command). Numbers are decimal or `0x` hex and must fit 32 bits; a sign, a space or a leading zero is refused rather than read some other way |
| `SOA_MODS=dir` | load every mod in `dir`: each folder holding a `mod.ini` (`manifest = 2`, a stable `id` and a `version`, which a recording names it by as `id@version`, `name`, optional `authors`, `api`, the least mod API it needs, and `dol_sha1`, the SHA-1 of the DOL it was made for; a `mod.ini` without `manifest` is read as version 1, with no id; two mods with one id are refused; a key beginning `x_` is noted and passed over, any other unknown key refuses the mod) and a `patches.txt` of `trigger 0x80346d28 = value [when scene=N state=N map=NNNx]` lines, where the trigger is `every_frame`, `once` or `on_map_load`. A patch writes one 32-bit word at the end of a frame. A mod with any fault — a misspelled address, one in the hardware window, outside RAM, unaligned or inside the game's code, an unknown key, another DOL — is refused whole and says which line. The report ends with how often each patch applied, and a recording names the mods it was made with. `examples/mods/encounters-off` is an example (`docs/PLAN-60FPS-MODS.md` M1), and `mods/` holds the mods the port ships: `mods/encounter-rate`, `mods/autotext` and `mods/coop` (below). A mod may also, or instead, hold a `mod.dll`: native code on `runtime/soa_mod.h`, exporting `soa_mod_init`, loaded once `mod.ini` checks out -- see `examples/mods/map-log` for the template and its build line (M3). Its `pad_filter` sees every controller read after `SOA_PAD_RECORD` has its copy, so a recording made with a mod replays with it -- unless the mod reads what a recording does not hold yet, as `mods/coop` reads pad 2 -- its `projection_filter` sees each new projection, its `texture_provider` may replace any texture, by its content hash, with an image of any size, and from a safe-point callback its `call_guest` may call the game's own functions. Mods load under `--replay` too, which renders a captured frame through them deterministically |
| `SOA_MEMPOKE=a,b` | store a word at each guest address before the game boots and read it back; an address past the console's 24 MB, e.g. `0x81800000`, is how to fire the out-of-range tripwire on purpose (needs no disc) |
| `SOA_STRICT=1` | stop at the first hardware access outside the modelled range, with a guest backtrace (almost always a garbage pointer); without it the first twenty are reported and the run goes on |
| `SOA_HASH=1` | print an FNV-1a hash of every frame the port presents (what `tools/scenario.py replay` compares) |
| `--replay A B` | render two consecutive captures, A to `A.png` and B to `B.png`, then the image halfway between them to `B.mid.png`: every draw of B matched in A has its positions interpolated, the rest are drawn as B draws them (PLAN-60FPS-MODS H10). `SOA_PAIR_T=t` (0 to 1, default 0.5) picks another point, `SOA_PAIR_LIST=file` writes the matched pairs, and `python tools/midpoint.py` runs every check on the five pairs in `build/perfset` |
| `SOA_MMIO=1` | log the first few accesses of every hardware register as they happen |
| `SOA_SELFTEST=1` | run the library and device checks instead of the game |
| `SOA_PROFILE=n` | `0` turns off the end-of-run sampling profile (one fewer thread); `n>1` shows n rows |
| `SOA_COUNT_CALLS=1` or `=file.tsv` | count every call of every translated function (docs/SWAP.md); the report's `[swap]` line says how many were called and how many still ran translated, and a path also gets every count, a line a function (address, calls, answered, translated). Costs about a tenth of the run's speed while on (9% over the opening) |
| `SOA_SYMBOLS=path` | function names for that profile (default `config/functions.tsv`, found by running from the repository root) |
| `SOA_CARD=path` | memory card image for slot A (default `build/cards/slotA.raw`, created blank on the first write; `tools/cardformat.py write` makes one the game will mount) |
| `SOA_CARD_VERBOSE=1` | one `[card]` line per EXI transaction: the frame, the device, the command and the card address |
| `SOA_TRACE=1` / `SOA_TRACE_DUMP=1` / `SOA_WATCH=addr,len` | tracepoints from `config/trace.txt`; 12 words of memory at each hit; a store watchpoint |
| `SOA_PEEK=addr@N[-M][,...]` | print the word at guest address `addr` at the end of frame `N`, or of every frame from `N` to `M`, with the game's retrace count (`[peek] frame F: ADDR = VALUE (retrace R)`); fires before `SOA_POKE` in the same frame, so it reads what the game wrote. Up to 256 items, a list of its own. The way to time something in fields, e.g. a fade at 0x80347510 |
| `SOA_WATCH_FROM=N` | start `SOA_WATCH` at the game's frame `N`: earlier stores are neither printed nor counted against its 201 lines. Every `[watch]` line ends with the frame it happened in |
| `SOA_GXR_DEBUG=N` / `SOA_GXR_DRAWS=N` / `SOA_GXR_PIXEL=x,y` | renderer forensics in `--replay`: triangles from draw N on, stop after N draws, narrate one pixel |
| `SOA_TEXVERIFY=1` | hash every texture on every lookup, as before PLAN-60FPS-MODS H12, and count the textures whose bytes changed inside one texture epoch: a rewrite with no texture-cache invalidate (BP 0x66), EFB copy or frame end since the last hash, which the renderer would otherwise draw from its old decode. The report's `[gxr] textures:` line gives the count, and the first eight are named |
| `SOA_GXR_DRAIN=1` | order EFB copies the way the renderer did before PLAN-60FPS-MODS H14: a full drain before and after every copy that reads rows other workers own (every copy the game makes), and for every texture read from a queued copy. The fallback if a scene draws wrong with H14's fences, and the oracle they are tested against |
| `SOA_GXR_TOKENWAIT=1` | make a draw token (`GXSetDrawSync`) wait for the copies before it. Off, a token is answered as it is parsed, as it always has been, and one that arrives while a copy is still running is counted in the report; on, a game that reads a copy's memory right after its token sees it finished, at the cost of H14's gain (this game's tokens sit at the top of each frame) |
| `SOA_HOSTPROF=1` | sample the instruction pointer of every rasterizer worker and of the guest thread about once a millisecond and end the report with where their time went, by function and by source line (through `gen/soa.pdb`, which `--link` writes): the host code under a translated function -- a memory accessor, a paired-single helper -- that `SOA_PROFILE` charges to the guest function. The workers' table also folds by the innermost inlined function, so the pixel path's `__forceinline` helpers each get their own row and line. For finding what to make faster; a profiled run is for reading, not for timing |
| `SOA_GXR_NOSIMD=1` | the pixel path's SSE4.1 code (the bilinear blend, H15d) takes its scalar loop instead, which is also what a CPU without SSE4.1 gets; the two give the same bytes (`test_gxr_fastpath.py`) |
| `SOA_REPLAY_REPEAT=N` | `--replay` renders the capture N times, each from its own RAM image: a frame long enough for `SOA_HOSTPROF` to sample |
| `SOA_GXR_STALL=w:k:us[,...]` | a test knob: rasterizer worker `w` waits `us` microseconds before every command of kind `k` (0 a draw, 1 a copy, 2 a clear), which turns an ordering race between the workers into a certain failure (`tools/tests/test_gxr_overlap.py`). The run says it is a test |
| `SOA_GXR_LIGHTS=N` / `SOA_GXR_NOTEX=1` / `SOA_CULLFLIP=1` | more renderer forensics: dump the lighting setup of the first N draws, draw every texture flat grey, reverse the winding the rasterizer culls by |
| `SOA_NOAX=1` / `SOA_AX_VERBOSE=1` / `SOA_ARAM_VERBOSE=1` | audio forensics: skip the AX mixer entirely, one line per voice command, one line per ARAM DMA |
| `SOA_PACE=1` | yield to the host scheduler on every interrupt delivery (a loaded machine runs the guest more evenly) |
| `SOA_TSC=0` | time the renderer with `QueryPerformanceCounter` instead of the time-stamp counter |
| `SOA_QUIET_GUEST=1` | drop the game's own printf output |

## Checking it still works

```powershell
python -m pytest                     # 1360 tests; any that need a dump skip themselves
python -m ruff check tools           # lint and format both gate CI, and the
python -m ruff format --check tools  #   format one has broken it twice
python tools/checkdump.py            # the dump is still the build config/ describes
python tools/scenario.py list        # the 13 scripted runs this port is judged by
python tools/scenario.py run opening --check
```

A scenario is a `.scn` file in `config/scenarios/`: the environment it sets,
the scripted controller input, what the run should show, and the evidence
behind each claim. `show` prints one, along with the exact commands that run
it. `python tools/scenario.py replay` re-renders every capture in `build/fifo`
and compares each frame hash with the 23 rows of `config/fifo_manifest.tsv`.
Those two need your dump; pytest and ruff do not.

## Repository layout

- `tools/` — disc extraction (`extract.py`, `checkdump.py`), analysis, the recompiler (`tools/soa/`), disassembler, capture decoders, the decompilation pipeline (`decomp.py`, `matchcheck.py`), the scenario library (`scenario.py`), the memory-card formatter (`cardformat.py`), the CI checks (`tools/citest/`), tests
- `runtime/` — the native runtime: CPU helpers, memory and MMIO, device models, threads, the software GX, the window
- `config/` — analysis metadata: the function inventory, HLE bindings, hooks, tracepoints, the scenarios and the frame-hash manifest; `config/GEAE8P/` is the decomp-toolkit project (splits, symbols and the executable's sha1)
- `src/`, `include/` — hand-decompiled units, compiled with the original Metrowerks compiler and checked word for word against the executable (`tools/decomp.py`)
- `docs/` — specification, roadmap, plan, findings, progress log
