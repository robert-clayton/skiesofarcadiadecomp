"""Tests for the early depth test's premise (PLAN-60FPS-MODS H15a).

The renderer now tests depth before the TEV whenever a draw's alpha compare
passes every alpha a fragment can have, because then the order of the two
tests cannot change a pixel and the TEV is spared every fragment depth would
discard. Everything rests on ``alpha_always`` being right, and the plan names
the trap: two compares that always pass, joined by XOR, never pass. So these
cases are chosen by hand, with the answer worked out from the hardware's
definition rather than from the code, and include every logic op over the
always and never compares, a reference at each end of the range, and the
cache the function keeps between draws.

The driver includes gxr_tev.c itself, to call the static function. No disc and
no game data.
"""

import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from soa import toolchain  # noqa: E402

RUNTIME = ROOT / "runtime"
SOURCES = ["gx.c", "gx_frame.c", "gxr.c", "png.c"]  # gxr_tev.c comes in through the driver

# (comp0, ref0, comp1, ref1, logic, passes every alpha 0-255?)
# Compares: 0 never, 1 <, 2 ==, 3 <=, 4 >, 5 !=, 6 >=, 7 always.
# Logic: 0 AND, 1 OR, 2 XOR, 3 XNOR.
CASES = [
    (7, 0, 7, 0, 0, 1),  # always AND always
    (7, 0, 7, 0, 1, 1),  # always OR always
    (7, 0, 7, 0, 2, 0),  # always XOR always: never -- the trap
    (7, 0, 7, 0, 3, 1),  # always XNOR always
    (7, 0, 0, 0, 0, 0),  # always AND never
    (7, 0, 0, 0, 1, 1),  # always OR never
    (7, 0, 0, 0, 2, 1),  # always XOR never
    (7, 0, 0, 0, 3, 0),  # always XNOR never
    (0, 0, 0, 0, 3, 1),  # never XNOR never: always
    (6, 0, 7, 0, 0, 1),  # alpha >= 0 is every alpha
    (4, 0, 7, 0, 0, 0),  # alpha > 0 fails at 0 (the game's usual cutout test)
    (3, 255, 7, 0, 0, 1),  # alpha <= 255 is every alpha
    (1, 255, 7, 0, 0, 0),  # alpha < 255 fails at 255
    (5, 128, 2, 128, 1, 1),  # != 128 OR == 128 covers every alpha
    (5, 128, 2, 128, 0, 0),  # != 128 AND == 128 covers none
    (4, 100, 1, 100, 1, 0),  # > 100 OR < 100 misses 100 itself
]

DRIVER = r"""
#define _CRT_SECURE_NO_WARNINGS
#include "gxr_tev.c" /* alpha_always and alpha_passes are static */

uint32_t mmio_read32(CpuState* s, uint32_t ea) { (void)s; (void)ea; return 0; }
void hle_report(void) {}

int main(int argc, char** argv)
{
    int i;
    for (i = 1; i + 4 < argc; i += 5) {
        TevSetup T;
        uint32_t c0 = (uint32_t)atoi(argv[i]), r0 = (uint32_t)atoi(argv[i + 1]);
        uint32_t c1 = (uint32_t)atoi(argv[i + 2]), r1 = (uint32_t)atoi(argv[i + 3]), lg = (uint32_t)atoi(argv[i + 4]);
        uint32_t ac = r0 | (r1 << 8) | (c0 << 16) | (c1 << 19) | (lg << 22);
        memset(&T, 0, sizeof T);
        T.aref0 = (int)r0; T.aref1 = (int)r1; T.acomp0 = c0; T.acomp1 = c1; T.alogic = lg;
        printf("%d\n", alpha_always(&T, ac));
    }
    return 0;
}
"""


@pytest.fixture(scope="module")
def exe(tmp_path_factory):
    if toolchain.cl_path() is None:
        pytest.skip("no MSVC")
    out = tmp_path_factory.mktemp("alpha")
    (out / "driver.c").write_text(DRIVER, encoding="utf-8")
    exe = out / "alpha.exe"
    proc = toolchain.cl(
        [
            *toolchain.CFLAGS,
            "/I",
            str(RUNTIME),
            *[str(RUNTIME / name) for name in SOURCES],
            str(out / "driver.c"),
            "/Fo" + str(out) + os.sep,
            "/Fe" + str(exe),
        ],
        cwd=out,
    )
    assert proc.returncode == 0, (proc.stdout or "") + (proc.stderr or "")
    return exe


needs_msvc = pytest.mark.skipif(
    toolchain.cl_path() is None, reason="no MSVC: the renderer cannot be built here"
)


def answers(exe, cases) -> list:
    args = [str(v) for c in cases for v in c[:5]]
    run = subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=60, check=True)
    return [int(v) for v in run.stdout.split()]


@needs_msvc
def test_each_case_is_answered_as_the_hardware_defines_it(exe):
    got = answers(exe, CASES)
    want = [c[5] for c in CASES]
    wrong = [(c, g) for c, g in zip(CASES, got, strict=True) if g != c[5]]
    assert got == want, wrong


@needs_msvc
def test_the_cache_between_draws_follows_the_register(exe):
    """The answer is kept for the last compare register, so alternating two
    registers in one process must still give each its own answer."""
    trap, plain = CASES[2], CASES[0]
    got = answers(exe, [trap, plain, trap, plain, plain, trap])
    assert got == [0, 1, 0, 1, 1, 0], got
