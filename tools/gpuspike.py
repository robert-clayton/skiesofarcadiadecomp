"""The GPU spike: the renderer drawing through a Vulkan backend, headless.

    python tools/gpuspike.py build                        # shaders, then the binary
    python tools/gpuspike.py selftest [--mutate NAME]     # draw 17 scenes on both, compare
    python tools/gpuspike.py tevdiff [--cases 100000]     # the TEV, exact against tev_pixel
    python tools/gpuspike.py copydiff [--rects 200]       # the copies, exact against gxr.c's
    python tools/gpuspike.py loddiff [--cases 100000]     # the level of detail, against gxr's
    python tools/gpuspike.py oracle [--mutations]         # the captures, CPU against GPU, V0's verdict
    python tools/gpuspike.py time                         # GPU and consumer ms a frame
    python tools/gpuspike.py logicop [--mutate M]         # logic ops native, blend and snapshot
    python tools/gpuspike.py ramdiff                      # each copy's RAM, CPU against GPU
    python tools/gpuspike.py chain battle_4421 ...        # copies carried into the next frames
    python tools/gpuspike.py contrast [--mutate M]        # the spike and soa.exe --replay, the same pixels
    python tools/gpuspike.py live title [--range A-B]     # a scenario on CPU and GPU, seeded, V0 each frame
    python tools/gpuspike.py queue [--mutate M]           # V6a's queue frame: the thread, inline, stalled
    python tools/gpuspike.py overlap [--mutate M]         # V6b: the copy hazards with the GPU as consumer
    python tools/gpuspike.py specdiff [--mutate M]        # V7: specialised and interpreted, the same bytes
    python tools/gpuspike.py copyimage [--mutate M]       # V7: a copy sampled in its frame, from the pool
    python tools/gpuspike.py present [--mutate M]         # V8: the GPU's presenter against picture_scale
    python tools/gpuspike.py present --filters            # V8b: P5a's filters on the GPU against picture.c
    python tools/gpuspike.py logictest                    # V10: logic ops routed without logicOp
    python tools/gpuspike.py oracle --scale 3             # V9a: the EFB at 3x, its centre samples judged

specs/gpu-backend.md V3a, V3b, V4a, V4b and V5's loddiff. `build` compiles the shaders in
runtime/gxv/ to SPIR-V with the pinned glslang (tools/fetch_gpu.py), as C
arrays -- each mutation a variant of its own, tools/soa/shaders.py, which
recompile.py --link uses too -- and builds gx.c, gxr.c, gxr_tev.c, png.c,
gxv.c (the backend, with SOA_GXV=1) and plat.c with the spike's driver into
build/gpuspike/<compiler>/gpuspike.exe, when an input is newer than it. No
gen/, no disc: the binary links the renderer alone, as
tools/citest/render_check.py does.

`selftest` (V3a) runs the binary twice, `--backend cpu` and `--backend gpu`,
into build/gpuspike/<compiler>/cpu and gpu, and judges the scenes they wrote:

- the self test's two render recipes must pass on the GPU as they are
  written for the CPU (the driver checks them in-process);
- cull0..cull3: four triangle shapes in both windings, a strip, a fan and two
  quads under each cull mode. Away from edges the GPU covers exactly what the
  CPU covers and in the same colour; mode 3 covers nothing, mode 0 something;
- clip_*: triangles crossing the near and far planes, orthographic and in
  perspective. Away from edges the coverage matches and colours are within one
  step (the CPU steps attributes along a span, the GPU interpolates them, 3.4);
  and in the driver the vertices the consumer uploaded must be clip_polygon's;
- depth, depth_persp, scissor, quad_gradient and clear: an EQUAL redraw and
  LESS against the clear, two planes crossing in perspective, per-draw
  scissors, a quad's split, and a copy's clear;
- lines: every pixel either side drew is within one pixel of one the other
  drew (the CPU steps a line in ceil(length) floor()ed samples; Vulkan
  rasterizes by diamond exit, so endpoints and half-pixel steps may differ);
- points: exactly the CPU's pixels.

An edge pixel is one whose 3x3 neighbourhood in the CPU's image is not one
colour (for the gradients, not all covered or all uncovered): Vulkan's
top-left fill rule and the CPU's inclusive one disagree there by design.

`tevdiff` (V3b) runs tev.glsl, the TEV transcribed, against tev_pixel over
setups tev_prepare built from random registers: zero mismatches, and every
path it counts taken at least 100 times. `copydiff` (V3b) runs the same random
copies through the CPU renderer and through gxv's compute copy, in two
processes at once, and compares what each left in RAM, the decoded image and a
screen copy: byte for byte, with every refused copy leaving RAM as it was.
`loddiff` (V5) runs lod.glsl, the fragment stage's level of detail, against
the sampler's own level choice (tex_level) bit for bit, and against span_lod's
formula within 1/1024, the derivatives given to both, since V0 does not
reliably see a level's blur (FINDINGS "V4").

`oracle` (V4a, V4b) replays every capture of --set (corpus,perfset; add
gpuset for V1's): on the CPU, a reference that must hash to the manifest
(corpus) or equal V0's inspected image, and on the GPU, which V0 judges. A
frame that copies to a texture is replayed from a scratch copy with 0xA5 over
every row of tiles it copies into (3.12's poison). A failure must be listed
in BY_DESIGN, bisected. --mutations then runs the GPU mutations, each of which must fail
V0 wherever it changes 0.5% of a frame, on five captures or more, but for the
exceptions GPU_BLIND_SPOTS and SHORT_OF_FIVE list. `time` gives the GPU's and
the consumer's milliseconds a frame, beside perfbench's CPU figures.

`logicop` (V4b) draws the mask effect's 14 captures with native logic ops,
the blend approximation and the EFB snapshot, which must give byte-identical
frames; `ramdiff` holds every copy's bytes, CPU against GPU, and traces each
difference to the EFB the copy read; `chain` carries each frame's copies into
the next frame's RAM, as V1's battle start needs.

`contrast` (V5) replays every capture of --set through the spike and through gen/soa.exe --replay with
SOA_GPU=vulkan, from one scratch copy of it, and holds the two pictures to the
same pixels, with both runs' start lines naming the same device and driver.
--mutate hands one of gxv's mutations to soa.exe alone (SOA_GPU_MUTATE), and
the pictures must then differ, failing it.

`copyimage` (V7) draws a frame that copies to an RGBA8 texture and samples
the copy in the same frame, on the CPU and the GPU: the GPU must give the
CPU's hash and all 16 cells, serve the sampler from the copy's image in its
pool, and land the copy with the frame's submission rather than a wait of
its own. --mutate cimg-cpu (the producer's image, not yet landed, sampled)
and land-at-copy (V6's wait at each copy) must fail it.

`logictest` (V10) draws two synthetic logic-op scenes -- an OR quad, and two
overlapping triangles XORed -- through every route: the OR must be 0xFF but for
the forced blend's 198, and the overlapping XOR must come back black where
SOA_GPU_FEATURES=nologicop routes it to the interlock, and be a snapshot named
in a line, never a blend, under core.

`present` (V8) draws two synthetic screen copies through the GPU presenter's
pass into eight target sizes at both layouts (integer and fit) and holds each
to picture_scale's picture, the CPU presenter's, every pixel's colour exact.
--mutate present (one column over) must fail it. With --filters (V8b) it
holds P5a's filters on the GPU to picture.c instead: eight filter sets and a
flash-limiter blend of two frames, each into the picture's own size and into
1920x1080 fit, every byte picture_filter's, picture_blend's and the scaler's;
--mutate filter (one colour coefficient changed) must fail it.

`specdiff` (V7) replays every capture of --set on the GPU twice, every draw
specialised on the TEV's shape (SOA_GPU_SPECIALIZE=wait) and the interpreter
alone (0), and holds the two to the same bytes. --mutate spec-stages (a
stage dropped from the specialised shape) must fail it.

--scale 2|3 (V9a) draws every GPU run with the EFB at that many times the
console's size (SOA_GPU_SCALE). copydiff then also holds the pool's image and
the full screen to the native ones replicated (--mutate taps fails it),
copyimage the scaled copy's frame (copy-scale), present the averaging
presenter (present), and oracle, through oracle_scaled, each capture's centre
samples at 3 (V0, mip levels as at 1) or 2x2 boxes at 2 (MAE and bias), with
the native picture gxv takes cross-checked against them (phase fails that).

--specialize (V7) gives every run a command makes SOA_GPU_SPECIALIZE: wait
draws every draw with its pipeline specialised on the TEV's shape, made on
the draw path; 0 with the interpreter alone; 1, gxv's default, with the
interpreter until the compiler thread has made the specialised one. The
oracle's pictures must be the same under all three.

`queue` (V6a) draws one synthetic frame through the producer and the GPU on
its own thread: 64 quads each sampling the same texture slot at a new
generation, then 656,000 vertices that fill the producer's vertex arena
twice. On the GPU, three times on the thread, once inline (V5's path) and
once with the consumer stalled, it must give the CPU's frame hash, every
quad its own texture, and two arena drains on the thread. --mutate
pool-in-place (a slot's allocation rewritten under recorded draws) or
count-early (a variant build counting a command before it runs, stalled)
must fail it.

`overlap` (V6b) runs tools/citest/queue_check.py's overlap stream -- textures,
palettes and indexed vertex colours read from copy destinations, a copy
written twice, tokens, GXDrawDone, a hook's poke -- with the GPU as the
queue's consumer, unstalled, stalled before its draws, copies or clears, and
with SOA_GXR_TOKENWAIT=1. Every final hash the one-worker CPU run prints
must come out the same, but the EFB's, which is on the GPU. --mutate
late-readback (a copy's bytes landing after it is counted) must fail it.

`live` (after V5) runs a scenario through scenario.py three times -- on the
CPU twice and with SOA_GPU=vulkan once -- and holds each GPU snapshot to the
CPU's by V0: the scenario's own every N frames, or with --range every frame
from A to B. It is what replays cannot show: copies and textures carried from
frame to frame in a running game. A running game is not reproducible: it
reseeds from the clock, which SOA_SEED pins, and much of it is timed by the
clock, which follows the host's pace, so only the frames the two CPU runs
make byte for byte alike are judged; the rest are counted as not judged
(FINDINGS "V5, after").

--mutate names a mutation the command must fail on: unclipped (selftest),
clamp (tevdiff), rounding, intensity and unseeded (copydiff), lod and
lodmin (loddiff), and-copy,
or-copy and or-and (logicop), dest+32 (ramdiff), any gxv mutation for
soa.exe alone (contrast). Exit 0 pass,
1 fail, 3 skipped (no compiler, no vendor/, no Vulkan device), the reason
printed.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import statistics
import subprocess
import sys
from collections import Counter
from dataclasses import dataclass
from itertools import compress, count, groupby
from operator import ne
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import fetch_gpu  # noqa: E402
import fifo  # noqa: E402
import imgdiff  # noqa: E402
import scenario  # noqa: E402
from soa import png, shaders, toolchain  # noqa: E402

SPIKE = ROOT / "tools" / "gpuspike"
RUNTIME = ROOT / "runtime"
OUT = ROOT / "build" / "gpuspike"
VENDOR = ROOT / "vendor"
# The renderer, the backend (runtime/gxv.c, built with SOA_GXV=1 and the
# shaders tools/soa/shaders.py compiles) and plat.c, whose loader it opens
# Vulkan with; the driver is the spike's own.
SOURCES = [RUNTIME / n for n in ("gx.c", "gx_frame.c", "gxr.c", "gxr_tev.c", "png.c", "gxv.c", "picture.c")]
SOURCES += [*toolchain.runtime_support_sources(), SPIKE / "driver.c"]
SKIP = 3

# Scene name -> how it is judged: ("area", colour tolerance, edges by), ("lines",)
# or ("points",). Edges are where the CPU's image changes colour, for the
# flat-coloured scenes, or coverage, for the ones with gradients (every pixel
# of a gradient differs from its neighbours).
SCENES: dict[str, tuple] = {
    "cull0": ("area", 0, "colour"),
    "cull1": ("area", 0, "colour"),
    "cull2": ("area", 0, "colour"),
    "cull3": ("area", 0, "colour"),
    "clip_near_ortho": ("area", 1, "coverage"),
    "clip_far_ortho": ("area", 1, "coverage"),
    "clip_near_persp": ("area", 1, "coverage"),
    "clip_far_persp": ("area", 1, "coverage"),
    "depth": ("area", 0, "colour"),
    "depth_persp": ("area", 0, "colour"),
    "scissor": ("area", 0, "colour"),
    "quad_gradient": ("area", 1, "coverage"),
    "clear": ("area", 0, "colour"),
    "invariance": ("area", 0, "colour"),
    "logic": ("area", 0, "colour"),
    "lines": ("lines",),
    "points": ("points",),
}


# Builds of the spike with a mutation compiled in, which no runtime knob can
# reach: the name, and the defines every source gets (V6a's queue test).
VARIANTS = {"count-early": ("GXR_MUTATE_COUNT_EARLY",)}


def build_dir(prof: toolchain.Profile, variant: str = "") -> Path:
    """Each compiler's objects and binary apart: msvc and clang-cl both write
    .obj and .exe, and one must never be taken for the other's; a variant's
    apart again."""
    return OUT / (f"{prof.name}-{variant}" if variant else prof.name)


def exe_path(prof: toolchain.Profile, variant: str = "") -> Path:
    return build_dir(prof, variant) / f"gpuspike{prof.exeext}"


def inputs() -> list[Path]:
    """Everything the binary is made from: its sources and shaders, every
    header either directory holds (any of them may be included), the record
    of the fetched files, and the scripts that hold the flags."""
    return [
        *SOURCES,
        *shaders.sources(),
        *RUNTIME.glob("*.h"),
        Path(shaders.__file__),
        VENDOR / fetch_gpu.RECORD,
        Path(__file__),
        ROOT / "tools" / "soa" / "toolchain.py",
    ]


def up_to_date(prof: toolchain.Profile, variant: str = "") -> bool:
    exe = exe_path(prof, variant)
    return exe.exists() and exe.stat().st_mtime > max(p.stat().st_mtime for p in inputs())


def build(prof: toolchain.Profile, variant: str = "") -> tuple[bool, str]:
    """(built, why not). A missing compiler or vendor/ is a reason, not a
    crash. Nothing is rebuilt when the binary is newer than every input, and
    then no compiler is looked for: finding MSVC's environment takes 2 s."""
    bad = fetch_gpu.verify(VENDOR)
    if bad:
        return False, f"vendor/ is not as recorded ({bad[0]}); run python tools/fetch_gpu.py"
    if up_to_date(prof, variant):
        return True, ""
    if toolchain.compiler_path(prof) is None:
        return False, f"no {prof.name} compiler (tools/soa/toolchain.py compiler_path)"
    out = build_dir(prof, variant)
    out.mkdir(parents=True, exist_ok=True)
    exe_path(prof, variant).unlink(missing_ok=True)
    built, why = shaders.build(out)
    if not built:
        return False, why
    inc = [f"/I{RUNTIME}", f"/I{out}", f"/I{shaders.HEADERS}"]
    defines = ["/DSOA_GXV=1", *(f"/D{d}" for d in VARIANTS.get(variant, ()))]
    proc = toolchain.cc(
        [*prof.cflags, "/c", *defines, *inc, f"/Fo{out}/", *map(str, SOURCES)], ROOT, prof
    )
    text = (proc.stdout or "") + (proc.stderr or "")
    if proc.returncode != 0:
        print(text)
        return False, "compiling the spike failed"
    for line in text.splitlines():
        if "warning" in line:
            print(line)
    # Named rather than globbed, so a stale object cannot slip into the link.
    objs = [str(out / (s.stem + prof.objext)) for s in SOURCES]
    libs = ["-ldl"] if prof.style == "gnu" else []
    proc = toolchain.cc(
        [*prof.cflags, *objs, f"/Fe:{exe_path(prof, variant)}", *prof.linker, *libs], ROOT, prof
    )
    if proc.returncode != 0:
        print((proc.stdout or "") + (proc.stderr or ""))
        return False, "linking the spike failed"
    return True, ""


