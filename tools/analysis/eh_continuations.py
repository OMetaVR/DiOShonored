#!/usr/bin/env python3
"""Non-local continuation sites of MSVC exception handling in a PE: python3 -I eh_continuations.py <pe> [--base 0xNEW]
  * C++ EH: FuncInfo (magic 0x1993052x) found through `mov eax, FuncInfo; jmp ...` handler stubs; each catch funclet returns the
    continuation address (`mov eax, imm32` ... `ret`), which lies inside the parent function.
  * SEH4: scope tables referenced by `push -2; push ScopeTable` (inline prolog) or `push ScopeTable; call __SEH_prolog4`; records
    with a filter have an __except body (HandlerFunc) that _EH4_TransferToHandler jumps to.
Prints `<kind> <continuation> <owner-hint>` lines (addresses at --base when given, for rebased modules)."""
import sys, struct, pefile
from iced_x86 import Decoder, Mnemonic, OpKind, FlowControl, Register
path = sys.argv[1]
newbase = int(sys.argv[sys.argv.index('--base') + 1], 16) if '--base' in sys.argv else None
pe = pefile.PE(path, fast_load=True); B = pe.OPTIONAL_HEADER.ImageBase; SZ = pe.OPTIONAL_HEADER.SizeOfImage
img = bytearray(SZ)
img[:len(pe.header)] = pe.header
for s in pe.sections:
    d = s.get_data(); img[s.VirtualAddress:s.VirtualAddress + len(d)] = d
texts = [(B + s.VirtualAddress, B + s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData)) for s in pe.sections if s.Characteristics & 0x20000000]
def in_text(a): return any(lo <= a < hi for lo, hi in texts)
def u32(a): return struct.unpack_from('<I', img, a - B)[0] if B <= a < B + SZ - 4 else None
def i32(a): return struct.unpack_from('<i', img, a - B)[0]
reb = (lambda a: a - B + newbase) if newbase else (lambda a: a)
out = set()

def funclet_returns(f, limit=400):
    """continuation addresses returned by a catch funclet: `mov eax, imm32` reaching a `ret` (bounded walk over its CFG)"""
    res, seen, work = set(), set(), [(f, None)]
    while work and len(seen) < limit:
        a, eax = work.pop()
        if a in seen or not in_text(a): continue
        seen.add(a)
        d = Decoder(32, bytes(img[a - B:a - B + 16]), ip=a); i = next(iter(d))
        if i.mnemonic == Mnemonic.MOV and i.op_count == 2 and i.op0_kind == OpKind.REGISTER and i.op0_register == Register.EAX and i.op1_kind == OpKind.IMMEDIATE32:
            eax = i.immediate32
        fc = i.flow_control
        if fc == FlowControl.RETURN:
            if eax is not None and in_text(eax): res.add(eax)
            continue
        if fc in (FlowControl.UNCONDITIONAL_BRANCH,):
            work.append((i.near_branch_target, eax)); continue
        if fc == FlowControl.CONDITIONAL_BRANCH:
            work.append((i.near_branch_target, eax)); work.append((i.next_ip, eax)); continue
        if fc in (FlowControl.INDIRECT_BRANCH, FlowControl.INTERRUPT, FlowControl.EXCEPTION): continue
        if fc == FlowControl.CALL or fc == FlowControl.INDIRECT_CALL:
            eax = None
        work.append((i.next_ip, eax))
    return res

# ---- C++ EH: handler stubs `mov eax, imm32; jmp rel32` with a FuncInfo magic at imm32
for lo, hi in texts:
    for a in range(lo, hi - 10):
        r = a - B
        if img[r] == 0xB8 and img[r + 5] == 0xE9:
            fi = u32(a + 1)
            m = u32(fi) if fi else None
            if m is None or m not in (0x19930520, 0x19930521, 0x19930522): continue
            max_state, unwind, ntry, ptry = u32(fi + 4), u32(fi + 8), u32(fi + 12), u32(fi + 16)
            if ntry is None or ntry > 1000: continue
            for t in range(ntry):
                tb = ptry + 20 * t
                ncatch, phand = u32(tb + 12), u32(tb + 16)
                for c in range(ncatch or 0):
                    h = u32(phand + 16 * c + 12)
                    if h and in_text(h):
                        for k in funclet_returns(h): out.add(('catch', reb(k), reb(a)))
# ---- SEH4 scope tables: `push -2; push imm32 (table)` or `push imm32; call` where the table looks like a scope table
def scope_records(t):
    gs, gsx, eh, ehx = u32(t), u32(t + 4), u32(t + 8), u32(t + 12)
    if None in (gs, gsx, eh, ehx) or gs not in (0xFFFFFFFE,) and not (gs < 0x80000000 or gs > 0xFFFF0000): return None
    if eh is None or not (eh > 0xFFFF0000 or eh < 0x1000): return None
    recs, r = [], t + 16
    for n in range(64):
        enc, flt, hnd = u32(r), u32(r + 4), u32(r + 8)
        if hnd is None or not in_text(hnd) or not (enc == 0xFFFFFFFE or enc < n): break
        if flt and not in_text(flt) and flt != 1: break
        recs.append((enc, flt, hnd)); r += 12
    return recs or None
for lo, hi in texts:
    for a in range(lo, hi - 10):
        r = a - B
        if img[r] == 0x68:
            t = u32(a + 1)
            if t is None or not (B <= t < B + SZ): continue
            prev2 = img[r - 2:r] == b'\x6a\xfe'
            nxt_call = img[r + 5] == 0xE8
            if not (prev2 or nxt_call): continue
            recs = scope_records(t)
            if not recs: continue
            for enc, flt, hnd in recs:
                if flt: out.add(('except', reb(hnd), reb(a)))
for k, c, o in sorted(out, key=lambda x: (x[0], x[1])):
    print('%-6s %08x %08x' % (k, c, o))
