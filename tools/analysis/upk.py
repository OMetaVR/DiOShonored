#!/usr/bin/env python3
"""UE3 package (Dishonored, file version 801) reader for debugging: LZO1X chunk decompression, name/import/export tables.
python3 -I upk.py <file.upk> [--find NAME] [--dump-header OUT]   (prints to stdout; never writes game data outside private/)"""
import sys, struct

def lzo1x_decompress(src, out_len):
    """Reference LZO1X decompressor (lzo1x_decompress_safe semantics)."""
    out = bytearray(); ip = 0; n = len(src)
    def lit(k):
        nonlocal ip
        out.extend(src[ip:ip + k]); ip += k
    def ext_len(base):
        nonlocal ip
        t = 0
        while src[ip] == 0: t += 255; ip += 1
        t += src[ip]; ip += 1
        return base + t
    def copy_match(dist, k):
        s = len(out) - dist
        if s < 0: raise ValueError('lookbehind overrun at out %d' % len(out))
        for i in range(k): out.append(out[s + i])
    state = 0
    t = src[ip]
    if t > 17:
        ip += 1; t -= 17; lit(t); state = 4 if t >= 4 else t
    while ip < n:
        t = src[ip]; ip += 1
        if t < 16:
            if state == 0:
                k = ext_len(15) if t == 0 else t
                lit(k + 3); state = 4
                continue
            elif state < 4:
                dist = 1 + (t >> 2) + (src[ip] << 2); ip += 1
                copy_match(dist, 2); state = t & 3; lit(state); continue
            else:
                dist = 2049 + (t >> 2) + (src[ip] << 2); ip += 1
                copy_match(dist, 3); state = t & 3; lit(state); continue
        if t >= 64:
            dist = 1 + ((t >> 2) & 7) + (src[ip] << 3); ip += 1
            k = (t >> 5) - 1 + 2
        elif t >= 32:
            k = ext_len(31) if (t & 31) == 0 else (t & 31)
            k += 2
            v = src[ip] | (src[ip + 1] << 8); ip += 2
            dist = 1 + (v >> 2)
        else:
            k = ext_len(7) if (t & 7) == 0 else (t & 7)
            k += 2
            v = src[ip] | (src[ip + 1] << 8); ip += 2
            dist = ((t & 8) << 11) + (v >> 2)
            if dist == 0:  # end of stream
                break
            dist += 16384
        copy_match(dist, k)
        state = v & 3 if t < 64 else src[ip - 1] & 3 if False else None
        if t < 64: state = v & 3
        else: state = t & 3
        lit(state)
    if len(out) != out_len: raise ValueError('decompressed %d, expected %d' % (len(out), out_len))
    return bytes(out)

def fstring(d, o):
    n = struct.unpack_from('<i', d, o)[0]; o += 4
    if n < 0: return d[o:o - 2 * n].decode('utf-16-le').rstrip('\0'), o - 2 * n
    return d[o:o + n].decode('latin-1').rstrip('\0'), o + n

def read_package(path):
    d = open(path, 'rb').read()
    o = 12
    _, o = fstring(d, o)
    flags, nc, no, ec, eo, ic, io, dep = struct.unpack_from('<IIIIIIIi', d, o); o += 32
    # import/export guids offset, guid counts, thumbnail table, package guid, generations, engine/cooker version
    o += 4 * 4 + 16
    gens = struct.unpack_from('<i', d, o)[0]; o += 4 + 12 * gens
    eng, cook, cflags, nchunks = struct.unpack_from('<IIII', d, o); o += 16
    chunks = [struct.unpack_from('<IIII', d, o + 16 * i) for i in range(nchunks)]
    if not (flags & 0x02000000) or not chunks:
        return d, dict(nc=nc, no=no, ec=ec, eo=eo, ic=ic, io=io)
    total = max(u + us for u, us, c, cs in chunks)
    full = bytearray(total)
    full[:chunks[0][0]] = d[:chunks[0][0]]
    for uoff, usize, coff, csize in chunks:
        tag, bs, ctot, utot = struct.unpack_from('<IIII', d, coff)
        nb = (utot + bs - 1) // bs
        blocks = [struct.unpack_from('<II', d, coff + 16 + 8 * i) for i in range(nb)]
        p = coff + 16 + 8 * nb; w = uoff
        for bcs, bus in blocks:
            full[w:w + bus] = lzo1x_decompress(d[p:p + bcs], bus)
            p += bcs; w += bus
    return bytes(full), dict(nc=nc, no=no, ec=ec, eo=eo, ic=ic, io=io, cflags=cflags, chunks=chunks)

def names(d, h):
    o = h['no']; out = []
    for _ in range(h['nc']):
        s, o = fstring(d, o); o += 8; out.append(s)
    return out

if __name__ == '__main__':
    d, h = read_package(sys.argv[1])
    nm = names(d, h)
    print('names %d exports %d imports %d (compression %s)' % (h['nc'], h['ec'], h['ic'], h.get('cflags')))
    if '--find' in sys.argv:
        q = sys.argv[sys.argv.index('--find') + 1]
        print('name index of %s:' % q, [i for i, x in enumerate(nm) if x == q])
    if '--dump-header' in sys.argv:
        open(sys.argv[sys.argv.index('--dump-header') + 1], 'wb').write(d[:max(c[0] + c[1] for c in h.get('chunks', [(0, len(d), 0, 0)]))])
