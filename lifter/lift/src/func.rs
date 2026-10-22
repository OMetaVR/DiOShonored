//! Function formation: entry + reachable instructions (stopping at other entries, which become tail calls),
//! basic-block construction and lowering to IR.

use crate::ctx::LiftCtx;
use crate::discover::is_noise;
use iced_x86::{Decoder, DecoderOptions, FlowControl, Instruction, OpKind, Register};
use ir::*;
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};

pub struct FEnv<'a> {
    pub ctx: &'a LiftCtx,
    pub jts: HashMap<u32, Vec<u32>>,
}

impl<'a> LowerEnv for FEnv<'a> {
    fn iat_target(&self, slot: u32) -> Option<CallTarget> {
        self.ctx.iat_target(slot)
    }
    fn is_noreturn(&self, t: &CallTarget) -> bool {
        match t {
            CallTarget::Direct(a) => self.ctx.noreturn_funcs.contains(a),
            CallTarget::Hle(i) => self.ctx.hle_noreturn(*i),
            CallTarget::Indirect(_) => false,
        }
    }
    fn jump_table(&self, site: u32) -> Option<Vec<u32>> {
        self.jts.get(&site).cloned()
    }
}

#[derive(Default)]
pub struct Built {
    pub func: Func,
    pub unsupported: Vec<(u32, String)>,
    pub invalid: Vec<u32>,
    /// (block index, target) for tail-call terminators to Direct entries produced from jumps/fallthrough
    pub tail_edges: Vec<(u32, u32)>,
    pub module: usize,
    pub inlined: Vec<u32>,
}

