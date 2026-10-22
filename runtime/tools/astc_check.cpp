// Validates the DXT -> ASTC transcode (platform/d3d9_metal/texcodec.cpp) on real texture levels against ARM's astcenc decoder.
//   astc_check <astcenc binary> <work dir> <dxt level files...>
// Each input is a raw DXT level named dxt{1,3,5}_<w>x<h>_<n>.bin (DISH_D3D9_DUMP_DXT). For every level it writes <work>/t.astc,
// decodes it with `astcenc -dl` to <work>/t.png and reads the PNG back through ImageMagick, then prints PSNR of the astcenc decode
// against the BC decode, and the largest per-channel difference between astcenc and the in-tree reference decoder (should be <= 1).
#include "../platform/d3d9_metal/texcodec.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace dish::tex;

static bool read_file(const char *p, std::vector<uint8_t> *d) {
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    d->resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    bool ok = fread(d->data(), 1, d->size(), f) == d->size();
    fclose(f);
    return ok;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: astc_check <astcenc> <workdir> <files...>\n"); return 2; }
    std::string enc = argv[1], work = argv[2];
    double worst_psnr = 1e9, sum_psnr = 0;
    int worst_ref = 0, n = 0, fails = 0;
    for (int a = 3; a < argc; a++) {
        const char *base = strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a];
        int fmt = 0; unsigned w = 0, h = 0;
        if (sscanf(base, "dxt%d_%ux%u_", &fmt, &w, &h) != 3) continue;
        Bc f = fmt == 1 ? Bc::DXT1 : fmt == 3 ? Bc::DXT3 : Bc::DXT5;
        std::vector<uint8_t> bc;
        if (!read_file(argv[a], &bc) || bc.size() < bc_level_bytes(f, w, h)) { fprintf(stderr, "%s: short file\n", base); continue; }
        std::vector<uint8_t> ref((size_t)w * h * 4), mine((size_t)w * h * 4), arm((size_t)w * h * 4);
        bc_decode_level(f, bc.data(), w, h, ref.data(), w * 4);
        std::vector<uint8_t> astc(astc_level_bytes(w, h));
        bc_to_astc_level(f, bc.data(), w, h, astc.data());
        const uint32_t bw = bc_blocks(w), bh = bc_blocks(h);
        bool subset_ok = true;
        for (uint32_t by = 0; by < bh; by++) for (uint32_t bx = 0; bx < bw; bx++) {
            uint8_t blk[64];
            if (!astc_decode_block_subset(&astc[((size_t)by * bw + bx) * 16], blk)) { subset_ok = false; continue; }
            for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {
                uint32_t x = bx * 4 + i, y = by * 4 + j;
                if (x < w && y < h) memcpy(&mine[((size_t)y * w + x) * 4], blk + (j * 4 + i) * 4, 4);
            }
        }
        std::string ap = work + "/t.astc", pp = work + "/t.png", rp = work + "/t.rgba";
        FILE *o = fopen(ap.c_str(), "wb");
        uint8_t hdr[16] = {0x13, 0xAB, 0xA1, 0x5C, 4, 4, 1, (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)(w >> 16), (uint8_t)h, (uint8_t)(h >> 8), (uint8_t)(h >> 16), 1, 0, 0};
        fwrite(hdr, 1, 16, o);
        fwrite(astc.data(), 1, astc.size(), o);
        fclose(o);
        std::string cmd = enc + " -dl " + ap + " " + pp + " -silent >/dev/null 2>&1 && magick " + pp + " -depth 8 rgba:" + rp;
        if (system(cmd.c_str()) != 0) { fprintf(stderr, "%s: astcenc decode failed\n", base); fails++; continue; }
        std::vector<uint8_t> raw;
        if (!read_file(rp.c_str(), &raw) || raw.size() != arm.size()) { fprintf(stderr, "%s: bad decode size\n", base); fails++; continue; }
        arm = raw;
        double se = 0;
        int md = 0;
        for (size_t i = 0; i < ref.size(); i++) {
            int e = (int)arm[i] - ref[i];
            if (fmt == 1 && i % 4 == 3) e = 0;   // DXT1 alpha: 1-bit punch-through, compared separately by the unit tests
            se += (double)e * e;
            md = std::max(md, abs((int)arm[i] - (int)mine[i]));
        }
        double mse = se / ref.size(), psnr = mse > 0 ? 10 * log10(255.0 * 255.0 / mse) : 99;
        worst_psnr = std::min(worst_psnr, psnr);
        worst_ref = std::max(worst_ref, md);
        sum_psnr += psnr;
        n++;
        if (psnr < 30 || md > 1 || !subset_ok) printf("%-28s PSNR %.1f dB, astcenc vs in-tree decoder max diff %d%s\n", base, psnr, md, subset_ok ? "" : " (non-subset block)");
    }
    printf("%d levels: mean PSNR %.1f dB, worst %.1f dB; astcenc vs in-tree decoder worst max diff %d; %d decode failures\n", n,
           n ? sum_psnr / n : 0, worst_psnr, worst_ref, fails);
    return fails || worst_ref > 1 ? 1 : 0;
}
