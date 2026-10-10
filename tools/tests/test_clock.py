"""runtime/clock.c: the guest's clock (PLAN-60FPS-MODS M19).

Guest time accumulates speed x the host's monotonic time between reads; a
gap between two reads past the threshold counts as nothing and bumps the
epoch; a host step backwards counts as nothing; a speed change is
continuous; a pause is excluded. clock.c is built alone and fed synthetic
host times, so none of this waits on a real clock.
"""

import os
import subprocess
import sys
from pathlib import Path

import pytest

from test_memguard import needs_msvc

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

MS = 1_000_000
S = 1_000_000_000

DRIVER = r"""
#include <stdio.h>
#include <string.h>
#include "clock.h"
static unsigned g_notes;
static void note(double s) { (void)s; g_notes++; }
int main(void)
{
    char cmd[16];
    unsigned long long a, b;
    while (scanf("%15s", cmd) == 1) {
        if (!strcmp(cmd, "config") && scanf("%llu %llu", &a, &b) == 2) {
            clock_reset();
            clock_configure((unsigned)a, (long)b, note);
            g_notes = 0;
        } else if (!strcmp(cmd, "adv") && scanf("%llu", &a) == 1) {
            unsigned long long g = clock_advance_at(a);
            printf("%llu %u %u\n", g, clock_epoch(), g_notes);
        } else if (!strcmp(cmd, "peek") && scanf("%llu", &a) == 1) {
            printf("%llu\n", (unsigned long long)clock_peek_at(a));
        } else if (!strcmp(cmd, "speed") && scanf("%llu %llu", &a, &b) == 2) {
            clock_set_speed_at((unsigned)a, b);
        } else if (!strcmp(cmd, "pause") && scanf("%llu", &a) == 1) {
            clock_pause((int)a);
        } else if (!strcmp(cmd, "exclude") && scanf("%llu %llu", &a, &b) == 2) {
            clock_exclude_at(a, b);
        }
    }
    return 0;
}
"""


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    from soa import toolchain

    if toolchain.cl_path() is None:
        pytest.skip("no MSVC")
    out = tmp_path_factory.mktemp("clock")
    (out / "driver.c").write_text(DRIVER, encoding="utf-8")
    exe = out / "clock.exe"
    proc = toolchain.cl(
        [
            *toolchain.CFLAGS,
            "/I",
            str(ROOT / "runtime"),
            str(ROOT / "runtime" / "clock.c"),
            str(out / "driver.c"),
            "/Fo" + str(out) + os.sep,
            "/Fe" + str(exe),
        ],
        cwd=out,
    )
    assert proc.returncode == 0, (proc.stdout or "") + (proc.stderr or "")
    return exe


def run(exe: Path, lines: list[str]) -> list[tuple[int, ...]]:
    proc = subprocess.run(
        [str(exe)],
        input="\n".join(lines) + "\n",
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    )
    assert proc.returncode == 0, proc.stderr
    return [tuple(int(v) for v in ln.split()) for ln in proc.stdout.splitlines()]


T0 = 5 * S  # the host time of the first read: guest time starts from it


@needs_msvc
def test_steady_steps_are_guest_time(driver):
    got = run(driver, ["config 1 250", f"adv {T0}", *[f"adv {T0 + k * MS}" for k in range(1, 6)]])
    assert [g for g, _, _ in got] == [0, MS, 2 * MS, 3 * MS, 4 * MS, 5 * MS], got
    assert {e for _, e, _ in got} == {0}, got


@needs_msvc
def test_a_long_gap_counts_as_nothing_unless_the_rule_is_off(driver):
    got = run(
        driver,
        [
            "config 1 250",
            f"adv {T0}",
            f"adv {T0 + MS}",
            f"adv {T0 + MS + 10 * S}",
            f"adv {T0 + 2 * MS + 10 * S}",
        ],
    )
    assert [g for g, _, _ in got] == [0, MS, MS, 2 * MS], got
    assert got[2][1] == 1 and got[2][2] == 1, got  # one epoch, one note
    off = run(driver, ["config 1 0", f"adv {T0}", f"adv {T0 + MS}", f"adv {T0 + MS + 10 * S}"])
    assert off[-1][0] == MS + 10 * S and off[-1][1] == 0, off


@needs_msvc
def test_the_host_stepping_back_never_steps_the_guest_back(driver):
    got = run(
        driver,
        [
            "config 1 250",
            f"adv {T0}",
            f"adv {T0 + 5 * MS}",
            f"adv {T0 + 2 * MS}",
            f"adv {T0 + 6 * MS}",
        ],
    )
    guest = [g for g, _, _ in got]
    assert guest == sorted(guest) and guest == [0, 5 * MS, 5 * MS, 6 * MS], got


@needs_msvc
def test_a_speed_change_is_continuous_and_bumps_the_epoch(driver):
    got = run(
        driver,
        [
            "config 1 250",
            f"adv {T0}",
            f"adv {T0 + 100 * MS}",
            f"speed 2 {T0 + 101 * MS}",
            f"adv {T0 + 102 * MS}",
        ],
    )
    before, after = got[1], got[2]
    assert after[0] - before[0] <= 2 * MS + 1 * MS * 2, (
        before,
        after,
    )  # 1 ms at 1x, then 1 ms at 2x
    assert after[0] == 100 * MS + MS + 2 * MS and after[1] == 1, got


@needs_msvc
def test_a_pause_is_excluded(driver):
    got = run(
        driver,
        [
            "config 1 0",  # no gap rule, so only the pause can exclude the 5 s
            f"adv {T0}",
            f"adv {T0 + 10 * MS}",
            "pause 1",
            f"adv {T0 + 20 * MS}",
            f"adv {T0 + 5 * S}",
            "pause 0",
            f"adv {T0 + 5 * S + 1 * MS}",
            f"adv {T0 + 5 * S + 4 * MS}",
        ],
    )
    guest = [g for g, _, _ in got]
    assert guest == [0, 10 * MS, 20 * MS, 20 * MS, 20 * MS, 23 * MS], got
    assert got[-1][1] == 1, got  # the resume bumps the epoch


@needs_msvc
def test_a_wait_of_the_guest_threads_own_is_excluded(driver):
    """A host holding the frame's end (host.c): the time up to the wait
    counts, at the speed of the run, and the wait itself does not."""
    got = run(
        driver,
        [
            "config 30 0",
            f"adv {T0}",
            f"adv {T0 + 1 * MS}",
            f"exclude {T0 + 2 * MS} {T0 + 9 * MS}",
            f"adv {T0 + 9 * MS}",
            f"adv {T0 + 10 * MS}",
        ],
    )
    guest = [g for g, _, _ in got]
    assert guest == [0, 30 * MS, 60 * MS, 90 * MS], got
    assert got[-1][1] == 1, got  # an excluded span bumps the epoch, as a gap does


@needs_msvc
def test_peeking_writes_nothing(driver):
    """The report reads the clock without advancing it: a peek past a gap
    says no time passed, and the next advance still sees the gap."""
    out = run(
        driver,
        [
            "config 1 250",
            f"adv {T0}",
            f"adv {T0 + MS}",
            f"peek {T0 + 3 * MS}",
            f"peek {T0 + 9 * S}",
            f"adv {T0 + 9 * S}",
        ],
    )
    assert out[2] == (3 * MS,) and out[3] == (MS,), out
    assert out[4][0] == MS and out[4][1] == 1, out
