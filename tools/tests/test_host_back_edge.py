"""A native answer's loop at its translation's back edge (runtime/soa_host.h,
soa_host_back_edge; docs/SWAP.md "Interrupt delivery for native answers"):
the real runtime/host.c, with test_host_strict.py's stand-ins for the rest of
the runtime, whose irq_poll and irq_quiet_entry count their calls and keep the
pc they were given.

Checked, on the stand-in guest's thread and off it: with flags 0 the call is
irq_poll itself, under either clock rule, with s->pc the block's address given
(0 leaves the one there); SOA_HOST_EDGE_DUE is irq_quiet_entry under the quiet
rule and irq_poll otherwise; SOA_HOST_EDGE_ENTRY is irq_quiet_entry under the
quiet rule and nothing otherwise; an unknown flag, both flags, no call and a
thread that is not the guest's are refused with nothing called. Wants gcc or
clang on an ELF system (SOA_CC).
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
extern int g_clock_quiet;
extern unsigned g_polls, g_quiet_entries;
extern uint32_t g_poll_pc;
void host_frame_end(CpuState* s, unsigned frame);

static void edge(CpuState* s, SoaHostCall* c, const char* what, uint32_t pc, unsigned flags)
{
    unsigned polls = g_polls, entries = g_quiet_entries;
    int r;
    s->pc = 0x80001110u;
    g_poll_pc = 0;
    r = soa_host_back_edge(c, pc, flags);
    printf("%s: %d polls %u entries %u pc %08X\n", what, r, g_polls - polls, g_quiet_entries - entries, g_poll_pc);
}

/* the guest: one frame's end (which makes this thread the guest's), then each back edge */
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
    edge(&s, &c, "before", 0x80002220u, 0);
    host_frame_end(&s, 0);
    g_clock_quiet = 1;
    edge(&s, &c, "quiet 0", 0x80002220u, 0);
    edge(&s, &c, "quiet 0 kept", 0, 0);
    edge(&s, &c, "quiet due", 0x80002230u, SOA_HOST_EDGE_DUE);
    edge(&s, &c, "quiet entry", 0, SOA_HOST_EDGE_ENTRY);
    g_clock_quiet = 0;
    edge(&s, &c, "old 0", 0x80002240u, 0);
    edge(&s, &c, "old due", 0, SOA_HOST_EDGE_DUE);
    edge(&s, &c, "old entry", 0, SOA_HOST_EDGE_ENTRY);
    edge(&s, &c, "both", 0, SOA_HOST_EDGE_DUE | SOA_HOST_EDGE_ENTRY);
    edge(&s, &c, "unknown", 0, 4u);
    edge(&s, NULL, "no call", 0, 0);
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
    char why[200];
    c.runtime = &c; /* not the guest's thread: refused before anything is read */
    printf("host thread: %d\n", soa_host_back_edge(&c, 0, 0));
    fflush(stdout);
    if (soa_host_start(&start, why, sizeof why)) return 5;
    for (;;) usleep(1000);
}
"""


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("host_back_edge")
    stubs = STUBS.split("int window_pad(")[0]  # the stand-ins, without test_host_strict's guest
    (tmp / "stubs.c").write_text(stubs + GUEST, encoding="utf-8")
    (tmp / "driver.c").write_text(DRIVER, encoding="utf-8")
    exe = tmp / "edge"
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
def test_a_native_back_edge_is_the_translations_delivery_point(built):
    env = {k: v for k, v in os.environ.items() if not k.startswith("SOA_")}
    out = subprocess.run([str(built)], capture_output=True, text=True, timeout=60, env=env)
    assert out.returncode == 0, out.stdout + out.stderr
    lines = [line for line in out.stdout.splitlines() if ":" in line]
    assert lines == [
        "host thread: 0",
        "before: 0 polls 0 entries 0 pc 00000000",  # the guest's thread only once it is the guest's
        "quiet 0: 1 polls 1 entries 0 pc 80002220",  # irq_poll, at the block's address
        "quiet 0 kept: 1 polls 1 entries 0 pc 80001110",  # pc 0: as it was
        "quiet due: 1 polls 0 entries 1 pc 80002230",
        "quiet entry: 1 polls 0 entries 1 pc 80001110",
        "old 0: 1 polls 1 entries 0 pc 80002240",
        "old due: 1 polls 1 entries 0 pc 80001110",
        "old entry: 1 polls 0 entries 0 pc 00000000",
        "both: 0 polls 0 entries 0 pc 00000000",
        "unknown: 0 polls 0 entries 0 pc 00000000",
        "no call: 0 polls 0 entries 0 pc 00000000",
    ]
