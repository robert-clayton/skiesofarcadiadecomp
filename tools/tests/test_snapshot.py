"""Save states (runtime/snapshot.c, docs/SNAPSHOT.md).

Three things are held here without the game:

- **Nothing is forgotten.** Every variable the runtime keeps between calls --
  each data and bss symbol of every runtime object, a function's statics
  among them -- has a line in config/snapshot_state.txt saying what a
  snapshot does with it, and a line `saved` has its name in its file's
  *_snap function. A variable added to the runtime without a decision fails
  here, and so does a line for one that is gone.
- **The file.** snapshot.c with stand-ins for the sections: a snapshot
  written by one process and read by another gives back every byte, the
  registers, a heap buffer, a host's own section, a function pointer found
  again by its module (the second process has its modules elsewhere), and
  enters main at the resume point; a damaged, cut or foreign file is refused
  by name, and so is a section whose layout or configuration moved, and an
  event whose data is on the heap.
- **The way in.** The emitter gives the function holding a resume point a
  second entry there (config/resumepoints.txt), which runs from that block
  with the registers as they are; the game library's record names it.

The proof with the game itself -- a run resumed from a snapshot against the
run it was taken from, frame for frame -- is docs/SNAPSHOT.md's. Wants gcc or
clang on an ELF system (SOA_CC), and nm for the first.
"""

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from soa import seam, toolchain  # noqa: E402
from soa.hle import load_bindings  # noqa: E402
from soa.ppc import cfg  # noqa: E402
from soa.recomp import Emitter, c_name  # noqa: E402
from test_cfg import BASE, BLR, bc, make_dol  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / "runtime"
STATE = ROOT / "config" / "snapshot_state.txt"
PROFILE = toolchain.profile(os.environ.get("SOA_CC"))
gnu = pytest.mark.skipif(
    os.name == "nt"
    or PROFILE.name not in ("gcc", "clang")
    or toolchain.compiler_path(PROFILE) is None,
    reason="needs gcc or clang (SOA_CC) on an ELF system",
)
HOWS = {"saved", "cpu", "config", "report", "host", "scratch", "refused"}


def compiler() -> str:
    return str(toolchain.compiler_path(PROFILE))


# --------------------------------------------------------------------------
# every variable accounted for
# --------------------------------------------------------------------------


def read_state(path: Path = STATE) -> dict[tuple[str, str], str]:
    out: dict[tuple[str, str], str] = {}
    for n, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        assert len(parts) == 3, f"{path.name}:{n}: not `file name how`: {raw!r}"
        file, name, how = parts
        assert how in HOWS, f"{path.name}:{n}: {how!r} is not one of {sorted(HOWS)}"
        assert (file, name) not in out, f"{path.name}:{n}: {file} {name} is listed twice"
        out[(file, name)] = how
    return out


def runtime_symbols(out: Path) -> set[tuple[str, str]]:
    """(file, name) for every data and bss symbol of every runtime object, as
    a host build compiles it; a function's static name.N read as name."""
    found: set[tuple[str, str]] = set()
    for src in sorted(RUNTIME.glob("*.c")):
        obj = out / (src.stem + ".o")
        proc = subprocess.run(
            [compiler(), "-std=c17", "-O0", "-c", "-D_GNU_SOURCE", "-DSOA_SPLIT=1",
             "-DSOA_NO_DECOMP=1", "-DSOA_HOST=1", f"-I{RUNTIME}", str(src), "-o", str(obj)],
            capture_output=True, text=True, check=False,
        )  # fmt: skip
        assert proc.returncode == 0, f"{src.name}: {proc.stderr[-2000:]}"
        nm = subprocess.run(["nm", str(obj)], capture_output=True, text=True, check=True)
        for line in nm.stdout.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1] in "bBdD":
                found.add((src.stem, parts[2].split(".", 1)[0]))
    return found


