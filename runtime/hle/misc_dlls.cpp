// Small DLLs: ole32, oleaut32, shell32, user32 (non-window parts), winmm, wsock32, xinput1_3, d3d9 (stubs), dinput8, dsound,
// iphlpapi, powrprof. Anything not implemented here is bound to an auto-generated stub (see binding.cpp).
#include "dish/heap.h"
#include "dish/hle.h"
#include "dish/platform.h"
#include "dish/thread.h"
#include "dish/util.h"
#include "dish/vfs.h"
#include <cstring>
#include <random>
#include <vector>

namespace dish {
namespace {

constexpr uint32_t S_OK_ = 0, E_FAIL_ = 0x80004005u, REGDB_E_CLASSNOTREG = 0x80040154u;

// ---- ole32
uint32_t ole_CoInitialize(uint32_t) { return S_OK_; }
uint32_t ole_CoInitializeEx(uint32_t, uint32_t) { return S_OK_; }
void ole_CoUninitialize() {}
uint32_t ole_CoCreateGuid(uint32_t out) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    uint64_t a = rng(), b = rng();
    memcpy(guest_base + out, &a, 8);
    memcpy(guest_base + out + 8, &b, 8);
    wr8(out + 7, (rd8(out + 7) & 0x0F) | 0x40);
    wr8(out + 8, (rd8(out + 8) & 0x3F) | 0x80);
    return S_OK_;
}
uint32_t ole_CoCreateInstance(uint32_t clsid, uint32_t, uint32_t, uint32_t iid, uint32_t ppv) {
    if (clsid && iid && ppv) {
        uint32_t obj = 0, hr = 0;
        if (dsound_co_create(clsid, iid, &obj, &hr)) { wr32(ppv, obj); return hr; }
    }
    if (ppv) wr32(ppv, 0);
    LOGD(LC_HLE, "CoCreateInstance(%08x-...) -> REGDB_E_CLASSNOTREG", clsid ? rd32(clsid) : 0);
    return REGDB_E_CLASSNOTREG;
}
uint32_t ole_StringFromGUID2(uint32_t g, uint32_t buf, int32_t cch) {
    if (cch < 39) return 0;
    char tmp[40];
    snprintf(tmp, sizeof tmp, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", rd32(g), rd16(g + 4), rd16(g + 6), rd8(g + 8), rd8(g + 9),
             rd8(g + 10), rd8(g + 11), rd8(g + 12), rd8(g + 13), rd8(g + 14), rd8(g + 15));
    write_wstr(buf, (size_t)cch, utf8_to_utf16(tmp));
    return 39;
}

// ---- oleaut32
uint32_t oa_SysAllocStringLen(uint32_t s, uint32_t len) {
    uint32_t p = heap_alloc(heap_process(), 0, 4 + len * 2 + 2);
    if (!p) return 0;
    wr32(p, len * 2);
    if (s) memcpy(guest_base + p + 4, guest_base + s, len * 2);
    else memset(guest_base + p + 4, 0, len * 2);
    wr16(p + 4 + len * 2, 0);
    return p + 4;
}
uint32_t oa_SysAllocString(uint32_t s) { return s ? oa_SysAllocStringLen(s, (uint32_t)gwcslen(s)) : 0; }
void oa_SysFreeString(uint32_t b) { if (b) heap_free(heap_process(), 0, b - 4); }
uint32_t oa_SysStringLen(uint32_t b) { return b ? rd32(b - 4) / 2 : 0; }

// ---- shell32
uint32_t shell_folder(uint32_t csidl, uint32_t buf, bool wide) {
    std::string p = vfs_known_folder((int)csidl);
    if (p.empty()) return E_FAIL_;
    if (csidl & 0x8000) { uint32_t e; vfs_mkdir(p, &e); }
    if (wide) write_wstr(buf, 260, utf8_to_utf16(p));
    else write_astr(buf, 260, p);
    return S_OK_;
}
uint32_t sh_SHGetFolderPathW(uint32_t, uint32_t csidl, uint32_t, uint32_t, uint32_t buf) { return shell_folder(csidl, buf, true); }
uint32_t sh_SHGetFolderPathA(uint32_t, uint32_t csidl, uint32_t, uint32_t, uint32_t buf) { return shell_folder(csidl, buf, false); }
uint32_t sh_SHGetSpecialFolderPathW(uint32_t, uint32_t buf, uint32_t csidl, uint32_t) { return shell_folder(csidl, buf, true) == S_OK_; }
uint32_t sh_CommandLineToArgvW(uint32_t cmd, uint32_t pargc) {
    std::u16string s = read_wstr(cmd);
    std::vector<std::u16string> args;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == u' ' || s[i] == u'\t')) i++;
        if (i >= s.size()) break;
        std::u16string a;
        bool q = false;
        while (i < s.size() && (q || (s[i] != u' ' && s[i] != u'\t'))) {
            size_t bs = 0;
            while (i < s.size() && s[i] == u'\\') { bs++; i++; }
            if (i < s.size() && s[i] == u'"') {
                a.append(bs / 2, u'\\');
                if (bs % 2) a.push_back(u'"');
                else q = !q;
                i++;
            } else {
                a.append(bs, u'\\');
                if (i < s.size() && (q || (s[i] != u' ' && s[i] != u'\t'))) a.push_back(s[i++]);
            }
        }
        args.push_back(a);
    }
    size_t total = args.size() * 4 + 4;
    for (auto &a : args) total += (a.size() + 1) * 2;
    uint32_t base = heap_alloc(heap_process(), 0, (uint32_t)total);
    uint32_t dst = base + (uint32_t)args.size() * 4 + 4;
    for (size_t k = 0; k < args.size(); k++) {
        wr32(base + (uint32_t)k * 4, dst);
        write_wstr(dst, args[k].size() + 1, args[k]);
        dst += (uint32_t)(args[k].size() + 1) * 2;
    }
    wr32(base + (uint32_t)args.size() * 4, 0);
    wr32(pargc, (uint32_t)args.size());
    return base;
}
uint32_t sh_ShellExecuteW(uint32_t, uint32_t op, uint32_t file, uint32_t, uint32_t, uint32_t) {
    LOGI(LC_HLE, "ShellExecuteW(%s, %s) ignored", op ? read_wstr_utf8(op).c_str() : "", file ? read_wstr_utf8(file).c_str() : "");
    return 33;
}

