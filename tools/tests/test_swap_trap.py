"""The trap (SOA_TRAP_TRANSLATED; docs/SWAP.md, "The trap").

test_swap.py's synthetic program of two translated functions, compiled with the real swap.c: every entry into a
translated body is counted by function and by kind (the game's call, a replacement's hand-back, the translation run for
a replacement, a resume), an answered call is not, the counts equal the counter's translated column, the report file
has each function's first call chain, and the strict form stops the run with the address and the chain, but for the
functions SOA_TRAP_ALLOW lists. The compile wants gcc or clang (SOA_CC) and skips without one.
"""

import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from soa import embed, seam, toolchain  # noqa: E402
from soa.recomp import c_name  # noqa: E402
from test_swap import CALLEE, CALLER, PROFILE, RUNTIME, gnu, program  # noqa: E402

MAIN = r"""
#include <stdio.h>
#include <stdlib.h>
#include "functions.h"
#include "soa_game.h"
#include "swap.h"

const SoaGame* soa_game_table;
static int mode;
static void (*reports[4])(void);
static unsigned nreports;

int mod_call_guest(CpuState* s, uint32_t addr, const uint32_t* ints, uint32_t n_ints, const double* floats,
                   uint32_t n_floats, uint32_t* r3, double* f1, char* why, size_t cap)
{
    (void)s; (void)addr; (void)ints; (void)n_ints; (void)floats; (void)n_floats; (void)r3; (void)f1; (void)why; (void)cap;
    return 0;
}
void hle_on_report(void (*fn)(void)) { if (nreports < 4) reports[nreports++] = fn; }
static void report(void) { printf("hle_report\n"); fflush(stdout); }
void guest_trap(CpuState* s, uint32_t pc) { (void)s; printf("trap %08X\n", pc); }
int snapshot_foreign(void) { return 0; }
uint8_t mmio_read8(CpuState* s, uint32_t ea) { (void)s; (void)ea; return 0; }
uint16_t mmio_read16(CpuState* s, uint32_t ea) { (void)s; (void)ea; return 0; }
uint32_t mmio_read32(CpuState* s, uint32_t ea) { (void)s; (void)ea; return 0; }
uint64_t mmio_read64(CpuState* s, uint32_t ea) { (void)s; (void)ea; return 0; }

static int handler(CpuState* s, uint32_t address, SwapFn fn, void* user, int after)
{
    (void)fn; (void)user; (void)after;
    switch (mode) {
    case 1: s->gpr[3] = 100; return SWAP_ANSWERED;
    case 3: swap_original(s, address); return SWAP_ANSWERED;
    default: return SWAP_GAMES;
    }
}

static void wr(CpuState* s, uint32_t ea, uint32_t v) { *(uint32_t*)(s->mem + (ea & MEM_MASK)) = BSWAP32(v); }

static void run(CpuState* s, int m)
{
    mode = m;
    s->gpr[3] = 40;
    s->lr = 0x80001234u;
    s->gpr[1] = 0x80010000u; /* a back chain of two frames: saved LRs 0x80005678 and 0x8000ABCC */
    wr(s, 0x80010000u, 0x80010020u);
    wr(s, 0x80010024u, 0x80005678u);
    wr(s, 0x80010020u, 0x80010040u);
    wr(s, 0x80010044u, 0x8000ABCCu);
    wr(s, 0x80010040u, 0);
    CALLER_FN(s);
}

int main(int argc, char** argv)
{
    static CpuState s;
    SwapCount c[4];
    unsigned n, k, fns = 0;
    (void)argc;
    s.mem = (uint8_t*)calloc(1, MEM_MASK + 1u);
    soa_game_table = soa_game();
    swap_attach();
    swap_set_stop_hook(report);
    swap_env(); /* SOA_TRAP_TRANSLATED and SOA_COUNT_CALLS from the environment */
    printf("trap %d flags %u %u\n", swap_trap_on(), soa_game()->swap[0], soa_game()->swap[1]);
    if (argv[1] && argv[1][0] == 's') { /* strict: the callee, unanswered, stops the run (the caller is allowed) */
        run(&s, 0);
        printf("not stopped\n");
        return 0;
    }
    run(&s, 0); /* both translated: the game's calls */
    swap_set(CALLEE_ADDR, handler, NULL, NULL);
    run(&s, 1); /* the callee answered: not an entry; the caller is */
    run(&s, 0); /* handed back */
    run(&s, 3); /* answered around its translation: an original */
    swap_trap_note(&s, CALLER_ADDR, SWAP_TRAP_RESUME);
    printf("entries %llu", (unsigned long long)swap_trap_entries(&fns));
    printf(" in %u\n", fns);
    n = swap_counts(c, 4);
    for (k = 0; k < n; k++)
        printf("count %08X translated %llu\n", c[k].address, (unsigned long long)c[k].translated);
    for (k = 0; k < nreports; k++) reports[k]();
    return 0;
}
"""


