"""Real calls recorded and replayed (docs/RECORD.md).

A synthetic program of two functions is translated and run under the real
runtime/record.c and runtime/swap.c, recording real calls of both; the same
translation is built as a game library and the recording replayed against it
by the real tools/replay (replay.c and compare.c, built by tools/replay.py)
with candidates in C. The checks are the ones the tool exists for: the
translation agrees with its own recording on every call, a candidate that
does the function's work agrees, one that is wrong on one call is caught on
that call and no other, one that reads memory the function never touched is
told so, a candidate that leaves the call to the game is counted as such,
and a recording with one register of one call changed fails the self check
(the oracle can fail). The compile wants gcc or clang on an ELF system
(SOA_CC) and skips without one, as on Windows.
"""

import os
import struct
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

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / "runtime"
PROFILE = toolchain.profile(os.environ.get("SOA_CC"))
gnu = pytest.mark.skipif(
    os.name == "nt"
    or PROFILE.name not in ("gcc", "clang")
    or toolchain.compiler_path(PROFILE) is None,
    reason="needs gcc or clang (SOA_CC) on an ELF system",
)


def d_form(op, rd, ra, imm):
    return (op << 26) | (rd << 21) | (ra << 16) | (imm & 0xFFFF)


def lwz(rd, ra, d):
    return d_form(32, rd, ra, d)


def stw(rs, ra, d):
    return d_form(36, rs, ra, d)


def addi(rd, ra, imm):
    return d_form(14, rd, ra, imm)


def mr(ra, rs):
    return (31 << 26) | (rs << 21) | (ra << 16) | (rs << 11) | (444 << 1)


def mftb(rd):
    return (31 << 26) | (rd << 21) | (392 << 11) | (371 << 1)


CALLER, CALLEE, TIMED = BASE, BASE + 12, BASE + 32
# CALLER: bl CALLEE; bl TIMED; blr
# CALLEE: r4 = *r3 + 1; *r3 = r4; return r4
# TIMED:  the timebase into the word 16 under r1
WORDS = [
    bl(12),
    bl(28),
    BLR,
    lwz(4, 3, 0),
    addi(4, 4, 1),
    stw(4, 3, 0),
    mr(3, 4),
    BLR,
    mftb(5),
    stw(5, 1, -16),
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

/* what record.c and swap.c need of the rest of the runtime */
volatile int g_record_armed;
void (*g_record_note)(CpuState* s, int kind, uint32_t ea, uint64_t v);
unsigned g_record_polls;
static void (*g_report)(void);
static uint32_t g_tb = 1000;
void hle_on_report(void (*fn)(void)) { g_report = fn; }
uint64_t irq_delivered_count(unsigned n) { (void)n; return 0; }
uint64_t threads_switch_count(void) { return 0; }
unsigned gx_frame_count(void) { return 42; }
uint32_t guest_timebase_lo(CpuState* s)
{
    uint32_t v = g_tb += 7;
    if (g_record_armed && g_record_note) g_record_note(s, REC_IN_TB_LO, 0, v);
    return v;
}
uint32_t guest_timebase_hi(CpuState* s) { (void)s; return 0; }
void guest_trap(CpuState* s, uint32_t pc) { (void)s; printf("trap %08X\n", pc); }
uint32_t g_watch_addr, g_watch_len;
void watch_hit(CpuState* s, uint32_t ea, unsigned size, uint64_t v) { (void)s, (void)ea, (void)size, (void)v; }
uint32_t mmio_read32(CpuState* s, uint32_t ea) { (void)s, (void)ea; return 0; }
void mmio_write32(CpuState* s, uint32_t ea, uint32_t v) { (void)s, (void)ea, (void)v; }
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
    for (k = 0; k < 12; k++) {
        uint32_t at = 0x80100000u + 0x2000u * (k % 3);
        mem_w32(&s, at, 100u * k);
        s.gpr[1] = 0x80200000u;
        s.gpr[3] = at;
        s.gpr[31] = 0x31u + k;
        CALLER_FN(&s);
        printf("call %u: r3 %u\n", k, s.gpr[3]);
    }
    if (g_report) g_report();
    return 0;
}
"""

CANDIDATES = r"""
#include "replay.h"

