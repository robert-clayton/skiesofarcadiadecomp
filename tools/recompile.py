#!/usr/bin/env python3
"""Translate the whole DOL to C.

    python tools/recompile.py [--dol extracted/sys/main.dol] [--out gen] [--chunk 400]
                              [--cc msvc|clang-cl|gcc|clang|mingw] [--compile] [--link] [--limit N]

Writes gen/functions.h, gen/dispatch.c and gen/chunk_NNN.c (gitignored: they
are derived from the game binary and are reproduced locally from the user's
own dump). Prints instruction coverage so what is *not* translated is always
visible. --compile runs the compiler over every chunk to prove the C is valid;
--link links them with runtime/ into soa.exe and builds the mods.

--cc picks a toolchain profile (tools/soa/toolchain.py; portability.md 3.9):
msvc by default. Another profile writes everything -- the C, the objects, the
exe -- under its own directory, gen/clang for clang-cl, so a clang build can
never be linked into gen/soa.exe. mingw (llvm-mingw, distribution R1) links
GNU-style into gen/mingw/soa.exe with no Microsoft compiler, and builds each
mod's mod.dll under gen/mingw/mods, beside a copy of its mod.ini and
patches.txt, never over the msvc build's beside mod.c.

--no-decomp is the player's build (specs/distribution.md 3.1), which has no
src/ or include/: the translation leaves out every binding runtime/
decomp_swap.c answers with decompiled code, so the game's own MSL runs
translated, the link builds no native unit and defines SOA_NO_DECOMP, and the
self test says the swap's comparison is skipped. Translate, compile and link
with it together: chunks translated without it name functions it never links.

--reproducible (mingw; distribution 3.8) links with no timestamp, no PDB and
the source tree's path mapped to `.`, so two builds of the same package and
disc give the same soa.exe byte for byte, in any folder. --progress prints a
`[build]` line per step and translation unit, which tools/player_build.py
passes to the setup window.
"""

import argparse
import collections
import concurrent.futures
import dataclasses
import hashlib
import os
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

# units.txt is read by one parser, which decomp.py owns (stdlib only, so the
# no-pip CI job that imports this file still needs nothing installed).
import fetch_gpu  # noqa: E402
import fetch_sdl  # noqa: E402
import player_build  # noqa: E402
from decomp import read_units  # noqa: E402
from soa import dol as D  # noqa: E402
from soa import elfcheck, embed, seam, shaders, toolchain  # noqa: E402
from soa import symbols as S  # noqa: E402
from soa.hle import load_hle  # noqa: E402
from soa.ppc import cfg  # noqa: E402
from soa.recomp import Emitter  # noqa: E402

RUNTIME = Path(__file__).resolve().parents[1] / "runtime"
VENDOR = Path(__file__).resolve().parents[1] / "vendor"

# Where mods with native code live: the ones the port ships, and the examples
# a mod author copies. --link builds each folder's mod.c into its mod.dll.
MOD_FOLDERS = ("mods", "examples/mods")


BUILD_INPUTS = "build_inputs.txt"


def write_build_inputs(out: Path, dol_sha1: str, profile: str) -> None:
    """After a --compile that succeeded: the executable its chunks were
    translated from, and the toolchain profile (disc-layer I3, portability
    3.9). --link holds the executable it builds in to this."""
    (out / BUILD_INPUTS).write_text(
        f"dol_sha1 = {dol_sha1}\nprofile = {profile}\n", encoding="utf-8"
    )


def read_build_inputs(out: Path) -> dict[str, str] | None:
    path = out / BUILD_INPUTS
    if not path.exists():
        return None
    rows = (ln.split("=", 1) for ln in path.read_text(encoding="utf-8").splitlines() if "=" in ln)
    return {k.strip(): v.strip() for k, v in rows}


def check_build_inputs(out: Path, dol_sha1: str) -> str | None:
    """Why --link must refuse: the chunks in `out` were compiled from another
    executable than the one about to be built in beside them, which would
    link old code to new data with nothing saying so. None when they agree,
    or when there is no record (a gen/ from before this: build_inputs_note)."""
    rec = read_build_inputs(out)
    if rec is None or rec.get("dol_sha1") == dol_sha1:
        return None
    return (
        f"{out}: its translated code was compiled from the executable with SHA-1 "
        f"{rec.get('dol_sha1')}, and this one is {dol_sha1}; run --compile again (stale link)"
    )


def build_inputs_note(out: Path) -> str | None:
    """The one-time note for a gen/ compiled before the record existed."""
    if read_build_inputs(out) is not None:
        return None
    return (
        f"note: {out} has no {BUILD_INPUTS}, so this link cannot check its code was compiled "
        "from this executable; the next --compile writes one"
    )


