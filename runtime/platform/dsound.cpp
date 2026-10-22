// DirectSound over an SDL3 audio stream: IDirectSound/IDirectSound8 + IDirectSoundBuffer/Buffer8 as guest COM objects.
// Secondary buffers live in guest memory (heap) so Lock() hands the game a direct pointer; the SDL audio callback mixes the playing
// buffers (8/16/24/32-bit PCM and float, any channel count and sample rate, volume and pan) to 48 kHz float stereo and advances the play cursor.
#include "dish/call.h"
#include "dish/com.h"
#include "dish/heap.h"
#include "dish/hle.h"
#include "dish/lifecycle.h"
#include "dish/log.h"
#include "dish/mainthread.h"
#include "dish/platform.h"
#include "dish/thread.h"
#include "dish/util.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace dish {
namespace {

constexpr uint32_t DS_OK = 0, DSERR_INVALIDPARAM = 0x80070057u, DSERR_NOINTERFACE = 0x80004002u, DSERR_INVALIDCALL = 0x88780032u,
                   DSERR_BADFORMAT = 0x88780064u, DSERR_NODRIVER = 0x88780078u, DSERR_UNSUPPORTED = 0x80004001u, DSERR_OUTOFMEMORY = 0x8007000Eu;
constexpr uint32_t DSBCAPS_PRIMARYBUFFER = 1, DSBPLAY_LOOPING = 1, DSBSTATUS_PLAYING = 1, DSBSTATUS_LOOPING = 4, DSBLOCK_FROMWRITECURSOR = 1, DSBLOCK_ENTIREBUFFER = 2;
constexpr int kOutRate = 48000;

struct Guid { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; };
bool guid_eq(uint32_t g, const Guid &x) { return memcmp(guest_base + g, &x, 16) == 0; }
const Guid IID_IUnknown_{0, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
const Guid IID_IDirectSound_{0x279AFA83, 0x4981, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
const Guid IID_IDirectSound8_{0xC50A7E93, 0xF395, 0x4834, {0x9E, 0xF6, 0x7F, 0xA9, 0x9D, 0xE5, 0x09, 0x66}};
const Guid IID_IDirectSoundBuffer_{0x279AFA85, 0x4981, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
const Guid IID_IDirectSoundBuffer8_{0x6825A449, 0x7524, 0x4D82, {0x92, 0x0F, 0x50, 0xE3, 0x6A, 0xB3, 0xAB, 0x1E}};

struct Fmt {
    uint16_t tag = 1, channels = 2;
    uint32_t rate = 44100;
    uint16_t block = 4, bits = 16;
    bool is_float = false;
};

struct Buffer {
    bool primary = false;
    uint32_t flags = 0, bytes = 0, data = 0;     // guest memory
    Fmt fmt;
    std::atomic<bool> playing{false}, looping{false};
    std::atomic<uint32_t> pos_bytes{0};
    double frame_pos = 0;                        // mixer thread only (plus SetCurrentPosition under the mixer lock)
    std::atomic<int32_t> volume{0}, pan{0};
    std::atomic<uint32_t> freq{0};               // 0 = native
    uint32_t guest = 0;
};

struct Device {
    std::vector<Buffer *> buffers;
    uint32_t guest = 0;
    uint32_t coop_level = 0;
    Fmt primary_fmt;
};

std::mutex g_mu;                    // protects g_active and buffer lifetime against the mixer
std::vector<Buffer *> g_active;     // every live secondary buffer (mixed when playing)
SDL_AudioStream *g_stream = nullptr;
std::thread g_fallback;
std::atomic<bool> g_fallback_run{false};
bool g_audio_started = false;
uint32_t g_vt_dev = 0, g_vt_buf = 0;
std::atomic<uint64_t> g_frames_mixed{0};
std::atomic<std::vector<float> *> g_capture{nullptr};   // test hook: when set, every mixed block is appended here

bool parse_fmt(uint32_t p, Fmt *f) {
    if (!p) return false;
    f->tag = rd16(p);
    f->channels = rd16(p + 2);
    f->rate = rd32(p + 4);
    f->block = rd16(p + 12);
    f->bits = rd16(p + 14);
    f->is_float = f->tag == 3;
    if (f->tag == 0xFFFE && rd16(p + 16) >= 22) f->is_float = rd32(p + 24) == 3;   // WAVEFORMATEXTENSIBLE sub-format
    if (f->tag != 1 && f->tag != 3 && f->tag != 0xFFFE) return false;
    if (f->channels == 0 || f->rate < 100 || f->rate > 400000 || f->bits == 0) return false;
    if (f->block == 0) f->block = (uint16_t)(f->channels * f->bits / 8);
    return true;
}

inline float sample_at(const Buffer &b, uint32_t frame, int ch) {
    uint32_t off = frame * b.fmt.block + (uint32_t)ch * (b.fmt.bits / 8);
    const uint8_t *p = guest_base + b.data + off;
    switch (b.fmt.bits) {
        case 8: return ((int)p[0] - 128) * (1.0f / 128.0f);
        case 16: return (float)*(const int16_t *)p * (1.0f / 32768.0f);
        case 24: return (float)(((int32_t)(p[0] << 8 | p[1] << 16 | (uint32_t)p[2] << 24)) >> 8) * (1.0f / 8388608.0f);
        case 32: return b.fmt.is_float ? *(const float *)p : (float)((double)*(const int32_t *)p * (1.0 / 2147483648.0));
        default: return 0;
    }
}

void mix_buffer(Buffer &b, float *out, int frames) {
    uint32_t total_frames = b.bytes / b.fmt.block;
    if (!total_frames) { b.playing = false; return; }
    double step = (b.freq ? (double)b.freq.load() : (double)b.fmt.rate) / kOutRate;
    float vol = std::pow(10.0f, (float)b.volume.load() / 2000.0f);
    int pan = b.pan.load();
    float gl = vol, gr = vol;
    if (pan > 0) gl *= std::pow(10.0f, -(float)pan / 2000.0f);
    else if (pan < 0) gr *= std::pow(10.0f, (float)pan / 2000.0f);
    int chans = b.fmt.channels;
    bool loop = b.looping;
    for (int i = 0; i < frames; i++) {
        uint32_t f0 = (uint32_t)b.frame_pos;
        if (f0 >= total_frames) {
            if (!loop) { b.playing = false; b.frame_pos = 0; b.pos_bytes = 0; return; }
            b.frame_pos -= total_frames;
            f0 = (uint32_t)b.frame_pos;
        }
        float frac = (float)(b.frame_pos - f0);
        uint32_t f1 = f0 + 1 >= total_frames ? (loop ? 0 : f0) : f0 + 1;
        float l, r;
        if (frac == 0.0f) {
            l = sample_at(b, f0, 0);
            r = chans > 1 ? sample_at(b, f0, 1) : l;
        } else {
            l = sample_at(b, f0, 0) * (1 - frac) + sample_at(b, f1, 0) * frac;
            r = chans > 1 ? sample_at(b, f0, 1) * (1 - frac) + sample_at(b, f1, 1) * frac : l;
        }
        out[2 * i] += l * gl;
        out[2 * i + 1] += r * gr;
        b.frame_pos += step;
    }
    uint32_t pf = (uint32_t)b.frame_pos;
    if (pf >= total_frames) pf %= total_frames;
    b.pos_bytes = pf * b.fmt.block;
}

void mix_frames(float *out, int frames) {
    memset(out, 0, (size_t)frames * 2 * sizeof(float));
    std::lock_guard<std::mutex> lk(g_mu);
    for (Buffer *b : g_active)
        if (b->playing.load(std::memory_order_relaxed)) mix_buffer(*b, out, frames);
    for (int i = 0; i < frames * 2; i++) out[i] = std::max(-1.0f, std::min(1.0f, out[i]));
    g_frames_mixed += (uint64_t)frames;
    if (auto *cap = g_capture.load()) cap->insert(cap->end(), out, out + frames * 2);
}

void SDLCALL audio_cb(void *, SDL_AudioStream *s, int additional, int) {
    std::vector<float> tmp;
    while (additional > 0) {
        int frames = std::min(additional / 8, 2048);
        if (frames <= 0) break;
        tmp.resize((size_t)frames * 2);
        mix_frames(tmp.data(), frames);
        SDL_PutAudioStreamData(s, tmp.data(), frames * 8);
        additional -= frames * 8;
    }
}

void audio_lifecycle(bool paused) {
    if (!g_stream) return;
    if (paused) SDL_PauseAudioStreamDevice(g_stream);
    else SDL_ResumeAudioStreamDevice(g_stream);
}

void ensure_audio() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_audio_started) return;
    g_audio_started = true;
    mainthread_call([] {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            SDL_AudioSpec spec;
            spec.format = SDL_AUDIO_F32; spec.channels = 2; spec.freq = kOutRate;
            g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, nullptr);
            if (g_stream) { SDL_ResumeAudioStreamDevice(g_stream); lifecycle_add_hook(audio_lifecycle); LOGI(LC_AUDIO, "SDL audio stream open: %d Hz stereo float", kOutRate); return; }
        }
        LOGW(LC_AUDIO, "no SDL audio device (%s); using a silent real-time clock", SDL_GetError());
    });
    if (!g_stream) {
        g_fallback_run = true;
        g_fallback = std::thread([] {
            std::vector<float> tmp(960 * 2);
            auto next = std::chrono::steady_clock::now();
            while (g_fallback_run) {
                mix_frames(tmp.data(), 480);
                next += std::chrono::milliseconds(10);
                std::this_thread::sleep_until(next);
            }
        });
    }
}

