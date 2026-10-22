#!/usr/bin/env python3
"""From a relay raw log (relay_filter.py --raw output), list LoadLibrary* module names and GetProcAddress names
requested at runtime, grouped by caller module. Usage: python3 -I trace_dynamic.py <relay-prefix.raw.gz> <out.json>"""
import sys, gzip, re, json, collections
raw, out = sys.argv[1:3]
RE_GPA = re.compile(r'Call KERNEL32\.GetProcAddress\(([0-9a-f]+),[0-9a-f]+ "([^"]+)"\) ret=([0-9a-f]+)')
RE_LL = re.compile(r'Call KERNEL32\.LoadLibrary(?:Ex)?([AW])\([0-9a-f]+ L?"([^"]+)"')
RE_LLR = re.compile(r'ret=([0-9a-f]+)')
gpa = collections.defaultdict(set); ll = collections.defaultdict(set)
mods = {}
# module ranges by ret address high bits are not available here; keep raw ret for later mapping
gpa_ret = collections.defaultdict(set); ll_ret = collections.defaultdict(set)
for line in gzip.open(raw, 'rt', errors='replace'):
    if 'GetProcAddress' in line:
        m = RE_GPA.search(line)
        if m: gpa_ret[m.group(2)].add(int(m.group(3), 16) >> 20)
    elif 'LoadLibrary' in line:
        m = RE_LL.search(line); r = RE_LLR.search(line)
        if m: ll_ret[m.group(2)].add(int(r.group(1), 16) >> 20 if r else 0)
json.dump({'GetProcAddress': {k: sorted(hex(x << 20) for x in v) for k, v in sorted(gpa_ret.items())},
           'LoadLibrary': {k: sorted(hex(x << 20) for x in v) for k, v in sorted(ll_ret.items())}}, open(out, 'w'), indent=1)
print(len(gpa_ret), 'distinct GetProcAddress names;', len(ll_ret), 'distinct LoadLibrary names')