def run(
    prof: toolchain.Profile, backend: str, mutate: str | None, logicop: str | None = None
) -> subprocess.CompletedProcess:
    out = build_dir(prof) / backend
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("*.png"):
        old.unlink()
    args = [str(exe_path(prof)), "--backend", backend, "--out", str(out)]
    if mutate:
        args += ["--mutate", mutate]
    if logicop and backend == "gpu":
        args += ["--logicop", logicop]
    return subprocess.run(args, cwd=ROOT, capture_output=True, text=True, check=False)


# ---- the comparison ----------------------------------------------------------

# A pixel as one int, its alpha masked off; and where its three colour bytes
# are within it. Pixels are read as native words, so both depend on the order.
RGB = 0xFFFFFF if sys.byteorder == "little" else 0xFFFFFF00
SHIFTS = (0, 8, 16) if sys.byteorder == "little" else (8, 16, 24)


def pixel_rows(w: int, h: int, rgba: bytes) -> list[list[int]]:
    """Each row as a list of pixels, RGB only, as ints."""
    px = memoryview(rgba).cast("I").tolist()
    return [list(map(RGB.__and__, px[y * w : (y + 1) * w])) for y in range(h)]


def row_bits(values) -> dict:
    """One row's values -> {value: bit x set where pixel x has it}, a run of
    equal values at a time rather than a pixel."""
    out: dict = {}
    x = 0
    for v, run in groupby(values):
        n = len(list(run))
        out[v] = out.get(v, 0) | (((1 << n) - 1) << x)
        x += n
    return out


def regions(rows: list[list[int]]) -> dict[int, list[int]]:
    """Each colour in the image -> one int per row, bit x set where pixel x of
    that row is that colour."""
    out: dict[int, list[int]] = {}
    for y, row in enumerate(rows):
        for v, bits in row_bits(row).items():
            out.setdefault(v, [0] * len(rows))[y] = bits
    return out


def coverage(rows: list[list[int]]) -> list[int]:
    """One int per row, bit x set where the pixel is not black (the clear)."""
    return [row_bits(map(bool, row)).get(True, 0) for row in rows]


def neighbourhood(rows: list[int], w: int) -> tuple[list[int], list[int]]:
    """(all, any) over each pixel's 3x3 neighbourhood, the image's edges
    standing in for what lies beyond them."""
    full = (1 << w) - 1
    hi = 1 << (w - 1)

    def h_all(r: int) -> int:
        return r & (((r << 1) | (r & 1)) & full) & ((r >> 1) | (r & hi))

    def h_any(r: int) -> int:
        return (r | (r << 1) | (r >> 1)) & full

    a = [h_all(r) for r in rows]
    o = [h_any(r) for r in rows]
    n = len(rows)
    all3 = [a[max(y - 1, 0)] & a[y] & a[min(y + 1, n - 1)] for y in range(n)]
    any3 = [o[max(y - 1, 0)] | o[y] | o[min(y + 1, n - 1)] for y in range(n)]
    return all3, any3


def dilate(rows: list[int], w: int) -> list[int]:
    return neighbourhood(rows, w)[1]


def popcount(rows: list[int]) -> int:
    return sum(bin(r).count("1") for r in rows)


def colour_off(cr: list[list[int]], gr: list[list[int]], where: list[int]) -> tuple[int, int]:
    """(pixels whose colour differs, the largest step in any channel), over
    the pixels set in where."""
    off = worst = 0
    for y, bits in enumerate(where):
        if not bits or cr[y] == gr[y]:
            continue
        for x in compress(count(), map(ne, cr[y], gr[y], strict=True)):
            if (bits >> x) & 1:
                a, b = cr[y][x], gr[y][x]
                off += 1
                worst = max(worst, *(abs(((a >> k) & 255) - ((b >> k) & 255)) for k in SHIFTS))
    return off, worst


def judge(name: str, how: tuple, cpu: Path, gpu: Path) -> tuple[bool, str]:
    cw, ch, c = png.read_rgba(cpu)
    gw, gh, g = png.read_rgba(gpu)
    if (cw, ch) != (gw, gh):
        return False, f"sizes differ: cpu {cw}x{ch}, gpu {gw}x{gh}"
    w, h = cw, ch
    cr, gr = pixel_rows(w, h, c), pixel_rows(w, h, g)
    cc, gc = coverage(cr), coverage(gr)
    ncpu, ngpu = popcount(cc), popcount(gc)
    full = (1 << w) - 1
    if how[0] == "points":
        diff = popcount([a ^ b for a, b in zip(cc, gc, strict=True)])
        colours = colour_off(cr, gr, cc)[0]
        ok = diff == 0 and colours == 0 and ncpu > 0
        return (
            ok,
            f"cpu {ncpu} px, gpu {ngpu} px, {diff} placed differently, {colours} coloured differently",
        )
    if how[0] == "lines":
        dc, dg = dilate(cc, w), dilate(gc, w)
        cpu_only = popcount([a & ~b for a, b in zip(cc, gc, strict=True)])
        gpu_only = popcount([b & ~a for a, b in zip(cc, gc, strict=True)])
        far_c = popcount([a & ~b & full for a, b in zip(cc, dg, strict=True)])
        far_g = popcount([b & ~a & full for a, b in zip(dc, gc, strict=True)])
        ok = far_c == 0 and far_g == 0 and ncpu > 0
        return ok, (
            f"cpu {ncpu} px, gpu {ngpu} px; {cpu_only} cpu-only and {gpu_only} gpu-only, "
            f"of which {far_c} and {far_g} are more than one pixel from the other's"
        )
    tol, edges_by = how[1], how[2]
    all3, any3 = neighbourhood(cc, w)
    edge = [o & ~a & full for a, o in zip(all3, any3, strict=True)]
    if edges_by == "colour":
        for rows in regions(cr).values():
            ra, ro = neighbourhood(rows, w)
            edge = [e | (o & ~a & full) for e, a, o in zip(edge, ra, ro, strict=True)]
        all3 = [a & ~e for a, e in zip(all3, edge, strict=True)]
    wrong = popcount([(a ^ b) & ~e & full for a, b, e in zip(cc, gc, edge, strict=True)])
    at_edges = popcount([(a ^ b) & e for a, b, e in zip(cc, gc, edge, strict=True)])
    interior = popcount(all3)
    off, worst = colour_off(cr, gr, all3)
    ok = wrong == 0 and worst <= tol
    if name == "cull3":
        ok = ok and ncpu == 0 and ngpu == 0
    elif ncpu == 0:
        ok = False
    return ok, (
        f"cpu {ncpu} px, gpu {ngpu} px; {interior} interior, {wrong} covered differently away from edges, "
        f"{at_edges} at edges; colour off at {off} interior px, by at most {worst} (allowed {tol})"
    )


def ready(prof: toolchain.Profile, variant: str = "") -> int | None:
    """None when the binary is built; otherwise the exit code, the reason printed."""
    built, why = build(prof, variant)
    if built:
        return None
    skip = "compiler" in why or "vendor" in why
    print(f"skip: {why}" if skip else f"FAIL: {why}")
    return SKIP if skip else 1


def skipped(proc: subprocess.CompletedProcess) -> bool:
    """The driver's own skip (no Vulkan device), said aloud."""
    m = re.search(r"^skip: (.*)$", proc.stdout, re.M)
    if proc.returncode == SKIP and m:
        print(f"skip: {m.group(1)}")
        return True
    return False


def selftest(prof: toolchain.Profile, mutate: str | None, logicop: str | None = None) -> int:
    code = ready(prof)
    if code is not None:
        return code
    gpu = run(prof, "gpu", mutate, logicop)
    if skipped(gpu):
        return SKIP
    cpu = run(prof, "cpu", None)
    failures = 0
    for label, proc in (("cpu", cpu), ("gpu", gpu)):
        dev = re.search(r"^device (.*)$", proc.stdout, re.M)
        print(f"[{label}] exit {proc.returncode}" + (f", device {dev.group(1)}" if dev else ""))
        for line in proc.stderr.splitlines():
            if (
                line.startswith(("[selftest]", "[gpuspike]", "[gxv]"))
                or "refused" in line
                or "failed" in line
            ):
                print(f"  {line}")
        for line in proc.stdout.splitlines():
            if line.startswith(("recipes", "clip ", "invariance ")):
                print(f"  {line}")
        if proc.returncode != 0:
            failures += 1
    for name, how in SCENES.items():
        c, g = build_dir(prof) / "cpu" / f"{name}.png", build_dir(prof) / "gpu" / f"{name}.png"
        if not c.exists() or not g.exists():
            print(f"FAIL {name}: no image from {'cpu' if not c.exists() else 'gpu'}")
            failures += 1
            continue
        ok, detail = judge(name, how, c, g)
        print(f"{'ok  ' if ok else 'FAIL'} {name}: {detail}")
        failures += not ok
    print(f"[gpuspike] {'every check passes' if not failures else f'{failures} check(s) failed'}")
    return 1 if failures else 0


# ---- V3b: the exact differentials ----------------------------------------------


