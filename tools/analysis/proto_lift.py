#!/usr/bin/env python3
"""Spike 0.5 prototype: lift real Dishonored.exe functions (integer subset) to C in two styles and measure the AArch64 code size
clang produces, to compare design choices *quantitatively*:

  mode N  ("SR style"): guest registers live in a `cpu` struct in memory, EFLAGS fully materialised into cpu->eflags after every
                         flag-writing instruction (what M-HT/SR's llasm/x86 backends do).
  mode L  ("ours"):      guest registers are C locals; flags are lazy: a flag-writing instruction records its operands, the consumer
                         (jcc/setcc/cmovcc) rebuilds the condition from them; a forward reaching-definition analysis picks the
                         single producer kind; clang removes dead recordings.

Both use the same guest-memory model (base + u32), faithful guest stack (push/pop/call/ret touch guest memory) and the same
register-passing call ABI for lifted functions. Only a subset of x86 is handled (no SSE/x87/partial regs except AL/CL/DL/BL);
functions that use anything else are counted as "unsupported" with the reason.

Nothing here is meant to be correct enough to run the game; it measures code quality. Output goes to private/ (derived from game bytes).
Usage: python3 -I proto_lift.py <exe> <functions.tsv> <outdir> [max_functions] [seed]
"""
import sys, os, struct, random, collections, subprocess, re, json
import pefile
from iced_x86 import (Decoder, Instruction, Mnemonic, FlowControl, OpKind, Register, ConditionCode, MemorySize, CodeSize)

exe, ftsv, outdir = sys.argv[1:4]
MAXF = int(sys.argv[4]) if len(sys.argv) > 4 else 1500
SEED = int(sys.argv[5]) if len(sys.argv) > 5 else 1
os.makedirs(outdir, exist_ok=True)
pe = pefile.PE(exe, fast_load=True); BASE = pe.OPTIONAL_HEADER.ImageBase
t = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
T0 = BASE + t.VirtualAddress; TSZ = min(t.Misc_VirtualSize, t.SizeOfRawData); T1 = T0 + TSZ
text = pe.get_data(t.VirtualAddress, TSZ)
dec = Decoder(32, text, ip=T0)

entries = {}
for i, l in enumerate(open(ftsv)):
    if i == 0: continue
    f = l.rstrip('\n').split('\t'); entries[int(f[0], 16)] = (int(f[1], 16), int(f[2], 16), int(f[4]))

R32 = {Register.EAX: 'eax', Register.ECX: 'ecx', Register.EDX: 'edx', Register.EBX: 'ebx', Register.ESP: 'esp', Register.EBP: 'ebp',
       Register.ESI: 'esi', Register.EDI: 'edi'}
R8 = {Register.AL: 'eax', Register.CL: 'ecx', Register.DL: 'edx', Register.BL: 'ebx'}
MS = {}
for n in dir(MemorySize):
    if n.isupper():
        m = re.fullmatch(r'(U?INT)(\d+)', n)
        if m: MS[getattr(MemorySize, n)] = int(m.group(2)) // 8

MNN = {getattr(Mnemonic, n): n for n in dir(Mnemonic) if n.isupper()}
class Unsupported(Exception): pass

CC = {ConditionCode.E: 'e', ConditionCode.NE: 'ne', ConditionCode.B: 'b', ConditionCode.AE: 'ae', ConditionCode.BE: 'be', ConditionCode.A: 'a',
      ConditionCode.L: 'l', ConditionCode.GE: 'ge', ConditionCode.LE: 'le', ConditionCode.G: 'g', ConditionCode.S: 's', ConditionCode.NS: 'ns',
      ConditionCode.O: 'o', ConditionCode.NO: 'no', ConditionCode.P: 'p', ConditionCode.NP: 'np'}

TYPES = {1: ('uint8_t', 'int8_t'), 2: ('uint16_t', 'int16_t'), 4: ('uint32_t', 'int32_t')}

# ------------------------------------------------------------------ decode a function into a CFG
def decode_function(entry):
    insns = {}
    stack = [entry]
    while stack:
        va = stack.pop()
        while va not in insns:
            if not (T0 <= va < T1): raise Unsupported('flow leaves .text')
            dec.position = va - T0; dec.ip = va
            i = Instruction(); dec.decode_out(i)
            if i.is_invalid: raise Unsupported('invalid decode')
            insns[va] = i
            fc = i.flow_control
            nxt = va + i.len
            if fc == FlowControl.NEXT or fc == FlowControl.CALL or fc == FlowControl.INDIRECT_CALL:
                va = nxt; continue
            if fc == FlowControl.CONDITIONAL_BRANCH:
                t = i.near_branch_target
                if t in entries and t != entry: raise Unsupported('conditional tail jump')
                stack.append(t); va = nxt; continue
            if fc == FlowControl.UNCONDITIONAL_BRANCH:
                t = i.near_branch_target
                if t in entries and t != entry: break      # tail call
                stack.append(t); break
            if fc == FlowControl.RETURN: break
            if fc == FlowControl.INDIRECT_BRANCH: raise Unsupported('indirect jmp')
            raise Unsupported('flow %d' % fc)
        if len(insns) > 600: raise Unsupported('too large for prototype')
    return insns

_pop_cache = {}
def callee_pop(t):
    """Bytes popped by the callee beyond the return address (ret imm16), must agree on all return paths."""
    if t in _pop_cache: 
        v = _pop_cache[t]
        if v is None: raise Unsupported('callee pop unknown')
        return v
    try:
        ins = decode_function(t)
        pops = {(i.immediate16 if i.op_count else 0) for i in ins.values() if i.flow_control == FlowControl.RETURN}
        if len(pops) == 1: _pop_cache[t] = pops.pop()
        elif not pops: _pop_cache[t] = 0     # tail-call only / noreturn: assume 0
        else: _pop_cache[t] = None
    except Unsupported:
        _pop_cache[t] = 0      # prototype: callee not analysable here; a real lifter takes it from its own summary
    return callee_pop(t)

