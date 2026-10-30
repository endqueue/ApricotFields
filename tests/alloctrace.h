// Every heap allocation one thread makes while counting, each attributed to
// the code that asked for it. Test code only, with globals of its own:
// include it in one file per executable. Written in C, for the tests of C
// headers.
//
// What it sees, per platform:
// - Windows: RtlAllocateHeap and RtlReAllocateHeap, through the import
//   table of every module loaded when counting starts (each slot pointed
//   at a counter for the count's length, then put back). Every heap reaches
//   them: the CRT's malloc and operator new (debug or release CRT), and
//   HeapAlloc from the D3D12 runtime, the drivers' user-mode parts, the
//   Vulkan loader and its layers. It misses a module loaded after counting
//   starts, a call ntdll makes to itself, and memory that is not heap:
//   VirtualAlloc, a driver's own mappings. Release builds count too.
// - macOS: malloc_logger, which every malloc zone reports to.
// - Linux (glibc): malloc and the rest replaced here, forwarding to glibc's
//   own; every module's calls reach them. Written, not run.
// - Under a sanitizer the sanitizer's heap takes over and nothing above is
//   called: nothing is measured, and alloc_trace_available() says so.
//
// Attribution: each allocation's stack is kept raw while counting (no
// symbol is looked up and nothing is allocated then). At the report, the
// frames of the allocator itself are passed over: the trace's own, the C
// and C++ runtimes and the system's heap modules, `operator new` and the
// standard library's containers, the workspace's own allocator layers
// (SpudLib's allocator helpers, eqstd), and any function the test names with
// alloc_trace_skip() (a caller allocator's own functions). The
// first frame left is the asker. An allocation whose asker is in this
// executable (where the libraries under test are linked) is ours, counted
// and reported by function; any other (a runtime, a driver) is reported
// apart under its module, with the nearest frame of ours that led to it,
// and not counted.
//
// Only the thread that called alloc_trace_start() is counted. Each library
// that uses it keeps an identical copy in its own tests/, held equal by the
// workspace's check.sh (alloc-trace-copies).

#ifndef ALLOCTRACE_H
#define ALLOCTRACE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define ALLOC_TRACE_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define ALLOC_TRACE_SANITIZED 1
#endif
#endif
#ifndef ALLOC_TRACE_SANITIZED
#define ALLOC_TRACE_SANITIZED 0
#endif

#define ALLOC_TRACE_DEPTH 32
// Distinct stacks kept while counting. A validation layer's allocations
// come from thousands of stacks a frame (Metal's debug layer filled 1,024 in
// one), and what no stack has room for is counted as ours, unattributed.
#define ALLOC_TRACE_STACKS 16384
#define ALLOC_TRACE_SKIPS 16

typedef struct alloc_trace_internal_stack
{
    void *pcs[ALLOC_TRACE_DEPTH];
    unsigned depth;
    unsigned long count;
} alloc_trace_internal_stack;

static alloc_trace_internal_stack alloc_trace_internal_stacks[ALLOC_TRACE_STACKS];
static unsigned long alloc_trace_internal_unrecorded; // allocations the table had no room for
static volatile bool alloc_trace_internal_counting = false;
static const char *alloc_trace_internal_skips[ALLOC_TRACE_SKIPS];
static unsigned alloc_trace_internal_skip_count;

static unsigned long alloc_trace_internal_ours;  // after alloc_trace_stop()
static unsigned long alloc_trace_internal_apart; // after alloc_trace_stop()

// Keeps one stack: the same stack again adds to its count.
static void alloc_trace_internal_record(void **pcs, unsigned depth)
{
    uint64_t hash = 1469598103934665603ull;
    for (unsigned i = 0; i < depth; i++)
        hash = (hash ^ (uint64_t)(uintptr_t)pcs[i]) * 1099511628211ull;
    for (unsigned probe = 0; probe < ALLOC_TRACE_STACKS; probe++)
    {
        alloc_trace_internal_stack *s = &alloc_trace_internal_stacks[(hash + probe) % ALLOC_TRACE_STACKS];
        if (s->count == 0)
        {
            memcpy(s->pcs, pcs, depth * sizeof(void *));
            s->depth = depth;
            s->count = 1;
            return;
        }
        if (s->depth == depth && memcmp(s->pcs, pcs, depth * sizeof(void *)) == 0)
        {
            s->count++;
            return;
        }
    }
    alloc_trace_internal_unrecorded++;
}