void destroy_buffer(void *p) {
    Buffer *b = static_cast<Buffer *>(p);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_active.erase(std::remove(g_active.begin(), g_active.end(), b), g_active.end());
    }
    if (b->data) heap_free(heap_process(), 0, b->data);
    delete b;
}
void destroy_device(void *p) { delete static_cast<Device *>(p); }

// ---- buffer methods ------------------------------------------------------------------------------------------------------
Buffer *B(uint32_t esp) { return static_cast<Buffer *>(com_host_object_raw(*reinterpret_cast<const uint32_t *>(guest_base + esp + 4))); }
#define ARG(i) (*reinterpret_cast<const uint32_t *>(guest_base + esp + 4 + 4 * (i)))

GuestRet qi_common(uint32_t esp, bool is_dev) {
    uint32_t self = ARG(0), riid = ARG(1), ppv = ARG(2);
    if (!ppv) return guest_ret(DSERR_INVALIDPARAM, 0, 12);
    bool ok = guid_eq(riid, IID_IUnknown_);
    if (is_dev) ok = ok || guid_eq(riid, IID_IDirectSound_) || guid_eq(riid, IID_IDirectSound8_);
    else ok = ok || guid_eq(riid, IID_IDirectSoundBuffer_) || guid_eq(riid, IID_IDirectSoundBuffer8_);
    if (!ok) { wr32(ppv, 0); return guest_ret(DSERR_NOINTERFACE, 0, 12); }
    com_addref(self);
    wr32(ppv, self);
    return guest_ret(DS_OK, 0, 12);
}
GuestRet ds_QI(uint32_t esp, uint32_t, uint32_t, uint32_t) { return qi_common(esp, true); }
GuestRet dsb_QI(uint32_t esp, uint32_t, uint32_t, uint32_t) { return qi_common(esp, false); }
GuestRet ds_AddRef(uint32_t esp, uint32_t, uint32_t, uint32_t) { return guest_ret(com_addref(ARG(0)), 0, 4); }
GuestRet ds_Release(uint32_t esp, uint32_t, uint32_t, uint32_t) { return guest_ret(com_release(ARG(0)), 0, 4); }

