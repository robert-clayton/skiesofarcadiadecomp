/*
 * Save states: snapshot.h says what, docs/SNAPSHOT.md why and how it is
 * checked.
 *
 * A snapshot is one file:
 *
 *     "SOASNAP\0", version, header bytes, payload bytes, the payload's SHA-1,
 *     the frame, the guest ticks, the resume address   (the header, 64 bytes)
 *     section, section, ...                            (the payload)
 *
 * and a section is its name (16 bytes, NUL-padded), its size (8 bytes) and
 * its bytes: one per file of the runtime that has state (cpu, mem, irq, gx,
 * ...), in the order SECTIONS lists them, plus a host's own (host:<name>).
 * Every number is the writing machine's byte order and every struct its
 * compiler's layout: a snapshot is for the build that wrote it, and the
 * "build" section, the sizes and the checksum refuse anything else.
 *
 * Saving runs on the guest's thread at the safe point (tick.c), the frame's
 * picture finished first (gxr_flush). Loading runs on the guest's thread too,
 * in place of the entry point (main.c's run_guest): every section into its
 * file's statics, then main entered at the resume address with the registers
 * as they were (config/resumepoints.txt).
 */
#define _CRT_SECURE_NO_WARNINGS
#include "snapshot.h"
#include "cpu.h"
#include "plat.h"
#include "sha1.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SNAP_MAGIC "SOASNAP"
#define SNAP_VERSION 1u
#define SNAP_HEADER 64u
#define SNAP_NAME 16u
/* The resume point (config/resumepoints.txt) and the function that holds it:
 * the main loop's top, in main. The register the recompiler's entry reads is
 * emit.py's RESUME_SPR. */
#define RESUME_PC 0x801DCB84u
#define RESUME_FN 0x801DCB28u
#define RESUME_SPR 1000

typedef struct {
    Snap pub;
    char why[512];
    /* saving: the payload, grown as sections are added */
    uint8_t* buf;
    size_t len, cap;
    /* loading: the section being read */
    const uint8_t* in;
    size_t in_len, in_at;
    const char* section;
} SnapImpl;
#define IMPL(sn) ((SnapImpl*)(sn))

static void op_fail(Snap* sn, const char* why)
{
    if (sn->failed) return; /* the first reason is the one that matters */
    sn->failed = 1;
    snprintf(IMPL(sn)->why, sizeof IMPL(sn)->why, "%s", why);
}

static void put(SnapImpl* m, const void* p, size_t n)
{
    if (m->pub.failed) return;
    if (m->len + n > m->cap) {
        size_t cap = m->cap ? m->cap : (64u << 20);
        uint8_t* grown;
        while (cap < m->len + n) cap *= 2;
        grown = (uint8_t*)realloc(m->buf, cap);
        if (!grown) {
            snap_fail(&m->pub, "out of memory for a snapshot of %zu bytes", cap);
            return;
        }
        m->buf = grown;
        m->cap = cap;
    }
    memcpy(m->buf + m->len, p, n);
    m->len += n;
}

static int take(SnapImpl* m, void* p, size_t n)
{
    if (m->pub.failed) return 0;
    if (n > m->in_len - m->in_at) {
        snap_fail(&m->pub, "section %s is %zu bytes, shorter than this build's (it was written by another build)",
                  m->section, m->in_len);
        return 0;
    }
    if (p) memcpy(p, m->in + m->in_at, n);
    m->in_at += n;
    return 1;
}

/* Dry or not, loading fills what it is given: a section's function checked
 * dry (snapshot_precheck) reads into its own locals, never its statics. */
static void op_bytes(Snap* sn, void* p, size_t n)
{
    if (sn->loading) take(IMPL(sn), p, n);
    else put(IMPL(sn), p, n);
}

