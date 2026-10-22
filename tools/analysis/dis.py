#!/usr/bin/env python3
"""Tiny disassembler helper: python3 -I dis.py <exe> <va-hex> [count]   (prints to stdout only; do not save game disassembly outside private/)"""
import sys, pefile
from iced_x86 import Decoder, Formatter, FormatterSyntax
exe, va = sys.argv[1], int(sys.argv[2], 16); n = int(sys.argv[3]) if len(sys.argv) > 3 else 30
pe = pefile.PE(exe, fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
data = pe.get_data(va - base, n * 15)
d = Decoder(32, data, ip=va); f = Formatter(FormatterSyntax.INTEL)
for i, ins in enumerate(d):
    if i >= n: break
    print('%08x  %s' % (ins.ip, f.format(ins)))