// ---- Platforms ---------------------------------------------------------------

#if ALLOC_TRACE_SANITIZED

static bool alloc_trace_internal_platform_available(void)
{
    return false;
}
static void alloc_trace_internal_platform_start(void)
{
}
static void alloc_trace_internal_platform_stop(void)
{
}
static bool alloc_trace_internal_module(void *pc, char *name, size_t size, bool *ours)
{
    (void)pc, (void)name, (void)size, (void)ours;
    return false;
}
static bool alloc_trace_internal_symbol(void *pc, char *name, size_t size)
{
    (void)pc, (void)name, (void)size;
    return false;
}

#elif defined(_WIN32)

#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#pragma comment(lib, "dbghelp.lib")

typedef PVOID(NTAPI *alloc_trace_internal_allocate_fn)(PVOID heap, ULONG flags, SIZE_T size);
typedef PVOID(NTAPI *alloc_trace_internal_reallocate_fn)(PVOID heap, ULONG flags, PVOID block, SIZE_T size);
static alloc_trace_internal_allocate_fn alloc_trace_internal_real_allocate;
static alloc_trace_internal_reallocate_fn alloc_trace_internal_real_reallocate;
static DWORD alloc_trace_internal_thread;

static void alloc_trace_internal_note(void)
{
    if (!alloc_trace_internal_counting || GetCurrentThreadId() != alloc_trace_internal_thread)
        return;
    void *pcs[ALLOC_TRACE_DEPTH];
    // 2: this function and the counter that called it.
    const USHORT depth = RtlCaptureStackBackTrace(2, ALLOC_TRACE_DEPTH, pcs, NULL);
    alloc_trace_internal_record(pcs, depth);
}
static PVOID NTAPI alloc_trace_internal_allocate(PVOID heap, ULONG flags, SIZE_T size)
{
    alloc_trace_internal_note();
    return alloc_trace_internal_real_allocate(heap, flags, size);
}
static PVOID NTAPI alloc_trace_internal_reallocate(PVOID heap, ULONG flags, PVOID block, SIZE_T size)
{
    alloc_trace_internal_note();
    return alloc_trace_internal_real_reallocate(heap, flags, block, size);
}

// The import slots pointed at the counters, to be put back.
typedef struct alloc_trace_internal_slot
{
    void **slot;
    void *was;
} alloc_trace_internal_slot;
static alloc_trace_internal_slot alloc_trace_internal_slots[8192];
static unsigned alloc_trace_internal_slot_count;
static HMODULE alloc_trace_internal_modules[2048];

static void alloc_trace_internal_set_slot(void **slot, void *to)
{
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
        return;
    *(void *volatile *)slot = to;
    VirtualProtect(slot, sizeof(void *), old, &old);
}

// Every import slot of [module] that holds one of [targets] is pointed at
// the counter of the same kind.
static void alloc_trace_internal_patch_module(HMODULE module, void *const *allocate, unsigned allocate_count,
                                    void *const *reallocate, unsigned reallocate_count)
{
    BYTE *base = (BYTE *)module;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0)
        return;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++)
    {
        for (void **slot = (void **)(base + d->FirstThunk); *slot; slot++)
        {
            void *to = NULL;
            for (unsigned i = 0; i < allocate_count; i++)
                if (*slot == allocate[i])
                    to = (void *)alloc_trace_internal_allocate;
            for (unsigned i = 0; i < reallocate_count; i++)
                if (*slot == reallocate[i])
                    to = (void *)alloc_trace_internal_reallocate;
            if (!to || alloc_trace_internal_slot_count == sizeof(alloc_trace_internal_slots) / sizeof(alloc_trace_internal_slots[0]))
                continue;
            alloc_trace_internal_slots[alloc_trace_internal_slot_count].slot = slot;
            alloc_trace_internal_slots[alloc_trace_internal_slot_count].was = *slot;
            alloc_trace_internal_slot_count++;
            alloc_trace_internal_set_slot(slot, to);
        }
    }
}