def system_files(
    disc: Path, dol_bytes: bytes, want_sha1: str, force: bool
) -> tuple[dict[str, bytes] | None, str | None]:
    """The executable, boot.bin and file table to build in (I3), from the
    disc at `disc`; or None and why not. Refused, unless forced, when the
    disc's executable is not the one config/ names, or differs from --dol."""
    from soa.disc import Disc, open_data

    try:
        data = open_data(disc)
    except (OSError, ValueError) as exc:
        return None, f"{disc}: {exc}"
    if not isinstance(data, Disc):
        return None, f"{disc}: no disc image to take the system files from (tools/extract.py)"
    with data:
        files = data.system_files()
    got = hashlib.sha1(files["main.dol"], usedforsecurity=False).hexdigest()
    if not force and got != want_sha1:
        return None, f"{disc}: its executable has SHA-1 {got}, not config/'s {want_sha1}"
    if not force and files["main.dol"] != dol_bytes:
        return None, f"{disc}: its executable is not --dol's (the code is translated from --dol)"
    return files, None


def mod_dll_sources(root: Path = RUNTIME.parent) -> list[Path]:
    """Every mod.c under MOD_FOLDERS, one per mod folder, in name order."""
    return sorted(p for d in MOD_FOLDERS for p in (root / d).glob("*/mod.c"))


def mod_dll_command(src: Path) -> list[str]:
    """The cl line --link builds a mod's mod.dll with, beside its mod.c
    (test_mods.py builds the shipped mods with this same line)."""
    folder = src.parent
    return [
        *toolchain.CFLAGS,
        "/LD",
        f"/I{RUNTIME}",
        str(src),
        f"/Fo{folder}{os.sep}",
        f"/Fe:{folder / 'mod.dll'}",
    ]


# The text files a mod folder's mod.dll is loaded beside: a mingw build copies
# them into its own mods/ folder with the mod.dll it builds.
MOD_TEXT = ("mod.ini", "patches.txt")


def mod_out_dir(p: toolchain.Profile, out: Path, src: Path) -> Path:
    """Where --link puts a mod's library: beside its mod.c for msvc, under
    <out>/mods/<folder> for mingw (distribution R1) and Linux's gcc and clang
    (portability L10), absolute."""
    if p.name == "msvc":
        return src.parent.resolve()
    return (out / "mods" / src.parent.name).resolve()


# A reproducible mingw link (distribution 3.8): no timestamp in the headers,
# and the source tree's path, which __FILE__ would write into the exe, as `.`.
def reproducible_flags() -> list[str]:
    return ["-Wl,--no-insert-timestamp", f"-ffile-prefix-map={RUNTIME.parent}=."]


def gnu_mod_dll_command(
    p: toolchain.Profile, src: Path, dest: Path, reproducible: bool = False
) -> list[str]:
    """A GNU profile's line for a mod's library, written into dest: mod.dll
    under mingw, mod.so (position-independent) on Linux."""
    lib = "mod.dll" if p.name == "mingw" else "mod.so"
    return [
        *p.cflags,
        "-shared",
        *([] if p.name == "mingw" else ["-fPIC"]),
        *(reproducible_flags() if reproducible else []),
        f"/I{RUNTIME}",
        str(src.resolve()),
        f"/Fe{dest / lib}",
    ]


# ---- the command lines, one profile at a time (portability.md 3.9, L3a) ----
# Pure functions: test_toolchain_profiles.py holds the msvc profile's to a
# golden copy and checks that clang-cl's write only under gen/clang.


def out_dir(p: toolchain.Profile, given: Path | None) -> Path:
    """Where a build goes: --out, or the profile's own directory (clang-cl:
    gen/clang). A profile other than msvc may not write into msvc's: --link
    links whatever objects its directory holds, so a clang-cl runtime would
    join MSVC's chunks in gen/soa.exe, which replay --bless trusts by path."""
    out = given if given is not None else Path(p.out)
    if p is not toolchain.MSVC and out.resolve() == Path(toolchain.MSVC.out).resolve():
        raise ValueError(
            f"--cc {p.name} --out {given}: {toolchain.MSVC.out} is the msvc build's; "
            f"leave --out unset for {p.out}"
        )
    return out


# The optimisation flag in each grammar, and what --compile uses without
# --optimize.
_OPT_LEVELS = {"/O2": "/Od", "-O2": "-O0"}


def opt_level(p: toolchain.Profile, optimize: bool) -> str:
    """The level --compile builds the translated C at, in the profile's grammar."""
    flag = next(f for f in p.cflags if f in _OPT_LEVELS)
    return flag if optimize else _OPT_LEVELS[flag]


def compile_command(p: toolchain.Profile, out: Path, path: Path, optimize: bool) -> list[str]:
    """--compile's line for one translation unit, run in `out`. Validation
    compiles at /Od (-O0) unless --optimize: the point is to prove the C is
    well-formed, and /O2 over 50 MB of it takes many minutes."""
    level = opt_level(p, optimize)
    flags = [level if f in _OPT_LEVELS else f for f in p.cflags]
    return [
        *flags,
        "/c",
        f"/I{RUNTIME}",
        f"/I{out}",
        path.name,
        f"/Fo{path.with_suffix(p.objext).name}",
    ]