uint32_t make_buffer(Device *dev, uint32_t desc, bool dup_of, Buffer *src, uint32_t *err) {
    Buffer *b = new Buffer();
    if (dup_of && src) {
        b->flags = src->flags; b->bytes = src->bytes; b->fmt = src->fmt;
    } else {
        uint32_t dsize = rd32(desc);
        if (dsize < 20) { delete b; *err = DSERR_INVALIDPARAM; return 0; }
        b->flags = rd32(desc + 4);
        b->bytes = rd32(desc + 8);
        b->primary = (b->flags & DSBCAPS_PRIMARYBUFFER) != 0;
        if (!b->primary) {
            if (!parse_fmt(rd32(desc + 16), &b->fmt)) { delete b; *err = DSERR_BADFORMAT; return 0; }
            if (b->bytes < 4 || b->bytes > 0x0FFFFFFF) { delete b; *err = DSERR_INVALIDPARAM; return 0; }
        } else {
            b->fmt = dev->primary_fmt;
            b->bytes = 4096;
        }
    }
    b->data = heap_alloc(heap_process(), 0, b->bytes);
    if (!b->data) { delete b; *err = DSERR_OUTOFMEMORY; return 0; }
    memset(guest_base + b->data, b->fmt.bits == 8 ? 0x80 : 0, b->bytes);
    if (dup_of && src) memcpy(guest_base + b->data, guest_base + src->data, b->bytes);
    uint32_t g = com_new_object(g_vt_buf, b, destroy_buffer);
    if (!g) { heap_free(heap_process(), 0, b->data); delete b; *err = DSERR_OUTOFMEMORY; return 0; }
    b->guest = g;
    if (!b->primary) { std::lock_guard<std::mutex> lk(g_mu); g_active.push_back(b); }
    LOGD(LC_AUDIO, "DirectSound buffer %08x: %s %u bytes %u Hz %u ch %u bit%s", g, b->primary ? "primary" : "secondary", b->bytes, b->fmt.rate, b->fmt.channels, b->fmt.bits, b->fmt.is_float ? " float" : "");
    return g;
}