# ------------------------------------------------------------------ stack-slot promotion analysis (mode P)
def stack_analysis(insns, entry):
    """Per-instruction (esp_delta, ebp_delta) relative to entry esp; slot access table; raises Unsupported when promotion is unsafe."""
    ST = {entry: (0, None)}
    work = [entry]
    def push_state(va, st):
        if va not in insns: return
        old = ST.get(va)
        if old is None: ST[va] = st; work.append(va)
        elif old != st:
            # ebp unknown vs known is tolerated only if equal after merge; otherwise give up
            if old[0] != st[0]: raise Unsupported('stack depth differs at join')
            if old[1] != st[1]:
                ST[va] = (old[0], None); work.append(va)
    while work:
        va = work.pop(); i = insns[va]; d, eb = ST[va]; m = i.mnemonic
        nd, neb = d, eb
        for k in range(i.op_count):
            kind = i.op_kind(k)
            if kind == OpKind.MEMORY:
                if i.memory_base in (Register.ESP, Register.EBP) and i.memory_index != Register.NONE: raise Unsupported('indexed stack access')
                if m == Mnemonic.LEA and i.memory_base in (Register.ESP, Register.EBP): raise Unsupported('stack address escapes (lea)')
                if i.memory_base == Register.EBP and eb is None: raise Unsupported('ebp frame unknown')
                if i.memory_index in (Register.ESP, Register.EBP): raise Unsupported('stack reg as index')
            elif kind == OpKind.REGISTER and i.op_register(k) in (Register.ESP, Register.EBP):
                r = i.op_register(k)
                ok = (m in (Mnemonic.PUSH, Mnemonic.POP) and k == 0) \
                     or (m == Mnemonic.MOV and i.op_kind(0) == OpKind.REGISTER and i.op_kind(1) == OpKind.REGISTER and {i.op_register(0), i.op_register(1)} == {Register.EBP, Register.ESP}) \
                     or (m in (Mnemonic.SUB, Mnemonic.ADD) and r == Register.ESP and k == 0 and i.op_kind(1) in (OpKind.IMMEDIATE8TO32, OpKind.IMMEDIATE32))
                if not ok: raise Unsupported('stack register used as value')
        if m == Mnemonic.PUSH: nd = d - 4
        elif m == Mnemonic.POP:
            nd = d + 4
            if i.op_kind(0) == OpKind.REGISTER and i.op_register(0) == Register.EBP: neb = None
        elif m in (Mnemonic.SUB, Mnemonic.ADD) and i.op_kind(0) == OpKind.REGISTER and i.op_register(0) == Register.ESP:
            imm = i.immediate(1) & 0xffffffff
            if imm & 0x80000000: imm -= 1 << 32
            nd = d - imm if m == Mnemonic.SUB else d + imm
        elif m == Mnemonic.MOV and i.op_kind(0) == OpKind.REGISTER and i.op_register(0) == Register.EBP: neb = d
        elif m == Mnemonic.MOV and i.op_kind(0) == OpKind.REGISTER and i.op_register(0) == Register.ESP:
            if eb is None: raise Unsupported('mov esp,ebp with unknown ebp')
            nd = eb
        elif m == Mnemonic.LEAVE:
            if eb is None: raise Unsupported('leave with unknown ebp')
            nd = eb + 4; neb = None
        elif i.flow_control == FlowControl.CALL:
            nd = d + callee_pop(i.near_branch_target)
        elif i.flow_control == FlowControl.INDIRECT_CALL: raise Unsupported('indirect call: callee pop unknown')
        fc = i.flow_control; nxt = va + i.len
        if fc in (FlowControl.NEXT, FlowControl.CALL): push_state(nxt, (nd, neb))
        elif fc == FlowControl.CONDITIONAL_BRANCH:
            push_state(nxt, (nd, neb)); push_state(i.near_branch_target, (nd, neb))
        elif fc == FlowControl.UNCONDITIONAL_BRANCH: push_state(i.near_branch_target, (nd, neb))
    return ST

def slot_table(insns, ST, argabi=False):
    """Classify stack slots (offset from entry esp). Returns (promotable_set, elided_callee_saved: {offset: reg}, incoming_reads:set)."""
    info = collections.defaultdict(lambda: {'sizes': set(), 'push': [], 'pop': [], 'rw': 0, 'wr': 0})
    calls = []
    for va in sorted(insns):
        i = insns[va]
        if va not in ST: continue
        d, eb = ST[va]; m = i.mnemonic
        if i.flow_control == FlowControl.CALL: calls.append((va, d, callee_pop(i.near_branch_target)))
        if m == Mnemonic.PUSH:
            o = d - 4
            info[o]['sizes'].add(4); info[o]['push'].append((va, i.op_register(0) if i.op_kind(0) == OpKind.REGISTER else None))
        elif m == Mnemonic.POP:
            o = d
            info[o]['sizes'].add(4); info[o]['pop'].append((va, i.op_register(0) if i.op_kind(0) == OpKind.REGISTER else None))
        for k in range(i.op_count):
            if i.op_kind(k) == OpKind.MEMORY and i.memory_base in (Register.ESP, Register.EBP) and m != Mnemonic.LEA:
                base = d if i.memory_base == Register.ESP else eb
                o = base + (i.memory_displacement if i.memory_displacement < 0x80000000 else i.memory_displacement - (1 << 32))
                sz = MS.get(i.memory_size, 0)
                info[o]['sizes'].add(sz); info[o]['rw'] += 1
                if k == 0 and m not in (Mnemonic.CMP, Mnemonic.TEST): info[o]['wr'] += 1
    bad = set()
    offs = sorted(info)
    for o in offs:
        if info[o]['sizes'] != {4}: bad.add(o)
    for a, b in zip(offs, offs[1:]):
        if b - a < 4: bad.add(a); bad.add(b)
    # call argument regions are read by callees
    for va, d, pop in calls:
        # following `add esp, K` reveals cdecl cleanup size
        nxt = insns.get(va + insns[va].len)
        k = 0
        if nxt is not None and nxt.mnemonic == Mnemonic.ADD and nxt.op_kind(0) == OpKind.REGISTER and nxt.op_register(0) == Register.ESP:
            k = nxt.immediate(1) & 0xffffffff
        span = max(pop, k, 0 if (pop or k) else 32)
        if argabi:
            n_abi = reg_abi(insns[va].near_branch_target)
            if n_abi is not None: continue        # callee takes these as register arguments
        for o in offs:
            if d <= o < d + span: bad.add(o)
    prom = set(); elided = {}
    callvas = [c[0] for c in calls]
    for o in offs:
        if o in bad: continue
        e = info[o]
        if o >= 4:                                   # incoming argument slot: promotable only if read-only
            if e['wr'] == 0 and not e['push'] and not e['pop']: prom.add(o)
            continue
        if o == 0: continue                          # return address
        if e['push'] and not e['pop']:               # pushed as call argument / never popped: stays in memory
            continue
        if e['pop'] and not e['push']: continue
        if e['push']:
            ok = True
            for (pv, pr) in e['push']:
                pops = [q for q in e['pop'] if q[0] > pv]
                if not pops: ok = False; break
                for qv, qr in pops:
                    if any(pv < cv < qv for cv in callvas) and pr not in (Register.EBX, Register.ESI, Register.EDI, Register.EBP): ok = False
            if not ok: continue
            regs = {pr for _, pr in e['push']} | {qr for _, qr in e['pop']}
            if len(regs) == 1 and list(regs)[0] in (Register.EBX, Register.ESI, Register.EDI, Register.EBP) and e['rw'] == 0:
                elided[o] = list(regs)[0]; continue
        prom.add(o)
    slot_table.incoming = {o for o in info if o >= 4}
    return prom, elided