def tevdiff(prof: toolchain.Profile, cases: int, seed: int, mutate: str | None) -> int:
    code = ready(prof)
    if code is not None:
        return code
    args = [str(exe_path(prof)), "--backend", "gpu", "--tevdiff", str(cases), "--seed", str(seed)]
    if mutate:
        args += ["--mutate", mutate]
    proc = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, check=False)
    if skipped(proc):
        return SKIP
    for line in proc.stdout.splitlines():
        if line.startswith("hits "):
            words = line.split()[1:]
            print("hits, a path a count, every one at least 100:")
            for i in range(0, len(words), 8):
                print("  " + "  ".join(words[i : i + 8]))
        elif not line.startswith("device "):
            print(line)
    for line in proc.stderr.splitlines():
        if line.startswith("[gxv]") or "failed" in line:
            print(line)
    m = re.search(r"^tevdiff \d+ cases, seed \d+: (\d+) mismatch", proc.stdout, re.M)
    ok = proc.returncode == 0 and m is not None and m.group(1) == "0"
    print(f"[gpuspike] tevdiff {'passes' if ok else 'FAILS'}")
    return 0 if ok else 1


def loddiff(prof: toolchain.Profile, cases: int, seed: int, mutate: str | None) -> int:
    code = ready(prof)
    if code is not None:
        return code
    args = [str(exe_path(prof)), "--backend", "gpu", "--loddiff", str(cases), "--seed", str(seed)]
    if mutate:
        args += ["--mutate", mutate]
    proc = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, check=False)
    if skipped(proc):
        return SKIP
    for line in proc.stdout.splitlines():
        if line.startswith("lodhits "):
            print("hits, a path a count, every one at least 100:")
            print("  " + "  ".join(line.split()[1:]))
        elif not line.startswith("device "):
            print(line)
    for line in proc.stderr.splitlines():
        if line.startswith("[gxv]") or "failed" in line:
            print(line)
    level = re.search(
        r"^loddiff level \d+ cases, seed \d+: (\d+) mismatch\w*; (\d+) path", proc.stdout, re.M
    )
    formula = re.search(r"^loddiff formula (\d+) cases, seed \d+: (\d+) over", proc.stdout, re.M)
    ok = (
        proc.returncode == 0
        and level is not None
        and level.group(1) == "0"
        and level.group(2) == "0"
        and formula is not None
        and int(formula.group(1)) > 0
        and formula.group(2) == "0"
    )
    print(f"[gpuspike] loddiff {'passes' if ok else 'FAILS'}")
    return 0 if ok else 1


# A copydiff line: the case, then four hashes. Rows match when every field
# does; a refused copy (texfmt 99) must leave its RAM hash equal to the seed's.
COPY_FIELDS = [
    "combo",
    "rect",
    "tpf",
    "intensity",
    "half",
    "filtered",
    "x0",
    "y0",
    "w",
    "h",
    "stride",
    "dest",
    "texfmt",
    "extent",
    "seeded",
    "ram",
    "image",
    "screen",
]
HASHED = {"ram": 15, "image": 16, "screen": 17}


def compare_copies(cpu: list[str], gpu: list[str]) -> tuple[list[str], Counter]:
    """(problems, mismatches by (what, tpf, intensity, half, filtered))."""
    problems: list[str] = []
    mismatches: Counter = Counter()
    if len(cpu) != len(gpu) or not cpu:
        problems.append(f"the CPU wrote {len(cpu)} copies and the GPU {len(gpu)}")
    for a, b in zip(cpu, gpu, strict=False):
        fa, fb = a.split(), b.split()
        if len(fa) != len(COPY_FIELDS) or fa[:15] != fb[:15]:
            problems.append(f"the two runs made different copies: {a!r} against {b!r}")
            continue
        for side, f in (("cpu", fa), ("gpu", fb)):
            if f[12] == "99" and f[15] != f[14]:
                problems.append(f"{side} wrote RAM for a refused copy: {' '.join(f[:14])}")
        for what, k in HASHED.items():
            if fa[k] != fb[k]:
                mismatches[(what, int(fa[2]), int(fa[3]), int(fa[4]), int(fa[5]))] += 1
    return problems, mismatches


RE_COPY_SCALE = re.compile(
    r"^copydiff at scale (\d+): the pool's image is the native one replicated in (\d+) of (\d+) "
    r"copies, the full screen g_screen replicated in (\d+) of (\d+)$",
    re.M,
)


def copydiff(prof: toolchain.Profile, rects: int, seed: int, mutate: str | None) -> int:
    code = ready(prof)
    if code is not None:
        return code
    base = build_dir(prof) / "copydiff"
    procs = {}
    for backend in ("cpu", "gpu"):
        out = base / backend
        out.mkdir(parents=True, exist_ok=True)
        (out / "copydiff.txt").unlink(missing_ok=True)
        args = [
            str(exe_path(prof)),
            "--backend",
            backend,
            "--copydiff",
            str(rects),
            "--seed",
            str(seed),
        ]
        args += ["--out", str(out)]
        if mutate and backend == "gpu":
            args += ["--mutate", mutate]
        env = {**os.environ, "SOA_GPU_SCALE": str(SCALE)} if SCALE > 1 else None
        # The two sides are independent processes with their own output, so
        # they run at once: each takes about 18 s at 200 rectangles.
        procs[backend] = subprocess.Popen(
            args, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env
        )
    done = {}
    for backend, proc in procs.items():
        out, err = proc.communicate()
        done[backend] = subprocess.CompletedProcess(proc.args, proc.returncode, out, err)
    if skipped(done["gpu"]):
        return SKIP
    for backend, proc in done.items():
        for line in (proc.stdout + proc.stderr).splitlines():
            if line.startswith(("[gxv]", "copydiff")) or "failed" in line or "refused" in line:
                print(f"[{backend}] {line}")
        if proc.returncode != 0:
            print(f"FAIL: the {backend} side exited {proc.returncode}")
            return 1
    cpu = (base / "cpu" / "copydiff.txt").read_text(encoding="utf-8").splitlines()
    gpu = (base / "gpu" / "copydiff.txt").read_text(encoding="utf-8").splitlines()
    problems, mismatches = compare_copies(cpu, gpu)
    refused = sum(1 for line in cpu if line.split()[12] == "99")
    print(
        f"{len(cpu)} copies, {refused} of them refused; each also copied to the screen. "
        f"Differences: "
        + ", ".join(
            f"{what} {sum(n for k, n in mismatches.items() if k[0] == what)}" for what in HASHED
        )
    )
    if SCALE > 1:
        # V9a: on the replicated EFB, the pool's image and the full screen are
        # the native ones replicated, every copy.
        m = RE_COPY_SCALE.search(done["gpu"].stdout)
        if not m or int(m.group(1)) != SCALE:
            problems.append(f"no line for the copies at scale {SCALE}")
        elif m.group(2) != m.group(3) or m.group(4) != m.group(5) or m.group(3) == "0":
            problems.append(
                f"at scale {SCALE}: the pool's image replicated in {m.group(2)} of {m.group(3)} copies, "
                f"the full screen in {m.group(4)} of {m.group(5)}"
            )
    for line in problems[:5]:
        print(f"FAIL {line}")
    for (what, tpf, intensity, half, filtered), n in sorted(mismatches.items())[:12]:
        print(
            f"FAIL {what}: format {tpf}, intensity {intensity}, half {half}, filter {filtered}: "
            f"{n} of {rects}"
        )
    ok = not problems and not mismatches
    print(f"[gpuspike] copydiff {'passes' if ok else 'FAILS'}")
    return 0 if ok else 1


# ---- V4a: captures -------------------------------------------------------------


# --specialize: SOA_GPU_SPECIALIZE for every run a command makes, or None to
# leave gxv's default (1: specialised pipelines made on the compiler thread,
# the interpreter drawing meanwhile).
SPECIALIZE: str | None = None
# --features core: SOA_GPU_FEATURES=core for every run (V10, spec 3.10): every
# optional feature treated as absent -- logicOp, the interlock -- which is how
# an Android GPU's capability set is tried on this one.
FEATURES: str | None = None
# --scale (V9a): SOA_GPU_SCALE for every GPU run.
SCALE = 1


def clean_env() -> dict[str, str]:
    """The parent's environment with every SOA_* taken out, and SOA_SETTINGS=0:
    nothing set for another run may reach a replay (3.12's rule, and imgdiff's
    for its references). --specialize, --features and --scale alone are put
    back."""
    env = {k: v for k, v in os.environ.items() if not k.startswith("SOA_")}
    env["SOA_SETTINGS"] = "0"
    if SPECIALIZE is not None:
        env["SOA_GPU_SPECIALIZE"] = SPECIALIZE
    if FEATURES is not None:
        env["SOA_GPU_FEATURES"] = FEATURES
    if SCALE > 1:
        env["SOA_GPU_SCALE"] = str(SCALE)
    return env


@dataclass
class Cap:
    setname: str
    capture: imgdiff.Capture
    copies: list  # TexCopy: what the frame copies to a texture


def captures(sets: list[str]) -> list[Cap]:
    """Every capture of those sets, with the copies to a texture its stream
    makes (V4a's 21 have none)."""
    return [Cap(name, c, texture_copies(c.base)) for name in sets for c in imgdiff.SETS[name]()]


def replay_base(cap: Cap, scratch: Path) -> Path:
    """What to replay: the capture itself, or -- when its frame copies to a
    texture -- a scratch copy with every destination poisoned (3.12)."""
    if not cap.copies:
        return cap.capture.base
    return scratch_capture(cap.capture.base, scratch / cap.capture.name, cap.copies)


def replay(
    prof: toolchain.Profile,
    backend: str,
    base: Path,
    png_out: Path,
    mutate: str | None = None,
    logicop: str | None = None,
    dump_ram: Path | None = None,
    env: dict[str, str] | None = None,
    png_full: Path | None = None,
) -> tuple[subprocess.CompletedProcess, str | None]:
    """One capture through the spike: the run, and the frame's hash."""
    args = [str(exe_path(prof)), "--backend", backend, "--replay", str(base), "--png", str(png_out)]
    if mutate:
        args += ["--mutate", mutate]
    if logicop and backend == "gpu":
        args += ["--logicop", logicop]
    if dump_ram:
        args += ["--dump-ram", str(dump_ram)]
    if png_full and backend == "gpu":
        args += ["--png-full", str(png_full)]
    proc = subprocess.run(
        args,
        cwd=ROOT,
        env={**clean_env(), **(env or {})},
        capture_output=True,
        text=True,
        check=False,
    )
    m = re.search(r"^frame \d+x\d+ hash ([0-9a-f]{16})$", proc.stdout, re.M)
    return proc, m.group(1) if m else None


def same_pixels(a: Path, b: Path) -> bool:
    return png.read_rgba(a) == png.read_rgba(b)


def changed_fraction(a: Path, b: Path) -> float:
    """The fraction of pixels whose RGB differs at all."""
    w, h, x = png.read_rgba(a)
    _, _, y = png.read_rgba(b)
    rows_x, rows_y = pixel_rows(w, h, x), pixel_rows(w, h, y)
    differ = sum(sum(map(ne, rx, ry, strict=True)) for rx, ry in zip(rows_x, rows_y, strict=True))
    return differ / (w * h)


# V4a's mutations of the GPU path (specs/gpu-backend.md V4a): each must change
# at least 0.5% of the pixels of at least five captures ("applies", 3.12), and
# fail V0 on every capture where it applies.
GPU_MUTATIONS = {
    "skip-largest": "the most visible of the frame's five largest draws (by samples) skipped",
    "fog": "fog off",
    "nofilter": "the copy filter off in the screen copy",
    "alpha": "the alpha test off",
    "lod": "level-of-detail bias +1",
    "logic-copy": "logic ops ignored, drawn as copies (V4b)",
    "skip-copies": "copies to a texture skipped (V4b)",
    "dest+32": "copies to a texture written 32 bytes on (V4b)",
}
APPLIES = 0.005

