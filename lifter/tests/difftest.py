#!/usr/bin/env python3
"""Differential test: lifted C (tests/diffdrv, fake runtime) versus the original x86 code in Unicorn.

  difftest.py funcs  [--n N] [--seed S] [--cat NAME] [--addr 0x..,...]     random real functions of the three modules
  difftest.py insn   [--n N] [--seed S]                                      synthetic single-instruction tests (flags exact)

Both sides start from the same memory snapshot (module images from the original files with the IAT written like the loader does,
plus pseudo-random heap/stack/TEB arenas derived from the case seed) and the same registers; HLE imports are replaced by identical
record/replay stubs. Compared: eax, edx, popped bytes, st(0) return, and every written page (callee-private stack excluded).
"""
import sys, os, struct, subprocess, random, collections, math, time, json
import numpy as np
from unicorn import *
from unicorn.x86_const import *
import pefile

R = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
GD = os.environ.get('GAME_DIR', os.path.expanduser('~/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/Dishonored')) + '/Binaries/Win32/'
OUT = R + '/private/difftest'
MODS = [('msvcr90.dll', 'Microsoft.VC90.CRT/msvcr90.dll'), ('msvcp90.dll', 'Microsoft.VC90.CRT/msvcp90.dll'), ('Dishonored.exe', 'Dishonored.exe')]
HEAP, HEAP_SZ = 0x10000000, 0x100000
STACK, STACK_SZ = 0x20000000, 0x40000
TEB, TEB_SZ = 0x30000000, 0x1000
SENT = 0xFFFF8000
THUNK_B, THUNK_E = 0xFFF00000, 0xFFFF0000
M64 = (1 << 64) - 1
np.seterr(all='ignore')

def splitmix_np(x):
    x = x + np.uint64(0x9E3779B97F4A7C15)
    z = x
    z = (z ^ (z >> np.uint64(30))) * np.uint64(0xBF58476D1CE4E5B9)
    z = (z ^ (z >> np.uint64(27))) * np.uint64(0x94D049BB133111EB)
    return z ^ (z >> np.uint64(31))

def splitmix(x):
    x = (x + 0x9E3779B97F4A7C15) & M64
    z = x
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
    return z ^ (z >> 31)

def xform(w):
    """pseudo-random u32 words -> pointer-ish / small / raw mix (keep in sync with diffdrv.c xf())"""
    sel = (w >> np.uint32(29)) & np.uint32(7)
    out = w.copy()
    heap = (sel <= 3)
    out[heap] = np.uint32(HEAP) + (w[heap] & np.uint32(0x7FFFC))
    small = (sel == 4)
    out[small] = w[small] & np.uint32(0x3F)
    stk = (sel == 5)
    out[stk] = np.uint32(STACK + 0x10000) + (w[stk] & np.uint32(0xFFFC))
    return out