_abi_cache = {}
CALLEE_ARGS = {}
def reg_abi(t):
    """Number of stack argument slots callee `t` takes as register arguments (None = keeps the memory ABI)."""
    if t in _abi_cache: return _abi_cache[t]
    _abi_cache[t] = None                          # cycle guard
    res = None
    try:
        pop = callee_pop(t)
        ins = decode_function(t); ST = stack_analysis(ins, t)
        prom, el = slot_table(ins, ST, argabi=False); inc = slot_table.incoming
        if pop > 0:
            if all((4 <= o < 4 + pop and o in prom) for o in inc): res = pop // 4
        else:
            if not inc: res = 0
    except Unsupported:
        res = None
    _abi_cache[t] = res
    return res

# ------------------------------------------------------------------ code generation
class Gen:
    def __init__(self, mode, entry, insns):
        self.mode = mode; self.entry = entry; self.insns = insns
        self.lines = []; self.tmp = 0; self.used_flag_vars = set(); self.ninstr = len(insns)
        self.n_self = None; self.ST = None; self.prom = set(); self.elided = {}; self.slot_vars = {}; self.promoted_ops = 0

    # register access ------------------------------------------------
    def rd(self, r):
        n = R32[r]
        return ('cpu->' + n) if self.mode == 'N' else n
    def wr(self, r, v):
        return '%s = %s;' % (self.rd(r), v)
    def rd8(self, r):
        return '((uint8_t)%s)' % ('cpu->' + R8[r] if self.mode == 'N' else R8[r])
    def wr8(self, r, v):
        n = ('cpu->' + R8[r]) if self.mode == 'N' else R8[r]
        return '%s = (%s & 0xffffff00u) | (uint8_t)(%s);' % (n, n, v)

    def ea(self, i):
        parts = []
        if i.memory_segment in (Register.FS, Register.GS, Register.ES): raise Unsupported('segment override')
        if i.memory_base != Register.NONE:
            if i.memory_base not in R32: raise Unsupported('mem base')
            parts.append(self.rd(i.memory_base))
        if i.memory_index != Register.NONE:
            if i.memory_index not in R32: raise Unsupported('mem index')
            sc = i.memory_index_scale
            parts.append(self.rd(i.memory_index) + (('*%d' % sc) if sc != 1 else ''))
        d = i.memory_displacement & 0xffffffff
        if d or not parts: parts.append('0x%xu' % d)
        return '(uint32_t)(%s)' % ' + '.join(parts)

    def msz(self, i):
        s = MS.get(i.memory_size)
        if s is None: raise Unsupported('mem size')
        return s

    # operand value as unsigned T of given size
    def val(self, i, k, size):
        kind = i.op_kind(k)
        T = TYPES[size][0]
        if kind == OpKind.REGISTER:
            r = i.op_register(k)
            if size == 4 and r in R32: return self.rd(r)
            if size == 1 and r in R8: return self.rd8(r)
            raise Unsupported('register operand')
        if kind == OpKind.MEMORY:
            sv = self.slot_var(i, size)
            if sv: return sv
            return 'ld%d(%s)' % (size * 8, self.ea(i))
        if kind in (OpKind.IMMEDIATE8, OpKind.IMMEDIATE8TO32, OpKind.IMMEDIATE32, OpKind.IMMEDIATE16):
            return '(%s)0x%xu' % (T, i.immediate(k) & ((1 << (size * 8)) - 1))
        raise Unsupported('operand kind %d' % kind)

    def store(self, i, k, size, v):
        kind = i.op_kind(k)
        if kind == OpKind.REGISTER:
            r = i.op_register(k)
            if size == 4 and r in R32: return self.wr(r, v)
            if size == 1 and r in R8: return self.wr8(r, v)
            raise Unsupported('register dest')
        if kind == OpKind.MEMORY:
            sv = self.slot_var(i, size)
            if sv: return '%s = %s;' % (sv, v)
            return 'st%d(%s, %s);' % (size * 8, self.ea(i), v)
        raise Unsupported('dest kind')

    def slot_off(self, i):
        if self.ST is None or i.ip not in self.ST: return None
        if i.memory_base not in (Register.ESP, Register.EBP) or i.memory_index != Register.NONE: return None
        d, eb = self.ST[i.ip]
        base = d if i.memory_base == Register.ESP else eb
        if base is None: return None
        disp = i.memory_displacement
        if disp >= 0x80000000: disp -= 1 << 32
        return base + disp

    def slot_var(self, i, size):
        if size != 4: return None
        o = self.slot_off(i)
        if o is None or o not in self.prom: return None
        n = ('sp%d' if o >= 0 else 'sm%d') % abs(o)
        self.slot_vars[n] = o
        self.promoted_ops += 1
        return n

    def opsize(self, i):
        # operand size of op0 for ALU forms
        k = i.op_kind(0)
        if k == OpKind.MEMORY: return self.msz(i)
        r = i.op_register(0)
        if r in R32: return 4
        if r in R8: return 1
        raise Unsupported('operand size')

    def emit(self, s): self.lines.append('  ' + s)

    # flags ----------------------------------------------------------
    def set_flags(self, kind, size, a=None, b=None, r=None):
        T = TYPES[size][0]
        if self.mode == 'N':
            if kind == 'SUB': self.emit('cpu->eflags = (cpu->eflags & ~0x8d5u) | fl_sub%d(%s, %s);' % (size * 8, a, b))
            elif kind == 'ADD': self.emit('cpu->eflags = (cpu->eflags & ~0x8d5u) | fl_add%d(%s, %s);' % (size * 8, a, b))
            elif kind == 'LG': self.emit('cpu->eflags = (cpu->eflags & ~0x8d5u) | fl_logic%d(%s);' % (size * 8, r))
            elif kind == 'INC': self.emit('cpu->eflags = (cpu->eflags & ~0x8d4u) | (fl_add%d(%s, 1) & ~1u);' % (size * 8, a))
            elif kind == 'DEC': self.emit('cpu->eflags = (cpu->eflags & ~0x8d4u) | (fl_sub%d(%s, 1) & ~1u);' % (size * 8, a))
            elif kind == 'SH': self.emit('cpu->eflags = (cpu->eflags & ~0x8d5u) | fl_logic%d(%s);' % (size * 8, r))
            else: raise Unsupported('flag kind ' + kind)
            return
        v = 'f%s%d' % (kind.lower(), size * 8)
        if kind in ('SUB', 'ADD'):
            self.used_flag_vars.add((v + '_a', T)); self.used_flag_vars.add((v + '_b', T))
            self.emit('%s_a = %s; %s_b = %s;' % (v, a, v, b))
        elif kind in ('INC', 'DEC'):
            self.used_flag_vars.add((v + '_a', T)); self.emit('%s_a = %s;' % (v, a))
        else:
            self.used_flag_vars.add((v + '_r', T)); self.emit('%s_r = %s;' % (v, r))

    def cond(self, cc, reach):
        """C expression for condition code `cc` given the set of reaching flag kinds."""
        if self.mode == 'N':
            e = 'cpu->eflags'
            F = {'cf': '(%s & 1u)' % e, 'pf': '(%s & 4u)' % e, 'zf': '(%s & 0x40u)' % e, 'sf': '(%s & 0x80u)' % e, 'of': '(%s & 0x800u)' % e}
            return {
                'e': F['zf'], 'ne': '!' + F['zf'], 'b': F['cf'], 'ae': '!' + F['cf'], 'be': '(%s || %s)' % (F['cf'], F['zf']),
                'a': '!(%s || %s)' % (F['cf'], F['zf']), 'l': '(!%s != !%s)' % (F['sf'], F['of']), 'ge': '(!%s == !%s)' % (F['sf'], F['of']),
                'le': '(%s || (!%s != !%s))' % (F['zf'], F['sf'], F['of']), 'g': '(!%s && (!%s == !%s))' % (F['zf'], F['sf'], F['of']),
                's': F['sf'], 'ns': '!' + F['sf'], 'o': F['of'], 'no': '!' + F['of'], 'p': F['pf'], 'np': '!' + F['pf']}[cc]
        if len(reach) != 1: raise Unsupported('flags reach from %s' % ('entry/unknown' if not reach else 'multiple kinds'))
        (kind, size), = reach
        if kind == 'UNDEF': raise Unsupported('flags undefined at consumer')
        if kind == 'ENTRY': raise Unsupported('flags live-in at function entry')
        T, ST = TYPES[size]; bits = size * 8
        v = 'f%s%d' % (kind.lower(), bits)
        def par(x): return '__builtin_parity((uint8_t)(%s))' % x
        if kind in ('SUB', 'DEC'):
            a = v + '_a'; b = v + '_b' if kind == 'SUB' else '(%s)1' % T
            r = '(%s)(%s - %s)' % (T, a, b)
            ovf = '(((%s ^ %s) & (%s ^ %s)) >> %d)' % (a, b, a, r, bits - 1)
            carry_ok = kind == 'SUB'
            tab = {'e': '%s == %s' % (a, b), 'ne': '%s != %s' % (a, b), 'b': '%s < %s' % (a, b), 'ae': '%s >= %s' % (a, b), 'be': '%s <= %s' % (a, b),
                   'a': '%s > %s' % (a, b), 'l': '(%s)%s < (%s)%s' % (ST, a, ST, b), 'ge': '(%s)%s >= (%s)%s' % (ST, a, ST, b),
                   'le': '(%s)%s <= (%s)%s' % (ST, a, ST, b), 'g': '(%s)%s > (%s)%s' % (ST, a, ST, b), 's': '(%s)%s < 0' % (ST, r),
                   'ns': '(%s)%s >= 0' % (ST, r), 'o': '(%s & 1)' % ovf, 'no': '!(%s & 1)' % ovf, 'p': '!' + par(r), 'np': par(r)}
            if not carry_ok and cc in ('b', 'ae', 'be', 'a'): raise Unsupported('CF consumer after dec')
            return '(%s)' % tab[cc]
        if kind in ('ADD', 'INC'):
            a = v + '_a'; b = v + '_b' if kind == 'ADD' else '(%s)1' % T
            r = '(%s)(%s + %s)' % (T, a, b)
            ovf = '(((%s ^ %s) & (%s ^ %s)) >> %d)' % (a, r, b, r, bits - 1)
            if kind == 'INC' and cc in ('b', 'ae', 'be', 'a'): raise Unsupported('CF consumer after inc')
            tab = {'e': '%s == 0' % r, 'ne': '%s != 0' % r, 'b': '%s < %s' % (r, a), 'ae': '%s >= %s' % (r, a), 'be': '(%s <= %s)' % (r, a),
                   'a': '%s > %s' % (r, a), 'l': '(((%s)%s < 0) != ((%s & 1) != 0))' % (ST, r, ovf), 'ge': '(((%s)%s < 0) == ((%s & 1) != 0))' % (ST, r, ovf),
                   'le': '(%s == 0 || (((%s)%s < 0) != ((%s & 1) != 0)))' % (r, ST, r, ovf), 'g': '(%s != 0 && (((%s)%s < 0) == ((%s & 1) != 0)))' % (r, ST, r, ovf),
                   's': '(%s)%s < 0' % (ST, r), 'ns': '(%s)%s >= 0' % (ST, r), 'o': '(%s & 1)' % ovf, 'no': '!(%s & 1)' % ovf, 'p': '!' + par(r), 'np': par(r)}
            return '(%s)' % tab[cc]
        r = v + '_r'
        if kind == 'SH' and cc in ('b', 'ae', 'be', 'a', 'o', 'no', 'l', 'ge', 'le', 'g'): raise Unsupported('CF/OF consumer after shift')
        tab = {'e': '%s == 0' % r, 'ne': '%s != 0' % r, 'b': '0', 'ae': '1', 'be': '%s == 0' % r, 'a': '%s != 0' % r, 'l': '(%s)%s < 0' % (ST, r),
               'ge': '(%s)%s >= 0' % (ST, r), 'le': '(%s)%s <= 0' % (ST, r), 'g': '(%s)%s > 0' % (ST, r), 's': '(%s)%s < 0' % (ST, r),
               'ns': '(%s)%s >= 0' % (ST, r), 'o': '0', 'no': '1', 'p': '!' + par(r), 'np': par(r)}
        return '(%s)' % tab[cc]

