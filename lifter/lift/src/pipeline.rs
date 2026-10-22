//! Whole-program analysis driver: function facts (rounds), signature fixpoint, final per-function emission.

use crate::absint::{self, ext_bit, AbsRes, CalleeInfo};
use crate::ctx::{LiftCtx, OvrKind};
use crate::emit::{Callees, Emitter, Sig};
use crate::flags;
use crate::func::{build_func, Built};
use crate::liveness;
use crate::x87;
use ir::*;
use rayon::prelude::*;
use std::collections::{BTreeSet, HashMap, HashSet};

#[derive(Clone, Debug, Default)]
pub struct Facts {
    pub module: usize,
    pub n_x86: u32,
    pub n_blocks: u32,
    pub pop_imms: BTreeSet<u16>,
    pub has_ret: bool,
    pub tails: Vec<CallTarget>,
    pub calls: Vec<u32>,
    pub n_indirect_calls: u32,
    pub input_use: u8,
    pub mod_out: u8,
    pub unknown_mask: u8,
    pub ecx_demand: Vec<u32>, // direct callees for which ecx is live after the call
    pub ecx_demand_indirect: u32,
    pub reads_ret: bool,
    pub stack_ok: bool,
    pub n_unsupported: u32,
    pub n_invalid: u32,
    pub x87_anoms: u32,
    /// (address, kind) of each x87 anomaly; the block is replaced by a trap (written to report/x87.tsv)
    pub x87_anom_list: Vec<(u32, &'static str)>,
    pub returns_st0: bool,
    pub n_traps: u32,
    pub inline: Vec<u32>,
    pub bad_tails: u32,
    pub ecx_in: bool,
    /// some `ret` is reached with esp not at the entry level: the popped size is dynamic
    pub pop_dynamic: bool,
    /// instruction categories present (CAT_* bits)
    pub cats: u32,
}

/// functions replaced by a native host symbol have no lifted body
pub fn skip_body(ctx: &LiftCtx, a: u32) -> bool {
    ctx.overrides.get(&a).map(|o| o.kind == OvrKind::GuestFn).unwrap_or(false)
}

pub const CAT_X87: u32 = 1;
pub const CAT_SSE: u32 = 2;
pub const CAT_STR: u32 = 4;
pub const CAT_ATOMIC: u32 = 8;
pub const CAT_FS: u32 = 16;
pub const CAT_CALL: u32 = 32;
pub const CAT_ICALL: u32 = 64;
pub const CAT_SWITCH: u32 = 128;
pub const CAT_SHIFT: u32 = 256;
pub const CAT_MULDIV: u32 = 512;
pub const CAT_ADC: u32 = 1024;
pub const CAT_FLAGS: u32 = 2048;
pub const CAT_TAIL: u32 = 4096;
pub const CAT_EXT: u32 = 8192;
pub const CAT_NAMES: [&str; 14] = ["x87", "sse", "string", "atomic", "fs", "call", "icall", "switch", "shift", "muldiv", "adc", "flags", "tail", "ext"];

fn op_cats(op: &Op) -> u32 {
    let mut c = 0;
    if op.is_x87() {
        c |= CAT_X87;
    }
    match op {
        Op::V { .. } | Op::VLd { .. } | Op::VSt { .. } => c |= CAT_SSE,
        Op::Str { .. } => c |= CAT_STR,
        Op::AtomicRmw { .. } | Op::AtomicCas { .. } => c |= CAT_ATOMIC,
        Op::Ld { m, .. } | Op::St { m, .. } if m.fs => c |= CAT_FS,
        Op::Call { target: CallTarget::Indirect(_), .. } => c |= CAT_ICALL,
        Op::Call { .. } => c |= CAT_CALL,
        Op::MulWide { .. } | Op::DivWide { .. } => c |= CAT_MULDIV,
        Op::FlagsPack { .. } => c |= CAT_FLAGS,
        Op::FlagDef(d) => match d.kind {
            FlagKind::Adc | FlagKind::Sbb => c |= CAT_ADC,
            FlagKind::Shl | FlagKind::Shr | FlagKind::Sar | FlagKind::Rol | FlagKind::Ror => c |= CAT_SHIFT,
            _ => {}
        },
        _ => {}
    }
    c
}

pub struct SigTab<'a> {
    pub ctx: &'a LiftCtx,
    pub sigs: &'a HashMap<u32, Sig>,
}