def decomp_command(
    p: toolchain.Profile, out: Path, dc_files: list[str], dc_defines: list[str]
) -> list[str]:
    """The native build of the hand-decompiled units, run in the repository root."""
    return [*p.cflags, "/c", "/Iinclude", *dc_defines, f"/Fo{out / 'decomp'}/", *dc_files]


def decomp_objects(p: toolchain.Profile, out: Path, dc_files: list[str]) -> list[Path]:
    """The objects decomp_command makes: one per unit, named after it."""
    return sorted(out / "decomp" / (Path(f).stem + p.objext) for f in dc_files)


def gnu_link_command(
    p: toolchain.Profile,
    out: Path,
    objs: list[Path],
    gxv: bool = False,
    defines: tuple[str, ...] = (),
    reproducible: bool = False,
    sdl: tuple[list[str], list[str]] | None = None,
) -> list[str]:
    """A GNU profile's link: mingw's (distribution R1), msvc's in clang's
    words, where -gcodeview and lld's --pdb write <out>/soa.pdb, which
    SOA_HOSTPROF's report reads through dbghelp as it reads MSVC's, and a
    reproducible link writes none (reproducible_flags); or Linux's gcc and
    clang (portability L10), with neither. The libraries and the stack are the
    profile's linker flags, last, after every source and object.

    `sdl` is fetch_sdl.link_args' pair on Linux when vendor/sdl3 holds this
    host's SDL3 (L10): SOA_SDL and the headers on the compile, so
    window_sdl.c and audio_sdl.c are the window and the sound, and the
    static library and what it needs after the objects."""
    windows = p.name == "mingw"
    if not windows:
        debug: list[str] = []
    else:
        debug = reproducible_flags() if reproducible else ["-gcodeview"]
    return [
        *p.cflags,
        *debug,
        *defines,
        f"/I{RUNTIME}",
        f"/I{out}",
        *(["/DSOA_GXV=1", f"/I{out / 'gxv'}", f"/I{shaders.HEADERS}"] if gxv else []),
        *(sdl[0] if sdl else []),
        f"/Fe{out / ('soa' + p.exeext)}",
        *map(str, sorted(RUNTIME.glob("*.c"))),
        str(out / "disc_sys.c"),
        str(out / "game_table.c"),
        *map(str, objs),
        *([] if reproducible or not windows else [f"-Wl,--pdb={out / 'soa.pdb'}"]),
        *(sdl[1] if sdl else []),
        *p.linker,
    ]


# A binding runtime/decomp_swap.c answers: the one record of which bindings
# need src/, since each of its adapters calls a dc_ function built from there.
_SWAPPED = re.compile(r"^void fn_([0-9A-F]{8})\(CpuState\* s\)", re.M)


def decomp_bound(swap: Path = RUNTIME / "decomp_swap.c") -> set[int]:
    """The addresses decomp_swap.c answers with decompiled code: the
    bindings a build without src/ leaves out (--no-decomp)."""
    return {int(a, 16) for a in _SWAPPED.findall(swap.read_text(encoding="utf-8"))}


def link_command(
    p: toolchain.Profile,
    out: Path,
    objs: list[Path],
    gxv: bool = False,
    defines: tuple[str, ...] = (),
) -> list[str]:
    """The link of runtime/ and the objects into soa.exe, run in the root.

    gxv builds runtime/gxv.c as the GPU backend (SOA_GXV=1), against
    vendor/'s Vulkan-Headers and the SPIR-V headers shaders.build wrote into
    <out>/gxv; without it gxv.c is the stub that says the build has none.

    /Zi and /DEBUG write <out>/soa.pdb without changing the code /O2 makes
    (/OPT:REF and /OPT:ICF are what /DEBUG would otherwise turn off): the
    runtime's functions and lines get names, which SOA_HOSTPROF's report and a
    crash's stack both need. The translated chunks carry no debug information
    and add only their public names.

    Every runtime file is globbed, so runtime_support_sources() is not added
    here: it is for the tests that link a few runtime files, and plat.c would
    otherwise be on this line twice."""
    return [
        *p.cflags,
        "/Zi",
        f"/Fd{out}/runtime.pdb",
        *defines,
        f"/I{RUNTIME}",
        f"/I{out}",
        *(["/DSOA_GXV=1", f"/I{out / 'gxv'}", f"/I{shaders.HEADERS}"] if gxv else []),
        f"/Fo{out}/",
        f"/Fe:{out / ('soa' + p.exeext)}",
        *map(str, sorted(RUNTIME.glob("*.c"))),
        str(out / "disc_sys.c"),  # the player's system files (disc-layer I3), compiled every link
        str(
            out / "game_table.c"
        ),  # the game's table (specs/android.md 3.2), held to these by the self test
        *map(str, objs),
        *p.linker,
        "/link",
        "/DEBUG",
        "/OPT:REF",
        "/OPT:ICF",
        "/STACK:33554432",  # guest call depth becomes host call depth
    ]