# flag kind produced by an instruction (None if it does not write flags, 'UNDEF' if it clobbers them)
FLAG_KIND = {Mnemonic.ADD: 'ADD', Mnemonic.SUB: 'SUB', Mnemonic.CMP: 'SUB', Mnemonic.AND: 'LG', Mnemonic.OR: 'LG', Mnemonic.XOR: 'LG', Mnemonic.TEST: 'LG',
             Mnemonic.INC: 'INC', Mnemonic.DEC: 'DEC', Mnemonic.NEG: 'SUB', Mnemonic.SHL: 'SH', Mnemonic.SHR: 'SH', Mnemonic.SAR: 'SH',
             Mnemonic.IMUL: 'UNDEF'}

def op_size_for_flags(i):
    m = i.mnemonic
    if m in (Mnemonic.IMUL,): return 4
    k = i.op_kind(0)
    if k == OpKind.MEMORY:
        s = MS.get(i.memory_size)
        if s is None: raise Unsupported('mem size')
        return s
    r = i.op_register(0)
    if r in R32: return 4
    if r in R8: return 1
    raise Unsupported('flag operand size')

def flag_reach(insns, entry):
    """Forward dataflow: for each instruction VA, the set of (kind,size) flag producers that can reach it (empty set = from entry)."""
    order = sorted(insns)
    succ = {}
    for va in order:
        i = insns[va]; nxt = va + i.len; fc = i.flow_control
        if fc == FlowControl.NEXT or fc in (FlowControl.CALL, FlowControl.INDIRECT_CALL): succ[va] = [nxt]
        elif fc == FlowControl.CONDITIONAL_BRANCH: succ[va] = [nxt, i.near_branch_target]
        elif fc == FlowControl.UNCONDITIONAL_BRANCH: succ[va] = [i.near_branch_target] if i.near_branch_target in insns else []
        else: succ[va] = []
    IN = {va: None for va in order}      # None = unvisited
    IN[entry] = frozenset([('ENTRY', 0)])
    work = [entry]
    while work:
        va = work.pop()
        i = insns[va]; s = IN[va]
        m = i.mnemonic
        if i.flow_control in (FlowControl.CALL, FlowControl.INDIRECT_CALL): out = frozenset([('UNDEF', 4)])
        elif m in FLAG_KIND:
            k = FLAG_KIND[m]
            try: sz = op_size_for_flags(i)
            except Unsupported: sz = 4
            out = frozenset([(k, sz)])
        elif m in (Mnemonic.ADC, Mnemonic.SBB, Mnemonic.RCL, Mnemonic.RCR, Mnemonic.ROL, Mnemonic.ROR, Mnemonic.SHLD, Mnemonic.SHRD, Mnemonic.MUL, Mnemonic.DIV, Mnemonic.IDIV, Mnemonic.BT, Mnemonic.BSF, Mnemonic.BSR):
            out = frozenset([('UNDEF', 4)])
        else: out = s
        for n in succ[va]:
            if n not in insns: continue
            new = out if IN[n] is None else (IN[n] | out)
            if IN[n] != new:
                IN[n] = new; work.append(n)
    return IN

