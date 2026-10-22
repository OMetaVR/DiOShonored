// Persistent shader / pipeline record and shader preparation (see mtl_shadercache.h and dish/shader_prep.h).
#include "mtl_shadercache.h"
#include "mtl_internal.h"
#include "dish/log.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <pthread.h>
#include <sys/stat.h>
#include <thread>
#include <unordered_set>

namespace dish {
namespace mtl {
namespace {

uint64_t now_ns() { return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
uint64_t fnv_bytes(const void *p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}

constexpr uint32_t kRecMagic = 0x32525044;   // 'DPR2'
struct RecHdr { uint32_t magic, nelems; uint64_t vs, ps; PsoKey key; };

std::string shader_dir() { return options().cache_dir + "/shaders"; }
std::string pipeline_file() { return options().cache_dir + "/pipelines.bin"; }
std::string prepared_file() { return options().cache_dir + "/prepared.txt"; }

struct Recorder {
    std::once_flag once;
    std::mutex mu;
    std::unordered_set<uint64_t> shaders, pipelines;
    dispatch_queue_t q = nil;
    bool enabled = false;
};
Recorder &rec() { static Recorder r; return r; }

// Pipeline records in file order; also fills `ids` with each record's dedup id.
std::vector<std::vector<uint8_t>> read_pipelines(std::vector<uint64_t> *ids) {
    std::vector<std::vector<uint8_t>> out;
    FILE *f = fopen(pipeline_file().c_str(), "rb");
    if (!f) return out;
    for (;;) {
        RecHdr h;
        if (fread(&h, sizeof h, 1, f) != 1 || h.magic != kRecMagic || h.nelems > 64) break;
        std::vector<uint8_t> r(sizeof h + h.nelems * sizeof(D3DVERTEXELEMENT9));
        memcpy(r.data(), &h, sizeof h);
        if (h.nelems && fread(r.data() + sizeof h, sizeof(D3DVERTEXELEMENT9), h.nelems, f) != h.nelems) break;
        if (ids) ids->push_back(fnv_bytes(r.data(), r.size()));
        out.push_back(std::move(r));
    }
    fclose(f);
    return out;
}

std::vector<std::pair<uint64_t, bool>> list_shaders() {   // (hash, pixel)
    std::vector<std::pair<uint64_t, bool>> out;
    if (DIR *d = opendir(shader_dir().c_str())) {
        while (dirent *e = readdir(d)) {
            unsigned long long h = 0;
            char ext[4] = {};
            if (sscanf(e->d_name, "%16llx.%2s", &h, ext) == 2 && (!strcmp(ext, "vs") || !strcmp(ext, "ps"))) out.push_back({h, ext[0] == 'p'});
        }
        closedir(d);
    }
    return out;
}

bool read_shader(uint64_t h, bool pixel, std::vector<uint32_t> *bc) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%016llx.%s", shader_dir().c_str(), (unsigned long long)h, pixel ? "ps" : "vs");
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    bool ok = n > 0 && n % 4 == 0;
    if (ok) {
        bc->resize((size_t)n / 4);
        ok = fread(bc->data(), 4, bc->size(), f) == bc->size();
    }
    fclose(f);
    return ok && shader_content_hash(bc->data(), bc->size()) == h;
}

void recorder_init() {
    Recorder &r = rec();
    std::call_once(r.once, [&] {
        if (options().cache_dir.empty()) return;
        mkdir(options().cache_dir.c_str(), 0755);
        mkdir(shader_dir().c_str(), 0755);
        for (auto &s : list_shaders()) r.shaders.insert(s.first);
        std::vector<uint64_t> ids;
        read_pipelines(&ids);
        r.pipelines.insert(ids.begin(), ids.end());
        r.q = dispatch_queue_create("dish.shader-record", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_BACKGROUND, 0));
        r.enabled = true;
        LOGI(LC_GFX, "metal: shader record: %zu shaders, %zu pipelines known", r.shaders.size(), r.pipelines.size());
    });
}

}  // namespace

uint64_t shader_content_hash(const uint32_t *tokens, size_t n) { return fnv_bytes(tokens, n * 4); }
// v2: archives written before depth-only pipelines were excluded cannot be packed again ("expecting 'fragment' stage")
std::string pipeline_archive_path() { return options().cache_dir.empty() ? std::string() : options().cache_dir + "/pipelines-v2.archive"; }

bool compile_shader(id<MTLDevice> md, const uint32_t *tokens, size_t n, ShaderXlate *x, id<MTLLibrary> __strong *lib, id<MTLFunction> __strong *fn,
                    std::string *err, uint64_t *translate_ns, uint64_t *compile_ns) {
    uint64_t t0 = now_ns();
    bool ok = shader_translate(tokens, n, x, err);
    *translate_ns = now_ns() - t0;
    *compile_ns = 0;
    if (!ok) return false;
    @autoreleasepool {
        uint64_t t1 = now_ns();
        MTLCompileOptions *co = [[MTLCompileOptions alloc] init];
        co.fastMathEnabled = YES;
        if (@available(iOS 13.0, *)) co.preserveInvariance = YES;
        NSError *nserr = nil;
        NSString *src = [NSString stringWithUTF8String:x->msl.c_str()];
        *lib = [md newLibraryWithSource:src options:co error:&nserr];
        if (*lib) *fn = [*lib newFunctionWithName:[NSString stringWithUTF8String:x->entry.c_str()]];
        *compile_ns = now_ns() - t1;
        if (!*fn) {
            if (err) *err = nserr ? std::string("MSL compile: ") + [[nserr localizedDescription] UTF8String] : "MSL compile failed";
            return false;
        }
    }
    return true;
}

void record_shader(uint64_t hash, bool pixel, const std::vector<uint32_t> &bytecode) {
    recorder_init();
    Recorder &r = rec();
    if (!r.enabled) return;
    {
        std::lock_guard<std::mutex> lk(r.mu);
        if (!r.shaders.insert(hash).second) return;
    }
    std::vector<uint32_t> copy = bytecode;
    dispatch_async(r.q, ^{
        char path[1024], tmp[1040];
        snprintf(path, sizeof path, "%s/%016llx.%s", shader_dir().c_str(), (unsigned long long)hash, pixel ? "ps" : "vs");
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        if (FILE *f = fopen(tmp, "wb")) {
            bool ok = fwrite(copy.data(), 4, copy.size(), f) == copy.size();
            ok = fclose(f) == 0 && ok;
            if (ok) rename(tmp, path); else remove(tmp);
        }
    });
}

void record_pipeline(const PsoKey &k, uint64_t vs_hash, uint64_t ps_hash, const std::vector<D3DVERTEXELEMENT9> *elems) {
    recorder_init();
    Recorder &r = rec();
    if (!r.enabled || !vs_hash) return;
    RecHdr h{};
    h.magic = kRecMagic;
    h.nelems = elems ? (uint32_t)std::min<size_t>(elems->size(), 64) : 0;
    h.vs = vs_hash;
    h.ps = ps_hash;
    h.key = k;
    h.key.vs = h.key.ps = h.key.decl = 0;   // runtime ids: the hashes above name the shaders
    std::vector<uint8_t> rb(sizeof h + h.nelems * sizeof(D3DVERTEXELEMENT9));
    memcpy(rb.data(), &h, sizeof h);
    if (h.nelems) memcpy(rb.data() + sizeof h, elems->data(), h.nelems * sizeof(D3DVERTEXELEMENT9));
    const uint64_t id = fnv_bytes(rb.data(), rb.size());
    {
        std::lock_guard<std::mutex> lk(r.mu);
        if (!r.pipelines.insert(id).second) return;
    }
    dispatch_async(r.q, ^{
        if (FILE *f = fopen(pipeline_file().c_str(), "ab")) { fwrite(rb.data(), 1, rb.size(), f); fclose(f); }
    });
}

}  // namespace mtl

