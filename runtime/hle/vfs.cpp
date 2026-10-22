#include "dish/vfs.h"
#include "dish/config.h"
#include "dish/log.h"
#include "dish/util.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>

namespace dish {

RuntimeConfig &config() {
    static RuntimeConfig c;
    return c;
}

namespace {

struct Mount {
    std::string guest;      // canonical, no trailing '\'
    std::string lower;
    std::vector<std::string> roots;   // host directories, searched in order; roots[0] is the write target when writable
    bool writable;
};

std::mutex g_mu;
std::vector<Mount> g_mounts;
std::set<std::string> g_virtual_dirs;          // lower-case canonical ancestors of mounts
std::unordered_map<std::string, std::string> g_virtual_real;   // lower -> display name
std::string g_cwd;

struct DirCache {
    std::unordered_map<std::string, std::string> names;   // lower -> real
    std::chrono::steady_clock::time_point t;
};
std::mutex g_dmu;
std::unordered_map<std::string, DirCache> g_dc;

DirCache &scan_dir(const std::string &dir) {   // g_dmu held
    DirCache &dc = g_dc[dir];
    dc.names.clear();
    dc.t = std::chrono::steady_clock::now();
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            dc.names.emplace(lower_utf8(e->d_name), e->d_name);
        }
        closedir(d);
    }
    return dc;
}

bool ci_find(const std::string &dir, const std::string &comp, std::string *real) {
    std::string lc = lower_utf8(comp);
    std::lock_guard<std::mutex> lk(g_dmu);
    auto it = g_dc.find(dir);
    DirCache *dc = it == g_dc.end() ? &scan_dir(dir) : &it->second;
    auto f = dc->names.find(lc);
    if (f == dc->names.end()) {
        auto age = std::chrono::steady_clock::now() - dc->t;
        if (age < std::chrono::seconds(2)) return false;
        dc = &scan_dir(dir);
        f = dc->names.find(lc);
        if (f == dc->names.end()) return false;
    }
    *real = f->second;
    return true;
}

void add_mount(const std::string &guest, std::vector<std::string> roots, bool writable) {
    Mount m;
    m.guest = guest;
    m.lower = lower_utf8(guest);
    m.roots = std::move(roots);
    m.writable = writable;
    g_mounts.push_back(std::move(m));
}

void mkdirs(const std::string &path) {
    std::string cur;
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        cur = path.substr(0, j);
        if (!cur.empty()) mkdir(cur.c_str(), 0755);
        i = j + 1;
    }
}

std::vector<std::string> split(const std::string &canon) {   // components after "C:"
    std::vector<std::string> v;
    size_t i = 2;
    while (i < canon.size()) {
        if (canon[i] == '\\') { i++; continue; }
        size_t j = canon.find('\\', i);
        if (j == std::string::npos) j = canon.size();
        v.push_back(canon.substr(i, j - i));
        i = j;
    }
    return v;
}

const Mount *find_mount(const std::string &canon, size_t *rest_pos) {
    std::string lc = lower_utf8(canon);
    const Mount *best = nullptr;
    for (auto &m : g_mounts) {
        if (lc.size() >= m.lower.size() && lc.compare(0, m.lower.size(), m.lower) == 0 &&
            (lc.size() == m.lower.size() || lc[m.lower.size()] == '\\')) {
            if (!best || m.lower.size() > best->lower.size()) best = &m;
        }
    }
    if (best) *rest_pos = best->guest.size();
    return best;
}

// Walk `comps` below `root`, case-insensitively. Returns 0 = found, 1 = last component missing (parent exists), 2 = missing deeper
int walk(const std::string &root, const std::vector<std::string> &comps, std::string *out) {
    std::string cur = root;
    for (size_t i = 0; i < comps.size(); i++) {
        std::string real;
        if (!ci_find(cur, comps[i], &real)) {
            if (i + 1 == comps.size()) { *out = cur + "/" + comps[i]; return 1; }
            return 2;
        }
        cur += "/" + real;
    }
    *out = cur;
    return 0;
}

}  // namespace

void vfs_clear_cache() {
    std::lock_guard<std::mutex> lk(g_dmu);
    g_dc.clear();
}

std::string vfs_game_dir() { return config().guest_game_dir; }