def lift(entry, mode):
    insns = decode_function(entry)
    g = Gen(mode, entry, insns)
    if mode in ('P', 'A'):
        g.ST = stack_analysis(insns, entry)
        g.prom, g.elided = slot_table(insns, g.ST, argabi=(mode == 'A'))
        g.n_self = reg_abi(entry) if mode == 'A' else None
    reach = flag_reach(insns, entry) if mode != 'N' else None
    order = sorted(insns)
    labels = set()
    for va in order:
        i = insns[va]
        if i.flow_control in (FlowControl.CONDITIONAL_BRANCH, FlowControl.UNCONDITIONAL_BRANCH):
            t = i.near_branch_target
            if t in insns: labels.add(t)
    body = []
    g.lines = body
    saved_callee = set()
    for va in order:
        i = insns[va]; m = i.mnemonic
        if va in labels: body.append('L%x:;' % va)
        nxt = va + i.len
        e = g.emit
        try:
            if m in (Mnemonic.NOP, Mnemonic.INT3): continue
            if m == Mnemonic.MOV:
                sz = g.opsize(i)
                e(g.store(i, 0, sz, g.val(i, 1, sz)))
            elif m in (Mnemonic.MOVZX, Mnemonic.MOVSX):
                if i.op_kind(0) != OpKind.REGISTER or i.op_register(0) not in R32: raise Unsupported('movzx dest')
                if i.op_kind(1) == OpKind.MEMORY: ssz = g.msz(i)
                elif i.op_register(1) in R8: ssz = 1
                else: raise Unsupported('movzx src')
                v = g.val(i, 1, ssz)
                if m == Mnemonic.MOVSX: v = '(uint32_t)(int32_t)(%s)%s' % (TYPES[ssz][1], v)
                else: v = '(uint32_t)%s' % v
                e(g.wr(i.op_register(0), v))
            elif m == Mnemonic.LEA:
                if i.op_register(0) not in R32: raise Unsupported('lea dest')
                e(g.wr(i.op_register(0), g.ea(i)))
            elif m in (Mnemonic.ADD, Mnemonic.SUB, Mnemonic.AND, Mnemonic.OR, Mnemonic.XOR, Mnemonic.CMP, Mnemonic.TEST):
                sz = g.opsize(i); T = TYPES[sz][0]
                a = g.val(i, 0, sz); b = g.val(i, 1, sz)
                if m == Mnemonic.XOR and i.op_kind(0) == OpKind.REGISTER and i.op_kind(1) == OpKind.REGISTER and i.op_register(0) == i.op_register(1) and sz == 4:
                    e(g.wr(i.op_register(0), '0')); g.set_flags('LG', sz, r='(%s)0' % T); continue
                ta, tb = 'a%x' % va, 'b%x' % va
                e('{ %s %s = %s, %s = %s;' % (T, ta, a, tb, b))
                sym = {Mnemonic.ADD: '+', Mnemonic.SUB: '-', Mnemonic.AND: '&', Mnemonic.OR: '|', Mnemonic.XOR: '^'}
                if m in (Mnemonic.CMP,): g.set_flags('SUB', sz, ta, tb)
                elif m == Mnemonic.TEST: g.set_flags('LG', sz, r='(%s)(%s & %s)' % (T, ta, tb))
                else:
                    res = '(%s)(%s %s %s)' % (T, ta, sym[m], tb)
                    e(g.store(i, 0, sz, res))
                    if m == Mnemonic.ADD: g.set_flags('ADD', sz, ta, tb)
                    elif m == Mnemonic.SUB: g.set_flags('SUB', sz, ta, tb)
                    else: g.set_flags('LG', sz, r=res)
                e('}')
            elif m in (Mnemonic.INC, Mnemonic.DEC):
                sz = g.opsize(i); T = TYPES[sz][0]
                a = g.val(i, 0, sz)
                e('{ %s a = %s;' % (T, a))
                e(g.store(i, 0, sz, '(%s)(a %s 1)' % (T, '+' if m == Mnemonic.INC else '-')))
                g.set_flags('INC' if m == Mnemonic.INC else 'DEC', sz, 'a')
                e('}')
            elif m == Mnemonic.NEG:
                sz = g.opsize(i); T = TYPES[sz][0]
                e('{ %s a = %s;' % (T, g.val(i, 0, sz)))
                e(g.store(i, 0, sz, '(%s)(0 - a)' % T)); g.set_flags('SUB', sz, '(%s)0' % T, 'a'); e('}')
            elif m == Mnemonic.NOT:
                sz = g.opsize(i); T = TYPES[sz][0]
                e(g.store(i, 0, sz, '(%s)~%s' % (T, g.val(i, 0, sz))))
            elif m in (Mnemonic.SHL, Mnemonic.SHR, Mnemonic.SAR):
                sz = g.opsize(i)
                if sz != 4: raise Unsupported('shift size')
                if i.op_kind(1) in (OpKind.IMMEDIATE8,): cnt = '%d' % (i.immediate(1) & 31)
                elif i.op_kind(1) == OpKind.REGISTER and i.op_register(1) == Register.CL: cnt = '(%s & 31)' % g.rd(Register.ECX)
                else: raise Unsupported('shift count')
                T, ST = TYPES[sz]
                e('{ %s a = %s; %s c = %s;' % (T, g.val(i, 0, sz), 'uint32_t', cnt))
                op = {Mnemonic.SHL: '(a << c)', Mnemonic.SHR: '(a >> c)', Mnemonic.SAR: '(uint32_t)((int32_t)a >> c)'}[m]
                e('%s r = %s;' % (T, op))
                e(g.store(i, 0, sz, 'r'))
                if cnt in ('0',): pass
                else: g.set_flags('SH', sz, r='r')
                e('}')
            elif m == Mnemonic.IMUL:
                if i.op_count == 2: a = g.val(i, 0, 4); b = g.val(i, 1, 4)
                elif i.op_count == 3: a = g.val(i, 1, 4); b = g.val(i, 2, 4)
                else: raise Unsupported('imul form')
                e(g.store(i, 0, 4, '(uint32_t)((int32_t)%s * (int32_t)%s)' % (a, b)))
            elif m == Mnemonic.PUSH:
                if i.op_kind(0) == OpKind.REGISTER and i.op_register(0) in R32: v = g.rd(i.op_register(0))
                elif i.op_kind(0) in (OpKind.IMMEDIATE8TO32, OpKind.IMMEDIATE32): v = '0x%xu' % (i.immediate(0) & 0xffffffff)
                elif i.op_kind(0) == OpKind.MEMORY and g.msz(i) == 4: v = 'ld32(%s)' % g.ea(i)
                else: raise Unsupported('push operand')
                if g.ST is not None and (g.ST[va][0] - 4) in g.elided and i.op_kind(0) == OpKind.REGISTER:
                    e('%s -= 4;' % g.rd(Register.ESP)); g.promoted_ops += 1
                elif g.ST is not None and (g.ST[va][0] - 4) in g.prom:
                    o = g.ST[va][0] - 4; n = ('sp%d' if o >= 0 else 'sm%d') % abs(o); g.slot_vars[n] = o; g.promoted_ops += 1
                    e('%s = %s; %s -= 4;' % (n, v, g.rd(Register.ESP)))
                else:
                    e('%s = %s - 4; st32(%s, %s);' % (g.rd(Register.ESP), g.rd(Register.ESP), g.rd(Register.ESP), v))
            elif m == Mnemonic.POP:
                if i.op_kind(0) != OpKind.REGISTER or i.op_register(0) not in R32: raise Unsupported('pop operand')
                if g.ST is not None and g.ST[va][0] in g.elided:
                    e('%s += 4;' % g.rd(Register.ESP)); g.promoted_ops += 1
                elif g.ST is not None and g.ST[va][0] in g.prom:
                    o = g.ST[va][0]; n = ('sp%d' if o >= 0 else 'sm%d') % abs(o); g.slot_vars[n] = o; g.promoted_ops += 1
                    e('%s = %s; %s += 4;' % (g.rd(i.op_register(0)), n, g.rd(Register.ESP)))
                else:
                    e('%s = ld32(%s); %s += 4;' % (g.rd(i.op_register(0)), g.rd(Register.ESP), g.rd(Register.ESP)))
            elif m == Mnemonic.CDQ:
                e('%s = (uint32_t)((int32_t)%s >> 31);' % (g.rd(Register.EDX), g.rd(Register.EAX)))
            elif m == Mnemonic.LEAVE:
                e('%s = %s; %s = ld32(%s); %s += 4;' % (g.rd(Register.ESP), g.rd(Register.EBP), g.rd(Register.EBP), g.rd(Register.ESP), g.rd(Register.ESP)))
            elif m == Mnemonic.XCHG and i.op_kind(0) == OpKind.REGISTER and i.op_kind(1) == OpKind.REGISTER and i.op_register(0) in R32 and i.op_register(1) in R32:
                a, b = g.rd(i.op_register(0)), g.rd(i.op_register(1))
                e('{ uint32_t t = %s; %s = %s; %s = t; }' % (a, a, b, b))
            elif i.flow_control == FlowControl.CONDITIONAL_BRANCH:
                cc = CC[i.condition_code]
                e('if (%s) goto L%x;' % (g.cond(cc, reach[va] if reach else None), i.near_branch_target))
            elif m in (Mnemonic.SETE, Mnemonic.SETNE, Mnemonic.SETL, Mnemonic.SETGE, Mnemonic.SETLE, Mnemonic.SETG, Mnemonic.SETB, Mnemonic.SETAE, Mnemonic.SETBE,
                       Mnemonic.SETA, Mnemonic.SETS, Mnemonic.SETNS, Mnemonic.SETO, Mnemonic.SETNO, Mnemonic.SETP, Mnemonic.SETNP):
                cc = CC[i.condition_code]
                e(g.store(i, 0, 1, '(%s) ? 1 : 0' % g.cond(cc, reach[va] if reach else None)))
            elif m in (Mnemonic.CMOVE, Mnemonic.CMOVNE, Mnemonic.CMOVL, Mnemonic.CMOVGE, Mnemonic.CMOVLE, Mnemonic.CMOVG, Mnemonic.CMOVB,
                                              Mnemonic.CMOVAE, Mnemonic.CMOVBE, Mnemonic.CMOVA, Mnemonic.CMOVS, Mnemonic.CMOVNS):
                cc = CC[i.condition_code]
                if i.op_register(0) not in R32: raise Unsupported('cmov dest')
                e('if (%s) %s' % (g.cond(cc, reach[va] if reach else None), g.wr(i.op_register(0), g.val(i, 1, 4))))
            elif i.flow_control == FlowControl.UNCONDITIONAL_BRANCH:
                t = i.near_branch_target
                if t in insns: e('goto L%x;' % t)
                else:
                    n_abi = reg_abi(t) if mode in ('A', 'LA') else None
                    if n_abi:
                        args = []
                        for k in range(n_abi):
                            off = 4 + 4 * k
                            if mode == 'A' and g.n_self and off < 4 + 4 * g.n_self: args.append('sp%d' % off)
                            else: args.append('ld32(esp + %d)' % off)
                        e('return f_%x(esp, eax, ecx, edx, %s);' % (t, ', '.join(args)))
                    else: e('return CALLTAIL(f_%x);' % t)
            elif i.flow_control == FlowControl.CALL:
                t = i.near_branch_target
                if t == nxt: raise Unsupported('call $+5')
                if t not in entries: raise Unsupported('call to non-entry')
                if mode in ('A', 'LA'):
                    n_abi = reg_abi(t)
                    if n_abi:
                        d = g.ST[va][0] if g.ST else 0; args = []
                        for k in range(n_abi):
                            o = d + 4 * k
                            if g.ST and o in g.prom:
                                nm = ('sp%d' if o >= 0 else 'sm%d') % abs(o); g.slot_vars[nm] = o; args.append(nm)
                            else: args.append('ld32(esp + %d)' % (4 * k))
                        CALLEE_ARGS[t] = n_abi
                        e('CALLFA(f_%x, %d, %s);' % (t, callee_pop(t), ', '.join(args)))
                    else:
                        CALLEE_ARGS[t] = 0
                        e('CALLF(f_%x, %d);' % (t, callee_pop(t)))
                elif mode in ('L', 'P'): e('CALLF(f_%x, %d);' % (t, callee_pop(t)))
                else: e('CALLF(f_%x);' % t)
            elif i.flow_control == FlowControl.INDIRECT_CALL:
                if i.op0_kind == OpKind.REGISTER and i.op0_register in R32: tgt = g.rd(i.op0_register)
                elif i.op0_kind == OpKind.MEMORY and g.msz(i) == 4: tgt = 'ld32(%s)' % g.ea(i)
                else: raise Unsupported('indirect call form')
                e('CALLI(%s);' % tgt)
            elif i.flow_control == FlowControl.RETURN:
                n = i.immediate16 if i.op_count else 0
                e('RET(%d);' % n)
            else:
                raise Unsupported('mnemonic ' + MNN.get(m, str(m)))
        except Unsupported as ex:
            raise Unsupported(str(ex))
    # prologue
    if mode in ('L', 'L0', 'P', 'A', 'LA'):
        extra = ''
        if mode == 'A' and g.n_self:
            extra = ''.join(', uint32_t sp%d' % (4 * (k + 1)) for k in range(g.n_self))
        head = ['Ret f_%x(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx%s) {' % (entry, extra),
                '  uint32_t ebx = 0, esi = 0, edi = 0, ebp = 0;'] + (['  uint8_t *const gb = g_base;'] if mode != 'L0' else [])
        for n, o in sorted(g.slot_vars.items()):
            if mode == 'A' and g.n_self and 4 <= o < 4 + 4 * g.n_self: continue      # it is a parameter
            head.append('  uint32_t %s = %s;' % (n, ('ld32(esp + %d)' % o) if o >= 4 else '0'))
        for n, T in sorted(g.used_flag_vars): head.append('  %s %s = 0;' % (T, n))
    else:
        head = ['void f_%x(Cpu *cpu) {' % entry, '  uint8_t *const gb = g_base;']
    src = '\n'.join(head + body + ['}'])
    return src, len(insns)