GuestRet ds_CreateSoundBuffer(uint32_t esp, uint32_t, uint32_t, uint32_t) {    // (this, DSBUFFERDESC*, IDirectSoundBuffer**, IUnknown*)
    auto *dev = static_cast<Device *>(com_host_object_raw(ARG(0)));
    if (!dev) return guest_ret(DSERR_INVALIDPARAM, 0, 16);
    if (!ARG(1) || !ARG(2)) return guest_ret(DSERR_INVALIDPARAM, 0, 16);
    uint32_t err = DSERR_INVALIDPARAM;
    uint32_t g = make_buffer(dev, ARG(1), false, nullptr, &err);
    if (!g) { wr32(ARG(2), 0); return guest_ret(err, 0, 16); }
    wr32(ARG(2), g);
    return guest_ret(DS_OK, 0, 16);
}
GuestRet ds_GetCaps(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    uint32_t p = ARG(1);
    if (!p || rd32(p) < 96) return guest_ret(DSERR_INVALIDPARAM, 0, 8);
    uint32_t sz = rd32(p);
    memset(guest_base + p, 0, sz);
    wr32(p, sz);
    wr32(p + 4, 0xF1F);          // primary mono/stereo/8/16 bit, continuous rate, secondary mono/stereo/8/16 bit
    wr32(p + 8, 100); wr32(p + 12, 200000);
    wr32(p + 16, 1);             // primary buffers
    wr32(p + 68, 0); wr32(p + 72, 0);
    return guest_ret(DS_OK, 0, 8);
}
GuestRet ds_DuplicateSoundBuffer(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    auto *dev = static_cast<Device *>(com_host_object_raw(ARG(0)));
    auto *src = static_cast<Buffer *>(com_host_object_raw(ARG(1)));
    if (!dev || !src || !ARG(2)) return guest_ret(DSERR_INVALIDPARAM, 0, 12);
    uint32_t err = DSERR_INVALIDPARAM;
    uint32_t g = make_buffer(dev, 0, true, src, &err);
    wr32(ARG(2), g);
    return guest_ret(g ? DS_OK : err, 0, 12);
}
GuestRet ds_SetCooperativeLevel(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    if (auto *dev = static_cast<Device *>(com_host_object_raw(ARG(0)))) dev->coop_level = ARG(2);
    return guest_ret(DS_OK, 0, 12);
}
GuestRet ds_Compact(uint32_t, uint32_t, uint32_t, uint32_t) { return guest_ret(DS_OK, 0, 4); }
GuestRet ds_GetSpeakerConfig(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    if (!ARG(1)) return guest_ret(DSERR_INVALIDPARAM, 0, 8);
    wr32(ARG(1), 0x00140003u);   // DSSPEAKER_COMBINED(DSSPEAKER_STEREO, DSSPEAKER_GEOMETRY_WIDE)
    return guest_ret(DS_OK, 0, 8);
}
GuestRet ds_SetSpeakerConfig(uint32_t, uint32_t, uint32_t, uint32_t) { return guest_ret(DS_OK, 0, 8); }
GuestRet ds_Initialize(uint32_t, uint32_t, uint32_t, uint32_t) { return guest_ret(DS_OK, 0, 8); }
GuestRet ds_VerifyCertification(uint32_t esp, uint32_t, uint32_t, uint32_t) { if (ARG(1)) wr32(ARG(1), 0); return guest_ret(DS_OK, 0, 8); }

