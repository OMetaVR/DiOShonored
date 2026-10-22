#!/usr/bin/env python3
"""Generate the table/list parts of research/binary/api_surface.md from tools/analysis/out/*.json.
Usage: python3 -I gen_api_md.py <out dir with api_surface.json + imports_per_module.json> > fragment.md"""
import sys, json, collections, os
d = sys.argv[1]
rows = json.load(open(os.path.join(d, 'api_surface.json')))
mods = json.load(open(os.path.join(d, 'imports_per_module.json')))
out = []
w = out.append
# per module direct imports
w('| Module | Recompile? | Size | Exports | Direct imports by DLL (function counts) |')
w('|---|---|---|---|---|')
ROLE = [('Dishonored.exe', 'yes'), ('binkw32.dll', 'yes'), ('PhysXLoader.dll', 'yes'), ('PhysXCore.dll', 'yes'), ('PhysXExtensions.dll', 'yes'),
        ('PhysXCooking.dll', 'yes'), ('NxCharacter.dll', 'yes'), ('APEX_release.dll', 'optional (not loaded in startup trace)'), ('APEX_BasicIOS.dll', 'optional (not loaded in startup trace)'),
        ('APEX_Clothing.dll', 'optional (not loaded in startup trace)'), ('APEX_Destructible.dll', 'optional (not loaded in startup trace)'), ('APEX_Emitter.dll', 'optional (not loaded in startup trace)'),
        ('APEX_IOFX.dll', 'optional (not loaded in startup trace)'), ('APEX_Particles.dll', 'optional (not loaded in startup trace)'), ('APEX_RRB.dll', 'optional (not loaded in startup trace)'),
        ('ogg.dll', 'not referenced by exe; not loaded in trace'), ('vorbis.dll', 'not referenced by exe; not loaded in trace'), ('vorbisfile.dll', 'not referenced by exe; not loaded in trace'), ('vorbisenc.dll', 'no (encoder)'),
        ('Microsoft.VC90.CRT/msvcr90.dll', 'yes (or HLE natively)'), ('Microsoft.VC90.CRT/msvcp90.dll', 'on demand (9 functions used)'),
        ('steam_api.dll', 'stub'), ('libcurl.dll', 'stub'), ('dbghelp.dll', 'stub/drop'), ('cudart32_30_9.dll', 'stub (no CUDA)')]
for name, role in ROLE:
    m = mods.get(name)
    if not m: continue
    imp = collections.OrderedDict()
    for k in ('imports', 'delay'):
        for dll, fns in m[k].items():
            imp[dll + (' (delay)' if k == 'delay' else '')] = len(fns)
    w('| %s | %s | %d | %d | %s |' % (name, role, m['size'], len(m['exports']), ', '.join('%s %d' % kv for kv in imp.items())))
w('')
# union OS surface per DLL
byd = collections.defaultdict(list)
for r in rows:
    if r['category'] == 'os': byd[r['dll']].append(r)
w('| Host DLL | Unique functions (recompile exe+DLLs+CRT) | of which needed by exe directly | seen in startup trace |')
w('|---|---|---|---|')
tot = [0, 0, 0]
for dll in sorted(byd):
    rs = byd[dll]; n = len(rs); ex = sum(1 for r in rs if 'dishonored.exe' in r['static_importers']); sn = sum(1 for r in rs if r['called_in_startup_trace'])
    tot[0] += n; tot[1] += ex; tot[2] += sn
    w('| %s | %d | %d | %d |' % (dll, n, ex, sn))
w('| **total** | **%d** | **%d** | **%d** |' % tuple(tot))
w('')
w('Function names per host DLL (`*` = observed in the startup trace; the exe-only subset is the same list restricted by the importer column in `tools/analysis/out/api_surface.tsv`):')
w('')
for dll in sorted(byd):
    names = ['%s%s' % (r['function'], '*' if r['called_in_startup_trace'] else '') for r in sorted(byd[dll], key=lambda r: r['function'].lower())]
    w('**%s** (%d): %s' % (dll, len(names), ', '.join(names)))
    w('')
# crt
for cat, title in (('crt', 'CRT imports'), ('stub', 'Stubbed DLLs (imports from recompiled modules)')):
    byd2 = collections.defaultdict(list)
    for r in rows:
        if r['category'] == cat: byd2[r['dll']].append(r)
    w('**%s**' % title)
    w('')
    for dll in sorted(byd2):
        names = ['%s%s' % (r['function'], '*' if r['called_in_startup_trace'] else '') for r in sorted(byd2[dll], key=lambda r: r['function'].lower())]
        w('- %s (%d): %s' % (dll, len(names), ', '.join(names)))
    w('')
print('\n'.join(out))