// ---- shader preparation ---------------------------------------------------------------------------------------------------------------
using namespace mtl;

ShaderPrepSummary shader_prep_summary() {
    ShaderPrepSummary s;
    if (options().cache_dir.empty()) return s;
    s.shaders = (uint32_t)list_shaders().size();
    std::vector<uint64_t> ids;
    s.pipelines = (uint32_t)read_pipelines(&ids).size();
    unsigned ps = 0, pp = 0;
    if (FILE *f = fopen(prepared_file().c_str(), "r")) {
        if (fscanf(f, "%u %u", &ps, &pp) != 2) ps = pp = 0;
        fclose(f);
    }
    s.all_prepared = (s.shaders || s.pipelines) && ps == s.shaders && pp == s.pipelines;
    return s;
}

void shader_prep_start(ShaderPrepStatus *st) {
    std::thread([st] {
        pthread_setname_np("dish-shader-prep");
        id<MTLDevice> md = MTLCreateSystemDefaultDevice();
        const auto shaders = list_shaders();
        const auto pipes = read_pipelines(nullptr);
        st->shaders_total = (uint32_t)shaders.size();
        st->pipelines_total = (uint32_t)pipes.size();
        const uint64_t t0 = now_ns();
        LOGI(LC_GFX, "shader prep: %zu shaders, %zu pipelines recorded%s", shaders.size(), pipes.size(), st->background.load() ? " (background)" : "");
        // the binary archive the device loads (foreground only: in the background the running game's device owns the file)
        id<MTLBinaryArchive> arch = nil;
        std::mutex arch_mu;
        uint32_t arch_added = 0;
        const std::string arch_path = pipeline_archive_path();
        if (@available(iOS 14.0, *)) {
            MTLBinaryArchiveDescriptor *ad = [[MTLBinaryArchiveDescriptor alloc] init];
            struct stat sb;
            if (stat(arch_path.c_str(), &sb) == 0) ad.url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:arch_path.c_str()]];
            NSError *e = nil;
            arch = [md newBinaryArchiveWithDescriptor:ad error:&e];
            if (!arch) { ad.url = nil; arch = [md newBinaryArchiveWithDescriptor:ad error:&e]; }
        }
        auto set_qos = [st] {
            pthread_set_qos_class_self_np(st->background.load() ? QOS_CLASS_UTILITY : QOS_CLASS_USER_INITIATED, 0);
        };
        // phase 1: every recorded shader (warms the compiler cache); phase 2: every recorded pipeline (its shaders hit that cache)
        std::atomic<size_t> next_shader{0}, next_pipe{0};
        auto worker = [&] {
            pthread_setname_np("dish-shader-prep-w");
            for (;;) {
                if (st->cancel.load()) return;
                set_qos();
                size_t i = next_shader++;
                if (i >= shaders.size()) break;
                std::vector<uint32_t> bc;
                ShaderXlate x;
                id<MTLLibrary> lib = nil;
                id<MTLFunction> fn = nil;
                std::string err;
                uint64_t tn = 0, cn = 0;
                if (!read_shader(shaders[i].first, shaders[i].second, &bc) || !compile_shader(md, bc.data(), bc.size(), &x, &lib, &fn, &err, &tn, &cn)) st->failures++;
                st->shaders_done++;
            }
            for (;;) {
                if (st->cancel.load()) return;
                set_qos();
                size_t i = next_pipe++;
                if (i >= pipes.size()) break;
                @autoreleasepool {
                    RecHdr h;
                    memcpy(&h, pipes[i].data(), sizeof h);
                    std::vector<D3DVERTEXELEMENT9> elems(h.nelems);
                    if (h.nelems) memcpy(elems.data(), pipes[i].data() + sizeof h, h.nelems * sizeof(D3DVERTEXELEMENT9));
                    std::vector<uint32_t> vbc, pbc;
                    ShaderXlate vx, px;
                    id<MTLLibrary> vl = nil, pl = nil;
                    id<MTLFunction> vf = nil, pf = nil;
                    std::string err;
                    uint64_t tn = 0, cn = 0;
                    bool ok = read_shader(h.vs, false, &vbc) && compile_shader(md, vbc.data(), vbc.size(), &vx, &vl, &vf, &err, &tn, &cn);
                    if (ok && h.ps) ok = read_shader(h.ps, true, &pbc) && compile_shader(md, pbc.data(), pbc.size(), &px, &pl, &pf, &err, &tn, &cn);
                    if (ok) {
                        MTLRenderPipelineDescriptor *d = make_pso_desc(h.key, vf, vx.inputs, pf, &elems);
                        NSError *e = nil;
                        id<MTLRenderPipelineState> pso = [md newRenderPipelineStateWithDescriptor:d error:&e];
                        ok = pso != nil;
                        if (ok && pf && !st->background.load()) {
                            if (@available(iOS 14.0, *)) {
                                std::lock_guard<std::mutex> lk(arch_mu);
                                if (arch && [arch addRenderPipelineFunctionsWithDescriptor:d error:&e]) arch_added++;
                            }
                        }
                    }
                    if (!ok) st->failures++;
                }
                st->pipelines_done++;
            }
        };
        const unsigned n = std::max(2u, std::min(4u, std::thread::hardware_concurrency()));
        std::vector<std::thread> ws;
        for (unsigned i = 0; i < n; i++) ws.emplace_back(worker);
        for (auto &w : ws) w.join();
        if (@available(iOS 14.0, *)) {
            if (arch && arch_added && !st->background.load()) {
                NSError *e = nil;
                if (![arch serializeToURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:arch_path.c_str()]] error:&e])
                    LOGW(LC_GFX, "shader prep: cannot save the pipeline archive: %s", e ? [[e localizedDescription] UTF8String] : "?");
            }
        }
        const bool complete = !st->cancel.load() && st->shaders_done.load() == shaders.size() && st->pipelines_done.load() == pipes.size();
        if (complete) {
            if (FILE *f = fopen(prepared_file().c_str(), "w")) { fprintf(f, "%zu %zu\n", shaders.size(), pipes.size()); fclose(f); }
        }
        LOGI(LC_GFX, "shader prep: %s in %.1f s: %u shaders, %u pipelines (%u added to the archive), %u failures", complete ? "done" : "stopped",
             (now_ns() - t0) / 1e9, st->shaders_done.load(), st->pipelines_done.load(), arch_added, st->failures.load());
        st->finished = true;
    }).detach();
}

}  // namespace dish
