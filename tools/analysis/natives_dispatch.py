#!/usr/bin/env python3
"""Locate UnrealScript native dispatch through GNatives: sites reading [idx*4+GNATIVES] then calling the loaded register.
Usage: python3 -I natives_dispatch.py <exe> <functions.tsv> <gnatives_va_hex>"""
import sys, collections, pefile
from iced_x86 import Decoder, Instruction, Mnemonic, OpKind, Register, FlowControl
exe, ftsv, g = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
pe = pefile.PE(exe, fast_load=True); B = pe.OPTIONAL_HEADER.ImageBase
t = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
T0 = B + t.VirtualAddress; text = pe.get_data(t.VirtualAddress, min(t.Misc_VirtualSize, t.SizeOfRawData))
dec = Decoder(32, text, ip=T0); ins = Instruction()
funcs = []
for i, l in enumerate(open(ftsv)):
    if i: f = l.split('\t'); funcs.append((int(f[0], 16), int(f[2], 16)))
reads = []; writes = []; others = []
dispatch = []
for e, hi in funcs:
    va = e; window = []   # (va, mnemonic, dest reg)
    while va < max(hi, e + 1):
        dec.position = va - T0; dec.ip = va; dec.decode_out(ins)
        if ins.is_invalid: break
        isg = False
        for k in range(ins.op_count):
            if ins.op_kind(k) == OpKind.MEMORY and ins.memory_index != Register.NONE and ins.memory_base == Register.NONE \
                    and ins.memory_index_scale == 4 and (ins.memory_displacement & 0xffffffff) == g:
                isg = True
        if isg:
            if ins.mnemonic == Mnemonic.MOV and ins.op0_kind == OpKind.REGISTER:
                reads.append((va, ins.op0_register, e))
                window.append((va, ins.op0_register, e))
            elif ins.mnemonic == Mnemonic.MOV and ins.op0_kind == OpKind.MEMORY: writes.append((va, e))
            elif ins.mnemonic == Mnemonic.CALL: dispatch.append((va, 'call [idx*4+G]', e))
            else: others.append((va, ins.mnemonic, e))
        if ins.mnemonic == Mnemonic.CALL and ins.op0_kind == OpKind.REGISTER:
            for (rv, reg, fe) in window:
                if reg == ins.op0_register and va - rv < 40: dispatch.append((va, 'mov r,[idx*4+G] ... call r (read at %x)' % rv, e))
        va += ins.len
    window.clear()
# registration sites compare the loaded value with execUndefined; real dispatch sites call it without that compare
real = []
for (va, kind, e) in dispatch:
    real.append((va, e))
byfn = collections.Counter(e for va, e in real)
print('GNatives reads(mov r,[..]):', len(reads), 'writes:', len(writes), 'other:', len(others), 'read+call-reg sites:', len(dispatch), 'in', len(byfn), 'functions')
big = [(n, hex(e)) for e, n in byfn.most_common(12)]
print('functions with most read+call sites:', big)
