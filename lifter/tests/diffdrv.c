/* Differential-test driver: runs lifted functions in a tiny fake runtime (fork per case, write tracking by page protection).
 * usage: diffdrv <images.txt> <cases.bin> <results.bin>
 *   images.txt : lines "base size path" (read-only MAP_PRIVATE images; writes are tracked and copy-on-write)
 *   cases.bin  : u32 magic 'DIFC', u32 n, then n * Case
 *   results.bin: n * Result (+ dirty page list)
 */
#define _GNU_SOURCE
/* gen.h declares _setjmp itself (EH landing pads, ABI v3); keep glibc's prototype out of the way */
#define _setjmp diffdrv_glibc__setjmp
#include <setjmp.h>
#undef _setjmp
#include "gen.h"
#include <fcntl.h>
#include <ucontext.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

uint8_t *guest_base;
_Thread_local GuestThread *guest_thread;

#define HEAP_BASE 0x10000000u
#define HEAP_SIZE 0x100000u
#define STACK_BASE 0x20000000u
#define STACK_SIZE 0x40000u
#define TEB_BASE 0x30000000u
#define TEB_SIZE 0x1000u
#define SENTINEL 0xFFFF8000u

typedef struct { uint32_t fn, esp, r[7]; uint64_t seed; uint32_t flags, pad; uint32_t args[8]; } Case;
typedef struct { uint32_t status, eax, edx, pop, ctr, npages; double st0; } ResultHdr;

typedef struct { uint32_t base, size; int tracked; } Region;
static Region regions[64];
static int nregions;
static uint32_t dirty[65536];
static int ndirty;
static sigjmp_buf jb;
static volatile int fault_kind;   /* 1 = fault, 2 = unresolved, 3 = timeout */
static uint64_t case_seed;
static uint32_t stub_ctr;
static uint32_t entry_esp;
static uint32_t lazy_pages[65536];
static int nlazy;

static uint64_t splitmix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static uint64_t stub_mix(uint64_t seed, uint32_t ctr, uint32_t idx) { return splitmix(seed ^ ((uint64_t)ctr << 32) ^ ((uint64_t)idx * 0x9E3779B97F4A7C15ull)); }

GuestRet guest_unresolved(uint32_t target, uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx) {
    (void)esp; (void)eax; (void)ecx; (void)edx;
    if (getenv("DIFF_DEBUG")) fprintf(stderr, "unresolved target %08x\n", target);
    fault_kind = 2;
    siglongjmp(jb, 1);
}

GuestRet guest_call(uint32_t target, uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx) {
    if (target >= GUEST_THUNK_BASE && target < GUEST_THUNK_END) {
        uint32_t idx = (target - GUEST_THUNK_BASE) / GUEST_THUNK_STRIDE;
        uint64_t v = stub_mix(case_seed, stub_ctr++, idx);
        return guest_ret((uint32_t)v, (uint32_t)(v >> 32), 4 * (idx % 5));
    }
    GuestFn f = guest_lookup(target);
    if (!f) return guest_unresolved(target, esp, eax, ecx, edx);
    return f(esp, eax, ecx, edx);
}

