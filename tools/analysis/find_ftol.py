#!/usr/bin/env python3
"""Locate the MSVC CRT float->int helpers (_ftol2_sse, _ftol2_sse_excpt, _ftol2, _ftol) in PE modules by their standard instruction
sequences: python3 -I find_ftol.py <pe> [...]   (prints addresses only)"""
import sys, re, pefile
PATS = {
    # cmp [__sse2_available],0; je _ftol2; push ebp; mov ebp,esp; sub esp,8; and esp,-8; fstp qword [esp]; cvttsd2si eax,[esp]; leave; ret
    '_ftol2_sse': rb'\x83\x3d....\x00\x74.\x55\x8b\xec\x83\xec\x08\x83\xe4\xf8\xdd\x1c\x24\xf2\x0f\x2c\x04\x24\xc9\xc3',
    # push ebp; mov ebp,esp; sub esp,20h; and esp,-16; fld st0; fst dword [esp+18h]; fistp qword [esp+10h]; fild qword [esp+10h]; ...
    '_ftol2': rb'\x55\x8b\xec\x83\xec\x20\x83\xe4\xf0\xd9\xc0\xd9\x54\x24\x18\xdf\x7c\x24\x10\xdf\x6c\x24\x10',
    # push ebp; mov ebp,esp; add esp,-12; fnstcw [ebp-2]; ... fistp qword [ebp-12] ... (rounding mode forced to chop)
    '_ftol': rb'\x55\x8b\xec\x83\xc4\xf4\xd9\x7d\xfe\x66\x8b\x45\xfe\x80\xcc\x0c\x66\x89\x45\xfc\xd9\x6d\xfc\xdf\x7d\xf4',
}
for path in sys.argv[1:]:
    pe = pefile.PE(path, fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
    for s in pe.sections:
        if not s.Characteristics & 0x20000000: continue
        data = s.get_data(); va0 = base + s.VirtualAddress
        for name, pat in PATS.items():
            for m in re.finditer(pat, data, re.S):
                print('%-24s %-12s %08x (rva %08x)' % (path.split('/')[-1], name, va0 + m.start(), va0 + m.start() - base))
