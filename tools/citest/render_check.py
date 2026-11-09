"""Build the renderer on its own and run its two pixel checks.

    python tools/citest/render_check.py [--cc PROFILE] [--threads N] [--cflag FLAG]...
                                        [--out build/citest/render]

runtime/selftest.c draws a full-screen quad and the title screen's cloud
triangle through the real GX front end and counts what comes out, but it can
only run inside the port, which needs the user's disc. The renderer itself
needs nothing: gx.c, gx_frame.c, gxr.c, gxr_tev.c and png.c reach only two symbols outside
themselves, and tools/citest/render_driver.c supplies both, so the same checks
link into a binary of their own and run anywhere. This is the one check in
tools/citest/ that looks at a pixel rather than at whether the C compiles.
Since L6 the driver also holds plat_f2i to cvttss2si and draws a frame from
out-of-range conversions whose blocks and hash are pinned; every leg must
draw it alike, and --cflag=-DPLAT_F2I_SATURATE (ARM64's conversion) fails it.
And it checks that a copy to memory honours its destination stride (BP 0x4D),
which the battle transition depends on (FINDINGS "The battle transition").
Prints the compiler's output, then the driver's; exits non-zero if the build
fails, the compiler is missing, either check disagrees, or the renderer did
not start the N workers --threads asks for: a pool that quietly fell back to
one thread would pass every pixel check (L4b; on Linux, --threads 4 is the
POSIX pool's first run).
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from soa import toolchain  # noqa: E402

PROF = toolchain.MSVC  # --cc sets it

HERE = Path(__file__).resolve().parent
RUNTIME = ROOT / "runtime"
# The renderer and the front end that feeds it. No recompiled code, no device
# models, no window: anything else in runtime/ would drag in the whole port.
SOURCES = [RUNTIME / name for name in ("gx.c", "gx_frame.c", "gxr.c", "gxr_tev.c", "png.c")]
SOURCES.append(HERE / "render_driver.c")


def run_cl(what: str, args: list[str]) -> bool:
    proc = toolchain.cc(args, ROOT, PROF)
    out = ((proc.stdout or "") + (proc.stderr or "")).strip()
    if out:
        print(out)
    if proc.returncode != 0:
        print(f"::error::{what} failed", file=sys.stderr)
        return False
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--cc", choices=tuple(toolchain.PROFILES), default="msvc", help="the toolchain profile"
    )
    ap.add_argument("--threads", type=int, default=1, help="worker threads (SOA_THREADS)")
    ap.add_argument(
        "--cflag", action="append", default=[], help="added to every compile and link (L8: TSAN)"
    )
    ap.add_argument("--out", type=Path, default=None)
    args = ap.parse_args()
    global PROF
    PROF = toolchain.profile(args.cc)
    if args.out is None:
        args.out = (
            ROOT / "build" / "citest" / ("render" if PROF.name == "msvc" else f"render-{PROF.name}")
        )

    cl = toolchain.compiler_path(PROF)
    if cl is None:
        print(f"{PROF.name} not found (see tools/soa/toolchain.py compiler_path)", file=sys.stderr)
        print("this check must not pass silently, so it fails instead", file=sys.stderr)
        return 1
    print(f"compiler: {cl}")

    args.out.mkdir(parents=True, exist_ok=True)
    if not run_cl(
        "compiling the renderer",
        [*PROF.cflags, *args.cflag, "/c", f"/I{RUNTIME}", f"/Fo{args.out}/", *map(str, SOURCES)],
    ):
        return 1
    exe = args.out / f"render_check{PROF.exeext}"
    # Named rather than globbed, so a stale object from an earlier run in the
    # same directory cannot slip into the link.
    objs = [args.out / (s.stem + PROF.objext) for s in SOURCES]
    if not run_cl(
        "linking the renderer",
        [*PROF.cflags, *args.cflag, *map(str, objs), f"/Fe:{exe}", *PROF.linker],
    ):
        return 1

    proc = subprocess.run(
        [str(exe), "--threads", str(args.threads)],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    print(proc.stdout, end="")
    if proc.stderr:
        print(proc.stderr, end="")
    if proc.returncode != 0:
        print(f"::error::{proc.returncode} render check(s) failed")
        return 1
    want = f"[gxr] rasterizing on {args.threads} worker thread"
    if want not in proc.stdout + proc.stderr:
        print(
            f'::error::asked for {args.threads} worker thread(s), and the renderer never said "{want}"'
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
