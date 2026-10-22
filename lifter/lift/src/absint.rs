//! Abstract interpretation of register/stack identity: tracks esp/ebp as offsets from the entry esp, which values are
//! the caller's callee-saved registers (saved to / restored from stack slots), and classifies memory accesses
//! (stack-derived => LOCAL, otherwise SHARED). Produces the register-effect facts used for ABI verification.

use ir::*;
use std::collections::HashMap;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum AV {
    Top,
    Entry(Gpr),
    Delta(i32),
    Const(u32),
}

#[derive(Clone, PartialEq, Debug)]
struct State {
    regs: [AV; 8],
    slots: Vec<(i32, AV)>,
    arg_bytes: i32,
    last_dec: bool,
}

impl State {
    fn entry() -> State {
        let mut regs = [AV::Top; 8];
        regs[Gpr::Esp as usize] = AV::Delta(0);
        for g in [Gpr::Ebx, Gpr::Ebp, Gpr::Esi, Gpr::Edi] {
            regs[g as usize] = AV::Entry(g);
        }
        State { regs, slots: Vec::new(), arg_bytes: 0, last_dec: false }
    }
    fn slot_get(&self, k: i32) -> Option<AV> {
        self.slots.binary_search_by_key(&k, |s| s.0).ok().map(|i| self.slots[i].1)
    }
    fn slot_set(&mut self, k: i32, v: AV) {
        match self.slots.binary_search_by_key(&k, |s| s.0) {
            Ok(i) => self.slots[i].1 = v,
            Err(i) => self.slots.insert(i, (k, v)),
        }
    }
    fn slot_invalidate(&mut self, k: i32, size: i32) {
        self.slots.retain(|&(sk, _)| !(sk + 4 > k && sk < k + size));
    }
}

fn join_av(a: AV, b: AV) -> AV {
    if a == b {
        a
    } else {
        AV::Top
    }
}

/// How the pop count of a callee is known to the analysis.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PopInfo {
    Known(u16),
    Unknown,
}

#[derive(Clone, Debug, Default)]
pub struct CalleeInfo {
    pub pop: Option<u16>,
    pub ext_in: u8,  // mask over [ebx, esi, edi, ebp] bit order below
    pub ext_out: u8, // same
}

pub fn ext_bit(g: Gpr) -> u8 {
    match g {
        Gpr::Ebx => 1,
        Gpr::Esi => 2,
        Gpr::Edi => 4,
        Gpr::Ebp => 8,
        _ => 0,
    }
}
pub const EXT_REGS: [Gpr; 4] = [Gpr::Ebx, Gpr::Esi, Gpr::Edi, Gpr::Ebp];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Guess {
    /// unknown-pop calls: callee pops the bytes pushed for this call
    PopsArgs,
    /// unknown-pop calls: callee pops nothing
    Cdecl,
    /// per call site (bit i = site i pops its arguments)
    Mask(u64),
}

#[derive(Clone, Debug, Default)]
pub struct RetFact {
    pub blk: u32,
    pub pop_imm: Option<u16>,   // None for tail calls
    pub esp: Option<i32>,       // esp delta before the ret (None = unknown)
    pub modified: u8,           // ext-reg mask provably modified (stack tracking ok)
    pub unknown_mask: u8,       // ext regs whose state is unknown (tracking lost)
    pub is_tail: bool,
}

#[derive(Clone, Debug, Default)]
pub struct AbsRes {
    pub rets: Vec<RetFact>,
    pub input_use: u8,
    pub reads_ret_slot: bool,
    pub writes_ret_slot: bool,
    pub stack_ok: bool,
    pub guess_used: Option<Guess>,
    /// blocks never reached by the analysis
    pub unreachable: Vec<u32>,
    /// tail-call edges (block, target) with esp delta != 0
    pub bad_tail: Vec<(u32, u32)>,
    pub n_local: u32,
    pub n_shared: u32,
    pub esp_unknown_at_call: u32,
    pub use_sites: Vec<(u32, u32, String)>,
}