# Where a mutation applies and V0 still passes, each found on 2026-10-03 and
# looked at (FINDINGS "V4"), as imgdiff lists V0's own. A blind spot that is
# not listed fails the run, and so do more than three for one mutation.
GPU_BLIND_SPOTS: dict[str, dict[str, str]] = {
    "lod": {
        "sky_4000": "one level blurrier on the player's ship and the cloud band only: 12% of pixels move, "
        "most by one or two steps, 137 far; V0's whole-frame blur and MAE are diluted by the empty sky",
        "sky_4001": "the frame after sky_4000, the same",
        "15200": "the ship's hold: one level blurrier fades the floor's rivets almost away; 12% of pixels "
        "move, 1,012 far, and V0's vertical blur measure reads 0.089 against its 0.10",
        "15800": "the hold, later: the same, 0.089 again",
        "corpus_15800": "15800 as the benchmark set captured it: the same",
    },
}
# Captures of one moment, for counting blind spots over distinct frames
# (3.12): each "+1" benchmark frame and the benchmark set's copies of corpus
# frames, under the frame they repeat.
SAME_MOMENT: dict[str, str] = {
    "sky_4001": "sky_4000",
    "battle_4001": "battle_4000",
    "ship_6001": "ship_6000",
    "cutscene_4501": "cutscene_4500",
    "field_5001": "field_5000",
    "corpus_15800": "15800",
    "corpus_6000": "6000",
}
# Captures that fail V0 for a cause 3.12 calls by design, each bisected to
# its first diverging draws (FINDINGS "V4"). An unlisted failure fails the
# run, and so do more than three listed ones (3.12: the threshold question).
BY_DESIGN: dict[str, str] = {
    "field_5000": "coplanar decals' depth: the CPU steps depth along a span by float adds and drifts 27 to 57 "
    "24-bit steps over long spans (draws 346, 355 and 653), so decals 374, 393 and 653 pass LEQUAL on the CPU "
    "that exact arithmetic and the GPU (within 2 steps of it) fail; draw 656 ties exactly and the CPU's last "
    "bit fails it; draw 76 samples at LOD 3.50, on the boundary of two very different mip levels",
    "field_5001": "the frame after field_5000, the same",
}
# A mutation that applies on fewer than five captures, with why. The rule
# (3.12) wants five; the corpus and the benchmark set have no more to give,
# and with V1's captures (--set corpus,perfset,gpuset) the alpha test applies
# on five.
# Captures whose 2x picture -- the box of each 2x2 -- is beyond V0's MAE for
# a cause V9a calls by design: averaging four samples is supersampling, which
# blends text edges and fine texture that the CPU's single sample keeps, and
# shifts these frames' level a little. Each was checked against the 3x frame's
# box, which is the same supersampling from other samples (FINDINGS "V9a").
BY_DESIGN_SCALE2: dict[str, str] = {
    "4800": "the dialogue's text edges and the hall's metal averaged: MAE 1.652, bias -0.62; the 3x frame's "
    "box is darker by the same (-0.57) and within MAE 0.527 of the 2x box, while its centre samples are the "
    "CPU's level to 0.005",
    "ship_6000": "the hold's rivets and planks averaged: MAE 1.549, bias -0.36; the 3x box within MAE 0.382 "
    "of it, its centre samples the CPU's level",
    "ship_6001": "the frame after ship_6000, the same",
}

SHORT_OF_FIVE: dict[str, str] = {
    "alpha": "only 8000 and the ship pair have alpha-tested pixels that show once drawn (0.9%, 3.8%); "
    "sky's come to 0.49% and 12100's to 0.23%",
}


def oracle(prof: toolchain.Profile, sets: list[str], mutations: bool) -> int:
    """V4a's and V4b's Done: every capture of the sets, poisoned where its
    frame copies to a texture, replayed on the CPU (the reference, checked
    against the manifest's hash or V0's inspected image) and on the GPU, and
    V0's verdict on the pair."""
    code = ready(prof)
    if code is not None:
        return code
    out = build_dir(prof) / "oracle"
    out.mkdir(parents=True, exist_ok=True)
    caps = captures(sets)
    with_copies = sum(1 for cap in caps if cap.copies)
    print(
        f"{len(caps)} captures from {', '.join(sets)}; {with_copies} copy to a texture and are "
        f"replayed poisoned, {len(caps) - with_copies} do not"
    )
    failures = 0
    largest: dict[str, list[str]] = {}
    bases: dict[str, Path] = {}
    rows = []
    for cap in caps:
        setname, c = cap.setname, cap.capture
        base = bases[c.name] = replay_base(cap, out / "poisoned")
        ref, gpu = out / f"{c.name}_cpu.png", out / f"{c.name}_gpu.png"
        proc, h = replay(prof, "cpu", base, ref)
        if proc.returncode != 0 or h is None:
            print(f"FAIL {c.name}: the CPU replay exited {proc.returncode}")
            failures += 1
            continue
        if c.frame_hash is not None:
            ref_ok = h == c.frame_hash
            ref_note = f"reference {h} {'is' if ref_ok else 'is NOT'} the manifest's"
        else:
            v0 = imgdiff.OUT / "ref" / setname / f"{c.name}.png"
            ref_ok = v0.exists() and same_pixels(ref, v0)
            ref_note = (
                f"reference {'is' if ref_ok else 'is NOT'} V0's {v0.relative_to(ROOT).as_posix()}"
            )
        if not ref_ok:
            print(f"FAIL {c.name}: {ref_note}")
            failures += 1
            continue
        # The GPU's frame, counting each draw's samples on the way (which
        # changes no pixel) for the skip-largest mutation.
        proc, _ = replay(prof, "gpu", base, gpu, "measure")
        if skipped(proc):
            return SKIP
        m = re.search(r"largest draws \(draw:samples\)((?: \d+:\d+)+)", proc.stderr)
        if proc.returncode != 0 or not m:
            print(f"FAIL {c.name}: the GPU replay exited {proc.returncode}")
            for line in proc.stderr.splitlines():
                if "refused" in line or "failed" in line:
                    print(f"  {line}")
            failures += 1
            continue
        largest[c.name] = [d.split(":")[0] for d in m.group(1).split() if not d.endswith(":0")]
        metrics = imgdiff.compare(ref, gpu, out / f"{c.name}_heat.png")
        bad = metrics.failures()
        listed = bool(bad) and c.name in BY_DESIGN
        failures += bool(bad) and not listed
        rows.append((c.name, not bad, listed))
        verdict = "pass" if not bad else "by design" if listed else "FAIL"
        print(
            f"{verdict} {setname:7} {c.name:14} {'poisoned ' if cap.copies else ''}{metrics.line()}"
        )
        for line in bad:
            print(f"     {line}")
        if listed:
            print(f"     by design: {BY_DESIGN[c.name]}")
    passed = sum(ok for _, ok, _ in rows)
    by_design = sum(listed for _, _, listed in rows)
    if by_design > 3:
        print(
            f"FAIL {by_design} by-design failures: more than three is the threshold question (3.12)"
        )
        failures += 1
    print(f"[gpuspike] oracle: {passed} of {len(caps)} pass V0, {by_design} fail by design")
    if mutations and not failures:
        # A frame that already fails V0 cannot show a mutation failing it.
        judged = [(cap.setname, cap.capture) for cap in caps if cap.capture.name not in BY_DESIGN]
        failures += oracle_mutations(prof, judged, out, largest, bases)
    return 1 if failures else 0


def oracle_scaled(prof: toolchain.Profile, sets: list[str], scale: int, mutate: str | None) -> int:
    """V9a's Done: every capture of the sets, poisoned where it copies to a
    texture, replayed on the CPU (the reference, checked as oracle checks it)
    and on the GPU with the EFB at `scale`, the full-size screen written.

    At scale 3 the GPU picks mip levels as at scale 1 (SOA_GPU_SCALE_LOD=
    native), each 3x3 block's centre sample -- co-located with the native
    pixel's centre -- is taken (imgdiff's --sample centre), and V0 judges it
    against the CPU with its thresholds. At scale 2, which has no co-located
    sample, the box of each 2x2 is judged on MAE and bias alone. Either way
    the downsampled picture must be g_screen, the native picture gxv itself
    takes (exactly at 3; within one step at 2, where the copy filter's
    truncation comes before the mean in one and after it in the other): the
    check that --mutate phase fails. Then the same replay with mip levels
    chosen for the scale, as a player sees it: MAE and bias, reported."""
    code = ready(prof)
    if code is not None:
        return code
    out = build_dir(prof) / f"oracle{scale}"
    out.mkdir(parents=True, exist_ok=True)
    caps = captures(sets)
    how = "centre" if scale % 2 else "box"
    print(f"{len(caps)} captures from {', '.join(sets)} at scale {scale}, judged on the {how}")
    failures = 0
    rows = []
    for cap in caps:
        c = cap.capture
        base = replay_base(cap, out / "poisoned")
        ref = out / f"{c.name}_cpu.png"
        proc, h = replay(prof, "cpu", base, ref)
        if proc.returncode != 0 or h is None:
            print(f"FAIL {c.name}: the CPU replay exited {proc.returncode}")
            failures += 1
            continue
        if c.frame_hash is not None and h != c.frame_hash:
            print(f"FAIL {c.name}: reference {h} is NOT the manifest's")
            failures += 1
            continue
        verdicts = {}
        for lod in ("native", "scale"):
            native, full = (
                out / f"{c.name}_gpu_{lod}_native.png",
                out / f"{c.name}_gpu_{lod}_full.png",
            )
            gpu = out / f"{c.name}_gpu_{lod}.png"
            env = {"SOA_GPU_SCALE_LOD": lod}
            proc, _ = replay(prof, "gpu", base, native, mutate, env=env, png_full=full)
            if skipped(proc):
                return SKIP
            if proc.returncode != 0 or not full.exists():
                print(f"FAIL {c.name}: the GPU replay at scale {scale} exited {proc.returncode}")
                failures += 1
                break
            fw, fh, fpx = png.read_rgba(full)
            w, h_, small = imgdiff.downsample(fw, fh, fpx, scale, how)
            png.write_rgba(gpu, w, h_, small)
            nw, nh, npx = png.read_rgba(native)
            off = imgdiff.largest_step(small, npx) if (nw, nh) == (w, h_) else 256
            allowed = 0 if scale % 2 else 1
            if off > allowed:
                print(
                    f"FAIL {c.name}: the {how} of the full screen is not g_screen "
                    f"({'size' if off == 256 else f'off by up to {off}'}; {allowed} allowed)"
                )
                failures += 1
            verdicts[lod] = imgdiff.compare(ref, gpu, out / f"{c.name}_heat_{lod}.png")
        if len(verdicts) < 2:
            continue
        m = verdicts["native"]
        bad = m.failures()
        listing = BY_DESIGN
        if scale % 2 == 0:
            bad = [b for b in bad if b.startswith(("MAE", "bias"))]
            listing = BY_DESIGN_SCALE2
        listed = bool(bad) and c.name in listing
        failures += bool(bad) and not listed
        rows.append((c.name, not bad, listed))
        verdict = "pass" if not bad else "by design" if listed else "FAIL"
        print(f"{verdict} {cap.setname:7} {c.name:14} {m.line()}")
        for line in bad:
            print(f"     {line}")
        if listed:
            print(f"     by design: {listing[c.name]}")
        ms = verdicts["scale"]
        print(
            f"     mip levels for the scale: MAE {ms.mae:.3f}, bias "
            f"{ms.bias[0]:+.3f} {ms.bias[1]:+.3f} {ms.bias[2]:+.3f}"
        )
    passed = sum(ok for _, ok, _ in rows)
    by_design = sum(listed for _, _, listed in rows)
    if by_design > 3:
        print(
            f"FAIL {by_design} by-design failures: more than three is the threshold question (3.12)"
        )
        failures += 1
    print(
        f"[gpuspike] oracle at scale {scale}: {passed} of {len(caps)} pass "
        f"{'V0' if scale % 2 else 'MAE and bias'}, {by_design} fail by design"
    )
    return 1 if failures else 0


def oracle_mutations(
    prof: toolchain.Profile,
    caps: list[tuple[str, imgdiff.Capture]],
    out: Path,
    largest: dict[str, list[str]],
    bases: dict[str, Path],
) -> int:
    """Each GPU mutation on every capture: where it changes 0.5% of the
    pixels or more it applies, and there V0 must fail. The draw skipped is
    the one, of the five that passed the most samples, whose absence changes
    the most pixels: the most samples alone picks a full-screen fill that
    later draws cover entirely, whose absence nobody could see."""
    bad = 0
    for name, what in GPU_MUTATIONS.items():
        applies = failed = 0
        cells = []
        for _, c in caps:
            mutated = out / f"{c.name}_{name}.png"
            if name == "skip-largest":
                best, frac = None, -1.0
                for draw in largest[c.name]:
                    trial = out / f"{c.name}_skip{draw}.png"
                    proc, _ = replay(prof, "gpu", bases[c.name], trial, f"skip-draw:{draw}")
                    f = (
                        changed_fraction(out / f"{c.name}_gpu.png", trial)
                        if proc.returncode == 0
                        else -1.0
                    )
                    if f > frac:
                        best, frac = draw, f
                        trial.replace(mutated)
                    else:
                        trial.unlink(missing_ok=True)
                if best is None:
                    cells.append(f"{c.name} no draw to skip")
                    bad += 1
                    continue
            else:
                proc, _ = replay(prof, "gpu", bases[c.name], mutated, name)
                if proc.returncode != 0:
                    cells.append(f"{c.name} exit {proc.returncode}")
                    bad += 1
                    continue
                frac = changed_fraction(out / f"{c.name}_gpu.png", mutated)
            fails = bool(imgdiff.compare(out / f"{c.name}_cpu.png", mutated).failures())
            if frac >= APPLIES:
                applies += 1
                failed += fails
                cells.append(f"{c.name} {frac:.1%} {'fails' if fails else 'PASSES'}")
            else:
                cells.append(f"{c.name} {frac:.2%} -")
        listed = GPU_BLIND_SPOTS.get(name, {})
        spots = [cell.split()[0] for cell in cells if cell.endswith("PASSES")]
        unlisted = [n for n in spots if n not in listed]
        distinct = {SAME_MOMENT.get(n, n) for n in spots}
        ok = (applies >= 5 or name in SHORT_OF_FIVE) and not unlisted and len(distinct) <= 3
        bad += not ok
        print(
            f"{'ok  ' if ok else 'FAIL'} mutation {name} ({what}): applies on {applies}, "
            f"fails V0 on {failed}, blind spots {len(spots)} ({len(distinct)} distinct)"
        )
        for i in range(0, len(cells), 4):
            print("       " + ";  ".join(cells[i : i + 4]))
        if applies < 5:
            print(f"       applies on fewer than five: {SHORT_OF_FIVE.get(name, 'NOT LISTED')}")
        for n in spots:
            print(f"       blind spot {n}: {listed.get(n, 'NOT LISTED')}")
    print(
        f"[gpuspike] mutations: {'each fails V0 wherever it applies, but for the blind spots listed' if not bad else f'{bad} problem(s)'}"
    )
    return bad


