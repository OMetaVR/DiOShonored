//! Static x87 stack-depth resolution: rewrites relative `St(i)` / `FPush` / `FPop` / `FXch` into absolute slots
//! `Fp(k)` (C locals `double f0..f7`), inserts the `st0_ret` handoff at calls and returns, and reports anomalies.

use ir::*;

#[derive(Default, Debug, Clone)]
pub struct X87Info {
    pub used: bool,
    pub max_depth: u8,
    /// some `ret` leaves a value in st(0)
    pub returns_st0: bool,
    pub anomalies: Vec<(u32, &'static str)>,
    /// count of calls whose return value is consumed from st0_ret
    pub st0_consumers: u32,
}

fn is_x87_op(op: &Op) -> bool {
    op.is_x87()
}

/// minimum stack depth needed by the first ST references of an op, and net effect
fn fp_need(op: &Op) -> (i32, i32) {
    let mut need = 0i32;
    let mut chk = |p: &Place| {
        if let Place::St(i) = p {
            need = need.max(*i as i32 + 1);
        }
    };
    let o = |o: &Opnd, chk: &mut dyn FnMut(&Place)| {
        if let Opnd::P(p) = o {
            chk(p)
        }
    };
    let mut delta = 0;
    match op {
        Op::FPush { v } => {
            o(v, &mut chk);
            delta = 1;
        }
        Op::FPop => {
            need = 1;
            delta = -1;
        }
        Op::FXch { i } => need = *i as i32 + 1,
        Op::FMov { d, a } => {
            chk(d);
            o(a, &mut chk)
        }
        Op::FBin { d, a, b, .. } => {
            chk(d);
            o(a, &mut chk);
            o(b, &mut chk)
        }
        Op::FUn { d, a, .. } => {
            chk(d);
            o(a, &mut chk)
        }
        Op::FStM { a, .. } | Op::FTst { a } | Op::FXam { a } => o(a, &mut chk),
        Op::FCom { a, b } | Op::FComi { a, b, .. } => {
            o(a, &mut chk);
            o(b, &mut chk)
        }
        Op::FCmov { d, a, .. } => {
            chk(d);
            o(a, &mut chk)
        }
        Op::FPrem { d, b, .. } | Op::FScale { d, b } => {
            chk(d);
            o(b, &mut chk)
        }
        Op::FYl2x { d, a, b } => {
            chk(d);
            o(a, &mut chk);
            o(b, &mut chk)
        }
        Op::FAtan2 { d, y, x } => {
            chk(d);
            o(y, &mut chk);
            o(x, &mut chk)
        }
        Op::FHost { d, a, b, .. } => {
            chk(d);
            o(a, &mut chk);
            o(b, &mut chk)
        }
        _ => {}
    }
    (need, delta)
}

fn map_places(op: &mut Op, f: &mut dyn FnMut(&mut Place) -> bool) -> bool {
    let mut ok = true;
    let mut p = |pl: &mut Place| {
        if !f(pl) {
            ok = false
        }
    };
    fn o(x: &mut Opnd, p: &mut dyn FnMut(&mut Place)) {
        if let Opnd::P(pl) = x {
            p(pl)
        }
    }
    match op {
        Op::FPush { v } => o(v, &mut p),
        Op::FMov { d, a } => {
            p(d);
            o(a, &mut p)
        }
        Op::FBin { d, a, b, .. } => {
            p(d);
            o(a, &mut p);
            o(b, &mut p)
        }
        Op::FUn { d, a, .. } => {
            p(d);
            o(a, &mut p)
        }
        Op::FStM { a, .. } | Op::FTst { a } | Op::FXam { a } => o(a, &mut p),
        Op::FCom { a, b } | Op::FComi { a, b, .. } => {
            o(a, &mut p);
            o(b, &mut p)
        }
        Op::FCmov { d, a, .. } => {
            p(d);
            o(a, &mut p)
        }
        Op::FPrem { d, b, .. } | Op::FScale { d, b } => {
            p(d);
            o(b, &mut p)
        }
        Op::FYl2x { d, a, b } => {
            p(d);
            o(a, &mut p);
            o(b, &mut p)
        }
        Op::FAtan2 { d, y, x } => {
            p(d);
            o(y, &mut p);
            o(x, &mut p)
        }
        Op::FHost { d, a, b, .. } => {
            p(d);
            o(a, &mut p);
            o(b, &mut p)
        }
        _ => {}
    }
    ok
}

/// Does the first x87 operation reachable after (blk, idx) need a value that is not on the stack yet? `depth0` is the stack depth
/// the caller holds across the call: LTCG code keeps x87 values live across calls to functions it knows leave the stack alone
/// (GFx's SWF matrix reader keeps 1/65536 in st(0) across its bit-reader calls), so only a need beyond that depth is a returned value.
fn consumes_return(func: &Func, blk: usize, idx: usize, depth0: i32) -> bool {
    // iterative DFS over (block, start index, depth)
    let mut stack = vec![(blk, idx, depth0)];
    let mut seen = std::collections::HashSet::new();
    let mut budget = 400;
    while let Some((b, start, mut depth)) = stack.pop() {
        if budget == 0 {
            return false;
        }
        budget -= 1;
        if start == 0 && !seen.insert(b) && b != blk {
            continue;
        }
        let insts = &func.blocks[b].insts;
        let mut i = start;
        let mut terminated = false;
        while i < insts.len() {
            let op = &insts[i].op;
            if matches!(op, Op::Call { .. }) {
                terminated = true;
                break;
            }
            if is_x87_op(op) {
                let (need, delta) = fp_need(op);
                if need > depth {
                    return true;
                }
                depth += delta;
            }
            i += 1;
        }
        if terminated {
            continue;
        }
        match &func.blocks[b].term {
            Term::Ret { .. } | Term::TailCall(_) | Term::Unreachable | Term::Trap { .. } => {}
            t => {
                for s in t.succs() {
                    stack.push((s as usize, 0, depth));
                }
            }
        }
    }
    false
}

type CallSet = std::collections::HashSet<(usize, usize)>;

/// Stack depth at every call site, by forward propagation over the unrewritten IR, given which calls push a returned st0.
fn call_depths(func: &Func, consumed: &CallSet) -> std::collections::HashMap<(usize, usize), i32> {
    let nb = func.blocks.len();
    let mut depth_in: Vec<Option<i32>> = vec![None; nb];
    depth_in[0] = Some(0);
    let mut work = vec![0usize];
    let mut done = vec![false; nb];
    let mut out = std::collections::HashMap::new();
    while let Some(b) = work.pop() {
        if done[b] {
            continue;
        }
        done[b] = true;
        let mut d = depth_in[b].unwrap();
        let mut bad = false;
        for (ii, ins) in func.blocks[b].insts.iter().enumerate() {
            if let Op::Call { .. } = ins.op {
                out.insert((b, ii), d);
                if consumed.contains(&(b, ii)) {
                    d += 1;
                }
            } else if is_x87_op(&ins.op) {
                let (need, delta) = fp_need(&ins.op);
                if need > d || d + delta > 8 {
                    bad = true;
                    break;
                }
                d += delta;
            }
        }
        if bad {
            continue;
        }
        match &func.blocks[b].term {
            Term::Ret { .. } | Term::TailCall(_) => {}
            t => {
                for s in t.succs() {
                    let s = s as usize;
                    if depth_in[s].is_none() {
                        depth_in[s] = Some(d);
                        work.push(s);
                    }
                }
            }
        }
    }
    out
}

/// Which calls push a returned st0 value. The decision depends on the depth held across each call, which in turn depends on the
/// decisions for earlier calls, so iterate from "empty stack at every call" to a fixpoint.
/// `known` tells whether a direct call's target is known to return a value in st(0) (its signature); calls not known to are decided
/// by what the code after them needs beyond the depth held across the call.
fn consumed_calls(func: &Func, known: &dyn Fn(&CallTarget) -> Option<bool>) -> CallSet {
    let calls: Vec<(usize, usize)> = (0..func.blocks.len())
        .flat_map(|b| (0..func.blocks[b].insts.len()).map(move |i| (b, i)))
        .filter(|&(b, i)| matches!(func.blocks[b].insts[i].op, Op::Call { .. }))
        .collect();
    let decide = |b: usize, i: usize, depth: i32| -> bool {
        if let Op::Call { target, .. } = &func.blocks[b].insts[i].op {
            // only a positive answer is trusted: a callee can return st(0) through paths its own signature misses (a tail call into a
            // float function, an overridden CRT helper), so "no" falls back to looking at what the caller needs
            if known(target) == Some(true) {
                return true;
            }
        }
        consumes_return(func, b, i + 1, depth)
    };
    let mut consumed: CallSet = calls.iter().copied().filter(|&(b, i)| decide(b, i, 0)).collect();
    for _ in 0..4 {
        let depths = call_depths(func, &consumed);
        let next: CallSet = calls
            .iter()
            .copied()
            .filter(|&(b, i)| decide(b, i, depths.get(&(b, i)).copied().unwrap_or(0)))
            .collect();
        if next == consumed {
            break;
        }
        consumed = next;
    }
    consumed
}

pub fn resolve(func: &mut Func, known: &dyn Fn(&CallTarget) -> Option<bool>) -> X87Info {
    let mut info = X87Info::default();
    let any = func.blocks.iter().any(|b| b.insts.iter().any(|i| is_x87_op(&i.op)) );
    if !any {
        return info;
    }
    info.used = true;
    // which calls consume a returned st0 value
    let consumed = consumed_calls(func, known);
    info.st0_consumers = consumed.len() as u32;
    let nb = func.blocks.len();
    let mut depth_in: Vec<Option<i32>> = vec![None; nb];
    depth_in[0] = Some(0);
    let mut work = vec![0usize];
    let mut done = vec![false; nb];
    let mut max_depth = 0i32;
    while let Some(b) = work.pop() {
        if done[b] {
            continue;
        }
        done[b] = true;
        let mut d = depth_in[b].unwrap();
        let old = std::mem::take(&mut func.blocks[b].insts);
        let mut out: Vec<Inst> = Vec::with_capacity(old.len() + 2);
        let mut bad = false;
        for (ii, mut ins) in old.into_iter().enumerate() {
            let addr = ins.addr;
            if let Op::Call { .. } = ins.op {
                if d != 0 {
                    info.anomalies.push((addr, "x87 stack not empty across call"));
                }
                out.push(ins);
                if consumed.contains(&(b, ii)) {
                    out.push(Inst { addr, op: Op::FRetLoad { d: Place::Fp(d as u8) } });
                    d += 1;
                    max_depth = max_depth.max(d);
                }
                continue;
            }
            if !is_x87_op(&ins.op) {
                out.push(ins);
                continue;
            }
            let (need, _delta) = fp_need(&ins.op);
            if need > d {
                info.anomalies.push((addr, "x87 stack underflow"));
                bad = true;
                break;
            }
            match &mut ins.op {
                Op::FPush { v } => {
                    let mut v2 = *v;
                    if let Opnd::P(Place::St(i)) = v2 {
                        v2 = Opnd::P(Place::Fp((d - 1 - i as i32) as u8));
                    }
                    if d >= 8 {
                        info.anomalies.push((addr, "x87 stack overflow"));
                        bad = true;
                        break;
                    }
                    out.push(Inst { addr, op: Op::FMov { d: Place::Fp(d as u8), a: v2 } });
                    d += 1;
                    max_depth = max_depth.max(d);
                }
                Op::FPop => {
                    d -= 1;
                }
                Op::FXch { i } => {
                    let a = (d - 1) as u8;
                    let bsl = (d - 1 - *i as i32) as u8;
                    let t = func.new_tmp(Ty::F64);
                    out.push(Inst { addr, op: Op::FMov { d: Place::Tmp(t), a: Opnd::P(Place::Fp(a)) } });
                    out.push(Inst { addr, op: Op::FMov { d: Place::Fp(a), a: Opnd::P(Place::Fp(bsl)) } });
                    out.push(Inst { addr, op: Op::FMov { d: Place::Fp(bsl), a: Opnd::P(Place::Tmp(t)) } });
                }
                Op::FNstsw { top, .. } => {
                    *top = ((8 - d) & 7) as u8;
                    out.push(ins);
                }
                _ => {
                    let dd = d;
                    let mut ok = true;
                    map_places(&mut ins.op, &mut |p| {
                        if let Place::St(i) = *p {
                            let k = dd - 1 - i as i32;
                            if k < 0 {
                                ok = false;
                                return false;
                            }
                            *p = Place::Fp(k as u8);
                        }
                        true
                    });
                    if !ok {
                        info.anomalies.push((addr, "x87 operand beyond stack"));
                        bad = true;
                        break;
                    }
                    out.push(ins);
                }
            }
        }
        if bad {
            func.blocks[b].insts = Vec::new();
            func.blocks[b].term = Term::Trap { why: TRAP_UNSUPPORTED };
            continue;
        }
        // terminator effects
        match &func.blocks[b].term {
            Term::Ret { .. } => {
                if d >= 1 {
                    info.returns_st0 = true;
                    out.push(Inst { addr: func.blocks[b].term_addr, op: Op::FRetStore { a: Opnd::P(Place::Fp((d - 1) as u8)) } });
                }
            }
            Term::TailCall(_) => {
                if d != 0 {
                    info.anomalies.push((func.blocks[b].term_addr, "x87 stack not empty at tail call"));
                }
            }
            t => {
                for s in t.succs() {
                    let s = s as usize;
                    match depth_in[s] {
                        None => {
                            depth_in[s] = Some(d);
                            work.push(s);
                        }
                        Some(x) if x != d => {
                            info.anomalies.push((func.blocks[s].addr, "x87 depth mismatch at join"));
                        }
                        _ => {}
                    }
                }
            }
        }
        func.blocks[b].insts = out;
    }
    for b in 0..nb {
        if !done[b] {
            func.blocks[b].insts = Vec::new();
            func.blocks[b].term = Term::Trap { why: TRAP_UNSUPPORTED };
        }
    }
    info.max_depth = max_depth as u8;
    info
}
