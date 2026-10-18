#!/usr/bin/env python3
"""Build the game into a folder from a disc image: what a player's setup runs.

    python tools/player_build.py --disc <image.rvz|iso|gcm> --root <folder>

specs/distribution.md R2. One command, from the player's own disc image to a
folder that plays:

1. **The disc is checked before any work** (3.8): its game id must be GEAE8P
   and its executable's SHA-1 the one config/ describes, each refused by
   name; an image that is not a GameCube disc, and one this cannot check, are
   refused too.
2. **What the game reads is extracted** into <root>/extracted (the image the
   runtime reads, and sys/), as tools/extract.py does.
3. **Never a stale gen/:** every input the translated C bakes in is hashed
   into <root>/gen/build-inputs.txt. When any differs from the last build's,
   the old translation is thrown away and the game is translated again, and
   the log says which input moved. --rebuild translates again whatever the
   record says: the setup window's Rebuild (3.8).
4. **The build:** recompile.py with the bundled compiler (the mingw profile:
   SOA_MINGW, the package's toolchain/, or vendor/), --no-decomp (no src/ in
   a package, 3.1) and --reproducible, on every core.
5. soa.exe and the mods' mod.dll go to the top of <root>, and soa.ini is
   written, unless one is there already.

It prints one machine-readable line per step and translation unit --
`[build] check`, `[build] 7/19 chunk_006.c` -- which the setup window (R4)
reads, and ends with `[build] done soa.exe sha256 <hex>`. Exit status 0 when
the folder plays, 1 when a step failed, 2 when the disc was refused.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from soa.disc import IMAGE_NAME, Disc, ImageFile  # noqa: E402
from soa.dump import ProjectError, read_project  # noqa: E402
from soa.rvz import RVZ  # noqa: E402

SOURCE = Path(__file__).resolve().parents[1]  # the repository, or a package's source/
GAME_ID = "GEAE8P"
REFUSED = 2

# Everything the translated C bakes in (CLAUDE.md, "Relink, or retranslate"):
# what the chunks include, the binding lists, and the translator itself.
BAKED = (
    "runtime/cpu.h",
    "config/hle.txt",
    "config/hooks.txt",
    "config/savepoints.txt",
    "config/resumepoints.txt",
    "config/trace.txt",
    "config/functions.tsv",
    "runtime/decomp_swap.c",
    "tools/recompile.py",
    "tools/soa/recomp",
    "tools/soa/ppc",
    "tools/soa/hle.py",
    "tools/soa/dol.py",
)


class Refused(Exception):
    """The disc is not one this package builds; the message names why."""


def say(what: str) -> None:
    print(f"[build] {what}", flush=True)


def open_image(path: Path):
    return RVZ(str(path)) if path.suffix.lower() == ".rvz" else ImageFile(path)


def check_disc(image: Path, game_id: str = GAME_ID, dol_sha1: str | None = None) -> None:
    """Refuse an image that is not this game's disc, by name (3.8). dol_sha1
    is config/'s when None; a config that cannot be read is a refusal too,
    never a warning: nothing after this looks at the executable again."""
    if dol_sha1 is None:
        try:
            dol_sha1 = read_project(SOURCE / "config" / "GEAE8P" / "config.yml").sha1
        except (ProjectError, OSError) as exc:
            raise Refused(f"cannot check the disc's executable: {exc}") from exc
    try:
        disc = Disc(open_image(image), source=str(image))
    except (ValueError, OSError, EOFError) as exc:
        raise Refused(f"{image.name} is not a GameCube disc image ({exc})") from exc
    with disc:
        found = disc.boot.game_id
        if found != game_id:
            raise Refused(
                f"this is not the North American GameCube release ({game_id}); "
                f'{image.name} is {found} "{disc.boot.title}"'
            )
        got = hashlib.sha1(disc.read_dol(), usedforsecurity=False).hexdigest()
    if got != dol_sha1:
        raise Refused(
            f"this disc's executable is not the one this package was made for "
            f"(sha1 {got}, wanted {dol_sha1})"
        )


def extract(image: Path, out: Path) -> None:
    """The image the runtime reads and sys/, as tools/extract.py writes them."""
    import extract as ex

    args = argparse.Namespace(image=image, out=out, files=False, iso=False, force=False)
    out.mkdir(parents=True, exist_ok=True)
    with Disc(open_image(image), source=str(image)) as disc:
        if ex.unpack(disc, args):
            raise RuntimeError("extraction failed")


def inputs_record(source: Path | None = None) -> dict[str, str]:
    """sha256 of every input the translation bakes in, by path: under SOURCE,
    read when called, not when this module was imported."""
    source = SOURCE if source is None else source
    out: dict[str, str] = {}
    for rel in BAKED:
        p = source / rel
        files = sorted(p.rglob("*.py")) if p.is_dir() else [p]
        for f in files:
            if f.exists():
                out[f.relative_to(source).as_posix()] = hashlib.sha256(f.read_bytes()).hexdigest()
    return out


def read_record(path: Path) -> dict[str, str]:
    if not path.exists():
        return {}
    rows = (ln.split("  ", 1) for ln in path.read_text(encoding="utf-8").splitlines() if ln)
    return {name: digest for digest, name in rows}


def stale(gen: Path, now: dict[str, str]) -> list[str]:
    """The inputs that moved since gen/ was translated; every one when there
    is no record, which is a translation never made or never finished."""
    before = read_record(gen / "build-inputs.txt")
    if not before:
        return ["no record of the last translation"]
    return sorted(k for k in set(now) | set(before) if now.get(k) != before.get(k))


def write_record(path: Path, rec: dict[str, str]) -> None:
    path.write_text("".join(f"{d}  {n}\n" for n, d in sorted(rec.items())), encoding="utf-8")


def build(root: Path, gen: Path, retranslate: bool) -> int:
    """recompile.py into gen/, its lines turned into [build] lines."""
    cmd = [sys.executable, str(SOURCE / "tools" / "recompile.py"), "--cc", "mingw", "--no-decomp"]
    cmd += ["--reproducible", "--progress", "--out", str(gen), "--link"]
    if retranslate:
        cmd += ["--compile", "--optimize"]
    # the disc whose system files are built in (disc-layer I3): the player's,
    # not the default's, which would be source/extracted
    cmd += [
        "--dol",
        str(root / "extracted" / "sys" / "main.dol"),
        "--disc",
        str(root / "extracted"),
    ]
    proc = subprocess.Popen(
        cmd, cwd=SOURCE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1
    )
    assert proc.stdout is not None
    for line in proc.stdout:
        line = line.rstrip()
        if line.startswith("[build]"):
            print(line, flush=True)
        elif line:
            print(f"  {line}", flush=True)
    return proc.wait()


def install(root: Path, gen: Path) -> str:
    """soa.exe and the mods at the top of root; soa.ini when there is none.
    The answer is the exe's sha256."""
    exe = root / "soa.exe"
    shutil.copyfile(gen / "soa.exe", exe)
    mods = gen / "mods"
    if mods.is_dir():
        for folder in sorted(p for p in mods.iterdir() if p.is_dir()):
            dest = root / "mods" / folder.name
            dest.mkdir(parents=True, exist_ok=True)
            for f in folder.iterdir():
                if f.suffix in (".dll", ".ini", ".txt"):
                    shutil.copyfile(f, dest / f.name)
    ini = root / "soa.ini"
    if not ini.exists():
        ini.write_text(
            "# written by tools/player_build.py; an environment variable overrides each key\n"
            "disc = extracted\nrender = 1\ngpu = vulkan\nmods = mods\n",
            encoding="utf-8",
        )
    return hashlib.sha256(exe.read_bytes()).hexdigest()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--disc", type=Path, required=True, help="the player's disc image")
    ap.add_argument("--root", type=Path, required=True, help="the folder to build into")
    ap.add_argument(
        "--rebuild", action="store_true", help="translate again from scratch (Setup's Rebuild)"
    )
    args = ap.parse_args(argv)
    root = args.root.resolve()
    gen = root / "gen"

    say("check")
    try:
        check_disc(args.disc)
    except Refused as exc:
        say(f"refused: {exc}")
        return REFUSED
    say("extract")
    extracted = root / "extracted"
    if not (extracted / IMAGE_NAME).exists():
        extract(args.disc, extracted)
    now = inputs_record()
    moved = ["Rebuild asked for"] if args.rebuild else stale(gen, now)
    retranslate = bool(moved)
    if retranslate:
        say(f"translate: {', '.join(moved[:4])}{' and more' if len(moved) > 4 else ''}")
        if gen.exists():
            shutil.rmtree(gen)
    gen.mkdir(parents=True, exist_ok=True)
    (gen / "build-inputs.txt").unlink(missing_ok=True)
    if build(root, gen, retranslate) != 0:
        say("failed")
        return 1
    write_record(gen / "build-inputs.txt", now)
    digest = install(root, gen)
    say(f"done soa.exe sha256 {digest}")
    return 0


if __name__ == "__main__":
    os.environ.setdefault("PYTHONUTF8", "1")
    raise SystemExit(main())
