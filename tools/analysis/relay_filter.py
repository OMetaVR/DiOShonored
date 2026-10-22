#!/usr/bin/env python3
"""Streaming filter for WINEDEBUG=+relay,+loaddll output (read from stdin, or a file argument).

Keeps only calls whose return address lies in a *non-Wine* (game-supplied, native) module, i.e. calls the
recompiled code would make into the HLE runtime. Writes:
  <out>.calls.tsv   function  count  callers(module:count,...)  first_seen_order  sample_args
  <out>.modules.tsv module  base  kind(native/builtin)  size
  <out>.raw.gz      (optional, size-capped) the raw filtered Call lines
Usage: python3 -I relay_filter.py <game_bin_dir> <out_prefix> [--raw-cap-mb N] [infile]
"""
import sys, re, os, gzip, collections, bisect
import pefile

args = [a for a in sys.argv[1:]]
game_dir, out = args[0], args[1]
raw_cap = 256
rest = args[2:]
if '--raw-cap-mb' in rest:
    i = rest.index('--raw-cap-mb'); raw_cap = int(rest[i + 1]); del rest[i:i + 2]
src = open(rest[0], 'r', errors='replace') if rest else sys.stdin

def pe_size(path):
    try:
        return pefile.PE(path, fast_load=True).OPTIONAL_HEADER.SizeOfImage
    except Exception:
        return 0x1000000
def find_file(name):
    base = os.path.basename(name.replace('\\', '/'))
    for root in (game_dir, os.path.join(game_dir, 'Microsoft.VC90.CRT')):
        p = os.path.join(root, base)
        if os.path.exists(p): return p
    return None

RE_LOAD = re.compile(r'Loaded L?"([^"]+)" at ([0-9A-Fa-f]+): (\w+)')
RE_CALL = re.compile(r'Call (\S+?)\((.*)\) ret=([0-9A-Fa-f]+)')
mods = []           # (base, end, name)
starts = []
modlist = []
calls = collections.OrderedDict()
caller_cnt = collections.defaultdict(collections.Counter)
samples = collections.defaultdict(list)
raw = gzip.open(out + '.raw.gz', 'wt') if raw_cap > 0 else None
raw_bytes = 0
total = kept = 0

def add_module(name, base, kind):
    size = 0
    if kind == 'native':
        f = find_file(name)
        size = pe_size(f) if f else 0x2000000
    modlist.append((os.path.basename(name.replace('\\', '/')), base, kind, size))
    # only modules that ship with the game count as "game code"; Proton-provided native PE modules (dxvk d3d9/dxgi,
    # steamclient, nvapi ...) are the host side of the HLE boundary
    if kind == 'native' and find_file(name):
        bisect.insort(mods, (base, base + size, os.path.basename(name.replace('\\', '/'))))

def caller(addr):
    i = bisect.bisect_right(mods, (addr, 1 << 40, '')) - 1
    if i >= 0 and mods[i][0] <= addr < mods[i][1]:
        return mods[i][2]
    return None

for line in src:
    m = RE_LOAD.search(line)
    if m:
        add_module(m.group(1), int(m.group(2), 16), m.group(3)); continue
    if 'Call ' not in line: continue
    total += 1
    m = RE_CALL.search(line)
    if not m: continue
    fn, a, ret = m.group(1), m.group(2), int(m.group(3), 16)
    c = caller(ret)
    if c is None: continue
    kept += 1
    if fn not in calls:
        calls[fn] = len(calls)
    caller_cnt[fn][c] += 1
    if len(samples[fn]) < 2: samples[fn].append(a[:160])
    if raw is not None and raw_bytes < raw_cap * 1024 * 1024:
        raw.write(line); raw_bytes += len(line)
if raw: raw.close()
with open(out + '.calls.tsv', 'w') as fh:
    fh.write('function\tcount\tcallers\tfirst_seen\tsample_args\n')
    for fn, order in sorted(calls.items(), key=lambda kv: kv[0].lower()):
        cc = caller_cnt[fn]
        fh.write('%s\t%d\t%s\t%d\t%s\n' % (fn, sum(cc.values()), ','.join('%s:%d' % kv for kv in cc.most_common()), order, ' || '.join(samples[fn]).replace('\t', ' ')))
with open(out + '.modules.tsv', 'w') as fh:
    for n, b, k, s in modlist: fh.write('%s\t%08x\t%s\t%x\n' % (n, b, k, s))
print('relay lines with Call: %d, from game modules: %d, unique functions: %d' % (total, kept, len(calls)), file=sys.stderr)