static void op_check(Snap* sn, const void* p, size_t n, const char* what)
{
    SnapImpl* m = IMPL(sn);
    if (!sn->loading) {
        put(m, p, n);
        return;
    }
    if (sn->failed) return;
    if (n > m->in_len - m->in_at) {
        take(m, NULL, n);
        return;
    }
    if (memcmp(m->in + m->in_at, p, n) != 0) {
        snap_fail(sn, "%s differs from the snapshot's (section %s): a resume continues the run it was taken from", what,
                  m->section);
        return;
    }
    m->in_at += n;
}

static void op_buffer(Snap* sn, void** p, size_t* n)
{
    SnapImpl* m = IMPL(sn);
    uint64_t size = *n;
    op_bytes(sn, &size, sizeof size);
    if (sn->failed) return;
    if (sn->loading) {
        if (size > m->in_len - m->in_at) {
            take(m, NULL, (size_t)size);
            return;
        }
        if (sn->dry) {
            take(m, NULL, (size_t)size);
            return;
        }
        if ((size_t)size != *n || (!*p && size)) {
            void* q = size ? malloc((size_t)size) : NULL;
            if (size && !q) {
                snap_fail(sn, "out of memory for %llu bytes of section %s", (unsigned long long)size, m->section);
                return;
            }
            free(*p);
            *p = q;
            *n = (size_t)size;
        }
    }
    if (size) op_bytes(sn, *p, (size_t)size);
}

/* A pointer as the module it lies in and an offset (snapshot.h). */
typedef struct {
    uint32_t kind; /* 0 NULL, 1 in a module, 2 in none */
    uint32_t pad;
    uint64_t off;
    char module[64];
} SnapPtr;

static void op_pointer(Snap* sn, void** p, int* ok)
{
    SnapPtr r;
    memset(&r, 0, sizeof r);
    if (!sn->loading) {
        uintptr_t off = 0;
        if (!*p) r.kind = 0;
        else if (plat_addr_module(*p, r.module, sizeof r.module, &off)) r.kind = 1, r.off = off;
        else r.kind = 2, r.off = (uint64_t)(uintptr_t)*p;
    }
    op_bytes(sn, &r, sizeof r);
    if (ok) *ok = 1;
    if (!sn->loading || sn->failed) return;
    r.module[sizeof r.module - 1] = '\0';
    if (r.kind == 0) {
        *p = NULL;
    } else if (r.kind == 1) {
        *p = plat_module_addr(r.module, (uintptr_t)r.off);
        if (ok) *ok = *p != NULL;
    } else {
        *p = NULL;
        if (ok) *ok = 0;
    }
}

static const SnapOps OPS = {op_bytes, op_buffer, op_check, op_fail, op_pointer};

static void impl_init(SnapImpl* m, int loading, int dry)
{
    memset(m, 0, sizeof *m);
    m->pub.ops = &OPS;
    m->pub.loading = loading;
    m->pub.dry = dry;
}

/* ---- the sections ------------------------------------------------------- */

void threads_snap(Snap* sn);
void irq_events_snap(Snap* sn);
void irq_snap(Snap* sn);
void clock_snap(Snap* sn);
void hle_snap(Snap* sn);
void seed_snap(Snap* sn);
void tick_snap(Snap* sn);
void swap_list_snap(Snap* sn);
void swap_snap(Snap* sn);
void main_snap(Snap* sn);
void gx_snap(Snap* sn);
void gxr_snap(Snap* sn);
void tex_snap(Snap* sn);
void dsp_snap(Snap* sn);
void ax_snap(Snap* sn);
void dvd_snap(Snap* sn);
void aram_snap(Snap* sn);
void si_snap(Snap* sn);
void exi_snap(Snap* sn);
void record_snap(Snap* sn);
void mod_snap(Snap* sn);
#ifdef SOA_HOST
void host_snap(Snap* sn);
#endif

static CpuState* g_s; /* the guest's, for the cpu and mem sections */

static void build_snap(Snap* sn);
static void cpu_snap(Snap* sn);
static void mem_snap(Snap* sn);

typedef struct {
    const char* name;
    void (*fn)(Snap* sn);
} Section;

