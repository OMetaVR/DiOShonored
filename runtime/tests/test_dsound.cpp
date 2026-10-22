// DirectSound as Wwise/Bink use it: a fake guest client that creates the device the way the trace shows (CoCreateInstance(CLSID_DirectSound8)
// and DirectSoundCreate through GetProcAddress), builds a looping ring buffer, streams into it with Lock/Unlock and follows the play cursor.
#include "testing.h"
#include "dish/com.h"
#include "dish/platform.h"
#include <SDL3/SDL.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

using namespace dish;

static uint32_t vcall(uint32_t obj, uint32_t slot, std::initializer_list<uint32_t> args, uint32_t *pop = nullptr) {
    uint32_t fn = rd32(rd32(obj) + 4 * slot);
    std::vector<uint32_t> a{obj};
    a.insert(a.end(), args.begin(), args.end());
    uint32_t sp = (t::stack_top() & ~15u) - 4 * (uint32_t)a.size() - 4;
    for (size_t i = 0; i < a.size(); i++) wr32(sp + 4 + 4 * (uint32_t)i, a[i]);
    wr32(sp, kReturnSentinel);
    GuestRet r = guest_call(fn, sp, 0, 0, 0);
    if (pop) *pop = guest_ret_pop(r);
    else if (guest_ret_pop(r) != 4 * a.size()) { fprintf(stderr, "bad pop for slot %u\n", slot); abort(); }
    return guest_ret_eax(r);
}
// IDirectSound(8) slots
enum { QI = 0, ADDREF = 1, RELEASE = 2, CREATEBUF = 3, GETCAPS = 4, DUPBUF = 5, SETCOOP = 6, COMPACT = 7, GETSPK = 8, SETSPK = 9, DSINIT = 10 };
// IDirectSoundBuffer(8) slots
enum { B_GETCAPS = 3, B_GETPOS = 4, B_GETFMT = 5, B_GETVOL = 6, B_GETPAN = 7, B_GETFREQ = 8, B_GETSTATUS = 9, B_INIT = 10, B_LOCK = 11, B_PLAY = 12, B_SETPOS = 13,
       B_SETFMT = 14, B_SETVOL = 15, B_SETPAN = 16, B_SETFREQ = 17, B_STOP = 18, B_UNLOCK = 19, B_RESTORE = 20 };

static uint32_t make_wfx(uint16_t tag, uint16_t ch, uint32_t rate, uint16_t bits) {
    uint32_t w = t::galloc(40);
    wr16(w, tag); wr16(w + 2, ch); wr32(w + 4, rate); wr16(w + 12, (uint16_t)(ch * bits / 8)); wr32(w + 8, rate * (uint32_t)(ch * bits / 8)); wr16(w + 14, bits);
    return w;
}
static uint32_t make_desc(uint32_t flags, uint32_t bytes, uint32_t wfx) {
    uint32_t d = t::galloc(36);
    wr32(d, 36); wr32(d + 4, flags); wr32(d + 8, bytes); wr32(d + 16, wfx);
    return d;
}
static uint32_t g_dev = 0;
static bool setup() {
    t::setup_runtime("dsound");
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    return true;
}
static uint32_t make_buffer(uint32_t flags, uint32_t bytes, uint32_t wfx) {
    uint32_t out = t::galloc(4);
    uint32_t hr = vcall(g_dev, CREATEBUF, {make_desc(flags, bytes, wfx), out, 0});
    if (hr) { fprintf(stderr, "CreateSoundBuffer: %08x\n", hr); return 0; }
    return rd32(out);
}

