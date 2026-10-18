/*
 * The platform layer's cold half (docs/specs/portability.md 3.3): what only
 * main.c and the device files ask of an operating system. The renderer needs
 * none of it, so render_check.py's renderer-only build still links without
 * this file. Each function lands with its first caller -- L7's are the MEM1
 * image's guard and the guest's stack, V5's the shared-library loader the
 * GPU backend opens Vulkan with -- so nothing here is compiled and never run.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* REG_ERR in ucontext, on glibc */
#endif
#define _CRT_SECURE_NO_WARNINGS
#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#endif

/* ---- address space ------------------------------------------------------ */

#ifdef _WIN32
void* plat_reserve(size_t bytes) { return VirtualAlloc(NULL, bytes, MEM_RESERVE, PAGE_NOACCESS); }
int plat_commit(void* p, size_t bytes) { return VirtualAlloc(p, bytes, MEM_COMMIT, PAGE_READWRITE) != NULL; }
void plat_release(void* p, size_t bytes)
{
    (void)bytes;
    VirtualFree(p, 0, MEM_RELEASE);
}
unsigned long plat_last_error(void) { return (unsigned long)GetLastError(); }
#else
void* plat_reserve(size_t bytes)
{
    void* p = mmap(NULL, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
/* Anonymous pages are zero, as VirtualAlloc's committed ones are. The start
 * must be page-aligned; the length is rounded up to whole pages. */
int plat_commit(void* p, size_t bytes) { return mprotect(p, bytes, PROT_READ | PROT_WRITE) == 0; }
void plat_release(void* p, size_t bytes) { munmap(p, bytes); }
unsigned long plat_last_error(void) { return (unsigned long)errno; }
#endif

/* ---- the guard on a reserved range ---------------------------------------
 * One guard per process: main.c's MEM1 image is the only caller. The report
 * main.c's callback prints uses fprintf, which is not async-signal-safe; the
 * Windows handler does the same from a vectored handler, and it fires once a
 * run, on the faulting thread, in generated code rather than inside stdio
 * (3.3, "one shortcut, stated"). */
static uint8_t* g_guard_base;
static size_t g_guard_lo, g_guard_hi;
static PlatFaultFn g_guard_fn;

#ifdef _WIN32
static DWORD g_guard_owner;

static LONG CALLBACK guard_veh(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    uintptr_t off;
    /* Every other fault in the process belongs to somebody else. The
     * subtraction is unsigned, so an address below the range gives a huge
     * offset and falls out of the test with it. */
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 || !g_guard_base)
        return EXCEPTION_CONTINUE_SEARCH;
    off = (uintptr_t)er->ExceptionInformation[1] - (uintptr_t)g_guard_base;
    if (off < g_guard_lo || off >= g_guard_hi) return EXCEPTION_CONTINUE_SEARCH;
    return g_guard_fn((size_t)off, er->ExceptionInformation[0] != 0, GetCurrentThreadId() == g_guard_owner)
               ? EXCEPTION_CONTINUE_EXECUTION
               : EXCEPTION_CONTINUE_SEARCH;
}

int plat_guard_install(void* base, size_t lo, size_t hi, PlatFaultFn fn)
{
    g_guard_lo = lo;
    g_guard_hi = hi;
    g_guard_fn = fn;
    g_guard_owner = GetCurrentThreadId();
    g_guard_base = (uint8_t*)base;
    if (AddVectoredExceptionHandler(1, guard_veh)) return 1;
    g_guard_base = NULL;
    return 0;
}

void plat_guard_owner(void) { g_guard_owner = GetCurrentThreadId(); }
#else
static struct sigaction g_guard_prev;
static pthread_t g_guard_owner_id;
static int g_guard_owner_set;

/* Whether the faulting access was a store: the page fault's error code on
 * x86-64 (bit 1), and on AArch64 the ESR the kernel puts in the signal
 * frame's records (ESR_MAGIC), bit 6 (WnR). -1 where neither is to hand. The
 * AArch64 record's layout is the kernel's ABI, spelled out here rather than
 * taken from <asm/sigcontext.h>, which clashes with glibc's own. */
static int fault_storing(void* ucp)
{
#if defined(__x86_64__) && defined(REG_ERR)
    const ucontext_t* uc = (const ucontext_t*)ucp;
    return (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;
#elif defined(__aarch64__) && defined(__linux__)
    struct rec {
        uint32_t magic, size;
    };
    const ucontext_t* uc = (const ucontext_t*)ucp;
    const uint8_t* p = (const uint8_t*)uc->uc_mcontext.__reserved;
    const uint8_t* end = p + sizeof uc->uc_mcontext.__reserved;
    while (p + sizeof(struct rec) <= end) {
        const struct rec* r = (const struct rec*)p;
        if (r->magic == 0 || r->size < sizeof *r) break;
        if (r->magic == 0x45535201u /* ESR_MAGIC */ && r->size >= sizeof *r + 8) {
            uint64_t esr;
            memcpy(&esr, p + sizeof *r, 8);
            return (int)((esr >> 6) & 1);
        }
        p += r->size;
    }
    return -1;
#else
    (void)ucp;
    return -1;
#endif
}

static void guard_segv(int sig, siginfo_t* si, void* uc)
{
    uintptr_t off = (uintptr_t)si->si_addr - (uintptr_t)g_guard_base;
    if (g_guard_base && off >= g_guard_lo && off < g_guard_hi) {
        int owner = g_guard_owner_set && pthread_equal(pthread_self(), g_guard_owner_id);
        if (g_guard_fn((size_t)off, fault_storing(uc), owner)) return; /* the access runs again */
    }
    /* Not this guard's, or not fixable: the handler before it decides. With
     * none, the default comes back and the fault, run again, ends the process
     * as it would have without the guard. */
    if ((g_guard_prev.sa_flags & SA_SIGINFO) && g_guard_prev.sa_sigaction) {
        g_guard_prev.sa_sigaction(sig, si, uc);
        return;
    }
    if (!(g_guard_prev.sa_flags & SA_SIGINFO) && g_guard_prev.sa_handler != SIG_DFL &&
        g_guard_prev.sa_handler != SIG_IGN) {
        g_guard_prev.sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL);
}

/* The handler runs on an alternate stack where one has been set, so a fault
 * on a stack with no room left can still be reported. Per thread: the
 * installer's here, and the owner's when plat_guard_owner names another. */
static void alt_stack(void)
{
    stack_t ss;
    size_t n = 64 * 1024;
    ss.ss_sp = malloc(n);
    if (!ss.ss_sp) return;
    ss.ss_size = n;
    ss.ss_flags = 0;
    if (sigaltstack(&ss, NULL) != 0) free(ss.ss_sp);
}

int plat_guard_install(void* base, size_t lo, size_t hi, PlatFaultFn fn)
{
    struct sigaction sa;
    g_guard_lo = lo;
    g_guard_hi = hi;
    g_guard_fn = fn;
    g_guard_owner_id = pthread_self();
    g_guard_owner_set = 1;
    g_guard_base = (uint8_t*)base;
    alt_stack();
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = guard_segv;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &g_guard_prev) == 0) return 1;
    g_guard_base = NULL;
    return 0;
}

