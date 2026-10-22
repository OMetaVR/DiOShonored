//! Generic visitors over IR operations: places read and written. Every analysis builds on these.

use crate::repr::*;

fn opnd(o: &Opnd, f: &mut dyn FnMut(Place)) {
    if let Opnd::P(p) = o {
        f(*p);
    }
}
fn mem(m: &Mem, f: &mut dyn FnMut(Place)) {
    if let Some(b) = m.base {
        f(b);
    }
    if let Some(x) = m.index {
        f(x);
    }
}

impl CallTarget {
    pub fn visit_uses(&self, f: &mut dyn FnMut(Place)) {
        if let CallTarget::Indirect(o) = self {
            opnd(o, f);
        }
    }
}

impl Op {
    /// places (including implicit register operands) read by this op
    pub fn visit_uses(&self, f: &mut dyn FnMut(Place)) {
        use Op::*;
        let g = |r: Gpr| Place::Gpr(r);
        match self {
            Mov { a, .. } | Un { a, .. } | Cast { a, .. } => opnd(a, f),
            Bin { op, ty, a, b, .. } => {
                // value-independent idioms do not read their operands
                let mask = ty.mask();
                let indep = (matches!(op, BinOp::Xor | BinOp::Sub) && a == b) || (matches!(op, BinOp::And) && matches!(b, Opnd::Imm(0))) || (matches!(op, BinOp::Or) && matches!(b, Opnd::Imm(m) if *m == mask));
                if !indep {
                    opnd(a, f);
                    opnd(b, f)
                }
            }
            Cmp { a, b, .. } => {
                opnd(a, f);
                opnd(b, f)
            }
            Sel { c, a, b, .. } => {
                opnd(c, f);
                opnd(a, f);
                opnd(b, f)
            }
            Lea { m, .. } => mem(m, f),
            Ld { m, .. } => mem(m, f),
            St { m, v, .. } => {
                mem(m, f);
                opnd(v, f)
            }
            SetPart { r, v, .. } => {
                f(g(*r));
                opnd(v, f)
            }
            MulWide { a, b, .. } => {
                opnd(a, f);
                opnd(b, f)
            }
            DivWide { hi, lo, div, .. } => {
                opnd(hi, f);
                opnd(lo, f);
                opnd(div, f)
            }
            AtomicRmw { m, v, .. } => {
                mem(m, f);
                opnd(v, f)
            }
            AtomicCas { m, expected, new, .. } => {
                mem(m, f);
                opnd(expected, f);
                opnd(new, f)
            }
            Fence => {}
            FlagDef(d) => {
                opnd(&d.a, f);
                opnd(&d.b, f);
                opnd(&d.r, f);
                opnd(&d.cin, f);
                opnd(&d.cnt, f)
            }
            FlagGet { .. } | FlagGetBit { .. } | FlagsPack { .. } | GetDf { .. } => {}
            SetDf(o) => opnd(o, f),
            Call { target, .. } => {
                target.visit_uses(f);
                f(g(Gpr::Esp));
                f(g(Gpr::Eax));
                f(g(Gpr::Ecx));
                f(g(Gpr::Edx));
            }
            FRetLoad { .. } => {}
            ResumeSetup { .. } => f(g(Gpr::Ebp)),
            FHost { a, b, .. } => {
                opnd(a, f);
                opnd(b, f)
            }
            FRetStore { a } => opnd(a, f),
            Str { kind, rep, .. } => {
                let (rs, rd, ra) = str_regs(*kind);
                if rs {
                    f(g(Gpr::Esi));
                }
                if rd {
                    f(g(Gpr::Edi));
                }
                if ra {
                    f(g(Gpr::Eax));
                }
                if *rep != Rep::None {
                    f(g(Gpr::Ecx));
                }
            }
            FPush { v } => opnd(v, f),
            FPop => {}
            FXch { .. } => {}
            FMov { a, .. } => opnd(a, f),
            FBin { a, b, .. } => {
                opnd(a, f);
                opnd(b, f)
            }
            FUn { a, .. } => opnd(a, f),
            FLdM { m, .. } => mem(m, f),
            FStM { m, a, .. } => {
                mem(m, f);
                opnd(a, f)
            }
            FConst { .. } => {}
            FCom { a, b } | FComi { a, b, .. } => {
                opnd(a, f);
                opnd(b, f)
            }
            FTst { a } | FXam { a } => opnd(a, f),
            FNstsw { .. } | FNstcw { .. } => {}
            FLdcw { v } => opnd(v, f),
            FCmov { c, a, d } => {
                opnd(c, f);
                opnd(a, f);
                f(*d)
            }
            FPrem { d, b, .. } | FScale { d, b } => {
                f(*d);
                opnd(b, f)
            }
            FYl2x { a, b, .. } => {
                opnd(a, f);
                opnd(b, f)
            }
            FAtan2 { y, x, .. } => {
                opnd(y, f);
                opnd(x, f)
            }
            FInit => {}
            V { a, b, d, f: vf, .. } => {
                opnd(a, f);
                opnd(b, f);
                if vf.0.starts_with("ins_") || vf.0.starts_with("merge_") {
                    f(*d);
                }
            }
            VLd { m, d, zx, .. } => {
                mem(m, f);
                if !*zx {
                    f(*d)
                }
            }
            VSt { m, a, .. } => {
                mem(m, f);
                opnd(a, f)
            }
            Misc(m) => match m {
                MiscOp::Cpuid => {
                    f(g(Gpr::Eax));
                    f(g(Gpr::Ecx))
                }
                MiscOp::Prefetch(m) => mem(m, f),
                MiscOp::Ldmxcsr { v } => opnd(v, f),
                _ => {}
            },
            Trap { .. } => {}
        }
    }