static bool alloc_trace_internal_platform_available(void)
{
    return true;
}
static void alloc_trace_internal_platform_start(void)
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    alloc_trace_internal_real_allocate = (alloc_trace_internal_allocate_fn)(void *)GetProcAddress(ntdll, "RtlAllocateHeap");
    alloc_trace_internal_real_reallocate = (alloc_trace_internal_reallocate_fn)(void *)GetProcAddress(ntdll, "RtlReAllocateHeap");
    // What an import slot of HeapAlloc or HeapReAlloc holds: kernel32's and
    // kernelbase's exports, which are forwarded to ntdll's, or their own.
    void *allocate[3] = {(void *)alloc_trace_internal_real_allocate, NULL, NULL};
    void *reallocate[3] = {(void *)alloc_trace_internal_real_reallocate, NULL, NULL};
    unsigned allocate_count = 1, reallocate_count = 1;
    const wchar_t *forwarders[2] = {L"kernel32.dll", L"kernelbase.dll"};
    for (int i = 0; i < 2; i++)
    {
        HMODULE m = GetModuleHandleW(forwarders[i]);
        if (!m)
            continue;
        void *a = (void *)GetProcAddress(m, "HeapAlloc");
        void *r = (void *)GetProcAddress(m, "HeapReAlloc");
        if (a && a != allocate[0])
            allocate[allocate_count++] = a;
        if (r && r != reallocate[0])
            reallocate[reallocate_count++] = r;
    }
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), alloc_trace_internal_modules, sizeof(alloc_trace_internal_modules), &needed))
        needed = 0;
    unsigned count = needed / sizeof(HMODULE);
    if (count > sizeof(alloc_trace_internal_modules) / sizeof(alloc_trace_internal_modules[0]))
        count = sizeof(alloc_trace_internal_modules) / sizeof(alloc_trace_internal_modules[0]);
    alloc_trace_internal_slot_count = 0;
    for (unsigned i = 0; i < count; i++)
        alloc_trace_internal_patch_module(alloc_trace_internal_modules[i], allocate, allocate_count, reallocate, reallocate_count);
    alloc_trace_internal_thread = GetCurrentThreadId();
}
static void alloc_trace_internal_platform_stop(void)
{
    for (unsigned i = 0; i < alloc_trace_internal_slot_count; i++)
        alloc_trace_internal_set_slot(alloc_trace_internal_slots[i].slot, alloc_trace_internal_slots[i].was);
    alloc_trace_internal_slot_count = 0;
}

// [pc]'s module's file name, and whether it is this executable.
static bool alloc_trace_internal_module(void *pc, char *name, size_t size, bool *ours)
{
    HMODULE module = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)pc, &module))
        return false;
    char path[MAX_PATH];
    const DWORD length = GetModuleFileNameA(module, path, sizeof(path));
    const char *slash = strrchr(path, '\\');
    snprintf(name, size, "%s", length ? (slash ? slash + 1 : path) : "?");
    *ours = module == GetModuleHandleA(NULL);
    return true;
}
static bool alloc_trace_internal_symbols_ready = false;
static bool alloc_trace_internal_symbol(void *pc, char *name, size_t size)
{
    if (!alloc_trace_internal_symbols_ready)
    {
        SymSetOptions(SymGetOptions() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        alloc_trace_internal_symbols_ready = SymInitialize(GetCurrentProcess(), NULL, TRUE) != FALSE;
    }
    union
    {
        SYMBOL_INFO info;
        char bytes[sizeof(SYMBOL_INFO) + 512];
    } symbol;
    memset(&symbol, 0, sizeof(symbol));
    symbol.info.SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol.info.MaxNameLen = 511;
    DWORD64 displacement = 0;
    if (!alloc_trace_internal_symbols_ready || !SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)pc, &displacement, &symbol.info))
        return false;
    snprintf(name, size, "%s", symbol.info.Name);
    return true;
}

#elif defined(__APPLE__)

#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
typedef void(alloc_trace_internal_logger_t)(uint32_t type, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t result,
                                  uint32_t frames_to_skip);
extern alloc_trace_internal_logger_t *malloc_logger;
static pthread_t alloc_trace_internal_thread;

