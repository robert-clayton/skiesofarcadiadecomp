"""Calls that draw, recorded and replayed (docs/RECORD.md, "The GX pipe").

A synthetic program of three functions is translated and run under the real
runtime/record.c, swap.c and gx.c: a drawer that writes a BP and a CP load
to the write-gather pipe, a lister that does the same into a display list
(the CPU FIFO pointed at DisplayListFifo through the PI FIFO's registers,
as GXBeginDisplayList does, so gx.c puts the bytes into guest memory), and
a caller of both. The recording keeps each call's pipe bytes and GX's state
before and after; the real tools/replay walks the bytes again from the state
the call began with. Checked: the translation agrees with its recording,
pipe and GX state included (so the replay's walk is gx.c's); a candidate
that writes the same bytes agrees; one that writes a wrong value on one call
is told the byte, the command and the BP register it left wrong; one that
writes nothing fails, and passes --no-pipe, which lists what the game's
calls drew as retired; a changed pipe byte in a recording fails the self
check. And SOA_PIPE_CENSUS counts each function's own pipe bytes, not its
callees'. Wants gcc or clang on an ELF system (SOA_CC), as test_record.py.
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


def addi(rd, ra, imm):
    return d_form(14, rd, ra, imm)


def lis(rd, imm):
    return d_form(15, rd, 0, imm)


def ori(ra, rs, imm):
    return (24 << 26) | (rs << 21) | (ra << 16) | (imm & 0xFFFF)


def stb(rs, ra, d):
    return d_form(38, rs, ra, d)


def stw(rs, ra, d):
    return d_form(36, rs, ra, d)


PIPE = [lis(5, 0xCC01), addi(5, 5, -0x8000)]  # r5 = 0xCC008000
DRAWER_WORDS = [
    *PIPE,
    addi(6, 0, 0x61),
    stb(6, 5, 0),
    stw(3, 5, 0),  # BP: r3
    addi(6, 0, 0x08),
    stb(6, 5, 0),
    addi(6, 0, 0x50),
    stb(6, 5, 0),
    stw(4, 5, 0),  # CP 0x50 = r4
    BLR,
]
LISTER_WORDS = [
    addi(7, 13, -27520),  # GXSetCPUFifo's current FIFO object, r13-relative
    lis(8, 0x8031),
    ori(8, 8, 0x8B18),
    stw(8, 7, 0),  # ... is DisplayListFifo
    lis(9, 0xCC00),
    ori(9, 9, 0x300C),  # the PI FIFO's registers
    lis(10, 0x0010),
    stw(10, 9, 8),  # write pointer 0x00100000
    ori(11, 10, 0x0FFC),
    stw(11, 9, 4),  # top
    stw(10, 9, 0),  # base: gx.c sees the display-list FIFO
    *PIPE,
    addi(6, 0, 0x61),
    stb(6, 5, 0),
    stw(3, 5, 0),  # BP: r3, into the list in memory
    addi(8, 0, 0),
    stw(8, 7, 0),  # the FIFO object is not the list's any more
    stw(10, 9, 0),  # base again: back to the pipe
    BLR,
]
CALLER = BASE
DRAWER = BASE + 12
LISTER = DRAWER + 4 * len(DRAWER_WORDS)
WORDS = [bl(DRAWER - CALLER), bl(LISTER - (CALLER + 4)), BLR, *DRAWER_WORDS, *LISTER_WORDS]

RECORDER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "functions.h"
#include "record.h"
#include "soa_game.h"
#include "swap.h"

/* what record.c, swap.c and gx.c need of the rest of the runtime */
volatile int g_record_armed;
void (*g_record_note)(CpuState* s, int kind, uint32_t ea, uint64_t v);
unsigned g_record_polls;
static void (*g_report[8])(void);
static unsigned g_nreport;
void hle_on_report(void (*fn)(void)) { if (g_nreport < 8) g_report[g_nreport++] = fn; }
void hle_report(void) {}
uint64_t irq_delivered_count(unsigned n) { (void)n; return 0; }
uint64_t threads_switch_count(void) { return 0; }
uint32_t guest_timebase_lo(CpuState* s) { (void)s; return 0; }
uint32_t guest_timebase_hi(CpuState* s) { (void)s; return 0; }
void guest_trap(CpuState* s, uint32_t pc) { (void)s; printf("trap %08X\n", pc); }
uint32_t g_watch_addr, g_watch_len;
void watch_hit(CpuState* s, uint32_t ea, unsigned size, uint64_t v) { (void)s, (void)ea, (void)size, (void)v; }
int gx_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v);
uint32_t mmio_read32(CpuState* s, uint32_t ea) { (void)s, (void)ea; return 0; }
void mmio_write8(CpuState* s, uint32_t ea, uint8_t v) { (void)s, (void)ea, (void)v; } /* (the stb's other arm) */
void mmio_write32(CpuState* s, uint32_t ea, uint32_t v)
{
    if (g_record_armed && g_record_note) g_record_note(s, REC_MMIO_STORE | 4 << 8, ea, v);
    gx_write(s, ea, 4, v);
}
int mod_call_guest(CpuState* s, uint32_t addr, const uint32_t* ints, uint32_t n_ints, const double* floats,
                   uint32_t n_floats, uint32_t* r3, double* f1, char* why, size_t cap)
{
    (void)s, (void)addr, (void)ints, (void)n_ints, (void)floats, (void)n_floats, (void)r3, (void)f1, (void)why, (void)cap;
    return 0;
}
/* gx.c's renderer, which this program has none of */
void gxr_bp_written(CpuState* s, uint32_t reg, uint32_t value) { (void)s, (void)reg, (void)value; }
void gxr_draw(CpuState* s, unsigned op, unsigned count, const uint8_t* verts, unsigned vsize) { (void)s, (void)op, (void)count, (void)verts, (void)vsize; }
void gxr_flush(void) {}
void gxr_report(void) {}
void gxr_reset_efb(void) {}
void gxr_source_hazard(uint32_t addr, uint32_t bytes) { (void)addr, (void)bytes; }
void tex_epoch_advance(void) {}
int gxr_pair_mode(unsigned flags, float t) { (void)flags, (void)t; return 0; }
unsigned long long gxr_pair_rotations(void) { return 0; }
void gxr_pair_list(void* file) { (void)file; }
void gxr_pair_report(void) {}
void gxr_set_output(const char* png_path) { (void)png_path; }
uint64_t gx_pipe_bytes(void);
void gx_set_pipe_census(void (*note)(uint32_t pc, unsigned size));

int main(void)
{
    static CpuState s;
    unsigned k;
    s.mem = (uint8_t*)mmap(NULL, MEM_IMAGE_SIZE + 0x10000u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (s.mem == MAP_FAILED) return 3;
    s.gpr[2] = 0x80350000u, s.gpr[13] = 0x8034E720u;
    swap_env();
    if (swap_pipe_census_on()) gx_set_pipe_census(swap_pipe_census_note);
    record_env(&s);
    for (k = 0; k < 12; k++) {
        s.gpr[1] = 0x80200000u;
        s.gpr[3] = 0x41000000u | (0x111u * k); /* BP 41 */
        s.gpr[4] = 0x1000u + k;                /* CP 50 */
        CALLER_FN(&s);
    }
    printf("pipe %llu bytes; list %08X\n", (unsigned long long)gx_pipe_bytes(), mem_r32(&s, 0x80100000u));
    for (k = 0; k < g_nreport; k++) g_report[k]();
    return 0;
}
"""