GuestRet dsb_GetCaps(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    Buffer *b = B(esp);
    uint32_t p = ARG(1);
    if (!b || !p || rd32(p) < 20) return guest_ret(DSERR_INVALIDPARAM, 0, 8);
    wr32(p + 4, b->flags | 8 /*LOCSOFTWARE*/); wr32(p + 8, b->bytes); wr32(p + 12, 0); wr32(p + 16, 0);
    return guest_ret(DS_OK, 0, 8);
}
GuestRet dsb_GetCurrentPosition(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    Buffer *b = B(esp);
    if (!b) return guest_ret(DSERR_INVALIDPARAM, 0, 12);
    uint32_t play = b->pos_bytes.load();
    if (ARG(1)) wr32(ARG(1), play);
    if (ARG(2)) {
        uint32_t lead = std::min<uint32_t>(b->bytes / 2, (b->fmt.rate / 25) * b->fmt.block);   // 40 ms of audio
        wr32(ARG(2), b->bytes ? (play + lead) % b->bytes : 0);
    }
    return guest_ret(DS_OK, 0, 12);
}
GuestRet dsb_GetFormat(uint32_t esp, uint32_t, uint32_t, uint32_t) {   // (this, WAVEFORMATEX*, size, DWORD *written)
    Buffer *b = B(esp);
    if (!b) return guest_ret(DSERR_INVALIDPARAM, 0, 16);
    if (ARG(3)) wr32(ARG(3), 18);
    if (ARG(1) && ARG(2) >= 18) {
        wr16(ARG(1), b->fmt.tag == 0xFFFE ? (b->fmt.is_float ? 3 : 1) : b->fmt.tag); wr16(ARG(1) + 2, b->fmt.channels); wr32(ARG(1) + 4, b->fmt.rate);
        wr32(ARG(1) + 8, b->fmt.rate * b->fmt.block); wr16(ARG(1) + 12, b->fmt.block); wr16(ARG(1) + 14, b->fmt.bits); wr16(ARG(1) + 16, 0);
    }
    return guest_ret(DS_OK, 0, 16);
}
GuestRet dsb_GetVolume(uint32_t esp, uint32_t, uint32_t, uint32_t) { Buffer *b = B(esp); if (!b || !ARG(1)) return guest_ret(DSERR_INVALIDPARAM, 0, 8); wr32(ARG(1), (uint32_t)b->volume.load()); return guest_ret(DS_OK, 0, 8); }
GuestRet dsb_GetPan(uint32_t esp, uint32_t, uint32_t, uint32_t) { Buffer *b = B(esp); if (!b || !ARG(1)) return guest_ret(DSERR_INVALIDPARAM, 0, 8); wr32(ARG(1), (uint32_t)b->pan.load()); return guest_ret(DS_OK, 0, 8); }
GuestRet dsb_GetFrequency(uint32_t esp, uint32_t, uint32_t, uint32_t) { Buffer *b = B(esp); if (!b || !ARG(1)) return guest_ret(DSERR_INVALIDPARAM, 0, 8); wr32(ARG(1), b->freq ? b->freq.load() : b->fmt.rate); return guest_ret(DS_OK, 0, 8); }
GuestRet dsb_GetStatus(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    Buffer *b = B(esp);
    if (!b || !ARG(1)) return guest_ret(DSERR_INVALIDPARAM, 0, 8);
    wr32(ARG(1), (b->playing ? DSBSTATUS_PLAYING : 0) | (b->playing && b->looping ? DSBSTATUS_LOOPING : 0) | 0x10 /*LOCSOFTWARE*/);
    return guest_ret(DS_OK, 0, 8);
}
GuestRet dsb_Initialize(uint32_t, uint32_t, uint32_t, uint32_t) { return guest_ret(0x88780082u, 0, 12); }   // DSERR_ALREADYINITIALIZED
GuestRet dsb_Lock(uint32_t esp, uint32_t, uint32_t, uint32_t) {   // (this, offset, bytes, p1*, b1*, p2*, b2*, flags)
    Buffer *b = B(esp);
    if (!b || !ARG(3) || !ARG(4)) return guest_ret(DSERR_INVALIDPARAM, 0, 32);
    uint32_t off = ARG(1), len = ARG(2), flags = ARG(7);
    if (flags & DSBLOCK_FROMWRITECURSOR) off = (b->pos_bytes.load() + std::min<uint32_t>(b->bytes / 2, (b->fmt.rate / 25) * b->fmt.block)) % std::max<uint32_t>(b->bytes, 1);
    if (flags & DSBLOCK_ENTIREBUFFER) { off = 0; len = b->bytes; }
    if (off >= b->bytes || len > b->bytes) return guest_ret(DSERR_INVALIDPARAM, 0, 32);
    uint32_t first = std::min(len, b->bytes - off);
    wr32(ARG(3), b->data + off);
    wr32(ARG(4), first);
    if (ARG(5)) wr32(ARG(5), len > first ? b->data : 0);
    if (ARG(6)) wr32(ARG(6), len > first ? len - first : 0);
    return guest_ret(DS_OK, 0, 32);
}
GuestRet dsb_Play(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    Buffer *b = B(esp);
    if (!b) return guest_ret(DSERR_INVALIDPARAM, 0, 16);
    if (ARG(1) != 0) return guest_ret(DSERR_INVALIDPARAM, 0, 16);
    if (b->primary) return guest_ret(DS_OK, 0, 16);
    b->looping = (ARG(3) & DSBPLAY_LOOPING) != 0;
    b->playing = true;
    return guest_ret(DS_OK, 0, 16);
}
GuestRet dsb_Stop(uint32_t esp, uint32_t, uint32_t, uint32_t) { if (Buffer *b = B(esp)) b->playing = false; return guest_ret(DS_OK, 0, 4); }
GuestRet dsb_SetCurrentPosition(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    Buffer *b = B(esp);
    if (!b || ARG(1) > b->bytes) return guest_ret(DSERR_INVALIDPARAM, 0, 8);
    std::lock_guard<std::mutex> lk(g_mu);
    b->frame_pos = ARG(1) / b->fmt.block;
    b->pos_bytes = (ARG(1) / b->fmt.block) * b->fmt.block;
    return guest_ret(DS_OK, 0, 8);
}
GuestRet dsb_SetFormat(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    Buffer *b = B(esp);
    Fmt f;
    if (!b || !parse_fmt(ARG(1), &f)) return guest_ret(DSERR_BADFORMAT, 0, 8);
    if (!b->primary) return guest_ret(DSERR_INVALIDCALL, 0, 8);   // secondary formats are fixed at creation
    b->fmt = f;
    return guest_ret(DS_OK, 0, 8);
}
GuestRet dsb_SetVolume(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    Buffer *b = B(esp);
    int32_t v = (int32_t)ARG(1);
    if (!b || v > 0 || v < -10000) return guest_ret(DSERR_INVALIDPARAM, 0, 8);
    b->volume = v;
    return guest_ret(DS_OK, 0, 8);
}
GuestRet dsb_SetPan(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    Buffer *b = B(esp);
    int32_t v = (int32_t)ARG(1);
    if (!b || v > 10000 || v < -10000) return guest_ret(DSERR_INVALIDPARAM, 0, 8);
    b->pan = v;
    return guest_ret(DS_OK, 0, 8);
}
GuestRet dsb_SetFrequency(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    Buffer *b = B(esp);
    uint32_t f = ARG(1);
    if (!b || (f != 0 && (f < 100 || f > 200000))) return guest_ret(DSERR_INVALIDPARAM, 0, 8);
    b->freq = f == b->fmt.rate ? 0 : f;
    return guest_ret(DS_OK, 0, 8);
}
GuestRet dsb_Unlock(uint32_t, uint32_t, uint32_t, uint32_t) { return guest_ret(DS_OK, 0, 20); }   // the mixer reads guest memory directly
GuestRet dsb_Restore(uint32_t, uint32_t, uint32_t, uint32_t) { return guest_ret(DS_OK, 0, 4); }
GuestRet dsb_SetFX(uint32_t esp, uint32_t, uint32_t, uint32_t) { return guest_ret(ARG(1) ? DSERR_UNSUPPORTED : DS_OK, 0, 16); }
GuestRet dsb_AcquireResources(uint32_t esp, uint32_t, uint32_t, uint32_t) { if (ARG(3)) wr32(ARG(3), 0); return guest_ret(DS_OK, 0, 16); }
GuestRet dsb_GetObjectInPath(uint32_t, uint32_t, uint32_t, uint32_t) { return guest_ret(DSERR_UNSUPPORTED, 0, 24); }

