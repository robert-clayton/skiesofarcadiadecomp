"""soa-compare --no-hardware (docs/RECORD.md, "A native replacement of the
platform"): a native replacement of an SDK function must touch no hardware,
even though the game's call it is held to did.

A synthetic function reads one AI register, adds one, stores the sum to
another AI register and to memory, and returns it. Its calls are recorded
under the real runtime/record.c with the hardware's reads and stores in
them, and replayed by the real tools/replay. The checks: a candidate that
does the memory work natively and touches no hardware fails the ordinary
compare (the game's store to the hardware is missing) and passes
--no-hardware, which lists the register it read and the one it stored as
retired, with their values; a candidate that leaves the call to the game's
translation fails on the hardware read, and one that stores to the hardware
(through a function of the game's) fails on the store. The self check still
holds the translation to every hardware access. The compile wants gcc or
clang on an ELF system (SOA_CC), as test_record.py's does.
"""

import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import replay  # noqa: E402
from soa import embed, seam, toolchain  # noqa: E402
from soa.ppc import cfg  # noqa: E402
from soa.recomp import Emitter, c_name  # noqa: E402
from test_cfg import BASE, BLR, bl, make_dol  # noqa: E402
from test_record import PROFILE, RUNTIME, addi, cc, d_form, gnu, lwz, mr, record, stw  # noqa: E402


def lis(rd, imm):
    return d_form(15, rd, 0, imm)


ENTRY, F, G = BASE, BASE + 12, BASE + 40
# ENTRY: bl F; bl G; blr
# F: r5 = AI[0x6C00] + 1; AI[0x6C04] = r5; *r3 = r5; return r5
# G: AI[0x6C08] = r3
WORDS = [
    bl(F - ENTRY),
    bl(G - (ENTRY + 4)),
    BLR,
    lis(6, 0xCC00),
    lwz(5, 6, 0x6C00),
    addi(5, 5, 1),
    stw(5, 6, 0x6C04),
    stw(5, 3, 0),
    mr(3, 5),
    BLR,
    lis(6, 0xCC00),
    stw(3, 6, 0x6C08),
    BLR,
]

RECORDER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "functions.h"
#include "record.h"
#include "gx_state.h"
#include <string.h>
#include "soa_game.h"
#include "swap.h"

volatile int g_record_armed;
void (*g_record_note)(CpuState* s, int kind, uint32_t ea, uint64_t v);
unsigned g_record_polls;
static void (*g_report)(void);
static uint32_t g_ai = 0x40;
void hle_on_report(void (*fn)(void)) { g_report = fn; }
int snapshot_foreign(void) { return 0; } /* snapshot.c's: no resume here */
uint64_t irq_delivered_count(unsigned n) { (void)n; return 0; }
uint64_t threads_switch_count(void) { return 0; }
unsigned gx_frame_count(void) { return 7; }
uint32_t guest_timebase_lo(CpuState* s) { (void)s; return 0; }
uint32_t guest_timebase_hi(CpuState* s) { (void)s; return 0; }
void guest_trap(CpuState* s, uint32_t pc) { (void)s; printf("trap %08X\n", pc); }
uint32_t g_watch_addr, g_watch_len;
void watch_hit(CpuState* s, uint32_t ea, unsigned size, uint64_t v) { (void)s, (void)ea, (void)size, (void)v; }
/* the hardware as hle.c's entry points tell the recorder of it */
uint32_t mmio_read32(CpuState* s, uint32_t ea)
{
    uint32_t v = ea == 0xCC006C00u ? g_ai : 0;
    if (g_record_armed && g_record_note) g_record_note(s, REC_IN_MMIO | 4 << 8, ea, v);
    return v;
}
void mmio_write32(CpuState* s, uint32_t ea, uint32_t v)
{
    if (g_record_armed && g_record_note) g_record_note(s, REC_MMIO_STORE | 4 << 8, ea, v);
    if (ea == 0xCC006C04u) g_ai = v;
}
void gx_pipe_write(CpuState* s, unsigned size, uint64_t v) { (void)s, (void)size, (void)v; }
/* what record.c reads of gx.c: this program never draws */
void (*g_record_pipe)(CpuState* s, unsigned size, uint64_t v);
void gx_pipe_watch_changed(void) {}
void gx_image(GxImage* out) { memset(out, 0, sizeof *out); }
const uint8_t* gx_pipe_left(uint32_t* len) { static const uint8_t none[1]; *len = 0; return none; }
int mod_call_guest(CpuState* s, uint32_t addr, const uint32_t* ints, uint32_t n_ints, const double* floats,
                   uint32_t n_floats, uint32_t* r3, double* f1, char* why, size_t cap)
{
    (void)s, (void)addr, (void)ints, (void)n_ints, (void)floats, (void)n_floats, (void)r3, (void)f1, (void)why, (void)cap;
    return 0;
}