CANDIDATES = r"""
#include "replay.h"
#include "cpu.h"

void gx_pipe_write(CpuState* s, unsigned size, uint64_t v); /* the replay's pipe, as the game's stores reach it */

static void draw(SoaHostCall* c, uint32_t bp, uint32_t cp)
{
    CpuState* s = (CpuState*)c->runtime;
    gx_pipe_write(s, 1, 0x61);
    gx_pipe_write(s, 4, bp);
    gx_pipe_write(s, 2, 0x0850); /* the CP load's opcode and register in one store: the bytes are what count */
    gx_pipe_write(s, 4, cp);
}

static int same(void* user, SoaHostCall* c)
{
    (void)user;
    if (c->after) return SOA_HOST_GAMES;
    draw(c, c->gpr[0], c->gpr[1]);
    return SOA_HOST_ANSWERED;
}

/* BP 41 one off on the game's third call */
static int wrong(void* user, SoaHostCall* c)
{
    (void)user;
    if (c->after) return SOA_HOST_GAMES;
    draw(c, c->gpr[0] + (soa_replay_seen() == 2), c->gpr[1]);
    return SOA_HOST_ANSWERED;
}

/* a native drawer: the draw handed on elsewhere, nothing down the pipe */
static int silent(void* user, SoaHostCall* c)
{
    (void)user;
    if (c->after) return SOA_HOST_GAMES;
    return SOA_HOST_ANSWERED;
}

SOA_REPLAY_CANDIDATE(same, "same", DRAWER_ADDR, same, 0, SOA_REPLAY_NONE)
SOA_REPLAY_CANDIDATE(wrong, "wrong", DRAWER_ADDR, wrong, 0, SOA_REPLAY_NONE)
SOA_REPLAY_CANDIDATE(silent, "silent", DRAWER_ADDR, silent, 0, SOA_REPLAY_NONE)
"""

