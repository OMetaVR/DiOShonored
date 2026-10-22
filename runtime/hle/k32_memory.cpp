// kernel32: virtual memory, heaps, Global/Local allocation, memory status.
#include "dish/heap.h"
#include "dish/hle.h"
#include "dish/thread.h"
#include "dish/util.h"
#include "dish/vm.h"
#include <cstring>
#include <unistd.h>

namespace dish {
namespace {

uint32_t k32_VirtualAlloc(uint32_t addr, uint32_t size, uint32_t type, uint32_t prot) {
    uint32_t err = 0;
    uint32_t r = vm_alloc(addr, size, type, prot, &err);
    if (!r) set_last_error(err);
    LOGT(LC_MEM, "VirtualAlloc(%08x, %x, %x, %x) = %08x", addr, size, type, prot, r);
    return r;
}
uint32_t k32_VirtualAllocEx(uint32_t proc, uint32_t addr, uint32_t size, uint32_t type, uint32_t prot) {
    if (proc != HANDLE_CUR_PROCESS) { set_last_error(ERR_INVALID_HANDLE); return 0; }
    return k32_VirtualAlloc(addr, size, type, prot);
}
uint32_t k32_VirtualFree(uint32_t addr, uint32_t size, uint32_t type) {
    uint32_t err = 0;
    bool ok = vm_free(addr, size, type, &err);
    if (!ok) set_last_error(err);
    LOGT(LC_MEM, "VirtualFree(%08x, %x, %x) = %d", addr, size, type, ok);
    return ok;
}
uint32_t k32_VirtualFreeEx(uint32_t proc, uint32_t addr, uint32_t size, uint32_t type) {
    return proc == HANDLE_CUR_PROCESS ? k32_VirtualFree(addr, size, type) : 0;
}
uint32_t k32_VirtualProtect(uint32_t addr, uint32_t size, uint32_t prot, uint32_t old_ptr) {
    uint32_t err = 0, old = 0;
    if (!old_ptr) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    if (!vm_protect(addr, size, prot, &old, &err)) { set_last_error(err); return 0; }
    wr32(old_ptr, old);
    return 1;
}
uint32_t k32_VirtualProtectEx(uint32_t proc, uint32_t addr, uint32_t size, uint32_t prot, uint32_t old_ptr) {
    return proc == HANDLE_CUR_PROCESS ? k32_VirtualProtect(addr, size, prot, old_ptr) : 0;
}
uint32_t k32_VirtualQuery(uint32_t addr, uint32_t out, uint32_t len) {
    if (len < 28) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    VmQuery q;
    if (addr >= vm_user_max() || !vm_query(addr, &q)) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    wr32(out + 0, q.base);
    wr32(out + 4, q.alloc_base);
    wr32(out + 8, q.alloc_prot);
    wr32(out + 12, q.size);
    wr32(out + 16, q.state);
    wr32(out + 20, q.prot);
    wr32(out + 24, q.type);
    return 28;
}
uint32_t k32_VirtualQueryEx(uint32_t proc, uint32_t addr, uint32_t out, uint32_t len) {
    return proc == HANDLE_CUR_PROCESS ? k32_VirtualQuery(addr, out, len) : 0;
}
uint32_t k32_VirtualLock(uint32_t, uint32_t) { return 1; }
uint32_t k32_VirtualUnlock(uint32_t, uint32_t) { return 1; }

// ---- heaps
uint32_t k32_GetProcessHeap() { return heap_process(); }
uint32_t k32_HeapCreate(uint32_t flags, uint32_t initial, uint32_t max) {
    uint32_t h = heap_create(flags, initial, max);
    if (!h) set_last_error(ERR_NOT_ENOUGH_MEMORY);
    return h;
}
uint32_t k32_HeapDestroy(uint32_t h) { return heap_destroy(h); }
uint32_t k32_HeapAlloc(uint32_t h, uint32_t flags, uint32_t size) {
    uint32_t r = heap_alloc(h, flags, size);
    if (__builtin_expect(!r, 0)) LOGW(LC_HLE, "HeapAlloc(%08x, %x, %u) failed (error %u)", h, flags, size, get_last_error());
    return r;
}
uint32_t k32_HeapFree(uint32_t h, uint32_t flags, uint32_t p) { return heap_free(h, flags, p); }
uint32_t k32_HeapReAlloc(uint32_t h, uint32_t flags, uint32_t p, uint32_t size) { return heap_realloc(h, flags, p, size); }
uint32_t k32_HeapSize(uint32_t h, uint32_t flags, uint32_t p) { return heap_size(h, flags, p); }
uint32_t k32_HeapValidate(uint32_t h, uint32_t, uint32_t p) { return heap_validate(h, p); }
uint32_t k32_HeapCompact(uint32_t, uint32_t) { return 0x10000; }
uint32_t k32_HeapLock(uint32_t) { return 1; }
uint32_t k32_HeapUnlock(uint32_t) { return 1; }
uint32_t k32_HeapWalk(uint32_t, uint32_t) { set_last_error(50); return 0; }   // ERROR_NOT_SUPPORTED
uint32_t k32_HeapSetInformation(uint32_t, uint32_t, uint32_t, uint32_t) { return 1; }
uint32_t k32_HeapQueryInformation(uint32_t, uint32_t cls, uint32_t buf, uint32_t len, uint32_t ret) {
    if (cls == 0 && len >= 4) { wr32(buf, 0); if (ret) wr32(ret, 4); return 1; }   // standard heap
    set_last_error(ERR_INVALID_PARAMETER);
    return 0;
}

// ---- Global / Local (moveable memory is not relocated: the handle is the pointer)
constexpr uint32_t GMEM_ZEROINIT = 0x40;
uint32_t gl_alloc(uint32_t flags, uint32_t size) { return heap_alloc(heap_process(), (flags & GMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0, size); }
uint32_t gl_realloc(uint32_t p, uint32_t size, uint32_t flags) {
    if (!p) return gl_alloc(flags, size);
    return heap_realloc(heap_process(), (flags & GMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0, p, size);
}
uint32_t gl_free(uint32_t p) { return heap_free(heap_process(), 0, p) ? 0 : p; }
uint32_t gl_size(uint32_t p) { uint32_t s = heap_size(heap_process(), 0, p); return s == 0xFFFFFFFFu ? 0 : s; }
uint32_t k32_GlobalAlloc(uint32_t f, uint32_t s) { return gl_alloc(f, s); }
uint32_t k32_GlobalReAlloc(uint32_t p, uint32_t s, uint32_t f) { return gl_realloc(p, s, f); }
uint32_t k32_GlobalFree(uint32_t p) { return gl_free(p); }
uint32_t k32_GlobalSize(uint32_t p) { return gl_size(p); }
uint32_t k32_GlobalLock(uint32_t p) { return p; }
uint32_t k32_GlobalUnlock(uint32_t) { return 0; }
uint32_t k32_GlobalHandle(uint32_t p) { return p; }
uint32_t k32_LocalAlloc(uint32_t f, uint32_t s) { return gl_alloc(f, s); }
uint32_t k32_LocalReAlloc(uint32_t p, uint32_t s, uint32_t f) { return gl_realloc(p, s, f); }
uint32_t k32_LocalFree(uint32_t p) { return gl_free(p); }
uint32_t k32_LocalSize(uint32_t p) { return gl_size(p); }
uint32_t k32_LocalLock(uint32_t p) { return p; }
uint32_t k32_LocalUnlock(uint32_t) { return 0; }

uint32_t k32_GlobalMemoryStatusEx(uint32_t p) {
    if (rd32(p) < 64) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    uint64_t pages = (uint64_t)sysconf(_SC_PHYS_PAGES), psz = (uint64_t)sysconf(_SC_PAGESIZE);
    uint64_t phys = pages * psz;
    if (phys > (8ull << 30)) phys = 8ull << 30;   // what a typical 2012 gaming PC would report; the engine sizes pools from it
    uint64_t avail = phys / 2;
    uint64_t virt_total = 0xFFE00000ull, virt_avail = virt_total - vm_committed_bytes();
    wr32(p + 4, 50);               // dwMemoryLoad
    wr64(p + 8, phys);             // ullTotalPhys
    wr64(p + 16, avail);           // ullAvailPhys
    wr64(p + 24, phys + (phys / 2));   // ullTotalPageFile
    wr64(p + 32, avail + (phys / 2));  // ullAvailPageFile
    wr64(p + 40, virt_total);      // ullTotalVirtual
    wr64(p + 48, virt_avail);      // ullAvailVirtual
    wr64(p + 56, 0);
    return 1;
}
void k32_GlobalMemoryStatus(uint32_t p) {
    uint64_t pages = (uint64_t)sysconf(_SC_PHYS_PAGES), psz = (uint64_t)sysconf(_SC_PAGESIZE);
    uint64_t phys = std::min<uint64_t>(pages * psz, 2000ull << 20);
    wr32(p + 4, 50);
    wr32(p + 8, (uint32_t)phys);
    wr32(p + 12, (uint32_t)(phys / 2));
    wr32(p + 16, (uint32_t)phys);
    wr32(p + 20, (uint32_t)(phys / 2));
    wr32(p + 24, 0x7FFE0000u);
    wr32(p + 28, (uint32_t)(0x7FFE0000u - vm_committed_bytes()));
}
uint32_t k32_IsBadReadPtr(uint32_t p, uint32_t n) { return n && !vm_is_committed(p, n); }
uint32_t k32_IsBadWritePtr(uint32_t p, uint32_t n) { return n && !vm_is_committed(p, n); }
uint32_t k32_IsBadCodePtr(uint32_t p) { return !vm_is_committed(p, 1); }
uint32_t k32_IsBadStringPtrW(uint32_t p, uint32_t) { return !vm_is_committed(p, 2); }
uint32_t k32_IsBadStringPtrA(uint32_t p, uint32_t) { return !vm_is_committed(p, 1); }
uint32_t k32_GetLargePageMinimum() { return 0x200000; }
uint32_t k32_GetProcessWorkingSetSize(uint32_t, uint32_t pmin, uint32_t pmax) { wr32(pmin, 0x200000); wr32(pmax, 0x1400000); return 1; }
uint32_t k32_SetProcessWorkingSetSize(uint32_t, uint32_t, uint32_t) { return 1; }

}  // namespace

HLE("kernel32.dll", "VirtualAlloc", Stdcall, k32_VirtualAlloc);
HLE("kernel32.dll", "VirtualAllocEx", Stdcall, k32_VirtualAllocEx);
HLE("kernel32.dll", "VirtualFree", Stdcall, k32_VirtualFree);
HLE("kernel32.dll", "VirtualFreeEx", Stdcall, k32_VirtualFreeEx);
HLE("kernel32.dll", "VirtualProtect", Stdcall, k32_VirtualProtect);
HLE("kernel32.dll", "VirtualProtectEx", Stdcall, k32_VirtualProtectEx);
HLE("kernel32.dll", "VirtualQuery", Stdcall, k32_VirtualQuery);
HLE("kernel32.dll", "VirtualQueryEx", Stdcall, k32_VirtualQueryEx);
HLE("kernel32.dll", "VirtualLock", Stdcall, k32_VirtualLock);
HLE("kernel32.dll", "VirtualUnlock", Stdcall, k32_VirtualUnlock);
HLE("kernel32.dll", "GetProcessHeap", Stdcall, k32_GetProcessHeap);
HLE("kernel32.dll", "HeapCreate", Stdcall, k32_HeapCreate);
HLE("kernel32.dll", "HeapDestroy", Stdcall, k32_HeapDestroy);
HLE("kernel32.dll", "HeapAlloc", Stdcall, k32_HeapAlloc);
HLE("kernel32.dll", "HeapFree", Stdcall, k32_HeapFree);
HLE("kernel32.dll", "HeapReAlloc", Stdcall, k32_HeapReAlloc);
HLE("kernel32.dll", "HeapSize", Stdcall, k32_HeapSize);
HLE("kernel32.dll", "HeapValidate", Stdcall, k32_HeapValidate);
HLE("kernel32.dll", "HeapCompact", Stdcall, k32_HeapCompact);
HLE("kernel32.dll", "HeapLock", Stdcall, k32_HeapLock);
HLE("kernel32.dll", "HeapUnlock", Stdcall, k32_HeapUnlock);
HLE("kernel32.dll", "HeapWalk", Stdcall, k32_HeapWalk);
HLE("kernel32.dll", "HeapSetInformation", Stdcall, k32_HeapSetInformation);
HLE("kernel32.dll", "HeapQueryInformation", Stdcall, k32_HeapQueryInformation);
HLE("kernel32.dll", "GlobalAlloc", Stdcall, k32_GlobalAlloc);
HLE("kernel32.dll", "GlobalReAlloc", Stdcall, k32_GlobalReAlloc);
HLE("kernel32.dll", "GlobalFree", Stdcall, k32_GlobalFree);
HLE("kernel32.dll", "GlobalSize", Stdcall, k32_GlobalSize);
HLE("kernel32.dll", "GlobalLock", Stdcall, k32_GlobalLock);
HLE("kernel32.dll", "GlobalUnlock", Stdcall, k32_GlobalUnlock);
HLE("kernel32.dll", "GlobalHandle", Stdcall, k32_GlobalHandle);
HLE("kernel32.dll", "LocalAlloc", Stdcall, k32_LocalAlloc);
HLE("kernel32.dll", "LocalReAlloc", Stdcall, k32_LocalReAlloc);
HLE("kernel32.dll", "LocalFree", Stdcall, k32_LocalFree);
HLE("kernel32.dll", "LocalSize", Stdcall, k32_LocalSize);
HLE("kernel32.dll", "LocalLock", Stdcall, k32_LocalLock);
HLE("kernel32.dll", "LocalUnlock", Stdcall, k32_LocalUnlock);
HLE("kernel32.dll", "GlobalMemoryStatusEx", Stdcall, k32_GlobalMemoryStatusEx);
HLE("kernel32.dll", "GlobalMemoryStatus", Stdcall, k32_GlobalMemoryStatus);
HLE("kernel32.dll", "IsBadReadPtr", Stdcall, k32_IsBadReadPtr);
HLE("kernel32.dll", "IsBadWritePtr", Stdcall, k32_IsBadWritePtr);
HLE("kernel32.dll", "IsBadCodePtr", Stdcall, k32_IsBadCodePtr);
HLE("kernel32.dll", "IsBadStringPtrW", Stdcall, k32_IsBadStringPtrW);
HLE("kernel32.dll", "IsBadStringPtrA", Stdcall, k32_IsBadStringPtrA);
HLE("kernel32.dll", "GetLargePageMinimum", Stdcall, k32_GetLargePageMinimum);
HLE("kernel32.dll", "GetProcessWorkingSetSize", Stdcall, k32_GetProcessWorkingSetSize);
HLE("kernel32.dll", "SetProcessWorkingSetSize", Stdcall, k32_SetProcessWorkingSetSize);

}  // namespace dish
