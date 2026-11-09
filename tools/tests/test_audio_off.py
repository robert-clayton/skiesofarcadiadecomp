"""SOA_AUDIO=off in runtime/dsp.c and runtime/aram.c (soa-ue5's U3.3): the audio's device models off.

dsp.c and aram.c built on their own with gcc, the mixer, the output, the disc and the clock stood in for by counters. The
same driver runs twice, without the switch and with it: the DSP's mailbox written (a microcode boot and a command list),
AI's DMA started and polled past its end, an ARAM DMA from main memory started and polled, the DSP's control register,
AI's control and AI's volume read and written through audio_off_mmio (irq.c's first question for every register), and
the census's preparation.

Checked: without the switch audio_off_mmio takes nothing, the microcode boots and mixes the list, the AI block is played
and raises its interrupt, ARAM holds the bytes and its interrupt comes, the census reads the disc, and the report is the
model's. With it, every register access of AI and the DSP interface is taken, counted, read back 0 and written nowhere:
no mail, no mix, no block played, no ARAM (never allocated), no interrupt, no deadline, the census reads nothing, and the
report gives the count. No disc and no game data.
"""

import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / "runtime"

DRIVER = r"""
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t g_now;
unsigned g_mixed, g_played, g_fst_reads;
uint64_t guest_time_device(CpuState* s) { (void)s; return g_now; }
void ax_command_list(CpuState* s, uint32_t addr) { (void)s, (void)addr; g_mixed++; }
void ax_report(void) {}
void audio_push_block(const uint8_t* be_rl, unsigned bytes, unsigned rate) { (void)be_rl, (void)bytes, (void)rate; g_played++; }
void audio_report(void) {}
const uint8_t* disc_fst(size_t* n) { g_fst_reads++; *n = 0; return NULL; }
uint32_t disc_peek(uint64_t offset, uint8_t* dst, uint32_t length) { (void)offset, (void)dst, (void)length; return 0; }

int dsp_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out);
int dsp_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v);
int aram_read(CpuState* s, uint32_t ea, unsigned size, uint64_t* out);
int aram_write(CpuState* s, uint32_t ea, unsigned size, uint64_t v);
void dsp_poll(CpuState* s);
void aram_poll(CpuState* s);
int dsp_next_due(uint64_t* due);
int aram_next_due(uint64_t* due);
int dsp_irq_pending(void);
int ai_dma_irq_pending(void);
int aram_irq_pending(void);
uint8_t* aram_memory(void);
void aram_census_prepare(void);
void aram_report(void);
void dsp_report(void);
int audio_is_off(void);
int audio_off_mmio(CpuState* s, uint32_t ea, unsigned size, int write, uint64_t v, uint64_t* out);

/* a register access as irq.c's device_read and device_write make it: the switch's question first, then the models */
static uint64_t rd(CpuState* s, uint32_t ea, unsigned size)
{
    uint64_t v = 0xDEAD;
    if (audio_off_mmio(s, ea, size, 0, 0, &v)) return v;
    if (dsp_read(s, ea, size, &v) || aram_read(s, ea, size, &v)) return v;
    return 0xBAD;
}
static unsigned taken;
static void wr(CpuState* s, uint32_t ea, unsigned size, uint64_t v)
{
    if (audio_off_mmio(s, ea, size, 1, v, NULL)) { taken++; return; }
    if (!dsp_write(s, ea, size, v)) aram_write(s, ea, size, v);
}
static void mail(CpuState* s, uint32_t m) { wr(s, 0xCC005000u, 2, m >> 16); wr(s, 0xCC005002u, 2, m & 0xFFFFu); }

int main(void)
{
    static CpuState s;
    uint64_t due = 0, csr, ai, out;
    int i, irq = 0;
    s.mem = (uint8_t*)calloc(1, MEM_IMAGE_SIZE);
    if (!s.mem) return 2;
    for (i = 0; i < 32; i++) s.mem[0x1000 + i] = (uint8_t)(0xA0 + i);
    aram_census_prepare();
    printf("off %d census reads %u\n", audio_is_off(), g_fst_reads);
    mail(&s, 0x80F3D001u); mail(&s, 0x00000010u);  /* the ROM's boot: the microcode starts and mails DSP_INIT */
    mail(&s, 0xBABE0040u); mail(&s, 0x80002000u);  /* a command list */
    out = rd(&s, 0xCC005004u, 2);
    wr(&s, 0xCC005030u, 2, 0x0000); wr(&s, 0xCC005032u, 2, 0x2000);  /* AI DMA: 640 bytes from 0x2000 */
    wr(&s, 0xCC00500Au, 2, 0x0010 | 0x0040 | 0x0100);                  /* the DSP's control: every mask */
    wr(&s, 0xCC005036u, 2, 0x8000 | 20);
    wr(&s, 0xCC005020u, 4, 0x00001000u); wr(&s, 0xCC005024u, 4, 0x00000400u);  /* ARAM DMA: 32 bytes to ARAM 0x400 */
    wr(&s, 0xCC005028u, 4, 32);
    wr(&s, 0xCC006C00u, 4, 0x4);                                       /* AI's control and volume */
    wr(&s, 0xCC006C04u, 4, 0xFF);
    csr = rd(&s, 0xCC00500Au, 2);
    ai = rd(&s, 0xCC006C00u, 4);
    g_now = 40500000ull;  /* a second later: AI's block and ARAM's copy are long done */
    dsp_poll(&s);
    aram_poll(&s);
    irq = dsp_irq_pending() | ai_dma_irq_pending() << 1 | aram_irq_pending() << 2;
    printf("mixed %u played %u irq %d due %d%d mailbox %llx taken %u\n", g_mixed, g_played, irq, dsp_next_due(&due),
           aram_next_due(&due), (unsigned long long)out, taken);
    printf("csr %llx ai %llx aram %s\n", (unsigned long long)(csr & 0x0170u), (unsigned long long)ai,
           aram_memory() ? (memcmp(aram_memory() + 0x400, s.mem + 0x1000, 32) ? "other" : "copied") : "none");
    fflush(stdout);
    aram_report();
    dsp_report();
    return 0;
}
"""

