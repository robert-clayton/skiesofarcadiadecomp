"""Any function swapped at run time (docs/SWAP.md).

The recompiler's guard (soa.recomp.Emitter, swap=True), the swap table
tools/soa/seam.py writes into game_table.c, and runtime/swap.c: checked as
text first, then the real thing -- a synthetic program of two functions
translated, compiled with the real swap.c and run, its callee answered by a
replacement in each of the ways one can, counted, and a request made before
the table exists kept until it does. The compile wants gcc or clang (SOA_CC)
and skips without one, as on Windows; runtime/selftest.c holds the same
mechanism to the game's own sprintf in every build.
"""

import os
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from soa import embed, seam, toolchain  # noqa: E402
from soa.ppc import cfg  # noqa: E402
from soa.recomp import Emitter, c_name  # noqa: E402
from soa.recomp.emit import SWAP_GUARD  # noqa: E402
from test_cfg import BASE, BLR, bl, make_dol  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / "runtime"
PROFILE = toolchain.profile(os.environ.get("SOA_CC"))
gnu = pytest.mark.skipif(
    os.name == "nt"
    or PROFILE.name not in ("gcc", "clang")
    or toolchain.compiler_path(PROFILE) is None,
    reason="needs gcc or clang (SOA_CC) on an ELF system",
)

CALLER, CALLEE = BASE, BASE + 8


def addi(rd, ra, imm):
    return (14 << 26) | (rd << 21) | (ra << 16) | (imm & 0xFFFF)


# CALLER: bl CALLEE; blr     CALLEE: addi r3,r3,1; blr
WORDS = [bl(8), BLR, addi(3, 3, 1), BLR]


def program(swap=True, hle=None):
    dol = make_dol(WORDS)
    fns, _ = cfg.build_iterative(dol)
    em = Emitter(dol, fns, cfg.find_jump_tables(dol), hle=hle, swap=swap)
    return em, fns


def test_every_translated_function_starts_with_its_guard():
    em, fns = program()
    assert sorted(fns) == [CALLER, CALLEE]
    for n, a in enumerate(sorted(fns)):
        lines = em.function_c(fns[a]).splitlines()
        assert lines[:3] == [f"void {c_name(a)}(CpuState* s)", "{", f"    SOA_SWAP({n});"]
        # before the entry's own label: a loop back to the entry is not a call
        assert lines[3] == f"L_{a:08X}:;"


def test_a_function_the_runtime_answers_has_no_guard_and_the_table_says_so():
    em, fns = program(hle={CALLEE: "native"})
    callee = em.function_c(fns[CALLEE])
    assert callee.splitlines()[1:] and "SOA_SWAP" not in callee
    assert callee.startswith("/* native -- bound to HLE")
    assert "SOA_SWAP(0);" in em.function_c(fns[CALLER])
    defs, fields = seam.swap_table_c(fns, {CALLEE})
    assert fields == "soa_swap_flags, g_swap_address, g_swap_kind, 2u"
    assert f"0x{CALLER:08X}u, 0x{CALLEE:08X}u" in defs
    assert "g_swap_kind[2] = {\n    0, 1\n}" in defs


def test_without_swap_the_function_is_as_it_always_was():
    em, fns = program(swap=False)
    assert all("SOA_SWAP" not in em.function_c(f) for f in fns.values())
    assert "SOA_SWAP" not in em.prototypes()


def test_the_guard_is_declared_once_in_functions_h():
    em, _ = program()
    proto = em.prototypes()
    assert proto.count("extern SOA_SWAP_HIDDEN uint8_t soa_swap_flags[];") == 1
    assert SWAP_GUARD in proto
    assert "volatile uint8_t" in SWAP_GUARD  # the host sets the byte from its own thread


def test_a_table_without_functions_has_no_swap_table():
    table = seam.game_table_c(seam.read_seam(ROOT / "config" / "seam.txt"), set(), [], "abi=1")
    assert "soa_swap_flags" not in table
    assert "twin, 0, 0, 0, 0};" in table


