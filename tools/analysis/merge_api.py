#!/usr/bin/env python3
"""Merge static imports (private/api/os_api_surface.json from imports.py) with a runtime relay trace
(trace_post.py output) into tools/analysis/out/api_surface.json / .tsv.
Usage: python3 -I merge_api.py <os_api_surface.json> <api_called-*.txt> <outdir>"""
import sys, json, os, collections
surf = json.load(open(sys.argv[1])); called = {}
for l in open(sys.argv[2]):
    k, c, who = l.rstrip('\n').split('\t'); called[k.lower()] = (int(c), who)
outdir = sys.argv[3]; os.makedirs(outdir, exist_ok=True)
# Wine forwards some kernel32 exports to ntdll; the relay shows the forwarded name
FWD = {'heapalloc': 'rtlallocateheap', 'heapfree': 'rtlfreeheap', 'heaprealloc': 'rtlreallocateheap', 'heapsize': 'rtlsizeheap',
       'initializecriticalsection': 'rtlinitializecriticalsection', 'deletecriticalsection': 'rtldeletecriticalsection',
       'entercriticalsection': 'rtlentercriticalsection', 'leavecriticalsection': 'rtlleavecriticalsection',
       'tryentercriticalsection': 'rtltryentercriticalsection', 'encodepointer': 'rtlencodepointer', 'decodepointer': 'rtldecodepointer',
       'setcriticalsectionspincount': 'rtlsetcriticalsectionspincount', 'exitthread': 'rtlexituserthread',
       'rtlunwind': 'rtlunwind', 'interlockedincrement': 'rtlinterlockedincrement', 'interlockeddecrement': 'rtlinterlockeddecrement',
       'initializeslisthead': 'rtlinitializeslisthead', 'interlockedpushentryslist': 'rtlinterlockedpushentryslist',
       'interlockedflushslist': 'rtlinterlockedflushslist', 'interlockedpopentryslist': 'rtlinterlockedpopentryslist',
       'rtlcapturecontext': 'rtlcapturecontext', 'rtlunwindex': 'rtlunwindex'}
rows = []; stats = collections.defaultdict(lambda: [0, 0])
for cat in ('os', 'crt', 'stub'):
    for dll, fs in surf[cat].items():
        for f, imps in fs.items():
            fl = {'ord2': 'xinputgetstate', 'ord3': 'xinputsetstate'}.get(f.lower(), f.lower()) if dll.startswith('xinput1_3') else f.lower()
            key = '%s!%s' % (dll.replace('.dll', ''), fl)
            hit = called.get(key)
            if hit is None and dll.startswith('kernel32') and f.lower() in FWD:
                hit = called.get('ntdll!' + FWD[f.lower()])
            if hit is None and dll.startswith('msvcr80'):
                hit = called.get('msvcr90!' + f.lower())
            rows.append({'category': cat, 'dll': dll, 'function': f, 'static_importers': imps,
                         'called_in_startup_trace': hit is not None, 'trace_calls': hit[0] if hit else 0})
            stats[(cat, dll)][0] += 1; stats[(cat, dll)][1] += hit is not None
json.dump(rows, open(os.path.join(outdir, 'api_surface.json'), 'w'), indent=1)
with open(os.path.join(outdir, 'api_surface.tsv'), 'w') as fh:
    fh.write('category\tdll\tfunction\tcalled_in_startup_trace\ttrace_calls\tstatic_importers\n')
    for r in sorted(rows, key=lambda r: (r['category'], r['dll'], r['function'].lower())):
        fh.write('%s\t%s\t%s\t%d\t%d\t%s\n' % (r['category'], r['dll'], r['function'], r['called_in_startup_trace'], r['trace_calls'], ','.join(r['static_importers'])))
for (cat, dll), (n, c) in sorted(stats.items()): print('%-5s %-18s static=%3d  seen_in_trace=%3d' % (cat, dll, n, c))
# runtime-only (dynamic) functions: called but not statically imported
static_keys = {'%s!%s' % (r['dll'].replace('.dll', ''), r['function'].lower()) for r in rows}
dyn = sorted(k for k in called if k not in static_keys)
open(os.path.join(outdir, 'api_called_not_in_static_imports.txt'), 'w').write('\n'.join('%s\t%d\t%s' % (k, called[k][0], called[k][1]) for k in dyn) + '\n')
print('called but not in static import list (forwards, dynamic loads, internal):', len(dyn))