TEST(device_creation_paths) {
    setup();
    // 1. the CoCreateInstance route seen in the Wwise trace
    uint8_t clsid[16] = {0x3F, 0xCC, 0x01, 0x39, 0xB5, 0x84, 0xA4, 0x4F, 0xBA, 0x35, 0xAA, 0x81, 0x72, 0xB8, 0xA0, 0x9B};
    uint8_t iid8[16] = {0x93, 0x7E, 0x0A, 0xC5, 0x95, 0xF3, 0x34, 0x48, 0x9E, 0xF6, 0x7F, 0xA9, 0x9D, 0xE5, 0x09, 0x66};
    uint32_t pc = t::galloc(16), pi = t::galloc(16), out = t::galloc(4);
    memcpy(gp(pc), clsid, 16); memcpy(gp(pi), iid8, 16);
    CHECK_EQ(t::hle("ole32.dll", "CoCreateInstance", {pc, 0, 0x17, pi, out}).eax(), 0u);
    g_dev = rd32(out);
    CHECK(g_dev != 0);
    CHECK_EQ(vcall(g_dev, DSINIT, {0}), 0u);                          // Initialize(NULL)
    CHECK_EQ(vcall(g_dev, SETCOOP, {0x0009007E, 2 /*DSSCL_PRIORITY*/}), 0u);
    uint32_t caps = t::galloc(96), spk = t::galloc(4);
    wr32(caps, 96);
    CHECK_EQ(vcall(g_dev, GETCAPS, {caps}), 0u);
    CHECK(rd32(caps + 4) & 0x2);                                      // DSCAPS_PRIMARYSTEREO
    CHECK_EQ(vcall(g_dev, GETSPK, {spk}), 0u);
    CHECK_EQ(rd32(spk) & 0xFFFF, 3u);                                 // DSSPEAKER_STEREO
    // QueryInterface to the base IDirectSound and IUnknown yields the same object
    uint8_t iid1[16] = {0x83, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60};
    memcpy(gp(pi), iid1, 16);
    CHECK_EQ(vcall(g_dev, QI, {pi, out}), 0u);
    CHECK_EQ(rd32(out), g_dev);
    CHECK_EQ(vcall(g_dev, RELEASE, {}), 1u);
    memset(gp(pi), 0x42, 16);
    CHECK_EQ(vcall(g_dev, QI, {pi, out}), 0x80004002u);               // E_NOINTERFACE
    // 2. the DirectSoundCreate route Bink takes (LoadLibrary + GetProcAddress)
    uint32_t h = t::hle("kernel32.dll", "LoadLibraryA", {t::gstr("DSOUND.DLL")}).eax();
    CHECK(h != 0);
    uint32_t create = t::hle("kernel32.dll", "GetProcAddress", {h, t::gstr("DirectSoundCreate")}).eax();
    CHECK(create != 0);
    auto r = t::call_addr(create, {0, out, 0});
    CHECK_EQ(r.eax(), 0u);
    CHECK_EQ(r.pop(), 12u);
    uint32_t dev2 = rd32(out);
    CHECK(dev2 != 0 && dev2 != g_dev);
    vcall(dev2, RELEASE, {});
    CHECK(t::hle("kernel32.dll", "GetProcAddress", {h, t::gstr("DirectSoundCreate8")}).eax() != 0);
}