// ---- winmm waveOut: report no device
uint32_t mm_waveOutOpen(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 4; }   // MMSYSERR_NODRIVER
uint32_t mm_waveOutGetNumDevs() { return 0; }

// ---- wsock32 (no networking)
constexpr int32_t SOCKET_ERROR_ = -1;
constexpr uint32_t WSAENOTSOCK = 10038, WSAEAFNOSUPPORT = 10047, WSAHOST_NOT_FOUND = 11001;
uint32_t ws_WSAStartup(uint32_t ver, uint32_t data) {
    memset(guest_base + data, 0, 400);
    wr16(data, (uint16_t)ver);
    wr16(data + 2, 0x0202);
    write_astr(data + 4, 257, "Dishonored runtime sockets (disabled)");
    write_astr(data + 261, 129, "Running");
    wr16(data + 390, 0);
    return 0;
}
int32_t ws_WSACleanup() { return 0; }
uint32_t ws_WSAGetLastError() { return get_last_error(); }
void ws_WSASetLastError(uint32_t e) { set_last_error(e); }
uint32_t ws_socket(int32_t, int32_t, int32_t) { set_last_error(WSAEAFNOSUPPORT); return 0xFFFFFFFFu; }
int32_t ws_closesocket(uint32_t) { return 0; }
int32_t ws_fail_notsock() { set_last_error(WSAENOTSOCK); return SOCKET_ERROR_; }
int32_t ws_bind(uint32_t, uint32_t, int32_t) { return ws_fail_notsock(); }
int32_t ws_ioctlsocket(uint32_t, uint32_t, uint32_t) { return ws_fail_notsock(); }
int32_t ws_setsockopt(uint32_t, int32_t, int32_t, uint32_t, int32_t) { return ws_fail_notsock(); }
int32_t ws_getsockopt(uint32_t, int32_t, int32_t, uint32_t, uint32_t) { return ws_fail_notsock(); }
int32_t ws_getsockname(uint32_t, uint32_t, uint32_t) { return ws_fail_notsock(); }
int32_t ws_connect(uint32_t, uint32_t, int32_t) { return ws_fail_notsock(); }
int32_t ws_listen(uint32_t, int32_t) { return ws_fail_notsock(); }
uint32_t ws_accept(uint32_t, uint32_t, uint32_t) { set_last_error(WSAENOTSOCK); return 0xFFFFFFFFu; }
int32_t ws_send(uint32_t, uint32_t, int32_t, int32_t) { return ws_fail_notsock(); }
int32_t ws_sendto(uint32_t, uint32_t, int32_t, int32_t, uint32_t, int32_t) { return ws_fail_notsock(); }
int32_t ws_recv(uint32_t, uint32_t, int32_t, int32_t) { return ws_fail_notsock(); }
int32_t ws_recvfrom(uint32_t, uint32_t, int32_t, int32_t, uint32_t, uint32_t) { return ws_fail_notsock(); }
int32_t ws_select(int32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
int32_t ws_gethostname(uint32_t buf, int32_t len) {
    if (len < 14) { set_last_error(10014); return SOCKET_ERROR_; }
    write_astr(buf, (size_t)len, "DISHONORED-PC");
    return 0;
}
uint32_t ws_gethostbyname(uint32_t) { set_last_error(WSAHOST_NOT_FOUND); return 0; }
uint32_t ws_gethostbyaddr(uint32_t, int32_t, int32_t) { set_last_error(WSAHOST_NOT_FOUND); return 0; }
uint32_t ws_htons(uint32_t v) { return (uint16_t)(((v & 0xFF) << 8) | ((v >> 8) & 0xFF)); }
uint32_t ws_htonl(uint32_t v) { return __builtin_bswap32(v); }
uint32_t ws_inet_addr(uint32_t s) {
    unsigned a, b, c, d;
    char tail;
    std::string str = read_astr(s);
    if (sscanf(str.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return 0xFFFFFFFFu;
    return a | (b << 8) | (c << 16) | (d << 24);
}

uint32_t ip_GetAdaptersInfo(uint32_t, uint32_t) { return 232; }   // ERROR_NO_DATA
uint32_t pp_CallNtPowerInformation(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0xC00000BBu; }

}  // namespace

HLE("ole32.dll", "CoInitialize", Stdcall, ole_CoInitialize);
HLE("ole32.dll", "CoInitializeEx", Stdcall, ole_CoInitializeEx);
HLE("ole32.dll", "CoUninitialize", Stdcall, ole_CoUninitialize);
HLE("ole32.dll", "CoCreateGuid", Stdcall, ole_CoCreateGuid);
HLE("ole32.dll", "CoCreateInstance", Stdcall, ole_CoCreateInstance);
HLE("ole32.dll", "StringFromGUID2", Stdcall, ole_StringFromGUID2);
HLE("oleaut32.dll", "SysAllocString", Stdcall, oa_SysAllocString);
HLE("oleaut32.dll", "SysAllocStringLen", Stdcall, oa_SysAllocStringLen);
HLE("oleaut32.dll", "SysFreeString", Stdcall, oa_SysFreeString);
HLE("oleaut32.dll", "SysStringLen", Stdcall, oa_SysStringLen);
HLE("shell32.dll", "SHGetFolderPathW", Stdcall, sh_SHGetFolderPathW);
HLE("shell32.dll", "SHGetFolderPathA", Stdcall, sh_SHGetFolderPathA);
HLE("shell32.dll", "SHGetSpecialFolderPathW", Stdcall, sh_SHGetSpecialFolderPathW);
HLE("shell32.dll", "CommandLineToArgvW", Stdcall, sh_CommandLineToArgvW);
HLE("shell32.dll", "ShellExecuteW", Stdcall, sh_ShellExecuteW);
HLE("winmm.dll", "waveOutOpen", Stdcall, mm_waveOutOpen);
HLE("winmm.dll", "waveOutGetNumDevs", Stdcall, mm_waveOutGetNumDevs);
HLE("wsock32.dll", "WSAStartup", Stdcall, ws_WSAStartup);
HLE("wsock32.dll", "WSACleanup", Stdcall, ws_WSACleanup);
HLE("wsock32.dll", "WSAGetLastError", Stdcall, ws_WSAGetLastError);
HLE("wsock32.dll", "WSASetLastError", Stdcall, ws_WSASetLastError);
HLE("wsock32.dll", "socket", Stdcall, ws_socket);
HLE("wsock32.dll", "closesocket", Stdcall, ws_closesocket);
HLE("wsock32.dll", "bind", Stdcall, ws_bind);
HLE("wsock32.dll", "ioctlsocket", Stdcall, ws_ioctlsocket);
HLE("wsock32.dll", "setsockopt", Stdcall, ws_setsockopt);
HLE("wsock32.dll", "getsockopt", Stdcall, ws_getsockopt);
HLE("wsock32.dll", "getsockname", Stdcall, ws_getsockname);
HLE("wsock32.dll", "connect", Stdcall, ws_connect);
HLE("wsock32.dll", "listen", Stdcall, ws_listen);
HLE("wsock32.dll", "accept", Stdcall, ws_accept);
HLE("wsock32.dll", "send", Stdcall, ws_send);
HLE("wsock32.dll", "sendto", Stdcall, ws_sendto);
HLE("wsock32.dll", "recv", Stdcall, ws_recv);
HLE("wsock32.dll", "recvfrom", Stdcall, ws_recvfrom);
HLE("wsock32.dll", "select", Stdcall, ws_select);
HLE("wsock32.dll", "gethostname", Stdcall, ws_gethostname);
HLE("wsock32.dll", "gethostbyname", Stdcall, ws_gethostbyname);
HLE("wsock32.dll", "gethostbyaddr", Stdcall, ws_gethostbyaddr);
HLE("wsock32.dll", "htons", Stdcall, ws_htons);
HLE("wsock32.dll", "ntohs", Stdcall, ws_htons);
HLE("wsock32.dll", "htonl", Stdcall, ws_htonl);
HLE("wsock32.dll", "ntohl", Stdcall, ws_htonl);
HLE("wsock32.dll", "inet_addr", Stdcall, ws_inet_addr);
HLE("iphlpapi.dll", "GetAdaptersInfo", Stdcall, ip_GetAdaptersInfo);
HLE("powrprof.dll", "CallNtPowerInformation", Stdcall, pp_CallNtPowerInformation);

}  // namespace dish
