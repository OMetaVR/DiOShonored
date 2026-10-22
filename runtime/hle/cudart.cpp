// cudart32_30_9.dll (statically imported by PhysXCore): "no CUDA device". PhysX 2.8 then simulates on the CPU, which is what the
// game does on any non-NVIDIA PC. Every entry is __stdcall (CUDARTAPI); the argument sizes are checked against PhysXCore's call sites
// (runtime/tools/manual_sigs.txt). Kernel registration from PhysXCore's static initialisers is accepted and ignored.
#include "dish/heap.h"
#include "dish/hle.h"
#include "dish/log.h"
#include "dish/util.h"
#include <cstring>
#include <mutex>

namespace dish {
namespace {

constexpr uint32_t kSuccess = 0, kErrorNoDevice = 38;

uint32_t no_device() {
    static bool logged = false;
    if (!logged) { logged = true; LOGI(LC_HLE, "cudart: no CUDA device (PhysX runs on the CPU)"); }
    return kErrorNoDevice;
}

// ---- registration (static initialisers of the CUDA kernels' host stubs)
uint32_t cu_RegisterFatBinary(uint32_t) {
    static uint32_t handle = 0;   // void **: a valid, never-dereferenced-by-us guest cell
    static std::once_flag once;
    std::call_once(once, [] { handle = heap_alloc(heap_process(), HEAP_ZERO_MEMORY, 16); });
    return handle;
}
void cu_UnregisterFatBinary(uint32_t) {}
void cu_RegisterFunction(uint32_t, uint32_t, uint32_t, uint32_t, int32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
void cu_RegisterVar(uint32_t, uint32_t, uint32_t, uint32_t, int32_t, int32_t, int32_t, int32_t) {}
void cu_RegisterShared(uint32_t, uint32_t) {}
void cu_RegisterTexture(uint32_t, uint32_t, uint32_t, uint32_t, int32_t, int32_t, int32_t) {}

// ---- device / memory / launch: all fail with cudaErrorNoDevice; out-pointers are cleared
uint32_t cu_GetDeviceProperties(uint32_t, int32_t) { return no_device(); }
uint32_t cu_GetLastError() { return no_device(); }
uint32_t cu_GetErrorString(uint32_t err) {
    static uint32_t no_dev = 0, other = 0;
    static std::once_flag once;
    std::call_once(once, [] {
        const char a[] = "no CUDA-capable device is detected", b[] = "CUDA is not available";
        no_dev = heap_alloc(heap_process(), 0, sizeof a);
        memcpy(guest_base + no_dev, a, sizeof a);
        other = heap_alloc(heap_process(), 0, sizeof b);
        memcpy(guest_base + other, b, sizeof b);
    });
    return err == kErrorNoDevice ? no_dev : other;
}
uint32_t cu_Malloc(uint32_t pp, uint32_t) { if (pp) wr32(pp, 0); return no_device(); }
uint32_t cu_MallocHost(uint32_t pp, uint32_t) { if (pp) wr32(pp, 0); return no_device(); }
uint32_t cu_Free(uint32_t) { return no_device(); }
uint32_t cu_FreeHost(uint32_t) { return no_device(); }
uint32_t cu_Memcpy(uint32_t, uint32_t, uint32_t, int32_t) { return no_device(); }
uint32_t cu_MemcpyAsync(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) { return no_device(); }
uint32_t cu_MemcpyToSymbolAsync(uint32_t, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) { return no_device(); }
uint32_t cu_Memset(uint32_t, int32_t, uint32_t) { return no_device(); }
uint32_t cu_BindTexture(uint32_t off, uint32_t, uint32_t, uint32_t, uint32_t) { if (off) wr32(off, 0); return no_device(); }
uint32_t cu_UnbindTexture(uint32_t) { return no_device(); }
// struct cudaChannelFormatDesc {int x, y, z, w; enum f;} returned through the hidden pointer (popped by the callee under stdcall)
uint32_t cu_CreateChannelDesc(uint32_t ret, int32_t x, int32_t y, int32_t z, int32_t w, int32_t f) {
    int32_t v[5] = {x, y, z, w, f};
    if (ret) memcpy(guest_base + ret, v, sizeof v);
    return ret;
}
uint32_t cu_ConfigureCall(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return no_device(); }  // dim3, dim3, shmem, stream
uint32_t cu_SetupArgument(uint32_t, uint32_t, uint32_t) { return no_device(); }
uint32_t cu_Launch(uint32_t) { return no_device(); }
uint32_t cu_StreamCreate(uint32_t ps) { if (ps) wr32(ps, 0); return no_device(); }
uint32_t cu_StreamDestroy(uint32_t) { return no_device(); }
uint32_t cu_ThreadSynchronize() { return no_device(); }
uint32_t cu_EventCreate(uint32_t pe) { if (pe) wr32(pe, 0); return no_device(); }
uint32_t cu_EventRecord(uint32_t, uint32_t) { return no_device(); }
uint32_t cu_EventQuery(uint32_t) { return no_device(); }
uint32_t cu_EventSynchronize(uint32_t) { return no_device(); }
uint32_t cu_EventElapsedTime(uint32_t pms, uint32_t, uint32_t) { if (pms) wr32(pms, 0); return no_device(); }

}  // namespace

#define CU(name, fn) HLE("cudart32_30_9.dll", name, Stdcall, fn)
CU("__cudaRegisterFatBinary", cu_RegisterFatBinary);
CU("__cudaUnregisterFatBinary", cu_UnregisterFatBinary);
CU("__cudaRegisterFunction", cu_RegisterFunction);
CU("__cudaRegisterVar", cu_RegisterVar);
CU("__cudaRegisterShared", cu_RegisterShared);
CU("__cudaRegisterTexture", cu_RegisterTexture);
CU("cudaGetDeviceProperties", cu_GetDeviceProperties);
CU("cudaGetLastError", cu_GetLastError);
CU("cudaGetErrorString", cu_GetErrorString);
CU("cudaMalloc", cu_Malloc);
CU("cudaMallocHost", cu_MallocHost);
CU("cudaFree", cu_Free);
CU("cudaFreeHost", cu_FreeHost);
CU("cudaMemcpy", cu_Memcpy);
CU("cudaMemcpyAsync", cu_MemcpyAsync);
CU("cudaMemcpyToSymbolAsync", cu_MemcpyToSymbolAsync);
CU("cudaMemset", cu_Memset);
CU("cudaBindTexture", cu_BindTexture);
CU("cudaUnbindTexture", cu_UnbindTexture);
CU("cudaCreateChannelDesc", cu_CreateChannelDesc);
CU("cudaConfigureCall", cu_ConfigureCall);
CU("cudaSetupArgument", cu_SetupArgument);
CU("cudaLaunch", cu_Launch);
CU("cudaStreamCreate", cu_StreamCreate);
CU("cudaStreamDestroy", cu_StreamDestroy);
CU("cudaThreadSynchronize", cu_ThreadSynchronize);
CU("cudaEventCreate", cu_EventCreate);
CU("cudaEventRecord", cu_EventRecord);
CU("cudaEventQuery", cu_EventQuery);
CU("cudaEventSynchronize", cu_EventSynchronize);
CU("cudaEventElapsedTime", cu_EventElapsedTime);

}  // namespace dish
