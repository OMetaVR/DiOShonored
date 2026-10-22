// HLE registry, signature table, auto-generated stubs and import binding.
#include "dish/hle.h"
#include "dish/thunk.h"
#include "dish/thread.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dish {
namespace {

const HleSig kSigs[] = {
#include "sigs_generated.inc"
};
constexpr size_t kNumSigs = sizeof(kSigs) / sizeof(kSigs[0]);

std::string key(const char *dll, const char *name) { return std::string(dll) + "!" + name; }

struct Registry {
    std::unordered_map<std::string, HleImpl> impls;
    std::unordered_map<std::string, const HleSig *> sigs;
    std::unordered_map<std::string, const HleSig *> sig_ord;   // "dll#ordinal"
    std::unordered_map<std::string, uint32_t> bound;           // dll!name -> thunk index
    std::mutex mu;
    bool sigs_built = false;
    void build_sigs() {
        if (sigs_built) return;
        sigs_built = true;
        for (size_t i = 0; i < kNumSigs; i++) {
            sigs.emplace(key(kSigs[i].dll, kSigs[i].name), &kSigs[i]);
            if (kSigs[i].ordinal) sig_ord.emplace(std::string(kSigs[i].dll) + "#" + std::to_string(kSigs[i].ordinal), &kSigs[i]);
        }
    }
};
StubInfo g_stub_arr[8192];
size_t g_stub_n = 0;

Registry &R() {
    static Registry r;
    return r;
}

GuestRet stub_thunk(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx) {
    StubInfo *s = &g_stub_arr[g_thunks[tl_thunk_idx].aux];
    uint64_t n = __atomic_add_fetch(&s->calls, 1, __ATOMIC_RELAXED);
    g_unimpl_calls.fetch_add(1, std::memory_order_relaxed);
    if (n == 1) {
        const uint32_t *a = (const uint32_t *)(guest_base + esp);
        LOGW(LC_STUB, "UNIMPLEMENTED %s!%s%s (esp=%08x ret=%08x args %08x %08x %08x %08x) -> eax=%u pop=%u", s->dll, s->name,
             s->sig_known ? "" : " [signature unknown, assuming cdecl]", esp, a[0], a[1], a[2], a[3], a[4], s->default_ret, s->pop);
        s->logged = true;
    }
    return guest_ret(s->default_ret, 0, s->pop);
}

}  // namespace

void hle_register(const HleImpl &impl) {
    Registry &r = R();
    std::lock_guard<std::mutex> lk(r.mu);
    auto k = key(impl.dll, impl.name);
    if (r.impls.count(k)) {
        fprintf(stderr, "HLE: duplicate registration of %s\n", k.c_str());
        abort();
    }
    r.impls.emplace(k, impl);
}

const HleImpl *hle_find(const char *dll, const char *name) {
    Registry &r = R();
    std::lock_guard<std::mutex> lk(r.mu);
    auto it = r.impls.find(key(dll, name));
    return it == r.impls.end() ? nullptr : &it->second;
}
size_t hle_impl_count() {
    Registry &r = R();
    std::lock_guard<std::mutex> lk(r.mu);
    return r.impls.size();
}
void hle_for_each(void (*cb)(const HleImpl &, void *), void *user) {
    Registry &r = R();
    std::vector<HleImpl> v;
    {
        std::lock_guard<std::mutex> lk(r.mu);
        for (auto &p : r.impls) v.push_back(p.second);
    }
    for (auto &i : v) cb(i, user);
}

const HleSig *sig_find(const char *dll, const char *name) {
    Registry &r = R();
    std::lock_guard<std::mutex> lk(r.mu);
    r.build_sigs();
    auto it = r.sigs.find(key(dll, name));
    return it == r.sigs.end() ? nullptr : it->second;
}
const HleSig *sig_find_ordinal(const char *dll, uint32_t ordinal) {
    Registry &r = R();
    std::lock_guard<std::mutex> lk(r.mu);
    r.build_sigs();
    auto it = r.sig_ord.find(std::string(dll) + "#" + std::to_string(ordinal));
    return it == r.sig_ord.end() ? nullptr : it->second;
}
const HleSig *sig_table(size_t *n) {
    *n = kNumSigs;
    return kSigs;
}

static bool sig_matches(const HleImpl &impl, const HleSig *sig) {
    if (!sig || sig->pop < 0 || sig->conv == '?') return true;
    return (uint32_t)sig->pop == impl.pop;
}