void define_interfaces() {
    static bool done = false;
    if (done) return;
    done = true;
    static const ComMethod dev_methods[] = {
        {"QueryInterface", ds_QI}, {"AddRef", ds_AddRef}, {"Release", ds_Release}, {"CreateSoundBuffer", ds_CreateSoundBuffer}, {"GetCaps", ds_GetCaps},
        {"DuplicateSoundBuffer", ds_DuplicateSoundBuffer}, {"SetCooperativeLevel", ds_SetCooperativeLevel}, {"Compact", ds_Compact},
        {"GetSpeakerConfig", ds_GetSpeakerConfig}, {"SetSpeakerConfig", ds_SetSpeakerConfig}, {"Initialize", ds_Initialize}, {"VerifyCertification", ds_VerifyCertification},
    };
    static const ComMethod buf_methods[] = {
        {"QueryInterface", dsb_QI}, {"AddRef", ds_AddRef}, {"Release", ds_Release}, {"GetCaps", dsb_GetCaps}, {"GetCurrentPosition", dsb_GetCurrentPosition},
        {"GetFormat", dsb_GetFormat}, {"GetVolume", dsb_GetVolume}, {"GetPan", dsb_GetPan}, {"GetFrequency", dsb_GetFrequency}, {"GetStatus", dsb_GetStatus},
        {"Initialize", dsb_Initialize}, {"Lock", dsb_Lock}, {"Play", dsb_Play}, {"SetCurrentPosition", dsb_SetCurrentPosition}, {"SetFormat", dsb_SetFormat},
        {"SetVolume", dsb_SetVolume}, {"SetPan", dsb_SetPan}, {"SetFrequency", dsb_SetFrequency}, {"Stop", dsb_Stop}, {"Unlock", dsb_Unlock}, {"Restore", dsb_Restore},
        {"SetFX", dsb_SetFX}, {"AcquireResources", dsb_AcquireResources}, {"GetObjectInPath", dsb_GetObjectInPath},
    };
    g_vt_dev = com_define_interface("IDirectSound8", dev_methods, sizeof dev_methods / sizeof dev_methods[0]);
    g_vt_buf = com_define_interface("IDirectSoundBuffer8", buf_methods, sizeof buf_methods / sizeof buf_methods[0]);
}