def arena(base, size, seed):
    addrs = np.arange(base, base + size, 8, dtype=np.uint64)
    r = splitmix_np(np.uint64(seed) ^ (addrs * np.uint64(0xD6E8FEB86659FD93)))
    lo = (r & np.uint64(0xFFFFFFFF)).astype(np.uint32)
    hi = (r >> np.uint64(32)).astype(np.uint32)
    w = np.empty(size // 4, dtype=np.uint32)
    w[0::2] = lo
    w[1::2] = hi
    return xform(w).tobytes()

KTAB = np.array([splitmix(0x1234 + i) | 1 for i in range(512)], dtype=np.uint64)
def page_hash(buf):
    w = np.frombuffer(buf, dtype=np.uint64)
    return int((w * KTAB).sum(dtype=np.uint64))

def stub_mix(seed, ctr, idx):
    return splitmix(seed ^ (ctr << 32) ^ ((idx * 0x9E3779B97F4A7C15) & M64))

def mapped_image(path, base):
    pe = pefile.PE(path)
    size = pe.OPTIONAL_HEADER.SizeOfImage
    img = bytearray(pe.get_memory_mapped_image(ImageBase=base))
    img += b'\0' * ((size + 0xfff & ~0xfff) - len(img))
    return img

class Env:
    def __init__(self, gendir, mods=None, out=OUT, driver=None):
        self.gendir = gendir
        self.out = out
        self.driver = driver or (OUT + '/diffdrv')
        os.makedirs(out + '/imgs', exist_ok=True)
        self.images = []   # (base, size, bytes)
        if mods is None:
            bases = {'msvcr90.dll': 0x78520000, 'msvcp90.dll': 0x78480000, 'Dishonored.exe': 0x400000}
            iat = {}
            for l in open(gendir + '/report/iat.tsv'):
                s, k, v = l.split()
                iat[int(s, 16)] = (int(k), int(v, 16) if k == '0' else int(v))
            for name, rel in MODS:
                b = bases[name]
                img = mapped_image(GD + rel, b)
                for slot, (k, v) in iat.items():
                    if b <= slot < b + len(img):
                        val = v if k == 0 else THUNK_B + v * 16
                        struct.pack_into('<I', img, slot - b, val)
                self.images.append((b, len(img), bytes(img), name))
        else:
            self.images = mods
        with open(out + '/images.txt', 'w') as f:
            for b, sz, data, name in self.images:
                p = out + '/imgs/' + name + '.bin'
                open(p, 'wb').write(data)
                f.write('%x %x %s\n' % (b, sz, p))
        self.uc = None

    def setup_uc(self):
        uc = Uc(UC_ARCH_X86, UC_MODE_32)
        try:
            uc.ctl_set_cpu_model(UC_CPU_X86_CORE2DUO)
        except Exception:
            pass
        for b, sz, data, name in self.images:
            uc.mem_map(b, sz, UC_PROT_ALL)
            uc.mem_write(b, data)
        uc.mem_map(HEAP, HEAP_SZ); uc.mem_map(STACK, STACK_SZ); uc.mem_map(TEB, TEB_SZ)
        uc.mem_map(THUNK_B, THUNK_E - THUNK_B)
        uc.mem_write(THUNK_B, b'\x90' * (THUNK_E - THUNK_B))
        uc.mem_map(0xFFFF0000, 0x10000)
        # flat GDT + an FS descriptor whose base is the TEB
        GDT = 0x40000000
        uc.mem_map(GDT, 0x1000)
        def ent(base, limit, access, flags):
            return struct.pack('<HHBBBB', limit & 0xFFFF, base & 0xFFFF, (base >> 16) & 0xFF, access, ((limit >> 16) & 0xF) | (flags << 4), (base >> 24) & 0xFF)
        gdt = b'\0' * 8 + ent(0, 0xFFFFF, 0x9B, 0xC) + ent(0, 0xFFFFF, 0x93, 0xC) + ent(TEB, 0xFFF, 0x93, 0x4)
        uc.mem_write(GDT, gdt)
        uc.reg_write(UC_X86_REG_GDTR, (0, GDT, len(gdt) - 1, 0))
        uc.reg_write(UC_X86_REG_CR0, 0x13)
        uc.reg_write(UC_X86_REG_CR4, 0x600)
        uc.reg_write(UC_X86_REG_CS, (1 << 3) | 0)
        uc.reg_write(UC_X86_REG_SS, (2 << 3) | 0)
        uc.reg_write(UC_X86_REG_DS, (2 << 3) | 0)
        uc.reg_write(UC_X86_REG_ES, (2 << 3) | 0)
        uc.reg_write(UC_X86_REG_FS, (3 << 3) | 0)
        uc.mem_write(0xFFFF0000, b"\x90" * 0x10000)
        self.uc = uc
        self.written = set()
        self.st = {'ctr': 0, 'seed': 0, 'done': False}

        def on_unmapped(u, access, addr, size, value, ud):
            if addr < 0x10000 or addr >= 0xFFEF0000:
                return False
            pg = addr & ~0xfff
            u.mem_map(pg, 4096)
            u.mem_write(pg, arena(pg, 4096, self.st['seed'] ^ 4))
            self.lazy.add(pg)
            return True
        uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED, on_unmapped)
        self.lazy = set()

        def on_write(u, access, addr, size, value, ud):
            self.written.add(addr >> 12)
            if (addr + size - 1) >> 12 != addr >> 12:
                self.written.add((addr + size - 1) >> 12)
        uc.hook_add(UC_HOOK_MEM_WRITE, on_write)

        def on_thunk(u, addr, size, ud):
            idx = (addr - THUNK_B) // 16
            v = stub_mix(self.st['seed'], self.st['ctr'], idx)
            self.st['ctr'] += 1
            esp = u.reg_read(UC_X86_REG_ESP)
            ret = struct.unpack('<I', u.mem_read(esp, 4))[0]
            pop = 4 * (idx % 5)
            u.reg_write(UC_X86_REG_EAX, v & 0xFFFFFFFF)
            u.reg_write(UC_X86_REG_EDX, v >> 32)
            u.reg_write(UC_X86_REG_ESP, (esp + 4 + pop) & 0xFFFFFFFF)
            u.reg_write(UC_X86_REG_EIP, ret)
        uc.hook_add(UC_HOOK_CODE, on_thunk, begin=THUNK_B, end=THUNK_E)

        def on_sent(u, addr, size, ud):
            self.st['done'] = True
            u.emu_stop()
        uc.hook_add(UC_HOOK_CODE, on_sent, begin=SENT, end=SENT)

    def restore(self):
        uc = self.uc
        for pg in self.written:
            a = pg << 12
            for b, sz, data, name in self.images:
                if b <= a < b + sz:
                    uc.mem_write(a, data[a - b:a - b + 4096])
                    break
        self.written = set()

    def run_case(self, c):
        """returns dict with status/eax/edx/pop/st0/pages"""
        uc = self.uc
        self.restore()
        for pg in self.lazy:
            uc.mem_unmap(pg, 4096)
        self.lazy = set()
        seed = c['seed']
        uc.mem_write(HEAP, arena(HEAP, HEAP_SZ, seed ^ 1))
        uc.mem_write(STACK, arena(STACK, STACK_SZ, seed ^ 2))
        uc.mem_write(TEB, arena(TEB, TEB_SZ, seed ^ 3))
        uc.mem_write(TEB + 0, struct.pack('<I', 0xFFFFFFFF))
        uc.mem_write(TEB + 4, struct.pack('<I', STACK + STACK_SZ))
        uc.mem_write(TEB + 8, struct.pack('<I', STACK))
        uc.mem_write(TEB + 0x18, struct.pack('<I', TEB))
        uc.mem_write(TEB + 0x2C, struct.pack('<I', 0))
        esp = c['esp']
        uc.mem_write(esp, struct.pack('<I', SENT))
        if c.get('args'):
            uc.mem_write(esp + 4, struct.pack('<8I', *c['args']))
        r = c['r']
        for reg, v in zip([UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP], r):
            uc.reg_write(reg, v)
        uc.reg_write(UC_X86_REG_ESP, esp)
        uc.reg_write(UC_X86_REG_EFLAGS, 0x202 | (c.get('eflags', 0) & 0x8D5))
        uc.reg_write(UC_X86_REG_FPCW, 0x27F)
        uc.reg_write(UC_X86_REG_FPSW, 0)
        try:
            uc.reg_write(UC_X86_REG_FPTAG, 0xFFFF)
        except UcError:
            pass
        uc.reg_write(UC_X86_REG_MXCSR, 0x1F80)
        for i in range(8):
            uc.reg_write(UC_X86_REG_XMM0 + i, c.get('xmm', [0] * 8)[i])
        self.written = set()
        self.st.update(ctr=0, seed=seed, done=False)
        res = {'status': 0}
        try:
            uc.emu_start(c['fn'], 0, count=300000)
        except UcError as e:
            res['status'] = 1
            res['err'] = str(e)
            return res
        if not self.st['done']:
            res['status'] = 3
            return res
        res['eax'] = uc.reg_read(UC_X86_REG_EAX)
        res['edx'] = uc.reg_read(UC_X86_REG_EDX)
        res['pop'] = (uc.reg_read(UC_X86_REG_ESP) - esp - 4) & 0xFFFFFFFF
        res['ctr'] = self.st['ctr']
        res['regs'] = [uc.reg_read(x) for x in [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP]]
        top = (uc.reg_read(UC_X86_REG_FPSW) >> 11) & 7
        depth = (8 - top) & 7
        res['fdepth'] = depth
        if depth >= 1:
            m, e = uc.reg_read(UC_X86_REG_FP0 + top)
            res['st0'] = f80_to_float(m, e)
        res['xmm'] = [uc.reg_read(UC_X86_REG_XMM0 + i) for i in range(8)]
        res['cap'] = bytes(uc.mem_read(HEAP + 0x80000, 512))
        pages = {}
        for pg in self.written:
            a = pg << 12
            buf = bytearray(uc.mem_read(a, 4096))
            lo, hi = STACK, esp
            if a + 4096 > lo and a < hi:
                s = max(lo, a) - a
                e = min(hi, a + 4096) - a
                buf[s:e] = b'\0' * (e - s)
            pages[a] = page_hash(bytes(buf))
        res['pages'] = pages
        return res