static void fill(uint32_t base, uint32_t size, uint64_t seed);
static void on_segv(int sig, siginfo_t *si, void *ucv) {
    (void)sig;
    ucontext_t *uc = (ucontext_t *)ucv;
    uintptr_t a = (uintptr_t)si->si_addr;
    uintptr_t gb = (uintptr_t)guest_base;
    if (a >= gb && a < gb + (1ull << 32) && (uintptr_t)uc->uc_mcontext.gregs[REG_RIP] != a) {
        uint32_t ga = (uint32_t)(a - gb);
        int known = 0;
        for (int i = 0; i < nregions; i++) if (ga >= regions[i].base && ga < regions[i].base + regions[i].size) known = 1;
        for (int i = 0; i < nlazy && i < 65536; i++) if (lazy_pages[i] == (ga & ~0xfffu)) known = 1;
        if (!known && ga >= 0x10000 && ga < 0xFFEF0000u) {
            uint32_t pg = ga & ~0xfffu;
            if (mmap(guest_base + pg, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != MAP_FAILED) {
                fill(pg, 4096, case_seed ^ 4);
                if (!(uc->uc_mcontext.gregs[REG_ERR] & 2)) mprotect(guest_base + pg, 4096, PROT_READ);
                else if (ndirty < 65536) dirty[ndirty++] = pg;
                lazy_pages[nlazy++ & 0xFFFF] = pg;
                return;
            }
        }
    }
    if (si->si_code == SEGV_ACCERR && a >= gb && a < gb + (1ull << 32)) {
        uint32_t ga = (uint32_t)(a - gb);
        int is_lazy = 0;
        for (int i = 0; i < nlazy && i < 65536; i++) if (lazy_pages[i] == (ga & ~0xfffu)) { is_lazy = 1; break; }
        if (is_lazy) {
            uint32_t pg = ga & ~0xfffu;
            mprotect(guest_base + pg, 4096, PROT_READ | PROT_WRITE);
            if (ndirty < 65536) dirty[ndirty++] = pg;
            return;
        }
        for (int i = 0; i < nregions; i++) {
            if (regions[i].tracked && ga >= regions[i].base && ga < regions[i].base + regions[i].size) {
                uint32_t pg = ga & ~0xfffu;
                mprotect(guest_base + pg, 4096, PROT_READ | PROT_WRITE);
                if (ndirty < 65536) dirty[ndirty++] = pg;
                return;
            }
        }
    }
    if (getenv("DIFF_DEBUG")) { char b[200]; int n = snprintf(b, sizeof b, "fault: addr=%lx guest=%lx code=%d rip=%llx err=%llx\n", (unsigned long)a, (unsigned long)(a - gb), si->si_code, (unsigned long long)uc->uc_mcontext.gregs[REG_RIP], (unsigned long long)uc->uc_mcontext.gregs[REG_ERR]); (void)!write(2, b, n); }
    fault_kind = 1;
    siglongjmp(jb, 1);
}
static void on_alarm(int sig) { (void)sig; fault_kind = 3; siglongjmp(jb, 1); }

static uint64_t K[512];

static uint64_t page_hash(uint32_t pg, uint32_t zero_lo, uint32_t zero_hi) {
    uint8_t tmp[4096];
    memcpy(tmp, guest_base + pg, 4096);
    for (uint32_t o = 0; o < 4096; o++) { uint32_t a = pg + o; if (a >= zero_lo && a < zero_hi) tmp[o] = 0; }
    uint64_t h = 0;
    for (int i = 0; i < 512; i++) { uint64_t w; memcpy(&w, tmp + i * 8, 8); h += w * K[i]; }
    return h;
}

static uint32_t xf(uint32_t w) {
    uint32_t sel = (w >> 29) & 7;
    if (sel <= 3) return HEAP_BASE + (w & 0x7FFFC);
    if (sel == 4) return w & 0x3F;
    if (sel == 5) return STACK_BASE + 0x10000 + (w & 0xFFFC);
    return w;
}
static void fill(uint32_t base, uint32_t size, uint64_t seed) {
    uint32_t *p = (uint32_t *)(guest_base + base);
    for (uint32_t i = 0; i < size / 8; i++) {
        uint64_t r = splitmix(seed ^ ((uint64_t)(base + i * 8) * 0xD6E8FEB86659FD93ull));
        p[2 * i] = xf((uint32_t)r);
        p[2 * i + 1] = xf((uint32_t)(r >> 32));
    }
}

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    for (int i = 0; i < 512; i++) K[i] = splitmix(0x1234 + i) | 1;
    guest_base = mmap(NULL, 1ull << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (guest_base == MAP_FAILED) { perror("reserve"); return 3; }
    FILE *fi = fopen(argv[1], "r");
    char line[1024];
    while (fgets(line, sizeof line, fi)) {
        unsigned base, size; char path[900];
        if (sscanf(line, "%x %x %899s", &base, &size, path) != 3) continue;
        int fd = open(path, O_RDONLY);
        if (fd < 0) { perror(path); return 4; }
        if (mmap(guest_base + base, size, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0) == MAP_FAILED) { perror("map image"); return 5; }
        regions[nregions++] = (Region){base, size, 1};
    }
    fclose(fi);
    struct { uint32_t b, s; } ar[3] = {{HEAP_BASE, HEAP_SIZE}, {STACK_BASE, STACK_SIZE}, {TEB_BASE, TEB_SIZE}};
    for (int i = 0; i < 3; i++) {
        if (mmap(guest_base + ar[i].b, ar[i].s, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) { perror("arena"); return 6; }
        regions[nregions++] = (Region){ar[i].b, ar[i].s, 1};
    }
    /* identical to the Unicorn side: nop pages in the thunk/sentinel region and a flat GDT page */
    {
        uint32_t nb[3][2] = {{0xFFF00000u, 0xF0000u}, {0xFFFF0000u, 0x10000u}, {0x40000000u, 0x1000u}};
        for (int i = 0; i < 3; i++) {
            if (mmap(guest_base + nb[i][0], nb[i][1], PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) { perror("nop region"); return 6; }
            if (i < 2) memset(guest_base + nb[i][0], 0x90, nb[i][1]);
            regions[nregions++] = (Region){nb[i][0], nb[i][1], 1};
        }
        /* the Unicorn GDT page is not zero: it holds the descriptors */
    }
    FILE *fc = fopen(argv[2], "rb");
    uint32_t hdr[2];
    if (fread(hdr, 4, 2, fc) != 2 || hdr[0] != 0x43464944u) return 7;
    uint32_t n = hdr[1];
    FILE *fo = fopen(argv[3], "wb");
    static GuestThread gt;
    for (uint32_t ci = 0; ci < n; ci++) {
        Case c;
        if (fread(&c, sizeof c, 1, fc) != 1) return 8;
        int pfd[2];
        if (pipe(pfd)) return 9;
        pid_t pid = fork();
        if (pid == 0) {
            close(pfd[0]);
            struct sigaction sa = {0};
            sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
            static char altstack[65536];
            stack_t ss = {.ss_sp = altstack, .ss_size = sizeof altstack};
            sigaltstack(&ss, 0);
            sigaction(SIGSEGV, &sa, 0);
            sigaction(SIGBUS, &sa, 0);
            signal(SIGALRM, on_alarm);
            alarm(3);
            case_seed = c.seed; nlazy = 0; stub_ctr = 0; ndirty = 0; entry_esp = c.esp;
            fill(HEAP_BASE, HEAP_SIZE, c.seed ^ 1);
            fill(STACK_BASE, STACK_SIZE, c.seed ^ 2);
            fill(TEB_BASE, TEB_SIZE, c.seed ^ 3);
            *(uint32_t *)(guest_base + TEB_BASE + 0) = 0xFFFFFFFFu;
            *(uint32_t *)(guest_base + TEB_BASE + 4) = STACK_BASE + STACK_SIZE;
            *(uint32_t *)(guest_base + TEB_BASE + 8) = STACK_BASE;
            *(uint32_t *)(guest_base + TEB_BASE + 0x18) = TEB_BASE;
            *(uint32_t *)(guest_base + TEB_BASE + 0x2C) = 0;
            *(uint32_t *)(guest_base + c.esp) = SENTINEL;
            memcpy(guest_base + c.esp + 4, c.args, sizeof c.args);
            for (int i = 0; i < nregions; i++) mprotect(guest_base + regions[i].base, regions[i].size, PROT_READ);
            memset(&gt, 0, sizeof gt);
            gt.teb = TEB_BASE; gt.x87_cw = 0x27F; gt.mxcsr = 0x1F80;
            guest_thread = &gt;
            gt.ext_regs[0] = c.r[3]; gt.ext_regs[1] = c.r[4]; gt.ext_regs[2] = c.r[5]; gt.ext_regs[3] = c.r[6];
            ResultHdr rh; memset(&rh, 0, sizeof rh);
            if (sigsetjmp(jb, 1) == 0) {
                GuestFn f = guest_lookup(c.fn);
                if (!f) { rh.status = 5; }
                else {
                    GuestRet r = f(c.esp, c.r[0], c.r[1], c.r[2]);
                    rh.status = 0; rh.eax = guest_ret_eax(r); rh.edx = guest_ret_edx(r); rh.pop = guest_ret_pop(r);
                }
            } else {
                rh.status = fault_kind;
            }
            alarm(0);
            rh.ctr = stub_ctr; rh.st0 = gt.st0_ret;
            uint32_t lo = STACK_BASE, hi = c.esp;
            /* de-duplicate dirty pages */
            uint32_t up[65536]; int nu = 0;
            for (int i = 0; i < ndirty; i++) { int dup = 0; for (int j = 0; j < nu; j++) if (up[j] == dirty[i]) { dup = 1; break; } if (!dup) up[nu++] = dirty[i]; }
            rh.npages = rh.status == 0 ? (uint32_t)nu : 0;
            (void)!write(pfd[1], &rh, sizeof rh);
            { uint8_t cap[512]; memcpy(cap, guest_base + HEAP_BASE + 0x80000, 512); (void)!write(pfd[1], cap, 512); }
            if (rh.status == 0) {
                for (int i = 0; i < nu; i++) {
                    struct { uint32_t pg, pad; uint64_t h; } e = {up[i], 0, page_hash(up[i], lo, hi)};
                    (void)!write(pfd[1], &e, sizeof e);
                }
            }
            _exit(0);
        }
        close(pfd[1]);
        ResultHdr rh; memset(&rh, 0, sizeof rh);
        ssize_t got = read(pfd[0], &rh, sizeof rh);
        if (got != (ssize_t)sizeof rh) { memset(&rh, 0, sizeof rh); rh.status = 9; }
        fwrite(&rh, sizeof rh, 1, fo);
        { uint8_t cap[512]; memset(cap, 0, 512); if (got == (ssize_t)sizeof rh) (void)!read(pfd[0], cap, 512); fwrite(cap, 512, 1, fo); }
        for (uint32_t i = 0; i < rh.npages; i++) {
            char e[16];
            if (read(pfd[0], e, 16) != 16) break;
            fwrite(e, 16, 1, fo);
        }
        close(pfd[0]);
        int st; waitpid(pid, &st, 0);
        if (got != (ssize_t)sizeof rh) { /* crashed child: status 9 recorded */ }
    }
    fclose(fo);
    return 0;
}