def snap_bodies(path: Path) -> str:
    """The text of every function in a file whose name ends in _snap."""
    text = path.read_text(encoding="utf-8")
    out = []
    for m in re.finditer(r"^\w[\w \*]*?\b(\w+_snap)\s*\([^;{]*\)\s*\{", text, re.M):
        depth, i = 1, m.end()
        while depth and i < len(text):
            depth += {"{": 1, "}": -1}.get(text[i], 0)
            i += 1
        out.append(text[m.start() : i])
    return "\n".join(out)


@gnu
@pytest.mark.skipif(shutil.which("nm") is None, reason="needs nm")
def test_every_variable_of_the_runtime_has_a_line_saying_what_a_snapshot_does_with_it(tmp_path):
    state = read_state()
    found = runtime_symbols(tmp_path)
    missing = sorted(found - state.keys())
    gone = sorted(state.keys() - found)
    assert not missing, (
        "variables with no line in config/snapshot_state.txt (save them in the file's *_snap, "
        f"or say why not): {missing}"
    )
    assert not gone, f"lines in config/snapshot_state.txt for variables that are gone: {gone}"
    bodies: dict[str, str] = {}
    unsaved = []
    for (file, name), how in sorted(state.items()):
        if how != "saved":
            continue
        if file not in bodies:
            bodies[file] = snap_bodies(RUNTIME / f"{file}.c")
        if not re.search(rf"\b{re.escape(name)}\b", bodies[file]):
            unsaved.append(f"{file} {name}")
    assert not unsaved, f"`saved`, but not in their file's *_snap function: {unsaved}"


def test_the_state_list_itself_is_well_formed(tmp_path):
    state = read_state()
    assert state, "config/snapshot_state.txt is empty"
    bad = tmp_path / "bad.txt"
    bad.write_text("gx  g_cp  forgotten\n", encoding="utf-8")
    with pytest.raises(AssertionError, match="not one of"):
        read_state(bad)


# --------------------------------------------------------------------------
# the file, with stand-ins for the sections
# --------------------------------------------------------------------------