def time_frames(prof: toolchain.Profile, sets: list[str], runs: int, perf_runs: int) -> int:
    """GPU and consumer milliseconds a frame, median of `runs` replays, beside
    tools/perfbench.py's CPU figures taken before and after in the same
    session (A B A). Reported, not a gate (V4a)."""
    code = ready(prof)
    if code is not None:
        return code
    scratch = build_dir(prof) / "time"
    scratch.mkdir(parents=True, exist_ok=True)

    def perfbench() -> None:
        exe = ROOT / "gen" / "soa.exe"
        if not exe.exists():
            print("perfbench: no gen/soa.exe here, so no CPU figures")
            return
        proc = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "perfbench.py"), "run", "--runs", str(perf_runs)],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        print(proc.stdout.rstrip())

    perfbench()
    print(f"{'capture':<16} {'GPU ms':>8} {'consumer ms':>12}   (median of {runs})")
    for c in (c for name in sets for c in imgdiff.SETS[name]()):
        gpu_ms, consumer_ms = [], []
        for _ in range(runs):
            proc, _ = replay(prof, "gpu", c.base, scratch / "frame.png")
            if skipped(proc):
                return SKIP
            g = re.search(r"GPU ([0-9.]+) ms", proc.stderr)
            k = re.search(r"consumer ([0-9.]+) ms", proc.stderr)
            if proc.returncode != 0 or not g or not k:
                print(f"FAIL {c.name}: exit {proc.returncode}")
                return 1
            gpu_ms.append(float(g.group(1)))
            consumer_ms.append(float(k.group(1)))
        print(
            f"{c.name:<16} {statistics.median(gpu_ms):8.2f} {statistics.median(consumer_ms):12.2f}"
        )
    perfbench()
    return 0


# ---- V4b: copies to a texture: poison, ramdiff, chain ---------------------------

MEM1_SIZE = 0x01800000
POISON = 0xA5
# Each texture format's tile: width, height, bytes (gxr.c copy_bytes).
TILE = {
    0: (8, 8, 32),
    1: (8, 4, 32),
    2: (8, 4, 32),
    3: (4, 4, 32),
    4: (4, 4, 32),
    5: (4, 4, 32),
    6: (4, 4, 64),
}


def copy_texfmt(v: int) -> int:
    """gxr.c's copy_texfmt: the texture format a copy command makes, 99 for
    one it refuses. A third copy of it (gxv.c has the second), held to the
    first by the CPU replays this file's poison step must leave unmoved."""
    tpf = (v >> 3) & 15
    fmt = tpf // 2 + (tpf & 1) * 8
    if (v >> 15) & 1:
        return fmt if fmt <= 3 else 99
    return {0: 0, 1: 1, 7: 1, 8: 1, 9: 1, 10: 1, 2: 2, 3: 3, 11: 3, 12: 3, 4: 4, 5: 5, 6: 6}.get(
        fmt, 99
    )


@dataclass
class TexCopy:
    """A copy to a texture, as the stream makes it."""

    index: int  # among the frame's copies to a texture
    offset: int  # where its BP 0x52 write starts in the stream
    dest: int
    extent: int  # bytes, from its first tile to the end of its last
    texfmt: int
    x0: int
    y0: int
    ow: int
    oh: int
    half: int
    row_bytes: int

    def runs(self) -> list[tuple[int, int]]:
        """The bytes the copy writes, a run a row of tiles: a stride wider than
        the copy leaves gaps between them that it never touches."""
        tw, th, bpt = TILE[self.texfmt]
        cols, rows = (self.ow + tw - 1) // tw, (self.oh + th - 1) // th
        return [(self.dest + r * self.row_bytes, cols * bpt) for r in range(rows)]


def texture_copies(base: Path) -> list[TexCopy]:
    """Every copy to a texture a capture's stream makes, with copy_bytes'
    arithmetic (strides included), from the registers as the stream sets
    them. Only copies in the stream itself: a copy inside a display list is
    refused, since cutting the stream there (ramdiff) would cut the list."""
    cp_regs, xf_regs, bp_regs = fifo.read_regs(str(scenario.part(base, ".regs")))
    cp = {i: v for i, v in enumerate(cp_regs) if v}
    bp = dict(enumerate(bp_regs))
    ram = scenario.part(base, ".ram").read_bytes()
    stream = scenario.part(base, ".fifo").read_bytes()
    out: list[TexCopy] = []
    for cmd in fifo.walk(stream, cp, list(xf_regs), ram, bp=bp, follow_lists=True):
        if cmd[0] != "bp" or cmd[3] != 0x52 or cmd[4] & 0x4000:
            continue
        if cmd[2] is not None:
            raise SystemExit(f"gpuspike: {base.name} copies to a texture inside a display list")
        v = cmd[4]
        texfmt = copy_texfmt(v)
        if texfmt == 99:
            continue
        tl, wh = bp.get(0x49, 0), bp.get(0x4A, 0)
        w, h = (wh & 0x3FF) + 1, ((wh >> 10) & 0x3FF) + 1
        half = (v >> 9) & 1
        ow, oh = (w // 2, h // 2) if half else (w, h)
        tw, th, bpt = TILE[texfmt]
        natural = (ow + tw - 1) // tw * bpt
        row = max((bp.get(0x4D, 0) & 0x3FF) * 32, natural)
        rows, cols = (oh + th - 1) // th, (ow + tw - 1) // tw
        extent = (rows - 1) * row + cols * bpt if rows and cols else 0
        dest = ((bp.get(0x4B, 0) & 0x1FFFFF) << 5) & fifo.MEM_MASK
        if extent and dest + extent <= MEM1_SIZE:
            out.append(
                TexCopy(
                    len(out),
                    cmd[1],
                    dest,
                    extent,
                    texfmt,
                    tl & 0x3FF,
                    (tl >> 10) & 0x3FF,
                    ow,
                    oh,
                    half,
                    row,
                )
            )
    return out


def scratch_capture(
    base: Path,
    out: Path,
    poison: list[TexCopy],
    splice: dict[int, bytes] | None = None,
    stream: bytes | None = None,
) -> Path:
    """A copy of a capture in `out`: its RAM with `splice`'s bytes laid over
    it (chain) and then 0xA5 over every range in `poison` (3.12: a sampler
    can then see only what this replay's copies wrote), and its stream
    replaced by `stream` when given (ramdiff)."""
    out.mkdir(parents=True, exist_ok=True)
    dst = out / base.name
    shutil.copyfile(scenario.part(base, ".regs"), scenario.part(dst, ".regs"))
    if stream is None:
        shutil.copyfile(scenario.part(base, ".fifo"), scenario.part(dst, ".fifo"))
    else:
        scenario.part(dst, ".fifo").write_bytes(stream)
    ram = bytearray(scenario.part(base, ".ram").read_bytes())
    for at, data in (splice or {}).items():
        ram[at : at + len(data)] = data
    for c in poison:
        for at, n in c.runs():
            ram[at : at + n] = bytes([POISON]) * n
    scenario.part(dst, ".ram").write_bytes(bytes(ram))
    return dst


def bp_write(reg: int, value: int) -> bytes:
    return bytes([0x61, reg]) + (value & 0xFFFFFF).to_bytes(3, "big")


def efb_at(base: Path, c: TexCopy) -> bytes:
    """The capture's stream cut just before copy c, then an unfiltered
    640x480 screen copy: replayed, its frame is the EFB as that copy found it."""
    stream = scenario.part(base, ".fifo").read_bytes()[: c.offset]
    tail = bp_write(0x53, 0) + bp_write(0x54, 0) + bp_write(0x49, 0) + bp_write(0x4A, 0x077E7F)
    return stream + tail + bp_write(0x52, 0x4003)


def texel_of(off: int, c: TexCopy) -> tuple[int, int] | None:
    """The texel byte `off` of a copy holds (the first, for R4's two), or
    None for a byte between rows of tiles or past the copy's edge."""
    tw, th, bpt = TILE[c.texfmt]
    ty, rem = divmod(off, c.row_bytes)
    tx, b = divmod(rem, bpt)
    if tx >= (c.ow + tw - 1) // tw:
        return None
    if c.texfmt == 0:
        iy, ix = b // 4, (b % 4) * 2
    elif c.texfmt in (1, 2):
        iy, ix = divmod(b, 8)
    elif c.texfmt == 6:
        iy, ix = divmod((b % 32) // 2, 4)
    else:
        iy, ix = divmod(b // 2, 4)
    x, y = tx * tw + ix, ty * th + iy
    return (x, y) if x < c.ow and y < c.oh else None


def logic_captures(sets=("corpus", "perfset", "gpuset")) -> list[Cap]:
    """The captures that draw under a logic op: the mask effect's 14, and
    V1's that do (V10 holds those too)."""
    out = []
    for cap in captures(list(sets)):
        summary = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "fifo.py"), str(cap.capture.base), "--summary"],
            capture_output=True,
            text=True,
            check=False,
        ).stdout
        m = re.search(r"(\d+) draws under a logic op", summary)
        if m and int(m.group(1)):
            out.append(cap)
    return out


LOGIC_MODES = ("native", "blend", "snapshot", "interlock")


# The GPU backend's start line; contrast holds both binaries' to one device.
GXV_START = re.compile(r"^\[gxv\] Vulkan .*$", re.M)


RE_SPEC = re.compile(r"specialised on the TEV's shape: (\d+) for (\d+) distinct shapes")


def specdiff(prof: toolchain.Profile, sets: list[str], mutate: str | None) -> int:
    """V7: every capture of --set replayed on the GPU twice -- every draw
    with its pipeline specialised on the TEV's shape, made on the draw path
    (SOA_GPU_SPECIALIZE=wait), and with the interpreter alone (0). The two
    pictures must be byte-identical, which is what lets gxv draw with the
    interpreter while a specialised pipeline is made. A mutation goes to the
    specialised run alone, and must fail it."""
    code = ready(prof)
    if code is not None:
        return code
    out = build_dir(prof) / "specdiff"
    out.mkdir(parents=True, exist_ok=True)
    caps = captures(sets)
    differ, problems = [], []
    pipes = shapes = 0
    for cap in caps:
        name = cap.capture.name
        spec_png, interp_png = out / f"{name}_wait.png", out / f"{name}_interp.png"
        spec, _ = replay(
            prof, "gpu", cap.capture.base, spec_png, mutate, env={"SOA_GPU_SPECIALIZE": "wait"}
        )
        if skipped(spec):
            return SKIP
        interp, _ = replay(
            prof, "gpu", cap.capture.base, interp_png, env={"SOA_GPU_SPECIALIZE": "0"}
        )
        m = RE_SPEC.search(spec.stderr)
        if spec.returncode or interp.returncode or not m:
            problems.append(f"{name}: a replay failed or said nothing of its pipelines")
            continue
        pipes += int(m.group(1))
        shapes += int(m.group(2))
        if not same_pixels(spec_png, interp_png):
            differ.append(name)
    for p in problems:
        print(f"PROBLEM {p}")
    for name in differ:
        print(f"PROBLEM {name}: specialised and interpreted differ")
    print(
        f"[gpuspike] specdiff: {len(caps) - len(differ) - len(problems)} of {len(caps)} captures "
        f"the same pixels specialised and interpreted; {pipes} specialised pipelines for "
        f"{shapes} shapes, summed over the captures"
    )
    return 1 if differ or problems else 0