uint32_t new_device() {
    define_interfaces();
    ensure_audio();
    auto *d = new Device();
    d->primary_fmt = Fmt{};
    uint32_t g = com_new_object(g_vt_dev, d, destroy_device);
    d->guest = g;
    return g;
}

uint32_t ds_Create(uint32_t guid, uint32_t pp, uint32_t outer) {
    (void)guid;
    if (!pp) return DSERR_INVALIDPARAM;
    if (outer) { wr32(pp, 0); return 0x80040110u; }   // CLASS_E_NOAGGREGATION
    uint32_t g = new_device();
    wr32(pp, g);
    return g ? DS_OK : DSERR_OUTOFMEMORY;
}
uint32_t ds_DirectSoundCreate(uint32_t guid, uint32_t pp, uint32_t outer) { return ds_Create(guid, pp, outer); }
uint32_t ds_DirectSoundCreate8(uint32_t guid, uint32_t pp, uint32_t outer) { return ds_Create(guid, pp, outer); }
uint32_t ds_DirectSoundCaptureCreate(uint32_t, uint32_t pp, uint32_t) { if (pp) wr32(pp, 0); return DSERR_NODRIVER; }
uint32_t ds_DirectSoundCaptureEnumerate(uint32_t, uint32_t) { return DS_OK; }
uint32_t enumerate(const HleCtx &c, uint32_t cb, uint32_t ctx, bool wide) {
    if (!cb) return DSERR_INVALIDPARAM;
    uint32_t desc = heap_alloc(heap_process(), 0, 128), mod = heap_alloc(heap_process(), 0, 64);
    if (wide) { write_wstr(desc, 64, u"Primary Sound Driver"); write_wstr(mod, 32, u""); }
    else { write_astr(desc, 128, "Primary Sound Driver"); write_astr(mod, 64, ""); }
    guest_invoke_from(c.esp, cb, Conv::Stdcall, 0u, desc, mod, ctx);
    heap_free(heap_process(), 0, desc);
    heap_free(heap_process(), 0, mod);
    return DS_OK;
}
uint32_t ds_DirectSoundEnumerateA(const HleCtx &c, uint32_t cb, uint32_t ctx) { return enumerate(c, cb, ctx, false); }
uint32_t ds_DirectSoundEnumerateW(const HleCtx &c, uint32_t cb, uint32_t ctx) { return enumerate(c, cb, ctx, true); }
uint32_t ds_GetDeviceID(uint32_t src, uint32_t dst) {
    if (!dst) return DSERR_INVALIDPARAM;
    static const uint8_t def[16] = {0xDF, 0x2C, 0x4E, 0xDF, 0x4B, 0x8B, 0x2A, 0x46, 0xA7, 0x3E, 0x4C, 0x3F, 0x01, 0x23, 0x45, 0x67};
    memcpy(guest_base + dst, def, 16);
    (void)src;
    return DS_OK;
}

}  // namespace