int main(void)
{
    static CpuState s;
    unsigned k;
    s.mem = (uint8_t*)mmap(NULL, MEM_IMAGE_SIZE + 0x10000u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (s.mem == MAP_FAILED) return 3;
    s.gpr[2] = 0x80350000u, s.gpr[13] = 0x8034E720u;
    record_env(&s);
    for (k = 0; k < 6; k++) {
        s.gpr[1] = 0x80200000u;
        s.gpr[3] = 0x80100000u;
        ENTRY_FN(&s);
        printf("call %u: r3 %u\n", k, s.gpr[3]);
    }
    if (g_report) g_report();
    return 0;
}
"""

CANDIDATES = r"""
#include "replay.h"

/* What a native replacement does: AI's register is the native side's own
 * (the call's r31 here stands for it: the value the game read is the
 * recording's), so it is read from nowhere; the memory effect is made. */
static uint32_t g_native_ai = 0x40;
static int native(void* user, SoaHostCall* c)
{
    (void)user;
    if (c->after) return SOA_HOST_GAMES;
    g_native_ai += 1;
    soa_replay_w32(c, c->gpr[0], g_native_ai);
    c->gpr[0] = g_native_ai;
    return SOA_HOST_ANSWERED;
}

/* the game's own translation, which reads the hardware */
static int leaves(void* user, SoaHostCall* c)
{
    (void)user, (void)c;
    return SOA_HOST_GAMES;
}

/* native, but it calls a function of the game's that stores to the hardware */
static int stores(void* user, SoaHostCall* c)
{
    uint32_t arg = 5, r3 = 0;
    native(user, c);
    if (!c->after) c->game(c, G_ADDR, &arg, 1, &r3);
    return SOA_HOST_ANSWERED;
}

SOA_REPLAY_CANDIDATE(native, "native", F_ADDR, native, 0, SOA_REPLAY_R3)
SOA_REPLAY_CANDIDATE(leaves, "leaves", F_ADDR, leaves, 0, SOA_REPLAY_R3)
SOA_REPLAY_CANDIDATE(stores, "stores", F_ADDR, stores, 0, SOA_REPLAY_R3)
"""


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("nohw")
    dol = make_dol(WORDS)
    fns, _ = cfg.build_iterative(dol)
    assert sorted(fns) == [ENTRY, F, G]
    em = Emitter(dol, fns, cfg.find_jump_tables(dol), swap=True)
    (tmp / "functions.h").write_text(em.prototypes(), encoding="utf-8")
    (tmp / "dispatch.c").write_text(em.dispatch_c(), encoding="utf-8")
    body = ['#include "functions.h"', *(em.function_c(fns[a]) for a in sorted(fns))]
    (tmp / "chunk.c").write_text("\n".join(body) + "\n", encoding="utf-8")
    (tmp / "disc_sys.c").write_text(embed.disc_sys_c(None), encoding="utf-8")
    (tmp / "game_table.c").write_text(
        seam.game_table_c(seam.Seam((), (), ()), set(), fns, "abi=1 mode=test", functions=fns),
        encoding="utf-8",
    )
    (tmp / "main.c").write_text(RECORDER.replace("ENTRY_FN", c_name(ENTRY)), encoding="utf-8")
    game = ["chunk.c", "dispatch.c", "game_table.c", "disc_sys.c"]
    cc(
        tmp,
        [
            f"/Fe{tmp / 'recorder'}",
            "main.c",
            *game,
            str(RUNTIME / "record.c"),
            str(RUNTIME / "swap.c"),
            *map(str, toolchain.runtime_support_sources()),
            *PROFILE.linker,
        ],
    )
    cc(tmp, ["-fPIC", "-shared", f"/Fe{tmp / 'libsoa_game.so'}", *game])
    (tmp / "candidates.c").write_text(
        CANDIDATES.replace("F_ADDR", f"0x{F:08X}u").replace("G_ADDR", f"0x{G:08X}u"),
        encoding="utf-8",
    )
    exe = replay.build(tmp / "replay", [tmp / "candidates.c"], cc=PROFILE.name)
    record(tmp, "f.soarec", f"0x{F:08X}")
    return tmp, exe


def compare(tmp: Path, exe: Path, *args):
    return subprocess.run(
        [str(exe), str(tmp / "f.soarec"), "--game", str(tmp / "libsoa_game.so"), *args],
        capture_output=True,
        text=True,
        timeout=60,
    )


@gnu
def test_the_recording_has_the_hardware_and_the_self_check_holds_it(built):
    tmp, exe = built
    info = compare(tmp, exe, "--info")
    assert f"{F:08X}: 6 calls (0 with a flag" in info.stdout, info.stdout
    assert "makes 2.0 hardware and timebase accesses" in info.stdout
    out = compare(tmp, exe, "--original")
    assert out.returncode == 0, out.stdout + out.stderr
    assert "6 calls: 6 agree" in out.stdout
    # --no-hardware is for candidates: the translation is still held to every access
    out = compare(tmp, exe, "--original", "--no-hardware")
    assert out.returncode == 0 and "6 calls: 6 agree" in out.stdout, out.stdout


@gnu
def test_a_native_candidate_fails_the_ordinary_compare_and_passes_no_hardware(built):
    tmp, exe = built
    plain = compare(tmp, exe, "--candidate", "native")
    assert plain.returncode == 1
    assert "6 calls: 0 agree, 6 do not" in plain.stdout, plain.stdout
    assert "the game's call stored 41 to the hardware at CC006C04, and this did not" in plain.stdout

    out = compare(tmp, exe, "--candidate", "native", "--no-hardware")
    assert out.returncode == 0, out.stdout + out.stderr
    assert "held to the recording and to no hardware" in out.stdout
    assert "6 calls: 6 agree, 0 do not, 0 not replayable" in out.stdout
    assert "--no-hardware: the candidate touched the hardware in 0 calls" in out.stdout
    # what it is expected to have reproduced some other way, with the values the game's calls saw
    assert "the hardware the game's calls touched, retired" in out.stdout
    assert (
        "AI      CC006C00 read32: 6 times in 6 of 6 calls; values 40, 41, 42, 43, ..." in out.stdout
    )
    assert (
        "AI      CC006C04 store32: 6 times in 6 of 6 calls; values 41, 42, 43, 44, ..."
        in out.stdout
    )


@gnu
def test_a_candidate_that_touches_the_hardware_is_caught(built):
    tmp, exe = built
    out = compare(tmp, exe, "--candidate", "leaves", "--no-hardware")
    assert out.returncode == 1
    assert "6 calls: 0 agree, 6 do not, 0 not replayable" in out.stdout, out.stdout
    assert "a read of AI CC006C00 (the call ends there)" in out.stdout
    assert "touched the hardware in 6 calls" in out.stdout

    out = compare(tmp, exe, "--candidate", "stores", "--no-hardware")
    assert out.returncode == 1
    assert "6 calls: 0 agree, 6 do not" in out.stdout, out.stdout
    assert "1 store(s); the first AI CC006C08 = 5" in out.stdout
