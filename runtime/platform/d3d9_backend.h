// The d3d9 backend registry (see docs/runtime_d3d9_backend.md). Depends only on the host D3D9 headers so that Objective-C++ backends can include it.
#pragma once
#include <d3d9.h>

namespace dish {

struct D3d9Backend {
    const char *name;                          // "dxvk", "metal"
    IDirect3D9 *(*create)(UINT sdk_version);   // main thread; nullptr when this backend cannot run on this machine (reason logged)
    void (*shutdown)();                        // optional; process teardown
    const char *(*describe)();                 // optional; library path / GPU name for logs
};
void d3d9_register_backend(const D3d9Backend *b);   // static-initialiser friendly; the platform default is chosen at first use (Apple: "metal", else the first registered)
const D3d9Backend *d3d9_active_backend();           // after selection (DISH_D3D9_BACKEND=name overrides); nullptr when none

}  // namespace dish