/* In this order on both sides. "swaps" and "events" first, since a host's
 * start checks them before anything is loaded (snapshot_precheck). */
static const Section SECTIONS[] = {
    {"build", build_snap},   {"swaps", swap_list_snap}, {"events", irq_events_snap}, {"cpu", cpu_snap},
    {"mem", mem_snap},       {"threads", threads_snap}, {"clock", clock_snap},       {"hle", hle_snap},
    {"seed", seed_snap},     {"irq", irq_snap},         {"tick", tick_snap},         {"swap", swap_snap},
    {"main", main_snap},     {"gx", gx_snap},           {"gxr", gxr_snap},           {"tex", tex_snap},
    {"dvd", dvd_snap},       {"dsp", dsp_snap},       {"ax", ax_snap},       {"aram", aram_snap},       {"si", si_snap},
    {"exi", exi_snap},       {"record", record_snap},   {"mod", mod_snap},
#ifdef SOA_HOST
    {"host", host_snap},
#endif
};
#define NSECTIONS (sizeof SECTIONS / sizeof SECTIONS[0])

unsigned gx_frame_count(void);
uint64_t hle_guest_ticks(void);
int irq_in_handler(void);
void gxr_flush(void);
void dispatch(CpuState* s, uint32_t addr);

/* The game library's build record (soa_game.h): what names the executable,
 * the baked inputs and the resume points. A single-file build has none. */
#ifdef SOA_SPLIT
#include "soa_game.h"
static const char* game_record(void)
{
    return soa_game_table && soa_game_table->record ? soa_game_table->record : "";
}
#else
static const char* game_record(void)
{
    return "";
}
#endif

/* What the snapshot is of: the same game library, the same layouts. */
static void build_snap(Snap* sn)
{
    char record[512];
    uint32_t sizes[4] = {(uint32_t)sizeof(CpuState), MEM1_SIZE, RESUME_PC, RESUME_FN};
    memset(record, 0, sizeof record);
    snprintf(record, sizeof record, "%s", game_record());
    SNAP_CHECK(sn, record, "the game library's build record");
    SNAP_CHECK(sn, sizes, "the CPU state's layout, MEM1's size or the resume point");
}

/* Every register (CpuState up to its two pointers, which are this process's). */
static void cpu_snap(Snap* sn)
{
    snap_bytes(sn, g_s, offsetof(CpuState, mem));
}

static void mem_snap(Snap* sn)
{
    snap_bytes(sn, g_s->mem, MEM1_SIZE);
}

/* ---- the host's own sections ------------------------------------------------ */

typedef struct {
    char name[SNAP_NAME];
    void* data;
    size_t bytes;
} HostSection;
#define MAX_HOST_SECTIONS 32
static HostSection g_host_put[MAX_HOST_SECTIONS];
static unsigned g_nhost_put;
static PlatLock g_host_lock;
static void (*g_host_hook)(void* user);
static void* g_host_hook_user;

void snapshot_set_host_hook(void (*fn)(void* user), void* user)
{
    plat_lock(&g_host_lock);
    g_host_hook = fn;
    g_host_hook_user = user;
    plat_unlock(&g_host_lock);
}

int snapshot_host_put(const char* name, const void* data, size_t bytes)
{
    unsigned k;
    void* copy;
    if (!name || !*name || strlen(name) + 5 >= SNAP_NAME || (bytes && !data)) return 0;
    copy = malloc(bytes ? bytes : 1);
    if (!copy) return 0;
    if (bytes) memcpy(copy, data, bytes);
    plat_lock(&g_host_lock);
    for (k = 0; k < g_nhost_put && strcmp(g_host_put[k].name + 5, name); k++) {}
    if (k == MAX_HOST_SECTIONS) {
        plat_unlock(&g_host_lock);
        free(copy);
        return 0;
    }
    if (k == g_nhost_put) {
        memset(g_host_put[k].name, 0, SNAP_NAME);
        snprintf(g_host_put[k].name, SNAP_NAME, "host:%s", name);
        g_nhost_put++;
    } else {
        free(g_host_put[k].data);
    }
    g_host_put[k].data = copy;
    g_host_put[k].bytes = bytes;
    plat_unlock(&g_host_lock);
    return 1;
}