def f80_to_float(m, e):
    sign = -1.0 if e & 0x8000 else 1.0
    ex = e & 0x7FFF
    if ex == 0x7FFF:
        return float('nan') if (m << 1) & ((1 << 64) - 1) else sign * float('inf')
    if m == 0:
        return sign * 0.0
    try:
        return sign * math.ldexp(m, (ex or 1) - 16383 - 63)
    except OverflowError:
        return sign * float('inf')

CASE_FMT = '<9I4xQ2I8I'
RES_FMT = '<6IxxxxdI'

def write_cases(path, cases):
    with open(path, 'wb') as f:
        f.write(struct.pack('<II', 0x43464944, len(cases)))
        for c in cases:
            f.write(struct.pack(CASE_FMT, c['fn'], c['esp'], *c['r'], c['seed'], 0, 0, *(c.get('args') or [0] * 8)))

def read_results(path, n):
    data = open(path, 'rb').read()
    off = 0
    out = []
    for _ in range(n):
        st, eax, edx, pop, ctr, npg = struct.unpack_from('<6I', data, off)
        st0 = struct.unpack_from('<d', data, off + 24)[0]
        off += 32
        cap = data[off:off + 512]
        off += 512
        pages = {}
        for _ in range(npg):
            pg, _, h = struct.unpack_from('<IIQ', data, off)
            off += 16
            pages[pg] = h
        out.append({'status': st, 'eax': eax, 'edx': edx, 'pop': pop, 'ctr': ctr, 'st0': st0, 'pages': pages, 'cap': cap})
    return out

