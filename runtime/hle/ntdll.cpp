// ntdll and kernel32 pointer-encoding helpers that the CRT and PhysX resolve with GetProcAddress at startup.
#include "dish/hle.h"
#include "dish/thread.h"
#include "dish/util.h"
#include <cstring>

namespace dish {
namespace {

// Pointers are not really encoded: identity keeps every consumer (CRT atexit tables, SEH cookies) trivially correct.
uint32_t k32_EncodePointer(uint32_t p) { return p; }
uint32_t k32_DecodePointer(uint32_t p) { return p; }

uint32_t nt_RtlGetVersion(uint32_t p) {
    uint32_t sz = rd32(p);
    if (sz < 148) return 0xC000000Du;   // STATUS_INVALID_PARAMETER
    memset(guest_base + p + 4, 0, sz - 4);
    wr32(p + 4, 6);
    wr32(p + 8, 1);
    wr32(p + 12, 7601);
    wr32(p + 16, 2);
    write_wstr(p + 20, 128, u"Service Pack 1");
    return 0;
}
uint32_t nt_NtQueryInformationThread(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0xC0000002u; }     // STATUS_NOT_IMPLEMENTED
uint32_t nt_NtQueryInformationProcess(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0xC0000002u; }
uint32_t nt_NtSetInformationThread(uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
uint32_t nt_NtQuerySystemInformation(uint32_t, uint32_t, uint32_t, uint32_t) { return 0xC0000002u; }

}  // namespace

HLE("kernel32.dll", "EncodePointer", Stdcall, k32_EncodePointer);
HLE("kernel32.dll", "DecodePointer", Stdcall, k32_DecodePointer);
HLE("kernel32.dll", "EncodeSystemPointer", Stdcall, k32_EncodePointer);
HLE("kernel32.dll", "DecodeSystemPointer", Stdcall, k32_DecodePointer);
HLE("ntdll.dll", "RtlEncodePointer", Stdcall, k32_EncodePointer);
HLE("ntdll.dll", "RtlDecodePointer", Stdcall, k32_DecodePointer);
HLE("ntdll.dll", "RtlGetVersion", Stdcall, nt_RtlGetVersion);
HLE("ntdll.dll", "NtQueryInformationThread", Stdcall, nt_NtQueryInformationThread);
HLE("ntdll.dll", "NtQueryInformationProcess", Stdcall, nt_NtQueryInformationProcess);
HLE("ntdll.dll", "NtSetInformationThread", Stdcall, nt_NtSetInformationThread);
HLE("ntdll.dll", "NtQuerySystemInformation", Stdcall, nt_NtQuerySystemInformation);

}  // namespace dish
