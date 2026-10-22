#!/usr/bin/env python3
"""Independent code-discovery + characterisation pass over a 32-bit MSVC PE (spike 0.1).

Seeds: entry point, exports, call targets, .reloc-derived code pointers (immediates in .text, vtables/function
tables in .rdata/.data), prologue scan of leftover gaps.  Jump tables are recognised from
`jmp [reg*4+disp32]` and the `.reloc` table (a table entry is a relocated dword) so they are *not* turned into
function entries.  Two rounds: round 1 finds entries (all jumps followed); round 2 re-explores with the final entry
set so that `jmp <other entry>` is a tail call and does not merge functions.

Usage: python3 -I discover.py <exe> <outdir>
Writes (outdir): functions.tsv, stats.json, gaps.tsv, indirect_calls.tsv, natives_sites.tsv
Heavy output belongs under private/.
"""
import sys, os, json, struct, collections, array, time
import pefile
from iced_x86 import (Decoder, Instruction, Mnemonic, FlowControl, OpKind, Register, Code, InstructionInfoFactory,
                      CpuidFeature, DecoderOptions)

exe, outdir = sys.argv[1], sys.argv[2]
os.makedirs(outdir, exist_ok=True)
T0 = time.time()
def log(*a):
    print('[%6.1fs]' % (time.time() - T0), *a, flush=True)

data = open(exe, 'rb').read()
pe = pefile.PE(exe, fast_load=True)
pe.parse_data_directories()
BASE = pe.OPTIONAL_HEADER.ImageBase
EP = BASE + pe.OPTIONAL_HEADER.AddressOfEntryPoint
secs = {}
for s in pe.sections:
    n = s.Name.rstrip(b'\0').decode()
    secs[n] = (BASE + s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData, s.Characteristics)
TEXT_VA, TEXT_VSZ, TEXT_RAW, TEXT_RSZ, _ = secs['.text']
TEXT_SZ = min(TEXT_VSZ, TEXT_RSZ)
TEXT_END = TEXT_VA + TEXT_SZ
text = data[TEXT_RAW:TEXT_RAW + TEXT_SZ]
log('image base %x  ep %x  .text %x..%x (%d bytes)' % (BASE, EP, TEXT_VA, TEXT_END, TEXT_SZ))

def sec_of(va):
    for n, (v, vs, ro, rs, ch) in secs.items():
        if v <= va < v + max(vs, rs): return n
    return None
def read32(va):
    for n, (v, vs, ro, rs, ch) in secs.items():
        if v <= va and va + 4 <= v + rs:
            o = ro + (va - v); return struct.unpack_from('<I', data, o)[0]
    return None
def in_text(va): return TEXT_VA <= va < TEXT_END

# ---------------------------------------------------------------- relocations
relocs = []        # VAs of HIGHLOW relocations
for ent in pe.DIRECTORY_ENTRY_BASERELOC:
    for r in ent.entries:
        if r.type == 3: relocs.append(r.rva + BASE)
relocset = set(relocs)
log('relocs:', len(relocs))
IAT = {}   # va of IAT slot -> "dll!name"
for ent in pe.DIRECTORY_ENTRY_IMPORT:
    dll = ent.dll.decode().lower()
    for imp in ent.imports:
        IAT[imp.address] = '%s!%s' % (dll, imp.name.decode() if imp.name else 'ord%d' % imp.ordinal)
for ent in getattr(pe, 'DIRECTORY_ENTRY_DELAY_IMPORT', []):
    for imp in ent.imports:
        IAT[imp.address] = '%s!%s(delay)' % (ent.dll.decode().lower(), imp.name.decode() if imp.name else '?')
NORETURN_IMPORT = {n for n in IAT.values() if any(n.endswith('!' + x) for x in
                   ('_CxxThrowException', 'exit', '_exit', 'abort', 'longjmp', '_invoke_watson', '_amsg_exit', 'ExitProcess',
                    '?terminate@@YAXXZ', '_purecall'))}
reloc_by_sec = collections.Counter(sec_of(v) for v in relocs)
log('relocs by section:', dict(reloc_by_sec))

# reloc'd dwords that point into .text, split by the section holding the reloc site
text_ptrs = {'.text': [], '.rdata': [], '.data': [], 'other': []}
for rva in relocs:
    v = read32(rva)
    if v is not None and in_text(v):
        s = sec_of(rva)
        text_ptrs.get(s, text_ptrs['other']).append((rva, v))
log('reloc-> .text pointers: ' + ', '.join('%s=%d' % (k, len(v)) for k, v in text_ptrs.items()))

# ---------------------------------------------------------------- tables
OKINDS = OpKind
info_factory = InstructionInfoFactory()
MN = {getattr(Mnemonic, n): n for n in dir(Mnemonic) if n.isupper()}
RET_MN = {Mnemonic.RET}
dec = Decoder(32, text, ip=TEXT_VA)
ins = Instruction()

def decode_at(va, instr=ins):
    dec.position = va - TEXT_VA
    dec.ip = va
    dec.decode_out(instr)
    return instr

# ---------------------------------------------------------------- jump-table helpers
def read_jump_table(tbl_va, max_entries=4096):
    """Consecutive relocated dwords that point into .text, starting at tbl_va."""
    out = []
    va = tbl_va
    while len(out) < max_entries and va in relocset:
        t = read32(va)
        if t is None or not in_text(t): break
        out.append(t); va += 4
    return out

# ---------------------------------------------------------------- exploration
class Func:
    __slots__ = ('entry', 'ranges', 'ninstr', 'calls', 'tails', 'jts', 'flags', 'size')

