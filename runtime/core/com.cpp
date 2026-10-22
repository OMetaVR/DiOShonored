#include "dish/com.h"
#include "dish/log.h"
#include "dish/thunk.h"
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "dish/heap.h"

namespace dish {
ComObjEntry g_com_table[kComMaxObjs];
namespace {
std::mutex g_com_mu;
std::map<std::string, uint32_t> g_vtables;
uint32_t g_next_id = 0;
std::vector<uint32_t> g_free_ids;
size_t g_live = 0;
}  // namespace

uint32_t com_define_interface(const char *iface, const ComMethod *methods, size_t n) {
    std::lock_guard<std::mutex> lk(g_com_mu);
    auto it = g_vtables.find(iface);
    if (it != g_vtables.end()) return it->second;
    uint32_t vt = heap_alloc(heap_process(), 0, (uint32_t)(n * 4));
    if (!vt) fatal("com_define_interface(%s): out of guest memory", iface);
    static std::vector<std::string> *names = new std::vector<std::string>();   // keep names alive for the thunk table
    for (size_t i = 0; i < n; i++) {
        names->push_back(std::string(iface) + "::" + methods[i].name);
        uint32_t idx = thunk_alloc(methods[i].fn, TF_COM, (uint32_t)i, "com", names->back().c_str());
        *(uint32_t *)(guest_base + vt + 4 * i) = thunk_addr(idx);
    }
    g_vtables[iface] = vt;
    return vt;
}

uint32_t com_vtable_of(const char *iface) {
    std::lock_guard<std::mutex> lk(g_com_mu);
    auto it = g_vtables.find(iface);
    return it == g_vtables.end() ? 0 : it->second;
}

uint32_t com_new_object(uint32_t vtbl, void *host, void (*destroy)(void *)) {
    uint32_t g = heap_alloc(heap_process(), HEAP_ZERO_MEMORY, 16);
    if (!g) return 0;
    std::lock_guard<std::mutex> lk(g_com_mu);
    uint32_t id;
    if (!g_free_ids.empty()) { id = g_free_ids.back(); g_free_ids.pop_back(); }
    else if (g_next_id < kComMaxObjs) id = g_next_id++;
    else { heap_free(heap_process(), 0, g); LOGE(LC_HLE, "COM object table full"); return 0; }
    g_com_table[id].host = host; g_com_table[id].destroy = destroy; g_com_table[id].guest = g;
    __atomic_store_n(&g_com_table[id].live, true, __ATOMIC_RELEASE);
    uint32_t *w = (uint32_t *)(guest_base + g);
    w[0] = vtbl; w[1] = id; w[2] = 1; w[3] = 0xC0DEC0DE;
    g_live++;
    return g;
}

uint32_t com_addref(uint32_t g) { return __atomic_add_fetch((uint32_t *)(guest_base + g + 8), 1, __ATOMIC_ACQ_REL); }
uint32_t com_refcount(uint32_t g) { return *(uint32_t *)(guest_base + g + 8); }
uint32_t com_release(uint32_t g) {
    uint32_t r = __atomic_sub_fetch((uint32_t *)(guest_base + g + 8), 1, __ATOMIC_ACQ_REL);
    if (r == 0) {
        void *host = nullptr;
        void (*d)(void *) = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_com_mu);
            uint32_t id = *(uint32_t *)(guest_base + g + 4);
            host = g_com_table[id].host;
            d = g_com_table[id].destroy;
            __atomic_store_n(&g_com_table[id].live, false, __ATOMIC_RELEASE);
            g_free_ids.push_back(id);
            g_live--;
        }
        *(uint32_t *)(guest_base + g + 12) = 0;
        if (d) d(host);
        heap_free(heap_process(), 0, g);
    }
    return r;
}
size_t com_live_objects() {
    std::lock_guard<std::mutex> lk(g_com_mu);
    return g_live;
}

}  // namespace dish
