"""The seam between the runtime and a split game library (specs/android.md 3.2).

config/seam.txt names what crosses: the runtime symbols the translated code
may import (with the bindings config/hle.txt names), the C library names it
may import beside them, and the functions the self test runs by address.
This writes the C on each side of it for tools/recompile.py:

- <out>/game_table.c, in every build: the game's one export, soa_game, a
  table of the entry, dispatch, the system files and the self test's twins,
  with the build record, which an ELF build also carries in .note.soa;
- for a split build (--split): <out>/runtime_seam.c, the runtime's record,
  its export list and forwarders through the loaded table under the names
  the runtime calls; <out>/runtime.map, the runtime library's version script,
  exporting exactly the seam; and <out>/launcher.c.

The record says what a game library must agree on with the runtime that
loads it: the table's abi, the decomp mode (whether 25 or 13 bindings cross)
and one digest of everything the translated C bakes in
(player_build.BAKED). runtime/elfcheck.c compares the three before dlopen.
"""

from __future__ import annotations

import hashlib
from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from pathlib import Path

ABI = 1
RUNTIME_SONAME = "libsoa_runtime.so"
GAME_SONAME = "libsoa_game.so"
ENTRY = 0x80003140  # __start, runtime/main.c's ENTRY_FN


@dataclass(frozen=True)
class Seam:
    runtime: tuple[str, ...]
    libc: tuple[str, ...]
    twins: tuple[int, ...]


def read_seam(path: Path) -> Seam:
    """config/seam.txt; a line that is not `runtime NAME`, `libc NAME` or
    `twin 0xXXXXXXXX`, or a name given twice, stops the build with its line."""
    runtime: list[str] = []
    libc: list[str] = []
    twins: list[int] = []
    seen: set[str] = set()
    for n, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        kind, _, value = line.partition(" ")
        value = value.strip()
        ok = bool(value) and " " not in value and value not in seen
        if kind == "runtime" and ok:
            runtime.append(value)
        elif kind == "libc" and ok:
            libc.append(value)
        elif kind == "twin" and ok and value.startswith("0x") and len(value) == 10:
            twins.append(int(value, 16))
        else:
            raise ValueError(f"{path}:{n}: not a seam line (or a repeat): {raw!r}")
        seen.add(value)
    return Seam(tuple(runtime), tuple(libc), tuple(twins))


def baked_digest(inputs: Mapping[str, str]) -> str:
    """One sha256 over player_build.inputs_record's: every input the
    translated C bakes in, by path."""
    text = "".join(f"{digest}  {name}\n" for name, digest in sorted(inputs.items()))
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def record(no_decomp: bool, baked: str, dol_sha1: str = "", profile: str = "") -> str:
    """A build record: what elfcheck compares (abi, mode, baked), and what
    only says where a library came from (dol, profile)."""
    parts = [f"abi={ABI}", f"mode={'no-decomp' if no_decomp else 'decomp'}", f"baked={baked}"]
    if dol_sha1:
        parts.append(f"dol={dol_sha1}")
    if profile:
        parts.append(f"profile={profile}")
    return " ".join(parts)


def body_name(addr: int, bound: Iterable[int]) -> str:
    """The translated body at addr: recomp_fn_ where the runtime answers fn_
    itself (a binding in this build's mode), fn_ otherwise."""
    return f"recomp_fn_{addr:08X}" if addr in set(bound) else f"fn_{addr:08X}"


