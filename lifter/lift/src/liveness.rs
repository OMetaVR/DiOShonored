//! Backward liveness of the eight GPRs (used to find which direct calls need `ecx` after return).

use ir::*;

pub fn gpr_mask(p: &Place) -> u8 {
    match p {
        Place::Gpr(g) => 1 << (*g as u8),
        _ => 0,
    }
}

/// Returns, for every `Op::Call` (blk, idx), the set of GPRs live immediately after the call, treating `ecx` as
/// preserved by the call (so a read of `ecx` after the call keeps it live before the call).
pub fn live_after_calls(func: &Func, ecx_use: &dyn Fn(&CallTarget) -> bool) -> (Vec<((u32, u32), u8)>, u8) {
    let nb = func.blocks.len();
    let mut live_in = vec![0u8; nb];
    let mut changed = true;
    let succs: Vec<Vec<u32>> = func.blocks.iter().map(|b| b.term.succs()).collect();
    let term_uses = |t: &Term| -> u8 {
        let mut m = 0u8;
        t.visit_uses(&mut |p| m |= gpr_mask(&p));
        match t {
            Term::Ret { .. } => m & !(1 << Gpr::Ecx as u8),
            _ => m,
        }
    };
    while changed {
        changed = false;
        for b in (0..nb).rev() {
            let mut live = 0u8;
            for &s in &succs[b] {
                live |= live_in[s as usize];
            }
            live |= term_uses(&func.blocks[b].term);
            for ins in func.blocks[b].insts.iter().rev() {
                let mut defs = 0u8;
                let mut uses = 0u8;
                ins.op.visit_defs(&mut |p| defs |= gpr_mask(&p));
                ins.op.visit_uses(&mut |p| uses |= gpr_mask(&p));
                if let Op::Call { target, .. } = &ins.op {
                    defs &= !(1 << Gpr::Ecx as u8);
                    if !ecx_use(target) {
                        uses &= !(1 << Gpr::Ecx as u8);
                    }
                }
                live = (live & !defs) | uses;
            }
            if live != live_in[b] {
                live_in[b] = live;
                changed = true;
            }
        }
    }
    let mut out = Vec::new();
    for b in 0..nb {
        let mut live = 0u8;
        for &s in &succs[b] {
            live |= live_in[s as usize];
        }
        live |= term_uses(&func.blocks[b].term);
        for (ii, ins) in func.blocks[b].insts.iter().enumerate().rev() {
            if matches!(ins.op, Op::Call { .. }) {
                out.push(((b as u32, ii as u32), live));
            }
            let mut defs = 0u8;
            let mut uses = 0u8;
            ins.op.visit_defs(&mut |p| defs |= gpr_mask(&p));
            ins.op.visit_uses(&mut |p| uses |= gpr_mask(&p));
            if let Op::Call { target, .. } = &ins.op {
                defs &= !(1 << Gpr::Ecx as u8);
                if !ecx_use(target) {
                    uses &= !(1 << Gpr::Ecx as u8);
                }
            }
            live = (live & !defs) | uses;
        }
    }
    (out, live_in[0])
}
