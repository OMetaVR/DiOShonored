// kernel32: files, directories, find handles, file mappings, path APIs.
#include "dish/config.h"
#include "dish/hle.h"
#include "dish/sync.h"
#include "dish/thread.h"
#include "dish/util.h"
#include "dish/vfs.h"
#include "dish/watchdog.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef __APPLE__
#define ST_MTIME_NS(st) ((int64_t)(st).st_mtimespec.tv_sec * 1000000000ll + (st).st_mtimespec.tv_nsec)
#define ST_ATIME_NS(st) ((int64_t)(st).st_atimespec.tv_sec * 1000000000ll + (st).st_atimespec.tv_nsec)
#define ST_CTIME_NS(st) ((int64_t)(st).st_ctimespec.tv_sec * 1000000000ll + (st).st_ctimespec.tv_nsec)
#else
#define ST_MTIME_NS(st) ((int64_t)(st).st_mtim.tv_sec * 1000000000ll + (st).st_mtim.tv_nsec)
#define ST_ATIME_NS(st) ((int64_t)(st).st_atim.tv_sec * 1000000000ll + (st).st_atim.tv_nsec)
#define ST_CTIME_NS(st) ((int64_t)(st).st_ctim.tv_sec * 1000000000ll + (st).st_ctim.tv_nsec)
#endif

