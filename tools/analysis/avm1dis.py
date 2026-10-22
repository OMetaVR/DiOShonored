#!/usr/bin/env python3
"""Minimal AVM1 (ActionScript 2) disassembler for GFx/SWF movies extracted under private/.
python3 -I avm1dis.py <movie.gfx|swf> [--grep WORD]   prints DoAction / DoInitAction blocks whose constant pool contains WORD."""
import sys, struct

NAMES = {0x04:'nextFrame',0x05:'prevFrame',0x06:'play',0x07:'stop',0x0A:'add',0x0B:'subtract',0x0C:'multiply',0x0D:'divide',
 0x0E:'equals',0x0F:'less',0x10:'and',0x11:'or',0x12:'not',0x13:'stringEquals',0x14:'stringLength',0x15:'stringExtract',
 0x17:'pop',0x18:'toInteger',0x1C:'getVariable',0x1D:'setVariable',0x20:'setTarget2',0x21:'stringAdd',0x22:'getProperty',
 0x23:'setProperty',0x24:'cloneSprite',0x25:'removeSprite',0x26:'trace',0x27:'startDrag',0x28:'endDrag',0x29:'stringLess',
 0x2A:'throw',0x2B:'castOp',0x2C:'implementsOp',0x30:'randomNumber',0x31:'mbStringLength',0x32:'charToAscii',0x33:'asciiToChar',
 0x34:'getTime',0x35:'mbStringExtract',0x36:'mbCharToAscii',0x37:'mbAsciiToChar',0x3A:'delete',0x3B:'delete2',0x3C:'defineLocal',
 0x3D:'callFunction',0x3E:'return',0x3F:'modulo',0x40:'newObject',0x41:'defineLocal2',0x42:'initArray',0x43:'initObject',
 0x44:'typeOf',0x45:'targetPath',0x46:'enumerate',0x47:'add2',0x48:'less2',0x49:'equals2',0x4A:'toNumber',0x4B:'toString',
 0x4C:'pushDuplicate',0x4D:'stackSwap',0x4E:'getMember',0x4F:'setMember',0x50:'increment',0x51:'decrement',0x52:'callMethod',
 0x53:'newMethod',0x54:'instanceOf',0x55:'enumerate2',0x60:'bitAnd',0x61:'bitOr',0x62:'bitXor',0x63:'bitLShift',0x64:'bitRShift',
 0x65:'bitURShift',0x66:'strictEquals',0x67:'greater',0x68:'stringGreater',0x69:'extends',
 0x81:'gotoFrame',0x83:'getURL',0x87:'storeRegister',0x88:'constantPool',0x8A:'waitForFrame',0x8B:'setTarget',0x8C:'goToLabel',
 0x8D:'waitForFrame2',0x8E:'defineFunction2',0x8F:'try',0x94:'with',0x96:'push',0x99:'jump',0x9A:'getURL2',0x9B:'defineFunction',
 0x9D:'if',0x9E:'call',0x9F:'gotoFrame2'}

def cstr(b, o):
    e = b.index(b'\0', o); return b[o:e].decode('latin-1'), e + 1

def dis(b, start, end, out, ind=''):
    pool = []; p = start
    while p < end:
        op = b[p]; at = p; p += 1; ln = 0
        if op >= 0x80: ln = struct.unpack_from('<H', b, p)[0]; p += 2
        a = b[p:p + ln]; nm = NAMES.get(op, 'op%02x' % op); s = ''
        if op == 0x88:
            n = struct.unpack_from('<H', a, 0)[0]; q = 2; pool = []
            for _ in range(n): x, q = cstr(a, q); pool.append(x)
            s = '%d strings' % n
        elif op == 0x96:
            q = 0; items = []
            while q < ln:
                t = a[q]; q += 1
                if t == 0: x, q = cstr(a, q); items.append(repr(x))
                elif t == 1: items.append('%g' % struct.unpack_from('<f', a, q)[0]); q += 4
                elif t == 2: items.append('null')
                elif t == 3: items.append('undef')
                elif t == 4: items.append('r%d' % a[q]); q += 1
                elif t == 5: items.append('true' if a[q] else 'false'); q += 1
                elif t == 6: v = struct.unpack_from('<d', a[q+4:q+8] + a[q:q+4])[0]; items.append('%g' % v); q += 8
                elif t == 7: items.append('%d' % struct.unpack_from('<i', a, q)[0]); q += 4
                elif t == 8: items.append(repr(pool[a[q]]) if a[q] < len(pool) else 'c%d' % a[q]); q += 1
                elif t == 9: i = struct.unpack_from('<H', a, q)[0]; items.append(repr(pool[i]) if i < len(pool) else 'c%d' % i); q += 2
                else: items.append('?%d' % t); break
            s = ', '.join(items)
        elif op in (0x99, 0x9D):
            s = '-> %x' % (p + ln + struct.unpack_from('<h', a, 0)[0] - start)
        elif op == 0x87: s = 'r%d' % a[0]
        elif op == 0x8E or op == 0x9B:
            fn, q = cstr(a, 0); n = struct.unpack_from('<H', a, q)[0]; q += 2
            if op == 0x8E:
                q += 3; params = []
                for _ in range(n): r = a[q]; x, q = cstr(a, q + 1); params.append('%s(r%d)' % (x, r))
            else:
                params = []
                for _ in range(n): x, q = cstr(a, q); params.append(x)
            size = struct.unpack_from('<H', a, q)[0]
            out.append('%s%04x function %s(%s) [%d bytes]' % (ind, at - start, fn, ', '.join(params), size))
            p += ln
            dis(b, p, p + size, out, ind + '    ') if False else None
            # function bodies share the outer constant pool
            sub = []; _dis_body(b, p, p + size, sub, ind + '    ', pool)
            out.extend(sub); p += size; continue
        elif op == 0x8C: s = cstr(a, 0)[0]
        out.append('%s%04x %s %s' % (ind, at - start, nm, s))
        p += ln
    return pool

