"""Compile every runtime/*.c on its own, no linking and no generated code.

    python tools/citest/compile_runtime.py [--cc msvc|clang-cl|gcc|clang] [--out build/citest/runtime]

The runtime is the part of the port that is actually hand-written C, and
until this existed nothing built it except the owner's machine, so a syntax
error in runtime/ could reach main unnoticed. Each translation unit is
compiled with /c and the flags tools/soa/toolchain.py gives every unit, which
needs no disc, no gen/ and no link step: each runtime file declares for
itself the recompiled symbols it calls. A handful of warnings that mean a call
does not match the function it names -- the implicit declaration first among
them -- are promoted to errors here, since nothing links and they are all a
link step would have caught. Prints one line per file, with the compiler's
warnings underneath, and exits non-zero if any file fails or the compiler is
missing. --cc clang-cl builds with the clang-cl profile instead
(tools/soa/toolchain.py; portability.md 3.9), its own flags and strict set,
into build/citest/runtime-clang-cl.

gxv.c is compiled twice: as every file is, which is the stub a build without
the GPU backend links, and with SOA_GXV=1, which is the backend (V5). The
second needs Vulkan-Headers (python tools/fetch_gpu.py --headers) and is
given one-word stand-ins for the SPIR-V headers (tools/soa/shaders.py stub),
so it compiles all of the backend's C on any machine, glslang or not; without
the headers it is reported as not compiled, and --require-gxv, which CI
passes, makes that a failure.

--no-gx compiles the runtime a second time as a build without GX has it
(soa-ue5's U4.2; recompile.py --no-gx): every runtime file but gx.c, gxr.c,
gxr_tev.c and gxv.c, with SOA_NO_GX=1 (gx_none.c in their place), into
<out>/no-gx, so one run checks both builds.

window_sdl.c and audio_sdl.c, the window and the sound off Windows
(portability L10), are empty without SOA_SDL, so on Linux's gcc and clang
they are compiled again with SOA_SDL=1 against SDL3's headers (python
tools/fetch_sdl.py --headers), with the three files whose code SOA_SDL
changes; without the headers they are reported as not compiled, and
--require-sdl, which CI's Linux legs pass, makes that a failure.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import fetch_sdl  # noqa: E402
from soa import shaders, toolchain  # noqa: E402

RUNTIME = ROOT / "runtime"

# Runtime files this deliberately does not compile, and why. Empty today:
# decomp_swap.c, selftest.c and main.c are the only files that name recompiled
# or decompiled symbols (fn_XXXXXXXX, recomp_fn_XXXXXXXX, dc_*), and each one
# declares them itself, so none of them needs gen/functions.h to compile. A
# file that ever does belongs here with its reason, so that the log says
# plainly what is covered and what is not rather than quietly shrinking.
UNCOVERED: dict[str, str] = {}


# Warnings this check refuses. Without them a call to a function that no longer
# exists is C4013 -- "assuming extern returning int" -- and since nothing here
# links, renaming a runtime function and leaving a caller behind would pass.
# The rest are the same mistake seen through a prototype: a wrong argument
# count or type, a pointer indirection that does not match, a local read before
# it is written, a non-void function that falls off its end. They are promoted
# here rather than added to toolchain.CFLAGS, which has to stay exactly what
# the real build uses: this check may be stricter than the build, never
# different from it. The list lives with the profile (toolchain.MSVC.strict:
# C4013, C4020, C4024, C4028, C4029, C4047, C4133, C4700, C4716), and
# clang-cl's is the same mistakes in clang's words (implicit declarations,
# int and pointer conversions, a missing return).
STRICT = list(toolchain.MSVC.strict)


# What a build without GX leaves out (recompile.py's GX_FILES, soa-ue5's U4.2).
GX_FILES = ("gx.c", "gxr.c", "gxr_tev.c", "gxv.c")


def compile_one(
    path: Path, out: Path, prof: toolchain.Profile = toolchain.MSVC, defines: tuple[str, ...] = ()
) -> tuple[Path, int, str]:
    proc = toolchain.cc(
        [*prof.cflags, *prof.strict, *defines, "/c", f"/I{RUNTIME}", str(path), f"/Fo{out}/"],
        ROOT,
        prof,
    )
    return path, proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def compile_backend(
    out: Path, prof: toolchain.Profile = toolchain.MSVC, headers: Path = shaders.HEADERS
) -> tuple[bool | None, str]:
    """gxv.c with SOA_GXV=1, against the Vulkan-Headers under `headers` and
    stand-ins for the SPIR-V, into out/gxv: True compiled, False failed, None
    not compiled because there are no headers; and the compiler's output."""
    if not (headers / "vulkan" / "vulkan_core.h").exists():
        return None, ""
    stubs = out / "gxv-spirv-stubs"
    shaders.stub(stubs)
    (out / "gxv").mkdir(parents=True, exist_ok=True)
    proc = toolchain.cc(
        [
            *prof.cflags,
            *prof.strict,
            "/c",
            "/DSOA_GXV=1",
            f"/I{stubs}",
            f"/I{headers}",
            f"/I{RUNTIME}",
            str(RUNTIME / "gxv.c"),
            f"/Fo{out / 'gxv'}/",
        ],
        ROOT,
        prof,
    )
    return proc.returncode == 0, (proc.stdout or "") + (proc.stderr or "")