TEST(mixing_is_correct) {
    setup();
    if (!g_dev) return;
    // primary buffer: SetFormat + Play like both trace clients do
    uint32_t prim = make_buffer(1 /*PRIMARYBUFFER*/, 0, 0);
    CHECK(prim != 0);
    CHECK_EQ(vcall(prim, B_SETFMT, {make_wfx(1, 2, 48000, 16)}), 0u);
    CHECK_EQ(vcall(prim, B_PLAY, {0, 0, 1}), 0u);
    // secondary: 16-bit mono 24 kHz constant 0.5 -> output 48 kHz stereo 0.5; volume -6.02 dB ~ half
    const uint32_t N = 4800;   // 200 ms of source
    uint32_t b = make_buffer(0x80 | 0x40 | 0x20 | 0x10000, N * 2, make_wfx(1, 1, 24000, 16));
    CHECK(b != 0);
    uint32_t p1 = t::galloc(4), l1 = t::galloc(4), p2 = t::galloc(4), l2 = t::galloc(4);
    CHECK_EQ(vcall(b, B_LOCK, {0, 0, p1, l1, p2, l2, 2 /*ENTIREBUFFER*/}), 0u);
    CHECK_EQ(rd32(l1), N * 2);
    CHECK_EQ(rd32(p2), 0u);
    for (uint32_t i = 0; i < N; i++) wr16(rd32(p1) + 2 * i, 16384);   // 0.5
    CHECK_EQ(vcall(b, B_UNLOCK, {rd32(p1), rd32(l1), 0, 0}), 0u);
    CHECK_EQ(vcall(b, B_SETVOL, {(uint32_t)-602}), 0u);
    std::vector<float> cap;
    cap.reserve(1 << 20);
    dsound_set_capture(&cap);
    uint32_t st = t::galloc(4);
    CHECK_EQ(vcall(b, B_PLAY, {0, 0, 0}), 0u);
    CHECK_EQ(vcall(b, B_GETSTATUS, {st}), 0u);
    CHECK(rd32(st) & 1);
    // wait for the one-shot buffer to finish (200 ms of audio at real time)
    for (int i = 0; i < 100; i++) { SDL_Delay(20); vcall(b, B_GETSTATUS, {st}); if (!(rd32(st) & 1)) break; }
    dsound_set_capture(nullptr);
    CHECK(!(rd32(st) & 1));                                           // stopped by itself at the end
    // find the non-silent run in the captured output
    size_t first = 0, last = 0, nonzero = 0;
    for (size_t i = 0; i < cap.size() / 2; i++)
        if (fabsf(cap[2 * i]) > 1e-4f) { if (!nonzero) first = i; last = i; nonzero++; }
    fprintf(stderr, "  mixed %zu frames, %zu audible (expected 9600 = 200 ms at 48 kHz)\n", cap.size() / 2, nonzero);
    CHECK(nonzero >= 9500 && nonzero <= 9700);
    CHECK(last - first + 1 == nonzero);                                // contiguous
    float expect = 0.5f * powf(10.0f, -602.0f / 2000.0f);
    CHECK(fabsf(cap[2 * (first + 100)] - expect) < 0.002f);
    CHECK(fabsf(cap[2 * (first + 100) + 1] - expect) < 0.002f);
    // pan: full right -> left channel silent
    CHECK_EQ(vcall(b, B_SETVOL, {0}), 0u);
    CHECK_EQ(vcall(b, B_SETPAN, {10000}), 0u);
    CHECK_EQ(vcall(b, B_SETPOS, {0}), 0u);
    cap.clear();
    dsound_set_capture(&cap);
    vcall(b, B_PLAY, {0, 0, 0});
    SDL_Delay(120);
    vcall(b, B_STOP, {});
    dsound_set_capture(nullptr);
    float maxl = 0, maxr = 0;
    for (size_t i = 0; i < cap.size() / 2; i++) { maxl = std::max(maxl, fabsf(cap[2 * i])); maxr = std::max(maxr, fabsf(cap[2 * i + 1])); }
    CHECK(maxr > 0.45f);
    CHECK(maxl < 0.01f);
    // two buffers mix additively, and clipping saturates at 1.0
    uint32_t c = make_buffer(0x80, N * 2, make_wfx(1, 1, 24000, 16));
    CHECK_EQ(vcall(c, B_LOCK, {0, 0, p1, l1, p2, l2, 2}), 0u);
    for (uint32_t i = 0; i < N; i++) wr16(rd32(p1) + 2 * i, 32767);
    vcall(c, B_UNLOCK, {rd32(p1), rd32(l1), 0, 0});
    vcall(b, B_SETPAN, {0});
    vcall(b, B_SETPOS, {0});
    cap.clear();
    dsound_set_capture(&cap);
    vcall(b, B_PLAY, {0, 0, 1});
    vcall(c, B_PLAY, {0, 0, 1});
    SDL_Delay(100);
    vcall(b, B_STOP, {});
    vcall(c, B_STOP, {});
    dsound_set_capture(nullptr);
    float mx = 0;
    for (float v : cap) mx = std::max(mx, fabsf(v));
    CHECK(mx > 0.99f && mx <= 1.0f);
    vcall(b, RELEASE, {});
    vcall(c, RELEASE, {});
    vcall(prim, RELEASE, {});
}