def explore(entry, entries, owner, collect=None, stats=None, jt_sites=None, round1=False):
    """Explore one function from `entry`. Returns dict of results. owner: array('i') (round 2 only)."""
    stack = [entry]
    seen = set()
    new_calls = []
    jmp_targets = []
    ranges_off = []  # (off_start, off_end) per straight-line run
    ninstr = 0
    in_fn = set()
    while stack:
        va = stack.pop()
        if va in seen or not in_text(va): continue
        run_start = va
        while True:
            if not in_text(va): break
            off = va - TEXT_VA
            if va in seen: break
            if not round1:
                o = owner[off]
                if o != 0 and o != entry:
                    # runs into another function's code (fallthrough or shared block)
                    if stats is not None: stats['shared_entries'][(entry, va)] += 1
                    break
            i = decode_at(va)
            if i.is_invalid:
                if stats is not None: stats['invalid'].append(va)
                break
            seen.add(va)
            ninstr += 1
            l = i.len
            if not round1:
                owner[off] = entry
            if collect is not None: collect(i, entry)
            mn = i.mnemonic
            fc = i.flow_control
            nxt = va + l
            if fc == FlowControl.NEXT:
                va = nxt; continue
            if fc == FlowControl.CALL:
                if i.op0_kind == OpKind.NEAR_BRANCH32:
                    t = i.near_branch_target
                    if t != nxt and in_text(t): new_calls.append(t)
                else:
                    # indirect; noreturn import call?
                    if i.op0_kind == OpKind.MEMORY and i.memory_base == Register.NONE and i.memory_index == Register.NONE:
                        nm = IAT.get(i.memory_displacement & 0xffffffff)
                        if nm in NORETURN_IMPORT:
                            break
                va = nxt; continue
            if fc == FlowControl.CONDITIONAL_BRANCH:
                t = i.near_branch_target
                jmp_targets.append((va, t, True))
                if round1 or t == entry or t not in entries:
                    stack.append(t)
                va = nxt; continue
            if fc == FlowControl.UNCONDITIONAL_BRANCH:
                t = i.near_branch_target
                jmp_targets.append((va, t, False))
                if round1 or t == entry or t not in entries:
                    stack.append(t)
                break
            if fc == FlowControl.INDIRECT_BRANCH:
                # jump table?
                if i.op0_kind == OpKind.MEMORY and i.memory_index != Register.NONE and i.memory_index_scale == 4 \
                        and i.memory_base == Register.NONE:
                    tbl = i.memory_displacement & 0xffffffff
                    ents = read_jump_table(tbl)
                    if ents:
                        if jt_sites is not None: jt_sites.append((va, tbl, ents))
                        for t in ents: stack.append(t)
                    elif jt_sites is not None:
                        jt_sites.append((va, tbl, []))
                elif jt_sites is not None:
                    jt_sites.append((va, None, i.op0_kind))
                break
            if fc in (FlowControl.RETURN, FlowControl.INTERRUPT, FlowControl.EXCEPTION):
                if mn == Mnemonic.INT and fc == FlowControl.INTERRUPT and i.immediate8 != 3 and stats is not None:
                    stats['int_other'].append(va)
                break
            # XBEGIN, etc.
            va = nxt
    return seen, new_calls, jmp_targets

# ---------------------------------------------------------------- seed collection
exports = []
if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
    for s in pe.DIRECTORY_ENTRY_EXPORT.symbols:
        if s.address and in_text(BASE + s.address): exports.append(BASE + s.address)
log('exports in .text:', len(exports))

def seeds_from_relocs(exclude):
    ent = set()
    for k in ('.text', '.rdata', '.data', 'other'):
        for rva, v in text_ptrs[k]:
            if v in exclude: continue
            if k == '.rdata' and rva in jt_data_va: continue
            ent.add(v)
    return ent

jt_data_va = set()   # VAs of dword slots known to be jump table entries
jt_targets = set()
jt_sites_all = []

entries = set([EP]) | set(exports)
# reloc-derived seeds are added after the first descent has identified jump tables; but jump tables are
# only found by descending. Iterate: seeds -> explore -> find jump tables -> recompute reloc seeds.
entries |= seeds_from_relocs(set())
log('initial entries:', len(entries))

def run_round1(entries):
    work = sorted(entries)
    done = set()
    allcode = set()
    jts = []
    while work:
        e = work.pop()
        if e in done: continue
        done.add(e)
        seen, calls, jt = explore(e, entries, None, jt_sites=jts, round1=True)
        for c in calls:
            if c not in entries:
                entries.add(c); work.append(c)
        allcode |= seen
    return done, allcode, jts

for it in range(4):
    done, allcode, jts = run_round1(entries)
    # jump tables found -> remove their entries from the seed set (they are intra-function)
    new_jt_slots = set(); new_jt_targets = set()
    for va, tbl, ents in jts:
        if tbl is not None and ents:
            for k in range(len(ents)): new_jt_slots.add(tbl + 4 * k)
            new_jt_targets.update(ents)
    before = len(jt_data_va)
    jt_data_va |= new_jt_slots
    jt_targets |= new_jt_targets
    # a jump-table target that is *also* a genuine entry (called / vtable) stays an entry; we only drop the ones that
    # were seeded solely through the table slots
    solid = set()
    solid.add(EP); solid.update(exports)
    # recompute: entries = solid + call targets (re-found) + reloc seeds excluding jt slots
    # call targets are re-discovered by the next round1; keep those already known unless they are jt-only
    jt_only = {t for t in jt_targets if t in entries}
    # a target is jt-only if no reloc outside jt slots points at it and no call targets it.
    outside = set()
    for k in ('.text', '.rdata', '.data', 'other'):
        for rva, v in text_ptrs[k]:
            if rva not in jt_data_va: outside.add(v)
    called = set()
    # call targets collected during this iteration: gather by re-scan of seen code is expensive; approximate by round1 entries
    # that are not in jt_targets or are in outside
    entries = {e for e in entries if (e not in jt_targets) or (e in outside) or e == EP or e in exports}
    entries |= outside | {EP} | set(exports)
    log('iter %d: entries=%d code_instrs=%d jt_slots=%d jt_targets=%d' % (it, len(entries), len(allcode), len(jt_data_va), len(jt_targets)))
    if len(jt_data_va) == before and it > 0: break

