// Guest address space: one 4 GiB reservation, VirtualAlloc-style reserve/commit/protect at 4 KiB guest-page
// granularity (host protections are applied at host-page granularity, so a 16 KiB-page host over-commits).
#pragma once
#include <cstdint>
#include "guest_abi.h"

namespace dish {

enum : uint32_t {
    MEM_COMMIT = 0x1000, MEM_RESERVE = 0x2000, MEM_DECOMMIT = 0x4000, MEM_RELEASE = 0x8000, MEM_FREE = 0x10000,
    MEM_PRIVATE = 0x20000, MEM_MAPPED = 0x40000, MEM_RESET = 0x80000, MEM_TOP_DOWN = 0x100000,
    MEM_WRITE_WATCH = 0x200000, MEM_PHYSICAL = 0x400000, MEM_LARGE_PAGES = 0x20000000, MEM_IMAGE = 0x1000000,
};
enum : uint32_t {
    PAGE_NOACCESS = 0x01, PAGE_READONLY = 0x02, PAGE_READWRITE = 0x04, PAGE_WRITECOPY = 0x08,
    PAGE_EXECUTE = 0x10, PAGE_EXECUTE_READ = 0x20, PAGE_EXECUTE_READWRITE = 0x40, PAGE_EXECUTE_WRITECOPY = 0x80,
    PAGE_GUARD = 0x100, PAGE_NOCACHE = 0x200, PAGE_WRITECOMBINE = 0x400,
};
enum : uint32_t {
    ERR_SUCCESS = 0, ERR_FILE_NOT_FOUND = 2, ERR_PATH_NOT_FOUND = 3, ERR_ACCESS_DENIED = 5, ERR_INVALID_HANDLE = 6,
    ERR_NOT_ENOUGH_MEMORY = 8, ERR_INVALID_PARAMETER = 87, ERR_INVALID_ADDRESS = 487,
};

constexpr uint32_t kGuestPage = 4096;
constexpr uint32_t kAllocGranularity = 65536;

struct VmQuery {
    uint32_t base;        // BaseAddress (page-aligned start of the run)
    uint32_t alloc_base;  // AllocationBase (0 when free)
    uint32_t alloc_prot;  // AllocationProtect
    uint32_t size;        // RegionSize
    uint32_t state;       // MEM_COMMIT / MEM_RESERVE / MEM_FREE
    uint32_t prot;        // Protect
    uint32_t type;        // MEM_PRIVATE / MEM_MAPPED / MEM_IMAGE (0 when free)
};

// Reserve the guest region, set guest_base, mark the null guard and the top guard/thunk range permanently reserved.
// space_bytes == 0 tries 4 GiB first, then 3.5 / 3.0 / 2.5 GiB (iOS may refuse the full 4 GiB); a non-zero value caps the
// first attempt. DISH_GUEST_SPACE_MB in the environment overrides it (to exercise the reduced layouts on Linux).
bool vm_init(uint64_t space_bytes = 0);
// Exclusive upper bound of allocatable guest space: 0xFFEF0000 with the full 4 GiB, space-64K in a reduced layout.
uint32_t vm_user_max();
// Bytes of guest address space actually reserved (4 GiB, or less in a reduced layout).
uint64_t vm_space_bytes();
void vm_shutdown();
bool vm_ready();
uint32_t vm_host_page_size();

// VirtualAlloc. Returns the base address or 0 and sets *err.
uint32_t vm_alloc(uint32_t addr, uint32_t size, uint32_t type, uint32_t prot, uint32_t *err = nullptr);
// VirtualFree
bool vm_free(uint32_t addr, uint32_t size, uint32_t type, uint32_t *err = nullptr);
bool vm_protect(uint32_t addr, uint32_t size, uint32_t prot, uint32_t *old_prot, uint32_t *err = nullptr);
bool vm_query(uint32_t addr, VmQuery *out);
// Reserve [base, base+size) for a module image so nothing else is placed there; vm_map_image later commits it.
// runtime_init does this for every module BEFORE any thread (stack/TEB) is created.
bool vm_reserve_image(uint32_t base, uint32_t size, uint32_t *err = nullptr);
// Loader: map [base, base+size) as an image allocation and commit it read/write; later vm_protect sections.
bool vm_map_image(uint32_t base, uint32_t size, uint32_t *err = nullptr);
// File view: reserve a region (MEM_MAPPED) and map `fd` at `offset` over it. Returns the base or 0.
uint32_t vm_map_view(int fd, uint64_t offset, uint32_t size, uint32_t prot, uint32_t addr_hint, uint32_t *err = nullptr);
bool vm_unmap_view(uint32_t base);
// True when [addr, addr+size) is entirely committed and accessible with at least the given access.
bool vm_is_committed(uint32_t addr, uint32_t size);
// Debug: total committed bytes.
uint64_t vm_committed_bytes();
// Refuse MEM_COMMIT beyond this many committed guest bytes (ERROR_NOT_ENOUGH_MEMORY); 0 = unlimited (default).
void vm_set_commit_limit(uint64_t bytes);

inline uint8_t *gp(uint32_t a) { return guest_base + a; }
template <class T> inline T &gref(uint32_t a) { return *reinterpret_cast<T *>(guest_base + a); }
inline uint32_t g2h_off(const void *p) { return (uint32_t)((const uint8_t *)p - guest_base); }
inline bool is_guest_host_ptr(const void *p) { return (const uint8_t *)p >= guest_base && (const uint8_t *)p < guest_base + (1ull << 32); }

}  // namespace dish