static void alloc_trace_internal_logged(uint32_t type, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t result, uint32_t skip)
{
    (void)a, (void)b, (void)c, (void)result, (void)skip;
    if (!alloc_trace_internal_counting || !(type & 2u) || !pthread_equal(pthread_self(), alloc_trace_internal_thread)) // 2: an allocation
        return;
    void *pcs[ALLOC_TRACE_DEPTH + 1];
    const int depth = backtrace(pcs, ALLOC_TRACE_DEPTH + 1);
    if (depth > 1)
        alloc_trace_internal_record(pcs + 1, (unsigned)depth - 1); // 1: this function
}
static bool alloc_trace_internal_platform_available(void)
{
    return true;
}
// Both stores through a volatile pointer: the compiler takes malloc to read
// no global of the program's, so between them the first looked dead, and
// Release builds dropped it (the logger was never set).
static void alloc_trace_internal_platform_start(void)
{
    alloc_trace_internal_thread = pthread_self();
    *(alloc_trace_internal_logger_t *volatile *)&malloc_logger = alloc_trace_internal_logged;
}
static void alloc_trace_internal_platform_stop(void)
{
    *(alloc_trace_internal_logger_t *volatile *)&malloc_logger = NULL;
}

#elif defined(__linux__) && defined(__GLIBC__)

#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void *__libc_memalign(size_t, size_t);
static pthread_t alloc_trace_internal_thread;
static __thread bool alloc_trace_internal_inside = false;

#define ALLOC_TRACE_NOTE()                                                                                             \
    do                                                                                                                \
    {                                                                                                                 \
        if (alloc_trace_internal_counting && !alloc_trace_internal_inside && pthread_equal(pthread_self(), alloc_trace_internal_thread))            \
        {                                                                                                             \
            alloc_trace_internal_inside = true;                                                                                 \
            void *pcs_[ALLOC_TRACE_DEPTH + 1];                                                                         \
            const int depth_ = backtrace(pcs_, ALLOC_TRACE_DEPTH + 1);                                                 \
            if (depth_ > 1)                                                                                           \
                alloc_trace_internal_record(pcs_ + 1, (unsigned)depth_ - 1);                                                    \
            alloc_trace_internal_inside = false;                                                                                \
        }                                                                                                             \
    } while (0)

void *malloc(size_t size)
{
    ALLOC_TRACE_NOTE();
    return __libc_malloc(size);
}
void *calloc(size_t n, size_t size)
{
    ALLOC_TRACE_NOTE();
    return __libc_calloc(n, size);
}
void *realloc(void *p, size_t size)
{
    ALLOC_TRACE_NOTE();
    return __libc_realloc(p, size);
}
void *memalign(size_t alignment, size_t size)
{
    ALLOC_TRACE_NOTE();
    return __libc_memalign(alignment, size);
}
void *aligned_alloc(size_t alignment, size_t size)
{
    ALLOC_TRACE_NOTE();
    return __libc_memalign(alignment, size);
}
int posix_memalign(void **out, size_t alignment, size_t size)
{
    ALLOC_TRACE_NOTE();
    *out = __libc_memalign(alignment, size);
    return *out ? 0 : 12; // ENOMEM
}
static bool alloc_trace_internal_platform_available(void)
{
    return true;
}
static void alloc_trace_internal_platform_start(void)
{
    // glibc's first backtrace loads libgcc_s, which allocates: done here,
    // before counting.
    void *warm[4];
    backtrace(warm, 4);
    alloc_trace_internal_thread = pthread_self();
}
static void alloc_trace_internal_platform_stop(void)
{
}

#else

static bool alloc_trace_internal_platform_available(void)
{
    return false;
}
static void alloc_trace_internal_platform_start(void)
{
}
static void alloc_trace_internal_platform_stop(void)
{
}
static bool alloc_trace_internal_module(void *pc, char *name, size_t size, bool *ours)
{
    (void)pc, (void)name, (void)size, (void)ours;
    return false;
}
static bool alloc_trace_internal_symbol(void *pc, char *name, size_t size)
{
    (void)pc, (void)name, (void)size;
    return false;
}

#endif