/* ---- writing ------------------------------------------------------------------ */

/* A section's head: its name, and its size, filled in when it ends. The
 * offset of the size, for section_end. */
static size_t section_begin(SnapImpl* m, const char* name)
{
    char tag[SNAP_NAME];
    uint64_t zero = 0;
    memset(tag, 0, sizeof tag);
    snprintf(tag, sizeof tag, "%s", name);
    put(m, tag, sizeof tag);
    put(m, &zero, sizeof zero);
    return m->len;
}

static void section_end(SnapImpl* m, size_t at)
{
    uint64_t size = m->len - at;
    if (!m->pub.failed) memcpy(m->buf + at - sizeof size, &size, sizeof size);
}

static int write_file(const char* path, const SnapImpl* m, unsigned frame, uint64_t ticks, char* why, size_t cap)
{
    uint8_t head[SNAP_HEADER];
    uint32_t v;
    uint64_t q;
    FILE* f;
    char tmp[1100];
    memset(head, 0, sizeof head);
    memcpy(head, SNAP_MAGIC, 8);
    v = SNAP_VERSION, memcpy(head + 8, &v, 4);
    v = SNAP_HEADER, memcpy(head + 12, &v, 4);
    q = m->len, memcpy(head + 16, &q, 8);
    sha1(m->buf, m->len, head + 24);
    v = frame, memcpy(head + 44, &v, 4);
    memcpy(head + 48, &ticks, 8);
    v = RESUME_PC, memcpy(head + 56, &v, 4);
    /* whole or not at all: written beside the name, then renamed */
    snprintf(tmp, sizeof tmp, "%s.part", path);
    f = fopen(tmp, "wb");
    if (!f) {
        snprintf(why, cap, "cannot write %s", tmp);
        return 0;
    }
    if (fwrite(head, 1, sizeof head, f) != sizeof head || fwrite(m->buf, 1, m->len, f) != m->len) {
        fclose(f);
        remove(tmp);
        snprintf(why, cap, "cannot write %s (disk full?)", tmp);
        return 0;
    }
    if (fclose(f) != 0) {
        remove(tmp);
        snprintf(why, cap, "cannot finish %s", tmp);
        return 0;
    }
    remove(path);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        snprintf(why, cap, "cannot rename %s to %s", tmp, path);
        return 0;
    }
    return 1;
}

int record_busy(void);
unsigned long long audio_wav_bytes(void);
int di_next_due(uint64_t* due);
int aram_next_due(uint64_t* due);