void vfs_init() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_mounts.clear();
    g_virtual_dirs.clear();
    vfs_clear_cache();
    const RuntimeConfig &c = config();
    std::string sd = c.save_dir;
    if (sd.empty()) sd = "./dishonored_save";
    mkdirs(sd);
    auto sub = [&](const char *p) { std::string d = sd + "/" + p; mkdirs(d); return d; };
    std::string user = c.guest_user_dir;
    if (!c.game_root.empty()) {
        std::vector<std::string> roots;
        if (c.game_root_writes_to_overlay) roots.push_back(sub("GameOverlay"));
        roots.push_back(c.game_root);
        add_mount(c.guest_game_dir, roots, c.game_root_writes_to_overlay);
    }
    add_mount(user + "\\Documents", {sub("Documents")}, true);
    add_mount(user + "\\AppData\\Local", {sub("AppData/Local")}, true);
    add_mount(user + "\\AppData\\Roaming", {sub("AppData/Roaming")}, true);
    add_mount(user, {sub("UserProfile")}, true);
    add_mount("C:\\ProgramData", {sub("ProgramData")}, true);
    add_mount("C:\\Windows\\Temp", {sub("AppData/Local/Temp")}, true);
    add_mount("C:\\Windows", {sub("Windows")}, true);
    sub("AppData/Local/Temp");
    for (auto &m : g_mounts) {
        std::string p = m.guest;
        while (true) {
            size_t k = p.rfind('\\');
            if (k == std::string::npos) break;
            p = p.substr(0, k);
            std::string dir = p.size() == 2 ? p + "\\" : p;   // "C:" -> "C:\"
            g_virtual_dirs.insert(lower_utf8(dir));
            g_virtual_real.emplace(lower_utf8(dir), dir);
            if (p.size() <= 2) break;
        }
    }
    g_cwd = c.guest_game_dir + "\\Binaries\\Win32";
}

std::string vfs_getcwd() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_cwd.empty() ? "C:\\" : g_cwd;
}

std::string vfs_canon(const std::string &in) {
    std::string p = in;
    for (auto &c : p) if (c == '/') c = '\\';
    if (p.compare(0, 4, "\\\\?\\") == 0 || p.compare(0, 4, "\\\\.\\") == 0 || p.compare(0, 4, "\\??\\") == 0) p = p.substr(4);
    std::string base;
    if (p.size() >= 2 && p[1] == ':' && isalpha((unsigned char)p[0])) {
        base = std::string(1, (char)toupper((unsigned char)p[0])) + ":";
        p = p.substr(2);
        if (p.empty() || p[0] != '\\') {   // drive-relative: use cwd if same drive
            std::string cwd = vfs_getcwd();
            if (cwd.compare(0, 2, base) == 0) p = cwd.substr(2) + "\\" + p;
            else p = "\\" + p;
        }
    } else if (!p.empty() && p[0] == '\\') {
        base = "C:";
    } else {
        std::string cwd = vfs_getcwd();
        base = cwd.substr(0, 2);
        p = cwd.substr(2) + "\\" + p;
    }
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find('\\', i);
        if (j == std::string::npos) j = p.size();
        std::string comp = p.substr(i, j - i);
        i = j + 1;
        if (comp.empty() || comp == ".") continue;
        if (comp == "..") { if (!out.empty()) out.pop_back(); continue; }
        // Windows strips trailing dots and spaces
        while (!comp.empty() && (comp.back() == '.' || comp.back() == ' ')) comp.pop_back();
        if (comp.empty()) continue;
        out.push_back(comp);
    }
    std::string r = base;
    if (out.empty()) return r + "\\";
    for (auto &c : out) r += "\\" + c;
    return r;
}

bool vfs_setcwd(const std::string &path) {
    std::string c = vfs_canon(path);
    std::string host;
    bool isd = false;
    if (!vfs_resolve(c, &host, &isd) || !isd) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    g_cwd = c;
    return true;
}

