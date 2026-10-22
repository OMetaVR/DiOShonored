#!/usr/bin/env python3
"""Case-insensitive ASCII/UTF-16 substring search in binaries, printing whole NUL-delimited string + file offset.
Usage: python3 -I grep_strings.py <file> <needle> [<needle> ...]"""
import sys, re
f = sys.argv[1]
d = open(f, 'rb').read()
for n in sys.argv[2:]:
    for enc, pat in (('a', re.compile(rb'[\x20-\x7e]{0,60}' + re.escape(n.encode()) + rb'[\x20-\x7e]{0,60}', re.I)),
                     ('w', re.compile(rb'(?:[\x20-\x7e]\x00){0,60}' + b'\x00'.join(re.escape(c.encode()) for c in n) + rb'\x00(?:[\x20-\x7e]\x00){0,60}', re.I))):
        hits = set()
        for m in pat.finditer(d):
            s = m.group().decode('utf-16le' if enc == 'w' else 'ascii', 'replace')
            hits.add((s, m.start()))
        for s, o in sorted(hits, key=lambda x: x[1])[:12]:
            print('%-8s %s %08x %s' % (n, enc, o, s))
