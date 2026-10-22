// steam_api.dll for a device without a Steam client. The game's files are the user's own, verified by SHA-256 at load (the BYO
// requirement); this layer only lets the Steam build run where no Steam client can exist. It does not impersonate an account or
// ownership: SteamAPI_Init succeeds (Dishonored assumes it does: some engine code, e.g. a pool task enumerating Steam Cloud files,
// uses the interface pointers that are only fetched after a successful init), but the user is signed out (BLoggedOn false, invalid
// SteamID), there are no achievements, DLC, friends or overlay, and no callback is ever delivered. Steam Cloud is a local folder
// (the game keeps its saves there; see cloud_method).
//
// The interfaces are guest objects whose vtables lead to host methods that report "nothing" (false, 0, empty strings, invalid IDs
// and API call handles). Vtable layouts and stack argument sizes come from the interface versions the game was built against
// (steam_ifaces.inc, generated from Open Steamworks headers and checked against the game's call sites): a wrong argument size would
// unbalance the guest stack, so every slot is described exactly.
#include "dish/config.h"
#include "dish/heap.h"
#include "dish/hle.h"
#include "dish/log.h"
#include "dish/thunk.h"
#include "dish/util.h"
#include <atomic>
#include <vector>
#include <unordered_map>
#include <sys/stat.h>
#include <string>
#include <dirent.h>
#include <algorithm>
#include <cstring>
#include <mutex>