TEST(sample_formats) {
    setup();
    if (!g_dev) return;
    uint32_t p1 = t::galloc(4), l1 = t::galloc(4), p2 = t::galloc(4), l2 = t::galloc(4);
    struct Case { uint16_t tag, bits; float expect; };
    for (Case c : {Case{1, 8, 0.5f}, Case{1, 16, 0.5f}, Case{1, 24, 0.5f}, Case{1, 32, 0.5f}, Case{3, 32, 0.5f}}) {
        uint32_t b = make_buffer(0, 4800 * c.bits / 8, make_wfx(c.tag, 1, 48000, c.bits));
        CHECK(b != 0);
        vcall(b, B_LOCK, {0, 0, p1, l1, p2, l2, 2});
        uint32_t base = rd32(p1);
        for (uint32_t i = 0; i < 4800; i++) {
            if (c.bits == 8) wr8(base + i, 192);
            else if (c.bits == 16) wr16(base + 2 * i, 16384);
            else if (c.bits == 24) { wr8(base + 3 * i, 0); wr8(base + 3 * i + 1, 0); wr8(base + 3 * i + 2, 0x40); }
            else if (c.tag == 3) { float f = 0.5f; memcpy(gp(base + 4 * i), &f, 4); }
            else wr32(base + 4 * i, 0x40000000u);
        }
        std::vector<float> cap;
        dsound_set_capture(&cap);
        vcall(b, B_PLAY, {0, 0, 0});
        SDL_Delay(60);
        dsound_set_capture(nullptr);
        vcall(b, B_STOP, {});
        float mx = 0;
        for (float v : cap) mx = std::max(mx, v);
        fprintf(stderr, "  %s %u-bit -> peak %.3f\n", c.tag == 3 ? "float" : "pcm", c.bits, mx);
        CHECK(fabsf(mx - c.expect) < 0.01f);
        vcall(b, RELEASE, {});
    }
    // invalid formats and sizes are rejected
    uint32_t out = t::galloc(4);
    CHECK_EQ(vcall(g_dev, CREATEBUF, {make_desc(0, 1024, make_wfx(0x55, 2, 44100, 16)), out, 0}), 0x88780064u);   // DSERR_BADFORMAT
    CHECK_EQ(vcall(g_dev, CREATEBUF, {make_desc(0, 0, make_wfx(1, 2, 44100, 16)), out, 0}), 0x80070057u);
}

