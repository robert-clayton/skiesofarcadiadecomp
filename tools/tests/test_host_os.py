"""The OS service (runtime/soa_host.h, soa_host_os; docs/SWAP.md "The OS
service"): the real runtime/host.c, with test_host_strict.py's stand-ins for
the rest of the runtime, whose idle hook and timebase reads count their calls
and keep the pc they were given.

Checked: the version on any thread and with no call; an operation refused, with
nothing called, from the host's thread, with no call, and on the guest's thread
with a call that is not the answer running now (an operation is only for the
answer that asks, from inside it: an answer's own call is checked by soa-ue5's
headless pairs, which reach it through a swap); an unknown operation and a
timebase read with no value refused too. Wants gcc or clang on an ELF system
(SOA_CC).
"""

import os
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from soa import toolchain  # noqa: E402
from test_host_strict import PROFILE, RUNTIME, STUBS, gnu  # noqa: E402

GUEST = r"""
#include "soa_host.h"
extern unsigned g_os_idles, g_os_tb;
extern uint32_t g_poll_pc;
void host_frame_end(CpuState* s, unsigned frame);

static void op(CpuState* s, SoaHostCall* c, const char* what, unsigned o, uint32_t pc, int with_value)
{
    unsigned idles = g_os_idles, tb = g_os_tb;
    uint64_t v = 7;
    int r;
    s->pc = 0x80001110u;
    g_poll_pc = 0;
    r = soa_host_os(c, o, pc, with_value ? &v : NULL);
    printf("%s: %d idles %u tb %u value %llu pc %08X\n", what, r, g_os_idles - idles, g_os_tb - tb,
           (unsigned long long)v, g_poll_pc);
}

/* the guest: one frame's end (which makes this thread the guest's), then each operation with a call that no answer
   is running */
int soa_main(int argc, char** argv)
{
    static CpuState s;
    SoaHostCall c;
    (void)argc, (void)argv;
    s.mem = (uint8_t*)calloc(1, 0x02000000u);
    memset(&c, 0, sizeof c);
    c.runtime = &s;
    c.mem = s.mem;
    c.r = s.gpr;
    c.f = (SoaHostPair*)s.fpr;
    host_frame_end(&s, 0);
    op(&s, NULL, "version", SOA_HOST_OS_VERSION, 0, 1);
    op(&s, &c, "tb lo", SOA_HOST_OS_TB_LO, 0x8023851Cu, 1);
    op(&s, &c, "tb hi", SOA_HOST_OS_TB_HI, 0x80238504u, 1);
    op(&s, &c, "idle", SOA_HOST_OS_IDLE, 0x80237BA8u, 0);
    op(&s, NULL, "no call", SOA_HOST_OS_IDLE, 0, 0);
    op(&s, &c, "no value", SOA_HOST_OS_TB_LO, 0, 0);
    op(&s, &c, "unknown", 4u, 0, 1);
    fflush(stdout);
    exit(0);
}
"""

DRIVER = r"""
#include "soa_host.h"
#include <stdio.h>
#include <unistd.h>
int main(void)
{
    SoaHostStart start = {SOA_HOST_ABI, "stand-in", NULL, NULL};
    SoaHostCall c = {0};
    uint64_t v = 0;
    char why[200];
    int r;
    c.runtime = &c; /* not the guest's thread: refused before anything is read */
    printf("host thread: %d\n", soa_host_os(&c, SOA_HOST_OS_IDLE, 0, NULL));
    r = soa_host_os(NULL, SOA_HOST_OS_VERSION, 0, &v);
    printf("host version: %d %llu\n", r, (unsigned long long)v);
    fflush(stdout);
    if (soa_host_start(&start, why, sizeof why)) return 5;
    for (;;) usleep(1000);
}
"""


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("host_os")
    stubs = STUBS.split("int window_pad(")[0]  # the stand-ins, without test_host_strict's guest
    (tmp / "stubs.c").write_text(stubs + GUEST, encoding="utf-8")
    (tmp / "driver.c").write_text(DRIVER, encoding="utf-8")
    exe = tmp / "os"
    proc = toolchain.cc(
        [
            *PROFILE.cflags,
            "/DSOA_SPLIT=1",
            "/DSOA_HOST=1",
            f"/I{RUNTIME}",
            f"/Fe{exe}",
            str(RUNTIME / "host.c"),
            "stubs.c",
            "driver.c",
            *map(str, toolchain.runtime_support_sources()),
            *PROFILE.linker,
        ],
        tmp,
        PROFILE,
    )
    assert proc.returncode == 0, (proc.stdout or "") + (proc.stderr or "")
    return exe


@gnu
def test_the_os_service_is_only_for_the_answer_that_asks(built):
    env = {k: v for k, v in os.environ.items() if not k.startswith("SOA_")}
    out = subprocess.run([str(built)], capture_output=True, text=True, timeout=60, env=env)
    assert out.returncode == 0, out.stdout + out.stderr
    lines = [line for line in out.stdout.splitlines() if ":" in line]
    assert lines == [
        "host thread: 0",
        "host version: 1 1",  # any thread, no call
        "version: 1 idles 0 tb 0 value 1 pc 00000000",
        "tb lo: 0 idles 0 tb 0 value 7 pc 00000000",  # no answer runs this call: refused
        "tb hi: 0 idles 0 tb 0 value 7 pc 00000000",
        "idle: 0 idles 0 tb 0 value 7 pc 00000000",
        "no call: 0 idles 0 tb 0 value 7 pc 00000000",
        "no value: 0 idles 0 tb 0 value 7 pc 00000000",
        "unknown: 0 idles 0 tb 0 value 7 pc 00000000",
    ]