def contrast(
    prof: toolchain.Profile, sets: list[str], mutate: str | None, exe: Path | None = None
) -> int:
    """V5's same-session contrast. Each capture is copied once into
    build/gpuspike/<cc>/contrast/<set>/ -- soa.exe --replay writes <base>.png
    beside it -- and replayed by the spike and by gen/soa.exe (or --exe:
    distribution R1's gen/mingw/soa.exe) with SOA_GPU=vulkan. Pass: every pair
    of pictures has the same pixels and every run started on the same device.
    A mutation goes to soa.exe alone, and must fail it."""
    code = ready(prof)
    if code is not None:
        return code
    exe = (exe if exe is not None else ROOT / "gen" / f"soa{prof.exeext}").resolve()
    if not exe.exists():
        print(f"skip: no {exe} (python tools/recompile.py --link)")
        return SKIP
    out = build_dir(prof) / "contrast"
    caps = captures(sets)
    starts: set[str] = set()
    same, differ, problems = 0, [], []
    for cap in caps:
        d = out / cap.setname
        d.mkdir(parents=True, exist_ok=True)
        base = d / cap.capture.name
        for suffix in (".fifo", ".regs", ".ram"):
            shutil.copyfile(f"{cap.capture.base}{suffix}", f"{base}{suffix}")
        spike_png = d / f"{cap.capture.name}_spike.png"
        spike, _ = replay(prof, "gpu", base, spike_png)
        if skipped(spike):
            return SKIP
        env = {**clean_env(), "SOA_GPU": "vulkan"}
        if mutate:
            env["SOA_GPU_MUTATE"] = mutate
        port = subprocess.run(
            [str(exe), "--replay", str(base)],
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
            errors="replace",
            check=False,
        )
        at = f"{cap.setname}/{cap.capture.name}"
        for who, proc in (("spike", spike), ("soa.exe", port)):
            text = (proc.stdout or "") + (proc.stderr or "")
            found = GXV_START.findall(text)
            if proc.returncode != 0 or len(found) != 1:
                problems.append(f"{at}: {who} exit {proc.returncode}, {len(found)} start lines")
            starts.update(found)
        port_png = Path(f"{base}.png")
        if not spike_png.exists() or not port_png.exists():
            problems.append(f"{at}: a picture is missing")
            continue
        if png.read_rgba(spike_png)[2] == png.read_rgba(port_png)[2]:
            same += 1
        else:
            differ.append(at)
    for line in sorted(starts):
        print(line)
    if len(starts) > 1:
        problems.append(f"{len(starts)} different start lines: the two binaries did not run alike")
    for p in problems:
        print(f"PROBLEM {p}")
    print(f"[gpuspike] contrast: {same} of {len(caps)} captures the same pixels, spike and soa.exe")
    if differ:
        print(f"  differ: {' '.join(differ[:12])}{' ...' if len(differ) > 12 else ''}")
    ok = not problems and same == len(caps) and len(caps) > 0
    print(f"[gpuspike] contrast {'passes' if ok else 'FAILS'}")
    return 0 if ok else 1


def reproduced(a: Path, b: Path) -> set[str]:
    """The snapshots two runs made byte for byte alike, by name."""
    theirs = {p.name for p in b.glob("*.png")}
    return {
        p.name
        for p in a.glob("*.png")
        if p.name in theirs and png.read_rgba(p)[2] == png.read_rgba(b / p.name)[2]
    }


def compare_frames(
    cpu: Path, gpu: Path, only: set[str] | None = None
) -> tuple[list[str], list[str], int]:
    """Each snapshot in cpu against the one of the same name in gpu, by V0 --
    of those named in only, when given: (the frames that fail it, with V0's
    reasons; problems -- a frame one side lacks, or none at all; and how many
    were identical)."""
    names = sorted(p.name for p in cpu.glob("*.png") if only is None or p.name in only)
    problems = [] if names else [f"no snapshots in {cpu}"]
    theirs = {p.name for p in gpu.glob("*.png") if only is None or p.name in only}
    problems += [f"{n}: the GPU run has none" for n in names if n not in theirs]
    problems += [f"{n}: the CPU run has none" for n in sorted(theirs - set(names))]
    fails, same = [], 0
    for n in names:
        if n not in theirs:
            continue
        if png.read_rgba(cpu / n)[2] == png.read_rgba(gpu / n)[2]:
            same += 1
            continue
        f = imgdiff.compare(cpu / n, gpu / n).failures()
        if f:
            fails.append(f"{n}: {'; '.join(f)}")
    return fails, problems, same


def live(
    prof: toolchain.Profile, name: str, frames_range: str | None, mutate: str | None, seed: str
) -> int:
    """A scenario on each renderer, then compare_frames. Run one at a time,
    as every soa.exe run is (CLAUDE.md)."""
    exe = ROOT / "gen" / f"soa{prof.exeext}"
    if not exe.exists():
        print(f"skip: no {exe.relative_to(ROOT)} (python tools/recompile.py --link)")
        return SKIP
    out = build_dir(prof) / "live" / name
    dirs = {side: out / side for side in ("cpu", "cpu2", "gpu")}
    for side, d in dirs.items():
        shutil.rmtree(d, ignore_errors=True)
        env = [f"SOA_SEED={seed}", f"SOA_FRAMES_DIR={d}"]
        args = []
        if frames_range:
            first, last = frames_range.split("-")
            env.append(f"SOA_SNAP=1@{int(first)}-{int(last)}")
            args += ["--frames", str(int(last) + 1)]
        if side == "gpu":
            env.append("SOA_GPU=vulkan")
            if SCALE > 1:
                # V9a: the native picture, its centre samples at 3, under V0
                # with mip levels chosen as at scale 1.
                env += [f"SOA_GPU_SCALE={SCALE}", "SOA_GPU_SCALE_LOD=native"]
            if mutate:
                env.append(f"SOA_GPU_MUTATE={mutate}")
        cmd = [
            sys.executable,
            str(ROOT / "tools" / "scenario.py"),
            "run",
            name,
            "--check",
            "--quiet",
        ]
        cmd += [*args, "--log", str(out / f"{side}.log")]
        for e in env:
            cmd += ["--env", e]
        proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, check=False)
        verdict = next(
            (ln for ln in proc.stdout.splitlines() if "invariants hold" in ln or "FAILED" in ln), ""
        )
        print(f"[gpuspike] {side}: {verdict or 'no verdict'}")
        if proc.returncode != 0:
            print(proc.stdout[-2000:])
            return 1
    total = len(list(dirs["cpu"].glob("*.png")))
    stable = reproduced(dirs["cpu"], dirs["cpu2"])
    print(
        f"[gpuspike] {len(stable)} of {total} frames the same in both CPU runs; the other {total - len(stable)} are not judged"
    )
    if not stable:
        print("skip: no frame of this stretch reproduces on the CPU here, so nothing can be judged")
        return SKIP
    fails, problems, same = compare_frames(dirs["cpu"], dirs["gpu"], stable)
    for p in problems:
        print(f"PROBLEM {p}")
    for f in fails[:12]:
        print(f"  FAIL {f}")
    print(
        f"[gpuspike] live {name}: {len(stable) - len(fails)} of {len(stable)} reproducible frames "
        f"pass V0 ({same} identical), SOA_SEED={seed}"
    )
    ok = not problems and not fails
    print(f"[gpuspike] live {'passes' if ok else 'FAILS'}")
    return 0 if ok else 1


RE_QUEUE = re.compile(r"^queue textured quads right (\d+) of 64; frame hash ([0-9a-f]{16})$", re.M)
RE_ARENA = re.compile(r"^\[gxr\] waits:.*?arena [0-9.]+s \((\d+)\)", re.M)


def queue_run(prof: toolchain.Profile, backend: str, env: dict, variant: str = "", mutate=None):
    """One queue frame: (textured quads right, frame hash, arena drains), or
    None with the output printed when the driver said nothing of the kind."""
    out = build_dir(prof) / "queue" / backend
    out.mkdir(parents=True, exist_ok=True)
    args = [str(exe_path(prof, variant)), "--backend", backend, "--out", str(out), "--queue", "1"]
    if mutate:
        args += ["--mutate", mutate]
    proc = subprocess.run(
        args, cwd=ROOT, env={**clean_env(), **env}, capture_output=True, text=True, check=False
    )
    text = proc.stdout + proc.stderr
    m = RE_QUEUE.search(text)
    if skipped(proc):
        return "skip"
    if not m:
        print(text[-1500:])
        return None
    arena = RE_ARENA.search(text)
    return int(m.group(1)), m.group(2), int(arena.group(1)) if arena else 0


def queue(prof: toolchain.Profile, mutate: str | None) -> int:
    """V6a's queue frame on the CPU, then on the GPU: on its thread three
    times, inline (SOA_GXR_INLINE=1, V5's path) and stalled (SOA_GXR_STALL,
    2 ms before every draw on the consumer). Every GPU run must give the CPU's
    hash and all 64 quads; the runs on the thread, two arena drains each.
    count-early runs the variant build, stalled; pool-in-place is gxv's."""
    variant = mutate if mutate in VARIANTS else ""
    for v in {"", variant}:
        code = ready(prof, v)
        if code is not None:
            return code
    cpu = queue_run(prof, "cpu", {})
    if cpu in (None, "skip"):
        return 1
    print(f"cpu: {cpu[0]} of 64 quads right, hash {cpu[1]}, {cpu[2]} arena drains")
    runs = [("thread", {}), ("thread", {}), ("thread", {}), ("inline", {"SOA_GXR_INLINE": "1"})]
    runs.append(("stalled", {"SOA_GXR_STALL": "1:0:2000"}))
    if mutate:
        runs = [("stalled", {"SOA_GXR_STALL": "1:0:2000"})]
    problems = []
    for name, env in runs:
        got = queue_run(prof, "gpu", env, variant, None if variant else mutate)
        if got == "skip":
            return SKIP
        if got is None:
            problems.append(f"gpu {name}: no result")
            continue
        right, frame, drains = got
        print(f"gpu {name}: {right} of 64 quads right, hash {frame}, {drains} arena drains")
        if right != 64 or frame != cpu[1]:
            problems.append(f"gpu {name}: {right} of 64, hash {frame} where the CPU's is {cpu[1]}")
        if name != "inline" and drains < 2:
            problems.append(
                f"gpu {name}: {drains} arena drains, not the two the frame is built for"
            )
    if cpu[0] != 64 or cpu[2] < 2:
        problems.append(
            f"the CPU's frame is not the one intended: {cpu[0]} of 64, {cpu[2]} arena drains"
        )
    for p in problems:
        print(f"PROBLEM {p}")
    ok = not problems
    print(f"[gpuspike] queue {'passes' if ok else 'FAILS'}")
    return 0 if ok else 1


RE_LOGIC_OR = re.compile(
    r"^logictest or: centre (\d+) (\d+) (\d+); frame hash ([0-9a-f]{16})$", re.M
)
RE_LOGIC_XOR = re.compile(
    r"^logictest xor: overlap (\d+), first alone (\d+); frame hash ([0-9a-f]{16})$", re.M
)
RE_LOGIC_ROUTES = re.compile(
    r"logic ops: (\d+) draws: (\d+) native, (\d+) from a snapshot, (\d+) through the interlock, (\d+) as blends"
)


def logictest_run(prof: toolchain.Profile, backend: str, scene: int, logicop=None, features=None):
    """One of V10's logic scenes: (the parsed line, the routes or None, the
    output), or "skip"."""
    out = build_dir(prof) / "logictest"
    out.mkdir(parents=True, exist_ok=True)
    args = [str(exe_path(prof)), "--backend", backend, "--out", str(out), "--logictest", str(scene)]
    if logicop:
        args += ["--logicop", logicop]
    env = clean_env()
    if features:
        env["SOA_GPU_FEATURES"] = features
    proc = subprocess.run(args, cwd=ROOT, env=env, capture_output=True, text=True, check=False)
    if skipped(proc):
        return "skip"
    text = proc.stdout + proc.stderr
    m = (RE_LOGIC_OR if scene == 1 else RE_LOGIC_XOR).search(text)
    r = RE_LOGIC_ROUTES.search(text)
    return (m.groups() if m else None), (tuple(map(int, r.groups())) if r else None), text


