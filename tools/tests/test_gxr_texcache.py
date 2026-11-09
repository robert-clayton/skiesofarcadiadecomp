"""Tests for the texture cache's epochs and index (PLAN-60FPS-MODS H12).

A texture's source bytes are hashed at most once an epoch, and the epoch moves
on the game's texture-cache invalidate (BP 0x66), on every EFB copy and so at
every frame end. The cache holds 1,024 decodes and finds them through an
open-addressed index whose deletion closes gaps (Knuth's algorithm R) -- the
kind of code that works on every capture and fails on the one collision
pattern no capture has. So the driver below includes ``gxr_tev.c`` itself, to
reach the cache's statics, and checks the index's invariants after thousands
of lookups and evictions, then each rule the epochs live by.

Each case runs in its own process, so every one starts from an empty cache.
No disc and no game data: texture memory is a zeroed image the driver writes.
"""

import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from soa import toolchain  # noqa: E402

RUNTIME = ROOT / "runtime"
SOURCES = ["gx.c", "gx_frame.c", "gxr.c", "png.c"]  # gxr_tev.c comes in through the driver

DRIVER = r"""
#define _CRT_SECURE_NO_WARNINGS
#include "gxr_tev.c" /* the cache's statics, for the checks below */

uint32_t mmio_read32(CpuState* s, uint32_t ea) { (void)s; (void)ea; return 0; }
void hle_report(void) {}

static CpuState S;
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* An 8x4 I8 texture: 32 bytes, one texel a byte, decoded as (v, v, v, v). */
#define TEXA(k) (0x00100000u + (uint32_t)(k) * 32u)
static const TexEntry* i8(uint32_t addr) { return texture(addr, 1, 8, 4, 0, 0, 1); }
static void fill(uint32_t addr, uint8_t v) { memset(S.mem + addr, v, 32); }

/* Every keyed entry is found by its key; every occupied slot is reachable
 * from its key's home without crossing an empty one; one slot per entry. */
static void check_index(const char* when)
{
    int n, used = 0;
    uint32_t i, j;
    for (n = 0; n < g_cache_used; n++)
        CHECK(tex_find(&g_cache[n]) == &g_cache[n], "%s: entry %d is not found by its own key", when, n);
    for (i = 0; i < TEX_INDEX; i++) {
        if (!g_index[i]) continue;
        used++;
        for (j = tex_home(&g_cache[g_index[i] - 1]); j != i; j = (j + 1) & (TEX_INDEX - 1))
            if (!g_index[j]) { CHECK(0, "%s: slot %u sits past an empty slot %u from its home", when, i, j); break; }
    }
    CHECK(used == g_cache_used, "%s: %d index slots for %d entries", when, used, g_cache_used);
}

/* Thousands of lookups over three times the cache's keys, in an order that
 * evicts constantly: the index must stay whole, and every lookup must return
 * the texture asked for. */
static void t_index(void)
{
    uint32_t r = 12345, k, n;
    for (n = 0; n < 30000; n++) {
        const TexEntry* e;
        r = r * 1103515245u + 12345u;
        k = (r >> 8) % 3072;
        e = i8(TEXA(k));
        CHECK(e && e->addr == TEXA(k) && e->rgba, "lookup %u of key %u returned %08X", n, k, e ? e->addr : 0);
        if (n % 5000 == 4999) { check_index("during churn"); tex_graveyard_empty(); }
    }
    check_index("after churn");
    CHECK(g_cache_used == TEX_CACHE && g_evicted > 0, "the churn did not fill and evict: %d used, %u evicted", g_cache_used, g_evicted);
}

/* Least recently used goes first, and nothing else does. */
static void t_lru(void)
{
    int k;
    for (k = 0; k < TEX_CACHE; k++) i8(TEXA(k));
    i8(TEXA(0)); /* 0 is now the most recent; 1 the least */
    i8(TEXA(TEX_CACHE));
    CHECK(g_evicted == 1, "%u evicted", g_evicted);
    {
        TexEntry key = g_cache[0];
        key.addr = TEXA(1);
        CHECK(tex_find(&key) == NULL, "the least recently used texture is still cached");
        key.addr = TEXA(0);
        CHECK(tex_find(&key) != NULL, "a texture used just now was evicted");
    }
    check_index("after one eviction");
}

/* Inside one epoch a texture is hashed once, so a rewrite with nothing in
 * between is not seen -- the rule the game's own invalidate makes safe. After
 * the epoch moves, it is. */
static void t_epoch(void)
{
    const TexEntry* e;
    unsigned long long h0;
    fill(TEXA(0), 10);
    e = i8(TEXA(0));
    CHECK(e->level[0][0] == 10, "first decode %u", e->level[0][0]);
    h0 = g_hashes;
    fill(TEXA(0), 20);
    e = i8(TEXA(0));
    CHECK(g_hashes == h0 && e->level[0][0] == 10, "hashed again inside the epoch (%llu, texel %u)", g_hashes - h0, e->level[0][0]);
    tex_epoch_advance();
    e = i8(TEXA(0));
    CHECK(g_hashes == h0 + 1 && e->level[0][0] == 20, "not hashed again after the epoch moved (texel %u)", e->level[0][0]);
    e = i8(TEXA(0));
    CHECK(g_hashes == h0 + 1, "hashed twice in one epoch");
    CHECK(g_decodes == 2, "%llu decodes, not 2", g_decodes);
}

/* SOA_TEXVERIFY hashes every lookup: the same rewrite is seen, decoded and
 * counted as one the epochs would have missed. */
static void t_verify(void)
{
    const TexEntry* e;
    _putenv("SOA_TEXVERIFY=1");
    fill(TEXA(0), 10);
    i8(TEXA(0));
    fill(TEXA(0), 20);
    e = i8(TEXA(0));
    CHECK(e->level[0][0] == 20 && g_missed == 1, "verify: texel %u, %llu missed", e->level[0][0], g_missed);
    tex_epoch_advance();
    fill(TEXA(0), 30);
    e = i8(TEXA(0));
    CHECK(e->level[0][0] == 30 && g_missed == 1, "a rewrite across an epoch counted as missed (%llu)", g_missed);
}

/* The BP writes that move the epoch: the invalidate and every copy -- and
 * nothing else. */
static void t_bp(void)
{
    uint64_t e0 = g_tex_epoch;
    gxr_bp_written(&S, 0x66, 0x001000);
    CHECK(g_tex_epoch == e0 + 1, "BP 0x66 did not move the epoch");
    gxr_bp_written(&S, 0x52, 0x000043); /* a copy to texture; rendering is off, so no copy runs */
    CHECK(g_tex_epoch == e0 + 2, "a copy to texture did not move the epoch");
    gxr_bp_written(&S, 0x52, 0x004003); /* the screen copy */
    CHECK(g_tex_epoch == e0 + 3, "the screen copy did not move the epoch");
    gxr_bp_written(&S, 0x45, 0x000000);
    gxr_bp_written(&S, 0x28, 0x000040);
    CHECK(g_tex_epoch == e0 + 3, "an unrelated BP write moved the epoch");
}

/* A dropped decode keeps its key: the next lookup decodes again into the
 * same entry, the index untouched. */
static void t_dropped(void)
{
    const TexEntry *a, *b;
    int used;
    a = texture(TEXA(0), 8, 8, 8, 0, 0, 1); /* C4, palette at TMEM 0 */
    used = g_cache_used;
    tex_invalidate_all();
    CHECK(a->rgba == NULL, "tex_invalidate_all did not drop the C4 decode");
    b = texture(TEXA(0), 8, 8, 8, 0, 0, 1);
    CHECK(b == a && b->rgba && g_cache_used == used, "a dropped entry was not reused in place");
    CHECK(g_dec_dropped == 1 && g_dec_same == 1, "counted %llu dropped, %llu unchanged", g_dec_dropped, g_dec_same);
    check_index("after a drop");
}

/* A TLUT load sends the palettised decodes it overlaps to be hashed again,
 * and the hash covers the palette: the same palette loaded again keeps the
 * decode, and a different one in the same place is decoded again -- all
 * inside one epoch, where nothing else would notice. */
static void t_palette(void)
{
    const TexEntry* e;
    const uint8_t* rgba;
    unsigned long long d0;
    uint8_t* pal = S.mem + 0x200000; /* IA8 entry 0, which every index of a zeroed C4 texture reads */
    pal[0] = 0xFF;
    pal[1] = 0x40;
    tmem_load_tlut(&S, 0x80200000u, 0, 32);
    e = texture(TEXA(0), 8, 8, 8, 0, 0, 1);
    CHECK(e->rgba && e->level[0][0] == 0x40, "first decode %02X", e->rgba ? e->level[0][0] : 0);
    rgba = e->rgba;
    d0 = g_decodes;
    tmem_load_tlut(&S, 0x80200000u, 0, 32);
    e = texture(TEXA(0), 8, 8, 8, 0, 0, 1);
    CHECK(g_decodes == d0 && e->rgba == rgba, "the same palette loaded again was decoded again (%llu)", g_decodes - d0);
    pal[1] = 0x90;
    tmem_load_tlut(&S, 0x80200000u, 0, 32);
    e = texture(TEXA(0), 8, 8, 8, 0, 0, 1);
    CHECK(g_decodes == d0 + 1 && e->level[0][0] == 0x90, "a changed palette was not decoded again (texel %02X)", e->level[0][0]);
    check_index("after palette loads");
}

int main(int argc, char** argv)
{
    S.mem = (uint8_t*)calloc(1, MEM_IMAGE_SIZE);
    if (!S.mem || argc < 2) return 2;
    tex_set_memory(&S);
    if (!strcmp(argv[1], "index")) t_index();
    else if (!strcmp(argv[1], "lru")) t_lru();
    else if (!strcmp(argv[1], "epoch")) t_epoch();
    else if (!strcmp(argv[1], "verify")) t_verify();
    else if (!strcmp(argv[1], "bp")) t_bp();
    else if (!strcmp(argv[1], "dropped")) t_dropped();
    else if (!strcmp(argv[1], "palette")) t_palette();
    else return 2;
    printf("[texcache] %s: %d failure(s)\n", argv[1], fails);
    fflush(stdout);
    return fails ? 1 : 0;
}
"""