pub struct Analyzer<'a> {
    pub callee: &'a dyn Fn(&CallTarget) -> CalleeInfo,
    pub guess: Guess,
    pub sites: HashMap<(u32, u32), u32>,
    pub cur: std::cell::Cell<(u32, u32)>,
}

struct Run {
    input_use: u8,
    reads_ret: bool,
    writes_ret: bool,
    stack_ok: bool,
}

impl<'a> Analyzer<'a> {
    fn val(&self, st: &State, tm: &[AV], o: &Opnd) -> AV {
        match o {
            Opnd::Imm(v) => AV::Const(*v as u32),
            Opnd::P(Place::Gpr(g)) => st.regs[*g as usize],
            Opnd::P(Place::Tmp(t)) => tm.get(*t as usize).copied().unwrap_or(AV::Top),
            _ => AV::Top,
        }
    }
    fn pval(&self, st: &State, tm: &[AV], p: &Place) -> AV {
        match p {
            Place::Gpr(g) => st.regs[*g as usize],
            Place::Tmp(t) => tm.get(*t as usize).copied().unwrap_or(AV::Top),
            _ => AV::Top,
        }
    }
    fn setp(&self, st: &mut State, tm: &mut [AV], p: &Place, v: AV) {
        match p {
            Place::Gpr(g) => {
                st.regs[*g as usize] = v;
                if *g == Gpr::Esp {
                    st.last_dec = false;
                }
            }
            Place::Tmp(t) => {
                if (*t as usize) < tm.len() {
                    tm[*t as usize] = v
                }
            }
            _ => {}
        }
    }
    fn mark(run: &mut Run, v: AV) {
        if let AV::Entry(g) = v {
            run.input_use |= ext_bit(g);
        }
    }
    fn use_opnd(&self, run: &mut Run, st: &State, tm: &[AV], o: &Opnd) {
        Self::mark(run, self.val(st, tm, o));
    }
    fn use_mem(&self, run: &mut Run, st: &State, tm: &[AV], m: &Mem) {
        if let Some(b) = &m.base {
            Self::mark(run, self.pval(st, tm, b));
        }
        if let Some(x) = &m.index {
            Self::mark(run, self.pval(st, tm, x));
        }
    }
    /// stack slot key of a memory operand if it is a plain stack-derived address
    fn stack_key(&self, st: &State, tm: &[AV], m: &Mem) -> Option<i32> {
        if m.index.is_some() || m.fs {
            return None;
        }
        match m.base {
            Some(b) => match self.pval(st, tm, &b) {
                AV::Delta(k) => Some(k.wrapping_add(m.disp)),
                _ => None,
            },
            None => None,
        }
    }
    fn is_local(&self, st: &State, tm: &[AV], m: &Mem) -> bool {
        if m.fs {
            return true;
        }
        let stackish = |p: &Option<Place>| match p {
            Some(Place::Gpr(Gpr::Esp)) => true,
            Some(p) => matches!(self.pval(st, tm, p), AV::Delta(_)),
            None => false,
        };
        stackish(&m.base) || (m.base.is_none() && stackish(&m.index)) || (m.base.is_some() && stackish(&m.index) && m.scale == 1)
    }