TEST(streaming_ring_buffer_like_wwise) {
    setup();
    if (!g_dev) return;
    const uint32_t kRing = 16384;   // the size seen in the trace
    uint32_t b = make_buffer(0x80 | 0x40 | 0x10000, kRing, make_wfx(1, 2, 48000, 16));
    CHECK(b != 0);
    uint32_t p1 = t::galloc(4), l1 = t::galloc(4), p2 = t::galloc(4), l2 = t::galloc(4), pos = t::galloc(8);
    // prime the ring with a 440 Hz tone, start looping playback
    double phase = 0;
    auto fill = [&](uint32_t off, uint32_t bytes) {
        CHECK_EQ(vcall(b, B_LOCK, {off, bytes, p1, l1, p2, l2, 0}), 0u);
        auto write = [&](uint32_t ptr, uint32_t n) {
            for (uint32_t i = 0; i < n / 4; i++) {
                int16_t s = (int16_t)(sin(phase) * 8000);
                phase += 2 * M_PI * 440.0 / 48000.0;
                wr16(ptr + 4 * i, (uint16_t)s); wr16(ptr + 4 * i + 2, (uint16_t)s);
            }
        };
        write(rd32(p1), rd32(l1));
        if (rd32(p2)) write(rd32(p2), rd32(l2));
        vcall(b, B_UNLOCK, {rd32(p1), rd32(l1), rd32(p2), rd32(l2)});
        return rd32(l1) + rd32(l2);
    };
    fill(0, kRing);
    uint64_t f0 = dsound_frames_mixed();
    auto t0 = std::chrono::steady_clock::now();
    CHECK_EQ(vcall(b, B_PLAY, {0, 0, 1}), 0u);
    // follow the play cursor for ~400 ms like an engine feeding the ring; count the wraps
    uint32_t last_play = 0, wraps = 0, written_total = 0, write_ptr = 0;
    bool monotonic = true;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(400)) {
        CHECK_EQ(vcall(b, B_GETPOS, {pos, pos + 4}), 0u);
        uint32_t play = rd32(pos), write = rd32(pos + 4);
        CHECK(play < kRing && play % 4 == 0);
        CHECK(write < kRing && write % 4 == 0);
        if (play < last_play) wraps++;
        else if (play == last_play) {}
        last_play = play;
        // refill the region behind the play cursor (what has been consumed)
        uint32_t behind = (play + kRing - write_ptr) % kRing;
        if (behind >= 2048) { uint32_t n = fill(write_ptr, behind & ~3u); write_ptr = (write_ptr + n) % kRing; written_total += n; }
        SDL_Delay(2);
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    uint64_t frames = dsound_frames_mixed() - f0;
    fprintf(stderr, "  %.0f ms: %u wraps of the 16 KiB ring, %llu frames mixed (%.1f%% of real time), refilled %u bytes\n", secs * 1000, wraps, (unsigned long long)frames,
            100.0 * (double)frames / (secs * 48000.0), written_total);
    CHECK(monotonic);
    // 400 ms of 48 kHz stereo 16-bit = 76800 bytes = 4.7 laps of the ring
    CHECK(wraps >= 3 && wraps <= 6);
    CHECK(frames > 48000 * secs * 0.7 && frames < 48000 * secs * 1.4);
    CHECK_EQ(vcall(b, B_STOP, {}), 0u);
    uint32_t st = t::galloc(4);
    vcall(b, B_GETSTATUS, {st});
    CHECK(!(rd32(st) & 1));
    // wrap-around Lock returns two regions
    CHECK_EQ(vcall(b, B_LOCK, {kRing - 1024, 4096, p1, l1, p2, l2, 0}), 0u);
    CHECK_EQ(rd32(l1), 1024u);
    CHECK_EQ(rd32(l2), 3072u);
    CHECK(rd32(p2) != 0);
    vcall(b, B_UNLOCK, {rd32(p1), rd32(l1), rd32(p2), rd32(l2)});
    // the play cursor can be repositioned, volume/pan/frequency round-trip
    CHECK_EQ(vcall(b, B_SETPOS, {8192}), 0u);
    vcall(b, B_GETPOS, {pos, pos + 4});
    CHECK_EQ(rd32(pos), 8192u);
    CHECK_EQ(vcall(b, B_SETFREQ, {44100}), 0u);
    uint32_t v = t::galloc(4);
    vcall(b, B_GETFREQ, {v});
    CHECK_EQ(rd32(v), 44100u);
    CHECK_EQ(vcall(b, B_SETVOL, {500}), 0x80070057u);                 // positive attenuation is invalid
    // objects are released cleanly
    size_t live = com_live_objects();
    CHECK_EQ(vcall(b, RELEASE, {}), 0u);
    CHECK_EQ(com_live_objects(), live - 1);
}