# ---------------------------------------------------------------- prologue scan of gaps
covered = bytearray(TEXT_SZ)          # 1 where an instruction byte is covered
def mark(seen):
    for va in seen:
        i = decode_at(va); o = va - TEXT_VA
        covered[o:o + i.len] = b'\x01' * i.len
for va in allcode:
    pass

def fill_covered(allcode):
    for va in allcode:
        o = va - TEXT_VA
        # length: decode lazily; faster -> store lengths while exploring (re-decode here, simple)
        decode_at(va)
        covered[o:o + ins.len] = b'\x01' * ins.len

fill_covered(allcode)

def mark_jt_data(jts):
    d = bytearray(TEXT_SZ)
    for va, tbl, ents in jts:
        if tbl is not None and in_text(tbl):
            n = len(ents) * 4
            o = tbl - TEXT_VA
            d[o:o + n] = b'\x02' * n
    return d

PROLOGUES = (b'\x55\x8b\xec', b'\x8b\xff\x55\x8b\xec', b'\x83\xec', b'\x6a\xff\x68', b'\x6a\x00\x68', b'\x56\x57', b'\x53\x56',
             b'\x51\x53', b'\x51\x56', b'\x81\xec', b'\x8b\x44\x24', b'\x8b\x4c\x24', b'\x53', b'\x56', b'\x57', b'\x51',
             b'\x55', b'\x33\xc0', b'\x32\xc0', b'\xb8', b'\x6a', b'\xf6\x44', b'\x8b\x01', b'\x8b\x41', b'\xd9', b'\xf3', b'\x0f')
def gap_scan(allcode, covered, jt_data):
    """Find uncovered runs; for each, skip padding, try to decode from first non-pad byte as a function."""
    new = []
    n = TEXT_SZ
    o = 0
    gaps = []
    while o < n:
        if covered[o] or jt_data[o]:
            o += 1; continue
        s = o
        while o < n and not covered[o] and not jt_data[o]: o += 1
        gaps.append((s, o))
    return gaps

def is_pad(b): return b in (0xcc, 0x90, 0x00)

for gap_iter in range(6):
    jt_data = mark_jt_data(jts)
    gaps = gap_scan(allcode, covered, jt_data)
    added = 0
    for s, e in gaps:
        p = s
        while p < e:
            if is_pad(text[p]): p += 1; continue
            q = decode_at(TEXT_VA + p)
            if q.is_invalid: break
            nop_like = q.mnemonic == Mnemonic.NOP or (q.mnemonic == Mnemonic.LEA and q.op0_register == q.memory_base and q.memory_index == Register.NONE and (q.memory_displacement & 0xffffffff) == 0) \
                or (q.mnemonic == Mnemonic.MOV and q.op0_kind == OpKind.REGISTER and q.op1_kind == OpKind.REGISTER and q.op0_register == q.op1_register and text[p:p+5] != b'\x8b\xff\x55\x8b\xec')
            if nop_like: p += q.len; continue
            break
        if p >= e: continue
        va = TEXT_VA + p
        if va in entries: continue
        if not text[p:].startswith(PROLOGUES): continue
        # require it decodes to a terminator within a plausible span without invalid opcodes
        seen, calls, _ = explore(va, entries | {va}, None, round1=True)
        if len(seen) >= 3:
            # accept only if all within gap or at least no overlap with covered
            ok = True
            for a in seen:
                if covered[a - TEXT_VA]: ok = False; break
            if ok:
                entries.add(va); added += 1
                for a in seen:
                    decode_at(a); covered[a - TEXT_VA:a - TEXT_VA + ins.len] = b'\x01' * ins.len
                allcode |= seen
                for c in calls:
                    if c not in entries and not covered[c - TEXT_VA]:
                        entries.add(c)
    # new entries from calls inside gap functions
    d2, ac2, j2 = run_round1(entries)
    for a in ac2:
        if a not in allcode:
            allcode.add(a)
            decode_at(a); covered[a - TEXT_VA:a - TEXT_VA + ins.len] = b'\x01' * ins.len
    jts.extend(j2)
    log('gap pass %d: gaps=%d added=%d code_instrs=%d' % (gap_iter, len(gaps), added, len(allcode)))
    if added == 0 and len(ac2) == 0: break
    if added == 0: break

