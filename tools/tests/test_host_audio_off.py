"""SOA_AUDIO=off's host entries (runtime/soa_host.h, soa_host_audio_off and soa_host_audio_charge): the real
runtime/host.c, with test_host_strict.py's stand-ins for the rest of the runtime, whose audio switch the guest below
sets and whose charges count their calls.

Checked: without the switch both entries refuse and nothing is charged; with it, a charge is refused from the host's
thread, with no runtime, for an address that is not the DSP interface's control register (a halfword at 0xCC00500A) or
AI's control word (0xCC006C00), for a wrong size and an unknown flag, and otherwise reaches the runtime's charge once
(a read's and a write's apart). Wants gcc or clang on an ELF system (SOA_CC).
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
extern int g_audio_off_stub;
extern unsigned g_gx_charge_reads, g_gx_charge_writes;
void host_frame_end(CpuState* s, unsigned frame);

static void charge(CpuState* s, const char* what, uint32_t a, unsigned size, unsigned flags)
{
    unsigned r0 = g_gx_charge_reads, w0 = g_gx_charge_writes;
    int r = soa_host_audio_charge(s, a, size, flags, 5);
    printf("%s: %d reads %u writes %u\n", what, r, g_gx_charge_reads - r0, g_gx_charge_writes - w0);
}

/* the guest: one frame's end (which makes this thread the guest's), then each entry without the switch and with it */
int soa_main(int argc, char** argv)
{
    static CpuState s;
    unsigned k;
    (void)argc, (void)argv;
    s.mem = (uint8_t*)calloc(1, 0x02000000u);
    host_frame_end(&s, 0);
    for (k = 0; k < 2; k++) {
        g_audio_off_stub = (int)k;
        printf("switch %u: off %d\n", k, soa_host_audio_off());
        charge(&s, "CSR read", 0xCC00500Au, 2, 0);
        charge(&s, "CSR write", 0xCC00500Au, 2, SOA_HOST_GX_WRITE);
        charge(&s, "AI read", 0xCC006C00u, 4, 0);
        charge(&s, "AI write", 0xCC006C00u, 4, SOA_HOST_GX_WRITE);
        charge(&s, "AI volume", 0xCC006C04u, 4, 0);
        charge(&s, "mailbox", 0xCC005000u, 2, 0);
        charge(&s, "wide CSR", 0xCC00500Au, 4, 0);
        charge(&s, "short AI", 0xCC006C00u, 2, 0);
        charge(&s, "probe flag", 0xCC00500Au, 2, SOA_HOST_GX_PROBE);
        {
            unsigned r0 = g_gx_charge_reads;
            int r = soa_host_audio_charge(NULL, 0xCC00500Au, 2, 0, 0);
            printf("no runtime: %d reads %u\n", r, g_gx_charge_reads - r0);
        }
    }
    fflush(stdout);
    exit(0);
}
"""

DRIVER = r"""
#include "soa_host.h"
#include <stdio.h>
#include <unistd.h>
extern int g_audio_off_stub;
extern unsigned g_gx_charge_reads;
int main(void)
{
    SoaHostStart start = {SOA_HOST_ABI, "stand-in", NULL, NULL};
    char why[200];
    static uint8_t not_a_cpu[8192];
    g_audio_off_stub = 1; /* the host's thread: refused before anything is called */
    printf("host thread: charge %d reads %u\n", soa_host_audio_charge(not_a_cpu, 0xCC00500Au, 2, 0, 0), g_gx_charge_reads);
    g_audio_off_stub = 0;
    fflush(stdout);
    if (soa_host_start(&start, why, sizeof why)) return 5;
    for (;;) usleep(1000);
}
"""


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("host_audio_off")
    stubs = STUBS.split("int window_pad(")[0]  # the stand-ins, without test_host_strict's guest
    (tmp / "stubs.c").write_text(stubs + GUEST, encoding="utf-8")
    (tmp / "driver.c").write_text(DRIVER, encoding="utf-8")
    exe = tmp / "audiooff"
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
def test_the_audio_off_entries_refuse_without_the_switch_and_charge_only_the_two_control_registers(
    built,
):
    env = {k: v for k, v in os.environ.items() if not k.startswith("SOA_")}
    out = subprocess.run([str(built)], capture_output=True, text=True, timeout=60, env=env)
    assert out.returncode == 0, out.stdout + out.stderr
    lines = [line for line in out.stdout.splitlines() if ":" in line]
    never = [
        "AI volume: 0 reads 0 writes 0",
        "mailbox: 0 reads 0 writes 0",
        "wide CSR: 0 reads 0 writes 0",
        "short AI: 0 reads 0 writes 0",
        "probe flag: 0 reads 0 writes 0",
        "no runtime: 0 reads 0",
    ]
    assert lines == [
        "host thread: charge 0 reads 0",
        "switch 0: off 0",
        "CSR read: 0 reads 0 writes 0",
        "CSR write: 0 reads 0 writes 0",
        "AI read: 0 reads 0 writes 0",
        "AI write: 0 reads 0 writes 0",
        *never,
        "switch 1: off 1",
        "CSR read: 1 reads 1 writes 0",
        "CSR write: 1 reads 0 writes 1",
        "AI read: 1 reads 1 writes 0",
        "AI write: 1 reads 0 writes 1",
        *never,
    ]