impl<'a> Callees for SigTab<'a> {
    fn sig(&self, addr: u32) -> Sig {
        self.sigs.get(&addr).cloned().unwrap_or_default()
    }
    fn ovr(&self, addr: u32) -> Option<(String, OvrKind)> {
        self.ctx.overrides.get(&addr).map(|o| (o.host.clone(), o.kind))
    }
    fn ovr_pop(&self, addr: u32) -> Option<u16> {
        self.ctx.overrides.get(&addr).and_then(|o| o.pop)
    }
    fn sym_index(&self, host: &str) -> u32 {
        self.ctx.sym_index(host)
    }
    fn sym_name(&self, idx: u32) -> String {
        self.ctx.sym_name(idx)
    }
}

fn callee_info(ctx: &LiftCtx, sigs: &HashMap<u32, Sig>, t: &CallTarget) -> CalleeInfo {
    match t {
        CallTarget::Direct(a) => {
            if let Some(o) = ctx.overrides.get(a) {
                if o.kind == OvrKind::GuestFn {
                    return CalleeInfo { pop: o.pop, ext_in: 0, ext_out: 0 };
                }
            }
            match sigs.get(a) {
                Some(s) => CalleeInfo { pop: s.pop, ext_in: s.ext_in, ext_out: s.ext_out & 15 },
                None => CalleeInfo::default(),
            }
        }
        // An indirect call may reach an EH funclet (catch/__finally/filter), which runs on its parent's frame through ebp: the call
        // reads ebp. Funclet trampolines such as _NLG_Call (`call eax`) never touch ebp themselves, so this makes ebp an input of
        // every function up to the one that sets it (GuestThread.ext_regs carries it across the dispatch).
        CallTarget::Indirect(_) => CalleeInfo { ext_in: ext_bit(Gpr::Ebp), ..CalleeInfo::default() },
        _ => CalleeInfo::default(),
    }
}

/// Whether a direct call's target returns a value in st(0), when its signature is known.
fn st0_known(sigs: &HashMap<u32, Sig>, t: &CallTarget) -> Option<bool> {
    match t {
        CallTarget::Direct(a) => sigs.get(a).map(|s| s.st0_ret),
        _ => None,
    }
}

/// Build a function, resolve x87, run the stack analysis; retry with inlined entries for tail edges whose stack is not clean.
pub fn analyze_function(ctx: &LiftCtx, sigs: &HashMap<u32, Sig>, mi: usize, entry: u32, inline0: &[u32], annotate: bool) -> (Built, AbsRes, x87::X87Info) {
    let mut inline: HashSet<u32> = inline0.iter().copied().collect();
    let ci = |t: &CallTarget| callee_info(ctx, sigs, t);
    for _round in 0..6 {
        let mut built = build_func(ctx, mi, entry, &inline);
        let xi = x87::resolve(&mut built.func, &|t| st0_known(sigs, t));
        let abs = absint::analyze(&ci, &mut built.func, annotate);
        let mut grew = false;
        for &(_, t) in &abs.bad_tail {
            if t != entry && inline.insert(t) {
                grew = true;
            }
        }
        if !grew || _round == 5 {
            built.inlined = inline.iter().copied().collect();
            built.inlined.sort_unstable();
            return (built, abs, xi);
        }
    }
    unreachable!()
}