/* Save everything into a file: 1, or 0 and why. */
static int save(CpuState* s, const char* path, char* why, size_t cap)
{
    SnapImpl m;
    unsigned k, frame = gx_frame_count();
    uint64_t ticks = hle_guest_ticks(), t0 = plat_mono_ns();
    int ok;
    impl_init(&m, 0, 0);
    if (irq_in_handler()) {
        snprintf(why, cap, "the guest is inside an interrupt handler or an event");
        return 0;
    }
    if (record_busy()) {
        snprintf(why, cap, "a recorded call is under way (SOA_RECORD)");
        return 0;
    }
    g_s = s;
    gxr_flush(); /* the frame's picture whole, every copy it made in memory */
    {
        void (*hook)(void*);
        void* user;
        plat_lock(&g_host_lock);
        hook = g_host_hook;
        user = g_host_hook_user;
        plat_unlock(&g_host_lock);
        if (hook) hook(user); /* the host's own state, at the same point: soa_host_snapshot_put */
    }
    for (k = 0; k < NSECTIONS && !m.pub.failed; k++) {
        size_t at = section_begin(&m, SECTIONS[k].name);
        m.section = SECTIONS[k].name;
        SECTIONS[k].fn(&m.pub);
        section_end(&m, at);
    }
    plat_lock(&g_host_lock);
    for (k = 0; k < g_nhost_put && !m.pub.failed; k++) {
        size_t at = section_begin(&m, g_host_put[k].name);
        put(&m, g_host_put[k].data, g_host_put[k].bytes);
        section_end(&m, at);
    }
    for (k = 0; k < g_nhost_put; k++) free(g_host_put[k].data);
    g_nhost_put = 0;
    plat_unlock(&g_host_lock);
    if (m.pub.failed) {
        snprintf(why, cap, "%s", m.why);
        free(m.buf);
        return 0;
    }
    ok = write_file(path, &m, frame, ticks, why, cap);
    if (ok) {
        uint64_t due;
        char flight[160];
        int n = 0;
        flight[0] = '\0';
        if (di_next_due(&due)) n += snprintf(flight + n, sizeof flight - (size_t)n, "; a disc read in flight, done in %llu ticks", (unsigned long long)(due - ticks));
        if (aram_next_due(&due) && n < (int)sizeof flight)
            n += snprintf(flight + n, sizeof flight - (size_t)n, "; an ARAM DMA in flight, done in %llu ticks", (unsigned long long)(due - ticks));
        fprintf(stderr, "[snapshot] frame %u: %s, %.1f MB, guest tick %llu, %llu bytes of WAV so far%s (%.0f ms)\n", frame,
                path, (double)(m.len + SNAP_HEADER) / 1048576.0, (unsigned long long)ticks, audio_wav_bytes(), flight,
                (double)(plat_mono_ns() - t0) / 1e6);
    }
    free(m.buf);
    return ok;
}

/* ---- reading -------------------------------------------------------------------- */

typedef struct {
    uint8_t* file;
    size_t bytes;
    unsigned frame;
    uint64_t ticks;
    char path[1024];
} Loaded;
static Loaded g_loaded;

static int read_whole(const char* path, Loaded* out, char* why, size_t cap)
{
    FILE* f = fopen(path, "rb");
    uint8_t* b = NULL;
    uint64_t n = 0, payload;
    uint32_t v, hb;
    uint8_t digest[20];
    memset(out, 0, sizeof *out);
    if (!f) {
        snprintf(why, cap, "cannot open the snapshot %s", path);
        return 0;
    }
    if (plat_path_kind(path, &n) != 1 || n < SNAP_HEADER || n > ((uint64_t)1 << 32)) {
        fclose(f);
        snprintf(why, cap, "%s is not a snapshot (it is %llu bytes)", path, (unsigned long long)n);
        return 0;
    }
    b = (uint8_t*)malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(b);
        snprintf(why, cap, "cannot read the snapshot %s", path);
        return 0;
    }
    fclose(f);
    memcpy(&v, b + 8, 4);
    memcpy(&hb, b + 12, 4);
    memcpy(&payload, b + 16, 8);
    if (memcmp(b, SNAP_MAGIC, 8) != 0) {
        snprintf(why, cap, "%s is not a snapshot", path);
    } else if (v != SNAP_VERSION || hb != SNAP_HEADER) {
        snprintf(why, cap, "%s is a snapshot of format %u; this runtime reads %u", path, v, SNAP_VERSION);
    } else if (payload != n - SNAP_HEADER) {
        snprintf(why, cap, "%s is cut short (%llu bytes of %llu)", path, (unsigned long long)(n - SNAP_HEADER),
                 (unsigned long long)payload);
    } else {
        sha1(b + SNAP_HEADER, (size_t)payload, digest);
        memcpy(&v, b + 56, 4);
        if (memcmp(digest, b + 24, 20) != 0) {
            snprintf(why, cap, "%s is damaged: its checksum does not match", path);
        } else if (v != RESUME_PC) {
            snprintf(why, cap, "%s resumes at %08X; this runtime resumes at %08X", path, v, RESUME_PC);
        } else {
            out->file = b;
            out->bytes = (size_t)n;
            memcpy(&v, b + 44, 4);
            out->frame = v;
            memcpy(&out->ticks, b + 48, 8);
            snprintf(out->path, sizeof out->path, "%s", path);
            return 1;
        }
    }
    free(b);
    return 0;
}