CASES = ("index", "lru", "epoch", "verify", "bp", "dropped", "palette")


@pytest.fixture(scope="module")
def exe(tmp_path_factory):
    if toolchain.cl_path() is None:
        pytest.skip("no MSVC")
    out = tmp_path_factory.mktemp("texcache")
    (out / "driver.c").write_text(DRIVER, encoding="utf-8")
    exe = out / "texcache.exe"
    proc = toolchain.cl(
        [
            *toolchain.CFLAGS,
            "/I",
            str(RUNTIME),
            *[str(RUNTIME / name) for name in SOURCES],
            str(out / "driver.c"),
            "/Fo" + str(out) + os.sep,
            "/Fe" + str(exe),
        ],
        cwd=out,
    )
    assert proc.returncode == 0, (proc.stdout or "") + (proc.stderr or "")
    return exe


@pytest.mark.skipif(
    toolchain.cl_path() is None, reason="no MSVC: the renderer cannot be built here"
)
@pytest.mark.parametrize("case", CASES)
def test_texture_cache(exe, case):
    env = {k: v for k, v in os.environ.items() if not k.startswith("SOA_")}
    run = subprocess.run(
        [str(exe), case], capture_output=True, text=True, env=env, timeout=300, check=False
    )
    text = run.stdout + run.stderr
    assert f"[texcache] {case}: 0 failure(s)" in text, text
    assert run.returncode == 0, text