#if !ALLOC_TRACE_SANITIZED && (defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__)))
// The executable is ours; a shared library or framework is not.
static bool alloc_trace_internal_module(void *pc, char *name, size_t size, bool *ours)
{
    Dl_info info;
    Dl_info self;
    if (!dladdr(pc, &info) || !info.dli_fname)
        return false;
    const char *slash = strrchr(info.dli_fname, '/');
    snprintf(name, size, "%s", slash ? slash + 1 : info.dli_fname);
    *ours = dladdr((void *)alloc_trace_internal_record, &self) && self.dli_fbase == info.dli_fbase;
    return true;
}
extern char *__cxa_demangle(const char *mangled, char *out, size_t *length, int *status);
static bool alloc_trace_internal_symbol(void *pc, char *name, size_t size)
{
    Dl_info info;
    if (!dladdr(pc, &info) || !info.dli_sname)
        return false;
    int status = -1;
    char *demangled = __cxa_demangle(info.dli_sname, NULL, NULL, &status);
    snprintf(name, size, "%s", status == 0 && demangled ? demangled : info.dli_sname);
    free(demangled);
    return true;
}
#endif

// ---- Attribution ---------------------------------------------------------------

// The C and C++ runtimes, passed over whole: what one allocates for a call
// (an fopen's buffer, a strdup) is put down to its caller. The system's own
// modules are not: only their allocation functions are passed over, below.
static bool alloc_trace_internal_allocator_module(const char *module)
{
    static const char *const names[] = {
        // Windows
        "ucrtbase.dll", "ucrtbased.dll", "vcruntime140.dll", "vcruntime140d.dll", "vcruntime140_1.dll",
        "vcruntime140_1d.dll", "msvcp140.dll", "msvcp140d.dll", "msvcrt.dll",
        // macOS
        "libsystem_malloc.dylib", "libc++abi.dylib", "libc++.1.dylib", "libsystem_c.dylib",
        // Linux
        "libc.so.6", "libstdc++.so.6", "libc++.so.1", "libc++abi.so.1"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(module, names[i]) == 0)
            return true;
    return false;
}

static bool alloc_trace_internal_starts(const char *name, const char *prefix)
{
    return strncmp(name, prefix, strlen(prefix)) == 0;
}

// Functions that are the allocator, in any module: the trace, the system's
// heap calls, the C runtime's pieces linked into the executable, operator
// new, the standard library's allocation helpers, and what the test named.
static bool alloc_trace_internal_allocator_function(const char *name)
{
    static const char *const prefixes[] = {"alloc_trace_", "RtlAllocateHeap", "RtlReAllocateHeap",
                                           "HeapAlloc", "HeapReAlloc", "LocalAlloc", "LocalReAlloc", "GlobalAlloc",
                                           "GlobalReAlloc", "operator new", "std::allocator<",
                                           "std::_Allocate", "std::_Default_allocate_traits", "std::__1::allocator<",
                                           "std::__libcpp_allocate", "std::__allocate_at_least", "_malloc", "_calloc",
                                           "_realloc", "_aligned_", "_recalloc", "_expand", "malloc", "calloc",
                                           "realloc", "aligned_alloc", "posix_memalign", "memalign",
                                           // The workspace's own allocator layers: SpudLib's caller allocator
                                           // and its helpers, and eqstd's containers.
                                           "spud_allocate", "spud_free", "spud_system_", "spud_eq_", "eq_allocate",
                                           "eq_resize", "eq_internal_", "eq::",
                                           // The standard library's containers: the asker is the code that grew one.
                                           "std::"};
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
        if (alloc_trace_internal_starts(name, prefixes[i]))
            return true;
    for (unsigned i = 0; i < alloc_trace_internal_skip_count; i++)
        if (alloc_trace_internal_starts(name, alloc_trace_internal_skips[i]))
            return true;
    return false;
}

// ---- The calls -----------------------------------------------------------------

// Frames in a function whose name starts with [prefix] are the allocator's,
// not its caller's: a SPUD_ALLOCATOR's allocate function, say.
static void alloc_trace_skip(const char *prefix)
{
    if (alloc_trace_internal_skip_count < ALLOC_TRACE_SKIPS)
        alloc_trace_internal_skips[alloc_trace_internal_skip_count++] = prefix;
}

// Whether this platform and build can count at all.
static bool alloc_trace_available(void)
{
    return alloc_trace_internal_platform_available();
}

static void alloc_trace_start(void)
{
    memset(alloc_trace_internal_stacks, 0, sizeof(alloc_trace_internal_stacks));
    alloc_trace_internal_unrecorded = 0;
    alloc_trace_internal_ours = 0;
    alloc_trace_internal_apart = 0;
    alloc_trace_internal_platform_start();
    alloc_trace_internal_counting = true;
}