def test_the_seam_carries_the_guard_s_one_call_into_the_runtime():
    s = seam.read_seam(ROOT / "config" / "seam.txt")
    assert "soa_swap_enter" in s.runtime
    assert "soa_swap_enter" in seam.runtime_exports(s, set())


MAIN = r"""
#include <stdio.h>
#include "functions.h"
#include "soa_game.h"
#include "swap.h"

const SoaGame* soa_game_table; /* a split build's, set once "loaded" */
static int mode;
static uint32_t calls_after;

/* what swap.c needs of the rest of the runtime */
int mod_call_guest(CpuState* s, uint32_t addr, const uint32_t* ints, uint32_t n_ints, const double* floats,
                   uint32_t n_floats, uint32_t* r3, double* f1, char* why, size_t cap)
{
    (void)ints; (void)n_ints; (void)floats; (void)n_floats; (void)why; (void)cap;
    uint32_t keep = s->gpr[3];
    s->gpr[3] = 7;
    dispatch(s, addr);
    if (r3) *r3 = s->gpr[3];
    if (f1) *f1 = 0.0;
    s->gpr[3] = keep;
    return 1;
}
void hle_on_report(void (*fn)(void)) { (void)fn; }
void guest_trap(CpuState* s, uint32_t pc) { (void)s; printf("trap %08X\n", pc); }

static int handler(CpuState* s, uint32_t address, SwapFn fn, void* user, int after)
{
    (void)fn; (void)user;
    switch (mode) {
    case 1: s->gpr[3] = 100; return SWAP_ANSWERED;
    case 2: if (after) s->gpr[3] += 1000; else calls_after = s->gpr[3]; return SWAP_BOTH;
    case 3: swap_original(s, address); s->gpr[3] *= 2; return SWAP_ANSWERED;
    case 4: {
        uint32_t r3 = 0;
        swap_call_original(s, address, NULL, 0, NULL, 0, &r3, NULL, NULL, 0);
        s->gpr[3] = r3 + 50; /* the original's answer for 7, plus 50 */
        return SWAP_ANSWERED;
    }
    default: return SWAP_GAMES;
    }
}

/* the fixed clock's quiet rule: an entry that finds SWAP_DUE takes it */
static unsigned taken;
static void take(CpuState* s)
{
    (void)s;
    taken++;
    swap_due(0);
}

static uint32_t run(CpuState* s, int m)
{
    mode = m;
    s->gpr[3] = 40;
    CALLER_FN(s);
    return s->gpr[3];
}

int main(void)
{
    static CpuState s;
    SwapCount c[4];
    unsigned n, k, native = 9;
    int early;
    /* a host asks for many before the start: every request is kept (no
     * fixed limit), the last for an address wins, and those that name no
     * function are refused only once the table is there */
    for (k = 0, n = 0; k < 1000; k++) n += (unsigned)swap_set(0x80100000u + 4u * k, handler, NULL, NULL);
    printf("kept many %u\n", n);
    early = swap_set(CALLEE_ADDR, handler, NULL, NULL);    /* no table yet: kept */
    printf("before %d index %d\n", early, swap_index(CALLEE_ADDR));
    soa_game_table = soa_game();
    swap_attach();
    printf("kept %u\n", run(&s, 1));
    printf("games %u\n", run(&s, 0));
    k = run(&s, 2);
    printf("both %u saw %u\n", k, calls_after);
    printf("original %u\n", run(&s, 3));
    printf("call_original %u\n", run(&s, 4));
    swap_count(1);
    run(&s, 1);
    run(&s, 0);
    run(&s, 3);
    swap_count(0);
    run(&s, 0); /* not counted */
    n = swap_counts(c, 4);
    for (k = 0; k < n; k++)
        printf("count %08X replaced %u calls %llu answered %llu translated %llu\n", c[k].address, c[k].replaced,
               (unsigned long long)c[k].calls, (unsigned long long)c[k].answered,
               (unsigned long long)c[k].translated);
    k = swap_functions(&native);
    printf("functions %u native %u\n", k, native);
    printf("refused %d %d\n", swap_set(CALLEE_ADDR + 4, handler, NULL, NULL), swap_set(0x80000000u, handler, NULL, NULL));
    swap_set(CALLEE_ADDR, NULL, NULL, NULL);
    k = run(&s, 1);
    printf("cleared %u flags %u %u\n", k, soa_game()->swap[0], soa_game()->swap[1]);
    swap_due_handler(take);
    swap_due(1);
    printf("due flags %u %u\n", soa_game()->swap[0], soa_game()->swap[1]);
    k = run(&s, 0); /* the caller's entry takes it; the callee's finds it gone */
    printf("due taken %u, %u, flags %u %u\n", taken, k, soa_game()->swap[0], soa_game()->swap[1]);
    printf("answers %llu\n", (unsigned long long)g_swap_answers);
    return 0;
}
"""