pub fn compute_facts(ctx: &LiftCtx, sigs: &HashMap<u32, Sig>, mi: usize, entry: u32, inline0: &[u32], with_abs: bool) -> Facts {
    let (built, abs, xi) = if with_abs {
        analyze_function(ctx, sigs, mi, entry, inline0, false)
    } else {
        let inline: HashSet<u32> = inline0.iter().copied().collect();
        let mut b = build_func(ctx, mi, entry, &inline);
        let xi = x87::resolve(&mut b.func, &|t| st0_known(sigs, t));
        (b, AbsRes::default(), xi)
    };
    let f = &built.func;
    let mut fa = Facts { module: mi, n_x86: f.n_x86, n_blocks: f.blocks.len() as u32, ..Default::default() };
    fa.inline = built.inlined.clone();
    fa.n_unsupported = built.unsupported.len() as u32;
    fa.n_invalid = built.invalid.len() as u32;
    fa.x87_anoms = xi.anomalies.len() as u32;
    fa.x87_anom_list = xi.anomalies.clone();
    fa.returns_st0 = xi.returns_st0;
    fa.stack_ok = abs.stack_ok || !with_abs;
    fa.bad_tails = abs.bad_tail.len() as u32;
    for b in &f.blocks {
        for i in &b.insts {
            fa.cats |= op_cats(&i.op);
        }
        match &b.term {
            Term::Switch { .. } => fa.cats |= CAT_SWITCH,
            Term::TailCall(_) => fa.cats |= CAT_TAIL,
            _ => {}
        }
        match &b.term {
            Term::Ret { pop } => {
                fa.has_ret = true;
                fa.pop_imms.insert(*pop);
            }
            Term::TailCall(t) => fa.tails.push(t.clone()),
            Term::Trap { .. } => fa.n_traps += 1,
            _ => {}
        }
        for i in &b.insts {
            if let Op::Call { target, .. } = &i.op {
                match target {
                    CallTarget::Direct(a) => fa.calls.push(*a),
                    CallTarget::Indirect(_) => fa.n_indirect_calls += 1,
                    _ => {}
                }
            }
        }
    }
    if with_abs {
        fa.input_use = abs.input_use;
        fa.reads_ret = abs.reads_ret_slot || abs.writes_ret_slot;
        for r in &abs.rets {
            if !r.is_tail && r.esp != Some(0) {
                fa.pop_dynamic = true;
            }
            fa.mod_out |= r.modified;
            fa.unknown_mask |= r.unknown_mask;
        }
        // ecx demanded by callers: liveness after each call
        let ecx_use = |t: &CallTarget| match t {
            CallTarget::Direct(a) => sigs.get(a).map_or(true, |s| s.ecx_in),
            CallTarget::Hle(_) => false,
            CallTarget::Indirect(_) => true,
        };
        let (live, entry_live) = liveness::live_after_calls(f, &ecx_use);
        fa.ecx_in = entry_live & (1 << Gpr::Ecx as u8) != 0;
        for ((bi, ii), l) in live {
            if l & (1 << Gpr::Ecx as u8) != 0 {
                if let Op::Call { target, .. } = &f.blocks[bi as usize].insts[ii as usize].op {
                    match target {
                        CallTarget::Direct(a) => fa.ecx_demand.push(*a),
                        CallTarget::Indirect(_) => fa.ecx_demand_indirect += 1,
                        _ => {}
                    }
                }
            }
        }
    }
    fa
}

pub struct Analysis {
    pub facts: HashMap<u32, Facts>,
    pub sigs: HashMap<u32, Sig>,
    pub inline: HashMap<u32, Vec<u32>>,
    pub auto_noreturn: Vec<u32>,
    pub rounds: u32,
}

/// pop / ext propagation over the call graph
fn derive_sigs(ctx: &LiftCtx, facts: &HashMap<u32, Facts>) -> HashMap<u32, Sig> {
    #[derive(Clone, Copy, PartialEq, Eq)]
    enum P {
        Unset,
        Known(u16),
        Conflict,
    }
    let mut pop: HashMap<u32, P> = HashMap::new();
    let mut ext_out: HashMap<u32, u8> = HashMap::new();
    let mut ecx_demand: HashSet<u32> = HashSet::new();
    for (a, f) in facts {
        let p = if f.pop_dynamic {
            P::Conflict
        } else if f.pop_imms.len() == 1 {
            P::Known(*f.pop_imms.iter().next().unwrap())
        } else if f.pop_imms.is_empty() {
            P::Unset
        } else {
            P::Conflict
        };
        pop.insert(*a, p);
        ext_out.insert(*a, f.mod_out);
        for &c in &f.ecx_demand {
            ecx_demand.insert(c);
        }
    }
    // overrides keep their configured pop; they are not in facts when skipped
    // fixpoint over tail edges
    let mut changed = true;
    let mut it = 0;
    while changed && it < 64 {
        changed = false;
        it += 1;
        for (a, f) in facts {
            let mut p = pop[a];
            let mut eo = ext_out[a];
            for t in &f.tails {
                match t {
                    CallTarget::Direct(c) => {
                        let (cp, ceo) = if let Some(o) = ctx.overrides.get(c) {
                            (match o.pop {
                                Some(n) => P::Known(n),
                                None => P::Conflict,
                            }, 0)
                        } else {
                            (pop.get(c).copied().unwrap_or(P::Unset), ext_out.get(c).copied().unwrap_or(0))
                        };
                        eo |= ceo & 15;
                        p = match (p, cp) {
                            (P::Unset, x) => x,
                            (x, P::Unset) => x,
                            (P::Known(a1), P::Known(b1)) if a1 == b1 => P::Known(a1),
                            _ => P::Conflict,
                        };
                        // demand propagates down tail edges
                        if ecx_demand.contains(a) && !ecx_demand.contains(c) {
                            ecx_demand.insert(*c);
                            changed = true;
                        }
                    }
                    _ => {
                        p = P::Conflict;
                    }
                }
            }
            if p != pop[a] {
                pop.insert(*a, p);
                changed = true;
            }
            if eo != ext_out[a] {
                ext_out.insert(*a, eo);
                changed = true;
            }
        }
    }
    let mut sigs = HashMap::new();
    for (a, f) in facts {
        let mut s = Sig::default();
        s.pop = match pop[a] {
            P::Known(n) => Some(n),
            P::Unset => Some(0),
            P::Conflict => None,
        };
        // functions that never return: pop is irrelevant
        s.ext_out = ext_out[a] | if ecx_demand.contains(a) { 16 } else { 0 };
        // a function that returns a register must also receive it, so that paths which do not modify it return the caller's value
        s.ext_in = f.input_use | (s.ext_out & 15);
        s.ret_addr = f.reads_ret;
        s.st0_ret = f.returns_st0;
        s.noreturn = !f.has_ret && f.tails.is_empty();
        s.ecx_in = f.ecx_in;
        sigs.insert(*a, s);
    }
    sigs
}