def c_string(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def game_table_c(seam: Seam, bound: Iterable[int], emitted: Iterable[int], rec: str) -> str:
    """<out>/game_table.c. `bound` is this build's bindings (after
    --no-decomp), `emitted` the functions translated (all but --limit's)."""
    bound = set(bound)
    emitted = set(emitted)
    desc = (len(rec.encode("utf-8")) + 1 + 3) & ~3
    cases = "\n".join(
        f"    case 0x{a:08X}u: return {body_name(a, bound)};" for a in seam.twins if a in emitted
    )
    entry = f"fn_{ENTRY:08X}" if ENTRY in emitted else "0"
    return f"""/* Written by tools/recompile.py (specs/android.md 3.2): the game library's
 * one export, soa_game, and its build record, which an ELF build also
 * carries in .note.soa for runtime/elfcheck.c to read before dlopen. A
 * single-file build links it too, and the self test holds it to what that
 * build calls directly. */
#include "functions.h"
#include "soa_game.h"

extern const uint32_t disc_sys_dol[], disc_sys_boot[], disc_sys_fst[];
extern const size_t disc_sys_dol_size, disc_sys_boot_size, disc_sys_fst_size;
extern const char disc_sys_dol_sha1[], disc_sys_boot_sha1[], disc_sys_fst_sha1[];
int dispatch_known(uint32_t addr);

#define SOA_GAME_RECORD {c_string(rec)}

static SoaFn twin(uint32_t addr)
{{
    switch (addr) {{
{cases}
    default: return 0;
    }}
}}

static const SoaGame g_game = {{SOA_GAME_ABI, (uint32_t)sizeof(SoaGame), SOA_GAME_RECORD, {entry}, dispatch,
                               dispatch_known, disc_sys_dol, disc_sys_boot, disc_sys_fst, &disc_sys_dol_size,
                               &disc_sys_boot_size, &disc_sys_fst_size, disc_sys_dol_sha1, disc_sys_boot_sha1,
                               disc_sys_fst_sha1, twin}};

const SoaGame* soa_game(void)
{{
    return &g_game;
}}

#if defined(__ELF__)
__attribute__((section(".note.soa"), aligned(4), used)) static const struct {{
    uint32_t namesz, descsz, type;
    char name[4];
    char desc[{desc}];
}} g_note = {{4, {desc}, 1, "SOA", SOA_GAME_RECORD}};
#endif
"""


def runtime_exports(seam: Seam, bound: Iterable[int]) -> list[str]:
    """What the runtime library exports for a game library to import: the
    seam's runtime symbols and this mode's bindings, which the runtime
    answers itself."""
    return [*seam.runtime, *(f"fn_{a:08X}" for a in sorted(set(bound)))]


def runtime_seam_c(seam: Seam, bound: Iterable[int], runtime_rec: str) -> str:
    """<out>/runtime_seam.c, for a split build's runtime library."""
    bound = set(bound)
    exports = runtime_exports(seam, bound)
    forwarders = [f"void fn_{ENTRY:08X}(CpuState* s) {{ soa_game_table->entry(s); }}"]
    forwarders += [
        f"void {body_name(a, bound)}(CpuState* s) {{ soa_game_table->twin(0x{a:08X}u)(s); }}"
        for a in seam.twins
    ]
    names = ",\n".join(f"    {c_string(n)}" for n in exports)
    libc = ",\n".join(f"    {c_string(n)}" for n in seam.libc)
    return f"""/* Written by tools/recompile.py --split (specs/android.md 3.2): the
 * runtime's side of the seam. Its record, which a game library's must agree
 * with; what a game library may import from it, beside the C library's
 * names; and forwarders through the loaded table (runtime/game.c) under the
 * names the runtime calls, so no runtime file changes for a split build. */
#include "soa_game.h"

const char soa_runtime_record[] = {c_string(runtime_rec)};

const char* const soa_runtime_exports[] = {{
{names}
}};
const unsigned soa_runtime_export_count = {len(exports)};

const char* const soa_runtime_libc[] = {{
{libc}
}};
const unsigned soa_runtime_libc_count = {len(seam.libc)};

void dispatch(CpuState* s, uint32_t addr) {{ soa_game_table->dispatch(s, addr); }}
int dispatch_known(uint32_t addr) {{ return soa_game_table->dispatch_known(addr); }}
{chr(10).join(forwarders)}
"""


def version_script(seam: Seam, bound: Iterable[int], host: bool = False) -> str:
    """<out>/runtime.map: the runtime library exports the seam, the launcher's
    soa_run, and nothing else; SDL, linked in statically, stays inside. A
    host build (--host) exports runtime/soa_host.h's API too, which is every
    name starting soa_host_, for the program that loads it."""
    names = [*runtime_exports(seam, bound), "soa_run", *(["soa_host_*"] if host else [])]
    body = "\n".join(f"    {n};" for n in names)
    return f"{{\n  global:\n{body}\n  local: *;\n}};\n"


def stub_runtime_c(seam: Seam, bound: Iterable[int]) -> str:
    """<out>/stub/stub_runtime.c, for an Android profile (specs/android.md
    L12b): the runtime library's exports as stand-ins, so the PC links a game
    library against the runtime it will meet on the phone, by name, without
    building that runtime. Data for the g_* globals, an empty function for
    the rest; it is linked against, never run."""
    lines = [
        "/* Written by tools/recompile.py for an Android profile (specs/android.md",
        " * L12b): stand-ins for libsoa_runtime.so's exports, linked against and",
        " * never run. The phone's runtime library is the APK's. */",
        "#include <stdint.h>",
    ]
    for name in runtime_exports(seam, bound):
        lines.append(f"uint32_t {name};" if name.startswith("g_") else f"void {name}(void) {{}}")
    return "\n".join(lines) + "\n"


LAUNCHER_C = """/* Written by tools/recompile.py --split: the program a split build runs
 * (specs/android.md 3.2). It needs libsoa_runtime.so, which loads
 * libsoa_game.so from beside it (runtime/game.c). */
int soa_run(int argc, char** argv);

int main(int argc, char** argv)
{
    return soa_run(argc, argv);
}
"""