def build(tmp_path):
    em, fns = program()
    (tmp_path / "functions.h").write_text(em.prototypes(), encoding="utf-8")
    (tmp_path / "dispatch.c").write_text(em.dispatch_c(), encoding="utf-8")
    body = ['#include "functions.h"', *(em.function_c(fns[a]) for a in sorted(fns))]
    (tmp_path / "chunk.c").write_text("\n".join(body) + "\n", encoding="utf-8")
    (tmp_path / "disc_sys.c").write_text(embed.disc_sys_c(None), encoding="utf-8")
    (tmp_path / "game_table.c").write_text(
        seam.game_table_c(seam.Seam((), (), ()), set(), fns, "abi=1", functions=fns), encoding="utf-8"
    )
    (tmp_path / "main.c").write_text(
        MAIN.replace("CALLER_FN", c_name(CALLER))
        .replace("CALLEE_ADDR", f"0x{CALLEE:08X}u")
        .replace("CALLER_ADDR", f"0x{CALLER:08X}u"),
        encoding="utf-8",
    )
    sources = ["main.c", "chunk.c", "dispatch.c", "game_table.c", "disc_sys.c", str(RUNTIME / "swap.c")]
    proc = toolchain.cc(
        [*PROFILE.cflags, "/DSOA_SPLIT=1", f"/I{RUNTIME}", f"/I{tmp_path}", f"/Fe{tmp_path / 't'}", *sources, *PROFILE.linker],
        tmp_path,
        PROFILE,
    )
    assert proc.returncode == 0, (proc.stdout or "") + (proc.stderr or "")
    return tmp_path / "t"


def run(exe, env, *args):
    e = {k: v for k, v in os.environ.items() if not k.startswith("SOA_")}
    e.update(env)
    return subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=30, env=e)


@gnu
def test_every_translated_entry_is_counted_by_kind_and_equals_the_counter(tmp_path):
    exe = build(tmp_path)
    out = tmp_path / "trap.tsv"
    p = run(exe, {"SOA_TRAP_TRANSLATED": "count", "SOA_TRAP_FILE": str(out), "SOA_COUNT_CALLS": "1"})
    assert p.returncode == 0, p.stdout + p.stderr
    lines = p.stdout.splitlines()
    assert lines[0] == "trap 1 flags 18 18"  # SWAP_TRAP (16) and SWAP_COUNT (2) in every guarded byte
    # the caller 4 calls and a resume; the callee a call, a hand-back, an original: 8 entries in 2 functions
    assert lines[1] == "entries 8 in 2"
    # the counter's translated column holds the same (the resume is the trap's alone: it is past the guard)
    assert lines[2:4] == [f"count {CALLER:08X} translated 4", f"count {CALLEE:08X} translated 3"]
    rows = [r.split("\t") for r in out.read_text().splitlines()]
    assert rows[0] == ["address", "entries", "call", "handback", "original", "resume", "chain"]
    # the first entry's chain: LR, then each saved LR up the back chain
    assert rows[1] == [f"{CALLER:08X}", "5", "4", "0", "0", "1", "80001234 80005678 8000ABCC"]
    assert rows[2][:6] == [f"{CALLEE:08X}", "3", "1", "1", "1", "0"]
    assert rows[2][6].split()[0] == f"{CALLER + 4:08X}"  # called from the caller's bl
    assert "[trap] 8 translated entries in 2 functions" in p.stderr


@gnu
def test_the_strict_trap_stops_at_the_first_entry_with_its_chain_but_for_the_allowed(tmp_path):
    exe = build(tmp_path)
    p = run(exe, {"SOA_TRAP_TRANSLATED": "strict", "SOA_TRAP_ALLOW": f"{CALLER:08X}"}, "strict")
    assert p.returncode == 3, p.stdout + p.stderr
    assert "not stopped" not in p.stdout and "hle_report" in p.stdout
    assert f"the translation of {CALLEE:08X} entered (call)" in p.stderr
    assert f"the call chain: {CALLER + 4:08X} 80005678 8000ABCC" in p.stderr
    # nothing allowed: the caller is the first entry
    p = run(exe, {"SOA_TRAP_TRANSLATED": "strict"}, "strict")
    assert p.returncode == 3 and f"the translation of {CALLER:08X} entered (call)" in p.stderr


@gnu
def test_without_the_switch_nothing_is_trapped(tmp_path):
    exe = build(tmp_path)
    p = run(exe, {})
    assert p.returncode == 0, p.stdout + p.stderr
    assert p.stdout.splitlines()[:2] == ["trap 0 flags 0 0", "entries 0 in 0"]