def split_link_plan(
    p: toolchain.Profile,
    out: Path,
    gxv: bool = False,
    defines: tuple[str, ...] = (),
    sdl: tuple[list[str], list[str]] | None = None,
) -> list[tuple[list[str], Path]]:
    """--split's links (specs/android.md L12a), Android's arrangement on Linux:
    libsoa_runtime.so (every runtime file with SOA_SPLIT, and runtime_seam.c),
    exporting exactly runtime.map's names; libsoa_game.so (the translated
    objects, disc_sys.c and game_table.c, hidden but for soa_game), which
    needs the runtime by name and may not leave a symbol it does not have
    (--no-undefined); and the launcher. Both libraries are laid out for 16 KB
    pages, as Android's must be. Then the mods, as the single-file build's."""
    objs = sorted(out.glob("chunk_*" + p.objext)) + [out / ("dispatch" + p.objext)]
    runtime = [
        *p.cflags,
        "-fPIC",
        "-shared",
        "/DSOA_SPLIT=1",
        *defines,
        f"/I{RUNTIME}",
        f"/I{out}",
        *(["/DSOA_GXV=1", f"/I{out / 'gxv'}", f"/I{shaders.HEADERS}"] if gxv else []),
        *(sdl[0] if sdl else []),
        f"/Fe{out / seam.RUNTIME_SONAME}",
        *map(str, sorted(RUNTIME.glob("*.c"))),
        str(out / "runtime_seam.c"),
        f"-Wl,-soname,{seam.RUNTIME_SONAME}",
        f"-Wl,--version-script={out / 'runtime.map'}",
        "-Wl,-z,max-page-size=16384",
        *(sdl[1] if sdl else []),
        *p.linker,
        "-ldl",
    ]
    game = [
        *p.cflags,
        "-fPIC",
        "-fvisibility=hidden",
        "-shared",
        f"/I{RUNTIME}",
        f"/I{out}",
        f"/Fe{out / seam.GAME_SONAME}",
        str(out / "disc_sys.c"),
        str(out / "game_table.c"),
        *map(str, objs),
        f"-Wl,-soname,{seam.GAME_SONAME}",
        "-Wl,-z,max-page-size=16384",
        "-Wl,--no-undefined",
        f"-L{out}",
        f"-l:{seam.RUNTIME_SONAME}",
        "-lm",
    ]
    launcher = [
        *p.cflags,
        f"/Fe{out / ('soa' + p.exeext)}",
        str(out / "launcher.c"),
        f"-L{out}",
        f"-l:{seam.RUNTIME_SONAME}",
        "-Wl,-rpath,$ORIGIN",
        *p.linker,
    ]
    plan = [(runtime, Path(".")), (game, Path(".")), (launcher, Path("."))]
    plan += [
        (gnu_mod_dll_command(p, src, mod_out_dir(p, out, src)), mod_out_dir(p, out, src))
        for src in mod_dll_sources()
    ]
    return plan


def android_link_plan(p: toolchain.Profile, out: Path) -> list[tuple[list[str], Path]]:
    """An Android profile's links (specs/android.md L12b): a stand-in
    libsoa_runtime.so from <out>/stub/stub_runtime.c, then libsoa_game.so
    against it -- the translated objects, disc_sys.c and game_table.c, every
    function hidden but soa_game (the profile's flags), needing the runtime by
    name, laid out for 16 KB pages, and refused at link time if it calls
    anything the runtime does not export (--no-undefined). No runtime, mods or
    GPU here: the APK carries those (L12c, L12f, L12g)."""
    objs = sorted(out.glob("chunk_*" + p.objext)) + [out / ("dispatch" + p.objext)]
    stub = out / "stub"
    stand_in = [
        *p.cflags,
        "-fvisibility=default",
        "-shared",
        f"/Fe{stub / seam.RUNTIME_SONAME}",
        str(stub / "stub_runtime.c"),
        f"-Wl,-soname,{seam.RUNTIME_SONAME}",
    ]
    game = [
        *p.cflags,
        "-shared",
        f"/I{RUNTIME}",
        f"/I{out}",
        f"/Fe{out / seam.GAME_SONAME}",
        str(out / "disc_sys.c"),
        str(out / "game_table.c"),
        *map(str, objs),
        f"-Wl,-soname,{seam.GAME_SONAME}",
        "-Wl,-z,max-page-size=16384",
        "-Wl,--no-undefined",
        f"-L{stub}",
        f"-l:{seam.RUNTIME_SONAME}",
        *p.linker,
    ]
    return [(stand_in, Path(".")), (game, Path("."))]


def builds_mods(p: toolchain.Profile) -> bool:
    """The msvc profile builds mods/*/mod.c beside each, for gen/soa.exe;
    mingw, gcc and clang build them under their own directory (mod_out_dir):
    mod.dll, or mod.so on Linux (L9 loads it, L10 builds it). clang-cl's build
    must touch nothing outside its own."""
    return p.name in ("msvc", "mingw", "gcc", "clang")