/* the function's own work */
static int good(void* user, SoaHostCall* c)
{
    uint32_t v;
    (void)user;
    if (c->after) return SOA_HOST_GAMES;
    v = soa_replay_r32(c, c->gpr[0]) + 1;
    soa_replay_w32(c, c->gpr[0], v);
    c->gpr[0] = v;
    return SOA_HOST_ANSWERED;
}

/* wrong on the game's fourth call alone */
static int off_by_one(void* user, SoaHostCall* c)
{
    uint32_t v;
    (void)user;
    if (c->after) return SOA_HOST_GAMES;
    v = soa_replay_r32(c, c->gpr[0]) + 1;
    soa_replay_w32(c, c->gpr[0], v);
    c->gpr[0] = soa_replay_seen() == 3 ? v + 1 : v;
    return SOA_HOST_ANSWERED;
}

/* reads a word the game's call never touched */
volatile uint32_t g_seen_there;
static int nosy(void* user, SoaHostCall* c)
{
    (void)user;
    if (c->after) return SOA_HOST_GAMES;
    g_seen_there = soa_replay_r32(c, 0x80300000u);
    return SOA_HOST_GAMES;
}

/* answers the result and leaves the store undone */
static int answers_only(void* user, SoaHostCall* c)
{
    (void)user;
    if (c->after) return SOA_HOST_GAMES;
    c->gpr[0] = soa_replay_r32(c, c->gpr[0]) + 1;
    return SOA_HOST_ANSWERED;
}

/* leaves every call to the game */
static int leaves(void* user, SoaHostCall* c)
{
    (void)user, (void)c;
    return SOA_HOST_GAMES;
}