# ---------------------------------------------------------------- round 2: final function partition + statistics
entries_final = sorted(e for e in entries if in_text(e))
entries_set = set(entries_final)
owner = array.array('i', [0]) * TEXT_SZ      # entry VA as int32 (VA fits: < 2^31)
stats = {'shared_entries': collections.Counter(), 'invalid': [], 'int_other': []}
mn_hist = collections.Counter()
feat_cache = {}
feat_hist = collections.Counter()
feat_example = {}
indirect_calls = []            # (site, kind, detail)
ind_kind = collections.Counter()
imp_calls = collections.Counter()
fs_sites = collections.defaultdict(list)
fs0_write_funcs = set(); fs0_read_funcs = set(); fs_other = collections.Counter()
awkward = collections.defaultdict(list)
_AWK_NAMES = {'CPUID': 'cpuid', 'RDTSC': 'rdtsc', 'RDTSCP': 'rdtscp', 'INT': 'int', 'INT3': 'int3', 'PUSHFD': 'pushfd', 'POPFD': 'popfd',
 'PUSHF': 'pushf', 'POPF': 'popf', 'LAHF': 'lahf', 'SAHF': 'sahf', 'FLDCW': 'fldcw', 'FNSTCW': 'fnstcw', 'FSTCW': 'fstcw',
 'FNINIT': 'fninit', 'FINIT': 'finit', 'FNSTSW': 'fnstsw', 'FNSAVE': 'fnsave', 'FRSTOR': 'frstor', 'FXSAVE': 'fxsave',
 'FXRSTOR': 'fxrstor', 'LDMXCSR': 'ldmxcsr', 'STMXCSR': 'stmxcsr', 'EMMS': 'emms', 'XGETBV': 'xgetbv', 'SYSENTER': 'sysenter',
 'SYSCALL': 'syscall', 'IN': 'in', 'OUT': 'out', 'HLT': 'hlt', 'UD2': 'ud2', 'CMPXCHG': 'cmpxchg', 'CMPXCHG8B': 'cmpxchg8b',
 'XADD': 'xadd', 'PAUSE': 'pause', 'MFENCE': 'mfence', 'LFENCE': 'lfence', 'SFENCE': 'sfence', 'PREFETCHT0': 'prefetch',
 'PREFETCHNTA': 'prefetch', 'PREFETCHW': 'prefetch', 'PREFETCHT1': 'prefetch', 'PREFETCHT2': 'prefetch', 'PREFETCH': 'prefetch',
 'BSWAP': 'bswap', 'MOVNTQ': 'movntq', 'MOVNTPS': 'movntps', 'MOVNTDQ': 'movntdq', 'MOVNTI': 'movnti', 'ENTER': 'enter',
 'XLAT': 'xlat', 'XLATB': 'xlat', 'WAIT': 'wait', 'FWAIT': 'fwait', 'CLD': 'cld', 'STD': 'std', 'DAA': 'daa', 'AAM': 'aam',
 'BOUND': 'bound', 'INTO': 'into', 'ARPL': 'arpl', 'LES': 'les', 'LDS': 'lds', 'FEMMS': '3dnow femms',
 'PREFETCHWT1': 'prefetch', 'LOCK': 'lock'}
AWK = {getattr(Mnemonic, k): v for k, v in _AWK_NAMES.items() if hasattr(Mnemonic, k)}
STR_MN = {Mnemonic.MOVSB, Mnemonic.MOVSW, Mnemonic.MOVSD, Mnemonic.STOSB, Mnemonic.STOSW, Mnemonic.STOSD, Mnemonic.LODSB, Mnemonic.LODSW, Mnemonic.LODSD,
          Mnemonic.SCASB, Mnemonic.SCASW, Mnemonic.SCASD, Mnemonic.CMPSB, Mnemonic.CMPSW, Mnemonic.CMPSD}
rep_hist = collections.Counter()
segs = collections.Counter()
lock_cnt = 0
xchg_mem = 0
natives_sites = []
call_idx_table = collections.defaultdict(list)    # disp -> call sites [reg*4+disp]
x87_cnt = 0; sse_cnt = 0; mmx_cnt = 0; d3dnow = 0
ext_class = collections.Counter()

_FN = {'SSE': 'SSE', 'SSE2': 'SSE2', 'SSE3': 'SSE3', 'SSSE3': 'SSSE3', 'SSE4_1': 'SSE4.1', 'SSE4_2': 'SSE4.2', 'MMX': 'MMX', 'FPU': 'x87',
       'D3NOW': '3DNow', 'D3NOWEXT': '3DNowExt', 'AVX': 'AVX', 'AVX2': 'AVX2', 'CMOV': 'CMOV', 'CX8': 'CX8', 'POPCNT': 'POPCNT',
       'LZCNT': 'LZCNT', 'BMI1': 'BMI1', 'FXSR': 'FXSR', 'SSE4A': 'SSE4A', 'PCLMULQDQ': 'PCLMUL', 'AES': 'AES', 'TSC': 'TSC', 'MONITOR': 'MONITOR',
       'CMPXCHG8B': 'CX8', 'MMX_PLUS' if False else 'MULTIBYTENOP': 'multibyte-nop', 'SYSENTER': 'sysenter'}
SSE_FEATS = {getattr(CpuidFeature, k): v for k, v in _FN.items() if hasattr(CpuidFeature, k)}
def feats(i):
    code = i.code
    f = feat_cache.get(code)
    if f is None:
        f = tuple(sorted({SSE_FEATS.get(x, None) for x in i.cpuid_features()} - {None}))
        feat_cache[code] = f
    return f

