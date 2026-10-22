#!/usr/bin/env python3
"""Call sites of imported functions: python3 -I import_xrefs.py <pe> <dll> [name ...]
Finds `call/jmp [IAT slot]` and `mov reg, [IAT slot]` references in executable sections (stdout only; game code is not saved)."""
import sys, struct, pefile
pe_path, dll = sys.argv[1], sys.argv[2].lower()
names = set(sys.argv[3:])
pe = pefile.PE(pe_path)
base = pe.OPTIONAL_HEADER.ImageBase
slots = {}
for imp_list in (getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []), getattr(pe, 'DIRECTORY_ENTRY_DELAY_IMPORT', [])):
    for e in imp_list:
        if e.dll.decode().lower() != dll: continue
        for i in e.imports:
            n = i.name.decode() if i.name else '#%d' % i.ordinal
            if names and n not in names: continue
            slots[i.address] = n
for s in pe.sections:
    if not s.Characteristics & 0x20000000: continue
    data = s.get_data(); va0 = base + s.VirtualAddress
    for off in range(len(data) - 6):
        b0, b1 = data[off], data[off + 1]
        if b0 == 0xFF and b1 in (0x15, 0x25): op, at = ('call' if b1 == 0x15 else 'jmp'), off + 2
        elif b0 == 0x8B and (b1 & 0xC7) == 0x05: op, at = 'mov', off + 2
        elif b0 == 0xA1: op, at = 'mov eax', off + 1
        else: continue
        tgt = struct.unpack_from('<I', data, at)[0]
        if tgt in slots: print('%08x  %-8s %s' % (va0 + off, op, slots[tgt]))