gnu = pytest.mark.skipif(
    shutil.which("gcc") is None or os.name == "nt", reason="wants gcc on an ELF system"
)


@pytest.fixture(scope="module")
def exe(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("audio_off")
    (tmp / "driver.c").write_text(DRIVER, encoding="utf-8")
    out = tmp / "audiooff"
    proc = subprocess.run(
        ["gcc", "-std=c17", "-O1", "-D_GNU_SOURCE", "-fno-strict-aliasing", "-fwrapv", f"-I{RUNTIME}",
         str(RUNTIME / "dsp.c"), str(RUNTIME / "aram.c"), str(tmp / "driver.c"), "-lm", "-o", str(out)],
        capture_output=True, text=True, timeout=120, check=False,
    )  # fmt: skip
    assert proc.returncode == 0, proc.stdout + proc.stderr
    return out


def run(exe, **env):
    clean = {k: v for k, v in os.environ.items() if not k.startswith("SOA_")}
    r = subprocess.run(
        [str(exe)], capture_output=True, text=True, timeout=60, env={**clean, **env}, check=False
    )
    assert r.returncode == 0, r.stdout + r.stderr
    return r.stdout.splitlines(), r.stderr


@gnu
def test_without_the_switch_the_models_are_as_before(exe):
    out, err = run(exe)
    assert out[0] == "off 0 census reads 1"
    # the microcode booted and mixed one list; the AI block played; DSP, AI and ARAM interrupts all pending; the
    # AI DMA's next deadline (ARAM's is spent); DSP_INIT's mail waiting (its high half, whose top bit is the pending bit)
    assert out[1] == "mixed 1 played 1 irq 7 due 10 mailbox dcd1 taken 0", err
    # (AI's registers are irq.c's model, not built here: nothing answers them without the switch)
    assert out[2] == "csr 150 ai bad aram copied", err
    assert "[aram] 1 DMAs, 32 bytes" in err
    assert (
        "4 mails in, 3 out, 1 command lists; 1 AI DMA blocks played" in err
        and "[audio] off" not in err
    )


@gnu
def test_with_the_switch_nothing_reaches_the_models(exe):
    out, err = run(exe, SOA_AUDIO="off")
    assert out[0] == "off 1 census reads 0"
    assert out[1] == "mixed 0 played 0 irq 0 due 00 mailbox 0 taken 17", err
    assert out[2] == "csr 0 ai 0 aram none", err
    assert "[audio] off (SOA_AUDIO=off)" in err
    assert "[audio] off: 3 register reads, 17 register writes" in err
    assert "[aram]" not in err and "[dsp]" not in err