# Every section snapshot.c names, standing in for the runtime's: a few of them
# hold state the driver sets and prints, the rest nothing. STUB_* in the
# environment move a layout or a configuration, as another build would.
SECTIONS = """
#include "cpu.h"
#include "snapshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t g_word;               /* gx: a word */
uint8_t* g_heap; size_t g_heap_n; /* aram: a heap buffer */
uint32_t g_cfg = 1;            /* clock: a configuration both runs share */
void (*g_fn)(void);            /* events: a function and its data */
void* g_user;
int g_user_heap;
static uint32_t g_static_data = 0xC0FFEEu;

void the_event(void) { printf("the event ran\\n"); }

void gx_snap(Snap* sn)
{
    SNAP(sn, g_word);
    if (getenv("STUB_LONGER") && snap_loading(sn)) { uint32_t more; SNAP(sn, more); }
    if (getenv("STUB_SHORTER") && !snap_loading(sn)) { uint32_t more = 0; SNAP(sn, more); }
}
void aram_snap(Snap* sn) { snap_buffer(sn, (void**)&g_heap, &g_heap_n); }
void clock_snap(Snap* sn)
{
    uint32_t cfg = getenv("STUB_CFG") ? (uint32_t)atoi(getenv("STUB_CFG")) : g_cfg;
    SNAP_CHECK(sn, cfg, "the stand-in's configuration");
}
void irq_events_snap(Snap* sn)
{
    void* fn = (void*)g_fn;
    void* user = g_user;
    int ok_fn = 1, ok_user = 1;
    snap_pointer(sn, &fn, &ok_fn);
    snap_pointer(sn, &user, &ok_user);
    if (!snap_loading(sn)) return;
    if (!ok_fn || !ok_user) {
        snap_fail(sn, "an event whose data is on the heap");
        return;
    }
    if (!snap_dry(sn)) g_fn = (void (*)(void))fn, g_user = user;
}
#define NOTHING(name) void name(Snap* sn) { (void)sn; }
NOTHING(threads_snap) NOTHING(irq_snap) NOTHING(hle_snap) NOTHING(seed_snap) NOTHING(tick_snap)
NOTHING(swap_list_snap) NOTHING(swap_snap) NOTHING(main_snap) NOTHING(gxr_snap) NOTHING(tex_snap)
NOTHING(dsp_snap) NOTHING(dvd_snap) NOTHING(ax_snap) NOTHING(si_snap) NOTHING(exi_snap)
NOTHING(record_snap) NOTHING(mod_snap)

/* what snapshot.c asks of the rest of the runtime */
volatile int g_clock_due;
void (*g_clock_due_fn)(int on);
unsigned gx_frame_count(void) { return 7; }
uint64_t hle_guest_ticks(void) { return 123456789; }
int irq_in_handler(void) { return 0; }
void gxr_flush(void) {}
int record_busy(void) { return 0; }
unsigned long long audio_wav_bytes(void) { return 0; }
int di_next_due(uint64_t* due) { *due = 0; return 0; }
int aram_next_due(uint64_t* due) { *due = 0; return 0; }

/* the resume: main entered at the top of its loop */
void dispatch(CpuState* s, uint32_t addr)
{
    printf("entered %08X at %08X; r5 %u, mem %02X; word %u; heap %zu bytes, %02X; data %08X\\n", addr,
           s->spr[1000], s->gpr[5], s->mem[0x1234], g_word, g_heap_n, g_heap_n ? g_heap[g_heap_n - 1] : 0,
           g_user ? *(uint32_t*)g_user : 0u);
    if (g_fn) g_fn();
}

static void host_hook(void* user)
{
    (void)user;
    snapshot_host_put("mine", "kept by the host", 17);
}

int main(int argc, char** argv)
{
    static CpuState s;
    s.mem = (uint8_t*)calloc(1, MEM_IMAGE_SIZE);
    if (argc > 1 && !strcmp(argv[1], "save")) {
        s.gpr[5] = 77;
        s.mem[0x1234] = 0xAB;
        g_word = 4242;
        g_heap_n = 1000;
        g_heap = (uint8_t*)malloc(g_heap_n);
        memset(g_heap, 0x5A, g_heap_n);
        g_fn = the_event;
        g_user = &g_static_data;
        if (getenv("STUB_HEAP_USER")) g_user = malloc(4), *(uint32_t*)g_user = 1;
        snapshot_set_host_hook(host_hook, NULL);
        snapshot_safe_point(&s); /* SOA_SNAPSHOT_AT=7:path */
        return 0;
    }
    {
        char why[700], mine[32];
        long n;
        if (snapshot_resume_env(why, sizeof why)) { /* as main.c's start does */
            fprintf(stderr, "[snapshot] %s\\n", why);
            return 1;
        }
        if (!snapshot_resuming()) return 0;
        n = snapshot_host_get("mine", mine, sizeof mine);
        if (snapshot_precheck(why, sizeof why)) {
            printf("refused: %s\\n", why);
            return 1;
        }
        printf("the host's section: %ld bytes, %s\\n", n, n > 0 ? mine : "-");
        return snapshot_resume_run(&s);
    }
}
"""


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    if (
        os.name == "nt"
        or PROFILE.name not in ("gcc", "clang")
        or toolchain.compiler_path(PROFILE) is None
    ):
        pytest.skip("needs gcc or clang (SOA_CC) on an ELF system")
    d = tmp_path_factory.mktemp("snapshot")
    (d / "sections.c").write_text(SECTIONS, encoding="utf-8")
    exe = d / "driver"
    proc = subprocess.run(
        [compiler(), "-std=c17", "-O1", "-D_GNU_SOURCE", f"-I{RUNTIME}", str(d / "sections.c"),
         str(RUNTIME / "snapshot.c"), str(RUNTIME / "sha1.c"), str(RUNTIME / "plat.c"),
         "-o", str(exe), "-pthread", "-ldl"],
        capture_output=True, text=True, check=False,
    )  # fmt: skip
    assert proc.returncode == 0, proc.stderr[-3000:]
    return exe


def save(driver: Path, path: Path, **env) -> subprocess.CompletedProcess:
    e = {**os.environ, "SOA_SNAPSHOT_AT": f"7:{path}", **env}
    e.pop("SOA_RESUME", None)
    return subprocess.run([str(driver), "save"], capture_output=True, text=True, env=e, check=False)


