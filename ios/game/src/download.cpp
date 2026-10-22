#include "download.h"
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

struct Entry { uint64_t size; std::string rel; };

int connect_to(const std::string &host, const std::string &port) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) return -1;
    int fd = -1;
    for (addrinfo *a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        timeval tv{30, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

std::string url_escape(const std::string &s) {
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '/' || c == '.' || c == '_' || c == '-') o += (char)c;
        else { char b[4]; snprintf(b, sizeof b, "%%%02X", c); o += b; }
    }
    return o;
}

// GET path (optionally from byte `from`). sink(data, n) gets the body. Returns the HTTP status (0 on a transport error).
template <class Sink> int http_get(const std::string &host, const std::string &port, const std::string &path, uint64_t from, Sink sink) {
    int fd = connect_to(host, port);
    if (fd < 0) return 0;
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n";
    if (from) req += "Range: bytes=" + std::to_string(from) + "-\r\n";
    req += "\r\n";
    if (send(fd, req.data(), req.size(), 0) != (ssize_t)req.size()) { close(fd); return 0; }
    std::vector<char> buf(1 << 20);
    std::string head;
    bool in_body = false;
    int status = 0;
    uint64_t remaining = UINT64_MAX;
    for (;;) {
        ssize_t n = recv(fd, buf.data(), buf.size(), 0);
        if (n <= 0) break;
        size_t off = 0;
        if (!in_body) {
            head.append(buf.data(), (size_t)n);
            size_t e = head.find("\r\n\r\n");
            if (e == std::string::npos) continue;
            sscanf(head.c_str(), "HTTP/%*d.%*d %d", &status);
            size_t cl = head.find("Content-Length:");
            if (cl == std::string::npos) cl = head.find("content-length:");
            if (cl != std::string::npos) remaining = strtoull(head.c_str() + cl + 15, nullptr, 10);
            std::string body = head.substr(e + 4);
            in_body = true;
            if (!body.empty()) {
                if (!sink(body.data(), body.size())) { close(fd); return 0; }
                remaining -= std::min<uint64_t>(remaining, body.size());
            }
            if (remaining == 0) break;
            continue;
        }
        (void)off;
        if (!sink(buf.data(), (size_t)n)) { close(fd); return 0; }
        remaining -= std::min<uint64_t>(remaining, (uint64_t)n);
        if (remaining == 0) break;
    }
    close(fd);
    return remaining == 0 || remaining == UINT64_MAX ? status : 0;
}

void mkdirs(const std::string &path) {
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
}

}  // namespace

void game_download_start(GameDownload &d, const std::string &server, const std::string &dest_root) {
    {
        std::lock_guard<std::mutex> lk(d.mu);
        if (d.running) return;
        d.running = true; d.done = false; d.ok = false; d.error.clear();
        d.bytes_done = d.bytes_total = 0; d.files_done = d.files_total = 0;
    }
    std::thread([&d, server, dest_root] {
        size_t colon = server.rfind(':');
        std::string host = server.substr(0, colon), port = colon == std::string::npos ? "8791" : server.substr(colon + 1);
        auto fail = [&](const std::string &e) {
            std::lock_guard<std::mutex> lk(d.mu);
            d.error = e; d.running = false; d.done = true; d.ok = false;
        };
        const std::string marker = dest_root + "/" + kGameDownloadCompleteMarker;
        mkdirs(marker);
        unlink(marker.c_str());   // the game must not start from a partial copy
        std::string manifest;
        int st = http_get(host, port, "/manifest.tsv", 0, [&](const char *p, size_t n) { manifest.append(p, n); return true; });
        if (st != 200) { fail("cannot reach " + server + " (is tools/dev/game_files_server.py running?)"); return; }
        std::vector<Entry> files;
        uint64_t total = 0;
        for (size_t pos = 0; pos < manifest.size();) {
            size_t e = manifest.find('\n', pos);
            if (e == std::string::npos) e = manifest.size();
            std::string line = manifest.substr(pos, e - pos);
            pos = e + 1;
            size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;
            files.push_back({strtoull(line.c_str(), nullptr, 10), line.substr(tab + 1)});
            total += files.back().size;
        }
        {
            std::lock_guard<std::mutex> lk(d.mu);
            d.files_total = files.size();
            d.bytes_total = total;
        }
        std::atomic<size_t> next{0};
        std::atomic<bool> failed{false};
        std::string first_error;
        std::mutex err_mu;
        auto t0 = std::chrono::steady_clock::now();
        auto worker = [&] {
            for (;;) {
                size_t i = next++;
                if (i >= files.size() || failed) return;
                const Entry &f = files[i];
                std::string dst = dest_root + "/" + f.rel, part = dst + ".part";
                struct stat sb;
                if (stat(dst.c_str(), &sb) == 0 && (uint64_t)sb.st_size == f.size) {
                    std::lock_guard<std::mutex> lk(d.mu);
                    d.files_done++; d.bytes_done += f.size;
                    continue;
                }
                mkdirs(dst);
                uint64_t have = stat(part.c_str(), &sb) == 0 ? (uint64_t)sb.st_size : 0;
                if (have > f.size) have = 0;
                bool ok = false;
                for (int attempt = 0; attempt < 4 && !ok; attempt++) {
                    FILE *out = fopen(part.c_str(), have ? "ab" : "wb");
                    if (!out) break;
                    { std::lock_guard<std::mutex> lk(d.mu); d.current = f.rel; d.bytes_done += have; }
                    uint64_t got = have;
                    int code = have == f.size ? 206 : http_get(host, port, "/f/" + url_escape(f.rel), have, [&](const char *p, size_t n) {
                        if (fwrite(p, 1, n, out) != n) return false;
                        got += n;
                        std::lock_guard<std::mutex> lk(d.mu);
                        d.bytes_done += n;
                        return true;
                    });
                    fclose(out);
                    { std::lock_guard<std::mutex> lk(d.mu); d.bytes_done -= got; }
                    if ((code == 200 || code == 206) && got == f.size) ok = rename(part.c_str(), dst.c_str()) == 0;
                    else have = stat(part.c_str(), &sb) == 0 ? (uint64_t)sb.st_size : 0;   // resume on the next attempt
                }
                std::lock_guard<std::mutex> lk(d.mu);
                if (!ok) {
                    failed = true;
                    std::lock_guard<std::mutex> el(err_mu);
                    if (first_error.empty()) first_error = "download failed: " + f.rel;
                    return;
                }
                d.files_done++;
                d.bytes_done += f.size;
                double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                d.mb_per_s = s > 0 ? d.bytes_done / 1e6 / s : 0;
            }
        };
        std::vector<std::thread> ws;
        for (int k = 0; k < 4; k++) ws.emplace_back(worker);
        for (auto &w : ws) w.join();
        if (failed) { fail(first_error); return; }
        if (FILE *m = fopen(marker.c_str(), "w")) {
            fprintf(m, "%zu files, %llu bytes\n", files.size(), (unsigned long long)total);
            fclose(m);
        }
        std::lock_guard<std::mutex> lk(d.mu);
        d.running = false; d.done = true; d.ok = true; d.current.clear();
    }).detach();
}