def link_plan(
    p: toolchain.Profile,
    out: Path,
    dc_files: list[str],
    dc_defines: list[str],
    gxv: bool = False,
    defines: tuple[str, ...] = (),
    reproducible: bool = False,
    sdl: tuple[list[str], list[str]] | None = None,
) -> list[tuple[list[str], Path]]:
    """Every compiler command --link runs, in order, each with its working
    directory: the decompiled units, the link, then each mod (msvc and
    mingw). `defines` go on the link's runtime compile: SOA_NO_DECOMP for
    --no-decomp."""
    objs = sorted(out.glob("chunk_*" + p.objext)) + [out / ("dispatch" + p.objext)]
    plan: list[tuple[list[str], Path]] = []
    if dc_files:
        plan.append((decomp_command(p, out, dc_files, dc_defines), Path(".")))
        objs += decomp_objects(p, out, dc_files)
    if p.style == "gnu":
        plan.append((gnu_link_command(p, out, objs, gxv, defines, reproducible, sdl), Path(".")))
    else:
        plan.append((link_command(p, out, objs, gxv, defines), Path(".")))
    if p.name == "msvc":
        plan += [(mod_dll_command(src), src.parent) for src in mod_dll_sources()]
    elif builds_mods(p):
        plan += [
            (
                gnu_mod_dll_command(p, src, mod_out_dir(p, out, src), reproducible),
                mod_out_dir(p, out, src),
            )
            for src in mod_dll_sources()
        ]
    return plan


# A definition or declaration starting in column 1; the name is the last
# identifier before the parameter list. ``typedef`` is excluded because a
# function-pointer typedef ends in a parameter list of its own, and the
# identifier in front of that list is the return type -- so
# ``typedef void (*ARCallback)(void);`` reads as a function called "void".
_FUNC_DEF = re.compile(
    r"^(?!typedef\b)[A-Za-z_][^\n;{}=]*?\b(\w+)\s*\([^;{}]*\)\s*(?:\n\{|;)", re.M
)

# Every C89/C99 keyword, so that a declaration shaped like one of those
# typedefs is caught rather than turned into a /D that redefines the language.
_KEYWORD_TEXT = """
auto break case char const continue default do double else enum extern float for goto if
inline int long register restrict return short signed sizeof static struct switch typedef
union unsigned void volatile while _Bool _Complex _Imaginary
"""
_C_KEYWORDS = frozenset(_KEYWORD_TEXT.split())


class RenameError(Exception):
    """A unit's scan produced a rename that must never reach the compiler."""