void plat_guard_owner(void)
{
    g_guard_owner_id = pthread_self();
    g_guard_owner_set = 1;
    alt_stack();
}
#endif

/* ---- the guest's stack ---------------------------------------------------
 * The recompiled code recurses as deeply as the game does, on the host stack:
 * the Windows build links with a large /STACK for the main thread, and
 * elsewhere the guest gets a thread whose stack is made that size. */
#ifdef _WIN32
int plat_run_on_big_stack(int (*fn)(void*), void* arg, size_t bytes)
{
    (void)bytes;
    return fn(arg);
}
#else
typedef struct {
    int (*fn)(void*);
    void* arg;
    int rc;
} BigStack;

static void* big_stack_main(void* p)
{
    BigStack* b = (BigStack*)p;
    b->rc = b->fn(b->arg);
    return NULL;
}

int plat_run_on_big_stack(int (*fn)(void*), void* arg, size_t bytes)
{
    pthread_attr_t attr;
    pthread_t id;
    BigStack b;
    int made;
    b.fn = fn;
    b.arg = arg;
    b.rc = 0;
    pthread_attr_init(&attr);
    made = pthread_attr_setstacksize(&attr, bytes) == 0 && pthread_create(&id, &attr, big_stack_main, &b) == 0;
    pthread_attr_destroy(&attr);
    if (!made) {
        fprintf(stderr, "[plat] cannot start a thread with a %zu MB stack; running on this one\n", bytes >> 20);
        return fn(arg);
    }
    pthread_join(id, NULL);
    return b.rc;
}
#endif