HEADER = r'''
#include <stdint.h>
typedef struct { uint32_t eax, ecx, edx, esp; } Ret0;
typedef struct { uint64_t r0, r1; } Ret64;      /* r0 = eax | popped_bytes << 32, r1 = edx  (maps to x0/x1 with no packing) */
typedef struct { uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi, eflags; } Cpu;
extern uint8_t *g_base;
Ret0 dispatch0(uint32_t target, uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
Ret64 dispatch64(uint32_t target, uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
void dispatch_cpu(void *cpu, uint32_t target);
#ifdef MODE_L0
typedef Ret0 Ret;
static inline uint8_t  ld8 (uint32_t a){ return *(uint8_t*)(g_base + a); }
static inline uint16_t ld16(uint32_t a){ uint16_t v; __builtin_memcpy(&v, g_base + a, 2); return v; }
static inline uint32_t ld32(uint32_t a){ uint32_t v; __builtin_memcpy(&v, g_base + a, 4); return v; }
static inline void st8 (uint32_t a, uint8_t v){ *(uint8_t*)(g_base + a) = v; }
static inline void st16(uint32_t a, uint16_t v){ __builtin_memcpy(g_base + a, &v, 2); }
static inline void st32(uint32_t a, uint32_t v){ __builtin_memcpy(g_base + a, &v, 4); }
#define CALLF(fn) do { esp -= 4; st32(esp, 0); Ret r_ = fn(esp, eax, ecx, edx); eax = r_.eax; ecx = r_.ecx; edx = r_.edx; esp = r_.esp; } while (0)
#define CALLI(t)  do { uint32_t t_ = (t); esp -= 4; st32(esp, 0); Ret r_ = dispatch0(t_, esp, eax, ecx, edx); eax = r_.eax; ecx = r_.ecx; edx = r_.edx; esp = r_.esp; } while (0)
#define CALLTAIL(fn) fn(esp, eax, ecx, edx)
#define RET(n) do { Ret r_ = { eax, ecx, edx, esp + 4 + (n) }; return r_; } while (0)
#else
#define ld8(a)  (*(uint8_t*)(gb + (uint32_t)(a)))
#define ld16(a) ({ uint16_t v_; __builtin_memcpy(&v_, gb + (uint32_t)(a), 2); v_; })
#define ld32(a) ({ uint32_t v_; __builtin_memcpy(&v_, gb + (uint32_t)(a), 4); v_; })
#define st8(a, v)  (*(uint8_t*)(gb + (uint32_t)(a)) = (v))
#define st16(a, v) do { uint16_t v_ = (v); __builtin_memcpy(gb + (uint32_t)(a), &v_, 2); } while (0)
#define st32(a, v) do { uint32_t v_ = (v); __builtin_memcpy(gb + (uint32_t)(a), &v_, 4); } while (0)
#if defined(MODE_L) || defined(MODE_P) || defined(MODE_A)
typedef Ret64 Ret;
/* direct call: caller knows the callee's `ret N`; no return-address store (callee never reads it in this subset) */
#define CALLF(fn, pop) do { Ret r_ = fn(esp - 4, eax, ecx, edx); eax = (uint32_t)r_.r0; edx = (uint32_t)r_.r1; esp += (pop); } while (0)
#define CALLI(t)  do { uint32_t t_ = (t); Ret r_ = dispatch64(t_, esp - 4, eax, ecx, edx); eax = (uint32_t)r_.r0; edx = (uint32_t)r_.r1; esp = esp - 4 + (uint32_t)(r_.r0 >> 32); } while (0)
#define CALLFA(fn, pop, ...) do { Ret r_ = fn(esp - 4, eax, ecx, edx, __VA_ARGS__); eax = (uint32_t)r_.r0; edx = (uint32_t)r_.r1; esp += (pop); } while (0)
#define CALLTAIL(fn) fn(esp, eax, ecx, edx)
#define RET(n) do { Ret r_ = { (uint64_t)eax | ((uint64_t)(4 + (n)) << 32), edx }; return r_; } while (0)
#else
#define CALLF(fn) do { cpu->esp -= 4; fn(cpu); } while (0)
#define CALLI(t)  do { uint32_t t_ = (t); cpu->esp -= 4; dispatch_cpu(cpu, t_); } while (0)
#define CALLTAIL(fn) (fn(cpu), (void)0)
#define RET(n) do { cpu->esp += 4 + (n); return; } while (0)
static inline uint32_t par8(uint32_t v){ return (__builtin_parity(v & 0xff)) ? 0 : 4; }
#define FLSUB(B, T, ST) static inline uint32_t fl_sub##B(T a, T b){ T r = (T)(a - b); uint32_t f = 0; \
  f |= (a < b); f |= par8(r); f |= (((a ^ b ^ r) & 0x10) ? 0x10 : 0); f |= (r == 0) ? 0x40 : 0; f |= ((ST)r < 0) ? 0x80 : 0; \
  f |= ((((a ^ b) & (a ^ r)) >> (B - 1)) & 1) ? 0x800 : 0; return f; }
#define FLADD(B, T, ST) static inline uint32_t fl_add##B(T a, T b){ T r = (T)(a + b); uint32_t f = 0; \
  f |= (r < a); f |= par8(r); f |= (((a ^ b ^ r) & 0x10) ? 0x10 : 0); f |= (r == 0) ? 0x40 : 0; f |= ((ST)r < 0) ? 0x80 : 0; \
  f |= ((((a ^ r) & (b ^ r)) >> (B - 1)) & 1) ? 0x800 : 0; return f; }
#define FLLOG(B, T, ST) static inline uint32_t fl_logic##B(T r){ uint32_t f = par8(r); f |= (r == 0) ? 0x40 : 0; f |= ((ST)r < 0) ? 0x80 : 0; return f; }
FLSUB(8, uint8_t, int8_t) FLSUB(16, uint16_t, int16_t) FLSUB(32, uint32_t, int32_t)
FLADD(8, uint8_t, int8_t) FLADD(16, uint16_t, int16_t) FLADD(32, uint32_t, int32_t)
FLLOG(8, uint8_t, int8_t) FLLOG(16, uint16_t, int16_t) FLLOG(32, uint32_t, int32_t)
#endif
#endif
'''

