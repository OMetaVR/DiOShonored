#!/usr/bin/env python3
"""Turn <prefix>.calls.tsv (from relay_filter.py) into a sorted unique API list: DLL!Function<TAB>count<TAB>callers.
Usage: python3 -I trace_post.py relay <prefix> <out.txt>
"""
import sys
kind, prefix, out = sys.argv[1:4]
rows = []
for i, l in enumerate(open(prefix + '.calls.tsv')):
    if i == 0: continue
    f = l.rstrip('\n').split('\t')
    fn, cnt, callers = f[0], f[1], f[2]
    dll, _, name = fn.partition('.')
    rows.append((dll.lower(), name, int(cnt), callers))
rows.sort()
with open(out, 'w') as fh:
    for d, n, c, cl in rows: fh.write('%s!%s\t%d\t%s\n' % (d, n, c, cl))
print('%d unique API functions -> %s' % (len(rows), out))
