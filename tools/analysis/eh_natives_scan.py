#!/usr/bin/env python3
"""Second-stage scan over discover.py's function list:
  * classify EH frame setups (inline `push -1/-2; push handler; mov eax, fs:[0]` pattern) and handler stubs
  * find [idx*4+disp32] table accesses in .data/.rdata (GNatives-style dispatch candidates)
  * count call kinds with raw decode (cross-check of discover.py)
Usage: python3 -I eh_natives_scan.py <exe> <functions.tsv> <outjson>"""
import sys, json, struct, collections, pefile
from iced_x86 import Decoder, Instruction, Mnemonic, OpKind, Register, FlowControl
MNN = {getattr(Mnemonic, n): n for n in dir(Mnemonic) if n.isupper()}
exe, ftsv, out = sys.argv[1:4]
pe = pefile.PE(exe, fast_load=True); pe.parse_data_directories(directories=[1]); BASE = pe.OPTIONAL_HEADER.ImageBase
secs = [(s.Name.rstrip(b'\0').decode(), BASE + s.VirtualAddress, max(s.Misc_VirtualSize, s.SizeOfRawData)) for s in pe.sections]
def sec(va):
    for n, a, sz in secs:
        if a <= va < a + sz: return n
t = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
T0 = BASE + t.VirtualAddress; TSZ = min(t.Misc_VirtualSize, t.SizeOfRawData)
text = pe.get_data(t.VirtualAddress, TSZ)
dec = Decoder(32, text, ip=T0)
funcs = []
for i, l in enumerate(open(ftsv)):
    if i == 0: continue
    f = l.split('\t'); funcs.append((int(f[0], 16), int(f[2], 16)))
IATS = {}
for ent in pe.DIRECTORY_ENTRY_IMPORT:
    for imp in ent.imports: IATS[imp.address] = (ent.dll.decode().lower() + '!' + (imp.name.decode() if imp.name else '?'))
ins = Instruction()
eh_kind = collections.Counter(); handlers = {}
tbl_use = collections.defaultdict(lambda: collections.Counter())
tbl_sites = collections.defaultdict(list)
callk = collections.Counter()
eh_funcs = 0
seh4_funcs = []
EXC4 = None
for (e, hi) in funcs:
    # EH frame prologue: [push ebp; mov ebp,esp;] push imm8(-1/-2); push imm32; mov eax,fs:[0]
    va = e; seq = []
    for _ in range(6):
        dec.position = va - T0; dec.ip = va; dec.decode_out(ins)
        if ins.is_invalid: break
        seq.append((ins.mnemonic, ins.op0_kind, (ins.immediate(0) & 0xffffffff) if ins.op0_kind in (OpKind.IMMEDIATE8TO32, OpKind.IMMEDIATE32) else None,
                    ins.op1_kind, ins.memory_segment, ins.memory_displacement, ins.len))
        va += ins.len
    # find push imm ; push imm ; mov eax, fs:[0]
    for k in range(len(seq) - 2):
        a, b, c = seq[k], seq[k + 1], seq[k + 2]
        if a[0] == Mnemonic.PUSH and b[0] == Mnemonic.PUSH and c[0] == Mnemonic.MOV and c[4] == Register.FS and a[2] in (0xffffffff, 0xfffffffe):
            eh_funcs += 1
            state = a[2]; hnd = b[2]
            handlers.setdefault(hnd, 0); handlers[hnd] += 1
            eh_kind['init_state=%d' % (struct.unpack('<i', struct.pack('<I', state))[0])] += 1
            break
    # SEH4 frame: push -2; push <scope table>; push <__except_handler4 thunk>; mov eax,fs:[0]
    for k in range(len(seq) - 3):
        a, b, c, d4 = seq[k], seq[k + 1], seq[k + 2], seq[k + 3]
        if a[0] == Mnemonic.PUSH and b[0] == Mnemonic.PUSH and c[0] == Mnemonic.PUSH and d4[0] == Mnemonic.MOV and d4[4] == Register.FS and a[2] == 0xfffffffe:
            seh4_funcs.append((e, c[2])); break
    # linear decode of the function range for table accesses / call kinds
    va = e
    end = max(hi, e + 1)
    while va < end:
        dec.position = va - T0; dec.ip = va; dec.decode_out(ins)
        if ins.is_invalid: break
        l = ins.len
        for k in range(ins.op_count):
            if ins.op_kind(k) == OpKind.MEMORY and ins.memory_index != Register.NONE and ins.memory_index_scale == 4 and ins.memory_base == Register.NONE:
                d = ins.memory_displacement & 0xffffffff
                s = sec(d)
                if s in ('.data', '.rdata'):
                    tbl_use[d][ins.mnemonic] += 1
                    if len(tbl_sites[d]) < 6: tbl_sites[d].append(va)
        if ins.flow_control == FlowControl.INDIRECT_CALL:
            if ins.op0_kind == OpKind.REGISTER: callk['call reg'] += 1
            elif ins.op0_kind == OpKind.MEMORY:
                if ins.memory_base == Register.NONE and ins.memory_index == Register.NONE:
                    callk['call [IAT]' if (ins.memory_displacement & 0xffffffff) in IATS else 'call [abs]'] += 1
                else: callk['call [base+..]'] += 1
        va += l
# classify handlers: follow the E9 jmp at the end of the stub (mov eax,<FuncInfo/ScopeTable>; jmp <thunk>) to the thunk,
# which is `jmp [IAT]` of the CRT handler
def jmp_target(va, n=48):
    b = text[va - T0:va - T0 + n]
    for k in range(len(b) - 4):
        if b[k] == 0xe9:
            rel = struct.unpack('<i', b[k + 1:k + 5])[0]
            return va + k + 5 + rel
    return None
thunk_kind = {}
hk = collections.Counter()
for h in handlers:
    if not (T0 <= h < T0 + TSZ): hk['outside .text'] += 1; continue
    tg = jmp_target(h)
    if tg is None or not (T0 <= tg < T0 + TSZ): hk['no jmp found'] += 1; continue
    if tg not in thunk_kind:
        dec.position = tg - T0; dec.ip = tg; dec.decode_out(ins)
        nm = None
        if ins.mnemonic == Mnemonic.JMP and ins.op0_kind == OpKind.MEMORY and ins.memory_base == Register.NONE:
            nm = IATS.get(ins.memory_displacement & 0xffffffff)
        thunk_kind[tg] = nm or ('local:%x' % tg)
    hk[thunk_kind[tg]] += 1
cxx = hk
sx4 = other = 0
def fmt(d): return hex(d)
cands = sorted(((sum(c.values()), d, dict((MNN.get(k, str(k)), v) for k, v in c.items())) for d, c in tbl_use.items()), reverse=True)[:25]
res = {'seh4_frame_functions': len(seh4_funcs), 'seh4_handlers': dict(collections.Counter(hex(h) for e, h in seh4_funcs)), 'seh4_func_list': [hex(e) for e, h in seh4_funcs[:60]], 'functions_with_inline_eh_frame': eh_funcs, 'init_state': dict(eh_kind), 'distinct_handler_stubs': len(handlers),
       'handler_stub_final_target': dict(cxx),
       'call_kinds_rawdecode': dict(callk),
       'idx4_table_candidates (total_uses, disp, mnemonics)': [(n, hex(d), sec(d), m) for n, d, m in cands],
       'idx4_sites_top5': {hex(d): [hex(x) for x in tbl_sites[d]] for n, d, m in cands[:8]}}
json.dump(res, open(out, 'w'), indent=1, default=str)
print(json.dumps(res, indent=1, default=str)[:5000])
