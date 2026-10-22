#include "dish/loader.h"
#include "dish/profiler.h"
#include "dish/call.h"
#include "dish/config.h"
#include "dish/fault.h"
#include "dish/hle.h"
#include "dish/log.h"
#include "dish/sha256.h"
#include "dish/thread.h"
#include "dish/thunk.h"
#include "dish/util.h"
#include "dish/vfs.h"
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <unistd.h>
#include <sys/stat.h>

namespace dish {
namespace {

std::mutex g_mod_mu;
std::vector<std::unique_ptr<LoadedModule>> g_modules;

#pragma pack(push, 1)
struct DosHeader { uint16_t magic; uint8_t pad[58]; uint32_t lfanew; };
struct FileHeader { uint16_t machine, nsections; uint32_t timestamp, symtab, nsyms; uint16_t opt_size, characteristics; };
struct DataDir { uint32_t rva, size; };
struct OptHeader32 {
    uint16_t magic; uint8_t maj_link, min_link; uint32_t size_code, size_init, size_uninit, entry, base_code, base_data;
    uint32_t image_base, section_align, file_align;
    uint16_t maj_os, min_os, maj_img, min_img, maj_sub, min_sub; uint32_t win32_ver, size_image, size_headers, checksum;
    uint16_t subsystem, dll_char; uint32_t stack_reserve, stack_commit, heap_reserve, heap_commit, loader_flags, nrva;
    DataDir dd[16];
};
struct SectionHeader {
    char name[8]; uint32_t vsize, va, raw_size, raw_ptr, reloc_ptr, line_ptr; uint16_t nreloc, nline; uint32_t characteristics;
};
struct TlsDir32 { uint32_t start_raw, end_raw, index_addr, callbacks, zero_fill, characteristics; };
struct ExportDir { uint32_t chars, ts; uint16_t maj, min; uint32_t name, base, nfuncs, nnames, funcs, names, ords; };
#pragma pack(pop)

uint32_t rup(uint32_t x, uint32_t a) { return (x + a - 1) & ~(a - 1); }

bool read_file(const std::string &host, std::vector<uint8_t> *out) {
    int fd = open(host.c_str(), O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    fstat(fd, &st);
    out->resize((size_t)st.st_size);
    size_t done = 0;
    while (done < out->size()) {
        ssize_t n = pread(fd, out->data() + done, out->size() - done, (off_t)done);
        if (n <= 0) { close(fd); return false; }
        done += (size_t)n;
    }
    close(fd);
    return true;
}

std::string basename_lower(const std::string &p) {
    size_t k = p.find_last_of("/\\");
    return to_lower_ascii(k == std::string::npos ? p : p.substr(k + 1));
}

std::string guest_path_for(const GuestModuleDesc &d) {
    std::string rel = d.relpath ? d.relpath : d.name;
    for (auto &c : rel) if (c == '/') c = '\\';
    return config().guest_game_dir + "\\" + rel;
}

LoadedModule *add_module(std::unique_ptr<LoadedModule> m) {
    std::lock_guard<std::mutex> lk(g_mod_mu);
    g_modules.push_back(std::move(m));
    return g_modules.back().get();
}

struct Prot { uint32_t va, size, prot; };

bool map_one(const GuestModuleDesc &d, std::vector<Prot> *prots, std::string *err) {
    std::string name = d.name;
    std::string gpath = guest_path_for(d);
    std::string host;
    bool isd = false;
    if (!vfs_resolve(gpath, &host, &isd) || isd) {
        *err = "missing file for module " + name + ": expected " + (d.relpath ? d.relpath : name) + " under the game root " + config().game_root;
        return false;
    }
    std::vector<uint8_t> f;
    if (!read_file(host, &f)) { *err = "cannot read " + host; return false; }
    if (config().verify_hashes) {
        uint8_t h[32];
        sha256(f.data(), f.size(), h);
        if (memcmp(h, d.sha256, 32) != 0) {
            *err = "SHA-256 mismatch for " + name + " (" + host + "): file is " + hex(h, 32) + ", the recompiled program expects " + hex(d.sha256, 32) +
                   ". This is not the supported Steam build of the game.";
            return false;
        }
    }
    if (f.size() < sizeof(DosHeader)) { *err = name + ": too small"; return false; }
    const DosHeader *dos = (const DosHeader *)f.data();
    if (dos->magic != 0x5A4D || dos->lfanew + 4 + sizeof(FileHeader) + sizeof(OptHeader32) > f.size()) { *err = name + ": bad DOS header"; return false; }
    if (memcmp(f.data() + dos->lfanew, "PE\0\0", 4) != 0) { *err = name + ": bad PE signature"; return false; }
    const FileHeader *fh = (const FileHeader *)(f.data() + dos->lfanew + 4);
    const OptHeader32 *oh = (const OptHeader32 *)(fh + 1);
    if (fh->machine != 0x14C || oh->magic != 0x10B) { *err = name + ": not a PE32 i386 image"; return false; }
    const SectionHeader *sh = (const SectionHeader *)((const uint8_t *)oh + fh->opt_size);
    if ((const uint8_t *)(sh + fh->nsections) > f.data() + f.size()) { *err = name + ": truncated section table"; return false; }
    if (oh->size_image != d.image_size) LOGW(LC_LOADER, "%s: SizeOfImage %08x differs from program description %08x", name.c_str(), oh->size_image, d.image_size);
    if (oh->image_base != d.preferred_base) LOGW(LC_LOADER, "%s: ImageBase %08x differs from program description %08x", name.c_str(), oh->image_base, d.preferred_base);
    uint32_t image_size = rup(std::max(oh->size_image, d.image_size), kGuestPage);
    uint32_t verr = 0;
    if (!vm_map_image(d.base, image_size, &verr)) {
        char b[64];
        snprintf(b, sizeof b, "%08x", d.base);
        *err = "cannot reserve " + name + " at 0x" + b + " (guest address range busy, error " + std::to_string(verr) + ")";
        return false;
    }
    uint32_t base = d.base;
    uint8_t *img = guest_base + base;
    memset(img, 0, image_size);
    uint32_t hsz = std::min<uint32_t>(oh->size_headers, (uint32_t)f.size());
    memcpy(img, f.data(), hsz);
    for (unsigned i = 0; i < fh->nsections; i++) {
        const SectionHeader &s = sh[i];
        uint32_t span = std::max(s.vsize, s.raw_size);
        if ((uint64_t)s.va + span > image_size) { *err = name + ": section out of image"; return false; }
        uint32_t n = std::min<uint32_t>(s.raw_size, span);
        if (s.raw_ptr + (uint64_t)n > f.size()) n = s.raw_ptr < f.size() ? (uint32_t)(f.size() - s.raw_ptr) : 0;
        if (n) memcpy(img + s.va, f.data() + s.raw_ptr, n);
    }
    // relocations
    int32_t delta = (int32_t)(base - oh->image_base);
    if (delta != 0) {
        DataDir rd = oh->dd[5];
        uint32_t count = 0;
        if (rd.rva && rd.size) {
            uint32_t off = 0;
            while (off + 8 <= rd.size) {
                uint32_t page = *(uint32_t *)(img + rd.rva + off);
                uint32_t bsz = *(uint32_t *)(img + rd.rva + off + 4);
                if (bsz < 8) break;
                for (uint32_t k = 8; k + 2 <= bsz; k += 2) {
                    uint16_t e = *(uint16_t *)(img + rd.rva + off + k);
                    uint32_t type = e >> 12, o = e & 0xFFF;
                    if (type == 3 && page + o + 4 <= image_size) { *(uint32_t *)(img + page + o) += (uint32_t)delta; count++; }
                    else if (type != 0 && type != 3) LOGW(LC_LOADER, "%s: unsupported reloc type %u", name.c_str(), type);
                }
                off += bsz;
            }
        } else {
            LOGW(LC_LOADER, "%s: base differs from preferred but the image has no .reloc", name.c_str());
        }
        LOGI(LC_LOADER, "%s: applied %u relocations (delta %+d)", name.c_str(), count, delta);
    }
    auto m = std::make_unique<LoadedModule>();
    m->name = name;
    m->lname = to_lower_ascii(name);
    m->guest_path = gpath;
    m->base = base;
    m->size = image_size;
    m->entry = d.entry;
    m->preferred = oh->image_base;
    m->is_dll = d.is_dll != 0;
    m->stack_reserve = oh->stack_reserve;
    m->res_rva = oh->dd[2].rva;
    m->res_size = oh->dd[2].size;
    // exports
    DataDir ed = oh->dd[0];
    if (ed.rva && ed.size) {
        m->export_dir_rva = ed.rva;
        m->export_dir_size = ed.size;
        const ExportDir *e = (const ExportDir *)(img + ed.rva);
        m->ordinal_base = e->base;
        const uint32_t *funcs = (const uint32_t *)(img + e->funcs);
        const uint32_t *names = (const uint32_t *)(img + e->names);
        const uint16_t *ords = (const uint16_t *)(img + e->ords);
        std::vector<bool> named(e->nfuncs, false);
        for (uint32_t i = 0; i < e->nnames; i++) {
            uint16_t o = ords[i];
            if (o < e->nfuncs) { m->exports.push_back({(const char *)(img + names[i]), e->base + o, funcs[o]}); named[o] = true; }
        }
        for (uint32_t i = 0; i < e->nfuncs; i++)
            if (!named[i] && funcs[i]) m->exports.push_back({"", e->base + i, funcs[i]});
    }
    // TLS
    DataDir td = oh->dd[9];
    if (td.rva && td.size >= sizeof(TlsDir32)) {
        const TlsDir32 *t = (const TlsDir32 *)(img + td.rva);
        PeTls pt{t->start_raw, t->end_raw, t->zero_fill, t->index_addr, t->callbacks, 0};
        pe_tls_register(pt);
        if (t->callbacks)
            for (uint32_t a = t->callbacks; rd32(a); a += 4) m->tls_callbacks.push_back(rd32(a));
        LOGI(LC_LOADER, "%s: static TLS %u+%u bytes, %zu callbacks", name.c_str(), t->end_raw - t->start_raw, t->zero_fill, m->tls_callbacks.size());
    }
    prots->push_back({0, rup(hsz, kGuestPage), PAGE_READONLY});
    for (unsigned i = 0; i < fh->nsections; i++) {
        const SectionHeader &s = sh[i];
        uint32_t span = rup(std::max(s.vsize, s.raw_size), kGuestPage);
        if (!span) continue;
        prots->push_back({s.va, span, (s.characteristics & 0x80000000u) ? PAGE_READWRITE : PAGE_READONLY});
    }
    add_module(std::move(m));
    LOGI(LC_LOADER, "mapped %s at %08x size %08x entry %08x%s (%s)", name.c_str(), base, image_size, d.entry, d.is_dll ? " [dll]" : "", host.c_str());
    return true;
}

std::string lname_of(const std::string &n) {
    std::string b = basename_lower(n);
    if (b.find('.') == std::string::npos) b += ".dll";
    return b;
}

bool call_entry(const LoadedModule &m, uint32_t addr, uint32_t reason, bool tls_cb) {
    if (!guest_lookup(addr)) {
        LOGW(LC_LOADER, "%s: %s %08x is not a lifted entry, skipped", m.name.c_str(), tls_cb ? "TLS callback" : "DllMain", addr);
        return true;
    }
    uint32_t r = guest_invoke(addr, Conv::Stdcall, m.base, reason, 0u);
    return tls_cb ? true : r != 0 || reason != 1;
}

// Lower-case names of the DLLs in a mapped module's import directory (not its delay imports).
std::vector<std::string> static_imports(const LoadedModule &m) {
    std::vector<std::string> out;
    const DosHeader *dos = (const DosHeader *)(guest_base + m.base);
    const OptHeader32 *oh = (const OptHeader32 *)(guest_base + m.base + dos->lfanew + 4 + sizeof(FileHeader));
    if (oh->nrva < 2 || !oh->dd[1].rva) return out;
    for (uint32_t d = m.base + oh->dd[1].rva; rd32(d + 12) != 0; d += 20) out.push_back(lname_of(read_astr(m.base + rd32(d + 12))));
    return out;
}

std::recursive_mutex g_load_mu;   // the loader lock: serialises on-demand loads (DllMain may load further DLLs)

}  // namespace

uint32_t module_load_on_demand(const std::string &name) {
    std::lock_guard<std::recursive_mutex> lk(g_load_mu);
    LoadedModule *m = nullptr;
    {
        std::string ln = lname_of(name);
        std::lock_guard<std::mutex> ml(g_mod_mu);
        for (auto &x : g_modules) if (x->lname == ln && !x->fake) m = x.get();
    }
    if (!m) return 0;
    if (!m->hidden) return m->attached || !m->entry ? m->base : 0;   // loaded already (or being loaded: a DllMain recursion)
    m->hidden = false;
    for (const std::string &dep : static_imports(*m)) {
        const LoadedModule *dm = nullptr;
        {
            std::lock_guard<std::mutex> ml(g_mod_mu);
            for (auto &x : g_modules) if (x->lname == dep && x->hidden) dm = x.get();
        }
        if (dm && !module_load_on_demand(dep)) {
            LOGE(LC_LOADER, "LoadLibrary(%s): dependency %s failed", m->name.c_str(), dep.c_str());
            m->hidden = true;
            return 0;
        }
    }
    for (uint32_t cb : m->tls_callbacks) call_entry(*m, cb, 1, true);
    if (m->entry) {
        LOGI(LC_LOADER, "LoadLibrary(%s): DllMain(PROCESS_ATTACH)", m->name.c_str());
        if (!call_entry(*m, m->entry, 1, false)) {
            LOGE(LC_LOADER, "DllMain of %s returned FALSE for PROCESS_ATTACH", m->name.c_str());
            m->hidden = true;
            return 0;
        }
    }
    m->attached = true;
    return m->base;
}

bool is_simulated_system_dll(const std::string &lname) {
    size_t n;
    const HleSig *t = sig_table(&n);
    static std::vector<std::string> *names = nullptr;
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    if (!names) {
        names = new std::vector<std::string>();
        for (size_t i = 0; i < n; i++)
            if (names->empty() || names->back() != t[i].dll) names->push_back(t[i].dll);
        for (const char *extra : {"msvcrt.dll", "ucrtbase.dll", "kernelbase.dll"})
            names->push_back(extra);
    }
    return std::find(names->begin(), names->end(), lname) != names->end();
}

const LoadedModule *module_by_name(const std::string &name) {
    std::string ln = lname_of(name);
    std::lock_guard<std::mutex> lk(g_mod_mu);
    for (auto &m : g_modules) if (m->lname == ln && !m->hidden) return m.get();
    // "foo" may also match "foo.exe"
    std::string b = basename_lower(name);
    for (auto &m : g_modules) if (m->lname == b && !m->hidden) return m.get();
    return nullptr;
}
const LoadedModule *module_by_base(uint32_t base) {
    std::lock_guard<std::mutex> lk(g_mod_mu);
    for (auto &m : g_modules) if (m->base == base) return m.get();
    return nullptr;
}
LoadedModule *module_mutable(uint32_t base) { return const_cast<LoadedModule *>(module_by_base(base)); }
const LoadedModule *module_main_exe() {
    std::lock_guard<std::mutex> lk(g_mod_mu);
    for (auto &m : g_modules) if (!m->is_dll && !m->fake) return m.get();
    return nullptr;
}
std::vector<const LoadedModule *> module_list() {
    std::lock_guard<std::mutex> lk(g_mod_mu);
    std::vector<const LoadedModule *> v;
    for (auto &m : g_modules) if (!m->hidden) v.push_back(m.get());
    return v;
}

uint32_t module_fake_handle(const std::string &name) {
    std::string ln = lname_of(name);
    if (const LoadedModule *m = module_by_name(ln)) return m->base;
    if (!is_simulated_system_dll(ln)) return 0;
    uint32_t addr = vm_alloc(0, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!addr) return 0;
    uint8_t *p = guest_base + addr;
    memset(p, 0, 0x1000);
    DosHeader *dos = (DosHeader *)p;
    dos->magic = 0x5A4D;
    dos->lfanew = 0x80;
    memcpy(p + 0x80, "PE\0\0", 4);
    FileHeader *fh = (FileHeader *)(p + 0x84);
    fh->machine = 0x14C;
    fh->nsections = 0;
    fh->opt_size = sizeof(OptHeader32);
    fh->characteristics = 0x2102;
    OptHeader32 *oh = (OptHeader32 *)(fh + 1);
    oh->magic = 0x10B;
    oh->image_base = addr;
    oh->section_align = 0x1000;
    oh->file_align = 0x200;
    oh->size_image = 0x1000;
    oh->size_headers = 0x400;
    oh->nrva = 16;
    vm_protect(addr, 0x1000, PAGE_READONLY, nullptr);
    auto m = std::make_unique<LoadedModule>();
    m->name = ln;
    m->lname = ln;
    m->base = addr;
    m->size = 0x1000;
    m->fake = true;
    m->is_dll = true;
    static const char *kGameDlls[] = {"steam_api.dll", "libcurl.dll", "cudart32_30_9.dll", "mss32.dll", "nvcuda.dll"};
    bool in_game = false;
    for (auto g : kGameDlls) if (ln == g) in_game = true;
    m->guest_path = (in_game ? config().guest_game_dir + "\\Binaries\\Win32\\" : std::string("C:\\Windows\\System32\\")) + ln;
    add_module(std::move(m));
    LOGD(LC_LOADER, "simulated system module %s at %08x", ln.c_str(), addr);
    return addr;
}

uint32_t module_get_proc(uint32_t hmod, const char *name, uint32_t ordinal) {
    const LoadedModule *m = module_by_base(hmod);
    if (!m) return 0;
    if (m->fake) return hle_dynamic_proc(m->lname.c_str(), name, ordinal);
    const ExportEntry *hit = nullptr;
    for (auto &e : m->exports) {
        if (name ? e.name == name : e.ordinal == ordinal) { hit = &e; break; }
    }
    if (!hit) return 0;
    if (hit->rva >= m->export_dir_rva && hit->rva < m->export_dir_rva + m->export_dir_size) {   // forwarder "dll.func"
        std::string fw = (const char *)(guest_base + m->base + hit->rva);
        size_t dot = fw.find('.');
        if (dot == std::string::npos) return 0;
        std::string dll = lname_of(fw.substr(0, dot)), fn = fw.substr(dot + 1);
        uint32_t h = module_fake_handle(dll);
        if (const LoadedModule *t = module_by_name(dll)) h = t->base;
        return h ? module_get_proc(h, fn.c_str(), 0) : 0;
    }
    return m->base + hit->rva;
}

bool loader_map_all(const GuestProgram &prog, std::string *err) {
    std::map<uint32_t, std::vector<Prot>> prots;
    for (uint32_t i = 0; i < prog.n_modules; i++) {
        std::vector<Prot> p;
        if (!map_one(prog.modules[i], &p, err)) return false;
        prots[prog.modules[i].base] = std::move(p);
    }
    for (uint32_t i = 0; i < prog.n_iat; i++) {
        const GuestIatEntry &e = prog.iat[i];
        uint32_t v = e.kind == GUEST_IAT_IMPORT ? thunk_addr(e.value) : e.value;
        if (e.kind == GUEST_IAT_IMPORT && e.value >= prog.n_imports) {
            *err = "IAT entry " + std::to_string(i) + " refers to import index beyond n_imports";
            return false;
        }
        if (!vm_is_committed(e.slot, 4)) { *err = "IAT slot outside any mapped image"; return false; }
        wr32(e.slot, v);
    }
    for (auto &kv : prots)
        for (auto &p : kv.second)
            if (!vm_protect(kv.first + p.va, p.size, p.prot, nullptr)) LOGW(LC_LOADER, "protect %08x+%x failed", kv.first + p.va, p.size);
    if (RtThread *t = thread_current()) pe_tls_init_thread(t);
    // Windows initialises a DLL at startup only when the exe imports it, directly or through other DLLs. Recompiled DLLs outside
    // that closure (delay-loaded, or loaded by name like PhysXLoader -> PhysXCore) stay hidden until their first LoadLibrary.
    // Edges: names in a module's import directory, and lifted IAT entries (slot in one module, target in another).
    auto mod_index = [&](uint32_t addr) -> int {
        for (uint32_t i = 0; i < prog.n_modules; i++)
            if (addr - prog.modules[i].base < prog.modules[i].image_size) return (int)i;
        return -1;
    };
    std::vector<std::vector<int>> edges(prog.n_modules);
    for (uint32_t i = 0; i < prog.n_modules; i++) {
        const LoadedModule *m = module_by_base(prog.modules[i].base);
        if (!m) continue;
        for (const std::string &n : static_imports(*m))
            for (uint32_t j = 0; j < prog.n_modules; j++)
                if (lname_of(prog.modules[j].name) == n) edges[i].push_back((int)j);
    }
    for (uint32_t k = 0; k < prog.n_iat; k++) {
        if (prog.iat[k].kind != GUEST_IAT_GUEST_ADDR) continue;
        int a = mod_index(prog.iat[k].slot), b = mod_index(prog.iat[k].value);
        if (a >= 0 && b >= 0 && a != b) edges[a].push_back(b);
    }
    std::vector<bool> reached(prog.n_modules, false);
    std::vector<int> work;
    for (uint32_t i = 0; i < prog.n_modules; i++)
        if (!prog.modules[i].is_dll) { reached[i] = true; work.push_back((int)i); }
    while (!work.empty()) {
        int i = work.back();
        work.pop_back();
        for (int j : edges[i]) if (!reached[j]) { reached[j] = true; work.push_back(j); }
    }
    bool have_exe = std::find(reached.begin(), reached.end(), true) != reached.end();
    for (uint32_t i = 0; i < prog.n_modules && have_exe; i++) {
        if (reached[i]) continue;
        if (LoadedModule *m = module_mutable(prog.modules[i].base)) {
            m->hidden = true;
            LOGI(LC_LOADER, "%s: loaded on demand (not imported by the exe)", m->name.c_str());
        }
    }
    return true;
}

void loader_thread_notify(uint32_t reason) {
    for (const LoadedModule *m : module_list()) {
        if (m->fake || !m->attached) continue;
        for (uint32_t cb : m->tls_callbacks) call_entry(*m, cb, reason, true);
        if (m->is_dll && m->entry && !m->no_thread_calls) call_entry(*m, m->entry, reason, false);
    }
}

int loader_run_entries(const GuestProgram &prog) {
    const LoadedModule *exe = nullptr;
    for (uint32_t i = 0; i < prog.n_modules; i++) {
        const GuestModuleDesc &d = prog.modules[i];
        LoadedModule *m = module_mutable(d.base);
        if (!m) continue;
        if (!d.is_dll) { exe = m; continue; }
        if (m->hidden) continue;   // module_load_on_demand
        for (uint32_t cb : m->tls_callbacks) call_entry(*m, cb, 1, true);
        if (d.entry) {
            LOGI(LC_LOADER, "DllMain(%s, PROCESS_ATTACH)", d.name);
            if (!call_entry(*m, d.entry, 1, false)) {
                LOGE(LC_LOADER, "DllMain of %s returned FALSE for PROCESS_ATTACH", d.name);
                return -1;
            }
        }
        m->attached = true;
    }
    if (!exe) { LOGE(LC_LOADER, "program has no executable module"); return -1; }
    for (uint32_t cb : exe->tls_callbacks) call_entry(*exe, cb, 1, true);
    const_cast<LoadedModule *>(exe)->attached = true;
    if (!exe->entry) { LOGE(LC_LOADER, "executable has no entry point"); return -1; }
    LOGI(LC_LOADER, "calling exe entry %08x", exe->entry);
    uint32_t r = guest_invoke(exe->entry, Conv::Stdcall);
    LOGI(LC_LOADER, "exe entry returned %u", r);
    return (int)r;
}

void loader_reset() {
    std::lock_guard<std::mutex> lk(g_mod_mu);
    g_modules.clear();
}

bool runtime_init(const GuestProgram &prog) {
    log_init_from_env();
    if (prog.abi_version != GUEST_ABI_VERSION) {
        LOGE(LC_CORE, "guest_program ABI version %u != runtime %u", prog.abi_version, GUEST_ABI_VERSION);
        return false;
    }
    if (const char *e = getenv("DISH_TRACE_CALLS")) g_hle_trace = e[0] && e[0] != '0';
    if (!vm_init()) return false;
    fault_install();
    fault_thread_init();
    vfs_init();
    uint32_t exe_base = 0x400000;
    for (uint32_t i = 0; i < prog.n_modules; i++) if (!prog.modules[i].is_dll) exe_base = prog.modules[i].base;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    for (uint32_t i = 0; i < prog.n_modules; i++) {
        uint32_t err = 0;
        if (!vm_reserve_image(prog.modules[i].base, (prog.modules[i].image_size + 0xFFFu) & ~0xFFFu, &err)) {
            LOGE(LC_LOADER, "cannot reserve %s at %08x (error %u)", prog.modules[i].name, prog.modules[i].base, err);
            return false;
        }
    }
    if (!process_init_peb(exe_base, ncpu > 0 ? (uint32_t)ncpu : 1)) return false;
    for (uint32_t i = 0; i < prog.n_imports; i++) {
        std::string dll = to_lower_ascii(prog.imports[i].dll ? prog.imports[i].dll : "");
        hle_bind(dll.c_str(), prog.imports[i].name, prog.imports[i].ordinal, i);
    }
    g_thread_notify = loader_thread_notify;
    LOGI(LC_CORE, "runtime initialised: %u modules, %u imports bound (%zu implemented HLE functions registered)", prog.n_modules,
         prog.n_imports, hle_impl_count());
    return true;
}

namespace {
struct RunCtx { const GuestProgram *prog; };
int run_body(void *p) {
    const GuestProgram &prog = *((RunCtx *)p)->prog;
    std::string err;
    if (!loader_map_all(prog, &err)) {
        LOGE(LC_LOADER, "loader: %s", err.c_str());
        return -1;
    }
    return loader_run_entries(prog);
}
}  // namespace

int runtime_run(const GuestProgram &prog) {
    if (!runtime_init(prog)) return -1;
    profiler_start(log_dir());
    RunCtx c{&prog};
    int rc = thread_run_main(run_body, &c, 8u << 20);
    stub_report(true);
    log_flush();
    return rc;
}

}  // namespace dish