/* The section by name in a loaded file: its bytes and size, or NULL. */
static const uint8_t* find_section(const Loaded* l, const char* name, size_t* size)
{
    size_t at = SNAP_HEADER;
    while (l->file && at + SNAP_NAME + 8 <= l->bytes) {
        uint64_t n;
        char tag[SNAP_NAME + 1];
        memcpy(tag, l->file + at, SNAP_NAME);
        tag[SNAP_NAME] = '\0';
        memcpy(&n, l->file + at + SNAP_NAME, 8);
        at += SNAP_NAME + 8;
        if (n > l->bytes - at) return NULL;
        if (!strcmp(tag, name)) {
            *size = (size_t)n;
            return l->file + at;
        }
        at += (size_t)n;
    }
    return NULL;
}

/* Sections [first, last) into the statics (dry: only checked): 1, or 0 and why. */
static int load_range(CpuState* s, const Loaded* l, unsigned first, unsigned last, int dry, char* why, size_t cap)
{
    SnapImpl m;
    unsigned k;
    impl_init(&m, 1, dry);
    g_s = s;
    for (k = first; k < last && !m.pub.failed; k++) {
        size_t size = 0;
        const uint8_t* p = find_section(l, SECTIONS[k].name, &size);
        if (!p) {
            snap_fail(&m.pub, "the snapshot has no section %s (written by another build)", SECTIONS[k].name);
            break;
        }
        m.in = p;
        m.in_len = size;
        m.in_at = 0;
        m.section = SECTIONS[k].name;
        SECTIONS[k].fn(&m.pub);
        if (!m.pub.failed && m.in_at != m.in_len)
            snap_fail(&m.pub, "section %s is %zu bytes, longer than this build's %zu (written by another build)",
                      SECTIONS[k].name, m.in_len, m.in_at);
    }
    if (m.pub.failed) {
        snprintf(why, cap, "%s", m.why);
        return 0;
    }
    return 1;
}

long snapshot_host_get(const char* name, void* out, size_t max)
{
    char tag[SNAP_NAME];
    size_t size = 0;
    const uint8_t* p;
    if (!name || !*name || strlen(name) + 5 >= SNAP_NAME || !g_loaded.file) return -1;
    memset(tag, 0, sizeof tag);
    snprintf(tag, sizeof tag, "host:%s", name);
    p = find_section(&g_loaded, tag, &size);
    if (!p) return -1;
    if (out) memcpy(out, p, size < max ? size : max);
    return (long)size;
}

/* ---- when -------------------------------------------------------------------------- */

/* SOA_SNAPSHOT_AT=frame:path[,frame:path...]: a snapshot at the safe point
 * that begins each frame. The frame is gx_frame_count(), the frames presented
 * so far: the snapshot at N is taken after frame N-1 has ended, and a run
 * resumed from it presents frame N first. */
#define MAX_AT 16
typedef struct {
    unsigned frame;
    int done;
    char path[1024];
} At;
static At g_at[MAX_AT];
static int g_nat = -1;
static int g_skip_one;      /* the safe point a resume enters at is the snapshot, not a point to take one */
static unsigned g_resumed;  /* the frame a resumed run began at; 0 none */
static int g_resume_armed;  /* SOA_RESUME read, or soa_host_resume */

/* A host's request (soa_host_snapshot), and how the last one went. */
static PlatLock g_req_lock;
static char g_req_path[1024];
static int g_req;          /* 1: asked, not yet taken */
static int g_req_status;   /* -1 failed, 0 none or not yet, 1 written */
static long g_req_frame = -1;
static char g_req_why[512];

