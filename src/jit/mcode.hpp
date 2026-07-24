#ifndef LOVAX_JIT_MCODE_HPP
#define LOVAX_JIT_MCODE_HPP

// Executable memory for generated machine code (RFC-026).
//
// W^X discipline: a region is NEVER writable and executable at the same time.
// alloc() hands back a READ|WRITE page range, the assembler fills it, and
// finalize() flips it to READ|EXEC. After that the code can run but nothing can
// patch it — a W^X violation would hand an attacker who can corrupt VM memory a
// ready-made code-injection primitive.
//
// One mapping per compiled function (page-granular). That wastes the tail of a
// page for small functions; a shared code arena is a later refinement, and it
// would have to keep the W^X property (write to a staging buffer, then map).

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <unistd.h>
#endif

namespace Lovax {
namespace Jit {

inline size_t pageSize() {
#if defined(_WIN32)
    SYSTEM_INFO si; GetSystemInfo(&si); return (size_t)si.dwPageSize;
#else
    static size_t ps = (size_t)sysconf(_SC_PAGESIZE);
    return ps ? ps : 4096;
#endif
}

inline size_t roundToPage(size_t n) {
    size_t ps = pageSize();
    return ((n + ps - 1) / ps) * ps;
}

// Writable (NOT executable) code buffer. Returns null on failure.
inline void* mcodeAlloc(size_t bytes) {
    if (bytes == 0) return nullptr;
    size_t n = roundToPage(bytes);
#if defined(_WIN32)
    return VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    void* p = mmap(nullptr, n, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}

// Flip to executable and read-only. The buffer must not be written afterwards.
inline bool mcodeFinalize(void* p, size_t bytes) {
    if (!p || bytes == 0) return false;
    size_t n = roundToPage(bytes);
#if defined(_WIN32)
    DWORD old = 0;
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READ, &old)) return false;
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return true;
#else
    if (mprotect(p, n, PROT_READ | PROT_EXEC) != 0) return false;
    // x86-64 has coherent I/D caches; __builtin___clear_cache is a no-op there
    // but is required on arm64, so emit it unconditionally for portability.
    __builtin___clear_cache((char*)p, (char*)p + n);
    return true;
#endif
}

inline void mcodeRelease(void* p, size_t bytes) {
    if (!p) return;
#if defined(_WIN32)
    (void)bytes;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, roundToPage(bytes));
#endif
}

} // namespace Jit
} // namespace Lovax

#endif // LOVAX_JIT_MCODE_HPP
