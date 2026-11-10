"""A run without the game library (runtime/nogame.c; docs/SWAP.md, "A run without the game library").

nogame.c and the real swap.c with stand-ins for the rest of the runtime: the stand-in table holds the addresses the host
asked to replace before the start and the runtime's own answers; a call of a replaced function is its replacement's, of
a runtime function the runtime's; an address no answer covers, a replacement's hand-back and a request for a
translation each stop the run (exit 3) with the address and why. The compile wants gcc or clang (SOA_CC).
"""

import os
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from soa import toolchain  # noqa: E402
from test_swap import PROFILE, RUNTIME, gnu  # noqa: E402

MAIN = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "soa_game.h"
#include "swap.h"

const SoaGame* soa_game_table;
void fn_8023F704(CpuState* s) { s->gpr[3] = 77; printf("runtime answer\n"); }
const SoaRuntimeAnswer soa_runtime_answers[] = {{0x8023F704u, fn_8023F704}};
const unsigned soa_runtime_answer_count = 1;
void hle_on_report(void (*fn)(void)) { (void)fn; }
void hle_report(void) { printf("report\n"); fflush(stdout); }
void guest_backtrace(CpuState* s, uint32_t sp) { (void)s; fprintf(stderr, " (backtrace from %08X)\n", sp); }
void guest_trap(CpuState* s, uint32_t pc) { (void)s; (void)pc; }
int snapshot_foreign(void) { return 0; }
int mod_call_guest(CpuState* s, uint32_t addr, const uint32_t* ints, uint32_t n_ints, const double* floats,
                   uint32_t n_floats, uint32_t* r3, double* f1, char* why, size_t cap)
{
    (void)s; (void)addr; (void)ints; (void)n_ints; (void)floats; (void)n_floats; (void)r3; (void)f1; (void)why; (void)cap;
    return 0;
}
uint8_t mmio_read8(CpuState* s, uint32_t ea) { (void)s; (void)ea; return 0; }
uint16_t mmio_read16(CpuState* s, uint32_t ea) { (void)s; (void)ea; return 0; }
uint32_t mmio_read32(CpuState* s, uint32_t ea) { (void)s; (void)ea; return 0; }
uint64_t mmio_read64(CpuState* s, uint32_t ea) { (void)s; (void)ea; return 0; }
void dispatch(CpuState* s, uint32_t addr) { soa_game_table->dispatch(s, addr); } /* runtime_seam.c's forwarders */
int dispatch_known(uint32_t addr) { return soa_game_table->dispatch_known(addr); }
int soa_nogame(void);
void soa_nogame_set(int on);
int soa_nogame_build(char* why, size_t cap);

static int how;
static int handler(CpuState* s, uint32_t address, SwapFn fn, void* user, int after)
{
    (void)fn; (void)user; (void)after;
    if (how == 2) { swap_original(s, address); return SWAP_ANSWERED; }
    if (how == 1) return SWAP_GAMES;
    s->gpr[3] = address & 0xFFu;
    return SWAP_ANSWERED;
}

int main(int argc, char** argv)
{
    static CpuState s;
    char why[200];
    (void)argc;
    printf("refused %d\n", soa_nogame_build(why, sizeof why)); /* not asked for */
    swap_set(0x80003140u, handler, NULL, NULL); /* the entry */
    swap_set(0x80100000u, handler, NULL, NULL);
    soa_nogame_set(1);
    {
        int built = soa_nogame_build(why, sizeof why);
        printf("built %d %s\n", built, soa_game_table->record);
    }
    swap_attach();
    printf("count %u kinds %u %u %u\n", soa_game_table->count, soa_game_table->kind[0], soa_game_table->kind[1],
           soa_game_table->kind[2]);
    printf("known %d twin %d\n", soa_game_table->dispatch_known(0x80200000u), soa_game_table->twin(0x8023F704u) != NULL);
    soa_game_table->entry(&s);
    printf("entry %u\n", s.gpr[3]);
    dispatch(&s, 0x80100000u);
    printf("answered %u\n", s.gpr[3]);
    dispatch(&s, 0x8023F704u);
    printf("own %u\n", s.gpr[3]);
    fflush(stdout);
    how = argv[1][0] - '0';
    dispatch(&s, how == 3 ? 0x80200000u : 0x80100000u); /* 1: handed back; 2: its translation asked for; 3: unbound */
    printf("not stopped\n");
    return 0;
}
"""


def build(tmp_path):
    (tmp_path / "main.c").write_text(MAIN, encoding="utf-8")
    sources = ["main.c", str(RUNTIME / "nogame.c"), str(RUNTIME / "swap.c")]
    proc = toolchain.cc(
        [
            *PROFILE.cflags,
            "/DSOA_SPLIT=1",
            f"/I{RUNTIME}",
            f"/Fe{tmp_path / 't'}",
            *sources,
            *PROFILE.linker,
        ],
        tmp_path,
        PROFILE,
    )
    assert proc.returncode == 0, (proc.stdout or "") + (proc.stderr or "")
    return tmp_path / "t"


@gnu
@pytest.mark.parametrize(
    "how,why",
    [
        ("1", "80100000: its answer handed the call back or asked for its translation"),
        ("2", "80100000: its answer handed the call back or asked for its translation"),
        ("3", "80200000: no answer covers this address"),
    ],
)
def test_answers_run_and_anything_else_stops_the_run(tmp_path, how, why):
    exe = build(tmp_path)
    env = {k: v for k, v in os.environ.items() if not k.startswith("SOA_")}
    p = subprocess.run([str(exe), how], capture_output=True, text=True, timeout=30, env=env)
    assert p.returncode == 3, p.stdout + p.stderr
    assert p.stdout.splitlines() == [
        "refused 1",
        "built 0 nogame resume=801DCB84",
        "count 3 kinds 0 0 1",  # the entry and the replaced function, the runtime's own answer
        "known 1 twin 0",  # every address reaches dispatch; no twins
        "entry 64",  # __start, its replacement's
        "answered 0",
        "runtime answer",
        "own 77",
        "report",
    ]
    assert "[nogame] " + why in p.stderr
    assert "not stopped" not in p.stdout