def resume(driver: Path, path: Path, **env) -> subprocess.CompletedProcess:
    e = {**os.environ, "SOA_RESUME": str(path), **env}
    e.pop("SOA_SNAPSHOT_AT", None)
    return subprocess.run([str(driver)], capture_output=True, text=True, env=e, check=False)


def test_a_snapshot_read_by_another_process_gives_everything_back(driver, tmp_path):
    snap = tmp_path / "a.snap"
    proc = save(driver, snap)
    assert proc.returncode == 0, proc.stderr
    assert "[snapshot] frame 7:" in proc.stderr and snap.exists()
    assert not (tmp_path / "a.snap.part").exists()
    run = resume(driver, snap)
    assert run.returncode == 0, run.stdout + run.stderr
    assert "the host's section: 17 bytes, kept by the host" in run.stdout
    assert (
        "entered 801DCB28 at 801DCB84; r5 77, mem AB; word 4242; heap 1000 bytes, 5A; data 00C0FFEE"
        in run.stdout
    )
    assert "the event ran" in run.stdout
    assert "resumed from" in run.stderr and "at frame 7, guest tick 123456789" in run.stderr


def damaged(src: Path, dst: Path, at: int | None = None, cut: int | None = None) -> Path:
    b = bytearray(src.read_bytes())
    if at is not None:
        b[at] ^= 0x01
    if cut is not None:
        b = b[:cut]
    dst.write_bytes(bytes(b))
    return dst


def test_a_damaged_cut_or_foreign_file_is_refused_by_name(driver, tmp_path):
    snap = tmp_path / "a.snap"
    assert save(driver, snap).returncode == 0
    size = snap.stat().st_size
    for bad, says in (
        (damaged(snap, tmp_path / "flip.snap", at=size - 100), "checksum does not match"),
        (damaged(snap, tmp_path / "cut.snap", cut=size - 10), "cut short"),
        (damaged(snap, tmp_path / "magic.snap", at=0), "is not a snapshot"),
        (damaged(snap, tmp_path / "version.snap", at=8), "of format"),
    ):
        run = resume(driver, bad)
        assert run.returncode == 1, (bad.name, run.stdout)
        assert says in run.stderr, (bad.name, run.stderr)
        assert "entered" not in run.stdout
    run = resume(driver, tmp_path / "none.snap")
    assert run.returncode == 1 and "cannot open the snapshot" in run.stderr


def test_a_section_whose_layout_or_configuration_moved_is_refused(driver, tmp_path):
    snap = tmp_path / "a.snap"
    assert save(driver, snap).returncode == 0
    for env, says in (
        ({"STUB_LONGER": "1"}, "section gx is 4 bytes, shorter than this build's"),
        ({"STUB_CFG": "2"}, "the stand-in's configuration differs from the snapshot's"),
    ):
        run = resume(driver, snap, **env)
        assert run.returncode == 1, run.stdout
        assert says in run.stderr, run.stderr
    longer = tmp_path / "longer.snap"
    assert save(driver, longer, STUB_SHORTER="1").returncode == 0
    run = resume(driver, longer)
    assert run.returncode == 1 and "section gx is 8 bytes, longer than this build's 4" in run.stderr


def test_an_event_with_data_on_the_heap_is_refused_before_the_start(driver, tmp_path):
    snap = tmp_path / "heap.snap"
    assert save(driver, snap, STUB_HEAP_USER="1").returncode == 0
    run = resume(driver, snap)
    assert run.returncode == 1
    assert "refused: an event whose data is on the heap" in run.stdout
    assert "entered" not in run.stdout


def test_only_the_whole_file_goes_under_the_name(driver, tmp_path):
    """Written beside the name and renamed: a snapshot that cannot be
    written leaves nothing under the name asked for."""
    where = tmp_path / "no" / "such" / "dir" / "a.snap"
    proc = save(driver, where)
    assert proc.returncode == 0
    assert "no snapshot to" in proc.stderr and "cannot write" in proc.stderr
    assert not where.exists()


# --------------------------------------------------------------------------
# the way in: the emitter and the game's record
# --------------------------------------------------------------------------


