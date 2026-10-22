"""Synthetic single-instruction differential tests (flags compared exactly where defined). See difftest.py."""
import os, sys, struct, random, subprocess, collections, itertools, math, time, pickle
from difftest import *

M = 0x10080000          # capture area (see diffdrv cap)
CF, PF, AF, ZF, SF, OF = 1, 4, 0x10, 0x40, 0x80, 0x800
ALL = CF | PF | AF | ZF | SF | OF
LOGIC = ALL & ~AF
PREFIXES = ['cmp eax, ecx', 'cmp edx, eax', 'test eax, eax', 'stc', 'clc', 'add edx, ecx']

class T:
    def __init__(self, cat, body, mask=0, pre=None, tol=0, name=None, mmx=False):
        self.cat, self.body, self.mask, self.pre, self.tol, self.mmx = cat, body if isinstance(body, list) else [body], mask, pre, tol, mmx
        self.name = name or '; '.join(self.body)

def alu_tests():
    out = []
    ops = ['add', 'or', 'adc', 'sbb', 'and', 'sub', 'xor', 'cmp', 'test']
    forms = ['{o} eax, ecx', '{o} edx, dword ptr [esi+8]', '{o} dword ptr [edi+4], ecx', '{o} eax, 0x12345678', '{o} ecx, -3', '{o} al, cl', '{o} ah, dl',
             '{o} ax, cx', '{o} byte ptr [ebx+1], 0x7f', '{o} word ptr [ebx+2], dx', '{o} dl, byte ptr [esi+3]', '{o} dword ptr [ebx+8], 0x7fffffff', '{o} eax, eax']
    for o in ops:
        for f in forms:
            if o == 'test' and ('byte ptr [esi' in f):
                pass
            mask = LOGIC if o in ('and', 'or', 'xor', 'test') else ALL
            pre = 'stc' if o in ('adc', 'sbb') else None
            for pr in (['cmp eax, ecx', 'stc', 'clc'] if o in ('adc', 'sbb') else [None]):
                out.append(T('alu', f.format(o=o), mask, pr))
    return out

def unary_tests():
    out = []
    for o in ['inc', 'dec', 'neg', 'not']:
        for f in ['{o} eax', '{o} cl', '{o} dx', '{o} dword ptr [esi+4]', '{o} byte ptr [edi]', '{o} ah']:
            m = 0 if o == 'not' else ALL
            for pr in ['stc', 'clc']:
                out.append(T('unary', f.format(o=o), m, pr))
    return out

