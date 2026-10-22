#include "dish/gamefiles.h"
#include "dish/sha256.h"
#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace dish {
namespace {

std::string lower(std::string s) {
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool ci_child(const std::string &dir, const std::string &name, std::string *real) {
    std::string direct = dir + "/" + name;
    struct stat st;
    if (stat(direct.c_str(), &st) == 0) { *real = name; return true; }
    DIR *d = opendir(dir.c_str());
    if (!d) return false;
    std::string want = lower(name);
    bool found = false;
    while (dirent *e = readdir(d)) {
        if (lower(e->d_name) == want) { *real = e->d_name; found = true; break; }
    }
    closedir(d);
    return found;
}

bool is_dir(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

}  // namespace

bool gamefiles_resolve(const std::string &root, const std::string &relpath, std::string *host) {
    std::string cur = root;
    size_t i = 0;
    while (i < relpath.size()) {
        size_t j = relpath.find_first_of("/\\", i);
        std::string comp = relpath.substr(i, j == std::string::npos ? std::string::npos : j - i);
        i = j == std::string::npos ? relpath.size() : j + 1;
        if (comp.empty() || comp == ".") continue;
        std::string real;
        if (!ci_child(cur, comp, &real)) return false;
        cur += "/" + real;
    }
    if (host) *host = cur;
    return true;
}

std::string gamefiles_find_root(const std::string &search_dir, const std::string &marker) {
    auto has = [&](const std::string &d) { return gamefiles_resolve(d, marker, nullptr); };
    if (has(search_dir)) return search_dir;
    auto children = [](const std::string &d) {
        std::vector<std::string> v;
        if (DIR *dp = opendir(d.c_str())) {
            while (dirent *e = readdir(dp)) {
                if (e->d_name[0] == '.') continue;
                std::string p = d + "/" + e->d_name;
                if (is_dir(p)) v.push_back(p);
            }
            closedir(dp);
        }
        std::sort(v.begin(), v.end());
        return v;
    };
    auto l1 = children(search_dir);
    for (auto &c : l1) if (has(c)) return c;
    for (auto &c : l1)
        for (auto &g : children(c)) if (has(g)) return g;
    return "";
}

bool gamefiles_verify(const GuestProgram &prog, const std::string &root, bool hash, std::vector<FileCheck> *out,
                      const std::function<void(size_t, size_t)> &progress) {
    out->clear();
    bool all = true;
    for (uint32_t i = 0; i < prog.n_modules; i++) {
        const GuestModuleDesc &d = prog.modules[i];
        FileCheck fc;
        fc.name = d.name ? d.name : "";
        fc.relpath = d.relpath ? d.relpath : fc.name;
        if (progress) progress(i, prog.n_modules);
        if (!gamefiles_resolve(root, fc.relpath, &fc.host) || is_dir(fc.host)) {
            fc.state = FileCheck::Missing;
            fc.host.clear();
        } else {
            struct stat st;
            fc.size = stat(fc.host.c_str(), &st) == 0 ? (uint64_t)st.st_size : 0;
            fc.state = FileCheck::Ok;
            if (hash) {
                int fd = open(fc.host.c_str(), O_RDONLY);
                if (fd < 0) fc.state = FileCheck::Unreadable;
                else {
                    Sha256 sh;
                    std::vector<uint8_t> buf(1 << 20);
                    ssize_t n;
                    while ((n = read(fd, buf.data(), buf.size())) > 0) sh.update(buf.data(), (size_t)n);
                    close(fd);
                    if (n < 0) fc.state = FileCheck::Unreadable;
                    else {
                        uint8_t h[32];
                        sh.finish(h);
                        if (memcmp(h, d.sha256, 32) != 0) fc.state = FileCheck::Mismatch;
                    }
                }
            }
        }
        if (fc.state != FileCheck::Ok) all = false;
        out->push_back(std::move(fc));
    }
    if (progress) progress(prog.n_modules, prog.n_modules);
    return all;
}

}  // namespace dish
