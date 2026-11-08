"""SOA_UNCAP and the [frametime] line: a sustained uncap and its percentiles (PLAN H3).

The [run] line gives totals, so a run that holds 33 ms a frame and one that
averages the same while hitching to 200 ms every few seconds read alike. The
frame hook now marks the wall clock once a frame, and the report prints the
median, 95th and 99th percentile and worst frame beside the process's CPU
seconds, which is what H11's thread work is judged against.

SOA_UNCAP=N lets the frame end's spin go after one field, from frame N on:
runtime/tick.c's VIGetRetraceCount answers the frame's start plus one at the
spin's call site (M2; H3 first did it by zeroing 0x8034768C, and
test_tick.py holds the mechanism). A start frame is what lets a frame-keyed
pad script reach the scene first: a disc load runs on the wall clock, and
uncapped from boot the title would not be where START lands.
SOA_FRAMETIME_FROM=N starts the record at N without uncapping, so a capped
run can measure the same stretch.

What a test without a disc can hold is that the switches are read at
startup and say what they do; that the game then runs up to twice as fast is
a run, and FINDINGS "H3" records it.
"""

import os
import re
import subprocess
from pathlib import Path

import pytest

from test_memguard import build, needs_msvc

ROOT = Path(__file__).resolve().parents[2]


def boot(exe, tmp_path, **env_set):
    """One boot with no disc: main() reads its switches, finds no disc
    image and stops."""
    env = dict(os.environ)
    env.pop("SOA_UNCAP", None)
    env.update(env_set)
    proc = subprocess.run(
        [str(exe), str(tmp_path / "no-disc-here")],
        capture_output=True,
        text=True,
        env=env,
        timeout=120,
        check=False,
    )
    return proc.stdout + proc.stderr


@needs_msvc
@pytest.mark.parametrize("value", ["1", "3300"])
def test_the_uncap_says_so_at_startup(tmp_path, value):
    """Before the disc is read, naming the frame it starts at and what it
    does, so a run that was uncapped cannot be mistaken afterwards for one
    that was not."""
    out = boot(build(tmp_path), tmp_path, SOA_UNCAP=value)
    assert f"[uncap] from frame {value}, the frame end's spin is let go after one field" in out, out


@needs_msvc
@pytest.mark.parametrize("value", [None, "0", ""], ids=["unset", "zero", "empty"])
def test_no_uncap_says_nothing(tmp_path, value):
    env = {} if value is None else {"SOA_UNCAP": value}
    assert "[uncap]" not in boot(build(tmp_path), tmp_path, **env)


@needs_msvc
@pytest.mark.parametrize("bad", ["on", "yes", "1x", " 1", "-1", "0x10", "99999999999"])
def test_a_value_that_is_not_a_frame_is_refused_out_loud(tmp_path, bad):
    """`on` read by atoi is 0, silently off -- the run then measures the
    capped game and says nothing. The same parser as SOA_POKE's frames."""
    out = boot(build(tmp_path), tmp_path, SOA_UNCAP=bad)
    assert f"SOA_UNCAP={bad} is not a frame number; the cap stays on" in out, out
    assert "is let go" not in out, out


@needs_msvc
def test_a_frametime_start_that_is_not_a_frame_is_refused_out_loud(tmp_path):
    out = boot(build(tmp_path), tmp_path, SOA_FRAMETIME_FROM="3300s")
    assert "SOA_FRAMETIME_FROM=3300s is not a frame number; [frametime] covers the whole run" in out
    assert "[uncap] from frame" not in out, out


# runtime/hle.c and nothing else, as test_profiler's clock driver: nineteen
# frames of 20 ms and one of 250 ms, then the report.
FRAME_DRIVER = r"""
#define _CRT_SECURE_NO_WARNINGS
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

void hle_report(void);
void hle_clock_start(void);
void hle_frame_mark(void);

void irq_report(void) {}
void threads_report(void) {}
unsigned gx_frame_count(void) { return 20; }
uint64_t irq_retrace_count(void) { return 40; }
uint64_t gx_pipe_bytes(void) { return 0; }
uint64_t gx_drawn_bytes(void) { return 0; }
void guest_backtrace(CpuState* s, uint32_t sp) { (void)s; (void)sp; }
int device_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out)
{ (void)s; (void)ea; (void)size; (void)out; return 0; }
void device_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v)
{ (void)s; (void)ea; (void)size; (void)v; }
void profile_report(void) {}

void hle_frametime_restart(unsigned frame);

/* frames.exe N [R]: N frames, the middle one a 250 ms hitch; with R, the
 * record restarts after the hitch at frame R, as SOA_UNCAP=R does. */
int main(int argc, char** argv)
{
    int i, frames = argc > 1 ? atoi(argv[1]) : 0, restart = argc > 2 ? atoi(argv[2]) : 0;
    hle_clock_start();
    for (i = 0; i <= frames; i++) {
        hle_frame_mark();
        if (restart && i == frames / 2 + 1) hle_frametime_restart((unsigned)restart);
        if (i < frames) Sleep(i == frames / 2 ? 250 : 20);
    }
    hle_report();
    return 0;
}
"""