@gnu
def test_a_translated_function_is_answered_in_each_way_and_counted(tmp_path):
    em, fns = program()
    (tmp_path / "functions.h").write_text(em.prototypes(), encoding="utf-8")
    (tmp_path / "dispatch.c").write_text(em.dispatch_c(), encoding="utf-8")
    body = ['#include "functions.h"', *(em.function_c(fns[a]) for a in sorted(fns))]
    (tmp_path / "chunk.c").write_text("\n".join(body) + "\n", encoding="utf-8")
    (tmp_path / "disc_sys.c").write_text(embed.disc_sys_c(None), encoding="utf-8")
    no_twins = seam.Seam((), (), ())
    (tmp_path / "game_table.c").write_text(
        seam.game_table_c(no_twins, set(), fns, "abi=1", functions=fns), encoding="utf-8"
    )
    (tmp_path / "main.c").write_text(
        MAIN.replace("CALLER_FN", c_name(CALLER)).replace("CALLEE_ADDR", f"0x{CALLEE:08X}u"),
        encoding="utf-8",
    )
    proc = toolchain.cc(
        [
            *PROFILE.cflags,
            "/DSOA_SPLIT=1",
            f"/I{RUNTIME}",
            f"/I{tmp_path}",
            f"/Fe{tmp_path / 't'}",
            "main.c",
            "chunk.c",
            "dispatch.c",
            "game_table.c",
            "disc_sys.c",
            str(RUNTIME / "swap.c"),
            *PROFILE.linker,
        ],
        tmp_path,
        PROFILE,
    )
    assert proc.returncode == 0, (proc.stdout or "") + (proc.stderr or "")
    run = subprocess.run([str(tmp_path / "t")], capture_output=True, text=True, timeout=30)
    assert run.returncode == 0, run.stdout + run.stderr
    assert run.stdout.splitlines() == [
        "kept many 1000",  # no fixed limit on requests made before the start
        "before 1 index -3",  # kept: there is no table to ask yet
        "kept 100",  # the request made before the table was applied when it came
        "games 41",
        "both 1041 saw 40",
        "original 82",
        "call_original 58",  # the translation's 8 for the 7 passed, plus 50
        # counted: three calls of each; the callee answered twice (in its
        # place, and around its original), its translation run twice (for the
        # game, and for the replacement through swap_original)
        f"count {CALLER:08X} replaced 0 calls 3 answered 0 translated 3",
        f"count {CALLEE:08X} replaced 1 calls 3 answered 2 translated 2",
        "functions 2 native 0",
        "refused 0 0",
        "cleared 41 flags 0 0",
        # the quiet rule's due bit (SWAP_DUE, 8) in every byte, taken at the first entry
        "due flags 8 8",
        "due taken 1, 41, flags 0 0",
        # every call a replacement answered, counted or not, for the quiet
        # rule's waits (docs/CLOCK.md): kept, both, original, call_original,
        # and the two counted answers; not the games, not the cleared one
        "answers 6",
    ]
    # each of the 1000 that named no function said so once the table came
    assert run.stderr.count("not a function of the game's") == 1000