namespace dish {
namespace {

enum class SteamRet : uint8_t { Void, U32, U64, Float, Str, Ptr, Struct8 };
struct SteamMethod { const char *name; uint16_t arg_bytes; SteamRet ret; };
struct SteamIface { const char *name; const char *version; const SteamMethod *methods; size_t n; };
#include "steam_ifaces.inc"
constexpr size_t kIfaces = sizeof k_steam_ifaces / sizeof k_steam_ifaces[0];
constexpr uint32_t kDishonoredAppId = 205100;

uint32_t g_objects[kIfaces];
uint32_t g_empty_str;
std::once_flag g_once;
std::atomic<uint8_t> g_logged[kIfaces][64];

// ---- Steam Cloud (ISteamRemoteStorage) on a local folder: Dishonored keeps its saves (DishonoredN.sav, DisAutoSave0/1.sav,
// PROFILE.SAV ...) in Steam Cloud, so without this nothing is ever saved. Files live in <save_dir>/SteamCloud, the same names Steam
// uses in userdata/<id>/205100/remote/ (a player can copy their own PC saves in). Plain local storage: no account is involved.
std::mutex g_cloud_mu;
std::string cloud_dir() {
    std::string d = config().save_dir + "/SteamCloud";
    mkdir(d.c_str(), 0755);
    return d;
}
// guest file name -> host path; only plain names (Steam Cloud names have no directories here)
bool cloud_path(uint32_t gname, std::string *out) {
    if (!gname) return false;
    std::string n = (const char *)(guest_base + gname);
    if (n.empty() || n.size() > 255 || n.find('/') != std::string::npos || n.find('\\') != std::string::npos || n == "." || n == "..") return false;
    *out = cloud_dir() + "/" + n;
    return true;
}
std::vector<std::pair<std::string, uint32_t>> cloud_list() {
    std::vector<std::pair<std::string, uint32_t>> v;
    if (DIR *d = opendir(cloud_dir().c_str())) {
        while (dirent *e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            struct stat st;
            if (stat((cloud_dir() + "/" + e->d_name).c_str(), &st) == 0 && S_ISREG(st.st_mode)) v.push_back({e->d_name, (uint32_t)st.st_size});
        }
        closedir(d);
    }
    std::sort(v.begin(), v.end());
    return v;
}
// Returns true when the method is handled; r/hi receive the result.
bool cloud_method(const char *name, uint32_t esp, uint32_t *r, uint32_t *hi) {
    std::lock_guard<std::mutex> lk(g_cloud_mu);
    auto arg = [&](int i) { return rd32(esp + 4 + 4 * i); };
    std::string path;
    struct stat st;
    if (!strcmp(name, "FileWrite")) {   // (name, data, size) -> bool
        int32_t n = (int32_t)arg(2);
        bool ok = false;
        if (cloud_path(arg(0), &path) && n >= 0) {
            std::string tmp = path + ".tmp";
            if (FILE *f = fopen(tmp.c_str(), "wb")) {
                ok = n == 0 || fwrite(guest_base + arg(1), 1, (size_t)n, f) == (size_t)n;
                ok = fclose(f) == 0 && ok;
                ok = ok && rename(tmp.c_str(), path.c_str()) == 0;
                if (!ok) remove(tmp.c_str());
            }
        }
        LOGI(LC_HLE, "Steam Cloud: write %s (%d bytes)%s", (const char *)(guest_base + arg(0)), n, ok ? "" : " FAILED");
        *r = ok;
    } else if (!strcmp(name, "FileRead")) {   // (name, data, size) -> bytes read
        int32_t n = (int32_t)arg(2), got = 0;
        if (cloud_path(arg(0), &path) && n > 0)
            if (FILE *f = fopen(path.c_str(), "rb")) { got = (int32_t)fread(guest_base + arg(1), 1, (size_t)n, f); fclose(f); }
        *r = (uint32_t)got;
    } else if (!strcmp(name, "FileDelete") || !strcmp(name, "FileForget")) {
        bool ok = cloud_path(arg(0), &path) && (name[4] == 'F' || remove(path.c_str()) == 0);
        if (name[4] == 'D') LOGI(LC_HLE, "Steam Cloud: delete %s%s", (const char *)(guest_base + arg(0)), ok ? "" : " (not found)");
        *r = ok;
    } else if (!strcmp(name, "FileExists") || !strcmp(name, "FilePersisted")) {
        *r = cloud_path(arg(0), &path) && stat(path.c_str(), &st) == 0;
    } else if (!strcmp(name, "GetFileSize")) {
        *r = cloud_path(arg(0), &path) && stat(path.c_str(), &st) == 0 ? (uint32_t)st.st_size : 0;
    } else if (!strcmp(name, "GetFileTimestamp")) {   // int64 seconds since 1970
        uint64_t t = cloud_path(arg(0), &path) && stat(path.c_str(), &st) == 0 ? (uint64_t)st.st_mtime : 0;
        *r = (uint32_t)t; *hi = (uint32_t)(t >> 32);
    } else if (!strcmp(name, "GetFileCount")) {
        *r = (uint32_t)cloud_list().size();
    } else if (!strcmp(name, "GetFileNameAndSize")) {   // (index, int32 *size) -> const char *
        static std::unordered_map<std::string, uint32_t> names;   // guest copies of the names (stay valid, as Steam's do)
        auto v = cloud_list();
        uint32_t i = arg(0);
        if (i >= v.size()) { if (arg(1)) wr32(arg(1), 0); *r = g_empty_str; return true; }
        uint32_t &g = names[v[i].first];
        if (!g) {
            g = heap_alloc(heap_process(), HEAP_ZERO_MEMORY, (uint32_t)v[i].first.size() + 1);
            if (g) memcpy(guest_base + g, v[i].first.c_str(), v[i].first.size() + 1);
        }
        if (arg(1)) wr32(arg(1), v[i].second);
        *r = g ? g : g_empty_str;
    } else if (!strcmp(name, "GetQuota")) {   // (int32 *total, int32 *available) -> bool
        uint64_t used = 0;
        for (auto &e : cloud_list()) used += e.second;
        const uint32_t total = 1u << 30;
        if (arg(0)) wr32(arg(0), total);
        if (arg(1)) wr32(arg(1), used < total ? total - (uint32_t)used : 0);
        *r = 1;
    } else if (!strcmp(name, "IsCloudEnabledForAccount") || !strcmp(name, "IsCloudEnabledForApp")) {
        *r = 1;
    } else if (!strcmp(name, "SetSyncPlatforms")) {
        *r = 1;
    } else if (!strcmp(name, "GetSyncPlatforms")) {
        *r = 0xFFFFFFFFu;   // k_ERemoteStoragePlatformAll
    } else {
        return false;
    }
    return true;
}

GuestRet steam_method(uint32_t esp, uint32_t, uint32_t, uint32_t) {
    const uint32_t aux = thunk_aux();
    const SteamIface &it = k_steam_ifaces[aux >> 8];
    const SteamMethod &m = it.methods[aux & 0xFF];
    if ((aux & 0xFF) < 64 && !g_logged[aux >> 8][aux & 0xFF].exchange(1))
        LOGI(LC_HLE, "Steam (offline) %s::%s", it.version, m.name);
    uint32_t r = 0, hi = 0;
    if (!strcmp(it.name, "RemoteStorage") && cloud_method(m.name, esp, &r, &hi)) return guest_ret(r, hi, m.arg_bytes);
    switch (m.ret) {
        case SteamRet::Void: case SteamRet::U64: case SteamRet::Ptr: break;
        case SteamRet::U32:
            if (!strcmp(it.name, "Utils") && !strcmp(m.name, "GetAppID")) r = kDishonoredAppId;
            break;
        case SteamRet::Float: guest_thread->st0_ret = 0.0; break;
        case SteamRet::Str: r = g_empty_str; break;
        case SteamRet::Struct8: {   // CSteamID / CGameID through the hidden result pointer (first stack argument)
            r = rd32(esp + 4);
            if (r) { wr32(r, 0); wr32(r + 4, 0); }
            break;
        }
    }
    return guest_ret(r, hi, m.arg_bytes);
}

void build_objects() {
    g_empty_str = heap_alloc(heap_process(), HEAP_ZERO_MEMORY, 4);
    for (size_t i = 0; i < kIfaces; i++) {
        const SteamIface &it = k_steam_ifaces[i];
        uint32_t obj = heap_alloc(heap_process(), HEAP_ZERO_MEMORY, 16 + 4 * (uint32_t)it.n);
        uint32_t vt = obj + 16;
        wr32(obj, vt);
        for (size_t s = 0; s < it.n; s++)
            wr32(vt + 4 * (uint32_t)s, thunk_addr(thunk_alloc(steam_method, TF_NEEDS_AUX, (uint32_t)(i << 8 | s), "steam_api.dll", it.methods[s].name)));
        g_objects[i] = obj;
    }
}
uint32_t iface(const char *name) {
    std::call_once(g_once, build_objects);
    for (size_t i = 0; i < kIfaces; i++)
        if (!strcmp(k_steam_ifaces[i].name, name)) return g_objects[i];
    return 0;
}

uint32_t steam_Init() {
    static bool logged = false;
    if (!logged) { logged = true; LOGI(LC_HLE, "SteamAPI_Init: no Steam client; offline Steam layer (signed out: no achievements, cloud saves or overlay)"); }
    return 1;   // bool true
}
void steam_Shutdown() {}
void steam_RunCallbacks() {}
// Callback objects stay registered with nobody: no callback or call result will ever be posted.
void steam_RegisterCallback(uint32_t, uint32_t) {}
void steam_UnregisterCallback(uint32_t) {}
void steam_RegisterCallResult(uint32_t, uint64_t) {}     // (CCallbackBase *, SteamAPICall_t) on the stack: 4 + 8 bytes, cdecl
void steam_UnregisterCallResult(uint32_t, uint64_t) {}
void steam_GameServer_Shutdown() {}
uint32_t steam_GameServer() { return 0; }                 // no game server interface (the game checks it)
uint32_t steam_Apps() { return iface("Apps"); }
uint32_t steam_Friends() { return iface("Friends"); }
uint32_t steam_Matchmaking() { return iface("Matchmaking"); }
uint32_t steam_MatchmakingServers() { return iface("MatchmakingServers"); }
uint32_t steam_Networking() { return iface("Networking"); }
uint32_t steam_RemoteStorage() { return iface("RemoteStorage"); }
uint32_t steam_User() { return iface("User"); }
uint32_t steam_UserStats() { return iface("UserStats"); }
uint32_t steam_Utils() { return iface("Utils"); }

}  // namespace

HLE("steam_api.dll", "SteamAPI_Init", Cdecl, steam_Init);
HLE("steam_api.dll", "SteamAPI_Shutdown", Cdecl, steam_Shutdown);
HLE("steam_api.dll", "SteamAPI_RunCallbacks", Cdecl, steam_RunCallbacks);
HLE("steam_api.dll", "SteamAPI_RegisterCallback", Cdecl, steam_RegisterCallback);
HLE("steam_api.dll", "SteamAPI_UnregisterCallback", Cdecl, steam_UnregisterCallback);
HLE("steam_api.dll", "SteamAPI_RegisterCallResult", Cdecl, steam_RegisterCallResult);
HLE("steam_api.dll", "SteamAPI_UnregisterCallResult", Cdecl, steam_UnregisterCallResult);
HLE("steam_api.dll", "SteamGameServer_Shutdown", Cdecl, steam_GameServer_Shutdown);
HLE("steam_api.dll", "SteamGameServer", Cdecl, steam_GameServer);
HLE("steam_api.dll", "SteamApps", Cdecl, steam_Apps);
HLE("steam_api.dll", "SteamFriends", Cdecl, steam_Friends);
HLE("steam_api.dll", "SteamMatchmaking", Cdecl, steam_Matchmaking);
HLE("steam_api.dll", "SteamMatchmakingServers", Cdecl, steam_MatchmakingServers);
HLE("steam_api.dll", "SteamNetworking", Cdecl, steam_Networking);
HLE("steam_api.dll", "SteamRemoteStorage", Cdecl, steam_RemoteStorage);
HLE("steam_api.dll", "SteamUser", Cdecl, steam_User);
HLE("steam_api.dll", "SteamUserStats", Cdecl, steam_UserStats);
HLE("steam_api.dll", "SteamUtils", Cdecl, steam_Utils);

}  // namespace dish