bool vfs_resolve(const std::string &canon, std::string *host, bool *is_dir) {
    std::lock_guard<std::mutex> lk(g_mu);
    size_t rest;
    const Mount *m = find_mount(canon, &rest);
    if (!m) {
        std::string lc = lower_utf8(canon);
        if (lc.size() > 3 && lc.back() == '\\') lc.pop_back();
        if (g_virtual_dirs.count(lc)) { host->clear(); *is_dir = true; return true; }
        return false;
    }
    std::string sub = canon.substr(rest);
    std::vector<std::string> comps;
    {
        std::string tmp = "C:" + (sub.empty() ? std::string() : sub);
        comps = split(tmp);
    }
    for (auto &root : m->roots) {
        std::string out;
        if (walk(root, comps, &out) == 0) {
            struct stat st;
            if (stat(out.c_str(), &st) != 0) continue;
            *host = out;
            *is_dir = S_ISDIR(st.st_mode);
            return true;
        }
    }
    return false;
}

bool vfs_resolve_create(const std::string &canon, std::string *host, uint32_t *err) {
    std::lock_guard<std::mutex> lk(g_mu);
    size_t rest;
    const Mount *m = find_mount(canon, &rest);
    if (!m || !m->writable) { *err = m ? 5 : 3; return false; }
    std::string sub = canon.substr(rest);
    auto comps = split("C:" + sub);
    if (comps.empty()) { *err = 5; return false; }
    std::string leaf = comps.back();
    comps.pop_back();
    // Parent must exist in some root (the write root mirrors it if needed)
    std::string parent_real;   // path relative to a root, built with real names
    std::string wroot = m->roots[0];
    std::string cur_w = wroot;
    bool parent_found = comps.empty();
    for (auto &root : m->roots) {
        std::string out;
        if (comps.empty() || walk(root, comps, &out) == 0) {
            parent_found = true;
            if (!comps.empty()) {
                // mirror real names into the write root
                std::string rel = out.substr(root.size());
                cur_w = wroot + rel;
                mkdirs(cur_w);
                vfs_clear_cache();
            }
            break;
        }
    }
    if (!parent_found) { *err = 3; return false; }
    std::string real;
    if (ci_find(cur_w, leaf, &real)) *host = cur_w + "/" + real;
    else *host = cur_w + "/" + leaf;
    return true;
}

bool vfs_writable_for_existing(const std::string &canon, std::string *host, uint32_t *err) {
    std::string h;
    bool isd;
    if (!vfs_resolve(canon, &h, &isd)) { *err = 2; return false; }
    std::lock_guard<std::mutex> lk(g_mu);
    size_t rest;
    const Mount *m = find_mount(canon, &rest);
    if (!m || !m->writable) { *err = 5; return false; }
    if (h.compare(0, m->roots[0].size(), m->roots[0]) == 0) { *host = h; return true; }
    // exists only in a read-only root: copy up into the write root
    std::string rel = h.substr(m->roots.back().size());
    std::string dst = m->roots[0] + rel;
    size_t k = dst.rfind('/');
    mkdirs(dst.substr(0, k));
    vfs_clear_cache();
    if (isd) { mkdirs(dst); *host = dst; return true; }
    FILE *in = fopen(h.c_str(), "rb");
    FILE *out = fopen(dst.c_str(), "wb");
    if (!in || !out) { if (in) fclose(in); if (out) fclose(out); *err = 5; return false; }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    vfs_clear_cache();
    *host = dst;
    return true;
}

void vfs_note_change(const std::string &host_path) {
    size_t k = host_path.rfind('/');
    std::lock_guard<std::mutex> lk(g_dmu);
    if (k != std::string::npos) g_dc.erase(host_path.substr(0, k));
    g_dc.erase(host_path);
}

bool vfs_mkdir(const std::string &canon, uint32_t *err) {
    std::string h;
    bool isd;
    if (vfs_resolve(canon, &h, &isd)) { *err = 183; return false; }   // ERROR_ALREADY_EXISTS
    if (!vfs_resolve_create(canon, &h, err)) return false;
    if (mkdir(h.c_str(), 0755) != 0) { *err = errno == ENOENT ? 3 : errno == EEXIST ? 183 : 5; return false; }
    vfs_note_change(h);
    return true;
}