pub fn build_func(ctx: &LiftCtx, mi: usize, entry: u32, inline: &HashSet<u32>) -> Built {
    let img = &ctx.prog.images[mi];
    let mut jts_local: HashMap<u32, Vec<u32>> = HashMap::new();
    let boundary = |t: u32| -> bool { t != entry && ctx.entry_set.contains(&t) && !inline.contains(&t) };
    let in_code = |va: u32| img.in_code(va);
    let data_ranges = &ctx.prog.cfg.modules[mi].data_ranges;
    let env = FEnv { ctx, jts: HashMap::new() };

    // ---- phase 1: reachable instructions
    let mut insns: BTreeMap<u32, Instruction> = BTreeMap::new();
    let mut leaders: BTreeSet<u32> = BTreeSet::new();
    leaders.insert(entry);
    let mut work = vec![entry];
    // EH continuations of this function are extra roots (reached through the landing pad, see add_resume)
    let conts: Vec<crate::ehcont::Cont> = ctx.resume.get(&entry).cloned().unwrap_or_default();
    for c in &conts {
        if img.in_code(c.addr) {
            leaders.insert(c.addr);
            work.push(c.addr);
        }
    }
    let mut invalid = Vec::new();
    while let Some(start) = work.pop() {
        let mut va = start;
        loop {
            if !in_code(va) || data_ranges.iter().any(|r| va >= r[0] && va < r[1]) {
                break;
            }
            if va != entry && boundary(va) {
                break;
            }
            if insns.contains_key(&va) {
                leaders.insert(va);
                break;
            }
            let rva = (va - img.base) as usize;
            let mut dec = Decoder::with_ip(32, &img.mem[rva..(rva + 16).min(img.mem.len())], va as u64, DecoderOptions::NONE);
            let i = dec.decode();
            if i.is_invalid() {
                invalid.push(va);
                break;
            }
            if is_noise(i.mnemonic()) {
                invalid.push(va);
                break;
            }
            insns.insert(va, i);
            let nxt = i.next_ip() as u32;
            match i.flow_control() {
                FlowControl::Next => {
                    va = nxt;
                }
                FlowControl::Call => {
                    let t = i.near_branch_target() as u32;
                    if ctx.noreturn_funcs.contains(&t) {
                        break;
                    }
                    if let Some(st) = ctx.import_stub_target(t) {
                        if env.is_noreturn(&st) {
                            break;
                        }
                    }
                    va = nxt;
                }
                FlowControl::IndirectCall => {
                    if i.op0_kind() == OpKind::Memory && i.memory_base() == Register::None && i.memory_index() == Register::None {
                        if let Some(t) = ctx.iat_target(i.memory_displacement32()) {
                            if env.is_noreturn(&t) {
                                break;
                            }
                        }
                    }
                    va = nxt;
                }
                FlowControl::ConditionalBranch => {
                    let t = i.near_branch_target() as u32;
                    if !boundary(t) {
                        leaders.insert(t);
                        work.push(t);
                    }
                    leaders.insert(nxt);
                    va = nxt;
                }
                FlowControl::UnconditionalBranch => {
                    let t = i.near_branch_target() as u32;
                    if !boundary(t) {
                        leaders.insert(t);
                        work.push(t);
                    }
                    break;
                }
                FlowControl::IndirectBranch => {
                    if let Some(jt) = ctx.jts.get(&va) {
                        for &t in &jt.targets {
                            if !boundary(t) {
                                leaders.insert(t);
                                work.push(t);
                            }
                        }
                        jts_local.insert(va, jt.targets.clone());
                    }
                    break;
                }
                _ => break,
            }
        }
    }
    let env = FEnv { ctx, jts: jts_local };

    // ---- phase 2: blocks
    let starts: Vec<u32> = leaders.iter().copied().filter(|a| insns.contains_key(a)).collect();
    // entry block first
    let mut order: Vec<u32> = vec![entry];
    order.extend(starts.iter().copied().filter(|&a| a != entry));
    let mut bidx: HashMap<u32, u32> = HashMap::new();
    for (i, &a) in order.iter().enumerate() {
        bidx.insert(a, i as u32);
    }
    let mut func = Func { entry, blocks: Vec::new(), tmp_ty: Vec::new(), n_x86: insns.len() as u32 };
    let mut unsupported = Vec::new();
    let mut tail_edges: Vec<(u32, u32)> = Vec::new();
    let mut synth: Vec<(u32, Term)> = Vec::new(); // synthetic blocks, appended later: (index assigned at creation)
    let nblocks = order.len() as u32;
    let mut next_synth = nblocks;
    let mut synth_for: HashMap<u32, u32> = HashMap::new();

    let mut blocks: Vec<Block> = Vec::with_capacity(order.len());
    {
        let mut lw = Lower::new(&env, &mut func.tmp_ty);
        for &start in &order {
            let mut insts: Vec<Inst> = Vec::new();
            let mut x87_ops: Vec<Op> = Vec::new();
            let mut va = start;
            let term: Term;
            let term_addr;
            loop {
                let i = match insns.get(&va) {
                    Some(i) => *i,
                    None => {
                        // fell off into a boundary entry or undecodable code
                        term_addr = va;
                        term = if ctx.entry_set.contains(&va) && va != entry {
                            Term::TailCall(CallTarget::Direct(va))
                        } else {
                            Term::Trap { why: TRAP_INVALID }
                        };
                        break;
                    }
                };
                lw.ops.clear();
                let mut ctl = lw.lower(&i);
                // calls to import stubs become calls through the slot
                for op in lw.ops.iter_mut() {
                    if let Op::Call { target, .. } = op {
                        if let CallTarget::Direct(a) = target {
                            if let Some(t) = ctx.import_stub_target(*a) {
                                *target = t;
                            }
                        }
                    }
                }
                // a jump out of the function to an import stub or to an inline-lowered helper is a tail call through it
                if let Ctl::Jmp(a) = ctl {
                    if !bidx.contains_key(&a) {
                        if let Some(nt) = ctx.import_stub_target(a) {
                            ctl = Ctl::TailInd(nt);
                        } else if ctx.overrides.get(&a).is_some_and(|o| x87_replacement(ctx, o).is_some()) {
                            ctl = Ctl::TailInd(CallTarget::Direct(a));
                        }
                    }
                }
                if let Ctl::TailInd(t) = &mut ctl {
                    if let CallTarget::Direct(a) = t {
                        if let Some(nt) = ctx.import_stub_target(*a) {
                            *t = nt;
                        }
                    }
                }
                // native x87 helpers (_CIpow, ...) replace the call by a host call on the x87 stack
                for op in lw.ops.iter_mut() {
                    if let Op::Call { target: CallTarget::Direct(a), .. } = op {
                        if let Some(o) = ctx.overrides.get(a) {
                            if let Some(rep) = x87_replacement(ctx, o) {
                                *op = Op::Mov { ty: Ty::I32, d: Place::Gpr(Gpr::Ecx), a: Opnd::P(Place::Gpr(Gpr::Ecx)) };
                                x87_ops.extend(rep);
                            }
                        }
                    }
                }
                if let Ctl::TailInd(CallTarget::Direct(a)) = &ctl {
                    if let Some(o) = ctx.overrides.get(a) {
                        if let Some(rep) = x87_replacement(ctx, o) {
                            x87_ops.extend(rep);
                            ctl = Ctl::Ret(0);
                        }
                    }
                }
                if let Some(u) = lw.unsupported.take() {
                    unsupported.push((va, u));
                }
                let ops_now: Vec<Op> = lw.ops.drain(..).collect();
                for op in ops_now {
                    if let Op::Call { target: CallTarget::Direct(h), ret_addr } = &op {
                        if let Some((hins, hpop)) = ctx.inline_helpers.get(h) {
                            // splice the helper: call pushes the return address, the body runs, its ret pops it
                            let esp = Place::Gpr(Gpr::Esp);
                            insts.push(Inst { addr: va, op: Op::Bin { op: BinOp::Sub, ty: Ty::I32, d: esp, a: Opnd::P(esp), b: Opnd::Imm(4) } });
                            insts.push(Inst { addr: va, op: Op::St { ty: Ty::I32, m: Mem::base_disp(esp, 0), v: Opnd::Imm(*ret_addr as u64) } });
                            for hi in hins {
                                lw.ops.clear();
                                let _ = lw.lower(hi);
                                for o in lw.ops.drain(..) {
                                    insts.push(Inst { addr: va, op: o });
                                }
                            }
                            lw.addr = va;
                            insts.push(Inst { addr: va, op: Op::Bin { op: BinOp::Add, ty: Ty::I32, d: esp, a: Opnd::P(esp), b: Opnd::Imm(4 + *hpop as u64) } });
                            continue;
                        }
                    }
                    let is_marker = matches!(op, Op::Mov { ty: Ty::I32, d: Place::Gpr(Gpr::Ecx), a: Opnd::P(Place::Gpr(Gpr::Ecx)) });
                    if is_marker && !x87_ops.is_empty() {
                        for o in x87_ops.drain(..) {
                            insts.push(Inst { addr: va, op: o });
                        }
                        continue;
                    }
                    insts.push(Inst { addr: va, op });
                }
                for o in x87_ops.drain(..) {
                    insts.push(Inst { addr: va, op: o });
                }
                let nxt = i.next_ip() as u32;
                let mut target_blk = |a: u32, synth: &mut Vec<(u32, Term)>, tail_edges: &mut Vec<(u32, u32)>, next_synth: &mut u32| -> u32 {
                    if let Some(&b) = bidx.get(&a) {
                        return b;
                    }
                    // not part of this function: tail call if it is an entry, else invalid
                    if let Some(&s) = synth_for.get(&a) {
                        return s;
                    }
                    let s = *next_synth;
                    *next_synth += 1;
                    synth_for.insert(a, s);
                    if ctx.entry_set.contains(&a) {
                        synth.push((s, Term::TailCall(CallTarget::Direct(a))));
                        tail_edges.push((s, a));
                    } else {
                        synth.push((s, Term::Trap { why: TRAP_INVALID }));
                    }
                    s
                };
                match ctl {
                    Ctl::Next => {
                        if bidx.contains_key(&nxt) && nxt != start {
                            term = Term::Fall(bidx[&nxt]);
                            term_addr = va;
                            break;
                        }
                        va = nxt;
                        continue;
                    }
                    Ctl::Jmp(t) => {
                        let b = target_blk(t, &mut synth, &mut tail_edges, &mut next_synth);
                        term = Term::Jmp(b);
                    }
                    Ctl::Jcc(c, t) => {
                        let tb = target_blk(t, &mut synth, &mut tail_edges, &mut next_synth);
                        let fb = target_blk(nxt, &mut synth, &mut tail_edges, &mut next_synth);
                        term = Term::Br { c, t: tb, f: fb };
                    }
                    Ctl::Switch(idx, tg) => {
                        let v: Vec<u32> = tg.iter().map(|&a| target_blk(a, &mut synth, &mut tail_edges, &mut next_synth)).collect();
                        term = Term::Switch { idx, targets: v };
                    }
                    Ctl::TailInd(t) => term = Term::TailCall(t),
                    Ctl::Ret(n) => term = Term::Ret { pop: n },
                    Ctl::Stop => term = Term::Unreachable,
                    Ctl::Trap(w) => term = Term::Trap { why: w },
                }
                term_addr = va;
                break;
            }
            blocks.push(Block { addr: start, insts, term, term_addr });
        }
    }
    for (s, t) in synth {
        debug_assert_eq!(s as usize, blocks.len());
        blocks.push(Block { addr: 0, insts: vec![], term: t, term_addr: 0 });
    }
    // direct targets outside any discovered function (misdecoded code / data) go through the dispatcher
    let known = |a: &u32| ctx.entry_set.contains(a) || ctx.overrides.contains_key(a);
    for b in blocks.iter_mut() {
        for i in b.insts.iter_mut() {
            if let Op::Call { target, .. } = &mut i.op {
                if let CallTarget::Direct(a) = target {
                    if !known(a) {
                        *target = CallTarget::Indirect(Opnd::Imm(*a as u64));
                    }
                }
            }
        }
        if let Term::TailCall(t) = &mut b.term {
            if let CallTarget::Direct(a) = t {
                if !known(a) {
                    *t = CallTarget::Indirect(Opnd::Imm(*a as u64));
                }
            }
        }
    }
    func.blocks = blocks;
    if !conts.is_empty() {
        add_resume(&mut func, &conts, &bidx, &mut tail_edges, &mut unsupported);
    }
    // tail edges from Jmp/fallthrough that were not synthetic
    for (bi, b) in func.blocks.iter().enumerate() {
        if let Term::TailCall(CallTarget::Direct(t)) = &b.term {
            if b.addr != 0 && !tail_edges.iter().any(|(s, _)| *s as usize == bi) {
                tail_edges.push((bi as u32, *t));
            }
        }
    }
    Built { func, unsupported, invalid, tail_edges, module: mi, inlined: inline.iter().copied().collect() }
}

