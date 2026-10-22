#!/usr/bin/env python3
"""Virtual calls through the game's Steam interface globals: python3 -I steam_vcalls.py <exe> <functions.tsv>
For each `mov reg, [global]` the `this` register is followed to its virtual call; prints interface, vtable slot, the number of
pushes since the previous call (the stack argument count, thiscall pops them) and whether edx is read right after (64-bit
return). Stdout only."""
import sys, collections, pefile
from iced_x86 import Decoder, Mnemonic, OpKind, Register, FlowControl
G = {0x145B150: 'Utils', 0x145B154: 'User', 0x145B158: 'Friends', 0x145B15C: 'RemoteStorage', 0x145B160: 'UserStats',
     0x145B164: 'MatchmakingServers', 0x145B16C: 'Apps', 0x145B178: 'Networking', 0x145B174: 'Matchmaking'}
pe = pefile.PE(sys.argv[1], fast_load=True); B = pe.OPTIONAL_HEADER.ImageBase
img = bytearray(pe.OPTIONAL_HEADER.SizeOfImage)
for s in pe.sections:
    d = s.get_data(); img[s.VirtualAddress:s.VirtualAddress + len(d)] = d
ents = sorted(int(l.split('\t')[0], 16) for i, l in enumerate(open(sys.argv[2])) if i and l.split('\t')[1] == '2')
res = collections.defaultdict(collections.Counter); sites = collections.defaultdict(list)
for k, e in enumerate(ents):
    end = ents[k + 1] if k + 1 < len(ents) else e + 0x2000
    if end - e > 0x20000: end = e + 0x20000
    ins = list(Decoder(32, bytes(img[e - B:end - B]), ip=e))
    last_call = 0
    for i, x in enumerate(ins):
        if x.flow_control in (FlowControl.CALL, FlowControl.INDIRECT_CALL): last_call = i
        if not (x.mnemonic == Mnemonic.MOV and x.op0_kind == OpKind.REGISTER and x.op1_kind == OpKind.MEMORY and x.memory_base == Register.NONE
                and x.memory_index == Register.NONE and x.memory_displacement in G): continue
        iface = G[x.memory_displacement]; this = {x.op0_register}; vt = set(); meth = {}
        for j in range(i + 1, min(i + 30, len(ins))):
            y = ins[j]
            if y.mnemonic == Mnemonic.MOV and y.op0_kind == OpKind.REGISTER and y.op1_kind == OpKind.REGISTER and y.op1_register in this:
                this.add(y.op0_register); continue
            if y.mnemonic == Mnemonic.MOV and y.op0_kind == OpKind.REGISTER and y.op1_kind == OpKind.MEMORY and y.memory_base in this and y.memory_displacement == 0 and y.memory_index == Register.NONE:
                vt.add(y.op0_register); continue
            if y.mnemonic == Mnemonic.MOV and y.op0_kind == OpKind.REGISTER and y.op1_kind == OpKind.MEMORY and y.memory_base in vt and y.memory_index == Register.NONE:
                meth[y.op0_register] = y.memory_displacement; continue
            if y.flow_control == FlowControl.INDIRECT_CALL:
                off = None
                if y.op0_kind == OpKind.REGISTER and y.op0_register in meth: off = meth[y.op0_register]
                elif y.op0_kind == OpKind.MEMORY and y.memory_base in vt: off = y.memory_displacement
                if off is None: break
                pushes = sum(1 for z in ins[last_call + 1:j] if z.mnemonic == Mnemonic.PUSH)
                nxt = ins[j + 1] if j + 1 < len(ins) else None
                uses_edx = bool(nxt and ('edx' in str(nxt).lower()) and nxt.op1_kind == OpKind.REGISTER and nxt.op1_register == Register.EDX)
                res[(iface, off // 4)][(pushes, uses_edx)] += 1; sites[(iface, off // 4)].append(y.ip)
                break
            if y.flow_control in (FlowControl.CALL, FlowControl.RETURN, FlowControl.UNCONDITIONAL_BRANCH): break
for (iface, slot), c in sorted(res.items()):
    print('%-20s slot %2d (+0x%02x)  pushes/edx-read: %s  e.g. %08x' % (iface, slot, slot * 4, dict(c), sites[(iface, slot)][0]))
