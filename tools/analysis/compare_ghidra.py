#!/usr/bin/env python3
"""Cross-check discover.py's function set against the Ghidra export.
Usage: python3 -I compare_ghidra.py <ghidra_functions.tsv> <py functions.tsv> <exe> [out.json]"""
import sys, json, bisect, pefile
gh, py, exe = sys.argv[1:4]
pe = pefile.PE(exe, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
t = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
T0 = base + t.VirtualAddress; TSZ = min(t.Misc_VirtualSize, t.SizeOfRawData); T1 = T0 + TSZ
g_entries = {}; g_cov = bytearray(TSZ); g_ranges = []
for i, l in enumerate(open(gh)):
    if i == 0: continue
    f = l.rstrip('\n').split('\t')
    e = int(f[0], 16)
    if not (T0 <= e < T1): continue
    g_entries[e] = f[1]
    for r in f[7].split(','):
        a, b = r.split('-'); a = int(a, 16); b = int(b, 16)
        g_ranges.append((a, b, e))
        for x in range(max(a, T0), min(b + 1, T1)): g_cov[x - T0] = 1
g_ranges.sort()
starts = [r[0] for r in g_ranges]
def g_owner(va):
    k = bisect.bisect_right(starts, va) - 1
    while k >= 0 and k > bisect.bisect_right(starts, va) - 8:
        a, b, e = g_ranges[k]
        if a <= va <= b: return e
        k -= 1
    return None
p_entries = set(); p_cov = bytearray(TSZ); p_ranges = []
for i, l in enumerate(open(py)):
    if i == 0: continue
    f = l.rstrip('\n').split('\t')
    p_entries.add(int(f[0], 16))
ge = set(g_entries)
both = ge & p_entries
only_g = ge - p_entries; only_p = p_entries - ge
# classify only_p: inside a Ghidra function body (not its entry) vs not covered by Ghidra at all
p_in_g = sum(1 for e in only_p if g_cov[e - T0] and g_owner(e) is not None)
p_not_g = len(only_p) - p_in_g
res = {
 'text_bytes': TSZ, 'ghidra_functions': len(ge), 'python_functions': len(p_entries), 'common_entries': len(both),
 'ghidra_only': len(only_g), 'python_only': len(only_p),
 'python_only_inside_ghidra_body': p_in_g, 'python_only_not_covered_by_ghidra': p_not_g,
 'ghidra_function_bytes_pct_of_text': 100.0 * sum(g_cov) / TSZ,
}
json.dump(res, open(sys.argv[4], 'w') if len(sys.argv) > 4 else sys.stdout, indent=1)
print(json.dumps(res, indent=1))
