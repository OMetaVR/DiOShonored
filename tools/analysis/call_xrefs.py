#!/usr/bin/env python3
"""Direct call/jmp sites of guest addresses: python3 -I call_xrefs.py <pe> <va-hex> [...]   (counts per target; stdout only)"""
import sys, struct, pefile, collections
pe = pefile.PE(sys.argv[1], fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
want = {int(a, 16) for a in sys.argv[2:]}
cnt = collections.Counter()
for s in pe.sections:
    if not s.Characteristics & 0x20000000: continue
    data = s.get_data(); va0 = base + s.VirtualAddress
    for off in range(len(data) - 5):
        if data[off] in (0xE8, 0xE9):
            t = (va0 + off + 5 + struct.unpack_from('<i', data, off + 1)[0]) & 0xffffffff
            if t in want: cnt[(t, 'call' if data[off] == 0xE8 else 'jmp')] += 1
for (t, k), n in sorted(cnt.items()): print('%08x %-4s %d' % (t, k, n))
