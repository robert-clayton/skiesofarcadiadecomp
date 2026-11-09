"""A runtime built without GX (soa-ue5's U4.2; recompile.py --no-gx): runtime/gx_none.c and gx_frame.c in place of
gx.c, gxr.c, gxr_tev.c and gxv.c, answering the rest of the runtime as the full build does with SOA_GX=off.

test_gx_off.py's driver, built twice with gcc: once from gx.c and gx_frame.c (the full build, its renderer stood in for
by counters) and once from gx_none.c and gx_frame.c with SOA_NO_GX (no renderer at all). Each also saves its save
state's "gx" section, loads it back, and loads one whose model is not empty.

Checked: the build without GX prints, line for line, what the full build prints with SOA_GX=off -- the pipe's bytes
counted and nothing else, the register accesses counted and answered 0, the host's pending interrupts and drawn bytes,
the host's frame end running the frame hook and counting -- whether SOA_GX is off, on or unset (it cannot be on); its
report's counts are the same; its "gx" section is the full build's byte for byte and loads back; and a section whose
model is not empty (a snapshot taken with GX drawing) is refused, where the full build takes it. No disc and no game
data.
"""

import importlib.util
import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / "runtime"

_spec = importlib.util.spec_from_file_location("test_gx_off", Path(__file__).with_name("test_gx_off.py"))
_off = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_off)

# The renderer's stand-ins that gx_none.c defines itself (the full build's driver keeps them).
NONE_DEFINES = ("void gxr_flush(void)", "void gxr_reset_efb(void)", "void gxr_set_output(int a, int b)")

SNAP = r"""
#include "snapshot.h"
#include <string.h>
void gx_snap(Snap* sn);
static unsigned char g_sbuf[1 << 20];
static size_t g_slen, g_sat;
static void s_bytes(Snap* sn, void* p, size_t n)
{
    if (sn->loading) {
        if (g_sat + n > g_slen) { sn->failed = 1; return; }
        memcpy(p, g_sbuf + g_sat, n);
        g_sat += n;
    } else {
        memcpy(g_sbuf + g_slen, p, n);
        g_slen += n;
    }
}
static void s_fail(Snap* sn, const char* why) { (void)why; sn->failed = 1; }
static const SnapOps OPS = {s_bytes, 0, 0, s_fail, 0};
static void snap_lines(void)
{
    Snap save = {&OPS, 0, 0, 0}, load = {&OPS, 1, 0, 0}, bad = {&OPS, 1, 0, 0};
    unsigned long long h = 1469598103934665603ull;
    size_t i;
    gx_snap(&save);
    for (i = 0; i < g_slen; i++) h = (h ^ g_sbuf[i]) * 1099511628211ull;
    printf("snap %zu %016llx failed %d\n", g_slen, h, save.failed);
    g_sat = 0;
    gx_snap(&load);
    printf("load failed %d used %zu\n", load.failed, g_sat);
    g_sbuf[0] = 1; /* the first CP register: a model that is not empty */
    g_sat = 0;
    gx_snap(&bad);
    printf("nonempty failed %d\n", bad.failed);
}
"""


def driver(none: bool) -> str:
    text = _off.DRIVER
    if none:
        lines = [ln for ln in text.splitlines() if not ln.startswith(NONE_DEFINES)]
        text = "\n".join(lines) + "\n"
    text = text.replace("int main(void)", SNAP + "\nint main(void)", 1)
    marker = '    printf("host frame end %d frames %u hooks %u\\n", i, gx_frame_count(), g_hooks);\n'
    assert marker in text
    return text.replace(marker, marker + "    snap_lines();\n", 1)


gnu = pytest.mark.skipif(
    shutil.which("gcc") is None or os.name == "nt", reason="wants gcc on an ELF system"
)


def build(tmp: Path, none: bool) -> Path:
    (tmp / "driver.c").write_text(driver(none), encoding="utf-8")
    out = tmp / ("gxnone" if none else "gxfull")
    sources = ["gx_none.c", "gx_frame.c"] if none else ["gx.c", "gx_frame.c"]
    proc = subprocess.run(
        ["gcc", "-std=c17", "-O1", "-D_GNU_SOURCE", "-fno-strict-aliasing", "-fwrapv", f"-I{RUNTIME}",
         *(["-DSOA_NO_GX=1"] if none else []), *[str(RUNTIME / s) for s in sources], str(tmp / "driver.c"),
         "-lm", "-o", str(out)],
        capture_output=True, text=True, timeout=120, check=False,
    )  # fmt: skip
    assert proc.returncode == 0, proc.stdout + proc.stderr
    return out


@pytest.fixture(scope="module")
def exes(tmp_path_factory):
    full = build(tmp_path_factory.mktemp("gx_full"), False)
    none = build(tmp_path_factory.mktemp("gx_none"), True)
    return full, none


def report(err: str) -> list[str]:
    """The report's lines a check reads: the off line and the counts."""
    return [ln for ln in err.splitlines() if re.match(r"\[gx\] (off: \d|\d+ pipe bytes|display lists:)", ln)]


@gnu
@pytest.mark.parametrize("gx", [None, "off", "on"])
def test_without_gx_the_runtime_answers_as_gx_off(exes, gx):
    full, none = exes
    want, want_err = _off.run(full, SOA_GX="off")
    got, got_err = _off.run(none, **({"SOA_GX": gx} if gx else {}))
    assert want[0] == "off 1 native 1"
    # every line the same but the last: the full build takes a model that is not empty, this one refuses it
    assert got[:-1] == want[:-1], got_err
    assert want[-1] == "nonempty failed 0" and got[-1] == "nonempty failed 1"
    assert report(got_err) == report(want_err), got_err
    assert "[gx] off: this runtime is built without GX (--no-gx)" in got_err
    assert "[gxr] none: this runtime is built without GX (--no-gx)" in got_err


@gnu
def test_the_gx_section_is_the_full_builds_and_loads_back(exes):
    full, none = exes
    got, _ = _off.run(none)
    snap = next(ln for ln in got if ln.startswith("snap "))
    size = int(snap.split()[1])
    # gx.c's layout: the CP, XF and BP shadows, the CP and PE halfwords, the PI FIFO, the list flag, the frame, the
    # pipe's length, 13 counts, the last unknown byte, 5 list counts, the capture's three shadows and its length
    assert size == 4 * (0x100 + 0x1100 + 0x100) + 2 * (0x40 + 8) + 4 * 3 + 4 + 4 + 8 + 8 * 13 + 4 + 8 * 5 + 4 * (0x100 + 0x1100 + 0x100) + 8
    assert snap.endswith("failed 0")
    assert f"load failed 0 used {size}" in got
