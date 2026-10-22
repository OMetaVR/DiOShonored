#!/usr/bin/env python3
"""Classify memory accesses of every decoded instruction: stack (esp/ebp based, push/pop/call/ret) vs absolute (globals) vs other-register-based
(heap/this/array). Used to size the x86-TSO -> ARM ordering problem.
Usage: python3 -I memclass.py <exe> <functions.tsv> <out.json>"""
import sys, json, collections, pefile
from iced_x86 import Decoder, Instruction, InstructionInfoFactory, OpKind, Register, Mnemonic, OpAccess, FlowControl
exe, ftsv, out = sys.argv[1:4]
pe = pefile.PE(exe, fast_load=True); B = pe.OPTIONAL_HEADER.ImageBase
t = [s for s in pe.sections if s.Name.startswith(b'.text')][0]; T0 = B + t.VirtualAddress; TSZ = min(t.Misc_VirtualSize, t.SizeOfRawData)
text = pe.get_data(t.VirtualAddress, TSZ)
dec = Decoder(32, text, ip=T0); ins = Instruction(); fac = InstructionInfoFactory()
fs = []
for i, l in enumerate(open(ftsv)):
    if i: f = l.split('\t'); fs.append((int(f[0], 16), int(f[2], 16)))
fs.sort()
seen = set()
cnt = collections.Counter()
READ = (OpAccess.READ, OpAccess.COND_READ, OpAccess.READ_WRITE, OpAccess.READ_COND_WRITE)
WRITE = (OpAccess.WRITE, OpAccess.COND_WRITE, OpAccess.READ_WRITE, OpAccess.READ_COND_WRITE)
n = 0
for e, hi in fs:
    va = e
    while va < max(hi, e + 1):
        if va in seen: break
        dec.position = va - T0; dec.ip = va; dec.decode_out(ins)
        if ins.is_invalid: break
        seen.add(va); n += 1
        m = ins.mnemonic
        if m in (Mnemonic.LEA, Mnemonic.NOP, Mnemonic.PREFETCHT0, Mnemonic.PREFETCHNTA): va += ins.len; continue
        if m in (Mnemonic.PUSH, Mnemonic.CALL): cnt['stack_write(push/call)'] += 1
        if m in (Mnemonic.POP, Mnemonic.RET): cnt['stack_read(pop/ret)'] += 1
        if m == Mnemonic.LEAVE: cnt['stack_read(pop/ret)'] += 1
        if ins.op_count and any(ins.op_kind(k) == OpKind.MEMORY for k in range(ins.op_count)):
            info = fac.info(ins)
            for mem in info.used_memory():
                b = mem.base; ix = mem.index
                if mem.segment == Register.FS: cls = 'fs'
                elif b in (Register.ESP, Register.EBP): cls = 'stack(esp/ebp)'
                elif b == Register.NONE and ix == Register.NONE: cls = 'absolute(global/IAT)'
                else: cls = 'other-reg(heap/this/array)'
                acc = mem.access
                if acc in READ: cnt[cls + ' load'] += 1
                if acc in WRITE: cnt[cls + ' store'] += 1
        va += ins.len
cnt['instructions_scanned'] = n
tot_ld = sum(v for k, v in cnt.items() if k.endswith(' load')); tot_st = sum(v for k, v in cnt.items() if k.endswith(' store'))
res = dict(cnt); res['total_explicit_loads'] = tot_ld; res['total_explicit_stores'] = tot_st
json.dump(res, open(out, 'w'), indent=1)
print(json.dumps(res, indent=1))