    fn step(&self, run: &mut Run, st: &mut State, tm: &mut Vec<AV>, op: &Op, ann: Option<&mut Op>) {
        // `ann` is the mutable alias of `op` used to annotate memory classes in the final pass
        let mut cls_fix: Option<(MemCls, bool)> = None;
        match op {
            Op::Mov { ty, d, a } => {
                let v = self.val(st, tm, a);
                if *ty == Ty::I32 {
                    self.setp(st, tm, d, v);
                } else {
                    Self::mark(run, v);
                    self.setp(st, tm, d, AV::Top);
                }
            }
            Op::Bin { op: bop, ty, d, a, b } => {
                let (va, vb) = (self.val(st, tm, a), self.val(st, tm, b));
                let mut r = AV::Top;
                let mut used = true;
                // value-independent idioms: xor/sub r,r ; or r,-1 ; and r,0
                if matches!(bop, BinOp::Xor | BinOp::Sub) && a == b && *ty != Ty::I32 {
                    used = false;
                }
                if *ty == Ty::I32 {
                    if (matches!(bop, BinOp::Xor | BinOp::Sub) && a == b) || (matches!(bop, BinOp::And) && matches!(b, Opnd::Imm(0))) {
                        r = AV::Const(0);
                        used = false;
                    } else if matches!(bop, BinOp::Or) && matches!(b, Opnd::Imm(0xffff_ffff)) {
                        r = AV::Const(0xffff_ffff);
                        used = false;
                    }
                }
                if *ty == Ty::I32 && used {
                    match (bop, va, vb) {
                        (BinOp::Add, AV::Delta(k), AV::Const(c)) | (BinOp::Add, AV::Const(c), AV::Delta(k)) => {
                            r = AV::Delta(k.wrapping_add(c as i32));
                            used = false;
                        }
                        (BinOp::Sub, AV::Delta(k), AV::Const(c)) => {
                            r = AV::Delta(k.wrapping_sub(c as i32));
                            used = false;
                        }
                        (BinOp::Add, AV::Const(x), AV::Const(y)) => {
                            r = AV::Const(x.wrapping_add(y));
                            used = false;
                        }
                        (BinOp::Sub, AV::Const(x), AV::Const(y)) => {
                            r = AV::Const(x.wrapping_sub(y));
                            used = false;
                        }
                        _ => {}
                    }
                }
                if used {
                    Self::mark(run, va);
                    Self::mark(run, vb);
                }
                // push/pop pattern tracking
                if let Place::Gpr(Gpr::Esp) = d {
                    let dec4 = matches!(bop, BinOp::Sub) && matches!(b, Opnd::Imm(4)) && matches!(a, Opnd::P(Place::Gpr(Gpr::Esp)));
                    self.setp(st, tm, d, r);
                    st.last_dec = dec4;
                } else {
                    self.setp(st, tm, d, r);
                }
            }
            Op::Un { d, a, .. } | Op::Cast { d, a, .. } => {
                let v = self.val(st, tm, a);
                Self::mark(run, v);
                self.setp(st, tm, d, AV::Top);
            }
            Op::Sel { d, c, a, b, .. } => {
                for o in [c, a, b] {
                    let v = self.val(st, tm, o);
                    Self::mark(run, v);
                }
                self.setp(st, tm, d, AV::Top);
            }
            Op::Cmp { d, a, b, .. } => {
                for o in [a, b] {
                    let v = self.val(st, tm, o);
                    Self::mark(run, v);
                }
                self.setp(st, tm, d, AV::Top);
            }
            Op::Lea { d, m } => {
                let v = if m.fs {
                    AV::Top
                } else if m.index.is_none() {
                    match m.base {
                        Some(b) => match self.pval(st, tm, &b) {
                            AV::Delta(k) => AV::Delta(k.wrapping_add(m.disp)),
                            AV::Const(c) => AV::Const(c.wrapping_add(m.disp as u32)),
                            _ => {
                                self.use_mem(run, st, tm, m);
                                AV::Top
                            }
                        },
                        None => AV::Const(m.disp as u32),
                    }
                } else {
                    self.use_mem(run, st, tm, m);
                    AV::Top
                };
                self.setp(st, tm, d, v);
            }
            Op::Ld { ty, d, m } => {
                let local = self.is_local(st, tm, m);
                cls_fix = Some((if local { MemCls::Local } else { MemCls::Shared }, false));
                let key = self.stack_key(st, tm, m);
                let mut v = AV::Top;
                if let Some(k) = key {
                    if k == 0 {
                        run.reads_ret = true;
                    }
                    if *ty == Ty::I32 {
                        if let Some(sv) = st.slot_get(k) {
                            v = sv;
                        }
                    }
                } else {
                    self.use_mem(run, st, tm, m);
                }
                self.setp(st, tm, d, v);
            }
            Op::St { ty, m, v } => {
                let local = self.is_local(st, tm, m);
                cls_fix = Some((if local { MemCls::Local } else { MemCls::Shared }, false));
                let key = self.stack_key(st, tm, m);
                let val = self.val(st, tm, v);
                if let Some(k) = key {
                    if k == 0 && !st.last_dec {
                        run.writes_ret = true;
                    }
                    if *ty == Ty::I32 {
                        // a push of a non-saved value counts as an argument
                        if st.last_dec && m.disp == 0 {
                            let is_save = matches!(val, AV::Entry(_));
                            if !is_save {
                                st.arg_bytes += 4;
                            }
                        }
                        st.slot_set(k, val);
                    } else {
                        st.slot_invalidate(k, ty.bytes() as i32);
                        Self::mark(run, val);
                    }
                } else {
                    Self::mark(run, val);
                    self.use_mem(run, st, tm, m);
                }
                st.last_dec = false;
            }
            Op::SetPart { r, v, .. } => {
                let vv = self.val(st, tm, v);
                Self::mark(run, vv);
                st.regs[*r as usize] = AV::Top;
            }
            Op::MulWide { hi, lo, a, b, .. } => {
                for o in [a, b] {
                    let v = self.val(st, tm, o);
                    Self::mark(run, v);
                }
                self.setp(st, tm, hi, AV::Top);
                self.setp(st, tm, lo, AV::Top);
            }
            Op::DivWide { q, r, hi, lo, div, .. } => {
                for o in [hi, lo, div] {
                    let v = self.val(st, tm, o);
                    Self::mark(run, v);
                }
                self.setp(st, tm, q, AV::Top);
                self.setp(st, tm, r, AV::Top);
            }
            Op::AtomicRmw { old, m, v, .. } => {
                cls_fix = Some((MemCls::Shared, false));
                self.use_mem(run, st, tm, m);
                let vv = self.val(st, tm, v);
                Self::mark(run, vv);
                self.setp(st, tm, old, AV::Top);
            }
            Op::AtomicCas { old, m, expected, new, .. } => {
                cls_fix = Some((MemCls::Shared, false));
                self.use_mem(run, st, tm, m);
                for o in [expected, new] {
                    let v = self.val(st, tm, o);
                    Self::mark(run, v);
                }
                self.setp(st, tm, old, AV::Top);
            }
            Op::FlagDef(d) => {
                for o in [&d.a, &d.b, &d.r, &d.cin, &d.cnt] {
                    let v = self.val(st, tm, o);
                    Self::mark(run, v);
                }
            }
            Op::FlagGet { d, .. } | Op::FlagGetBit { d, .. } | Op::FlagsPack { d } | Op::GetDf { d } => self.setp(st, tm, d, AV::Top),
            Op::Call { target, .. } => {
                let ci = (self.callee)(target);
                if let CallTarget::Indirect(o) = target {
                    let v = self.val(st, tm, o);
                    Self::mark(run, v);
                }
                for g in [Gpr::Eax, Gpr::Ecx, Gpr::Edx] {
                    let v = st.regs[g as usize];
                    Self::mark(run, v);
                }
                for g in EXT_REGS {
                    if ci.ext_in & ext_bit(g) != 0 {
                        let v = st.regs[g as usize];
                        Self::mark(run, v);
                    }
                }
                let pop: Option<i32> = match ci.pop {
                    Some(p) => Some(p as i32),
                    None => match self.guess {
                        Guess::PopsArgs => Some(st.arg_bytes),
                        Guess::Cdecl => Some(0),
                        Guess::Mask(m) => {
                            let id = self.sites.get(&self.cur.get()).copied().unwrap_or(0);
                            Some(if (m >> id) & 1 == 1 { st.arg_bytes } else { 0 })
                        }
                    },
                };
                match (st.regs[Gpr::Esp as usize], pop) {
                    (AV::Delta(k), Some(p)) => st.regs[Gpr::Esp as usize] = AV::Delta(k + p),
                    _ => st.regs[Gpr::Esp as usize] = AV::Top,
                }
                for g in [Gpr::Eax, Gpr::Ecx, Gpr::Edx] {
                    st.regs[g as usize] = AV::Top;
                }
                for g in EXT_REGS {
                    if ci.ext_out & ext_bit(g) != 0 {
                        st.regs[g as usize] = AV::Top;
                    }
                }
                st.arg_bytes = 0;
                st.last_dec = false;
            }
            Op::Str { kind, rep, ta, tb, done, src_local, dst_local, .. } => {
                let (esi, edi) = (st.regs[Gpr::Esi as usize], st.regs[Gpr::Edi as usize]);
                let _ = (src_local, dst_local);
                let (ru_s, ru_d, ru_a) = str_regs(*kind);
                if ru_s {
                    Self::mark(run, esi);
                }
                if ru_d {
                    Self::mark(run, edi);
                }
                if ru_a {
                    let v = st.regs[Gpr::Eax as usize];
                    Self::mark(run, v);
                }
                cls_fix = Some((MemCls::Unknown, false));
                let sl = matches!(esi, AV::Delta(_));
                let dl = matches!(edi, AV::Delta(_));
                if let Some(a) = ann {
                    if let Op::Str { src_local, dst_local, .. } = a {
                        *src_local = sl;
                        *dst_local = dl;
                    }
                }
                if ru_s {
                    st.regs[Gpr::Esi as usize] = AV::Top;
                }
                if ru_d {
                    st.regs[Gpr::Edi as usize] = AV::Top;
                }
                if *rep != Rep::None {
                    st.regs[Gpr::Ecx as usize] = AV::Top;
                }
                if *kind == StrKind::Lods {
                    st.regs[Gpr::Eax as usize] = AV::Top;
                }
                if matches!(kind, StrKind::Cmps | StrKind::Scas) {
                    for p in [ta, tb, done] {
                        self.setp(st, tm, p, AV::Top);
                    }
                }
                st.last_dec = false;
                return;
            }
            Op::FLdM { m, .. } | Op::FStM { m, .. } => {
                let local = self.is_local(st, tm, m);
                cls_fix = Some((if local { MemCls::Local } else { MemCls::Shared }, false));
                if let Op::FStM { fmt, .. } = op {
                    if let Some(k) = self.stack_key(st, tm, m) {
                        let sz = match fmt {
                            FMem::F32 | FMem::I32 => 4,
                            FMem::F64 | FMem::I64 => 8,
                            FMem::I16 => 2,
                            FMem::F80 => 10,
                        };
                        st.slot_invalidate(k, sz);
                    }
                }
                self.use_mem(run, st, tm, m);
            }
            Op::VLd { m, .. } | Op::VSt { m, .. } => {
                let local = self.is_local(st, tm, m);
                cls_fix = Some((if local { MemCls::Local } else { MemCls::Shared }, false));
                if let Op::VSt { w, .. } = op {
                    if let Some(k) = self.stack_key(st, tm, m) {
                        st.slot_invalidate(k, *w as i32);
                    }
                }
                self.use_mem(run, st, tm, m);
            }
            Op::Misc(MiscOp::Cpuid) => {
                for g in [Gpr::Eax, Gpr::Ebx, Gpr::Ecx, Gpr::Edx] {
                    st.regs[g as usize] = AV::Top;
                }
            }
            Op::Misc(MiscOp::Rdtsc) => {
                st.regs[Gpr::Eax as usize] = AV::Top;
                st.regs[Gpr::Edx as usize] = AV::Top;
            }
            Op::Misc(MiscOp::Stmxcsr { d }) => self.setp(st, tm, d, AV::Top),
            Op::Misc(MiscOp::Prefetch(m)) => {
                let local = self.is_local(st, tm, m);
                cls_fix = Some((if local { MemCls::Local } else { MemCls::Shared }, false));
            }
            other => {
                // generic: conservatively mark uses, set defs to Top
                other.visit_uses(&mut |p| {
                    if let Place::Gpr(g) = p {
                        Self::mark(run, st.regs[g as usize]);
                    }
                });
                let mut defs = Vec::new();
                other.visit_defs(&mut |p| defs.push(p));
                for p in defs {
                    self.setp(st, tm, &p, AV::Top);
                }
            }
        }
        if !matches!(op, Op::St { .. } | Op::Bin { .. }) {
            // keep last_dec only across the esp decrement and the following store
            if !matches!(op, Op::Ld { .. }) {
                st.last_dec = false;
            }
        }
        if let (Some((cls, _)), Some(a)) = (cls_fix, ann) {
            let set = |m: &mut Mem, local: MemCls, size: u32| {
                m.cls = local;
                if m.base.is_none() && m.index.is_none() && !m.fs {
                    m.aligned = size <= 1 || (m.disp as u32) % size == 0;
                } else if size <= 1 {
                    m.aligned = true;
                }
            };
            match a {
                Op::Ld { m, ty, .. } | Op::St { m, ty, .. } => set(m, cls, ty.bytes()),
                Op::AtomicRmw { m, ty, .. } | Op::AtomicCas { m, ty, .. } => set(m, cls, ty.bytes()),
                Op::FLdM { m, fmt, .. } | Op::FStM { m, fmt, .. } => set(
                    m,
                    cls,
                    match fmt {
                        FMem::F32 | FMem::I32 => 4,
                        FMem::F64 | FMem::I64 => 8,
                        FMem::I16 => 2,
                        FMem::F80 => 8,
                    },
                ),
                Op::VLd { m, w, .. } | Op::VSt { m, w, .. } => set(m, cls, *w as u32),
                Op::Misc(MiscOp::Prefetch(m)) => set(m, cls, 1),
                _ => {}
            }
        }
    }