# What SOA_SDL changes, compiled again with it (L10): the two SDL files are
# empty without it, window.c's stubs must leave the names to them, and
# audio_out.c and main.c take another branch.
SDL_FILES = ("window_sdl.c", "audio_sdl.c", "window.c", "audio_out.c", "main.c")


def compile_sdl(
    out: Path, prof: toolchain.Profile, include: Path | None = None
) -> tuple[bool | None, str]:
    """SDL_FILES with SOA_SDL=1 against SDL3's headers under `include`, into
    out/sdl: True compiled, False failed, None not compiled because there are
    no headers; and the compiler's output."""
    include = include or fetch_sdl.headers(ROOT / "vendor")
    if not (include / "SDL3" / "SDL.h").exists():
        return None, ""
    (out / "sdl").mkdir(parents=True, exist_ok=True)
    proc = toolchain.cc(
        [
            *prof.cflags,
            *prof.strict,
            "/c",
            "/DSOA_SDL=1",
            f"/I{include}",
            f"/I{RUNTIME}",
            *(str(RUNTIME / f) for f in SDL_FILES),
            f"/Fo{out / 'sdl'}/",
        ],
        ROOT,
        prof,
    )
    return proc.returncode == 0, (proc.stdout or "") + (proc.stderr or "")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--cc", choices=tuple(toolchain.PROFILES), default="msvc", help="the toolchain profile"
    )
    ap.add_argument("--out", type=Path, default=None)
    ap.add_argument(
        "--require-gxv",
        action="store_true",
        help="fail when gxv.c cannot be compiled as the backend (no vendor/vulkan-headers)",
    )
    ap.add_argument(
        "--require-sdl",
        action="store_true",
        help="fail when the SDL window and sound cannot be compiled (no SDL3 headers; gcc and clang only)",
    )
    ap.add_argument(
        "--no-gx",
        action="store_true",
        help="compile the build without GX too (SOA_NO_GX, the four GX files left out) into <out>/no-gx",
    )
    args = ap.parse_args()
    prof = toolchain.profile(args.cc)
    if args.out is None:
        args.out = (
            ROOT
            / "build"
            / "citest"
            / ("runtime" if prof.name == "msvc" else f"runtime-{prof.name}")
        )

    cl = toolchain.compiler_path(prof)
    if cl is None:
        print(f"{prof.name} not found (see tools/soa/toolchain.py compiler_path)", file=sys.stderr)
        print("this check must not pass silently, so it fails instead", file=sys.stderr)
        return 1
    print(f"compiler: {cl}")
    print(f"flags:    {' '.join([*prof.cflags, *prof.strict])} /c /I{RUNTIME}\n")

    args.out.mkdir(parents=True, exist_ok=True)
    sources = sorted(RUNTIME.glob("*.c"))
    if not sources:
        print(f"no sources under {RUNTIME}", file=sys.stderr)
        return 1
    covered = [p for p in sources if p.name not in UNCOVERED]

    failed: list[Path] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for path, rc, out in pool.map(lambda p: compile_one(p, args.out, prof), covered):
            print(f"{'ok  ' if rc == 0 else 'FAIL'} {path.name}")
            if rc != 0:
                failed.append(path)
            # The echo of the file name is the only line MSVC prints when it is
            # happy; anything else is a diagnostic worth having in the log.
            for line in out.splitlines():
                if line.strip() and line.strip() != path.name:
                    print(f"      {line}")

    # The build without GX (--no-gx): every file but the four, with SOA_NO_GX, into a directory of its own.
    nogx_failed: list[Path] = []
    nogx = [p for p in covered if p.name not in GX_FILES]
    if args.no_gx:
        (args.out / "no-gx").mkdir(parents=True, exist_ok=True)
        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
            for path, rc, out in pool.map(
                lambda p: compile_one(p, args.out / "no-gx", prof, ("/DSOA_NO_GX=1",)), nogx
            ):
                print(f"{'ok  ' if rc == 0 else 'FAIL'} {path.name} without GX")
                if rc != 0:
                    nogx_failed.append(path)
                for line in out.splitlines():
                    if line.strip() and line.strip() != path.name:
                        print(f"      {line}")

    # The backend: gxv.c again, with SOA_GXV=1, into a directory of its own so
    # its object does not replace the stub's.
    gxv_note = "not compiled as the backend: no vendor/vulkan-headers (python tools/fetch_gpu.py --headers)"
    compiled, text = compile_backend(args.out, prof)
    if compiled is None:
        print(f"---- gxv.c with SOA_GXV=1 (the backend): {gxv_note}")
        backend_ok = not args.require_gxv
    else:
        print(f"{'ok  ' if compiled else 'FAIL'} gxv.c with SOA_GXV=1 (the backend)")
        for line in text.splitlines():
            if line.strip() and line.strip() != "gxv.c":
                print(f"      {line}")
        backend_ok = compiled
        gxv_note = "compiled as the backend too" if compiled else "FAILED as the backend"

    # The window and the sound off Windows (L10): Linux's profiles and
    # Android's (L12), since window_sdl.c is POSIX's (_exit from unistd.h) and
    # Windows keeps window.c.
    sdl_ok = True
    sdl_note = "not compiled with SOA_SDL: Windows keeps window.c (D2)"
    if prof.name in ("gcc", "clang") or prof in toolchain.ANDROID:
        sdl_note = (
            "not compiled with SOA_SDL: no SDL3 headers (python tools/fetch_sdl.py --headers)"
        )
        compiled, text = compile_sdl(args.out, prof)
        if compiled is None:
            print(f"---- {', '.join(SDL_FILES)} with SOA_SDL=1: {sdl_note}")
            sdl_ok = not args.require_sdl
        else:
            print(f"{'ok  ' if compiled else 'FAIL'} {', '.join(SDL_FILES)} with SOA_SDL=1")
            for line in text.splitlines():
                if line.strip() and line.strip() not in SDL_FILES:
                    print(f"      {line}")
            sdl_ok = compiled
            sdl_note = "compiled with SOA_SDL too" if compiled else "FAILED with SOA_SDL"
    elif args.require_sdl:
        sdl_ok = False

    print(f"\ncompiled {len(covered) - len(failed)}/{len(covered)} runtime translation units")
    if args.no_gx:
        print(f"without GX (SOA_NO_GX): compiled {len(nogx) - len(nogx_failed)}/{len(nogx)}")
    print(f"gxv.c: {gxv_note}")
    print(f"the SDL window and sound: {sdl_note}")
    if UNCOVERED:
        print("not compiled here:")
        for name, why in sorted(UNCOVERED.items()):
            print(f"  {name}: {why}")
    else:
        print("not compiled here: nothing, every runtime/*.c is covered")
    if failed or nogx_failed:
        print(f"::error::{len(failed) + len(nogx_failed)} runtime file(s) failed to compile")
        return 1
    if not backend_ok:
        print(f"::error::gxv.c {gxv_note}")
        return 1
    if not sdl_ok:
        print(f"::error::the SDL window and sound: {sdl_note}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