def addi(rd, ra, imm):
    return (14 << 26) | (rd << 21) | (ra << 16) | (imm & 0xFFFF)


def emit_with(words, resumes, swap=False):
    dol = make_dol(words)
    fns, _ = cfg.build_iterative(dol)
    em = Emitter(dol, fns, cfg.find_jump_tables(dol), swap=swap, resumes=resumes)
    return {a: em.function_c(f) for a, f in fns.items()}, em


def test_the_function_holding_a_resume_point_is_given_a_way_in_there():
    # 0: li r3,1 ; 4: (a branch target) addi r3,r3,10 ; 8: beq 4 ; 12: blr
    words = [addi(3, 0, 1), addi(3, 3, 10), bc(-4, bo=12) | (2 << 16), BLR]
    cs, em = emit_with(words, {BASE + 4: "loop.top"}, swap=True)
    c = cs[BASE]
    assert "if (s->spr[1000] != 0u) { /* a snapshot resumes here */" in c
    assert f"case 0x{BASE + 4:08X}u: s->spr[1000] = 0u; goto L_{BASE + 4:08X}; /* loop.top */" in c
    # before the swap guard: a resume is not a call, nothing is counted or delivered
    assert c.index("a snapshot resumes here") < c.index("SOA_SWAP(")
    assert em.resumed == {BASE + 4}
    plain, em2 = emit_with(words, {}, swap=True)
    assert "spr[1000]" not in plain[BASE] and not em2.resumed


@gnu
def test_a_resumed_function_runs_from_the_resume_point_with_the_registers_it_has(tmp_path):
    # 0: li r3,1 ; 4: addi r3,r3,10 ; 8: blr. From the head r3 is 11; resumed at 4 with r3 5, 15.
    words = [addi(3, 0, 1), addi(3, 3, 10), BLR]
    cs, em = emit_with(words, {BASE + 4: "second"})
    assert em.resumed == {BASE + 4}
    fn = c_name(BASE)
    (tmp_path / "fn.c").write_text('#include "cpu.h"\n' + cs[BASE], encoding="utf-8")
    (tmp_path / "main.c").write_text(
        '#include "cpu.h"\n#include <stdio.h>\n'
        f"void {fn}(CpuState* s);\n"
        "int main(void) {\n"
        "    static CpuState s;\n"
        f'    {fn}(&s); printf("head %u\\n", s.gpr[3]);\n'
        f"    s.gpr[3] = 5; s.spr[1000] = 0x{BASE + 4:08X}u; {fn}(&s);\n"
        '    printf("resumed %u spr %u\\n", s.gpr[3], s.spr[1000]);\n'
        "    return 0;\n}\n",
        encoding="utf-8",
    )
    exe = tmp_path / "t"
    proc = subprocess.run(
        [compiler(), "-std=c17", "-O1", f"-I{RUNTIME}", str(tmp_path / "fn.c"), str(tmp_path / "main.c"),
         "-o", str(exe), "-lm"],
        capture_output=True, text=True, check=False,
    )  # fmt: skip
    assert proc.returncode == 0, proc.stderr[-3000:]
    run = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
    assert run.stdout.splitlines() == ["head 11", "resumed 15 spr 0"]


def test_the_game_record_names_its_resume_points():
    rec = seam.record(True, "b" * 64, "d" * 40, "gcc", resume=[0x801DCB84])
    assert rec.endswith(" resume=801DCB84")
    assert "resume=" not in seam.record(True, "b" * 64, "d" * 40, "gcc")


def test_the_runtime_and_the_list_agree_on_the_resume_point():
    """snapshot.c resumes at one address in one function; config/resumepoints.txt
    must give it a way in, and nothing else."""
    points = load_bindings(ROOT / "config" / "resumepoints.txt")
    text = (RUNTIME / "snapshot.c").read_text(encoding="utf-8")
    pc = int(re.search(r"#define RESUME_PC (0x[0-9A-Fa-f]+)u", text).group(1), 16)
    assert set(points) == {pc}
    assert f"resume={pc:08X}" in text
