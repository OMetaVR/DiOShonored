#!/usr/bin/env python3
"""Find LoadLibrary / GetProcAddress candidate strings in PE files.
 1. DLL-name strings (ASCII and UTF-16) ending in .dll
 2. ASCII strings that exactly equal an export name of a system DLL (taken from a Wine/Proton i386-windows
    directory) AND which are not already in the file's import table -> likely GetProcAddress targets.
Usage: python3 -I dyn_strings.py <pe file> <wine i386-windows dir> <out json>
"""
import sys, os, re, json, pefile, collections
pe_path, wine_dir, out = sys.argv[1:4]
data = open(pe_path, 'rb').read()
pe = pefile.PE(pe_path, fast_load=True)
pe.parse_data_directories(directories=[1, 13])
imported = set()
for d in ('DIRECTORY_ENTRY_IMPORT', 'DIRECTORY_ENTRY_DELAY_IMPORT'):
    for e in getattr(pe, d, []):
        for i in e.imports:
            if i.name: imported.add(i.name.decode())

def secname(off):
    for s in pe.sections:
        if s.PointerToRawData <= off < s.PointerToRawData + s.SizeOfRawData:
            return s.Name.rstrip(b'\0').decode()
    return '?'

dlls = collections.defaultdict(set)
for m in re.finditer(rb'[A-Za-z0-9_\-\.\\/ ]{3,64}\.(?:dll|DLL|Dll|drv|ocx|exe)\b', data):
    dlls[m.group().decode()].add(('a', hex(m.start()), secname(m.start())))
for m in re.finditer(rb'(?:[A-Za-z0-9_\-\.\\/]\x00){3,64}\.\x00(?:[dD]\x00[lL]\x00[lL]\x00|d\x00r\x00v\x00)', data):
    dlls[m.group().decode('utf-16le')].add(('w', hex(m.start()), secname(m.start())))
dll_list = {k: sorted(v) for k, v in dlls.items()}

# export names of system DLLs
exports = collections.defaultdict(set)
for fn in os.listdir(wine_dir):
    if not fn.endswith('.dll'): continue
    try:
        p = pefile.PE(os.path.join(wine_dir, fn), fast_load=True)
        p.parse_data_directories(directories=[0])
        for s in getattr(p, 'DIRECTORY_ENTRY_EXPORT', None).symbols if hasattr(p, 'DIRECTORY_ENTRY_EXPORT') else []:
            if s.name and len(s.name) >= 8:
                exports[s.name.decode()].add(fn)
    except Exception:
        pass
cands = {}
for m in re.finditer(rb'(?<![A-Za-z0-9_@?$])([A-Za-z_][A-Za-z0-9_]{7,63})\x00', data):
    s = m.group(1).decode()
    if s in exports and s not in imported:
        cands.setdefault(s, []).append((hex(m.start()), secname(m.start())))
# restrict to the well-known system DLL families to cut noise (common words that are also exports)
keep = {k: v for k, v in cands.items()}
json.dump({'dll_strings': dll_list, 'proc_candidates': {k: {'dlls': sorted(exports[k]), 'where': v[:3]} for k, v in sorted(keep.items())}},
          open(out, 'w'), indent=1)
print('dll strings:', len(dll_list), ' proc candidates:', len(keep))