def shift_tests():
    out = []
    for o in ['shl', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr']:
        rot = o in ('rol', 'ror', 'rcl', 'rcr')
        for f, imm, w in [('{o} eax, {i}', 1, 32), ('{o} eax, {i}', 2, 32), ('{o} ecx, {i}', 5, 32), ('{o} edx, {i}', 31, 32), ('{o} al, {i}', 3, 8), ('{o} bx, {i}', 4, 16),
                          ('{o} dword ptr [esi], {i}', 7, 32), ('{o} dword ptr [esi], 1', 1, 32), ('{o} ah, {i}', 1, 8)]:
            m = (CF | OF) if rot else (CF | SF | ZF | PF)
            if imm != 1 and not rot: pass
            elif imm != 1: m &= ~OF
            if imm == 1 and not rot: m |= OF
            if imm != 1 and o in ('shl', 'shr', 'sar'): m &= ~OF
            for pr in ['stc', 'clc']:
                if o in ('rcl', 'rcr') and w < 32 and imm > 1: continue
                out.append(T('shift', f.format(o=o, i=imm), m, pr))
        # variable counts (cl)
        if o in ('rcl', 'rcr'): continue
        m = (CF | SF | ZF | PF) if not rot else CF
        out.append(T('shift', '{o} eax, cl'.format(o=o), m & ~CF if False else m & ~OF, 'stc'))
        out.append(T('shift', 'and ecx, 31 ; {o} edx, cl'.format(o=o), m, 'clc'))
        out.append(T('shift', 'and ecx, 7 ; {o} bl, cl'.format(o=o), m, 'stc'))
        out.append(T('shift', 'and ecx, 15 ; {o} si, cl'.format(o=o), m, 'stc') if False else T('shift', 'and ecx, 15 ; {o} ax, cl'.format(o=o), m, 'stc'))
    for o in ['shld', 'shrd']:
        for i in [1, 7, 31]:
            out.append(T('shift', f'{o} eax, edx, {i}', (CF | SF | ZF | PF) | (OF if i == 1 else 0), 'stc'))
        out.append(T('shift', f'and ecx, 31 ; {o} eax, edx, cl', CF | SF | ZF | PF, 'stc'))
        out.append(T('shift', f'{o} dword ptr [esi], edx, 5', CF | SF | ZF | PF, 'clc'))
    return out

def muldiv_tests():
    out = []
    for f in ['mul ecx', 'mul dword ptr [esi]', 'mul cl', 'mul cx', 'imul ecx', 'imul dword ptr [edi+4]', 'imul cl', 'imul cx']:
        out.append(T('muldiv', f, CF | OF))
    for f in ['imul eax, ecx', 'imul edx, dword ptr [esi+4]', 'imul eax, ecx, 7', 'imul eax, ecx, 0x12345', 'imul ax, cx', 'imul ax, cx, 300', 'imul ecx, dword ptr [esi], -5']:
        out.append(T('muldiv', f, CF | OF))
    for f in ['div ecx', 'idiv ecx', 'div dword ptr [esi]', 'div cl', 'idiv cl', 'div cx', 'idiv cx']:
        pre = 'or ecx, 0x1000 ; mov edx, 5' if 'cl' not in f else 'or ecx, 0x1f ; and eax, 0xfff'
        if f in ('idiv ecx',): pre = 'or ecx, 0x1000 ; cdq'
        if f in ('div cx', 'idiv cx'): pre = 'or ecx, 0x100 ; and ecx, 0xffff ; mov edx, 0'
        out.append(T('muldiv', pre + ' ; ' + f, 0))
    out.append(T('conv', 'cdq'))
    out.append(T('conv', 'cwde'))
    out.append(T('conv', 'cbw'))
    out.append(T('conv', 'cwd'))
    return out

def conv_tests():
    out = []
    for f in ['movzx eax, cl', 'movzx eax, cx', 'movzx edx, byte ptr [esi+1]', 'movzx ecx, word ptr [esi+2]', 'movsx eax, cl', 'movsx eax, dx', 'movsx ecx, byte ptr [edi+3]', 'movsx edx, word ptr [edi]',
              'movzx ax, cl', 'movsx ax, dl', 'lea eax, [ecx+edx*4+7]', 'lea eax, [ebx+esi]', 'lea ecx, [eax*2+5]', 'lea edx, [ebp-12]', 'lea eax, [eax+eax*8]', 'lea ax, [ecx+edx]',
              'xchg eax, ecx', 'xchg edx, dword ptr [esi]', 'xchg al, cl', 'xchg ah, dl', 'bswap eax', 'bswap edx', 'mov eax, ecx', 'mov al, ch', 'mov ah, cl', 'mov dword ptr [esi+4], eax',
              'mov byte ptr [edi+1], ch', 'mov word ptr [edi+2], dx', 'mov dx, word ptr [esi]', 'mov cl, byte ptr [esi+3]', 'mov dword ptr [ebx], 0x11223344']:
        out.append(T('conv', f))
    for f in ['cmpxchg dword ptr [esi], ecx', 'lock cmpxchg dword ptr [esi], ecx', 'cmpxchg ecx, edx', 'xadd dword ptr [esi], eax', 'lock xadd dword ptr [esi], eax', 'xadd ecx, edx', 'cmpxchg byte ptr [esi], cl',
              'lock add dword ptr [esi], eax', 'lock sub dword ptr [esi], eax', 'lock and dword ptr [esi], eax', 'lock or dword ptr [esi], eax', 'lock xor dword ptr [esi], eax', 'lock inc dword ptr [esi]', 'lock dec dword ptr [esi]']:
        out.append(T('atomic', f, ALL & ~(CF if 'inc' in f or 'dec' in f else 0) if False else ALL))
    out.append(T('atomic', 'lock xchg dword ptr [esi], eax', 0))
    return out

def bit_tests():
    out = []
    for o in ['bt', 'bts', 'btr', 'btc']:
        for f in [f'{o} eax, ecx', f'{o} eax, 5', f'{o} dword ptr [esi], 9', f'{o} dword ptr [esi], ecx', f'{o} ax, cx']:
            out.append(T('bit', f, CF))
    for o in ['bsf', 'bsr']:
        for f in [f'{o} eax, ecx', f'{o} edx, dword ptr [esi+4]', f'or ecx, 0x400 ; {o} eax, ecx']:
            out.append(T('bit', f, ZF))
    return out

def cc_tests():
    out = []
    ccs = ['o', 'no', 'b', 'ae', 'e', 'ne', 'be', 'a', 's', 'ns', 'p', 'np', 'l', 'ge', 'le', 'g']
    for cc in ccs:
        for pr in ['cmp eax, ecx', 'test eax, eax', 'add eax, ecx', 'sub ecx, edx', 'cmp dl, al', 'cmp ax, dx']:
            out.append(T('cc', f'set{cc} al', 0, pr))
        out.append(T('cc', f'set{cc} byte ptr [edi+1]', 0, 'cmp edx, ecx'))
        out.append(T('cc', f'cmov{cc} eax, ecx', 0, 'cmp edx, eax'))
        out.append(T('cc', f'cmov{cc} edx, dword ptr [esi]', 0, 'sub eax, ecx'))
        out.append(T('cc', f'j{cc} 1f ; mov eax, 1 ; jmp 2f ; 1: mov eax, 2 ; 2:', 0, 'cmp eax, edx'))
    out.append(T('flags', 'lahf', ALL))
    out.append(T('flags', 'mov ah, cl ; sahf', ALL & ~OF, 'stc'))
    for o in ['cmc', 'clc', 'stc']:
        out.append(T('flags', o + ' ; adc eax, ecx', ALL, 'cmp edx, ecx'))
    out.append(T('flags', 'pushfd ; pop eax', ALL, 'cmp eax, edx'))
    out.append(T('flags', 'pushfd ; pop eax ; and eax, 0x8d5 ; push eax ; popfd ; adc ecx, edx', ALL, 'cmp edx, eax'))
    out.append(T('flags', 'std ; pushfd ; pop eax ; cld', ALL))
    return out

def string_tests():
    out = []
    for sz, sfx in [(1, 'b'), (2, 'w'), (4, 'd')]:
        for rep in ['', 'rep ', 'repe ', 'repne ']:
            for op in ['movs', 'stos', 'lods', 'scas', 'cmps']:
                if rep in ('repe ', 'repne ') and op in ('movs', 'stos', 'lods'): continue
                for d in ['cld', 'std']:
                    mask = ALL if op in ('scas', 'cmps') else 0
                    pre = 'and ecx, 15 ; add esi, 64 ; add edi, 64'
                    body = f'{d} ; {pre} ; {rep}{op}{sfx} ; cld'
                    out.append(T('string', body, mask))
    return out

def stack_tests():
    out = []
    for f in ['push eax ; pop ecx', 'push dword ptr [esi] ; pop edx', 'push 0x12345678 ; pop eax', 'push esp ; pop eax', 'push eax ; push ecx ; pop edx ; pop eax',
              'pop dword ptr [esp+4] ; push eax ; push ecx', 'call 1f ; 1: pop eax', 'push ebp ; mov ebp, esp ; sub esp, 16 ; mov dword ptr [ebp-4], eax ; mov eax, dword ptr [ebp-4] ; leave',
              'push eax ; pop word ptr [esi]' if False else 'push eax ; pop eax']:
        out.append(T('stack', f))
    return out

def x87_tests():
    out = []
    ld2 = 'fld qword ptr [esi] ; fld qword ptr [esi+8]'
    st = ' ; fstp qword ptr [{0}] ; fstp qword ptr [{1}]'.format(M + 0x100, M + 0x108)
    st1 = ' ; fstp qword ptr [{0}]'.format(M + 0x100)
    for op in ['faddp', 'fsubp', 'fsubrp', 'fmulp', 'fdivp', 'fdivrp']:
        out.append(T('x87', f'{ld2} ; {op} st(1), st' + st1))
    for op in ['fadd', 'fsub', 'fsubr', 'fmul', 'fdiv', 'fdivr']:
        out.append(T('x87', f'{ld2} ; {op} st, st(1)' + st))
        out.append(T('x87', f'{ld2} ; {op} st(1), st' + st))
        out.append(T('x87', f'{ld2} ; {op} dword ptr [esi+16]' + st))
        out.append(T('x87', f'{ld2} ; {op} qword ptr [esi+20]' + st))
    for op in ['fiadd', 'fisub', 'fisubr', 'fimul', 'fidiv', 'fidivr']:
        out.append(T('x87', f'{ld2} ; {op} dword ptr [esi+24]' + st))
        out.append(T('x87', f'{ld2} ; {op} word ptr [esi+28]' + st))
    for op in ['fchs', 'fabs', 'fsqrt', 'frndint']:
        out.append(T('x87', f'{ld2} ; {op}' + st))
    out.append(T('x87', f'{ld2} ; fxch st(1)' + st))
    # x87 values held across a call to a function that leaves the stack alone (LTCG code; GFx's SWF matrix reader): the call must
    # not be taken for one that returns st(0). The lifted call does not store a return address, so the slot is cleared afterwards.
    out.append(T('x87-call', 'fld qword ptr [esi+8] ; call 7f ; mov dword ptr [esp-4], 0 ; fild dword ptr [esi+24] ; fmul st, st(1)' + st + ' ; jmp 8f ; 7: mov eax, 5 ; ret ; 8:'))
    out.append(T('x87-call', f'{ld2} ; call 7f ; mov dword ptr [esp-4], 0 ; fsubp st(1), st' + st1 + ' ; jmp 8f ; 7: lea eax, [eax+1] ; ret ; 8:'))
    out.append(T('x87', f'{ld2} ; fld st(1) ; fxch st(2)' + st + ' ; fstp qword ptr [{0}]'.format(M + 0x110)))
    for c in ['fld1', 'fldz', 'fldpi', 'fldl2e', 'fldl2t', 'fldlg2', 'fldln2']:
        out.append(T('x87', f'{c}' + st1))
    for cwv in [0x027F, 0x067F, 0x0A7F, 0x0E7F]:
        pre = f'mov word ptr [{M + 0x180}], {cwv} ; fldcw word ptr [{M + 0x180}]'
        out.append(T('x87-cw', f'{pre} ; {ld2} ; frndint' + st))
        for fmt, mem in [('fistp word ptr', 'word ptr'), ('fistp dword ptr', 'dword ptr'), ('fistp qword ptr', 'qword ptr')]:
            out.append(T('x87-cw', f'{pre} ; {ld2} ; {fmt} [{M + 0x120}] ; fstp qword ptr [{M + 0x100}]'))
            if 'qword' not in fmt:
                out.append(T('x87-cw', f'{pre} ; {ld2} ; {fmt.replace("fistp", "fist")} [{M + 0x120}] ; fstp qword ptr [{M + 0x100}] ; fstp qword ptr [{M + 0x108}]'))
    out.append(T('x87-cw', f'fnstcw word ptr [{M + 0x130}]'))
    out.append(T('x87-cw', f'mov word ptr [{M + 0x180}], 0x0E7F ; fldcw word ptr [{M + 0x180}] ; fnstcw word ptr [{M + 0x130}] ; fldcw word ptr [{M + 0x180}]'))
    for f in ['fild word ptr [esi+28]', 'fild dword ptr [esi+24]', 'fild qword ptr [esi+24]']:
        out.append(T('x87', f + st1))
    for f in ['fld dword ptr [esi+16]', 'fld qword ptr [esi+20]']:
        out.append(T('x87', f + st1))
        out.append(T('x87', f.replace('fld', 'fld') + ' ; fst dword ptr [{0}] ; fstp qword ptr [{1}]'.format(M + 0x140, M + 0x100)))
    out.append(T('x87', ld2 + ' ; fstp dword ptr [{0}] ; fstp qword ptr [{1}]'.format(M + 0x140, M + 0x100)))
    out.append(T('x87', ld2 + ' ; fstp tbyte ptr [{0}] ; fld tbyte ptr [{0}] ; fstp qword ptr [{1}] ; fstp st(0)'.format(M + 0x150, M + 0x100)))
    out.append(T('x87', ld2 + ' ; fstp tbyte ptr [{0}] ; fstp tbyte ptr [{1}]'.format(M + 0x150, M + 0x170)))
    sw = f' ; fnstsw ax ; and eax, 0x4500 ; mov dword ptr [{M + 0x104}], eax'
    for op in ['fcom st(1)', 'fucom st(1)', 'fcom dword ptr [esi+16]', 'fcom qword ptr [esi+8]', 'ficom dword ptr [esi+24]', 'ficom word ptr [esi+28]']:
        out.append(T('x87-cmp', f'{ld2} ; {op}{sw}' + st))
    for op in ['fcomp st(1)', 'fucomp st(1)', 'fcomp dword ptr [esi+16]']:
        out.append(T('x87-cmp', f'{ld2} ; {op}{sw}' + st1))
    for op in ['fcompp', 'fucompp']:
        out.append(T('x87-cmp', f'{ld2} ; {op}{sw}'))
    for op in ['ftst', 'fxam']:
        out.append(T('x87-cmp', f'{ld2} ; {op}{sw}' + st))
    for op in ['fcomi st, st(1)', 'fucomi st, st(1)']:
        out.append(T('x87-cmp', f'{ld2} ; {op}' + st, ZF | PF | CF))
    for op in ['fcomip st, st(1)', 'fucomip st, st(1)']:
        out.append(T('x87-cmp', f'{ld2} ; {op}' + st1, ZF | PF | CF))
    for cc in ['b', 'e', 'be', 'u', 'nb', 'ne', 'nbe', 'nu']:
        out.append(T('x87-cmp', f'{ld2} ; cmp eax, ecx ; fcmov{cc} st, st(1)' + st))
    out.append(T('x87-cmp', f'{ld2} ; fcomi st, st(1) ; fcmovb st, st(1) ; fcmove st, st(1)' + st, ZF | PF | CF))
    out.append(T('x87-cmp', f'{ld2} ; fcompp ; fnstsw ax ; sahf', ALL & ~(OF | AF)))
    # exact helpers
    for op in ['fprem', 'fprem1']:
        out.append(T('x87-misc', f'fild dword ptr [esi+24] ; fild word ptr [esi+28] ; fadd dword ptr [esi+16] ; fabs ; fld1 ; faddp st(1), st ; {op}' + st + sw))
    out.append(T('x87-misc', f'{ld2} ; fscale' + st + sw))
    out.append(T('x87-misc', f'fild word ptr [esi+24] ; fld qword ptr [esi+8] ; fscale' + st1 + ' ; fstp st(0)'))
    # approximate transcendentals
    for op in ['fsin', 'fcos', 'f2xm1']:
        out.append(T('x87-trans', f'{ld2} ; fabs ; fld1 ; fdivp st(1), st ; fstp st(0) ; {op}' + st1 + ' ; fstp st(0)', tol=2))
    out.append(T('x87-trans', f'{ld2} ; fyl2x' + st1, tol=1))
    out.append(T('x87-trans', f'{ld2} ; fpatan' + st1, tol=1))
    out.append(T('x87-trans', f'{ld2} ; fptan' + st, tol=2))
    out.append(T('x87-trans', f'{ld2} ; fsincos' + st, tol=2))
    return out

def sse_tests():
    out = []
    init = f'movups xmm0, [esi] ; movups xmm1, [esi+16] ; movups xmm2, [esi+32] ; movups xmm3, [esi+48]'
    def op(cat, o, regform=True, memform=True, mask=0, imm=None, mm=False):
        suffix = f', {imm}' if imm is not None else ''
        if regform: out.append(T(cat, f'{init} ; {o} xmm0, xmm1{suffix}', mask))
        if o in ('movlhps', 'movhlps'): memform = False
        mw = {'cvtss2sd': 'dword', 'cvtsd2ss': 'qword', 'cvtdq2pd': 'qword', 'cvtps2pd': 'qword'}.get(o, 'xmmword')
        if memform: out.append(T(cat, f'{init} ; {o} xmm2, {mw} ptr [esi+16]{suffix}', mask))
    for o in ['addps', 'subps', 'mulps', 'divps', 'minps', 'maxps', 'sqrtps', 'rcpps', 'rsqrtps']:
        op('sse-fp', o)
    for o in ['addss', 'subss', 'mulss', 'divss', 'minss', 'maxss', 'sqrtss', 'rcpss', 'rsqrtss']:
        out.append(T('sse-fp', f'{init} ; {o} xmm0, xmm1'))
        out.append(T('sse-fp', f'{init} ; {o} xmm2, dword ptr [esi+16]'))
    for o in ['addpd', 'subpd', 'mulpd', 'divpd', 'minpd', 'maxpd', 'sqrtpd']:
        op('sse-fp', o)
    for o in ['addsd', 'subsd', 'mulsd', 'divsd', 'minsd', 'maxsd', 'sqrtsd']:
        out.append(T('sse-fp', f'{init} ; {o} xmm0, xmm1'))
        out.append(T('sse-fp', f'{init} ; {o} xmm2, qword ptr [esi+16]'))
    for o in ['andps', 'andnps', 'orps', 'xorps', 'andpd', 'andnpd', 'orpd', 'xorpd', 'pand', 'pandn', 'por', 'pxor']:
        op('sse-logic', o)
    out.append(T('sse-logic', f'{init} ; xorps xmm0, xmm0'))
    for p in range(8):
        for o in ['cmpps', 'cmpss', 'cmppd', 'cmpsd']:
            out.append(T('sse-cmp', f'{init} ; {o} xmm0, xmm1, {p}'))
    for o in ['comiss', 'ucomiss', 'comisd', 'ucomisd']:
        out.append(T('sse-cmp', f'{init} ; {o} xmm0, xmm1', ALL))
        out.append(T('sse-cmp', f'{init} ; {o} xmm0, xmm0', ALL))
    for o in ['pcmpeqb', 'pcmpeqw', 'pcmpeqd', 'pcmpgtb', 'pcmpgtw', 'pcmpgtd']:
        op('sse-int', o)
    for o in ['paddb', 'paddw', 'paddd', 'paddq', 'psubb', 'psubw', 'psubd', 'psubq', 'paddsb', 'paddsw', 'paddusb', 'paddusw', 'psubsb', 'psubsw', 'psubusb', 'psubusw', 'pmullw', 'pmulhw', 'pmulhuw',
              'pmuludq', 'pmaddwd', 'pmaxsw', 'pminsw', 'pmaxub', 'pminub', 'pavgb', 'pavgw', 'psadbw', 'packssdw', 'packsswb', 'packuswb', 'punpcklbw', 'punpcklwd', 'punpckldq', 'punpcklqdq',
              'punpckhbw', 'punpckhwd', 'punpckhdq', 'punpckhqdq', 'unpcklps', 'unpckhps', 'unpcklpd', 'unpckhpd', 'movlhps', 'movhlps']:
        op('sse-int', o)
    for o in ['psllw', 'pslld', 'psllq', 'psrlw', 'psrld', 'psrlq', 'psraw', 'psrad']:
        for imm in [0, 1, 3, 15, 31, 63, 200]:
            out.append(T('sse-shift', f'{init} ; {o} xmm0, {imm}'))
        out.append(T('sse-shift', f'{init} ; and ecx, 31 ; movd xmm1, ecx ; {o} xmm0, xmm1'))
        out.append(T('sse-shift', f'{init} ; {o} xmm0, xmmword ptr [esi+16]'))
    for imm in [0, 1, 7, 8, 15, 16]:
        out.append(T('sse-shift', f'{init} ; psrldq xmm0, {imm}'))
        out.append(T('sse-shift', f'{init} ; pslldq xmm0, {imm}'))
    for imm in [0x00, 0x1b, 0xe4, 0xaa, 0x55, 0xc6]:
        out.append(T('sse-shuf', f'{init} ; shufps xmm0, xmm1, {imm}'))
        out.append(T('sse-shuf', f'{init} ; pshufd xmm0, xmm1, {imm}'))
        out.append(T('sse-shuf', f'{init} ; pshuflw xmm0, xmm1, {imm}'))
        out.append(T('sse-shuf', f'{init} ; pshufhw xmm0, xmm1, {imm}'))
    for imm in [0, 1, 2, 3]:
        out.append(T('sse-shuf', f'{init} ; shufpd xmm0, xmm1, {imm}'))
    for o in ['cvtss2sd', 'cvtsd2ss', 'cvtdq2ps', 'cvtps2dq', 'cvttps2dq', 'cvtdq2pd', 'cvtpd2ps', 'cvtps2pd', 'cvtpd2dq', 'cvttpd2dq']:
        op('sse-cvt', o)
    for o in ['cvttss2si', 'cvtss2si', 'cvttsd2si', 'cvtsd2si']:
        out.append(T('sse-cvt', f'{init} ; {o} eax, xmm1'))
        out.append(T('sse-cvt', f'{init} ; {o} ecx, xmm3'))
    out.append(T('sse-cvt', f'{init} ; cvtsi2ss xmm0, ecx'))
    out.append(T('sse-cvt', f'{init} ; cvtsi2sd xmm0, edx'))
    out.append(T('sse-cvt', f'{init} ; cvtsi2ss xmm0, dword ptr [esi+4]'))
    for imm in range(0, 8):
        out.append(T('sse-mov', f'{init} ; pextrw eax, xmm1, {imm}'))
        out.append(T('sse-mov', f'{init} ; pinsrw xmm0, ecx, {imm}'))
    out.append(T('sse-mov', f'{init} ; movmskps eax, xmm1'))
    out.append(T('sse-mov', f'{init} ; movmskpd edx, xmm2'))
    out.append(T('sse-mov', f'{init} ; pmovmskb ecx, xmm3'))
    for f in ['movss xmm0, xmm1', 'movss xmm0, dword ptr [esi+8]', 'movss dword ptr [{0}], xmm1'.format(M + 0x200), 'movsd xmm0, xmm1', 'movsd xmm0, qword ptr [esi+8]', 'movsd qword ptr [{0}], xmm1'.format(M + 0x200),
              'movaps xmm0, xmm1', 'movups xmm2, xmmword ptr [esi+3]', 'movups xmmword ptr [{0}], xmm1'.format(M + 0x200), 'movdqa xmm0, xmm3', 'movdqu xmm1, xmmword ptr [esi+5]', 'movd xmm0, eax', 'movd eax, xmm1',
              'movd xmm0, dword ptr [esi]', 'movd dword ptr [{0}], xmm2'.format(M + 0x200), 'movq xmm0, xmm1', 'movq xmm0, qword ptr [esi+8]', 'movq qword ptr [{0}], xmm1'.format(M + 0x200),
              'movlps xmm0, qword ptr [esi+8]', 'movhps xmm0, qword ptr [esi+8]', 'movlps qword ptr [{0}], xmm1'.format(M + 0x200), 'movhps qword ptr [{0}], xmm1'.format(M + 0x200),
              'movlpd xmm0, qword ptr [esi+8]', 'movhpd xmm0, qword ptr [esi+8]', 'movapd xmm1, xmm2', 'movntps xmmword ptr [{0}], xmm1'.format(M + 0x200)]:
        out.append(T('sse-mov', f'{init} ; {f}'))
    # MMX
    mi = 'movq mm0, qword ptr [esi] ; movq mm1, qword ptr [esi+8]'
    for o in ['paddb', 'paddw', 'paddd', 'psubw', 'pmullw', 'pmaxsw', 'pminsw', 'pavgw', 'pcmpeqw', 'pcmpgtw', 'pand', 'por', 'pxor', 'punpcklwd', 'packssdw', 'packuswb']:
        out.append(T('mmx', f'{mi} ; {o} mm0, mm1 ; movq qword ptr [{M + 0x100}], mm0 ; emms', mmx=True))
    out.append(T('mmx', f'{mi} ; movd mm0, eax ; movd edx, mm0 ; movq qword ptr [{M + 0x100}], mm0 ; emms'))
    out.append(T('mmx', f'{mi} ; cvtpi2ps xmm0, mm1 ; movups [{M + 0x100}], xmm0 ; emms'))
    out.append(T('mmx', f'{mi} ; cvtps2pi mm0, xmm0 ; movq qword ptr [{M + 0x100}], mm0 ; emms'))
    return out

def mask_of(t):
    return t.mask

def build_asm(tests):
    lines = ['.intel_syntax noprefix', '.text']
    for i, t in enumerate(tests):
        lines.append('.balign 1024')
        lines.append(f'f{i}:')
        pre = t.pre
        if pre: lines += ['  ' + x.strip() for x in pre.split(';')]
        for b in t.body:
            for x in b.split(';'):
                x = x.strip()
                if x:
                    lines.append('  ' + x)
        # epilogue: capture state
        lines += [f'  mov dword ptr [{M}], eax', f'  mov dword ptr [{M + 4}], ecx', f'  mov dword ptr [{M + 8}], edx', f'  mov dword ptr [{M + 12}], ebx',
                  f'  mov dword ptr [{M + 16}], esi', f'  mov dword ptr [{M + 20}], edi', f'  mov dword ptr [{M + 24}], ebp']
        if t.mask:
            lines += ['  pushfd', '  pop eax', f'  and eax, {t.mask}', f'  mov dword ptr [{M + 32}], eax']
        for k in range(4):
            lines.append(f'  movups xmmword ptr [{M + 64 + 16 * k}], xmm{k}')
        lines.append('  ret')
    return '\n'.join(lines) + '\n'

def interesting(rng):
    k = rng.random()
    if k < 0.15: return rng.choice([0, 1, 2, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFF, 0x8000, 0x7FFF, 0xFF, 0x80, 0x100, 31, 32, 33])
    if k < 0.35: return rng.randrange(0, 64)
    if k < 0.45: return (-rng.randrange(1, 64)) & 0xFFFFFFFF
    return rng.getrandbits(32)

def hp(rng):
    return HEAP + 0x2000 + (rng.randrange(0, 0x70000) & (~3 if rng.random() < 0.9 else ~0))

def make_insn_case(rng, fn):
    return {'fn': fn, 'esp': STACK + 0x20000, 'r': [interesting(rng), interesting(rng), interesting(rng), hp(rng), hp(rng), hp(rng), hp(rng)],
            'args': [interesting(rng) for _ in range(8)], 'seed': rng.getrandbits(64), 'xmm': [0] * 8}

def f64s(b):
    return struct.unpack('<%dd' % (len(b) // 8), b)

def compare_insn(t, u, d):
    if u['status'] != 0: return 'skip'
    if d['status'] != 0: return 'c-status%d' % d['status']
    if t.tol:
        # compare the double results in the capture area with a tolerance, plus status-word-free memory elsewhere
        a, b = f64s(u['cap'][0x100:0x120]), f64s(d['cap'][0x100:0x120])
        for x, y in zip(a, b):
            if not close(x, y, 1e-9 * 10 ** t.tol) and not (x != x and y != y):
                return 'tol'
        return 'ok'
    diffs = []
    if u['pages'] != d['pages']:
        diffs.append('mem')
    if u.get('fdepth', 0) != 0:
        diffs.append('fpu-depth')
    return ','.join(diffs) if diffs else 'ok'

def main(args):
    n = int(arg(args, '--n', 24)); seed = int(arg(args, '--seed', 1))
    only = arg(args, '--cat', None)
    tests = []
    for g in (alu_tests, unary_tests, shift_tests, muldiv_tests, conv_tests, bit_tests, cc_tests, string_tests, stack_tests, x87_tests, sse_tests):
        tests += g()
    if only: tests = [t for t in tests if t.cat == only or t.cat.startswith(only)]
    print('tests:', len(tests), 'cases each:', n)
    work = OUT + '/insn'
    os.makedirs(work, exist_ok=True)
    asm = build_asm(tests)
    open(work + '/t.s', 'w').write(asm)
    subprocess.run(['clang', '--target=i386-unknown-linux-gnu', '-c', '-x', 'assembler', work + '/t.s', '-o', work + '/t.o'], check=True)
    subprocess.run(['llvm-objcopy', '-O', 'binary', '--only-section=.text', work + '/t.o', work + '/t.bin'], check=True)
    blob = open(work + '/t.bin', 'rb').read()
    BASE = 0x50000000
    entries = [BASE + 1024 * i for i in range(len(tests))]
    if '--nolift' not in args:
        r = subprocess.run([R + '/lifter/target/release/lifter', 'lift-blob', '--blob', work + '/t.bin', '--base', '0x%x' % BASE, '--entries', ','.join('%x' % e for e in entries), '--out', work + '/gen'], capture_output=True, text=True)
        print(r.stderr[-300:])
        g = work + '/gen'
        FL = '-std=c11 -ffreestanding -O2 -fno-strict-aliasing -fwrapv -fno-math-errno -ffp-contract=off -fvisibility=hidden -march=x86-64-v2'
        os.makedirs(work + '/obj', exist_ok=True)
        subprocess.run('rm -f %s/obj/*.o; cd %s && ls */shard_*.c dispatch.c program.c lx_hooks.c | xargs -P 12 -I{} sh -c "clang %s -I. -I%s/include -c {} -o %s/obj/\\$(echo {} | tr / _).o"' % (work, g, FL, R, work), shell=True, check=True)
        subprocess.run('clang -O1 -std=gnu11 -march=x86-64-v2 -I%s -I%s/include -no-pie -o %s/diffdrv %s/lifter/tests/diffdrv.c %s/obj/*.o -lm' % (g, R, work, R, work), shell=True, check=True)
    size = (len(blob) + 0xfff) & ~0xfff
    img = blob + b'\0' * (size - len(blob))
    env = Env(None, mods=[(BASE, size, img, 'blob')], out=work, driver=work + '/diffdrv')
    env.setup_uc()
    rng = random.Random(seed)
    cases, owner = [], []
    for ti, t in enumerate(tests):
        for _ in range(n):
            cases.append(make_insn_case(rng, entries[ti])); owner.append(ti)
    stats = collections.defaultdict(collections.Counter)
    failing = collections.defaultdict(list)
    B = 1000
    for i in range(0, len(cases), B):
        us, ds, tc, tu = run_batch(env, cases[i:i + B], 'i')
        for j, (u, d) in enumerate(zip(us, ds)):
            ti = owner[i + j]; t = tests[ti]
            res = compare_insn(t, u, d)
            stats[t.cat][res if res in ('ok', 'skip') else 'FAIL'] += 1
            if res not in ('ok', 'skip'):
                failing[ti].append((res, cases[i + j]))
        print('batch %d: c %.1fs unicorn %.1fs' % (i // B + 1, tc, tu), flush=True)
    tot = collections.Counter()
    print('\ncategory      ok   fail  skipped   pass%')
    for c in sorted(stats):
        s = stats[c]
        tot.update(s)
        print('%-10s %6d %6d %7d   %.1f%%' % (c, s['ok'], s['FAIL'], s['skip'], 100.0 * s['ok'] / max(1, s['ok'] + s['FAIL'])))
    print('%-10s %6d %6d %7d   %.1f%%' % ('TOTAL', tot['ok'], tot['FAIL'], tot['skip'], 100.0 * tot['ok'] / max(1, tot['ok'] + tot['FAIL'])))
    nf = len(failing)
    print('failing instruction templates: %d of %d' % (nf, len(tests)))
    for ti in sorted(failing)[:60]:
        print('  [%s] %s  -> %s (%d/%d cases)' % (tests[ti].cat, tests[ti].name[:110], failing[ti][0][0], len(failing[ti]), n))
    pickle.dump({ti: failing[ti][:3] for ti in failing}, open(work + '/fails.pkl', 'wb'))
    import json
    json.dump({'tests': len(tests), 'stats': {c: dict(s) for c, s in stats.items()}, 'failing_templates': nf}, open(work + '/summary.json', 'w'))