uint32_t hle_bind(const char *dll, const char *name, uint32_t ordinal, int64_t want_index) {
    Registry &r = R();
    std::string nm;
    if (name) nm = name;
    else {
        const HleSig *s = sig_find_ordinal(dll, ordinal);
        nm = s ? s->name : "ord" + std::to_string(ordinal);
    }
    std::string k = key(dll, nm.c_str());
    std::lock_guard<std::mutex> lk(r.mu);
    r.build_sigs();
    auto b = r.bound.find(k);
    if (b != r.bound.end() && want_index < 0) return b->second;
    ThunkEntry e{};
    // The strings stored in thunk entries must outlive the table: intern through the stub/impl storage.
    static std::deque<std::string> interned;
    interned.push_back(dll);
    const char *dll_i = interned.back().c_str();
    interned.push_back(nm);
    const char *name_i = interned.back().c_str();
    auto ii = r.impls.find(k);
    const HleSig *sig = nullptr;
    {
        auto si = r.sigs.find(k);
        if (si != r.sigs.end()) sig = si->second;
    }
    if (ii != r.impls.end()) {
        if (!sig_matches(ii->second, sig))
            LOGE(LC_HLE, "SIGNATURE MISMATCH %s: implementation pops %u, spec says %d (%c)", k.c_str(), ii->second.pop, sig->pop, sig->conv);
        e = ThunkEntry{ii->second.fn, 0, 0, dll_i, name_i};
    } else {
        StubInfo si{};
        si.dll = dll_i;
        si.name = name_i;
        si.sig_known = sig && sig->conv != '?' && sig->pop >= 0;
        si.pop = si.sig_known ? (uint32_t)sig->pop : 0;
        si.default_ret = sig ? sig->default_ret : 0;
        if (g_stub_n >= 8192) fatal("too many stubs");
        g_stub_arr[g_stub_n] = si;
        e = ThunkEntry{stub_thunk, TF_NEEDS_AUX | TF_STUB, (uint32_t)g_stub_n, dll_i, name_i};
        g_stub_n++;
    }
    uint32_t idx;
    if (want_index >= 0) {
        idx = (uint32_t)want_index;
        thunk_set(idx, e.fn, e.flags, e.aux, e.dll, e.name);
    } else {
        idx = thunk_alloc(e.fn, e.flags, e.aux, e.dll, e.name);
    }
    if (b == r.bound.end()) r.bound.emplace(k, idx);
    return idx;
}

uint32_t hle_dynamic_proc(const char *dll, const char *name, uint32_t ordinal) {
    Registry &r = R();
    {
        std::string nm = name ? name : "";
        if (!name) {
            const HleSig *s = sig_find_ordinal(dll, ordinal);
            if (!s) return 0;
            nm = s->name;
        }
        const HleSig *s = sig_find(dll, nm.c_str());
        bool impl = hle_find(dll, nm.c_str()) != nullptr;
        bool bound;
        {
            std::lock_guard<std::mutex> lk(r.mu);
            bound = r.bound.count(key(dll, nm.c_str())) != 0;
        }
        if (!impl && !bound && !(s && s->needed)) return 0;
        return thunk_addr(hle_bind(dll, nm.c_str(), 0, -1));
    }
}

size_t stub_count() {
    Registry &r = R();
    std::lock_guard<std::mutex> lk(r.mu);
    return g_stub_n;
}
const StubInfo *stub_at(size_t i) {
    Registry &r = R();
    std::lock_guard<std::mutex> lk(r.mu);
    return i < g_stub_n ? &g_stub_arr[i] : nullptr;
}

void stub_report(bool only_called) {
    size_t n = stub_count();
    std::vector<const StubInfo *> v;
    for (size_t i = 0; i < n; i++) {
        const StubInfo *s = stub_at(i);
        if (!only_called || s->calls) v.push_back(s);
    }
    std::sort(v.begin(), v.end(), [](const StubInfo *a, const StubInfo *b) { return a->calls > b->calls; });
    LOGI(LC_STUB, "unimplemented imports: %zu bound stubs, %zu called, %llu total calls", n, v.size(),
         (unsigned long long)g_unimpl_calls.load());
    for (auto *s : v) LOGI(LC_STUB, "  %8llu  %s!%s", (unsigned long long)s->calls, s->dll, s->name);
}

}  // namespace dish
