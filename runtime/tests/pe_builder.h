#pragma once
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// ---- tiny PE32 writer ---------------------------------------------------------------------------------------
struct Sec { const char *name; uint32_t va; uint32_t flags; std::vector<uint8_t> data; uint32_t vsize; };
static inline void put32(std::vector<uint8_t> &v, size_t off, uint32_t x) { if (v.size() < off + 4) v.resize(off + 4); memcpy(&v[off], &x, 4); }
static inline void put16(std::vector<uint8_t> &v, size_t off, uint16_t x) { if (v.size() < off + 2) v.resize(off + 2); memcpy(&v[off], &x, 2); }

static inline std::vector<uint8_t> build_pe(uint32_t image_base, uint32_t entry, bool dll, std::vector<Sec> secs, uint32_t image_size,
                                     std::vector<std::pair<int, std::pair<uint32_t, uint32_t>>> dirs) {
    std::vector<uint8_t> f(0x400, 0);
    put16(f, 0, 0x5A4D);
    put32(f, 0x3C, 0x80);
    memcpy(&f[0x80], "PE\0\0", 4);
    put16(f, 0x84, 0x14C);
    put16(f, 0x86, (uint16_t)secs.size());
    put16(f, 0x94, 224);
    put16(f, 0x96, dll ? 0x2102 : 0x0102);
    size_t o = 0x98;
    put16(f, o, 0x10B);
    put32(f, o + 16, entry);
    put32(f, o + 28, image_base);
    put32(f, o + 32, 0x1000);
    put32(f, o + 36, 0x200);
    put32(f, o + 56, image_size);
    put32(f, o + 60, 0x400);
    put16(f, o + 68, 2);
    put32(f, o + 72, 0x100000);   // stack reserve
    put32(f, o + 92, 16);
    for (auto &d : dirs) { put32(f, o + 96 + 8 * d.first, d.second.first); put32(f, o + 100 + 8 * d.first, d.second.second); }
    size_t sh = o + 224;
    uint32_t raw = 0x400;
    for (size_t i = 0; i < secs.size(); i++) {
        memcpy(&f[sh + 40 * i], secs[i].name, strlen(secs[i].name));
        put32(f, sh + 40 * i + 8, secs[i].vsize);
        put32(f, sh + 40 * i + 12, secs[i].va);
        uint32_t rs = (uint32_t)((secs[i].data.size() + 0x1FF) & ~0x1FFu);
        put32(f, sh + 40 * i + 16, rs);
        put32(f, sh + 40 * i + 20, raw);
        put32(f, sh + 40 * i + 36, secs[i].flags);
        f.resize(raw + rs, 0);
        memcpy(&f[raw], secs[i].data.data(), secs[i].data.size());
        raw += rs;
    }
    return f;
}
static inline void write_file(const std::string &p, const std::vector<uint8_t> &d) {
    std::string cmd = "mkdir -p '" + p.substr(0, p.rfind('/')) + "'";
    (void)!system(cmd.c_str());
    std::ofstream(p, std::ios::binary).write((const char *)d.data(), (std::streamsize)d.size());
}