static void at_parse(void)
{
    const char* all = getenv("SOA_SNAPSHOT_AT");
    const char* p = all;
    g_nat = 0;
    while (p && *p && g_nat < MAX_AT) {
        char* end;
        unsigned long f = strtoul(p, &end, 10);
        const char* comma;
        size_t n;
        if (end == p || *end != ':') {
            fprintf(stderr, "[snapshot] SOA_SNAPSHOT_AT=%s: each item is frame:path; stopped at %s\n", all, p);
            break;
        }
        p = end + 1;
        comma = strchr(p, ',');
        n = comma ? (size_t)(comma - p) : strlen(p);
        if (!n || n >= sizeof g_at[0].path) {
            fprintf(stderr, "[snapshot] SOA_SNAPSHOT_AT: an item with no path, or one too long; stopped\n");
            break;
        }
        g_at[g_nat].frame = (unsigned)f;
        g_at[g_nat].done = 0;
        memcpy(g_at[g_nat].path, p, n);
        g_at[g_nat].path[n] = '\0';
        g_nat++;
        p = comma ? comma + 1 : p + n;
    }
    if (g_nat) fprintf(stderr, "[snapshot] %d snapshot(s) armed (SOA_SNAPSHOT_AT)\n", g_nat);
}

int snapshot_request(const char* path)
{
    if (!path || !*path || strlen(path) >= sizeof g_req_path) return 0;
    plat_lock(&g_req_lock);
    snprintf(g_req_path, sizeof g_req_path, "%s", path);
    g_req = 1;
    g_req_status = 0;
    g_req_frame = -1;
    g_req_why[0] = '\0';
    plat_unlock(&g_req_lock);
    return 1;
}

int snapshot_status(long* frame, char* why, size_t cap)
{
    int st;
    plat_lock(&g_req_lock);
    st = g_req_status;
    if (frame) *frame = g_req_frame;
    if (why && cap) snprintf(why, cap, "%s", g_req_why);
    plat_unlock(&g_req_lock);
    return st;
}

void snapshot_safe_point(CpuState* s)
{
    unsigned frame = gx_frame_count(), k;
    int asked;
    char why[512];
    if (g_nat < 0) at_parse();
    if (g_skip_one) {
        g_skip_one = 0;
        return;
    }
    for (k = 0; k < (unsigned)g_nat; k++) {
        if (g_at[k].done || frame < g_at[k].frame) continue;
        g_at[k].done = 1;
        if (!save(s, g_at[k].path, why, sizeof why))
            fprintf(stderr, "[snapshot] frame %u: no snapshot to %s: %s\n", frame, g_at[k].path, why);
    }
    plat_lock(&g_req_lock);
    asked = g_req;
    plat_unlock(&g_req_lock);
    if (asked) {
        char path[1024];
        int ok;
        plat_lock(&g_req_lock);
        snprintf(path, sizeof path, "%s", g_req_path);
        g_req = 0;
        plat_unlock(&g_req_lock);
        why[0] = '\0';
        ok = save(s, path, why, sizeof why);
        if (!ok) fprintf(stderr, "[snapshot] frame %u: no snapshot to %s: %s\n", frame, path, why);
        plat_lock(&g_req_lock);
        g_req_status = ok ? 1 : -1;
        g_req_frame = (long)frame;
        snprintf(g_req_why, sizeof g_req_why, "%s", ok ? "" : why);
        plat_unlock(&g_req_lock);
    }
}

/* ---- resuming ---------------------------------------------------------------------- */

int snapshot_arm_resume(const char* path, char* why, size_t cap)
{
    if (g_loaded.file) {
        free(g_loaded.file);
        memset(&g_loaded, 0, sizeof g_loaded);
    }
    g_resume_armed = 0;
    if (!path || !*path) return 0; /* NULL: no resume */
    if (!read_whole(path, &g_loaded, why, cap)) return 1;
    g_resume_armed = 1;
    return 0;
}