GX_SOURCES = ["record.c", "swap.c", "gx.c"]


def write_program(tmp: Path):
    dol = make_dol(WORDS)
    fns, _ = cfg.build_iterative(dol)
    assert sorted(fns) == [CALLER, DRAWER, LISTER]
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
    tmp = tmp_path_factory.mktemp("record_pipe")
    write_program(tmp)
    (tmp / "main.c").write_text(RECORDER.replace("CALLER_FN", c_name(CALLER)), encoding="utf-8")
    game = ["chunk.c", "dispatch.c", "game_table.c", "disc_sys.c"]
    cc(
        tmp,
        [
            f"/Fe{tmp / 'recorder'}",
            "main.c",
            *game,
            *(str(RUNTIME / f) for f in GX_SOURCES),
            *map(str, toolchain.runtime_support_sources()),
            *PROFILE.linker,
        ],
    )
    cc(tmp, ["-fPIC", "-shared", f"/Fe{tmp / 'libsoa_game.so'}", *game])
    (tmp / "candidates.c").write_text(
        CANDIDATES.replace("DRAWER_ADDR", f"0x{DRAWER:08X}u"), encoding="utf-8"
    )
    exe = replay.build(tmp / "replay", [tmp / "candidates.c"], cc=PROFILE.name)
    return tmp, exe


def run(tmp: Path, extra: dict):
    env = {k: v for k, v in os.environ.items() if not k.startswith("SOA_")}
    env.update(extra)
    out = subprocess.run([str(tmp / "recorder")], capture_output=True, text=True, timeout=60, env=env)
    assert out.returncode == 0, out.stdout + out.stderr
    return out


def record(tmp: Path, name: str, spec: str):
    return run(tmp, {"SOA_RECORD": spec, "SOA_RECORD_FILE": str(tmp / name)})


def compare(tmp: Path, exe: Path, rec: str, *args):
    return subprocess.run(
        [str(exe), str(tmp / rec), "--game", str(tmp / "libsoa_game.so"), *args],
        capture_output=True,
        text=True,
        timeout=60,
    )


@gnu
def test_recording_a_drawer_changes_nothing_it_draws(built):
    tmp, _ = built
    plain = run(tmp, {})
    # 12 calls of 11 bytes to the pipe; the lister's 5 a call into memory, the last call's list there
    assert "pipe 192 bytes; list 6141000B" in plain.stdout, plain.stdout
    rec = record(tmp, "all.soarec", f"0x{CALLER:08X}")
    assert rec.stdout == plain.stdout
    assert f"[record] {CALLER:08X}: 12 of the recorded calls wrote the gather pipe, 192 bytes in all" in rec.stderr


@gnu
def test_the_translation_agrees_on_the_pipe_and_gx_state(built):
    tmp, exe = built
    for fn, rec in ((DRAWER, "drawer.soarec"), (LISTER, "lister.soarec"), (CALLER, "caller.soarec")):
        record(tmp, rec, f"0x{fn:08X}")
        out = compare(tmp, exe, rec, "--original")
        assert out.returncode == 0, out.stdout + out.stderr
        assert "12 calls: 12 agree, 0 do not, 0 not replayable" in out.stdout, out.stdout
        assert "12 of the calls wrote GX's pipe" in out.stdout, out.stdout
    info = compare(tmp, exe, "drawer.soarec", "--info")
    assert f"{DRAWER:08X}: 12 calls wrote GX's pipe, 11.0 bytes a call" in info.stdout, info.stdout