/* ---- shared libraries ---------------------------------------------------- */

#ifdef _WIN32
void* plat_dl_open(const char* path, char* err, size_t cap)
{
    /* Given a path (a mod's library), the libraries it needs are looked for
     * beside it first; a bare name (vulkan-1.dll) is the system's search. */
    DWORD flags = strchr(path, '\\') || strchr(path, '/') ? LOAD_WITH_ALTERED_SEARCH_PATH : 0;
    HMODULE m = LoadLibraryExA(path, NULL, flags);
    if (!m && err && cap) snprintf(err, cap, "LoadLibrary(%s) failed (error %lu)", path, GetLastError());
    return (void*)m;
}
void* plat_dl_sym(void* lib, const char* name) { return (void*)(uintptr_t)GetProcAddress((HMODULE)lib, name); }
void plat_dl_close(void* lib)
{
    if (lib) FreeLibrary((HMODULE)lib);
}
#else
#include <dlfcn.h>
void* plat_dl_open(const char* path, char* err, size_t cap)
{
    void* m = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!m && err && cap) snprintf(err, cap, "dlopen(%s) failed: %s", path, dlerror());
    return m;
}
void* plat_dl_sym(void* lib, const char* name) { return dlsym(lib, name); }
void plat_dl_close(void* lib)
{
    if (lib) dlclose(lib);
}
#endif

/* plat_addr_module and plat_module_addr. Windows: the module that holds the
 * address, its file name, and the offset from its base (the HMODULE). ELF:
 * the object whose loaded segments hold it (dl_iterate_phdr), its file name
 * ("<program>" for the executable itself, which the loader names ""), and
 * the offset from its load bias -- the same bias the way back adds. */
static const char* base_name(const char* path)
{
    const char* a = strrchr(path, '/');
    const char* b = strrchr(path, '\\');
    if (b && (!a || b > a)) a = b;
    return a ? a + 1 : path;
}
#ifdef _WIN32
int plat_addr_module(const void* p, char* name, size_t cap, uintptr_t* off)
{
    HMODULE m = NULL;
    char path[MAX_PATH];
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)p, &m) ||
        !m || !GetModuleFileNameA(m, path, sizeof path))
        return 0;
    snprintf(name, cap, "%s", base_name(path));
    *off = (uintptr_t)p - (uintptr_t)m;
    return 1;
}
void* plat_module_addr(const char* name, uintptr_t off)
{
    char exe[MAX_PATH];
    HMODULE m = GetModuleHandleA(name);
    if (!m && GetModuleFileNameA(NULL, exe, sizeof exe) && !_stricmp(base_name(exe), name)) m = GetModuleHandleA(NULL);
    return m ? (void*)((uintptr_t)m + off) : NULL;
}
#else
#include <link.h>
typedef struct {
    uintptr_t addr;
    const char* want; /* by name: plat_module_addr */
    char name[256];
    uintptr_t bias;
    int found;
} ModFind;
static int mod_find(struct dl_phdr_info* info, size_t size, void* arg)
{
    ModFind* f = (ModFind*)arg;
    const char* nm = info->dlpi_name && *info->dlpi_name ? base_name(info->dlpi_name) : "<program>";
    int i;
    (void)size;
    if (f->want) {
        if (strcmp(nm, f->want) != 0) return 0;
        f->bias = (uintptr_t)info->dlpi_addr;
        f->found = 1;
        return 1;
    }
    for (i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr)* ph = &info->dlpi_phdr[i];
        uintptr_t lo = (uintptr_t)info->dlpi_addr + ph->p_vaddr;
        if (ph->p_type != PT_LOAD || f->addr < lo || f->addr - lo >= ph->p_memsz) continue;
        snprintf(f->name, sizeof f->name, "%s", nm);
        f->bias = (uintptr_t)info->dlpi_addr;
        f->found = 1;
        return 1;
    }
    return 0;
}
int plat_addr_module(const void* p, char* name, size_t cap, uintptr_t* off)
{
    ModFind f;
    memset(&f, 0, sizeof f);
    f.addr = (uintptr_t)p;
    dl_iterate_phdr(mod_find, &f);
    if (!f.found) return 0;
    snprintf(name, cap, "%s", f.name);
    *off = (uintptr_t)p - f.bias;
    return 1;
}
void* plat_module_addr(const char* name, uintptr_t off)
{
    ModFind f;
    memset(&f, 0, sizeof f);
    f.want = name;
    dl_iterate_phdr(mod_find, &f);
    return f.found ? (void*)(f.bias + off) : NULL;
}
#endif