SOA_REPLAY_CANDIDATE(good, "good", CALLEE_ADDR, good, 0, SOA_REPLAY_R3)
SOA_REPLAY_CANDIDATE(off, "off-by-one", CALLEE_ADDR, off_by_one, 0, SOA_REPLAY_R3)
SOA_REPLAY_CANDIDATE(nosy, "nosy", CALLEE_ADDR, nosy, 0, SOA_REPLAY_R3)
SOA_REPLAY_CANDIDATE(leaves, "leaves", CALLEE_ADDR, leaves, 0, SOA_REPLAY_R3)
SOA_REPLAY_CANDIDATE(answers, "answers-only", CALLEE_ADDR, answers_only, 0, SOA_REPLAY_R3)
"""


def write_program(tmp: Path):
    dol = make_dol(WORDS)
    fns, _ = cfg.build_iterative(dol)
    assert sorted(fns) == [CALLER, CALLEE, TIMED]
    em = Emitter(dol, fns, cfg.find_jump_tables(dol), swap=True)
    (tmp / "functions.h").write_text(em.prototypes(), encoding="utf-8")
    (tmp / "dispatch.c").write_text(em.dispatch_c(), encoding="utf-8")
    body = ['#include "functions.h"', *(em.function_c(fns[a]) for a in sorted(fns))]
    (tmp / "chunk.c").write_text("\n".join(body) + "\n", encoding="utf-8")
    (tmp / "disc_sys.c").write_text(embed.disc_sys_c(None), encoding="utf-8")
    no_twins = seam.Seam((), (), ())
    (tmp / "game_table.c").write_text(
        seam.game_table_c(no_twins, set(), fns, "abi=1 mode=test", functions=fns), encoding="utf-8"
    )


def cc(tmp: Path, args: list[str]):
    proc = toolchain.cc([*PROFILE.cflags, f"/I{RUNTIME}", f"/I{tmp}", *args], tmp, PROFILE)
    assert proc.returncode == 0, (proc.stdout or "") + (proc.stderr or "")


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("record")
    write_program(tmp)
    (tmp / "main.c").write_text(RECORDER.replace("CALLER_FN", c_name(CALLER)), encoding="utf-8")
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
    # the same translation as a game library, as a split build's (its imports met by the replay's)
    cc(tmp, ["-fPIC", "-shared", f"/Fe{tmp / 'libsoa_game.so'}", *game])
    (tmp / "candidates.c").write_text(
        CANDIDATES.replace("CALLEE_ADDR", f"0x{CALLEE:08X}u"), encoding="utf-8"
    )
    exe = replay.build(tmp / "replay", [tmp / "candidates.c"], cc=PROFILE.name)
    return tmp, exe


def record(tmp: Path, name: str, spec: str, extra=None):
    env = dict(os.environ, SOA_RECORD=spec, SOA_RECORD_FILE=str(tmp / name), **(extra or {}))
    run = subprocess.run(
        [str(tmp / "recorder")], capture_output=True, text=True, timeout=60, env=env
    )
    assert run.returncode == 0, run.stdout + run.stderr
    return run


def compare(tmp: Path, exe: Path, rec: str, *args):
    return subprocess.run(
        [str(exe), str(tmp / rec), "--game", str(tmp / "libsoa_game.so"), *args],
        capture_output=True,
        text=True,
        timeout=60,
    )


@gnu
def test_a_recorded_run_is_the_run_without_it(built):
    tmp, _ = built
    plain = subprocess.run([str(tmp / "recorder")], capture_output=True, text=True, timeout=60)
    run = record(tmp, "all.soarec", f"0x{CALLER:08X},0x{CALLEE:08X}")
    assert plain.stdout == run.stdout  # recording changes nothing the game sees
    assert "call 3: r3 301" in run.stdout
    # the caller's 12 calls kept; the callee's are inside them, so its own are not
    assert f"[record] {CALLER:08X}: 12 calls seen, 12 recorded (0 with a flag)" in run.stderr
    assert f"[record] {CALLEE:08X}: 12 calls seen, 0 recorded" in run.stderr


@gnu
def test_the_translation_agrees_with_its_own_recording(built):
    tmp, exe = built
    record(tmp, "callee.soarec", f"0x{CALLEE:08X};5;2")  # 5 at most, one call in 2
    info = compare(tmp, exe, "callee.soarec", "--info")
    assert f"{CALLEE:08X}: 5 calls (0 with a flag" in info.stdout, info.stdout
    assert "makes 0.0 hardware and timebase accesses" in info.stdout
    out = compare(tmp, exe, "callee.soarec", "--original")
    assert out.returncode == 0, out.stdout + out.stderr
    assert "5 calls: 5 agree, 0 do not, 0 not replayable" in out.stdout
    # the caller: the calls it makes made again, the timebase given back as it was read
    record(tmp, "caller.soarec", f"0x{CALLER:08X}")
    info = compare(tmp, exe, "caller.soarec", "--info")
    assert "makes 1.0 hardware and timebase accesses and 2.0 calls" in info.stdout, info.stdout
    out = compare(tmp, exe, "caller.soarec", "--original")
    assert out.returncode == 0, out.stdout + out.stderr
    assert "12 calls: 12 agree" in out.stdout


@gnu
def test_candidates_are_held_call_by_call(built):
    tmp, exe = built
    record(tmp, "callee.soarec", f"0x{CALLEE:08X}")
    out = compare(tmp, exe, "callee.soarec", "--candidate", "good")
    assert out.returncode == 0, out.stdout + out.stderr
    assert "12 calls: 12 agree, 0 do not" in out.stdout
    assert "the candidate answered 12" in out.stdout

    out = compare(tmp, exe, "callee.soarec", "--candidate", "off-by-one")
    assert out.returncode == 1
    assert "12 calls: 11 agree, 1 do not" in out.stdout
    # told where: the game's fourth call, its r3
    assert f"call 3 of {CALLEE:08X} (frame 42):" in out.stdout
    assert "r3 0000012E, the game's 0000012D" in out.stdout

    out = compare(tmp, exe, "callee.soarec", "--candidate", "nosy")
    assert out.returncode == 1
    assert "a load from 80300000, a page the game's call never touched" in out.stdout

    out = compare(tmp, exe, "callee.soarec", "--candidate", "leaves")
    assert out.returncode == 0
    assert "left 12 to the game" in out.stdout


@gnu
def test_a_rule_that_answers_part_of_a_function_is_told_what_it_left(built):
    tmp, exe = built
    record(tmp, "callee.soarec", f"0x{CALLEE:08X}")
    strict = compare(tmp, exe, "callee.soarec", "--candidate", "answers-only")
    assert strict.returncode == 1
    assert "12 calls: 0 agree, 12 do not" in strict.stdout
    # the store it left undone, told as the game's value against what is there still
    assert "80100003+1: 00, the game's 01" in strict.stdout  # big-endian: 0 + 1 in its last byte
    part = compare(tmp, exe, "callee.soarec", "--candidate", "answers-only", "--partial")
    assert part.returncode == 0, part.stdout
    assert "12 calls: 0 agree, 0 do not" in part.stdout
    assert "--partial: 12 more agree on every byte they wrote and every result" in part.stdout
    assert "80100000..8010001F" in part.stdout and "80104000..8010401F" in part.stdout
    # a wrong answer is still wrong under --partial
    off = compare(tmp, exe, "callee.soarec", "--candidate", "off-by-one", "--partial")
    assert "12 calls: 11 agree, 1 do not" in off.stdout


@gnu
def test_a_changed_recording_fails_the_self_check(built):
    tmp, exe = built
    record(tmp, "callee.soarec", f"0x{CALLEE:08X};3")
    data = bytearray((tmp / "callee.soarec").read_bytes())
    head = 8 + 4 * 4 + 192  # RecFileHead
    regs = 32 * 4 + 32 * 16 + 6 * 4 + 8 * 4  # RecRegs
    out_r3 = head + 48 + regs + 3 * 4  # the first call's out.gpr[3]
    (value,) = struct.unpack_from("<I", data, out_r3)
    assert value == 1  # the first call: 0 + 1
    struct.pack_into("<I", data, out_r3, value + 1)
    (tmp / "changed.soarec").write_bytes(bytes(data))
    out = compare(tmp, exe, "changed.soarec", "--original")
    assert out.returncode == 1
    assert "3 calls: 2 agree, 1 do not" in out.stdout
    assert "r3 00000001, the game's 00000002" in out.stdout


@gnu
def test_more_memory_is_kept_when_asked(built):
    tmp, exe = built
    record(tmp, "with.soarec", f"0x{CALLEE:08X}", {"SOA_RECORD_WITH": "0x80300000+4"})
    out = compare(tmp, exe, "with.soarec", "--candidate", "nosy")
    assert out.returncode == 0, out.stdout
    assert "12 calls: 12 agree" in out.stdout
    record(tmp, "every.soarec", f"0x{CALLEE:08X};2", {"SOA_RECORD_WITH": "all"})
    info = compare(tmp, exe, "every.soarec", "--info")
    pages = 0x01800000 // os.sysconf("SC_PAGE_SIZE")
    assert f"a call touches {pages}.0 pages" in info.stdout, info.stdout
    out = compare(tmp, exe, "every.soarec", "--original")
    assert "2 calls: 2 agree" in out.stdout, out.stdout


def test_the_recording_s_layout_is_the_test_s():
    """The offsets test_a_changed_recording_fails_the_self_check writes at
    are record.h's structs, field for field."""
    text = (RUNTIME / "record.h").read_text(encoding="utf-8")
    assert "char game[192];" in text
    assert "uint32_t cr, xer, lr, ctr, fpscr, msr;\n    uint32_t gqr[8];" in text
    assert "    uint64_t seen;" in text and "    uint64_t frame;" in text