namespace dish {
namespace {

enum : uint32_t {
    GENERIC_READ = 0x80000000u, GENERIC_WRITE = 0x40000000u, GENERIC_ALL = 0x10000000u,
    FILE_WRITE_DATA = 2, FILE_APPEND_DATA = 4, DELETE_ACC = 0x10000,
    CREATE_NEW = 1, CREATE_ALWAYS = 2, OPEN_EXISTING = 3, OPEN_ALWAYS = 4, TRUNCATE_EXISTING = 5,
    FLAG_OVERLAPPED = 0x40000000, FLAG_BACKUP = 0x02000000, FLAG_DELETE_ON_CLOSE = 0x04000000,
    ATTR_DIR = 0x10, ATTR_NORMAL = 0x80, ATTR_ARCHIVE = 0x20, INVALID_ATTR = 0xFFFFFFFFu,
    ERR_FILE_EXISTS = 80, ERR_ALREADY_EXISTS = 183, ERR_HANDLE_EOF = 38, ERR_NO_MORE_FILES = 18,
};
constexpr uint64_t kEpochDiff100ns = 116444736000000000ull;

uint64_t ns_to_filetime(int64_t ns) { return (uint64_t)(ns / 100) + kEpochDiff100ns; }

struct KFile : KObject {
    int fd = -1;
    std::string host, guest;
    bool overlapped = false, can_read = true, can_write = false, is_std = false, is_char = false, delete_on_close = false, is_dir = false, append = false;
    std::atomic<uint64_t> pos{0};
    KFile() : KObject(KType::File) {}
    ~KFile() override {
        if (fd > 2 || (fd >= 0 && !is_std)) close(fd);
        if (delete_on_close && !host.empty()) { unlink(host.c_str()); vfs_note_change(host); }
    }
};
struct KFind : KObject {
    std::vector<DirEntry> ents;
    size_t idx = 0;
    KFind() : KObject(KType::Find) {}
};
struct KMapping : KObject {
    int fd = -1;
    uint64_t size = 0;
    uint32_t prot = 0;
    bool anon = false;
    KMapping() : KObject(KType::Mapping) {}
    ~KMapping() override { if (fd >= 0) close(fd); }
};

bool is_ro_host(const std::string &host) {
    const std::string &gr = config().game_root;
    return !gr.empty() && host.compare(0, gr.size(), gr) == 0 && (host.size() == gr.size() || host[gr.size()] == '/');
}

uint32_t errno_to_win(int e) {
    switch (e) {
        case ENOENT: return ERR_FILE_NOT_FOUND;
        case ENOTDIR: return ERR_PATH_NOT_FOUND;
        case EACCES: case EPERM: case EROFS: return ERR_ACCESS_DENIED;
        case EEXIST: return ERR_FILE_EXISTS;
        case ENOSPC: return 112;
        case EISDIR: return ERR_ACCESS_DENIED;
        case ENOTEMPTY: return 145;
        default: return 31;   // ERROR_GEN_FAILURE
    }
}

std::string path_a(uint32_t p) { return vfs_canon(read_astr_utf8(p)); }
std::string path_w(uint32_t p) { return vfs_canon(read_wstr_utf8(p)); }

uint32_t attrs_of(const struct stat &st) { return S_ISDIR(st.st_mode) ? ATTR_DIR : ATTR_ARCHIVE; }

// ---------------------------------------------------------------------------------------------------------------
uint32_t create_file(const std::string &canon, uint32_t access, uint32_t disp, uint32_t flags) {
    if (canon.size() >= 4) {
        std::string tail = to_lower_ascii(canon.substr(canon.find_last_of('\\') + 1));
        if (tail == "nul") {
            auto *f = new KFile();
            f->fd = open("/dev/null", O_RDWR);
            f->can_write = true;
            return handle_alloc(f);
        }
        if (tail == "conout$" || tail == "conin$") {
            auto *f = new KFile();
            f->fd = tail == "conin$" ? 0 : 1;
            f->is_std = f->is_char = true;
            f->can_write = true;
            return handle_alloc(f);
        }
    }
    bool want_write = access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE_ACC);
    bool creating = disp == CREATE_NEW || disp == CREATE_ALWAYS || disp == OPEN_ALWAYS;
    bool truncating = disp == CREATE_ALWAYS || disp == TRUNCATE_EXISTING;
    std::string host;
    bool isdir = false;
    bool exists = vfs_resolve(canon, &host, &isdir);
    uint32_t err = 0;
    uint32_t last_ok_err = 0;
    if (exists) {
        if (disp == CREATE_NEW) { set_last_error(ERR_FILE_EXISTS); return INVALID_HANDLE; }
        if (disp == CREATE_ALWAYS || disp == OPEN_ALWAYS) last_ok_err = ERR_ALREADY_EXISTS;
        if (isdir) {
            if (!(flags & FLAG_BACKUP) && want_write) { set_last_error(ERR_ACCESS_DENIED); return INVALID_HANDLE; }
            auto *f = new KFile();
            f->is_dir = true;
            f->host = host;
            f->guest = canon;
            f->can_read = false;
            if (!host.empty()) f->fd = open(host.c_str(), O_RDONLY);
            LOGD(LC_FILE, "open dir %s", canon.c_str());
            set_last_error(0);
            return handle_alloc(f);
        }
        if (want_write || truncating) {
            std::string w;
            if (!vfs_writable_for_existing(canon, &w, &err)) {
                LOGD(LC_FILE, "open %s for write denied (err %u)", canon.c_str(), err);
                set_last_error(err);
                return INVALID_HANDLE;
            }
            host = w;
        }
    } else {
        if (!creating) {
            // distinguish missing parent
            std::string tmp;
            uint32_t e2 = 0;
            std::string parent = canon.substr(0, canon.find_last_of('\\'));
            bool pd = false;
            std::string ph;
            bool parent_ok = parent.size() <= 2 || vfs_resolve(parent.size() == 2 ? parent + "\\" : parent, &ph, &pd);
            (void)tmp; (void)e2;
            set_last_error(parent_ok ? ERR_FILE_NOT_FOUND : ERR_PATH_NOT_FOUND);
            LOGT(LC_FILE, "open %s: not found", canon.c_str());
            return INVALID_HANDLE;
        }
        if (!vfs_resolve_create(canon, &host, &err)) {
            LOGD(LC_FILE, "create %s denied (err %u)", canon.c_str(), err);
            set_last_error(err);
            return INVALID_HANDLE;
        }
    }
    int oflags = want_write ? O_RDWR : O_RDONLY;
    if (!exists) oflags |= O_CREAT | O_EXCL;
    if (exists && truncating) oflags |= O_TRUNC;
    int fd = open(host.c_str(), oflags, 0644);
    if (fd < 0 && want_write && errno == EACCES) fd = open(host.c_str(), O_RDONLY);
    if (fd < 0) {
        set_last_error(errno_to_win(errno));
        LOGD(LC_FILE, "open %s -> %s failed: %s", canon.c_str(), host.c_str(), strerror(errno));
        return INVALID_HANDLE;
    }
    if (!exists) vfs_note_change(host);
    LOGD(LC_FILE, "open %s%s", canon.c_str(), want_write ? " (write)" : "");
    watchdog_note_progress();
    auto *f = new KFile();
    f->fd = fd;
    f->host = host;
    f->guest = canon;
    f->overlapped = flags & FLAG_OVERLAPPED;
    f->can_write = want_write;
    f->delete_on_close = flags & FLAG_DELETE_ON_CLOSE;
    f->append = (access & FILE_APPEND_DATA) && !(access & FILE_WRITE_DATA) && !(access & GENERIC_WRITE);
    LOGT(LC_FILE, "open %s -> %s fd=%d%s", canon.c_str(), host.c_str(), fd, want_write ? " (rw)" : "");
    uint32_t h = handle_alloc(f);
    set_last_error(last_ok_err);
    return h;
}

uint32_t k32_CreateFileW(uint32_t name, uint32_t access, uint32_t, uint32_t, uint32_t disp, uint32_t flags, uint32_t) {
    if (!name) { set_last_error(ERR_PATH_NOT_FOUND); return INVALID_HANDLE; }
    return create_file(path_w(name), access, disp, flags);
}
uint32_t k32_CreateFileA(uint32_t name, uint32_t access, uint32_t, uint32_t, uint32_t disp, uint32_t flags, uint32_t) {
    if (!name) { set_last_error(ERR_PATH_NOT_FOUND); return INVALID_HANDLE; }
    return create_file(path_a(name), access, disp, flags);
}

KRef<KFile> file_of(uint32_t h) {
    KObject *o = kobj_resolve(h);
    if (o && o->type != KType::File) { o->release(); o = nullptr; }
    return KRef<KFile>(static_cast<KFile *>(o));
}

void complete_overlapped(uint32_t ov, uint32_t n, uint32_t status) {
    wr32(ov + 0, status);
    wr32(ov + 4, n);
    uint32_t ev = rd32(ov + 16);
    if (ev) {
        if (auto e = handle_ref<KEvent>(ev & ~1u, KType::Event)) event_set(e.get());
    }
}

uint32_t k32_ReadFile(uint32_t h, uint32_t buf, uint32_t n, uint32_t pread_, uint32_t ov) {
    auto f = file_of(h);
    if (!f || f->is_dir) { set_last_error(ERR_INVALID_HANDLE); return 0; }
    if (pread_) wr32(pread_, 0);
    ssize_t got;
    if (f->is_std) got = read(f->fd, guest_base + buf, n);
    else {
        uint64_t off = ov && f->overlapped ? (uint64_t)rd32(ov + 8) | ((uint64_t)rd32(ov + 12) << 32) : f->pos.load();
        got = pread(f->fd, guest_base + buf, n, (off_t)off);
        if (got > 0 && !(ov && f->overlapped)) f->pos += (uint64_t)got;
        else if (got > 0 && ov) f->pos = off + (uint64_t)got;
    }
    if (got < 0) { set_last_error(errno_to_win(errno)); return 0; }
    if (ov && f->overlapped) {
        if (got == 0 && n) { complete_overlapped(ov, 0, 0xC0000011u); set_last_error(ERR_HANDLE_EOF); return 0; }
        complete_overlapped(ov, (uint32_t)got, 0);
    }
    if (pread_) wr32(pread_, (uint32_t)got);
    set_last_error(0);
    return 1;
}
uint32_t k32_WriteFile(uint32_t h, uint32_t buf, uint32_t n, uint32_t pwritten, uint32_t ov) {
    auto f = file_of(h);
    if (!f || f->is_dir) { set_last_error(ERR_INVALID_HANDLE); return 0; }
    if (!f->can_write && !f->is_std) { set_last_error(ERR_ACCESS_DENIED); return 0; }
    if (pwritten) wr32(pwritten, 0);
    ssize_t put;
    if (f->is_std) put = write(f->fd, guest_base + buf, n);
    else {
        bool ovl = ov && f->overlapped;
        uint64_t off;
        if (f->append) { struct stat st; fstat(f->fd, &st); off = (uint64_t)st.st_size; }
        else off = ovl ? (uint64_t)rd32(ov + 8) | ((uint64_t)rd32(ov + 12) << 32) : f->pos.load();
        put = pwrite(f->fd, guest_base + buf, n, (off_t)off);
        if (put > 0) f->pos = off + (uint64_t)put;
    }
    if (put < 0) { set_last_error(errno_to_win(errno)); return 0; }
    if (ov && f->overlapped) complete_overlapped(ov, (uint32_t)put, 0);
    if (pwritten) wr32(pwritten, (uint32_t)put);
    set_last_error(0);
    return 1;
}
uint32_t k32_GetOverlappedResult(uint32_t, uint32_t ov, uint32_t pbytes, uint32_t) {
    if (!ov) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    if (pbytes) wr32(pbytes, rd32(ov + 4));
    return 1;
}
uint32_t k32_CancelIo(uint32_t) { return 1; }

uint32_t k32_SetFilePointer(uint32_t h, int32_t lo, uint32_t phigh, uint32_t method) {
    auto f = file_of(h);
    if (!f || f->is_std) { set_last_error(ERR_INVALID_HANDLE); return 0xFFFFFFFFu; }
    int64_t dist = phigh ? ((int64_t)(int32_t)rd32(phigh) << 32) | (uint32_t)lo : (int64_t)lo;
    int64_t base = 0;
    if (method == 1) base = (int64_t)f->pos.load();
    else if (method == 2) { struct stat st; fstat(f->fd, &st); base = st.st_size; }
    int64_t np = base + dist;
    if (np < 0) { set_last_error(131); return 0xFFFFFFFFu; }
    f->pos = (uint64_t)np;
    if (phigh) wr32(phigh, (uint32_t)((uint64_t)np >> 32));
    set_last_error(0);
    return (uint32_t)np;
}
uint32_t k32_SetFilePointerEx(uint32_t h, int64_t dist, uint32_t pnew, uint32_t method) {
    auto f = file_of(h);
    if (!f || f->is_std) { set_last_error(ERR_INVALID_HANDLE); return 0; }
    int64_t base = 0;
    if (method == 1) base = (int64_t)f->pos.load();
    else if (method == 2) { struct stat st; fstat(f->fd, &st); base = st.st_size; }
    int64_t np = base + dist;
    if (np < 0) { set_last_error(131); return 0; }
    f->pos = (uint64_t)np;
    if (pnew) wr64(pnew, (uint64_t)np);
    return 1;
}
uint32_t k32_GetFileSize(uint32_t h, uint32_t phigh) {
    auto f = file_of(h);
    struct stat st;
    if (!f || f->fd < 0 || fstat(f->fd, &st) != 0) { set_last_error(ERR_INVALID_HANDLE); return 0xFFFFFFFFu; }
    if (phigh) wr32(phigh, (uint32_t)((uint64_t)st.st_size >> 32));
    set_last_error(0);
    return (uint32_t)st.st_size;
}
uint32_t k32_GetFileSizeEx(uint32_t h, uint32_t out) {
    auto f = file_of(h);
    struct stat st;
    if (!f || f->fd < 0 || fstat(f->fd, &st) != 0) { set_last_error(ERR_INVALID_HANDLE); return 0; }
    wr64(out, (uint64_t)st.st_size);
    return 1;
}
uint32_t k32_SetEndOfFile(uint32_t h) {
    auto f = file_of(h);
    if (!f || !f->can_write) { set_last_error(ERR_ACCESS_DENIED); return 0; }
    if (ftruncate(f->fd, (off_t)f->pos.load()) != 0) { set_last_error(errno_to_win(errno)); return 0; }
    return 1;
}
uint32_t k32_FlushFileBuffers(uint32_t h) {
    auto f = file_of(h);
    if (!f) { set_last_error(ERR_INVALID_HANDLE); return 0; }
    return 1;
}
uint32_t k32_GetFileType(uint32_t h) {
    auto f = file_of(h);
    if (!f) { set_last_error(ERR_INVALID_HANDLE); return 0; }
    return f->is_char ? 2 : 1;
}
void fill_times(uint32_t base, const struct stat &st) {
    wr64(base + 0, ns_to_filetime(ST_CTIME_NS(st)));
    wr64(base + 8, ns_to_filetime(ST_ATIME_NS(st)));
    wr64(base + 16, ns_to_filetime(ST_MTIME_NS(st)));
}
uint32_t k32_GetFileInformationByHandle(uint32_t h, uint32_t out) {
    auto f = file_of(h);
    struct stat st;
    if (!f || f->fd < 0 || fstat(f->fd, &st) != 0) { set_last_error(ERR_INVALID_HANDLE); return 0; }
    memset(guest_base + out, 0, 52);
    wr32(out, attrs_of(st));
    fill_times(out + 4, st);
    wr32(out + 28, 0x12345678);
    wr32(out + 32, (uint32_t)((uint64_t)st.st_size >> 32));
    wr32(out + 36, (uint32_t)st.st_size);
    wr32(out + 40, 1);
    wr32(out + 44, (uint32_t)((uint64_t)st.st_ino >> 32));
    wr32(out + 48, (uint32_t)st.st_ino);
    return 1;
}
uint32_t k32_GetFileTime(uint32_t h, uint32_t c, uint32_t a, uint32_t w) {
    auto f = file_of(h);
    struct stat st;
    if (!f || f->fd < 0 || fstat(f->fd, &st) != 0) { set_last_error(ERR_INVALID_HANDLE); return 0; }
    if (c) wr64(c, ns_to_filetime(ST_CTIME_NS(st)));
    if (a) wr64(a, ns_to_filetime(ST_ATIME_NS(st)));
    if (w) wr64(w, ns_to_filetime(ST_MTIME_NS(st)));
    return 1;
}
uint32_t k32_SetFileTime(uint32_t h, uint32_t, uint32_t a, uint32_t w) {
    auto f = file_of(h);
    if (!f || f->fd < 0) { set_last_error(ERR_INVALID_HANDLE); return 0; }
    timespec ts[2];
    ts[0].tv_sec = ts[0].tv_nsec = UTIME_OMIT;
    ts[1] = ts[0];
    auto conv = [](uint64_t ft, timespec *t) { if (ft > kEpochDiff100ns) { uint64_t u = ft - kEpochDiff100ns; t->tv_sec = (time_t)(u / 10000000ull); t->tv_nsec = (long)(u % 10000000ull) * 100; } };
    if (a) conv(rd64(a), &ts[0]);
    if (w) conv(rd64(w), &ts[1]);
    futimens(f->fd, ts);
    return 1;
}
uint32_t k32_LockFile(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 1; }
uint32_t k32_UnlockFile(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 1; }
uint32_t k32_CreatePipe(uint32_t, uint32_t, uint32_t, uint32_t) { set_last_error(ERR_ACCESS_DENIED); return 0; }
uint32_t k32_PeekNamedPipe(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { set_last_error(109); return 0; }

// ---- standard handles
uint32_t g_std[3] = {0, 0, 0};
std::mutex g_std_mu;
uint32_t k32_GetStdHandle(int32_t which) {
    int idx = which == -10 ? 0 : which == -11 ? 1 : which == -12 ? 2 : -1;
    if (idx < 0) { set_last_error(ERR_INVALID_PARAMETER); return INVALID_HANDLE; }
    std::lock_guard<std::mutex> lk(g_std_mu);
    if (!g_std[idx]) {
        auto *f = new KFile();
        f->fd = idx;
        f->is_std = true;
        f->is_char = isatty(idx);
        f->can_write = idx != 0;
        g_std[idx] = handle_alloc(f);
    }
    return g_std[idx];
}
uint32_t k32_SetStdHandle(int32_t which, uint32_t h) {
    int idx = which == -10 ? 0 : which == -11 ? 1 : which == -12 ? 2 : -1;
    if (idx < 0) return 0;
    std::lock_guard<std::mutex> lk(g_std_mu);
    g_std[idx] = h;
    return 1;
}

// ---- attributes and namespace operations
bool stat_canon(const std::string &canon, struct stat *st, std::string *host = nullptr) {
    std::string h;
    bool isd;
    if (!vfs_resolve(canon, &h, &isd)) return false;
    if (host) *host = h;
    if (h.empty()) { memset(st, 0, sizeof *st); st->st_mode = S_IFDIR | 0755; return true; }
    return stat(h.c_str(), st) == 0;
}
uint32_t get_attrs(const std::string &canon) {
    struct stat st;
    if (!stat_canon(canon, &st)) {
        std::string parent = canon.substr(0, canon.find_last_of('\\'));
        struct stat ps;
        bool pok = parent.size() <= 2 ? true : stat_canon(parent, &ps);
        set_last_error(pok ? ERR_FILE_NOT_FOUND : ERR_PATH_NOT_FOUND);
        return INVALID_ATTR;
    }
    return attrs_of(st);
}
uint32_t k32_GetFileAttributesW(uint32_t p) { return get_attrs(path_w(p)); }
uint32_t k32_GetFileAttributesA(uint32_t p) { return get_attrs(path_a(p)); }
uint32_t get_attrs_ex(const std::string &canon, uint32_t level, uint32_t out) {
    if (level != 0) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    struct stat st;
    if (!stat_canon(canon, &st)) { set_last_error(ERR_FILE_NOT_FOUND); return 0; }
    wr32(out, attrs_of(st));
    fill_times(out + 4, st);
    wr32(out + 28, (uint32_t)((uint64_t)st.st_size >> 32));
    wr32(out + 32, (uint32_t)st.st_size);
    return 1;
}
uint32_t k32_GetFileAttributesExW(uint32_t p, uint32_t lv, uint32_t out) { return get_attrs_ex(path_w(p), lv, out); }
uint32_t k32_GetFileAttributesExA(uint32_t p, uint32_t lv, uint32_t out) { return get_attrs_ex(path_a(p), lv, out); }
uint32_t k32_SetFileAttributes(uint32_t, uint32_t) { return 1; }

uint32_t delete_file(const std::string &canon) {
    std::string host;
    bool isd;
    if (!vfs_resolve(canon, &host, &isd)) { set_last_error(ERR_FILE_NOT_FOUND); return 0; }
    if (isd || host.empty()) { set_last_error(ERR_ACCESS_DENIED); return 0; }
    std::string w;
    uint32_t err = 0;
    if (is_ro_host(host)) { LOGD(LC_FILE, "DeleteFile %s denied (read-only game root)", canon.c_str()); set_last_error(ERR_ACCESS_DENIED); return 0; }
    (void)w; (void)err;
    if (unlink(host.c_str()) != 0) { set_last_error(errno_to_win(errno)); return 0; }
    vfs_note_change(host);
    return 1;
}
uint32_t k32_DeleteFileW(uint32_t p) { return delete_file(path_w(p)); }
uint32_t k32_DeleteFileA(uint32_t p) { return delete_file(path_a(p)); }

uint32_t move_file(const std::string &src, const std::string &dst, uint32_t flags) {
    std::string hs, hd;
    bool isd;
    if (!vfs_resolve(src, &hs, &isd)) { set_last_error(ERR_FILE_NOT_FOUND); return 0; }
    if (is_ro_host(hs)) { set_last_error(ERR_ACCESS_DENIED); return 0; }
    bool dexists = vfs_resolve(dst, &hd, &isd);
    if (dexists && !(flags & 1)) { set_last_error(ERR_ALREADY_EXISTS); return 0; }
    if (!dexists) {
        uint32_t err;
        if (!vfs_resolve_create(dst, &hd, &err)) { set_last_error(err); return 0; }
    } else if (is_ro_host(hd)) { set_last_error(ERR_ACCESS_DENIED); return 0; }
    if (rename(hs.c_str(), hd.c_str()) != 0) { set_last_error(errno_to_win(errno)); return 0; }
    vfs_note_change(hs);
    vfs_note_change(hd);
    return 1;
}
uint32_t k32_MoveFileW(uint32_t a, uint32_t b) { return move_file(path_w(a), path_w(b), 0); }
uint32_t k32_MoveFileA(uint32_t a, uint32_t b) { return move_file(path_a(a), path_a(b), 0); }
uint32_t k32_MoveFileExW(uint32_t a, uint32_t b, uint32_t fl) { return move_file(path_w(a), b ? path_w(b) : "", fl); }
uint32_t k32_MoveFileExA(uint32_t a, uint32_t b, uint32_t fl) { return move_file(path_a(a), b ? path_a(b) : "", fl); }
uint32_t copy_file(const std::string &src, const std::string &dst, bool fail_if_exists) {
    std::string hs, hd;
    bool isd;
    if (!vfs_resolve(src, &hs, &isd) || isd) { set_last_error(ERR_FILE_NOT_FOUND); return 0; }
    bool dexists = vfs_resolve(dst, &hd, &isd);
    if (dexists && fail_if_exists) { set_last_error(ERR_FILE_EXISTS); return 0; }
    if (dexists) {
        uint32_t err;
        std::string w;
        if (!vfs_writable_for_existing(dst, &w, &err)) { set_last_error(err); return 0; }
        hd = w;
    } else {
        uint32_t err;
        if (!vfs_resolve_create(dst, &hd, &err)) { set_last_error(err); return 0; }
    }
    int in = open(hs.c_str(), O_RDONLY);
    int out = open(hd.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (in < 0 || out < 0) { if (in >= 0) close(in); if (out >= 0) close(out); set_last_error(errno_to_win(errno)); return 0; }
    char buf[65536];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0) if (write(out, buf, n) != n) break;
    close(in);
    close(out);
    vfs_note_change(hd);
    return 1;
}
uint32_t k32_CopyFileW(uint32_t a, uint32_t b, uint32_t fail) { return copy_file(path_w(a), path_w(b), fail); }
uint32_t k32_CopyFileA(uint32_t a, uint32_t b, uint32_t fail) { return copy_file(path_a(a), path_a(b), fail); }

uint32_t make_dir(const std::string &canon) {
    uint32_t err = 0;
    if (!vfs_mkdir(canon, &err)) { set_last_error(err); return 0; }
    return 1;
}
uint32_t k32_CreateDirectoryW(uint32_t p, uint32_t) { return make_dir(path_w(p)); }
uint32_t k32_CreateDirectoryA(uint32_t p, uint32_t) { return make_dir(path_a(p)); }
uint32_t remove_dir(const std::string &canon) {
    std::string host;
    bool isd;
    if (!vfs_resolve(canon, &host, &isd) || !isd) { set_last_error(ERR_PATH_NOT_FOUND); return 0; }
    if (host.empty() || is_ro_host(host)) { set_last_error(ERR_ACCESS_DENIED); return 0; }
    if (rmdir(host.c_str()) != 0) { set_last_error(errno_to_win(errno)); return 0; }
    vfs_note_change(host);
    return 1;
}
uint32_t k32_RemoveDirectoryW(uint32_t p) { return remove_dir(path_w(p)); }
uint32_t k32_RemoveDirectoryA(uint32_t p) { return remove_dir(path_a(p)); }

// ---- FindFirstFile
bool wild_match(const std::u16string &pat, size_t pi, const std::u16string &s, size_t si) {
    while (pi < pat.size()) {
        char16_t c = pat[pi];
        if (c == u'*') {
            while (pi < pat.size() && pat[pi] == u'*') pi++;
            if (pi == pat.size()) return true;
            for (size_t k = si; k <= s.size(); k++) if (wild_match(pat, pi, s, k)) return true;
            return false;
        }
        if (c == u'?') {
            if (si < s.size()) si++;
            pi++;
            continue;
        }
        if (si >= s.size() || u16_lower(c) != u16_lower(s[si])) return false;
        pi++; si++;
    }
    return si == s.size();
}
bool has_wild(const std::string &s) { return s.find_first_of("*?") != std::string::npos; }

KFind *do_find(const std::string &canon_with_pattern, uint32_t *err) {
    size_t k = canon_with_pattern.find_last_of('\\');
    std::string dir = canon_with_pattern.substr(0, k), pat = canon_with_pattern.substr(k + 1);
    if (dir.size() == 2) dir += "\\";
    if (pat.empty() || pat == "*.*") pat = "*";
    auto *kf = new KFind();
    std::vector<DirEntry> all;
    std::string dh;
    bool isd = false;
    if (!has_wild(pat)) {
        std::string target = canon_with_pattern;
        std::string h;
        bool d2;
        if (!vfs_resolve(target, &h, &d2)) {
            bool pexists = vfs_resolve(dir, &dh, &isd) && isd;
            *err = pexists ? ERR_FILE_NOT_FOUND : ERR_PATH_NOT_FOUND;
            delete kf;
            return nullptr;
        }
        DirEntry de;
        de.name = pat;
        struct stat st;
        if (!h.empty() && stat(h.c_str(), &st) == 0) {
            de.is_dir = S_ISDIR(st.st_mode);
            de.size = de.is_dir ? 0 : (uint64_t)st.st_size;
            de.mtime_ns = ST_MTIME_NS(st);
            // real-case name
            size_t sl = h.find_last_of('/');
            de.name = h.substr(sl + 1);
        } else { de.is_dir = true; de.size = 0; de.mtime_ns = 0; }
        kf->ents.push_back(de);
        return kf;
    }
    if (!vfs_list(dir, &all)) {
        delete kf;
        *err = vfs_resolve(dir, &dh, &isd) ? ERR_PATH_NOT_FOUND : ERR_PATH_NOT_FOUND;
        return nullptr;
    }
    std::u16string wpat = utf8_to_utf16(pat);
    if (dir.size() > 3) {   // "." and ".." exist in every directory except the root
        for (const char *dots : {".", ".."}) {
            if (wild_match(wpat, 0, utf8_to_utf16(dots), 0)) {
                DirEntry de;
                de.name = dots; de.is_dir = true; de.size = 0; de.mtime_ns = 0;
                kf->ents.push_back(de);
            }
        }
    }
    std::sort(all.begin(), all.end(), [](const DirEntry &a, const DirEntry &b) { return lower_utf8(a.name) < lower_utf8(b.name); });
    for (auto &e : all)
        if (wild_match(wpat, 0, utf8_to_utf16(e.name), 0)) kf->ents.push_back(e);
    if (kf->ents.empty()) { delete kf; *err = ERR_FILE_NOT_FOUND; return nullptr; }
    return kf;
}

void fill_find(uint32_t out, const DirEntry &e, bool wide) {
    uint64_t t = e.mtime_ns ? ns_to_filetime(e.mtime_ns) : kEpochDiff100ns;
    memset(guest_base + out, 0, wide ? 592 : 318);
    wr32(out, e.is_dir ? ATTR_DIR : ATTR_ARCHIVE);
    wr64(out + 4, t);
    wr64(out + 12, t);
    wr64(out + 20, t);
    wr32(out + 28, (uint32_t)(e.size >> 32));
    wr32(out + 32, (uint32_t)e.size);
    if (wide) {
        std::u16string n = utf8_to_utf16(e.name);
        if (n.size() > 259) n.resize(259);
        memcpy(guest_base + out + 44, n.data(), n.size() * 2);
    } else {
        std::u16string n = utf8_to_utf16(e.name);
        std::string a(n.size() * 3 + 1, 0);
        int64_t k = wide_to_mb(1252, (const uint16_t *)n.data(), n.size(), (uint8_t *)a.data(), a.size(), nullptr, '?');
        a.resize(k < 0 ? 0 : (size_t)k);
        if (a.size() > 259) a.resize(259);
        memcpy(guest_base + out + 44, a.data(), a.size());
    }
}
uint32_t find_first(const std::string &canon, uint32_t out, bool wide) {
    uint32_t err = 0;
    KFind *kf = do_find(canon, &err);
    if (!kf) { set_last_error(err); return INVALID_HANDLE; }
    fill_find(out, kf->ents[0], wide);
    kf->idx = 1;
    LOGT(LC_FILE, "FindFirstFile %s: %zu entries", canon.c_str(), kf->ents.size());
    return handle_alloc(kf);
}
uint32_t find_next(uint32_t h, uint32_t out, bool wide) {
    auto kf = handle_ref<KFind>(h, KType::Find);
    if (!kf) { set_last_error(ERR_INVALID_HANDLE); return 0; }
    if (kf->idx >= kf->ents.size()) { set_last_error(ERR_NO_MORE_FILES); return 0; }
    fill_find(out, kf->ents[kf->idx++], wide);
    return 1;
}
uint32_t k32_FindFirstFileW(uint32_t p, uint32_t out) { return find_first(path_w(p), out, true); }
uint32_t k32_FindFirstFileA(uint32_t p, uint32_t out) { return find_first(path_a(p), out, false); }
uint32_t k32_FindFirstFileExW(uint32_t p, uint32_t, uint32_t out, uint32_t, uint32_t, uint32_t) { return find_first(path_w(p), out, true); }
uint32_t k32_FindNextFileW(uint32_t h, uint32_t out) { return find_next(h, out, true); }
uint32_t k32_FindNextFileA(uint32_t h, uint32_t out) { return find_next(h, out, false); }
uint32_t k32_FindClose(uint32_t h) { return handle_ref<KFind>(h, KType::Find) ? handle_close(h) : (set_last_error(ERR_INVALID_HANDLE), 0); }

// ---- paths
uint32_t ret_path_w(const std::string &canon, uint32_t len, uint32_t buf, uint32_t filepart) {
    std::u16string w = utf8_to_utf16(canon);
    if (len <= w.size()) return (uint32_t)w.size() + 1;
    write_wstr(buf, len, w);
    if (filepart) {
        size_t k = w.find_last_of(u'\\');
        wr32(filepart, buf + (uint32_t)(k + 1) * 2);
    }
    return (uint32_t)w.size();
}
uint32_t ret_path_a(const std::string &canon, uint32_t len, uint32_t buf, uint32_t filepart) {
    std::u16string w = utf8_to_utf16(canon);
    std::string a(w.size() + 1, 0);
    int64_t n = wide_to_mb(1252, (const uint16_t *)w.data(), w.size(), (uint8_t *)a.data(), a.size(), nullptr, '?');
    a.resize(n < 0 ? 0 : (size_t)n);
    if (len <= a.size()) return (uint32_t)a.size() + 1;
    write_astr(buf, len, a);
    if (filepart) {
        size_t k = a.find_last_of('\\');
        wr32(filepart, buf + (uint32_t)(k + 1));
    }
    return (uint32_t)a.size();
}
uint32_t k32_GetFullPathNameW(uint32_t p, uint32_t len, uint32_t buf, uint32_t fp) { return ret_path_w(path_w(p), len, buf, fp); }
uint32_t k32_GetFullPathNameA(uint32_t p, uint32_t len, uint32_t buf, uint32_t fp) { return ret_path_a(path_a(p), len, buf, fp); }
std::string real_case(const std::string &canon) {
    // rebuild the path with on-disk case where it resolves
    std::string host;
    bool isd;
    if (!vfs_resolve(canon, &host, &isd) || host.empty()) return canon;
    std::string g = vfs_host_to_guest(host);
    return g.size() == canon.size() ? g : canon;
}
uint32_t k32_GetLongPathNameW(uint32_t p, uint32_t buf, uint32_t len) {
    std::string c = real_case(path_w(p));
    return ret_path_w(c, len, buf, 0);
}
uint32_t k32_GetShortPathNameW(uint32_t p, uint32_t buf, uint32_t len) { return k32_GetLongPathNameW(p, buf, len); }
uint32_t k32_GetCurrentDirectoryW(uint32_t len, uint32_t buf) { return ret_path_w(vfs_getcwd(), len, buf, 0); }
uint32_t k32_GetCurrentDirectoryA(uint32_t len, uint32_t buf) { return ret_path_a(vfs_getcwd(), len, buf, 0); }
uint32_t k32_SetCurrentDirectoryW(uint32_t p) { return vfs_setcwd(path_w(p)) ? 1 : (set_last_error(ERR_PATH_NOT_FOUND), 0); }
uint32_t k32_SetCurrentDirectoryA(uint32_t p) { return vfs_setcwd(path_a(p)) ? 1 : (set_last_error(ERR_PATH_NOT_FOUND), 0); }
std::string temp_dir() { return config().guest_user_dir + "\\AppData\\Local\\Temp\\"; }
uint32_t k32_GetTempPathW(uint32_t len, uint32_t buf) {
    std::u16string w = utf8_to_utf16(temp_dir());
    if (len <= w.size()) return (uint32_t)w.size() + 1;
    write_wstr(buf, len, w);
    return (uint32_t)w.size();
}
uint32_t k32_GetTempPathA(uint32_t len, uint32_t buf) {
    std::string a = temp_dir();
    if (len <= a.size()) return (uint32_t)a.size() + 1;
    write_astr(buf, len, a);
    return (uint32_t)a.size();
}
uint32_t temp_file_name(const std::string &dir, const std::string &prefix, uint32_t unique, std::string *out) {
    static std::atomic<uint32_t> ctr{1};
    uint32_t u = unique ? unique : (ctr++ + (uint32_t)getpid());
    char hexs[16];
    snprintf(hexs, sizeof hexs, "%X", u & 0xFFFF);
    std::string d = dir;
    if (!d.empty() && d.back() == '\\') d.pop_back();
    *out = d + "\\" + prefix.substr(0, 3) + hexs + ".tmp";
    if (!unique) {   // create the file
        std::string h;
        uint32_t err;
        std::string canon = vfs_canon(*out);
        if (vfs_resolve_create(canon, &h, &err)) { int fd = open(h.c_str(), O_CREAT | O_WRONLY, 0644); if (fd >= 0) close(fd); vfs_note_change(h); }
    }
    return u & 0xFFFF;
}
uint32_t k32_GetTempFileNameW(uint32_t dir, uint32_t prefix, uint32_t unique, uint32_t buf) {
    std::string out;
    uint32_t u = temp_file_name(read_wstr_utf8(dir), read_wstr_utf8(prefix), unique, &out);
    write_wstr(buf, 261, utf8_to_utf16(out));
    return u;
}
uint32_t k32_GetTempFileNameA(uint32_t dir, uint32_t prefix, uint32_t unique, uint32_t buf) {
    std::string out;
    uint32_t u = temp_file_name(read_astr_utf8(dir), read_astr_utf8(prefix), unique, &out);
    write_astr(buf, 261, out);
    return u;
}
uint32_t k32_GetDriveTypeW(uint32_t) { return 3; }
uint32_t k32_GetDriveTypeA(uint32_t) { return 3; }
uint32_t k32_GetLogicalDrives() { return 4; }
uint32_t k32_GetDiskFreeSpaceExW(uint32_t, uint32_t avail, uint32_t total, uint32_t freeb) {
    if (avail) wr64(avail, 200ull << 30);
    if (total) wr64(total, 500ull << 30);
    if (freeb) wr64(freeb, 200ull << 30);
    return 1;
}
uint32_t k32_GetDiskFreeSpaceA(uint32_t, uint32_t spc, uint32_t bps, uint32_t nfree, uint32_t total) {
    wr32(spc, 8); wr32(bps, 512); wr32(nfree, 0x3FFFFFF); wr32(total, 0x7FFFFFF);
    return 1;
}
uint32_t k32_GetVolumeInformationW(uint32_t, uint32_t name, uint32_t nlen, uint32_t serial, uint32_t maxlen, uint32_t flags, uint32_t fsname, uint32_t fslen) {
    if (name && nlen) write_wstr(name, nlen, u"System");
    if (serial) wr32(serial, 0x1A2B3C4D);
    if (maxlen) wr32(maxlen, 255);
    if (flags) wr32(flags, 0x0003 | 0x40000);   // case preserved + unicode on disk
    if (fsname && fslen) write_wstr(fsname, fslen, u"NTFS");
    return 1;
}
uint32_t get_dir(const std::string &s, uint32_t buf, uint32_t len, bool wide) {
    return wide ? ret_path_w(s, len, buf, 0) : ret_path_a(s, len, buf, 0);
}
uint32_t k32_GetSystemDirectoryW(uint32_t buf, uint32_t len) { return get_dir("C:\\Windows\\System32", buf, len, true); }
uint32_t k32_GetSystemDirectoryA(uint32_t buf, uint32_t len) { return get_dir("C:\\Windows\\System32", buf, len, false); }
uint32_t k32_GetWindowsDirectoryW(uint32_t buf, uint32_t len) { return get_dir("C:\\Windows", buf, len, true); }
uint32_t k32_GetWindowsDirectoryA(uint32_t buf, uint32_t len) { return get_dir("C:\\Windows", buf, len, false); }
uint32_t k32_GetSystemWindowsDirectoryW(uint32_t buf, uint32_t len) { return get_dir("C:\\Windows", buf, len, true); }
uint32_t k32_GetSystemWow64DirectoryW(uint32_t buf, uint32_t len) { return get_dir("C:\\Windows\\SysWOW64", buf, len, true); }
uint32_t k32_SearchPathW(uint32_t path, uint32_t file, uint32_t ext, uint32_t len, uint32_t buf, uint32_t fp) {
    std::string f = read_wstr_utf8(file);
    if (ext && f.find('.') == std::string::npos) f += read_wstr_utf8(ext);
    std::string dirs = path ? read_wstr_utf8(path) : (vfs_game_dir() + "\\Binaries\\Win32");
    size_t pos = 0;
    while (pos <= dirs.size()) {
        size_t e = dirs.find(';', pos);
        if (e == std::string::npos) e = dirs.size();
        std::string cand = vfs_canon(dirs.substr(pos, e - pos) + "\\" + f);
        std::string h;
        bool d;
        if (vfs_resolve(cand, &h, &d) && !d) return ret_path_w(cand, len, buf, fp);
        pos = e + 1;
    }
    set_last_error(ERR_FILE_NOT_FOUND);
    return 0;
}

// ---- file mapping
uint32_t create_mapping(uint32_t hfile, uint32_t protect, uint32_t size_hi, uint32_t size_lo, const std::string &name) {
    uint64_t size = ((uint64_t)size_hi << 32) | size_lo;
    KObject *existing = name.empty() ? nullptr : named_lookup(KType::Mapping, name);
    if (existing) { uint32_t h = handle_alloc(existing); set_last_error(ERR_ALREADY_EXISTS); return h; }
    auto *m = new KMapping();
    m->prot = protect;
    if (hfile == INVALID_HANDLE) {
        if (size == 0) { delete m; set_last_error(ERR_INVALID_PARAMETER); return 0; }
        char tmpl[] = "/tmp/dishmapXXXXXX";
        std::string base = config().save_dir.empty() ? std::string("/tmp") : config().save_dir + "/AppData/Local/Temp";
        std::string path = base + "/dishmapXXXXXX";
        std::vector<char> pb(path.begin(), path.end());
        pb.push_back(0);
        int fd = mkstemp(pb.data());
        if (fd < 0) { fd = mkstemp(tmpl); unlink(tmpl); } else unlink(pb.data());
        if (fd < 0) { delete m; set_last_error(ERR_NOT_ENOUGH_MEMORY); return 0; }
        if (ftruncate(fd, (off_t)size) != 0) { close(fd); delete m; set_last_error(ERR_NOT_ENOUGH_MEMORY); return 0; }
        m->fd = fd;
        m->size = size;
        m->anon = true;
    } else {
        auto f = file_of(hfile);
        if (!f || f->fd < 0) { delete m; set_last_error(ERR_INVALID_HANDLE); return 0; }
        struct stat st;
        fstat(f->fd, &st);
        if (size == 0) size = (uint64_t)st.st_size;
        if (size == 0) { delete m; set_last_error(ERR_INVALID_PARAMETER); return 0; }
        if (size > (uint64_t)st.st_size && f->can_write && (protect & 0xFF) != PAGE_READONLY) ftruncate(f->fd, (off_t)size);
        m->fd = dup(f->fd);
        m->size = size;
    }
    if (!name.empty()) { m->name = name; named_insert(m); }
    set_last_error(0);
    return handle_alloc(m);
}
uint32_t k32_CreateFileMappingA(uint32_t hf, uint32_t, uint32_t prot, uint32_t hi, uint32_t lo, uint32_t name) {
    return create_mapping(hf, prot, hi, lo, name ? lower_utf8(read_astr_utf8(name)) : "");
}
uint32_t k32_CreateFileMappingW(uint32_t hf, uint32_t, uint32_t prot, uint32_t hi, uint32_t lo, uint32_t name) {
    return create_mapping(hf, prot, hi, lo, name ? lower_utf8(read_wstr_utf8(name)) : "");
}
uint32_t map_view(uint32_t h, uint32_t access, uint32_t offhi, uint32_t offlo, uint32_t bytes, uint32_t hint) {
    auto m = handle_ref<KMapping>(h, KType::Mapping);
    if (!m) { set_last_error(ERR_INVALID_HANDLE); return 0; }
    uint64_t off = ((uint64_t)offhi << 32) | offlo;
    if (off & 0xFFFF) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    if (off >= m->size) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    uint64_t len = bytes ? bytes : m->size - off;
    if (off + len > m->size) { set_last_error(ERR_INVALID_PARAMETER); return 0; }
    uint32_t prot = (access & 2) ? PAGE_READWRITE : (access & 1) ? PAGE_WRITECOPY : PAGE_READONLY;
    if ((access & 0xFF) == 0) prot = PAGE_READONLY;
    if (prot == PAGE_READWRITE && (m->prot & 0xFF) == PAGE_READONLY) { set_last_error(ERR_ACCESS_DENIED); return 0; }
    uint32_t err = 0;
    uint32_t base = vm_map_view(m->fd, off, (uint32_t)len, prot, hint, &err);
    if (!base) set_last_error(err);
    return base;
}
uint32_t k32_MapViewOfFile(uint32_t h, uint32_t access, uint32_t hi, uint32_t lo, uint32_t bytes) { return map_view(h, access, hi, lo, bytes, 0); }
uint32_t k32_MapViewOfFileEx(uint32_t h, uint32_t access, uint32_t hi, uint32_t lo, uint32_t bytes, uint32_t base) { return map_view(h, access, hi, lo, bytes, base); }
uint32_t k32_UnmapViewOfFile(uint32_t base) {
    if (!vm_unmap_view(base)) { set_last_error(ERR_INVALID_ADDRESS); return 0; }
    return 1;
}
uint32_t k32_FlushViewOfFile(uint32_t, uint32_t) { return 1; }

}  // namespace

HLE("kernel32.dll", "CreateFileW", Stdcall, k32_CreateFileW);
HLE("kernel32.dll", "CreateFileA", Stdcall, k32_CreateFileA);
HLE("kernel32.dll", "ReadFile", Stdcall, k32_ReadFile);
HLE("kernel32.dll", "WriteFile", Stdcall, k32_WriteFile);
HLE("kernel32.dll", "GetOverlappedResult", Stdcall, k32_GetOverlappedResult);
HLE("kernel32.dll", "CancelIo", Stdcall, k32_CancelIo);
HLE("kernel32.dll", "SetFilePointer", Stdcall, k32_SetFilePointer);
HLE("kernel32.dll", "SetFilePointerEx", Stdcall, k32_SetFilePointerEx);
HLE("kernel32.dll", "GetFileSize", Stdcall, k32_GetFileSize);
HLE("kernel32.dll", "GetFileSizeEx", Stdcall, k32_GetFileSizeEx);
HLE("kernel32.dll", "SetEndOfFile", Stdcall, k32_SetEndOfFile);
HLE("kernel32.dll", "FlushFileBuffers", Stdcall, k32_FlushFileBuffers);
HLE("kernel32.dll", "GetFileType", Stdcall, k32_GetFileType);
HLE("kernel32.dll", "GetFileInformationByHandle", Stdcall, k32_GetFileInformationByHandle);
HLE("kernel32.dll", "GetFileTime", Stdcall, k32_GetFileTime);
HLE("kernel32.dll", "SetFileTime", Stdcall, k32_SetFileTime);
HLE("kernel32.dll", "LockFile", Stdcall, k32_LockFile);
HLE("kernel32.dll", "UnlockFile", Stdcall, k32_UnlockFile);
HLE("kernel32.dll", "CreatePipe", Stdcall, k32_CreatePipe);
HLE("kernel32.dll", "PeekNamedPipe", Stdcall, k32_PeekNamedPipe);
HLE("kernel32.dll", "GetStdHandle", Stdcall, k32_GetStdHandle);
HLE("kernel32.dll", "SetStdHandle", Stdcall, k32_SetStdHandle);
HLE("kernel32.dll", "GetFileAttributesW", Stdcall, k32_GetFileAttributesW);
HLE("kernel32.dll", "GetFileAttributesA", Stdcall, k32_GetFileAttributesA);
HLE("kernel32.dll", "GetFileAttributesExW", Stdcall, k32_GetFileAttributesExW);
HLE("kernel32.dll", "GetFileAttributesExA", Stdcall, k32_GetFileAttributesExA);
HLE("kernel32.dll", "SetFileAttributesW", Stdcall, k32_SetFileAttributes);
HLE("kernel32.dll", "SetFileAttributesA", Stdcall, k32_SetFileAttributes);
HLE("kernel32.dll", "DeleteFileW", Stdcall, k32_DeleteFileW);
HLE("kernel32.dll", "DeleteFileA", Stdcall, k32_DeleteFileA);
HLE("kernel32.dll", "MoveFileW", Stdcall, k32_MoveFileW);
HLE("kernel32.dll", "MoveFileA", Stdcall, k32_MoveFileA);
HLE("kernel32.dll", "MoveFileExW", Stdcall, k32_MoveFileExW);
HLE("kernel32.dll", "MoveFileExA", Stdcall, k32_MoveFileExA);
HLE("kernel32.dll", "CopyFileW", Stdcall, k32_CopyFileW);
HLE("kernel32.dll", "CopyFileA", Stdcall, k32_CopyFileA);
HLE("kernel32.dll", "CreateDirectoryW", Stdcall, k32_CreateDirectoryW);
HLE("kernel32.dll", "CreateDirectoryA", Stdcall, k32_CreateDirectoryA);
HLE("kernel32.dll", "RemoveDirectoryW", Stdcall, k32_RemoveDirectoryW);
HLE("kernel32.dll", "RemoveDirectoryA", Stdcall, k32_RemoveDirectoryA);
HLE("kernel32.dll", "FindFirstFileW", Stdcall, k32_FindFirstFileW);
HLE("kernel32.dll", "FindFirstFileA", Stdcall, k32_FindFirstFileA);
HLE("kernel32.dll", "FindFirstFileExW", Stdcall, k32_FindFirstFileExW);
HLE("kernel32.dll", "FindNextFileW", Stdcall, k32_FindNextFileW);
HLE("kernel32.dll", "FindNextFileA", Stdcall, k32_FindNextFileA);
HLE("kernel32.dll", "FindClose", Stdcall, k32_FindClose);
HLE("kernel32.dll", "GetFullPathNameW", Stdcall, k32_GetFullPathNameW);
HLE("kernel32.dll", "GetFullPathNameA", Stdcall, k32_GetFullPathNameA);
HLE("kernel32.dll", "GetLongPathNameW", Stdcall, k32_GetLongPathNameW);
HLE("kernel32.dll", "GetShortPathNameW", Stdcall, k32_GetShortPathNameW);
HLE("kernel32.dll", "GetCurrentDirectoryW", Stdcall, k32_GetCurrentDirectoryW);
HLE("kernel32.dll", "GetCurrentDirectoryA", Stdcall, k32_GetCurrentDirectoryA);
HLE("kernel32.dll", "SetCurrentDirectoryW", Stdcall, k32_SetCurrentDirectoryW);
HLE("kernel32.dll", "SetCurrentDirectoryA", Stdcall, k32_SetCurrentDirectoryA);
HLE("kernel32.dll", "GetTempPathW", Stdcall, k32_GetTempPathW);
HLE("kernel32.dll", "GetTempPathA", Stdcall, k32_GetTempPathA);
HLE("kernel32.dll", "GetTempFileNameW", Stdcall, k32_GetTempFileNameW);
HLE("kernel32.dll", "GetTempFileNameA", Stdcall, k32_GetTempFileNameA);
HLE("kernel32.dll", "GetDriveTypeW", Stdcall, k32_GetDriveTypeW);
HLE("kernel32.dll", "GetDriveTypeA", Stdcall, k32_GetDriveTypeA);
HLE("kernel32.dll", "GetLogicalDrives", Stdcall, k32_GetLogicalDrives);
HLE("kernel32.dll", "GetDiskFreeSpaceExW", Stdcall, k32_GetDiskFreeSpaceExW);
HLE("kernel32.dll", "GetDiskFreeSpaceA", Stdcall, k32_GetDiskFreeSpaceA);
HLE("kernel32.dll", "GetVolumeInformationW", Stdcall, k32_GetVolumeInformationW);
HLE("kernel32.dll", "GetSystemDirectoryW", Stdcall, k32_GetSystemDirectoryW);
HLE("kernel32.dll", "GetSystemDirectoryA", Stdcall, k32_GetSystemDirectoryA);
HLE("kernel32.dll", "GetWindowsDirectoryW", Stdcall, k32_GetWindowsDirectoryW);
HLE("kernel32.dll", "GetWindowsDirectoryA", Stdcall, k32_GetWindowsDirectoryA);
HLE("kernel32.dll", "GetSystemWindowsDirectoryW", Stdcall, k32_GetSystemWindowsDirectoryW);
HLE("kernel32.dll", "GetSystemWow64DirectoryW", Stdcall, k32_GetSystemWow64DirectoryW);
HLE("kernel32.dll", "SearchPathW", Stdcall, k32_SearchPathW);
HLE("kernel32.dll", "CreateFileMappingA", Stdcall, k32_CreateFileMappingA);
HLE("kernel32.dll", "CreateFileMappingW", Stdcall, k32_CreateFileMappingW);
HLE("kernel32.dll", "MapViewOfFile", Stdcall, k32_MapViewOfFile);
HLE("kernel32.dll", "MapViewOfFileEx", Stdcall, k32_MapViewOfFileEx);
HLE("kernel32.dll", "UnmapViewOfFile", Stdcall, k32_UnmapViewOfFile);
HLE("kernel32.dll", "FlushViewOfFile", Stdcall, k32_FlushViewOfFile);

}  // namespace dish