    /// Run to a fixpoint and (optionally) annotate memory classes in `func`.
    pub fn run(&self, func: &mut Func, annotate: bool) -> AbsRes {
        let nb = func.blocks.len();
        let mut res = AbsRes { stack_ok: true, ..Default::default() };
        let mut ins: Vec<Option<State>> = vec![None; nb];
        ins[0] = Some(State::entry());
        let mut work = vec![0u32];
        let mut run = Run { input_use: 0, reads_ret: false, writes_ret: false, stack_ok: true };
        let mut tm: Vec<AV> = vec![AV::Top; func.tmp_ty.len()];
        let mut iters = 0usize;
        let mut mismatch = false;
        while let Some(b) = work.pop() {
            iters += 1;
            if iters > 50 * nb + 1000 {
                mismatch = true;
                break;
            }
            let b = b as usize;
            let mut st = ins[b].clone().unwrap();
            for t in tm.iter_mut() {
                *t = AV::Top;
            }
            st.arg_bytes = 0;
            for ii in 0..func.blocks[b].insts.len() {
                let op = func.blocks[b].insts[ii].op.clone();
                self.cur.set((b as u32, ii as u32));
                self.step(&mut run, &mut st, &mut tm, &op, None);
            }
            for s in func.blocks[b].term.succs() {
                let s = s as usize;
                let nst = match &mut ins[s] {
                    None => {
                        ins[s] = Some(st.clone());
                        work.push(s as u32);
                        continue;
                    }
                    Some(cur) => {
                        let mut n = cur.clone();
                        for k in 0..8 {
                            n.regs[k] = join_av(cur.regs[k], st.regs[k]);
                        }
                        if cur.regs[Gpr::Esp as usize] != st.regs[Gpr::Esp as usize] {
                            mismatch = true;
                        }
                        n.slots = cur.slots.iter().filter(|(k, v)| st.slot_get(*k) == Some(*v)).copied().collect();
                        n.arg_bytes = cur.arg_bytes.max(st.arg_bytes);
                        n.last_dec = false;
                        n
                    }
                };
                if Some(&nst) != ins[s].as_ref() {
                    ins[s] = Some(nst);
                    work.push(s as u32);
                }
            }
        }
        // final pass: collect ret facts and annotate
        let mut annot_run = Run { input_use: 0, reads_ret: false, writes_ret: false, stack_ok: true };
        // blocks from which a return / tail call is reachable (uses on noreturn-only paths do not make inputs)
        let mut reach_exit = vec![false; nb];
        for b in 0..nb {
            if matches!(func.blocks[b].term, Term::Ret { .. } | Term::TailCall(_)) {
                reach_exit[b] = true;
            }
        }
        loop {
            let mut ch = false;
            for b in 0..nb {
                if !reach_exit[b] && func.blocks[b].term.succs().iter().any(|&s| reach_exit[s as usize]) {
                    reach_exit[b] = true;
                    ch = true;
                }
            }
            if !ch {
                break;
            }
        }
        for b in 0..nb {
            let use_before = annot_run.input_use;
            let mut st = match &ins[b] {
                Some(s) => s.clone(),
                None => {
                    res.unreachable.push(b as u32);
                    continue;
                }
            };
            for t in tm.iter_mut() {
                *t = AV::Top;
            }
            let ninsts = func.blocks[b].insts.len();
            st.arg_bytes = 0;
            for ii in 0..ninsts {
                let op = func.blocks[b].insts[ii].op.clone();
                self.cur.set((b as u32, ii as u32));
                let ub = annot_run.input_use;
                if annotate {
                    let mut op2 = op.clone();
                    self.step(&mut annot_run, &mut st, &mut tm, &op, Some(&mut op2));
                    func.blocks[b].insts[ii].op = op2;
                } else {
                    self.step(&mut annot_run, &mut st, &mut tm, &op, None);
                }
                if std::env::var("LX_DEBUG_ESP").is_ok() && matches!(op, Op::Call { .. }) {
                    res.use_sites.push((b as u32, ii as u32, format!("after {:?}: esp={:?} ebp={:?}", op, st.regs[Gpr::Esp as usize], st.regs[Gpr::Ebp as usize])));
                }
                if annot_run.input_use != ub && reach_exit[b] && res.use_sites.len() < 6 && std::env::var("LX_DEBUG_ESP").is_err() {
                    res.use_sites.push((b as u32, ii as u32, format!("{:?}", op)));
                }
            }
            match &func.blocks[b].term {
                Term::Ret { pop } => {
                    let esp = match st.regs[Gpr::Esp as usize] {
                        AV::Delta(k) => Some(k),
                        _ => None,
                    };
                    for g in [Gpr::Eax, Gpr::Edx] {
                        let v = st.regs[g as usize];
                        Self::mark(&mut annot_run, v);
                    }
                    let mut rf = RetFact { blk: b as u32, pop_imm: Some(*pop), esp, ..Default::default() };
                    for g in EXT_REGS {
                        let v = st.regs[g as usize];
                        if v != AV::Entry(g) {
                            if matches!(v, AV::Delta(_) | AV::Const(_)) || esp.is_some() {
                                rf.modified |= ext_bit(g);
                            } else {
                                // value reloaded through a stack we could not track: treat as modified (returning the
                                // register is always correct, dropping it is not)
                                rf.unknown_mask |= ext_bit(g);
                                rf.modified |= ext_bit(g);
                            }
                        }
                    }
                    res.rets.push(rf);
                }
                Term::TailCall(t) => {
                    let esp = match st.regs[Gpr::Esp as usize] {
                        AV::Delta(k) => Some(k),
                        _ => None,
                    };
                    if let CallTarget::Indirect(o) = t {
                        let v = self.val(&st, &tm, o);
                        Self::mark(&mut annot_run, v);
                    }
                    for g in [Gpr::Eax, Gpr::Ecx, Gpr::Edx] {
                        let v = st.regs[g as usize];
                        Self::mark(&mut annot_run, v);
                    }
                    let ci = match t {
                        CallTarget::Direct(_) | CallTarget::Indirect(_) => (self.callee)(t),
                        _ => CalleeInfo::default(),
                    };
                    for g in EXT_REGS {
                        if ci.ext_in & ext_bit(g) != 0 {
                            let v = st.regs[g as usize];
                            Self::mark(&mut annot_run, v);
                        }
                    }
                    let mut rf = RetFact { blk: b as u32, pop_imm: None, esp, is_tail: true, ..Default::default() };
                    for g in EXT_REGS {
                        let v = st.regs[g as usize];
                        if v != AV::Entry(g) {
                            if matches!(v, AV::Delta(_) | AV::Const(_)) || esp.is_some() {
                                rf.modified |= ext_bit(g);
                            } else {
                                // value reloaded through a stack we could not track: treat as modified (returning the
                                // register is always correct, dropping it is not)
                                rf.unknown_mask |= ext_bit(g);
                                rf.modified |= ext_bit(g);
                            }
                        }
                    }
                    if esp != Some(0) {
                        if let CallTarget::Direct(a) = t {
                            res.bad_tail.push((b as u32, *a));
                        }
                    }
                    res.rets.push(rf);
                }
                Term::Switch { idx, .. } => {
                    let v = self.val(&st, &tm, idx);
                    Self::mark(&mut annot_run, v);
                }
                Term::Br { c, .. } => {
                    let v = self.val(&st, &tm, c);
                    Self::mark(&mut annot_run, v);
                }
                _ => {}
            }
            if !reach_exit[b] {
                annot_run.input_use = use_before;
            }
        }
        // consistency of returns
        let mut ok = !mismatch;
        for r in &res.rets {
            if !r.is_tail && r.esp != Some(0) {
                ok = false;
            }
        }
        res.stack_ok = ok;
        res.input_use = annot_run.input_use;
        res.reads_ret_slot = annot_run.reads_ret;
        res.writes_ret_slot = annot_run.writes_ret;
        let _ = (run.stack_ok, run.input_use, run.reads_ret, run.writes_ret);
        res.guess_used = Some(self.guess);
        if annotate {
            for b in &func.blocks {
                for i in &b.insts {
                    let m = match &i.op {
                        Op::Ld { m, .. } | Op::St { m, .. } | Op::FLdM { m, .. } | Op::FStM { m, .. } | Op::VLd { m, .. } | Op::VSt { m, .. } | Op::AtomicRmw { m, .. } | Op::AtomicCas { m, .. } => Some(m),
                        _ => None,
                    };
                    if let Some(m) = m {
                        match m.cls {
                            MemCls::Local => res.n_local += 1,
                            _ => res.n_shared += 1,
                        }
                    }
                }
            }
        }
        res
    }
}