/* ---- directories ------------------------------------------------------------ */

#ifdef _WIN32
int plat_mkdir(const char* path)
{
    return CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}
#else
#include <sys/stat.h>
int plat_mkdir(const char* path)
{
    return mkdir(path, 0755) == 0 || errno == EEXIST;
}
#endif

/* ---- L9: the executable, paths, mods' folders, CPU time ------------------ */

#ifdef _WIN32
int plat_exe_path(char* out, size_t cap)
{
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    return n && n < cap;
}
int plat_realpath(const char* in, char* out, size_t cap)
{
    DWORD n = GetFullPathNameA(in, (DWORD)cap, out, NULL);
    return n && n < cap;
}
static char g_dl_why[96];
const char* plat_dl_why(void)
{
    snprintf(g_dl_why, sizeof g_dl_why, "error %lu", (unsigned long)GetLastError());
    return g_dl_why;
}
int plat_list_dirs(const char* dir, void (*fn)(const char* name, void* u), void* u)
{
    char pattern[1100];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0;
    snprintf(pattern, sizeof pattern, "%s/*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        fn(fd.cFileName, u);
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}
double plat_process_cpu(double* user, double* kernel)
{
    FILETIME c, e, k, us;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &us)) {
        *user = (double)(((uint64_t)us.dwHighDateTime << 32) | us.dwLowDateTime) / 1e7;
        *kernel = (double)(((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) / 1e7;
        return *user + *kernel;
    }
    *user = *kernel = 0.0;
    return 0.0;
}
#else
#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
int plat_exe_path(char* out, size_t cap)
{
    ssize_t n = cap ? readlink("/proc/self/exe", out, cap - 1) : -1;
    if (n <= 0 || (size_t)n >= cap - 1) return 0;
    out[n] = '\0';
    return 1;
}
int plat_realpath(const char* in, char* out, size_t cap)
{
    char buf[PATH_MAX];
    if (!realpath(in, buf) || strlen(buf) >= cap) return 0;
    memcpy(out, buf, strlen(buf) + 1);
    return 1;
}
const char* plat_dl_why(void)
{
    const char* e = dlerror();
    return e ? e : "no reason given";
}
int plat_list_dirs(const char* dir, void (*fn)(const char* name, void* u), void* u)
{
    DIR* d = opendir(dir);
    struct dirent* e;
    int n = 0;
    if (!d) return -1;
    while ((e = readdir(d)) != NULL) {
        char path[1100];
        struct stat st;
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        fn(e->d_name, u);
        n++;
    }
    closedir(d);
    return n;
}
double plat_process_cpu(double* user, double* kernel)
{
    struct rusage r;
    if (getrusage(RUSAGE_SELF, &r) == 0) {
        *user = (double)r.ru_utime.tv_sec + r.ru_utime.tv_usec / 1e6;
        *kernel = (double)r.ru_stime.tv_sec + r.ru_stime.tv_usec / 1e6;
        return *user + *kernel;
    }
    *user = *kernel = 0.0;
    return 0.0;
}
#endif
