#!/usr/bin/env python3
"""Summarise a runtime sampling profile (runtime/core/profiler.cpp, DISH_PROFILE=start:duration).

  profile_report.py <profile.txt> <binary> [--top N] [--thread SUBSTR] [--lr]

<binary> is the executable the profile came from (Linux: private/dev/dishonored_dev; iOS: build/ios-game/Dishonored.app/Dishonored,
which keeps its local symbols). Samples are resolved with llvm-nm and summed per function: lifted guest functions are g_XXXXXXXX
(their _x variants are folded in), everything else keeps its host symbol. --lr reports the callers' return addresses instead
(Apple only), which attributes time spent in small leaf helpers to the code that calls them. Prints to stdout only.
"""
import bisect, collections, subprocess, sys


def load_symbols(binary):
    out = subprocess.run(['llvm-nm', '-n', '--defined-only', binary], capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 3 or parts[1] not in 'tTwW':
            continue
        addrs.append(int(parts[0], 16))
        names.append(parts[2])
    macho = any(n.startswith('_') for n in names[:200])
    return addrs, names, (0x100000000 if macho else 0)


def fold(name):
    n = name[1:] if name.startswith('_g_') else name
    if n.startswith('g_') and n.endswith('_x'):
        n = n[:-2]
    return n


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    top = int(args[args.index('--top') + 1]) if '--top' in args else 40
    only = args[args.index('--thread') + 1] if '--thread' in args else None
    kind = 'lr' if '--lr' in args else 'pc'
    prof, binary = args[0], args[1]
    addrs, names, base = load_symbols(binary)
    per_thread = collections.defaultdict(collections.Counter)
    for line in open(prof):
        if line.startswith('#'):
            continue
        if line.startswith('thread '):
            print(line.rstrip())
            continue
        f = line.rstrip('\n').split('\t')
        if len(f) != 5 or f[0] != kind:
            continue
        _, th, mod, off, n = f
        if only and only not in th:
            continue
        if mod == 'exe':
            a = int(off, 16) + base
            i = bisect.bisect_right(addrs, a) - 1
            fn = fold(names[i]) if i >= 0 else '?'
        else:
            fn = mod
        per_thread[th][fn] += int(n)
    total = collections.Counter()
    for th, c in per_thread.items():
        total.update(c)
    for th, c in sorted(per_thread.items(), key=lambda kv: -sum(kv[1].values())):
        s = sum(c.values())
        print('\n== %s: %d samples' % (th, s))
        for fn, n in c.most_common(top):
            print('  %6d %5.1f%%  %s' % (n, 100.0 * n / s, fn))
    s = sum(total.values())
    print('\n== all threads: %d samples' % s)
    for fn, n in total.most_common(top):
        print('  %6d %5.1f%%  %s' % (n, 100.0 * n / s, fn))


main()