/* SOA_RESUME, read once: 0 (armed, or nothing asked), or 1 and why. */
static int g_env_read, g_env_failed;
static char g_env_why[600];

int snapshot_resume_env(char* why, size_t cap)
{
    if (!g_env_read) {
        const char* p = getenv("SOA_RESUME");
        g_env_read = 1;
        if (!g_resume_armed && p && *p && snapshot_arm_resume(p, g_env_why, sizeof g_env_why) != 0) g_env_failed = 1;
    }
    if (g_env_failed && why && cap) snprintf(why, cap, "SOA_RESUME: %s", g_env_why);
    return g_env_failed;
}

int snapshot_resuming(void)
{
    snapshot_resume_env(NULL, 0);
    return g_resume_armed;
}

unsigned snapshot_resumed_frame(void)
{
    return g_resumed;
}

/* The checks a host's start makes before the run (soa_host_start), and the
 * launcher's before the resume: this game library has the resume point, and
 * the build record, the replacements and the events are the snapshot's. */
int snapshot_precheck(char* why, size_t cap)
{
    const char* rec = game_record();
    if (snapshot_resume_env(why, cap)) return 1;
    if (!snapshot_resuming()) return 0;
    if (*rec && !strstr(rec, "resume=801DCB84")) {
        snprintf(why, cap, "the game library was built without its resume point (config/resumepoints.txt): rebuild it");
        return 1;
    }
    return load_range(NULL, &g_loaded, 0, 3, 1, why, cap) ? 0 : 1;
}

void snapshot_after_load(CpuState* s); /* the fix-ups once every section is in */

int snapshot_resume_run(CpuState* s)
{
    char why[600];
    uint64_t t0 = plat_mono_ns();
    if (!g_loaded.file) {
        fprintf(stderr, "[snapshot] nothing to resume from\n");
        return 1;
    }
    /* A section that fails to load leaves the statics part the snapshot's and
     * part not: the run does not start (the checks before it make this a
     * matter of memory or of a file changed since it was checked). */
    if (snapshot_precheck(why, sizeof why) != 0 || !load_range(s, &g_loaded, 0, NSECTIONS, 0, why, sizeof why)) {
        fprintf(stderr, "[snapshot] cannot resume from %s: %s\n", g_loaded.path, why);
        return 1;
    }
    snapshot_after_load(s);
    g_resumed = g_loaded.frame;
    g_skip_one = 1;
    /* SOA_SNAPSHOT_AT means the frames of the run this one continues: one at
     * or before the snapshot's frame was taken already (or is this one) */
    if (g_nat < 0) at_parse();
    {
        int k;
        for (k = 0; k < g_nat; k++)
            if (g_at[k].frame <= g_loaded.frame) g_at[k].done = 1;
    }
    fprintf(stderr, "[snapshot] resumed from %s at frame %u, guest tick %llu (%.0f ms)\n", g_loaded.path, g_loaded.frame,
            (unsigned long long)g_loaded.ticks, (double)(plat_mono_ns() - t0) / 1e6);
    /* main, entered at the top of its loop with every register as it was:
     * the call of VIGetRetraceCount there is the safe point again */
    s->spr[RESUME_SPR] = RESUME_PC;
    dispatch(s, RESUME_FN);
    fprintf(stderr, "[snapshot] main returned after a resume\n");
    return 0;
}

/* Once every section is in: what follows from the restored state rather than
 * being part of it. The quiet rule's "something is due" is in every swap
 * guard's byte (swap.c), which belongs to the game library's table and is
 * set from the clock's flag. */
#include "clock.h"
void snapshot_after_load(CpuState* s)
{
    (void)s;
    if (g_clock_due_fn) g_clock_due_fn(g_clock_due ? 1 : 0);
}

/* The frame of the snapshot armed for a resume (before it is resumed). */
unsigned snapshot_armed_frame(void)
{
    return g_loaded.file ? g_loaded.frame : 0u;
}