@gnu
def test_a_candidate_is_held_to_the_bytes_and_the_state(built):
    tmp, exe = built
    record(tmp, "drawer.soarec", f"0x{DRAWER:08X}")
    out = compare(tmp, exe, "drawer.soarec", "--candidate", "same")
    assert out.returncode == 0, out.stdout + out.stderr
    assert "12 calls: 12 agree" in out.stdout

    out = compare(tmp, exe, "drawer.soarec", "--candidate", "wrong")
    assert out.returncode == 1
    assert "12 calls: 11 agree, 1 do not" in out.stdout, out.stdout
    assert f"call 2 of {DRAWER:08X}" in out.stdout
    # the byte, the command it is in, and the register it left
    assert "the first difference at byte 4, in BP 41 = 000222, the command at byte 0: 23085000001002, the game's 22085000001002" in out.stdout, out.stdout
    assert "GX BP 41: 00000223, the game's 00000222" in out.stdout

    out = compare(tmp, exe, "drawer.soarec", "--candidate", "silent")
    assert out.returncode == 1
    assert "GX pipe: 0 bytes, the game's 11" in out.stdout, out.stdout


@gnu
def test_a_native_drawer_writes_nothing_down_the_pipe(built):
    tmp, exe = built
    record(tmp, "drawer.soarec", f"0x{DRAWER:08X}")
    out = compare(tmp, exe, "drawer.soarec", "--candidate", "silent", "--no-pipe")
    assert out.returncode == 0, out.stdout + out.stderr
    assert "12 calls: 12 agree" in out.stdout
    assert "what the game's 12 drawing calls sent down the pipe, retired" in out.stdout
    assert "132 bytes, 0 draws (0 vertices), 12 BP, 12 CP" in out.stdout, out.stdout
    out = compare(tmp, exe, "drawer.soarec", "--candidate", "same", "--no-pipe")
    assert out.returncode == 1
    assert "wrote 11 bytes to GX's pipe, where a native drawer must write none" in out.stdout


def first_pipe_byte(data: bytes) -> int:
    """The file offset of the first call's first pipe byte (record.h's layout)."""
    head = 8 + 4 * 4 + 192
    regs = 32 * 4 + 32 * 16 + 6 * 4 + 8 * 4
    at = head
    pages, inputs, callees = struct.unpack_from("<3I", data, at + 32)
    gx, pipe_bytes = struct.unpack_from("<2I", data, at + 48 + 2 * regs)
    assert gx == 1 and pipe_bytes == 11
    o = at + 48 + 2 * regs + 24
    for _ in range(pages):
        _, _, pre, post = struct.unpack_from("<4I", data, o)
        o += 16 + pre + post
    return o + inputs * 16 + callees * 8


@gnu
def test_a_changed_pipe_byte_fails_the_self_check(built):
    tmp, exe = built
    record(tmp, "drawer.soarec", f"0x{DRAWER:08X};3")
    data = bytearray((tmp / "drawer.soarec").read_bytes())
    at = first_pipe_byte(data)
    assert data[at : at + 5] == bytes([0x61, 0x41, 0x00, 0x00, 0x00])
    data[at + 10] ^= 1  # the CP load's last byte
    (tmp / "changed.soarec").write_bytes(bytes(data))
    out = compare(tmp, exe, "changed.soarec", "--original")
    assert out.returncode == 1
    assert "3 calls: 2 agree, 1 do not" in out.stdout, out.stdout
    assert "the first difference at byte 10, in CP 50 = 00001001, the command at byte 5: 00, the game's 01" in out.stdout, out.stdout


@gnu
def test_the_census_counts_a_function_s_own_bytes(built):
    tmp, _ = built
    out = run(tmp, {"SOA_PIPE_CENSUS": str(tmp / "census.tsv")})
    rows = [ln.split("\t") for ln in (tmp / "census.tsv").read_text().splitlines()]
    assert rows[0] == ["address", "pipe_bytes", "pipe_stores", "calls", "translated", "bytes_per_call"]
    got = {int(r[0], 16): r[1:5] for r in rows[1:]}
    # the drawer's 11 bytes in 5 stores a call, the lister's 5 in 2; the caller wrote none itself
    assert got == {DRAWER: ["132", "60", "12", "12"], LISTER: ["60", "24", "12", "12"]}, got
    assert "[census] 2 functions wrote 192 bytes in 84 stores to the gather pipe themselves" in out.stderr