/// Find call sites whose callee pop is unknown (in block order).
fn unknown_sites(callee: &dyn Fn(&CallTarget) -> CalleeInfo, func: &Func) -> HashMap<(u32, u32), u32> {
    let mut m = HashMap::new();
    for (bi, b) in func.blocks.iter().enumerate() {
        for (ii, i) in b.insts.iter().enumerate() {
            if let Op::Call { target, .. } = &i.op {
                if callee(target).pop.is_none() {
                    let n = m.len() as u32;
                    m.insert((bi as u32, ii as u32), n);
                }
            }
        }
    }
    m
}

/// Try pop-guess modes (all pop, all cdecl, then per-site combinations) and keep one that yields a consistent stack.
pub fn analyze(callee: &dyn Fn(&CallTarget) -> CalleeInfo, func: &mut Func, annotate: bool) -> AbsRes {
    let sites = unknown_sites(callee, func);
    let n = sites.len();
    let mut modes: Vec<Guess> = vec![Guess::PopsArgs];
    if n > 0 {
        modes.push(Guess::Cdecl);
        if n <= 10 {
            for m in 1..((1u64 << n) - 1) {
                modes.push(Guess::Mask(m));
            }
        }
    }
    let mut first: Option<(Func, AbsRes)> = None;
    for g in modes {
        let a = Analyzer { callee, guess: g, sites: sites.clone(), cur: std::cell::Cell::new((0, 0)) };
        let mut f1 = func.clone();
        let r = a.run(&mut f1, annotate);
        if r.stack_ok {
            if annotate {
                *func = f1;
            }
            return r;
        }
        if first.is_none() {
            first = Some((f1, r));
        }
    }
    let (f1, r) = first.unwrap();
    if annotate {
        *func = f1;
    }
    r
}

pub type _Unused = HashMap<u32, u32>;
