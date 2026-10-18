/*
 * Save states (docs/SNAPSHOT.md): the runtime's whole state at a frame
 * boundary, written to a file and read back into a fresh process, which then
 * runs on as the first would have.
 *
 * The point is the top of the game's main loop (tick.c's safe point, the call
 * of VIGetRetraceCount at 0x801DCB84): the previous frame is presented, the
 * next has not begun, no interrupt handler or event is running, and the
 * guest's C stack is only main's frame -- which the recompiler gives a second
 * way in, at that address (config/resumepoints.txt). Everything else the
 * future depends on is state: guest memory, the registers, and each device
 * model's statics, which each file hands over with one function, the same for
 * saving and for loading:
 *
 *     void gx_snap(Snap* sn) { SNAP(sn, g_cp); SNAP(sn, g_xf); ... }
 *
 * A file's function is called with the section open; while saving SNAP
 * appends the bytes, while loading it fills them, and the section must come
 * out the size it went in (a layout that moved is refused, never half read).
 */
#ifndef SOA_SNAPSHOT_H
#define SOA_SNAPSHOT_H
#include <stddef.h>
#include <stdint.h>

#include <stdarg.h>
#include <stdio.h>

struct CpuState;
typedef struct Snap Snap;

/* What a file's function is handed: the direction, and the few operations,
 * through pointers, so that a file which hands over its state still builds
 * and links alone (the tests build most of runtime/ a file at a time). */
typedef struct {
    void (*bytes)(Snap* sn, void* p, size_t n);
    void (*buffer)(Snap* sn, void** p, size_t* n);
    void (*check)(Snap* sn, const void* p, size_t n, const char* what);
    void (*fail)(Snap* sn, const char* why);
    void (*pointer)(Snap* sn, void** p, int* ok);
} SnapOps;

struct Snap {
    const SnapOps* ops;
    int loading; /* 1: the bytes go into the variables; 0: they are saved */
    int dry;     /* 1: loading, but only checked (soa_host_start): into locals, never statics */
    int failed;
};

static inline int snap_loading(const Snap* sn) { return sn->loading; }
static inline int snap_dry(const Snap* sn) { return sn->dry; }
static inline int snap_failed(const Snap* sn) { return sn->failed; }
/* n bytes at p: written, or filled from the file. */
static inline void snap_bytes(Snap* sn, void* p, size_t n) { sn->ops->bytes(sn, p, n); }
#define SNAP(sn, v) snap_bytes((sn), (void*)&(v), sizeof(v))
/* A heap buffer whose size may vary: its size, then its bytes. Loading
 * reallocates *p (malloc) when the size differs; a NULL with size 0 stays so. */
static inline void snap_buffer(Snap* sn, void** p, size_t* n) { sn->ops->buffer(sn, p, n); }
/* A value only checked on loading: the file's must equal the process's own
 * (a configuration both runs must share, a table's size). */
static inline void snap_check(Snap* sn, const void* p, size_t n, const char* what) { sn->ops->check(sn, p, n, what); }
#define SNAP_CHECK(sn, v, what) snap_check((sn), (const void*)&(v), sizeof(v), (what))
/* This state cannot be saved now (saving), or the file's cannot be taken
 * (loading): the whole snapshot is refused, with the reason. */
static inline void snap_fail(Snap* sn, const char* fmt, ...)
{
    char why[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    sn->ops->fail(sn, why);
}
/* A pointer into code or data of a loaded module (an event's function, a
 * replacement's), as the module's file name and an offset, so a fresh
 * process -- with the modules at other addresses -- finds the same one. A
 * pointer into no module (the heap) loads as NULL with *ok 0. */
static inline void snap_pointer(Snap* sn, void** p, int* ok) { sn->ops->pointer(sn, p, ok); }

/* ---- the runtime's side (snapshot.c) ------------------------------------- */

/* tick.c's safe point, on the guest's thread, before anything of it runs. */
void snapshot_safe_point(struct CpuState* s);
/* main.c: before the guest's first instruction. 1 when the run resumes from a
 * snapshot (SOA_RESUME, or soa_host_resume): then snapshot_resume_run runs
 * the guest from it in place of the entry point. */
int snapshot_resuming(void);
void snapshot_resume_run(struct CpuState* s);
/* The frame a resumed run began at (the snapshot's), or 0; and before it
 * has, the frame of the snapshot armed. */
unsigned snapshot_resumed_frame(void);
unsigned snapshot_armed_frame(void);
/* Ask for a snapshot at the next safe point (soa_host_snapshot): 1 taken. */
int snapshot_request(const char* path);
/* -1 failed, 0 none asked or not yet written, 1 written; *frame its frame. */
int snapshot_status(long* frame, char* why, size_t cap);
/* Check a file can be resumed from (header, checksum, this build), and arm
 * the resume: 0, or nonzero and why. */
int snapshot_arm_resume(const char* path, char* why, size_t cap);
/* A host's own section (soa_host_snapshot_put/get): kept with the next
 * snapshot written, and read back from the one being resumed. The hook is
 * called on the guest's thread as a snapshot is taken, for the host to put
 * its state as of that point. */
int snapshot_host_put(const char* name, const void* data, size_t bytes);
long snapshot_host_get(const char* name, void* out, size_t max);
void snapshot_set_host_hook(void (*fn)(void* user), void* user);
/* Before a resumed run starts (soa_host_start; the launcher's run_guest): the
 * game library has the resume point, and the replacements and events the
 * snapshot was taken with are here again. 0, or nonzero and why. */
int snapshot_precheck(char* why, size_t cap);

#endif
