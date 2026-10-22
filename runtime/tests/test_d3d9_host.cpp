// Host-side companion of test_d3d9.cpp: direct calls into DXVK for overhead comparison and the spike scene as the reference renderer.
// Kept in its own translation unit because the DXVK headers define Windows macros that collide with dish/sync.h.
#include <d3d9.h>
#include <chrono>
#include <cstdint>
#include <cstring>
#include "d3d9_scene.h"
#include "d3d9_internal.h"

using Clock = std::chrono::steady_clock;
static double ns_per(Clock::time_point t0, long n) { return std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / (double)n; }

extern "C" {
void *host_create9(unsigned v) { return dish::d3d9_load_and_create(v); }

double host_bench_ps_const(void *dev, long n) {
    auto *d = static_cast<IDirect3DDevice9 *>(dev);
    alignas(16) float v[4] = {1, 2, 3, 4};
    auto t0 = Clock::now();
    for (long i = 0; i < n; i++) { v[0] = (float)i; d->SetPixelShaderConstantF(i & 15, v, 1); }
    return ns_per(t0, n);
}
double host_bench_vs_const4(void *dev, long n) {
    auto *d = static_cast<IDirect3DDevice9 *>(dev);
    alignas(16) float m[16] = {};
    auto t0 = Clock::now();
    for (long i = 0; i < n; i++) { m[0] = (float)i; d->SetVertexShaderConstantF(0, m, 4); }
    return ns_per(t0, n);
}
double host_bench_render_state(void *dev, long n) {
    auto *d = static_cast<IDirect3DDevice9 *>(dev);
    auto t0 = Clock::now();
    for (long i = 0; i < n; i++) d->SetRenderState(D3DRS_ALPHAREF, (DWORD)(i & 255));
    return ns_per(t0, n);
}
double host_bench_sampler_state(void *dev, long n) {
    auto *d = static_cast<IDirect3DDevice9 *>(dev);
    auto t0 = Clock::now();
    for (long i = 0; i < n; i++) d->SetSamplerState(i & 7, D3DSAMP_MAXANISOTROPY, (DWORD)(1 + (i & 15)));
    return ns_per(t0, n);
}
double host_bench_set_texture(void *dev, void *tex, long n) {
    auto *d = static_cast<IDirect3DDevice9 *>(dev);
    auto t0 = Clock::now();
    for (long i = 0; i < n; i++) d->SetTexture(i & 7, static_cast<IDirect3DBaseTexture9 *>(tex));
    return ns_per(t0, n);
}
double host_bench_draw(void *dev, long n) {
    auto *d = static_cast<IDirect3DDevice9 *>(dev);
    d->BeginScene();
    auto t0 = Clock::now();
    for (long i = 0; i < n; i++) d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 4, 0, 2);
    double r = ns_per(t0, n);
    d->EndScene();
    return r;
}
}
