// advapi32: in-memory registry and a few identity calls.
#include "dish/hle.h"
#include "dish/sync.h"
#include "dish/thread.h"
#include "dish/util.h"
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace dish {
namespace {

enum : uint32_t {
    ERROR_SUCCESS_ = 0, ERROR_FILE_NOT_FOUND_ = 2, ERROR_ACCESS_DENIED_ = 5, ERROR_MORE_DATA_ = 234, ERROR_NO_MORE_ITEMS_ = 259,
    ERROR_INVALID_HANDLE_ = 6, ERROR_INVALID_PARAMETER_ = 87, ERROR_KEY_DELETED_ = 1018,
    REG_NONE = 0, REG_SZ = 1, REG_EXPAND_SZ = 2, REG_BINARY = 3, REG_DWORD = 4, REG_MULTI_SZ = 7, REG_QWORD = 11,
};

struct RegValue { std::string name; uint32_t type = 0; std::vector<uint8_t> data; };
struct RegNode {
    std::string name;
    std::map<std::string, RegNode> kids;     // lower-case -> node
    std::map<std::string, RegValue> vals;    // lower-case -> value
    bool deleted = false;
};
struct KKey : KObject {
    RegNode *node;
    explicit KKey(RegNode *n) : KObject(KType::Key), node(n) {}
};

std::mutex g_reg_mu;
RegNode g_roots[5];     // HKCR, HKCU, HKLM, HKU, HKCC
bool g_reg_init = false;

RegNode *root_for(uint32_t h) {
    switch (h) {
        case 0x80000000u: return &g_roots[0];
        case 0x80000001u: return &g_roots[1];
        case 0x80000002u: return &g_roots[2];
        case 0x80000003u: return &g_roots[3];
        case 0x80000005u: return &g_roots[4];
    }
    return nullptr;
}

RegNode *open_path(RegNode *base, const std::string &path, bool create) {
    RegNode *cur = base;
    size_t i = 0;
    while (i < path.size()) {
        size_t j = path.find('\\', i);
        if (j == std::string::npos) j = path.size();
        std::string comp = path.substr(i, j - i);
        i = j + 1;
        if (comp.empty()) continue;
        std::string lc = lower_utf8(comp);
        auto it = cur->kids.find(lc);
        if (it == cur->kids.end()) {
            if (!create) return nullptr;
            RegNode &n = cur->kids[lc];
            n.name = comp;
            cur = &n;
        } else cur = &it->second;
    }
    return cur;
}

void set_val(RegNode *n, const std::string &name, uint32_t type, const void *data, size_t len) {
    RegValue &v = n->vals[lower_utf8(name)];
    v.name = name;
    v.type = type;
    v.data.assign((const uint8_t *)data, (const uint8_t *)data + len);
}
void set_sz(RegNode *n, const std::string &name, const std::string &s) {
    std::u16string w = utf8_to_utf16(s);
    w.push_back(0);
    set_val(n, name, REG_SZ, w.data(), w.size() * 2);
}
void set_dw(RegNode *n, const std::string &name, uint32_t d) { set_val(n, name, REG_DWORD, &d, 4); }

void init_defaults() {
    if (g_reg_init) return;
    g_reg_init = true;
    RegNode *cv = open_path(&g_roots[2], "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", true);
    set_sz(cv, "ProductName", "Windows 7 Professional");
    set_sz(cv, "CurrentVersion", "6.1");
    set_sz(cv, "CurrentBuildNumber", "7601");
    set_sz(cv, "CSDVersion", "Service Pack 1");
    set_sz(cv, "SystemRoot", "C:\\Windows");
    RegNode *wv = open_path(&g_roots[2], "SOFTWARE\\Microsoft\\Windows\\CurrentVersion", true);
    set_sz(wv, "ProgramFilesDir", "C:\\Program Files (x86)");
    set_sz(wv, "ProgramFilesDir (x86)", "C:\\Program Files (x86)");
    set_sz(wv, "CommonFilesDir", "C:\\Program Files (x86)\\Common Files");
    RegNode *cpu = open_path(&g_roots[2], "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", true);
    set_sz(cpu, "ProcessorNameString", "Intel(R) Core(TM) i7-2600K CPU @ 3.40GHz");
    set_sz(cpu, "VendorIdentifier", "GenuineIntel");
    set_dw(cpu, "~MHz", 3400);
}

// A strings are cp1252; stored values keep REG_SZ-family data as UTF-16.
bool is_str_type(uint32_t t) { return t == REG_SZ || t == REG_EXPAND_SZ || t == REG_MULTI_SZ; }

uint32_t key_open(uint32_t hkey, const std::string &sub, uint32_t out, bool create, uint32_t disp_ptr) {
    std::lock_guard<std::mutex> lk(g_reg_mu);
    init_defaults();
    RegNode *base = root_for(hkey);
    if (!base) {
        auto k = handle_ref<KKey>(hkey, KType::Key);
        if (!k) return ERROR_INVALID_HANDLE_;
        base = k->node;
    }
    RegNode *n = open_path(base, sub, false);
    uint32_t disposition = 2;   // REG_OPENED_EXISTING_KEY
    if (!n) {
        if (!create) { wr32(out, 0); return ERROR_FILE_NOT_FOUND_; }
        n = open_path(base, sub, true);
        disposition = 1;
    }
    wr32(out, handle_alloc(new KKey(n)));
    if (disp_ptr) wr32(disp_ptr, disposition);
    return ERROR_SUCCESS_;
}
uint32_t adv_RegOpenKeyExW(uint32_t h, uint32_t sub, uint32_t, uint32_t, uint32_t out) { return key_open(h, sub ? read_wstr_utf8(sub) : "", out, false, 0); }
uint32_t adv_RegOpenKeyExA(uint32_t h, uint32_t sub, uint32_t, uint32_t, uint32_t out) { return key_open(h, sub ? read_astr_utf8(sub) : "", out, false, 0); }
uint32_t adv_RegCreateKeyExW(uint32_t h, uint32_t sub, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t out, uint32_t disp) {
    return key_open(h, sub ? read_wstr_utf8(sub) : "", out, true, disp);
}
uint32_t adv_RegCreateKeyExA(uint32_t h, uint32_t sub, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t out, uint32_t disp) {
    return key_open(h, sub ? read_astr_utf8(sub) : "", out, true, disp);
}
uint32_t adv_RegCloseKey(uint32_t h) {
    if (root_for(h)) return ERROR_SUCCESS_;
    return handle_close(h) ? ERROR_SUCCESS_ : ERROR_INVALID_HANDLE_;
}

RegNode *node_of(uint32_t hkey, KRef<KKey> *hold) {
    if (RegNode *r = root_for(hkey)) return r;
    *hold = handle_ref<KKey>(hkey, KType::Key);
    return *hold ? hold->get()->node : nullptr;
}

uint32_t query_value(uint32_t hkey, const std::string &name, uint32_t ptype, uint32_t pdata, uint32_t pcb, bool wide) {
    std::lock_guard<std::mutex> lk(g_reg_mu);
    init_defaults();
    KRef<KKey> hold;
    RegNode *n = node_of(hkey, &hold);
    if (!n) return ERROR_INVALID_HANDLE_;
    auto it = n->vals.find(lower_utf8(name));
    if (it == n->vals.end()) return ERROR_FILE_NOT_FOUND_;
    const RegValue &v = it->second;
    if (ptype) wr32(ptype, v.type);
    std::vector<uint8_t> out = v.data;
    if (!wide && is_str_type(v.type)) {
        std::string a(out.size(), 0);
        int64_t k = wide_to_mb(1252, (const uint16_t *)out.data(), out.size() / 2, (uint8_t *)a.data(), a.size(), nullptr, '?');
        out.assign(a.begin(), a.begin() + (k < 0 ? 0 : k));
    }
    if (!pcb) return pdata ? ERROR_INVALID_PARAMETER_ : ERROR_SUCCESS_;
    uint32_t cap = rd32(pcb);
    wr32(pcb, (uint32_t)out.size());
    if (!pdata) return ERROR_SUCCESS_;
    if (cap < out.size()) return ERROR_MORE_DATA_;
    memcpy(guest_base + pdata, out.data(), out.size());
    return ERROR_SUCCESS_;
}
uint32_t adv_RegQueryValueExW(uint32_t h, uint32_t name, uint32_t, uint32_t t, uint32_t d, uint32_t cb) { return query_value(h, name ? read_wstr_utf8(name) : "", t, d, cb, true); }
uint32_t adv_RegQueryValueExA(uint32_t h, uint32_t name, uint32_t, uint32_t t, uint32_t d, uint32_t cb) { return query_value(h, name ? read_astr_utf8(name) : "", t, d, cb, false); }

uint32_t set_value(uint32_t hkey, const std::string &name, uint32_t type, uint32_t data, uint32_t cb, bool wide) {
    std::lock_guard<std::mutex> lk(g_reg_mu);
    init_defaults();
    KRef<KKey> hold;
    RegNode *n = node_of(hkey, &hold);
    if (!n) return ERROR_INVALID_HANDLE_;
    std::vector<uint8_t> bytes(guest_base + data, guest_base + data + cb);
    if (!wide && is_str_type(type)) {
        std::u16string w(cb, 0);
        int64_t k = mb_to_wide(1252, bytes.data(), cb, (uint16_t *)w.data(), cb, false);
        bytes.assign((uint8_t *)w.data(), (uint8_t *)w.data() + (k < 0 ? 0 : k) * 2);
    }
    set_val(n, name, type, bytes.data(), bytes.size());
    return ERROR_SUCCESS_;
}
uint32_t adv_RegSetValueExW(uint32_t h, uint32_t name, uint32_t, uint32_t type, uint32_t d, uint32_t cb) { return set_value(h, name ? read_wstr_utf8(name) : "", type, d, cb, true); }
uint32_t adv_RegSetValueExA(uint32_t h, uint32_t name, uint32_t, uint32_t type, uint32_t d, uint32_t cb) { return set_value(h, name ? read_astr_utf8(name) : "", type, d, cb, false); }
uint32_t adv_RegDeleteValueW(uint32_t h, uint32_t name) {
    std::lock_guard<std::mutex> lk(g_reg_mu);
    KRef<KKey> hold;
    RegNode *n = node_of(h, &hold);
    if (!n) return ERROR_INVALID_HANDLE_;
    return n->vals.erase(lower_utf8(name ? read_wstr_utf8(name) : "")) ? ERROR_SUCCESS_ : ERROR_FILE_NOT_FOUND_;
}
uint32_t adv_RegEnumValueW(uint32_t h, uint32_t idx, uint32_t name, uint32_t pcname, uint32_t, uint32_t ptype, uint32_t data, uint32_t pcb) {
    std::lock_guard<std::mutex> lk(g_reg_mu);
    KRef<KKey> hold;
    RegNode *n = node_of(h, &hold);
    if (!n) return ERROR_INVALID_HANDLE_;
    if (idx >= n->vals.size()) return ERROR_NO_MORE_ITEMS_;
    auto it = n->vals.begin();
    std::advance(it, idx);
    std::u16string w = utf8_to_utf16(it->second.name);
    if (rd32(pcname) <= w.size()) return ERROR_MORE_DATA_;
    write_wstr(name, rd32(pcname), w);
    wr32(pcname, (uint32_t)w.size());
    if (ptype) wr32(ptype, it->second.type);
    if (pcb) {
        uint32_t cap = rd32(pcb);
        wr32(pcb, (uint32_t)it->second.data.size());
        if (data) {
            if (cap < it->second.data.size()) return ERROR_MORE_DATA_;
            memcpy(guest_base + data, it->second.data.data(), it->second.data.size());
        }
    }
    return ERROR_SUCCESS_;
}
uint32_t adv_RegEnumKeyExW(uint32_t h, uint32_t idx, uint32_t name, uint32_t pcname, uint32_t, uint32_t, uint32_t, uint32_t ftime) {
    std::lock_guard<std::mutex> lk(g_reg_mu);
    init_defaults();
    KRef<KKey> hold;
    RegNode *n = node_of(h, &hold);
    if (!n) return ERROR_INVALID_HANDLE_;
    if (idx >= n->kids.size()) return ERROR_NO_MORE_ITEMS_;
    auto it = n->kids.begin();
    std::advance(it, idx);
    std::u16string w = utf8_to_utf16(it->second.name);
    if (rd32(pcname) <= w.size()) return ERROR_MORE_DATA_;
    write_wstr(name, rd32(pcname), w);
    wr32(pcname, (uint32_t)w.size());
    if (ftime) wr64(ftime, 0);
    return ERROR_SUCCESS_;
}
uint32_t adv_RegQueryInfoKeyW(uint32_t h, uint32_t cls, uint32_t pclslen, uint32_t, uint32_t psub, uint32_t pmaxsub, uint32_t pmaxcls, uint32_t pvals,
                              uint32_t pmaxvname, uint32_t pmaxvdata, uint32_t psec, uint32_t pftime) {
    std::lock_guard<std::mutex> lk(g_reg_mu);
    init_defaults();
    KRef<KKey> hold;
    RegNode *n = node_of(h, &hold);
    if (!n) return ERROR_INVALID_HANDLE_;
    uint32_t maxsub = 0, maxv = 0, maxd = 0;
    for (auto &k : n->kids) maxsub = std::max<uint32_t>(maxsub, (uint32_t)utf8_to_utf16(k.second.name).size());
    for (auto &v : n->vals) {
        maxv = std::max<uint32_t>(maxv, (uint32_t)utf8_to_utf16(v.second.name).size());
        maxd = std::max<uint32_t>(maxd, (uint32_t)v.second.data.size());
    }
    if (cls && pclslen && rd32(pclslen)) wr16(cls, 0);
    if (pclslen) wr32(pclslen, 0);
    if (psub) wr32(psub, (uint32_t)n->kids.size());
    if (pmaxsub) wr32(pmaxsub, maxsub);
    if (pmaxcls) wr32(pmaxcls, 0);
    if (pvals) wr32(pvals, (uint32_t)n->vals.size());
    if (pmaxvname) wr32(pmaxvname, maxv);
    if (pmaxvdata) wr32(pmaxvdata, maxd);
    if (psec) wr32(psec, 0);
    if (pftime) wr64(pftime, 0);
    return ERROR_SUCCESS_;
}
uint32_t adv_RegDeleteKeyW(uint32_t h, uint32_t sub) {
    std::lock_guard<std::mutex> lk(g_reg_mu);
    KRef<KKey> hold;
    RegNode *n = node_of(h, &hold);
    if (!n) return ERROR_INVALID_HANDLE_;
    return n->kids.erase(lower_utf8(read_wstr_utf8(sub))) ? ERROR_SUCCESS_ : ERROR_FILE_NOT_FOUND_;
}
uint32_t adv_RegFlushKey(uint32_t) { return ERROR_SUCCESS_; }

uint32_t adv_GetUserNameW(uint32_t buf, uint32_t psz) {
    std::u16string n = u"Player";
    if (rd32(psz) <= n.size()) { wr32(psz, (uint32_t)n.size() + 1); set_last_error(122); return 0; }
    write_wstr(buf, rd32(psz), n);
    wr32(psz, (uint32_t)n.size() + 1);
    return 1;
}
uint32_t adv_GetUserNameA(uint32_t buf, uint32_t psz) {
    if (rd32(psz) <= 6) { wr32(psz, 7); set_last_error(122); return 0; }
    write_astr(buf, rd32(psz), "Player");
    wr32(psz, 7);
    return 1;
}

}  // namespace

HLE("advapi32.dll", "RegOpenKeyExW", Stdcall, adv_RegOpenKeyExW);
HLE("advapi32.dll", "RegOpenKeyExA", Stdcall, adv_RegOpenKeyExA);
HLE("advapi32.dll", "RegCreateKeyExW", Stdcall, adv_RegCreateKeyExW);
HLE("advapi32.dll", "RegCreateKeyExA", Stdcall, adv_RegCreateKeyExA);
HLE("advapi32.dll", "RegCloseKey", Stdcall, adv_RegCloseKey);
HLE("advapi32.dll", "RegQueryValueExW", Stdcall, adv_RegQueryValueExW);
HLE("advapi32.dll", "RegQueryValueExA", Stdcall, adv_RegQueryValueExA);
HLE("advapi32.dll", "RegSetValueExW", Stdcall, adv_RegSetValueExW);
HLE("advapi32.dll", "RegSetValueExA", Stdcall, adv_RegSetValueExA);
HLE("advapi32.dll", "RegDeleteValueW", Stdcall, adv_RegDeleteValueW);
HLE("advapi32.dll", "RegEnumValueW", Stdcall, adv_RegEnumValueW);
HLE("advapi32.dll", "RegEnumKeyExW", Stdcall, adv_RegEnumKeyExW);
HLE("advapi32.dll", "RegQueryInfoKeyW", Stdcall, adv_RegQueryInfoKeyW);
HLE("advapi32.dll", "RegDeleteKeyW", Stdcall, adv_RegDeleteKeyW);
HLE("advapi32.dll", "RegFlushKey", Stdcall, adv_RegFlushKey);
HLE("advapi32.dll", "GetUserNameW", Stdcall, adv_GetUserNameW);
HLE("advapi32.dll", "GetUserNameA", Stdcall, adv_GetUserNameA);

}  // namespace dish