/// EH landing pad: split the block that saves esp into the frame (`mov [ebp-10h], esp` / `mov [ebp-18h], esp`) after that store and
/// end it with ResumeSetup + Switch { normal path, one resume block per continuation }. A resume block reloads esp from the slot
/// the continuation expects (as _JumpToContinuation / the __except prologue do) and jumps to the continuation, so the analyses see
/// the resumed path with the frame's real stack state.
fn add_resume(func: &mut Func, conts: &[crate::ehcont::Cont], bidx: &HashMap<u32, u32>, tail_edges: &mut Vec<(u32, u32)>, unsupported: &mut Vec<(u32, String)>) {
    let ebp = Place::Gpr(Gpr::Ebp);
    let esp = Place::Gpr(Gpr::Esp);
    let slots: BTreeSet<i32> = conts.iter().map(|c| c.kind.esp_slot()).collect();
    let mut at: Option<(usize, usize)> = None;
    'find: for (b, blk) in func.blocks.iter().enumerate() {
        for (j, ins) in blk.insts.iter().enumerate() {
            if let Op::St { ty: Ty::I32, m, v: Opnd::P(Place::Gpr(Gpr::Esp)) } = &ins.op {
                if m.base == Some(ebp) && m.index.is_none() && !m.fs && slots.contains(&m.disp) {
                    at = Some((b, j));
                    break 'find;
                }
            }
        }
    }
    let Some((b, j)) = at else {
        unsupported.push((func.entry, "EH continuations without a frame esp save: landing pad not created".into()));
        return;
    };
    let targets_ok: Vec<&crate::ehcont::Cont> = conts.iter().filter(|c| bidx.contains_key(&c.addr)).collect();
    if targets_ok.is_empty() {
        return;
    }
    let sel = func.new_tmp(Ty::I32);
    let rest: Vec<Inst> = func.blocks[b].insts.split_off(j + 1);
    let addr = func.blocks[b].insts[j].addr;
    let next = func.blocks.len() as u32;
    let old_term = std::mem::replace(&mut func.blocks[b].term, Term::Unreachable);
    let old_term_addr = func.blocks[b].term_addr;
    func.blocks.push(Block { addr: 0, insts: rest, term: old_term, term_addr: old_term_addr });
    for te in tail_edges.iter_mut() {
        if te.0 == b as u32 {
            te.0 = next;
        }
    }
    let mut targets = vec![next];
    for c in &targets_ok {
        let ri = func.blocks.len() as u32;
        let ld = Op::Ld { ty: Ty::I32, d: esp, m: Mem::base_disp(ebp, c.kind.esp_slot()) };
        func.blocks.push(Block { addr: 0, insts: vec![Inst { addr: c.addr, op: ld }], term: Term::Jmp(bidx[&c.addr]), term_addr: c.addr });
        targets.push(ri);
    }
    func.blocks[b].insts.push(Inst { addr, op: Op::ResumeSetup { d: Place::Tmp(sel), conts: targets_ok.iter().map(|c| c.addr).collect() } });
    func.blocks[b].term = Term::Switch { idx: Opnd::P(Place::Tmp(sel)), targets };
    func.blocks[b].term_addr = addr;
}