def _dis_body(b, start, end, out, ind, pool):
    # same as dis() but with an inherited constant pool
    global_pool = pool
    p = start
    while p < end:
        op = b[p]; at = p; p += 1; ln = 0
        if op >= 0x80: ln = struct.unpack_from('<H', b, p)[0]; p += 2
        if op in (0x8E, 0x9B, 0x96):
            # reuse dis for a single instruction by temporarily slicing
            tmp = []
            saved = list(global_pool)
            _single(b, at, tmp, ind, saved, start)
            out.extend(tmp)
            if op in (0x8E, 0x9B):
                a = b[at+3:at+3+ln]; fn, q = cstr(a, 0); n = struct.unpack_from('<H', a, q)[0]; q += 2
                if op == 0x8E:
                    q += 3
                    for _ in range(n): q = cstr(a, q + 1)[1]
                else:
                    for _ in range(n): q = cstr(a, q)[1]
                size = struct.unpack_from('<H', a, q)[0]
                p = at + 3 + ln
                _dis_body(b, p, p + size, out, ind + '    ', global_pool); p += size; continue
            p = at + 3 + ln; continue
        a = b[p:p + ln]; nm = NAMES.get(op, 'op%02x' % op); s = ''
        if op in (0x99, 0x9D): s = '-> %x' % (p + ln + struct.unpack_from('<h', a, 0)[0] - start)
        elif op == 0x87: s = 'r%d' % a[0]
        out.append('%s%04x %s %s' % (ind, at - start, nm, s)); p += ln

def _single(b, at, out, ind, pool, base):
    op = b[at]; ln = struct.unpack_from('<H', b, at + 1)[0]; a = b[at + 3: at + 3 + ln]
    if op == 0x96:
        q = 0; items = []
        while q < ln:
            t = a[q]; q += 1
            if t == 0: x, q = cstr(a, q); items.append(repr(x))
            elif t == 1: items.append('%g' % struct.unpack_from('<f', a, q)[0]); q += 4
            elif t == 2: items.append('null')
            elif t == 3: items.append('undef')
            elif t == 4: items.append('r%d' % a[q]); q += 1
            elif t == 5: items.append('true' if a[q] else 'false'); q += 1
            elif t == 6: v = struct.unpack_from('<d', a[q+4:q+8] + a[q:q+4])[0]; items.append('%g' % v); q += 8
            elif t == 7: items.append('%d' % struct.unpack_from('<i', a, q)[0]); q += 4
            elif t == 8: items.append(repr(pool[a[q]]) if a[q] < len(pool) else 'c%d' % a[q]); q += 1
            elif t == 9: i = struct.unpack_from('<H', a, q)[0]; items.append(repr(pool[i]) if i < len(pool) else 'c%d' % i); q += 2
            else: items.append('?%d' % t); break
        out.append('%s%04x push %s' % (ind, at - base, ', '.join(items)))
    else:
        fn, q = cstr(a, 0); n = struct.unpack_from('<H', a, q)[0]; q += 2; params = []
        if op == 0x8E:
            q += 3
            for _ in range(n): r = a[q]; x, q = cstr(a, q + 1); params.append('%s(r%d)' % (x, r))
        else:
            for _ in range(n): x, q = cstr(a, q); params.append(x)
        out.append('%s%04x function %s(%s)' % (ind, at - base, fn, ', '.join(params)))

def main():
    b = open(sys.argv[1], 'rb').read()[8:]
    word = sys.argv[sys.argv.index('--grep') + 1] if '--grep' in sys.argv else None
    nbits = b[0] >> 3; p = (5 + 4 * nbits + 7) // 8 + 4
    while p < len(b):
        h = struct.unpack_from('<H', b, p)[0]; p += 2; t = h >> 6; l = h & 63
        if l == 63: l = struct.unpack_from('<I', b, p)[0]; p += 4
        if t in (12, 59):
            q = p + (2 if t == 59 else 0)
            out = []
            dis(b, q, p + l, out)
            text = '\n'.join(out)
            if not word or word in text:
                print('==== tag %d at %x (%d bytes)' % (t, p, l)); print(text)
        p += l
        if t == 0: break

main()