def collect(i, entry):
    global lock_cnt, xchg_mem
    mn = i.mnemonic
    mn_hist[mn] += 1
    f = feats(i)
    if f:
        for x in f: feat_hist[x] += 1
        ext = f[0]
        if len(feat_example) < 400 and (i.code, ) not in feat_example:
            pass
    else:
        feat_hist['base'] += 1
    a = AWK.get(mn)
    if a:
        awkward[a].append(i.ip)
    if i.has_lock_prefix:
        lock_cnt += 1
    if i.has_rep_prefix or i.has_repe_prefix or i.has_repne_prefix:
        if mn in STR_MN:
            rep_hist[(MN[mn], 'rep' if i.has_rep_prefix or i.has_repe_prefix else 'repne')] += 1
    elif mn in STR_MN:
        rep_hist[(MN[mn], 'norep')] += 1
    if mn == Mnemonic.XCHG and i.op0_kind == OpKind.MEMORY or (mn == Mnemonic.XCHG and i.op1_kind == OpKind.MEMORY):
        xchg_mem += 1
        awkward['xchg_mem(implicit lock)'].append(i.ip)
    # segment overrides
    if i.segment_prefix != Register.NONE:
        segs[(i.segment_prefix, )] += 1
        for k in range(i.op_count):
            if i.op_kind(k) == OpKind.MEMORY and i.memory_segment == Register.FS:
                d = i.memory_displacement & 0xffffffff
                if i.memory_base == Register.NONE and i.memory_index == Register.NONE:
                    if d == 0:
                        if k == 0 and mn in (Mnemonic.MOV, Mnemonic.PUSH, Mnemonic.POP, Mnemonic.ADD, Mnemonic.XCHG): fs0_write_funcs.add(entry)
                        else: fs0_read_funcs.add(entry)
                        fs_sites['fs:[0]'].append(i.ip)
                        if k == 0 and mn == Mnemonic.MOV: fs_sites['fs:[0]=w'].append(i.ip)
                    else:
                        fs_sites['fs:[%#x]' % d].append(i.ip)
                else:
                    fs_sites['fs:[reg+..]'].append(i.ip)
    fc = i.flow_control
    if fc == FlowControl.CALL or fc == FlowControl.INDIRECT_CALL:
        k = i.op0_kind
        if k == OpKind.NEAR_BRANCH32:
            ind_kind['call direct'] += 1
        elif k == OpKind.REGISTER:
            ind_kind['call reg'] += 1; indirect_calls.append((i.ip, 'reg', str(i.op0_register)))
        elif k == OpKind.MEMORY:
            if i.memory_base == Register.NONE and i.memory_index == Register.NONE:
                d = i.memory_displacement & 0xffffffff
                nm = IAT.get(d)
                if nm:
                    ind_kind['call [IAT]'] += 1; imp_calls[nm] += 1
                else:
                    ind_kind['call [abs data]'] += 1; indirect_calls.append((i.ip, 'abs', hex(d)))
            elif i.memory_index != Register.NONE and i.memory_base == Register.NONE and i.memory_index_scale == 4:
                ind_kind['call [idx*4+tbl]'] += 1
                call_idx_table[i.memory_displacement & 0xffffffff].append(i.ip)
                indirect_calls.append((i.ip, 'idx4tbl', hex(i.memory_displacement & 0xffffffff)))
            elif i.memory_base == Register.ESP:
                ind_kind['call [esp+..]'] += 1
            else:
                disp = i.memory_displacement & 0xffffffff
                if disp & 0x80000000: disp -= 1 << 32
                ind_kind['call [reg+disp] (vtable-like)'] += 1
                indirect_calls.append((i.ip, 'mem', '[%s+%d]' % (str(i.memory_base), disp)))
    elif fc == FlowControl.INDIRECT_BRANCH:
        k = i.op0_kind
        if k == OpKind.REGISTER:
            ind_kind['jmp reg'] += 1
        elif k == OpKind.MEMORY:
            if i.memory_base == Register.NONE and i.memory_index == Register.NONE:
                nm = IAT.get(i.memory_displacement & 0xffffffff)
                ind_kind['jmp [IAT] (import thunk/tail)' if nm else 'jmp [abs]'] += 1
                if nm: imp_calls['jmp:' + nm] += 1
            elif i.memory_index != Register.NONE and i.memory_index_scale == 4:
                ind_kind['jmp [idx*4+tbl] (switch)'] += 1
            else:
                ind_kind['jmp [reg+disp]'] += 1

jt_sites2 = []
t_r2 = time.time()
funcs = {}
for e in entries_final:
    if owner[e - TEXT_VA] != 0 and owner[e - TEXT_VA] != e:
        # entry lies inside code already owned (can happen for jump-table-only targets): skip as separate function
        continue
    seen, calls, jt = explore(e, entries_set, owner, collect=collect, stats=stats, jt_sites=jt_sites2)
    funcs[e] = (seen, jt)
log('round2 done: %d functions, %.1fs' % (len(funcs), time.time() - t_r2))

# --------------------------------------------------------------- function table + coverage
cov2 = bytearray(TEXT_SZ)
rows = []
tail_calls = 0; tail_jcc = 0
tail_by_func = collections.Counter()
lens = {}
for e, (seen, jt) in funcs.items():
    lo = e; hi = e; nb = 0
    for va in seen:
        decode_at(va); l = ins.len
        o = va - TEXT_VA
        cov2[o:o + l] = b'\x01' * l
        nb += l
        if va < lo: lo = va
        if va + l > hi: hi = va + l
    for (va, t, cond) in jt:
        if t != e and t in entries_set and t not in seen:
            if cond: tail_jcc += 1
            else: tail_calls += 1
            tail_by_func[e] += 1
    rows.append((e, lo, hi, nb, len(seen)))
rows.sort()