// CoCreateInstance hook (ole32): CLSID_DirectSound8 {3901CC3F-84B5-4FA4-BA35-AA8172B8A09B}, CLSID_DirectSound {47D4D946-62E8-11CF-93BC-444553540000}
bool dsound_co_create(uint32_t clsid, uint32_t iid, uint32_t *out_obj, uint32_t *hr) {
    static const Guid clsid8{0x3901CC3F, 0x84B5, 0x4FA4, {0xBA, 0x35, 0xAA, 0x81, 0x72, 0xB8, 0xA0, 0x9B}};
    static const Guid clsid1{0x47D4D946, 0x62E8, 0x11CF, {0x93, 0xBC, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
    if (!guid_eq(clsid, clsid8) && !guid_eq(clsid, clsid1)) return false;
    uint32_t g = new_device();
    *hr = DSERR_OUTOFMEMORY;
    *out_obj = 0;
    if (!g) return true;
    if (guid_eq(iid, IID_IUnknown_) || guid_eq(iid, IID_IDirectSound_) || guid_eq(iid, IID_IDirectSound8_)) { *out_obj = g; *hr = DS_OK; }
    else { com_release(g); *hr = DSERR_NOINTERFACE; }
    return true;
}

uint64_t dsound_frames_mixed() { return g_frames_mixed.load(); }
void dsound_set_capture(std::vector<float> *sink) { g_capture = sink; }

void dsound_shutdown() {
    g_fallback_run = false;
    if (g_fallback.joinable()) g_fallback.join();
    mainthread_call([] { if (g_stream) { SDL_DestroyAudioStream(g_stream); g_stream = nullptr; } });
}

HLE("dsound.dll", "DirectSoundCreate", Stdcall, ds_DirectSoundCreate);
HLE("dsound.dll", "DirectSoundCreate8", Stdcall, ds_DirectSoundCreate8);
HLE("dsound.dll", "DirectSoundEnumerateA", Stdcall, ds_DirectSoundEnumerateA);
HLE("dsound.dll", "DirectSoundEnumerateW", Stdcall, ds_DirectSoundEnumerateW);
HLE("dsound.dll", "DirectSoundCaptureCreate", Stdcall, ds_DirectSoundCaptureCreate);
HLE("dsound.dll", "DirectSoundCaptureCreate8", Stdcall, ds_DirectSoundCaptureCreate);
HLE("dsound.dll", "DirectSoundCaptureEnumerateA", Stdcall, ds_DirectSoundCaptureEnumerate);
HLE("dsound.dll", "DirectSoundCaptureEnumerateW", Stdcall, ds_DirectSoundCaptureEnumerate);
HLE("dsound.dll", "GetDeviceID", Stdcall, ds_GetDeviceID);

}  // namespace dish
