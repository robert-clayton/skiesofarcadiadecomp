"""SOA_GX=off's host entries (runtime/soa_host.h, soa_host_gx_off, soa_host_gx_native, soa_host_gx_frame_end and
soa_host_gx_charge; docs/SWAP.md "The no-FIFO mode"): the real runtime/host.c, with test_host_strict.py's stand-ins for
the rest of the runtime, whose GX switch the guest below sets and whose frame end and charges count their calls.

Checked: without the switch every entry refuses and nothing is called; with it, the frame's end and a charge are refused
from the host's thread, with no runtime, for an address that is not a CP or PE halfword or a PI FIFO word, for a
misaligned one, a wrong size and an unknown flag, and otherwise each reaches its runtime function once (a read's charge
and a write's apart). Wants gcc or clang on an ELF system (SOA_CC).
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
extern int g_gx_off_stub;
extern unsigned g_gx_frames, g_gx_charge_reads, g_gx_charge_writes;
void host_frame_end(CpuState* s, unsigned frame);
static unsigned pe(void* user) { (void)user; return 0; }
static volatile uint64_t drawn;

static void charge(CpuState* s, const char* what, uint32_t a, unsigned size, unsigned flags)
{
    unsigned r0 = g_gx_charge_reads, w0 = g_gx_charge_writes;
    int r = soa_host_gx_charge(s, a, size, flags, 5);
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
        unsigned f0 = g_gx_frames;
        int r;
        g_gx_off_stub = (int)k;
        printf("switch %u: off %d native %d\n", k, soa_host_gx_off(), soa_host_gx_native(pe, NULL, &drawn));
        r = soa_host_gx_frame_end(&s);
        printf("frame end: %d frames %u\n", r, g_gx_frames - f0);
        r = soa_host_gx_frame_end(NULL);
        printf("frame end, no runtime: %d\n", r);
        charge(&s, "CP read", 0xCC000000u, 2, 0);
        charge(&s, "PE write", 0xCC00100Au, 2, SOA_HOST_GX_WRITE);
        charge(&s, "PI wptr read", 0xCC003014u, 4, 0);
        charge(&s, "PI cause", 0xCC003000u, 4, 0);
        charge(&s, "VI", 0xCC002000u, 2, 0);
        charge(&s, "odd", 0xCC000001u, 2, 0);
        charge(&s, "wide CP", 0xCC000000u, 4, 0);
        charge(&s, "probe flag", 0xCC000000u, 2, SOA_HOST_GX_PROBE);
        {
            unsigned r0 = g_gx_charge_reads;
            r = soa_host_gx_charge(NULL, 0xCC000000u, 2, 0, 0);
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
extern int g_gx_off_stub;
extern unsigned g_gx_frames, g_gx_charge_reads;
int main(void)
{
    SoaHostStart start = {SOA_HOST_ABI, "stand-in", NULL, NULL};
    char why[200];
    static uint8_t not_a_cpu[8192];
    g_gx_off_stub = 1; /* the host's thread: refused before anything is called */
    printf("host thread: frame end %d charge %d frames %u reads %u\n", soa_host_gx_frame_end(not_a_cpu),
           soa_host_gx_charge(not_a_cpu, 0xCC000000u, 2, 0, 0), g_gx_frames, g_gx_charge_reads);
    g_gx_off_stub = 0;
    fflush(stdout);
    if (soa_host_start(&start, why, sizeof why)) return 5;
    for (;;) usleep(1000);
}
"""


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("host_gx_off")
    stubs = STUBS.split("int window_pad(")[0]  # the stand-ins, without test_host_strict's guest
    (tmp / "stubs.c").write_text(stubs + GUEST, encoding="utf-8")
    (tmp / "driver.c").write_text(DRIVER, encoding="utf-8")
    exe = tmp / "gxoff"
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
def test_the_gx_off_entries_refuse_without_the_switch_and_charge_only_gx_registers(built):
    env = {k: v for k, v in os.environ.items() if not k.startswith("SOA_")}
    out = subprocess.run([str(built)], capture_output=True, text=True, timeout=60, env=env)
    assert out.returncode == 0, out.stdout + out.stderr
    lines = [line for line in out.stdout.splitlines() if ":" in line]
    refused = [
        "CP read: 0 reads 0 writes 0",
        "PE write: 0 reads 0 writes 0",
        "PI wptr read: 0 reads 0 writes 0",
    ]
    assert (
        lines
        == [
            "host thread: frame end 0 charge 0 frames 0 reads 0",
            "switch 0: off 0 native 0",
            "frame end: 0 frames 0",
            "frame end, no runtime: 0",
            *refused,
            "PI cause: 0 reads 0 writes 0",
            "VI: 0 reads 0 writes 0",
            "odd: 0 reads 0 writes 0",
            "wide CP: 0 reads 0 writes 0",
            "probe flag: 0 reads 0 writes 0",
            "no runtime: 0 reads 0",
            "switch 1: off 1 native 1",
            "frame end: 1 frames 1",
            "frame end, no runtime: 0",
            "CP read: 1 reads 1 writes 0",
            "PE write: 1 reads 0 writes 1",
            "PI wptr read: 1 reads 1 writes 0",
            "PI cause: 0 reads 0 writes 0",  # the interrupt controller's, not GX's: never answered natively
            "VI: 0 reads 0 writes 0",
            "odd: 0 reads 0 writes 0",
            "wide CP: 0 reads 0 writes 0",
            "probe flag: 0 reads 0 writes 0",
            "no runtime: 0 reads 0",
        ]
    )
