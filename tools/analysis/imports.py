#!/usr/bin/env python3
"""Enumerate PE imports (normal + delay-load + bound) for the Dishonored install and compute
the transitive OS-level API set required if a given set of DLLs is recompiled.

Usage: python3 -I imports.py <Binaries/Win32 dir> <outdir>
Outputs (outdir): imports_per_module.json, os_api_surface.json, os_api_surface.txt
"""
import sys, os, json, collections
import pefile

GAME = sys.argv[1]
OUT = sys.argv[2]
os.makedirs(OUT, exist_ok=True)

RECOMPILE = ['Dishonored.exe', 'binkw32.dll', 'PhysXLoader.dll', 'PhysXCore.dll', 'PhysXExtensions.dll',
             'PhysXCooking.dll', 'NxCharacter.dll', 'APEX_release.dll', 'APEX_BasicIOS.dll', 'APEX_Clothing.dll',
             'APEX_Destructible.dll', 'APEX_Emitter.dll', 'APEX_IOFX.dll', 'APEX_Particles.dll', 'APEX_RRB.dll',
             'ogg.dll', 'vorbis.dll', 'vorbisfile.dll', 'vorbisenc.dll',
             'Microsoft.VC90.CRT/msvcr90.dll', 'Microsoft.VC90.CRT/msvcp90.dll']
STUBBED = ['steam_api.dll', 'libcurl.dll']          # exe side only
OTHER = ['dbghelp.dll', 'SteamAPIUpdater.dll', 'cudart32_30_9.dll', 'cudart32_40_12.dll',
         'Microsoft.VC90.CRT/msvcm90.dll'] + [f for f in os.listdir(GAME) if f.startswith('wxmsw')]

def load(path):
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[
        pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT'],
        pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT'],
        pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT']])
    res = {'imports': collections.OrderedDict(), 'delay': collections.OrderedDict(), 'exports': []}
    for kind, attr in (('imports', 'DIRECTORY_ENTRY_IMPORT'), ('delay', 'DIRECTORY_ENTRY_DELAY_IMPORT')):
        for ent in getattr(pe, attr, []):
            dll = ent.dll.decode().lower()
            names = []
            for imp in ent.imports:
                names.append(imp.name.decode() if imp.name else 'ord%d' % imp.ordinal)
            res[kind].setdefault(dll, []).extend(names)
    if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
        for s in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            res['exports'].append(s.name.decode() if s.name else 'ord%d' % s.ordinal)
    res['machine'] = hex(pe.FILE_HEADER.Machine)
    res['size'] = os.path.getsize(path)
    return res

mods = {}
for rel in RECOMPILE + STUBBED + OTHER:
    p = os.path.join(GAME, rel)
    if os.path.exists(p):
        mods[rel] = load(p)
json.dump(mods, open(os.path.join(OUT, 'imports_per_module.json'), 'w'), indent=1)


# Classification of every imported DLL name
CRT_NAMES = {'msvcr90.dll', 'msvcp90.dll', 'msvcr80.dll'}          # recompiled CRT (msvcr80 aliased onto msvcr90, see api_surface.md)
STUB_NAMES = {'steam_api.dll', 'libcurl.dll', 'dbghelp.dll', 'faultrep.dll', 'cudart32_30_9.dll', 'cudart32_40_12.dll',
              'nvcuda.dll', 'mss32.dll'}
recomp_names = {os.path.basename(r).lower() for r in RECOMPILE}
def modkey(rel): return os.path.basename(rel).lower()

os_surface = collections.defaultdict(lambda: collections.defaultdict(set))   # host-provided OS DLLs
stub_surface = collections.defaultdict(lambda: collections.defaultdict(set)) # stubbed DLLs
crt_surface = collections.defaultdict(lambda: collections.defaultdict(set))  # CRT DLLs (resolved by recompiled CRT)
inter = collections.defaultdict(lambda: collections.defaultdict(set))        # recompiled-DLL -> recompiled-DLL
per_importer = collections.defaultdict(lambda: collections.OrderedDict())
for rel in RECOMPILE:
    if rel not in mods: continue
    m = mods[rel]; me = modkey(rel)
    for kind in ('imports', 'delay'):
        for dll, fns in m[kind].items():
            if dll in CRT_NAMES and dll != me:
                tgt = crt_surface
            elif dll in STUB_NAMES:
                tgt = stub_surface
            elif dll in recomp_names:
                tgt = inter
            else:
                tgt = os_surface
            for f in fns:
                tgt[dll][f].add(me)
            per_importer[me][dll + (' (delay)' if kind == 'delay' else '')] = len(fns)
def fin(d): return {k: {f: sorted(v) for f, v in sorted(fs.items())} for k, fs in sorted(d.items())}
out = {'os': fin(os_surface), 'crt': fin(crt_surface), 'stub': fin(stub_surface), 'inter_module': fin(inter),
       'per_importer': per_importer}
json.dump(out, open(os.path.join(OUT, 'os_api_surface.json'), 'w'), indent=1)
with open(os.path.join(OUT, 'os_api_surface.txt'), 'w') as fh:
    for cat in ('os', 'crt', 'stub', 'inter_module'):
        for d, fs in out[cat].items():
            for f, imp in fs.items():
                fh.write('%s\t%s!%s\t%s\n' % (cat, d, f, ','.join(imp)))
print('== host OS functions needed (unique per DLL) ==')
tot = 0
for d, fs in out['os'].items():
    print('%-16s %4d' % (d, len(fs))); tot += len(fs)
print('TOTAL', tot)
print('== CRT ==')
for d, fs in out['crt'].items(): print('%-16s %4d' % (d, len(fs)))
print('== stub ==')
for d, fs in out['stub'].items(): print('%-20s %4d' % (d, len(fs)))
print('== per importer ==')
for k, v in per_importer.items(): print(k, dict(v))
# msvcr80 names present in msvcr90 exports?
ex = set(mods['Microsoft.VC90.CRT/msvcr90.dll']['exports'])
miss = [f for f in out['crt'].get('msvcr80.dll', {}) if f not in ex]
print('msvcr80 imports missing from msvcr90 exports:', miss)