FRAMETIME = re.compile(
    r"\[frametime\] (\d+) frames(?: from frame (\d+))?, ([\d.]+) a second; wall ms per frame: "
    r"p50 ([\d.]+), p95 ([\d.]+), p99 ([\d.]+), max ([\d.]+); "
    r"process CPU ([\d.]+) s \(([\d.]+) user \+ ([\d.]+) kernel\)"
)


@pytest.fixture(scope="module")
def frame_driver(tmp_path_factory):
    from soa import toolchain

    if toolchain.cl_path() is None:
        pytest.skip("no MSVC")
    out = tmp_path_factory.mktemp("frametime")
    (out / "frames.c").write_text(FRAME_DRIVER, encoding="utf-8")
    exe = out / "frames.exe"
    proc = toolchain.cl(
        [
            *toolchain.CFLAGS,
            "/I",
            str(ROOT / "runtime"),
            str(out / "frames.c"),
            str(ROOT / "runtime" / "hle.c"),
            str(ROOT / "runtime" / "seed.c"),
            str(ROOT / "runtime" / "clock.c"),
            *map(str, toolchain.runtime_support_sources()),  # plat.c (portability L9)
            "/Fo" + str(out) + os.sep,
            "/Fe" + str(exe),
        ],
        cwd=out,
    )
    assert proc.returncode == 0, (proc.stdout or "") + (proc.stderr or "")
    return exe


def frames(exe, *args):
    proc = subprocess.run(
        [str(exe), *map(str, args)], capture_output=True, text=True, timeout=60, check=False
    )
    return proc.stdout + proc.stderr


def parse(out):
    m = FRAMETIME.search(out)
    assert m, out
    n, start = int(m[1]), m[2]
    rate, p50, p95, p99, worst, cpu, user, kernel = map(float, m.groups()[2:])
    return n, start, rate, p50, p95, p99, worst, cpu, user, kernel


@needs_msvc
def test_the_percentiles_tell_a_hitch_from_a_steady_run(frame_driver):
    """Twenty frames, one of them 250 ms: the median stays near the 20 ms
    the other nineteen took and the worst frame is the hitch -- the pair an
    average of 31 ms a frame cannot tell apart from a steady 31."""
    out = frames(frame_driver, 20)
    n, start, rate, p50, p95, p99, worst, cpu, user, kernel = parse(out)
    assert n == 20 and start is None, out
    # Sleep(20) is 20-32 ms depending on the timer resolution in force.
    assert 15.0 <= p50 <= 40.0, out
    assert worst >= 240.0, out
    assert p50 <= p95 <= p99 <= worst, out
    # the rate is frames over their summed time, so the hitch pulls it down
    assert 1000.0 / (p50 * 19 / 20 + worst / 20) * 0.8 <= rate <= 1000.0 / p50, out
    assert abs(cpu - (user + kernel)) <= 0.15, out


@needs_msvc
def test_the_uncap_restarts_the_record_and_says_where(frame_driver):
    """Restarted after the hitch, the record holds only the frames after it:
    an uncapped stretch is judged on its own frames, and the line says which
    frame it counted from."""
    out = frames(frame_driver, 20, 3300)
    n, start, rate, p50, p95, p99, worst, *_ = parse(out)
    assert start == "3300", out
    assert n == 9, out
    assert worst < 200.0, out


@needs_msvc
def test_a_run_with_no_frames_says_so(frame_driver):
    """A run that dies before its first present still reports CPU time, and
    does not print percentiles of nothing."""
    out = frames(frame_driver, 0)
    assert "[frametime] no frames marked; process CPU" in out, out


def test_the_switch_is_documented():
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    assert "`SOA_UNCAP=" in readme, "SOA_UNCAP is not in the README's switch table"
    assert "`SOA_FRAMETIME_FROM=" in readme, (
        "SOA_FRAMETIME_FROM is not in the README's switch table"
    )
    assert "[frametime]" in readme, "the [frametime] line is not described in the README"