def ptrish(rng, kinds):
    k = rng.random()
    if k < 0.50: return HEAP + (rng.randrange(0, 0x7F000) & ~3)
    if k < 0.58: return STACK + 0x10000 + (rng.randrange(0, 0x8000) & ~3)
    if k < 0.64: return rng.choice(kinds) if kinds else rng.getrandbits(32)
    if k < 0.80: return rng.randrange(0, 64)
    if k < 0.84: return (-rng.randrange(1, 64)) & 0xFFFFFFFF
    return rng.getrandbits(32)

def make_case(rng, fn, ptr_targets):
    seed = rng.getrandbits(64)
    esp = STACK + 0x20000
    r = [ptrish(rng, ptr_targets) for _ in range(7)]
    args = [ptrish(rng, ptr_targets) for _ in range(8)]
    xmm = [0] * 8
    return {'fn': fn, 'esp': esp, 'r': r, 'args': args, 'seed': seed, 'xmm': xmm}

def close(a, b, rel=1e-9):
    if a != a and b != b: return True
    if a == b: return True
    if math.isinf(a) or math.isinf(b): return False
    return abs(a - b) <= rel * max(abs(a), abs(b), 1e-300)

def compare(c, u, d, check_pages=True):
    if u['status'] != 0:
        return 'skip-unicorn'
    if d['status'] != 0:
        return 'c-status%d' % d['status']
    diffs = []
    if u['eax'] != d['eax']: diffs.append('eax')
    if u['edx'] != d['edx']: diffs.append('edx')
    if u['pop'] != d['pop']: diffs.append('pop')
    if u['ctr'] != d['ctr']: diffs.append('ctr')
    if u.get('fdepth', 0) >= 1 and 'st0' in u:
        if not close(u['st0'], d['st0'], 1e-9): diffs.append('st0')
    if check_pages and u['pages'] != d['pages']:
        diffs.append('mem')
    return ','.join(diffs) if diffs else 'ok'

def run_batch(env, cases, tag='b'):
    cpath = env.out + '/%s_cases.bin' % tag
    rpath = env.out + '/%s_res.bin' % tag
    write_cases(cpath, cases)
    t = time.time()
    p = subprocess.run([env.driver, env.out + '/images.txt', cpath, rpath], capture_output=True, text=True)
    if p.returncode != 0:
        print('diffdrv failed', p.returncode, p.stderr[:500])
        sys.exit(1)
    ds = read_results(rpath, len(cases))
    t_c = time.time() - t
    t = time.time()
    us = [env.run_case(c) for c in cases]
    return us, ds, t_c, time.time() - t