def logictest(prof: toolchain.Profile) -> int:
    """V10: logic ops without logicOp, on two synthetic scenes, depth off.
    1, 0x55 ORed into 0xAA: 0xFF from the CPU, native, a snapshot, the
    interlock and the routes SOA_GPU_FEATURES=core takes; 198 from the forced
    blend -- so the modes differ where they should. 2, two overlapping
    triangles XORed onto black: the overlap comes back black from the CPU,
    native and the interlock, which SOA_GPU_FEATURES=nologicop must route it
    to; with core (no interlock either) it goes to a snapshot, said by name,
    and leaves the overlap 0xF0 -- never to blend, which cannot draw XOR."""
    code = ready(prof)
    if code is not None:
        return code
    problems = []

    def run(scene, backend="gpu", logicop=None, features=None):
        got = logictest_run(prof, backend, scene, logicop, features)
        if got == "skip":
            raise SystemExit(SKIP)
        return got

    cpu1, _, _ = run(1, "cpu")
    native1, routes, text = run(1)
    has_interlock = "; interlock yes" in text
    print(f"or: cpu {cpu1}; native {native1}")
    if not cpu1 or cpu1[0] != "255" or not native1 or native1[0] != "255":
        problems.append("the OR scene is not 0xFF from the CPU and native")
    for mode, want in (("snapshot", "255"), ("blend", "198")) + (
        (("interlock", "255"),) if has_interlock else ()
    ):
        got, _, _ = run(1, logicop=mode)
        print(f"or, {mode}: {got}")
        if not got or got[0] != want:
            problems.append(f"the OR scene through {mode} gives {got and got[0]}, not {want}")
    core1, routes1, _ = run(1, features="core")
    print(f"or, core: {core1}, routes {routes1}")
    if not core1 or core1[0] != "255" or not routes1 or routes1[2] != 1:
        problems.append("SOA_GPU_FEATURES=core does not draw the OR quad from a snapshot as 0xFF")

    cpu2, _, _ = run(2, "cpu")
    native2, _, _ = run(2)
    print(f"xor: cpu {cpu2}; native {native2}")
    if not cpu2 or cpu2[0] != "0" or not native2 or native2[0] != "0":
        problems.append("the XOR scene's overlap is not black from the CPU and native")
    if has_interlock:
        nl, routes2, _ = run(2, features="nologicop")
        print(f"xor, nologicop: {nl}, routes {routes2}")
        if not nl or nl[0] != "0" or nl[2] != native2[2] or not routes2 or routes2[3] != 1:
            problems.append(
                "with no logicOp the overlapping XOR is not routed to the interlock and drawn as native"
            )
    core2, routes3, text = run(2, features="core")
    print(f"xor, core: {core2}, routes {routes3}")
    if (
        not core2
        or core2[0] != "240"
        or not routes3
        or routes3[2] != 1
        or "may overlap itself" not in text
    ):
        problems.append(
            "with neither logicOp nor interlock the XOR is not a snapshot named in a line"
        )
    for p in problems:
        print(f"PROBLEM {p}")
    print(f"[gpuspike] logictest {'passes' if not problems else 'FAILS'}")
    return 0 if not problems else 1


RE_PRESENT = re.compile(r"^present (\d+) of (\d+) layouts exact, at scale (\d+)$", re.M)
RE_FILTERS = re.compile(r"^filters (\d+) of (\d+) cases exact, at scale (\d+)$", re.M)


def present(prof: toolchain.Profile, mutate: str | None, filters: bool = False) -> int:
    """V8's presenter: two synthetic screen copies through the GPU's present
    pass into eight target sizes at both layouts, each held to picture_scale's
    picture (the CPU presenter's), every pixel's colour exact. --mutate
    present (one column over) must fail it."""
    code = ready(prof)
    if code is not None:
        return code
    out = build_dir(prof) / "present"
    out.mkdir(parents=True, exist_ok=True)
    args = [str(exe_path(prof)), "--backend", "gpu", "--out", str(out), "--present"]
    args.append("2" if filters else "1")
    if mutate:
        args += ["--mutate", mutate]
    proc = subprocess.run(
        args, cwd=ROOT, env=clean_env(), capture_output=True, text=True, check=False
    )
    if skipped(proc):
        return SKIP
    print(proc.stdout.strip())
    m = (RE_FILTERS if filters else RE_PRESENT).search(proc.stdout)
    ok = proc.returncode == 0 and bool(m) and m.group(1) == m.group(2) and int(m.group(3)) == SCALE
    print(f"[gpuspike] present {'passes' if ok else 'FAILS'}")
    return 0 if ok else 1


RE_COPYIMAGE = re.compile(r"^copyimage cells right (\d+) of 16; frame hash ([0-9a-f]{16})$", re.M)
RE_LANDING = re.compile(
    r"copies to a texture: (\d+), landed with the submissions they were in but for (\d+) readback "
    r"waits of their own; (\d+) samplers served by a copy image"
)


def copyimage_run(prof: toolchain.Profile, backend: str, mutate: str | None = None):
    """V7's copy image frame: (cells right, frame hash, the landing figures or
    None), "skip", or None with the output printed."""
    out = build_dir(prof) / "copyimage" / backend
    out.mkdir(parents=True, exist_ok=True)
    args = [str(exe_path(prof)), "--backend", backend, "--out", str(out), "--copyimage", "1"]
    if mutate:
        args += ["--mutate", mutate]
    proc = subprocess.run(
        args, cwd=ROOT, env=clean_env(), capture_output=True, text=True, check=False
    )
    if skipped(proc):
        return "skip"
    text = proc.stdout + proc.stderr
    m = RE_COPYIMAGE.search(text)
    if not m:
        print(text[-3000:])
        return None
    land = RE_LANDING.search(text)
    return int(m.group(1)), m.group(2), tuple(map(int, land.groups())) if land else None


def copyimage(prof: toolchain.Profile, mutate: str | None) -> int:
    """V7's copy image: a frame that copies to a texture and samples the copy
    in the same frame, on the CPU and on the GPU. The GPU must give the CPU's
    frame hash and all 16 cells, sample the copy's image from its pool (the
    count above zero), and land the copy with the frame's own submission
    rather than a wait of its own. A mutation goes to the GPU run: cimg-cpu
    (the producer's image, not yet landed, sampled instead) and land-at-copy
    (V6's wait at every copy) must each fail it.

    At --scale 3 (V9a) the copy's image in the pool is three times its size,
    and the frame -- its centre samples -- must still be the CPU's, hash and
    all; copy-scale (the image sampled as if native) must fail it. At
    --scale 2, which has no centre sample, the cells alone are judged."""
    code = ready(prof)
    if code is not None:
        return code
    cpu = copyimage_run(prof, "cpu")
    if cpu in (None, "skip"):
        return 1
    print(f"cpu: {cpu[0]} of 16 cells right, hash {cpu[1]}")
    gpu = copyimage_run(prof, "gpu", mutate)
    if gpu == "skip":
        return SKIP
    problems = []
    if cpu[0] != 16:
        problems.append(f"the CPU's frame is not the one intended: {cpu[0]} of 16 cells")
    if gpu is None or gpu[2] is None:
        problems.append("gpu: no result, or no landing figures")
    else:
        right, frame, (copies, waits, served) = gpu
        print(
            f"gpu: {right} of 16 cells right, hash {frame}; {copies} copies to a texture, "
            f"{waits} readback waits, {served} samplers served by a copy image"
        )
        if right != 16 or (frame != cpu[1] and SCALE % 2):
            problems.append(f"gpu: {right} of 16, hash {frame} where the CPU's is {cpu[1]}")
        if not served:
            problems.append("gpu: no sampler served by a copy image from the pool")
        if not copies or waits >= copies:
            problems.append(f"gpu: {waits} readback waits for {copies} copies, not fewer")
    for p in problems:
        print(f"PROBLEM {p}")
    ok = not problems
    print(f"[gpuspike] copyimage {'passes' if ok else 'FAILS'}")
    return 0 if ok else 1


# The GPU runs of overlap: (SOA_GXR_STALL, other environment). The consumer
# is worker 1, so the stalls hold it before every draw, copy or clear.
GPU_OVERLAP_RUNS = [
    ("", ""),
    ("1:0:300", ""),
    ("1:1:20000", ""),
    ("1:2:20000", ""),
    ("", "SOA_GXR_TOKENWAIT=1"),
    ("1:1:20000", "SOA_GXR_TOKENWAIT=1"),
]


# The copies of overlap's stream that no textured draw touches: these the
# GPU must make byte for byte as the CPU does.
UNTEXTURED_FINALS = ("first copies", "second copies", "third copies")


def overlap(prof: toolchain.Profile, mutate: str | None) -> int:
    """V6b: the overlap stream with the GPU as the consumer, held to the
    GPU's own synchronous run (SOA_GXR_INLINE=1, V5's path, where no command
    can overlap another) on every final hash. Not to the CPU's: the stream's
    textured quads sample at half size, putting pixel centres on texel
    boundaries, where a coordinate's last bit picks the texel -- 3.4's
    sampling difference, by design (FINDINGS "V6b"). The CPU's one-worker run
    anchors the three copies no textured draw touches. The GPU build is the
    same driver with OVERLAP_GPU, gxv.c and plat.c, and the spike's SPIR-V."""
    sys.path.insert(0, str(ROOT / "tools" / "citest"))
    import queue_check

    code = ready(prof)
    if code is not None:
        return code
    out = build_dir(prof) / "overlap"
    out.mkdir(parents=True, exist_ok=True)
    try:
        cpu_exe = queue_check.build(
            "overlap-cpu", queue_check.OVERLAP_DRIVER, queue_check.OVERLAP_SOURCES, out, prof
        )
        gpu_exe = queue_check.build(
            "overlap-gpu",
            queue_check.OVERLAP_DRIVER,
            [*queue_check.OVERLAP_SOURCES, "gxv.c", "picture.c", "plat.c"],
            out,
            prof,
            ("/DSOA_GXV=1", "/DOVERLAP_GPU", f"/I{build_dir(prof)}", f"/I{shaders.HEADERS}"),
        )
    except queue_check.BuildError as e:
        print(f"FAIL: {e}")
        return 1
    code, text = queue_check.run(cpu_exe, queue_check.overlap_env(("1", "", "1", "")), out)
    cpu = queue_check.overlap_finals(text)
    if code or None in cpu.values():
        print(f"FAIL: the one-worker CPU run: exit {code}\n{text[-2000:]}")
        return 1
    code, text = queue_check.run(gpu_exe, {"SOA_THREADS": "1", "SOA_GXR_INLINE": "1"}, out)
    if code == SKIP and "skip: " in text:
        print(next(ln for ln in text.splitlines() if ln.startswith("skip: ")))
        return SKIP
    oracle = queue_check.overlap_finals(text)
    if code or None in oracle.values() or "on the producer" not in text:
        print(f"FAIL: the GPU's synchronous run: exit {code}\n{text[-2000:]}")
        return 1
    judged = list(queue_check.OVERLAP_FINAL)
    problems = [
        f"inline: {k} differs from the CPU's" for k in UNTEXTURED_FINALS if oracle[k] != cpu[k]
    ]
    print(
        f"{'FAIL' if problems else 'ok  '} gpu SOA_GXR_INLINE=1, the oracle: the untextured copies the CPU's"
    )
    for stall, other in GPU_OVERLAP_RUNS:
        env = queue_check.overlap_env(("1", stall, "", other))
        if mutate:
            env["SOA_GPU_MUTATE"] = mutate
        code, text = queue_check.run(gpu_exe, env, out)
        if code == SKIP and "skip: " in text:
            print(next(ln for ln in text.splitlines() if ln.startswith("skip: ")))
            return SKIP
        label = (
            "gpu" + (f" SOA_GXR_STALL={stall}" if stall else "") + (f" {other}" if other else "")
        )
        finals = queue_check.overlap_finals(text)
        why = (
            f"exit {code}"
            if code
            else "it did not run on the consumer thread"
            if "on a thread of its own" not in text
            else "the stall was not in force"
            if stall and "SOA_GXR_STALL: 1 stall in force" not in text
            else f"missing {[k for k in judged if finals[k] is None]}"
            if any(finals[k] is None for k in judged)
            else ""
        )
        if not why:
            differ = [k for k in judged if finals[k] != oracle[k]]
            if differ:
                why = f"{', '.join(differ)} differ from the GPU's synchronous run"
        if why:
            problems.append(f"{label}: {why}")
        print(f"{'FAIL' if why else 'ok  '} {label}")
    for p in problems:
        print(f"PROBLEM {p}")
    ok = not problems
    print(
        f"[gpuspike] overlap {'passes' if ok else 'FAILS'}: {len(GPU_OVERLAP_RUNS)} GPU runs, {len(judged)} hashes each"
    )
    return 0 if ok else 1


