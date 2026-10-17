"""Build soa-compare: a recording's calls replayed and held (docs/RECORD.md).

    python tools/replay.py [--out DIR] [--cc gcc|clang] [-I DIR] [-D NAME[=V]] [CANDIDATE.c|.cpp ...]

writes DIR/libsoa_runtime.so (tools/replay/replay.c: the replay, and the
runtime's side of the seam as a replay needs it) and DIR/soa-compare
(tools/replay/compare.c with the candidates given linked in; a .cpp among them
links with the C++ compiler). Then

    DIR/soa-compare REC --game gen/linux-split/libsoa_game.so

replays a recording SOA_RECORD made (runtime/record.c) against the game
library of the build that made it. Linux (and any ELF system with gcc or
clang): the replay is page protection and dlopen. The default DIR is
build/replay, which is ignored, like everything a recording touches.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "runtime"
REPLAY = ROOT / "tools" / "replay"

CFLAGS = [
    "-O2",
    "-g",
    "-fno-strict-aliasing",
    "-Wall",
    "-Wextra",
    "-Wno-unused-parameter",
    "-Wno-comment",
]


def compilers(cc: str) -> tuple[str, str]:
    """The C and C++ compilers of a profile name, from PATH."""
    c, cxx = ("clang", "clang++") if cc == "clang" else ("gcc", "g++")
    if not shutil.which(c):
        raise SystemExit(f"replay.py: no {c} on PATH")
    return c, cxx


def build_commands(
    out: Path,
    candidates: list[Path],
    includes: list[str] = (),
    defines: list[str] = (),
    cc: str = "gcc",
) -> list[list[str]]:
    """The two links: the replay library, then soa-compare with the candidates."""
    c, cxx = compilers(cc)
    inc = [f"-I{RUNTIME}", f"-I{REPLAY}", *(f"-I{i}" for i in includes)]
    dfs = [f"-D{d}" for d in defines]
    lib = [
        c,
        *CFLAGS,
        "-fPIC",
        "-shared",
        "-fvisibility=hidden",
        *inc,
        *dfs,
        "-o",
        str(out / "libsoa_runtime.so"),
        str(REPLAY / "replay.c"),
        "-Wl,-soname,libsoa_runtime.so",
        "-ldl",
        "-lm",
    ]
    cpp = any(p.suffix in (".cpp", ".cc", ".cxx") for p in candidates)
    objs = [[c, *CFLAGS, "-c", *inc, *dfs, "-o", str(out / "compare.o"), str(REPLAY / "compare.c")]]
    linked = [str(out / "compare.o")]
    for p in candidates:
        if p.suffix in (".cpp", ".cc", ".cxx"):
            o = out / (p.stem + ".o")
            objs.append([cxx, "-std=c++17", *CFLAGS, "-c", *inc, *dfs, "-o", str(o), str(p)])
            linked.append(str(o))
        else:
            linked.append(str(p))
    exe = [
        cxx if cpp else c,
        *CFLAGS,
        *inc,
        *dfs,
        "-o",
        str(out / "soa-compare"),
        *linked,
        f"-L{out}",
        "-l:libsoa_runtime.so",
        "-Wl,-rpath,$ORIGIN",
        "-lm",
    ]
    return [lib, *objs, exe]


def build(out: Path, candidates: list[Path], includes=(), defines=(), cc: str = "gcc") -> Path:
    out.mkdir(parents=True, exist_ok=True)
    for cmd in build_commands(out, candidates, list(includes), list(defines), cc):
        r = subprocess.run(cmd, capture_output=True, text=True, check=False)
        if r.returncode:
            sys.stderr.write(" ".join(cmd) + "\n" + r.stdout + r.stderr)
            raise SystemExit(r.returncode)
    return out / "soa-compare"


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "candidates", nargs="*", type=Path, help="C or C++ files of candidates (replay.h)"
    )
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "replay")
    ap.add_argument("--cc", choices=("gcc", "clang"), default=os.environ.get("SOA_CC", "gcc"))
    ap.add_argument("-I", dest="includes", action="append", default=[], help="an include directory")
    ap.add_argument("-D", dest="defines", action="append", default=[], help="a define")
    args = ap.parse_args()
    if os.name == "nt":
        print("replay.py: the replay needs an ELF system (Linux)", file=sys.stderr)
        return 2
    exe = build(
        args.out, [p.resolve() for p in args.candidates], args.includes, args.defines, args.cc
    )
    print(exe)
    return 0


if __name__ == "__main__":
    sys.exit(main())
