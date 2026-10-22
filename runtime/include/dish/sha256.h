#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace dish {
struct Sha256 {
    uint32_t h[8];
    uint64_t len = 0;
    uint8_t buf[64];
    size_t nbuf = 0;
    Sha256();
    void update(const void *data, size_t n);
    void finish(uint8_t out[32]);
};
void sha256(const void *data, size_t n, uint8_t out[32]);
std::string hex(const uint8_t *p, size_t n);
}  // namespace dish