def native_decomp_sources(units: Path) -> tuple[list[str], list[str]]:
    """The units marked ``native`` in config/GEAE8P/units.txt, and the /D renames
    that prefix every function they define or declare with dc_ (a declared
    callee that is not decompiled yet comes from runtime/decomp_shims.c).
    A unit earns ``native`` by compiling under MSVC, reading nothing whose
    meaning depends on byte order, touching no memory-mapped register, and
    calling nothing undecompiled; units that read the game's globals fail the
    second of those until the native build maps them onto guest memory."""
    files = [u.src for u in read_units(units) if u.native] if units.exists() else []
    names = set()
    for f in files:
        for name in _FUNC_DEF.findall(f.read_text(encoding="utf-8")):
            # A keyword here means the scan misread a declaration. Emitting the
            # /D anyway would define the keyword away over every unit in the
            # build, and the damage would surface as an unrelated syntax error
            # in whichever file used it next.
            if name in _C_KEYWORDS:
                raise RenameError(
                    f"{f}: read the C keyword '{name}' as a function name. "
                    f"A rename of '{name}' would break every unit in the native build; "
                    f"fix _FUNC_DEF in {Path(__file__).name} rather than the unit."
                )
            names.add(name)
    return [str(f) for f in files], [f"/D{n}=dc_{n}" for n in sorted(names)]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dol", type=Path, default=Path("extracted/sys/main.dol"))
    ap.add_argument(
        "--disc",
        type=Path,
        default=Path("extracted"),
        help="the disc whose executable, boot.bin and file table are built in (disc-layer I3)",
    )
    ap.add_argument(
        "--no-embed",
        action="store_true",
        help="build none in: the runtime reads them from the image, and says so",
    )
    ap.add_argument(
        "--force",
        action="store_true",
        help="build in a disc's system files even when its executable is not config/'s or --dol's",
    )
    ap.add_argument("--config", type=Path, default=Path("config"))
    ap.add_argument(
        "--cc",
        choices=list(toolchain.PROFILES),
        default="msvc",
        help="the toolchain profile; a non-msvc one writes to its own --out (clang-cl: gen/clang)",
    )
    ap.add_argument(
        "--out",
        type=Path,
        default=None,
        help="where the C, objects and exe go (default the profile's: gen, gen/clang, gen/linux)",
    )
    ap.add_argument("--chunk", type=int, default=400, help="functions per C file")
    ap.add_argument("--limit", type=int, default=0, help="translate only the first N functions")
    ap.add_argument("--compile", action="store_true", help="compile every chunk with --cc")
    ap.add_argument("--optimize", action="store_true", help="compile at /O2 instead of /Od")
    ap.add_argument(
        "--link", action="store_true", help="link the objects with runtime/ into soa.exe"
    )
    ap.add_argument(
        "--reproducible",
        action="store_true",
        help="mingw: no timestamp, no PDB, the source path mapped to . (distribution 3.8)",
    )
    ap.add_argument(
        "--progress", action="store_true", help="a [build] line per step and unit (player_build)"
    )
    ap.add_argument(
        "--no-decomp",
        action="store_true",
        help="the player's build, with no src/: the translated MSL runs (distribution 3.1)",
    )
    ap.add_argument(
        "--split",
        action="store_true",
        help="gcc or clang: libsoa_runtime.so, libsoa_game.so and a launcher, as Android loads them "
        "(specs/android.md L12a), into <the profile's directory>-split; implies --no-decomp",
    )
    ap.add_argument(
        "--host",
        action="store_true",
        help="--split for a program that loads the runtime and is its window and sound "
        "(runtime/soa_host.h): SOA_HOST in place of SDL, into <the profile's directory>-host; "
        "implies --split",
    )
    args = ap.parse_args()
    prof = toolchain.profile(args.cc)
    # An Android profile builds the game library alone, as the player's PC
    # does for the phone (specs/android.md L12b): no src/, as the APK's
    # runtime has none.
    android = prof in toolchain.ANDROID
    if android:
        args.no_decomp = True
    if args.host:
        args.split = True
        if args.out is None:
            args.out = Path(prof.out + "-host")
    if args.split:
        if prof.name not in ("gcc", "clang"):
            ap.error("--split builds Android's arrangement on Linux: --cc gcc or --cc clang")
        args.no_decomp = True  # the player's build, and the APK's runtime: no src/
        if args.out is None:
            args.out = Path(prof.out + "-split")
    # A clang build goes to its own directory, so it can never be linked into
    # gen/soa.exe: --link globs whatever objects --out holds.
    try:
        args.out = out_dir(prof, args.out)
    except ValueError as exc:
        ap.error(str(exc))
    # The msvc profile's lines are the ones the docs quote (TESTING.md).
    shown = "MSVC" if prof is toolchain.MSVC else prof.name
    into = "" if prof is toolchain.MSVC else f" into {args.out}"

    if args.progress:
        print("[build] translate", flush=True)
    dol_bytes = args.dol.read_bytes()
    dol_sha1 = hashlib.sha1(dol_bytes, usedforsecurity=False).hexdigest()
    # The stale-link guard, before anything is parsed: a link of chunks
    # compiled from another executable is refused (disc-layer I3).
    if args.link and not args.compile:
        stale = check_build_inputs(args.out, dol_sha1)
        if stale:
            print(stale, file=sys.stderr)
            return 1
    if args.no_embed:
        system = None
    else:
        from soa.dump import read_project

        want = read_project(args.config / "GEAE8P" / "config.yml").sha1.lower()
        system, why = system_files(args.disc, dol_bytes, want, args.force)
        if system is None:
            print(f"{why}; --no-embed builds without them", file=sys.stderr)
            return 1
    dol = D.parse(dol_bytes)
    t0 = time.time()
    functions, _ = cfg.build_iterative(dol)
    code = cfg.CodeView(dol)
    tables = cfg.find_jump_tables(dol, code)
    print(f"{len(functions):,} functions, {len(tables)} switch tables ({time.time() - t0:.1f}s)")

    names = {}
    tsv = args.config / "functions.tsv"
    if tsv.exists():
        names = {a: r["name"] for a, r in S.load_tsv(tsv).items()}

    hle = load_hle(args.config / "hle.txt")
    if args.no_decomp:
        swapped = decomp_bound()
        hle = {a: n for a, n in hle.items() if a not in swapped}
        print(
            f"no src/ (--no-decomp): {len(swapped)} bindings to decompiled code left out; "
            "the translated MSL runs"
        )
    hooks = load_hle(args.config / "hooks.txt")
    savepoints = load_hle(args.config / "savepoints.txt")
    traces = load_hle(args.config / "trace.txt")
    if hle or hooks or savepoints:
        print(
            f"{len(hle)} functions bound to HLE, {len(hooks)} runtime hooks, "
            f"{len(savepoints)} savepoints"
        )
    em = Emitter(dol, functions, tables, code, names, hle, hooks, savepoints, traces)
    entries = sorted(functions)
    if args.limit:
        entries = entries[: args.limit]

    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "functions.h").write_text(em.prototypes(), encoding="utf-8")
    (args.out / "dispatch.c").write_text(em.dispatch_c(), encoding="utf-8")
    (args.out / "disc_sys.c").write_text(embed.disc_sys_c(system), encoding="utf-8")
    # The game's table and record (specs/android.md 3.2), in every build; the
    # runtime's side of the seam, its version script and the launcher for a
    # split one.
    the_seam = seam.read_seam(args.config / "seam.txt")
    baked = seam.baked_digest(player_build.inputs_record(RUNTIME.parent))
    (args.out / "game_table.c").write_text(
        seam.game_table_c(
            the_seam, hle, entries, seam.record(args.no_decomp, baked, dol_sha1, prof.name)
        ),
        encoding="utf-8",
    )
    if android:
        (args.out / "stub").mkdir(exist_ok=True)
        (args.out / "stub" / "stub_runtime.c").write_text(
            seam.stub_runtime_c(the_seam, hle), encoding="utf-8"
        )
    if args.split:
        (args.out / "runtime_seam.c").write_text(
            seam.runtime_seam_c(the_seam, hle, seam.record(True, baked)), encoding="utf-8"
        )
        (args.out / "runtime.map").write_text(
            seam.version_script(the_seam, hle, host=args.host), encoding="utf-8"
        )
        (args.out / "launcher.c").write_text(seam.LAUNCHER_C, encoding="utf-8")
    print(
        "system files: built in (disc_sys.c; never share this build)"
        if system is not None
        else "system files: not built in (--no-embed): the runtime reads the image's"
    )

    t0 = time.time()
    chunks: list[Path] = []
    for n in range(0, len(entries), args.chunk):
        body = ['#include "functions.h"', ""]
        for entry in entries[n : n + args.chunk]:
            body.append(em.function_c(functions[entry]))
            body.append("")
        path = args.out / f"chunk_{n // args.chunk:03d}.c"
        path.write_text("\n".join(body), encoding="utf-8")
        chunks.append(path)
    st = em.stats
    total_bytes = sum(p.stat().st_size for p in chunks)
    print(
        f"emitted {len(entries):,} functions into {len(chunks)} files, "
        f"{total_bytes / 1024**2:.1f} MB of C ({time.time() - t0:.1f}s)"
    )

    print(
        f"\ninstruction coverage: {st.coverage_pct:.3f}%  "
        f"({sum(st.translated.values()):,} translated, {sum(st.unsupported.values()):,} not)"
    )
    if st.oe_ignored:
        print(f"  OE forms translated without overflow tracking: {st.oe_ignored}")
    if st.unsupported:
        print("  unsupported, by mnemonic:")
        for mn, c in st.unsupported.most_common():
            print(f"    {mn:16} {c:>7,}")

    if args.compile:
        if toolchain.compiler_path(prof) is None:
            print(f"\n{shown} not found; skipping compile", file=sys.stderr)
            if android:
                print("no Android NDK in any of these, in order:", file=sys.stderr)
                for place in toolchain.android_ndk_places():
                    print(f"  {place}", file=sys.stderr)
            return 1
        units = [args.out / "dispatch.c", *chunks]
        level = opt_level(prof, args.optimize)
        # A split build's game library is a shared object, every function in
        # it hidden but the table (specs/android.md 3.2).
        cprof = (
            dataclasses.replace(prof, cflags=(*prof.cflags, "-fPIC", "-fvisibility=hidden"))
            if args.split
            else prof
        )
        print(f"\ncompiling {len(units)} translation units with {shown} ({level}){into} ...")
        t0 = time.time()

        def build(path: Path):
            return path, toolchain.cc(
                compile_command(cprof, args.out, path, args.optimize), args.out, prof
            )

        failures = collections.Counter()
        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
            for n, (path, proc) in enumerate(pool.map(build, units), 1):
                if args.progress:
                    print(f"[build] {n}/{len(units)} {path.name}", flush=True)
                if proc.returncode != 0:
                    failures[path.name] += 1
                    errs = [ln for ln in proc.stdout.splitlines() if "error" in ln.lower()]
                    print(f"  FAIL {path.name}: {errs[0] if errs else proc.stdout[-300:]}")
        elapsed = time.time() - t0
        print(f"compiled {len(units) - len(failures)}/{len(units)} units in {elapsed:.1f}s")
        if failures:
            return 1
        write_build_inputs(args.out, dol_sha1, prof.name)

    if args.link:
        if toolchain.compiler_path(prof) is None:
            print(f"\n{shown} not found; skipping link", file=sys.stderr)
            return 1
        exe = args.out / ("soa" + prof.exeext)
        note = build_inputs_note(args.out)
        if note:
            print(note)
        t0 = time.time()
        # The hand-decompiled units (src/) are built natively too, every function
        # renamed dc_<name> so they sit beside the C runtime's own strlen and
        # friends; the selftest runs them against their recompiled twins.
        try:
            dc_files, dc_defines = (
                ([], [])
                if args.no_decomp
                else native_decomp_sources(Path("config/GEAE8P/units.txt"))
            )
        except RenameError as exc:
            print(exc, file=sys.stderr)
            return 1
        except FileNotFoundError as exc:
            print(
                f"{exc.filename}: no decompiled source here; --no-decomp builds without src/",
                file=sys.stderr,
            )
            return 1
        if dc_files:
            (args.out / "decomp").mkdir(parents=True, exist_ok=True)
        # The GPU backend (specs/gpu-backend.md 3.10), when tools/fetch_gpu.py
        # has filled vendor/: the shaders to SPIR-V first, and a shader that
        # does not compile fails the build. Without vendor/ soa.exe is built
        # as before, and SOA_GPU=vulkan says how to get the backend.
        gxv = shaders.available() and not android
        if gxv:
            bad = fetch_gpu.verify(VENDOR)
            if bad:
                print(
                    f"vendor/ is not as recorded ({bad[0]}); run python tools/fetch_gpu.py",
                    file=sys.stderr,
                )
                return 1
            built, why = shaders.build(args.out / "gxv")
            if not built:
                print(why, file=sys.stderr)
                return 1
            print("GPU backend: built in (SOA_GPU=vulkan)")
        elif not android:
            print("GPU backend: not built in (python tools/fetch_gpu.py, then --link again)")
        # The window and the sound off Windows (portability L10), when
        # tools/fetch_sdl.py has built SDL3 for this host into vendor/sdl3.
        # Without it the Linux build is as before: headless, the run's own
        # watchdog and SOA_WAV.
        sdl = None
        if args.host:
            # The program that loads the runtime is the window and the sound
            # (runtime/host.c), and may well carry an SDL of its own.
            print("window and sound: the host's (runtime/soa_host.h)")
        elif prof.name in ("gcc", "clang"):
            if fetch_sdl.available(VENDOR):
                bad = fetch_sdl.verify(VENDOR)
                if bad:
                    print(
                        f"vendor/sdl3 is not as recorded ({bad[0]}); run python tools/fetch_sdl.py",
                        file=sys.stderr,
                    )
                    return 1
                sdl = fetch_sdl.link_args(VENDOR)
                print(f"window and sound: SDL3 {fetch_sdl.VERSION} (vendor/sdl3)")
            else:
                print(
                    "window and sound: none, headless (python tools/fetch_sdl.py, then --link again)"
                )
        failed_mods = 0
        defines = ("/DSOA_NO_DECOMP=1",) if args.no_decomp else ()
        if args.host:
            defines += ("/DSOA_HOST=1",)
        if android:
            plan = android_link_plan(prof, args.out)
        elif args.split:
            plan = split_link_plan(prof, args.out, gxv, defines, sdl)
        else:
            plan = link_plan(
                prof, args.out, dc_files, dc_defines, gxv, defines, args.reproducible, sdl
            )
        exe_flags = (f"/Fe:{exe}", f"/Fe{exe}")
        for cmd, cwd in plan:
            if args.progress:
                what = "link" if any(a in exe_flags for a in cmd) else f"mod {cwd.name}"
                if cwd == Path(".") and what != "link":
                    what = "decompiled units"
                print(f"[build] {what}", flush=True)
            if cwd != Path(".") and prof.style == "gnu":
                # the mod's own text beside the library this build makes
                src = next(s for s in mod_dll_sources() if s.parent.name == cwd.name)
                cwd.mkdir(parents=True, exist_ok=True)
                for name in MOD_TEXT:
                    if (src.parent / name).exists():
                        (cwd / name).write_bytes((src.parent / name).read_bytes())
            proc = toolchain.cc(cmd, cwd, prof)
            if cwd != Path("."):  # a mod's library, beside its mod.c or under <out>/mods
                lib = "mod.so" if prof.name in ("gcc", "clang") else "mod.dll"
                try:
                    where = cwd.relative_to(RUNTIME.parent) / lib
                except ValueError:  # a player's folder, outside the source tree
                    where = cwd / lib
                if proc.returncode != 0:
                    out_text = proc.stdout + proc.stderr
                    errs = [ln for ln in out_text.splitlines() if "error" in ln.lower()]
                    print(
                        f"  FAIL {where}: {errs[0] if errs else out_text[-300:]}",
                        file=sys.stderr,
                    )
                    failed_mods += 1
                else:
                    print(f"built {where}")
                continue
            if proc.returncode != 0:
                print((proc.stdout + proc.stderr)[-2000:], file=sys.stderr)
                return 1
            if any(a in exe_flags for a in cmd):
                print(f"linked {exe} ({time.time() - t0:.1f}s)")
            elif args.split or android:
                made = next(a[3:] for a in cmd if a.startswith("/Fe"))
                print(f"linked {made} ({time.time() - t0:.1f}s)")
        if failed_mods:
            return 1
        if android:
            # The phone's own checks (runtime/elfcheck.c), here first, in its
            # words: what the player would be told after copying it over.
            lib = args.out / seam.GAME_SONAME
            found = elfcheck.problems(
                elfcheck.read(lib),
                machine=elfcheck.EM_AARCH64
                if prof is toolchain.ANDROID_ARM64
                else elfcheck.EM_X86_64,
                exports=seam.runtime_exports(the_seam, hle),
                libc=the_seam.libc,
                record=seam.record(True, baked),
                path=str(lib),
            )
            for problem in found:
                print(f"  {problem}", file=sys.stderr)
            if found:
                return 1
            print(f"checked {lib}: the phone's runtime would load it")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