# jump-table data and padding
jt_data2 = bytearray(TEXT_SZ)
jt_total_entries = 0; jt_count = 0; jt_unknown_sites = 0
jt_ent_hist = collections.Counter()
idx_tables = 0
jt_seen = set()
for va, tbl, ents in jt_sites2:
    if tbl is None: jt_unknown_sites += 1; continue
    if tbl in jt_seen: continue
    jt_seen.add(tbl)
    jt_count += 1; jt_total_entries += len(ents)
    jt_ent_hist[min(len(ents) // 8 * 8, 64)] += 1
    if in_text(tbl):
        o = tbl - TEXT_VA; n = len(ents) * 4
        jt_data2[o:o + n] = b'\x02' * n
# byte index tables: `movzx r32, byte [reg + disp32]` with relocated disp32 pointing into .text, shortly before a jmp [idx*4+tbl]
idx_tbl_bytes = 0
for e, (seen, jt) in funcs.items():
    pass

def rescan_idx_tables():
    global idx_tbl_bytes, idx_tables
    # scan each function's instructions for movzx r, byte [reg+disp] where disp is a .text address and reloc'd
    for e, (seen, jt) in funcs.items():
        for va in seen:
            i = decode_at(va)
            if i.mnemonic == Mnemonic.MOVZX and i.op1_kind == OpKind.MEMORY and i.memory_size == 1 and i.memory_base != Register.NONE \
                    and i.memory_index == Register.NONE:
                d = i.memory_displacement & 0xffffffff
                if in_text(d):
                    # find immediately-following jmp [reg*4+tbl] within 3 instructions and its table size
                    p = va + i.len; ok = False; ents = None
                    for _ in range(3):
                        j = decode_at(p)
                        if j.flow_control == FlowControl.INDIRECT_BRANCH and j.op0_kind == OpKind.MEMORY and j.memory_index_scale == 4:
                            ok = True; break
                        p += j.len
                    if not ok: continue
                    # length of index table = (max idx)+1; scan bytes after the dword table or at d; use cmp imm before movzx -> look back
                    # fallback: bound by next non-covered run end
                    o = d - TEXT_VA
                    n = 0
                    # take bytes while not covered and not jt data and < 256
                    while o + n < TEXT_SZ and n < 256 and not cov2[o + n] and not jt_data2[o + n]: n += 1
                    # trim trailing alignment padding (cc/90)
                    idx_tables += 1
                    jt_data2[o:o + n] = b'\x03' * n
                    idx_tbl_bytes += n
rescan_idx_tables()

pad = bytearray(TEXT_SZ)
# padding: runs of cc/90/00 or nop-ish between code
o = 0
while o < TEXT_SZ:
    if not cov2[o] and not jt_data2[o] and text[o] in (0xcc, 0x90, 0x00):
        pad[o] = 1
    o += 1
# multi-byte nops that are alignment fill (lea esi,[esi+0] etc.) are decoded as code by explore only if reached; count leftovers
cov_code = sum(cov2)
cov_jt = sum(1 for o in range(TEXT_SZ) if jt_data2[o] and not cov2[o])
pad = bytearray(1 if (pad[o] and not cov2[o] and not jt_data2[o]) else 0 for o in range(TEXT_SZ))
cov_pad = sum(pad)
unk = sum(1 for o in range(TEXT_SZ) if not (cov2[o] or jt_data2[o] or pad[o]))
log('coverage: code=%d jt/idx data=%d pad=%d unknown=%d of %d' % (cov_code, cov_jt, cov_pad, unk, TEXT_SZ))

# unknown gaps listing
gaps_out = []
o = 0
while o < TEXT_SZ:
    if cov2[o] or jt_data2[o] or pad[o]: o += 1; continue
    s = o
    while o < TEXT_SZ and not (cov2[o] or jt_data2[o] or pad[o]): o += 1
    gaps_out.append((TEXT_VA + s, o - s))
gaps_out.sort(key=lambda g: -g[1])

# --------------------------------------------------------------- per-function characterisation
fa = info_factory
RET_IMM = collections.Counter()
conv = collections.Counter()
first_pat = collections.Counter()
nonstd_examples = []
frame_kind = collections.Counter()
fsz_hist = collections.Counter()
thiscall_like = 0
seh_helpers = collections.Counter()   # call target -> count for `push imm; push imm; call X` entry shape
eh_stub_funcs = 0
for (e, lo, hi, nb, n) in rows:
    b = text[e - TEXT_VA:e - TEXT_VA + 12]
    # prologue kind
    if b.startswith(b'\x55\x8b\xec') or b.startswith(b'\x8b\xff\x55\x8b\xec'): k = 'push ebp; mov ebp,esp'
    elif b.startswith(b'\x6a\xff\x68') or (b[0:1] == b'\x6a' and b[2:3] == b'\x68' and b[7:8] == b'\xe8'): k = 'SEH/EH frame setup (push N; push handler; call prolog)'
    elif b.startswith(b'\x6a') and b[2:3] == b'\x68': k = 'SEH/EH frame setup (push N; push handler; ...)'
    elif b.startswith(b'\x83\xec') or b.startswith(b'\x81\xec'): k = 'sub esp (frameless)'
    elif b[0] in (0x51, 0x53, 0x56, 0x57, 0x55): k = 'push reg (frameless)'
    elif b[0:1] == b'\xe9' or b[0:2] == b'\xff\x25': k = 'jmp thunk'
    elif b[0:1] in (b'\xc3', b'\xc2'): k = 'ret (empty/stub)'
    elif b[0:1] == b'\x8b' : k = 'mov first (leaf/ecx-this)'
    elif b[0:1] == b'\xb8': k = 'mov eax,imm (probe/ret const)'
    else: k = 'other'
    frame_kind[k] += 1
    if k == 'other' and len(nonstd_examples) < 50: nonstd_examples.append((e, b.hex()))
    fsz_hist[min(nb // 64 * 64, 1024)] += 1

# ret kinds and thiscall-ish detection (sample entry block)
sample = 0
for (e, lo, hi, nb, n) in rows:
    seen, jt = funcs[e]
    retn = None
    for va in seen:
        i = decode_at(va)
        if i.mnemonic == Mnemonic.RET:
            retn = i.immediate16 if i.op_count else 0
            RET_IMM[retn] += 1
    # entry block liveness for ecx/edx
    va = e; ecx_in = edx_in = False; wr = set(); steps = 0
    while steps < 24:
        i = decode_at(va); steps += 1
        if i.flow_control in (FlowControl.CALL, FlowControl.INDIRECT_CALL, FlowControl.RETURN) and steps > 1:
            pass
        info = fa.info(i)
        for ru in info.used_registers():
            r = ru.register
            if ru.access in (1, 3, 4, 5):   # read / cond read / read-write
                pass
        rd = [(u.register, u.access) for u in info.used_registers()]
        for reg, acc in rd:
            full = reg
            if reg in (Register.ECX, Register.CX, Register.CL, Register.CH):
                if acc in (1, 3, 4, 5) and Register.ECX not in wr and not (i.mnemonic in (Mnemonic.XOR, Mnemonic.SUB) and i.op0_register == i.op1_register):
                    ecx_in = True
            if reg in (Register.EDX, Register.DX, Register.DL, Register.DH):
                if acc in (1, 3, 4, 5) and Register.EDX not in wr and not (i.mnemonic in (Mnemonic.XOR, Mnemonic.SUB) and i.op0_register == i.op1_register):
                    edx_in = True
        for u in info.used_registers():
            if u.access in (2, 3) or u.access == 5 and False:
                if u.register in (Register.ECX, Register.CX, Register.CL): wr.add(Register.ECX)
                if u.register in (Register.EDX, Register.DX, Register.DL): wr.add(Register.EDX)
        if i.flow_control in (FlowControl.CALL, FlowControl.INDIRECT_CALL, FlowControl.RETURN, FlowControl.UNCONDITIONAL_BRANCH, FlowControl.INDIRECT_BRANCH):
            break
        va += i.len
    if retn is None: c = 'noreturn/tail-only'
    elif retn == 0: c = 'cdecl-like (ret)' + (' +ecx_in' if ecx_in else '') + (' +edx_in' if edx_in else '')
    else: c = 'callee-pops (ret N)' + (' thiscall-like(ecx_in)' if ecx_in else ' stdcall-like') + (' +edx(fastcall?)' if edx_in and ecx_in else '')
    conv[c] += 1

# ---------------------------------------------------------------- EH / SEH structures
# C++ EH: FuncInfo magic 0x19930522 (+ older 0x19930520/21), scan .rdata dword-aligned
eh_infos = []
rd = secs['.rdata']; rdata_bytes = data[rd[2]:rd[2] + rd[3]]
for magic in (0x19930522, 0x19930521, 0x19930520):
    pat = struct.pack('<I', magic)
    pos = 0
    while True:
        p = rdata_bytes.find(pat, pos)
        if p < 0: break
        pos = p + 1
        if p % 4: continue
        va = rd[0] + p
        f = struct.unpack_from('<IiIiIiIiII', rdata_bytes, p) if p + 40 <= len(rdata_bytes) else None
        if not f: continue
        _, maxState, pUnw, nTry, pTry, nIP, pIP, uh, pES, flags = f
        if not (0 <= maxState < 4096 and 0 <= nTry < 512): continue
        if maxState > 0 and pUnw not in relocset and (va + 8) not in relocset: continue
        eh_infos.append((va, maxState, nTry, nIP))
n_eh = len(eh_infos)
n_try = sum(1 for x in eh_infos if x[2] > 0)
n_try_blocks = sum(x[2] for x in eh_infos)
# SEH-style scope tables: look at stubs `mov eax, offset X; jmp __CxxFrameHandler3` count: calls to jmp [__CxxFrameHandler3]
cxx_fh_slot = [k for k, v in IAT.items() if v.endswith('!__CxxFrameHandler3')]
cxx_stubs = 0
if cxx_fh_slot:
    sl = cxx_fh_slot[0]
    # stubs: B8 <imm32=FuncInfo> E9/FF25 ... ; count jmp [sl]
    needle = b'\xff\x25' + struct.pack('<I', sl)
    cxx_stubs = text.count(needle)
exh3 = [k for k, v in IAT.items() if v.endswith('!_except_handler3')]
exh4 = [k for k, v in IAT.items() if v.endswith('!_except_handler4_common')]
setjmp_slots = [k for k, v in IAT.items() if v.endswith('!_setjmp3')]
longjmp_slots = [k for k, v in IAT.items() if v.endswith('!longjmp')]
setjmp_sites = []; longjmp_sites = []
raise_slots = [k for k, v in IAT.items() if v.endswith('!_CxxThrowException')]
throw_sites = 0
for sl, lst, name in ((setjmp_slots, setjmp_sites, 'setjmp'), (longjmp_slots, longjmp_sites, 'longjmp'), (raise_slots, None, 'throw')):
    for s in sl:
        needle = b'\xff\x15' + struct.pack('<I', s)
        p = 0
        while True:
            p = text.find(needle, p)
            if p < 0: break
            if lst is not None: lst.append(TEXT_VA + p)
            else: throw_sites += 1
            p += 1
# who references the _except_handler3/4 IAT slot (the `jmp [IAT]` stubs) :
exh_stub_cnt = {}
for name, sls in (('_except_handler3', exh3), ('_except_handler4_common', exh4)):
    c = 0
    for s in sls:
        c += text.count(b'\xff\x25' + struct.pack('<I', s)) + text.count(b'\xff\x15' + struct.pack('<I', s))
        # push/ mov via address (rare)
    exh_stub_cnt[name] = c

# helper functions that set up the chain: any function with mov fs:[0] write that is called from many sites
helper_cands = [e for e in fs0_write_funcs]
# count static call sites to those functions: scan direct calls
call_targets = collections.Counter()
for e, (seen, jt) in funcs.items():
    for va in seen:
        i = decode_at(va)
        if i.mnemonic == Mnemonic.CALL and i.op0_kind == OpKind.NEAR_BRANCH32:
            call_targets[i.near_branch_target] += 1
helpers = sorted(((call_targets[e], e) for e in helper_cands), reverse=True)[:12]
n_fs0_funcs = len(fs0_write_funcs)
# functions that *call* a helper that writes fs:[0] (e.g. __SEH_prolog4 / __EH_prolog3*)
hub = {e for c, e in helpers if c >= 50}
callers_of_hub = set()
for e, (seen, jt) in funcs.items():
    for va in seen:
        i = decode_at(va)
        if i.mnemonic == Mnemonic.CALL and i.op0_kind == OpKind.NEAR_BRANCH32 and i.near_branch_target in hub:
            callers_of_hub.add(e); break

# ---------------------------------------------------------------- indirect calls summary / natives table candidates
tbls = sorted(((len(v), d) for d, v in call_idx_table.items()), reverse=True)

# ---------------------------------------------------------------- write outputs
with open(os.path.join(outdir, 'functions.tsv'), 'w') as fh:
    fh.write('entry\tlo\thi\tbytes\tninstr\ttail_out\n')
    for (e, lo, hi, nb, n) in rows:
        fh.write('%x\t%x\t%x\t%d\t%d\t%d\n' % (e, lo, hi, nb, n, tail_by_func.get(e, 0)))
with open(os.path.join(outdir, 'gaps.tsv'), 'w') as fh:
    fh.write('va\tsize\tfirst_bytes\n')
    for va, n in gaps_out[:5000]:
        fh.write('%x\t%d\t%s\n' % (va, n, text[va - TEXT_VA:va - TEXT_VA + 16].hex()))
with open(os.path.join(outdir, 'indirect_calls.tsv'), 'w') as fh:
    fh.write('site\tkind\tdetail\n')
    for s, k, d in indirect_calls: fh.write('%x\t%s\t%s\n' % (s, k, d))
with open(os.path.join(outdir, 'jump_tables.tsv'), 'w') as fh:
    fh.write('site\ttable\tentries\n')
    for va, tbl, ents in jt_sites2:
        fh.write('%x\t%s\t%s\n' % (va, '%x' % tbl if isinstance(tbl, int) else '-', len(ents) if isinstance(tbl, int) else str(ents)))
with open(os.path.join(outdir, 'awkward_sites.json'), 'w') as fh:
    json.dump({k: [hex(x) for x in v[:200]] for k, v in awkward.items()}, fh, indent=1)
natives = [(d, [hex(x) for x in v[:20]], len(v)) for d, v in call_idx_table.items()]
natives.sort(key=lambda x: -x[2])

mnemonics_named = {MN[k]: v for k, v in mn_hist.items()}
total_instr = sum(mn_hist.values())
stats_out = {
    'text_bytes': TEXT_SZ, 'functions': len(funcs), 'entries_seeded': len(entries_set),
    'instructions': total_instr,
    'coverage_bytes': {'code': cov_code, 'jump_table_and_index_data': cov_jt, 'padding_cc_90_00': cov_pad, 'unknown': unk},
    'coverage_pct_code': 100.0 * cov_code / TEXT_SZ,
    'coverage_pct_code_plus_data_plus_pad': 100.0 * (cov_code + cov_jt + cov_pad) / TEXT_SZ,
    'largest_unknown_gaps': [(hex(a), n) for a, n in gaps_out[:20]],
    'unknown_gap_count': len(gaps_out),
    'reloc_total': len(relocs), 'reloc_by_section': {str(k): v for k, v in reloc_by_sec.items()},
    'reloc_to_text_by_site_section': {k: len(v) for k, v in text_ptrs.items()},
    'indirect_kinds': dict(ind_kind),
    'import_call_sites': sum(imp_calls.values()),
    'jump_tables': {'count': jt_count, 'total_entries': jt_total_entries, 'entry_hist(bucket8)': dict(jt_ent_hist),
                    'unresolved_indirect_jmp_sites': jt_unknown_sites, 'index_byte_tables': idx_tables, 'index_table_bytes': idx_tbl_bytes,
                    'in_text': sum(1 for va, tbl, ents in {t: (va, t, e) for va, t, e in jt_sites2 if t}.values() if in_text(tbl)) if False else None},
    'tail_calls': {'jmp_to_other_entry': tail_calls, 'jcc_to_other_entry': tail_jcc, 'functions_with_tail': len(tail_by_func)},
    'shared_code_collisions': len(stats['shared_entries']),
    'invalid_decode_sites': len(stats['invalid']),
    'frame_kinds': dict(frame_kind), 'nonstd_prologue_examples': [(hex(a), b) for a, b in nonstd_examples[:20]],
    'calling_convention_guess': dict(conv),
    'ret_imm_hist_top': RET_IMM.most_common(12),
    'func_size_hist': dict(sorted(fsz_hist.items())),
    'seh': {'fs0_write_funcs': n_fs0_funcs, 'fs0_read_funcs': len(fs0_read_funcs), 'fs_sites': {k: len(v) for k, v in fs_sites.items()},
            'top_fs0_writer_helpers (callcount, va)': [(c, hex(e)) for c, e in helpers],
            'funcs_calling_hub_helpers(>=50 calls)': len(callers_of_hub),
            'cxx_ehfuncinfo_structs': n_eh, 'ehfuncinfo_with_try': n_try, 'try_blocks_total': n_try_blocks,
            'jmp_cxxframehandler3_stubs': cxx_stubs, 'handler_iat_refs': exh_stub_cnt, 'throw_call_sites': throw_sites,
            'setjmp3_call_sites': len(setjmp_sites), 'longjmp_call_sites': len(longjmp_sites),
            'setjmp_sites': [hex(x) for x in setjmp_sites[:30]], 'longjmp_sites': [hex(x) for x in longjmp_sites[:30]]},
    'segments': {str(k): v for k, v in segs.items()},
    'extension_usage_instrs': dict(feat_hist),
    'awkward_counts': {k: len(v) for k, v in awkward.items()},
    'lock_prefix_count': lock_cnt,
    'string_ops_rep': {'%s/%s' % k: v for k, v in rep_hist.items()},
    'int_other_sites': [hex(x) for x in stats['int_other'][:20]],
    'natives_dispatch_candidates (idx*4+tbl calls)': [(hex(d), n) for n, d in tbls[:15]],
    'mnemonic_hist_distinct': len(mnemonics_named),
    'mnemonic_hist': dict(sorted(mnemonics_named.items(), key=lambda kv: -kv[1])),
    'top_import_call_sites': imp_calls.most_common(40),
}
json.dump(stats_out, open(os.path.join(outdir, 'stats.json'), 'w'), indent=1, default=str)
json.dump({'natives': natives[:30]}, open(os.path.join(outdir, 'natives_sites.json'), 'w'), indent=1)
log('done')
print(json.dumps({k: v for k, v in stats_out.items() if k not in ('mnemonic_hist', 'func_size_hist', 'top_import_call_sites')}, indent=1, default=str)[:6000])