def count_arm_insns(asm):
    """Return {symbol: instruction count} from clang -S output (Mach-O aarch64)."""
    res = {}; cur = None
    for line in asm.splitlines():
        m = re.match(r'^(_f_[0-9a-f]+):', line)
        if m: cur = m.group(1)[3:]; res[cur] = 0; continue
        if cur is None: continue
        if line.startswith('\t') and not line.lstrip().startswith(('.', ';')) and line.strip():
            res[cur] += 1
    return res

def main():
    random.seed(SEED)
    cand = [e for e, (lo, hi, n) in entries.items() if 3 <= n <= 120]
    random.shuffle(cand)
    reasons = collections.Counter(); ok = []; pfb = collections.Counter()
    srcs = {'L0': {}, 'L': {}, 'N': {}, 'P': {}, 'A': {}}; pinfo = {}; ainfo = {}
    tried = 0
    for e in cand:
        tried += 1
        try:
            s0, n = lift(e, 'L0'); sl, _ = lift(e, 'L'); sn, _ = lift(e, 'N')
            try:
                spp, _ = lift(e, 'P'); pinfo[e] = True
                try:
                    sa, _ = lift(e, 'A'); ainfo[e] = True
                except Unsupported:
                    sa, _ = lift(e, 'LA'); ainfo[e] = False
            except Unsupported as ex2:
                spp = sl; sa, _ = lift(e, 'LA'); pinfo[e] = False; ainfo[e] = False; reasons['[P fallback] ' + str(ex2)] += 0; pfb[str(ex2)] += 1
        except Unsupported as ex:
            reasons[str(ex)] += 1; continue
        except Exception as ex:
            reasons['internal: %s' % ex] += 1; continue
        srcs['L0'][e] = s0; srcs['L'][e] = sl; srcs['N'][e] = sn; srcs['P'][e] = spp; srcs['A'][e] = sa; ok.append((e, n))
        if len(ok) >= MAXF: break
    print('candidates tried %d, liftable %d (%.1f%%)' % (tried, len(ok), 100.0 * len(ok) / tried))
    print('top unsupported reasons:', reasons.most_common(15))
    results = {}
    for mode in ('L0', 'L', 'P', 'A', 'N'):
        path = os.path.join(outdir, 'proto_%s.c' % mode)
        with open(path, 'w') as fh:
            fh.write('#define MODE_%s 1\n' % mode + HEADER)
            defined = {e for e, n in ok}
            callees = set()
            for e, n in ok:
                callees |= {int(x, 16) for x in re.findall(r'\bf_([0-9a-f]+)\b', srcs[mode][e])}
            for c in sorted(callees):
                if mode == 'A':
                    fh.write('Ret f_%x(uint32_t, uint32_t, uint32_t, uint32_t%s);\n' % (c, ', uint32_t' * (reg_abi(c) or 0)))
                else:
                    fh.write(('Ret f_%x(uint32_t, uint32_t, uint32_t, uint32_t);\n' if mode != 'N' else 'void f_%x(Cpu *);\n') % c)
            for e, n in ok: fh.write(srcs[mode][e] + '\n')
        p = subprocess.run(['clang', '--target=aarch64-apple-ios', '-O2', '-S', '-o', '-', '-w', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', path], capture_output=True, text=True)
        if p.returncode: print(mode, 'clang failed:', p.stderr[:2000]); sys.exit(1)
        open(os.path.join(outdir, 'proto_%s.s' % mode), 'w').write(p.stdout)
        results[mode] = count_arm_insns(p.stdout)
    tot_x86 = sum(n for e, n in ok)
    stats = {}
    for mode in ('L0', 'L', 'P', 'A', 'N'):
        c = results[mode]
        tot = sum(c.get('%x' % e, 0) for e, n in ok)
        ratios = sorted(c.get('%x' % e, 0) / n for e, n in ok)
        stats[mode] = {'arm_insns_total': tot, 'per_x86_insn': tot / tot_x86, 'median_ratio': ratios[len(ratios) // 2], 'p90_ratio': ratios[int(len(ratios) * .9)]}
    stats['x86_insns_total'] = tot_x86; stats['functions'] = len(ok)
    stats['P_applied_functions'] = sum(1 for v in pinfo.values() if v); stats['P_fallback_reasons'] = pfb.most_common(12)
    pa = [e for e, v in pinfo.items() if v]
    stats['P_subset'] = {m: sum(results[m].get('%x' % e, 0) for e in pa) for m in ('L', 'P', 'A', 'N')}
    stats['A_applied_functions'] = sum(1 for v in ainfo.values() if v)
    stats['P_subset_x86'] = sum(n for e, n in ok if pinfo.get(e))
    stats['reasons'] = reasons.most_common(40)
    json.dump(stats, open(os.path.join(outdir, 'proto_stats.json'), 'w'), indent=1)
    print(json.dumps({k: v for k, v in stats.items() if k != 'reasons'}, indent=1))

main()