pub fn analyze_program(ctx: &mut LiftCtx, log: &mut dyn FnMut(String)) -> Analysis {
    let all: Vec<(u32, usize)> = ctx.all_funcs().into_iter().collect();
    let t0 = std::time::Instant::now();
    // ---- round 0: structure only (no stack analysis): rets, tails, calls; derive auto-noreturn
    let empty: HashMap<u32, Sig> = HashMap::new();
    let mut inline: HashMap<u32, Vec<u32>> = HashMap::new();
    let mut facts0: HashMap<u32, Facts> = {
        let c: &LiftCtx = ctx;
        all.par_iter().filter(|(a, _)| !skip_body(c, *a)).map(|&(a, mi)| (a, compute_facts(c, &empty, mi, a, &[], false))).collect()
    };
    log(format!("round 0: {} functions in {:.1}s", facts0.len(), t0.elapsed().as_secs_f32()));
    // auto noreturn: no ret, only tails to noreturn functions, clean decode
    let mut nr: HashSet<u32> = ctx.noreturn_funcs.clone();
    let mut auto: Vec<u32> = Vec::new();
    loop {
        let mut added = false;
        for (a, f) in &facts0 {
            if nr.contains(a) || f.has_ret || f.n_invalid > 0 || f.n_unsupported > 0 || f.n_traps > 0 || f.n_indirect_calls > 0 && false {
                continue;
            }
            let tails_ok = f.tails.iter().all(|t| match t {
                CallTarget::Direct(c) => nr.contains(c),
                CallTarget::Hle(i) => ctx.hle_noreturn(*i),
                _ => false,
            });
            if tails_ok {
                // must actually end: it needs at least one noreturn call, tail, or be a pure loop; require a call/tail evidence
                if !f.tails.is_empty() || f.calls.iter().any(|c| nr.contains(c)) {
                    nr.insert(*a);
                    auto.push(*a);
                    added = true;
                }
            }
        }
        if !added {
            break;
        }
    }
    auto.sort_unstable();
    log(format!("auto-detected noreturn functions: {}", auto.len()));
    if !auto.is_empty() {
        ctx.noreturn_funcs = nr;
        let c: &LiftCtx = ctx;
        facts0 = all.par_iter().filter(|(a, _)| !skip_body(c, *a)).map(|&(a, mi)| (a, compute_facts(c, &empty, mi, a, &[], false))).collect();
    }
    let mut sigs = derive_sigs(ctx, &facts0);
    // ---- pre-round: find tiny straight-line helpers that manipulate the stack (SEH4/EH prolog+epilog style) and inline them
    {
        let c: &LiftCtx = ctx;
        let pre: Vec<(u32, Facts)> = all.par_iter().filter(|(a, _)| !skip_body(c, *a)).map(|&(a, mi)| (a, compute_facts(c, &sigs, mi, a, &[], true))).collect();
        let mut cands: Vec<u32> = Vec::new();
        for (a, f) in &pre {
            if f.n_blocks == 1 && f.n_x86 <= 48 && f.has_ret && f.calls.is_empty() && f.tails.is_empty() && f.pop_imms.len() == 1 && f.n_unsupported == 0 && f.n_invalid == 0 && (f.cats & !CAT_FS) == 0 && (f.pop_dynamic || f.reads_ret || f.mod_out != 0 || f.input_use != 0) {
                cands.push(*a);
            }
        }
        cands.sort_unstable();
        let mut helpers = HashMap::new();
        for a in &cands {
            if let Some(h) = crate::func::decode_straight_line(ctx, *a) {
                helpers.insert(*a, h);
            }
        }
        log(format!("inlined stack helpers: {} candidates, {} accepted", cands.len(), helpers.len()));
        ctx.inline_helpers = helpers;
        let c: &LiftCtx = ctx;
        facts0 = all.par_iter().filter(|(a, _)| !skip_body(c, *a)).map(|&(a, mi)| (a, compute_facts(c, &empty, mi, a, &[], false))).collect();
        sigs = derive_sigs(ctx, &facts0);
    }
    // ---- rounds with stack analysis
    let mut facts: HashMap<u32, Facts> = HashMap::new();
    let mut todo: Vec<(u32, usize)> = all.iter().copied().filter(|(a, _)| !skip_body(ctx, *a)).collect();
    let mut rounds = 0;
    // reverse call graph is built lazily from facts
    loop {
        rounds += 1;
        let t = std::time::Instant::now();
        let c: &LiftCtx = ctx;
        let sg = &sigs;
        let inl = &inline;
        let res: Vec<(u32, Facts)> = todo
            .par_iter()
            .map(|&(a, mi)| {
                let il = inl.get(&a).cloned().unwrap_or_default();
                (a, compute_facts(c, sg, mi, a, &il, true))
            })
            .collect();
        let n_todo = todo.len();
        for (a, f) in res {
            if !f.inline.is_empty() {
                inline.insert(a, f.inline.clone());
            }
            facts.insert(a, f);
        }
        let new_sigs = derive_sigs(ctx, &facts);
        // functions whose callees' signatures changed need recomputation
        let mut changed: HashSet<u32> = HashSet::new();
        for (a, s) in &new_sigs {
            let old = sigs.get(a);
            let diff = match old {
                None => true,
                Some(o) => o.pop != s.pop || o.ext_in != s.ext_in || (o.ext_out & 15) != (s.ext_out & 15) || o.ecx_in != s.ecx_in,
            };
            if diff {
                changed.insert(*a);
            }
        }
        log(format!("round {rounds}: analysed {n_todo} in {:.1}s, {} signatures changed", t.elapsed().as_secs_f32(), changed.len()));
        sigs = new_sigs;
        if changed.is_empty() || rounds >= 10 {
            break;
        }
        todo = all
            .iter()
            .copied()
            .filter(|(a, _)| {
                if skip_body(ctx, *a) {
                    return false;
                }
                let f = &facts[a];
                f.calls.iter().any(|c| changed.contains(c)) || f.tails.iter().any(|t| matches!(t, CallTarget::Direct(c) if changed.contains(c)))
            })
            .collect();
        if todo.is_empty() {
            break;
        }
    }
    Analysis { facts, sigs, inline, auto_noreturn: auto, rounds }
}