// Stops counting, without attributing: cheap, and allocates nothing, so a
// callback on the counted thread can call it. alloc_trace_attribute() follows,
// on any thread.
static void alloc_trace_end(void)
{
    alloc_trace_internal_counting = false;
    alloc_trace_internal_platform_stop();
}

// Attributes what was counted. Looks symbols up, which may allocate.
static void alloc_trace_attribute(void)
{
    alloc_trace_internal_ours = 0;
    alloc_trace_internal_apart = 0;
    for (unsigned i = 0; i < ALLOC_TRACE_STACKS; i++)
    {
        alloc_trace_internal_stack *s = &alloc_trace_internal_stacks[i];
        if (s->count == 0)
            continue;
        bool ours = true;
        for (unsigned f = 0; f < s->depth; f++)
        {
            char module[260], symbol[512];
            bool in_ours = false;
            if (!alloc_trace_internal_module(s->pcs[f], module, sizeof(module), &in_ours))
                continue;
            if (alloc_trace_internal_allocator_module(module))
                continue;
            if (alloc_trace_internal_symbol(s->pcs[f], symbol, sizeof(symbol)) && alloc_trace_internal_allocator_function(symbol))
                continue;
            ours = in_ours;
            break;
        }
        if (ours)
            alloc_trace_internal_ours += s->count;
        else
            alloc_trace_internal_apart += s->count;
    }
    // Unrecorded allocations can't be attributed: they count as ours, so
    // they can't pass for none.
    alloc_trace_internal_ours += alloc_trace_internal_unrecorded;
}

// Stops counting and attributes what was counted: alloc_trace_end() and
// alloc_trace_attribute().
static void alloc_trace_stop(void)
{
    alloc_trace_end();
    alloc_trace_attribute();
}

// Allocations whose asker is this executable, after alloc_trace_stop().
static unsigned long alloc_trace_ours(void)
{
    return alloc_trace_internal_ours;
}

// Allocations whose asker is another module, after alloc_trace_stop().
static unsigned long alloc_trace_apart(void)
{
    return alloc_trace_internal_apart;
}

// Prints every stack counted, ours first, each as its asker and the [frames]
// of ours above it; a stack apart as its module and the nearest frame of
// ours that led to it.
static void alloc_trace_report(const char *what, unsigned frames)
{
    printf("  %s: %lu of ours, %lu apart\n", what, alloc_trace_internal_ours, alloc_trace_internal_apart);
    if (alloc_trace_internal_unrecorded)
        printf("    %lu more not recorded: the table is full\n", alloc_trace_internal_unrecorded);
    for (int pass = 0; pass < 2; pass++)
        for (unsigned i = 0; i < ALLOC_TRACE_STACKS; i++)
        {
            const alloc_trace_internal_stack *s = &alloc_trace_internal_stacks[i];
            if (s->count == 0)
                continue;
            char line[2048];
            size_t used = 0;
            bool found = false, ours = true;
            unsigned shown = 0;
            line[0] = 0;
            for (unsigned f = 0; f < s->depth && shown < frames; f++)
            {
                char module[260], symbol[512];
                bool in_ours = false;
                if (!alloc_trace_internal_module(s->pcs[f], module, sizeof(module), &in_ours))
                    continue;
                const bool named = alloc_trace_internal_symbol(s->pcs[f], symbol, sizeof(symbol));
                if (!found)
                {
                    if (alloc_trace_internal_allocator_module(module))
                        continue;
                    if (named && alloc_trace_internal_allocator_function(symbol))
                        continue;
                    found = true;
                    ours = in_ours;
                    if (!ours)
                    {
                        used += (size_t)snprintf(line + used, sizeof(line) - used, "[%s] %s", module,
                                                 named ? symbol : "?");
                        continue;
                    }
                }
                else if (!ours && !in_ours)
                    continue; // apart: on to the nearest frame of ours
                if (used < sizeof(line))
                    used += (size_t)snprintf(line + used, sizeof(line) - used, "%s%s", shown || used ? " <- " : "",
                                             named ? symbol : "?");
                shown++;
                if (!ours && shown == 1)
                    break;
            }
            if ((pass == 0) != ours)
                continue;
            printf("    %6lu  %s%s\n", s->count, ours ? "" : "apart: ", line);
        }
}

#endif