def cmd_funcs(args):
    n = int(arg(args, '--n', 2000)); seed = int(arg(args, '--seed', 1))
    cat = arg(args, '--cat', None)
    gen = OUT + '/gen'
    rows = []
    for l in list(open(gen + '/report/functions.tsv'))[1:]:
        f = l.rstrip('\n').split('\t')
        rows.append({'addr': int(f[0], 16), 'mod': int(f[1]), 'n': int(f[2]), 'cats': f[3].split(',') if f[3] else [], 'unsup': int(f[7]), 'traps': int(f[8])})
    addrs = arg(args, '--addr', None)
    if addrs:
        sel = [r for r in rows if r['addr'] in [int(a, 16) for a in addrs.split(',')]]
        sel = sel * max(1, n // max(1, len(sel)))
    else:
        rng0 = random.Random(seed)
        pool = [r for r in rows if r['unsup'] == 0 and r['n'] >= 3 and r['n'] <= 400 and (cat is None or cat in r['cats'] or (cat == 'int' and not r['cats']))]
        sel = rng0.sample(pool, min(n, len(pool)))
    per = int(arg(args, '--per', 1))
    sel = [r for r in sel for _ in range(per)]
    env = Env(gen)
    env.setup_uc()
    rng = random.Random(seed * 7919 + 1)
    exe = [b for b in env.images if b[3] == 'Dishonored.exe'][0]
    ptr_targets = [0x400000 + 0x1000 + rng.randrange(0, 0xB00000) for _ in range(64)]
    stats = collections.defaultdict(lambda: collections.Counter())
    fails = []
    BATCH = 400
    for i in range(0, len(sel), BATCH):
        chunk = sel[i:i + BATCH]
        cases = [make_case(rng, r['addr'], ptr_targets) for r in chunk]
        us, ds, t_c, t_u = run_batch(env, cases)
        for r, c, u, d in zip(chunk, cases, us, ds):
            res = compare(c, u, d)
            keys = ['all'] + (r['cats'] if r['cats'] else ['int-only'])
            for k in keys:
                stats[k][res if res in ('ok', 'skip-unicorn') else 'FAIL'] += 1
            if res not in ('ok', 'skip-unicorn'):
                fails.append((r['addr'], res, c))
        print('batch %d/%d  c:%.1fs unicorn:%.1fs' % (i // BATCH + 1, (len(sel) + BATCH - 1) // BATCH, t_c, t_u), flush=True)
    print('\ncategory      ok   fail  skipped  (pass rate over executed cases)')
    for k in sorted(stats, key=lambda k: (k != 'all', k)):
        s = stats[k]
        ex = s['ok'] + s['FAIL']
        print('%-10s %6d %6d %7d   %.1f%%' % (k, s['ok'], s['FAIL'], s['skip-unicorn'], 100.0 * s['ok'] / max(1, ex)))
    kinds = collections.Counter(f[1] for f in fails)
    print('\nfailure kinds:', dict(kinds.most_common(12)))
    byf = collections.Counter(f[0] for f in fails)
    print('distinct failing functions:', len(byf))
    json.dump([(hex(a), k) for a, k, _ in fails[:200]], open(OUT + '/fails.json', 'w'))
    import pickle
    pickle.dump(fails, open(OUT + '/fails.pkl', 'wb'))
    for a, k, c in fails[:12]:
        print('  %08x %s' % (a, k))

def arg(a, name, default):
    return a[a.index(name) + 1] if name in a else default

if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'funcs':
        cmd_funcs(sys.argv[2:])
    elif cmd == 'replay':
        import pickle
        fails = pickle.load(open(OUT + '/fails.pkl', 'rb'))
        want = sys.argv[2] if len(sys.argv) > 2 else None
        sel = [f for f in fails if want is None or '%08x' % f[0] == want.lower().replace('0x', '')][:1]
        env = Env(OUT + '/gen'); env.setup_uc()
        for a, k, c in sel:
            u = env.run_case(c)
            write_cases(OUT + '/one_cases.bin', [c])
            os.environ['DIFF_DEBUG'] = '1'
            subprocess.run([OUT + '/diffdrv', OUT + '/images.txt', OUT + '/one_cases.bin', OUT + '/one_res.bin'])
            dd = read_results(OUT + '/one_res.bin', 1)[0]
            print('fn %08x kind %s' % (a, k))
            print('unicorn:', {x: (hex(y) if isinstance(y, int) else y) for x, y in u.items() if x not in ('pages', 'xmm', 'regs')})
            print('c      :', {x: (hex(y) if isinstance(y, int) else y) for x, y in dd.items() if x != 'pages'})
            up, dp = u.get('pages', {}), dd['pages']
            for pg in sorted(set(up) | set(dp)):
                if up.get(pg) != dp.get(pg): print('  page %08x unicorn=%s c=%s' % (pg, up.get(pg), dp.get(pg)))
    elif cmd == 'insn':
        import insn_tests
        insn_tests.main(sys.argv[2:])