def logicop(prof: toolchain.Profile, mutate: str | None) -> int:
    """V4b: the mask effect's captures through the ways of drawing a logic op
    -- native, blend, snapshot and (V10, where the device has it) interlock --
    poisoned; byte-identical images (a same-replay contrast). V10 adds V1's
    captures that draw logic ops. With
    --mutate, the mutation is applied to the native path alone: the paths must
    then differ, and the native frame must fail V0 against the CPU's."""
    code = ready(prof)
    if code is not None:
        return code
    out = build_dir(prof) / "logicop"
    out.mkdir(parents=True, exist_ok=True)
    caps = logic_captures()
    bad = 0
    for cap in caps:
        c = cap.capture
        base = replay_base(cap, out / "poisoned")
        pngs, counts = {}, {}
        modes = list(LOGIC_MODES)
        for mode in LOGIC_MODES:
            if mode not in modes:
                continue
            pngs[mode] = out / f"{c.name}_{mode}.png"
            proc, _ = replay(
                prof, "gpu", base, pngs[mode], mutate if mode == "native" else None, mode
            )
            if skipped(proc):
                return SKIP
            if mode == "native" and "; interlock yes" not in proc.stderr:
                modes.remove("interlock")  # V10's route, where the device has it
            m = re.search(r"logic ops: (\d+) draws", proc.stderr)
            counts[mode] = int(m.group(1)) if proc.returncode == 0 and m else -1
        images = {mode: png.read_rgba(pngs[mode]) for mode in modes}
        w, h, _ = images["native"]
        rows = {mode: pixel_rows(w, h, images[mode][2]) for mode in modes}
        differ = {
            mode: sum(
                sum(map(ne, a, b, strict=True))
                for a, b in zip(rows["native"], rows[mode], strict=True)
            )
            for mode in modes
            if mode != "native"
        }
        same = images["blend"] == images["snapshot"]
        drawn = min(counts.values())
        line = (
            f"{c.name:14} logic draws {drawn}; native differs from blend at {differ['blend']} px, "
            f"from snapshot at {differ['snapshot']}, from interlock at {differ.get('interlock', '-')}; "
            f"blend {'=' if same else '!='} snapshot"
        )
        if mutate:
            ref = out / f"{c.name}_cpu.png"
            replay(prof, "cpu", base, ref)
            fails = bool(imgdiff.compare(ref, pngs["native"]).failures())
            ok = drawn > 0 and all(differ.values()) and same and fails
            line += f"; the mutated native frame {'fails' if fails else 'PASSES'} V0"
        else:
            ok = drawn > 0 and not any(differ.values()) and same
        bad += not ok
        print(f"{'ok  ' if ok else 'FAIL'} {line}")
    what = (
        f"--mutate {mutate}: every capture's paths differ and the native one fails V0"
        if mutate
        else ("every path gives byte-identical images")
    )
    print(f"[gpuspike] logicop over {len(caps)} captures: {what if not bad else f'{bad} FAIL'}")
    return 1 if bad else 0


def ramdiff(prof: toolchain.Profile, sets: list[str], mutate: str | None = None) -> int:
    """V4b: after each copy to a texture, the GPU's bytes against the CPU's,
    both poisoned: bytes written, bytes differing and the largest difference.
    Each differing byte is traced to the EFB as that copy found it (efb_at,
    on both paths): a texel can differ only where one of the pixels its
    filter reads does, the encoders being byte for byte the same (V3b)."""
    code = ready(prof)
    if code is not None:
        return code
    out = build_dir(prof) / "ramdiff"
    out.mkdir(parents=True, exist_ok=True)
    bad = 0
    copies_seen = 0
    for cap in (cap for cap in captures(sets) if cap.copies):
        c = cap.capture
        base = replay_base(cap, out / "poisoned")
        rams = {}
        for b in ("cpu", "gpu"):
            rams[b] = out / f"{c.name}_{b}.ram"
            proc, _ = replay(
                prof,
                b,
                base,
                out / f"{c.name}_{b}.png",
                mutate if b == "gpu" else None,
                dump_ram=rams[b],
            )
            if b == "gpu" and skipped(proc):
                return SKIP
        cpu_ram, gpu_ram = rams["cpu"].read_bytes(), rams["gpu"].read_bytes()
        for tc in cap.copies:
            copies_seen += 1
            a = cpu_ram[tc.dest : tc.dest + tc.extent]
            g = gpu_ram[tc.dest : tc.dest + tc.extent]
            written = sum(1 for at, n in tc.runs() for x in cpu_ram[at : at + n] if x != POISON)
            differ = [i for i in range(tc.extent) if a[i] != g[i]]
            largest = max((abs(a[i] - g[i]) for i in differ), default=0)
            untraced = 0
            if differ:
                efb = {}
                for b in ("cpu", "gpu"):
                    cut = scratch_capture(
                        c.base,
                        out / f"{c.name}_at{tc.index}_{b}",
                        cap.copies,
                        stream=efb_at(c.base, tc),
                    )
                    replay(prof, b, cut, out / f"{c.name}_at{tc.index}_{b}.png")
                    efb[b] = png.read_rgba(out / f"{c.name}_at{tc.index}_{b}.png")[2]
                for i in differ:
                    t = texel_of(i, tc)
                    if t is None:
                        untraced += 1
                        continue
                    sx = tc.x0 + (2 * t[0] if tc.half else t[0])
                    sy = tc.y0 + (2 * t[1] if tc.half else t[1])
                    rows = range(max(sy - 1, 0), min(sy + 2 + tc.half, 480))
                    cols = range(sx, min(sx + 1 + tc.half, 640))
                    if not any(
                        efb["cpu"][(y * 640 + x) * 4 : (y * 640 + x) * 4 + 3]
                        != efb["gpu"][(y * 640 + x) * 4 : (y * 640 + x) * 4 + 3]
                        for y in rows
                        for x in cols
                    ):
                        untraced += 1
            ok = written > 0 and not untraced
            bad += not ok
            print(
                f"{'ok  ' if ok else 'FAIL'} {c.name:14} copy {tc.index + 1} to {tc.dest:08X} (format {tc.texfmt}, "
                f"{tc.extent} bytes): {written} written, {len(differ)} differ, largest {largest}, "
                f"{untraced} not traced to the EFB"
            )
    print(
        f"[gpuspike] ramdiff over {copies_seen} copies: {'every difference is the EFB' if not bad else f'{bad} FAIL'}"
    )
    return 1 if bad else 0


def chain(prof: toolchain.Profile, names: list[str]) -> int:
    """V4b: a sequence of V1's captures N ... N+k, each frame's copies kept
    and spliced into the next frame's RAM, except where that frame copies
    itself. On the CPU the spliced bytes must equal what the next frame's RAM
    already holds there (no CPU write came between); each chained GPU frame
    must pass V0 against the chained CPU frame."""
    code = ready(prof)
    if code is not None:
        return code
    out = build_dir(prof) / "chain"
    out.mkdir(parents=True, exist_ok=True)
    caps = {cap.capture.name: cap for cap in captures(["gpuset"])}
    kept: dict[str, dict[int, bytes]] = {"cpu": {}, "gpu": {}}
    bad = 0
    for name in names:
        cap = caps[name]
        own = [(tc.dest, tc.dest + tc.extent) for tc in cap.copies]

        def outside(at: int, n: int, own: list[tuple[int, int]] = own) -> bool:
            return all(at + n <= lo or at >= hi for lo, hi in own)

        splice = {
            b: {at: data for at, data in kept[b].items() if outside(at, len(data))} for b in kept
        }
        ram = scenario.part(cap.capture.base, ".ram").read_bytes()
        mismatched = sum(
            sum(map(ne, ram[at : at + len(data)], data, strict=True))
            for at, data in splice["cpu"].items()
        )
        pngs = {}
        for b in ("cpu", "gpu"):
            base = scratch_capture(cap.capture.base, out / f"{name}_{b}", cap.copies, splice[b])
            pngs[b] = out / f"{name}_{b}.png"
            dump = out / f"{name}_{b}.ram"
            proc, _ = replay(prof, b, base, pngs[b], dump_ram=dump)
            if b == "gpu" and skipped(proc):
                return SKIP
            after = dump.read_bytes()
            for tc in cap.copies:
                kept[b] = {
                    at: d
                    for at, d in kept[b].items()
                    if at + len(d) <= tc.dest or at >= tc.dest + tc.extent
                }
                for at, n in tc.runs():
                    kept[b][at] = after[at : at + n]
            dump.unlink()
        metrics = imgdiff.compare(pngs["cpu"], pngs["gpu"])
        fails = metrics.failures()
        ok = not mismatched and not fails
        bad += not ok
        spliced = sum(len(d) for d in splice["cpu"].values())
        print(
            f"{'ok  ' if ok else 'FAIL'} {name}: {len(splice['cpu'])} range(s), {spliced} bytes spliced, "
            f"{'equal to its RAM' if not mismatched else f'{mismatched} byte(s) NOT equal to its RAM'}; "
            f"V0 {'passes' if not fails else 'FAILS'}: {metrics.line()}"
        )
    print(
        f"[gpuspike] chain over {len(names)} frames: {'every frame chains and passes' if not bad else f'{bad} FAIL'}"
    )
    return 1 if bad else 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "command",
        choices=(
            "build",
            "selftest",
            "tevdiff",
            "copydiff",
            "loddiff",
            "oracle",
            "time",
            "logicop",
            "ramdiff",
            "chain",
            "contrast",
            "live",
            "queue",
            "overlap",
            "specdiff",
            "copyimage",
            "present",
            "logictest",
        ),
    )
    ap.add_argument(
        "frames",
        nargs="*",
        help="chain: V1's captures in order, e.g. battle_4421 battle_4422; live: a scenario",
    )
    ap.add_argument("--range", default=None, help="live: every frame from A to B, as A-B")
    ap.add_argument("--live-seed", default="12345", help="live: the SOA_SEED both runs get")
    ap.add_argument("--cc", choices=tuple(toolchain.PROFILES), default="msvc")
    ap.add_argument("--mutate", default=None, help="a gxv mutation the command must fail on")
    ap.add_argument("--cases", type=int, default=100000, help="tevdiff and loddiff: random cases")
    ap.add_argument("--rects", type=int, default=200, help="copydiff: rectangles a combination")
    ap.add_argument("--seed", type=int, default=1, help="tevdiff, copydiff and loddiff")
    ap.add_argument("--set", default="corpus,perfset", help="oracle and time: capture sets")
    ap.add_argument("--mutations", action="store_true", help="oracle: also the GPU mutations")
    ap.add_argument("--runs", type=int, default=5, help="time: replays a capture")
    ap.add_argument(
        "--logicop",
        default=None,
        help="selftest: how gxv draws logic ops (native, blend, snapshot)",
    )
    ap.add_argument(
        "--specialize",
        choices=("0", "1", "wait"),
        default=None,
        help="SOA_GPU_SPECIALIZE for every run: 1 in the background, wait on the draw path, 0 never",
    )
    ap.add_argument(
        "--features",
        choices=("core",),
        default=None,
        help="SOA_GPU_FEATURES for every run: core treats every optional feature as absent",
    )
    ap.add_argument(
        "--exe",
        type=Path,
        default=None,
        help="contrast: the soa.exe to hold to the spike (default gen/soa.exe)",
    )
    ap.add_argument(
        "--filters",
        action="store_true",
        help="present: P5a's filters on the GPU against picture.c (V8b)",
    )
    ap.add_argument(
        "--scale",
        type=int,
        choices=(1, 2, 3),
        default=1,
        help="SOA_GPU_SCALE for every GPU run (V9a): copydiff, copyimage, present and oracle judge it",
    )
    args = ap.parse_args(argv)
    global SPECIALIZE, FEATURES, SCALE
    SPECIALIZE = args.specialize
    FEATURES = args.features
    SCALE = args.scale
    prof = toolchain.profile(args.cc)
    if args.command == "build":
        built, why = build(prof)
        print(f"built {exe_path(prof)}" if built else f"not built: {why}")
        return 0 if built else 1
    if args.command == "tevdiff":
        return tevdiff(prof, args.cases, args.seed, args.mutate)
    if args.command == "copydiff":
        return copydiff(prof, args.rects, args.seed, args.mutate)
    if args.command == "loddiff":
        return loddiff(prof, args.cases, args.seed, args.mutate)
    if args.command == "oracle":
        if SCALE > 1:
            return oracle_scaled(prof, args.set.split(","), SCALE, args.mutate)
        return oracle(prof, args.set.split(","), args.mutations)
    if args.command == "logicop":
        return logicop(prof, args.mutate)
    if args.command == "ramdiff":
        return ramdiff(prof, args.set.split(","), args.mutate)
    if args.command == "chain":
        return chain(prof, args.frames)
    if args.command == "overlap":
        return overlap(prof, args.mutate)
    if args.command == "queue":
        return queue(prof, args.mutate)
    if args.command == "live":
        if len(args.frames) != 1:
            ap.error("live takes one scenario, e.g. title")
        return live(prof, args.frames[0], args.range, args.mutate, args.live_seed)
    if args.command == "contrast":
        return contrast(prof, args.set.split(","), args.mutate, args.exe)
    if args.command == "specdiff":
        return specdiff(prof, args.set.split(","), args.mutate)
    if args.command == "copyimage":
        return copyimage(prof, args.mutate)
    if args.command == "present":
        return present(prof, args.mutate, args.filters)
    if args.command == "logictest":
        return logictest(prof)
    if args.command == "time":
        return time_frames(prof, args.set.split(","), args.runs, 3)
    return selftest(prof, args.mutate, args.logicop)


if __name__ == "__main__":
    raise SystemExit(main())