fn x87_replacement(ctx: &LiftCtx, o: &crate::ctx::Ovr) -> Option<Vec<Op>> {
    use crate::ctx::OvrKind;
    let sym = ctx.sym_index(&o.host);
    match o.kind {
        OvrKind::X87_2 => Some(vec![
            Op::FHost { sym, n: 2, d: Place::St(1), a: Opnd::P(Place::St(0)), b: Opnd::P(Place::St(1)) },
            Op::FPop,
        ]),
        OvrKind::X87_1 => Some(vec![Op::FHost { sym, n: 1, d: Place::St(0), a: Opnd::P(Place::St(0)), b: Opnd::Imm(0) }]),
        OvrKind::Ftol32 | OvrKind::Ftol64 => {
            // the helper's own stack use: store the truncated value in the bytes below esp, as fistp/cvttsd2si into its frame would
            let wide = o.kind == OvrKind::Ftol64;
            let esp = Place::Gpr(Gpr::Esp);
            let mut v = vec![
                Op::Bin { op: BinOp::Sub, ty: Ty::I32, d: esp, a: Opnd::P(esp), b: Opnd::Imm(8) },
                Op::FStM { fmt: if wide { FMem::I64 } else { FMem::I32 }, m: Mem::base_disp(esp, 0), a: Opnd::P(Place::St(0)), trunc: true },
                Op::FPop,
                Op::Ld { ty: Ty::I32, d: Place::Gpr(Gpr::Eax), m: Mem::base_disp(esp, 0) },
            ];
            if wide {
                v.push(Op::Ld { ty: Ty::I32, d: Place::Gpr(Gpr::Edx), m: Mem::base_disp(esp, 4) });
            }
            v.push(Op::Bin { op: BinOp::Add, ty: Ty::I32, d: esp, a: Opnd::P(esp), b: Opnd::Imm(8) });
            Some(v)
        }
        OvrKind::GuestFn => None,
    }
}

/// Decode a function that is a single straight line ending in `ret [imm]`; returns the instructions before the ret.
pub fn decode_straight_line(ctx: &LiftCtx, entry: u32) -> Option<(Vec<Instruction>, u16)> {
    let mi = *ctx.entry_module.get(&entry)?;
    let img = &ctx.prog.images[mi];
    let mut va = entry;
    let mut out = Vec::new();
    loop {
        let rva = (va.checked_sub(img.base)?) as usize;
        let mut dec = Decoder::with_ip(32, img.mem.get(rva..(rva + 16).min(img.mem.len()))?, va as u64, DecoderOptions::NONE);
        let i = dec.decode();
        if i.is_invalid() || out.len() > 60 {
            return None;
        }
        match i.flow_control() {
            FlowControl::Next => {
                out.push(i);
                va = i.next_ip() as u32;
            }
            FlowControl::Return => {
                let pop = if i.op_count() > 0 { i.immediate(0) as u16 } else { 0 };
                return Some((out, pop));
            }
            _ => return None,
        }
    }
}