bool vfs_list(const std::string &canon, std::vector<DirEntry> *out) {
    std::string host;
    bool isd = false;
    if (!vfs_resolve(canon, &host, &isd) || !isd) return false;
    std::set<std::string> seen;
    auto add_dir = [&](const std::string &d) {
        DIR *dir = opendir(d.c_str());
        if (!dir) return;
        while (dirent *e = readdir(dir)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            std::string lc = lower_utf8(e->d_name);
            if (!seen.insert(lc).second) continue;
            struct stat st;
            std::string full = d + "/" + e->d_name;
            if (stat(full.c_str(), &st) != 0) continue;
            DirEntry de;
            de.name = e->d_name;
            de.is_dir = S_ISDIR(st.st_mode);
            de.size = de.is_dir ? 0 : (uint64_t)st.st_size;
#ifdef __APPLE__
            de.mtime_ns = (int64_t)st.st_mtimespec.tv_sec * 1000000000ll + st.st_mtimespec.tv_nsec;
#else
            de.mtime_ns = (int64_t)st.st_mtim.tv_sec * 1000000000ll + st.st_mtim.tv_nsec;
#endif
            out->push_back(de);
        }
        closedir(dir);
    };
    {
        std::lock_guard<std::mutex> lk(g_mu);
        size_t rest;
        const Mount *m = find_mount(canon, &rest);
        if (m) {
            std::string sub = canon.substr(rest);
            auto comps = split("C:" + sub);
            for (auto &root : m->roots) {
                std::string o;
                if (walk(root, comps, &o) == 0) add_dir(o);
            }
        }
        // virtual children: mounts and virtual dirs whose parent is `canon`
        std::string lc = lower_utf8(canon);
        if (lc.back() != '\\') lc += '\\';
        auto consider = [&](const std::string &gl, const std::string &disp) {
            if (gl.size() > lc.size() && gl.compare(0, lc.size(), lc) == 0) {
                size_t j = gl.find('\\', lc.size());
                std::string childl = gl.substr(lc.size(), j == std::string::npos ? std::string::npos : j - lc.size());
                if (!seen.insert(childl).second) return;
                DirEntry de;
                size_t dpos = disp.size() > lc.size() ? lc.size() : 0;
                size_t dj = disp.find('\\', dpos);
                de.name = disp.substr(dpos, dj == std::string::npos ? std::string::npos : dj - dpos);
                de.is_dir = true;
                de.size = 0;
                de.mtime_ns = 0;
                out->push_back(de);
            }
        };
        for (auto &mt : g_mounts) consider(mt.lower, mt.guest);
        for (auto &vd : g_virtual_dirs) consider(vd, g_virtual_real[vd]);
    }
    return true;
}

std::string vfs_known_folder(int csidl) {
    const std::string &u = config().guest_user_dir;
    switch (csidl & 0xFF) {
        case 0x05: return u + "\\Documents";                    // CSIDL_PERSONAL
        case 0x1A: return u + "\\AppData\\Roaming";             // CSIDL_APPDATA
        case 0x1C: return u + "\\AppData\\Local";               // CSIDL_LOCAL_APPDATA
        case 0x23: return "C:\\ProgramData";                    // CSIDL_COMMON_APPDATA
        case 0x2E: return "C:\\Users\\Public\\Documents";       // CSIDL_COMMON_DOCUMENTS
        case 0x28: return u;                                    // CSIDL_PROFILE
        case 0x24: return "C:\\Windows";                        // CSIDL_WINDOWS
        case 0x25: return "C:\\Windows\\System32";              // CSIDL_SYSTEM
        case 0x26: return "C:\\Program Files (x86)";            // CSIDL_PROGRAM_FILES
        case 0x2A: return "C:\\Program Files (x86)";            // CSIDL_PROGRAM_FILESX86
        case 0x00: case 0x10: return u + "\\Desktop";
        case 0x2B: return "C:\\Program Files (x86)\\Common Files";
        default: return "";
    }
}

std::string vfs_host_to_guest(const std::string &host) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto &m : g_mounts)
        for (auto &r : m.roots)
            if (host.compare(0, r.size(), r) == 0 && (host.size() == r.size() || host[r.size()] == '/')) {
                std::string rel = host.substr(r.size());
                for (auto &c : rel) if (c == '/') c = '\\';
                return m.guest + rel;
            }
    return host;
}

}  // namespace dish