    /// places written (fully or partially) by this op
    pub fn visit_defs(&self, f: &mut dyn FnMut(Place)) {
        use Op::*;
        let g = |r: Gpr| Place::Gpr(r);
        match self {
            Mov { d, .. } | Bin { d, .. } | Un { d, .. } | Cast { d, .. } | Sel { d, .. } | Cmp { d, .. } | Lea { d, .. } | Ld { d, .. } => f(*d),
            St { .. } | Fence => {}
            SetPart { r, .. } => f(g(*r)),
            MulWide { hi, lo, .. } => {
                f(*hi);
                f(*lo)
            }
            DivWide { q, r, .. } => {
                f(*q);
                f(*r)
            }
            AtomicRmw { old, .. } | AtomicCas { old, .. } => f(*old),
            FlagDef(_) => {}
            FlagGet { d, .. } | FlagGetBit { d, .. } | FlagsPack { d } | GetDf { d } => f(*d),
            SetDf(_) => {}
            Call { .. } => {
                f(g(Gpr::Esp));
                f(g(Gpr::Eax));
                f(g(Gpr::Ecx));
                f(g(Gpr::Edx));
            }
            FRetLoad { d } => f(*d),
            FHost { d, .. } => f(*d),
            ResumeSetup { d, .. } => f(*d),
            FRetStore { .. } => {}
            Str { kind, rep, ta, tb, done, .. } => {
                let (rs, rd, _) = str_regs(*kind);
                if rs {
                    f(g(Gpr::Esi));
                }
                if rd {
                    f(g(Gpr::Edi));
                }
                if *kind == StrKind::Lods {
                    f(g(Gpr::Eax));
                }
                if *rep != Rep::None {
                    f(g(Gpr::Ecx));
                }
                if matches!(kind, StrKind::Cmps | StrKind::Scas) {
                    f(*ta);
                    f(*tb);
                    f(*done);
                }
            }
            FPush { .. } | FPop | FXch { .. } => {}
            FMov { d, .. } | FBin { d, .. } | FUn { d, .. } | FLdM { d, .. } | FConst { d, .. } | FCmov { d, .. } | FPrem { d, .. } | FScale { d, .. } | FYl2x { d, .. } | FAtan2 { d, .. } => f(*d),
            FComi { d, .. } => f(*d),
            FStM { .. } | FCom { .. } | FTst { .. } | FXam { .. } | FLdcw { .. } | FInit => {}
            FNstsw { d, .. } | FNstcw { d } => f(*d),
            V { d, .. } | VLd { d, .. } => f(*d),
            VSt { .. } => {}
            Misc(m) => match m {
                MiscOp::Cpuid => {
                    f(g(Gpr::Eax));
                    f(g(Gpr::Ebx));
                    f(g(Gpr::Ecx));
                    f(g(Gpr::Edx))
                }
                MiscOp::Rdtsc => {
                    f(g(Gpr::Eax));
                    f(g(Gpr::Edx))
                }
                MiscOp::Stmxcsr { d } => f(*d),
                _ => {}
            },
            Trap { .. } => {}
        }
    }

    pub fn is_x87(&self) -> bool {
        use Op::*;
        matches!(
            self,
            FPush { .. } | FPop | FXch { .. } | FMov { .. } | FBin { .. } | FUn { .. } | FLdM { .. } | FStM { .. } | FConst { .. } | FCom { .. } | FComi { .. } | FTst { .. } | FXam { .. } | FNstsw { .. } | FNstcw { .. } | FLdcw { .. } | FCmov { .. } | FPrem { .. } | FScale { .. } | FYl2x { .. } | FAtan2 { .. } | FInit | FRetLoad { .. } | FRetStore { .. } | FHost { .. }
        )
    }
}

impl Term {
    pub fn visit_uses(&self, f: &mut dyn FnMut(Place)) {
        match self {
            Term::Br { c, .. } => opnd(c, f),
            Term::Switch { idx, .. } => opnd(idx, f),
            Term::TailCall(t) => {
                t.visit_uses(f);
                f(Place::Gpr(Gpr::Esp));
                f(Place::Gpr(Gpr::Eax));
                f(Place::Gpr(Gpr::Ecx));
                f(Place::Gpr(Gpr::Edx));
            }
            Term::Ret { .. } => {
                f(Place::Gpr(Gpr::Esp));
                f(Place::Gpr(Gpr::Eax));
                f(Place::Gpr(Gpr::Edx));
            }
            _ => {}
        }
    }
    pub fn succs(&self) -> Vec<u32> {
        match self {
            Term::Fall(t) | Term::Jmp(t) => vec![*t],
            Term::Br { t, f, .. } => vec![*t, *f],
            Term::Switch { targets, .. } => {
                let mut v = targets.clone();
                v.sort_unstable();
                v.dedup();
                v
            }
            _ => vec![],
        }
    }
}

/// (uses esi, uses edi, uses eax) of a string instruction
pub fn str_regs(k: StrKind) -> (bool, bool, bool) {
    match k {
        StrKind::Movs | StrKind::Cmps => (true, true, false),
        StrKind::Stos | StrKind::Scas => (false, true, true),
        StrKind::Lods => (true, false, false),
    }
}
