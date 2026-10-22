#!/usr/bin/env python3
"""Streaming reducer for WINEDEBUG trace output of Wine's d3d9/d3d/dsound/dinput/xinput/ole channels.
stdin -> <out>.com.tsv : channel  function  count  first_seen  sample_line
      -> <out>.clsids.tsv : every CoCreateInstance/CoGetClassObject CLSID/IID seen (ole channel)
      -> <out>.loaddll.txt : loaddll lines
      -> <out>.raw.gz : capped copy of the interesting lines
Usage: python3 -I com_filter.py <out_prefix> [--raw-cap-mb N]
"""
import sys, re, gzip, collections
out = sys.argv[1]
cap = 128
if '--raw-cap-mb' in sys.argv: cap = int(sys.argv[sys.argv.index('--raw-cap-mb') + 1])
CH = ('d3d9', 'd3d', 'd3d_shader', 'dsound', 'dinput', 'xinput', 'ole', 'xaudio2', 'mmdevapi', 'winmm', 'dxgi')
RE = re.compile(r'(?:trace|fixme|warn|err):(\w+):(\w+)')
RE_CLS = re.compile(r'(Co(?:Create(?:Instance(?:Ex)?)|GetClassObject)|rclsid=|riid=)')
cnt = collections.Counter(); first = {}; sample = {}
cls = collections.OrderedDict()
raw = gzip.open(out + '.raw.gz', 'wt'); rb = 0
ld = open(out + '.loaddll.txt', 'w')
for line in sys.stdin:
    if 'loaddll' in line:
        ld.write(line); continue
    m = RE.search(line)
    if not m: continue
    ch, fn = m.groups()
    if ch not in CH: continue
    k = (ch, fn)
    kind = line[line.index(m.group(0)) :].split(':', 1)[0]
    cnt[k] += 1
    if k not in first:
        first[k] = len(first); sample[k] = line.strip()[:200]
    if ch == 'ole' and RE_CLS.search(line) and len(cls) < 5000:
        cls.setdefault(line.strip()[:300], 0)
    if rb < cap * 1024 * 1024:
        raw.write(line); rb += len(line)
raw.close(); ld.close()
with open(out + '.com.tsv', 'w') as fh:
    fh.write('channel\tfunction\tcount\tfirst_seen\tsample\n')
    for k in sorted(cnt):
        fh.write('%s\t%s\t%d\t%d\t%s\n' % (k[0], k[1], cnt[k], first[k], sample[k].replace('\t', ' ')))
with open(out + '.clsids.tsv', 'w') as fh:
    for l in cls: fh.write(l + '\n')
print('com trace: %d unique (channel,function)' % len(cnt), file=sys.stderr)