pub struct FnOut {
    pub addr: u32,
    pub module: usize,
    pub n_x86: u32,
    pub text: String,
    pub refs: BTreeSet<(u32, bool)>,
    pub ovr_refs: BTreeSet<String>,
    pub x87_refs: BTreeSet<String>,
    pub traps: Vec<(u32, String)>,
    pub unsupported: Vec<(u32, String)>,
    pub n_local: u32,
    pub n_shared: u32,
}

pub fn emit_one(ctx: &LiftCtx, an: &Analysis, mi: usize, entry: u32, trace: bool) -> FnOut {
    let il = an.inline.get(&entry).cloned().unwrap_or_default();
    let (built, abs, _xi) = {
        let mut r = analyze_function(ctx, &an.sigs, mi, entry, &il, true);
        let b = &mut r.0;
        // memory classes were annotated by analyze (annotate=true)
        let _ = b;
        r
    };
    let plan = flags::plan(&built.func);
    let tab = SigTab { ctx, sigs: &an.sigs };
    let me = an.sigs.get(&entry).cloned().unwrap_or_default();
    let mut em = Emitter::new(&built.func, &plan, &abs, me, &tab);
    em.trace = trace;
    let text = em.emit_function();
    let mut traps = Vec::new();
    for (a, _) in &em.n_traps {
        traps.push((*a, "trap".to_string()));
    }
    // block-level traps with reasons
    for b in &built.func.blocks {
        if let Term::Trap { why } = b.term {
            traps.push((b.term_addr, format!("trap{}", why)));
        }
    }
    FnOut {
        addr: entry,
        module: mi,
        n_x86: built.func.n_x86,
        text,
        refs: em.refs.clone(),
        ovr_refs: em.ovr_refs.clone(),
        x87_refs: em.x87_host_refs.clone(),
        traps,
        unsupported: built.unsupported.clone(),
        n_local: abs.n_local,
        n_shared: abs.n_shared,
    }
}
