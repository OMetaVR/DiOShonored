// Direct D3D9 -> Metal backend: shared declarations. Objective-C++ (compiled for Apple targets only).
//
// The backend implements the host-side D3D9 COM interfaces of d3d9.h (the seam documented in docs/runtime_d3d9_backend.md): the guest-facing thunk
// layer calls these objects exactly as it calls DXVK's. Everything runs on the guest render thread except device creation / Reset (main thread).
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <MetalFX/MetalFX.h>
#include <dispatch/dispatch.h>
// NOTE: every Objective-C header must be included before the D3D9 headers: they `#define interface struct`, which breaks `@interface`.
// DXVK's windows_base.h declares `typedef INT BOOL;`, which clashes with Objective-C's BOOL: rename it while the D3D9 headers are read
// (the headers' own signatures use WINBOOL, so nothing else changes).
#define BOOL WIN_BOOL_
#include <d3d9.h>
#undef BOOL
#include <SDL3/SDL.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dish/log.h"
#include "dish/memacct.h"
#include "shader_xlate.h"
#include "texcodec.h"

namespace dish {
namespace mtl {

class MtlDevice;

// ---- logging / stubs ---------------------------------------------------------------------------------------------------------
HRESULT stub_called(const char *what);   // logs the first calls of each unimplemented method, returns D3DERR_NOTAVAILABLE

// ---- intrusive reference to a COM object (AddRef/Release) ---------------------------------------------------------------------
template <class T> class Ref {
public:
    Ref() = default;
    Ref(T *p) : p_(p) { if (p_) p_->AddRef(); }
    Ref(const Ref &o) : p_(o.p_) { if (p_) p_->AddRef(); }
    Ref(Ref &&o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ~Ref() { if (p_) p_->Release(); }
    Ref &operator=(const Ref &o) { if (o.p_) o.p_->AddRef(); if (p_) p_->Release(); p_ = o.p_; return *this; }
    Ref &operator=(Ref &&o) noexcept { if (this != &o) { if (p_) p_->Release(); p_ = o.p_; o.p_ = nullptr; } return *this; }
    T *get() const { return p_; }
    T *operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    void reset() { if (p_) p_->Release(); p_ = nullptr; }
    // adopt: take over an existing reference without AddRef
    static Ref adopt(T *p) { Ref r; r.p_ = p; return r; }
private:
    T *p_ = nullptr;
};

// ---- IUnknown mixin -----------------------------------------------------------------------------------------------------------
template <class... Iids> inline bool iid_matches(REFIID riid) {
    return ((riid == __uuidof(Iids)) || ...);
}
// Base = Stub_I... (every method unimplemented), Iids = interfaces QueryInterface answers (besides IUnknown).
template <class Base, class... Iids> struct Com : public Base {
    std::atomic<ULONG> rc_{1};
    virtual ~Com() = default;
    STDMETHOD(QueryInterface)(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || iid_matches<Iids...>(riid)) { *ppv = static_cast<Base *>(this); AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++rc_; }
    STDMETHOD_(ULONG, Release)() override {
        ULONG r = --rc_;
        if (r == 0) destroy();
        return r;
    }
    virtual void destroy() { delete this; }
};

// ---- formats ------------------------------------------------------------------------------------------------------------------
enum class Conv : uint8_t { None, Bc1, Bc2, Bc3, Bc4, Bc5, A4R4G4B4, X4R4G4B4, A8R3G3B2, R3G3B2 };

struct FormatInfo {
    D3DFORMAT d3d;
    MTLPixelFormat mtl;        // format of the GPU texture (for BC: the decoded format; ASTC when the transcode path is enabled)
    uint8_t bpp;               // D3D bytes per texel (per 4x4 block for compressed formats)
    bool block;                // block compressed
    bool depth, stencil;
    Conv conv;
    uint8_t swz;               // 0 none, 1 alpha forced to one, 2 L8 (r,r,r,1), 3 A8L8 (r,r,r,g)
    bool rt_ok;                // usable as colour render target
    bool filter_ok;            // linear filtering
};
const FormatInfo *format_info(D3DFORMAT f);
bool format_is_bc(D3DFORMAT f);
uint32_t level_pitch(const FormatInfo *fi, uint32_t w);              // D3D row pitch of a level (bytes per row, or per block row)
uint32_t level_rows(const FormatInfo *fi, uint32_t h);               // rows in the D3D layout (block rows for BC)
uint32_t level_bytes(const FormatInfo *fi, uint32_t w, uint32_t h);
inline uint32_t mip_dim(uint32_t v, uint32_t level) { v >>= level; return v ? v : 1; }
uint32_t full_mip_count(uint32_t w, uint32_t h, uint32_t d = 1);

// ---- statistics (read by the on-device acceptance app) ------------------------------------------------------------------------
struct MtlStats {
    std::atomic<uint64_t> draws{0}, clears{0}, pipelines_created{0}, pipeline_archive_hits{0}, shaders_created{0}, shader_failures{0};
    std::atomic<uint64_t> uploads{0}, upload_bytes{0}, transcode_blocks{0}, transcode_in_bytes{0}, decode_in_bytes{0};
    std::atomic<uint64_t> copy_tokens_out{0}, copy_tokens_used{0};
    std::atomic<uint64_t> ring_bytes{0}, frames{0}, readbacks{0}, command_buffers{0};
    std::atomic<uint64_t> shader_translate_ns{0}, shader_compile_ns{0}, pipeline_create_ns{0}, transcode_ns{0}, decode_ns{0};
    std::atomic<uint64_t> gpu_bytes_textures{0}, gpu_bytes_buffers{0};
    std::atomic<int64_t> shaders_pending{0};           // created, not yet compiled
    std::atomic<uint64_t> gpu_busy_ns{0};              // sum of command buffer GPU execution times
    std::atomic<uint64_t> draws_skipped_pso{0};        // draws skipped because their pipeline was still being built
    std::atomic<uint64_t> draws_skipped_shader{0};     // draws skipped because a shader was still compiling (async mode)
};
MtlStats &stats();

// Runtime options (environment): DISH_METAL_ASTC=0 (DXT -> RGBA8 instead of ASTC), DISH_METAL_TEX_SHADOW=1 (CPU copies of DXT levels), DISH_METAL_UPSCALE=metalfx|bilinear|off, DISH_METAL_INTERNAL_HEIGHT=720,
// DISH_METAL_VALIDATE=1 (extra logging), DISH_METAL_CACHE_DIR=<dir> (pipeline archive and transcode cache; default Library/Caches/DishMetal).
struct Options {
    bool sync_shaders = false;       // DISH_METAL_SYNC_SHADERS=1: draws wait for their shaders instead of being skipped while they compile
    bool astc = true;                // DISH_METAL_ASTC=0: decode DXT to RGBA8 instead of transcoding to ASTC 4x4
    bool tex_shadow = false;         // DISH_METAL_TEX_SHADOW=1: keep a CPU copy of every DXT level (else READONLY locks hand out copy tokens)
    int upscale = 1;                 // 0 off (bilinear/nearest blit), 1 MetalFX spatial when available, else blit
    int internal_height = 720;
    int fps_cap = 0;                 // DISH_FRAME_RATE_CAP: 0 = auto (60, 30 while thermally serious), 30, 60
    bool verbose = false;
    std::string cache_dir;
};
const Options &options();

}  // namespace mtl
}  // namespace dish
