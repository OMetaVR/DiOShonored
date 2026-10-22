#include "dish/handles.h"
#include "dish/log.h"
#include <map>
#include <mutex>

namespace dish {
namespace {
std::mutex g_hmu;
std::vector<KObject *> g_slots;     // handle = (index + 1) * 4
std::vector<uint32_t> g_free;
std::mutex g_nmu;
std::map<std::string, KObject *> g_named;   // key: type char + ':' + lower name; weak (objects remove themselves)

std::string nkey(KType t, const std::string &n) { return std::string(1, (char)('0' + (int)t)) + ":" + n; }
}  // namespace

void KObject::release() {
    if (ref.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (!name.empty()) named_remove(this);
        delete this;
    }
}

uint32_t handle_alloc(KObject *obj) {
    std::lock_guard<std::mutex> lk(g_hmu);
    uint32_t idx;
    if (!g_free.empty()) { idx = g_free.back(); g_free.pop_back(); }
    else { idx = (uint32_t)g_slots.size(); g_slots.push_back(nullptr); }
    g_slots[idx] = obj;
    return (idx + 1) * 4;
}

KObject *handle_get(uint32_t h) {
    if (h == 0 || (h & 3)) return nullptr;
    std::lock_guard<std::mutex> lk(g_hmu);
    uint32_t idx = h / 4 - 1;
    if (idx >= g_slots.size() || !g_slots[idx]) return nullptr;
    g_slots[idx]->add_ref();
    return g_slots[idx];
}

bool handle_close(uint32_t h) {
    KObject *o = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_hmu);
        if (h == 0 || (h & 3)) return false;
        uint32_t idx = h / 4 - 1;
        if (idx >= g_slots.size() || !g_slots[idx]) return false;
        o = g_slots[idx];
        g_slots[idx] = nullptr;
        g_free.push_back(idx);
    }
    o->release();
    return true;
}

uint32_t handle_dup(uint32_t h) {
    KObject *o = handle_get(h);   // new ref becomes the new handle's ref
    if (!o) return 0;
    return handle_alloc(o);
}

size_t handle_count() {
    std::lock_guard<std::mutex> lk(g_hmu);
    return g_slots.size() - g_free.size();
}

void handle_table_reset() {
    std::lock_guard<std::mutex> lk(g_hmu);
    for (auto *o : g_slots) if (o) o->release();
    g_slots.clear();
    g_free.clear();
}

KObject *named_lookup(KType t, const std::string &lname) {
    std::lock_guard<std::mutex> lk(g_nmu);
    auto it = g_named.find(nkey(t, lname));
    if (it == g_named.end()) return nullptr;
    // The object may be mid-destruction (ref already 0): only take a reference when it is still alive.
    int r = it->second->ref.load();
    while (r > 0) {
        if (it->second->ref.compare_exchange_weak(r, r + 1)) return it->second;
    }
    return nullptr;
}
void named_insert(KObject *obj) {
    std::lock_guard<std::mutex> lk(g_nmu);
    g_named[nkey(obj->type, obj->name)] = obj;
}
void named_remove(KObject *obj) {
    std::lock_guard<std::mutex> lk(g_nmu);
    auto it = g_named.find(nkey(obj->type, obj->name));
    if (it != g_named.end() && it->second == obj) g_named.erase(it);
}

}  // namespace dish
